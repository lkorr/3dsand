#pragma once
#include <functional>

#include "math3d.h"
#include "sim/world.h"

// Per-frame movement intent, filled from GLFW polling in main.cpp.
struct PlayerInput {
  float forward = 0;  // -1..1
  float strafe = 0;   // -1..1
  bool up = false;    // space
  bool down = false;  // ctrl
  bool sprint = false;
  bool jumpPressed = false;
};

// AABB character vs the one-tick-latent voxel mirror (DESIGN.md v0 note).
// All units are voxels; sizes below are stated in meters and converted via
// kVoxelMeters so voxel size can be tuned in one place (world.h).
class Player {
 public:
  using KindFn = std::function<CellKind(IVec3)>;

  void Update(float dt, const PlayerInput& in, const Vec3& flatFwd,
              const Vec3& right, const Vec3& lookFwd, const KindFn& kindAt);

  Vec3 EyePos() const { return pos + Vec3{0, kEyeOffset, 0}; }

  Vec3 pos{128, 100, 140};  // AABB center
  Vec3 vel{0, 0, 0};
  bool fly = true;          // start in fly mode until the first mirror arrives
  bool grounded = false;
  bool inLiquid = false;

  static constexpr float kHalfXZ = 0.30f / kVoxelMeters;     // 0.6 m wide
  static constexpr float kHalfY = 0.85f / kVoxelMeters;      // 1.7 m tall
  static constexpr float kEyeOffset = 0.65f / kVoxelMeters;  // eyes near the top
};
