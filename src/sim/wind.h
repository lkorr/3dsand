#pragma once
#include <cstdint>

#include "sim/intmath.h"  // the integer sine/sqrt this file's weather runs on
#include "sim/rng.h"
#include "sim/tuning.h"
#include "sim/world.h"   // kVoxelMeters — the one m -> cell conversion

// wind.h — the WEATHER half of the wind system.
//
// docs/RESEARCH_wind.md is the plan of record (§13 is the weather-driven
// model of 2026-09-30); DESIGN.md §9b states the invariants. Read the
// two-line version: wind is a pure function `windAt(worldPos, t)` living in
// common.wgsl, and everything about it is either a compile-time TUNE_*
// constant, one of the weather words this file produces, or the wf* field
// block windfield.h fills from them.
//
// WHY THIS EXISTS AT ALL, given that the field is a shader function. Because
// weather has to DRIFT. A gust band's wavelength is authored once and folded
// into the shader; the direction the wind is blowing has to wander over
// minutes, and no compile-time constant does that. So the evolving part —
// direction, mean speed, gust amplitude, and since 2026-09-30 the REGIME that
// sets the field's character (ground coupling, meander, thermals, terrain
// response, storm timeline) — is computed here, once per tick.
//
// WHY IT IS ONE FUNCTION AND NOT TWO. `windAtQ` is fed from TickParams, and
// TickParams is a determinism input: a replay reproduces it, the twice-run
// gate compares it. If the renderer and the sim each derived their own
// weather, grass and smoke would blow different ways in the same frame and the
// bug would look like a shader problem. So `WindWeatherQ` is the ONE author of
// these values for both UBOs — the same arrangement `dayPhase` already has.
//
// DETERMINISM. `WindWeatherQ` is a pure function of (tuning, seed, tick, the
// day phase, the sky's integer schedule). It holds no state, integrates
// nothing, and cannot drift: asking for tick 90000 costs the same as tick 1.
//
// INTEGER, END TO END. Once `sim.windMode` shipped at 1 these words became a
// per-tick INPUT to the CA, so a single machine rounding one of them
// differently would desync a session silently (docs/PLAN_multiplayer_now.md
// L6). Everything from the draws to the words is integer arithmetic using
// sim/intmath.h's exact BAM sine, sqrt and log2; float knobs cross into it by
// one exact multiply each. `WindWeather` below is a VIEW that divides the
// integers out into the floats RenderParams wants.
//
// ONE WEATHER. The regime — intensity, gale, convective — comes from the SKY
// by default (weather::SimWindRegime: each sky preset names a wind regime,
// assets/wind/regimes.json, and the sky's integer ladder blends them), so a
// thunderstorm sky has a thunderstorm wind and a fog bank is calm. The hook
// below lets any future weather system replace that source.

// One epoch of weather, as a power-of-two run of ticks. 2^11 = 2048 ticks =
// ~68 s at 30 Hz: long enough that the wind reads as a mood rather than a
// wobble, short enough that standing still for a minute shows you a change.
// A shift rather than a divide so the epoch boundary is exact at any tick.
constexpr uint32_t kWindEpochShift = 11;

// Salt for the weather RNG stream. DISTINCT, never a bit-slice of an existing
// stream (the worldgen salt rule — common.wgsl's hash3 block). Two streams that
// share bits are correlated in a way that shows up as the wind veering every
// time worldgen happens to draw, which is unfindable by inspection.
constexpr uint32_t kWindWeatherSalt = 0xAE01u;

// Chance an epoch is a STORM when no sky drives the regime (weather.clouds
// off): one in eight, against the 24-bit draw (2^21 / 2^24 = 1/8).
constexpr int32_t kWindStormChanceQ24 = 2097152;

// The epoch draws and the two 0..1 weather scalars live in Q24.
constexpr int32_t kWQOne = 16777216;  // 1.0 in Q24

// "No day phase": neutral stability. The advection clock samples the weather
// with this, because the speed the fronts ride must not depend on the day
// (the day phase can be driven by the dev time slider, whose history is not a
// function of the tick).
constexpr uint32_t kWindNoDayPhase = 0xFFFFFFFFu;

// ---- THE REGIME, and the hook that sets it ---------------------------------
//
// Three numbers describe the wind's CHARACTER, all Q16 (65536 = 1.0):
//   intensity   0..1, roughly Beaufort / 12: sets the mean speed (through the
//               wind.speed* curve), gustiness, meander, terrain response.
//   gale        weight of the sustained-gale character: steady direction,
//               gust factor ~1.5.
//   convective  weight of the thunderstorm character: lull, gust front
//               (direction jump + 2-3x speed spike), gusty decay, downbursts.
// plus the sky cover the stability term reads (cloud damps convection and
// radiative cooling alike).
struct WeatherRegime {
  int32_t intensity = 19661;  // 0.3: the reference, where the speed curve = 1
  int32_t gale = 0;
  int32_t convective = 0;
  int32_t cover = 19661;
};

