#pragma once
#include <cmath>
#include <cstdint>

#include "sim/rng.h"
#include "sim/tuning.h"
#include "sim/world.h"   // kVoxelMeters — the one m -> cell conversion

// wind.h — the WEATHER half of the wind system.
//
// docs/RESEARCH_wind.md is the plan of record; DESIGN.md §12 states the
// invariants. Read the two-line version: wind is a pure function
// `windAt(worldPos, t)` living in common.wgsl, and everything about it is
// either a compile-time TUNE_* constant or one of the three numbers this file
// produces. There is no stored field anywhere, at any resolution.
//
// WHY THIS EXISTS AT ALL, given that the field is a shader function. Because
// weather has to DRIFT. A gust band's wavelength is authored once and folded
// into the shader; the direction the wind is blowing has to wander over
// minutes, and no compile-time constant does that. So the evolving part —
// direction, mean speed, gust amplitude — is computed here, once per frame,
// and shipped in RenderParams.
//
// WHY IT IS ONE FUNCTION AND NOT TWO. Phase 4 gives the CA an integer
// `windAtQ` fed from TickParams, and TickParams is a determinism input: a
// replay reproduces it, the twice-run gate compares it. If the renderer and
// the sim each derived their own weather, grass and smoke would blow different
// ways in the same frame and the bug would look like a shader problem. So
// `WindWeather` is the ONE author of these values for both UBOs — the same
// arrangement `dayPhase` already has (world.h TickParams).
//
// DETERMINISM. `WindWeather` is a pure function of (tuning, seed, tick). It
// holds no state, integrates nothing, and cannot drift: asking for tick 90000
// costs the same as tick 1 and gives the same answer on every machine. That is
// the same discipline `ComputeSky` follows and for the same reason — a
// stateful weather integrator desyncs the instant a frame boundary moves.
//
// FLOAT, and that is fine. Nothing here reaches the CA in phase 1; these
// values feed RenderParams only. When phase 4 wires the sim up it converts to
// Q16.16 at the boundary, exactly as sim_fluid.wgsl converts its human-unit
// knobs, and the kernel stays integer.

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

// Chance an epoch is a STORM: harder wind, gustier. One in eight, so a session
// sees one every ~9 minutes rather than never or constantly.
constexpr float kWindStormChance = 0.125f;

// The evolving weather, resolved for one tick.
struct WindState {
  // Ready for RenderParams: unit XZ direction (pointing DOWNWIND) and two
  // magnitudes already converted to world CELLS PER SECOND.
  float dirX = 0.0f, dirZ = 1.0f;
  float speed = 0.0f;
  float gust = 0.0f;
  // The raw weather draws behind those, for the dev overlay and the tuner
  // readout: heading in radians, and the two 0..1 draws the multipliers came
  // from. Exposed because "why is it calm" is otherwise unanswerable.
  float dirRad = 0.0f;
  float speed01 = 0.5f;
  float gustiness01 = 0.5f;
  bool storm = false;
};

namespace winddetail {

// The four per-epoch draws. Components are separate hash3 calls rather than
// bit-slices of one word for the same reason the salt is distinct: slices of
// one PCG output are not independent, and correlated direction/speed reads as
// "it always gets windier when it turns north".
inline float Draw01(uint32_t seed, uint32_t epoch, uint32_t component) {
  return rng::Unit01(rng::Hash3(seed ^ kWindWeatherSalt, epoch, component));
}

// One epoch's targets. Storm epochs push speed and gustiness to the top of
// their ranges together — a storm is not merely a fast breeze.
inline void EpochTarget(uint32_t seed, uint32_t epoch, float& dirRad,
                        float& speed01, float& gust01, bool& storm) {
  dirRad = Draw01(seed, epoch, 0u) * 6.28318531f;
  storm = Draw01(seed, epoch, 3u) < kWindStormChance;
  const float us = Draw01(seed, epoch, 1u);
  const float ug = Draw01(seed, epoch, 2u);
  if (storm) {
    speed01 = 0.80f + 0.20f * us;
    gust01 = 0.70f + 0.30f * ug;
  } else {
    speed01 = 0.10f + 0.75f * us;
    gust01 = 0.20f + 0.70f * ug;
  }
}

}  // namespace winddetail

