// mob_shock.cpp — SHOCKS REACH BODIES, AND BODIES CONDUCT BY THEIR MATERIAL
// (docs/PLAN_electricity.md section 4, package E4; docs/PLAN_electricity_wave2.md
// package B; DESIGN.md "Electricity -- shocks reach bodies").
//
// The charge field (src/sim/elec.h, sim_elec.wgsl) lives on the GPU and the
// bodies live on the CPU, so a body learns what it stands in by ASKING:
//
//   QueueShockQueries   end of MobSystem::PreTick, tick T: ONE world box per
//                       living body -- the union of its limbs, worn shells and
//                       held items under the current pose, dilated a cell (a
//                       foot ON a charged plate or IN charged water overlaps the
//                       charged cells) -- with its GRID, queued on
//                       World::QueueElecQuery. Only while
//                       World::ElecMayBeLive(T): a world with no charge asks
//                       nothing (rule 2). The players' bodies queue first, so a
//                       crowd is refused before a player is (counted).
//   SubmitTick(T)       uploads them; sim_elec.wgsl elecQuery answers each with
//                       the settled field's max P, charged cells, P sum, cells
//                       scanned and the grid (P | air for every cell of the
//                       box); the answers ride tick T's readback slot.
//   ApplyShocks         top of PreTick, tick T + K + 1 (K =
//                       World::kSnapshotLatency): World::TakeElecHits hands
//                       over the answers of every snapshot published since the
//                       last call -- exactly tick T's at the steady state -- in
//                       query order. FIXED LATENCY, the same contract every
//                       gameplay readback keeps: the answer a body acts on is a
//                       pure function of the tick, never of when a fence
//                       retired, so two runs (and the op record's replay, whose
//                       world is reproduced from the ops this pass emits)
//                       agree.
//
// ---- THE BODY IS MATTER, AND THE MATTER CONDUCTS (wave 2) -------------------
// A body whose box held charge is solved as the world would solve the same
// voxels. Each rig slot's lattice is binned into WORLD-pitch cells
// (ElecSlotCache: lattice coordinate / scale, floored), and each cell takes
// the resist of its matter -- materials.json electric.resist, combined over
// the cell's voxels as conductances in parallel (an insulating voxel carries
// nothing, so a cell half bone and half flesh conducts at half flesh's rate)
// -- under the world's WET RULE (a voxel under a conducting coat takes
// min(resist, the wet table at its coat amount), elec.h ElecWetResist with
// sim.elecWetResist: the same numbers sim_elec.wgsl elecCellResist uses).
// Two resists a cell: BULK over all its voxels (charge crossing it inside the
// body) and ENTRY over its EXPOSED voxels only (charge arriving from outside
// the slot -- the world, a shell, a held item, another body -- crosses the
// skin, the shell, the grip). Then the field's own rule, max-plus:
//
//   P(cell) = max( world P at the cell and its six faces - entry,
//                  P(neighbour cell of the same slot)     - bulk,
//                  P(touching cell of a jointed limb)     - bulk,
//                  P(touching cell of a shell / item / other body) - entry ),
//
// solved to its unique least fixpoint (a widest-path search, integer, so it
// does not depend on visit order). Nothing is stored between ticks: a body's
// charge is a pure function of this tick's answer and its pose, and fades
// with the world's.
//
// What follows is the materials', never the creature's:
//   felt P      per limb, the max over its cells of P x electric.shock / 1000
//               (what the matter FEELS: tissue and an android's circuitry
//               1000; bone, blood, wood, metal 0 -- a wooden body is not
//               stunned, it chars)
//   effective P felt P x (1 + shockWetGain x the limb's CONDUCTING coat
//               fraction) x shockArmourGain when a worn conductor CARRIES charge
//   hp          DamageCause::Electric on each touching limb and, by
//               shockTorsoShare, on the vital core (the current crosses the
//               body); no wound, no bleed (severpolicy.h's Electric row)
//   stun        Mob::stunUntil_ pushed later (never earlier); honoured by
//               DecideIntent and, for a player, TickAuthority
//   knock-down  at shockRagdollP: StartRagdoll(shockRagdollSeconds, "shock")
//   ohmic       per body cell, E2's elecReact exactly: E = P x the cell's
//               resist, chance = min(electric.ignite.chance, E x
//               sim.elecIgniteGain) x (air faces + 2) / 8, and the cell's
//               voxels become ignite.into (an air face) or char (none). Wood
//               becomes ember and burns; flesh sears; alloy, with no ignite
//               data, never does.
//   crackle     per body cell, E2's crackle exactly: P over sim.elecCrackle*P
//               throws a spark / arc into an AIR face, as a fill-air cell op
//               (recorded: this is how a charged body passes charge on into
//               the world -- the discharge it throws is a source)
//   fire        at shockIgniteMinP, a hashed roll: Mob::Ignite on the hair, the
//               clothes and the touching limbs -- the burn pass's own door, so
//               a wet voxel refuses the flame exactly as it refuses a torch
//   twitch      TickStuns: Mob::HitReact in a hashed direction every
//               shockTwitchTicks while stunned (presentation only)
//
// Every roll is an integer hash of (creature id, tick, cell); nothing reads a
// clock or a pointer. Corpses and ghosts never ask: a corpse has no hp to
// lose, and a ghost's body is its owner's to shock.

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <queue>

#include "game/anim.h"
#include "game/mob.h"
#include "game/workpool.h"
#include "sim/elec.h"
#include "sim/materials.h"   // kReactChanceDen
#include "sim/rng.h"
#include "sim/tuning.h"
#include "sim/voxload.h"   // kBodyStainAmtMax, BodyStain*
#include "sim/world.h"

// THE QUERY IS SIZED FROM THE CREATURE CAP: every living creature plus the
// players' bodies get a box, or the build fails here (elec.h kElecQueryMax).
static_assert(MobSystem::MaxLiveMobs() + kElecQueryPlayerReserve <= kElecQueryMax,
              "raise elec.h kElecQueryMax with MobSystem::kMaxMobs");

