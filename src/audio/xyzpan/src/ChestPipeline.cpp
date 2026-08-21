#include "xyzpan/ChestPipeline.h"
#include "xyzpan/Types.h"
#include "xyzpan/Constants.h"
#include <algorithm>
#include <cmath>

namespace xyzpan {

void ChestPipeline::prepare(float sr) {
    for (auto& hp : hpf) {
        hp.setType(dsp::SVFType::HP);
        hp.setCoefficients(kChestHPFHz, sr);
        hp.reset();
    }
    lp.setCoefficients(kChestLPHz, sr);
    lp.reset();
    int chestCap = static_cast<int>(kChestDelayMaxMs * 0.001f * sr) + 8;
    delay.prepare(chestCap);
    delay.reset();
    gainSmooth.prepare(kDefaultSmoothMs_Gain, sr);
    gainSmooth.reset(0.0f);
    delaySmooth.prepare(kDefaultSmoothMs_Gain, sr);
    delaySmooth.reset(2.0f);
}

void ChestPipeline::reset() {
    for (auto& hp : hpf) hp.reset();
    lp.reset();
    delay.reset();
    gainSmooth.reset(0.0f);
    delaySmooth.reset(2.0f);
}

float ChestPipeline::processSample(float input, float elevFactor, float sr,
                                    float chestGainLin, const EngineParams& params) {
    // elevFactor: 0.0 = below (max chest bounce), 1.0 = above (no bounce)
    const float belowFactor    = 1.0f - elevFactor;
    const float chestDelaySamp = elevFactor * delayMaxSamp_;
    const float chestLinTarget = chestGainLin * belowFactor;

    const float chestGain = gainSmooth.process(chestLinTarget);
    const float chestReadSamp = std::max(2.0f, delaySmooth.process(chestDelaySamp));

    if (!params.bypassChest) {
        float chestSig = input;
        for (auto& hp_f : hpf)
            chestSig = hp_f.process(chestSig);
        chestSig = lp.process(chestSig);
        delay.push(chestSig);

        if (chestGain > 1e-6f)
            return delay.read(chestReadSamp) * chestGain;
        return 0.0f;
    }

    delay.push(input);
    return 0.0f;
}

} // namespace xyzpan
