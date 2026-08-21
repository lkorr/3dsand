#include "xyzpan/Engine.h"
#include "xyzpan/Coordinates.h"
#include "xyzpan/Constants.h"
#include "xyzpan/SwarmMode.h"
#include "xyzpan/dsp/SineLUT.h"
#include "xyzpan/dsp/FastMath.h"
#include <algorithm>
#include <cstring>
#include <cmath>

namespace xyzpan {

// Build an ordering permutation for per-node value distribution (pitch, delay):
// perm[i] = which ascending-sorted slot goes to node i.
// order: 0=Linear, 1=Random (seeded LCG shuffle), 2=Interleave, 3=Reverse,
//        4=Mirror, 5=Pairs, 6=Converge
// var: Interleave = number of divisions (2-8); Pairs = group size (2-8)
static void buildOrderPermutation(int order, int N, unsigned int seedBase, int var, int* perm)
{
    for (int i = 0; i < N; ++i) perm[i] = i;  // Linear (default)
    const int v = var < 2 ? 2 : (var > 8 ? 8 : var);

    switch (order) {
        case 0: // Linear — already set
            break;
        case 1: // Random — deterministic shuffle from seedBase
        {
            unsigned int seed = seedBase;
            for (int i = N - 1; i > 0; --i) {
                seed = seed * 1664525u + 1013904223u;
                int j = static_cast<int>(seed % static_cast<unsigned int>(i + 1));
                int tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
            }
            break;
        }
        case 2: // Interleave — deal slots into v round-robin groups
                // (v=2: 0,2,4,...,1,3,5,...  v=4: 0,4,8,...,1,5,9,..., etc.)
        {
            int idx = 0;
            for (int g = 0; g < v; ++g)
                for (int i = g; i < N; i += v) perm[idx++] = i;
            break;
        }
        case 3: // Reverse — highest value on node 0
            for (int i = 0; i < N; ++i) perm[i] = N - 1 - i;
            break;
        case 4: // Mirror — values radiate outward from center
        {
            // Assign: node 0 = slot 0, node N-1 = slot 1, node 1 = slot 2, ...
            int lo = 0, hi = N - 1, slot = 0;
            while (lo <= hi) {
                perm[lo++] = slot++;
                if (lo <= hi) perm[hi--] = slot++;
            }
            break;
        }
        case 5: // Pairs — groups of v adjacent nodes share the same value
        {
            // Map node groups to evenly-spaced sorted slots
            const int numSlots = (N + v - 1) / v;
            for (int i = 0; i < N; ++i) {
                const int grp = i / v;
                perm[i] = (numSlots > 1) ? grp * (N - 1) / (numSlots - 1) : 0;
                if (perm[i] >= N) perm[i] = N - 1;
            }
            break;
        }
        case 6: // Converge — extremes at edges, mid at center
        {
            // Reverse of Mirror: center nodes get extreme values, edges get mid
            int lo = 0, hi = N - 1, slot = N - 1;
            while (lo <= hi) {
                perm[lo++] = slot--;
                if (lo <= hi) perm[hi--] = slot--;
            }
            break;
        }
    }
}

// Distance-difference azimuth: virtual ears at (±h, 0, 0) in listener-relative space.
// Returns signed factor: +1 = right, -1 = left, 0 = median plane.
static inline float computeAzimuthFactor(float x, float y, float z, float h) {
    if (h < 1e-7f) return 0.0f;
    const float yz2 = y * y + z * z;
    const float distLeft  = dsp::fastSqrt((x + h) * (x + h) + yz2);
    const float distRight = dsp::fastSqrt((x - h) * (x - h) + yz2);
    const float delta = distLeft - distRight;  // positive when source is right of center
    return std::clamp(delta / (2.0f * h), -1.0f, 1.0f);
}

// Distance-difference rear factor: virtual ears at (0, ±h, 0) in listener-relative space.
// Returns signed factor: +1 = rear, -1 = front, 0 = interaural plane.
static inline float computeRearFactor(float x, float y, float z, float h) {
    if (h < 1e-7f) return 0.0f;
    const float xz2 = x * x + z * z;
    const float distFront = dsp::fastSqrt(xz2 + (y - h) * (y - h));
    const float distBack  = dsp::fastSqrt(xz2 + (y + h) * (y + h));
    const float delta = distFront - distBack;  // positive when source is behind
    return std::clamp(delta / (2.0f * h), -1.0f, 1.0f);
}

// Elevation factor from distance-difference between top/bottom virtual ear nodes.
// Returns 0.0 (nadir) to 1.0 (zenith), matching the old atan2-based range.
static inline float computeElevFactor(float x, float y, float z, float earOffset) {
    const float h = earOffset;
    if (h < 1e-7f) return 0.5f;
    const float xy2 = x * x + y * y;
    const float distTop    = dsp::fastSqrt(xy2 + (z - h) * (z - h));
    const float distBottom = dsp::fastSqrt(xy2 + (z + h) * (z + h));
    const float delta = distBottom - distTop;
    const float maxDelta = 2.0f * h;
    return std::clamp(delta / maxDelta * 0.5f + 0.5f, 0.0f, 1.0f);
}

// ============================================================================
// prepare()
// ============================================================================

void XYZPanEngine::prepare(double inSampleRate, int inMaxBlockSize, const EngineParams& initialParams) {
    sampleRate   = inSampleRate;
    maxBlockSize = inMaxBlockSize;

    // Pre-allocate mono mixing buffer.
    monoBuffer.resize(static_cast<size_t>(inMaxBlockSize), 0.0f);

    const float sr = static_cast<float>(inSampleRate);

    // Prepare per-node delay offset lines (kSwarmDelayOffsetCapMs max; the
    // runtime param is clamped to the same constant in process())
    {
        const int delayOffsetCap = static_cast<int>(kSwarmDelayOffsetCapMs * 0.001f * sr) + 8;
        for (int n = 0; n < kMaxSwarmNodes; ++n) {
            nodes_[n].delayOffset.prepare(delayOffsetCap);
            nodes_[n].delayOffset.reset();
            nodes_[n].delayOffsetSmooth.prepare(kDistSmoothMs, sr);
            nodes_[n].delayOffsetSmooth.reset(0.0f);
            nodes_[n].pitchShifter.prepare(sr);
        }
    }

    int delayCap = static_cast<int>(kMaxITDUpperBound_ms * 0.001f * sr) + 8;

    // Prepare all N node pipelines (binaural, chest, floor, distance, ER)
    for (int n = 0; n < kMaxSwarmNodes; ++n) {
        nodes_[n].binaural.prepare(sr, delayCap, kCombMaxDelay_ms);
        nodes_[n].chest.prepare(sr);
        nodes_[n].floor.prepare(sr);
        nodes_[n].distance.prepare(sr);
        nodes_[n].er.prepare(sr);
    }

    // Sync tracking members.
    lastSmoothMs_ITD_    = kDefaultSmoothMs_ITD;
    lastSmoothMs_Filter_ = kDefaultSmoothMs_Filter;
    lastSmoothMs_Gain_   = kDefaultSmoothMs_Gain;
    lastDistSmoothMs_ = kDistSmoothMs;

    // -------------------------------------------------------------------------
    // Phase 5: Reverb
    // -------------------------------------------------------------------------
    reverb_.prepare(inSampleRate, inMaxBlockSize);
    reverb_.setSize(kVerbDefaultSize);
    reverb_.setDecay(kVerbDefaultDecay);
    reverb_.setDamping(kVerbDefaultDamping);
    reverb_.setDiffusion(kVerbDefaultDiffusion);
    reverb_.setModDepth(kVerbDefaultModDepth);
    reverb_.setWetDry(1.0f);
    reverb_.reset();
    verbWetSmooth_.prepare(kDefaultSmoothMs_Gain, sr);
    verbWetSmooth_.reset(kVerbDefaultWet);

    // Aux reverb send pre-delay lines
    {
        int auxCap = static_cast<int>(kVerbPreDelayMaxMs * 0.001f * sr) + 8;
        auxPreDelayL_.prepare(auxCap);
        auxPreDelayR_.prepare(auxCap);
        auxPreDelayL_.reset();
        auxPreDelayR_.reset();
    }
    auxGainSmooth_.prepare(kDefaultSmoothMs_Gain, sr);
    auxGainSmooth_.reset(1.0f);
    auxDelaySmooth_.prepare(kDefaultSmoothMs_Gain, sr);
    auxDelaySmooth_.reset(0.0f);
    // Phase 5: LFO
    lfoX_.prepare(inSampleRate);
    lfoY_.prepare(inSampleRate);
    lfoZ_.prepare(inSampleRate);
    constexpr float kLfoDepthSmoothMs = 20.0f;  // longer than gain (5ms) to prevent clicks on knob drag
    lfoDepthXSmooth_.prepare(kLfoDepthSmoothMs, sr);
    lfoDepthYSmooth_.prepare(kLfoDepthSmoothMs, sr);
    lfoDepthZSmooth_.prepare(kLfoDepthSmoothMs, sr);
    lfoDepthXSmooth_.reset(0.0f);
    lfoDepthYSmooth_.reset(0.0f);
    lfoDepthZSmooth_.reset(0.0f);
    lfoDepthMulSmooth_.prepare(kLfoDepthSmoothMs, sr);
    lfoDepthMulSmooth_.reset(1.0f);
    blkPosXSmooth_.prepare(150.0f, sr);
    blkPosYSmooth_.prepare(150.0f, sr);
    blkPosZSmooth_.prepare(150.0f, sr);
    blkPosXSmooth_.reset(initialParams.x);
    blkPosYSmooth_.reset(initialParams.y);
    blkPosZSmooth_.reset(initialParams.z);
    prevSmoothBaseX_ = initialParams.x;
    prevSmoothBaseY_ = initialParams.y;
    prevSmoothBaseZ_ = initialParams.z;
    firstSetParams_ = true;

    // -------------------------------------------------------------------------
    // Stereo orbit LFOs
    // -------------------------------------------------------------------------
    orbitLfoXY_.prepare(inSampleRate);
    orbitLfoXZ_.prepare(inSampleRate);
    orbitLfoYZ_.prepare(inSampleRate);
    orbitDepthXYSmooth_.prepare(kDefaultSmoothMs_Gain, sr);
    orbitDepthXZSmooth_.prepare(kDefaultSmoothMs_Gain, sr);
    orbitDepthYZSmooth_.prepare(kDefaultSmoothMs_Gain, sr);
    orbitDepthXYSmooth_.reset(0.0f);
    orbitDepthXZSmooth_.reset(0.0f);
    orbitDepthYZSmooth_.reset(0.0f);
    orbitDepthMulSmooth_.prepare(kDefaultSmoothMs_Gain, sr);
    orbitDepthMulSmooth_.reset(1.0f);
    stereoWidthSmooth_.prepare(kDefaultSmoothMs_Gain, sr);
    stereoWidthSmooth_.reset(0.0f);
    nodeCountGainSmooth_.prepare(30.0f, sr);  // 30ms crossfade for node count changes
    nodeCountGainSmooth_.reset(1.0f);         // will converge to 1/sqrt(N) in processBlock

    // Angular smoothers for circular phase/offset knobs (5ms time constant)
    angularSmA_ = std::exp(-6.28318530f / (kDefaultSmoothMs_Gain * 0.001f * sr));
    phaseSmCos_ = 1.0f; phaseSmSin_ = 0.0f;   // angle = 0
    offsetSmCos_ = 1.0f; offsetSmSin_ = 0.0f;  // angle = 0

    // Block-rate IIR coefficients for listener rotation smoothing.
    {
        const float blockPeriod = static_cast<float>(inMaxBlockSize) / sr;
        // During movement: 5ms time constant (tracks rapidly but still click-free).
        constexpr float kTrackMs = 5.0f;
        listenerMovSmA_ = std::exp(-blockPeriod / (kTrackMs * 0.001f));
        // After movement stops: 20ms time constant → converges within ~100ms.
        constexpr float kConvergeMs = 20.0f;
        listenerBlkSmA_ = std::exp(-blockPeriod / (kConvergeMs * 0.001f));
    }

    // Initialize listener rotation from supplied params (not identity)
    // so the engine starts at the correct orientation immediately.
    currentParams = initialParams;
    snapListenerRotation(initialParams.listenerYaw,
                         initialParams.listenerPitch,
                         initialParams.listenerRoll);

    // -------------------------------------------------------------------------
    // Early Reflections (shared smoothers)
    // -------------------------------------------------------------------------
    {
        erLevelSmooth_.prepare(kDefaultSmoothMs_Gain, sr);
        erLevelSmooth_.reset(0.0f);
        erReverbSendSmooth_.prepare(kDefaultSmoothMs_Gain, sr);
        erReverbSendSmooth_.reset(kERReverbSendDefault);
    }

    // Binaural toggle smoother — 5ms for click-free transition
    binauralBlendSmooth_.prepare(5.0f, sr);
    binauralBlendSmooth_.reset(1.0f);

    // Dev tool: test tone pulse LFO + noise RNG
    pulseLFO_.prepare(inSampleRate);
    pulseLFO_.waveform = dsp::LFOWaveform::Square;
    pulseLFO_.reset(0.0f);
    noiseRng_.seed(12345u);

    // Test tone gain — start muted so first block ramps up smoothly if enabled
    testToneGainSmooth_.prepare(kDefaultSmoothMs_Gain, sr);
    testToneGainSmooth_.reset(0.0f);
}

// ============================================================================
// setParams()
// ============================================================================

void XYZPanEngine::setParams(const EngineParams& params) {
    // Snap position smoothers on first call so they don't ramp from prepare()'s
    // default position to the actual starting position.
    if (firstSetParams_) {
        firstSetParams_ = false;
        blkPosXSmooth_.reset(params.x);
        blkPosYSmooth_.reset(params.y);
        blkPosZSmooth_.reset(params.z);
        prevSmoothBaseX_ = params.x;
        prevSmoothBaseY_ = params.y;
        prevSmoothBaseZ_ = params.z;
    }
    currentParams = params;
}

void XYZPanEngine::snapPosition(float x, float y, float z) {
    blkPosXSmooth_.reset(x);
    blkPosYSmooth_.reset(y);
    blkPosZSmooth_.reset(z);
    prevSmoothBaseX_ = x;
    prevSmoothBaseY_ = y;
    prevSmoothBaseZ_ = z;
}

void XYZPanEngine::snapListenerRotation(float yawRad, float pitchRad, float rollRad) {
    yawSmCos_   = dsp::SineLUT::cosLookupAngle(yawRad);
    yawSmSin_   = dsp::SineLUT::lookupAngle(yawRad);
    pitchSmCos_ = dsp::SineLUT::cosLookupAngle(pitchRad);
    pitchSmSin_ = dsp::SineLUT::lookupAngle(pitchRad);
    rollSmCos_  = dsp::SineLUT::cosLookupAngle(rollRad);
    rollSmSin_  = dsp::SineLUT::lookupAngle(rollRad);
    cachedCosY_ = yawSmCos_;  cachedSinY_ = yawSmSin_;
    cachedCosP_ = pitchSmCos_; cachedSinP_ = pitchSmSin_;
    cachedCosR_ = rollSmCos_;  cachedSinR_ = rollSmSin_;
    prevCosY_ = yawSmCos_;  prevSinY_ = yawSmSin_;
    prevCosP_ = pitchSmCos_; prevSinP_ = pitchSmSin_;
    prevCosR_ = rollSmCos_;  prevSinR_ = rollSmSin_;
    prevListenerYaw_   = yawRad;
    prevListenerPitch_ = pitchRad;
    prevListenerRoll_  = rollRad;
    listenerSettled_ = true;
}





// ============================================================================
// process()
// ============================================================================

void XYZPanEngine::process(const float* const* inputs, int numInputChannels,
                            float* outL, float* outR,
                            float* auxL, float* auxR,
                            int numSamples) {
    if (inputs == nullptr || inputs[0] == nullptr || outL == nullptr || outR == nullptr)
        return;

    if (numSamples > maxBlockSize || numSamples <= 0) {
        std::memset(outL, 0, sizeof(float) * static_cast<size_t>(numSamples > 0 ? numSamples : 0));
        std::memset(outR, 0, sizeof(float) * static_cast<size_t>(numSamples > 0 ? numSamples : 0));
        if (auxL) std::memset(auxL, 0, sizeof(float) * static_cast<size_t>(numSamples > 0 ? numSamples : 0));
        if (auxR) std::memset(auxR, 0, sizeof(float) * static_cast<size_t>(numSamples > 0 ? numSamples : 0));
        return;
    }

    // -------------------------------------------------------------------------
    // Input preparation — keep separate L/R pointers for stereo mode
    // -------------------------------------------------------------------------
    const float* inputL = inputs[0];
    const float* inputR = (numInputChannels >= 2 && inputs[1] != nullptr) ? inputs[1] : nullptr;

    // When mono (no second input), monoIn = inputL. When stereo, still prepare
    // mono buffer for mono fallback path (width=0).
    if (inputR != nullptr) {
        for (int i = 0; i < numSamples; ++i)
            monoBuffer[static_cast<size_t>(i)] = 0.5f * (inputL[i] + inputR[i]);
    }

    // -------------------------------------------------------------------------
    // Per-block preamble: re-prepare smoothers if time constants changed.
    // -------------------------------------------------------------------------
    const float sr = static_cast<float>(sampleRate);

    if (currentParams.smoothMs_ITD != lastSmoothMs_ITD_) {
        for (int n = 0; n < kMaxSwarmNodes; ++n)
            nodes_[n].binaural.itdSmooth.prepare(currentParams.smoothMs_ITD, sr);
        lastSmoothMs_ITD_ = currentParams.smoothMs_ITD;
    }
    if (currentParams.smoothMs_Filter != lastSmoothMs_Filter_) {
        for (int n = 0; n < kMaxSwarmNodes; ++n) {
            nodes_[n].binaural.shadowCutoffSmooth.prepare(currentParams.smoothMs_Filter, sr);
            nodes_[n].binaural.rearCutoffSmooth.prepare(currentParams.smoothMs_Filter, sr);
        }
        lastSmoothMs_Filter_ = currentParams.smoothMs_Filter;
    }
    if (currentParams.smoothMs_Gain != lastSmoothMs_Gain_) {
        for (int n = 0; n < kMaxSwarmNodes; ++n)
            nodes_[n].binaural.ildGainSmooth.prepare(currentParams.smoothMs_Gain, sr);
        lastSmoothMs_Gain_ = currentParams.smoothMs_Gain;
    }

    // -------------------------------------------------------------------------
    // Phase 3: per-block updates (position-independent only)
    // -------------------------------------------------------------------------
    for (int n = 0; n < kMaxSwarmNodes; ++n) {
        for (int c = 0; c < kMaxCombFilters; ++c) {
            nodes_[n].binaural.combBank[c].setDelay(static_cast<int>(currentParams.combDelays_ms[c] * 0.001f * sr));
            nodes_[n].binaural.combBank[c].setFeedback(currentParams.combFeedback[c]);
        }
        // Node 0 uses direct set (block-start); others use smoothed to avoid clicks
        if (n == 0) {
            nodes_[n].binaural.pinnaP1.setCoefficients(dsp::BiquadType::PeakingEQ,
                currentParams.pinnaP1FreqHz, sr, currentParams.pinnaP1Q, currentParams.pinnaP1GainDb);
        } else {
            nodes_[n].binaural.pinnaP1.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
                currentParams.pinnaP1FreqHz, sr, currentParams.pinnaP1Q, currentParams.pinnaP1GainDb, numSamples);
        }
    }

