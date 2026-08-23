#pragma once
#include <cstdint>
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

struct Tuning {
  // ---- player movement (meters / seconds; converted to voxels at use) ----
  struct Player {
    std::string model = "mina";
    float flySpeed = 13.75f, flySprint = 32.5f;
    float walkSpeed = 4.5f, sprintSpeed = 8.0f;
    float gravity = 9.81f;
    float jumpSpeed = 5.25f;
    float swimUp = 17.5f, swimDown = 7.5f;
    float maxFall = 30.0f;
    float stepUp = 0.58f;
    float smoothBump = 0.12f;
    float stepSpeedPenaltyPerM = 2.8f;
    float minStepSpeedScale = 0.20f;
    float nonJumpSpeed = 0.5f;
    float coyoteTime = 0.12f, jumpBufferTime = 0.12f;
    // Accel/damping are per-second rates, converted to a per-frame lerp with
    // 1-exp(-rate*dt) at the call site. The old code lerped by a raw constant
    // every frame (ground 0.35, air 0.06, liquid 0.15, vertical drag 0.92),
    // which made acceleration and water drag scale with frame rate. These
    // rates are chosen to reproduce exactly those blends at ~100 fps — the
    // speed the game actually runs — so the feel is unchanged where it was
    // tuned, and now stays put at 30 or 144 fps instead of drifting.
    float groundAccel = 43.1f, airAccel = 6.2f, liquidAccel = 16.3f;
    float liquidDrag = 8.3f;
    float liquidGravityScale = 0.25f;
    float liquidSpeedScale = 0.55f;
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
    float waterMantleSpeed = 4.5f;
    // Hard cap (seconds) on one climb. This is a timeout, not a duration: the
    // mantle normally ends on arrival. It exists so a climb blocked partway —
    // the bank collapsed, something shoved into the target — returns control
    // instead of holding movement hostage.
    float waterMantleTime = 0.9f;
    // ---- ledge grab (procedural climbing) ----
    // Airborne with space held and the arms facing a voxel lip within hand
    // reach, the body latches on and dangles; W pulls it up (player.cpp
    // LedgeGrabAhead + the hanging block in Player::Update).
    //
    // How far above the crown of the head the hands reach (meters). A 1.7 m
    // body's standing reach is ~2.25 m, so ~0.55 past the top. Physical like
    // stepUp, so voxel-size changes never change how much real wall is
    // grabbable. 0 disables ledge grabbing entirely.
    float ledgeReach = 0.55f;
    // Dangling drop: the top of the head hangs this far below the held lip
    // (arms extended overhead, slightly bent).
    float ledgeHangDrop = 0.35f;
    // Upward velocity of the ARM BOOST — the pull-up used when there is no
    // room to stand on the lip (a rough wall's one-voxel ledge): ballistic,
    // so the next lip up can catch near the apex and the climb chains.
    // Matches jumpSpeed by default so a boost feels like a jump's worth of
    // pull. 0 turns W-on-an-unstandable-lip into simply letting go.
    float ledgeBoostSpeed = 5.25f;
    // Speed and timeout of the committed pull-up onto a standable lip, and of
    // the settle into the dead hang. Same semantics as the water mantle pair
    // above — the timeout exists for a climb blocked partway by a live world.
    float ledgeMantleSpeed = 4.5f;
    float ledgeMantleTime = 0.9f;
    float halfWidth = 0.30f, halfHeight = 0.85f, eyeOffset = 0.65f;
    // Camera step smoothing: half-life (seconds) of the render-only eye
    // offset that cancels the vertical pop when the body steps up/down a
    // ledge (Player::ViewEyePos). 0 disables. CPU/render only — the physics
    // position and the sim are untouched.
    float viewSmoothHalflife = 0.10f;
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
    float unstickMaxDepth = 0.9f;
    // How fast (m/s) the body rises while being ejected. Rate-limited rather
    // than teleported so a two-voxel lift is a glide, not a pop; the climb is
    // banked into the same view offset a step-up uses.
    float unstickSpeed = 3.0f;
  } player;

  // ---- camera ----
  struct Camera {
    float mouseSensitivity = 0.0022f;  // radians per pixel
    float fovY = 1.2f;                 // radians (~69 deg)
    float pitchClamp = 1.55f;
    // Multiplier on look sensitivity while a melee weapon is up (any swing
    // phase but Idle). The same mouse motion both turns the view and steers
    // the blade (game/melee.h), so at 1.0 a cut you want to watch also whips
    // the camera off the target. Slowing the VIEW while leaving the blade on
    // full gain is what makes a swing readable: the mouse travel buys mostly
    // arm, not mostly yaw. The melee state machine never sees this scale —
    // it is fed the raw delta, so commitSpeed still means true mouse pixels.
    float meleeSensitivity = 0.5f;
    // Half-life (seconds) of the scale easing in and out. Stepping the gain
    // on the click edge is a visible jolt in a mid-turn mouse stroke.
    float meleeSensHalflife = 0.08f;
  } camera;

  // ---- third-person camera rig ----
  //
  // Distances are METERS and converted to voxels at use, exactly like the
  // player block: that is what keeps the framing physically meaningful if
  // kVoxelMeters ever changes. Render-only — the picking ray and every sim
  // input keep using the player's own eye, so nothing here can move the hash.
  struct ThirdPerson {
    float distance = 3.2f;        // boom length behind the focus point
    float shoulderDist = 1.7f;    // boom length in over-shoulder mode
    float shoulderOffset = 0.55f; // lateral offset, over-shoulder mode
    float heightOffset = 0.25f;   // focus point above the head anchor
    float sideOffset = 0.0f;      // lateral offset in plain third person
    // Collision: the boom is swept against the voxel world and pulled in to
    // the first hit, minus this margin, so the near plane never clips inside
    // a wall. `collideRadius` fattens the sweep so the camera does not slip
    // through a one-voxel gap and pop to the far side.
    float collideMargin = 0.35f;
    float collideRadius = 0.25f;
    bool collide = true;
    // Smoothing half-lives, seconds. The focus point is smoothed so the
    // camera does not jitter with every step bob; the boom length is smoothed
    // separately and ASYMMETRICALLY — pulling IN must be instant (or the
    // camera spends a frame inside the wall) while pushing back OUT is eased,
    // which is the standard fix for a camera that pops when clearing a corner.
    float focusHalflife = 0.06f;
    float distInHalflife = 0.0f;   // 0 = snap in immediately
    float distOutHalflife = 0.25f;
    // Extra pitch-driven lift: at steep downward pitch the boom rises so the
    // character stays framed instead of being hidden by its own hat.
    float pitchLift = 0.35f;
    // How strongly the dismemberment state's body drop moves the camera.
    // 1 = follow the pose exactly, 0 = ignore it. Below 1 the camera stays a
    // little higher than a crawling body, which reads better than lying on
    // the floor with it.
    float stateFollow = 0.75f;
    // Field-of-view widening with speed, radians at full sprint. Sells speed
    // without the player touching a setting.
    float speedFov = 0.06f;
    float speedFovHalflife = 0.35f;
  } thirdPerson;

