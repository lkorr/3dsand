#include "phys/physics.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
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
constexpr JPH::ObjectLayer PLAYER = 2;
// PLAYER AVATAR LIMBS. Identical to MOVING in every respect EXCEPT that it
// does not collide with PLAYER. The avatar is drawn AROUND the player's own
// capsule by construction (avatar.cpp derives origin_ from player.pos), so on
// MOVING every limb is permanently interpenetrated with the proxy. That fed
// the solver a contact it could never resolve and fed PlayerPushOut a large
// depenetration vector whose direction swung with the gait — which is exactly
// "walking forward moves me backwards/diagonally, sporadically". Your own body
// must never be able to push you.
constexpr JPH::ObjectLayer AVATAR = 3;
constexpr JPH::ObjectLayer NUM = 4;
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
    if (layer == Layers::PLAYER) return bp == BP::MOVING;  // proxy: bodies only
    return true;
  }
};

class ObjPairFilter final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    // The avatar's own limbs never touch the player proxy they live inside.
    if ((a == Layers::PLAYER && b == Layers::AVATAR) ||
        (a == Layers::AVATAR && b == Layers::PLAYER))
      return false;
    if (a == Layers::PLAYER || b == Layers::PLAYER)
      return a == Layers::MOVING || b == Layers::MOVING;
    return !(a == Layers::STATIC && b == Layers::STATIC);
  }
};

// "Any body a query should be able to see", i.e. both dynamic layers.
// SpecifiedObjectLayerFilter takes a single layer, which stopped being enough
// once the avatar moved off MOVING.
class DynamicLayerFilter final : public JPH::ObjectLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer layer) const override {
    return layer == Layers::MOVING || layer == Layers::AVATAR;
  }
};

float VoxToM(float v) { return v * kVoxelMeters; }

JPH::BodyID ToBodyID(uint64_t h) { return JPH::BodyID((uint32_t)(h - 1)); }
uint64_t FromBodyID(JPH::BodyID id) { return (uint64_t)id.GetIndexAndSequenceNumber() + 1; }

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
  float minSpeedVox = 0.0f;  // latched by Step; read-only during Update

  ContactImpls() { impacts.reserve(kMaxPerStep); }

  void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                      const JPH::ContactManifold& m,
                      JPH::ContactSettings&) override {
    // YOUR OWN BODY MUST NOT FIRE DEBRIS IMPACTS. Layers::AVATAR exists
    // precisely to split the player's limbs out of contact handling, and the
    // player proxy is teleported onto the player every tick so its contacts
    // are an artifact of that, not of anything landing.
    const JPH::ObjectLayer l1 = b1.GetObjectLayer(), l2 = b2.GetObjectLayer();
    for (JPH::ObjectLayer l : {l1, l2})
      if (l == Layers::AVATAR || l == Layers::PLAYER) return;
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

    std::lock_guard<std::mutex> lk(mu);
    if (impacts.size() < kMaxPerStep) impacts.push_back(ci);
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
  };
  std::unordered_map<uint64_t, Entry> joints;                 // handle -> entry
  std::unordered_map<uint64_t, std::vector<uint64_t>> byBody; // body -> joints
};

Physics::Physics() = default;
Physics::~Physics() { Shutdown(); }

bool Physics::Init() {
  JPH::RegisterDefaultAllocator();
  if (!JPH::Factory::sInstance) {
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
  }
  tempAlloc_ = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
  jobs_ = std::make_unique<JPH::JobSystemThreadPool>(
      JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 2 /*threads*/);
  layers_ = std::make_unique<LayerImpls>();
  joints_ = std::make_unique<JointImpls>();
  contacts_ = std::make_unique<ContactImpls>();

  system_ = std::make_unique<JPH::PhysicsSystem>();
  system_->Init(4096 /*max bodies*/, 0, 4096, 2048, layers_->bpInterface,
                layers_->objVsBp, layers_->objPair);
  system_->SetContactListener(contacts_.get());
  system_->SetGravity(JPH::Vec3(0, -CurrentTuning().physics.gravity, 0));
  return true;
}

