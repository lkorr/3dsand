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
struct Tuning {
  // ---- player movement (meters / seconds; converted to voxels at use) ----
  struct Player {
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
    float halfWidth = 0.30f, halfHeight = 0.85f, eyeOffset = 0.65f;
  } player;

  // ---- camera ----
  struct Camera {
    float mouseSensitivity = 0.0022f;  // radians per pixel
    float fovY = 1.2f;                 // radians (~69 deg)
    float pitchClamp = 1.55f;
  } camera;

  // ---- Jolt rigid bodies ----
  struct Physics {
    float gravity = 9.81f;
    int collisionSteps = 1;
    float debrisFriction = 0.75f, debrisRestitution = 0.05f;
    float debrisLinearDamping = 0.05f, debrisAngularDamping = 0.15f;
    float terrainFriction = 0.85f, playerProxyFriction = 0.3f;
    float explosionImpulseScale = 0.15f;
    float explosionImpulseRadiusScale = 3.0f;
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
    float rippleAmpScale = 1.0f, rippleSpeedScale = 1.0f;
    float reflectionCutoff = 0.06f;
    int reflectionSteps = 96;
    float causticGain = 1.5f, causticCap = 0.85f;
    float glintIntensity = 0.85f;
    float glintPowerNear = 180.0f, glintPowerFar = 900.0f;
    float foamDepth = 0.42f, foamStrength = 0.55f;

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
    int pondTile = 224, pondChance = 4, pondRadiusMin = 20, pondRadiusSpan = 17;
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