    // -------------------------------------------------------------------------
    // Phase 4: per-block distance processing
    // -------------------------------------------------------------------------
    if (currentParams.distSmoothMs != lastDistSmoothMs_) {
        for (int n = 0; n < kMaxSwarmNodes; ++n)
            nodes_[n].distance.distDelaySmooth.prepare(currentParams.distSmoothMs, sr);
        lastDistSmoothMs_ = currentParams.distSmoothMs;
    }

    const bool dopplerOn = currentParams.dopplerEnabled;

    // -------------------------------------------------------------------------
    // Phase 5: per-block reverb parameter updates
    // -------------------------------------------------------------------------
    reverb_.setDecay(currentParams.verbDecay);
    reverb_.setDamping(currentParams.verbDamping);
    reverb_.setDiffusion(currentParams.verbDiffusion);
    reverb_.setModDepth(currentParams.verbModDepth);

    // -------------------------------------------------------------------------
    // Phase 5: LFO — set rate and waveform per block
    // -------------------------------------------------------------------------
    // beatDiv is in BARS. One bar = hostTimeSigNum beats (quarter-note beats when
    // denominator is 4). Seconds per bar = hostTimeSigNum * (60 / BPM) * (4 / den).
    // LFO freq = 1 / (beatDiv * secondsPerBar).
    auto lfoRate = [&](float freeHz, float beatDiv, bool tempoSync) -> float {
        if (tempoSync && currentParams.hostBpm > 0.0f && beatDiv > 0.0f) {
            const int num = currentParams.hostTimeSigNum > 0 ? currentParams.hostTimeSigNum : 4;
            const int den = currentParams.hostTimeSigDen > 0 ? currentParams.hostTimeSigDen : 4;
            // secondsPerBar = num * (4.0 / den) * (60.0 / bpm)
            const float secondsPerBar = static_cast<float>(num) * (4.0f / static_cast<float>(den))
                                      * (60.0f / currentParams.hostBpm);
            return 1.0f / (beatDiv * secondsPerBar);
        }
        return freeHz;
    };

    // Compute LFO phase from host ppqPosition for grid-locked sync.
    // Sets the accumulator absolutely at block start; intra-block free-runs.
    // When transport is playing: lock LFO phase to the grid via ppqPosition.
    // When stopped: let LFOs free-run at synced rate so the user can preview movement.
    auto syncLfoPhase = [&](dsp::LFO& lfo, float beatDiv, float speedMul,
                            float userPhaseOffset, bool tempoSync) {
        if (!tempoSync || !currentParams.hostIsPlaying
            || currentParams.hostPpqPosition < 0.0) return;
        const int num = currentParams.hostTimeSigNum > 0 ? currentParams.hostTimeSigNum : 4;
        const int den = currentParams.hostTimeSigDen > 0 ? currentParams.hostTimeSigDen : 4;
        const double ppqPerBar = static_cast<double>(num) * (4.0 / static_cast<double>(den));
        const double barsPos = currentParams.hostPpqPosition / ppqPerBar;
        const float qSpeed = quantizeSyncSpeed(speedMul);
        if (qSpeed <= 0.0f) return;
        const double effDiv = static_cast<double>(beatDiv) / static_cast<double>(qSpeed);
        if (effDiv <= 0.0) return;
        const double cycles = barsPos / effDiv;
        const float phase = static_cast<float>(cycles - std::floor(cycles))
                          + userPhaseOffset;
        const int64_t cycleNum = static_cast<int64_t>(std::floor(cycles));
        lfo.setPhaseFromPosition(phase, cycleNum);
    };

    lfoX_.waveform = static_cast<dsp::LFOWaveform>(currentParams.lfoXWaveform);
    lfoY_.waveform = static_cast<dsp::LFOWaveform>(currentParams.lfoYWaveform);
    lfoZ_.waveform = static_cast<dsp::LFOWaveform>(currentParams.lfoZWaveform);
    lfoX_.setRateHz(lfoRate(currentParams.lfoXRate, currentParams.lfoXBeatDiv, currentParams.lfoXTempoSync) * (currentParams.lfoXTempoSync ? quantizeSyncSpeed(currentParams.lfoSpeedMul) : currentParams.lfoSpeedMul));
    lfoY_.setRateHz(lfoRate(currentParams.lfoYRate, currentParams.lfoYBeatDiv, currentParams.lfoYTempoSync) * (currentParams.lfoYTempoSync ? quantizeSyncSpeed(currentParams.lfoSpeedMul) : currentParams.lfoSpeedMul));
    lfoZ_.setRateHz(lfoRate(currentParams.lfoZRate, currentParams.lfoZBeatDiv, currentParams.lfoZTempoSync) * (currentParams.lfoZTempoSync ? quantizeSyncSpeed(currentParams.lfoSpeedMul) : currentParams.lfoSpeedMul));
    const bool ppqPlaying = currentParams.hostIsPlaying && currentParams.hostPpqPosition >= 0.0;
    const bool xGridSync = currentParams.lfoXTempoSync && ppqPlaying;
    const bool yGridSync = currentParams.lfoYTempoSync && ppqPlaying;
    const bool zGridSync = currentParams.lfoZTempoSync && ppqPlaying;
    if (!xGridSync) lfoX_.setPhaseOffset(currentParams.lfoXPhase);
    if (!yGridSync) lfoY_.setPhaseOffset(currentParams.lfoYPhase);
    if (!zGridSync) lfoZ_.setPhaseOffset(currentParams.lfoZPhase);
    lfoX_.setSmoothMs(currentParams.lfoXSmooth * 300.0f);  // 0-1 → 0-300ms
    lfoY_.setSmoothMs(currentParams.lfoYSmooth * 300.0f);
    lfoZ_.setSmoothMs(currentParams.lfoZSmooth * 300.0f);
    if (currentParams.lfoXResetPhase && !xGridSync) lfoX_.requestReset();
    if (currentParams.lfoYResetPhase && !yGridSync) lfoY_.requestReset();
    if (currentParams.lfoZResetPhase && !zGridSync) lfoZ_.requestReset();
    syncLfoPhase(lfoX_, currentParams.lfoXBeatDiv, currentParams.lfoSpeedMul,
                 currentParams.lfoXPhase, currentParams.lfoXTempoSync);
    syncLfoPhase(lfoY_, currentParams.lfoYBeatDiv, currentParams.lfoSpeedMul,
                 currentParams.lfoYPhase, currentParams.lfoYTempoSync);
    syncLfoPhase(lfoZ_, currentParams.lfoZBeatDiv, currentParams.lfoSpeedMul,
                 currentParams.lfoZPhase, currentParams.lfoZTempoSync);

