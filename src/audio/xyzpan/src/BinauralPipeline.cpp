#include "xyzpan/BinauralPipeline.h"
#include "xyzpan/Types.h"
#include "xyzpan/dsp/FastMath.h"
#include <algorithm>
#include <cmath>

// Distance-difference azimuth: virtual ears at (±h, 0, 0).
// Returns signed factor: +1 = right, -1 = left, 0 = median plane.
static inline float computeAzimuthFactor(float x, float y, float z, float h) {
    if (h < 1e-7f) return 0.0f;
    const float yz2 = y * y + z * z;
    const float distLeft  = xyzpan::dsp::fastSqrt((x + h) * (x + h) + yz2);
    const float distRight = xyzpan::dsp::fastSqrt((x - h) * (x - h) + yz2);
    const float delta = distLeft - distRight;  // positive when source is right of center
    return std::clamp(delta / (2.0f * h), -1.0f, 1.0f);
}

// Distance-difference rear factor: virtual ears at (0, ±h, 0).
// Returns signed factor: +1 = rear, -1 = front, 0 = interaural plane.
static inline float computeRearFactor(float x, float y, float z, float h) {
    if (h < 1e-7f) return 0.0f;
    const float xz2 = x * x + z * z;
    const float distFront = xyzpan::dsp::fastSqrt(xz2 + (y - h) * (y - h));
    const float distBack  = xyzpan::dsp::fastSqrt(xz2 + (y + h) * (y + h));
    const float delta = distFront - distBack;  // positive when source is behind
    return std::clamp(delta / (2.0f * h), -1.0f, 1.0f);
}

