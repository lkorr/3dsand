#include "game/avatar.h"

#include "sim/scale.h"  // MetresToCells

#include "phys/lattice.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "game/rigrender.h"
#include "sim/bytestream.h"
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

// Ceiling on the gait's velocity lookahead, in leg lengths. `leadTime` is a
// DURATION, so the unclamped offset grows linearly with speed and blows past
// what a two-bone chain can reach — see the note at the clamp in UpdateGait.
// Just under 1 keeps the planted foot inside the leg's reach annulus even at a
// full-lean sprint, so the IK poses a bent leg instead of a straight pointer.
constexpr float kMaxLeadLegLengths = 0.9f;

// Floor on the speed scaling of the swing, and an absolute floor on the swing
// itself. Together they stop a near-stationary step from swinging for seconds
// and a sprint from snapping the foot across with no visible arc.
constexpr float kMinSwingScale = 0.35f;
constexpr float kMinSwingSeconds = 0.09f;

// THE STRIDE BUDGET — why the feet trailed no matter how the gait was tuned.
//
// Only ONE leg may swing at a time, so during a swing the body advances
// `speed * swingDuration` while the swinging foot advances at most its reach:
// the stance point plus the capped lead, i.e. ~kMaxLeadLegLengths * legLength
// ahead of where the body will be. If the body's travel exceeds that, the foot
// lands BEHIND where it lifted off relative to the body, every single step. The
// error is cumulative and unbounded, so the planted foot ratchets backwards
// until the leg is straight and pointing away — and since a foot can then never
// get back out in front, the legs sit permanently behind the character instead
// of alternating fore and aft.
//
// The numbers on this rig at kVoxelMeters 0.10 (walk 35 world vox/s, legLength
// ~5.79 vox): the body covers 35 * 0.171 = 6.00 voxels per swing against a
// 5.21-voxel lead cap. Net -0.79 voxels PER STEP. No cadence, threshold or
// lead-time value can fix that, because the budget itself is negative — which
// is why tuning it repeatedly changed nothing.
//
// So bound the swing by the budget instead of hoping a constant fits: the
// duration is whatever keeps the body's travel inside the distance the foot can
// actually gain. Expressed as a fraction so the foot lands with margin rather
// than exactly at full extension (a foot that lands at maximum reach is a
// straight, locked leg — the pose we are trying to avoid).
constexpr float kSwingTravelFrac = 0.7f;

// THE STANCE RESERVE — how much of the leg's reach is kept in hand.
//
// A two-bone chain asked for exactly L1 + L2 is a locked, straight leg, and
// AnimSolveTwoBone clamps at dMax = L1 + L2 - eps, so a target at or past full
// extension produces the SAME pose for every target beyond it. Solving at 97%
// leaves a real bend at the knee in the neutral stance and keeps the solve away
// from the numerically nasty end of the acos.
constexpr float kStanceReachFrac = 0.97f;

// Ceiling on the stance crouch, in leg lengths. The crouch below is derived
// from the geometry and is well-behaved, but `strideBias` is authored data —
// a rig that asks for a stride longer than its own leg would otherwise drive
// the pelvis into the floor rather than simply failing to reach.
constexpr float kMaxCrouchLegLengths = 0.30f;

// Half-life on the stance crouch. Short on purpose: the crouch demand
// oscillates once per STEP now (see the stance note in UpdateGait), and a
// half-life anywhere near the step period averages that oscillation away and
// gives back the permanent half-crouch it replaced.
constexpr float kCrouchHalflife = 0.05f;

// How long the GAIT keeps its footing after `grounded` drops, and how far the
// body may have left the ground it last stood on while it does.
//
// These are not the clips' `avatar.airDebounce`, and deliberately longer than
// it: the clips only have to avoid firing a one-shot, whereas losing the gait
// re-plants both feet from scratch (UpdateAirPose clears footInit_) and fades
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

// Stride-clock lock. `kStrideSyncGain` is how much of the phase error is taken
// out at each touchdown: 1 snaps (and pops the bob), 0 never locks. Half
// converges inside two steps while keeping every correction sub-visible once
// locked. `kStepPeriodHalflife` smooths the measured step period so one long
// stride over a ledge does not slew the whole clock.
constexpr float kStrideSyncGain = 0.5f;
constexpr float kStepPeriodHalflife = 0.25f;
// A step slower than this is not a gait — the clock parks rather than crawling.
constexpr float kMaxStepPeriod = 1.2f;

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
  lookYaw_ = lookYawGoal_ = 0;
  lookPitch_ = lookPitchGoal_ = 0;
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
    // The pose and the camera must agree about how low the body is, and the
    // authored bodyYOffset is already that number (in world voxels, negative
    // = lower). Deriving the eye scale from it means a tuning change to the
    // crawl pose moves the camera with it instead of needing a second edit.
    if (rule.disableGait) {
      const float standing = std::max(def_->worldSize.y, 0.01f);
      out.eyeHeightScale =
          std::clamp(1.0f + rule.bodyYOffset / standing, 0.15f, 1.0f);
    }
  }
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

// ---- animation --------------------------------------------------------------

