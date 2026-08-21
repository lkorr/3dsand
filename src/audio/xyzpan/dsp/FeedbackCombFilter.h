#pragma once
// FeedbackCombFilter.h
// IIR feedback comb filter: y[n] = x[n] + g * y[n - M]
//
// Creates a series of exponentially decaying echoes at multiples of the delay
// period M. Used in a series bank to model front/back depth perception via
// spectral coloration of the mono input signal (DEPTH-01, DEPTH-02).
//
// Design:
//   - Power-of-2 ring buffer with bitmask wraparound (never modulo).
//   - Integer delay only — comb filters do not require fractional interpolation.
//   - prepare() allocates the buffer. Zero allocation in process/reset after that.
//   - setFeedback() HARD CLAMPS to [-0.95, 0.95] (DEPTH-04). At g=0.95 the filter
//     decays slowly but remains stable. At g>=1.0 it diverges — the class prevents
//     that regardless of caller input.
//
// Source: Julius O. Smith, Physical Audio Signal Processing
//   ccrma.stanford.edu/~jos/pasp/Feedback_Comb_Filters.html

#include <vector>
#include <algorithm>

namespace xyzpan::dsp {

class FeedbackCombFilter {
public:
    // Allocate ring buffer with at least capacitySamples usable delay.
    // Internal size is next power-of-2 >= capacitySamples + 2.
    void prepare(int capacitySamples) {
        int n = 1;
        while (n < capacitySamples + 2) n <<= 1;
        mask_             = n - 1;
        buf_.assign(static_cast<size_t>(n), 0.0f);
        writePos_         = 0;
        delayInSamples_   = 1;
        feedback_         = 0.0f;
    }

    // Zero all samples and reset write position.
    void reset() {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        writePos_ = 0;
    }

    // Set delay in samples (integer).
    // Clamped to [1, bufferSize - 2] to ensure valid ring-buffer read.
    void setDelay(int delaySamples) {
        delayInSamples_ = std::max(1, std::min(delaySamples,
                                               static_cast<int>(buf_.size()) - 2));
    }

    // Set feedback gain.
    // HARD CLAMPED to [-0.95, 0.95] — stability invariant, not caller's responsibility.
    // At g >= 1.0 the IIR feedback diverges to infinity; the clamp enforces |g| < 1.
    void setFeedback(float g) {
        feedback_ = std::clamp(g, -0.95f, 0.95f);
    }

    // Process one sample: y[n] = x[n] + feedback * y[n - M]
    float process(float x) {
        int readPos = (writePos_ - delayInSamples_) & mask_;
        float y = x + feedback_ * buf_[static_cast<size_t>(readPos)];
        buf_[static_cast<size_t>(writePos_ & mask_)] = y;
        ++writePos_;
        return y;
    }

private:
    std::vector<float> buf_;
    int   mask_           = 0;
    int   writePos_       = 0;
    int   delayInSamples_ = 1;
    float feedback_       = 0.0f;
};

} // namespace xyzpan::dsp
