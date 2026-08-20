#include "game/thirdperson.h"

#include <algorithm>
#include <cmath>

#include "sim/tuning.h"

namespace {

// Frame-rate independent exponential smoothing. `halflife` is the time for the
// remaining error to halve; 0 means "snap". Using a half-life rather than a
// raw per-frame lerp constant is what keeps the camera feeling identical at 30
// and 144 fps — the same reasoning as the accel rates in the player block.
float SmoothT(float halflife, float dt) {
  if (halflife <= 1e-4f) return 1.0f;
  return 1.0f - std::exp2(-dt / halflife);
}

}  // namespace

const char* CameraModeName(CameraMode m) {
  switch (m) {
    case CameraMode::First: return "first";
    case CameraMode::Third: return "third";
    case CameraMode::OverShoulder: return "shoulder";
    default: return "?";
  }
}

void ThirdPersonRig::Update(float dt, CameraMode mode, const Camera& cam,
                            Vec3 focusWorld, const AvatarLocomotion& loco,
                            const World& world,
                            const Player::KindFn& kindAt) {
  const auto& t = CurrentTuning().thirdPerson;
  const float m2v = 1.0f / kVoxelMeters;   // meters -> voxels

  // ---- focus point ----
  // The boom orbits a point slightly above the head anchor, lowered by the
  // dismemberment state's body drop. `stateFollow` below 1 keeps the camera a
  // little higher than a crawling body, which frames it better than lying on
  // the floor with it.
  float stateDrop = (1.0f - loco.eyeHeightScale) * t.stateFollow;
  Vec3 focus = focusWorld;
  focus.y += t.heightOffset * m2v;
  focus.y -= stateDrop * t.heightOffset * m2v;

  if (!initialized_) {
    focusSmooth_ = focus;
    distNow_ = 0;
    initialized_ = true;
  } else {
    // Smoothed so the camera does not jitter with every step bob. Only the
    // FOCUS is smoothed, never the final eye — smoothing the eye after
    // collision would let it drift back into the wall it was just pulled out
    // of, one frame at a time.
    focusSmooth_ = focusSmooth_ + (focus - focusSmooth_) *
                                      SmoothT(t.focusHalflife, dt);
  }

  if (mode == CameraMode::First) {
    eye_ = focusSmooth_;
    distNow_ = 0;
    distGoal_ = 0;
    occluded_ = false;
    return;
  }

  // ---- boom direction and requested length ----
  const bool shoulder = mode == CameraMode::OverShoulder;
  distGoal_ = (shoulder ? t.shoulderDist : t.distance) * m2v;
  float side = (shoulder ? t.shoulderOffset : t.sideOffset) * m2v;

  Vec3 fwd = cam.Forward();
  Vec3 right = cam.Right();
  Vec3 up{0, 1, 0};

  // Pitch lift: looking down steeply, raise the boom so the character stays
  // framed instead of being hidden under its own hat brim.
  Vec3 pivot = focusSmooth_ + right * side;
  float lift = std::max(0.0f, -std::sin(cam.pitch)) * t.pitchLift * m2v;
  pivot.y += lift;

  Vec3 dir = (fwd * -1.0f).normalized();   // boom points BEHIND the camera

  // ---- collision: sweep the boom and pull in to the first hit ----
  float allowed = distGoal_;
  occluded_ = false;
  if (t.collide) {
    const float radius = std::max(0.0f, t.collideRadius) * m2v;
    const float margin = std::max(0.0f, t.collideMargin) * m2v;
    // March in voxel-sized steps and probe a small cross of offsets around the
    // ray, which fattens the sweep cheaply. A true swept sphere is not worth
    // it here: the failure this prevents is the camera slipping through a
    // one-voxel gap and popping to the far side of a wall, and sampling the
    // ray's neighbourhood catches exactly that.
    const int steps = std::max(1, (int)std::ceil(distGoal_ * 2.0f));
    const float dStep = distGoal_ / (float)steps;
    for (int i = 1; i <= steps; i++) {
      float d = dStep * (float)i;
      Vec3 p = pivot + dir * d;
      bool hit = false;
      const Vec3 probes[5] = {
          p,
          p + right * radius, p - right * radius,
          p + up * radius,    p - up * radius,
      };
      for (const Vec3& q : probes) {
        IVec3 c{ifloor(q.x), ifloor(q.y), ifloor(q.z)};
        // Unloaded space is SOLID and inert (the residency-window rule), so an
        // out-of-window probe must stop the boom rather than be treated as
        // empty — otherwise the camera happily backs out of the loaded world.
        if (!world.CellInWindow(c) || kindAt(c) == CellKind::Solid) {
          hit = true;
          break;
        }
      }
      if (hit) {
        allowed = std::max(0.0f, d - margin);
        occluded_ = true;
        break;
      }
    }
  }

  // ---- asymmetric distance smoothing ----
  // Pulling IN is (by default) instant: easing inward would leave the camera
  // inside the wall for the duration of the ease, which is the one artifact
  // players actually notice. Pushing back OUT is eased, which is what stops
  // the camera snapping outward the instant it clears a corner.
  float hl = allowed < distNow_ ? t.distInHalflife : t.distOutHalflife;
  distNow_ = distNow_ + (allowed - distNow_) * SmoothT(hl, dt);
  distNow_ = std::clamp(distNow_, 0.0f, distGoal_);

  eye_ = pivot + dir * distNow_;
}
