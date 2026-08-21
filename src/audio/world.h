#pragma once
// Owns the spatializer voice pools and renders the master mix.
//
// Layering, top to bottom:
//   cues.h    decides WHAT plays and WHEN (footsteps, impacts, ambience)
//   world.h   decides WHICH VOICE plays it and renders the mix   <- this file
//   voice.h   one emitter: buffer playback + occlusion filter + one engine
//   xyzpan/   the spatializer itself
//
// This class knows only buffers, positions and voices. It has no idea what a
// footstep is, and that separation is what keeps adding a new sound cheap.
//
// VOICE BUDGET. Each voice is a fully prepared XYZPanEngine (~0.6 MB with one
// swarm node), so the pools are fixed and the real ceiling is audio-thread CPU,
// not memory. When a pool saturates, Audibility() decides who gets to play:
// a new one-shot steals the least audible playing voice only if it is clearly
// louder, otherwise it is dropped. Dropping is correct -- a sound you would not
// have heard costs nothing to lose, and stealing an audible voice to play an
// inaudible one is strictly worse than silence.

#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "audio/occlusion.h"
#include "audio/voice.h"
#include "math3d.h"

class World;

namespace audio {

// Listener pose in the game's own units and axes: position in VOXELS, Y up.
// Conversion to the engine's meters/Z-up convention happens in exactly one
// place (AudioWorld::MakeParams) -- see xyzpan/VENDORED_FROM.md.
struct ListenerPose {
  Vec3 posVox{};
  float yaw = 0.0f;    // radians, sandvox camera convention
  float pitch = 0.0f;
};

// Per-trigger character, supplied by the cue layer.
struct VoiceConfig {
  float gain = 1.0f;
  float audibleRadius = 35.0f;  // meters; distance gain hits the floor here
  float verbWet = 0.0f;
  bool doppler = true;
  float dopplerScale = 1.0f;
  float occlusionScale = 1.0f;  // < 1 for spatially extended sources
  float rate = 1.0;             // playback rate (pitch); 1 = natural
};

class AudioWorld {
 public:
  static constexpr int kMaxBlock = 512;
  static constexpr int kLoopVoices = 8;     // ambience beds, continuous emitters
  static constexpr int kOneShotVoices = 12; // footsteps, impacts, everything transient
  static constexpr float kPerSourceGain = 0.5f;  // fixed pre-master headroom trim

  // Prepares every voice engine. Must complete before the device starts.
  void Init(double sampleRate, const ListenerPose& initialListener);

  // ---- game thread ----

  // Once per frame: republish the listener pose and re-solve occlusion for
  // every active voice. `world` may be null (no occlusion solve) -- the
  // headless selftest paths never build one.
  void Update(const ListenerPose& listener, World* world);

  // Acoustic table indexed by material id, and the occlusion knobs. Both are
  // re-published from tuning each frame by the cue layer.
  void SetAcoustics(std::vector<MaterialAcoustics> a) { acoustics_ = std::move(a); }
  void SetOcclusionTuning(const OcclusionTuning& t) { occlTuning_ = t; }
  void SetMasterGain(float g) { masterTarget_.store(g, std::memory_order_relaxed); }

  // Fires a one-shot at a world position (VOXELS). Returns false if the sound
  // is below the audible floor at this distance, or if the pool is saturated
  // and every playing voice outranks it.
  bool PlayOneShot(const std::vector<float>* buf, const Vec3& posVox,
                   const VoiceConfig& cfg);

  // Starts a looping voice. Returns a stable handle for StopLoop/SetLoopPos,
  // or -1 if the loop pool is exhausted.
  int PlayLoop(const std::vector<float>* buf, const Vec3& posVox, const VoiceConfig& cfg);
  void StopLoop(int handle);
  void SetLoopPos(int handle, const Vec3& posVox);
  void SetLoopGain(int handle, float gain);
  bool LoopActive(int handle) const;

  int ActiveVoices() const;
  // Average audio-callback CPU fraction (0..1) since the last call.
  double ConsumeCpuLoad();
  double SampleRate() const { return sampleRate_; }

  // Predicted level of a sound at the listener: the trigger gain through the
  // engine's own distance curve, so ranking agrees with what the mix will
  // actually do. Occlusion is deliberately ignored -- a muffled nearby sound
  // still outranks a clear distant one.
  float Audibility(float gain, float distM, float audibleRadius) const;

  // ---- audio thread ----
  // Renders `frames` of interleaved stereo. No allocation, no locks.
  void Render(float* interleavedOut, int frames);

 private:
  xyzpan::EngineParams MakeParams(const Voice& v, const Vec3& srcVox,
                                  const ListenerPose& listener,
                                  const OcclusionResult* occ) const;
  // One voice's per-frame publish: solve occlusion, write engine params, hand
  // the filter targets to the audio thread. Also the pre-trigger setup path.
  void RefreshVoice(Voice& v, const ListenerPose& listener, World* world) const;

  double sampleRate_ = 48000.0;
  ListenerPose listener_;
  std::vector<MaterialAcoustics> acoustics_;
  OcclusionTuning occlTuning_;

  std::vector<std::unique_ptr<Voice>> loops_;
  std::vector<std::unique_ptr<Voice>> oneShots_;
  // Generation counter per loop voice, so a stale handle from a loop that
  // already ended cannot silence whatever took its slot.
  std::vector<uint32_t> loopGen_;

  // Audio-thread scratch, preallocated in Init.
  std::vector<float> mixL_, mixR_, scratchIn_, srcL_, srcR_;

  // Master gain: a startup ramp masks the engines' delay smoothers settling
  // from their reset state over the first ~0.5 s, and the target is the
  // tunable master volume.
  std::atomic<float> masterTarget_{1.0f};
  float masterGain_ = 0.0f;
  float masterRampInc_ = 0.0f;

  std::atomic<uint64_t> renderNanos_{0};
  std::atomic<uint64_t> renderedFrames_{0};
};

}  // namespace audio
