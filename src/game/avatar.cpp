#include "game/avatar.h"

#include "sim/scale.h"  // MetresToCells

#include "phys/lattice.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "game/rigrender.h"
#include "sim/bytestream.h"
#include "sim/oprecord.h"  // the op stream's author side table (N3)
#include "sim/rng.h"
#include "sim/tuning.h"

namespace {

inline Quat AxisAngle(Vec3 axis, float a) { return QuatAxisAngle(axis, a); }
inline Quat Mul(const Quat& a, const Quat& b) { return QuatMul(a, b); }
inline Vec3 Rotate(const Quat& q, Vec3 v) { return QuatRotate(q, v); }
inline Vec3 RotateInv(const Quat& q, Vec3 v) { return QuatRotateInv(q, v); }

// sim/rng.h. The avatar's spray is presentation, but it is authored INTO the
// tick's spawn stream, which a replay must reproduce (CLAUDE.md rule 1).
using rng::Hash3;
using rng::Pcg;
using rng::SignedUnit;

// (The gait's constants — the lead cap, the stride budget, the stance
// reserve and crouch, the stride-clock lock and the airborne key table —
// moved to game/pose.cpp with the one gait every creature runs, W2-L.)

// ---- WHAT THE DRAG COSTS, AND WHAT A HOP BUYS BACK -------------------------
//
// The fraction of its one-legged walking speed a body keeps while it is
// actually scrubbing a stump along the ground: a third, so dragging is three
// times slower than the same body in the air.
//
// It is charged on `dragW_` and NOT on the loco state, which is the whole
// mechanic. `dragW_` falls to nothing inside one hop's airtime (mob.cpp
// kDragReleaseHalflife) and takes the better part of a second to come back on
// landing, so a player who keeps jumping keeps the speed and a player who walks
// it off does not. Nothing here tests for a jump; the incentive falls out of
// the same weight that drives the pose, so the thing you SEE is exactly the
// thing you are being charged for.
constexpr float kDragSpeedPenalty = 1.0f / 3.0f;

// How long the GAIT keeps its footing after `grounded` drops, and how far the
// body may have left the ground it last stood on while it does.
//
// These are not the clips' `avatar.airDebounce`, and deliberately longer than
// it: the clips only have to avoid firing a one-shot, whereas losing the gait
// re-plants both feet from scratch (Mob::ParkGaitForAir clears footInit_) and fades
// the leg IK out to the rest hang, so every flicker of `grounded` snapped the
// legs to a standing pose mid-stride. On microvoxel terrain that is most steps
// — Player::grounded is a 0.1-voxel positional probe, and a one-voxel step-down
// at walk pace is genuinely airborne for ~0.14 s, longer than the 0.12 s the
// clips debounce with.
//
// The DISTANCE is what keeps this from swallowing a real jump: air time alone
// cannot tell a kerb from a launch, but a jump has left the floor by half a leg
// within about three ticks, and once the body is that far up there is no
// footing to keep.
constexpr float kGaitCoyoteSeconds = 0.30f;
constexpr float kGaitCoyoteLegLengths = 0.5f;

ParticleSpawn MakeDroplet(Vec3 posVoxel, Vec3 vel, uint32_t material,
                          bool micro, int lifeTicks, int microScale) {
  ParticleSpawn s{};
  s.px = (int32_t)std::lround(posVoxel.x * 256.0f);
  s.py = (int32_t)std::lround(posVoxel.y * 256.0f);
  s.pz = (int32_t)std::lround(posVoxel.z * 256.0f);
  s.vx = (int32_t)std::lround(vel.x * 256.0f / 30.0f);
  s.vy = (int32_t)std::lround(vel.y * 256.0f / 30.0f);
  s.vz = (int32_t)std::lround(vel.z * 256.0f / 30.0f);
  s.payload = material & 0xFFFu;
  s.flags = kPFlagAlive;
  if (micro) s.flags |= kPFlagMicro | ParticleMicroBits(microScale, lifeTicks);
  return s;
}

}  // namespace

void PlayerAvatar::Init(Physics* phys, World* world, DebrisSystem* debris,
                        const std::vector<MaterialDef>& mats, MobSystem* mobs) {
  phys_ = phys;
  world_ = world;
  debris_ = debris;
  sys_ = mobs;
  OnMaterialsReloaded(mats);
}

void PlayerAvatar::OnMaterialsReloaded(const std::vector<MaterialDef>& mats) {
  // The density/class tables live on MobSystem now (Mob::DensityOf/ClassOf):
  // ONE table shared by every creature, so the avatar cannot get a material's
  // mass off by one against a mob's. Kept for call-site compatibility.
  (void)mats;
}

void PlayerAvatar::SetDefs(const std::vector<MobDef>* defs,
                           const std::string& defName) {
  // Despawn FIRST: the old def's part list is what tells us which bodies
  // exist, so dropping the pointer before tearing down would leak every limb
  // body and joint into Jolt with nothing left holding their handles.
  Despawn();
  defs_ = defs;
  defName_ = defName;
  def_ = nullptr;
  defIndex_ = -1;  // rides on sever/voice events; -1 is guarded by consumers
  if (!defs_) return;
  for (size_t i = 0; i < defs_->size(); i++)
    if ((*defs_)[i].name == defName_) {
      def_ = &(*defs_)[i];
      defIndex_ = (int)i;
    }
  ResolveParts();
}

void PlayerAvatar::ResolveParts() {
  parts_ = AvatarParts{};
  if (!def_) {
    skel_ = AnimSkeleton{};
    limbDefs_.clear();
    return;
  }
  // Re-seed the owned rig from the (possibly hot-reloaded) def. This runs
  // while DESPAWNED — SetDefs tears the body down first — so there is no
  // equipped item to preserve here; Spawn re-seeds it again and EquipItem
  // re-appends the slot. Doing it in both places keeps "the owned rig always
  // matches the current def" true no matter which entry point ran last.
  skel_ = def_->skel;
  limbDefs_ = def_->limbs;
  heldSlot_ = -1;
  heldItem_.clear();
  const AnimSkeleton& sk = skel_;
  parts_.head = sk.FindPart("head");
  parts_.torso = sk.FindPart("torso");
  parts_.hips = sk.FindPart("hips");
  parts_.handL = sk.FindPart("hand.L");
  parts_.handR = sk.FindPart("hand.R");
  parts_.armUL = sk.FindPart("armU.L");
  parts_.armUR = sk.FindPart("armU.R");
  parts_.armLL = sk.FindPart("armL.L");
  parts_.armLR = sk.FindPart("armL.R");
  parts_.footL = sk.FindPart("foot.L");
  parts_.footR = sk.FindPart("foot.R");
  parts_.legUL = sk.FindPart("legU.L");
  parts_.legUR = sk.FindPart("legU.R");
  parts_.staff = sk.FindPart("staff");
  // Same reasoning as the limbs_ above, for the clips the per-tick locomotion
  // path selects between.
  locoClips_.idle = sk.FindClip("idle");
  locoClips_.walk = sk.FindClip("walk");
  locoClips_.run = sk.FindClip("run");
  locoClips_.fall = sk.FindClip("fall");
  locoClips_.hang = sk.FindClip("hang");
  // Re-resolve the held prop against the new def: a hot reload replaces the
  // skeleton, so a cached part index from the old one would point at whatever
  // limb happens to sit there now.
  heldPartIndex_ = heldPart_.empty() ? -1 : sk.FindPart(heldPart_);
}