    // -------------------------------------------------------------------------
    // Stereo orbit LFOs — per-block setup
    // -------------------------------------------------------------------------
    orbitLfoXY_.waveform = static_cast<dsp::LFOWaveform>(currentParams.stereoOrbitXYWaveform);
    orbitLfoXZ_.waveform = static_cast<dsp::LFOWaveform>(currentParams.stereoOrbitXZWaveform);
    orbitLfoYZ_.waveform = static_cast<dsp::LFOWaveform>(currentParams.stereoOrbitYZWaveform);
    orbitLfoXY_.setRateHz(lfoRate(currentParams.stereoOrbitXYRate, currentParams.stereoOrbitXYBeatDiv, currentParams.stereoOrbitXYTempoSync) * (currentParams.stereoOrbitXYTempoSync ? quantizeSyncSpeed(currentParams.stereoOrbitSpeedMul) : currentParams.stereoOrbitSpeedMul));
    orbitLfoXZ_.setRateHz(lfoRate(currentParams.stereoOrbitXZRate, currentParams.stereoOrbitXZBeatDiv, currentParams.stereoOrbitXZTempoSync) * (currentParams.stereoOrbitXZTempoSync ? quantizeSyncSpeed(currentParams.stereoOrbitSpeedMul) : currentParams.stereoOrbitSpeedMul));
    orbitLfoYZ_.setRateHz(lfoRate(currentParams.stereoOrbitYZRate, currentParams.stereoOrbitYZBeatDiv, currentParams.stereoOrbitYZTempoSync) * (currentParams.stereoOrbitYZTempoSync ? quantizeSyncSpeed(currentParams.stereoOrbitSpeedMul) : currentParams.stereoOrbitSpeedMul));
    const bool xyGridSync = currentParams.stereoOrbitXYTempoSync && ppqPlaying;
    const bool xzGridSync = currentParams.stereoOrbitXZTempoSync && ppqPlaying;
    const bool yzGridSync = currentParams.stereoOrbitYZTempoSync && ppqPlaying;
    if (!xyGridSync) orbitLfoXY_.setPhaseOffset(currentParams.stereoOrbitXYPhase);
    if (!xzGridSync) orbitLfoXZ_.setPhaseOffset(currentParams.stereoOrbitXZPhase);
    if (!yzGridSync) orbitLfoYZ_.setPhaseOffset(currentParams.stereoOrbitYZPhase);
    orbitLfoXY_.setSmoothMs(currentParams.stereoOrbitXYSmooth * 300.0f);
    orbitLfoXZ_.setSmoothMs(currentParams.stereoOrbitXZSmooth * 300.0f);
    orbitLfoYZ_.setSmoothMs(currentParams.stereoOrbitYZSmooth * 300.0f);
    if (currentParams.stereoOrbitXYResetPhase && !xyGridSync) orbitLfoXY_.requestReset();
    if (currentParams.stereoOrbitXZResetPhase && !xzGridSync) orbitLfoXZ_.requestReset();
    if (currentParams.stereoOrbitYZResetPhase && !yzGridSync) orbitLfoYZ_.requestReset();
    syncLfoPhase(orbitLfoXY_, currentParams.stereoOrbitXYBeatDiv, currentParams.stereoOrbitSpeedMul,
                 currentParams.stereoOrbitXYPhase, currentParams.stereoOrbitXYTempoSync);
    syncLfoPhase(orbitLfoXZ_, currentParams.stereoOrbitXZBeatDiv, currentParams.stereoOrbitSpeedMul,
                 currentParams.stereoOrbitXZPhase, currentParams.stereoOrbitXZTempoSync);
    syncLfoPhase(orbitLfoYZ_, currentParams.stereoOrbitYZBeatDiv, currentParams.stereoOrbitSpeedMul,
                 currentParams.stereoOrbitYZPhase, currentParams.stereoOrbitYZTempoSync);

    // -------------------------------------------------------------------------
    // Per-block orbit angular smoother update
    // Analytically converge sin/cos IIR over the full block (pow),
    // matching the OnePoleSmooth::converge() behaviour used for listener XYZ.
    // -------------------------------------------------------------------------
    constexpr float kPI_early = 3.14159265358979323846f;
    constexpr float kTwoPI_early = 2.0f * kPI_early;
    float blkSmoothedPhase = 0.0f, blkSmoothedOffset = 0.0f;
    {
        const float smoothedWidth_peek = stereoWidthSmooth_.current();
        if (smoothedWidth_peek > 0.001f && inputR != nullptr) {
            const float aN = std::pow(angularSmA_, static_cast<float>(numSamples));
            const float bN = 1.0f - aN;
            {
                const float phaseRad = currentParams.stereoOrbitPhase * kTwoPI_early;
                phaseSmCos_ = std::cos(phaseRad) * bN + phaseSmCos_ * aN;
                phaseSmSin_ = std::sin(phaseRad) * bN + phaseSmSin_ * aN;
            }
            {
                const float offsetRad = currentParams.stereoOrbitOffset * kTwoPI_early;
                offsetSmCos_ = std::cos(offsetRad) * bN + offsetSmCos_ * aN;
                offsetSmSin_ = std::sin(offsetRad) * bN + offsetSmSin_ * aN;
            }
            blkSmoothedPhase  = std::atan2(phaseSmSin_,  phaseSmCos_);
            blkSmoothedOffset = std::atan2(offsetSmSin_, offsetSmCos_);
        }
    }

    // -------------------------------------------------------------------------
    // Per-block coefficient pre-computation
    // All transcendental math (cos, sin, pow, sqrt, tan, exp) runs here ONCE
    // per block, not per sample. Filter .process() calls remain per-sample.
    // -------------------------------------------------------------------------
    constexpr float kPI = 3.14159265358979323846f;
    constexpr float kTwoPI = 2.0f * kPI;


    // Pre-compute dB-to-linear conversions (std::pow) once per block
    const float chestGainLin   = std::pow(10.0f, currentParams.chestGainDb / 20.0f);
    const float floorGainLin   = std::pow(10.0f, currentParams.floorGainDb / 20.0f);
    ildGainBase_               = std::pow(10.0f, -currentParams.ildMaxDb / 20.0f);
    // kHardpanMaxDb is constexpr — computed once, not every block
    static const float kHardpanGainLin = std::pow(10.0f, kHardpanMaxDb / 20.0f);
    hardpanGainBase_           = kHardpanGainLin;
    const float auxMaxBoostLin   = std::pow(10.0f, currentParams.auxSendGainMaxDb / 20.0f);
    static const float auxERSendGainLin = std::pow(10.0f, kAuxERSendGainDb / 20.0f);
    const float blkDistGainMaxDb = 20.0f * std::log10(currentParams.distGainMax);

    // Distance gain curve + sphere range — block-constant, evaluated per sample
    const DistGainCurve blkDistCurve = makeDistGainCurve(blkDistGainMaxDb,
        currentParams.distGainFloorDb, currentParams.distCurveSteep, currentParams.distGainMax);
    const float blkMaxRange = std::max(currentParams.sphereRadius - kMinDistance, 0.001f);
    const float blkInvMaxRange = 1.0f / blkMaxRange;

    // Per-block chest filter coefficient update (cheap: only recalc when params change)
    {
        const float chestHPF = currentParams.chestHPFHz;
        const float chestLP  = currentParams.chestLPHz;
        for (int n = 0; n < kMaxSwarmNodes; ++n) {
            for (auto& hp : nodes_[n].chest.hpf)
                hp.setCoefficients(chestHPF, sr);
            nodes_[n].chest.lp.setCoefficients(chestLP, sr);
        }
    }

    // Per-block scaling factor pre-computation (avoids per-sample multiply)
    for (int n = 0; n < kMaxSwarmNodes; ++n) {
        nodes_[n].chest.setBlockConstants(sr, currentParams.chestDelayMaxMs);
        nodes_[n].floor.setBlockConstants(sr, currentParams.floorDelayMaxMs);
        nodes_[n].distance.setBlockConstants(sr, currentParams.distDelayMaxMs,
                                             blkDistCurve, blkInvMaxRange);
    }

    // Smooth base position (without LFO) — 150ms time constant absorbs mouse-drag
    // jitter for block-rate EQ targets and per-sample interpolation base.
    // LFO modulation adds on top per-sample, unaffected by the smoothing.
    blkPosXSmooth_.converge(currentParams.x, numSamples);
    blkPosYSmooth_.converge(currentParams.y, numSamples);
    blkPosZSmooth_.converge(currentParams.z, numSamples);
    const float smoothBaseX = blkPosXSmooth_.current();
    const float smoothBaseY = blkPosYSmooth_.current();
    const float smoothBaseZ = blkPosZSmooth_.current();

    // Per-sample interpolation from previous block's position to current.
    // When LFOs are inactive, modX/modY/modZ would otherwise be block-constant,
    // causing staircase stepping in binaural/distance/doppler targets.  Linear
    // interpolation makes the mouse path produce the same smooth per-sample
    // position changes that the LFO path naturally produces.
    const float posInterpInc = 1.0f / static_cast<float>(numSamples);
    const float posDeltaX = smoothBaseX - prevSmoothBaseX_;
    const float posDeltaY = smoothBaseY - prevSmoothBaseY_;
    const float posDeltaZ = smoothBaseZ - prevSmoothBaseZ_;
    prevSmoothBaseX_ = smoothBaseX;
    prevSmoothBaseY_ = smoothBaseY;
    prevSmoothBaseZ_ = smoothBaseZ;

    // Block-start position: smoothed base + LFO peek (no phase advance) so
    // per-block filter coefficients track the modulated position.  LFO delta
    // within one 64-128 sample block is negligible for coefficient purposes.
    const float blkDepthMul = lfoDepthMulSmooth_.current();
    float blkX = smoothBaseX + lfoX_.peek() * lfoDepthXSmooth_.current() * blkDepthMul;
    float blkY = smoothBaseY + lfoY_.peek() * lfoDepthYSmooth_.current() * blkDepthMul;
    float blkZ = smoothBaseZ + lfoZ_.peek() * lfoDepthZSmooth_.current() * blkDepthMul;

    // Walker mode: subtract listener position before head rotation so all
    // downstream DSP (pinna, presence, distance, air absorption) operates
    // on listener-relative coordinates.
    blkX -= currentParams.listenerX;
    blkY -= currentParams.listenerY;
    blkZ -= currentParams.listenerZ;

    // Listener head rotation — angular-smooth yaw/pitch/roll to handle 360°↔0° wrap.
    // While the parameter is actively changing, the IIR tracks the target using the
    // per-sample coefficient (for circle-wrap handling).  Once movement stops, the
    // IIR continues running with a block-rate coefficient (~100ms convergence) so
    // the transition is smooth.  Once converged, snap to exact target for
    // deterministic steady-state.
    float cosY, sinY, cosP, sinP, cosR, sinR;
    bool listenerFlipSnap = false;
    {
        const bool listenerParamsChanged =
            (currentParams.listenerYaw   != prevListenerYaw_) ||
            (currentParams.listenerPitch != prevListenerPitch_) ||
            (currentParams.listenerRoll  != prevListenerRoll_);

        if (listenerParamsChanged) {
            // Snap-on-flip: when pitch crosses ±90° via mouse drag, quatToRPY
            // (ui/QuatMath.h) jumps yaw and roll by ~π simultaneously. The physical
            // orientation is continuous, but its Euler representation isn't — so
            // IIR-ramping the cos/sin pairs through the flip would collapse the
            // rotation matrix mid-block (click). Detect and snap instead.
            auto wrapPi = [](float a) {
                constexpr float kTwoPi = 6.28318530717958647692f;
                constexpr float kPi    = 3.14159265358979323846f;
                a = std::fmod(a + kPi, kTwoPi);
                if (a < 0.0f) a += kTwoPi;
                return a - kPi;
            };
            constexpr float kHalfPi   = 1.57079632679489661923f;
            constexpr float kPi       = 3.14159265358979323846f;
            constexpr float kFlipTol  = 0.35f;   // ~20° around the ideal ±π jump
            constexpr float kPitchTol = 0.30f;   // only near the pole
            const float dYaw  = wrapPi(currentParams.listenerYaw  - prevListenerYaw_);
            const float dRoll = wrapPi(currentParams.listenerRoll - prevListenerRoll_);
            listenerFlipSnap =
                std::abs(std::abs(dYaw)  - kPi) < kFlipTol &&
                std::abs(std::abs(dRoll) - kPi) < kFlipTol &&
                std::abs(std::abs(currentParams.listenerPitch) - kHalfPi) < kPitchTol;

            prevListenerYaw_   = currentParams.listenerYaw;
            prevListenerPitch_ = currentParams.listenerPitch;
            prevListenerRoll_  = currentParams.listenerRoll;
            listenerSettled_ = false;

            if (listenerFlipSnap) {
                // Bypass IIR: write targets directly so rotation matrix stays unit.
                yawSmCos_   = dsp::SineLUT::cosLookupAngle(currentParams.listenerYaw);
                yawSmSin_   = dsp::SineLUT::lookupAngle   (currentParams.listenerYaw);
                pitchSmCos_ = dsp::SineLUT::cosLookupAngle(currentParams.listenerPitch);
                pitchSmSin_ = dsp::SineLUT::lookupAngle   (currentParams.listenerPitch);
                rollSmCos_  = dsp::SineLUT::cosLookupAngle(currentParams.listenerRoll);
                rollSmSin_  = dsp::SineLUT::lookupAngle   (currentParams.listenerRoll);
            } else {
                // While knob is moving: block-rate IIR (5ms time constant, tracks rapidly)
                const float aM = listenerMovSmA_;
                const float bM = 1.0f - aM;
                yawSmCos_   = dsp::SineLUT::cosLookupAngle(currentParams.listenerYaw)   * bM + yawSmCos_   * aM;
                yawSmSin_   = dsp::SineLUT::lookupAngle(currentParams.listenerYaw)       * bM + yawSmSin_   * aM;
                pitchSmCos_ = dsp::SineLUT::cosLookupAngle(currentParams.listenerPitch) * bM + pitchSmCos_ * aM;
                pitchSmSin_ = dsp::SineLUT::lookupAngle(currentParams.listenerPitch)     * bM + pitchSmSin_ * aM;
                rollSmCos_  = dsp::SineLUT::cosLookupAngle(currentParams.listenerRoll)  * bM + rollSmCos_  * aM;
                rollSmSin_  = dsp::SineLUT::lookupAngle(currentParams.listenerRoll)      * bM + rollSmSin_  * aM;
            }

            // Normalize IIR vectors to extract cos/sin directly
            auto normCS = [](float c, float s, float& outCos, float& outSin) {
                const float mag = std::sqrt(c * c + s * s);
                const float inv = (mag > 1e-12f) ? 1.0f / mag : 1.0f;
                outCos = c * inv;
                outSin = s * inv;
            };
            normCS(yawSmCos_,   yawSmSin_,   cosY, sinY);
            normCS(pitchSmCos_, pitchSmSin_, cosP, sinP);
            normCS(rollSmCos_,  rollSmSin_,  cosR, sinR);

            cachedCosY_ = cosY; cachedSinY_ = sinY;
            cachedCosP_ = cosP; cachedSinP_ = sinP;
            cachedCosR_ = cosR; cachedSinR_ = sinR;
        } else if (!listenerSettled_) {
            // Knob stopped — continue IIR toward target with block-rate coefficient
            // for smooth convergence (~100ms).
            const float aBlk = listenerBlkSmA_;
            const float bBlk = 1.0f - aBlk;
            const float tYC = dsp::SineLUT::cosLookupAngle(currentParams.listenerYaw);
            const float tYS = dsp::SineLUT::lookupAngle(currentParams.listenerYaw);
            const float tPC = dsp::SineLUT::cosLookupAngle(currentParams.listenerPitch);
            const float tPS = dsp::SineLUT::lookupAngle(currentParams.listenerPitch);
            const float tRC = dsp::SineLUT::cosLookupAngle(currentParams.listenerRoll);
            const float tRS = dsp::SineLUT::lookupAngle(currentParams.listenerRoll);

            yawSmCos_   = tYC * bBlk + yawSmCos_   * aBlk;
            yawSmSin_   = tYS * bBlk + yawSmSin_   * aBlk;
            pitchSmCos_ = tPC * bBlk + pitchSmCos_ * aBlk;
            pitchSmSin_ = tPS * bBlk + pitchSmSin_ * aBlk;
            rollSmCos_  = tRC * bBlk + rollSmCos_  * aBlk;
            rollSmSin_  = tRS * bBlk + rollSmSin_  * aBlk;

            auto normCS = [](float c, float s, float& outCos, float& outSin) {
                const float mag = std::sqrt(c * c + s * s);
                const float inv = (mag > 1e-12f) ? 1.0f / mag : 1.0f;
                outCos = c * inv;
                outSin = s * inv;
            };
            normCS(yawSmCos_,   yawSmSin_,   cosY, sinY);
            normCS(pitchSmCos_, pitchSmSin_, cosP, sinP);
            normCS(rollSmCos_,  rollSmSin_,  cosR, sinR);

            // Snap to exact when close enough (avoid infinite asymptotic tail)
            constexpr float kSnapThresh = 1e-6f;
            if (std::abs(yawSmCos_   - tYC) < kSnapThresh && std::abs(yawSmSin_   - tYS) < kSnapThresh &&
                std::abs(pitchSmCos_ - tPC) < kSnapThresh && std::abs(pitchSmSin_ - tPS) < kSnapThresh &&
                std::abs(rollSmCos_  - tRC) < kSnapThresh && std::abs(rollSmSin_  - tRS) < kSnapThresh) {
                cosY = tYC; sinY = tYS;
                cosP = tPC; sinP = tPS;
                cosR = tRC; sinR = tRS;
                yawSmCos_ = tYC;   yawSmSin_ = tYS;
                pitchSmCos_ = tPC; pitchSmSin_ = tPS;
                rollSmCos_ = tRC;  rollSmSin_ = tRS;
                listenerSettled_ = true;
            }

            cachedCosY_ = cosY; cachedSinY_ = sinY;
            cachedCosP_ = cosP; cachedSinP_ = sinP;
            cachedCosR_ = cosR; cachedSinR_ = sinR;
        } else {
            cosY = cachedCosY_; sinY = cachedSinY_;
            cosP = cachedCosP_; sinP = cachedSinP_;
            cosR = cachedCosR_; sinR = cachedSinR_;
        }
    }

