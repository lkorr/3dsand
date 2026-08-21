#pragma once
// SVFLowPass.h
// TPT (Topology-Preserving Transform) State Variable Filter in low-pass mode.
// Andy Simper / Cytomic formulation — stable under per-sample cutoff modulation.
//
// Used for head shadow and rear shadow filtering in the binaural panning pipeline.
//
// Coefficient formula:
//   g  = tan(pi * cutoffHz / sampleRate)    [frequency warping]
//   k  = 1/Q                                 [damping; Q=0.7071 = Butterworth]
//   a1 = 1 / (1 + g*(g + k))
//   a2 = g * a1
//   a3 = g * a2
//
// Per-sample update:
//   v3 = v0 - ic2eq_
//   v1 = a1_*ic1eq_ + a2_*v3
//   v2 = ic2eq_ + a2_*ic1eq_ + a3_*v3
//   ic1eq_ = 2*v1 - ic1eq_
//   ic2eq_ = 2*v2 - ic2eq_
//   LP output = v2
//
// Cutoff is clamped to 0.45 * sampleRate to prevent instability near Nyquist.
// (At 0.45: g = tan(pi*0.45) ≈ 9.5; above this the SVF becomes unstable.)
//
// Sources:
//   cytomic.com/files/dsp/SvfLinearTrapOptimised.pdf
//   gist.github.com/hollance/2891d89c57adc71d9560bcf0e1e55c4b

#include <cmath>
#include <algorithm>
#include "SineLUT.h"

namespace xyzpan::dsp {

class SVFLowPass {
public:
    // Zero filter state. Call before processing or after silence.
    void reset() {
        ic1eq_ = 0.0f;
        ic2eq_ = 0.0f;
    }

    // Set or update the low-pass cutoff frequency.
    //   cutoffHz   — desired cutoff in Hz (clamped to 0.45 * sampleRate)
    //   sampleRate — current sample rate in Hz
    //   Q          — resonance (0.7071 = Butterworth, no resonance bump)
    //
    // Safe to call every sample for smooth cutoff modulation.
    void setCoefficients(float cutoffHz, float sampleRate, float Q = 0.7071f) {
        // Clamp to prevent instability near Nyquist
        float safeHz = std::min(cutoffHz, 0.45f * sampleRate);
        if (std::abs(safeHz - lastCutoff_) < 2.0f) return;  // skip when delta < 2 Hz (imperceptible at audio freqs)
        lastCutoff_ = safeHz;
        float g = SineLUT::fastTan(3.14159265f * safeHz / sampleRate);
        float k = 1.0f / Q;
        a1_ = 1.0f / (1.0f + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    // Process one input sample and return the LP output.
    // Safe to call at audio rate with changing coefficients.
    float process(float v0) {
        float v3 = v0 - ic2eq_;
        float v1 = a1_ * ic1eq_ + a2_ * v3;
        float v2 = ic2eq_ + a2_ * ic1eq_ + a3_ * v3;
        ic1eq_ = 2.0f * v1 - ic1eq_;
        ic2eq_ = 2.0f * v2 - ic2eq_;
        return v2;  // LP output
    }

private:
    float a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
    float ic1eq_ = 0.0f, ic2eq_ = 0.0f;
    float lastCutoff_ = -1.0f;
};

} // namespace xyzpan::dsp
