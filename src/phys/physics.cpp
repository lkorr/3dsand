#include "phys/physics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhase.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Body/MotionQuality.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include "sim/tuning.h"
#include "sim/world.h"  // kVoxelMeters

namespace {

namespace Layers {
constexpr JPH::ObjectLayer STATIC = 0;
constexpr JPH::ObjectLayer MOVING = 1;
// player proxy: collides with MOVING only — terrain collision is the voxel
// AABB controller's job, and resolving against both would double-collide
// (A proxy is given PLAYER_BASE + its slot when one is free, CreatePlayerBody;
// this unslotted PLAYER is the overflow and behaves as the old single layer.)
constexpr JPH::ObjectLayer PLAYER = 2;
// EXEMPT FROM EVERY PLAYER (the old AVATAR layer; Physics::BodyRole says who
// lands here now: a piece still CLEARING, a severed hold, an avatar that has
// not been told its proxy). Identical to MOVING in every respect EXCEPT that
// it does not collide with any PLAYER. The avatar is drawn AROUND the player's own
// capsule by construction (avatar.cpp derives origin_ from player.pos), so on
// MOVING every limb is permanently interpenetrated with the proxy. That fed
// the solver a contact it could never resolve and fed PlayerPushOut a large
// depenetration vector whose direction swung with the gait — which is exactly
// "walking forward moves me backwards/diagonally, sporadically". Your own body
// must never be able to push you — and, with two players, SOMEBODY ELSE's body
// must still be able to, which is what OWNED_BASE below is for.
constexpr JPH::ObjectLayer EXEMPT = 3;
// A HELD PROP: geometry that is CARRIED, not simulated. Collides with NOTHING
// — not terrain, not debris, not another creature, not the player proxy — and
// is fully visible to every QUERY (ray casts, shape casts, overlap tests).
//
// The split this layer makes is the same one AVATAR makes, taken to its end:
// CONTACTS vs VISIBILITY. A sword in a fist is posed by its wielder's hand
// every tick — it is kinematic and jointed to the hand, so contacts can never
// move the weapon itself, and the only thing they can do is move everything
// the weapon touches. That is not physics, it is a kinematic body of rig mass
// sweeping through the world at swing speed, and it reads as exactly the jank
// it is: standing next to an armed NPC shoves you off your feet, and walking
// through a fight scatters the corpses.
//
// NOTHING IN COMBAT IS LOST BY THIS, because nothing in combat was ever routed
// through these contacts. A swing damages via `MeleeSweep`'s ray probes down
// the blade's own axis (game/melee.cpp `CastRayBody`), a parry is decided
// GEOMETRICALLY by `MobSystem::FindParry` on the two edge segments, and melee
// applies no impulses at all. All three keep working here, because a query
// filter is what they go through and this layer is in DynamicLayerFilter.
constexpr JPH::ObjectLayer PROP = 4;
// A THROWN BODY STILL LEAVING THE THROWER. AVATAR is not enough for this: it
// keeps a released piece off the player PROXY, but AVATAR collides with AVATAR,
// so a flask launched from the hand slammed into the thrower's own head and arm
// on its first step — a velocity jump the vessel break pass reads as a blow,
// and the flask burst in your face. This layer touches only terrain and
// ordinary bodies (STATIC, MOVING); TickPendingReleases moves it to MOVING the
// moment it is clear of every player, after which it hits anyone, the thrower
// included. Reported by the contact listener like MOVING — it is a real
// projectile, and its impacts are what break it.
constexpr JPH::ObjectLayer THROWN = 5;
// A DOOR LEAF WHILE IT IS OPEN (world/refs_doors.h, BodyRole::Door): a
// dynamic body on a hinge to the world. It meets bodies, creatures and every
// player capsule -- it stops against a man in the doorway and he cannot walk
// through it -- but NOT terrain. The terrain collider around it is meshed from
// the fetch cache, which still holds the leaf's own cells for a tick or two
// after they were cleared, and the frame touches the leaf on three faces:
// meeting STATIC would start every door inside its own wall. What stops the
// leaf is the hinge's limit, and what keeps a wall out of its arc is the
// author (the References page draws the arc).
constexpr JPH::ObjectLayer DOOR = 6;
// OWNER-SCOPED LAYERS (W2-N), one per player slot. PLAYER_BASE+s is player s's
// capsule. OWNED_BASE+s is MOVING except that it never meets PLAYER_BASE+s:
// player s's own limbs, worn shells and grabbed load — exempt from THEIR
// capsule and nobody else's. THROWN_BASE+s is THROWN except that it also
// meets what is not player s's (another player's limbs and capsule): a flask
// clears its thrower and still hits the man it was thrown at.
constexpr int kSlots = 8;  // == Physics::kMaxPlayerProxies
constexpr JPH::ObjectLayer PLAYER_BASE = 8;
constexpr JPH::ObjectLayer OWNED_BASE = 16;
constexpr JPH::ObjectLayer THROWN_BASE = 24;
constexpr JPH::ObjectLayer NUM = 32;

// The slot a layer is scoped to, or -1. One reader per family so the pair
// filter, the query filters and the contact listener cannot disagree.
int PlayerSlot(JPH::ObjectLayer l) {
  return l >= PLAYER_BASE && l < PLAYER_BASE + kSlots ? (int)(l - PLAYER_BASE) : -1;
}
int OwnedSlot(JPH::ObjectLayer l) {
  return l >= OWNED_BASE && l < OWNED_BASE + kSlots ? (int)(l - OWNED_BASE) : -1;
}
int ThrownSlot(JPH::ObjectLayer l) {
  return l >= THROWN_BASE && l < THROWN_BASE + kSlots ? (int)(l - THROWN_BASE) : -1;
}
bool IsPlayer(JPH::ObjectLayer l) { return l == PLAYER || PlayerSlot(l) >= 0; }
bool IsOwned(JPH::ObjectLayer l) { return OwnedSlot(l) >= 0; }
bool IsThrown(JPH::ObjectLayer l) { return l == THROWN || ThrownSlot(l) >= 0; }
// Is a body on `b` exempt from the capsule on player layer `p`?
bool ExemptFromPlayer(JPH::ObjectLayer b, JPH::ObjectLayer p) {
  if (b == STATIC || b == EXEMPT || b == PROP || b == THROWN) return true;
  const int s = OwnedSlot(b) >= 0 ? OwnedSlot(b) : ThrownSlot(b);
  return s >= 0 && PlayerSlot(p) == s;  // an unslotted PLAYER owns nothing
}
}  // namespace Layers

namespace BP {
constexpr JPH::BroadPhaseLayer STATIC(0);
constexpr JPH::BroadPhaseLayer MOVING(1);
constexpr uint32_t NUM = 2;
}  // namespace BP

class BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
 public:
  uint32_t GetNumBroadPhaseLayers() const override { return BP::NUM; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
    return layer == Layers::STATIC ? BP::STATIC : BP::MOVING;
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override {
    return "bp";
  }
#endif
};

class ObjVsBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
    if (layer == Layers::STATIC) return bp == BP::MOVING;  // static vs moving only
    if (Layers::IsPlayer(layer)) return bp == BP::MOVING;  // proxy: bodies only
    // A held prop is rejected by the pair filter against every layer there is,
    // so stopping it here costs the broadphase nothing and saves it pairing a
    // fast-swinging body against the whole world every step. This filter is
    // the SIMULATION's; queries take a JPH::BroadPhaseLayerFilter instead and
    // are unaffected (see Layers::PROP).
    if (layer == Layers::PROP) return false;
    if (layer == Layers::DOOR) return bp == BP::MOVING;  // never terrain
    return true;
  }
};

class ObjPairFilter final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    // A held prop has no contacts with anything, including another prop: two
    // blades that cross are a PARRY, decided geometrically by
    // MobSystem::FindParry, and letting the solver see them as well would put
    // a second, disagreeing answer underneath the one combat actually reads.
    if (a == Layers::PROP || b == Layers::PROP) return false;
    if (a == Layers::STATIC && b == Layers::STATIC) return false;
    if (a == Layers::DOOR || b == Layers::DOOR) {
      const JPH::ObjectLayer o = a == Layers::DOOR ? b : a;
      return o != Layers::STATIC && o != Layers::DOOR;
    }
    // A capsule meets bodies only (terrain is the AABB controller's), never
    // another capsule, and never a body exempt from IT: the EXEMPT layer is
    // exempt from everyone, an OWNED/THROWN one from its own player only.
    const bool pa = Layers::IsPlayer(a), pb = Layers::IsPlayer(b);
    if (pa && pb) return false;
    if (pa || pb) {
      const JPH::ObjectLayer p = pa ? a : b, o = pa ? b : a;
      return !Layers::ExemptFromPlayer(o, p);
    }
    // A body on its way out of a thrower meets only the world — terrain and
    // ordinary bodies — plus, when the thrower is known, bodies owned by
    // somebody ELSE (see THROWN / THROWN_BASE).
    if (Layers::IsThrown(a) || Layers::IsThrown(b)) {
      const JPH::ObjectLayer t = Layers::IsThrown(a) ? a : b;
      const JPH::ObjectLayer o = t == a ? b : a;
      if (o == Layers::STATIC || o == Layers::MOVING) return true;
      const int ts = Layers::ThrownSlot(t), os = Layers::OwnedSlot(o);
      return ts >= 0 && os >= 0 && os != ts;
    }
    return true;
  }
};

// "Any body a query should be able to see", i.e. every non-static layer.
// SpecifiedObjectLayerFilter takes a single layer, which stopped being enough
// once the avatar moved off MOVING — and PROP is here for the same reason it
// is: the split those layers make is about CONTACTS, not about visibility.
// A laser must still be able to burn a sword out of somebody's hand, the look
// ray must still name it, and melee's blade probes must still be able to find
// one.
class DynamicLayerFilter final : public JPH::ObjectLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer layer) const override {
    return layer != Layers::STATIC && !Layers::IsPlayer(layer);
  }
};

// What a player capsule's PlayerPushOut may be shoved by: ordinary bodies,
// and bodies OWNED by some other player (their limbs, their grabbed load) —
// never its own. `ownSlot` -1 (an unslotted capsule) owns nothing, so every
// OWNED body reaches it, exactly as the pair filter lets them meet.
class PushLayerFilter final : public JPH::ObjectLayerFilter {
 public:
  explicit PushLayerFilter(int ownSlot) : ownSlot_(ownSlot) {}
  bool ShouldCollide(JPH::ObjectLayer layer) const override {
    if (layer == Layers::MOVING || layer == Layers::DOOR) return true;
    const int s = Layers::OwnedSlot(layer);
    return s >= 0 && s != ownSlot_;
  }

 private:
  int ownSlot_;
};

// Role + owner packed into a Jolt body's user data (see Physics::BodyRole):
// role in the low byte, owner handle above it. 0 = Debris, no owner, which is
// what every freshly created body already carries.
uint64_t PackRole(Physics::BodyRole r, uint64_t owner) {
  return (uint64_t)r | ((owner & Physics::kAnyPlayer) << 8);
}
Physics::BodyRole UnpackRole(uint64_t ud) {
  const uint64_t r = ud & 0xFF;
  return r < (uint64_t)Physics::BodyRole::Count ? (Physics::BodyRole)r
                                                : Physics::BodyRole::Debris;
}
uint64_t UnpackOwner(uint64_t ud) { return ud >> 8; }

float VoxToM(float v) { return v * kVoxelMeters; }

JPH::BodyID ToBodyID(uint64_t h) { return JPH::BodyID((uint32_t)(h - 1)); }
uint64_t FromBodyID(JPH::BodyID id) { return (uint64_t)id.GetIndexAndSequenceNumber() + 1; }

// ---- SANDVOX_PHYS_TRACE=<file>: the boot-to-boot physics diff (det-cpu) ----
// One line per body birth (B), removal (R), terrain patch (T, with a hash of
// its triangles) and per Step the full body-state hash before (P) and after
// (S) Update. Two boots of the same run, `diff`ed, name the first input or
// step that differs -- the attribution village-twice gives in-process, for
// the cross-process case. Off (one null test) unless the variable is set.
FILE* PhysTrace() {
  static FILE* f = [] {
    const char* e = std::getenv("SANDVOX_PHYS_TRACE");
    return (e != nullptr && *e) ? std::fopen(e, "w") : (FILE*)nullptr;
  }();
  return f;
}
uint64_t FnvBytes(uint64_t h, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

// ---- deterministic body ids (Physics::ResetBodyIdHistory, det-cpu) ----------
// The max-bodies number Physics::Init hands Jolt; an index at or past it is
// refused by CreateBodyWithID.
constexpr uint32_t kMaxJoltBodies = 4096;
// Every body this file creates goes through here instead of Jolt's own id
// allocator: the lowest free index, its sequence number bumped.
JPH::BodyID CreateAndAddDet(JPH::BodyInterface& bi, Physics::BodyIdAlloc& a,
                            const JPH::BodyCreationSettings& bcs,
                            JPH::EActivation act) {
  if (a.used.size() < kMaxJoltBodies) {
    a.used.resize(kMaxJoltBodies, 0);
    a.seq.resize(kMaxJoltBodies, 0);
  }
  uint32_t idx = a.lowestFree;
  while (idx < kMaxJoltBodies && a.used[idx]) idx++;
  if (idx >= kMaxJoltBodies) return JPH::BodyID();   // out of bodies, as Jolt
  const uint8_t s = (uint8_t)(a.seq[idx] + 1);
  JPH::Body* b = bi.CreateBodyWithID(JPH::BodyID(idx, s), bcs);
  if (b == nullptr) return JPH::BodyID();
  a.seq[idx] = s;
  a.used[idx] = 1;
  a.lowestFree = idx + 1;
  bi.AddBody(b->GetID(), act);
  if (FILE* tf = PhysTrace()) {
    const JPH::RVec3 p = b->GetPosition();
    const float pf[3] = {(float)p.GetX(), (float)p.GetY(), (float)p.GetZ()};
    uint32_t pb[3];
    std::memcpy(pb, pf, sizeof pb);
    std::fprintf(tf, "B %x layer %u motion %u pos %08x %08x %08x\n",
                 b->GetID().GetIndexAndSequenceNumber(), (unsigned)b->GetObjectLayer(),
                 (unsigned)b->GetMotionType(), pb[0], pb[1], pb[2]);
  }
  return b->GetID();
}
void FreeDet(Physics::BodyIdAlloc& a, JPH::BodyID id) {
  const uint32_t idx = id.GetIndex();
  if (idx >= a.used.size() || !a.used[idx] || a.seq[idx] != id.GetSequenceNumber())
    return;   // not ours, or a stale handle: the slot's body is someone else
  a.used[idx] = 0;
  if (idx < a.lowestFree) a.lowestFree = idx;
  if (FILE* tf = PhysTrace()) std::fprintf(tf, "R %x\n", id.GetIndexAndSequenceNumber());
}

// Joint friction, N*m, from the dimensionless JointDesc::friction.
//
// The authored number is a fraction of the torque this limb's OWN WEIGHT
// exerts about the anchor, because that is the only scale-free way to say
// "stiff". A fixed N*m would be a hinge on a finger and nothing at all on a
// torso, and every rig at a new kVoxelMeters would need it retuned — mass goes
// as the cube of the voxel size. At `frac` < 1 gravity still wins and the limb
// still falls; it just stops behaving like wet rope.
//
// GetInverseMassUnchecked, not GetInverseMass: mob limbs are KINEMATIC at
// spawn (they animate) and Jolt asserts on the checked accessor for those,
// even though the mass it would return is the correct one.
float FrictionTorque(const JPH::Body& child, JPH::RVec3Arg anchor, float frac) {
  if (frac <= 0.0f) return 0.0f;
  const JPH::MotionProperties* mp = child.GetMotionPropertiesUnchecked();
  if (!mp) return 0.0f;
  const float invMass = mp->GetInverseMassUnchecked();
  if (invMass <= 0.0f) return 0.0f;  // static: nothing to hold up
  const float lever = (float)(child.GetCenterOfMassPosition() - anchor).Length();
  const float g = CurrentTuning().physics.gravity;
  return frac * (1.0f / invMass) * g * lever;
}

}  // namespace

struct Physics::LayerImpls {
  BPLayerInterface bpInterface;
  ObjVsBPFilter objVsBp;
  ObjPairFilter objPair;
};

// ---- contact reporting (audio; DESIGN.md §12b "Built but not yet triggered")
//
// THREADING. Jolt calls OnContactAdded from its narrow-phase JOB THREADS, in
// parallel, during PhysicsSystem::Update. Everything this listener touches must
// therefore be either immutable for the duration of the step (`minSpeedVox`,
// latched by Step before Update) or under the lock. In particular it must NOT
// call CurrentTuning(): F5 replaces that global wholesale, which is the same
// hazard the audio thread has (DESIGN.md §12b threading contract).
//
// The mutex is not a hot path. The speed gate below runs BEFORE the lock and
// rejects the overwhelming majority of contacts — a pile of debris settling
// generates its contacts at millimetres per second — so the lock is taken only
// for events that will actually be voiced, a few times a second at worst.
struct Physics::ContactImpls final : public JPH::ContactListener {
  // Hard ceiling per step. A wall blasted into thirty pieces lands them all on
  // the same tick and there is no sound design in which thirty simultaneous
  // rock impacts is better than the loudest few; the consumer sorts by energy
  // and takes the top of them anyway. Dropping the tail here keeps the
  // allocation bounded inside a job thread, which is the part that matters.
  static constexpr size_t kMaxPerStep = 64;

  std::mutex mu;
  std::vector<ContactImpact> impacts;
  // A PLAYER's body (OWNED / EXEMPT layer) met by a LOOSE body (MOVING,
  // THROWN) — Physics::OwnedBodyImpacts. Its own list, so the audio and vessel
  // consumers of `impacts` see exactly what they saw before (W2-K), and its
  // own cap for the reason `impacts` has one.
  static constexpr size_t kMaxBodyPerStep = 32;
  std::vector<ContactImpact> bodyImpacts;
  float minSpeedVox = 0.0f;  // latched by Step; read-only during Update

  ContactImpls() {
    impacts.reserve(kMaxPerStep);
    bodyImpacts.reserve(kMaxBodyPerStep);
  }

