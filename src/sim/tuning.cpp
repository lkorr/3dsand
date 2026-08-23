#include "sim/tuning.h"

#include <algorithm>
#include <cmath>
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

SkyState ComputeSkyState(const Tuning& t, uint32_t phase, uint32_t dayNumber) {
  const auto& d = t.dayNight;
  SkyState s{};
  const float kPi = 3.14159265358979f;
  const float deg = kPi / 180.0f;

  // phase 0 = midnight, so the sun's hour angle is measured from straight
  // down. At phase 0.25 (0x4000) it is on the eastern horizon, at 0.5 it is at
  // peak elevation, at 0.75 on the western horizon.
  float f = (float)(phase & kDayPhaseMask) / (float)kDayPhaseMax;
  s.dayT = f;
  float hour = (f - 0.5f) * 2.0f * kPi;   // -pi at midnight, 0 at noon

  // Sun on a tilted great circle: elevation peaks at sunPeakElevation and the
  // whole arc is rotated by sunAzimuth, which is what makes the sun rise in a
  // consistent compass direction instead of straight overhead.
  float peak = d.sunPeakElevation * deg;
  float el = std::asin(std::sin(peak) * std::cos(hour));
  // Azimuth sweeps east->west across the day.
  float az = d.sunAzimuth * deg + std::atan2(std::sin(hour), std::cos(hour) * std::cos(peak));
  s.sunDir[0] = std::cos(el) * std::sin(az);
  s.sunDir[1] = std::sin(el);
  s.sunDir[2] = std::cos(el) * std::cos(az);

  // Daylight weight. Smoothstep over the twilight band around the horizon so
  // the sky, ambient and key light all crossfade together rather than snapping
  // at the geometric horizon. This is the RENDER-side daylight measure; the
  // sim uses the integer daylightStrength() in common.wgsl, and they need only
  // agree qualitatively (the sim's is the one that touches voxel state).
  float w = d.twilightWidth > 0.0f ? d.twilightWidth : 0.22f;
  float x = (s.sunDir[1] + w * 0.35f) / w;
  x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
  s.sunUp = x * x * (3.0f - 2.0f * x);

  // Moon: roughly opposite the sun, drifting by one lunar period so the phase
  // cycles, and inclined so it does not simply retrace the sun's arc.
  float lunar = (float)(dayNumber % (uint32_t)(d.lunarPeriodDays < 1 ? 1 : d.lunarPeriodDays)) /
                (float)(d.lunarPeriodDays < 1 ? 1 : d.lunarPeriodDays);
  s.moonPhase = lunar;
  // Moon hour angle lags the sun by the phase fraction — that lag IS the
  // phase, physically: a full moon is opposite the sun and rises at sunset.
  float mhour = hour + kPi + lunar * 2.0f * kPi;
  float minc = d.moonInclination * deg;
  float mel = std::asin(std::sin(peak) * std::cos(mhour) * std::cos(minc) +
                        std::sin(minc) * 0.35f);
  float maz = d.sunAzimuth * deg +
              std::atan2(std::sin(mhour), std::cos(mhour) * std::cos(peak));
  s.moonDir[0] = std::cos(mel) * std::sin(maz);
  s.moonDir[1] = std::sin(mel);
  s.moonDir[2] = std::cos(mel) * std::cos(maz);

  // Stars wheel with the day, plus the accumulated whole days so the sky does
  // not reset every dawn.
  s.starRot = (f + (float)dayNumber) * 2.0f * kPi * d.starRotSpeed;
  return s;
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
    ReadF(*g, "halfWidth", p.halfWidth, out, at);
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
    ReadI(*g, "wanderHopMask", s.wanderHopMask, out, at);
    ReadI(*g, "expMicroPerMille", s.expMicroPerMille, out, at);
    ReadI(*g, "expMicroLifeTicks", s.expMicroLifeTicks, out, at);
    ReadI(*g, "expMicroScaleIdx", s.expMicroScaleIdx, out, at);
    ReadF(*g, "fluidStiffness", s.fluidStiffness, out, at);
    ReadF(*g, "fluidGravity", s.fluidGravity, out, at);
    ReadF(*g, "fluidRestDensity", s.fluidRestDensity, out, at);
    ReadI(*g, "fluidEosPower", s.fluidEosPower, out, at);
    ReadF(*g, "fluidCohesion", s.fluidCohesion, out, at);
    ReadF(*g, "fluidAttractSame", s.fluidAttractSame, out, at);
    ReadF(*g, "fluidAttractDiff", s.fluidAttractDiff, out, at);
    ReadF(*g, "fluidViscosity", s.fluidViscosity, out, at);
    ReadF(*g, "fluidDamping", s.fluidDamping, out, at);
    ReadF(*g, "fluidSplashRate", s.fluidSplashRate, out, at);
    ReadF(*g, "fluidSplashSpeed", s.fluidSplashSpeed, out, at);
    ReadF(*g, "fluidSplashMaxDensity", s.fluidSplashMaxDensity, out, at);
    ReadF(*g, "fluidSplashLife", s.fluidSplashLife, out, at);
    ReadI(*g, "fluidSplashScaleIdx", s.fluidSplashScaleIdx, out, at);
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
    clampWarnF(s.fluidSplashRate, 0.0f, 180.0f, "fluidSplashRate");   // <=1/substep
    clampWarnF(s.fluidSplashSpeed, 0.0f, 90.0f, "fluidSplashSpeed");  // < VMAX
    clampWarnF(s.fluidSplashMaxDensity, 0.0f, 4.0f, "fluidSplashMaxDensity");
    clampWarnF(s.fluidSplashLife, 0.05f, 8.5f, "fluidSplashLife");    // 255 ticks
    if (s.fluidSplashScaleIdx < 0 || s.fluidSplashScaleIdx > 3) {
      out.warnings.push_back("sim.fluidSplashScaleIdx out of 0..3; clamped");
      s.fluidSplashScaleIdx = s.fluidSplashScaleIdx < 0 ? 0 : 3;
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
    ReadF(*g, "sunPeakElevation", d.sunPeakElevation, out, at);
    ReadF(*g, "sunAzimuth", d.sunAzimuth, out, at);
    ReadF(*g, "twilightWidth", d.twilightWidth, out, at);
    ReadI(*g, "lunarPeriodDays", d.lunarPeriodDays, out, at);
    ReadF(*g, "moonInclination", d.moonInclination, out, at);
    ReadF(*g, "starRotSpeed", d.starRotSpeed, out, at);
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
    if (d.freezePhase < 0 || d.freezePhase > 65535) {
      out.warnings.push_back("dayNight.freezePhase outside 0..65535; wrapped");
      d.freezePhase = ((d.freezePhase % 65536) + 65536) % 65536;
    }
    if (d.twilightWidth <= 0.0f) {
      out.warnings.push_back("dayNight.twilightWidth must be > 0; reset to 0.22");
      d.twilightWidth = 0.22f;
    }
  }

  if (const json* g = Find(j, "weather")) {
    auto& w = out.weather;
    const std::string at = "weather";
    ReadB(*g, "waterFreezes", w.waterFreezes, out, at);
    ReadB(*g, "iceMelts", w.iceMelts, out, at);
  }

  if (const json* g = Find(j, "render")) {
    auto& r = out.render;
    const std::string at = "render";
    ReadF(*g, "skyGradient", r.skyGradient, out, at);
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
    ReadF(*g, "fluidIor", r.fluidIor, out, at);
    ReadF(*g, "fluidClarity", r.fluidClarity, out, at);
    ReadF(*g, "fluidReflect", r.fluidReflect, out, at);
    ReadF(*g, "fluidSpecular", r.fluidSpecular, out, at);
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
    ReadF(*g, "nebulaStrength", r.nebulaStrength, out, at);
    ReadV3(*g, "nebulaCool", r.nebulaCool, out, at);
    ReadV3(*g, "nebulaWarm", r.nebulaWarm, out, at);
    ReadF(*g, "auroraStrength", r.auroraStrength, out, at);
    ReadF(*g, "auroraHeight", r.auroraHeight, out, at);
    ReadV3(*g, "auroraLow", r.auroraLow, out, at);
    ReadV3(*g, "auroraHigh", r.auroraHigh, out, at);
    // moon
    ReadF(*g, "moonRadius", r.moonRadius, out, at);
    ReadF(*g, "moonBrightness", r.moonBrightness, out, at);
    ReadV3(*g, "moonColor", r.moonColor, out, at);
    ReadF(*g, "moonGlow", r.moonGlow, out, at);
    ReadF(*g, "moonEarthshine", r.moonEarthshine, out, at);
    ReadV3(*g, "moonLightColor", r.moonLightColor, out, at);
    ReadF(*g, "moonLightIntensity", r.moonLightIntensity, out, at);
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
    ReadF(*g, "heatSpillStrength", r.heatSpillStrength, out, at);
    ReadF(*g, "emberBrightness", r.emberBrightness, out, at);
    ReadF(*g, "emberRise", r.emberRise, out, at);
    ReadF(*g, "emberRate", r.emberRate, out, at);
    ReadI(*g, "emberDensity", r.emberDensity, out, at);
    ReadF(*g, "exposureWhite", r.exposureWhite, out, at);
    ReadF(*g, "bleachAmount", r.bleachAmount, out, at);
    ReadF(*g, "gamma", r.gamma, out, at);
    ReadF(*g, "microLodDist", r.microLodDist, out, at);
    ReadI(*g, "microMaxPerRay", r.microMaxPerRay, out, at);
    ReadF(*g, "microSwayAmp", r.microSwayAmp, out, at);
    ReadF(*g, "microSwaySpeed", r.microSwaySpeed, out, at);
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
    // starSize divides in the star PSF, starDensity scales the direction grid,
    // moonRadius divides when building the lunar disc frame, and skyMieG at
    // exactly +-1 makes the Henyey-Greenstein denominator collapse.
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
    if (r.moonRadius <= 0.0f) {
      out.warnings.push_back("render.moonRadius must be > 0; reset to 0.03");
      r.moonRadius = 0.03f;
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
    // The sway models are authored with a 2-sub-voxel wall margin; a larger
    // amplitude shears blade tips out of the cell where they simply vanish.
    if (r.microSwayAmp < 0.0f) { r.microSwayAmp = 0.0f; }
    if (r.microSwayAmp > 2.0f) { r.microSwayAmp = 2.0f; }
    if (r.microSwaySpeed < 0.0f) { r.microSwaySpeed = 0.0f; }
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
    ReadI(*g, "pondDepth", w.pondDepth, out, at);
    ReadI(*g, "pondDepthRim", w.pondDepthRim, out, at);
    ReadI(*g, "lilyChance", w.lilyChance, out, at);
    ReadI(*g, "lilyFlowerChance", w.lilyFlowerChance, out, at);
    ReadI(*g, "reedChance", w.reedChance, out, at);
    ReadI(*g, "reedHeight", w.reedHeight, out, at);
    ReadI(*g, "kelpChance", w.kelpChance, out, at);
    ReadI(*g, "kelpHeight", w.kelpHeight, out, at);
    ReadI(*g, "shoreBand", w.shoreBand, out, at);
    ReadI(*g, "shoreMudWidth", w.shoreMudWidth, out, at);
    ReadI(*g, "shoreLift", w.shoreLift, out, at);
    ReadI(*g, "shoreCattailChance", w.shoreCattailChance, out, at);
    ReadI(*g, "shoreCattailReach", w.shoreCattailReach, out, at);
    ReadI(*g, "shoreCattailHeight", w.shoreCattailHeight, out, at);
    ReadI(*g, "shoreSedgeChance", w.shoreSedgeChance, out, at);
    ReadI(*g, "shoreHorsetailChance", w.shoreHorsetailChance, out, at);
    ReadI(*g, "shoreHorsetailHeight", w.shoreHorsetailHeight, out, at);
    ReadI(*g, "shoreIrisChance", w.shoreIrisChance, out, at);
    ReadI(*g, "shoreMossChance", w.shoreMossChance, out, at);
    ReadI(*g, "vineChance", w.vineChance, out, at);
    ReadI(*g, "vineLenMin", w.vineLenMin, out, at);
    ReadI(*g, "vineLenSpan", w.vineLenSpan, out, at);
    ReadI(*g, "creeperFlowerChance", w.creeperFlowerChance, out, at);
    ReadI(*g, "mossChance", w.mossChance, out, at);
    ReadI(*g, "mossLenMin", w.mossLenMin, out, at);
    ReadI(*g, "mossLenSpan", w.mossLenSpan, out, at);
    ReadI(*g, "ivyChance", w.ivyChance, out, at);
    ReadI(*g, "ivyTwist", w.ivyTwist, out, at);
    ReadI(*g, "wallIvyDensity", w.wallIvyDensity, out, at);
    ReadI(*g, "cactusChance", w.cactusChance, out, at);
    ReadI(*g, "saguaroFraction", w.saguaroFraction, out, at);
    ReadI(*g, "tussockChance", w.tussockChance, out, at);
    ReadI(*g, "scrubChance", w.scrubChance, out, at);
    ReadI(*g, "desertPatch", w.desertPatch, out, at);
    ReadI(*g, "heathChance", w.heathChance, out, at);
    ReadI(*g, "heathPatch", w.heathPatch, out, at);
    ReadI(*g, "alpineChance", w.alpineChance, out, at);
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
    atLeast("lilyChance", w.lilyChance, 1);
    atLeast("lilyFlowerChance", w.lilyFlowerChance, 1);
    atLeast("reedChance", w.reedChance, 1);
    atLeast("kelpChance", w.kelpChance, 1);
    atLeast("pondDepthRim", w.pondDepthRim, 1);
    atLeast("pondDepth", w.pondDepth, w.pondDepthRim);
    // The bowl is carved DOWN from the water surface, and the cave system
    // starts 40 voxels under the terrain surface. A bowl deeper than that
    // breaches a tunnel, the pond drains into the cave network, and the world
    // never settles — which shows up as a sleep-gate failure a long way from
    // this file. 34 keeps a margin under the 40.
    if (w.pondDepth > 34) {
      out.warnings.push_back(
          "worldgen.pondDepth > 34 would breach the cave layer and drain the "
          "pond; clamped to 34");
      w.pondDepth = 34;
    }
    // Shoreline knobs. Every "chance" is a modulo divisor (zero is a
    // div-by-zero in the shader); the band has two ceilings of its own.
    atLeast("shoreBand", w.shoreBand, 0);      // 0 legally disables the fringe
    atLeast("shoreMudWidth", w.shoreMudWidth, 0);
    atLeast("shoreLift", w.shoreLift, 0);
    atLeast("shoreCattailChance", w.shoreCattailChance, 1);
    atLeast("shoreCattailHeight", w.shoreCattailHeight, 1);
    atLeast("shoreSedgeChance", w.shoreSedgeChance, 1);
    atLeast("shoreHorsetailChance", w.shoreHorsetailChance, 1);
    atLeast("shoreHorsetailHeight", w.shoreHorsetailHeight, 1);
    atLeast("shoreIrisChance", w.shoreIrisChance, 1);
    atLeast("shoreMossChance", w.shoreMossChance, 1);
    // shoreAt() resolves the distance past the rim by 8 steps of bisection over
    // [0, shoreBand], which is exact only while the band fits in 2^8.
    if (w.shoreBand > 255) {
      out.warnings.push_back(
          "worldgen.shoreBand > 255 exceeds shoreAt's 8-step bisection; "
          "clamped to 255");
      w.shoreBand = 255;
    }
    // shoreAt() checks the column's own pond tile plus AT MOST one neighbour
    // per axis, which is only sound while a column can be within `band` of one
    // tile edge at a time. Past pondTile/2 - 1 both edges of the same axis are
    // in reach and the far side's pond would be missed — a marsh sliced off
    // flat along a tile boundary. Half the tile is already absurdly wide
    // (224 voxels at the default), so this never bites real tunings.
    if (w.shoreBand > w.pondTile / 2 - 1) {
      out.warnings.push_back(
          "worldgen.shoreBand must stay under half of pondTile for shoreAt's "
          "2x2 tile scan; clamped");
      w.shoreBand = w.pondTile / 2 - 1;
      if (w.shoreBand < 0) w.shoreBand = 0;
    }
    // The mud ring lives inside the band; a wider one would just be clipped
    // silently, which reads as "shoreMudWidth stopped doing anything".
    if (w.shoreMudWidth > w.shoreBand) w.shoreMudWidth = w.shoreBand;
    if (w.shoreCattailReach > w.shoreBand) w.shoreCattailReach = w.shoreBand;
    atLeast("ruinChance", w.ruinChance, 1);
    atLeast("autumnFraction", w.autumnFraction, 1);
    // Vine/moss/ivy knobs: every "chance" is a modulo divisor and every "span"
    // a modulo range, so zero is a div-by-zero in the shader.
    atLeast("vineChance", w.vineChance, 1);
    atLeast("vineLenMin", w.vineLenMin, 1);
    atLeast("vineLenSpan", w.vineLenSpan, 1);
    atLeast("creeperFlowerChance", w.creeperFlowerChance, 1);
    atLeast("mossChance", w.mossChance, 1);
    atLeast("mossLenMin", w.mossLenMin, 1);
    atLeast("mossLenSpan", w.mossLenSpan, 1);
    atLeast("ivyChance", w.ivyChance, 1);
    atLeast("ivyTwist", w.ivyTwist, 0);
    // wallIvyDensity is the NUMERATOR of a coverage ramp (32/d and 48/d). At 0
    // it divides by zero; past 8 the integer division collapses to 4 and 6 and
    // the knob stops doing anything, so the useful range is 1..8.
    atLeast("wallIvyDensity", w.wallIvyDensity, 1);
    if (w.wallIvyDensity > 8) {
      out.warnings.push_back(
          "worldgen.wallIvyDensity > 8 has no further effect; clamped to 8");
      w.wallIvyDensity = 8;
    }
    // A strand that reaches the ground reads as a pillar and blocks a path the
    // player expected to be open. The shader holds it clear of the trunk's own
    // ground height, but a length past the tallest canopy is simply wasted
    // work on cells that can never be reached — cap it at a great oak's crown.
    if (w.vineLenMin + w.vineLenSpan > 160) {
      out.warnings.push_back(
          "worldgen.vineLenMin + vineLenSpan > 160 exceeds the tallest canopy; "
          "clamped");
      w.vineLenSpan = 160 - w.vineLenMin;
      if (w.vineLenSpan < 1) { w.vineLenMin = 159; w.vineLenSpan = 1; }
    }
  }

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
