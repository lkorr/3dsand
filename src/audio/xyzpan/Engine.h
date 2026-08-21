#pragma once
#include "xyzpan/Types.h"
#include "xyzpan/Constants.h"
#include "xyzpan/DSPStateBridge.h"
#include "xyzpan/BinauralPipeline.h"
#include "xyzpan/DistancePipeline.h"
#include "xyzpan/ChestPipeline.h"
#include "xyzpan/FloorPipeline.h"
#include "xyzpan/ERPipeline.h"
#include "xyzpan/dsp/FractionalDelayLine.h"
#include "xyzpan/dsp/OnePoleSmooth.h"
#include "xyzpan/dsp/FDNReverb.h"
#include "xyzpan/dsp/OLAPitchShifter.h"
#include "xyzpan/dsp/LFO.h"
#include "xyzpan/SwarmMode.h"
#include <vector>
#include <random>

namespace xyzpan {

// XYZPanEngine — pure C++ audio processing engine with no JUCE dependency.
//
// Lifecycle:
//   1. prepare(sampleRate, maxBlockSize) — called before processing; pre-allocates buffers
//   2. setParams(params)                 — called once per processBlock with current parameter snapshot
//   3. process(...)                      — called each processBlock; fills outL and outR
//   4. reset()                           — called on transport restart; clears all state
//
// Audio contract:
//   - Accepts 1 or 2 input channels; stereo is summed to mono internally.
//   - Always produces 2-channel (stereo) output.
//   - No allocation inside process(); all buffers pre-allocated in prepare().
//
// Signal flow (per sample):
//   1. Stereo-to-mono sum (Phase 1)
//   2. Doppler delay (mono, before all spatial processing) [DIST-03, DIST-04]
//   3. Comb bank (series) with Y-driven dry/wet blend [DEPTH]
//   4. Mono EQ: presenceShelf (Y) → earCanalPeak (Y) → P1 → N1 → N2 → pinnaShelf (Z) [ELEV]
//   5. ITD/ILD binaural split (Phase 2) — with proximity-scaled ITD and head shadow
//   6. Chest bounce: parallel filtered+delayed copy of doppler'd input [ELEV-03]
//   7. Floor bounce: parallel delayed copy added to both ears [ELEV-04]
//   8. Distance processing: gain attenuation + air absorption LPF [DIST-01, DIST-02, DIST-05, DIST-06]
//   9. Early reflections: image source method, taps doppler'd input [ER]
//  10. FDN Reverb (VERB-01, VERB-02) — final stereo stage, with distance-scaled pre-delay
class XYZPanEngine {
public:
    XYZPanEngine() = default;
    ~XYZPanEngine() = default;

    // Non-copyable, non-movable (owns audio buffers).
    XYZPanEngine(const XYZPanEngine&) = delete;
    XYZPanEngine& operator=(const XYZPanEngine&) = delete;

    // Called before processing begins. Allocates monoBuffer to maxBlockSize.
    // initialParams supplies the current listener state so the engine starts
    // at the correct position/orientation rather than hardcoded defaults.
    void prepare(double sampleRate, int maxBlockSize, const EngineParams& initialParams = EngineParams{});

    // Set current parameters (snapshot from APVTS atomics, called once per block).
    void setParams(const EngineParams& params);

    // Snap listener rotation smoothers to the given angles (radians) with no
    // ramping.  Call once after state restoration so the engine starts at the
    // correct orientation instead of smoothing from identity.
    void snapListenerRotation(float yawRad, float pitchRad, float rollRad);

    // Snap the base-position smoothers to (x, y, z) with no ramping.  Call
    // when the engine input switches to a new sound at a new location (voice
    // adoption/retrigger): the 150 ms position smoothing otherwise drags the
    // new note's attack from wherever the voice last played -- an audible
    // through-the-world sweep.  Downstream ITD/ILD/filter smoothers (5-8 ms)
    // converge from the snapped position on their own.
    void snapPosition(float x, float y, float z);

