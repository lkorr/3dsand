#pragma once
#include "xyzpan/Constants.h"
#include "xyzpan/dsp/BiquadFilter.h"
#include "xyzpan/dsp/FractionalDelayLine.h"
#include "xyzpan/dsp/OnePoleLP.h"
#include "xyzpan/dsp/OnePoleSmooth.h"
#include "xyzpan/dsp/SVFLowPass.h"
#include <array>

namespace xyzpan {

// Per-reflection DSP state for the early reflections image source method.
// 6 instances (one per wall face of the virtual cube: ±X, ±Y, ±Z).
struct EarlyReflection {
    dsp::OnePoleLP wallAbsorption;          // Wall absorption LPF
    dsp::OnePoleSmooth delaySmooth;         // Smooth delay transitions
    dsp::OnePoleSmooth gainSmooth;          // Smooth gain transitions
    // Binaural: ITD + ILD + head shadow + rear shadow per ear
    dsp::FractionalDelayLine itdDelayL, itdDelayR;
    dsp::SVFLowPass shadowL, shadowR;
    dsp::OnePoleSmooth itdSmooth;
    dsp::OnePoleSmooth ildSmooth;

    // Pinna EQ chain (9 bands — excludes fixed P1 peak)
    dsp::BiquadFilter presenceShelf;        // F/B: 3kHz high shelf
    dsp::BiquadFilter earCanalPeak;         // F/B: 2.7kHz peak
    dsp::BiquadFilter shoulderPeak;         // T/B: 1.5kHz (expanded pinna)
    dsp::BiquadFilter conchaNotch;          // T/B: 4kHz (expanded pinna)
    dsp::BiquadFilter pinnaNotch;           // T/B: N1 6.5-10kHz
    dsp::BiquadFilter pinnaNotch2;          // T/B: N2 = N1+3kHz
    dsp::BiquadFilter upperPinna;           // T/B: 12kHz (expanded pinna)
    dsp::BiquadFilter tragusNotch;          // F/B+T/B: 8.5kHz (expanded pinna)
    dsp::BiquadFilter pinnaShelf;           // T/B: 4kHz high shelf

    // Rear shadow SVFs (both ears)
    dsp::SVFLowPass rearSvfL, rearSvfR;
    dsp::OnePoleSmooth rearCutoffSmooth;

    // Near-field LF boost (per ear)
    dsp::BiquadFilter nearFieldLF_L, nearFieldLF_R;
};

// Per-node early reflections DSP state. When stereo width > 0, each node
// gets its own set of 6 reflections with independent image source positions.
struct EngineParams;

struct ERPipeline {
    std::array<EarlyReflection, kNumER> reflections;
    dsp::FractionalDelayLine sharedDelay;
    void prepare(float sr);
    void reset();

    // Per-block: update wall absorption coefficients (smoothed across block).
    void updateWallAbsorption(float dampCutoff, float sr, int blockSize);

    // Per-block: compute image source directions and set pinna EQ + near-field
    // coefficients for each tap based on elevation, rear, and azimuth factors.
    void updateTapDirectionalCoeffs(
        float nodeX, float nodeY, float nodeZ,
        float listenerX, float listenerY, float listenerZ,
        float roomHalf, float sr, int blockSize,
        bool rotated, float cY, float sY, float cP, float sP, float cR, float sR,
        float sphereRadius,
        const EngineParams& params);

    // Process early reflections for a single source node.
    struct ERResult { float directL, directR, reverbL, reverbR; };
    ERResult processSample(float input, float nodeX, float nodeY, float nodeZ,
                           float listenerX, float listenerY, float listenerZ,
                           float distGainTarget, float sr,
                           float roomHalf,
                           float ildGainBase, bool rotated,
                           float cosY, float sinY, float cosP, float sinP,
                           float cosR, float sinR,
                           const EngineParams& params);
};

} // namespace xyzpan