    constexpr float kRotEps = 1e-7f;
    const bool listenerRotated = (std::abs(sinY) > kRotEps || std::abs(cosY - 1.0f) > kRotEps
                               || std::abs(sinP) > kRotEps || std::abs(sinR) > kRotEps);

    // If an Euler-representation flip was detected, align the per-sample ramp
    // start with this block's target so no interpolation occurs — otherwise the
    // cos/sin pair would still lerp through (0,0) over the block (click).
    if (listenerFlipSnap) {
        prevCosY_ = cosY; prevSinY_ = sinY;
        prevCosP_ = cosP; prevSinP_ = sinP;
        prevCosR_ = cosR; prevSinR_ = sinR;
    }

    // Per-sample trig interpolation: linearly ramp cos/sin from previous block's
    // values to this block's values, eliminating block-boundary discontinuities
    // during fast head rotation (especially roll near close sources).
    const bool interpRotation = listenerRotated
        && (prevCosY_ != cosY || prevSinY_ != sinY
         || prevCosP_ != cosP || prevSinP_ != sinP
         || prevCosR_ != cosR || prevSinR_ != sinR);
    const float invN = 1.0f / static_cast<float>(numSamples);
    const float dCosY = (cosY - prevCosY_) * invN;
    const float dSinY = (sinY - prevSinY_) * invN;
    const float dCosP = (cosP - prevCosP_) * invN;
    const float dSinP = (sinP - prevSinP_) * invN;
    const float dCosR = (cosR - prevCosR_) * invN;
    const float dSinR = (sinR - prevSinR_) * invN;
    float rCosY = prevCosY_, rSinY = prevSinY_;
    float rCosP = prevCosP_, rSinP = prevSinP_;
    float rCosR = prevCosR_, rSinR = prevSinR_;
    // Store current values for next block
    prevCosY_ = cosY; prevSinY_ = sinY;
    prevCosP_ = cosP; prevSinP_ = sinP;
    prevCosR_ = cosR; prevSinR_ = sinR;

    // Save listener-relative (pre-head-rotation) coords for face-observer spread + ER
    const float blkRelX = blkX;
    const float blkRelY = blkY;
    const float blkRelZ = blkZ;

    // Rotate block-start position into listener-relative frame for EQ targets.
    // Inverse yaw around Z, then inverse pitch around X, then roll around Y-forward.
    if (listenerRotated) {
        const float rx = blkX * cosY + blkY * sinY;
        const float ry = -blkX * sinY + blkY * cosY;
        blkX = rx;
        blkY = ry * cosP + blkZ * sinP;
        blkZ = -ry * sinP + blkZ * cosP;
        // Roll around forward axis (Y in engine coords)
        const float rrx =  blkX * cosR - blkZ * sinR;
        const float rrz =  blkX * sinR + blkZ * cosR;
        blkX = rrx;
        blkZ = rrz;
    }

    // Swarm mode and node count — declared early so block EQ gate can reference them.
    // Clamped to what prepare() allocated: nothing upstream guarantees the
    // param stays within [1, kMaxSwarmNodes].
    const int activeNodeCount = std::clamp(currentParams.swarmNodeCount, 1, kMaxSwarmNodes);
    const auto swarmMode = static_cast<SwarmMovementMode>(currentParams.swarmMovementMode);

    // --- Per-block EQ coefficient setup ---
    // Mono path: setNodeBlockCoefficients on center position.
    // Stereo path: setNodeBlockCoefficients on L/R node positions (below).
    const bool congaLineActive = swarmMode == SwarmMovementMode::CongaLine && activeNodeCount > 1 && inputR != nullptr;
    // CongaLine gain taper step — per-node taper is (1 - step * n), hoisted from the node loop
    const float congaTaperStep = (activeNodeCount > 1)
        ? currentParams.swarmParam2 / static_cast<float>(activeNodeCount - 1) : 0.0f;
    const bool blkStereoLikely = (stereoWidthSmooth_.current() > 0.001f && inputR != nullptr) || congaLineActive;
    if (!blkStereoLikely) {
        setNodeBlockCoefficients(0, blkX, blkY, blkZ, sr, numSamples);
    }

    // Head shadow + rear shadow SVFs: coefficients updated per-sample in inner loop

    // Floor bounce HF absorption LPF — smoothed to avoid block-boundary clicks
    for (int n = 0; n < kMaxSwarmNodes; ++n) {
        nodes_[n].floor.lpfL.setCoefficientsSmoothed(currentParams.floorAbsHz, sr, numSamples);
        nodes_[n].floor.lpfR.setCoefficientsSmoothed(currentParams.floorAbsHz, sr, numSamples);
    }

    // ER wall absorption LPF — per-block smoothed (was per-sample setCoefficients)
    const float erDampCutoff = kERDampingLPMaxHz
        + (kERDampingLPMinHz - kERDampingLPMaxHz) * currentParams.erDamping;
    for (int n = 0; n < kMaxSwarmNodes; ++n)
        nodes_[n].er.updateWallAbsorption(erDampCutoff, sr, numSamples);

    // ER directional cues — per-block pinna EQ + near-field coefficients (mono path).
    // Skip when stereo is active — the stereo path below sets per-node ER coefficients.
    if (!blkStereoLikely) {
        nodes_[0].er.updateTapDirectionalCoeffs(
            blkRelX, blkRelY, blkRelZ,
            currentParams.listenerX, currentParams.listenerY, currentParams.listenerZ,
            currentParams.erRoomSize, sr, numSamples,
            listenerRotated, cosY, sinY, cosP, sinP, cosR, sinR,
            currentParams.sphereRadius, currentParams);
    }

    // --- Stereo/swarm path per-block setCoefficients ---
    // Compute N-node block-start positions using SwarmMode, then set per-node EQ.
    // Pre-cache orbit XY trig for inner loop when orbit XY depth is zero
    // (angle is block-constant, saves LUT lookups per sample)
    NodeOffset blkNodeOffsets[kMaxSwarmNodes];
    {
        const float blkHalfSpread = currentParams.stereoWidth * kStereoMaxSpreadRadius;
        float blkSpreadX = 1.0f, blkSpreadY = 0.0f;
        const float blkRelHorizMag = std::sqrt(blkRelX * blkRelX + blkRelY * blkRelY);
        if (currentParams.stereoFaceListener && blkRelHorizMag > 1e-5f) {
            blkSpreadX =  blkRelY / blkRelHorizMag;
            blkSpreadY = -blkRelX / blkRelHorizMag;
        }

        // Peek orbit LFOs to get block-start orbit state (before per-sample ticks)
        const float blkOrbitRawXY = orbitLfoXY_.peek();
        const float blkOrbitRawXZ = orbitLfoXZ_.peek();
        const float blkOrbitRawYZ = orbitLfoYZ_.peek();
        const float blkOrbitDMul  = orbitDepthMulSmooth_.current();
        const float blkOrbitDepXY = orbitDepthXYSmooth_.current() * blkOrbitDMul;
        const float blkOrbitDepXZ = orbitDepthXZSmooth_.current() * blkOrbitDMul;
        const float blkOrbitDepYZ = orbitDepthYZSmooth_.current() * blkOrbitDMul;

        const float blkOrbitAngleXY = blkOrbitRawXY * blkOrbitDepXY * kPI;

        // Compute block-start node offsets using SwarmMode
        if (swarmMode == SwarmMovementMode::CongaLine) {
            // CongaLine block-start: same logic as per-sample but using peek values
            const float spacing = currentParams.swarmParam1;
            const float curPhaseX = lfoX_.getPhase();
            const float curPhaseY = lfoY_.getPhase();
            const float curPhaseZ = lfoZ_.getPhase();
            const float curValX = lfoX_.peekAtPhase(curPhaseX);
            const float curValY = lfoY_.peekAtPhase(curPhaseY);
            const float curValZ = lfoZ_.peekAtPhase(curPhaseZ);
            const float blkDX = lfoDepthXSmooth_.current() * blkDepthMul;
            const float blkDY = lfoDepthYSmooth_.current() * blkDepthMul;
            const float blkDZ = lfoDepthZSmooth_.current() * blkDepthMul;
            const float nf = static_cast<float>(activeNodeCount);
            for (int n = 0; n < activeNodeCount; ++n) {
                const float phaseShift = static_cast<float>(n) * spacing / nf;
                blkNodeOffsets[n] = {
                    (lfoX_.peekAtPhase(curPhaseX - phaseShift) - curValX) * blkDX,
                    (lfoY_.peekAtPhase(curPhaseY - phaseShift) - curValY) * blkDY,
                    (lfoZ_.peekAtPhase(curPhaseZ - phaseShift) - curValZ) * blkDZ
                };
            }
        } else {
            computeSwarmOffsets(swarmMode, activeNodeCount,
                blkHalfSpread, blkSpreadX, blkSpreadY,
                blkOrbitAngleXY, blkOrbitRawXY,
                blkSmoothedOffset, blkSmoothedPhase,
                blkOrbitDepXZ, blkOrbitRawXZ,
                blkOrbitDepYZ, blkOrbitRawYZ,
                currentParams.swarmParam1, currentParams.swarmParam2, currentParams.swarmParam3,
                blkNodeOffsets);
        }

        // Per-node block EQ + ER directional cues
        for (int n = 0; n < activeNodeCount; ++n) {
            const float blkNodeX = blkX + blkNodeOffsets[n].dx;
            const float blkNodeY = blkY + blkNodeOffsets[n].dy;
            const float blkNodeZ = blkZ + blkNodeOffsets[n].dz;
            setNodeBlockCoefficients(n, blkNodeX, blkNodeY, blkNodeZ, sr, numSamples);

            nodes_[n].er.updateTapDirectionalCoeffs(
                blkRelX + blkNodeOffsets[n].dx, blkRelY + blkNodeOffsets[n].dy, blkRelZ + blkNodeOffsets[n].dz,
                currentParams.listenerX, currentParams.listenerY, currentParams.listenerZ,
                currentParams.erRoomSize, sr, numSamples,
                listenerRotated, cosY, sinY, cosP, sinP, cosR, sinR,
                currentParams.sphereRadius, currentParams);
        }

        // Compute end-of-block offsets for linear interpolation (non-CongaLine).
        // Replaces per-sample computeSwarmOffsets with simple add-per-sample.
        if (swarmMode != SwarmMovementMode::CongaLine) {
            const float endPhaseXY = orbitLfoXY_.getPhase() + orbitLfoXY_.getIncrement() * static_cast<float>(numSamples);
            const float endPhaseXZ = orbitLfoXZ_.getPhase() + orbitLfoXZ_.getIncrement() * static_cast<float>(numSamples);
            const float endPhaseYZ = orbitLfoYZ_.getPhase() + orbitLfoYZ_.getIncrement() * static_cast<float>(numSamples);
            const float endOrbitRawXY = orbitLfoXY_.peekAtPhase(endPhaseXY);
            const float endOrbitRawXZ = orbitLfoXZ_.peekAtPhase(endPhaseXZ);
            const float endOrbitRawYZ = orbitLfoYZ_.peekAtPhase(endPhaseYZ);
            const float endOrbitAngleXY = endOrbitRawXY * blkOrbitDepXY * kPI;

            // End-of-block spread direction (from smoothed end position)
            const float endRelXs = smoothBaseX - currentParams.listenerX;
            const float endRelYs = smoothBaseY - currentParams.listenerY;
            const float endRelHorizMag = std::sqrt(endRelXs * endRelXs + endRelYs * endRelYs);
            float endSpreadX = 1.0f, endSpreadY = 0.0f;
            if (currentParams.stereoFaceListener && endRelHorizMag > 1e-5f) {
                endSpreadX =  endRelYs / endRelHorizMag;
                endSpreadY = -endRelXs / endRelHorizMag;
            }

            NodeOffset endNodeOffsets[kMaxSwarmNodes];
            computeSwarmOffsets(swarmMode, activeNodeCount,
                blkHalfSpread, endSpreadX, endSpreadY,
                endOrbitAngleXY, endOrbitRawXY,
                blkSmoothedOffset, blkSmoothedPhase,
                blkOrbitDepXZ, endOrbitRawXZ,
                blkOrbitDepYZ, endOrbitRawYZ,
                currentParams.swarmParam1, currentParams.swarmParam2, currentParams.swarmParam3,
                endNodeOffsets);

            const float invN = 1.0f / static_cast<float>(numSamples);
            for (int n = 0; n < activeNodeCount; ++n) {
                swarmOffsetInc_[n] = {
                    (endNodeOffsets[n].dx - blkNodeOffsets[n].dx) * invN,
                    (endNodeOffsets[n].dy - blkNodeOffsets[n].dy) * invN,
                    (endNodeOffsets[n].dz - blkNodeOffsets[n].dz) * invN
                };
                swarmOffsetCur_[n] = blkNodeOffsets[n];
            }
        }
    }