  // ---- player avatar ----
  struct Avatar {
    // Which mob def the avatar rig is loaded from. Data, not code: pointing
    // this at another def in assets/mobs swaps the player character whole.
    // Not hot-reloadable by itself — it is read when the avatar is (re)spawned.
    bool enabled = true;
    // Body facing. In third person the body turns toward its MOTION and only
    // faces the camera when the player aims, which is what stops the character
    // from moon-walking sideways. This is the turn rate, radians/sec.
    float turnRate = 12.0f;
    // Below this speed (m/s) the body keeps its last facing instead of
    // snapping to a near-zero velocity vector, which would spin on the spot.
    float turnMinSpeed = 0.35f;
    // In first person the body is hidden, but the ARMS are kept so the player
    // can see their own hands and staff. Turning this off hides everything.
    bool firstPersonArms = true;
    // Vertical offset applied to the whole avatar relative to the player AABB,
    // in meters. The rig's own feet should land on the box's bottom face; this
    // is the trim for art whose contact point is not exactly at its origin.
    float footTrim = 0.0f;
    // ---- motion smoothing ----
    // The rig's own measured speed drives cadence, bob, sway, roll, the
    // walk/run clip choice, the spring goals and the swing budget — so any
    // noise in it is amplified into every one of those at once. Half-life in
    // seconds (frame-rate independent): the measurement covers half the
    // remaining distance to the truth every this-many seconds. 0 disables the
    // smoothing entirely and uses the raw per-tick measurement.
    float velocityHalflife = 0.08f;
    // Half-life (seconds) of the FIRST-PERSON body yaw. Third person has its
    // own rate limit (turnRate above) because you are watching the body pivot;
    // first person used to snap outright, on the reasoning that the body is
    // off-screen — but the ARMS are not, and they are welded to the torso, so
    // a fast mouse turn steps them across the view in hard jumps. A short
    // half-life keeps them attached to the view without visible lag. 0 restores
    // the old hard snap.
    float firstPersonTurnHalflife = 0.05f;
    // ---- head look (the body does not turn until the neck runs out) --------
    // How far the HEAD may yaw away from the body's facing before the BODY
    // has to start turning, in degrees. Inside this cone a mouse turn is a
    // glance: only the head (and a fraction of the spine) rotates, the feet
    // stay planted and the arms stay where they were. Past it the body is
    // dragged along so that the offset never exceeds this angle — which is
    // why there is no separate "recenter" rate and no hysteresis to chatter
    // on: the constraint is geometric, not a state machine.
    float headLookYaw = 70.0f;
    // Head pitch range, degrees up/down. The camera pitch clamp is ~89°, and
    // a neck does not do that, so this clamps separately.
    float headLookPitchUp = 55.0f;
    float headLookPitchDown = 60.0f;
    // Fraction of the head's yaw that is ALSO applied to the spine, so a look
    // to the side twists the chest a little instead of swivelling a head on a
    // rigid torso. Small on purpose: the arms are welded to the spine, so this
    // moves a held weapon across the screen. 0 = head only.
    float headLookSpine = 0.25f;
    // Half-life (seconds) of the head easing to the look angle. This is what
    // keeps the head from stepping with the raw mouse; the body's own
    // firstPersonTurnHalflife sits behind it.
    float headLookHalflife = 0.07f;
    // Half-life (seconds) of the FIRST-PERSON body squaring back up to the
    // view while the player WALKS. Without this the head-look cone is a drift
    // trap: inside the cone the body's turn is dropped to zero, so its facing
    // is never driven back to anything and it freezes wherever the last big
    // turn left it — with the arms welded to the torso, they end up stuck
    // pointing off-view while you walk somewhere else. Standing still there is
    // deliberately no recentring; that is the glance. Longer feels looser;
    // 0 squares the body up immediately whenever you move.
    float headLookRecenterHalflife = 0.35f;
    // Half-life (seconds) of the leg IK fading in and out as the gait starts
    // and stops. `grounded` is genuinely ragged crossing bumpy ground — the
    // body really does leave the surface cresting each bump — and switching the
    // IK on that as a bool snapped the limbs between the IK pose and the rest
    // hang every time, which is what "the arms shoot up straight going uphill"
    // is. Longer is smoother but makes the legs slower to commit to the ground
    // on landing; 0 restores the old hard switch.
    float ikBlendHalflife = 0.08f;
    // How long (seconds) the body must be continuously off the ground before
    // the air-state clips believe it. `grounded` drops false for a tick at a
    // time cresting bumps, and the jump/fall/land clips used to fire on that
    // raw edge — so walking up a noisy incline retriggered the arms-up `jump`
    // one-shot over and over, which is the tweaking. A real jump clears this in
    // one tick; a bump crest never does. Too high and a genuine jump animates
    // late. 0 restores the old undebounced behaviour.
    float airDebounce = 0.12f;
    // Damage/dismemberment feel.
    float severImpulse = 6.0f;   // extra shove given to a part as it comes off
    // How long the corpse's parts stay before the avatar can respawn, seconds.
    float respawnDelay = 3.0f;
  } avatar;

  // ---- sound ----
  // CPU-only: nothing here reaches a shader, so there is no TUNE_* emitter and
  // no entry in scripts/tuning_prelude.py. Read through CurrentTuning() on the
  // GAME thread and copied into the audio layer once per frame — the audio
  // thread must never touch this struct, since F5 replaces it wholesale
  // (see src/audio/voice.h for the threading contract).
  struct Audio {
    bool enabled = true;
    float masterVolume = 0.8f;

    // Footsteps. `volume` is the overall trim; per-material trims multiply it.
    float footstepVolume = 0.85f;
    // Audible radius in meters — the distance at which a step falls to the
    // gain floor. Steps are small sounds; a big radius makes them carry
    // unnaturally and wastes voices on inaudible ones.
    float footstepRadius = 22.0f;
    // Step pitch is randomized per trigger to hide sample repetition. This is
    // the half-range: 0.06 means each step lands in [0.94, 1.06] of natural
    // rate. Too much and the surface changes identity step to step.
    float footstepPitchJitter = 0.06f;
    // Loudness at walking pace vs at sprint. Speed maps between them, so a
    // sneak is quiet and a sprint is not merely faster but heavier.
    float footstepWalkGain = 0.55f;
    float footstepSprintGain = 1.0f;
    // Speed (m/s) that counts as a full sprint for the mapping above.
    float footstepSprintSpeed = 7.0f;
    // Left and right feet are pitched apart by this fraction so a gait reads
    // as two feet rather than one repeated impact.
    float footstepFootDetune = 0.03f;

