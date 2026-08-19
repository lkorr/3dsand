#pragma once
#include "math3d.h"

// FPS look camera: orientation only, position comes from the player.
class Camera {
 public:
  void ApplyMouse(float dx, float dy);
  Vec3 Forward() const;
  Vec3 Right() const;
  Vec3 Up() const;
  // Horizontal-plane basis for walking.
  Vec3 FlatForward() const;

  float yaw = 2.35f;    // radians; initial look toward world center
  float pitch = -0.2f;
  float fovY = 1.2f;    // ~69 degrees
};
