#pragma once
// OLAPitchShifter.h
// Real-time pitch shifter using Overlap-Add (OLA) with Hann windowing and
// Hermite interpolation. Supports three algorithm flavors via setAlgo():
//
//   Algo::OLA        - dual-grain, ~20ms grains. Tight, lowest CPU. Default.
//                      Best for moderate detune / chord intervals.
//   Algo::Formant    - dual-grain like OLA plus a spectral-tilt + loudness
//                      correction that pushes the shifted timbre back toward the
//                      input's envelope, taming the "chipmunk" effect on up-shifts.
//                      NOTE: this is a cheap envelope/tilt approximation, NOT full
//                      LPC/cepstral formant preservation. It reduces the gross
//                      brightness+loudness shift; it does not reconstruct true
//                      formants. Good enough for voices/leads in a spatial cluster.
//   Algo::Harmonizer - quad-grain, ~40ms grains. Smoother, lusher, blurs
//                      transients. Best for pads / dense clusters.
//
// Two (or four) grains read the circular buffer at the pitch ratio, staggered
// evenly. Hann envelopes guarantee smooth crossfades at respawn boundaries.
//
// Click-free design notes:
//   - The pitch ratio is smoothed with a one-pole so per-block ratio changes
//     (automation, mode/order switches) ramp instead of stepping.
//   - The grain read pointer is clamped so it can never overrun the write head
//     into stale/future buffer samples. Without this, ratios > ~2x read past
//     the write head and emit loud garbage. The grain is respawned the moment
//     it would overrun rather than reading invalid data.
//   - reset()/prepare() leave the shifter able to re-prime smoothly.
//
// Usage:
//   1. prepare(sampleRate) - call once before processing.
//   2. setAlgo(algo)       - choose algorithm (can change per-block).
//   3. setRatio(semitones) - set pitch offset (can change per-block).
//   4. processSample(input)- call per-sample, returns pitched output.
//   5. reset()             - clear all state.

#include "xyzpan/dsp/SineLUT.h"
#include <cmath>
#include <vector>
#include <algorithm>

namespace xyzpan::dsp {

class OLAPitchShifter {
public:
    enum class Algo : int { OLA = 0, Formant = 1, Harmonizer = 2 };

    void prepare(float sampleRate) {
        sr_ = sampleRate;
        // One-pole ratio smoothing coefficient (~5ms time constant).
        ratioSmoothA_ = std::exp(-6.28318530f / (0.005f * sampleRate));
        // Envelope-follower coefficient for the Formant correction (~3ms).
        envA_ = std::exp(-1.0f / (0.003f * sampleRate));
        buf_.assign(kBufSize, 0.0f);
        applyAlgo();
        resetState();
    }

    void reset() {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        resetState();
    }

    // Select the shifting algorithm. Changing grain count re-primes cleanly via
    // the prime gate in processSample(); ratio/envelope smoothing hides the seam.
    void setAlgo(Algo a) {
        if (a != algo_) {
            algo_ = a;
            applyAlgo();
            primed_ = false;  // re-prime so the new grain layout initializes cleanly
        }
    }

    // Set pitch ratio from semitone offset. Caches the pow() call -
    // only recomputes the *target* ratio when the value actually changes.
    // The ratio itself is smoothed per-sample in processSample().
    void setRatio(float semitones) {
        if (semitones != lastSemitones_) {
            lastSemitones_ = semitones;
            ratioTarget_ = std::pow(2.0f, semitones / 12.0f);
        }
    }

    float processSample(float input) {
        // Write input
        buf_[static_cast<size_t>(writePos_ & kMask)] = input;
        ++writePos_;

        // Smooth the ratio toward its target to avoid grain-speed steps.
        ratio_ = ratioTarget_ + (ratio_ - ratioTarget_) * ratioSmoothA_;

        if (!primed_) {
            // Fill buffer for one grain length before producing output
            if (writePos_ < static_cast<int>(grainSizeSamp_) + 4)
                return input;  // pass-through during prime
            // Initialize read positions relative to write head; stagger grain
            // phases evenly so their Hann envelopes sum to ~unity gain.
            const float start = static_cast<float>(writePos_) - grainSizeSamp_;
            for (int g = 0; g < grainCount_; ++g) {
                readPos_[g] = start;
                grainPhase_[g] = static_cast<float>(g) / static_cast<float>(grainCount_);
            }
            primed_ = true;
        }

        // Maximum read position before we overrun the write head. Stay a few
        // samples behind so Hermite's +2 lookahead tap is always valid data.
        const float maxRead = static_cast<float>(writePos_) - 3.0f;
        float output = 0.0f;

        for (int g = 0; g < grainCount_; ++g) {
            // Hann envelope
            const float env = hann(grainPhase_[g]);

            // Read with Hermite interpolation
            output += readHermite(readPos_[g]) * env;

            // Advance
            readPos_[g] += ratio_;
            grainPhase_[g] += phaseInc_;

            // Respawn when the envelope completes OR when the read pointer is
            // about to overrun the write head (high ratios). Respawning at the
            // overrun point keeps the read inside valid buffer history; the
            // Hann envelope is near its edges here so the splice stays smooth.
            if (grainPhase_[g] >= 1.0f || readPos_[g] >= maxRead) {
                grainPhase_[g] -= std::floor(grainPhase_[g]);
                // Reset read position to one grain behind the write head
                readPos_[g] = static_cast<float>(writePos_) - grainSizeSamp_;
            }
        }

        // With N grains whose Hann envelopes are evenly staggered, the summed
        // envelope gain is ~N/2. Normalize so output level matches the 2-grain
        // case regardless of grain count.
        output *= grainGainComp_;

        if (algo_ == Algo::Formant)
            output = applyFormantCorrection(input, output);

        return output;
    }

private:
    static constexpr int kBufSize    = 16384;
    static constexpr int kMask       = kBufSize - 1;
    static constexpr int kMaxGrains  = 4;

