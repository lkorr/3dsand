#pragma once
// The game-facing sound layer: decides WHAT plays and WHEN.
//
// This is the only audio header the rest of sandvox includes. Everything below
// it (voice pools, the spatializer, occlusion) is an implementation detail, and
// everything above it -- avatar, physics, main -- speaks in game events:
//
//     audio.Footstep(matId, posVox, speed, foot);
//     audio.Impact(matId, posVox, energy);
//     audio.Land(matId, posVox, fallSpeed);
//
// MATERIAL -> SOUND IS DATA. materials.json carries a "footstep" key naming a
// sound set ("leaf", "metal", ...); this class resolves those names to library
// ids ONCE at load into a flat table indexed by material id, so a footstep
// costs an array lookup, not a string hash. Materials with no key fall back by
// tag (organic -> leaf, mineral -> path) so a new material is audible the day
// it is added and only needs an explicit key when the fallback is wrong. This
// mirrors how reactions target tags rather than enumerating materials.
//
// THREADING. Every method here is GAME THREAD ONLY. Tuning is read on this side
// and copied into the audio layer; the audio thread never sees a game object.
// See voice.h for the full contract.

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "audio/device.h"
#include "audio/library.h"
#include "audio/occlusion.h"
#include "audio/world.h"
#include "math3d.h"

struct MaterialDef;
struct MobDef;
class World;

namespace audio {

// Everything needed to voice one footstep, resolved per material.
struct FootstepMapping {
  int setId = -1;      // library set for this material's steps, -1 = silent
  float gain = 1.0f;   // per-material loudness trim
};

class Cues {
 public:
  // Opens the device, loads assets, and builds the material tables. Safe to
  // call when there is no audio hardware: everything downstream no-ops and the
  // game runs silent. `soundDir` is assets/sounds.
  //
  // Returns false if audio is unavailable, which is informational only -- no
  // caller should treat it as an error.
  bool Init(const std::string& soundDir, const std::vector<MaterialDef>& mats);

  void Shutdown();
  bool Enabled() const { return enabled_; }

  // Once per frame, after the player/camera have moved. Publishes the listener
  // pose, re-solves occlusion for active voices, and re-reads tuning.
  // `world` may be null (headless), which disables the occlusion solve.
  void Update(float dt, const Vec3& listenerPosVox, float yaw, float pitch, World* world);

  // Re-resolve material -> sound after a materials.json hot-reload (R).
  void RebuildMaterialTable(const std::vector<MaterialDef>& mats);
  // Re-scan assets/sounds for newly added files, then re-resolve. Returns the
  // number of new variant buffers loaded.
  int RescanSounds(const std::vector<MaterialDef>& mats);

  // ---- events -------------------------------------------------------------

  // A foot planted on `matId` at `posVox`. `speed` is the walker's speed in
  // voxels/sec (drives loudness and pitch), `foot` distinguishes left/right so
  // the two can be pitched apart slightly -- a real gait is not two identical
  // impacts, and identical ones read as a machine.
  void Footstep(uint32_t matId, const Vec3& posVox, float speed, int foot);

  // Landing from a fall. `fallSpeed` is the downward speed at touchdown in
  // voxels/sec; louder and lower-pitched than a step.
  void Land(uint32_t matId, const Vec3& posVox, float fallSpeed);

  // A physical impact (debris, a thrown body) carrying `energy` in [0,1].
  void Impact(uint32_t matId, const Vec3& posVox, float energy);

  // ---- creature events ----------------------------------------------------
  // Bound per mob in its .json sidecar (MobDef::sounds), keyed by the slot
  // names in assets/sound_schema.js. There is deliberately no fallback: a mob
  // with nothing bound is silent, rather than borrowing another creature's
  // voice.
  //
  // `intensity` is in [0,1] and means whatever the event needs it to — the
  // fraction of max hp removed for Hurt, blow severity for Sever. It drives
  // gain and a downward pitch bend, the same shape Land and Impact use.
  enum class MobEvent { Hurt, Death, Sever, Idle, Alert, Attack };
  void MobSound(const MobDef& def, MobEvent ev, const Vec3& posVox,
                float intensity = 1.0f, uint64_t sourceId = 0);

  // Resolve one mob slot to a library id, or -1. Exposed so callers can skip
  // building an event for a mob that binds nothing.
  int MobSetId(const MobDef& def, MobEvent ev) const;

  // ---- ambience -----------------------------------------------------------
  // A positioned looping bed, e.g. a lava lake or a waterfall. Returns a handle
  // for MoveAmbience/StopAmbience, or -1 if the set is missing or the loop pool
  // is full. `radius` is the audible radius in meters.
  int StartAmbience(const std::string& setName, const Vec3& posVox, float gain,
                    float radius, float occlusionScale = 1.0f);
  void MoveAmbience(int handle, const Vec3& posVox);
  void SetAmbienceGain(int handle, float gain);
  void StopAmbience(int handle);

  // ---- diagnostics --------------------------------------------------------
  int ActiveVoices() const { return enabled_ ? world_.ActiveVoices() : 0; }
  double CpuLoad() { return enabled_ ? world_.ConsumeCpuLoad() : 0.0; }
  const Library& Lib() const { return lib_; }
  // Material ids that resolved to no footstep set, for the load-time report.
  const std::vector<std::string>& Warnings() const { return warnings_; }

  // Slot -> sound-set namespace ("footstep" -> "footsteps"), so an authored
  // "leaf" resolves to the set "footsteps/leaf". Mirrored by the `prefix`
  // field in assets/sound_schema.js — see the definition in cues.cpp.
  static const std::map<std::string, std::string> kSlotPrefix;
  // Cue counters since startup. Cheap, and the fastest way to answer "is the
  // gait producing steps at all" separately from "can I hear them" — the two
  // failure modes look identical from the speakers.
  struct Stats {
    uint32_t steps = 0, lands = 0, impacts = 0, mobs = 0, dropped = 0;
  };
  const Stats& GetStats() const { return stats_; }

 private:
  void ApplyTuning();
  const std::vector<float>* PickStep(int setId, int& lastVariant);
  // A material slot with the tag fallback applied; -1 when deliberately silent.
  int ResolveMaterialSlot(const MaterialDef& m, const char* slot) const;

  bool enabled_ = false;
  Device device_;
  AudioWorld world_;
  Library lib_;
  std::mt19937 rng_{0xA0D10};

  // Indexed by 12-bit material id. One table per slot, resolved once at load so
  // a cue costs an array lookup rather than a map lookup and a string concat.
  std::vector<FootstepMapping> footstep_;
  std::vector<FootstepMapping> land_;
  std::vector<FootstepMapping> impact_;
  std::vector<FootstepMapping> break_;
  std::vector<MaterialAcoustics> acoustics_;

  // Mob slots are resolved on demand and memoised: there are few mob defs, they
  // are rebound only on a rescan, and the alternative — a table parallel to the
  // def list — would go stale on every mob hot-reload.
  mutable std::map<std::string, int> mobSetCache_;
  // Last tick a given source made a Hurt noise, so a body taking a burst of
  // damage in one frame speaks once. Keyed by caller-supplied source id; a
  // source id of 0 opts out.
  std::map<uint64_t, double> lastMobVoice_;
  double now_ = 0.0;

  // Last variant played per set, so a set never repeats a sample back to back.
  std::vector<int> lastVariant_;

  std::vector<std::string> warnings_;
  std::string soundDir_;
  Stats stats_;
};

}  // namespace audio