void Physics::Shutdown() {
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

void Physics::SetContactReportSpeed(float voxPerSec) {
  // Below zero would report every resting contact in the world; a caller that
  // wants the listener off passes a huge number, not a negative one.
  if (contacts_) contacts_->minSpeedVox = std::max(0.0f, voxPerSec);
}

void Physics::Step(float dt) {
  if (!system_) return;
  // Gravity is re-applied here rather than only at Init so a tuning reload
  // takes effect without restarting the world.
  system_->SetGravity(JPH::Vec3(0, -CurrentTuning().physics.gravity, 0));
  // Contacts belong to the step that produced them: clear on the GAME THREAD
  // before Update hands the buffer to the job threads, so a caller reading
  // after Step sees exactly that step and nothing accumulates when nobody
  // drains (a headless run never reads this at all).
  if (contacts_) contacts_->impacts.clear();
  system_->Update(dt, CurrentTuning().physics.collisionSteps, tempAlloc_.get(),
                  jobs_.get());
}

uint64_t Physics::CreateDebrisBody(const std::vector<DebrisVoxel>& voxels,
                                   IVec3 originVoxel,
                                   const std::vector<float>& densityOfMat,
                                   bool allowKinematic, float voxelPitch) {
  BodyTransform xf{};
  xf.pos = Vec3{(float)originVoxel.x, (float)originVoxel.y, (float)originVoxel.z};
  xf.quat[3] = 1;
  return CreateDebrisBodyXf(voxels, xf, densityOfMat, allowKinematic, voxelPitch);
}

uint64_t Physics::CreateDebrisBodyXf(const std::vector<DebrisVoxel>& voxels,
                                     const BodyTransform& xf,
                                     const std::vector<float>& densityOfMat,
                                     bool allowKinematic, float voxelPitch) {
  if (!system_ || voxels.empty()) return 0;
  if (!(voxelPitch > 0.0f)) voxelPitch = 1.0f;

  // greedy box merge over the local voxel set (runs in +x, extended in +y,
  // then +z) — keeps compound shapes small for compact islands
  std::unordered_map<uint64_t, uint16_t> cell;  // packed local -> voxel index
  auto key = [](int x, int y, int z) {
    return ((uint64_t)(uint8_t)x << 16) | ((uint64_t)(uint8_t)y << 8) | (uint8_t)z;
  };
  int maxc[3] = {0, 0, 0};
  for (size_t i = 0; i < voxels.size(); i++) {
    const DebrisVoxel& v = voxels[i];
    cell[key(v.x, v.y, v.z)] = (uint16_t)i;
    maxc[0] = std::max(maxc[0], (int)v.x);
    maxc[1] = std::max(maxc[1], (int)v.y);
    maxc[2] = std::max(maxc[2], (int)v.z);
  }
  std::unordered_map<uint64_t, bool> used;
  auto has = [&](int x, int y, int z) {
    return x >= 0 && y >= 0 && z >= 0 &&
           cell.count(key(x, y, z)) && !used[key(x, y, z)];
  };

  JPH::StaticCompoundShapeSettings compound;
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

  int boxes = 0;
  for (const DebrisVoxel& v : voxels) {
    if (used[key(v.x, v.y, v.z)]) continue;
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
        for (int i = 0; i < sx; i++) used[key(v.x + i, v.y + j, v.z + k)] = true;

    JPH::Vec3 half(VoxToM(sx * 0.5f * voxelPitch), VoxToM(sy * 0.5f * voxelPitch),
                   VoxToM(sz * 0.5f * voxelPitch));
    JPH::Vec3 center(VoxToM((v.x + sx * 0.5f) * voxelPitch),
                     VoxToM((v.y + sy * 0.5f) * voxelPitch),
                     VoxToM((v.z + sz * 0.5f) * voxelPitch));
    // Tiny convex radius: debris voxels are 12.5 cm. Scale it with the pitch
    // too — a fixed 1 cm skin on a 3 cm micro voxel is a third of the box, and
    // Jolt would round the limb off into a lump.
    compound.AddShape(center, JPH::Quat::sIdentity(),
                      new JPH::BoxShape(half, 0.01f * voxelPitch));
    boxes++;
    if (boxes >= 1024) break;  // pathological shapes get a truncated collider
  }

  auto shapeResult = compound.Create();
  if (shapeResult.HasError()) {
    std::fprintf(stderr, "debris shape error: %s\n",
                 shapeResult.GetError().c_str());
    return 0;
  }

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
  // Ghost contacts with internal edges of the marching-cubes terrain are what
  // make debris snag and hop on flat-looking ground; this is Jolt's fix.
  bcs.mEnhancedInternalEdgeRemoval = true;

  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::Activate);
  if (id.IsInvalid()) return 0;
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
  bcs.mEnhancedInternalEdgeRemoval = true;

  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::Activate);
  if (id.IsInvalid()) return 0;
  uint64_t h = FromBodyID(id);
  dynamicBodies_.push_back(h);
  return h;
}