    // Landing after a fall: gain scales with impact speed up to this speed
    // (m/s), which also caps the pitch drop.
    float landVolume = 1.0f;
    float landFullSpeed = 12.0f;

    // Physical impacts (debris, bodies).
    float impactVolume = 0.9f;
    float impactRadius = 30.0f;

    // Something coming apart: terrain losing support and detaching as a
    // rigidbody, or being dug/blasted loose.
    float breakVolume = 1.0f;
    // Breaks carry further than impacts — a tree limb giving way is a loud,
    // low event and hearing it from off-screen is most of its value.
    float breakRadius = 45.0f;
    // Half-range of the per-event random detune, in SEMITONES. Expressed in
    // semitones rather than as a rate multiplier because that is the unit the
    // ear (and whoever is tuning this) actually thinks in; cues.cpp converts
    // with 2^(n/12). 5 is wide — deliberately, since one support scan can free
    // several islands in the same tick and identical repeats read as a
    // machine. Beyond ~7 the material stops sounding like itself.
    float breakPitchSemitones = 5.0f;
    // Pitch centre by piece size: a lone voxel snapping is a twig, a large
    // island is a log. These are the rate multipliers at the two ends, and a
    // piece's voxel count maps between them (see breakBigVoxels).
    float breakSmallRate = 1.25f;
    float breakBigRate = 0.8f;
    // Voxel count that counts as "big" for the mapping above.
    float breakBigVoxels = 400.0f;
    // Creature voices (hurt/death/sever/...). Kept separate from impacts
    // because a mob crying out and a rock landing are mixed against each
    // other, and one trim cannot serve both.
    float mobVolume = 1.0f;
    float mobRadius = 45.0f;
    float mobPitchJitter = 0.07f;

    // The blade cut itself, separate from the creature's cry (which is the
    // `sever` slot on mobVolume). Its own trim because a wet mechanical sound
    // and a voice sit differently in the mix, and a wider jitter because four
    // takes have to cover a whole fight.
    float dismemberVolume = 1.0f;
    float dismemberPitchJitter = 0.12f;

    // Bleeding: a positioned wet loop while a wound is pumping hard.
    float bleedVolume = 0.9f;
    float bleedRadius = 18.0f;
    // Intensity (0..1 of the bleed budget cap) to START the loop, and the
    // lower level it must fall back through to STOP. Two thresholds, not one:
    // a wound sitting exactly on a single threshold retriggers the voice every
    // frame. On > off is required and enforced at load.
    float bleedOnThreshold = 0.35f;
    float bleedOffThreshold = 0.12f;

    // The night bed (assets/sounds/ambience/starlight). Rare by design.
    float nightVolume = 0.5f;
    // Audible radius. Large because the bed is centred on the listener and is
    // meant to sit around them rather than to come from a place.
    float nightRadius = 60.0f;
    // Chance, per retry, that a new pass begins. With the defaults below that
    // is one roll a minute at 8%, so most nights stay silent and the bed is an
    // event rather than a backing track.
    float nightChance = 0.08f;
    float nightRetrySeconds = 60.0f;
    // Per-frame easing factor for the fade in and out. Small = slow: the bed
    // should arrive and leave without the player catching either moment.
    float nightFadeRate = 0.004f;

    // Reverb send for world sounds, 0..1. The engine's FDN reverb is what
    // makes a cave read as a cave; keep it modest for outdoor-heavy worlds.
    float reverbWet = 0.16f;

    // ---- occlusion ----
    // See src/audio/occlusion.h for the model. These are the knobs that decide
    // how much a wall between you and a sound matters.
    bool occlusion = true;
    float occlusionMaxDb = 24.0f;      // cap on the broadband duck
    float occlusionMinCutoffHz = 320.0f;  // fully-muffled low-pass floor
    float occlusionScale = 1.0f;       // multiplies the accumulated dB
    float occlusionCutoffScale = 1.0f; // <1 = darker through walls
    float occlusionMaxRangeM = 40.0f;  // never trace a ray longer than this
    // How much of the reverb send survives an occluded path. High values keep
    // a blocked sound present-but-muffled instead of switching it off.
    float occlusionWetKeep = 0.7f;
  } audio;

  // ---- Jolt rigid bodies ----
  struct Physics {
    float gravity = 9.81f;
    int collisionSteps = 1;
    float debrisFriction = 0.75f, debrisRestitution = 0.05f;
    float debrisLinearDamping = 0.05f, debrisAngularDamping = 0.15f;
    float terrainFriction = 0.85f, playerProxyFriction = 0.3f;
    float explosionImpulseScale = 0.15f;
    float explosionImpulseRadiusScale = 3.0f;
    // How far an explosion actually BLOWS VOXELS OFF bodies, as a multiple of
    // the destruction radius. Kept separate from the impulse reach on purpose:
    // the blast should push objects from further away than it dismembers them,
    // so this is normally the smaller of the two.
    float explosionBodyDamageScale = 1.0f;
    // Player proxy mass: the shove-strength knob. Contact impulses split by
    // mass ratio, so this vs a body's density-derived mass decides how far a
    // walking player moves it.
    float playerMassKg = 80.0f;
    // Rolling spheres (analytic colliders, not boxed voxels).
    float sphereFriction = 0.5f, sphereRestitution = 0.3f;
    float sphereAngularDamping = 0.05f;
  } physics;

  // ---- debris / island -> rigidbody conversion ----
  struct Debris {
    int minBodyVoxels = 8;
    int minBurnFragmentVoxels = 24;
    int maxNewBodiesPerTick = 4;
    int settleAfterTicks = 60;
    float alignCos = 0.94f;
    int maxBodies = 200;
    int burnOpsPerTick = 384;
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
    int microLifeTicks = 70;
    int microScale = 4;

    // ---- A2. spray from an open wound (the drip's companion) ----
    // Droplets per whole blood voxel a wound drips. Bleeding already drips real
    // voxels into the grid; this is the visible spray that accompanies each
    // drip, so it multiplies an existing, already-bounded rate.
    float bleedSprayPerDrip = 3.0f;
    float bleedSpraySpeed = 3.5f;      // voxels/sec, upward-biased cone
    float bleedSprayCone = 0.55f;      // lateral spread as a fraction of speed

