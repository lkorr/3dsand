#pragma once
// One sound emitter: a full XYZPanEngine instance, a mono sample buffer, and a
// lock-free parameter handoff between the game thread and the audio thread.
//
// THE THREADING CONTRACT. This is the first real concurrency in sandvox --
// everything else (including the GPU readback, which lands via ProcessEvents on
// the main thread) is single-threaded. So the boundary is stated here once and
// enforced everywhere:
//
//   GAME THREAD owns  World, Player, PlayerAvatar, Tuning, AudioLibrary.
//   AUDIO THREAD owns playback position, filter state, engine smoothers.
//   They share ONLY: this class's atomics, and sample buffers that the library
//                    guarantees are immortal (see library.h).
//
// The audio thread must never touch a game object. In particular it must never
// call CurrentTuning(): F5 replaces that global wholesale (sim/tuning.cpp), so
// reading it off-thread is a use-after-free waiting to happen. Tuning values
// reach a voice only by being copied into params/atomics on the game thread.
//
//   Free --Trigger()/TriggerLoop()--> Triggered --(audio adopts)--> Playing
//   Playing --RequestStop()--> StopRequested --(audio clears)--> Free
//   Playing (one-shot, buffer + tail done) --(audio)--> Free
//
// The game thread writes the pending* fields before a RELEASE store to
// Triggered; the audio thread's ACQUIRE load makes them visible. A Free voice
// is never processed, so its engine smoothers freeze rather than drifting.
//
// Adapted from audio_webgame's game/src/Source.h. The differences are sandvox's
// Vec3 in place of glm, and the layered-loop path, which existed for that
// game's wind rig and has no client here.

#include <atomic>
#include <cstddef>
#include <vector>

#include "audio/xyzpan/Engine.h"
#include "math3d.h"

namespace audio {

enum class VoiceState : int { Free = 0, Triggered = 1, Playing = 2, StopRequested = 3 };

// Lock-free single-writer/single-reader handoff of EngineParams. Double buffer
// plus an atomic index flip: the writer fills the inactive slot then publishes
// it, the reader copies the active slot once per block.
//
// Accepted limitation: if the writer flips twice while the reader is mid-copy,
// the reader can observe a torn snapshot. At one write per frame against a
// sub-microsecond copy this is vanishingly rare, and the cost is one block of
// slightly inconsistent position -- which the engine's own smoothers absorb.
class ParamBuffer {
 public:
  void Write(const xyzpan::EngineParams& p) {  // game thread
    const int next = 1 - active_.load(std::memory_order_acquire);
    slots_[next] = p;
    active_.store(next, std::memory_order_release);
  }
  void Read(xyzpan::EngineParams& out) const {  // audio thread
    out = slots_[active_.load(std::memory_order_acquire)];
  }

 private:
  xyzpan::EngineParams slots_[2];
  std::atomic<int> active_{0};
};

class Voice {
 public:
  // ---- game-thread config; write before the release-store that activates ----
  Vec3 worldPos{};            // emitter position, METERS (not voxels)
  float emitGain = 1.0f;      // pre-engine input gain
  float audibleRadius = 35.0f;  // -> engine sphereRadius, meters
  float verbWet = 0.0f;
  bool dopplerEnabled = true;
  float dopplerScale = 1.0f;
  // How fully point-source occlusion applies. < 1 for spatially extended
  // sources (a wide lava lake is never fully shadowed by one pillar).
  float occlScale = 1.0f;
  float triggerGain = 1.0f;  // game-thread mirror of the trigger gain, for ranking
  bool isOneShot = false;

  ParamBuffer params;

  void Prepare(double sampleRate, int maxBlockSize,
               const xyzpan::EngineParams& initialParams);

  // Loop activation. The caller must already have written worldPos/emitGain/
  // verbWet and fresh params. False if the voice is not Free.
  bool TriggerLoop(const std::vector<float>* buf, size_t startOffset);

  // One-shot activation. `rate` is the playback rate (pitch): 1.0 is natural.
  bool Trigger(const std::vector<float>* buf, float gain, double rate);

  // Like Trigger but restarts even while Playing: the audio thread fades the
  // old note across one block, then adopts with the engine position SNAPPED to
  // the new note's location. Without the snap the new attack audibly sweeps
  // across the world from wherever this voice last played. Only safe for
  // voices with a single game-thread owner (the steal path in AudioWorld).
  void Retrigger(const std::vector<float>* buf, float gain, double rate);

  // Occlusion targets, game thread. The audio thread smooths toward these and
  // applies a low-pass + gain to the voice input BEFORE the engine, so the
  // muffling ducks the reverb send too; AudioWorld::MakeParams compensates
  // verbWet with the solve's separate wetGain (occlusion vs obstruction).
  void SetOcclusion(float cutoffHz, float gain, float smoothMs) {
    occlCutoffTarget_.store(cutoffHz, std::memory_order_relaxed);
    occlGainTarget_.store(gain, std::memory_order_relaxed);
    occlSmoothMs_.store(smoothMs, std::memory_order_relaxed);
  }

  // Ask the audio thread to fade out and free this voice next block. Safe in
  // any state; a Triggered-but-unadopted voice is simply cancelled.
  void RequestStop() {
    state_.store((int)VoiceState::StopRequested, std::memory_order_release);
  }

  bool Active() const {
    return state_.load(std::memory_order_relaxed) != (int)VoiceState::Free;
  }

  // Audio thread: render n samples and accumulate into mix with mixGain.
  // scratch* are caller-owned blocks of >= n floats.
  void RenderAdd(float* mixL, float* mixR, float* scratchIn, float* scratchL,
                 float* scratchR, int n, float mixGain);

 private:
  void ApplyOcclusion(float* x, int n);

  xyzpan::XYZPanEngine engine_;
  const std::vector<float>* buffer_ = nullptr;
  double sampleRate_ = 48000.0;

  // Occlusion handoff. Relaxed is enough: a frame of staleness is absorbed by
  // the smoothing, and these three are independent scalars, not a struct whose
  // fields must agree.
  std::atomic<float> occlCutoffTarget_{20000.0f};
  std::atomic<float> occlGainTarget_{1.0f};
  std::atomic<float> occlSmoothMs_{100.0f};

  // Audio-thread occlusion state: smoothed cutoff/gain + an RBJ low-pass
  // (transposed direct form II). Snapped to the targets on adoption.
  float occlCutoff_ = 20000.0f;
  float occlGain_ = 1.0f;
  float occlZ1_ = 0.0f, occlZ2_ = 0.0f;

  // Audio-thread playback state.
  bool engaged_ = false;  // adopted and live; distinguishes retrigger-of-live
  double playhead_ = 0.0;
  double rate_ = 1.0;
  float voiceGain_ = 1.0f;
  int tailLeft_ = 0;  // post-buffer flush samples (delay lines, filter ring-out)

  // Activation handoff: game writes pending*, then flips state_ (release).
  std::atomic<int> state_{(int)VoiceState::Free};
  const std::vector<float>* pendingBuffer_ = nullptr;
  float pendingGain_ = 1.0f;
  double pendingRate_ = 1.0;
  std::size_t pendingOffset_ = 0;
};

}  // namespace audio
