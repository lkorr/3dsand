#include "sim/tuning.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

// kDayPhaseMax / kDayPhaseMask — the day phase is defined alongside the other
// world constants so the sim and the renderer share one definition.
#include "sim/rng.h"
#include "sim/world.h"

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

// Booleans are read strictly rather than accepting 0/1: a checkbox that
// silently accepts a number is a checkbox that silently accepts a typo, and
// these gate whole subsystems (avatar on/off, camera collision on/off).
void ReadStr(const json& j, const char* key, std::string& dst, Tuning& t,
             const std::string& at) {
  const json* v = Find(j, key);
  if (!v) return;
  if (!v->is_string()) {
    t.warnings.push_back(at + "." + key + ": expected a string");
    return;
  }
  dst = v->get<std::string>();
}

void ReadB(const json& j, const char* key, bool& dst, Tuning& t,
           const std::string& at) {
  const json* v = Find(j, key);
  if (!v) return;
  if (!v->is_boolean()) {
    t.warnings.push_back(at + "." + key + ": expected true or false");
    return;
  }
  dst = v->get<bool>();
}

// ---- variance readers ----------------------------------------------------
// A variance rides alongside its parameter as a sibling object, so the tuner's
// generic tune[group][key] writer round-trips it with no save-path changes:
//
//   "bleedSprayPerDrip": 3.0,
//   "bleedSprayPerDripVar": { "dist": "gaussian", "scope": "entity",
//                             "amount": 1.2, "sigmaClamp": 3.0 }
//
// Absent or dist:"none" leaves the parameter a plain constant.
void ReadVar(const json& j, const char* key, Variance& dst, Tuning& t,
             const std::string& at) {
  const json* v = Find(j, key);
  if (!v) return;
  if (!v->is_object()) {
    t.warnings.push_back(std::string(at) + "." + key + ": expected an object");
    return;
  }
  const std::string vat = at + "." + key;
  if (const json* d = Find(*v, "dist")) {
    if (!d->is_string()) {
      t.warnings.push_back(vat + ".dist: expected a string");
    } else {
      const std::string s = d->get<std::string>();
      if (s == "none") dst.dist = Variance::kNone;
      else if (s == "uniform") dst.dist = Variance::kUniform;
      else if (s == "gaussian") dst.dist = Variance::kGaussian;
      else t.warnings.push_back(vat + ".dist: expected none|uniform|gaussian");
    }
  }
  if (const json* s = Find(*v, "scope")) {
    if (!s->is_string()) {
      t.warnings.push_back(vat + ".scope: expected a string");
    } else {
      const std::string ss = s->get<std::string>();
      if (ss == "event") dst.scope = Variance::kEvent;
      else if (ss == "entity") dst.scope = Variance::kEntity;
      else t.warnings.push_back(vat + ".scope: expected event|entity");
    }
  }
  ReadF(*v, "amount", dst.amount, t, vat);
  ReadF(*v, "sigmaClamp", dst.sigmaClamp, t, vat);
  ReadF(*v, "min", dst.minValue, t, vat);
  ReadF(*v, "max", dst.maxValue, t, vat);
  if (dst.amount < 0.0f) {
    t.warnings.push_back(vat + ".amount < 0; using its magnitude");
    dst.amount = -dst.amount;
  }
  // A clamp at 0 sigma would silence a gaussian entirely, which looks like the
  // feature is broken rather than like a deliberate setting.
  if (dst.sigmaClamp < 0.25f) {
    t.warnings.push_back(vat + ".sigmaClamp < 0.25; clamped to 0.25");
    dst.sigmaClamp = 0.25f;
  }
  if (dst.minValue > dst.maxValue) {
    t.warnings.push_back(vat + ": min > max; ignoring both");
    dst.minValue = -1e30f;
    dst.maxValue = 1e30f;
  }
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

// --sweep's setter, for ANY group, generated from the same X-macro table that
// emits the WGSL constants. Written this way rather than as another hand-kept
// list because the hand-kept list is what limited --sweep to `sim.*` -- and
// CLAUDE.md tells you to prove a new knob reaches the kernel with --sweep, which
// no worldgen knob could ever do. A row in tuning_params.def is now sweepable
// the moment it exists, with nothing else to remember.
//
// TP_V3 is skipped: a vec3 has no single float to sweep, and --sweep's grammar
// has no place to say which component.
bool SetTuningField(Tuning& t, const std::string& group,
                    const std::string& name, float value) {
#define TP_F(g, m, n, d) \
  if (group == #g && name == #m) { t.g.m = value; return true; }
#define TP_I(g, m, n, d) \
  if (group == #g && name == #m) { t.g.m = (int)value; return true; }
#define TP_U(g, m, n, d) \
  if (group == #g && name == #m) { t.g.m = (int)value; return true; }
#define TP_V3(g, m, n, ...)
#include "sim/tuning_params.def"
#undef TP_V3
#undef TP_U
#undef TP_I
#undef TP_F
  return false;
}

bool SetSimField(Tuning& t, const std::string& name, float value) {
  auto& s = t.sim;
  // Integer sim fields
  struct IntEntry { const char* n; int Tuning::Sim::*p; };
  static const IntEntry iFields[] = {
    {"partGravity", &Tuning::Sim::partGravity},
    {"partMaxVel", &Tuning::Sim::partMaxVel},
    {"airDensity", &Tuning::Sim::airDensity},
    {"falloffPerCell", &Tuning::Sim::falloffPerCell},
    {"ejectSolid", &Tuning::Sim::ejectSolid},
    {"ejectLiquid", &Tuning::Sim::ejectLiquid},
    {"ejectPowder", &Tuning::Sim::ejectPowder},
    {"ejectGas", &Tuning::Sim::ejectGas},
    {"liquidEqualize", &Tuning::Sim::liquidEqualize},
    {"liquidMinFilm", &Tuning::Sim::liquidMinFilm},
    {"wanderHopMask", &Tuning::Sim::wanderHopMask},
    {"expMicroPerMille", &Tuning::Sim::expMicroPerMille},
    {"expMicroLifeTicks", &Tuning::Sim::expMicroLifeTicks},
    {"expMicroScaleIdx", &Tuning::Sim::expMicroScaleIdx},
    {"fluidEosPower", &Tuning::Sim::fluidEosPower},
    {"fluidExciteMode", &Tuning::Sim::fluidExciteMode},
    {"fluidExciteCeiling", &Tuning::Sim::fluidExciteCeiling},
    {"fluidExciteRate", &Tuning::Sim::fluidExciteRate},
    {"fluidExcitePerch", &Tuning::Sim::fluidExcitePerch},
    {"fluidExciteStep", &Tuning::Sim::fluidExciteStep},
    {"fluidSettleTicks", &Tuning::Sim::fluidSettleTicks},
    {"fluidSplashScaleIdx", &Tuning::Sim::fluidSplashScaleIdx},
    {"fluidFoamScaleIdx", &Tuning::Sim::fluidFoamScaleIdx},
    {"waterBodyMode", &Tuning::Sim::waterBodyMode},
    {"waterBodyMinVolume", &Tuning::Sim::waterBodyMinVolume},
    {"waterBodyExitVolume", &Tuning::Sim::waterBodyExitVolume},
    {"waterBodySpreadEnter", &Tuning::Sim::waterBodySpreadEnter},
    {"waterBodySpreadExit", &Tuning::Sim::waterBodySpreadExit},
    {"waterBodyQuietTicks", &Tuning::Sim::waterBodyQuietTicks},
    {"waterBodyMaxCount", &Tuning::Sim::waterBodyMaxCount},
    {"waterBodyTestDrain", &Tuning::Sim::waterBodyTestDrain},
    {"drainMaxEighthsPerTick", &Tuning::Sim::drainMaxEighthsPerTick},
    {"drainExciteRadius", &Tuning::Sim::drainExciteRadius},
    {"windMode", &Tuning::Sim::windMode},
    {"currentMode", &Tuning::Sim::currentMode},
    {"currentVortexRadius", &Tuning::Sim::currentVortexRadius},
    {"currentStreamMinSlope", &Tuning::Sim::currentStreamMinSlope},
  };
  for (const auto& e : iFields) {
    if (name == e.n) { s.*(e.p) = (int)value; return true; }
  }
  // Float sim fields
  struct FloatEntry { const char* n; float Tuning::Sim::*p; };
  static const FloatEntry fFields[] = {
    {"fluidGravity", &Tuning::Sim::fluidGravity},
    {"fluidStiffness", &Tuning::Sim::fluidStiffness},
    {"fluidRestDensity", &Tuning::Sim::fluidRestDensity},
    {"fluidCohesion", &Tuning::Sim::fluidCohesion},
    {"fluidViscosity", &Tuning::Sim::fluidViscosity},
    {"fluidDamping", &Tuning::Sim::fluidDamping},
    {"fluidFriction", &Tuning::Sim::fluidFriction},
    {"fluidSplashRate", &Tuning::Sim::fluidSplashRate},
    {"fluidSplashSpeed", &Tuning::Sim::fluidSplashSpeed},
    {"fluidSplashMaxDensity", &Tuning::Sim::fluidSplashMaxDensity},
    {"fluidSplashLife", &Tuning::Sim::fluidSplashLife},
    {"fluidFoamRate", &Tuning::Sim::fluidFoamRate},
    {"fluidFoamCrestRate", &Tuning::Sim::fluidFoamCrestRate},
    {"fluidSettledMass", &Tuning::Sim::fluidSettledMass},
    {"fluidSettleEps", &Tuning::Sim::fluidSettleEps},
    {"fluidWakeSpeed", &Tuning::Sim::fluidWakeSpeed},
    {"fluidStainRate", &Tuning::Sim::fluidStainRate},
    {"drainCd", &Tuning::Sim::drainCd},
    {"drainGravity", &Tuning::Sim::drainGravity},
    {"windDrag", &Tuning::Sim::windDrag},
    {"windFluidGain", &Tuning::Sim::windFluidGain},
    {"windFluidMass", &Tuning::Sim::windFluidMass},
    {"windDriftSpeed", &Tuning::Sim::windDriftSpeed},
    {"windDriftMax", &Tuning::Sim::windDriftMax},
    {"windEntrainSpeed", &Tuning::Sim::windEntrainSpeed},
    {"windEntrainRate", &Tuning::Sim::windEntrainRate},
    {"windGasScale", &Tuning::Sim::windGasScale},
    {"windPartScale", &Tuning::Sim::windPartScale},
    {"windDragRef", &Tuning::Sim::windDragRef},
    {"fluidAttractSame", &Tuning::Sim::fluidAttractSame},
    {"fluidAttractDiff", &Tuning::Sim::fluidAttractDiff},
    {"currentVortexGamma", &Tuning::Sim::currentVortexGamma},
    {"currentVortexDecay", &Tuning::Sim::currentVortexDecay},
    {"currentSinkSpeed", &Tuning::Sim::currentSinkSpeed},
    {"currentStreamScale", &Tuning::Sim::currentStreamScale},
    {"currentDrag", &Tuning::Sim::currentDrag},
  };
  for (const auto& e : fFields) {
    if (name == e.n) { s.*(e.p) = value; return true; }
  }
  return false;
}

// ---- variance draws ------------------------------------------------------
// Hash from sim/rng.h: the draw is a pure function of (seed, tick, index), so
// it is identical on every machine and a replay reproduces it. Never seed this
// from a counter that advances with frame boundaries or iteration order.
namespace {

using rng::Hash3;
using rng::Pcg;
using rng::Unit01;

}  // namespace

float ApplyVariance(float base, const Variance& v, uint32_t seed, uint32_t tick,
                    uint32_t index) {
  if (!v.on()) return base;
  // Entity scope drops the tick and the index: one roll per seed, stable for
  // that entity's whole life. Event scope keeps both, so every draw differs.
  const bool entity = v.scope == Variance::kEntity;
  const uint32_t t = entity ? 0x5E17A11u : tick;
  const uint32_t i = entity ? 0x9E3779B9u : index;
  const uint32_t h = Hash3(seed * 2654435761u + 0x9E3779B9u, t, i);

  float off;
  if (v.dist == Variance::kUniform) {
    off = (Unit01(h) * 2.0f - 1.0f) * v.amount;
  } else {
    // Box-Muller from two independent hash words. Deterministic and closed
    // form — no rejection loop, so it cannot vary in iteration count across
    // machines. u1 is nudged off zero because log(0) is -inf.
    const float u1 = Unit01(h) * 0.99999994f + 3e-8f;
    const float u2 = Unit01(Pcg(h ^ 0xB10057u));
    const float r = std::sqrt(-2.0f * std::log(u1));
    const float g = r * std::cos(6.28318530718f * u2);
    // Clamp the tail: `amount` is one sigma, and an unbounded gaussian on a
    // spawn count is an unbounded particle budget (rule 2).
    const float c = g < -v.sigmaClamp ? -v.sigmaClamp
                                      : (g > v.sigmaClamp ? v.sigmaClamp : g);
    off = c * v.amount;
  }
  float out = base + off;
  if (out < v.minValue) out = v.minValue;
  if (out > v.maxValue) out = v.maxValue;
  return out;
}

int ApplyVarianceI(int base, const Variance& v, uint32_t seed, uint32_t tick,
                   uint32_t index) {
  if (!v.on()) return base;
  const float f = ApplyVariance((float)base, v, seed, tick, index);
  if (!std::isfinite(f)) return base;
  const long r = std::lround(f);
  // Counts are work. A wide draw must never ask for negative work, and must
  // never overflow an int on its way to a loop bound.
  if (r < 0) return 0;
  if (r > 1000000L) return 1000000;
  return (int)r;
}

// The one celestial clock (world.h CelestialClock). Function-local static so
// it needs no initialisation order guarantee and no owner: it is dev-tool
// state, written only by the frame loop, and DISENGAGED by default so every
// headless path sees celestialTick == simTick exactly.
CelestialClock& Celestial() {
  static CelestialClock g_clock;
  return g_clock;
}

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
    ReadStr(*g, "model", p.model, out, at);
    ReadF(*g, "flySpeed", p.flySpeed, out, at);
    ReadF(*g, "flySprint", p.flySprint, out, at);
    ReadF(*g, "walkSpeed", p.walkSpeed, out, at);
    ReadF(*g, "sprintSpeed", p.sprintSpeed, out, at);
    ReadF(*g, "gravity", p.gravity, out, at);
    ReadF(*g, "jumpSpeed", p.jumpSpeed, out, at);
    ReadF(*g, "swimUp", p.swimUp, out, at);
    ReadF(*g, "swimDown", p.swimDown, out, at);
    ReadF(*g, "maxFall", p.maxFall, out, at);
    ReadF(*g, "fallDamageSpeed", p.fallDamageSpeed, out, at);
    ReadF(*g, "fallSplatSpeed", p.fallSplatSpeed, out, at);
    ReadF(*g, "fallDamageScale", p.fallDamageScale, out, at);
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
    ReadF(*g, "waterMantleSpeed", p.waterMantleSpeed, out, at);
    ReadF(*g, "waterMantleTime", p.waterMantleTime, out, at);
    ReadF(*g, "ledgeReach", p.ledgeReach, out, at);
    ReadF(*g, "ledgeHangDrop", p.ledgeHangDrop, out, at);
    ReadF(*g, "ledgeBoostSpeed", p.ledgeBoostSpeed, out, at);
    ReadF(*g, "ledgeMantleSpeed", p.ledgeMantleSpeed, out, at);
    ReadF(*g, "ledgeMantleTime", p.ledgeMantleTime, out, at);
    ReadF(*g, "ledgeSettleSpeed", p.ledgeSettleSpeed, out, at);
    ReadF(*g, "ledgeShimmySpeed", p.ledgeShimmySpeed, out, at);
    ReadF(*g, "ledgePullDelay", p.ledgePullDelay, out, at);
    ReadF(*g, "collisionWidth", p.collisionWidth, out, at);
    ReadF(*g, "collisionHeight", p.collisionHeight, out, at);
    ReadF(*g, "crouchHeight", p.crouchHeight, out, at);
    ReadF(*g, "crouchSpeedScale", p.crouchSpeedScale, out, at);
    ReadF(*g, "crouchKneeDrop", p.crouchKneeDrop, out, at);
    ReadF(*g, "halfHeight", p.halfHeight, out, at);
    ReadF(*g, "eyeOffset", p.eyeOffset, out, at);
    ReadF(*g, "viewSmoothHalflife", p.viewSmoothHalflife, out, at);
    ReadF(*g, "unstickMaxDepth", p.unstickMaxDepth, out, at);
    ReadF(*g, "unstickSpeed", p.unstickSpeed, out, at);
  }

  if (const json* g = Find(j, "camera")) {
    auto& c = out.camera;
    const std::string at = "camera";
    ReadF(*g, "mouseSensitivity", c.mouseSensitivity, out, at);
    ReadF(*g, "fovY", c.fovY, out, at);
    ReadF(*g, "pitchClamp", c.pitchClamp, out, at);
    ReadF(*g, "meleeSensitivity", c.meleeSensitivity, out, at);
    // A scale of 0 would freeze the view while guarding, which reads as the
    // game having locked up rather than as a feel choice; above 1 it is a
    // speed-up, which is a legitimate thing to try, so only the floor is hard.
    c.meleeSensitivity = std::clamp(c.meleeSensitivity, 0.05f, 2.0f);
    ReadF(*g, "meleeSensHalflife", c.meleeSensHalflife, out, at);
    c.meleeSensHalflife = std::clamp(c.meleeSensHalflife, 0.0f, 1.0f);
  }

  if (const json* g = Find(j, "thirdPerson")) {
    auto& c = out.thirdPerson;
    const std::string at = "thirdPerson";
    ReadF(*g, "distance", c.distance, out, at);
    ReadF(*g, "shoulderDist", c.shoulderDist, out, at);
    ReadF(*g, "shoulderOffset", c.shoulderOffset, out, at);
    ReadF(*g, "heightOffset", c.heightOffset, out, at);
    ReadF(*g, "sideOffset", c.sideOffset, out, at);
    ReadF(*g, "collideMargin", c.collideMargin, out, at);
    ReadF(*g, "collideRadius", c.collideRadius, out, at);
    ReadB(*g, "collide", c.collide, out, at);
    ReadF(*g, "focusHalflife", c.focusHalflife, out, at);
    ReadF(*g, "distInHalflife", c.distInHalflife, out, at);
    ReadF(*g, "distOutHalflife", c.distOutHalflife, out, at);
    ReadF(*g, "pitchLift", c.pitchLift, out, at);
    ReadF(*g, "stateFollow", c.stateFollow, out, at);
    ReadF(*g, "speedFov", c.speedFov, out, at);
    ReadF(*g, "speedFovHalflife", c.speedFovHalflife, out, at);
    ReadF(*g, "zoomStep", c.zoomStep, out, at);
    ReadF(*g, "zoomMin", c.zoomMin, out, at);
    ReadF(*g, "zoomMax", c.zoomMax, out, at);
    // An inverted or degenerate range would let the wheel drive the boom to
    // zero (the camera inside the character) or past the far cascade. Clamped
    // rather than reported, because both ends have a defensible meaning and the
    // honest failure is a zoom that stops sooner than asked.
    c.zoomMin = std::clamp(c.zoomMin, 0.05f, 4.0f);
    c.zoomMax = std::clamp(c.zoomMax, c.zoomMin, 8.0f);
    c.zoomStep = std::clamp(c.zoomStep, 0.0f, 1.0f);
  }

  if (const json* g = Find(j, "gear")) {
    auto& c = out.gear;
    const std::string at = "gear";
    ReadF(*g, "ruinedCondition", c.ruinedCondition, out, at);
    c.ruinedCondition = std::clamp(c.ruinedCondition, 0.0f, 1.0f);
    ReadF(*g, "cutHardnessRef", c.cutHardnessRef, out, at);
    ReadF(*g, "cutHardnessMin", c.cutHardnessMin, out, at);
    if (c.cutHardnessRef < 0.0f) c.cutHardnessRef = 0.0f;
    c.cutHardnessMin = std::clamp(c.cutHardnessMin, 0.0f, 1.0f);
  }

  if (const json* g = Find(j, "avatar")) {
    auto& a = out.avatar;
    const std::string at = "avatar";
    ReadB(*g, "enabled", a.enabled, out, at);
    ReadF(*g, "turnRate", a.turnRate, out, at);
    ReadF(*g, "turnMinSpeed", a.turnMinSpeed, out, at);
    ReadF(*g, "velocityHalflife", a.velocityHalflife, out, at);
    ReadF(*g, "firstPersonTurnHalflife", a.firstPersonTurnHalflife, out, at);
    ReadF(*g, "headLookYaw", a.headLookYaw, out, at);
    // 0 is meaningful (body always faces the camera, the old behaviour); the
    // ceiling stops a value that would let the head face backwards.
    a.headLookYaw = std::clamp(a.headLookYaw, 0.0f, 110.0f);
    ReadF(*g, "headLookReleaseYaw", a.headLookReleaseYaw, out, at);
    // 0 disables the release. The ceiling is 180 minus the head cone: a band
    // wider than the reachable range would be fading a look that has not
    // started yet, so the neck would never reach its stop at any angle.
    a.headLookReleaseYaw =
        std::clamp(a.headLookReleaseYaw, 0.0f, 180.0f - a.headLookYaw);
    ReadF(*g, "headLookPitchUp", a.headLookPitchUp, out, at);
    a.headLookPitchUp = std::clamp(a.headLookPitchUp, 0.0f, 89.0f);
    ReadF(*g, "headLookPitchDown", a.headLookPitchDown, out, at);
    a.headLookPitchDown = std::clamp(a.headLookPitchDown, 0.0f, 89.0f);
    ReadF(*g, "headLookSpine", a.headLookSpine, out, at);
    a.headLookSpine = std::clamp(a.headLookSpine, 0.0f, 1.0f);
    ReadF(*g, "headLookHalflife", a.headLookHalflife, out, at);
    a.headLookHalflife = std::clamp(a.headLookHalflife, 0.0f, 1.0f);
    ReadF(*g, "headLookRecenterHalflife", a.headLookRecenterHalflife, out, at);
    a.headLookRecenterHalflife =
        std::clamp(a.headLookRecenterHalflife, 0.0f, 3.0f);
    ReadF(*g, "ikBlendHalflife", a.ikBlendHalflife, out, at);
    ReadF(*g, "airDebounce", a.airDebounce, out, at);
    ReadF(*g, "fallFlailDelay", a.fallFlailDelay, out, at);
    ReadF(*g, "fallFlailRamp", a.fallFlailRamp, out, at);
    a.fallFlailRamp = std::max(a.fallFlailRamp, 0.0f);
    ReadF(*g, "fallMinDrop", a.fallMinDrop, out, at);
    ReadB(*g, "firstPersonArms", a.firstPersonArms, out, at);
    ReadF(*g, "footTrim", a.footTrim, out, at);
    ReadF(*g, "severImpulse", a.severImpulse, out, at);
    ReadF(*g, "respawnDelay", a.respawnDelay, out, at);
  }

  if (const json* g = Find(j, "audio")) {
    auto& a = out.audio;
    const std::string at = "audio";
    ReadB(*g, "enabled", a.enabled, out, at);
    ReadF(*g, "masterVolume", a.masterVolume, out, at);
    ReadF(*g, "footstepVolume", a.footstepVolume, out, at);
    ReadF(*g, "footstepRadius", a.footstepRadius, out, at);
    ReadF(*g, "footstepPitchJitter", a.footstepPitchJitter, out, at);
    ReadF(*g, "footstepWalkGain", a.footstepWalkGain, out, at);
    ReadF(*g, "footstepSprintGain", a.footstepSprintGain, out, at);
    ReadF(*g, "footstepSprintSpeed", a.footstepSprintSpeed, out, at);
    ReadF(*g, "footstepFootDetune", a.footstepFootDetune, out, at);
    ReadF(*g, "landVolume", a.landVolume, out, at);
    ReadF(*g, "landFullSpeed", a.landFullSpeed, out, at);
    ReadF(*g, "impactVolume", a.impactVolume, out, at);
    ReadF(*g, "impactRadius", a.impactRadius, out, at);
    ReadF(*g, "impactMinSpeed", a.impactMinSpeed, out, at);
    ReadF(*g, "impactFullSpeed", a.impactFullSpeed, out, at);
    ReadF(*g, "impactMinGap", a.impactMinGap, out, at);
    ReadF(*g, "breakVolume", a.breakVolume, out, at);
    ReadF(*g, "breakRadius", a.breakRadius, out, at);
    ReadF(*g, "breakPitchSemitones", a.breakPitchSemitones, out, at);
    ReadF(*g, "breakSmallRate", a.breakSmallRate, out, at);
    ReadF(*g, "breakBigRate", a.breakBigRate, out, at);
    ReadF(*g, "breakBigVoxels", a.breakBigVoxels, out, at);
    // An octave of detune either way is already past the point where the
    // material stops sounding like itself; beyond that it is a bug, not taste.
    a.breakPitchSemitones = std::clamp(a.breakPitchSemitones, 0.0f, 12.0f);
    // Rates are clamped to what the resampler can do without turning a break
    // into a click or a drone.
    a.breakSmallRate = std::clamp(a.breakSmallRate, 0.25f, 4.0f);
    a.breakBigRate = std::clamp(a.breakBigRate, 0.25f, 4.0f);
    // Guard the size mapping's divisor: a 0 here would make every break the
    // "big" pitch via a division by zero.
    a.breakBigVoxels = std::max(a.breakBigVoxels, 1.0f);
    ReadF(*g, "mobVolume", a.mobVolume, out, at);
    ReadF(*g, "mobRadius", a.mobRadius, out, at);
    ReadF(*g, "mobPitchJitter", a.mobPitchJitter, out, at);
    ReadF(*g, "dismemberVolume", a.dismemberVolume, out, at);
    ReadF(*g, "dismemberPitchJitter", a.dismemberPitchJitter, out, at);
    ReadF(*g, "bleedVolume", a.bleedVolume, out, at);
    ReadF(*g, "bleedRadius", a.bleedRadius, out, at);
    ReadF(*g, "bleedOnThreshold", a.bleedOnThreshold, out, at);
    ReadF(*g, "bleedOffThreshold", a.bleedOffThreshold, out, at);
    ReadF(*g, "ambienceVolume", a.ambienceVolume, out, at);
    ReadF(*g, "ambienceRadius", a.ambienceRadius, out, at);
    ReadF(*g, "nightVolume", a.nightVolume, out, at);
    ReadF(*g, "nightRadius", a.nightRadius, out, at);
    ReadF(*g, "nightChance", a.nightChance, out, at);
    ReadF(*g, "nightRetrySeconds", a.nightRetrySeconds, out, at);
    ReadF(*g, "nightFadeRate", a.nightFadeRate, out, at);
    a.bleedOnThreshold = std::clamp(a.bleedOnThreshold, 0.0f, 1.0f);
    a.bleedOffThreshold = std::clamp(a.bleedOffThreshold, 0.0f, 1.0f);
    // The hysteresis only works while off < on. Equal (or inverted) collapses
    // it to a single threshold and the loop chatters on and off every frame at
    // the boundary, so pull `off` down rather than trusting the author.
    if (a.bleedOffThreshold >= a.bleedOnThreshold) {
      out.warnings.push_back(
          at + ".bleedOffThreshold >= bleedOnThreshold; lowered to keep the "
               "loop from retriggering every frame");
      a.bleedOffThreshold = a.bleedOnThreshold * 0.5f;
    }
    a.nightChance = std::clamp(a.nightChance, 0.0f, 1.0f);
    // A zero retry period would roll every frame, which is not "rare" at any
    // chance; a zero fade rate would leave the bed stuck silent forever.
    a.nightRetrySeconds = std::max(a.nightRetrySeconds, 1.0f);
    a.nightFadeRate = std::clamp(a.nightFadeRate, 0.0001f, 1.0f);
    ReadF(*g, "reverbWet", a.reverbWet, out, at);
    ReadB(*g, "occlusion", a.occlusion, out, at);
    ReadF(*g, "occlusionMaxDb", a.occlusionMaxDb, out, at);
    ReadF(*g, "occlusionMinCutoffHz", a.occlusionMinCutoffHz, out, at);
    ReadF(*g, "occlusionScale", a.occlusionScale, out, at);
    ReadF(*g, "occlusionCutoffScale", a.occlusionCutoffScale, out, at);
    ReadF(*g, "occlusionMaxRangeM", a.occlusionMaxRangeM, out, at);
    ReadF(*g, "occlusionWetKeep", a.occlusionWetKeep, out, at);
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
    ReadF(*g, "explosionBodyDamageScale", p.explosionBodyDamageScale, out, at);
    ReadF(*g, "playerMassKg", p.playerMassKg, out, at);
    ReadF(*g, "sphereFriction", p.sphereFriction, out, at);
    ReadF(*g, "sphereRestitution", p.sphereRestitution, out, at);
    ReadF(*g, "sphereAngularDamping", p.sphereAngularDamping, out, at);
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

  if (const json* g = Find(j, "gore")) {
    auto& e = out.gore;
    const std::string at = "gore";
    // Read order mirrors the banding in Tuning::Gore: micro spray first
    // (shared droplet properties, then bleed, then sever), whole-voxel blood
    // second, variance last.
    // ---- A. micro spray ----
    ReadI(*g, "microLifeTicks", e.microLifeTicks, out, at);
    ReadI(*g, "microScale", e.microScale, out, at);
    ReadF(*g, "bleedSprayPerDrip", e.bleedSprayPerDrip, out, at);
    ReadF(*g, "bleedSpraySpeed", e.bleedSpraySpeed, out, at);
    ReadF(*g, "bleedSprayCone", e.bleedSprayCone, out, at);
    ReadI(*g, "severSpray", e.severSpray, out, at);
    ReadI(*g, "severDecayTicks", e.severDecayTicks, out, at);
    ReadF(*g, "severSpraySpeed", e.severSpraySpeed, out, at);
    ReadF(*g, "severSprayCone", e.severSprayCone, out, at);
    // ---- B. whole-voxel blood ----
    ReadF(*g, "bleedVoxelGain", e.bleedVoxelGain, out, at);
    ReadF(*g, "bleedBudgetCap", e.bleedBudgetCap, out, at);
    ReadF(*g, "severStumpBudget", e.severStumpBudget, out, at);
    ReadF(*g, "corpseBleedPerVoxel", e.corpseBleedPerVoxel, out, at);
    ReadI(*g, "bleedDripTicks", e.bleedDripTicks, out, at);
    ReadI(*g, "bleedOpsPerTick", e.bleedOpsPerTick, out, at);
    ReadI(*g, "bleedClumpRadius", e.bleedClumpRadius, out, at);
    ReadI(*g, "severVoxels", e.severVoxels, out, at);
    ReadF(*g, "severVoxelSpeed", e.severVoxelSpeed, out, at);
    ReadI(*g, "severGobbetVoxels", e.severGobbetVoxels, out, at);
    ReadF(*g, "severGobbetSpread", e.severGobbetSpread, out, at);
    // ---- C. variance ----
    ReadF(*g, "bleedGain", e.bleedGain, out, at);
    ReadVar(*g, "bleedGainVar", e.bleedGainVar, out, at);
    ReadVar(*g, "bleedSprayPerDripVar", e.bleedSprayPerDripVar, out, at);
    ReadVar(*g, "bleedSpraySpeedVar", e.bleedSpraySpeedVar, out, at);
    ReadVar(*g, "bleedSprayConeVar", e.bleedSprayConeVar, out, at);
    ReadVar(*g, "severSprayVar", e.severSprayVar, out, at);
    ReadVar(*g, "severSpraySpeedVar", e.severSpraySpeedVar, out, at);
    ReadVar(*g, "severSprayConeVar", e.severSprayConeVar, out, at);
    ReadVar(*g, "severVoxelsVar", e.severVoxelsVar, out, at);
    ReadVar(*g, "severVoxelSpeedVar", e.severVoxelSpeedVar, out, at);
    ReadVar(*g, "severDecayTicksVar", e.severDecayTicksVar, out, at);
    ReadVar(*g, "microLifeTicksVar", e.microLifeTicksVar, out, at);
    // ---- D. crater shape ----
    ReadF(*g, "carveChunkiness", e.carveChunkiness, out, at);
    // 0 is a load-bearing value, not just the low end: the mob gate asserts
    // that carveChunkiness 0 removes byte-identically the same voxels the old
    // white-noise crater did. Clamping it here is what keeps that identity
    // reachable from a slider that has been dragged past the end.
    e.carveChunkiness = std::clamp(e.carveChunkiness, 0.0f, 1.0f);
    ReadF(*g, "carveBlobSize", e.carveBlobSize, out, at);
    e.carveBlobSize = std::max(e.carveBlobSize, 0.5f);
    ReadF(*g, "carveFalloff", e.carveFalloff, out, at);
    e.carveFalloff = std::clamp(e.carveFalloff, 1.0f, 8.0f);
    ReadI(*g, "carveSpallRounds", e.carveSpallRounds, out, at);
    // Each round is a full pass over the limb's voxels and can only ever
    // REMOVE, so an unbounded count is a way to erase a body one slider drag
    // at a time. Four is already past the point where more reads as different.
    e.carveSpallRounds = std::clamp(e.carveSpallRounds, 0, 4);
    // ---- E. the wound model (game/mob.h Mob::CutLimb) ----------------------
    ReadF(*g, "cutDepth", e.cutDepth, out, at);
    ReadF(*g, "cutDepthPower", e.cutDepthPower, out, at);
    ReadF(*g, "cutLength", e.cutLength, out, at);
    ReadF(*g, "cutWidth", e.cutWidth, out, at);
    ReadI(*g, "cutSpallRounds", e.cutSpallRounds, out, at);
    ReadF(*g, "cutSpallStrength", e.cutSpallStrength, out, at);
    ReadF(*g, "woundHeftRef", e.woundHeftRef, out, at);
    ReadF(*g, "woundHeftMax", e.woundHeftMax, out, at);
    ReadF(*g, "woundStainRadius", e.woundStainRadius, out, at);
    ReadF(*g, "woundStainSurface", e.woundStainSurface, out, at);
    ReadF(*g, "woundStainDensity", e.woundStainDensity, out, at);
    ReadF(*g, "woundSeverFraction", e.woundSeverFraction, out, at);
    ReadF(*g, "woundNeckRadius", e.woundNeckRadius, out, at);
    ReadF(*g, "woundNeckFraction", e.woundNeckFraction, out, at);
    ReadF(*g, "woundImpactSeverScale", e.woundImpactSeverScale, out, at);
    // ---- F. blood is health / G. burns cap health (game/mob.h) -------------
    ReadF(*g, "bleedHpPerVoxel", e.bleedHpPerVoxel, out, at);
    ReadB(*g, "stumpBleedsOpen", e.stumpBleedsOpen, out, at);
    ReadF(*g, "burnCapMidFraction", e.burnCapMidFraction, out, at);
    ReadF(*g, "burnCapMidHealth", e.burnCapMidHealth, out, at);
    ReadF(*g, "burnDeathFraction", e.burnDeathFraction, out, at);
    // BOUNDS, not taste. Each of these is a value that turns the wound model
    // into something other than a wound model at the ends of its range, and
    // the tuner offers a text box as well as a slider.
    //
    // A negative depth or length is a kerf that removes nothing, which reads
    // as "the sword stopped working" rather than as a bad number; a huge one
    // is a swing that erases a limb, which is exactly the behaviour this whole
    // change exists to remove. woundHeftRef divides, so zero is a division by
    // zero dressed up as a slider.
    e.cutDepth = std::clamp(e.cutDepth, 0.0f, 8.0f);
    e.cutDepthPower = std::clamp(e.cutDepthPower, 0.0f, 8.0f);
    e.cutLength = std::clamp(e.cutLength, 0.1f, 16.0f);
    e.cutWidth = std::clamp(e.cutWidth, 0.05f, 4.0f);
    e.cutSpallRounds = std::clamp(e.cutSpallRounds, 0, 4);
    e.cutSpallStrength = std::clamp(e.cutSpallStrength, 0.0f, 1.0f);
    if (e.woundHeftRef < 0.01f) {
      out.warnings.push_back(at + ".woundHeftRef <= 0; clamped");
      e.woundHeftRef = 0.01f;
    }
    e.woundHeftMax = std::clamp(e.woundHeftMax, 1.0f, 32.0f);
    e.woundStainRadius = std::clamp(e.woundStainRadius, 0.0f, 16.0f);
    e.woundStainSurface = std::clamp(e.woundStainSurface, 0.0f, 1.0f);
    e.woundStainDensity = std::clamp(e.woundStainDensity, 0.0f, 1.0f);
    // A sever fraction of 0 severs on the first disconnected speck — that is
    // the straggler bug in Mob::CarveLimb wearing a slider — and 1 can never
    // fire at all. Both ends are excluded rather than merely discouraged.
    e.woundSeverFraction = std::clamp(e.woundSeverFraction, 0.05f, 0.95f);
    e.woundNeckRadius = std::clamp(e.woundNeckRadius, 0.0f, 16.0f);
    e.woundNeckFraction = std::clamp(e.woundNeckFraction, 0.0f, 0.95f);
    e.woundImpactSeverScale = std::max(e.woundImpactSeverScale, 0.0f);
    // A negative rate would make bleeding HEAL, which is not a tuning
    // mistake anyone means; zero is a legitimate "blood is cosmetic again".
    e.bleedHpPerVoxel = std::max(e.bleedHpPerVoxel, 0.0f);
    // The burn curve must stay a curve: the mid knot strictly inside (0,
    // death), the death knot at most 1 (a body cannot be more than all
    // burnt), and the mid health inside [0, 1] so the cap is monotone.
    e.burnDeathFraction = std::clamp(e.burnDeathFraction, 0.05f, 1.0f);
    e.burnCapMidFraction =
        std::clamp(e.burnCapMidFraction, 0.01f, e.burnDeathFraction - 0.01f);
    e.burnCapMidHealth = std::clamp(e.burnCapMidHealth, 0.0f, 1.0f);
    // A negative or zero gain would silently disable bleeding rather than
    // reading as a tuning mistake, so floor it just above zero.
    if (e.bleedGain < 0.0f) {
      out.warnings.push_back(at + ".bleedGain < 0; clamped to 0");
      e.bleedGain = 0.0f;
    }
    // The life field is 8 bits in Particle.flags (PMICRO_LIFE_MASK). A value
    // past 255 would wrap and produce droplets that die instantly, which reads
    // as "the spray stopped working" rather than as a bad number.
    if (e.microLifeTicks < 1 || e.microLifeTicks > 255) {
      out.warnings.push_back(at + ".microLifeTicks out of 1..255; clamped");
      e.microLifeTicks = e.microLifeTicks < 1 ? 1 : 255;
    }
    // Only 2/3/4/6 are representable in the 2-bit scale field.
    if (e.microScale != 2 && e.microScale != 3 && e.microScale != 4 &&
        e.microScale != 6) {
      out.warnings.push_back("gore.microScale must be 2, 3, 4 or 6; using 4");
      e.microScale = 4;
    }
    if (e.severDecayTicks < 1) e.severDecayTicks = 1;
    // ---- whole-voxel bleeding bounds (rule 2) ----
    // The drip period and the op budget together cap how much real matter a
    // wound can push into the CA, so both need a hard floor/ceiling here
    // rather than a "should be sensible" comment. A period of 0 would drip
    // every tick AND divide nothing — it is the one value that turns a wound
    // into an unbounded fountain.
    if (e.bleedDripTicks < 1) {
      out.warnings.push_back(at + ".bleedDripTicks < 1; clamped to 1");
      e.bleedDripTicks = 1;
    }
    // 64 is far above anything the look needs (six is the shipped value) and
    // still leaves the per-tick cell-op queue overwhelmingly free.
    if (e.bleedOpsPerTick < 0 || e.bleedOpsPerTick > 64) {
      out.warnings.push_back(at + ".bleedOpsPerTick out of 0..64; clamped");
      e.bleedOpsPerTick = e.bleedOpsPerTick < 0 ? 0 : 64;
    }
    if (e.bleedVoxelGain < 0.0f) {
      out.warnings.push_back(at + ".bleedVoxelGain < 0; clamped to 0");
      e.bleedVoxelGain = 0.0f;
    }
    // The cap is what bounds ONE wound's total output. Zero means a wound
    // never owes anything, i.e. drips are off; negative is meaningless.
    if (e.bleedBudgetCap < 0.0f) {
      out.warnings.push_back(at + ".bleedBudgetCap < 0; clamped to 0");
      e.bleedBudgetCap = 0.0f;
    }
    if (e.corpseBleedPerVoxel < 0.0f) e.corpseBleedPerVoxel = 0.0f;
    if (e.severStumpBudget < 0.0f) {
      out.warnings.push_back(at + ".severStumpBudget < 0; clamped to 0");
      e.severStumpBudget = 0.0f;
    }
    // ---- clump size (rule 2) ----
    // 3 is the ceiling, not the shader's max brush radius of 7. A drip is a
    // REPEATING source: the sphere volume goes 1/7/33/123/257, so by radius 4
    // one wound is pushing a quarter of a chunk's worth of liquid per drip and
    // the CA cannot settle it between drips — the chunk stops sleeping, which
    // is a rule 2 failure that presents as a perf bug. The budget debit scales
    // with the same volume (BleedClumpVoxels), so a big clump empties a wound
    // proportionally faster rather than multiplying its total output.
    if (e.bleedClumpRadius < 0 || e.bleedClumpRadius > 3) {
      out.warnings.push_back(at + ".bleedClumpRadius out of 0..3; clamped");
      e.bleedClumpRadius = e.bleedClumpRadius < 0 ? 0 : 3;
    }
    // A gobbet subdivides severVoxels rather than multiplying it, so the only
    // real bound needed is "at least one particle per gobbet". 64 is a sanity
    // ceiling: past it a gobbet exceeds any sane severVoxels and the throw
    // degenerates to a single lump.
    if (e.severGobbetVoxels < 1 || e.severGobbetVoxels > 64) {
      out.warnings.push_back(at + ".severGobbetVoxels out of 1..64; clamped");
      e.severGobbetVoxels = e.severGobbetVoxels < 1 ? 1 : 64;
    }
    if (e.severGobbetSpread < 0.0f) {
      out.warnings.push_back(at + ".severGobbetSpread < 0; clamped to 0");
      e.severGobbetSpread = 0.0f;
    }
  }

  // ---- melee: the stroke driver's feel (game/melee.h MeleeTuning) -----------
  // Read straight into Tuning::Melee; game/melee.cpp ApplyMeleeTuning is what
  // turns this into a MeleeTuning (and what does the metres -> world voxel
  // conversion for the four keys whose names carry a unit).
  if (const json* g = Find(j, "melee")) {
    auto& e = out.melee;
    const std::string at = "melee";
    ReadF(*g, "commitSpeed", e.commitSpeed, out, at);
    ReadF(*g, "slashTime", e.slashTime, out, at);
    ReadF(*g, "recoverTime", e.recoverTime, out, at);
    ReadF(*g, "fullSpeedMps", e.fullSpeedMps, out, at);
    ReadF(*g, "minSpeedMps", e.minSpeedMps, out, at);
    ReadF(*g, "aimGainX", e.aimGainX, out, at);
    ReadF(*g, "aimGainY", e.aimGainY, out, at);
    ReadF(*g, "reachGainM", e.reachGainM, out, at);
    ReadF(*g, "azOut", e.azOut, out, at);
    ReadF(*g, "azAcross", e.azAcross, out, at);
    ReadF(*g, "elMin", e.elMin, out, at);
    ReadF(*g, "elMax", e.elMax, out, at);
    ReadF(*g, "handExtend", e.handExtend, out, at);
    ReadF(*g, "extendSmoothing", e.extendSmoothing, out, at);
    ReadF(*g, "leanTurnRate", e.leanTurnRate, out, at);
    ReadF(*g, "handLead", e.handLead, out, at);
    ReadF(*g, "fallbackReachM", e.fallbackReachM, out, at);
    ReadF(*g, "reachFraction", e.reachFraction, out, at);
    ReadF(*g, "guardForwardM", e.guardForwardM, out, at);
    ReadF(*g, "guardUpM", e.guardUpM, out, at);
    ReadF(*g, "guardSideM", e.guardSideM, out, at);
    ReadF(*g, "dirSmoothing", e.dirSmoothing, out, at);
    ReadF(*g, "swingArc", e.swingArc, out, at);
    ReadF(*g, "swingAnticipate", e.swingAnticipate, out, at);
    ReadF(*g, "swingExtend", e.swingExtend, out, at);
    ReadF(*g, "bladeSmoothing", e.bladeSmoothing, out, at);
    ReadF(*g, "wristMaxAngle", e.wristMaxAngle, out, at);
    ReadF(*g, "steerSpeedLoMps", e.steerSpeedLoMps, out, at);
    ReadF(*g, "steerSpeedHiMps", e.steerSpeedHiMps, out, at);
    ReadF(*g, "steerFloor", e.steerFloor, out, at);
    ReadF(*g, "armSmoothing", e.armSmoothing, out, at);
    ReadF(*g, "wristSmoothing", e.wristSmoothing, out, at);
    ReadF(*g, "handBackFrac", e.handBackFrac, out, at);
    ReadF(*g, "elbowPoleCone", e.elbowPoleCone, out, at);
    ReadF(*g, "elbowAxisCone", e.elbowAxisCone, out, at);
    ReadF(*g, "edgeFloor", e.edgeFloor, out, at);
    ReadF(*g, "blockGapM", e.blockGapM, out, at);
    ReadF(*g, "blockItemDamage", e.blockItemDamage, out, at);
    ReadF(*g, "blockNudgeAz", e.blockNudgeAz, out, at);
    ReadF(*g, "blockNudgeEl", e.blockNudgeEl, out, at);
    ReadI(*g, "controlMode", e.controlMode, out, at);
    ReadF(*g, "pickMinSpeed", e.pickMinSpeed, out, at);
    ReadF(*g, "aimYaw", e.aimYaw, out, at);
    // 180 is meaningful: the swing follows the camera wherever it looks (the
    // pre-cone behaviour). 0 pins every swing to the body's facing.
    e.aimYaw = std::clamp(e.aimYaw, 0.0f, 180.0f);
    ReadF(*g, "aimReleaseYaw", e.aimReleaseYaw, out, at);
    // Same ceiling law as avatar.headLookReleaseYaw: a band wider than the
    // reach past the cone would fade a swing that never reached its stop.
    e.aimReleaseYaw = std::clamp(e.aimReleaseYaw, 0.0f, 180.0f - e.aimYaw);
    ReadF(*g, "torsoShare", e.torsoShare, out, at);
    ReadF(*g, "torsoPitch", e.torsoPitch, out, at);
    ReadF(*g, "headClearM", e.headClearM, out, at);

    // ---- BOUNDS, not taste ---------------------------------------------------
    // Every clamp here protects a STRUCTURAL property of the stroke rather than
    // an opinion about how it should feel, and each one is a thing a slider
    // dragged to its end can otherwise break outright:
    //
    //   * a zero or negative time divides in SlashProgress and in the phase
    //     machine's own normalisation;
    //   * a zero speed BAND makes the damage ramp `(v - min) / (full - min)`
    //     singular, and the sweep's own `max(..., 1e-3f)` would then quietly
    //     turn every touch into a full-power hit;
    //   * a smoothing halflife of 0 is a step function, which is what the lean
    //     plane's rate limit exists to avoid (melee.h: the interpolation
    //     between two opposite leans passes through the degenerate radius);
    //   * an aim gain of 0 disconnects the mouse, which reads as a hung game
    //     rather than as a setting.
    e.commitSpeed = std::max(e.commitSpeed, 1.0f);
    e.slashTime = std::clamp(e.slashTime, 0.02f, 2.0f);
    e.recoverTime = std::clamp(e.recoverTime, 0.02f, 2.0f);
    e.fullSpeedMps = std::clamp(e.fullSpeedMps, 0.05f, 60.0f);
    e.minSpeedMps = std::clamp(e.minSpeedMps, 0.0f, 60.0f);
    if (e.minSpeedMps >= e.fullSpeedMps) {
      out.warnings.push_back(at +
                             ".minSpeedMps >= fullSpeedMps; the damage ramp "
                             "has no band. Pulling min down to 0.5x full.");
      e.minSpeedMps = e.fullSpeedMps * 0.5f;
    }
    e.aimGainX = std::clamp(e.aimGainX, 0.0002f, 0.10f);
    e.aimGainY = std::clamp(e.aimGainY, 0.0002f, 0.10f);
    e.reachGainM = std::clamp(e.reachGainM, 0.0f, 0.2f);
    // The azimuth stops are asymmetric because an arm is (melee.h), but both
    // have to leave a window the seed can live in: a walk cycle parks the
    // weapon arm near the bottom of the elevation range, and a window that
    // could not represent it would move the blade on the take-over tick — the
    // one thing the whole design forbids.
    e.azOut = std::clamp(e.azOut, 0.20f, 3.10f);
    e.azAcross = std::clamp(e.azAcross, 0.20f, 3.10f);
    e.elMin = std::clamp(e.elMin, -1.55f, -0.10f);
    e.elMax = std::clamp(e.elMax, 0.10f, 1.55f);
    e.handExtend = std::clamp(e.handExtend, 0.15f, 1.0f);
    e.extendSmoothing = std::clamp(e.extendSmoothing, 0.005f, 2.0f);
    e.leanTurnRate = std::clamp(e.leanTurnRate, 0.5f, 120.0f);
    // ONLY THE SIGN IS READ (melee.h), so normalise it here rather than letting
    // a 0 in the JSON become an unreadable "the hand neither leads nor trails".
    e.handLead = e.handLead < 0.0f ? -1.0f : 1.0f;
    e.fallbackReachM = std::clamp(e.fallbackReachM, 0.05f, 4.0f);
    // Strictly under 1: a fully straight two-bone chain is a locked elbow, and
    // AnimSolveTwoBone clamps to its own annulus anyway — a target outside the
    // reach costs mouse travel to wind back before the arm visibly moves, which
    // is exactly the "the input went dead" feel integration exists to avoid.
    e.reachFraction = std::clamp(e.reachFraction, 0.20f, 0.99f);
    e.guardForwardM = std::clamp(e.guardForwardM, -2.0f, 2.0f);
    e.guardUpM = std::clamp(e.guardUpM, -2.0f, 2.0f);
    e.guardSideM = std::clamp(e.guardSideM, -2.0f, 2.0f);
    e.dirSmoothing = std::clamp(e.dirSmoothing, 0.005f, 1.0f);
    e.swingArc = std::clamp(e.swingArc, 0.0f, 3.10f);
    e.swingAnticipate = std::clamp(e.swingAnticipate, 0.0f, 1.0f);
    e.swingExtend = std::clamp(e.swingExtend, 0.0f, 1.0f);
    e.bladeSmoothing = std::clamp(e.bladeSmoothing, 0.005f, 1.0f);
    // Past ~pi the "wrist" is a ball joint and the fist can end up facing
    // backwards down its own arm (game/mob.cpp applies this as a slerp limit).
    e.wristMaxAngle = std::clamp(e.wristMaxAngle, 0.0f, 3.14f);
    // ---- THE STEER RAMP, and what each bound is protecting -------------------
    //
    //   * the two speeds are a BAND, so an inverted or zero-width one makes the
    //     ramp `(v - lo) / (hi - lo)` singular. `steerSpeedHiMps` is pushed
    //     above the low end rather than the pair being swapped: the low end is
    //     the one a player tunes ("how still is still?") and swapping would
    //     silently redefine the knob they just dragged.
    //   * steerFloor at 1 disables the ramp entirely and restores the previous
    //     always-align behaviour, which is a legitimate setting and the A/B for
    //     the whole change. 0 is legitimate too: a guard that shares nothing
    //     with the stroke.
    e.steerSpeedLoMps = std::clamp(e.steerSpeedLoMps, 0.0f, 20.0f);
    e.steerSpeedHiMps = std::clamp(e.steerSpeedHiMps, 0.0f, 40.0f);
    if (e.steerSpeedHiMps <= e.steerSpeedLoMps) {
      out.warnings.push_back(at +
                             ".steerSpeedHiMps <= steerSpeedLoMps; the wrist "
                             "ramp has no band. Pushing high to 2x low.");
      e.steerSpeedHiMps = std::max(e.steerSpeedLoMps * 2.0f, 0.05f);
    }
    e.steerFloor = std::clamp(e.steerFloor, 0.0f, 1.0f);
    // ---- PER-JOINT SMOOTHING + THE HAND'S FRONTAL PLANE ---------------------
    // Halflives: 0 is a legitimate off switch (SmoothAlpha returns 1); the
    // ceilings only stop a fat-fingered "2" from turning the arm into taffy.
    // handBackFrac at 0 pins the hand to the plane exactly; past ~0.5 the
    // clamp stops meaning "in front" at all.
    e.armSmoothing = std::clamp(e.armSmoothing, 0.0f, 1.0f);
    e.wristSmoothing = std::clamp(e.wristSmoothing, 0.0f, 1.0f);
    e.handBackFrac = std::clamp(e.handBackFrac, 0.0f, 0.5f);
    // ---- THE ELBOW/SHOULDER CONES -------------------------------------------
    // Both are half-angles, so pi is "unbounded" and restores the old free
    // behaviour for an A/B. The floors are not zero: a cone of exactly 0 pins
    // the bend plane to the authored one, which is a stiff arm rather than a
    // wrong one, and is worth being able to ask for — but a NEGATIVE one would
    // make `acos(c) > cone` true on every tick including the aligned case.
    e.elbowPoleCone = std::clamp(e.elbowPoleCone, 0.05f, 3.14f);
    e.elbowAxisCone = std::clamp(e.elbowAxisCone, 0.0f, 3.14f);
    e.edgeFloor = std::clamp(e.edgeFloor, 0.0f, 1.0f);
    // ---- THE BLOCK KNOBS, and what each clamp is protecting ------------------
    //
    //   * blockGapM is the slack in the segment-vs-segment test. NEGATIVE would
    //     mean the two blades must overlap to register, which no real pair of
    //     sweeps does, and a parry that can never fire is indistinguishable
    //     from the feature being absent. The ceiling is the other failure and
    //     the more insidious one: a gap of half a metre parries blows that
    //     passed a foot from the defender's blade, which reads as "the AI is
    //     invincible" rather than as a bad number.
    //   * blockItemDamage above 1 makes the blocking weapon take MORE than the
    //     blow carried, so parrying is worse than being cut, which inverts the
    //     mechanic. 0 is allowed on purpose — a world where blades never wear.
    //   * the two nudges are radians of stroke angle at full power, applied
    //     every parry. Past ~1 rad a single block spins the defender's guard
    //     most of the way across its own azimuth stops, and a fast exchange
    //     turns into the guard oscillating instead of guarding. 0 is allowed:
    //     that is "blocking is free", a legitimate setting, not a broken one.
    e.blockGapM = std::clamp(e.blockGapM, 0.0f, 0.5f);
    e.blockItemDamage = std::clamp(e.blockItemDamage, 0.0f, 1.0f);
    e.blockNudgeAz = std::clamp(e.blockNudgeAz, 0.0f, 1.0f);
    e.blockNudgeEl = std::clamp(e.blockNudgeEl, 0.0f, 1.0f);
    // An out-of-range mode is a typo, not a request: clamp to the two that
    // exist so nothing downstream needs a default arm (handLead's convention).
    e.controlMode = std::clamp(e.controlMode, 0, 1);
    e.pickMinSpeed = std::max(e.pickMinSpeed, 1.0f);
    e.torsoShare = std::clamp(e.torsoShare, 0.0f, 1.0f);
    e.torsoPitch = std::clamp(e.torsoPitch, 0.0f, 1.0f);
    e.headClearM = std::clamp(e.headClearM, 0.0f, 0.5f);
  }

  // ---- combat feel: hit-stop, hit flash, combat cues ------------------------
  if (const json* g = Find(j, "combatfx")) {
    auto& e = out.combatfx;
    const std::string at = "combatfx";
    ReadB(*g, "hitStop", e.hitStop, out, at);
    ReadF(*g, "hitStopChipScale", e.hitStopChipScale, out, at);
    ReadF(*g, "hitStopChipMs", e.hitStopChipMs, out, at);
    ReadF(*g, "hitStopFleshScale", e.hitStopFleshScale, out, at);
    ReadF(*g, "hitStopFleshMs", e.hitStopFleshMs, out, at);
    ReadF(*g, "hitStopSeverScale", e.hitStopSeverScale, out, at);
    ReadF(*g, "hitStopSeverMs", e.hitStopSeverMs, out, at);
    ReadF(*g, "flashChip", e.flashChip, out, at);
    ReadF(*g, "flashFlesh", e.flashFlesh, out, at);
    ReadF(*g, "flashSever", e.flashSever, out, at);
    ReadF(*g, "flashHalflife", e.flashHalflife, out, at);
    ReadF(*g, "whooshVolume", e.whooshVolume, out, at);
    ReadF(*g, "whooshMinSpeed", e.whooshMinSpeed, out, at);
    ReadF(*g, "whooshRateSlow", e.whooshRateSlow, out, at);
    ReadF(*g, "whooshRateFast", e.whooshRateFast, out, at);
    ReadF(*g, "fleshVolume", e.fleshVolume, out, at);
    ReadF(*g, "clangVolume", e.clangVolume, out, at);
    ReadF(*g, "cueRadius", e.cueRadius, out, at);

    // ---- BOUNDS ---------------------------------------------------------------
    // THE DIP MAY NOT REACH ZERO AND MAY NOT LAST. A scale of 0 stops the tick
    // accumulator filling at all, which is a hang the player cannot get out of
    // by any input, since every input is consumed inside the tick loop. A
    // duration in whole seconds is the same hang with a timer on it. 0.02 and
    // 400 ms are both well past anything that reads as impact and comfortably
    // short of anything that reads as broken.
    e.hitStopChipScale = std::clamp(e.hitStopChipScale, 0.02f, 1.0f);
    e.hitStopFleshScale = std::clamp(e.hitStopFleshScale, 0.02f, 1.0f);
    e.hitStopSeverScale = std::clamp(e.hitStopSeverScale, 0.02f, 1.0f);
    e.hitStopChipMs = std::clamp(e.hitStopChipMs, 0.0f, 400.0f);
    e.hitStopFleshMs = std::clamp(e.hitStopFleshMs, 0.0f, 400.0f);
    e.hitStopSeverMs = std::clamp(e.hitStopSeverMs, 0.0f, 400.0f);
    // The flash is added to a LINEAR HDR colour before the tonemap. Past about
    // 4 the tonemap has nothing left to give and every tier looks identical, so
    // the ceiling is where the knob stops doing anything rather than where it
    // starts looking bad.
    e.flashChip = std::clamp(e.flashChip, 0.0f, 4.0f);
    e.flashFlesh = std::clamp(e.flashFlesh, 0.0f, 4.0f);
    e.flashSever = std::clamp(e.flashSever, 0.0f, 4.0f);
    // A zero halflife would never decay (the per-frame factor is
    // exp2(-dt/halflife), which is 0/0 at 0), leaving every struck limb lit for
    // the rest of the session.
    e.flashHalflife = std::clamp(e.flashHalflife, 0.01f, 2.0f);
    e.whooshVolume = std::clamp(e.whooshVolume, 0.0f, 4.0f);
    e.whooshMinSpeed = std::max(e.whooshMinSpeed, 0.0f);
    e.whooshRateSlow = std::clamp(e.whooshRateSlow, 0.25f, 4.0f);
    e.whooshRateFast = std::clamp(e.whooshRateFast, 0.25f, 4.0f);
    e.fleshVolume = std::clamp(e.fleshVolume, 0.0f, 4.0f);
    e.clangVolume = std::clamp(e.clangVolume, 0.0f, 4.0f);
    e.cueRadius = std::clamp(e.cueRadius, 1.0f, 400.0f);
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
    ReadF(*g, "laserCarveRadius", t.laserCarveRadius, out, at);
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
    ReadI(*g, "liquidMinFilm", s.liquidMinFilm, out, at);
    ReadI(*g, "wanderHopMask", s.wanderHopMask, out, at);
    ReadI(*g, "expMicroPerMille", s.expMicroPerMille, out, at);
    ReadI(*g, "expMicroLifeTicks", s.expMicroLifeTicks, out, at);
    ReadI(*g, "expMicroScaleIdx", s.expMicroScaleIdx, out, at);
    ReadI(*g, "fluidSubsteps", s.fluidSubsteps, out, at);
    ReadF(*g, "fluidStiffness", s.fluidStiffness, out, at);
    ReadF(*g, "fluidGravity", s.fluidGravity, out, at);
    ReadF(*g, "fluidRestDensity", s.fluidRestDensity, out, at);
    ReadI(*g, "fluidEosPower", s.fluidEosPower, out, at);
    ReadF(*g, "fluidCohesion", s.fluidCohesion, out, at);
    ReadF(*g, "fluidAttractSame", s.fluidAttractSame, out, at);
    ReadF(*g, "fluidAttractDiff", s.fluidAttractDiff, out, at);
    ReadF(*g, "fluidViscosity", s.fluidViscosity, out, at);
    ReadF(*g, "fluidDamping", s.fluidDamping, out, at);
    ReadF(*g, "fluidFriction", s.fluidFriction, out, at);
    ReadF(*g, "fluidSplashRate", s.fluidSplashRate, out, at);
    ReadF(*g, "fluidSplashSpeed", s.fluidSplashSpeed, out, at);
    ReadF(*g, "fluidSplashMaxDensity", s.fluidSplashMaxDensity, out, at);
    ReadF(*g, "fluidSplashLife", s.fluidSplashLife, out, at);
    ReadI(*g, "fluidSplashScaleIdx", s.fluidSplashScaleIdx, out, at);
    ReadF(*g, "fluidFoamRate", s.fluidFoamRate, out, at);
    ReadF(*g, "fluidFoamCrestRate", s.fluidFoamCrestRate, out, at);
    ReadF(*g, "fluidTrappedMin", s.fluidTrappedMin, out, at);
    ReadF(*g, "fluidTrappedMax", s.fluidTrappedMax, out, at);
    ReadF(*g, "fluidCrestMin", s.fluidCrestMin, out, at);
    ReadF(*g, "fluidCrestMax", s.fluidCrestMax, out, at);
    ReadF(*g, "fluidFoamEnergyMin", s.fluidFoamEnergyMin, out, at);
    ReadF(*g, "fluidFoamEnergyMax", s.fluidFoamEnergyMax, out, at);
    ReadF(*g, "fluidFoamLife", s.fluidFoamLife, out, at);
    ReadF(*g, "fluidFoamLifeMin", s.fluidFoamLifeMin, out, at);
    ReadF(*g, "fluidBubbleBuoyancy", s.fluidBubbleBuoyancy, out, at);
    ReadF(*g, "fluidFoamDrag", s.fluidFoamDrag, out, at);
    ReadF(*g, "fluidBubbleDensity", s.fluidBubbleDensity, out, at);
    ReadF(*g, "fluidSprayDensity", s.fluidSprayDensity, out, at);
    ReadI(*g, "fluidFoamScaleIdx", s.fluidFoamScaleIdx, out, at);
    ReadI(*g, "fluidExciteMode", s.fluidExciteMode, out, at);
    ReadI(*g, "fluidExciteCeiling", s.fluidExciteCeiling, out, at);
    ReadI(*g, "fluidExciteRate", s.fluidExciteRate, out, at);
    ReadI(*g, "fluidExcitePerch", s.fluidExcitePerch, out, at);
    ReadI(*g, "fluidExciteStep", s.fluidExciteStep, out, at);
    ReadF(*g, "fluidSettledMass", s.fluidSettledMass, out, at);
    ReadF(*g, "fluidSettleEps", s.fluidSettleEps, out, at);
    ReadF(*g, "fluidWakeSpeed", s.fluidWakeSpeed, out, at);
    ReadI(*g, "fluidSettleTicks", s.fluidSettleTicks, out, at);
    ReadF(*g, "fluidStainRate", s.fluidStainRate, out, at);
    ReadI(*g, "waterBodyMode", s.waterBodyMode, out, at);
    ReadI(*g, "waterBodyMinVolume", s.waterBodyMinVolume, out, at);
    ReadI(*g, "waterBodyExitVolume", s.waterBodyExitVolume, out, at);
    ReadI(*g, "waterBodySpreadEnter", s.waterBodySpreadEnter, out, at);
    ReadI(*g, "waterBodySpreadExit", s.waterBodySpreadExit, out, at);
    ReadI(*g, "waterBodyQuietTicks", s.waterBodyQuietTicks, out, at);
    ReadI(*g, "waterBodyMaxCount", s.waterBodyMaxCount, out, at);
    ReadI(*g, "waterBodyTestDrain", s.waterBodyTestDrain, out, at);
    ReadI(*g, "drainMaxEighthsPerTick", s.drainMaxEighthsPerTick, out, at);
    ReadI(*g, "drainExciteRadius", s.drainExciteRadius, out, at);
    ReadF(*g, "drainCd", s.drainCd, out, at);
    ReadF(*g, "drainGravity", s.drainGravity, out, at);
    ReadI(*g, "windMode", s.windMode, out, at);
    ReadF(*g, "windDrag", s.windDrag, out, at);
    ReadF(*g, "windFluidGain", s.windFluidGain, out, at);
    ReadF(*g, "windFluidMass", s.windFluidMass, out, at);
    ReadF(*g, "windDriftSpeed", s.windDriftSpeed, out, at);
    ReadF(*g, "windDriftMax", s.windDriftMax, out, at);
    ReadF(*g, "windEntrainSpeed", s.windEntrainSpeed, out, at);
    ReadF(*g, "windEntrainRate", s.windEntrainRate, out, at);
    ReadF(*g, "windGasScale", s.windGasScale, out, at);
    ReadF(*g, "windPartScale", s.windPartScale, out, at);
    ReadF(*g, "windDragRef", s.windDragRef, out, at);
    ReadI(*g, "windWakeChunks", s.windWakeChunks, out, at);
    // The current field (plan component 8). CPU-side by design —
    // the shader reads resolved primitives, never a knob.
    ReadI(*g, "currentMode", s.currentMode, out, at);
    ReadF(*g, "currentVortexGamma", s.currentVortexGamma, out, at);
    ReadF(*g, "currentVortexDecay", s.currentVortexDecay, out, at);
    ReadI(*g, "currentVortexRadius", s.currentVortexRadius, out, at);
    ReadF(*g, "currentSinkSpeed", s.currentSinkSpeed, out, at);
    ReadF(*g, "currentStreamScale", s.currentStreamScale, out, at);
    ReadI(*g, "currentStreamMinSlope", s.currentStreamMinSlope, out, at);
    ReadF(*g, "currentDrag", s.currentDrag, out, at);
    // MLS-MPM fluid: HUMAN units in the JSON (voxels/s², (vox/s)², vox²/s,
    // seconds), converted to Q16.16-per-tick at shader compile time
    // (sim_fluid.wgsl's const block). These clamps keep the CONVERTED values
    // inside the ranges the kernel's i32 overflow audit assumes — the human
    // bound is the fixed-point bound expressed in the human unit (x900 for
    // per-tick² quantities, x30 for per-tick). Out-of-range values would wrap
    // i32 mid-kernel, not merely look wrong, so clamp rather than trust the
    // file.
    auto clampWarnF = [&](float& v, float lo, float hi, const char* name) {
      if (!(v >= lo) || v > hi) {  // !(>=) also catches NaN
        out.warnings.push_back(std::string("sim.") + name + " out of range; clamped");
        v = !(v >= lo) ? lo : hi;
      }
    };
    // Substeps: the CFL budget and the solver's price. The 32 ceiling is the
    // pass-table/timer sanity bound (the substep table is recorded this many
    // times per tick); the 1 floor keeps every /FLUID_SUBSTEPS in the kernels
    // legal. Not warned against stiffness here — an under-budgeted stiffness
    // is legal, just clamped (FA_CLAMPED is the probe that says so).
    if (s.fluidSubsteps < 1 || s.fluidSubsteps > 32) {
      out.warnings.push_back("sim.fluidSubsteps out of 1..32; clamped");
      s.fluidSubsteps = s.fluidSubsteps < 1 ? 1 : 32;
    }
    clampWarnF(s.fluidStiffness, 0.0f, 43200.0f, "fluidStiffness");   // 48 c²/t²
    clampWarnF(s.fluidGravity, 0.0f, 1800.0f, "fluidGravity");        // 2 c/t²
    clampWarnF(s.fluidRestDensity, 1.0f, 32.0f, "fluidRestDensity");
    if (s.fluidEosPower < 1 || s.fluidEosPower > 7) {
      out.warnings.push_back("sim.fluidEosPower out of 1..7; clamped");
      s.fluidEosPower = s.fluidEosPower < 1 ? 1 : 7;
    }
    clampWarnF(s.fluidCohesion, 0.0f, 14400.0f, "fluidCohesion");     // 16 c²/t²
    clampWarnF(s.fluidAttractSame, -7200.0f, 7200.0f, "fluidAttractSame");
    clampWarnF(s.fluidAttractDiff, -7200.0f, 7200.0f, "fluidAttractDiff");
    clampWarnF(s.fluidViscosity, 0.0f, 240.0f, "fluidViscosity");     // 8 c²/t
    clampWarnF(s.fluidDamping, 0.0f, 20.0f, "fluidDamping");          // <0.9/tick
    clampWarnF(s.fluidFriction, 0.0f, 20.0f, "fluidFriction");        // <0.9/tick
    clampWarnF(s.fluidSplashRate, 0.0f, 180.0f, "fluidSplashRate");   // <=1/substep
    clampWarnF(s.fluidSplashSpeed, 0.0f, 90.0f, "fluidSplashSpeed");  // < VMAX
    clampWarnF(s.fluidSplashMaxDensity, 0.0f, 4.0f, "fluidSplashMaxDensity");
    clampWarnF(s.fluidSplashLife, 0.05f, 8.5f, "fluidSplashLife");    // 255 ticks
    if (s.fluidSplashScaleIdx < 0 || s.fluidSplashScaleIdx > 3) {
      out.warnings.push_back("sim.fluidSplashScaleIdx out of 0..3; clamped");
      s.fluidSplashScaleIdx = s.fluidSplashScaleIdx < 0 ? 0 : 3;
    }
    // Diffuse material (spray/foam/bubbles). Same discipline as the splash
    // rows: these become Q16.16-per-tick constants and bit fields at shader
    // compile time, so an out-of-range value wraps i32 or bleeds into the
    // neighbouring flag field rather than merely looking wrong.
    clampWarnF(s.fluidFoamRate, 0.0f, 180.0f, "fluidFoamRate");   // <=1/substep
    clampWarnF(s.fluidFoamCrestRate, 0.0f, 180.0f, "fluidFoamCrestRate");
    clampWarnF(s.fluidTrappedMin, 0.0f, 400.0f, "fluidTrappedMin");
    clampWarnF(s.fluidTrappedMax, 0.0f, 400.0f, "fluidTrappedMax");
    clampWarnF(s.fluidCrestMin, 0.0f, 64.0f, "fluidCrestMin");
    clampWarnF(s.fluidCrestMax, 0.0f, 64.0f, "fluidCrestMax");
    clampWarnF(s.fluidFoamEnergyMin, 0.0f, 8100.0f, "fluidFoamEnergyMin");
    clampWarnF(s.fluidFoamEnergyMax, 0.0f, 8100.0f, "fluidFoamEnergyMax");
    clampWarnF(s.fluidFoamLife, 0.05f, 8.5f, "fluidFoamLife");     // 255 ticks
    clampWarnF(s.fluidFoamLifeMin, 0.05f, 8.5f, "fluidFoamLifeMin");
    clampWarnF(s.fluidBubbleBuoyancy, -4.0f, 8.0f, "fluidBubbleBuoyancy");
    clampWarnF(s.fluidFoamDrag, 0.0f, 1.0f, "fluidFoamDrag");      // kd in 0..1
    clampWarnF(s.fluidBubbleDensity, 0.0f, 4.0f, "fluidBubbleDensity");
    clampWarnF(s.fluidSprayDensity, 0.0f, 4.0f, "fluidSprayDensity");
    // Phi(I, tmin, tmax) divides by (tmax - tmin): an inverted or degenerate
    // pair is a divide-by-zero in a const-eval expression, which is a SHADER
    // COMPILE failure, not a bad-looking frame. Order them here instead.
    auto orderPair = [&](float& lo, float& hi, float minGap, const char* name) {
      if (!(hi > lo + minGap)) {
        out.warnings.push_back(std::string("sim.") + name +
                               " max must exceed min; raised");
        hi = lo + minGap;
      }
    };
    orderPair(s.fluidTrappedMin, s.fluidTrappedMax, 0.5f, "fluidTrapped");
    orderPair(s.fluidCrestMin, s.fluidCrestMax, 0.05f, "fluidCrest");
    orderPair(s.fluidFoamEnergyMin, s.fluidFoamEnergyMax, 1.0f, "fluidFoamEnergy");
    // Foam lifetime is scaled between min and max by the generation potential
    // (the paper's "large clusters are more stable" rule), so min <= max.
    if (s.fluidFoamLifeMin > s.fluidFoamLife) {
      out.warnings.push_back("sim.fluidFoamLifeMin above fluidFoamLife; clamped");
      s.fluidFoamLifeMin = s.fluidFoamLife;
    }
    if (s.fluidFoamScaleIdx < 0 || s.fluidFoamScaleIdx > 3) {
      out.warnings.push_back("sim.fluidFoamScaleIdx out of 0..3; clamped");
      s.fluidFoamScaleIdx = s.fluidFoamScaleIdx < 0 ? 0 : 3;
    }
    // Settle/excite seam. Floats through the same clampWarnF discipline; the
    // int windows are open-coded like fluidEosPower. The settleTicks floor of
    // 8 is a HARD requirement — it covers the CPU-side page-materialization
    // readback latency (see tuning.h), so it clamps rather than warns-only.
    if (s.fluidExciteMode < 0 || s.fluidExciteMode > 1) {
      out.warnings.push_back("sim.fluidExciteMode out of 0..1; clamped");
      s.fluidExciteMode = s.fluidExciteMode < 0 ? 0 : 1;
    }
    // The burst bound. 256 is the floor rather than 0 because a ceiling of
    // zero would silently turn excite off entirely — that is what
    // fluidExciteMode is for, and two ways to spell "off" is one too many.
    // The upper bound is the pool: a ceiling above kFluidCap is not a bound.
    if (s.fluidExciteCeiling < 256 || s.fluidExciteCeiling > 262144) {
      out.warnings.push_back("sim.fluidExciteCeiling out of 256..262144; "
                             "clamped");
      s.fluidExciteCeiling = s.fluidExciteCeiling < 256 ? 256 : 262144;
    }
    if (s.fluidExciteRate < 64 || s.fluidExciteRate > 262144) {
      out.warnings.push_back("sim.fluidExciteRate out of 64..262144; clamped");
      s.fluidExciteRate = s.fluidExciteRate < 64 ? 64 : 262144;
    }
    if (s.fluidExcitePerch < 0 || s.fluidExcitePerch > 1) {
      out.warnings.push_back("sim.fluidExcitePerch out of 0..1; clamped");
      s.fluidExcitePerch = s.fluidExcitePerch < 0 ? 0 : 1;
    }
    // ---- the current field (plan component 8) ----
    // The bounds are the shader's overflow audit expressed in human units:
    // kCurrentPrimMaxSpeed is 200 cells/s = 20 m/s, and kCurrentPrimMaxExtent
    // is what every intermediate in currentPrimEvalQ is bounded against.
    if (s.currentMode < 0 || s.currentMode > 1) {
      out.warnings.push_back("sim.currentMode out of 0..1; clamped");
      s.currentMode = s.currentMode < 0 ? 0 : 1;
    }
    clampWarnF(s.currentVortexGamma, 0.0f, 400.0f, "currentVortexGamma");
    // A decay of zero is the bug plan component 8 names outright — a funnel
    // standing open in still water — so the floor is a real number, not 0.
    clampWarnF(s.currentVortexDecay, 0.05f, 120.0f, "currentVortexDecay");
    clampWarnF(s.currentSinkSpeed, 0.0f, 20.0f, "currentSinkSpeed");
    clampWarnF(s.currentStreamScale, 0.0f, 20.0f, "currentStreamScale");
    clampWarnF(s.currentDrag, 0.0f, 30.0f, "currentDrag");
    if (s.currentVortexRadius < 4 ||
        s.currentVortexRadius > kCurrentPrimMaxExtent) {
      out.warnings.push_back("sim.currentVortexRadius out of 4..512; clamped");
      s.currentVortexRadius =
          s.currentVortexRadius < 4 ? 4 : kCurrentPrimMaxExtent;
    }
    if (s.currentStreamMinSlope < 0 || s.currentStreamMinSlope > 4096) {
      out.warnings.push_back("sim.currentStreamMinSlope out of 0..4096; clamped");
      s.currentStreamMinSlope = s.currentStreamMinSlope < 0 ? 0 : 4096;
    }
    // ---- water bodies (docs/PLAN_water_master.md) ----
    if (s.waterBodyMode < 0 || s.waterBodyMode > 1) {
      out.warnings.push_back("sim.waterBodyMode out of 0..1; clamped");
      s.waterBodyMode = s.waterBodyMode < 0 ? 0 : 1;
    }
    if (s.waterBodyMinVolume < 0) {
      out.warnings.push_back("sim.waterBodyMinVolume negative; clamped to 0");
      s.waterBodyMinVolume = 0;
    }
    if (s.waterBodyExitVolume < 0) {
      out.warnings.push_back("sim.waterBodyExitVolume negative; clamped to 0");
      s.waterBodyExitVolume = 0;
    }
    // HYSTERESIS IS NOT OPTIONAL, so it is enforced here rather than trusted to
    // the author. An exit threshold at or above the enter threshold is not a
    // narrow gap, it is a body that adopts and releases on alternating ticks —
    // and every one of those transitions is a seam crossing where mass can be
    // lost (PLAN_water_master.md §5). Halving is the shipped ratio.
    if (s.waterBodyExitVolume >= s.waterBodyMinVolume &&
        s.waterBodyMinVolume > 0) {
      out.warnings.push_back(
          "sim.waterBodyExitVolume must be BELOW sim.waterBodyMinVolume "
          "(hysteresis); set to half the enter threshold");
      s.waterBodyExitVolume = s.waterBodyMinVolume / 2;
    }
    if (s.waterBodySpreadEnter < 0 || s.waterBodySpreadEnter > 64) {
      out.warnings.push_back("sim.waterBodySpreadEnter out of 0..64; clamped");
      s.waterBodySpreadEnter = s.waterBodySpreadEnter < 0 ? 0 : 64;
    }
    if (s.waterBodySpreadExit <= s.waterBodySpreadEnter) {
      out.warnings.push_back(
          "sim.waterBodySpreadExit must be ABOVE sim.waterBodySpreadEnter "
          "(hysteresis); set to enter + 3");
      s.waterBodySpreadExit = s.waterBodySpreadEnter + 3;
    }
    if (s.waterBodyQuietTicks < 0 || s.waterBodyQuietTicks > 3600) {
      out.warnings.push_back("sim.waterBodyQuietTicks out of 0..3600; clamped");
      s.waterBodyQuietTicks = s.waterBodyQuietTicks < 0 ? 0 : 3600;
    }
    // kWaterBodyCap (sim/waterbody.h) is the hard ceiling: from M2 the
    // descriptor array is a GPU layout, and a count past its end is not a
    // tuning mistake, it is an out-of-bounds write.
    if (s.waterBodyMaxCount < 0 || s.waterBodyMaxCount > (int)kWaterBodyCap) {
      out.warnings.push_back("sim.waterBodyMaxCount out of 0..64; clamped");
      s.waterBodyMaxCount =
          s.waterBodyMaxCount < 0 ? 0 : (int)kWaterBodyCap;
    }
    // 8 is the seam's own surface scan depth (SEAM_EX_STEP_SCAN); a step it
    // cannot see is a trigger that never fires, so the knob stops there.
    if (s.fluidExciteStep < 0 || s.fluidExciteStep > 8) {
      out.warnings.push_back("sim.fluidExciteStep out of 0..8 (0 = off); "
                             "clamped");
      s.fluidExciteStep = s.fluidExciteStep < 0 ? 0 : 8;
    }
    // The WGSL side folds this to a Q8 scale (sim_fluid.wgsl
    // FLUID_SETTLED_Q8); past ~2 the static boundary out-pressures the fluid
    // and fires particles off a still pool.
    clampWarnF(s.fluidSettledMass, 0.0f, 2.0f, "fluidSettledMass");
    clampWarnF(s.fluidSettleEps, 0.05f, 20.0f, "fluidSettleEps");
    clampWarnF(s.fluidWakeSpeed, 0.1f, 50.0f, "fluidWakeSpeed");
    if (s.fluidSettleTicks < 8 || s.fluidSettleTicks > 600) {
      out.warnings.push_back("sim.fluidSettleTicks out of 8..600; clamped");
      s.fluidSettleTicks = s.fluidSettleTicks < 8 ? 8 : 600;
    }
    clampWarnF(s.fluidStainRate, 0.0f, 30.0f, "fluidStainRate");
    // ---- the discharge law (component 6) -------------------------------
    // The rate cap is the rule-2 bound on the jet AND the size of the spawn-op
    // block the CPU reserves, so it is clamped to kWaterDrainOpsPerBody here
    // rather than in the kernel: an emission the block cannot hold would be a
    // debit with no particle behind it, which is exactly the mass pump plan §6
    // forbids. 0 turns the discharge off with no other effect.
    if (s.drainMaxEighthsPerTick < 0 ||
        s.drainMaxEighthsPerTick > (int)kWaterDrainOpsPerBody) {
      out.warnings.push_back("sim.drainMaxEighthsPerTick out of 0..512; clamped");
      s.drainMaxEighthsPerTick =
          s.drainMaxEighthsPerTick < 0 ? 0 : (int)kWaterDrainOpsPerBody;
    }
    // Component 7's shell radius. Bounded because the shell is a DISC of cells
    // at the free surface plus a column: at r the surface term is ~pi*r^2 cells
    // of up to 8 eighths each, and plan §9 item 2 is that a violent drain's
    // solid-ball excite is ~33,000 particles against a ~40,000 envelope. 32
    // caps the disc at ~3,200 cells, which the 8,000 excite ceiling refuses
    // gracefully rather than converting the pool.
    if (s.drainExciteRadius < 0 || s.drainExciteRadius > 32) {
      out.warnings.push_back("sim.drainExciteRadius out of 0..32; clamped");
      s.drainExciteRadius = s.drainExciteRadius < 0 ? 0 : 32;
    }
    // Cd above 1 is not a discharge coefficient, it is a mass source. The
    // gravity bound is the fluid lane's, for the same fixed-point reason.
    clampWarnF(s.drainCd, 0.0f, 1.0f, "drainCd");
    clampWarnF(s.drainGravity, 0.0f, 4000.0f, "drainGravity");
    // Wind coupling. The gate first: an unknown mode must not fall through to
    // "some wind", because the whole hash argument for shipping this is that
    // mode 0 means literally no kernel reads the field.
    if (s.windMode < (int)kWindModeOff || s.windMode > (int)kWindModeEntrain) {
      out.warnings.push_back("sim.windMode out of 0..2; clamped");
      s.windMode = s.windMode < (int)kWindModeOff ? (int)kWindModeOff
                                                  : (int)kWindModeEntrain;
    }
    // Mode 2 is legal and it works — a bed of sand really does creep downwind
    // — but it has two known defects that both trace to the same missing
    // piece, and shipping a knob whose caveats live only in a design doc is
    // how a caveat gets lost. See kWindModeEntrain in world.h.
    if (s.windMode >= (int)kWindModeEntrain) {
      out.warnings.push_back(
          "sim.windMode 2 (entrainment) is EXPERIMENTAL: an exposed dune never "
          "sleeps (rule 2), and moving settled powder writes into chunks the "
          "page table was told would stay untouched (page faults = lost "
          "voxels). Both need phase 2's wind primitives, which put the wind's "
          "footprint on the mutation path where the CPU can see it.");
    }
    // The rest are const-eval'd into integers by the kernels that read them
    // (sim_particle, sim_fluid, sim_step), so these bounds are the fixed-point
    // bounds written in the human unit — the same discipline as the fluid rows
    // above, and for the same reason: an out-of-range value wraps an i32
    // mid-kernel rather than merely looking wrong.
    clampWarnF(s.windDrag, 0.0f, 30.0f, "windDrag");
    clampWarnF(s.windFluidGain, 0.0f, 4.0f, "windFluidGain");
    // The floor is not 0: a zero mass band would make the taper divide by
    // nothing, and "no node ever feels wind" is what windFluidGain 0 is for.
    clampWarnF(s.windFluidMass, 0.02f, 4.0f, "windFluidMass");
    // A drift reference speed of 0 would make every breath of air a full-bias
    // gale, so the floor is a real wind rather than a nominal epsilon.
    clampWarnF(s.windDriftSpeed, 0.5f, 200.0f, "windDriftSpeed");
    // Capped below 1: at 1.0 a moving voxel ALWAYS tries downwind first, which
    // removes the RNG order entirely and turns a gas into a conveyor belt.
    clampWarnF(s.windDriftMax, 0.0f, 0.95f, "windDriftMax");
    clampWarnF(s.windEntrainSpeed, 0.5f, 200.0f, "windEntrainSpeed");
    // Rate, not certainty (rule 2). 30/s is one hop per tick, which is the
    // most the substep gate can deliver anyway.
    clampWarnF(s.windEntrainRate, 0.0f, 30.0f, "windEntrainRate");
    // Dev force multipliers. The ceiling is the one the Q8 arithmetic in the
    // kernels is sized for (kWindScaleMax); 0 is a legal "pin this tier still"
    // and is genuinely useful, since it isolates one tier from the other.
    const float kScaleMax = (float)kWindScaleMax / (float)kWindScaleOne;
    clampWarnF(s.windGasScale, 0.0f, kScaleMax, "windGasScale");
    clampWarnF(s.windPartScale, 0.0f, kScaleMax, "windPartScale");
    // The drag ramp reference. Floored well above zero because the kernel
    // divides by it: windDragRampQ's `max(refQ >> 16, 1)` keeps the shader
    // safe whatever arrives, but a reference of 0.1 m/s would saturate the
    // ramp in dead calm and quietly restore the fixed-rate behaviour this
    // replaces — which is a bug report about gravity, not about wind.
    clampWarnF(s.windDragRef, 1.0f, 200.0f, "windDragRef");
    // The wake budget cannot exceed the TickParams array it is copied into;
    // 0 is a legal "no primitive may wake anything", which is the switch that
    // turns entrainment off without turning the fans off.
    if (s.windWakeChunks < 0 || s.windWakeChunks > (int)kWindWakeCap) {
      out.warnings.push_back("sim.windWakeChunks out of 0..128; clamped");
      s.windWakeChunks = s.windWakeChunks < 0 ? 0 : (int)kWindWakeCap;
    }
    // Both of these are packed into bit fields in Particle.flags; an
    // out-of-range value would wrap into the neighbouring field rather than
    // merely looking wrong, so clamp instead of trusting the file.
    if (s.expMicroLifeTicks < 1 || s.expMicroLifeTicks > 255) {
      out.warnings.push_back("sim.expMicroLifeTicks out of 1..255; clamped");
      s.expMicroLifeTicks = s.expMicroLifeTicks < 1 ? 1 : 255;
    }
    if (s.expMicroScaleIdx < 0 || s.expMicroScaleIdx > 3) {
      out.warnings.push_back("sim.expMicroScaleIdx out of 0..3; clamped");
      s.expMicroScaleIdx = s.expMicroScaleIdx < 0 ? 0 : 3;
    }
    if (s.expMicroPerMille < 0 || s.expMicroPerMille > 1000) {
      out.warnings.push_back("sim.expMicroPerMille out of 0..1000; clamped");
      s.expMicroPerMille = s.expMicroPerMille < 0 ? 0 : 1000;
    }
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
    // 1 is "no floor" — the pre-WP5 halving, kept reachable as the A/B oracle.
    // The upper bound is 4 because the rule needs f >= 2*minFilm to split at
    // all, and a cell holds 8: past 4 no cell could ever spread into air and
    // water would stop advancing across a flat floor entirely.
    if (s.liquidMinFilm < 1 || s.liquidMinFilm > 4) {
      out.warnings.push_back(
          "sim.liquidMinFilm outside 1..4 (4 is the largest floor a cell can "
          "split and still leave both halves at it); clamped");
      s.liquidMinFilm = s.liquidMinFilm < 1 ? 1 : 4;
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

  if (const json* g = Find(j, "dayNight")) {
    auto& d = out.dayNight;
    const std::string at = "dayNight";
    ReadI(*g, "cycleMinutes", d.cycleMinutes, out, at);
    ReadI(*g, "freeze", d.freeze, out, at);
    ReadI(*g, "freezePhase", d.freezePhase, out, at);
    ReadF(*g, "sunPeakElevation", d.sunPeakElevation, out, at);  // legacy, unread
    ReadF(*g, "sunAzimuth", d.sunAzimuth, out, at);
    ReadF(*g, "twilightWidth", d.twilightWidth, out, at);
    ReadF(*g, "starRotSpeed", d.starRotSpeed, out, at);
    // ---- orbital elements (sim/celestial.cpp) ----
    ReadF(*g, "axialTilt", d.axialTilt, out, at);
    ReadF(*g, "latitudeDeg", d.latitudeDeg, out, at);
    ReadF(*g, "yearLengthDays", d.yearLengthDays, out, at);
    ReadF(*g, "orbitEccentricity", d.orbitEccentricity, out, at);
    ReadF(*g, "orbitArgPeriapsis", d.orbitArgPeriapsis, out, at);
    ReadF(*g, "orbitMeanAnomaly0", d.orbitMeanAnomaly0, out, at);
    ReadF(*g, "sunAngularRadius", d.sunAngularRadius, out, at);
    ReadI(*g, "lunarPeriodDays", d.lunarPeriodDays, out, at);
    ReadF(*g, "moonInclination", d.moonInclination, out, at);
    ReadF(*g, "moonEccentricity", d.moonEccentricity, out, at);
    ReadF(*g, "moonArgPeriapsis", d.moonArgPeriapsis, out, at);
    ReadF(*g, "moonNode", d.moonNode, out, at);
    ReadF(*g, "moonMeanAnomaly0", d.moonMeanAnomaly0, out, at);
    ReadF(*g, "moonAngularRadius", d.moonAngularRadius, out, at);
    ReadI(*g, "moon2PeriodDays", d.moon2PeriodDays, out, at);
    ReadF(*g, "moon2Inclination", d.moon2Inclination, out, at);
    ReadF(*g, "moon2Eccentricity", d.moon2Eccentricity, out, at);
    ReadF(*g, "moon2ArgPeriapsis", d.moon2ArgPeriapsis, out, at);
    ReadF(*g, "moon2Node", d.moon2Node, out, at);
    ReadF(*g, "moon2MeanAnomaly0", d.moon2MeanAnomaly0, out, at);
    ReadF(*g, "moon2AngularRadius", d.moon2AngularRadius, out, at);
    // A zero-length day divides by zero in DayPhaseForTick and would freeze
    // the cycle at midnight; a negative one is meaningless.
    if (d.cycleMinutes < 1) {
      out.warnings.push_back("dayNight.cycleMinutes < 1; clamped to 1");
      d.cycleMinutes = 1;
    }
    if (d.lunarPeriodDays < 1) {
      out.warnings.push_back("dayNight.lunarPeriodDays < 1; clamped to 1");
      d.lunarPeriodDays = 1;
    }
    if (d.moon2PeriodDays < 1) {
      out.warnings.push_back("dayNight.moon2PeriodDays < 1; clamped to 1");
      d.moon2PeriodDays = 1;
    }
    if (d.freezePhase < 0 || d.freezePhase > 65535) {
      out.warnings.push_back("dayNight.freezePhase outside 0..65535; wrapped");
      d.freezePhase = ((d.freezePhase % 65536) + 65536) % 65536;
    }
    if (d.twilightWidth <= 0.0f) {
      out.warnings.push_back("dayNight.twilightWidth must be > 0; reset to 0.22");
      d.twilightWidth = 0.22f;
    }
    // ---- orbital sanity ----
    // Kepler's equation is only solved for bound elliptical orbits, and the
    // fixed-iteration Newton solver (celestial.cpp) is accurate well past 0.6
    // but not near 1. A parabolic or hyperbolic element set here is a JSON
    // typo, not a design choice, so clamp loudly rather than emit a sky that
    // flies off.
    auto clampEcc = [&](const char* name, float& e) {
      if (e >= 0.0f && e <= 0.7f) return;
      out.warnings.push_back(std::string("dayNight.") + name +
                             " outside 0..0.7; clamped (orbits must stay bound)");
      e = e < 0.0f ? 0.0f : 0.7f;
    };
    clampEcc("orbitEccentricity", d.orbitEccentricity);
    clampEcc("moonEccentricity", d.moonEccentricity);
    clampEcc("moon2Eccentricity", d.moon2Eccentricity);
    if (d.yearLengthDays < 0.5f) {
      out.warnings.push_back("dayNight.yearLengthDays < 0.5; clamped to 0.5");
      d.yearLengthDays = 0.5f;
    }
    // The apparent radii feed the eclipse geometry directly. Zero would make
    // eclipses impossible and a huge value would make one permanent, so bound
    // both to something a sky can hold.
    auto clampAng = [&](const char* name, float& r, float lo, float hi) {
      if (r >= lo && r <= hi) return;
      out.warnings.push_back(std::string("dayNight.") + name +
                             " outside range; clamped");
      r = r < lo ? lo : hi;
    };
    clampAng("sunAngularRadius", d.sunAngularRadius, 0.02f, 12.0f);
    clampAng("moonAngularRadius", d.moonAngularRadius, 0.05f, 20.0f);
    clampAng("moon2AngularRadius", d.moon2AngularRadius, 0.05f, 20.0f);
    // Latitude past the poles is a wrapped angle, not an error the user meant.
    if (d.latitudeDeg < -89.0f || d.latitudeDeg > 89.0f) {
      out.warnings.push_back("dayNight.latitudeDeg outside +-89; clamped");
      d.latitudeDeg = d.latitudeDeg < 0.0f ? -89.0f : 89.0f;
    }
  }

  if (const json* g = Find(j, "weather")) {
    auto& w = out.weather;
    const std::string at = "weather";
    ReadB(*g, "waterFreezes", w.waterFreezes, out, at);
    ReadB(*g, "iceMelts", w.iceMelts, out, at);
  }

  // ---- combustion: read by the REACTION COMPILER, not by a kernel ----
  if (const json* g = Find(j, "combustion")) {
    auto& cb = out.combustion;
    const std::string at = "combustion";
    ReadI(*g, "burnDurationPct", cb.burnDurationPct, out, at);
    // Clamped rather than validated away, because the failure at the bottom is
    // silent and total: at 0 the divide would send every retire rule's chance
    // to the 1-unit floor materials.cpp applies, i.e. "a lit voxel effectively
    // never goes out", which is rule 2 broken by a typo. 25% is a quarter of
    // the authored duration and is already very fast; 800% is ~27 s per cloth
    // voxel and is well past anything playable.
    if (cb.burnDurationPct < 25 || cb.burnDurationPct > 800) {
      out.warnings.push_back("combustion.burnDurationPct outside 25..800; clamped");
      cb.burnDurationPct = cb.burnDurationPct < 25 ? 25 : 800;
    }
    ReadI(*g, "spreadPct", cb.spreadPct, out, at);
    // Clamped for the same reason burnDurationPct is: at 0 every ignition
    // chance would land on the 1-unit floor materials.cpp applies, i.e. "fire
    // never spreads at all", which is a typo silently deleting a whole
    // mechanic. 1% is a hundredth of the authored rate and already glacial;
    // 400% is four times and is well past anything survivable.
    if (cb.spreadPct < 1 || cb.spreadPct > 400) {
      out.warnings.push_back("combustion.spreadPct outside 1..400; clamped");
      cb.spreadPct = cb.spreadPct < 1 ? 1 : 400;
    }
    ReadI(*g, "flamePct", cb.flamePct, out, at);
    // 0 IS LEGAL HERE, unlike spreadPct: "the drifting flame ignites nothing,
    // only the coals spread fire" is a coherent game rather than a typo that
    // deletes a mechanic, and it is the one setting that makes the exception
    // absolute. The compiled chance still floors at 1 unit (materials.cpp), so
    // 0 means "as rare as the table can express", not "never".
    if (cb.flamePct < 0 || cb.flamePct > 800) {
      out.warnings.push_back("combustion.flamePct outside 0..800; clamped");
      cb.flamePct = std::clamp(cb.flamePct, 0, 800);
    }
    ReadI(*g, "crossLimbPct", cb.crossLimbPct, out, at);
    if (cb.crossLimbPct < 0 || cb.crossLimbPct > 100) {
      out.warnings.push_back("combustion.crossLimbPct outside 0..100; clamped");
      cb.crossLimbPct = std::clamp(cb.crossLimbPct, 0, 100);
    }
  }

  // ---- wind (docs/RESEARCH_wind.md; the field itself is common.wgsl) ----
  if (const json* g = Find(j, "wind")) {
    auto& w = out.wind;
    const std::string at = "wind";
    ReadF(*g, "windSpeed", w.windSpeed, out, at);
    ReadF(*g, "windDirDeg", w.windDirDeg, out, at);
    ReadF(*g, "gustStrength", w.gustStrength, out, at);
    ReadB(*g, "weatherAuto", w.weatherAuto, out, at);
    ReadF(*g, "gustWavelength", w.gustWavelength, out, at);
    ReadF(*g, "gustSpeed", w.gustSpeed, out, at);
    ReadF(*g, "altitudeGain", w.altitudeGain, out, at);
    ReadF(*g, "altitudeRefY", w.altitudeRefY, out, at);
    ReadB(*g, "dbgWindField", w.dbgWindField, out, at);
    ReadF(*g, "dbgWindSpacing", w.dbgWindSpacing, out, at);
    ReadF(*g, "dbgWindRadius", w.dbgWindRadius, out, at);

    // Clamps, in the order they can bite.
    //
    // A zero or negative gust wavelength is an infinite spatial frequency: the
    // gust phase changes by more than pi between adjacent samples and the
    // "field" aliases into per-cell noise. The shader has its own guard on
    // this constant (WIND_GUST_K) because a shader compiled offline never
    // passes through here at all.
    if (w.gustWavelength < 0.25f) {
      out.warnings.push_back("wind.gustWavelength below 0.25 m; clamped "
                             "(shorter than a cell aliases into noise)");
      w.gustWavelength = 0.25f;
    }
    if (w.windSpeed < 0.0f) {
      out.warnings.push_back("wind.windSpeed negative; clamped to 0 — reverse "
                             "the direction with windDirDeg, not the speed");
      w.windSpeed = 0.0f;
    }
    if (w.gustStrength < 0.0f) {
      out.warnings.push_back("wind.gustStrength negative; clamped to 0");
      w.gustStrength = 0.0f;
    }
    if (w.gustSpeed < 0.0f) {
      out.warnings.push_back("wind.gustSpeed negative; clamped to 0 (a frozen "
                             "field, which is a legitimate thing to want)");
      w.gustSpeed = 0.0f;
    }
    // The overlay's cost is cubic in radius/spacing, and the arrow count is
    // computed identically on the CPU (for the instance count) and in the
    // vertex shader (for the lattice). Clamping the INPUTS here is what keeps
    // those two derivations agreeing about a silly value instead of one of
    // them saturating and the other not.
    if (w.dbgWindSpacing < 1.0f) {
      out.warnings.push_back("wind.dbgWindSpacing below 1 voxel; clamped");
      w.dbgWindSpacing = 1.0f;
    }
    if (w.dbgWindRadius < 0.0f) {
      out.warnings.push_back("wind.dbgWindRadius negative; clamped to 0");
      w.dbgWindRadius = 0.0f;
    }
  }

  if (const json* g = Find(j, "render")) {
    auto& r = out.render;
    const std::string at = "render";
    ReadF(*g, "skyGradient", r.skyGradient, out, at);
    ReadB(*g, "dbgCurrentField", r.dbgCurrentField, out, at);
    ReadF(*g, "dbgCurrentSpacing", r.dbgCurrentSpacing, out, at);
    ReadF(*g, "dbgCurrentRadius", r.dbgCurrentRadius, out, at);
    ReadF(*g, "skyHorizonOffset", r.skyHorizonOffset, out, at);
    ReadV3(*g, "skyHorizon", r.skyHorizon, out, at);
    ReadV3(*g, "skyZenith", r.skyZenith, out, at);
    ReadV3(*g, "fluidColor", r.fluidColor, out, at);
    ReadV3(*g, "fluidColor1", r.fluidColor1, out, at);
    ReadV3(*g, "fluidColor2", r.fluidColor2, out, at);
    ReadV3(*g, "fluidColor3", r.fluidColor3, out, at);
    ReadF(*g, "fluidParticleSize", r.fluidParticleSize, out, at);
    ReadF(*g, "fluidStretch", r.fluidStretch, out, at);
    ReadF(*g, "fluidDensityShade", r.fluidDensityShade, out, at);
    ReadF(*g, "fluidFoam", r.fluidFoam, out, at);
    ReadF(*g, "fluidSurface", r.fluidSurface, out, at);
    ReadF(*g, "fluidIso", r.fluidIso, out, at);
    ReadF(*g, "fluidSmooth", r.fluidSmooth, out, at);
    ReadF(*g, "fluidLevel", r.fluidLevel, out, at);
    ReadF(*g, "fluidIor", r.fluidIor, out, at);
    ReadF(*g, "fluidClarity", r.fluidClarity, out, at);
    ReadF(*g, "fluidReflect", r.fluidReflect, out, at);
    ReadF(*g, "fluidSpecular", r.fluidSpecular, out, at);
    ReadV3(*g, "fluidShallow", r.fluidShallow, out, at);
    ReadV3(*g, "fluidDeep", r.fluidDeep, out, at);
    ReadF(*g, "fluidDepth", r.fluidDepth, out, at);
    ReadF(*g, "fluidGradient", r.fluidGradient, out, at);
    ReadF(*g, "fluidFoamField", r.fluidFoamField, out, at);
    ReadF(*g, "fluidFoamTexture", r.fluidFoamTexture, out, at);
    ReadV3(*g, "foamColor", r.foamColor, out, at);
    ReadF(*g, "foamColorVar", r.foamColorVar, out, at);
    ReadF(*g, "fluidFoamSpeed", r.fluidFoamSpeed, out, at);
    ReadF(*g, "fluidWobble", r.fluidWobble, out, at);
    ReadV3(*g, "sunTint", r.sunTint, out, at);
    ReadF(*g, "sunDiscPower", r.sunDiscPower, out, at);
    ReadF(*g, "sunDiscGain", r.sunDiscGain, out, at);
    ReadF(*g, "sunHaloPower", r.sunHaloPower, out, at);
    ReadF(*g, "sunHaloGain", r.sunHaloGain, out, at);
    ReadV3(*g, "sunDir", r.sunDir, out, at);
    ReadV3(*g, "sunColor", r.sunColor, out, at);
    ReadF(*g, "sunIntensity", r.sunIntensity, out, at);
    // atmospheric sky
    ReadF(*g, "skyRayleigh", r.skyRayleigh, out, at);
    ReadF(*g, "skyMie", r.skyMie, out, at);
    ReadF(*g, "skyMieG", r.skyMieG, out, at);
    ReadF(*g, "skyMieStrength", r.skyMieStrength, out, at);
    ReadF(*g, "skyExposure", r.skyExposure, out, at);
    ReadV3(*g, "skyGround", r.skyGround, out, at);
    ReadF(*g, "sunSize", r.sunSize, out, at);
    ReadF(*g, "sunReddening", r.sunReddening, out, at);
    // night sky
    ReadV3(*g, "nightZenith", r.nightZenith, out, at);
    ReadV3(*g, "nightHorizon", r.nightHorizon, out, at);
    ReadF(*g, "starBrightness", r.starBrightness, out, at);
    ReadF(*g, "starDensity", r.starDensity, out, at);
    ReadF(*g, "starSize", r.starSize, out, at);
    ReadF(*g, "starSparsity", r.starSparsity, out, at);
    ReadF(*g, "starTwinkle", r.starTwinkle, out, at);
    ReadF(*g, "milkyWayStrength", r.milkyWayStrength, out, at);
    ReadV3(*g, "milkyWayColor", r.milkyWayColor, out, at);
    ReadV3(*g, "galaxyNormal", r.galaxyNormal, out, at);
    ReadF(*g, "galaxyWidth", r.galaxyWidth, out, at);
    ReadF(*g, "nebulaStrength", r.nebulaStrength, out, at);
    ReadV3(*g, "nebulaCool", r.nebulaCool, out, at);
    ReadV3(*g, "nebulaWarm", r.nebulaWarm, out, at);
    ReadF(*g, "auroraStrength", r.auroraStrength, out, at);
    ReadF(*g, "auroraHeight", r.auroraHeight, out, at);
    ReadV3(*g, "auroraLow", r.auroraLow, out, at);
    ReadV3(*g, "auroraHigh", r.auroraHigh, out, at);
    // moons
    ReadF(*g, "moonBrightness", r.moonBrightness, out, at);
    ReadV3(*g, "moonColor", r.moonColor, out, at);
    ReadV3(*g, "moonMariaSeed", r.moonMariaSeed, out, at);
    ReadF(*g, "moonGlow", r.moonGlow, out, at);
    ReadF(*g, "moonEarthshine", r.moonEarthshine, out, at);
    ReadV3(*g, "moonLightColor", r.moonLightColor, out, at);
    ReadF(*g, "moonLightIntensity", r.moonLightIntensity, out, at);
    ReadV3(*g, "moon2Color", r.moon2Color, out, at);
    ReadV3(*g, "moon2MariaSeed", r.moon2MariaSeed, out, at);
    ReadF(*g, "moon2Brightness", r.moon2Brightness, out, at);
    ReadF(*g, "moon2LightIntensity", r.moon2LightIntensity, out, at);
    ReadV3(*g, "moon2LightColor", r.moon2LightColor, out, at);
    ReadF(*g, "eclipseDarkness", r.eclipseDarkness, out, at);
    ReadF(*g, "eclipseCurve", r.eclipseCurve, out, at);
    // night ambient
    ReadV3(*g, "nightAmbSky", r.nightAmbSky, out, at);
    ReadV3(*g, "nightAmbGround", r.nightAmbGround, out, at);
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
    ReadI(*g, "shadowCache", r.shadowCache, out, at);
    ReadI(*g, "shadowCacheSubdiv", r.shadowCacheSubdiv, out, at);
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
    ReadV3(*g, "burnTintColor", r.burnTintColor, out, at);
    ReadF(*g, "burnTintRate", r.burnTintRate, out, at);
    ReadF(*g, "burnTintMin", r.burnTintMin, out, at);
    ReadF(*g, "burnTintMax", r.burnTintMax, out, at);
    ReadF(*g, "waterF0", r.waterF0, out, at);
    ReadV3(*g, "waterAbsorb", r.waterAbsorb, out, at);
    ReadV3(*g, "waterScatter", r.waterScatter, out, at);
    ReadF(*g, "waterFresnelPower", r.waterFresnelPower, out, at);
    ReadF(*g, "rippleAmpScale", r.rippleAmpScale, out, at);
    ReadF(*g, "rippleSpeedScale", r.rippleSpeedScale, out, at);
    ReadF(*g, "waterFetchLow", r.waterFetchLow, out, at);
    ReadF(*g, "waterFetchHigh", r.waterFetchHigh, out, at);
    ReadF(*g, "reflectionCutoff", r.reflectionCutoff, out, at);
    ReadI(*g, "reflectionSteps", r.reflectionSteps, out, at);
    ReadF(*g, "causticGain", r.causticGain, out, at);
    ReadF(*g, "causticCap", r.causticCap, out, at);
    ReadF(*g, "glintIntensity", r.glintIntensity, out, at);
    ReadF(*g, "glintPowerNear", r.glintPowerNear, out, at);
    ReadF(*g, "glintPowerFar", r.glintPowerFar, out, at);
    ReadF(*g, "foamDepth", r.foamDepth, out, at);
    ReadF(*g, "foamStrength", r.foamStrength, out, at);
    ReadF(*g, "iceF0", r.iceF0, out, at);
    ReadF(*g, "iceFresnelPower", r.iceFresnelPower, out, at);
    ReadF(*g, "iceAbsorb", r.iceAbsorb, out, at);
    ReadF(*g, "iceAbsorbFloor", r.iceAbsorbFloor, out, at);
    ReadF(*g, "iceScatter", r.iceScatter, out, at);
    ReadF(*g, "iceScatterDepth", r.iceScatterDepth, out, at);
    ReadF(*g, "iceScatterNight", r.iceScatterNight, out, at);
    ReadF(*g, "iceGrain", r.iceGrain, out, at);
    ReadF(*g, "iceGrainScale", r.iceGrainScale, out, at);
    ReadF(*g, "iceGloss", r.iceGloss, out, at);
    ReadF(*g, "iceSpec", r.iceSpec, out, at);
    ReadF(*g, "iceDepthMax", r.iceDepthMax, out, at);
    ReadF(*g, "iceReflectMin", r.iceReflectMin, out, at);

    // ---- submerged view ----
    ReadV3(*g, "subAbsorb", r.subAbsorb, out, at);
    ReadV3(*g, "subScatter", r.subScatter, out, at);
    ReadF(*g, "subScatterGain", r.subScatterGain, out, at);
    ReadF(*g, "subVisibility", r.subVisibility, out, at);
    // A visibility of 0 divides by zero in the shader's fade and takes the
    // whole submerged view to NaN, which renders as a black screen the moment
    // the player's head goes under. Floor it well clear of that.
    if (r.subVisibility < 0.5f) {
      out.warnings.push_back(at + ".subVisibility < 0.5; clamped to 0.5");
      r.subVisibility = 0.5f;
    }
    ReadF(*g, "subVignette", r.subVignette, out, at);
    r.subVignette = std::clamp(r.subVignette, 0.0f, 1.0f);
    ReadF(*g, "subSnellGain", r.subSnellGain, out, at);
    ReadF(*g, "bedCausticGain", r.bedCausticGain, out, at);
    ReadF(*g, "bedCausticCap", r.bedCausticCap, out, at);
    ReadF(*g, "bedCausticFade", r.bedCausticFade, out, at);
    if (r.bedCausticFade < 0.1f) {
      out.warnings.push_back(at + ".bedCausticFade < 0.1; clamped to 0.1");
      r.bedCausticFade = 0.1f;
    }
    ReadF(*g, "bedCausticSharp", r.bedCausticSharp, out, at);
    r.bedCausticSharp = std::clamp(r.bedCausticSharp, 0.1f, 8.0f);

    // ---- god rays ----
    // Both step counts are a direct per-submerged-pixel frame-time multiplier
    // (steps x shadowSteps is the real cost), so the ceilings here are a perf
    // guard, not a taste judgement. 0 steps disables the effect cleanly.
    ReadI(*g, "godRaySteps", r.godRaySteps, out, at);
    if (r.godRaySteps < 0 || r.godRaySteps > 64) {
      out.warnings.push_back(at + ".godRaySteps out of 0..64; clamped");
      r.godRaySteps = std::clamp(r.godRaySteps, 0, 64);
    }
    ReadF(*g, "godRayStrength", r.godRayStrength, out, at);
    ReadF(*g, "godRayAniso", r.godRayAniso, out, at);
    // The Henyey-Greenstein phase function has a (1 - g^2) numerator and a
    // denominator that goes to zero as |g| -> 1, so a g of exactly 1 is a
    // division by zero and anything past it is not a valid phase function.
    r.godRayAniso = std::clamp(r.godRayAniso, -0.95f, 0.95f);
    ReadF(*g, "godRayRange", r.godRayRange, out, at);
    if (r.godRayRange < 0.1f) {
      out.warnings.push_back(at + ".godRayRange < 0.1; clamped to 0.1");
      r.godRayRange = 0.1f;
    }
    ReadI(*g, "godRayShadowSteps", r.godRayShadowSteps, out, at);
    if (r.godRayShadowSteps < 0 || r.godRayShadowSteps > 64) {
      out.warnings.push_back(at + ".godRayShadowSteps out of 0..64; clamped");
      r.godRayShadowSteps = std::clamp(r.godRayShadowSteps, 0, 64);
    }

    // ---- silt ----
    ReadF(*g, "siltDensity", r.siltDensity, out, at);
    r.siltDensity = std::clamp(r.siltDensity, 0.0f, 4.0f);
    ReadF(*g, "siltBrightness", r.siltBrightness, out, at);
    ReadF(*g, "siltDrift", r.siltDrift, out, at);

    // ---- waterfall mist / spray ----
    // Clamped at 0 on the low side so `mistDensity <= 0` stays the ONE off
    // switch the shader const-folds on, and generously on the high side
    // because these are look knobs with no stability hazard behind them.
    ReadF(*g, "mistDensity", r.mistDensity, out, at);
    r.mistDensity = std::clamp(r.mistDensity, 0.0f, 4.0f);
    ReadF(*g, "mistBrightness", r.mistBrightness, out, at);
    r.mistBrightness = std::clamp(r.mistBrightness, 0.0f, 4.0f);
    ReadF(*g, "mistRadius", r.mistRadius, out, at);
    r.mistRadius = std::clamp(r.mistRadius, 0.5f, 48.0f);
    ReadF(*g, "mistFallSpeed", r.mistFallSpeed, out, at);
    ReadF(*g, "sprayDensity", r.sprayDensity, out, at);
    r.sprayDensity = std::clamp(r.sprayDensity, 0.0f, 4.0f);
    ReadF(*g, "sprayRadius", r.sprayRadius, out, at);
    r.sprayRadius = std::clamp(r.sprayRadius, 0.5f, 48.0f);
    ReadF(*g, "subSurfaceRipple", r.subSurfaceRipple, out, at);

    // ---- generic per-liquid submerged profile ----
    ReadF(*g, "subMurkVis", r.subMurkVis, out, at);
    // Same divide-by-zero hazard as subVisibility: visM is the denominator of
    // the extinction term, and a zero there takes a submerged frame to NaN,
    // which renders as a black screen the moment you go under.
    if (r.subMurkVis < 0.05f) {
      out.warnings.push_back(at + ".subMurkVis < 0.05; clamped to 0.05");
      r.subMurkVis = 0.05f;
    }
    ReadF(*g, "subVisCurve", r.subVisCurve, out, at);
    // pow() with a negative exponent on a clarity approaching 0 explodes to
    // infinity; 0 would make every liquid equally visible and defeat the knob.
    r.subVisCurve = std::clamp(r.subVisCurve, 0.05f, 8.0f);
    ReadF(*g, "subAbsorbGain", r.subAbsorbGain, out, at);
    r.subAbsorbGain = std::max(r.subAbsorbGain, 0.0f);
    ReadF(*g, "subAbsorbFloor", r.subAbsorbFloor, out, at);
    r.subAbsorbFloor = std::max(r.subAbsorbFloor, 0.0f);
    ReadF(*g, "subScatterDense", r.subScatterDense, out, at);
    ReadF(*g, "subScatterClear", r.subScatterClear, out, at);
    ReadF(*g, "subClearLow", r.subClearLow, out, at);
    ReadF(*g, "subClearHigh", r.subClearHigh, out, at);
    // smoothstep with low >= high is undefined-ish (it degenerates to a step
    // at best); keep a real band so the crossover onto water's coefficients
    // stays a blend rather than a cliff.
    if (r.subClearHigh <= r.subClearLow) {
      out.warnings.push_back(
          at + ".subClearHigh <= subClearLow; widened to keep a blend band");
      r.subClearHigh = r.subClearLow + 0.05f;
    }

    ReadF(*g, "subMurkGlow", r.subMurkGlow, out, at);
    r.subMurkGlow = std::max(r.subMurkGlow, 0.0f);

    // ---- oil / petroleum-like viscous liquids ----
    ReadF(*g, "oilSatLow", r.oilSatLow, out, at);
    ReadF(*g, "oilSatHigh", r.oilSatHigh, out, at);
    // Same smoothstep hazard as the clarity band above: low >= high collapses
    // the blend into a hard switch, so a liquid authored near the boundary
    // would flip between the blood and oil looks instead of crossing over.
    if (r.oilSatHigh <= r.oilSatLow) {
      out.warnings.push_back(
          at + ".oilSatHigh <= oilSatLow; widened to keep a blend band");
      r.oilSatHigh = r.oilSatLow + 0.05f;
    }
    ReadF(*g, "oilF0", r.oilF0, out, at);
    ReadF(*g, "oilGraze", r.oilGraze, out, at);
    ReadF(*g, "oilGloss", r.oilGloss, out, at);
    // pow(x, e) with e <= 0 is 1 everywhere, which turns the specular lobe into
    // a full-screen white wash.
    r.oilGloss = std::max(r.oilGloss, 1.0f);
    ReadF(*g, "oilSheen", r.oilSheen, out, at);
    ReadF(*g, "oilReflectTint", r.oilReflectTint, out, at);
    ReadF(*g, "oilDarken", r.oilDarken, out, at);
    ReadF(*g, "oilIridescence", r.oilIridescence, out, at);
    ReadF(*g, "oilFilmScale", r.oilFilmScale, out, at);
    ReadF(*g, "oilFloatSens", r.oilFloatSens, out, at);
    r.oilFloatSens = std::max(r.oilFloatSens, 0.0f);
    ReadF(*g, "oilEdgeBand", r.oilEdgeBand, out, at);
    // A zero-width smoothstep band is a hard step, which puts the aliased cube
    // silhouette straight back; too wide and the droplet goes translucent,
    // which is the bug this parameter exists to fix.
    r.oilEdgeBand = std::clamp(r.oilEdgeBand, 0.01f, 0.5f);
    ReadF(*g, "oilDropReflect", r.oilDropReflect, out, at);
    r.oilDropReflect = std::clamp(r.oilDropReflect, 0.0f, 1.0f);
    // Divides into the band phase; zero makes the whole surface one flat colour.
    r.oilFilmScale = std::max(r.oilFilmScale, 0.01f);

    ReadF(*g, "bloodF0", r.bloodF0, out, at);
    ReadF(*g, "bloodGraze", r.bloodGraze, out, at);
    ReadF(*g, "bloodAbsorb", r.bloodAbsorb, out, at);
    ReadF(*g, "bloodTransmit", r.bloodTransmit, out, at);
    ReadF(*g, "bloodMaxTransmit", r.bloodMaxTransmit, out, at);
    ReadF(*g, "bloodDepthRamp", r.bloodDepthRamp, out, at);
    ReadF(*g, "bloodPoolLow", r.bloodPoolLow, out, at);
    ReadF(*g, "bloodPoolHigh", r.bloodPoolHigh, out, at);
    ReadF(*g, "bloodSmooth", r.bloodSmooth, out, at);
    ReadF(*g, "bloodEdgeFeather", r.bloodEdgeFeather, out, at);
    ReadF(*g, "bloodWobble", r.bloodWobble, out, at);
    ReadF(*g, "bloodSheen", r.bloodSheen, out, at);
    ReadF(*g, "bloodSheenDrop", r.bloodSheenDrop, out, at);
    ReadF(*g, "bloodSheenPool", r.bloodSheenPool, out, at);
    ReadF(*g, "bloodAmbientSheen", r.bloodAmbientSheen, out, at);
    ReadF(*g, "bloodEdgeDepth", r.bloodEdgeDepth, out, at);
    ReadF(*g, "bloodEdgeStrength", r.bloodEdgeStrength, out, at);
    ReadV3(*g, "bloodEdgeTint", r.bloodEdgeTint, out, at);
    ReadF(*g, "stainCoverage", r.stainCoverage, out, at);
    ReadF(*g, "stainMottle", r.stainMottle, out, at);
    ReadF(*g, "stainMottleScale", r.stainMottleScale, out, at);
    ReadF(*g, "stainDarken", r.stainDarken, out, at);
    ReadF(*g, "stainOpacity", r.stainOpacity, out, at);
    ReadF(*g, "stainSheen", r.stainSheen, out, at);
    ReadF(*g, "stainSheenPower", r.stainSheenPower, out, at);
    ReadF(*g, "lavaCrackFreq", r.lavaCrackFreq, out, at);
    ReadF(*g, "lavaCrackKneeLow", r.lavaCrackKneeLow, out, at);
    ReadF(*g, "lavaCrackKneeHigh", r.lavaCrackKneeHigh, out, at);
    ReadF(*g, "lavaWarmBias", r.lavaWarmBias, out, at);
    ReadF(*g, "lavaEmissionGain", r.lavaEmissionGain, out, at);
    ReadF(*g, "lavaPulseAmp", r.lavaPulseAmp, out, at);
    ReadF(*g, "lavaPulseRate", r.lavaPulseRate, out, at);
    ReadF(*g, "emberBrightness", r.emberBrightness, out, at);
    ReadF(*g, "emberRise", r.emberRise, out, at);
    ReadF(*g, "emberRate", r.emberRate, out, at);
    ReadI(*g, "emberDensity", r.emberDensity, out, at);
    ReadF(*g, "exposureWhite", r.exposureWhite, out, at);
    ReadF(*g, "bleachAmount", r.bleachAmount, out, at);
    ReadF(*g, "gamma", r.gamma, out, at);
    ReadF(*g, "microLodDist", r.microLodDist, out, at);
    ReadF(*g, "plantLodDist", r.plantLodDist, out, at);
    ReadI(*g, "microMaxPerRay", r.microMaxPerRay, out, at);
    ReadF(*g, "microSwayAmp", r.microSwayAmp, out, at);
    ReadF(*g, "microSwaySpeed", r.microSwaySpeed, out, at);
    ReadF(*g, "trampleRecover", r.trampleRecover, out, at);
    ReadF(*g, "trampleDepth", r.trampleDepth, out, at);
    ReadF(*g, "trampleLean", r.trampleLean, out, at);
    ReadF(*g, "trampleRadius", r.trampleRadius, out, at);
    ReadI(*g, "primarySteps", r.primarySteps, out, at);
    ReadI(*g, "farSteps", r.farSteps, out, at);
    ReadF(*g, "farShadowReach", r.farShadowReach, out, at);
    ReadI(*g, "farBlockerHitLevel", r.farBlockerHitLevel, out, at);
    ReadF(*g, "lodHandoffDist", r.lodHandoffDist, out, at);
    ReadF(*g, "renderScale", r.renderScale, out, at);
    ReadI(*g, "taa", r.taa, out, at);
    ReadF(*g, "taaMaxHist", r.taaMaxHist, out, at);
    ReadF(*g, "taaClamp", r.taaClamp, out, at);
    ReadF(*g, "taaJitter", r.taaJitter, out, at);
    ReadI(*g, "taaSharpLod", r.taaSharpLod, out, at);
    ReadI(*g, "presentMode", r.presentMode, out, at);
    ReadF(*g, "fpsCap", r.fpsCap, out, at);
    ReadF(*g, "shadowMaxDist", r.shadowMaxDist, out, at);

    ReadF(*g, "shadowCoarseDist", r.shadowCoarseDist, out, at);
    ReadF(*g, "opennessReach", r.opennessReach, out, at);
    ReadI(*g, "opennessChunksPerFrame", r.opennessChunksPerFrame, out, at);
    ReadF(*g, "opennessStrength", r.opennessStrength, out, at);
    ReadF(*g, "opennessFloor", r.opennessFloor, out, at);
    ReadI(*g, "opennessBilinear", r.opennessBilinear, out, at);
    ReadF(*g, "giStrength", r.giStrength, out, at);
    ReadF(*g, "giDecay", r.giDecay, out, at);
    ReadF(*g, "giFeedback", r.giFeedback, out, at);
    ReadI(*g, "giGatherBlocks", r.giGatherBlocks, out, at);
    ReadF(*g, "shortRangeDist", r.shortRangeDist, out, at);
    ReadF(*g, "shortRangeFogStart", r.shortRangeFogStart, out, at);
    ReadF(*g, "shortRangeFogDensity", r.shortRangeFogDensity, out, at);
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
    // starSize divides in the star PSF, starDensity scales the direction grid,
    // and skyMieG at exactly +-1 makes the Henyey-Greenstein denominator
    // collapse.
    if (r.starSize <= 0.0f) {
      out.warnings.push_back("render.starSize must be > 0; reset to 0.85");
      r.starSize = 0.85f;
    }
    if (r.starSparsity < 0.0f || r.starSparsity > 1.0f) {
      out.warnings.push_back("render.starSparsity outside 0..1; clamped");
      r.starSparsity = r.starSparsity < 0.0f ? 0.0f : 1.0f;
    }
    if (r.starDensity < 1.0f) {
      out.warnings.push_back("render.starDensity < 1; clamped to 1");
      r.starDensity = 1.0f;
    }
    // eclipseDarkness scales a subtraction from the daylight weight; past 1 it
    // would drive the weight negative and the sky would light from below.
    if (r.eclipseDarkness < 0.0f || r.eclipseDarkness > 1.0f) {
      out.warnings.push_back("render.eclipseDarkness outside 0..1; clamped");
      r.eclipseDarkness = r.eclipseDarkness < 0.0f ? 0.0f : 1.0f;
    }
    // eclipseCurve is an exponent on a 0..1 fraction. At or below 0, pow()
    // takes the uncovered sky (f = 0) to infinity rather than to zero, so a
    // clear day would render as permanent totality.
    if (r.eclipseCurve < 0.05f) {
      out.warnings.push_back("render.eclipseCurve < 0.05; clamped");
      r.eclipseCurve = 0.05f;
    }
    // galaxyWidth is a smoothstep edge; at 0 the band is a zero-width slit and
    // the divide inside smoothstep degenerates.
    if (r.galaxyWidth < 0.001f) {
      out.warnings.push_back("render.galaxyWidth < 0.001; clamped");
      r.galaxyWidth = 0.001f;
    }
    // galaxyNormal is normalize()d in the shader, where a zero vector is NaN
    // and NaN spreads across the whole night sky rather than failing locally.
    {
      const float* n = r.galaxyNormal;
      if (n[0] * n[0] + n[1] * n[1] + n[2] * n[2] < 1e-8f) {
        out.warnings.push_back(
            "render.galaxyNormal is degenerate (zero length); reset");
        r.galaxyNormal[0] = 0.36f;
        r.galaxyNormal[1] = 0.52f;
        r.galaxyNormal[2] = -0.77f;
      }
    }
    if (r.skyMieG <= -0.99f || r.skyMieG >= 0.99f) {
      out.warnings.push_back("render.skyMieG must be within (-0.99, 0.99); clamped");
      r.skyMieG = r.skyMieG < 0.0f ? -0.99f : 0.99f;
    }
    if (r.auroraHeight <= 0.0f) {
      out.warnings.push_back("render.auroraHeight must be > 0; reset to 900");
      r.auroraHeight = 900.0f;
    }
    if (r.sunSize <= 0.0f) {
      out.warnings.push_back("render.sunSize must be > 0; reset to 1.0");
      r.sunSize = 1.0f;
    }
    if (r.primarySteps < 1) { r.primarySteps = 1; }
    // A negative micro budget would underflow the shader's u32 counter and
    // uncap the very thing the knob exists to bound; 0 means "never nest",
    // which is a legitimate way to turn the feature off by eye.
    if (r.microMaxPerRay < 0) { r.microMaxPerRay = 0; }
    if (r.microLodDist < 0.0f) { r.microLodDist = 0.0f; }
    if (r.plantLodDist < 0.0f) { r.plantLodDist = 0.0f; }
    // The sway models are authored with a 2-sub-voxel wall margin; a larger
    // amplitude shears blade tips out of the cell where they simply vanish.
    if (r.microSwayAmp < 0.0f) { r.microSwayAmp = 0.0f; }
    if (r.microSwayAmp > 2.0f) { r.microSwayAmp = 2.0f; }
    if (r.microSwaySpeed < 0.0f) { r.microSwaySpeed = 0.0f; }
    if (r.trampleRecover < 0.05f) { r.trampleRecover = 0.05f; }
    if (r.trampleDepth < 0.0f) { r.trampleDepth = 0.0f; }
    if (r.trampleDepth > 0.95f) { r.trampleDepth = 0.95f; }
    if (r.trampleLean < 0.0f) { r.trampleLean = 0.0f; }
    if (r.trampleLean > 1.0f) { r.trampleLean = 1.0f; }
    if (r.trampleRadius < 0.0f) { r.trampleRadius = 0.0f; }
    if (r.shadowSteps < 0) { r.shadowSteps = 0; }
    // Subdivision must be >= 1 and within the 3 bits the shadow request record
    // gives each sub-index (world.h kShadowSubdivMax). A 0 here would divide by
    // zero in shadowPatchCentre; anything over the max would alias two patches
    // onto one key, which reads as shadows flickering between neighbours.
    if (r.shadowCacheSubdiv < 1) { r.shadowCacheSubdiv = 1; }
    // 8 is world.h's kShadowSubdivMax, restated rather than included: this TU
    // does not pull in world.h, and the shader clamps to SHADOW_SUBDIV_MAX
    // independently, so an over-large value here is bounded twice over. The
    // clamp exists so the tuner slider cannot silently alias two patches onto
    // one key, not as the load-bearing guard.
    if (r.shadowCacheSubdiv > 8) { r.shadowCacheSubdiv = 8; }
    if (r.reflectionSteps < 0) { r.reflectionSteps = 0; }
    if (r.farSteps < 1) { r.farSteps = 1; }
    // A zero/negative reach would clamp to the 8-step floor everywhere and
    // silently drop far shadows; keep it positive.
    if (r.farShadowReach < 1.0f) { r.farShadowReach = 1.0f; }
    // A level cap, so the legal range is 0 (off) .. kFarLevels. Out-of-range
    // values are not merely useless here: the shader compares it against a
    // 1-based level, so a negative would read as "off" by accident rather
    // than by intent and anything past the top level is a lie about coverage.
    if (r.farBlockerHitLevel < 0) { r.farBlockerHitLevel = 0; }
    if (r.farBlockerHitLevel > (int)kFarLevels) {
      r.farBlockerHitLevel = (int)kFarLevels;
    }
    // The LOD handoff must not land in front of the near clip: a zero here
    // would hand EVERY ray to the cascade at t=0 and render the world as
    // 40 cm blocks from the camera outward. Floored at 2 m, which is still
    // absurdly near but is a knob setting rather than a broken frame. There is
    // deliberately no ceiling — >= 25.6 m disables the handoff, which is the
    // documented way to A/B it.
    if (r.lodHandoffDist < 2.0f) { r.lodHandoffDist = 2.0f; }
    // A scale above 1 would be supersampling the most expensive shader in the
    // engine; below a quarter the frame is 400x225 and the UI text on top is
    // larger than the terrain. Present mode is an enum; fpsCap 0 = off.
    if (r.renderScale > 1.0f) { r.renderScale = 1.0f; }
    if (r.renderScale < 0.25f) { r.renderScale = 0.25f; }
    // TAA: a history shorter than 1 sample is not a history, and one longer
    // than the f16 the shader stores it in cannot be counted. The clamp width
    // and the jitter are both allowed to be 0 (those are the A/B arms) but
    // neither is allowed to be negative, which would invert the test it feeds.
    if (r.taaMaxHist < 1.0f) { r.taaMaxHist = 1.0f; }
    if (r.taaMaxHist > 512.0f) { r.taaMaxHist = 512.0f; }
    if (r.taaClamp < 0.0f) { r.taaClamp = 0.0f; }
    if (r.taaJitter < 0.0f) { r.taaJitter = 0.0f; }
    if (r.taaJitter > 2.0f) { r.taaJitter = 2.0f; }
    if (r.presentMode < 0 || r.presentMode > 2) { r.presentMode = 1; }
    if (r.fpsCap < 0.0f) { r.fpsCap = 0.0f; }
    // 0 is meaningful here (all shadows go through the cascade), so only
    // negatives are refused.
    if (r.shadowMaxDist < 0.0f) { r.shadowMaxDist = 0.0f; }

    // 0 is the OFF value (the shader const-folds the coarse march away), so
    // only negatives are refused — a negative would make every ray coarse from
    // its first cell, which is what god rays ask for explicitly and no shadow
    // ray should get by accident.
    if (r.shadowCoarseDist < 0.0f) { r.shadowCoarseDist = 0.0f; }
    // A negative reach would make every ray report "blocked at t < 0" and
    // paint the world black; a negative strength would brighten the ambient
    // past the sky value. Clamp rather than warn -- neither is expressible as
    // an intent, and both are one keystroke away in the tuner.
    if (r.opennessReach < 0.0f) { r.opennessReach = 0.0f; }
    r.opennessStrength = std::clamp(r.opennessStrength, 0.0f, 1.0f);
    r.opennessFloor = std::clamp(r.opennessFloor, 0.0f, 1.0f);
    if (r.opennessChunksPerFrame < 0) { r.opennessChunksPerFrame = 0; }
    // Indirect light (PLAN_gi.md §3-4). A negative strength would subtract
    // light; a decay outside [0,1] is meaningless; and the P2 write-back must
    // stay strictly below the decay or every bounce adds more than the walk
    // forgets and the grid brightens without bound. That last one is the
    // assertion PLAN_gi.md §4 asks for, expressed as a warning plus a clamp
    // because a bad value is one keystroke away in the tuner and must not
    // take the game down.
    if (r.giStrength < 0.0f) { r.giStrength = 0.0f; }
    r.giDecay = std::clamp(r.giDecay, 0.0f, 1.0f);
    r.giFeedback = std::clamp(r.giFeedback, 0.0f, 1.0f);
    if (r.giFeedback > 0.0f && r.giFeedback >= r.giDecay) {
      out.warnings.push_back(
          "render.giFeedback must be below render.giDecay (multi-bounce would "
          "brighten without bound); clamped");
      r.giFeedback = std::max(0.0f, r.giDecay * 0.5f);
    }
    r.giGatherBlocks = std::clamp(r.giGatherBlocks, 0, 8);
    // Multi-bounce gain (P2): each bounce is albedo x the gather's 0.28 form
    // factor x giStrength, and the series converges only while that is below
    // 1 -- so with write-back on, giStrength above 3 can run away on a white
    // room. Clamped, and said.
    if (r.giFeedback > 0.0f && r.giStrength > 3.0f) {
      out.warnings.push_back(
          "render.giStrength above 3 with giFeedback on diverges (albedo x 0.28 "
          "x strength must stay below 1); clamped to 3");
      r.giStrength = 3.0f;
    }
    // ---- short-range mode ----
    // The ceiling divides the fog ramp's span, so it must stay positive; 4 m
    // is under the near field's own reach and is already an absurd setting,
    // but it is a setting rather than a NaN frame.
    if (r.shortRangeDist < 4.0f) { r.shortRangeDist = 4.0f; }
    // The start fraction is a fraction. 0.95 rather than 1.0 at the top so the
    // ramp always has a span to run over.
    if (r.shortRangeFogStart < 0.0f) { r.shortRangeFogStart = 0.0f; }
    if (r.shortRangeFogStart > 0.95f) { r.shortRangeFogStart = 0.95f; }
    // Zero density would renormalise 0/0 at the ceiling.
    if (r.shortRangeFogDensity < 0.05f) { r.shortRangeFogDensity = 0.05f; }
  }

  // ---- world / debug: what is left of the old `worldgen` group (P-G) ---------
  // The terrain numbers are the map's (worldmap.cpp LoadWorldMap reads and
  // rescales map.json `terrain`); the per-biome relief is the biome files'.
  if (const json* g = Find(j, "world")) {
    auto& w = out.world;
    const std::string at = "world";
    // A NAME, never a path: worldedit.cpp joins it under assets/worldedits/,
    // and a value with a separator in it would reach outside that directory.
    ReadStr(*g, "mapLayer", w.mapLayer, out, at);
    if (w.mapLayer.empty() || w.mapLayer.find_first_of("/\\:") != std::string::npos) {
      out.warnings.push_back("world.mapLayer must be a bare map name; using \"default\"");
      w.mapLayer = "default";
    }
    ReadStr(*g, "editLayer", w.editLayer, out, at);
    if (w.editLayer.find_first_of("/\\:") != std::string::npos) {
      out.warnings.push_back("world.editLayer must be a bare layer name; ignored");
      w.editLayer.clear();
    }
  }
  if (const json* g = Find(j, "debug")) {
    auto& d = out.debug;
    const std::string at = "debug";
    ReadI(*g, "vegetation", d.vegetation, out, at);
    d.vegetation = (d.vegetation != 0) ? 1 : 0;   // a flag: anything nonzero is on
  }

  return true;
}


// ============================================================================
// WRITING THE COMBAT GROUPS BACK — see SaveCombatTuning's note in tuning.h.
// ============================================================================
namespace {

// The byte span of `"<group>": { ... }` at the top level, or (npos, npos).
// Brace-counted rather than regexed because the gore group contains nested
// objects (the `<key>Var` siblings) and a first-closing-brace search would stop
// inside the first of them.
std::pair<size_t, size_t> JsonGroupSpan(const std::string& text,
                                        const char* group) {
  const std::string needle = std::string("\"") + group + "\"";
  const size_t k = text.find(needle);
  if (k == std::string::npos) return {std::string::npos, std::string::npos};
  const size_t open = text.find('{', k);
  if (open == std::string::npos) return {std::string::npos, std::string::npos};
  int depth = 0;
  for (size_t i = open; i < text.size(); i++) {
    if (text[i] == '{') depth++;
    else if (text[i] == '}' && --depth == 0) return {open, i};
  }
  return {std::string::npos, std::string::npos};
}

// Replace the VALUE of `"key"` inside [lo, hi) with `value`. Returns false —
// changing nothing — if the key is absent or the value is not a literal we
// recognise, so the caller can refuse the whole write.
bool PatchJsonValue(std::string& text, size_t lo, size_t hi, const char* key,
                    const std::string& value) {
  const std::string needle = std::string("\"") + key + "\"";
  const size_t k = text.find(needle, lo);
  if (k == std::string::npos || k >= hi) return false;
  size_t c = text.find(':', k + needle.size());
  if (c == std::string::npos || c >= hi) return false;
  size_t b = c + 1;
  while (b < hi && (text[b] == ' ' || text[b] == '\t')) b++;
  // The value runs to the next separator. A string value would need quote
  // handling; nothing in these three groups has one, so a value that starts
  // with a quote is refused rather than mangled.
  if (b >= hi || text[b] == '"' || text[b] == '{' || text[b] == '[') return false;
  size_t e = b;
  while (e < hi && text[e] != ',' && text[e] != '\n' && text[e] != '}') e++;
  // Trim trailing space so a value written into `1 ,` does not grow one.
  size_t trimmed = e;
  while (trimmed > b && (text[trimmed - 1] == ' ' || text[trimmed - 1] == '\r'))
    trimmed--;
  text.replace(b, trimmed - b, value);
  return true;
}

std::string NumLit(float v) {
  // %g so an integral value stays integral in the file (`18`, not `18.000000`)
  // and the diff after a Save is only the knobs that actually moved. 9 digits
  // is exactly round-trippable for a float, which matters because the panel
  // Saves values it just read back out of this same file.
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.9g", (double)v);
  return buf;
}

}  // namespace

bool SaveCombatTuning(const std::string& path, const Tuning& t,
                      std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "cannot read " + path;
    return false;
  }
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  in.close();

  // Patch into a COPY and only commit if every key landed. See the all-or-
  // nothing note in tuning.h: a partial write is the failure mode that looks
  // like a success.
  std::string outText = text;
  std::string missing;
  const char* curGroup = nullptr;
  auto group = [&](const char* name, size_t& lo, size_t& hi) {
    const auto span = JsonGroupSpan(outText, name);
    lo = span.first;
    hi = span.second;
    curGroup = lo != std::string::npos ? name : nullptr;
    if (lo == std::string::npos) missing += std::string(" ") + name;
    return lo != std::string::npos;
  };

  // EVERY PATCH RE-DERIVES ITS GROUP'S SPAN. PatchJsonValue edits outText in
  // place and a replaced literal rarely keeps its length — NumLit prints the
  // float that actually ships (`%.9g`, so `0.005` becomes `0.00499999989`) —
  // so a span computed once per group DRIFTS: enough growth early in a long
  // group pushes its tail keys past the stale `hi`, and the save then refuses
  // a file that plainly contains them. Measured on the shipped tuning.json:
  // the Combat panel's Save reported blockItemDamage through headClearM (the
  // melee tail) and fleshVolume/clangVolume/cueRadius (the combatfx tail)
  // missing. Re-deriving is a substring search over ~22 KB per key, which is
  // nothing against a button press, and it makes the span correct BY
  // CONSTRUCTION rather than correct until the group grows again.
  size_t lo = 0, hi = 0;
  auto patchIn = [&](const char* key, const std::string& value) {
    if (curGroup == nullptr) return;   // group() already reported the group
    const auto span = JsonGroupSpan(outText, curGroup);
    if (span.first == std::string::npos ||
        !PatchJsonValue(outText, span.first, span.second, key, value))
      missing += std::string(" ") + key;
  };
  auto put = [&](const char* key, float v) { patchIn(key, NumLit(v)); };
  auto putI = [&](const char* key, int v) { patchIn(key, std::to_string(v)); };
  auto putB = [&](const char* key, bool v) {
    patchIn(key, v ? "true" : "false");
  };

  if (group("melee", lo, hi)) {
    const Tuning::Melee& m = t.melee;
    put("commitSpeed", m.commitSpeed);
    put("slashTime", m.slashTime);
    put("recoverTime", m.recoverTime);
    put("fullSpeedMps", m.fullSpeedMps);
    put("minSpeedMps", m.minSpeedMps);
    put("aimGainX", m.aimGainX);
    put("aimGainY", m.aimGainY);
    put("reachGainM", m.reachGainM);
    put("azOut", m.azOut);
    put("azAcross", m.azAcross);
    put("elMin", m.elMin);
    put("elMax", m.elMax);
    put("handExtend", m.handExtend);
    put("extendSmoothing", m.extendSmoothing);
    put("leanTurnRate", m.leanTurnRate);
    put("handLead", m.handLead);
    put("fallbackReachM", m.fallbackReachM);
    put("reachFraction", m.reachFraction);
    put("guardForwardM", m.guardForwardM);
    put("guardUpM", m.guardUpM);
    put("guardSideM", m.guardSideM);
    put("dirSmoothing", m.dirSmoothing);
    put("swingArc", m.swingArc);
    put("swingAnticipate", m.swingAnticipate);
    put("swingExtend", m.swingExtend);
    put("bladeSmoothing", m.bladeSmoothing);
    put("wristMaxAngle", m.wristMaxAngle);
    put("steerSpeedLoMps", m.steerSpeedLoMps);
    put("steerSpeedHiMps", m.steerSpeedHiMps);
    put("steerFloor", m.steerFloor);
    put("armSmoothing", m.armSmoothing);
    put("wristSmoothing", m.wristSmoothing);
    put("handBackFrac", m.handBackFrac);
    put("elbowPoleCone", m.elbowPoleCone);
    put("elbowAxisCone", m.elbowAxisCone);
    put("edgeFloor", m.edgeFloor);
    put("blockGapM", m.blockGapM);
    put("blockItemDamage", m.blockItemDamage);
    put("blockNudgeAz", m.blockNudgeAz);
    put("blockNudgeEl", m.blockNudgeEl);
    putI("controlMode", m.controlMode);
    put("pickMinSpeed", m.pickMinSpeed);
    put("torsoShare", m.torsoShare);
    put("torsoPitch", m.torsoPitch);
    put("headClearM", m.headClearM);
  }
  if (group("combatfx", lo, hi)) {
    const Tuning::CombatFx& f = t.combatfx;
    putB("hitStop", f.hitStop);
    put("hitStopChipScale", f.hitStopChipScale);
    put("hitStopChipMs", f.hitStopChipMs);
    put("hitStopFleshScale", f.hitStopFleshScale);
    put("hitStopFleshMs", f.hitStopFleshMs);
    put("hitStopSeverScale", f.hitStopSeverScale);
    put("hitStopSeverMs", f.hitStopSeverMs);
    put("flashChip", f.flashChip);
    put("flashFlesh", f.flashFlesh);
    put("flashSever", f.flashSever);
    put("flashHalflife", f.flashHalflife);
    put("whooshVolume", f.whooshVolume);
    put("whooshMinSpeed", f.whooshMinSpeed);
    put("whooshRateSlow", f.whooshRateSlow);
    put("whooshRateFast", f.whooshRateFast);
    put("fleshVolume", f.fleshVolume);
    put("clangVolume", f.clangVolume);
    put("cueRadius", f.cueRadius);
  }
  // GORE TOO, because the Combat panel's Damage tab edits it. A Save button
  // that silently declined to save one of its own three tabs is the worst kind
  // of bug: the work is gone and nothing said so. Only the keys that tab
  // exposes are written — the rest of gore.* belongs to the browser tuner and
  // must not be re-emitted from a struct the panel never showed.
  if (group("gore", lo, hi)) {
    const Tuning::Gore& g = t.gore;
    put("cutDepth", g.cutDepth);
    put("cutDepthPower", g.cutDepthPower);
    put("cutLength", g.cutLength);
    put("cutWidth", g.cutWidth);
    putI("cutSpallRounds", g.cutSpallRounds);
    put("cutSpallStrength", g.cutSpallStrength);
    put("woundSeverFraction", g.woundSeverFraction);
    put("woundNeckRadius", g.woundNeckRadius);
    put("woundNeckFraction", g.woundNeckFraction);
    put("woundImpactSeverScale", g.woundImpactSeverScale);
    put("woundHeftRef", g.woundHeftRef);
    put("woundHeftMax", g.woundHeftMax);
    put("woundStainRadius", g.woundStainRadius);
    put("woundStainSurface", g.woundStainSurface);
    put("woundStainDensity", g.woundStainDensity);
    put("corpseBleedPerVoxel", g.corpseBleedPerVoxel);
    put("bleedGain", g.bleedGain);
    put("bleedHpPerVoxel", g.bleedHpPerVoxel);
    putB("stumpBleedsOpen", g.stumpBleedsOpen);
    put("burnCapMidFraction", g.burnCapMidFraction);
    put("burnCapMidHealth", g.burnCapMidHealth);
    put("burnDeathFraction", g.burnDeathFraction);
  }

  if (!missing.empty()) {
    err = "tuning.json is missing:" + missing;
    return false;
  }
  std::ofstream outF(path, std::ios::binary | std::ios::trunc);
  if (!outF) {
    err = "cannot write " + path;
    return false;
  }
  outF << outText;
  return true;
}

std::string TuningWgslBlock(const Tuning& t) {
  std::ostringstream o;
  o << "// GENERATED from assets/materials/tuning.json by TuningWgslBlock() —\n"
       "// do not edit here and do not redeclare these in any shader.\n";

  // Every constant, its type and its WGSL name come from the one table in
  // sim/tuning_params.def, which scripts/tuning_prelude.py is also generated
  // from. Adding a knob that shaders can see is now a single row there plus a
  // declaration in tuning.h -- it is no longer possible to emit a constant the
  // offline validator does not know about, or vice versa.
#define TP_F(group, member, name, def) EmitF(o, #name, t.group.member);
#define TP_I(group, member, name, def) EmitI(o, #name, t.group.member);
#define TP_U(group, member, name, def) EmitU(o, #name, t.group.member);
#define TP_V3(group, member, name, ...) EmitV3(o, #name, t.group.member);
#include "sim/tuning_params.def"
#undef TP_V3
#undef TP_U
#undef TP_I
#undef TP_F

  return o.str();
}
