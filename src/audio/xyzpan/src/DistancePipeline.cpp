#include "xyzpan/DistancePipeline.h"
#include "xyzpan/Types.h"
#include "xyzpan/Constants.h"
#include "xyzpan/dsp/FastMath.h"
#include <algorithm>
#include <cmath>

namespace xyzpan {

void DistancePipeline::prepare(float sr) {
    // Sized at the actual device rate: reads are bounded by delayMaxSamp_,
    // which setBlockConstants derives from distDelayMaxMs (<= kDistDelayMaxMs)
    // at the same rate.
    int distDelayCap = static_cast<int>(kDistDelayMaxMs * 0.001f * sr) + 8;
    dopplerDelay.prepare(distDelayCap);
    dopplerDelay.reset();
    airLPF_L.setCoefficients(kAirAbsMaxHz, sr);
    airLPF_R.setCoefficients(kAirAbsMaxHz, sr);
    airLPF_L.reset();
    airLPF_R.reset();
    airLPF2_L.setCoefficients(kAirAbs2MaxHz, sr);
    airLPF2_R.setCoefficients(kAirAbs2MaxHz, sr);
    airLPF2_L.reset();
    airLPF2_R.reset();
    distDelaySmooth.prepare(kDistSmoothMs, sr);
    distDelaySmooth.reset(2.0f);
    distGainSmooth.prepare(kDefaultSmoothMs_Gain, sr);
    distGainSmooth.reset(1.0f);
}

void DistancePipeline::reset() {
    dopplerDelay.reset();
    airLPF_L.reset();
    airLPF_R.reset();
    airLPF2_L.reset();
    airLPF2_R.reset();
    distDelaySmooth.reset(2.0f);
    distGainSmooth.reset(1.0f);
    prevDelaySamp = 2.0f;
}

float DistancePipeline::processDoppler(float input, float rawNodeDistFrac, float sr,
                                       bool effectiveDoppler, const EngineParams& params) {
    const float delayTargetSamples = std::max(2.0f,
        rawNodeDistFrac * delayMaxSamp_);

    float out;
    if (effectiveDoppler) {
        dopplerDelay.push(input);
        float smoothed = distDelaySmooth.process(delayTargetSamples);
        prevDelaySamp = smoothed;
        const float delaySamp = std::max(2.0f, smoothed);
        out = dopplerDelay.read(delaySamp);
    } else {
        dopplerDelay.push(input);
        distDelaySmooth.process(params.dopplerEnabled ? delayTargetSamples : 2.0f);
        prevDelaySamp = 2.0f;
        out = dopplerDelay.read(2.0f);
    }
    return out;
}

DistancePipeline::DistResult DistancePipeline::processDistance(
    float dL, float dR, float nodeX, float nodeY, float nodeZ,
    float sr, const EngineParams& params) {
    const float rawDist = dsp::fastSqrt(nodeX * nodeX + nodeY * nodeY + nodeZ * nodeZ);
    const float nodeDist = std::max(rawDist, kMinDistance);
    const float nodeDistFrac = std::clamp((nodeDist - kMinDistance) * invMaxRange_, 0.0f, 1.0f);

    const float distGainTarget = evalDistGainCurve(distCurve_, nodeDistFrac);
    const float distGain = distGainSmooth.process(distGainTarget);

    dL *= params.bypassDistGain ? 1.0f : distGain;
    dR *= params.bypassDistGain ? 1.0f : distGain;

    if (!params.bypassAirAbs) {
        dL = airLPF_L.process(dL);
        dR = airLPF_R.process(dR);
        dL = airLPF2_L.process(dL);
        dR = airLPF2_R.process(dR);
    }

    return { dL, dR, nodeDistFrac };
}

} // namespace xyzpan
