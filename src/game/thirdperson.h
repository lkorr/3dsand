#pragma once
#include <cstdint>

#include "game/avatar.h"
#include "game/camera.h"
#include "math3d.h"
#include "sim/world.h"

// Third-person camera rig: an orbit boom around the avatar, with voxel
// collision pull-in and per-dismemberment-state framing.
//
// WHY THIS IS SEPARATE FROM Camera. `Camera` is orientation only — yaw, pitch
// and the basis vectors — and both modes share it, so mouse look, the picking
// ray and the walk basis are identical in first and third person. This class
// adds only the POSITION policy: where the eye sits relative to the body, and
// what to do when that point is inside a wall. Keeping them apart is what lets
// the mode toggle be a one-line change of eye position with nothing else in
// the frame path caring which mode is active.
//
// DETERMINISM (rule 1): pure render-side float state. The camera never feeds
// the sim — brush/laser/grenade rays keep using the player's own eye position,
// exactly as the existing view-smoothing note in player.h requires.

enum class CameraMode : uint8_t {
  First = 0,      // eye at the avatar's head, body hidden except arms
  Third = 1,      // orbit boom behind the avatar
  OverShoulder = 2,  // tighter boom, offset to one side, for aiming
  Count = 3
};

const char* CameraModeName(CameraMode m);

class ThirdPersonRig {
 public:
  // Advances the rig one frame. `focusWorld` is the point the boom orbits —
  // normally the avatar's head/upper chest, so the camera pivots about the
  // character rather than about its feet.
  //
  // `kindAt` is the same voxel probe the player controller uses; the boom is
  // swept against it so the camera never ends up inside terrain.
  void Update(float dt, CameraMode mode, const Camera& cam, Vec3 focusWorld,
              const AvatarLocomotion& loco, const World& world,
              const Player::KindFn& kindAt);

  // Where to put the render camera this frame. In first person this is the
  // focus point itself; in third it is the collision-resolved boom end.
  Vec3 EyePos() const { return eye_; }
  // Current boom length after collision pull-in, in voxels. 0 in first person.
  float Distance() const { return distNow_; }
  // True while the camera is pulled closer than the requested distance, i.e.
  // it is hugging geometry. The UI uses this; nothing gameplay-facing does.
  bool Occluded() const { return occluded_; }

  // Reset all smoothing (teleport, respawn, mode change on the same frame).
  // Without this a teleport smears the camera across the whole world.
  void Snap() { initialized_ = false; }

 private:
  Vec3 eye_{};
  Vec3 focusSmooth_{};
  float distNow_ = 0;
  float distGoal_ = 0;
  bool occluded_ = false;
  bool initialized_ = false;
};
