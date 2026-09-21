#pragma once
#include <cstdint>

#include "sim/intmath.h"  // the integer sine/sqrt this file's weather runs on
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
// DETERMINISM. `WindWeatherQ` is a pure function of (tuning, seed, tick). It
// holds no state, integrates nothing, and cannot drift: asking for tick 90000
// costs the same as tick 1 and gives the same answer on every machine. That is
// the same discipline `ComputeSky` follows and for the same reason — a
// stateful weather integrator desyncs the instant a frame boundary moves.
//
// INTEGER, END TO END, and that is not decoration. This file used to compute
// the weather in floats — libm cos, sin and atan2 — and quantise the
// four scalars to Q16.16 at the end, with a long note here arguing that the
// quantisation made libm's cross-platform wobble harmless. That argument was
// only ever probabilistic, and once `sim.windMode` shipped defaulting to 1 the
// four words became a per-tick INPUT to the CA: a single machine rounding one
// of them differently desyncs a session, silently, for as long as it runs.
// docs/PLAN_multiplayer_now.md L6 called it in and this is the answer.
//
// The producer is now `WindWeatherQ`, which is integer arithmetic on integer
// draws using sim/intmath.h's exact BAM sine and integer sqrt. `WindWeather`
// below is a VIEW of it: the floats RenderParams wants, divided out of the
// integers the sim already agreed on. Nothing derives the weather twice, and
// no libm call sits between the seed and the world hash.

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
// sees one every ~9 minutes rather than never or constantly. Expressed against
// the 24-bit draw (2^21 / 2^24 = 1/8) rather than as a float threshold, so the
// comparison the sim makes is the comparison written here — and so that it is
// the SAME comparison the float version made, epoch for epoch.
constexpr int32_t kWindStormChanceQ24 = 2097152;

// The epoch draws and the two 0..1 weather scalars live in Q24, not Q16.16.
// One reason: a Q16.16 speed01 quantises the mean wind to ~1e-3 cells/s, which
// is a hundred low bits of the Q16.16 speed the CA actually receives. Q24 is
// finer than the output and costs nothing in an int64.
constexpr int32_t kWQOne = 16777216;  // 1.0 in Q24

// ---- the SIM's copy: the weather as integers (research doc §4.2) -----------
// windAtQ (common.wgsl) is integer end to end, so the numbers it starts from
// have to BE integers, not floats that were rounded on the way past. This
// struct is what TickParams carries and what the world hash therefore depends
// on; WindState below is the render-side view of it.
struct WindStateQ {
  int32_t dirX = 0, dirZ = 65536;  // unit XZ downwind, Q16.16
  int32_t speed = 0;               // world cells/s, Q16.16
  int32_t gust = 0;                // world cells/s, Q16.16
  // The raw weather draws behind those, for the dev overlay and the tuner
  // readout: the two 0..1 draws the multipliers came from, in Q16.16, and the
  // storm flag. Exposed because "why is it calm" is otherwise unanswerable.
  int32_t speed01 = 32768, gust01 = 32768;
  bool storm = false;
};

// The evolving weather, resolved for one tick, as the RENDERER wants it.
// Every field is a division of the WindStateQ above — there is no second
// derivation and no second rounding.
struct WindState {
  // Ready for RenderParams: unit XZ direction (pointing DOWNWIND) and two
  // magnitudes already converted to world CELLS PER SECOND.
  float dirX = 0.0f, dirZ = 1.0f;
  float speed = 0.0f;
  float gust = 0.0f;
  // Readout. NOTE there is no angle here any more: the direction is an
  // interpolated VECTOR and recovering an angle from it costs an atan2, which
  // is exactly the libm call this file exists to be rid of. Every consumer
  // wanted the vector anyway.
  float speed01 = 0.5f;
  float gustiness01 = 0.5f;
  bool storm = false;
  // The integers the sim sees, carried along so nothing has to re-quantise a
  // float that came out of them (a Q16.16 speed above ~128 cells/s does not
  // survive the f32 round trip, so re-quantising would NOT be the identity).
  WindStateQ q;
};

