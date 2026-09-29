#pragma once
#include <algorithm>
#include <cmath>
#include <functional>

#include "math3d.h"
#include "sim/tickinput.h"
#include "sim/world.h"

// THE CONTROLLER IS ON THE FIXED TICK (docs/PLAN_multiplayer_now.md N2).
//
// `PlayerInput` — a per-FRAME movement intent filled from GLFW polling — is
// gone. Its replacement is `TickInput` (sim/tickinput.h), one command per
// TICK, and Update() is called once per tick at kTickDt from inside the
// fixed-tick loop rather than once per frame with a frame dt. Everything the
// controller integrates (exp/pow smoothing, the coyote/buffer/hang timers, the
// mantle drive) therefore advances on the sim clock and produces the same
// trajectory at any frame rate — which is what the `tick-input` gate asserts
// and what a networked client will have to reproduce from a command stream.
//
// AABB character vs the one-tick-latent voxel mirror (DESIGN.md v0 note).
// All units are voxels; sizes below are stated in meters and converted via
// kVoxelMeters so voxel size can be tuned in one place (world.h).
// ---- THE LEDGE CLIMB IS A MUSCLE-UP, NOT A LIFT ----------------------------
//
// The pull-up from a hang used to raise the body at a flat ledgeMantleSpeed,
// which with no animation on it read as floating up the wall. It now follows
// an authored height-vs-time curve whose SLOW BEATS are the ones a body makes:
// the explosive pull that gets the chest over the hands, a stop at the top of
// it, the slow heave that brings the waist to the lip, a lag while one knee
// swings up onto it, then the stand. The pose (pose.cpp, "the ledge climb") keys its
// phases on the RISE (Player::LedgeClimbRise), not on the clock, so wherever
// the body stalls the limbs stall with it.
//
// `t` is a fraction of player.ledgeClimbTime, `h` the fraction of the rise from
// the dead hang to standing on the lip. Strictly increasing in both — the curve
// is inverted when a cancelled climb resumes part-way — and interpolated
// monotone (Fritsch-Carlson), so the flat stretches are slow, never backward.
namespace ledgeclimb {
struct Key {
  float t, h;
};
inline constexpr Key kKeys[] = {
    {0.00f, 0.000f},  // dead hang
    {0.06f, 0.012f},  // loading the pull
    {0.24f, 0.260f},  // the explosive pull: chest over the hands
    {0.34f, 0.272f},  // STOP at the top of it
    {0.52f, 0.500f},  // the heave: arms press out, waist to the lip
    {0.66f, 0.545f},  // lag: the knee swings up onto the lip
    {0.78f, 0.680f},  // weight onto the knee
    {1.00f, 1.000f},  // stand up
};
// The pose's phase marks, on the same `h` axis as the keys above.
inline constexpr float kHChestOver = 0.26f;  // shoulders have passed the hands
// Measured on the human rig (ledge-climb-pose): hips are ~0.78 m over the
// soles, so they reach the lip top at h ~0.50 — the knee cannot go on before
// that without folding the hip past its 85 degree limit into the block.
inline constexpr float kHWaist = 0.50f;      // arms locked out, hips at the lip
inline constexpr float kHKneeOn = 0.545f;    // lead knee is on the lip
inline constexpr float kHKneel = 0.68f;      // weight is on that knee
// h at `u` (a fraction of the climb time), and the inverse.
float RiseAt(float u);
float TimeAt(float h);
}  // namespace ledgeclimb

class Player {
 public:
  using KindFn = std::function<CellKind(IVec3)>;
  // HOW TALL a Solid cell is, as a fraction 0..1 of a voxel (1 = a whole
  // cell). Only a PARTIAL POWDER cell is shorter: it is grains up to
  // y + mass/8 (docs/PLAN_powder_mass.md P4). Null = every Solid is whole,
  // which is what every fixture and every non-player caller gets.
  using TopFn = std::function<float(IVec3)>;
  TopFn cellTop;

  // ONE TICK of the controller. `dt` is kTickDt from every live caller; it
  // stays a parameter because the fixtures drive shorter and longer steps to
  // probe the integrators.
  void Update(float dt, const TickInput& in, const KindFn& kindAt);
  // The same call for fixtures that build an intent without a camera: the
  // basis is passed alongside instead of inside the command. A convenience,
  // not a second code path — it fills the three basis fields and forwards.
  void Update(float dt, TickInput in, const Vec3& flatFwd, const Vec3& right,
              const Vec3& lookFwd, const KindFn& kindAt) {
    in.flatFwd = flatFwd;
    in.right = right;
    in.lookFwd = lookFwd;
    Update(dt, in, kindAt);
  }

