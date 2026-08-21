#pragma once
#include "xyzpan/dsp/FractionalDelayLine.h"
#include "xyzpan/dsp/OnePoleLP.h"
#include "xyzpan/dsp/OnePoleSmooth.h"

namespace xyzpan {

// Per-node floor bounce DSP state. When stereo width > 0, L and R nodes each
// get independent floor bounce with separate L/R absorption LPFs.
struct EngineParams;

struct FloorPipeline {
    dsp::FractionalDelayLine delayL, delayR;
    dsp::OnePoleLP lpfL, lpfR;               // separate L/R absorption
    dsp::OnePoleSmooth gainSmooth, delaySmooth;
    float delayMaxSamp_ = 0.0f;              // pre-computed per-block
    void prepare(float sr);
    void reset();
    void setBlockConstants(float sr, float floorDelayMaxMs) { delayMaxSamp_ = floorDelayMaxMs * 0.001f * sr; }

    // Process floor bounce for a single source node (modifies dL/dR in-place).
    // elevFactor: 0.0 (nadir/below) to 1.0 (zenith/above) from T/B virtual ear distance-difference.
    void processSample(float& dL, float& dR, float elevFactor, float sr,
                       float floorGainLin, const EngineParams& params);
};

} // namespace xyzpan
