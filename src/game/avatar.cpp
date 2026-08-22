#include "game/avatar.h"

#include "phys/lattice.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "game/rigrender.h"
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
  if (!def_) {
    skel_ = AnimSkeleton{};
    limbs_.clear();
    return;
  }
  // Re-seed the owned rig from the (possibly hot-reloaded) def. This runs
  // while DESPAWNED — SetDefs tears the body down first — so there is no
  // equipped item to preserve here; Spawn re-seeds it again and EquipItem
  // re-appends the slot. Doing it in both places keeps "the owned rig always
  // matches the current def" true no matter which entry point ran last.
  skel_ = def_->skel;
  limbs_ = def_->limbs;
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
  // Re-resolve the held prop against the new def: a hot reload replaces the
  // skeleton, so a cached part index from the old one would point at whatever
  // limb happens to sit there now.
  heldPartIndex_ = heldPart_.empty() ? -1 : sk.FindPart(heldPart_);
}

// ---- holding an item --------------------------------------------------------
//
// THE ENTITY <-> SLOT SYNC SEAM. Everything that makes a held item behave like
// a limb happens here and nowhere else; see the note in avatar.h.

bool PlayerAvatar::EquipItem(const ItemDef* item, const char* context) {
  if (!def_ || !phys_) return false;

  // Unequip first, always — including on a re-equip, so swapping weapons goes
  // through exactly one code path instead of a "replace in place" variant that
  // would have to duplicate the joint and body teardown.
  if (heldSlot_ >= 0) {
    Part& old = parts[heldSlot_];
    if (old.joint) phys_->DestroyJoint(old.joint);
    if (old.body) phys_->RemoveBody(old.body);
    // The slot is appended last, so popping it keeps parts/skel_/limbs_
    // index-parallel with no renumbering. Anything holding a part index across
    // an equip would be broken by a mid-vector erase; this cannot be.
    parts.pop_back();
    skel_.parts.pop_back();
    limbs_.pop_back();
    if ((int)hidden_.size() > heldSlot_) hidden_.pop_back();
    anim_.partAlive.resize(skel_.parts.size());
    anim_.springs.resize(skel_.parts.size());
    heldSlot_ = -1;
    heldItem_.clear();
    heldPartIndex_ = -1;
    heldPart_.clear();
    instancesDirty_ = true;
  }
  if (!item) return true;
  if (!spawned_) return false;

  const int si = def_->FindSocket(context);
  if (si < 0) return false;              // no such socket: loud, per avatar.h
  const MobSocketDef& sock = def_->sockets[si];
  const ItemGrip* grip = item->Grip(context);
  if (!grip) return false;               // item cannot be held this way
  if (sock.partIndex < 0 || sock.partIndex >= (int)parts.size()) return false;
  if (!PartAlive(sock.partIndex)) return false;   // no hand, no grip

  // ---- the borrowed slot --------------------------------------------------
  const int slot = (int)parts.size();
  parts.push_back(Part{});
  limbs_.push_back(MobLimbDef{});
  skel_.parts.push_back(AnimPart{});
  hidden_.push_back(0);

  MobLimbDef& ld = limbs_[slot];
  ld.name = "item:" + item->name;
  ld.parent = sock.part;
  ld.joint = Physics::JointType::Fixed;   // a grip does not hinge
  ld.hp = item->hp;
  ld.severable = item->severable;
  ld.vital = false;                       // losing your sword is not fatal
  ld.tag = "item";
  ld.severImpactSpeed = item->severImpactSpeed;
  ld.hasSpring = item->hasSpring;
  ld.spring = item->spring;
  ld.hasEdge = item->hasEdge;
  ld.edgeFrom = item->edgeFrom;
  ld.edgeTo = item->edgeTo;
  ld.edgeHalfWidth = item->edgeHalfWidth;
  ld.microModel = item->microModel;

  AnimPart& ap = skel_.parts[slot];
  ap.name = ld.name;
  ap.parent = sock.partIndex;
  ap.tag = ld.tag;
  ap.hasSpring = ld.hasSpring;
  ap.spring = ld.spring;

  // ---- SOCKET x GRIP, composed forward ------------------------------------
  //
  // The socket is a point in the HAND's own frame; the grip is how the item
  // sits once placed there. Translation applies BEFORE rotation (item.h), so
  // the item's local offset is rotated into the socket frame rather than added
  // after it — getting that order backwards puts the pommel where the tip
  // should be and looks like a bad grip constant.
  //
  // NOT the inverse form (Inverse(grip) x socket) VR rigs use: that solves for
  // a hand pose given a fixed grip, which is not the question here.
  const Part& hand = parts[sock.partIndex];
  const Quat q = QuatMul(sock.rotation, grip->rotation);
  ap.rest.rot = q;
  // The slot's rest position, relative to the hand part, is the socket offset
  // measured from the hand's own model corner.
  const Vec3 socketLocal = sock.offset - hand.restOffset;

  // ---- put the HILT BOX on the socket -------------------------------------
  //
  // The socket is the centre of the hand's authored limb box; the hilt is the
  // centre of the item's authored hilt box (item.h ItemHilt). Aligning the two
  // is the whole placement, and both sides come from the LIMB/ART definitions
  // rather than from a collider or a hand-tuned constant — so re-authoring
  // either one keeps the sword in the fist instead of silently sliding it out.
  //
  // Subtracted, not added: the hilt centre is measured from the ITEM's origin,
  // and what we need is where to put that origin so the hilt lands on the
  // socket. Rotated first, because the item is placed rotated.
  //
  // WITHOUT a hilt box this falls back to translation alone, which is how the
  // sword used to be placed — and which was wrong by 2.5 world voxels against
  // a fist 1 voxel wide, so the blade hung outboard and low. `translation` now
  // means a residual nudge on top of a correct alignment, and is zero for a
  // well-authored item.
  // `gripLocal` is the vector, IN THE ITEM'S OWN FRAME, from the item's origin
  // to the point the fist closes on. With a hilt box that is the hilt centre
  // (plus any residual nudge); without one it degrades to the bare translation,
  // which is the arrangement that shipped the bug.
  const Vec3 gripLocal =
      item->hilt.has ? item->hilt.center - grip->translation
                     : (grip->translation * -1.0f);
  // REST.POS IS THE ANCHOR, exactly as it is for every other part.
  //
  // This is the convention the whole rig runs on and the item must not be the
  // exception: AnimFlatten chains rest.pos parent-to-child, the drive loop
  // recovers the body corner with `anchorW - Rotate(rot, anchorLimb)`, and
  // WeaponEdge/DetachPart go the other way with `xf.pos + Rotate(q,
  // anchorLimb)`. Placing the item's ORIGIN here instead — the intuitive
  // reading, since the grip is expressed from the origin — silently redefines
  // anchorLimb for this one part and every one of those call sites then
  // disagrees with it by the grip vector, rotated by the animated hand. That
  // reads as the blade wandering during a swing rather than as a fixed offset,
  // which is why it does not look like a placement bug at all.
  ap.rest.pos = socketLocal;
  ap.anchorLocal = sock.offset;

  Part& p = parts[slot];
  p.hp = item->hp;
  p.size = item->size;
  p.microModel = item->microModel;
  const float inv = 1.0f / (float)(item->scale ? item->scale : 1);
  // restOffset MEANS THE MODEL'S MIN CORNER, in the part's own frame — that is
  // the contract every other part in this file keeps (see the note above
  // Spawn, and mob.cpp's limb.restOffset), and it is what the kinematic drive
  // loop assumes when it does `pos = anchorW - Rotate(rot, anchorLimb)`.
  //
  // It used to be set to ap.rest.pos here, which is a completely different
  // quantity: the item's ORIGIN measured relative to the HAND. Mixing the two
  // spaces made anchorLimb meaningless, and the drive loop then placed the
  // sword body several voxels from the fist every tick — the "sword lying at
  // the character's feet" symptom, which survived fixing the grip offset
  // because the two bugs are independent.
  p.restOffset = Vec3{(float)item->offset.x, (float)item->offset.y,
                      (float)item->offset.z} * inv;
  // anchorLimb: the anchor measured from this part's own min corner, in the
  // part's own frame — the identical relationship `anchor - restOffset` states
  // for a limb. Here the anchor sits `gripLocal` from the item's ORIGIN, and
  // restOffset is that origin's offset to the corner, so the two compose.
  // Set BEFORE the body is created, because the initial placement below runs
  // the same two steps the drive loop does and needs this term.
  p.anchorLimb = gripLocal - p.restOffset;
  p.voxels.reserve(item->voxels.size());
  for (const PrefabVoxel& v : item->voxels) {
    uint32_t variant = ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
    p.voxels.push_back({(int8_t)v.x, (int8_t)v.y, (int8_t)v.z, 0,
                        (uint16_t)(v.material | (variant << 12))});
  }

  // Body at the composed pose, on the avatar layer like every other part (your
  // own sword must not shove you any more than your own elbow may).
  //
  // The body sits at the item's MIN CORNER, not at its anchor — the same place
  // Spawn puts a limb's body.
  BodyTransform bxf{};
  const Quat handQ{hand.xf.quat[0], hand.xf.quat[1], hand.xf.quat[2],
                   hand.xf.quat[3]};
  const Quat worldQ = QuatMul(handQ, q);
  // Same two steps the drive loop takes, in the same order, so the pose on the
  // equip frame matches the pose on every frame after it: reach the anchor
  // through the hand, then back off to the corner the collider is built around.
  // Doing only the first step leaves the body one anchor-offset out for one
  // tick, which is a visible pop as the sword snaps into the fist.
  bxf.pos = hand.xf.pos + QuatRotate(handQ, ap.rest.pos) -
            QuatRotate(worldQ, p.anchorLimb);
  bxf.quat[0] = q.x; bxf.quat[1] = q.y; bxf.quat[2] = q.z; bxf.quat[3] = q.w;
  p.body = phys_->CreateDebrisBodyXf(p.voxels, bxf, densityOf_, true, inv);
  if (p.body == 0) {
    parts.pop_back();
    skel_.parts.pop_back();
    limbs_.pop_back();
    hidden_.pop_back();
    return false;
  }
  phys_->SetBodyKinematic(p.body, true);
  phys_->SetBodyAvatarLayer(p.body, true);

  // THE GRIP POINT IN THE BODY'S OWN FRAME.
  //
  // Everything above is in the item's AUTHORED frame, where the origin is the
  // model's corner. Jolt does not keep that frame: a compound shape is
  // RE-CENTRED on its centre of mass, so the body position GetTransform hands
  // back is the middle of the blade, not the corner — the sword's local bounds
  // run about -5.2..+5.8 rather than 0..11. That recentring is the reason
  // every offset derived against the authored corner came out wrong.
  //
  // So ask the shape where its own corner ended up and rebase the grip point
  // on it, rather than modelling Jolt's recentring here (which would be a
  // second source of truth for it). Same reasoning the collision-box overlay
  // uses: read the shape that exists, not the one we meant to build.
  {
    Vec3 clo, chi;
    if (phys_->GetLocalBounds(p.body, clo, chi)) gripBody_ = clo + gripLocal;
    else gripBody_ = gripLocal;
    // Place it once, right now, by the SAME expression the drive loop uses —
    // so the equip frame and every frame after it agree and the sword does not
    // pop into position on the first tick.
    const Vec3 socketW = hand.xf.pos + QuatRotate(handQ, ap.rest.pos);
    bxf.pos = socketW - QuatRotate(worldQ, gripBody_);
    float bq[4] = {q.x, q.y, q.z, q.w};
    phys_->MoveKinematicBody(p.body, bxf.pos, bq, 0.0f);
  }

  p.xf = bxf;
  p.anchorRoot = sock.offset;
  p.joint = phys_->CreateJoint(hand.body, p.body, ld.joint,
                               p.xf.pos, ld.axis, ld.minAngle, ld.maxAngle);

  // The new body must not collide with the rest of the avatar, exactly as
  // Spawn arranges for the limbs — a sword resting against the thigh would
  // otherwise fight the solver every tick.
  {
    std::vector<uint64_t> handles;
    for (const Part& q2 : parts)
      if (q2.body) handles.push_back(q2.body);
    phys_->DisableCollisionsAmong(handles);
  }

  anim_.partAlive.resize(skel_.parts.size(), 1);
  anim_.springs.resize(skel_.parts.size(), SpringState{});
  heldSlot_ = slot;
  heldItem_ = item->name;
  // The weapon-arm IK derives the arm from the held part's parent, so these
  // stay in step with the slot rather than being a second source of truth.
  heldPartIndex_ = slot;
  heldPart_ = ld.name;
  instancesDirty_ = true;
  return true;
}