// THE HOOK. A weather system that wants to drive the wind — rain, an event, a
// scripted storm — installs a source here. It MUST be a pure function of its
// arguments (and of whatever it reads that is itself on the tick stream):
// WindWeatherQ is sampled at arbitrary past ticks by the advection clock and
// by replays, so a source with memory desyncs the sim. Return false for "no
// opinion" and the built-in chain continues (the sky, then the wind's own
// epochs). nullptr restores the default (the sky, weather::SimWindRegime).
// Tuning's wind.regime / wind.intensity pins still override whatever the
// source says — they are the manual controls.
using WindRegimeSource = bool (*)(const Tuning& t, uint32_t seed, uint32_t tick,
                                  WeatherRegime& out);
void SetWindRegimeSource(WindRegimeSource src);

// ---- the SIM's copy: the weather as integers (research doc §4.2) -----------
// windAtQ (common.wgsl) is integer end to end, so the numbers it starts from
// have to BE integers. The first four ride TickParams directly; the regime
// section below is turned into the wf* block by windfield.h.
struct WindStateQ {
  int32_t dirX = 0, dirZ = 65536;  // unit XZ downwind, Q16.16
  int32_t speed = 0;               // mean at the reference height, cells/s Q16.16
  int32_t gust = 0;                // gust band amplitude, cells/s Q16.16
  // The raw epoch draws behind the mood, Q16.16, for the dev overlay.
  int32_t speed01 = 32768, gust01 = 32768;
  bool storm = false;              // convective or gale weight past one half

  // ---- the regime, resolved (all Q16 unless stated) ----
  uint8_t source = 0;              // kWindSrc*: who set the regime
  int32_t intensity = 19661;
  int32_t gale = 0, convective = 0, cover = 19661;
  int32_t stability = 0;           // [-1, 1]: stable night .. convective day,
                                   // after mixing by the wind
  int32_t gustFrac = 0;            // gust amplitude / mean
  int32_t coupling = 65536;        // surface coupling (1 = well mixed)
  int32_t wanderAmp = 0;           // max heading deviation, BAM16
  int32_t thermal = 0;             // isotropic thermal gust, cells/s Q16.16
  int32_t ridgeGain = 0, valleyGain = 0;
  int32_t lee = 0;                 // lee turbulence strength
  int32_t slopeWind = 0;           // up(+)/down(-)slope wind, cells/s Q16.16
  int32_t seaBreeze = 0;           // onshore(+)/offshore(-), cells/s Q16.16
  // ---- the convective storm timeline (stage 4; zero outside a storm) ----
  int32_t stormPhase = -1;         // Q16 position in the storm cycle; -1 none
  uint32_t stormCycle = 0;         // which cycle
  int32_t envelope = 65536;        // speed multiplier the timeline applied
  uint32_t jumpBam = 0;            // direction jump the timeline applied, BAM32
};

// Who set the regime (WindStateQ::source).
enum : uint8_t {
  kWindSrcManual = 0,   // weatherAuto off: the knobs
  kWindSrcSky = 1,      // the sky's preset ladder (or an installed source)
  kWindSrcEpochs = 2,   // no sky: the wind's own epoch draws
  kWindSrcPreset = 3,   // wind.regime pins a named regime
};

// The evolving weather, resolved for one tick, as the RENDERER wants it.
// Every field is a division of the WindStateQ above — there is no second
// derivation and no second rounding.
struct WindState {
  float dirX = 0.0f, dirZ = 1.0f;
  float speed = 0.0f;
  float gust = 0.0f;
  float speed01 = 0.5f;
  float gustiness01 = 0.5f;
  bool storm = false;
  // The integers the sim sees, carried along so nothing has to re-quantise a
  // float that came out of them.
  WindStateQ q;
};