uint64_t Physics::CreateTerrainMesh(const std::vector<float>& vertsXYZ,
                                    const std::vector<uint32_t>& indices) {
  if (!system_ || indices.size() < 3) return 0;

  JPH::VertexList verts;
  verts.reserve(vertsXYZ.size() / 3);
  for (size_t i = 0; i + 2 < vertsXYZ.size(); i += 3)
    verts.push_back(JPH::Float3(VoxToM(vertsXYZ[i]), VoxToM(vertsXYZ[i + 1]),
                                VoxToM(vertsXYZ[i + 2])));
  JPH::IndexedTriangleList tris;
  tris.reserve(indices.size() / 3);
  for (size_t i = 0; i + 2 < indices.size(); i += 3)
    tris.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2]));

  JPH::MeshShapeSettings mesh(verts, tris);
  // Default is cos(5°): nearly every seam between marching-cubes triangles
  // counts as an "active" edge whose normal can kick a rolling body. 25° keeps
  // real ridges active but lets the near-coplanar facets of smooth-looking
  // terrain read as one surface. Pairs with mEnhancedInternalEdgeRemoval on
  // the dynamic bodies.
  mesh.mActiveEdgeCosThresholdAngle = 0.9063f;  // cos(25 deg)
  auto shapeResult = mesh.Create();
  if (shapeResult.HasError()) return 0;

  JPH::BodyCreationSettings bcs(shapeResult.Get(), JPH::RVec3::sZero(),
                                JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                Layers::STATIC);
  bcs.mFriction = CurrentTuning().physics.terrainFriction;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
  return id.IsInvalid() ? 0 : FromBodyID(id);
}

uint64_t Physics::CreatePlayerBody(float halfXZVox, float halfYVox) {
  if (!system_) return 0;
  float radius = VoxToM(halfXZVox);
  float cylHalf = std::max(VoxToM(halfYVox) - radius, 0.01f);
  // Dynamic, not kinematic — see the header comment: finite mass is what
  // makes shoves scale with the shoved body's mass. Rotation is locked (a
  // capsule that tips over is not a player) and gravity is off (the AABB
  // controller owns vertical motion; the proxy just mirrors it).
  JPH::BodyCreationSettings bcs(new JPH::CapsuleShape(cylHalf, radius),
                                JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                JPH::EMotionType::Dynamic, Layers::PLAYER);
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
  JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::Activate);
  // NOT in dynamicBodies_: the proxy never despawns and must not receive
  // explosion impulses or WakeNear — the player controller owns its motion.
  return id.IsInvalid() ? 0 : FromBodyID(id);
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

Vec3 Physics::PlayerPushOut(uint64_t handle, Vec3 centerVoxel) const {
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
  JPH::SpecifiedObjectLayerFilter movingOnly(Layers::MOVING);
  JPH::IgnoreSingleBodyFilter ignoreSelf(id);
  system_->GetNarrowPhaseQuery().CollideShape(
      shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(center),
      settings, JPH::RVec3::sZero(), collector, {}, movingOnly, ignoreSelf);

  JPH::Vec3 push = JPH::Vec3::sZero();
  for (const JPH::CollideShapeResult& hit : collector.mHits) {
    float len = hit.mPenetrationAxis.Length();
    if (len < 1e-6f || hit.mPenetrationDepth <= 0) continue;
    // mPenetrationAxis points the way shape 2 (the body) moves to separate;
    // the player moves the opposite way
    push -= hit.mPenetrationAxis * (hit.mPenetrationDepth / len);
  }
  return Vec3{push.GetX(), push.GetY(), push.GetZ()} * (1.0f / kVoxelMeters);
}