// ---- holding an item --------------------------------------------------------
//
// THE ENTITY <-> SLOT SYNC SEAM. Everything that makes a held item behave like
// a limb happens here and nowhere else; see the note in avatar.h.

void PlayerAvatar::SetLook(float yawRel, float pitch) {
  const auto& a = CurrentTuning().avatar;
  const float kDeg = 3.14159265f / 180.0f;
  // Wrap defensively: the caller wraps too, but a stale unwrapped angle here
  // would clamp to the wrong stop rather than to the near one.
  while (yawRel > 3.14159265f) yawRel -= 6.2831853f;
  while (yawRel < -3.14159265f) yawRel += 6.2831853f;
  const float yLim = a.headLookYaw * kDeg;
  // THE NECK LETS GO WHEN THE CAMERA COMES ROUND TO THE FRONT.
  //
  // Third person faces the body at its TRAVEL direction, so orbiting the
  // camera sweeps `yawRel` across the whole circle. Clamped alone, everything
  // past the cone reads as one pose: the head pinned at its stop, craning over
  // a shoulder at a camera it cannot reach. That is the right answer at 90
  // degrees and the wrong one at 180, where the camera is looking the
  // character in the face and the interesting pose is the one the character is
  // actually holding — facing forward.
  //
  // So the goal is scaled down to nothing across the last `headLookReleaseYaw`
  // degrees before straight-behind. Two properties matter and both come from
  // the smoothstep rather than from a lerp:
  //   - it is flat at t=1, so there is no crease where the band begins; the
  //     head keeps sitting at its stop through the whole approach;
  //   - it is flat at t=0, so the released zone is genuinely a zone and not a
  //     single angle, which is the "sweet spot" this exists for. It also makes
  //     the +180/-180 wrap a non-event: both signs reach zero there, so the
  //     goal is continuous even though `yawRel` jumps.
  // The head then eases onto the new goal over headLookHalflife like any other
  // look, so crossing into the band is a turn-back, not a snap.
  //
  // PITCH IS DELIBERATELY NOT RELEASED. Yaw is what hides the character's face
  // from a front-on camera; pitch tracking the camera just means the head is
  // level with whoever is looking at it, which is what you want while circling.
  //
  // First person never gets here: ResolveAvatarHeading drags the body so the
  // offset cannot leave the cone, so |yawRel| stays well under the band.
  float release = 1.0f;
  const float band = a.headLookReleaseYaw * kDeg;
  if (band > 1e-4f) {
    const float t =
        std::clamp((3.14159265f - std::fabs(yawRel)) / band, 0.0f, 1.0f);
    release = t * t * (3.0f - 2.0f * t);
  }
  lookYawGoal_ = std::clamp(yawRel, -yLim, yLim) * release;
  lookPitchGoal_ =
      std::clamp(pitch, -a.headLookPitchDown * kDeg, a.headLookPitchUp * kDeg);
}

uint64_t PlayerAvatar::PartBody(int part) const {
  return (part >= 0 && part < (int)limbs_.size()) ? limbs_[part].body : 0;
}

int PlayerAvatar::PartIndex(const std::string& name) const {
  return def_ ? skel_.FindPart(name) : -1;
}

bool PlayerAvatar::Spawn(const Player& player, float headingRad) {
  if (spawned_ || !def_ || !phys_ || !sys_) return false;
  const MobDef& def = *def_;
  if (def.limbs.empty()) return false;

  // The avatar's prefab min corner sits under the player's AABB: centred in
  // x/z, feet at the bottom of the box. Everything downstream derives from
  // origin_, so this one expression is where "the art lines up with the
  // collision box" is decided.
  const Vec3 at{player.pos.x - def.worldSize.x * 0.5f,
                player.pos.y - Player::kHalfY,
                player.pos.z - def.worldSize.z * 0.5f};
  heading_ = headingRad;
  alive_ = true;
  // A respawn must not inherit the corpse's last glance: the goal is refreshed
  // from the camera on the next tick anyway, but the SMOOTHED value would ease
  // out of a stale twist and the new body would be born looking over its
  // shoulder for a tenth of a second.
  pose_.lookYaw = lookYawGoal_ = 0;
  pose_.lookPitch = lookPitchGoal_ = 0;
  // The player's own gore character, drawn by the SAME entity-variance roll a
  // mob gets at spawn — one gore pipeline (the point of this refactor).
  gore_ = MakeGoreProfile(id_);

  // THE one rig-construction path (Mob::BuildRig) — identical to a mob spawn,
  // including the owned skel_/limbDefs_ copies an item slot appends to. The
  // avatar-layer flag on every body rides the AvatarLayer() override.
  if (!BuildRig(def, at)) return false;
  // Only LEG chains schedule footsteps — the arm chains exist so gameplay can
  // place a hand, and a gait that tried to walk on them would plant the
  // wizard's palms on the ground every stride. The avatar's gait keys on this
  // flag from the first frame (unlike the NPC gait, which lazily plants), so
  // it is set here rather than in the shared BuildRig.
  for (size_t ci = 0; ci < skel_.chains.size(); ci++)
    anim_.feet[ci].valid = skel_.chains[ci].tag == "leg";
  spawned_ = true;
  instancesDirty_ = true;

  // A loaded save's damage state applies HERE (avatar.h persistence note):
  // LoadState ran while the avatar was despawned, so it parked the state and
  // the fresh rig built above is what it applies to. One-shot — an ordinary
  // respawn later in the session must come back whole.
  if (restore_.valid && restore_.defName == defName_) {
    const size_t n = std::min(restore_.parts.size(), limbs_.size());
    for (size_t i = 0; i < n; i++) limbs_[i].hp = restore_.parts[i].hp;
    // A SAVED AMPUTATION IS STILL AN OPEN STUMP (sim/tuning.h Gore §F). The
    // wound is not in the file and does not need to be: it is DERIVED from
    // what is — a severed part whose parent is still on the body reopens the
    // parent's stump, placed at the joint exactly as Sever() places it. The
    // rig was just built at rest (BuildRig writes every limb's xf), so the
    // anchor arithmetic is the same one Sever() runs on a live pose. A loss
    // that fire cauterised is indistinguishable here and reopens too; that
    // is the conservative side of a bleed.
    for (size_t i = 0; i < n; i++) {
      if (restore_.parts[i].alive || !limbs_[i].body) continue;
      const MobLimb& cut = limbs_[i];
      const Quat cq{cut.xf.quat[0], cut.xf.quat[1], cut.xf.quat[2],
                    cut.xf.quat[3]};
      const Vec3 anchorW = cut.xf.pos + Rotate(cq, cut.anchorLimb);
      for (size_t k = 0; k < limbDefs_.size() && k < limbs_.size(); k++) {
        if (limbDefs_[k].name != limbDefs_[i].parent || !limbs_[k].body)
          continue;
        MobLimb& parent = limbs_[k];
        const Quat pq{parent.xf.quat[0], parent.xf.quat[1], parent.xf.quat[2],
                      parent.xf.quat[3]};
        parent.woundLocal = RotateInv(pq, anchorW - parent.xf.pos);
        parent.stumpOpen = true;
      }
    }
    for (size_t i = 0; i < n; i++)
      if (!restore_.parts[i].alive && limbs_[i].body) DetachLimb((int)i, false);
  }
  restore_.valid = false;
  return true;
}