// The weather for one tick.
//
// `seed` is the world seed; `tick` the sim tick. With weatherAuto off this is
// just the two manual knobs converted to engine units — which is the point of
// the switch: an evolving field makes two screenshots incomparable, so
// inspecting the field means pinning it.
inline WindState WindWeather(const Tuning& t, uint32_t seed, uint32_t tick) {
  const Tuning::Wind& w = t.wind;
  WindState s;

  float dirRad = w.windDirDeg * 0.01745329252f;
  float speedMul = 1.0f;
  float gustMul = 1.0f;

  if (w.weatherAuto) {
    const uint32_t epoch = tick >> kWindEpochShift;
    const uint32_t span = 1u << kWindEpochShift;
    // Smoothstep across the epoch, so the wind eases between moods instead of
    // stepping every 68 seconds. C1 at the boundaries, which is what stops the
    // ease itself from being visible as a kink.
    float u = (float)(tick & (span - 1u)) / (float)span;
    u = u * u * (3.0f - 2.0f * u);

    float d0, d1, sp0, sp1, g0, g1;
    bool st0, st1;
    winddetail::EpochTarget(seed, epoch, d0, sp0, g0, st0);
    winddetail::EpochTarget(seed, epoch + 1u, d1, sp1, g1, st1);

    // Direction is interpolated as a VECTOR, not as an angle. Lerping angles
    // takes the long way round whenever the pair straddles 0/2pi — the wind
    // would spin through 359 degrees to get 1 degree over, once every few
    // epochs, and it would look exactly like a bug in the shader. Normalising
    // a lerp of the two unit vectors always takes the short arc.
    float cx = std::cos(d0) * (1.0f - u) + std::cos(d1) * u;
    float cz = std::sin(d0) * (1.0f - u) + std::sin(d1) * u;
    float len = std::sqrt(cx * cx + cz * cz);
    // Exactly antipodal targets cancel and there is no short arc to take. Rare
    // and momentary; snapping to the incoming target is stable and beats a
    // normalize(0) NaN spreading into every sample point in the world.
    dirRad = (len > 1e-4f) ? std::atan2(cz, cx) : d1;

    s.speed01 = sp0 * (1.0f - u) + sp1 * u;
    s.gustiness01 = g0 * (1.0f - u) + g1 * u;
    s.storm = (u < 0.5f) ? st0 : st1;

    // The 0..1 draws are mapped so that 0.5 is EXACTLY 1.0x. That makes the
    // manual path (below, where both are held at 0.5) reproduce the authored
    // knobs bit for bit, instead of "about the knob" — which is what lets a
    // screenshot with weatherAuto off be compared against the knob values.
    speedMul = 0.25f + 1.50f * s.speed01;
    gustMul = 0.35f + 1.30f * s.gustiness01;
  }

  s.dirRad = dirRad;
  // Heading convention, engine-wide: 0 = +Z, increasing toward +X. Same
  // convention mob/avatar headings use, so "wind at 90 degrees" means the same
  // thing everywhere in this codebase.
  s.dirX = std::sin(dirRad);
  s.dirZ = std::cos(dirRad);

  // m/s -> world cells/s. The ONE place this conversion happens: the shader
  // never sees metres, and the knobs never see cells.
  const float cellsPerMeter = 1.0f / kVoxelMeters;
  s.speed = w.windSpeed * speedMul * cellsPerMeter;
  // Gust amplitude is a fraction of the MEAN, which is how gustiness behaves:
  // a windier day has bigger gusts, not the same gusts on a faster mean.
  s.gust = s.speed * w.gustStrength * gustMul;
  return s;
}

