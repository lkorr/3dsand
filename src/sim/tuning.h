#pragma once
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Runtime-tunable parameters (assets/materials/tuning.json)
// ============================================================================
// The look-and-feel counterpart to materials.json. Where materials.json says
// what a voxel IS, this says how the engine renders and moves it: sky and fog,
// water and lava shading, AO and shadows, player speeds, Jolt body materials,
// debris budgets, and the integer sim constants.
//
// Two delivery paths, because the values land in two different places:
//
//   1. SHADER params are emitted as WGSL `const` declarations by WgslBlock()
//      and prepended by LoadShader() alongside ShaderConstantPrelude(). The
//      shaders name these constants instead of hardcoding literals, so F5
//      (Simulation::ReloadShaders) re-reads the JSON and recompiles against
//      the new values. That path is already wrapped in a validation error
//      scope that keeps the old pipelines on failure, so a bad tuning value
//      cannot take the renderer down.
//
//   2. CPU params are plain fields on Tuning, read directly by player.cpp,
//      physics.cpp, debris.cpp and main.cpp. These apply on reload without a
//      recompile of anything.
//
// DETERMINISM (CLAUDE.md rule 1): the `sim.*` group feeds voxel state and is
// integer-only by construction — every field is an int, JSON floats are
// rejected, and changing any of them changes the world hash. They are exposed
// deliberately (blast size and sand fall speed are worth tuning by eye) but a
// change there means re-running --selftest to re-baseline. Everything outside
// the sim group is render- or CPU-side only and cannot perturb the hash.
// ---- per-instance variance (the "randomness" column in the tuner) ---------
//
// A Variance turns one authored constant into a distribution. It is the answer
// to "every NPC bleeds exactly the same amount": the tuned value stays the
// CENTRE, and each instance draws an offset around it, so on a rare roll a mob
// bleeds far more than the mean and the sim gets a moment worth watching.
//
// DETERMINISM (CLAUDE.md rule 1). Nothing here is a stateful RNG and nothing
// reads wall clock. Every draw is `Hash3(seed, tick, index)` — the same
// stateless counter-based scheme the sim shaders use — so two machines at the
// same tick draw the SAME offset, and a replay reproduces it exactly. That is
// what makes this safe to apply to spawn streams, which are per-tick INPUTS
// that replays must reproduce (see the Gore comment below).
//
// This is deliberately NOT available on the `sim.*` integers or on material
// interaction rules. Those feed voxel state directly through the CA, where the
// authored number IS the physics; randomising them would not add excitement,
// it would make the same collision resolve differently for no legible reason.
// Variance belongs on PRESENTATION and on per-instance CHARACTER, not on the
// rules that decide what a material does.
struct Variance {
  enum Dist : int { kNone = 0, kUniform = 1, kGaussian = 2 };
  // Scope decides what a single roll is shared across, and it is the whole
  // reason "a rare NPC is a gusher" is expressible at all:
  //   kEvent   re-rolls per droplet/spawn — droplet-to-droplet jitter.
  //   kEntity  rolls ONCE per mob (from its id) and holds for that mob's
  //            lifetime — this mob bleeds heavily, consistently, until it dies.
  // Event scope on a bleed rate averages out over a wound and reads as noise;
  // entity scope is what reads as character.
  enum Scope : int { kEvent = 0, kEntity = 1 };
  int dist = kNone;
  int scope = kEvent;
  // Half-width of the offset, in the parameter's own units. Uniform draws flat
  // in [-amount, +amount]. Gaussian treats `amount` as ONE SIGMA and clamps at
  // `sigmaClamp` sigma, so a heavy tail stays bounded (rule 2: an unbounded
  // draw on a spawn count is an unbounded particle budget).
  float amount = 0.0f;
  float sigmaClamp = 3.0f;
  // Optional hard floor/ceiling on the RESULT. Defaults are inert; a negative
  // spray count or a negative speed is meaningless, so callers that need a
  // floor set one (Apply always clamps counts at >= 0 regardless).
  float minValue = -1e30f, maxValue = 1e30f;
  bool on() const { return dist != kNone && amount != 0.0f; }
};

// Draws `base` perturbed by `v`. `seed` identifies the thing being varied
// (mob id for entity scope, or mob id mixed with the droplet index for event
// scope), `tick` is the sim tick, `index` separates draws within one tick.
//
// Pure function of its arguments — no globals, no state, no clock.
float ApplyVariance(float base, const Variance& v, uint32_t seed, uint32_t tick,
                    uint32_t index);

// Integer form, for counts (droplets, voxels, ticks). Rounds half-to-even via
// lround and clamps at >= 0 so a wide draw can never request negative work.
int ApplyVarianceI(int base, const Variance& v, uint32_t seed, uint32_t tick,
                   uint32_t index);

// ---- defaults: generated from sim/tuning_params.def -------------------------
// Every row of the table becomes one constexpr here, and the member below
// initializes from it with TPD(group, member) -- so a default is written ONCE,
// in the .def, and the struct, the loader, the shader prelude and the tuner
// cannot hold different numbers for it. check_invariants.py (`tuning reach`)
// refuses a TPD whose (group, member) is not the member it initializes.
//
// A member with a literal initializer instead of TPD is a knob with no .def
// row (the gore.*Var objects, or a knob added in the old hand style); that is
// still legal, it just has to be read by hand in LoadTuning.
//
// TP_OPEN is "no bound" in a row's min/max. NaN, so every comparison against
// it is false and the generated clamp is a no-op on that side.
inline constexpr double TP_OPEN = std::numeric_limits<double>::quiet_NaN();

namespace tuning_def {
#define TP_F(g, m, n, d, lo, hi) inline constexpr float g##_##m = d;
#define TP_I(g, m, n, d, lo, hi) inline constexpr int g##_##m = d;
#define TP_U(g, m, n, d, lo, hi) inline constexpr int g##_##m = d;
#define TP_B(g, m, n, d) inline constexpr bool g##_##m = d;
#define TP_S(g, m, n, d) inline constexpr const char* g##_##m = d;
#define TP_V3(g, m, n, x, y, z) inline constexpr float g##_##m[3] = {x, y, z};
#include "sim/tuning_params.def"
#undef TP_V3
#undef TP_S
#undef TP_B
#undef TP_U
#undef TP_I
#undef TP_F
}  // namespace tuning_def

