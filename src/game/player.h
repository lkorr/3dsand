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

  // Positional shove from debris rigidbodies (Physics::PlayerPushOut), applied
  // through the same voxel sweeps as movement so a body can never push the
  // player inside terrain. An upward push while falling counts as support, so
  // standing on debris works.
  void ApplyPush(Vec3 push, const KindFn& kindAt);

  Vec3 EyePos() const { return pos + Vec3{0, kEyeOffset, 0}; }

  Vec3 pos{128, 100, 140};  // AABB center
  Vec3 vel{0, 0, 0};
  bool fly = true;          // start in fly mode until the first mirror arrives
  bool grounded = false;
  bool inLiquid = false;

  // Jump grace windows, seconds remaining. coyoteTimer keeps a jump legal
  // briefly after leaving the ground; jumpBuffer remembers a press made just
  // before landing. On noisy terrain the grounded/airborne boundary is
  // genuinely ragged frame to frame, so without these a jump pressed while
  // running over rough ground is silently swallowed on the frames the body
  // happens to be cresting a bump.
  float coyoteTimer = 0.0f;
  float jumpBuffer = 0.0f;

  static constexpr float kHalfXZ = 0.30f / kVoxelMeters;     // 0.6 m wide
  static constexpr float kHalfY = 0.85f / kVoxelMeters;      // 1.7 m tall
  static constexpr float kEyeOffset = 0.65f / kVoxelMeters;  // eyes near the top

  // Tallest ledge walked over without jumping, in meters. Because it is
  // physical, shrinking kVoxelMeters turns the same real-world ledge into more
  // (smaller) voxels rather than into an impassable wall. This is what makes
  // finely-diced noisy ground read as a smooth floor: at 0.05 m voxels a
  // 1-voxel bump is 5 cm of a 45 cm budget, so it is absorbed silently.
  // ~1/3 of body height, which is where every surveyed engine lands: Quake
  // STEPSIZE 18 against a 56-unit body, Minecraft maxUpStep 0.6 against 1.8,
  // Vintage Story 0.6. The previous 0.45 m was only 26% of this 1.7 m body,
  // and a step budget that tight is what let noisy ground stop the player.
  static constexpr float kStepUpM = 0.58f;
  static constexpr int kMaxStepUpVoxels =
      (int)(kStepUpM / kVoxelMeters + 0.5f) < 1 ? 1
                                               : (int)(kStepUpM / kVoxelMeters + 0.5f);
};
