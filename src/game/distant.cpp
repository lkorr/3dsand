// distant.cpp — the distant tier (game/distant.h, docs/PLAN_distant_npcs.md).
//
// Two halves in one file, because they are one seam:
//   * the container and the pure pieces (DistantMobs, the ground classifier,
//     the overlay a DistantMob carries, the run totals);
//   * MobSystem's side of the borders: the demote branch of the park rule,
//     the retire scan, the promotion, and the either-tier liveness questions
//     the §2.14 audit found had to stop answering "gone" for a creature that
//     only walked out of the window.
// PatchMobRecord and Mob::ApplyDistantOverlay live in mob.cpp, beside the
// record reader and the dead-state code they are built on.

#include "game/distant.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "game/mob.h"
#include "sim/tuning.h"
#include "sim/world.h"
#include "sim/worlddefaults.h"  // kDefaultSeed: the seed the ground answers for
#include "sim/worldmap.h"

namespace {

DistantMobs::Stats& RunTotalsMut() {
  static DistantMobs::Stats s;
  return s;
}

// The window's centre, world voxels: what "near the player" means to this
// tier. The window is centred on the primary interest point with two chunks
// of hysteresis (stream.cpp), so this is within ~3 chunks of the player --
// immaterial at a 128 m radius -- and, unlike the player's position, it is
// the same answer in the game and in a gate whose window is pinned.
Vec3 WindowCentre(Vec3 wlo) {
  const float h = 0.5f * (float)kWorldN;
  return Vec3{wlo.x + h, wlo.y + h, wlo.z + h};
}

float DistSq(Vec3 a, Vec3 b) {
  const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return dx * dx + dy * dy + dz * dz;
}

}  // namespace

const DistantMobs::Stats& DistantRunTotals() { return RunTotalsMut(); }

const char* DistantGroundName(DistantGround g) {
  return g == DistantGround::Anchored ? "anchored" : "surface";
}

// ---- the ground (§2.4) -------------------------------------------------------

bool InPlacedDungeonBox(IVec3 c) {
  for (const worldmap::DungeonDef& g : worldmap::CurrentWorldMap().dungeons) {
    if (!g.Placed()) continue;
    if (c.x >= g.boxMin[0] && c.x <= g.boxMax[0] && c.y >= g.boxMin[1] &&
        c.y <= g.boxMax[1] && c.z >= g.boxMin[2] && c.z <= g.boxMax[2])
      return true;
  }
  return false;
}

DistantGround ClassifyDistantGround(Vec3 origin, Vec3 worldSize,
                                    float surfaceSnapMaxVox, uint32_t seed,
                                    float* gapOut) {
  // The footprint's centre column, at the feet.
  const int cx = (int)std::floor(origin.x + worldSize.x * 0.5f);
  const int cz = (int)std::floor(origin.z + worldSize.z * 0.5f);
  const int fy = (int)std::floor(origin.y);
  // TerrainHeight is the top solid cell of the column, so a body standing on
  // it has its feet one cell up: the `+ 1` every spawn onto terrain uses.
  const int h = World::TerrainHeight(cx, cz, seed);
  const float gap = origin.y - (float)(h + 1);
  if (gapOut != nullptr) *gapOut = gap;
  // `!(<=)` rather than `>`: a NaN gap is not a surface.
  if (!(std::fabs(gap) <= surfaceSnapMaxVox)) return DistantGround::Anchored;
  // A dungeon is carved UNDER the surface, but its box may reach up through
  // it (an entrance): a creature standing in one is the dungeon's.
  if (InPlacedDungeonBox(IVec3{cx, fy, cz})) return DistantGround::Anchored;
  return DistantGround::Surface;
}

// ---- one creature ------------------------------------------------------------

DistantOverlay DistantMob::Overlay() const {
  DistantOverlay o;
  o.origin = origin;
  o.heading = steer.heading;
  o.worldSize = worldSize;
  o.limbHp = limbHp;
  o.dead = dead;
  o.cause = dead ? cause : std::string();
  o.deathSeq = dead ? deathSeq : 0;
  o.refId = refId;
  return o;
}

// ---- the container -------------------------------------------------------------