void PlayerAvatar::Despawn() {
  // Shared teardown (Mob::ReleaseRig): joints, bodies, owned bricks, burn
  // indices, and any in-flight severed holds — the identical path a mob takes
  // on reset/despawn, so neither side can forget the brick return.
  if (phys_) ReleaseRig();
  limbs_.clear();
  anim_ = AnimState{};
  pendingSpawns_.clear();
  spawned_ = false;
  alive_ = true;
  instancesDirty_ = true;
}

void PlayerAvatar::SetHiddenParts(const std::vector<uint8_t>& hidden) {
  if (hidden_.size() == hidden.size() &&
      std::equal(hidden_.begin(), hidden_.end(), hidden.begin()))
    return;                       // no change: don't force an instance rebuild
  hidden_ = hidden;
  hidden_.resize(limbs_.size(), 0);
  instancesDirty_ = true;
}

// ---- locomotion coupling ----------------------------------------------------

AvatarLocomotion PlayerAvatar::Locomotion() const {
  AvatarLocomotion out;
  out.alive = alive_;
  if (!def_ || !spawned_) return out;
  const AnimSkeleton& sk = skel_;
  if (anim_.locoState >= 0 && anim_.locoState < (int)sk.states.size()) {
    const AnimStateRule& rule = sk.states[anim_.locoState];
    out.stateIndex = anim_.locoState;
    out.stateName = rule.name.c_str();
    out.speedScale = rule.speedScale;
    // The pose and the camera must agree about how low the body is, so the
    // camera reads the DRAWN body rather than the authored number that used to
    // determine it. `bodyY_ - origin_.y` is exactly the drop the pose pipeline
    // settled on this tick, which for a prone state is now the fitted ground
    // and this rig's own hip height rather than a hand-guessed offset (see
    // AnimStateRule::groundAlign). For an upright clip-owned state the two are
    // identical — bodyY_ eases to origin_.y + bodyYOffset — so nothing about a
    // hop changes, and a tuning edit still moves the camera with the pose.
    if (rule.disableGait) {
      const float standing = std::max(def_->worldSize.y, 0.01f);
      out.eyeHeightScale =
          std::clamp(1.0f + (bodyY_ - origin_.y) / standing, 0.15f, 1.0f);
    }
  }
  // ---- THE DRAG COSTS YOU SPEED; A HOP DOES NOT (kDragSpeedPenalty) -------
  // Multiplied onto whatever the state authored rather than replacing it: the
  // state says what a one-legged body is worth, this says what dragging that
  // body along the floor costs on top. Zero when both feet are on, zero in the
  // air, and eased in and out with the pose, so there is no threshold to feel.
  out.speedScale *= 1.0f - dragW_ * (1.0f - kDragSpeedPenalty);

  // Jumping is derived from LEG LIVENESS rather than authored per state: it is
  // a physical fact about how many legs are under you, and stating it once
  // here keeps a new state rule from silently getting a free jump.
  int legs = 0;
  const int legParts[2] = {parts_.legUL, parts_.legUR};
  const int footParts[2] = {parts_.footL, parts_.footR};
  for (int s = 0; s < 2; s++)
    if (PartAlive(legParts[s]) && PartAlive(footParts[s])) legs++;
  out.jumpScale = legs >= 2 ? 1.0f : (legs == 1 ? 0.55f : 0.0f);
  out.canJump = legs > 0 && alive_;
  if (!alive_) {
    out.speedScale = 0.0f;
    out.jumpScale = 0.0f;
    out.canJump = false;
  }
  return out;
}

// ---- animation: the PLAYER DRIVER's half -------------------------------------
//
// The pipeline is Mob::PosePipeline (game/pose.cpp) — the same one every NPC
// runs. Until W2-L this file held a second copy of it (UpdateAnimation,
// UpdateGait, UpdateAirPose, UpdateAirDrive, ApplyAirArms, SyncStrideClock);
// every bug fix in that copy is now the one gait, and what remains here is the
// part only the player can supply: the facts in a PoseInputs.
PoseInputs PlayerAvatar::AvatarPoseInputs(bool grounded,
                                          const Vec3& playerVel) const {
  PoseInputs in;
  // THE VELOCITY COMES FROM THE PLAYER, NOT FROM DIFFERENCING OUR OWN ORIGIN:
  // Player::Update runs once per FRAME and PreTick 0..4 times a frame, so a
  // difference over kTickDt measures the wrong interval every frame (pose.cpp
  // says what that did). The controller knows its velocity exactly.
  in.velocity = playerVel;
  in.grounded = grounded;
  in.airPoseEligible = airPoseEligible_;
  // THE PLAYER OWNS THE HEIGHT: the AABB already resolved against the terrain,
  // so origin_.y IS the sole of the boot (gotcha-avatar-gait-height-feedback).
  in.height = PoseInputs::Height::FromDriver;
  // The avatar has never leaned into a grade; see the note in Mob::StepGait.
  in.tiltFromGround = false;
  // THE HEAD LEADS, THE BODY FOLLOWS: main.cpp holds the body's facing still
  // while the camera stays inside the neck's cone and SetLook turns the camera
  // into this goal. The spine share is `avatar.headLookSpine` rather than the
  // def's `aimSpineShare`: a player's idle glance is a FEEL question with a
  // slider behind it, and a creature's is anatomy.
  in.haveLook = true;
  in.lookYaw = lookYawGoal_;
  in.lookPitch = lookPitchGoal_;
  in.lookSpineShare = CurrentTuning().avatar.headLookSpine;
  in.crouch = crouchWant_;
  in.hangActive = hangActive_;
  in.hangLip = hangLipW_;
  in.hangDir = hangDirW_;
  return in;
}

// THE LIVE HALF OF THE BODY VELOCITY (Mob::BodyVelocity / AddBodyVelocity):
// the controller's own. Unbound — a harness that never runs the controller —
// it answers with what PreTick last mirrored and absorbs nothing.
Vec3 PlayerAvatar::DriverVelocity() const {
  return player_ != nullptr ? player_->vel : playerVel_;
}

void PlayerAvatar::AddDriverVelocity(Vec3 vps) {
  // Exactly what session.cpp's spell loop did for the caster before W2-L
  // (`player.vel.y += bi.vps.y`), now reached through the body like every
  // other creature's lift.
  if (player_ != nullptr) player_->vel = player_->vel + vps;
}

// ---- per-tick ---------------------------------------------------------------

