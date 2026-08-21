#pragma once
#include "xyzpan/dsp/FractionalDelayLine.h"
#include "xyzpan/dsp/SineLUT.h"
#include "xyzpan/Constants.h"
#include <array>
#include <cmath>
#include <algorithm>

namespace xyzpan::dsp {

// Dattorro plate reverb (Jon Dattorro, "Effect Design Part 1", 1997).
// 4 input allpass diffusers → figure-8 tank with 2 modulated allpasses,
// 2 tank delays, per-loop damping, and 6-tap decorrelated stereo output.
// Pre-delay line (FractionalDelayLine) before input, distance-scaled.
// All delay lines sized for 192kHz worst case in prepare().
class FDNReverb {
public:
    void prepare(double sampleRate, int maxBlockSize);
    void reset();

    // Set room size [0,1]: scales all delay lengths proportionally.
    // Only call per-block or at prepare() — never per-sample.
    void setSize(float sizeNorm);

    // Set decay [0,1]: maps to T60 (0 = instant, 1 = ~5s).
    void setDecay(float decayNorm);

    // Set damping [0,1]: one-pole LP pole in each tank loop.
    void setDamping(float d);

    // Set input diffusion [0,1]: scales allpass coefficients.
    // 0 = no diffusion (discrete echoes), 1 = maximum smearing.
    void setDiffusion(float d);

    // Set modulation depth [0,1]: LFO excursion on tank allpasses.
    // 0 = no modulation (metallic), 1 = heavy chorus.
    void setModDepth(float d);

    // Process one stereo sample. preDelaySamp = fractional pre-delay in samples.
    // wetL/wetR are reverb-only output; caller mixes with dry signal.
    void processSample(float inL, float inR,
                       float preDelaySamp,
                       float& wetL, float& wetR);

    // Set wet gain [0,1] for output mixing.
    void setWetDry(float wet);

private:
    // Pre-delay
    FractionalDelayLine preDelayLine_;

    // Input diffusion: 4 series allpass filters
    std::array<FractionalDelayLine, 4> inputAP_;
    std::array<float, 4> inputAPDelays_ = {};  // in samples, scaled

    // Tank: 2 modulated allpasses + 2 delay lines
    std::array<FractionalDelayLine, 2> tankAP_;
    std::array<FractionalDelayLine, 2> tankDelay_;
    std::array<float, 2> tankAPDelays_ = {};   // base delays in samples, scaled
    std::array<float, 2> tankDelayLens_ = {};  // in samples, scaled

    // Output tap positions (scaled)
    float tapA_ = 0.0f;  // early tap from tank delays
    float tapB_ = 0.0f;  // late tap from tank delays
    float tapC_ = 0.0f;  // cross-channel tap from tank allpasses

    // One-pole LP damping state per tank half
    std::array<float, 2> dampState_ = {};

    // Tank cross-feedback state (output of each half feeds the other)
    std::array<float, 2> tankFB_ = {};

    // Modulation LFO
    std::array<float, 2> lfoPhase_ = {};
    std::array<float, 2> lfoInc_ = {};   // phase increment per sample
    float modExcursion_ = 0.0f;          // scaled excursion in samples

    // Coefficients
    float inputDiff1_ = kDatInputDiffCoeff1;   // AP1, AP2
    float inputDiff2_ = kDatInputDiffCoeff2;   // AP3, AP4
    float decayGain_  = 0.0f;   // cross-feedback gain (from T60)
    float damping_    = 0.0f;   // one-pole LP coefficient
    float dampCoeff_  = 1.0f;  // pre-computed (1 - damping_)
    float wetGain_    = 0.0f;

    // DC blocker state (one-pole HPF ~5 Hz per channel)
    float dcStateL_ = 0.0f;
    float dcStateR_ = 0.0f;
    float dcCoeff_  = 0.0f;

    double sampleRate_ = 44100.0;

    // Helper: compute scaled delay length from reference-rate value
    float scaleDelay(int refSamples, float size) const;
};

} // namespace xyzpan::dsp