    // Per-node delay offset in samples (stagger across nodes).
    // Custom mode uses absolute per-node values, so it engages regardless of the
    // Max Delay knob (which only scales the generated distributions).
    float nodeDelayOffsetSamp[kMaxSwarmNodes] = {};
    const bool delayCustom = (currentParams.swarmDelayMode == 4);
    if (delayCustom && activeNodeCount > 1) {
        // Max Delay acts as a macro scaler on the custom delay map: 1000ms = 100%
        const float scale = currentParams.swarmDelayOffset * (1.0f / 1000.0f);
        for (int n = 0; n < activeNodeCount; ++n) {
            const float ms = currentParams.customNodeDelay[n] * scale;
            const float clamped = ms < 0.0f ? 0.0f
                : (ms > kSwarmDelayOffsetCapMs ? kSwarmDelayOffsetCapMs : ms);
            nodeDelayOffsetSamp[n] = clamped * 0.001f * sr;
        }
    } else if (!delayCustom && currentParams.swarmDelayOffset > 0.0f && activeNodeCount > 1) {
        const float maxDelayMs = std::min(currentParams.swarmDelayOffset, kSwarmDelayOffsetCapMs);
        const int N = activeNodeCount;
        const float nf = static_cast<float>(N - 1);
        // Legacy CongaLine param3 curve only applies in Linear mode (it predates
        // the delay mode parameter and shapes the same ramp)
        const float delayCurve = (currentParams.swarmDelayMode == 0
                                  && swarmMode == SwarmMovementMode::CongaLine)
                                 ? currentParams.swarmParam3 : 0.0f;

        // Sorted delay values (ascending), shaped by the delay mode
        float sortedDelay[kMaxSwarmNodes];
        for (int n = 0; n < N; ++n) {
            float t = static_cast<float>(n) / nf;
            switch (currentParams.swarmDelayMode) {
                case 0: // Linear ramp (+ optional CongaLine curve)
                default:
                    if (delayCurve > 0.001f) {
                        // 0=linear, <0.5=sqrt (clustered early), >0.5=quadratic (clustered late)
                        if (delayCurve < 0.5f) {
                            const float blend = delayCurve * 2.0f;
                            t = t * (1.0f - blend) + std::sqrt(t) * blend;
                        } else {
                            const float blend = (delayCurve - 0.5f) * 2.0f;
                            const float sqrtT = std::sqrt(t);
                            t = sqrtT * (1.0f - blend) + (t * t) * blend;
                        }
                    }
                    break;
                case 1: // Exponential — delays cluster early, spread late (var = power)
                {
                    const int p = currentParams.swarmDelayModeVar < 2 ? 2
                                : (currentParams.swarmDelayModeVar > 8 ? 8 : currentParams.swarmDelayModeVar);
                    float acc = t;
                    for (int k = 1; k < p; ++k) acc *= t;
                    t = acc;
                    break;
                }
                case 2: // Logarithmic — delays spread early, cluster late (var = root power)
                {
                    const int p = currentParams.swarmDelayModeVar < 2 ? 2
                                : (currentParams.swarmDelayModeVar > 8 ? 8 : currentParams.swarmDelayModeVar);
                    t = std::pow(t, 1.0f / static_cast<float>(p));
                    break;
                }
                case 3: // Steps — quantized to var evenly-spaced levels (2-8)
                {
                    const int levels = currentParams.swarmDelayModeVar < 2 ? 2
                                     : (currentParams.swarmDelayModeVar > 8 ? 8 : currentParams.swarmDelayModeVar);
                    const float lf = static_cast<float>(levels - 1);
                    t = std::round(t * lf) / lf;
                    break;
                }
            }
            sortedDelay[n] = t * maxDelayMs * 0.001f * sr;
        }

        // Distribute across nodes via the delay order permutation. The seed is
        // shared with pitch but salted so Random gives independent patterns.
        int delayPerm[kMaxSwarmNodes];
        buildOrderPermutation(currentParams.swarmDelayOrder, N,
                              (static_cast<unsigned int>(N) * 2654435761u)
                              ^ (static_cast<unsigned int>(currentParams.swarmPitchSeed) * 1013904223u)
                              ^ 0x9E3779B9u,
                              currentParams.swarmDelayOrderVar,
                              delayPerm);
        for (int n = 0; n < N; ++n)
            nodeDelayOffsetSamp[n] = sortedDelay[delayPerm[n]];
    }

    // Per-node gain from dB parameter (0 to -6 dB range)
    const float swarmNodeGain = std::pow(10.0f, currentParams.swarmNodeGainDb / 20.0f);
    // Pre-computed node-count constants (block-constant, avoids per-sample sqrt/div)
    const float fNodeCount = static_cast<float>(activeNodeCount);
    const float invSqrtNodeCount = 1.0f / std::sqrt(fNodeCount);
    const float sqrtNodeCount = std::sqrt(fNodeCount);
    const float invNodeCountBlk = 1.0f / fNodeCount;

    // Per-node pitch offset in semitones (spread across nodes)
    float nodePitchSemitones[kMaxSwarmNodes] = {};
    // Block-level flag: is the pitch system engaged at all this block? When it
    // is, every node runs through its shifter (even unison nodes) so the grain
    // engine stays primed and per-node pitch transitions to/from 0st ramp
    // smoothly via the shifter's internal ratio smoothing instead of snapping
    // between dry and pitched output (which clicked).
    const bool pitchActive = (currentParams.swarmPitchMode == 5 || currentParams.swarmPitchSpread > 0.001f) && activeNodeCount > 1;
    if (pitchActive) {
        const float spread = currentParams.swarmPitchSpread;
        const int mode = currentParams.swarmPitchMode;
        const int N = activeNodeCount;
        const float nf = static_cast<float>(N - 1);

        // Compute pitch values in sorted order first
        float sorted[kMaxSwarmNodes] = {};
        for (int n = 0; n < N; ++n) {
            const float t = static_cast<float>(n) / nf;  // 0..1
            switch (mode) {
                case 0: // Detune — centered, continuous spread ± half-range
                {
                    sorted[n] = (2.0f * t - 1.0f) * spread * 0.5f;
                    break;
                }
                case 1: // Chromatic — centered, quantized to var-semitone steps
                        // (1 = chromatic, 2 = whole tone, 3 = minor thirds, ...)
                default:
                {
                    const float step =
                        currentParams.swarmPitchModeVar < 1.0f ? 1.0f
                        : (currentParams.swarmPitchModeVar > 8.0f ? 8.0f : currentParams.swarmPitchModeVar);
                    sorted[n] = std::round((2.0f * t - 1.0f) * spread * 0.5f / step) * step;
                    break;
                }
                case 2: // Octaves — ±spread range, quantized to 12st intervals
                {
                    // t maps -1..+1 so nodes span ±spread around unison
                    const float raw = (2.0f * t - 1.0f) * spread;
                    sorted[n] = std::round(raw / 12.0f) * 12.0f;
                    break;
                }
                case 3: // Fifths — ±spread range, quantized to 7st intervals
                {
                    // t maps -1..+1 so nodes span ±spread around unison
                    const float raw = (2.0f * t - 1.0f) * spread;
                    sorted[n] = std::round(raw / 7.0f) * 7.0f;
                    break;
                }
                case 4: // Harmonics — natural harmonic series / ratio stacks
                {
                    // spread controls max partial (0-64 maps to partials 1-16);
                    // var = stack ratio: 1 = continuous harmonic sweep (legacy),
                    // 1.5 = stacked fifths (1, 1.5, 2.25, ...), 2 = octave stack
                    // (1, 2, 4, 8), 3 = stacked twelfths, etc.
                    const float maxPartial = 1.0f + (spread / 64.0f) * 15.0f; // 1..16
                    const float ratio =
                        currentParams.swarmPitchModeVar < 1.0f ? 1.0f
                        : (currentParams.swarmPitchModeVar > 8.0f ? 8.0f : currentParams.swarmPitchModeVar);
                    float partial;
                    if (ratio <= 1.001f) {
                        partial = 1.0f + t * (maxPartial - 1.0f);
                    } else {
                        // Geometric stack: ratio^m for m = 0..M, capped at maxPartial
                        const int M = std::max(0, static_cast<int>(
                            std::floor(std::log(maxPartial) / std::log(ratio))));
                        const int m = static_cast<int>(std::round(t * static_cast<float>(M)));
                        partial = std::pow(ratio, static_cast<float>(m));
                    }
                    sorted[n] = 12.0f * std::log2(partial);
                    break;
                }
                case 5: // Custom — per-node user-defined pitches, scaled by spread
                {
                    // Spread acts as a macro scaler on the map: 12 st = 100%
                    const float v = currentParams.customNodePitch[n] * (spread * (1.0f / 12.0f));
                    sorted[n] = v < -64.0f ? -64.0f : (v > 64.0f ? 64.0f : v);
                    break;
                }
            }
        }

        // Build ordering permutation: perm[i] = which sorted slot goes to node i
        int perm[kMaxSwarmNodes];
        buildOrderPermutation(currentParams.swarmPitchOrder, N,
                              (static_cast<unsigned int>(N) * 2654435761u)
                              ^ (static_cast<unsigned int>(currentParams.swarmPitchSeed) * 1013904223u),
                              currentParams.swarmPitchOrderVar,
                              perm);

        // Apply permutation (Custom mode bypasses — values are already per-node)
        if (mode == 5) {
            for (int n = 0; n < N; ++n)
                nodePitchSemitones[n] = sorted[n];
        } else {
            for (int n = 0; n < N; ++n)
                nodePitchSemitones[n] = sorted[perm[n]];
        }
    }

    // Store per-node values for UI display grid
    lastNodeDisplayValues_.count = activeNodeCount;
    for (int n = 0; n < activeNodeCount; ++n) {
        lastNodeDisplayValues_.pitchSemitones[n] = nodePitchSemitones[n];
        lastNodeDisplayValues_.delayMs[n] = nodeDelayOffsetSamp[n] * 1000.0f / static_cast<float>(sr);
    }

    // ER sharing policy
    // N=2 StereoOrbit always uses per-node ER for backward compatibility with
    // the original stereo path (regardless of erShared flag).
    const bool erLegacyPerNode = (swarmMode == SwarmMovementMode::StereoOrbit && activeNodeCount == 2);
    const bool erShared = !erLegacyPerNode && (currentParams.swarmERShared || activeNodeCount > 4);
    const bool erPerNode = erLegacyPerNode || (!erShared && activeNodeCount <= 4);

    // -------------------------------------------------------------------------
    // Per-sample loop
    // -------------------------------------------------------------------------
    const float testGainTargetBlock = currentParams.testToneEnabled
        ? std::pow(10.0f, currentParams.testToneGainDb / 20.0f)
        : 0.0f;
    const float sawIncrement = currentParams.testTonePitchHz / sr;
    pulseLFO_.setRateHz(currentParams.testTonePulseHz);
    std::uniform_real_distribution<float> noiseDist(-1.0f, 1.0f);
    const float verbPreDelayMaxSamp = currentParams.verbPreDelayMax * sr / 1000.0f;


    // Pre-converge block-constant smoothers analytically (O(1) instead of O(numSamples))
    // LFO depth smoothers run per-sample (not pre-converged) to avoid
    // block-boundary staircase jumps when the depth knob is dragged.
    const float lfoDepthXTarget = currentParams.lfoXDepth;
    const float lfoDepthYTarget = currentParams.lfoYDepth;
    const float lfoDepthZTarget = currentParams.lfoZDepth;
    const float lfoDepthMulTarget = currentParams.lfoDepthMul;
    orbitDepthXYSmooth_.converge(currentParams.stereoOrbitXYDepth, numSamples);
    orbitDepthXZSmooth_.converge(currentParams.stereoOrbitXZDepth, numSamples);
    orbitDepthYZSmooth_.converge(currentParams.stereoOrbitYZDepth, numSamples);
    orbitDepthMulSmooth_.converge(currentParams.stereoOrbitDepthMul, numSamples);
    stereoWidthSmooth_.converge(currentParams.stereoWidth, numSamples);
    binauralBlendSmooth_.converge(currentParams.binauralEnabled ? 1.0f : 0.0f, numSamples);

