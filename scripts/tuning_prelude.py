#!/usr/bin/env python3
"""Emit the WGSL tuning constants, mirroring TuningWgslBlock() in tuning.cpp.

check_shaders.sh has to compile each shader exactly the way LoadShader() does,
and LoadShader now prepends this block after the world prelude. Rather than
re-parse JSON in bash, the script shells out here. The NAMES and TYPES below
must stay in step with src/sim/tuning.cpp — a name that exists in one and not
the other shows up as an "unresolved identifier" from tint, which is the
failure mode we want (loud, at validation time) rather than a silent drift.

Values are read from assets/materials/tuning.json; anything missing falls back
to the same default the C++ struct carries, so the two agree on a partial file.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TUNING = os.path.join(ROOT, "assets", "materials", "tuning.json")

# (group, key, wgsl_name, kind) — kind is 'f', 'i', 'u' or 'v3'.
SPEC = [
    ("render", "skyGradient", "TUNE_SKY_GRADIENT", "f", 1.4),
    ("render", "skyHorizonOffset", "TUNE_SKY_HORIZON_OFFSET", "f", 0.25),
    ("render", "skyHorizon", "TUNE_SKY_HORIZON", "v3", [0.72, 0.80, 0.90]),
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
    ("render", "skyGround", "TUNE_SKY_GROUND", "v3", [0.22, 0.20, 0.17]),
    ("render", "sunSize", "TUNE_SUN_SIZE", "f", 3.0),
    ("render", "sunReddening", "TUNE_SUN_REDDENING", "f", 1.0),

    # night sky
    ("render", "nightZenith", "TUNE_NIGHT_ZENITH", "v3", [0.006, 0.010, 0.028]),
    ("render", "nightHorizon", "TUNE_NIGHT_HORIZON", "v3", [0.030, 0.036, 0.062]),
    ("render", "starBrightness", "TUNE_STAR_BRIGHTNESS", "f", 1.0),
    ("render", "starDensity", "TUNE_STAR_DENSITY", "f", 150.0),
    ("render", "starSize", "TUNE_STAR_SIZE", "f", 0.85),
    ("render", "starSparsity", "TUNE_STAR_SPARSITY", "f", 0.012),
    ("render", "starTwinkle", "TUNE_STAR_TWINKLE", "f", 0.35),
    ("render", "milkyWayStrength", "TUNE_MILKYWAY_STRENGTH", "f", 0.55),
    ("render", "milkyWayColor", "TUNE_MILKYWAY_COLOR", "v3", [0.52, 0.56, 0.78]),
    ("render", "nebulaStrength", "TUNE_NEBULA_STRENGTH", "f", 0.40),
    ("render", "nebulaCool", "TUNE_NEBULA_COOL", "v3", [0.16, 0.30, 0.62]),
    ("render", "nebulaWarm", "TUNE_NEBULA_WARM", "v3", [0.55, 0.20, 0.38]),
    ("render", "auroraStrength", "TUNE_AURORA_STRENGTH", "f", 0.55),
    ("render", "auroraHeight", "TUNE_AURORA_HEIGHT", "f", 900.0),
    ("render", "auroraLow", "TUNE_AURORA_LOW", "v3", [0.10, 0.85, 0.45]),
    ("render", "auroraHigh", "TUNE_AURORA_HIGH", "v3", [0.65, 0.20, 0.85]),

    # moon
    ("render", "moonRadius", "TUNE_MOON_RADIUS", "f", 0.030),
    ("render", "moonBrightness", "TUNE_MOON_BRIGHTNESS", "f", 1.6),
    ("render", "moonColor", "TUNE_MOON_COLOR", "v3", [0.92, 0.93, 0.88]),
    ("render", "moonGlow", "TUNE_MOON_GLOW", "f", 0.35),
    ("render", "moonEarthshine", "TUNE_MOON_EARTHSHINE", "f", 0.055),
    ("render", "moonLightColor", "TUNE_MOON_LIGHT_COLOR", "v3", [0.55, 0.68, 1.0]),
    ("render", "moonLightIntensity", "TUNE_MOON_LIGHT_INTENSITY", "f", 0.16),

    # night ambient
    ("render", "nightAmbSky", "TUNE_NIGHT_AMB_SKY", "v3", [0.055, 0.075, 0.135]),
    ("render", "nightAmbGround", "TUNE_NIGHT_AMB_GROUND", "v3", [0.022, 0.026, 0.042]),

    ("render", "ambSky", "TUNE_AMB_SKY", "v3", [0.40, 0.48, 0.62]),
    ("render", "ambGround", "TUNE_AMB_GROUND", "v3", [0.25, 0.22, 0.17]),
    ("render", "diffuseWrap", "TUNE_DIFFUSE_WRAP", "f", 0.55),
    ("render", "faceX", "TUNE_FACE_X", "f", 0.96),
    ("render", "faceZ", "TUNE_FACE_Z", "f", 0.92),

    ("render", "aoStrength", "TUNE_AO_STRENGTH", "f", 0.45),
    ("render", "aoFar", "TUNE_AO_FAR", "f", 0.72),
    ("render", "shadowBias", "TUNE_SHADOW_BIAS", "f", 0.02),
    ("render", "shadowSteps", "TUNE_SHADOW_STEPS", "i", 384),
    ("render", "shadowSoftNear", "TUNE_SHADOW_SOFT_NEAR", "f", 0.6),
    ("render", "shadowSoftFar", "TUNE_SHADOW_SOFT_FAR", "f", 9.0),
    ("render", "shadowLift", "TUNE_SHADOW_LIFT", "f", 0.45),
    ("render", "shadowFarLift", "TUNE_SHADOW_FAR_LIFT", "f", 0.3),

    ("render", "grainBroadScale", "TUNE_GRAIN_BROAD_SCALE", "f", 11.0),
    ("render", "grainFineScale", "TUNE_GRAIN_FINE_SCALE", "f", 2.5),
    ("render", "grainMix", "TUNE_GRAIN_MIX", "f", 0.68),
    ("render", "grainAmp", "TUNE_GRAIN_AMP", "f", 0.065),
    ("render", "grainAmpFar", "TUNE_GRAIN_AMP_FAR", "f", 0.05),

    ("render", "mediaAbsorb", "TUNE_MEDIA_ABSORB", "f", 6.4),
    ("render", "mediaTauMax", "TUNE_MEDIA_TAU_MAX", "f", 6.0),
    ("render", "fireFlickerBase", "TUNE_FIRE_FLICKER_BASE", "f", 0.70),
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

    ("render", "waterF0", "TUNE_WATER_F0", "f", 0.0204),
    ("render", "waterAbsorb", "TUNE_WATER_ABSORB", "v3", [1.85, 0.42, 0.20]),
    ("render", "waterScatter", "TUNE_WATER_SCATTER", "v3", [0.045, 0.16, 0.20]),
    ("render", "waterFresnelPower", "TUNE_WATER_FRESNEL_POWER", "f", 5.0),
    ("render", "rippleAmpScale", "TUNE_RIPPLE_AMP_SCALE", "f", 1.0),
    ("render", "rippleSpeedScale", "TUNE_RIPPLE_SPEED_SCALE", "f", 1.0),
    ("render", "reflectionCutoff", "TUNE_REFLECTION_CUTOFF", "f", 0.06),
    ("render", "reflectionSteps", "TUNE_REFLECTION_STEPS", "i", 96),
    ("render", "causticGain", "TUNE_CAUSTIC_GAIN", "f", 1.5),
    ("render", "causticCap", "TUNE_CAUSTIC_CAP", "f", 0.85),
    ("render", "glintIntensity", "TUNE_GLINT_INTENSITY", "f", 0.85),
    ("render", "glintPowerNear", "TUNE_GLINT_POWER_NEAR", "f", 180.0),
    ("render", "glintPowerFar", "TUNE_GLINT_POWER_FAR", "f", 900.0),
    ("render", "foamDepth", "TUNE_FOAM_DEPTH", "f", 0.42),
    ("render", "foamStrength", "TUNE_FOAM_STRENGTH", "f", 0.55),

    ("render", "lavaCrackFreq", "TUNE_LAVA_CRACK_FREQ", "f", 2.4),
    ("render", "lavaCrackKneeLow", "TUNE_LAVA_CRACK_KNEE_LOW", "f", 0.50),
    ("render", "lavaCrackKneeHigh", "TUNE_LAVA_CRACK_KNEE_HIGH", "f", 0.90),
    ("render", "lavaWarmBias", "TUNE_LAVA_WARM_BIAS", "f", 0.035),
    ("render", "lavaEmissionGain", "TUNE_LAVA_EMISSION_GAIN", "f", 1.9),
    ("render", "lavaPulseAmp", "TUNE_LAVA_PULSE_AMP", "f", 0.06),
    ("render", "lavaPulseRate", "TUNE_LAVA_PULSE_RATE", "f", 0.9),
    ("render", "heatSpillStrength", "TUNE_HEAT_SPILL_STRENGTH", "f", 0.16),
    ("render", "emberBrightness", "TUNE_EMBER_BRIGHTNESS", "f", 2.2),
    ("render", "emberRise", "TUNE_EMBER_RISE", "f", 26.0),
    ("render", "emberRate", "TUNE_EMBER_RATE", "f", 3.4),
    ("render", "emberDensity", "TUNE_EMBER_DENSITY", "u", 84),

    ("render", "exposureWhite", "TUNE_EXPOSURE_WHITE", "f", 4.2),
    ("render", "bleachAmount", "TUNE_BLEACH_AMOUNT", "f", 0.9),
    ("render", "gamma", "TUNE_GAMMA", "f", 2.2),
    ("render", "primarySteps", "TUNE_PRIMARY_STEPS", "i", 4096),
    ("render", "farSteps", "TUNE_FAR_STEPS", "i", 384),

    # determinism-critical (CLAUDE.md rule 1)
    ("sim", "partGravity", "TUNE_PART_GRAVITY", "i", 22),
    ("sim", "partMaxVel", "TUNE_PART_MAX_VEL", "i", 1536),
    ("sim", "airDensity", "TUNE_AIR_DENSITY", "i", 10),
    ("sim", "falloffPerCell", "TUNE_FALLOFF_PER_CELL", "i", 6),
    ("sim", "ejectSolid", "TUNE_EJECT_SOLID", "u", 250),
    ("sim", "ejectLiquid", "TUNE_EJECT_LIQUID", "u", 500),
    ("sim", "ejectPowder", "TUNE_EJECT_POWDER", "u", 350),
    ("sim", "ejectGas", "TUNE_EJECT_GAS", "u", 0),
    ("sim", "liquidEqualize", "TUNE_LIQUID_EQUALIZE", "u", 2),
    ("sim", "wanderHopMask", "TUNE_WANDER_HOP_MASK", "u", 7),

    ("worldgen", "treeline", "TUNE_TREELINE", "i", 72),
    ("worldgen", "baseHeight", "TUNE_BASE_HEIGHT", "i", 32),
    ("worldgen", "hillAmplitude", "TUNE_HILL_AMPLITUDE", "i", 42),
    ("worldgen", "hillWavelength", "TUNE_HILL_WAVELENGTH", "i", 64),
    ("worldgen", "detailAmplitude", "TUNE_DETAIL_AMPLITUDE", "i", 12),
    ("worldgen", "detailWavelength", "TUNE_DETAIL_WAVELENGTH", "i", 16),
    ("worldgen", "biomeScale", "TUNE_BIOME_SCALE", "i", 384),
    ("worldgen", "desertThreshold", "TUNE_DESERT_THRESHOLD", "u", 214),
    ("worldgen", "pineThreshold", "TUNE_PINE_THRESHOLD", "u", 176),
    ("worldgen", "meadowThreshold", "TUNE_MEADOW_THRESHOLD", "u", 92),
    ("worldgen", "treeTile", "TUNE_TREE_TILE", "i", 144),
    ("worldgen", "treeChanceForest", "TUNE_TREE_CHANCE_FOREST", "u", 78),
    ("worldgen", "treeChancePine", "TUNE_TREE_CHANCE_PINE", "u", 70),
    ("worldgen", "treeChanceMeadow", "TUNE_TREE_CHANCE_MEADOW", "u", 22),
    ("worldgen", "treeChanceDesert", "TUNE_TREE_CHANCE_DESERT", "u", 6),
    ("worldgen", "autumnFraction", "TUNE_AUTUMN_FRACTION", "u", 5),
    ("worldgen", "pondTile", "TUNE_POND_TILE", "i", 224),
    ("worldgen", "pondChance", "TUNE_POND_CHANCE", "u", 4),
    ("worldgen", "pondRadiusMin", "TUNE_POND_RADIUS_MIN", "i", 20),
    ("worldgen", "pondRadiusSpan", "TUNE_POND_RADIUS_SPAN", "u", 17),
    ("worldgen", "ruinChance", "TUNE_RUIN_CHANCE", "u", 5),
    ("worldgen", "caveThreshold1", "TUNE_CAVE_THRESHOLD1", "u", 150),
    ("worldgen", "caveThreshold2", "TUNE_CAVE_THRESHOLD2", "u", 148),
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