    // Process audio.
    //   inputs            — array of input channel pointers (1 or 2 channels)
    //   numInputChannels  — number of valid input channel pointers
    //   outL, outR        — output channel pointers (pre-allocated by caller)
    //   numSamples        — number of samples to process (must not exceed maxBlockSize)
    void process(const float* const* inputs, int numInputChannels,
                 float* outL, float* outR,
                 float* auxL, float* auxR,  // nullptr when aux bus inactive
                 int numSamples);

    // Reset all internal state (delay lines, filter states, smoothers).
    // Call on transport restart or after silence gaps.
    void reset();

    // Phase 6: Last modulated position (base XYZ + LFO offset) from most recent process() call.
    // Written on audio thread at end of process(); read via PositionBridge by audio thread.
    struct ModulatedPosition { float x = 0.0f, y = 1.0f, z = 0.0f; };
    ModulatedPosition getLastModulatedPosition() const noexcept { return lastModulated_; }

    // Stereo node positions from most recent process() call
    struct StereoNodePositions { float lx, ly, lz, rx, ry, rz; float width; };
    StereoNodePositions getLastStereoNodes() const noexcept { return lastStereoNodes_; }

    // N-node swarm positions from most recent process() call
    struct SwarmNodePositions {
        struct Pos { float x, y, z; };
        Pos nodes[kMaxSwarmNodes];
        int count = 0;
    };
    SwarmNodePositions getLastSwarmPositions() const noexcept { return lastSwarmPositions_; }

    // Per-node pitch/delay values from most recent process() call (for UI grid)
    struct NodeDisplayValues {
        float pitchSemitones[kMaxSwarmNodes] = {};
        float delayMs[kMaxSwarmNodes] = {};
        int count = 0;
    };
    NodeDisplayValues getLastNodeDisplayValues() const noexcept { return lastNodeDisplayValues_; }

    // Live DSP state snapshot for dev panel display (UI-07).
    DSPStateSnapshot getLastDSPState() const noexcept { return lastDSPState_; }

    // LFO output snapshot — final tick()*depth values for UI waveform displays.
    struct LFOOutputs {
        float x = 0.f, y = 0.f, z = 0.f;
        float orbitXY = 0.f, orbitXZ = 0.f, orbitYZ = 0.f;
    };
    LFOOutputs getLastLFOOutputs() const noexcept;

private:
    EngineParams currentParams;
    std::vector<float> monoBuffer;  // pre-allocated in prepare() to maxBlockSize
    double sampleRate   = 44100.0;
    int    maxBlockSize = 512;

    // =========================================================================
    // N-node swarm pipeline array (replaces hardcoded L/R pairs)
    // =========================================================================
    struct NodePipeline {
        dsp::FractionalDelayLine delayOffset;  // per-node delay stagger (before doppler)
        dsp::OnePoleSmooth       delayOffsetSmooth;
        dsp::OLAPitchShifter     pitchShifter;
        BinauralPipeline binaural;
        DistancePipeline distance;
        ChestPipeline    chest;
        FloorPipeline    floor;
        ERPipeline       er;
    };

    NodePipeline nodes_[kMaxSwarmNodes];

    // Block-rate swarm offset interpolation state (avoids per-sample computeSwarmOffsets)
    NodeOffset swarmOffsetCur_[kMaxSwarmNodes] = {};
    NodeOffset swarmOffsetInc_[kMaxSwarmNodes] = {};

    // Tracking last smoothing time constants to detect changes from dev panel.
    float lastSmoothMs_ITD_    = kDefaultSmoothMs_ITD;
    float lastSmoothMs_Filter_ = kDefaultSmoothMs_Filter;
    float lastSmoothMs_Gain_   = kDefaultSmoothMs_Gain;

    float lastDistSmoothMs_ = kDistSmoothMs;   // track dev panel changes to re-prepare smoother

    // =========================================================================
    // Phase 5: Reverb (VERB-01 through VERB-04)
    // =========================================================================
    dsp::FDNReverb   reverb_;
    dsp::OnePoleSmooth verbWetSmooth_;   // smooth wet/dry transitions

