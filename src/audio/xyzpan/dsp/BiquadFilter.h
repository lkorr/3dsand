#pragma once
// BiquadFilter.h
// Audio EQ Cookbook biquad filter: PeakingEQ, HighShelf, LowShelf modes.
//
// Used for:
//   - Pinna notch/peak filter: PeakingEQ at 8kHz, gain from -15dB (Z=0) to +5dB (Z=1)
//   - High shelf: HighShelf at 4kHz, gain from 0dB (Z=0) to +3dB (Z=1)
//
// CRITICAL: setCoefficients() calls std::cos, std::sin, std::pow, std::sqrt.
// Update coefficients per BLOCK (e.g., once per 64-512 samples) — NOT per sample.
// Direct Form II is used for the biquad difference equations.
//
// Unity bypass: when gainDb=0, PeakingEQ reduces to all-pass (output equals input).
//
// Sources:
//   webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
//   Robert Bristow-Johnson, Audio EQ Cookbook

#include <cmath>
#include <algorithm>

namespace xyzpan::dsp {

enum class BiquadType { PeakingEQ, HighShelf, LowShelf };

class BiquadFilter {
public:
    // Zero biquad delay state. Call before processing or after silence.
    void reset() {
        z1_ = 0.0f;
        z2_ = 0.0f;
    }

    // Compute biquad coefficients.
    //   type      — filter topology (PeakingEQ, HighShelf, or LowShelf)
    //   freqHz    — center (PeakingEQ) or shelf (shelf types) frequency in Hz
    //   sampleRate— audio sample rate in Hz
    //   Q         — bandwidth/slope (0.7071 ≈ 1 octave bandwidth for PeakingEQ)
    //   gainDb    — boost/cut in dB; 0 = unity (all-pass for PeakingEQ)
    //
    // Call once per block, not per sample (transcendental function overhead).
    void setCoefficients(BiquadType type, float freqHz, float sampleRate,
                         float Q, float gainDb) {
        // Audio EQ Cookbook intermediate variables
        const float A     = std::pow(10.0f, gainDb / 40.0f);   // 10^(dBgain/40)
        const float w0    = 2.0f * 3.14159265f * freqHz / sampleRate;
        const float cs    = std::cos(w0);
        const float sn    = std::sin(w0);
        const float alpha = sn / (2.0f * Q);

        float b0, b1, b2, a0, a1, a2;

        switch (type) {
            case BiquadType::PeakingEQ:
                b0 =  1.0f + alpha * A;
                b1 = -2.0f * cs;
                b2 =  1.0f - alpha * A;
                a0 =  1.0f + alpha / A;
                a1 = -2.0f * cs;
                a2 =  1.0f - alpha / A;
                break;

            case BiquadType::HighShelf: {
                const float sqrtA         = std::sqrt(A);
                const float twoSqrtAalpha = 2.0f * sqrtA * alpha;
                b0 =       A * ((A + 1.0f) + (A - 1.0f) * cs + twoSqrtAalpha);
                b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs);
                b2 =       A * ((A + 1.0f) + (A - 1.0f) * cs - twoSqrtAalpha);
                a0 =            (A + 1.0f) - (A - 1.0f) * cs + twoSqrtAalpha;
                a1 =  2.0f *   ((A - 1.0f) - (A + 1.0f) * cs);
                a2 =            (A + 1.0f) - (A - 1.0f) * cs - twoSqrtAalpha;
                break;
            }

            case BiquadType::LowShelf: {
                const float sqrtA         = std::sqrt(A);
                const float twoSqrtAalpha = 2.0f * sqrtA * alpha;
                b0 =       A * ((A + 1.0f) - (A - 1.0f) * cs + twoSqrtAalpha);
                b1 =  2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs);
                b2 =       A * ((A + 1.0f) - (A - 1.0f) * cs - twoSqrtAalpha);
                a0 =            (A + 1.0f) + (A - 1.0f) * cs + twoSqrtAalpha;
                a1 = -2.0f *   ((A - 1.0f) + (A + 1.0f) * cs);
                a2 =            (A + 1.0f) + (A - 1.0f) * cs - twoSqrtAalpha;
                break;
            }

