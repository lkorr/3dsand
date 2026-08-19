#include "phys/physics.h"

#include <cstdarg>
#include <cstdio>
#include <unordered_map>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include "sim/world.h"  // kVoxelMeters

namespace {

namespace Layers {
constexpr JPH::ObjectLayer STATIC = 0;
constexpr JPH::ObjectLayer MOVING = 1;
constexpr JPH::ObjectLayer NUM = 2;
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
    return true;
  }
};

class ObjPairFilter final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    return !(a == Layers::STATIC && b == Layers::STATIC);
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

  system_ = std::make_unique<JPH::PhysicsSystem>();
  system_->Init(4096 /*max bodies*/, 0, 4096, 2048, layers_->bpInterface,
                layers_->objVsBp, layers_->objPair);
  system_->SetGravity(JPH::Vec3(0, -9.81f, 0));
  return true;
}

void Physics::Shutdown() {
  system_.reset();
  layers_.reset();
  jobs_.reset();
  tempAlloc_.reset();
}

void Physics::Step(float dt) {
  if (!system_) return;
  system_->Update(dt, 1, tempAlloc_.get(), jobs_.get());
}

uint64_t Physics::CreateDebrisBody(const std::vector<DebrisVoxel>& voxels,
                                   IVec3 originVoxel,
                                   const std::vector<float>& densityOfMat) {
  if (!system_ || voxels.empty()) return 0;

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
  const float voxVol = kVoxelMeters * kVoxelMeters * kVoxelMeters;
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

    JPH::Vec3 half(VoxToM(sx * 0.5f), VoxToM(sy * 0.5f), VoxToM(sz * 0.5f));
    JPH::Vec3 center(VoxToM(v.x + sx * 0.5f), VoxToM(v.y + sy * 0.5f),
                     VoxToM(v.z + sz * 0.5f));
    // tiny convex radius: debris voxels are 12.5 cm
    compound.AddShape(center, JPH::Quat::sIdentity(),
                      new JPH::BoxShape(half, 0.01f));
    boxes++;
    if (boxes >= 1024) break;  // pathological shapes get a truncated collider
  }

  auto shapeResult = compound.Create();
  if (shapeResult.HasError()) {
    std::fprintf(stderr, "debris shape error: %s\n",
                 shapeResult.GetError().c_str());
    return 0;
  }

  JPH::BodyCreationSettings bcs(
      shapeResult.Get(),
      JPH::RVec3(VoxToM((float)originVoxel.x), VoxToM((float)originVoxel.y),
                 VoxToM((float)originVoxel.z)),
      JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, Layers::MOVING);
  bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
  bcs.mMassPropertiesOverride.mMass = std::max(totalMass, 0.05f);
  bcs.mFriction = 0.75f;
  bcs.mRestitution = 0.05f;
  bcs.mLinearDamping = 0.05f;
  bcs.mAngularDamping = 0.15f;

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
  auto shapeResult = mesh.Create();
  if (shapeResult.HasError()) return 0;

  JPH::BodyCreationSettings bcs(shapeResult.Get(), JPH::RVec3::sZero(),
                                JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                Layers::STATIC);
  bcs.mFriction = 0.85f;
  JPH::BodyInterface& bi = system_->GetBodyInterface();
  JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
  return id.IsInvalid() ? 0 : FromBodyID(id);
}

void Physics::RemoveBody(uint64_t handle) {
  if (!system_ || handle == 0) return;
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
