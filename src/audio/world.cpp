#include "audio/world.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "audio/xyzpan/Constants.h"
#include "sim/world.h"

#if defined(_MSC_VER) || defined(__SSE2__)
#include <immintrin.h>
#include <pmmintrin.h>
#define SANDVOX_SSE_DENORMAL 1
#endif

namespace audio {
namespace {

// Startup ramp length. Long enough to hide the engines' distance-delay
// smoothers converging from their reset values, short enough not to swallow a
// sound the player triggers immediately.
constexpr float kStartupRampSec = 0.5f;

// Master gain slew once past the startup ramp (per-sample coefficient is
// derived from this in Render), so a tuning change to master volume does not
// step.
constexpr float kMasterSlewSec = 0.05f;

}  // namespace

void AudioWorld::Init(double sampleRate, const ListenerPose& initialListener) {
  sampleRate_ = sampleRate;
  listener_ = initialListener;

  mixL_.assign(kMaxBlock, 0.0f);
  mixR_.assign(kMaxBlock, 0.0f);
  scratchIn_.assign(kMaxBlock, 0.0f);
  srcL_.assign(kMaxBlock, 0.0f);
  srcR_.assign(kMaxBlock, 0.0f);

  loops_.clear();
  oneShots_.clear();
  loopGen_.assign(kLoopVoices, 0);

  Voice probe;
  probe.worldPos = initialListener.posVox * kVoxelMeters;
  const xyzpan::EngineParams init = MakeParams(probe, initialListener.posVox,
                                               initialListener, nullptr);
  for (int i = 0; i < kLoopVoices; i++) {
    auto v = std::make_unique<Voice>();
    v->isOneShot = false;
    v->Prepare(sampleRate, kMaxBlock, init);
    loops_.push_back(std::move(v));
  }
  for (int i = 0; i < kOneShotVoices; i++) {
    auto v = std::make_unique<Voice>();
    v->isOneShot = true;
    v->Prepare(sampleRate, kMaxBlock, init);
    oneShots_.push_back(std::move(v));
  }

  masterGain_ = 0.0f;
  masterRampInc_ = 1.0f / (kStartupRampSec * (float)sampleRate);
}

xyzpan::EngineParams AudioWorld::MakeParams(const Voice& v, const Vec3& srcVox,
                                            const ListenerPose& listener,
                                            const OcclusionResult* occ) const {
  xyzpan::EngineParams p;  // defaults neutralize LFOs, orbits, test tone, width

  // ---- THE ONE COORDINATE CONVERSION ------------------------------------
  // sandvox: voxels, Y up.   xyzpan: meters, Z up / Y forward.
  // So (x, y_up, z) -> (x, z, y_up) and everything scales by kVoxelMeters.
  // Meters are required, not merely convenient: the engine's binaural cues use
  // virtual ears offset by 0.087 UNITS, which is only a head radius if a unit
  // is a meter. Feeding voxels would put the ears 87 cm apart.
  const Vec3 sM = srcVox * kVoxelMeters;
  const Vec3 lM = listener.posVox * kVoxelMeters;
  p.x = sM.x;
  p.y = sM.z;
  p.z = sM.y;
  p.listenerX = lM.x;
  p.listenerY = lM.z;
  p.listenerZ = lM.y;

  // Yaw: sandvox measures the camera's heading with the same handedness the
  // engine expects once Y and Z are swapped, so it passes through. Pitch is
  // negated because swapping the two axes above mirrors the elevation sense.
  p.listenerYaw = listener.yaw;
  p.listenerPitch = -listener.pitch;
  p.listenerRoll = 0.0f;

  p.sphereRadius = std::max(0.5f, v.audibleRadius);

  // Propagation delay / doppler. The engine derives its delay fraction as
  // clamp(dist / sqrt(3)) -- a leftover unit-cube assumption that would
  // saturate at 1.73 m. Driving distDelayMaxMs by the actual distance makes
  // the product exactly dist/343 s at every range, so this is physically
  // correct propagation delay without forking the engine. See
  // xyzpan/VENDORED_FROM.md.
  const float distM = (sM - lM).len();
  p.distDelayMaxMs =
      std::min(450.0f, std::max(distM * v.dopplerScale, xyzpan::kSqrt3) * (1000.0f / 343.0f));
  p.dopplerEnabled = v.dopplerEnabled;

  // The occlusion filter sits pre-engine, so it ducks the reverb send along
  // with the dry. Scale the wet back up by wetGain/gain to restore the share
  // the solve says survives (a sound behind a wall should still sound like it
  // is in a space, just not like it is in YOUR space).
  p.verbWet = v.verbWet;
  if (occ != nullptr && occ->gain > 1e-4f)
    p.verbWet = std::min(1.0f, v.verbWet * occ->wetGain / occ->gain);

  // Floor bounce (the vendored LOCAL MOD): driven by height above the ground
  // rather than the listener-relative elevation angle. We do not have a cheap
  // ground height for an arbitrary emitter here, so this is left at the
  // engine's legacy elevation behaviour (-1) except for sounds the cue layer
  // explicitly marks as ground-level. See cues.cpp.
  p.floorBounceFactor = -1.0f;

  p.swarmNodeCount = 1;  // only node 0 processes; this is what keeps a voice small
  return p;
}

float AudioWorld::Audibility(float gain, float distM, float audibleRadius) const {
  // Mirrors the engine's own per-node distance gain so voice ranking agrees
  // with the mix.
  const float maxRange = std::max(audibleRadius - xyzpan::kMinDistance, 0.001f);
  const float frac = std::clamp((distM - xyzpan::kMinDistance) / maxRange, 0.0f, 1.0f);
  const xyzpan::DistGainCurve curve = xyzpan::makeDistGainCurve(
      20.0f * std::log10(xyzpan::kDistGainMax), xyzpan::kDistGainFloorDb,
      xyzpan::kDistCurveSteepDefault, xyzpan::kDistGainMax);
  return gain * xyzpan::evalDistGainCurve(curve, frac);
}

void AudioWorld::RefreshVoice(Voice& v, const ListenerPose& listener, World* world) const {
  OcclusionResult occ;  // defaults are the fully-open state
  const Vec3 srcVox = v.worldPos * (1.0f / kVoxelMeters);
  if (world != nullptr && occlTuning_.enabled && !acoustics_.empty()) {
    occ = ComputeOcclusion(*world, listener.posVox, srcVox, acoustics_, occlTuning_);
    occ = ScaleOcclusion(occ, v.occlScale);
  }
  v.params.Write(MakeParams(v, srcVox, listener, &occ));
  v.SetOcclusion(occ.cutoffHz, occ.gain, 120.0f);
}

void AudioWorld::Update(const ListenerPose& listener, World* world) {
  listener_ = listener;
  for (auto& v : loops_)
    if (v->Active()) RefreshVoice(*v, listener, world);
  for (auto& v : oneShots_)
    if (v->Active()) RefreshVoice(*v, listener, world);
}

bool AudioWorld::PlayOneShot(const std::vector<float>* buf, const Vec3& posVox,
                             const VoiceConfig& cfg) {
  if (buf == nullptr || buf->empty()) return false;

  const float distM = (posVox - listener_.posVox).len() * kVoxelMeters;
  const float rank = Audibility(cfg.gain, distM, cfg.audibleRadius);
  // Below this the sound is inaudible in the mix; spending a voice on it would
  // only starve one that matters.
  if (rank < 1e-4f) return false;

  Voice* pick = nullptr;
  for (auto& v : oneShots_) {
    if (!v->Active()) {
      pick = v.get();
      break;
    }
  }

  bool steal = false;
  if (pick == nullptr) {
    // Saturated: find the least audible playing voice and take it only if the
    // newcomer is clearly louder. The margin stops a stream of similar-ranked
    // sounds from cutting each other off every frame.
    Voice* worst = nullptr;
    float worstRank = rank * 0.7f;
    for (auto& v : oneShots_) {
      const float d = (v->worldPos - listener_.posVox * kVoxelMeters).len();
      const float r = Audibility(v->triggerGain, d, v->audibleRadius);
      if (r < worstRank) {
        worstRank = r;
        worst = v.get();
      }
    }
    if (worst == nullptr) return false;  // everything playing outranks this
    pick = worst;
    steal = true;
  }

  pick->worldPos = posVox * kVoxelMeters;
  pick->emitGain = 1.0f;
  pick->audibleRadius = cfg.audibleRadius;
  pick->verbWet = cfg.verbWet;
  pick->dopplerEnabled = cfg.doppler;
  pick->dopplerScale = cfg.dopplerScale;
  pick->occlScale = cfg.occlusionScale;
  pick->isOneShot = true;
  // Params and occlusion must be correct BEFORE the release-store, so the
  // voice is born at the right place and already muffled if it is behind a
  // wall. This is why RefreshVoice is also the pre-trigger path.
  RefreshVoice(*pick, listener_, nullptr);

  if (steal)
    pick->Retrigger(buf, cfg.gain, cfg.rate);
  else
    pick->Trigger(buf, cfg.gain, cfg.rate);
  return true;
}

int AudioWorld::PlayLoop(const std::vector<float>* buf, const Vec3& posVox,
                         const VoiceConfig& cfg) {
  if (buf == nullptr || buf->empty()) return -1;
  for (size_t i = 0; i < loops_.size(); i++) {
    Voice& v = *loops_[i];
    if (v.Active()) continue;
    v.worldPos = posVox * kVoxelMeters;
    v.emitGain = cfg.gain;
    v.audibleRadius = cfg.audibleRadius;
    v.verbWet = cfg.verbWet;
    v.dopplerEnabled = cfg.doppler;
    v.dopplerScale = cfg.dopplerScale;
    v.occlScale = cfg.occlusionScale;
    v.isOneShot = false;
    v.triggerGain = cfg.gain;
    RefreshVoice(v, listener_, nullptr);
    if (!v.TriggerLoop(buf, 0)) return -1;
    // Encode the generation in the handle so a stale handle cannot stop a
    // loop that reused the slot.
    return (int)((loopGen_[i] << 8) | (uint32_t)i);
  }
  return -1;
}

namespace {
inline int LoopSlot(int handle) { return handle & 0xFF; }
inline uint32_t LoopGenOf(int handle) { return (uint32_t)handle >> 8; }
}  // namespace

bool AudioWorld::LoopActive(int handle) const {
  if (handle < 0) return false;
  const int i = LoopSlot(handle);
  if (i < 0 || i >= (int)loops_.size()) return false;
  if (loopGen_[(size_t)i] != LoopGenOf(handle)) return false;
  return loops_[(size_t)i]->Active();
}

void AudioWorld::StopLoop(int handle) {
  if (!LoopActive(handle)) return;
  const int i = LoopSlot(handle);
  loops_[(size_t)i]->RequestStop();
  loopGen_[(size_t)i]++;  // retire the handle
}

void AudioWorld::SetLoopPos(int handle, const Vec3& posVox) {
  if (!LoopActive(handle)) return;
  loops_[(size_t)LoopSlot(handle)]->worldPos = posVox * kVoxelMeters;
}

void AudioWorld::SetLoopGain(int handle, float gain) {
  if (!LoopActive(handle)) return;
  Voice& v = *loops_[(size_t)LoopSlot(handle)];
  v.emitGain = gain;
  v.triggerGain = gain;
}

int AudioWorld::ActiveVoices() const {
  int n = 0;
  for (const auto& v : loops_)
    if (v->Active()) n++;
  for (const auto& v : oneShots_)
    if (v->Active()) n++;
  return n;
}

double AudioWorld::ConsumeCpuLoad() {
  const uint64_t ns = renderNanos_.exchange(0, std::memory_order_relaxed);
  const uint64_t fr = renderedFrames_.exchange(0, std::memory_order_relaxed);
  if (fr == 0) return 0.0;
  const double budgetNs = (double)fr / sampleRate_ * 1e9;
  return budgetNs > 0.0 ? (double)ns / budgetNs : 0.0;
}

void AudioWorld::Render(float* interleavedOut, int frames) {
#ifdef SANDVOX_SSE_DENORMAL
  // Reverb tails and a dozen IIR filters decay into denormal range, where an
  // x86 core takes a microcoded slow path. Flushing to zero is the difference
  // between a stable callback and one that spikes as sounds fade out.
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
  const auto t0 = std::chrono::steady_clock::now();

  const float target = masterTarget_.load(std::memory_order_relaxed);
  const float slew = 1.0f - std::exp(-1.0f / (kMasterSlewSec * (float)sampleRate_));

  int done = 0;
  while (done < frames) {
    const int n = std::min(frames - done, kMaxBlock);
    float* mixL = mixL_.data();
    float* mixR = mixR_.data();
    std::fill(mixL, mixL + n, 0.0f);
    std::fill(mixR, mixR + n, 0.0f);

    for (auto& v : loops_)
      v->RenderAdd(mixL, mixR, scratchIn_.data(), srcL_.data(), srcR_.data(), n,
                   kPerSourceGain);
    for (auto& v : oneShots_)
      v->RenderAdd(mixL, mixR, scratchIn_.data(), srcL_.data(), srcR_.data(), n,
                   kPerSourceGain);

    float* out = interleavedOut + (size_t)done * 2;
    for (int i = 0; i < n; i++) {
      // Startup ramp first, then slew toward the tunable master volume.
      if (masterGain_ < target) {
        masterGain_ = std::min(target, masterGain_ + masterRampInc_);
      } else {
        masterGain_ += slew * (target - masterGain_);
      }
      // tanh soft clip: with a dozen voices summing, transient stacks WILL
      // exceed unity, and a hard clip on a footstep transient is far more
      // audible than the gentle compression this applies.
      out[2 * i] = std::tanh(mixL[i] * masterGain_);
      out[2 * i + 1] = std::tanh(mixR[i] * masterGain_);
    }
    done += n;
  }

  const auto t1 = std::chrono::steady_clock::now();
  renderNanos_.fetch_add(
      (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count(),
      std::memory_order_relaxed);
  renderedFrames_.fetch_add((uint64_t)frames, std::memory_order_relaxed);
}

}  // namespace audio
