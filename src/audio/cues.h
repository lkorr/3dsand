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

  // This material coming apart: a chunk of terrain losing its support and
  // detaching as a rigidbody, dug out, or blasted loose.
  //
  // `sizeVoxels` is the voxel count of the piece that broke off. It selects
  // the pitch CENTRE -- a big log snaps lower than a twig -- around which a
  // random ±kBreakSemitones detune is applied per event. That randomization is
  // not decoration: a break is the one cue most likely to fire several times
  // in a second (one support scan can free a dozen islands), and identical
  // repeats of a one-second sample read as a stuck machine gun.
  //
  // Deliberately NOT tied to the world hash — audio is presentation, so this
  // uses the plain rng_ rather than the sim's counter-based hash (rule 1
  // constrains the sim, and a cue can never feed back into it).
  void Break(uint32_t matId, const Vec3& posVox, int sizeVoxels);

  // ---- creature events ----------------------------------------------------
  // Bound per mob in its .json sidecar (MobDef::sounds), keyed by the slot
  // names in assets/sound_schema.js. There is deliberately no fallback: a mob
  // with nothing bound is silent, rather than borrowing another creature's
  // voice.
  //
  // `intensity` is in [0,1] and means whatever the event needs it to — the
  // fraction of max hp removed for Hurt, blow severity for Sever. It drives
  // gain and a downward pitch bend, the same shape Land and Impact use.
  //
  // Dismember is the CUT, not the cry: flesh parting under a blade. It is a
  // separate event from Sever because the two have different causes — every
  // sever has a voice, but only a bladed one saws through anything, and an
  // explosion that takes the same arm off must not play it. A sword blow
  // fires both.
  enum class MobEvent { Hurt, Death, Sever, Idle, Alert, Attack, Dismember };
  void MobSound(const MobDef& def, MobEvent ev, const Vec3& posVox,
                float intensity = 1.0f, uint64_t sourceId = 0);

  // Resolve one mob slot to a library id, or -1. Exposed so callers can skip
  // building an event for a mob that binds nothing.
  int MobSetId(const MobDef& def, MobEvent ev) const;

  // ---- bleeding -----------------------------------------------------------
  // A positioned wet loop for a creature losing a lot of blood, keyed by a
  // caller-chosen id (mob id, or a limb key) so several wounds can sound at
  // once and each can be updated independently.
  //
  // `intensity` in [0,1] is how hard the wound is pumping. Call it every frame
  // for every heavily-bleeding wound: the loop starts itself when intensity
  // first exceeds the on-threshold, tracks gain while it runs, and stops when
  // intensity falls back under the off-threshold (which is LOWER, so a wound
  // hovering at the boundary does not stutter the voice on and off).
  //
  // Wounds not updated on a frame are reaped, so a caller that stops
  // reporting a mob — because it died, despawned or stopped bleeding — does
  // not have to remember to stop its loop.
  void MobBleed(uint64_t sourceId, const Vec3& posVox, float intensity);

  // ---- world ambience -----------------------------------------------------
  // The night bed: rare, quiet, and non-diegetic. `want` in [0,1] is how much
  // of it should be audible right now — main.cpp drives that from the day
  // phase, so it eases in after dusk and out at dawn — and `allowStart` gates
  // whether a NEW pass may begin, which is where the rarity lives. A pass that
  // has already started is never cut off by allowStart going false; only `want`
  // reaching 0 stops it.
  void SetNightAmbience(const Vec3& listenerPosVox, float want, bool allowStart);

  // ---- ambience -----------------------------------------------------------
  // A positioned looping bed, e.g. a lava lake or a waterfall. Returns a handle
  // for MoveAmbience/StopAmbience, or -1 if the set is missing or the loop pool
  // is full. `radius` is the audible radius in meters.
  int StartAmbience(const std::string& setName, const Vec3& posVox, float gain,
                    float radius, float occlusionScale = 1.0f);
  void MoveAmbience(int handle, const Vec3& posVox);
  void SetAmbienceGain(int handle, float gain);
  void StopAmbience(int handle);

  // ---- material ambience (the automatic driver for the loops above) -------
  //
  // WHAT DRIVES THE LOOP. A material carries an "ambience" slot naming a set
  // (`{"ambience": "water"}`); this scans the CPU mirror for the largest body
  // of any such material near the listener and keeps ONE positioned loop on
  // it. The three quantities and where each comes from:
  //
  //   position  the CENTROID of the sampled cells, not the nearest cell and
  //             not the player. A shoreline should pan toward the water as you
  //             walk along it, and a nearest-cell emitter instead sticks to
  //             your feet and pans to nothing. Eased, so the emitter drifts
  //             rather than jumping each scan.
  //   gain      the sampled cell COUNT, i.e. how much water is nearby. A
  //             puddle is under the floor and silent; a lake is at full gain.
  //             Also eased, so crossing a chunk boundary is not a step.
  //   radius    audio.ambienceRadius. One number: the loop is a bed, and a
  //             body of water does not have an authored size.
  //
  // WHAT IT DELIBERATELY DOES NOT DO. Exactly one loop is live at a time (the
  // strongest material wins). Two lakes on opposite sides of the player is a
  // CLUSTERING problem — "is that one body of water or two" — and inventing an
  // answer to it here would be a system, not a hook.
  //
  // COST WHEN IDLE. If no material in the project binds an ambience set, this
  // is one bool test per frame, forever. Otherwise it is a stride-4 subsample
  // of the 3x3x3 mirror (1728 reads) at most twice a second, over memory that
  // is already resident, with no allocation and no GPU work.
  struct AmbienceProbe {
    uint32_t material = 0;  // 0 = nothing ambient within the mirror
    Vec3 posVox{};          // centroid of the sampled cells, world voxels
    float weight = 0.0f;    // 0..1, sampled-cell count vs kAmbienceFullCells
    int cells = 0;          // raw sampled-cell count, for tests and the panel
  };
  // The scan, split out from the voice so it can be asserted with no audio
  // device: a headless run has no mixer but does have a world (DESIGN.md §12b
  // "Headless is silent"). Needs only RebuildMaterialTable to have run.
  AmbienceProbe ProbeAmbience(const World& world,
                              const Vec3& listenerPosVox) const;
  // False when no material binds an ambience set at all, which is the
  // early-out that makes the whole feature free in a project without one.
  bool AnyAmbienceMaterial() const { return anyAmbience_; }

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
    uint32_t steps = 0, lands = 0, impacts = 0, breaks = 0, mobs = 0,
             bleeds = 0, dropped = 0;
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

  // ---- bleed loops --------------------------------------------------------
  // One live loop per heavily-bleeding wound. `seen` is the reap flag: Update
  // clears it, MobBleed sets it, and anything still clear at the end of the
  // frame stopped bleeding (or its owner died) and is stopped. That inverted
  // ownership is deliberate -- the caller cannot leak a voice by forgetting to
  // close one, which for a mob that can be dismembered, ragdolled and
  // despawned on the same frame is a real hazard.
  struct BleedLoop {
    int handle = -1;
    float gain = 0.0f;   // eased, so a wound does not click on and off
    bool seen = false;
  };
  std::map<uint64_t, BleedLoop> bleeds_;
  void ReapBleeds();

  // ---- material ambience --------------------------------------------------
  void UpdateAmbience(float dt, const Vec3& listenerPosVox, World* world);
  // Indexed by material id: does this material author an "ambience" slot. A
  // flat byte table because it is tested once per sampled cell, 1728 times a
  // scan, and a map lookup there would be the whole cost of the feature.
  std::vector<uint8_t> ambienceOwner_;
  // material id -> resolved full set name. Sparse: consulted once per scan,
  // never per cell.
  std::map<uint32_t, std::string> ambienceSet_;
  bool anyAmbience_ = false;
  int ambHandle_ = -1;
  uint32_t ambMat_ = 0;     // material the live loop is following
  float ambGain_ = 0.0f;    // eased toward the probe's weight
  Vec3 ambPos_{};           // eased toward the probe's centroid
  float ambScanTimer_ = 0.0f;
  AmbienceProbe ambLast_{};

  // ---- night bed ----------------------------------------------------------
  int nightHandle_ = -1;
  float nightGain_ = 0.0f;   // eased toward the caller's `want`
  bool nightPlaying_ = false;

  std::vector<std::string> warnings_;
  std::string soundDir_;
  Stats stats_;
};

}  // namespace audio