            default:
                // Identity fallback (should not occur)
                b0_ = 1.0f; b1_ = 0.0f; b2_ = 0.0f;
                a1_ = 0.0f; a2_ = 0.0f;
                return;
        }

        // Normalize all coefficients by a0
        b0_ = b0 / a0;
        b1_ = b1 / a0;
        b2_ = b2 / a0;
        a1_ = a1 / a0;
        a2_ = a2 / a0;
    }

    // Compute new target coefficients and linearly interpolate from old to new
    // across blockSize samples. Eliminates clicks from coefficient jumps at
    // block boundaries. Call once per block instead of setCoefficients().
    void setCoefficientsSmoothed(BiquadType type, float freqHz, float sampleRate,
                                 float Q, float gainDb, int blockSize) {
        // Snapshot current coefficients as "old"
        old_b0_ = b0_; old_b1_ = b1_; old_b2_ = b2_;
        old_a1_ = a1_; old_a2_ = a2_;

        // Compute new target coefficients
        setCoefficients(type, freqHz, sampleRate, Q, gainDb);

        // Store new targets, restore old as active starting point
        new_b0_ = b0_; new_b1_ = b1_; new_b2_ = b2_;
        new_a1_ = a1_; new_a2_ = a2_;
        b0_ = old_b0_; b1_ = old_b1_; b2_ = old_b2_;
        a1_ = old_a1_; a2_ = old_a2_;

        smoothSamplesRemaining_ = blockSize;
        smoothInc_ = 1.0f / static_cast<float>(blockSize);
        smoothT_ = 0.0f;
    }

    // Process one sample through the biquad (Direct Form II).
    // When smoothing is active, linearly interpolates coefficients per-sample.
    //   y   = b0*x + z1
    //   z1' = b1*x - a1*y + z2
    //   z2' = b2*x - a2*y
    float process(float x) {
        if (smoothSamplesRemaining_ > 0) {
            smoothT_ += smoothInc_;
            b0_ = old_b0_ + (new_b0_ - old_b0_) * smoothT_;
            b1_ = old_b1_ + (new_b1_ - old_b1_) * smoothT_;
            b2_ = old_b2_ + (new_b2_ - old_b2_) * smoothT_;
            a1_ = old_a1_ + (new_a1_ - old_a1_) * smoothT_;
            a2_ = old_a2_ + (new_a2_ - old_a2_) * smoothT_;
            --smoothSamplesRemaining_;
        }
        float y = b0_ * x + z1_;
        z1_     = b1_ * x - a1_ * y + z2_;
        z2_     = b2_ * x - a2_ * y;
        return y;
    }

private:
    // Normalized biquad coefficients (divided by a0 at construction time)
    float b0_ = 1.0f;  // feedforward
    float b1_ = 0.0f;
    float b2_ = 0.0f;
    float a1_ = 0.0f;  // feedback (a0 already factored out)
    float a2_ = 0.0f;

    // Delay state (Direct Form II)
    float z1_ = 0.0f;
    float z2_ = 0.0f;

    // Per-sample coefficient smoothing state
    float old_b0_ = 1.0f, old_b1_ = 0.0f, old_b2_ = 0.0f;
    float old_a1_ = 0.0f, old_a2_ = 0.0f;
    float new_b0_ = 1.0f, new_b1_ = 0.0f, new_b2_ = 0.0f;
    float new_a1_ = 0.0f, new_a2_ = 0.0f;
    int   smoothSamplesRemaining_ = 0;
    float smoothInc_ = 0.0f;
    float smoothT_   = 0.0f;
};

} // namespace xyzpan::dsp
