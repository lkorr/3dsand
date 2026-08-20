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