  // ---- WHAT THE NARROW PHASE DID THIS STEP, for the watchdog line ---------
  // A slow Update used to report only "N active bodies", which cannot tell a
  // pair of 1024-box compounds grinding face to face from a fast body's swept
  // cast. These say how many manifolds and contact points the step produced,
  // body-vs-body and body-vs-static apart. Relaxed atomics: written from the
  // job threads, read on the game thread after Update returns.
  std::atomic<uint32_t> manifoldsDyn{0}, manifoldsStatic{0};
  std::atomic<uint32_t> pointsDyn{0}, pointsStatic{0};
  void CountManifold(const JPH::Body& b1, const JPH::Body& b2,
                     const JPH::ContactManifold& m) {
    const bool st = b1.IsStatic() || b2.IsStatic();
    (st ? manifoldsStatic : manifoldsDyn).fetch_add(1, std::memory_order_relaxed);
    (st ? pointsStatic : pointsDyn)
        .fetch_add((uint32_t)m.mRelativeContactPointsOn1.size(),
                   std::memory_order_relaxed);
  }
  void ResetCounts() {
    manifoldsDyn = 0;
    manifoldsStatic = 0;
    pointsDyn = 0;
    pointsStatic = 0;
  }
  void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2,
                          const JPH::ContactManifold& m,
                          JPH::ContactSettings&) override {
    CountManifold(b1, b2, m);
  }

  void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                      const JPH::ContactManifold& m,
                      JPH::ContactSettings&) override {
    CountManifold(b1, b2, m);
    // YOUR OWN BODY MUST NOT FIRE DEBRIS IMPACTS. The OWNED and EXEMPT layers
    // exist precisely to split a player's limbs out of contact handling, and
    // a player proxy is teleported onto its player every tick so its contacts
    // are an artifact of that, not of anything landing.
    //
    // ...BUT A LOOSE BODY HITTING IT IS A BLOW (W2-K). A thrown rock or a
    // falling log meeting a player's limb — or the player's CAPSULE, which
    // encloses the torso and is what a rock thrown at a player mostly meets —
    // goes to its OWN list (`bodyImpacts`), which only
    // MobSystem::ApplyContactDamage reads. The audio and vessel-break
    // consumers of `impacts` are unchanged, and so is the collision response
    // (this only REPORTS a contact the pair filter already allowed). Player
    // side vs player side (a body in its own capsule, two players) or vs the
    // terrain (a landing: fall damage) is still dropped.
    const JPH::ObjectLayer l1 = b1.GetObjectLayer(), l2 = b2.GetObjectLayer();
    const bool own1 = l1 == Layers::EXEMPT || Layers::IsOwned(l1) ||
                      Layers::IsPlayer(l1);
    const bool own2 = l2 == Layers::EXEMPT || Layers::IsOwned(l2) ||
                      Layers::IsPlayer(l2);
    if (own1 && own2) return;
    const bool toBody = own1 || own2;
    if (toBody && (own1 ? b2 : b1).IsStatic()) return;
    // Something has to be moving. Two statics never reach here, but a
    // static-vs-static pair would carry no speed anyway.
    if (b1.IsStatic() && b2.IsStatic()) return;

    const JPH::RVec3 p = m.GetWorldSpaceContactPointOn1(0);
    const JPH::Vec3 v1 =
        b1.IsStatic() ? JPH::Vec3::sZero() : b1.GetPointVelocity(p);
    const JPH::Vec3 v2 =
        b2.IsStatic() ? JPH::Vec3::sZero() : b2.GetPointVelocity(p);
    // Magnitude, not signed closing speed: this is a manifold that did not
    // exist last step, so the pair is approaching by construction, and taking
    // the absolute value means a sign convention flip in a future Jolt cannot
    // silently turn every impact off.
    const float speedM = std::abs((v1 - v2).Dot(m.mWorldSpaceNormal));
    const float speedVox = speedM / kVoxelMeters;
    if (speedVox < minSpeedVox) return;

    ContactImpact ci;
    ci.bodyA = b1.IsStatic() ? 0 : FromBodyID(b1.GetID());
    ci.bodyB = b2.IsStatic() ? 0 : FromBodyID(b2.GetID());
    ci.posVoxel = Vec3{(float)p.GetX(), (float)p.GetY(), (float)p.GetZ()} *
                  (1.0f / kVoxelMeters);
    ci.normal = Vec3{m.mWorldSpaceNormal.GetX(), m.mWorldSpaceNormal.GetY(),
                     m.mWorldSpaceNormal.GetZ()};
    ci.speedVoxPerSec = speedVox;
    ci.velAVoxPerSec = v1.Dot(m.mWorldSpaceNormal) / kVoxelMeters;
    ci.velBVoxPerSec = v2.Dot(m.mWorldSpaceNormal) / kVoxelMeters;

    // COLLECTED UNCAPPED, CAPPED AFTER THE STEP (Finish). The job threads
    // race here, so "the first 64 to arrive" was a function of scheduling --
    // on a busy step the kept set changed run to run, and bodyImpacts feeds
    // MobSystem::ApplyContactDamage. Finish sorts and keeps the strongest
    // under a total order, so the list is the same for any worker count.
    // kCollectCeiling bounds the allocation on a pathological step.
    std::lock_guard<std::mutex> lk(mu);
    std::vector<ContactImpact>& dst = toBody ? bodyImpacts : impacts;
    if (dst.size() < kCollectCeiling) dst.push_back(ci);
  }
  static constexpr size_t kCollectCeiling = 4096;
  // Game thread, after Update: strongest first, ties broken by every field,
  // then the per-step caps.
  void Finish() {
    auto order = [](const ContactImpact& a, const ContactImpact& b) {
      if (a.speedVoxPerSec != b.speedVoxPerSec) return a.speedVoxPerSec > b.speedVoxPerSec;
      if (a.bodyA != b.bodyA) return a.bodyA < b.bodyA;
      if (a.bodyB != b.bodyB) return a.bodyB < b.bodyB;
      if (a.posVoxel.x != b.posVoxel.x) return a.posVoxel.x < b.posVoxel.x;
      if (a.posVoxel.y != b.posVoxel.y) return a.posVoxel.y < b.posVoxel.y;
      if (a.posVoxel.z != b.posVoxel.z) return a.posVoxel.z < b.posVoxel.z;
      if (a.normal.x != b.normal.x) return a.normal.x < b.normal.x;
      if (a.normal.y != b.normal.y) return a.normal.y < b.normal.y;
      return a.normal.z < b.normal.z;
    };
    std::sort(impacts.begin(), impacts.end(), order);
    if (impacts.size() > kMaxPerStep) impacts.resize(kMaxPerStep);
    std::sort(bodyImpacts.begin(), bodyImpacts.end(), order);
    if (bodyImpacts.size() > kMaxBodyPerStep) bodyImpacts.resize(kMaxBodyPerStep);
  }
};

// Constraint bookkeeping: Jolt asserts if a constraint outlives either body,
// so RemoveBody tears down attached joints first (byBody index).
struct Physics::JointImpls {
  struct Entry {
    JPH::Ref<JPH::Constraint> constraint;
    uint64_t bodyA = 0, bodyB = 0;
    // Rest-frame bone direction (JointDesc::boneAxis) for ball joints, so
    // JointSwingAngle can measure against the same line the cone is built on
    // without re-deriving it. Zero for hinge/fixed.
    JPH::Vec3 boneAxis = JPH::Vec3::sZero();
    // Enough to build the constraint AGAIN against a replacement body
    // (ReplaceBody): the desc as given, and the anchor in each body's own
    // frame (metres) — the body that is NOT being replaced is the one that
    // still knows where the joint is.
    Physics::JointDesc desc;
    JPH::Vec3 anchorLocalA = JPH::Vec3::sZero();
    JPH::Vec3 anchorLocalB = JPH::Vec3::sZero();
    // A motored hinge's target (SetJointMotorTarget), re-applied when the
    // joint is rebuilt against a replacement body.
    float motorTarget = 0.0f;
  };
  std::unordered_map<uint64_t, Entry> joints;                 // handle -> entry
  std::unordered_map<uint64_t, std::vector<uint64_t>> byBody; // body -> joints
};

bool AntiTunnelOff(AntiTunnel part) {
  // 0 = all on. Bit 0 ccd, bit 1 lookahead, bit 2 clamp.
  static const unsigned mask = [] {
    const char* e = std::getenv("SANDVOX_NO_ANTITUNNEL");
    if (e == nullptr || *e == 0) return 0u;
    if (std::strcmp(e, "ccd") == 0) return 1u;
    if (std::strcmp(e, "lookahead") == 0) return 2u;
    if (std::strcmp(e, "clamp") == 0) return 4u;
    std::fprintf(stderr,
                 "SANDVOX_NO_ANTITUNNEL=%s: all three anti-tunnel mechanisms "
                 "OFF (pre-2026-09-12 behaviour)\n",
                 e);
    return 7u;
  }();
  return (mask & (1u << (unsigned)part)) != 0;
}

// ---- JOLT'S WORKER THREADS -------------------------------------------------
//
// Was a literal 2 on a 16-thread machine. A burning house is ~200 awake
// compounds, and one Update over them was 6-10 ms a tick on two workers
// (--burn-house, 2026-09-29) -- the largest CPU cost of the fire after the
// terrain meshes, and with up to four ticks a frame it is what tips a slow
// frame into the next. The narrow phase and the solver islands are exactly
// what Jolt spreads across workers.
//
// DETERMINISM: Jolt's contract (Docs/Architecture.md, "Deterministic
// Simulation") is same binary + same API call order; the worker count is not
// a condition -- contacts and constraints are sorted before solving. The one
// thread-order-dependent thing in this file is ContactImpls' capped impact
// list, which was already filled from two racing workers.
//
// Half the hardware threads less one (the main thread helps while it waits),
// clamped to [2, 7]: the GPU driver, audio and the voxel mirror also want
// cores. SANDVOX_PHYS_THREADS=<n> is the A/B arm in one binary.
static int PhysicsWorkerThreads() {
  if (const char* e = std::getenv("SANDVOX_PHYS_THREADS")) {
    const int n = std::atoi(e);
    if (n >= 1) return std::min(n, 32);
  }
  const int hw = (int)std::thread::hardware_concurrency();
  return std::clamp(hw / 2 - 1, 2, 7);
}
constexpr JPH::uint kMaxBodyPairs = 16384;
constexpr JPH::uint kMaxContactConstraints = 16384;

Physics::Physics() = default;
Physics::~Physics() { Shutdown(); }

bool Physics::Init() {
  JPH::RegisterDefaultAllocator();
  if (!JPH::Factory::sInstance) {
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
  }
  // 64 MB of step scratch: a burning house is ~200 awake compounds over ~1,500
  // manifolds, and TempAllocatorImpl ABORTS when a step outgrows it.
  tempAlloc_ = std::make_unique<JPH::TempAllocatorImpl>(64 * 1024 * 1024);
  jobs_ = std::make_unique<JPH::JobSystemThreadPool>(
      JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, PhysicsWorkerThreads());
  layers_ = std::make_unique<LayerImpls>();
  joints_ = std::make_unique<JointImpls>();
  contacts_ = std::make_unique<ContactImpls>();

  system_ = std::make_unique<JPH::PhysicsSystem>();
  // Pair and contact-constraint capacity. These were 4096 / 2048, and a
  // burning house ran at ~1,500 manifolds (--burn-house, 2026-09-29): past the
  // ceiling Jolt DROPS contacts, bodies sink into each other and the ground,
  // and the pile never comes to rest. Memory is ~100 bytes a slot.
  system_->Init(4096 /*max bodies*/, 0, kMaxBodyPairs, kMaxContactConstraints,
                layers_->bpInterface, layers_->objVsBp, layers_->objPair);
  system_->SetContactListener(contacts_.get());
  system_->SetGravity(JPH::Vec3(0, -CurrentTuning().physics.gravity, 0));
  return true;
}

void Physics::ResetBodyIdHistory() {
  for (size_t i = 0; i < ids_.used.size(); i++)
    if (!ids_.used[i]) ids_.seq[i] = 0;
}

void Physics::Shutdown() {
  ids_ = BodyIdAlloc{};
  pendingRelease_.clear();
  playerBodies_.clear();
  motionRun_.clear();
  joints_.reset();  // constraint refs drop before the system that owns bodies
  system_.reset();  // ...and the system drops before the listener it points at
  contacts_.reset();
  layers_.reset();
  jobs_.reset();
  tempAlloc_.reset();
}

const std::vector<Physics::ContactImpact>& Physics::ContactImpacts() const {
  static const std::vector<ContactImpact> kNone;
  return contacts_ ? contacts_->impacts : kNone;
}

const std::vector<Physics::ContactImpact>& Physics::OwnedBodyImpacts() const {
  static const std::vector<ContactImpact> kNone;
  return contacts_ ? contacts_->bodyImpacts : kNone;
}

void Physics::TrackMotionRuns(float dt) {
  const uint32_t n =
      system_ ? system_->GetNumActiveBodies(JPH::EBodyType::RigidBody) : 0;
  const JPH::BodyID* active =
      n ? system_->GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody) : nullptr;
  if (!active) {
    motionRun_.clear();
    return;
  }
  // Rebuilt, not patched: a body missing from the active list (asleep,
  // removed) or under the rest bar has no run. Per-key and order-free, so
  // the job threads' active-list order cannot reach the result.
  std::unordered_map<uint64_t, float> next;
  next.reserve(n);
  const JPH::BodyInterface& bi = system_->GetBodyInterfaceNoLock();
  for (uint32_t i = 0; i < n; i++) {
    const JPH::BodyID id = active[i];
    const float speedVox = bi.GetLinearVelocity(id).Length() / kVoxelMeters;
    if (!(speedVox >= kMotionRunRestVox)) continue;
    const uint64_t h = FromBodyID(id);
    auto it = motionRun_.find(h);
    next[h] = (it == motionRun_.end() ? 0.0f : it->second) + speedVox * dt;
  }
  motionRun_.swap(next);
}

float Physics::MotionRunVox(uint64_t handle) const {
  auto it = motionRun_.find(handle);
  return it == motionRun_.end() ? 0.0f : it->second;
}

void Physics::SetContactReportSpeed(float voxPerSec) {
  // Below zero would report every resting contact in the world; a caller that
  // wants the listener off passes a huge number, not a negative one.
  if (contacts_) contacts_->minSpeedVox = std::max(0.0f, voxPerSec);
}

// ---- THE FP TRAP INSIDE JOLT'S OWN CLAMP (crash.log, 2026-09-12 + 09-13) ---
//
// Jolt's worker threads run with the SSE invalid / divide-by-zero / OVERFLOW
// exceptions UNMASKED -- JobSystemThreadPool::ThreadMain opens with
// `FPExceptionsEnable`, and Jolt.cmake turns JPH_FLOATING_POINT_EXCEPTIONS_-
// ENABLED on for Release as well as Debug -- so the first float op that
// overflows on a job thread kills the process with 0xC0000091 and no message.
// Twice, three minutes and nine hours after felled trees first became single
// rigid bodies, that op was
//
//   JPH::PhysicsSystem::JobIntegrateVelocity +0x1C8  (PhysicsSystem.cpp:1535)
//
// which disassembles to `mulss %xmm2,%xmm2` on `0x10(mp)` -- the x component
// of MotionProperties::mAngularVelocity, squared inside ClampAngularVelocity.
// THE CLAMP THAT EXISTS TO STOP EXACTLY THIS IS WHAT DIED: `LengthSq()`
// overflows before the comparison it feeds. The LINEAR clamp four
// instructions earlier had already passed, so linear velocity was fine;
// angular-only garbage is the signature of `omega += invI * (r x J)`, not of a
// body that was merely thrown hard.
//
// WHERE IT CANNOT HAVE COME FROM, measured rather than reasoned. Every
// BodyInterface velocity write in Jolt goes through SetLinearVelocityClamped /
// SetAngularVelocityClamped (BodyInterface.cpp:561-665), so nothing this class
// hands Jolt can be the source. SANDVOX_PHYS_FAULT below writes 1e30 rad/s
// straight at a live body to prove it: the value never survives to the next
// line, because the clamp runs on the GAME thread, where the overflow is
// MASKED -- len_sq becomes +inf, `max / sqrt(inf)` becomes 0, and the spin is
// zeroed. Jolt's clamp handles the overflow correctly. It is only lethal on a
// job thread, and only because the exception is unmasked there.
//
// So the fix is in three parts, and the first is the one that matters:
//
//  1. CMakeLists turns FLOATING_POINT_EXCEPTIONS_ENABLED OFF. The identical
//     numeric situation then resolves the way the main-thread injection above
//     demonstrates: the body's spin is zeroed and the frame continues.
//  2. GuardBodyInertia, at birth, closes the one mechanism that can multiply a
//     bounded spin by 4e17 inside a SINGLE step -- a principal moment that
//     came back at (or below) zero from an ill-conditioned eigendecomposition.
//     MotionProperties::SetMassProperties takes `diagonal.Reciprocal()`
//     whenever the diagonal as a VECTOR is not near zero, so one bad component
//     becomes an inverse inertia of 1e20 and the next contact impulse is the
//     end of the body. Nothing this engine builds can legitimately exceed
//     kMaxInvInertia: the smallest is a single micro voxel at physScale 8,
//     floored to the 0.05 kg minimum mass, at ~7.7e5.
//  3. SweepInsaneVelocities + VelocityIsSane catch the case Jolt's clamp
//     silently PASSES: a NaN. `NaN > Square(max)` is false, so a NaN velocity
//     goes through SetAngularVelocityClamped untouched, never traps (squaring
//     a quiet NaN raises nothing), and quietly moves the body to a NaN
//     position forever. That one has no self-healing path at all, so it is
//     caught here and named.
//
// Every check below is written to be overflow-safe itself: components are
// compared, never squared. Reading a float and calling isfinite on it is not
// an FP operation and cannot trap.
namespace {
// 1/(kg m^2). See (2) above for where the three orders of margin come from.
constexpr float kMaxInvInertia = 1.0e9f;
// rad/s and m/s. Jolt clamps to 47.1 and 500 respectively on every integrate,
// so a body an order of magnitude past either is not physics, it is a bug.
constexpr float kInsaneSpin = 1.0e4f;
constexpr float kInsaneSpeed = 1.0e5f;

// ---- THE CEILINGS A BODY OF THIS WORLD IS BORN WITH ------------------------
//
// Jolt's defaults are 500 m/s and 47.124 rad/s, and one of those two numbers
// is wrong for this engine by a factor of fifteen.
//
// 500 m/s is 8.3 METRES of travel in one 60 Hz step, which is 83 voxels. Every
// dynamic body thinner than kDiscreteMinExtentVox is EMotionQuality::LinearCast
// (the long note below CreateDebrisBody), so a step that long is a shape cast
// of a compound of up to kColliderBoxBudget boxes swept 83 voxels through
// marching-cubes terrain, and the sweep happens for every such body every step. Nothing in this world legitimately
// travels at 500 m/s: the fastest legal values are physics.explosionMaxSpeed
// (30 m/s), ragdoll.maxLaunchSpeed (14 m/s) and terminal velocity for a fall
// the height of the whole residency window (sqrt(2 g * 51 m) = 32 m/s). 80 m/s
// is more than twice the fastest of those and a sixth of Jolt's, which is the
// point: it is a SAFETY ceiling, not a gameplay one, and nothing should ever
// touch it.
//
// The ANGULAR ceiling is deliberately left at Jolt's own value. 47.1 rad/s is
// 7.5 revolutions a second, which looks like a spasm on a severed arm, but it
// is not absurd for a struck pebble and a smaller cap would make small rolling
// spheres skid instead of roll (omega = v/r: a one-voxel ball rolling at 5 m/s
// is at 100 rad/s legitimately). The problem the owner reported is not that a
// limb reached this number; it is that it SAT on it. That is what
// SweepRunawayRigs is for, and it is stated against the ceiling rather than
// against a magic rad/s so the two cannot drift apart.
constexpr float kBodyMaxSpeedMS = 80.0f;
constexpr float kBodyMaxSpinRad = 0.25f * 3.14159265358979f * 60.0f;

// A body is "hot" at 90% of its own ceiling. Not 100%: Jolt clamps TO the
// ceiling, so a body the solver is driving reads exactly the ceiling, but a
// body decelerating past it reads just under, and the net wants both.
constexpr float kRunawayFrac = 0.9f;
// Steps at the ceiling before the net slows the body, and before it cuts the
// joints driving it. At 60 Hz: a fifth of a second, then (because the counter
// falls by one for every quiet step) about a second and a half of a body that
// keeps coming back. A piece genuinely thrown that hard is under the first
// bar long before it reaches it.
constexpr int kRunawayDampSteps = 12;
constexpr int kRunawayCutSteps = 45;
// What "slow it down" means. Hard enough that one application is visible in
// the next step's reading, soft enough that it reads as the piece losing its
// fight rather than as a teleport to rest.
constexpr float kRunawayDampScale = 0.2f;
// One Update longer than this is the symptom the owner actually reported --
// "the framerate plummeted and the whole game froze". Report-only: wall clock
// may not change what the simulation does, but it may say what it saw.
constexpr double kStepWatchdogMs = 100.0;

// ---- WHAT A BIG BODY'S COLLIDER MAY COST (2026-09-22) ------------------------
//
// The owner report: "cut a tree down, or split a big body into several, and it
// runs at 3 fps for a few seconds". Measured with `--fell-tree` on the baked
// birch, split twice: ONE Jolt Update 100-138 ms over four bodies, and 1.6 ms
// with SANDVOX_NO_ANTITUNNEL=ccd in the same binary (turning off enhanced
// internal-edge removal changed nothing). The contacts were never the cost --
// 17 manifolds, 44 points on a 100 ms step. The LINEAR CAST was, twice over:
//
//  * Jolt casts when a step moves the body more than 0.75 x its shape's INNER
//    RADIUS, and a compound's inner radius is its SMALLEST sub-shape's. One
//    leaf left as a 1x1x1 box made a 7 m tree cast on every step it moved
//    faster than about 1 m/s -- i.e. for the whole of its fall.
//  * the cast sweeps every sub-shape, and a crown was 1024 of them.
//
// kDiscreteMinExtentVox answers the first: a body whose THINNEST extent is at
// least this many world voxels cannot cross a terrain sheet in one step at any
// speed a falling or thrown body reaches here -- it would have to move half its
// thickness, 12 voxels, in one 30 Hz step, which is 36 m/s, and the fastest
// legal body is a fall the height of the whole window at 32 m/s (the ceilings
// note above). So it steps discretely and never casts. Its thin parts (a twig,
// a branch) can still graze through a triangle for a step; the body does not,
// because its thick parts are what the discrete contacts stop. Everything
// thinner -- limbs, rubble, a plank, a trunk section -- keeps LinearCast
// exactly as before. kColliderBoxBudget answers the second, and the collider
// builder's note says how a body over it is re-merged.
constexpr float kDiscreteMinExtentVox = 24.0f;
constexpr int kColliderBoxBudget = 256;
// ...AND A SMALL BODY GETS A SMALL SHARE OF IT (2026-09-29). The narrow phase
// and the LinearCast both pay per sub-shape, and a burnt plank is exactly the
// shape the greedy merge does worst on: a 600-voxel piece of charred roof came
// out as 130+ boxes, a 2,000-voxel one at the full 256. Two hundred of them in
// one pile made single Jolt steps of 20-55 ms (--burn-house with
// SANDVOX_BURN_SEED=low: the roof comes down in pieces). Measured in one
// binary on that scene: a flat 64 took the step p99 6.5 -> 3.4 ms and the max
// 10.8 -> 4.5; turning the casts off entirely did no better (2.7 / 4.7).
// The budget is therefore a box per 16 voxels, floored at 32 and capped at the
// ceiling, so a tree (28k voxels) keeps its 256 and rubble keeps its shape to
// within the coarse lattice the over-budget path already uses.
// SANDVOX_BOX_BUDGET=<n> replaces the rule with a flat <n> (the A/B arm).
constexpr int kColliderBoxesPerVoxel = 16;
constexpr int kColliderBoxFloor = 32;
static int ColliderBoxBudgetFor(size_t voxels) {
  static const int flat = [] {
    const char* e = std::getenv("SANDVOX_BOX_BUDGET");
    const int n = e ? std::atoi(e) : 0;
    return n >= 4 ? n : 0;
  }();
  if (flat) return flat;
  return std::clamp((int)std::min<size_t>(voxels / kColliderBoxesPerVoxel,
                                          (size_t)kColliderBoxBudget),
                    kColliderBoxFloor, kColliderBoxBudget);
}
// A greedy box at least this big (and at least 2 voxels on every side) is
// kept exactly when a body is over budget; everything smaller goes coarse.
constexpr int kFatBoxMinVoxels = 8;

// True when every component is finite and under `limit`. No multiplies, so
// this is safe to call on the garbage it is looking for.
bool ComponentsUnder(JPH::Vec3Arg v, float limit) {
  for (uint32_t i = 0; i < 3; i++) {
    const float c = v[i];
    if (!std::isfinite(c) || c > limit || c < -limit) return false;
  }
  return true;
}
}  // namespace

