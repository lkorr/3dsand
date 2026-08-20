#include "sim/tuning.h"

#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

Tuning g_current;

// ---- readers -------------------------------------------------------------
// Every reader is total: a missing key leaves the default in place, and a
// present-but-wrong value is reported and ignored rather than poisoning the
// renderer. The tuner writes well-formed JSON, but this file is also meant to
// be hand-edited, so bad input has to degrade gracefully.

const json* Find(const json& j, const std::string& key) {
  auto it = j.find(key);
  return it == j.end() ? nullptr : &*it;
}

void ReadF(const json& j, const char* key, float& dst, Tuning& t,
           const std::string& at) {
  const json* v = Find(j, key);
  if (!v) return;
  if (!v->is_number()) {
    t.warnings.push_back(at + "." + key + ": expected a number");
    return;
  }
  float f = v->get<float>();
  if (!std::isfinite(f)) {
    t.warnings.push_back(at + "." + key + ": not finite");
    return;
  }
  dst = f;
}

void ReadI(const json& j, const char* key, int& dst, Tuning& t,
           const std::string& at) {
  const json* v = Find(j, key);
  if (!v) return;
  if (!v->is_number_integer()) {
    // Determinism rule 1: the sim group must stay integer. Rejecting a float
    // here rather than truncating it means a fractional value in the JSON is
    // a loud error instead of a silent rounding that shifts the world hash.
    t.warnings.push_back(at + "." + key + ": expected an integer");
    return;
  }
  dst = v->get<int>();
}

void ReadV3(const json& j, const char* key, float dst[3], Tuning& t,
            const std::string& at) {
  const json* v = Find(j, key);
  if (!v) return;
  if (!v->is_array() || v->size() != 3) {
    t.warnings.push_back(at + "." + key + ": expected [r, g, b]");
    return;
  }
  for (int i = 0; i < 3; i++) {
    if (!(*v)[i].is_number()) {
      t.warnings.push_back(at + "." + key + ": component " +
                           std::to_string(i) + " is not a number");
      return;
    }
  }
  for (int i = 0; i < 3; i++) dst[i] = (*v)[i].get<float>();
}

// ---- WGSL emitters -------------------------------------------------------
// Full precision so a value round-trips the f32 it came from; a truncated
// literal here would silently shade differently from what the tuner shows.

std::string F(float v) {
  std::ostringstream o;
  o.precision(9);
  o << v;
  std::string s = o.str();
  // WGSL needs a decimal point to infer f32 from a bare literal.
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find("inf") == std::string::npos)
    s += ".0";
  return s;
}

void EmitF(std::ostringstream& o, const char* name, float v) {
  o << "const " << name << " : f32 = " << F(v) << ";\n";
}
void EmitI(std::ostringstream& o, const char* name, int v) {
  o << "const " << name << " : i32 = " << v << ";\n";
}
void EmitU(std::ostringstream& o, const char* name, int v) {
  o << "const " << name << " : u32 = " << (v < 0 ? 0 : v) << "u;\n";
}
void EmitV3(std::ostringstream& o, const char* name, const float v[3]) {
  o << "const " << name << " : vec3f = vec3f(" << F(v[0]) << ", " << F(v[1])
    << ", " << F(v[2]) << ");\n";
}

}  // namespace

const Tuning& CurrentTuning() { return g_current; }
void SetCurrentTuning(const Tuning& t) { g_current = t; }