    // ---- A3. spray from a dismemberment (the arterial gout) ----
    // `severSpray` droplets are emitted over `severDecayTicks`, front-loaded so
    // the gout is at the cut and the tail dies down — a flat rate over the same
    // window reads as a sprinkler rather than a wound.
    int severSpray = 220;
    int severDecayTicks = 45;
    float severSpraySpeed = 9.0f;
    float severSprayCone = 0.8f;

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
    float bleedVoxelGain = 1.0f;
    float bleedBudgetCap = 120.0f;   // max voxels one wound can still owe
    // Voxels added to the stump's budget when a limb comes off, on top of the
    // thrown sever voxels below. This is the puddle under a fresh amputation.
    float severStumpBudget = 40.0f;

    // ---- B2. how fast that budget leaves the wound, and in what size lumps ----
    // Rate is a PERIOD, not a chance, because bleeding must stay bounded per
    // rule 2: a wound drips at most once every `bleedDripTicks`, and at most
    // `bleedOpsPerTick` drips happen across all limbs of all mobs in a tick.
    int bleedDripTicks = 4;      // ticks between drips from one wound
    int bleedOpsPerTick = 6;     // global op budget for drips, per tick
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
    int bleedClumpRadius = 0;
    // Whole blood VOXELS thrown by a cut, alongside the sub-voxel gout. Kept
    // small next to the hundreds of micro droplets — the spray does the visual
    // work, these do the lasting mess.
    int severVoxels = 14;
    float severVoxelSpeed = 6.0f;
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
    int severGobbetVoxels = 1;
    // How far apart a gobbet's members are spread at launch, in voxels. Zero
    // stacks them on one cell, where the claim lattice lets exactly one win and
    // the rest retry next tick — a slow-motion drip instead of a lump. A small
    // jitter gives them distinct target cells so they land together.
    float severGobbetSpread = 0.6f;

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
    float bleedGain = 1.0f;
    Variance bleedGainVar;
  } gore;

  // ---- grenade ----
  struct Grenade {
    float throwSpeed = 20.0f;   // m/s
    float fuse = 2.2f;          // seconds
    float restitution = 0.45f;
    float friction = 0.8f;
    float waterDrag = 0.90f;
    int blastRadius = 13, blastPower = 380;
  } grenade;

  // ---- tools ----
  struct Tools {
    int detonateRadius = 12, detonatePower = 340;
    float laserRange = 200.0f;
    int laserMeltRadius = 2;
    // Carve radius when the beam is on LIVING flesh, in WORLD voxels — float,
    // and deliberately sub-voxel by default. This is the precision dial for
    // surgery: at mob scale 4 a micro voxel is 0.25 world voxels, so 0.3 bores
    // a channel roughly one micro voxel wide, while the same beam still melts
    // a 2-voxel hole in stone. Flesh is cut, not blasted.
    float laserCarveRadius = 0.3f;
    float laserDamage = 1.5f;
    float brushAirDistance = 48.0f;
  } tools;

  // ---- integer sim constants: DETERMINISM-CRITICAL (CLAUDE.md rule 1) ----
  // Emitted into the WGSL prelude as integers. Changing any of these changes
  // the world hash; --selftest must be re-run.
  struct Sim {
    int partGravity = 22;        // 24.8 fixed voxels/tick^2
    int partMaxVel = 1536;       // 24.8 fixed voxels/tick
    int airDensity = 10;         // density below which things rise
    int falloffPerCell = 6;      // explosion power lost per cell
    int ejectSolid = 250;        // per-mille of destroyed voxels that fly
    int ejectLiquid = 500;
    int ejectPowder = 350;
    int ejectGas = 0;
    int liquidEqualize = 2;      // eighths a neighbor must be emptier to flow
    int wanderHopMask = 7;       // critter hop chance = 1/(mask+1) per tick
    // Explosion micro grit: sub-voxel spall thrown alongside the real ejecta.
    // Visual, but spawned BY A SIM KERNEL from the hashed RNG — the roll
    // advances sim state and the droplets can stain, so these are integers in
    // the determinism-critical group and --selftest must be re-run when they
    // change. expMicroScaleIdx indexes microScaleOf's 2/3/4/6 table.
    int expMicroPerMille = 900;
    int expMicroLifeTicks = 40;
    int expMicroScaleIdx = 2;    // 0=2, 1=3, 2=4, 3=6 micro voxels per voxel
    // MLS-MPM fluid prototype (sim_fluid.wgsl). In the sim group because they
    // are integers feeding a deterministic solver — they do NOT touch the
    // world hash (the fluid never writes voxels) but they DO change the
    // fluid_det gate's particle hash, so re-run --selftest after changing.
    int fluidStiffness = 393216;  // EOS stiffness E, Q16.16 (6.0). Higher =
                                  // less compressible water, stronger substep
                                  // impulses (CFL clamps keep it stable).
    int fluidGravity = 7144;      // Q16.16 cells/tick^2 (0.109 = 9.81 m/s^2
                                  // at 0.10 m voxels, 30 Hz ticks)
  } sim;

  // ---- day/night cycle ----
  // The cycle phase is derived from the SIM TICK (see DayPhaseForTick in
  // world.h), not from wall clock, because the daylight-gated reactions make
  // sunlight feed voxel state. cycleMinutes and the freeze controls therefore
  // change WHEN reactions fire — they are render-and-sim, and a change to them
  // changes the world hash. They are integers for the same reason.
  struct DayNight {
    int cycleMinutes = 20;      // real minutes for one full in-game day
    int freeze = 0;             // 1 = pin the cycle at freezePhase
    int freezePhase = 32768;    // 0..65535, 0 = midnight, 32768 = noon
    // Sun path. The sun rises in +X and sets in -X, tracking an arc whose peak
    // elevation is set by `sunPeakElevation` (degrees) and whose orbital plane
    // is tilted by `sunAzimuth` (degrees) — together these are latitude and
    // season, and they are what decide how long shadows get at noon.
    float sunPeakElevation = 58.0f;
    float sunAzimuth = 24.0f;
    // How sharply day turns into night. This is the smoothed daylight weight
    // (R.sunUp) that crossfades the sky, ambient and key light; widening it
    // lengthens twilight.
    float twilightWidth = 0.22f;
    // Moon orbit: lunarPeriodDays days per full phase cycle, inclined off the
    // sun's plane so it is not simply opposite the sun.
    int lunarPeriodDays = 8;
    float moonInclination = 18.0f;   // degrees off the solar plane
    float starRotSpeed = 1.0f;       // multiplier on the star wheel rate
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
  struct Weather {
    // Exposed water freezes to ice on clear nights (the shore-inward frontier
    // rule in reactions.json). Off leaves ponds liquid through the night.
    bool waterFreezes = true;
    // Snow and ice in direct daylight melt back to water. Off makes winter
    // permanent — note that leaving this off while waterFreezes is on means
    // ice only ever accumulates, which is stable but one-way.
    bool iceMelts = true;
  } weather;