#define TPD(g, m) (::tuning_def::g##_##m)
#define TPD_V3(g, m) \
  {::tuning_def::g##_##m[0], ::tuning_def::g##_##m[1], ::tuning_def::g##_##m[2]}

struct Tuning {
  // ---- player movement (meters / seconds; converted to voxels at use) ----
  struct Player {
    std::string model = TPD(player, model);
    float flySpeed = TPD(player, flySpeed), flySprint = TPD(player, flySprint);
    float walkSpeed = TPD(player, walkSpeed);
    float sprintSpeed = TPD(player, sprintSpeed);
    float gravity = TPD(player, gravity);
    float jumpSpeed = TPD(player, jumpSpeed);
    float swimUp = TPD(player, swimUp), swimDown = TPD(player, swimDown);
    float maxFall = TPD(player, maxFall);
    float fallDamageSpeed = TPD(player, fallDamageSpeed);
    float fallSplatSpeed = TPD(player, fallSplatSpeed);
    float fallDamageScale = TPD(player, fallDamageScale);
    // ---- what a landing does to a body (Mob::ApplyFallDamage, W2-H) --------
    // EVERY creature's since 2026-09-24, not only the player's: an NPC's limp
    // landing is measured by the same Mob::TakeRagdollImpact the avatar's is,
    // and billed by the same function. They live in this group because the
    // three onset knobs above always did. A SPLAT (>= fallSplatSpeed, or a
    // bill larger than the body's hp) carves a sphere of fallSplatCarveBase +
    // fallSplatCarvePerMs x impact m/s out of the body, throws
    // fallSplatDroplets micro droplets and fallSplatBloodVoxels whole blood
    // voxels, and shoves loose bodies within fallSplatImpulseRadius voxels by
    // fallSplatImpulsePerMs x impact m/s. A sub-lethal landing opens a bleed on
    // every LEG/FOOT-tagged limb of fallLegBleed x the damage.
    float fallSplatCarveBase = TPD(player, fallSplatCarveBase);
    float fallSplatCarvePerMs = TPD(player, fallSplatCarvePerMs);
    int fallSplatDroplets = TPD(player, fallSplatDroplets);
    int fallSplatBloodVoxels = TPD(player, fallSplatBloodVoxels);
    float fallSplatImpulseRadius = TPD(player, fallSplatImpulseRadius);
    float fallSplatImpulsePerMs = TPD(player, fallSplatImpulsePerMs);
    float fallLegBleed = TPD(player, fallLegBleed);
    float stepUp = TPD(player, stepUp);
    float smoothBump = TPD(player, smoothBump);
    float stepSpeedPenaltyPerM = TPD(player, stepSpeedPenaltyPerM);
    float minStepSpeedScale = TPD(player, minStepSpeedScale);
    float nonJumpSpeed = TPD(player, nonJumpSpeed);
    float coyoteTime = TPD(player, coyoteTime);
    float jumpBufferTime = TPD(player, jumpBufferTime);
    float jumpRepressTime = TPD(player, jumpRepressTime);
    // Accel/damping are per-second rates, converted to a per-frame lerp with
    // 1-exp(-rate*dt) at the call site. The old code lerped by a raw constant
    // every frame (ground 0.35, air 0.06, liquid 0.15, vertical drag 0.92),
    // which made acceleration and water drag scale with frame rate. These
    // rates are chosen to reproduce exactly those blends at ~100 fps — the
    // speed the game actually runs — so the feel is unchanged where it was
    // tuned, and now stays put at 30 or 144 fps instead of drifting.
    float groundAccel = TPD(player, groundAccel);
    float airAccel = TPD(player, airAccel);
    float liquidAccel = TPD(player, liquidAccel);
    // ---- slippery feet (a coat whose material lists "slippery" in its
    // materials.json coat.effects -- oil) ----
    // Grip on the ground is groundAccel scaled from 1 down to slipGrip as the
    // SOLES' amount-weighted coat fraction (MobSystem::CoatTagFraction over
    // limbs tagged "foot", sole band only) climbs from slipCoatStart to
    // slipCoatFull. A fresh oil coat is amount 6 = 0.40; the start sits above
    // oil's decayFloor sheen (2/15 = 0.133), so a greasy sheen that never
    // dries does not keep you skating until you wash it off. It was a
    // whole-foot fraction until 2026-09-23 and a foot fresh out of a pool read
    // ~0.1 of it, so oily feet did nothing.
    float slipCoatStart = TPD(player, slipCoatStart);
    float slipCoatFull = TPD(player, slipCoatFull);
    float slipGrip = TPD(player, slipGrip);
    float liquidDrag = TPD(player, liquidDrag);
    float liquidGravityScale = TPD(player, liquidGravityScale);
    float liquidSpeedScale = TPD(player, liquidSpeedScale);
    // ---- water-edge mantle (climbing out of a pool) ----
    // Swim thrust is drag-limited on purpose, which means it cannot climb out
    // of anything: at a pool wall you bob against the rim forever. So a jump
    // pressed INTO a climbable bank while in liquid pulls the body up onto it
    // (Player::Update, WaterLedgeAhead).
    //
    // A mantle rather than a bigger jump because a floating body's feet dangle
    // most of a body below the waterline, which puts an ordinary pool lip ~11
    // voxels above them — an impulse big enough to clear that from a dead float
    // would fling you off a shallow bank by the same amount. See player.h.
    //
    // How fast (m/s) the body climbs. Fast enough not to feel like a cutscene,
    // slow enough to read as pulling yourself out rather than teleporting.
    float waterMantleSpeed = TPD(player, waterMantleSpeed);
    // Hard cap (seconds) on one climb. This is a timeout, not a duration: the
    // mantle normally ends on arrival. It exists so a climb blocked partway —
    // the bank collapsed, something shoved into the target — returns control
    // instead of holding movement hostage.
    float waterMantleTime = TPD(player, waterMantleTime);
    // ---- ledge grab (procedural climbing) ----
    // Airborne with space held and the arms facing a voxel lip within hand
    // reach, the body latches on and dangles; W pulls it up (player.cpp
    // LedgeGrabAhead + the hanging block in Player::Update).
    //
    // How far above the crown of the head the hands reach (meters). A 1.7 m
    // body's standing reach is ~2.25 m, so ~0.55 past the top. Physical like
    // stepUp, so voxel-size changes never change how much real wall is
    // grabbable. 0 disables ledge grabbing entirely.
    float ledgeReach = TPD(player, ledgeReach);
    // Dangling drop: how far below the held lip the top of the head hangs.
    // NEGATIVE means the head rides ABOVE the lip. The default is negative
    // deliberately: these are chibi rigs — mina's arm chain is ~3.3 voxels
    // against a ~5 voxel shoulder-to-lip gap — and hands can only actually
    // touch the lip (avatar.cpp hang IK, shrug included) with the face at
    // the ledge edge, the way toon games hang. Long-armed rigs tolerate a
    // deeper drop; raise this and the hands stay planted as far as the
    // shrug allows.
    float ledgeHangDrop = TPD(player, ledgeHangDrop);
    // Upward velocity of the ARM BOOST — the pull-up used when there is no
    // room to stand on the lip (a rough wall's one-voxel ledge): ballistic,
    // so the next lip up can catch near the apex and the climb chains.
    // Matches jumpSpeed by default so a boost feels like a jump's worth of
    // pull. 0 turns W-on-an-unstandable-lip into simply letting go.
    float ledgeBoostSpeed = TPD(player, ledgeBoostSpeed);
    // Speed and timeout of the committed pull-up onto a standable lip. Same
    // semantics as the water mantle pair above — the timeout exists for a
    // climb blocked partway by a live world. Deliberately SLOW (a body-length
    // climb takes over a second): at the old 4.5 the pull-up read as a big
    // jump, not as hauling yourself up. The timeout must cover the full climb
    // at this speed or it aborts mid-pull.
    // Since the ledge climb got its muscle-up profile (ledgeClimbTime below)
    // the SPEED only drives the ledge climb's final step across onto the lip,
    // and the water climb-out; the timeout is floored at ledgeClimbTime + 1 s.
    float ledgeMantleSpeed = TPD(player, ledgeMantleSpeed);
    float ledgeMantleTime = TPD(player, ledgeMantleTime);
    // Seconds from the dead hang to standing on the lip. The RISE follows the
    // authored muscle-up curve (player.h ledgeclimb::kKeys: pull, stop, heave
    // the waist up, lag while the knee swings on, stand); this scales it.
    float ledgeClimbTime = TPD(player, ledgeClimbTime);
    // How fast the body settles into the dead hang after a catch. Split from
    // the mantle speed on purpose: slowing the pull-up must not make the
    // catch itself feel sluggish.
    float ledgeSettleSpeed = TPD(player, ledgeSettleSpeed);
    // Sideways hand-over-hand speed along the ledge (A/D while hanging).
    // Slow by design — it is a traverse, not a strafe. 0 disables.
    float ledgeShimmySpeed = TPD(player, ledgeShimmySpeed);
    // Minimum time a grab hangs before W (held or pressed) pulls up. W is
    // almost always still held from the jump approach, so without this floor
    // the mantle fires on the first hang frame and the catch never appears on
    // screen. 0 restores instant pull-up.
    float ledgePullDelay = TPD(player, ledgePullDelay);
    // ---- the collision box (Player::Box) ----
    // The player's collision box is NOT the figure. It is a small box centred
    // on where the feet go — collisionWidth wide, collisionHeight tall from
    // the sole — and the arms, shoulders and the top of the head are allowed
    // to clip terrain by however much the 1.7 m art overhangs it. Movement
    // is decided by this box alone: a corridor one head-clip lower than the
    // figure still admits the figure. Metres; converted at use.
    float collisionWidth = TPD(player, collisionWidth);
    float collisionHeight = TPD(player, collisionHeight);
    // Ctrl. The box shrinks to this height while crouched (and stays crouched
    // under a ceiling the standing box would not fit back under), speed is
    // scaled by crouchSpeedScale, and the avatar bends its knees by
    // crouchKneeDrop — the pelvis drops that far and the leg IK, whose foot
    // targets are world points, turns the drop into a bend.
    float crouchHeight = TPD(player, crouchHeight);
    float crouchSpeedScale = TPD(player, crouchSpeedScale);
    float crouchKneeDrop = TPD(player, crouchKneeDrop);
    // The FIGURE contract, not collision: scripts/test_mobgen.mjs reads these
    // out of tuning.json and asserts the generated human is halfHeight*2 tall
    // with the face at halfHeight+eyeOffset. The controller does not read
    // them (it has Player::kHalfY/kEyeOffset for the same 1.7 m / 1.5 m
    // numbers, static_asserted equal to these defaults in player.cpp); they
    // are in check_invariants.py's TUNING_CONSUMER_ALLOWLIST for that reason.
    float halfHeight = TPD(player, halfHeight);
    float eyeOffset = TPD(player, eyeOffset);
    // Camera step smoothing: half-life (seconds) of the render-only eye
    // offset that cancels the vertical pop when the body steps up/down a
    // ledge (Player::ViewEyePos). 0 disables. CPU/render only — the physics
    // position and the sim are untouched.
    float viewSmoothHalflife = TPD(player, viewSmoothHalflife);
    // ---- unstick (de-penetration) ----
    // Every collision sweep is a hard veto that fails from an overlapping
    // start, so a body that ends up INSIDE solid ground cannot move on any
    // axis — it is welded there until noclip. These control the way out.
    //
    // How deep (meters) the body may be buried and still be lifted clear.
    // Beyond this it stays put: being entombed by a collapse is a real state,
    // and teleporting out of it would be worse than being stuck. About a step
    // height and a half covers the cases that actually happen (a powder
    // settling into your feet, a step-down landing a fraction inside a face).
    float unstickMaxDepth = TPD(player, unstickMaxDepth);
    // How fast (m/s) the body rises while being ejected. Rate-limited rather
    // than teleported so a two-voxel lift is a glide, not a pop; the climb is
    // banked into the same view offset a step-up uses.
    float unstickSpeed = TPD(player, unstickSpeed);
    // ---- the physics grab (game/grab.h) ----
    // Hold E on a loose rigid body and it comes off the floor and rides in
    // front of the face until you let go. Tap E is unchanged (pick up / loot),
    // so these two live on the same key: the pickup fires on RELEASE, and only
    // when the hold never latched.
    //
    // How long E must be down before the grab takes over. Long enough that a
    // deliberate tap never lifts anything, short enough that holding the key
    // is not a chore. 0 disables the physics grab entirely (E stays a tap).
    float grabHoldTime = TPD(player, grabHoldTime);
    // Farthest (metres) a body can ride from the eye. A grab KEEPS the
    // distance the thing was already at, clamped to this, so the carry range
    // is a ceiling rather than a snap target — see GrabHold::Begin.
    float grabDistance = TPD(player, grabDistance);
    // WEIGHT, in three numbers. Anything at or under `grabFreeMass` is carried
    // as if it weighed nothing; past that both the player's speed and the
    // speed the body can follow the crosshair fall off as
    // 1/(1 + excess/grabSlowMass) — half at free+slow, a third at free+2*slow.
    // Nothing above `grabMaxMass` can be picked up at all (0 = no ceiling).
    // One curve for both so a thing that walks you at half speed is also the
    // thing that swings a beat behind where you are looking.
    float grabFreeMass = TPD(player, grabFreeMass);   // kg
    // kg of excess that halves you
    float grabSlowMass = TPD(player, grabSlowMass);
    float grabMaxMass = TPD(player, grabMaxMass);   // kg
    // Floor under the carry speed penalty. Being unable to move while holding
    // something the game let you pick up is a softlock, not a weight.
    float grabMinSpeedScale = TPD(player, grabMinSpeedScale);
    // Servo gain (per second) and its speed cap (m/s, before the weight curve
    // scales it). Gain decides how tightly the body tracks the carry point;
    // the cap is what turns mass into lag. Very high gain with a low cap is a
    // thing that snaps to the crosshair then crawls; the defaults are a
    // followed-with-effort feel.
    float grabStiffness = TPD(player, grabStiffness);
    float grabCarrySpeed = TPD(player, grabCarrySpeed);
    // How far the body may fall behind the carry point before the grab lets
    // go (metres). This is what drops a crate caught on a doorframe instead of
    // dragging it through the frame on the next servo step.
    float grabBreakDistance = TPD(player, grabBreakDistance);
    // Per-tick multiplier on the held body's spin. 1 keeps whatever tumble it
    // had (it windmills, since nothing it touches can slow it while the servo
    // owns its linear velocity); 0 welds it to one orientation.
    float grabSpinDamp = TPD(player, grabSpinDamp);
  } player;

  // ---- camera ----
  struct Camera {
    // radians per pixel
    float mouseSensitivity = TPD(camera, mouseSensitivity);
    float fovY = TPD(camera, fovY);                 // radians (~69 deg)
    float pitchClamp = TPD(camera, pitchClamp);
  } camera;

  // ---- third-person camera rig ----
  //
  // Distances are METERS and converted to voxels at use, exactly like the
  // player block: that is what keeps the framing physically meaningful if
  // kVoxelMeters ever changes. Render-only — the picking ray and every sim
  // input keep using the player's own eye, so nothing here can move the hash.
  struct ThirdPerson {
    // boom length behind the focus point
    float distance = TPD(thirdPerson, distance);
    // boom length in over-shoulder mode
    float shoulderDist = TPD(thirdPerson, shoulderDist);
    // lateral offset, over-shoulder mode
    float shoulderOffset = TPD(thirdPerson, shoulderOffset);
    // focus point above the head anchor
    float heightOffset = TPD(thirdPerson, heightOffset);
    // lateral offset in plain third person
    float sideOffset = TPD(thirdPerson, sideOffset);
    // Collision: the boom is swept against the voxel world and pulled in to
    // the first hit, minus this margin, so the near plane never clips inside
    // a wall. `collideRadius` fattens the sweep so the camera does not slip
    // through a one-voxel gap and pop to the far side.
    float collideMargin = TPD(thirdPerson, collideMargin);
    float collideRadius = TPD(thirdPerson, collideRadius);
    bool collide = TPD(thirdPerson, collide);
    // Smoothing half-lives, seconds. The focus point is smoothed so the
    // camera does not jitter with every step bob; the boom length is smoothed
    // separately and ASYMMETRICALLY — pulling IN must be instant (or the
    // camera spends a frame inside the wall) while pushing back OUT is eased,
    // which is the standard fix for a camera that pops when clearing a corner.
    float focusHalflife = TPD(thirdPerson, focusHalflife);
    // 0 = snap in immediately
    float distInHalflife = TPD(thirdPerson, distInHalflife);
    float distOutHalflife = TPD(thirdPerson, distOutHalflife);
    // Extra pitch-driven lift: at steep downward pitch the boom rises so the
    // character stays framed instead of being hidden by its own hat.
    float pitchLift = TPD(thirdPerson, pitchLift);
    // How strongly the dismemberment state's body drop moves the camera.
    // 1 = follow the pose exactly, 0 = ignore it. Below 1 the camera stays a
    // little higher than a crawling body, which reads better than lying on
    // the floor with it.
    float stateFollow = TPD(thirdPerson, stateFollow);
    // Field-of-view widening with speed, radians at full sprint. Sells speed
    // without the player touching a setting.
    float speedFov = TPD(thirdPerson, speedFov);
    float speedFovHalflife = TPD(thirdPerson, speedFovHalflife);
    // ---- wheel zoom ---------------------------------------------------------
    // The boom length the player can dial for themselves, as a MULTIPLIER on
    // `distance`/`shoulderDist` rather than as its own pair of distances: the
    // two modes then keep their authored relationship (over-shoulder stays
    // tighter than plain third) at every zoom level, and re-tuning either
    // distance does not silently move where the player's zoom sits.
    // multiplier change per wheel notch
    float zoomStep = TPD(thirdPerson, zoomStep);
    // closest, as a fraction of the tuned boom
    float zoomMin = TPD(thirdPerson, zoomMin);
    float zoomMax = TPD(thirdPerson, zoomMax);     // farthest
  } thirdPerson;

  // ---- gear condition ----
  struct Gear {
    // Below this fraction of its authored voxels a worn piece is RUINED — see
    // GearRuined in game/equipment.h. Condition is measured in VOXELS STILL
    // THERE, because that is what the armour mechanic reads: a shell protects
    // by being geometrically in the way, so how much of it is in the way is
    // what its condition means.
    float ruinedCondition = TPD(gear, ruinedCondition);
    // A blade's kerf into a WORN shell is scaled by cutHardnessRef divided by
    // the shell material's hardness (materials.json, 0..255), floored at
    // cutHardnessMin: a shell as hard as skin (8) is cut like flesh, iron
    // (160) is chipped. See Mob::CutLimb. 0 disables the scaling.
    float cutHardnessRef = TPD(gear, cutHardnessRef);
    float cutHardnessMin = TPD(gear, cutHardnessMin);

    // ---- A MACE IS THE ANSWER TO PLATE -------------------------------------
    //
    // The three rows above are what makes a BLADE skate off iron. These are
    // the other half of the same argument, and the reason the owner asked for
    // a mace at all: armour that cannot be answered is not a mechanic, it is a
    // wall. A blunt hit does to a shell what an edge cannot -- it breaks it
    // in, and a share of it arrives on the body underneath regardless.
    //
    // How far a full-power hit at armorBreak 1 breaks INTO a worn shell, in
    // world voxels, before the weapon's own `armorBreak` fraction scales it
    // (a fist 0, a sword 0.05, a mace 0.8). Radial, and it is real geometry:
    // the plate is genuinely gone there, so the flesh under it is exposed to
    // the next blow, to fire and to acid -- "indent/destroy plate (revealing
    // flesh)" in the owner's words, with no armour-value number anywhere.
    float bluntDentRadius = TPD(gear, bluntDentRadius);
    // ...scaled by the shell's own MATERIAL HARDNESS, exactly as the kerf is,
    // against this reference and floored here. 60 rather than the kerf's 8
    // because the whole point is that plate is much LESS proof against trauma
    // than against an edge: iron (160) keeps about 38% of the dent, where it
    // keeps 5% of a kerf.
    float bluntHardnessRef = TPD(gear, bluntHardnessRef);
    float bluntHardnessMin = TPD(gear, bluntHardnessMin);
    // WHAT GETS THROUGH. Fraction of a blunt blow's hp that is TRANSMITTED to
    // the limb the shell is strapped to (MobLimb::wornHost), as trauma with a
    // bruise and no dent. This is the number that says "plate stops swords
    // almost entirely; maces go through": it is charged whether or not the
    // shell broke, because the shell deforming is how the energy arrives.
    float bluntThrough = TPD(gear, bluntThrough);
    // ...and how much of it the shell itself takes as hp. Under 1 because a
    // plate that absorbed the whole blow would be destroyed by the same number
    // of hits that kill the wearer, and then the armour would have no history.
    float bluntShellHp = TPD(gear, bluntShellHp);
    // A BITE ON ARMOUR IS A BLOW, NOT A WOUND. Fraction of a bite's damage
    // that lands as blunt trauma when the teeth meet a worn shell. No
    // shell-breaking on this path: teeth do not dent plate.
    float biteOnShell = TPD(gear, biteOnShell);
    // ---- ...BUT A LINEN SHIRT IS NOT ARMOUR (2026-09-16) -----------------
    //
    // "Armour defends" was applied to every garment equally, so a tunic
    // stopped a zombie's teeth exactly as dead as a cuirass: the whole bite
    // became `biteOnShell` of blunt trauma, the tear never ran and the
    // infection was dropped. A clothed player could be bitten indefinitely and
    // see nothing but bruises, which is what the owner reported.
    //
    // WHAT DECIDES IT IS THE SHELL'S OWN MATERIAL HARDNESS, read off the first
    // voxel of its lattice exactly as the blunt path already reads it for the
    // dent. That is a fact the garment already carries, so this needs no new
    // field on nineteen item files and a garment authored tomorrow answers it
    // for free -- and it is the same "one authoritative source per fact" rule
    // that put the kerf's hardness there.
    //
    // A TWO-POINT RAMP rather than a ratio, because a ratio has no zero: teeth
    // pass entirely at or below `biteThroughSoft` (leather, 14 -- linen is 4
    // and cloth 5, so every woven garment is inside it), nothing passes at or
    // above `biteThroughHard`, and it is linear between. Iron is 160 and steel
    // 200, so plate stops a bite dead and keeps the behaviour the knob above
    // was written for.
    float biteThroughSoft = TPD(gear, biteThroughSoft);
    float biteThroughHard = TPD(gear, biteThroughHard);
    // ---- A SHELL IN A BLAST'S WAY (W2-H, game/shellresponse.h) ----------
    //
    // How many terrain CELLS of its material one crossing of a worn shell
    // counts as, on the ray from an explosion to a voxel of the limb under it.
    // sim_explode.wgsl walks that ray summing the hardness of every cell it
    // samples; a shell is thinner than a cell (one authored micro), and 1 says
    // "the finest thing that stops a ray costs what one sample of it costs",
    // so an iron cuirass (160) takes 160 off a grenade's 380 before the flesh
    // behind it is reached. 0 = shells do not occlude a blast (the pre-W2-H
    // behaviour).
    float blastShellCells = TPD(gear, blastShellCells);
    // ---- A COAT MOVES ON CONTACT (game/coattransfer.cpp, DESIGN.md §7) ----
    // The share of the coat on a striker's touched voxels that leaves them on
    // one landed blow (and of the target's that comes back). 0 = coats never
    // move by contact; 1 = everything touched goes.
    float coatTransferFrac = TPD(gear, coatTransferFrac);
    // ...capped at this many coat levels (the 0..15 scale, summed over
    // voxels) per blow and per direction, so one dip does not empty into one
    // wound.
    int coatTransferMax = TPD(gear, coatTransferMax);
    // The contact patch's reach in METRES: how far past where two surfaces
    // met a voxel still counts as touching. A floor under the weapon's own
    // carve radius (a blade's edge is a few millimetres; its flat is not).
    float coatContactRadius = TPD(gear, coatContactRadius);
    // The level of a wound's own fluid (Mob::WoundFluid) smeared on the
    // striker voxels that went in. A smear, not a dip: it displaces only a
    // weaker coat (phys/coatcontact.h CoatSmear). 0 = blades come out clean.
    int coatBleedPickup = TPD(gear, coatBleedPickup);
    // THE DOSE IS SPREAD (2026-10-01): a transferred parcel is laid no thicker
    // than max(this, an even share of it over the struck patch) per voxel, so
    // one good cut coats a wound's wall rather than soaking two cells. 6 is
    // one venom seeding's worth (`coat.infectCost` 5) with a level to dry.
    int coatLayerMin = TPD(gear, coatLayerMin);
  } gear;

  // ---- player avatar ----
  struct Avatar {
    // Which mob def the avatar rig is loaded from. Data, not code: pointing
    // this at another def in assets/mobs swaps the player character whole.
    // Not hot-reloadable by itself — it is read when the avatar is (re)spawned.
    bool enabled = TPD(avatar, enabled);
    // Body facing. In third person the body turns toward its MOTION and only
    // faces the camera when the player aims, which is what stops the character
    // from moon-walking sideways. This is the turn rate, radians/sec.
    float turnRate = TPD(avatar, turnRate);
    // Below this speed (m/s) the body keeps its last facing instead of
    // snapping to a near-zero velocity vector, which would spin on the spot.
    float turnMinSpeed = TPD(avatar, turnMinSpeed);
    // How far forward (metres) the first-person eye sits from the body centre.
    // Pushes the camera in front of the arms so looking down shows hands behind
    // you rather than surrounding you.
    float firstPersonForward = TPD(avatar, firstPersonForward);
    // Vertical offset applied to the whole avatar relative to the player AABB,
    // in meters. The rig's own feet should land on the box's bottom face; this
    // is the trim for art whose contact point is not exactly at its origin.
    float footTrim = TPD(avatar, footTrim);
    // ---- motion smoothing ----
    // The rig's own measured speed drives cadence, bob, sway, roll, the
    // walk/run clip choice, the spring goals and the swing budget — so any
    // noise in it is amplified into every one of those at once. Half-life in
    // seconds (frame-rate independent): the measurement covers half the
    // remaining distance to the truth every this-many seconds. 0 disables the
    // smoothing entirely and uses the raw per-tick measurement.
    float velocityHalflife = TPD(avatar, velocityHalflife);
    // Half-life (seconds) of the FIRST-PERSON body yaw. Third person has its
    // own rate limit (turnRate above) because you are watching the body pivot;
    // first person used to snap outright, on the reasoning that the body is
    // off-screen — but the ARMS are not, and they are welded to the torso, so
    // a fast mouse turn steps them across the view in hard jumps. A short
    // half-life keeps them attached to the view without visible lag. 0 restores
    // the old hard snap.
    float firstPersonTurnHalflife = TPD(avatar, firstPersonTurnHalflife);
    // Rate limit (radians/sec) on the body yaw while PRONE (crawling on the
    // ground, Mob::LocoGroundAlign), in BOTH camera modes: a body lying on
    // the floor drags itself round slowly instead of pivoting like a
    // standing one. Blended in by how prone the pose is.
    float crawlTurnRate = TPD(avatar, crawlTurnRate);
    // While crawling in first person the VIEW is locked to this many degrees
    // either side of the body, and looking never turns the body; it turns by
    // crawling toward where it is going (ResolveAvatarHeading).
    float crawlLookYaw = TPD(avatar, crawlLookYaw);
    // Half-life (seconds) of the FIRST-PERSON eye following the posed HEAD.
    // The head is posed once per 30 Hz tick; this eases the eye between
    // poses so it tracks the head (a crawl puts it near the floor, out in
    // front of the body) without stepping at the tick rate.
    float firstPersonHeadHalflife = TPD(avatar, firstPersonHeadHalflife);
    // ---- head look ---------------------------------------------------------
    // How far the HEAD may yaw toward the camera away from the body's facing,
    // in degrees (PlayerAvatar::SetLook clamps to it). It does NOT drive the
    // body: since 2026-09-25 the first-person body always faces the view
    // (ResolveAvatarHeading), so this only matters in third person, where the
    // body faces its run and the head looks round at the camera.
    float headLookYaw = TPD(avatar, headLookYaw);
    // THE LOOK LETS GO WHEN THE CAMERA COMES ROUND TO THE FRONT. Width, in
    // degrees measured inward from straight-behind (180), of the band where
    // the head-look goal fades back to the body's own facing. Third person
    // only in practice: first person drags the body so the offset can never
    // leave the cone above, so this band is unreachable there.
    //
    // Without it, orbiting the camera to look the character in the face leaves
    // the neck pinned at its stop craning over one shoulder for the whole
    // front half of the orbit — you can never see the character's actual
    // forward pose. The fade is a smoothstep, so it is flat at both ends: no
    // crease entering the band, and no snap across the 180 wrap (both signs
    // approach zero there). 0 disables it and restores the always-crane.
    float headLookReleaseYaw = TPD(avatar, headLookReleaseYaw);
    // Head pitch range, degrees up/down. The camera pitch clamp is ~89°, and
    // a neck does not do that, so this clamps separately.
    float headLookPitchUp = TPD(avatar, headLookPitchUp);
    float headLookPitchDown = TPD(avatar, headLookPitchDown);
    // Fraction of the head's yaw that is ALSO applied to the spine, so a look
    // to the side twists the chest a little instead of swivelling a head on a
    // rigid torso. Small on purpose: the arms are welded to the spine, so this
    // moves a held weapon across the screen. 0 = head only.
    float headLookSpine = TPD(avatar, headLookSpine);
    // Half-life (seconds) of the head easing to the look angle. This is what
    // keeps the head from stepping with the raw mouse; the body's own
    // firstPersonTurnHalflife sits behind it.
    float headLookHalflife = TPD(avatar, headLookHalflife);
    // Half-life (seconds) of the leg IK fading in and out as the gait starts
    // and stops. `grounded` is genuinely ragged crossing bumpy ground — the
    // body really does leave the surface cresting each bump — and switching the
    // IK on that as a bool snapped the limbs between the IK pose and the rest
    // hang every time, which is what "the arms shoot up straight going uphill"
    // is. Longer is smoother but makes the legs slower to commit to the ground
    // on landing; 0 restores the old hard switch.
    float ikBlendHalflife = TPD(avatar, ikBlendHalflife);
    // How long (seconds) the body must be continuously off the ground before
    // the air-state clips believe it. `grounded` drops false for a tick at a
    // time cresting bumps, and the jump/fall/land clips used to fire on that
    // raw edge — so walking up a noisy incline retriggered the arms-up `jump`
    // one-shot over and over, which is the tweaking. A real jump clears this in
    // one tick; a bump crest never does. Too high and a genuine jump animates
    // late. 0 restores the old undebounced behaviour.
    float airDebounce = TPD(avatar, airDebounce);
    // ---- what counts as a FALL, and how wild it looks -----------------------
    // THE FLAIL IS RAMPED, NOT SWITCHED. `fall` used to be a single wide pose
    // (both arms out in front, both legs raked behind) that started whole the
    // moment airTime passed a threshold — so a step off a kerb played the same
    // arms-out shape as a drop off a cliff, and the character seemed to be
    // permanently falling while walking on rough ground. The clip is authored
    // near-natural now and its WEIGHT ramps with how long the body has been in
    // the air: a short drop never reaches the wide pose at all, and a genuine
    // fall arrives at it over a beat instead of snapping into it.
    //
    // Seconds of air before the flail starts to come in, and seconds it takes
    // to reach full once it does.
    float fallFlailDelay = TPD(avatar, fallFlailDelay);
    float fallFlailRamp = TPD(avatar, fallFlailRamp);
    // ---- THE AIRBORNE POSE IS DRIVEN BY VERTICAL VELOCITY, NOT BY A CLOCK ----
    //
    // `fall` is a 900 ms LOOPING clip whose two keyframes are ten degrees apart,
    // so the whole of being in the air was a slow sway of the arms over a rest
    // hang that the leg IK had just faded out of — "the character just wobbles
    // left and right slightly". A looping clip is the wrong shape for this: a
    // jump has no period to loop, it has a PHASE, and the phase is exactly
    // `vel.y`. Launch, apex, descent and the reach for the ground are four
    // readings of one number, so the pose is interpolated from it directly and
    // driven onto the rig through the same leg/arm IK chains the gait uses.
    //
    // Off restores the clip-driven air pose (`jump` + `fall` + the flail ramp
    // above), which is the A/B arm for judging this.
    bool airPose = TPD(avatar, airPose);
    // The upward speed that reads as a full-power launch and the downward speed
    // that reads as a committed fall, m/s. These NORMALIZE `vel.y` into the
    // pose's phase: at +riseSpeed the body is fully in the tuck, at 0 it is at
    // the apex, at -fallSpeed it is fully in the reach. Default rise is the
    // player's own jumpSpeed, so a jump starts exactly at the tuck; fall is
    // higher than any jump because a drop keeps accelerating past it.
    float airPoseRiseSpeed = TPD(avatar, airPoseRiseSpeed);
    float airPoseFallSpeed = TPD(avatar, airPoseFallSpeed);
    // Height above the ground, in metres, at which the legs start reaching for
    // the landing. This is the part that makes a fall read as a fall rather
    // than as a floating pose: the feet come down and the body tips into the
    // landing BEFORE contact. Probed against the CPU mirror, so a fall the
    // mirror cannot see yet simply keeps the reach pose. 0 disables it.
    float airPoseLandHeight = TPD(avatar, airPoseLandHeight);
    // How far the body leans into its own horizontal travel while airborne,
    // degrees at the def's top speed. A running jump tips forward; a standing
    // one does not. Half of it is taken by the pelvis and half by the spine
    // above it, so the back curves rather than tilting as a plank.
    float airPoseLean = TPD(avatar, airPoseLean);
    // Meters the body must have DROPPED below the height it last had support at
    // before the fall clip may play at all. Air time alone is not a fall: a
    // step-down clears any debounce, and so does cresting a bump at speed.
    // Distance is the honest question, and it is the one a player would answer.
    float fallMinDrop = TPD(avatar, fallMinDrop);
    // How long the corpse's parts stay before the avatar can respawn, seconds.
    float respawnDelay = TPD(avatar, respawnDelay);
    // ---- WHAT YOUR ZOMBIE TAKES WITH IT -----------------------------------
    //
    // The avatar turns like anybody else: die with the rot in you and your own
    // corpse gets up as a zombie of you (MobDef::Turn, inherited from
    // human.json by every character). It rises in the gear it fell in — and
    // the question this answers is whether you STILL HAVE THAT GEAR.
    //
    // true  — it rises with a COPY and you respawn with your kit intact. Two
    //         swords now exist where there was one. The default, because
    //         losing your kit to a test bite costs more than a duplicate
    //         does while the mechanic is being played with.
    // false — the kit MOVES. Bag, hotbar and equipment are emptied at the
    //         moment of death and everything is on the thing wearing your
    //         face. No duplication, and the death penalty this becomes.
    //
    // CPU-only, read at the one seam (MobSystem::ServiceRising, where a
    // rising takes the owning avatar's Kit). Nothing here reaches a shader or
    // the CA.
    bool keepKitOnTurn = TPD(avatar, keepKitOnTurn);
  } avatar;

  // ---- sound ----
  // CPU-only: nothing here reaches a shader, so there is no TUNE_* emitter and
  // no entry in scripts/tuning_prelude.py. Read through CurrentTuning() on the
  // GAME thread and copied into the audio layer once per frame — the audio
  // thread must never touch this struct, since F5 replaces it wholesale
  // (see src/audio/voice.h for the threading contract).
  struct Audio {
    bool enabled = TPD(audio, enabled);
    float masterVolume = TPD(audio, masterVolume);

    // Footsteps. `volume` is the overall trim; per-material trims multiply it.
    float footstepVolume = TPD(audio, footstepVolume);
    // Audible radius in meters — the distance at which a step falls to the
    // gain floor. Steps are small sounds; a big radius makes them carry
    // unnaturally and wastes voices on inaudible ones.
    float footstepRadius = TPD(audio, footstepRadius);
    // Step pitch is randomized per trigger to hide sample repetition. This is
    // the half-range: 0.06 means each step lands in [0.94, 1.06] of natural
    // rate. Too much and the surface changes identity step to step.
    float footstepPitchJitter = TPD(audio, footstepPitchJitter);
    // Loudness at walking pace vs at sprint. Speed maps between them, so a
    // sneak is quiet and a sprint is not merely faster but heavier.
    float footstepWalkGain = TPD(audio, footstepWalkGain);
    float footstepSprintGain = TPD(audio, footstepSprintGain);
    // Speed (m/s) that counts as a full sprint for the mapping above.
    float footstepSprintSpeed = TPD(audio, footstepSprintSpeed);
    // Left and right feet are pitched apart by this fraction so a gait reads
    // as two feet rather than one repeated impact.
    float footstepFootDetune = TPD(audio, footstepFootDetune);

    // Landing after a fall: gain scales with impact speed up to this speed
    // (m/s), which also caps the pitch drop.
    float landVolume = TPD(audio, landVolume);
    float landFullSpeed = TPD(audio, landFullSpeed);

    // Physical impacts (debris, bodies).
    float impactVolume = TPD(audio, impactVolume);
    float impactRadius = TPD(audio, impactRadius);
    // THE GATE. Contact speed (m/s) below which a landing is not a sound at
    // all. This is the knob that makes a settling pile silent instead of a
    // machine gun, and it is enforced inside the Jolt contact listener, so
    // raising it is genuinely free — rejected contacts never become events.
    // A rock rolling to rest touches down at centimetres per second; a rock
    // that FELL arrives at several m/s, and the gap between them is wide.
    float impactMinSpeed = TPD(audio, impactMinSpeed);
    // Contact speed (m/s) that counts as a full-energy impact: gain tops out
    // and pitch bottoms out here. Held above impactMinSpeed by the consumer.
    float impactFullSpeed = TPD(audio, impactFullSpeed);
    // Minimum seconds between two impacts from the SAME body. A tumbling rock
    // generates a contact per bounce and per face; without this one fall is a
    // clatter of six identical thuds.
    float impactMinGap = TPD(audio, impactMinGap);

    // Something coming apart: terrain losing support and detaching as a
    // rigidbody, or being dug/blasted loose.
    float breakVolume = TPD(audio, breakVolume);
    // Breaks carry further than impacts — a tree limb giving way is a loud,
    // low event and hearing it from off-screen is most of its value.
    float breakRadius = TPD(audio, breakRadius);
    // Half-range of the per-event random detune, in SEMITONES. Expressed in
    // semitones rather than as a rate multiplier because that is the unit the
    // ear (and whoever is tuning this) actually thinks in; cues.cpp converts
    // with 2^(n/12). 5 is wide — deliberately, since one support scan can free
    // several islands in the same tick and identical repeats read as a
    // machine. Beyond ~7 the material stops sounding like itself.
    float breakPitchSemitones = TPD(audio, breakPitchSemitones);
    // Pitch centre by piece size: a lone voxel snapping is a twig, a large
    // island is a log. These are the rate multipliers at the two ends, and a
    // piece's voxel count maps between them (see breakBigVoxels).
    float breakSmallRate = TPD(audio, breakSmallRate);
    float breakBigRate = TPD(audio, breakBigRate);
    // Voxel count that counts as "big" for the mapping above.
    float breakBigVoxels = TPD(audio, breakBigVoxels);
    // Creature voices (hurt/death/sever/...). Kept separate from impacts
    // because a mob crying out and a rock landing are mixed against each
    // other, and one trim cannot serve both.
    float mobVolume = TPD(audio, mobVolume);
    float mobRadius = TPD(audio, mobRadius);
    float mobPitchJitter = TPD(audio, mobPitchJitter);

    // The blade cut itself, separate from the creature's cry (which is the
    // `sever` slot on mobVolume). Its own trim because a wet mechanical sound
    // and a voice sit differently in the mix, and a wider jitter because four
    // takes have to cover a whole fight.
    float dismemberVolume = TPD(audio, dismemberVolume);
    float dismemberPitchJitter = TPD(audio, dismemberPitchJitter);

    // Bleeding: a positioned wet loop while a wound is pumping hard.
    float bleedVolume = TPD(audio, bleedVolume);
    float bleedRadius = TPD(audio, bleedRadius);
    // Intensity (0..1 of the bleed budget cap) to START the loop, and the
    // lower level it must fall back through to STOP. Two thresholds, not one:
    // a wound sitting exactly on a single threshold retriggers the voice every
    // frame. On > off is required and enforced at load.
    float bleedOnThreshold = TPD(audio, bleedOnThreshold);
    float bleedOffThreshold = TPD(audio, bleedOffThreshold);

    // The automatic material ambience bed: one positioned loop following the
    // largest nearby body of a material that binds an "ambience" set (water,
    // lava). Unlike the night bed this one IS a thing at a place — it pans and
    // it occludes — so the radius is what decides how far a lake carries.
    float ambienceVolume = TPD(audio, ambienceVolume);
    float ambienceRadius = TPD(audio, ambienceRadius);

    // The night bed (assets/sounds/ambience/starlight). Rare by design.
    float nightVolume = TPD(audio, nightVolume);
    // Audible radius. Large because the bed is centred on the listener and is
    // meant to sit around them rather than to come from a place.
    float nightRadius = TPD(audio, nightRadius);
    // Chance, per retry, that a new pass begins. With the defaults below that
    // is one roll a minute at 8%, so most nights stay silent and the bed is an
    // event rather than a backing track.
    float nightChance = TPD(audio, nightChance);
    float nightRetrySeconds = TPD(audio, nightRetrySeconds);
    // Per-frame easing factor for the fade in and out. Small = slow: the bed
    // should arrive and leave without the player catching either moment.
    float nightFadeRate = TPD(audio, nightFadeRate);

    // Reverb send for world sounds, 0..1. The engine's FDN reverb is what
    // makes a cave read as a cave; keep it modest for outdoor-heavy worlds.
    float reverbWet = TPD(audio, reverbWet);

    // ---- occlusion ----
    // See src/audio/occlusion.h for the model. These are the knobs that decide
    // how much a wall between you and a sound matters.
    bool occlusion = TPD(audio, occlusion);
    // cap on the broadband duck
    float occlusionMaxDb = TPD(audio, occlusionMaxDb);
    // fully-muffled low-pass floor
    float occlusionMinCutoffHz = TPD(audio, occlusionMinCutoffHz);
    // multiplies the accumulated dB
    float occlusionScale = TPD(audio, occlusionScale);
    // <1 = darker through walls
    float occlusionCutoffScale = TPD(audio, occlusionCutoffScale);
    // never trace a ray longer than this
    float occlusionMaxRangeM = TPD(audio, occlusionMaxRangeM);
    // How much of the reverb send survives an occluded path. High values keep
    // a blocked sound present-but-muffled instead of switching it off.
    float occlusionWetKeep = TPD(audio, occlusionWetKeep);
  } audio;

  // ---- Jolt rigid bodies ----
  struct Physics {
    float gravity = TPD(physics, gravity);
    int collisionSteps = TPD(physics, collisionSteps);
    float debrisFriction = TPD(physics, debrisFriction);
    float debrisRestitution = TPD(physics, debrisRestitution);
    float debrisLinearDamping = TPD(physics, debrisLinearDamping);
    float debrisAngularDamping = TPD(physics, debrisAngularDamping);
    // Drag a rigidbody feels from the LIQUID it is in, as opposed to the air
    // damping above (docs/PLAN_debris_buoyancy.md phase 3). Jolt's own
    // buoyancy coefficients: linear is a quadratic drag against the submerged
    // frontal area, angular damps the tumble. The pair is what makes a floating
    // log stop wallowing and go to sleep instead of bobbing forever.
    float waterLinearDrag = TPD(physics, waterLinearDrag);
    float waterAngularDrag = TPD(physics, waterAngularDrag);
    float terrainFriction = TPD(physics, terrainFriction);
    float playerProxyFriction = TPD(physics, playerProxyFriction);
    float explosionImpulseScale = TPD(physics, explosionImpulseScale);
    float explosionImpulseRadiusScale =
        TPD(physics, explosionImpulseRadiusScale);
    // The fastest the per-body blast impulse may make any ONE body go, m/s.
    // impulse / mass is unbounded from below in mass: a 0.05 kg gobbet carved
    // off a creature by the same explosion took 1000 m/s (Jolt's own ceiling
    // is 500) and, born inside the limb it came from, rammed the rig it had
    // just left — "bodies zoom across the map" when a blast was big enough to
    // carve. Ordinary debris (a 2.5 kg stone voxel takes 20 m/s from the
    // X-detonate charge) never reaches this.
    float explosionMaxSpeed = TPD(physics, explosionMaxSpeed);
    // How far an explosion actually BLOWS VOXELS OFF bodies, as a multiple of
    // the destruction radius. Kept separate from the impulse reach on purpose:
    // the blast should push objects from further away than it dismembers them,
    // so this is normally the smaller of the two.
    float explosionBodyDamageScale = TPD(physics, explosionBodyDamageScale);
    // Player proxy mass: the shove-strength knob. Contact impulses split by
    // mass ratio, so this vs a body's density-derived mass decides how far a
    // walking player moves it.
    float playerMassKg = TPD(physics, playerMassKg);
    // ---- A LIVING CREATURE MAY LEAN INTO YOU (PlayerPushOut) --------------
    //
    // How deep a LIVE rig's limb may sit inside the player's capsule before it
    // displaces the player at all, and the most it may displace them in one
    // tick once it is deeper than that.
    //
    // WHY THIS EXISTS. A creature's posed limbs are KINEMATIC bodies on the
    // ordinary MOVING layer reporting their whole rig's mass, so every one of
    // them sails past the kick-it-aside mass gate and depenetrates the proxy
    // by its full overlap, every tick, for as long as it overlaps. Nothing
    // holds an NPC out of you either — `CrowdPush` and `BlockedByMob` iterate
    // `mobs_`, and the player is an `ai::Actor`, not a Mob — so a creature
    // whose band floor is 2 voxels walks into your volume and then bulldozes
    // you out of it. Reported as "the zombies push my body around", and it is
    // also why they could not bite: the standing `bite`'s derived reach is
    // about 2 voxels (MobSystem::StyleReachOn), which is nearly contact, and
    // the victim was being shoved out of contact on the very tick the teeth
    // arrived.
    //
    // ONLY KINEMATIC BODIES, and that is the discriminator rather than a new
    // flag: a live posed rig is the only thing kinematic on MOVING. A corpse
    // or a ragdoll is dynamic (the solver pushes it out and the mass gate
    // already covers it), a held weapon is on PROP, a severed limb mid-hold is
    // on AVATAR. So this softens a creature LEANING on you and changes nothing
    // about being crushed by a log.
    //
    // THE SLACK IS THE FEATURE AND THE CAP IS THE NET. The slack is what lets
    // teeth reach: a head may come a voxel and a half inside you for free. The
    // cap stops the remainder ever being a launch — past the slack you are
    // eased out at a bounded rate instead of teleported a body-width, which is
    // what the full depenetration was worth at 30 Hz (~36 m/s).
    //
    // Zero slack with a huge cap is the old behaviour exactly.
    float creaturePhaseVox = TPD(physics, creaturePhaseVox);
    float creaturePushMaxVox = TPD(physics, creaturePushMaxVox);
    // Rolling spheres (analytic colliders, not boxed voxels).
    float sphereFriction = TPD(physics, sphereFriction);
    float sphereRestitution = TPD(physics, sphereRestitution);
    float sphereAngularDamping = TPD(physics, sphereAngularDamping);
  } physics;

  // ---- live ragdoll: a creature goes limp and gets back up (game/mob.h) ----
  // CPU-only floats, never in a shader: a ragdoll is Jolt presentation state
  // and re-enters the grid only through the ordinary body paths. Metres and
  // seconds, converted at the point of use.
  struct Ragdoll {
    // Continuous freefall before a creature goes limp mid-air. NPCs fall under
    // the same gravity as the player since this landed (they used to hang).
    float fallSeconds = TPD(ragdoll, fallSeconds);
    // A blast within the ONE push reach (radius x
    // physics.explosionImpulseRadiusScale -- the same reach the per-body
    // debris impulse uses; the separate `blastRadiusScale` that held the same
    // 3.0 was retired by W2-H) launches a creature. The impulse at the centre
    // is power * blastImpulseScale (kg*m/s) for the WHOLE creature, falling
    // off linearly to zero at that reach; launch speed is impulse / body mass,
    // so a heavy creature flies less far than a light one from the same
    // charge, and a grenade (power 380) sends ~70 kg about 5 m/s. Read only
    // through BlastForceOf (game/session.h).
    float blastImpulseScale = TPD(ragdoll, blastImpulseScale);
    // Below this launch speed (m/s) a blast does not knock the creature down
    // at all; above maxLaunchSpeed it is clamped, which is what keeps a large
    // charge from putting a body into orbit. "Across the room, not across
    // the map" — a massive explosion still tops out here.
    float blastMinSpeed = TPD(ragdoll, blastMinSpeed);
    float maxLaunchSpeed = TPD(ragdoll, maxLaunchSpeed);
    // Fraction of straight-up mixed into the launch direction, so a body on
    // the floor beside a blast arcs rather than skidding along the ground.
    float blastUpBias = TPD(ragdoll, blastUpBias);
    // ---- THE TUMBLE (Mob::BlastRadial) -------------------------------------
    // How much a limb's OWN distance to the charge varies the shove it takes,
    // as a fraction: 0 is the flat launch every limb used to get (a body that
    // floats away from the blast facing the same way it stood), 1 would scale
    // each limb by its own falloff over the rig's mean. The differential is
    // deliberately small — enough that a blast at the ankles clearly lifts the
    // legs before the head, not enough to tear a rig apart.
    float blastLimbBias = TPD(ragdoll, blastLimbBias);
    // The per-limb differential is reduced to ONE rigid motion — a launch
    // velocity at the rig's centre of mass plus a spin about it — so no
    // constraint is violated and the joints do no launching (see
    // Mob::BlastRadial). The spin comes from the angular impulse over a
    // POINT-MASS inertia (each limb's own spin inertia is ignored, which
    // overstates it), so this gain corrects for that and is the dial for how
    // hard a body tumbles. The cap is the "not a helicopter" rule.
    float blastSpinGain = TPD(ragdoll, blastSpinGain);
    float blastMaxSpin = TPD(ragdoll, blastMaxSpin);  // rad/s
    // Shortest time a creature stays limp before it may start getting up,
    // and the stillness test that then lets it: the pelvis has moved slower
    // than settleSpeed (m/s) for settleSeconds. maxSeconds is the ceiling
    // for a body that never settles (wedged, twitching on a slope).
    float minSeconds = TPD(ragdoll, minSeconds);
    float settleSpeed = TPD(ragdoll, settleSpeed);
    float settleSeconds = TPD(ragdoll, settleSeconds);
    float maxSeconds = TPD(ragdoll, maxSeconds);
    // The procedural get-up: every limb blends from where it landed into a
    // crouched pose (torso pitched getUpPitchDeg forward about the feet, hips
    // dropped getUpDropFrac of the standing hip height), which then rises to
    // the ordinary standing pose over getUpSeconds in total.
    float getUpSeconds = TPD(ragdoll, getUpSeconds);
    float getUpPitchDeg = TPD(ragdoll, getUpPitchDeg);
    float getUpDropFrac = TPD(ragdoll, getUpDropFrac);
    // How long the dev panel's "ragdoll me" keeps the player down.
    float devSeconds = TPD(ragdoll, devSeconds);
  } ragdoll;

  // ---- debris / island -> rigidbody conversion ----
  // Read by phys/debris.cpp through its MinBodyVoxels()/... accessors. These
  // were debris.cpp constexprs the tuning pipeline exposed on day one
  // (46993d8) and never wired; they drive it since 2026-09-24. They decide
  // what becomes a body and when a body re-enters the grid, so they feed the
  // world hash.
  struct Debris {
    // an island smaller than this crumbles to rubble instead of a body
    int minBodyVoxels = TPD(debris, minBodyVoxels);
    // the (higher) floor for a fragment that breaks off a BURNING body
    int minBurnFragmentVoxels = TPD(debris, minBurnFragmentVoxels);
    // new bodies a shatter/split may create per tick, across all bodies
    int maxNewBodiesPerTick = TPD(debris, maxNewBodiesPerTick);
    // ticks a body must sleep before it may settle back into the grid
    int settleAfterTicks = TPD(debris, settleAfterTicks);
    // how axis-aligned (cos of the worst axis) a body must be to settle back
    float alignCos = TPD(debris, alignCos);
    // global body ceiling, oldest evicted first; at most debris.h kMaxBodies
    int maxBodies = TPD(debris, maxBodies);
    // fire/ash grid writes all burning bodies may emit per tick
    int burnOpsPerTick = TPD(debris, burnOpsPerTick);
  } debris;

  // ---- gore ------------------------------------------------------------------
  //
  // TWO SIZES OF MATTER come off a wound, and almost every confusion in this
  // group came from the two being interleaved. They are now separated, and the
  // separation is the organising idea of the whole struct:
  //
  //   MICRO SPRAY  — sub-voxel droplets (`micro*`, `*Spray*`). They fly, they
  //                  NEVER re-enter the grid as matter, they stain the first
  //                  surface they hit and then they expire. Cost is particle
  //                  slots and nothing else; a droplet cannot pool, flow, or
  //                  keep a chunk awake. This is what SELLS the hit.
  //   WHOLE VOXELS — real blood cells the CA owns (`bleed*`, `sever*Voxel*`,
  //                  `clump*`). They fall, pool, flow, soak, stain from below
  //                  and are still on the floor a minute later. Every one is
  //                  conserved matter the sim has to move, so these are perf
  //                  knobs as much as look knobs. This is the LASTING MESS.
  //
  // Within each size, the parameters split again by OCCASION: a slow bleed from
  // an open wound, versus the one-shot burst when a limb comes off. So the
  // struct reads as a 2x2 — spray/voxels x bleed/sever — plus the shared micro
  // droplet properties and the per-instance variance block.
  //
  // These drive CPU-authored ParticleSpawns and BrushOps (mob.cpp, avatar.cpp),
  // not shader constants, so they are floats and do NOT change the world hash
  // by themselves. What the spawns do once they land IS sim state — micro
  // droplets stain — but the stain is authored in materials.json, and the spawn
  // stream is a per-tick INPUT exactly like a BrushOp. Retuning these changes
  // future worlds, the same way moving the mouse does; it does not make a
  // replay diverge.
  struct Gore {
    // ========================================================================
    // A. MICRO SPRAY — sub-voxel droplets. Stain and expire; never become matter.
    // ========================================================================

    // ---- A1. shared droplet properties (both occasions) ----
    // Lifetime in ticks, and how finely a droplet is subdivided (2/3/4/6 micro
    // voxels per world voxel). Life is the guarantee that spray CLEARS: no
    // droplet outlives it, whether or not it ever hits anything.
    int microLifeTicks = TPD(gore, microLifeTicks);
    int microScale = TPD(gore, microScale);

    // ---- A2. spray from an open wound (the drip's companion) ----
    // Droplets per whole blood voxel a wound drips. Bleeding already drips real
    // voxels into the grid; this is the visible spray that accompanies each
    // drip, so it multiplies an existing, already-bounded rate.
    float bleedSprayPerDrip = TPD(gore, bleedSprayPerDrip);
    // voxels/sec, upward-biased cone
    float bleedSpraySpeed = TPD(gore, bleedSpraySpeed);
    // lateral spread as a fraction of speed
    float bleedSprayCone = TPD(gore, bleedSprayCone);

    // ---- A3. spray from a dismemberment (the arterial gout) ----
    // `severSpray` droplets are emitted over `severDecayTicks`, front-loaded so
    // the gout is at the cut and the tail dies down — a flat rate over the same
    // window reads as a sprinkler rather than a wound.
    int severSpray = TPD(gore, severSpray);
    int severDecayTicks = TPD(gore, severDecayTicks);
    float severSpraySpeed = TPD(gore, severSpraySpeed);
    float severSprayCone = TPD(gore, severSprayCone);

    // ========================================================================
    // B. WHOLE-VOXEL BLOOD — real matter the CA carries. Pools, flows, persists.
    // ========================================================================

    // ---- B1. how much blood a wound is worth, and its ceiling ----
    // A wound carries a BUDGET in whole voxels and drips it out over time.
    // `bleedVoxelGain` multiplies the per-mob rate authored in the mob's own
    // .json (bleed.perDamage), so it is the global "how wet is this game" dial:
    // it decides the size of the puddle still on the floor a minute later. The
    // cap is what stops a huge hit turning into a minute-long fountain, and is
    // the real bound on how much matter one wound can push into the CA.
    float bleedVoxelGain = TPD(gore, bleedVoxelGain);
    // max voxels one wound can still owe
    float bleedBudgetCap = TPD(gore, bleedBudgetCap);
    // Voxels added to the stump's budget when a limb comes off, on top of the
    // thrown sever voxels below. This is the puddle under a fresh amputation.
    float severStumpBudget = TPD(gore, severStumpBudget);
    // A CORPSE BLEEDS FROM WHERE IT IS CUT. Every debris body that was once
    // flesh carries a wound of its own (DebrisSystem::BodyWound): the neck
    // stump on the torso and the head that came off it each drip from their
    // own place. Blood voxels a corpse owes per WORLD voxel carved off it,
    // through the same cap as a live wound; a cut that takes a piece off also
    // arms the sever gout and the stump budget above on BOTH pieces, so a
    // dismembered corpse bleeds like a dismembered creature, minus the hp.
    float corpseBleedPerVoxel = TPD(gore, corpseBleedPerVoxel);
    // ---- WHAT STILL HOLDS A CORPSE TOGETHER (2026-09-20) -------------------
    //
    // A corpse is a dozen bodies held by the joints Mob::Die leaves on, and
    // until now nothing could cut one: a blade could part a neck completely
    // and the head stayed attached, because severing on the debris side is
    // connectivity INSIDE one body. This is the living rule's radius, one
    // population later (Mob::CutLimb's "the flesh AT the joint is gone"): if
    // no voxel survives within this many WORLD voxels of a joint's anchor,
    // the joint has nothing left to hold and lets go. 0 disables corpse
    // dismemberment entirely and puts the heads back on.
    float corpseJointHold = TPD(gore, corpseJointHold);
    // ...AND HOW LITTLE OF IT IS TOO LITTLE. A binary "no voxel at all within
    // the radius" test is the trap phys/kerf.h already names in another form:
    // ONE surviving straggler keeps a head on forever. Measured — 60 chops at
    // a corpse's neck took the flesh holding that joint from 476 voxels to 4
    // and the head stayed attached, because 4 is not 0. So the rule is the
    // living one's shape: a FRACTION of what was there when the blows started
    // (Mob::CarveLimb measures its neck against `neckAtSpawn` for exactly this
    // reason). 1.0 parts a joint the moment anything is taken; 0 restores the
    // all-or-nothing rule and puts the heads back on.
    float corpseJointCut = TPD(gore, corpseJointCut);

    // ---- B2. how fast that budget leaves the wound, and in what size lumps ----
    // Rate is a PERIOD, not a chance, because bleeding must stay bounded per
    // rule 2: a wound drips at most once every `bleedDripTicks`, and at most
    // `bleedOpsPerTick` drips happen across all limbs of all mobs in a tick.
    // ticks between drips from one wound
    int bleedDripTicks = TPD(gore, bleedDripTicks);
    // global op budget for drips, per tick
    int bleedOpsPerTick = TPD(gore, bleedOpsPerTick);
    // CLUMP SIZE: the brush radius of one drip, so a drip can be a single bead
    // or a thick gout. A BrushOp paints a solid sphere (sim_mutate.wgsl tests
    // dot(local,local) <= radius^2), so this is a VOLUME dial, not a width one:
    //   radius 0 -> 1 voxel   1 -> 7   2 -> 33   3 -> 123
    // Radius 3 is the ceiling on purpose. The op's thread box is 16^3 centred
    // on the cell (max brush radius 7), so the shader allows more, but a drip
    // is a repeating source: at radius 4 (257 voxels) a single wound outruns
    // what the CA can settle between drips and the chunk never sleeps.
    //
    // The budget is debited by the SPHERE VOLUME this radius paints, not by 1
    // (see BleedClumpVoxels below). Otherwise raising clump size multiplies the
    // matter entering the world while `bleedBudgetCap` reports the same number,
    // and rule 2's bound quietly becomes a 123x underestimate.
    int bleedClumpRadius = TPD(gore, bleedClumpRadius);
    // Whole blood VOXELS thrown by a cut, alongside the sub-voxel gout. Kept
    // small next to the hundreds of micro droplets — the spray does the visual
    // work, these do the lasting mess.
    int severVoxels = TPD(gore, severVoxels);
    float severVoxelSpeed = TPD(gore, severVoxelSpeed);
    // GOBBET SIZE: how many thrown voxels travel together as one lump.
    //
    // This is NOT a brush radius, and the difference is forced by the engine
    // rather than chosen. A thrown voxel is a ballistic PARTICLE, and
    // sim_particle.wgsl deposits exactly one cell per particle, arbitrated by
    // the claim lattice — a particle cannot paint a sphere without writing
    // several cells from one thread, which breaks both the <=1-cell write reach
    // and the claim arbitration (rule 1). So a clump is expressed the way the
    // particle system CAN express it: `severGobbetVoxels` particles launched
    // from the same point with the same velocity, landing as a contiguous lump
    // instead of a fine mist of single cells.
    //
    // severVoxels stays the TOTAL voxel count, so this subdivides the throw
    // rather than multiplying it: 14 voxels at gobbet 1 is fourteen scattered
    // cells, at gobbet 7 it is two fat gouts. Matter thrown is unchanged, which
    // is what keeps this a look knob and not a perf knob.
    int severGobbetVoxels = TPD(gore, severGobbetVoxels);
    // How far apart a gobbet's members are spread at launch, in voxels. Zero
    // stacks them on one cell, where the claim lattice lets exactly one win and
    // the rest retry next tick — a slow-motion drip instead of a lump. A small
    // jitter gives them distinct target cells so they land together.
    float severGobbetSpread = TPD(gore, severGobbetSpread);

    // ========================================================================
    // C. PER-INSTANCE VARIANCE
    // ========================================================================
    // Each of these perturbs the like-named value above. Defaults are all
    // dist=kNone, so gore behaves exactly as before until a knob is turned on
    // in the tuner — this whole feature is opt-in and inert at rest.
    //
    // The interesting ones are entity-scoped: bleedSprayPerDrip and
    // severSpray/severVoxels rolled per mob are what make ONE npc a gusher for
    // its whole life rather than making every wound flicker.
    Variance bleedSprayPerDripVar, bleedSpraySpeedVar, bleedSprayConeVar;
    Variance severSprayVar, severSpraySpeedVar, severSprayConeVar;
    Variance severVoxelsVar, severVoxelSpeedVar, severDecayTicksVar;
    Variance microLifeTicksVar;
    // Whole-wound gain: multiplies every blood quantity for one mob at once
    // (spray, sever spray, sever voxels). This is the single knob for "rare
    // NPC bleeds an extreme amount" — varying the individual counts
    // independently gives a mob that gushes spray but throws normal voxels,
    // which reads as a bug rather than as a heavy bleeder. Centre is 1.0.
    //
    // NOTE it scales COUNTS, not the whole-voxel budget: bleedVoxelGain is the
    // volume dial and is deliberately not per-instance, because a wound budget
    // that varies per mob makes the bleedBudgetCap bound unreadable.
    float bleedGain = TPD(gore, bleedGain);
    Variance bleedGainVar;

    // ========================================================================
    // D. CRATER SHAPE — what a blast takes off a body, and in what size pieces
    // ========================================================================
    // The old crater was per-voxel WHITE NOISE against a `1 - t^2` radial
    // falloff. Both halves work against concentration: `1 - t^2` is still 0.75
    // at half the radius and 0.36 at 80% of it, so a large blast genuinely does
    // sprinkle the whole body, and an independent coin flip per voxel has no
    // feature size at all — what comes off is a fine speckle rather than a
    // piece. That is the reported "thin scatter of voxels spread over the whole
    // body".
    //
    // These four turn that into a torn chunk. carveChunkiness is the master
    // slider and 0 reproduces the old behaviour EXACTLY (the noise lerps back
    // to the same Hash3 draw, the falloff exponent lerps back to 1, and the
    // spall pass is skipped) — that identity is asserted by the mob gate, so
    // the knob is a genuine A/B rather than an approximation of one.
    float carveChunkiness = TPD(gore, carveChunkiness);
    // Feature size of the correlated noise, in SKIN voxels. This is the size of
    // the lumps that come off. Kept on the skin lattice for the same reason the
    // rim jitter already is: the crater's shape must be a property of the ART,
    // not of whichever collider resolution the engine happened to derive, or
    // the same blast tears differently on two rigs that differ only in scale.
    float carveBlobSize = TPD(gore, carveBlobSize);
    // Exponent on the radial falloff at full chunkiness. Higher concentrates
    // the removal at the blast: at 3, the chance is 0.42 at half the radius and
    // 0.047 at 80% of it, against 0.75 and 0.36 before.
    float carveFalloff = TPD(gore, carveFalloff);
    // How many SPALL rounds run after the radial pass. Each round takes
    // surviving voxels that are inside the blast and already have enough
    // missing face-neighbours — so a hole grows into its own rim instead of a
    // second blast having to find fresh voxels. This is what makes damage
    // accumulate in one place, and what makes a blast beside an arm take the
    // arm. Bounded and small: each round is one pass over the limb's voxels.
    int carveSpallRounds = TPD(gore, carveSpallRounds);

    // ========================================================================
    // E. THE WOUND MODEL — what a BLADE does, as opposed to a blast
    // ========================================================================
    // A blast is a sphere and a blade is a SLOT. The distinction is the whole
    // feature: severing used to be an EVENT (three thresholds in Mob::Damage
    // fired a Sever() the instant a sword touched anything), and it is now a
    // CONSEQUENCE — the limb comes off when its lattice has genuinely been cut
    // through. Every number below therefore describes GEOMETRY, and the only
    // thing that decides dismemberment is what is left of the limb.
    //
    // All lengths are WORLD VOXELS, so they mean the same thing on a
    // skinScale-8 human and a scale-1 critter (game/mob.h Mob::CutLimb
    // converts into whichever lattice it is testing).

    // ---- E1. the kerf: how deep the edge bites, and how wide a slot ----------
    // Depth at zero commitment, and the extra at a full-speed cut. Total depth
    // is (cutDepth + cutDepthPower * power) * heft, where `power` is the swing's
    // 0..1 speed fraction and `heft` is the weapon's own volume against
    // woundHeftRef below. A knife at a lazy wave takes a chip; a greatsword at
    // full swing opens a gash three times as deep, and that ratio IS the
    // "a big enough sword dismembers in one or two blows" rule — no separate
    // chance roll decides it.
    float cutDepth = TPD(gore, cutDepth);
    float cutDepthPower = TPD(gore, cutDepthPower);
    // Half-length of the slot ALONG the edge, at full power. A cut is a slice,
    // not a hole: this is what makes it read as an edge passing through rather
    // than as a bite. Scaled by 0.4 + 0.6 * power so a graze is short.
    float cutLength = TPD(gore, cutLength);
    // Kerf half-thickness as a MULTIPLE of the blade's own authored
    // edgeHalfWidth (item.h). Below 1 because the authored half-width is the
    // widest part of the blade and the edge itself is thinner; the taper toward
    // the bottom of the cut is applied on top of this.
    float cutWidth = TPD(gore, cutWidth);
    // Spall applied to a cut, separately from the blast's carveSpallRounds. A
    // cut wants a LITTLE of it — enough that the second blow into the same
    // gash widens it instead of stippling fresh flesh beside it (that is the
    // "sustained hits dismember" mechanism), and not so much that one swing
    // tears an arm off. Zero makes every cut a clean bore.
    int cutSpallRounds = TPD(gore, cutSpallRounds);
    float cutSpallStrength = TPD(gore, cutSpallStrength);
    // ---- E1b. the cleave (phys/kerf.h KerfBite, 2026-09-25) ----------------
    // THE CLEAVE: a committed blow's bite past the ordinary chip, in world
    // voxels^2 of skin-equivalent cross-section at heft 1 and power 1
    // (KerfBite). A blow whose bite covers everything left in the plane it is
    // cutting -- within its edge's reach, bone at three times skin -- comes
    // out the other side and parts the limb. Scaled by heft and by
    // ((power - cleaveFrom) / (1 - cleaveFrom))^2, so only a fast, square,
    // heavy blow has any, and a neck already notched costs less to finish.
    float cleaveArea = TPD(gore, cleaveArea);
    float cleaveFrom = TPD(gore, cleaveFrom);
    // The kerf's half-thickness never drops below this, world voxels. A skin
    // cell is 1/skinScale of a voxel (0.125 on the human), and a slot thinner
    // than about one cell falls between cell centres and takes nothing.
    float cutWidthMin = TPD(gore, cutWidthMin);

    // ---- E1c. the stab (2026-09-26) -----------------------------------------
    // A blade driven ALONG ITS OWN LENGTH is a stab, not a cut: the wound is a
    // bore from the entry point down the direction of travel, a slit as wide
    // as the blade and as thick as its edge, narrowing toward the point (the
    // kerf's wedge). melee.cpp BuildStrikeParts decides which a blow is:
    // stabAlign is the cosine between the blade (hilt -> point) and its
    // travel above which the blow is a stab; above 1 turns stabs off.
    //
    // Depth = stabDepth + stabDepthPower * power, world voxels, capped at the
    // blade's own edge length. NO HEFT, unlike a cut: a point concentrates the
    // force, so a dagger stab goes deep where a dagger slash only scratches --
    // the heft that makes a knife need sustained work to part a limb (0.2 on
    // the shipped dagger) would make it unable to stab at all. A bigger blade
    // still makes a bigger hole, through its width (stabWidth).
    // stabWidth is the slit's half-length across the blade as a multiple of
    // the blade's authored half-width; stabThick its half-thickness floor.
    // A stab never cleaves or parts a limb, and lands once per slot per
    // stroke (the blade does not re-stab every tick it is inside).
    float stabAlign = TPD(gore, stabAlign);
    float stabDepth = TPD(gore, stabDepth);
    float stabDepthPower = TPD(gore, stabDepthPower);
    float stabWidth = TPD(gore, stabWidth);
    float stabThick = TPD(gore, stabThick);

    // ---- E2. heft: how much weapon is behind the edge -----------------------
    // The item's own voxel volume in WORLD voxels that reads as heft 1.0.
    // DERIVED, not authored: an item's .vox is right there, so a weapon's mass
    // is a fact about its art rather than a number somebody has to keep in
    // sync with it (item.h ItemDef::heftVolume). The stock arming sword is
    // 340 art voxels at scale 4 = 5.3 world voxels, which is where this
    // default comes from — retune it and every weapon rescales together.
    float woundHeftRef = TPD(gore, woundHeftRef);
    // Ceiling on the derived factor, so a comically large authored prop cannot
    // turn one swing into an amputation by arithmetic alone.
    float woundHeftMax = TPD(gore, woundHeftMax);

    // ---- E3. blood on the flesh --------------------------------------------
    // A cut leaves the meat around it soaked. Same mechanism as charring: the
    // exposed voxels' MATERIAL is rewritten (mob sidecar bleed.woundMaterial,
    // defaulting to the creature's own bleed material), so it renders, it
    // travels with a severed limb, and it ejects as blood when cut again.
    // Radius around the cut in world voxels, and the fraction of the voxels in
    // range that take the stain — below 1 so the soak is mottled rather than a
    // uniform repaint, which reads as a red limb rather than a wound.
    // Two densities since 2026-09-02: `woundStainSurface` is the chance on an
    // EXPOSED voxel (the hole's walls and the skin round its mouth — what the
    // wound looks like), `woundStainDensity` on a BURIED one (what a later cut
    // finds). Bone is never soaked (MobDef::tissue), so the hole shows it
    // through the blood instead of one more red voxel.
    float woundStainRadius = TPD(gore, woundStainRadius);
    float woundStainSurface = TPD(gore, woundStainSurface);
    float woundStainDensity = TPD(gore, woundStainDensity);
    // WHITE NOISE CANNOT MAKE A SMEAR, for the same reason it cannot make a
    // chunk (see carveChunkiness). An independent draw per voxel has no
    // feature size, so a soak thresholded against it is a fine red speckle
    // sprinkled evenly over everything in range -- which is what a blast on a
    // body looked like until 2026-09-13. Correlating the draws over a few
    // voxels is what turns the speckle into blotches, and the correlation
    // length IS the size of a blotch. `woundStainCoherence` blends from the
    // old independent draw (0) to fully correlated (1); `woundStainBlob` is
    // the feature size in WORLD voxels, like every other radius here, so a
    // fine skin gets a finer-grained field of the same physical size instead
    // of blotches eight times too big.
    float woundStainBlob = TPD(gore, woundStainBlob);
    float woundStainCoherence = TPD(gore, woundStainCoherence);
    // A CRATER IS NOT A KERF. The blade's soak is a ball of `woundStainRadius`
    // round the slot it cut, which describes a kerf fairly. A blast crater's
    // predicate removes with a chance that falls to zero at the rim, so a
    // GRAZE takes a scatter of voxels across the whole blast sphere: its
    // centroid is inside the limb and its spread is most of the blast radius,
    // and a ball of that size bloodies a quarter of the limb for eight lost
    // voxels. So the blast path measures its soak from the cells it actually
    // REMOVED (phys/bodystain.h CellDist), and this is how far past them the
    // blood reaches, in the LIMB'S OWN LATTICE CELLS -- the art's resolution
    // is the right unit for "a cell of rim", and it is what keeps a scratch a
    // scratch on a rig authored at any scale. The tint rides at the same
    // stainCutRadius : woundStainRadius ratio the kerf uses.
    float craterStainRim = TPD(gore, craterStainRim);
    // ---- E3a. AND THEN THE SOAK DRIES BACK TO FLESH (2026-09-14) -----------
    // The soak above is the creature's blood as a MATERIAL, sitting in the
    // limb's own lattice — and the body burn pass runs the ordinary authored
    // reaction table over that lattice. Blood's rule in reactions.json is
    // `decay -> air` at 8 per-mille a tick, written so a pool on the ground
    // dries up and its chunk goes back to sleep (rule 2), and it applied
    // unchanged to blood inside a limb. Every sword cut therefore opened a
    // hole that ate itself outward at a ~3 s half-life until the geometry
    // rules took the limb off: "the blood voxels just entirely evaporate
    // revealing the below structure, which causes limbs to fall off".
    //
    // With `woundHeals` on, a wound-material voxel that rolls its decay is
    // put BACK to the word it covered (MobLimb::woundWas) instead of being
    // removed, and rolls it `woundHealSlow` times more slowly, because meat
    // settling is not a puddle evaporating. The limb keeps its volume and the
    // red fades off it over about six seconds instead of three.
    //
    // OFF IS THE UNDEAD SETTING, and it is exactly the old behaviour: cuts on
    // a zombie go on rotting outward and shedding its parts. Per-creature as
    // well as global — mob sidecar `bleed.woundHeals` (MobDef::woundHeals) —
    // so the living and the walking dead can disagree in one content key.
    bool woundHeals = TPD(gore, woundHeals);
    float woundHealSlow = TPD(gore, woundHealSlow);

    // ---- E3b. BLOOD ON A BODY: the stain lattice (2026-09-13) --------------
    // The soak above REWRITES flesh to blood. This is the other half, and it
    // is what the owner asked for in as many words: every voxel a cut
    // exposes -- bone included -- carries a STAIN, a tint the renderer lays
    // over the art the way the ground's stain layer does (same palette entry,
    // same look), to a degree that falls off from the cut. A stain never
    // changes what a voxel IS, so bone stays bone and reads as blood-smeared
    // bone; it goes with a severed limb, into every fragment, and comes off
    // again under a washing liquid (water's `washes`).
    //
    // Radius in WORLD voxels round the cut; amounts are the 0..15 scale the
    // world's stain uses. `stainCutAmount` at the centre of an exposed voxel
    // tapering toward the rim; buried voxels take `stainCutBuried` with
    // `stainCutBuriedChance`, low so a later cut finds meat that bled a
    // little rather than a red interior. `stainBoneMin` is the FLOOR for any
    // exposed bone in range: bone is always shown bloodied to some degree.
    float stainCutRadius = TPD(gore, stainCutRadius);
    int stainCutAmount = TPD(gore, stainCutAmount);
    int stainCutBuried = TPD(gore, stainCutBuried);
    float stainCutBuriedChance = TPD(gore, stainCutBuriedChance);
    int stainBoneMin = TPD(gore, stainBoneMin);
    // ---- THE RE-BLEED (2026-09-26, Mob::ReBloodWound) ----------------------
    // While a LIVING limb still owes blood (bleed budget >= 1 voxel, or an
    // open stump), every woundRebloodTicks the smear is laid again round the
    // wound: exposed cells within woundRebloodRadius world voxels, up to
    // woundRebloodAmount of the 0..15 coat at the centre (bone floored at
    // stainBoneMin). Scaled 0.5..1 by how much the wound still owes against
    // woundRebloodFull voxels (a stump is always full). So a wound rinsed
    // clean fills back up with blood while it bleeds, and stays clean once
    // it has stopped. A coat is a maximum, so an already-bloody wound only
    // costs one lattice walk. woundRebloodTicks 0 = off.
    int woundRebloodTicks = TPD(gore, woundRebloodTicks);
    float woundRebloodRadius = TPD(gore, woundRebloodRadius);
    int woundRebloodAmount = TPD(gore, woundRebloodAmount);
    float woundRebloodFull = TPD(gore, woundRebloodFull);
    // CONTACT. A limb in a blood pool, on a bloodied floor or under a drip
    // takes the liquid's authored stain (materials.json `stain`: type, amount,
    // per-mille chance per tick) on its exposed voxels, scaled by this. A dry
    // stain on the ground transfers at half its amount and this fraction of
    // its chance, so walking through old blood lightly bloodies the boots.
    float stainContactScale = TPD(gore, stainContactScale);
    float stainFloorTransfer = TPD(gore, stainFloorTransfer);
    // WASHING. A liquid whose stain `washes` (water) rinses this much amount
    // off an exposed voxel per successful roll at the liquid's own chance.
    int stainWashPerContact = TPD(gore, stainWashPerContact);
    // SPLATTER. A gout or a drip's spray is checked against every body within
    // this many voxels of the wound. The replay flies the particle kernel's
    // own arc (launch speed, then sim.partGravity), aimed across each limb in
    // proportion to the share of the burst's cone the limb covers, so a body
    // is marked where the droplets are seen to land: each arc that meets a
    // limb paints a splat of `splatterSplatRadius` world voxels at
    // `splatterAmount`, at most `splatterPerLimb` arcs per limb per event
    // (past that, one arc stands for several droplets and paints wider).
    // This is how killing something covers YOU in it.
    float splatterReach = TPD(gore, splatterReach);
    int splatterAmount = TPD(gore, splatterAmount);
    int splatterPerLimb = TPD(gore, splatterPerLimb);
    float splatterSplatRadius = TPD(gore, splatterSplatRadius);

    // ---- E4. when a cut becomes a dismemberment -----------------------------
    // Both rules are STRUCTURAL and both fire only on a blade cut (a burn's
    // charring behaviour is deliberately untouched — see Mob::CarveLimb).
    //
    // CUT THROUGH: the carve disconnected a piece of the limb that is at least
    // this fraction of what the limb still had. That is the edge coming out the
    // other side, and it routes through Sever() so the gout, the byBlade audio
    // and the dismember loco states all fire.
    float woundSeverFraction = TPD(gore, woundSeverFraction);
    // HANGING BY A THREAD: the limb is still one piece, but the flesh at its
    // JOINT is mostly gone. Measured as the voxel count inside a sphere of
    // woundNeckRadius world voxels around the joint anchor, against the same
    // count taken on the intact limb (MobLimb::neckAtSpawn). Below this
    // fraction the limb is not attached to anything worth the name.
    float woundNeckRadius = TPD(gore, woundNeckRadius);
    float woundNeckFraction = TPD(gore, woundNeckFraction);

    // ---- E5. what is left of the old instant-sever thresholds ---------------
    // A limb's authored `severImpactSpeed` (assets/mobs/*.json) survives as an
    // EXTREME-speed exception, multiplied by this. The authored numbers (9..20
    // voxels/sec) were written when a hit anywhere near a joint severed
    // outright, so at face value an ordinary swing trips them every time and
    // nothing below ever gets a chance to run. Scaling here rather than
    // rewriting every mob sidecar keeps it one knob and one rebuild-free edit.
    float woundImpactSeverScale = TPD(gore, woundImpactSeverScale);

    // ---- E6. TRAUMA AND TEETH (docs/PLAN_impact_unarmed.md §2) -------------
    //
    // The wound model above is a KERF, and until 2026-09-15 it was the only
    // wound this engine had: every weapon arrived as an edge, which is why
    // there was no mace and no fist. A strike is now three parts (game/
    // impact.h StrikeProfile), and these are the numbers the other two read.
    //
    // THE VICTIM DOES NOT DECIDE WHAT THE WOUND IS MADE OF. A cut leaves the
    // creature's own woundMat; a bruise leaves `bruiseMat`; a bite leaves the
    // BITER's infection. That is the one idea these rows encode, and it is
    // what `Mob::StainWoundAs` exists for.

    // BRUISING. How far a punch discolours the skin around it, in world
    // voxels, at full power -- scaled by (0.5 + 0.5 * power), so even a
    // glancing hit marks. There is no lower bound on how many blows this
    // takes: repeat hits deepen the same patch because the coat is keyed on
    // the lattice position, exactly as the blood soak is.
    float bruiseRadius = TPD(gore, bruiseRadius);
    // ---- A BRUISE IS AN ALPHA THAT DEEPENS, NOT A REPAINT (2026-09-16) -----
    //
    // It used to REWRITE the skin voxel to `bruiseMat`, and that is why it
    // looked wrong: the rewrite is all-or-nothing per voxel, so the falloff had
    // nowhere to go but into a hash-picked SUBSET of the cells in the radius.
    // One blow left a scatter of flat purple voxels, the next left a different
    // scatter, and the result read as speckled damage rather than as a mark.
    //
    // It is a BODY COAT now (phys/bodystain.h): the voxel keeps its material
    // and its art colour, and carries a 0..15 amount that microbody.wgsl
    // multiplies and lerps over the albedo. Every blow ADDS `bruiseStep` to
    // whatever is already there, so the same patch darkens in even steps and
    // the falloff lives in the AMOUNT where it belongs.
    //
    // TWO PUNCHES TO A FULL BRUISE: 6 of 15 is 40%, so the contact goes to 40%
    // on the first blow and 80% on the second, and the third is blood.
    //
    // IT WAS 2.25 (15% a blow) AND THAT WAS INVISIBLE, which is worth writing
    // down because the arithmetic is not obvious from here. The renderer does
    // not draw `amt` directly: `microbody.wgsl` bodyStainTint thresholds it
    // against a value-noise mottle first --
    //     cover = (amt/15 * (1 + stainMottle) - mottle * stainMottle) * stainCoverage
    // -- with `stainMottle` 0.85 and `stainCoverage` 1.35. At amt 2 that is
    // (0.246 - 0.85 * mottle) * 1.35, which is NEGATIVE for any voxel whose
    // mottle exceeds 0.29: about seven voxels in ten drew nothing at all, and
    // the rest drew a cover of ~0.2. The owner's report was "I don't see any
    // bruising at all", and that is why. At amt 6 the same expression clears
    // zero for ~87% of voxels; at 12 it saturates. A coat amount below about a
    // quarter of full is not a faint stain in this renderer, it is no stain.
    float bruiseStep = TPD(gore, bruiseStep);
    // ...AND IT STOPS SHORT OF OPAQUE. 12 of 15 is 80%: deep purple, and still
    // short of the flat stain colour that would cost the anatomy underneath.
    // What happens past here is not a darker bruise, it is blood.
    float bruiseMax = TPD(gore, bruiseMax);
    // AND THEN IT BREAKS. Per-voxel chance, at full power, that a blow lays
    // BLOOD over a voxel ALREADY AT THE CEILING instead of doing nothing --
    // "each hit adds until 80%, and then after that is blood". A chance rather
    // than a flip so a hammered limb goes wet in a spreading scatter over
    // several blows and the two coats interleave the way a real contusion does;
    // at 0.5 a saturated patch is visibly bloody within two or three further
    // blows. 0 disables it and leaves a bruise a bruise forever.
    float bruiseBleedChance = TPD(gore, bruiseBleedChance);
    // ---- HOW FAR UP THE CEILING A VOXEL HAS TO BE TO BREAK ------------------
    //
    // Fraction of `bruiseMax` at or past which `bruiseBleedChance` is rolled.
    // It was hardcoded at 1.0, i.e. EXACTLY the ceiling, and that is a harder
    // line than it looks: the per-voxel jitter is 0.85..1.0, so a patch two
    // blows of 6 deep lands at 10 or 11 of a 12 ceiling and has to wait for a
    // THIRD blow merely to round up. 0.85 lets the contact break on the blow
    // after it saturates instead of the blow after that, which is the whole
    // difference between "hit a bruise again and it bleeds" and "hit a bruise
    // three more times".
    float bruiseBleedFrom = TPD(gore, bruiseBleedFrom);
    // ---- A WEAK BLOW MARKS LESS (the fist/mace difference) ------------------
    //
    // `bruiseStep` is authored for a FULL blow, and until 2026-09-19 a punch
    // and a mace deposited exactly the same coat -- which made "similar with
    // fists, but slower" unexpressible, because the whole ladder below
    // (saturate -> bleed -> pulp) is clocked by how fast the coat deepens.
    //
    // This is the hp that earns the full step: a blow carrying `bruiseHpRef` of
    // blunt gets `bruiseStep`, and a weaker one gets a SQUARE-ROOTED share of
    // it. Root, not linear, for the reason `bruiseStep` itself documents at
    // length -- a coat below about a quarter of full draws NOTHING in this
    // renderer, so a fist at a quarter of a mace's hp scaled linearly would
    // leave a mark nobody can see and the owner would report "punching does
    // nothing" exactly as they once reported it of 15%-a-blow bruising. Rooted,
    // a 4 hp fist against a 16 hp reference lands half a step (20% a punch,
    // visible) and needs about twice the blows to reach the same place.
    float bruiseHpRef = TPD(gore, bruiseHpRef);
    // ...and the floor under that share, so an incidental tap still marks.
    float bruiseHpFloor = TPD(gore, bruiseHpFloor);
    // ...and WHAT it discolours the skin to, BY NAME.
    //
    // THE ONE NAME-TYPED TUNING ROW IN THE FILE, and the reason is that it
    // cannot be anything else. An id would be stale after either hot reload:
    // tuning.json reloads on F5 and materials.json on R, independently, and a
    // number here would silently start meaning a different substance the first
    // time somebody inserted a material. So it is resolved at USE through
    // MobSystem::MaterialIdNamed, the same way a mob sidecar's
    // `bleed.material` is resolved at load -- behaviour is data, and a
    // material is named (CLAUDE.md design rule 4).
    //
    // An unknown name resolves to 0, which means "no bruise": the blow still
    // lands, still hurts and still dents, it simply leaves no mark. That is
    // the right failure for a cosmetic row -- louder would mean a typo in a
    // colour costing somebody their combat.
    std::string bruiseMat = TPD(gore, bruiseMat);
    // A PUNCH DOES NOT OPEN YOU. Fraction of a cut's drip budget that blunt
    // trauma tops up (Mob::Damage and the dent's own carve). 0 makes a mace a
    // completely dry weapon; 1 makes it bleed like a sword, which is the
    // behaviour this whole split exists to avoid.
    float bluntBleedScale = TPD(gore, bluntBleedScale);
    // How deep a DENT a full-power blunt hit takes out of flesh, in world
    // voxels, before the weapon's own `bluntCarve` fraction scales it. A fist
    // authors 0 and removes nothing at all; a gauntlet ~0.35 and a mace ~0.6
    // of this. Radial, never a kerf, and carved as DamageCause::Blunt, whose
    // row in game/severpolicy.h refuses the collapse sever -- the crater is still soaked in the victim's
    // woundMat, which is the "replace them with gore" half of the owner's
    // spec.
    //
    // NOTHING IS REMOVED UNTIL THE SPOT IS PULPED -- see `pulpCarveFrom`. This
    // is the radius a blow lands on tissue that has ALREADY been beaten open,
    // not the radius of a first blow on clean skin.
    float bluntCarveRadius = TPD(gore, bluntCarveRadius);
    // ---- THE THIRD RUNG: PULPED TISSUE COMES AWAY (2026-09-19) --------------
    //
    // A blunt blow used to dent from the FIRST hit, at a radius that depended
    // only on the weapon -- so `bluntCarveRadius` was a choice between "a mace
    // shaves voxels off a pristine arm" and (what shipped, 0) "a mace only ever
    // bruises, however long you beat somebody with it". Neither is the thing:
    // the owner's spec is a LADDER, and each rung has to be EARNED on the spot
    // being hit.
    //
    //   1. clean skin       -> the coat deepens by `bruiseStep` to `bruiseMax`
    //   2. a saturated one  -> `bruiseBleedChance` breaks it and lays BLOOD
    //   3. broken, bloodied -> voxels start coming away
    //
    // Rung 3 is these two rows. `Mob::BruiseLimb` reports how much of the
    // contact CORE already wears blood at `pulpAmt` or deeper; the dent radius
    // is `bluntCarveRadius` scaled by how far that share has climbed past
    // `pulpCarveFrom`. So the first blows on an intact limb remove NOTHING at
    // any weapon's `bluntCarve`, and a crater only opens where somebody has
    // hit the same place over and over -- which is also why the bulk of what a
    // mace kill leaves behind is still bruise: the bruise radius is wider than
    // the dent by a factor of two or more, and it applies on every blow while
    // the dent applies on few.
    //
    // A COMPOUNDING RULE ON PURPOSE. The crater `CarveLimbRadial` opens is
    // soaked in the victim's own woundMat and blood, so the tissue it exposes
    // reads as pulped to the NEXT blow and the hole deepens faster than it
    // started. That is "if a player consistently hits just the exact same spot
    // it will become pretty gruesome"; the sever is still refused outright
    // (the Blunt row's collapseSevers, game/severpolicy.h), so a caved-in skull is a caved-in skull and never
    // a decapitation.
    // blood-coat depth (0..15) that counts as pulped
    float pulpAmt = TPD(gore, pulpAmt);
    // pulped share of the core below which nothing comes away
    float pulpCarveFrom = TPD(gore, pulpCarveFrom);
    // ---- PULPED TISSUE DISSOLVES (the blunt counterpart of bite rot) ---------
    //
    // Until now the dent was instant: `CarveLimbRadial` in one call, one tick,
    // the voxels gone. That made a mace blow EITHER quiet (bruise only) or
    // surgical (a clean sphere of nothing), with no middle that reads as beaten
    // flesh coming apart. These two rows are the middle. When a blow earns a
    // dent (ripeness > pulpCarveFrom), the limb is flagged for DISSOLUTION
    // instead, and Mob::BluntPulpTick eats pulped voxels one at a time at this
    // rate -- the same per-tick Bernoulli draw the infection uses, so the
    // disappearance is noisy, gradual, and never a slab.
    //
    // The candidates are blood-coated voxels at `pulpAmt` depth or deeper on
    // the same limb. Further blows keep adding pulp; the dissolution keeps
    // eating it. Both run concurrently, which is why sustained hits on one
    // spot cave it in faster than the first blow alone would.
    // world voxels/minute, per flagged limb
    float pulpRotRate = TPD(gore, pulpRotRate);
    // ---- UNARMED OVERRIDES --------------------------------------------------
    //
    // A fist and a mace share every row above, and the only thing that
    // separated them was `bruiseHpRef`'s square-root scaling on the step. That
    // is not enough: a punch should mark a smaller area, break the skin less
    // readily, and cave nothing in under any amount of beating -- all of which
    // are SHAPE differences, not rate differences, and the sqrt cannot say them.
    //
    // Each row below overrides the matching row above when the blow is from a
    // natural weapon (BluntHit::unarmed). A NEGATIVE value means "use the main
    // row": the default, and the way to say "fists and maces are the same here".
    float unarmedBruiseRadius = TPD(gore, unarmedBruiseRadius);
    float unarmedBruiseStep = TPD(gore, unarmedBruiseStep);
    float unarmedBleedChance = TPD(gore, unarmedBleedChance);
    float unarmedBleedScale = TPD(gore, unarmedBleedScale);
    float unarmedCarveRadius = TPD(gore, unarmedCarveRadius);
    float unarmedPulpCarveFrom = TPD(gore, unarmedPulpCarveFrom);
    // KNOCKED DOWN BY A BLOW TO THE HEAD (Mob::BluntHit). Blunt trauma (mace,
    // fist, a bite's blow through armour) landing on the HEAD at or above
    // `headKnockPower` -- the sweep's 0..1 power, speed x alignment x segment
    // scale -- has `headKnockChance` of ragdolling the creature for
    // `headKnockSeconds` before it may get up. Rolled on the blow's own seed.
    float headKnockPower = TPD(gore, headKnockPower);
    float headKnockChance = TPD(gore, headKnockChance);
    float headKnockSeconds = TPD(gore, headKnockSeconds);
    // A BITE. Radius of the tear in world voxels at full power, scaled by
    // (0.4 + 0.6 * power); `biteBlob` is the correlated noise's feature size
    // in SKIN voxels, i.e. the size of one piece that comes away. Same pair,
    // same meaning, as a zombie's `rot.radius` / `rot.blob` -- a bite and the
    // hole a zombie was born with are the same shape (Mob::CarveBlob).
    //
    // THE PLAN AUTHORED 1.1 AND THE MEASUREMENT SAYS 0.45. The world is ten
    // voxels to the metre and a human thigh is about 1.2 world voxels thick,
    // so a 1.1-voxel ball is most of a limb's cross-section: `bite-rot`
    // measured ONE bite taking 936 of 1344 voxels off a thigh and the second
    // collapsing it, which is not a rotten wound -- it is an amputation with
    // teeth, and it put the gate on a knife-edge where a different mob id
    // flipped the answer. 0.45 is a hole about 9 cm across, which is a bite.
    // Enough of them still take a hand off, because the collapse sever is left
    // ON for a bite (the Bite row's collapseSevers, game/severpolicy.h) -- it
    // simply takes several.
    float biteRadius = TPD(gore, biteRadius);
    float biteBlob = TPD(gore, biteBlob);
    // ...and how much wider than a blade's crater rim the bite's soak reaches,
    // as a MULTIPLE of craterStainRim. Above 1 because a tear is a ragged hole
    // rather than a clean slot: the mess goes further than the damage.
    float biteStainScale = TPD(gore, biteStainScale);
    // ROT SETTLES SLOWER THAN BLOOD DOES. `woundHealSlow` divides blood's own
    // decay inside a limb so a cut fades over ~6 s instead of ~3 s; an
    // infection is not a wound settling, it is something living in you, so it
    // gets its own (larger) divisor. Applies to the material a BITE rewrote
    // flesh to (MobLimb::infectMat), on a creature whose wounds heal at all --
    // in an undead, whose do not, rot never goes away, which is the point.
    float infectHealSlow = TPD(gore, infectHealSlow);
    // ---- AN INFECTION IS ALIVE, AND IT IS EATING YOU ------------------------
    //
    // Until 2026-09-16 a bite's rot was a PICTURE: `infectMat` rewrote the
    // tissue the tear exposed and then sat there, the same shape forever, on a
    // creature whose wounds heal at all it dried back and on an undead it did
    // nothing. Something a zombie put in you that never got any worse is not an
    // infection, it is a tattoo.
    //
    // These two rates are what make it a CLOCK (Mob::InfectTick). Both are in
    // WORLD VOXELS PER MINUTE and both are charged PER INFECTED LIMB, so being
    // bitten five times really is five times the disease:
    //
    //   SPREAD  converts healthy tissue next to the rot into more rot. Only
    //           MobDef::tissue voxels -- bone stays bone, a worn shell is not
    //           the body -- and only the six-neighbours of what is already
    //           infected, so it grows as a front from the wound rather than
    //           appearing all over. When a limb has no tissue left to take it
    //           crosses a JOINT into a rig-adjacent limb, which is the whole
    //           reason an untreated bite on the hand eventually reaches the
    //           torso and kills. That jump is parameter-free on purpose: it is
    //           "the limb is full", not a second rate to keep in sync.
    //
    //   ROT     takes infected voxels away for good -- the ichor evaporates,
    //           the hole is real. Expressed through the ordinary burn flush, so
    //           hp falls with the fraction of the limb that is gone, a limb
    //           eaten past kLimbCollapseFraction comes off, and a `vital` limb
    //           eaten through kills, with no death rule of its own.
    //
    // SPREAD ABOVE ROT IS A DISEASE; ROT ABOVE SPREAD IS A SCAB. At the
    // defaults the infected mass grows half a voxel a minute while half a voxel
    // a minute of the creature leaves, which is slow enough to be a background
    // dread over a session and fast enough to be fatal if nothing is done. 0 on
    // either is the old static behaviour of that half.
    //
    // AN AVERAGE, REALISED BY CHANCE. Per minute rather than per tick because
    // that is the unit the question is actually asked in ("how long have I
    // got"), and the pass converts to lattice voxels per tick with the limb's
    // own scale^3, so a fine skin rots at the same PHYSICAL rate as a coarse
    // one instead of 512 times slower. What it does NOT do is bank the
    // fraction and spend it in instalments: each tick rolls an independent
    // chance whose expectation is this number, and each voxel is drawn
    // uniformly from the eligible rim, because a batched average of the right
    // size still looks like a machine (see the note above Mob::InfectTick for
    // the version that did, and what it looked like).
    // UNITS CHANGED 2026-10-01 (PLAN_weapon_coats B, owner follow-up): these
    // are now the ROT's PER-VOXEL rates -- the chance per second that one
    // rotflesh voxel (one world voxel across; a finer lattice runs its cells
    // x scale as often) converts a neighbour / is eaten. Everything above about
    // per-limb minutes describes the model these replaced (DESIGN.md
    // "Infection is a material"). They are rotflesh's `infect` numbers
    // (its block authors none) and the world-grid rot rules read the same
    // spread (reactions.json `infectSpread`): one number, both places.
    // Spread above rot (branching ratio > 1) is a disease that grows.
    // per voxel per second
    float infectSpreadRate = TPD(gore, infectSpreadRate);
    // per voxel per second
    float infectRotRate = TPD(gore, infectRotRate);
    // multiplier on both rates for mob limbs only
    float infectMobMult = TPD(gore, infectMobMult);

    // ---- ...AND WHAT IT LEAVES STANDING IN THE HOLE ------------------------
    //
    // The rot eats TISSUE and cannot touch bone, so a limb rotted through ends
    // as a clean, near-white skeleton in a green wound -- which is not what
    // rot looks like. Every voxel the infection takes coats the non-tissue
    // voxels it was touching (Mob::InfectStep), so bone is bloodied exactly as
    // it is uncovered. This is the incremental form of a cut's
    // `CutSoak::boneMin`, which already does it for a blade.
    //
    // MEAN COAT on the body-stain 0..15 scale. The coat is an ALPHA over the
    // voxel's own colour, so this is "how much of the bone is hidden": 15 is
    // opaque gore, 0 turns the whole thing off and bone comes out white again
    // (the pre-2026-09-17 behaviour, kept reachable).
    float infectBoneStain = TPD(gore, infectBoneStain);
    // ...AND THE SPREAD ROUND IT, +/- this, drawn per voxel and keyed on the
    // bone voxel's own position so a cell exposed twice does not change
    // colour. THIS IS THE ROW THAT KEEPS IT READING AS BONE: at 0 every
    // exposed cell takes the identical amount and the surface is a flat slab
    // of red, however well chosen the mean. The variation is what lets the
    // bone show through in patches. Clamped into 0..15 after the jitter, so a
    // wide spread simply saturates at the ends rather than wrapping.
    float infectBoneStainVary = TPD(gore, infectBoneStainVary);
    // WHICH SUBSTANCE, per voxel: this fraction take the INFECTION's own stain
    // (the biter's `bite.stain` -- a zombie's ichor, green) and the rest the
    // victim's blood (dark red). Two hues interleaved at the voxel scale read
    // as a diseased, mottled surface; 0 or 1 is one flat colour over the
    // whole exposure. Falls back to whichever of the two exists when the other
    // does not (a creature with no blood, a biter with no ichor).
    float infectBoneIchor = TPD(gore, infectBoneIchor);
    // ---- WHAT A VOXEL OF BRAIN IS WORTH -------------------------------------
    //
    // Flat hp per BRAIN voxel destroyed, by any cause, charged in
    // Mob::CarveLimb on top of the ordinary volume damage. The only absolute
    // per-voxel hp figure in the gore model -- everything else is a fraction of
    // a limb, which is what makes a wound read the same on any rig; this one
    // cannot be, because the point of it is that a hole in the brain is not a
    // proportion of a head, it is a hole in the brain.
    //
    // At the shipped 10 and a human head's 45 hp, roughly five voxels of brain
    // is fatal -- so breaching the skull (two voxels of bone, which the rot
    // chews at half rate) is what takes the time, and what is behind it goes
    // fast. 0 disables the mechanic entirely and restores the pre-2026-09-18
    // behaviour, where brain was ordinary flesh with a different colour.
    float brainHpPerVoxel = TPD(gore, brainHpPerVoxel);
    // ---- WHAT LOSING MATTER COSTS (was mob.h kCarveDamagePerVolume) ------
    //
    // Mob::CarveLimb charges a limb this multiple of its max hp for the whole
    // of its (woundHp-weighted) volume: losing a third of a limb costs half of
    // its hp at 1.5. Every carve pays it -- blade, blast, beam, burn, rot.
    float carveHpPerVolume = TPD(gore, carveHpPerVolume);
    // ---- A BLAST'S POWER, NOT ONLY ITS RADIUS (W2-H) ---------------------
    //
    // The body crater follows the terrain crater's rule (sim_explode.wgsl): the
    // power that reaches a voxel is the blast's power, less
    // sim.falloffPerCell per voxel of distance, less every worn shell the ray
    // crossed (gear.blastShellCells). What is left over the voxel's own
    // hardness, as a fraction of THIS, scales the crater radius at that voxel:
    // at or above it the crater is the full blast radius (a grenade on bare
    // flesh, unchanged), below it the crater shrinks, and at nothing left the
    // voxel is untouched. Only a blast that states a power reads it (an
    // explosion does; a fall's splat is radius-only).
    float blastPowerRef = TPD(gore, blastPowerRef);
    // ---- A THROWN ROCK IS A BLOW (W2-H phase 2, MobSystem::ApplyContactDamage)
    //
    // A loose body (debris, a log, a flung limb) that strikes a living
    // creature's limb hard enough is a BLUNT blow on that limb, through the
    // same Mob::BluntHit (so the shell table, the bruise and the transmitted
    // share all apply). Strength is the IMPULSE of the contact -- the
    // striking body's mass (reduced against the creature's when the creature
    // is limp) x its approach speed, kg*m/s -- and only what exceeds
    // contactImpulseMin counts, at contactHpPerImpulse hp per kg*m/s, capped
    // at contactMaxHp per contact. At most contactMaxPerTick contacts are
    // billed per tick (the hardest first): rule 2, a collapsing wall must not
    // bill a thousand. contactHpPerImpulse 0 switches it off.
    float contactImpulseMin = TPD(gore, contactImpulseMin);
    float contactHpPerImpulse = TPD(gore, contactHpPerImpulse);
    float contactMaxHp = TPD(gore, contactMaxHp);
    int contactMaxPerTick = TPD(gore, contactMaxPerTick);
    // ...AND ONLY A BODY THAT WAS ALREADY MOVING STRIKES. The approach speed
    // is the STRIKER's own (pre-impact, along the contact normal, toward the
    // creature), so walking into a resting log is you kicking it and bills
    // nothing. Below contactMinSpeed (vox/s) nothing is a blow; nor is a body
    // whose current run of motion (Physics::MotionRunVox) is shorter than
    // contactMinTravel voxels -- a log nudged an inch, or rolling back onto
    // the foot that pushed it, has not been going anywhere.
    float contactMinSpeed = TPD(gore, contactMinSpeed);
    float contactMinTravel = TPD(gore, contactMinTravel);

    // ========================================================================
    // F. BLOOD IS HEALTH — every drop that leaves a body is hp leaving it
    // ========================================================================
    // Until 2026-09-02 blood was a VISUAL: a wound carried a voxel budget and
    // dripped it out, and hp only ever moved at the moment of the blow. A
    // creature could lose an arm, stand in a puddle of its own blood for a
    // minute and be exactly as alive as the tick after the cut. Now the drip
    // IS the damage (Mob::DrainBlood): every whole blood voxel a wound puts
    // into the grid costs this much hp, spread across the creature's live
    // limbs in proportion to what each still has, and a micro droplet costs
    // 1/microScale^3 of it (a droplet is that fraction of a voxel). When the
    // total reaches zero the creature dies of blood loss, through the same
    // Die() a blow to the heart reaches.
    //
    // ONE knob, and it is a RATE per voxel rather than a time-to-death, so the
    // same blood costs the same life on a critter and on a human: the small
    // creature dies of less blood because it has less. Time to bleed out from
    // a single open stump at the defaults (30 Hz, bleedDripTicks 4, clump
    // radius 0) is hpTotal / (7.5 drips/s x 1 voxel x this).
    float bleedHpPerVoxel = TPD(gore, bleedHpPerVoxel);
    // AN AMPUTATION DOES NOT CLOSE. The stump's authored severStumpBudget was
    // the whole of what a lost limb bled, and it ran dry in seconds. With this
    // on, the stump wound is topped back up to one clump every tick for as
    // long as the creature lives, so a lost limb is a clock: the drip above
    // takes hp at a bounded rate until nothing is left. Rule 2 still holds
    // because the process is bounded by the creature's own hp — the drip ends
    // at death, and a corpse does not bleed. Off restores the finite stump.
    bool stumpBleedsOpen = TPD(gore, stumpBleedsOpen);

    // ========================================================================
    // G. BURNS CAP HEALTH — the more of the body is burnt, the less it can hold
    // ========================================================================
    // Burning already charges hp for the voxels it removes, but a body that is
    // COOKED rather than consumed lost nothing by that account: flesh_cooked
    // and flesh_charred are still voxels, so a creature 60% charred and 100%
    // present read as healthy. Now the burnt FRACTION of the body sets a CAP
    // on every limb's hp (Mob::ApplyBurnCap), authored as one piecewise-linear
    // curve through three points: intact -> full; `burnCapMidFraction` burnt
    // -> `burnCapMidHealth` of full; `burnDeathFraction` burnt -> zero, which
    // is death. hp is clamped DOWN to the cap and nothing may ever heal past
    // it, so a badly burnt body is a permanently short bar until it dies.
    //
    // The fraction is BURNT SURFACE OVER SURFACE, the body-surface-area grading
    // burns get in the clinic: a cooked / burning voxel is half burnt, a
    // charred / ash voxel is fully burnt, a voxel fire removed is fully burnt
    // (BodyBurnState::burntAway), summed at any depth, against the body's
    // burnable voxels that had an open face when it was whole
    // (MobLimb::surfaceAtSpawn; burnable = tag:flammable or a burn stage, so
    // bone never counts). A surface and not a volume because char is inert
    // and shields what is under it — a human stood in a fire converged at 30%
    // of its burnable volume with most of its skin raw under a black shell,
    // and would have stood there forever. Garments and held items are not the
    // body and are not counted (Mob::IsWornSlot).
    float burnCapMidFraction = TPD(gore, burnCapMidFraction);
    float burnCapMidHealth = TPD(gore, burnCapMidHealth);
    float burnDeathFraction = TPD(gore, burnDeathFraction);
  } gore;

  // ---- coats: a substance ON a body, as opposed to in the ground -------------
  //
  // A body voxel carries a COAT — a material and how much of it (sim/voxload.h
  // PrefabVoxel::stain) — and materials.json says per substance how fast it
  // dries off and whether a foot tracks it (MaterialDef::coatDecay/coatShed).
  // These are the ENGINE-side numbers that govern the same machinery: how
  // often the per-limb ledger is retaken, how the authored dry times are
  // scaled globally, and the budgets that keep tracking bounded (rule 2).
  //
  // CPU-ONLY, like `gore` and `melee` above: NO_WGSL rows in tuning_params.def,
  // no WGSL constant. A coat never reaches the sim.
  struct Coat {
    // Ticks between recounts of the per-limb coat ledger (game/mob.h
    // LimbCoat). Only ever taken when something changed a coat byte since the
    // last one, so this bounds the cost of a body that is ACTIVELY being
    // bloodied — a clean or settled one pays nothing whatever this says.
    int recountTicks = TPD(coat, recountTicks);
    // Global multiplier on how fast every authored coat dries: the material's
    // `coat.decay` seconds per amount level are DIVIDED by this, so 2 dries
    // everything twice as fast and small values make blood permanent. A dial
    // on the whole look rather than a per-material edit.
    float decayScale = TPD(coat, decayScale);
    // How much faster a WET coat (a washer, i.e. water) dries while the body is
    // in sunshine: daylight up and open sky over the limb (MobSystem::
    // InSunlight). Divides the drying period like decayScale; 1 = no effect.
    float sunDryScale = TPD(coat, sunDryScale);
    // Ground cells one footfall may track a coat onto. A footprint is a patch,
    // not a point, and this is how big the patch may get.
    int shedCells = TPD(coat, shedCells);
    // Deposits every creature together may make in one tick. The bound on how
    // much tracking a crowd can push into the world; a foot refused here
    // simply leaves no print that tick.
    int shedPerTick = TPD(coat, shedPerTick);
    // Amount of coat one deposit takes off the foot, in the 0..15 scale — how
    // fast a bloodied boot walks itself clean.
    int shedAmount = TPD(coat, shedAmount);
    // Below this coated fraction of a body part, the HUD says nothing about
    // it: a single splashed voxel is not "covered in blood".
    float hudMinFrac = TPD(coat, hudMinFrac);
  } coat;

  // ---- melee: the stroke driver's feel ---------------------------------------
  //
  // THE VALUES BEHIND MeleeTuning (game/melee.h), AND NOTHING ELSE. That struct
  // stays exactly where it is and keeps exactly its fields — MeleeState holds
  // one, MeleeSweepDamage takes one by const reference, and both the NPC driver
  // and the gates read `t.commitSpeed` style. What moved is only where the
  // NUMBERS come from: `ApplyMeleeTuning` (game/melee.h) copies this group into
  // a MeleeTuning at startup and on every F5, so the whole feel loop is a JSON
  // edit rather than a rebuild. Its header said this would happen:
  //
  //   "Lives here rather than in tuning.json for the first pass because these
  //    are the knobs being *designed*, not the ones being dialled; once the
  //    feel settles they belong in tuning.json like everything else."
  //
  // CPU-ONLY, so its tuning_params.def rows are NO_WGSL — same as `gore`
  // above. Nothing in a shader reads a melee number, and nothing here
  // may ever reach one: the sim must not see presentation state (rule 1).
  //
  // UNITS. Where MeleeTuning stores WORLD VOXELS derived from a physical size
  // (MetresPerSecToCells / MetresToCells in its initialisers), the key here is
  // the PHYSICAL number with the unit in its name — `fullSpeedMps`, not
  // `fullSpeed`. Storing the post-conversion voxel figure would bake the
  // current kVoxelMeters into the JSON, and the sword has already been bitten
  // once by exactly that (item.h ItemDef::scale). Angles, fractions and
  // seconds are unitless-in-voxels and carry their MeleeTuning name unchanged.
  struct Melee {
    // ---- the control law ----------------------------------------------------
    // reference drive speed, px/s: the Wind label, the steer cap, the whoosh
    float commitSpeed = TPD(melee, commitSpeed);
    // seconds of follow-through
    float recoverTime = TPD(melee, recoverTime);
    // tip speed for full damage
    float fullSpeedMps = TPD(melee, fullSpeedMps);
    // tip speed below which a hit does nothing
    float minSpeedMps = TPD(melee, minSpeedMps);
    // radians of tip azimuth per mouse pixel
    float aimGainX = TPD(melee, aimGainX);
    // radians of tip elevation per mouse pixel
    float aimGainY = TPD(melee, aimGainY);
    // metres of tip reach per dReach unit
    float reachGainM = TPD(melee, reachGainM);
    // ---- where the point may go --------------------------------------------
    // 2.36 (135 deg) drove the commanded point BEHIND the character and parked
    // the shoulder ball on its authored 50-degrees-past-the-back-plane stop.
    // 1.83 is 105 deg: the whole front plus a little past side-on. The full
    // note is on MeleeTuning::azOut in game/melee.h.
    float azOut = TPD(melee, azOut);             // radians, to the weapon side
    float azAcross = TPD(melee, azAcross);          // radians, across the body
    // radians, arm hanging at the side
    float elMin = TPD(melee, elMin);
    float elMax = TPD(melee, elMax);             // radians, overhead
    // ---- how the arm holds it ----------------------------------------------
    float handExtend = TPD(melee, handExtend);        // fraction of arm reach
    float extendSmoothing = TPD(melee, extendSmoothing);   // seconds halflife
    // radians/sec on the lean plane
    float leanTurnRate = TPD(melee, leanTurnRate);
    // SIGN only: +1 hand leads, -1 point leads
    float handLead = TPD(melee, handLead);
    // metres, only when the rig cannot say
    float fallbackReachM = TPD(melee, fallbackReachM);
    // fraction of arm reach the hand may use
    float reachFraction = TPD(melee, reachFraction);
    // metres — the seed of last resort
    float guardForwardM = TPD(melee, guardForwardM);
    float guardUpM = TPD(melee, guardUpM);
    float guardSideM = TPD(melee, guardSideM);
    // seconds of mouse history
    float dirSmoothing = TPD(melee, dirSmoothing);
    // seconds halflife on the blade frame
    float bladeSmoothing = TPD(melee, bladeSmoothing);
    // HOW FAR THE WRIST MAY TAKE THE BLADE from what the solved forearm gives
    // it for free, radians. THIS IS A GROSS BUDGET, NOT AN ANATOMICAL ANGLE,
    // and reading it as the latter is what cost the phase C/D merge its only
    // real interaction bug.
    //
    // The blade asks the wrist for a FULL orientation delta — direction AND
    // roll — and the neutral grip is about pi of roll away from what a
    // committed cut wants. Measured on the repaired swing-plane fixture, every
    // pass reports wristWant 2.6..3.1 rad whichever grip is authored. So the
    // budget the STEERING actually gets is `wristMaxAngle` minus that standing
    // tax, and it is the difference that has to be a wrist:
    //
    //   old grip, cap 2.80 -> ~1.2 rad free   (what phase C measured against)
    //   new grip, cap 1.50 -> ~0.0 rad free   (pegged in every pass; the bug)
    //   new grip, cap 3.10 -> ~1.1 rad free   (what ships)
    //
    // 1.50 was set from an A/B on the OLD swing-plane fixture, whose open-loop
    // ready never drove the arm into the poses a committed cut reaches, so the
    // cap read as non-binding there and clamped half of every real cut here.
    // With 3.10 the gate's follow residual is 0.34 vox where 1.50 gave 3.66,
    // and its az/el tracking, planarity and diagonal ratio all fall inside
    // their bounds instead of outside them.
    //
    // AND THE GRIP THE MIDDLE ROW WAS AUTHORED FOR IS GONE (2026-09-01). The
    // `[0,0,-90]` along-the-arm grip was landed to buy budget here, and the
    // measurement above is the proof it never did: the ask is 2.6..3.1 rad
    // WHICHEVER grip is authored, so the cap ended at 3.10 either way. What
    // the grip cost instead was the idle pose — at weight 0 nothing steers and
    // the blade simply lay down the forearm, hilt through the fist. The grip
    // is back to `[0,-90,0]` and this number is unchanged, which is the whole
    // evidence that it was never the grip's number.
    //
    // OWNER ITEM: the standing ~pi of roll is still a GRIP question, and
    // authoring the neutral roll into {sword,cleaver}.json would let this come
    // down to a real wrist. The naive attempt ([180,0,-90]) made the follow
    // residual worse (8.08), so it is a piece of work rather than a sign flip.
    float wristMaxAngle = TPD(melee, wristMaxAngle);
    // ---- HOW MUCH OF THAT IS APPLIED, AND WHEN -----------------------------
    // The wrist RAMPS with commitment. Above is the ceiling; this is the
    // throttle. A blade held still keeps the orientation the solved forearm
    // gives it (the authored grip: blade out of the fist, and UP when the arm
    // is forward), and a moving one is aligned to the stroke. Without it a
    // motionless guard was still wrenched round to lay the blade along the
    // shoulder-to-point radius, which is "the sword points vertically down".
    //
    // Tip speed in m/s, converted to world voxels/sec by ApplyMeleeTuning
    // like the damage ramp beside it. ONLY the committed Slash bypasses the
    // ramp (2026-09-01; Wind and Recover used to as well, which made raising
    // the sword for an overhead wrench the wrist onto the radius — the band
    // is wider for the same reason, so a deliberate 1-2 m/s raise mostly
    // keeps the grip pose and the blade stays generally UP).
    // below: the grip pose
    float steerSpeedLoMps = TPD(melee, steerSpeedLoMps);
    // above: full alignment to the stroke
    float steerSpeedHiMps = TPD(melee, steerSpeedHiMps);
    // applied at and below the low speed
    float steerFloor = TPD(melee, steerFloor);
    // ---- PER-JOINT SMOOTHING (seconds of halflife) -------------------------
    // Two knobs because the joints tolerate lag differently: the ARM lagging
    // the mouse reads as weight, the WRIST lagging a cut misaligns the edge.
    // `armSmoothing` eases the integrated stroke before the tip is built
    // (0 = off, bit for bit the old behaviour); `wristSmoothing` owns the
    // wrist's commitment envelope AND its chase of the commanded blade
    // orientation (3x faster through a Slash). See game/melee.h.
    float armSmoothing = TPD(melee, armSmoothing);
    float wristSmoothing = TPD(melee, wristSmoothing);
    // How far BEHIND the shoulder's frontal plane the hand may sit, as a
    // fraction of arm reach. The azimuth window bounds the commanded POINT;
    // the hand is that point minus a whole blade, and unbounded it sat
    // voxels behind the plane at the stops — "the arm goes behind him".
    float handBackFrac = TPD(melee, handBackFrac);
    // ---- WHAT THE ARM MAY DO WHILE IT SERVES THE BLADE ---------------------
    // Radians. `elbowPoleCone` bounds the bend PLANE the driver asks for, off
    // straight-back in its own basis; `elbowAxisCone` caps how far the rig's
    // hinge-axis override may sit from the forearm's AUTHORED axis, and the
    // rig takes the tighter of it and the shoulder's own authored twist range.
    // Both exist because an unbounded override turned a one-way elbow into a
    // joint that bent either way — see game/melee.h and game/mob.cpp.
    // 100 deg: down, up or out, never forward
    float elbowPoleCone = TPD(melee, elbowPoleCone);
    // pi = OFF; an A/B knob, see game/mob.cpp
    float elbowAxisCone = TPD(melee, elbowAxisCone);
    // damage floor for a flat-on slap
    float edgeFloor = TPD(melee, edgeFloor);
    // widest gap between the sweep's rows of probe rays, metres; thinner
    //   things can pass between them (game/melee.cpp MeleeSweepDamage)
    float sweepSpacingM = TPD(melee, sweepSpacingM);
    // most rows one tick's sweep may cast (cost bound: x5 rays each)
    int sweepMaxSteps = TPD(melee, sweepMaxSteps);
    // a follow-up strike from the opposite side starts in the recover, its
    //   windup this many times faster (session.cpp strike chaining)
    float chainWindupRate = TPD(melee, chainWindupRate);
    // compass steps either side of dead opposite that still chain
    int chainSectorLeeway = TPD(melee, chainSectorLeeway);
    // a strike held past its windup parks there and, released, lands this
    //   many times harder (session.cpp charged strike)
    float chargeDamage = TPD(melee, chargeDamage);
    // ticks a held strike takes to re-aim to another stroke's windup end
    int chargeBlendTicks = TPD(melee, chargeBlendTicks);
    // a held strike re-aims only on a FLICK: a mouse motion over within this
    //   many ticks and at most chargeFlickPx pixels; longer or bigger is a
    //   camera turn and leaves the held strike alone (strike_pick.h Feed)
    int chargeFlickTicks = TPD(melee, chargeFlickTicks);
    float chargeFlickPx = TPD(melee, chargeFlickPx);
    // AN INJURED ARM (Mob::HandCondition: the arm chain's weakest hp
    //   fraction). Below `injuredArmFrom` its strokes slow, up to
    //   1 + `injuredArmSlow` times as long; below `injuredArmDrop` the
    //   hand lets go of what it holds.
    float injuredArmFrom = TPD(melee, injuredArmFrom);
    float injuredArmSlow = TPD(melee, injuredArmSlow);
    float injuredArmDrop = TPD(melee, injuredArmDrop);
    // ---- BLADE ON BLADE (game/melee.h MeleeSweepDamage's parry block) -------
    // The four knobs a parry has. They are `melee.*` rather than `combatfx.*`
    // because a block is MECHANICS — it stops a cut, it costs the blocking
    // weapon hp, and it shoves the defender's guard — where combatfx is
    // presentation that can be switched off without changing an outcome.
    // metres of slack around the two segments
    float blockGapM = TPD(melee, blockGapM);
    // fraction of the blow the blade takes
    float blockItemDamage = TPD(melee, blockItemDamage);
    // radians the defender's guard is beaten
    float blockNudgeAz = TPD(melee, blockNudgeAz);
    //   open, azimuth and elevation, at power 1
    float blockNudgeEl = TPD(melee, blockNudgeEl);
    // ---- ONE STROKE, DIMINISHING STRIKES (game/melee.h EdgeSweep::victimHits)
    // The hp multiplier on each strike after the first that one stroke lands
    // on the same creature, compounding: strike n does repeatHitScale^(n-1).
    // A strike is one body resolved in one sweep tick, so a blade dragged
    // through an arm for three ticks and then into the chest is four strikes,
    // and the first -- where the blow ARRIVED -- is the one at full weight.
    float repeatHitScale = TPD(melee, repeatHitScale);
    // ---- DISCRETE STRIKES (the player's ONLY control since 2026-09-25) ------
    // A click fires an AUTHORED stroke program (the same attack_styles.json
    // entries the NPCs replay), direction picked by the mouse flick at the
    // press. The freeform mouse-steers-the-tip mode (`controlMode` 1) is gone.
    // Mouse px/s at the press below which a click has no direction: the strike
    // alternates horizontal L/R instead. Same unit as commitSpeed, far lower —
    // a flick is a read of intent, not a commitment gesture.
    float pickMinSpeed = TPD(melee, pickMinSpeed);
    // ---- the swing is bound to the BODY, like the head ---------------------
    // The camera's yaw relative to the body is folded before it aims a
    // strike: a look BEHIND is reflected to the front across the body's
    // left-right axis (150 deg right swings 30 deg right; straight behind
    // swings straight ahead), then the camera may lead the body by `aimYaw`
    // degrees and past that the swing pins at the cone's edge. Pitch is never
    // touched. Applied in game/thirdperson.h ResolveSwingYaw; a cone of 90+
    // leaves only the reflection.
    float aimYaw = TPD(melee, aimYaw);
    // ---- the body serves the swing (game/melee.h WeaponPose torso fields) --
    // Fractions of the stroke's own azimuth/elevation the torso carries, the
    // way `avatar.headLookSpine` shares the look yaw into the chest. 0 = arm
    // only (the pre-overhaul look, and the A/B).
    float torsoShare = TPD(melee, torsoShare);
    float torsoPitch = TPD(melee, torsoPitch);
    // ---- the blade stays out of the wielder's own face ----------------------
    // Metres of clearance beyond the head's own radius the hand-to-tip segment
    // is pushed out to (a rigid translate in RebuildFrame, after the
    // frontal-plane clamp). 0 = clamp OFF — the A/B, same convention as
    // elbowAxisCone's pi.
    float headClearM = TPD(melee, headClearM);
    // ---- ...and the ARM stays out of the wielder's own chest (2026-09-21) --
    // Metres of clearance beyond the torso capsule's own half-width the HAND
    // is pushed out to. The head sphere covered the one body part a BLADE
    // could be swept through and left the one an ARM goes through untouched.
    // 0 = clamp OFF, the same A/B convention.
    float bodyClearM = TPD(melee, bodyClearM);
    // ---- and the lean plane does not chase a reversal (game/melee.h) ------
    // Radians. A commanded turn of the blade's lean plane larger than this is
    // a REVERSAL, not a turn: the travel flipped, and a real blade keeps its
    // lean rather than swapping which side the hilt leads on. The plane holds
    // instead. pi disables it, which is the A/B.
    float leanFlipHold = TPD(melee, leanFlipHold);
    // Metres/sec of TANGENTIAL tip travel below which there is nothing to
    // chase and the lean plane and the roll simply hold. Under it the tangent
    // is numerical dust.
    float leanMinSpeed = TPD(melee, leanMinSpeed);
    // Sine of the angle between the blade and its travel below which the
    // blade's ROLL is held rather than recomputed. The flat is their cross
    // product, so its direction is noise as they approach parallel — every
    // thrust and every stall. 0.20 is 11.5 degrees.
    float flatMinSin = TPD(melee, flatMinSin);
  } melee;

  // ---- combat feel: hit-stop, hit flash, combat cues --------------------------
  //
  // PRESENTATION ONLY, and the distinction is load-bearing rather than tidy.
  //
  // HIT-STOP changes how many fixed 30 Hz ticks the FRAME LOOP runs, never what
  // a tick computes. main.cpp's accumulator already runs the tick loop 0..4
  // times a frame depending on frame time; a dip scales the rate that
  // accumulator fills at, so the sim sees exactly the tick stream it would have
  // seen on a slower machine. No sim value is touched, no hashed state moves,
  // and the headless/selftest paths never execute that loop at all.
  //
  // HIT FLASH is one float per micro-body limb, packed into a word that was
  // already padding (sim/microbody.h MicroBodyInstGpu::pad0) and added at shade
  // time. It is not a material, not a stain, and not in the voxel word — the
  // word is full (CLAUDE.md) and a flash is over in a fifth of a second.
  struct CombatFx {
    // ---- hit-stop -----------------------------------------------------------
    // OFF is a real setting, not a debug escape: some players hate it.
    bool hitStop = TPD(combatfx, hitStop);
    // Three tiers, weakest first. `Scale` is the multiplier on the rate the
    // tick accumulator fills at (0.15 = the world runs at 15% speed); `Ms` is
    // how long the dip lasts in REAL milliseconds, so it is the same length of
    // held breath whatever the frame rate.
    //
    // A CHIP is the blade finding something that is not live flesh — debris, a
    // dropped item, another weapon. FLESH is a mob that voiced being hurt.
    // SEVER is a limb coming off. They are strictly ordered because the dip is
    // peak-held: a sever landing in the same frame as a chip must not be
    // shortened by it.
    float hitStopChipScale = TPD(combatfx, hitStopChipScale);
    float hitStopChipMs = TPD(combatfx, hitStopChipMs);
    float hitStopFleshScale = TPD(combatfx, hitStopFleshScale);
    float hitStopFleshMs = TPD(combatfx, hitStopFleshMs);
    float hitStopSeverScale = TPD(combatfx, hitStopSeverScale);
    float hitStopSeverMs = TPD(combatfx, hitStopSeverMs);
    // ---- hit flash ----------------------------------------------------------
    // Peak additive intensity per tier, in LINEAR HDR before the tonemap (the
    // micro-body pass tonemaps to match the cube path exactly, so the flash has
    // to be added on the linear side or the two paths diverge).
    float flashChip = TPD(combatfx, flashChip);
    float flashFlesh = TPD(combatfx, flashFlesh);
    float flashSever = TPD(combatfx, flashSever);
    // Seconds of halflife on the decay, aged on the TICK (MobSystem::PreTick)
    // so it runs in a gate as well as in the game — and so it slows down with
    // the world under hit-stop, which is right, since the two are describing
    // the same blow. Short: a flash the player can still see when the next blow
    // lands stops reading as a hit and starts reading as a shader bug.
    float flashHalflife = TPD(combatfx, flashHalflife);
    // ---- hit reaction: the body answers, and it answers DIRECTIONALLY -------
    //
    // Hit-stop says "something landed" and the flash says "here". Neither says
    // WHICH WAY, and a blow with no direction in it reads as a light going on
    // rather than as a thing hitting a body. This is the third channel: a
    // struck creature rocks AWAY from the blade's own travel, on a critically
    // damped spring (anim.h SpringDef — Holden's closed form, stable at any
    // dt), and is back on its feet inside a quarter second.
    //
    // SLIGHT IS THE POINT. This is not a stagger and not a knockback: nothing
    // here moves the creature's ORIGIN, so pathing, personal space, the gait's
    // planted feet and every collider stay exactly where they were. It is a
    // pose-space lean the feet absorb, which is why it can fire on every hit
    // without an animation budget or a recovery state machine.
    bool hitReact = TPD(combatfx, hitReact);
    // THE BLOW THIS IS ALL MEASURED AGAINST, in hp at full swing speed. A
    // strike's whole profile (game/impact.h StrikeProfile::Total) over this is
    // the multiplier on every peak below, so a mace shoves harder than a fist
    // because it IS harder, and nothing here has to know a weapon's name.
    // 14 is a sword's cut — the reference blow is "an ordinary sword hit".
    float hitReactRefDamage = TPD(combatfx, hitReactRefDamage);
    // ...and the ceiling on that multiplier, so a freak number in an items.json
    // cannot fold somebody in half.
    float hitReactMaxScale = TPD(combatfx, hitReactMaxScale);
    // Peak lean away from the blow at the reference blow, in DEGREES. The
    // spring is CLAMPED at this, so a cut that lasts four ticks and re-pumps
    // the spring on each of them still leans exactly this far — it just stays
    // leaned while the blade is in the wound, which is what a blade in a wound
    // does.
    // Landed on by eye through `--shot-strike human+sword horizontal_r human`,
    // which is the only instrument for this: a gate can prove the body goes the
    // right way and comes back, and cannot tell you 6 degrees is invisible.
    // It was 6 first, and at 6 the victim of a sword through the chest did not
    // perceptibly move.
    float hitReactLeanDeg = TPD(combatfx, hitReactLeanDeg);
    // ...of which the SPINE takes this share and the root limb the rest. All on
    // the root tips the creature like a signpost; all on the spine leaves the
    // hips unnaturally still. Same distribution law as Mob::ApplyAimPart.
    float hitReactSpineShare = TPD(combatfx, hitReactSpineShare);
    // Peak shove of the root, as a fraction of the creature's OWN HEIGHT, so
    // one number means the same lurch on a rat and on a troll. Horizontal
    // components come from the blow's travel; the vertical one is what makes an
    // overhead blow drive a body DOWN into its knees.
    float hitReactPushFrac = TPD(combatfx, hitReactPushFrac);
    // Peak flick of the STRUCK limb about its own joint, degrees. This is the
    // part that says which arm was hit. Stage 6 clamps it to the joint's
    // authored range like everything else, so it cannot produce a pose the rig
    // says is impossible.
    float hitReactLimbDeg = TPD(combatfx, hitReactLimbDeg);
    // Seconds to halve. The whole reaction is over in about 4x this; past
    // ~0.2 s it stops reading as a flinch and starts reading as a wobble.
    float hitReactHalflife = TPD(combatfx, hitReactHalflife);
    // ---- ...AND THE SAME BLOW ON A BODY JOLT OWNS (2026-09-20) --------------
    //
    // THE OTHER HALF OF THE SAME REACTION, not a second feature. Everything
    // above is a POSE SPRING, and a pose spring is a write nobody reads once
    // the ragdoll owns the rig — which is true of a LIMP LIVING LIMB and of a
    // CORPSE alike (docs/PLAN_struck_matter.md: the axis is how a body is
    // DRIVEN, not whether it is alive). Mob::HitReact has said so since it was
    // written — "a corpse being hit already has an answer for where it goes,
    // and it is a better one than this: the impulse goes into Jolt" — and this
    // is that impulse.
    //
    // kg*m/s at hitReactRefDamage, scaled by the SAME hp x power ramp the
    // flinch uses, so one reference blow moves both halves and a mace shoves
    // harder than a fist because it is harder. Applied at the contact POINT,
    // so a blow off the centre of mass turns a body over instead of sliding
    // it. A human limb is ~3-8 kg, so 12 is roughly a 2-4 m/s kick.
    float hitReactImpulse = TPD(combatfx, hitReactImpulse);
    // ---- combat cues --------------------------------------------------------
    // Volumes are the same 0..N trim every other cue group uses; radius is the
    // audible radius in METRES, matching Tuning::Audio.
    float whooshVolume = TPD(combatfx, whooshVolume);
    // Mouse px/s below which a committed stroke gets no whoosh at all. A cut
    // that barely moved should not sound like one; this is the audio half of
    // the same "speed is the damage" law the sweep runs on.
    float whooshMinSpeed = TPD(combatfx, whooshMinSpeed);
    // Rate multipliers at min speed and at commitSpeed. A faster cut is a
    // higher, tighter whoosh, so the LOW value belongs to the slow stroke.
    float whooshRateSlow = TPD(combatfx, whooshRateSlow);
    float whooshRateFast = TPD(combatfx, whooshRateFast);
    // ---- where the whoosh IS, and how much of that you hear ----------------
    // A whoosh is not a point event: it is the air a blade is STILL moving, so
    // the voice follows the weapon for as long as the sample lasts and a cut
    // from left to right pans from left to right (main.cpp, the audio block).
    //
    // `whooshEdgeFrac` picks the point along the authored edge that is tracked,
    // 0 = the hand, 1 = the tip. Neither end is right: the hand barely moves, so
    // the sound does not travel; the tip is where most of the noise is made
    // (air drag goes with speed cubed) but on a long weapon it swings a metre
    // wide of the player holding it and the pan becomes a gimmick.
    float whooshEdgeFrac = TPD(combatfx, whooshEdgeFrac);
    // How much of the whoosh's position to actually USE, as a fraction of the
    // way from the listener's own ear to the tracked point. 1 = fully
    // spatialized; 0 = pinned to your head, which is effectively mono and is
    // the escape hatch if a swing in your own hands panning across the image
    // reads as wrong rather than as physical. Only the player's own swings go
    // through this; an NPC's whoosh is somebody ELSE's weapon and is always
    // placed where it is.
    float whooshPan = TPD(combatfx, whooshPan);
    float fleshVolume = TPD(combatfx, fleshVolume);
    float clangVolume = TPD(combatfx, clangVolume);
    float strikeEdgeVolume = TPD(combatfx, strikeEdgeVolume);
    float strikeBluntVolume = TPD(combatfx, strikeBluntVolume);
    float cutVolume = TPD(combatfx, cutVolume);
    float cueRadius = TPD(combatfx, cueRadius);   // metres
  } combatfx;

  // ---- grenade ----
  struct Grenade {
    float throwSpeed = TPD(grenade, throwSpeed);   // m/s
    float fuse = TPD(grenade, fuse);          // seconds
    // UpdateGrenade (game/session.cpp): the velocity kept along the axis it
    // hit (bounce), on the other two axes at that contact (friction), and per
    // tick while inside liquid (drag).
    float restitution = TPD(grenade, restitution);
    float friction = TPD(grenade, friction);
    float waterDrag = TPD(grenade, waterDrag);
    int blastRadius = TPD(grenade, blastRadius);
    int blastPower = TPD(grenade, blastPower);
  } grenade;

  // ---- tools ----
  struct Tools {
    int detonateRadius = TPD(tools, detonateRadius);
    int detonatePower = TPD(tools, detonatePower);
    float laserRange = TPD(tools, laserRange);
    int laserMeltRadius = TPD(tools, laserMeltRadius);
    // Carve radius when the beam is on LIVING flesh, in WORLD voxels — float,
    // and deliberately sub-voxel by default. This is the precision dial for
    // surgery: at mob scale 4 a micro voxel is 0.25 world voxels, so 0.3 bores
    // a channel roughly one micro voxel wide, while the same beam still melts
    // a 2-voxel hole in stone. Flesh is cut, not blasted.
    float laserCarveRadius = TPD(tools, laserCarveRadius);
    float laserDamage = TPD(tools, laserDamage);
    // How far ahead (voxels) the brush paints when the crosshair hits nothing.
    float brushAirDistance = TPD(tools, brushAirDistance);
    // The ALCHEMY BENCH's room draught (game/flasksim.h SimConfig::gasWind):
    // peak speed of the slow, wandering breeze that pushes gas about outside
    // the vessels and draws a light gas out of an open mouth, in bench px
    // per gas step (120 gas steps a second). 0 = a still room. Read by the
    // bench every frame, so F5 applies it live.
    float alchemyWind = TPD(tools, alchemyWind);
  } tools;

  // ---- integer sim constants: DETERMINISM-CRITICAL (CLAUDE.md rule 1) ----
  // Emitted into the WGSL prelude as integers. Changing any of these changes
  // the world hash; --selftest must be re-run.
  struct Sim {
    int partGravity = TPD(sim, partGravity);        // 24.8 fixed voxels/tick^2
    int partMaxVel = TPD(sim, partMaxVel);       // 24.8 fixed voxels/tick
    // ---- a voxel in flight, inside a liquid (materials.json "fluid") ----
    // Buoyancy itself is per material (density vs the liquid's); these three are
    // the parts that are a property of the SYSTEM rather than of a substance.
    // ceiling on the buoyant term, 24.8/tick^2 (4 g)
    int partBuoyMax = TPD(sim, partBuoyMax);
    // below this speed a floater looks for a berth
    int partSettleSpeed = TPD(sim, partSettleSpeed);
    // ticks it may hunt before it takes any cell
    int partFloatPatience = TPD(sim, partFloatPatience);
    // density below which things rise
    int airDensity = TPD(sim, airDensity);
    // explosion power lost per cell
    int falloffPerCell = TPD(sim, falloffPerCell);
    // per-mille of destroyed voxels that fly
    int ejectSolid = TPD(sim, ejectSolid);
    int ejectLiquid = TPD(sim, ejectLiquid);
    int ejectPowder = TPD(sim, ejectPowder);
    int ejectGas = TPD(sim, ejectGas);
    // eighths a neighbor must be emptier to flow
    int liquidEqualize = TPD(sim, liquidEqualize);
    // MINIMUM FILM, in eighths. Lateral spread into AIR is repeated halving,
    // and with no floor the halving runs all the way down: one placed water
    // voxel (8 eighths) becomes 8 cells of ONE eighth each, i.e. a puddle
    // eight times the footprint it was placed with and an eighth as deep.
    // Since the renderer draws liquid at fullness-proportional height that is
    // exactly what "I cannot place a single voxel of water, it always splashes"
    // looks like. A cell may now only split into air if BOTH halves land at or
    // above this, so the equilibrium film is minFilm..2*minFilm-1 eighths and
    // the footprint of a placement shrinks by the same factor.
    // 1 is the old behaviour bit-for-bit. Same-liquid EQUALIZE is untouched, so
    // ponds still level; this gates only the leading edge advancing into air.
    int liquidMinFilm = TPD(sim, liquidMinFilm);
    // SUB-VOXEL REPOSE (docs/PLAN_powder_mass.md P5). 1: a resting powder
    // cell sheds eighths toward a neighbour column whose top is more than the
    // material's repose threshold (eighths per cell) lower, and worldgen's
    // loose cover gives its step columns partial tops that already satisfy
    // it. 0: the whole-cell repose tiers only, and whole-cell generated tops.
    int powderFineRepose = TPD(sim, powderFineRepose);
    // critter hop chance = 1/(mask+1) per tick
    int wanderHopMask = TPD(sim, wanderHopMask);
    // THE SOLUTE LAYER (docs/PLAN_solutes.md). 0 = nothing dissolves (the
    // CA's dissolve step is off; mass already in the world still moves and
    // diffuses). 1 = on.
    int soluteMode = TPD(sim, soluteMode);
    // Percent scale on each species' solutes.json `diffusivity`: 0 freezes the
    // pair exchange, 100 is the authored rate. The --sweep reach knob.
    int soluteDiffusion = TPD(sim, soluteDiffusion);
    // THE TEMPERATURE LAYER (docs/PLAN_temperature.md). 0 = off: the layer
    // releases its pages and no heat transition (melt, ignite, freeze) fires.
    int heatMode = TPD(sim, heatMode);
    // How far a heat source reaches, in 2x2x2-voxel blocks (6 = 12 voxels).
    int heatRadius = TPD(sim, heatRadius);
    // How strongly coverage counts: a block's sources reach their own
    // temperature once they fill 1/heatGain of its surroundings.
    int heatGain = TPD(sim, heatGain);
    // HEAT RISES: a source's contribution scaled by direction, one factor per
    // axis -- up when the source is below the block, down when it is above,
    // side off the block's column (a diagonal is the product). Never lifts a
    // block past the hottest source in reach. x16 fixed point in the kernel.
    float heatUpGain = TPD(sim, heatUpGain);
    float heatSideGain = TPD(sim, heatSideGain);
    float heatDownGain = TPD(sim, heatDownGain);
    // The climate above the snowline (the map's treeline - 1), in heat units
    // where 0 is water's freezing point: base +- swing by day / night. Kept
    // below 0 by day (LoadTuning) so the snow caps never melt by themselves.
    int heatSnowlineBase = TPD(sim, heatSnowlineBase);
    int heatSnowlineSwing = TPD(sim, heatSnowlineSwing);
    // THE CHARGE FIELD (docs/PLAN_electricity.md). 0 = off: the field drains
    // and no cell carries charge.
    int elecMode = TPD(sim, elecMode);
    // Rounds a tick; the field crosses one chunk (16 cells) per round.
    int elecRounds = TPD(sim, elecRounds);
    // Potential every charged cell loses per tick (the fade without a source).
    int elecDecay = TPD(sim, elecDecay);
    // Resist of a cell under a full conducting coat (water); thinner = worse.
    int elecWetResist = TPD(sim, elecWetResist);
    // Explosion micro grit: sub-voxel spall thrown alongside the real ejecta.
    // Visual, but spawned BY A SIM KERNEL from the hashed RNG — the roll
    // advances sim state and the droplets can stain, so these are integers in
    // the determinism-critical group and --selftest must be re-run when they
    // change. expMicroScaleIdx indexes microScaleOf's 2/3/4/6 table.
    int expMicroPerMille = TPD(sim, expMicroPerMille);
    int expMicroLifeTicks = TPD(sim, expMicroLifeTicks);
    // 0=2, 1=3, 2=4, 3=6 micro voxels per voxel
    int expMicroScaleIdx = TPD(sim, expMicroScaleIdx);
    // MLS-MPM fluid (sim_fluid.wgsl), HUMAN UNITS: real voxels-and-seconds
    // values, converted to Q16.16-per-tick integers at shader compile time by
    // sim_fluid.wgsl's const-eval block (IEEE-exact, so identical JSON gives
    // identical solver constants everywhere — the deliberate, documented
    // exception to "sim.* is integer-only"). They do NOT touch the world hash
    // (the fluid never writes voxels) but they DO change the fluid_det gate's
    // particle hash, so re-run --selftest after changing defaults.
    //
    // fluidSubsteps is the CFL BUDGET, and the only knob that buys the
    // stiffness above its legality. Everything downstream is derived from it
    // at shader compile time (common.wgsl): FLUID_VMAX = 0.45*substeps
    // cells/tick, FLUID_MARK_PAD = ceil(0.45*substeps) cells, and the
    // per-substep divisors of gravity/viscosity/splash. It is also the whole
    // per-tick price of the solver — the substep table is ~linear in it — so
    // the look and the cost are one number here on purpose.
    // 9 is sqrt(14000)/(30*0.45) rounded up: at 6 the sound speed did not fit
    // and the VMAX clamp engaged on ~575 of 600 bench ticks, which is the
    // "mushy under agitation" regime (plan §1.2 item 1).
    int fluidSubsteps = TPD(sim, fluidSubsteps);
    // EOS stiffness, (vox/s)^2 — the square
    // of a pseudo speed of sound. CHOSEN BY
    // EYE in the fluid lab (2026-08-24) and
    // DELIBERATELY above the CFL cap: 14000
    // -> c = 118 vox/s = 0.66 cells/substep
    // vs the 0.45 FLUID_VMAX ceiling. The
    // WP2 analysis (plan §1.2) says that
    // regime mushes out, and 3600 (0.33
    // cells/substep) is the honest-headroom
    // value — but this pairs with 9x gravity
    // below, which is a fast-water look the
    // owner picked over the physical one.
    // DO NOT "fix" this back without asking:
    // check FA_CLAMPED in --fluid-bench for
    // what the clamp is actually doing, and
    // raise kFluidSubsteps if it engages.
    float fluidStiffness = TPD(sim, fluidStiffness);
    // fall acceleration, voxels/s^2. 98.1 is
    // Earth at 0.10 m voxels; 900 is ~9x,
    // the owner's snappy-water default (see
    // stiffness above — the two go together)
    float fluidGravity = TPD(sim, fluidGravity);
    // Density EOS (grantkot MLS-MPM shape): pressure = stiffness *
    // ((rho/rest)^power - 1), clamped below at -cohesion. rho is sampled from
    // the P2G mass grid each substep, so cramming particles into a cavity
    // builds real ejecting pressure instead of saturating a per-particle J.
    // particle masses per voxel at rest (8 =
    // the 8-per-cell spawn lattice exactly)
    float fluidRestDensity = TPD(sim, fluidRestDensity);
    // integer exponent 1..7; higher = harder
    // incompressibility knee, sharper splashes
    int fluidEosPower = TPD(sim, fluidEosPower);
    // max NEGATIVE pressure, (vox/s)^2.
    // Surface tension: how hard under-dense
    // fluid pulls itself together into blobs.
    // 0 = water's zero-tension default (the
    // EOS floor is then exactly p >= 0);
    // non-zero is the honey/goo authoring
    // surface — plan §5 item 3.
    float fluidCohesion = TPD(sim, fluidCohesion);
    // Species interaction, both (vox/s)^2 and SIGNED. attractSame > 0 pulls a
    // particle toward its own species (blobbing/fusing); attractDiff < 0
    // pushes different species apart (immiscible layers that sit on each
    // other instead of interpenetrating), > 0 encourages mixing. Both 0 by
    // default: negative-pressure terms are the classic sticky-ropes look.
    float fluidAttractSame = TPD(sim, fluidAttractSame);
    float fluidAttractDiff = TPD(sim, fluidAttractDiff);
    // vox^2/s: resists shear via the APIC C
    // matrix. 0 = the owner's default, every
    // shear-damping term off (APIC's own
    // smoothing is the only one left).
    // References run 0.02-0.1; 1.5 was syrup.
    float fluidViscosity = TPD(sim, fluidViscosity);
    // fraction of velocity shed per SECOND
    // (0..20). Non-physical settle aid
    float fluidDamping = TPD(sim, fluidDamping);
    // fraction/s of TANGENTIAL velocity shed
    // while touching solid (gridUpdate's
    // separate BC). 0 = free-slip water;
    // authoring knob for mud/goo.
    float fluidFriction = TPD(sim, fluidFriction);
    // Splash coupling (sim_fluid.wgsl g2p): fluid particles that are FAST and
    // at LOW density (spray, breaking crests) shed PFLAG_MICRO droplets into
    // the ballistic particle system, carrying the species' pour material —
    // so MPM blood spatters stains and MPM water is pure sparkle.
    // droplets/s per eligible particle
    float fluidSplashRate = TPD(sim, fluidSplashRate);
    // vox/s a particle must exceed
    float fluidSplashSpeed = TPD(sim, fluidSplashSpeed);
    // eligible below this x rest density
    float fluidSplashMaxDensity = TPD(sim, fluidSplashMaxDensity);
    // droplet lifetime, seconds (<= 8.5)
    float fluidSplashLife = TPD(sim, fluidSplashLife);
    // droplet size: 0=1/2,1=1/3,2=1/4,3=1/6 vox
    int fluidSplashScaleIdx = TPD(sim, fluidSplashScaleIdx);
    // ---- diffuse material: spray / foam / bubbles ----
    // Ihmsen et al., "Unified Spray, Foam and Bubbles for Particle-Based
    // Fluids" (CGI 2012). Three potentials — trapped air, wave crest, kinetic
    // energy — each clamped to 0..1 by the paper's Phi(), then combined as
    //   n_d = I_k * (kta * I_ta + kwc * I_wc) * dt.
    // Generated particles are classified by local fluid density into spray
    // (ballistic), foam (advected by the fluid, ages out) and bubbles
    // (buoyant, dragged by the fluid) — the classification the paper does by
    // neighbour COUNT, which here is the density the solver already gathered.
    // kta: foam particles/s from trapped air
    float fluidFoamRate = TPD(sim, fluidFoamRate);
    // kwc: foam particles/s from wave crests
    float fluidFoamCrestRate = TPD(sim, fluidFoamCrestRate);
    // Phi thresholds on the convergence-
    float fluidTrappedMin = TPD(sim, fluidTrappedMin);
    //   weighted relative velocity, vox/s
    float fluidTrappedMax = TPD(sim, fluidTrappedMax);
    // Phi thresholds on gated curvature,
    float fluidCrestMin = TPD(sim, fluidCrestMin);
    float fluidCrestMax = TPD(sim, fluidCrestMax);       //   dimensionless
    // Phi thresholds on kinetic energy,
    float fluidFoamEnergyMin = TPD(sim, fluidFoamEnergyMin);
    float fluidFoamEnergyMax = TPD(sim, fluidFoamEnergyMax); //   (vox/s)^2
    // foam lifetime at full potential, s
    float fluidFoamLife = TPD(sim, fluidFoamLife);
    // lifetime at the generation threshold, s
    float fluidFoamLifeMin = TPD(sim, fluidFoamLifeMin);
    // kb: bubble rise, x gravity, upward
    float fluidBubbleBuoyancy = TPD(sim, fluidBubbleBuoyancy);
    // kd: how hard the fluid drags foam and
    //   bubbles toward its own velocity
    float fluidFoamDrag = TPD(sim, fluidFoamDrag);
    // above this x rest -> bubble
    float fluidBubbleDensity = TPD(sim, fluidBubbleDensity);
    // below this x rest -> spray
    float fluidSprayDensity = TPD(sim, fluidSprayDensity);
    // foam particle size (0=1/2 .. 3=1/6 vox)
    int fluidFoamScaleIdx = TPD(sim, fluidFoamScaleIdx);
    // ---- MLS-MPM settle / excite seam: the CA <-> particle handover ----
    // 0 = disturbance-excite off: the CA owns
    // disturbed settled liquid; 1 = settled
    // liquid with air below converts to MPM
    // particles. 1 is what tuning.json ships.
    // This initializer said 0 while the .def said
    // 1 until 2026-09-24; since W2-Q it IS the
    // .def value, so the two cannot drift again.
    int fluidExciteMode = TPD(sim, fluidExciteMode);
    // ---- THE BURST BOUND (WP5) ----
    // Excite is a per-cell trigger with no notion of "only wake what the
    // disturbance can reach", and it emits one particle per eighth of
    // fullness. It is easy to assume the CA liquid fix (merge a2e723e) removed
    // the need for it — dig under a pond and only the water actually falling
    // through the hole should excite. MEASURED, it does not, and the reason is
    // worth keeping: while the CA is draining a body, its partial descent
    // leaves a transient gap under a cell all over the body, not only at the
    // hole. Trigger (a) is "air below", so it fires on every one of them.
    //
    // `--fluid-bench wp5`, `worldlake` (worldgen's authored 347,832-voxel lake
    // at (420,420), a 5x5 shaft opened underneath it once the body is provably
    // asleep), fixed CA, exciteMode 1, ceiling lifted to the pool:
    //   live 352 -> 1,916 (+1t) -> 262,144 (+91t) = the ENTIRE pool, held
    //   there to end of run. Frame p50 69.34  p95 72.28  p99 73.74 ms.
    // That is still the reported "it turns the whole lake into fluid".
    //
    // Both knobs are in PARTICLES (8 per water voxel), and both apply ONLY to
    // excite — never to explicit spawns. Pouring water with the mpm tool is
    // something the player asked for; a lake converting itself is not, and the
    // two must not share a budget or the tool stops working next to water.
    // most excited particles the seam will
    // hold at once = 1,000 water voxels in
    // motion, a 10-voxel cube. `--fluid-bench
    // wp5b`, worldlake, same puncture, and
    // the last column is the acceptance
    // criterion frame time cannot express —
    // eighths that reached the sealed chamber
    // in the 400 ticks after the plug:
    //  ceiling   p50    p95    p99   drained
    //   CA only 15.00  15.87  17.12   70,743
    //    4,000  23.43  24.93  26.21   73,672
    //    8,000  25.05  27.13  28.60   72,996
    //   16,000  27.10  28.82  29.86   74,572
    //   32,000  33.23  34.80  36.22   75,599
    //  262,144  69.34  72.28  73.74  102,402
    // Drain THROUGHPUT is flat from 4k to 32k
    // — the CA is doing the transport in all
    // of them (71,479 of the 73,672 at 4,000
    // arrived as settled voxels). So the
    // ceiling buys nothing but COVERAGE: how
    // much of the body is visibly in motion.
    // 8,000 is the cheapest value that still
    // clears the largest peak any scene
    // reaches unforced (the pond's own 5,700),
    // so it never clips a body that was not
    // going to burst anyway. It is a LOOK
    // knob above that — raise it for a wider
    // churn at ~1.2 ms of frame per 4,000
    int fluidExciteCeiling = TPD(sim, fluidExciteCeiling);
    // most particles converted per TICK. Does
    // not bind at any shipped ceiling (the
    // worldlake ramp to 8,000 takes 46 ticks,
    // ~174/tick); it is the guard for the case
    // the ceiling cannot cover — a blast that
    // exposes thousands of cells at once with
    // the ceiling raised. Deliberately equal
    // to kMaxFluidSpawnsPerTick: the seam may
    // not convert world water faster than the
    // MutationQueue can pour it
    int fluidExciteRate = TPD(sim, fluidExciteRate);
    // 1 = excite also takes water PERCHED on
    // terrain (a base cell with a diagonal
    // void beside it), not only water with
    // air directly below.
    //
    // OFF, and that is a measured reversal of
    // WP3's expectation. The trigger was for
    // water the CA had parked on a slope, and
    // the CA no longer parks water on slopes.
    // `--fluid-bench wp5` / `wp5b`, perch 1 vs
    // 0, everything else equal:
    //   pond68     candidates 1,150 vs 1,150
    //   worldlake  candidates 169,616 vs
    //              169,616, emitted 35,158 vs
    //              35,158, p50 33.23 vs 33.13
    //   hill       basin capture 50.9% vs
    //              51.7% (ceiling lifted, so
    //              excite is actually live)
    // Byte-identical on both settled-water
    // scenes; on the ramp it is a fraction of
    // a point and in the WRONG direction. It
    // costs 24% of seam time (fluidSeam 0.230
    // -> 0.174 ms on pond68) for nothing.
    //
    // EXCITE SIDE ONLY. settleCheck's
    // stability veto evaluates the full
    // predicate whatever this says, because
    // settle refusing MORE than excite takes
    // is the safe direction of the hysteresis
    // — water stays particles a while longer —
    // and the reverse lets settle create a
    // configuration excite immediately tears
    // up again
    int fluidExcitePerch = TPD(sim, fluidExcitePerch);
    // SURFACE DISTURBANCE, in whole cells.
    // A settled liquid cell whose own water
    // surface stands this many cells or more
    // above the water surface in a lateral
    // neighbour's column is a SPLASH sitting
    // on a pool, and goes to the solver so it
    // falls with momentum and throws a wave,
    // instead of relaxing in place as a mound.
    // 0 disables the trigger.
    //
    // Measured in CELLS against the surface,
    // not in eighths against the neighbouring
    // cell, and that is the whole design. The
    // eighth-level version is trigger (c) from
    // plan §6, which was measured twice and
    // rejected both times: a settled pool
    // carries a couple of eighths of shot
    // noise column to column, and bottom
    // packing puts a deeper column's top cell
    // beside a shallower one's empty cell, so
    // "2 eighths lower" is true at the surface
    // of every pool that is not perfectly
    // level. A whole-cell step in the SURFACE
    // is above that noise floor by
    // construction: two columns that differ by
    // a few eighths have surfaces in the same
    // cell or one apart, never two.
    //
    // The trigger only looks over WATER — the
    // neighbour column must itself hold liquid
    // — so a puddle spreading across dry
    // ground never fires it. It is a lakebed
    // disturbance trigger, not a spill one.
    //
    // EXCITE SIDE ONLY, like fluidExcitePerch
    // above, and this is the UNSAFE direction
    // of that asymmetry: settle can in
    // principle rebuild a 2-cell step that
    // excite then tears up again. What makes
    // it hold in practice is that the CA
    // flattens such a step by itself (partial
    // descent takes it straight down), so the
    // configuration does not persist for
    // either side to fight over, and the calm
    // window throttles settle retries
    // regardless. If a pool is ever seen
    // churning at rest, set this to 0 first —
    // that is the differential.
    int fluidExciteStep = TPD(sim, fluidExciteStep);
    // ---- settled liquid as MPM boundary mass ------------------------------
    // WP4 shipped the two representations passing straight THROUGH each other:
    // sim_fluid.wgsl's fluidSolid() blocks solids and powders only, and a
    // settled water voxel contributes no node mass, so an MPM waterfall poured
    // onto a full basin fell to the floor as if the basin were empty and the
    // basin never noticed. The same hole is why a partly-settled pool sprays:
    // the instant one chunk converts to voxels its neighbours lose the density
    // that was holding them up and collapse sideways into it.
    //
    // This is the fix, and it is the standard static-boundary treatment: a
    // settled liquid cell seeds its node with `fullness/8 * restDensity` of
    // ZERO-VELOCITY mass before P2G runs. Pressure then supports particles on
    // the pool surface (they float instead of tunnelling), and the momentum
    // divide in gridUpdate dilutes an impacting jet against static mass, which
    // is the drag a real pool applies. Deliberately NOT a prescribed-zero
    // boundary: leaving the node velocity as (real momentum / total mass) is
    // what keeps the WAKE trigger alive at the impact point — a splash still
    // excites the water it lands on, it just no longer excites the whole lake.
    //
    // 1.0 = a full voxel reads exactly rest density. 0 restores WP4 exactly and
    // is the live A/B oracle for anything this changed. Above ~1 the boundary
    // over-pressurises and ejects particles off the surface.
    float fluidSettledMass = TPD(sim, fluidSettledMass);
    // SUBMERGED settled liquid is a boundary, so
    // particles ride the free surface instead of
    // sinking into water that has no depth
    // profile to push them back out. A buried
    // particle can never settle (its column has
    // no room), so this is what makes settle able
    // to terminate. 0 = the pass-through control
    int fluidSubmergedSolid = TPD(sim, fluidSubmergedSolid);
    // vox/s: a fluid block whose FASTEST
    // particle stays below this for
    // settleTicks in a row counts as calm and
    // may settle back into CA voxels.
    // STILL SCALES WITH GRAVITY, and the reason
    // is NOT the one an earlier WP3 revision
    // gave. That revision blamed the solver's
    // free-surface gravity bias (a surface node
    // has no pressure, so it carries exactly
    // gravity/substeps forever) and expected
    // that stripping the bias — seamRestVy in
    // sim_fluid_seam.wgsl — would let 0.9 come
    // back and make the knob g-independent.
    // Measured: it does not. The bias is only
    // 3.3 vox/s at 900/9, and what actually
    // sets the floor is the genuine turbulence
    // of a 9x-gravity scene. Sweep on the lab
    // basin, 400 ticks, eighths still live at
    // the end (lower = more settled), with
    // wakeSpeed held at 4x:
    //   eps 0.9 -> 15,359   (nothing settles)
    //   eps 2.7 ->  7,878
    //   eps 4.0 ->  5,548
    //   eps 6.0 ->  3,438
    // and settle<->wake thrash falls the same
    // way (re-excited/settled 100% -> 38%), so
    // a LOWER threshold is worse on both axes,
    // not a safer trade. 6.0 = 0.2 cells/tick =
    // 0.6 m/s: a drift, not a motion. The
    // excite-stability test in settleCheck is
    // what guards the RESULT; this knob only
    // decides when to ask it.
    float fluidSettleEps = TPD(sim, fluidSettleEps);
    // vox/s: grid-node speed at an active/
    // settled interface above this excites the
    // neighbouring settled liquid (progressive
    // wake). Keep ~4x settleEps: the gap is the
    // hysteresis
    float fluidWakeSpeed = TPD(sim, fluidWakeSpeed);
    // consecutive calm ticks before a block
    // settles. 24 beats the old 45 by 2x on
    // settled mass with the bias stripped too
    // (lab hill at exciteMode 0: 2,131 standing
    // eighths at 24 against 1,071 at 45), so
    // this half of the trio is confirmed, not
    // inherited. The >= 8 floor is a HARD
    // requirement: it covers the CPU-side
    // page-materialization readback latency, so
    // the settle converter never writes voxels
    // into a chunk the mirror has not seen
    int fluidSettleTicks = TPD(sim, fluidSettleTicks);
    // ticks a chunk slot may hold particles
    // before its blocks are force-settled
    // regardless of calm. 0 disables the
    // backstop entirely. Keyed on EXISTENCE, not
    // on refusal: a submerged block is never
    // calm, so it is never picked, so a
    // refusal-triggered age would never fire
    int fluidStuckTicks = TPD(sim, fluidStuckTicks);
    // forced blocks per tick; bounds the drain
    // rate, and forced picks take a stricter
    // (x,z)-column exclusion because their write
    // set reaches past SETTLE_SPILL
    int fluidForceBlocks = TPD(sim, fluidForceBlocks);
    // cells past the spill ceiling a forced walk
    // may climb looking for room. Exhausting it
    // means a sealed column — counted, not
    // silently retried
    int fluidForceReach = TPD(sim, fluidForceReach);

    // ---- water bodies (docs/PLAN_water_master.md; src/sim/waterbody.h) ----
    //
    // A still lake gets a NAME and a record of aggregates — level, surface cell
    // count, volume in eighths, a drain ledger — so that draining it becomes
    // arithmetic on that record instead of pressure propagating cell by cell
    // through 87,000 voxels. At M1 the record exists and does nothing; the
    // ledger and the surface shave that spend it arrive with M2.
    //
    // Every value here is an integer (rule 1). Components 6 and 8 need genuinely
    // physical quantities (a discharge coefficient, a circulation) and those go
    // in the sanctioned human-unit float lane beside sim.fluid*, with them.

    // THE OFF SWITCH, and the reason this subsystem could land at all. At 0
    // nothing is built, nothing is labelled and nothing is classified, so
    // `--sweep sim.waterBodyMode=0,1` reports one hash and the pinned world is
    // provably untouched. It stays 0 until a milestone that moves the hash
    // arrives with its own rebaseline commit.
    int waterBodyMode = TPD(sim, waterBodyMode);

    // ENTER / EXIT VOLUME, in EIGHTHS (the CA's state nibble is eighths, so the
    // whole ledger is). Small ponds are cheap to simulate honestly AND the
    // level model's error is relatively largest there, so this threshold is a
    // correctness argument before it is a performance one.
    //
    // The default admits a body of ~8,192 whole voxels — well under the
    // smallest natural tarn (~87,000 voxels at radius 48) and well over any
    // puddle. The exit sits at HALF the enter value, and the gap is the point:
    // a body oscillating across one shared threshold would change
    // representation every tick, and every change is a seam crossing where mass
    // can be lost.
    int waterBodyMinVolume = TPD(sim, waterBodyMinVolume);
    int waterBodyExitVolume = TPD(sim, waterBodyExitVolume);

    // There is no SURFACE-SPREAD adopt/release pair. `waterBodySpreadEnter` /
    // `waterBodySpreadExit` were written for it at M1 (681dd72) and deleted
    // 2026-09-24 because nothing ever read them: only closed analytic basins
    // are registered, so a stream never reaches the ladder, and the spread the
    // GPU now measures (sim_waterbody.wgsl rvLo/rvHi) feeds only the wave's
    // sleep. `--gate waterbody` bounds the settled spread from baseline.json.

    // How long every enter test must hold before adoption. Ticks — 30 is one
    // second. A body still sloshing has a surface that is not an equipotential,
    // and adopting it would freeze that transient into a `level`.
    int waterBodyQuietTicks = TPD(sim, waterBodyQuietTicks);

    // Rule 2's bound on the whole feature. At the cap the SMALLEST candidate is
    // refused, and refusal is a safe degradation: unadopted means "simulated the
    // way it is today", never "lost". Hard-capped at kWaterBodyCap (waterbody.h)
    // because the descriptor array's size is a GPU layout from M2 onward.
    int waterBodyMaxCount = TPD(sim, waterBodyMaxCount);

    // THE M2 TEST TAP, eighths per tick per governed body. M2 lands the ledger
    // and the shave but not the discharge law (component 6 is M3), so this is
    // what gives the ledger something to be exact ABOUT: it opens a hole of a
    // known size in every governed lake and `--gate waterbody` pass A conserves
    // across it.
    //
    // 0 in every shipped world, and 0 is load-bearing twice over: it is what
    // keeps the surface shave from ever firing (so a labelled lake still
    // sleeps, pass E) and it is what tells SubmitTick not to declare the
    // footprint to the page table (so a labelled lake materializes no pages).
    // Idle cost is zero rather than small, and that is a property of this knob
    // being the only drain source rather than of a threshold.
    int waterBodyTestDrain = TPD(sim, waterBodyTestDrain);

    // ---- THE DISCHARGE LAW (component 6) and THE LOCAL EXCITE (7), M3 ----
    //
    // A hole is an orifice and a lake behind it is a head: Q = Cd*A*sqrt(2 g h).
    // ONE evaluation of `h` (in sim_waterbody.wgsl's wbLedger) produces both the
    // emitted particle momentum and the ledger debit — emit by one rule and
    // decrement by another and the pair is a mass pump under every edge case.
    //
    // THE BOUND, eighths per hole per tick, and it is rule 2 rather than taste:
    // the CPU reserves exactly kWaterDrainOpsPerBody spawn-op slots per body, so
    // this is clamped to that and the ledger can never publish an emission the
    // op block cannot hold. When the cap binds, the debit is what was ACTUALLY
    // written (discipline 3.2) — capping slows a drain, it cannot lose an eighth.
    int drainMaxEighthsPerTick = TPD(sim, drainMaxEighthsPerTick);
    // COMPONENT 7's v1 radius, world cells. The shell is the free-surface disc
    // at the body's level plus the throat column over the hole — NOT a solid
    // ball: plan §9 ranks the ball's ~33,000 particles against a ~40,000
    // measured envelope as the second-most-likely way this work fails. 0
    // disables the shell and is an exact identity (no cell can satisfy it).
    int drainExciteRadius = TPD(sim, drainExciteRadius);
    // The two PHYSICAL quantities, human-unit floats in the sanctioned
    // sim.fluid* lane — const-eval'd to fixed point at the top of
    // sim_waterbody.wgsl, so the kernel stays integer (rule 1).
    float drainCd = TPD(sim, drainCd);        // orifice discharge coefficient
    // vox/s^2; should match sim.fluidGravity
    float drainGravity = TPD(sim, drainGravity);

    // ---- RELEVEL (docs/PLAN_water_relevel.md W1) --------------------------
    //
    // Why this exists at all, in one line: the CA's equalize branch fires at a
    // 2-eighth difference between neighbours, so a ramp of 1 eighth per 2 cells
    // is a STABLE FIXED POINT — a 5-voxel cone over an 80-cell radius never
    // goes away, and above that slope it flattens by diffusion at r^2 ticks per
    // eighth, which is minutes. Flatter than reach 1 allows needs a global
    // operation, and the body system already is one.
    //
    // Eighths a column may move per tick. 0 is an EXACT IDENTITY: the two new
    // passes return after their first comparison, no voxel is written, and the
    // pinned world hash is the pre-W1 one. 4 flattens a 10-voxel cone in ~20
    // ticks (0.7 s); 8 does it in 10 and starts to read as an edit rather than
    // as water finding its level.
    int waterRelevelMax = TPD(sim, waterRelevelMax);
    // Eighths of deficit per EXTRA eighth of rate: k = clamp(|s - m| / gain, 1,
    // max). A column one voxel down moves at 1 eighth/tick, a column four
    // voxels down at 4 — so a crater closes fast and a one-eighth ripple does
    // not get bulldozed.
    int waterRelevelGain = TPD(sim, waterRelevelGain);
    // How far below the body's level the measure looks, in VOXELS. This sizes
    // nothing at runtime — the histogram block is sized from world.h's
    // kWaterRelevelDepthMax and this is clamped to it — but it does decide how
    // deep a hole still counts as "a low column of this lake" rather than as a
    // void the MPM owns. A column further down than this clamps into the end
    // bucket and is treated as deep.
    int waterRelevelDepth = TPD(sim, waterRelevelDepth);

    // ---- W2: surface momentum (docs/PLAN_water_relevel.md §4) ----
    //
    // The relevel RELAXES: every column moves toward the body's mean and stops.
    // A real pond OVERSHOOTS — the water beside a crater accelerates into it,
    // arrives with momentum, piles past level and rings back. That needs one
    // extra integer per column FACE and nothing else new: four non-negative
    // outflow pipes per column (the Mei/O'Brien virtual-pipe layout), living in
    // `world.waterFlux`.
    //
    // 0 = OFF and it is an EXACT IDENTITY, stronger than a cheap kernel: the
    // `waterFlux` pass row is not recorded at all (Cond::WaterWave), so the
    // pinned world hash cannot see the feature. 1 = the pipe layer is live.
    int waveMode = TPD(sim, waveMode);
    // Gravity for the pipe acceleration, in VOXELS/s^2 — the sanctioned
    // human-unit float lane, exactly like `drainGravity`, and converted to Q8
    // cells/tick^2 by WGSL const-eval at the top of sim_waterbody.wgsl so the
    // kernel itself stays integer and bit-deterministic (rule 1).
    //
    // 98 is real gravity at kVoxelsPerMetre = 10. The wave speed is
    // sqrt(g * depth); at the shipped depth cap of 10 voxels that is ~1.04
    // cells/tick, which is the CFL limit for a reach-1 scheme — raise either
    // and the ring outruns the grid instead of travelling on it.
    float waveGravity = TPD(sim, waveGravity);
    // VOXELS of depth the wave speed may see. A deep lake's waves would
    // otherwise travel faster than one cell per tick, which a reach-1 update
    // cannot represent; capping the depth caps the speed and reads fine,
    // because what the eye follows is the ring, not its absolute celerity.
    int waveDepthCap = TPD(sim, waveDepthCap);
    // How fast the ring dies, per SECOND. A pipe keeps (1 - damping/30) of its
    // flux each tick, so 0.8 is an e-folding time of ~1.25 s: a crater rings
    // three or four times and is gone. 0 never settles and is what the sleep
    // epsilon exists to make safe anyway; above ~8 the overshoot is invisible
    // and this is just a slower relevel.
    float waveDamping = TPD(sim, waveDamping);
    // The Q8 pipe magnitude a body must be STRICTLY UNDER to count as still.
    // The shipped 256 is not a tolerance: a pipe transfers `q >> 8` whole
    // eighths, so under 256 it moves literally nothing. A body that is still
    // AND flat for `fluidSettleTicks` consecutive ticks publishes flux-asleep,
    // and both W2 passes then return after three loads — which is what keeps a
    // settled lake inside its hot window at zero cost (rule 2). Measured on the
    // harness lake: asleep 112 ticks after a 17x17x6 crater.
    int waveSleepEps = TPD(sim, waveSleepEps);

    // ---- W3: what disturbs the surface (PLAN_water_relevel.md §5) ----
    //
    // Three sources of a disturbance the head difference alone cannot produce,
    // each behind its own knob and each an EXACT IDENTITY at 0. All three are
    // Q8 pipe flux — the unit the pipes already carry — so none of them needs
    // the human-unit float lane and none of them puts an f32 in a kernel.
    //
    // Peak flux a blast adds to the column under its centre, falling linearly
    // to nothing at the explosion's own radius. One tick, added before the
    // outflow clamp, so a blast cannot move more water than the column is
    // allowed to give. 3072 is twelve whole eighths per tick at the centre —
    // three times the shipped relevel rate, which is what makes the crater
    // punch a visible bowl rather than merely smooth one.
    //
    // At 0 the CPU emits no impulse record at all, so TickParams'
    // `waterImpulseCount` stays 0 and `wbFlux` is bit-identical.
    int waveBlastImpulse = TPD(sim, waveBlastImpulse);
    // Q8 flux a LIVE DISCHARGE pulls its neighbours in with, so the surface
    // genuinely dips toward the throat instead of the render vortex sitting
    // over a flat lake. GPU-generated, because "this body is emitting" is a
    // fact only the ledger knows (WBS_EMIT) — the CPU could learn it only from
    // the async readback, which is rule 1 through the back door.
    int waveDrainSink = TPD(sim, waveDrainSink);
    // Q8 flux a swimmer drags behind them, per whole voxel-per-tick of their
    // own submerged speed. Small on purpose: a wake is a trail, not a wave, and
    // a body that shoved the surface as hard as a blast would let a player pump
    // a lake by swimming in circles. Bounded anyway by the outflow clamp.
    int waveSwimWake = TPD(sim, waveSwimWake);

    // ---- W-D: discovery (docs/PLAN_water_relevel.md §8) ----
    //
    // A body the PLAYER creates — a basin dug and filled by hand, a pool a
    // drain leaves behind — is adopted and gets the W1 relevel; a puddle stays
    // CA and costs nothing. The CPU accounts placed-liquid EVIDENCE off the
    // mutation stream and proposes a probe disc; the GPU measures the real
    // water and adopts or refuses it (the M2 authority split, verbatim).

    // EIGHTHS of liquid placed in one cluster of the coarse evidence grid
    // before a probe disc is raised. 4096 is ~512 full voxels of water — a
    // small real pond, and three orders more than anything a bucket or a
    // burst pipe leaves behind. 0 is an EXACT IDENTITY: no evidence is
    // accumulated, no probe exists, no descriptor carries WBF_DISCOVER, and
    // the pinned world hash is the pre-W-D one.
    int waterDiscoverMinEighths = TPD(sim, waterDiscoverMinEighths);
    // MEASURED free-surface CELLS below which the GPU refuses a discovered
    // probe and parks it in WB_REFUSED (four ledger loads a tick, no footprint
    // work). This is the backstop behind the CPU filter above, for evidence
    // that evaporated, soaked away or ran off before the reduce ever looked:
    // the eighths were genuinely placed, and there is still no body there.
    // Never applied to an AUTHORED basin — see TickParams::waterAdoptMinArea.
    int waterAdoptMinArea = TPD(sim, waterAdoptMinArea);

    // ---- wind coupling (docs/RESEARCH_wind.md §4.5/§4.6) ----
    // The SHAPE of the field is the `wind` group below; these are what the
    // three SIM consumers do with what they sample. Human-unit floats, the
    // sim.fluid* exception and by the same mechanism — each is const-eval'd to
    // an integer at the top of the kernel that reads it, so no f32 ever
    // reaches a sim kernel (rule 1).

    // THE GATE, and the only knob in this file that can move the pinned world
    // hash. 0 = no sim kernel evaluates wind at all; 1 = drift (particles, MPM
    // spray, the CA's bias on matter that is already moving); 2 = also
    // settled-powder entrainment. See kWindMode* in world.h for what each step
    // promises about rule 2 — 2 is deliberately NOT rule-2 clean yet and is
    // there to be looked at, not shipped.
    int windMode = TPD(sim, windMode);
    // ---- gas particles (docs/PLAN_gas_particles.md stage 1) ----
    // THE EDGE. 0 = wall: `gasLeave` never fires and a gas voxel pressed
    // against the residency boundary spreads along it, which is the top-plane
    // sheet that used to hold up to 1,024 chunks awake over a big fire. 1 =
    // sink: it becomes a parcel that keeps rising and drifting outside the
    // window under the same buoyancy/wind model, bounded by its authored decay
    // and an outer box, and reconverts to a voxel if it drifts back in.
    //
    // At 0 the CPU records NO gas pass (Cond::Gas is false) and the kernel
    // branch is never reached, so this is an exact identity in the windMode /
    // waterBodyMode sense rather than merely a cheap path.
    int gasMode = TPD(sim, gasMode);
    // THIN SMOKE DISSIPATES (sim_step.wgsl gThinMul). A buoyant gas voxel that
    // emits no heat and has at most gasThinNeighbors gas voxels on its six
    // faces is THIN; its decay-to-air rules roll at gasThinDecayMul x their
    // authored chance. The plume's core is untouched, the haze it sheds fades
    // sooner -- which is most of a big fire's awake chunks and raymarch media.
    // 1 = off, compiled out: today's world and hash.
    int gasThinDecayMul = TPD(sim, gasThinDecayMul);
    int gasThinNeighbors = TPD(sim, gasThinNeighbors);
    // Ballistic debris and spray: fraction of the gap between a particle's
    // velocity and the local wind that closes per SECOND, at a material's full
    // windResponse of 15. A drag law rather than a push, because drag is
    // self-limiting — a particle accelerates toward the wind and then stops,
    // so no gust can fling debris faster than the air is moving, whatever the
    // knob says. That bound is why this can be a plain multiplier and does not
    // need a budget.
    float windDrag = TPD(sim, windDrag);
    // MPM grid nodes: how much of the field a fully exposed node feels, as a
    // fraction. Below 1 because a fluid surface is not a free particle — it is
    // dragged by the air, not carried.
    float windFluidGain = TPD(sim, windFluidGain);
    // ...and which nodes count as exposed: node mass, as a fraction of the mass
    // a node deep inside fluid at rest density carries. Wind fades to nothing
    // as a node approaches this, so it acts on spray and the top skin of a pool
    // and not on its body. Full-body wind on a pond reads as a CURRENT, which
    // is a different phenomenon and the wrong one (research doc §8's open
    // question, answered here in favour of low-mass-only).
    float windFluidMass = TPD(sim, windFluidMass);
    // CA drift bias: the wind speed at which a full-response material reaches
    // the maximum bias probability, and that maximum. The bias only reorders
    // the direction candidates a moving voxel already tries, so the cap is what
    // keeps wind from becoming a second gravity — at 0.5 a gale still leaves an
    // even chance of the ordinary random order, which is what keeps smoke
    // looking like smoke rather than like a conveyor.
    float windDriftSpeed = TPD(sim, windDriftSpeed);
    float windDriftMax = TPD(sim, windDriftMax);
    // Entrainment (windMode 2): the per-axis wind speed that just lifts a grain
    // whose windFriction is 1; the threshold scales with the authored nibble,
    // so friction 4 needs four times this. Bagnold's fluid threshold, authored.
    // 1.2 m/s (was 2 until 2026-09-30): sand (friction 5, derived from its
    // density) lifts at 6 m/s AT THE GRAIN, the observed 5-7 m/s near the
    // ground; the height profile puts a grain at ~0.3x the reference wind.
    float windEntrainSpeed = TPD(sim, windEntrainSpeed);
    // ...and how often a grain over that threshold actually hops, in chances
    // per second. This is the bound (rule 2): entrainment is a rate, not a
    // certainty, so a dune creeps instead of exploding.
    float windEntrainRate = TPD(sim, windEntrainRate);
    // THRESHOLD PLUS POWER (2026-09-30). Saltation does not switch on at a
    // threshold and run at a flat rate: sand flux grows roughly with the CUBE
    // of the excess over the threshold. The hop chance is windEntrainRate x
    // min(excess / (threshold x windEntrainSpan), 1)^windEntrainPower, so a
    // wind just over the line barely creeps and one at (1 + span) x the line
    // runs at the full rate. Still bounded by the rate (rule 2).
    float windEntrainPower = TPD(sim, windEntrainPower);
    float windEntrainSpan = TPD(sim, windEntrainSpan);

    // ---- wind drafts: the shelter volume (docs/RESEARCH_wind.md 14) ----
    // THE GATE. 1 = the ambient field is redirected by the geometry in a box
    // round the player (sim_draft.wgsl): sealed rooms are still, a room with
    // two openings carries a draft between them. 0 = no draft row recorded and
    // every consumer reads the ambient field, an exact identity. Moves the
    // world hash whenever gas or debris moves near a structure.
    int draftMode = TPD(sim, draftMode);
    // The solver's FIXED iteration counts: red-black SOR sweeps of the coarse
    // grid (one cell per chunk, solved in workgroup memory), and sweeps per
    // fine pass (three overlapping-tile passes over the 0.4 m grid). More =
    // closer to the exact potential flow and a costlier solve tick (a solve
    // runs only when blockers in the box change).
    int draftCoarseSweeps = TPD(sim, draftCoarseSweeps);
    int draftFineSweeps = TPD(sim, draftFineSweeps);

    // ---- dev force multipliers, one per TIER ----
    // NO_WGSL rows in tuning_params.def, and that is deliberate rather than
    // an omission: a WGSL row becomes a const-folded shader constant and
    // needs F5. These two ride TickParams as Q8 integers instead, because they
    // exist to be DRAGGED: a slider you have to reload a shader to see is a
    // slider nobody moves. (`sim.fluidExciteMode` carries a WGSL name it does
    // not use and rides the tick stream anyway; that is the wart, not this.)
    //
    // WHY TWO, AND WHY THEY SCALE DIFFERENT QUANTITIES. The engine's own split
    // (research doc §4.6) is CA tier vs particle tier, so the sliders are that
    // split. What "more force" means is not the same on both sides:
    //   * gas scales the CA drift-bias PROBABILITY, past its windDriftMax cap,
    //     to certainty. Scaling the velocity there would die at ~2x, because
    //     the bias ramp already saturates near the default weather — a control
    //     that goes dead halfway is worse than none.
    //   * particle scales the wind VELOCITY that debris, spray and MPM nodes
    //     are dragged toward, which is what actually throws them further. It
    //     is also the only way past the drag law's own ceiling, since a
    //     particle cannot outrun the air however hard it is dragged.
    // Both are 1.0 by default and every consumer takes an exact-identity
    // early-out at exactly 1.0, so the pinned world hash cannot move until a
    // slider does. Off 1.0 they are still fully deterministic — integers on
    // the tick input stream, captured by replays and the twice-run gate.
    float windGasScale = TPD(sim, windGasScale);
    float windPartScale = TPD(sim, windPartScale);
    // ...and the third thing on that stream, which is not a multiplier: the
    // wind speed (m/s) at which windDrag above applies IN FULL. Below it the
    // drag RATE ramps linearly with the local wind, which is what keeps a calm
    // day ballistic — a fixed rate is an atmosphere that resists a falling
    // ember as hard when nothing is blowing as it does in a gale, and it made
    // terminal fall 0.86 vox/tick against a 6 vox/tick cap the day wind
    // shipped. Read the curve off windDragRampQ (common.wgsl): at the default
    // 40 the 6 m/s weather falls at 5.7 vox/tick and only a named storm looks
    // floaty; drop it to 20 and ordinary weather already halves the fall.
    //
    // NO_WGSL for the windGasScale reason — this is the knob you drag WHILE
    // watching an explosion, and one that needs F5 between each nudge cannot
    // be judged by eye.
    float windDragRef = TPD(sim, windDragRef);
    // ---- the wind primitive wake budget (docs/RESEARCH_wind.md §4.3) ----
    // Chunks a tick may WAKE across every wind primitive holding the
    // entrainment licence. This is the rule-2 budget for the whole feature and
    // the one number that decides whether a fan is free.
    //
    // It is a BUDGET, not a switch: primitives are served in list order and
    // once it is spent the rest are refused (and counted — a silently trimmed
    // footprint would read as "entrainment is flaky"). A refused primitive
    // still blows; it just cannot pick settled matter up this tick.
    //
    // NO_WGSL for the windGasScale reason: it is read CPU-side per tick, so a
    // gate can set it with no shader reload.
    // Clamped to kWindWakeCap, which is the TickParams array it fills.
    int windWakeChunks = TPD(sim, windWakeChunks);

    // ---- HEAT UPDRAFTS + THE STACK EFFECT (wind phase 5) ----------------
    // docs/RESEARCH_wind.md §4.4; common.wgsl HEAT UPDRAFTS. Where the
    // temperature layer's excess X is above zero the sim's wind gains a lift
    // of windUpdraftGain m/s per 100 heat units (looked for up to 32 voxels
    // below the sample, so a plume stands ~4 m over its fire), capped at
    // windUpdraftCap, plus an inflow toward the hotter side of
    // windUpdraftInflow per unit of lift gradient. Inside the draft volume the
    // lift is projected onto what the walls allow (sim_draft.wgsl's third
    // right-hand side) and windStackGain scales that correction: a room with a
    // low and a high opening draws air in at one and vents at the other.
    // GPU only -- debris, corpses and trees (the CPU wind mirror) do not feel
    // it. NO_WGSL: SubmitTick converts them into TickParams integers, so the
    // F1 sliders are live. Gain 0 is the exact pre-phase-5 field.
    float windUpdraftGain = TPD(sim, windUpdraftGain);
    float windUpdraftCap = TPD(sim, windUpdraftCap);
    float windUpdraftInflow = TPD(sim, windUpdraftInflow);
    float windStackGain = TPD(sim, windStackGain);

    // ---- THE CURRENT FIELD (docs/PLAN_water_master.md component 8) --------
    //
    // ALL OF THESE ARE CPU-SIDE, and that is not the windGasScale exception —
    // it is what this system's shape makes correct. The shader never reads a
    // current knob: `CurrentPrimSystem` resolves the knobs into PRIMITIVES on
    // the CPU once a tick and ships the resolved list, exactly the way
    // `WindPrimSystem` does. A TUNE_CURRENT_* constant would be a second,
    // never-read copy of a number, and the one thing this repo's tuning
    // pipeline is designed to prevent is two places that must agree.
    //
    // The wave knobs are the opposite case — raymarch.wgsl evaluates the
    // Gerstner sum itself — so those are WGSL rows of the .def.
    //
    // THE OFF SWITCH. `currentMode` 0 must be bit-identical to a build without
    // this feature, and it is by construction: currentAtQ returns the zero
    // vector before reading anything else, so no sim kernel can see the field.
    // The RENDER arm does not consult it (a renderer cannot write a voxel), so
    // the look ships on with the hash pinned.
    //   0 = no sim kernel evaluates the current field
    //   1 = MPM fluid nodes (sim_fluid.wgsl) and debris floating in or on a
    //       liquid (sim_particle.wgsl) are dragged by it
    //
    // The debris arm is an EXACT identity at mode 0 for a second reason on top
    // of currentAtQ's early return: it re-aims a damping term that already
    // existed and already aimed at zero, so a zero field reproduces it
    // bit-for-bit. It is still gated, to skip the primitive loop.
    int currentMode = TPD(sim, currentMode);
    // Base circulation of a drain's whirlpool, m^2/s. Gamma, the quantity that
    // is actually conserved: v_theta = Gamma / (2 pi r), so this fixes how far
    // the swirl reaches, not how fast the throat is. 22 m^2/s is the figure the
    // plan's own excite-radius arithmetic is written against.
    float currentVortexGamma = TPD(sim, currentVortexGamma);
    // Seconds a whirlpool takes to wind down after the flow stops. Plan
    // component 8: "Gamma must decay when flow stops. Otherwise any funnel
    // effect stands open in still water, which is instantly and obviously
    // wrong." This is that number, and 0 would be the bug.
    float currentVortexDecay = TPD(sim, currentVortexDecay);
    // How far a whirlpool reaches, world cells. Separate from Gamma because
    // circulation sets the STRENGTH and this sets the FOOTPRINT — and the
    // footprint is what the shader's AABB reject and the per-sample cost are
    // paid against.
    int currentVortexRadius = TPD(sim, currentVortexRadius);
    // Peak inflow speed at a drain's throat, m/s. The sink term is violent and
    // only a couple of voxels wide at any realistic discharge; that asymmetry
    // against the vortex is why real whirlpools look enormous while the actual
    // suction is a small hole.
    float currentSinkSpeed = TPD(sim, currentSinkSpeed);
    // Chezy coefficient for the stream arm: v = scale * sqrt(slope * depth),
    // depth in metres. 0 disables the stream arm entirely.
    float currentStreamScale = TPD(sim, currentStreamScale);
    // Landform slope below which standing water is a POND, not a stream, Q8
    // (256 = one voxel per voxel = the angle of repose). Read against
    // World::Column::slope, which is `Land.slope` — the HILL-octave gradient,
    // deliberately not the fine one. See the trap note over SeedStreams.
    int currentStreamMinSlope = TPD(sim, currentStreamMinSlope);
    // How fast an MPM node closes the gap to the local current, per second.
    // The ONE current knob a shader reads (sim_fluid.wgsl const-evals it), and
    // it is a DRAG rather than a push for windDrag's reason: a drag law is
    // self-limiting, so no whirlpool and no knob value can fling water faster
    // than the field says it is moving. Only consulted when currentMode is 1.
    float currentDrag = TPD(sim, currentDrag);
  } sim;

  // ---- day/night cycle ----
  // The cycle phase is derived from the SIM TICK (see DayPhaseForTick in
  // world.h), not from wall clock, because the daylight-gated reactions make
  // sunlight feed voxel state. cycleMinutes and the freeze controls therefore
  // change WHEN reactions fire — they are render-and-sim, and a change to them
  // changes the world hash. They are integers for the same reason.
  // Since the celestial overhaul these are ORBITAL ELEMENTS, not a hand-drawn
  // sun arc: sim/celestial.cpp solves Kepler's equation for the planet and
  // both moons every frame and derives the sky from that geometry. Seasons,
  // lunar phase, the 72-day beat between the two moons and eclipses are all
  // consequences, so there is no knob for any of them — you change the orbit.
  struct DayNight {
    // real minutes for one full in-game SOLAR day
    int cycleMinutes = TPD(dayNight, cycleMinutes);
    // 1 = pin the cycle at freezePhase
    int freeze = TPD(dayNight, freeze);
    // 0..65535, 0 = midnight, 32768 = noon
    int freezePhase = TPD(dayNight, freezePhase);

    // ---- the planet ----
    // Axial tilt is the obliquity of the spin axis to the orbital plane, and
    // it is the ENTIRE mechanism behind seasons: the same orbit seen through a
    // tipped equator puts the sun higher in one half of the year than the
    // other. Together with the observer's latitude it fixes noon elevation
    // (90 - |lat - tilt| at the solstice) and day length — which is why it
    // replaced the old `sunPeakElevation` clamp, a knob that set the sun's
    // height while leaving day length wrong.
    float axialTilt = TPD(dayNight, axialTilt);        // degrees
    // observer latitude, degrees north
    float latitudeDeg = TPD(dayNight, latitudeDeg);
    // in-game days per orbit — the season rate
    float yearLengthDays = TPD(dayNight, yearLengthDays);
    // Orbit shape. Near-circular by default: eccentricity mostly shows as the
    // sun changing apparent size and as the equation of time, both subtle.
    float orbitEccentricity = TPD(dayNight, orbitEccentricity);
    // degrees — where in the year perihelion falls
    float orbitArgPeriapsis = TPD(dayNight, orbitArgPeriapsis);
    // degrees — the epoch (tick 0) position
    float orbitMeanAnomaly0 = TPD(dayNight, orbitMeanAnomaly0);
    // Rotates the whole sky about the vertical, i.e. picks which way is east.
    float sunAzimuth = TPD(dayNight, sunAzimuth);
    // True angular RADIUS of the star as seen at a = 1, in degrees. The real
    // sun is 0.266; larger reads better at game FOV and, since the eclipse
    // test is pure geometry, directly sets how often a moon can cover it.
    float sunAngularRadius = TPD(dayNight, sunAngularRadius);

    // How sharply day turns into night. This is the smoothed daylight weight
    // (R.sunUp) that crossfades the sky, ambient and key light; widening it
    // lengthens twilight.
    float twilightWidth = TPD(dayNight, twilightWidth);

    // ---- moon A ----
    // Periods are SYNODIC (new moon to new moon) because that is the cycle a
    // player watches; celestial.cpp derives the sidereal period the orbit is
    // actually integrated with. Authoring the sidereal period instead would
    // make "an 8-day moon" mean an 8.7-day phase cycle.
    int lunarPeriodDays = TPD(dayNight, lunarPeriodDays);
    // degrees to the ecliptic — see below
    float moonInclination = TPD(dayNight, moonInclination);
    float moonEccentricity = TPD(dayNight, moonEccentricity);
    float moonArgPeriapsis = TPD(dayNight, moonArgPeriapsis);
    // longitude of the ascending node, degrees
    float moonNode = TPD(dayNight, moonNode);
    // epoch position, degrees
    float moonMeanAnomaly0 = TPD(dayNight, moonMeanAnomaly0);
    // Angular radius in DEGREES at the orbit's mean distance.
    float moonAngularRadius = TPD(dayNight, moonAngularRadius);

    // ---- moon B ----
    // 9 days against A's 8: coprime, so the pair of phases takes 72 days to
    // repeat. Smaller, further out, and on a differently-oriented plane, so
    // the two moons cross each other rather than travelling together.
    int moon2PeriodDays = TPD(dayNight, moon2PeriodDays);
    float moon2Inclination = TPD(dayNight, moon2Inclination);
    float moon2Eccentricity = TPD(dayNight, moon2Eccentricity);
    float moon2ArgPeriapsis = TPD(dayNight, moon2ArgPeriapsis);
    float moon2Node = TPD(dayNight, moon2Node);
    float moon2MeanAnomaly0 = TPD(dayNight, moon2MeanAnomaly0);
    float moon2AngularRadius = TPD(dayNight, moon2AngularRadius);

    // multiplier on the star wheel rate
    float starRotSpeed = TPD(dayNight, starRotSpeed);
  } dayNight;

  // ---- weather: switches for the sun-driven reactions ----
  //
  // These do NOT scale a rate — they decide whether a reaction rule is
  // COMPILED AT ALL. A rule switched off here is dropped by LoadReactionsJson
  // and never reaches the GPU table, so an off switch costs exactly zero at
  // runtime rather than a per-cell predicate that always fails (rule 2).
  //
  // The mechanism is a "requires" key in reactions.json naming one of these
  // flags; see kWeatherFlagName in sim/materials.cpp for the binding. That
  // keeps the rules themselves data — the flag gates content it does not know
  // about, so adding a second freeze rule needs no C++ change.
  //
  // These change WHICH rules exist, so they change the world hash exactly the
  // way editing reactions.json does. That is fine and expected — it is content,
  // not a divergence — but a lockstep session must agree on them, and toggling
  // one mid-session is a reload, not a live tweak. Booleans rather than ints
  // for exactly that reason: there is no half-on.
  //
  // NONE EXIST TODAY (2026-10-02): `waterFreezes` / `iceMelts` gated the
  // sun-melt and night-freeze rules, and the temperature layer
  // (docs/PLAN_temperature.md) replaced both rules and both switches. The
  // "requires" mechanism stays for the next switch.
  struct Weather {
    // ---- the SKY's weather (src/sim/weather.h, cloud.wgsl) ----------------
    // Everything below is RENDER-ONLY: it picks which assets/weather/*.json
    // preset the sky is showing and how that drifts. None of it reaches the
    // CA or the world hash (weather.h's header says what must change first
    // if rain is ever to touch the world).
    //
    // Master switch. Off skips every cloud pass (nothing is recorded) and the
    // sky is the bare atmosphere it was before clouds existed.
    bool clouds = TPD(weather, clouds);
    // On: the sky walks the moisture ladder of the presets on its own.
    // Off: it holds `preset`. The dev panel's weather row overrides both
    // without touching this file (weather::SetOverride).
    bool autoCycle = TPD(weather, autoCycle);
    std::string preset = TPD(weather, preset);
    // Length of one weather epoch in SIM minutes: the moisture signal gets one
    // new knot per epoch, so this is roughly how long a given sky lasts.
    float epochMinutes = TPD(weather, epochMinutes);
    // Multiplier on how fast the cycle runs (1 = epochMinutes as authored).
    // 0 freezes the automatic sky on whatever it is showing.
    float cycleSpeed = TPD(weather, cycleSpeed);
    // Rerolls the whole weather sequence without touching the world seed.
    int seedOffset = TPD(weather, seedOffset);
    // How long a PINNED change (dev panel, preset edit) takes to ease in, in
    // wall seconds. The automatic cycle is continuous and needs no ease.
    float transitionSeconds = TPD(weather, transitionSeconds);
    // Added to every preset's coverage (-1..1), and a multiplier on every
    // preset's raininess — the two global "make it cloudier / wetter" knobs.
    float coverageBias = TPD(weather, coverageBias);
    float precipScale = TPD(weather, precipScale);
    // Time constant, in sim seconds, over which rained-on ground dries.
    float drySeconds = TPD(weather, drySeconds);
    // The RAIN SHADOW MAP (rain_map.wgsl, render-only): drops and wet ground
    // follow the lean of the fall, so a roof keeps its floor dry and a
    // windward door lets a wedge of rain in. Off = the old openness gate
    // (its rows are not recorded) -- the A/B switch, not a look knob.
    bool rainShadowMap = TPD(weather, rainShadowMap);
    // ---- where the sky touches the WORLD (weather::SimRainWord) ----
    // The one sim-affecting half of the weather: rain on the tick stream, read
    // by reactions authored "rain" (douses) and "rainDamped" (ignitions).
    // Off = the word is 0 and every such rule behaves as if the sky were dry.
    bool rainTouchesWorld = TPD(weather, rainTouchesWorld);
    // How much of an exposed ignition's chance full rain / soaked ground
    // removes (0 = none, 1 = all of it). Scaled by max(rain, wetness), so a
    // drizzle already slows a fire and a field stays slow to catch after it.
    float rainIgniteDamp = TPD(weather, rainIgniteDamp);
    // GROUND STRIKES (docs/PLAN_electricity.md E3). The share of the sky's
    // flash rate (a preset's `lightning`, flashes a minute) that strikes the
    // ground near the player as a real bolt (game/lightning.h): 0.3 in a storm
    // of 7/min is about two strikes a minute. Scheduled from hash(seed, tick),
    // never wall time, so it is part of the authoritative tick and replays.
    float strikeRate = TPD(weather, strikeRate);
    // The radius of the disc round the player they land in, cells (160 = 16 m).
    // The inner quarter is kept clear, so a bolt never lands on your head.
    int strikeRadius = TPD(weather, strikeRadius);
  } weather;

  // ---- combustion: how long anything in the world stays alight ----
  //
  // The second group (after `weather`) that is consumed by the REACTION
  // COMPILER rather than by a kernel, and for the same reason: it is cheaper
  // and clearer to change what the table SAYS than to have every cell in the
  // world re-derive it. LoadAssets reads this while compiling reactions.json —
  // which is why LoadTuning must run before LoadAssets on every path that has
  // both, including the F5 reload (see the note at the reload site in
  // main.cpp).
  //
  // Nothing here reaches a shader, so its tuning_params.def rows are NO_WGSL
  // and there is no TUNE_* constant. The multiplier is applied on the CPU in double and
  // rounded ONCE into the same integer chance an authored value compiles to
  // (rule 1) — the GPU cannot tell a scaled rule from a hand-authored one.
  struct Combustion {
    // Percent multiplier on how long a LIT voxel stays lit: 100 = exactly what
    // reactions.json authors, 200 = twice as long, 50 = half.
    //
    // It divides the chance of every rule that carries "burnDuration": true,
    // which is every rule belonging to the COMBUSTION CLOCK. Two kinds qualify.
    // The rules that RETIRE a burning voxel — ember to ash/smoke/air,
    // cloth/undercloth/hair/flesh burning to their charred or spent forms — are
    // the feature. The RELIGHT rules (charred back to burning) are the subtler
    // half: they are the only closed loop in the fire economy, its gain is
    // (relight chance) x (how long a neighbour stays lit), and scaling only the
    // retire side multiplies that gain by the same factor. That is a CEILING
    // argument, not a fix for anything visible at the default: measured at
    // 200%, scaling the relight rules moved `mob-burn`'s quiet corpse from 13
    // voxels still alight to 15, i.e. not at all. At this knob's 800% the
    // unscaled gain would cross 1.0 and the fire would never go out (rule 2).
    // Scaling both holds the gain exactly where the author put it at every
    // setting — the slider changes the fire's CLOCK and never its shape.
    //
    // Because it scales all of a material's branches by the same factor, the
    // RATIOS between them are untouched: doubling the duration does not change
    // what fraction of a burnt robe survives as ash.
    //
    // It deliberately does NOT touch three neighbouring things:
    //   * IGNITION chances — how fast fire spreads through fuel is a separate
    //     lever, and reactions.json says so at the top of its combustion
    //     section. Turning this up still spreads fire FURTHER, though, because
    //     a voxel that stays lit twice as long emits its upward fire twice as
    //     many times over its life.
    //   * EMIT chances — how much flame a lit voxel shows per tick. Scaling
    //     these too would make a longer burn a dimmer one, which is the
    //     opposite of what anyone reaches for this knob to get.
    //   * EXTINGUISHER rules — water must beat the burn to the tick at any
    //     setting.
    //
    // Default 200 (owner request, 2026-08-30): flammability had just been
    // raised and bodies caught readily but went out again in well under a
    // second, so a burning character neither spread the fire nor took much
    // damage from it.
    int burnDurationPct = TPD(combustion, burnDurationPct);
    // HOW FAST FIRE SPREADS THROUGH FUEL, as a percentage of the ignition
    // chances reactions.json authors. The twin of burnDurationPct next to it,
    // and the two are the pair that file's combustion note names as the two
    // separate levers: this one is how readily the voxel BESIDE a burning one
    // catches, that one is how long a lit voxel then stays lit.
    //
    // A rule is an IGNITION if its product carries tag:hot -- wood to ember,
    // leaf to leaf_burning, cloth to cloth_burning, cooked flesh to burning
    // flesh -- or if it is a hot neighbour acting on FLAMMABLE matter, which
    // is the SEAR (skin -> flesh_cooked, whose product is not itself a heat
    // source). Every one of them scales by this together, so the RATIOS
    // the owner has tuned three times (dry needles catch faster than green
    // leaves, cloth about eight times faster than flesh, leather an eighth of
    // cloth) are preserved exactly. Folded into the chance at reaction-COMPILE
    // time (sim/materials.cpp), so the GPU never learns the knob exists and
    // the kernels stay integer.
    //
    // FOUR THINGS ARE DELIBERATELY NOT SCALED, and each exclusion is the same
    // one burnDurationPct makes from the other side:
    //   * the RETIRE rules (a lit voxel going out) and the RELIGHT rules --
    //     anything marked "burnDuration" in the JSON belongs to that knob
    //     alone, so each rule has exactly one owner and the two cannot
    //     compound on the same number;
    //   * EMIT rules, so a slower fire is not also a dimmer one -- the flame a
    //     burning voxel throws off is what it looks like, not how it spreads;
    //   * HEAT'S OTHER JOBS -- water steaming, ice melting -- because the
    //     neighbour is hot but the self is not fuel, and a flame over a pond
    //     should still steam it however slowly fire spreads;
    //   * EXTINGUISHER rules, so water beats the burn to the tick at any
    //     setting.
    //
    // Default 12 (owner request, 2026-09-03: "slow down the burning voxel
    // spread by like 8x") -- 12% is a factor of 8.3, the nearest the integer
    // knob comes to an eighth. It was 100 until that report, i.e. the authored
    // numbers as written. MOVES THE WORLD HASH: every ignition chance in the
    // compiled table changes, so a change here is a rebaseline in the same
    // commit.
    int spreadPct = TPD(combustion, spreadPct);
    // HEAT CROSSES A JOINT. A creature's limbs are separate lattices that
    // cannot see each other, and a mob is not in the grid, so until 2026-09-03
    // a burning torso reached the legs only through the `fire` gas it emitted
    // -- which rises, and which every ignition rule treats as a weak igniter.
    // Owner report: "setting a mob on fire leads to their legs never catching".
    // Mob::BuildCrossLimbHeat now hands each limb the world cells its OTHER
    // limbs are alight in (plus the six cells beside each, so a joint that does
    // not quite touch at world pitch still conducts), and BurnOneLimb reads
    // them as hot neighbours where the grid holds air. This is the percentage
    // of the authored chance such a neighbour ignites at when it is the ONLY
    // thing arming the rule: 100 = a burning hip lights the thigh exactly as a
    // burning voxel lights the one beside it in the grid, 0 = the old
    // behaviour, limbs invisible to each other. Low by owner request -- the
    // fire should cross a joint, not race across it. Read by the burn pass
    // every tick, no rebuild; F5 hot-reloads it.
    int crossLimbPct = TPD(combustion, crossLimbPct);
    // HOW MUCH OF A FIRE THE DRIFTING FLAME CARRIES. `fire` is the gas that
    // rises off every burning voxel and floats through the air; it carries
    // tag:hot like the coals do, so before neighborChance existed a flame that
    // brushed a tree lit it at the same rate as a bed of embers pressed
    // against it, and a campfire set light to everything downwind.
    // reactions.json authors the exception per ignition rule
    // (`"neighborChance": { "fire": 0.0625 }`, a sixteenth) because WHICH
    // rules get it is a content decision -- the skin sear deliberately does
    // not, so a lick of flame still cooks a surface at full rate. This knob is
    // the global scale over every one of those authored exceptions, the same
    // relationship spreadPct has to the ignition chances themselves: the JSON
    // owns the per-rule ratios, the knob owns the strength.
    //
    // 100 = as authored, so a drifting flame ignites at a sixteenth (6.25%) of
    // a stationary source's rate. 200 = an eighth, 50 = a thirty-second, 0 =
    // the flame cannot ignite anything at all and only the coals spread fire.
    //
    // Applied to a neighbour that is a HOT GAS rather than to the name
    // "fire" -- that is what the exception is actually about, and it means a
    // second hot gas is covered by construction instead of by remembering to
    // add it here. An exception naming a hot SOLID or LIQUID (a future
    // "lava ignites this faster" rule) is untouched: that is not a drifting
    // flame and this knob has no business scaling it.
    //
    // MOVES THE WORLD HASH, like its two neighbours: the compiled chance of
    // every fire-exception rule changes. The `weak-flame` gate reads this and
    // scales its expected ratio with it, so moving the knob does not fail it.
    int flamePct = TPD(combustion, flamePct);
  } combustion;

  // ---- wind: the ambient field (docs/RESEARCH_wind.md, DESIGN.md §12) ----
  //
  // Wind is a PURE FUNCTION of (world position, time) — `windAt` in
  // common.wgsl. Nothing here is stored per chunk or per voxel, so none of it
  // is saved, hashed, streamed, or capable of waking a chunk. Read that as the
  // reason the group is cheap: a knob here changes what a SAMPLE returns, and
  // the world pays only where something samples.
  //
  // The group splits in two, and the split is not cosmetic:
  //
  //   * windSpeed / windDirDeg / gustStrength / weatherAuto are CPU-side. They
  //     feed WindWeather() (sim/wind.h), whose three outputs ride RenderParams
  //     to the shader each frame. Their tuning_params.def rows are NO_WGSL
  //     because a compile-time constant cannot drift over minutes, which is
  //     exactly what weather has to do.
  //   * gustWavelength / gustSpeed / dbgWind* are WGSL rows and const-fold
  //     into every shader (F5). Everything added by the 2026-09-30 wind
  //     overhaul is CPU-side and rides the wf* block (windfield.h): live.
  //
  // Phase 1 is render-only: the two foliage sway sites and the debug overlay.
  // The CA does not read wind until phase 4, which is gated behind
  // sim.windMode and lands in its own rebaseline commit — so nothing in this
  // group can move the world hash today, and nothing in it is integer-only.
  struct Wind {
    // ---- weather (CPU-side; resolved by WindWeather) ----
    // Typical mean wind speed. Metres per second, converted to cells/s (x10 at
    // kVoxelMeters 0.10) once, on the CPU. With weatherAuto on this is the
    // CENTRE the weather varies around (roughly 0.25x..1.75x), not a ceiling.
    // 6 m/s is a fresh breeze — grass visibly leaning and rippling.
    float windSpeed = TPD(wind, windSpeed);
    // Direction the wind BLOWS TOWARD, degrees, using the engine's heading
    // convention: 0 = +Z, increasing toward +X. Ignored while weatherAuto is
    // on. This is the knob to turn to prove the field is real — arrows and
    // grass must both swing to follow it.
    float windDirDeg = TPD(wind, windDirDeg);
    // Gust amplitude as a FRACTION of the mean speed, which is how gustiness
    // actually behaves: a windier day has bigger gusts, not the same gusts on
    // a faster mean. At 1.0 the wind ranges from roughly still to twice the
    // mean; at 0 it is a dead steady breeze and the grass just leans.
    float gustStrength = TPD(wind, gustStrength);
    // Let the weather evolve on its own (deterministic chaos keyed on the
    // tick — see WindWeather). Off pins direction and speed to the two knobs
    // above, which is what you want for inspecting the field or comparing
    // screenshots: an evolving field makes two shots incomparable.
    bool weatherAuto = TPD(wind, weatherAuto);

    // ---- field shape (mirrored in tuning_params.def as TUNE_WIND_*) ----
    // Distance between gust crests along the wind, metres. Short wavelengths
    // read as a rippling meadow; long ones as slow rolling swells. 8 m since
    // 2026-09-30 (was 4.8, the old sway code's): the fronts now ride the wind,
    // so a crest passes a point at U / wavelength — 0.8 Hz in a 6 m/s breeze
    // at 8 m, where 4.8 m flickered at 1.3 Hz.
    float gustWavelength = TPD(wind, gustWavelength);
    // EVOLUTION rate of the gust bands, rad/s — how fast the pattern changes
    // shape in the frame moving WITH the air. Since 2026-09-30 the fronts are
    // carried downwind by the advection clock (gustAdvect below); this is no
    // longer what moves them, and at the old 1.1 it made the bands run
    // upwind. Shared by every consumer — render.microSwaySpeed is only a
    // foliage-local trim on top of it.
    float gustSpeed = TPD(wind, gustSpeed);
    // How fast the gust FRONTS travel, as a fraction of the reference mean
    // wind (docs/RESEARCH_wind.md §13.1). 1.0 is physical: the pattern is
    // frozen into the moving air and crosses the meadow downwind at the mean
    // speed, so a stronger wind means more frequent gusts at a point with the
    // wavelength unchanged. gustSpeed above is then only the slow EVOLUTION of
    // the pattern in the air's own frame. CPU-side (windfield.h AdvPhase), live.
    float gustAdvect = TPD(wind, gustAdvect);
    // ---- the height/terrain ramp (CPU-side; windfield.cpp; live) ----
    // ramp = profile(height above ground) x exposure(x, z) x absTerm(y),
    // docs/RESEARCH_wind.md §13.2. Replaced altitudeGain/altitudeRefY
    // (2026-09-30), which ramped on ABSOLUTE Y from y = 64 while the map's
    // ground sits near y = 200 — every player stood in 1.8x the wind.
    //
    // Log-law profile ln(h / roughness + 1) / ln(profileRef / roughness + 1),
    // clamped to [profileFloor, profileCap]. roughness is z0 in metres (grass
    // ~0.03); profileRef is the height, metres, where the mean wind equals the
    // authored speed — about chest height, so "the wind speed" is the wind you
    // stand in. profileNeutral is used outside the table's 204.8 m coverage.
    float roughness = TPD(wind, roughness);
    float profileRef = TPD(wind, profileRef);
    float profileFloor = TPD(wind, profileFloor);
    float profileCap = TPD(wind, profileCap);
    float profileNeutral = TPD(wind, profileNeutral);
    // Fractional speed-up per 100 m above sea level. Small: the only term
    // that still reads absolute altitude, and the only one past the table.
    float absGain = TPD(wind, absGain);
    // Exposure = 1 + gain x clamp(TPI / tpiScale, -1, 1). TPI (topographic
    // position) is the ground minus the mean ground within tpiRadius metres.
    // A hill's speed-up is ~2H/L; with tpiScale = tpiRadius / 2 a gain of 1
    // is exactly that. Ridge and hollow gains are separate and blended from
    // light to strong wind by the regime intensity: in light wind hollows are
    // strongly sheltered and ridges barely faster; in strong wind the ridge
    // speed-up approaches 2H/L. exposureDepth (m) fades it with height.
    float tpiRadius = TPD(wind, tpiRadius);
    float tpiScale = TPD(wind, tpiScale);
    float ridgeLight = TPD(wind, ridgeLight);
    float ridgeStrong = TPD(wind, ridgeStrong);
    float valleyLight = TPD(wind, valleyLight);
    float valleyStrong = TPD(wind, valleyStrong);
    float exposureDepth = TPD(wind, exposureDepth);
    // Radius, metres, of the water fraction stored per table cell: the signal
    // the sea/lake breeze blows along (stage 4).
    float seaRadius = TPD(wind, seaRadius);
    // ---- the weather REGIME (CPU-side; WindWeatherQ; live) ----
    // Pin a named wind regime (assets/wind/regimes.json: calm, light, breezy,
    // windy, gale, thunderstorm, ...) or "auto" to let the sky drive it. The
    // F1 preset picker writes this. Live.
    std::string regime = TPD(wind, regime);
    // Manual intensity override, 0..1 (~ Beaufort / 12); below 0 = off. Beats
    // the sky and a pinned regime alike, so it is the one slider that always
    // answers "what does THIS strength look like". Live.
    float intensity = TPD(wind, intensity);
    // How far the wind's own ~68 s epoch draw swings the sky's intensity, +-
    // this fraction: the sky sets the day, the epochs set the hour.
    float moodSpread = TPD(wind, moodSpread);
    // The intensity -> mean speed curve, as multiples of windSpeed: piecewise
    // linear through (0, calm), (0.3, 1.0), (0.75, gale), (1, max). windSpeed
    // is therefore the mean at intensity 0.3 — a breezy day, and the manual
    // default — at the reference height.
    float speedCalmMul = TPD(wind, speedCalmMul);
    float speedGaleMul = TPD(wind, speedGaleMul);
    float speedMaxMul = TPD(wind, speedMaxMul);
    // Stability. The day phase makes the air convective by day (sun) and stable
    // at night, damped by cloud cover; wind MIXES that away, linearly to zero at
    // this intensity — a gale is neutral day and night.
    float mixIntensity = TPD(wind, mixIntensity);
    // In stable air (a calm night) the air near the ground partly stops
    // following the air above: the surface wind is scaled by 1 - this x
    // stability at the ground, returning to 1 by decoupleHeight metres.
    float stableDecouple = TPD(wind, stableDecouple);
    float decoupleHeight = TPD(wind, decoupleHeight);
    // Gust amplitude as a fraction of the mean, light -> strong wind, before
    // gustStrength. Peak gust ~ mean x (1 + this): 1.5x in strong wind.
    // gustConvective adds to it in light convective (sunny) air, galeGust is
    // what a gale converges to.
    float gustLight = TPD(wind, gustLight);
    float gustStrong = TPD(wind, gustStrong);
    float gustConvective = TPD(wind, gustConvective);
    float galeGust = TPD(wind, galeGust);
    // Direction MEANDER: a slow, spatially coherent heading perturbation,
    // +- this many degrees in light wind and in strong wind, with a period
    // (s) and a spatial wavelength (m). galeHold is the fraction a gale
    // suppresses it by — a gale holds its heading.
    float wanderLight = TPD(wind, wanderLight);
    float wanderStrong = TPD(wind, wanderStrong);
    float wanderPeriod = TPD(wind, wanderPeriod);
    float wanderWavelength = TPD(wind, wanderWavelength);
    float galeHold = TPD(wind, galeHold);
    // Thermals: a small ISOTROPIC gust term (m/s at full convection) with its
    // own cell size (m) and period (s). Present only in convective air.
    float thermalGust = TPD(wind, thermalGust);
    float thermalWavelength = TPD(wind, thermalWavelength);
    float thermalPeriod = TPD(wind, thermalPeriod);
    // LEE TURBULENCE: past leeOnset intensity, the lee slope of a steep drop
    // (ground descending downwind steeper than leeSlope) gets a gust boost
    // (leeGust) and some reverse flow (leeReverse), in a layer leeDepth
    // metres deep. leeStrength scales it all.
    float leeOnset = TPD(wind, leeOnset);
    float leeStrength = TPD(wind, leeStrength);
    float leeSlope = TPD(wind, leeSlope);
    float leeDepth = TPD(wind, leeDepth);
    float leeReverse = TPD(wind, leeReverse);
    float leeGust = TPD(wind, leeGust);
    // LOCAL WINDS on light-wind days (docs/RESEARCH_wind.md §13.4). Slope winds:
    // up the table's slope by day at up to slopeWind m/s (x convective
    // stability), down it at night at slopeNight x that (katabatic), in a layer
    // slopeDepth metres deep. The sea/lake breeze: onshore by day along the
    // water-fraction gradient at up to seaBreeze m/s, offshore at night at
    // seaNight x that, seaDepth metres deep. Both fade to nothing as the mean
    // wind rises past localFade m/s.
    float slopeWind = TPD(wind, slopeWind);
    float slopeNight = TPD(wind, slopeNight);
    float slopeDepth = TPD(wind, slopeDepth);
    float seaBreeze = TPD(wind, seaBreeze);
    float seaNight = TPD(wind, seaNight);
    float seaDepth = TPD(wind, seaDepth);
    float localFade = TPD(wind, localFade);
    // THE THUNDERSTORM TIMELINE (§13.4): cycles of stormCycle seconds weighted
    // by the regime's convective input. A lull to stormLull x the mean, the
    // gust front (heading jumps ~stormJump degrees, mean spikes to stormFront x),
    // decay to stormDecay x, then gusty decay home with the gust fraction
    // raised by stormGust. The front sweeps across as a wind-primitive jet
    // (stormFrontJet) and stormBursts downbursts of stormBurstRadius metres
    // land near the window, all positioned from seed + tick.
    float stormCycle = TPD(wind, stormCycle);
    float stormLull = TPD(wind, stormLull);
    float stormFront = TPD(wind, stormFront);
    float stormDecay = TPD(wind, stormDecay);
    float stormGust = TPD(wind, stormGust);
    float stormJump = TPD(wind, stormJump);
    int stormBursts = TPD(wind, stormBursts);
    float stormBurstRadius = TPD(wind, stormBurstRadius);
    bool stormFrontJet = TPD(wind, stormFrontJet);
    // GUST STREAKS (wind_streak.wgsl; render-only, never hashed; live through
    // RenderParams, no F5). streakAlpha is the master visibility: 0 records
    // neither the update pass nor the draw. A streak is born only where the
    // gust excess (the bands along the local mean, plus primitives) passes
    // streakThreshold m/s, with certainty by threshold + streakSpan; it lives
    // streakLife s within streakRadius m of the camera, drawn as a ribbon of
    // streakTrail points pushed every streakSpacing s, streakWidth m wide.
    float streakAlpha = TPD(wind, streakAlpha);
    int streakCount = TPD(wind, streakCount);
    float streakThreshold = TPD(wind, streakThreshold);
    float streakSpan = TPD(wind, streakSpan);
    int streakTrail = TPD(wind, streakTrail);
    float streakSpacing = TPD(wind, streakSpacing);
    float streakLife = TPD(wind, streakLife);
    float streakRadius = TPD(wind, streakRadius);
    float streakWidth = TPD(wind, streakWidth);

    // ---- debug slope-field overlay (research doc §4.8) ----
    // Initial state of the arrow overlay; F4 toggles it in-game. It is a
    // tuning knob as well as a key so the field can be inspected from a saved
    // tuning.json and from a headless screenshot run, neither of which can
    // press a key. Costs exactly nothing when off — the draw is skipped, not
    // drawn transparent.
    bool dbgWindField = TPD(wind, dbgWindField);
    // Spacing between arrow lattice points, world voxels. The lattice is
    // snapped to this grid in WORLD space, so the arrows stay put as the
    // camera moves instead of swimming with it.
    float dbgWindSpacing = TPD(wind, dbgWindSpacing);
    // Radius of the arrow lattice around the camera, world voxels. Cost is
    // cubic in radius/spacing, so this is the knob that decides whether the
    // overlay is free or not: the default 48/8 is 13^3 = 2197 arrows.
    float dbgWindRadius = TPD(wind, dbgWindRadius);
  } wind;

  // ---- render: everything below is emitted as WGSL and F5-reloadable ----
  struct Render {
    // sky / sun
    float sunDiscGain = TPD(render, sunDiscGain);
    float sunDir[3] = TPD_V3(render, sunDir);
    float sunColor[3] = TPD_V3(render, sunColor);
    float sunIntensity = TPD(render, sunIntensity);

    // ---- atmospheric sky (physically-flavoured scattering model) ----
    // Rayleigh scales the molecular scattering that makes the sky blue and the
    // sunset red; Mie is the forward-scattering haze that puts a glow around
    // the sun. These two, plus the air-mass curve, replace the old two-colour
    // lerp and are what let one model cover noon, sunset and night.
    float skyRayleigh = TPD(render, skyRayleigh);
    float skyMie = TPD(render, skyMie);
    // Mie anisotropy; higher = tighter halo
    float skyMieG = TPD(render, skyMieG);
    float skyMieStrength = TPD(render, skyMieStrength);
    float skyExposure = TPD(render, skyExposure);
    float skyGround[3] = TPD_V3(render, skyGround);  // below-horizon bounce
    // Multiplier on the true 0.53 deg disc. 1.0 is physically correct and
    // reads as a pinprick on a 16:9 screen at a game FOV — every engine that
    // wants the sun to be a PRESENCE oversizes it. 3x is about the smallest
    // that still looks deliberate rather than like a dead pixel.
    float sunSize = TPD(render, sunSize);
    // How hard the atmosphere reddens a low sun. Scales the extinction that
    // colours BOTH the sun disc and the dome, and is deliberately separate
    // from skyRayleigh: that one sets how blue the sky is, and sharing one
    // constant between them makes a rich blue sky imply a permanently orange
    // sun (it did — the whole dome came out khaki).
    float sunReddening = TPD(render, sunReddening);

    // ---- night sky ----
    float nightZenith[3] = TPD_V3(render, nightZenith);
    float nightHorizon[3] = TPD_V3(render, nightHorizon);
    // MLS-MPM fluid prototype (debris.wgsl vsFluid). There is no fluid
    // colour knob: a particle is drawn in its MATERIAL's colour (W1-B2).
    // Cube half-extent per particle, in cells. 0.5 tiles the rest lattice
    // exactly; slightly over closes the gaps so a pool reads as a surface.
    float fluidParticleSize = TPD(render, fluidParticleSize);
    // How much a particle elongates along its velocity (0 = always a cube).
    // Motion blur for free: falling streams read as streaks, not dice.
    float fluidStretch = TPD(render, fluidStretch);
    // Albedo darkening with compression (density above rest), so pressure
    // visibly travels through a pool.
    float fluidDensityShade = TPD(render, fluidDensityShade);
    // ---- MPM fluid SURFACE rendering (raymarch.wgsl MPM FLUID SURFACE) ----
    // The Splash-style water look: the solver's node grid marched as a smooth
    // isosurface with traced reflection/refraction. All render-only.
    // DRAW MODE, not a boolean — it keeps the name because 0 and 1 still mean
    // what they always did, so tuning.json needs no migration:
    //   0 = one raster cube per particle (solver debug; DrawFluid, not the
    //       raymarcher — this is the only mode that draws on the CPU side)
    //   1 = smooth isosurface, the Splash look
    //   2 = voxelized at half a cell (2x2x2 sub-voxels per sim cell, which is
    //       one sub-voxel per particle at rest density). DEFAULT — the engine
    //       is a voxel engine, so MPM water reads as voxels by default and the
    //       smooth surface is the opt-in look
    //   3 = voxelized on the sim lattice, one cube per world cell — MPM water
    //       that reads as ordinary voxel water
    // Modes 2 and 3 are RENDER-ONLY quantization of the same density field the
    // isosurface marches; nothing is written to the voxel buffer (rules 1+3).
    float fluidSurface = TPD(render, fluidSurface);
    // isosurface threshold, fraction of rest
    // density. Lower = fatter, more merged fluid
    float fluidIso = TPD(render, fluidIso);
    // normal-gradient baseline, voxels. Higher
    // smooths harder at the cost of small shapes
    float fluidSmooth = TPD(render, fluidSmooth);
    // How much of the CA's geometry SUPPORTED fluid borrows (raymarch.wgsl,
    // "TWO MODELS OF THE SAME WATER"). 1 = water resting on ground or on other
    // water is drawn as a height field with its surface at cell.y + fill,
    // exactly where the CA draws `cell.y + (state+1)/8`, so a spreading film is
    // one eighth tall and settling does not make it jump. 0 = the pre-2026-08-25
    // behaviour, one isotropic blob field everywhere. Airborne water (a
    // droplet, a splash arch) is unaffected at any setting — it has nothing
    // underneath it, so the blob model keeps it.
    float fluidLevel = TPD(render, fluidLevel);
    // refraction index (water 1.33, oil ~1.47)
    float fluidIor = TPD(render, fluidIor);
    // metres of fluid to ~1/e absorption
    float fluidClarity = TPD(render, fluidClarity);
    // traced/sky reflection gain
    float fluidReflect = TPD(render, fluidReflect);
    float fluidSpecular = TPD(render, fluidSpecular);  // sun glint gain
    // surface speed (vox/s) for full churn foam
    float fluidFoamSpeed = TPD(render, fluidFoamSpeed);
    // sub-voxel normal shimmer on moving fluid
    float fluidWobble = TPD(render, fluidWobble);
    // Speed-driven whitening: fast, loose particles read as spray/foam.
    float fluidFoam = TPD(render, fluidFoam);
    // ---- depth colour gradient (raymarch.wgsl DEPTH GRADIENT) ----
    // A thin film reads as `fluidShallow`, the deep body tends toward
    // `fluidDeep`, ramped over `fluidDepth` metres of in-fluid path. The ramped
    // colour is what drives the per-channel Beer-Lambert absorption, so the
    // gradient is a real absorption change, not a tint painted on top.
    float fluidShallow[3] = TPD_V3(render, fluidShallow);
    float fluidDeep[3] = TPD_V3(render, fluidDeep);
    // metres over which the ramp completes
    float fluidDepth = TPD(render, fluidDepth);
    // 0 = flat species albedo (old look), 1 = full
    float fluidGradient = TPD(render, fluidGradient);
    // ---- grid foam field (sim_fluid.wgsl foam potentials) ----
    // gain on the advected foam field's whitening
    float fluidFoamField = TPD(render, fluidFoamField);
    // fbm break-up of the foam field, 0 = flat
    float fluidFoamTexture = TPD(render, fluidFoamTexture);
    // Foam PARTICLE colour (debris.wgsl, PPAY_FOAM). Foam is entrained air,
    // not a substance, so it has no material to take an albedo from — it is
    // coloured from here, jittered per particle by foamColorVar so a burst
    // reads as many bubbles rather than one flat white mass.
    float foamColor[3] = TPD_V3(render, foamColor);
    float foamColorVar = TPD(render, foamColorVar);
    float starBrightness = TPD(render, starBrightness);
    // direction-grid cells per unit
    float starDensity = TPD(render, starDensity);
    // PSF core radius in PIXELS, not radians. Sizing in pixels is what keeps a
    // star a point at any resolution/FOV; the first version used a fixed
    // angular radius ~4x the SUN's, which read as nearby blobs with visible
    // pixel steps across their falloff.
    float starSize = TPD(render, starSize);
    // Fraction of grid cells that hold a star (per layer; the fine layer uses
    // 1.7x this). Low on purpose — filling a fifth of the grid is TV static.
    float starSparsity = TPD(render, starSparsity);
    float starTwinkle = TPD(render, starTwinkle);
    float milkyWayStrength = TPD(render, milkyWayStrength);
    float milkyWayColor[3] = TPD_V3(render, milkyWayColor);
    // Pole of the galactic plane, in the STAR SPHERE's frame (it wheels with
    // the stars). The band is drawn perpendicular to this, so rotating it
    // moves the Milky Way across the constellations.
    float galaxyNormal[3] = TPD_V3(render, galaxyNormal);
    // Half-width of the band in |cos| from that plane, before the fbm that
    // ragged-edges it. Small values give a tight bright river.
    float galaxyWidth = TPD(render, galaxyWidth);
    float nebulaStrength = TPD(render, nebulaStrength);
    float nebulaCool[3] = TPD_V3(render, nebulaCool);
    float nebulaWarm[3] = TPD_V3(render, nebulaWarm);
    // Aurora — the Shivering Isles curtains.
    float auroraStrength = TPD(render, auroraStrength);
    // voxels; sets how curtains converge
    float auroraHeight = TPD(render, auroraHeight);
    float auroraLow[3] = TPD_V3(render, auroraLow);
    float auroraHigh[3] = TPD_V3(render, auroraHigh);

    // ---- moons ----
    // NOTE: no moon RADIUS here. Apparent size is an output of the orbit
    // (dayNight.moon*AngularRadius, modulated by orbital distance), because
    // the disc and the eclipse test must read ONE number for how big a moon
    // is. A render-side radius knob would be a second, diverging answer.
    float moonBrightness = TPD(render, moonBrightness);
    float moonColor[3] = TPD_V3(render, moonColor);
    // Offsets the fbm that carves this moon's maria and craters. Not a colour
    // and not a position — it is the only thing making a moon a distinct rock
    // rather than the same face drawn twice, so changing it rerolls the
    // surface wholesale.
    float moonMariaSeed[3] = TPD_V3(render, moonMariaSeed);
    float moonGlow = TPD(render, moonGlow);
    float moonEarthshine = TPD(render, moonEarthshine);
    float moonLightColor[3] = TPD_V3(render, moonLightColor);
    float moonLightIntensity = TPD(render, moonLightIntensity);
    // Moon B: a smaller, colder, dimmer body. Look only.
    float moon2Color[3] = TPD_V3(render, moon2Color);
    float moon2MariaSeed[3] = TPD_V3(render, moon2MariaSeed);
    float moon2Brightness = TPD(render, moon2Brightness);
    float moon2LightIntensity = TPD(render, moon2LightIntensity);
    float moon2LightColor[3] = TPD_V3(render, moon2LightColor);
    // How dark a TOTAL solar eclipse gets. 1 = the dome falls to its full
    // night value; lower keeps some daylight so totality reads as
    // daytime-gone-wrong rather than as night.
    float eclipseDarkness = TPD(render, eclipseDarkness);
    // Perceptual exponent on covered AREA before that darkening applies. 3 is
    // the old hardcoded cube: the world stays bright until the last sliver of
    // sun goes, which is how a real partial eclipse reads. 1 tracks area
    // linearly and looks like someone sliding the exposure down.
    float eclipseCurve = TPD(render, eclipseCurve);

    // ---- night ambient ----
    float nightAmbSky[3] = TPD_V3(render, nightAmbSky);
    float nightAmbGround[3] = TPD_V3(render, nightAmbGround);

    // fog: the LIVE adaptive fog in the frame loop (main.cpp). The optical
    // depth budget spent across the filled cascade radius, and the per-frame
    // ease toward it. world.h kFogOpticalDepths/kFogLerpPerFrame are the same
    // numbers for the constexpr static pin the --shot harnesses use;
    // main.cpp static_asserts the defaults agree.
    float fogOpticalDepths = TPD(render, fogOpticalDepths);
    float fogLerpPerFrame = TPD(render, fogLerpPerFrame);

    // ambient / diffuse
    float ambSky[3] = TPD_V3(render, ambSky);
    float ambGround[3] = TPD_V3(render, ambGround);
    float diffuseWrap = TPD(render, diffuseWrap);
    float faceX = TPD(render, faceX), faceZ = TPD(render, faceZ);

    // AO
    float aoStrength = TPD(render, aoStrength);
    float aoFar = TPD(render, aoFar);

    // shadows
    float shadowBias = TPD(render, shadowBias);
    int shadowSteps = TPD(render, shadowSteps);
    float shadowSoftNear = TPD(render, shadowSoftNear);
    float shadowSoftFar = TPD(render, shadowSoftFar);
    float shadowLift = TPD(render, shadowLift);
    float shadowFarLift = TPD(render, shadowFarLift);
    // Voxel-keyed shadow cache (world.h kShadowCacheBuckets). `shadowCache` is
    // the A/B toggle and is const-folded, so flipping it in tuning.json + F5
    // recompiles raymarch.wgsl WITHOUT the shadow trace() call site — which is
    // the whole point, since that call site costs 3.59 ms of register footprint
    // against 2.29 ms of traversal (--render-budget, 2026-09-01).
    // `shadowCacheSubdiv` is the patch granularity per voxel-face axis: a
    // QUALITY knob, since the ray saving saturates well before it gets coarse.
    int shadowCache = TPD(render, shadowCache);
    int shadowCacheSubdiv = TPD(render, shadowCacheSubdiv);
    // THE SUN'S APPARENT RADIUS AS THE SHADOW RAY SEES IT, in DEGREES, and the
    // one knob that sets how wide a penumbra is (shadow_resolve.wgsl, world.h
    // kShadowHistBytes). The resolve pass jitters its ray inside this cone and
    // averages the last kShadowSamples verdicts, so a blocker `d` away casts an
    // edge about 2*d*tan(angle) wide: crisp under a kerb, soft under a canopy.
    //
    // NOT dayNight.sunAngularRadius, which is the star's TRUE size (0.3 deg,
    // and what the disc is drawn at and what eclipse geometry uses). At 0.3 deg
    // a canopy 10 m up softens over 10 cm — one voxel — which is physically
    // right and reads as the hard edge this replaced. This is the artistic one.
    // 0 turns the cone off and restores the single-ray hard shadow exactly.
    float shadowSunAngle = TPD(render, shadowSunAngle);

    // grain
    float grainBroadScale = TPD(render, grainBroadScale);
    float grainFineScale = TPD(render, grainFineScale);
    float grainMix = TPD(render, grainMix);
    float grainAmp = TPD(render, grainAmp);
    float grainAmpFar = TPD(render, grainAmpFar);

    // media / smoke
    float mediaAbsorb = TPD(render, mediaAbsorb);
    float mediaTauMax = TPD(render, mediaTauMax);

    // fire
    float fireFlickerBase = TPD(render, fireFlickerBase);
    float fireFlickerAmp = TPD(render, fireFlickerAmp);
    float fireFlickerRate = TPD(render, fireFlickerRate);
    float fireGlowRate = TPD(render, fireGlowRate);
    float fireIntensity = TPD(render, fireIntensity);
    float fireBreatheAmp = TPD(render, fireBreatheAmp);
    float fireBreatheRate = TPD(render, fireBreatheRate);
    float emissiveStrength = TPD(render, emissiveStrength);
    float emissiveFlickerBase = TPD(render, emissiveFlickerBase);
    float emissiveFlickerAmp = TPD(render, emissiveFlickerAmp);
    float emissiveFlickerRate = TPD(render, emissiveFlickerRate);
    // Burn-tinted materials (kMatFlagBurnTint): a burning leaf keeps its
    // leaf palette and pulses toward this flame colour. Rate in rad/s on a
    // per-cell phase; Min/Max bound the pulse weight (0 = pure leaf, 1 = pure
    // flame). The emission is scaled by the weight too, so the leaf phase is
    // lit like a leaf rather than glowing green.
    float burnTintColor[3] = TPD_V3(render, burnTintColor);
    float burnTintRate = TPD(render, burnTintRate);
    float burnTintMin = TPD(render, burnTintMin);
    float burnTintMax = TPD(render, burnTintMax);

    // water
    float waterF0 = TPD(render, waterF0);
    float waterAbsorb[3] = TPD_V3(render, waterAbsorb);
    float waterScatter[3] = TPD_V3(render, waterScatter);
    float waterFresnelPower = TPD(render, waterFresnelPower);
    // Global calm-down of the travelling wave field. Water reads as still,
    // glassy water at rest rather than as a windswept sea; the column-height
    // gradient still gives real bodies their macro shape, so lowering these
    // makes water calm, not flat.
    float rippleAmpScale = TPD(render, rippleAmpScale);
    float rippleSpeedScale = TPD(render, rippleSpeedScale);
    // Fetch gate (waterOpenness in raymarch.wgsl): fraction of a 12-tap
    // horizontal ring that must be liquid before travelling waves appear.
    // Below LOW a surface is a droplet or puddle and stays perfectly still.
    float waterFetchLow = TPD(render, waterFetchLow);
    float waterFetchHigh = TPD(render, waterFetchHigh);
    float reflectionCutoff = TPD(render, reflectionCutoff);
    int reflectionSteps = TPD(render, reflectionSteps);
    float causticGain = TPD(render, causticGain);
    float causticCap = TPD(render, causticCap);
    float glintIntensity = TPD(render, glintIntensity);
    float glintPowerNear = TPD(render, glintPowerNear);
    float glintPowerFar = TPD(render, glintPowerFar);
    float foamDepth = TPD(render, foamDepth);
    float foamStrength = TPD(render, foamStrength);

    // ---- SURFACE WAVES (docs/PLAN_water_master.md component 9) -----------
    //
    // Render-side, not `sim.*`, and the boundary is absolute: a render wave can
    // never push anything. The body's LEVEL (sim) and its DISPLACEMENT (render)
    // stay strictly separate and the CA never sees the displacement. The moment
    // a wave height feeds back so a boat bobs, a render field has become
    // authoritative for sim (design guideline #3).
    //
    // THE ONE THAT MATTERS. `waveDispersion` mixes between one speed for every
    // octave (0) and the real relation w^2 = g k tanh(k h) (1). At 0 the
    // surface reads as a scrolling texture; at 1 the 8 m swell runs 4x faster
    // than the 0.5 m chop in 2.6 m of water and SLOWS as it reaches a bank,
    // which is shoaling, and it costs nothing but the choice of constant.
    // It exists as a knob because it is also the honest A/B for that claim.
    float waveDispersion = TPD(render, waveDispersion);
    // Gerstner crest sharpening, 0..1. Sinusoids have round crests and round
    // troughs; real gravity waves have sharp crests and flat troughs, and this
    // is the term that produces the difference.
    float waveSteepness = TPD(render, waveSteepness);
    // Depth in METRES below which wave amplitude fades to nothing. Sum-of-waves
    // does not reflect off a bank and shallow water damps chop anyway, so the
    // cheap fix is also the physically right one.
    float waveShoreDepth = TPD(render, waveShoreDepth);
    // How hard the current field advects the wave phase — the wave is evaluated
    // at `position - current * t`, so the Doppler stretch downstream is what
    // makes a surface look like it is GOING somewhere. 0 pins the waves to the
    // world and the flow stops reading as flow.
    float waveFlowScale = TPD(render, waveFlowScale);
    // Foam on the current field's CONVERGENCE lines. `threshold` is the
    // convergence (1/s) at which foam starts and `gain` how fast it saturates.
    float waveFoamThreshold = TPD(render, waveFoamThreshold);
    float waveFoamGain = TPD(render, waveFoamGain);
    // Impact ripples: ring expansion speed (m/s), amplitude e-fold time
    // (seconds) and the wavelength of the ring train (metres).
    float waveImpactSpeed = TPD(render, waveImpactSpeed);
    float waveImpactDecay = TPD(render, waveImpactDecay);
    float waveImpactLen = TPD(render, waveImpactLen);
    // ---- W3: the SIM's surface momentum, in the fragment stage ----------
    // docs/PLAN_water_relevel.md §5, last bullet. `waterFlux` is bound
    // read-only to the render group and the water surface reads its OWN
    // column's four pipes. The height stays authoritative through
    // liquidColumn(); the pipes carry what an eighth-quantised height cannot —
    // which way the surface is moving and how hard.
    //
    // waveSimSlope: how far the normal tilts down-flow, per whole eighth/tick
    // of net pipe flux. Sub-eighth detail BETWEEN the steps the column height
    // can express, which is the point of reading the flux at all.
    // waveSimFoam: how much froth a column running at one whole eighth/tick
    // gets. It joins the existing shoreline/convergence foam through a max(),
    // so a sloshing lake foams on its rings and a still one is unchanged.
    //
    // BOTH AT 0 CONST-FOLDS THE ENTIRE BLOCK, the buffer read included. That is
    // the arm --shader-stats is compared against, and it is not optional
    // bookkeeping: this shader has no register headroom.
    float waveSimSlope = TPD(render, waveSimSlope);
    float waveSimFoam = TPD(render, waveSimFoam);
    // ---- the current-field arrow overlay (plan component 8) -------------
    // A clone of the wind overlay's two knobs, at a water scale: currents are
    // metres per second where wind is tens, so the lattice is tighter and the
    // reach shorter. `dbgCurrentField` is a bool for the dbgWindField reason —
    // a knob as well as a key, so the overlay is reachable from a saved
    // tuning.json and from a headless screenshot run, neither of which can
    // press anything.
    bool dbgCurrentField = TPD(render, dbgCurrentField);
    // ---- async compute (docs/PLAN_async_compute.md) ---------------------
    // Run the tick's RENDER-ONLY derived passes (pass_table.def PT_DERIVED:
    // the openness grid and the glow field) on the device's async compute
    // queue, so they overlap the next frame's render instead of running in
    // front of it. Ignored (single queue) on a device without one. OFF by
    // default: measured, it does not win on the RTX 3060 Ti (the plan has the
    // numbers). SANDVOX_ASYNC_COMPUTE=0/1 overrides it for an A/B. READ AT
    // DEVICE CREATION too: the async queue exists only if this is on at boot
    // (an unused second queue costs frame time on NVIDIA).
    bool asyncCompute = TPD(render, asyncCompute);
    float dbgCurrentSpacing = TPD(render, dbgCurrentSpacing);
    float dbgCurrentRadius = TPD(render, dbgCurrentRadius);

    // translucent solids — ice, glass (shadeTranslucent in raymarch.wgsl).
    // A solid is translucent when its authored `opacity` is < 255; these
    // control what that translucency LOOKS like. Absorption is per metre of
    // real path through the slab, so one number covers "thin ice is clear"
    // and "thick ice is deep cyan" at once.
    // head-on reflectance (ice IOR 1.31)
    float iceF0 = TPD(render, iceF0);
    float iceFresnelPower = TPD(render, iceFresnelPower);   // Schlick exponent
    // absorption gain per metre, x opacity
    float iceAbsorb = TPD(render, iceAbsorb);
    // floor so even clear ice tints slightly
    float iceAbsorbFloor = TPD(render, iceAbsorbFloor);
    // internal bubble/grain scatter strength
    float iceScatter = TPD(render, iceScatter);
    // how fast scatter saturates with depth
    float iceScatterDepth = TPD(render, iceScatterDepth);
    // scatter retained with the sun down
    float iceScatterNight = TPD(render, iceScatterNight);
    // frost normal perturbation amplitude
    float iceGrain = TPD(render, iceGrain);
    // frost noise frequency (world space)
    float iceGrainScale = TPD(render, iceGrainScale);
    // specular exponent (higher = tighter)
    float iceGloss = TPD(render, iceGloss);
    // specular highlight strength
    float iceSpec = TPD(render, iceSpec);
    // metres of ice past which the march stops
    float iceDepthMax = TPD(render, iceDepthMax);
    // Fresnel weight below which the traced reflection is replaced by a plain
    // sky lookup. Unlike water, a translucent SOLID can present many surfaces
    // to one ray, so an ungated reflection here is a frame-time cliff.
    float iceReflectMin = TPD(render, iceReflectMin);

    // ---- submerged view (shadeSubmerged in raymarch.wgsl) ----
    // Everything in this block applies ONLY when the view ray is inside a
    // liquid, so a dry frame is untouched by all of it.
    //
    // subAbsorb is deliberately NOT waterAbsorb. Looking down THROUGH a
    // surface, a hard red kill is the depth cue that makes a lake read deep.
    // Living inside the water at that same strength puts you in a featureless
    // blue void two metres from your face — there is no distance information
    // left to see. Underwater wants a much longer visibility range, so it gets
    // its own (weaker) coefficients and its own scatter floor.
    float subAbsorb[3] = TPD_V3(render, subAbsorb);   // per metre, per channel
    // colour the volume tends to
    float subScatter[3] = TPD_V3(render, subScatter);
    // in-scatter strength multiplier
    float subScatterGain = TPD(render, subScatterGain);
    // Metres at which the view has fully faded to the scatter colour. The
    // underwater analogue of fog distance: this is the "murky pond" vs "clear
    // tropical water" knob.
    float subVisibility = TPD(render, subVisibility);
    // screen-edge darkening while submerged
    float subVignette = TPD(render, subVignette);
    // Snell's window: from below, the entire sky is compressed into a ~97
    // degree cone straight up, and outside it the surface is a mirror of the
    // murk. This scales how bright that window reads.
    float subSnellGain = TPD(render, subSnellGain);

    // caustics cast onto submerged surfaces (bedCaustic in raymarch.wgsl).
    // Separate from causticGain/Cap, which drive the caustic seen looking DOWN
    // through a surface from dry land. This is a different projection — from
    // the surface directly above the LIT POINT rather than above the bed the
    // primary ray found — and sharing one gain makes one of the two views
    // always wrong.
    float bedCausticGain = TPD(render, bedCausticGain);
    float bedCausticCap = TPD(render, bedCausticCap);
    // metres of water above, past which it washes out
    float bedCausticFade = TPD(render, bedCausticFade);
    // higher = thinner, brighter filaments
    float bedCausticSharp = TPD(render, bedCausticSharp);

    // volumetric light shafts (godRays in raymarch.wgsl). Ray-marched with a
    // real per-sample occlusion test, so shafts break around the shore and any
    // overhang instead of passing through terrain. Sample count is a direct
    // frame-time multiplier, but on SUBMERGED pixels only.
    int godRaySteps = TPD(render, godRaySteps);
    float godRayStrength = TPD(render, godRayStrength);
    // Henyey-Greenstein asymmetry. Shafts are far brighter looking toward the
    // sun than away from it; that anisotropy is what makes them read as beams
    // rather than as a uniform brightening of the whole volume.
    float godRayAniso = TPD(render, godRayAniso);
    // metres the shaft march covers
    float godRayRange = TPD(render, godRayRange);
    // BLOCK steps for the per-sample occ ray
    int godRayShadowSteps = TPD(render, godRayShadowSteps);
    // Metres past which a shadow-class ray terminates on the 4^3 blocker mask
    // instead of the voxel (traceOpaque in common.wgsl). 0 = off, and 0 is
    // also bit-identical to the pre-W2-B tracer by construction.
    float shadowCoarseDist = TPD(render, shadowCoarseDist);

    // ---- the openness (sky-visibility) grid (docs/PLAN_gi.md §2) ----
    // Per (4^3 block, face) sky visibility, marched over the blockers mask by
    // sim_openness.wgsl and read by ambientAt / ambientAtP. Render-only: the
    // sim has no binding for any of it and the world hash cannot move.
    // metres a hemisphere ray looks
    float opennessReach = TPD(render, opennessReach);
    // slots the rolling refresh walks/tick
    int opennessChunksPerFrame = TPD(render, opennessChunksPerFrame);
    // 0 = old lerp AND the pass unrecorded
    float opennessStrength = TPD(render, opennessStrength);
    // the pre-2026-09-11 daylight leak, kept as its A/B arm
    float opennessFloor = TPD(render, opennessFloor);
    // The enclosed face's own ambient, ADDED at (1 - openness) and independent
    // of the sun/moons: a cave must not know what time it is.
    float enclosedAmbient[3] = TPD_V3(render, enclosedAmbient);
    // blend the 4 blocks in the face plane
    int opennessBilinear = TPD(render, opennessBilinear);

    // ---- one-bounce indirect light (docs/PLAN_gi.md §3) ----
    // The irradiance grid: injected by the shadow resolve pass and the
    // openness walk, gathered at every near-field hit. Render-only.
    // 0 = everything const-folded away
    float giStrength = TPD(render, giStrength);
    // per-visit fade of unmeasurable faces
    float giDecay = TPD(render, giDecay);
    // P2 write-back weight, < giDecay
    float giFeedback = TPD(render, giFeedback);
    // STEP budget per gather ray (a clear chunk = 1 step)
    int giGatherBlocks = TPD(render, giGatherBlocks);
    // frames between a slot's re-gathers; 0 = uncached
    int giCachePeriod = TPD(render, giCachePeriod);

    // ---- the glow field (src/sim/world.h kGlowBytes) ----
    // A coarse position-keyed field of emitter light, written by sim_glow.wgsl
    // and read with ONE buffer load by the paths giGather cannot serve (the
    // raster body/mob cubes). Render-only: the sim has no binding for it.
    // 0 = both rows unrecorded, term folded
    float glowStrength = TPD(render, glowStrength);
    // metres an emitter chunk throws light
    float glowReach = TPD(render, glowReach);
    // emitting-cell fraction that saturates
    float glowFill = TPD(render, glowFill);
    // slots the rolling refresh walks/tick
    int glowChunksPerFrame = TPD(render, glowChunksPerFrame);
    // dirty workgroups that may rewrite 3^3
    int glowRingBudget = TPD(render, glowRingBudget);
    // also sample at the terrain hit (dbl-counts giGather)
    int glowTerrain = TPD(render, glowTerrain);

    // drifting particulate. Render-only motes suspended in the water, which is
    // what gives the light shafts something visible to catch.
    float siltDensity = TPD(render, siltDensity);
    float siltBrightness = TPD(render, siltBrightness);
    float siltDrift = TPD(render, siltDrift);

    // ---- waterfall mist and spray ----
    // Render-only overlay on a FALLING CA liquid column and on its impact
    // site, both derived per pixel from the cells under/over a liquid hit
    // (raymarch.wgsl fallCueAt). No particle, no buffer, nothing hashed.
    // mistDensity <= 0 removes the whole term at shader-compile time.
    float mistDensity = TPD(render, mistDensity);
    float mistBrightness = TPD(render, mistBrightness);
    // voxels the veil wraps around the column
    float mistRadius = TPD(render, mistRadius);
    // m/s the vapour field drifts DOWN
    float mistFallSpeed = TPD(render, mistFallSpeed);
    float sprayDensity = TPD(render, sprayDensity);
    // voxels the impact puff reaches
    float sprayRadius = TPD(render, sprayRadius);

    // how strongly the underside of the surface ripples the view of the sky
    float subSurfaceRipple = TPD(render, subSurfaceRipple);

    // ---- the generic per-liquid submerged profile ----
    // (submergedProfile in raymarch.wgsl.) These shape the MAPPING from a
    // liquid's authored opacity + palette to its submerged look, so EVERY
    // liquid gets a complete treatment with no shader change — including ones
    // added later. The subAbsorb/subScatter/subVisibility values above are not
    // "the underwater settings" any more; they are WATER'S refinement, blended
    // in at the clear end of the curve rather than picked by a material test.
    //
    // Opacity is the axis, because it is already the authored measure of how
    // much a medium blocks and it already orders the shipped liquids the way
    // submersion should: water 90, acid 170, blood 200, oil 235.
    // visibility (m) in a fully opaque liquid
    float subMurkVis = TPD(render, subMurkVis);
    // clarity exponent for visibility only
    float subVisCurve = TPD(render, subVisCurve);
    // opacity -> per-metre absorption
    float subAbsorbGain = TPD(render, subAbsorbGain);
    // so even a clear liquid is not a vacuum
    float subAbsorbFloor = TPD(render, subAbsorbFloor);
    // How much of its own colour a liquid scatters back at the eye, at the
    // dense and clear ends. In a dense liquid, that scatter IS what you see.
    float subScatterDense = TPD(render, subScatterDense);
    float subScatterClear = TPD(render, subScatterClear);
    // Clarity band over which a liquid crosses from the derived profile onto
    // water's hand-tuned coefficients. Water sits at clarity ~0.79, oil ~0.25;
    // widening this band makes more liquids inherit water's look.
    float subClearLow = TPD(render, subClearLow);
    float subClearHigh = TPD(render, subClearHigh);

    // Faint directional glow toward the surface when submerged in a medium
    // too dense to see through. A near-opaque liquid gates off Snell's window,
    // and what that left was a featureless field of colour with no sense of up
    // and nothing in motion - honest, but it reads as a broken shader rather
    // than as being under the oil.
    float subMurkGlow = TPD(render, subMurkGlow);

    // ---- oil / petroleum-like viscous liquids ----
    // Oil and blood share the viscous SURFACE path (isViscousLiquid) but look
    // nothing alike, and blood's constants applied to oil rendered the pool as
    // flat beige mud: matte, desaturated, no highlight, no reflection. These
    // are the oil end of every term that differs.
    //
    // oiliness() in raymarch.wgsl derives the blend from the material's own
    // authored palette SATURATION - no material ids, no new JSON key, the same
    // principle isViscousLiquid itself follows. Blood's colour0 is 0.85
    // saturated and oil's is 0.46: pigment suspensions are strongly chromatic,
    // petroleum is a near-neutral brown-black.
    float oilSatLow = TPD(render, oilSatLow);
    float oilSatHigh = TPD(render, oilSatHigh);
    // Oil's IOR (~1.47 vs water's 1.33) puts F0 at roughly double water's, and
    // unlike blood it approaches a real mirror at grazing - that hard bright
    // rim is the look, not the artifact blood's lower graze guards against.
    float oilF0 = TPD(render, oilF0), oilGraze = TPD(render, oilGraze);
    // Tighter lobe than blood's: a smooth film gives a small hard glint where
    // a rough suspension gives a broad soft one, and that narrowness is most
    // of what the eye reads as "glossy" rather than "damp".
    float oilGloss = TPD(render, oilGloss), oilSheen = TPD(render, oilSheen);
    // How much the reflection is tinted by the liquid itself. Near zero: a
    // petroleum film is a near-NEUTRAL dark mirror, so what you see in it is
    // the sky and the far bank, not a brown wash of its own body colour.
    float oilReflectTint = TPD(render, oilReflectTint);
    // How far the body colour is pushed toward black. Petroleum absorbs nearly
    // everything entering it and reflects the rest off the surface - the
    // opposite of blood's bright backscatter, and the term that kills the beige.
    float oilDarken = TPD(render, oilDarken);
    // Thin-film interference (the rainbow slick): strength, and the spatial
    // scale of the film-thickness field that sets the band spacing.
    float oilIridescence = TPD(render, oilIridescence);
    float oilFilmScale = TPD(render, oilFilmScale);
    // The sheen appears ONLY where oil floats on a DENSER liquid - a film needs
    // two interfaces close together, and a deep pool on rock has no second one
    // within reach of the light (floatingOnLiquid in raymarch.wgsl). This
    // scales how much denser the layer below must be to count as a real
    // boundary; oil 900 on water 1000 is a ratio of 0.111.
    float oilFloatSens = TPD(render, oilFloatSens);
    // Silhouette-feather width for oil, against blood's 0.28. Blood can afford
    // a wide fade because its body colour reads through the blend; oil's body
    // is nearly black, so the same fade leaves a droplet as a smear of the
    // scene behind it. This is most of why oil looked see-through.
    float oilEdgeBand = TPD(render, oilEdgeBand);
    // How much plain sky reflection an UNPOOLED oil surface returns. A droplet
    // is a tiny curved mirror scattering the sky everywhere, so far less
    // reaches the eye than off a flat pool; at 1.0 a grazing droplet returns
    // full-brightness sky and reads as a hole in the world.
    float oilDropReflect = TPD(render, oilDropReflect);

    // blood / viscous liquids (shadeViscous in raymarch.wgsl)
    float bloodF0 = TPD(render, bloodF0);        // head-on reflectance
    // grazing reflectance (water goes to 1.0)
    float bloodGraze = TPD(render, bloodGraze);
    // opacity -> per-metre absorption
    float bloodAbsorb = TPD(render, bloodAbsorb);
    // how much of the surface behind shows through
    float bloodTransmit = TPD(render, bloodTransmit);
    // Hard ceiling on that transmission. Beer-Lambert alone leaves a lone
    // droplet (path << one voxel) half-transparent no matter how absorbing the
    // material is; blood is opaque at sub-millimetre scale because it
    // backscatters, and this models that. Raise it and blood becomes red glass.
    float bloodMaxTransmit = TPD(render, bloodMaxTransmit);
    // metres^-1: bright thin -> dark deep
    float bloodDepthRamp = TPD(render, bloodDepthRamp);
    // droplet <-> pool ramp
    float bloodPoolLow = TPD(render, bloodPoolLow);
    float bloodPoolHigh = TPD(render, bloodPoolHigh);
    // field value below which the rim fades out
    float bloodEdgeFeather = TPD(render, bloodEdgeFeather);
    // field-gradient baseline in voxels (anti-faceting)
    float bloodSmooth = TPD(render, bloodSmooth);
    // surface-tension wobble (NOT wind ripples)
    float bloodWobble = TPD(render, bloodWobble);
    float bloodSheen = TPD(render, bloodSheen);      // wet highlight strength
    // specular exponent on a droplet (broad)
    float bloodSheenDrop = TPD(render, bloodSheenDrop);
    // ... and on a pool (tight)
    float bloodSheenPool = TPD(render, bloodSheenPool);
    // sky-lit sheen, so it reads wet in shade
    float bloodAmbientSheen = TPD(render, bloodAmbientSheen);
    // metres of column counted as "thin edge"
    float bloodEdgeDepth = TPD(render, bloodEdgeDepth);
    float bloodEdgeStrength = TPD(render, bloodEdgeStrength);
    float bloodEdgeTint[3] = TPD_V3(render, bloodEdgeTint);

    // stains (applyStain in raymarch.wgsl)
    // how fast amount turns into coverage
    float stainCoverage = TPD(render, stainCoverage);
    // splatter break-up (0 = flat wash)
    float stainMottle = TPD(render, stainMottle);
    // noise frequency of that break-up
    float stainMottleScale = TPD(render, stainMottleScale);
    // how much a stain darkens its substrate
    float stainDarken = TPD(render, stainDarken);
    // how far it goes to the pure stain colour
    float stainOpacity = TPD(render, stainOpacity);
    // wet highlight on a fresh stain
    float stainSheen = TPD(render, stainSheen);
    float stainSheenPower = TPD(render, stainSheenPower);

    // lava
    float lavaCrackFreq = TPD(render, lavaCrackFreq);
    float lavaCrackKneeLow = TPD(render, lavaCrackKneeLow);
    float lavaCrackKneeHigh = TPD(render, lavaCrackKneeHigh);
    float lavaWarmBias = TPD(render, lavaWarmBias);
    float lavaEmissionGain = TPD(render, lavaEmissionGain);
    float lavaPulseAmp = TPD(render, lavaPulseAmp);
    float lavaPulseRate = TPD(render, lavaPulseRate);

    // tonemap
    float exposureWhite = TPD(render, exposureWhite);
    float bleachAmount = TPD(render, bleachAmount);
    float gamma = TPD(render, gamma);

    // static micro-detail (traceMicro in raymarch.wgsl)
    // Distance in METRES past which a PARTIAL POWDER cell stops being drawn as
    // its grain arrangement (tracePowder) and becomes a whole cube or air by
    // mass. It USED to be the micro-brick LOD too (a plain voxel past it);
    // since 2026-09-28 micro bricks and plants instead fade out entirely
    // before the far handoff — see plantLodDist below and NEAR-DETAIL FADE in
    // raymarch.wgsl — so this no longer affects them.
    float microLodDist = TPD(render, microLodDist);
    // Where the NEAR-DETAIL FADE begins (metres of camera distance), for
    // every micro model — plants, tile plants, bricks. Past it each model
    // thins out (a world-keyed hash places its vanish distance in the band)
    // and column plants shrink into the ground, reaching zero density at
    // DETAIL_FADE_END_M = min(lodHandoffDist, nearest window face) - 0.5 m,
    // so nothing near-only is left when the far cascade takes over. Clamped
    // in the shader to at most END - 1 m. Until 2026-09-28 this was the
    // distance past which a column plant became a SOLID PROXY CUBE (the
    // measured reason: an analytic tuft is a wind sample plus 6-8 blade
    // tests per cell, and a 40 m meadow fell from ~50 to ~15 fps); the cubes
    // are gone — the fade bounds the band's cost instead.
    float plantLodDist = TPD(render, plantLodDist);
    // Cap on nested micro marches per primary ray. A ray grazing a meadow can
    // cross dozens of grass cells, and each one that MISSES keeps the ray
    // alive, so without a cap one pixel can pay for the whole field. Past the
    // cap a micro cell is treated as SOLID (not as air), because terminating
    // the ray is bounded and correct-ish while letting it fly is neither —
    // nearer than plantLodDist; inside the fade band it is air (a solid cube
    // there would bring back the proxy pillars the fade replaced).
    int microMaxPerRay = TPD(render, microMaxPerRay);
    // Wind bend at a swaying plant's TIP, in sub-voxels (subdiv 8 => 1.25 cm
    // each). Clamped to 2.0: the models keep a 2-sub-voxel margin from their
    // cell walls, and anything past that shears blade tips through the wall
    // where the nested DDA never marches them — they vanish, not clip.
    float microSwayAmp = TPD(render, microSwayAmp);
    // ---- trample (render-only; DESIGN.md §9 "Analytic plants") ----
    // Seconds a flattened plant takes to stand back up after the presser
    // leaves. The press-in itself is fixed (~0.12 s) because a foot lands
    // faster than anything worth tuning.
    float trampleRecover = TPD(render, trampleRecover);
    // How far a fully trampled plant compresses: 0.8 leaves 20% of its height.
    float trampleDepth = TPD(render, trampleDepth);
    // Lateral lean of a fully trampled plant's tip, in cells, AWAY from the
    // presser. Clamped inside the plant's own column by the renderer, so past
    // ~0.4 it saturates rather than shearing blades into neighbour cells.
    float trampleLean = TPD(render, trampleLean);
    // Multiplier on a presser's collision half-width to get its stamp radius:
    // feet reach a little past the capsule, and grass bends past the foot.
    float trampleRadius = TPD(render, trampleRadius);
    // FOLIAGE-LOCAL trim on the wind clock, applied on top of wind.gustSpeed.
    // It used to be the band rate outright; since the wind rewrite the field
    // itself owns that (windAt in common.wgsl, wind.gustSpeed), and this is a
    // multiplier the two sway sites apply to the time they hand it. Default is
    // 1.0 for a reason: at anything else, grass samples the field at a
    // different phase than the debug arrow overlay draws, so the overlay stops
    // being evidence about the grass. Move wind.gustSpeed instead unless you
    // specifically want foliage running off the shared clock.
    float microSwaySpeed = TPD(render, microSwaySpeed);

    // ---- how fast the cascade REBUILDS, in sieve entries per tick --------
    // CPU-ONLY (never reaches a shader): FarField::SetBulkCap. Applies to a
    // wholesale refill — the startup horizon, a load, a teleport — not to the
    // incoming planes of ordinary travel, which have their own cap inside
    // FarField (kPlayFillCap).
    //
    // A refill is kFarLevels x kFarNumChunks = 262,144 sieve entries; this
    // number is how many of them a tick may take. It used to be kFarListCap
    // = 4,096 with no knob, i.e. 267 ms of GPU in ONE tick and 2 fps until
    // the queue drained — the horizon's arrival was the largest stall in the
    // walking frame, reported from live play as "everything goes foggy and
    // the fps dies for ten seconds".
    //
    // THE SLICE IS NOT FREE TO SHRINK, which is the non-obvious half.
    // Measured `--frames 2500 --autowalk`, same binary, same walk, one full
    // refill each (the entry count is printed by that harness):
    //
    //   cap   frame p50/p95/p99/max   >33ms  >100ms   far GPU total  far ticks
    //   4096   17.2  42.0  57.2  374   11.8%    10       5.7 s          456
    //   1024   21.3  63.7  79.9  287   22.7%     3      15.3 s          847
    //    256   23.5  56.8  69.7  151   34.3%     1      20.6 s         1506
    //
    // Same ~290k entries in all three, but the TOTAL GPU cost is 3.6x higher
    // at 256 than at 4096: a far dispatch has a large fixed cost (its two
    // rows barrier against farDown's writes to a 1 GiB farVox), and the whole
    // machine runs hotter for longer, which shows up on the raymarch row too.
    // So this trades peak hitch against total work, not against nothing.
    //
    // 1024 is the knee: it turns 63 frames over 100 ms into 3 and keeps the
    // median within 4 ms of the cheapest arm. Raise it toward 4096 to get the
    // horizon back sooner and accept the hitches; lower it toward 256 if a
    // dropped frame matters more than a busy minute. Hot-reloads on F5.
    int farRefillRate = TPD(render, farRefillRate);

    // ---- THE ORDINARY-TRAVEL CAP (farfield.h kPlayFillCap) -----------------
    // Entries an INCOMING PLANE may drain per tick: the horizon keeping up
    // with a player who is walking, sprinting or flying, as opposed to the
    // wholesale refill above. It was a hard-coded 64 until 2026-09-12, sized
    // when a sieve entry cost ~45 us of GPU; the `far` kernel's sky early-out
    // took that to ~19 us, and 64 was by then the reason the queue backlogged
    // at all — 437k entries (3.8 minutes of drain) after 22 s of flight, deep
    // enough that the valid-box face counts overflowed and the renderer fell
    // through to house-sized cells at 40 m. 256 is ~4.8 ms/tick of sieve,
    // still under what 64 was sized to spend. Hot-reloads on F5.
    int farPlaneFillRate = TPD(render, farPlaneFillRate);

    // budgets
    int primarySteps = TPD(render, primarySteps);
    int farSteps = TPD(render, farSteps);
    // How far a far-field sun shadow ray reaches, in METERS. Converted to a
    // per-level step count by farShadowSteps() in raymarch.wgsl so the reach
    // is the same world distance at every cascade level (a raw step count is
    // not: it scales with the level's cell size — see the comment there).
    float farShadowReach = TPD(render, farShadowReach);
    // Highest cascade level at which the conservative "any blocker" flag
    // (common.wgsl FAR_BLOCKER_BIT) may terminate a PRIMARY ray. Shadows use
    // it at every level unconditionally; the visible surface only up to here,
    // because the flag is set for any cell whose floor reaches the ground, so
    // honouring it lifts terrain by up to one cell — 0.4 m at level 1, 51 m at
    // level 8. 0 is the material-only hit test the cascade shipped with.
    //
    // DEFAULT 0 IS A MEASURED RESULT, NOT A PLACEHOLDER (13.2.2's kill
    // criterion, `--shot` pair 2026-09-01). At 2 the visible half genuinely
    // does what it was built for — a snow patch at 60 m stops being a
    // dithered smear of half-missing cells and becomes one solid streak, and
    // the whole 25..205 m slope reads as a ramp instead of a checkerboard —
    // but the same one-cell lift BURIES the single-cell ground cover standing
    // on that slope (scrub, flowers, litter simply vanish under the risen
    // ground), and a cell that hits on the flag alone shades from the nearest
    // material below it, which paints occasional flat single-colour facets on
    // an otherwise textured hillside. The distant ridge line is NOT the
    // casualty: the sky silhouette is pixel-identical at 0 and 2 (levels >= 3
    // never take the flag), and the change is confined to the bottom 40% of
    // the frame. Raise it to 2 to see the trade; it is one tuning edit and no
    // rebuild.
    int farBlockerHitLevel = TPD(render, farBlockerHitLevel);

    // ---- the far SURFACE MAP refine (LOD-seam package A, DESIGN.md §9) ----
    // Highest cascade level whose SURFACE cells are refined against the far
    // surface map (world.h kFarMap*): a per-level 2D heightfield at twice the
    // level's XZ resolution — level 1's is the fine 10 cm column grid — holding
    // each column's true top and skin. A cell the map vouches for (pristine
    // heightfield, not edited) is intersected against its 2x2 sub-columns
    // instead of being drawn as one centre-sampled cube, so level 1 draws the
    // near field's own columns and every coarser level gains 2x surface
    // detail. Trees, rocks, ruins and edits keep the 3D cells. 0 turns it off
    // everywhere (the A/B), which is what `--budget-arms norefine` does.
    int farRefineLevel = TPD(render, farRefineLevel);

    // ---- in-window LOD handoff (PLAN_surface_flight_perf.md A1) ----
    // Distance in METERS past which the PRIMARY march stops resolving fine
    // 10 cm voxels and hands the rest of the ray to the far-field cascade,
    // instead of only doing so when the ray leaves the 25.6 m window.
    //
    // The trade, stated plainly: at the handoff a 1-voxel cell becomes a
    // FAR_CELL1_VOX-voxel one (4 voxels = 40 cm here), so terrain past this
    // distance quantises 4x coarser. Silhouettes and positions are preserved
    // — an A/B at 18 m vs off showed trees, hillsides and structures in the
    // same places with the same shapes — but per-blade grass detail in the
    // mid field visibly becomes 40 cm blocks.
    //
    // MEASURED (offscreen 1080p sweep, camera 12 m over canopy, quiet
    // machine, shadows on): 10.36 ms off / 9.65 ms at 22 / 9.02 ms at 24 /
    // 9.22 ms at 18. The win is ~8-11% and it SATURATES around 22-24 m:
    // pushing the handoff nearer buys nothing more and only spends image
    // quality. That is why the default is 24 and not the 18 first tried, and
    // it is also the honest verdict on the plan's expectation that A1 alone
    // would "flatten the altitude curve" — it does not. It is a single-digit
    // percentage win, not the 46 ms the plan attributed to Part A.
    //
    // Set >= WINDOW_HALF_EXTENT_METERS (25.6 m) to disable the handoff
    // entirely and get the old "switch only at window exit" behaviour — which
    // is exactly how to A/B it without a rebuild (F5 reloads it).
    // ---- the gas crossfade (docs/PLAN_gas_particles.md stage 1b) --------
    // Where the voxel representation of gas starts handing over to the coarse
    // one, as a FRACTION of the residency window's half-extent measured from
    // the window CENTRE in the max norm. 0.5 = the fade runs over the outer
    // half, from 12.8 m to the face at 25.6 m.
    //
    // The window CENTRE and not the camera, which is the whole trick: the
    // weight is then exactly 1 at every one of the six faces regardless of
    // where the camera is, so the seam the fade exists to remove disappears on
    // all of them at once rather than on the one the camera happens to face.
    //
    // 1.0 disables the crossfade (voxels at full opacity right up to the face,
    // coarse gas starting at the face) and gets stage 1's hard edge back --
    // which is how to A/B it without a rebuild, since F5 reloads this.
    float gasBlendStart = TPD(render, gasBlendStart);
    // ---- FAR FIRE PLUMES (world.h kGasFarEmitMax, sim_gas.wgsl
    // `gasFarPlume`) ---------------------------------------------------------
    //
    // A fire whose chunk leaves the residency window is frozen mid-burn: its
    // embers stay baked into the far cascade and glow for the rest of the
    // session, while its smoke dies within about four seconds because smoke
    // parcels are only ever born at the window face by the running CA. These
    // two knobs shape the column the GPU synthesizes into the coarse density
    // box for each such fire instead.
    //
    // farPlumeStrength: density multiplier on the splat. 0 is an EXACT off
    // switch and not a cheap path — the emitter count goes to zero, so the
    // splat row is not recorded, no buffer is written, and the box's clear
    // falls back to the parcel latch exactly as it did before the feature
    // existed. That is the A/B arm, and F5 reloads it.
    float farPlumeStrength = TPD(render, farPlumeStrength);
    // How far the synthesized column climbs above the fire, in METRES. The
    // shader converts with kVoxelMeters and then clamps to the density box, so
    // a value past the box edge costs nothing extra and simply saturates. 28 m
    // is a little over half the box's half-extent, which reads as a tall plume
    // from outside without the top of every column sitting on the box lid.
    float farPlumeHeight = TPD(render, farPlumeHeight);
    // HOW FAR OUT A FROZEN FIRE STILL SMOKES, in metres.
    //
    // There are TWO density boxes. gasOuter spans ±51.2 m at 0.8 m cells; the
    // long-range box (world.h kGasFarOuterN) spans ±409.6 m at 6.4 m cells,
    // which is exactly far cascade level 4's box edge. This is the outer bound
    // on the second one, in the max norm from the window centre, and it is
    // clamped to the box so a value past it saturates rather than doing
    // nothing visible.
    //
    // 0 is an EXACT off switch for the long-range half only, and a genuinely
    // useful one: the wide emitter list comes out empty, so the wide splat row
    // is not recorded, its 4 MiB box is never cleared, the render flag stays
    // down and the raymarch's coarse segment is not walked. Near plumes carry
    // on. That is the A/B arm for what the long-range box costs.
    // 3276.8, NOT 409.6, and the difference is the whole feature. The wide
    // list ADMITS an emitter at >= kWorldN voxels (409.6 m) and rejects it past
    // this, so a default of 409.6 makes the admissible band a one-voxel shell:
    // the long-range box was never written in any real session and only the
    // gas-farplume2 gate, which sets its own range, ever exercised it. Fixed in
    // tuning.json on 2026-09-19; this struct default was left behind, and a
    // struct default matters whenever the key is missing from the JSON.
    float farPlumeRange = TPD(render, farPlumeRange);

    // ---- clouds (cloud.wgsl; DESIGN.md 9.w). The LOOK of the sky's clouds;
    // which weather is showing lives in `weather` (src/sim/weather.h). ----
    // Tile period of the Perlin-Worley shape volume, metres. Sets the size of individual cloud bodies: a quarter of this is roughly one cumulus tower.
    float cloudShapeScaleM = TPD(render, cloudShapeScaleM);
    // Tile period of the Worley erosion volume. Smaller = finer cauliflower and wisps on the cloud edges.
    float cloudDetailScaleM = TPD(render, cloudDetailScaleM);
    // Feature size of the coverage/type/rain fields. Large = broad fronts and wide clear gaps; small = a sky of scattered patches.
    float cloudWeatherScaleM = TPD(render, cloudWeatherScaleM);
    // Optical density of cloud at density 1, per metre. Higher = more opaque, darker undersides, harder silhouettes.
    float cloudExtinction = TPD(render, cloudExtinction);
    // How deep the detail noise eats into the base shape. 0 = smooth blobs, 1 = ragged wisps.
    float cloudErosion = TPD(render, cloudErosion);
    // View-ray samples through the deck per low-res pixel. The main cost knob.
    int cloudSteps = TPD(render, cloudSteps);
    // Samples toward the sun per view sample (plus one long sample). Fewer = flatter clouds, cheaper.
    int cloudLightSteps = TPD(render, cloudLightSteps);
    // How far along a ray the deck is marched. Past it the deck is left to the haze.
    float cloudMaxDistM = TPD(render, cloudMaxDistM);
    // e-folding distance of the aerial perspective on clouds: how quickly distant cloud dissolves into the horizon sky.
    float cloudHazeM = TPD(render, cloudHazeM);
    // Henyey-Greenstein anisotropy of the forward lobe. Higher = brighter silver lining when looking toward the sun.
    float cloudPhaseG = TPD(render, cloudPhaseG);
    // Octave falloff of the multiple-scattering approximation. Higher = brighter, softer cloud interiors.
    float cloudMultiScatter = TPD(render, cloudMultiScatter);
    // Darkening of thin cloud edges seen away from the sun (the 'powdered sugar' look).
    float cloudPowder = TPD(render, cloudPowder);
    // Skylight on clouds, as a multiple of the terrain's hemisphere ambient.
    float cloudAmbient = TPD(render, cloudAmbient);
    // Multiplier on direct sun/moon light scattered by clouds.
    float cloudSunGain = TPD(render, cloudSunGain);
    // How much cloud shadows darken the direct light on the ground. 0 = clouds cast nothing.
    float cloudShadowStrength = TPD(render, cloudShadowStrength);
    // Opacity of the distant rain shafts under raining cells.
    float cloudRainDensity = TPD(render, cloudRainDensity);
    // Brightness of the falling streaks (or flakes) around the camera.
    float cloudRainStreaks = TPD(render, cloudRainStreaks);
    // Weight of the new frame in the low-res accumulation. Lower = smoother but more lag when clouds move fast.
    float cloudTemporal = TPD(render, cloudTemporal);
    // The cloud buffer is the render target divided by this on each axis. 2 = quarter the pixels.
    int cloudResDiv = TPD(render, cloudResDiv);
    // Cloud drift as a multiple of the surface wind (winds aloft are faster than at the ground).
    float cloudWindScale = TPD(render, cloudWindScale);
    // How much rain-soaked, sky-exposed ground darkens at full wetness.
    float cloudWetDarken = TPD(render, cloudWetDarken);
    // Strength of the rainbow in sunlit rain (42 degrees from the anti-solar point).
    float cloudRainbow = TPD(render, cloudRainbow);
    // Feature size of the high ice-cloud streaks.
    float cloudCirrusScaleM = TPD(render, cloudCirrusScaleM);
    // How much the clouds in a direction colour the distance fog and reflections in it (the env map).
    float cloudFogMix = TPD(render, cloudFogMix);
    // shipped: 20.5 m (tuning.json) = ON, a camera sphere inside the nearest
    // window face (22.4 m); >= 25.6 disables it (the window box is the handoff).
    float lodHandoffDist = TPD(render, lodHandoffDist);
    // ---- frame pacing and internal resolution (CPU-only: NO_WGSL rows, no
    // TUNE_* constant — nothing here reaches a shader) ----------------------
    //
    // renderScale: the WORLD is rendered at (width, height) x this, into an
    // offscreen target, and blitted up to the swapchain with NEAREST
    // filtering; the ImGui overlay draws at native resolution on top. The
    // raymarch is cleanly pixel-linear (the render-budget `halfres` arm saved
    // 70% of it), and the far cascade already renders at ~6 px per cell at
    // 1080p, so most of what a scaled frame loses is resolution the data
    // never had. 1.0 renders straight into the swapchain as before — the
    // offscreen target and the blit exist only below 1. Clamped to [0.25, 1].
    float renderScale = TPD(render, renderScale);
    // ---- TAA + temporal upscale (assets/shaders/taa.wgsl) ----------------
    //
    // taa: 1 replaces the NEAREST blit at the end of a scaled frame with a
    // resolve pass that accumulates sub-pixel-jittered frames into a
    // native-resolution history. What renderScale costs in edges, this buys
    // back over ~16 frames — the point of the pair is that 0.7 stops looking
    // like 0.7. 0 is the old blit, byte for byte, and is the A/B arm.
    //
    // At renderScale 1 it is still worth having: the jitter + accumulation is
    // then plain temporal ANTI-ALIASING of a full-resolution frame, which is
    // what softens the shadow cache's patch quantisation and the voxel
    // silhouettes without touching either system.
    //
    // CPU-ONLY, all four of these: they reach taa.wgsl through its own small
    // uniform buffer, not through a TUNE_* shader constant, so changing one
    // costs no shader recompile and applies on the NEXT FRAME. That is
    // deliberate — an A/B you can flip mid-flight is one that gets run.
    int taa = TPD(render, taa);
    // taaMaxHist: the ceiling on accumulated sample weight, i.e. the effective
    // length of the running average. 64 is voxelbit's stationary ceiling and
    // converges hardest; lower reacts faster to a change and ghosts less.
    // Below ~4 there is not enough history to reconstruct anything and the
    // pass is a cost with no product.
    float taaMaxHist = TPD(render, taaMaxHist);
    // taaClamp: how far OUTSIDE the 3x3 colour box of the current frame a
    // history sample is allowed to sit before it is pulled in. 0 is the
    // hardest clamp (sharpest, most flicker), large is no clamp at all
    // (smoothest, ghosts behind every moving edge). This is the one knob that
    // trades ghosting against flicker; everything else trades cost.
    float taaClamp = TPD(render, taaClamp);
    // taaJitter: scale on the R2 sub-pixel camera offset, in render pixels.
    // 1 = the full +/-0.5 px the reconstruction filter integrates over. 0
    // disables the jitter and leaves the accumulation running, which is the
    // arm that isolates "what did the jitter buy" from "what did the temporal
    // average buy" — with it at 0 the pass can only blur.
    float taaJitter = TPD(render, taaJitter);
    // taaSharpLod: with TAA on, tell the raymarch its pixels are NATIVE-sized
    // rather than render-sized — `viewPx` goes to the window height instead of
    // the render height. This is the voxel equivalent of the negative mip bias
    // every temporal upscaler ships with: `viewPx` drives the plant LOD
    // distance, the water ripple footprint and the micro-detail cutoff, so at
    // renderScale 0.7 without it the low-resolution frame is not merely
    // sampled more coarsely, it is DRAWN with coarser content — and detail the
    // renderer chose not to draw is detail no accumulator can recover.
    //
    // The trade is real in both directions: each frame is more aliased (which
    // is what the accumulator is for) and finer LOD is kept further out (which
    // costs time). 0 keeps the old behaviour and is the A/B arm; `--gate taa`
    // measures both arms in one run and prints both errors.
    int taaSharpLod = TPD(render, taaSharpLod);
    // taaSharpness: the reconstruction filter's Gaussian exponent, in NATIVE
    // pixels — exp(-taaSharpness * d^2). At 2.29 a sample one native pixel away
    // still counts for 10%, i.e. a filter about half a native pixel wide.
    //
    // THIS IS THE PASS'S OPEN QUESTION, and it is a knob rather than a shader
    // constant so the next person can answer it with a tuning.json edit and one
    // `--gate taa`, no rebuild. Measured at 2.29 the accumulated error RISES
    // with frame count (1.19 at 16 frames, 1.42 at 48) and finishes level with
    // a plain NEAREST blit: the accumulator is converging to a half-pixel blur
    // rather than to the reference. Larger values let only the frames whose
    // jittered sample landed nearly on the pixel speak for it — sharper, at the
    // cost of starving pixels the jitter sequence keeps missing.
    float taaSharpness = TPD(render, taaSharpness);
    // ---- the shading-LOD filter (assets/shaders/denoise.wgsl) --------------
    // CPU-ONLY, all seven: they reach denoise.wgsl through its own uniform
    // (Simulation::WriteDenoiseParams), never the TUNE_ prelude, so a change
    // is one tuning.json edit and no shader recompile.
    //
    // denoise: 1 runs a depth-guided a-trous filter over the world frame, at
    // render resolution, before TAA or the upscale blit. It averages the
    // lighting of terrain whose FINE voxels project smaller than denoisePx*
    // pixels — the mid-distance staircase speckle of lit and shadowed cube
    // faces — and leaves near geometry, the sky and depth edges alone. 0 is
    // the A/B arm. Nothing here has a history: no ghosting, nothing to reset.
    // SHIPS OFF (owner's call, 2026-09-12): with the filter on, distant
    // terrain read as out of focus; the in-raymarch contact-term fade
    // (raymarch.wgsl lodShadeFade) stays on and is the part that ships.
    int denoise = TPD(render, denoise);
    // denoiseIters: a-trous iterations, each a 5x5 tap at dilation 2^i. 1 =
    // a 5-px support (takes the 1-2 px stipple and leaves the 6 px cascade
    // mosaic), 2 = 13 px (shipped: the mosaic averages, terrace bands and
    // relief survive), 3 = 25 px (measured 2026-09-12: every hillside reads
    // out of focus), 4 = 49 px. Cost is linear in it.
    int denoiseIters = TPD(render, denoiseIters);
    // denoisePxFull / denoisePxStart: the strength ramp, in PROJECTED PIXELS
    // PER FINE VOXEL at the pixel's depth. Full strength at or below pxFull,
    // off at or above pxStart. At 1080p / 70 deg a 10 cm voxel is 2 px at
    // ~39 m and 8 px at ~10 m, so the shipped pair means "off inside 10 m,
    // ramping in to 39 m, full beyond" — and the far cascade, at a constant
    // ~6 px per CELL (= 1.5 px per fine voxel at level 1, less beyond), is
    // always full. Written in pixels so the band follows resolution and fov.
    // The ramp is this WIDE on purpose: a 15-31 m ramp measured as a visible
    // line where crisp voxels met filtered ones, the same defect as the LOD
    // ring the cascade seam dither exists to break.
    float denoisePxFull = TPD(render, denoisePxFull);
    float denoisePxStart = TPD(render, denoisePxStart);
    // denoiseDepthTol: the depth edge stop, as a fraction of view depth per
    // pixel of tap offset. A tap whose depth differs from the centre by more
    // than about this is a different surface (a crest against the hill
    // behind, a mob against the ground) and stops the filter. A grazing
    // ground plane at 100 m changes depth by ~1% per pixel, which this must
    // exceed or every tread becomes an edge.
    float denoiseDepthTol = TPD(render, denoiseDepthTol);
    // denoiseChromaTol: the chroma edge stop, the width of a Gaussian on the
    // distance between two taps' luminance-normalised colours. The speckle is
    // LUMINANCE (lit vs shadowed faces of one material); a material boundary
    // is mostly hue. Smaller preserves more material edges and less of the
    // lit/shadow averaging (shadow is slightly bluer than sun); larger blurs
    // across everything.
    float denoiseChromaTol = TPD(render, denoiseChromaTol);
    // denoiseStrength: overall ceiling on the filter (0..1). 1 is the full
    // average; 0.5 keeps half the original speckle under it. Shipped 0.7 with
    // two iterations: the residual is what keeps the picture reading as
    // in-focus terrain rather than a soft gradient.
    float denoiseStrength = TPD(render, denoiseStrength);
    // presentMode: 0 fifo (vsync, quantises a 22 ms frame to 33), 1 mailbox
    // (newest frame at each vblank, no tearing, no quantisation), 2 immediate
    // (tears). Applied when it CHANGES (a swapchain recreate). Use fifo or an
    // fpsCap while recording: mailbox lets the game submit as fast as it can,
    // which starves a capture tool of GPU time.
    int presentMode = TPD(render, presentMode);
    // fpsCap: frames per second the loop will not exceed, 0 = uncapped. A
    // sleep at the end of the frame, billed to the `present` scope as a wait.
    // The way to leave a recorder (OBS) its share of the GPU.
    float fpsCap = TPD(render, fpsCap);
    // Distance in METERS past which a PRIMARY hit takes the cascade shadow
    // (farShadowed) instead of a real per-voxel sun ray (A3).
    //
    // DEFAULT 999 = OFF, because it was measured and it does not pay: 10.35 ms
    // control vs 10.26 ms at 12 m (noise) and 497 ms at 0 m — 48x WORSE, not
    // better. A fine shadow ray terminates on the first blocker a few voxels
    // away; a cascade ray must cross farShadowReach (60 m) at level-1 cell
    // size before it may conclude "unshadowed". See the long comment on
    // sunShadowAt in raymarch.wgsl for why, and for what would actually work.
    // Kept as a knob so the experiment is re-runnable, not as a feature.
    float shadowMaxDist = TPD(render, shadowMaxDist);

    // ---- SHORT-RANGE MODE (dev panel "short range (100 m + fog)") ----
    // A comparison arm against the dense-100 m WebGPU voxel engines
    // (voxelbit.net and friends): they draw ~100 m of 5 cm voxels at 190 fps
    // and hide the cutoff behind fog, where this engine draws a ~6.5 km
    // cascade. The mode is a PERF mode, not a filter: every ray — the fine
    // march and every cascade level — is ceilinged at shortRangeDist, so the
    // frame genuinely stops paying for the horizon instead of fogging it after
    // marching it.
    //
    // WHETHER THE MODE IS ON IS NOT HERE, ON PURPOSE. It is overlay state
    // (Overlay::State::shortRange) and reaches the shader as RenderParams flag
    // bit 2, because the running game writes tuning.json and a view toggle
    // must not be able to clobber the user's saved defaults. These three rows
    // only say what the mode LOOKS like when it is on.
    //
    // Ray ceiling in METERS. Nothing past this is marched at all.
    float shortRangeDist = TPD(render, shortRangeDist);
    // The NEAR arm's ceiling, in metres: the same mode with a tighter wall, so
    // the panel can compare two cutoffs without the tuner (dev panel radio
    // "50 m" vs "100 m"). A SECOND DISTANCE rather than a fraction of the
    // first, because the two are independent comparison points — halving
    // shortRangeDist would silently move this one too the moment the far arm
    // is retuned. Which arm is live is flag bit 4, not a tuning value, for the
    // same reason the mode's on/off is not one.
    float shortRangeNearDist = TPD(render, shortRangeNearDist);
    // Where the fog ramp starts, as a FRACTION of shortRangeDist. Below it the
    // image is unfogged; the mode's whole point is that the near field looks
    // untouched and only the wall dissolves. 0.65 = fog begins at 65 m of 100.
    float shortRangeFogStart = TPD(render, shortRangeFogStart);
    // Steepness of the ramp between the start fraction and the ceiling. The
    // curve is 1 - exp(-density * x^2) renormalised so it reaches EXACTLY 1 at
    // the ceiling — the renormalisation is what stops the mode from trading a
    // geometric cliff for a colour one. Higher = the fog closes sooner and
    // the last few metres are pure sky; lower = a longer, thinner haze.
    float shortRangeFogDensity = TPD(render, shortRangeFogDensity);
  } render;

  // ---- world: which authored map the game loads -------------------------------
  // NOT the terrain. Every number that shapes the ground moved to the map
  // (assets/worldmap/<name>/map.json `terrain`) and to the biome files in
  // P-G of docs/PLAN_environment_truth.md; what is left here is which files.
  struct World {
    // Names assets/worldmap/<mapLayer>/{map.json,map.svmap}: the painted
    // biome/landform planes, the terrain numbers and the site table. NOT
    // optional -- "default" ships, and a missing or unparsable map ABORTS at
    // load rather than silently generating an all-ocean or all-forest world.
    std::string mapLayer = TPD(world, mapLayer);
    // (The edit layer used to be named here as world.editLayer. Since
    // map-overhaul P7 the MAP names it — map.json `editLayer` — because a
    // layer is authored against one map's ground. See sim/worldedit.h.)
  } world;

  // ---- debug: dev switches that are not world content ---------------------------
  struct Debug {
    // 1 = plants generate, 0 = a bare world (trees, cacti, flowers, grass,
    // undergrowth, cover rows, shore/pond/cave flora, alpine cushion, wet moss
    // all off). A frame-rate A/B lever; see tuning_params.def.
    int vegetation = TPD(debug, vegetation);
    // 1 = the small plants generate, 0 = trees and cacti stand on bare ground
    // (cover rows, tile plants, shore rows, pond life, wet moss and cave flora
    // all off). The half of `vegetation` you usually want; ANDed with it, so
    // vegetation = 0 is still the bare world. See tuning_params.def.
    int groundCover = TPD(debug, groundCover);
    // --render-budget's exp1..exp3 arms set this; shader experiments branch on
    // TUNE_PERF_EXP. 0 = the shipped renderer (tuning_params.def says more).
    int perfExp = TPD(debug, perfExp);
  } debug;

  // Values that failed validation, for the overlay / console. Empty on success.
  std::vector<std::string> warnings;
};

// Loads tuning.json over `out` (which starts at the compiled-in defaults, so a
// missing file or a partial JSON is fine — anything absent keeps its default).
// Returns false only on unreadable/unparseable JSON; per-field problems are
// clamped and reported through out.warnings. Every tuning_params.def row is
// read and clamped by generated code; the hand-written remainder is only what
// a row cannot state.
bool LoadTuning(const std::string& path, Tuning& out);

// ---- writing the combat groups back ----------------------------------------
//
// SURGICAL TEXT PATCH, NOT A RE-SERIALIZE. Rewrites only the value literals of
// the keys in the `melee`, `combatfx` and `gore` objects, in the file's own
// text, leaving every other key, every prose "comment" string and the browser
// tuner's formatting exactly as they were. Same choice and same mechanism as
// LabPatchTuningJson (src/lab/lab.h) makes for the fluid look knobs, and it is
// what lets the in-game Combat panel and the browser tuner both own this file
// without either flattening the other's work.
//
// SCOPED TO THE GROUP, which the lab's version is not: that one searches for a
// bare `"key"` anywhere in the file, which is safe for eight uniquely-named
// keys and wrong in general — two groups may reasonably both have a `gain`.
// This finds the group object first and patches only inside its braces.
//
// ALL OR NOTHING. Returns false with `err` set, having written nothing, if a
// group or a key is missing. A half-patched tuning.json is worse than an
// unpatched one, because the half that landed looks like it worked.
bool SaveCombatTuning(const std::string& path, const Tuning& t, std::string& err);

// WGSL `const` declarations for every shader-visible value above, prepended to
// each shader by LoadShader() right after ShaderConstantPrelude(). Shaders
// reference these names rather than literals.
std::string TuningWgslBlock(const Tuning& t);

// Process-wide current tuning. Read by the shader prelude and by the CPU-side
// systems; replaced wholesale on reload.
const Tuning& CurrentTuning();
void SetCurrentTuning(const Tuning& t);

// Any group, by the tuning_params.def name. This is what --sweep uses, so a new
// row is sweepable the moment it exists. Returns false for an unknown group or
// member, and for TP_V3 / TP_S rows (a vec3 has no single float to sweep, a
// string is not a number). Not clamped to the row's range.
bool SetTuningField(Tuning& t, const std::string& group,
                    const std::string& name, float value);

// ---- gore: adding to a wound's whole-voxel budget ---------------------------
// One helper for every site that grows a bleed budget (mob damage, limb carve,
// sever stump, the avatar's equivalents). It applies the volume gain and the
// per-wound ceiling in one place, because the ceiling is the actual bound on
// how much conserved matter one wound can push into the CA (rule 2) and six
// copies of a literal cap is exactly how one of them ends up stale.
//
// `add` is in whole voxels, already scaled by the mob's bleedPerDamage.
inline float AddBleedBudget(float current, float add) {
  const auto& g = CurrentTuning().gore;
  const float grown = current + add * g.bleedVoxelGain;
  return grown > g.bleedBudgetCap ? g.bleedBudgetCap : grown;
}

// ---- gore: what one clump actually costs -----------------------------------
// A BrushOp paints a SOLID SPHERE: sim_mutate.wgsl keeps every cell whose
// dot(local, local) <= radius^2. So a clump radius is a volume, and the wound
// budget has to be debited by that volume or the `bleedBudgetCap` bound means
// nothing the moment clump size leaves 0.
//
// Exact counts rather than the 4/3*pi*r^3 approximation, because at these radii
// the continuous formula is wrong by up to 30% (r=1: 4.2 vs the real 7) and the
// budget is small enough that the error shows. Radii beyond the table are
// clamped by the loader, so the fallback is only reached if that clamp changes.
inline int BleedClumpVoxels(int radius) {
  switch (radius) {
    case 0: return 1;
    case 1: return 7;
    case 2: return 33;
    case 3: return 123;
    default: return radius <= 0 ? 1 : 257;
  }
}

// ---- day/night: the length of one day ---------------------------------------
// Ticks in one SOLAR day, from the tuning cycle length. Sim runs at 30 Hz.
//
// This lives here rather than in test/support.cpp (where it used to) because
// the orbital solver in sim/celestial.cpp needs it and does not link the
// engine's render plumbing. Two copies of "how long is a day" would be a
// silent way for the sky and the sim's reaction gate to disagree.
inline uint32_t TicksPerDayFromTuning(const Tuning& t) {
  int m = t.dayNight.cycleMinutes < 1 ? 1 : t.dayNight.cycleMinutes;
  return (uint32_t)m * 60u * 30u;
}

// SkyState and ComputeSky* now live in sim/celestial.h — the sky is driven by
// a real Keplerian orbital simulation rather than a phase ramp, and that is a
// large enough thing to own a file. Included here so every existing consumer
// of tuning.h keeps seeing SkyState.
#include "sim/celestial.h"
