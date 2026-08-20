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
  // allowKinematic: body may later switch motion type (mob limbs animate
  // kinematically while alive, go dynamic as ragdoll — PLAN §B4).
  uint64_t CreateDebrisBody(const std::vector<DebrisVoxel>& voxels,
                            IVec3 originVoxel,
                            const std::vector<float>& densityOfMat,
                            bool allowKinematic = false);
  // Same, but at an arbitrary transform (laser splits inherit the parent
  // body's pose mid-tumble — PLAN §C2).
  uint64_t CreateDebrisBodyXf(const std::vector<DebrisVoxel>& voxels,
                              const BodyTransform& xf,
                              const std::vector<float>& densityOfMat,
                              bool allowKinematic = false);
  // Linear/angular velocity in voxel units (split halves keep momentum).
  bool GetBodyVelocities(uint64_t handle, Vec3& lin, Vec3& angRadPerSec) const;
  void SetBodyVelocities(uint64_t handle, Vec3 lin, Vec3 angRadPerSec);

  // ---- joints (PLAN §B1) ----
  // Anchors/axes in world-space voxel units at creation time. Ball is a free
  // point constraint (no limits); Hinge takes min/max angle in radians about
  // `axis`. Returns an opaque handle (0 = failure). Joints attached to a body
  // are destroyed automatically when that body is removed.
  enum class JointType { Fixed, Hinge, Ball };
  uint64_t CreateJoint(uint64_t bodyA, uint64_t bodyB, JointType type,
                       Vec3 anchorVoxel, Vec3 axis, float minAngle,
                       float maxAngle);
  void DestroyJoint(uint64_t joint);  // <- this is dismemberment

  // ---- mob locomotion plumbing (PLAN §B4) ----
  // Requires the body to have been created with allowKinematic.
  void SetBodyKinematic(uint64_t handle, bool kinematic);
  // Drive a kinematic body toward a pose over dt (gives it real velocity).
  void MoveKinematicBody(uint64_t handle, Vec3 posVoxel, const float quat[4],
                         float dt);
  void SetBodyVelocity(uint64_t handle, Vec3 velVoxelsPerSec);

  // Disable collisions among a set of bodies (one mob's limbs): adjacent limb
  // boxes otherwise fight their own joints — the push/pull jitter keeps the
  // ragdoll awake forever. Jolt's own Ragdoll class does exactly this.
  void DisableCollisionsAmong(const std::vector<uint64_t>& handles);

  // First dynamic (MOVING-layer) body hit by the ray, or 0. `fraction` is the
  // hit position along the ray (0..1 of maxDistVoxels). Laser body cuts.
  uint64_t CastRayBody(Vec3 fromVoxel, Vec3 dirNormalized, float maxDistVoxels,
                       float& fraction) const;

  // Static terrain collision patch (triangles in voxel units, world space).
  uint64_t CreateTerrainMesh(const std::vector<float>& vertsXYZ,
                             const std::vector<uint32_t>& indices);

  // ---- player proxy (deferred from M6; DESIGN.md §8) ----
  // Kinematic capsule the debris collides against. Voxel terrain collision
  // stays in the AABB controller (player.cpp); this body only exists so
  // rigidbodies can't pass through the player and so a moving player shoves
  // debris (MoveKinematic gives it real velocity). It ignores STATIC terrain
  // meshes — colliding with both grids would double-resolve.
  uint64_t CreatePlayerBody(float halfXZVox, float halfYVox);
  // Drive the proxy toward the player's AABB center over dt seconds.
  void MovePlayerBody(uint64_t handle, Vec3 centerVoxel, float dt);
  // Depenetration vector (voxel units) to move the player out of any debris
  // bodies overlapping the proxy shape at centerVoxel. Zero when clear.
  // The caller applies it through its own terrain sweeps.
  Vec3 PlayerPushOut(uint64_t handle, Vec3 centerVoxel) const;

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
  struct JointImpls;                     // Jolt constraint refs (impl detail)
  std::unique_ptr<JointImpls> joints_;
  uint64_t nextJointId_ = 1;
  uint32_t nextCollisionGroup_ = 1;
};
