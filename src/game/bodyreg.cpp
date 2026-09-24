#include "game/bodyreg.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>

// The walk order — debris, then the avatar's parts, then mob limbs — appears
// exactly three times in this file and nowhere else in the codebase.
//
// THE AVATAR BEFORE THE MOBS (2026-09-24, PLAN_corpse_is_a_mob.md). Every walk
// gives up at kMaxBodySlots, so whoever is walked LAST is who a crowded frame
// starves. Corpses are Mobs now and stay rigs for as long as the dead cap lets
// them, so the mob walk is the one that grows; the player's own body must not
// be the one to vanish when it does. Each system's
// Append* takes the base its slots start at; the bases are derived here from
// the same counts every time, so the three arrays cannot disagree.
//
// THE BASE IS WHAT THE WALK EMITS, NOT WHAT THE SYSTEM CONTAINS. Until
// 2026-09-16 the mob base was `debris_.BodyCount()` while all three debris
// walks stop at `kMaxBodies` — and `DebrisSystem::AdoptBody` takes no cap, so
// `bodies_` runs past that ceiling every time a corpse hands over fifteen
// limbs at once and stays there until the next PostStep cull. In that window
// the base overstates the transforms debris actually wrote, so every mob limb
// and every avatar part declared a slot belonging to a DIFFERENT body: your
// arm drawn at a corpse's head, a zombie's torso drawn where your foot is.
// `SlotCount()` is the emitted count and is the only thing a base may be built
// from. See the audit below, which fails loudly if the two ever part again.

void BodyRegistry::BuildXforms(std::vector<BodyXformGpu>& out) const {
  debris_.BuildXforms(out);  // clears + fills [0, SlotCount)
  if (avatar_) avatar_->AppendXforms(out);
  mobs_.AppendXforms(out);
}

void BodyRegistry::BuildHandles(std::vector<uint64_t>& out) const {
  out.clear();
  const uint32_t d = debris_.SlotCount();
  for (uint32_t s = 0; s < d; s++) out.push_back(debris_.BodyHandle(s));
  if (avatar_) avatar_->AppendBodyHandles(out);
  mobs_.AppendBodyHandles(out);
}

void BodyRegistry::BuildInstances(std::vector<BodyVoxInst>& out) {
  debris_.BuildInstances(out);  // clears + fills, slots [0, SlotCount)
  const uint32_t avatarBase = debris_.SlotCount();
  const uint32_t mobBase =
      avatar_ ? avatar_->AppendInstances(out, avatarBase) : avatarBase;
  mobs_.AppendInstances(out, mobBase);
}

