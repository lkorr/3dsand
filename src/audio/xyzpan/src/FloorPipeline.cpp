#include "xyzpan/FloorPipeline.h"
#include "xyzpan/Types.h"
#include "xyzpan/Constants.h"
#include <algorithm>
#include <cmath>

namespace xyzpan {

void FloorPipeline::prepare(float sr) {
    int floorCap = static_cast<int>(kFloorDelayMaxMs * 0.001f * sr) + 8;
    delayL.prepare(floorCap);
    delayR.prepare(floorCap);
    delayL.reset();
    delayR.reset();
    lpfL.setCoefficients(kFloorAbsHz, sr);
    lpfR.setCoefficients(kFloorAbsHz, sr);
    lpfL.reset();
    lpfR.reset();
    gainSmooth.prepare(kDefaultSmoothMs_Gain, sr);
    gainSmooth.reset(0.0f);
    delaySmooth.prepare(kDefaultSmoothMs_Gain, sr);
    delaySmooth.reset(2.0f);
}

void FloorPipeline::reset() {
    delayL.reset();
    delayR.reset();
    lpfL.reset();
    lpfR.reset();
    gainSmooth.reset(0.0f);
    delaySmooth.reset(2.0f);
}

void FloorPipeline::processSample(float& dL, float& dR, float elevFactor, float sr,
                                   float floorGainLin, const EngineParams& params) {
    // elevFactor: 0.0 = below (max floor bounce), 1.0 = above (no bounce)
    const float belowFactor    = 1.0f - elevFactor;
    const float floorDelaySamp = elevFactor * delayMaxSamp_;
    const float floorLinTarget = floorGainLin * belowFactor;

    delayL.push(dL);
    delayR.push(dR);

    const float floorGain = gainSmooth.process(floorLinTarget);
    const float floorReadSamp = std::max(2.0f, delaySmooth.process(floorDelaySamp));

    if (!params.bypassFloor && floorGain > 1e-6f) {
        float floorL = delayL.read(floorReadSamp);
        float floorR = delayR.read(floorReadSamp);
        floorL = lpfL.process(floorL);
        floorR = lpfR.process(floorR);
        dL += floorL * floorGain;
        dR += floorR * floorGain;
    }
}

} // namespace xyzpan
