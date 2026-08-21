#include "xyzpan/ERPipeline.h"
#include "xyzpan/Types.h"
#include "xyzpan/Constants.h"
#include "xyzpan/dsp/FastMath.h"
#include <algorithm>
#include <cmath>

// Distance-difference azimuth: virtual ears at (±h, 0, 0).
static inline float computeAzimuthFactor(float x, float y, float z, float h) {
    if (h < 1e-7f) return 0.0f;
    const float yz2 = y * y + z * z;
    const float distLeft  = xyzpan::dsp::fastSqrt((x + h) * (x + h) + yz2);
    const float distRight = xyzpan::dsp::fastSqrt((x - h) * (x - h) + yz2);
    const float delta = distLeft - distRight;
    return std::clamp(delta / (2.0f * h), -1.0f, 1.0f);
}

// Distance-difference rear factor: virtual ears at (0, ±h, 0).
static inline float computeRearFactor(float x, float y, float z, float h) {
    if (h < 1e-7f) return 0.0f;
    const float xz2 = x * x + z * z;
    const float distFront = xyzpan::dsp::fastSqrt(xz2 + (y - h) * (y - h));
    const float distBack  = xyzpan::dsp::fastSqrt(xz2 + (y + h) * (y + h));
    const float delta = distFront - distBack;
    return std::clamp(delta / (2.0f * h), -1.0f, 1.0f);
}

// Elevation factor: virtual ears at (0, 0, ±h). Returns 0.0 (nadir) to 1.0 (zenith).
static inline float computeElevFactor(float x, float y, float z, float h) {
    if (h < 1e-7f) return 0.5f;
    const float xy2 = x * x + y * y;
    const float distTop    = xyzpan::dsp::fastSqrt(xy2 + (z - h) * (z - h));
    const float distBottom = xyzpan::dsp::fastSqrt(xy2 + (z + h) * (z + h));
    const float delta = distBottom - distTop;
    const float maxDelta = 2.0f * h;
    return std::clamp(delta / maxDelta * 0.5f + 0.5f, 0.0f, 1.0f);
}