void BodyRegistry::BuildMicroInsts(std::vector<MicroBodyInstGpu>& out) const {
  out.clear();
  debris_.AppendMicroInsts(out);
  const uint32_t debrisEnd = (uint32_t)out.size();
  const uint32_t avatarBase = debris_.SlotCount();
  // The mobs' base is where the AVATAR WALK ACTUALLY STOPPED, not
  // `avatarBase + LimbBodyCount()`: the walk gives up at kMaxBodySlots and the
  // count does not, so on a crowded frame the two differ and the mob limbs
  // would be laid over somebody else's transforms.
  const uint32_t mobBase =
      avatar_ ? avatar_->AppendMicroInsts(out, avatarBase) : avatarBase;
  const uint32_t avatarEnd = (uint32_t)out.size();
  mobs_.AppendMicroInsts(out, mobBase);

  // ---- ALIASING DIAGNOSTIC --------------------------------------------------
  // Two micro instances sharing the same OWNED model draw from one brick:
  // the first edit overwrites the other's art, and the first free zeroes
  // its dims — an invisible limb, or a limb wearing another body's shape.
  // That is the failure MicroBodyClone was written to prevent (see its
  // header). Catching it HERE, in the compacted draw list, names the TWO
  // bodies involved and which system each one came from.
  if (microSet_) {
    std::unordered_map<uint32_t, uint32_t> seen;
    seen.reserve(out.size());
    auto region = [&](uint32_t idx) -> const char* {
      if (idx < debrisEnd) return "debris";
      if (idx < avatarEnd) return "avatar";
      return "mob";
    };
    for (uint32_t i = 0; i < (uint32_t)out.size(); i++) {
      const uint32_t m = out[i].model;
      if (m == kMicroBodyNoModel) continue;
      if (m >= microSet_->owned.size() || !microSet_->owned[m]) continue;
      auto it = seen.find(m);
      if (it != seen.end()) {
        Report("MICRO MODEL ALIAS: owned model %u drawn by inst %u "
               "(%s, slot %u) AND inst %u (%s, slot %u)",
               m, it->second, region(it->second), out[it->second].slot,
               i, region(i), out[i].slot);
      } else {
        seen[m] = i;
      }
    }
  }

  // ---- THE SLOT SPACE ITSELF ------------------------------------------------
  // Model aliasing is one of two ways a limb can wear another body's shape;
  // this is the other, and no amount of looking at the model table finds it.
  // An instance is (slot, model) and the shader draws `model` at
  // `bodyXforms[slot]`, so a slot that belongs to somebody else swaps the two
  // bodies on screen just as visibly — and a base computed from a count rather
  // than from a walk is how that happens.
  {
    const uint32_t total = TotalSlots();
    std::unordered_set<uint32_t> slots;
    slots.reserve(out.size());
    for (uint32_t i = 0; i < (uint32_t)out.size(); i++) {
      const uint32_t s = out[i].slot;
      if (s >= total || s >= kMaxBodySlots)
        Report("MICRO SLOT OUT OF RANGE: inst %u claims slot %u of %u "
               "(ceiling %u) — it will march a transform nobody wrote",
               i, s, total, kMaxBodySlots);
      if (!slots.insert(s).second)
        Report("MICRO SLOT COLLISION: two instances claim slot %u "
               "(inst %u model %u, and inst %u model %u) — one of these two "
               "bodies is drawn at the other's transform",
               s, i, out[i].model, i, out[i].model);
    }
  }
}

// ---- the full holder audit --------------------------------------------------
//
// The draw-list check above can only see holders that DRAW, and the dangerous
// holder usually does not: a limb keeps `microModel` after a sever hands the
// brick to DebrisSystem, a first-person-suppressed avatar part emits no
// instance, a body past kMaxBodies is a holder the walks skip. Any of those,
// freeing later, hands a live creature's record back to the pool — and the
// next carve anywhere in the world gets it. So this walks every system's FULL
// holder list (sim/microbody.h MicroHolder) and reports in words.
uint32_t BodyRegistry::AuditMicroModels() const {
  if (!microSet_) return 0;
  // Phase 1 is allocation-free and runs on every call: collect the indices,
  // look for a repeat or a record that has been zeroed under its holder. Only
  // a hit pays for phase 2, which builds the descriptions. An audit that cost
  // three hundred std::strings a frame would not be left switched on, and one
  // that is not switched on catches nothing.
  scratch_.clear();
  bool suspect = false;
  auto note = [&](uint32_t m) {
    if (m == kMicroBodyNoModel || m >= microSet_->models.size()) return;
    // A record whose dims are zero has been FREED while this holder still
    // points at it (MicroBodyFree zeroes dims); the holder draws nothing and
    // the record is already back on freeModels for the next body to take.
    if ((microSet_->models[m].dims & kMicroBodyDimsMask) == 0) suspect = true;
    scratch_.push_back(m);
  };
  for (uint32_t i = 0; i < debris_.BodyCount(); i++)
    note(debris_.BodyMicroModel(i));
  if (!suspect) {
    // Mob/avatar holders as bare indices (Mob::AppendMicroModelIds): phase 1
    // used to take their full descriptions — an snprintf and a std::string
    // per limb per frame — only to read the model index back out of them.
    ids_.clear();
    mobs_.AppendMicroModelIds(ids_);
    if (avatar_) avatar_->AppendMicroModelIds(ids_);
    for (uint32_t m : ids_) note(m);
  }
  if (!suspect) {
    std::sort(scratch_.begin(), scratch_.end());
    for (size_t i = 1; i < scratch_.size(); i++)
      if (scratch_[i] == scratch_[i - 1] && scratch_[i] < microSet_->owned.size() &&
          microSet_->owned[scratch_[i]]) {
        suspect = true;
        break;
      }
  }
  if (!suspect) return 0;

  // Phase 2: name everybody.
  holders_.clear();
  debris_.AppendMicroHolders(holders_);
  mobs_.AppendMicroHolders(holders_);
  if (avatar_) avatar_->AppendMicroHolders(holders_);

  uint32_t faults = 0;
  std::unordered_map<uint32_t, uint32_t> first;
  first.reserve(holders_.size());
  for (uint32_t i = 0; i < (uint32_t)holders_.size(); i++) {
    const uint32_t m = holders_[i].model;
    if (m == kMicroBodyNoModel) continue;
    if (m >= microSet_->models.size()) {
      Report("MICRO HOLDER OUT OF RANGE: %s holds model %u of %zu",
             holders_[i].what.c_str(), m, microSet_->models.size());
      faults++;
      continue;
    }
    if ((microSet_->models[m].dims & kMicroBodyDimsMask) == 0) {
      // Freed under its holder. Whoever takes this record next inherits a
      // second, invisible owner that will free it a second time.
      Report("MICRO RECORD FREED UNDER ITS HOLDER: model %u (owned=%u) is held "
             "by %s but its dims are zero",
             m, m < microSet_->owned.size() ? microSet_->owned[m] : 0u,
             holders_[i].what.c_str());
      faults++;
    }
    auto it = first.find(m);
    if (it == first.end()) {
      first[m] = i;
      continue;
    }
    if (m < microSet_->owned.size() && microSet_->owned[m]) {
      Report("MICRO MODEL SHARED BY TWO HOLDERS: owned model %u held by %s AND "
             "%s — one of them is about to go invisible and the record is about "
             "to be handed to a third body",
             m, holders_[it->second].what.c_str(), holders_[i].what.c_str());
      faults++;
    }
  }
  return faults;
}

