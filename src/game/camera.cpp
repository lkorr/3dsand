#include "game/camera.h"

#include <algorithm>

#include "sim/tuning.h"

void Camera::ApplyMouse(float dx, float dy) {
  const auto& t = CurrentTuning().camera;
  yaw += dx * t.mouseSensitivity;
  pitch -= dy * t.mouseSensitivity;
  pitch = std::clamp(pitch, -t.pitchClamp, t.pitchClamp);
}

Vec3 Camera::Forward() const {
  float cp = std::cos(pitch);
  return Vec3{std::cos(yaw) * cp, std::sin(pitch), std::sin(yaw) * cp};
}

Vec3 Camera::FlatForward() const {
  return Vec3{std::cos(yaw), 0.0f, std::sin(yaw)};
}

Vec3 Camera::Right() const {
  return Forward().cross(Vec3{0, 1, 0}).normalized();
}

Vec3 Camera::Up() const { return Right().cross(Forward()).normalized(); }