void PlayerAvatar::PreTick(uint32_t tick, const Player& player, float heading,
                           float dt, World& world, std::vector<BrushOp>& ops,
                           std::vector<CellOp>& cellOps,
                           std::vector<ParticleSpawn>& spawns) {
  if (!spawned_ || !def_) return;
  const MobDef& def = *def_;
  // Per-voxel burning and dissolution, once per TICK — never per frame. The
  // pass writes fire into the hashed grid, so running it off the render clock
  // would make the world a function of frame rate.
  BurnParts(tick, world, cellOps, spawns);
  if (!spawned_ || !def_) return;  // burnt to death: nothing left to drive

  // Shared per-tick body upkeep (Mob) — identical to what MobSystem::PreTick
  // runs for every NPC: drain gore authored outside the tick, and tick the
  // severed holds down (a released piece becomes loose Debris and clears the
  // player first, Mob::EndSeveredHold, the same as an NPC's).
  DrainPendingSpawns(world, spawns);
  TickSeveredHolds(dt);
  // ...and the hit flash, which is the same kind of thing: per-limb state that
  // MobSystem::PreTick ages for every NPC and that nothing else ages for the
  // avatar. Without this line a severed player limb leaves its stump lit at
  // combatfx.flashSever forever (the reported permanent glow).
  DecayHitFlash(dt);

  if (alive_ && ragdoll_ == RagdollPhase::Limp) {
    // ---- LIMP: Jolt has the body; the player is a passenger ----
    // Same as the NPC loop (MobSystem::PreTick): no driver, no pose, the
    // limbs are dynamic and PostStep reads them back. The air clocks are
    // held at zero so the landing edge below cannot fire "land" or a fall
    // clip against a body that is lying down.
    TickRagdollLimp(world, dt);
    // ---- A LIMP BODY STILL HITS THE GROUND -------------------------------
    //
    // The driven branch bills impact damage off Player::impactDeltaV, which is
    // velocity the CONTROLLER'S SWEEP refused. A ragdoll has no controller:
    // main.cpp teleports the capsule onto the pelvis and zeroes its velocity
    // every tick (RagdollFollow), so the sweep never refuses anything and the
    // whole of a limp fall was free — which is the wrong way round, since the
    // reason a body is limp at all is usually that it has been falling for
    // three seconds. Mob::TakeRagdollImpact is the SAME measurement (a sudden
    // deceleration, peak-held, in voxels/s) taken where it still exists: off
    // the solver, at the pelvis. One ApplyFallDamage, one set of thresholds
    // (player.fallDamageSpeed / fallSplatSpeed), both paths.
    //
    // Billed at the pelvis rather than at player.pos: the capsule is a
    // passenger here and RagdollFollow has not moved it yet this tick, so a
    // splat would carve the body at where the camera was last frame.
    ApplyFallDamage(TakeRagdollImpact(), RootWorldPos(), tick, world, ops,
                    spawns);
    airOffTime_ = 0.0f;
    airTime_ = 0.0f;
    wasGrounded_ = true;
    hangActive_ = false;
    pose_.hangIkWeight = 0.0f;
    // Jolt owns every limb here and no pose pass runs, so the air pose must be
    // put away rather than left faded: getting up starts from the pose the
    // ragdoll left, and a stale air weight would aim the first solve of the
    // get-up at a tuck.
    airPoseEligible_ = false;
    pose_.airFrac = 0.0f;
    pose_.airLandW = 0.0f;
    pose_.airLeanPitch = pose_.airLeanRoll = 0.0f;
  } else if (alive_ && ragdoll_ == RagdollPhase::GetUp) {
    // ---- GETTING UP: the shared pipeline, no player input ----
    // origin_/heading_ are what BeginGetUp derived from where the pelvis
    // lay, NOT the player's (the capsule is being moved to us, see
    // RagdollFollow). Grounded, standing still: the get-up blend in
    // SubmitPose does the rest.
    TickGetUp(dt);
    hangActive_ = false;
    crouchWant_ = false;
    airPoseEligible_ = false;
    playerVel_ = Vec3{};
    PosePipeline(AvatarPoseInputs(/*grounded=*/true, Vec3{}), dt, world, tick);
    SubmitPose(dt, /*writeXf=*/true);
  } else if (alive_) {
    // The body follows the PLAYER, which is the whole difference from a mob.
    origin_ = Vec3{player.pos.x - def.worldSize.x * 0.5f,
                   player.pos.y - Player::kHalfY,
                   player.pos.z - def.worldSize.z * 0.5f};
    heading_ = heading;

    // Mirror the hang state for the arm-IK block; the pipeline itself stays
    // player-free by design (it reads PoseInputs, which AvatarPoseInputs
    // fills from these).
    playerVel_ = player.vel;
    hangActive_ = player.hanging;
    crouchWant_ = player.crouching;
    // ...and the states that are airborne to the GAIT but must not play a
    // jump/fall pose. A hang and a mantle carry the weight on the hands, a
    // swimmer is carried by the water (the same argument the air CLOCK makes
    // below, one layer up), and fly mode is not falling at all — it used to
    // hold the fall clip open for the whole flight.
    airPoseEligible_ = !player.hanging && player.mantleTimer <= 0.0f &&
                       !player.inLiquid && !player.fly;
    if (player.hanging) {
      hangLipW_ = player.hangLip;
      hangDirW_ = player.hangDir;
    }
    // ---- air bookkeeping, BEFORE the pose ----
    // The debounce below used to live after the pose pass and feed the CLIPS
    // only, with the gait deliberately taking the raw `grounded` bit. That was
    // the wrong half to protect. See kGaitCoyoteSeconds: losing the gait for a
    // tick is not a subtle artifact, it re-plants both feet and fades the leg
    // IK out to the rest hang, so the legs snap to standing in the middle of a
    // stride — the reported "his animation resets every time he goes up a voxel
    // or is off the ground for a fraction of a second, which is all the time".
    // So it is computed here and both consumers read it.
    const float debounce = CurrentTuning().avatar.airDebounce;
    // A hanging body and a mantling one are SUPPORTED, not airborne: the
    // hands (or the committed climb) hold the weight, so neither may trigger
    // the jump/fall clips nor a landing when it ends. Without the mantle
    // half, every water climb-out and ledge pull-up fired "jump" mid-climb
    // the moment the debounce elapsed.
    const bool hangingNow = player.hanging;
    const bool mantling = player.mantleTimer > 0.0f;
    const bool supported = player.grounded || hangingNow || mantling;
    if (supported) airOffTime_ = 0.0f;
    else airOffTime_ += dt;
    const bool airborneNow = airOffTime_ > debounce;

    // THE GAIT'S OWN VIEW OF THE GROUND — longer than the clips' debounce, and
    // bounded by DISTANCE rather than by time alone. A hang or a mantle is
    // support for the clips but not for the gait: the feet are nowhere near a
    // floor in either, and IK-ing them to one is the pose those states exist to
    // replace.
    float gaitLegLength = 0.0f;
    for (size_t c = 0; c < skel_.chains.size() && c < anim_.feet.size(); c++)
      if (skel_.chains[c].tag == "leg")
        gaitLegLength = std::max(gaitLegLength, anim_.feet[c].legLength);
    const bool gaitGrounded =
        player.grounded ||
        (!hangingNow && !mantling && airOffTime_ < kGaitCoyoteSeconds &&
         std::fabs(origin_.y - supportY_) <
             kGaitCoyoteLegLengths * gaitLegLength);

    // A SLIDE IS NOT A WALK. On slippery footing the part of the velocity the
    // legs are not driving (Player::slideVel) is taken out of what the gait
    // and the walk/run clips see, and the planted feet ride along with it --
    // otherwise the body coasts away from feet the gait thinks are planted,
    // and it walks them after it, which reads as walking on ice rather than
    // sliding on it. A swinging foot's endpoints ride too, so a step already
    // in the air when the slide starts lands where it was aimed, relative to
    // the body.
    if (gaitGrounded && (player.slideVel.x != 0.0f || player.slideVel.z != 0.0f)) {
      const Vec3 d{player.slideVel.x * dt, 0, player.slideVel.z * dt};
      for (FootState& f : anim_.feet) {
        f.planted = f.planted + d;
        f.swingFrom = f.swingFrom + d;
        f.swingTo = f.swingTo + d;
      }
    }
    PosePipeline(AvatarPoseInputs(gaitGrounded, player.vel - player.slideVel),
                 dt, world, tick);

    // ---- air state clips ----
    // Grounded transitions drive jump/land; sustained air drives fall. Kept
    // here rather than in main.cpp so every consumer of the avatar gets the
    // same behaviour without restating the thresholds.
    // AIR STATE IS DEBOUNCED, because `grounded` is not a clean signal.
    //
    // THIS is the "arms shoot up straight walking uphill" bug. Crossing bumpy
    // ground, the body genuinely leaves the surface for a fraction of a voxel
    // cresting each bump, so `grounded` drops false for a tick at a time — the
    // player controller has hysteresis and coyote time precisely because of it.
    // The clip logic had none: every one of those flickers ran the `wasGrounded_
    // -> airborne` edge and fired PlayClip("jump"), a one-shot that throws the
    // arms up. Ascending a noisy incline retriggers it over and over, which is
    // exactly the reported tweaking — and it also explains why the arms were
    // the loudest part of it, since the jump clip is an ARM pose while the legs
    // are mostly IK.
    //
    // So the transition runs on SUSTAINED air, not on the raw bit: you must be
    // off the ground for airDebounce seconds before the body believes it is
    // airborne. A real jump clears that in one tick of upward travel; a bump
    // crest never does. `supported` / `airOffTime_` / `airborneNow` are all
    // computed above the pose now, because the GAIT needs the same protection
    // (and rather more of it) — see the note at the PosePipeline call.
    //
    // RISING EDGE of the jump latch, not the latch itself. `player.jumped` is
    // sticky until main.cpp drains it after the whole tick batch, so on a frame
    // that fires four ticks the raw flag reads true in all four — and
    // PlayClipIndex REWINDS a one-shot on retrigger, which would pin the jump
    // clip at t=0 for the frame and play nothing at all.
    const bool jumpLatched_ = player.jumped && !prevJumpLatch_;
    prevJumpLatch_ = player.jumped;

    // `wasGrounded_` now tracks the DEBOUNCED state, so these edges fire once
    // per real takeoff/landing rather than once per bump.
    if (!airborneNow) {
      // Catching a ledge is not a landing: the feet never arrive, so neither
      // the land clip nor its footfall may fire on the grab frame.
      if (!wasGrounded_ && airTime_ > 0.25f && !hangingNow) {
        PlayClip("land");
        // A landing is its own sound, not a footstep: both feet arrive at once
        // and the impact carries the fall. Uses the fall speed remembered from
        // the last airborne tick, because by the time `grounded` is true the
        // collision sweep has already zeroed the downward velocity.
        Footfall ff;
        ff.posVox = Vec3{player.pos.x, origin_.y, player.pos.z};
        ff.speed = speedNow_;
        ff.foot = 0;
        ff.landing = true;
        ff.fallSpeed = lastFallSpeed_;
        int gy = 0;
        uint32_t gmat = 0;
        if (GroundHeightAt(world, ifloor(player.pos.x), ifloor(player.pos.z),
                           ifloor(origin_.y) + 2, gy, &gmat))
          ff.mat = gmat;
        if (ff.mat != 0) PushFootfall(ff);
        // BOTH feet arrive on a landing, so both print. Per chain rather than
        // once at the body centre: the two soles are a stride apart and a
        // landing that stamped one cell twice would be a smaller mark than a
        // walk's, which is backwards.
        for (size_t c = 0; c < skel_.chains.size() && c < anim_.feet.size();
             c++) {
          const IkChain& ch = skel_.chains[c];
          if (ch.tag != "leg") continue;  // arm chains never land
          if (!anim_.feet[c].valid) continue;
          ShedCoat(ch.effector, anim_.feet[c].planted, tick, world);
        }
      }
      airTime_ = 0;
    } else if (player.inLiquid) {
      // WATER IS NOT AIR.
      //
      // `grounded` is false for a body the water is carrying (Player::Update
      // sets `onGround = drop >= 0 && !swimming`, and a swimmer has no floor
      // under it in any case), so nothing above this made a swimmer anything
      // but airborne and the air clock ran the entire swim. A WADER is a
      // different thing and never reaches here — it is grounded, and takes the
      // branch above with the rest of the walking bodies, which is right: it
      // is standing on the ground with its feet wet. Every airborne rule fired
      // off the air clock: the fall
      // flail opened the arms out underwater, and at ragdoll.fallSeconds — 3 s,
      // which is a short swim — the swimmer went limp in the water.
      //
      // Buoyancy carries the weight, so the water is support for the air
      // clock even though it is not ground. Holding airTime_ at zero disarms
      // all three rules at once: no limp, no flail weight (the ramp below is
      // driven by airTime_), and no landing clip on climbing out, since that
      // one is gated on airTime_ > 0.25 s. A fall INTO water zeroes it on the
      // frame of entry, which is correct: the splash ends the fall.
      //
      // The gait is untouched — `supported` and `airOffTime_` still read the
      // swimmer as off the ground, which is what the leg IK needs.
      airTime_ = 0;
      lastFallSpeed_ = 0.0f;
    } else {
      // A JUMP IS A LAUNCH, NOT A LOSS OF CONTACT.
      //
      // This fired on the `wasGrounded_ -> airborne` edge, which any downward
      // step produces: walking at 16 voxels/s a one-voxel drop is airborne for
      // longer than the 0.12 s debounce, so ordinary broken ground retriggered
      // an arms-up one-shot over and over. `player.jumped` is set where the
      // jump impulse is actually applied and is sticky across the tick batch
      // (see Player::jumped), so it says what the edge could not.
      // The velocity-driven air pose OWNS the body while it is on: the jump
      // clip is an additive arms-and-thighs pose over joints the air IK then
      // overwrites, and its torso keys would fight the lean. Off, this is the
      // old path exactly.
      if (jumpLatched_ && !wasHanging_ && !CurrentTuning().avatar.airPose)
        PlayClip("jump");
      airTime_ += dt;
      // ---- LONG ENOUGH IN THE AIR TO GO LIMP (sim/tuning.h Ragdoll) ----
      // The same rule an NPC falls under (MobSystem::UpdateFall). The limbs
      // take the player's velocity with them so the body keeps falling at
      // the speed it had instead of stalling for a tick; from here the
      // capsule follows the pelvis (RagdollFollow) until the get-up ends.
      // `blindFall` means the controller is HOLDING the drop because the CPU
      // mirror cannot see the ground yet (Player::blindFall) — the body is not
      // moving, so it has not fallen far enough to go limp however long the
      // clock has run.
      // ONE RULE, Mob::ShouldGoLimp; the exemptions are the player's facts.
      LimpExemptions ex;
      ex.fly = player.fly;
      ex.hanging = hangingNow;
      ex.blindFall = player.blindFall;
      if (ShouldGoLimp(airTime_, ex)) GoLimpFromFall();
      // ...AND A FALL IS A DROP. Air time alone says nothing about height:
      // `supportY_` records where the body last had something under it, so the
      // clip waits for real distance to have been given up. A step-down never
      // qualifies no matter how long the debounce takes to clear.
      const float dropped = supportY_ - origin_.y;
      const float minDrop = CurrentTuning().avatar.fallMinDrop / kVoxelMeters;
      // Same hand-over as the jump clip above: with avatar.airPose on, the
      // shape of a fall is a function of vel.y and this loop has nothing to
      // add. (It is also where the "wobbles left and right" came from — the
      // clip's two keyframes are ten degrees apart over 900 ms.)
      if (airTime_ > 0.45f && dropped > minDrop &&
          !CurrentTuning().avatar.airPose)
        PlayClip("fall");
      // Remember how fast we are falling; the landing tick needs it after the
      // sweep has already cancelled the velocity.
      lastFallSpeed_ = player.vel.y < 0 ? -player.vel.y : 0.0f;
    }
    // THE FLAIL RAMPS IN; IT DOES NOT SWITCH ON.
    //
    // The clip itself is authored near-natural now, and this weight is what
    // opens it out into the wide arms-out pose. So a short drop plays a barely
    // perceptible shape and only a sustained fall reaches the full one — the
    // difference between "the character keeps dropping into a falling pose"
    // and a fall that reads as a fall. Applied to the running instance's
    // requested weight, which AnimSampleAndBlend already multiplies through
    // its blend-in fade, so the two compose instead of fighting.
    {
      const auto& av = CurrentTuning().avatar;
      const float ramp = std::max(av.fallFlailRamp, 1e-3f);
      const float w =
          std::clamp((airTime_ - av.fallFlailDelay) / ramp, 0.0f, 1.0f);
      for (ClipInstance& inst : anim_.clips)
        if (inst.clip == locoClips_.fall) inst.weight = w;
    }
    // Where the body last had something under it. Sampled while SUPPORTED, so
    // it survives the whole of the following fall; a hang or a mantle counts,
    // because releasing one is a fall from there and not from the last floor.
    if (supported) supportY_ = origin_.y;
    // The DEBOUNCED state, not the raw bit: storing the raw one would put the
    // edge detection straight back on the flickering signal this block exists
    // to filter, and the jump clip would retrigger on every bump again.
    wasGrounded_ = !airborneNow;
    // Updated only WHILE supported: the jump-clip edge fires a debounce
    // interval after the support was lost, so a per-frame copy would already
    // read false by then. This holds "was the most recent support a hang"
    // until the next real support replaces it.
    if (supported) wasHanging_ = hangingNow;

    // Impact damage. Deliberately OUTSIDE the landing edge above: the latch
    // already means "a sweep just refused this much velocity", which is true of
    // a wall slam that never touches the ground and of a landing whose grounded
    // edge is still inside its debounce. main.cpp clears the latch after this
    // tick, so a 4-tick frame cannot bill the same hit four times.
    ApplyFallDamage(player.impactDeltaV, player.pos, tick, world, ops, spawns);

    // ---- locomotion clips ----
    // Additive arm swing over the IK legs; `run` replaces `walk` past half of
    // the def's top speed. Both are retriggered every tick, which PlayClip
    // turns into a no-op once the instance exists.
    // Debounced, not the raw bit: gating the locomotion clips on the flickering
    // `grounded` made walk/run drop out for a tick on every bump crest, and a
    // clip that stops and restarts never gets past a fraction of its blend-in
    // weight (the same mechanism as the walk/run hysteresis note below).
    const bool moving = speedNow_ > 0.4f && !airborneNow;
    // `def.speed` is the SPRINT reference, so a plain walk (35 of 60 voxels/s)
    // already sits at 0.58 of it — right on top of a 0.55 threshold. The clip
    // selection then flipped between walk and run every frame, and since each
    // one blends in over 180 ms neither ever got past a fraction of its weight:
    // the authored 28-degree arm swing rendered as about 4 degrees of twitch,
    // which is the "arms held out stiff" look. Split at the midpoint between
    // walk and sprint instead, with hysteresis so speeds that sit near the
    // boundary pick one and stay there.
    const float runOn = def.speed * 0.80f;
    const float runOff = def.speed * 0.70f;
    running_ = running_ ? (speedNow_ > runOff) : (speedNow_ > runOn);
    const bool running = running_;
    {
      // IDLE MUST STOP WHEN YOU MOVE. It was started but never retired, and it
      // is a LOOPING ADDITIVE keyed on the same arms and spine the walk swing
      // drives — so it kept composing with the walk forever, dragging an
      // authored 14-degree arm swing down to about 4 and leaving the arms
      // nearly rigid. That near-motionless pose is the "arms outstretched like
      // a zombie" look. Each of the three is exclusive with the other two, so
      // retire the two that do not apply rather than only the locomotion pair.
      // FALL BELONGS TO THIS FAMILY TOO, and leaving it out was a real leak:
      // `fall` is a LOOPING clip and nothing anywhere retired it, so a single
      // airborne moment past 0.45 s started it and it then played FOREVER,
      // composing over walk and run for the rest of the session. That is the
      // same failure the idle note above describes, one clip over — and it is
      // why the legs kept reading wrong on the ground after any jump or drop.
      // Landing must retire it; it is exclusive with the three locomotion
      // clips by construction (you are either on the ground or you are not).
      // Resolved once per def load in ResolveParts, not per tick — see
      // AvatarLocoClips. This block runs on every one of PreTick's four calls a
      // frame, and FindClip is a linear scan of std::string compares.
      const int ic = locoClips_.idle;
      const int wc = locoClips_.walk;
      const int rc = locoClips_.run;
      const int fc = locoClips_.fall;
      const int hc = locoClips_.hang;
      // Hang joins the exclusive family: while dangling from a ledge the
      // arms belong to the reach-up pose and nothing else. A def without a
      // hang clip (hc < 0) simply falls back to idle arms.
      // WITH avatar.airPose ON THE AIRBORNE MEMBER OF THE FAMILY IS `idle`.
      // `fall` is never started in that mode, so asking for it here would retire
      // idle every tick while the line below restarts it — and PlayClipIndex
      // skips an instance already marked `stopping`, so each tick APPENDS a new
      // one. Naming idle keeps the arms' base pose alive under the air IK, which
      // is what the note below says the airborne case wants anyway.
      const int want =
          hangingNow && hc >= 0
              ? hc
              : airborneNow ? (CurrentTuning().avatar.airPose ? ic : fc)
                            : (!moving ? ic : (running ? rc : wc));
      for (ClipInstance& inst : anim_.clips) {
        if (inst.clip < 0) continue;
        if ((inst.clip == ic || inst.clip == wc || inst.clip == rc ||
             inst.clip == fc || inst.clip == hc) &&
            inst.clip != want)
          inst.stopping = true;
      }
    }
    // Deliberately NOT `PlayClipIndex(want)`: when airborne
    // `want` is `fall`, but this line has always started `idle` there (`moving`
    // is false while airborne), and the airborne branch above owns starting
    // `fall`. Keeping idle's blend alive under a jump is what stops the arms
    // snapping on landing, so the airborne case must stay idle, not want.
    // Hanging is the exception: the hang pose is started HERE (there is no
    // edge-driven owner for it the way jump/fall have one), and it must be,
    // or the retire block above would empty the family and leave rest arms.
    PlayClipIndex(hangingNow && locoClips_.hang >= 0
                      ? locoClips_.hang
                      : moving ? (running ? locoClips_.run : locoClips_.walk)
                               : locoClips_.idle);

    // ---- lock the arm swing to the feet --------------------------------
    // The walk and run clips are authored at ONE speed each (the arm cycle is
    // derived from the runtime's own step model at walk pace and at sprint
    // pace). Every speed between them — which is most of them — plays an arm
    // cycle the feet do not share, and the arms slide in and out of phase with
    // the legs over a few strides. Re-rating the instance to the live stride
    // makes one authored cycle span one stride at any pace, so the derivation
    // in gen_human.py stops being a special case and becomes the value the
    // rate is 1.0 at.
    //
    // Only the locomotion pair: `idle`, `fall`, `hang`, `jump` and `land` are
    // not stride-locked motions and must keep their authored timing.
    if (pose_.strideRate > 1e-4f) {
      for (ClipInstance& inst : anim_.clips) {
        if (inst.clip != locoClips_.walk && inst.clip != locoClips_.run)
          continue;
        if (inst.clip < 0 || inst.clip >= (int)skel_.clips.size()) continue;
        const float durS = (float)skel_.clips[inst.clip].durationMs * 0.001f;
        if (durS <= 1e-4f) continue;
        // rate 1 == the clip's authored period equals one stride.
        inst.rate = std::clamp(pose_.strideRate * durS, 0.25f, 3.0f);
      }
    }

    // ---- submit kinematic targets (shared Mob path, held item included) ----
    // writeXf=true: the held-item placement and the camera read the hand's
    // FRESH pose this tick (see Mob::SubmitPose).
    SubmitPose(dt, /*writeXf=*/true);
  }

  // ---- bleeding: THE mob bleed drive (Mob::BleedTick) — decaying wound
  // budgets, dismemberment gouts, gore-profile variance and drip spray, under
  // the avatar's own per-tick op counter ----
  int bleedOps = 0;
  BleedTick(tick, world, ops, spawns, bleedOps);
}