  // ---- render: everything below is emitted as WGSL and F5-reloadable ----
  struct Render {
    // sky / sun
    float skyGradient = 1.4f, skyHorizonOffset = 0.25f;
    float skyHorizon[3] = {0.72f, 0.80f, 0.90f};
    float skyZenith[3] = {0.25f, 0.47f, 0.85f};
    float sunTint[3] = {1.0f, 0.9f, 0.7f};
    float sunDiscPower = 800.0f, sunDiscGain = 3.0f;
    float sunHaloPower = 8.0f, sunHaloGain = 0.12f;
    float sunDir[3] = {0.50f, 0.55f, 0.38f};
    float sunColor[3] = {1.0f, 0.95f, 0.86f};
    float sunIntensity = 1.35f;

    // ---- atmospheric sky (physically-flavoured scattering model) ----
    // Rayleigh scales the molecular scattering that makes the sky blue and the
    // sunset red; Mie is the forward-scattering haze that puts a glow around
    // the sun. These two, plus the air-mass curve, replace the old two-colour
    // lerp and are what let one model cover noon, sunset and night.
    float skyRayleigh = 12.0f;
    float skyMie = 1.0f;
    float skyMieG = 0.76f;          // Mie anisotropy; higher = tighter halo
    float skyMieStrength = 1.0f;
    float skyExposure = 1.6f;
    float skyGround[3] = {0.22f, 0.20f, 0.17f};  // below-horizon bounce
    // Multiplier on the true 0.53 deg disc. 1.0 is physically correct and
    // reads as a pinprick on a 16:9 screen at a game FOV — every engine that
    // wants the sun to be a PRESENCE oversizes it. 3x is about the smallest
    // that still looks deliberate rather than like a dead pixel.
    float sunSize = 3.0f;
    // How hard the atmosphere reddens a low sun. Scales the extinction that
    // colours BOTH the sun disc and the dome, and is deliberately separate
    // from skyRayleigh: that one sets how blue the sky is, and sharing one
    // constant between them makes a rich blue sky imply a permanently orange
    // sun (it did — the whole dome came out khaki).
    float sunReddening = 1.0f;

    // ---- night sky ----
    float nightZenith[3] = {0.006f, 0.010f, 0.028f};
    float nightHorizon[3] = {0.030f, 0.036f, 0.062f};
    // MLS-MPM fluid prototype droplet albedo (debris.wgsl vsFluid).
    float fluidColor[3] = {0.20f, 0.42f, 0.85f};
    float starBrightness = 1.0f;
    float starDensity = 150.0f;     // direction-grid cells per unit
    // PSF core radius in PIXELS, not radians. Sizing in pixels is what keeps a
    // star a point at any resolution/FOV; the first version used a fixed
    // angular radius ~4x the SUN's, which read as nearby blobs with visible
    // pixel steps across their falloff.
    float starSize = 0.85f;
    // Fraction of grid cells that hold a star (per layer; the fine layer uses
    // 1.7x this). Low on purpose — filling a fifth of the grid is TV static.
    float starSparsity = 0.012f;
    float starTwinkle = 0.35f;
    float milkyWayStrength = 0.55f;
    float milkyWayColor[3] = {0.52f, 0.56f, 0.78f};
    float nebulaStrength = 0.40f;
    float nebulaCool[3] = {0.16f, 0.30f, 0.62f};
    float nebulaWarm[3] = {0.55f, 0.20f, 0.38f};
    // Aurora — the Shivering Isles curtains.
    float auroraStrength = 0.55f;
    float auroraHeight = 900.0f;    // voxels; sets how curtains converge
    float auroraLow[3] = {0.10f, 0.85f, 0.45f};
    float auroraHigh[3] = {0.65f, 0.20f, 0.85f};

    // ---- moon ----
    float moonRadius = 0.030f;      // angular radius, radians (~5x the real one)
    float moonBrightness = 1.6f;
    float moonColor[3] = {0.92f, 0.93f, 0.88f};
    float moonGlow = 0.35f;
    float moonEarthshine = 0.055f;
    float moonLightColor[3] = {0.55f, 0.68f, 1.0f};
    float moonLightIntensity = 0.16f;

    // ---- night ambient ----
    float nightAmbSky[3] = {0.055f, 0.075f, 0.135f};
    float nightAmbGround[3] = {0.022f, 0.026f, 0.042f};

    // fog
    float fogOpticalDepths = 4.5f;
    float fogLerpPerFrame = 0.08f;

    // ambient / diffuse
    float ambSky[3] = {0.40f, 0.48f, 0.62f};
    float ambGround[3] = {0.25f, 0.22f, 0.17f};
    float diffuseWrap = 0.55f;
    float faceX = 0.96f, faceZ = 0.92f;

    // AO
    float aoStrength = 0.45f;
    float aoFar = 0.72f;

    // shadows
    float shadowBias = 0.02f;
    int shadowSteps = 384;
    float shadowSoftNear = 0.6f, shadowSoftFar = 9.0f, shadowLift = 0.45f;
    float shadowFarLift = 0.3f;

    // grain
    float grainBroadScale = 11.0f, grainFineScale = 2.5f;
    float grainMix = 0.68f;
    float grainAmp = 0.065f, grainAmpFar = 0.05f;

    // media / smoke
    float mediaAbsorb = 6.4f, mediaTauMax = 6.0f;

    // fire
    float fireFlickerBase = 0.70f, fireFlickerAmp = 0.55f, fireFlickerRate = 13.0f;
    float fireGlowRate = 1.4f, fireIntensity = 2.1f;
    float fireBreatheAmp = 0.08f, fireBreatheRate = 5.3f;
    float emissiveStrength = 1.7f;
    float emissiveFlickerBase = 0.82f, emissiveFlickerAmp = 0.28f,
          emissiveFlickerRate = 9.0f;