namespace {

constexpr uint16_t kIns = (uint16_t)kElecResistInsulator;   // 4095: no conduction
// sim_step.wgsl ELEC_CRACKLE_CAP (200 per-mille of kReactChanceDen) and its
// salts: a body cell's crackle and ignition roll as a world cell's do.
constexpr uint32_t kElecCrackleCap = 200000u;
constexpr uint32_t kOhmSalt = 0x0B0D1E5u, kCrackleSalt = 0x0C2AC1Eu;
// The poses a body keeps for its answers in flight: K + 1 ticks and slack.
constexpr uint32_t kElecPoseRing = World::kSnapshotLatency + 3;
// Shell-lattice steps the cover ray may take: two world cells at the finest
// art scale (8 a cell) and then some.
constexpr int kCoverMarchMax = 40;

// The world box of one rig slot's collider under its current pose. The stain
// pass walks the same box (MobSystem::StainOneLimb's "IS ANYTHING THERE?").
bool ViewWorldBox(const BurnLimbView& v, Vec3& mn, Vec3& mx) {
  if (v.xf == nullptr) return false;
  const Quat q{v.xf->quat[0], v.xf->quat[1], v.xf->quat[2], v.xf->quat[3]};
  const float sinv = 1.0f / (float)std::max(1u, v.physScale);
  mn = {1e30f, 1e30f, 1e30f};
  mx = {-1e30f, -1e30f, -1e30f};
  for (int k = 0; k < 8; k++) {
    const Vec3 c{(float)((k & 1) ? v.size.x : v.sizeMin.x) * sinv,
                 (float)((k & 2) ? v.size.y : v.sizeMin.y) * sinv,
                 (float)((k & 4) ? v.size.z : v.sizeMin.z) * sinv};
    const Vec3 w = v.xf->pos + QuatRotate(q, c);
    mn.x = std::min(mn.x, w.x); mn.y = std::min(mn.y, w.y); mn.z = std::min(mn.z, w.z);
    mx.x = std::max(mx.x, w.x); mx.y = std::max(mx.y, w.y); mx.z = std::max(mx.z, w.z);
  }
  return mn.x <= mx.x && mn.y <= mx.y && mn.z <= mx.z;
}

// The body's box, a cell of slack round it, at most kElecQueryAxisMax cells an
// axis (a pathological pose or a long spear cannot make the GPU scan a
// building): the LOW end kept on the vertical axis (the feet are what stand
// in the water), the middle on the others.
void BodyBox(const Vec3& mn, const Vec3& mx, int32_t lo[3], int32_t hi[3]) {
  const float m[3] = {mn.x, mn.y, mn.z}, M[3] = {mx.x, mx.y, mx.z};
  for (int a = 0; a < 3; a++) {
    lo[a] = ifloor(m[a]) - 1;
    hi[a] = ifloor(M[a]) + 1;
    if (hi[a] - lo[a] + 1 > (int32_t)kElecQueryAxisMax) {
      const int32_t over = hi[a] - lo[a] + 1 - (int32_t)kElecQueryAxisMax;
      if (a == 1) {
        hi[a] -= over;
      } else {
        lo[a] += over / 2;
        hi[a] = lo[a] + (int32_t)kElecQueryAxisMax - 1;
      }
    }
  }
}

int FloorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Conductance of one voxel of resist r, 16.16-ish fixed point (0 = insulator),
// and the resist of n voxels whose conductances sum to g: the voxels of a cell
// in PARALLEL, n / sum(1 / r), rounded up, an insulator past 254.
constexpr uint64_t kConductOne = 1ull << 24;
uint32_t Conductance(uint32_t r) { return (r == 0 || r >= kIns) ? 0u : (uint32_t)(kConductOne / r); }
uint16_t ParallelResist(uint64_t n, uint64_t g) {
  if (n == 0 || g == 0) return kIns;
  const uint64_t r = (kConductOne * n + g - 1) / g;
  return r >= kIns ? kIns : (uint16_t)std::max<uint64_t>(r, 1);
}

uint64_t CellKey(IVec3 c) {
  return ((uint64_t)((uint32_t)c.x & 0x1FFFFFu)) | ((uint64_t)((uint32_t)c.y & 0x1FFFFFu) << 21) |
         ((uint64_t)((uint32_t)c.z & 0x1FFFFFu) << 42);
}

const IVec3 kFace6[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

// Grid of one answer: P and air at world cell c (0 / false outside it).
uint32_t GridWord(const ElecHit* h, IVec3 c) {
  if (h == nullptr || h->grid.empty()) return 0u;
  const int x = c.x - h->lo[0], y = c.y - h->lo[1], z = c.z - h->lo[2];
  if (x < 0 || y < 0 || z < 0 || x >= h->dims[0] || y >= h->dims[1] || z >= h->dims[2]) return 0u;
  return h->grid[((size_t)z * h->dims[1] + y) * h->dims[0] + x];
}

}  // namespace

void MobSystem::QueueShockQueries(uint32_t tick, World& world) {
  if (!world.ElecMayBeLive(tick)) return;
  auto ask = [&](Mob& m) {
    if (m.def_ == nullptr || !m.alive_ || m.IsGhost() || m.rigReleased_) return;
    bool any = false;
    Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    for (MobLimb& L : m.limbs_) {
      if (!L.body) continue;
      const BurnLimbView v = m.ViewOf(L);
      Vec3 a, b;
      if (!ViewWorldBox(v, a, b)) continue;
      any = true;
      mn.x = std::min(mn.x, a.x); mn.y = std::min(mn.y, a.y); mn.z = std::min(mn.z, a.z);
      mx.x = std::max(mx.x, b.x); mx.y = std::max(mx.y, b.y); mx.z = std::max(mx.z, b.z);
    }
    if (!any) return;
    ElecQuery q;
    BodyBox(mn, mx, q.lo, q.hi);
    q.mobId = m.id_;
    q.limb = -1;
    if (!world.QueueElecQuery(q, /*wantGrid=*/true)) {
      shockCounters_.refused++;
      return;
    }
    shockCounters_.queued++;
    // The pose it asked with, for the answer K + 1 ticks from now.
    if (m.elecPoses_.size() != kElecPoseRing) m.elecPoses_.resize(kElecPoseRing);
    Mob::ElecPose& e = m.elecPoses_[tick % kElecPoseRing];
    e.tick = tick;
    e.valid = true;
    e.xf.resize(m.limbs_.size());
    for (size_t li = 0; li < m.limbs_.size(); li++) e.xf[li] = m.limbs_[li].xf;
  };
  // The players first: a crowd that fills the boxes or the grid budget
  // refuses NPCs (counted by World::QueueElecQuery), never the player.
  for (Mob* av : avatars_)
    if (av != nullptr) ask(*av);
  for (Mob& m : mobs_) ask(m);
}

void MobSystem::ApplyShocks(uint32_t tick, World& world, std::vector<CellOp>& cellOps) {
  std::vector<ElecHit> hits = world.TakeElecHits();
  if (hits.empty()) return;
  // Wall time, a diagnostic only (the gate's per-tick cost readout); never
  // read by the sim.
  const auto t0 = std::chrono::steady_clock::now();
  shockCounters_.hitsRead += hits.size();
  // One tick's answers at a time (a hiccup can hand over two).
  size_t i0 = 0;
  while (i0 < hits.size()) {
    size_t i1 = i0 + 1;
    while (i1 < hits.size() && hits[i1].tick == hits[i0].tick) i1++;
    ApplyShockTick(tick, world, cellOps, hits.data() + i0, i1 - i0);
    i0 = i1;
  }
  shockCounters_.applyNanos += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count();
  shockCounters_.applyCalls++;
}

void MobSystem::ApplyShockTick(uint32_t tick, World& world, std::vector<CellOp>& cellOps,
                               const ElecHit* hits, size_t nHits) {
  const Tuning& tn = CurrentTuning();
  const Tuning::Gore& g = tn.gore;

  // ---- WHO TAKES PART -------------------------------------------------------
  // Every body whose box held charge, then every UNCHARGED body touching one
  // of them (two creatures holding hands in a pond: the one on the bank is
  // charged through the other). In query order, so the ops and the shared
  // budgets below are spent in a fixed order.
  struct Part {
    Mob* m = nullptr;
    const ElecHit* hit = nullptr;
    bool charged = false;
    Vec3 mn{}, mx{};
    std::vector<int32_t> slotBase;   // first node of each rig slot, -1 = none
  };
  auto usable = [](const Mob* m) {
    return m != nullptr && m->alive_ && m->def_ != nullptr && !m->IsGhost() && !m->rigReleased_;
  };
  auto bodyBox = [&](Mob& m, Vec3& mn, Vec3& mx) {
    mn = {1e30f, 1e30f, 1e30f};
    mx = {-1e30f, -1e30f, -1e30f};
    bool any = false;
    for (MobLimb& L : m.limbs_) {
      if (!L.body) continue;
      Vec3 a, b;
      if (!ViewWorldBox(m.ViewOf(L), a, b)) continue;
      any = true;
      mn.x = std::min(mn.x, a.x); mn.y = std::min(mn.y, a.y); mn.z = std::min(mn.z, a.z);
      mx.x = std::max(mx.x, b.x); mx.y = std::max(mx.y, b.y); mx.z = std::max(mx.z, b.z);
    }
    return any;
  };
  std::vector<Part> parts;
  for (size_t k = 0; k < nHits; k++) {
    const ElecHit& h = hits[k];
    if (h.maxP == 0) continue;
    shockCounters_.hitsCharged++;
    Mob* m = FindCreature(h.mobId);
    if (!usable(m)) {
      shockCounters_.stale++;
      continue;
    }
    Part p;
    p.m = m;
    p.hit = &h;
    p.charged = true;
    // The RAW box P, recorded whatever the body then makes of it (rule 6:
    // "the field never reached it" and "its matter refused it" differ).
    m->shock_.maxP = std::max(m->shock_.maxP, h.maxP);
    if (!bodyBox(*m, p.mn, p.mx)) continue;
    parts.push_back(std::move(p));
  }
  if (parts.empty()) return;
  const size_t nCharged = parts.size();
  for (size_t k = 0; k < nHits; k++) {
    const ElecHit& h = hits[k];
    if (h.maxP != 0) continue;
    Mob* m = FindCreature(h.mobId);
    if (!usable(m)) continue;
    Part p;
    p.m = m;
    p.hit = &h;
    if (!bodyBox(*m, p.mn, p.mx)) continue;
    bool touches = false;
    for (size_t c = 0; c < nCharged && !touches; c++) {
      const Part& q = parts[c];
      touches = p.mn.x <= q.mx.x + 1.5f && q.mn.x <= p.mx.x + 1.5f && p.mn.y <= q.mx.y + 1.5f &&
                q.mn.y <= p.mx.y + 1.5f && p.mn.z <= q.mx.z + 1.5f && q.mn.z <= p.mx.z + 1.5f;
    }
    if (!touches) continue;
    shockCounters_.linked++;
    parts.push_back(std::move(p));
  }

  // ---- THE BODY CELLS -------------------------------------------------------
  const uint32_t wet = (uint32_t)std::clamp(tn.sim.elecWetResist, 1, (int)kElecResistInsulator - 1);
  struct Node {
    uint32_t part = 0;
    int32_t slot = -1;
    int32_t cell = -1;      // index into the slot's ElecSlotCache::cells
    IVec3 w{};              // the world cell it sits in, this pose
    Vec3 at{};              // ...and its centre, in world voxels
    Vec3 atNow{};           // its centre under the CURRENT pose (the cover ray)
    uint16_t bulk = kIns, entry = kIns;
    uint16_t spreadQ = 0;   // the spreading loss of entering it, /4096 of what arrives
  };
  std::vector<Node> nodes;
  // ---- EVERY SLOT'S CACHE, REFRESHED ACROSS THE POOL (PLAN_fight64_perf M) --
  // RefreshElecSlotCache is a pure function of one slot's lattice, the
  // material tables and the tick, and writes only that slot's cache: the
  // per-item contract workpool.h states. So the refreshes run first, in
  // parallel, and the node list below is built from the refreshed caches in
  // the same (part, slot) order as before -- the result is the serial loop's,
  // bit for bit. A slot listed twice (never expected: one answer per creature
  // per tick) would be two writers of one cache, so that case runs serially.
  {
    struct Job {
      BurnLimbView v;
      ElecSlotCache* c = nullptr;
      int how = 0;
      uint64_t nanos = 0;
    };
    std::vector<Job> jobs;
    for (uint32_t pi = 0; pi < parts.size(); pi++) {
      Mob& m = *parts[pi].m;
      if (m.elecCache_.size() != m.limbs_.size()) m.elecCache_.resize(m.limbs_.size());
      for (int li = 0; li < (int)m.limbs_.size(); li++) {
        MobLimb& L = m.limbs_[li];
        if (!L.body) continue;
        Job j;
        j.v = m.ViewOf(L);
        if (j.v.xf == nullptr || j.v.Size() == 0) continue;
        j.c = &m.elecCache_[li];
        jobs.push_back(j);
      }
    }
    std::vector<const ElecSlotCache*> seen;
    seen.reserve(jobs.size());
    for (const Job& j : jobs) seen.push_back(j.c);
    std::sort(seen.begin(), seen.end());
    const bool unique = std::adjacent_find(seen.begin(), seen.end()) == seen.end();
    auto run = [&](size_t k) {
      Job& j = jobs[k];
      const auto r0 = std::chrono::steady_clock::now();
      j.how = RefreshElecSlotCache(j.v, wet, tick, *j.c);
      j.nanos = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - r0)
                    .count();
    };
    if (unique) workpool::ParallelFor(jobs.size(), 1, run);
    else for (size_t k = 0; k < jobs.size(); k++) run(k);
    for (const Job& j : jobs) {
      if (j.how == 2) shockCounters_.cacheBuilds++;
      else if (j.how == 1) shockCounters_.cacheAccums++;
      else shockCounters_.cacheHits++;
      shockCounters_.refreshNanos += j.nanos;
    }
  }
  for (uint32_t pi = 0; pi < parts.size(); pi++) {
    Part& p = parts[pi];
    Mob& m = *p.m;
    p.slotBase.assign(m.limbs_.size(), -1);
    // The pose of the tick the box was asked on (Mob::elecPoses_), so the
    // body's cells meet the grid where the body WAS; the current pose when
    // that tick's is gone (a linked body never asked with a grid, a ring
    // overrun).
    const Mob::ElecPose* pose = nullptr;
    for (const Mob::ElecPose& e : m.elecPoses_)
      if (e.valid && p.hit && e.tick == p.hit->tick && e.xf.size() == m.limbs_.size()) pose = &e;
    for (int li = 0; li < (int)m.limbs_.size(); li++) {
      MobLimb& L = m.limbs_[li];
      if (!L.body) continue;
      const BurnLimbView v = m.ViewOf(L);
      if (v.xf == nullptr || v.Size() == 0) continue;
      ElecSlotCache& c = m.elecCache_[li];
      if (c.cells.empty()) continue;
      p.slotBase[li] = (int32_t)nodes.size();
      const BodyTransform& xf = pose ? pose->xf[li] : *v.xf;
      const Quat q{xf.quat[0], xf.quat[1], xf.quat[2], xf.quat[3]};
      const Quat qNow{v.xf->quat[0], v.xf->quat[1], v.xf->quat[2], v.xf->quat[3]};
      for (size_t ci = 0; ci < c.cells.size(); ci++) {
        const ElecBodyCell& e = c.cells[ci];
        const Vec3 local{(float)e.c[0] + 0.5f, (float)e.c[1] + 0.5f, (float)e.c[2] + 0.5f};
        const Vec3 w = xf.pos + QuatRotate(q, local);
        Node nd;
        nd.part = pi;
        nd.slot = li;
        nd.cell = (int32_t)ci;
        nd.w = {ifloor(w.x), ifloor(w.y), ifloor(w.z)};
        nd.at = w;
        nd.atNow = v.xf->pos + QuatRotate(qNow, local);
        nd.bulk = e.bulk;
        nd.entry = e.entry;
        nodes.push_back(nd);
      }
    }
  }
  if (nodes.empty()) return;
  // THE SPREADING LOSS (wave 2 package A, sim_elec.wgsl elecEnter): a cell
  // entered from a neighbour also loses prev x spreadQ(n) / 4096, n = its
  // conducting face neighbours past sim.elecSpreadFree (a wire's two cost
  // nothing; the current divides in bulk). n here counts the cell's own
  // slot's neighbours -- the body's inside.
  {
    const uint32_t k = (uint32_t)std::clamp(tn.sim.elecSpreadLoss, 0, 4095);
    const uint32_t free = (uint32_t)std::clamp(tn.sim.elecSpreadFree, 0, 6);
    for (Node& nd : nodes) {
      if (k == 0 || nd.bulk == kIns) continue;
      const ElecSlotCache& c = parts[nd.part].m->elecCache_[nd.slot];
      const ElecBodyCell& e = c.cells[nd.cell];
      uint32_t n = 0;
      for (const IVec3& d : kFace6) {
        const int x = e.c[0] + d.x - c.cmin.x, y = e.c[1] + d.y - c.cmin.y,
                  z = e.c[2] + d.z - c.cmin.z;
        if (x < 0 || y < 0 || z < 0 || x >= c.cdim.x || y >= c.cdim.y || z >= c.cdim.z) continue;
        const int32_t ci = c.at[((size_t)z * c.cdim.y + y) * c.cdim.x + x];
        if (ci >= 0 && c.cells[ci].bulk != kIns) n++;
      }
      if (n > free) nd.spreadQ = (uint16_t)std::min<uint32_t>((n - free) * k, 4095u);
    }
  }
  shockCounters_.bodiesSolved += parts.size();
  shockCounters_.cellsSolved += nodes.size();
  // World cell -> body cells there, sorted: contact between slots and bodies.
  std::vector<std::pair<uint64_t, uint32_t>> byCell;
  byCell.reserve(nodes.size());
  for (uint32_t k = 0; k < nodes.size(); k++) byCell.push_back({CellKey(nodes[k].w), k});
  std::sort(byCell.begin(), byCell.end());
  // ...and each distinct cell's run of that sorted list, behind an
  // open-addressed hash (PLAN_fight64_perf M). The solve below asks for seven
  // cells per settled node and the shell / air tests ask more; a binary search
  // of the whole list per ask was a sixth of this pass in the 64-creature
  // brawl. Same runs, same order inside a run: only the lookup changed.
  struct Run {
    uint64_t key = ~0ull;   // ~0 = empty (CellKey never sets the top bit)
    uint32_t begin = 0, end = 0;
  };
  size_t cap = 16;
  while (cap < byCell.size() * 2) cap <<= 1;
  std::vector<Run> runs(cap);
  const size_t runMask = cap - 1;
  auto slotOf = [&](uint64_t key) {
    return (size_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & runMask;
  };
  for (uint32_t i = 0; i < byCell.size();) {
    uint32_t j = i + 1;
    while (j < byCell.size() && byCell[j].first == byCell[i].first) j++;
    size_t s = slotOf(byCell[i].first);
    while (runs[s].key != ~0ull) s = (s + 1) & runMask;
    runs[s] = {byCell[i].first, i, j};
    i = j;
  }
  using CellIt = std::vector<std::pair<uint64_t, uint32_t>>::const_iterator;
  auto cellRange = [&](IVec3 c) -> std::pair<CellIt, CellIt> {
    const uint64_t key = CellKey(c);
    for (size_t s = slotOf(key);; s = (s + 1) & runMask) {
      const Run& r = runs[s];
      if (r.key == key)
        return {byCell.cbegin() + r.begin, byCell.cbegin() + r.end};
      if (r.key == ~0ull) return {byCell.cend(), byCell.cend()};
    }
  };
  // Is world cell c AIR to part pi: the grid says air there and none of its
  // own body cells sits in it.
  auto airFor = [&](uint32_t pi, IVec3 c) {
    if ((GridWord(parts[pi].hit, c) & kElecQueryGridAir) == 0) return false;
    const auto r = cellRange(c);
    for (auto it = r.first; it != r.second; ++it)
      if (nodes[it->second].part == pi) return false;
    return true;
  };

  // ---- THE SOLVE: max-plus to the least fixpoint ----------------------------
  std::vector<int32_t> P(nodes.size(), 0);
  std::priority_queue<std::pair<int32_t, uint32_t>> pq;
  // PER BODY, ONCE (PLAN_fight64_perf M): whether it wears anything at all
  // (no worn slot = no shell can be in any cell, so the shell tests below
  // are false without looking), each slot's LimbHasShells, and the highest P
  // in its answer's grid (no face can offer more, so a cell already reading
  // it has nothing to look for across its faces). All three only skip work
  // whose answer they already know.
  std::vector<uint8_t> partWorn(parts.size(), 0);
  std::vector<std::vector<int8_t>> partSlotShells(parts.size());
  std::vector<uint32_t> partGridMax(parts.size(), 0);
  for (uint32_t pi = 0; pi < parts.size(); pi++) {
    Mob& m = *parts[pi].m;
    partSlotShells[pi].assign(m.limbs_.size(), -1);
    for (int li = 0; li < (int)m.limbs_.size(); li++)
      if (m.IsWornSlot(li)) partWorn[pi] = 1;
    if (const ElecHit* h = parts[pi].hit)
      for (uint32_t w : h->grid) partGridMax[pi] = std::max(partGridMax[pi], w & kElecQueryGridPMax);
  }
  for (uint32_t k = 0; k < nodes.size(); k++) {
    const Node& nd = nodes[k];
    if (nd.entry == kIns) continue;
    // COVERED: a body cell takes the world's charge across a face only when no
    // WORN shell stands in the way -- no cell of a shell in its own world cell
    // or in the world cell across that face (BurnLimbView::WornAlong's rule,
    // at cell pitch). Otherwise the charge reaches it only THROUGH the shell:
    // a leather sole stands between the foot and the plate, an iron one
    // carries the plate's charge into it.
    Mob& owner = *parts[nd.part].m;
    const bool body = nd.slot < owner.baseLimbs_;
    const bool worn = partWorn[nd.part] != 0;
    auto shellAt = [&](IVec3 c) {
      if (!worn) return false;
      const auto r = cellRange(c);
      for (auto it = r.first; it != r.second; ++it) {
        const Node& o = nodes[it->second];
        if (o.part == nd.part && o.slot != nd.slot && owner.IsWornSlot(o.slot)) return true;
      }
      return false;
    };
    if (body && shellAt(nd.w)) {
      if (GridWord(parts[nd.part].hit, nd.w) & kElecQueryGridPMax) owner.shock_.covered++;
      continue;
    }
    uint32_t wp = GridWord(parts[nd.part].hit, nd.w) & kElecQueryGridPMax;
    // CONTACT: the six face cells, and -- across ONE air cell no part of this
    // body fills -- the cell beyond. A body cell is a world-pitch BIN of
    // voxels placed by its centre, so a sole resting on a plate can bin a
    // cell above the one touching it (measured: elec-stun's zombie, every
    // foot cell one air cell over the copper). Wave 1 dilated each limb box
    // by a cell for the same reason; this is that slack, and no more.
    // (Nothing past the grid's own maximum can be read: `wp` there is final.)
    for (int f = 0; f < 6 && wp < partGridMax[nd.part]; f++) {
      const IVec3& d = kFace6[f];
      const IVec3 n1{nd.w.x + d.x, nd.w.y + d.y, nd.w.z + d.z};
      for (int step = 1; step <= 2; step++) {
        const IVec3 n = step == 1 ? n1 : IVec3{n1.x + d.x, n1.y + d.y, n1.z + d.z};
        if (step == 2) {
          // Only across air...
          if ((GridWord(parts[nd.part].hit, n1) & kElecQueryGridAir) == 0) break;
        }
        const uint32_t pn = GridWord(parts[nd.part].hit, n) & kElecQueryGridPMax;
        if (pn <= wp) continue;
        if (step == 2) {
          // ...and not into the body's own cells. Asked only now that the
          // far cell has something to offer: either refusal ends the face.
          bool own = false;
          const auto r = cellRange(n1);
          for (auto it = r.first; it != r.second && !own; ++it) own = nodes[it->second].part == nd.part;
          if (own) break;
        }
        // A shell cell in the way, or a shell met on the way there
        // (Mob::WornShellAlong, the burn pass's ray: a limb is a rounded tube
        // in a garment cut to its box, and the two lattices' world-pitch
        // cells need not line up).
        bool behind = body && (shellAt(n1) || (step == 2 && shellAt(n)));
        int8_t& hasShells = partSlotShells[nd.part][(size_t)nd.slot];
        if (body && !behind && hasShells < 0) hasShells = owner.LimbHasShells(nd.slot) ? 1 : 0;
        if (body && !behind && hasShells > 0) {
          uint32_t sm = 0;
          behind = owner.WornShellAlong(nd.slot, nd.atNow, Vec3{(float)d.x, (float)d.y, (float)d.z},
                                        (float)step, kCoverMarchMax, &sm, nullptr) >= 0;
        }
        if (behind) owner.shock_.covered++;
        else wp = pn;
      }
    }
    const int32_t seed =
        (int32_t)wp - (int32_t)nd.entry - (int32_t)(((uint64_t)wp * nd.spreadQ) >> 12);
    if (seed > 0) {
      P[k] = seed;
      pq.push({seed, k});
      // ATTRIBUTION (CLAUDE.md rule 6): which rig slots took the world's
      // charge directly.
      owner.shock_.seededSlots |= 1ull << (nd.slot < 63 ? nd.slot : 63);
      // SANDVOX_SHOCK_DEBUG=1: every seeded cell of a body's first seeded tick
      // -- where it is, what it is made of, what it read (diagnostic only).
      static const bool dbg = std::getenv("SANDVOX_SHOCK_DEBUG") != nullptr;
      if (dbg && owner.shock_.firstTick == 0 && owner.shock_.ticks == 0) {
        std::string mats;
        BurnLimbView dv = owner.ViewOf(owner.limbs_[nd.slot]);
        const ElecSlotCache& dc = owner.elecCache_[nd.slot];
        std::vector<uint32_t> seen;
        for (size_t i = 0; i < dv.Size() && i < dc.vcell.size(); i++)
          if (dc.vcell[i] == nd.cell && dc.vexp[i] &&
              std::find(seen.begin(), seen.end(), dv.Mat(i)) == seen.end())
            seen.push_back(dv.Mat(i));
        for (uint32_t m : seen) mats += " " + std::to_string(m);
        std::vector<uint32_t> coats;
        for (size_t i = 0; i < dv.Size() && i < dc.vcell.size(); i++) {
          const uint16_t st = dv.Stain(i);
          if (dc.vcell[i] == nd.cell && dc.vexp[i] && BodyStainAmt(st) &&
              std::find(coats.begin(), coats.end(), BodyStainMat(st)) == coats.end())
            coats.push_back(BodyStainMat(st));
        }
        mats += " | coats";
        for (uint32_t m : coats) mats += " " + std::to_string(m);
        std::printf("shock-debug: tick %u mob %llu slot %d '%s' cell (%d,%d,%d) w (%d,%d,%d) "
                    "entry %u bulk %u read P %u seed %d | exposed mats%s\n",
                    tick, (unsigned long long)owner.id_, nd.slot, owner.SlotName(nd.slot).c_str(),
                    dc.cells[nd.cell].c[0], dc.cells[nd.cell].c[1], dc.cells[nd.cell].c[2], nd.w.x,
                    nd.w.y, nd.w.z, nd.entry, nd.bulk, wp, seed, mats.c_str());
      }
    }
  }
  // SANDVOX_SHOCK_DEBUG=1: a CHARGED box whose body took no seed -- where the
  // body sits in its box and what the grid holds under its lowest cell.
  static const bool shockDebug = std::getenv("SANDVOX_SHOCK_DEBUG") != nullptr;
  if (shockDebug) {
    for (uint32_t pi = 0; pi < parts.size(); pi++) {
      if (!parts[pi].charged) continue;
      bool any = false;
      int lowY = INT_MAX;
      IVec3 low{};
      for (uint32_t k = 0; k < nodes.size(); k++) {
        if (nodes[k].part != pi) continue;
        any = any || P[k] > 0;
        if (nodes[k].w.y < lowY) {
          lowY = nodes[k].w.y;
          low = nodes[k].w;
        }
      }
      if (any || parts[pi].m->shock_.ticks != 0) continue;
      const ElecHit* h = parts[pi].hit;
      std::printf("shock-debug: tick %u mob %llu UNSEEDED: box P %u, %u charged cells, sum %u, "
                  "lo (%d,%d,%d) dims %dx%dx%d, grid %zu cells; lowest body cell (%d,%d,%d) "
                  "grid there %04x, below %04x\n",
                  tick, (unsigned long long)parts[pi].m->id_, h->maxP, h->charged, h->sumP,
                  h->lo[0], h->lo[1], h->lo[2], h->dims[0], h->dims[1], h->dims[2], h->grid.size(),
                  low.x, low.y, low.z, GridWord(h, low), GridWord(h, {low.x, low.y - 1, low.z}));
      // Where the charge IS in the box, and the body cell nearest it.
      IVec3 cmn{INT_MAX, INT_MAX, INT_MAX}, cmx{INT_MIN, INT_MIN, INT_MIN};
      for (int z = 0; z < h->dims[2]; z++)
        for (int y = 0; y < h->dims[1]; y++)
          for (int x = 0; x < h->dims[0]; x++) {
            const IVec3 cc{h->lo[0] + x, h->lo[1] + y, h->lo[2] + z};
            if ((GridWord(h, cc) & kElecQueryGridPMax) == 0) continue;
            cmn = {std::min(cmn.x, cc.x), std::min(cmn.y, cc.y), std::min(cmn.z, cc.z)};
            cmx = {std::max(cmx.x, cc.x), std::max(cmx.y, cc.y), std::max(cmx.z, cc.z)};
          }
      int best = INT_MAX;
      IVec3 bw{};
      int bslot = -1;
      for (uint32_t k = 0; k < nodes.size(); k++) {
        if (nodes[k].part != pi) continue;
        const IVec3 w = nodes[k].w;
        const int dx = std::max({cmn.x - w.x, 0, w.x - cmx.x}), dy = std::max({cmn.y - w.y, 0, w.y - cmx.y}),
                  dz = std::max({cmn.z - w.z, 0, w.z - cmx.z});
        if (dx + dy + dz < best) {
          best = dx + dy + dz;
          bw = w;
          bslot = nodes[k].slot;
        }
      }
      std::printf("shock-debug:   charge spans (%d,%d,%d)..(%d,%d,%d); nearest body cell (%d,%d,%d) slot %d "
                  "'%s' at %d cells; root xf (%.2f,%.2f,%.2f)\n",
                  cmn.x, cmn.y, cmn.z, cmx.x, cmx.y, cmx.z, bw.x, bw.y, bw.z, bslot,
                  parts[pi].m->SlotName(bslot).c_str(), best, parts[pi].m->limbs_[0].xf.pos.x,
                  parts[pi].m->limbs_[0].xf.pos.y, parts[pi].m->limbs_[0].xf.pos.z);
      // The layers under the body: P>0 '#', air '.', anything else 'o';
      // body cells of this creature in the layer above marked 'B'.
      for (int yy = low.y - 2; yy <= low.y; yy++) {
        std::string row;
        for (int z = 0; z < h->dims[2]; z++) {
          for (int x = 0; x < h->dims[0]; x++) {
            const IVec3 cc{h->lo[0] + x, yy, h->lo[2] + z};
            const uint32_t gw = GridWord(h, cc);
            char ch = (gw & kElecQueryGridPMax) ? '#' : ((gw & kElecQueryGridAir) ? '.' : 'o');
            for (uint32_t k = 0; k < nodes.size(); k++)
              if (nodes[k].part == pi && nodes[k].w.x == cc.x && nodes[k].w.y == yy && nodes[k].w.z == cc.z)
                ch = 'B';
            row += ch;
          }
          row += ' ';
        }
        std::printf("shock-debug:   y %d: %s\n", yy, row.c_str());
      }
    }
  }
  while (!pq.empty()) {
    const int32_t pk = pq.top().first;
    const uint32_t k = pq.top().second;
    pq.pop();
    if (pk != P[k]) continue;
    const Node& nd = nodes[k];
    const Part& p = parts[nd.part];
    Mob& m = *p.m;
    auto relax = [&](uint32_t j, uint16_t cost) {
      if (cost == kIns) return;
      const int32_t cand =
          pk - (int32_t)cost - (int32_t)(((uint64_t)(uint32_t)pk * nodes[j].spreadQ) >> 12);
      if (cand > P[j]) {
        P[j] = cand;
        pq.push({cand, j});
      }
    };
    // Inside the slot: the six lattice-cell neighbours, at their bulk resist.
    const ElecSlotCache& c = m.elecCache_[nd.slot];
    const ElecBodyCell& e = c.cells[nd.cell];
    for (const IVec3& d : kFace6) {
      const int x = e.c[0] + d.x - c.cmin.x, y = e.c[1] + d.y - c.cmin.y, z = e.c[2] + d.z - c.cmin.z;
      if (x < 0 || y < 0 || z < 0 || x >= c.cdim.x || y >= c.cdim.y || z >= c.cdim.z) continue;
      const int32_t ci = c.at[((size_t)z * c.cdim.y + y) * c.cdim.x + x];
      if (ci >= 0) relax((uint32_t)(p.slotBase[nd.slot] + ci), c.cells[ci].bulk);
    }
    // Across slots and bodies: the cells sharing this world cell or a face of
    // it. A jointed limb of the same body is tissue continuing (bulk); a
    // shell, an item or another body is a surface crossed (entry).
    for (int f = -1; f < 6; f++) {
      const IVec3 w = f < 0 ? nd.w : IVec3{nd.w.x + kFace6[f].x, nd.w.y + kFace6[f].y,
                                           nd.w.z + kFace6[f].z};
      const auto r = cellRange(w);
      for (auto it = r.first; it != r.second; ++it) {
        const uint32_t j = it->second;
        const Node& o = nodes[j];
        if (o.part == nd.part && o.slot == nd.slot) continue;
        const bool tissue = o.part == nd.part && o.slot < m.baseLimbs_ && nd.slot < m.baseLimbs_;
        relax(j, tissue ? o.bulk : o.entry);
      }
    }
  }

  // ---- WHAT IT DOES: the materials' answer ----------------------------------
  auto q4 = [](float gain) {
    return (uint32_t)std::clamp(std::lround((double)gain * 2.0 * 16.0), 0l, 1l << 16);
  };
  const uint32_t igniteQ = tn.sim.elecMode != 0 ? q4(tn.sim.elecIgniteGain) : 0u;
  const uint32_t crackleQ = tn.sim.elecMode != 0 ? q4(tn.sim.elecCrackle) : 0u;
  const uint32_t thrLo = ElecCrackleThreshold(tn.sim.elecCrackleSparkP, crackleLoMat_, crackleLoSrc_);
  const uint32_t thrHi = ElecCrackleThreshold(tn.sim.elecCrackleArcP, crackleHiMat_, crackleHiSrc_);
  uint32_t ohmicLeft = kShockOhmicCellsPerTick, crackleLeft = kShockCrackleOpsPerTick;

  for (uint32_t pi = 0; pi < parts.size(); pi++) {
    Part& p = parts[pi];
    Mob& mob = *p.m;
    if (!mob.alive_) continue;
    Mob::ShockRecord& rec = mob.shock_;
    const uint32_t mobKey = (uint32_t)mob.id_ ^ (uint32_t)(mob.id_ >> 32);

    // Per slot: the hottest cell, what it feels, whether a worn conductor
    // carries charge.
    int32_t bodyPeak = 0;
    uint32_t charged = 0;
    std::vector<float> felt(mob.limbs_.size(), 0.0f);
    bool armour = false;
    // The body's charge per slot, for presentation (Mob::ElecSlotCharge).
    mob.elecSlotP_.assign(mob.limbs_.size(), 0u);
    mob.elecSolveTick_ = tick;
    for (int li = 0; li < (int)mob.limbs_.size(); li++) {
      if (p.slotBase[li] < 0) continue;
      const ElecSlotCache& c = mob.elecCache_[li];
      const bool worn = mob.IsWornSlot(li);
      for (size_t ci = 0; ci < c.cells.size(); ci++) {
        const int32_t pk = P[(size_t)p.slotBase[li] + ci];
        if (pk <= 0) continue;
        charged++;
        bodyPeak = std::max(bodyPeak, pk);
        mob.elecSlotP_[li] = std::max(mob.elecSlotP_[li], (uint32_t)pk);
        felt[li] = std::max(felt[li], (float)pk * (float)c.cells[ci].feel / 1000.0f);
        if (worn && c.cells[ci].bulk <= g.shockArmourResistMax) armour = true;
      }
    }
    if (charged == 0) continue;
    rec.maxP = std::max(rec.maxP, std::max(p.hit ? p.hit->maxP : 0u, (uint32_t)bodyPeak));
    rec.cellsPeak = std::max(rec.cellsPeak, charged);
    rec.peakCellP = std::max(rec.peakCellP, (uint32_t)bodyPeak);
    if (!p.charged) rec.viaBody++;

    const float armourGain = armour ? std::max(1.0f, g.shockArmourGain) : 1.0f;
    const float minP = (float)g.shockMinP * (armour ? g.shockArmourMinPScale : 1.0f);

    // ---- WHICH LIMBS FEEL IT, and how hard ----------------------------------
    struct Touch {
      int limb;
      float eff;
    };
    std::vector<Touch> touch;
    float peak = 0.0f, wetMax = 0.0f;
    for (int li = 0; li < mob.baseLimbs_ && li < (int)mob.limbs_.size(); li++) {
      if (felt[li] <= 0.0f) continue;
      const MobLimb& L = mob.limbs_[li];
      if (!L.body) continue;
      // WET: the share of the limb's coat that is a CONDUCTOR (water, blood,
      // brine: materials.json electric.resist), amount-weighted, 0..1.
      uint64_t num = 0;
      for (const CoatEntry& e : L.coat.top)
        if (e.mat != 0 && e.mat < matElecResist_.size() && matElecResist_[e.mat] != 0)
          num += e.sumAmt;
      const float wetF =
          L.coat.voxels ? std::min(1.0f, (float)num / (float)(kBodyStainAmtMax * L.coat.voxels))
                        : 0.0f;
      const float eff = felt[li] * (1.0f + std::max(0.0f, g.shockWetGain) * wetF) * armourGain;
      if (eff < minP) continue;
      touch.push_back({li, eff});
      peak = std::max(peak, eff);
      wetMax = std::max(wetMax, wetF);
    }
    if (!touch.empty()) {
      shockCounters_.bodiesShocked++;
      PushShockCue(mob, touch.front().limb, peak);  // the sound + HUD cue (presentation)
      if (rec.ticks == 0) rec.firstTick = tick;
      rec.ticks++;
      rec.lastTick = tick;
      rec.limbHits += (uint32_t)touch.size();
      rec.maxEffP = std::max(rec.maxEffP, peak);
      rec.wetMax = std::max(rec.wetMax, wetMax);
      rec.armour = rec.armour || armour;

      // ---- THE VITAL CORE the current crosses: the vital base limb with the
      // most authored hp (the torso on a human), alive.
      int core = -1;
      for (int li = 0; li < mob.baseLimbs_ && li < (int)mob.limbDefs_.size(); li++) {
        if (!mob.limbDefs_[li].vital || !mob.limbs_[li].body) continue;
        if (core < 0 || mob.limbDefs_[li].hp > mob.limbDefs_[core].hp) core = li;
      }

      // ---- HP ----------------------------------------------------------------
      float total = 0.0f;
      for (const Touch& t : touch) total += std::max(0.0f, g.shockHpPerKiloP) * t.eff / 1000.0f;
      const float scale =
          total > g.shockHpMaxPerTick && total > 0.0f ? g.shockHpMaxPerTick / total : 1.0f;
      const float share = std::clamp(g.shockTorsoShare, 0.0f, 1.0f);
      const DamageCtx ctx(DamageCause::Electric);
      float coreDose = 0.0f;
      for (const Touch& t : touch) {
        if (!mob.alive_) break;
        const float d = std::max(0.0f, g.shockHpPerKiloP) * t.eff / 1000.0f * scale;
        const float toCore = (core >= 0 && core != t.limb) ? d * share : 0.0f;
        const MobLimb& L = mob.limbs_[t.limb];
        if (d - toCore > 0.0f && L.body) {
          mob.Damage(L.body, d - toCore, L.xf.pos, 0.0f, ctx);
          rec.hp += d - toCore;
        }
        coreDose += toCore;
      }
      if (mob.alive_ && core >= 0 && coreDose > 0.0f && mob.limbs_[core].body) {
        mob.Damage(mob.limbs_[core].body, coreDose, mob.limbs_[core].xf.pos, 0.0f, ctx);
        rec.hp += coreDose;
      }

      // ---- STUN, and the lightning-class knock-down --------------------------
      if (mob.alive_) {
        const int lo = std::max(0, g.shockStunMinTicks), hi = std::max(lo, g.shockStunMaxTicks);
        const int want = std::clamp(
            (int)std::lround(std::max(0.0f, g.shockStunTicksPerKiloP) * peak / 1000.0f), lo, hi);
        const uint32_t until = tick + (uint32_t)want;
        if (until > mob.stunUntil_) {
          rec.stunTicks += until - std::max(mob.stunUntil_, tick);
          mob.stunUntil_ = until;
        }
        if (g.shockRagdollP > 0 && peak >= (float)g.shockRagdollP && !mob.Ragdolled()) {
          mob.StartRagdoll(std::max(0.0f, g.shockRagdollSeconds), "shock");
          if (mob.Ragdolled()) rec.ragdolls++;
        }
      }

      // ---- FIRE: hair, clothes and the touching limbs ------------------------
      // One roll per body per tick, then Mob::Ignite on each candidate (the
      // burn pass takes it from there; a wet voxel refuses, IgniteOneLimb).
      if (mob.alive_ && g.shockIgniteMinP > 0 && peak >= (float)g.shockIgniteMinP &&
          g.shockIgniteVoxels > 0 && g.shockIgniteGain > 0.0f) {
        const float chance = std::min(1.0f, g.shockIgniteGain * peak / 1000.0f);
        const uint32_t h = rng::Hash3((uint32_t)mob.id_ ^ 0xE1EC5u, tick, (uint32_t)(mob.id_ >> 32));
        if (rng::Unit01(h) < chance) {
          for (int li = 0; li < (int)mob.limbs_.size(); li++) {
            if (!mob.limbs_[li].body) continue;
            bool cand = mob.IsWornSlot(li) || (li < mob.baseLimbs_ && mob.IsBloodless(li));
            for (const Touch& t : touch) cand = cand || t.limb == li;
            if (!cand) continue;
            rec.ignited += mob.Ignite(li, (uint32_t)g.shockIgniteVoxels);
          }
        }
      }
    }

    // ---- OHMIC HEATING AND CRACKLE, per body cell (E2's rules) -------------
    // Rolled for every charged cell of every slot, the shells and the held
    // item included: a charged iron cuirass crackles, a wooden arm chars.
    std::vector<std::vector<std::pair<int32_t, bool>>> ohm(mob.limbs_.size());   // (cell, air)
    for (int li = 0; li < (int)mob.limbs_.size(); li++) {
      if (p.slotBase[li] < 0) continue;
      const ElecSlotCache& c = mob.elecCache_[li];
      for (size_t ci = 0; ci < c.cells.size(); ci++) {
        const uint32_t k = (uint32_t)p.slotBase[li] + (uint32_t)ci;
        const int32_t pk = P[k];
        if (pk <= 0) continue;
        const ElecBodyCell& e = c.cells[ci];
        const Node& nd = nodes[k];
        // The air faces are read only by the ohmic roll (a cell with ohmic
        // data under a live ignite gain) and by a crackle tier the cell's P
        // reaches; a cell that can do neither skips the six lookups
        // (PLAN_fight64_perf M) -- both rolls below then refuse it exactly as
        // they would have.
        const bool ohmicCan = e.ohmMat != 0 && e.ohmMat < matElec_.size() &&
                              matElec_[e.ohmMat].igniteCap != 0 && igniteQ != 0 &&
                              e.bulk != kIns;
        const bool crackleCan = crackleQ != 0 && ((uint32_t)pk >= thrHi ? crackleHiMat_ != 0
                                                  : (uint32_t)pk >= thrLo ? crackleLoMat_ != 0
                                                                           : false);
        uint32_t air = 0;
        if (ohmicCan || crackleCan)
          for (const IVec3& d : kFace6)
            air += airFor(pi, {nd.w.x + d.x, nd.w.y + d.y, nd.w.z + d.z}) ? 1u : 0u;
        const uint32_t cellKey = rng::Hash3(mobKey ^ (uint32_t)li * 0x9E3779B9u,
                                            (uint32_t)(uint16_t)e.c[0] | ((uint32_t)(uint16_t)e.c[1] << 16),
                                            (uint32_t)(uint16_t)e.c[2]);
        // OHMIC: E = P x the cell's resist (the wet rule in it).
        const ElecMat& em = e.ohmMat < matElec_.size() ? matElec_[e.ohmMat] : ElecMat{};
        const uint32_t prod = air != 0 ? em.igniteInto : em.charInto;
        if (e.ohmMat != 0 && prod != 0 && em.igniteCap != 0 && igniteQ != 0 && e.bulk != kIns) {
          const uint64_t E = (uint64_t)pk * e.bulk;
          const uint64_t lim = ((uint64_t)em.igniteCap << 4) / igniteQ;
          uint64_t ch = E < lim ? (E * igniteQ) >> 4 : em.igniteCap;
          ch = ch * (air + 2u) / 8u;
          if (ch != 0 && rng::Hash3(cellKey ^ kOhmSalt, tick, 0x0A11u) % kReactChanceDen < ch) {
            if (ohmicLeft > 0) {
              ohmicLeft--;
              ohm[li].push_back({(int32_t)ci, air != 0});
            } else {
              shockCounters_.ohmicRefused++;
            }
          }
        }
        // CRACKLE: over a tier's threshold, with an air face.
        if (air == 0 || crackleQ == 0) continue;
        uint32_t cm = 0, thr = 0;
        if ((uint32_t)pk >= thrHi) {
          cm = crackleHiMat_;
          thr = thrHi;
        } else if ((uint32_t)pk >= thrLo) {
          cm = crackleLoMat_;
          thr = thrLo;
        }
        if (cm == 0) continue;
        const uint32_t ch = std::min<uint32_t>((uint32_t)(((uint64_t)((uint32_t)pk - thr) * crackleQ) >> 4),
                                               kElecCrackleCap);
        const uint32_t rr = rng::Hash3(cellKey ^ kCrackleSalt, tick, 0xC4ACu);
        if (ch == 0 || rr % kReactChanceDen >= ch) continue;
        const uint32_t rot = rr >> 12;
        for (uint32_t f = 0; f < 6; f++) {
          const IVec3& d = kFace6[(f + rot) % 6u];
          const IVec3 n{nd.w.x + d.x, nd.w.y + d.y, nd.w.z + d.z};
          if (!airFor(pi, n) || !world.CellInWindow(n)) continue;
          if (crackleLeft == 0 || cellOps.size() >= kMaxCellOpsPerTick) {
            shockCounters_.crackleRefused++;
            break;
          }
          crackleLeft--;
          cellOps.push_back({World::SlotCellIndex(n), PackVoxNew(cm, 0u) | kCellOpIfAir});
          shockCounters_.crackleOps++;
          rec.crackles++;
          break;
        }
      }
    }
    // The rewrite: every voxel of a rolled cell whose material the current
    // changes (its own electric.ignite.into with an air face, its char
    // without), one lattice pass for each slot that rolled any.
    bool rewrote = false;
    for (int li = 0; li < (int)mob.limbs_.size(); li++) {
      if (ohm[li].empty()) continue;
      MobLimb& L = mob.limbs_[li];
      if (!L.body) continue;
      BurnLimbView v = mob.ViewOf(L);
      const ElecSlotCache& c = mob.elecCache_[li];
      std::vector<int8_t> act(c.cells.size(), 0);   // 0 none, 1 air face, 2 buried
      for (const auto& [ci, air] : ohm[li]) act[ci] = air ? 1 : 2;
      std::vector<std::pair<uint32_t, uint32_t>> rw;
      for (size_t i = 0; i < v.Size() && i < c.vcell.size(); i++) {
        const int32_t ci = c.vcell[i];
        if (ci < 0 || act[ci] == 0) continue;
        const uint32_t mt = v.Mat(i);
        if (mt == 0 || mt >= matElec_.size()) continue;
        const ElecMat& em = matElec_[mt];
        if (em.igniteInto == 0 && em.charInto == 0) continue;
        const uint32_t prod = act[ci] == 1 ? em.igniteInto : em.charInto;
        if (prod != 0) rw.push_back({(uint32_t)i, prod});
      }
      const uint32_t n = ElecRewriteLimb(v, rw);
      rec.ohmic += (uint32_t)ohm[li].size();
      rec.ohmicVoxels += n;
      shockCounters_.ohmicCells += ohm[li].size();
      shockCounters_.ohmicVoxels += n;
      rewrote = rewrote || n != 0;
      if (n != 0) mob.elecCache_[li].fresh = false;   // its cells changed matter
    }
    if (rewrote) {
      mob.burnFracDirty_ = true;
      mob.MarkInstancesDirty();
    }
  }
}

// One rig slot's lattice at world pitch (ElecSlotCache), refreshed for this
// tick. The GEOMETRY -- which world-pitch cell each voxel falls in, which
// voxels are exposed, which cells exist -- is rebuilt only when the digest of
// the voxels' positions and presence moves (a cut, a burn-through, a new
// lattice). The cells' resists, feel and ohmic material are re-accumulated
// from the voxels' materials and coats every call, in the same pass that
// computes that digest: one pass over the lattice a tick in the steady state.
namespace {
uint64_t GeomMix(uint64_t h, uint64_t x) {
  h ^= x;
  return h * 1099511628211ull;
}
}  // namespace

int MobSystem::RefreshElecSlotCache(const BurnLimbView& v, uint32_t wet, uint32_t tick,
                                    ElecSlotCache& c) {
  const size_t n = v.Size();
  const uint32_t s = std::max(1u, v.scale);
  // REUSED AS IT STANDS within the cadence, when nothing a re-accumulation
  // could not miss has moved: the lattice's size and scale, the table, the
  // wet knob. Tick-keyed, so a pure function of the tick stream.
  if (c.fresh && c.geomKey != 0 && c.vcell.size() == n && c.scale == s && c.gen == elecMatGen_ &&
      c.wet == wet && tick - c.lastFull < kElecSlotRefreshTicks)
    return 0;
  // ---- the geometry ---------------------------------------------------------
  auto buildGeometry = [&]() {
    c.cells.clear();
    c.at.clear();
    c.cmin = {0, 0, 0};
    c.cdim = {0, 0, 0};
    c.scale = s;
    c.vcell.assign(n, -1);
    c.vexp.assign(n, 0);
    uint64_t h = GeomMix(GeomMix(1469598103934665603ull, n), s);
    IVec3 mn{INT_MAX, INT_MAX, INT_MAX}, mx{INT_MIN, INT_MIN, INT_MIN};
    for (size_t i = 0; i < n; i++) {
      const IVec3 a = v.At(i);
      const bool here = v.Mat(i) != 0;
      h = GeomMix(h, ((uint64_t)((uint32_t)a.x & 0xFFFFu)) | ((uint64_t)((uint32_t)a.y & 0xFFFFu) << 16) |
                         ((uint64_t)((uint32_t)a.z & 0xFFFFu) << 32) | ((uint64_t)here << 48));
      if (!here) continue;
      mn.x = std::min(mn.x, a.x); mn.y = std::min(mn.y, a.y); mn.z = std::min(mn.z, a.z);
      mx.x = std::max(mx.x, a.x); mx.y = std::max(mx.y, a.y); mx.z = std::max(mx.z, a.z);
    }
    c.geomKey = h ? h : 1u;
    if (mn.x > mx.x) return;
    const IVec3 d{mx.x - mn.x + 1, mx.y - mn.y + 1, mx.z - mn.z + 1};
    std::vector<uint8_t> occ((size_t)d.x * d.y * d.z, 0);
    auto at = [&](IVec3 a) { return ((size_t)(a.z - mn.z) * d.y + (a.y - mn.y)) * d.x + (a.x - mn.x); };
    for (size_t i = 0; i < n; i++)
      if (v.Mat(i) != 0) occ[at(v.At(i))] = 1;
    const int si = (int)s;
    c.cmin = {FloorDiv(mn.x, si), FloorDiv(mn.y, si), FloorDiv(mn.z, si)};
    const IVec3 cmax{FloorDiv(mx.x, si), FloorDiv(mx.y, si), FloorDiv(mx.z, si)};
    c.cdim = {cmax.x - c.cmin.x + 1, cmax.y - c.cmin.y + 1, cmax.z - c.cmin.z + 1};
    c.at.assign((size_t)c.cdim.x * c.cdim.y * c.cdim.z, -1);
    // Cells in (z, y, x) order: the first voxel to land in a cell does not
    // decide its index, so the cell list is a pure function of the geometry.
    for (size_t i = 0; i < n; i++) {
      if (v.Mat(i) == 0) continue;
      const IVec3 a = v.At(i);
      const size_t k = ((size_t)(FloorDiv(a.z, si) - c.cmin.z) * c.cdim.y +
                        (FloorDiv(a.y, si) - c.cmin.y)) * c.cdim.x + (FloorDiv(a.x, si) - c.cmin.x);
      c.at[k] = 0;
    }
    for (size_t k = 0; k < c.at.size(); k++) {
      if (c.at[k] < 0) continue;
      ElecBodyCell cell;
      cell.c[0] = (int16_t)(c.cmin.x + (int)(k % (size_t)c.cdim.x));
      cell.c[1] = (int16_t)(c.cmin.y + (int)((k / (size_t)c.cdim.x) % (size_t)c.cdim.y));
      cell.c[2] = (int16_t)(c.cmin.z + (int)(k / ((size_t)c.cdim.x * c.cdim.y)));
      c.at[k] = (int32_t)c.cells.size();
      c.cells.push_back(cell);
    }
    for (size_t i = 0; i < n; i++) {
      if (v.Mat(i) == 0) continue;
      const IVec3 a = v.At(i);
      const size_t k = ((size_t)(FloorDiv(a.z, si) - c.cmin.z) * c.cdim.y +
                        (FloorDiv(a.y, si) - c.cmin.y)) * c.cdim.x + (FloorDiv(a.x, si) - c.cmin.x);
      c.vcell[i] = c.at[k];
      for (const IVec3& f : kFace6) {
        const IVec3 b{a.x + f.x, a.y + f.y, a.z + f.z};
        if (b.x < mn.x || b.y < mn.y || b.z < mn.z || b.x > mx.x || b.y > mx.y || b.z > mx.z ||
            !occ[at(b)]) {
          c.vexp[i] = 1;
          break;
        }
      }
    }
  };
  // ---- the resists: accumulate, and digest the geometry on the way ---------
  struct Acc {
    uint32_t n = 0, ns = 0, feel = 0;
    uint64_t g = 0, gs = 0;
    uint16_t om[4] = {0, 0, 0, 0};
    uint32_t oc[4] = {0, 0, 0, 0};
  };
  std::vector<Acc> acc;
  // THE CELL RESIST of a voxel, as a conductance: sim_elec.wgsl elecCellResist
  // on the CPU -- the material's resist, or the wet table's at the coat amount
  // when the coat conducts, whichever conducts better (conductance falls as
  // resist rises, so min(resist) is max(conductance)).
  uint32_t wetG[16];
  for (uint32_t a = 0; a < 16; a++) wetG[a] = a ? Conductance(ElecWetResist(wet, a)) : 0u;
  const size_t nm = matElecResist_.size();
  auto accumulate = [&]() -> uint64_t {
    acc.assign(c.cells.size(), Acc{});
    uint64_t h = GeomMix(GeomMix(1469598103934665603ull, n), s);
    auto run = [&](const auto* vx, auto matOf) {
      for (size_t i = 0; i < n; i++) {
        const auto& q = vx[i];
        const uint32_t m = matOf(q) & 0xFFFu;
        h = GeomMix(h, ((uint64_t)((uint32_t)(int32_t)q.x & 0xFFFFu)) |
                           ((uint64_t)((uint32_t)(int32_t)q.y & 0xFFFFu) << 16) |
                           ((uint64_t)((uint32_t)(int32_t)q.z & 0xFFFFu) << 32) |
                           ((uint64_t)(m != 0) << 48));
        const int32_t ci = c.vcell[i];
        if (m == 0 || ci < 0) continue;
        uint32_t gv = m < nm ? Conductance(matElecResist_[m] ? matElecResist_[m] : kIns) : 0u;
        const uint16_t st = q.stain;
        const uint32_t amt = BodyStainAmt(st);
        if (amt != 0) {
          const uint32_t cm = BodyStainMat(st);
          if (cm != 0 && cm < nm && matElecResist_[cm] != 0) gv = std::max(gv, wetG[std::min(amt, 15u)]);
        }
        Acc& e = acc[(size_t)ci];
        e.n++;
        e.g += gv;
        if (c.vexp[i]) {
          e.ns++;
          e.gs += gv;
        }
        if (m < matElec_.size()) {
          const ElecMat& em = matElec_[m];
          e.feel += em.feel;
          if (em.igniteInto != 0 || em.charInto != 0) {
            for (int k = 0; k < 4; k++) {
              if (e.om[k] == m || e.om[k] == 0) {
                e.om[k] = (uint16_t)m;
                e.oc[k]++;
                break;
              }
            }
          }
        }
      }
    };
    if (v.skin) run(v.skin->data(), [](const PrefabVoxel& q) { return (uint32_t)q.material; });
    else run(v.coll->data(), [](const DebrisVoxel& q) { return (uint32_t)q.payload; });
    return h ? h : 1u;
  };
  bool rebuilt = false;
  c.fresh = true;
  c.gen = elecMatGen_;
  c.wet = wet;
  c.lastFull = tick;
  if (c.geomKey == 0 || c.vcell.size() != n || c.scale != s) {
    buildGeometry();
    rebuilt = true;
  }
  if (accumulate() != c.geomKey) {
    buildGeometry();
    rebuilt = true;
    accumulate();
  }
  for (size_t k = 0; k < c.cells.size(); k++) {
    const Acc& e = acc[k];
    ElecBodyCell& cell = c.cells[k];
    cell.bulk = ParallelResist(e.n, e.g);
    cell.entry = ParallelResist(e.ns, e.gs);
    cell.feel = e.n ? (uint16_t)std::min<uint32_t>(e.feel / e.n, 1000u) : 0;
    cell.ohmMat = 0;
    uint32_t best = 0;
    for (int j = 0; j < 4; j++)
      if (e.om[j] != 0 && (e.oc[j] > best || (e.oc[j] == best && e.om[j] < cell.ohmMat))) {
        best = e.oc[j];
        cell.ohmMat = e.om[j];
      }
  }
  return rebuilt ? 2 : 1;
}

void MobSystem::TickStuns(uint32_t tick) {
  const Tuning::Gore& g = CurrentTuning().gore;
  const uint32_t every = (uint32_t)std::max(1, g.shockTwitchTicks);
  auto twitch = [&](Mob& m) {
    if (!m.Stunned(tick) || m.def_ == nullptr || m.IsGhost() || m.Ragdolled()) return;
    if ((tick - m.shock_.lastTick) % every != 0) return;
    const int n = std::min<int>(m.baseLimbs_, (int)m.limbs_.size());
    if (n <= 0) return;
    const uint32_t h = rng::Hash3((uint32_t)m.id_ ^ 0x7A11C4u, tick, 0x5u);
    const int limb = (int)(h % (uint32_t)n);
    if (!m.limbs_[limb].body) return;
    const Vec3 dir{rng::SignedUnit(rng::Hash3(h, 1u, 0u)), 0.25f * rng::SignedUnit(rng::Hash3(h, 2u, 0u)),
                   rng::SignedUnit(rng::Hash3(h, 3u, 0u))};
    m.HitReact(limb, dir, std::max(0.0f, g.shockTwitchHp), 1.0f);
    m.shock_.twitches++;
  };
  for (Mob* av : avatars_)
    if (av != nullptr) twitch(*av);
  for (Mob& m : mobs_) twitch(m);
}

// The shock's cue for the frame (MobSystem::ShockCues): where it bit, how hard.
// Intensity is the effective P against the knock-down P (gore.shockRagdollP),
// so a lightning-class jolt is 1 and a tingle from wet ground is a fraction.
void MobSystem::PushShockCue(const Mob& m, int limb, float peakP) {
  constexpr size_t kMaxShockCues = 64;
  if (shockCues_.size() >= kMaxShockCues) return;
  const Tuning::Gore& g = CurrentTuning().gore;
  ShockCue c;
  c.mobId = m.id_;
  c.posVoxel = limb >= 0 && limb < (int)m.limbs_.size() ? m.limbs_[limb].xf.pos : Vec3{};
  const float ref = g.shockRagdollP > 0 ? (float)g.shockRagdollP : 30000.0f;
  c.intensity = std::clamp(peakP / ref, 0.05f, 1.0f);
  shockCues_.push_back(c);
}
