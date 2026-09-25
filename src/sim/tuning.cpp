#include "sim/tuning.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

// A .def row's WGSL column is NO_WGSL when no shader reads it. Stringised,
// never expanded, so NO_WGSL needs no definition anywhere.
bool HasWgsl(const char* name) { return std::strcmp(name, "NO_WGSL") != 0; }

// ---- generated read + clamp (tuning_params.def) --------------------------
// The clamp compares in the MEMBER's type, not in double: a JSON 3.1400001
// read into a float is exactly 3.14f, and a double comparison against 3.14
// would clamp it to itself and warn about a value that is in range. A bound
// of TP_OPEN (NaN) is skipped explicitly -- (int)NaN is undefined behaviour.
void ClampRowF(float& v, double lo, double hi, Tuning& t, const char* g,
               const char* m) {
  const bool hasLo = !std::isnan(lo), hasHi = !std::isnan(hi);
  if (hasLo && v < (float)lo) {
    t.warnings.push_back(std::string(g) + "." + m + " below " + F((float)lo) +
                         "; clamped");
    v = (float)lo;
  } else if (hasHi && v > (float)hi) {
    t.warnings.push_back(std::string(g) + "." + m + " above " + F((float)hi) +
                         "; clamped");
    v = (float)hi;
  }
}
void ClampRowI(int& v, double lo, double hi, Tuning& t, const char* g,
               const char* m) {
  const bool hasLo = !std::isnan(lo), hasHi = !std::isnan(hi);
  if (hasLo && v < (int)lo) {
    t.warnings.push_back(std::string(g) + "." + m + " below " +
                         std::to_string((int)lo) + "; clamped");
    v = (int)lo;
  } else if (hasHi && v > (int)hi) {
    t.warnings.push_back(std::string(g) + "." + m + " above " +
                         std::to_string((int)hi) + "; clamped");
    v = (int)hi;
  }
}