void PlayerAvatar::SetWeaponPose(Vec3 handOffset, Vec3 bladeDir, Vec3 bladeUp,
                                 float weight) {
  weaponHand_ = handOffset;
  // bladeDir/bladeUp are accepted but NOT applied to the held part: the blade
  // keeps its grip angle and only the ARM is driven (see the weapon-arm block
  // in UpdateAnimation). They are kept in the signature because the caller
  // computes them anyway for the HUD, and because a weapon that genuinely does
  // re-aim in the hand — a levelled spear, a raised shield — would want them.
  if (bladeDir.len() > 1e-4f) weaponDir_ = bladeDir.normalized();
  if (bladeUp.len() > 1e-4f) weaponUp_ = bladeUp.normalized();
  weaponWeight_ = weight < 0 ? 0 : (weight > 1 ? 1 : weight);
}

void PlayerAvatar::SetLook(float yawRel, float pitch) {
  const auto& a = CurrentTuning().avatar;
  const float kDeg = 3.14159265f / 180.0f;
  // Wrap defensively: the caller wraps too, but a stale unwrapped angle here
  // would clamp to the wrong stop rather than to the near one.
  while (yawRel > 3.14159265f) yawRel -= 6.2831853f;
  while (yawRel < -3.14159265f) yawRel += 6.2831853f;
  const float yLim = a.headLookYaw * kDeg;
  lookYawGoal_ = std::clamp(yawRel, -yLim, yLim);
  lookPitchGoal_ =
      std::clamp(pitch, -a.headLookPitchDown * kDeg, a.headLookPitchUp * kDeg);
}