void PlayerAvatar::UpdateGait(float dt, World& world) {
  const AnimSkeleton& sk = skel_;
  const GaitDef& g = sk.gait;
  if (sk.chains.empty()) return;

  Vec3 fwd{std::sin(heading_), 0, std::cos(heading_)};
  float speedFactor =
      std::clamp(speedNow_ / std::max(def_->speed, 0.01f), 0.0f, 1.5f);

  // Stride clock: the time since the last touchdown, which the sync at the
  // bottom of the swing turns into a measured step period. Advanced here so it
  // keeps counting through a stride where no foot happens to land.
  sinceTouchdown_ += dt;

  // ---- THE STANCE CROUCH: where the stride's reach actually comes from -----
  //
  // The avatar pins bodyY_ to the player's AABB sole (see the body-height note
  // at the end of this function), so the hip sits at a FIXED height above the
  // ground. That is the whole stride budget, and on a rig authored standing it
  // is essentially zero: this human's hip is 7.50 above the min corner and its
  // ankle 0.75, a 6.75 span against a 6.79-voxel chain. A foot placed on the
  // ground `s` voxels fore or aft of the hip is sqrt(6.75^2 + s^2) away, so the
  // leg runs out of reach at s = 0.7 VOXELS. Every step longer than that lands
  // on AnimSolveTwoBone's reach clamp, which produces one identical straight-leg
  // pose for every target beyond it — the stride stops existing and the leg just
  // rotates. That is "the legs don't swing enough", and it is geometry, not
  // tuning: no cadence, threshold or amplitude can buy reach the leg has not got.
  //
  // A real walker buys it by bending the knees, so do that.
  //
  // BUT ONLY WHILE THE LEG IS ACTUALLY OUT AT THE END OF ITS STRIDE. The first
  // version of this solved for the WORST-CASE excursion — the far end of the
  // stride the gait was about to ask for — and then held that crouch for the
  // whole cycle. That is the wrong shape, and backwards from a real walk: a
  // walking pelvis is LOWEST at double support, when the feet are apart, and
  // HIGHEST at midstance, when the supporting leg is vertical and straight.
  // Holding the double-support height through midstance means the leg is at its
  // most bent exactly where it should be straightest. Measured on this rig at
  // walk pace it pinned 0.77 voxels of crouch permanently — 88% leg extension,
  // ~56 degrees of knee, held every tick — which is the reported "the character
  // walks on flat ground in a kneeling pose". At a sprint it sat on the
  // kMaxCrouchLegLengths clamp (2.04 voxels) from the first step to the last.
  //
  // So the demand is measured per FOOT, per tick, against where that foot
  // actually is: the highest hip that can still reach it, taken over the legs.
  // At midstance the supporting foot is under the hip and the demand collapses
  // to the reach reserve alone; at touchdown it rises to cover the outstretched
  // leg. The pelvis then falls and rises once per step on its own, out of the
  // geometry, with no oscillator to keep in phase with anything.
  //
  // This is still NOT the feedback path the body-height note warns about, and
  // the reason is worth stating because it now DOES read a foot. Exactly ONE
  // thing is taken from `f.planted`: its HORIZONTAL offset from the hip, which
  // descends from origin_.x/z and the velocity lead and is not a function of
  // bodyY_. The vertical stays the rig's own authored span. The per-leg block
  // below says what happens if you take the foot's height as well.
  //
  // Accumulated by the per-leg block at the bottom of the foot loop below.
  float crouchNeed = 0.0f;
  float crouchLegLength = 0.0f;

  // Exactly ONE gait group may swing at a time — that single constraint IS the
  // gait state machine, and it degrades gracefully when a leg is severed (the
  // survivors simply take their turns sooner).
  int swingingGroup = -1;
  for (size_t c = 0; c < anim_.feet.size(); c++) {
    if (!anim_.feet[c].swinging) continue;
    for (size_t gi = 0; gi < g.groups.size(); gi++)
      for (int p : g.groups[gi])
        if (p == sk.chains[c].effector || p == sk.chains[c].parts[0])
          swingingGroup = (int)gi;
    if (swingingGroup < 0) swingingGroup = (int)c;
  }

  float sumY = 0;
  int nFeet = 0;
  float footLo = 0, footHi = 0;
  Vec3 pivot{def_->worldSize.x * 0.5f, 0, def_->worldSize.z * 0.5f};
  // Best step candidate this frame — see the "neediest foot" note below.
  int bestFoot = -1;
  float bestDrift = 0;
  Vec3 bestGoal{};
  uint32_t bestMat = 0;

  for (size_t c = 0; c < sk.chains.size() && c < anim_.feet.size(); c++) {
    const IkChain& ch = sk.chains[c];
    FootState& f = anim_.feet[c];
    if (ch.tag != "leg") continue;          // arm chains never step
    bool alive = true;
    for (int p : ch.parts)
      if (p >= 0 && p < (int)anim_.partAlive.size() && !anim_.partAlive[p])
        alive = false;
    if (!alive) {
      f.valid = false;
      f.swinging = false;
      continue;
    }
    f.valid = true;

    // Rest stance position for this foot, in world space.
    Vec3 restLocal = sk.parts[ch.effector].rest.pos;
    for (int par = sk.parts[ch.effector].parent; par >= 0;
         par = sk.parts[par].parent)
      restLocal = restLocal + sk.parts[par].rest.pos;
    // restLocal is ALREADY the foot's anchor in PREFAB coordinates: the walk
    // above accumulates rest.pos all the way up through the root, and each
    // rest.pos is a joint anchor measured from its parent's anchor, so the sum
    // telescopes to the effector's absolute prefab anchor. Adding the root
    // anchor on top double-counts it and slides the whole stance by
    // (rootAnchor) — here (2.5, 0, 2.0) world voxels, so both feet were planted
    // 2.5 voxels FORWARD and 2 sideways of where the legs actually hang. The
    // gait then stepped correctly about a stance that was never under the body,
    // the IK stretched to reach it, and the legs raked out behind: the
    // "naruto run". The IK below stays in this same prefab-absolute frame and
    // rebases nothing — see the note at the AnimSolveTwoBone call.
    Vec3 stancePrefab = restLocal;
    Quat yaw = AxisAngle({0, 1, 0}, heading_);
    Vec3 stance = Vec3{origin_.x, bodyY_, origin_.z} + pivot +
                  Rotate(yaw, stancePrefab - pivot);
    // This chain's HIP, in world XZ. anchorLocal is the prefab-absolute joint
    // anchor (mob.cpp builds rest.pos as the delta between two of them), so
    // this is the same frame `stance` above is in. Only the HORIZONTAL is used
    // — see the crouch block at the bottom of the loop for why the vertical
    // deliberately is not.
    const Vec3 hipXZ = origin_ + pivot +
                       Rotate(yaw, sk.parts[ch.parts[0]].anchorLocal - pivot);
    // Lead the target by the current velocity so the foot lands where the body
    // is GOING, not where it was — without this a walk always trails.
    //
    // THE LEAD MUST BE CLAMPED TO THE LEG'S REACH. `leadTime` is a duration, so
    // the raw offset grows without bound with speed: the player moves an order
    // of magnitude faster than the mobs these gait numbers were tuned on
    // (sprintSpeed 6 m/s at kVoxelMeters 0.10 is 60 world voxels/s, against a
    // ~6-voxel leg), which puts the target ~15 voxels behind a leg that can
    // reach ~6. AnimSolveTwoBone then clamps to its reach annulus every frame
    // and the leg simply points at the target — a straight limb trailing the
    // body, which is the "legs flail behind instead of walking" report. Capping
    // the lead at a fraction of leg length keeps the same lean at walk pace and
    // degrades to a reachable stride at a sprint.
    Vec3 lead = Vec3{anim_.velocity.x, 0, anim_.velocity.z} *
                (g.leadTime + g.strideBias * 0.1f);
    const float maxLead = kMaxLeadLegLengths * f.legLength;
    float leadLen = lead.len();
    if (leadLen > maxLead) lead = lead * (maxLead / leadLen);
    Vec3 goal = stance + lead;
    // Probe from just above the SOLE (origin_.y), and fall back to the sole
    // when the probe misses. Both used to reference bodyY_, which is what let
    // the height feed itself — see the body-height note at the end of this
    // function. origin_.y is player-owned and cannot be perturbed from here,
    // so the fallback is a fixed reference rather than a feedback path.
    // THE IK EFFECTOR IS THE ANKLE, NOT THE SOLE.
    //
    // GroundHeightAt returns the SURFACE (the top face of the solid, `y + 1`),
    // which is the plane the player's AABB and the art's min corner both rest
    // on. The chain this goal becomes a target for ends at the ankle JOINT, and
    // on this rig that joint sits `restSoleY_` (0.75 world voxels) above the min
    // corner in the authored pose. Handing the solver the surface therefore asks
    // for the full hip-to-min-corner drop — 7.50 voxels out of a 6.79-voxel
    // chain — so AnimSolveTwoBone clamped to its reach annulus on EVERY tick of
    // every walk, and a clamped solve returns one fixed straight-leg pose no
    // matter what the gait computed. Every subtlety upstream (the lead, the
    // re-target, the landing ease) was being thrown away at this one line.
    //
    // restSoleY_ is measured off the rig in Mob::BuildRig, so this is exact for
    // any character rather than a per-def trim. `footTrim` rides here too — the
    // probe and the pose have to agree about where the foot is, and applying it
    // only at draw time meant the IK solved for one contact point and the art
    // showed another.
    const float ankleRise = restSoleY_ + CurrentTuning().avatar.footTrim;
    int gy = 0;
    uint32_t gmat = 0;
    if (GroundHeightAt(world, ifloor(goal.x), ifloor(goal.z),
                       ifloor(origin_.y) + 2, gy, &gmat))
      goal.y = (float)gy + ankleRise;
    else
      goal.y = origin_.y + ankleRise;

    if (!footInit_) {
      f.planted = goal;
      f.swinging = false;
    }
    if (f.swinging) {
      // SWING TIME IS BOUNDED BY THE STRIDE BUDGET, not just scaled by speed.
      //
      // Scaling by speedFactor (the previous fix) is directionally right — a
      // faster gait does swing faster — but it is still a CONSTANT divided by a
      // number, so nothing ties it to the distance the leg can actually cover.
      // On this rig it settles at 0.171 s at walk pace, during which the body
      // travels 6.00 voxels against a 5.21-voxel reach: negative budget, feet
      // ratchet backwards forever. See kSwingTravelFrac above.
      //
      // So take the speed-scaled duration as the DESIRED swing and then cap it
      // at the budget: the swing may never last longer than it takes the body
      // to consume the ground the foot is able to gain. At walk pace that caps
      // 0.171 s at ~0.104 s, which turns a -0.79 voxel net step into a positive
      // one — the foot lands ahead of the body and the legs alternate fore and
      // aft the way they should. At low speed the budget is enormous and the
      // speed scaling governs, so a slow walk is unaffected.
      float durScale = std::clamp(speedFactor, kMinSwingScale, 1.5f);
      float dur = std::max(g.stepDuration / durScale, kMinSwingSeconds);
      const float reach = kMaxLeadLegLengths * f.legLength;
      if (speedNow_ > 0.01f) {
        float budget = kSwingTravelFrac * reach / speedNow_;
        dur = std::min(dur, budget);
      }
      dur = std::max(dur, kMinSwingSeconds);
      // THE ARC MUST LAND ON THE GROUND, NOT ABOVE IT.
      //
      // The swing height is a parabola, sin(t*pi)*stepHeight*legLength, which
      // is only zero exactly at t = 1. The step used to end on the first tick
      // where swingT >= 1, and then assign `planted = swingTo` — but the
      // PREVIOUS tick rendered the foot at, say, t = 0.97, still a third of a
      // voxel up in the air. So touchdown dropped the foot that whole distance
      // in a single frame, every step. Through the two-bone IK that is a large
      // instantaneous change in knee and hip angle: measured at 45-48 degrees
      // of leg rotation in one tick on a walk. On flat ground it is a subtle
      // hitch; on rising ground the probe returns a new height each tick, the
      // arc lands higher than it lifted, and the snap grows — which is why it
      // reads as the legs tweaking out going uphill specifically.
      //
      // Clamping t to exactly 1 for the final sample makes the arc evaluate at
      // sin(pi) = 0, so the foot is already ON the target when the step ends
      // and the handover is continuous by construction.
      f.swingT += dt / dur;
      if (f.swingT >= 1.0f) {
        // Land ON the target: at t = 1 the arc's lift term is sin(pi) = 0, so
        // assigning swingTo here is now continuous with the previous sample
        // rather than a drop from wherever the arc happened to be.
        f.swingT = 0;
        f.swinging = false;
        f.planted = f.swingTo;
        // THE FOOTFALL. The gait's own touchdown moment, so a step is heard
        // exactly when the art shows the foot land — no separate accumulator
        // to drift out of sync with the animation, and a severed leg stops
        // producing steps for free because it never swings again.
        Footfall ff;
        ff.posVox = f.swingTo;
        ff.mat = f.swingMat;
        ff.speed = speedNow_;
        ff.foot = (int)c;
        if (ff.mat != 0) footfalls_.push_back(ff);
        // ...and the stride clock's only input. See SyncStrideClock.
        SyncStrideClock((int)c);
      } else {
        // RE-TARGET THE SWING WHILE IT IS IN THE AIR. Freezing swingTo at
        // lift-off is what actually starved the gait: the body travels
        // speed * stepDuration during a swing (about 6 voxels at walk pace)
        // while the foot only gains the lead (about 5), so every single step
        // lost ground and the feet fell permanently behind the body no matter
        // how the cadence was tuned. Easing the target toward the CURRENT goal
        // as the swing progresses lets the foot land where the body actually
        // is. Weighted by t so the early swing keeps its committed direction
        // (no jitter at lift-off) and the late swing homes in on the truth.
        f.swingTo = f.swingTo * (1.0f - f.swingT) + goal * f.swingT;
        // Parabolic arc between lift-off and touch-down.
        //
        // The lift is EASED OUT over the last part of the swing rather than
        // being left to the parabola alone. sin(t*pi) is only zero at exactly
        // t = 1, and the swing almost never samples exactly 1 — it crosses it
        // mid-tick — so the last airborne sample sits a fraction of a voxel up
        // and touchdown drops the foot that distance in one frame. Through the
        // two-bone IK that measured as a 45+ degree leg snap per step. Scaling
        // the lift down to nothing over the final quarter of the swing makes
        // the foot arrive already flat on the target whatever t lands on.
        float t = f.swingT;
        Vec3 flat = f.swingFrom * (1.0f - t) + f.swingTo * t;
        float lift = std::sin(t * 3.14159265f) * g.stepHeight * f.legLength;
        const float kLandEase = 0.75f;   // lift is fully shed by t = 1
        if (t > kLandEase)
          lift *= std::max(0.0f, (1.0f - t) / (1.0f - kLandEase));
        flat.y += lift;
        f.planted = flat;
      }
    } else {
      // THE NEEDIEST FOOT STEPS, NOT THE FIRST ONE IN THE LIST. Only one leg
      // may swing at a time, and claiming that slot inline meant chain 0 took
      // it every single time: by the frame its own swing ended, its drift was
      // already past the threshold again, and since the loop visits it first it
      // re-claimed the slot before chain 1 was ever considered. The right leg
      // stayed planted at its spawn position forever while the left pogo'd —
      // one leg raking out behind the body, which is most of what "naruto run"
      // looked like. Record the best candidate here and commit after the loop.
      float drift = Vec3{goal.x - f.planted.x, 0, goal.z - f.planted.z}.len();
      if (swingingGroup < 0 && drift > g.stepThreshold * f.legLength &&
          speedFactor > 0.05f && drift > bestDrift) {
        bestDrift = drift;
        bestFoot = (int)c;
        bestGoal = goal;
        bestMat = gmat;  // surface this step is aimed at, for the footfall
      }
    }
    // ---- this leg's share of the stance crouch (see the note at the top) ----
    // How far the hip must come down to reach THIS foot at THIS excursion: the
    // horizontal separation is spent out of the leg's reach, and what is left
    // over is the vertical the hip may keep. Whichever leg wants the lowest hip
    // wins, since one pelvis has to satisfy them all.
    //
    // ONLY THE HORIZONTAL COMES FROM THE FOOT; THE VERTICAL IS THE RIG'S OWN
    // `span`. Using the foot's world height instead is the obvious version and
    // it is wrong twice. It makes the crouch a function of the gap between the
    // body's sole and the surface the probe found — so anything that floats or
    // sinks the body relative to its footing (the mob gate's walk fixture does
    // exactly this, by two voxels) is answered by burying the pelvis instead of
    // by the IK clamping one leg, measured: crouch pinned at the 2.04 clamp for
    // the whole walk with the knees at their REST angle, which is the pose this
    // change exists to remove wearing a different mask. And it would put
    // bodyY_ downstream of a height the probe supplies, which is the feedback
    // path the body-height note at the end of this function is about.
    //
    // The horizontal is safe on both counts: `planted` descends from origin_.x/z
    // and the velocity lead, so no term of it is a function of bodyY_.
    //
    // A SWINGING foot still contributes — its excursion is real and its leg has
    // to cover it — but its lift is ignored for the same reason, which costs
    // nothing here because the arc is a fraction of a voxel.
    {
      const float reach = f.legLength * kStanceReachFrac;
      const float dx = hipXZ.x - f.planted.x;
      const float dz = hipXZ.z - f.planted.z;
      const float vert =
          std::sqrt(std::max(reach * reach - (dx * dx + dz * dz), 0.0f));
      crouchNeed = std::max(crouchNeed, (restHipY_ - restSoleY_) - vert);
      crouchLegLength = std::max(crouchLegLength, f.legLength);
    }

    // Collected for the SLOPE only (see the body-height note below): the
    // lowest and highest planted feet give the tilt of the ground under the
    // character, which the player's axis-aligned AABB cannot express.
    sumY += f.planted.y;
    nFeet++;
    if (nFeet == 1 || f.planted.y < footLo) footLo = f.planted.y;
    if (nFeet == 1 || f.planted.y > footHi) footHi = f.planted.y;
  }

  // Commit the single step chosen above. Deferring it out of the loop is what
  // makes the choice order-independent: every eligible foot has been measured
  // by the time the slot is awarded, so the leg that has fallen furthest behind
  // gets it and the legs alternate on their own. No per-gait table, no explicit
  // left/right state — the same "one group at a time" rule as before, only now
  // it is a fair comparison rather than a race won by list position.
  if (bestFoot >= 0 && (size_t)bestFoot < anim_.feet.size()) {
    FootState& bf = anim_.feet[bestFoot];
    bf.swinging = true;
    bf.swingT = 0;
    bf.swingFrom = bf.planted;
    bf.swingTo = bestGoal;
    bf.swingMat = bestMat;
  }

  // ---- commit the crouch ---------------------------------------------------
  // Clamped because `legLength` and the foot targets are both authored data in
  // the end: a rig asking for a stride longer than its own leg would otherwise
  // drive the pelvis into the floor rather than simply failing to reach.
  crouchNeed = std::clamp(crouchNeed, 0.0f,
                          kMaxCrouchLegLengths * crouchLegLength);
  // Eased, not assigned — but on a SHORT half-life, which the per-foot form
  // above is what makes safe. The old speed-derived crouch was a step function
  // of speedFactor and needed heavy smoothing to keep a change of pace from
  // putting a vertical jolt through the whole rig; this one is continuous by
  // construction (the feet move continuously, and the landing ease makes even
  // touchdown continuous), so the filter only has to take the corners off. It
  // must stay short: the demand now oscillates ONCE PER STEP, and a half-life
  // near the step period would low-pass exactly the rise-and-fall this change
  // exists to produce — averaging it back into the constant crouch it replaced.
  stanceCrouch_ += (crouchNeed - stanceCrouch_) *
                   (1.0f - std::pow(0.5f, dt / kCrouchHalflife));

  // BODY HEIGHT COMES FROM THE PLAYER, NOT FROM THE FEET.
  //
  // This is the one place the avatar must NOT copy MobSystem. A mob derives
  // bodyY from its own foot plane (`footAvg + rideHeight*legLength`) because
  // nothing else knows how tall it is standing. The avatar does: the player's
  // AABB already resolved against the terrain this frame, so origin_.y IS the
  // sole of the boot and re-deriving it is both redundant and unstable.
  //
  // Unstable how: the foot goal falls back to `goal.y = bodyY_` whenever the
  // ground probe misses (unloaded chunk, mid-air, a foot over a ledge). Feed
  // that back through `footAvg + rideHeight*legLength` and bodyY_ becomes its
  // own input — it climbs by ~9.5 voxels a tick with nothing to stop it, which
  // is exactly the "wizard is 100 feet above the player" report. A mob never
  // hits this because its feet are pinned to real ground it is standing on.
  //
  // So: the player owns the height, and the feet only supply the SLOPE (the
  // foot-plane tilt below), which is the part the player AABB genuinely does
  // not know.
  //
  // The CROUCH is subtracted here and nowhere else. It is a function of the rig
  // and of the player's own speed (see the stance note at the top of this
  // function), so it stays outside the feedback loop this paragraph is about —
  // and because the leg IK targets are WORLD points, lowering the pelvis pushes
  // the bend into the knees and leaves the feet exactly where the gait put them,
  // which is what a crouch is.
  bodyY_ = origin_.y - stanceCrouch_;

  // The body stays upright. A real foot-plane tilt needs the horizontal
  // separation between the feet as well as their height difference (the lean
  // is rise/run about the axis BETWEEN them), and deriving that from two
  // planted points is a slope feature in its own right — not something to
  // fake here with a placeholder that always evaluates to straight up.
  // footLo/footHi are collected above and left for that work.
  (void)footLo;
  (void)footHi;
  bodyUp_ = (bodyUp_ * 0.85f + Vec3{0, 1, 0} * 0.15f).normalized();
  if (bodyUp_.len() < 0.5f) bodyUp_ = {0, 1, 0};
  footInit_ = true;
}

