#include "audio/cues.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/mob.h"
#include "sim/materials.h"
#include "sim/tuning.h"
#include "sim/world.h"

namespace audio {
namespace {

// Preferred device rate. miniaudio negotiates; whatever comes back is what the
// library decodes to.
constexpr unsigned kPreferredRate = 48000;

// Minimum gap between two rate-limited voices from the SAME source. Long
// enough that a burst of per-tick damage events speaks once, short enough that
// two genuinely separate hits are two sounds.
constexpr double kMobVoiceMinGap = 0.12;

bool HasTag(const MaterialDef& m, const char* tag) {
  for (const std::string& t : m.tags)
    if (t == tag) return true;
  return false;
}

// Footstep set for a material that did not name one. Tags, not material ids --
// same guard against the N x M explosion the reaction system uses. Order
// matters: the most specific tag wins.
//
// These map onto the sets that exist today (path/leaf/metal/branch). As real
// sets are recorded, this table is where "grass" or "snow" gets pointed at its
// own folder; nothing else changes.
const char* FallbackFootstep(const MaterialDef& m) {
  if (HasTag(m, "foliage")) return "leaf";
  if (m.gpu.klass == CLASS_LIQUID) return "";  // liquids: no step sound yet
  if (m.gpu.klass == CLASS_GAS) return "";
  if (HasTag(m, "organic")) return "branch";   // wood, plants: a dry snap
  if (HasTag(m, "soil")) return "path";        // dirt, sand, ash, grass
  if (HasTag(m, "mineral")) return "path";     // stone, gravel
  return "path";                               // everything solid gets a step
}

// Acoustic transmission for a material. Derived from what the sim already
// knows -- class, hardness and tags -- rather than a new authored field,
// because a per-material acoustics block would be 56 rows of guesswork that
// nobody would keep current. When a material genuinely needs its own value,
// this is the function to special-case.
MaterialAcoustics AcousticsFor(const MaterialDef& m) {
  MaterialAcoustics a;
  switch (m.gpu.klass) {
    case CLASS_GAS:
      // Smoke and fire do not block sound. (They should SCATTER it, which the
      // engine has no path for; see occlusion.h.)
      a.dbPerM = 0.0f;
      a.hfDepthM = 100.0f;
      break;
    case CLASS_LIQUID:
      // Sound crosses water readily but loses highs fast.
      a.dbPerM = 3.0f;
      a.hfDepthM = 1.2f;
      break;
    case CLASS_POWDER:
      // Loose granular material is a poor barrier but a good absorber: sand
      // and snow are the classic "sound dies here" surfaces.
      a.dbPerM = 9.0f;
      a.hfDepthM = 0.6f;
      break;
    case CLASS_SOLID:
    default:
      if (HasTag(m, "foliage")) {
        // Leaves scatter highs and pass lows -- a hedge muffles, it does not
        // block. This is the one case where getting it wrong is very audible.
        a.dbPerM = 2.5f;
        a.hfDepthM = 2.5f;
      } else {
        // Hardness (0..255) stands in for density: bedrock blocks far more
        // than a plank. The floor keeps a soft solid from being transparent.
        const float h = (float)m.gpu.hardness / 255.0f;
        a.dbPerM = 12.0f + 26.0f * h;
        a.hfDepthM = std::max(0.20f, 0.85f - 0.55f * h);
      }
      break;
  }
  return a;
}

}  // namespace

// Slot -> sound-set namespace. THIS TABLE IS MIRRORED in the `prefix` field of
// assets/sound_schema.js, which is what the tuner offers set lists from; if the
// two disagree the tuner will happily write a binding this code cannot resolve.
// Adding a slot means adding it in both places (and firing it from somewhere).
const std::map<std::string, std::string> Cues::kSlotPrefix = {
    {"footstep", "footsteps"}, {"land", "footsteps"}, {"impact", "impacts"},
    {"break", "breaks"},       {"ambience", "ambience"},
    // Creature slots. All live under mobs/ so one creature's takes stay
    // together in one folder tree.
    {"hurt", "mobs"},          {"death", "mobs"},      {"sever", "mobs"},
    {"idle", "mobs"},          {"alert", "mobs"},      {"attack", "mobs"},
    {"step", "footsteps"},     {"dismember", "mobs"},
    // Bleeding lives under gore/ rather than mobs/: it is the sound of BLOOD,
    // not of a particular creature, and every mob that bleeds wants the same
    // wet bed. Keeping it out of mobs/ is what lets one set serve all of them
    // without pretending it is any one creature's voice.
    {"bleed", "gore"},
    // World beds (see the `world` owner in sound_schema.js). One owner, so the
    // set name is fixed in code; the slot exists here so the tuner and the
    // wiki can describe it.
    {"night", "ambience"},
};

namespace {

// The slot key for a mob event, matching assets/sound_schema.js.
const char* MobSlotName(Cues::MobEvent ev) {
  switch (ev) {
    case Cues::MobEvent::Hurt:   return "hurt";
    case Cues::MobEvent::Death:  return "death";
    case Cues::MobEvent::Sever:  return "sever";
    case Cues::MobEvent::Idle:   return "idle";
    case Cues::MobEvent::Alert:  return "alert";
    case Cues::MobEvent::Attack: return "attack";
    case Cues::MobEvent::Dismember: return "dismember";
  }
  return "";
}

}  // namespace

bool Cues::Init(const std::string& soundDir, const std::vector<MaterialDef>& mats) {
  soundDir_ = soundDir;
  if (!CurrentTuning().audio.enabled) {
    std::printf("[audio] disabled by tuning\n");
    return false;
  }
  if (!device_.Init(kPreferredRate)) return false;

  const double sr = device_.SampleRate();
  const int loaded = lib_.Init(sr, soundDir);
  for (const std::string& w : lib_.Warnings()) std::printf("[audio] %s\n", w.c_str());

  ListenerPose lp;
  world_.Init(sr, lp);
  RebuildMaterialTable(mats);
  ApplyTuning();

  if (!device_.Start(&world_)) return false;
  enabled_ = true;
  std::printf("[audio] %.0f Hz, %d sets, %d samples\n", sr, lib_.Count(), loaded);
  return true;
}

void Cues::Shutdown() {
  // Stop the device FIRST: it is the only thing that can be inside Render()
  // while this object is torn down.
  device_.Stop();
  enabled_ = false;
}

int Cues::ResolveMaterialSlot(const MaterialDef& m, const char* slot) const {
  const std::string& authored = m.Sound(slot);
  // An authored value containing '/' is already a full set name -- the escape
  // hatch for pointing a slot outside its own namespace. Same rule as
  // soundSetName() in assets/sound_schema.js; the two must agree or the tuner
  // will bind names the engine cannot resolve.
  if (!authored.empty()) {
    const std::string full = authored.find('/') != std::string::npos
                                 ? authored
                                 : std::string(kSlotPrefix.at(slot)) + "/" + authored;
    return lib_.Find(full);
  }
  return -1;
}

void Cues::RebuildMaterialTable(const std::vector<MaterialDef>& mats) {
  warnings_.clear();
  mobSetCache_.clear();  // a rescan may have added the set a mob asked for
  const size_t n = mats.size();
  footstep_.assign(n, FootstepMapping{});
  land_.assign(n, FootstepMapping{});
  impact_.assign(n, FootstepMapping{});
  break_.assign(n, FootstepMapping{});
  acoustics_.assign(n, MaterialAcoustics{});
  lastVariant_.assign((size_t)std::max(1, lib_.Count()), -1);

  for (size_t i = 0; i < n; i++) {
    const MaterialDef& m = mats[i];
    acoustics_[i] = AcousticsFor(m);
    if (i == 0) continue;  // air

    // Footsteps: authored name wins; otherwise fall back by tag. The fallback
    // is what keeps a brand-new material audible without touching audio code.
    std::string name = m.Sound("footstep");
    const bool authored = !name.empty();
    if (!authored) name = FallbackFootstep(m);

    int stepId = -1;
    if (!name.empty()) {  // empty = deliberately silent (liquids, gases)
      stepId = lib_.Find(name.find('/') != std::string::npos
                             ? name
                             : "footsteps/" + name);
      if (stepId < 0 && authored) {
        // Diagnostics, not breakage (DESIGN.md §6): an unrecorded surface is
        // silent, and the game still runs. Only complain about names someone
        // actually wrote -- a fallback pointing at a set that does not exist
        // yet is the expected state, not an error.
        warnings_.push_back("material \"" + m.name + "\": no sound set \"" +
                            name + "\" for slot footstep");
      }
    }
    footstep_[i].setId = stepId;
    footstep_[i].gain = 1.0f;

    // The other slots each fall back to the footstep set, because a body
    // hitting stone and a boot hitting stone are the same surface -- pitch and
    // gain separate them well enough to be worth having before dedicated sets
    // are recorded. A slot authored to a set that does not exist reports it
    // and then falls back, rather than going silently quiet.
    auto slotOr = [&](const char* slot, std::vector<FootstepMapping>& table) {
      const int id = ResolveMaterialSlot(m, slot);
      if (id < 0 && !m.Sound(slot).empty())
        warnings_.push_back("material \"" + m.name + "\": no sound set \"" +
                            m.Sound(slot) + "\" for slot " + slot);
      table[i].setId = id >= 0 ? id : stepId;
      table[i].gain = 1.0f;
    };
    slotOr("land", land_);
    slotOr("impact", impact_);
    // Breaking has no sensible surrogate: a step is not a shatter. Silent
    // until a set is bound.
    const int bid = ResolveMaterialSlot(m, "break");
    break_[i].setId = bid;
    break_[i].gain = 1.0f;
  }

  if ((int)lastVariant_.size() < lib_.Count()) lastVariant_.assign((size_t)lib_.Count(), -1);
  for (const std::string& w : warnings_) std::printf("[audio] %s\n", w.c_str());
}

int Cues::RescanSounds(const std::vector<MaterialDef>& mats) {
  const int n = lib_.Rescan();
  for (const std::string& w : lib_.Warnings()) std::printf("[audio] %s\n", w.c_str());
  RebuildMaterialTable(mats);
  return n;
}

void Cues::ApplyTuning() {
  const Tuning::Audio& t = CurrentTuning().audio;
  world_.SetMasterGain(std::clamp(t.masterVolume, 0.0f, 2.0f));

  OcclusionTuning ot;
  ot.enabled = t.occlusion;
  ot.maxAttenDb = t.occlusionMaxDb;
  ot.minCutoffHz = t.occlusionMinCutoffHz;
  ot.attenScale = t.occlusionScale;
  ot.cutoffScale = t.occlusionCutoffScale;
  ot.maxRayM = t.occlusionMaxRangeM;
  ot.wetKeep = t.occlusionWetKeep;
  world_.SetOcclusionTuning(ot);
}

void Cues::Update(float dt, const Vec3& listenerPosVox, float yaw, float pitch,
                  World* world) {
  if (!enabled_) return;
  now_ += (double)dt;
  // The per-source voice map is keyed by mob handle, and mobs die. Drop entries
  // older than the gap they enforce: after that they can never suppress
  // anything, so keeping them is pure growth (CLAUDE.md rule 2 — bound every
  // emergent process, including bookkeeping).
  if (lastMobVoice_.size() > 64) {
    for (auto it = lastMobVoice_.begin(); it != lastMobVoice_.end();)
      it = (now_ - it->second > kMobVoiceMinGap) ? lastMobVoice_.erase(it)
                                                 : std::next(it);
  }
  // Tuning is read HERE, on the game thread, and pushed down. The audio thread
  // never reads CurrentTuning() -- F5 swaps that global wholesale.
  ApplyTuning();
  // Republished every frame because a materials hot-reload can change it.
  world_.SetAcoustics(acoustics_);

  // Close out any bleed loop whose wound was not reported this frame. Runs
  // AFTER the game has had its say (Update is called at the end of the frame,
  // past the MobBleed calls) so "not seen" genuinely means "stopped bleeding"
  // and not "not reported yet".
  ReapBleeds();

  ListenerPose lp;
  lp.posVox = listenerPosVox;
  lp.yaw = yaw;
  lp.pitch = pitch;
  world_.Update(lp, world);
}

const std::vector<float>* Cues::PickStep(int setId, int& lastVariant) {
  return lib_.RandomVariantNoRepeat(setId, rng_, lastVariant);
}

void Cues::Footstep(uint32_t matId, const Vec3& posVox, float speed, int foot) {
  if (!enabled_ || matId >= footstep_.size()) return;
  const FootstepMapping& fm = footstep_[matId];
  if (fm.setId < 0) return;
  const Tuning::Audio& t = CurrentTuning().audio;

  // Speed -> loudness. speed arrives in voxels/sec; the tuning speaks m/s.
  const float speedMs = speed * kVoxelMeters;
  const float k = std::clamp(speedMs / std::max(0.5f, t.footstepSprintSpeed), 0.0f, 1.0f);
  const float gain = fm.gain * t.footstepVolume *
                     (t.footstepWalkGain + (t.footstepSprintGain - t.footstepWalkGain) * k);

  // Pitch: random jitter to hide repetition, plus a fixed per-foot offset so
  // the left and right feet are not the same impact twice.
  std::uniform_real_distribution<float> d(-t.footstepPitchJitter, t.footstepPitchJitter);
  const float detune = (foot & 1) ? t.footstepFootDetune : -t.footstepFootDetune;
  const double rate = std::clamp(1.0f + d(rng_) + detune, 0.5f, 2.0f);

  if ((int)lastVariant_.size() <= fm.setId) lastVariant_.assign((size_t)lib_.Count(), -1);
  const std::vector<float>* buf = PickStep(fm.setId, lastVariant_[(size_t)fm.setId]);

  VoiceConfig cfg;
  cfg.gain = gain;
  cfg.audibleRadius = t.footstepRadius;
  cfg.verbWet = t.reverbWet;
  // Doppler off: the step is at the listener's own feet (or a mob's), and its
  // motion is already legible. Leaving it on pitch-bends every step you take
  // while running, which reads as a broken sample.
  cfg.doppler = false;
  cfg.rate = rate;
  if (world_.PlayOneShot(buf, posVox, cfg))
    stats_.steps++;
  else
    stats_.dropped++;
}

void Cues::Land(uint32_t matId, const Vec3& posVox, float fallSpeed) {
  if (!enabled_ || matId >= footstep_.size()) return;
  const FootstepMapping& fm = footstep_[matId];
  if (fm.setId < 0) return;
  const Tuning::Audio& t = CurrentTuning().audio;

  const float speedMs = fallSpeed * kVoxelMeters;
  const float k = std::clamp(speedMs / std::max(1.0f, t.landFullSpeed), 0.0f, 1.0f);

  if ((int)lastVariant_.size() <= fm.setId) lastVariant_.assign((size_t)lib_.Count(), -1);
  const std::vector<float>* buf = PickStep(fm.setId, lastVariant_[(size_t)fm.setId]);

  VoiceConfig cfg;
  cfg.gain = t.landVolume * (0.6f + 0.7f * k);
  cfg.audibleRadius = t.footstepRadius * 1.6f;
  cfg.verbWet = t.reverbWet;
  cfg.doppler = false;
  // Pitched DOWN with impact speed: the same surface hit harder by more mass
  // rings lower, and this is what separates a landing from a step without a
  // second set of samples.
  cfg.rate = 1.0f - 0.22f * k;
  if (world_.PlayOneShot(buf, posVox, cfg)) stats_.lands++;
}

void Cues::Impact(uint32_t matId, const Vec3& posVox, float energy) {
  if (!enabled_ || matId >= impact_.size()) return;
  const FootstepMapping& im = impact_[matId];
  if (im.setId < 0) return;
  const Tuning::Audio& t = CurrentTuning().audio;
  const float k = std::clamp(energy, 0.0f, 1.0f);

  if ((int)lastVariant_.size() <= im.setId) lastVariant_.assign((size_t)lib_.Count(), -1);
  const std::vector<float>* buf = PickStep(im.setId, lastVariant_[(size_t)im.setId]);

  VoiceConfig cfg;
  cfg.gain = t.impactVolume * (0.35f + 0.85f * k);
  cfg.audibleRadius = t.impactRadius;
  cfg.verbWet = t.reverbWet;
  cfg.doppler = false;
  cfg.rate = 1.15f - 0.35f * k;  // heavier impact, lower pitch
  if (world_.PlayOneShot(buf, posVox, cfg)) stats_.impacts++;
}

void Cues::Break(uint32_t matId, const Vec3& posVox, int sizeVoxels) {
  if (!enabled_ || matId >= break_.size()) return;
  const FootstepMapping& bm = break_[matId];
  // No surrogate: a step is not a shatter, so an unbound material is silent
  // (see RebuildMaterialTable). This is the early-out that keeps the cue free
  // for the 50-odd materials with nothing recorded yet.
  if (bm.setId < 0) return;
  const Tuning::Audio& t = CurrentTuning().audio;

  // Size -> pitch centre. sqrt, not linear: perceived size goes with a piece's
  // linear dimension rather than its volume, so a 400-voxel island and a
  // 40-voxel one should not be a full mapping apart.
  const float big = std::max(1.0f, t.breakBigVoxels);
  const float k =
      std::clamp(std::sqrt((float)std::max(0, sizeVoxels) / big), 0.0f, 1.0f);
  const float centre = t.breakSmallRate + (t.breakBigRate - t.breakSmallRate) * k;

  // Random detune in SEMITONES, converted to a rate multiplier. Uniform in
  // semitones (not in rate) so the shift is symmetric to the ear: ±5 st is
  // ×1.335 up but only ×0.749 down, and randomizing the rate directly would
  // bias every break upward.
  std::uniform_real_distribution<float> d(-t.breakPitchSemitones,
                                          t.breakPitchSemitones);
  const float rate = centre * std::pow(2.0f, d(rng_) / 12.0f);

  if ((int)lastVariant_.size() <= bm.setId) lastVariant_.assign((size_t)lib_.Count(), -1);
  const std::vector<float>* buf = PickStep(bm.setId, lastVariant_[(size_t)bm.setId]);

  VoiceConfig cfg;
  cfg.gain = t.breakVolume * bm.gain * (0.7f + 0.5f * k);
  cfg.audibleRadius = t.breakRadius;
  cfg.verbWet = t.reverbWet;
  // Doppler off: the piece is not moving yet at the instant it breaks — it
  // detaches at rest and only then falls.
  cfg.doppler = false;
  cfg.rate = std::clamp(rate, 0.25f, 4.0f);
  if (world_.PlayOneShot(buf, posVox, cfg))
    stats_.breaks++;
  else
    stats_.dropped++;
}

int Cues::MobSetId(const MobDef& def, MobEvent ev) const {
  const char* slot = MobSlotName(ev);
  const std::string& authored = def.Sound(slot);
  std::string name = authored;
  // Severing falls back to hurt: dismemberment without its own take should
  // still make the creature cry out, and it is the one mob slot where the
  // surrogate is clearly right. Every other slot is silent when unbound.
  if (name.empty() && ev == MobEvent::Sever) name = def.Sound("hurt");
  if (name.empty()) return -1;

  // Memoised on the resolved SET NAME, not on the def: two mobs sharing a set
  // share the lookup, and a def whose sidecar was hot-reloaded re-resolves
  // through the fresh string.
  const std::string full =
      name.find('/') != std::string::npos
          ? name
          : std::string(kSlotPrefix.at(slot)) + "/" + name;
  auto it = mobSetCache_.find(full);
  if (it != mobSetCache_.end()) return it->second;
  const int id = lib_.Find(full);
  mobSetCache_[full] = id;
  if (id < 0)
    std::printf("[audio] mob \"%s\": no sound set \"%s\" for slot %s\n",
                def.name.c_str(), full.c_str(), slot);
  return id;
}

void Cues::MobSound(const MobDef& def, MobEvent ev, const Vec3& posVox,
                    float intensity, uint64_t sourceId) {
  if (!enabled_) return;
  const int setId = MobSetId(def, ev);
  if (setId < 0) return;
  const Tuning::Audio& t = CurrentTuning().audio;
  const float k = std::clamp(intensity, 0.0f, 1.0f);

  // ONE VOICE PER SOURCE PER WINDOW. A body taking a burst of laser damage
  // generates a damage event per tick; without this a single hit reads as a
  // machine-gun of overlapping copies of the same sample, which is both the
  // loudest possible bug and a straight waste of the voice pool. Death, Sever
  // and Dismember are state changes that happen once, so they bypass it -- and
  // a blow that takes two limbs at once SHOULD be two cuts.
  const bool limited = (ev == MobEvent::Hurt || ev == MobEvent::Attack ||
                        ev == MobEvent::Alert || ev == MobEvent::Idle);
  if (limited && sourceId) {
    auto it = lastMobVoice_.find(sourceId);
    if (it != lastMobVoice_.end() && now_ - it->second < kMobVoiceMinGap) {
      stats_.dropped++;
      return;
    }
    lastMobVoice_[sourceId] = now_;
  }

  if ((int)lastVariant_.size() <= setId) lastVariant_.assign((size_t)lib_.Count(), -1);
  const std::vector<float>* buf = PickStep(setId, lastVariant_[(size_t)setId]);

  VoiceConfig cfg;
  // Death is always at full weight -- it happens once and it is the event the
  // player most needs to hear. Everything else scales with severity.
  cfg.gain = t.mobVolume * (ev == MobEvent::Death ? 1.0f : 0.45f + 0.75f * k);
  cfg.audibleRadius = t.mobRadius;
  cfg.verbWet = t.reverbWet;
  // Doppler stays off for the same reason it does on footsteps: these are
  // short transients on a body whose motion is already legible, and bending
  // them reads as a broken sample rather than as speed.
  cfg.doppler = false;
  // Pitch: a jitter so repeated hurts are not identical, plus a downward bend
  // with severity -- a harder blow rings lower, exactly as Land and Impact do.
  std::uniform_real_distribution<float> d(-t.mobPitchJitter, t.mobPitchJitter);
  cfg.rate = std::clamp(1.0f + d(rng_) - 0.18f * k, 0.5f, 2.0f);

  if (ev == MobEvent::Dismember) {
    // The cut is a physical event, not a voice, so it takes its own trim and a
    // wider jitter than the creature slots: four takes cover a whole fight and
    // the ear finds the repeat quickly without it. `k` here is blade speed,
    // and a faster cut reads LOWER -- more mass parting at once.
    cfg.gain = t.dismemberVolume * (0.6f + 0.6f * k);
    std::uniform_real_distribution<float> dd(-t.dismemberPitchJitter,
                                             t.dismemberPitchJitter);
    cfg.rate = std::clamp(1.0f + dd(rng_) - 0.15f * k, 0.5f, 2.0f);
  }

  if (world_.PlayOneShot(buf, posVox, cfg))
    stats_.mobs++;
  else
    stats_.dropped++;
}

void Cues::MobBleed(uint64_t sourceId, const Vec3& posVox, float intensity) {
  if (!enabled_) return;
  const Tuning::Audio& t = CurrentTuning().audio;
  const float k = std::clamp(intensity, 0.0f, 1.0f);

  BleedLoop& b = bleeds_[sourceId];
  b.seen = true;

  // Hysteresis: start above `on`, keep going until below `off`. A single
  // threshold makes a wound sitting exactly at it retrigger the voice every
  // frame, which is both audible and a voice-pool leak.
  if (b.handle < 0) {
    if (k < t.bleedOnThreshold) return;
    const int id = lib_.Find("gore/bleed");
    if (id < 0) return;
    b.gain = 0.0f;
    b.handle = StartAmbience("gore/bleed", posVox, 0.0f, t.bleedRadius);
    if (b.handle < 0) return;   // pool full; retried next frame
    stats_.bleeds++;
  }

  // Track the wound. Eased rather than snapped so a drip-by-drip budget does
  // not chatter the gain.
  b.gain += (k * t.bleedVolume - b.gain) * 0.25f;
  world_.SetLoopPos(b.handle, posVox);
  world_.SetLoopGain(b.handle, b.gain);
}

void Cues::ReapBleeds() {
  const Tuning::Audio& t = CurrentTuning().audio;
  for (auto it = bleeds_.begin(); it != bleeds_.end();) {
    BleedLoop& b = it->second;
    // Not reported this frame: the wound closed, or its owner is gone. Fade
    // out rather than cutting, then drop the entry once it is silent.
    if (!b.seen) {
      b.gain *= 0.80f;
      if (b.handle >= 0) {
        if (b.gain < 0.005f) {
          world_.StopLoop(b.handle);
          it = bleeds_.erase(it);
          continue;
        }
        world_.SetLoopGain(b.handle, b.gain);
      } else {
        it = bleeds_.erase(it);
        continue;
      }
    } else if (b.handle >= 0 && b.gain < t.bleedOffThreshold * t.bleedVolume) {
      // Still reported, but the wound has slowed past the off-threshold.
      world_.StopLoop(b.handle);
      b.handle = -1;
      b.gain = 0.0f;
    }
    b.seen = false;   // cleared for the next frame; MobBleed sets it again
    ++it;
  }
}

void Cues::SetNightAmbience(const Vec3& listenerPosVox, float want,
                            bool allowStart) {
  if (!enabled_) return;
  const Tuning::Audio& t = CurrentTuning().audio;
  const float k = std::clamp(want, 0.0f, 1.0f);

  if (!nightPlaying_) {
    // Only a caller-approved roll starts a pass -- that is where the rarity
    // lives. Deliberately checked before the set lookup so a project with no
    // night bed costs nothing here.
    if (!allowStart || k <= 0.0f) return;
    const int id = lib_.Find("ambience/starlight");
    if (id < 0) return;
    nightGain_ = 0.0f;
    // Centred ON the listener: this is a non-diegetic bed, not a thing at a
    // place, so it must not pan or occlude as the player turns. Occlusion
    // scale 0 for the same reason -- a wall between you and "the night" is
    // not a meaningful idea.
    nightHandle_ = StartAmbience("ambience/starlight", listenerPosVox, 0.0f,
                                 t.nightRadius, 0.0f /*no occlusion*/);
    if (nightHandle_ < 0) return;
    nightPlaying_ = true;
  }

  // Ease toward the target. Slow on purpose: this bed should arrive and leave
  // without the player catching the moment it did either.
  nightGain_ += (k * t.nightVolume - nightGain_) * t.nightFadeRate;
  world_.SetLoopPos(nightHandle_, listenerPosVox);
  world_.SetLoopGain(nightHandle_, nightGain_);

  // Daybreak, or the pass was told to end: once faded out, release the voice
  // so the pool is free and the next night rolls fresh.
  if (k <= 0.0f && nightGain_ < 0.004f) {
    world_.StopLoop(nightHandle_);
    nightHandle_ = -1;
    nightPlaying_ = false;
    nightGain_ = 0.0f;
  }
}

int Cues::StartAmbience(const std::string& setName, const Vec3& posVox, float gain,
                        float radius, float occlusionScale) {
  if (!enabled_) return -1;
  const int id = lib_.Find(setName);
  if (id < 0) return -1;
  // Stable variant per handle would be better once sets have several; index 0
  // is deterministic and enough while ambience sets are single-variant.
  const std::vector<float>* buf = lib_.Variant(id, 0);
  VoiceConfig cfg;
  cfg.gain = gain;
  cfg.audibleRadius = radius;
  cfg.verbWet = CurrentTuning().audio.reverbWet;
  cfg.doppler = false;
  cfg.occlusionScale = occlusionScale;
  return world_.PlayLoop(buf, posVox, cfg);
}

void Cues::MoveAmbience(int handle, const Vec3& posVox) {
  if (enabled_) world_.SetLoopPos(handle, posVox);
}
void Cues::SetAmbienceGain(int handle, float gain) {
  if (enabled_) world_.SetLoopGain(handle, gain);
}
void Cues::StopAmbience(int handle) {
  if (enabled_) world_.StopLoop(handle);
}

}  // namespace audio
