#include "xyzpan/dsp/FDNReverb.h"
#include "xyzpan/Constants.h"
#include <cmath>
#include <algorithm>

namespace xyzpan::dsp {

// ============================================================================
// scaleDelay() — convert reference-rate delay to current sample rate + size
// ============================================================================

float FDNReverb::scaleDelay(int refSamples, float size) const {
    return static_cast<float>(refSamples) *
           static_cast<float>(sampleRate_ / kDattorroRefRate) * size;
}

// ============================================================================
// prepare()
// ============================================================================

void FDNReverb::prepare(double sampleRate, int /*maxBlockSize*/) {
    sampleRate_ = sampleRate;

    // Scale factor sized at the actual device rate; setSize clamps size to
    // [0.1, 1.0], so scaleDelay can never exceed refSamples * worstScale.
    const double worstScale = sampleRate / kDattorroRefRate;
    const int margin = 8;

    // Pre-delay line: 50ms at the device rate (param clamped to kVerbPreDelayMaxMs)
    int preDelayCap = static_cast<int>(kVerbPreDelayMaxMs * sampleRate / 1000.0) + margin;
    preDelayLine_.prepare(preDelayCap);

    // Input allpass delay lines
    for (int i = 0; i < 4; ++i) {
        int cap = static_cast<int>(kDatInputAP[i] * worstScale) + margin;
        inputAP_[i].prepare(cap);
    }

    // Tank modulated allpass delay lines (need extra room for modulation excursion)
    for (int i = 0; i < 2; ++i) {
        int cap = static_cast<int>((kDatTankAP[i] + kDatModExcursion + 4) * worstScale) + margin;
        tankAP_[i].prepare(cap);
    }

    // Tank delay lines (need to support taps at kDatTapB which can be > delay length)
    for (int i = 0; i < 2; ++i) {
        int cap = static_cast<int>(kDatTankDelay[i] * worstScale) + margin;
        tankDelay_[i].prepare(cap);
    }

    // Compute initial delay lengths at default size
    setSize(kVerbDefaultSize);

    // LFO phase increments
    lfoInc_[0] = static_cast<float>(kDatModRate1 / sampleRate_);
    lfoInc_[1] = static_cast<float>(kDatModRate2 / sampleRate_);

    // Modulation excursion scaled to current sample rate
    modExcursion_ = kDatModExcursion * static_cast<float>(sampleRate_ / kDattorroRefRate);

    // DC blocker: one-pole HPF at ~5 Hz
    // y[n] = x[n] - x[n-1] + coeff * y[n-1], coeff = 1 - (2*pi*5/sr)
    dcCoeff_ = 1.0f - static_cast<float>(2.0 * 3.14159265358979 * 5.0 / sampleRate_);

    // Zero all state
    reset();
}

// ============================================================================
// reset()
// ============================================================================

void FDNReverb::reset() {
    preDelayLine_.reset();
    for (int i = 0; i < 4; ++i)
        inputAP_[i].reset();
    for (int i = 0; i < 2; ++i) {
        tankAP_[i].reset();
        tankDelay_[i].reset();
    }
    dampState_.fill(0.0f);
    tankFB_.fill(0.0f);
    lfoPhase_.fill(0.0f);
    dcStateL_ = 0.0f;
    dcStateR_ = 0.0f;
}

// ============================================================================
// setSize()
// ============================================================================

void FDNReverb::setSize(float sizeNorm) {
    sizeNorm = std::clamp(sizeNorm, 0.1f, 1.0f);

    for (int i = 0; i < 4; ++i)
        inputAPDelays_[i] = scaleDelay(kDatInputAP[i], sizeNorm);

    for (int i = 0; i < 2; ++i) {
        tankAPDelays_[i]  = scaleDelay(kDatTankAP[i], sizeNorm);
        tankDelayLens_[i] = scaleDelay(kDatTankDelay[i], sizeNorm);
    }

    tapA_ = scaleDelay(kDatTapA, sizeNorm);
    tapB_ = scaleDelay(kDatTapB, sizeNorm);
    tapC_ = scaleDelay(kDatTankAP[0], sizeNorm);  // tap from tank allpass, position = kDatTapC
    // Actually tap C is from the allpass's delay line at a specific position:
    tapC_ = scaleDelay(kDatTapC, sizeNorm);
}

// ============================================================================
// setDecay()
// ============================================================================

void FDNReverb::setDecay(float decayNorm) {
    float t60 = decayNorm * kVerbMaxDecayT60_s;

    // Longest loop path: tankAP + tankDelay for the larger half
    // At reference rate: 908 + 4453 = 5361 samples → ~180ms
    constexpr float maxLoopMs = static_cast<float>(
        (kDatTankAP[1] + kDatTankDelay[0]) * 1000.0 / kDattorroRefRate);

    float safeT60 = std::max(t60, 0.001f);
    decayGain_ = std::pow(10.0f, -3.0f * maxLoopMs / (1000.0f * safeT60));
    decayGain_ = std::clamp(decayGain_, 0.0f, 0.999f);
}

// ============================================================================
// setDamping()
// ============================================================================

void FDNReverb::setDamping(float d) {
    damping_ = std::clamp(d, 0.0f, 0.95f);
    dampCoeff_ = 1.0f - damping_;
}

// ============================================================================
// setDiffusion()
// ============================================================================

void FDNReverb::setDiffusion(float d) {
    d = std::clamp(d, 0.0f, 1.0f);
    inputDiff1_ = kDatInputDiffCoeff1 * d;
    inputDiff2_ = kDatInputDiffCoeff2 * d;
}

// ============================================================================
// setModDepth()
// ============================================================================

void FDNReverb::setModDepth(float d) {
    d = std::clamp(d, 0.0f, 1.0f);
    modExcursion_ = kDatModExcursion * static_cast<float>(sampleRate_ / kDattorroRefRate) * d;
}

// ============================================================================
// setWetDry()
// ============================================================================

void FDNReverb::setWetDry(float wet) {
    wetGain_ = std::clamp(wet, 0.0f, 1.0f);
}

// ============================================================================
// processSample()
// ============================================================================

void FDNReverb::processSample(float inL, float inR,
                               float preDelaySamp,
                               float& wetL, float& wetR) {
    // 1. Push mono sum into pre-delay line
    const float monoIn = (inL + inR) * 0.5f;
    preDelayLine_.push(monoIn);
    const float preDOut = preDelayLine_.readLinear(std::max(2.0f, preDelaySamp));

    // 2. Input diffusion: 4 series allpass filters
    // Schroeder allpass: out = -g*in + delayed + g*(-g*in + delayed)
    // Simplified: v = in - g*delayed; push(v); out = delayed + g*v
    float sig = preDOut;

    // AP1 and AP2 use inputDiff1_ (fixed delays — linear interp)
    for (int i = 0; i < 2; ++i) {
        float delayed = inputAP_[i].readLinear(std::max(2.0f, inputAPDelays_[i]));
        float v = sig - inputDiff1_ * delayed;
        inputAP_[i].push(v);
        sig = delayed + inputDiff1_ * v;
    }

    // AP3 and AP4 use inputDiff2_ (fixed delays — linear interp)
    for (int i = 2; i < 4; ++i) {
        float delayed = inputAP_[i].readLinear(std::max(2.0f, inputAPDelays_[i]));
        float v = sig - inputDiff2_ * delayed;
        inputAP_[i].push(v);
        sig = delayed + inputDiff2_ * v;
    }

    // 3. Tank processing — figure-8 topology
    // Each half: input + cross-feedback → modulated allpass → delay → damping → output
    float tankOut[2];

    for (int h = 0; h < 2; ++h) {
        // Cross-feedback from the OTHER half
        float tankIn = sig + decayGain_ * tankFB_[1 - h];

        // Modulated allpass: delay modulated by LFO
        float lfoVal = SineLUT::lookup(lfoPhase_[h]);
        float modDelay = std::max(2.0f, tankAPDelays_[h] + modExcursion_ * lfoVal);

        float delayed = tankAP_[h].read(modDelay);
        float v = tankIn - kDatDecayDiffCoeff * delayed;
        tankAP_[h].push(v);
        float apOut = delayed + kDatDecayDiffCoeff * v;

        // Advance LFO phase
        lfoPhase_[h] += lfoInc_[h];
        if (lfoPhase_[h] >= 1.0f) lfoPhase_[h] -= 1.0f;

        // Tank delay line (fixed delay — linear interp)
        tankDelay_[h].push(apOut);
        float delayOut = tankDelay_[h].readLinear(std::max(2.0f, tankDelayLens_[h]));

        // One-pole LP damping
        dampState_[h] = dampCoeff_ * delayOut + damping_ * dampState_[h];

        // Store feedback for the other half (will be used next sample)
        tankFB_[h] = dampState_[h];
        tankOut[h] = dampState_[h];
    }

    // 4. Output taps — decorrelated stereo from Dattorro Table 1
    // L: +tap(tankDelay0, tapA) +tap(tankDelay0, tapB) -tap(tankAP1, tapC)
    // R: +tap(tankDelay1, tapA) +tap(tankDelay1, tapB) -tap(tankAP0, tapC)
    // Output taps — fixed positions, linear interp sufficient
    float rawL = tankDelay_[0].readLinear(std::max(2.0f, tapA_))
               + tankDelay_[0].readLinear(std::max(2.0f, tapB_))
               - tankAP_[1].readLinear(std::max(2.0f, tapC_));

    float rawR = tankDelay_[1].readLinear(std::max(2.0f, tapA_))
               + tankDelay_[1].readLinear(std::max(2.0f, tapB_))
               - tankAP_[0].readLinear(std::max(2.0f, tapC_));

    // Scale output (3 taps summed, normalize)
    rawL *= 0.33f;
    rawR *= 0.33f;

    // 5. DC blocker (one-pole HPF)
    float dcOutL = rawL - dcStateL_;
    dcStateL_ = rawL - dcCoeff_ * dcOutL;
    float dcOutR = rawR - dcStateR_;
    dcStateR_ = rawR - dcCoeff_ * dcOutR;

    wetL = dcOutL * wetGain_;
    wetR = dcOutR * wetGain_;
}

} // namespace xyzpan::dsp