size_t DistantMobs::LowerBound(uint64_t id) const {
  size_t lo = 0, hi = list_.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (list_[mid].id < id) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

size_t DistantMobs::LivingCount() const {
  size_t n = 0;
  for (const DistantMob& d : list_) n += d.dead ? 0u : 1u;
  return n;
}

DistantMob* DistantMobs::Find(uint64_t id) {
  const size_t i = LowerBound(id);
  return i < list_.size() && list_[i].id == id ? &list_[i] : nullptr;
}

const DistantMob* DistantMobs::Find(uint64_t id) const {
  const size_t i = LowerBound(id);
  return i < list_.size() && list_[i].id == id ? &list_[i] : nullptr;
}

DistantMob* DistantMobs::FindByRef(const std::string& refId) {
  if (refId.empty()) return nullptr;
  for (DistantMob& d : list_)
    if (d.refId == refId) return &d;
  return nullptr;
}

const DistantMob* DistantMobs::FindByRef(const std::string& refId) const {
  if (refId.empty()) return nullptr;
  for (const DistantMob& d : list_)
    if (d.refId == refId) return &d;
  return nullptr;
}

bool DistantMobs::Insert(DistantMob&& d) {
  if (!HasRoom() || d.id == 0) return false;
  const size_t i = LowerBound(d.id);
  if (i < list_.size() && list_[i].id == d.id) return false;
  list_.insert(list_.begin() + (ptrdiff_t)i, std::move(d));
  stats_.peak = std::max(stats_.peak, (uint32_t)list_.size());
  RunTotalsMut().peak = std::max(RunTotalsMut().peak, (uint32_t)list_.size());
  // A new creature is never over the promotion line (it left the window by
  // the park pad), but the scan latch is cheap and keeps the rule simple:
  // anything that changes the tier says so.
  promoteScanDue_ = true;
  return true;
}

bool DistantMobs::Take(uint64_t id, DistantMob& out) {
  const size_t i = LowerBound(id);
  if (i >= list_.size() || list_[i].id != id) return false;
  out = std::move(list_[i]);
  list_.erase(list_.begin() + (ptrdiff_t)i);
  return true;
}

bool DistantMobs::Erase(uint64_t id) {
  const size_t i = LowerBound(id);
  if (i >= list_.size() || list_[i].id != id) return false;
  list_.erase(list_.begin() + (ptrdiff_t)i);
  return true;
}

void DistantMobs::Clear() {
  list_.clear();
  promoteScanDue_ = retireScanDue_ = false;
}

void DistantMobs::PromotionCandidates(IVec3 wo, int marginChunks,
                                      std::vector<uint64_t>& out) const {
  out.clear();
  const int n = (int)(kWorldN / kChunk);
  const int m = marginChunks;
  auto chunkOf = [](float v) { return (int)std::floor((double)v / (double)kChunk); };
  const float h = 0.5f * (float)kWorldN;
  const Vec3 centre{(float)(wo.x * (int)kChunk) + h, (float)(wo.y * (int)kChunk) + h,
                    (float)(wo.z * (int)kChunk) + h};
  std::vector<std::pair<float, uint64_t>> cands;
  for (const DistantMob& d : list_) {
    const int cx = chunkOf(d.origin.x), cy = chunkOf(d.origin.y), cz = chunkOf(d.origin.z);
    if (!(cx >= wo.x + m && cx < wo.x + n - m && cy >= wo.y + m && cy < wo.y + n - m &&
          cz >= wo.z + m && cz < wo.z + n - m))
      continue;
    cands.push_back({DistSq(d.Centre(), centre), d.id});
  }
  // (distance, id): a total order, so the service order is a pure function
  // of the tier and the window.
  std::sort(cands.begin(), cands.end());
  out.reserve(cands.size());
  for (const auto& c : cands) out.push_back(c.second);
}

void DistantMobs::Note(uint64_t Stats::*field, uint64_t n) {
  stats_.*field += n;
  RunTotalsMut().*field += n;
}

// ============================================================================
// MobSystem's side of the borders
// ============================================================================

// ---- NEAR -> DISTANT: the demote branch of the park rule (§2.1) -------------
bool MobSystem::DemoteToDistant(Mob& mob, std::vector<uint8_t>& record, Vec3 wlo) {
  const Tuning::Distant& tune = CurrentTuning().distant;
  // THE SWITCH: off, and this function is never more than this line -- the
  // old park path, byte for byte (§1 goal 6).
  if (!tune.enabled) return false;
  // Whose creature: this machine's own (a ghost is the peer's -- the caller
  // already parks only non-ghosts, and a ghost has no business here either),
  // never a body a player drives (a player is never distant, §2.7), and one
  // whose def this system can name.
  if (mob.IsGhost() || mob.PlayerControlled() || mob.def_ == nullptr ||
      mob.defIndex_ < 0 || mob.defIndex_ >= (int)defs_.size())
    return false;
  const MobDef& def = defs_[(size_t)mob.defIndex_];
  const Vec3 centre{mob.origin_.x + def.worldSize.x * 0.5f,
                    mob.origin_.y + def.worldSize.y * 0.5f,
                    mob.origin_.z + def.worldSize.z * 0.5f};
  if (!(DistSq(centre, WindowCentre(wlo)) <= tune.radius * tune.radius)) {
    distant_.Note(&DistantMobs::Stats::outsideRadius);
    return false;
  }
  if (!distant_.HasRoom()) {
    distant_.Note(&DistantMobs::Stats::refusedFull);
    return false;
  }

  DistantMob d;
  d.id = mob.id_;
  d.defIndex = mob.defIndex_;
  d.defName = def.name;
  d.refId = mob.refId_;
  d.origin = mob.origin_;
  d.steer.heading = mob.heading_;
  d.steer.desiredHeading = mob.desiredHeading_;
  d.steer.turnVel = mob.turnVel_;
  d.steer.speedNow = mob.speedNow_;
  // The velocity it left with: the planar drive along the facing (the drive
  // measures speedNow_ along it), the fall, and a lunge's planar carry. D1's
  // mid-air landing reads it; a frozen D0 creature never does.
  d.vel = Vec3{std::sin(mob.heading_) * mob.speedNow_, 0.0f,
               std::cos(mob.heading_) * mob.speedNow_};
  if (mob.airborne_) {
    d.vel.y = mob.fallVel_;
    d.vel = d.vel + mob.airVel_;
  }
  d.worldSize = def.worldSize;
  d.speed = def.speed;
  d.loco = mob.skel_.loco;
  // ---- THE ABSTRACT BODY: one entry per RECORD limb ----------------------
  // Mob::SaveOne's own predicate (v6+): every base limb, then each appended
  // slot that still has a body. The identity patch below is what proves the
  // two walks agree for this creature.
  for (size_t li = 0; li < mob.limbs_.size(); li++) {
    const MobLimb& L = mob.limbs_[li];
    if ((int)li >= mob.baseLimbs_ && L.body == 0) continue;
    d.limbHp.push_back(L.hp);
    DistantLimbRole role;
    if (L.body != 0) role.flags |= kDistantLimbAttached;
    if (li < mob.limbDefs_.size()) {
      const MobLimbDef& ld = mob.limbDefs_[li];
      if (ld.vital) role.flags |= kDistantLimbVital;
      if (ld.severable) role.flags |= kDistantLimbSeverable;
      role.tag = ld.tag;
    }
    if ((int)li == def.rootLimb) role.flags |= kDistantLimbRoot;
    if (mob.IsBloodless((int)li)) role.flags |= kDistantLimbBloodless;
    if (mob.IsWornSlot((int)li)) role.flags |= kDistantLimbWorn;
    if (mob.IsHeldSlot((int)li)) role.flags |= kDistantLimbHeld;
    d.limbRole.push_back(std::move(role));
  }
  d.dead = !mob.alive_;
  d.cause = d.dead && mob.deathCause_ != nullptr ? std::string(mob.deathCause_)
                                                 : std::string();
  d.deathSeq = d.dead ? mob.deathSeq_ : 0;
  d.ground = ClassifyDistantGround(d.origin, d.worldSize, tune.surfaceSnapMaxVox,
                                   sandvox::kDefaultSeed);
  d.demotedTick = tick_;
  d.asleep = true;   // D0: frozen. D1 wakes the living.

  // THE OVERLAY MUST ROUND-TRIP BEFORE THE CREATURE IS TRUSTED TO IT. The
  // overlay was read off the live Mob and the record was written by SaveOne;
  // patching the record with its own overlay has to give back the same bytes,
  // or a promotion / retire / save would write something the creature never
  // was. A creature that fails is parked instead (the old path, no loss) and
  // the fault is logged -- it is a bug in this file or in SaveOne's walk.
  {
    std::vector<uint8_t> probe = record;
    if (!PatchMobRecord(probe, kSaveVersion, d.Overlay()) || probe != record) {
      std::fprintf(stderr,
                   "distant: mob %llu ('%s') did not round-trip its own overlay "
                   "(%zu limbs); parked instead\n",
                   (unsigned long long)mob.id_, def.name.c_str(), d.limbHp.size());
      distant_.Note(&DistantMobs::Stats::failed);
      return false;
    }
  }
  d.record = std::move(record);
  // THE BRAIN MOVES (§2.2): every timer, the path it was walking, the target
  // it was hunting. The rig is released right after this by the caller.
  d.brain = std::move(mob.ai_);
  const bool dead = d.dead;
  const bool anchored = d.ground == DistantGround::Anchored;
  if (!distant_.Insert(std::move(d))) {
    // Room was checked above and ids are unique, so this is unreachable --
    // but a creature must never be lost to it: Insert consumed nothing, so
    // the record and the brain go back where they came from and it parks.
    std::fprintf(stderr, "distant: insert of mob %llu refused; parked instead\n",
                 (unsigned long long)mob.id_);
    record = std::move(d.record);
    mob.ai_ = std::move(d.brain);
    return false;
  }
  distant_.Note(&DistantMobs::Stats::demoted);
  if (dead) distant_.Note(&DistantMobs::Stats::demotedDead);
  if (anchored) distant_.Note(&DistantMobs::Stats::anchored);
  return true;
}

// ---- DISTANT -> PARKED: retire (§2.1) ---------------------------------------
void MobSystem::RetireDistant(Vec3 wlo) {
  if (distant_.Count() == 0) return;
  // Nowhere to put them (a client, a gate without parking): they stay. The
  // scan memory is left as it was, so the next tick with a park function
  // looks again.
  if (!parkFn_) return;
  const Tuning::Distant& tune = CurrentTuning().distant;
  const IVec3 wo{(int)std::floor(wlo.x / (float)kChunk),
                 (int)std::floor(wlo.y / (float)kChunk),
                 (int)std::floor(wlo.z / (float)kChunk)};
  const float reach = tune.radius + tune.hysteresis;
  // What could have moved the border: the window, the tier (D1's drive, via
  // the latch), or the border itself (an F5 that changed radius/hysteresis).
  const bool moved = !distantScanHave_ || wo.x != distantScanOrigin_.x ||
                     wo.y != distantScanOrigin_.y || wo.z != distantScanOrigin_.z ||
                     std::memcmp(&reach, &distantScanReach_, sizeof(float)) != 0;
  // RULE 2: a frozen tier under a still window has nothing to retire. The
  // switch turned off is the exception -- then everybody parks, now.
  if (tune.enabled && !moved && !distant_.RetireScanDue()) return;
  distantScanHave_ = true;
  distantScanOrigin_ = wo;
  distantScanReach_ = reach;
  distant_.ClearRetireScan();
  distant_.Note(&DistantMobs::Stats::retireScans);
  const Vec3 c = WindowCentre(wlo);
  // In ID ORDER, decided over the tier as it stood at the top of the pass.
  std::vector<uint64_t> out;
  for (const DistantMob& d : distant_.All())
    if (!tune.enabled || !(DistSq(d.Centre(), c) <= reach * reach))
      out.push_back(d.id);
  for (uint64_t id : out) {
    DistantMob d;
    if (!distant_.Take(id, d)) continue;
    std::vector<uint8_t> bytes;
    // The id and the brain are dropped here: a parked creature has neither
    // (today's rule; a save/load drops them the same way).
    if (DistantRecord(d, bytes) && parkFn_(d.origin, bytes)) {
      parkedTotal_++;
      distant_.Note(&DistantMobs::Stats::retired);
    } else {
      std::fprintf(stderr,
                   "distant: retiring mob %llu ('%s') failed (record or park "
                   "function refused); it is lost\n",
                   (unsigned long long)d.id, d.defName.c_str());
      distant_.Note(&DistantMobs::Stats::failed);
    }
  }
}

// ---- DISTANT -> NEAR: promote (§2.1) ----------------------------------------
Mob* MobSystem::PromoteDistant(uint64_t id, bool* spawnRefused) {
  if (spawnRefused) *spawnRefused = false;
  const DistantMob* d = distant_.Find(id);
  if (d == nullptr) return nullptr;
  std::vector<uint8_t> bytes;
  if (!DistantRecord(*d, bytes)) {
    std::fprintf(stderr, "distant: mob %llu ('%s') has a record the overlay "
                         "will not patch; dropped\n",
                 (unsigned long long)id, d->defName.c_str());
    distant_.Note(&DistantMobs::Stats::failed);
    distant_.Erase(id);
    return nullptr;
  }
  const Vec3 wantOrigin = d->origin;
  ByteReader rd{bytes.data(), bytes.size()};
  bool refused = false;
  // placeLimbs: it comes back in the pose it left in (an unpark's rule), the
  // corpse on the pose it was lying in. catchUp: see mob.h PromoteDistant.
  // keepId: the id the tier held.
  Mob* m = LoadOne(rd, kSaveVersion, /*placeLimbs=*/true, &refused,
                   /*catchUp=*/true, /*keepId=*/id);
  // LoadOne reads the tier (its duplicate-ref test) and writes nothing in it,
  // but look the creature up again rather than trust a pointer across it.
  DistantMob* dd = distant_.Find(id);
  if (m == nullptr || dd == nullptr) {
    if (refused && dd != nullptr) {
      if (spawnRefused) *spawnRefused = true;
      distant_.Note(&DistantMobs::Stats::spawnWaits);
      return nullptr;
    }
    // For good (def retired, a ref that already has a near body): the
    // record path's rule -- dropped, and LoadOne said why.
    distant_.Note(&DistantMobs::Stats::failed);
    distant_.Erase(id);
    return nullptr;
  }
  if (m->alive_) {
    // THE BRAIN MOVES BACK (§2.2), whole: the record's five brain facts that
    // LoadOne put on a fresh brain are a subset of it.
    m->ai_ = std::move(dd->brain);
    // ...and the turn it was in, unless the catch-up placer stood it
    // somewhere else (a villager comes back where its day says, facing as
    // the placer turned it).
    if (std::memcmp(&m->origin_, &wantOrigin, sizeof(Vec3)) == 0) {
      m->heading_ = dd->steer.heading;
      m->desiredHeading_ = dd->steer.desiredHeading;
      m->turnVel_ = dd->steer.turnVel;
      m->speedNow_ = dd->steer.speedNow;
    }
  }
  m->ApplyDistantOverlay(*dd);
  const bool dead = dd->dead;
  distant_.Erase(id);
  distant_.Note(&DistantMobs::Stats::promoted);
  if (dead) distant_.Note(&DistantMobs::Stats::promotedDead);
  return FindMobById(id);
}

bool MobSystem::DistantRecord(const DistantMob& d, std::vector<uint8_t>& out) const {
  std::vector<uint8_t> bytes = d.record;
  if (!PatchMobRecord(bytes, kSaveVersion, d.Overlay())) return false;
  out = std::move(bytes);
  return true;
}

// ---- either tier: liveness and identity (§2.14) -------------------------------

MobSystem::CreatureWhere MobSystem::LocateCreature(uint64_t id) const {
  CreatureWhere w;
  if (id == 0) return w;
  if (const Mob* m = FindCreature(id)) {
    w.exists = true;
    w.alive = m->alive_;
    w.id = id;
    return w;
  }
  if (const DistantMob* d = distant_.Find(id)) {
    w.exists = true;
    w.distant = true;
    w.alive = !d->dead;
    w.id = id;
  }
  return w;
}

MobSystem::CreatureWhere MobSystem::LocateByRef(const std::string& refId) const {
  CreatureWhere w;
  if (refId.empty()) return w;
  for (const Mob& m : mobs_)
    if (m.RefId() == refId) {
      w.exists = true;
      w.alive = m.alive_;
      w.id = m.id_;
      return w;
    }
  if (const DistantMob* d = distant_.FindByRef(refId)) {
    w.exists = true;
    w.distant = true;
    w.alive = !d->dead;
    w.id = d->id;
  }
  return w;
}

bool MobSystem::IdInUse(uint64_t id) const {
  if (id == 0) return false;
  for (const Mob& m : mobs_)
    if (m.id_ == id) return true;
  return distant_.Find(id) != nullptr;
}

bool MobSystem::SetDistantRefId(uint64_t id, const std::string& refId) {
  DistantMob* d = distant_.Find(id);
  if (d == nullptr) return false;
  d->refId = refId;   // DistantMob::Overlay carries it into every patched record
  return true;
}

ai::Brain* MobSystem::BrainAnyTierMut(uint64_t id) {
  if (ai::Brain* b = MobBrainMut(id)) return b;
  if (DistantMob* d = distant_.Find(id)) return &d->brain;
  return nullptr;
}

bool MobSystem::SetBehaviorAnyTier(uint64_t mobId, const std::string& name) {
  if (FindMobById(mobId) != nullptr) return SetMobBehavior(mobId, name);
  DistantMob* d = distant_.Find(mobId);
  if (d == nullptr) return false;
  const int p = name.empty() ? -1 : behaviors_.Find(name);
  if (!name.empty() && p < 0) return false;
  // SetMobBehavior's rule: a fresh Brain, not a profile swap on the old one.
  d->brain = ai::Brain(p);
  return true;
}
