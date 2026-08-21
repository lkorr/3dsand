#pragma once
#include "xyzpan/dsp/SVFFilter.h"
#include "xyzpan/dsp/OnePoleLP.h"
#include "xyzpan/dsp/FractionalDelayLine.h"
#include "xyzpan/dsp/OnePoleSmooth.h"
#include <array>

namespace xyzpan {

struct EngineParams;

// Per-node chest bounce DSP state. When stereo width > 0, L and R nodes each
// get independent chest bounce processing driven by their own Z position.
struct ChestPipeline {
    std::array<dsp::SVFFilter, 4> hpf;       // 4x HP cascade at 700Hz
    dsp::OnePoleLP lp;                        // 1x 6dB/oct LP at 1kHz
    dsp::FractionalDelayLine delay;           // 0–2ms delay
    dsp::OnePoleSmooth gainSmooth, delaySmooth;
    float delayMaxSamp_ = 0.0f;              // pre-computed per-block
    void prepare(float sr);
    void reset();
    void setBlockConstants(float sr, float chestDelayMaxMs) { delayMaxSamp_ = chestDelayMaxMs * 0.001f * sr; }

    // Process chest bounce for a single source node.
    // elevFactor: 0.0 (nadir/below) to 1.0 (zenith/above) from T/B virtual ear distance-difference.
    // Returns the chest bounce signal to be added to both ears.
    float processSample(float input, float elevFactor, float sr,
                        float chestGainLin, const EngineParams& params);
};

} // namespace xyzpan
