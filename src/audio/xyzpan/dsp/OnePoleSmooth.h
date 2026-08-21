#pragma once
// OnePoleSmooth.h
// Exponential (one-pole IIR) parameter smoother for click-free automation.
// Converts instantaneous target jumps into smooth exponential ramps.
//
// Form:  z = target * b + z * a
//
// Coefficient formula:
//   a = exp(-2*pi / (smoothingMs * 0.001 * sampleRate))
//   b = 1.0 - a
//
// Interpretation:
//   smoothingMs is approximately the RC time constant (63% rise time).
//   After 5 * smoothingMs the output is within ~1% of the target.
//
// Usage:
//   1. prepare(smoothingMs, sampleRate) once before processing.
//   2. reset(value) to set the initial state (optional; default = 0).
//   3. process(target) each sample; returns the smoothed value.
//   4. current() reads the current state without updating.
//
// prepare() is safe to call mid-stream to change the smoothing time constant.
// It recomputes a_ and b_ but does NOT reset z_ — no click from time constant change.
//
// Source: musicdsp.org/en/latest/Filters/257-1-pole-lpf-for-smooth-parameter-changes.html

#include <cmath>

namespace xyzpan::dsp {

class OnePoleSmooth {
public:
    // Compute smoothing coefficients from the time constant and sample rate.
    // smoothingMs: RC time constant in milliseconds (63% of step is covered in this time).
    // Safe to call without resetting state — allows live time constant changes.
    void prepare(float smoothingMs, float sampleRate) {
        a_ = std::exp(-6.28318530f / (smoothingMs * 0.001f * sampleRate));
        b_ = 1.0f - a_;
        // Note: z_ is intentionally NOT reset here so prepare() can be called
        // to update the time constant without causing a state discontinuity.
    }

    // Set the smoother state to value immediately (no transition).
    void reset(float value = 0.0f) {
        z_ = value;
    }

    // Process one target sample and return the smoothed output.
    float process(float target) {
        z_ = target * b_ + z_ * a_;
        return z_;
    }

    // Analytically converge toward target by numSamples iterations.
    // Equivalent to calling process(target) numSamples times but O(1).
    void converge(float target, int numSamples) {
        z_ = target + (z_ - target) * std::pow(a_, static_cast<float>(numSamples));
    }

    // Return the current smoothed value without advancing state.
    float current() const {
        return z_;
    }

private:
    float a_ = 0.0f;   // pole (smoothing coefficient)
    float b_ = 1.0f;   // (1 - a)
    float z_ = 0.0f;   // state (current output)
};

} // namespace xyzpan::dsp