namespace winddetail {

// The four per-epoch draws, as Q24 fractions in [0, 1). Components are
// separate hash3 calls rather than bit-slices of one word for the same reason
// the salt is distinct: slices of one PCG output are not independent, and
// correlated direction/speed reads as "it always gets windier when it turns
// north".
//
// 24 BITS, and the mask matters: it is the same `h & 0x00FFFFFF` rng::Unit01
// takes. That is what makes this rewrite a change of ARITHMETIC and not a
// change of WEATHER — the same seed still draws the same headings, the same
// storms and the same moods it drew in floats, to within a couple of low bits
// of the Q16.16 the CA receives.
inline int32_t Draw24(uint32_t seed, uint32_t epoch, uint32_t component) {
  return (int32_t)(rng::Hash3(seed ^ kWindWeatherSalt, epoch, component) & 0x00FFFFFFu);
}

// One epoch's targets, integer. Storm epochs push speed and gustiness to the
// top of their ranges together — a storm is not merely a fast breeze.
struct EpochQ {
  uint32_t headBam = 0;  // downwind heading, BAM32 (2^32 = one turn)
  int32_t speed01 = 0;   // Q24 in [0, 1]
  int32_t gust01 = 0;    // Q24 in [0, 1]
  bool storm = false;
};

inline EpochQ EpochTargetQ(uint32_t seed, uint32_t epoch) {
  EpochQ e;
  // The 24 draw bits ARE the heading, shifted into a BAM32. `<< 8` is exactly
  // the old `Unit01(h) * 2*pi` — a full turn is a full word, so 24 bits of
  // fraction become the top 24 bits of the angle with no scale factor and no
  // wrap. That identity is why this file can be integer without the wind
  // changing where it blows.
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
// cells. `/` and `*` on a double are IEEE-exact (unlike a transcendental), so
// converting the float knob here is reproducible; everything after it is
// integer.
inline int64_t MetresPerSecToCellsQ(float mps) {
  const double cells = (double)mps / (double)kVoxelMeters;
  const double r = cells * 65536.0;
  const int64_t v = (int64_t)(r >= 0.0 ? (r + 0.5) : (r - 0.5));
  // A Q16.16 speed of 2^31 is 32,768 cells/s. The clamp is not expected to
  // engage (LoadTuning bounds the knobs); it is here because an i32 that wraps
  // produces a wind blowing the other way, which is a bug report about the
  // shader.
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

// The weather for one tick, as integers. THE producer.
//
// `seed` is the world seed; `tick` the sim tick. With weatherAuto off this is
// just the two manual knobs converted to engine units — which is the point of
// the switch: an evolving field makes two screenshots incomparable, so
// inspecting the field means pinning it.
inline WindStateQ WindWeatherQ(const Tuning& t, uint32_t seed, uint32_t tick) {
  const Tuning::Wind& w = t.wind;
  WindStateQ o;

  uint32_t headBam = imath::BamFromDegrees((double)w.windDirDeg);
  int64_t speedMulQ = kWQOne;
  int64_t gustMulQ = kWQOne;
  bool haveDir = false;

  if (w.weatherAuto) {
    const uint32_t epoch = tick >> kWindEpochShift;
    const uint32_t span = 1u << kWindEpochShift;
    // Position within the epoch as Q24, exactly: a shift, because the span is
    // a power of two and 24 - kWindEpochShift is the whole conversion.
    int64_t u = (int64_t)(tick & (span - 1u)) << (24 - kWindEpochShift);
    // Smoothstep across the epoch, so the wind eases between moods instead of
    // stepping every 68 seconds. C1 at the boundaries, which is what stops the
    // ease itself from being visible as a kink. u*u*(3 - 2u) in Q24.
    const int64_t uu = (u * u) >> 24;
    u = (uu * (3ll * kWQOne - 2 * u)) >> 24;

    const winddetail::EpochQ e0 = winddetail::EpochTargetQ(seed, epoch);
    const winddetail::EpochQ e1 = winddetail::EpochTargetQ(seed, epoch + 1u);

    // Direction is interpolated as a VECTOR, not as an angle. Lerping angles
    // takes the long way round whenever the pair straddles the wrap — the wind
    // would spin through 359 degrees to get 1 degree over, once every few
    // epochs, and it would look exactly like a bug in the shader. Normalising
    // a lerp of the two unit vectors always takes the short arc.
    //
    // Heading convention, engine-wide: 0 = +Z, increasing toward +X, so the
    // unit vector of a heading is (sin, cos). Same convention mob/avatar
    // headings use, so "wind at 90 degrees" means the same thing everywhere.
    const int64_t vx = (((int64_t)imath::SinQ30(e0.headBam) * (kWQOne - u) +
                         (int64_t)imath::SinQ30(e1.headBam) * u) >> 24);
    const int64_t vz = (((int64_t)imath::CosQ30(e0.headBam) * (kWQOne - u) +
                         (int64_t)imath::CosQ30(e1.headBam) * u) >> 24);
    // |v| in Q30. vx, vz <= 2^30, so vx*vx + vz*vz <= 2^61 — inside i64 with
    // two bits to spare, which is why the lerp stays at Q30 instead of being
    // staged down first.
    const int64_t len = (int64_t)imath::Sqrt64((uint64_t)(vx * vx + vz * vz));
    // Exactly antipodal targets cancel and there is no short arc to take. Rare
    // and momentary; snapping to the incoming target is stable and beats
    // dividing by zero (the float version's normalize(0) produced a NaN that
    // spread into every sample point in the world).
    if (len > 107374) {  // 1e-4 of unit in Q30 — the float version's threshold
      o.dirX = (int32_t)imath::DivRound(vx << 16, len);
      o.dirZ = (int32_t)imath::DivRound(vz << 16, len);
      haveDir = true;
    } else {
      headBam = e1.headBam;
    }

    const int32_t sp01 = winddetail::LerpQ24(e0.speed01, e1.speed01, u);
    const int32_t gu01 = winddetail::LerpQ24(e0.gust01, e1.gust01, u);
    o.storm = (u < kWQOne / 2) ? e0.storm : e1.storm;
    // The readout is Q16.16 like everything else that leaves this file; only
    // the arithmetic above needed the extra eight bits.
    o.speed01 = (sp01 + 128) >> 8;
    o.gust01 = (gu01 + 128) >> 8;

    // The 0..1 draws are mapped so that 0.5 is EXACTLY 1.0x. That makes the
    // manual path (above, where both multipliers are held at kWQOne) reproduce
    // the authored knobs bit for bit, instead of "about the knob" — which is
    // what lets a screenshot with weatherAuto off be compared against the knob
    // values. Both constant pairs below land on exactly kWQOne at u = 0.5.
    speedMulQ = 4194304 + (((int64_t)25165824 * sp01) >> 24);  // 0.25 + 1.50u
    gustMulQ = 5872026 + (((int64_t)21810381 * gu01) >> 24);   // 0.35 + 1.30u
  }

  if (!haveDir) {
    o.dirX = imath::SinQ16(headBam);
    o.dirZ = imath::CosQ16(headBam);
  }

  const int64_t baseQ = winddetail::MetresPerSecToCellsQ(w.windSpeed);
  const int64_t speedQ = imath::MulShiftRound(baseQ, speedMulQ, 24);
  o.speed = winddetail::ClampQ(speedQ);
  // Gust amplitude is a fraction of the MEAN, which is how gustiness behaves:
  // a windier day has bigger gusts, not the same gusts on a faster mean.
  const int64_t gsQ = (int64_t)((double)w.gustStrength * 65536.0 +
                                (w.gustStrength >= 0.0f ? 0.5 : -0.5));
  o.gust = winddetail::ClampQ(
      imath::MulShiftRound(imath::MulShiftRound(speedQ, gsQ, 16), gustMulQ, 24));
  return o;
}

// The same weather, as the renderer's floats. A VIEW: every number here is a
// division of the integer the sim already committed to, so the grass and the
// CA cannot end up in different weather even by a low bit.
inline WindState WindWeather(const Tuning& t, uint32_t seed, uint32_t tick) {
  const WindStateQ q = WindWeatherQ(t, seed, tick);
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

// The integers, from a resolved WindState. Kept as a named call because
// SubmitTick reads better for it, but it is now an ACCESSOR, not a
// conversion: WindWeatherQ already produced these and WindWeather only
// divided them out. Quantising the floats back would not be the identity —
// a Q16.16 cells/s above ~128 needs 25 mantissa bits and f32 has 24 — which
// is the bug this shape exists to make unwritable.
inline WindStateQ WindQuantize(const WindState& s) { return s.q; }

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