bool PlayerAvatar::RagdollFollow(Vec3& outPlayerPos) const {
  if (!spawned_ || !alive_ || !def_ || !Ragdolled()) return false;
  const MobDef& def = *def_;
  if (ragdoll_ == RagdollPhase::Limp) {
    // The pelvis body's origin is its lattice corner; lift a little so the
    // camera boom pivots inside the body rather than at its underside.
    outPlayerPos = RootWorldPos() + Vec3{0.0f, MetresToCells(0.2f), 0.0f};
  } else {
    // The standing spot: the inverse of the origin_ line in PreTick.
    outPlayerPos = Vec3{origin_.x + def.worldSize.x * 0.5f,
                        origin_.y + Player::kHalfY,
                        origin_.z + def.worldSize.z * 0.5f};
  }
  return true;
}

// ---- damage -----------------------------------------------------------------

// ---- health, as the caster VM sees it (game/spell.h) ------------------------
//
// The player deliberately gains no hp field: health IS the per-part hp the
// dismemberment system already maintains, so the mana bar's overdraw and the
// visible damage state cannot drift apart.

// HAIR IS NOT HEALTH. A `bloodless` base limb (a generated character's
// `hair`/`mane`) is left out of all three of TotalHealth, HealthMax and
// SpendHealth -- the same set Mob::TotalHp/DrainBlood leave out -- so a
// haircut neither empties the bar nor makes the bar longer. Asked of the DEF
// field and not of Mob::IsBloodless, which also folds in worn slots: whether
// a garment's hp belongs in the player's bar is a separate question these
// three have always answered "yes" to, and this change does not reopen it.
int32_t PlayerAvatar::TotalHealth() const {
  if (!spawned_ || !alive_) return 0;
  float sum = 0;
  for (size_t i = 0; i < limbs_.size(); i++)
    if (PartAlive((int)i) && limbs_[i].hp > 0 &&
        !(i < limbDefs_.size() && limbDefs_[i].bloodless))
      sum += limbs_[i].hp;
  return sum <= 0 ? 0 : (int32_t)sum;
}