// THE LEGS MUST NOT BE IK-DRIVEN IN MID-AIR.
//
// `f.planted` is a WORLD-SPACE point. On the ground that is exactly right: the
// foot stays put while the body moves over it, which is what a planted foot is.
// In the air it is a trap. The ground probe scans DOWNWARD only (24 voxels from
// the sole), so in a fall it misses every tick and the goal degrades to the
// sole's current height — but nothing ever re-plants the foot, so `planted`
// stays at the world position where the ground USED to be while the body
// plummets away from it. Within a few ticks that stale point is ABOVE the hip,
// and AnimSolveTwoBone does exactly what it is asked: it aims the leg up at the
// target. The legs fold up through the pelvis and end up inverted inside the
// torso and head — the "legs invert entirely and fall upside down inside the
// model" report. It is not floppiness or a weak constraint; it is the IK
// faithfully solving for a target that should not exist.
//
// There is no sensible foot target in the air, so we do not invent one: the
// legs simply relax to the rest hang the flatten pass already produced, and the
// foot states are parked so the first grounded tick re-plants from scratch
// rather than resuming a swing that began before the jump.
// ---- the stride clock ------------------------------------------------------
//
// TWO CLOCKS IS THE BUG. The feet step on a DRIFT THRESHOLD — a foot unplants
// once the body has walked far enough past it — while the pelvis bob, sway and
// roll ran off `gaitPhase += dt * cadence * speedFactor`, a free-running
// oscillator that knows nothing about any of that. On this rig at walk pace the
// runtime's own step model gives 3.09 footfalls/s (a 1.54 Hz stride), against
// cadence 8 x speedFactor 0.508 = 4.06 Hz of sway and bobFreqMul 2 -> 8.13 Hz
// of bob. The bob is 2.6x the real footfall rate, and at a 30 Hz tick 8.13 Hz is
// under four samples a cycle — which is precisely "the body sways left and right
// really fast and it's jittery". It cannot be tuned out, because the two clocks
// disagree by a ratio that itself moves with speed.
//
// So there is one clock now and the FEET own it. The phase still advances
// smoothly (the bob must not step), but its RATE is the measured stride rate and
// its PHASE is pulled onto a half-turn boundary at every touchdown. bobFreqMul 2
// then means "once per footfall" and swayAmp "once per stride" by construction,
// which is what those names have always claimed.
//
// A partial correction rather than a snap: once locked the residual is a few
// milliseconds and invisible, whereas snapping would put a step into the bob
// twice per stride — trading one visible artifact for another.
void PlayerAvatar::SyncStrideClock(int chain) {
  const AnimSkeleton& sk = skel_;
  // Which leg is this, among the leg chains? Ordinal, not chain index: a rig
  // whose arm chains are interleaved with its legs must still split the stride
  // evenly between the legs that actually step.
  int legOrdinal = 0, nLegs = 0, mine = 0;
  for (size_t c = 0; c < sk.chains.size(); c++) {
    if (sk.chains[c].tag != "leg") continue;
    if ((int)c == chain) mine = legOrdinal;
    legOrdinal++;
    nLegs++;
  }
  if (nLegs <= 0) return;

  const float elapsed = sinceTouchdown_;
  sinceTouchdown_ = 0.0f;
  // A first touchdown, or one after a stop/jump, has no period to measure —
  // adopt the boundary outright and wait for the next step to time the rate.
  const bool haveStep = lastFootDown_ >= 0 && elapsed > 1e-3f &&
                        elapsed < kMaxStepPeriod;
  lastFootDown_ = chain;
  if (haveStep) {
    const float k = 1.0f - std::pow(0.5f, elapsed / kStepPeriodHalflife);
    stepPeriod_ += (elapsed - stepPeriod_) * k;
    // A stride is nLegs steps (two, for a biped): every leg lands once.
    const float stride = stepPeriod_ * (float)nLegs;
    if (stride > 1e-3f) strideRate_ = 1.0f / stride;
  } else {
    stepPeriod_ = 0.0f;
  }

  // Pull the phase onto this leg's boundary. Error taken the short way round so
  // a clock running a hair fast is nudged back rather than dragged forward
  // through a whole cycle.
  const float want = (float)mine / (float)nLegs;
  float err = want - anim_.gaitPhase;
  err -= std::floor(err + 0.5f);
  anim_.gaitPhase += err * (haveStep ? kStrideSyncGain : 1.0f);
  anim_.gaitPhase -= std::floor(anim_.gaitPhase);
}