void Physics::GuardBodyInertia(uint32_t bodyIndexAndSeq, const char* what) {
  if (!system_) return;
  JPH::BodyLockWrite lock(system_->GetBodyLockInterface(),
                          JPH::BodyID(bodyIndexAndSeq));
  if (!lock.Succeeded()) return;
  JPH::Body& body = lock.GetBody();
  if (!body.IsDynamic()) return;
  JPH::MotionProperties* mp = body.GetMotionPropertiesUnchecked();
  if (!mp) return;
  const JPH::Vec3 d = mp->GetInverseInertiaDiagonal();
  if (ComponentsUnder(d, kMaxInvInertia)) return;  // the normal path
  // A zero component is Jolt's own "this axis does not rotate" and is fine;
  // only a component that is negative, non-finite or absurdly large is the
  // artefact, and each is floored independently so a merely thin body keeps
  // the two good axes it decomposed correctly.
  float f[3];
  for (uint32_t i = 0; i < 3; i++) {
    const float c = d[i];
    f[i] = (!std::isfinite(c) || c < 0.0f || c > kMaxInvInertia)
               ? kMaxInvInertia
               : c;
  }
  mp->SetInverseInertia(JPH::Vec3(f[0], f[1], f[2]), mp->GetInertiaRotation());
  if (insaneReports_ < 8) {
    insaneReports_++;
    std::fprintf(stderr,
                 "[phys] %s body %u: inverse inertia (%g, %g, %g) is not "
                 "physical (invMass %g) -> floored to (%g, %g, %g). See the "
                 "FP-trap note in phys/physics.cpp.\n",
                 what, (unsigned)JPH::BodyID(bodyIndexAndSeq).GetIndex(),
                 (double)d.GetX(), (double)d.GetY(), (double)d.GetZ(),
                 (double)mp->GetInverseMassUnchecked(), (double)f[0],
                 (double)f[1], (double)f[2]);
  }
}

void Physics::InjectPhysFault() {
  if (!system_) return;
  // A SAFETY NET NOBODY HAS SEEN WORK IS NOT A SAFETY NET. Every gate this
  // engine has runs the checks below and none has ever tripped one (correctly
  // -- they are looking for garbage), so the ACTION half would ship
  // unexercised. SANDVOX_PHYS_FAULT=<n> writes a NaN spin straight at the
  // n-th step's first active body through Jolt's own clamped setter, and the
  // sweep immediately below has to name it and zero it.
  //
  // It is a NaN and not a huge number ON PURPOSE, and the first version of
  // this injector is why the note above says what it says: 1e30 rad/s was
  // eaten by SetAngularVelocityClamped before the sweep ever saw it, which is
  // how we learned that no value this class writes can reach Jolt's
  // integrator un-clamped. A NaN is the one thing that clamp lets through.
  // Unset (the normal case) reads one cached int.
  //
  // `SANDVOX_PHYS_FAULT=spin:<n>` is the other half: it writes 1e30 rad/s
  // STRAIGHT INTO MotionProperties, under the body lock and past every clamp,
  // which is the state both crashes died in.
  //
  // AND MASKING THE EXCEPTION IS NOT, BY ITSELF, ENOUGH -- that arm is how we
  // know. Run past the sweep (InjectPhysFault called AFTER it) on the masked
  // build, a `corpse-intact` gate that takes 5 s did not crash and did not
  // finish either: killed at 8 minutes, crash.log untouched. Jolt's clamp
  // lives in JobIntegrateVelocity, which is the LAST job of the step, so
  // collision detection, constraint setup and the velocity solve all run
  // first with omega = 1e30 in hand -- `v + omega x r` is astronomical at
  // every contact and the step never comes back. Masking turns the kill into
  // a hang; the sweep below is what turns it into a log line. Both halves are
  // the fix, which is why this injector runs BEFORE the sweep: the arm that
  // matters is the one the net is allowed to see.
  //
  // `SANDVOX_PHYS_FAULT=pos:<n>` is the THIRD arm, and it exists because the
  // repair in SweepInsaneVelocities used to be a printf. A NaN POSITION is
  // what actually wedges the process -- the quadtree gets a NaN AABB and the
  // step stops coming back -- and neither of the two arms above can produce
  // one directly: the velocity checks catch their garbage before it ever
  // integrates, which is exactly what they are for. So this one writes the end
  // state straight into the body, past everything, and the sweep has to put it
  // back inside a finite world instead of narrating it.
  static const char* kFaultEnv = std::getenv("SANDVOX_PHYS_FAULT");
  static const bool kFaultRaw =
      kFaultEnv && std::strncmp(kFaultEnv, "spin:", 5) == 0;
  static const bool kFaultPos =
      kFaultEnv && std::strncmp(kFaultEnv, "pos:", 4) == 0;
  static const int kFaultAt =
      kFaultEnv ? std::atoi(kFaultRaw ? kFaultEnv + 5
                                      : (kFaultPos ? kFaultEnv + 4 : kFaultEnv))
                : 0;
  if (kFaultAt > 0 && ++faultStep_ == kFaultAt) {
    const uint32_t na = system_->GetNumActiveBodies(JPH::EBodyType::RigidBody);
    const JPH::BodyID* ab =
        system_->GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody);
    if (na > 0 && ab) {
      std::fprintf(stderr,
                   "[phys] SANDVOX_PHYS_FAULT: injecting %s into body %u on "
                   "step %d\n",
                   kFaultPos ? "a NaN POSITION past every clamp"
                             : (kFaultRaw ? "1e30 rad/s past every clamp"
                                          : "a NaN spin"),
                   (unsigned)ab[0].GetIndex(), kFaultAt);
      if (kFaultPos) {
        // Straight at the body, under the write lock, with no notification to
        // the broadphase: this is the state a body ends up in when a solver
        // integrates a velocity nothing caught, and the sweep's repair is what
        // has to notice it.
        JPH::BodyLockWrite lock(system_->GetBodyLockInterface(), ab[0]);
        if (lock.Succeeded())
          lock.GetBody().SetPositionAndRotationInternal(
              JPH::RVec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f),
              JPH::Quat::sIdentity(), false);
      } else if (kFaultRaw) {
        JPH::BodyLockWrite lock(system_->GetBodyLockInterface(), ab[0]);
        if (lock.Succeeded())
          if (JPH::MotionProperties* mp =
                  lock.GetBody().GetMotionPropertiesUnchecked())
            mp->SetAngularVelocity(JPH::Vec3(1.0e30f, 0.0f, 0.0f));
      } else {
        system_->GetBodyInterfaceNoLock().SetAngularVelocity(
            ab[0],
            JPH::Vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f));
      }
    } else {
      std::fprintf(stderr,
                   "[phys] SANDVOX_PHYS_FAULT: no active body on step %d\n",
                   kFaultAt);
      faultStep_--;  // try again next step
    }
  }
}

void Physics::SweepInsaneVelocities() {
  if (!system_) return;
  // Safe unlocked: this runs on the game thread, outside Update, so no job is
  // touching the active list (the same reason WakeNear may walk bodies here).
  const uint32_t n = system_->GetNumActiveBodies(JPH::EBodyType::RigidBody);
  if (n == 0) return;
  const JPH::BodyID* active =
      system_->GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody);
  if (!active) return;
  JPH::BodyInterface& bi = system_->GetBodyInterfaceNoLock();
  for (uint32_t i = 0; i < n; i++) {
    const JPH::BodyID id = active[i];
    const JPH::Vec3 w = bi.GetAngularVelocity(id);
    const JPH::Vec3 v = bi.GetLinearVelocity(id);
    const bool badW = !ComponentsUnder(w, kInsaneSpin);
    const bool badV = !ComponentsUnder(v, kInsaneSpeed);
    const JPH::RVec3 p = bi.GetPosition(id);
    const JPH::Quat r = bi.GetRotation(id);
    // A NaN TRANSFORM IS WHAT THE FREEZE IS MADE OF: the quadtree gets a NaN
    // AABB, every overlap test against it answers neither yes nor no, and the
    // step stops coming back (SANDVOX_PHYS_FAULT=spin:1 past the sweep: an
    // 8-minute `corpse-intact` that normally takes 5 s, crash.log untouched).
    //
    // This USED TO BE REPORT-ONLY, on the argument that the position is
    // already gone and the real repair is upstream. Both halves of that are
    // true and it is still the wrong call, because the two failures are not
    // the same size: the body being wrong is one dropped limb, and the
    // broadphase stalling is the whole process wedged for minutes with a
    // window that will not close. So the body is put back inside a FINITE
    // world -- not a correct place, there is no correct place, just a defined
    // one -- zeroed, and put to sleep. The ordinary debris cull collects it
    // from there like any other piece that ended up somewhere silly.
    //
    // A NaN QUATERNION does the same damage by the same route (the rotated
    // AABB), and nothing was checking for one, so it is replaced with the
    // identity here rather than left to poison the transform that was just
    // repaired.
    const bool badP = !ComponentsUnder(JPH::Vec3(p), 1.0e9f);
    const bool badR = !std::isfinite(r.GetX()) || !std::isfinite(r.GetY()) ||
                      !std::isfinite(r.GetZ()) || !std::isfinite(r.GetW()) ||
                      !(r.LengthSq() > 1.0e-6f);
    if (badP || badR) {
      float f[3];
      for (uint32_t a = 0; a < 3; a++) {
        const float cc = JPH::Vec3(p)[a];
        f[a] = std::isfinite(cc) ? std::max(-1.0e6f, std::min(1.0e6f, cc)) : 0.0f;
      }
      bi.SetPositionAndRotation(
          id, JPH::RVec3(f[0], f[1], f[2]),
          badR ? JPH::Quat::sIdentity() : r.Normalized(),
          JPH::EActivation::DontActivate);
      bi.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
      runaway_.repaired++;
      if (insaneReports_ < 8) {
        insaneReports_++;
        std::fprintf(stderr,
                     "[phys] body %u had an unusable TRANSFORM (%g, %g, %g) m, "
                     "quat (%g, %g, %g, %g) -- the broadphase would have "
                     "stalled on it, so it was put back at (%g, %g, %g) and "
                     "stopped. Find what got past the velocity checks above "
                     "this line; see the FP-trap note in phys/physics.cpp.\n",
                     (unsigned)id.GetIndex(), (double)p.GetX(),
                     (double)p.GetY(), (double)p.GetZ(), (double)r.GetX(),
                     (double)r.GetY(), (double)r.GetZ(), (double)r.GetW(),
                     (double)f[0], (double)f[1], (double)f[2]);
      }
      hotSteps_.erase(id.GetIndex());
      continue;  // nothing further to say about a body that has been reset
    }
    if (!badW && !badV) continue;
    if (insaneReports_ < 8) {
      insaneReports_++;
      std::fprintf(stderr,
                   "[phys] body %u at (%.1f, %.1f, %.1f) vox carried %s "
                   "velocity into the step: lin (%g, %g, %g) m/s, ang "
                   "(%g, %g, %g) rad/s -> zeroed. See the FP-trap note in "
                   "phys/physics.cpp.\n",
                   (unsigned)id.GetIndex(), (double)p.GetX() / kVoxelMeters,
                   (double)p.GetY() / kVoxelMeters,
                   (double)p.GetZ() / kVoxelMeters,
                   badW ? (badV ? "an unusable" : "an unusable angular")
                        : "an unusable linear",
                   (double)v.GetX(), (double)v.GetY(), (double)v.GetZ(),
                   (double)w.GetX(), (double)w.GetY(), (double)w.GetZ());
    }
    bi.SetLinearAndAngularVelocity(id, badV ? JPH::Vec3::sZero() : v,
                                   badW ? JPH::Vec3::sZero() : w);
  }
}

float Physics::MaxBodySpeedVox() { return kBodyMaxSpeedMS / kVoxelMeters; }
float Physics::MaxBodySpinRad() { return kBodyMaxSpinRad; }
int Physics::ColliderBoxBudget() { return kColliderBoxBudget; }
float Physics::DiscreteMinExtentVox() { return kDiscreteMinExtentVox; }

// ---- A CORPSE IN ARMOUR IS NOT ALLOWED TO BE A MOTOR ------------------------
//
// Owner report, 2026-09-13: a player-model NPC in a full set of armour, killed
// after its head had been cut off. "The body spasmed out and flew in a billion
// directions, all of his limbs moving dramatically and flying everywhere while
// still technically being attached. The framerate plummeted and the whole game
// froze for several minutes; closing it took a minute or two." No crash.log.
//
// WHY NOTHING ABOVE CATCHES IT. Every net in this file so far is looking for
// GARBAGE -- a NaN, an inverse inertia of 1e20, a spin of 1e30 -- and this is
// not garbage. `corpse-armor` reproduces it and measures the peak at 47.12
// rad/s, which is Jolt's own mMaxAngularVelocity to five figures: the solver
// asked for more and the clamp gave it exactly the ceiling. Everything about
// that number is legal. kInsaneSpin is 1e4, three orders above it, and
// correctly so -- lowering kInsaneSpin would not describe this failure, it
// would just move the arbitrary line.
//
// WHAT IS ACTUALLY WRONG is not the value but its PERSISTENCE. A limb thrown
// hard is fast for a few steps and then slower. A limb at the ceiling on step
// after step is not being thrown, it is being DRIVEN, and in a dressed corpse
// there is an obvious motor: every worn shell is its own Jolt body, FIXED to
// the limb it wraps (Mob::AppendWornShell) and geometrically INSIDE it, so a
// dressed corpse doubles the bodies, doubles the constraints, and puts an iron
// mass ratio across every one of the new ones. A stiff constraint between
// deeply overlapping bodies is the textbook way to make a sequential-impulse
// solver gain energy, and the joints Mob::Die deliberately leaves on the
// corpse are what spread it from one limb to all of them -- which is exactly
// what "flying everywhere while still technically being attached" describes.
//
// AND WHY IT COSTS MINUTES RATHER THAN LOOKING SILLY. Two multipliers, both
// already documented elsewhere in this file. Every dynamic body is
// EMotionQuality::LinearCast, so once the linear velocity is large the step is
// a swept compound-shape cast against marching-cubes terrain, per body, per
// step (that is the other half of why the ceiling above came down from Jolt's
// 500 m/s). And the contact solve sees `v + omega x r` at every contact point,
// which is the same term the 46e3848 note identifies as what makes a step
// stop coming back.
//
// THE NET, in two stages, and the escalation is the point:
//
//  1. A body at 90% of its own ceiling has its hot counter raised by one; a
//     body under it has it lowered by one. Past kRunawayDampSteps the net
//     scales both velocities by kRunawayDampScale every step. A piece that was
//     merely thrown hard decays out of the counter and is never touched.
//  2. A body that keeps coming back anyway reaches kRunawayCutSteps, and then
//     the JOINTS come off. That is the motor, and it is also the only thing
//     the damping cannot reach: slowing a body that a constraint is feeding
//     just means the constraint feeds it again next step. Cut, zeroed, and
//     deactivated -- the corpse comes apart, which is a far better outcome
//     than the game stopping, and it is the outcome the player was going to
//     get anyway one second later.
//
// Both stages are counted in RunawayProbe so a gate can assert on them, and
// both are named on stderr the first few times they fire. Zero cost when
// nothing is wrong: one length compare per active body and an empty map.
void Physics::SweepRunawayRigs() {
  // THE A/B ARM, IN ONE BINARY, for the same reason SANDVOX_NO_ANTITUNNEL
  // exists a few hundred lines up: this changes where bodies end up, so the
  // next unexplained settling number is going to want to ask "was it this?",
  // and a differential measured across two BUILDS measures the builds. Off =
  // the pre-2026-09-13 behaviour, ceilings included (the ceiling is applied at
  // body birth, so it is listed here as a reminder that this switch does NOT
  // undo it -- use a fresh world to A/B that half).
  static const bool kOff = [] {
    const char* e = std::getenv("SANDVOX_NO_RUNAWAY_NET");
    return e != nullptr && e[0] != '0';
  }();
  runaway_.hot = 0;
  if (kOff || !system_) return;
  const uint32_t n = system_->GetNumActiveBodies(JPH::EBodyType::RigidBody);
  if (n == 0) {
    hotSteps_.clear();
    return;
  }
  const JPH::BodyID* active =
      system_->GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody);
  if (!active) return;
  JPH::BodyInterface& bi = system_->GetBodyInterfaceNoLock();
  const float spinBar = kRunawayFrac * kBodyMaxSpinRad;
  const float speedBar = kRunawayFrac * kBodyMaxSpeedMS;
  // DEFERRED, because DeactivateBody and DestroyJoint both reach into the very
  // list being walked: GetActiveBodiesUnsafe hands out a pointer INTO the
  // active array, and deactivating a body swaps the tail into its slot. The
  // damping below is safe inline (it writes velocity and nothing else).
  std::vector<JPH::BodyID> toCut;
  for (uint32_t i = 0; i < n; i++) {
    const JPH::BodyID id = active[i];
    const JPH::Vec3 w = bi.GetAngularVelocity(id);
    const JPH::Vec3 v = bi.GetLinearVelocity(id);
    // Safe to square: SweepInsaneVelocities ran first and zeroed anything that
    // could overflow here, which is the whole reason it runs first.
    const float spin = w.Length(), speed = v.Length();
    runaway_.peakSpinRad = std::max(runaway_.peakSpinRad, spin);
    runaway_.peakSpeedVox =
        std::max(runaway_.peakSpeedVox, speed / kVoxelMeters);
    const uint32_t idx = id.GetIndex();
    if (spin < spinBar && speed < speedBar) {
      auto it = hotSteps_.find(idx);
      if (it == hotSteps_.end()) continue;
      if (it->second <= 1)
        hotSteps_.erase(it);
      else
        it->second--;
      continue;
    }
    runaway_.hot++;
    // A KINEMATIC BODY IS NOT SIMULATED and therefore cannot be driven: a
    // living mob's limbs are posed by the animation and MoveKinematicBody
    // hands Jolt the velocity that move implies, so a fast clip or a respawn
    // snap can read at the ceiling for a tick or two entirely correctly. The
    // solver has no way to feed one, so there is nothing here to find and
    // every intervention would be wrong.
    //
    // A live RAGDOLL is dynamic and jointed and so can still reach the cut,
    // and that is deliberate: getting there takes 45 steps of being
    // re-energised to the ceiling AFTER the damping has been cutting it to 20%
    // every step, which is a rig nothing was going to recover gracefully.
    // Losing a limb beats losing the frame budget.
    if (bi.GetMotionType(id) != JPH::EMotionType::Dynamic) {
      hotSteps_.erase(idx);
      continue;
    }
    // ONLY A RIG, and this restriction is not caution, it is the diagnosis.
    // The motor in the owner's report is a CONSTRAINT GRAPH -- a dressed
    // corpse's fixed straps and its joints -- and a body with nothing attached
    // to it has nobody to feed it. Meanwhile the thing that most often sits at
    // the angular ceiling in ordinary play is a small rolling ball, which is
    // there LEGITIMATELY: omega = v/r, so a one-voxel sphere rolling at 5 m/s
    // is at 100 rad/s and Jolt is already clipping it to 47.1 every step. A
    // net that did not check this would damp every pebble in the world to a
    // skid and then put it to sleep mid-roll, which is a far more visible bug
    // than the one being fixed.
    const uint64_t h = FromBodyID(id);
    if (!joints_ || joints_->byBody.find(h) == joints_->byBody.end()) {
      hotSteps_.erase(idx);
      continue;
    }
    uint16_t& steps = hotSteps_[idx];
    if (steps < 0xFFFFu) steps++;
    if (steps >= (uint16_t)kRunawayCutSteps) {
      toCut.push_back(id);
      continue;
    }
    if (steps < (uint16_t)kRunawayDampSteps) continue;
    bi.SetLinearAndAngularVelocity(id, v * kRunawayDampScale,
                                   w * kRunawayDampScale);
    runaway_.damped++;
    // ONCE PER EPISODE, not once per step. This fires every step for as long
    // as the body keeps coming back, and eight identical lines about body 51
    // is not eight times the information -- it is the global report budget
    // spent on one body, so the SECOND body to go wrong says nothing. The
    // escalation to the cut below is the line that reports the outcome.
    if (steps == (uint16_t)kRunawayDampSteps && runawayReports_ < 8) {
      runawayReports_++;
      const JPH::RVec3 p = bi.GetPosition(id);
      std::fprintf(stderr,
                   "[phys] body %u at (%.1f, %.1f, %.1f) vox has been at its "
                   "velocity ceiling for %u steps (lin %.1f vox/s of %.0f, ang "
                   "%.1f rad/s of %.1f) -- slowed to %.0f%%. A body that stays "
                   "here is being DRIVEN; see the runaway note in "
                   "phys/physics.cpp.\n",
                   (unsigned)idx, (double)p.GetX() / kVoxelMeters,
                   (double)p.GetY() / kVoxelMeters,
                   (double)p.GetZ() / kVoxelMeters, (unsigned)steps,
                   (double)(speed / kVoxelMeters), (double)MaxBodySpeedVox(),
                   (double)spin, (double)kBodyMaxSpinRad,
                   (double)(kRunawayDampScale * 100.0f));
    }
  }
  // A body that fell ASLEEP while hot is not on the active list any more, so
  // the decay branch above can never reach its entry. Anything held here that
  // is not active is by definition stale (only an active body is ever inserted
  // or raised), so when the map outgrows the active list it is swept against
  // it. O(active) and only when it is needed -- in the steady state the map is
  // empty and this never runs.
  if (hotSteps_.size() > (size_t)n) {
    std::unordered_map<uint32_t, uint16_t> keep;
    keep.reserve(hotSteps_.size());
    for (uint32_t i = 0; i < n; i++) {
      auto it = hotSteps_.find(active[i].GetIndex());
      if (it != hotSteps_.end()) keep.insert(*it);
    }
    hotSteps_.swap(keep);
  }
  for (JPH::BodyID id : toCut) {
    const uint64_t handle = FromBodyID(id);
    uint32_t broke = 0;
    if (joints_) {
      auto bit = joints_->byBody.find(handle);
      if (bit != joints_->byBody.end()) {
        const std::vector<uint64_t> attached = bit->second;  // DestroyJoint
        for (uint64_t j : attached) {                        // mutates the map
          DestroyJoint(j);
          broke++;
        }
      }
    }
    bi.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    bi.DeactivateBody(id);
    hotSteps_.erase(id.GetIndex());
    runaway_.cut++;
    if (runawayReports_ < 8) {
      runawayReports_++;
      std::fprintf(stderr,
                   "[phys] body %u would not come off its velocity ceiling in "
                   "%d steps: %u joint(s) cut and the body stopped. Something "
                   "was feeding it; see the runaway note in "
                   "phys/physics.cpp.\n",
                   (unsigned)id.GetIndex(), kRunawayCutSteps, broke);
    }
  }
}

