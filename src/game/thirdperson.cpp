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

float ResolveAvatarHeading(CameraMode mode, float camHeading, float heading,
                           Vec3 planarVel, float dt) {
  const auto& av = CurrentTuning().avatar;
  const float sp = planarVel.len() * kVoxelMeters;   // voxels/s -> m/s
  const bool moving = sp > av.turnMinSpeed;

  float wantHeading = camHeading;
  if (mode != CameraMode::First) {
    // Face where you RUN. Below the threshold hold the current facing rather
    // than chasing a near-zero velocity vector, which would spin on the spot.
    wantHeading = moving ? std::atan2(planarVel.x, planarVel.z) : heading;
  }

  float d = wantHeading - heading;
  while (d > 3.14159265f) d -= 6.2831853f;
  while (d < -3.14159265f) d += 6.2831853f;

  // THE NECK ABSORBS THE FIRST headLookYaw DEGREES (first person only).
  //
  // The cone kills the SNAP, not the convergence. Zeroing `d` inside it
  // outright makes the facing a value nothing ever drives back, so it freezes
  // wherever the last big turn left it — and the arms, welded to the torso,
  // freeze with it. Hence the recentre term while walking.
  const float headCone = av.headLookYaw * (3.14159265f / 180.0f);
  if (mode == CameraMode::First && headCone > 1e-3f) {
    const float excess = std::max(0.0f, std::fabs(d) - headCone);
    const float sign = d < 0 ? -1.0f : 1.0f;
    float want = sign * excess;      // past the cone: dragged by the excess
    if (moving) {                    // inside it: square up while walking
      const float hl = av.headLookRecenterHalflife;
      const float rk = hl > 1e-4f ? 1.0f - std::pow(0.5f, dt / hl) : 1.0f;
      want += (d - sign * excess) * rk;
    }
    d = want;
  }

  // First person eases on a short half-life; third person is rate-limited so
  // the body visibly pivots on its feet instead of snapping.
  float out = heading;
  if (mode == CameraMode::First) {
    const float hl = av.firstPersonTurnHalflife;
    out += hl > 1e-4f ? d * (1.0f - std::pow(0.5f, dt / hl)) : d;
  } else {
    const float maxStep = av.turnRate * dt;
    out += std::clamp(d, -maxStep, maxStep);
  }
  while (out > 3.14159265f) out -= 6.2831853f;
  while (out < -3.14159265f) out += 6.2831853f;
  return out;
}

float ResolveSwingYaw(float yawRel) {
  const auto& m = CurrentTuning().melee;
  const float kDeg = 3.14159265f / 180.0f;
  while (yawRel > 3.14159265f) yawRel -= 6.2831853f;
  while (yawRel < -3.14159265f) yawRel += 6.2831853f;
  const float cone = m.aimYaw * kDeg;
  // The rear release, verbatim from PlayerAvatar::SetLook: scale the offset to
  // nothing across the last `aimReleaseYaw` degrees before straight-behind.
  // Smoothstep so it is flat at both ends — no crease where the band begins,
  // and a genuine zone (not a single angle) where it is fully released.
  float release = 1.0f;
  const float band = m.aimReleaseYaw * kDeg;
  if (band > 1e-4f) {
    const float t =
        std::clamp((3.14159265f - std::fabs(yawRel)) / band, 0.0f, 1.0f);
    release = t * t * (3.0f - 2.0f * t);
  }
  return std::clamp(yawRel, -cone, cone) * release;
}

void ResolveSwingBasis(float camHeading, float bodyHeading, Vec3 camRight,
                       Vec3 camUp, Vec3 camFwd, Vec3& right, Vec3& up,
                       Vec3& fwd) {
  float rel = camHeading - bodyHeading;
  while (rel > 3.14159265f) rel -= 6.2831853f;
  while (rel < -3.14159265f) rel += 6.2831853f;
  const float delta = ResolveSwingYaw(rel) - rel;
  if (std::fabs(delta) < 1e-6f) {
    right = camRight;
    up = camUp;
    fwd = camFwd;
    return;
  }
  // Rotate about +Y by `delta` in the HEADING sense (forward is
  // (sin h, ., cos h), so a positive delta turns (sin h, cos h) into
  // (sin(h+d), cos(h+d))): x' = x cos d + z sin d, z' = -x sin d + z cos d.
  const float c = std::cos(delta), s = std::sin(delta);
  auto rot = [c, s](Vec3 v) {
    return Vec3{v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
  };
  right = rot(camRight);
  up = rot(camUp);
  fwd = rot(camFwd);
}

void ThirdPersonRig::Zoom(float notches) {
  const auto& t = CurrentTuning().thirdPerson;
  if (t.zoomStep <= 0.0f) return;
  // MULTIPLICATIVE, not additive. A fixed step in metres is coarse when you are
  // close and imperceptible when you are far; a fixed step in FRACTION gives
  // the same felt increment at every distance, which is what makes a zoom feel
  // like one control rather than two.
  zoom_ *= std::pow(1.0f + t.zoomStep, -notches);
  zoom_ = std::clamp(zoom_, t.zoomMin, t.zoomMax);
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
  // The wheel's factor rides on the AUTHORED boom, and only on the boom: the
  // side offset and the pitch lift are framing decisions about where the
  // character sits in shot, and scaling them with zoom would swing the subject
  // across the screen every time the player rolled the wheel.
  distGoal_ = (shoulder ? t.shoulderDist : t.distance) * m2v * zoom_;
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