    for (int i = 0; i < numSamples; ++i) {
        // ----------------------------------------------------------------
        // Test tone generation
        // ----------------------------------------------------------------
        float testSig = 0.0f;
        float testSigL = 0.0f;
        float testSigR = 0.0f;
        bool  testStereo = false;
        const float smoothedTestGain = testToneGainSmooth_.process(testGainTargetBlock);
        if (currentParams.testToneEnabled) {
            switch (currentParams.testToneWaveform) {
                case xyzpan::TestToneWaveform::WhiteNoise:
                    testSig = noiseDist(noiseRng_);
                    break;
                case xyzpan::TestToneWaveform::PulsingWhiteNoise:
                    testSig = noiseDist(noiseRng_) * (pulseLFO_.tick() > 0.0f ? 1.0f : 0.0f);
                    break;
                case xyzpan::TestToneWaveform::PulsingSaw:
                    testSig = (2.0f * sawPhase_ - 1.0f) * (pulseLFO_.tick() > 0.0f ? 1.0f : 0.0f);
                    sawPhase_ += sawIncrement;
                    if (sawPhase_ >= 1.0f) sawPhase_ -= 1.0f;
                    break;
                case xyzpan::TestToneWaveform::PulsingSquare: {
                    const float gate = pulseLFO_.tick() > 0.0f ? 1.0f : 0.0f;
                    testSig = (sawPhase_ < 0.5f ? 1.0f : -1.0f) * gate;
                    sawPhase_ += sawIncrement;
                    if (sawPhase_ >= 1.0f) sawPhase_ -= 1.0f;
                    break;
                }
                case xyzpan::TestToneWaveform::Square:
                    testSig = sawPhase_ < 0.5f ? 1.0f : -1.0f;
                    sawPhase_ += sawIncrement;
                    if (sawPhase_ >= 1.0f) sawPhase_ -= 1.0f;
                    break;
                case xyzpan::TestToneWaveform::StereoNoiseSaw: {
                    testStereo = true;
                    const float gate = pulseLFO_.tick() > 0.0f ? 1.0f : 0.0f;
                    testSigL = noiseDist(noiseRng_) * gate;
                    testSigR = (2.0f * sawPhase_ - 1.0f) * gate;
                    sawPhase_ += sawIncrement;
                    if (sawPhase_ >= 1.0f) sawPhase_ -= 1.0f;
                    break;
                }
                case xyzpan::TestToneWaveform::Sine:
                    testSig = std::sin(6.283185307f * sawPhase_);
                    sawPhase_ += sawIncrement;
                    if (sawPhase_ >= 1.0f) sawPhase_ -= 1.0f;
                    break;
                case xyzpan::TestToneWaveform::Click: {
                    const float gate = pulseLFO_.tick() > 0.0f ? 1.0f : 0.0f;
                    const bool gateOn = gate > 0.5f;
                    if (gateOn && !prevPulseGate_)
                        clickSamplesLeft_ = static_cast<float>(sampleRate) * 0.001f;
                    prevPulseGate_ = gateOn;
                    testSig = (clickSamplesLeft_ > 0.0f) ? 1.0f : 0.0f;
                    clickSamplesLeft_ = std::max(0.0f, clickSamplesLeft_ - 1.0f);
                    break;
                }
                case xyzpan::TestToneWaveform::Triangle:
                    testSig = 4.0f * (sawPhase_ < 0.5f ? sawPhase_ : (1.0f - sawPhase_)) - 1.0f;
                    sawPhase_ += sawIncrement;
                    if (sawPhase_ >= 1.0f) sawPhase_ -= 1.0f;
                    break;
                case xyzpan::TestToneWaveform::Saw: default:
                    testSig = 2.0f * sawPhase_ - 1.0f;
                    sawPhase_ += sawIncrement;
                    if (sawPhase_ >= 1.0f) sawPhase_ -= 1.0f;
                    break;
            }
            if (testStereo) {
                testSigL *= smoothedTestGain;
                testSigR *= smoothedTestGain;
            } else {
                testSig *= smoothedTestGain;
            }
        }

        // ----------------------------------------------------------------
        // Position LFOs + per-sample base interpolation
        // ----------------------------------------------------------------
        const float depthMul = lfoDepthMulSmooth_.process(lfoDepthMulTarget);
        const float depthX = lfoDepthXSmooth_.process(lfoDepthXTarget) * depthMul;
        const float depthY = lfoDepthYSmooth_.process(lfoDepthYTarget) * depthMul;
        const float depthZ = lfoDepthZSmooth_.process(lfoDepthZTarget) * depthMul;

        // Per-sample interpolated base position (prev→current across block)
        const float posT = static_cast<float>(i + 1) * posInterpInc;
        const float interpBaseX = smoothBaseX - posDeltaX * (1.0f - posT);
        const float interpBaseY = smoothBaseY - posDeltaY * (1.0f - posT);
        const float interpBaseZ = smoothBaseZ - posDeltaZ * (1.0f - posT);

        float modX, modY, modZ;
        float lfoValX, lfoValY, lfoValZ;
        // Always tick LFOs to keep phase accumulation consistent
        const float rawLfoX = lfoX_.tick();
        const float rawLfoY = lfoY_.tick();
        const float rawLfoZ = lfoZ_.tick();
        lfoValX = rawLfoX * depthX;
        lfoValY = rawLfoY * depthY;
        lfoValZ = rawLfoZ * depthZ;
        modX = interpBaseX + lfoValX;
        modY = interpBaseY + lfoValY;
        modZ = interpBaseZ + lfoValZ;

        // Save world-space position for bridge — use unsmoothed position so the GL
        // source sphere tracks the mouse instantly (audio uses smoothed position).
        const float worldModX = currentParams.x + lfoValX;
        const float worldModY = currentParams.y + lfoValY;
        const float worldModZ = currentParams.z + lfoValZ;

        // Walker mode: subtract listener position (listener-relative for DSP)
        modX -= currentParams.listenerX;
        modY -= currentParams.listenerY;
        modZ -= currentParams.listenerZ;

        // Rotate into listener-relative frame for DSP (binaural cues, EQ targets).
        // When interpolating, use per-sample ramped trig values to avoid block-boundary jumps.
        if (listenerRotated) {
            const float cY = interpRotation ? rCosY : cosY;
            const float sY = interpRotation ? rSinY : sinY;
            const float cP = interpRotation ? rCosP : cosP;
            const float sP = interpRotation ? rSinP : sinP;
            const float cR = interpRotation ? rCosR : cosR;
            const float sR = interpRotation ? rSinR : sinR;
            const float rx = modX * cY + modY * sY;
            const float ry = -modX * sY + modY * cY;
            modX = rx;
            modY = ry * cP + modZ * sP;
            modZ = -ry * sP + modZ * cP;
            // Roll around forward axis (Y in engine coords)
            const float rrx =  modX * cR - modZ * sR;
            const float rrz =  modX * sR + modZ * cR;
            modX = rrx;
            modZ = rrz;
        }

        // Position-dependent targets from listener-relative center position
        // Always per-sample now (position interpolates across block)
        const float rawModDist = dsp::fastSqrt(modX * modX + modY * modY + modZ * modZ);
        const float modDist     = std::max(rawModDist, kMinDistance);
        const float modDistFrac = std::clamp((modDist - kMinDistance) * blkInvMaxRange, 0.0f, 1.0f);

        const float rawDistFrac = std::clamp(rawModDist * kInvSqrt3, 0.0f, 1.0f);

        // Distance targets — compressed cubic Hermite curve
        const float distGainTarget = evalDistGainCurve(blkDistCurve, modDistFrac);

        // Air absorption coefficients — mono path only (stereo sets per-node in processDistance)
        const float airCutoffMod = currentParams.airAbsMaxHz
            + (currentParams.airAbsMinHz - currentParams.airAbsMaxHz) * modDistFrac;

        // Capture modulated position (world-space for GL bridge)
        lastModulated_ = {worldModX, worldModY, worldModZ};

        // ----------------------------------------------------------------
        // Stereo width smooth + orbit LFOs
        // ----------------------------------------------------------------
        const float smoothedWidth = stereoWidthSmooth_.current();
        const bool stereoActive = (smoothedWidth > 0.001f && inputR != nullptr) || congaLineActive;

        // Orbit LFO ticks — always tick to keep phase accumulation consistent
        const float orbitDMul  = orbitDepthMulSmooth_.current();
        const float orbitDepXY = orbitDepthXYSmooth_.current() * orbitDMul;
        const float orbitDepXZ = orbitDepthXZSmooth_.current() * orbitDMul;
        const float orbitDepYZ = orbitDepthYZSmooth_.current() * orbitDMul;
        const float orbitRawXY = orbitLfoXY_.tick();
        const float orbitRawXZ = orbitLfoXZ_.tick();
        const float orbitRawYZ = orbitLfoYZ_.tick();
        const float orbitValXY = orbitRawXY * orbitDepXY;
        const float orbitValXZ = orbitRawXZ * orbitDepXZ;
        const float orbitValYZ = orbitRawYZ * orbitDepYZ;

        // Binaural blend — pre-converged above, read cached value
        const float binBlend = binauralBlendSmooth_.current();

        float dL, dR;
        float effectiveDistGain = 1.0f;
        float blendedDistFrac = modDistFrac;  // overwritten in stereo path
        float dopplerInputMono = 0.0f;        // doppler'd input for chest/ER
        float erReverbAccumL = 0.0f, erReverbAccumR = 0.0f;

        if (stereoActive) {
            // Smoothed world-space position (for DSP node placement)
            const float smoothWorldX = interpBaseX + lfoValX;
            const float smoothWorldY = interpBaseY + lfoValY;
            const float smoothWorldZ = interpBaseZ + lfoValZ;


            // Per-sample N-node offsets
            NodeOffset nodeOffsets[kMaxSwarmNodes];
            if (swarmMode == SwarmMovementMode::CongaLine) {
                // CongaLine: nodes follow the main-axis LFO path with phase offsets.
                const float spacing = currentParams.swarmParam1;
                const float curPhaseX = lfoX_.getPhase();
                const float curPhaseY = lfoY_.getPhase();
                const float curPhaseZ = lfoZ_.getPhase();
                const float curValX = lfoX_.peekAtPhase(curPhaseX);
                const float curValY = lfoY_.peekAtPhase(curPhaseY);
                const float curValZ = lfoZ_.peekAtPhase(curPhaseZ);
                const float phaseStep = spacing / static_cast<float>(activeNodeCount);
                for (int n = 0; n < activeNodeCount; ++n) {
                    const float phaseShift = static_cast<float>(n) * phaseStep;
                    nodeOffsets[n] = {
                        (lfoX_.peekAtPhase(curPhaseX - phaseShift) - curValX) * depthX,
                        (lfoY_.peekAtPhase(curPhaseY - phaseShift) - curValY) * depthY,
                        (lfoZ_.peekAtPhase(curPhaseZ - phaseShift) - curValZ) * depthZ
                    };
                }
            } else {
                // Block-rate interpolation: advance running offsets
                for (int n = 0; n < activeNodeCount; ++n) {
                    swarmOffsetCur_[n].dx += swarmOffsetInc_[n].dx;
                    swarmOffsetCur_[n].dy += swarmOffsetInc_[n].dy;
                    swarmOffsetCur_[n].dz += swarmOffsetInc_[n].dz;
                    nodeOffsets[n] = swarmOffsetCur_[n];
                }
            }

            // Get input samples -- test tone overrides both channels
            float sampleL = currentParams.testToneEnabled ? (testStereo ? testSigL : testSig) : inputL[i];
            float sampleR = currentParams.testToneEnabled ? (testStereo ? testSigR : testSig) : inputR[i];

            const bool effectiveDoppler = dopplerOn && !currentParams.bypassDoppler;

            // Per-sample trig for head rotation (shared across all nodes)
            const float cY = interpRotation ? rCosY : cosY;
            const float sY = interpRotation ? rSinY : sinY;
            const float cP = interpRotation ? rCosP : cosP;
            const float sP = interpRotation ? rSinP : sinP;
            const float cR = interpRotation ? rCosR : cosR;
            const float sR = interpRotation ? rSinR : sinR;

            // Accumulate node outputs + track positions for GL bridge
            dL = 0.0f;
            dR = 0.0f;
            float distFracSum = 0.0f;
            float distGainSum = 0.0f;
            float erInputSum = 0.0f;
            float centroidRelX = 0.0f, centroidRelY = 0.0f, centroidRelZ = 0.0f;

            // Per-node arrays for ER processing
            float nodeRelXArr[kMaxSwarmNodes], nodeRelYArr[kMaxSwarmNodes], nodeRelZArr[kMaxSwarmNodes];
            float nodeRawDistArr[kMaxSwarmNodes];
            float nodeDopplerInputArr[kMaxSwarmNodes];

            for (int n = 0; n < activeNodeCount; ++n) {
                // DSP node positions (smoothed base)
                const float nDspWorldX = smoothWorldX + nodeOffsets[n].dx;
                const float nDspWorldY = smoothWorldY + nodeOffsets[n].dy;
                const float nDspWorldZ = smoothWorldZ + nodeOffsets[n].dz;

                // Listener-relative node position
                const float nRelX = nDspWorldX - currentParams.listenerX;
                const float nRelY = nDspWorldY - currentParams.listenerY;
                const float nRelZ = nDspWorldZ - currentParams.listenerZ;
                nodeRelXArr[n] = nRelX;
                nodeRelYArr[n] = nRelY;
                nodeRelZArr[n] = nRelZ;
                centroidRelX += nRelX;
                centroidRelY += nRelY;
                centroidRelZ += nRelZ;

                // Rotate listener-relative position into head frame for binaural DSP
                float dspNX = nRelX, dspNY = nRelY, dspNZ = nRelZ;
                if (listenerRotated) {
                    float rx = nRelX * cY + nRelY * sY;
                    float ry = -nRelX * sY + nRelY * cY;
                    dspNX = rx;
                    dspNY = ry * cP + nRelZ * sP;
                    dspNZ = -ry * sP + nRelZ * cP;
                    float rrx =  dspNX * cR - dspNZ * sR;
                    float rrz =  dspNX * sR + dspNZ * cR;
                    dspNX = rrx;
                    dspNZ = rrz;
                }

                // Input assignment
                float nodeSample;
                if (swarmMode == SwarmMovementMode::StereoOrbit && activeNodeCount == 2) {
                    // N=2 StereoOrbit: node 0 = L, node 1 = R (exact legacy)
                    nodeSample = (n == 0) ? sampleL : sampleR;
                } else if (swarmMode == SwarmMovementMode::CongaLine) {
                    // CongaLine: all nodes trace same path (phase-offset), mono input
                    nodeSample = 0.5f * (sampleL + sampleR);
                } else {
                    // General: even=L, odd=R, last odd-count node = mono
                    if (n == activeNodeCount - 1 && (activeNodeCount & 1)) {
                        nodeSample = 0.5f * (sampleL + sampleR);
                    } else {
                        nodeSample = (n & 1) ? sampleR : sampleL;
                    }
                }

                // Per-node delay offset (stagger) — smooth target to avoid clicks
                {
                    const float smoothedDelay = nodes_[n].delayOffsetSmooth.process(nodeDelayOffsetSamp[n]);
                    if (smoothedDelay > 0.001f) {
                        nodes_[n].delayOffset.push(nodeSample);
                        nodeSample = nodes_[n].delayOffset.read(smoothedDelay);
                    }
                }

                // Per-node pitch shift (OLA). Run every node while the pitch
                // system is engaged so the grain engine stays primed and 0st
                // nodes ramp smoothly rather than hard-bypassing (click-free).
                if (pitchActive) {
                    nodes_[n].pitchShifter.setRatio(nodePitchSemitones[n]);
                    nodeSample = nodes_[n].pitchShifter.processSample(nodeSample);
                }

                // Doppler
                const float nNodeRawDist = dsp::fastSqrt(nRelX*nRelX + nRelY*nRelY + nRelZ*nRelZ);
                nodeRawDistArr[n] = nNodeRawDist;
                const float nRawDistFrac = std::clamp(nNodeRawDist * kInvSqrt3, 0.0f, 1.0f);
                nodeSample = nodes_[n].distance.processDoppler(nodeSample, nRawDistFrac, sr, effectiveDoppler, currentParams);
                nodeDopplerInputArr[n] = nodeSample;
                erInputSum += nodeSample;

                // Per-node DSP: chest -> binaural -> distance -> floor
                auto nResult = processNodeSample(n, nodeSample, nRelX, nRelY, nRelZ,
                    dspNX, dspNY, dspNZ, sr, binBlend,
                    chestGainLin, floorGainLin, currentParams);

                // Per-node gain taper for CongaLine (param2)
                // Chase uses param2 for radius taper only (spatial, not gain)
                float nodeGainTaper = 1.0f;
                if (swarmMode == SwarmMovementMode::CongaLine && activeNodeCount > 1) {
                    nodeGainTaper = 1.0f - congaTaperStep * static_cast<float>(n);
                }
                dL += nResult.left * nodeGainTaper;
                dR += nResult.right * nodeGainTaper;
                distFracSum += nResult.distFrac;
                distGainSum += nResult.distGain;
            }

            // Apply N-node gain scaling with smoothed count normalization
            const float countNorm = nodeCountGainSmooth_.process(invSqrtNodeCount);
            const float totalGain = swarmNodeGain * countNorm * sqrtNodeCount;
            dL *= totalGain;
            dR *= totalGain;
            blendedDistFrac = distFracSum * invNodeCountBlk;
            effectiveDistGain = distGainSum * invNodeCountBlk;
            centroidRelX *= invNodeCountBlk;
            centroidRelY *= invNodeCountBlk;
            centroidRelZ *= invNodeCountBlk;

            // GL bridge -- legacy stereo nodes + N-node swarm positions
            if (activeNodeCount >= 2) {
                lastStereoNodes_ = { worldModX + nodeOffsets[0].dx, worldModY + nodeOffsets[0].dy, worldModZ + nodeOffsets[0].dz,
                                     worldModX + nodeOffsets[1].dx, worldModY + nodeOffsets[1].dy, worldModZ + nodeOffsets[1].dz, smoothedWidth };
            } else {
                lastStereoNodes_ = { worldModX + nodeOffsets[0].dx, worldModY + nodeOffsets[0].dy, worldModZ + nodeOffsets[0].dz,
                                     worldModX + nodeOffsets[0].dx, worldModY + nodeOffsets[0].dy, worldModZ + nodeOffsets[0].dz, smoothedWidth };
            }
            lastSwarmPositions_.count = activeNodeCount;
            for (int n = 0; n < activeNodeCount; ++n) {
                lastSwarmPositions_.nodes[n] = { worldModX + nodeOffsets[n].dx,
                                                  worldModY + nodeOffsets[n].dy,
                                                  worldModZ + nodeOffsets[n].dz };
            }

            // Early reflections for N-node path
            {
                const float erLevelSm = erLevelSmooth_.process(
                    (currentParams.erEnabled && !currentParams.bypassER) ? currentParams.erLevel : 0.0f);
                const float erSendSm = erReverbSendSmooth_.process(currentParams.erReverbSend);
                const bool erNowActive = erLevelSm > 1e-6f;

                // Reset per-tap filter state on gate-open to prevent stale delay line clicks
                if (erNowActive && !erWasActive_) {
                    for (int n = 0; n < activeNodeCount; ++n)
                        nodes_[n].er.reset();
                }
                erWasActive_ = erNowActive;

                if (erNowActive) {
                    const float roomHalf = currentParams.erRoomSize;

                    if (erPerNode) {
                        // Per-node ER (N <= 4 and erShared=false)
                        for (int n = 0; n < activeNodeCount; ++n) {
                            const float nNodeDist = std::max(nodeRawDistArr[n], kMinDistance);
                            const float nNodeDistFrac = std::clamp((nNodeDist - kMinDistance) * blkInvMaxRange, 0.0f, 1.0f);
                            const float nDistGainTarget = evalDistGainCurve(blkDistCurve, nNodeDistFrac);

                            auto erResult = nodes_[n].er.processSample(nodeDopplerInputArr[n],
                                nodeRelXArr[n], nodeRelYArr[n], nodeRelZArr[n],
                                currentParams.listenerX, currentParams.listenerY, currentParams.listenerZ,
                                nDistGainTarget, sr, roomHalf,
                                ildGainBase_, listenerRotated, cY, sY, cP, sP, cR, sR,
                                currentParams);

                            dL += erResult.directL * erLevelSm;
                            dR += erResult.directR * erLevelSm;
                            erReverbAccumL += erResult.reverbL * erLevelSm * erSendSm;
                            erReverbAccumR += erResult.reverbR * erLevelSm * erSendSm;
                        }
                    } else {
                        // Shared ER at centroid position
                        const float centroidDist = std::max(dsp::fastSqrt(
                            centroidRelX*centroidRelX + centroidRelY*centroidRelY + centroidRelZ*centroidRelZ), kMinDistance);
                        const float centroidDistFrac = std::clamp((centroidDist - kMinDistance) * blkInvMaxRange, 0.0f, 1.0f);
                        const float centroidDistGainTarget = evalDistGainCurve(blkDistCurve, centroidDistFrac);

                        const float erInput = erInputSum * invNodeCountBlk;
                        auto erResult = nodes_[0].er.processSample(erInput,
                            centroidRelX, centroidRelY, centroidRelZ,
                            currentParams.listenerX, currentParams.listenerY, currentParams.listenerZ,
                            centroidDistGainTarget, sr, roomHalf,
                            ildGainBase_, listenerRotated, cY, sY, cP, sP, cR, sR,
                            currentParams);

                        dL += erResult.directL * erLevelSm;
                        dR += erResult.directR * erLevelSm;
                        erReverbAccumL = erResult.reverbL * erLevelSm * erSendSm;
                        erReverbAccumR = erResult.reverbR * erLevelSm * erSendSm;
                    }
                } else {
                    // ER inactive: push input into shared delay to keep it primed
                    if (erPerNode) {
                        for (int n = 0; n < activeNodeCount; ++n)
                            nodes_[n].er.sharedDelay.push(nodeDopplerInputArr[n]);
                    } else {
                        const float erInput = erInputSum * invNodeCountBlk;
                        nodes_[0].er.sharedDelay.push(erInput);
                    }
                }
            }
        } else {
            // Mono path — sum to mono, use node 0 pipeline
            float mono;
            if (currentParams.testToneEnabled) {
                mono = testStereo ? 0.5f * (testSigL + testSigR) : testSig;
            } else if (inputR != nullptr) {
                mono = monoBuffer[static_cast<size_t>(i)];
            } else {
                mono = inputL[i];
            }

            lastStereoNodes_ = { worldModX, worldModY, worldModZ, worldModX, worldModY, worldModZ, 0.0f };
            lastSwarmPositions_.count = 0;  // mono — no swarm nodes
            lastNodeDisplayValues_.count = 0;

            // Doppler FIRST — mono doppler before comb/pinna/binaural
            const bool effectiveDoppler = dopplerOn && !currentParams.bypassDoppler;
            mono = nodes_[0].distance.processDoppler(mono, rawDistFrac, sr, effectiveDoppler, currentParams);
            dopplerInputMono = mono;

            // Binaural processing — comb bank + pinna EQ + ITD/ILD/shadow
            {
                auto [binL, binR] = nodes_[0].binaural.processSample(
                    mono, modX, modY, modZ, sr, binBlend,
                    ildGainBase_, hardpanGainBase_, currentParams);
                dL = binL;
                dR = binR;
            }

            // DSP state capture for mono path — read back from pipeline smoothers
            {
                lastDSPState_.itdSamples     = nodes_[0].binaural.itdSmooth.current();
                lastDSPState_.shadowCutoffHz = nodes_[0].binaural.shadowCutoffSmooth.current();
                lastDSPState_.ildGainLinear  = nodes_[0].binaural.ildGainSmooth.current();
                lastDSPState_.rearCutoffHz   = nodes_[0].binaural.rearCutoffSmooth.current();
                lastDSPState_.combWet        = nodes_[0].binaural.combWetSmooth.current();
                lastDSPState_.monoBlend      = 0.0f;  // deprecated — cylinder blend removed
            }
        }

        // ----------------------------------------------------------------
        // Mono-only pipeline: chest bounce → floor bounce → distance → ER
        // (Stereo path handles these per-node inside the stereoActive block above)
        // ----------------------------------------------------------------
        if (!stereoActive) {

            // Chest bounce — driven by T/B virtual ear elevation factor
            {
                const float monoElevF = computeElevFactor(modX, modY, modZ, currentParams.elevEarOffset);
                float chestOut = nodes_[0].chest.processSample(dopplerInputMono, monoElevF, sr,
                                                       chestGainLin, currentParams);
                dL += chestOut;
                dR += chestOut;

                // Floor bounce — source-to-floor closeness override when the
                // caller provides it (LOCAL MOD), else the elevation factor.
                const float monoFloorF = (currentParams.floorBounceFactor >= 0.0f)
                    ? 1.0f - currentParams.floorBounceFactor : monoElevF;
                nodes_[0].floor.processSample(dL, dR, monoFloorF, sr, floorGainLin, currentParams);
            }

            // Distance processing — mono path only (gain + air absorption; doppler already applied)
            {
                auto distResult = nodes_[0].distance.processDistance(dL, dR, modX, modY, modZ, sr,
                    currentParams);
                dL = distResult.left;
                dR = distResult.right;
                effectiveDistGain = nodes_[0].distance.distGainSmooth.current();
            }

            // Early Reflections (Image Source Method) — mono path
            // ER input = doppler'd mono input (carries pitch shift into reflections)
            {
                const float erLevelSm = erLevelSmooth_.process(
                    (currentParams.erEnabled && !currentParams.bypassER) ? currentParams.erLevel : 0.0f);
                const float erSendSm = erReverbSendSmooth_.process(currentParams.erReverbSend);
                const bool erNowActive = erLevelSm > 1e-6f;

                if (erNowActive && !erWasActive_) {
                    nodes_[0].er.reset();
                }
                erWasActive_ = erNowActive;

                if (erNowActive) {
                    const float roomHalf = currentParams.erRoomSize;

                    const float eCY = interpRotation ? rCosY : cosY;
                    const float eSY = interpRotation ? rSinY : sinY;
                    const float eCP = interpRotation ? rCosP : cosP;
                    const float eSP = interpRotation ? rSinP : sinP;
                    const float eCR = interpRotation ? rCosR : cosR;
                    const float eSR = interpRotation ? rSinR : sinR;
                    // Per-sample interpolated pre-rotation listener-relative (ER does its own rotation)
                    const float mRelX = (interpBaseX + lfoValX) - currentParams.listenerX;
                    const float mRelY = (interpBaseY + lfoValY) - currentParams.listenerY;
                    const float mRelZ = (interpBaseZ + lfoValZ) - currentParams.listenerZ;
                    auto erResult = nodes_[0].er.processSample(dopplerInputMono,
                        mRelX, mRelY, mRelZ,
                        currentParams.listenerX, currentParams.listenerY, currentParams.listenerZ,
                        distGainTarget, sr, roomHalf,
                        ildGainBase_, listenerRotated, eCY, eSY, eCP, eSP, eCR, eSR,
                        currentParams);

                    dL += erResult.directL * erLevelSm;
                    dR += erResult.directR * erLevelSm;
                    erReverbAccumL = erResult.reverbL * erLevelSm * erSendSm;
                    erReverbAccumR = erResult.reverbR * erLevelSm * erSendSm;
                } else {
                    nodes_[0].er.sharedDelay.push(dopplerInputMono);
                }
            }
        } // end !stereoActive

        // Use blendedDistFrac (averaged from both nodes) in stereo, modDistFrac in mono
        const float effectiveDistFrac = blendedDistFrac;

        // Aux reverb send — auxMaxBoostLin pre-computed per-block (no std::pow per sample)
        if (auxL != nullptr) {
            auxPreDelayL_.push(dL + erReverbAccumL * auxERSendGainLin);
            auxPreDelayR_.push(dR + erReverbAccumR * auxERSendGainLin);
            const float auxDelaySamp = std::max(2.0f, auxDelaySmooth_.process(
                effectiveDistFrac * verbPreDelayMaxSamp));
            const float auxGainTarget = 1.0f + effectiveDistFrac * (auxMaxBoostLin - 1.0f);
            const float auxGain = auxGainSmooth_.process(auxGainTarget);
            auxL[i] = std::clamp(auxPreDelayL_.read(auxDelaySamp) * auxGain, -2.0f, 2.0f);
            auxR[i] = std::clamp(auxPreDelayR_.read(auxDelaySamp) * auxGain, -2.0f, 2.0f);
        } else {
            auxPreDelayL_.push(0.0f);
            auxPreDelayR_.push(0.0f);
            auxGainSmooth_.process(1.0f);
        }

        // Reverb — gate FDN when wet is zero (both target and smoothed) to save CPU.
        // FDN will build up naturally when reverb is re-enabled.
        {
            const float wetGain = verbWetSmooth_.process(currentParams.verbWet);
            if (wetGain > 1e-6f || currentParams.verbWet > 1e-6f) {
                const float preDelaySamp = effectiveDistFrac * verbPreDelayMaxSamp;
                float wetL, wetR;
                reverb_.processSample(dL + erReverbAccumL, dR + erReverbAccumR, preDelaySamp, wetL, wetR);
                if (!currentParams.bypassReverb) {
                    dL += wetGain * wetL;
                    dR += wetGain * wetR;
                }
            }
        }

        // Output clamp
        outL[i] = std::clamp(dL, -2.0f, 2.0f);
        outR[i] = std::clamp(dR, -2.0f, 2.0f);

        // DSP state capture (shared fields)
        lastDSPState_.sampleRate     = static_cast<float>(sampleRate);
        lastDSPState_.distDelaySamp  = nodes_[0].distance.distDelaySmooth.current();
        lastDSPState_.distGainLinear = stereoActive ? 0.0f : nodes_[0].distance.distGainSmooth.current();
        lastDSPState_.airCutoffHz    = stereoActive ? 0.0f : airCutoffMod;
        lastDSPState_.modX           = modX;

        // Capture last-sample LFO output values for UI display
        // Advance per-sample trig interpolation
        if (interpRotation) {
            rCosY += dCosY; rSinY += dSinY;
            rCosP += dCosP; rSinP += dSinP;
            rCosR += dCosR; rSinR += dSinR;
        }

        lastLfoOutX_ = lfoValX;
        lastLfoOutY_ = lfoValY;
        lastLfoOutZ_ = lfoValZ;
        lastLfoOutOrbitXY_ = orbitValXY;
        lastLfoOutOrbitXZ_ = orbitValXZ;
        lastLfoOutOrbitYZ_ = orbitValYZ;
    }
}