  // Positional shove from debris rigidbodies (Physics::PlayerPushOut), applied
  // through the same voxel sweeps as movement so a body can never push the
  // player inside terrain. An upward push while falling counts as support, so
  // standing on debris works.
  void ApplyPush(Vec3 push, const KindFn& kindAt);

  // ---- THE COLLISION BOX IS NOT THE FIGURE -------------------------------
  //
  // `pos` is the centre of the NOMINAL 1.7 m figure box (kHalfXZ x kHalfY):
  // the sole is `pos.y - kHalfY`, the avatar art hangs off that, and every
  // test and caller that says "feet = pos.y - kHalfY" keeps being right. But
  // the box the SWEEPS use is a smaller one standing on that same sole —
  // tuning.json player.collisionWidth wide and player.collisionHeight tall
  // (player.crouchHeight while crouched) — and it decides movement alone.
  // The shoulders, arms and the top of the head overhang it and are allowed
  // to clip terrain by exactly that overhang.
  //
  // Why: a corridor whose ceiling was a head-clip above the figure refused
  // the figure, and a doorway the shoulders brushed refused it too. What
  // the player experiences as "where I am" is where the feet go, so the box
  // is sized from the feet up and the limbs are cosmetic. A rig with arms
  // out or a hat on does not change how it fits through a gap.
  struct Box {
    float hx;   // half-width, voxels
    float yLo;  // bottom relative to pos.y (the sole: -kHalfY)
    float yHi;  // top relative to pos.y
    // The body's cellTop (BoxFor fills it), carried on the box because the
    // box is what every sweep and probe in player.cpp already receives.
    const TopFn* top = nullptr;
  };
  Box BoxFor(bool crouched) const;
  Box CurrentBox() const { return BoxFor(crouching); }
  // Height of the live collision box above the sole, voxels.
  float BoxHeight() const {
    const Box b = CurrentBox();
    return b.yHi - b.yLo;
  }

  // Set the eye offset from the avatar's model data. Called once when the
  // avatar spawns or changes def. eyeFromFeet is in world voxels from the
  // creature's bottom; converted to an offset from pos (the AABB centre).
  void SetModelEyeHeight(float eyeFromFeet) {
    modelEyeOffset_ = eyeFromFeet - kHalfY;
  }

  // The first-person eye rides at the FIGURE's face row or just under the top
  // of the live collision box, whichever is lower. The box is what is
  // guaranteed to be clear of terrain; the face row is not, and a camera inside
  // a ceiling voxel is the one thing the head-clip must not buy. Crouching
  // drops it with the box.
  float EyeOffsetNow() const {
    const float base = modelEyeOffset_ > 0 ? modelEyeOffset_ : kEyeOffset;
    const Box b = CurrentBox();
    return std::min(base, b.yHi - kEyeBelowTopM / kVoxelMeters);
  }
  Vec3 EyePos() const { return pos + Vec3{0, EyeOffsetNow(), 0}; }

  // Render-only eye position: EyePos plus a vertical offset that cancels the
  // instantaneous pop when the body steps up/down a voxel ledge, then decays
  // to zero (tuning.json player.viewSmoothHalflife). The raymarch camera is
  // its ONLY consumer — physics, picking rays and everything that can feed the
  // sim keep using EyePos()/pos, so the world hash cannot be affected.
  // A crouch's eye drop is banked into the same offset, so the camera eases
  // down and up instead of stepping by the box-height change.
  Vec3 ViewEyePos() const {
    return pos + Vec3{0, EyeOffsetNow() + viewYOffset, 0};
  }

