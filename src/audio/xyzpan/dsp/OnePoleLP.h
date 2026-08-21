#pragma once
// OnePoleLP.h
// First-order 6 dB/octave lowpass filter parameterised by cutoff frequency.
//
// Distinct from OnePoleSmooth (which uses a smoothing time constant in ms).
// This class uses a cutoff frequency in Hz, making it suitable for audio
// filtering rather than parameter smoothing.
//
// Coefficient formula:
//   a = exp(-2 * pi * cutoffHz / sampleRate)   [pole position]
//   b = 1 - a                                   [DC gain normalisation]
//
// Difference equation: z[n] = b * x[n] + a * z[n-1]
//
// Used for: chest bounce 6 dB/oct lowpass at 1 kHz (ELEV-03).
//
// Source: standard first-order IIR lowpass design
//   (equivalent to RC-circuit impulse response discretised via impulse invariant method)

#include <cmath>

namespace xyzpan::dsp {

class OnePoleLP {
public:
    // Zero filter state. Call before processing or after silence.
    void reset() {
        z_ = 0.0f;
        samplesRemaining_ = 0;
    }

    // Compute filter coefficients.
    //   cutoffHz   — -3 dB cutoff frequency in Hz
    //   sampleRate — audio sample rate in Hz
    //
    // Call once per block or whenever the cutoff changes.
    void setCoefficients(float cutoffHz, float sampleRate) {
        a_ = std::exp(-2.0f * 3.14159265f * cutoffHz / sampleRate);
        b_ = 1.0f - a_;
        // Snap current coefficients — no interpolation
        a_current_ = a_;
        b_current_ = b_;
        samplesRemaining_ = 0;
    }

    // Compute filter coefficients with per-sample linear interpolation across
    // the block. Prevents discontinuities when cutoff changes between blocks.
    //   cutoffHz   — target -3 dB cutoff frequency in Hz
    //   sampleRate — audio sample rate in Hz
    //   blockSize  — number of samples in the current block
    void setCoefficientsSmoothed(float cutoffHz, float sampleRate, int blockSize) {
        const float aTarget = std::exp(-2.0f * 3.14159265f * cutoffHz / sampleRate);
        const float bTarget = 1.0f - aTarget;

        if (blockSize <= 1) {
            a_ = aTarget;
            b_ = bTarget;
            a_current_ = aTarget;
            b_current_ = bTarget;
            samplesRemaining_ = 0;
            return;
        }

        // Start from where we currently are
        a_current_ = a_;
        b_current_ = b_;

        // Store targets
        a_ = aTarget;
        b_ = bTarget;

        const float inv = 1.0f / static_cast<float>(blockSize);
        a_inc_ = (aTarget - a_current_) * inv;
        b_inc_ = (bTarget - b_current_) * inv;
        samplesRemaining_ = blockSize;
    }

    // Process one input sample and return the filtered output.
    float process(float x) {
        if (samplesRemaining_ > 0) {
            z_ = b_current_ * x + a_current_ * z_;
            a_current_ += a_inc_;
            b_current_ += b_inc_;
            --samplesRemaining_;
            return z_;
        }
        z_ = b_ * x + a_ * z_;
        return z_;
    }

private:
    float a_ = 0.0f;  // pole (feedback coefficient) — target
    float b_ = 1.0f;  // feedforward coefficient (DC gain = 1) — target
    float z_ = 0.0f;  // filter state

    // Coefficient interpolation state
    float a_current_ = 0.0f;
    float b_current_ = 1.0f;
    float a_inc_ = 0.0f;
    float b_inc_ = 0.0f;
    int   samplesRemaining_ = 0;
};

} // namespace xyzpan::dsp
