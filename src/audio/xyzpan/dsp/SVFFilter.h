#pragma once
// SVFFilter.h
// Generalised TPT (Topology-Preserving Transform) State Variable Filter.
// Andy Simper / Cytomic formulation — supports LP, HP, BP, and Notch output modes.
//
// Parallel class to SVFLowPass — do NOT modify SVFLowPass (Phase 2 uses it).
// The same state-update equations are used; only the output mix differs.
//
// Mode mix coefficients (from Cytomic SvfLinearTrapOptimised.pdf):
//   LP:    output = v2
//   HP:    output = v0 - k*v1 - v2
//   BP:    output = k * v1
//   Notch: output = v0 - k*v1
//
// Coefficient formula (identical to SVFLowPass):
//   g  = tan(pi * cutoffHz / sampleRate)    [frequency warping]
//   k  = 1/Q                                [damping; Q=0.7071 = Butterworth]
//   a1 = 1 / (1 + g*(g + k))
//   a2 = g * a1
//   a3 = g * a2
//
// Cutoff is clamped to 0.45 * sampleRate to prevent instability near Nyquist.
// setCoefficients() is safe to call every sample for smooth modulation.
//
// Used for: chest bounce 4x HPF cascade (ELEV-03).
//
// Sources:
//   cytomic.com/files/dsp/SvfLinearTrapOptimised.pdf
//   gist.github.com/hollance/2891d89c57adc71d9560bcf0e1e55c4b

#include <cmath>
#include <algorithm>
#include "SineLUT.h"

namespace xyzpan::dsp {

enum class SVFType { LP, HP, BP, Notch };

class SVFFilter {
public:
    // Zero filter state. Call before processing or after silence.
    void reset() {
        ic1eq_ = 0.0f;
        ic2eq_ = 0.0f;
    }

    // Select the output mode (LP / HP / BP / Notch).
    void setType(SVFType type) { type_ = type; }

    // Set or update the filter cutoff frequency.
    //   cutoffHz   — desired cutoff in Hz (clamped to 0.45 * sampleRate)
    //   sampleRate — current sample rate in Hz
    //   Q          — resonance (0.7071 = Butterworth, no resonance bump)
    //
    // Safe to call every sample for smooth cutoff modulation.
    void setCoefficients(float cutoffHz, float sampleRate, float Q = 0.7071f) {
        float safeHz = std::min(cutoffHz, 0.45f * sampleRate);
        if (std::abs(safeHz - lastCutoff_) < 2.0f) return;  // skip when delta < 2 Hz (imperceptible at audio freqs)
        lastCutoff_ = safeHz;
        float g = SineLUT::fastTan(3.14159265f * safeHz / sampleRate);
        k_  = 1.0f / Q;
        a1_ = 1.0f / (1.0f + g * (g + k_));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    // Process one input sample and return the output for the selected mode.
    // Safe to call at audio rate with changing coefficients.
    float process(float v0) {
        float v3 = v0 - ic2eq_;
        float v1 = a1_ * ic1eq_ + a2_ * v3;
        float v2 = ic2eq_ + a2_ * ic1eq_ + a3_ * v3;
        ic1eq_ = 2.0f * v1 - ic1eq_;
        ic2eq_ = 2.0f * v2 - ic2eq_;

        switch (type_) {
            case SVFType::LP:    return v2;
            case SVFType::HP:    return v0 - k_ * v1 - v2;
            case SVFType::BP:    return k_ * v1;
            case SVFType::Notch: return v0 - k_ * v1;
            default:             return v2;
        }
    }

private:
    SVFType type_    = SVFType::LP;
    float   k_       = 1.4142f;  // 1/Q — default Butterworth (Q=0.7071)
    float   a1_      = 0.0f;
    float   a2_      = 0.0f;
    float   a3_      = 0.0f;
    float   ic1eq_   = 0.0f;
    float   ic2eq_   = 0.0f;
    float   lastCutoff_ = -1.0f;
};

} // namespace xyzpan::dsp
