#pragma once
#include "xyzpan/Constants.h"
#include "xyzpan/dsp/FractionalDelayLine.h"
#include "xyzpan/dsp/OnePoleLP.h"
#include "xyzpan/dsp/OnePoleSmooth.h"

namespace xyzpan {

// Per-source distance DSP state. When stereo width > 0, L and R input channels
// each get independent distance processing (gain attenuation, delay+doppler,
// air absorption) based on their own node positions.
struct EngineParams;

struct DistancePipeline {
    dsp::FractionalDelayLine dopplerDelay;        // mono doppler delay line
    dsp::OnePoleLP airLPF_L, airLPF_R;            // air absorption stage 1 (stereo, post-binaural)
    dsp::OnePoleLP airLPF2_L, airLPF2_R;          // air absorption stage 2 (stereo, post-binaural)
    dsp::OnePoleSmooth distGainSmooth;
    dsp::OnePoleSmooth distDelaySmooth;
    float prevDelaySamp = 2.0f;  // rate limiter state for doppler
    float delayMaxSamp_ = 0.0f; // pre-computed per-block
    DistGainCurve distCurve_;    // pre-computed per-block gain curve coefficients
    float invMaxRange_ = 1.0f;   // 1 / (sphereRadius - kMinDistance), pre-computed per-block
    void prepare(float sr);
    void reset();
    void setBlockConstants(float sr, float distDelayMaxMs,
                           const DistGainCurve& curve, float invMaxRange) {
        delayMaxSamp_ = distDelayMaxMs * 0.001f * sr;
        distCurve_ = curve;
        invMaxRange_ = invMaxRange;
    }

    // Mono doppler delay (applied before comb/binaural).
    float processDoppler(float input, float rawNodeDistFrac, float sr,
                         bool effectiveDoppler, const EngineParams& params);

    // Distance processing for a single source node (gain + air absorption).
    // Uses the per-block distCurve_/invMaxRange_ set via setBlockConstants().
    struct DistResult { float left; float right; float distFrac; };
    DistResult processDistance(float dL, float dR, float nodeX, float nodeY, float nodeZ,
                               float sr, const EngineParams& params);
};

} // namespace xyzpan
