#include "game/avatar.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sim/tuning.h"

namespace {

inline Quat AxisAngle(Vec3 axis, float a) { return QuatAxisAngle(axis, a); }
inline Quat Mul(const Quat& a, const Quat& b) { return QuatMul(a, b); }
inline Vec3 Rotate(const Quat& q, Vec3 v) { return QuatRotate(q, v); }
inline Vec3 RotateInv(const Quat& q, Vec3 v) { return QuatRotateInv(q, v); }

// Same stateless counter-based hash the sim shaders and mob.cpp use. The
// avatar's spray is presentation, but it is authored INTO the tick's spawn
// stream, which a replay must reproduce — a stateful rng here would desync the
// moment a frame boundary moved (CLAUDE.md rule 1).
uint32_t Pcg(uint32_t v) {
  uint32_t s = v * 747796405u + 2891336453u;
  uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
  return (w >> 22u) ^ w;
}
uint32_t Hash3(uint32_t a, uint32_t b, uint32_t c) {
  return Pcg(a ^ Pcg(b ^ Pcg(c)));
}
float SignedUnit(uint32_t h) {
  return (float)(int32_t)(h & 0xFFFFu) / 32768.0f - 1.0f;
}

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

int FindModelIndex(const Prefab& p, const std::string& name) {
  for (size_t i = 0; i < p.models.size(); i++)
    if (p.models[i].name == name) return (int)i;
  return -1;
}

}  // namespace

void PlayerAvatar::Init(Physics* phys, World* world, DebrisSystem* debris,
                        const std::vector<MaterialDef>& mats) {
  phys_ = phys;
  world_ = world;
  debris_ = debris;
  OnMaterialsReloaded(mats);
}

void PlayerAvatar::OnMaterialsReloaded(const std::vector<MaterialDef>& mats) {
  // Indexed exactly as MobSystem's table is — no leading air slot. The two
  // feed the same Physics::CreateDebrisBodyXf, so a different convention here
  // would silently give the avatar every material's mass off by one.
  densityOf_.clear();
  classOf_.clear();
  for (const MaterialDef& m : mats) {
    densityOf_.push_back((float)m.gpu.density);
    classOf_.push_back(m.gpu.klass);
  }
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
  if (!defs_) return;
  for (const MobDef& d : *defs_)
    if (d.name == defName_) def_ = &d;
  ResolveParts();
}

void PlayerAvatar::ResolveParts() {
  parts_ = AvatarParts{};
  if (!def_) return;
  const AnimSkeleton& sk = def_->skel;
  parts_.head = sk.FindPart("head");
  parts_.torso = sk.FindPart("torso");
  parts_.hips = sk.FindPart("hips");
  parts_.handL = sk.FindPart("hand.L");
  parts_.handR = sk.FindPart("hand.R");
  parts_.armUL = sk.FindPart("armU.L");
  parts_.armUR = sk.FindPart("armU.R");
  parts_.footL = sk.FindPart("foot.L");
  parts_.footR = sk.FindPart("foot.R");
  parts_.legUL = sk.FindPart("legU.L");
  parts_.legUR = sk.FindPart("legU.R");
  parts_.staff = sk.FindPart("staff");
}

int PlayerAvatar::PartIndex(const std::string& name) const {
  return def_ ? def_->skel.FindPart(name) : -1;
}