int32_t PlayerAvatar::HealthMax() const {
  // The AUTHORED total, read straight back off the def rather than cached at
  // spawn: an intact avatar's TotalHealth() must equal this, and taking both
  // numbers from the same source is what guarantees it. A severed limb lowers
  // TotalHealth but NOT this, so the HUD bar shows the missing chunk instead of
  // silently rescaling itself to the smaller body.
  if (!def_) return 0;
  float sum = 0;
  for (const MobLimbDef& ld : limbDefs_)
    if (ld.hp > 0 && !ld.bloodless) sum += ld.hp;
  return sum <= 0 ? 0 : (int32_t)sum;
}

int32_t PlayerAvatar::HealthCap() const {
  const float cap = (float)HealthMax() * BurnHealthCap();
  return cap <= 0.0f ? 0 : (int32_t)cap;
}

void PlayerAvatar::SpendHealth(int32_t amount) {
  if (!spawned_ || !alive_ || amount <= 0) return;
  // Spread across live slots in proportion to what each still has, through
  // Mob::Damage with the Other cause ("the caster's cost",
  // phys/damagecause.h) -- Mob::SpendHp. It used to subtract the hp here and
  // Sever() whatever reached zero, which contradicted the hp-kills-in-place
  // rule every other damage path reads (W2-H).
  SpendHp((float)amount, DamageCtx(DamageCause::Other));
}