// Every row of the table: find its group object, read the key into the
// member, clamp to the row's range. Runs before LoadTuning's hand-written
// group blocks, so the hand rules (cross-field bounds, resets, enum gates)
// see generated values exactly as they used to see their own reads.
void ReadDefRows(const json& j, Tuning& out) {
#define TP_ROW_(g, m, READ)                                \
  if (const json* grp = Find(j, #g)) {                     \
    READ(*grp, #m, out.g.m, out, #g);                      \
  }
#define TP_F(g, m, n, d, lo, hi)                           \
  if (const json* grp = Find(j, #g)) {                     \
    ReadF(*grp, #m, out.g.m, out, #g);                     \
    ClampRowF(out.g.m, lo, hi, out, #g, #m);               \
  }
#define TP_I(g, m, n, d, lo, hi)                           \
  if (const json* grp = Find(j, #g)) {                     \
    ReadI(*grp, #m, out.g.m, out, #g);                     \
    ClampRowI(out.g.m, lo, hi, out, #g, #m);               \
  }
#define TP_U(g, m, n, d, lo, hi) TP_I(g, m, n, d, lo, hi)
#define TP_B(g, m, n, d) TP_ROW_(g, m, ReadB)
#define TP_S(g, m, n, d) TP_ROW_(g, m, ReadStr)
#define TP_V3(g, m, n, x, y, z) TP_ROW_(g, m, ReadV3)
#include "sim/tuning_params.def"
#undef TP_V3
#undef TP_S
#undef TP_B
#undef TP_U
#undef TP_I
#undef TP_F
#undef TP_ROW_
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
// Since W2-Q every knob has a row (NO_WGSL ones included), so player.*, gore.*
// and the rest are sweepable too. Deliberately NOT clamped to the row's range:
// a sweep is a dev differential and may want to step past the slider.
//
// TP_V3 and TP_S are skipped: a vec3 has no single float to sweep (and the
// grammar has no place to say which component), a string is not a number.
// TP_B takes nonzero = true.
bool SetTuningField(Tuning& t, const std::string& group,
                    const std::string& name, float value) {
#define TP_F(g, m, n, d, lo, hi) \
  if (group == #g && name == #m) { t.g.m = value; return true; }
#define TP_I(g, m, n, d, lo, hi) \
  if (group == #g && name == #m) { t.g.m = (int)value; return true; }
#define TP_U(g, m, n, d, lo, hi) TP_I(g, m, n, d, lo, hi)
#define TP_B(g, m, n, d) \
  if (group == #g && name == #m) { t.g.m = value != 0.0f; return true; }
#define TP_S(g, m, n, d)
#define TP_V3(g, m, n, x, y, z)
#include "sim/tuning_params.def"
#undef TP_V3
#undef TP_S
#undef TP_B
#undef TP_U
#undef TP_I
#undef TP_F
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

  // Every tuning_params.def row: read + clamp, generated. What follows is only
  // what a row cannot say.
  ReadDefRows(j, out);

  if (const json* g = Find(j, "thirdPerson")) {
    // HAND-WRITTEN: the rules a tuning_params.def row cannot state (a
    // bound that depends on another field, a reset-to-default, an enum
    // gate, a non-row read). Every plain read and constant clamp of this
    // group is generated from the .def by ReadDefRows(), which ran first.
    auto& c = out.thirdPerson;
    c.zoomMax = std::clamp(c.zoomMax, c.zoomMin, 8.0f);
  }

  if (const json* g = Find(j, "gear")) {
    // HAND-WRITTEN: the rules a tuning_params.def row cannot state (a
    // bound that depends on another field, a reset-to-default, an enum
    // gate, a non-row read). Every plain read and constant clamp of this
    // group is generated from the .def by ReadDefRows(), which ran first.
    auto& c = out.gear;
    // The ramp must have width, or a hardness exactly on the pair divides by
    // zero; an author who wants a hard cliff gets it from a narrow band.
    c.biteThroughHard = std::max(c.biteThroughHard, c.biteThroughSoft + 1e-3f);
  }

  if (const json* g = Find(j, "avatar")) {
    // HAND-WRITTEN: the rules a tuning_params.def row cannot state (a
    // bound that depends on another field, a reset-to-default, an enum
    // gate, a non-row read). Every plain read and constant clamp of this
    // group is generated from the .def by ReadDefRows(), which ran first.
    auto& a = out.avatar;
    // 0 disables the release. The ceiling is 180 minus the head cone: a band
    // wider than the reachable range would be fading a look that has not
    // started yet, so the neck would never reach its stop at any angle.
    a.headLookReleaseYaw =
        std::clamp(a.headLookReleaseYaw, 0.0f, 180.0f - a.headLookYaw);
  }

  if (const json* g = Find(j, "audio")) {
    // HAND-WRITTEN: the rules a tuning_params.def row cannot state (a
    // bound that depends on another field, a reset-to-default, an enum
    // gate, a non-row read). Every plain read and constant clamp of this
    // group is generated from the .def by ReadDefRows(), which ran first.
    auto& a = out.audio;
    const std::string at = "audio";
    // The hysteresis only works while off < on. Equal (or inverted) collapses
    // it to a single threshold and the loop chatters on and off every frame at
    // the boundary, so pull `off` down rather than trusting the author.
    if (a.bleedOffThreshold >= a.bleedOnThreshold) {
      out.warnings.push_back(
          at + ".bleedOffThreshold >= bleedOnThreshold; lowered to keep the "
               "loop from retriggering every frame");
      a.bleedOffThreshold = a.bleedOnThreshold * 0.5f;
    }
  }

  if (const json* g = Find(j, "gore")) {
    // HAND-WRITTEN: the rules a tuning_params.def row cannot state (a
    // bound that depends on another field, a reset-to-default, an enum
    // gate, a non-row read). Every plain read and constant clamp of this
    // group is generated from the .def by ReadDefRows(), which ran first.
    auto& e = out.gore;
    const std::string at = "gore";
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
    e.burnCapMidFraction =
        std::clamp(e.burnCapMidFraction, 0.01f, e.burnDeathFraction - 0.01f);
    // Only 2/3/4/6 are representable in the 2-bit scale field.
    if (e.microScale != 2 && e.microScale != 3 && e.microScale != 4 &&
        e.microScale != 6) {
      out.warnings.push_back("gore.microScale must be 2, 3, 4 or 6; using 4");
      e.microScale = 4;
    }
  }

  // ---- melee: the stroke driver's feel (game/melee.h MeleeTuning) -----------
  // Read straight into Tuning::Melee; game/melee.cpp ApplyMeleeTuning is what
  // turns this into a MeleeTuning (and what does the metres -> world voxel
  // conversion for the four keys whose names carry a unit).
  if (const json* g = Find(j, "melee")) {
    // HAND-WRITTEN: the rules a tuning_params.def row cannot state (a
    // bound that depends on another field, a reset-to-default, an enum
    // gate, a non-row read). Every plain read and constant clamp of this
    // group is generated from the .def by ReadDefRows(), which ran first.
    auto& e = out.melee;
    const std::string at = "melee";
    // Same ceiling law as avatar.headLookReleaseYaw: a band wider than the
    // reach past the cone would fade a swing that never reached its stop.
    e.aimReleaseYaw = std::clamp(e.aimReleaseYaw, 0.0f, 180.0f - e.aimYaw);
    if (e.minSpeedMps >= e.fullSpeedMps) {
      out.warnings.push_back(at +
                             ".minSpeedMps >= fullSpeedMps; the damage ramp "
                             "has no band. Pulling min down to 0.5x full.");
      e.minSpeedMps = e.fullSpeedMps * 0.5f;
    }
    // ONLY THE SIGN IS READ (melee.h), so normalise it here rather than letting
    // a 0 in the JSON become an unreadable "the hand neither leads nor trails".
    e.handLead = e.handLead < 0.0f ? -1.0f : 1.0f;
    if (e.steerSpeedHiMps <= e.steerSpeedLoMps) {
      out.warnings.push_back(at +
                             ".steerSpeedHiMps <= steerSpeedLoMps; the wrist "
                             "ramp has no band. Pushing high to 2x low.");
      e.steerSpeedHiMps = std::max(e.steerSpeedLoMps * 2.0f, 0.05f);
    }
  }

  if (const json* g = Find(j, "sim")) {
    // HAND-WRITTEN: the rules a tuning_params.def row cannot state (a
    // bound that depends on another field, a reset-to-default, an enum
    // gate, a non-row read). Every plain read and constant clamp of this
    // group is generated from the .def by ReadDefRows(), which ran first.
    auto& s = out.sim;
    // A clamp whose bound is computed, not a literal (windGasScale below).
    auto clampWarnF = [&](float& v, float lo, float hi, const char* name) {
      if (!(v >= lo) || v > hi) {  // !(>=) also catches NaN
        out.warnings.push_back(std::string("sim.") + name + " out of range; clamped");
        v = !(v >= lo) ? lo : hi;
      }
    };
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
    if (s.waterBodySpreadExit <= s.waterBodySpreadEnter) {
      out.warnings.push_back(
          "sim.waterBodySpreadExit must be ABOVE sim.waterBodySpreadEnter "
          "(hysteresis); set to enter + 3");
      s.waterBodySpreadExit = s.waterBodySpreadEnter + 3;
    }
    // ---- W2: surface momentum (docs/PLAN_water_relevel.md §4.2) ---------
    // The mode gate FIRST, and for windMode's reason: an unknown value must
    // not fall through to "some wave", because the whole identity argument is
    // that 0 means the pass row is never recorded.
    if (s.waveMode < 0 || s.waveMode > 1) {
      out.warnings.push_back("sim.waveMode not 0 or 1; clamped to 0 (off)");
      s.waveMode = 0;
    }
    // W3. All three are Q8 pipe flux and all three are OFF at 0, which is the
    // documented identity, so only the negative side is a typo. The ceilings are
    // generous rather than tuned: the outflow clamp in wbFlux is the real bound
    // on what any of them can move, and a knob whose value the clamp eats is
    // merely useless rather than dangerous. 65536 is 256 whole eighths/tick,
    // two orders past anything the clamp will pass.
    {
      const int kWaveImpulseMax = 65536;
      auto clampWave = [&](int& v, const char* name) {
        if (v < 0 || v > kWaveImpulseMax) {
          out.warnings.push_back(std::string("sim.") + name +
                                 " out of 0..65536; clamped");
          v = v < 0 ? 0 : kWaveImpulseMax;
        }
      };
      clampWave(s.waveBlastImpulse, "waveBlastImpulse");
      clampWave(s.waveDrainSink, "waveDrainSink");
      clampWave(s.waveSwimWake, "waveSwimWake");
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
    // Dev force multipliers. The ceiling is the one the Q8 arithmetic in the
    // kernels is sized for (kWindScaleMax); 0 is a legal "pin this tier still"
    // and is genuinely useful, since it isolates one tier from the other.
    const float kScaleMax = (float)kWindScaleMax / (float)kWindScaleOne;
    clampWarnF(s.windGasScale, 0.0f, kScaleMax, "windGasScale");
    clampWarnF(s.windPartScale, 0.0f, kScaleMax, "windPartScale");
  }

  if (const json* g = Find(j, "dayNight")) {
    // HAND-WRITTEN: the rules a tuning_params.def row cannot state (a
    // bound that depends on another field, a reset-to-default, an enum
    // gate, a non-row read). Every plain read and constant clamp of this
    // group is generated from the .def by ReadDefRows(), which ran first.
    auto& d = out.dayNight;
    if (d.freezePhase < 0 || d.freezePhase > 65535) {
      out.warnings.push_back("dayNight.freezePhase outside 0..65535; wrapped");
      d.freezePhase = ((d.freezePhase % 65536) + 65536) % 65536;
    }
    if (d.twilightWidth <= 0.0f) {
      out.warnings.push_back("dayNight.twilightWidth must be > 0; reset to 0.22");
      d.twilightWidth = 0.22f;
    }
    // Latitude past the poles is a wrapped angle, not an error the user meant.
    if (d.latitudeDeg < -89.0f || d.latitudeDeg > 89.0f) {
      out.warnings.push_back("dayNight.latitudeDeg outside +-89; clamped");
      d.latitudeDeg = d.latitudeDeg < 0.0f ? -89.0f : 89.0f;
    }
  }

  if (const json* g = Find(j, "render")) {
    // HAND-WRITTEN: the rules a tuning_params.def row cannot state (a
    // bound that depends on another field, a reset-to-default, an enum
    // gate, a non-row read). Every plain read and constant clamp of this
    // group is generated from the .def by ReadDefRows(), which ran first.
    auto& r = out.render;
    const std::string at = "render";
    // smoothstep with low >= high is undefined-ish (it degenerates to a step
    // at best); keep a real band so the crossover onto water's coefficients
    // stays a blend rather than a cliff.
    if (r.subClearHigh <= r.subClearLow) {
      out.warnings.push_back(
          at + ".subClearHigh <= subClearLow; widened to keep a blend band");
      r.subClearHigh = r.subClearLow + 0.05f;
    }
    // Same smoothstep hazard as the clarity band above: low >= high collapses
    // the blend into a hard switch, so a liquid authored near the boundary
    // would flip between the blood and oil looks instead of crossing over.
    if (r.oilSatHigh <= r.oilSatLow) {
      out.warnings.push_back(
          at + ".oilSatHigh <= oilSatLow; widened to keep a blend band");
      r.oilSatHigh = r.oilSatLow + 0.05f;
    }
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
    if (r.farPlumeHeight > (float)kWorldN * kVoxelMeters) {
      r.farPlumeHeight = (float)kWorldN * kVoxelMeters;
    }
    {
      const float maxM =
          (float)((kGasFarOuterN << kGasFarOuterShift) / 2) * kVoxelMeters;
      if (r.farPlumeRange > maxM) r.farPlumeRange = maxM;
    }
    if (r.denoisePxStart < r.denoisePxFull + 0.01f) {
      r.denoisePxStart = r.denoisePxFull + 0.01f;
    }
    if (r.presentMode < 0 || r.presentMode > 2) { r.presentMode = 1; }
    if (r.giFeedback > 0.0f && r.giFeedback >= r.giDecay) {
      out.warnings.push_back(
          "render.giFeedback must be below render.giDecay (multi-bounce would "
          "brighten without bound); clamped");
      r.giFeedback = std::max(0.0f, r.giDecay * 0.5f);
    }
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
    // The glow field (world.h kGlowBytes). glowReach is the one value here that
    // is not merely a taste knob: sim_glow's gather stencil is 3x3x3 CHUNKS, so
    // the NEAREST source it does not read sits 2 chunks from the block's own
    // chunk centre, less the furthest a block centre can lean toward it
    // (kChunk/2 - block/2). Past that distance the kernel is TRUNCATED rather
    // than falling to zero, and a truncated falloff is a hard step in the light
    // at a chunk boundary -- the same class of artifact the LOD seam work spent
    // a package on. Widening the reach means widening the stencil (a 5^3 gather
    // is 125 taps, not 27), so this is a clamp with a warning rather than a
    // silent one.
    {
      const float kMaxReach =
          (float)(2 * (int)kChunk -
                  ((int)kChunk / 2 - (int)(1u << kSubOccShift) / 2)) *
          kVoxelMeters;
      if (r.glowReach < 0.0f) { r.glowReach = 0.0f; }
      if (r.glowReach > kMaxReach) {
        out.warnings.push_back(
            "render.glowReach past the 3x3x3 chunk gather stencil truncates the "
            "falloff (a hard step at a chunk boundary); clamped");
        r.glowReach = kMaxReach;
      }
    }
    r.glowTerrain = r.glowTerrain ? 1 : 0;
  }

  // ---- world / debug: what is left of the old `worldgen` group (P-G) ---------
  // The terrain numbers are the map's (worldmap.cpp LoadWorldMap reads and
  // rescales map.json `terrain`); the per-biome relief is the biome files'.
  if (const json* g = Find(j, "world")) {
    // HAND-WRITTEN: the rules a tuning_params.def row cannot state (a
    // bound that depends on another field, a reset-to-default, an enum
    // gate, a non-row read). Every plain read and constant clamp of this
    // group is generated from the .def by ReadDefRows(), which ran first.
    auto& w = out.world;
    if (w.mapLayer.empty() || w.mapLayer.find_first_of("/\\:") != std::string::npos) {
      out.warnings.push_back("world.mapLayer must be a bare map name; using \"default\"");
      w.mapLayer = "default";
    }
    if (w.editLayer.find_first_of("/\\:") != std::string::npos) {
      out.warnings.push_back("world.editLayer must be a bare layer name; ignored");
      w.editLayer.clear();
    }
  }
  if (const json* g = Find(j, "debug")) {
    // HAND-WRITTEN: the rules a tuning_params.def row cannot state (a
    // bound that depends on another field, a reset-to-default, an enum
    // gate, a non-row read). Every plain read and constant clamp of this
    // group is generated from the .def by ReadDefRows(), which ran first.
    auto& d = out.debug;
    d.vegetation = (d.vegetation != 0) ? 1 : 0;   // a flag: anything nonzero is on
    d.groundCover = (d.groundCover != 0) ? 1 : 0;
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
    put("bodyClearM", m.bodyClearM);
    put("leanFlipHold", m.leanFlipHold);
    put("leanMinSpeed", m.leanMinSpeed);
    put("flatMinSin", m.flatMinSin);
    put("aimYaw", m.aimYaw);
    put("aimReleaseYaw", m.aimReleaseYaw);
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
    putB("hitReact", f.hitReact);
    put("hitReactRefDamage", f.hitReactRefDamage);
    put("hitReactMaxScale", f.hitReactMaxScale);
    put("hitReactLeanDeg", f.hitReactLeanDeg);
    put("hitReactSpineShare", f.hitReactSpineShare);
    put("hitReactPushFrac", f.hitReactPushFrac);
    put("hitReactLimbDeg", f.hitReactLimbDeg);
    put("hitReactHalflife", f.hitReactHalflife);
    put("hitReactImpulse", f.hitReactImpulse);
    put("whooshVolume", f.whooshVolume);
    put("whooshMinSpeed", f.whooshMinSpeed);
    put("whooshRateSlow", f.whooshRateSlow);
    put("whooshRateFast", f.whooshRateFast);
    put("whooshEdgeFrac", f.whooshEdgeFrac);
    put("whooshPan", f.whooshPan);
    put("fleshVolume", f.fleshVolume);
    put("clangVolume", f.clangVolume);
    put("strikeEdgeVolume", f.strikeEdgeVolume);
    put("strikeBluntVolume", f.strikeBluntVolume);
    put("cutVolume", f.cutVolume);
    put("cueRadius", f.cueRadius);
  }
  // GORE TOO, because the Combat panel's Damage tab edits it. A Save button
  // that silently declined to save one of its own three tabs is the worst kind
  // of bug: the work is gone and nothing said so. GEAR TOO, because the
  // Damage tab's "Armour vs blunt/bite" section edits it.
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
    put("woundStainBlob", g.woundStainBlob);
    put("woundStainCoherence", g.woundStainCoherence);
    put("craterStainRim", g.craterStainRim);
    putB("woundHeals", g.woundHeals);
    put("woundHealSlow", g.woundHealSlow);
    put("corpseBleedPerVoxel", g.corpseBleedPerVoxel);
    put("corpseJointHold", g.corpseJointHold);
    put("corpseJointCut", g.corpseJointCut);
    put("bleedGain", g.bleedGain);
    put("bleedHpPerVoxel", g.bleedHpPerVoxel);
    putB("stumpBleedsOpen", g.stumpBleedsOpen);
    put("burnCapMidFraction", g.burnCapMidFraction);
    put("burnCapMidHealth", g.burnCapMidHealth);
    put("burnDeathFraction", g.burnDeathFraction);
    // E6: blunt / bruise / bite — the panel's new "Blunt and bruise" + "Bite"
    put("bruiseRadius", g.bruiseRadius);
    put("bruiseStep", g.bruiseStep);
    put("bruiseMax", g.bruiseMax);
    put("bruiseBleedChance", g.bruiseBleedChance);
    put("bruiseBleedFrom", g.bruiseBleedFrom);
    put("bruiseHpRef", g.bruiseHpRef);
    put("bruiseHpFloor", g.bruiseHpFloor);
    put("bluntBleedScale", g.bluntBleedScale);
    put("bluntCarveRadius", g.bluntCarveRadius);
    put("pulpAmt", g.pulpAmt);
    put("pulpCarveFrom", g.pulpCarveFrom);
    put("pulpRotRate", g.pulpRotRate);
    put("unarmedBruiseRadius", g.unarmedBruiseRadius);
    put("unarmedBruiseStep", g.unarmedBruiseStep);
    put("unarmedBleedChance", g.unarmedBleedChance);
    put("unarmedBleedScale", g.unarmedBleedScale);
    put("unarmedCarveRadius", g.unarmedCarveRadius);
    put("unarmedPulpCarveFrom", g.unarmedPulpCarveFrom);
    put("biteRadius", g.biteRadius);
    put("biteBlob", g.biteBlob);
    put("biteStainScale", g.biteStainScale);
    put("infectHealSlow", g.infectHealSlow);
    put("infectSpreadRate", g.infectSpreadRate);
    put("infectRotRate", g.infectRotRate);
    put("infectMobMult", g.infectMobMult);
    put("infectBoneStain", g.infectBoneStain);
    put("infectBoneStainVary", g.infectBoneStainVary);
    put("infectBoneIchor", g.infectBoneIchor);
    put("brainHpPerVoxel", g.brainHpPerVoxel);
    put("carveHpPerVolume", g.carveHpPerVolume);
    put("blastPowerRef", g.blastPowerRef);
    put("contactImpulseMin", g.contactImpulseMin);
    put("contactHpPerImpulse", g.contactHpPerImpulse);
    put("contactMaxHp", g.contactMaxHp);
    putI("contactMaxPerTick", g.contactMaxPerTick);
  }
  if (group("gear", lo, hi)) {
    const Tuning::Gear& gr = t.gear;
    put("ruinedCondition", gr.ruinedCondition);
    put("cutHardnessRef", gr.cutHardnessRef);
    put("cutHardnessMin", gr.cutHardnessMin);
    put("bluntDentRadius", gr.bluntDentRadius);
    put("bluntHardnessRef", gr.bluntHardnessRef);
    put("bluntHardnessMin", gr.bluntHardnessMin);
    put("bluntThrough", gr.bluntThrough);
    put("bluntShellHp", gr.bluntShellHp);
    put("biteOnShell", gr.biteOnShell);
    put("biteThroughSoft", gr.biteThroughSoft);
    put("biteThroughHard", gr.biteThroughHard);
    put("blastShellCells", gr.blastShellCells);
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
  // sim/tuning_params.def, which scripts/tuning_prelude.py also parses, so it
  // is not possible to emit a constant the offline validator does not know
  // about, or vice versa. NO_WGSL rows (and every bool/string row) emit
  // nothing: they are CPU knobs, and a const no shader names would only be
  // stripped again by ReferencedTuningBlock.
#define TP_F(g, m, n, d, lo, hi) if (HasWgsl(#n)) EmitF(o, #n, t.g.m);
#define TP_I(g, m, n, d, lo, hi) if (HasWgsl(#n)) EmitI(o, #n, t.g.m);
#define TP_U(g, m, n, d, lo, hi) if (HasWgsl(#n)) EmitU(o, #n, t.g.m);
#define TP_B(g, m, n, d)
#define TP_S(g, m, n, d)
#define TP_V3(g, m, n, x, y, z) if (HasWgsl(#n)) EmitV3(o, #n, t.g.m);
#include "sim/tuning_params.def"
#undef TP_V3
#undef TP_S
#undef TP_B
#undef TP_U
#undef TP_I
#undef TP_F

  return o.str();
}