  // ---- RENDER INTERPOLATION (docs/PLAN_multiplayer_now.md N2) -------------
  //
  // The body now moves ONLY on the 30 Hz tick, so at 144 fps four frames in
  // five would draw the eye at the identical position and the fifth would jump
  // — a visible 30 Hz stutter that no amount of view smoothing hides. `alpha`
  // is the frame loop's leftover accumulator as a fraction of a tick, exactly
  // as Celestial::RenderTickInterp already uses it for the sky, and the camera
  // rides prevPos -> pos by it.
  //
  // RENDER ONLY, and that is a hard rule rather than a preference: the sim
  // must never read a value derived from a frame quantity or the world stops
  // being a function of the tick stream. Every picking ray, the brush, the
  // laser, the grenade, physics and the audio LISTENER's own logic keep using
  // pos/EyePos(). The only consumers are the raymarch camera, the third-person
  // boom's focus and the ear position — all presentation.
  Vec3 RenderPos(float alpha) const {
    return prevPos + (pos - prevPos) * alpha;
  }
  // How much of the step-smoothing offset is still standing `alpha` of the way
  // through this tick.
  //
  // THE SMOOTHING WAS ITSELF A 30 Hz STAIRCASE until 2026-09-21, and it is
  // worth being precise about why, because it is the same mistake as the one
  // BankVerticalSnap fixes wearing different clothes. The offset is aged once
  // per TICK, at the top of Update, so across the five frames of a tick it is
  // a CONSTANT and then drops by the whole tick's worth at the boundary. With
  // the default 0.1 s half-life that is 20.6% of the offset released in one
  // frame — 0.21 voxels on a one-voxel step, measured — so a "smoothed" climb
  // still arrived in thirty discrete lumps a second. Fading the offset by the
  // same exponential WITHIN the tick makes the release continuous at any frame
  // rate, and it costs no state: `viewDecayPerTick` is the exact factor the
  // next Update will apply, so alpha=1 here lands on the value the next tick
  // starts from. Render only, like everything else on this path.
  float SmoothFade(float alpha) const {
    return std::pow(viewDecayPerTick, alpha);
  }
  Vec3 RenderEyePos(float alpha) const {
    return RenderPos(alpha) +
           Vec3{0, EyeOffsetNow() + viewYOffset * SmoothFade(alpha), 0};
  }
  // Kill the interpolation for one frame: a teleport (world load, lab scene
  // placement, the autofly park pin) must not be smeared across a tick.
  void SnapRender() { prevPos = pos; }

  // Where the BODY should be drawn this frame, relative to where the sim
  // believes it is. The player's art (PlayerAvatar) is posed once per tick
  // around `pos`, so without this it stair-steps at 30 Hz while the camera —
  // which has ridden RenderEyePos since N2 — glides, and it takes a whole
  // step-up in one frame while the eye eases up over the half-life. Both
  // corrections are the camera's own, so body and eye move as one rigid thing.
  //
  // `bodyYOffset`, NOT `viewYOffset`: the two agree on every body snap and
  // differ on the crouch, which changes where the EYE sits inside a body that
  // has not moved at all. Feeding the crouch bank to the art would slide the
  // whole figure down through the floor while the avatar's own stanceCrouch_
  // was already bending its knees.
  //
  // RENDER ONLY: nothing derived from `alpha` or from these offsets may reach
  // the sim, physics or a picking ray.
  Vec3 RenderBodyOffset(float alpha) const {
    return (RenderPos(alpha) - pos) +
           Vec3{0, bodyYOffset * SmoothFade(alpha), 0};
  }

  // Bank a vertical SNAP of the body — a step-up climb, the walk-down ground
  // snap, the unstick lift, a scripted mantle or ledge settle — out of the
  // render path. `dy` is how far `pos.y` just jumped (signed, up positive).
  //
  // TWO corrections, and until 2026-09-21 only the first was applied:
  //   * `viewYOffset -= dy` holds the eye where it was and lets it glide back
  //     over viewSmoothHalflife, and
  //   * `prevPos.y += dy` moves the render lerp's START to AFTER the snap,
  //     because a snap is not travel along this tick's segment.
  // Without the second, RenderPos re-adds the whole snap linearly across the
  // tick while the offset subtracts it in one lump: the eye drops by the full
  // step height at every tick boundary and ramps back up over the next 33 ms.
  // Walking up a hill that is a 30 Hz sawtooth of one voxel — exactly the
  // "it teleports up a voxel at a time" this whole system exists to remove.
  void BankVerticalSnap(float dy) {
    viewYOffset -= dy;
    bodyYOffset -= dy;
    prevPos.y += dy;
  }

  // The EYE moved inside a body that did not: a crouch changes EyeOffsetNow by
  // the box-height change. Banked so the camera eases down and up instead of
  // stepping — but deliberately NOT into bodyYOffset (see RenderBodyOffset).
  void BankEyeShift(float dy) { viewYOffset -= dy; }

  // Cap the smoothing at `maxOff` voxels: anything bigger than a step is not a
  // step (a long fall the ground snap resolved, a spawn, a shove) and smearing
  // it reads as lag, not smoothness. The excess has to come off BOTH halves of
  // the body bank or the render lerp quietly re-smears what the clamp just
  // refused, so what is taken out of bodyYOffset is taken out of prevPos too.
  void ClampViewSmooth(float maxOff) {
    viewYOffset = std::clamp(viewYOffset, -maxOff, maxOff);
    const float clamped = std::clamp(bodyYOffset, -maxOff, maxOff);
    prevPos.y += bodyYOffset - clamped;
    bodyYOffset = clamped;
  }

