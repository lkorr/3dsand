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

// A promotion that is still burning (D2b) re-lights this many voxels of its
// trunk (MobSystem::IgniteLimb) -- a foothold the near burn pass then spreads
// by its own rules. Of the order of the calibration's burn probe (60).
constexpr uint32_t kDistantRelightVoxels = 40;

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
  moveGen_++;
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
  moveGen_++;
  return true;
}

bool DistantMobs::Erase(uint64_t id) {
  const size_t i = LowerBound(id);
  if (i >= list_.size() || list_[i].id != id) return false;
  Unfile(list_[i]);
  list_.erase(list_.begin() + (ptrdiff_t)i);
  moveGen_++;
  return true;
}

void DistantMobs::Clear() {
  list_.clear();
  moveGen_++;
  cells_.clear();
  arrows_.clear();
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
  moveGen_++;
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
    // STRIKING from the swing's start to its end (D2b): the statue's one
    // strike pose for the whole stroke -- windup, cut and recovery alike.
    const bool striking = !d.dead && d.strokeEndTick != 0 && tick >= d.strokeStartTick &&
                          tick < d.strokeEndTick;
    it.state = striking  ? DistantDrawState::Strike
               : walking ? DistantDrawState::Walk
                         : DistantDrawState::Idle;
    it.gaitPhase = d.gaitPhase;
    out.push_back(it);
  }
}