    // =========================================================================
    // Aux reverb send (post-air-absorption, pre-FDN reverb)
    // =========================================================================
    dsp::FractionalDelayLine auxPreDelayL_;
    dsp::FractionalDelayLine auxPreDelayR_;
    dsp::OnePoleSmooth       auxGainSmooth_;
    dsp::OnePoleSmooth       auxDelaySmooth_;   // smooth aux pre-delay transitions

    // =========================================================================
    // Phase 5: LFO (LFO-01 through LFO-05)
    // =========================================================================
    dsp::LFO lfoX_, lfoY_, lfoZ_;
    dsp::OnePoleSmooth lfoDepthXSmooth_, lfoDepthYSmooth_, lfoDepthZSmooth_;
    dsp::OnePoleSmooth lfoDepthMulSmooth_;

    // Block-position smoothers — 150ms smooth for block-rate EQ targets.
    dsp::OnePoleSmooth blkPosXSmooth_, blkPosYSmooth_, blkPosZSmooth_;

    // Per-sample position interpolation — previous block's smoothed base position.
    // When LFOs are inactive, linearly interpolate from prev→current across the
    // block so per-sample binaural/distance/doppler targets change continuously
    // instead of stepping at block boundaries.
    float prevSmoothBaseX_ = 0.0f, prevSmoothBaseY_ = 1.0f, prevSmoothBaseZ_ = 0.0f;
    bool  firstSetParams_ = true;

    // Dev tool: test tone oscillator state — persistent across blocks
    float        sawPhase_ = 0.0f;  // [0, 1)
    float        clickSamplesLeft_ = 0.0f;
    bool         prevPulseGate_ = false;
    dsp::LFO     pulseLFO_;
    std::mt19937 noiseRng_;

    // Stereo orbit LFOs (3 planes) + depth smoothers
    dsp::LFO orbitLfoXY_, orbitLfoXZ_, orbitLfoYZ_;
    dsp::OnePoleSmooth orbitDepthXYSmooth_, orbitDepthXZSmooth_, orbitDepthYZSmooth_;
    dsp::OnePoleSmooth orbitDepthMulSmooth_;

    // =========================================================================
    // Early Reflections (shared smoothers)
    // =========================================================================
    dsp::OnePoleSmooth erLevelSmooth_;
    dsp::OnePoleSmooth erReverbSendSmooth_;
    bool erWasActive_ = false;                   // gate-open tracker for click-free transitions

    // Test tone gain smoother — prevents click when test tone is enabled
    // on fresh prepare/reset (first block goes from silence to full gain).
    dsp::OnePoleSmooth testToneGainSmooth_;

    // Last LFO output values (tick()*depth) — captured per block for UI display
    float lastLfoOutX_ = 0.f, lastLfoOutY_ = 0.f, lastLfoOutZ_ = 0.f;
    float lastLfoOutOrbitXY_ = 0.f, lastLfoOutOrbitXZ_ = 0.f, lastLfoOutOrbitYZ_ = 0.f;

    // Width transition smoother (avoids pops when width changes)
    dsp::OnePoleSmooth stereoWidthSmooth_;

    // Node count gain smoother (avoids clicks when swarmNodeCount changes)
    dsp::OnePoleSmooth nodeCountGainSmooth_;

    // Circular (angular) smoothers for phase and offset — prevents clicks at wrap-around.
    // These smooth in the angular domain (radians) using sin/cos decomposition to handle
    // the 0↔1 boundary correctly: the smoother always takes the shortest path.
    float phaseSmCos_ = 1.0f, phaseSmSin_ = 0.0f;   // unit-circle state for phase
    float offsetSmCos_ = 1.0f, offsetSmSin_ = 0.0f;  // unit-circle state for offset
    float angularSmA_ = 0.0f;     // per-sample IIR coefficient (used for phase/offset)
    float listenerBlkSmA_ = 0.0f; // per-block IIR coefficient (used after movement stops)
    float listenerMovSmA_ = 0.0f; // per-block IIR coefficient (used during movement)