bool LoadTuning(const std::string& path, Tuning& out) {
  std::ifstream f(path);
  if (!f) {
    out.warnings.push_back("cannot open " + path + " (using defaults)");
    return false;
  }
  json j;
  try {
    j = json::parse(f, nullptr, true, /*ignore_comments=*/true);
  } catch (const std::exception& e) {
    out.warnings.push_back(std::string("parse error: ") + e.what());
    return false;
  }
  if (!j.is_object()) {
    out.warnings.push_back("top level must be an object");
    return false;
  }

  if (const json* g = Find(j, "player")) {
    auto& p = out.player;
    const std::string at = "player";
    ReadF(*g, "flySpeed", p.flySpeed, out, at);
    ReadF(*g, "flySprint", p.flySprint, out, at);
    ReadF(*g, "walkSpeed", p.walkSpeed, out, at);
    ReadF(*g, "sprintSpeed", p.sprintSpeed, out, at);
    ReadF(*g, "gravity", p.gravity, out, at);
    ReadF(*g, "jumpSpeed", p.jumpSpeed, out, at);
    ReadF(*g, "swimUp", p.swimUp, out, at);
    ReadF(*g, "swimDown", p.swimDown, out, at);
    ReadF(*g, "maxFall", p.maxFall, out, at);
    ReadF(*g, "stepUp", p.stepUp, out, at);
    ReadF(*g, "smoothBump", p.smoothBump, out, at);
    ReadF(*g, "stepSpeedPenaltyPerM", p.stepSpeedPenaltyPerM, out, at);
    ReadF(*g, "minStepSpeedScale", p.minStepSpeedScale, out, at);
    ReadF(*g, "nonJumpSpeed", p.nonJumpSpeed, out, at);
    ReadF(*g, "coyoteTime", p.coyoteTime, out, at);
    ReadF(*g, "jumpBufferTime", p.jumpBufferTime, out, at);
    ReadF(*g, "groundAccel", p.groundAccel, out, at);
    ReadF(*g, "airAccel", p.airAccel, out, at);
    ReadF(*g, "liquidAccel", p.liquidAccel, out, at);
    ReadF(*g, "liquidDrag", p.liquidDrag, out, at);
    ReadF(*g, "liquidGravityScale", p.liquidGravityScale, out, at);
    ReadF(*g, "liquidSpeedScale", p.liquidSpeedScale, out, at);
    ReadF(*g, "halfWidth", p.halfWidth, out, at);
    ReadF(*g, "halfHeight", p.halfHeight, out, at);
    ReadF(*g, "eyeOffset", p.eyeOffset, out, at);
  }

  if (const json* g = Find(j, "camera")) {
    auto& c = out.camera;
    const std::string at = "camera";
    ReadF(*g, "mouseSensitivity", c.mouseSensitivity, out, at);
    ReadF(*g, "fovY", c.fovY, out, at);
    ReadF(*g, "pitchClamp", c.pitchClamp, out, at);
  }

  if (const json* g = Find(j, "physics")) {
    auto& p = out.physics;
    const std::string at = "physics";
    ReadF(*g, "gravity", p.gravity, out, at);
    ReadI(*g, "collisionSteps", p.collisionSteps, out, at);
    ReadF(*g, "debrisFriction", p.debrisFriction, out, at);
    ReadF(*g, "debrisRestitution", p.debrisRestitution, out, at);
    ReadF(*g, "debrisLinearDamping", p.debrisLinearDamping, out, at);
    ReadF(*g, "debrisAngularDamping", p.debrisAngularDamping, out, at);
    ReadF(*g, "terrainFriction", p.terrainFriction, out, at);
    ReadF(*g, "playerProxyFriction", p.playerProxyFriction, out, at);
    ReadF(*g, "explosionImpulseScale", p.explosionImpulseScale, out, at);
    ReadF(*g, "explosionImpulseRadiusScale", p.explosionImpulseRadiusScale, out, at);
    if (p.collisionSteps < 1) {
      out.warnings.push_back("physics.collisionSteps < 1; clamped to 1");
      p.collisionSteps = 1;
    }
  }

  if (const json* g = Find(j, "debris")) {
    auto& d = out.debris;
    const std::string at = "debris";
    ReadI(*g, "minBodyVoxels", d.minBodyVoxels, out, at);
    ReadI(*g, "minBurnFragmentVoxels", d.minBurnFragmentVoxels, out, at);
    ReadI(*g, "maxNewBodiesPerTick", d.maxNewBodiesPerTick, out, at);
    ReadI(*g, "settleAfterTicks", d.settleAfterTicks, out, at);
    ReadF(*g, "alignCos", d.alignCos, out, at);
    ReadI(*g, "maxBodies", d.maxBodies, out, at);
    ReadI(*g, "burnOpsPerTick", d.burnOpsPerTick, out, at);
    if (d.minBodyVoxels < 1) {
      out.warnings.push_back("debris.minBodyVoxels < 1; clamped to 1");
      d.minBodyVoxels = 1;
    }
  }

  if (const json* g = Find(j, "grenade")) {
    auto& n = out.grenade;
    const std::string at = "grenade";
    ReadF(*g, "throwSpeed", n.throwSpeed, out, at);
    ReadF(*g, "fuse", n.fuse, out, at);
    ReadF(*g, "restitution", n.restitution, out, at);
    ReadF(*g, "friction", n.friction, out, at);
    ReadF(*g, "waterDrag", n.waterDrag, out, at);
    ReadI(*g, "blastRadius", n.blastRadius, out, at);
    ReadI(*g, "blastPower", n.blastPower, out, at);
  }

  if (const json* g = Find(j, "tools")) {
    auto& t = out.tools;
    const std::string at = "tools";
    ReadI(*g, "detonateRadius", t.detonateRadius, out, at);
    ReadI(*g, "detonatePower", t.detonatePower, out, at);
    ReadF(*g, "laserRange", t.laserRange, out, at);
    ReadI(*g, "laserMeltRadius", t.laserMeltRadius, out, at);
    ReadF(*g, "laserDamage", t.laserDamage, out, at);
    ReadF(*g, "brushAirDistance", t.brushAirDistance, out, at);
  }

  if (const json* g = Find(j, "sim")) {
    auto& s = out.sim;
    const std::string at = "sim";
    ReadI(*g, "partGravity", s.partGravity, out, at);
    ReadI(*g, "partMaxVel", s.partMaxVel, out, at);
    ReadI(*g, "airDensity", s.airDensity, out, at);
    ReadI(*g, "falloffPerCell", s.falloffPerCell, out, at);
    ReadI(*g, "ejectSolid", s.ejectSolid, out, at);
    ReadI(*g, "ejectLiquid", s.ejectLiquid, out, at);
    ReadI(*g, "ejectPowder", s.ejectPowder, out, at);
    ReadI(*g, "ejectGas", s.ejectGas, out, at);
    ReadI(*g, "liquidEqualize", s.liquidEqualize, out, at);
    ReadI(*g, "wanderHopMask", s.wanderHopMask, out, at);
    // A zero falloff makes every explosion reach the full residency window:
    // not a crash, but a guaranteed frame-time cliff and an unbounded emergent
    // process (CLAUDE.md rule 2). Clamp rather than trust the file.
    if (s.falloffPerCell < 1) {
      out.warnings.push_back("sim.falloffPerCell < 1; clamped to 1");
      s.falloffPerCell = 1;
    }
    if (s.liquidEqualize < 1) {
      out.warnings.push_back(
          "sim.liquidEqualize < 1 would let liquids spread forever and never "
          "sleep; clamped to 1");
      s.liquidEqualize = 1;
    }
    auto clampMille = [&](const char* name, int& v) {
      if (v < 0 || v > 1000) {
        out.warnings.push_back(std::string("sim.") + name +
                               " outside 0..1000 per-mille; clamped");
        v = v < 0 ? 0 : 1000;
      }
    };
    clampMille("ejectSolid", s.ejectSolid);
    clampMille("ejectLiquid", s.ejectLiquid);
    clampMille("ejectPowder", s.ejectPowder);
    clampMille("ejectGas", s.ejectGas);
  }

  if (const json* g = Find(j, "render")) {
    auto& r = out.render;
    const std::string at = "render";
    ReadF(*g, "skyGradient", r.skyGradient, out, at);
    ReadF(*g, "skyHorizonOffset", r.skyHorizonOffset, out, at);
    ReadV3(*g, "skyHorizon", r.skyHorizon, out, at);
    ReadV3(*g, "skyZenith", r.skyZenith, out, at);
    ReadV3(*g, "sunTint", r.sunTint, out, at);
    ReadF(*g, "sunDiscPower", r.sunDiscPower, out, at);
    ReadF(*g, "sunDiscGain", r.sunDiscGain, out, at);
    ReadF(*g, "sunHaloPower", r.sunHaloPower, out, at);
    ReadF(*g, "sunHaloGain", r.sunHaloGain, out, at);
    ReadV3(*g, "sunDir", r.sunDir, out, at);
    ReadV3(*g, "sunColor", r.sunColor, out, at);
    ReadF(*g, "sunIntensity", r.sunIntensity, out, at);
    ReadF(*g, "fogOpticalDepths", r.fogOpticalDepths, out, at);
    ReadF(*g, "fogLerpPerFrame", r.fogLerpPerFrame, out, at);
    ReadV3(*g, "ambSky", r.ambSky, out, at);
    ReadV3(*g, "ambGround", r.ambGround, out, at);
    ReadF(*g, "diffuseWrap", r.diffuseWrap, out, at);
    ReadF(*g, "faceX", r.faceX, out, at);
    ReadF(*g, "faceZ", r.faceZ, out, at);
    ReadF(*g, "aoStrength", r.aoStrength, out, at);
    ReadF(*g, "aoFar", r.aoFar, out, at);
    ReadF(*g, "shadowBias", r.shadowBias, out, at);
    ReadI(*g, "shadowSteps", r.shadowSteps, out, at);
    ReadF(*g, "shadowSoftNear", r.shadowSoftNear, out, at);
    ReadF(*g, "shadowSoftFar", r.shadowSoftFar, out, at);
    ReadF(*g, "shadowLift", r.shadowLift, out, at);
    ReadF(*g, "shadowFarLift", r.shadowFarLift, out, at);
    ReadF(*g, "grainBroadScale", r.grainBroadScale, out, at);
    ReadF(*g, "grainFineScale", r.grainFineScale, out, at);
    ReadF(*g, "grainMix", r.grainMix, out, at);
    ReadF(*g, "grainAmp", r.grainAmp, out, at);
    ReadF(*g, "grainAmpFar", r.grainAmpFar, out, at);
    ReadF(*g, "mediaAbsorb", r.mediaAbsorb, out, at);
    ReadF(*g, "mediaTauMax", r.mediaTauMax, out, at);
    ReadF(*g, "fireFlickerBase", r.fireFlickerBase, out, at);
    ReadF(*g, "fireFlickerAmp", r.fireFlickerAmp, out, at);
    ReadF(*g, "fireFlickerRate", r.fireFlickerRate, out, at);
    ReadF(*g, "fireGlowRate", r.fireGlowRate, out, at);
    ReadF(*g, "fireIntensity", r.fireIntensity, out, at);
    ReadF(*g, "fireBreatheAmp", r.fireBreatheAmp, out, at);
    ReadF(*g, "fireBreatheRate", r.fireBreatheRate, out, at);
    ReadF(*g, "emissiveStrength", r.emissiveStrength, out, at);
    ReadF(*g, "emissiveFlickerBase", r.emissiveFlickerBase, out, at);
    ReadF(*g, "emissiveFlickerAmp", r.emissiveFlickerAmp, out, at);
    ReadF(*g, "emissiveFlickerRate", r.emissiveFlickerRate, out, at);
    ReadF(*g, "waterF0", r.waterF0, out, at);
    ReadV3(*g, "waterAbsorb", r.waterAbsorb, out, at);
    ReadV3(*g, "waterScatter", r.waterScatter, out, at);
    ReadF(*g, "waterFresnelPower", r.waterFresnelPower, out, at);
    ReadF(*g, "rippleAmpScale", r.rippleAmpScale, out, at);
    ReadF(*g, "rippleSpeedScale", r.rippleSpeedScale, out, at);
    ReadF(*g, "reflectionCutoff", r.reflectionCutoff, out, at);
    ReadI(*g, "reflectionSteps", r.reflectionSteps, out, at);
    ReadF(*g, "causticGain", r.causticGain, out, at);
    ReadF(*g, "causticCap", r.causticCap, out, at);
    ReadF(*g, "glintIntensity", r.glintIntensity, out, at);
    ReadF(*g, "glintPowerNear", r.glintPowerNear, out, at);
    ReadF(*g, "glintPowerFar", r.glintPowerFar, out, at);
    ReadF(*g, "foamDepth", r.foamDepth, out, at);
    ReadF(*g, "foamStrength", r.foamStrength, out, at);
    ReadF(*g, "lavaCrackFreq", r.lavaCrackFreq, out, at);
    ReadF(*g, "lavaCrackKneeLow", r.lavaCrackKneeLow, out, at);
    ReadF(*g, "lavaCrackKneeHigh", r.lavaCrackKneeHigh, out, at);
    ReadF(*g, "lavaWarmBias", r.lavaWarmBias, out, at);
    ReadF(*g, "lavaEmissionGain", r.lavaEmissionGain, out, at);
    ReadF(*g, "lavaPulseAmp", r.lavaPulseAmp, out, at);
    ReadF(*g, "lavaPulseRate", r.lavaPulseRate, out, at);
    ReadF(*g, "heatSpillStrength", r.heatSpillStrength, out, at);
    ReadF(*g, "emberBrightness", r.emberBrightness, out, at);
    ReadF(*g, "emberRise", r.emberRise, out, at);
    ReadF(*g, "emberRate", r.emberRate, out, at);
    ReadI(*g, "emberDensity", r.emberDensity, out, at);
    ReadF(*g, "exposureWhite", r.exposureWhite, out, at);
    ReadF(*g, "bleachAmount", r.bleachAmount, out, at);
    ReadF(*g, "gamma", r.gamma, out, at);
    ReadI(*g, "primarySteps", r.primarySteps, out, at);
    ReadI(*g, "farSteps", r.farSteps, out, at);
    // Zero step budgets compile fine and render nothing; a zero white point or
    // gamma divides by zero in the tonemap. Guard the ones that break the
    // image rather than merely change it.
    if (r.exposureWhite <= 0.0f) {
      out.warnings.push_back("render.exposureWhite must be > 0; reset to 4.2");
      r.exposureWhite = 4.2f;
    }
    if (r.gamma <= 0.0f) {
      out.warnings.push_back("render.gamma must be > 0; reset to 2.2");
      r.gamma = 2.2f;
    }
    if (r.primarySteps < 1) { r.primarySteps = 1; }
    if (r.shadowSteps < 0) { r.shadowSteps = 0; }
    if (r.reflectionSteps < 0) { r.reflectionSteps = 0; }
    if (r.farSteps < 1) { r.farSteps = 1; }
  }

  if (const json* g = Find(j, "worldgen")) {
    auto& w = out.worldgen;
    const std::string at = "worldgen";
    ReadI(*g, "treeline", w.treeline, out, at);
    ReadI(*g, "baseHeight", w.baseHeight, out, at);
    ReadI(*g, "hillAmplitude", w.hillAmplitude, out, at);
    ReadI(*g, "hillWavelength", w.hillWavelength, out, at);
    ReadI(*g, "detailAmplitude", w.detailAmplitude, out, at);
    ReadI(*g, "detailWavelength", w.detailWavelength, out, at);
    ReadI(*g, "biomeScale", w.biomeScale, out, at);
    ReadI(*g, "desertThreshold", w.desertThreshold, out, at);
    ReadI(*g, "pineThreshold", w.pineThreshold, out, at);
    ReadI(*g, "meadowThreshold", w.meadowThreshold, out, at);
    ReadI(*g, "treeTile", w.treeTile, out, at);
    ReadI(*g, "treeChanceForest", w.treeChanceForest, out, at);
    ReadI(*g, "treeChancePine", w.treeChancePine, out, at);
    ReadI(*g, "treeChanceMeadow", w.treeChanceMeadow, out, at);
    ReadI(*g, "treeChanceDesert", w.treeChanceDesert, out, at);
    ReadI(*g, "autumnFraction", w.autumnFraction, out, at);
    ReadI(*g, "pondTile", w.pondTile, out, at);
    ReadI(*g, "pondChance", w.pondChance, out, at);
    ReadI(*g, "pondRadiusMin", w.pondRadiusMin, out, at);
    ReadI(*g, "pondRadiusSpan", w.pondRadiusSpan, out, at);
    ReadI(*g, "ruinChance", w.ruinChance, out, at);
    ReadI(*g, "caveThreshold1", w.caveThreshold1, out, at);
    ReadI(*g, "caveThreshold2", w.caveThreshold2, out, at);
    // These divide or modulo in worldgen.wgsl; zero is a hang or a div-by-zero.
    auto atLeast = [&](const char* name, int& v, int lo) {
      if (v < lo) {
        out.warnings.push_back(std::string("worldgen.") + name + " < " +
                               std::to_string(lo) + "; clamped");
        v = lo;
      }
    };
    atLeast("hillWavelength", w.hillWavelength, 1);
    atLeast("detailWavelength", w.detailWavelength, 1);
    atLeast("biomeScale", w.biomeScale, 1);
    atLeast("treeTile", w.treeTile, 8);
    atLeast("pondTile", w.pondTile, 8);
    atLeast("pondChance", w.pondChance, 1);
    atLeast("pondRadiusSpan", w.pondRadiusSpan, 1);
    atLeast("ruinChance", w.ruinChance, 1);
    atLeast("autumnFraction", w.autumnFraction, 1);
  }

  return true;
}