void PlayerAvatar::SelfDestruct(Vec3 atWorldVoxel, float radiusVox,
                                World& world,
                                std::vector<ParticleSpawn>& spawns) {
  if (!spawned_ || !def_) return;
  // A FATAL OVERCAST TEARS THE BODY THE WAY A BLAST DOES, and then it dies.
  // This severed every severable part in range "because the avatar has no
  // CarveLimb" -- it has had Mob's since the refactor that made PlayerAvatar a
  // Mob, so the carve is the ordinary radial one (ejected gobbets,
  // connectivity splits, collapse severing, the crater soak), carrying the
  // caster's-cost cause. 20 cm of margin, as the sever sweep had, so the same
  // real shell of body is reached at any voxel size.
  CarveRadialAll(atWorldVoxel, radiusVox + MetresToCells(0.2f), world, spawns,
                 /*power=*/0.0f, DamageCtx(DamageCause::Other));
  Die();
}

void PlayerAvatar::CarveRadial(Vec3 centerWorldVoxel, float radiusVoxels,
                               World& world,
                               std::vector<ParticleSpawn>& spawns) {
  // Real per-voxel carving through the shared Mob path: the blast takes
  // actual voxels out of the player's body exactly as it does an NPC's —
  // ejected gobbets, connectivity splits, collapse-severing and all. This
  // replaces the old avatar-only hp approximation (the drift this refactor
  // exists to end).
  if (!spawned_) return;
  CarveRadialAll(centerWorldVoxel, radiusVoxels, world, spawns);
}

// (PlayerAvatar::ApplyFallDamage moved to Mob::ApplyFallDamage in W2-H: every
// creature takes a landing now, through the one function.)

bool PlayerAvatar::SeverByName(const std::string& name) {
  int i = PartIndex(name);
  if (i < 0 || !PartAlive(i)) return false;
  Sever(i);
  return true;
}

// ---- per-voxel burning: the avatar's half --------------------------------
//
// docs/PLAN_body_reactivity.md. The PASS is MobSystem::BurnOneLimb and is not
// duplicated here — the player has to catch fire, char and dissolve exactly as
// an NPC does, and one implementation is the only way to be sure of that. What
// is genuinely the avatar's own is what happens AFTER: how hp falls when matter
// is lost, and how a part burnt through comes off.

uint32_t PlayerAvatar::PartMaterialCount(int part, uint32_t mat) const {
  if (part < 0 || part >= (int)limbs_.size()) return 0;
  const MobLimb& p = limbs_[part];
  uint32_t n = 0;
  if (p.HasFineSkin()) {
    for (const PrefabVoxel& v : p.skinVoxels)
      if ((v.material & 0xFFFu) == (mat & 0xFFFu)) n++;
  } else {
    for (const DebrisVoxel& v : p.voxels)
      if ((v.payload & 0xFFFu) == (mat & 0xFFFu)) n++;
  }
  return n;
}