// ---- the SIM's copy: the same weather, quantised (research doc §4.2) --------
// windAtQ (common.wgsl) is integer end to end, so the three numbers it starts
// from have to arrive as integers. This is that boundary, and it is the ONLY
// place floats become the sim's wind: everything downstream of TickParams is
// i32 arithmetic with an integer sine.
//
// DETERMINISM, honestly. WindWeather above calls std::sin/cos/atan2, and libm
// is not bit-identical between platforms — so in principle two machines could
// round a weather value to different Q16.16 integers and, once windMode is on,
// diverge. Three things bound that, in order of how much they matter:
//
//   1. It is a per-TICK INPUT, not a per-cell computation. The engine already
//      accepts CPU-float inputs that reach the grid: every debris ParticleSpawn
//      comes out of Jolt, which is float by design, and those particles
//      reinsert themselves as voxels. This is strictly better behaved than
//      that — four scalars a tick, quantised.
//   2. The quantisation is the guard. Two libm results have to straddle a
//      1/65536 boundary to differ AFTER rounding, which needs them to disagree
//      by more than ~2^-36 of the value; real implementations agree to ~1 ulp
//      of a double.
//   3. Every gate that compares hashes runs twice on ONE machine, so this
//      cannot mask a real bug — it can only be a cross-machine surprise.
//
// If wind ever does desync across machines, the fix is known and contained:
// make WindWeather integer end to end (draw the heading as a BAM angle, keep
// the epoch lerp as a vector, normalise with the existing integer isqrt) and
// derive the float outputs FROM the integers, with a check_invariants entry
// pinning the C++ sine to windSinQ. That is a bigger change than phases 3-4
// warrant on the evidence available, which is none.
struct WindStateQ {
  int32_t dirX = 0, dirZ = 65536;  // unit XZ downwind, Q16.16
  int32_t speed = 0;               // world cells/s, Q16.16
  int32_t gust = 0;                // world cells/s, Q16.16
};

inline WindStateQ WindQuantize(const WindState& s) {
  // Round-half-away-from-zero by hand rather than through std::lround: the
  // rounding MODE is part of the value the sim sees, and one written here is
  // one that cannot be changed by a compiler flag.
  auto q = [](float v) {
    double x = (double)v * 65536.0;
    double r = x >= 0.0 ? (x + 0.5) : (x - 0.5);
    // A Q16.16 speed of 2^31 is 32,768 cells/s. The clamp is not expected to
    // engage (LoadTuning bounds the knobs); it is here because an i32 that
    // wraps produces a wind blowing the other way, which is a bug report about
    // the shader.
    if (r > 2147483000.0) r = 2147483000.0;
    if (r < -2147483000.0) r = -2147483000.0;
    return (int32_t)r;
  };
  WindStateQ o;
  o.dirX = q(s.dirX);
  o.dirZ = q(s.dirZ);
  o.speed = q(s.speed);
  o.gust = q(s.gust);
  return o;
}

// ---- debug slope-field overlay (research doc §4.8) --------------------------
// Arrow lattice points along one axis, from the two knobs. THIS FORMULA IS
// MIRRORED in debug_wind.wgsl's vertex shader, which derives its lattice
// coordinate from the instance index the same way. They must agree: the CPU
// side decides how many instances to draw, the shader side decides where each
// one goes. Disagreement is graceful in both directions (too few instances
// draws a smaller field; too many would index past the lattice and the shader
// discards them), which is why this is a comment rather than a check_invariants
// entry — but keep them in step anyway.
inline uint32_t WindDebugArrowsPerAxis(const Tuning& t) {
  int half = (int)(t.wind.dbgWindRadius / t.wind.dbgWindSpacing);
  if (half < 0) half = 0;
  if (half > 24) half = 24;   // 49^3 = 117,649 arrows is already absurd
  return (uint32_t)(2 * half + 1);
}

// Instance count for the overlay draw. Cubic in radius/spacing, hence the cap
// above: this is the number that decides whether the overlay is free.
inline uint32_t WindDebugArrowCount(const Tuning& t) {
  const uint32_t n = WindDebugArrowsPerAxis(t);
  return n * n * n;
}