uint64_t Physics::CreateJoint(uint64_t bodyA, uint64_t bodyB,
                              const JointDesc& d) {
  if (!system_ || bodyA == 0 || bodyB == 0) return 0;
  // TwoBodyConstraintSettings::Create wants Body&: lock both bodies
  const JPH::BodyLockInterface& bli = system_->GetBodyLockInterface();
  JPH::BodyID ids[2] = {ToBodyID(bodyA), ToBodyID(bodyB)};
  JPH::BodyLockMultiWrite lock(bli, ids, 2);
  JPH::Body* a = lock.GetBody(0);
  JPH::Body* b = lock.GetBody(1);
  if (!a || !b) return 0;

  JPH::RVec3 anchor(VoxToM(d.anchorVoxel.x), VoxToM(d.anchorVoxel.y),
                    VoxToM(d.anchorVoxel.z));

  // REST FRAME -> WORLD, per body. Jolt's settings are world-space and it
  // immediately converts them back through each body's rotation, so feeding it
  // `bodyRotation * restDirection` lands the constraint's local frame exactly
  // on the rest direction — whatever pose the bodies are in right now. See the
  // JointDesc comment in physics.h for why that matters.
  const JPH::Quat ra = a->GetRotation();
  const JPH::Quat rb = b->GetRotation();

  JPH::Ref<JPH::Constraint> constraint;
  JPH::Vec3 boneOut = JPH::Vec3::sZero();
  switch (d.type) {
    case JointType::Fixed: {
      JPH::FixedConstraintSettings s;
      s.mAutoDetectPoint = true;
      constraint = s.Create(*a, *b);
      break;
    }
    case JointType::Hinge: {
      JPH::HingeConstraintSettings s;
      s.mPoint1 = s.mPoint2 = anchor;
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
      s.mMaxFrictionTorque = FrictionTorque(*b, anchor, d.friction);
      constraint = s.Create(*a, *b);
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
      s.mPosition1 = s.mPosition2 = anchor;
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
      s.mMaxFrictionTorque = FrictionTorque(*b, anchor, d.friction);
      constraint = s.Create(*a, *b);
      boneOut = bone;
      break;
    }
  }
  if (!constraint) return 0;
  system_->AddConstraint(constraint);

  uint64_t h = nextJointId_++;
  joints_->joints[h] = {constraint, bodyA, bodyB, boneOut};
  joints_->byBody[bodyA].push_back(h);
  joints_->byBody[bodyB].push_back(h);
  return h;
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
  if (bi.IsAdded(ToBodyID(it->second.bodyA))) bi.ActivateBody(ToBodyID(it->second.bodyA));
  if (bi.IsAdded(ToBodyID(it->second.bodyB))) bi.ActivateBody(ToBodyID(it->second.bodyB));
  joints_->joints.erase(it);
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

void Physics::SetBodyVelocity(uint64_t handle, Vec3 velVoxelsPerSec) {
  if (!system_ || handle == 0) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return;
  bi.SetLinearVelocity(id, JPH::Vec3(VoxToM(velVoxelsPerSec.x),
                                     VoxToM(velVoxelsPerSec.y),
                                     VoxToM(velVoxelsPerSec.z)));
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
  bi.SetLinearVelocity(id, JPH::Vec3(VoxToM(lin.x), VoxToM(lin.y), VoxToM(lin.z)));
  bi.SetAngularVelocity(id, JPH::Vec3(angRadPerSec.x, angRadPerSec.y, angRadPerSec.z));
}

void Physics::SetBodyAvatarLayer(uint64_t handle, bool isAvatar) {
  if (!system_ || handle == 0) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = ToBodyID(handle);
  if (!bi.IsAdded(id)) return;
  // Both layers map to BP::MOVING, so this never needs a broadphase rebuild.
  bi.SetObjectLayer(id, isAvatar ? Layers::AVATAR : Layers::MOVING);
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
  bi.RemoveBody(id);
  bi.DestroyBody(id);
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

void Physics::ApplyRadialImpulse(Vec3 centerVoxel, float radiusVoxels,
                                 float impulse) {
  if (!system_) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::RVec3 c(VoxToM(centerVoxel.x), VoxToM(centerVoxel.y), VoxToM(centerVoxel.z));
  float rM = VoxToM(radiusVoxels);
  for (uint64_t h : dynamicBodies_) {
    JPH::BodyID id = ToBodyID(h);
    if (!bi.IsAdded(id)) continue;
    JPH::RVec3 p = bi.GetCenterOfMassPosition(id);
    JPH::Vec3 d = JPH::Vec3(p - c);
    float dist = d.Length();
    if (dist > rM) continue;
    JPH::Vec3 dir = dist > 1e-4f ? d / dist : JPH::Vec3(0, 1, 0);
    float falloff = 1.0f - dist / rM;
    bi.ActivateBody(id);
    bi.AddImpulse(id, dir * (impulse * falloff));
  }
}

void Physics::WakeNear(Vec3 centerVoxel, float radiusVoxels) {
  if (!system_) return;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::RVec3 c(VoxToM(centerVoxel.x), VoxToM(centerVoxel.y), VoxToM(centerVoxel.z));
  float rM = VoxToM(radiusVoxels);
  for (uint64_t h : dynamicBodies_) {
    JPH::BodyID id = ToBodyID(h);
    if (!bi.IsAdded(id)) continue;
    if (JPH::Vec3(bi.GetCenterOfMassPosition(id) - c).Length() <= rM)
      bi.ActivateBody(id);
  }
}

uint32_t Physics::NumActiveBodies() const {
  return system_ ? system_->GetNumActiveBodies(JPH::EBodyType::RigidBody) : 0;
}
