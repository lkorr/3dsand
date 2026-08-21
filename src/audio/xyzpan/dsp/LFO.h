#pragma once
#include <cstdint>

namespace xyzpan::dsp {

// Saw and RampDown are adjacent (2,3) for UI grouping.
enum class LFOWaveform { Sine = 0, Triangle, Saw, RampDown, Square, SampleHold };
inline constexpr int kLFOWaveformCount = 6;

// Phase accumulator LFO with 6 waveforms + optional output smoothing.
// Output range: [-1, 1]. Use setRateHz() + tick() pattern.
// Phase offset via reset(phaseOffsetNorm) before first tick.
class LFO {
public:
    LFOWaveform waveform = LFOWaveform::Sine;

    // Call once before processing; sets internal sample rate.
    // Does NOT reset accumulator (allows live sample rate changes without phase jump).
    void prepare(double sampleRate);

    // Set accumulator to phaseOffsetNorm, wrapped to [0, 1).
    // Call before first tick() to set initial phase.
    void reset(float phaseOffsetNorm = 0.0f);

    // Set the LFO rate in Hz. Must be called after prepare().
    void setRateHz(float hz);

    // Store a phase offset (normalized [0,1]) added to the accumulator each tick.
    void setPhaseOffset(float offset);

    // Set output smoothing time in milliseconds (one-pole lowpass).
    // 0 = no smoothing (bypass). Must be called after prepare().
    void setSmoothMs(float ms);

    // Request a phase reset on the next tick().
    void requestReset();

    // Return current output in [-1, 1] without advancing the accumulator.
    float peek() const;

    // Evaluate the waveform at an arbitrary phase [0,1) without touching state.
    // Returns raw waveform value in [-1, 1] (no smoothing, no S&H).
    float peekAtPhase(float phase) const;

    // Return raw phase accumulator value in [0, 1).
    float getPhase() const noexcept { return accumulator_; }

    // Return phase increment per sample (rate / sampleRate).
    float getIncrement() const noexcept { return increment_; }

    // Return last S&H held value in [-1, 1] (for accurate UI display).
    float getHeldValue() const noexcept { return shHeldValue_; }

    // Set accumulator absolutely from transport position (for grid-synced LFOs).
    // cycleNumber seeds S&H deterministically so the same cycle always holds the same value.
    void setPhaseFromPosition(float phase, int64_t cycleNumber);

    // Advance accumulator and return output in [-1, 1].
    float tick();

private:
    float  accumulator_  = 0.0f;
    float  increment_    = 0.0f;
    double sampleRate_   = 44100.0;
    float  phaseOffset_  = 0.0f;
    bool   resetPending_ = false;

    // Sample & Hold state
    float    shHeldValue_ = 0.0f;
    float    shPrevPhase_ = 0.0f;
    uint32_t shState_     = 123456789u;  // xorshift32 RNG seed

    // Output smoother (one-pole lowpass)
    float smoothMs_     = 0.0f;
    float smoothCoeffA_ = 0.0f;  // feedback coefficient
    float smoothCoeffB_ = 1.0f;  // input coefficient (1 - A)
    float smoothZ_      = 0.0f;  // filter state

    // Lightweight xorshift32 RNG, returns value in [-1, 1]
    float xorshift32();
};

} // namespace xyzpan::dsp
