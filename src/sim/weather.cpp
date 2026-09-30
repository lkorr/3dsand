#include "sim/weather.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "sim/rng.h"
#include "sim/tuning.h"
#include "sim/windfield.h"
#include "test/support.h"  // AssetDir(): the one asset-path chokepoint

namespace weather {
namespace {

// Salt for the weather-cycle stream. DISTINCT from kWindWeatherSalt and every
// worldgen salt (the rule wind.h states: two streams sharing bits correlate in
// a way nobody can find by looking). The corpus says the same thing about the
// fields themselves — "if you use one noise ... one weather pattern can only
// follow or be followed by a pair of two others" — which is why the lightning
// draw below takes its own component index rather than a slice of this one.
constexpr uint32_t kCycleSalt = 0xC10Du;
constexpr uint32_t kFlashSalt = 0xF1A5u;

float Unit(uint32_t seed, uint32_t a, uint32_t b) {
  return (float)(rng::Hash3(seed, a, b) & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

float Smooth(float x) { return x * x * (3.0f - 2.0f * x); }

// The built-in sky, used when assets/weather is missing. Deliberately the same
// numbers as the shipped JSON for `fair`, so a checkout without the folder
// looks like the default game and not like a debug fallback.
std::vector<Preset> BuiltIn() {
  Preset clear;
  clear.name = "clear"; clear.label = "Clear";
  clear.moisture = 0.0f; clear.weight = 1.0f;
  clear.coverage = 0.0f; clear.cirrus = 0.15f;
  Preset fair;
  fair.name = "fair"; fair.label = "Fair";
  fair.moisture = 0.2f; fair.weight = 2.0f;
  fair.coverage = 0.32f; fair.cloudType = 0.45f; fair.cirrus = 0.25f;
  return {clear, fair};
}

bool ReadPreset(const std::string& path, Preset& p, std::string& err) {
  std::ifstream f(path);
  if (!f) { err = "cannot open"; return false; }
  nlohmann::json j;
  try {
    f >> j;
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
  if (!j.is_object()) { err = "not an object"; return false; }
  auto num = [&](const char* k, float& dst, float lo, float hi) {
    auto it = j.find(k);
    if (it == j.end()) return;
    if (!it->is_number()) { err += std::string(k) + " not a number; "; return; }
    dst = std::clamp((float)it->get<double>(), lo, hi);
  };
  if (j.contains("label") && j["label"].is_string()) p.label = j["label"].get<std::string>();
  num("moisture", p.moisture, 0.0f, 1.0f);
  num("weight", p.weight, 0.0f, 100.0f);
  num("coverage", p.coverage, 0.0f, 1.0f);
  num("cloudType", p.cloudType, 0.0f, 1.0f);
  num("density", p.density, 0.0f, 8.0f);
  num("baseM", p.baseM, 50.0f, 12000.0f);
  num("thicknessM", p.thicknessM, 50.0f, 12000.0f);
  num("darkness", p.darkness, 0.0f, 1.0f);
  num("cirrus", p.cirrus, 0.0f, 1.0f);
  num("cirrusAltM", p.cirrusAltM, 1000.0f, 20000.0f);
  num("precip", p.precip, 0.0f, 1.0f);
  num("precipType", p.precipType, 0.0f, 1.0f);
  num("mist", p.mist, 0.0f, 20.0f);
  num("lightning", p.lightning, 0.0f, 60.0f);
  if (j.contains("wind") && j["wind"].is_string()) p.wind = j["wind"].get<std::string>();
  if (p.label.empty()) p.label = p.name;
  return true;
}

// ---- the moisture signal --------------------------------------------------
// Smooth 1-D value noise over EPOCH index: one hashed value per epoch knot,
// smoothstep between them. Two octaves so the walk has both a slow trend
// (a wet afternoon) and a nudge per epoch. Output 0..1.
float MoistureAt(uint32_t seed, double epochF) {
  auto oct = [&](double x, uint32_t comp) {
    const double fl = std::floor(x);
    const int64_t i = (int64_t)fl;
    const float f = Smooth((float)(x - fl));
    const float a = Unit(seed ^ kCycleSalt, (uint32_t)i, comp);
    const float b = Unit(seed ^ kCycleSalt, (uint32_t)(i + 1), comp);
    return a + (b - a) * f;
  };
  // Slow octave over ~3 epochs, fast one per epoch. Weighted so the extremes
  // (a cloudless day, a storm) are reachable but not typical.
  const float v = 0.65f * oct(epochF / 3.0, 1u) + 0.35f * oct(epochF, 2u);
  // Stretch: the sum of two uniforms bunches toward the middle, which would
  // make "clear" and "storm" near-impossible. A mild contrast curve spreads
  // it back toward the ends.
  const float c = std::clamp((v - 0.5f) * 1.45f + 0.5f, 0.0f, 1.0f);
  return c;
}

// Pick the pair of presets bracketing ladder position `m`, and the blend
// between them. The ladder is the presets in moisture order, each owning a
// run proportional to its weight; inside a run the preset is pure, and the
// last `edge` of each run blends into the next — so the automatic sky spends
// most of its time on authored looks and only passes through blends.
void Ladder(const std::vector<Preset>& ps, float m, int& a, int& b, float& t) {
  float total = 0.0f;
  for (const Preset& p : ps) total += p.weight;
  a = b = 0;
  t = 0.0f;
  if (total <= 0.0f || ps.empty()) return;
  float acc = 0.0f;
  const float x = m * total;
  int last = -1;
  for (int i = 0; i < (int)ps.size(); i++) {
    if (ps[i].weight <= 0.0f) continue;
    const float lo = acc, hi = acc + ps[i].weight;
    last = i;
    if (x <= hi) {
      // Find the next weighted preset up the ladder.
      int nx = -1;
      for (int k = i + 1; k < (int)ps.size(); k++)
        if (ps[k].weight > 0.0f) { nx = k; break; }
      const float u = (x - lo) / std::max(hi - lo, 1e-6f);
      const float edge = 0.5f;
      a = i;
      b = nx < 0 ? i : nx;
      t = nx < 0 ? 0.0f : Smooth(std::clamp((u - (1.0f - edge)) / edge, 0.0f, 1.0f));
      return;
    }
    acc = hi;
  }
  a = b = std::max(last, 0);
}

// The auto/pinned weather at sim time `ts`, before the override ease.
Preset Scheduled(const Tuning& tn, const std::vector<Preset>& ps, uint32_t seed,
                 double ts, std::string* from, std::string* to, float* blend) {
  const Tuning::Weather& w = tn.weather;
  if (!w.autoCycle || ps.size() < 2) {
    const Preset* p = &ps[0];
    for (const Preset& q : ps)
      if (q.name == w.preset) p = &q;
    if (from) *from = p->name;
    if (to) *to = p->name;
    if (blend) *blend = 0.0f;
    return *p;
  }
  const double epochS = std::max(0.5, (double)w.epochMinutes) * 60.0;
  const double e = ts * (double)std::max(0.0f, w.cycleSpeed) / epochS;
  const float m = MoistureAt(seed + (uint32_t)w.seedOffset, e);
  int a, b;
  float t;
  Ladder(ps, m, a, b, t);
  if (from) *from = ps[a].name;
  if (to) *to = ps[b].name;
  if (blend) *blend = t;
  return Lerp(ps[a], ps[b], t);
}

// Global rain actually reaching the ground: raininess only turns into rain
// under enough cloud, and at low raininess never (the corpus's three-field
// rule, plan §1c, evaluated for the dome as a whole).
float GroundPrecip(const Preset& p) {
  return std::clamp(p.precip * Smooth(std::clamp((p.coverage - 0.25f) / 0.5f, 0.0f, 1.0f)),
                    0.0f, 1.0f);
}

// Wetness: a leaky integral of past rain, evaluated as a sum. `now` is the
// preset standing over the ground this instant (the eased one for the
// renderer, the un-eased target for the sim); the history is the SCHEDULE, or
// the pin itself when one is set. See Resolve for why.
float WetnessAt(const Tuning& tn, const std::vector<Preset>& ps, uint32_t seed,
                double ts, const Preset* ov, const Preset& now) {
  const float dry = std::max(5.0f, tn.weather.drySeconds);
  // exp(-k/4), k = 1..8, as LITERALS: this sum feeds SimRainWord's wetness
  // byte, which is hashed sim input, and a CRT exp() is not guaranteed to
  // round the same on every CPU / runtime. Basic IEEE arithmetic is.
  static constexpr float kDecay[8] = {
      0.77880078f, 0.60653066f, 0.47236655f, 0.36787944f,
      0.28650480f, 0.22313016f, 0.17377394f, 0.13533528f};
  float acc = GroundPrecip(now), wsum = 1.0f;
  for (int k = 1; k <= 8; k++) {
    const double back = (double)k * dry / 4.0;
    const float wk = kDecay[k - 1];
    Preset p = ov ? *ov : Scheduled(tn, ps, seed, ts - back, nullptr, nullptr, nullptr);
    acc += GroundPrecip(p) * wk;
    wsum += wk;
  }
  // Snow does not wet the ground the way rain does (it sits on it); the
  // composite whitens instead, which is a later package.
  return std::clamp(acc / wsum * 1.6f, 0.0f, 1.0f) * (1.0f - now.precipType);
}

std::string gOverride;
bool gEnvPinRead = false;
uint32_t gLastSimWord = 0;
bool gRainLatched = false;
uint32_t gRainLatchTick = 0;
uint32_t gRainLatchWord = 0;
State gLast;
Preset gEased;
bool gEasedValid = false;

// ============================================================================
// THE SIM'S HALF, IN INTEGERS (SimRainWord; weather.h says why)
// ============================================================================
//
// Q16 fixed point throughout: 65536 = 1.0. Every float that enters is turned
// into an integer by ONE exact step — float -> double is exact, a multiply by
// 65536 (or by a whole tick count that fits the mantissa) is exact, and
// llround's half-away-from-zero is specified — so the quantised inputs are a
// pure function of the authored bits. Everything after that is int64 add,
// multiply, arithmetic shift and division, which no two machines disagree on.
// The structure mirrors the float path above step for step (MoistureAt,
// Ladder, Scheduled, GroundPrecip, WetnessAt), so the renderer's sky and the
// sim's rain are the same weather to within fixed-point rounding.

constexpr int64_t kQ = 65536;
// Ticks per second, from the one tick constant rather than a literal 30.
constexpr int64_t kTickHz = (int64_t)(1.0f / sandvox::kTickDt + 0.5f);
static_assert(kTickHz == 30, "weather's tick clock assumes the 30 Hz tick");

int64_t Q16(float v) { return (int64_t)std::llround((double)v * 65536.0); }
int64_t ClampQ(int64_t v) { return std::clamp<int64_t>(v, 0, kQ); }
// Floor division for a possibly negative numerator, positive denominator: the
// float path's std::floor, which C++'s truncating `/` is not below zero.
int64_t FloorDiv(int64_t a, int64_t b) {
  const int64_t q = a / b;
  return (a % b != 0 && a < 0) ? q - 1 : q;
}
// smoothstep's x*x*(3-2x) on a Q16 x in [0, 1].
int64_t SmoothQ(int64_t x) { return (x * x * (3 * kQ - 2 * x)) >> 32; }
// Unit() as Q16: the same 24 hashed bits, truncated to 16.
int64_t UnitQ(uint32_t seed, uint32_t a, uint32_t b) {
  return (int64_t)((rng::Hash3(seed, a, b) & 0x00FFFFFFu) >> 8);
}
int64_t LerpQ(int64_t a, int64_t b, int64_t t) { return a + (((b - a) * t) >> 16); }

// exp(-k/4) for k = 1..8, the wetness decay weights, as Q16 literals —
// round(exp(-k/4) * 65536). A table rather than std::exp: the one
// transcendental in the old path, and the reason this block exists.
constexpr int64_t kWetDecayQ[8] = {51039, 39750, 30957, 24109,
                                   18776, 14623, 11388, 8869};

// The weather.* tuning the sim word reads, quantised once per call.
struct SimTuneQ {
  bool autoCycle = false;
  int64_t epochTicks = 1;  // one weather epoch, in ticks (>= 15 s)
  int64_t speedQ = 0;      // cycleSpeed, Q16
  uint32_t seedOffset = 0;
  int64_t dryTicks = 1;    // drySeconds, in ticks (>= 5 s)
  int64_t precipScaleQ = 0;
  int64_t igniteDampQ = 0;
};

SimTuneQ QuantiseTuning(const Tuning& tn) {
  const Tuning::Weather& w = tn.weather;
  SimTuneQ q;
  q.autoCycle = w.autoCycle;
  q.epochTicks = std::max<int64_t>(
      1, std::llround((double)std::max(0.5f, w.epochMinutes) * 60.0 * (double)kTickHz));
  q.speedQ = Q16(std::max(0.0f, w.cycleSpeed));
  q.seedOffset = (uint32_t)w.seedOffset;
  q.dryTicks = std::max<int64_t>(
      1, std::llround((double)std::max(5.0f, w.drySeconds) * (double)kTickHz));
  q.precipScaleQ = Q16(w.precipScale);
  q.igniteDampQ = ClampQ(Q16(w.rainIgniteDamp));
  return q;
}

// MoistureAt, Q16 in and out. `eQ` is the epoch position in Q16 epochs.
int64_t MoistureQ(uint32_t seed, int64_t eQ) {
  auto oct = [&](int64_t xQ, uint32_t comp) {
    const int64_t i = xQ >> 16;  // floor: arithmetic shift of a signed value
    const int64_t f = SmoothQ(xQ - (i << 16));
    const int64_t a = UnitQ(seed ^ kCycleSalt, (uint32_t)i, comp);
    const int64_t b = UnitQ(seed ^ kCycleSalt, (uint32_t)(i + 1), comp);
    return LerpQ(a, b, f);
  };
  // 0.65 / 0.35 and the 1.45 contrast stretch, as Q16 (42598 + 22938 = 1.0).
  const int64_t v = (oct(FloorDiv(eQ, 3), 1u) * 42598 + oct(eQ, 2u) * 22938) >> 16;
  return ClampQ((((v - kQ / 2) * 95027) >> 16) + kQ / 2);
}

// Ladder, Q16: the preset pair bracketing moisture `m` and the blend.
void LadderQ(const std::vector<PresetQ>& ps, int64_t m, int& a, int& b, int64_t& t) {
  int64_t total = 0;
  for (const PresetQ& p : ps) total += p.weight;
  a = b = 0;
  t = 0;
  if (total <= 0 || ps.empty()) return;
  int64_t acc = 0;
  const int64_t x = (m * total) >> 16;
  int last = -1;
  for (int i = 0; i < (int)ps.size(); i++) {
    if (ps[i].weight <= 0) continue;
    const int64_t lo = acc, hi = acc + ps[i].weight;
    last = i;
    if (x <= hi) {
      int nx = -1;
      for (int k = i + 1; k < (int)ps.size(); k++)
        if (ps[k].weight > 0) { nx = k; break; }
      const int64_t u = ((x - lo) << 16) / std::max<int64_t>(hi - lo, 1);
      a = i;
      b = nx < 0 ? i : nx;
      // edge = 0.5: (u - (1 - edge)) / edge = (u - 0.5) * 2
      t = nx < 0 ? 0 : SmoothQ(ClampQ((u - kQ / 2) * 2));
      return;
    }
    acc = hi;
  }
  a = b = std::max(last, 0);
}

// Scheduled, Q16, at `tick` (signed: the wetness history reaches before 0).
// `pinned` is the tuning's fixed preset index when the cycle is off.
PresetQ ScheduledQ(const SimTuneQ& q, const std::vector<PresetQ>& ps, int pinned,
                   uint32_t seed, int64_t tick) {
  if (!q.autoCycle || ps.size() < 2) return ps[pinned];
  const int64_t eQ = FloorDiv(tick * q.speedQ, q.epochTicks);
  const int64_t m = MoistureQ(seed + q.seedOffset, eQ);
  int a, b;
  int64_t t;
  LadderQ(ps, m, a, b, t);
  PresetQ o;
  o.weight = LerpQ(ps[a].weight, ps[b].weight, t);
  o.coverage = LerpQ(ps[a].coverage, ps[b].coverage, t);
  o.precip = LerpQ(ps[a].precip, ps[b].precip, t);
  o.precipType = LerpQ(ps[a].precipType, ps[b].precipType, t);
  o.windI = LerpQ(ps[a].windI, ps[b].windI, t);
  o.windG = LerpQ(ps[a].windG, ps[b].windG, t);
  o.windC = LerpQ(ps[a].windC, ps[b].windC, t);
  return o;
}

int64_t GroundPrecipQ(const PresetQ& p) {
  return ClampQ((p.precip * SmoothQ(ClampQ((p.coverage - kQ / 4) * 2))) >> 16);
}

}  // namespace

Preset Lerp(const Preset& a, const Preset& b, float tIn) {
  const float t = std::clamp(tIn, 0.0f, 1.0f);
  auto L = [t](float x, float y) { return x + (y - x) * t; };
  Preset o = t < 0.5f ? a : b;
  o.moisture = L(a.moisture, b.moisture);
  o.weight = L(a.weight, b.weight);
  o.coverage = L(a.coverage, b.coverage);
  o.cloudType = L(a.cloudType, b.cloudType);
  o.density = L(a.density, b.density);
  o.baseM = L(a.baseM, b.baseM);
  o.thicknessM = L(a.thicknessM, b.thicknessM);
  o.darkness = L(a.darkness, b.darkness);
  o.cirrus = L(a.cirrus, b.cirrus);
  o.cirrusAltM = L(a.cirrusAltM, b.cirrusAltM);
  o.precip = L(a.precip, b.precip);
  o.precipType = L(a.precipType, b.precipType);
  o.mist = L(a.mist, b.mist);
  o.lightning = L(a.lightning, b.lightning);
  return o;
}

// SANDVOX_WEATHER=<preset> pins the sky for a whole run -- the headless handle
// for --shot / --verify look iteration, the same shape SANDVOX_SHORT_RANGE is.
// Read ONCE, as the initial override, so the dev panel can still change it;
// and EAGERLY -- at preset load or the first override access, whichever comes
// first -- so the tick stream sees the pin from tick 0, not from the first
// frame that happened to resolve the sky.
static void EnsureEnvPin() {
  if (gEnvPinRead) return;
  gEnvPinRead = true;
  if (const char* e = std::getenv("SANDVOX_WEATHER")) {
    if (e[0]) gOverride = e;
  }
}

void Library::EnsureLoaded() {
  if (loaded_) return;
  loaded_ = true;
  EnsureEnvPin();
  presets_.clear();
  warnings_.clear();
  namespace fs = std::filesystem;
  const fs::path dir = fs::path(sandvox::AssetDir()) / "weather";
  std::error_code ec;
  if (fs::is_directory(dir, ec)) {
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
      if (!ent.is_regular_file() || ent.path().extension() != ".json") continue;
      Preset p;
      p.name = ent.path().stem().string();
      std::string err;
      if (!ReadPreset(ent.path().string(), p, err)) {
        warnings_.push_back("weather/" + p.name + ".json: " + err);
        continue;
      }
      if (!err.empty()) warnings_.push_back("weather/" + p.name + ".json: " + err);
      presets_.push_back(p);
    }
  }
  if (presets_.empty()) {
    warnings_.push_back("assets/weather has no presets; using the built-in pair");
    presets_ = BuiltIn();
  }
  // Moisture order IS the ladder. Name breaks ties so two presets authored at
  // the same moisture land in a stable order on every machine.
  std::sort(presets_.begin(), presets_.end(), [](const Preset& a, const Preset& b) {
    if (a.moisture != b.moisture) return a.moisture < b.moisture;
    return a.name < b.name;
  });
  for (const std::string& w : warnings_) std::fprintf(stderr, "weather: %s\n", w.c_str());
  // Each sky's wind regime, by name (assets/wind/regimes.json). An unknown
  // name is a warning, not a failure: the sky keeps the breezy default.
  for (Preset& p : presets_) {
    const windfield::Regime* r = windfield::FindRegime(p.wind);
    if (r) {
      p.windIntensity = r->intensity;
      p.windGale = r->gale;
      p.windConvective = r->convective;
    } else {
      warnings_.push_back("weather/" + p.name + ".json: unknown wind regime '" + p.wind +
                          "' (assets/wind/regimes.json); using breezy");
      std::fprintf(stderr, "weather: %s\n", warnings_.back().c_str());
    }
  }
  // The sim's quantised copy, parallel and in the same (sorted) order.
  presetsQ_.clear();
  for (const Preset& p : presets_) {
    PresetQ q{Q16(p.weight), Q16(p.coverage), Q16(p.precip), Q16(p.precipType)};
    q.windI = Q16(p.windIntensity);
    q.windG = Q16(p.windGale);
    q.windC = Q16(p.windConvective);
    presetsQ_.push_back(q);
  }
}

const std::vector<Preset>& Library::Presets() {
  EnsureLoaded();
  return presets_;
}

const std::vector<PresetQ>& Library::PresetsQ() {
  EnsureLoaded();
  return presetsQ_;
}

const Preset* Library::Find(const std::string& name) {
  EnsureLoaded();
  for (const Preset& p : presets_)
    if (p.name == name) return &p;
  return nullptr;
}

void Library::Reload() {
  windfield::ReloadRegimes();
  loaded_ = false;
  EnsureLoaded();
  gEasedValid = false;
}

Library& Presets() {
  static Library lib;
  return lib;
}

void SetOverride(const std::string& name) {
  EnsureEnvPin();  // an explicit pin set first must not be clobbered later
  gOverride = name;
}
const std::string& Override() {
  EnsureEnvPin();
  return gOverride;
}

State Resolve(const Tuning& tn, uint32_t seed, double ts, float dt, bool commit) {
  EnsureEnvPin();
  State s;
  s.enabled = tn.weather.clouds;
  const std::vector<Preset>& ps = Presets().Presets();

  // ---- the target: the override if one is pinned, else the schedule ----
  Preset target;
  const Preset* ov = gOverride.empty() ? nullptr : Presets().Find(gOverride);
  if (ov) {
    target = *ov;
    s.fromName = s.toName = ov->name;
  } else {
    target = Scheduled(tn, ps, seed, ts, &s.fromName, &s.toName, &s.blend);
  }

  // ---- the manual ease ------------------------------------------------------
  // The schedule is already continuous in time, so it needs no ease; what does
  // is a CUT — the dev panel pinning a preset, or handing the sky back. That is
  // eased in FRAME time over transitionSeconds. A headless path passes dt = 0
  // and snaps, which is what makes --shot of a pinned preset show the preset.
  if (!commit) {
    // A peek: the main view's sky as it stands, nothing written.
    s.mix = gEasedValid ? gEased : target;
  } else if (!gEasedValid || dt <= 0.0f) {
    gEased = target;
    gEasedValid = true;
  } else {
    const float tau = std::max(0.05f, tn.weather.transitionSeconds);
    const float k = 1.0f - std::exp(-dt * 3.0f / tau);
    gEased = Lerp(gEased, target, k);
    gEased.name = target.name;
    gEased.label = target.label;
  }
  if (commit) s.mix = gEased;

  // Global knobs on top of whatever the sky is doing.
  s.mix.coverage = std::clamp(s.mix.coverage + tn.weather.coverageBias, 0.0f, 1.0f);
  s.mix.precip = std::clamp(s.mix.precip * tn.weather.precipScale, 0.0f, 1.0f);

  // How much of the dome is under cloud: coverage, but a thin sheet of
  // stratus overcasts the light more than the same coverage of cumulus with
  // blue between the towers, so type pulls it a little.
  s.overcast = std::clamp(s.mix.coverage * (1.1f - 0.25f * s.mix.cloudType) *
                          std::min(1.0f, 0.6f + 0.4f * s.mix.density), 0.0f, 1.0f);
  s.overcast = Smooth(s.overcast);

  // ---- wetness: a leaky integral of past rain, evaluated as a sum ----------
  // A pure function of the schedule: eight samples of the ground rain over
  // the last few minutes, weighted by exp decay. No integrator state, so a
  // replay and a fresh boot at the same tick agree on how wet it is. Uses the
  // SCHEDULED weather, not the eased override, except for "now", so pinning
  // rain in the dev panel wets the ground within one sample step rather than
  // waiting for history that never happened.
  s.wetness = WetnessAt(tn, ps, seed, ts, ov, s.mix);

  // ---- lightning: a pure function of time ----------------------------------
  // The clock is cut into 0.5 s slots; a slot flashes with the probability the
  // preset's rate implies, at a hashed moment inside it, and a flash is a
  // stroke plus up to two re-strikes (the flicker that makes lightning read as
  // lightning and not a camera flash). No state: the same tick flashes on
  // every machine, and a flash never "finishes" a frame late.
  if (s.mix.lightning > 0.0f && s.enabled) {
    const double slotS = 0.5;
    const float pSlot = std::min(0.9f, s.mix.lightning * (float)slotS / 60.0f);
    const int64_t cur = (int64_t)std::floor(ts / slotS);
    for (int64_t sl = cur - 3; sl <= cur; sl++) {
      const uint32_t us = (uint32_t)sl;
      if (Unit(seed ^ kFlashSalt, us, 0u) >= pSlot) continue;
      const double t0 = (double)sl * slotS + Unit(seed ^ kFlashSalt, us, 1u) * slotS;
      const float age = (float)(ts - t0);
      if (age < 0.0f || age > 1.2f) continue;
      // Stroke + re-strikes: three decaying pulses at hashed offsets.
      float f = 0.0f;
      for (uint32_t r = 0; r < 3; r++) {
        const float off = r == 0 ? 0.0f : 0.06f + Unit(seed ^ kFlashSalt, us, 2u + r) * 0.25f * r;
        const float a = age - off;
        if (a < 0.0f) continue;
        const float amp = r == 0 ? 1.0f : 0.55f * Unit(seed ^ kFlashSalt, us, 6u + r);
        f = std::max(f, amp * std::exp(-a * 18.0f));
      }
      if (f > s.flash) {
        s.flash = f;
        // Somewhere in the storm around the camera: 0.6..5 km out.
        const float ang = Unit(seed ^ kFlashSalt, us, 9u) * 6.2831853f;
        const float dist = 600.0f + Unit(seed ^ kFlashSalt, us, 10u) * 4400.0f;
        s.flashX = std::cos(ang) * dist;
        s.flashZ = std::sin(ang) * dist;
      }
    }
    s.flash *= std::clamp(s.mix.coverage * 1.5f - 0.3f, 0.0f, 1.0f);
  }
  if (!s.enabled) {
    s.overcast = 0.0f;
    s.wetness = 0.0f;
    s.flash = 0.0f;
  }
  if (commit) gLast = s;
  return s;
}

const State& Last() { return gLast; }

uint32_t SimRainWord(const Tuning& tn, uint32_t seed, uint32_t tick) {
  EnsureEnvPin();
  if (!tn.weather.clouds || !tn.weather.rainTouchesWorld) return 0u;
  // Integers only from here (the block above the anonymous namespace's end
  // says how the floats got in). The same steps as the renderer's
  // Scheduled / GroundPrecip / WetnessAt, on PresetQ.
  const std::vector<Preset>& ps = Presets().Presets();
  const std::vector<PresetQ>& pq = Presets().PresetsQ();
  const SimTuneQ q = QuantiseTuning(tn);
  // The tuning's fixed preset when the cycle is off: the LAST name match, as
  // Scheduled picks it, else the first preset.
  int pinned = 0;
  for (int i = 0; i < (int)ps.size(); i++)
    if (ps[i].name == tn.weather.preset) pinned = i;
  // The TARGET, never the frame-time ease: the ease is wall-clock and would
  // make the world hash depend on frame pacing. So a dev-panel pin rains on
  // the world at once while the sky takes transitionSeconds to catch up.
  const Preset* ov = gOverride.empty() ? nullptr : Presets().Find(gOverride);
  const PresetQ* ovq = ov ? &pq[(size_t)(ov - ps.data())] : nullptr;
  const int64_t t0 = (int64_t)tick;
  PresetQ now = ovq ? *ovq : ScheduledQ(q, pq, pinned, seed, t0);
  now.precip = ClampQ((now.precip * q.precipScaleQ) >> 16);
  // Rain only: snow neither douses nor soaks (it is a later package, as the
  // composite's whitening is).
  const int64_t gpNow = GroundPrecipQ(now);
  const int64_t rain = (gpNow * (kQ - now.precipType)) >> 16;
  // Wetness: WetnessAt's eight exp-weighted samples of the schedule, reaching
  // back k * drySeconds / 4 (the pin itself when one is set), plus `now` at
  // weight 1. The weighted mean is Q16; the x1.6 is 104858 / 65536.
  int64_t acc = gpNow * kQ, wsum = kQ;
  for (int k = 1; k <= 8; k++) {
    const int64_t back = ((int64_t)k * q.dryTicks) / 4;
    const PresetQ p = ovq ? *ovq : ScheduledQ(q, pq, pinned, seed, t0 - back);
    acc += GroundPrecipQ(p) * kWetDecayQ[k - 1];
    wsum += kWetDecayQ[k - 1];
  }
  const int64_t wet =
      (ClampQ(((acc / wsum) * 104858) >> 16) * (kQ - now.precipType)) >> 16;
  auto q8 = [](int64_t v) { return (uint32_t)((ClampQ(v) * 255 + kQ / 2) >> 16); };
  gLastSimWord = (q8(rain) & 0xFFu) | (q8(q.igniteDampQ) << 8) | (q8(wet) << 16);
  return gLastSimWord;
}

uint32_t LastSimRainWord() { return gLastSimWord; }

bool SimWindRegime(const Tuning& tn, uint32_t seed, uint32_t tick, int32_t& intensity,
                   int32_t& gale, int32_t& convective, int32_t& cover) {
  EnsureEnvPin();
  if (!tn.weather.clouds) return false;
  const std::vector<Preset>& ps = Presets().Presets();
  const std::vector<PresetQ>& pq = Presets().PresetsQ();
  if (ps.empty()) return false;
  const SimTuneQ q = QuantiseTuning(tn);
  int pinned = 0;
  for (int i = 0; i < (int)ps.size(); i++)
    if (ps[i].name == tn.weather.preset) pinned = i;
  const Preset* ov = gOverride.empty() ? nullptr : Presets().Find(gOverride);
  const PresetQ now = ov ? pq[(size_t)(ov - ps.data())]
                         : ScheduledQ(q, pq, pinned, seed, (int64_t)tick);
  intensity = (int32_t)ClampQ(now.windI);
  gale = (int32_t)ClampQ(now.windG);
  convective = (int32_t)ClampQ(now.windC);
  cover = (int32_t)ClampQ(now.coverage + Q16(tn.weather.coverageBias));
  return true;
}

uint64_t SimWindFingerprint(const Tuning& tn) {
  EnsureEnvPin();
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const void* p, size_t n) {
    const auto* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
  };
  const Tuning::Weather& w = tn.weather;
  const int64_t v[] = {w.clouds ? 1 : 0, w.autoCycle ? 1 : 0, Q16(w.epochMinutes),
                       Q16(w.cycleSpeed), (int64_t)w.seedOffset, Q16(w.coverageBias)};
  mix(v, sizeof v);
  mix(w.preset.data(), w.preset.size());
  mix(gOverride.data(), gOverride.size());
  for (const PresetQ& p : Presets().PresetsQ()) mix(&p, sizeof p);
  return h;
}

uint32_t LatchTickRain(const Tuning& tn, uint32_t seed, uint32_t tick) {
  gRainLatchWord = SimRainWord(tn, seed, tick);
  gRainLatchTick = tick;
  gRainLatched = true;
  return gRainLatchWord;
}

uint32_t TakeTickRain(const Tuning& tn, uint32_t seed, uint32_t tick) {
  if (gRainLatched && gRainLatchTick == tick) {
    gRainLatched = false;
    gLastSimWord = gRainLatchWord;
    return gRainLatchWord;
  }
  gRainLatched = false;  // a stale latch (a tick that never submitted) dies here
  return SimRainWord(tn, seed, tick);
}

void Snap() { gEasedValid = false; }

}  // namespace weather