void PlayerAvatar::PartMaterialTally(int part, const uint16_t* binMask,
                                     uint32_t* bins) const {
  if (part < 0 || part >= (int)limbs_.size()) return;
  const MobLimb& p = limbs_[part];
  // The SAME lattice choice PartMaterialCount makes, so a tally and the
  // per-material count can never disagree about which voxels are the limb.
  auto add = [&](uint32_t mat) {
    uint16_t m = binMask[mat & 0xFFFu];
    while (m) {
      const int k = std::countr_zero((unsigned)m);
      bins[k]++;
      m &= (uint16_t)(m - 1);
    }
  };
  if (p.HasFineSkin()) {
    for (const PrefabVoxel& v : p.skinVoxels) add(v.material);
  } else {
    for (const DebrisVoxel& v : p.voxels) add(v.payload);
  }
}

uint32_t PlayerAvatar::IgnitePart(int partIndex, uint32_t count,
                                  uint32_t onlyMat) {
  // Thin wrapper over Mob::Ignite — same resolution of "what does this
  // material become when it catches", out of the same table as any creature.
  if (!spawned_) return 0;
  return Ignite(partIndex, count, onlyMat);
}

void PlayerAvatar::BurnParts(uint32_t tick, World& world,
                             std::vector<CellOp>& cellOps,
                             std::vector<ParticleSpawn>& spawns) {
  if (!sys_ || !spawned_ || !alive_ || !sys_->BurnTablesReady()) return;
  // A terrain anchor around the body keeps the chunks under it FETCHED AND
  // REFRESHED in the CPU mirror; the burn pass reads that mirror to find out
  // whether it is standing in a fire. Without it the player did not burn
  // while mobs did (see Mob::RegisterTerrainAnchor).
  RegisterTerrainAnchor();
  // A separate budget from the mob pass's, and deliberately: the player is
  // one creature out of up to sixteen, and sharing one pool would let a crowd
  // of burning NPCs starve the fire on the character the camera is pointed
  // at. The PASS underneath (Mob::BurnTick -> MobSystem::BurnOneLimb) is the
  // shared one.
  // 4096 was sized for FIRE, whose front is a 2D flame edge a few hundred
  // voxels wide. DISSOLUTION is not a front — acid attacks the whole wetted
  // surface at once, and at skinScale 8 a submerged human is order 25k exposed
  // sub-voxels, so the pass was sampling about a sixth of the body per tick and
  // the character came apart over a minute instead of over seconds. Raised for
  // the player only, in the same spirit the separate budget exists at all: this
  // is one creature, and it is the one the camera is pointed at.
  uint32_t frontBudget = 16384;
  uint32_t opsBudget = 48;
  BurnTick(tick, world, cellOps, spawns, frontBudget, opsBudget);
  // Blood on the player: from the world it is standing in, and from every
  // burst this tick queued (Mob::StainTick / MobSystem::SplatterOnto). Its
  // own budget, for the reason the burn budget above is its own.
  uint32_t stainBudget = 8192;
  // Rain's own pot, as the crowd's is (MobSystem::kRainLatticePerTick): the
  // player in a storm must not dry-starve its own contact/drying passes.
  uint32_t rainBudget = 8192;
  StainTick(tick, world, stainBudget, rainBudget);
  if (sys_) sys_->SplatterOnto(*this);
}

void PlayerAvatar::Revive(const Player& player, float heading) {
  // THE CORPSE FIRST (PLAN_corpse_is_a_mob.md P2b). A registered avatar's
  // dead rig was moved into mobs_ as a dead Mob at the top of the tick after
  // it died (MobSystem::AdoptDeadAvatar), and this avatar is a husk; one
  // that was never registered (a harness), or a respawn in the same tick as
  // the death, is moved here. Despawn would otherwise DESTROY the body.
  if (spawned_ && !alive_ && sys_) sys_->AdoptDeadAvatar(*this);
  Despawn();
  Spawn(player, heading);
}

// ---- persistence (entities.sve section 'AVTR') ------------------------------

void PlayerAvatar::SaveState(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  // A despawned or dead avatar saves as absent: the corpse (if any) is a dead
  // Mob in mobs_ by now (MobSystem::AdoptDeadAvatar) and saves in MOBS as a
  // player corpse, and the player respawns whole — see avatar.h.
  const bool present = spawned_ && alive_ && def_ != nullptr;
  w.U32(present ? 1u : 0u);
  if (!present) return;
  w.Str(defName_);
  // Only the DEF's limbs_: a borrowed item slot past them is inventory, not
  // body, and is re-equipped through EquipItem rather than persisted here.
  const size_t n = std::min(limbs_.size(), def_->limbs.size());
  w.U32((uint32_t)n);
  for (size_t i = 0; i < n; i++) {
    w.Pod((uint8_t)(limbs_[i].body ? 1 : 0));
    w.F32(limbs_[i].hp);
  }
}

bool PlayerAvatar::LoadState(const uint8_t* data, size_t len,
                             uint32_t version) {
  restore_ = SavedState{};
  if (version != kSaveVersion) {
    std::printf("avatar: unknown AVTR section version %u\n", version);
    return false;
  }
  ByteReader r{data, len};
  uint32_t present = 0;
  r.U32(present);
  if (!r.ok || !present) return r.ok;
  SavedState s;
  r.Str(s.defName);
  uint32_t n = 0;
  r.U32(n);
  s.parts.resize(n);
  for (uint32_t i = 0; i < n && r.ok; i++) {
    r.Pod(s.parts[i].alive);
    r.F32(s.parts[i].hp);
  }
  if (!r.ok) return false;
  s.valid = true;
  restore_ = std::move(s);  // applied by the next Spawn()
  return true;
}

// ---- queries ----------------------------------------------------------------

bool PlayerAvatar::PartWorldTransform(int part, Vec3& outPos,
                                      Quat& outRot) const {
  if (part < 0 || part >= (int)limbs_.size() || !limbs_[part].body) return false;
  const MobLimb& p = limbs_[part];
  outPos = p.xf.pos;
  outRot = Quat{p.xf.quat[0], p.xf.quat[1], p.xf.quat[2], p.xf.quat[3]};
  return true;
}

bool PlayerAvatar::PartAnchorWorld(int part, Vec3& out) const {
  if (part < 0 || part >= (int)limbs_.size()) return false;
  const MobLimb& p = limbs_[part];
  if (!p.body) return false;
  Quat q{p.xf.quat[0], p.xf.quat[1], p.xf.quat[2], p.xf.quat[3]};
  out = p.xf.pos + Rotate(q, p.anchorLimb);
  return true;
}

int PlayerAvatar::LivePartCount() const {
  int n = 0;
  for (const MobLimb& p : limbs_)
    if (p.body) n++;
  return n;
}

// ---- render -----------------------------------------------------------------
//
// The three Append* walks MUST visit slots in the same order, because the slot
// a transform lands in is the slot the instance records. This mirrors
// MobSystem's contract exactly.


// ---- collision-box debug overlay (world.h DebugBox) -------------------------
//
// Draws the individual sub-shapes of each compound collider rather than one
// big AABB per body. See rigrender::AppendDebugBoxesFor for why these come
// from the Jolt shape rather than from the voxels that built it.