// ============================================================================
// setNodeBlockCoefficients() — per-block EQ setup for one node pipeline
// ============================================================================

void XYZPanEngine::setNodeBlockCoefficients(
    int nodeIdx,
    float nodeX, float nodeY, float nodeZ,
    float sr, int numSamples)
{
    auto& bin  = nodes_[nodeIdx].binaural;
    auto& dist = nodes_[nodeIdx].distance;

    // Direction factors from head-rotated node position
    const float rearFactor = computeRearFactor(nodeX, nodeY, nodeZ, currentParams.rearEarOffset);
    const float presGainDb = currentParams.presenceShelfMaxDb * (-rearFactor);
    const float earGainDb  = std::min(0.0f, currentParams.earCanalMaxDb * (-rearFactor));
    const float elevFactor = computeElevFactor(nodeX, nodeY, nodeZ, currentParams.elevEarOffset);
    const float elevAbove  = std::max(0.0f, elevFactor * 2.0f - 1.0f);
    const float pinnaGain  = -15.0f + 20.0f * elevAbove;
    const float shelfGain  = 3.0f * std::min(1.0f, elevFactor * 2.0f);
    const float n1Freq     = std::clamp(currentParams.pinnaN1MinHz + (currentParams.pinnaN1MaxHz - currentParams.pinnaN1MinHz) * elevFactor, currentParams.pinnaN1MinHz, currentParams.pinnaN1MaxHz);
    const float n2Freq     = n1Freq + currentParams.pinnaN2OffsetHz;

    // Near-field
    const float effAz      = computeAzimuthFactor(nodeX, nodeY, nodeZ, currentParams.azimuthEarOffset);
    const float fullDist   = std::sqrt(nodeX*nodeX + nodeY*nodeY + nodeZ*nodeZ);
    const float maxRange   = std::max(currentParams.sphereRadius - kMinDistance, 0.001f);
    const float nodeDistFrac = std::clamp((fullDist - kMinDistance) / maxRange, 0.0f, 1.0f);
    const float prox       = 1.0f - nodeDistFrac;
    const float nfBaseDb   = currentParams.nearFieldLFMaxDb * prox;
    const float nfGainR    = nfBaseDb * std::max(0.0f,  effAz);
    const float nfGainL    = nfBaseDb * std::max(0.0f, -effAz);

    // Air absorption cutoffs
    const float airCut1 = currentParams.airAbsMaxHz + (currentParams.airAbsMinHz - currentParams.airAbsMaxHz) * nodeDistFrac;
    const float airCut2 = currentParams.airAbs2MaxHz + (currentParams.airAbs2MinHz - currentParams.airAbs2MaxHz) * nodeDistFrac;

    // Binaural EQ — smoothed coefficients
    bin.presenceShelf.setCoefficientsSmoothed(dsp::BiquadType::HighShelf,
        currentParams.presenceShelfFreqHz, sr, 0.7071f, presGainDb, numSamples);
    bin.earCanalPeak.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
        currentParams.earCanalFreqHz, sr, currentParams.earCanalQ, earGainDb, numSamples);
    bin.pinnaNotch.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
        n1Freq, sr, currentParams.pinnaNotchQ, pinnaGain, numSamples);
    bin.pinnaNotch2.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
        n2Freq, sr, currentParams.pinnaN2Q, currentParams.pinnaN2GainDb, numSamples);
    bin.pinnaShelf.setCoefficientsSmoothed(dsp::BiquadType::HighShelf,
        currentParams.pinnaShelfFreqHz, sr, 0.7071f, shelfGain, numSamples);

    // Expanded pinna EQ (P5) — 4 additional bands
    {
        const float below = 1.0f - elevFactor;
        const float above = elevFactor;
        const float shoulderDb = currentParams.shoulderPeakMaxDb * below;
        const float conchaDb   = currentParams.conchaNotchMaxDb * below;
        const float upperDb    = currentParams.upperPinnaMinDb
            + (currentParams.upperPinnaMaxDb - currentParams.upperPinnaMinDb) * above;
        const float tragusDb   = currentParams.tragusNotchMaxDb
            * std::max(0.0f, rearFactor) * below;
        bin.shoulderPeak.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
            currentParams.shoulderPeakFreqHz, sr, currentParams.shoulderPeakQ, shoulderDb, numSamples);
        bin.conchaNotch.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
            currentParams.conchaNotchFreqHz, sr, currentParams.conchaNotchQ, conchaDb, numSamples);
        bin.upperPinna.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
            currentParams.upperPinnaFreqHz, sr, currentParams.upperPinnaQ, upperDb, numSamples);
        bin.tragusNotch.setCoefficientsSmoothed(dsp::BiquadType::PeakingEQ,
            currentParams.tragusNotchFreqHz, sr, currentParams.tragusNotchQ, tragusDb, numSamples);
    }

    // Near-field ILD biquads
    bin.nearFieldLF_R.setCoefficientsSmoothed(dsp::BiquadType::LowShelf, currentParams.nearFieldLFHz, sr, 0.7071f, nfGainR, numSamples);
    bin.nearFieldLF_L.setCoefficientsSmoothed(dsp::BiquadType::LowShelf, currentParams.nearFieldLFHz, sr, 0.7071f, nfGainL, numSamples);

    // Air absorption LPFs
    dist.airLPF_L.setCoefficientsSmoothed(airCut1, sr, numSamples);
    dist.airLPF_R.setCoefficientsSmoothed(airCut1, sr, numSamples);
    dist.airLPF2_L.setCoefficientsSmoothed(airCut2, sr, numSamples);
    dist.airLPF2_R.setCoefficientsSmoothed(airCut2, sr, numSamples);
}

