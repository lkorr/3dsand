// distant.cpp — the distant tier (game/distant.h, docs/PLAN_distant_npcs.md).
//
// Three parts in one file, because they are one seam:
//   * the container and the pure pieces (DistantMobs and its spatial hash,
//     the ground classifier, the overlay a DistantMob carries, the run
//     totals, the statues' item list);
//   * DistantGround, the ground the tier walks on (D1);
//   * MobSystem's side: the demote branch of the park rule, the retire scan,
//     the promotion (and its snap onto the real ground), the either-tier
//     liveness questions the §2.14 audit found had to stop answering "gone"
//     for a creature that only walked out of the window -- and THE DISTANT
//     TICK (D1: think, steer, space, drive, sleep).
// PatchMobRecord and Mob::ApplyDistantOverlay live in mob.cpp, beside the
// record reader and the dead-state code they are built on.

#include "game/distant.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "game/arrows.h"   // MobShotsLeft: an archer's arrows, cached at demotion
#include "game/mob.h"
#include "game/persist.h"   // EntityParking::kUnparkMarginChunks: the promotion line
#include "sim/materials.h"  // CLASS_SOLID / CLASS_POWDER: what carries weight
#include "sim/rng.h"
#include "sim/scale.h"      // MetresToCells
#include "sim/stream.h"     // Stream::SettledStoredChunk, RleDecodeChunk
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