namespace xyzpan {

// Shared image source + rotation helper used by both updateTapDirectionalCoeffs
// and processSample. Computes listener-relative rotated image source position
// for a given wall tap.
static inline void computeRotatedImageSource(
    int axis, float sign,
    float nodeX, float nodeY, float nodeZ,
    float listenerX, float listenerY, float listenerZ,
    float roomHalf,
    bool rotated, float cY, float sY, float cP, float sP, float cR, float sR,
    float& outX, float& outY, float& outZ) {
    float imgX = nodeX, imgY = nodeY, imgZ = nodeZ;
    if (axis == 0)      { const float wRel = sign * roomHalf - listenerX; imgX = 2.0f * wRel - nodeX; }
    else if (axis == 1) { const float wRel = sign * roomHalf - listenerY; imgY = 2.0f * wRel - nodeY; }
    else                { const float wRel = sign * roomHalf - listenerZ; imgZ = 2.0f * wRel - nodeZ; }

    outX = imgX; outY = imgY; outZ = imgZ;
    if (rotated) {
        const float rx = imgX * cY + imgY * sY;
        const float ry = -imgX * sY + imgY * cY;
        outX = rx;
        outY = ry * cP + imgZ * sP;
        outZ = -ry * sP + imgZ * cP;
        const float tmpX = outX * cR + outZ * sR;
        outZ = -outX * sR + outZ * cR;
        outX = tmpX;
    }
}

void ERPipeline::prepare(float sr) {
    // Sized at the actual device rate: the read-side clamp in processSample
    // (delayCap = kERMaxDelayMs at sr) uses the same formula.
    int erDelayCap = static_cast<int>(kERMaxDelayMs * 0.001f * sr) + 8;
    sharedDelay.prepare(erDelayCap);
    sharedDelay.reset();

    int itdCap = static_cast<int>(kMaxITDUpperBound_ms * 0.001f * sr) + 8;
    for (auto& er : reflections) {
        er.wallAbsorption.setCoefficients(kERDampingLPMaxHz, sr);
        er.wallAbsorption.reset();
        er.delaySmooth.prepare(kDefaultSmoothMs_ITD, sr);
        er.delaySmooth.reset(2.0f);
        er.gainSmooth.prepare(kDefaultSmoothMs_Gain, sr);
        er.gainSmooth.reset(0.0f);
        er.itdDelayL.prepare(itdCap);
        er.itdDelayR.prepare(itdCap);
        er.itdDelayL.reset();
        er.itdDelayR.reset();
        er.shadowL.setCoefficients(kHeadShadowFullOpenHz, sr);
        er.shadowR.setCoefficients(kHeadShadowFullOpenHz, sr);
        er.shadowL.reset();
        er.shadowR.reset();
        er.itdSmooth.prepare(kDefaultSmoothMs_ITD, sr);
        er.itdSmooth.reset(0.0f);
        er.ildSmooth.prepare(kDefaultSmoothMs_Gain, sr);
        er.ildSmooth.reset(1.0f);

        // Pinna EQ biquads — no initial coefficients needed, set per-block
        er.presenceShelf.reset();
        er.earCanalPeak.reset();
        er.shoulderPeak.reset();
        er.conchaNotch.reset();
        er.pinnaNotch.reset();
        er.pinnaNotch2.reset();
        er.upperPinna.reset();
        er.tragusNotch.reset();
        er.pinnaShelf.reset();

        // Rear shadow SVFs
        er.rearSvfL.setCoefficients(kRearShadowFullOpenHz, sr);
        er.rearSvfR.setCoefficients(kRearShadowFullOpenHz, sr);
        er.rearSvfL.reset();
        er.rearSvfR.reset();
        er.rearCutoffSmooth.prepare(kDefaultSmoothMs_Filter, sr);
        er.rearCutoffSmooth.reset(kRearShadowFullOpenHz);

        // Near-field biquads
        er.nearFieldLF_L.reset();
        er.nearFieldLF_R.reset();
    }
}

void ERPipeline::reset() {
    sharedDelay.reset();
    for (auto& er : reflections) {
        er.wallAbsorption.reset();
        er.delaySmooth.reset(2.0f);
        er.gainSmooth.reset(0.0f);
        er.itdDelayL.reset();
        er.itdDelayR.reset();
        er.shadowL.reset();
        er.shadowR.reset();
        er.itdSmooth.reset(0.0f);
        er.ildSmooth.reset(1.0f);

        er.presenceShelf.reset();
        er.earCanalPeak.reset();
        er.shoulderPeak.reset();
        er.conchaNotch.reset();
        er.pinnaNotch.reset();
        er.pinnaNotch2.reset();
        er.upperPinna.reset();
        er.tragusNotch.reset();
        er.pinnaShelf.reset();

        er.rearSvfL.reset();
        er.rearSvfR.reset();
        er.rearCutoffSmooth.reset(kRearShadowFullOpenHz);

        er.nearFieldLF_L.reset();
        er.nearFieldLF_R.reset();
    }
}

void ERPipeline::updateWallAbsorption(float dampCutoff, float sr, int blockSize) {
    for (auto& er : reflections)
        er.wallAbsorption.setCoefficientsSmoothed(dampCutoff, sr, blockSize);
}

void ERPipeline::updateTapDirectionalCoeffs(
    float nodeX, float nodeY, float nodeZ,
    float listenerX, float listenerY, float listenerZ,
    float roomHalf, float sr, int blockSize,
    bool rotated, float cY, float sY, float cP, float sP, float cR, float sR,
    float sphereRadius,
    const EngineParams& params) {

    constexpr int wallAxis[kNumER]   = { 0, 0, 1, 1, 2, 2 };
    constexpr float wallSign[kNumER] = { 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f };

    for (int w = 0; w < kNumER; ++w) {
        auto& er = reflections[w];

        float lrX, lrY, lrZ;
        computeRotatedImageSource(wallAxis[w], wallSign[w],
            nodeX, nodeY, nodeZ, listenerX, listenerY, listenerZ, roomHalf,
            rotated, cY, sY, cP, sP, cR, sR, lrX, lrY, lrZ);

        const float rearFactor = computeRearFactor(lrX, lrY, lrZ, params.rearEarOffset);
        const float elevFactor = computeElevFactor(lrX, lrY, lrZ, params.elevEarOffset);
        const float azFactor   = computeAzimuthFactor(lrX, lrY, lrZ, params.azimuthEarOffset);

        // Pinna EQ targets — same formulas as Engine.cpp direct sound path
        const float presGainDb  = params.presenceShelfMaxDb * (-rearFactor);
        const float earGainDb   = std::min(0.0f, params.earCanalMaxDb * (-rearFactor));
        const float elevAbove   = std::max(0.0f, elevFactor * 2.0f - 1.0f);
        const float pinnaGainDb = -15.0f + 20.0f * elevAbove;
        const float shelfGainDb = 3.0f * std::min(1.0f, elevFactor * 2.0f);
        const float n1Freq      = std::clamp(
            params.pinnaN1MinHz + (params.pinnaN1MaxHz - params.pinnaN1MinHz) * elevFactor,
            params.pinnaN1MinHz, params.pinnaN1MaxHz);
        const float n2Freq      = n1Freq + params.pinnaN2OffsetHz;

        er.presenceShelf.setCoefficientsSmoothed(dsp::BiquadType::HighShelf,
            params.presenceShelfFreqHz, sr, 0.7071f, presGainDb, blockSize);
        er.earCanalPeak.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
            params.earCanalFreqHz, sr, params.earCanalQ, earGainDb, blockSize);
        er.pinnaNotch.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
            n1Freq, sr, params.pinnaNotchQ, pinnaGainDb, blockSize);
        er.pinnaNotch2.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
            n2Freq, sr, params.pinnaN2Q, params.pinnaN2GainDb, blockSize);
        er.pinnaShelf.setCoefficientsSmoothed(dsp::BiquadType::HighShelf,
            params.pinnaShelfFreqHz, sr, 0.7071f, shelfGainDb, blockSize);

        // Expanded pinna EQ (P5)
        const float belowFactor = 1.0f - elevFactor;
        const float shoulderGainDb = params.shoulderPeakMaxDb * belowFactor;
        const float conchaGainDb   = params.conchaNotchMaxDb * belowFactor;
        const float upperGainDb    = params.upperPinnaMinDb
            + (params.upperPinnaMaxDb - params.upperPinnaMinDb) * elevFactor;
        const float tragusRear  = std::max(0.0f, rearFactor);
        const float tragusGainDb = params.tragusNotchMaxDb * tragusRear * belowFactor;

        er.shoulderPeak.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
            params.shoulderPeakFreqHz, sr, params.shoulderPeakQ, shoulderGainDb, blockSize);
        er.conchaNotch.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
            params.conchaNotchFreqHz, sr, params.conchaNotchQ, conchaGainDb, blockSize);
        er.upperPinna.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
            params.upperPinnaFreqHz, sr, params.upperPinnaQ, upperGainDb, blockSize);
        er.tragusNotch.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
            params.tragusNotchFreqHz, sr, params.tragusNotchQ, tragusGainDb, blockSize);

        // Near-field LF boost — proximity from image source distance
        const float imgDist = dsp::fastSqrt(lrX * lrX + lrY * lrY + lrZ * lrZ);
        const float maxRange = std::max(sphereRadius - kMinDistance, 0.001f);
        const float prox = 1.0f - std::clamp((imgDist - kMinDistance) / maxRange, 0.0f, 1.0f);
        const float nfBaseDb = params.nearFieldLFMaxDb * prox;
        const float nfGainR = nfBaseDb * std::max(0.0f,  azFactor);
        const float nfGainL = nfBaseDb * std::max(0.0f, -azFactor);

        er.nearFieldLF_R.setCoefficientsSmoothed(dsp::BiquadType::LowShelf,
            params.nearFieldLFHz, sr, 0.7071f, nfGainR, blockSize);
        er.nearFieldLF_L.setCoefficientsSmoothed(dsp::BiquadType::LowShelf,
            params.nearFieldLFHz, sr, 0.7071f, nfGainL, blockSize);
    }
}