// ============================================================================
// processNodeSample() — per-sample DSP for one node
// ============================================================================

XYZPanEngine::NodeSampleResult XYZPanEngine::processNodeSample(
    int nodeIdx,
    float dopplerInput,
    float relX, float relY, float relZ,
    float dspX, float dspY, float dspZ,
    float sr, float binBlend,
    float chestGainLin, float floorGainLin,
    const EngineParams& params)
{
    auto& bin   = nodes_[nodeIdx].binaural;
    auto& dist  = nodes_[nodeIdx].distance;
    auto& chest = nodes_[nodeIdx].chest;
    auto& flr   = nodes_[nodeIdx].floor;

    // Chest bounce — driven by T/B virtual ear elevation factor
    const float elevF = computeElevFactor(dspX, dspY, dspZ, params.elevEarOffset);
    float chestOut = chest.processSample(dopplerInput, elevF, sr, chestGainLin, params);

    // Binaural processing — comb bank + pinna EQ + ITD/ILD/shadow
    auto [binL, binR] = bin.processSample(
        dopplerInput, dspX, dspY, dspZ, sr, binBlend,
        ildGainBase_, hardpanGainBase_, params);

    // Add chest to both ears (before distance)
    binL += chestOut;
    binR += chestOut;

    // Distance processing (gain + air absorption; doppler already applied)
    auto distResult = dist.processDistance(binL, binR, relX, relY, relZ, sr,
        params);

    // Floor bounce (on distance output) — closeness override when provided
    // (LOCAL MOD), else the elevation factor.
    const float floorF = (params.floorBounceFactor >= 0.0f)
        ? 1.0f - params.floorBounceFactor : elevF;
    float fL = distResult.left, fR = distResult.right;
    flr.processSample(fL, fR, floorF, sr, floorGainLin, params);

    return { fL, fR, distResult.distFrac, dist.distGainSmooth.current() };
}

// ============================================================================
// reset()
// ============================================================================

void XYZPanEngine::reset() {
    // Reset all N node pipelines
    for (int n = 0; n < kMaxSwarmNodes; ++n) {
        nodes_[n].delayOffset.reset();
        nodes_[n].delayOffsetSmooth.reset(0.0f);
        nodes_[n].pitchShifter.reset();
        nodes_[n].binaural.reset();
        nodes_[n].chest.reset();
        nodes_[n].floor.reset();
        nodes_[n].distance.reset();
        nodes_[n].er.reset();
    }

    // Reset tracking members so the next process() block re-evaluates them.
    lastSmoothMs_ITD_    = kDefaultSmoothMs_ITD;
    lastSmoothMs_Filter_ = kDefaultSmoothMs_Filter;
    lastSmoothMs_Gain_   = kDefaultSmoothMs_Gain;

    // Aux reverb send
    auxPreDelayL_.reset();
    auxPreDelayR_.reset();
    auxGainSmooth_.reset(1.0f);
    auxDelaySmooth_.reset(0.0f);
    // Phase 5: reverb
    reverb_.reset();
    verbWetSmooth_.reset(kVerbDefaultWet);

    // Phase 5: LFO
    lfoX_.reset(currentParams.lfoXPhase);
    lfoY_.reset(currentParams.lfoYPhase);
    lfoZ_.reset(currentParams.lfoZPhase);
    lfoDepthXSmooth_.reset(0.0f);
    lfoDepthYSmooth_.reset(0.0f);
    lfoDepthZSmooth_.reset(0.0f);
    lfoDepthMulSmooth_.reset(1.0f);
    blkPosXSmooth_.reset(currentParams.x);
    blkPosYSmooth_.reset(currentParams.y);
    blkPosZSmooth_.reset(currentParams.z);
    prevSmoothBaseX_ = currentParams.x;
    prevSmoothBaseY_ = currentParams.y;
    prevSmoothBaseZ_ = currentParams.z;

    // Stereo orbit LFOs
    orbitLfoXY_.reset(currentParams.stereoOrbitXYPhase);
    orbitLfoXZ_.reset(currentParams.stereoOrbitXZPhase);
    orbitLfoYZ_.reset(currentParams.stereoOrbitYZPhase);
    orbitDepthXYSmooth_.reset(0.0f);
    orbitDepthXZSmooth_.reset(0.0f);
    orbitDepthYZSmooth_.reset(0.0f);
    orbitDepthMulSmooth_.reset(1.0f);
    stereoWidthSmooth_.reset(0.0f);

    // Angular smoothers for circular phase/offset
    phaseSmCos_ = 1.0f; phaseSmSin_ = 0.0f;
    offsetSmCos_ = 1.0f; offsetSmSin_ = 0.0f;

    // Previous block trig for per-sample interpolation
    prevCosY_ = 1.f; prevSinY_ = 0.f;
    prevCosP_ = 1.f; prevSinP_ = 0.f;
    prevCosR_ = 1.f; prevSinR_ = 0.f;

    // Early Reflections (shared smoothers)
    erLevelSmooth_.reset(0.0f);
    erReverbSendSmooth_.reset(kERReverbSendDefault);

    // Binaural toggle smoother
    binauralBlendSmooth_.reset(1.0f);

    // LFO output values for UI display
    lastLfoOutX_ = 0.f; lastLfoOutY_ = 0.f; lastLfoOutZ_ = 0.f;
    lastLfoOutOrbitXY_ = 0.f; lastLfoOutOrbitXZ_ = 0.f; lastLfoOutOrbitYZ_ = 0.f;

    // Dev tool: test tone oscillator
    sawPhase_ = 0.0f;
    clickSamplesLeft_ = 0.0f;
    prevPulseGate_ = false;
    pulseLFO_.reset(0.0f);
    testToneGainSmooth_.reset(0.0f);
}

XYZPanEngine::LFOOutputs XYZPanEngine::getLastLFOOutputs() const noexcept {
    return { lastLfoOutX_, lastLfoOutY_, lastLfoOutZ_,
             lastLfoOutOrbitXY_, lastLfoOutOrbitXZ_, lastLfoOutOrbitYZ_ };
}

} // namespace xyzpan
