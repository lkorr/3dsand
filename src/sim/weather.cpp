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
std::string gEasedTarget;

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
}

const std::vector<Preset>& Library::Presets() {
  EnsureLoaded();
  return presets_;
}

const Preset* Library::Find(const std::string& name) {
  EnsureLoaded();
  for (const Preset& p : presets_)
    if (p.name == name) return &p;
  return nullptr;
}

void Library::Reload() {
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

State Resolve(const Tuning& tn, uint32_t seed, double ts, float dt, float camXM,
              float camZM, bool commit) {
  (void)camXM;
  (void)camZM;
  EnsureEnvPin();
  State s;
  s.enabled = tn.weather.clouds;
  const std::vector<Preset>& ps = Presets().Presets();

  // ---- the target: the override if one is pinned, else the schedule ----
  Preset target;
  std::string targetKey;
  const Preset* ov = gOverride.empty() ? nullptr : Presets().Find(gOverride);
  if (ov) {
    target = *ov;
    s.fromName = s.toName = ov->name;
    targetKey = "ov:" + ov->name;
  } else {
    target = Scheduled(tn, ps, seed, ts, &s.fromName, &s.toName, &s.blend);
    targetKey = "sched";
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
  (void)targetKey;
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
  const std::vector<Preset>& ps = Presets().Presets();
  const double ts = (double)tick / 30.0;
  // The TARGET, never the frame-time ease: the ease is wall-clock and would
  // make the world hash depend on frame pacing. So a dev-panel pin rains on
  // the world at once while the sky takes transitionSeconds to catch up.
  const Preset* ov = gOverride.empty() ? nullptr : Presets().Find(gOverride);
  Preset now = ov ? *ov : Scheduled(tn, ps, seed, ts, nullptr, nullptr, nullptr);
  now.precip = std::clamp(now.precip * tn.weather.precipScale, 0.0f, 1.0f);
  // Rain only: snow neither douses nor soaks (it is a later package, as the
  // composite's whitening is).
  const float rain = GroundPrecip(now) * (1.0f - now.precipType);
  const float wet = WetnessAt(tn, ps, seed, ts, ov, now);
  auto q8 = [](float v) {
    return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
  };
  gLastSimWord = (q8(rain) & 0xFFu) | (q8(tn.weather.rainIgniteDamp) << 8) |
                 (q8(wet) << 16);
  return gLastSimWord;
}

uint32_t LastSimRainWord() { return gLastSimWord; }

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
