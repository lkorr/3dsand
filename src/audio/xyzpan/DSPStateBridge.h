#pragma once
#include <atomic>

namespace xyzpan {

// Live DSP telemetry snapshot — written by audio thread at end of process(),
// read by PositionBridge for dev panel display (UI-07).
struct DSPStateSnapshot {
    float itdSamples     = 0.0f;
    float shadowCutoffHz = 0.0f;
    float ildGainLinear  = 0.0f;
    float rearCutoffHz   = 0.0f;
    float combWet        = 0.0f;
    float monoBlend      = 0.0f;  // deprecated: always 0, cylinder blend removed
    float sampleRate     = 0.0f;
    float distDelaySamp  = 0.0f;
    float distGainLinear = 0.0f;
    float airCutoffHz    = 0.0f;
    float modX           = 0.0f;
};

// Lock-free double-buffer for audio->UI DSP state transfer.
// Same pattern as PositionBridge: one writer (audio thread), one reader (UI timer).
class DSPStateBridge {
public:
    void write(const DSPStateSnapshot& state) {
        const int idx = 1 - writeIdx_.load(std::memory_order_relaxed);
        buf_[idx] = state;
        writeIdx_.store(idx, std::memory_order_release);
    }

    [[nodiscard]] DSPStateSnapshot read() const {
        return buf_[writeIdx_.load(std::memory_order_acquire)];
    }

private:
    DSPStateSnapshot buf_[2];
    std::atomic<int> writeIdx_{0};
};

} // namespace xyzpan
