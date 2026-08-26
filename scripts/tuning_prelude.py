#!/usr/bin/env python3
"""Emit the WGSL tuning constants, mirroring TuningWgslBlock() in tuning.cpp.

GENERATED FILE -- do not edit. The table lives in src/sim/tuning_params.def
and this file is produced from it by scripts/gen_tuning_prelude.py, which is
also what keeps the names, the TYPES and the DEFAULTS identical to the ones
the engine compiles in. Edit the .def and re-run the generator.

check_shaders.sh has to compile each shader exactly the way LoadShader() does,
and LoadShader prepends this block after the world prelude. Rather than
re-parse JSON in bash, the script shells out here.

Values are read from assets/materials/tuning.json; anything missing falls back
to the same default the C++ struct carries, so the two agree on a partial file.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TUNING = os.path.join(ROOT, "assets", "materials", "tuning.json")

# (group, key, wgsl_name, kind, default) -- kind is 'f', 'i', 'u' or 'v3'.
SPEC = [
    # sky / sun
    ("render", "skyGradient", "TUNE_SKY_GRADIENT", "f", 1.4),
    ("render", "skyHorizonOffset", "TUNE_SKY_HORIZON_OFFSET", "f", 0.25),
    ("render", "skyHorizon", "TUNE_SKY_HORIZON", "v3", [0.72, 0.8, 0.9]),
    ("render", "skyZenith", "TUNE_SKY_ZENITH", "v3", [0.25, 0.47, 0.85]),
    ("render", "sunTint", "TUNE_SUN_TINT", "v3", [1.0, 0.9, 0.7]),
    ("render", "sunDiscPower", "TUNE_SUN_DISC_POWER", "f", 800.0),
    ("render", "sunDiscGain", "TUNE_SUN_DISC_GAIN", "f", 3.0),
    ("render", "sunHaloPower", "TUNE_SUN_HALO_POWER", "f", 8.0),
    ("render", "sunHaloGain", "TUNE_SUN_HALO_GAIN", "f", 0.12),
    ("render", "sunColor", "TUNE_SUN_COLOR", "v3", [1.0, 0.95, 0.86]),
    ("render", "sunIntensity", "TUNE_SUN_INTENSITY", "f", 1.35),

    # atmospheric sky
    ("render", "skyRayleigh", "TUNE_SKY_RAYLEIGH", "f", 12.0),
    ("render", "skyMie", "TUNE_SKY_MIE", "f", 1.0),
    ("render", "skyMieG", "TUNE_SKY_MIE_G", "f", 0.76),
    ("render", "skyMieStrength", "TUNE_SKY_MIE_STRENGTH", "f", 1.0),
    ("render", "skyExposure", "TUNE_SKY_EXPOSURE", "f", 1.6),
    ("render", "skyGround", "TUNE_SKY_GROUND", "v3", [0.22, 0.2, 0.17]),
    ("render", "sunSize", "TUNE_SUN_SIZE", "f", 3.0),
    ("render", "sunReddening", "TUNE_SUN_REDDENING", "f", 1.0),

    # night sky
    ("render", "nightZenith", "TUNE_NIGHT_ZENITH", "v3", [0.006, 0.01, 0.028]),
    ("render", "nightHorizon", "TUNE_NIGHT_HORIZON", "v3", [0.03, 0.036, 0.062]),
    ("render", "fluidColor", "TUNE_FLUID_COLOR", "v3", [0.2, 0.42, 0.85]),
    ("render", "fluidColor1", "TUNE_FLUID_COLOR1", "v3", [0.92, 0.34, 0.1]),
    ("render", "fluidColor2", "TUNE_FLUID_COLOR2", "v3", [0.22, 0.78, 0.28]),
    ("render", "fluidColor3", "TUNE_FLUID_COLOR3", "v3", [0.88, 0.72, 0.25]),
    ("render", "fluidParticleSize", "TUNE_FLUID_PARTICLE_SIZE", "f", 0.58),
    ("render", "fluidStretch", "TUNE_FLUID_STRETCH", "f", 0.4),
    ("render", "fluidDensityShade", "TUNE_FLUID_DENSITY_SHADE", "f", 0.45),
    ("render", "fluidFoam", "TUNE_FLUID_FOAM", "f", 0.55),
    ("render", "fluidSurface", "TUNE_FLUID_SURFACE", "f", 2.0),
    ("render", "fluidIso", "TUNE_FLUID_ISO", "f", 0.3),
    ("render", "fluidSmooth", "TUNE_FLUID_SMOOTH", "f", 1.3),
    ("render", "fluidLevel", "TUNE_FLUID_LEVEL", "f", 1.0),
    ("render", "fluidIor", "TUNE_FLUID_IOR", "f", 1.33),
    ("render", "fluidClarity", "TUNE_FLUID_CLARITY", "f", 1.3),
    ("render", "fluidReflect", "TUNE_FLUID_REFLECT", "f", 1.0),
    ("render", "fluidSpecular", "TUNE_FLUID_SPECULAR", "f", 1.0),
    ("render", "fluidFoamSpeed", "TUNE_FLUID_FOAM_SPEED", "f", 22.0),
    ("render", "fluidWobble", "TUNE_FLUID_WOBBLE", "f", 0.5),
    ("render", "fluidShallow", "TUNE_FLUID_SHALLOW", "v3", [0.42, 0.86, 0.82]),
    ("render", "fluidDeep", "TUNE_FLUID_DEEP", "v3", [0.02, 0.15, 0.42]),
    ("render", "fluidDepth", "TUNE_FLUID_DEPTH", "f", 2.6),
    ("render", "fluidGradient", "TUNE_FLUID_GRADIENT", "f", 1.0),
    ("render", "fluidFoamField", "TUNE_FLUID_FOAM_FIELD", "f", 1.0),
    ("render", "fluidFoamTexture", "TUNE_FLUID_FOAM_TEXTURE", "f", 0.65),
    ("render", "foamColor", "TUNE_FOAM_COLOR", "v3", [0.97, 0.98, 1.0]),
    ("render", "foamColorVar", "TUNE_FOAM_COLOR_VAR", "f", 0.18),
    ("render", "starBrightness", "TUNE_STAR_BRIGHTNESS", "f", 1.0),
    ("render", "starDensity", "TUNE_STAR_DENSITY", "f", 150.0),
    ("render", "starSize", "TUNE_STAR_SIZE", "f", 0.85),
    ("render", "starSparsity", "TUNE_STAR_SPARSITY", "f", 0.012),
    ("render", "starTwinkle", "TUNE_STAR_TWINKLE", "f", 0.35),
    ("render", "milkyWayStrength", "TUNE_MILKYWAY_STRENGTH", "f", 0.55),
    ("render", "milkyWayColor", "TUNE_MILKYWAY_COLOR", "v3", [0.52, 0.56, 0.78]),
    ("render", "galaxyNormal", "TUNE_GALAXY_NORMAL", "v3", [0.36, 0.52, -0.77]),
    ("render", "galaxyWidth", "TUNE_GALAXY_WIDTH", "f", 0.17),
    ("render", "nebulaStrength", "TUNE_NEBULA_STRENGTH", "f", 0.4),
    ("render", "nebulaCool", "TUNE_NEBULA_COOL", "v3", [0.16, 0.3, 0.62]),
    ("render", "nebulaWarm", "TUNE_NEBULA_WARM", "v3", [0.55, 0.2, 0.38]),
    ("render", "auroraStrength", "TUNE_AURORA_STRENGTH", "f", 0.55),
    ("render", "auroraHeight", "TUNE_AURORA_HEIGHT", "f", 900.0),
    ("render", "auroraLow", "TUNE_AURORA_LOW", "v3", [0.1, 0.85, 0.45]),
    ("render", "auroraHigh", "TUNE_AURORA_HIGH", "v3", [0.65, 0.2, 0.85]),

    # moons
    ("render", "moonBrightness", "TUNE_MOON_BRIGHTNESS", "f", 1.6),
    ("render", "moonColor", "TUNE_MOON_COLOR", "v3", [0.92, 0.93, 0.88]),
    ("render", "moonMariaSeed", "TUNE_MOON_MARIA_SEED", "v3", [4.0, 1.0, 9.0]),
    ("render", "moonGlow", "TUNE_MOON_GLOW", "f", 0.35),
    ("render", "moonEarthshine", "TUNE_MOON_EARTHSHINE", "f", 0.055),
    ("render", "moonLightColor", "TUNE_MOON_LIGHT_COLOR", "v3", [0.55, 0.68, 1.0]),
    ("render", "moonLightIntensity", "TUNE_MOON_LIGHT_INTENSITY", "f", 0.16),
    ("render", "moon2Color", "TUNE_MOON2_COLOR", "v3", [0.78, 0.8, 0.86]),
    ("render", "moon2MariaSeed", "TUNE_MOON2_MARIA_SEED", "v3", [-21.0, 13.0, 37.0]),
    ("render", "moon2Brightness", "TUNE_MOON2_BRIGHTNESS", "f", 0.72),
    ("render", "moon2LightIntensity", "TUNE_MOON2_LIGHT_INTENSITY", "f", 0.055),
    ("render", "moon2LightColor", "TUNE_MOON2_LIGHT_COLOR", "v3", [0.62, 0.62, 0.86]),
    ("render", "eclipseDarkness", "TUNE_ECLIPSE_DARKNESS", "f", 0.93),
    ("render", "eclipseCurve", "TUNE_ECLIPSE_CURVE", "f", 3.0),

    # night ambient
    ("render", "nightAmbSky", "TUNE_NIGHT_AMB_SKY", "v3", [0.055, 0.075, 0.135]),
    ("render", "nightAmbGround", "TUNE_NIGHT_AMB_GROUND", "v3", [0.022, 0.026, 0.042]),

    # ambient / diffuse
    ("render", "ambSky", "TUNE_AMB_SKY", "v3", [0.4, 0.48, 0.62]),
    ("render", "ambGround", "TUNE_AMB_GROUND", "v3", [0.25, 0.22, 0.17]),
    ("render", "diffuseWrap", "TUNE_DIFFUSE_WRAP", "f", 0.55),
    ("render", "faceX", "TUNE_FACE_X", "f", 0.96),
    ("render", "faceZ", "TUNE_FACE_Z", "f", 0.92),

    # AO / shadows
    ("render", "aoStrength", "TUNE_AO_STRENGTH", "f", 0.45),
    ("render", "aoFar", "TUNE_AO_FAR", "f", 0.72),
    ("render", "shadowBias", "TUNE_SHADOW_BIAS", "f", 0.02),
    ("render", "shadowSteps", "TUNE_SHADOW_STEPS", "i", 384),
    ("render", "shadowSoftNear", "TUNE_SHADOW_SOFT_NEAR", "f", 0.6),
    ("render", "shadowSoftFar", "TUNE_SHADOW_SOFT_FAR", "f", 9.0),
    ("render", "shadowLift", "TUNE_SHADOW_LIFT", "f", 0.45),
    ("render", "shadowFarLift", "TUNE_SHADOW_FAR_LIFT", "f", 0.3),

    # grain
    ("render", "grainBroadScale", "TUNE_GRAIN_BROAD_SCALE", "f", 11.0),
    ("render", "grainFineScale", "TUNE_GRAIN_FINE_SCALE", "f", 2.5),
    ("render", "grainMix", "TUNE_GRAIN_MIX", "f", 0.68),
    ("render", "grainAmp", "TUNE_GRAIN_AMP", "f", 0.065),
    ("render", "grainAmpFar", "TUNE_GRAIN_AMP_FAR", "f", 0.05),

    # media / fire
    ("render", "mediaAbsorb", "TUNE_MEDIA_ABSORB", "f", 6.4),
    ("render", "mediaTauMax", "TUNE_MEDIA_TAU_MAX", "f", 6.0),
    ("render", "fireFlickerBase", "TUNE_FIRE_FLICKER_BASE", "f", 0.7),
    ("render", "fireFlickerAmp", "TUNE_FIRE_FLICKER_AMP", "f", 0.55),
    ("render", "fireFlickerRate", "TUNE_FIRE_FLICKER_RATE", "f", 13.0),
    ("render", "fireGlowRate", "TUNE_FIRE_GLOW_RATE", "f", 1.4),
    ("render", "fireIntensity", "TUNE_FIRE_INTENSITY", "f", 2.1),
    ("render", "fireBreatheAmp", "TUNE_FIRE_BREATHE_AMP", "f", 0.08),
    ("render", "fireBreatheRate", "TUNE_FIRE_BREATHE_RATE", "f", 5.3),
    ("render", "emissiveStrength", "TUNE_EMISSIVE_STRENGTH", "f", 1.7),
    ("render", "emissiveFlickerBase", "TUNE_EMISSIVE_FLICKER_BASE", "f", 0.82),
    ("render", "emissiveFlickerAmp", "TUNE_EMISSIVE_FLICKER_AMP", "f", 0.28),
    ("render", "emissiveFlickerRate", "TUNE_EMISSIVE_FLICKER_RATE", "f", 9.0),

    # water
    ("render", "waterF0", "TUNE_WATER_F0", "f", 0.0204),
    ("render", "waterAbsorb", "TUNE_WATER_ABSORB", "v3", [1.85, 0.42, 0.2]),
    ("render", "waterScatter", "TUNE_WATER_SCATTER", "v3", [0.045, 0.16, 0.2]),
    ("render", "waterFresnelPower", "TUNE_WATER_FRESNEL_POWER", "f", 5.0),
    ("render", "rippleAmpScale", "TUNE_RIPPLE_AMP_SCALE", "f", 0.35),
    ("render", "rippleSpeedScale", "TUNE_RIPPLE_SPEED_SCALE", "f", 0.4),
    ("render", "waterFetchLow", "TUNE_WATER_FETCH_LOW", "f", 0.35),
    ("render", "waterFetchHigh", "TUNE_WATER_FETCH_HIGH", "f", 0.85),
    ("render", "reflectionCutoff", "TUNE_REFLECTION_CUTOFF", "f", 0.06),
    ("render", "reflectionSteps", "TUNE_REFLECTION_STEPS", "i", 96),
    ("render", "causticGain", "TUNE_CAUSTIC_GAIN", "f", 1.5),
    ("render", "causticCap", "TUNE_CAUSTIC_CAP", "f", 0.85),
    ("render", "glintIntensity", "TUNE_GLINT_INTENSITY", "f", 0.85),
    ("render", "glintPowerNear", "TUNE_GLINT_POWER_NEAR", "f", 180.0),
    ("render", "glintPowerFar", "TUNE_GLINT_POWER_FAR", "f", 900.0),
    ("render", "foamDepth", "TUNE_FOAM_DEPTH", "f", 0.42),
    ("render", "foamStrength", "TUNE_FOAM_STRENGTH", "f", 0.55),
    ("render", "iceF0", "TUNE_ICE_F0", "f", 0.021),
    ("render", "iceFresnelPower", "TUNE_ICE_FRESNEL_POWER", "f", 5.0),
    ("render", "iceAbsorb", "TUNE_ICE_ABSORB", "f", 3.2),
    ("render", "iceAbsorbFloor", "TUNE_ICE_ABSORB_FLOOR", "f", 0.06),
    ("render", "iceScatter", "TUNE_ICE_SCATTER", "f", 0.3),
    ("render", "iceScatterDepth", "TUNE_ICE_SCATTER_DEPTH", "f", 1.6),
    ("render", "iceScatterNight", "TUNE_ICE_SCATTER_NIGHT", "f", 0.25),
    ("render", "iceGrain", "TUNE_ICE_GRAIN", "f", 0.09),
    ("render", "iceGrainScale", "TUNE_ICE_GRAIN_SCALE", "f", 0.35),
    ("render", "iceGloss", "TUNE_ICE_GLOSS", "f", 190.0),
    ("render", "iceSpec", "TUNE_ICE_SPEC", "f", 0.55),
    ("render", "iceDepthMax", "TUNE_ICE_DEPTH_MAX", "f", 3.0),
    ("render", "iceReflectMin", "TUNE_ICE_REFLECT_MIN", "f", 0.12),

    # submerged view (raymarch.wgsl shadeSubmerged / godRays / bedCaustic)
    ("render", "subAbsorb", "TUNE_SUB_ABSORB", "v3", [0.42, 0.11, 0.075]),
    ("render", "subScatter", "TUNE_SUB_SCATTER", "v3", [0.055, 0.19, 0.24]),
    ("render", "subScatterGain", "TUNE_SUB_SCATTER_GAIN", "f", 1.0),
    ("render", "subVisibility", "TUNE_SUB_VISIBILITY", "f", 11.0),
    ("render", "subVignette", "TUNE_SUB_VIGNETTE", "f", 0.34),
    ("render", "subSnellGain", "TUNE_SUB_SNELL_GAIN", "f", 1.25),

    # caustics cast onto submerged surfaces
    ("render", "bedCausticGain", "TUNE_BED_CAUSTIC_GAIN", "f", 2.4),
    ("render", "bedCausticCap", "TUNE_BED_CAUSTIC_CAP", "f", 1.5),
    ("render", "bedCausticFade", "TUNE_BED_CAUSTIC_FADE", "f", 6.0),
    ("render", "bedCausticSharp", "TUNE_BED_CAUSTIC_SHARP", "f", 2.2),

    # volumetric light shafts (god rays)
    ("render", "godRaySteps", "TUNE_GODRAY_STEPS", "i", 14),
    ("render", "godRayStrength", "TUNE_GODRAY_STRENGTH", "f", 0.55),
    ("render", "godRayAniso", "TUNE_GODRAY_ANISO", "f", 0.62),
    ("render", "godRayRange", "TUNE_GODRAY_RANGE", "f", 14.0),
    ("render", "godRayShadowSteps", "TUNE_GODRAY_SHADOW_STEPS", "i", 20),

    # drifting particulate (silt)
    ("render", "siltDensity", "TUNE_SILT_DENSITY", "f", 0.55),
    ("render", "siltBrightness", "TUNE_SILT_BRIGHTNESS", "f", 0.5),
    ("render", "siltDrift", "TUNE_SILT_DRIFT", "f", 0.05),

    # surface-from-below
    ("render", "subSurfaceRipple", "TUNE_SUB_SURFACE_RIPPLE", "f", 1.6),

    # the generic per-liquid submerged profile (submergedProfile)
    ("render", "subMurkVis", "TUNE_SUB_MURK_VIS", "f", 0.55),
    ("render", "subVisCurve", "TUNE_SUB_VIS_CURVE", "f", 2.2),
    ("render", "subAbsorbGain", "TUNE_SUB_ABSORB_GAIN", "f", 7.0),
    ("render", "subAbsorbFloor", "TUNE_SUB_ABSORB_FLOOR", "f", 0.05),
    ("render", "subScatterDense", "TUNE_SUB_SCATTER_DENSE", "f", 0.42),
    ("render", "subScatterClear", "TUNE_SUB_SCATTER_CLEAR", "f", 0.16),
    ("render", "subClearLow", "TUNE_SUB_CLEAR_LOW", "f", 0.62),
    ("render", "subClearHigh", "TUNE_SUB_CLEAR_HIGH", "f", 0.82),

    # blood / viscous liquids
    ("render", "bloodF0", "TUNE_BLOOD_F0", "f", 0.03),
    ("render", "bloodGraze", "TUNE_BLOOD_GRAZE", "f", 0.55),
    ("render", "bloodAbsorb", "TUNE_BLOOD_ABSORB", "f", 55.0),
    ("render", "bloodTransmit", "TUNE_BLOOD_TRANSMIT", "f", 0.35),
    ("render", "bloodMaxTransmit", "TUNE_BLOOD_MAX_TRANSMIT", "f", 0.06),
    ("render", "bloodDepthRamp", "TUNE_BLOOD_DEPTH_RAMP", "f", 22.0),
    ("render", "bloodPoolLow", "TUNE_BLOOD_POOL_LOW", "f", 0.18),
    ("render", "bloodPoolHigh", "TUNE_BLOOD_POOL_HIGH", "f", 0.55),
    ("render", "bloodSmooth", "TUNE_BLOOD_SMOOTH", "f", 1.0),
    ("render", "bloodEdgeFeather", "TUNE_BLOOD_EDGE_FEATHER", "f", 0.1),
    ("render", "bloodWobble", "TUNE_BLOOD_WOBBLE", "f", 0.004),
    ("render", "bloodSheen", "TUNE_BLOOD_SHEEN", "f", 1.15),
    ("render", "bloodSheenDrop", "TUNE_BLOOD_SHEEN_DROP", "f", 32.0),
    ("render", "bloodSheenPool", "TUNE_BLOOD_SHEEN_POOL", "f", 220.0),
    ("render", "bloodAmbientSheen", "TUNE_BLOOD_AMBIENT_SHEEN", "f", 0.35),
    ("render", "bloodEdgeDepth", "TUNE_BLOOD_EDGE_DEPTH", "f", 0.035),
    ("render", "bloodEdgeStrength", "TUNE_BLOOD_EDGE_STRENGTH", "f", 0.65),
    ("render", "bloodEdgeTint", "TUNE_BLOOD_EDGE_TINT", "v3", [0.55, 0.4, 0.38]),

    # stains
    ("render", "stainCoverage", "TUNE_STAIN_COVERAGE", "f", 1.35),
    ("render", "stainMottle", "TUNE_STAIN_MOTTLE", "f", 0.85),
    ("render", "stainMottleScale", "TUNE_STAIN_MOTTLE_SCALE", "f", 0.55),
    ("render", "stainDarken", "TUNE_STAIN_DARKEN", "f", 0.55),
    ("render", "stainOpacity", "TUNE_STAIN_OPACITY", "f", 0.7),
    ("render", "stainSheen", "TUNE_STAIN_SHEEN", "f", 0.55),
    ("render", "stainSheenPower", "TUNE_STAIN_SHEEN_POWER", "f", 90.0),

    # lava / embers
    ("render", "lavaCrackFreq", "TUNE_LAVA_CRACK_FREQ", "f", 2.4),
    ("render", "lavaCrackKneeLow", "TUNE_LAVA_CRACK_KNEE_LOW", "f", 0.5),
    ("render", "lavaCrackKneeHigh", "TUNE_LAVA_CRACK_KNEE_HIGH", "f", 0.9),
    ("render", "lavaWarmBias", "TUNE_LAVA_WARM_BIAS", "f", 0.035),
    ("render", "lavaEmissionGain", "TUNE_LAVA_EMISSION_GAIN", "f", 1.9),
    ("render", "lavaPulseAmp", "TUNE_LAVA_PULSE_AMP", "f", 0.06),
    ("render", "lavaPulseRate", "TUNE_LAVA_PULSE_RATE", "f", 0.9),
    ("render", "heatSpillStrength", "TUNE_HEAT_SPILL_STRENGTH", "f", 0.16),
    ("render", "emberBrightness", "TUNE_EMBER_BRIGHTNESS", "f", 2.2),
    ("render", "emberRise", "TUNE_EMBER_RISE", "f", 26.0),
    ("render", "emberRate", "TUNE_EMBER_RATE", "f", 3.4),
    ("render", "emberDensity", "TUNE_EMBER_DENSITY", "u", 84),

    # tonemap / budgets
    ("render", "exposureWhite", "TUNE_EXPOSURE_WHITE", "f", 4.2),
    ("render", "bleachAmount", "TUNE_BLEACH_AMOUNT", "f", 0.9),
    ("render", "gamma", "TUNE_GAMMA", "f", 2.2),

    # static micro-detail
    ("render", "microLodDist", "TUNE_MICRO_LOD_DIST", "f", 40.0),
    ("render", "microMaxPerRay", "TUNE_MICRO_MAX_PER_RAY", "i", 8),
    ("render", "microSwayAmp", "TUNE_MICRO_SWAY_AMP", "f", 1.5),
    ("render", "microSwaySpeed", "TUNE_MICRO_SWAY_SPEED", "f", 1.0),
    ("render", "primarySteps", "TUNE_PRIMARY_STEPS", "i", 4096),
    ("render", "farSteps", "TUNE_FAR_STEPS", "i", 384),
    ("render", "farShadowReach", "TUNE_FAR_SHADOW_REACH", "f", 60.0),

    # in-window LOD handoff (PLAN_surface_flight_perf.md A1)
    ("render", "lodHandoffDist", "TUNE_LOD_HANDOFF_DIST", "f", 24.0),
    ("render", "shadowMaxDist", "TUNE_SHADOW_MAX_DIST", "f", 999.0),

    # wind: the SHAPE of the field (docs/RESEARCH_wind.md, DESIGN.md 12)
    ("wind", "gustWavelength", "TUNE_WIND_GUST_WAVELENGTH", "f", 4.8),
    ("wind", "gustSpeed", "TUNE_WIND_GUST_SPEED", "f", 1.1),
    ("wind", "altitudeGain", "TUNE_WIND_ALT_GAIN", "f", 0.6),
    ("wind", "altitudeRefY", "TUNE_WIND_ALT_REF_Y", "f", 64.0),
    ("wind", "dbgWindSpacing", "TUNE_WIND_DBG_SPACING", "f", 8.0),
    ("wind", "dbgWindRadius", "TUNE_WIND_DBG_RADIUS", "f", 48.0),

    # sim: DETERMINISM-CRITICAL, integer only (CLAUDE.md rule 1)
    ("sim", "partGravity", "TUNE_PART_GRAVITY", "i", 22),
    ("sim", "partMaxVel", "TUNE_PART_MAX_VEL", "i", 1536),
    ("sim", "airDensity", "TUNE_AIR_DENSITY", "i", 10),
    ("sim", "falloffPerCell", "TUNE_FALLOFF_PER_CELL", "i", 6),
    ("sim", "ejectSolid", "TUNE_EJECT_SOLID", "u", 250),
    ("sim", "ejectLiquid", "TUNE_EJECT_LIQUID", "u", 500),
    ("sim", "ejectPowder", "TUNE_EJECT_POWDER", "u", 350),
    ("sim", "ejectGas", "TUNE_EJECT_GAS", "u", 0),
    ("sim", "liquidEqualize", "TUNE_LIQUID_EQUALIZE", "u", 2),
    ("sim", "liquidMinFilm", "TUNE_LIQUID_MIN_FILM", "u", 1),
    ("sim", "wanderHopMask", "TUNE_WANDER_HOP_MASK", "u", 7),
    ("sim", "expMicroPerMille", "TUNE_EXP_MICRO_PERMILLE", "u", 900),
    ("sim", "expMicroLifeTicks", "TUNE_EXP_MICRO_LIFE", "u", 40),
    ("sim", "expMicroScaleIdx", "TUNE_EXP_MICRO_SCALE_IDX", "u", 2),

    # MLS-MPM fluid, HUMAN units
    ("sim", "fluidSubsteps", "TUNE_FLUID_SUBSTEPS", "i", 9),
    ("sim", "fluidStiffness", "TUNE_FLUID_STIFFNESS", "f", 14000.0),
    ("sim", "fluidGravity", "TUNE_FLUID_GRAVITY", "f", 900.0),
    ("sim", "fluidRestDensity", "TUNE_FLUID_REST_DENSITY", "f", 8.0),
    ("sim", "fluidEosPower", "TUNE_FLUID_EOS_POWER", "i", 4),
    ("sim", "fluidCohesion", "TUNE_FLUID_COHESION", "f", 0.0),
    ("sim", "fluidAttractSame", "TUNE_FLUID_ATTRACT_SAME", "f", 0.0),
    ("sim", "fluidAttractDiff", "TUNE_FLUID_ATTRACT_DIFF", "f", 0.0),
    ("sim", "fluidViscosity", "TUNE_FLUID_VISCOSITY", "f", 0.0),
    ("sim", "fluidDamping", "TUNE_FLUID_DAMPING", "f", 0.0),
    ("sim", "fluidFriction", "TUNE_FLUID_FRICTION", "f", 0.0),
    ("sim", "fluidSplashRate", "TUNE_FLUID_SPLASH_RATE", "f", 4.0),
    ("sim", "fluidSplashSpeed", "TUNE_FLUID_SPLASH_SPEED", "f", 18.0),
    ("sim", "fluidSplashMaxDensity", "TUNE_FLUID_SPLASH_MAXDENS", "f", 0.7),
    ("sim", "fluidSplashLife", "TUNE_FLUID_SPLASH_LIFE", "f", 1.1),
    ("sim", "fluidSplashScaleIdx", "TUNE_FLUID_SPLASH_SCALE_IDX", "u", 2),

    # diffuse material: spray / foam / bubbles (Ihmsen et al. 2012, CGI)
    ("sim", "fluidFoamRate", "TUNE_FLUID_FOAM_RATE", "f", 90.0),
    ("sim", "fluidFoamCrestRate", "TUNE_FLUID_FOAM_CREST_RATE", "f", 120.0),
    ("sim", "fluidTrappedMin", "TUNE_FLUID_TRAPPED_MIN", "f", 1.5),
    ("sim", "fluidTrappedMax", "TUNE_FLUID_TRAPPED_MAX", "f", 11.0),
    ("sim", "fluidCrestMin", "TUNE_FLUID_CREST_MIN", "f", 0.25),
    ("sim", "fluidCrestMax", "TUNE_FLUID_CREST_MAX", "f", 2.0),
    ("sim", "fluidFoamEnergyMin", "TUNE_FLUID_FOAM_EMIN", "f", 8.0),
    ("sim", "fluidFoamEnergyMax", "TUNE_FLUID_FOAM_EMAX", "f", 260.0),
    ("sim", "fluidFoamLife", "TUNE_FLUID_FOAM_LIFE", "f", 2.2),
    ("sim", "fluidFoamLifeMin", "TUNE_FLUID_FOAM_LIFE_MIN", "f", 0.5),
    ("sim", "fluidBubbleBuoyancy", "TUNE_FLUID_BUBBLE_BUOY", "f", 1.6),
    ("sim", "fluidFoamDrag", "TUNE_FLUID_FOAM_DRAG", "f", 0.72),
    ("sim", "fluidBubbleDensity", "TUNE_FLUID_BUBBLE_RHO", "f", 1.05),
    ("sim", "fluidSprayDensity", "TUNE_FLUID_SPRAY_RHO", "f", 0.42),
    ("sim", "fluidFoamScaleIdx", "TUNE_FLUID_FOAM_SCALE_IDX", "u", 3),

    # MLS-MPM settle / excite seam
    ("sim", "fluidExciteMode", "TUNE_FLUID_EXCITE_MODE", "i", 1),
    ("sim", "fluidExciteCeiling", "TUNE_FLUID_EXCITE_CEILING", "i", 8000),
    ("sim", "fluidExciteRate", "TUNE_FLUID_EXCITE_RATE", "i", 4096),
    ("sim", "fluidExcitePerch", "TUNE_FLUID_EXCITE_PERCH", "i", 0),
    ("sim", "fluidExciteStep", "TUNE_FLUID_EXCITE_STEP", "i", 2),
    ("sim", "fluidSettledMass", "TUNE_FLUID_SETTLED_MASS", "f", 1.0),
    ("sim", "fluidSettleEps", "TUNE_FLUID_SETTLE_EPS", "f", 6.0),
    ("sim", "fluidWakeSpeed", "TUNE_FLUID_WAKE_SPEED", "f", 24.0),
    ("sim", "fluidSettleTicks", "TUNE_FLUID_SETTLE_TICKS", "i", 24),
    ("sim", "fluidStainRate", "TUNE_FLUID_STAIN_RATE", "f", 8.0),
    ("sim", "windMode", "TUNE_WIND_MODE", "i", 1),
    ("sim", "windDrag", "TUNE_WIND_DRAG", "f", 3.0),
    ("sim", "windFluidGain", "TUNE_WIND_FLUID_GAIN", "f", 0.35),
    ("sim", "windFluidMass", "TUNE_WIND_FLUID_MASS", "f", 0.5),
    ("sim", "windDriftSpeed", "TUNE_WIND_DRIFT_SPEED", "f", 12.0),
    ("sim", "windDriftMax", "TUNE_WIND_DRIFT_MAX", "f", 0.5),
    ("sim", "windEntrainSpeed", "TUNE_WIND_ENTRAIN_SPEED", "f", 2.0),
    ("sim", "windEntrainRate", "TUNE_WIND_ENTRAIN_RATE", "f", 6.0),

    # worldgen (integer; needs a world regen to take effect)
    ("worldgen", "refVoxelsPerMetre", "TUNE_REF_VOXELS_PER_METRE", "i", 10),
    ("worldgen", "treeline", "TUNE_TREELINE", "i", 228),
    ("worldgen", "baseHeight", "TUNE_BASE_HEIGHT", "i", 200),
    ("worldgen", "contAmplitude", "TUNE_CONT_AMPLITUDE", "i", 1024),
    ("worldgen", "contLog2", "TUNE_CONT_LOG2", "u", 11),
    ("worldgen", "rangeAmplitude", "TUNE_RANGE_AMPLITUDE", "i", 256),
    ("worldgen", "rangeLog2", "TUNE_RANGE_LOG2", "u", 9),
    ("worldgen", "hillAmplitude", "TUNE_HILL_AMPLITUDE", "i", 64),
    ("worldgen", "hillLog2", "TUNE_HILL_LOG2", "u", 7),
    ("worldgen", "detailAmplitude", "TUNE_DETAIL_AMPLITUDE", "i", 16),
    ("worldgen", "detailLog2", "TUNE_DETAIL_LOG2", "u", 5),
    ("worldgen", "grainAmplitude", "TUNE_GRAIN_AMPLITUDE", "i", 4),
    ("worldgen", "grainLog2", "TUNE_GRAIN_LOG2", "u", 3),
    ("worldgen", "fbmAtten", "TUNE_FBM_ATTEN", "i", 256),
    ("worldgen", "spawnPlainY", "TUNE_SPAWN_PLAIN_Y", "i", 200),
    ("worldgen", "spawnPlainR", "TUNE_SPAWN_PLAIN_R", "i", 320),
    ("worldgen", "spawnPlainFade", "TUNE_SPAWN_PLAIN_FADE", "i", 2048),
    ("worldgen", "sedCeil", "TUNE_SED_CEIL", "i", 264),
    ("worldgen", "sedFraction", "TUNE_SED_FRACTION", "i", 64),
    ("worldgen", "sedStrip", "TUNE_SED_STRIP", "i", 6),
    ("worldgen", "sedSlope", "TUNE_SED_SLOPE", "i", 96),
    ("worldgen", "sedMax", "TUNE_SED_MAX", "i", 32),
    ("worldgen", "sedTopsoil", "TUNE_SED_TOPSOIL", "i", 4),
    ("worldgen", "biomeLog2", "TUNE_BIOME_LOG2", "u", 9),
    ("worldgen", "desertThreshold", "TUNE_DESERT_THRESHOLD", "u", 214),
    ("worldgen", "pineThreshold", "TUNE_PINE_THRESHOLD", "u", 176),
    ("worldgen", "meadowThreshold", "TUNE_MEADOW_THRESHOLD", "u", 92),
    ("worldgen", "treeTile", "TUNE_TREE_TILE", "i", 144),
    ("worldgen", "treeChanceForest", "TUNE_TREE_CHANCE_FOREST", "u", 78),
    ("worldgen", "treeChancePine", "TUNE_TREE_CHANCE_PINE", "u", 70),
    ("worldgen", "treeChanceMeadow", "TUNE_TREE_CHANCE_MEADOW", "u", 22),
    ("worldgen", "treeChanceDesert", "TUNE_TREE_CHANCE_DESERT", "u", 6),
    ("worldgen", "autumnFraction", "TUNE_AUTUMN_FRACTION", "u", 5),
    ("worldgen", "pondTile", "TUNE_POND_TILE", "i", 448),
    ("worldgen", "pondChance", "TUNE_POND_CHANCE", "u", 4),
    ("worldgen", "pondRadiusMin", "TUNE_POND_RADIUS_MIN", "i", 48),
    ("worldgen", "pondRadiusSpan", "TUNE_POND_RADIUS_SPAN", "u", 32),
    ("worldgen", "pondMaxSlope", "TUNE_POND_MAX_SLOPE", "i", 96),
    ("worldgen", "pondBerm", "TUNE_POND_BERM", "i", 5),
    ("worldgen", "pondBermWidth", "TUNE_POND_BERM_WIDTH", "i", 14),
    ("worldgen", "pondDepth", "TUNE_POND_DEPTH", "i", 26),
    ("worldgen", "pondDepthRim", "TUNE_POND_DEPTH_RIM", "i", 3),
    ("worldgen", "lilyChance", "TUNE_LILY_CHANCE", "u", 22),
    ("worldgen", "lilyFlowerChance", "TUNE_LILY_FLOWER_CHANCE", "u", 5),
    ("worldgen", "reedChance", "TUNE_REED_CHANCE", "u", 130),
    ("worldgen", "reedHeight", "TUNE_REED_HEIGHT", "i", 16),
    ("worldgen", "kelpChance", "TUNE_KELP_CHANCE", "u", 120),
    ("worldgen", "kelpHeight", "TUNE_KELP_HEIGHT", "i", 10),

    # vines / climbers / hanging moss (worldgen agent B)
    ("worldgen", "vineChance", "TUNE_VINE_CHANCE", "u", 26),
    ("worldgen", "vineLenMin", "TUNE_VINE_LEN_MIN", "i", 10),
    ("worldgen", "vineLenSpan", "TUNE_VINE_LEN_SPAN", "i", 26),
    ("worldgen", "creeperFlowerChance", "TUNE_CREEPER_FLOWER_CHANCE", "u", 9),
    ("worldgen", "mossChance", "TUNE_MOSS_CHANCE", "u", 14),
    ("worldgen", "mossLenMin", "TUNE_MOSS_LEN_MIN", "i", 4),
    ("worldgen", "mossLenSpan", "TUNE_MOSS_LEN_SPAN", "i", 9),
    ("worldgen", "ivyChance", "TUNE_IVY_CHANCE", "u", 2),
    ("worldgen", "ivyTwist", "TUNE_IVY_TWIST", "i", 5),
    ("worldgen", "wallIvyDensity", "TUNE_WALL_IVY_DENSITY", "u", 3),

    # shoreline: the wet fringe outside a pond
    ("worldgen", "shoreBand", "TUNE_SHORE_BAND", "i", 24),
    ("worldgen", "shoreMudWidth", "TUNE_SHORE_MUD_WIDTH", "i", 10),
    ("worldgen", "shoreLift", "TUNE_SHORE_LIFT", "i", 12),
    ("worldgen", "shoreCattailChance", "TUNE_SHORE_CATTAIL_CHANCE", "u", 12),
    ("worldgen", "shoreCattailReach", "TUNE_SHORE_CATTAIL_REACH", "i", 9),
    ("worldgen", "shoreCattailHeight", "TUNE_SHORE_CATTAIL_HEIGHT", "i", 20),
    ("worldgen", "shoreSedgeChance", "TUNE_SHORE_SEDGE_CHANCE", "u", 4),
    ("worldgen", "shoreHorsetailChance", "TUNE_SHORE_HORSETAIL_CHANCE", "u", 10),
    ("worldgen", "shoreHorsetailHeight", "TUNE_SHORE_HORSETAIL_HEIGHT", "i", 9),
    ("worldgen", "shoreIrisChance", "TUNE_SHORE_IRIS_CHANCE", "u", 34),
    ("worldgen", "shoreMossChance", "TUNE_SHORE_MOSS_CHANCE", "u", 3),

    # desert / pine highland / alpine ground cover (worldgen agent E)
    ("worldgen", "cactusChance", "TUNE_CACTUS_CHANCE", "u", 26),
    ("worldgen", "saguaroFraction", "TUNE_SAGUARO_FRACTION", "u", 22),
    ("worldgen", "tussockChance", "TUNE_TUSSOCK_CHANCE", "u", 9),
    ("worldgen", "scrubChance", "TUNE_SCRUB_CHANCE", "u", 26),
    ("worldgen", "desertPatch", "TUNE_DESERT_PATCH", "i", 130),
    ("worldgen", "heathChance", "TUNE_HEATH_CHANCE", "u", 7),
    ("worldgen", "heathPatch", "TUNE_HEATH_PATCH", "i", 128),
    ("worldgen", "alpineChance", "TUNE_ALPINE_CHANCE", "u", 40),
    ("worldgen", "ruinChance", "TUNE_RUIN_CHANCE", "u", 5),
    ("worldgen", "caveThreshold1", "TUNE_CAVE_THRESHOLD1", "u", 150),
    ("worldgen", "caveThreshold2", "TUNE_CAVE_THRESHOLD2", "u", 148),

    # oil / petroleum-like viscous liquids (shadeViscous)
    ("render", "oilSatLow", "TUNE_OIL_SAT_LOW", "f", 0.5),
    ("render", "oilSatHigh", "TUNE_OIL_SAT_HIGH", "f", 0.78),
    ("render", "oilF0", "TUNE_OIL_F0", "f", 0.043),
    ("render", "oilGraze", "TUNE_OIL_GRAZE", "f", 0.97),
    ("render", "oilGloss", "TUNE_OIL_GLOSS", "f", 620.0),
    ("render", "oilSheen", "TUNE_OIL_SHEEN", "f", 1.6),
    ("render", "oilReflectTint", "TUNE_OIL_REFLECT_TINT", "f", 0.12),
    ("render", "oilDarken", "TUNE_OIL_DARKEN", "f", 0.35),
    ("render", "oilIridescence", "TUNE_OIL_IRIDESCENCE", "f", 0.16),
    ("render", "oilFilmScale", "TUNE_OIL_FILM_SCALE", "f", 1.1),
    ("render", "oilFloatSens", "TUNE_OIL_FLOAT_SENS", "f", 9.0),
    ("render", "oilEdgeBand", "TUNE_OIL_EDGE_BAND", "f", 0.07),
    ("render", "oilDropReflect", "TUNE_OIL_DROP_REFLECT", "f", 0.3),
    ("render", "subMurkGlow", "TUNE_SUB_MURK_GLOW", "f", 2.2),
]


def fmt(v):
    s = repr(float(v))
    return s if ("." in s or "e" in s) else s + ".0"


def main():
    data = {}
    if os.path.exists(TUNING):
        try:
            with open(TUNING, "r", encoding="utf-8") as f:
                data = json.load(f)
        except Exception as e:  # a broken file must not silently use defaults
            sys.stderr.write("tuning_prelude: cannot parse %s: %s\n" % (TUNING, e))
            return 1

    out = []
    for group, key, name, kind, default in SPEC:
        v = data.get(group, {}).get(key, default)
        if kind == "f":
            out.append("const %s : f32 = %s;" % (name, fmt(v)))
        elif kind == "i":
            out.append("const %s : i32 = %d;" % (name, int(v)))
        elif kind == "u":
            out.append("const %s : u32 = %du;" % (name, max(0, int(v))))
        else:
            out.append("const %s : vec3f = vec3f(%s, %s, %s);"
                       % (name, fmt(v[0]), fmt(v[1]), fmt(v[2])))
    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