const char* DistantGroundModeName(DistantGroundMode g) {
  return g == DistantGroundMode::Anchored ? "anchored" : "surface";
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

DistantGroundMode ClassifyDistantGround(Vec3 origin, Vec3 worldSize,
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
  if (!(std::fabs(gap) <= surfaceSnapMaxVox)) return DistantGroundMode::Anchored;
  // A dungeon is carved UNDER the surface, but its box may reach up through
  // it (an entrance): a creature standing in one is the dungeon's.
  if (InPlacedDungeonBox(IVec3{cx, fy, cz})) return DistantGroundMode::Anchored;
  return DistantGroundMode::Surface;
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
  // FILED, and it is an ARRIVAL: every sleeper whose neighbourhood it now
  // stands in wakes (D1). Effective from the next distant tick -- a demotion
  // happens in PreTick's creature loop, after this tick's TickDistant.
  DistantMob& in = list_[i];
  File(in);
  WakeEntered(in.cellX, in.cellZ, /*fromValid=*/false, 0, 0);
  if (!in.dead && !in.asleep) anyAwakeHint_ = true;   // an arrival decides once
  return true;
}

bool DistantMobs::Take(uint64_t id, DistantMob& out) {
  const size_t i = LowerBound(id);
  if (i >= list_.size() || list_[i].id != id) return false;
  Unfile(list_[i]);
  out = std::move(list_[i]);
  list_.erase(list_.begin() + (ptrdiff_t)i);
  return true;
}

bool DistantMobs::Erase(uint64_t id) {
  const size_t i = LowerBound(id);
  if (i >= list_.size() || list_[i].id != id) return false;
  Unfile(list_[i]);
  list_.erase(list_.begin() + (ptrdiff_t)i);
  return true;
}

void DistantMobs::Clear() {
  list_.clear();
  cells_.clear();
  nextWakeTick_ = UINT32_MAX;
  anyAwakeHint_ = false;
  promoteScanDue_ = retireScanDue_ = false;
}

// ---- the spatial hash (§2.11) ---------------------------------------------------

void DistantMobs::File(DistantMob& d) {
  const Vec3 c = d.Centre();
  d.cellX = CellOf(c.x);
  d.cellZ = CellOf(c.z);
  std::vector<uint64_t>& v = cells_[CellKey(d.cellX, d.cellZ)];
  v.insert(std::lower_bound(v.begin(), v.end(), d.id), d.id);
}

void DistantMobs::Unfile(const DistantMob& d) {
  auto it = cells_.find(CellKey(d.cellX, d.cellZ));
  if (it == cells_.end()) return;
  std::vector<uint64_t>& v = it->second;
  auto at = std::lower_bound(v.begin(), v.end(), d.id);
  if (at != v.end() && *at == d.id) v.erase(at);
  if (v.empty()) cells_.erase(it);
}

uint32_t DistantMobs::Relocate(uint64_t id, Vec3 origin) {
  DistantMob* d = Find(id);
  if (d == nullptr) return 0;
  const int ox = d->cellX, oz = d->cellZ;
  d->origin = origin;
  d->movedWhileDistant = true;
  const Vec3 c = d->Centre();
  const int nx = CellOf(c.x), nz = CellOf(c.z);
  if (nx == ox && nz == oz) return 0;
  Unfile(*d);
  File(*d);
  return WakeEntered(nx, nz, /*fromValid=*/true, ox, oz);
}

void DistantMobs::Neighbourhood(int cx, int cz, int radiusCells,
                                std::vector<uint64_t>& out) const {
  out.clear();
  for (int dz = -radiusCells; dz <= radiusCells; dz++)
    for (int dx = -radiusCells; dx <= radiusCells; dx++) {
      auto it = cells_.find(CellKey(cx + dx, cz + dz));
      if (it != cells_.end()) out.insert(out.end(), it->second.begin(), it->second.end());
    }
  // Cells are walked in a fixed order but ids interleave across them: the
  // RESULT is what must be id-sorted (§2.12), not each cell.
  std::sort(out.begin(), out.end());
}

uint32_t DistantMobs::WakeEntered(int cx, int cz, bool fromValid, int fromX, int fromZ) {
  uint32_t woke = 0;
  for (int dz = -1; dz <= 1; dz++)
    for (int dx = -1; dx <= 1; dx++) {
      const int x = cx + dx, z = cz + dz;
      // A cell the mover's OLD neighbourhood already covered: it did not
      // enter that sleeper's neighbourhood, it was in it.
      if (fromValid && std::abs(x - fromX) <= 1 && std::abs(z - fromZ) <= 1) continue;
      auto it = cells_.find(CellKey(x, z));
      if (it == cells_.end()) continue;
      for (const uint64_t id : it->second) {
        DistantMob* s = Find(id);
        if (s == nullptr || s->dead || !s->asleep) continue;
        s->asleep = false;
        s->wakeTick = 0;
        s->wakes++;
        woke++;
      }
    }
  if (woke != 0) {
    Note(&Stats::wakesNeighbour, woke);
    anyAwakeHint_ = true;
  }
  return woke;
}

size_t DistantMobs::AwakeCount() const {
  size_t n = 0;
  for (const DistantMob& d : list_) n += (!d.dead && !d.asleep) ? 1u : 0u;
  return n;
}

void DistantMobs::RefreshNextWake() {
  uint32_t t = UINT32_MAX;
  for (const DistantMob& d : list_)
    if (!d.dead && d.asleep && d.wakeTick != 0) t = std::min(t, d.wakeTick);
  nextWakeTick_ = t;
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

void DistantMobs::NoteMax(uint64_t Stats::*field, uint64_t v) {
  stats_.*field = std::max(stats_.*field, v);
  RunTotalsMut().*field = std::max(RunTotalsMut().*field, v);
}

// ---- the statues' item list (D4a's seam) ----------------------------------------
void FillDistantDrawItems(const DistantMobs& tier, uint32_t tick, int idleStride,
                          std::vector<DistantDrawItem>& out) {
  out.clear();
  const uint32_t walkWindow = (uint32_t)std::max(1, idleStride) + 1u;
  for (const DistantMob& d : tier.All()) {
    if (!d.look.Valid()) continue;
    DistantDrawItem it;
    it.look = d.look;
    it.dead = d.dead;
    it.heading = d.steer.heading;
    it.origin = d.origin;
    // A corpse rests at the height its own lying body reached (the statue's
    // lowest cell), not at its pelvis-following origin.
    if (d.dead) it.origin.y = d.statueRestY;
    // WALKING while it moved within its last stride: a strider moves once
    // every `idleStride` ticks and must not flicker to idle between.
    const bool walking = !d.dead && d.lastMoveTick != 0 && tick >= d.lastMoveTick &&
                         tick - d.lastMoveTick <= walkWindow;
    it.state = walking ? DistantDrawState::Walk : DistantDrawState::Idle;
    it.gaitPhase = d.gaitPhase;
    out.push_back(it);
  }
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
  // ---- D1: what the walk needs from the rig, cached (no def lookups past
  // the edge: the tier does not hold its def, see DistantMob::defIndex).
  d.stepUpCells = mob.StepUpCells();
  d.stepDownCells = mob.StepDownCells();
  d.headroomCells = mob.HeadroomCells();
  d.aquatic = def.swim.present;
  // CanSwimAfloat's predicate, with the tread clip looked up HERE: the pose
  // resolves its swim-clip table on its first animated tick (pose.cpp), so a
  // creature demoted before it ever animated would read "cannot swim".
  d.canFloat = !d.aquatic && (!def.slither.present || def.slither.upright) &&
               mob.skel_.FindClip("tread") >= 0;
  d.swimming = d.canFloat && mob.swimming_;
  d.armed = !mob.HeldItem().empty();
  d.canGuard = (d.armed && mob.HeldSlot() >= 0) || mob.HoldsShield();
  if (items_ != nullptr)
    if (const ai::Profile* pr = behaviors_.At(mob.ai_.profile))
      if (pr->ranged.maxRange > 0.0f) d.ammo = MobShotsLeft(mob, *items_);
  {
    // Mob::BodyFacts' denominator: the def's hp over the base limbs that bleed.
    const int n = std::min({mob.baseLimbs_, (int)mob.limbs_.size(), (int)def.limbs.size()});
    for (int i = 0; i < n; i++)
      if (!mob.IsBloodless(i)) d.hpFull += std::max(0.0f, def.limbs[(size_t)i].hp);
    d.baseLimbs = mob.baseLimbs_;
  }
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
  d.groundMode = ClassifyDistantGround(d.origin, d.worldSize, tune.surfaceSnapMaxVox,
                                       sandvox::kDefaultSeed);
  d.demotedTick = tick_;
  // THE LIVING ARRIVE AWAKE (D1): a demotion decides once -- think, and
  // sleep if there is nothing to do -- before it may cost nothing. A corpse
  // is asleep forever (§2.8).
  d.asleep = d.dead;
  // A SURFACE creature that left IN THE AIR (thrown, falling off the
  // window's bottom face) lands on its first distant step, with the fall
  // damage the drop and its vertical speed earn (§2.4). An ANCHORED one
  // holds where it was, as anchored creatures do.
  d.landPending = !d.dead && d.groundMode == DistantGroundMode::Surface && mob.airborne_;

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
  const bool anchored = d.groundMode == DistantGroundMode::Anchored;
  const uint64_t did = d.id;
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
  // ---- THE STATUE (D4a, game/distant_draw.h) ------------------------------
  // Baked from the LIVE rig -- its gear, its blood, its lost limbs -- which
  // still exists: the caller releases it right after this returns. Render
  // only (never hashed, never an input to the sim); no cache, no statue.
  if (distantDraw_ != nullptr && microSet_ != nullptr)
    if (DistantMob* in = distant_.Find(did)) {
      const DistantBake b = distantDraw_->Bake(mob, *microSet_);
      if (b.baked) {
        in->look = b.key;
        in->statueRestY = b.restY;
      }
    }
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
  DistantMob* dm = distant_.Find(id);
  if (dm == nullptr) return nullptr;
  // ---- THE SNAP ONTO THE REAL GROUND (§2.4) ---------------------------------
  // It walked on DistantGround, which does not know trees, buildings or a
  // ruin floor; the fetch cache does (EntityParking::Unpark waited on the
  // chunks this reads). A living SURFACE creature that MOVED while distant is
  // put on the ground the window really has under its footprint -- up out of
  // a wall it would otherwise come back inside -- before its record is
  // patched, so the limbs are placed round the snapped origin. A corpse, an
  // ANCHORED creature and one that never moved come back where they lie /
  // stood (DistantMob::movedWhileDistant).
  if (!dm->dead && dm->groundMode == DistantGroundMode::Surface && dm->movedWhileDistant &&
      world_ != nullptr)
    SnapPromotionGround(*dm);
  const DistantMob* d = dm;
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

// ============================================================================
// DistantGround -- the ground the tier walks on (§2.4, D1; distant.h)
// ============================================================================

namespace {

// Tiles of base columns held at once (each ~2 KiB): an A* grid of the widest
// profile (navRadius 30, 61 x 61 columns) touches ~16, a long walk one more
// every 16 voxels. Past the cap the cache is trimmed by age. Pure cache:
// eviction can change only the cost of an answer, never the answer.
constexpr size_t kGroundTilesMax = 2048;
// Stored chunks held decoded (512 B of mask each).
constexpr size_t kGroundChunksMax = 4096;
// The listener's inbox past this many keys degrades to "everything changed"
// (a window shift evicts a whole plane; re-deciding 1,000 keys one by one
// costs more than re-reading the few chunks the tier actually stands on).
constexpr size_t kInboxMax = 8192;

inline uint64_t TileKey(int tx, int tz) {
  return ((uint64_t)(uint32_t)tx << 32) | (uint64_t)(uint32_t)tz;
}

// The near tier's "carries weight" (Mob::GroundHeightAt's `supports`): solid,
// or a powder holding at least kPowderMobSupportMin of a cell. Liquids, gases
// and passable vegetation (mapped to gas in the class table) never.
inline bool WordSupports(uint32_t w, const std::vector<uint32_t>& cls) {
  const uint32_t mat = w & 0xFFFu;
  if (mat == 0 || mat >= cls.size()) return false;
  return cls[mat] == CLASS_SOLID ||
         (cls[mat] == CLASS_POWDER &&
          PowderMassOfState((w >> 12) & 0xFu) >= kPowderMobSupportMin);
}

}  // namespace

// The seed is the world's (sandvox::kDefaultSeed, D0's classifier's) from the
// start, so a query made before the first Bind answers for the right world.
DistantGround::DistantGround()
    : seed_(sandvox::kDefaultSeed), inbox_(std::make_shared<Inbox>()) {}

void DistantGround::Bind(Stream* stream, const std::vector<uint32_t>* classOf,
                         uint32_t seed) {
  if (classOf != classOf_ || (classOf != nullptr && classOf->size() != classOfSize_)) {
    // What carries weight changed: every decoded mask is wrong.
    classOf_ = classOf;
    classOfSize_ = classOf != nullptr ? classOf->size() : 0;
    chunks_.clear();
    gen_++;
    stats_.flushes++;
  }
  if (seed != seed_) {
    seed_ = seed;
    tiles_.clear();
    stats_.flushes++;
  }
  if (stream != stream_) {
    stream_ = stream;
    chunks_.clear();
    gen_++;
    stats_.flushes++;
    // A fresh inbox: a listener left on another store (if any) writes into
    // the old one, which nobody reads -- the shared_ptr keeps it alive for
    // exactly as long as that store does, so neither side can dangle.
    inbox_ = std::make_shared<Inbox>();
    if (stream_ != nullptr) {
      std::shared_ptr<Inbox> box = inbox_;
      stream_->Store().SetChangeFn([box](IVec3 wc, bool all) {
        if (all) {
          box->all = true;
          box->changed.clear();
          return;
        }
        if (box->all) return;
        if (box->changed.size() >= kInboxMax) {
          box->all = true;
          box->changed.clear();
          return;
        }
        box->changed.push_back(World::PackChunkKey(wc));
      });
    }
  }
}

void DistantGround::InvalidateAll() {
  tiles_.clear();
  chunks_.clear();
  inbox_->changed.clear();
  inbox_->all = false;
  mapSerial_ = ~0ull;
  gen_++;
  stats_.flushes++;
}

void DistantGround::CheckEpoch() {
  // The base columns are a pure function of (seed, MAP): a gate or a load
  // that installs another map makes every one of them wrong at once.
  const uint64_t ms = worldmap::CurrentWorldMapSerial();
  if (ms != mapSerial_) {
    mapSerial_ = ms;
    tiles_.clear();
    stats_.flushes++;
  }
}

void DistantGround::Absorb() {
  Inbox& in = *inbox_;
  if (in.all) {
    if (!chunks_.empty()) {
      chunks_.clear();
      gen_++;
      stats_.flushes++;
    }
    in.all = false;
    in.changed.clear();
    return;
  }
  if (in.changed.empty()) return;
  for (const uint64_t k : in.changed)
    if (chunks_.erase(k) != 0) stats_.invalidations++;
  in.changed.clear();
  gen_++;
}

void DistantGround::Trim() {
  // Drop the older half by last use. Rare (past the cap), pure (a dropped
  // entry is recomputed to the same answer).
  auto trim = [&](auto& map) {
    std::vector<uint64_t> ages;
    ages.reserve(map.size());
    for (const auto& kv : map) ages.push_back(kv.second.lastUse);
    std::nth_element(ages.begin(), ages.begin() + (ptrdiff_t)(ages.size() / 2), ages.end());
    const uint64_t cut = ages[ages.size() / 2];
    for (auto it = map.begin(); it != map.end();)
      it = it->second.lastUse <= cut ? map.erase(it) : std::next(it);
  };
  if (tiles_.size() >= kGroundTilesMax) trim(tiles_);
  if (chunks_.size() >= kGroundChunksMax) {
    trim(chunks_);
    gen_++;
  }
}

DistantGround::Tile& DistantGround::TileOf(int x, int z) {
  CheckEpoch();
  const uint64_t key = TileKey(x >> 4, z >> 4);
  auto it = tiles_.find(key);
  if (it == tiles_.end()) {
    if (tiles_.size() >= kGroundTilesMax) Trim();
    it = tiles_.emplace(key, Tile{}).first;
  }
  it->second.lastUse = ++useClock_;
  return it->second;
}

int DistantGround::TerrainTop(int x, int z) {
  Tile& t = TileOf(x, z);
  const int i = ((z & 15) << 4) | (x & 15);
  const uint64_t bit = 1ull << (i & 63);
  if ((t.haveH[i >> 6] & bit) == 0) {
    t.h[i] = World::TerrainHeight(x, z, seed_);
    t.haveH[i >> 6] |= bit;
    stats_.columnEvals++;
  }
  return t.h[i];
}

int DistantGround::WaterTop(int x, int z) {
  Tile& t = TileOf(x, z);
  const int i = ((z & 15) << 4) | (x & 15);
  const uint64_t bit = 1ull << (i & 63);
  if ((t.haveW[i >> 6] & bit) == 0) {
    // Tarns and the sea (TerrainColumn), rivers and lakes (WaterColumnAt):
    // the higher surface wins. Two sources because worldgen has two, and the
    // plan's risk list says to check they agree at shorelines before trusting
    // the shark to either -- taking the max makes a shore they disagree on
    // read as water, which keeps a fish IN rather than letting it strand.
    const World::Column c = World::TerrainColumn(x, z, seed_);
    int w = c.water;   // INT32_MIN = none
    const worldmap::WaterColumn wc = worldmap::WaterColumnAt(x, z, seed_);
    if (wc.water != -1) w = std::max(w, wc.water);
    t.water[i] = w;
    t.haveW[i >> 6] |= bit;
    if ((t.haveH[i >> 6] & bit) == 0) {
      t.h[i] = c.h;   // TerrainColumn's h IS TerrainHeight
      t.haveH[i >> 6] |= bit;
    }
    stats_.waterEvals++;
  }
  return t.water[i] == INT32_MIN ? kNoWater : t.water[i];
}

const DistantGround::Chunk* DistantGround::StoredChunk(IVec3 wc) {
  if (stream_ == nullptr) return nullptr;
  const uint64_t key = World::PackChunkKey(wc);
  // THE DRAIN COMES FIRST, ALWAYS -- before the memo, before the cache. An
  // eviction of this chunk still in flight lands NOW; its Put reaches the
  // inbox, and Absorb drops whatever this cache held for it. Asking the
  // cache first would answer from bytes the store is about to replace, on a
  // tick the GPU chose.
  if (stream_->EvictionPending(wc)) {
    const uint64_t before = stream_->SettleDrains();
    stream_->SettledStoredChunk(wc);
    stats_.drains += stream_->SettleDrains() - before;
  }
  Absorb();
  if (key == memoKey_ && memoGen_ == gen_) return memoStored_ ? memoChunk_ : nullptr;
  auto it = chunks_.find(key);
  if (it == chunks_.end()) {
    if (chunks_.size() >= kGroundChunksMax) Trim();
    Chunk ch;
    const std::vector<uint32_t>* rle = stream_->SettledStoredChunk(wc);
    if (rle != nullptr && classOf_ != nullptr) {
      if (decodeScratch_.size() != kChunkVol) decodeScratch_.assign(kChunkVol, 0u);
      if (RleDecodeChunk(rle->data(), rle->size() / 2, decodeScratch_.data())) {
        ch.stored = true;
        for (uint32_t v = 0; v < kChunkVol; v++)
          if (WordSupports(decodeScratch_[v], *classOf_)) ch.solid[v >> 6] |= 1ull << (v & 63);
        stats_.chunkDecodes++;
      }
    }
    if (!ch.stored) stats_.chunkAbsent++;
    it = chunks_.emplace(key, ch).first;
  }
  it->second.lastUse = ++useClock_;
  memoKey_ = key;
  memoGen_ = gen_;
  memoChunk_ = &it->second;
  memoStored_ = it->second.stored;
  return memoStored_ ? memoChunk_ : nullptr;
}

bool DistantGround::EditedPossible() const {
  // Nothing stored, nothing on a disk, nothing in flight: every column is
  // procgen, and not one chunk needs asking about. Every harness and every
  // fresh world takes this path until the player edits something and leaves.
  if (stream_ == nullptr) return false;
  Stream& st = *stream_;
  return st.Store().Count() != 0 || st.Store().Bound() || st.AnyEvictionPending();
}

bool DistantGround::AnyStored(int x, int z, int yLo, int yHi) {
  if (!EditedPossible()) return false;
  for (int cy = yLo >> 4; cy <= (yHi >> 4); cy++)
    if (StoredChunk(IVec3{x >> 4, cy, z >> 4}) != nullptr) return true;
  return false;
}

bool DistantGround::Solid(int x, int y, int z) {
  if (EditedPossible()) {
    if (const Chunk* c = StoredChunk(IVec3{x >> 4, y >> 4, z >> 4})) {
      const uint32_t v = ((uint32_t)(z & 15) * kChunk + (uint32_t)(y & 15)) * kChunk +
                         (uint32_t)(x & 15);
      return (c->solid[v >> 6] >> (v & 63)) & 1ull;
    }
  }
  return y <= TerrainTop(x, z);
}

bool DistantGround::Stand(int x, int z, int fromY, int stepUp, int stepDown,
                          int headroom, int& outY, bool* edited) {
  if (edited != nullptr) *edited = false;
  const int lo = fromY - stepDown - 1;   // support cell c: c + 1 >= fromY - stepDown
  const int hi = fromY + stepUp - 1;     //                 c + 1 <= fromY + stepUp
  if (!AnyStored(x, z, lo, hi + headroom + 1)) {
    // PROCGEN: the column is TerrainHeight, solid below and air above, so the
    // only standing place is its top and headroom is free.
    const int s = TerrainTop(x, z) + 1;
    if (s < fromY - stepDown || s > fromY + stepUp) return false;
    outY = s;
    return true;
  }
  if (edited != nullptr) *edited = true;
  for (int c = hi; c >= lo; c--) {
    if (!Solid(x, c, z)) continue;
    bool clear = true;
    for (int k = 1; k <= headroom && clear; k++) clear = !Solid(x, c + k, z);
    if (!clear) continue;
    outY = c + 1;
    return true;
  }
  return false;
}

bool DistantGround::ProbeGround(int x, int z, int yFrom, int& outY) {
  if (!AnyStored(x, z, yFrom - kProbeDown, yFrom + kProbeUp)) {
    outY = TerrainTop(x, z) + 1;
    return true;
  }
  if (Solid(x, yFrom, z)) {
    for (int y = yFrom + 1; y <= yFrom + kProbeUp; y++)
      if (!Solid(x, y, z)) {
        outY = y;
        return true;
      }
    outY = yFrom + kProbeUp + 1;   // buried past the reach: a wall to StepTo
    return true;
  }
  for (int y = yFrom - 1; y >= yFrom - kProbeDown; y--)
    if (Solid(x, y, z)) {
      outY = y + 1;
      return true;
    }
  outY = yFrom - kProbeDown;       // a pit past the reach: a drop to StepTo
  return true;
}

bool DistantGround::Land(int x, int z, int fromY, int maxUp, int maxDown, int& outY) {
  if (!AnyStored(x, z, fromY - maxDown - 1, fromY + maxUp + 1)) {
    const int s = TerrainTop(x, z) + 1;
    if (s >= fromY ? s - fromY <= maxUp : fromY - s <= maxDown) {
      outY = s;
      return true;
    }
    return false;
  }
  if (Solid(x, fromY, z)) {
    for (int y = fromY + 1; y <= fromY + maxUp; y++)
      if (!Solid(x, y, z)) {
        outY = y;
        return true;
      }
    return false;
  }
  for (int y = fromY - 1; y >= fromY - maxDown - 1; y--)
    if (Solid(x, y, z)) {
      outY = y + 1;
      return true;
    }
  return false;
}

// ============================================================================
// THE DISTANT TICK (§2.5, §2.11; package D1)
// ============================================================================

namespace {

// THE EDGE BAND (§2.7): a near creature this close to the window's boundary
// is part of a distant creature's world -- the longest near sight (the
// archer's 160) plus a margin; the band D3 opens the other way.
constexpr float kDistantEdgeBandVox = 192.0f;
// The sleep test's margin past the keep range (§2.11), and the reach of a
// creature's actor list past its own keep range (so a stranger is in the list
// a little before it can be acquired, and the hysteresis has room).
constexpr float kDistantSleepMarginVox = 16.0f;
// The afloat Schmitt pair and the float line, in shares of the body's
// standing height of water over its bed: game/mob_afloat.cpp's kAfloatEnter /
// kAfloatLeave / kAfloatShare, restated (they are file-local there).
constexpr float kDistantAfloatEnter = 0.92f;
constexpr float kDistantAfloatLeave = 0.80f;
constexpr float kDistantAfloatShare = 0.72f;
// A fish's water: at least its own height of it (and two voxels) between
// the bed and the surface, or the step is refused -- it never strands.
constexpr float kDistantFishMinDepthBodies = 1.0f;
// Substep length of the drive, voxels: no step crosses more than one column
// between ground checks, so a stride's 4-tick move cannot hop a thin wall.
constexpr float kDistantSubstepVox = 1.0f;

// THE TICK-START STATE of one living distant creature: what every OTHER
// creature's step reads (perception, spacing, the sleep test). Built once per
// stepping tick, before anyone moves, so the order creatures are stepped in
// cannot change what any of them sees (§2.12).
struct DistantSnap {
  bool valid = false;   // a living creature
  uint64_t id = 0;
  Vec3 base{};          // footprint centre x / z, feet y (BodyCentre's frame)
  Vec3 centre{};        // the body box's centre
  float radius = 1.0f, height = 2.0f, heading = 0.0f, hpFrac = 1.0f;
  float immersion = 0.0f, keep = 0.0f;
  uint32_t faction = ~0u;
  bool hostile = false, armed = false;
  uint64_t targetId = 0;
  Vec3 vel{};
};

// THE PROMOTION BOX (§2.1): EntityParking::Unpark's "inside the window by
// kUnparkMarginChunks on every face", as voxel bounds on the min corner.
struct PromoBox {
  float lo[3], hi[3];
  bool Inside(Vec3 p) const {
    return p.x >= lo[0] && p.x < hi[0] && p.y >= lo[1] && p.y < hi[1] && p.z >= lo[2] &&
           p.z < hi[2];
  }
  // How far inside (the nearest face), negative outside.
  float Depth(Vec3 p) const {
    const float v[3] = {p.x, p.y, p.z};
    float d = std::numeric_limits<float>::infinity();
    for (int a = 0; a < 3; a++) d = std::min(d, std::min(v[a] - lo[a], hi[a] - v[a]));
    return d;
  }
};
PromoBox PromoBoxOf(IVec3 wo) {
  const int n = (int)(kWorldN / kChunk), m = EntityParking::kUnparkMarginChunks;
  const int w[3] = {wo.x, wo.y, wo.z};
  PromoBox b{};
  for (int a = 0; a < 3; a++) {
    b.lo[a] = (float)((w[a] + m) * (int)kChunk);
    b.hi[a] = (float)((w[a] + n - m) * (int)kChunk);
  }
  return b;
}

// The NavProbe over DistantGround (ai_nav.h rule 2: the probe is injected),
// per creature: for a body that SWIMS (MobSystem::CanSwimAfloat), water reads
// as a floor at its surface -- the planner, the straight-line test and the
// ground fan then route it across a lake the way the drive will take it,
// instead of refusing the drop to the bed. A non-swimmer walks the bed.
struct DistantNavCtx {
  DistantGround* g = nullptr;
  bool swimmer = false;
};
bool DistantNavGround(DistantNavCtx& c, int x, int z, int yFrom, int& outY) {
  c.g->ProbeGround(x, z, yFrom, outY);
  if (c.swimmer) {
    const int wt = c.g->WaterTop(x, z);
    if (wt != DistantGround::kNoWater && wt > outY) outY = wt;
  }
  return true;
}
bool DistantProbeGround(void* ctx, int x, int z, int yFrom, int& outY) {
  return DistantNavGround(*static_cast<DistantNavCtx*>(ctx), x, z, yFrom, outY);
}
bool DistantProbeBlocked(void* ctx, int x, int y, int z) {
  return static_cast<DistantNavCtx*>(ctx)->g->Solid(x, y, z);
}

float KeepRangeOf(const ai::Profile* p) {
  if (p == nullptr || p->perception.aggro == ai::Aggro::Passive) return 0.0f;
  return p->perception.sightRange * std::max(1.0f, p->perception.keepRangeScale);
}

// BodyFacts' hpFrac and limbsLost on the abstract body.
void DistantBodyFacts(const DistantMob& d, float& hpFrac, int& limbsLost) {
  float sum = 0.0f;
  limbsLost = 0;
  const size_t n = std::min({(size_t)std::max(0, d.baseLimbs), d.limbHp.size(), d.limbRole.size()});
  for (size_t i = 0; i < n; i++) {
    const uint8_t f = d.limbRole[i].flags;
    if (f & kDistantLimbBloodless) continue;
    if (!(f & kDistantLimbAttached)) {
      limbsLost++;
      continue;
    }
    if (d.limbHp[i] > 0.0f) sum += d.limbHp[i];
  }
  hpFrac = d.hpFull > 0.0f ? std::clamp(sum / d.hpFull, 0.0f, 1.0f) : 0.0f;
}

}  // namespace

void MobSystem::SetDistantStream(Stream* s) {
  distantGround_.Bind(s, &classOf_, sandvox::kDefaultSeed);
}

bool MobSystem::DemoteForTest(uint64_t id) {
  if (world_ == nullptr) return false;
  for (size_t mi = 0; mi < mobs_.size(); mi++) {
    Mob& mob = mobs_[mi];
    if (mob.id_ != id) continue;
    if (mob.IsGhost() || mob.rigReleased_ || mob.defIndex_ < 0 ||
        mob.defIndex_ >= (int)defs_.size())
      return false;
    const IVec3 wo = world_->WindowOrigin();
    const Vec3 wlo{(float)(wo.x * (int)kChunk), (float)(wo.y * (int)kChunk),
                   (float)(wo.z * (int)kChunk)};
    std::vector<uint8_t> rec;
    {
      ByteWriter w{rec};
      mob.SaveOne(w);
    }
    // THE PARK RULE'S OWN DEMOTION, from here on (PreTick's park branch).
    if (!DemoteToDistant(mob, rec, wlo)) return false;
    if (!mob.alive_)
      for (size_t k = 0; k < rises_.size();)
        if (rises_[k].mobId == mob.id_) {
          rises_[k] = std::move(rises_.back());
          rises_.pop_back();
        } else {
          k++;
        }
    mob.ReleaseRig();
    mobs_[mi] = std::move(mobs_.back());
    mobs_.pop_back();
    instancesDirty_ = true;
    return true;
  }
  return false;
}

// ---- the mid-air landing (§2.4) ---------------------------------------------
void MobSystem::LandDistant(DistantMob& d, uint32_t tick) {
  d.landPending = false;
  const Tuning::Distant& tune = CurrentTuning().distant;
  const Vec3 c = d.Centre();
  const int cx = ifloor(c.x), cz = ifloor(c.z);
  const int fy = ifloor(d.origin.y);
  // SURFACE means its feet were within surfaceSnapMaxVox of the ground, so
  // the ground is searched that far (and a little: the classification read
  // TerrainHeight, this reads the edited ground too).
  const int reach = std::max(1, (int)std::ceil(tune.surfaceSnapMaxVox)) + 8;
  int gy = 0;
  if (!distantGround_.Land(cx, cz, fy, reach, reach, gy))
    gy = distantGround_.TerrainTop(cx, cz) + 1;
  const float drop = std::max(0.0f, d.origin.y - (float)gy);
  // The speed it arrives at: what it had (either way -- one thrown upward
  // comes back down through the same height) plus the drop under gravity.
  const float g = MetresToCells(CurrentTuning().physics.gravity);
  const float impactVox = std::sqrt(d.vel.y * d.vel.y + 2.0f * g * drop);
  d.origin.y = (float)gy;
  d.vel = Vec3{};
  d.movedWhileDistant = true;
  distant_.Note(&DistantMobs::Stats::landings);
  // ---- Mob::ApplyFallDamage's verdict, on the abstract body -------------
  const auto& pt = CurrentTuning().player;
  const float impactMs = impactVox * kVoxelMeters;
  if (impactMs < pt.fallDamageSpeed) return;
  const float excess = impactMs - pt.fallDamageSpeed;
  const float damage = excess * excess * pt.fallDamageScale;
  // SpendableHp: every attached limb with hp that bleeds.
  float total = 0.0f;
  for (size_t i = 0; i < d.limbHp.size() && i < d.limbRole.size(); i++) {
    const uint8_t f = d.limbRole[i].flags;
    if ((f & kDistantLimbAttached) && !(f & kDistantLimbBloodless) && d.limbHp[i] > 0.0f)
      total += d.limbHp[i];
  }
  const float bar = std::floor(total);
  const float bill = (float)std::lround(damage);
  // Lethal by ApplyFallDamage's test (a splat, or a bill of the whole bar),
  // or by SpendHp's (the rounded bill reaches the bar).
  if (impactMs >= pt.fallSplatSpeed || damage >= bar || bill >= bar || bar <= 0.0f) {
    d.dead = true;
    d.cause = "fall";
    d.deathSeq = ++deathSeq_;
    d.deathTick = tick;
    d.asleep = true;
    distant_.Note(&DistantMobs::Stats::landingDeaths);
    return;
  }
  // SpendHp's spread: each limb pays in proportion to what it still has, so
  // none is singled out to be ruined (and none reaches zero: frac < 1).
  const float frac = bill / bar;
  for (size_t i = 0; i < d.limbHp.size() && i < d.limbRole.size(); i++) {
    const uint8_t f = d.limbRole[i].flags;
    if ((f & kDistantLimbAttached) && !(f & kDistantLimbBloodless) && d.limbHp[i] > 0.0f)
      d.limbHp[i] -= d.limbHp[i] * frac;
  }
  distant_.Note(&DistantMobs::Stats::landingHurt);
}

// ---- the promotion snap (§2.4) ----------------------------------------------
namespace {
// The footprint's columns the snap reads: a 3 x 3 grid over the body box,
// inset half a voxel (Mob::FootprintFooting's shape: the ground a box rests
// on is the MAX over its footprint).
void FootprintColumns(const DistantMob& d, int (&xs)[3], int (&zs)[3]) {
  const float hx = std::max(0.5f, d.worldSize.x * 0.5f - 0.5f);
  const float hz = std::max(0.5f, d.worldSize.z * 0.5f - 0.5f);
  const Vec3 c = d.Centre();
  xs[0] = ifloor(c.x - hx);
  xs[1] = ifloor(c.x);
  xs[2] = ifloor(c.x + hx);
  zs[0] = ifloor(c.z - hz);
  zs[1] = ifloor(c.z);
  zs[2] = ifloor(c.z + hz);
}
}  // namespace

void MobSystem::DistantPromotionChunks(const DistantMob& d, std::vector<IVec3>& out) const {
  out.clear();
  if (world_ == nullptr || d.dead || d.groundMode != DistantGroundMode::Surface ||
      !d.movedWhileDistant)
    return;
  const int maxD = std::max(0, (int)std::ceil(CurrentTuning().distant.promoteMaxDropVox));
  int xs[3], zs[3];
  FootprintColumns(d, xs, zs);
  const int fy = ifloor(d.origin.y + 1e-3f);
  const int cyLo = (fy - maxD - 1) >> 4, cyHi = (fy + maxD + d.headroomCells + 1) >> 4;
  for (int a = 0; a < 3; a++)
    for (int b = 0; b < 3; b++)
      for (int cy = cyLo; cy <= cyHi; cy++) {
        const IVec3 wc{xs[a] >> 4, cy, zs[b] >> 4};
        if (!world_->ChunkInWindow(wc)) continue;
        bool dup = false;
        for (const IVec3& o : out) dup |= o.x == wc.x && o.y == wc.y && o.z == wc.z;
        if (!dup) out.push_back(wc);
      }
}

void MobSystem::SnapPromotionGround(DistantMob& d) {
  World& world = *world_;
  const int maxD = std::max(0, (int)std::ceil(CurrentTuning().distant.promoteMaxDropVox));
  int xs[3], zs[3];
  FootprintColumns(d, xs, zs);
  const int fy = ifloor(d.origin.y + 1e-3f);
  // 1 carries weight, 0 does not, -1 unknown (not in the window, or not in
  // the fetch cache -- Unpark waited on DistantPromotionChunks, so only a
  // chunk outside the window can be unknown here).
  auto support = [&](int x, int y, int z) -> int {
    const IVec3 cell{x, y, z};
    if (!world.CellInWindow(cell)) return -1;
    const CachedChunk* cc = world.Cached({x >> 4, y >> 4, z >> 4});
    if (cc == nullptr || cc->voxels.size() != kChunkVol) return -1;
    const uint32_t w = cc->voxels[((uint32_t)(z & 15) * kChunk + (uint32_t)(y & 15)) * kChunk +
                                  (uint32_t)(x & 15)];
    return WordSupports(w, classOf_) ? 1 : 0;
  };
  int best = INT_MIN;
  for (int a = 0; a < 3; a++)
    for (int b = 0; b < 3; b++) {
      const int x = xs[a], z = zs[b];
      const int s0 = support(x, fy, z);
      if (s0 < 0) continue;
      int stand = INT_MIN;
      if (s0 == 1) {
        // INSIDE the real ground (a building, a rock, a ruin floor TerrainHeight
        // does not know): up to the first cell with headroom above it.
        for (int y = fy + 1; y <= fy + maxD; y++) {
          const int s = support(x, y, z);
          if (s < 0) break;
          if (s == 1) continue;
          bool clear = true;
          for (int k = 1; k < d.headroomCells && clear; k++) clear = support(x, y + k, z) == 0;
          if (!clear) continue;
          stand = y;
          break;
        }
      } else {
        for (int y = fy - 1; y >= fy - maxD - 1; y--) {
          const int s = support(x, y, z);
          if (s < 0) break;
          if (s == 1) {
            stand = y + 1;
            break;
          }
        }
      }
      if (stand != INT_MIN) best = std::max(best, stand);
    }
  if (best == INT_MIN) {
    // No ground within the bound on any column it can see: it keeps its
    // height, and the near tier's own fall takes it from there (a kinematic
    // creature's fall bills no damage -- only a ragdoll's arrest does).
    distant_.Note(&DistantMobs::Stats::snapMisses);
    return;
  }
  const float err = std::fabs((float)best - d.origin.y);
  distant_.Note(&DistantMobs::Stats::snaps);
  distant_.NoteMax(&DistantMobs::Stats::snapErrQ, (uint64_t)std::lround(err * 16.0f));
  d.origin.y = (float)best;
}

// ---- the tick -------------------------------------------------------------------
void MobSystem::TickDistant(uint32_t tick, World& world,
                            const std::vector<ai::Actor>* nearActors) {
  if (distant_.Count() == 0) {
    nearBand_.clear();
    return;
  }
  const Tuning::Distant& tune = CurrentTuning().distant;
  // THE SWITCH OFF: RetireDistant parks everyone this very tick (with a park
  // function); without one they stay, frozen, as D0 kept them.
  if (!tune.enabled) return;
  const auto t0 = std::chrono::steady_clock::now();
  auto timeOut = [&]() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    distant_.Note(&DistantMobs::Stats::tickNs, (uint64_t)std::max<int64_t>(0, ns));
  };
  distant_.Note(&DistantMobs::Stats::tickCallsAll);
  // The classes and the seed the ground answers for (a materials reload
  // re-points classOf_; Bind is two compares when nothing changed).
  distantGround_.Bind(distantGround_.BoundStream(), &classOf_, sandvox::kDefaultSeed);
  const IVec3 wo = world.WindowOrigin();
  const Vec3 wlo{(float)(wo.x * (int)kChunk), (float)(wo.y * (int)kChunk),
                 (float)(wo.z * (int)kChunk)};

  // ---- THE EDGE BAND: near creatures, NEVER a player (§2.7) -----------------
  // The near tier's actor list as it stood at the top of this tick. Players
  // (the player-actor band), loose flesh and the dead are not part of a
  // distant creature's world in v1 -- whatever the list holds, whatever the
  // geometry -- so "a distant creature cannot perceive the player" holds by
  // construction.
  thread_local std::vector<ai::Actor> band;
  band.clear();
  if (nearActors != nullptr)
    for (const ai::Actor& a : *nearActors) {
      if (ai::IsPlayerActorId(a.id) || ai::IsLooseActorId(a.id) || !a.alive) continue;
      // Distance to the window's boundary (the nearest face, any axis).
      const float v[3] = {a.centre.x - wlo.x, a.centre.y - wlo.y, a.centre.z - wlo.z};
      float edge = std::numeric_limits<float>::infinity();
      for (int k = 0; k < 3; k++) edge = std::min(edge, std::min(v[k], (float)kWorldN - v[k]));
      if (edge > kDistantEdgeBandVox) continue;
      band.push_back(a);
    }
  std::sort(band.begin(), band.end(),
            [](const ai::Actor& x, const ai::Actor& y) { return x.id < y.id; });
  // ...and a near creature that MOVED INTO a distant sleeper's neighbourhood
  // wakes it, as a distant mover does (§2.11).
  {
    std::vector<NearBandCell> now;
    now.reserve(band.size());
    size_t j = 0;
    for (const ai::Actor& a : band) {
      const int cx = DistantMobs::CellOf(a.centre.x), cz = DistantMobs::CellOf(a.centre.z);
      while (j < nearBand_.size() && nearBand_[j].id < a.id) j++;
      const bool had = j < nearBand_.size() && nearBand_[j].id == a.id;
      if (!had)
        distant_.WakeEntered(cx, cz, /*fromValid=*/false, 0, 0);
      else if (nearBand_[j].cx != cx || nearBand_[j].cz != cz)
        distant_.WakeEntered(cx, cz, /*fromValid=*/true, nearBand_[j].cx, nearBand_[j].cz);
      now.push_back({a.id, cx, cz});
    }
    nearBand_.swap(now);
  }

  // ---- NOTHING TO DO COSTS NOTHING (rule 2) ---------------------------------
  // Everybody asleep (or dead) and no sleeper's clock due: not one creature is
  // looked at. 512 sleepers cost two compares a tick.
  if (!distant_.AnyAwakeHint() && tick < distant_.NextWakeTick()) {
    timeOut();
    return;
  }

  // ---- WHO STEPS ----------------------------------------------------------------
  // The dead: never. The asleep: not, unless their clock came round. The
  // ENGAGED (a target, a threat, a landing to make): every tick. Everyone
  // else awake: on the idle stride, phase (tick + id), moving stride ticks'
  // worth when they do.
  const int stride = std::max(1, tune.idleStride);
  const float dt = 1.0f / 30.0f;
  thread_local std::vector<size_t> steppers;
  steppers.clear();
  bool anyAwake = false;
  uint32_t nextWake = UINT32_MAX;
  for (size_t i = 0; i < distant_.Count(); i++) {
    DistantMob& d = distant_.At(i);
    if (d.dead) continue;
    if (d.asleep) {
      if (d.wakeTick == 0 || tick < d.wakeTick) {
        if (d.wakeTick != 0) nextWake = std::min(nextWake, d.wakeTick);
        continue;
      }
      d.asleep = false;
      d.wakeTick = 0;
      d.wakes++;
      distant_.Note(&DistantMobs::Stats::wakesTimer);
    }
    anyAwake = true;
    const bool engaged = d.brain.hasTarget || d.brain.hasThreat || d.landPending;
    if (!engaged && (tick + (uint32_t)d.id) % (uint32_t)stride != 0) continue;
    steppers.push_back(i);
  }
  if (steppers.empty()) {
    distant_.SetSleepSummary(anyAwake, nextWake);
    timeOut();
    return;
  }
  distant_.Note(&DistantMobs::Stats::tickCalls);

  // ---- THE SNAPSHOT: everyone living, AS OF NOW ------------------------------
  // What every step reads about every OTHER creature. Built EAGERLY for the
  // steppers, before any of them decides (they are the only creatures phase 1
  // writes), and LAZILY for the rest, which nothing this tick changes -- so
  // every read is tick-start state and the step order cannot matter (§2.12),
  // and a lone walker among 512 sleepers builds a handful of entries, not 512.
  thread_local std::vector<DistantSnap> snap;
  thread_local std::vector<uint8_t> snapBuilt;
  snap.assign(distant_.Count(), DistantSnap{});
  snapBuilt.assign(distant_.Count(), 0);
  // The longest keep (or scent) range in the library: how far the sleep test
  // must look for a stranger whose eye reaches further than this one's.
  float libMaxKeep = 0.0f;
  for (const ai::Profile& pr : behaviors_.profiles)
    libMaxKeep = std::max({libMaxKeep, KeepRangeOf(&pr), pr.perception.scentRange});
  auto buildSnap = [&](size_t i) {
    snapBuilt[i] = 1;
    const DistantMob& d = distant_.At(i);
    if (d.dead) return;
    DistantSnap& s = snap[i];
    s.valid = true;
    s.id = d.id;
    s.centre = d.Centre();
    s.base = Vec3{s.centre.x, d.origin.y, s.centre.z};
    s.radius = 0.5f * std::max(d.worldSize.x, d.worldSize.z);
    s.height = d.worldSize.y;
    s.heading = d.steer.heading;
    int lost = 0;
    DistantBodyFacts(d, s.hpFrac, lost);
    const ai::Profile* pr = behaviors_.At(d.brain.profile);
    s.faction = pr != nullptr ? ai::FactionId(pr->faction) : ~0u;
    s.hostile = pr != nullptr && pr->perception.aggro == ai::Aggro::Hostile;
    s.keep = KeepRangeOf(pr);
    s.armed = d.armed;
    s.targetId = d.brain.hasTarget ? d.brain.targetId : 0;
    s.vel = d.vel;
    if (d.swimming || d.aquatic) {
      const int wt = distantGround_.WaterTop(ifloor(s.centre.x), ifloor(s.centre.z));
      if (wt != DistantGround::kNoWater && d.worldSize.y > 0.0f)
        s.immersion = std::clamp(((float)wt - d.origin.y) / d.worldSize.y, 0.0f, 1.0f);
    }
  };
  for (const size_t i : steppers) buildSnap(i);
  auto snapAt = [&](size_t i) -> const DistantSnap& {
    if (!snapBuilt[i]) buildSnap(i);
    return snap[i];
  };
  auto indexOf = [&](uint64_t id) -> size_t {
    const std::vector<DistantMob>& all = distant_.All();
    size_t lo = 0, hi = all.size();
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (all[mid].id < id) lo = mid + 1;
      else hi = mid;
    }
    return lo < all.size() && all[lo].id == id ? lo : SIZE_MAX;
  };
  const PromoBox box = PromoBoxOf(wo);
  const Vec3 wc{wlo.x + 0.5f * (float)kWorldN, wlo.y + 0.5f * (float)kWorldN,
                wlo.z + 0.5f * (float)kWorldN};
  const float retireReach = tune.radius + tune.hysteresis;

  // ---- PHASE 1: every stepper decides, writing only itself -------------------
  struct Next {
    size_t i;
    Vec3 origin;
    bool moved;
    float movedLen;
  };
  thread_local std::vector<Next> nexts;
  nexts.clear();
  thread_local std::vector<uint64_t> ids;
  thread_local std::vector<ai::Actor> actors;
  const auto& pt = CurrentTuning().player;
  for (const size_t i : steppers) {
    DistantMob& d = distant_.At(i);
    const uint32_t elapsed = d.lastStepTick == 0 ? 1u : tick - d.lastStepTick;
    const float dtStep = dt * (float)std::clamp(elapsed, 1u, (uint32_t)stride);
    d.lastStepTick = tick;
    d.steps++;
    distant_.Note(&DistantMobs::Stats::steps);
    Next nx{i, d.origin, false, 0.0f};
    if (d.landPending) {
      LandDistant(d, tick);
      nx.origin = d.origin;
    }
    const ai::Profile* prof = d.dead ? nullptr : behaviors_.At(d.brain.profile);
    // NO PROFILE, NO MIND (the near tier's legacy wander-and-avoid is a rig's
    // fallback, not a distant one): it stands, and the sleep test below puts
    // it to sleep. A corpse made by the landing likewise.
    if (prof == nullptr) {
      nexts.push_back(nx);
      continue;
    }
    const Vec3 c = d.Centre();
    const int cx = ifloor(c.x), cz = ifloor(c.z);
    const int feet = ifloor(d.origin.y + 1e-3f);

    // ---- the water at its column ----
    const int waterTop = distantGround_.WaterTop(cx, cz);
    const bool wet = waterTop != DistantGround::kNoWater && (float)waterTop > d.origin.y;
    const bool inWater = d.aquatic ? wet : false;

    // ---- SELF ----
    ai::SelfView self;
    self.id = d.id;
    self.origin = d.origin;
    self.size = d.worldSize;
    self.heading = d.steer.heading;
    self.speed = d.speed;
    self.turnRate = d.loco.turnRate * std::max(0.05f, d.loco.turnRateMoving);
    self.stepUpCells = d.stepUpCells;
    self.stepDownCells = d.stepDownCells;
    self.headroomCells = d.headroomCells;
    self.faction = ai::FactionId(prof->faction);
    DistantBodyFacts(d, self.hpFrac, self.limbsLost);
    self.burningFrac = d.burnTicksLeft > 0 ? 1.0f : 0.0f;
    self.volumetric = d.aquatic && inWater;
    if (wet && d.worldSize.y > 0.0f)
      self.immersion = std::clamp(((float)waterTop - d.origin.y) / d.worldSize.y, 0.0f, 1.0f);
    self.velocity = d.vel;
    self.length = d.worldSize.z;
    self.armed = d.armed;
    self.canGuard = d.canGuard;
    self.ammo = d.ammo;

    // ---- THE GROUND FAN (SenseGround's shape, over DistantGround) ----
    DistantNavCtx nav{&distantGround_, d.canFloat};
    ai::GroundView gv;
    if (!self.volumetric) {
      gv.haveGround = true;
      gv.groundY = feet;
      const float margin = MetresToCells(0.2f);
      const float reachAcross = d.worldSize.x * 0.5f + margin;
      const float reachAlong = d.worldSize.z * 0.5f + margin;
      const Vec3 fwdAx{std::sin(d.steer.heading), 0, std::cos(d.steer.heading)};
      const Vec3 rgtAx{std::cos(d.steer.heading), 0, -std::sin(d.steer.heading)};
      for (int k = 0; k < ai::GroundView::kProbes; k++) {
        const float th = (6.2831853f * (float)k) / (float)ai::GroundView::kProbes;
        const float along = std::cos(th) * reachAlong, across = std::sin(th) * reachAcross;
        const int px = ifloor(c.x + fwdAx.x * along + rgtAx.x * across);
        const int pz = ifloor(c.z + fwdAx.z * along + rgtAx.z * across);
        int py = feet;
        DistantNavGround(nav, px, pz, feet + d.stepUpCells + 1, py);
        // In water it can swim, its "feet" for the fan are the surface it
        // swims at (afloat already, or about to be: one standing on a deep
        // bed is not walled in by the water over it).
        const int rise = py - (d.canFloat && wet ? std::max(feet, waterTop) : feet);
        gv.stepUp[k] = rise;
        // Past the climb budget is a wall, as near; and -- unlike near, where
        // a body that steps off a ledge FALLS and survives -- past the drop
        // budget is a wall too: this tier has no fall, its drive refuses the
        // step, so the steering had better see it coming.
        gv.clear[k] = rise <= d.stepUpCells && -rise <= d.stepDownCells;
      }
    }
    self.cornered = gv.haveGround && !gv.clear[ai::GroundView::kProbes / 2];

    // ---- THE WORLD: distant creatures round it + the near band ----
    // Its own keep (or scent) range, and at least its own 3x3 cells.
    const float reachR = std::max({KeepRangeOf(prof), prof->perception.scentRange,
                                   (float)DistantMobs::kCellVox}) +
                         kDistantSleepMarginVox;
    const int rc = (int)std::ceil(reachR / (float)DistantMobs::kCellVox);
    distant_.Neighbourhood(d.cellX, d.cellZ, rc, ids);
    actors.clear();
    {
      size_t bj = 0;
      auto pushBandUpTo = [&](uint64_t limit) {
        for (; bj < band.size() && band[bj].id < limit; bj++) {
          const ai::Actor& a = band[bj];
          const float dx = a.centre.x - c.x, dz = a.centre.z - c.z;
          if (dx * dx + dz * dz <= reachR * reachR) actors.push_back(a);
        }
      };
      for (const uint64_t oid : ids) {
        pushBandUpTo(oid);
        const size_t oi = indexOf(oid);
        if (oi == SIZE_MAX || !snapAt(oi).valid) continue;
        const DistantSnap& s = snapAt(oi);
        const float dx = s.centre.x - c.x, dz = s.centre.z - c.z;
        if (oid != d.id && dx * dx + dz * dz > reachR * reachR) continue;
        ai::Actor a;
        a.id = s.id;
        a.centre = s.centre;
        a.radius = s.radius;
        a.height = s.height;
        a.faction = s.faction;
        a.alive = true;
        a.armed = s.armed;
        a.hpFrac = s.hpFrac;
        a.heading = s.heading;
        a.targetId = s.targetId;
        a.hostile = s.hostile;
        a.immersion = s.immersion;
        a.velocity = s.vel;
        actors.push_back(a);
      }
      pushBandUpTo(UINT64_MAX);
    }
    ai::WorldView wv;
    wv.actors = &actors;
    wv.probe.ground = &DistantProbeGround;
    wv.probe.blocked = &DistantProbeBlocked;
    wv.probe.ctx = &nav;
    // OPEN-FIELD SIGHT and no part aim at this tier (§2.5): hills do not
    // block a distant creature's eye -- a known difference from near.
    wv.lineOfSight = nullptr;
    wv.losCtx = nullptr;
    wv.aimPart = nullptr;
    wv.aimCtx = nullptr;

    // ---- THINK ----
    ai::IntentOut out;
    const bool thought = ai::Think(d.brain, behaviors_, self, gv, wv, tick, dtStep, out);
    if (!thought) {
      nexts.push_back(nx);
      continue;
    }
    // NO FIGHTING YET (D2b wires the abstract strike, the abstract arrow and
    // -- owner decision 5.3 -- casting stays held): the intent is counted and
    // dropped; the brain's own clocks ran as if it had swung.
    if (out.attack || out.shoot || out.cast)
      distant_.Note(&DistantMobs::Stats::combatHolds);

    // ---- STEER (the near tier's own function) ----
    d.steer.desiredHeading = out.desiredHeading;
    const float align = SteerStep(d.steer, d.loco, d.speed,
                                  std::numeric_limits<float>::infinity(), 1.0f, 1.0f, dtStep);
    // ANCHORED: it thinks and it turns, and it never moves (§2.4).
    if (d.groundMode == DistantGroundMode::Anchored) {
      nexts.push_back(nx);
      continue;
    }
    float drive = out.driveScale, strafe = out.driveStrafe;

    // ---- SPACE: the near crowd's rule (MobSystem::CrowdPush / Apply-
    // CrowdSpacing) over the tick-start neighbours ----
    if (d.loco.spacingMul > 0.0f && d.loco.crowdPush > 0.0f) {
      const float rSelf = 0.5f * std::max(d.worldSize.x, d.worldSize.z);
      const Vec3 cs{c.x, d.origin.y, c.z};
      Vec3 push{};
      auto pushFrom = [&](uint64_t oid, Vec3 co, float rOther) {
        const float want = (rSelf + rOther) * d.loco.spacingMul;
        if (want <= 0.0f) return;
        if (std::abs(cs.y - co.y) > d.worldSize.y * 0.5f) return;
        float dx = cs.x - co.x, dz = cs.z - co.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 >= want * want) return;
        float len = std::sqrt(d2);
        if (len < 1e-4f) {
          // Co-located: MobSystem::CrowdPush's id-pair rule, so a replay
          // separates them the same way and the two leave opposite ways.
          const uint32_t h = rng::Hash3((uint32_t)(d.id ^ oid), 0x5EAu, 0);
          const float ang = (float)(h & 0xFFFFu) * (6.2831853f / 65536.0f);
          const float sg = d.id < oid ? -1.0f : 1.0f;
          dx = std::cos(ang) * sg;
          dz = std::sin(ang) * sg;
          len = 1.0f;
        }
        const float w = (want - len) / want;
        push.x += (dx / len) * w;
        push.z += (dz / len) * w;
      };
      thread_local std::vector<uint64_t> near3;
      distant_.Neighbourhood(d.cellX, d.cellZ, 1, near3);
      for (const uint64_t oid : near3) {
        if (oid == d.id) continue;
        const size_t oi = indexOf(oid);
        if (oi == SIZE_MAX || !snapAt(oi).valid) continue;
        pushFrom(oid, snapAt(oi).base, snapAt(oi).radius);
      }
      for (const ai::Actor& a : band)
        pushFrom(a.id, Vec3{a.centre.x, a.centre.y - a.height * 0.5f, a.centre.z}, a.radius);
      const float m = std::sqrt(push.x * push.x + push.z * push.z);
      if (m > 1.0f) {
        push.x /= m;
        push.z /= m;
      }
      if (push.x != 0.0f || push.z != 0.0f) {
        const Vec3 fwd{std::sin(d.steer.heading), 0, std::cos(d.steer.heading)};
        const Vec3 rgt{std::cos(d.steer.heading), 0, -std::sin(d.steer.heading)};
        const float lat = push.x * rgt.x + push.z * rgt.z;
        const float ahead = push.x * fwd.x + push.z * fwd.z;
        strafe = std::clamp(strafe + lat * d.loco.crowdPush, -1.0f, 1.0f);
        if (ahead < 0.0f && drive > 0.0f)
          drive *= std::clamp(1.0f + ahead * d.loco.crowdPush, 0.0f, 1.0f);
      }
    }

    // ---- DRIVE ----
    const float base = d.speed * (d.swimming ? pt.liquidSpeedScale : 1.0f);
    const float fwdDrive = base * drive * align;
    const float sideDrive = base * strafe;
    const Vec3 fwd{std::sin(d.steer.heading), 0, std::cos(d.steer.heading)};
    const Vec3 rgt{std::cos(d.steer.heading), 0, -std::sin(d.steer.heading)};
    // Do not walk into a wall already felt, per direction (DriveLocomotion).
    const bool fwdBlocked = gv.haveGround && ((fwdDrive > 0 && !gv.clear[0]) ||
                                              (fwdDrive < 0 && !gv.clear[4]));
    const bool sideBlocked = gv.haveGround && ((sideDrive > 0 && !gv.clear[2]) ||
                                               (sideDrive < 0 && !gv.clear[6]));
    Vec3 want{};
    if (!fwdBlocked) want += fwd * (fwdDrive * dtStep);
    if (!sideBlocked) want += rgt * (sideDrive * dtStep);
    // A STRIDE MUST NOT OVERSHOOT ITS WAYPOINT. The follower retires a
    // waypoint only when the feet come within ai_behavior.cpp's kWaypointRadius
    // (1.9) of it at a step boundary, and a strider covers `stride` ticks'
    // worth (~4 voxels for a man) between boundaries: it would step past, turn
    // back, step past again -- a creature circling its own waypoint. Near
    // walkers move ~1 voxel a tick and never meet this. So the step is cut to
    // end just short of the live waypoint; the next step retires it.
    if (!d.brain.path.Done()) {
      const Vec3 wp = d.brain.path.Current();
      const float wx = wp.x - c.x, wz = wp.z - c.z;
      const float dWp = std::sqrt(wx * wx + wz * wz);
      const float len = std::sqrt(want.x * want.x + want.z * want.z);
      const float limit = std::max(0.5f, dWp - 1.0f);
      if (len > limit) want = want * (limit / len);
    }

    const float hx = d.worldSize.x * 0.5f, hz = d.worldSize.z * 0.5f;
    const float h = d.worldSize.y;
    enum Refusal { kNone = 0, kClimb, kDrop, kWater, kLine };
    // ONE substep from origin `p` (feet at `p.y`) by `dv`, onto the ground of
    // the column it lands in. Writes the new origin and the swim state.
    auto stepOnce = [&](Vec3 p, Vec3 dv, bool& swim, Vec3& outP) -> Refusal {
      Vec3 q{p.x + dv.x, p.y, p.z + dv.z};
      // THE PROMOTION LINE (§2.1): once over it -- waiting -- never deeper.
      if (box.Inside(p) && box.Inside(q) && box.Depth(q) > box.Depth(p) + 1e-4f) return kLine;
      const int qx = ifloor(q.x + hx), qz = ifloor(q.z + hz);
      const int from = ifloor(p.y + 1e-3f);
      const int wt = distantGround_.WaterTop(qx, qz);
      if (d.aquatic) {
        // A FISH: only where there is water deep enough to hold it, at the
        // height it swims (kept between its bed and the surface).
        if (wt == DistantGround::kNoWater) return kWater;
        int bed = 0;
        distantGround_.ProbeGround(qx, qz, wt, bed);
        const float need = std::max(2.0f, kDistantFishMinDepthBodies * h);
        if ((float)(wt - bed) < need) return kWater;
        q.y = std::clamp(p.y, (float)bed, (float)wt - h);
        outP = q;
        return kNone;
      }
      if (wt != DistantGround::kNoWater && d.canFloat) {
        int bed = 0;
        distantGround_.ProbeGround(qx, qz, std::max(wt, from + d.stepUpCells + 1), bed);
        const float depth = (float)(wt - bed);
        const bool deep = swim ? depth >= kDistantAfloatLeave * h : depth > kDistantAfloatEnter * h;
        if (deep) {
          // AFLOAT: the waterline kAfloatShare up the body, never below its bed.
          swim = true;
          q.y = std::max((float)bed, (float)wt - kDistantAfloatShare * h);
          outP = q;
          return kNone;
        }
        // CLIMBING OUT (a bank within a step of the SURFACE, mob_afloat.cpp's
        // rule, not of the float line) or WADING (down to a bed the water
        // stands over: the water breaks the drop, as near it lands the body
        // in it rather than refusing the bank).
        int sy = 0;
        const int ref = swim ? std::max(from, wt) : from;
        const int down = d.stepDownCells + std::max(0, (int)std::ceil(depth));
        if (!distantGround_.Stand(qx, qz, ref, d.stepUpCells, down, d.headroomCells, sy))
          return distantGround_.TerrainTop(qx, qz) + 1 > ref ? kClimb : kDrop;
        swim = false;
        q.y = (float)sy;
        outP = q;
        return kNone;
      }
      int sy = 0;
      if (!distantGround_.Stand(qx, qz, from, d.stepUpCells, d.stepDownCells,
                                d.headroomCells, sy)) {
        // Which budget refused it: is there ground ABOVE the climb reach?
        const int top = distantGround_.TerrainTop(qx, qz) + 1;
        return top > from ? kClimb : kDrop;
      }
      q.y = (float)sy;
      outP = q;
      return kNone;
    };
    // The whole move in substeps; keeps the progress it made up to a refusal.
    auto tryMove = [&](Vec3 dv, bool& swim, Vec3& outP) -> Refusal {
      const float len = std::sqrt(dv.x * dv.x + dv.z * dv.z);
      const int n = std::max(1, (int)std::ceil(len / kDistantSubstepVox));
      const Vec3 sv{dv.x / (float)n, 0.0f, dv.z / (float)n};
      Vec3 p = d.origin;
      for (int k = 0; k < n; k++) {
        Vec3 q{};
        const Refusal r = stepOnce(p, sv, swim, q);
        if (r != kNone) {
          outP = p;
          return r;
        }
        p = q;
      }
      outP = p;
      return kNone;
    };
    if (want.x != 0.0f || want.z != 0.0f) {
      bool swim = d.swimming;
      Vec3 p{};
      Refusal r = tryMove(want, swim, p);
      if (r != kNone && p.x == d.origin.x && p.z == d.origin.z) {
        // WALL SLIDE (DriveLocomotion's): each axis alone, the longer first.
        const bool xFirst = std::abs(want.x) >= std::abs(want.z);
        for (int pass = 0; pass < 2; pass++) {
          const bool doX = (pass == 0) == xFirst;
          const Vec3 dv{doX ? want.x : 0.0f, 0.0f, doX ? 0.0f : want.z};
          if (dv.x == 0.0f && dv.z == 0.0f) continue;
          bool s2 = d.swimming;
          Vec3 p2{};
          const Refusal r2 = tryMove(dv, s2, p2);
          if (p2.x != d.origin.x || p2.z != d.origin.z) {
            p = p2;
            swim = s2;
            r = r2;
            break;
          }
        }
      }
      switch (r) {
        case kClimb: distant_.Note(&DistantMobs::Stats::climbRefusals); break;
        case kDrop: distant_.Note(&DistantMobs::Stats::dropRefusals); break;
        case kWater: distant_.Note(&DistantMobs::Stats::waterRefusals); break;
        case kLine: distant_.Note(&DistantMobs::Stats::lineHolds); break;
        default: break;
      }
      if (swim) distant_.Note(&DistantMobs::Stats::swimSteps);
      d.swimming = swim;
      nx.origin = p;
    }
    // A FISH ASKS FOR A HEIGHT (IntentOut::altitude, its centre's y): eased
    // toward it at its swim speed, kept inside the water.
    if (d.aquatic && out.altitudeSet) {
      const int qx = ifloor(nx.origin.x + hx), qz = ifloor(nx.origin.z + hz);
      const int wt = distantGround_.WaterTop(qx, qz);
      if (wt != DistantGround::kNoWater) {
        int bed = 0;
        distantGround_.ProbeGround(qx, qz, wt, bed);
        const float wantY = out.altitude - h * 0.5f;
        const float dy = std::clamp(wantY - nx.origin.y, -base * dtStep, base * dtStep);
        nx.origin.y = std::clamp(nx.origin.y + dy, (float)bed, std::max((float)bed, (float)wt - h));
      }
    }
    const float mdx = nx.origin.x - d.origin.x, mdz = nx.origin.z - d.origin.z;
    nx.movedLen = std::sqrt(mdx * mdx + mdz * mdz);
    nx.moved = nx.movedLen > 0.0f || nx.origin.y != d.origin.y;
    d.steer.speedNow = dtStep > 0.0f ? nx.movedLen / dtStep : 0.0f;
    d.vel = dtStep > 0.0f ? Vec3{mdx / dtStep, 0.0f, mdz / dtStep} : Vec3{};
    nexts.push_back(nx);
  }

  // ---- PHASE 2: who falls asleep (tick-start state of everyone else) ----------
  // Before the moves are filed: a sleeper a mover then reaches is woken by
  // that move, never slept through it.
  for (const Next& nx : nexts) {
    DistantMob& d = distant_.At(nx.i);
    if (d.dead) {
      d.asleep = true;
      continue;
    }
    if (nx.moved || d.landPending || d.brain.hasTarget || d.brain.hasThreat) continue;
    if (d.strokeImpactTick != 0) continue;   // D2b: a swing in flight
    const ai::Profile* prof = behaviors_.At(d.brain.profile);
    // No mind: nothing near it can change what it does, so it sleeps now
    // (a stranger that wants it comes to it, and its arrival wakes it).
    if (prof == nullptr) {
      d.asleep = true;
      d.wakeTick = 0;
      distant_.Note(&DistantMobs::Stats::sleeps);
      continue;
    }
    // A GOAL TO WALK TO keeps it awake -- unless it can never walk it (an
    // anchored or immobile body, which would otherwise think for nothing on
    // its stride forever).
    const bool canMove = prof != nullptr && prof->movement.mobile &&
                         d.groundMode == DistantGroundMode::Surface;
    if (canMove && d.brain.routine.active && !d.brain.routine.hold) continue;
    // ...and NOTHING HOSTILE NEAR: a stranger of another faction within the
    // longer of the two keep ranges (+ a margin), where either side is the
    // hostile one. Two passive strangers leave each other asleep.
    const float selfKeep = KeepRangeOf(prof);
    const bool selfHostile = prof != nullptr && prof->perception.aggro == ai::Aggro::Hostile;
    const uint32_t selfFaction = prof != nullptr ? ai::FactionId(prof->faction) : ~0u;
    const Vec3 c = snap[nx.i].valid ? snap[nx.i].centre : d.Centre();
    const float reachR = std::max(selfKeep, libMaxKeep) + kDistantSleepMarginVox;
    bool threat = false;
    if (reachR > kDistantSleepMarginVox) {
      const int rc = (int)std::ceil(reachR / (float)DistantMobs::kCellVox);
      distant_.Neighbourhood(d.cellX, d.cellZ, rc, ids);
      auto hostileAt = [&](Vec3 oc, uint32_t of, bool oh, float okeep) {
        if (of == selfFaction || !(oh || selfHostile)) return false;
        const float r = std::max(selfKeep, okeep) + kDistantSleepMarginVox;
        const float dx = oc.x - c.x, dz = oc.z - c.z;
        return dx * dx + dz * dz <= r * r;
      };
      for (const uint64_t oid : ids) {
        if (oid == d.id) continue;
        const size_t oi = indexOf(oid);
        if (oi == SIZE_MAX || !snapAt(oi).valid) continue;
        const DistantSnap& o = snapAt(oi);
        if (hostileAt(o.centre, o.faction, o.hostile, o.keep)) {
          threat = true;
          break;
        }
      }
      for (size_t k = 0; !threat && k < band.size(); k++)
        threat = hostileAt(band[k].centre, band[k].faction, band[k].hostile, 0.0f);
    }
    if (threat) continue;
    d.asleep = true;
    // ITS OWN CLOCK: a wanderer resting between legs wakes when the rest ends.
    d.wakeTick = (d.brain.wanderOwned && d.brain.routine.active && d.brain.routine.hold)
                     ? std::max(d.brain.wanderPauseUntil, tick + 1)
                     : 0;
    distant_.Note(&DistantMobs::Stats::sleeps);
  }

  // ---- PHASE 3: the moves, filed in id order (waking whom they reach) ---------
  for (const Next& nx : nexts) {
    DistantMob& d = distant_.At(nx.i);
    if (!nx.moved) {
      d.steer.speedNow = 0.0f;
      continue;
    }
    if (nx.movedLen > 0.0f) {
      d.walked += nx.movedLen;
      // Gait cycles: one full stride cycle per ~0.8 body heights walked.
      d.gaitPhase += nx.movedLen / std::max(4.0f, d.worldSize.y * 0.8f);
      d.lastMoveTick = tick;
    }
    const uint64_t id = d.id;
    distant_.Relocate(id, nx.origin);   // may refile: `d` is still valid (no
                                        // insert or erase happens here)
    // THE BORDER LATCHES (game/distant.h): a creature that ended its step
    // over the promotion line, or past the retire border, asks its scan.
    if (box.Inside(nx.origin)) distant_.MarkPromoteDue();
    const DistantMob* dm = distant_.Find(id);
    if (dm != nullptr) {
      const Vec3 cc = dm->Centre();
      const float ex = cc.x - wc.x, ey = cc.y - wc.y, ez = cc.z - wc.z;
      if (!(ex * ex + ey * ey + ez * ez <= retireReach * retireReach)) distant_.MarkRetireDue();
    }
  }
  // The sleep summary the fast path reads: anyone left awake, and the
  // earliest clock among the sleepers (this tick's new ones included).
  {
    bool awake = anyAwake;
    uint32_t nw = nextWake;
    if (awake) {
      awake = false;
      for (const DistantMob& d : distant_.All()) {
        if (d.dead) continue;
        if (!d.asleep) awake = true;
        else if (d.wakeTick != 0) nw = std::min(nw, d.wakeTick);
      }
    }
    distant_.SetSleepSummary(awake, nw);
  }
  timeOut();
}
