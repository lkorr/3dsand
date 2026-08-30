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
    float fallDamageSpeed = 8.0f;
    float fallSplatSpeed = 25.0f;
    float fallDamageScale = 0.75f;
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
    // Dangling drop: how far below the held lip the top of the head hangs.
    // NEGATIVE means the head rides ABOVE the lip. The default is negative
    // deliberately: these are chibi rigs — mina's arm chain is ~3.3 voxels
    // against a ~5 voxel shoulder-to-lip gap — and hands can only actually
    // touch the lip (avatar.cpp hang IK, shrug included) with the face at
    // the ledge edge, the way toon games hang. Long-armed rigs tolerate a
    // deeper drop; raise this and the hands stay planted as far as the
    // shrug allows.
    float ledgeHangDrop = -0.15f;
    // Upward velocity of the ARM BOOST — the pull-up used when there is no
    // room to stand on the lip (a rough wall's one-voxel ledge): ballistic,
    // so the next lip up can catch near the apex and the climb chains.
    // Matches jumpSpeed by default so a boost feels like a jump's worth of
    // pull. 0 turns W-on-an-unstandable-lip into simply letting go.
    float ledgeBoostSpeed = 5.25f;
    // Speed and timeout of the committed pull-up onto a standable lip. Same
    // semantics as the water mantle pair above — the timeout exists for a
    // climb blocked partway by a live world. Deliberately SLOW (a body-length
    // climb takes over a second): at the old 4.5 the pull-up read as a big
    // jump, not as hauling yourself up. The timeout must cover the full climb
    // at this speed or it aborts mid-pull.
    float ledgeMantleSpeed = 1.5f;
    float ledgeMantleTime = 2.8f;
    // How fast the body settles into the dead hang after a catch. Split from
    // the mantle speed on purpose: slowing the pull-up must not make the
    // catch itself feel sluggish.
    float ledgeSettleSpeed = 4.5f;
    // Sideways hand-over-hand speed along the ledge (A/D while hanging).
    // Slow by design — it is a traverse, not a strafe. 0 disables.
    float ledgeShimmySpeed = 0.8f;
    // Minimum time a grab hangs before W (held or pressed) pulls up. W is
    // almost always still held from the jump approach, so without this floor
    // the mantle fires on the first hang frame and the catch never appears on
    // screen. 0 restores instant pull-up.
    float ledgePullDelay = 0.25f;
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
    float fallFlailDelay = 0.35f;
    float fallFlailRamp = 0.9f;
    // Meters the body must have DROPPED below the height it last had support at
    // before the fall clip may play at all. Air time alone is not a fall: a
    // step-down clears any debounce, and so does cresting a bump at speed.
    // Distance is the honest question, and it is the one a player would answer.
    float fallMinDrop = 1.2f;
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
    // THE GATE. Contact speed (m/s) below which a landing is not a sound at
    // all. This is the knob that makes a settling pile silent instead of a
    // machine gun, and it is enforced inside the Jolt contact listener, so
    // raising it is genuinely free — rejected contacts never become events.
    // A rock rolling to rest touches down at centimetres per second; a rock
    // that FELL arrives at several m/s, and the gap between them is wide.
    float impactMinSpeed = 2.5f;
    // Contact speed (m/s) that counts as a full-energy impact: gain tops out
    // and pitch bottoms out here. Held above impactMinSpeed by the consumer.
    float impactFullSpeed = 14.0f;
    // Minimum seconds between two impacts from the SAME body. A tumbling rock
    // generates a contact per bounce and per face; without this one fall is a
    // clatter of six identical thuds.
    float impactMinGap = 0.25f;

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

    // The automatic material ambience bed: one positioned loop following the
    // largest nearby body of a material that binds an "ambience" set (water,
    // lava). Unlike the night bed this one IS a thing at a place — it pans and
    // it occludes — so the radius is what decides how far a lake carries.
    float ambienceVolume = 0.7f;
    float ambienceRadius = 40.0f;

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
    float carveChunkiness = 0.65f;
    // Feature size of the correlated noise, in SKIN voxels. This is the size of
    // the lumps that come off. Kept on the skin lattice for the same reason the
    // rim jitter already is: the crater's shape must be a property of the ART,
    // not of whichever collider resolution the engine happened to derive, or
    // the same blast tears differently on two rigs that differ only in scale.
    float carveBlobSize = 3.5f;
    // Exponent on the radial falloff at full chunkiness. Higher concentrates
    // the removal at the blast: at 3, the chance is 0.42 at half the radius and
    // 0.047 at 80% of it, against 0.75 and 0.36 before.
    float carveFalloff = 3.0f;
    // How many SPALL rounds run after the radial pass. Each round takes
    // surviving voxels that are inside the blast and already have enough
    // missing face-neighbours — so a hole grows into its own rim instead of a
    // second blast having to find fresh voxels. This is what makes damage
    // accumulate in one place, and what makes a blast beside an arm take the
    // arm. Bounded and small: each round is one pass over the limb's voxels.
    int carveSpallRounds = 2;
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
    int liquidMinFilm = 1;
    int wanderHopMask = 7;       // critter hop chance = 1/(mask+1) per tick
    // Explosion micro grit: sub-voxel spall thrown alongside the real ejecta.
    // Visual, but spawned BY A SIM KERNEL from the hashed RNG — the roll
    // advances sim state and the droplets can stain, so these are integers in
    // the determinism-critical group and --selftest must be re-run when they
    // change. expMicroScaleIdx indexes microScaleOf's 2/3/4/6 table.
    int expMicroPerMille = 900;
    int expMicroLifeTicks = 40;
    int expMicroScaleIdx = 2;    // 0=2, 1=3, 2=4, 3=6 micro voxels per voxel
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
    int fluidSubsteps = 9;
    float fluidStiffness = 14000.0f;  // EOS stiffness, (vox/s)^2 — the square
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
    float fluidGravity = 900.0f;     // fall acceleration, voxels/s^2. 98.1 is
                                     // Earth at 0.10 m voxels; 900 is ~9x,
                                     // the owner's snappy-water default (see
                                     // stiffness above — the two go together)
    // Density EOS (grantkot MLS-MPM shape): pressure = stiffness *
    // ((rho/rest)^power - 1), clamped below at -cohesion. rho is sampled from
    // the P2G mass grid each substep, so cramming particles into a cavity
    // builds real ejecting pressure instead of saturating a per-particle J.
    float fluidRestDensity = 8.0f;  // particle masses per voxel at rest (8 =
                                    // the 8-per-cell spawn lattice exactly)
    int fluidEosPower = 4;          // integer exponent 1..7; higher = harder
                                    // incompressibility knee, sharper splashes
    float fluidCohesion = 0.0f;     // max NEGATIVE pressure, (vox/s)^2.
                                    // Surface tension: how hard under-dense
                                    // fluid pulls itself together into blobs.
                                    // 0 = water's zero-tension default (the
                                    // EOS floor is then exactly p >= 0);
                                    // non-zero is the honey/goo authoring
                                    // surface — plan §5 item 3.
    // Species interaction, both (vox/s)^2 and SIGNED. attractSame > 0 pulls a
    // particle toward its own species (blobbing/fusing); attractDiff < 0
    // pushes different species apart (immiscible layers that sit on each
    // other instead of interpenetrating), > 0 encourages mixing. Both 0 by
    // default: negative-pressure terms are the classic sticky-ropes look.
    float fluidAttractSame = 0.0f;
    float fluidAttractDiff = 0.0f;
    float fluidViscosity = 0.0f;    // vox^2/s: resists shear via the APIC C
                                    // matrix. 0 = the owner's default, every
                                    // shear-damping term off (APIC's own
                                    // smoothing is the only one left).
                                    // References run 0.02-0.1; 1.5 was syrup.
    float fluidDamping = 0.0f;      // fraction of velocity shed per SECOND
                                    // (0..20). Non-physical settle aid
    float fluidFriction = 0.0f;     // fraction/s of TANGENTIAL velocity shed
                                    // while touching solid (gridUpdate's
                                    // separate BC). 0 = free-slip water;
                                    // authoring knob for mud/goo.
    // Splash coupling (sim_fluid.wgsl g2p): fluid particles that are FAST and
    // at LOW density (spray, breaking crests) shed PFLAG_MICRO droplets into
    // the ballistic particle system, carrying the species' pour material —
    // so MPM blood spatters stains and MPM water is pure sparkle.
    float fluidSplashRate = 4.0f;        // droplets/s per eligible particle
    float fluidSplashSpeed = 18.0f;      // vox/s a particle must exceed
    float fluidSplashMaxDensity = 0.7f;  // eligible below this x rest density
    float fluidSplashLife = 1.1f;        // droplet lifetime, seconds (<= 8.5)
    int fluidSplashScaleIdx = 2;         // droplet size: 0=1/2,1=1/3,2=1/4,3=1/6 vox
    // ---- diffuse material: spray / foam / bubbles ----
    // Ihmsen et al., "Unified Spray, Foam and Bubbles for Particle-Based
    // Fluids" (CGI 2012). Three potentials — trapped air, wave crest, kinetic
    // energy — each clamped to 0..1 by the paper's Phi(), then combined as
    //   n_d = I_k * (kta * I_ta + kwc * I_wc) * dt.
    // Generated particles are classified by local fluid density into spray
    // (ballistic), foam (advected by the fluid, ages out) and bubbles
    // (buoyant, dragged by the fluid) — the classification the paper does by
    // neighbour COUNT, which here is the density the solver already gathered.
    float fluidFoamRate = 90.0f;      // kta: foam particles/s from trapped air
    float fluidFoamCrestRate = 120.0f; // kwc: foam particles/s from wave crests
    float fluidTrappedMin = 1.5f;     // Phi thresholds on the convergence-
    float fluidTrappedMax = 11.0f;    //   weighted relative velocity, vox/s
    float fluidCrestMin = 0.25f;       // Phi thresholds on gated curvature,
    float fluidCrestMax = 2.0f;       //   dimensionless
    float fluidFoamEnergyMin = 8.0f;  // Phi thresholds on kinetic energy,
    float fluidFoamEnergyMax = 260.0f; //   (vox/s)^2
    float fluidFoamLife = 2.2f;       // foam lifetime at full potential, s
    float fluidFoamLifeMin = 0.5f;    // lifetime at the generation threshold, s
    float fluidBubbleBuoyancy = 1.6f; // kb: bubble rise, x gravity, upward
    float fluidFoamDrag = 0.72f;      // kd: how hard the fluid drags foam and
                                      //   bubbles toward its own velocity
    float fluidBubbleDensity = 1.05f; // above this x rest -> bubble
    float fluidSprayDensity = 0.42f;  // below this x rest -> spray
    int fluidFoamScaleIdx = 3;        // foam particle size (0=1/2 .. 3=1/6 vox)
    // ---- MLS-MPM settle / excite seam: the CA <-> particle handover ----
    int fluidExciteMode = 0;      // 0 = disturbance-excite off: the CA owns
                                  // disturbed settled liquid (Phase-2 default,
                                  // keeps the pinned world hash); 1 = settled
                                  // liquid with air below converts to MPM
                                  // particles
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
    int fluidExciteCeiling = 8000;   // most excited particles the seam will
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
    int fluidExciteRate = 4096;      // most particles converted per TICK. Does
                                     // not bind at any shipped ceiling (the
                                     // worldlake ramp to 8,000 takes 46 ticks,
                                     // ~174/tick); it is the guard for the case
                                     // the ceiling cannot cover — a blast that
                                     // exposes thousands of cells at once with
                                     // the ceiling raised. Deliberately equal
                                     // to kMaxFluidSpawnsPerTick: the seam may
                                     // not convert world water faster than the
                                     // MutationQueue can pour it
    int fluidExcitePerch = 0;        // 1 = excite also takes water PERCHED on
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
    int fluidExciteStep = 2;         // SURFACE DISTURBANCE, in whole cells.
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
    float fluidSettledMass = 1.0f;
    float fluidSettleEps = 6.0f;  // vox/s: a fluid block whose FASTEST
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
    float fluidWakeSpeed = 24.0f; // vox/s: grid-node speed at an active/
                                  // settled interface above this excites the
                                  // neighbouring settled liquid (progressive
                                  // wake). Keep ~4x settleEps: the gap is the
                                  // hysteresis
    int fluidSettleTicks = 24;    // consecutive calm ticks before a block
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
    float fluidStainRate = 8.0f;  // chances/s that an excited-fluid contact
                                  // stains an adjacent solid cell — the MPM
                                  // counterpart of CA liquid staining

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
    int waterBodyMode = 0;

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
    int waterBodyMinVolume = 65536;
    int waterBodyExitVolume = 32768;

    // SURFACE HEIGHT SPREAD, whole voxels — the error term of the entire model.
    // A stream down a hillside is ONE connected component with a 200-voxel head
    // difference between its ends: connectivity is a topological fact, an
    // equipotential surface is a hydrostatic one, and the two coincide only at
    // equilibrium. Anything over the enter threshold is a stream and belongs
    // entirely to the CA/MPM.
    //
    // At M1 this is structural rather than measured: only closed analytic
    // basins are registered, and a stream has no basin, so nothing with a real
    // spread can reach the ladder. `--gate waterbody` measures the true spread
    // from voxels and asserts it against these. The runtime measurement lands
    // with M2's GPU reduce, which is the pass that can see a whole lake.
    int waterBodySpreadEnter = 1;
    int waterBodySpreadExit = 4;

    // How long every enter test must hold before adoption. Ticks — 30 is one
    // second. A body still sloshing has a surface that is not an equipotential,
    // and adopting it would freeze that transient into a `level`.
    int waterBodyQuietTicks = 30;

    // Rule 2's bound on the whole feature. At the cap the SMALLEST candidate is
    // refused, and refusal is a safe degradation: unadopted means "simulated the
    // way it is today", never "lost". Hard-capped at kWaterBodyCap (waterbody.h)
    // because the descriptor array's size is a GPU layout from M2 onward.
    int waterBodyMaxCount = 64;

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
    int waterBodyTestDrain = 0;

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
    int drainMaxEighthsPerTick = 512;
    // COMPONENT 7's v1 radius, world cells. The shell is the free-surface disc
    // at the body's level plus the throat column over the hole — NOT a solid
    // ball: plan §9 ranks the ball's ~33,000 particles against a ~40,000
    // measured envelope as the second-most-likely way this work fails. 0
    // disables the shell and is an exact identity (no cell can satisfy it).
    int drainExciteRadius = 6;
    // The two PHYSICAL quantities, human-unit floats in the sanctioned
    // sim.fluid* lane — const-eval'd to fixed point at the top of
    // sim_waterbody.wgsl, so the kernel stays integer (rule 1).
    float drainCd = 0.6f;        // orifice discharge coefficient
    float drainGravity = 900.0f; // vox/s^2; should match sim.fluidGravity

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
    int windMode = 1;
    // Ballistic debris and spray: fraction of the gap between a particle's
    // velocity and the local wind that closes per SECOND, at a material's full
    // windResponse of 15. A drag law rather than a push, because drag is
    // self-limiting — a particle accelerates toward the wind and then stops,
    // so no gust can fling debris faster than the air is moving, whatever the
    // knob says. That bound is why this can be a plain multiplier and does not
    // need a budget.
    float windDrag = 3.0f;
    // MPM grid nodes: how much of the field a fully exposed node feels, as a
    // fraction. Below 1 because a fluid surface is not a free particle — it is
    // dragged by the air, not carried.
    float windFluidGain = 0.35f;
    // ...and which nodes count as exposed: node mass, as a fraction of the mass
    // a node deep inside fluid at rest density carries. Wind fades to nothing
    // as a node approaches this, so it acts on spray and the top skin of a pool
    // and not on its body. Full-body wind on a pond reads as a CURRENT, which
    // is a different phenomenon and the wrong one (research doc §8's open
    // question, answered here in favour of low-mass-only).
    float windFluidMass = 0.5f;
    // CA drift bias: the wind speed at which a full-response material reaches
    // the maximum bias probability, and that maximum. The bias only reorders
    // the direction candidates a moving voxel already tries, so the cap is what
    // keeps wind from becoming a second gravity — at 0.5 a gale still leaves an
    // even chance of the ordinary random order, which is what keeps smoke
    // looking like smoke rather than like a conveyor.
    float windDriftSpeed = 12.0f;
    float windDriftMax = 0.5f;
    // Entrainment (windMode 2): the per-axis wind speed that just lifts a grain
    // whose windFriction is 1; the threshold scales with the authored nibble,
    // so friction 4 needs four times this. Bagnold's fluid threshold, authored.
    float windEntrainSpeed = 2.0f;
    // ...and how often a grain over that threshold actually hops, in chances
    // per second. This is the bound (rule 2): entrainment is a rate, not a
    // certainty, so a dune creeps instead of exploding.
    float windEntrainRate = 6.0f;

    // ---- dev force multipliers, one per TIER ----
    // Not in tuning_params.def, and that is deliberate rather than an
    // omission. That table is "the ONE table of SHADER-FACING tuning
    // parameters" — rows that become const-folded WGSL constants and therefore
    // need F5. These two ride TickParams as Q8 integers instead, because they
    // exist to be DRAGGED: a slider you have to reload a shader to see is a
    // slider nobody moves. (`sim.fluidExciteMode` carries a .def row it does
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
    float windGasScale = 1.0f;
    float windPartScale = 1.0f;
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
    // Here rather than in tuning_params.def for the windGasScale reason — this
    // is the knob you drag WHILE watching an explosion, and one that needs F5
    // between each nudge cannot be judged by eye.
    float windDragRef = 40.0f;
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
    // Here rather than in tuning_params.def for the windGasScale reason: it is
    // read CPU-side per tick, so a gate can set it with no shader reload.
    // Clamped to kWindWakeCap, which is the TickParams array it fills.
    int windWakeChunks = 96;
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
    int cycleMinutes = 20;      // real minutes for one full in-game SOLAR day
    int freeze = 0;             // 1 = pin the cycle at freezePhase
    int freezePhase = 32768;    // 0..65535, 0 = midnight, 32768 = noon

    // ---- the planet ----
    // Axial tilt is the obliquity of the spin axis to the orbital plane, and
    // it is the ENTIRE mechanism behind seasons: the same orbit seen through a
    // tipped equator puts the sun higher in one half of the year than the
    // other. Together with the observer's latitude it fixes noon elevation
    // (90 - |lat - tilt| at the solstice) and day length — which is why it
    // replaced the old `sunPeakElevation` clamp, a knob that set the sun's
    // height while leaving day length wrong.
    float axialTilt = 23.4f;        // degrees
    float latitudeDeg = 42.0f;      // observer latitude, degrees north
    float yearLengthDays = 96.0f;   // in-game days per orbit — the season rate
    // Orbit shape. Near-circular by default: eccentricity mostly shows as the
    // sun changing apparent size and as the equation of time, both subtle.
    float orbitEccentricity = 0.017f;
    float orbitArgPeriapsis = 283.0f;   // degrees — where in the year perihelion falls
    float orbitMeanAnomaly0 = 0.0f;     // degrees — the epoch (tick 0) position
    // Rotates the whole sky about the vertical, i.e. picks which way is east.
    float sunAzimuth = 24.0f;
    // True angular RADIUS of the star as seen at a = 1, in degrees. The real
    // sun is 0.266; larger reads better at game FOV and, since the eclipse
    // test is pure geometry, directly sets how often a moon can cover it.
    float sunAngularRadius = 0.30f;

    // How sharply day turns into night. This is the smoothed daylight weight
    // (R.sunUp) that crossfades the sky, ambient and key light; widening it
    // lengthens twilight.
    float twilightWidth = 0.22f;

    // ---- moon A ----
    // Periods are SYNODIC (new moon to new moon) because that is the cycle a
    // player watches; celestial.cpp derives the sidereal period the orbit is
    // actually integrated with. Authoring the sidereal period instead would
    // make "an 8-day moon" mean an 8.7-day phase cycle.
    int lunarPeriodDays = 8;
    float moonInclination = 5.1f;    // degrees to the ecliptic — see below
    float moonEccentricity = 0.055f;
    float moonArgPeriapsis = 130.0f;
    float moonNode = 0.0f;           // longitude of the ascending node, degrees
    float moonMeanAnomaly0 = 40.0f;  // epoch position, degrees
    // Angular radius in DEGREES at the orbit's mean distance.
    float moonAngularRadius = 1.7f;

    // ---- moon B ----
    // 9 days against A's 8: coprime, so the pair of phases takes 72 days to
    // repeat. Smaller, further out, and on a differently-oriented plane, so
    // the two moons cross each other rather than travelling together.
    int moon2PeriodDays = 9;
    float moon2Inclination = 8.7f;
    float moon2Eccentricity = 0.03f;
    float moon2ArgPeriapsis = 20.0f;
    float moon2Node = 95.0f;
    float moon2MeanAnomaly0 = 200.0f;
    float moon2AngularRadius = 1.05f;

    float starRotSpeed = 1.0f;       // multiplier on the star wheel rate

    // Retained ONLY so an old tuning.json still loads without a warning storm.
    // Nothing reads it; noon elevation is now an output of tilt + latitude.
    float sunPeakElevation = 58.0f;
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
  //     to the shader each frame. They are here rather than in
  //     tuning_params.def because a compile-time constant cannot drift over
  //     minutes, which is exactly what weather has to do.
  //   * gustWavelength / gustSpeed / altitudeGain / altitudeRefY / dbgWind*
  //     ARE in tuning_params.def and const-fold into every shader (F5).
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
    float windSpeed = 6.0f;
    // Direction the wind BLOWS TOWARD, degrees, using the engine's heading
    // convention: 0 = +Z, increasing toward +X. Ignored while weatherAuto is
    // on. This is the knob to turn to prove the field is real — arrows and
    // grass must both swing to follow it.
    float windDirDeg = 45.0f;
    // Gust amplitude as a FRACTION of the mean speed, which is how gustiness
    // actually behaves: a windier day has bigger gusts, not the same gusts on
    // a faster mean. At 1.0 the wind ranges from roughly still to twice the
    // mean; at 0 it is a dead steady breeze and the grass just leans.
    float gustStrength = 1.0f;
    // Let the weather evolve on its own (deterministic chaos keyed on the
    // tick — see WindWeather). Off pins direction and speed to the two knobs
    // above, which is what you want for inspecting the field or comparing
    // screenshots: an evolving field makes two shots incomparable.
    bool weatherAuto = true;

    // ---- field shape (mirrored in tuning_params.def as TUNE_WIND_*) ----
    // Distance between gust crests along the wind, metres. Short wavelengths
    // read as a rippling meadow; long ones as slow rolling swells. 4.8 m
    // reproduces the spatial frequency the sway code shipped with.
    float gustWavelength = 4.8f;
    // Rate of the gust bands. This is the field's clock, shared by every
    // consumer — see the note on render.microSwaySpeed, which is now only a
    // foliage-local trim on top of it.
    float gustSpeed = 1.1f;
    // Fractional wind speed-up per 100 world voxels (10 m) above altitudeRefY.
    // SIGNED both ways: below the reference the boundary layer slows the wind,
    // which is why a valley floor is calmer than the ridge above it. Clamped
    // in the shader to [0.15x, 4x] so a silly value is still a look.
    float altitudeGain = 0.6f;
    // World Y the altitude ramp is measured from. 64 sits mid-terrain
    // (worldgen's band is y32..y86), so ridges get a gain and basins a loss.
    // Absolute Y rather than terrain-relative on purpose: terrain-relative
    // needs a height query at every sample point, and absolute is what makes
    // the field a pure function of position (research doc §8).
    float altitudeRefY = 64.0f;

    // ---- debug slope-field overlay (research doc §4.8) ----
    // Initial state of the arrow overlay; F4 toggles it in-game. It is a
    // tuning knob as well as a key so the field can be inspected from a saved
    // tuning.json and from a headless screenshot run, neither of which can
    // press a key. Costs exactly nothing when off — the draw is skipped, not
    // drawn transparent.
    bool dbgWindField = false;
    // Spacing between arrow lattice points, world voxels. The lattice is
    // snapped to this grid in WORLD space, so the arrows stay put as the
    // camera moves instead of swimming with it.
    float dbgWindSpacing = 8.0f;
    // Radius of the arrow lattice around the camera, world voxels. Cost is
    // cubic in radius/spacing, so this is the knob that decides whether the
    // overlay is free or not: the default 48/8 is 13^3 = 2197 arrows.
    float dbgWindRadius = 48.0f;
  } wind;

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
    // MLS-MPM fluid prototype (debris.wgsl vsFluid). fluidColor is species 0
    // (water); 1..3 are the other pourable species (keys 1-4 in the mpm tool).
    float fluidColor[3] = {0.20f, 0.42f, 0.85f};
    float fluidColor1[3] = {0.92f, 0.34f, 0.10f};
    float fluidColor2[3] = {0.22f, 0.78f, 0.28f};
    float fluidColor3[3] = {0.88f, 0.72f, 0.25f};
    // Cube half-extent per particle, in cells. 0.5 tiles the rest lattice
    // exactly; slightly over closes the gaps so a pool reads as a surface.
    float fluidParticleSize = 0.58f;
    // How much a particle elongates along its velocity (0 = always a cube).
    // Motion blur for free: falling streams read as streaks, not dice.
    float fluidStretch = 0.4f;
    // Albedo darkening with compression (density above rest), so pressure
    // visibly travels through a pool.
    float fluidDensityShade = 0.45f;
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
    float fluidSurface = 2.0f;
    float fluidIso = 0.30f;      // isosurface threshold, fraction of rest
                                 // density. Lower = fatter, more merged fluid
    float fluidSmooth = 1.3f;    // normal-gradient baseline, voxels. Higher
                                 // smooths harder at the cost of small shapes
    // How much of the CA's geometry SUPPORTED fluid borrows (raymarch.wgsl,
    // "TWO MODELS OF THE SAME WATER"). 1 = water resting on ground or on other
    // water is drawn as a height field with its surface at cell.y + fill,
    // exactly where the CA draws `cell.y + (state+1)/8`, so a spreading film is
    // one eighth tall and settling does not make it jump. 0 = the pre-2026-08-25
    // behaviour, one isotropic blob field everywhere. Airborne water (a
    // droplet, a splash arch) is unaffected at any setting — it has nothing
    // underneath it, so the blob model keeps it.
    float fluidLevel = 1.0f;
    float fluidIor = 1.33f;      // refraction index (water 1.33, oil ~1.47)
    float fluidClarity = 1.3f;   // metres of fluid to ~1/e absorption
    float fluidReflect = 1.0f;   // traced/sky reflection gain
    float fluidSpecular = 1.0f;  // sun glint gain
    float fluidFoamSpeed = 22.0f; // surface speed (vox/s) for full churn foam
    float fluidWobble = 0.5f;    // sub-voxel normal shimmer on moving fluid
    // Speed-driven whitening: fast, loose particles read as spray/foam.
    float fluidFoam = 0.35f;
    // ---- depth colour gradient (raymarch.wgsl DEPTH GRADIENT) ----
    // A thin film reads as `fluidShallow`, the deep body tends toward
    // `fluidDeep`, ramped over `fluidDepth` metres of in-fluid path. The ramped
    // colour is what drives the per-channel Beer-Lambert absorption, so the
    // gradient is a real absorption change, not a tint painted on top.
    float fluidShallow[3] = {0.42f, 0.86f, 0.82f};
    float fluidDeep[3] = {0.02f, 0.15f, 0.42f};
    float fluidDepth = 2.6f;     // metres over which the ramp completes
    float fluidGradient = 1.0f;  // 0 = flat species albedo (old look), 1 = full
    // ---- grid foam field (sim_fluid.wgsl foam potentials) ----
    float fluidFoamField = 1.0f;   // gain on the advected foam field's whitening
    float fluidFoamTexture = 0.65f; // fbm break-up of the foam field, 0 = flat
    // Foam PARTICLE colour (debris.wgsl, PPAY_FOAM). Foam is entrained air,
    // not a substance, so it has no material to take an albedo from — it is
    // coloured from here, jittered per particle by foamColorVar so a burst
    // reads as many bubbles rather than one flat white mass.
    float foamColor[3] = {0.97f, 0.98f, 1.0f};
    float foamColorVar = 0.18f;
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
    // Pole of the galactic plane, in the STAR SPHERE's frame (it wheels with
    // the stars). The band is drawn perpendicular to this, so rotating it
    // moves the Milky Way across the constellations.
    float galaxyNormal[3] = {0.36f, 0.52f, -0.77f};
    // Half-width of the band in |cos| from that plane, before the fbm that
    // ragged-edges it. Small values give a tight bright river.
    float galaxyWidth = 0.17f;
    float nebulaStrength = 0.40f;
    float nebulaCool[3] = {0.16f, 0.30f, 0.62f};
    float nebulaWarm[3] = {0.55f, 0.20f, 0.38f};
    // Aurora — the Shivering Isles curtains.
    float auroraStrength = 0.55f;
    float auroraHeight = 900.0f;    // voxels; sets how curtains converge
    float auroraLow[3] = {0.10f, 0.85f, 0.45f};
    float auroraHigh[3] = {0.65f, 0.20f, 0.85f};

    // ---- moons ----
    // NOTE: no moon RADIUS here. Apparent size is an output of the orbit
    // (dayNight.moon*AngularRadius, modulated by orbital distance), because
    // the disc and the eclipse test must read ONE number for how big a moon
    // is. A render-side radius knob would be a second, diverging answer.
    float moonBrightness = 1.6f;
    float moonColor[3] = {0.92f, 0.93f, 0.88f};
    // Offsets the fbm that carves this moon's maria and craters. Not a colour
    // and not a position — it is the only thing making a moon a distinct rock
    // rather than the same face drawn twice, so changing it rerolls the
    // surface wholesale.
    float moonMariaSeed[3] = {4.0f, 1.0f, 9.0f};
    float moonGlow = 0.35f;
    float moonEarthshine = 0.055f;
    float moonLightColor[3] = {0.55f, 0.68f, 1.0f};
    float moonLightIntensity = 0.16f;
    // Moon B: a smaller, colder, dimmer body. Look only.
    float moon2Color[3] = {0.78f, 0.80f, 0.86f};
    float moon2MariaSeed[3] = {-21.0f, 13.0f, 37.0f};
    float moon2Brightness = 0.72f;
    float moon2LightIntensity = 0.055f;
    float moon2LightColor[3] = {0.62f, 0.62f, 0.86f};
    // How dark a TOTAL solar eclipse gets. 1 = the dome falls to its full
    // night value; lower keeps some daylight so totality reads as
    // daytime-gone-wrong rather than as night.
    float eclipseDarkness = 0.93f;
    // Perceptual exponent on covered AREA before that darkening applies. 3 is
    // the old hardcoded cube: the world stays bright until the last sliver of
    // sun goes, which is how a real partial eclipse reads. 1 tracks area
    // linearly and looks like someone sliding the exposure down.
    float eclipseCurve = 3.0f;

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
    // FOLIAGE-LOCAL trim on the wind clock, applied on top of wind.gustSpeed.
    // It used to be the band rate outright; since the wind rewrite the field
    // itself owns that (windAt in common.wgsl, wind.gustSpeed), and this is a
    // multiplier the two sway sites apply to the time they hand it. Default is
    // 1.0 for a reason: at anything else, grass samples the field at a
    // different phase than the debug arrow overlay draws, so the overlay stops
    // being evidence about the grass. Move wind.gustSpeed instead unless you
    // specifically want foliage running off the shared clock.
    float microSwaySpeed = 1.0f;

    // budgets
    int primarySteps = 4096;
    int farSteps = 384;
    // How far a far-field sun shadow ray reaches, in METERS. Converted to a
    // per-level step count by farShadowSteps() in raymarch.wgsl so the reach
    // is the same world distance at every cascade level (a raw step count is
    // not: it scales with the level's cell size — see the comment there).
    float farShadowReach = 60.0f;

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
    float lodHandoffDist = 24.0f;
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
    float shadowMaxDist = 999.0f;
  } render;

  // ---- worldgen (integer; regenerating the world is required to see edits) ----
  struct Worldgen {
    // The scale every LENGTH below is authored at. LoadTuning multiplies those
    // rows by kVoxelsPerMetre / this, so the group means the same physical
    // world at any voxel size. Dimensionless rows (chances, 0..255 thresholds,
    // the Q8 slope) are untouched, because a probability and a gradient do not
    // have a length in them.
    int refVoxelsPerMetre = 10;
    int treeline = 228;
    // The world DATUM. Every octave below is a CENTRED deviation, so terrain
    // sits at baseHeight on average and spans +- half the summed amplitudes.
    int baseHeight = 200;
    // THE OCTAVE LADDER: five rungs, lacunarity 4, persistence 1/4, so every
    // rung has the same amplitude/wavelength ratio of 0.5 and detail is added
    // without adding slope. Amplitudes are the FULL swing in voxels (the field
    // spans +-amp/2); half-ranges sum to 682 voxels = 68 m each way.
    //
    // Noise cells are LOG2 EXPONENTS (11 = 2048 voxels, 3 = 8), which is what
    // lets vnoise2d replace fdiv/fmodp with >> and & — see the Q14 noise block
    // in worldgen.wgsl. LoadTuning clamps every one of them to 3..15.
    int contAmplitude = 1024, contLog2 = 11;
    int rangeAmplitude = 256, rangeLog2 = 9;
    int hillAmplitude = 64, hillLog2 = 7;
    int detailAmplitude = 16, detailLog2 = 5;
    int grainAmplitude = 4, grainLog2 = 3;
    // iq's derivative attenuation, Q8: each octave is scaled by
    // 1/(1 + fbmAtten*|g|^2/256) against the gradient accumulated above it.
    // 0 is plain fBm (five 0.5 slopes summing to 2.5, i.e. the whole world
    // above the CA's angle of repose); 256 is the textbook form. This is a
    // rule-2 mechanism, not a look knob.
    int fbmAtten = 256;
    // The calm home area: the two COARSE octaves fade toward the world origin
    // so spawn is rolling country at spawnPlainY instead of a random point on
    // a mountainside. The three fine octaves stay live, which is what makes it
    // calm rather than flat. spawnPlainFade is load-bearing — a short fade
    // builds a cliff at exactly the boundary; the `terrain` gate's A4 measures
    // it.
    int spawnPlainY = 200, spawnPlainR = 320, spawnPlainFade = 2048;
    // The sediment wedge: low flat ground carries loose dirt over gravel,
    // ridges carry none. thickness = (sedCeil - ground)*sedFraction/256 -
    // sedStrip, slope-gated and clamped to sedMax. Dirt and gravel are
    // POWDERS, so sedSlope is a rule-2 knob — ship it conservative and raise it
    // under `--gate sleep`. sedMax must stay under caveBands' 40-voxel shell or
    // a cavern breaches the wedge from below; LoadTuning enforces that.
    int sedCeil = 264, sedFraction = 64, sedStrip = 6;
    int sedSlope = 96, sedMax = 32, sedTopsoil = 4;
    int biomeLog2 = 9;
    int desertThreshold = 214, pineThreshold = 176, meadowThreshold = 92;
    int treeTile = 144;
    int treeChanceForest = 78, treeChancePine = 70;
    int treeChanceMeadow = 22, treeChanceDesert = 6;
    int autumnFraction = 5;   // 1-in-N broadleaves turn autumn
    int pondTile = 448, pondChance = 4, pondRadiusMin = 48, pondRadiusSpan = 32;
    // Steepest ground a tarn may sit on, |dh/dx|+|dh/dz| in Q8 (256 = the
    // CA's angle of repose). A bowl cut into a slope lays its sand bed on a
    // wall and never settles.
    int pondMaxSlope = 96;
    // The tarn berm: the annulus just outside the disc is forced to
    // (waterline + pondBerm) and ramps back to natural ground over
    // pondBermWidth voxels. This is what makes pond containment STRUCTURAL —
    // the rim-sampling density it replaced was already stale at these radii.
    int pondBerm = 5, pondBermWidth = 14;
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
    // ---- the authored edit layer (src/sim/worldedit.h) ---------------------
    // Names assets/worldedits/<editLayer>.svedit, the hand-built patch the
    // Worldgen tab's voxel view writes. Applied through the MutationQueue to
    // every chunk worldgen produces, so it survives streaming and composes with
    // any seed.
    //
    // EMPTY BY DEFAULT, and no gate may set it: a layer moves the world hash by
    // construction (it puts voxels in the world), so a shipped default would
    // silently re-pin every determinism number in tests/baseline.json.
    std::string editLayer;
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

// The `worldgen` group as the engine's own compiled-in defaults, as JSON — what
// `--dump-tuning-defaults` writes and what the tuner's "Reset terrain" button
// applies. Emitted from a DEFAULT-CONSTRUCTED Tuning, so the lengths are the
// authored values LoadTuning would rescale, not already-rescaled ones. See the
// note over the definition.
std::string WorldgenDefaultsJson();

// Process-wide current tuning. Read by the shader prelude and by the CPU-side
// systems; replaced wholesale on reload.
const Tuning& CurrentTuning();
void SetCurrentTuning(const Tuning& t);

// Set a sim.* field by name (e.g. "windDragRef"). Returns false if the name
// is unknown. For --sweep: lets you test parameter reachability without editing
// tuning.json. Handles both int and float sim fields.
bool SetSimField(Tuning& t, const std::string& name, float value);

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