uint32_t BodyRegistry::MicroHolderCount() const {
  holders_.clear();
  debris_.AppendMicroHolders(holders_);
  mobs_.AppendMicroHolders(holders_);
  if (avatar_) avatar_->AppendMicroHolders(holders_);
  uint32_t n = 0;
  for (const MicroHolder& h : holders_)
    if (h.model != kMicroBodyNoModel) n++;
  return n;
}

void BodyRegistry::ReleaseMicroHolders() {
  // DEBRIS LAST. The other two can hand a body to it on the way out (a sever
  // in flight, a dropped shell), and a body adopted after the sweep would be a
  // holder that outlived the table by one line — the same off-by-one-event
  // that made this bug survive the sever-path fix in the first place.
  mobs_.Reset();
  if (avatar_) avatar_->Despawn();
  debris_.Reset();
}

void BodyRegistry::Report(const char* fmt, ...) const {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  // ONE LINE PER DISTINCT FAULT, not one per frame. A body-swap persists for
  // as long as the two holders live, so an unthrottled report is thousands of
  // identical lines a second — which is the same as no report at all, and is
  // why the first version of this diagnostic went unread.
  static std::unordered_set<std::string> said;
  const std::string key(msg);
  if (!said.insert(key).second) return;
  std::fprintf(stderr, "*** %s\n", msg);
  // ...and to a file, because the report that matters is made while somebody
  // is PLAYING, and a game launched from the tuner or from Explorer has no
  // stderr anybody will ever read.
  if (std::FILE* f = std::fopen("build/microbody_audit.log", "a")) {
    std::fprintf(f, "%s\n", msg);
    std::fclose(f);
  }
}

uint32_t BodyRegistry::TotalSlots() const {
  return debris_.SlotCount() + mobs_.LimbBodyCount() +
         (avatar_ ? avatar_->LimbBodyCount() : 0u);
}

bool BodyRegistry::AnyInstancesDirty() const {
  return debris_.InstancesDirty() || mobs_.InstancesDirty() ||
         (avatar_ && avatar_->InstancesDirty());
}