namespace winddetail {

// The four per-epoch draws, as Q24 fractions in [0, 1). Components are
// separate hash3 calls rather than bit-slices of one word for the same reason
// the salt is distinct.
inline int32_t Draw24(uint32_t seed, uint32_t epoch, uint32_t component) {
  return (int32_t)(rng::Hash3(seed ^ kWindWeatherSalt, epoch, component) & 0x00FFFFFFu);
}

// One epoch's targets, integer.
struct EpochQ {
  uint32_t headBam = 0;  // downwind heading, BAM32 (2^32 = one turn)
  int32_t speed01 = 0;   // Q24 in [0, 1]
  int32_t gust01 = 0;    // Q24 in [0, 1]
  bool storm = false;
};

inline EpochQ EpochTargetQ(uint32_t seed, uint32_t epoch) {
  EpochQ e;
  // The 24 draw bits ARE the heading, shifted into a BAM32.
  e.headBam = (uint32_t)Draw24(seed, epoch, 0u) << 8;
  e.storm = Draw24(seed, epoch, 3u) < kWindStormChanceQ24;
  const int64_t us = Draw24(seed, epoch, 1u);
  const int64_t ug = Draw24(seed, epoch, 2u);
  if (e.storm) {
    e.speed01 = (int32_t)(13421773 + ((3355443 * us) >> 24));   // 0.80 + 0.20u
    e.gust01 = (int32_t)(11744051 + ((5033165 * ug) >> 24));    // 0.70 + 0.30u
  } else {
    e.speed01 = (int32_t)(1677722 + ((12582912 * us) >> 24));   // 0.10 + 0.75u
    e.gust01 = (int32_t)(3355443 + ((11744051 * ug) >> 24));    // 0.20 + 0.70u
  }
  return e;
}

// Q24 lerp, a + (b - a) * u with u in Q24, written as the two-weight form so
// both ends are reached exactly at u = 0 and u = kWQOne.
inline int32_t LerpQ24(int32_t a, int32_t b, int64_t u) {
  return (int32_t)(((int64_t)a * (kWQOne - u) + (int64_t)b * u) >> 24);
}

// A knob in metres per second as Q16.16 world CELLS per second. The ONE place
// this conversion happens: the shader never sees metres, the knobs never see
// cells. `/` and `*` on a double are IEEE-exact, so converting the float knob
// here is reproducible; everything after it is integer.
inline int64_t MetresPerSecToCellsQ(float mps) {
  const double cells = (double)mps / (double)kVoxelMeters;
  const double r = cells * 65536.0;
  const int64_t v = (int64_t)(r >= 0.0 ? (r + 0.5) : (r - 0.5));
  if (v > 2147483000ll) return 2147483000ll;
  if (v < -2147483000ll) return -2147483000ll;
  return v;
}

inline int32_t ClampQ(int64_t v) {
  if (v > 2147483000ll) return 2147483000;
  if (v < -2147483000ll) return -2147483000;
  return (int32_t)v;
}

}  // namespace winddetail

// The weather for one tick, as integers. THE producer (windfield.cpp).
//
// `seed` is the world seed; `tick` the sim tick; `dayPhase` the tick's day
// phase (TickParams.dayPhase), which sets the stability — pass
// kWindNoDayPhase for neutral. With weatherAuto off the heading and the
// intensity are the knobs', so the mean at the reference height IS
// wind.windSpeed exactly: an evolving field makes two screenshots
// incomparable, so inspecting the field means pinning it.
WindStateQ WindWeatherQ(const Tuning& t, uint32_t seed, uint32_t tick,
                        uint32_t dayPhase = kWindNoDayPhase);

// The same weather, as the renderer's floats. A VIEW: every number here is a
// division of the integer the sim already committed to.
inline WindState WindWeather(const Tuning& t, uint32_t seed, uint32_t tick,
                             uint32_t dayPhase = kWindNoDayPhase) {
  const WindStateQ q = WindWeatherQ(t, seed, tick, dayPhase);
  WindState s;
  s.q = q;
  s.dirX = (float)q.dirX / 65536.0f;
  s.dirZ = (float)q.dirZ / 65536.0f;
  s.speed = (float)q.speed / 65536.0f;
  s.gust = (float)q.gust / 65536.0f;
  s.speed01 = (float)q.speed01 / 65536.0f;
  s.gustiness01 = (float)q.gust01 / 65536.0f;
  s.storm = q.storm;
  return s;
}

// The integers, from a resolved WindState. An ACCESSOR, not a conversion:
// quantising the floats back would not be the identity (a Q16.16 cells/s above
// ~128 needs 25 mantissa bits and f32 has 24).
inline WindStateQ WindQuantize(const WindState& s) { return s.q; }

// ---- debug slope-field overlay (research doc §4.8) --------------------------
// Arrow lattice points along one axis, from the two knobs. THIS FORMULA IS
// MIRRORED in debug_wind.wgsl's vertex shader; keep them in step.
inline uint32_t WindDebugArrowsPerAxis(const Tuning& t) {
  int half = (int)(t.wind.dbgWindRadius / t.wind.dbgWindSpacing);
  if (half < 0) half = 0;
  if (half > 24) half = 24;   // 49^3 = 117,649 arrows is already absurd
  return (uint32_t)(2 * half + 1);
}

// Instance count for the overlay draw. Cubic in radius/spacing, hence the cap.
inline uint32_t WindDebugArrowCount(const Tuning& t) {
  const uint32_t n = WindDebugArrowsPerAxis(t);
  return n * n * n;
}

// ---- gust streaks (wind_streak.wgsl) -----------------------------------------
// Instances for the streak draw: the live pool, or 0 when the master alpha is
// off — the SAME test Simulation::EncodeShadowResolve uses to record the
// update row, so the draw never reads a pool nobody advanced.
inline uint32_t WindStreakDrawCount(const Tuning& t) {
  if (t.wind.streakAlpha <= 0.0f) return 0u;
  const int n = t.wind.streakCount < 0 ? 0 : t.wind.streakCount;
  return (uint32_t)(n > (int)kWindStreakCap ? (int)kWindStreakCap : n);
}