namespace xyzpan {

void BinauralPipeline::prepare(float sr, int delayCap, float combMaxMs) {
    delayL.prepare(delayCap);
    delayR.prepare(delayCap);
    shadowL.setCoefficients(kHeadShadowFullOpenHz, sr);
    shadowR.setCoefficients(kHeadShadowFullOpenHz, sr);
    rearSvfL.setCoefficients(kRearShadowFullOpenHz, sr);
    rearSvfR.setCoefficients(kRearShadowFullOpenHz, sr);
    itdSmooth.prepare(kDefaultSmoothMs_ITD, sr);
    shadowCutoffSmooth.prepare(kDefaultSmoothMs_Filter, sr);
    shadowCutoffSmoothR.prepare(kDefaultSmoothMs_Filter, sr);
    ildGainSmooth.prepare(kDefaultSmoothMs_Gain, sr);
    rearCutoffSmooth.prepare(kDefaultSmoothMs_Filter, sr);

    int combCap = static_cast<int>(combMaxMs * 0.001f * sr) + 4;
    for (int i = 0; i < kMaxCombFilters; ++i) {
        combBank[i].prepare(combCap);
        combBank[i].setDelay(static_cast<int>(kCombDefaultDelays_ms[i] * 0.001f * sr));
        combBank[i].setFeedback(kCombDefaultFeedback[i]);
    }
    combWetSmooth.prepare(kDefaultSmoothMs_Gain, sr);

    reset();
}

void BinauralPipeline::reset() {
    delayL.reset();
    delayR.reset();
    shadowL.reset();
    shadowR.reset();
    rearSvfL.reset();
    rearSvfR.reset();
    itdSmooth.reset(0.0f);
    shadowCutoffSmooth.reset(kHeadShadowFullOpenHz);
    shadowCutoffSmoothR.reset(kHeadShadowFullOpenHz);
    ildGainSmooth.reset(1.0f);
    rearCutoffSmooth.reset(kRearShadowFullOpenHz);
    nearFieldLF_L.reset();
    nearFieldLF_R.reset();
    for (auto& c : combBank) c.reset();
    combWetSmooth.reset(0.0f);
    presenceShelf.reset();
    earCanalPeak.reset();
    pinnaP1.reset();
    pinnaNotch.reset();
    pinnaNotch2.reset();
    pinnaShelf.reset();
    shoulderPeak.reset();
    conchaNotch.reset();
    upperPinna.reset();
    tragusNotch.reset();
}

BinauralPipeline::BinauralResult BinauralPipeline::processSample(
    float inputSample, float nodeX, float nodeY, float nodeZ,
    float sr, float binBlend,
    float ildGainBase, float hardpanGainBase,
    const EngineParams& params) {
    // Distance-difference virtual ear model — replaces angle-based azimuth + cylinder blend
    const float nodeEffAzimuth = computeAzimuthFactor(nodeX, nodeY, nodeZ, params.azimuthEarOffset);
    const float nodeRearFactor = computeRearFactor(nodeX, nodeY, nodeZ, params.rearEarOffset);

    // Per-node binaural cue targets (signed azimuth: +right, -left)
    const float nodeAbsEffAzimuth = std::abs(nodeEffAzimuth);
    const float nodeItdTarget = params.maxITD_ms * nodeEffAzimuth * (sr * 0.001f);
    const float nodeShadowRange = params.headShadowMinHz - params.headShadowFullOpenHz;
    const float nodeShadowCutTargetL = params.headShadowFullOpenHz + nodeShadowRange * std::max(0.0f,  nodeEffAzimuth);
    const float nodeShadowCutTargetR = params.headShadowFullOpenHz + nodeShadowRange * std::max(0.0f, -nodeEffAzimuth);
    const float nodeIldTarget = 1.0f - (1.0f - ildGainBase) * nodeAbsEffAzimuth;
    const float nodeRearAmount = std::max(0.0f, nodeRearFactor);
    const float nodeRearCutTarget = kRearShadowFullOpenHz
        + (params.rearShadowMinHz - kRearShadowFullOpenHz) * nodeRearAmount;
    const float nodeCombWetTarget = params.combWetMax * std::max(0.0f, nodeRearFactor);

    // Comb bank — smoother always runs; gate filter chain when wet ≈ 0
    const float combWet = combWetSmooth.process(nodeCombWetTarget);
    float depthOut;
    if (params.bypassComb || combWet < 1e-6f) {
        depthOut = inputSample;
    } else {
        float combSig = inputSample;
        for (int c = 0; c < kMaxCombFilters; ++c)
            combSig = combBank[c].process(combSig);
        depthOut = inputSample * (1.0f - combWet) + combSig * combWet;
    }

    // Mono EQ chain — bypass skips biquad .process() calls
    float monoEQ;
    if (params.bypassPinnaEQ) {
        monoEQ = depthOut;
    } else {
        monoEQ = presenceShelf.process(depthOut);
        monoEQ = earCanalPeak.process(monoEQ);
        if (!params.bypassExpandedPinna) {
            monoEQ = shoulderPeak.process(monoEQ);
            monoEQ = conchaNotch.process(monoEQ);
        }
        monoEQ = pinnaP1.process(monoEQ);
        monoEQ = pinnaNotch.process(monoEQ);
        monoEQ = pinnaNotch2.process(monoEQ);
        if (!params.bypassExpandedPinna) {
            monoEQ = upperPinna.process(monoEQ);
            monoEQ = tragusNotch.process(monoEQ);
        }
        monoEQ = pinnaShelf.process(monoEQ);
    }

    // Smooth binaural parameters — smoothers always run
    const float itdSamples      = itdSmooth.process(nodeItdTarget);
    const float smoothedShadowL = shadowCutoffSmooth.process(nodeShadowCutTargetL);
    const float smoothedShadowR = shadowCutoffSmoothR.process(nodeShadowCutTargetR);
    const float ildGain         = ildGainSmooth.process(nodeIldTarget);
    const float smoothedRearCut = rearCutoffSmooth.process(nodeRearCutTarget);

    // Update SVF coefficients per-sample (TPT topology is modulation-safe)
    shadowL.setCoefficients(smoothedShadowL, sr);
    shadowR.setCoefficients(smoothedShadowR, sr);
    rearSvfL.setCoefficients(smoothedRearCut, sr);
    rearSvfR.setCoefficients(smoothedRearCut, sr);

    // Push into delay lines — always push to keep delay state consistent
    delayL.push(monoEQ);
    delayR.push(monoEQ);

    constexpr float kMinDelay = 2.0f;
    float dL, dR;
    if (params.bypassITD) {
        dL = delayL.read(kMinDelay);
        dR = delayR.read(kMinDelay);
    } else {
        const float itdL = std::max(0.0f,  itdSamples) * binBlend;
        const float itdR = std::max(0.0f, -itdSamples) * binBlend;
        dL = delayL.read(kMinDelay + itdL);
        dR = delayR.read(kMinDelay + itdR);
    }

    if (!std::isfinite(dL)) dL = 0.0f;
    if (!std::isfinite(dR)) dR = 0.0f;

    // Hardpan — opposite-ear attenuation when binaural is OFF
    if (binBlend < 0.999f) {
        const float hpAmount = 1.0f - binBlend;
        const float hpGain = 1.0f - (1.0f - hardpanGainBase) * nodeAbsEffAzimuth;
        const float appliedGain = 1.0f - hpAmount * (1.0f - hpGain);
        if (nodeEffAzimuth > 0.0f)      dL *= appliedGain;
        else if (nodeEffAzimuth < 0.0f) dR *= appliedGain;
    }

    // ILD — smooth crossfade around ITD zero to avoid gain discontinuity
    if (!params.bypassILD) {
        const float ildAtten = 1.0f - ildGain;
        const float blend = std::clamp(itdSamples / kILDCrossfadeWidth, -1.0f, 1.0f);
        dL *= 1.0f - ildAtten * std::max(0.0f,  blend);
        dR *= 1.0f - ildAtten * std::max(0.0f, -blend);
    }

    // Near-field ILD — coefficients pre-set per-block, only .process() here
    if (!params.bypassNearField) {
        dL = nearFieldLF_L.process(dL);
        dR = nearFieldLF_R.process(dR);
    }

    // Head shadow — coefficients updated per-sample above
    if (!params.bypassHeadShadow) {
        dL = shadowL.process(dL);
        dR = shadowR.process(dR);
    }

    // Rear shadow — coefficients updated per-sample above
    if (!params.bypassRearShadow) {
        dL = rearSvfL.process(dL);
        dR = rearSvfR.process(dR);
    }

    return { dL, dR };
}

} // namespace xyzpan