std::string TuningWgslBlock(const Tuning& t) {
  const auto& r = t.render;
  const auto& s = t.sim;
  const auto& w = t.worldgen;
  std::ostringstream o;
  o << "// GENERATED from assets/materials/tuning.json by TuningWgslBlock() —\n"
       "// do not edit here and do not redeclare these in any shader.\n";

  // ---- sky / sun ----
  EmitF(o, "TUNE_SKY_GRADIENT", r.skyGradient);
  EmitF(o, "TUNE_SKY_HORIZON_OFFSET", r.skyHorizonOffset);
  EmitV3(o, "TUNE_SKY_HORIZON", r.skyHorizon);
  EmitV3(o, "TUNE_SKY_ZENITH", r.skyZenith);
  EmitV3(o, "TUNE_SUN_TINT", r.sunTint);
  EmitF(o, "TUNE_SUN_DISC_POWER", r.sunDiscPower);
  EmitF(o, "TUNE_SUN_DISC_GAIN", r.sunDiscGain);
  EmitF(o, "TUNE_SUN_HALO_POWER", r.sunHaloPower);
  EmitF(o, "TUNE_SUN_HALO_GAIN", r.sunHaloGain);
  EmitV3(o, "TUNE_SUN_COLOR", r.sunColor);
  EmitF(o, "TUNE_SUN_INTENSITY", r.sunIntensity);

  // ---- ambient / diffuse ----
  EmitV3(o, "TUNE_AMB_SKY", r.ambSky);
  EmitV3(o, "TUNE_AMB_GROUND", r.ambGround);
  EmitF(o, "TUNE_DIFFUSE_WRAP", r.diffuseWrap);
  EmitF(o, "TUNE_FACE_X", r.faceX);
  EmitF(o, "TUNE_FACE_Z", r.faceZ);

  // ---- AO / shadows ----
  EmitF(o, "TUNE_AO_STRENGTH", r.aoStrength);
  EmitF(o, "TUNE_AO_FAR", r.aoFar);
  EmitF(o, "TUNE_SHADOW_BIAS", r.shadowBias);
  EmitI(o, "TUNE_SHADOW_STEPS", r.shadowSteps);
  EmitF(o, "TUNE_SHADOW_SOFT_NEAR", r.shadowSoftNear);
  EmitF(o, "TUNE_SHADOW_SOFT_FAR", r.shadowSoftFar);
  EmitF(o, "TUNE_SHADOW_LIFT", r.shadowLift);
  EmitF(o, "TUNE_SHADOW_FAR_LIFT", r.shadowFarLift);

  // ---- grain ----
  EmitF(o, "TUNE_GRAIN_BROAD_SCALE", r.grainBroadScale);
  EmitF(o, "TUNE_GRAIN_FINE_SCALE", r.grainFineScale);
  EmitF(o, "TUNE_GRAIN_MIX", r.grainMix);
  EmitF(o, "TUNE_GRAIN_AMP", r.grainAmp);
  EmitF(o, "TUNE_GRAIN_AMP_FAR", r.grainAmpFar);

  // ---- media / fire ----
  EmitF(o, "TUNE_MEDIA_ABSORB", r.mediaAbsorb);
  EmitF(o, "TUNE_MEDIA_TAU_MAX", r.mediaTauMax);
  EmitF(o, "TUNE_FIRE_FLICKER_BASE", r.fireFlickerBase);
  EmitF(o, "TUNE_FIRE_FLICKER_AMP", r.fireFlickerAmp);
  EmitF(o, "TUNE_FIRE_FLICKER_RATE", r.fireFlickerRate);
  EmitF(o, "TUNE_FIRE_GLOW_RATE", r.fireGlowRate);
  EmitF(o, "TUNE_FIRE_INTENSITY", r.fireIntensity);
  EmitF(o, "TUNE_FIRE_BREATHE_AMP", r.fireBreatheAmp);
  EmitF(o, "TUNE_FIRE_BREATHE_RATE", r.fireBreatheRate);
  EmitF(o, "TUNE_EMISSIVE_STRENGTH", r.emissiveStrength);
  EmitF(o, "TUNE_EMISSIVE_FLICKER_BASE", r.emissiveFlickerBase);
  EmitF(o, "TUNE_EMISSIVE_FLICKER_AMP", r.emissiveFlickerAmp);
  EmitF(o, "TUNE_EMISSIVE_FLICKER_RATE", r.emissiveFlickerRate);

  // ---- water ----
  EmitF(o, "TUNE_WATER_F0", r.waterF0);
  EmitV3(o, "TUNE_WATER_ABSORB", r.waterAbsorb);
  EmitV3(o, "TUNE_WATER_SCATTER", r.waterScatter);
  EmitF(o, "TUNE_WATER_FRESNEL_POWER", r.waterFresnelPower);
  EmitF(o, "TUNE_RIPPLE_AMP_SCALE", r.rippleAmpScale);
  EmitF(o, "TUNE_RIPPLE_SPEED_SCALE", r.rippleSpeedScale);
  EmitF(o, "TUNE_REFLECTION_CUTOFF", r.reflectionCutoff);
  EmitI(o, "TUNE_REFLECTION_STEPS", r.reflectionSteps);
  EmitF(o, "TUNE_CAUSTIC_GAIN", r.causticGain);
  EmitF(o, "TUNE_CAUSTIC_CAP", r.causticCap);
  EmitF(o, "TUNE_GLINT_INTENSITY", r.glintIntensity);
  EmitF(o, "TUNE_GLINT_POWER_NEAR", r.glintPowerNear);
  EmitF(o, "TUNE_GLINT_POWER_FAR", r.glintPowerFar);
  EmitF(o, "TUNE_FOAM_DEPTH", r.foamDepth);
  EmitF(o, "TUNE_FOAM_STRENGTH", r.foamStrength);

  // ---- lava / embers ----
  EmitF(o, "TUNE_LAVA_CRACK_FREQ", r.lavaCrackFreq);
  EmitF(o, "TUNE_LAVA_CRACK_KNEE_LOW", r.lavaCrackKneeLow);
  EmitF(o, "TUNE_LAVA_CRACK_KNEE_HIGH", r.lavaCrackKneeHigh);
  EmitF(o, "TUNE_LAVA_WARM_BIAS", r.lavaWarmBias);
  EmitF(o, "TUNE_LAVA_EMISSION_GAIN", r.lavaEmissionGain);
  EmitF(o, "TUNE_LAVA_PULSE_AMP", r.lavaPulseAmp);
  EmitF(o, "TUNE_LAVA_PULSE_RATE", r.lavaPulseRate);
  EmitF(o, "TUNE_HEAT_SPILL_STRENGTH", r.heatSpillStrength);
  EmitF(o, "TUNE_EMBER_BRIGHTNESS", r.emberBrightness);
  EmitF(o, "TUNE_EMBER_RISE", r.emberRise);
  EmitF(o, "TUNE_EMBER_RATE", r.emberRate);
  EmitU(o, "TUNE_EMBER_DENSITY", r.emberDensity);

  // ---- tonemap / budgets ----
  EmitF(o, "TUNE_EXPOSURE_WHITE", r.exposureWhite);
  EmitF(o, "TUNE_BLEACH_AMOUNT", r.bleachAmount);
  EmitF(o, "TUNE_GAMMA", r.gamma);
  EmitI(o, "TUNE_PRIMARY_STEPS", r.primarySteps);
  EmitI(o, "TUNE_FAR_STEPS", r.farSteps);

  // ---- sim: DETERMINISM-CRITICAL, integer only (CLAUDE.md rule 1) ----
  o << "// sim constants below feed voxel state: integer-only, and any change\n"
       "// here changes the world hash (re-run --selftest).\n";
  EmitI(o, "TUNE_PART_GRAVITY", s.partGravity);
  EmitI(o, "TUNE_PART_MAX_VEL", s.partMaxVel);
  EmitI(o, "TUNE_AIR_DENSITY", s.airDensity);
  EmitI(o, "TUNE_FALLOFF_PER_CELL", s.falloffPerCell);
  EmitU(o, "TUNE_EJECT_SOLID", s.ejectSolid);
  EmitU(o, "TUNE_EJECT_LIQUID", s.ejectLiquid);
  EmitU(o, "TUNE_EJECT_POWDER", s.ejectPowder);
  EmitU(o, "TUNE_EJECT_GAS", s.ejectGas);
  EmitU(o, "TUNE_LIQUID_EQUALIZE", s.liquidEqualize);
  EmitU(o, "TUNE_WANDER_HOP_MASK", s.wanderHopMask);

  // ---- worldgen (integer; needs a world regen to take effect) ----
  EmitI(o, "TUNE_TREELINE", w.treeline);
  EmitI(o, "TUNE_BASE_HEIGHT", w.baseHeight);
  EmitI(o, "TUNE_HILL_AMPLITUDE", w.hillAmplitude);
  EmitI(o, "TUNE_HILL_WAVELENGTH", w.hillWavelength);
  EmitI(o, "TUNE_DETAIL_AMPLITUDE", w.detailAmplitude);
  EmitI(o, "TUNE_DETAIL_WAVELENGTH", w.detailWavelength);
  EmitI(o, "TUNE_BIOME_SCALE", w.biomeScale);
  EmitU(o, "TUNE_DESERT_THRESHOLD", w.desertThreshold);
  EmitU(o, "TUNE_PINE_THRESHOLD", w.pineThreshold);
  EmitU(o, "TUNE_MEADOW_THRESHOLD", w.meadowThreshold);
  EmitI(o, "TUNE_TREE_TILE", w.treeTile);
  EmitU(o, "TUNE_TREE_CHANCE_FOREST", w.treeChanceForest);
  EmitU(o, "TUNE_TREE_CHANCE_PINE", w.treeChancePine);
  EmitU(o, "TUNE_TREE_CHANCE_MEADOW", w.treeChanceMeadow);
  EmitU(o, "TUNE_TREE_CHANCE_DESERT", w.treeChanceDesert);
  EmitU(o, "TUNE_AUTUMN_FRACTION", w.autumnFraction);
  EmitI(o, "TUNE_POND_TILE", w.pondTile);
  EmitU(o, "TUNE_POND_CHANCE", w.pondChance);
  EmitI(o, "TUNE_POND_RADIUS_MIN", w.pondRadiusMin);
  EmitU(o, "TUNE_POND_RADIUS_SPAN", w.pondRadiusSpan);
  EmitU(o, "TUNE_RUIN_CHANCE", w.ruinChance);
  EmitU(o, "TUNE_CAVE_THRESHOLD1", w.caveThreshold1);
  EmitU(o, "TUNE_CAVE_THRESHOLD2", w.caveThreshold2);

  return o.str();
}