bool PlayerAvatar::Spawn(const Player& player, float headingRad) {
  if (spawned_ || !def_ || !phys_) return false;
  const MobDef& def = *def_;
  if (def.limbs.empty()) return false;

  // The avatar's prefab min corner sits under the player's AABB: centred in
  // x/z, feet at the bottom of the box. Everything downstream derives from
  // origin_, so this one expression is where "the art lines up with the
  // collision box" is decided.
  origin_ = Vec3{player.pos.x - def.worldSize.x * 0.5f,
                 player.pos.y - Player::kHalfY,
                 player.pos.z - def.worldSize.z * 0.5f};
  heading_ = headingRad;
  bodyY_ = origin_.y;
  bodyUp_ = Vec3{0, 1, 0};
  footInit_ = false;
  alive_ = true;

  parts.assign(def.limbs.size(), Part{});
  const float inv = 1.0f / (float)def.scale;

  for (size_t i = 0; i < def.limbs.size(); i++) {
    const MobLimbDef& ld = def.limbs[i];
    int mi = FindModelIndex(def.prefab, ld.name);
    if (mi < 0) continue;
    const PrefabModel& model = def.prefab.models[mi];
    Part& p = parts[i];
    p.hp = ld.hp;
    p.size = model.size;
    p.microModel = ld.microModel;
    p.restOffset = Vec3{(float)model.offset.x, (float)model.offset.y,
                        (float)model.offset.z} * inv;
    p.voxels.reserve(model.voxels.size());
    for (const PrefabVoxel& v : model.voxels) {
      uint32_t variant = ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
      p.voxels.push_back({(int8_t)v.x, (int8_t)v.y, (int8_t)v.z, 0,
                          (uint16_t)(v.material | (variant << 12))});
    }
    Vec3 o = origin_ + p.restOffset;
    BodyTransform bxf{};
    bxf.pos = o;
    bxf.quat[3] = 1;
    p.body = phys_->CreateDebrisBodyXf(p.voxels, bxf, densityOf_, true, inv);
    if (p.body == 0) {
      for (Part& q : parts)
        if (q.body) phys_->RemoveBody(q.body);
      parts.clear();
      return false;
    }
    phys_->SetBodyKinematic(p.body, true);
    // YOUR OWN BODY MUST NOT PUSH YOU. These limbs live inside the player's
    // capsule proxy by construction (origin_ above), so on the normal dynamic
    // layer they are permanently interpenetrated with it: the solver fights a
    // contact it can never resolve, and PlayerPushOut reads a large ejection
    // vector whose direction swings with the gait. That is what made walking
    // forward drift backwards and diagonally. The AVATAR layer is identical
    // in every other respect and stays visible to rays, so laser hits and
    // dismemberment are unchanged.
    phys_->SetBodyAvatarLayer(p.body, true);
    p.xf.pos = o;
    p.xf.quat[3] = 1;
  }

  // Joints, after every body exists. Anchors come from the RIG
  // (skel.parts[i].anchorLocal), already resolved and already converted
  // micro -> world at load — re-deriving them here would be a second
  // implementation of the same rule, and the two disagreeing shows up as
  // limbs pivoting off-joint.
  for (size_t i = 0; i < def.limbs.size(); i++) {
    const MobLimbDef& ld = def.limbs[i];
    if ((int)i == def.rootLimb) continue;
    int pi = -1;
    for (size_t k = 0; k < def.limbs.size(); k++)
      if (def.limbs[k].name == ld.parent) pi = (int)k;
    if (pi < 0 || !parts[i].body || !parts[pi].body) continue;
    Vec3 anchor = def.skel.parts[i].anchorLocal;
    parts[i].anchorRoot = anchor;
    parts[i].anchorLimb = anchor - parts[i].restOffset;
    parts[i].joint = phys_->CreateJoint(parts[pi].body, parts[i].body, ld.joint,
                                        origin_ + anchor, ld.axis, ld.minAngle,
                                        ld.maxAngle);
  }
  {
    std::vector<uint64_t> handles;
    for (const Part& p : parts)
      if (p.body) handles.push_back(p.body);
    phys_->DisableCollisionsAmong(handles);
  }
  if (def.rootLimb >= 0 && def.rootLimb < (int)parts.size()) {
    Part& root = parts[def.rootLimb];
    root.anchorRoot = def.skel.parts[def.rootLimb].anchorLocal;
    root.anchorLimb = root.anchorRoot - root.restOffset;
  }

  anim_ = AnimState{};
  anim_.partAlive.assign(def.limbs.size(), 1);
  anim_.springs.assign(def.limbs.size(), SpringState{});
  anim_.feet.assign(def.skel.chains.size(), FootState{});
  for (size_t c = 0; c < def.skel.chains.size(); c++) {
    const IkChain& ch = def.skel.chains[c];
    float len = 0;
    for (size_t k = 1; k < ch.parts.size(); k++)
      len += def.skel.parts[ch.parts[k]].rest.pos.len();
    anim_.feet[c].legLength = len > 0.01f ? len : 1.0f;
    // Only LEG chains schedule footsteps. The arm chains exist so gameplay can
    // place a hand, and a gait that tried to walk on them would plant the
    // wizard's palms on the ground every stride.
    anim_.feet[c].valid = ch.tag == "leg";
  }
  anim_.lastPos = origin_;
  hidden_.assign(def.limbs.size(), 0);
  spawned_ = true;
  instancesDirty_ = true;
  return true;
}

