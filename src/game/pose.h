#pragma once
// ONE POSE PIPELINE FOR EVERY CREATURE (rule-unification W2-L, 2026-09-24).
//
// `Mob::PosePipeline` (game/pose.cpp) is the whole of "turn this body's state
// into a pose" — loco state, clip sample and blend, the procedural layers, the
// springs, the head look, the spine twist, the strike aim, the airborne pose,
// the hit reaction, the stump drag, the flatten, the gait, the leg IK, the
// weapon arm, the ledge-hang palms and the anatomical clamp — and BOTH drivers
// call it. What a driver does is fill in a `PoseInputs` and add its own genuine
// extras; it no longer owns a copy of the pipeline.
//
// Until W2-L there were two: `MobSystem::UpdateAnimation` + `MobSystem::
// UpdateGait` for NPCs and `PlayerAvatar::UpdateAnimation` + `UpdateGait` for
// the player, calling the same stages in the same order with the loco-state
// block copied word for word, and a ~480-line and a ~390-line gait that had
// drifted apart (dc7f896 and 214770a are two of the bugs that came out of the
// split). DESIGN.md "One pose pipeline" records which drift was resolved which
// way and why.
//
// Everything here is CPU-float PRESENTATION state (CLAUDE.md rule 1): never
// hashed, never saved, never on the grid.

#include <functional>

#include "game/anim.h"
#include "math3d.h"

// ONE SHAPE OF THE AIRBORNE POSE (pose.cpp's kAirTuck / kAirFloat / kAirReach /
// kAirPrepare, and the blend of them the current `vel.y` asks for).
//
// Authored in LEG LENGTHS and ARM REACHES, never in voxels or metres, so one
// table poses any rig — the same argument the gait's leg-length thresholds are
// written down under in sim/scale.h. Foot offsets are relative to that leg's own
// hip, hand offsets to that arm's own shoulder, in prefab space tilted by the
// body's lean: +Y up, +Z forward, `Out` away from the midline on that limb's
// side. See the table in pose.cpp for what each shape is and why.
struct AirKeyPose {
  float footDown = 0.95f;  // below the hip
  float footFwd = 0;       // fore/aft; SIGNED per leg by the scissor split
  float footLead = 0;      // ...and the part BOTH feet share (a landing)
  float footOut = 0;       // lateral splay, outward
  // ---- THE ARMS ARE JOINT ANGLES, NOT A HAND TARGET ----------------------
  // Radians, applied at the joints the rig already authors limits for:
  // `armPitch` +/- `armSwing` swings the whole arm fore and aft at the
  // shoulder (positive = forward), `armOut` abducts it away from the midline,
  // `armFlex` bends the elbow. Unlike the legs these are NOT solved as IK —
  // see the note at the key table in pose.cpp for why an off-plane hand
  // target leaves a hinged elbow pointing somewhere nobody asked for.
  float armPitch = 0;
  float armSwing = 0;  // signed OPPOSITE this arm's own leg
  float armOut = 0;
  float armFlex = 0;
  float lean = 0;  // radians of forward torso pitch this shape carries
};
// The name the avatar's callers grew up with.
using AvatarAirKey = AirKeyPose;