uint64_t PlayerAvatar::PartBody(int part) const {
  return (part >= 0 && part < (int)parts.size()) ? parts[part].body : 0;
}

bool PlayerAvatar::OwnsBody(uint64_t bodyHandle) const {
  if (!bodyHandle) return false;
  for (const Part& p : parts)
    if (p.body == bodyHandle) return true;
  return false;
}

bool PlayerAvatar::WeaponEdge(Vec3& outBase, Vec3& outTip,
                              float& outHalfWidth) const {
  if (!def_ || heldPartIndex_ < 0) return false;
  if (heldPartIndex_ >= (int)limbs_.size()) return false;
  const MobLimbDef& ld = limbs_[heldPartIndex_];
  if (!ld.hasEdge) return false;
  if (!PartAlive(heldPartIndex_)) return false;   // severed: nothing to cut with
  const Part& p = parts[heldPartIndex_];
  if (!p.body) return false;
  // The part's body transform sits at the model's MIN CORNER (restOffset in
  // Spawn), and the authored edge is measured from that same corner — so the
  // composition is direct, with no anchor rebasing. Reading the LIVE transform
  // is the point: the hitbox is wherever the renderer just drew the blade.
  Quat q{p.xf.quat[0], p.xf.quat[1], p.xf.quat[2], p.xf.quat[3]};
  outBase = p.xf.pos + QuatRotate(q, ld.edgeFrom);
  outTip = p.xf.pos + QuatRotate(q, ld.edgeTo);
  outHalfWidth = ld.edgeHalfWidth;
  return true;
}