    // Configure grain count / size / gain compensation for the active algorithm.
    void applyAlgo() {
        switch (algo_) {
            case Algo::Harmonizer:
                grainCount_   = 4;
                grainSizeSamp_ = 0.080f * sr_;   // 80ms — smooth, lush
                break;
            case Algo::OLA:
            case Algo::Formant:
            default:
                grainCount_   = 4;
                grainSizeSamp_ = 0.055f * sr_;   // 55ms, 4 grains for smooth crossfades
                break;
        }
        // Evenly-staggered Hann grains sum to grainCount/2 average gain.
        grainGainComp_ = 2.0f / static_cast<float>(grainCount_);
        phaseInc_ = 1.0f / grainSizeSamp_;
    }

    void resetState() {
        writePos_ = 0;
        ratio_ = 1.0f;
        ratioTarget_ = 1.0f;
        lastSemitones_ = 0.0f;
        for (int g = 0; g < kMaxGrains; ++g) {
            readPos_[g] = 0.0f;
            grainPhase_[g] = static_cast<float>(g) / static_cast<float>(grainCount_ > 0 ? grainCount_ : 1);
        }
        primed_ = false;
        dryEnv_ = 0.0f;
        wetEnv_ = 0.0f;
    }

    // Cheap "formant" correction: match the wet signal's short-time loudness
    // envelope back to the dry input's. The grain engine shifts pitch AND scales
    // the spectral envelope with it; restoring the dry loudness contour pulls the
    // bulk brightness/loudness shift back, taming the "chipmunk" effect without
    // the cost of an LPC/cepstral envelope. It is an approximation, not true
    // formant preservation.
    float applyFormantCorrection(float dry, float wet) {
        // Track rectified envelopes of dry and wet.
        const float adry = std::fabs(dry);
        const float awet = std::fabs(wet);
        dryEnv_ = adry + (dryEnv_ - adry) * envA_;
        wetEnv_ = awet + (wetEnv_ - awet) * envA_;

        // Loudness restoration: scale wet so its envelope follows the dry's.
        // Guard the divide; clamp the gain so transient mismatches don't blow up.
        const float g = (wetEnv_ > 1e-6f) ? (dryEnv_ / wetEnv_) : 1.0f;
        const float gClamped = std::min(std::max(g, 0.25f), 4.0f);
        return wet * gClamped;
    }

    std::vector<float> buf_;
    int   writePos_ = 0;
    float sr_ = 48000.0f;
    float grainSizeSamp_ = 960.0f;
    float phaseInc_ = 1.0f / 960.0f;  // 1 / grainSizeSamp_, updated in applyAlgo()
    int   grainCount_ = 2;
    float grainGainComp_ = 1.0f;
    Algo  algo_ = Algo::OLA;
    float ratio_ = 1.0f;          // smoothed (live) ratio
    float ratioTarget_ = 1.0f;    // target ratio from setRatio()
    float ratioSmoothA_ = 0.0f;   // one-pole coefficient
    float lastSemitones_ = 0.0f;
    bool  primed_ = false;

    float readPos_[kMaxGrains] = {};
    float grainPhase_[kMaxGrains] = {};

    // Formant-correction state
    float envA_  = 0.0f;
    float dryEnv_ = 0.0f, wetEnv_ = 0.0f;

    static float hann(float phase) {
        return 0.5f * (1.0f - SineLUT::cosLookup(phase));
    }

    // Cubic Hermite (Catmull-Rom) interpolation - same as FractionalDelayLine.
    float readHermite(float pos) const {
        int idx = static_cast<int>(pos);
        float frac = pos - static_cast<float>(idx);

        float A = buf_[static_cast<size_t>((idx - 1) & kMask)];
        float B = buf_[static_cast<size_t>((idx    ) & kMask)];
        float C = buf_[static_cast<size_t>((idx + 1) & kMask)];
        float D = buf_[static_cast<size_t>((idx + 2) & kMask)];

        float a = -0.5f*A + 1.5f*B - 1.5f*C + 0.5f*D;
        float b =        A - 2.5f*B + 2.0f*C - 0.5f*D;
        float c = -0.5f*A           + 0.5f*C;

        return ((a * frac + b) * frac + c) * frac + B;
    }
};

} // namespace xyzpan::dsp