// WHAT A DRIVER HANDS THE PIPELINE, once per tick.
//
// Every field is a FACT the driver owns and the pipeline cannot know. Nothing
// in here names which driver filled it: the `pose-parity` gate feeds one of
// these to an NPC and to the avatar on the same def and asserts the same pose
// comes out, so a field that meant "am I the player" would be a second pipeline
// hiding behind a bool.
struct PoseInputs {
  // ---- THE VELOCITY SOURCE ------------------------------------------------
  // World voxels per second, whatever integrator currently owns the body: the
  // player's controller for the avatar, the ORIGIN'S OWN TRAVEL for an NPC
  // (which moves itself one step per tick, so a difference over the tick is
  // exact there — and is exactly the measurement that was garbage on the
  // avatar, whose controller runs per frame). Planar drives the gait, the
  // springs and the clip rates; `y` is the phase of the airborne pose.
  Vec3 velocity{};
  // ---- SUPPORT ------------------------------------------------------------
  // The driver's own view of "the feet have a floor", already debounced the
  // way that driver needs (the avatar's coyote window; an NPC's `airborne_`,
  // which does not flicker). False runs the airborne branch.
  bool grounded = true;
  // Is this body actually IN THE AIR, as opposed to not-grounded? A hang, a
  // mantle, a swim and fly mode are all not grounded and all not falling
  // (gotcha-swimmer-is-airborne-by-construction); the air pose must not play
  // in any of them. False disables the air pose outright.
  bool airPoseEligible = false;
  // ---- WHO OWNS THE BODY'S HEIGHT -----------------------------------------
  // FromDriver: `origin_.y` IS the sole of the body — the player's AABB has
  // already been resolved against the terrain — and re-deriving height from
  // feet whose goal falls back to that same height is the runaway feedback
  // loop gotcha-avatar-gait-height-feedback describes. The pelvis then has to
  // CROUCH to buy the stride any reach, which is why the stance crouch and the
  // held crouch only apply in this mode.
  //
  // FromFeet: height is the planted-foot average, bounded against the ground
  // the body's own footprint reports and eased (MobSystem::EaseBodyY). An
  // NPC's `origin_.y` is a ground snap that climbs a step in one tick with no
  // smoothing at all; drawing the body there would pop it up every voxel of a
  // hillside, which is what the foot average exists to smooth.
  enum class Height : uint8_t { FromFeet, FromDriver };
  Height height = Height::FromFeet;
  // Lean the body into the ground's grade (the four-probe fore/aft + left/right
  // grade clamped by LocomotionDef::tiltMaxDeg). Off keeps the torso upright.
  bool tiltFromGround = true;
  // ---- LOOK TARGET --------------------------------------------------------
  // Where the head is asked to look, as a GOAL in Mob::ApplyAimPart's angle
  // convention (yaw = heading delta, pitch positive up). The pipeline eases
  // toward it on avatar.headLookHalflife; with `haveLook` false it eases back
  // to straight ahead. `lookSpineShare` is how much of the yaw the spine takes.
  bool haveLook = false;
  float lookYaw = 0.0f, lookPitch = 0.0f;
  float lookSpineShare = 0.0f;
  // ---- AIM TARGET ---------------------------------------------------------
  // The strike aim (Mob::ApplyStrikeAim): an Aim-mode effector pointed along a
  // live stroke, or a look at `aimLook_` between strokes. Both halves are gated
  // on the creature's own state, so this is a no-op on a body with no aim
  // effector and no look target; the flag exists so a driver can refuse it.
  bool strikeAim = true;
  // MobDef::chaseClip wanted on (the brain has a target and is engaged).
  bool chaseWant = false;
  // ---- DRIVER EXTRAS ------------------------------------------------------
  // The held crouch (Ctrl): eased toward player.crouchKneeDrop.
  bool crouch = false;
  // Ledge-hang palms: pin the arm chains' item sockets to a held lip.
  bool hangActive = false;
  IVec3 hangLip{};       // the held lip voxel, world
  Vec3 hangDir{1, 0, 0};  // horizontal facing at grab time, toward the wall
  // The ledge climb out of that hang (pose.cpp, "the ledge climb"): the muscle-up,
  // the knee onto the lip and the stand, all keyed on `climbRise` — how far
  // the BODY has risen, 0 = the dead hang, 1 = feet on the lip (Player::
  // LedgeClimbRise) — against the same hangLip / hangDir as the hang.
  bool climbActive = false;
  float climbRise = 0.0f;
  // Anything else a driver needs to solve, AFTER the leg IK, the weapon arm
  // and the hang palms and BEFORE the anatomical clamp (so it is clamped like
  // everything else). Empty on both current drivers.
  std::function<void(const AnimSkeleton&, AnimState&)> extraIk;
};

// The pipeline's own state between ticks, per creature (Mob::pose_). Was
// PlayerAvatar-only until W2-L; every creature carries it now because every
// creature runs the stages that read it.
struct PoseDrive {
  // How strongly the leg IK is applied, 0..1, eased on avatar.ikBlendHalflife.
  // "The legs are IK-driven", not "the gait is running": it stays at 1 through
  // a take-off, and it is the TARGET that crossfades from the last plant to the
  // air pose. See the note in PosePipeline.
  float gaitWeight = 0.0f;
  // ---- the velocity-driven airborne pose (avatar.airPose) ----------------
  float airFrac = 0.0f;   // how much of the pose the air shapes own, 0..1
  float airVy = 0.0f;     // vertical velocity the air pose poses from, vox/s
  float airLandW = 0.0f;  // how far into the landing reach, 0..1, eased
  float airLeanPitch = 0.0f, airLeanRoll = 0.0f;  // radians
  AirKeyPose airKey;
  // ---- the stride clock (Mob::SyncStrideClock) ---------------------------
  // Seconds since the last touchdown, the smoothed step period it measures,
  // the stride rate derived from it (strides/sec, 0 = not walking) and which
  // leg landed last.
  float sinceTouchdown = 0.0f;
  float stepPeriod = 0.0f;
  float strideRate = 0.0f;
  int lastFootDown = -1;
  // ---- the pelvis ---------------------------------------------------------
  // Stance crouch: the gait's reach budget when the driver pins the height
  // (PoseInputs::Height::FromDriver). Held crouch: the Ctrl pose.
  float stanceCrouch = 0.0f;
  float crouchHold = 0.0f;
  // ---- head look: the smoothed angles the rig is actually posed at -------
  float lookYaw = 0.0f, lookPitch = 0.0f;
  // ---- ledge-hang palms, faded like gaitWeight ---------------------------
  float hangIkWeight = 0.0f;
  // ---- the ledge climb: how much of the pose it owns, faded the same way,
  // and the rise it is posed at (held through the fade-out) -----------------
  float climbW = 0.0f;
  float climbRise = 0.0f;
  IVec3 climbLip{};
  Vec3 climbDir{1, 0, 0};
  // ---- lateral undulation (Mob::ApplySlither) -----------------------------
  // The odometer the wave's phase is (world voxels travelled along the
  // facing), the heading last tick (for the turn rate the bend is read off)
  // and the eased path curvature that bend is drawn at.
  float slitherOdo = 0.0f;
  float slitherHeading = 0.0f;
  float slitherKappa = 0.0f;
  bool slitherInit = false;
  // Has the drawn height ever been set by the pipeline? The feet-derived
  // height snaps to its target the first time and eases ever after.
  bool bodyPlaced = false;
};