  // Decay the render-only step-smoothing offsets toward zero, frame-rate
  // independently: half the remaining distance every viewSmoothHalflife
  // seconds at any tick rate. Called at the top of Update, and ALSO while the
  // ragdoll owns the body (session.cpp) — Update does not run then, and an
  // offset left frozen would hold the camera and the body off the ground for
  // as long as the ragdoll lasts. A half-life of 0 disables smoothing.
  void DecayViewSmooth(float dt, float halflife) {
    if (halflife > 1e-4f) {
      const float k = std::pow(0.5f, dt / halflife);
      viewYOffset *= k;
      bodyYOffset *= k;
      // Published for SmoothFade: the frames of the tick about to run spread
      // THIS factor out instead of waiting for the boundary to apply it.
      viewDecayPerTick = k;
    } else {
      viewYOffset = bodyYOffset = 0.0f;
      viewDecayPerTick = 0.0f;
    }
  }

  // Drop the smoothing entirely: a teleport, a world load, fly mode. Pairs
  // with SnapRender at every site that moves the body on purpose.
  void ResetViewSmooth() { viewYOffset = bodyYOffset = 0.0f; }

  Vec3 pos{128, 100, 140};  // centre of the nominal figure box (see Box)
  // `pos` at the START of the tick Update() is running, for RenderPos above.
  // Written by Update and by SnapRender, read by nothing that feeds the sim.
  Vec3 prevPos{128, 100, 140};
  Vec3 vel{0, 0, 0};
  bool fly = true;          // start in fly mode until the first mirror arrives
  bool grounded = false;
  // Ctrl held on the ground (or in the air; not while swimming, hanging or
  // mantling). The collision box is crouchHeight tall while this holds and
  // ground speed is scaled by crouchSpeedScale. STICKY under a low ceiling:
  // releasing Ctrl only stands the body up once the standing box fits, so
  // a crawl-space cannot wedge you by letting go at the wrong moment. The
  // avatar reads it for the knee bend. Fly mode clears it (Ctrl is descend).
  bool crouching = false;
  bool inLiquid = false;
  // Any part of the body wet is `inLiquid`; SWIMMING is the state where the
  // water has actually taken over — the body three-quarters under
  // (kSwimSubmersion in player.cpp; depth alone, and that note says why the
  // tempting "and no floor underfoot" half is wrong). It is the flag the
  // movement rules want: `inLiquid` used to stand in for it and cost a wader
  // their jump, their step-up and their ground snap for one voxel of puddle.
  // Never true in fly or during a scripted climb.
  bool swimming = false;
  // THE DROP IS RATE-LIMITED BY THE MIRROR: the body is descending faster than
  // the CPU mirror can answer for the cells beneath it, so this frame's fall is
  // clamped to the distance the mirror does vouch for and gravity is not added
  // on top (see KnownDrop in player.cpp). Fall speed is kept, so the drop
  // resumes at its real rate the moment the snapshot catches up. Exposed
  // because a fall that visibly drags is the only symptom, and without a name
  // for it the next person to see one has nothing to grep for: it is a starved
  // readback, not a physics bug.
  bool blindFall = false;
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
  // Seconds into the ledge climb's authored RISE PROFILE (ledgeclimb::kKeys,
  // player.ledgeClimbTime long). Only the ledge pull-up reads it; the water
  // climb-out is still a flat-rate drive. Held back while a live world blocks
  // the rise, so the clock never runs ahead of the body.
  float climbClock = 0.0f;
  // How far through the ledge climb the BODY is, 0 (the dead hang) .. 1
  // (feet on the lip), measured from pos and hangLip — not from climbClock.
  // That makes it the one number the pose can follow on any body that has
  // those two facts, a network ghost included, and keeps the limbs in step
  // with where the body actually is when the rise is held or cancelled.
  float LedgeClimbRise() const;

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
  // Seconds until a jump press counts again. Every press arms it, used or
  // not, so mashing space cannot keep the buffer permanently primed — the
  // one-legged hop was an infinite bunny-hop by spam.
  float jumpLockout = 0.0f;