int PlayerAvatar::PartIndex(const std::string& name) const {
  return def_ ? skel_.FindPart(name) : -1;
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
  // A respawn must not inherit the corpse's last glance: the goal is refreshed
  // from the camera on the next tick anyway, but the SMOOTHED value would ease
  // out of a stale twist and the new body would be born looking over its
  // shoulder for a tenth of a second.
  lookYaw_ = lookYawGoal_ = 0;
  lookPitch_ = lookPitchGoal_ = 0;

  // The rig this instance animates is a COPY of the def's (see avatar.h): a
  // held item borrows a slot by APPENDING a part, and the shared def must not
  // grow one. Reset here so a respawn never inherits the last life's sword.
  skel_ = def.skel;
  limbs_ = def.limbs;
  heldSlot_ = -1;
  heldItem_.clear();
  heldPartIndex_ = -1;
  heldPart_.clear();

  parts.assign(def.limbs.size(), Part{});
  // SKIN -> WORLD for positions authored in .vox units; the collider is built
  // at 1/physScale because that is the lattice p.voxels live on (mob.h).
  const float inv = 1.0f / (float)def.skinScale;
  const float physInv = 1.0f / (float)std::max(1u, def.physScale);
  const uint32_t ratio =
      std::max(1u, def.skinScale / std::max(1u, def.physScale));

  for (size_t i = 0; i < def.limbs.size(); i++) {
    const MobLimbDef& ld = def.limbs[i];
    int mi = FindModelIndex(def.prefab, ld.name);
    if (mi < 0) continue;
    const PrefabModel& model = def.prefab.models[mi];
    Part& p = parts[i];
    p.hp = ld.hp;
    p.microModel = ld.microModel;
    p.restOffset = Vec3{(float)model.offset.x, (float)model.offset.y,
                        (float)model.offset.z} * inv;
    if (ratio > 1) {
      // Fine skin: the art is the skin lattice and the collider is DERIVED
      // from it by majority-fill, exactly as MobSystem::Spawn does.
      p.skinVoxels.reserve(model.voxels.size());
      for (const PrefabVoxel& v : model.voxels) {
        uint32_t variant = ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
        p.skinVoxels.push_back(
            {v.x, v.y, v.z, (uint16_t)(v.material | (variant << 12))});
      }
      bool overflow = false;
      p.voxels = DownsampleSkin(p.skinVoxels, ratio, &overflow);
      p.size = IVec3{(model.size.x + (int)ratio - 1) / (int)ratio,
                     (model.size.y + (int)ratio - 1) / (int)ratio,
                     (model.size.z + (int)ratio - 1) / (int)ratio};
    } else {
      p.size = model.size;
      p.voxels.reserve(model.voxels.size());
      for (const PrefabVoxel& v : model.voxels) {
        uint32_t variant = ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
        p.voxels.push_back({(int8_t)v.x, (int8_t)v.y, (int8_t)v.z, 0,
                            (uint16_t)(v.material | (variant << 12))});
      }
    }
    Vec3 o = origin_ + p.restOffset;
    BodyTransform bxf{};
    bxf.pos = o;
    bxf.quat[3] = 1;
    p.body = phys_->CreateDebrisBodyXf(p.voxels, bxf, densityOf_, true, physInv);
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
  int ci = skel_.FindClip(name);
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
      if (!skel_.clips[ci].loop) {
        inst.timeMs = 0;
        inst.ageMs = 0;   // a re-triggered one-shot replays its blend-in too
      }
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
  const AnimSkeleton& sk = skel_;
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
    // "naruto run". The IK below stays in this same prefab-absolute frame and
    // rebases nothing — see the note at the AnimSolveTwoBone call.
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
void PlayerAvatar::UpdateAirPose(float dt) {
  (void)dt;
  for (FootState& f : anim_.feet) {
    f.swinging = false;
    f.swingT = 0;
  }
  // Force a fresh plant on landing. Without this the first grounded tick would
  // measure drift against a plant left over from before take-off — anywhere in
  // the world — and immediately fire a bogus step (and a bogus footstep sound).
  footInit_ = false;
  bodyY_ = origin_.y;
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

  // ---- weapon arm (game/melee.h) -------------------------------------------
  // The swing is driven by aiming the WEAPON ARM's IK chain at a point derived
  // from the mouse. Doing it through the existing chain rather than as a
  // bespoke clip is what makes it compose with everything else: the legs keep
  // walking, the gait keeps running, dismemberment still disables the chain (a
  // severed arm simply stops solving and the sword falls), and a mob could
  // swing the same way through the same call.
  //
  // THE BLADE IS NOT AIMED — THE ARM IS. The sword keeps the orientation its
  // rig gives it, fixed relative to the hand, so it stays orthogonal to the
  // forearm through the whole swing exactly as a gripped weapon does. Clicking
  // buys you CONTROL OF THE ARM, not a blade that re-points itself.
  //
  // An earlier version rotated the held part toward the cut direction every
  // tick. That was wrong twice over: the sword swivelled in the fist (pointing
  // "directly out" mid-swing instead of holding its grip angle), and because
  // the override wrote model[] after the flatten, it fought the pose pipeline
  // and dragged the walk off its authored stride — which read as a leg bug,
  // not a weapon one.
  //
  // POST-PROCESS, LIKE THE LEGS. Same rule as the note above: IK is applied to
  // the flattened pose, never blended as a layer. `weaponWeight_` fades the
  // solve itself, which AnimSolveTwoBone already supports.
  if (weaponWeight_ > 0.0f && heldPartIndex_ >= 0 && !sk.chains.empty()) {
    Quat yaw = AxisAngle({0, 1, 0}, heading_);
    // The weapon arm is the one whose hand is this prop's ancestor. Deriving it
    // from the rig rather than hardcoding "arm.R" means a left-handed rig, or
    // a second weapon, needs no change here.
    int handPart = sk.parts[heldPartIndex_].parent;
    for (size_t c = 0; c < sk.chains.size(); c++) {
      const IkChain& ch = sk.chains[c];
      if (ch.tag != "arm" || ch.effector != handPart) continue;
      float weight = ch.weight * weaponWeight_;
      if (weight <= 0) continue;
      // The shoulder in model space is the chain root's anchor; the target is
      // the mouse-driven offset from it. The offset arrives in WORLD space
      // (main.cpp built it from the camera basis), so it is un-yawed into the
      // rig's frame here — the same conversion the legs do one block up.
      //
      // THEN X IS NEGATED, and that is not a fudge — but the reason is the ART,
      // not the loader. (An earlier note here blamed the .vox -> engine map
      // (x, z, -y) for "flipping handedness". It does not: that matrix has
      // determinant +1 and voxload.cpp says so at the conversion — chirality is
      // preserved. Believing otherwise invites "fixing" the legs one block up
      // by adding a matching negation, which would be wrong.)
      //
      // The real cause is that these rigs are AUTHORED mirrored: every def puts
      // .L at the HIGHER engine x and .R at the lower (asha: armU.L x=28 vs
      // armU.R x=4, legU.L x=21 vs legU.R x=11), so model +X is the character's
      // LEFT. Un-yawing alone therefore lands camera-RIGHT on the model's LEFT:
      // measured at six yaws, RotateInv(yaw, camera Right) is model +X every
      // time. Without this flip the mouse drives the weapon arm to the mirrored
      // side — you could reach across your chest but not out to the side you
      // were actually pointing at. Z (forward) is unaffected: the toe of the
      // shoe sits at model +Z, which is where camera-forward lands.
      //
      // The target is prefab-absolute, like the legs: `shoulder` is anchorLocal
      // and st.model[] already carries the root offset, so nothing is rebased.
      Vec3 shoulder = sk.parts[ch.parts[0]].anchorLocal;
      Vec3 handRig = RotateInv(yaw, weaponHand_);
      handRig.x = -handRig.x;
      Vec3 targetLocal = shoulder + handRig;
      AnimSolveTwoBone(sk, st, ch, targetLocal, weight);
      break;
    }
    // NOTHING ROTATES THE BLADE HERE, deliberately — see the note above. The
    // sword is a child of the hand and AnimFlatten has already composed it
    // against whatever the arm solve produced, so it rides the fist with its
    // authored grip angle intact.
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

    UpdateAnimation(dt, world, player.grounded, player.vel);

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
    // crest never does. Note this deliberately does not touch `grounded` itself
    // — the gait still wants the instantaneous value, and the footfall/landing
    // logic below keys off this debounced view instead.
    const float debounce = CurrentTuning().avatar.airDebounce;
    if (player.grounded) airOffTime_ = 0.0f;
    else airOffTime_ += dt;
    const bool airborneNow = airOffTime_ > debounce;

    // `wasGrounded_` now tracks the DEBOUNCED state, so these edges fire once
    // per real takeoff/landing rather than once per bump.
    if (!airborneNow) {
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
    // The DEBOUNCED state, not the raw bit: storing the raw one would put the
    // edge detection straight back on the flickering signal this block exists
    // to filter, and the jump clip would retrigger on every bump again.
    wasGrounded_ = !airborneNow;

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
      const int ic = def.skel.FindClip("idle");
      const int wc = def.skel.FindClip("walk");
      const int rc = def.skel.FindClip("run");
      const int fc = def.skel.FindClip("fall");
      const int want = airborneNow ? fc : (!moving ? ic : (running ? rc : wc));
      for (ClipInstance& inst : anim_.clips) {
        if (inst.clip < 0) continue;
        if ((inst.clip == ic || inst.clip == wc || inst.clip == rc ||
             inst.clip == fc) &&
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
    Vec3 bodyOrigin{origin_.x, bodyY_, origin_.z};
    for (size_t i = 0; i < parts.size(); i++) {
      Part& p = parts[i];
      if (!p.body) continue;
      if (p.holdSeconds > 0) continue;   // severed, holding its last pose
      Quat local = i < anim_.model.size() ? anim_.model[i].rot : Quat{};
      Vec3 modelPos = i < anim_.model.size() ? anim_.model[i].pos : Vec3{};
      Quat rot = QuatNormalize(Mul(bodyRot, local));
      // modelPos is already in prefab coordinates: AnimFlatten starts from the
      // root's rest.pos which IS rootAnchor, so every part's model pos already
      // contains the root offset. Adding rootAnchor again shifts the entire rig
      // by (pivot) from the player — invisible on a mob (nothing external tracks
      // mob.origin), but on the avatar the camera sits at player.pos and the
      // body must be there too. Using modelPos directly cancels the offset.
      Vec3 anchorW =
          bodyOrigin + pivot + Rotate(bodyRot, modelPos - pivot);
      Vec3 pos = anchorW - Rotate(rot, p.anchorLimb);
      float q[4] = {rot.x, rot.y, rot.z, rot.w};
      phys_->MoveKinematicBody(p.body, pos, q, dt);
      p.xf.pos = pos;
      p.xf.quat[0] = rot.x; p.xf.quat[1] = rot.y;
      p.xf.quat[2] = rot.z; p.xf.quat[3] = rot.w;
    }

    // ---- THE HELD ITEM: HILT ONTO THE HAND ---------------------------------
    //
    // Placed directly, from the hand's just-computed world transform, rather
    // than through the anchorLimb/restOffset pair every other part uses. Those
    // two fields describe a JOINT BETWEEN LIMBS OF ONE PREFAB — they are
    // measured in prefab-local space against the model's own corner, and they
    // are the right tool for a forearm hanging off an elbow. An item is not
    // that: it is a foreign object with its own origin whose entire
    // relationship to the rig is "this point of me sits at that point of the
    // hand". Stated that way it is one rotate and one subtract, and there is
    // no convention left to get subtly wrong.
    //
    // (Getting it wrong through the generic path is what put the sword at the
    // character's feet: the two frames differ by the model corner AND by
    // Jolt's centre-of-mass recentring, and an offset that absorbs both is a
    // magic number nobody can check.)
    if (heldSlot_ >= 0 && heldSlot_ < (int)parts.size()) {
      Part& item = parts[heldSlot_];
      const int handIdx = skel_.parts[heldSlot_].parent;
      if (item.body && item.holdSeconds <= 0 && handIdx >= 0 &&
          handIdx < (int)parts.size() && parts[handIdx].body) {
        const Part& hand = parts[handIdx];
        const Quat handQ{hand.xf.quat[0], hand.xf.quat[1], hand.xf.quat[2],
                         hand.xf.quat[3]};
        // The socket, in world space: a point in the hand's own frame, carried
        // by whatever pose the hand is in this tick.
        const Vec3 socketW =
            hand.xf.pos + Rotate(handQ, skel_.parts[heldSlot_].rest.pos);
        // The item's orientation is the hand's, composed with the authored
        // grip rotation — so the blade keeps its angle in the fist through a
        // swing instead of being re-aimed (melee.h's rule).
        const Quat itemQ =
            QuatNormalize(Mul(handQ, skel_.parts[heldSlot_].rest.rot));
        // Put the grip point on the socket. gripBody_ is already in the item's
        // BODY frame, so this needs no corner or recentring correction.
        const Vec3 pos = socketW - Rotate(itemQ, gripBody_);
        float q[4] = {itemQ.x, itemQ.y, itemQ.z, itemQ.w};
        phys_->MoveKinematicBody(item.body, pos, q, dt);
        item.xf.pos = pos;
        item.xf.quat[0] = q[0]; item.xf.quat[1] = q[1];
        item.xf.quat[2] = q[2]; item.xf.quat[3] = q[3];
      }
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

      if (p.bleedBudget < 1.0f || bleedOps >= gore.bleedOpsPerTick) continue;
      // Charge before emitting, shrinking the clump to what the wound can still
      // afford — same rule 2 reasoning as the mob path (mob.cpp).
      int clumpR = gore.bleedClumpRadius;
      while (clumpR > 0 && (float)BleedClumpVoxels(clumpR) > p.bleedBudget)
        clumpR--;
      // Drip period, tunable — same rule as the mob path (mob.cpp). Modulo,
      // not a mask, because the tuner offers every period and not just powers
      // of two; the divisor is clamped >= 1 at load.
      if (tick % (uint32_t)std::max(1, gore.bleedDripTicks) != 0) continue;
      Vec3 w = p.body ? p.xf.pos + Rotate(lq, p.woundLocal)
                      : bodyOriginNow + Rotate(bodyRotNow, p.anchorRoot);
      // Clump size as a brush radius, debited by the sphere volume it paints —
      // same rule and same reasoning as the mob drip (mob.cpp).
      ops.push_back({ifloor(w.x), ifloor(w.y), ifloor(w.z), clumpR,
                     def.bleedMat, 0, 0, 0});
      p.bleedBudget -= (float)BleedClumpVoxels(clumpR);
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
    const MobLimbDef& ld = limbs_[i];
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
    p.bleedBudget = AddBleedBudget(p.bleedBudget, amount * def.bleedPerDamage);
    if (p.hp <= 0 || impactSevers) Sever((int)i);
    else PlayClip("attack");
    return true;
  }
  return false;
}

// ---- health, as the caster VM sees it (game/spell.h) ------------------------
//
// The player deliberately gains no hp field: health IS the per-part hp the
// dismemberment system already maintains, so the mana bar's overdraw and the
// visible damage state cannot drift apart.

int32_t PlayerAvatar::TotalHealth() const {
  if (!spawned_ || !alive_) return 0;
  float sum = 0;
  for (size_t i = 0; i < parts.size(); i++)
    if (PartAlive((int)i) && parts[i].hp > 0) sum += parts[i].hp;
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
  for (const MobLimbDef& ld : limbs_)
    if (ld.hp > 0) sum += ld.hp;
  return sum <= 0 ? 0 : (int32_t)sum;
}

void PlayerAvatar::SpendHealth(int32_t amount) {
  if (!spawned_ || !alive_ || amount <= 0) return;
  // Spread the cost across live parts in proportion to what each still has,
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
  // Collect first: Sever() mutates `parts` (it detaches children too), so
  // deciding everything against the pre-carve state and acting afterwards is
  // what keeps this from walking a list that reshapes underneath it.
  std::vector<int> severed;
  for (size_t i = 0; i < parts.size(); i++) {
    if (!PartAlive((int)i) || parts[i].hp <= 0) continue;
    parts[i].hp -= parts[i].hp * frac;
    // Bleeding from the strain of the overcast, through the ordinary budget.
    if (def_)
      parts[i].bleedBudget = AddBleedBudget(parts[i].bleedBudget,
                                            6.0f * frac * def_->bleedPerDamage);
    if (parts[i].hp <= 0.0f) severed.push_back((int)i);
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
  for (size_t i = 0; i < parts.size(); i++) {
    if (!PartAlive((int)i) || !parts[i].body) continue;
    const MobLimbDef& ld = limbs_[i];
    if ((int)i == def_->rootLimb || ld.vital || !ld.severable) continue;
    if ((parts[i].xf.pos - atWorldVoxel).len() <= radiusVox + 2.0f)
      hits.push_back((int)i);
  }
  for (int i : hits)
    if (PartAlive(i)) Sever(i);
  Die();
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
  const MobLimbDef& ld = limbs_[partIndex];
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
  for (size_t k = 0; k < limbs_.size(); k++) {
    if (limbs_[k].name != ld.parent) continue;
    Part& parent = parts[k];
    Quat q{parent.xf.quat[0], parent.xf.quat[1], parent.xf.quat[2],
           parent.xf.quat[3]};
    const auto& gore = CurrentTuning().gore;
    parent.woundLocal = RotateInv(q, anchorW - parent.xf.pos);
    parent.bleedBudget =
        AddBleedBudget(parent.bleedBudget, gore.severStumpBudget);
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
  for (size_t k = 0; k < limbs_.size(); k++)
    if (limbs_[k].parent == limbs_[index].name && parts[k].body)
      DetachPart((int)k, adopt);

  if (index < (int)anim_.partAlive.size())
    anim_.partAlive[index] = 0;   // gait stops scheduling it, IK weight -> 0
  if (adopt) {
    // Hand the micro description over with the body — that is what keeps a
    // severed microvoxel limb detailed as ordinary debris.
    debris_->AdoptBody(p.body, p.voxels, p.xf, p.MicroRef(def.skinScale),
                       def.physScale, std::move(p.skinVoxels), def.bleedMat);
    p.skinVoxels.clear();
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
    debris_->AdoptBody(p.body, p.voxels, p.xf, p.MicroRef(def.skinScale),
                       def.physScale, std::move(p.skinVoxels), def.bleedMat);
    p.skinVoxels.clear();
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
    rigrender::AppendVoxInsts(out, slot, p.voxels);
    slot++;
  }
  instancesDirty_ = false;
}

void PlayerAvatar::AppendXforms(std::vector<BodyXformGpu>& out) const {
  for (const Part& p : parts) {
    if (!p.body) continue;
    if (out.size() >= kMaxBodySlots) return;
    rigrender::AppendXform(out, p.xf);
  }
}


// ---- collision-box debug overlay (world.h DebugBox) -------------------------
//
// Draws the individual sub-shapes of each compound collider rather than one
// big AABB per body. See rigrender::AppendDebugBoxesFor for why these come
// from the Jolt shape rather than from the voxels that built it.
void PlayerAvatar::AppendDebugBoxes(std::vector<DebugBox>& out, size_t limit,
                                   uint32_t color) const {
  if (!phys_) return;
  static std::vector<SubShapeBox> subs;
  for (const Part& p : parts) {
    if (!p.body) continue;
    if (out.size() >= limit) return;
    rigrender::AppendDebugBoxesFor(out, *phys_, p.body, p.xf, limit, color,
                                   subs);
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