ERPipeline::ERResult ERPipeline::processSample(
    float input, float nodeX, float nodeY, float nodeZ,
    float listenerX, float listenerY, float listenerZ,
    float distGainTarget, float sr,
    float roomHalf,
    float ildGainBase, bool rotated,
    float cY, float sY, float cP, float sP,
    float cR, float sR,
    const EngineParams& params) {
    sharedDelay.push(input);

    float erDirectL = 0.0f, erDirectR = 0.0f;
    float erRevL = 0.0f, erRevR = 0.0f;

    constexpr int wallAxis[6]  = { 0, 0, 1, 1, 2, 2 };
    constexpr float wallSign[6] = { 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f };

    const float rawDist = dsp::fastSqrt(nodeX * nodeX + nodeY * nodeY + nodeZ * nodeZ);
    const float directDist = std::max(rawDist, kMinDistance);

    // Wall-invariant scalars — hoisted from the 6-wall loop (identical every iteration)
    const float delayCap = kERMaxDelayMs * 0.001f * sr;
    const float srOverSpeed = sr / kSpeedOfSound;
    const float invRoomHalf = 1.0f / std::max(roomHalf, kMinDistance);
    const float itdScale = params.maxITD_ms * sr * 0.001f;
    const float erShadowRange = params.headShadowMinHz - params.headShadowFullOpenHz;

    for (int w = 0; w < kNumER; ++w) {
        auto& er = reflections[w];

        float imgX = nodeX, imgY = nodeY, imgZ = nodeZ;
        const int axis = wallAxis[w];
        const float sign = wallSign[w];
        if (axis == 0)      { const float wRel = sign * roomHalf - listenerX; imgX = 2.0f * wRel - nodeX; }
        else if (axis == 1) { const float wRel = sign * roomHalf - listenerY; imgY = 2.0f * wRel - nodeY; }
        else                { const float wRel = sign * roomHalf - listenerZ; imgZ = 2.0f * wRel - nodeZ; }

        const float pathMeters = dsp::fastSqrt(imgX * imgX + imgY * imgY + imgZ * imgZ);
        const float delaySamp = std::clamp(pathMeters * srOverSpeed, 2.0f, delayCap);

        const float pathNorm = pathMeters * invRoomHalf;
        const float reflDist = std::max(pathNorm, kMinDistance);
        const float ratioPenalty = directDist / reflDist;
        const float gainTarget = std::clamp(distGainTarget * ratioPenalty, 0.0f, params.distGainMax);

        const float smoothDelay = std::max(2.0f, er.delaySmooth.process(delaySamp));
        const float smoothGain = er.gainSmooth.process(gainTarget);

        float reflected = sharedDelay.read(smoothDelay);
        reflected = er.wallAbsorption.process(reflected);
        reflected *= smoothGain;

        // Pinna EQ chain (mono, before binaural split) — coefficients set per-block
        if (!params.bypassPinnaEQ) {
            reflected = er.presenceShelf.process(reflected);
            reflected = er.earCanalPeak.process(reflected);
            if (!params.bypassExpandedPinna) {
                reflected = er.shoulderPeak.process(reflected);
                reflected = er.conchaNotch.process(reflected);
            }
            reflected = er.pinnaNotch.process(reflected);
            reflected = er.pinnaNotch2.process(reflected);
            if (!params.bypassExpandedPinna) {
                reflected = er.upperPinna.process(reflected);
                reflected = er.tragusNotch.process(reflected);
            }
            reflected = er.pinnaShelf.process(reflected);
        }

        // Head rotation — produce listener-relative image source coords with Z
        float lrImgX = imgX, lrImgY = imgY, lrImgZ = imgZ;
        if (rotated) {
            const float rx = imgX * cY + imgY * sY;
            const float ry = -imgX * sY + imgY * cY;
            lrImgX = rx;
            lrImgY = ry * cP + imgZ * sP;
            lrImgZ = -ry * sP + imgZ * cP;
            const float tmpX = lrImgX * cR + lrImgZ * sR;
            lrImgZ = -lrImgX * sR + lrImgZ * cR;
            lrImgX = tmpX;
        }
        const float imgHorizMag = dsp::fastSqrt(lrImgX * lrImgX + lrImgY * lrImgY);
        const float imgAzFactor = (imgHorizMag > 1e-7f) ? lrImgX / imgHorizMag : 0.0f;

        const float erItdTarget = itdScale * imgAzFactor;
        const float erItdSamples = er.itdSmooth.process(erItdTarget);

        const float absImgAz = std::abs(imgAzFactor);
        const float erIldTarget = 1.0f - (1.0f - ildGainBase) * absImgAz;
        const float erIldGain = er.ildSmooth.process(erIldTarget);

        er.shadowL.setCoefficients(
            params.headShadowFullOpenHz + erShadowRange * std::max(0.0f,  imgAzFactor), sr);
        er.shadowR.setCoefficients(
            params.headShadowFullOpenHz + erShadowRange * std::max(0.0f, -imgAzFactor), sr);

        er.itdDelayL.push(reflected);
        er.itdDelayR.push(reflected);

        constexpr float kERMinDelay = 2.0f;
        float erL = er.itdDelayL.read(kERMinDelay + std::max(0.0f,  erItdSamples));
        float erR = er.itdDelayR.read(kERMinDelay + std::max(0.0f, -erItdSamples));

        const float erIldAtten = 1.0f - erIldGain;
        const float erBlend = std::clamp(erItdSamples / kILDCrossfadeWidth, -1.0f, 1.0f);
        erL *= 1.0f - erIldAtten * std::max(0.0f,  erBlend);
        erR *= 1.0f - erIldAtten * std::max(0.0f, -erBlend);

        // Near-field LF boost — coefficients set per-block
        if (!params.bypassNearField) {
            erL = er.nearFieldLF_L.process(erL);
            erR = er.nearFieldLF_R.process(erR);
        }

        // Head shadow
        erL = er.shadowL.process(erL);
        erR = er.shadowR.process(erR);

        // Rear shadow — per-sample coefficient update (TPT safe)
        {
            const float rearFactor = computeRearFactor(lrImgX, lrImgY, lrImgZ, params.rearEarOffset);
            const float rearAmount = std::max(0.0f, rearFactor);
            const float rearCutTarget = kRearShadowFullOpenHz
                + (params.rearShadowMinHz - kRearShadowFullOpenHz) * rearAmount;
            const float smoothedRearCut = er.rearCutoffSmooth.process(rearCutTarget);
            er.rearSvfL.setCoefficients(smoothedRearCut, sr);
            er.rearSvfR.setCoefficients(smoothedRearCut, sr);
            if (!params.bypassRearShadow) {
                erL = er.rearSvfL.process(erL);
                erR = er.rearSvfR.process(erR);
            }
        }

        erDirectL += erL;
        erDirectR += erR;
        erRevL += erL;
        erRevR += erR;
    }

    return { erDirectL, erDirectR, erRevL, erRevR };
}

} // namespace xyzpan