void PlayerAvatar::Despawn() {
  if (phys_) {
    for (Part& p : parts) {
      if (p.joint) phys_->DestroyJoint(p.joint);
      // holdBody is the SAME handle already handed to DebrisSystem, so it is
      // not ours to remove — AdoptBody transferred ownership.
      if (p.body) phys_->RemoveBody(p.body);
      p.joint = 0;
      p.body = 0;
      p.holdBody = 0;
    }
  }
  parts.clear();
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
  hidden_.resize(parts.size(), 0);
  instancesDirty_ = true;
}

void PlayerAvatar::PlayClip(const std::string& name) {
  if (!def_) return;
  int ci = def_->skel.FindClip(name);
  if (ci < 0) return;
  for (ClipInstance& inst : anim_.clips)
    if (inst.clip == ci && !inst.stopping) {
      // ALREADY PLAYING: leave it alone. This used to rewind to timeMs = 0,
      // which is right for a one-shot you re-trigger (cast, land) but fatal for
      // the locomotion clips, because PreTick calls PlayClip("walk") EVERY
      // TICK to keep it alive. Rewinding every tick pinned the clip at t=0
      // forever: the arms sat frozen on the first keyframe — held out in front,
      // never swinging — which is exactly the "zombie arms" look. A looping
      // clip that is already running needs no retrigger at all.
      if (!def_->skel.clips[ci].loop) inst.timeMs = 0;
      return;
    }
  ClipInstance inst;
  inst.clip = ci;
  inst.weight = 1.0f;
  anim_.clips.push_back(inst);
}

// ---- locomotion coupling ----------------------------------------------------