void Physics::Step(float dt) {
  if (!system_) return;
  // Before Update, so a body that was handed garbage while asleep never
  // reaches Jolt's own clamp — which overflows rather than clamping.
  InjectPhysFault();
  SweepInsaneVelocities();
  // ...and straight after it, because it is written to assume the sweep has
  // already removed everything that cannot safely be squared.
  SweepRunawayRigs();
  // Before Update, so MotionRunVox answers for the motion that CARRIED a body
  // into this step's contacts, not for the stop the contact put it in.
  TrackMotionRuns(dt);
  // Gravity is re-applied here rather than only at Init so a tuning reload
  // takes effect without restarting the world.
  system_->SetGravity(JPH::Vec3(0, -CurrentTuning().physics.gravity, 0));
  // Contacts belong to the step that produced them: clear on the GAME THREAD
  // before Update hands the buffer to the job threads, so a caller reading
  // after Step sees exactly that step and nothing accumulates when nobody
  // drains (a headless run never reads this at all).
  if (contacts_) {
    contacts_->impacts.clear();
    contacts_->bodyImpacts.clear();
    contacts_->ResetCounts();
  }
  // WALL CLOCK, REPORT ONLY. The owner's report was "the whole game froze for
  // several minutes", and the one thing that was missing when it happened was
  // any line saying so. This may not change what the simulation does -- a
  // decision taken on wall clock is a decision that differs between machines
  // -- but it may say what it saw, with the two numbers that attribute it.
  auto traceStates = [&](char tag) {
    FILE* tf = PhysTrace();
    if (tf == nullptr) return;
    static uint64_t step = 0;
    if (tag == 'P') step++;
    std::vector<BodyStateBits> st;
    DebugBodyStates(st);
    uint64_t h = 1469598103934665603ull;
    for (BodyStateBits& b : st) {
      b.birth = 0;   // per-process diagnostic, not state
      h = FnvBytes(h, &b, sizeof b);
    }
    std::fprintf(tf, "%c %llu n %zu h %016llx\n", tag, (unsigned long long)step, st.size(),
                 (unsigned long long)h);
  };
  traceStates('P');
  const auto t0 = std::chrono::steady_clock::now();
  system_->Update(dt, CurrentTuning().physics.collisionSteps, tempAlloc_.get(),
                  jobs_.get());
  traceStates('S');
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
  if (ms > runaway_.worstStepMs) runaway_.worstStepMs = ms;
  lastStep_.ms = ms;
  if (contacts_) contacts_->Finish();
  if (contacts_) {
    lastStep_.manifoldsDyn = contacts_->manifoldsDyn.load();
    lastStep_.pointsDyn = contacts_->pointsDyn.load();
    lastStep_.manifoldsStatic = contacts_->manifoldsStatic.load();
    lastStep_.pointsStatic = contacts_->pointsStatic.load();
  }
  if (ms >= kStepWatchdogMs && runawayReports_ < 8) {
    runawayReports_++;
    std::fprintf(stderr,
                 "[phys] one Update took %.0f ms over %u active bodies "
                 "(%u of them at their velocity ceiling; fastest %.0f vox/s, "
                 "%.1f rad/s since the last probe reset); contacts %u "
                 "body-body manifolds / %u points, %u body-static manifolds / "
                 "%u points. Every dynamic body is LinearCast, so a fast body "
                 "pays a swept cast against terrain every step; see the "
                 "runaway note in phys/physics.cpp.\n",
                 ms, (unsigned)NumActiveBodies(), (unsigned)runaway_.hot,
                 (double)runaway_.peakSpeedVox, (double)runaway_.peakSpinRad,
                 lastStep_.manifoldsDyn, lastStep_.pointsDyn,
                 lastStep_.manifoldsStatic, lastStep_.pointsStatic);
  }
  // After the step, so a piece is judged against where it has fallen TO.
  TickPendingReleases();
}

// ---- WHY (NEARLY) EVERY DYNAMIC WORLD BODY IS LinearCast (anti-tunnelling) --
//
// Jolt's default motion quality is Discrete: the body is advanced by v*dt and
// only THEN asked what it overlaps. The thing it has to not miss is a
// marching-cubes terrain patch, which is a sheet of triangles with no
// thickness at all, so the margin is the thinnest box in the collider and
// nothing else. A ragdoll forearm is 2-3 voxels = 20-30 cm through; a fall
// that has had three seconds to build (ragdoll.fallSeconds, which is when a
// creature goes limp in mid-air in the first place) is already at 29 m/s, or
// 48 cm in a 60 Hz step, and keeps accelerating because nothing in this engine
// models air drag on a rigid body. The ground was not being missed by a little.
//
// LinearCast shape-casts the step and stops the body at the first hit. It is
// not free, but Jolt only pays for it when the step is longer than
// mLinearCastThreshold (0.75) * the collider's inner radius -- the smallest
// half-extent of any sub-box -- so a settled or walking body costs exactly
// what it did before, and the bodies that do pay are the handful that are
// moving fast enough to be about to leave the world.
//
// EXCEPT A BIG ONE (2026-09-22). "The smallest half-extent of any sub-box" is
// half a voxel on anything with one loose leaf, so a felled tree paid the cast
// on every step of its fall, over every one of its boxes: 100+ ms a step. A
// body at least kDiscreteMinExtentVox thick on every axis is Discrete; see
// that constant for why it cannot tunnel.
//
// This is only half the guarantee: a cast can only hit a triangle that EXISTS,
// and the patch under a fast-falling body is built by DebrisSystem::
// ManageTerrain a few ticks after something asks for it. The other half --
// a body may not enter space no collider describes at all -- is
// DebrisSystem::UntunnelBody.
uint64_t Physics::CreateDebrisBody(const std::vector<DebrisVoxel>& voxels,
                                   IVec3 originVoxel,
                                   const std::vector<float>& densityOfMat,
                                   bool allowKinematic, float voxelPitch,
                                   bool looseMatter) {
  BodyTransform xf{};
  xf.pos = Vec3{(float)originVoxel.x, (float)originVoxel.y, (float)originVoxel.z};
  xf.quat[3] = 1;
  return CreateDebrisBodyXf(voxels, xf, densityOfMat, allowKinematic, voxelPitch,
                            looseMatter);
}

uint64_t Physics::CreateDebrisBodyXf(const std::vector<DebrisVoxel>& voxels,
                                     const BodyTransform& xf,
                                     const std::vector<float>& densityOfMat,
                                     bool allowKinematic, float voxelPitch,
                                     bool looseMatter) {
  if (!system_ || voxels.empty()) return 0;
  if (!(voxelPitch > 0.0f)) voxelPitch = 1.0f;

  // SANDVOX_PHYS_PROFILE=1 prints one line per body splitting the birth cost
  // into the greedy merge, Jolt's compound build (a BVH over the boxes) and
  // CreateAndAddBody, so a slow birth tick names its term without a rebuild.
  static const bool kProfile = [] {
    const char* e = std::getenv("SANDVOX_PHYS_PROFILE");
    return e && e[0] != '0';
  }();
  using Clock = std::chrono::steady_clock;
  const Clock::time_point t0 = kProfile ? Clock::now() : Clock::time_point{};

  // greedy box merge over the local voxel set (runs in +x, extended in +y,
  // then +z) — keeps compound shapes small for compact islands.
  //
  // The occupancy is a DENSE byte lattice over the body's bounding box, not a
  // hash map: a body is an int8 lattice (<= 128 a side; the tree-fell oak is
  // 51x90x51 = 232 KB), and the merge probes it once per voxel per axis of
  // extension. With two unordered_maps the merge was 8.3 ms of the 8.6 ms
  // birth of that 28,478-voxel tree (Jolt's compound build was 0.3 ms); the
  // byte lattice is the same walk in the same order — identical box set, so
  // identical collider and resting pose — at memory speed. 0 = absent,
  // 1 = present, 2 = already inside a merged box.
  int minc[3] = {127, 127, 127}, maxc[3] = {-128, -128, -128};
  for (const DebrisVoxel& v : voxels) {
    minc[0] = std::min(minc[0], (int)v.x);
    minc[1] = std::min(minc[1], (int)v.y);
    minc[2] = std::min(minc[2], (int)v.z);
    maxc[0] = std::max(maxc[0], (int)v.x);
    maxc[1] = std::max(maxc[1], (int)v.y);
    maxc[2] = std::max(maxc[2], (int)v.z);
  }
  const int ex = maxc[0] - minc[0] + 1, ey = maxc[1] - minc[1] + 1,
            ez = maxc[2] - minc[2] + 1;
  // A box never extends into negative local coordinates (the original rule,
  // kept), so the low probe bound is max(0, min) per axis.
  const int lo[3] = {std::max(0, minc[0]), std::max(0, minc[1]),
                     std::max(0, minc[2])};
  std::vector<uint8_t> occ((size_t)ex * ey * ez, 0);
  auto idx = [&](int x, int y, int z) {
    return ((size_t)(z - minc[2]) * ey + (size_t)(y - minc[1])) * ex +
           (size_t)(x - minc[0]);
  };
  for (const DebrisVoxel& v : voxels) occ[idx(v.x, v.y, v.z)] = 1;
  auto has = [&](int x, int y, int z) {
    return x >= lo[0] && y >= lo[1] && z >= lo[2] && x <= maxc[0] &&
           y <= maxc[1] && z <= maxc[2] && occ[idx(x, y, z)] == 1;
  };

  JPH::StaticCompoundShapeSettings compound;
  // The box budget below bounds the sub-shape list, so this reserve is exact
  // for a budgeted body and an upper bound (n voxels = n boxes at worst)
  // otherwise; it keeps Jolt's Array from regrowing.
  const int boxBudget =
      looseMatter ? ColliderBoxBudgetFor(voxels.size()) : kColliderBoxBudget;
  compound.mSubShapes.reserve(std::min<size_t>(voxels.size(), (size_t)boxBudget));
  float totalMass = 0;
  // One supplied voxel is `voxelPitch` world voxels on a side, so its physical
  // volume is (pitch * kVoxelMeters)^3. A scale-2 limb has 8x the voxels at 1/8
  // the volume each: same total mass, same density, same physical size as the
  // scale-1 art it replaced. The `pitch == 1` path is arithmetically identical
  // to the original expression, so ordinary debris is unchanged.
  const float pm = voxelPitch * kVoxelMeters;
  const float voxVol = pm * pm * pm;
  for (const DebrisVoxel& v : voxels) {
    uint32_t mat = v.payload & 0xFFF;
    float density = mat < densityOfMat.size() ? densityOfMat[mat] : 1000.0f;
    totalMass += density * voxVol;
  }

  // A box in lattice voxels: min corner and size.
  struct Box { int x, y, z, sx, sy, sz; };
  std::vector<Box> fine;
  for (const DebrisVoxel& v : voxels) {
    if (occ[idx(v.x, v.y, v.z)] == 2) continue;
    // extend +x
    int sx = 1;
    while (has(v.x + sx, v.y, v.z)) sx++;
    // extend +y while the whole x-run exists
    int sy = 1;
    for (;; sy++) {
      bool ok = true;
      for (int i = 0; i < sx && ok; i++) ok = has(v.x + i, v.y + sy, v.z);
      if (!ok) break;
    }
    // extend +z while the whole xy-slab exists
    int sz = 1;
    for (;; sz++) {
      bool ok = true;
      for (int j = 0; j < sy && ok; j++)
        for (int i = 0; i < sx && ok; i++) ok = has(v.x + i, v.y + j, v.z + sz);
      if (!ok) break;
    }
    for (int k = 0; k < sz; k++)
      for (int j = 0; j < sy; j++)
        for (int i = 0; i < sx; i++) occ[idx(v.x + i, v.y + j, v.z + k)] = 2;
    fine.push_back({v.x, v.y, v.z, sx, sy, sz});
  }

  // ---- THE BOX BUDGET: a felled tree is not 1024 one-voxel boxes ----------
  //
  // The greedy merge is exact, and on a SOLID body (a slab, a trunk, a limb)
  // it is also small. On a crown it is neither: leaves are a sparse dither, so
  // nearly every leaf is its own 1x1x1 box. This used to stop at 1024 boxes,
  // which on the tree-fell oak covered 4,907 of 28,401 voxels -- the collider
  // was the trunk plus whichever sixth of the crown the flood order reached
  // first -- and 1024 boxes is also what every narrow-phase query on the body
  // then iterates. Measured 2026-09-22 (`--fell-tree` with the baked birch,
  // split twice): three such bodies made ONE Jolt Update 100-138 ms, i.e. 3 fps
  // at 4 ticks a frame, for as long as the logs moved.
  //
  // Over budget, the body keeps its FAT boxes exactly (the trunk: what rests
  // on the ground, where a coarse box would read as floating) and the rest of
  // its voxels -- the leaves and twigs -- are re-merged on a coarser lattice,
  // doubling it until the total fits. A coarse cell is solid if ANY of its
  // voxels is, clipped to the body's own bounds, so the collider covers every
  // voxel of the body instead of a sixth of them, at the price of a crown that
  // collides as a slightly fuller blob. Under budget nothing changes: same
  // boxes in the same order, so every existing body keeps its collider.
  std::vector<Box> emit;
  int coarseCell = 1;
  size_t keptFat = 0;
  if ((int)fine.size() <= boxBudget) {
    emit = std::move(fine);
  } else {
    std::vector<uint32_t> order(fine.size());
    for (uint32_t i = 0; i < (uint32_t)fine.size(); i++) order[i] = i;
    auto vol = [&](const Box& b) { return b.sx * b.sy * b.sz; };
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      return vol(fine[a]) > vol(fine[b]);
    });
    for (uint32_t i : order) {
      const Box& b = fine[i];
      if (vol(b) < kFatBoxMinVoxels) break;
      if (std::min(b.sx, std::min(b.sy, b.sz)) < 2) continue;
      emit.push_back(b);
      for (int k = 0; k < b.sz; k++)
        for (int j = 0; j < b.sy; j++)
          for (int ii = 0; ii < b.sx; ii++) occ[idx(b.x + ii, b.y + j, b.z + k)] = 3;
      if ((int)emit.size() >= boxBudget / 2) break;
    }
    keptFat = emit.size();
    // Everything a kept box does not hold, on a lattice of `cell` voxels.
    std::vector<uint8_t> cocc;
    std::vector<Box> coarse;
    for (int cell = 2;; cell *= 2) {
      const int cx = (ex + cell - 1) / cell, cy = (ey + cell - 1) / cell,
                cz = (ez + cell - 1) / cell;
      cocc.assign((size_t)cx * cy * cz, 0);
      auto cidx = [&](int x, int y, int z) {
        return ((size_t)z * cy + (size_t)y) * cx + (size_t)x;
      };
      for (const DebrisVoxel& v : voxels)
        if (occ[idx(v.x, v.y, v.z)] == 2)
          cocc[cidx((v.x - minc[0]) / cell, (v.y - minc[1]) / cell,
                    (v.z - minc[2]) / cell)] = 1;
      auto chas = [&](int x, int y, int z) {
        return x < cx && y < cy && z < cz && cocc[cidx(x, y, z)] == 1;
      };
      coarse.clear();
      for (int z = 0; z < cz; z++)
        for (int y = 0; y < cy; y++)
          for (int x = 0; x < cx; x++) {
            if (cocc[cidx(x, y, z)] != 1) continue;
            int sx = 1;
            while (chas(x + sx, y, z)) sx++;
            int sy = 1;
            for (;; sy++) {
              bool ok = true;
              for (int i = 0; i < sx && ok; i++) ok = chas(x + i, y + sy, z);
              if (!ok) break;
            }
            int sz = 1;
            for (;; sz++) {
              bool ok = true;
              for (int j = 0; j < sy && ok; j++)
                for (int i = 0; i < sx && ok; i++) ok = chas(x + i, y + j, z + sz);
              if (!ok) break;
            }
            for (int k = 0; k < sz; k++)
              for (int j = 0; j < sy; j++)
                for (int i = 0; i < sx; i++) cocc[cidx(x + i, y + j, z + k)] = 2;
            // Back to lattice voxels, clipped to the body's own bounds so a
            // coarse cell past the edge does not grow the body.
            Box b;
            b.x = minc[0] + x * cell;
            b.y = minc[1] + y * cell;
            b.z = minc[2] + z * cell;
            b.sx = std::min(sx * cell, maxc[0] + 1 - b.x);
            b.sy = std::min(sy * cell, maxc[1] + 1 - b.y);
            b.sz = std::min(sz * cell, maxc[2] + 1 - b.z);
            coarse.push_back(b);
          }
      coarseCell = cell;
      if ((int)(keptFat + coarse.size()) <= boxBudget ||
          cell >= std::max(ex, std::max(ey, ez)))
        break;
    }
    emit.insert(emit.end(), coarse.begin(), coarse.end());
  }

  const int boxes = (int)emit.size();
  size_t covered = 0;  // voxels inside a box (with overlap, when coarse)
  for (const Box& b : emit) {
    JPH::Vec3 half(VoxToM(b.sx * 0.5f * voxelPitch), VoxToM(b.sy * 0.5f * voxelPitch),
                   VoxToM(b.sz * 0.5f * voxelPitch));
    JPH::Vec3 center(VoxToM((b.x + b.sx * 0.5f) * voxelPitch),
                     VoxToM((b.y + b.sy * 0.5f) * voxelPitch),
                     VoxToM((b.z + b.sz * 0.5f) * voxelPitch));
    // Tiny convex radius: debris voxels are 12.5 cm. Scale it with the pitch
    // too — a fixed 1 cm skin on a 3 cm micro voxel is a third of the box, and
    // Jolt would round the limb off into a lump.
    compound.AddShape(center, JPH::Quat::sIdentity(),
                      new JPH::BoxShape(half, 0.01f * voxelPitch));
    covered += (size_t)b.sx * b.sy * b.sz;
  }

  const Clock::time_point t1 = kProfile ? Clock::now() : Clock::time_point{};
  auto shapeResult = compound.Create();
  if (shapeResult.HasError()) {
    std::fprintf(stderr, "debris shape error: %s\n",
                 shapeResult.GetError().c_str());
    return 0;
  }
  const Clock::time_point t2 = kProfile ? Clock::now() : Clock::time_point{};

  JPH::Quat q(xf.quat[0], xf.quat[1], xf.quat[2], xf.quat[3]);
  if (q.LengthSq() < 1e-6f) q = JPH::Quat::sIdentity();
  JPH::BodyCreationSettings bcs(
      shapeResult.Get(),
      JPH::RVec3(VoxToM(xf.pos.x), VoxToM(xf.pos.y), VoxToM(xf.pos.z)),
      q.Normalized(), JPH::EMotionType::Dynamic, Layers::MOVING);
  bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
  bcs.mMassPropertiesOverride.mMass = std::max(totalMass, 0.05f);
  const auto& pt = CurrentTuning().physics;
  bcs.mFriction = pt.debrisFriction;
  bcs.mRestitution = pt.debrisRestitution;
  bcs.mLinearDamping = pt.debrisLinearDamping;
  bcs.mAngularDamping = pt.debrisAngularDamping;
  bcs.mAllowDynamicOrKinematic = allowKinematic;
  bcs.mMaxLinearVelocity = kBodyMaxSpeedMS;   // see the ceilings note above
  bcs.mMaxAngularVelocity = kBodyMaxSpinRad;
  // Ghost contacts with internal edges of the marching-cubes terrain are what
  // make debris snag and hop on flat-looking ground; this is Jolt's fix.
  bcs.mEnhancedInternalEdgeRemoval = true;
  // LinearCast only where tunnelling is possible (see the note above
  // CreateDebrisBody, and kDiscreteMinExtentVox for why a big body is exempt).
  const float thinnestVox =
      (float)std::min(ex, std::min(ey, ez)) * voxelPitch;
  const bool cast = thinnestVox < kDiscreteMinExtentVox;
  if (cast && !AntiTunnelOff(AntiTunnel::Ccd))
    bcs.mMotionQuality = JPH::EMotionQuality::LinearCast;  // see note above

  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = CreateAndAddDet(bi, ids_, bcs, JPH::EActivation::Activate);
  NoteBirth(id.GetIndexAndSequenceNumber());
  if (kProfile) {
    const Clock::time_point t3 = Clock::now();
    auto us = [](Clock::time_point a, Clock::time_point b) {
      return std::chrono::duration<double, std::micro>(b - a).count();
    };
    std::printf(
        "[phys-prof] body %zu vox extent %dx%dx%d -> %d boxes (%zu fat kept, "
        "rest on a %d-voxel lattice) covering %zu, %s: merge %.0f us, "
        "compound.Create %.0f us, CreateAndAddBody %.0f us (total %.0f us)\n",
        voxels.size(), ex, ey, ez, boxes, keptFat, coarseCell, covered,
        cast ? "LinearCast" : "discrete", us(t0, t1), us(t1, t2), us(t2, t3),
        us(t0, t3));
  }
  if (id.IsInvalid()) return 0;
  GuardBodyInertia(id.GetIndexAndSequenceNumber(), "debris");
  uint64_t h = FromBodyID(id);
  dynamicBodies_.push_back(h);
  return h;
}

