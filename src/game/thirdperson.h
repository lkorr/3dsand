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

// ---- avatar body facing policy ---------------------------------------------
// Where the BODY should face this tick, given where the camera looks and where
// the player is moving. Pulled out of main.cpp's frame loop so it can be
// tested: it is pure (no globals but CurrentTuning, no side effects), and the
// two bugs it has already had were both invisible to every existing gate
// because the policy only ran inside the render loop.
//
//   - Third person: face the TRAVEL DIRECTION. Holding W squares the character
//     to where they run, with no slack. Below `turnMinSpeed` the facing holds
//     rather than chasing a near-zero velocity vector.
//   - First person: face the CAMERA, always — standing or moving, no neck
//     dead zone — eased only by `firstPersonTurnHalflife`. The walk basis is
//     the camera, so any body/view slack read as walking crabwise; A/D is a
//     true strafe with the body square to the view.
//
// `camHeading` and `heading` are in the rig's heading convention (forward is
// (sin h, ., cos h)); `planarVel` is the player's world-voxel velocity with y
// dropped. Returns the NEW heading, already wrapped.
// `prone` (0..1, Mob::LocoGroundAlign) blends in avatar.crawlTurnRate as a
// rate limit on top of either mode: a crawling body turns slowly.
float ResolveAvatarHeading(CameraMode mode, float camHeading, float heading,
                           Vec3 planarVel, float dt, float prone = 0.0f);

// ---- swing aim policy ------------------------------------------------------
// The sword's version of the neck rule above. The melee driver (game/melee.h)
// is fed a BASIS to express the stroke in, and for the player that was the raw
// camera — so with the body facing its travel direction in third person, a
// camera swung round behind the character made every strike cut backwards at
// the lens, or sideways at nothing. The body cannot turn to follow (that would
// make every glance a turn again), so the SWING is bound to the body instead:
//
//   - a look BEHIND the body (|yawRel| > 90) is first reflected to the front
//     across the body's left-right axis: 150 deg right swings at 30 deg
//     right, looking the character in the face swings straight ahead. Both
//     signs reach 0 at the +180/-180 wrap, so the wrap is a non-event;
//   - inside `melee.aimYaw` degrees of the body's facing the camera IS the
//     basis, unchanged — a strike goes where you look;
//   - past the cone the basis is pinned at the cone's edge (the yaw excess is
//     removed, pitch kept), so aiming further round your own shoulder does not
//     put the blade behind you.
//
// `yawRel` is camera heading minus body heading, radians, any range (wrapped
// here). Returns the yaw offset the swing basis should actually have, radians
// in [-cone, cone]. Pure; a few hundred calls cost nothing, so it is gated.
float ResolveSwingYaw(float yawRel);

// Rotates the camera basis about world +Y so its heading becomes
// `bodyHeading + ResolveSwingYaw(camHeading - bodyHeading)`. Right/up/forward
// are carried together as one rigid frame, so the stroke's own geometry (a
// diagonal cut, an overhead) is untouched — only the direction the whole thing
// faces moves, and only in yaw. With the offset inside the cone this returns
// the inputs bit-for-bit.
void ResolveSwingBasis(float camHeading, float bodyHeading, Vec3 camRight,
                       Vec3 camUp, Vec3 camFwd, Vec3& right, Vec3& up,
                       Vec3& fwd);

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
  //
  // The ZOOM deliberately survives it: it is a player preference, not smoothing
  // state, and having a respawn or a fly-mode toggle silently put the camera
  // back where the tuning file wanted it is the sort of thing that reads as the
  // setting not sticking.
  void Snap() { initialized_ = false; }

  // The wheel. `notches` is signed, positive = scrolled up = zoom IN, which is
  // the direction every game in the genre uses. Held as a MULTIPLIER on the
  // tuned boom (thirdPerson.zoomMin/zoomMax) rather than as a distance, so the
  // two boom lengths keep their authored relationship and re-tuning either one
  // does not move where the player left their zoom.
  void Zoom(float notches);
  float ZoomFactor() const { return zoom_; }

 private:
  Vec3 eye_{};
  Vec3 focusSmooth_{};
  float distNow_ = 0;
  float distGoal_ = 0;
  float zoom_ = 1.0f;
  bool occluded_ = false;
  bool initialized_ = false;
};