AvatarLocomotion PlayerAvatar::Locomotion() const {
  AvatarLocomotion out;
  out.alive = alive_;
  if (!def_ || !spawned_) return out;
  const AnimSkeleton& sk = def_->skel;
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

bool PlayerAvatar::GroundHeightAt(World& world, int wx, int wz, int yFrom,
                                  int& outY, uint32_t* outMat) const {
  // Scans down through the chunk cache, requesting a fetch for a missing
  // chunk. Bounded to one column per foot per tick (rule 2). Same probe
  // MobSystem uses — solids and powders carry weight, liquids and gases do
  // not, so the wizard wades through blood rather than walking on it.
  for (int y = yFrom; y > yFrom - 24; y--) {
    IVec3 cell{wx, y, wz};
    if (!world.CellInWindow(cell)) return false;
    IVec3 wc{wx >> 4, y >> 4, wz >> 4};
    const CachedChunk* cc = world.Cached(wc);
    if (!cc || cc->voxels.size() != kChunkVol) {
      world.RequestChunkFetch(wc);
      return false;
    }
    uint32_t lx = (uint32_t)(wx & 15), ly = (uint32_t)(y & 15),
             lz = (uint32_t)(wz & 15);
    uint32_t mat = cc->voxels[(lz * kChunk + ly) * kChunk + lx] & 0xFFF;
    if (mat != 0 && mat < classOf_.size() &&
        (classOf_[mat] == CLASS_SOLID || classOf_[mat] == CLASS_POWDER)) {
      outY = y + 1;
      if (outMat != nullptr) *outMat = mat;
      return true;
    }
  }
  return false;
}

void PlayerAvatar::UpdateGait(float dt, World& world) {
  const AnimSkeleton& sk = def_->skel;
  const GaitDef& g = sk.gait;
  if (sk.chains.empty()) return;

  Vec3 fwd{std::sin(heading_), 0, std::cos(heading_)};
  float speedFactor =
      std::clamp(speedNow_ / std::max(def_->speed, 0.01f), 0.0f, 1.5f);

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
    // "naruto run". The IK below closes the loop by subtracting rootAnchor from
    // the prefab point, which is the other half of this same convention.
    Vec3 stancePrefab = restLocal;
    Quat yaw = AxisAngle({0, 1, 0}, heading_);
    Vec3 stance = Vec3{origin_.x, bodyY_, origin_.z} + pivot +
                  Rotate(yaw, stancePrefab - pivot);
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
    int gy = 0;
    uint32_t gmat = 0;
    if (GroundHeightAt(world, ifloor(goal.x), ifloor(goal.z),
                       ifloor(origin_.y) + 2, gy, &gmat))
      goal.y = (float)gy;
    else
      goal.y = origin_.y;

    if (!footInit_) {
      f.planted = goal;
      f.swinging = false;
    }
    if (f.swinging) {
      // SWING TIME SHORTENS WITH SPEED. A fixed stepDuration is what actually
      // caps the stride: only one leg may swing at a time, so the body keeps
      // advancing for the whole swing and the planted foot falls further behind
      // the faster you go — the leg ends up permanently over-extended and the
      // IK just points it backward. Real gaits shorten the swing as they speed
      // up; dividing by speedFactor does the same here and keeps the distance
      // covered per step roughly constant in leg lengths. Clamped so a crawl
      // does not get an infinitely long swing and a sprint keeps a visible arc.
      float durScale = std::clamp(speedFactor, kMinSwingScale, 1.5f);
      float dur = std::max(g.stepDuration / durScale, kMinSwingSeconds);
      f.swingT += dt / dur;
      if (f.swingT >= 1.0f) {
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
        // parabolic arc between lift-off and touch-down
        float t = f.swingT;
        Vec3 flat = f.swingFrom * (1.0f - t) + f.swingTo * t;
        flat.y += std::sin(t * 3.14159265f) * g.stepHeight * f.legLength;
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
  bodyY_ = origin_.y;

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

void PlayerAvatar::UpdateAnimation(float dt, World& world) {
  const AnimSkeleton& sk = def_->skel;
  AnimState& st = anim_;
  if (sk.parts.empty()) return;
  st.partAlive.resize(sk.parts.size(), 1);
  st.springs.resize(sk.parts.size(), SpringState{});

  Vec3 delta = origin_ - st.lastPos;
  st.lastPos = origin_;
  Vec3 planar{delta.x / std::max(dt, 1e-4f), 0, delta.z / std::max(dt, 1e-4f)};
  st.velocity = st.velocity * 0.7f + planar * 0.3f;
  speedNow_ = Vec3{st.velocity.x, 0, st.velocity.z}.len();
  float speedFactor =
      std::clamp(speedNow_ / std::max(def_->speed, 0.01f), 0.0f, 1.5f);

  const GaitDef& g = sk.gait;
  st.gaitPhase += dt * (g.present ? g.cadence : 2.2f) * speedFactor;
  if (st.gaitPhase > 1.0f) st.gaitPhase -= std::floor(st.gaitPhase);

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
  // clip owns the pose (a crawl keys the same parts these drive)
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

  AnimFlatten(sk, st);

  const bool gaitActive = g.present && !clipOwnsPose;
  if (gaitActive) {
    UpdateGait(dt, world);
  } else {
    float targetY = origin_.y + (loco ? loco->bodyYOffset : 0.0f);
    bodyY_ += std::clamp(targetY - bodyY_, -0.4f, 0.4f);
    footInit_ = true;
    bodyUp_ = (bodyUp_ * 0.85f + Vec3{0, 1, 0} * 0.15f).normalized();
    if (bodyUp_.len() < 0.5f) bodyUp_ = {0, 1, 0};
  }

  // IK is a POST-PROCESS on the flattened pose, never a blended layer:
  // blending two IK results gives a pose that satisfies neither end-effector.
  if (!sk.chains.empty() && gaitActive) {
    Quat yaw = AxisAngle({0, 1, 0}, heading_);
    Vec3 rootAnchor = sk.parts[def_->rootLimb].anchorLocal;
    Vec3 pivot{def_->worldSize.x * 0.5f, 0, def_->worldSize.z * 0.5f};
    Vec3 bodyOrigin{origin_.x, bodyY_, origin_.z};
    for (size_t c = 0; c < sk.chains.size() && c < st.feet.size(); c++) {
      if (sk.chains[c].tag != "leg") continue;
      const FootState& f = st.feet[c];
      float weight = f.valid ? sk.chains[c].weight : 0.0f;
      if (weight <= 0) continue;
      Vec3 rel = f.planted - bodyOrigin - pivot;
      Vec3 prefabPt = RotateInv(yaw, rel) + pivot;
      AnimSolveTwoBone(sk, st, sk.chains[c], prefabPt - rootAnchor, weight);
    }
  }
}

// ---- per-tick ---------------------------------------------------------------

void PlayerAvatar::PreTick(uint32_t tick, const Player& player, float heading,
                           float dt, World& world, std::vector<BrushOp>& ops,
                           std::vector<ParticleSpawn>& spawns) {
  if (!spawned_ || !def_) return;
  const MobDef& def = *def_;

  // Drain particles authored since the last tick (dismemberment bursts). Same
  // reasoning as MobSystem: Sever() is reached from damage handling at several
  // points in the frame, and appending straight to the caller's list from
  // there would need it threaded through every one of those paths.
  for (const ParticleSpawn& s : pendingSpawns_) {
    if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
    if (!world.CellInWindow({s.px >> 8, s.py >> 8, s.pz >> 8})) continue;
    spawns.push_back(s);
  }
  pendingSpawns_.clear();

  // Severed pieces tick down their hold before going dynamic, wherever the
  // avatar is in its life: a piece left kinematic and unowned would never
  // sleep (rule 2).
  for (Part& p : parts) {
    if (!p.holdBody) continue;
    p.holdSeconds -= dt;
    if (p.holdSeconds > 0) continue;
    p.holdSeconds = 0;
    phys_->SetBodyKinematic(p.holdBody, false);
    // A severed part must collide with the body it came off again: the
    // DisableCollisionsAmong group suppressed those contacts forever.
    phys_->ClearCollisionGroup(p.holdBody);
    // It also stops being "you". Back on the normal dynamic layer it can bump
    // the player like any other debris — the avatar-layer exemption is only
    // for limbs still attached and still living inside the player's capsule.
    phys_->SetBodyAvatarLayer(p.holdBody, false);
    p.holdBody = 0;
  }

  if (alive_) {
    // The body follows the PLAYER, which is the whole difference from a mob.
    origin_ = Vec3{player.pos.x - def.worldSize.x * 0.5f,
                   player.pos.y - Player::kHalfY,
                   player.pos.z - def.worldSize.z * 0.5f};
    heading_ = heading;

    UpdateAnimation(dt, world);

    // ---- air state clips ----
    // Grounded transitions drive jump/land; sustained air drives fall. Kept
    // here rather than in main.cpp so every consumer of the avatar gets the
    // same behaviour without restating the thresholds.
    if (player.grounded) {
      if (!wasGrounded_ && airTime_ > 0.25f) {
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
      if (wasGrounded_) PlayClip("jump");
      airTime_ += dt;
      if (airTime_ > 0.45f) PlayClip("fall");
      // Remember how fast we are falling; the landing tick needs it after the
      // sweep has already cancelled the velocity.
      lastFallSpeed_ = player.vel.y < 0 ? -player.vel.y : 0.0f;
    }
    wasGrounded_ = player.grounded;

    // ---- locomotion clips ----
    // Additive arm swing over the IK legs; `run` replaces `walk` past half of
    // the def's top speed. Both are retriggered every tick, which PlayClip
    // turns into a no-op once the instance exists.
    const bool moving = speedNow_ > 0.4f && player.grounded;
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
      const int ic = def.skel.FindClip("idle");
      const int wc = def.skel.FindClip("walk");
      const int rc = def.skel.FindClip("run");
      const int want = !moving ? ic : (running ? rc : wc);
      for (ClipInstance& inst : anim_.clips) {
        if (inst.clip < 0) continue;
        if ((inst.clip == ic || inst.clip == wc || inst.clip == rc) &&
            inst.clip != want)
          inst.stopping = true;
      }
    }
    if (moving) PlayClip(running ? "run" : "walk");
    else PlayClip("idle");

    // ---- submit kinematic targets ----
    Quat yaw = AxisAngle({0, 1, 0}, heading_);
    Quat tilt = QuatFromTo({0, 1, 0}, bodyUp_);
    Quat bodyRot = Mul(tilt, yaw);
    Vec3 pivot{def.worldSize.x * 0.5f, 0, def.worldSize.z * 0.5f};
    Vec3 rootAnchor = def.skel.parts.empty()
                          ? Vec3{}
                          : def.skel.parts[def.rootLimb].anchorLocal;
    Vec3 bodyOrigin{origin_.x, bodyY_, origin_.z};
    for (size_t i = 0; i < parts.size(); i++) {
      Part& p = parts[i];
      if (!p.body) continue;
      if (p.holdSeconds > 0) continue;   // severed, holding its last pose
      Quat local = i < anim_.model.size() ? anim_.model[i].rot : Quat{};
      Vec3 modelPos = i < anim_.model.size() ? anim_.model[i].pos : Vec3{};
      Quat rot = QuatNormalize(Mul(bodyRot, local));
      Vec3 anchorPrefab = modelPos + rootAnchor;
      Vec3 anchorW =
          bodyOrigin + pivot + Rotate(bodyRot, anchorPrefab - pivot);
      Vec3 pos = anchorW - Rotate(rot, p.anchorLimb);
      float q[4] = {rot.x, rot.y, rot.z, rot.w};
      phys_->MoveKinematicBody(p.body, pos, q, dt);
    }
  }

  // ---- bleeding: same decaying wound budget and bounded ops as mobs ----
  if (def.bleedMat != 0) {
    const auto& gore = CurrentTuning().gore;
    const Quat bodyRotNow = AxisAngle({0, 1, 0}, heading_);
    const Vec3 bodyOriginNow{origin_.x, bodyY_, origin_.z};
    int bleedOps = 0;
    for (size_t li = 0; li < parts.size(); li++) {
      Part& p = parts[li];
      Quat lq{p.xf.quat[0], p.xf.quat[1], p.xf.quat[2], p.xf.quat[3]};

      if (p.gushTicks > 0) {
        int decay = std::max(1, gore.severDecayTicks);
        // Triangular weighting so severSpray is the droplet COUNT released
        // over the window, not a rate to be multiplied out. Front-loaded: the
        // first ticks after the cut throw the bulk and the tail thins, which
        // is what reads as arterial rather than as a running tap.
        float frac = (float)p.gushTicks / (float)decay;
        int want = (int)std::lround(2.0f * (float)gore.severSpray * frac /
                                    (float)decay);
        Vec3 sOrigin = p.body ? p.xf.pos + Rotate(lq, p.gushLocal)
                              : bodyOriginNow + Rotate(bodyRotNow, p.anchorRoot);
        Vec3 axis = p.body ? Rotate(lq, p.gushDir)
                           : Rotate(bodyRotNow, p.gushDir);
        for (int k = 0; k < want; k++) {
          if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
          uint32_t h = Hash3((uint32_t)id_ * 2654435761u + (uint32_t)li, tick,
                             (uint32_t)k * 0x9E3779B9u);
          float cone = gore.severSprayCone;
          Vec3 dir{axis.x + SignedUnit(h) * cone,
                   axis.y + SignedUnit(Pcg(h ^ 0x51A17u)) * cone,
                   axis.z + SignedUnit(Pcg(h ^ 0xB0011u)) * cone};
          float sp = gore.severSpraySpeed *
                     (0.75f + 0.5f * (float)(Pcg(h ^ 0x1234u) & 0xFFFFu) / 65535.0f);
          if (sp < 0.0f) sp = 0.0f;
          int life = std::clamp(gore.microLifeTicks, 1, 255);
          if (!world.CellInWindow({ifloor(sOrigin.x), ifloor(sOrigin.y),
                                   ifloor(sOrigin.z)}))
            break;
          spawns.push_back(MakeDroplet(sOrigin, dir * sp, def.bleedMat, true,
                                       life, gore.microScale));
        }
        p.gushTicks--;
      }

      if (p.bleedBudget < 1.0f || bleedOps >= kBleedOpsPerTick) continue;
      if ((tick & 3u) != 0) continue;   // drip every 4th tick
      Vec3 w = p.body ? p.xf.pos + Rotate(lq, p.woundLocal)
                      : bodyOriginNow + Rotate(bodyRotNow, p.anchorRoot);
      ops.push_back({ifloor(w.x), ifloor(w.y), ifloor(w.z), 1, def.bleedMat, 0,
                     0, 0});
      p.bleedBudget -= 1.0f;
      bleedOps++;
    }
  }
}

void PlayerAvatar::PostStep() {
  for (Part& p : parts)
    if (p.body) phys_->GetTransform(p.body, p.xf);
}

// ---- damage -----------------------------------------------------------------

bool PlayerAvatar::Damage(uint64_t bodyHandle, float amount, Vec3 hitWorldVoxel,
                          float impactSpeed) {
  if (!def_ || !spawned_) return false;
  for (size_t i = 0; i < parts.size(); i++) {
    Part& p = parts[i];
    if (p.body != bodyHandle) continue;
    const MobDef& def = *def_;
    const MobLimbDef& ld = def.limbs[i];
    Quat q{p.xf.quat[0], p.xf.quat[1], p.xf.quat[2], p.xf.quat[3]};
    // a beam crossing the joint anchor severs outright
    if ((int)i != def.rootLimb && p.joint) {
      Vec3 anchorW = p.xf.pos + Rotate(q, p.anchorLimb);
      if ((hitWorldVoxel - anchorW).len() < 1.75f) {
        Sever((int)i);
        return true;
      }
    }
    bool impactSevers =
        ld.severImpactSpeed > 0 && impactSpeed >= ld.severImpactSpeed;
    p.hp -= amount;
    p.woundLocal = RotateInv(q, hitWorldVoxel - p.xf.pos);
    p.bleedBudget = std::min(p.bleedBudget + amount * def.bleedPerDamage, 120.0f);
    if (p.hp <= 0 || impactSevers) Sever((int)i);
    else PlayClip("attack");
    return true;
  }
  return false;
}

bool PlayerAvatar::SeverByName(const std::string& name) {
  int i = PartIndex(name);
  if (i < 0 || !PartAlive(i)) return false;
  Sever(i);
  return true;
}

void PlayerAvatar::Sever(int partIndex) {
  if (!def_ || !spawned_) return;
  const MobDef& def = *def_;
  if (partIndex < 0 || partIndex >= (int)parts.size()) return;
  const MobLimbDef& ld = def.limbs[partIndex];
  if (partIndex == def.rootLimb || ld.vital || !ld.severable) {
    Die();
    return;
  }
  // The cut point in WORLD space, captured BEFORE DetachPart: the joint anchor
  // expressed through the severed part's own live transform. Using the
  // rest-pose `origin_ + anchorRoot` instead would ignore both the heading and
  // the animated body height, putting the spray under the avatar's feet and on
  // its mirrored side.
  const Part& cut = parts[partIndex];
  Quat cq{cut.xf.quat[0], cut.xf.quat[1], cut.xf.quat[2], cut.xf.quat[3]};
  Vec3 anchorW = cut.body ? cut.xf.pos + Rotate(cq, cut.anchorLimb)
                          : origin_ + cut.anchorRoot;

  DetachPart(partIndex, true);

  // the stump bleeds: wound at the joint on the PARENT side
  for (size_t k = 0; k < def.limbs.size(); k++) {
    if (def.limbs[k].name != ld.parent) continue;
    Part& parent = parts[k];
    Quat q{parent.xf.quat[0], parent.xf.quat[1], parent.xf.quat[2],
           parent.xf.quat[3]};
    parent.woundLocal = RotateInv(q, anchorW - parent.xf.pos);
    parent.bleedBudget = std::min(parent.bleedBudget + 40.0f, 120.0f);
    const auto& gore = CurrentTuning().gore;
    parent.gushTicks = std::max(1, gore.severDecayTicks);
    parent.gushLocal = parent.woundLocal;
    // Spray along the stump: from the parent's centre out through the wound,
    // so a cut arm sprays away from the torso instead of into it.
    Vec3 out = anchorW - parent.xf.pos;
    float len = out.len();
    out = len > 1e-3f ? out * (1.0f / len) : Vec3{0, 1, 0};
    // Tilt upward — a horizontal jet mostly misses the world and the droplets
    // expire in mid-air with nothing stained.
    out.y += 0.5f;
    len = out.len();
    parent.gushDir =
        RotateInv(q, len > 1e-3f ? out * (1.0f / len) : Vec3{0, 1, 0});
    break;
  }
  instancesDirty_ = true;
}

void PlayerAvatar::DetachPart(int index, bool adopt) {
  Part& p = parts[index];
  if (!p.body) return;
  if (p.joint) {
    phys_->DestroyJoint(p.joint);
    p.joint = 0;
  }
  // Children are orphaned too: their joints attach to this part and die with
  // it. A severed forearm must take the hand (and the staff) with it.
  const MobDef& def = *def_;
  for (size_t k = 0; k < def.limbs.size(); k++)
    if (def.limbs[k].parent == def.limbs[index].name && parts[k].body)
      DetachPart((int)k, adopt);

  if (index < (int)anim_.partAlive.size())
    anim_.partAlive[index] = 0;   // gait stops scheduling it, IK weight -> 0
  if (adopt) {
    // Hand the micro description over with the body — that is what keeps a
    // severed microvoxel limb detailed as ordinary debris.
    debris_->AdoptBody(p.body, p.voxels, p.xf, p.MicroRef(def.scale));
    p.holdBody = p.body;
    p.holdSeconds = kSeverHoldSeconds;
  } else if (phys_) {
    phys_->RemoveBody(p.body);
  }
  p.body = 0;
  instancesDirty_ = true;
}

void PlayerAvatar::Die() {
  if (!alive_) return;
  alive_ = false;
  // Ragdoll: every remaining part goes dynamic and becomes ordinary debris, so
  // culling, terrain upkeep and settle-back all apply with no avatar-specific
  // code. Joints are kept, so the corpse stays articulated as it falls.
  const MobDef& def = *def_;
  for (size_t i = 0; i < parts.size(); i++) {
    Part& p = parts[i];
    if (!p.body) continue;
    debris_->AdoptBody(p.body, p.voxels, p.xf, p.MicroRef(def.scale));
    phys_->SetBodyKinematic(p.body, false);
    phys_->ClearCollisionGroup(p.body);
    // The corpse is debris now, not the player's body — it collides normally.
    phys_->SetBodyAvatarLayer(p.body, false);
    p.body = 0;
    p.joint = 0;
  }
  instancesDirty_ = true;
}

void PlayerAvatar::Revive(const Player& player, float heading) {
  Despawn();
  Spawn(player, heading);
}

// ---- queries ----------------------------------------------------------------

bool PlayerAvatar::PartWorldTransform(int part, Vec3& outPos,
                                      Quat& outRot) const {
  if (part < 0 || part >= (int)parts.size() || !parts[part].body) return false;
  const Part& p = parts[part];
  outPos = p.xf.pos;
  outRot = Quat{p.xf.quat[0], p.xf.quat[1], p.xf.quat[2], p.xf.quat[3]};
  return true;
}

bool PlayerAvatar::PartAnchorWorld(int part, Vec3& out) const {
  if (part < 0 || part >= (int)parts.size()) return false;
  const Part& p = parts[part];
  if (!p.body) return false;
  Quat q{p.xf.quat[0], p.xf.quat[1], p.xf.quat[2], p.xf.quat[3]};
  out = p.xf.pos + Rotate(q, p.anchorLimb);
  return true;
}

int PlayerAvatar::LivePartCount() const {
  int n = 0;
  for (const Part& p : parts)
    if (p.body) n++;
  return n;
}

uint32_t PlayerAvatar::LimbBodyCount() const {
  uint32_t n = 0;
  for (const Part& p : parts)
    if (p.body) n++;
  return n;
}

// ---- render -----------------------------------------------------------------
//
// The three Append* walks MUST visit slots in the same order, because the slot
// a transform lands in is the slot the instance records. This mirrors
// MobSystem's contract exactly.

void PlayerAvatar::AppendInstances(std::vector<BodyVoxInst>& out,
                                   uint32_t slotBase) {
  uint32_t slot = slotBase;
  for (size_t i = 0; i < parts.size(); i++) {
    const Part& p = parts[i];
    if (!p.body) continue;
    if (slot >= kMaxBodySlots) break;
    // Micro parts render through the OBB/brick-march pass, so they must not
    // also emit cube instances — that would draw them twice, at the wrong
    // size. The slot is still consumed: slots are shared between the passes.
    if (p.microModel >= 0) {
      slot++;
      continue;
    }
    if (i < hidden_.size() && hidden_[i]) {
      slot++;
      continue;
    }
    for (const DebrisVoxel& v : p.voxels) {
      if (out.size() >= kMaxBodyVoxInstances) break;
      out.push_back({(float)v.x, (float)v.y, (float)v.z,
                     (uint32_t)v.payload | (slot << 16)});
    }
    slot++;
  }
  instancesDirty_ = false;
}

void PlayerAvatar::AppendXforms(std::vector<BodyXformGpu>& out) const {
  for (const Part& p : parts) {
    if (!p.body) continue;
    if (out.size() >= kMaxBodySlots) return;
    BodyXformGpu x{};
    x.pos[0] = p.xf.pos.x;
    x.pos[1] = p.xf.pos.y;
    x.pos[2] = p.xf.pos.z;
    std::memcpy(x.quat, p.xf.quat, sizeof(x.quat));
    out.push_back(x);
  }
}

void PlayerAvatar::AppendMicroInsts(std::vector<MicroBodyInstGpu>& out,
                                    uint32_t slotBase) const {
  uint32_t slot = slotBase;
  for (size_t i = 0; i < parts.size(); i++) {
    const Part& p = parts[i];
    if (!p.body) continue;
    if (slot >= kMaxBodySlots) return;
    if (p.microModel >= 0 && !(i < hidden_.size() && hidden_[i]))
      out.push_back({slot, (uint32_t)p.microModel, 0, 0});
    slot++;
  }
}