void PlayerAvatar::UpdateAirPose(float dt) {
  // The crouch is a WALKING pose; ease it out while there is no ground under
  // the feet, so landing does not have to unwind a stance the air never used.
  stanceCrouch_ *= std::pow(0.5f, dt / 0.12f);
  for (FootState& f : anim_.feet) {
    f.swinging = false;
    f.swingT = 0;
  }
  // Force a fresh plant on landing. Without this the first grounded tick would
  // measure drift against a plant left over from before take-off — anywhere in
  // the world — and immediately fire a bogus step (and a bogus footstep sound).
  footInit_ = false;
  // A landing must re-time the stride from scratch: the elapsed time across a
  // jump is not a step period, and adopting it would slew the clock to a rate
  // the feet never ran at.
  lastFootDown_ = -1;
  sinceTouchdown_ = 0.0f;
  bodyY_ = origin_.y - stanceCrouch_;
  bodyUp_ = (bodyUp_ * 0.85f + Vec3{0, 1, 0} * 0.15f).normalized();
  if (bodyUp_.len() < 0.5f) bodyUp_ = {0, 1, 0};
}

void PlayerAvatar::UpdateAnimation(float dt, World& world, bool grounded,
                                   const Vec3& playerVel) {
  const AnimSkeleton& sk = skel_;
  AnimState& st = anim_;
  if (sk.parts.empty()) return;
  st.partAlive.resize(sk.parts.size(), 1);
  st.springs.resize(sk.parts.size(), SpringState{});

  // THE VELOCITY COMES FROM THE PLAYER, NOT FROM DIFFERENCING OUR OWN ORIGIN.
  //
  // This is the jitter. A mob differences its position because it moves itself,
  // one step per tick, so the delta is always exactly one tick of travel. The
  // avatar does not: Player::Update runs once per FRAME at real dt, while this
  // runs 0..4 times per frame inside the fixed-tick loop. So the delta over
  // `kTickDt` is measuring the wrong interval every single frame —
  //
  //   * two ticks in one frame: the first sees the whole frame's travel (too
  //     fast), the SECOND sees zero, because the player has not moved since.
  //   * zero ticks: the measurement is simply stale.
  //   * one tick: too fast or too slow depending on how dt compares to kTickDt.
  //
  // The result is a speed that slams between roughly double the truth and zero
  // at frame rate. And `speedNow_` is not some minor readout — it drives the
  // gait cadence, the bob/sway/roll amplitudes, the walk/run clip selection, the
  // spring goals and the swing-duration budget. Every one of those oscillates
  // together, which is exactly the "character spazzes, hands move like crazy"
  // report. It is not a tuning problem and no amount of extra filtering downstream
  // would have fixed it, because the signal itself was garbage.
  //
  // The player already knows its velocity exactly. Use it.
  st.lastPos = origin_;  // still tracked: other code reads it as "where we were"
  Vec3 planar{playerVel.x, 0, playerVel.z};
  // Frame-rate independent smoothing, on a half-life rather than a bare
  // per-call lerp. The old `0.7/0.3` blend was per CALL, so its time constant
  // scaled with how many ticks happened to fire — the same class of bug as the
  // measurement above, and it would have made the gait feel different at 30 and
  // 144 fps even with a clean velocity.
  const float hl = CurrentTuning().avatar.velocityHalflife;
  float k = hl > 1e-4f ? 1.0f - std::pow(0.5f, dt / hl) : 1.0f;
  st.velocity = st.velocity + (planar - st.velocity) * k;
  speedNow_ = Vec3{st.velocity.x, 0, st.velocity.z}.len();
  float speedFactor =
      std::clamp(speedNow_ / std::max(def_->speed, 0.01f), 0.0f, 1.5f);

  const GaitDef& g = sk.gait;
  // ONE CLOCK, AND THE FEET OWN IT — see SyncStrideClock for why the old
  // `cadence * speedFactor` oscillator ran at 2.6x the real footfall rate and
  // read on screen as a fast, jittery sway. The rate is measured between
  // touchdowns; the phase is corrected at each one. Until the first two steps
  // have been timed `strideRate_` is zero and the pelvis simply holds still,
  // which is the honest answer for a body that has not taken a stride yet.
  //
  // The rig with NO leg chains (dummy.json) keeps the oscillator: it never
  // plants a foot, so there is nothing to lock to. MobSystem::UpdateAnimation
  // is the other holder of that fallback and states it the same way.
  const bool haveLegs = [&] {
    for (const IkChain& ch : sk.chains)
      if (ch.tag == "leg") return true;
    return false;
  }();
  if (haveLegs) {
    // Park the clock when the feet stop reporting: a body standing still takes
    // no steps, and a rate left running would keep the (speed-scaled, so
    // invisible) bob accumulating phase to land on an arbitrary value the
    // moment it moves again.
    if (sinceTouchdown_ > kMaxStepPeriod)
      strideRate_ *= std::pow(0.5f, dt / 0.15f);
    st.gaitPhase += dt * strideRate_;
  } else {
    st.gaitPhase += dt * (g.present ? g.cadence : 2.2f) * speedFactor;
  }
  st.gaitPhase -= std::floor(st.gaitPhase);

  // ---- dismemberment locomotion state ----
  // Polled every frame rather than only on Sever: partAlive changes in several
  // places (Sever, recursive DetachPart, Die), and this is a handful of
  // comparisons. On a transition the outgoing clip blends out while the new
  // one blends in over its own blendInMs — a crossfade for free.
  {
    int want = AnimSelectState(sk, st);
    if (want != st.locoState) {
      if (st.locoState >= 0 && !sk.states[st.locoState].clip.empty()) {
        int old = sk.FindClip(sk.states[st.locoState].clip);
        for (ClipInstance& inst : st.clips)
          if (inst.clip == old) inst.stopping = true;
      }
      st.locoState = want;
      if (want >= 0) {
        const AnimStateRule& rule = sk.states[want];
        if (!rule.clip.empty()) PlayClip(rule.clip);
        // A foot frozen mid-swing would report "swinging" forever once the
        // gait stops running; land everything where it stands.
        if (rule.disableGait)
          for (FootState& f : st.feet) f.swinging = false;
      }
    }
  }
  const AnimStateRule* loco =
      st.locoState >= 0 ? &sk.states[st.locoState] : nullptr;
  const bool clipOwnsPose = loco && loco->disableGait;

  AnimSampleAndBlend(sk, st, dt);

  // pelvis bob / sway / spine counter-rotation, suppressed while an authored
  // clip owns the pose (a crawl keys the same limbs_ these drive)
  if (g.present && !clipOwnsPose && def_->rootLimb >= 0 &&
      def_->rootLimb < (int)sk.parts.size()) {
    Transform& root = st.local[def_->rootLimb];
    root.pos.y += g.bobAmp * std::sin(g.bobFreqMul * 6.2831853f * st.gaitPhase) *
                  speedFactor;
    root.pos.x += g.swayAmp * std::sin(6.2831853f * st.gaitPhase) * speedFactor;
    float roll = g.rollAmp * std::sin(6.2831853f * st.gaitPhase) * speedFactor;
    root.rot = QuatNormalize(Mul(root.rot, AxisAngle({0, 0, 1}, roll)));
    for (size_t i = 0; i < sk.parts.size(); i++) {
      if (sk.parts[i].tag != "spine") continue;
      st.local[i].rot = QuatNormalize(
          Mul(st.local[i].rot, AxisAngle({0, 1, 0}, -g.spineCounter * roll)));
    }
  }

  // springs: a part is KEYED or JIGGLED, never both
  //
  // The goal is driven by velocity NORMALIZED against the def's own top speed,
  // not by raw voxels/second. MobSystem's `velocity * gain * 0.05` is tuned for
  // mob speeds; the player moves ~10x faster (60 world voxels/s at a sprint),
  // which drives the head spring to ~2.7 rad and leaves it PINNED at its
  // maxAngle clamp for the whole of every move — the head permanently reared
  // back, snapping between extremes on every turn. Dividing by def_->speed
  // makes `gain` mean "deflection at full speed" for any character, so a
  // spring authored on a mob reads the same on the avatar.
  const float speedRef = std::max(def_->speed, 0.01f);
  for (size_t i = 0; i < sk.parts.size(); i++) {
    const AnimPart& p = sk.parts[i];
    if (!p.hasSpring) continue;
    Vec3 goal{-st.velocity.z / speedRef * p.spring.gain * kSpringVelScale, 0,
              st.velocity.x / speedRef * p.spring.gain * kSpringVelScale};
    goal.x = std::clamp(goal.x, -p.spring.maxAngle, p.spring.maxAngle);
    goal.z = std::clamp(goal.z, -p.spring.maxAngle, p.spring.maxAngle);
    AnimSpringStep(p.spring, st.springs[i], goal, dt);
    const Vec3& s = st.springs[i].x;
    Quat jiggle = Mul(AxisAngle({1, 0, 0}, s.x),
                      Mul(AxisAngle({0, 1, 0}, s.y), AxisAngle({0, 0, 1}, s.z)));
    st.local[i].rot = QuatNormalize(Mul(st.local[i].rot, jiggle));
  }

  // ---- head look ------------------------------------------------------------
  // THE HEAD LEADS, THE BODY FOLLOWS. main.cpp holds the body's facing still
  // while the camera stays inside the neck's cone (avatar.headLookYaw) and only
  // drags the feet around once the view leaves it; this is the other half of
  // that — the head actually pointing where the camera is looking, so a glance
  // to the side reads as a glance rather than as the whole character strafing
  // with a fixed stare.
  //
  // WHY IT IS APPLIED HERE, BEFORE THE FLATTEN, and not as a post-process the
  // way the IK is: `local[i].rot` is a joint rotation and the head has
  // children (hat, hair — and on another rig, anything socketed to it), so
  // composing it at the joint is what carries them along. AnimFlatten runs
  // one line below and does exactly that. The IK is a post-process for the
  // opposite reason: it needs the flattened pose to solve against.
  //
  // The pivot is free: rest.pos is the parent-relative anchor delta (mob.cpp
  // builds it that way), so a part's local origin already IS its joint, and
  // rotating `rot` alone swings the head about the neck rather than about the
  // model origin. The springs directly above rely on the same property.
  //
  // THE YAW SIGN COMES FROM THE BODY, NOT FROM THE MODEL AXES. This is the
  // trap, and the first version got it backwards: `lookYaw_` is a HEADING
  // delta (camHeading - avatarHeading, both in the rig's heading convention),
  // not a camera yaw. The body applies its own heading as
  // AxisAngle({0,1,0}, heading_) — so a POSITIVE heading delta must be applied
  // to the neck the same positive way, or the head turns opposite the camera.
  //
  // Reasoning about it via "model +X is the character's left" is what produced
  // the inverted version: that fact is true, and it is why the weapon arm
  // negates X, but it does not apply here. The arm converts a WORLD-space
  // offset into the model frame and so meets the handedness flip head-on; this
  // composes an angle that is already expressed in the same convention the
  // body's own yaw uses, so it needs no flip at all.
  //
  // Pitch is the one that does get negated: camera pitch is positive UP, and a
  // positive rotation about model +X pitches the nose DOWN (verified with
  // scripts/geometry.py: +90 about X takes +Z to -Y).
  {
    const auto& av = CurrentTuning().avatar;
    const float hl = av.headLookHalflife;
    const float k = hl > 1e-4f ? 1.0f - std::pow(0.5f, dt / hl) : 1.0f;
    lookYaw_ += (lookYawGoal_ - lookYaw_) * k;
    lookPitch_ += (lookPitchGoal_ - lookPitch_) * k;

    const int head = parts_.head;
    // A severed head does not look at anything. `partAlive` is the same gate
    // the rest of the pose pipeline uses, and skipping it here also keeps a
    // detached head's debris pose from being written every tick.
    const bool haveHead = head >= 0 && head < (int)sk.parts.size() &&
                          head < (int)st.local.size() &&
                          (head >= (int)st.partAlive.size() || st.partAlive[head]);
    if (haveHead && (std::fabs(lookYaw_) > 1e-4f ||
                     std::fabs(lookPitch_) > 1e-4f)) {
      // Spine carries a share of the yaw so the chest twists into the look
      // instead of a head swivelling on a rigid torso. The head then only
      // needs the REMAINDER — it inherits the spine's share through the
      // flatten, so adding the full yaw at both joints would double it.
      //
      // THE ROOT LIMB IS NOT PART OF THE SPINE FOR THIS PURPOSE, even though
      // it carries the "spine" tag. On mina the tag is on BOTH `hips` and
      // `torso`, and hips is rootLimb — so rotating it yaws the entire rig,
      // legs and all, rather than twisting the chest. That is not a subtle
      // wrongness: the head turns with the body it is measured against, which
      // reads on screen as the head not turning at all, which is exactly the
      // bug this excludes. The tag means "part of the back" to the gait's
      // counter-rotation, which wants the root; a look twist wants only the
      // joints ABOVE it.
      const float spineShare = av.headLookSpine;
      float spineTotal = 0;
      if (spineShare > 1e-4f) {
        int nSpine = 0;
        for (size_t i = 0; i < sk.parts.size(); i++)
          if (sk.parts[i].tag == "spine" && (int)i != def_->rootLimb) nSpine++;
        if (nSpine > 0) {
          // Split across however many spine joints the rig has, so a rig with
          // a three-segment back twists the same TOTAL amount as one with a
          // single torso rather than three times as far.
          const float per = lookYaw_ * spineShare / (float)nSpine;
          for (size_t i = 0; i < sk.parts.size(); i++) {
            if (sk.parts[i].tag != "spine" || (int)i == def_->rootLimb) continue;
            if (i < st.partAlive.size() && !st.partAlive[i]) continue;
            st.local[i].rot = QuatNormalize(
                Mul(st.local[i].rot, AxisAngle({0, 1, 0}, per)));
            spineTotal += per;
          }
        }
      }
      const float headYaw = lookYaw_ - spineTotal;
      Quat look = Mul(AxisAngle({0, 1, 0}, headYaw),
                      AxisAngle({1, 0, 0}, -lookPitch_));
      st.local[head].rot = QuatNormalize(Mul(st.local[head].rot, look));
    }
  }

  AnimFlatten(sk, st);

  // `grounded` is part of the gate, not just an input to it: a gait with no
  // floor under it has no meaningful foot target (see UpdateAirPose).
  const bool gaitActive = g.present && !clipOwnsPose && grounded;

  // THE IK FADES IN AND OUT; IT DOES NOT SWITCH.
  //
  // This is the uphill tweaking. `grounded` is genuinely ragged when you cross
  // a bumpy incline — the body really does leave the surface for a fraction of
  // a voxel as it crests each bump, which is why the player controller has
  // hysteresis and coyote time for exactly this. The gait inherits that
  // raggedness, and until now it turned it into a HARD switch: IK fully on one
  // tick, fully off the next, with the limbs snapping between the IK-solved
  // pose and the rest hang that UpdateAirPose leaves behind. That snap-to-rest
  // is the "arms shoot up straight" frame, and on a bumpy ascent it fires over
  // and over.
  //
  // Clips already crossfade (ClipFade/blendInMs) and the dismemberment states
  // only change on a sever, so neither of those could be the discontinuity —
  // this gate was the only thing in the pose pipeline still teleporting.
  //
  // AnimSolveTwoBone already takes a WEIGHT and blends its result against the
  // incoming pose, so the fix is to drive that weight continuously rather than
  // to gate the block on a bool. Note this is NOT the thing the comment below
  // warns about: that warns against blending two IK RESULTS together (a pose
  // satisfying neither foot), whereas this fades a single solve against the
  // flattened animation pose, which is what the weight parameter is for.
  {
    const float hl = CurrentTuning().avatar.ikBlendHalflife;
    float want = gaitActive ? 1.0f : 0.0f;
    float k = hl > 1e-4f ? 1.0f - std::pow(0.5f, dt / hl) : 1.0f;
    gaitWeight_ += (want - gaitWeight_) * k;
    if (gaitWeight_ < 1e-3f) gaitWeight_ = 0.0f;
    if (gaitWeight_ > 0.999f) gaitWeight_ = 1.0f;
  }

  if (!grounded && !clipOwnsPose) {
    UpdateAirPose(dt);
  } else if (gaitActive) {
    UpdateGait(dt, world);
  } else {
    // An authored clip owns the pose (crawl, etc.): it keys the same pelvis the
    // crouch moves, so unwind the crouch rather than composing the two.
    stanceCrouch_ *= std::pow(0.5f, dt / 0.12f);
    float targetY = origin_.y + (loco ? loco->bodyYOffset : 0.0f);
    bodyY_ += std::clamp(targetY - bodyY_, -0.4f, 0.4f);
    footInit_ = true;
    bodyUp_ = (bodyUp_ * 0.85f + Vec3{0, 1, 0} * 0.15f).normalized();
    if (bodyUp_.len() < 0.5f) bodyUp_ = {0, 1, 0};
  }

  // IK is a POST-PROCESS on the flattened pose, never a blended layer:
  // blending two IK results gives a pose that satisfies neither end-effector.
  // (Fading ONE solve's weight against the animation pose, which is what
  // gaitWeight_ does, is a different thing — see the note above.)
  //
  // Runs whenever the weight is non-zero, not only while gaitActive: that is
  // the whole point of the fade, since the ticks that need blending are exactly
  // the ones just after the gait switched off. `f.planted` is still the last
  // real foot target during those ticks — UpdateAirPose parks the SWING but
  // deliberately leaves `planted` alone — so the legs ease out of their last
  // stance instead of snapping to the rest hang.
  if (!sk.chains.empty() && gaitWeight_ > 0.0f) {
    Quat yaw = AxisAngle({0, 1, 0}, heading_);
    Vec3 pivot{def_->worldSize.x * 0.5f, 0, def_->worldSize.z * 0.5f};
    Vec3 bodyOrigin{origin_.x, bodyY_, origin_.z};
    for (size_t c = 0; c < sk.chains.size() && c < st.feet.size(); c++) {
      if (sk.chains[c].tag != "leg") continue;
      const FootState& f = st.feet[c];
      float weight = f.valid ? sk.chains[c].weight * gaitWeight_ : 0.0f;
      if (weight <= 0) continue;
      Vec3 rel = f.planted - bodyOrigin - pivot;
      // A STALE PLANT MUST NOT BE REACHED FOR. `planted` is a WORLD point, and
      // in a real fall the body drops away from it until it sits above the hip
      // — at which point the solver aims the legs UP and folds them through the
      // pelvis (the leg-inversion failure UpdateAirPose exists to prevent).
      // The fade is short enough that a fall leaves the weight at zero within a
      // few ticks, but "short enough" is a timing argument and this is a
      // geometry problem, so state it as geometry: once the target is further
      // than the leg can reach, there is nothing sensible to solve for and the
      // remaining fade is dropped rather than pointed at a ghost.
      if (rel.len() > f.legLength * 1.6f) continue;
      Vec3 prefabPt = RotateInv(yaw, rel) + pivot;
      // THE TARGET IS PREFAB-ABSOLUTE, AND SO IS THE HIP IT IS SOLVED AGAINST.
      // Nothing is rebased here. AnimFlatten seeds the root with its own
      // rest.pos, which IS rootAnchor, so every st.model[].pos already carries
      // the root offset — including st.model[hip].pos, which AnimSolveTwoBone
      // reads as its chain root. Subtracting rootAnchor from the target while
      // the hip keeps it moved the two ends into different frames and the
      // solver's (target - root) came out wrong by exactly rootAnchor: on this
      // rig a spurious (-2, -11, -0.6) that swamped the +-0.6 hip split, so
      // BOTH legs chased one identical point 11.2 voxels away against a 5.55
      // reach. The solver clamped to its annulus every frame, which pinned a
      // fixed ~10 degree splay toward model -X (the character's left, since
      // these rigs author .L at the higher engine x) and left the stride only
      // able to rotate an already-straight leg — halving the visible swing.
      // Same convention as the submit path below, which likewise does not
      // re-add rootAnchor to modelPos, and for exactly the same reason.
      AnimSolveTwoBone(sk, st, sk.chains[c], prefabPt, weight);
    }
  }

  // ---- stage 5.5: the weapon arm (game/melee.h) ---------------------------
  // The swing is driven by aiming the WEAPON ARM's IK chain at a point derived
  // from the stroke driver. Doing it through the existing chain rather than as
  // a bespoke clip is what makes it compose with everything else: the legs keep
  // walking, the gait keeps running, dismemberment still disables the chain (a
  // severed arm simply stops solving and the sword falls), and a mob swings the
  // same way through the same call.
  //
  // THE IMPLEMENTATION IS ON Mob, NOT HERE. It used to be inline in this pass,
  // which meant the avatar could swing and an NPC could not — and Phase C's
  // attacking mobs would have had to grow a second copy that drifted. The long
  // notes about the mirrored-authoring x flip, about what the pole does, and
  // about why nothing may rotate the held part after the flatten now live next
  // to the code they describe, in Mob::ApplyWeaponArm.
  //
  // POST-PROCESS, LIKE THE LEGS: applied to the flattened pose, never blended
  // as a layer. `weaponWeight_` fades the solve itself.
  PoseAxisOverride weaponHinge;
  ApplyWeaponArm(sk, st, weaponHinge);

  // ---- ledge-hang arms: pin the PALMS to the held lip ----------------------
  //
  // The hang clip gets the arms into the neighbourhood; this solve makes the
  // contact EXACT, and it is what makes the pose work on any rig out of the
  // box: chain lengths come from the skeleton, the hand spread comes from each
  // chain's own shoulder anchor, and the contact point is the ITEM SOCKET —
  // the same "held_right" frame a sword hangs from — so "where does this rig's
  // palm sit inside its hand part" is answered by the rig, never by a per-def
  // pose constant.
  //
  // POST-PROCESS, LIKE THE LEGS AND THE WEAPON ARM, and faded through
  // hangIkWeight_ for the same reason gaitWeight_ exists: the ticks around a
  // grab and a release are exactly the ones that must blend, and a hard
  // switch snaps the arms between the solved reach and whatever pose the
  // clips left. Targets are WORLD points un-yawed the way the legs un-yaw
  // `planted` (no x negation — that flip is for CAMERA-space offsets, see the
  // weapon note above), so if the camera yaws the body inside its hold cone
  // the hands stay planted on the lip and the shoulders turn under them.
  {
    const float hl = CurrentTuning().avatar.ikBlendHalflife;
    const float want = hangActive_ ? 1.0f : 0.0f;
    const float k = hl > 1e-4f ? 1.0f - std::pow(0.5f, dt / hl) : 1.0f;
    hangIkWeight_ += (want - hangIkWeight_) * k;
    if (hangIkWeight_ < 1e-3f) hangIkWeight_ = 0.0f;
    if (hangIkWeight_ > 0.999f) hangIkWeight_ = 1.0f;
  }
  if (hangIkWeight_ > 0.0f && !sk.chains.empty()) {
    const Quat yaw = AxisAngle({0, 1, 0}, heading_);
    const Vec3 pivot{def_->worldSize.x * 0.5f, 0, def_->worldSize.z * 0.5f};
    const Vec3 bodyOrigin{origin_.x, bodyY_, origin_.z};
    // The body's centre column in world, and the wall geometry off the lip.
    const float cx = origin_.x + pivot.x, cz = origin_.z + pivot.z;
    const Vec3 dir = hangDirW_;
    // Model +X is the character's LEFT on these rigs (see the weapon note);
    // facing `dir`, that lateral maps to (dir.z, 0, -dir.x) in world.
    const Vec3 leftW{dir.z, 0.0f, -dir.x};
    const float lipTopY = (float)(hangLipW_.y + 1);
    // How far ahead the lip actually is, measured to the held cell rather
    // than assumed from the probe constant, so hands reach a recessed lip.
    // The hands aim at the lip's NEAR TOP CORNER (cell centre minus half a
    // cell, plus a finger's width onto the surface): every fraction of a
    // voxel of horizontal reach is a fraction the arms — short on a chibi
    // rig — do not have to spend.
    const float aheadRaw = ((float)hangLipW_.x + 0.5f - cx) * dir.x +
                           ((float)hangLipW_.z + 0.5f - cz) * dir.z;
    // The two bare terms were world voxels sitting either side of a
    // metres-derived kHalfXZ — a mixed-unit clamp, so at 5 cm the 5.5 cm
    // setback and the 12 cm overhang both halved while the capsule they bound
    // did not, and the palm stopped landing on the lip.
    const float ahead = std::clamp(aheadRaw - MetresToCells(0.055f),
                                   Player::kHalfXZ * 0.6f,
                                   Player::kHalfXZ + MetresToCells(0.12f));
    for (size_t c = 0; c < sk.chains.size(); c++) {
      const IkChain& ch = sk.chains[c];
      if (ch.tag != "arm" || ch.parts.empty() || ch.effector < 0) continue;
      const float weight = ch.weight * hangIkWeight_;
      if (weight <= 0) continue;
      const Vec3 shoulder = sk.parts[ch.parts[0]].anchorLocal;
      const float latM = shoulder.x - pivot.x;  // this arm's own spread
      // Palm rest point on the lip: fingers just over the top surface.
      Vec3 palmW{cx + leftW.x * latM + dir.x * ahead,
                 lipTopY + MetresToCells(0.02f),
                 cz + leftW.z * latM + dir.z * ahead};
      Vec3 rel = palmW - bodyOrigin - pivot;
      Vec3 palmPrefab = RotateInv(yaw, rel) + pivot;
      // The solver places the EFFECTOR's joint (the wrist). The grip point is
      // the hand's item socket — resolve its offset from the wrist and aim
      // the wrist short of the lip by exactly that.
      Vec3 sockLocal{};
      bool found = false, mirrored = false;
      for (const MobSocketDef& sock : def_->sockets) {
        if (sock.partIndex < 0) continue;
        if (sock.partIndex == ch.effector) {
          sockLocal = sock.offset - limbs_[sock.partIndex].restOffset;
          found = true;
          mirrored = false;
          break;
        }
        // No socket on THIS hand: borrow one from another ARM's hand and
        // mirror its lateral component. Palms are near-centred in a hand's
        // width, so the residual error is a fraction of a voxel — and a rig
        // that wants it exact just authors a socket on both hands. A socket
        // on anything that is not a hand (a back scabbard, say) must not be
        // borrowed, hence the effector check.
        if (found) continue;
        for (const IkChain& other : sk.chains) {
          if (other.tag == "arm" && other.effector == sock.partIndex) {
            sockLocal = sock.offset - limbs_[sock.partIndex].restOffset;
            found = true;
            mirrored = true;
            break;
          }
        }
      }
      if (mirrored) sockLocal.x = -sockLocal.x;
      if (ch.parts.size() < 2) continue;
      const int i0 = ch.parts[0], i1 = ch.parts[1], ie = ch.effector;
      if (ie >= (int)st.model.size() || i1 >= (int)st.model.size()) continue;
      if (!st.partAlive.empty() && (!st.partAlive[i0] || !st.partAlive[i1]))
        continue;  // limb lost: the chain has gone silent, so must this

      // Rough wrist target from the pre-solve hand rotation; refined by the
      // second pass below. (The flatten restarts from the clips every frame,
      // so a single pass never converges — the correction must be measured
      // from the SOLVED pose inside the same frame.)
      Vec3 palmOff = QuatRotate(st.model[ie].rot, sockLocal);
      Vec3 wristT = palmPrefab - palmOff;

      const Vec3 root = st.model[i0].pos;
      const float L1 = (st.model[i1].pos - root).len();
      const float L2 = ie == i1 ? sk.parts[i1].rest.pos.len()
                                : (st.model[ie].pos - st.model[i1].pos).len();
      const float armReach = (L1 + L2) * 0.98f;
      Vec3 toT = wristT - root;
      const float shortBy = toT.len() - armReach;
      // Ghost guard, same geometry rule as the legs — but only while FADING
      // OUT: a live hang is allowed to be short (the shrug covers it), a
      // stale lip receding as the body falls away is not worth aiming at.
      if (!hangActive_ && shortBy > 3.0f) continue;

      // ---- SHOULDER SHRUG: the stretch that makes short arms reach -------
      // Two-bone IK clamps to its annulus, so a rig whose arms cannot span
      // shoulder->lip (mina's are ~3.3 voxels against a ~5+ voxel gap —
      // chibi proportions) lands its hands exactly the shortfall below the
      // lip, which is the reported bug. Rather than fake it with a pose, the
      // whole chain TRANSLATES toward the target by the (capped) shortfall —
      // shoulders pulled up out of the socket by the body's weight, the way
      // a real dead hang stretches them. Bounded, weight-faded, and ZERO for
      // any rig whose arms genuinely reach: long-armed models never shrug.
      if (shortBy > 0.0f && toT.len() > 1e-4f) {
        // 25 cm of shoulder shrug. In metres because it is bounded against a
        // real arm's shortfall: a bare 2.5 cells would let the shoulder travel
        // half as far at 5 cm and the hang would stop reaching the ledge.
        const float kShrugCap = MetresToCells(0.25f);
        const Vec3 shift =
            toT * (std::min(shortBy, kShrugCap) / toT.len() * weight);
        st.model[i0].pos += shift;
        st.model[i1].pos += shift;
        if (ie != i1) st.model[ie].pos += shift;
      }

      // Pass 1 settles the chain onto the rough target; pass 2 re-measures
      // the palm from the solved hand rotation and corrects. The residual
      // after pass 2 is second-order (the correction barely re-rotates the
      // hand) — sub-voxel in practice.
      AnimSolveTwoBone(sk, st, ch, wristT, weight);
      palmOff = QuatRotate(st.model[ie].rot, sockLocal);
      AnimSolveTwoBone(sk, st, ch, palmPrefab - palmOff, weight);
    }
  }

  // ---- stage 6: the pose has to be anatomically possible ------------------
  //
  // LAST, after every solver on this path — the leg gait, the weapon arm and
  // the ledge hang all write st.model[] and any of them can push a joint past
  // its range. Clamping between them would let a later solve undo the clamp,
  // which is the same reason the IK itself is a post-process rather than a
  // layer.
  //
  // This is where "the legs rake out behind" and "the legs invert into the
  // torso" stop being things a gate has to notice after the fact and become
  // unrepresentable: the hip simply cannot reach those angles, whatever target
  // the solver was handed. Rigs that author no `poseLimit` pay one bool test.
  //
  // The weapon arm hands in ONE exception, and only while a stroke is live: the
  // elbow's hinge PLANE is steered into the plane of the cut. Its RANGE is not
  // touched, so the joint stays exactly as impossible to hyperextend as it was
  // — see anim.h PoseAxisOverride for why a steered plane is the anatomically
  // honest answer and not a loophole.
  //
  // FORGETTING THE OVERRIDE HERE IS SILENT AND EXPENSIVE. It was, for four
  // measured runs: the solve reached its target (`ikMiss 0.00` every tick) and
  // the clamp then discarded the whole off-plane component of the elbow, so the
  // arm arrived up to 2.9 voxels from where the stroke asked with every
  // in-pipeline probe reporting success. Mob::RecordWeaponClamp is what makes
  // that visible now, and it only runs if it is called — which is why it sits
  // on the same line.
  AnimClampPoseLimits(sk, st, &weaponHinge, 1);
  RecordWeaponClamp(sk, st);
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
  // severed holds down (the avatar-layer strip on release happens in the
  // OnBodyReleasedToWorld override).
  DrainPendingSpawns(world, spawns);
  TickSeveredHolds(dt);
  // ...and the hit flash, which is the same kind of thing: per-limb state that
  // MobSystem::PreTick ages for every NPC and that nothing else ages for the
  // avatar. Without this line a severed player limb leaves its stump lit at
  // combatfx.flashSever forever (the reported permanent glow).
  DecayHitFlash(dt);

  if (alive_) {
    // The body follows the PLAYER, which is the whole difference from a mob.
    origin_ = Vec3{player.pos.x - def.worldSize.x * 0.5f,
                   player.pos.y - Player::kHalfY,
                   player.pos.z - def.worldSize.z * 0.5f};
    heading_ = heading;

    // Mirror the hang state for the arm-IK block; UpdateAnimation itself
    // stays player-free by design.
    hangActive_ = player.hanging;
    if (player.hanging) {
      hangLipW_ = player.hangLip;
      hangDirW_ = player.hangDir;
    }
    // ---- air bookkeeping, BEFORE the pose ----
    // The debounce below used to live after UpdateAnimation and feed the CLIPS
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

    UpdateAnimation(dt, world, gaitGrounded, player.vel);

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
    // (and rather more of it) — see the note at the UpdateAnimation call.
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
        if (ff.mat != 0) footfalls_.push_back(ff);
      }
      airTime_ = 0;
    } else {
      // A JUMP IS A LAUNCH, NOT A LOSS OF CONTACT.
      //
      // This fired on the `wasGrounded_ -> airborne` edge, which any downward
      // step produces: walking at 16 voxels/s a one-voxel drop is airborne for
      // longer than the 0.12 s debounce, so ordinary broken ground retriggered
      // an arms-up one-shot over and over. `player.jumped` is set where the
      // jump impulse is actually applied and is sticky across the tick batch
      // (see Player::jumped), so it says what the edge could not.
      if (jumpLatched_ && !wasHanging_) PlayClip("jump");
      airTime_ += dt;
      // ...AND A FALL IS A DROP. Air time alone says nothing about height:
      // `supportY_` records where the body last had something under it, so the
      // clip waits for real distance to have been given up. A step-down never
      // qualifies no matter how long the debounce takes to clear.
      const float dropped = supportY_ - origin_.y;
      const float minDrop = CurrentTuning().avatar.fallMinDrop / kVoxelMeters;
      if (airTime_ > 0.45f && dropped > minDrop) PlayClip("fall");
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
      const int want = hangingNow && hc >= 0
                           ? hc
                           : airborneNow ? fc
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
    if (strideRate_ > 1e-4f) {
      for (ClipInstance& inst : anim_.clips) {
        if (inst.clip != locoClips_.walk && inst.clip != locoClips_.run)
          continue;
        if (inst.clip < 0 || inst.clip >= (int)skel_.clips.size()) continue;
        const float durS = (float)skel_.clips[inst.clip].durationMs * 0.001f;
        if (durS <= 1e-4f) continue;
        // rate 1 == the clip's authored period equals one stride.
        inst.rate = std::clamp(strideRate_ * durS, 0.25f, 3.0f);
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

// ---- damage -----------------------------------------------------------------

// ---- health, as the caster VM sees it (game/spell.h) ------------------------
//
// The player deliberately gains no hp field: health IS the per-part hp the
// dismemberment system already maintains, so the mana bar's overdraw and the
// visible damage state cannot drift apart.

int32_t PlayerAvatar::TotalHealth() const {
  if (!spawned_ || !alive_) return 0;
  float sum = 0;
  for (size_t i = 0; i < limbs_.size(); i++)
    if (PartAlive((int)i) && limbs_[i].hp > 0) sum += limbs_[i].hp;
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
    if (ld.hp > 0) sum += ld.hp;
  return sum <= 0 ? 0 : (int32_t)sum;
}

void PlayerAvatar::SpendHealth(int32_t amount) {
  if (!spawned_ || !alive_ || amount <= 0) return;
  // Spread the cost across live limbs_ in proportion to what each still has,
  // rather than draining the first one to zero — otherwise a mana overdraw
  // deterministically severs whichever limb happens to sort first, which reads
  // as a bug rather than as a cost.
  const int32_t total = TotalHealth();
  if (total <= 0) {
    Die();
    return;
  }
  if (amount >= total) {
    Die();
    return;
  }
  const float frac = (float)amount / (float)total;
  // Collect first: Sever() mutates `limbs_` (it detaches children too), so
  // deciding everything against the pre-carve state and acting afterwards is
  // what keeps this from walking a list that reshapes underneath it.
  std::vector<int> severed;
  for (size_t i = 0; i < limbs_.size(); i++) {
    if (!PartAlive((int)i) || limbs_[i].hp <= 0) continue;
    limbs_[i].hp -= limbs_[i].hp * frac;
    // Bleeding from the strain of the overcast, through the ordinary budget.
    if (def_)
      limbs_[i].bleedBudget = AddBleedBudget(limbs_[i].bleedBudget,
                                            6.0f * frac * def_->bleedPerDamage);
    if (limbs_[i].hp <= 0.0f) severed.push_back((int)i);
  }
  for (int i : severed)
    if (PartAlive(i)) Sever(i);
}

void PlayerAvatar::SelfDestruct(Vec3 atWorldVoxel, float radiusVox,
                                World& world,
                                std::vector<ParticleSpawn>& spawns) {
  (void)world;
  (void)spawns;
  if (!spawned_ || !def_) return;
  // Take off every severable part whose body is inside the blast, then die.
  // Severing rather than carving is a deliberate simplification for this
  // slice: the avatar has no equivalent of MobSystem's private CarveLimb, and
  // duplicating that machinery here to shave voxels off a body that is about
  // to become a corpse anyway is not worth the second copy. Everything after
  // the sever — ragdoll, gore spray, debris adoption, settle-back — is the
  // existing pipeline with no spell-specific code.
  std::vector<int> hits;
  for (size_t i = 0; i < limbs_.size(); i++) {
    if (!PartAlive((int)i) || !limbs_[i].body) continue;
    const MobLimbDef& ld = limbDefs_[i];
    if ((int)i == def_->rootLimb || ld.vital || !ld.severable) continue;
    // 20 cm of margin past the blast radius, so the limb sweep covers the same
    // real shell at any voxel size.
    if ((limbs_[i].xf.pos - atWorldVoxel).len() <=
        radiusVox + MetresToCells(0.2f))
      hits.push_back((int)i);
  }
  for (int i : hits)
    if (PartAlive(i)) Sever(i);
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

void PlayerAvatar::ApplyFallDamage(Vec3 impactDeltaV, Vec3 centerWorldVoxel,
                                   uint32_t tick,
                                   World& world, std::vector<BrushOp>& ops,
                                   std::vector<ParticleSpawn>& spawns) {
  if (!spawned_ || !alive_ || !def_) return;
  const float impactVox = impactDeltaV.len();
  if (impactVox < 1e-3f) return;
  const auto& pt = CurrentTuning().player;
  const float impactMs = impactVox * kVoxelMeters;
  if (impactMs < pt.fallDamageSpeed) return;

  const MobDef& def = *def_;
  const auto& gore = CurrentTuning().gore;
  const Vec3 center = centerWorldVoxel;

  float excess = impactMs - pt.fallDamageSpeed;
  float damage = excess * excess * pt.fallDamageScale;

  bool lethal = impactMs >= pt.fallSplatSpeed ||
                damage >= (float)TotalHealth();

  if (lethal) {
    // --- splat: carve voxels out of the body, sever some limbs, die ---

    // CarveRadial blows chunks out of every live limb like an explosion would:
    // voxels get ejected as particles, limbs_ losing >75% of volume collapse.
    float carveRadius = 4.0f + impactMs * 0.1f;
    CarveRadial(center, carveRadius, world, spawns);

    // Sever roughly half the remaining severable limbs at random.
    std::vector<int> severable;
    for (size_t i = 0; i < limbs_.size(); i++) {
      if (!PartAlive((int)i) || !limbs_[i].body) continue;
      const MobLimbDef& ld = limbDefs_[i];
      if ((int)i == def.rootLimb || ld.vital || !ld.severable) continue;
      severable.push_back((int)i);
    }
    for (size_t j = 0; j < severable.size(); j++) {
      uint32_t h = Hash3((uint32_t)id_ ^ 0xFA11u, tick, (uint32_t)j);
      if ((h & 1) == 0) continue;
      int i = severable[j];
      if (PartAlive(i)) {
        Sever(i);
        if (limbs_[i].holdBody) limbs_[i].holdSeconds = 0.0f;
      }
    }
    Die();

    // Radial impulse scatters debris outward from the impact.
    if (phys_)
      phys_->ApplyRadialImpulse(center, 8.0f, impactMs * 2.0f);

    // Blood micro-spray burst.
    if (def.bleedMat != 0) {
      int droplets = 400;
      for (int k = 0; k < droplets; k++) {
        if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
        uint32_t h = Hash3((uint32_t)id_ ^ 0xFA11u, tick, (uint32_t)(k + 100));
        Vec3 dir{SignedUnit(h),
                 0.3f + 0.7f * (float)(Pcg(h ^ 0xA001u) & 0xFFFFu) / 65535.0f,
                 SignedUnit(Pcg(h ^ 0xB002u))};
        float len = dir.len();
        if (len > 1e-4f) dir = dir * (1.0f / len);
        float sp = gore.severSpraySpeed *
                   (0.5f + 1.0f * (float)(Pcg(h ^ 0xC003u) & 0xFFFFu) / 65535.0f);
        int life = std::clamp(gore.microLifeTicks, 1, 255);
        spawns.push_back(MakeDroplet(center, dir * sp, def.bleedMat, true,
                                     life, gore.microScale));
      }
      // Whole-voxel blood thrown outward — pools and persists.
      for (int k = 0; k < 30; k++) {
        if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
        uint32_t h = Hash3((uint32_t)id_ ^ 0xB10Du, tick, (uint32_t)(k + 500));
        Vec3 dir{SignedUnit(h),
                 0.2f + 0.4f * (float)(Pcg(h ^ 0xD004u) & 0xFFFFu) / 65535.0f,
                 SignedUnit(Pcg(h ^ 0xE005u))};
        float len = dir.len();
        if (len > 1e-4f) dir = dir * (1.0f / len);
        float sp = gore.severVoxelSpeed *
                   (0.6f + 0.8f * (float)(Pcg(h ^ 0xF006u) & 0xFFFFu) / 65535.0f);
        spawns.push_back(MakeDroplet(center, dir * sp, def.bleedMat, false,
                                     0, 0));
      }
      // Blood stain at the impact site.
      ops.push_back({ifloor(center.x), ifloor(center.y),
                     ifloor(center.z), 2, def.bleedMat, 0, 0, 0});
    }
    return;
  }

  // --- sub-lethal impact: proportional damage, bleed on legs ---
  SpendHealth((int32_t)std::lround(damage));
  if (def.bleedMat != 0) {
    for (size_t i = 0; i < limbs_.size(); i++) {
      if (!PartAlive((int)i) || !limbs_[i].body) continue;
      const MobLimbDef& ld = limbDefs_[i];
      if (ld.name.find("leg") == std::string::npos &&
          ld.name.find("foot") == std::string::npos)
        continue;
      limbs_[i].bleedBudget =
          AddBleedBudget(limbs_[i].bleedBudget, damage * 0.3f * def.bleedPerDamage);
      Quat q{limbs_[i].xf.quat[0], limbs_[i].xf.quat[1],
             limbs_[i].xf.quat[2], limbs_[i].xf.quat[3]};
      limbs_[i].woundLocal = RotateInv(q, center - limbs_[i].xf.pos);
    }
  }
}

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
}

void PlayerAvatar::Revive(const Player& player, float heading) {
  Despawn();
  Spawn(player, heading);
}

// ---- persistence (entities.sve section 'AVTR') ------------------------------

void PlayerAvatar::SaveState(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  // A despawned or dead avatar saves as absent: the corpse (if any) is debris
  // already, and the player respawns whole — see avatar.h.
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

// A body leaving the rig for the world stops being "you": back on the normal
// dynamic layer it can bump the player like any other debris — the avatar-
// layer exemption is only for parts still attached and still living inside
// the player's capsule.
void PlayerAvatar::OnBodyReleasedToWorld(uint64_t bodyHandle) {
  if (phys_) phys_->SetBodyAvatarLayer(bodyHandle, false);
}