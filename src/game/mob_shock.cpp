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
#include <functional>
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

// Pool-call timing for SANDVOX_SHOCK_DIGEST (diagnostic only, main thread).
struct ParStats {
  uint64_t calls = 0;
  double wall = 0, first = 0, maxTask = 0, sumTask = 0;
};
ParStats g_parStats;

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
  // Phase clocks (ShockCounters::phaseNanos; diagnostic only, never read by
  // the sim).
  auto tLap = std::chrono::steady_clock::now();
  auto lap = [&](int ph) {
    const auto t = std::chrono::steady_clock::now();
    shockCounters_.phaseNanos[ph] +=
        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t - tLap).count();
    tLap = t;
  };

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

  lap(7);
  // ---- THE BODY CELLS -------------------------------------------------------
  const uint32_t wet = (uint32_t)std::clamp(tn.sim.elecWetResist, 1, (int)kElecResistInsulator - 1);
  struct Node {
    uint32_t part = 0;
    int32_t slot = -1;
    int32_t cell = -1;      // index into the slot's ElecSlotCache::cells
    IVec3 w{};              // the world cell it sits in, this pose
    Vec3 atNow{};           // its centre under the CURRENT pose (the cover ray)
    uint16_t bulk = kIns, entry = kIns;
    uint16_t spreadQ = 0;   // the spreading loss of entering it, /4096 of what arrives
  };
  // ---- THE POOL, AND WHEN IT MAY NOT BE USED (PLAN_fight64_perf round 3 K) --
  // Every parallel phase below is one task per SLOT or per PART and writes
  // only that slot's or part's state: the slot's cache and its nodes, P of
  // the part's own nodes, its owner's shock record, its rolls. That is one
  // writer per item only while no creature is listed twice (never expected:
  // one answer per creature per tick) -- and the shock debug prints in node
  // order -- so either case runs every phase serially, in order: the serial
  // loop IS the result. FEW calls, not many: a pool call costs ~100 us of
  // wake and join on top of its longest task (measured: SANDVOX_SHOCK_DIGEST's
  // shock-pool line), which is most of a phase this size. Serially the tasks
  // run in plain index order (the debug prints are in node order).
  static const bool dbgSeed = std::getenv("SANDVOX_SHOCK_DEBUG") != nullptr;
  bool unique = true;
  {
    std::vector<const Mob*> ms;
    ms.reserve(parts.size());
    for (const Part& p : parts) ms.push_back(p.m);
    std::sort(ms.begin(), ms.end());
    unique = std::adjacent_find(ms.begin(), ms.end()) == ms.end();
  }
  const bool pooled = unique && !dbgSeed;
  // SANDVOX_SHOCK_DIGEST also times every pool call: its wall, when its
  // first task started, its longest task, the sum of its tasks (diagnostic
  // only: is a phase slow because of its work or because of the pool?).
  static const bool parTimed = std::getenv("SANDVOX_SHOCK_DIGEST") != nullptr;
  auto par = [&](size_t n, const std::function<void(size_t)>& fn) {
    if (!parTimed || n == 0) {
      if (pooled) workpool::ParallelFor(n, 1, fn);
      else for (size_t i = 0; i < n; i++) fn(i);
      return;
    }
    using clk = std::chrono::steady_clock;
    std::vector<clk::time_point> t0s(n), t1s(n);
    const clk::time_point c0 = clk::now();
    auto timed = [&](size_t i) {
      t0s[i] = clk::now();
      fn(i);
      t1s[i] = clk::now();
    };
    if (pooled) workpool::ParallelFor(n, 1, timed);
    else for (size_t i = 0; i < n; i++) timed(i);
    const clk::time_point c1 = clk::now();
    auto us = [](clk::duration d) { return std::chrono::duration<double, std::micro>(d).count(); };
    clk::time_point first = t0s[0];
    double maxT = 0, sumT = 0;
    for (size_t i = 0; i < n; i++) {
      first = std::min(first, t0s[i]);
      maxT = std::max(maxT, us(t1s[i] - t0s[i]));
      sumT += us(t1s[i] - t0s[i]);
    }
    g_parStats.calls++;
    g_parStats.wall += us(c1 - c0);
    g_parStats.first += us(first - c0);
    g_parStats.maxTask += maxT;
    g_parStats.sumTask += sumT;
  };
  const uint32_t spreadK = (uint32_t)std::clamp(tn.sim.elecSpreadLoss, 0, 4095);
  const uint32_t spreadFree = (uint32_t)std::clamp(tn.sim.elecSpreadFree, 0, 6);

  // ---- PHASE A, ONE TASK PER SLOT: its cache, then its nodes ---------------
  // EVERY SLOT'S CACHE (PLAN_fight64_perf M): RefreshElecSlotCache is a pure
  // function of one slot's lattice, the material tables and the tick, and
  // writes only that slot's cache. The same task then lays out the slot's
  // NODES from it (round 3 K): one cell each, placed by the pose the box was
  // asked with, with its spreading loss (wave 2 package A, sim_elec.wgsl
  // elecEnter: a cell entered from a neighbour also loses prev x spreadQ(n) /
  // 4096, n = its conducting face neighbours in its own slot past
  // sim.elecSpreadFree). The slots' lists are then laid end to end in (part,
  // slot) order -- the very order the serial loop pushed them in.
  struct SlotJob {
    uint32_t part = 0;
    int32_t li = -1;
    BurnLimbView v;
    const BodyTransform* xf = nullptr;   // the pose the box was asked with
    int how = 0;
    uint64_t nanos = 0;
    uint64_t cost = 0;   // what the task is expected to take (its place in the queue)
    std::vector<Node> nodes;
  };
  // SCRATCH KEPT ACROSS TICKS (this pass runs on the main thread only): a
  // tick's fresh 100-KiB vectors were page faults and allocator time on every
  // solved tick. Jobs keep their node lists' capacity too.
  static std::vector<SlotJob> jobs;
  size_t nj = 0;
  for (uint32_t pi = 0; pi < parts.size(); pi++) {
    Part& p = parts[pi];
    Mob& m = *p.m;
    if (m.elecCache_.size() != m.limbs_.size()) m.elecCache_.resize(m.limbs_.size());
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
      if (nj == jobs.size()) jobs.emplace_back();
      SlotJob& j = jobs[nj++];
      j.v = v;
      j.part = pi;
      j.li = li;
      j.xf = pose ? &pose->xf[li] : j.v.xf;
      j.how = 0;
      j.nanos = 0;
      j.nodes.clear();
      // LONGEST FIRST: a slot whose cache stands is laid out and nothing else;
      // one that must be re-accumulated walks its lattice, and one whose
      // geometry is new walks it three times. The pool takes tasks in index
      // order, so the queue below starts the long ones first and the wall is
      // not one big torso started last.
      const ElecSlotCache& c = m.elecCache_[li];
      const uint32_t s = std::max(1u, v.scale);
      const bool stands = c.fresh && c.geomKey != 0 && c.vcell.size() == v.Size() && c.scale == s &&
                          c.gen == elecMatGen_ && c.wet == wet && tick - c.lastFull < kElecSlotRefreshTicks;
      j.cost = stands ? (uint64_t)c.cells.size() : (uint64_t)v.Size() * (c.geomKey == 0 ? 3u : 1u);
    }
  }
  std::vector<uint32_t> jobOrder(nj);
  for (uint32_t k = 0; k < nj; k++) jobOrder[k] = k;
  if (pooled)
    std::stable_sort(jobOrder.begin(), jobOrder.end(),
                     [&](uint32_t a, uint32_t b) { return jobs[a].cost > jobs[b].cost; });
  par(nj, [&](size_t t) {
    SlotJob& j = jobs[jobOrder[t]];
    Mob& m = *parts[j.part].m;
    ElecSlotCache& c = m.elecCache_[j.li];
    const auto r0 = std::chrono::steady_clock::now();
    j.how = RefreshElecSlotCache(j.v, wet, tick, c);
    j.nanos = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - r0)
                  .count();
    if (c.cells.empty()) return;
    const BodyTransform& xf = *j.xf;
    const Quat q{xf.quat[0], xf.quat[1], xf.quat[2], xf.quat[3]};
    const Quat qNow{j.v.xf->quat[0], j.v.xf->quat[1], j.v.xf->quat[2], j.v.xf->quat[3]};
    j.nodes.resize(c.cells.size());
    for (size_t ci = 0; ci < c.cells.size(); ci++) {
      const ElecBodyCell& e = c.cells[ci];
      const Vec3 local{(float)e.c[0] + 0.5f, (float)e.c[1] + 0.5f, (float)e.c[2] + 0.5f};
      const Vec3 w = xf.pos + QuatRotate(q, local);
      Node& nd = j.nodes[ci];
      nd.part = j.part;
      nd.slot = j.li;
      nd.cell = (int32_t)ci;
      nd.w = {ifloor(w.x), ifloor(w.y), ifloor(w.z)};
      nd.atNow = j.v.xf->pos + QuatRotate(qNow, local);
      nd.bulk = e.bulk;
      nd.entry = e.entry;
      if (spreadK != 0 && nd.bulk != kIns) {
        uint32_t n = 0;
        for (const IVec3& d : kFace6) {
          const int x = e.c[0] + d.x - c.cmin.x, y = e.c[1] + d.y - c.cmin.y,
                    z = e.c[2] + d.z - c.cmin.z;
          if (x < 0 || y < 0 || z < 0 || x >= c.cdim.x || y >= c.cdim.y || z >= c.cdim.z) continue;
          const int32_t cj = c.at[((size_t)z * c.cdim.y + y) * c.cdim.x + x];
          if (cj >= 0 && c.cells[cj].bulk != kIns) n++;
        }
        if (n > spreadFree) nd.spreadQ = (uint16_t)std::min<uint32_t>((n - spreadFree) * spreadK, 4095u);
      }
    }
  });
  static std::vector<Node> nodesScratch;
  std::vector<Node>& nodes = nodesScratch;
  nodes.clear();
  std::vector<uint32_t> partBegin(parts.size() + 1, 0);
  {
    size_t total = 0;
    for (size_t k = 0; k < nj; k++) total += jobs[k].nodes.size();
    nodes.reserve(total);
  }
  for (size_t k = 0, pi = 0; pi <= parts.size(); pi++) {
    partBegin[pi] = (uint32_t)nodes.size();
    if (pi == parts.size()) break;
    for (; k < nj && jobs[k].part == pi; k++) {
      const SlotJob& j = jobs[k];
      if (j.how == 2) shockCounters_.cacheBuilds++;
      else if (j.how == 1) shockCounters_.cacheAccums++;
      else shockCounters_.cacheHits++;
      shockCounters_.refreshNanos += j.nanos;
      if (j.nodes.empty()) continue;
      parts[pi].slotBase[j.li] = (int32_t)nodes.size();
      nodes.insert(nodes.end(), j.nodes.begin(), j.nodes.end());
    }
  }
  lap(0);
  if (nodes.empty()) return;
  shockCounters_.bodiesSolved += parts.size();
  shockCounters_.cellsSolved += nodes.size();
  // Per part: its nodes' cell box, whether it wears anything, and the highest
  // P in its answer's grid (no face can offer more).
  struct PartInfo {
    IVec3 lo{INT_MAX, INT_MAX, INT_MAX}, hi{INT_MIN, INT_MIN, INT_MIN};
    uint32_t gridMax = 0;
    uint8_t worn = 0;
  };
  std::vector<PartInfo> info(parts.size());
  for (uint32_t pi = 0; pi < parts.size(); pi++) {
    PartInfo& I = info[pi];
    for (uint32_t k = partBegin[pi]; k < partBegin[pi + 1]; k++) {
      const IVec3& w = nodes[k].w;
      I.lo = {std::min(I.lo.x, w.x), std::min(I.lo.y, w.y), std::min(I.lo.z, w.z)};
      I.hi = {std::max(I.hi.x, w.x), std::max(I.hi.y, w.y), std::max(I.hi.z, w.z)};
    }
  }
  // (`worn` and `gridMax` are the part's own seeds task's to fill.)

  // ---- WORLD CELL -> BODY CELLS, ONE INDEX ----------------------------------
  // Contact between slots and bodies: every node by the world cell it sits
  // in, behind an open-addressed hash (PLAN_fight64_perf M). BUCKETED, NOT
  // SORTED (round 3 K): count each cell's nodes into the hash, lay the runs
  // end to end, drop every node into its run in node order -- the runs a
  // sort by (cell, node) made, without the sort.
  struct Run {
    uint64_t key = ~0ull;   // ~0 = empty (CellKey never sets the top bit)
    uint32_t begin = 0, end = 0;
  };
  static std::vector<Run> runs;
  static std::vector<uint32_t> byCell;
  byCell.resize(nodes.size());
  size_t runMask = 0;
  auto slotOf = [](uint64_t key, size_t mask) {
    return (size_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & mask;
  };
  {
    size_t cap = 16;
    while (cap < nodes.size() * 2) cap <<= 1;
    runs.assign(cap, Run{});
    runMask = cap - 1;
    static std::vector<uint32_t> slotOfNode;
    slotOfNode.resize(nodes.size());
    for (uint32_t k = 0; k < nodes.size(); k++) {
      const uint64_t key = CellKey(nodes[k].w);
      size_t s = slotOf(key, runMask);
      while (runs[s].key != ~0ull && runs[s].key != key) s = (s + 1) & runMask;
      runs[s].key = key;
      runs[s].end++;   // a count, for now
      slotOfNode[k] = (uint32_t)s;
    }
    uint32_t at = 0;
    for (Run& r : runs) {
      if (r.key == ~0ull) continue;
      r.begin = at;
      at += r.end;
      r.end = r.begin;   // the fill cursor
    }
    for (uint32_t k = 0; k < nodes.size(); k++) byCell[runs[slotOfNode[k]].end++] = k;
  }
  auto cellRange = [&](IVec3 c) -> std::pair<const uint32_t*, const uint32_t*> {
    const uint64_t key = CellKey(c);
    for (size_t s = slotOf(key, runMask);; s = (s + 1) & runMask) {
      const Run& r = runs[s];
      if (r.key == key) return {byCell.data() + r.begin, byCell.data() + r.end};
      if (r.key == ~0ull) return {nullptr, nullptr};
    }
  };
  // Is world cell c AIR to part pi: the grid says air there and none of its
  // own body cells sits in it.
  auto airFor = [&](uint32_t pi, IVec3 c) {
    if ((GridWord(parts[pi].hit, c) & kElecQueryGridAir) == 0) return false;
    const auto r = cellRange(c);
    for (const uint32_t* it = r.first; it != r.second; ++it)
      if (nodes[*it].part == pi) return false;
    return true;
  };

  lap(2);
  // ---- CONNECTED COMPONENTS (ranked item 4) ---------------------------------
  // Charge crosses between two parts only where a cell of one sits in, or on
  // a face of, a cell of the other. So parts whose cell boxes, dilated a
  // cell, do not meet cannot exchange anything, and each group of parts that
  // do is solved on its own, the groups across the pool. A box test
  // over-merges, which costs parallelism and never correctness. (Measured:
  // the 64-creature brawl's charged bodies are pressed into ONE group, 26 of
  // 28 parts -- this buys nothing there; it is for separate puddles, rooms,
  // fights.)
  std::vector<uint32_t> root(parts.size());
  for (uint32_t i = 0; i < parts.size(); i++) root[i] = i;
  auto findRoot = [&](uint32_t i) {
    while (root[i] != i) i = root[i] = root[root[i]];
    return i;
  };
  for (uint32_t i = 0; i < parts.size(); i++) {
    if (partBegin[i] == partBegin[i + 1]) continue;
    for (uint32_t j = i + 1; j < parts.size(); j++) {
      if (partBegin[j] == partBegin[j + 1]) continue;
      const PartInfo &a = info[i], &b = info[j];
      if (a.lo.x > b.hi.x + 1 || b.lo.x > a.hi.x + 1 || a.lo.y > b.hi.y + 1 || b.lo.y > a.hi.y + 1 ||
          a.lo.z > b.hi.z + 1 || b.lo.z > a.hi.z + 1)
        continue;
      const uint32_t ri = findRoot(i), rj = findRoot(j);
      if (ri != rj) root[std::max(ri, rj)] = std::min(ri, rj);
    }
  }
  std::vector<std::vector<uint32_t>> comps;   // parts, ascending
  {
    std::vector<uint32_t> compOf(parts.size(), UINT32_MAX);
    for (uint32_t i = 0; i < parts.size(); i++) {
      if (partBegin[i] == partBegin[i + 1]) continue;
      const uint32_t r = findRoot(i);
      if (compOf[r] == UINT32_MAX) {
        compOf[r] = (uint32_t)comps.size();
        comps.emplace_back();
      }
      comps[compOf[r]].push_back(i);
    }
  }
  lap(1);

  // ---- ONE NODE'S EDGES --------------------------------------------------------
  // The six lattice-cell neighbours of its own slot at their bulk resist, then
  // the cells sharing its world cell or a face of it -- a jointed limb of the
  // same body is tissue continuing (bulk); a shell, an item or another body is
  // a surface crossed (entry). `fn(j, cost)` for every edge that conducts.
  auto forEdges = [&](uint32_t k, auto&& fn) {
    const Node& nd = nodes[k];
    const Part& p = parts[nd.part];
    const Mob& m = *p.m;
    const ElecSlotCache& c = m.elecCache_[nd.slot];
    const ElecBodyCell& e = c.cells[nd.cell];
    for (const IVec3& d : kFace6) {
      const int x = e.c[0] + d.x - c.cmin.x, y = e.c[1] + d.y - c.cmin.y, z = e.c[2] + d.z - c.cmin.z;
      if (x < 0 || y < 0 || z < 0 || x >= c.cdim.x || y >= c.cdim.y || z >= c.cdim.z) continue;
      const int32_t cj = c.at[((size_t)z * c.cdim.y + y) * c.cdim.x + x];
      if (cj >= 0 && c.cells[cj].bulk != kIns) fn((uint32_t)(p.slotBase[nd.slot] + cj), c.cells[cj].bulk);
    }
    for (int f = -1; f < 6; f++) {
      const IVec3 w = f < 0 ? nd.w : IVec3{nd.w.x + kFace6[f].x, nd.w.y + kFace6[f].y,
                                           nd.w.z + kFace6[f].z};
      const auto r = cellRange(w);
      for (const uint32_t* it = r.first; it != r.second; ++it) {
        const uint32_t j = *it;
        const Node& o = nodes[j];
        if (o.part == nd.part && o.slot == nd.slot) continue;
        const bool tissue = o.part == nd.part && o.slot < m.baseLimbs_ && nd.slot < m.baseLimbs_;
        const uint16_t cost = tissue ? o.bulk : o.entry;
        if (cost != kIns) fn(j, cost);
      }
    }
  };
  static const int solveMode = [] {
    const char* e = std::getenv("SANDVOX_SHOCK_SOLVE");
    return e ? std::atoi(e) : 1;
  }();
  // EVERY NODE'S EDGES, WRITTEN DOWN IN THE SEEDS' TASKS (round 3 K). The
  // solve below is serial over one connected mass (a brawl is one), and it
  // was memory-bound: a hash probe per face per settled node, and the slot
  // caches of twenty bodies. The same lookups, made once per node across the
  // pool, leave the solve a walk of flat arrays: (target, cost) per edge, and
  // the targets' spreading loss beside P.
  struct Adj {
    std::vector<uint32_t> begin;                        // local node -> first edge
    std::vector<std::pair<uint32_t, uint16_t>> e;      // (global node, cost)
  };
  static std::vector<Adj> adjPool;   // reused across ticks (main-thread pass)
  if (adjPool.size() < parts.size()) adjPool.resize(parts.size());
  std::vector<uint16_t> spreadQ(nodes.size());
  for (uint32_t k = 0; k < nodes.size(); k++) spreadQ[k] = nodes[k].spreadQ;

  // ---- THE SEEDS, ONE TASK PER PART -----------------------------------------
  // A part's seeds read only its own cells, its own answer's grid and its own
  // shells, and write P of its own nodes and its owner's shock record.
  // (WornShellAlong's march index is the owner's, too.)
  std::vector<int32_t> P(nodes.size(), 0);
  std::vector<uint8_t> partSeeded(parts.size(), 0);   // took a seed (the debug report)
  // Largest part first (the pool takes tasks in index order).
  std::vector<uint32_t> partOrder(parts.size());
  for (uint32_t pi = 0; pi < parts.size(); pi++) partOrder[pi] = pi;
  if (pooled)
    std::stable_sort(partOrder.begin(), partOrder.end(), [&](uint32_t x, uint32_t y) {
      return partBegin[x + 1] - partBegin[x] > partBegin[y + 1] - partBegin[y];
    });
  par(parts.size(), [&](size_t t) {
    const uint32_t pi = partOrder[t];
    if (partBegin[pi] == partBegin[pi + 1]) return;
    Mob& owner = *parts[pi].m;
    const ElecHit* hit = parts[pi].hit;
    PartInfo& I = info[pi];
    for (int li = 0; li < (int)owner.limbs_.size(); li++)
      if (owner.IsWornSlot(li)) I.worn = 1;
    if (hit)
      for (uint32_t w : hit->grid) I.gridMax = std::max(I.gridMax, w & kElecQueryGridPMax);
    const uint32_t gridMax = I.gridMax;
    const bool worn = I.worn != 0;
    // PER BODY, ONCE (PLAN_fight64_perf M): whether it wears anything at all
    // (no worn slot = no shell can be in any cell, so the shell tests below
    // are false without looking), each slot's LimbHasShells, and the highest P
    // in its answer's grid (no face can offer more, so a cell already reading
    // it has nothing to look for across its faces). All three only skip work
    // whose answer they already know.
    std::vector<int8_t> slotShells(owner.limbs_.size(), -1);
    for (uint32_t k = partBegin[pi]; k < partBegin[pi + 1]; k++) {
      const Node& nd = nodes[k];
      if (nd.entry == kIns) continue;
      // COVERED: a body cell takes the world's charge across a face only when no
      // WORN shell stands in the way -- no cell of a shell in its own world cell
      // or in the world cell across that face (BurnLimbView::WornAlong's rule,
      // at cell pitch). Otherwise the charge reaches it only THROUGH the shell:
      // a leather sole stands between the foot and the plate, an iron one
      // carries the plate's charge into it.
      const bool body = nd.slot < owner.baseLimbs_;
      auto shellAt = [&](IVec3 c) {
        if (!worn) return false;
        const auto r = cellRange(c);
        for (const uint32_t* it = r.first; it != r.second; ++it) {
          const Node& o = nodes[*it];
          if (o.part == nd.part && o.slot != nd.slot && owner.IsWornSlot(o.slot)) return true;
        }
        return false;
      };
      if (body && shellAt(nd.w)) {
        if (GridWord(hit, nd.w) & kElecQueryGridPMax) owner.shock_.covered++;
        continue;
      }
      uint32_t wp = GridWord(hit, nd.w) & kElecQueryGridPMax;
      // CONTACT: the six face cells, and -- across ONE air cell no part of this
      // body fills -- the cell beyond. A body cell is a world-pitch BIN of
      // voxels placed by its centre, so a sole resting on a plate can bin a
      // cell above the one touching it (measured: elec-stun's zombie, every
      // foot cell one air cell over the copper). Wave 1 dilated each limb box
      // by a cell for the same reason; this is that slack, and no more.
      // (Nothing past the grid's own maximum can be read: `wp` there is final.)
      for (int f = 0; f < 6 && wp < gridMax; f++) {
        const IVec3& d = kFace6[f];
        const IVec3 n1{nd.w.x + d.x, nd.w.y + d.y, nd.w.z + d.z};
        for (int step = 1; step <= 2; step++) {
          const IVec3 n = step == 1 ? n1 : IVec3{n1.x + d.x, n1.y + d.y, n1.z + d.z};
          if (step == 2) {
            // Only across air...
            if ((GridWord(hit, n1) & kElecQueryGridAir) == 0) break;
          }
          const uint32_t pnv = GridWord(hit, n) & kElecQueryGridPMax;
          if (pnv <= wp) continue;
          if (step == 2) {
            // ...and not into the body's own cells. Asked only now that the
            // far cell has something to offer: either refusal ends the face.
            bool own = false;
            const auto r = cellRange(n1);
            for (const uint32_t* it = r.first; it != r.second && !own; ++it)
              own = nodes[*it].part == nd.part;
            if (own) break;
          }
          // A shell cell in the way, or a shell met on the way there
          // (Mob::WornShellAlong, the burn pass's ray: a limb is a rounded tube
          // in a garment cut to its box, and the two lattices' world-pitch
          // cells need not line up).
          bool behind = body && (shellAt(n1) || (step == 2 && shellAt(n)));
          int8_t& hasShells = slotShells[(size_t)nd.slot];
          if (body && !behind && hasShells < 0) hasShells = owner.LimbHasShells(nd.slot) ? 1 : 0;
          if (body && !behind && hasShells > 0) {
            uint32_t sm = 0;
            behind = owner.WornShellAlong(nd.slot, nd.atNow, Vec3{(float)d.x, (float)d.y, (float)d.z},
                                          (float)step, kCoverMarchMax, &sm, nullptr) >= 0;
          }
          if (behind) owner.shock_.covered++;
          else wp = pnv;
        }
      }
      const int32_t seed =
          (int32_t)wp - (int32_t)nd.entry - (int32_t)(((uint64_t)wp * nd.spreadQ) >> 12);
      if (seed > 0) {
        P[k] = seed;
        partSeeded[pi] = 1;
        // ATTRIBUTION (CLAUDE.md rule 6): which rig slots took the world's
        // charge directly.
        owner.shock_.seededSlots |= 1ull << (nd.slot < 63 ? nd.slot : 63);
        // SANDVOX_SHOCK_DEBUG=1: every seeded cell of a body's first seeded tick
        // -- where it is, what it is made of, what it read (diagnostic only;
        // the phases run serially, in node order, while it is set).
        if (dbgSeed && owner.shock_.firstTick == 0 && owner.shock_.ticks == 0) {
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
    if (solveMode != 0) {
      Adj& A = adjPool[pi];
      const uint32_t n0 = partBegin[pi], n1 = partBegin[pi + 1];
      A.begin.resize((size_t)(n1 - n0) + 1);
      A.e.clear();
      for (uint32_t k = n0; k < n1; k++) {
        A.begin[k - n0] = (uint32_t)A.e.size();
        forEdges(k, [&](uint32_t j, uint16_t cost) { A.e.push_back({j, cost}); });
      }
      A.begin[n1 - n0] = (uint32_t)A.e.size();
    }
  });
  lap(3);
  // SANDVOX_SHOCK_DEBUG=1: a CHARGED box whose body took no seed -- where the
  // body sits in its box and what the grid holds under its lowest cell.
  static const bool shockDebug = std::getenv("SANDVOX_SHOCK_DEBUG") != nullptr;
  if (shockDebug) {
    for (uint32_t pi = 0; pi < parts.size(); pi++) {
      if (!parts[pi].charged) continue;
      const bool any = partSeeded[pi] != 0;
      int lowY = INT_MAX;
      IVec3 low{};
      for (uint32_t k = 0; k < nodes.size(); k++) {
        if (nodes[k].part != pi) continue;
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

  // ---- THE SOLVE: max-plus to the least fixpoint ---------------------------
  // Every edge is f(x) = x - cost - (x * spreadQ >> 12) with cost >= 1: it
  // strictly falls and never decreases in x. So the answer is the LEAST
  // FIXPOINT above the seeds -- for every node, the best over every path from
  // a seed -- and ANY order of relaxations that keeps going until nothing
  // improves reaches it (a widest-path label-correcting search). That is
  // what lets the schedule be chosen for speed: the result is the global
  // binary-heap search's, bit for bit (SANDVOX_SHOCK_DIGEST prints a hash of
  // every node's P to show it; SANDVOX_SHOCK_SOLVE=0 is that search, serial
  // over every node through the hash, the reference arm).
  uint64_t pushes = 0, pops = 0;
  if (solveMode == 0) {
    using QE = std::pair<int32_t, uint32_t>;
    std::priority_queue<QE> q;
    for (uint32_t k = 0; k < nodes.size(); k++)
      if (P[k] > 0) {
        q.push({P[k], k});
        pushes++;
      }
    while (!q.empty()) {
      const QE t = q.top();
      q.pop();
      pops++;
      if (t.first != P[t.second]) continue;
      const int32_t pk = t.first;
      forEdges(t.second, [&](uint32_t j, uint16_t cost) {
        const int32_t cand =
            pk - (int32_t)cost - (int32_t)(((uint64_t)(uint32_t)pk * spreadQ[j]) >> 12);
        if (cand > P[j]) {
          P[j] = cand;
          q.push({cand, j});
          pushes++;
        }
      });
    }
  } else {
    // A BUCKET QUEUE PER COMPONENT, the components across the pool. Every
    // edge strictly lowers P, so buckets are visited from the highest seed
    // down, each once, and a pop is O(1) instead of a heap's log n.
    std::vector<uint64_t> cPush(comps.size(), 0), cPop(comps.size(), 0);
    par(comps.size(), [&](size_t ci) {
      static thread_local std::vector<std::vector<uint32_t>> bucket;
      int32_t top = 0;
      for (uint32_t pi : comps[ci])
        for (uint32_t k = partBegin[pi]; k < partBegin[pi + 1]; k++) top = std::max(top, P[k]);
      if (top <= 0) return;
      if (bucket.size() < (size_t)top + 1) bucket.resize((size_t)top + 1);
      uint64_t pu = 0, po = 0;
      for (uint32_t pi : comps[ci])
        for (uint32_t k = partBegin[pi]; k < partBegin[pi + 1]; k++)
          if (P[k] > 0) {
            bucket[(size_t)P[k]].push_back(k);
            pu++;
          }
      int32_t* Pp = P.data();
      const uint16_t* sq = spreadQ.data();
      for (int32_t b = top; b > 0; b--) {
        std::vector<uint32_t>& B = bucket[(size_t)b];
        // A relax pushes only into lower buckets, so B does not grow here.
        for (size_t i = 0; i < B.size(); i++) {
          const uint32_t k = B[i];
          po++;
          if (Pp[k] != b) continue;
          const uint32_t pi = nodes[k].part;
          const Adj& A = adjPool[pi];
          const uint32_t lk = k - partBegin[pi];
          for (uint32_t x = A.begin[lk]; x < A.begin[lk + 1]; x++) {
            const uint32_t j = A.e[x].first;
            const int32_t cand =
                b - (int32_t)A.e[x].second - (int32_t)(((uint64_t)(uint32_t)b * sq[j]) >> 12);
            if (cand > Pp[j]) {
              Pp[j] = cand;
              bucket[(size_t)cand].push_back(j);
              pu++;
            }
          }
        }
        B.clear();
      }
      cPush[ci] = pu;
      cPop[ci] = po;
    });
    for (size_t ci = 0; ci < comps.size(); ci++) {
      pushes += cPush[ci];
      pops += cPop[ci];
    }
  }
  shockCounters_.solvePushes += pushes;
  shockCounters_.solvePops += pops;
  lap(4);

  // ---- WHAT IT DOES: the materials' answer ----------------------------------
  auto q4 = [](float gain) {
    return (uint32_t)std::clamp(std::lround((double)gain * 2.0 * 16.0), 0l, 1l << 16);
  };
  const uint32_t igniteQ = tn.sim.elecMode != 0 ? q4(tn.sim.elecIgniteGain) : 0u;
  const uint32_t crackleQ = tn.sim.elecMode != 0 ? q4(tn.sim.elecCrackle) : 0u;
  const uint32_t thrLo = ElecCrackleThreshold(tn.sim.elecCrackleSparkP, crackleLoMat_, crackleLoSrc_);
  const uint32_t thrHi = ElecCrackleThreshold(tn.sim.elecCrackleArcP, crackleHiMat_, crackleHiSrc_);
  uint32_t ohmicLeft = kShockOhmicCellsPerTick, crackleLeft = kShockCrackleOpsPerTick;

  // ---- THE ROLLS, DECIDED PER PART ACROSS THE POOL (round 3 K) -------------
  // Every ohmic and crackle roll is an integer hash of (creature, slot, cell,
  // tick) against the cell's P and its air faces; none reads a budget. So the
  // rolls -- the six air lookups, the hashes, the crackle's face -- are taken
  // here per part, and the serial pass below spends the two shared budgets on
  // them in exactly the (part, slot, cell) order the inline loop did. A roll
  // records only what that pass needs: the ohmic hit and its air face, and
  // the crackle's material and first open in-window face (or that it rolled
  // and found none, which spends and refuses nothing).
  struct Roll {
    int32_t li = 0, ci = 0;
    bool ohm = false, ohmAir = false;
    uint32_t crackleMat = 0;   // 0 = no crackle op
    IVec3 crackleAt{};
  };
  std::vector<std::vector<Roll>> rolls(parts.size());
  par(parts.size(), [&](size_t piz) {
    const uint32_t pi = (uint32_t)piz;
    const Part& p = parts[pi];
    const Mob& mob = *p.m;
    const uint32_t mobKey = (uint32_t)mob.id_ ^ (uint32_t)(mob.id_ >> 32);
    std::vector<Roll>& out = rolls[pi];
    for (int li = 0; li < (int)mob.limbs_.size() && li < (int)p.slotBase.size(); li++) {
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
        if (!ohmicCan && !crackleCan) continue;
        uint32_t air = 0;
        for (const IVec3& d : kFace6)
          air += airFor(pi, {nd.w.x + d.x, nd.w.y + d.y, nd.w.z + d.z}) ? 1u : 0u;
        const uint32_t cellKey = rng::Hash3(mobKey ^ (uint32_t)li * 0x9E3779B9u,
                                            (uint32_t)(uint16_t)e.c[0] | ((uint32_t)(uint16_t)e.c[1] << 16),
                                            (uint32_t)(uint16_t)e.c[2]);
        Roll r;
        r.li = li;
        r.ci = (int32_t)ci;
        // OHMIC: E = P x the cell's resist (the wet rule in it).
        const ElecMat& em = e.ohmMat < matElec_.size() ? matElec_[e.ohmMat] : ElecMat{};
        const uint32_t prod = air != 0 ? em.igniteInto : em.charInto;
        if (e.ohmMat != 0 && prod != 0 && em.igniteCap != 0 && igniteQ != 0 && e.bulk != kIns) {
          const uint64_t E = (uint64_t)pk * e.bulk;
          const uint64_t lim = ((uint64_t)em.igniteCap << 4) / igniteQ;
          uint64_t ch = E < lim ? (E * igniteQ) >> 4 : em.igniteCap;
          ch = ch * (air + 2u) / 8u;
          if (ch != 0 && rng::Hash3(cellKey ^ kOhmSalt, tick, 0x0A11u) % kReactChanceDen < ch) {
            r.ohm = true;
            r.ohmAir = air != 0;
          }
        }
        // CRACKLE: over a tier's threshold, with an air face.
        if (air != 0 && crackleQ != 0) {
          uint32_t cm = 0, thr = 0;
          if ((uint32_t)pk >= thrHi) {
            cm = crackleHiMat_;
            thr = thrHi;
          } else if ((uint32_t)pk >= thrLo) {
            cm = crackleLoMat_;
            thr = thrLo;
          }
          if (cm != 0) {
            const uint32_t ch = std::min<uint32_t>(
                (uint32_t)(((uint64_t)((uint32_t)pk - thr) * crackleQ) >> 4), kElecCrackleCap);
            const uint32_t rr = rng::Hash3(cellKey ^ kCrackleSalt, tick, 0xC4ACu);
            if (ch != 0 && rr % kReactChanceDen < ch) {
              const uint32_t rot = rr >> 12;
              for (uint32_t f = 0; f < 6; f++) {
                const IVec3& d = kFace6[(f + rot) % 6u];
                const IVec3 n{nd.w.x + d.x, nd.w.y + d.y, nd.w.z + d.z};
                if (!airFor(pi, n) || !world.CellInWindow(n)) continue;
                r.crackleMat = cm;
                r.crackleAt = n;
                break;
              }
            }
          }
        }
        if (r.ohm || r.crackleMat != 0) out.push_back(r);
      }
    }
  });
  lap(5);

  // SANDVOX_SHOCK_DIGEST=1: one line per solved tick digesting every node's P
  // and every decided roll, in node / part order -- the probe that shows a
  // change to this pass's SCHEDULE left its answer alone (diagnostic only).
  static const bool shockDigest = std::getenv("SANDVOX_SHOCK_DIGEST") != nullptr;
  if (shockDigest) {
    uint64_t hP = 1469598103934665603ull, hR = hP;
    auto mix = [](uint64_t& h, uint64_t x) {
      h ^= x;
      h *= 1099511628211ull;
    };
    for (uint32_t k = 0; k < nodes.size(); k++) mix(hP, (uint64_t)(uint32_t)P[k] | ((uint64_t)k << 32));
    for (uint32_t pi = 0; pi < parts.size(); pi++)
      for (const Roll& r : rolls[pi]) {
        mix(hR, ((uint64_t)pi << 40) | ((uint64_t)(uint32_t)r.li << 20) | (uint32_t)r.ci);
        mix(hR, (uint64_t)r.ohm | ((uint64_t)r.ohmAir << 1) | ((uint64_t)r.crackleMat << 2));
        mix(hR, CellKey(r.crackleAt));
      }
    size_t big = 0;
    for (const auto& C : comps) big = std::max(big, C.size());
    std::printf("shock-digest tick %u parts %zu nodes %zu comps %zu (largest %zu) P %016llx rolls %016llx\n",
                tick, parts.size(), nodes.size(), comps.size(), big, (unsigned long long)hP,
                (unsigned long long)hR);
    // ...and, cumulative over the run, where the pass's time went (us per
    // solved tick) and the solve's queue traffic. ("effects" is the previous
    // tick's, and includes this printing.)
    const ShockCounters& sc = shockCounters_;
    const double calls = (double)std::max<uint64_t>(1, sc.applyCalls + 1);
    std::printf("shock-phases us/tick: parts %.0f caches+nodes %.0f index %.0f comps %.0f seeds+edges %.0f "
                "solve %.0f rolls %.0f effects %.0f | pushes %.0f pops %.0f\n",
                sc.phaseNanos[7] / 1e3 / calls, sc.phaseNanos[0] / 1e3 / calls,
                sc.phaseNanos[2] / 1e3 / calls, sc.phaseNanos[1] / 1e3 / calls,
                sc.phaseNanos[3] / 1e3 / calls, sc.phaseNanos[4] / 1e3 / calls,
                sc.phaseNanos[5] / 1e3 / calls, sc.phaseNanos[6] / 1e3 / calls,
                sc.solvePushes / calls, sc.solvePops / calls);
    const double pc = (double)std::max<uint64_t>(1, g_parStats.calls);
    std::printf("shock-pool: %.1f calls/tick, per call: wall %.0f us, first task at %.0f us, "
                "longest task %.0f us, task sum %.0f us\n",
                g_parStats.calls / calls, g_parStats.wall / pc, g_parStats.first / pc,
                g_parStats.maxTask / pc, g_parStats.sumTask / pc);
  }

  for (uint32_t pi = 0; pi < parts.size(); pi++) {
    Part& p = parts[pi];
    Mob& mob = *p.m;
    if (!mob.alive_) continue;
    Mob::ShockRecord& rec = mob.shock_;

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
    // item included: a charged iron cuirass crackles, a wooden arm chars. The
    // rolls were decided above; the shared budgets are spent here, in order.
    std::vector<std::vector<std::pair<int32_t, bool>>> ohm(mob.limbs_.size());   // (cell, air)
    for (const Roll& r : rolls[pi]) {
      if (r.li >= (int)ohm.size()) continue;
      if (r.ohm) {
        if (ohmicLeft > 0) {
          ohmicLeft--;
          ohm[r.li].push_back({r.ci, r.ohmAir});
        } else {
          shockCounters_.ohmicRefused++;
        }
      }
      if (r.crackleMat == 0) continue;
      if (crackleLeft == 0 || cellOps.size() >= kMaxCellOpsPerTick) {
        shockCounters_.crackleRefused++;
        continue;
      }
      crackleLeft--;
      cellOps.push_back({World::SlotCellIndex(r.crackleAt), PackVoxNew(r.crackleMat, 0u) | kCellOpIfAir});
      shockCounters_.crackleOps++;
      rec.crackles++;
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
  lap(6);
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
    // A power-of-two scale (every authored one) floors by an arithmetic shift
    // -- the same answer as FloorDiv, negative coordinates included -- rather
    // than a division per axis per voxel, twice (round 3 K).
    int sh = -1;
    if ((s & (s - 1)) == 0) {
      sh = 0;
      while ((1u << sh) != s) sh++;
    }
    auto fdiv = [&](int a) { return sh >= 0 ? (a >> sh) : FloorDiv(a, si); };
    c.cmin = {fdiv(mn.x), fdiv(mn.y), fdiv(mn.z)};
    const IVec3 cmax{fdiv(mx.x), fdiv(mx.y), fdiv(mx.z)};
    c.cdim = {cmax.x - c.cmin.x + 1, cmax.y - c.cmin.y + 1, cmax.z - c.cmin.z + 1};
    c.at.assign((size_t)c.cdim.x * c.cdim.y * c.cdim.z, -1);
    // Cells in (z, y, x) order: the first voxel to land in a cell does not
    // decide its index, so the cell list is a pure function of the geometry.
    for (size_t i = 0; i < n; i++) {
      if (v.Mat(i) == 0) continue;
      const IVec3 a = v.At(i);
      const size_t k = ((size_t)(fdiv(a.z) - c.cmin.z) * c.cdim.y +
                        (fdiv(a.y) - c.cmin.y)) * c.cdim.x + (fdiv(a.x) - c.cmin.x);
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
      const size_t k = ((size_t)(fdiv(a.z) - c.cmin.z) * c.cdim.y +
                        (fdiv(a.y) - c.cmin.y)) * c.cdim.x + (fdiv(a.x) - c.cmin.x);
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
  // ...and each MATERIAL's own conductance and "is a conducting coat", once a
  // call rather than a 64-bit division per voxel (round 3 K: the division was
  // most of a re-accumulation). Thread-local: refreshes run on the pool.
  static thread_local std::vector<uint32_t> matG;
  static thread_local std::vector<uint8_t> coatConducts;
  matG.resize(nm);
  coatConducts.resize(nm);
  for (size_t m = 0; m < nm; m++) {
    matG[m] = Conductance(matElecResist_[m] ? matElecResist_[m] : kIns);
    coatConducts[m] = matElecResist_[m] != 0 ? 1 : 0;
  }
  const uint32_t* matGp = matG.data();
  const uint8_t* coatCp = coatConducts.data();
  auto accumulate = [&]() -> uint64_t {
    acc.assign(c.cells.size(), Acc{});
    uint64_t h = GeomMix(GeomMix(1469598103934665603ull, n), s);
    // THE CELL'S SUMS IN REGISTERS WHILE THE CELL LASTS (round 3 K). Voxels
    // come in lattice order, so a run of them lands in one cell; adding each
    // into the cell's record in memory made every voxel wait on the last one's
    // store. The run is summed locally and folded in when the cell changes:
    // the same integer sums, and the ohmic materials folded in the order they
    // first appeared -- which is the order the per-voxel loop met them, so a
    // cell's four slots fill (and refuse a fifth) exactly as they did.
    struct Loc {
      int32_t ci = -1;
      uint32_t n = 0, ns = 0, feel = 0;
      uint64_t g = 0, gs = 0;
      int nom = 0;
      uint16_t om[8];
      uint32_t oc[8];
    } L;
    auto fold = [&]() {
      if (L.ci < 0) return;
      Acc& e = acc[(size_t)L.ci];
      e.n += L.n;
      e.ns += L.ns;
      e.feel += L.feel;
      e.g += L.g;
      e.gs += L.gs;
      for (int t = 0; t < L.nom; t++)
        for (int k = 0; k < 4; k++)
          if (e.om[k] == L.om[t] || e.om[k] == 0) {
            e.om[k] = L.om[t];
            e.oc[k] += L.oc[t];
            break;
          }
      L.n = L.ns = L.feel = 0;
      L.g = L.gs = 0;
      L.nom = 0;
    };
    const size_t nme = matElec_.size();
    const ElecMat* mel = matElec_.data();
    const int32_t* vc = c.vcell.data();
    const uint8_t* ve = c.vexp.data();
    auto run = [&](const auto* vx, auto matOf) {
      for (size_t i = 0; i < n; i++) {
        const auto& q = vx[i];
        const uint32_t m = matOf(q) & 0xFFFu;
        h = GeomMix(h, ((uint64_t)((uint32_t)(int32_t)q.x & 0xFFFFu)) |
                           ((uint64_t)((uint32_t)(int32_t)q.y & 0xFFFFu) << 16) |
                           ((uint64_t)((uint32_t)(int32_t)q.z & 0xFFFFu) << 32) |
                           ((uint64_t)(m != 0) << 48));
        const int32_t ci = vc[i];
        if (m == 0 || ci < 0) continue;
        if (ci != L.ci) {
          fold();
          L.ci = ci;
        }
        uint32_t gv = m < nm ? matGp[m] : 0u;
        const uint16_t st = q.stain;
        const uint32_t amt = BodyStainAmt(st);
        if (amt != 0) {
          const uint32_t cm = BodyStainMat(st);
          if (cm != 0 && cm < nm && coatCp[cm]) gv = std::max(gv, wetG[std::min(amt, 15u)]);
        }
        L.n++;
        L.g += gv;
        if (ve[i]) {
          L.ns++;
          L.gs += gv;
        }
        if (m < nme) {
          const ElecMat& em = mel[m];
          L.feel += em.feel;
          if (em.igniteInto != 0 || em.charInto != 0) {
            int t = 0;
            while (t < L.nom && L.om[t] != m) t++;
            if (t == L.nom) {
              if (L.nom == 8) {
                // More distinct ohmic materials in one run than the local list
                // holds: fold what came first, in order, and go on.
                fold();
                t = 0;
              }
              L.om[t] = (uint16_t)m;
              L.oc[t] = 0;
              L.nom = t + 1;
            }
            L.oc[t]++;
          }
        }
      }
      fold();
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
