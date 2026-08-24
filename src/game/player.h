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

  // Render-only eye position: EyePos plus a vertical offset that cancels the
  // instantaneous pop when the body steps up/down a voxel ledge, then decays
  // to zero (tuning.json player.viewSmoothHalflife). The raymarch camera is
  // its ONLY consumer — physics, picking rays and everything that can feed the
  // sim keep using EyePos()/pos, so the world hash cannot be affected.
  Vec3 ViewEyePos() const { return pos + Vec3{0, kEyeOffset + viewYOffset, 0}; }

  Vec3 pos{128, 100, 140};  // AABB center
  Vec3 vel{0, 0, 0};
  bool fly = true;          // start in fly mode until the first mirror arrives
  bool grounded = false;
  bool inLiquid = false;
  // Fraction of the body under liquid, 0..1. Every liquid effect (drag,
  // buoyancy, wade speed) scales with this rather than switching on the first
  // submerged sample, so ankle-deep and fully-under are different states.
  float submersion = 0.0f;

  // ---- water-edge mantle (climbing out of a pool) ----
  // Set for one frame when a jump pressed into a water's edge is accepted;
  // informational for the caller (cues, avatar animation, tests).
  bool waterJumped = false;
  // While non-zero, the body is being pulled up onto `mantleTarget` — the
  // standing position on top of the ledge that WaterLedgeAhead validated.
  // Counts down in seconds; the climb is rate-limited rather than a teleport.
  //
  // A mantle rather than a ballistic impulse because a floating swimmer's feet
  // dangle most of a body below the waterline (buoyancy is a gravity scale, so
  // equilibrium sits deep), which makes the lip of an ordinary pool an 11-voxel
  // lift from a dead float — past what a jump reaches. Tuning the impulse up to
  // cover it would launch you off shallow banks by the same amount. Committing
  // to a validated target instead makes climbing out reliable at any depth and
  // at any bank height, which is what the move is actually for.
  float mantleTimer = 0.0f;
  Vec3 mantleTarget{};
  // Climb rate of the active mantle, m/s — set alongside mantleTarget by
  // whichever move latched it (waterMantleSpeed or ledgeMantleSpeed), so the
  // one drive loop serves both without re-deciding whose climb this is.
  float mantleSpeed = 0.0f;
  // True while the active mantle is a ledge pull-up. A pull-up is a HOLD:
  // releasing W mid-climb cancels it and lowers the body back to the hang
  // (space still gripping) instead of finishing a fire-and-forget event.
  // Water climb-outs stay committed — they were triggered by a discrete jump
  // press and have no hold to release.
  bool mantleFromHang = false;

  // ---- ledge grab (procedural climbing) ----
  // Airborne with space held and the arms facing a voxel lip within hand
  // reach, the body latches on and dangles (LedgeGrabAhead in player.cpp).
  // Pressing forward pulls it up: onto the lip through the same committed
  // mantle the water edge uses when there is room to stand, or as a ballistic
  // arm boost when there is not (a noisy wall's one-voxel ledge) — from the
  // top of that boost the next lip is within reach, which is how a rough wall
  // is scaled: grab, boost, grab. All feel numbers live in tuning.json
  // player.ledge* (reach, hang drop, boost, mantle speed/timeout).
  bool hanging = false;       // dangling from hangLip by the hands
  bool ledgeGrabbed = false;  // one frame, when a grab latches (cues/tests)
  IVec3 hangLip{};            // the solid voxel the hands are on
  Vec3 hangAnchor{};          // where the body settles while dangling
  Vec3 hangStand{};           // standing spot on the lip, re-validated at pull-up
  Vec3 hangDir{1, 0, 0};      // horizontal facing at grab time, toward the wall
  // Live HUD readout: the lip probe runs EVERY walk frame — grounded, rising,
  // space or not — so the dev panel can say "a lip is in reach and here is the
  // latch gate that refused" rather than the player inferring it. Costs one
  // LedgeGrabAhead per frame (~a hundred cell reads), nothing when flying.
  bool ledgeInReach = false;  // a grabbable lip is within hand reach right now
  IVec3 ledgeLip{};           // which lip (valid while ledgeInReach or hanging)
  // Seconds spent in the current hang. The pull-up honours W only after
  // tuning.json player.ledgePullDelay of it: W is almost always still held
  // from the jump approach, and without the delay the mantle fired on the
  // very first hang frame — the catch-and-dangle beat never existed on
  // screen, which read as "hanging doesn't work".
  float hangTime = 0.0f;

  // Jump grace windows, seconds remaining. coyoteTimer keeps a jump legal
  // briefly after leaving the ground; jumpBuffer remembers a press made just
  // before landing. On noisy terrain the grounded/airborne boundary is
  // genuinely ragged frame to frame, so without these a jump pressed while
  // running over rough ground is silently swallowed on the frames the body
  // happens to be cresting a bump.
  float coyoteTimer = 0.0f;
  float jumpBuffer = 0.0f;

  // View-smoothing state (voxels): when the BODY snaps vertically by a step
  // (step-up climb or the walk-down ground snap), the negative of that snap is
  // added here so the EYE stays put that frame, then Update() decays it toward
  // zero exponentially. Clamped to one step height so falls, teleports and
  // spawns never smear the camera. Zeroed in fly mode and on teleports.
  // Render-only — see ViewEyePos().
  float viewYOffset = 0.0f;

  // ---- avatar damage coupling ----
  // Multipliers the PlayerAvatar's dismemberment state feeds in (see
  // AvatarLocomotion): a wizard missing a leg walks at speedScale and jumps at
  // jumpScale, and one missing both cannot jump at all.
  //
  // Kept as plain fields set by the caller rather than as extra Update()
  // parameters so that every existing caller — including tests/movement_test,
  // which has no avatar at all — keeps compiling and keeps its 1.0 defaults.
  // They multiply the tuned speeds, so "intact" is exactly the old behaviour.
  float speedScale = 1.0f;
  float jumpScale = 1.0f;
  bool canJump = true;

  // Largest single-frame velocity LOSS to a collision sweep since the avatar
  // last drained this, in voxels/sec. Magnitude = how hard the body hit
  // something: a fall arrested by the ground, or a horizontal slam into a wall.
  // The avatar reads it for impact damage instead of relying on landing
  // detection, so one code path covers both.
  //
  // A LATCH, not a per-frame value, and that distinction IS the feature.
  // Update() runs once per FRAME; the avatar consumes this inside the fixed-tick
  // loop, which runs ZERO times on any frame where the accumulator has not
  // reached a whole tick — at 60+ fps against the 30 Hz tick that is most
  // frames. An impact is a single-frame event, so a plain per-frame assignment
  // is overwritten by the next frame's ~0 (the body is grounded by then) before
  // any tick ever reads it, and fall damage never fires at all. This is the same
  // trap the RMB cast latch documents in main.cpp, and every other one-shot here
  // (prefab stamp, mob spawn, detonate) is sticky for exactly this reason.
  //
  // PEAK-HOLD, not accumulate: gravity contributes ~1.6 vox/s of cancelled
  // velocity on every grounded frame, so summing would reach a lethal total
  // just standing still. The largest single arrest is the impact.
  //
  // Cleared by main.cpp on the first tick of the frame batch that sees it —
  // which is also what stops a multi-tick frame applying the same hit 4 times.
  Vec3 impactDeltaV{0, 0, 0};

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
