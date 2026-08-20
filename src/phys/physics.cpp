#include "phys/physics.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
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

}  // namespace

struct Physics::LayerImpls {
  BPLayerInterface bpInterface;
  ObjVsBPFilter objVsBp;
  ObjPairFilter objPair;
};

// Constraint bookkeeping: Jolt asserts if a constraint outlives either body,
// so RemoveBody tears down attached joints first (byBody index).
struct Physics::JointImpls {
  struct Entry {
    JPH::Ref<JPH::Constraint> constraint;
    uint64_t bodyA = 0, bodyB = 0;
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

  system_ = std::make_unique<JPH::PhysicsSystem>();
  system_->Init(4096 /*max bodies*/, 0, 4096, 2048, layers_->bpInterface,
                layers_->objVsBp, layers_->objPair);
  system_->SetGravity(JPH::Vec3(0, -CurrentTuning().physics.gravity, 0));
  return true;
}

void Physics::Shutdown() {
  joints_.reset();  // constraint refs drop before the system that owns bodies
  system_.reset();
  layers_.reset();
  jobs_.reset();
  tempAlloc_.reset();
}

void Physics::Step(float dt) {
  if (!system_) return;
  // Gravity is re-applied here rather than only at Init so a tuning reload
  // takes effect without restarting the world.
  system_->SetGravity(JPH::Vec3(0, -CurrentTuning().physics.gravity, 0));
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

uint64_t Physics::CreateJoint(uint64_t bodyA, uint64_t bodyB, JointType type,
                              Vec3 anchorVoxel, Vec3 axis, float minAngle,
                              float maxAngle) {
  if (!system_ || bodyA == 0 || bodyB == 0) return 0;
  // TwoBodyConstraintSettings::Create wants Body&: lock both bodies
  const JPH::BodyLockInterface& bli = system_->GetBodyLockInterface();
  JPH::BodyID ids[2] = {ToBodyID(bodyA), ToBodyID(bodyB)};
  JPH::BodyLockMultiWrite lock(bli, ids, 2);
  JPH::Body* a = lock.GetBody(0);
  JPH::Body* b = lock.GetBody(1);
  if (!a || !b) return 0;

  JPH::RVec3 anchor(VoxToM(anchorVoxel.x), VoxToM(anchorVoxel.y),
                    VoxToM(anchorVoxel.z));
  JPH::Ref<JPH::Constraint> constraint;
  switch (type) {
    case JointType::Fixed: {
      JPH::FixedConstraintSettings s;
      s.mAutoDetectPoint = true;
      constraint = s.Create(*a, *b);
      break;
    }
    case JointType::Hinge: {
      JPH::HingeConstraintSettings s;
      s.mPoint1 = s.mPoint2 = anchor;
      JPH::Vec3 ax(axis.x, axis.y, axis.z);
      if (ax.LengthSq() < 1e-6f) ax = JPH::Vec3::sAxisX();
      ax = ax.Normalized();
      s.mHingeAxis1 = s.mHingeAxis2 = ax;
      s.mNormalAxis1 = s.mNormalAxis2 = ax.GetNormalizedPerpendicular();
      s.mLimitsMin = std::max(minAngle, -JPH::JPH_PI);
      s.mLimitsMax = std::min(maxAngle, JPH::JPH_PI);
      constraint = s.Create(*a, *b);
      break;
    }
    case JointType::Ball: {
      JPH::PointConstraintSettings s;
      s.mPoint1 = s.mPoint2 = anchor;
      constraint = s.Create(*a, *b);
      break;
    }
  }
  if (!constraint) return 0;
  system_->AddConstraint(constraint);

  uint64_t h = nextJointId_++;
  joints_->joints[h] = {constraint, bodyA, bodyB};
  joints_->byBody[bodyA].push_back(h);
  joints_->byBody[bodyB].push_back(h);
  return h;
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

bool Physics::IsActive(uint64_t handle) const {
  if (!system_ || handle == 0) return false;
  return system_->GetBodyInterface().IsActive(ToBodyID(handle));
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
