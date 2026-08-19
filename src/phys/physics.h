#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "math3d.h"

// Thin ownership wrapper around Jolt (DESIGN.md §7): debris rigidbodies +
// static marching-cubes terrain patches. All public APIs use VOXEL units for
// positions/sizes; conversion to meters (kVoxelMeters) happens inside so the
// rest of the engine stays in one coordinate system. Fixed 30 Hz stepping,
// same tick as the CA.
//
// Jolt is CPU float physics: its results never feed the deterministic grid
// directly — bodies only re-enter the grid through MutationQueue ops.

namespace JPH {
class PhysicsSystem;
class TempAllocatorImpl;
class JobSystemThreadPool;
class BodyInterface;
}  // namespace JPH

struct BodyTransform {
  Vec3 pos;        // voxel units (body center of mass)
  float quat[4];   // x, y, z, w
};

// One voxel of a debris body, in body-local voxel coordinates.
struct DebrisVoxel {
  int8_t x, y, z;
  uint8_t pad = 0;
  uint16_t payload;  // material | state<<12
};

class Physics {
 public:
  Physics();
  ~Physics();  // out-of-line: members are incomplete types here
  bool Init();
  void Shutdown();
  void Step(float dt);

  // Debris body from island voxels (local coords relative to `originVoxel`).
  // Boxes are greedy-merged; mass comes from per-voxel material density
  // (kg/m^3). Returns an opaque handle (0 = failure).
  uint64_t CreateDebrisBody(const std::vector<DebrisVoxel>& voxels,
                            IVec3 originVoxel,
                            const std::vector<float>& densityOfMat);

  // Static terrain collision patch (triangles in voxel units, world space).
  uint64_t CreateTerrainMesh(const std::vector<float>& vertsXYZ,
                             const std::vector<uint32_t>& indices);

  void RemoveBody(uint64_t handle);
  bool GetTransform(uint64_t handle, BodyTransform& out) const;
  bool IsActive(uint64_t handle) const;
  // Radial impulse (explosions). center/radius in voxels, impulse in kg*m/s
  // at the center, falling off linearly to zero at radius.
  void ApplyRadialImpulse(Vec3 centerVoxel, float radiusVoxels, float impulse);
  // Wake dynamic bodies whose AABB intersects the given voxel-space sphere
  // (terrain changed under them).
  void WakeNear(Vec3 centerVoxel, float radiusVoxels);

  uint32_t NumActiveBodies() const;

 private:
  std::unique_ptr<JPH::TempAllocatorImpl> tempAlloc_;
  std::unique_ptr<JPH::JobSystemThreadPool> jobs_;
  std::unique_ptr<JPH::PhysicsSystem> system_;
  struct LayerImpls;
  std::unique_ptr<LayerImpls> layers_;
  std::vector<uint64_t> dynamicBodies_;  // handles of live debris bodies
};