uint64_t Physics::CreateSphereBody(Vec3 centerVoxel, float radiusVoxels,
                                   float densityKgM3, Vec3 originOffsetVox) {
  if (!system_ || radiusVoxels <= 0) return 0;
  const float rM = VoxToM(radiusVoxels);
  // Offset origin: the sphere sits at +offset from the body origin so a
  // min-corner microvoxel render model and the collider agree (header note).
  // Jolt keeps the COM at the shape's centre either way, so rotation still
  // pivots through the middle of the ball and rolling is unaffected.
  JPH::Ref<JPH::Shape> shape = new JPH::SphereShape(rM);
  if (originOffsetVox.x != 0 || originOffsetVox.y != 0 || originOffsetVox.z != 0) {
    JPH::RotatedTranslatedShapeSettings rts(
        JPH::Vec3(VoxToM(originOffsetVox.x), VoxToM(originOffsetVox.y),
                  VoxToM(originOffsetVox.z)),
        JPH::Quat::sIdentity(), shape);
    auto res = rts.Create();
    if (res.HasError()) return 0;
    shape = res.Get();
  }
  Vec3 origin = centerVoxel - originOffsetVox;
  JPH::BodyCreationSettings bcs(
      shape,
      JPH::RVec3(VoxToM(origin.x), VoxToM(origin.y), VoxToM(origin.z)),
      JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, Layers::MOVING);
  bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
  bcs.mMassPropertiesOverride.mMass =
      std::max(densityKgM3 * (4.0f / 3.0f) * JPH::JPH_PI * rM * rM * rM, 0.05f);
  const auto& pt = CurrentTuning().physics;
  bcs.mFriction = pt.sphereFriction;
  bcs.mRestitution = pt.sphereRestitution;
  bcs.mLinearDamping = pt.debrisLinearDamping;
  // Deliberately lighter angular damping than debris: rolling is the entire
  // point of this shape, and the debris value is tuned to stop tumbling fast.
  bcs.mAngularDamping = pt.sphereAngularDamping;
  // The linear ceiling, but NOT a lower angular one: omega = v/r, so a small
  // ball rolling at a normal speed is legitimately at hundreds of rad/s and
  // capping that would make it skid. See the ceilings note above.
  bcs.mMaxLinearVelocity = kBodyMaxSpeedMS;
  bcs.mEnhancedInternalEdgeRemoval = true;
  // A ball's inner radius IS its radius, so the cast only fires on a genuinely
  // fast roll or fall. See the note above CreateDebrisBody.
  if (!AntiTunnelOff(AntiTunnel::Ccd))
    bcs.mMotionQuality = JPH::EMotionQuality::LinearCast;

  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = CreateAndAddDet(bi, ids_, bcs, JPH::EActivation::Activate);
  NoteBirth(id.GetIndexAndSequenceNumber());
  if (id.IsInvalid()) return 0;
  GuardBodyInertia(id.GetIndexAndSequenceNumber(), "sphere");
  uint64_t h = FromBodyID(id);
  dynamicBodies_.push_back(h);
  return h;
}

uint64_t Physics::CreateTerrainMesh(const std::vector<float>& vertsXYZ,
                                    const std::vector<uint32_t>& indices) {
  if (!system_ || indices.size() < 3) return 0;
  TerrainShapeJob job;
  job.verts = vertsXYZ;
  job.indices = indices;
  BuildTerrainShape(job);
  return AddTerrainShape(job);
}

void Physics::BuildTerrainShape(TerrainShapeJob& job) const {
  job.shape = nullptr;
  if (job.indices.size() < 3) return;
  JPH::VertexList verts;
  verts.reserve(job.verts.size() / 3);
  for (size_t i = 0; i + 2 < job.verts.size(); i += 3)
    verts.push_back(JPH::Float3(VoxToM(job.verts[i]), VoxToM(job.verts[i + 1]),
                                VoxToM(job.verts[i + 2])));
  JPH::IndexedTriangleList tris;
  tris.reserve(job.indices.size() / 3);
  for (size_t i = 0; i + 2 < job.indices.size(); i += 3)
    tris.push_back(JPH::IndexedTriangle(job.indices[i], job.indices[i + 1],
                                        job.indices[i + 2]));

  JPH::MeshShapeSettings mesh(verts, tris);
  // Default is cos(5°): nearly every seam between marching-cubes triangles
  // counts as an "active" edge whose normal can kick a rolling body. 25° keeps
  // real ridges active but lets the near-coplanar facets of smooth-looking
  // terrain read as one surface. Pairs with mEnhancedInternalEdgeRemoval on
  // the dynamic bodies.
  mesh.mActiveEdgeCosThresholdAngle = 0.9063f;  // cos(25 deg)
  auto shapeResult = mesh.Create();
  if (shapeResult.HasError()) return;
  const JPH::Shape* sh = shapeResult.Get().GetPtr();
  sh->AddRef();  // the ShapeResult's ref dies with it; this one is the job's
  job.shape = sh;
}

void Physics::DropTerrainShape(TerrainShapeJob& job) const {
  if (job.shape) static_cast<const JPH::Shape*>(job.shape)->Release();
  job.shape = nullptr;
}

uint64_t Physics::AddTerrainShape(TerrainShapeJob& job) {
  if (!system_ || !job.shape) {
    DropTerrainShape(job);
    return 0;
  }
  const JPH::Shape* sh = static_cast<const JPH::Shape*>(job.shape);
  JPH::BodyCreationSettings bcs(sh, JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                JPH::EMotionType::Static, Layers::STATIC);
  bcs.mFriction = CurrentTuning().physics.terrainFriction;
  const auto t0 = std::chrono::steady_clock::now();
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = CreateAndAddDet(bi, ids_, bcs, JPH::EActivation::DontActivate);
  NoteBirth(id.GetIndexAndSequenceNumber());
  if (FILE* tf = PhysTrace()) {
    uint64_t h = FnvBytes(1469598103934665603ull, job.verts.data(), job.verts.size() * 4);
    h = FnvBytes(h, job.indices.data(), job.indices.size() * 4);
    std::fprintf(tf, "T %x tris %zu h %016llx\n", id.GetIndexAndSequenceNumber(),
                 job.indices.size() / 3, (unsigned long long)h);
  }
  meshStats_.meshes++;
  meshStats_.tris += job.indices.size() / 3;
  meshStats_.addUs += std::chrono::duration<double, std::micro>(
                          std::chrono::steady_clock::now() - t0).count();
  DropTerrainShape(job);  // the body holds its own reference now
  return id.IsInvalid() ? 0 : FromBodyID(id);
}

void Physics::ParallelFor(uint32_t n, const std::function<void(uint32_t)>& fn) {
  if (n == 0) return;
  // SANDVOX_NO_PARALLEL_FOR=1: the serial arm of the A/B, in one binary.
  static const bool serial = std::getenv("SANDVOX_NO_PARALLEL_FOR") != nullptr;
  if (!jobs_ || n == 1 || serial) {
    for (uint32_t i = 0; i < n; i++) fn(i);
    return;
  }
  JPH::JobSystem::Barrier* barrier = jobs_->CreateBarrier();
  for (uint32_t i = 0; i < n; i++) {
    JPH::JobHandle h =
        jobs_->CreateJob("sv-parallel-for", JPH::Color::sGreen, [&fn, i] { fn(i); });
    barrier->AddJob(h);
  }
  jobs_->WaitForJobs(barrier);
  jobs_->DestroyBarrier(barrier);
}

uint64_t Physics::CreatePlayerBody(float halfXZVox, float halfYVox) {
  if (!system_) return 0;
  float radius = VoxToM(halfXZVox);
  float cylHalf = std::max(VoxToM(halfYVox) - radius, 0.01f);
  // THE OWNER-SCOPING SLOT: the lowest one no live proxy holds, carried as the
  // proxy's own object layer (PLAYER_BASE + slot). Past kSlots the proxy is
  // the unslotted PLAYER, which owns nothing (every OWNED body meets it).
  static_assert(Layers::kSlots == kMaxPlayerProxies,
                "one owner-scoping slot per proxy the release sweep tests");
  JPH::ObjectLayer layer = Layers::PLAYER;
  {
    bool used[Layers::kSlots] = {};
    for (uint64_t p : playerBodies_) {
      const int s = PlayerSlotOf(p);
      if (s >= 0) used[s] = true;
    }
    for (int s = 0; s < Layers::kSlots; s++)
      if (!used[s]) {
        layer = (JPH::ObjectLayer)(Layers::PLAYER_BASE + s);
        break;
      }
  }
  // Dynamic, not kinematic — see the header comment: finite mass is what
  // makes shoves scale with the shoved body's mass. Rotation is locked (a
  // capsule that tips over is not a player) and gravity is off (the AABB
  // controller owns vertical motion; the proxy just mirrors it).
  JPH::BodyCreationSettings bcs(new JPH::CapsuleShape(cylHalf, radius),
                                JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                JPH::EMotionType::Dynamic, layer);
  bcs.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX |
                     JPH::EAllowedDOFs::TranslationY |
                     JPH::EAllowedDOFs::TranslationZ;
  bcs.mGravityFactor = 0.0f;
  bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
  bcs.mMassPropertiesOverride.mMass =
      std::max(CurrentTuning().physics.playerMassKg, 1.0f);
  bcs.mFriction = CurrentTuning().physics.playerProxyFriction;
  bcs.mRestitution = 0.0f;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = CreateAndAddDet(bi, ids_, bcs, JPH::EActivation::Activate);
  NoteBirth(id.GetIndexAndSequenceNumber());
  // NOT in dynamicBodies_: the proxy never despawns and must not receive
  // explosion impulses or WakeNear — the player controller owns its motion.
  if (id.IsInvalid()) return 0;
  // EVERY live proxy is A player for CLEARING (see the member's note): a
  // piece leaves the EXEMPT layer only once it is clear of all of
  // them. RemoveBody erases, so the selftests that make and drop several
  // proxies do not accumulate stale handles here.
  const uint64_t h = FromBodyID(id);
  playerBodies_.push_back(h);
  return h;
}

void Physics::MovePlayerBody(uint64_t handle, Vec3 centerVoxel, float dt) {
  if (!system_ || handle == 0) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return;
  JPH::RVec3 target(VoxToM(centerVoxel.x), VoxToM(centerVoxel.y),
                    VoxToM(centerVoxel.z));
  // Teleport to the authoritative position (discarding whatever contacts did
  // to the proxy last step), then set the velocity the move implies so the
  // solver has real momentum to hand to anything the player walks into.
  JPH::RVec3 cur = bi.GetPosition(id);
  bi.SetPositionAndRotation(id, target, JPH::Quat::sIdentity(),
                            JPH::EActivation::Activate);
  JPH::Vec3 vel = JPH::Vec3(target - cur) / std::max(dt, 1e-3f);
  // A teleport (spawn, world load) is not a sprint: cap the implied speed so
  // one warped frame can't hand a resting body a 1000 m/s contact impulse.
  constexpr float kMaxSpeed = 30.0f;  // m/s, ~2x top sprint speed
  float speed = vel.Length();
  if (speed > kMaxSpeed) vel *= kMaxSpeed / speed;
  bi.SetLinearVelocity(id, vel);
}

Vec3 Physics::PlayerPushOut(uint64_t handle, Vec3 centerVoxel,
                            PushSource* outWorst) const {
  if (outWorst) *outWorst = PushSource{};
  if (!system_ || handle == 0) return {0, 0, 0};
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return {0, 0, 0};
  JPH::RefConst<JPH::Shape> shape = bi.GetShape(id);
  if (!shape) return {0, 0, 0};

  JPH::RVec3 center(VoxToM(centerVoxel.x), VoxToM(centerVoxel.y),
                    VoxToM(centerVoxel.z));
  JPH::CollideShapeSettings settings;
  JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
  // MOVING, plus whatever another player owns — never this player's own.
  PushLayerFilter pushable(PlayerSlotOf(handle));
  JPH::IgnoreSingleBodyFilter ignoreSelf(id);
  system_->GetNarrowPhaseQuery().CollideShape(
      shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(center),
      settings, JPH::RVec3::sZero(), collector, {}, pushable, ignoreSelf);

  // ONE DEPTH PER BODY, and only from a body heavy enough to move you.
  //
  // CollideShape reports a hit per SUB-SHAPE: a compound of eight voxel boxes
  // buried in the capsule comes back as eight hits of the same depth, and
  // summing them asked the player to move eight body-depths in one tick — a
  // 0.06 kg burn gobbet carried into the capsule by the avatar's own swinging
  // arm shoved the player 12 voxels a tick (36 m/s) for as long as it sat
  // there. Measured in the `ragdoll` gate's avatar-on-fire block: 62 voxels in
  // one tick, 432 voxels before the player burned to death. The deepest hit
  // per body is the whole of what that body asks for.
  //
  // And a body you could kick aside cannot move you: below kPushMinMassFrac of
  // the player's mass the depenetration is the body's problem (the proxy is a
  // dynamic 80 kg capsule and the solver pushes the light body out), not the
  // player's. Above it — a log, a boulder, a corpse's torso — the full push
  // applies, so standing on debris and being shoved by heavy things is as it
  // was. Kinematic limbs (a living creature's) report their rig mass and
  // keep pushing — another PLAYER's included (their limbs are OWNED by them,
  // not by this capsule); this player's own are OWNED by it (or EXEMPT) and
  // never seen here.
  constexpr float kPushMinMassFrac = 0.05f;
  const Tuning::Physics& pt = CurrentTuning().physics;
  const float minMass = kPushMinMassFrac * std::max(pt.playerMassKg, 1.0f);
  struct Deepest { JPH::BodyID id; JPH::Vec3 axis; float depth; bool alive; };
  std::vector<Deepest> perBody;
  for (const JPH::CollideShapeResult& hit : collector.mHits) {
    float len = hit.mPenetrationAxis.Length();
    if (len < 1e-6f || hit.mPenetrationDepth <= 0) continue;
    bool merged = false;
    for (Deepest& d : perBody) {
      if (d.id != hit.mBodyID2) continue;
      if (hit.mPenetrationDepth > d.depth) { d.depth = hit.mPenetrationDepth; d.axis = hit.mPenetrationAxis / len; }
      merged = true;
      break;
    }
    if (merged) continue;
    // A LIVE CREATURE'S POSED LIMB, and the motion type IS the question.
    // Kinematic on MOVING/OWNED is only ever a rig somebody is driving: a
    // corpse or a ragdoll has been handed to the solver and is dynamic, a held
    // weapon is a HeldProp, a severed limb mid-hold a SeveredHold (EXEMPT),
    // neither seen here. See the tuning note on
    // physics.creaturePhaseVox for why that distinction earns a softer rule.
    const bool alive = bi.GetMotionType(hit.mBodyID2) == JPH::EMotionType::Kinematic;
    perBody.push_back({hit.mBodyID2, hit.mPenetrationAxis / len,
                       hit.mPenetrationDepth, alive});
  }
  // A CREATURE MAY LEAN INTO YOU, AND MAY NOT LAUNCH YOU. Both halves are
  // metres here because that is the frame `mPenetrationDepth` is in; the
  // authored numbers are voxels.
  const float phaseM = pt.creaturePhaseVox * kVoxelMeters;
  const float capM = pt.creaturePushMaxVox * kVoxelMeters;
  JPH::Vec3 push = JPH::Vec3::sZero();
  for (const Deepest& d : perBody) {
    const float m = BodyMass(FromBodyID(d.id));
    if (m < minMass) continue;
    float depth = d.depth;
    if (d.alive) {
      // THE SLACK IS SPENT FIRST. Inside it the creature is simply standing in
      // you and you do not move at all, which is the whole of what makes a
      // 2-voxel bite reach: nothing shoves the victim out of contact on the
      // tick the teeth close.
      depth -= phaseM;
      if (depth <= 0.0f) continue;
      // ...AND WHAT IS LEFT IS RATE-LIMITED, so a creature that buries itself
      // eases you out over several ticks instead of teleporting you a
      // body-width in one. A cap of 0 would weld you together, so it is only
      // applied when the author asked for one.
      if (capM > 0.0f) depth = std::min(depth, capM);
    }
    // mPenetrationAxis points the way shape 2 (the body) moves to separate;
    // the player moves the opposite way
    push -= d.axis * depth;
    // The EFFECTIVE depth, not the raw overlap: this reports what actually
    // moved the player, which is the only number a budget gate can assert on.
    if (outWorst && depth / kVoxelMeters > outWorst->depthVox)
      *outWorst = PushSource{FromBodyID(d.id), m, depth / kVoxelMeters};
  }
  return Vec3{push.GetX(), push.GetY(), push.GetZ()} * (1.0f / kVoxelMeters);
}