    // water
    float waterF0 = 0.0204f;
    float waterAbsorb[3] = {1.85f, 0.42f, 0.20f};
    float waterScatter[3] = {0.045f, 0.16f, 0.20f};
    float waterFresnelPower = 5.0f;
    // Global calm-down of the travelling wave field. Water reads as still,
    // glassy water at rest rather than as a windswept sea; the column-height
    // gradient still gives real bodies their macro shape, so lowering these
    // makes water calm, not flat.
    float rippleAmpScale = 0.35f, rippleSpeedScale = 0.40f;
    // Fetch gate (waterOpenness in raymarch.wgsl): fraction of a 12-tap
    // horizontal ring that must be liquid before travelling waves appear.
    // Below LOW a surface is a droplet or puddle and stays perfectly still.
    float waterFetchLow = 0.35f, waterFetchHigh = 0.85f;
    float reflectionCutoff = 0.06f;
    int reflectionSteps = 96;
    float causticGain = 1.5f, causticCap = 0.85f;
    float glintIntensity = 0.85f;
    float glintPowerNear = 180.0f, glintPowerFar = 900.0f;
    float foamDepth = 0.42f, foamStrength = 0.55f;

    // translucent solids — ice, glass (shadeTranslucent in raymarch.wgsl).
    // A solid is translucent when its authored `opacity` is < 255; these
    // control what that translucency LOOKS like. Absorption is per metre of
    // real path through the slab, so one number covers "thin ice is clear"
    // and "thick ice is deep cyan" at once.
    float iceF0 = 0.021f;           // head-on reflectance (ice IOR 1.31)
    float iceFresnelPower = 5.0f;   // Schlick exponent
    float iceAbsorb = 3.2f;         // absorption gain per metre, x opacity
    float iceAbsorbFloor = 0.06f;   // floor so even clear ice tints slightly
    float iceScatter = 0.30f;       // internal bubble/grain scatter strength
    float iceScatterDepth = 1.6f;   // how fast scatter saturates with depth
    float iceScatterNight = 0.25f;  // scatter retained with the sun down
    float iceGrain = 0.09f;         // frost normal perturbation amplitude
    float iceGrainScale = 0.35f;    // frost noise frequency (world space)
    float iceGloss = 190.0f;        // specular exponent (higher = tighter)
    float iceSpec = 0.55f;          // specular highlight strength
    float iceDepthMax = 3.0f;       // metres of ice past which the march stops
    // Fresnel weight below which the traced reflection is replaced by a plain
    // sky lookup. Unlike water, a translucent SOLID can present many surfaces
    // to one ray, so an ungated reflection here is a frame-time cliff.
    float iceReflectMin = 0.12f;

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
    float subAbsorb[3] = {0.42f, 0.11f, 0.075f};   // per metre, per channel
    float subScatter[3] = {0.055f, 0.19f, 0.24f};  // colour the volume tends to
    float subScatterGain = 1.0f;    // in-scatter strength multiplier
    // Metres at which the view has fully faded to the scatter colour. The
    // underwater analogue of fog distance: this is the "murky pond" vs "clear
    // tropical water" knob.
    float subVisibility = 11.0f;
    float subVignette = 0.34f;      // screen-edge darkening while submerged
    // Snell's window: from below, the entire sky is compressed into a ~97
    // degree cone straight up, and outside it the surface is a mirror of the
    // murk. This scales how bright that window reads.
    float subSnellGain = 1.25f;

    // caustics cast onto submerged surfaces (bedCaustic in raymarch.wgsl).
    // Separate from causticGain/Cap, which drive the caustic seen looking DOWN
    // through a surface from dry land. This is a different projection — from
    // the surface directly above the LIT POINT rather than above the bed the
    // primary ray found — and sharing one gain makes one of the two views
    // always wrong.
    float bedCausticGain = 2.4f;
    float bedCausticCap = 1.5f;
    float bedCausticFade = 6.0f;    // metres of water above, past which it washes out
    float bedCausticSharp = 2.2f;   // higher = thinner, brighter filaments

    // volumetric light shafts (godRays in raymarch.wgsl). Ray-marched with a
    // real per-sample occlusion test, so shafts break around the shore and any
    // overhang instead of passing through terrain. Sample count is a direct
    // frame-time multiplier, but on SUBMERGED pixels only.
    int godRaySteps = 14;
    float godRayStrength = 0.55f;
    // Henyey-Greenstein asymmetry. Shafts are far brighter looking toward the
    // sun than away from it; that anisotropy is what makes them read as beams
    // rather than as a uniform brightening of the whole volume.
    float godRayAniso = 0.62f;
    float godRayRange = 14.0f;      // metres the shaft march covers
    int godRayShadowSteps = 20;     // steps for the per-sample occlusion ray

    // drifting particulate. Render-only motes suspended in the water, which is
    // what gives the light shafts something visible to catch.
    float siltDensity = 0.55f;
    float siltBrightness = 0.50f;
    float siltDrift = 0.05f;

    // how strongly the underside of the surface ripples the view of the sky
    float subSurfaceRipple = 1.6f;

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
    float subMurkVis = 0.55f;      // visibility (m) in a fully opaque liquid
    float subVisCurve = 2.2f;      // clarity exponent for visibility only
    float subAbsorbGain = 7.0f;    // opacity -> per-metre absorption
    float subAbsorbFloor = 0.05f;  // so even a clear liquid is not a vacuum
    // How much of its own colour a liquid scatters back at the eye, at the
    // dense and clear ends. In a dense liquid, that scatter IS what you see.
    float subScatterDense = 0.42f, subScatterClear = 0.16f;
    // Clarity band over which a liquid crosses from the derived profile onto
    // water's hand-tuned coefficients. Water sits at clarity ~0.79, oil ~0.25;
    // widening this band makes more liquids inherit water's look.
    float subClearLow = 0.62f, subClearHigh = 0.82f;

