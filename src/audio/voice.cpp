#include "audio/voice.h"

#include <algorithm>
#include <cmath>

namespace audio {

// Post-buffer flush for a one-shot: the sample data has run out but the
// engine's doppler/ITD delay lines and reverb still hold tail. 4096 samples is
// ~85 ms at 48 kHz, comfortably past the longest delay the engine can hold at
// the ranges this game uses.
constexpr int kTailFlushSamples = 4096;

void Voice::Prepare(double sampleRate, int maxBlockSize,
                    const xyzpan::EngineParams& initialParams) {
  sampleRate_ = sampleRate;
  engine_.prepare(sampleRate, maxBlockSize, initialParams);
  params.Write(initialParams);
}

bool Voice::TriggerLoop(const std::vector<float>* buf, size_t startOffset) {
  if (buf == nullptr || buf->empty()) return false;
  if (state_.load(std::memory_order_acquire) != (int)VoiceState::Free) return false;
  isOneShot = false;
  pendingBuffer_ = buf;
  pendingGain_ = 1.0f;
  pendingRate_ = 1.0;
  pendingOffset_ = startOffset;
  state_.store((int)VoiceState::Triggered, std::memory_order_release);
  return true;
}

bool Voice::Trigger(const std::vector<float>* buf, float gain, double rate) {
  if (buf == nullptr || buf->empty()) return false;
  if (state_.load(std::memory_order_acquire) != (int)VoiceState::Free) return false;
  isOneShot = true;
  triggerGain = gain;
  pendingBuffer_ = buf;
  pendingGain_ = gain;
  pendingRate_ = rate;
  pendingOffset_ = 0;
  state_.store((int)VoiceState::Triggered, std::memory_order_release);
  return true;
}

void Voice::Retrigger(const std::vector<float>* buf, float gain, double rate) {
  if (buf == nullptr || buf->empty()) return;
  isOneShot = true;
  triggerGain = gain;
  pendingBuffer_ = buf;
  pendingGain_ = gain;
  pendingRate_ = rate;
  pendingOffset_ = 0;
  // No Free check: if the voice is live, RenderAdd sees Triggered while
  // engaged_ is still true and spends one block fading the old note out.
  state_.store((int)VoiceState::Triggered, std::memory_order_release);
}

void Voice::RenderAdd(float* mixL, float* mixR, float* scratchIn, float* scratchL,
                      float* scratchR, int n, float mixGain) {
  const int st = state_.load(std::memory_order_acquire);
  if (st == (int)VoiceState::Free) return;

  bool stopping = (st == (int)VoiceState::StopRequested);
  bool adopted = false;
  bool refire = false;

  if (st == (int)VoiceState::Triggered) {
    if (engaged_) {
      // A live note is being retriggered. Fade the old one out across this
      // block and leave state_ at Triggered; the next block adopts cleanly.
      // Resetting the playhead in place would land mid-waveform and click.
      refire = true;
      stopping = true;
    } else {
      adopted = true;
      engaged_ = true;
      buffer_ = pendingBuffer_;
      voiceGain_ = pendingGain_;
      rate_ = pendingRate_;
      if (isOneShot) {
        playhead_ = 0.0;
        tailLeft_ = kTailFlushSamples;
      } else {
        playhead_ = (buffer_ != nullptr && !buffer_->empty())
                        ? (double)(pendingOffset_ % buffer_->size())
                        : 0.0;
      }
      // Snap occlusion to the trigger-time targets so a sound born behind a
      // wall starts muffled rather than sweeping shut over the smoothing time.
      occlCutoff_ = occlCutoffTarget_.load(std::memory_order_relaxed);
      occlGain_ = occlGainTarget_.load(std::memory_order_relaxed);
      occlZ1_ = occlZ2_ = 0.0f;
      state_.store((int)VoiceState::Playing, std::memory_order_relaxed);
    }
  }

  if (buffer_ == nullptr || buffer_->empty()) return;

  xyzpan::EngineParams p;
  params.Read(p);
  if (adopted) {
    // A newly adopted note is a fresh event at its own place: snap the
    // engine's ~150 ms position smoothing there instead of dragging the attack
    // across the world from wherever this voice last played.
    engine_.snapPosition(p.x, p.y, p.z);
  }
  engine_.setParams(p);

  const std::vector<float>& buf = *buffer_;
  const size_t size = buf.size();
  const float g = emitGain * voiceGain_;
  bool finished = false;

  if (!isOneShot) {
    size_t pos = (size_t)playhead_;
    for (int i = 0; i < n; i++) {
      scratchIn[i] = buf[pos] * g;
      if (++pos >= size) pos = 0;
    }
    playhead_ = (double)pos;
  } else {
    // Linear-interpolated fractional rate (pitch variation per trigger).
    double pos = playhead_;
    for (int i = 0; i < n; i++) {
      if (pos < (double)(size - 1)) {
        const size_t i0 = (size_t)pos;
        const float frac = (float)(pos - (double)i0);
        scratchIn[i] = (buf[i0] * (1.0f - frac) + buf[i0 + 1] * frac) * g;
        pos += rate_;
      } else {
        scratchIn[i] = 0.0f;
        if (--tailLeft_ <= 0) finished = true;
      }
    }
    playhead_ = pos;
  }

  if (stopping) {
    // Linear fade across the final block -- click-free stop.
    const float inc = 1.0f / (float)n;
    for (int i = 0; i < n; i++) scratchIn[i] *= 1.0f - inc * (float)(i + 1);
    finished = true;
  }

  ApplyOcclusion(scratchIn, n);

  const float* inputs[1] = {scratchIn};
  engine_.process(inputs, 1, scratchL, scratchR, nullptr, nullptr, n);

  for (int i = 0; i < n; i++) {
    mixL[i] += scratchL[i] * mixGain;
    mixR[i] += scratchR[i] * mixGain;
  }

  if (finished) {
    if (refire) {
      // Old note faded; state_ is still Triggered, so the next block adopts
      // pending* as a fresh, position-snapped note.
      engaged_ = false;
    } else {
      if (stopping) buffer_ = nullptr;
      engaged_ = false;
      state_.store((int)VoiceState::Free, std::memory_order_release);
    }
  }
}

void Voice::ApplyOcclusion(float* x, int n) {
  const float cutoffTarget = occlCutoffTarget_.load(std::memory_order_relaxed);
  const float gainTarget = occlGainTarget_.load(std::memory_order_relaxed);

  // Block-rate smoothing. Cutoff moves in LOG frequency so a sweep from 20 kHz
  // to 400 Hz is perceptually even; gain moves linearly.
  const float tau = std::max(1.0f, occlSmoothMs_.load(std::memory_order_relaxed)) * 0.001f;
  const float a = 1.0f - std::exp(-(float)n / (tau * (float)sampleRate_));
  occlCutoff_ *= std::pow(cutoffTarget / occlCutoff_, a);
  const float gainPrev = occlGain_;
  occlGain_ += a * (gainTarget - occlGain_);

  if (occlCutoff_ >= 19500.0f && occlGain_ >= 0.999f) {
    // Open: bypass. The filter is transparent up here anyway, and zeroed state
    // cannot click when occlusion re-engages.
    occlZ1_ = occlZ2_ = 0.0f;
    return;
  }

  // RBJ cookbook low-pass at Butterworth Q. Coefficients once per block; the
  // smoothed cutoff keeps the block-to-block step small enough to be inaudible.
  const float fc = std::min(occlCutoff_, 0.45f * (float)sampleRate_);
  const float w0 = 2.0f * 3.14159265f * fc / (float)sampleRate_;
  const float cw = std::cos(w0), sw = std::sin(w0);
  const float alpha = sw / (2.0f * 0.70710678f);
  const float a0inv = 1.0f / (1.0f + alpha);
  const float b1 = (1.0f - cw) * a0inv;
  const float b0 = 0.5f * b1, b2 = b0;
  const float a1 = -2.0f * cw * a0inv;
  const float a2 = (1.0f - alpha) * a0inv;

  // Gain ramps across the block so a fast-changing solve cannot zipper.
  float gg = gainPrev;
  const float gInc = (occlGain_ - gainPrev) / (float)n;
  float z1 = occlZ1_, z2 = occlZ2_;
  for (int i = 0; i < n; i++) {
    const float in = x[i];
    const float out = b0 * in + z1;
    z1 = b1 * in - a1 * out + z2;
    z2 = b2 * in - a2 * out;
    gg += gInc;
    x[i] = out * gg;
  }
  occlZ1_ = z1;
  occlZ2_ = z2;
}

}  // namespace audio