// A world anchor expressed in one body's frame, metres. What ReplaceBody
// reads back when the OTHER body is rebuilt and the anchor has to be found
// again from something that did not move.
static JPH::Vec3 AnchorLocal(const JPH::Body& body, JPH::RVec3Arg anchor) {
  return body.GetRotation().Conjugated() *
         JPH::Vec3(anchor - body.GetPosition());
}

// The constraint itself, from two LOCKED bodies and the pivot in world metres
// as each body holds it (`anchor` on A, `anchorB` on B: the same point unless
// the desc carries rig anchors and the bodies have drifted apart). One
// function for CreateJoint and ReplaceBody, so a joint rebuilt against a
// replacement body is the same joint with the same limits, not a second
// reading of the desc.
static JPH::Ref<JPH::Constraint> BuildConstraint(JPH::Body& bodyA,
                                                 JPH::Body& bodyB,
                                                 const Physics::JointDesc& d,
                                                 JPH::RVec3Arg anchor,
                                                 JPH::RVec3Arg anchorB,
                                                 JPH::Vec3& boneOut) {
  using JointType = Physics::JointType;
  JPH::Body* a = &bodyA;
  JPH::Body* b = &bodyB;
  // REST FRAME -> WORLD, per body. Jolt's settings are world-space and it
  // immediately converts them back through each body's rotation, so feeding it
  // `bodyRotation * restDirection` lands the constraint's local frame exactly
  // on the rest direction — whatever pose the bodies are in right now. See the
  // JointDesc comment in physics.h for why that matters.
  const JPH::Quat ra = a->GetRotation();
  const JPH::Quat rb = b->GetRotation();

  JPH::Ref<JPH::Constraint> constraint;
  boneOut = JPH::Vec3::sZero();
  switch (d.type) {
    case JointType::Fixed: {
      JPH::FixedConstraintSettings s;
      if (!d.rigAnchors) {
        s.mAutoDetectPoint = true;
      } else {
        // B's frame is A's frame turned by relB: the same two world axes a
        // body pair AT that relationship would report, whatever pose the pair
        // is in now, so Jolt drives them back to it rather than keeping it.
        const JPH::Quat relInv =
            JPH::Quat(d.relB[0], d.relB[1], d.relB[2], d.relB[3])
                .Normalized()
                .Conjugated();
        s.mPoint1 = anchor;
        s.mPoint2 = anchorB;
        s.mAxisX1 = ra * JPH::Vec3::sAxisX();
        s.mAxisY1 = ra * JPH::Vec3::sAxisY();
        s.mAxisX2 = rb * (relInv * JPH::Vec3::sAxisX());
        s.mAxisY2 = rb * (relInv * JPH::Vec3::sAxisY());
      }
      constraint = s.Create(*a, *b);
      break;
    }
    case JointType::Hinge: {
      JPH::HingeConstraintSettings s;
      s.mPoint1 = anchor;
      s.mPoint2 = anchorB;
      JPH::Vec3 ax(d.axis.x, d.axis.y, d.axis.z);
      if (ax.LengthSq() < 1e-6f) ax = JPH::Vec3::sAxisX();
      ax = ax.Normalized();
      const JPH::Vec3 nrm = ax.GetNormalizedPerpendicular();
      s.mHingeAxis1 = ra * ax;
      s.mHingeAxis2 = rb * ax;
      // Angle zero is where the two normal axes align, so rotating the SAME
      // rest-frame vector into each body puts zero at the rest pose — the
      // frame every authored minAngle/maxAngle in the sidecars was measured
      // from.
      s.mNormalAxis1 = ra * nrm;
      s.mNormalAxis2 = rb * nrm;
      s.mLimitsMin = std::max(d.minAngle, -JPH::JPH_PI);
      s.mLimitsMax = std::min(d.maxAngle, JPH::JPH_PI);
      s.mMaxFrictionTorque = FrictionTorque(*b, anchorB, d.friction);
      if (d.motorTorque > 0.0f) {
        s.mMotorSettings = JPH::MotorSettings(std::max(d.motorFreq, 0.1f), 1.0f);
        s.mMotorSettings.SetTorqueLimit(d.motorTorque);
      }
      constraint = s.Create(*a, *b);
      if (d.motorTorque > 0.0f && constraint) {
        auto* h = static_cast<JPH::HingeConstraint*>(constraint.GetPtr());
        h->SetMotorState(JPH::EMotorState::Position);
        h->SetTargetAngle(0.0f);
      }
      break;
    }
    case JointType::Ball: {
      JPH::Vec3 bone(d.boneAxis.x, d.boneAxis.y, d.boneAxis.z);
      if (bone.LengthSq() < 1e-6f) bone = -JPH::Vec3::sAxisY();
      bone = bone.Normalized();
      // The fore/aft plane. Rigs are authored Y-up facing +Z, so the lateral
      // axis is world X at rest and the plane through it and the bone is the
      // one a stride or a reach happens in. A bone that IS lateral (an arm
      // modelled straight out) has no such plane, so fall back to any
      // perpendicular and let the two half-angles mean "in a plane" and "out
      // of it" without promising which is which.
      JPH::Vec3 plane = JPH::Vec3::sAxisX() - bone * bone.GetX();
      plane = plane.LengthSq() < 1e-4f ? bone.GetNormalizedPerpendicular()
                                       : plane.Normalized();

      JPH::SwingTwistConstraintSettings s;
      s.mPosition1 = anchor;
      s.mPosition2 = anchorB;
      s.mTwistAxis1 = ra * bone;
      s.mTwistAxis2 = rb * bone;
      s.mPlaneAxis1 = ra * plane;
      s.mPlaneAxis2 = rb * plane;
      s.mSwingType = JPH::ESwingType::Cone;
      // Jolt's normal axis is plane x twist, and its limit naming is off by
      // one from that: mNormalHalfConeAngle bounds the swing ABOUT the plane
      // axis (fore/aft, since the plane axis is lateral) and
      // mPlaneHalfConeAngle bounds the swing about the normal (out of plane).
      // Reading it the other way round gives a hip that does the splits but
      // cannot take a step.
      const float kMax = JPH::JPH_PI - 0.01f;
      s.mNormalHalfConeAngle = std::clamp(d.coneFwd, 0.0f, kMax);
      s.mPlaneHalfConeAngle = std::clamp(d.coneSide, 0.0f, kMax);
      const float tw = std::clamp(d.twist, 0.0f, kMax);
      s.mTwistMinAngle = -tw;
      s.mTwistMaxAngle = tw;
      s.mMaxFrictionTorque = FrictionTorque(*b, anchorB, d.friction);
      constraint = s.Create(*a, *b);
      boneOut = bone;
      break;
    }
  }
  return constraint;
}

uint64_t Physics::CreateJoint(uint64_t bodyA, uint64_t bodyB,
                              const JointDesc& d) {
  if (!system_ || bodyB == 0) return 0;
  // TwoBodyConstraintSettings::Create wants Body&: lock both bodies. bodyA 0
  // is the world (Body::sFixedToWorld, never locked: it is not in the system).
  const JPH::BodyLockInterface& bli = system_->GetBodyLockInterface();
  const bool world = bodyA == 0;
  JPH::BodyID ids[2] = {world ? ToBodyID(bodyB) : ToBodyID(bodyA), ToBodyID(bodyB)};
  JPH::BodyLockMultiWrite lock(bli, ids, world ? 1 : 2);
  JPH::Body* a = world ? &JPH::Body::sFixedToWorld : lock.GetBody(0);
  JPH::Body* b = world ? lock.GetBody(0) : lock.GetBody(1);
  if (!a || !b) return 0;

  JPH::RVec3 anchor(VoxToM(d.anchorVoxel.x), VoxToM(d.anchorVoxel.y),
                    VoxToM(d.anchorVoxel.z));
  JPH::RVec3 anchorB = anchor;
  if (d.rigAnchors && !world) {
    // Each body's own record of the pivot, through its pose now (JointDesc).
    anchor = a->GetPosition() +
             a->GetRotation() * JPH::Vec3(VoxToM(d.localA.x),
                                          VoxToM(d.localA.y),
                                          VoxToM(d.localA.z));
    anchorB = b->GetPosition() +
              b->GetRotation() * JPH::Vec3(VoxToM(d.localB.x),
                                           VoxToM(d.localB.y),
                                           VoxToM(d.localB.z));
  }
  JPH::Vec3 boneOut = JPH::Vec3::sZero();
  JPH::Ref<JPH::Constraint> constraint =
      BuildConstraint(*a, *b, d, anchor, anchorB, boneOut);
  if (!constraint) return 0;
  system_->AddConstraint(constraint);

  uint64_t h = nextJointId_++;
  JointImpls::Entry e;
  e.constraint = constraint;
  e.bodyA = bodyA;
  e.bodyB = bodyB;
  e.boneAxis = boneOut;
  e.desc = d;
  e.anchorLocalA = AnchorLocal(*a, anchor);
  e.anchorLocalB = AnchorLocal(*b, anchorB);
  joints_->joints[h] = e;
  joints_->byBody[bodyA].push_back(h);
  joints_->byBody[bodyB].push_back(h);
  return h;
}

bool Physics::RetargetJoint(uint64_t joint, uint64_t oldBody,
                            uint64_t newBody) {
  auto it = joints_->joints.find(joint);
  if (it == joints_->joints.end()) return false;
  JointImpls::Entry& e = it->second;
  const bool aMoves = e.bodyA == oldBody;
  const bool bMoves = e.bodyB == oldBody;
  if (aMoves == bMoves) return false;  // not on this body, or self-jointed
  const uint64_t newA = aMoves ? newBody : e.bodyA;
  const uint64_t newB = bMoves ? newBody : e.bodyB;
  const JPH::BodyLockInterface& bli = system_->GetBodyLockInterface();
  // A WORLD-ANCHORED joint (bodyA 0, CreateJoint): only B is a real body.
  const bool world = newA == 0;
  if (world && !bMoves) return false;
  JPH::BodyID ids[2] = {world ? ToBodyID(newB) : ToBodyID(newA), ToBodyID(newB)};
  JPH::BodyLockMultiWrite lock(bli, ids, world ? 1 : 2);
  JPH::Body* a = world ? &JPH::Body::sFixedToWorld : lock.GetBody(0);
  JPH::Body* b = world ? lock.GetBody(0) : lock.GetBody(1);
  if (!a || !b) return false;
  // THE ANCHOR COMES FROM THE SIDE THAT DID NOT CHANGE. A rebuilt collider
  // may sit at a rebased origin (DebrisSystem::RebaseVoxels shifts the
  // transform so the voxels stay put), so the replaced body's own local
  // anchor means nothing any more; the other body has not moved and still
  // holds the joint exactly where it was.
  const JPH::Body& keeper = aMoves ? *b : *a;
  const JPH::Vec3 local = aMoves ? e.anchorLocalB : e.anchorLocalA;
  const JPH::RVec3 anchor =
      keeper.GetPosition() + keeper.GetRotation() * local;
  JPH::Vec3 boneOut = JPH::Vec3::sZero();
  JPH::Ref<JPH::Constraint> c =
      BuildConstraint(*a, *b, e.desc, anchor, anchor, boneOut);
  if (!c) return false;
  if (e.desc.type == JointType::Hinge && e.desc.motorTorque > 0.0f)
    static_cast<JPH::HingeConstraint*>(c.GetPtr())->SetTargetAngle(e.motorTarget);
  system_->RemoveConstraint(e.constraint);
  system_->AddConstraint(c);
  e.constraint = c;
  e.boneAxis = boneOut;
  e.bodyA = newA;
  e.bodyB = newB;
  e.anchorLocalA = AnchorLocal(*a, anchor);
  e.anchorLocalB = AnchorLocal(*b, anchor);
  auto bit = joints_->byBody.find(oldBody);
  if (bit != joints_->byBody.end()) {
    auto& v = bit->second;
    v.erase(std::remove(v.begin(), v.end(), joint), v.end());
    if (v.empty()) joints_->byBody.erase(bit);
  }
  joints_->byBody[newBody].push_back(joint);
  return true;
}

void Physics::ReplaceBody(uint64_t oldHandle, uint64_t newHandle) {
  if (!system_ || oldHandle == 0 || newHandle == 0 ||
      oldHandle == newHandle)
    return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  const JPH::BodyLockInterface& bli = system_->GetBodyLockInterface();
  const JPH::BodyID oldId = ToBodyID(oldHandle), newId = ToBodyID(newHandle);
  if (bi.IsAdded(oldId) && bi.IsAdded(newId)) {
    // Body state, not shape state, so a fresh body starts without it: the
    // exclusion set that stops one mob's limbs fighting their own joints
    // (DisableCollisionsAmong) and the role + layer (SetBodyRole, CarryLayer).
    JPH::CollisionGroup group;
    {
      JPH::BodyLockRead lock(bli, oldId);
      if (lock.Succeeded()) group = lock.GetBody().GetCollisionGroup();
    }
    {
      JPH::BodyLockWrite lock(bli, newId);
      if (lock.Succeeded()) lock.GetBody().SetCollisionGroup(group);
    }
  }
  CarryLayer(oldHandle, newHandle);
  if (joints_) {
    auto bit = joints_->byBody.find(oldHandle);
    if (bit != joints_->byBody.end()) {
      const std::vector<uint64_t> attached = bit->second;  // Retarget mutates
      for (uint64_t j : attached) RetargetJoint(j, oldHandle, newHandle);
    }
  }
  // Whatever could not be moved (a joint whose other body is gone) dies with
  // the old body, as it always did.
  RemoveBody(oldHandle);
}

void Physics::CarryLayer(uint64_t from, uint64_t to) {
  if (!system_ || from == 0 || to == 0 || from == to) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  const JPH::BodyID fromId = ToBodyID(from), toId = ToBodyID(to);
  if (!bi.IsAdded(fromId) || !bi.IsAdded(toId)) return;
  // The role and owner travel in the user data, the layer is what they
  // resolved to (and whether the body is still clearing, below).
  bi.SetUserData(toId, bi.GetUserData(fromId));
  bi.SetObjectLayer(toId, bi.GetObjectLayer(fromId));
  bool fromPending = false, toPending = false;
  for (uint64_t h : pendingRelease_) {
    fromPending |= h == from;
    toPending |= h == to;
  }
  if (fromPending && !toPending) AddPending(to);
  rebuilt_[rebuiltHead_] = {from, to};
  rebuiltHead_ = (rebuiltHead_ + 1) % (uint32_t)rebuilt_.size();
}

uint64_t Physics::Successor(uint64_t handle) const {
  if (!system_ || handle == 0) return 0;
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  // A successor that is itself already gone is still the answer: one sword
  // hit rebuilds a limb TWICE in a tick (the Damage carve, then CutLimb's
  // kerf), A -> B -> C, and the caller walks the chain. Skipping dead B here
  // was "handle gone, no successor after 1 hop". A live successor wins over a
  // dead one (the heaviest, for a split); among dead ones, the newest record.
  uint64_t best = 0, newestDead = 0;
  float bestMass = -1.0f;
  const uint32_t n = (uint32_t)rebuilt_.size();
  for (uint32_t k = 1; k <= n; k++) {           // newest first
    const Rebuilt& r = rebuilt_[(rebuiltHead_ + n - k) % n];
    if (r.from != handle || r.to == 0) continue;
    if (!bi.IsAdded(ToBodyID(r.to))) {
      if (!newestDead) newestDead = r.to;
      continue;
    }
    const float m = BodyMass(r.to);
    if (m > bestMass) { best = r.to; bestMass = m; }
  }
  return best ? best : newestDead;
}

uint32_t Physics::JointCount(uint64_t handle) const {
  if (!joints_ || handle == 0) return 0;
  auto it = joints_->byBody.find(handle);
  return it == joints_->byBody.end() ? 0u : (uint32_t)it->second.size();
}

uint32_t Physics::JointCount() const {
  return joints_ ? (uint32_t)joints_->joints.size() : 0u;
}

void Physics::JointsOn(uint64_t handle, std::vector<BodyJoint>& out) const {
  out.clear();
  if (!joints_ || handle == 0) return;
  auto it = joints_->byBody.find(handle);
  if (it == joints_->byBody.end()) return;
  const float inv = 1.0f / kVoxelMeters;
  for (uint64_t j : it->second) {
    auto e = joints_->joints.find(j);
    if (e == joints_->joints.end()) continue;
    const bool isA = e->second.bodyA == handle;
    // The anchor in the frame of the body being ASKED ABOUT: each entry keeps
    // both, captured when the joint was built, which is what makes this exact
    // rather than a re-fit against a pose that has since moved.
    const JPH::Vec3 a =
        isA ? e->second.anchorLocalA : e->second.anchorLocalB;
    BodyJoint bj;
    bj.joint = j;
    bj.other = isA ? e->second.bodyB : e->second.bodyA;
    bj.anchorLocalVox = Vec3{a.GetX() * inv, a.GetY() * inv, a.GetZ() * inv};
    bj.type = e->second.desc.type;
    out.push_back(bj);
  }
}

bool Physics::JointSwingAngle(uint64_t joint, float& outRadians) const {
  outRadians = 0;
  if (!system_ || !joints_) return false;
  auto it = joints_->joints.find(joint);
  if (it == joints_->joints.end()) return false;
  const JPH::Vec3 bone = it->second.boneAxis;
  if (bone.LengthSq() < 0.5f) return false;  // not a ball joint

  const JPH::BodyLockInterface& bli = system_->GetBodyLockInterface();
  JPH::BodyID ids[2] = {ToBodyID(it->second.bodyA), ToBodyID(it->second.bodyB)};
  JPH::BodyLockMultiRead lock(bli, ids, 2);
  const JPH::Body* a = lock.GetBody(0);
  const JPH::Body* b = lock.GetBody(1);
  if (!a || !b) return false;
  // Where the child's bone points, expressed in the PARENT's frame. Comparing
  // in world space instead would report the parent's own tumble as bend.
  const JPH::Vec3 inParent = a->GetRotation().Conjugated() * (b->GetRotation() * bone);
  outRadians = std::acos(std::clamp(inParent.Dot(bone), -1.0f, 1.0f));
  return true;
}

