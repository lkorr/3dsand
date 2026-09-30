#pragma once
#include <cmath>
#include <cstdint>

// intmath.h — the engine's EXACT integer transcendentals, for numbers that
// enter the SIMULATION'S INPUT STREAM.
//
// WHY THIS EXISTS. CLAUDE.md rule 1 says the sim is bit-deterministic: same
// seed + tick + inputs, same world on every machine. The kernels honour that —
// they are integer-only by construction. The hole was on the CPU side of the
// boundary: `WindWeather` (sim/wind.h) called libm cos/sin/atan2 and the world
// map's landform bake called libm cos/sin, and libm is NOT bit-identical
// across platforms or compiler versions. Those results are quantised into
// TickParams and baked into a byte plane, so a one-ulp libm difference could
// land two machines on different integers and desync a session that never
// touched a shader. See docs/PLAN_multiplayer_now.md L6.
//
// The fix is not "round harder", it is "never call libm". Everything below is
// integer arithmetic on integer inputs, so it produces the same bits on every
// machine that has a 64-bit integer multiply.
//
// Nothing in this header is mirrored in WGSL -- it runs CPU-side only, on
// values that then cross as integers.
namespace imath {

// pi/2 in Q30. The ONE irrational constant in this header, written as an
// integer literal rather than computed, so it cannot be a compiler's idea of
// what M_PI rounds to.
inline constexpr int64_t kHalfPiQ30 = 1686629713;  // round(pi/2 * 2^30)

/**
 * sin of a 32-bit BAM angle, Q30.
 *
 * BAM32: the whole turn is 2^32, so the angle wraps for free in the type and
 * there is no modulo anywhere. Output is Q30 — [-2^30, +2^30] — exact at the
 * four cardinal angles and accurate to ~3e-9 absolute, i.e. about 3 low bits
 * of Q30 and a five-thousandth of one bit of Q16.16. That is better than an
 * f32 sine and, unlike one, it is REPRODUCIBLE, which is the whole point.
 *
 * Shape: quadrant from the top two bits, octant fold so the polynomial only
 * ever sees theta <= pi/4, then the Taylor series to T^9 / T^10. Every
 * intermediate is Q30 in an int64 and every >>30 is applied to a non-negative
 * value (the fold guarantees it), so the truncation is a uniform floor with no
 * sign asymmetry to bias a direction field.
 */
inline int32_t SinQ30(uint32_t bam) {
  const uint32_t q = bam >> 30;                    // quadrant 0..3
  const int64_t r = (int64_t)(bam & 0x3FFFFFFFu);  // 0 .. 2^30-1 within it
  // Octant fold: past pi/4 into the quadrant, sin and cos swap roles.
  bool useCos = (q & 1u) != 0u;
  int64_t x = r;
  if (r > (1 << 29)) {
    x = (1 << 30) - r;
    useCos = !useCos;
  }
  // theta, Q30 radians: theta = x / 2^30 * pi/2.
  const int64_t T = (x * kHalfPiQ30) >> 30;
  const int64_t t2 = (T * T) >> 30;
  int64_t v;
  if (useCos) {
    const int64_t p4 = (t2 * t2) >> 30;
    const int64_t p6 = (p4 * t2) >> 30;
    const int64_t p8 = (p6 * t2) >> 30;
    const int64_t p10 = (p8 * t2) >> 30;
    v = (1 << 30) - t2 / 2 + p4 / 24 - p6 / 720 + p8 / 40320 - p10 / 3628800;
  } else {
    const int64_t p3 = (T * t2) >> 30;
    const int64_t p5 = (p3 * t2) >> 30;
    const int64_t p7 = (p5 * t2) >> 30;
    const int64_t p9 = (p7 * t2) >> 30;
    v = T - p3 / 6 + p5 / 120 - p7 / 5040 + p9 / 362880;
  }
  return (int32_t)(q >= 2u ? -v : v);
}

/** cos of a 32-bit BAM angle, Q30. cos(a) = sin(a + pi/2). */
inline int32_t CosQ30(uint32_t bam) { return SinQ30(bam + 0x40000000u); }

// Q30 -> Q16.16, rounded half AWAY FROM ZERO. Written on the magnitude for the
// reason common.wgsl's wq() documents at length: an arithmetic shift floors,
// so a signed field built out of shifted products drifts uniformly negative
// and reads as a mysterious bias toward -x/-z that no knob explains.
inline int32_t Q30ToQ16(int32_t v) {
  const int64_t a = v < 0 ? -(int64_t)v : (int64_t)v;
  const int64_t m = (a + (1 << 13)) >> 14;
  return (int32_t)(v < 0 ? -m : m);
}

/** sin of a 32-bit BAM angle, Q16.16 ([-65536, 65536]). */
inline int32_t SinQ16(uint32_t bam) { return Q30ToQ16(SinQ30(bam)); }
/** cos of a 32-bit BAM angle, Q16.16. */
inline int32_t CosQ16(uint32_t bam) { return Q30ToQ16(CosQ30(bam)); }

/** floor(sqrt(v)) for a u64, exact. Bit-by-bit, no float, no libm. */
inline uint64_t Sqrt64(uint64_t v) {
  uint64_t res = 0, bit = 1ull << 62;
  while (bit > v) bit >>= 2;
  while (bit != 0) {
    if (v >= res + bit) {
      v -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  return res;
}

/** a / b rounded half away from zero, integer only. `b` must be > 0. */
inline int64_t DivRound(int64_t a, int64_t b) {
  const int64_t m = a < 0 ? -a : a;
  const int64_t r = (m + b / 2) / b;
  return a < 0 ? -r : r;
}

/** (a * b) >> shift, rounded half away from zero. Fixed-point multiply. */
inline int64_t MulShiftRound(int64_t a, int64_t b, int shift) {
  const int64_t p = a * b;
  const int64_t m = p < 0 ? -p : p;
  const int64_t r = (m + ((int64_t)1 << (shift - 1))) >> shift;
  return p < 0 ? -r : r;
}

/**
 * Whole degrees -> BAM32, exactly. 90 degrees is exactly 2^30 and 180 exactly
 * 2^31; no other rounding rule gives that, and it matters because an authored
 * `rotation: 90` has to mean a quarter turn and not a quarter turn minus a
 * hair. Negative and >360 inputs wrap.
 */
inline uint32_t BamFromDegreesI(int deg) {
  int d = deg % 360;
  if (d < 0) d += 360;
  return (uint32_t)((((int64_t)d << 32) + 180) / 360);
}

/**
 * Fractional degrees -> BAM32. `floor`, `/`, `*` and the final round are all
 * IEEE-exact operations on a double — unlike sin/cos/exp, which are not
 * specified to be correctly rounded — so this is reproducible even though it
 * touches a float. The knob it converts is itself a float read from JSON;
 * this is the boundary where it stops being one.
 */
inline uint32_t BamFromDegrees(double deg) {
  double turns = deg / 360.0;
  turns -= std::floor(turns);             // [0, 1)
  const double x = turns * 4294967296.0;  // [0, 2^32)
  const int64_t v = (int64_t)(x + 0.5);
  return (uint32_t)((uint64_t)v & 0xFFFFFFFFull);
}

/**
 * log2 of a Q16.16 value, Q16.16 out, exact for a given input. `x` must be
 * > 0; x = 65536 (1.0) gives 0. The integer part is the position of the top
 * bit; the fraction is the classic bit-by-bit squaring loop on a Q30 mantissa
 * in [1, 2): square it, and if it reached 2 that bit of the log is set and the
 * mantissa halves. Sixteen iterations for sixteen fraction bits. No float, no
 * libm — the wind profile's log-law is built from this so the lookup table the
 * sim reads is the same integers on every machine.
 */
inline int32_t Log2Q16(uint64_t x) {
  if (x == 0) return INT32_MIN;
  int msb = 63;
  while (((x >> msb) & 1u) == 0) msb--;
  const int32_t ip = msb - 16;
  // Mantissa in Q30, [2^30, 2^31).
  uint64_t m = msb >= 30 ? (x >> (msb - 30)) : (x << (30 - msb));
  int32_t frac = 0;
  for (int b = 15; b >= 0; b--) {
    m = (m * m) >> 30;
    if (m >= (2ull << 30)) {
      m >>= 1;
      frac |= (1 << b);
    }
  }
  return ip * 65536 + frac;
}

}  // namespace imath