  // View-smoothing state (voxels): when the BODY snaps vertically by a step
  // (step-up climb or the walk-down ground snap), the negative of that snap is
  // added here so the EYE stays put that frame, then Update() decays it toward
  // zero exponentially. Clamped to one step height so falls, teleports and
  // spawns never smear the camera. Zeroed in fly mode and on teleports.
  // Render-only — see ViewEyePos().
  float viewYOffset = 0.0f;
  // The same accumulator restricted to snaps of the BODY, i.e. everything
  // viewYOffset carries except the crouch's eye-height change. This is what
  // the player's art is drawn by (RenderBodyOffset), so that the figure and
  // the camera ride a step together instead of the art taking the whole voxel
  // in one frame while the eye eases up over the half-life.
  float bodyYOffset = 0.0f;
  // The decay factor the LAST DecayViewSmooth applied, i.e. the one the
  // next tick will apply again. SmoothFade raises it to `alpha` so the
  // offset eases out across the tick's frames instead of stepping down by
  // a whole tick's worth at the boundary. Render-only; 1.0 means "nothing
  // has been aged yet", which is the correct no-op.
  float viewDecayPerTick = 1.0f;

  float modelEyeOffset_ = 0;

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
  // Ground-control multiplier on groundAccel: 1 = normal footing, lower =
  // slippery (oily feet -- session.cpp derives it from the feet's coat). Only
  // the ON-GROUND rate: air and water control do not care what is on a sole.
  float groundGrip = 1.0f;
  // OUTPUT: the horizontal velocity (vox/s) the feet are NOT walking -- the
  // coast on slippery footing (groundGrip < 1): nothing held, or held in a
  // direction the body is not yet moving. Zero on normal ground and in the
  // air. The avatar's gait animates `vel - slideVel` and carries the planted
  // feet along by slideVel, so a slide is a glide, not a walk. Render/anim
  // only: nothing in the sim reads it.
  Vec3 slideVel{0, 0, 0};
  bool canJump = true;

  // Largest single-frame velocity LOSS to a collision sweep since the avatar
  // last drained this, in voxels/sec. Magnitude = how hard the body hit
  // something: a fall arrested by the ground, or a horizontal slam into a wall.
  // The avatar reads it for impact damage instead of relying on landing
  // detection, so one code path covers both.
  //
  // A PEAK-HOLD OVER THE TICK. Update() and the avatar's consumer now run in
  // the same tick (N2 moved the controller inside the loop), so this no longer
  // has to survive a frame batch — but it is still a peak and not the last
  // value written, because ONE tick can arrest the body more than once: the
  // unstick lift, the vertical sweep and the horizontal sweep each cancel
  // velocity, and the largest single arrest is the impact.
  //
  // NOT AN ACCUMULATION: gravity contributes ~1.6 vox/s of cancelled velocity
  // on every grounded tick, so summing would reach a lethal total just
  // standing still.
  //
  // Cleared by main.cpp at the end of the tick that produced it, whether or
  // not an avatar consumed it. A peak-hold that is never drained only ratchets
  // upward, and the next avatar to spawn would inherit the hardest hit of the
  // session and die on its first tick.
  Vec3 impactDeltaV{0, 0, 0};

  // A JUMP WAS ACTUALLY LAUNCHED THIS TICK. Set by Update, read by the
  // avatar's `jump` clip later in the same tick, cleared by main.cpp at the
  // end of it — the same one-tick lifetime impactDeltaV above has.
  //
  // The avatar's `jump` clip keys on THIS rather than on losing ground contact.
  // Contact loss is not a jump: stepping off a kerb at 16 voxels/s clears the
  // 0.12 s air debounce easily, and the clip is an arms-up one-shot, so every
  // step-down on broken ground threw the arms in the air. Only a launch is a
  // launch.
  bool jumped = false;

  // The NOMINAL figure box: what the art is drawn to (gen_human.py asserts
  // it), what the Jolt player proxy and the mob sense actor are sized from,
  // and the frame every "feet = pos.y - kHalfY" expression lives in. NOT the
  // collision box the sweeps use — see Box above.
  static constexpr float kHalfXZ = 0.30f / kVoxelMeters;     // 0.6 m wide
  static constexpr float kHalfY = 0.85f / kVoxelMeters;      // 1.7 m tall
  static constexpr float kEyeOffset = 0.65f / kVoxelMeters;  // the face row
  // How far under the live box top the eye sits when the box is what limits
  // it (EyeOffsetNow). Over half a voxel so the eye's cell is the box's top
  // cell, which the sweeps keep clear, and never the ceiling above it.
  static constexpr float kEyeBelowTopM = 0.06f;

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