void Physics::DestroyJoint(uint64_t joint) {
  if (!system_ || !joints_) return;
  auto it = joints_->joints.find(joint);
  if (it == joints_->joints.end()) return;
  system_->RemoveConstraint(it->second.constraint);
  for (uint64_t body : {it->second.bodyA, it->second.bodyB}) {
    auto bit = joints_->byBody.find(body);
    if (bit == joints_->byBody.end()) continue;
    auto& v = bit->second;
    v.erase(std::remove(v.begin(), v.end(), joint), v.end());
    if (v.empty()) joints_->byBody.erase(bit);
  }
  // waking both sides matters: a severed limb must start falling even if the
  // ragdoll had gone to sleep
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  for (uint64_t body : {it->second.bodyA, it->second.bodyB})
    if (body != 0 && bi.IsAdded(ToBodyID(body))) bi.ActivateBody(ToBodyID(body));
  joints_->joints.erase(it);
}

bool Physics::JointAlive(uint64_t joint) const {
  return joints_ && joints_->joints.count(joint) != 0;
}

bool Physics::SetJointMotorTarget(uint64_t joint, float radians) {
  if (!system_ || !joints_) return false;
  auto it = joints_->joints.find(joint);
  if (it == joints_->joints.end()) return false;
  JointImpls::Entry& e = it->second;
  if (e.desc.type != JointType::Hinge || !(e.desc.motorTorque > 0.0f)) return false;
  e.motorTarget = radians;
  static_cast<JPH::HingeConstraint*>(e.constraint.GetPtr())->SetTargetAngle(radians);
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  for (uint64_t body : {e.bodyA, e.bodyB})
    if (body != 0 && bi.IsAdded(ToBodyID(body))) bi.ActivateBody(ToBodyID(body));
  return true;
}

bool Physics::JointHingeAngle(uint64_t joint, float& outRadians) const {
  outRadians = 0.0f;
  if (!system_ || !joints_) return false;
  auto it = joints_->joints.find(joint);
  if (it == joints_->joints.end() || it->second.desc.type != JointType::Hinge) return false;
  outRadians = static_cast<const JPH::HingeConstraint*>(it->second.constraint.GetPtr())
                   ->GetCurrentAngle();
  return true;
}

void Physics::SetBodyKinematic(uint64_t handle, bool kinematic) {
  if (!system_ || handle == 0) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return;
  bi.SetMotionType(id, kinematic ? JPH::EMotionType::Kinematic
                                 : JPH::EMotionType::Dynamic,
                   JPH::EActivation::Activate);
}

bool Physics::UsesLinearCast(uint64_t handle) const {
  if (!system_ || handle == 0) return false;
  JPH::BodyLockRead lock(system_->GetBodyLockInterface(), ToBodyID(handle));
  if (!lock.Succeeded()) return false;
  const JPH::Body& body = lock.GetBody();
  const JPH::MotionProperties* mp = body.GetMotionPropertiesUnchecked();
  return body.IsDynamic() && mp != nullptr &&
         mp->GetMotionQuality() == JPH::EMotionQuality::LinearCast;
}

void Physics::DebugBodyStates(std::vector<BodyStateBits>& out) const {
  out.clear();
  if (!system_) return;
  JPH::BodyIDVector ids;
  system_->GetBodies(ids);
  std::sort(ids.begin(), ids.end());
  const JPH::BodyLockInterfaceNoLock& li = system_->GetBodyLockInterfaceNoLock();
  auto bits = [](float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
  };
  for (const JPH::BodyID& id : ids) {
    JPH::BodyLockRead lock(li, id);
    if (!lock.Succeeded()) continue;
    const JPH::Body& b = lock.GetBody();
    BodyStateBits s;
    s.handle = (uint64_t)id.GetIndexAndSequenceNumber() + 1;
    s.layer = (uint16_t)b.GetObjectLayer();
    s.motion = (uint8_t)b.GetMotionType();
    s.active = b.IsActive() ? 1 : 0;
    const JPH::RVec3 p = b.GetPosition();
    const JPH::Quat q = b.GetRotation();
    const JPH::Vec3 lv = b.GetLinearVelocity(), av = b.GetAngularVelocity();
    for (int k = 0; k < 3; k++) {
      s.pos[k] = bits((float)p[k]);
      s.lin[k] = bits(lv[k]);
      s.ang[k] = bits(av[k]);
    }
    s.rot[0] = bits(q.GetX());
    s.rot[1] = bits(q.GetY());
    s.rot[2] = bits(q.GetZ());
    s.rot[3] = bits(q.GetW());
    s.user = b.GetUserData();
    const uint32_t idx = id.GetIndex();
    s.birth = idx < birth_.size() ? birth_[idx] : 0ull;
    out.push_back(s);
  }
}

bool Physics::IsBodyDynamic(uint64_t handle) const {
  if (!system_ || handle == 0) return false;
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  const JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return false;
  return bi.GetMotionType(id) == JPH::EMotionType::Dynamic;
}

bool Physics::ApplyImpulseAt(uint64_t handle, Vec3 dir, float impulse,
                             Vec3 atVoxel) {
  if (!system_ || handle == 0 || impulse <= 0.0f) return false;
  const float dl = dir.len();
  if (dl < 1e-6f) return false;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  const JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return false;
  // A KINEMATIC BODY IS POSED BY SOMEBODY ELSE. See the header: refusing here
  // is what lets one call site serve a rig-posed limb and a corpse without
  // deciding which it has.
  if (bi.GetMotionType(id) != JPH::EMotionType::Dynamic) return false;
  float mag = impulse;
  const float mass = BodyMass(handle);
  const float maxSpeed = std::max(CurrentTuning().physics.explosionMaxSpeed, 0.0f);
  if (mass > 0.0f && mag > mass * maxSpeed) mag = mass * maxSpeed;
  const JPH::Vec3 d(dir.x / dl, dir.y / dl, dir.z / dl);
  // ---- A BLOW MAY TURN A BODY OVER; IT MAY NOT MAKE IT A TOP ---------------
  //
  // THE LEVER IS CLAMPED, not the torque. Jolt turns an off-centre impulse
  // into spin through `r x J` with the body's own inverse inertia, and a limb
  // is small: measured on a human thigh, a mace's impulse at the contact point
  // put the body at 47.12 rad/s — which is Jolt's OWN angular clamp, i.e. the
  // solver refusing the number rather than simulating it. A body that sits at
  // that clamp is the armoured-corpse runaway `corpse-armor` was written to
  // catch, and arriving there by hand would be no better.
  //
  // So the impulse keeps its direction and its magnitude — the SHOVE is
  // untouched, and that is the part the player reads as force — and only the
  // arm it acts on is shortened, toward the centre of mass. The blow still
  // spins the body the way the blade went, just not like a rotor.
  // THE LEVER IS SHORTENED so that spin still SCALES with the blow instead of
  // saturating on every one of them...
  constexpr float kMaxLeverM = 0.12f;   // ~1.2 world voxels at 0.1 m/voxel
  const JPH::RVec3 com = bi.GetCenterOfMassPosition(id);
  JPH::Vec3 r(JPH::RVec3(VoxToM(atVoxel.x), VoxToM(atVoxel.y),
                         VoxToM(atVoxel.z)) -
              com);
  const float rl = r.Length();
  if (rl > kMaxLeverM) r = r * (kMaxLeverM / rl);
  bi.ActivateBody(id);
  bi.AddImpulse(id, d * mag, com + JPH::RVec3(r));
  // ...and the RESULT is capped, which is the actual guarantee. A limb's
  // inverse inertia is large (2.4 kg over a third of a metre), so even a short
  // lever can land past Jolt's own 47.12 rad/s clamp — and a body sitting at
  // that clamp is the armoured-corpse runaway, arrived at politely. Capping
  // the outcome needs no inertia math and cannot be defeated by a heavier
  // weapon or a second blow in the same tick.
  constexpr float kMaxSpinRad = 12.0f;   // ~2 turns/s
  const JPH::Vec3 w = bi.GetAngularVelocity(id);
  const float wl = w.Length();
  if (wl > kMaxSpinRad) bi.SetAngularVelocity(id, w * (kMaxSpinRad / wl));
  return true;
}

void Physics::MoveKinematicBody(uint64_t handle, Vec3 posVoxel,
                                const float quat[4], float dt) {
  if (!system_ || handle == 0) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return;
  bi.MoveKinematic(id,
                   JPH::RVec3(VoxToM(posVoxel.x), VoxToM(posVoxel.y),
                              VoxToM(posVoxel.z)),
                   JPH::Quat(quat[0], quat[1], quat[2], quat[3]).Normalized(),
                   std::max(dt, 1e-3f));
}

bool Physics::SetBodyPosition(uint64_t handle, Vec3 posVoxel) {
  if (!system_ || handle == 0) return false;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return false;
  // Rotation kept: the caller is undoing a translation, not a tumble, and
  // re-solving the orientation would fight the joints holding a rig together.
  // EActivation::Activate rather than DontActivate so a body put back at a
  // chunk boundary is still awake to resume the moment the patch lands.
  bi.SetPosition(id,
                 JPH::RVec3(VoxToM(posVoxel.x), VoxToM(posVoxel.y),
                            VoxToM(posVoxel.z)),
                 JPH::EActivation::Activate);
  return true;
}

bool Physics::SetBodyTransform(uint64_t handle, Vec3 posVoxel,
                               const float quat[4]) {
  if (!system_ || handle == 0) return false;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return false;
  JPH::Quat q(quat[0], quat[1], quat[2], quat[3]);
  // A DERIVED POSE MUST NOT BE ABLE TO POISON THE SOLVER. The quaternion here
  // came out of another body's read-back and was composed with an authored
  // offset, so it is normally fine — but the whole point of the FP-trap note
  // above Physics::Step is that "normally fine" is what gets handed to Jolt
  // right before a step dies on it, and SetRotation asserts on a non-unit
  // quat in a Debug build and silently scales the shape in Release.
  const float len2 = q.LengthSq();
  if (!std::isfinite(len2) || len2 < 1.0e-6f) return false;
  q = q.Normalized();
  bi.SetPositionAndRotation(id,
                            JPH::RVec3(VoxToM(posVoxel.x), VoxToM(posVoxel.y),
                                       VoxToM(posVoxel.z)),
                            q, JPH::EActivation::Activate);
  return true;
}

bool Physics::SnapBodyTransform(uint64_t handle, Vec3 posVoxel,
                                const float quat[4]) {
  if (!system_ || handle == 0) return false;
  JPH::Quat q(quat[0], quat[1], quat[2], quat[3]);
  const float len2 = q.LengthSq();  // SetBodyTransform's guard, same reason
  if (!std::isfinite(len2) || len2 < 1.0e-6f) return false;
  q = q.Normalized();
  const JPH::BodyID id = ToBodyID(handle);
  JPH::BodyLockWrite lock(system_->GetBodyLockInterface(), id);
  if (!lock.Succeeded()) return false;
  JPH::Body& body = lock.GetBody();
  if (body.IsStatic() || !body.IsActive() || !body.IsInBroadPhase())
    return false;
  // BodyInterface::SetPositionAndRotation with the one difference that
  // matters: inResetSleepTimer false. The broadphase is told exactly as it
  // tells it (the query interface IS the broadphase; Jolt only hands it out
  // const).
  body.SetPositionAndRotationInternal(
      JPH::RVec3(VoxToM(posVoxel.x), VoxToM(posVoxel.y), VoxToM(posVoxel.z)), q,
      /*inResetSleepTimer=*/false);
  JPH::BroadPhase& bp = static_cast<JPH::BroadPhase&>(
      const_cast<JPH::BroadPhaseQuery&>(system_->GetBroadPhaseQuery()));
  JPH::BodyID ids[1] = {id};
  bp.NotifyBodiesAABBChanged(ids, 1);
  return true;
}

// A velocity written from game code is the one input to the solver this engine
// controls, so it is also the one place a non-number can be kept out of Jolt
// for free. A value that fails here is DROPPED, not clamped: it is not a fast
// body, it is a bug upstream, and the report names it. See the FP-trap note
// above Physics::Step for what happens to a body that carries one.
bool Physics::VelocityIsSane(Vec3 linVox, Vec3 angRad, const char* what) {
  const JPH::Vec3 l(linVox.x, linVox.y, linVox.z);
  const JPH::Vec3 a(angRad.x, angRad.y, angRad.z);
  if (ComponentsUnder(l, kInsaneSpeed) && ComponentsUnder(a, kInsaneSpin))
    return true;
  if (insaneReports_ < 8) {
    insaneReports_++;
    std::fprintf(stderr,
                 "[phys] %s refused: lin (%g, %g, %g) vox/s, ang (%g, %g, %g) "
                 "rad/s is not a velocity. See the FP-trap note in "
                 "phys/physics.cpp.\n",
                 what, (double)linVox.x, (double)linVox.y, (double)linVox.z,
                 (double)angRad.x, (double)angRad.y, (double)angRad.z);
  }
  return false;
}

void Physics::SetBodyVelocity(uint64_t handle, Vec3 velVoxelsPerSec) {
  if (!system_ || handle == 0) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return;
  if (!VelocityIsSane(velVoxelsPerSec, Vec3{}, "SetBodyVelocity")) return;
  bi.SetLinearVelocity(id, JPH::Vec3(VoxToM(velVoxelsPerSec.x),
                                     VoxToM(velVoxelsPerSec.y),
                                     VoxToM(velVoxelsPerSec.z)));
}

float Physics::BodyMass(uint64_t handle) const {
  if (!system_ || handle == 0) return 0.0f;
  JPH::BodyID id = ToBodyID(handle);
  // Unchecked, for the same reason FrictionTorque uses it: the checked
  // accessor asserts on a kinematic body, and the mass it holds is right.
  JPH::BodyLockRead lock(system_->GetBodyLockInterface(), id);
  if (!lock.Succeeded()) return 0.0f;
  const JPH::MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked();
  if (!mp) return 0.0f;
  const float inv = mp->GetInverseMassUnchecked();
  return inv > 0.0f ? 1.0f / inv : 0.0f;
}

int Physics::BodyObjectLayer(uint64_t handle) const {
  if (!system_ || handle == 0) return -1;
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return -1;
  return (int)bi.GetObjectLayer(id);
}

bool Physics::BodyCenterOfMass(uint64_t handle, Vec3& outVoxel) const {
  if (!system_ || handle == 0) return false;
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return false;
  JPH::RVec3 p = bi.GetCenterOfMassPosition(id);
  const float inv = 1.0f / kVoxelMeters;
  outVoxel = Vec3{(float)p.GetX() * inv, (float)p.GetY() * inv,
                  (float)p.GetZ() * inv};
  return true;
}

bool Physics::GetBodyVelocities(uint64_t handle, Vec3& lin,
                                Vec3& angRadPerSec) const {
  if (!system_ || handle == 0) return false;
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return false;
  JPH::Vec3 l = bi.GetLinearVelocity(id);
  JPH::Vec3 a = bi.GetAngularVelocity(id);
  lin = Vec3{l.GetX(), l.GetY(), l.GetZ()} * (1.0f / kVoxelMeters);
  angRadPerSec = Vec3{a.GetX(), a.GetY(), a.GetZ()};
  return true;
}

void Physics::SetBodyVelocities(uint64_t handle, Vec3 lin, Vec3 angRadPerSec) {
  if (!system_ || handle == 0) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return;
  if (!VelocityIsSane(lin, angRadPerSec, "SetBodyVelocities")) return;
  bi.SetLinearVelocity(id, JPH::Vec3(VoxToM(lin.x), VoxToM(lin.y), VoxToM(lin.z)));
  bi.SetAngularVelocity(id, JPH::Vec3(angRadPerSec.x, angRadPerSec.y, angRadPerSec.z));
}

bool Physics::ApplyBuoyancy(uint64_t handle, float surfaceYVoxel, float buoyancy,
                            float linearDrag, float angularDrag, float dt) {
  if (!system_ || handle == 0 || dt <= 0.0f) return false;
  JPH::BodyLockWrite lock(system_->GetBodyLockInterface(), ToBodyID(handle));
  if (!lock.Succeeded()) return false;
  JPH::Body& body = lock.GetBody();
  // Rigid and dynamic: ApplyBuoyancyImpulse asserts the first and dereferences
  // the motion properties the second guarantees. ACTIVE as well — a velocity
  // step written into a sleeping body is discarded, and waking it to receive
  // one is how a raft at rest would come to cost forever.
  if (!body.IsRigidBody() || !body.IsDynamic() || !body.IsActive()) return false;
  // A flat surface at the waterline. The x/z of the surface POSITION are
  // irrelevant to a horizontal plane and Jolt only uses it to place the plane,
  // so 0 is not a hidden assumption about where the water is.
  const JPH::RVec3 surface(0.0f, VoxToM(surfaceYVoxel), 0.0f);
  return body.ApplyBuoyancyImpulse(surface, JPH::Vec3(0, 1, 0), buoyancy,
                                   linearDrag, angularDrag, JPH::Vec3::sZero(),
                                   system_->GetGravity(), dt);
}

// ---- ROLE -> LAYER (W2-N; the table is in physics.h) -----------------------

const char* Physics::RoleName(BodyRole r) {
  switch (r) {
    case BodyRole::Debris: return "debris";
    case BodyRole::RigLive: return "rig-live";
    case BodyRole::RigLimp: return "rig-limp";
    case BodyRole::RigDead: return "rig-dead";
    case BodyRole::WornShell: return "worn-shell";
    case BodyRole::HeldProp: return "held-prop";
    case BodyRole::Carried: return "carried";
    case BodyRole::SeveredHold: return "severed-hold";
    case BodyRole::Thrown: return "thrown";
    case BodyRole::Door: return "door";
    default: return "?";
  }
}

int Physics::ResolveLayer(BodyRole role, int ownerSlot, bool clearing) {
  switch (role) {
    case BodyRole::HeldProp:
      return Layers::PROP;
    case BodyRole::Door:
      return Layers::DOOR;
    case BodyRole::SeveredHold:
      return Layers::EXEMPT;
    case BodyRole::Thrown:
      if (!clearing) return Layers::MOVING;
      return ownerSlot >= 0 ? Layers::THROWN_BASE + ownerSlot : Layers::THROWN;
    case BodyRole::RigLive:
    case BodyRole::WornShell:
    case BodyRole::Carried:
      if (clearing || ownerSlot == -1) return Layers::EXEMPT;
      return ownerSlot >= 0 ? Layers::OWNED_BASE + ownerSlot : Layers::MOVING;
    default:  // Debris, RigLimp, RigDead: the loose roles
      return clearing ? Layers::EXEMPT : Layers::MOVING;
  }
}

int Physics::PlayerSlotOf(uint64_t proxy) const {
  if (!system_ || proxy == 0) return -1;
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  const JPH::BodyID id = ToBodyID(proxy);
  if (!bi.IsAdded(id)) return -1;
  return Layers::PlayerSlot(bi.GetObjectLayer(id));
}

int Physics::OwnerSlot(uint64_t owner) const {
  if (owner == kNoOwner) return -2;
  if (owner == kAnyPlayer) return -1;
  return PlayerSlotOf(owner);  // -1 for a dead or unslotted proxy: exempt from all
}

bool Physics::AnyLivePlayer() const {
  // "Nobody" is "not one live proxy among them" -- a dead handle that
  // RemoveBody never saw (Shutdown order, a Jolt-side destroy) must not count
  // as a player to hide behind.
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  for (uint64_t p : playerBodies_)
    if (p != 0 && bi.IsAdded(ToBodyID(p))) return true;
  return false;
}

void Physics::SettleBody(uint64_t handle) {
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  const JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return;
  const uint64_t ud = bi.GetUserData(id);
  BodyRole role = UnpackRole(ud);
  const uint64_t owner = UnpackOwner(ud);
  // A throw that has cleared its thrower is a thing lying in the world.
  if (role == BodyRole::Thrown) {
    role = BodyRole::Debris;
    bi.SetUserData(id, PackRole(role, owner));
  }
  // Every layer maps to BP::MOVING, so no transition here needs a broadphase
  // rebuild.
  bi.SetObjectLayer(id, (JPH::ObjectLayer)ResolveLayer(role, OwnerSlot(owner), false));
}

void Physics::AddPending(uint64_t handle) {
  for (uint64_t h : pendingRelease_)
    if (h == handle) return;
  if (pendingRelease_.size() >= kMaxPendingRelease) {
    // Bounded (CLAUDE.md rule 2): the oldest settles now, clear or not. At 256
    // simultaneous pieces inside one player something else is already wrong.
    const uint64_t old = pendingRelease_.front();
    pendingRelease_.erase(pendingRelease_.begin());
    SettleBody(old);
  }
  pendingRelease_.push_back(handle);
}