    // Faint directional glow toward the surface when submerged in a medium
    // too dense to see through. A near-opaque liquid gates off Snell's window,
    // and what that left was a featureless field of colour with no sense of up
    // and nothing in motion - honest, but it reads as a broken shader rather
    // than as being under the oil.
    float subMurkGlow = 2.2f;

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
    float oilSatLow = 0.50f, oilSatHigh = 0.78f;
    // Oil's IOR (~1.47 vs water's 1.33) puts F0 at roughly double water's, and
    // unlike blood it approaches a real mirror at grazing - that hard bright
    // rim is the look, not the artifact blood's lower graze guards against.
    float oilF0 = 0.043f, oilGraze = 0.97f;
    // Tighter lobe than blood's: a smooth film gives a small hard glint where
    // a rough suspension gives a broad soft one, and that narrowness is most
    // of what the eye reads as "glossy" rather than "damp".
    float oilGloss = 620.0f, oilSheen = 1.6f;
    // How much the reflection is tinted by the liquid itself. Near zero: a
    // petroleum film is a near-NEUTRAL dark mirror, so what you see in it is
    // the sky and the far bank, not a brown wash of its own body colour.
    float oilReflectTint = 0.12f;
    // How far the body colour is pushed toward black. Petroleum absorbs nearly
    // everything entering it and reflects the rest off the surface - the
    // opposite of blood's bright backscatter, and the term that kills the beige.
    float oilDarken = 0.35f;
    // Thin-film interference (the rainbow slick): strength, and the spatial
    // scale of the film-thickness field that sets the band spacing.
    float oilIridescence = 0.16f, oilFilmScale = 1.1f;
    // The sheen appears ONLY where oil floats on a DENSER liquid - a film needs
    // two interfaces close together, and a deep pool on rock has no second one
    // within reach of the light (floatingOnLiquid in raymarch.wgsl). This
    // scales how much denser the layer below must be to count as a real
    // boundary; oil 900 on water 1000 is a ratio of 0.111.
    float oilFloatSens = 9.0f;
    // Silhouette-feather width for oil, against blood's 0.28. Blood can afford
    // a wide fade because its body colour reads through the blend; oil's body
    // is nearly black, so the same fade leaves a droplet as a smear of the
    // scene behind it. This is most of why oil looked see-through.
    float oilEdgeBand = 0.07f;
    // How much plain sky reflection an UNPOOLED oil surface returns. A droplet
    // is a tiny curved mirror scattering the sky everywhere, so far less
    // reaches the eye than off a flat pool; at 1.0 a grazing droplet returns
    // full-brightness sky and reads as a hole in the world.
    float oilDropReflect = 0.30f;

    // blood / viscous liquids (shadeViscous in raymarch.wgsl)
    float bloodF0 = 0.030f;        // head-on reflectance
    float bloodGraze = 0.55f;      // grazing reflectance (water goes to 1.0)
    float bloodAbsorb = 55.0f;     // opacity -> per-metre absorption
    float bloodTransmit = 0.35f;   // how much of the surface behind shows through
    // Hard ceiling on that transmission. Beer-Lambert alone leaves a lone
    // droplet (path << one voxel) half-transparent no matter how absorbing the
    // material is; blood is opaque at sub-millimetre scale because it
    // backscatters, and this models that. Raise it and blood becomes red glass.
    float bloodMaxTransmit = 0.06f;
    float bloodDepthRamp = 22.0f;  // metres^-1: bright thin -> dark deep
    float bloodPoolLow = 0.18f, bloodPoolHigh = 0.55f;  // droplet <-> pool ramp
    float bloodEdgeFeather = 0.10f;  // field value below which the rim fades out
    float bloodSmooth = 1.0f;   // field-gradient baseline in voxels (anti-faceting)
    float bloodWobble = 0.004f;    // surface-tension wobble (NOT wind ripples)
    float bloodSheen = 1.15f;      // wet highlight strength
    float bloodSheenDrop = 32.0f;  // specular exponent on a droplet (broad)
    float bloodSheenPool = 220.0f; // ... and on a pool (tight)
    float bloodAmbientSheen = 0.35f;  // sky-lit sheen, so it reads wet in shade
    float bloodEdgeDepth = 0.035f;    // metres of column counted as "thin edge"
    float bloodEdgeStrength = 0.65f;
    float bloodEdgeTint[3] = {0.55f, 0.40f, 0.38f};

    // stains (applyStain in raymarch.wgsl)
    float stainCoverage = 1.35f;      // how fast amount turns into coverage
    float stainMottle = 0.85f;        // splatter break-up (0 = flat wash)
    float stainMottleScale = 0.55f;   // noise frequency of that break-up
    float stainDarken = 0.55f;        // how much a stain darkens its substrate
    float stainOpacity = 0.70f;       // how far it goes to the pure stain colour
    float stainSheen = 0.55f;         // wet highlight on a fresh stain
    float stainSheenPower = 90.0f;

    // lava
    float lavaCrackFreq = 2.4f;
    float lavaCrackKneeLow = 0.50f, lavaCrackKneeHigh = 0.90f;
    float lavaWarmBias = 0.035f;
    float lavaEmissionGain = 1.9f;
    float lavaPulseAmp = 0.06f, lavaPulseRate = 0.9f;
    float heatSpillStrength = 0.16f;

    // embers (sub-voxel points; see emberGlow in raymarch.wgsl for why the
    // splat radius is clamped to ~1/4 voxel and brightness is area-compensated)
    float emberBrightness = 2.2f;
    float emberRise = 26.0f, emberRate = 3.4f;
    int emberDensity = 84;  // 0..255 threshold; higher = more sparks

    // tonemap
    float exposureWhite = 4.2f;
    float bleachAmount = 0.9f;
    float gamma = 2.2f;

    // static micro-detail (traceMicro in raymarch.wgsl)
    // Distance in METRES past which a micro cell is drawn as a plain voxel
    // instead of running its nested DDA. At 0.0625 m voxels a cell subtends
    // one pixel at ~110 m for a 1080p 90-degree view, so anything past that is
    // paying a 3*subdiv-step march to decide the colour of a sub-pixel — the
    // LOD is not an approximation there, it is the same answer for less.
    float microLodDist = 40.0f;
    // Cap on nested micro marches per primary ray. A ray grazing a meadow can
    // cross dozens of grass cells, and each one that MISSES keeps the ray
    // alive, so without a cap one pixel can pay for the whole field. Past the
    // cap a micro cell is treated as SOLID (not as air), because terminating
    // the ray is bounded and correct-ish while letting it fly is neither.
    int microMaxPerRay = 8;
    // Wind bend at a swaying plant's TIP, in sub-voxels (subdiv 8 => 1.25 cm
    // each). Clamped to 2.0: the models keep a 2-sub-voxel margin from their
    // cell walls, and anything past that shears blade tips through the wall
    // where the nested DDA never marches them — they vanish, not clip.
    float microSwayAmp = 1.5f;
    // Base wind frequency in rad/s. The shader derives its second flutter
    // band from this (x1.73), so one knob moves the whole gait of the field.
    float microSwaySpeed = 1.1f;

    // budgets
    int primarySteps = 4096;
    int farSteps = 384;
  } render;

