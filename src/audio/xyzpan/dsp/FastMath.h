#pragma once
// FastMath.h
// Approximate math functions for performance-critical DSP paths.
// Use only where ~0.1% error is acceptable (distance calculations, gain curves).
// Keep std:: versions for exact reference paths (block-constant, no-LFO).

#include <cstring>

namespace xyzpan::dsp {

// Fast square root using bit-hack initial guess + one Newton-Raphson iteration.
// Max relative error: ~0.1% for positive inputs.
// ~3x faster than std::sqrt on x86 without SSE rsqrt.
static inline float fastSqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    float y = x;
    int i;
    std::memcpy(&i, &y, sizeof(i));
    i = 0x1fbd1df5 + (i >> 1);
    std::memcpy(&y, &i, sizeof(y));
    return 0.5f * (y + x / y);
}

} // namespace xyzpan::dsp
