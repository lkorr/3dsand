#pragma once
#include "xyzpan/Constants.h"
#include "xyzpan/dsp/FractionalDelayLine.h"
#include "xyzpan/dsp/SVFLowPass.h"
#include "xyzpan/dsp/OnePoleSmooth.h"
#include "xyzpan/dsp/FeedbackCombFilter.h"
#include "xyzpan/dsp/BiquadFilter.h"
#include <array>

namespace xyzpan {

// Per-source binaural DSP state. When stereo width > 0, L and R input channels
// are each processed through an independent BinauralPipeline with the same
// coefficients but separate filter state. Shared stages (chest/floor bounce,
// distance, reverb) run once on the summed binaural output.
struct EngineParams;

struct BinauralPipeline {
    // ITD delay lines — one per ear
    dsp::FractionalDelayLine delayL, delayR;

    // Head shadow SVFs — one per ear
    dsp::SVFLowPass shadowL, shadowR;

    // Rear shadow SVFs — both ears
    dsp::SVFLowPass rearSvfL, rearSvfR;

    // Per-parameter smoothers
    dsp::OnePoleSmooth itdSmooth;
    dsp::OnePoleSmooth shadowCutoffSmooth;
    dsp::OnePoleSmooth shadowCutoffSmoothR;
    dsp::OnePoleSmooth ildGainSmooth;
    dsp::OnePoleSmooth rearCutoffSmooth;

    // Near-field ILD biquads
    dsp::BiquadFilter nearFieldLF_L, nearFieldLF_R;

    // Per-source comb bank (same coefficients as L, independent state)
    std::array<dsp::FeedbackCombFilter, kMaxCombFilters> combBank;
    dsp::OnePoleSmooth combWetSmooth;

    // Per-source mono EQ chain (same coefficients, independent state)
    dsp::BiquadFilter presenceShelf, earCanalPeak, pinnaP1;
    dsp::BiquadFilter pinnaNotch, pinnaNotch2, pinnaShelf;

    // Expanded pinna EQ (P5) — 4 additional bands
    dsp::BiquadFilter shoulderPeak, conchaNotch, upperPinna, tragusNotch;

    void prepare(float sr, int delayCap, float combMaxMs);
    void reset();

    // Process comb bank + mono EQ + binaural split for one source node.
    struct BinauralResult { float left; float right; };
    BinauralResult processSample(float inputSample, float nodeX, float nodeY, float nodeZ,
                                  float sr, float binBlend,
                                  float ildGainBase, float hardpanGainBase,
                                  const EngineParams& params);
};

} // namespace xyzpan