// ---- the arrows in the air (D2b) -----------------------------------------------
void AppendDistantArrowBodies(const DistantMobs& tier, const ItemLibrary& items,
                              uint32_t tick, float tickAlpha,
                              std::vector<RenderOnlyBody>& out) {
  if (tier.Arrows().empty()) return;
  // The arrow integrator's own gravity (game/arrows.h), per second^2: the arc
  // an abstract arrow is drawn on is the one an inbound flight converts from
  // (TickDistant's impact pass), so the two cannot disagree about the drop.
  const float g = ArrowGravityPerTick2() * 900.0f;   // voxels/s^2
  for (const DistantArrowFlight& f : tier.Arrows()) {
    const ItemDef* def = items.Named(f.ammo);
    if (def == nullptr || def->microModel < 0) continue;
    if (f.inbound) {
      // AN INBOUND FLIGHT (D3) is a real ballistic state: drawn where it is,
      // led by the frame's share of its velocity, pointing along it.
      const Vec3 p{SpellFxToFloat(f.pos[0]), SpellFxToFloat(f.pos[1]), SpellFxToFloat(f.pos[2])};
      Vec3 v{SpellFxToFloat(f.vel[0]), SpellFxToFloat(f.vel[1]), SpellFxToFloat(f.vel[2])};
      const Vec3 tip = p + v * std::clamp(tickAlpha, 0.0f, 1.0f);
      const float vl = v.len();
      v = vl > 1e-6f ? v * (1.0f / vl) : Vec3{1, 0, 0};
      const Quat q = QuatNormalize(QuatFromTo(Vec3{1, 0, 0}, v));
      RenderOnlyBody b;
      b.pos = tip - QuatRotate(q, def->arrow.tip);
      b.quat[0] = q.x;
      b.quat[1] = q.y;
      b.quat[2] = q.z;
      b.quat[3] = q.w;
      b.model = (uint32_t)def->microModel;
      out.push_back(b);
      continue;
    }
    if (f.impactTick <= f.looseTick) continue;
    // THE ARC: a parabola through both ends over the flight's own duration,
    // so the arrow leaves the archer rising and comes down on its mark.
    const float T = (float)(f.impactTick - f.looseTick) / 30.0f;
    const float t = std::clamp(((float)(tick - f.looseTick) + std::clamp(tickAlpha, 0.0f, 1.0f)) /
                                   30.0f,
                               0.0f, T);
    Vec3 to = f.to;
    if (!f.out.hit) {
      // A MISS goes past: a few voxels wide of the mark, the side from the
      // shooter's id (render-only, so any stable rule will do).
      const float dx = f.to.x - f.from.x, dz = f.to.z - f.from.z;
      const float l = std::max(1e-3f, std::sqrt(dx * dx + dz * dz));
      const float side = (f.shooter & 1u) ? 3.0f : -3.0f;
      to = Vec3{f.to.x + (-dz / l) * side + (dx / l) * 4.0f, f.to.y,
                f.to.z + (dx / l) * side + (dz / l) * 4.0f};
    }
    const Vec3 span = to - f.from;
    const float u = T > 0.0f ? t / T : 1.0f;
    const Vec3 tip{f.from.x + span.x * u, f.from.y + span.y * u + 0.5f * g * t * (T - t),
                   f.from.z + span.z * u};
    Vec3 dir{span.x / T, span.y / T + 0.5f * g * (T - 2.0f * t), span.z / T};
    const float dl = dir.len();
    dir = dl > 1e-6f ? dir * (1.0f / dl) : Vec3{1, 0, 0};
    // game/arrows.cpp ArrowXf (file-local there): the tip at `tip`, the shaft
    // along `dir` (the model's +x).
    const Quat q = QuatNormalize(QuatFromTo(Vec3{1, 0, 0}, dir));
    RenderOnlyBody b;
    b.pos = tip - QuatRotate(q, def->arrow.tip);
    b.quat[0] = q.x;
    b.quat[1] = q.y;
    b.quat[2] = q.z;
    b.quat[3] = q.w;
    b.model = (uint32_t)def->microModel;
    out.push_back(b);
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

  // ---- D2b: THE ABSTRACT FIGHTER (section 2.6, game/distant_combat.h) -----
  // Read off the LIVE rig while it still exists (the caller releases it right
  // after this returns): what it swings and with what, what it holds up, its
  // limbs' roles and the shells over them, its bow -- and its statuses (open
  // wounds become a bleed rate, a burning body keeps burning). The brain is
  // still on the Mob here, which BuildDistantKit reads for the profile's
  // style list. A corpse needs none of it.
  if (!d.dead) {
    d.kit = distant::BuildDistantKit(mob, items_);
    // A ruined limb is still in the way of the next blow, as it is near
    // (DistantKit::absorbRuined says why the table needs this).
    d.kit.absorbRuined = true;
    // A REACH OF 0 MEANS "UNLIMITED" to the style draw and the impact test
    // (`s.reach > 0` guards both), and StyleReachOn answers 0 for a rig that
    // has never been posed (its arm's bones are all at the origin). Near, the
    // next posed tick corrects it; here the kit is kept, so such a style
    // takes the profile's authored reach -- AttackReachOf's floor -- rather
    // than striking from anywhere.
    if (const ai::Profile* pr = behaviors_.At(mob.ai_.profile))
      for (distant::DistantStyle& st : d.kit.styles)
        if (!(st.reach > 0.0f)) st.reach = std::max(1.0f, pr->attack.reach);
    d.hasKit = !d.kit.limbs.empty() && d.kit.limbs.size() <= d.limbHp.size();
    const distant::DistantVitals v = distant::CaptureDistantVitals(mob, distantCalib_);
    d.bloodLossPerTick = v.bleedRate;
    d.burnFrac = v.burnFrac;
    d.burnTicksLeft = (uint16_t)std::clamp(v.burnTicksLeft, 0, 65535);
    // THE FISTS: per hand, the arm chain that serves it (HandCondition's
    // parts), what is in it, and the fist it has once it is empty -- its
    // reach the chain's live bone length, StyleReachOn's own fist term.
    for (int h = 0; h < kHands; h++) {
      DistantMob::DistantHand& H = d.hands[h];
      const Hand hand = HandAt(h);
      const int part = mob.HandPart(hand);
      if (part >= 0) {
        int n = 0;
        H.parts[n++] = (int16_t)part;
        int eff = -1;
        if (const IkChain* ch = mob.ChainForEffector(mob.skel_, part, eff))
          for (int p : ch->parts)
            if (n < 4 && p != part) H.parts[n++] = (int16_t)p;
      }
      const int slot = mob.HeldSlot(hand);
      H.shield = slot >= 0 && mob.IsShieldSlot(slot);
      H.weapon = slot >= 0 && !H.shield;
      if (H.weapon && items_ != nullptr)
        if (const ItemDef* it = items_->Named(mob.HeldItem(hand))) {
          H.weaponStrike = it->strike;
          H.weaponName = it->name;
        }
      if (const MobNaturalWeaponDef* nw = mob.HandFist(hand)) {
        H.fist = true;
        H.fistStrike = mob.StrikeProfileFor(*nw);
        H.fistName = nw->name;
        int handPart = -1;
        if (const IkChain* ch = mob.ChainForEffector(mob.skel_, nw->partIndex, handPart)) {
          const int i0 = ch->parts[0], i1 = ch->parts[1];
          const size_t nm = mob.anim_.model.size();
          if ((size_t)i0 < nm && (size_t)i1 < nm && handPart >= 0 && (size_t)handPart < nm)
            H.fistReach = (mob.anim_.model[i1].pos - mob.anim_.model[i0].pos).len() +
                          (mob.anim_.model[(size_t)handPart].pos - mob.anim_.model[i1].pos).len();
        }
        // An unposed rig (see the reach floor below): the profile's reach.
        if (!(H.fistReach > 0.0f))
          if (const ai::Profile* pr = behaviors_.At(mob.ai_.profile))
            H.fistReach = std::max(1.0f, pr->attack.reach * 0.5f);
      }
    }
    // THE BOW and, for an archer, the SIDEARM in its sheath (mob_archery.cpp
    // DrawSidearm). Its reach: a quarter of the bow arm plus the item's own
    // (StyleReachOn's held-weapon term, with the item's stated reach for the
    // live edge it does not have in the sheath -- an estimate).
    if (items_ != nullptr) {
      Hand bh = Hand::Right;
      if (const ItemDef* bow = HeldBow(mob, *items_, &bh)) {
        d.bowHand = (int8_t)HandIndex(bh);
        d.bowWindupTicks = bow->bow.windupTicks;
        d.bowDrawTicks = bow->bow.drawTicks;
        const ItemStack* st =
            mob.GetKit().Resolve(KitRef{KitSpace::Equip, (int)EquipSlotId::Sheath});
        if (st != nullptr && !st->Empty())
          if (const ItemDef* side = items_->Named(st->name); side != nullptr && !side->IsBow()) {
            d.haveSidearm = true;
            d.sidearm = side->name;
            d.sidearmStrike = side->strike;
            const DistantMob::DistantHand& H = d.hands[d.bowHand];
            d.sidearmReach = 0.25f * H.fistReach + side->reach;
          }
      }
    }
  }

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
  // THE THRASH BOUND's clock (section 2.7): a tier change, on the tick it
  // happened.
  NoteTierFlip(did, tick_);
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
Mob* MobSystem::PromoteDistant(uint64_t id, bool* spawnRefused, uint32_t tick) {
  if (spawnRefused) *spawnRefused = false;
  DistantMob* dm = distant_.Find(id);
  if (dm == nullptr) return nullptr;
  // ---- THE SNAP ONTO THE REAL GROUND (§2.4) ---------------------------------
  // It walked on DistantGround, which does not know trees, buildings or a
  // ruin floor; the fetch cache does (EntityParking::Unpark waited on the
  // chunks this reads). A living SURFACE creature that MOVED while distant is
  // put on the ground the window really has under its footprint -- up out of
  // a wall it would otherwise come back inside -- before its record is
  // patched, so the limbs are placed round the snapped origin. An ANCHORED
  // creature and one that never moved come back where they lie / stood
  // (DistantMob::movedWhileDistant). A creature that walked and then DIED at
  // this tier (D2b) is snapped too: it fell where DistantGround had it, and
  // its corpse must come down on the real ground, not inside a wall. A corpse
  // demoted dead never moved, so D0's rule for it is unchanged.
  if (dm->groundMode == DistantGroundMode::Surface && dm->movedWhileDistant && world_ != nullptr)
    SnapPromotionGround(*dm);
  const DistantMob* d = dm;
  // STILL BURNING (D2b, section 2.6): the fire is re-lit on one limb after
  // the creature is back, so it goes on visibly where it was abstract. The
  // trunk -- the biggest attached torso limb -- is the one lit (it has the
  // most to burn, and it is where a burning body's fire is seen).
  const bool relight = !d->dead && d->burnTicksLeft > 0 && d->hasKit;
  int relightLimb = -1;
  if (relight) {
    float best = -1.0f;
    for (size_t i = 0; i < d->kit.limbs.size(); i++) {
      const distant::DistantLimb& L = d->kit.limbs[i];
      if (!L.alive || L.bloodless || L.role != distant::LimbRole::Torso) continue;
      if (L.maxHp > best) {
        best = L.maxHp;
        relightLimb = (int)i;
      }
    }
  }
  const bool rebleed = !d->dead && d->bloodLossPerTick > 0.0f;
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
  NoteTierFlip(id, tick != 0 ? tick : tick_);
  if (dead) distant_.Note(&DistantMobs::Stats::promotedDead);
  // The fire, re-lit (see above) -- through the near tier's own door, after
  // the overlay, on a creature that came back alive.
  if (Mob* back = FindMobById(id); back != nullptr && back->alive_) {
    if (relight && relightLimb >= 0 &&
        IgniteLimb(id, relightLimb, kDistantRelightVoxels) > 0)
      distant_.Note(&DistantMobs::Stats::relit);
    if (rebleed) distant_.Note(&DistantMobs::Stats::rebled);
  }
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

// THE EDGE BAND's width is game/distant.h's kDistantEdgeBandVox (D3 opened
// it the other way: the near tier's list gets the distant creatures in it).
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
// A step shorter than this does not keep a creature awake (D2b, measured):
// two idle survivors standing just inside each other's personal space are
// nudged by the spacing push a ten-thousandth of a voxel a stride -- the
// push resolved against ground that refuses most of it -- and "it moved"
// kept both awake for ever. A twentieth of a voxel is no walk.
constexpr float kDistantSleepJitterVox = 0.05f;

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
  // D2b: what a watcher can read off it (ai::Actor's fight fields): how far
  // its blows land, whether it bleeds, and what its swing is doing.
  float reach = 0.0f;
  bool bleeding = false;
  ai::Action action = ai::Action::None;
};

// THE ACTOR LIST'S CAP (D2b, section 2.11). A creature's WorldView holds at
// most this many others: the nearest ones (ties by id), its own target and
// threat always among them. Think walks the list several times a step, so an
// uncapped list made a 64-a-side battle cost O(N^2) -- every fighter weighing
// every other, and an uncapped list makes a dense battle O(N^2). Nobody in a
// melee needs more than the nearest couple of dozen; the near tier's list is
// the whole crowd because the crowd is capped at 64.
constexpr size_t kDistantActorsMax = 24;

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

// ---- D2b: the abstract body, as the resolver sees it --------------------------
//
// The resolver's DistantVitals is built from the fields the tier keeps (the
// BASE part of limbHp -- the record writes every base limb first, index-
// parallel to the kit -- and the statuses), stepped, and written back. A
// thread-local scratch is reused, so a bleeding crowd allocates nothing.
void LoadVitals(const DistantMob& d, distant::DistantVitals& v) {
  const size_t n = std::min(d.kit.limbs.size(), d.limbHp.size());
  v.limbHp.assign(d.limbHp.begin(), d.limbHp.begin() + (ptrdiff_t)n);
  v.bleedRate = d.bloodLossPerTick;
  v.stumps = d.stumps;
  v.burnFrac = d.burnFrac;
  v.burnTicksLeft = d.burnTicksLeft;
  v.dead = d.dead ? distant::DistantDeath::Other : distant::DistantDeath::None;
}
void StoreVitals(const distant::DistantVitals& v, DistantMob& d) {
  for (size_t i = 0; i < v.limbHp.size() && i < d.limbHp.size(); i++) d.limbHp[i] = v.limbHp[i];
  d.bloodLossPerTick = v.bleedRate;
  d.stumps = (uint8_t)std::clamp(v.stumps, 0, 255);
  d.burnFrac = v.burnFrac;
  d.burnTicksLeft = (uint16_t)std::clamp(v.burnTicksLeft, 0, 65535);
}

// The near tier's cause name for a distant death (InternDeathCause's known
// list), so a corpse promoted from this tier reads like one that died near.
const char* DistantDeathCause(distant::DistantDeath k) {
  switch (k) {
    case distant::DistantDeath::BloodLoss: return "blood loss";
    case distant::DistantDeath::Burns: return "burnt past the death knot";
    default: return "vital limb destroyed";
  }
}

// Mob::HandCondition on the abstract body: the worst hp fraction over the arm
// chain serving hand `h` (0 if a part of it is gone), against the authored hp.
float DistantHandCondition(const DistantMob& d, int h) {
  const DistantMob::DistantHand& H = d.hands[h];
  if (H.parts[0] < 0) return 0.0f;
  float worst = 1.0f;
  for (int k = 0; k < 4; k++) {
    const int p = H.parts[k];
    if (p < 0) continue;
    if ((size_t)p >= d.kit.limbs.size() || (size_t)p >= d.limbHp.size()) return 0.0f;
    if (!d.kit.limbs[(size_t)p].alive) return 0.0f;
    const float full = d.kit.limbs[(size_t)p].maxHp;
    if (full <= 0.0f) continue;
    worst = std::min(worst, std::clamp(d.limbHp[(size_t)p] / full, 0.0f, 1.0f));
  }
  return worst;
}

// THE HELD STYLES, RE-ARMED from what the fists now hold (a grip that failed,
// an archer's sidearm): the first fist with a weapon swings it (the right
// first, as StyleUsable / the strike hand prefer); else a usable fist punches
// (its reach the arm's, MobSystem::StyleReachOn's fist term); else the held
// styles go -- "none of its styles is usable", as near. Natural weapons (the
// jaws) are untouched. `weaponReach` >= 0 replaces the held styles' reach
// (a sidearm drawn); < 0 keeps it (the same weapon in the other fist). The
// guard follows: a shield, else a blade, else nothing. A swing in flight with
// a held style is dropped (the hand that was making it has let go).
void RearmHeld(DistantMob& d, float weaponReach, uint32_t tick) {
  const DistantMob::DistantHand* w = nullptr;
  for (int h = 0; h < kHands && w == nullptr; h++)
    if (d.hands[h].weapon) w = &d.hands[h];
  const DistantMob::DistantHand* fist = nullptr;
  for (int h = 0; h < kHands && fist == nullptr; h++)
    if (d.hands[h].fist && !d.hands[h].shield) fist = &d.hands[h];
  const std::string closeName =
      d.kit.closeStyle >= 0 && d.kit.closeStyle < (int)d.kit.styles.size()
          ? d.kit.styles[(size_t)d.kit.closeStyle].name
          : std::string();
  if (d.strokeImpactTick != 0 && d.strokeStyle < d.kit.styles.size() &&
      d.kit.styles[d.strokeStyle].held) {
    d.strokeImpactTick = 0;
    d.strokeEndTick = tick;
    d.strokeTarget = 0;
  }
  std::vector<distant::DistantStyle> kept;
  kept.reserve(d.kit.styles.size());
  for (distant::DistantStyle s : d.kit.styles) {
    if (s.held) {
      if (w != nullptr) {
        s.strike = w->weaponStrike;
        s.weapon = w->weaponName;
        s.natural = false;
        if (weaponReach >= 0.0f) s.reach = weaponReach;
      } else if (fist != nullptr) {
        s.strike = fist->fistStrike;
        s.weapon = fist->fistName;
        s.natural = true;
        if (fist->fistReach > 0.0f) s.reach = fist->fistReach;
      } else {
        continue;
      }
      s.kind = distant::DominantKind(s.strike);
    }
    kept.push_back(std::move(s));
  }
  d.kit.styles.swap(kept);
  d.kit.closeStyle = -1;
  for (size_t i = 0; i < d.kit.styles.size() && !closeName.empty(); i++)
    if (d.kit.styles[i].name == closeName) d.kit.closeStyle = (int)i;
  bool shield = false;
  for (int h = 0; h < kHands; h++) shield = shield || d.hands[h].shield;
  d.kit.guard = shield ? distant::GuardClass::Shield
                : w != nullptr ? distant::GuardClass::Blade
                               : distant::GuardClass::None;
  d.armed = w != nullptr;
  d.canGuard = w != nullptr || shield;
  if (d.strokeStyle >= d.kit.styles.size()) d.strokeStyle = 0;
}

// The reach facts Think needs (ai_behavior.h SelfView::attackReach /
// strikeReach), off the kit as MobSystem::AttackReachOf / StrikeReachOf
// compute them off the rig: the longest usable style (the fallback group
// only when no real style is left), and that with the profile's floor.
void KitReach(const DistantMob& d, const ai::Profile* prof, float& attackReach,
              float& strikeReach) {
  attackReach = prof != nullptr ? prof->attack.reach : 0.0f;
  strikeReach = 0.0f;
  if (!d.hasKit) return;
  float real = 0.0f, fb = 0.0f;
  bool haveReal = false;
  for (const distant::DistantStyle& s : d.kit.styles) {
    attackReach = std::max(attackReach, s.reach);
    if (s.fallback) {
      fb = std::max(fb, s.reach);
    } else {
      haveReal = true;
      real = std::max(real, s.reach);
    }
  }
  strikeReach = haveReal ? real : fb;
}

// ONE CREATURE'S TICK-START SNAPSHOT (D1's, factored out by D3): what every
// OTHER creature reads about it. The distant tick's own actor lists AND the
// near tier's edge band (MobSystem::PublishDistantBand) are built from this
// one function, so the two tiers cannot see a creature differently. A corpse
// leaves it invalid (the dead are not actors at this tier).
void FillDistantSnap(const DistantMob& d, const ai::Library& lib, DistantGround& ground,
                     uint32_t tick, DistantSnap& s) {
  s = DistantSnap{};
  if (d.dead) return;
  s.valid = true;
  s.id = d.id;
  s.centre = d.Centre();
  s.base = Vec3{s.centre.x, d.origin.y, s.centre.z};
  s.radius = 0.5f * std::max(d.worldSize.x, d.worldSize.z);
  s.height = d.worldSize.y;
  s.heading = d.steer.heading;
  int lost = 0;
  DistantBodyFacts(d, s.hpFrac, lost);
  const ai::Profile* pr = lib.At(d.brain.profile);
  s.faction = pr != nullptr ? ai::FactionId(pr->faction) : ~0u;
  s.hostile = pr != nullptr && pr->perception.aggro == ai::Aggro::Hostile;
  s.keep = KeepRangeOf(pr);
  s.armed = d.armed;
  s.targetId = d.brain.hasTarget ? d.brain.targetId : 0;
  s.vel = d.vel;
  if (d.swimming || d.aquatic) {
    const int wt = ground.WaterTop(ifloor(s.centre.x), ifloor(s.centre.z));
    if (wt != DistantGround::kNoWater && d.worldSize.y > 0.0f)
      s.immersion = std::clamp(((float)wt - d.origin.y) / d.worldSize.y, 0.0f, 1.0f);
  }
  // D2b: what a watcher reads off it -- its blows' reach (ThreatReachOf's
  // order: the styles, else half a metre), whether it bleeds, and what its
  // swing is doing (the telegraph a defence reacts to).
  float ar = 0.0f, sr = 0.0f;
  KitReach(d, pr, ar, sr);
  s.reach = sr > 0.0f ? sr : MetresToCells(0.5f);
  s.bleeding = d.bloodLossPerTick > 0.0f;
  if (d.strokeEndTick != 0 && tick >= d.strokeStartTick)
    s.action = d.strokeImpactTick == 0         ? ai::Action::Recover
               : tick + 2 < d.strokeImpactTick ? ai::Action::Windup
                                               : ai::Action::Cut;
}

// ...and that snapshot as an ai::Actor (the shape both tiers' lists hold).
ai::Actor DistantActorOf(const DistantSnap& s) {
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
  a.reach = s.reach;
  a.bleeding = s.bleeding;
  a.action = s.action;
  return a;
}

// THE LOOSE'S ERROR (game/mob_archery.cpp Spread: up to `spread` radians in
// yaw and in pitch, two counter-based draws), on this tier's own salt -- an
// inbound arrow is aimed the way a near archer aims, and misses the way one
// does.
Vec3 DistantSpread(Vec3 dir, float spread, uint64_t id, uint32_t tick) {
  if (!(spread > 0.0f)) return dir;
  const uint32_t h = rng::Hash3((uint32_t)id ^ ai::kSaltDistantArrow, tick, 11u);
  const float u1 = (float)(h & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;
  const float u2 = (float)((h >> 16) & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;
  const float yaw = std::atan2(dir.x, dir.z) + u1 * spread;
  const float pitch =
      std::clamp(std::asin(std::clamp(dir.y, -1.0f, 1.0f)) + u2 * spread, -1.55f, 1.55f);
  const float cp = std::cos(pitch);
  return Vec3{cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)};
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

// ============================================================================
// THE EDGE (docs/PLAN_distant_npcs.md 2.7, package D3; game/distant.h)
// ============================================================================

void MobSystem::PublishDistantBand(uint32_t tick, Vec3 wlo) {
  distantBand_.clear();
  if (distant_.Count() == 0 || !CurrentTuning().distant.enabled) {
    bandHave_ = false;
    bandIds_.clear();
    return;
  }
  const IVec3 wo{ifloor(wlo.x / (float)kChunk), ifloor(wlo.y / (float)kChunk),
                 ifloor(wlo.z / (float)kChunk)};
  // WHO IS IN THE BAND is a matter of POSITION only, so it is recomputed only
  // when somebody moved, joined, left or died (the tier's move generation) or
  // the window moved; a still tier costs a walk of its members (rule 2).
  if (!bandHave_ || bandGen_ != distant_.MoveGen() || wo.x != bandWo_.x || wo.y != bandWo_.y ||
      wo.z != bandWo_.z) {
    bandHave_ = true;
    bandGen_ = distant_.MoveGen();
    bandWo_ = wo;
    bandIds_.clear();
    const float n = (float)kWorldN;
    thread_local std::vector<std::pair<float, uint64_t>> in;
    in.clear();
    for (const DistantMob& d : distant_.All()) {
      if (d.dead) continue;
      // Distance from the body centre to the window BOX (0 inside it: a
      // creature held at the promotion line is in the band too).
      const Vec3 c = d.Centre();
      const float dx = std::max({wlo.x - c.x, 0.0f, c.x - (wlo.x + n)});
      const float dy = std::max({wlo.y - c.y, 0.0f, c.y - (wlo.y + n)});
      const float dz = std::max({wlo.z - c.z, 0.0f, c.z - (wlo.z + n)});
      const float out = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (out <= kDistantEdgeBandVox) in.push_back({out, d.id});
    }
    if (in.size() > kDistantBandMax) {
      // The nearest to the window first, ties by id: a total order, so the
      // members are a pure function of the tier and the window.
      std::nth_element(in.begin(), in.begin() + (ptrdiff_t)kDistantBandMax, in.end());
      in.resize(kDistantBandMax);
      distant_.Note(&DistantMobs::Stats::bandCapped);
    }
    for (const auto& e : in) bandIds_.push_back(e.second);
    std::sort(bandIds_.begin(), bandIds_.end());
  }
  // THE ACTORS, built from each member's TICK-START state (this runs before
  // TickDistant moves anybody) by the function the distant tick's own lists
  // use, in id order.
  DistantSnap snap;
  for (const uint64_t id : bandIds_) {
    const DistantMob* d = distant_.Find(id);
    if (d == nullptr || d->dead) continue;
    FillDistantSnap(*d, behaviors_, distantGround_, tick, snap);
    if (snap.valid) distantBand_.push_back(DistantActorOf(snap));
  }
  if (!distantBand_.empty()) {
    distant_.Note(&DistantMobs::Stats::bandPublished, distantBand_.size());
    distant_.NoteMax(&DistantMobs::Stats::bandPeak, distantBand_.size());
  }
}

const ai::Actor* MobSystem::EdgeActor(uint64_t id) const {
  auto it = std::lower_bound(distantBand_.begin(), distantBand_.end(), id,
                             [](const ai::Actor& a, uint64_t v) { return a.id < v; });
  return it != distantBand_.end() && it->id == id ? &*it : nullptr;
}

bool MobSystem::QueueEdgeBlow(const Mob& attacker, int styleIndex, uint64_t targetId,
                              uint32_t tick) {
  if (!CurrentTuning().distant.enabled) return false;
  const DistantMob* d = distant_.Find(targetId);
  if (d == nullptr || d->dead || attacker.def_ == nullptr) return false;
  NearEdgeBlow b;
  b.attacker = attacker.id_;
  b.target = targetId;
  b.tick = tick;
  const Vec3 ws = attacker.def_->worldSize;
  b.from = Vec3{attacker.origin_.x + ws.x * 0.5f, attacker.origin_.y + ws.y * 0.5f,
                attacker.origin_.z + ws.z * 0.5f};
  // THE ATTACKER'S KIT AS IT IS AT THE CUT: its styles (reach, strike, the
  // table row) -- not its body, which nobody reads (ResolveStrike reads the
  // defender's).
  b.kit = distant::BuildDistantKit(attacker, items_, /*withBody=*/false);
  // StartStroke resolved the profile's style to the held weapon's FORM; the
  // kit keeps both, so either names the row.
  for (size_t i = 0; i < b.kit.styles.size() && b.style < 0; i++)
    if (b.kit.styles[i].form == styleIndex || b.kit.styles[i].style == styleIndex)
      b.style = (int)i;
  if (b.style < 0)
    if (const AttackStyle* sty = styles_.At(styleIndex))
      for (size_t i = 0; i < b.kit.styles.size() && b.style < 0; i++)
        if (b.kit.styles[i].name == sty->name) b.style = (int)i;
  if (b.style < 0) {
    // A swing the profile does not list (a script's, a gate's ForceAttack):
    // there is no table row to roll it on. Counted, not guessed.
    distant_.Note(&DistantMobs::Stats::edgeNearUnmatched);
    return false;
  }
  edgeBlows_.push_back(std::move(b));
  distant_.Note(&DistantMobs::Stats::edgeNearBlows);
  return true;
}

bool MobSystem::QueueEdgeArrowOut(const NearArrowOut& out) {
  if (!CurrentTuning().distant.enabled) return false;
  const DistantMob* d = distant_.Find(out.target);
  if (d == nullptr || d->dead) return false;
  edgeArrowsOut_.push_back(out);
  distant_.Note(&DistantMobs::Stats::edgeArrowsOut);
  return true;
}

// ---- THE THRASH BOUND (section 2.7) -------------------------------------------
void MobSystem::NoteTierFlip(uint64_t id, uint32_t tick) {
  TierFlipLog& L = tierFlips_[id];
  if (L.n < (uint8_t)kDistantMaxFlips) {
    L.t[L.n++] = tick;
  } else {
    for (int k = 1; k < kDistantMaxFlips; k++) L.t[k - 1] = L.t[k];
    L.t[kDistantMaxFlips - 1] = tick;
  }
  distant_.Note(&DistantMobs::Stats::flips);
  // Bounded (rule 2): a log whose newest flip has left the window says
  // nothing any more. The erase SET is a function of the logs and the tick,
  // so the map's iteration order cannot change an outcome.
  if (tierFlips_.size() > 1024)
    for (auto it = tierFlips_.begin(); it != tierFlips_.end();) {
      const TierFlipLog& f = it->second;
      const uint32_t newest = f.n > 0 ? f.t[f.n - 1] : 0;
      it = (f.n == 0 || (newest <= tick && tick - newest >= kDistantFlipWindowTicks))
               ? tierFlips_.erase(it)
               : std::next(it);
    }
}

int MobSystem::TierFlipsWithin(uint64_t id, uint32_t tick) const {
  const auto it = tierFlips_.find(id);
  if (it == tierFlips_.end()) return 0;
  int n = 0;
  // A flip stamped LATER than `tick` (a gate that rewound its clock) is not
  // counted: the log forgets rather than blocks.
  for (int k = 0; k < it->second.n; k++)
    n += (it->second.t[k] <= tick && tick - it->second.t[k] < kDistantFlipWindowTicks) ? 1 : 0;
  return n;
}

bool MobSystem::DistantFlipBudgetOk(uint64_t id, uint32_t tick) const {
  // The promotion AND the demotion that may follow it must still fit.
  return TierFlipsWithin(id, tick) + 2 <= kDistantMaxFlips;
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
    distant_.TouchMoveGen();
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
  // The dead too (D2b): one that walked and then died here is snapped like
  // the living (PromoteDistant). A corpse demoted dead never moved.
  if (world_ == nullptr || d.groundMode != DistantGroundMode::Surface || !d.movedWhileDistant)
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
                            const std::vector<ai::Actor>* nearActors,
                            std::vector<ParticleSpawn>* spawns) {
  // D3: what the near tier handed over since the last call (a near stroke's
  // blow at a distant creature, a near arrow that left the window at one) is
  // work even for an empty or sleeping tier, and an INBOUND arrow in the air
  // (a distant archer's shot at a near creature) flies on after its shooter
  // was promoted -- so the tier's arrows are kept while it is empty.
  const bool edgeWork = !edgeBlows_.empty() || !edgeArrowsOut_.empty();
  // Inbound arrows queued LAST tick that nobody took (a harness that steps
  // this tier without the archery phase that drains them): dropped, counted
  // -- bounded (rule 2), never carried into a tick they do not belong to.
  if (!edgeArrowsIn_.empty()) {
    distant_.Note(&DistantMobs::Stats::edgeArrowsExpired, edgeArrowsIn_.size());
    edgeArrowsIn_.clear();
  }
  if (distant_.Count() == 0 && distant_.Arrows().empty() && !edgeWork) {
    nearBand_.clear();
    return;
  }
  const Tuning::Distant& tune = CurrentTuning().distant;
  // THE SWITCH OFF: RetireDistant parks everyone this very tick (with a park
  // function); without one they stay, frozen, as D0 kept them. What the edge
  // queued goes: nothing is left to land it on.
  if (!tune.enabled) {
    edgeBlows_.clear();
    edgeArrowsOut_.clear();
    distant_.ArrowsMut().clear();
    return;
  }
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  // WHERE THE TIME GOES (section 2.11): wall time per phase, summed in locals
  // and noted once at the end. MEASUREMENTS -- nothing below reads them.
  auto nsSince = [](Clock::time_point a) -> uint64_t {
    return (uint64_t)std::max<int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - a).count());
  };
  uint64_t nsSelf = 0, nsWorld = 0, nsThink = 0, nsMove = 0;
  auto timeOut = [&]() { distant_.Note(&DistantMobs::Stats::tickNs, nsSince(t0)); };
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
  // Everybody asleep (or dead), no sleeper's clock due and no arrow in the
  // air: not one creature is looked at. 512 sleepers cost two compares a tick.
  if (!distant_.AnyAwakeHint() && tick < distant_.NextWakeTick() && distant_.Arrows().empty() &&
      !edgeWork) {
    timeOut();
    return;
  }
  const distant::DistantCalib& cal = distantCalib_;
  const Tuning::Gear& gear = CurrentTuning().gear;
  thread_local distant::DistantVitals vit;
  // ---- THE DEAD, and how (the impact pass, the statuses, a stump paid late) ----
  auto kill = [&](DistantMob& d, distant::DistantDeath why) {
    d.dead = true;
    d.cause = DistantDeathCause(why);
    d.deathKind = (uint8_t)why;
    // The near tier's death order (EnterLoadedDead keeps it): this tier's
    // deaths take their turn in the same sequence.
    d.deathSeq = ++deathSeq_;
    d.deathTick = tick;
    // ASLEEP FOREVER (section 2.8), holding nothing: no swing, no draw, no
    // bleed or fire to step. Its statue lies where it stood. Nothing is
    // booked in rises_ -- infection is not modelled at this tier (D2a's open
    // item), and a corpse's rising travels in its record.
    d.asleep = true;
    d.wakeTick = 0;
    d.strokeImpactTick = d.strokeEndTick = 0;
    d.strokeTarget = 0;
    d.drawLooseTick = 0;
    d.drawTarget = 0;
    d.bloodLossPerTick = 0.0f;
    d.burnTicksLeft = 0;
    d.steer.speedNow = 0.0f;
    d.vel = Vec3{};
    d.statueRestY = d.origin.y;
    distant_.TouchMoveGen();   // the dead leave the published edge band
    distant_.Note(&DistantMobs::Stats::deaths);
    switch (why) {
      case distant::DistantDeath::BloodLoss: distant_.Note(&DistantMobs::Stats::deathsBlood); break;
      case distant::DistantDeath::Burns: distant_.Note(&DistantMobs::Stats::deathsBurns); break;
      default: distant_.Note(&DistantMobs::Stats::deathsVital); break;
    }
  };
  // A STUMP PAID LATE (D2b): a creature whose only status is open stumps
  // may have slept; the ticks it slept through are paid now, in one sum
  // (distant::StepStumps -- exact), before anything reads or hurts it. One
  // that bled out inside them dies on the tick it would have.
  auto catchUpStumps = [&](DistantMob& d) {
    if (d.dead || !d.hasKit || d.stumps == 0 || d.statusTick == 0 || tick <= d.statusTick + 1)
      return;
    const int missed = (int)(tick - d.statusTick - 1);
    LoadVitals(d, vit);
    int diedAfter = -1;
    distant::StepStumps(vit, d.kit, cal, missed, &diedAfter);
    StoreVitals(vit, d);
    d.statusTick = tick - 1;
    distant_.Note(&DistantMobs::Stats::bleedTicks, (uint64_t)missed);
    if (vit.dead != distant::DistantDeath::None) {
      kill(d, vit.dead);
      if (diedAfter > 0) d.deathTick = tick - 1 - (uint32_t)(missed - diedAfter);
    }
  };
  // ...and the clock of one falling asleep with them: the tick it bleeds out.
  auto stumpClock = [&](DistantMob& d) {
    if (d.stumps == 0 || !d.hasKit) return;
    LoadVitals(d, vit);
    const int left = distant::StumpTicksLeft(vit, d.kit, cal);
    if (left >= INT32_MAX - 1) return;
    const uint32_t dieAt = tick + 1 + (uint32_t)left;
    d.wakeTick = d.wakeTick == 0 ? dieAt : std::min(d.wakeTick, dieAt);
  };

  // ---- WHO STEPS ----------------------------------------------------------------
  // The dead: never. The asleep: not, unless their clock came round. The
  // ENGAGED (a target, a threat, a landing to make, a swing or a draw in
  // flight): every tick. Everyone else awake: on the idle stride, phase
  // (tick + id), moving stride ticks' worth when they do. Beside them, D2b's
  // two lists, both in id order: the creatures whose swing lands or whose
  // bow looses THIS tick (`due`), and those bleeding or burning (`status`,
  // stepped every tick whether or not their turn to think came round).
  const int stride = std::max(1, tune.idleStride);
  const float dt = 1.0f / 30.0f;
  thread_local std::vector<size_t> steppers, due, status;
  steppers.clear();
  due.clear();
  status.clear();
  bool anyAwake = false;
  uint32_t nextWake = UINT32_MAX;
  uint64_t engagedSteps = 0;
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
    // A sleeper with an open stump, woken (by its clock or a neighbour):
    // what it bled while it slept, first.
    catchUpStumps(d);
    if (d.dead) continue;
    anyAwake = true;
    // A FINISHED SWING frees the arm (its impact was resolved on its own
    // tick, below, before this one could come round).
    if (d.strokeEndTick != 0 && tick >= d.strokeEndTick) {
      d.strokeEndTick = 0;
      d.strokeImpactTick = 0;
      d.strokeTarget = 0;
    }
    const bool swinging = d.strokeEndTick != 0;
    const bool drawing = d.drawLooseTick != 0;
    if ((d.strokeImpactTick != 0 && d.strokeImpactTick <= tick) ||
        (drawing && d.drawLooseTick <= tick))
      due.push_back(i);
    if (d.bloodLossPerTick > 0.0f || d.burnTicksLeft > 0 || d.stumps > 0) status.push_back(i);
    const bool engaged =
        d.brain.hasTarget || d.brain.hasThreat || d.landPending || swinging || drawing;
    if (!engaged && (tick + (uint32_t)d.id) % (uint32_t)stride != 0) continue;
    engagedSteps += engaged ? 1u : 0u;
    steppers.push_back(i);
  }
  // ANY arrow in the air is this tick's work (D3): one due lands, an inbound
  // one flies a tick, one whose target was promoted converts to a real one.
  const bool arrowsDue = !distant_.Arrows().empty();
  if (steppers.empty() && due.empty() && status.empty() && !arrowsDue && !edgeWork) {
    distant_.SetSleepSummary(anyAwake, nextWake);
    timeOut();
    return;
  }
  if (!steppers.empty()) distant_.Note(&DistantMobs::Stats::tickCalls);
  if (engagedSteps != 0) distant_.Note(&DistantMobs::Stats::stepsEngaged, engagedSteps);

  // ---- THE SNAPSHOT: everyone living, AS OF NOW ------------------------------
  // What every step reads about every OTHER creature. Built EAGERLY for the
  // steppers, before any of them decides (they are the only creatures phase 1
  // writes), and LAZILY for the rest, which nothing this tick changes before
  // the impact pass has read them -- so every read is tick-start state and the
  // step order cannot matter (§2.12), and a lone walker among 512 sleepers
  // builds a handful of entries, not 512.
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
    FillDistantSnap(distant_.At(i), behaviors_, distantGround_, tick, snap[i]);
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
  // ...and each living creature as an ai::Actor, built once per tick from its
  // snapshot (every stepper that sees it copies the same one).
  thread_local std::vector<ai::Actor> actorOf;
  thread_local std::vector<uint8_t> actorBuilt;
  actorOf.resize(distant_.Count());
  actorBuilt.assign(distant_.Count(), 0);
  auto actorAt = [&](size_t i) -> const ai::Actor& {
    if (!actorBuilt[i]) {
      actorBuilt[i] = 1;
      actorOf[i] = DistantActorOf(snapAt(i));
    }
    return actorOf[i];
  };
  // THE NEIGHBOURHOODS, ONCE PER CELL AND REACH PER TICK (section 2.11).
  // Every creature filed within `rc` cells of a cell, as TIER INDICES in id
  // order -- asked by perception, spacing and the sleep test of every
  // creature in that cell. A battle packs dozens of creatures into a few
  // cells, and walking + sorting + id-searching the same neighbourhood once
  // per creature was the largest single cost of an engaged step (measured:
  // 375 of 795 us a tick for 64 a side). The hash does not change until
  // phase 3, so one list per (cell, reach) is the same list every asker got.
  thread_local std::vector<uint64_t> ids;
  thread_local std::unordered_map<uint64_t, uint32_t> nbKey;
  thread_local std::vector<std::vector<uint32_t>> nbPool;
  nbKey.clear();
  size_t nbUsed = 0;
  auto neighbours = [&](int cx, int cz, int rcells) -> uint32_t {
    const uint64_t key = ((uint64_t)(uint32_t)cx << 32) |
                         ((uint64_t)((uint32_t)cz & 0xFFFFFFu) << 8) |
                         (uint64_t)((uint32_t)rcells & 0xFFu);
    const auto it = nbKey.find(key);
    if (it != nbKey.end()) return it->second;
    if (nbUsed == nbPool.size()) nbPool.emplace_back();
    std::vector<uint32_t>& v = nbPool[nbUsed];
    v.clear();
    distant_.Neighbourhood(cx, cz, rcells, ids);   // id-sorted
    for (const uint64_t oid : ids) {
      const size_t oi = indexOf(oid);
      if (oi != SIZE_MAX) v.push_back((uint32_t)oi);
    }
    nbKey.emplace(key, (uint32_t)nbUsed);
    return (uint32_t)nbUsed++;
  };
  const PromoBox box = PromoBoxOf(wo);
  const Vec3 wc{wlo.x + 0.5f * (float)kWorldN, wlo.y + 0.5f * (float)kWorldN,
                wlo.z + 0.5f * (float)kWorldN};
  const float retireReach = tune.radius + tune.hysteresis;
  const uint64_t nsSetup = nsSince(t0);

  // ---- PHASE 1: every stepper decides, writing only itself -------------------
  struct Next {
    size_t i;
    Vec3 origin;
    bool moved;
    float movedLen;
  };
  thread_local std::vector<Next> nexts;
  nexts.clear();
  thread_local std::vector<ai::Actor> actors;
  struct Cand {
    float d2;
    uint64_t id;
    int32_t idx;     // tier index, or band index
    bool fromBand;
    bool keep;       // itself, or its target: never capped away
  };
  thread_local std::vector<Cand> cand;
  const auto& pt = CurrentTuning().player;
  for (const size_t i : steppers) {
    const auto tStep = Clock::now();
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
      nsSelf += nsSince(tStep);
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
    // D2b: how far it can reach, off its kit (MobSystem::AttackReachOf /
    // StrikeReachOf's numbers, which the near tier hands Think the same way).
    KitReach(d, prof, self.attackReach, self.strikeReach);
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
    self.drawing = d.drawLooseTick != 0;   // the string is back: hold the facing

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
    const auto tWorld = Clock::now();
    nsSelf += (uint64_t)std::max<int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(tWorld - tStep).count());

    // ---- THE WORLD: distant creatures round it + the near band ----
    // Its own keep (or scent) range, and at least its own 3x3 cells; at most
    // kDistantActorsMax others, the nearest (its target always among them).
    const float reachR = std::max({KeepRangeOf(prof), prof->perception.scentRange,
                                   (float)DistantMobs::kCellVox}) +
                         kDistantSleepMarginVox;
    const float reachR2 = reachR * reachR;
    const int rc = (int)std::ceil(reachR / (float)DistantMobs::kCellVox);
    const uint64_t tgt = d.brain.hasTarget ? d.brain.targetId : 0;
    cand.clear();
    for (const uint32_t oi : nbPool[neighbours(d.cellX, d.cellZ, rc)]) {
      const DistantSnap& s = snapAt(oi);
      if (!s.valid) continue;
      const float dx = s.centre.x - c.x, dz = s.centre.z - c.z;
      const float d2 = dx * dx + dz * dz;
      if (s.id != d.id && d2 > reachR2) continue;
      cand.push_back({d2, s.id, (int32_t)oi, false, s.id == d.id || s.id == tgt});
    }
    for (size_t k = 0; k < band.size(); k++) {
      const ai::Actor& a = band[k];
      const float dx = a.centre.x - c.x, dz = a.centre.z - c.z;
      const float d2 = dx * dx + dz * dz;
      if (d2 <= reachR2) cand.push_back({d2, a.id, (int32_t)k, true, a.id == tgt});
    }
    if (cand.size() > kDistantActorsMax + 1) {
      // The kept, then the nearest, ties by id: a total order, so the set
      // that survives is a pure function of the tick-start snapshot.
      std::nth_element(cand.begin(), cand.begin() + (ptrdiff_t)kDistantActorsMax, cand.end(),
                       [](const Cand& a, const Cand& b) {
                         if (a.keep != b.keep) return a.keep;
                         if (a.d2 != b.d2) return a.d2 < b.d2;
                         return a.id < b.id;
                       });
      cand.resize(kDistantActorsMax + 1);   // + itself
      distant_.Note(&DistantMobs::Stats::actorCaps);
    }
    // In ID ORDER, as the near tier's list is (§2.12).
    std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.id < b.id; });
    actors.clear();
    for (const Cand& k : cand) {
      // NEVER A PLAYER (section 2.7, D3's assertion): the band is built
      // without them, and this is the point a regression would show at --
      // counted (Stats::playerActorsSeen, which `distant-edge` holds at 0)
      // and refused.
      if (ai::IsPlayerActorId(k.id)) {
        distant_.Note(&DistantMobs::Stats::playerActorsSeen);
        continue;
      }
      actors.push_back(k.fromBand ? band[(size_t)k.idx] : actorAt((size_t)k.idx));
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
    const auto tThink = Clock::now();
    nsWorld += (uint64_t)std::max<int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(tThink - tWorld).count());

    // ---- THINK ----
    ai::IntentOut out;
    const bool thought = ai::Think(d.brain, behaviors_, self, gv, wv, tick, dtStep, out);
    const auto tMove = Clock::now();
    nsThink += (uint64_t)std::max<int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(tMove - tThink).count());
    if (!thought) {
      nexts.push_back(nx);
      continue;
    }

    // ---- THE FIGHT'S INTENTS (D2b, section 2.6) ----
    // Started here, writing only this creature; RESOLVED in the impact pass
    // after every step, against tick-start snapshots. An arm already busy (a
    // swing in flight, a bow drawn) drops the request, as BeginStroke drops a
    // request that arrives mid-stroke and DecideIntent drops a swing through
    // a drawn bow: the AI's own cadence clock has already advanced.
    if (out.cast) distant_.Note(&DistantMobs::Stats::combatHolds);   // casters HOLD (5.3)
    const bool armBusy = d.strokeEndTick != 0 || d.drawLooseTick != 0;
    if (out.attack) {
      // THE STYLE, as PickAttackStyle draws one: usable, reaching the
      // request's distance with the body's slack, the fallback group dropped
      // while a real style survives, the close style first inside its range.
      const int si = d.hasKit && !armBusy
                         ? distant::PickDistantStyle(d.kit, out.request.distance, d.id, tick)
                         : -1;
      if (si < 0) {
        distant_.Note(&DistantMobs::Stats::strokesRefused);
      } else {
        const distant::DistantStyle& st = d.kit.styles[(size_t)si];
        // THE IMPACT: the table's MEASURED ticks from a stroke's start to its
        // first contact (which already holds the tempo and the injured arms of
        // the fights it was measured in); the authored windup + half the cut
        // where the table has no row.
        const distant::StyleCalib& sc = cal.Style(st.name);
        const int impact = std::max(
            1, sc.impactTicks > 0.0f ? (int)std::lround(sc.impactTicks) : st.impactTicks);
        d.strokeStyle = (uint32_t)si;
        d.strokeTarget = out.request.targetId;
        d.strokeStartTick = tick;
        d.strokeImpactTick = tick + (uint32_t)impact;
        d.strokeEndTick = tick + (uint32_t)std::max(impact + 1, st.totalTicks);
        distant_.Note(&DistantMobs::Stats::strokes);
      }
    } else if (out.shoot) {
      // THE DRAW (MobArcheryTick's BeginDraw): a bow in a fist and an arrow
      // for it; the loose after the bow's windup + its pull x the profile's
      // `draw`, then the profile's hold + a counter-based jitter.
      if (!armBusy && d.hasKit && d.kit.ranged && d.ammo > 0 && d.bowHand >= 0 &&
          out.shootRequest.targetId != 0) {
        const ai::RangedTuning& rt = prof->ranged;
        const int drawTo = d.bowWindupTicks +
                           std::max(1, (int)std::lround((double)d.bowDrawTicks * (double)rt.draw));
        const int hold =
            (int)rt.holdTicks +
            (rt.holdJitter > 0
                 ? (int)(rng::Hash3((uint32_t)d.id ^ ai::kSaltDistantArrow, tick, 8u) %
                         (rt.holdJitter + 1u))
                 : 0);
        d.drawStartTick = tick;
        d.drawTarget = out.shootRequest.targetId;
        d.drawLooseTick = tick + (uint32_t)std::max(1, drawTo + hold);
        distant_.Note(&DistantMobs::Stats::draws);
      } else {
        distant_.Note(&DistantMobs::Stats::strokesRefused);
      }
    }

    // ---- STEER (the near tier's own function) ----
    d.steer.desiredHeading = out.desiredHeading;
    const float align = SteerStep(d.steer, d.loco, d.speed,
                                  std::numeric_limits<float>::infinity(), 1.0f, 1.0f, dtStep);
    // ANCHORED: it thinks and it turns, and it never moves (§2.4).
    if (d.groundMode == DistantGroundMode::Anchored) {
      nexts.push_back(nx);
      nsMove += nsSince(tMove);
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
      for (const uint32_t oi : nbPool[neighbours(d.cellX, d.cellZ, 1)]) {
        if (oi == i) continue;
        const DistantSnap& s = snapAt(oi);
        if (!s.valid) continue;
        pushFrom(s.id, s.base, s.radius);
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
    nsMove += nsSince(tMove);
  }

  // ==== THE IMPACT PASS (D2b, section 2.6; D3, section 2.7) =====================
  //
  // Every outcome due this tick is RESOLVED first -- each against its target as
  // it stood at the start of the tick (the snapshot's positions and the hp no
  // blow of this tick has touched yet) -- and only then APPLIED, in a fixed
  // order: the near tier's blows at distant creatures (attacker id), the
  // swings and looses by attacker id, then the arrows landing in the order
  // they were loosed, then the distant blows on NEAR creatures (attacker id).
  // So two blows on one body, or two bodies trading blows on the same tick,
  // come out the same whichever was stepped first: the resolver is pure
  // (game/distant_combat.h) and nothing it reads is written until every roll
  // is taken.
  //
  // D3, THE EDGE: a target that is a NEAR creature of the band is rolled
  // against that creature as it stands now -- the near creature loop has not
  // run this tick, so that IS its tick-start state -- and a hit enters it
  // through its own door (ApplyEdgeBlow). An arrow at one is flown for real
  // and handed to ArrowFlights where it crosses into the window.
  const auto tCombat = Clock::now();
  struct Blow {
    size_t def;               // the defender's tier index
    distant::StrikeOutcome o;
  };
  thread_local std::vector<Blow> blows;
  thread_local std::vector<size_t> touched;
  blows.clear();
  touched.clear();
  double hpDealt = 0.0;
  auto planarDist = [](Vec3 a, Vec3 b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
  };
  // A near creature of the band by id (the band is id-sorted), or null.
  auto bandFind = [&](uint64_t id) -> const ai::Actor* {
    auto it = std::lower_bound(band.begin(), band.end(), id,
                               [](const ai::Actor& a, uint64_t v) { return a.id < v; });
    return it != band.end() && it->id == id ? &*it : nullptr;
  };
  // THE NEAR DEFENDERS of this tick's distant swings: each one's body as the
  // roll reads it (its kit: limb roles, the shells over them, its guard; its
  // limbs' hp), off the live rig ONCE and before any blow of this tick is
  // applied, so two distant blows on one near body resolve against the same
  // state whatever their order. A ruined limb is still in the way, as near.
  struct NearDef {
    uint64_t id = 0;
    bool ok = false;
    distant::DistantKit kit;
    std::vector<float> hp;
  };
  thread_local std::vector<NearDef> nearDefs;
  nearDefs.clear();
  auto nearDef = [&](uint64_t id) -> int {
    for (size_t k = 0; k < nearDefs.size(); k++)
      if (nearDefs[k].id == id) return nearDefs[k].ok ? (int)k : -1;
    NearDef n;
    n.id = id;
    const Mob* m = FindMobById(id);
    if (m != nullptr && m->alive_ && !m->PlayerControlled() && OwnedHere(*m)) {
      n.kit = distant::BuildDistantKit(*m, items_);
      n.kit.absorbRuined = true;
      for (int li = 0; li < m->AppendedBase(); li++) n.hp.push_back(m->LimbHpAt(li));
      n.ok = !n.kit.limbs.empty() && n.hp.size() == n.kit.limbs.size();
    }
    nearDefs.push_back(std::move(n));
    return nearDefs.back().ok ? (int)nearDefs.size() - 1 : -1;
  };
  // DISTANT HITS NEAR, resolved, waiting for the apply below.
  struct NearHit {
    size_t att;                // the attacker's tier index
    uint64_t target;           // the near creature
    distant::StrikeOutcome o;
    distant::DistantStyle style;
  };
  thread_local std::vector<NearHit> nearHits;
  nearHits.clear();

  // ---- (0) THE EDGE, INWARD (D3): what the near tier handed over -----------
  // NEAR HITS DISTANT: every near stroke at a distant creature that reached
  // its cut since the last call, in attacker-id order (then the tick it cut),
  // rolled on the attacker's own kit against the defender's tick-start state
  // -- the same roll a distant swing takes; then applied with the rest.
  if (!edgeBlows_.empty()) {
    thread_local std::vector<NearEdgeBlow> inb;
    inb.clear();
    inb.swap(edgeBlows_);
    std::sort(inb.begin(), inb.end(), [](const NearEdgeBlow& x, const NearEdgeBlow& y) {
      return x.attacker != y.attacker ? x.attacker < y.attacker : x.tick < y.tick;
    });
    for (const NearEdgeBlow& b : inb) {
      const size_t ti = indexOf(b.target);
      if (ti == SIZE_MAX || !snapAt(ti).valid || !distant_.At(ti).hasKit || b.style < 0 ||
          b.style >= (int)b.kit.styles.size()) {
        distant_.Note(&DistantMobs::Stats::edgeNearNoTarget);
        continue;
      }
      const DistantMob& t = distant_.At(ti);
      LoadVitals(t, vit);
      const distant::StrikeOutcome o =
          distant::ResolveStrike(b.kit, t.kit, vit.limbHp, b.style,
                                 planarDist(b.from, snapAt(ti).centre), {b.attacker, b.tick},
                                 cal, gear);
      switch (o.result) {
        case distant::StrikeResult::Hit: distant_.Note(&DistantMobs::Stats::edgeNearHits); break;
        case distant::StrikeResult::Miss: distant_.Note(&DistantMobs::Stats::edgeNearMisses); break;
        case distant::StrikeResult::Blocked:
          distant_.Note(&DistantMobs::Stats::edgeNearBlocked);
          break;
        case distant::StrikeResult::OutOfReach:
          distant_.Note(&DistantMobs::Stats::edgeNearOutOfReach);
          break;
        case distant::StrikeResult::NoTarget:
          distant_.Note(&DistantMobs::Stats::edgeNearNoTarget);
          break;
      }
      if (o.hit) blows.push_back({ti, o});
    }
    inb.clear();
  }
  // A NEAR ARROW THAT LEFT THE WINDOW at a distant creature: rolled over the
  // WHOLE shot's distance (the table's knots are a shot's, loose to target),
  // landing after the flight time it has LEFT (exit to target at the speed it
  // left with) -- an ordinary arrow in the air from here, applied below on
  // its impact tick like any other.
  if (!edgeArrowsOut_.empty()) {
    thread_local std::vector<NearArrowOut> outs;
    outs.clear();
    outs.swap(edgeArrowsOut_);
    std::sort(outs.begin(), outs.end(), [](const NearArrowOut& x, const NearArrowOut& y) {
      return x.shooter != y.shooter ? x.shooter < y.shooter : x.tick < y.tick;
    });
    for (const NearArrowOut& o : outs) {
      const size_t ti = indexOf(o.target);
      if (ti == SIZE_MAX || !snapAt(ti).valid || !distant_.At(ti).hasKit) {
        distant_.Note(&DistantMobs::Stats::edgeArrowsOutLost);
        continue;
      }
      const DistantMob& t = distant_.At(ti);
      const DistantSnap& ts = snapAt(ti);
      distant::DistantKit sk;   // the near shooter, as the ranged roll reads it
      sk.id = o.shooter;
      sk.ranged = true;
      sk.arrowSpeed = o.speed;
      sk.arrowDamage = o.damage;
      sk.ammo = o.ammo;
      LoadVitals(t, vit);
      DistantArrowFlight f;
      f.out = distant::ResolveArrow(sk, t.kit, vit.limbHp, (ts.centre - o.from).len(),
                                    std::sqrt(ts.vel.x * ts.vel.x + ts.vel.z * ts.vel.z),
                                    {o.shooter, o.tick}, cal, gear);
      const float perTick = std::max(1e-3f, o.speed / 30.0f);
      f.out.flightTicks = std::max(1, (int)std::ceil((ts.centre - o.exit).len() / perTick));
      f.shooter = o.shooter;
      f.target = o.target;
      f.looseTick = tick;
      f.impactTick = tick + (uint32_t)f.out.flightTicks;
      f.from = o.exit;
      f.to = ts.centre;
      f.ammo = o.ammo;
      distant_.ArrowsMut().push_back(std::move(f));
      distant_.Note(&DistantMobs::Stats::edgeArrowsOutResolved);
    }
    outs.clear();
  }

  // THE LAST ARROW (D2b): the kit and the count go down by one, and an archer
  // out of arrows draws its sidearm into the bow's fist (DrawSidearm).
  auto spendArrow = [&](DistantMob& a) {
    a.ammo = std::max(0, a.ammo - 1);
    a.kit.shots = std::max(0, a.kit.shots - 1);
    if (a.ammo <= 0 || a.kit.shots <= 0) {
      a.ammo = 0;
      a.kit.ranged = false;
      if (a.haveSidearm && a.bowHand >= 0) {
        DistantMob::DistantHand& H = a.hands[a.bowHand];
        H.weapon = true;
        H.weaponStrike = a.sidearmStrike;
        H.weaponName = a.sidearm;
        a.bowHand = -1;
        RearmHeld(a, a.sidearmReach, tick);
        distant_.Note(&DistantMobs::Stats::sidearms);
      }
    }
  };
  // AN INBOUND FLIGHT's state, in ArrowFlights' own units.
  auto setFx = [](DistantArrowFlight& f, Vec3 p, Vec3 vPerTick) {
    f.pos[0] = SpellFxFromFloat(p.x);
    f.pos[1] = SpellFxFromFloat(p.y);
    f.pos[2] = SpellFxFromFloat(p.z);
    f.vel[0] = SpellFxFromFloat(vPerTick.x);
    f.vel[1] = SpellFxFromFloat(vPerTick.y);
    f.vel[2] = SpellFxFromFloat(vPerTick.z);
  };

  // ---- (a) the swings and the looses due, attacker by attacker -------------
  for (const size_t i : due) {
    DistantMob& a = distant_.At(i);
    if (a.dead) continue;   // a landing killed it this tick
    const DistantSnap& as = snapAt(i);
    if (a.strokeImpactTick != 0 && a.strokeImpactTick <= tick) {
      a.strokeImpactTick = 0;   // resolved, once: the arm stays busy to strokeEndTick
      const size_t ti = indexOf(a.strokeTarget);
      const ai::Actor* na = ti == SIZE_MAX ? bandFind(a.strokeTarget) : nullptr;
      if (na != nullptr && a.strokeStyle < a.kit.styles.size()) {
        // ---- DISTANT HITS NEAR (D3): the same roll, the near body's numbers
        const int nd = nearDef(na->id);
        if (nd < 0) {
          distant_.Note(&DistantMobs::Stats::strokeNoTarget);
        } else {
          const distant::StrikeOutcome o = distant::ResolveStrike(
              a.kit, nearDefs[(size_t)nd].kit, nearDefs[(size_t)nd].hp, (int)a.strokeStyle,
              planarDist(as.centre, na->centre), {a.id, tick}, cal, gear);
          distant_.Note(&DistantMobs::Stats::edgeFarSwings);
          switch (o.result) {
            case distant::StrikeResult::Hit: distant_.Note(&DistantMobs::Stats::edgeFarHits); break;
            case distant::StrikeResult::Miss:
              distant_.Note(&DistantMobs::Stats::edgeFarMisses);
              break;
            case distant::StrikeResult::Blocked:
              distant_.Note(&DistantMobs::Stats::edgeFarBlocked);
              break;
            case distant::StrikeResult::OutOfReach:
              distant_.Note(&DistantMobs::Stats::edgeFarOutOfReach);
              break;
            case distant::StrikeResult::NoTarget:
              distant_.Note(&DistantMobs::Stats::strokeNoTarget);
              break;
          }
          if (o.hit) nearHits.push_back({i, na->id, o, a.kit.styles[a.strokeStyle]});
        }
      } else if (ti == SIZE_MAX || ti == i || !snapAt(ti).valid || !distant_.At(ti).hasKit ||
                 a.strokeStyle >= a.kit.styles.size()) {
        distant_.Note(&DistantMobs::Stats::strokeNoTarget);
      } else {
        const DistantMob& t = distant_.At(ti);
        LoadVitals(t, vit);
        // Planar, as the AI's request distance and the reach it was drawn
        // against are measured for walkers.
        const distant::StrikeOutcome o =
            distant::ResolveStrike(a.kit, t.kit, vit.limbHp, (int)a.strokeStyle,
                                   planarDist(as.centre, snapAt(ti).centre), {a.id, tick}, cal,
                                   gear);
        switch (o.result) {
          case distant::StrikeResult::Hit: distant_.Note(&DistantMobs::Stats::strokeHits); break;
          case distant::StrikeResult::Miss: distant_.Note(&DistantMobs::Stats::strokeMisses); break;
          case distant::StrikeResult::Blocked:
            distant_.Note(&DistantMobs::Stats::strokeBlocked);
            break;
          case distant::StrikeResult::OutOfReach:
            distant_.Note(&DistantMobs::Stats::strokeOutOfReach);
            break;
          case distant::StrikeResult::NoTarget:
            distant_.Note(&DistantMobs::Stats::strokeNoTarget);
            break;
        }
        if (o.hit) blows.push_back({ti, o});
      }
    }
    if (a.drawLooseTick != 0 && a.drawLooseTick <= tick) {
      a.drawLooseTick = 0;
      const size_t ti = indexOf(a.drawTarget);
      const ai::Actor* na = ti == SIZE_MAX ? bandFind(a.drawTarget) : nullptr;
      if (na != nullptr && a.kit.ranged && a.ammo > 0) {
        // ---- DISTANT SHOOTS NEAR (D3): a REAL flight, aimed as a near
        // archer aims (game/mob_archery.cpp AimShot + Spread: from the
        // shoulder at the target's centre raised by aimRaise of its height,
        // led where it will be, held high by the drop, the loose's error),
        // flown from here by ArrowFlights' own integrator over DistantGround
        // until it crosses into the window -- where it hits or misses
        // PHYSICALLY. No roll: the near tier's physics is the higher fidelity.
        const ai::Profile* pr = behaviors_.At(a.brain.profile);
        const ai::RangedTuning rt = pr != nullptr ? pr->ranged : ai::RangedTuning{};
        const float speed = a.kit.arrowSpeed > 0.0f ? a.kit.arrowSpeed : cal.ranged.arrowSpeed;
        const Vec3 from{as.centre.x, as.centre.y + a.worldSize.y * 0.25f, as.centre.z};
        Vec3 to = na->centre + Vec3{0.0f, na->height * rt.aimRaise, 0.0f};
        to = ai::LeadPoint(from, to, na->velocity, speed / 30.0f, rt.lead, rt.leadCap);
        to = ArrowHoldover(from, to, speed);
        Vec3 dir = to - from;
        dir = dir.len() > 1e-3f ? dir.normalized()
                                : Vec3{std::sin(a.steer.heading), 0.0f, std::cos(a.steer.heading)};
        const float aimTune =
            a.brain.tune[(int)ai::Tune::Aim] > 0.0f ? a.brain.tune[(int)ai::Tune::Aim] : 1.0f;
        dir = DistantSpread(dir, rt.spread * aimTune, a.id, tick);
        DistantArrowFlight f;
        f.inbound = true;
        f.shooter = a.id;
        f.target = na->id;
        f.looseTick = tick;
        f.impactTick = tick + (uint32_t)ArrowFlights::kLifeTicks;
        f.from = from;
        f.to = to;
        f.ammo = a.kit.ammo;
        f.seed = rng::Hash3(ai::kSaltDistantArrow ^ 0xA770B2u, tick, (uint32_t)a.id);
        setFx(f, from, dir * (speed / 30.0f));
        distant_.ArrowsMut().push_back(std::move(f));
        distant_.Note(&DistantMobs::Stats::looses);
        distant_.Note(&DistantMobs::Stats::edgeArrowsIn);
        spendArrow(a);
      } else if (ti == SIZE_MAX || ti == i || !snapAt(ti).valid || !distant_.At(ti).hasKit ||
                 !a.kit.ranged || a.ammo <= 0) {
        // THE STRING IS LET DOWN (StepDraw's "the target is gone"): the
        // arrow stays in the kit.
        distant_.Note(&DistantMobs::Stats::drawsAborted);
      } else {
        const DistantMob& t = distant_.At(ti);
        const DistantSnap& ts = snapAt(ti);
        LoadVitals(t, vit);
        const Vec3 dv = ts.centre - as.centre;
        DistantArrowFlight f;
        f.out = distant::ResolveArrow(a.kit, t.kit, vit.limbHp, dv.len(),
                                      std::sqrt(ts.vel.x * ts.vel.x + ts.vel.z * ts.vel.z),
                                      {a.id, tick}, cal, gear);
        f.shooter = a.id;
        f.target = t.id;
        f.looseTick = tick;
        f.impactTick = tick + (uint32_t)std::max(1, f.out.flightTicks);
        f.from = as.centre;
        f.to = ts.centre;
        f.ammo = a.kit.ammo;
        distant_.ArrowsMut().push_back(std::move(f));
        distant_.Note(&DistantMobs::Stats::looses);
        spendArrow(a);
      }
    }
  }
  // ---- (b) the arrows in the air, in the order they were loosed --------------
  {
    std::vector<DistantArrowFlight>& arrows = distant_.ArrowsMut();
    const float gTick = ArrowGravityPerTick2();
    auto solid = [&](int x, int y, int z) { return distantGround_.Solid(x, y, z); };
    size_t keep = 0;
    for (size_t k = 0; k < arrows.size(); k++) {
      DistantArrowFlight& f = arrows[k];
      // ---- AN ABSTRACT ARROW WHOSE TARGET WAS PROMOTED (D3) ----------------
      // It is a near creature now: the arrow becomes a real flight from the
      // state its arc has at this tick (the parabola it is drawn on, under the
      // arrow integrator's own gravity), flown on below like any inbound one.
      // Physics decides what it hits; the roll it carried is dropped.
      if (!f.inbound) {
        const size_t ti = indexOf(f.target);
        if (ti == SIZE_MAX || !snapAt(ti).valid) {
          const Mob* nm = FindMobById(f.target);
          if (nm != nullptr && nm->alive_ && !nm->PlayerControlled()) {
            const float T = (float)std::max<uint32_t>(1u, f.impactTick - f.looseTick);
            const float tt = std::clamp((float)tick - (float)f.looseTick, 0.0f, T);
            const Vec3 span = f.to - f.from;
            const Vec3 p{f.from.x + span.x * (tt / T),
                         f.from.y + span.y * (tt / T) + 0.5f * gTick * tt * (T - tt),
                         f.from.z + span.z * (tt / T)};
            const Vec3 v{span.x / T, span.y / T + 0.5f * gTick * (T - 2.0f * tt), span.z / T};
            f.inbound = true;
            f.life = (int)tt;
            f.impactTick = f.looseTick + (uint32_t)ArrowFlights::kLifeTicks;
            f.seed = rng::Hash3(ai::kSaltDistantArrow ^ 0xA770B3u, f.looseTick,
                                (uint32_t)f.shooter);
            setFx(f, p, v);
            distant_.Note(&DistantMobs::Stats::edgeArrowsConverted);
          } else {
            if (f.impactTick > tick) {
              if (keep != k) arrows[keep] = std::move(f);
              keep++;
              continue;
            }
            // Not a living creature in either tier any more: dead, retired.
            distant_.Note(&DistantMobs::Stats::arrowsLost);
            continue;
          }
        }
      }
      // ---- AN INBOUND ARROW: one tick of real flight outside the window ----
      if (f.inbound) {
        SpellFxVec p{f.pos[0], f.pos[1], f.pos[2]}, v{f.vel[0], f.vel[1], f.vel[2]};
        const ArrowOutside r = ArrowStepOutside(p, v, world, solid);
        f.pos[0] = p.x;
        f.pos[1] = p.y;
        f.pos[2] = p.z;
        f.vel[0] = v.x;
        f.vel[1] = v.y;
        f.vel[2] = v.z;
        f.life++;
        if (r == ArrowOutside::Entered) {
          // INTO THE WINDOW: a real ArrowFlight from exactly this state
          // (MobArcheryTick injects it into this tick's ArrowFlights).
          DistantArrowIn in;
          in.shooter = f.shooter;
          in.target = f.target;
          in.tick = tick;
          for (int c = 0; c < 3; c++) {
            in.pos[c] = f.pos[c];
            in.vel[c] = f.vel[c];
          }
          in.seed = f.seed;
          in.ammo = f.ammo;
          edgeArrowsIn_.push_back(std::move(in));
          continue;
        }
        if (r == ArrowOutside::Grounded) {
          distant_.Note(&DistantMobs::Stats::edgeArrowsGrounded);
          continue;
        }
        if (f.life >= ArrowFlights::kLifeTicks) {
          distant_.Note(&DistantMobs::Stats::edgeArrowsExpired);
          continue;
        }
        if (keep != k) arrows[keep] = std::move(f);
        keep++;
        continue;
      }
      if (f.impactTick > tick) {
        if (keep != k) arrows[keep] = std::move(f);
        keep++;
        continue;
      }
      const size_t ti = indexOf(f.target);
      if (ti == SIZE_MAX || !snapAt(ti).valid) {
        distant_.Note(&DistantMobs::Stats::arrowsLost);
        continue;
      }
      if (!f.out.hit) {
        distant_.Note(&DistantMobs::Stats::arrowMisses);
        continue;
      }
      distant_.Note(&DistantMobs::Stats::arrowHits);
      distant::StrikeOutcome o;
      o.result = distant::StrikeResult::Hit;
      o.hit = true;
      o.limb = f.out.limb;
      o.role = f.out.role;
      o.hp = f.out.hp;
      o.kind = distant::StrikeKind::Pierce;
      o.cause = DamageCause::Pierce;
      o.bleedAdded = f.out.bleedAdded;
      blows.push_back({ti, o});
    }
    arrows.resize(keep);
  }
  // ---- (d) APPLY, in the order above -----------------------------------------
  for (const Blow& b : blows) {
    DistantMob& t = distant_.At(b.def);
    catchUpStumps(t);   // a sleeper struck: what it bled asleep, first
    if (t.dead || !t.hasKit) continue;
    LoadVitals(t, vit);
    distant::DistantDeath died = distant::ApplyStrike(vit, t.kit, b.o, cal);
    // THE LIMB COMES OFF (D2b): the struck limb and all under it leave the
    // body -- the kit stops striking and counting them, the record's limb is
    // marked off for the promotion (Mob::ApplyDistantOverlay severs it for
    // real), and the stump bleeds from now until death.
    if (b.o.sever && died == distant::DistantDeath::None) {
      thread_local std::vector<int> off;
      died = distant::SeverLimb(vit, t.kit, b.o.limb, cal, &off);
      for (const int li : off)
        if ((size_t)li < t.limbRole.size()) t.limbRole[(size_t)li].flags &= ~kDistantLimbAttached;
      distant_.Note(&DistantMobs::Stats::severs);
    }
    StoreVitals(vit, t);
    hpDealt += b.o.hp;
    if (b.o.limb >= 0 && b.o.bleedAdded > 0.0f) t.lastWoundLimb = (int16_t)b.o.limb;
    // A BLOW WAKES (it could only land on a sleeper from an arrow out of the
    // dark, or a near creature's swing across the edge); the hurt is read by
    // its own Think next step (Brain::prevHp).
    if (t.asleep) {
      t.asleep = false;
      t.wakeTick = 0;
      t.wakes++;
      distant_.MarkAwakeHint();
    }
    touched.push_back(b.def);
    if (died != distant::DistantDeath::None) kill(t, died);
  }
  // ...and DISTANT HITS NEAR (D3), in attacker-id order: through the near
  // creature's ORDINARY door (game/melee.h ApplyEdgeBlow), at the limb the
  // roll drew, as hard as it drew -- the roll's hp before the shell, over the
  // style's strike total, is the sweep's own 0..1 power; the near model then
  // prices it with its own carve, organs and shell (the struck limb's real
  // armour is retargeted onto there). Whether a limb comes off is the near
  // lattice's to say (the roll's `sever` is a distant body's). The wound, the
  // blood and their ops are the near creature's own; THIS TIER STILL EMITS
  // NOTHING. With no door to enter (a CPU-only arm: no physics, no spawns),
  // it is counted, not applied.
  for (const NearHit& h : nearHits) {
    const DistantSnap& as = snapAt(h.att);
    const float total = std::max(1e-3f, h.style.strike.Total());
    const float preShell = h.o.hp / std::max(1e-3f, h.o.shellFactor);
    EdgeBlow eb;
    eb.attackerId = distant_.At(h.att).id;
    eb.targetId = h.target;
    eb.limb = h.o.limb;
    eb.from = as.centre;
    eb.strike = h.style.strike;
    eb.weapon = h.style.natural ? std::string() : h.style.weapon;
    eb.natural = h.style.natural;
    eb.stab = !h.style.natural && h.style.name.find("thrust") != std::string::npos;
    eb.power = std::clamp(preShell / total, 0.15f, 1.0f);
    eb.seed = rng::Hash3((uint32_t)eb.attackerId ^ ai::kSaltDistantStrike, tick, 7u);
    distant_.Note(&DistantMobs::Stats::edgeFarHpRollMilli,
                  (uint64_t)std::llround((double)h.o.hp * 1000.0));
    if (spawns == nullptr || phys_ == nullptr || debris_ == nullptr) {
      distant_.Note(&DistantMobs::Stats::edgeFarUnapplied);
      continue;
    }
    const EdgeBlowResult r = ApplyEdgeBlow(eb, *phys_, *this, *debris_, world, *spawns);
    if (!r.applied) {
      distant_.Note(&DistantMobs::Stats::edgeFarUnapplied);
      continue;
    }
    distant_.Note(&DistantMobs::Stats::edgeFarApplied);
    if (!r.rayHit) distant_.Note(&DistantMobs::Stats::edgeFarRayMisses);
    if (r.covered) distant_.Note(&DistantMobs::Stats::edgeFarCovered);
    distant_.Note(&DistantMobs::Stats::edgeFarHpDoorMilli,
                  (uint64_t)std::llround((double)r.hpCharged * 1000.0));
  }
  // ---- (e) THE STATUSES: a tick of bleeding, a tick of burning ---------------
  for (const size_t i : status) {
    DistantMob& d = distant_.At(i);
    if (d.dead) continue;
    if (!d.hasKit) {
      d.bloodLossPerTick = 0.0f;
      d.burnTicksLeft = 0;
      continue;
    }
    LoadVitals(d, vit);
    if (vit.bleedRate > 0.0f || vit.stumps > 0) {
      distant::StepBleed(vit, d.kit, cal);
      distant_.Note(&DistantMobs::Stats::bleedTicks);
    }
    if (vit.dead == distant::DistantDeath::None && vit.burnTicksLeft > 0) {
      distant::StepBurn(vit, d.kit, cal);
      distant_.Note(&DistantMobs::Stats::burnTicks);
    }
    StoreVitals(vit, d);
    d.statusTick = tick;
    touched.push_back(i);
    if (vit.dead != distant::DistantDeath::None) kill(d, vit.dead);
  }
  // ---- (f) A HAND TOO HURT TO HOLD ON lets go (Mob::GripFails) ---------------
  // Below melee.injuredArmDrop of its arm chain's worst limb -- and above
  // zero: GripFails' own window -- the weapon falls, and the held styles are
  // thrown with the fist from then on (RearmHeld). The record still has the
  // item in the hand; a promotion's first GripFails drops it again.
  const float dropBelow = CurrentTuning().melee.injuredArmDrop;
  if (dropBelow > 0.0f && !touched.empty()) {
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (const size_t i : touched) {
      DistantMob& d = distant_.At(i);
      if (d.dead || !d.hasKit) continue;
      bool fell = false;
      for (int hh = 0; hh < kHands; hh++) {
        DistantMob::DistantHand& H = d.hands[hh];
        // A PART OF THE ARM OFF (D2b): what the hand held left with it, and
        // there is no fist on the end of it any more (NaturalWeaponUsable:
        // a chain effector needs its whole chain).
        bool chainOff = false;
        for (const int16_t pp : H.parts)
          chainOff = chainOff || (pp >= 0 && (size_t)pp < d.kit.limbs.size() &&
                                  !d.kit.limbs[(size_t)pp].alive);
        if (chainOff) {
          if (H.weapon || H.fist || H.shield) {
            if (d.bowHand == hh) {
              d.bowHand = -1;
              d.kit.ranged = false;
              d.ammo = 0;
              d.drawLooseTick = 0;
            }
            H.weapon = H.fist = H.shield = false;
            fell = true;
          }
          continue;
        }
        if (!H.weapon) continue;
        const float cnd = DistantHandCondition(d, hh);
        if (cnd <= 0.0f || cnd >= dropBelow) continue;
        H.weapon = false;
        fell = true;
        if (d.bowHand == hh) {
          // The bow went with it: no more shots.
          d.bowHand = -1;
          d.kit.ranged = false;
          d.ammo = 0;
          d.drawLooseTick = 0;
        }
        distant_.Note(&DistantMobs::Stats::gripFails);
      }
      if (fell) RearmHeld(d, -1.0f, tick);
    }
  }
  if (hpDealt > 0.0) distant_.Note(&DistantMobs::Stats::hpDealtMilli, (uint64_t)std::llround(hpDealt * 1000.0));
  const auto tSleep = Clock::now();
  const uint64_t nsCombat = (uint64_t)std::max<int64_t>(
      0, std::chrono::duration_cast<std::chrono::nanoseconds>(tSleep - tCombat).count());

  // ---- PHASE 2: who falls asleep (tick-start state of everyone else) ----------
  // Before the moves are filed: a sleeper a mover then reaches is woken by
  // that move, never slept through it.
  for (const Next& nx : nexts) {
    DistantMob& d = distant_.At(nx.i);
    if (d.dead) {
      d.asleep = true;
      continue;
    }
    const bool movedEnough = nx.moved && (nx.movedLen > kDistantSleepJitterVox ||
                                          std::fabs(nx.origin.y - d.origin.y) > kDistantSleepJitterVox);
    if (movedEnough || d.landPending || d.brain.hasTarget || d.brain.hasThreat) continue;
    // D2b: never with a swing or a draw in flight, nor while a wound bleeds
    // or a fire burns (both run out: a bleed decays, a fire has its ticks).
    // AN OPEN STUMP bleeds until the creature is dead (near's rule, gore.
    // stumpBleedsOpen) -- thousands of ticks for a survivor -- so it does NOT
    // keep a body awake: it sleeps with a clock at the tick it bleeds out
    // (stumpClock), and whatever wakes it first pays the ticks it slept
    // through in one sum (catchUpStumps). Rule 2 for an open-ended status.
    if (d.strokeEndTick != 0 || d.drawLooseTick != 0) continue;
    if (d.bloodLossPerTick > 0.0f || d.burnTicksLeft > 0) continue;
    const ai::Profile* prof = behaviors_.At(d.brain.profile);
    // No mind: nothing near it can change what it does, so it sleeps now
    // (a stranger that wants it comes to it, and its arrival wakes it).
    if (prof == nullptr) {
      d.asleep = true;
      d.wakeTick = 0;
      stumpClock(d);
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
      auto hostileAt = [&](Vec3 oc, uint32_t of, bool oh, float okeep) {
        if (of == selfFaction || !(oh || selfHostile)) return false;
        const float r = std::max(selfKeep, okeep) + kDistantSleepMarginVox;
        const float dx = oc.x - c.x, dz = oc.z - c.z;
        return dx * dx + dz * dz <= r * r;
      };
      for (const uint32_t oi : nbPool[neighbours(d.cellX, d.cellZ, rc)]) {
        if (oi == nx.i) continue;
        const DistantSnap& o = snapAt(oi);
        if (!o.valid) continue;
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
    stumpClock(d);
    distant_.Note(&DistantMobs::Stats::sleeps);
  }
  const auto tFile = Clock::now();
  const uint64_t nsSleep = (uint64_t)std::max<int64_t>(
      0, std::chrono::duration_cast<std::chrono::nanoseconds>(tFile - tSleep).count());

  // ---- PHASE 3: the moves, filed in id order (waking whom they reach) ---------
  for (const Next& nx : nexts) {
    DistantMob& d = distant_.At(nx.i);
    // One that died this tick falls where it stood at the start of it (its
    // statue already rests there): the step it had decided is not taken.
    if (!nx.moved || d.dead) {
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
  // Walked whole (a blow can wake a creature the stepping loop never saw).
  {
    bool awake = false;
    uint32_t nw = UINT32_MAX;
    for (const DistantMob& d : distant_.All()) {
      if (d.dead) continue;
      if (!d.asleep) awake = true;
      else if (d.wakeTick != 0) nw = std::min(nw, d.wakeTick);
    }
    distant_.SetSleepSummary(awake, nw);
  }
  const uint64_t nsFile = nsSince(tFile);
  distant_.Note(&DistantMobs::Stats::nsSetup, nsSetup);
  distant_.Note(&DistantMobs::Stats::nsSelf, nsSelf);
  distant_.Note(&DistantMobs::Stats::nsWorld, nsWorld);
  distant_.Note(&DistantMobs::Stats::nsThink, nsThink);
  distant_.Note(&DistantMobs::Stats::nsMove, nsMove);
  distant_.Note(&DistantMobs::Stats::nsCombat, nsCombat);
  distant_.Note(&DistantMobs::Stats::nsSleep, nsSleep);
  distant_.Note(&DistantMobs::Stats::nsFile, nsFile);
  timeOut();
}