    // Circular (angular) smoothers for listener yaw/pitch — prevents clicks at 360°↔0° wrap.
    float yawSmCos_ = 1.0f, yawSmSin_ = 0.0f;
    float pitchSmCos_ = 1.0f, pitchSmSin_ = 0.0f;
    float rollSmCos_ = 1.0f, rollSmSin_ = 0.0f;

    // Previous block's smoothed cos/sin for per-sample trig interpolation.
    // Eliminates block-boundary discontinuities during fast head rotation.
    float prevCosY_ = 1.f, prevSinY_ = 0.f;
    float prevCosP_ = 1.f, prevSinP_ = 0.f;
    float prevCosR_ = 1.f, prevSinR_ = 0.f;

    // Cached listener orientation for change-detection gating (skip smoother when static)
    float prevListenerYaw_   = 0.0f;
    float prevListenerPitch_ = 0.0f;
    float prevListenerRoll_  = 0.0f;
    float cachedCosY_ = 1.0f, cachedSinY_ = 0.0f;
    float cachedCosP_ = 1.0f, cachedSinP_ = 0.0f;
    float cachedCosR_ = 1.0f, cachedSinR_ = 0.0f;
    bool  listenerSettled_ = true;  // true when cos/sin have snapped to exact target

    // Last L/R node positions for position bridge
    StereoNodePositions lastStereoNodes_{};

    // N-node swarm positions for position bridge
    SwarmNodePositions lastSwarmPositions_{};

    // Per-node pitch/delay for UI display grid
    NodeDisplayValues lastNodeDisplayValues_{};

    // Phase 6: Last-sample modulated position from most recent process() call (UI-07).
    // Audio thread writes after process(); PositionBridge propagates to GL thread.
    ModulatedPosition lastModulated_;

    // Live DSP state for dev panel display bridge (UI-07).
    DSPStateSnapshot lastDSPState_;

    // =========================================================================
    // Per-block pre-computed cache (optimization: avoid per-sample transcendentals)
    // =========================================================================
    // Cached linear ILD gain base: pow(10, -ildMaxDb/20). Updated once per block.
    // Used by BinauralPipeline::processSample() to compute per-node ILD target
    // without re-calling std::pow on every sample.
    float ildGainBase_ = 1.0f;

    // Binaural toggle — click-free blend between binaural (1) and hardpan (0)
    dsp::OnePoleSmooth binauralBlendSmooth_;
    float hardpanGainBase_ = 1.0f;  // pow(10, kHardpanMaxDb/20), recomputed per block

    // =========================================================================
    // Per-node helpers (extracted from duplicated stereo L/R processing)
    // =========================================================================

    // Per-block: set smoothed EQ coefficients for a single node pipeline.
    // Computes binaural EQ (presence, ear canal, pinna, expanded pinna P5,
    // near-field LF), distance air absorption, and ER directional cues from
    // the node's head-rotated block-start position.
    void setNodeBlockCoefficients(
        int nodeIdx,
        float nodeX, float nodeY, float nodeZ,  // head-rotated block-start position
        float sr, int numSamples);

    // Per-sample result from processing one node through the DSP chain.
    struct NodeSampleResult {
        float left;       // binaural left ear (post-distance, post-floor)
        float right;      // binaural right ear (post-distance, post-floor)
        float distFrac;   // distance fraction (for reverb pre-delay)
        float distGain;   // effective distance gain (smoothed)
    };

    // Per-sample: process one node through chest → binaural → distance → floor.
    // Input should already be doppler-processed. Doppler and ER are handled externally.
    NodeSampleResult processNodeSample(
        int nodeIdx,
        float dopplerInput,                      // mono input (already doppler-processed)
        float relX, float relY, float relZ,      // listener-relative position (pre-rotation)
        float dspX, float dspY, float dspZ,      // head-rotated position
        float sr, float binBlend,
        float chestGainLin, float floorGainLin,
        const EngineParams& params);
};

} // namespace xyzpan