void Physics::SetBodyRole(uint64_t handle, BodyRole role, uint64_t owner) {
  if (!system_ || handle == 0) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  const JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return;
  bi.SetUserData(id, PackRole(role, owner));
  const int slot = OwnerSlot(owner);
  auto pendingAt = std::find(pendingRelease_.begin(), pendingRelease_.end(), handle);
  const bool pending = pendingAt != pendingRelease_.end();
  if (IsLooseRole(role)) {
    // CLEARING: every loose role enters by waiting until clear of every
    // player — or settles at once when there is nobody to protect.
    if (!AnyLivePlayer()) {
      if (pending) pendingRelease_.erase(pendingAt);
      SettleBody(handle);
      return;
    }
    bi.SetObjectLayer(id, (JPH::ObjectLayer)ResolveLayer(role, slot, true));
    AddPending(handle);
    return;
  }
  // ATTACHED: at once. A body still clearing keeps clearing if the role it
  // was given could be felt by some player (MOVING, or OWNED by one of them) —
  // a limp NPC standing back up inside the player is still inside the player —
  // and settles on that role's layer when clear.
  const JPH::ObjectLayer settled = (JPH::ObjectLayer)ResolveLayer(role, slot, false);
  const bool felt = settled == Layers::MOVING || Layers::IsOwned(settled);
  if (pending && felt) {
    bi.SetObjectLayer(id, (JPH::ObjectLayer)ResolveLayer(role, slot, true));
    return;
  }
  if (pending) pendingRelease_.erase(pendingAt);
  bi.SetObjectLayer(id, settled);
}

Physics::BodyRole Physics::BodyRoleOf(uint64_t handle) const {
  if (!system_ || handle == 0) return BodyRole::Debris;
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  const JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return BodyRole::Debris;
  return UnpackRole(bi.GetUserData(id));
}

uint64_t Physics::BodyOwnerOf(uint64_t handle) const {
  if (!system_ || handle == 0) return kNoOwner;
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  const JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return kNoOwner;
  return UnpackOwner(bi.GetUserData(id));
}

bool Physics::BodyClearing(uint64_t handle) const {
  return handle != 0 && std::find(pendingRelease_.begin(), pendingRelease_.end(),
                                  handle) != pendingRelease_.end();
}

bool Physics::LayersCollide(uint64_t a, uint64_t b) const {
  if (!system_ || !layers_) return false;
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  const JPH::BodyID ia = ToBodyID(a), ib = ToBodyID(b);
  if (a == 0 || b == 0 || !bi.IsAdded(ia) || !bi.IsAdded(ib)) return false;
  return layers_->objPair.ShouldCollide(bi.GetObjectLayer(ia), bi.GetObjectLayer(ib));
}

bool Physics::WorldBounds(uint64_t handle, float outMin[3],
                          float outMax[3]) const {
  if (!system_ || handle == 0) return false;
  const JPH::BodyLockInterface& bli = system_->GetBodyLockInterface();
  JPH::BodyLockRead lock(bli, ToBodyID(handle));
  if (!lock.Succeeded()) return false;
  const JPH::AABox b = lock.GetBody().GetWorldSpaceBounds();
  outMin[0] = b.mMin.GetX(); outMin[1] = b.mMin.GetY(); outMin[2] = b.mMin.GetZ();
  outMax[0] = b.mMax.GetX(); outMax[1] = b.mMax.GetY(); outMax[2] = b.mMax.GetZ();
  return true;
}

void Physics::TickPendingReleases() {
  if (pendingRelease_.empty() || !system_) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  // One AABB per live proxy, gathered once for the whole sweep rather than per
  // pending body: this runs every physics tick over up to kMaxPendingRelease
  // (256) entries, and a body lock per (piece x player) would be the cost of
  // the whole function. Players are a handful; pieces are not.
  float pmin[kMaxPlayerProxies][3], pmax[kMaxPlayerProxies][3];
  int nPlayers = 0;
  for (uint64_t p : playerBodies_) {
    if (nPlayers >= kMaxPlayerProxies) break;
    if (p == 0) continue;
    if (!WorldBounds(p, pmin[nPlayers], pmax[nPlayers])) continue;
    nPlayers++;
  }
  // A hand's breadth of clearance, so a body resting AGAINST the capsule does
  // not flip layers on the tick it touches and shove on the next. Metres.
  constexpr float kMargin = 0.05f;
  size_t w = 0;
  for (size_t i = 0; i < pendingRelease_.size(); i++) {
    const uint64_t h = pendingRelease_[i];
    const JPH::BodyID id = ToBodyID(h);
    if (!bi.IsAdded(id)) continue;  // dead: forget it
    float bmin[3], bmax[3];
    bool overlaps = false;
    if (nPlayers > 0 && WorldBounds(h, bmin, bmax)) {
      // INSIDE ANY player holds the piece. A piece released the moment it left
      // player 0 while still buried in player 1 would be shoved out of them at
      // full contact force; the EXEMPT layer exists precisely to stop that.
      for (int pi = 0; pi < nPlayers && !overlaps; pi++) {
        bool hit = true;
        for (int a = 0; a < 3; a++)
          if (bmax[a] + kMargin < pmin[pi][a] || bmin[a] - kMargin > pmax[pi][a])
            hit = false;
        overlaps = hit;
      }
    }
    if (overlaps) {
      pendingRelease_[w++] = h;
      continue;
    }
    // Clear: onto the layer its ROLE resolves to — MOVING for anything loose,
    // but OWNED/PROP for a body that was given an attached role while it was
    // still clearing (SetBodyRole). This used to be MOVING unconditionally,
    // which is how a get-up NPC's re-held sword could be dropped back onto
    // MOVING by the release its ragdoll had left pending.
    SettleBody(h);
  }
  pendingRelease_.resize(w);
}

void Physics::DisableCollisionsAmong(const std::vector<uint64_t>& handles) {
  if (!system_ || handles.size() < 2) return;
  JPH::Ref<JPH::GroupFilterTable> table =
      new JPH::GroupFilterTable((uint32_t)handles.size());
  for (uint32_t i = 1; i < (uint32_t)handles.size(); i++)
    for (uint32_t j = 0; j < i; j++) table->DisableCollision(i, j);
  uint32_t gid = nextCollisionGroup_++;
  const JPH::BodyLockInterface& bli = system_->GetBodyLockInterface();
  for (uint32_t i = 0; i < (uint32_t)handles.size(); i++) {
    JPH::BodyLockWrite lock(bli, ToBodyID(handles[i]));
    if (lock.Succeeded())
      lock.GetBody().SetCollisionGroup(JPH::CollisionGroup(table, gid, i));
  }
}

void Physics::ClearCollisionGroup(uint64_t handle) {
  if (!system_ || handle == 0) return;
  // CollisionGroup::sInvalidGroup with a null filter = "collides with
  // everything", which is what an adopted debris body should do.
  const JPH::BodyLockInterface& bli = system_->GetBodyLockInterface();
  JPH::BodyLockWrite lock(bli, ToBodyID(handle));
  if (lock.Succeeded()) lock.GetBody().SetCollisionGroup(JPH::CollisionGroup());
}

uint64_t Physics::CastRayBody(Vec3 fromVoxel, Vec3 dirNormalized,
                              float maxDistVoxels, float& fraction) const {
  fraction = 1.0f;
  if (!system_) return 0;
  JPH::RRayCast ray(JPH::RVec3(VoxToM(fromVoxel.x), VoxToM(fromVoxel.y),
                               VoxToM(fromVoxel.z)),
                    JPH::Vec3(dirNormalized.x, dirNormalized.y,
                              dirNormalized.z) *
                        VoxToM(maxDistVoxels));
  JPH::RayCastResult hit;
  // Both dynamic layers: the avatar's limbs sit on AVATAR rather than MOVING
  // so they cannot shove the player proxy, but a laser must still be able to
  // hit them — the split is about CONTACTS, not about visibility to queries.
  DynamicLayerFilter dynamicOnly;
  if (!system_->GetNarrowPhaseQuery().CastRay(ray, hit, {}, dynamicOnly))
    return 0;
  fraction = hit.mFraction;
  return FromBodyID(hit.mBodyID);
}

uint64_t Physics::CastRayBody(Vec3 fromVoxel, Vec3 dirNormalized,
                              float maxDistVoxels, float& fraction,
                              const std::vector<uint64_t>& ignore) const {
  fraction = 1.0f;
  if (!system_) return 0;
  JPH::RRayCast ray(JPH::RVec3(VoxToM(fromVoxel.x), VoxToM(fromVoxel.y),
                               VoxToM(fromVoxel.z)),
                    JPH::Vec3(dirNormalized.x, dirNormalized.y,
                              dirNormalized.z) *
                        VoxToM(maxDistVoxels));
  JPH::RayCastResult hit;
  DynamicLayerFilter dynamicOnly;
  JPH::IgnoreMultipleBodiesFilter skip;
  skip.Reserve((JPH::uint)ignore.size());
  for (uint64_t h : ignore)
    if (h) skip.IgnoreBody(ToBodyID(h));
  if (!system_->GetNarrowPhaseQuery().CastRay(ray, hit, {}, dynamicOnly, skip))
    return 0;
  fraction = hit.mFraction;
  return FromBodyID(hit.mBodyID);
}

void Physics::RemoveBody(uint64_t handle) {
  if (!system_ || handle == 0) return;
  // joints attached to this body die with it (Jolt asserts otherwise)
  if (joints_) {
    auto bit = joints_->byBody.find(handle);
    if (bit != joints_->byBody.end()) {
      std::vector<uint64_t> attached = bit->second;  // DestroyJoint mutates
      for (uint64_t j : attached) DestroyJoint(j);
    }
  }
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  // JOLT REUSES BODY INDICES, and SweepRunawayRigs keys its escalation counter
  // on one. A body removed while hot would otherwise hand a count of up to 44
  // to whatever is created next in its slot, and that body would be cut on its
  // fifth bad step instead of its forty-fifth.
  hotSteps_.erase(id.GetIndex());
  const auto tr0 = std::chrono::steady_clock::now();
  bi.RemoveBody(id);
  bi.DestroyBody(id);
  FreeDet(ids_, id);
  meshStats_.removes++;
  meshStats_.removeUs += std::chrono::duration<double, std::micro>(
                             std::chrono::steady_clock::now() - tr0).count();
  for (size_t i = 0; i < playerBodies_.size(); i++) {
    if (playerBodies_[i] == handle) {
      playerBodies_.erase(playerBodies_.begin() + (ptrdiff_t)i);
      break;
    }
  }
  for (size_t i = 0; i < pendingRelease_.size(); i++) {
    if (pendingRelease_[i] == handle) {
      pendingRelease_.erase(pendingRelease_.begin() + (ptrdiff_t)i);
      break;
    }
  }
  for (size_t i = 0; i < dynamicBodies_.size(); i++) {
    if (dynamicBodies_[i] == handle) {
      dynamicBodies_[i] = dynamicBodies_.back();
      dynamicBodies_.pop_back();
      break;
    }
  }
}

bool Physics::GetTransform(uint64_t handle, BodyTransform& out) const {
  if (!system_ || handle == 0) return false;
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return false;
  JPH::RVec3 p;
  JPH::Quat q;
  bi.GetPositionAndRotation(id, p, q);
  out.pos = Vec3{(float)p.GetX() / kVoxelMeters, (float)p.GetY() / kVoxelMeters,
                 (float)p.GetZ() / kVoxelMeters};
  out.quat[0] = q.GetX();
  out.quat[1] = q.GetY();
  out.quat[2] = q.GetZ();
  out.quat[3] = q.GetW();
  return true;
}

bool Physics::GetLocalBounds(uint64_t handle, Vec3& outMin, Vec3& outMax) const {
  if (!system_ || handle == 0) return false;
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return false;
  JPH::RefConst<JPH::Shape> shape = bi.GetShape(id);
  if (!shape) return false;
  const JPH::AABox b = shape->GetLocalBounds();
  const JPH::Vec3 com = shape->GetCenterOfMass();
  const float inv = 1.0f / kVoxelMeters;
  outMin = Vec3{(b.mMin.GetX() + com.GetX()) * inv,
                (b.mMin.GetY() + com.GetY()) * inv,
                (b.mMin.GetZ() + com.GetZ()) * inv};
  outMax = Vec3{(b.mMax.GetX() + com.GetX()) * inv,
                (b.mMax.GetY() + com.GetY()) * inv,
                (b.mMax.GetZ() + com.GetZ()) * inv};
  return true;
}

size_t Physics::GetSubShapeBoxes(uint64_t handle,
                                 std::vector<SubShapeBox>& out,
                                 size_t limit) const {
  if (!system_ || handle == 0) return 0;
  const JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return 0;
  JPH::RefConst<JPH::Shape> shape = bi.GetShape(id);
  if (!shape || out.size() >= limit) return 0;

  const float inv = 1.0f / kVoxelMeters;
  const JPH::Vec3 com = shape->GetCenterOfMass();

  // Jolt optimizes a single-sub-shape compound into a bare BoxShape or a
  // RotatedTranslatedShape wrapping one. Handle both so those limbs also
  // get tight-fitting boxes instead of falling back to the AABB.
  if (shape->GetSubType() == JPH::EShapeSubType::Box) {
    const auto* box = static_cast<const JPH::BoxShape*>(shape.GetPtr());
    JPH::Vec3 half = box->GetHalfExtent();
    SubShapeBox b;
    b.center = Vec3{com.GetX() * inv, com.GetY() * inv, com.GetZ() * inv};
    b.halfExtents = Vec3{half.GetX() * inv, half.GetY() * inv,
                         half.GetZ() * inv};
    b.quat[0] = 0; b.quat[1] = 0; b.quat[2] = 0; b.quat[3] = 1;
    out.push_back(b);
    return 1;
  }

  if (shape->GetSubType() == JPH::EShapeSubType::RotatedTranslated) {
    const auto* rt = static_cast<const JPH::RotatedTranslatedShape*>(
        shape.GetPtr());
    const JPH::Shape* inner = rt->GetInnerShape();
    if (inner && inner->GetSubType() == JPH::EShapeSubType::Box) {
      const auto* box = static_cast<const JPH::BoxShape*>(inner);
      JPH::Vec3 pos = rt->GetPosition() + com;
      JPH::Quat rot = rt->GetRotation();
      JPH::Vec3 half = box->GetHalfExtent();
      SubShapeBox b;
      b.center = Vec3{pos.GetX() * inv, pos.GetY() * inv,
                       pos.GetZ() * inv};
      b.halfExtents = Vec3{half.GetX() * inv, half.GetY() * inv,
                           half.GetZ() * inv};
      b.quat[0] = rot.GetX(); b.quat[1] = rot.GetY();
      b.quat[2] = rot.GetZ(); b.quat[3] = rot.GetW();
      out.push_back(b);
      return 1;
    }
    return 0;
  }

  if (shape->GetSubType() != JPH::EShapeSubType::StaticCompound) return 0;
  const auto* compound =
      static_cast<const JPH::StaticCompoundShape*>(shape.GetPtr());
  size_t added = 0;
  for (uint32_t i = 0; i < compound->GetNumSubShapes(); i++) {
    if (out.size() >= limit) break;
    const JPH::CompoundShape::SubShape& ss = compound->GetSubShape(i);
    if (ss.mShape->GetSubType() != JPH::EShapeSubType::Box) continue;
    const auto* box = static_cast<const JPH::BoxShape*>(ss.mShape.GetPtr());
    JPH::Vec3 pos = ss.GetPositionCOM() + com;
    JPH::Vec3 half = box->GetHalfExtent();
    JPH::Quat rot = ss.GetRotation();
    SubShapeBox b;
    b.center = Vec3{pos.GetX() * inv, pos.GetY() * inv, pos.GetZ() * inv};
    b.halfExtents = Vec3{half.GetX() * inv, half.GetY() * inv,
                         half.GetZ() * inv};
    b.quat[0] = rot.GetX(); b.quat[1] = rot.GetY();
    b.quat[2] = rot.GetZ(); b.quat[3] = rot.GetW();
    out.push_back(b);
    added++;
  }
  return added;
}

bool Physics::IsActive(uint64_t handle) const {
  if (!system_ || handle == 0) return false;
  return system_->GetBodyInterface().IsActive(ToBodyID(handle));
}

void Physics::DeactivateBody(uint64_t handle) {
  if (!system_ || handle == 0) return;
  system_->GetBodyInterface().DeactivateBody(ToBodyID(handle));
}

void Physics::ActivateBody(uint64_t handle) {
  if (!system_ || handle == 0) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (bi.IsAdded(id)) bi.ActivateBody(id);
}

void Physics::ApplyRadialImpulse(Vec3 centerVoxel, float radiusVoxels,
                                 float impulse,
                                 const std::vector<uint64_t>* skipSorted) {
  if (!system_) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::RVec3 c(VoxToM(centerVoxel.x), VoxToM(centerVoxel.y), VoxToM(centerVoxel.z));
  float rM = VoxToM(radiusVoxels);
  for (uint64_t h : dynamicBodies_) {
    if (skipSorted && std::binary_search(skipSorted->begin(), skipSorted->end(), h)) continue;
    JPH::BodyID id = ToBodyID(h);
    if (!bi.IsAdded(id)) continue;
    JPH::RVec3 p = bi.GetCenterOfMassPosition(id);
    JPH::Vec3 d = JPH::Vec3(p - c);
    float dist = d.Length();
    if (dist > rM) continue;
    JPH::Vec3 dir = dist > 1e-4f ? d / dist : JPH::Vec3(0, 1, 0);
    float falloff = 1.0f - dist / rM;
    // Bound the SPEED this impulse buys, not the impulse: impulse / mass on
    // a 0.05 kg gobbet is 1000 m/s (physics.explosionMaxSpeed).
    float mag = impulse * falloff;
    const float mass = BodyMass(h);
    const float maxSpeed = std::max(CurrentTuning().physics.explosionMaxSpeed, 0.0f);
    if (mass > 0.0f && mag > mass * maxSpeed) mag = mass * maxSpeed;
    bi.ActivateBody(id);
    bi.AddImpulse(id, dir * mag);
  }
}

void Physics::WakeNear(Vec3 centerVoxel, float radiusVoxels) {
  if (!system_) return;
  // The NO-LOCK interface: this runs on the game thread between Steps (the
  // terrain sweep calls it once per rebuilt patch, the island drain once per
  // scan), where nothing else can touch a body, and the locking interface
  // took a body lock three times per body per call.
  JPH::BodyInterface& bi = system_->GetBodyInterfaceNoLock();
  JPH::RVec3 c(VoxToM(centerVoxel.x), VoxToM(centerVoxel.y), VoxToM(centerVoxel.z));
  float rM = VoxToM(radiusVoxels);
  const auto t0 = std::chrono::steady_clock::now();
  meshStats_.wakeCalls++;
  for (uint64_t h : dynamicBodies_) {
    JPH::BodyID id = ToBodyID(h);
    if (!bi.IsAdded(id)) continue;
    if (JPH::Vec3(bi.GetCenterOfMassPosition(id) - c).Length() <= rM) {
      if (!bi.IsActive(id)) meshStats_.woken++;
      bi.ActivateBody(id);
    }
  }
  meshStats_.wakeUs += std::chrono::duration<double, std::micro>(
                           std::chrono::steady_clock::now() - t0).count();
}

void Physics::WakeInBox(Vec3 loVoxel, Vec3 hiVoxel) {
  if (!system_) return;
  const auto t0 = std::chrono::steady_clock::now();
  meshStats_.wakeCalls++;
  const JPH::AABox box(JPH::Vec3(VoxToM(loVoxel.x), VoxToM(loVoxel.y), VoxToM(loVoxel.z)),
                       JPH::Vec3(VoxToM(hiVoxel.x), VoxToM(hiVoxel.y), VoxToM(hiVoxel.z)));
  const uint32_t before = system_->GetNumActiveBodies(JPH::EBodyType::RigidBody);
  system_->GetBodyInterfaceNoLock().ActivateBodiesInAABox(
      box, JPH::BroadPhaseLayerFilter{}, JPH::ObjectLayerFilter{});
  const uint32_t after = system_->GetNumActiveBodies(JPH::EBodyType::RigidBody);
  if (after > before) meshStats_.woken += after - before;
  meshStats_.wakeUs += std::chrono::duration<double, std::micro>(
                           std::chrono::steady_clock::now() - t0).count();
}

uint32_t Physics::NumActiveBodies() const {
  return system_ ? system_->GetNumActiveBodies(JPH::EBodyType::RigidBody) : 0;
}