  // ---- worldgen (integer; regenerating the world is required to see edits) ----
  struct Worldgen {
    int treeline = 72;
    int baseHeight = 32;
    int hillAmplitude = 42, hillWavelength = 64;
    int detailAmplitude = 12, detailWavelength = 16;
    int biomeScale = 384;
    int desertThreshold = 214, pineThreshold = 176, meadowThreshold = 92;
    int treeTile = 144;
    int treeChanceForest = 78, treeChancePine = 70;
    int treeChanceMeadow = 22, treeChanceDesert = 6;
    int autumnFraction = 5;   // 1-in-N broadleaves turn autumn
    int pondTile = 448, pondChance = 4, pondRadiusMin = 68, pondRadiusSpan = 60;
    // Bowl depth in VOXELS: pondDepth at the centre, pondDepthRim at the edge.
    // At kVoxelMeters 0.10 the player is 17 voxels tall, so a pond has to reach
    // roughly 20 before you can actually submerge in one — the previous
    // 8-voxel bowl was 0.8 m and could only be waded through.
    int pondDepth = 26, pondDepthRim = 3;
    // Pond vegetation. Each is a 1-in-N placement roll per candidate column,
    // plus a height in voxels where the plant is more than one cell tall.
    // These are ordinary inert solids placed once at generation: nothing here
    // grows or reacts, so a settled pond still sleeps (rule 2).
    int lilyChance = 22, lilyFlowerChance = 5;
    int reedChance = 130, reedHeight = 16;
    int kelpChance = 120, kelpHeight = 10;
    // Shoreline: the wet fringe OUTSIDE the pond disc, which used to go
    // straight from water to plain hillside grass. shoreBand is how many
    // voxels past the rim the fringe reaches AND the sole cost knob for
    // shoreAt() — it is the width of the tile-edge strip where a column has to
    // consult a second pond tile, so it must stay well under pondTile.
    // shoreMudWidth is the (shorter) inner ring where the ground skin becomes
    // wet mud instead of grass. The rest are 1-in-N placement rolls per shore
    // column, same inert-solid contract as the pond vegetation above.
    int shoreBand = 24, shoreMudWidth = 10;
    // shoreLift is the VERTICAL half of the band: how far above the waterline
    // a column may stand and still be shore. pondSurface is min(rim) - 2, so
    // most of a rim is well above the water and a fringe cut by radius alone
    // paints marsh up the abutting hillside; cutting on height instead puts
    // the reed beds in the shallow bays. High leverage — at the default pond
    // it keeps 5% of the raw band at 4 and 99% at 24.
    int shoreLift = 12;
    int shoreCattailChance = 12, shoreCattailReach = 9, shoreCattailHeight = 20;
    int shoreSedgeChance = 4;
    int shoreHorsetailChance = 10, shoreHorsetailHeight = 9;
    int shoreIrisChance = 34;
    int shoreMossChance = 3;         // 1-in-N wet stone faces wear moss
    // Vines, climbers and hanging moss. Same shape as the pond vegetation and
    // for the same reason: a 1-in-N roll per candidate COLUMN (never per cell,
    // or the strands come out dashed), placing inert solids once at
    // generation. Nothing here grows, so a settled forest still sleeps.
    // The geometry is derived implicitly from the tree that hosts it — the
    // canopy underside is solved in closed form from the crown parameters, so
    // a strand costs one integer sqrt and no extra world scan.
    int vineChance = 26;             // 1-in-N canopy columns carry a strand
    int vineLenMin = 10, vineLenSpan = 26;   // strand length, voxels
    int creeperFlowerChance = 9;     // 1-in-N strands are a flowering creeper
    int mossChance = 14;             // 1-in-N columns carry a moss beard
    int mossLenMin = 4, mossLenSpan = 9;
    int ivyChance = 2;               // 1-in-N bole-shell cells grow ivy
    int ivyTwist = 5;                // ivy rope spiral, 1/16 turn per voxel
    int wallIvyDensity = 3;          // 1..8, arena + ruin stone-wall coverage
    // ---- desert / pine highland / alpine ground cover ----
    // Percent of 2.5 m tiles in the desert that hold a cactus, and the percent
    // of those that are tall saguaro columns rather than ground-level barrels.
    // Saguaros are landmarks: keep them occasional or the desert reads as a
    // planted grid rather than as somewhere you cross to find one.
    int cactusChance = 26, saguaroFraction = 22;
    // 1-in-N per desert column, inside desertPatch. Tussock is the common
    // species (it is what makes bare sand read as ground rather than as a
    // texture); scrub is the sparse woody accent.
    int tussockChance = 9, scrubChance = 26;
    int desertPatch = 130;           // vnoise 0..255 gate; higher = barer
    // 1-in-N per pine-highland column, inside heathPatch: the huckleberry and
    // juniper floor under a conifer stand.
    int heathChance = 7, heathPatch = 128;
    // 1-in-N per column above TREELINE. The sparsest density here on purpose —
    // the snowline is meant to read as harsh, so this is the one knob that can
    // undo the intent of the whole alpine band by being made generous.
    int alpineChance = 40;
    int ruinChance = 5;
    int caveThreshold1 = 150, caveThreshold2 = 148;
  } worldgen;

  // Values that failed validation, for the overlay / console. Empty on success.
  std::vector<std::string> warnings;
};

// Loads tuning.json over `out` (which starts at the compiled-in defaults, so a
// missing file or a partial JSON is fine — anything absent keeps its default).
// Returns false only on unreadable/unparseable JSON; per-field problems are
// clamped and reported through out.warnings.
bool LoadTuning(const std::string& path, Tuning& out);

// WGSL `const` declarations for every shader-visible value above, prepended to
// each shader by LoadShader() right after ShaderConstantPrelude(). Shaders
// reference these names rather than literals.
std::string TuningWgslBlock(const Tuning& t);

// Process-wide current tuning. Read by the shader prelude and by the CPU-side
// systems; replaced wholesale on reload.
const Tuning& CurrentTuning();
void SetCurrentTuning(const Tuning& t);

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

// ---- day/night: celestial state for one tick --------------------------------
// Everything the renderer needs about the sky at a given day phase. Derived
// purely from the integer phase (and the lunar day count), so two machines at
// the same tick compute the same sky — and, more importantly, the same
// daylight weight that the sim's reactions are gated on.
struct SkyState {
  float sunDir[3];    // unit, toward the sun
  float moonDir[3];   // unit, toward the moon
  float dayT;         // 0..1, 0 = midnight
  float sunUp;        // smoothed 0..1 daylight weight (drives all crossfades)
  float moonPhase;    // 0 = new, 0.5 = full
  float starRot;      // radians
};

// phase is the integer day phase (0..kDayPhaseMask); dayNumber counts elapsed
// in-game days and drives the lunar phase. Both come from the tick.
SkyState ComputeSkyState(const Tuning& t, uint32_t phase, uint32_t dayNumber);
