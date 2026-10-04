// selftest_elec.cpp — gate for THE CHARGE FIELD (docs/PLAN_electricity.md
// section 1, package E1; src/sim/elec.h, assets/shaders/sim_elec.wgsl).
//
//   elec-field   three SEALED STONE ROOMS, each fed by a spark re-laid every
//                tick of the hold in a stone pocket (so it can neither drift
//                nor touch anything flammable):
//                  A  a straight drawn-copper wire `elec.wireCells` long on the
//                     floor, crossing ~10 chunks, the spark at one end: the far
//                     end must charge within ceil((chunk hops + 1) / rounds) +
//                     `elec.arrivalSlackTicks` ticks of the near end; P must
//                     fall along the
//                     wire; the air above it and the stone under it stay 0.
//                  B  two wood strips behind a one-cell copper electrode, one
//                     dry and one under a full water coat: the wet strip must
//                     carry charge further than the dry one, and the dry one
//                     at most `elec.dryReachMax` cells (since wave 2 dry wood
//                     is 1,500 against the spark's 200: none at all).
//                  C  a 32 x 32 water pool two cells deep, the electrode in one
//                     corner: at least `elec.waterChargedMin` water cells
//                     charged, and the far corner (Manhattan 61) never (since
//                     wave 2 the spreading loss holds a spark to ~a dozen
//                     cells of a pool).
//                Then the sparks stop: every page must be freed within
//                `elec.fadeTicksMax` ticks, and after `elec.settleTicks` more
//                at most `elec.awakeMax` chunks may be awake. No refusal, no
//                purge. Run TWICE from a fresh worldgen: the arrival tick and a
//                slot-keyed hash of the field at the end of the hold must
//                agree (the field is not in the world hash, so this is where
//                its determinism is asserted).
//
// P is read back by a test-only readback (ReadbackBlocking after WaitIdle),
// never on the frame path. Ticked on THE tick (support::TickCursor).

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "gpu/resources.h"
#include "sim/elec.h"
#include "sim/worldgen_run.h"
#include "sim/tuning.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

struct Box {
  int x0, y0, z0, x1, y1, z1;  // inclusive
  bool Has(int x, int y, int z) const {
    return x >= x0 && x <= x1 && y >= y0 && y <= y1 && z >= z0 && z <= z1;
  }
};

uint32_t MatId(const Ctx& c, const char* name) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == name) return (uint32_t)i;
  return 0;
}

constexpr uint32_t kFull = 7u << 12;   // a full liquid cell's state nibble

void Put(std::vector<CellOp>& ops, int x, int y, int z, uint32_t word) {
  ops.push_back({World::SlotCellIndex({x, y, z}), word});
}
void Fill(std::vector<CellOp>& ops, const Box& b, uint32_t word) {
  for (int z = b.z0; z <= b.z1; z++)
    for (int y = b.y0; y <= b.y1; y++)
      for (int x = b.x0; x <= b.x1; x++) Put(ops, x, y, z, word);
}
// A sealed room: stone shell one cell thick round `in`, air inside.
void Room(std::vector<CellOp>& ops, const Box& in) {
  for (int z = in.z0 - 1; z <= in.z1 + 1; z++)
    for (int y = in.y0 - 1; y <= in.y1 + 1; y++)
      for (int x = in.x0 - 1; x <= in.x1 + 1; x++)
        Put(ops, x, y, z, in.Has(x, y, z) ? 0u : (uint32_t)kMatStone);
}
// A spark pocket: stone on ALL 26 neighbours of the spark cell but the `open`
// face (the conductor it feeds). Faces alone are not enough: a gas moves
// DIAGONALLY, so a spark walled in on five faces rose out past the corner of
// the open one into the air over the wire and charged nothing (measured: every
// seed the field saw had air beside it).
void Pocket(std::vector<CellOp>& ops, int x, int y, int z, int openDx, int openDy, int openDz) {
  for (int dz = -1; dz <= 1; dz++)
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        if ((dx == 0 && dy == 0 && dz == 0) || (dx == openDx && dy == openDy && dz == openDz))
          continue;
        Put(ops, x + dx, y + dy, z + dz, kMatStone);
      }
}

// The whole field: elecMeta and the pool, read after the queue drains.
struct ElecView {
  std::vector<uint32_t> meta, pool;
  void Read(GpuContext& ctx, World& world) {
    ctx.WaitIdle();
    meta.assign(kEmWords, 0);
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.elecMeta, 0, meta.data(),
                          (uint64_t)kEmWords * 4, "elecMetaRead");
    pool.assign((size_t)kElecPoolPages * kElecPageWords, 0);
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.elecPool, 0, pool.data(),
                          pool.size() * 4, "elecPoolRead");
  }
  uint32_t PagesInUse() const { return meta[kEmNextFresh] - meta[kEmFreeTop]; }
  // P at world cell (x, y, z): half 0 of its chunk's page, 0 if unpaged.
  uint32_t P(int x, int y, int z) const {
    const uint32_t e = meta[kEmEntry + World::SlotChunkIndex({x >> 4, y >> 4, z >> 4})];
    if ((e & kElecEntryHas) == 0) return 0;
    const uint32_t local = (((uint32_t)z & 15) * 16 + ((uint32_t)y & 15)) * 16 + ((uint32_t)x & 15);
    const uint32_t w = pool[(size_t)(e & kElecEntryPage) * kElecPageWords + (local >> 1)];
    return (w >> ((local & 1) * 16)) & 0xFFFFu;
  }
  // The field's CONTENT keyed by SLOT (never by page index, which depends on
  // free-stack order and is unobservable by design).
  uint64_t Hash() const {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t s = 0; s < kNumChunks; s++) {
      const uint32_t e = meta[kEmEntry + s];
      if ((e & kElecEntryHas) == 0) continue;
      const uint32_t* p = pool.data() + (size_t)(e & kElecEntryPage) * kElecPageWords;
      uint32_t any = 0;
      for (uint32_t i = 0; i < kElecHalfWords; i++) any |= p[i];
      if (any == 0) continue;   // a page handed out at the tail, still empty
      h = (h ^ s) * 1099511628211ull;
      for (uint32_t i = 0; i < kElecHalfWords; i++) h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
  }
};

// The header words (counters, stats): one small readback.
std::vector<uint32_t> ElecHeader(GpuContext& ctx, World& world) {
  ctx.WaitIdle();
  std::vector<uint32_t> h(kEmSnapWords, 0);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.elecMeta, 0, h.data(), kEmSnapWords * 4,
                        "elecHeader");
  return h;
}
// P at one world cell: two 4-byte readbacks (the entry, then the pool word).
uint32_t ProbeP(GpuContext& ctx, World& world, int x, int y, int z) {
  ctx.WaitIdle();
  const uint32_t slot = World::SlotChunkIndex({x >> 4, y >> 4, z >> 4});
  uint32_t e = 0;
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.elecMeta, (uint64_t)(kEmEntry + slot) * 4,
                        &e, 4, "elecProbeEntry");
  if ((e & kElecEntryHas) == 0) return 0;
  const uint32_t local = (((uint32_t)z & 15) * 16 + ((uint32_t)y & 15)) * 16 + ((uint32_t)x & 15);
  uint32_t w = 0;
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.elecPool,
                        ((uint64_t)(e & kElecEntryPage) * kElecPageWords + (local >> 1)) * 4, &w,
                        4, "elecProbeWord");
  return (w >> ((local & 1) * 16)) & 0xFFFFu;
}

uint32_t AwakeAll(GpuContext& ctx, Simulation& sim) {
  ctx.WaitIdle();
  std::vector<uint32_t> flags(kNumSlots, 0);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0, flags.data(),
                        kNumSlots * 4, "elecAwakeAll");
  uint32_t n = 0;
  for (uint32_t f : flags) n += f != 0;
  return n;
}

void Regenerate(Ctx& c) {
  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

// ---- the fixture ------------------------------------------------------------
// Room A: the wire along x at y = kWy, z = kWz, from kWx0; the spark at
// kWx0 - 1. Room B: two wood strips along x at y = kBy (z = kBzDry / kBzWet)
// from x = kBx0, a copper electrode at kBx0 - 1, the spark at kBx0 - 2. Room
// C: the pool, water at y kCy and kCy + 1 over the whole floor; a copper
// electrode standing on it at (kCx0 + 1, kCy + 2, kCz0 + 1), the spark on top.
constexpr int kWy = 120, kWz = 101, kWx0 = 40;
constexpr int kBy = 136, kBzDry = 101, kBzWet = 105, kBx0 = 42, kBLen = 16;
constexpr int kCx0 = 100, kCz0 = 100, kCy = 136, kCN = 32;

struct FieldResult {
  int started = -1;              // hold tick the near end first read P > 0
  int arrival = -1;              // ticks from then until the far end read P > 0 (1 = same tick)
  uint32_t nearP = 0, farP = 0;  // at the end of the hold
  int nonMonotone = 0;           // wire cells holding MORE than the one before
  uint32_t airMax = 0, floorMax = 0;
  int dryReach = 0, wetReach = 0;
  uint32_t wetStain = 0;         // the wet strip's first cell's stain amount then
  uint32_t waterCharged = 0, waterCells = 0, cornerP = 0;
  uint32_t pagesHold = 0;
  int fadeTick = -1;
  uint32_t awake = 0;
  std::vector<uint32_t> hdr;     // the header at the end
  uint64_t hash = 0;
  std::string profile;
  std::string diag;              // the feeds: what stands there, what it holds
};

FieldResult RunField(Ctx& c, int wireCells, int hold, int fadeMax, int settle) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  const uint32_t copper = MatId(c, "copper_bar"), wood = MatId(c, "wood"),
                 water = MatId(c, "water"), spark = MatId(c, "spark");
  const uint32_t wetSlot = c.mats[water].stainSlot;
  Regenerate(c);
  FieldResult r;
  const int wx1 = kWx0 + wireCells - 1;
  const Box roomA{kWx0 - 4, kWy, kWz - 1, wx1 + 4, kWy + 2, kWz + 1};
  const Box roomB{kBx0 - 6, kBy, kBzDry - 1, kBx0 + kBLen + 4, kBy + 2, kBzWet + 1};
  const Box roomC{kCx0, kCy, kCz0, kCx0 + kCN - 1, kCy + 4, kCz0 + kCN - 1};
  std::vector<CellOp> rooms, build, feed;
  Room(rooms, roomA);
  Room(rooms, roomB);
  Room(rooms, roomC);
  // A: the wire, its spark pocket (open toward +x, the wire's first cell).
  for (int x = kWx0; x <= wx1; x++) Put(build, x, kWy, kWz, copper);
  Pocket(build, kWx0 - 1, kWy, kWz, 1, 0, 0);
  Put(feed, kWx0 - 1, kWy, kWz, spark);
  // B: per strip, spark -> one copper cell -> 16 wood cells.
  for (int z : {kBzDry, kBzWet}) {
    const uint32_t w = z == kBzWet ? (wood | PackStain(wetSlot, kStainAmtMax)) : wood;
    for (int x = kBx0; x < kBx0 + kBLen; x++) Put(build, x, kBy, z, w);
    Put(build, kBx0 - 1, kBy, z, copper);
    Pocket(build, kBx0 - 2, kBy, z, 1, 0, 0);
    Put(feed, kBx0 - 2, kBy, z, spark);
  }
  // C: the pool, the electrode on it, the spark on the electrode.
  Fill(build, {kCx0, kCy, kCz0, kCx0 + kCN - 1, kCy + 1, kCz0 + kCN - 1}, water | kFull);
  const int ex = kCx0 + 1, ez = kCz0 + 1;
  Put(build, ex, kCy + 2, ez, copper);
  Pocket(build, ex, kCy + 3, ez, 0, -1, 0);
  Put(feed, ex, kCy + 3, ez, spark);

  uint32_t t = 61000;
  support::TickCursor ticker{c, t, {(kWx0 + wireCells / 2) >> 4, kWy >> 4, kWz >> 4}};
  // Rooms in one tick, their contents in the next (two cell ops on one cell
  // in one tick: the first wins, so a room's air would eat its contents).
  ticker({}, rooms);
  ticker({}, build);
  for (int i = 0; i < 4; i++) ticker();
  // THE HOLD: the sparks re-laid every tick (one decays in a tick or two).
  // The arrival is counted from the first tick the wire's NEAR end holds
  // charge: a spark decays at 450 per-mille a tick, so the one laid on a
  // given tick may already be gone when the field reads the voxels -- which
  // delays the start, never the travel.
  for (int i = 0; i < hold; i++) {
    ticker({}, feed);
    if (r.started < 0 && ProbeP(ctx, world, kWx0, kWy, kWz) > 0) r.started = i + 1;
    if (r.started > 0 && r.arrival < 0 && ProbeP(ctx, world, wx1, kWy, kWz) > 0)
      r.arrival = i + 2 - r.started;
  }
  ElecView v;
  v.Read(ctx, world);
  r.pagesHold = v.PagesInUse();
  r.hash = v.Hash();
  r.nearP = v.P(kWx0, kWy, kWz);
  r.farP = v.P(wx1, kWy, kWz);
  uint32_t prev = 0xFFFFFFFFu;
  for (int x = kWx0; x <= wx1; x++) {
    const uint32_t p = v.P(x, kWy, kWz);
    if (p > prev) r.nonMonotone++;
    prev = p;
    r.airMax = std::max(r.airMax, v.P(x, kWy + 1, kWz));
    r.airMax = std::max(r.airMax, v.P(x, kWy, kWz - 1));
    r.floorMax = std::max(r.floorMax, v.P(x, kWy - 1, kWz));
    if ((x - kWx0) % 32 == 0 || x == wx1) r.profile += Format(" %d:%u", x - kWx0, p);
  }
  auto reach = [&](int z) {
    int n = 0;
    while (n < kBLen && v.P(kBx0 + n, kBy, z) > 0) n++;
    return n;
  };
  r.dryReach = reach(kBzDry);
  r.wetReach = reach(kBzWet);
  // ATTRIBUTION: what stands at each feed (the spark cell, the cell it feeds,
  // the one after) and the P each holds, so a dead fixture names its cause.
  {
    auto cellWord = [&](int x, int y, int z) {
      std::vector<uint32_t> vox(kChunkVol);
      ReadVoxelsSync(ctx, world, World::SlotChunkIndex({x >> 4, y >> 4, z >> 4}), 1, vox.data(),
                     "elecDiagVox");
      return vox[(((uint32_t)z & 15) * 16 + ((uint32_t)y & 15)) * 16 + ((uint32_t)x & 15)];
    };
    auto name = [&](uint32_t w) {
      const uint32_t m = w & 0xFFFu;
      return m < c.mats.size() ? c.mats[m].name : std::string("?");
    };
    auto cellNote = [&](int x, int y, int z) {
      const uint32_t w = cellWord(x, y, z);
      return Format(" %s(r%u s%u)=%u", name(w).c_str(),
                    (w & 0xFFFu) < c.mats.size() ? c.mats[w & 0xFFFu].elec.resist : 0u,
                    (w & 0xFFFu) < c.mats.size() ? c.mats[w & 0xFFFu].elec.source : 0u,
                    v.P(x, y, z));
    };
    r.diag = "wire:" + cellNote(kWx0 - 1, kWy, kWz) + cellNote(kWx0, kWy, kWz) +
             cellNote(kWx0 + 1, kWy, kWz) + "; dry strip:" + cellNote(kBx0 - 2, kBy, kBzDry) +
             cellNote(kBx0 - 1, kBy, kBzDry) + cellNote(kBx0, kBy, kBzDry) +
             "; pool:" + cellNote(ex, kCy + 3, ez) + cellNote(ex, kCy + 2, ez) +
             cellNote(ex, kCy + 1, ez);
  }
  {
    std::vector<uint32_t> vox(kChunkVol);
    ReadVoxelsSync(ctx, world, World::SlotChunkIndex({kBx0 >> 4, kBy >> 4, kBzWet >> 4}), 1,
                   vox.data(), "elecWetVox");
    const uint32_t li = (((uint32_t)kBzWet & 15) * 16 + ((uint32_t)kBy & 15)) * 16 +
                        ((uint32_t)kBx0 & 15);
    r.wetStain = VoxStainAmt(vox[li]);
  }
  for (int z = kCz0; z < kCz0 + kCN; z++)
    for (int y = kCy; y <= kCy + 1; y++)
      for (int x = kCx0; x < kCx0 + kCN; x++) {
        r.waterCells++;
        r.waterCharged += v.P(x, y, z) > 0;
      }
  r.cornerP = std::max(v.P(kCx0 + kCN - 1, kCy, kCz0 + kCN - 1),
                       v.P(kCx0 + kCN - 1, kCy + 1, kCz0 + kCN - 1));
  // THE FADE: no more sparks; every page must come back.
  for (int i = 0; i < fadeMax; i++) {
    ticker();
    const std::vector<uint32_t> h = ElecHeader(ctx, world);
    if (h[kEmNextFresh] - h[kEmFreeTop] == 0) {
      r.fadeTick = i + 1;
      break;
    }
  }
  for (int i = 0; i < settle; i++) ticker();
  r.awake = AwakeAll(ctx, c.sim);
  r.hdr = ElecHeader(ctx, world);
  ElecNoteRun(r.hdr.data());
  return r;
}

Status GateElecField(Ctx& c, std::string& detail) {
  const int wireCells = (int)BaselineNumber("elec.wireCells", 160);
  const int hold = (int)BaselineNumber("elec.holdTicks", 12);
  const int slack = (int)BaselineNumber("elec.arrivalSlackTicks", 2);
  const int fadeMax = (int)BaselineNumber("elec.fadeTicksMax", 80);
  const int settle = (int)BaselineNumber("elec.settleTicks", 90);
  const int dryMax = (int)BaselineNumber("elec.dryReachMax", 4);
  const uint32_t waterMin = (uint32_t)BaselineNumber("elec.waterChargedMin", 300);
  const uint32_t awakeMax = (uint32_t)BaselineNumber("elec.awakeMax", 32);
  for (const char* m : {"copper_bar", "wood", "water", "spark"}) {
    if (MatId(c, m) == 0) {
      detail = Format("missing material %s", m);
      std::printf("elec-field: FAIL (%s)\n", detail.c_str());
      return Status::Fail;
    }
  }
  // The FIELD's claims, not E2's: a charged dry strip may catch (ohmic
  // ignition, elec-ignite's subject; P ~131 there is ~1.7 per-mille a tick),
  // and an ember conducts nothing, so it would cut the reach being measured.
  // Ignition is off for this gate; nothing else here is charged past E2's
  // crackle or reaction thresholds' reach (fresh water, P <= 200).
  struct NoIgnite {
    Tuning saved;
    NoIgnite() : saved(CurrentTuning()) {
      Tuning t = saved;
      t.sim.elecIgniteGain = 0.0f;
      SetCurrentTuning(t);
    }
    ~NoIgnite() { SetCurrentTuning(saved); }
  } noIgnite;
  const int rounds = std::clamp(CurrentTuning().sim.elecRounds, 1, (int)kElecRoundsMax);
  // The near end's chunk charges in round 0 of the start tick and the charge
  // crosses one chunk per round after it: `hops` chunk faces to the far end.
  const int hops = ((kWx0 + wireCells - 1) >> 4) - (kWx0 >> 4);
  const int bound = (hops + 1 + rounds - 1) / rounds + slack;
  const FieldResult a = RunField(c, wireCells, hold, fadeMax, settle);
  const FieldResult b = RunField(c, wireCells, hold, fadeMax, settle);
  Regenerate(c);
  RecordObserved("elec.arrivalTicksObserved", (double)a.arrival);
  RecordObserved("elec.farPObserved", (double)a.farP);
  RecordObserved("elec.waterChargedObserved", (double)a.waterCharged);
  RecordObserved("elec.fadeTicksObserved", (double)a.fadeTick);
  RecordObserved("elec.pagesHoldObserved", (double)a.pagesHold);

  const bool reached = a.arrival > 0 && a.arrival <= bound && a.farP > 0;
  const bool falls = a.nonMonotone == 0 && a.nearP > a.farP;
  const bool confined = a.airMax == 0 && a.floorMax == 0;
  const bool wet = a.dryReach <= dryMax && a.wetReach > a.dryReach;
  const bool pool = a.waterCharged >= waterMin && a.cornerP == 0;
  const bool freed = a.fadeTick > 0;
  const bool idle = a.awake <= awakeMax;
  const uint32_t refused = a.hdr[kEmRefused] + b.hdr[kEmRefused];
  const uint32_t purges = a.hdr[kEmPurges] + b.hdr[kEmPurges];
  const bool bounded = refused == 0 && purges == 0;
  const bool twice = a.hash == b.hash && a.started == b.started && a.arrival == b.arrival &&
                     a.fadeTick == b.fadeTick;
  const bool ok = reached && falls && confined && wet && pool && freed && idle && bounded && twice;
  detail = Format(
      "copper wire %d cells (%d chunk hops), %d rounds: near end charged at hold tick %d, "
      "far end %d tick(s) later counting that one (bound %d) %s, P near "
      "%u far %u, profile%s, %d rises %s; air beside / stone under the wire max %u / %u %s; "
      "wood strips: dry reach %d (max %d), wet reach %d (stain %u) %s; water pool: %u of %u "
      "cells charged (min %u), far corner %u %s; %u pages at the end of the hold, all freed "
      "%d ticks after the sparks %s; %u chunks awake %d ticks later (max %u) %s; refused %u, "
      "purged %u, stranded %u, P peak %u, live peak %u, pages peak %u %s; run twice: field "
      "hash %016llx vs %016llx, arrival %d vs %d, fade %d vs %d %s",
      wireCells, hops, rounds, a.started, a.arrival, bound, reached ? "OK" : "FAIL", a.nearP,
      a.farP,
      a.profile.c_str(), a.nonMonotone, falls ? "OK" : "FAIL", a.airMax, a.floorMax,
      confined ? "OK" : "LEAKED", a.dryReach, dryMax, a.wetReach, a.wetStain,
      wet ? "OK" : "FAIL", a.waterCharged, a.waterCells, waterMin, a.cornerP,
      pool ? "OK" : "FAIL", a.pagesHold, a.fadeTick, freed ? "OK" : "PAGES LEFT", a.awake,
      settle, awakeMax, idle ? "OK" : "FAIL", refused, purges, a.hdr[kEmStranded],
      a.hdr[kEmPPeak], a.hdr[kEmLivePeak], a.hdr[kEmPagesPeak], bounded ? "OK" : "FAIL",
      (unsigned long long)a.hash, (unsigned long long)b.hash, a.arrival, b.arrival, a.fadeTick,
      b.fadeTick, twice ? "OK" : "NONDETERMINISTIC");
  if (!ok)
    detail += "; feeds at the end of the hold: " + a.diag +
              Format("; conductor P peak %u, chunk-rounds by round 0/1/2/3+: %u/%u/%u/%u, "
                     "allocs %u, frees %u",
                     a.hdr[kEmCondPeak], a.hdr[kEmRoundHist], a.hdr[kEmRoundHist + 1],
                     a.hdr[kEmRoundHist + 2], a.hdr[kEmRoundHist + 3], a.hdr[kEmAllocs],
                     a.hdr[kEmFrees]);
  std::printf("elec-field: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ============================================================================
// PACKAGE E2: WHAT CHARGE DOES IN THE CA (sim_step.wgsl; DESIGN.md
// "Electricity -- charge field", "What charge does").
//
//   elec-electrolysis  three molten-salt pools whose surfaces sit at each
//                      residue of y mod 3, and a brine and a fresh water pool,
//                      each fed through ONE copper electrode in its floor by a
//                      spark sealed in a stone pocket under it -- so no spark
//                      ever touches the liquid and the only way it can react
//                      is THE CHARGE AS PARTNER. Every molten pool must make
//                      sodium and chlorine; the brine chlorine plus lye or
//                      hydrogen; the fresh water nothing.
//   elec-ignite        lightning (held in a pocket) down a 40-cell copper bar:
//                      the wood block at its far end must catch within
//                      `elecIgnite.lightningTicksMax`, a wood cell BURIED under
//                      the bar (no air face) must char to charcoal, and the
//                      copper must be all still there. 24 spark -> copper ->
//                      wood units held `elecIgnite.sparkHoldTicks`: at most
//                      `elecIgnite.sparkIgnitedMax` of their wood cells may
//                      catch (a spark lights wood rarely). Run twice: the
//                      census hash of both rooms must agree.
//   elec-crackle-bounded  lightning down a copper bar in a sealed room (the
//                      game's own decay since the endgame's proportional decay
//                      fades 30,000 in ~55 ticks; `elecCrackle.decay` > 0 still
//                      overrides the floor -- the bound does not depend on it): the bar
//                      must crackle arcs while fed, and after the source stops
//                      every page must be freed, no crackle cell may remain
//                      and nothing may crackle after the field is gone, and
//                      the world must sleep (<= `elecCrackle.awakeMax`).

// Material census over an inclusive box (sentinel chunks synthesized by
// ReadVoxelsSync). `hash` folds every word, stamp and excite scratch masked.
std::vector<uint32_t> Census(Ctx& c, const Box& b, uint64_t* hash = nullptr) {
  std::vector<uint32_t> h(c.mats.size() + 1, 0);
  std::vector<uint32_t> buf(kChunkVol);
  uint64_t fnv = 1469598103934665603ull;
  for (int cz = b.z0 >> 4; cz <= (b.z1 >> 4); cz++)
    for (int cy = b.y0 >> 4; cy <= (b.y1 >> 4); cy++)
      for (int cx = b.x0 >> 4; cx <= (b.x1 >> 4); cx++) {
        ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({cx, cy, cz}), 1, buf.data(),
                       "elecCensus");
        for (uint32_t k = 0; k < kChunkVol; k++) {
          const int x = (int)(k % 16) + cx * 16, y = (int)((k / 16) % 16) + cy * 16,
                    z = (int)(k / 256) + cz * 16;
          if (!b.Has(x, y, z)) continue;
          const uint32_t m = buf[k] & 0xFFFu;
          h[m < c.mats.size() ? m : c.mats.size()]++;
          fnv = (fnv ^ (buf[k] & ~0x00FF0000u)) * 1099511628211ull;
        }
      }
  if (hash) *hash = fnv;
  return h;
}

// Day frozen at a fixed phase (no light-gated rule changes mid-gate) and the
// MPM seam off (it would carry a brine cell's mass away), as the solute gates.
struct ElecFixtureTuning {
  Tuning saved;
  explicit ElecFixtureTuning(int decay = -1) : saved(CurrentTuning()) {
    Tuning t = saved;
    t.dayNight.freeze = 1;
    t.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
    t.sim.fluidExciteMode = 0;
    if (decay > 0) t.sim.elecDecay = decay;
    SetCurrentTuning(t);
  }
  ~ElecFixtureTuning() { SetCurrentTuning(saved); }
};

// A liquid pool fed from BELOW: interior `in` (the pool fills its bottom
// `depth` layers), one copper electrode in the floor under the pool's middle,
// and a spark pocket under the electrode (open up into it). Room shell in
// `rooms`, contents in `build`, the feed cell returned.
struct FedPool {
  Box in;
  int ex, ey, ez;  // the electrode
};
FedPool BuildFedPool(std::vector<CellOp>& rooms, std::vector<CellOp>& build, const Box& in,
                     int depth, uint32_t liquidWord, uint32_t copper) {
  Room(rooms, in);
  Fill(build, {in.x0, in.y0, in.z0, in.x1, in.y0 + depth - 1, in.z1}, liquidWord);
  FedPool p{in, (in.x0 + in.x1) / 2, in.y0 - 1, (in.z0 + in.z1) / 2};
  Put(build, p.ex, p.ey, p.ez, copper);
  Pocket(build, p.ex, p.ey - 1, p.ez, 0, 1, 0);
  return p;
}

Status GateElecElectrolysis(Ctx& c, std::string& detail) {
  const uint32_t copper = MatId(c, "copper_bar"), molten = MatId(c, "molten_salt"),
                 water = MatId(c, "water"), salt = MatId(c, "salt"), spark = MatId(c, "spark"),
                 sodium = MatId(c, "sodium"), chlorine = MatId(c, "chlorine"),
                 lye = MatId(c, "lye"), hydrogen = MatId(c, "hydrogen");
  for (uint32_t m : {copper, molten, water, salt, spark, sodium, chlorine, lye, hydrogen})
    if (m == 0) {
      detail = "missing one of copper_bar/molten_salt/water/salt/spark/sodium/chlorine/lye/hydrogen";
      std::printf("elec-electrolysis: FAIL (%s)\n", detail.c_str());
      return Status::Fail;
    }
  const int moltenHold = (int)BaselineNumber("elecElectrolysis.moltenHoldTicks", 40);
  const int dissolve = (int)BaselineNumber("elecElectrolysis.dissolveTicks", 300);
  const int brineHold = (int)BaselineNumber("elecElectrolysis.brineHoldTicks", 40);
  ElecFixtureTuning tune;
  Regenerate(c);
  // Molten pools: floor (pool surface, one layer deep) at y = 120 + k, so the
  // surfaces cover every residue of y mod 3 (120 = 0 mod 3).
  constexpr int kYb = 120, kZm = 100;
  std::vector<CellOp> rooms, build, grains, feedMolten, feedWater;
  FedPool mp[3];
  for (int k = 0; k < 3; k++) {
    const int x0 = 40 + k * 12;
    mp[k] = BuildFedPool(rooms, build, {x0, kYb + k, kZm, x0 + 6, kYb + k + 4, kZm + 6}, 1,
                         molten | kFull, copper);
    Put(feedMolten, mp[k].ex, mp[k].ey - 1, mp[k].ez, spark);
  }
  // Water pools, two layers: brine (salt grains dropped in, dissolved for
  // `dissolve` ticks before the sparks) and fresh (the control).
  constexpr int kYw = 121;
  const FedPool brine = BuildFedPool(rooms, build, {100, kYw, kZm, 105, kYw + 8, kZm + 5}, 2,
                                     water | kFull, copper);
  const FedPool fresh = BuildFedPool(rooms, build, {112, kYw, kZm, 117, kYw + 8, kZm + 5}, 2,
                                     water | kFull, copper);
  for (const FedPool* p : {&brine, &fresh})
    Put(feedWater, p->ex, p->ey - 1, p->ez, spark);
  // 16 grains into 72 water cells: ~56 units a cell, over the rules' cMin 24
  // (the solute-electrolysis gate's dose).
  for (int z = brine.in.z0; z <= brine.in.z1; z++)
    for (int x = brine.in.x0; x <= brine.in.x1; x++)
      if (grains.size() < 16 && ((x + z) % 2 == 0 || grains.size() < 4))
        Put(grains, x, kYw + 3, z, salt);

  uint32_t t = 62000;
  support::TickCursor ticker{c, t, {80 >> 4, kYb >> 4, kZm >> 4}};
  ticker({}, rooms);
  ticker({}, build);
  ticker({}, grains);
  auto grow = [](const Box& b) { return Box{b.x0, b.y0, b.z0, b.x1, b.y1, b.z1}; };
  uint32_t naM[3] = {}, clM[3] = {}, moltenLeft[3] = {};
  uint32_t lyeB = 0, clB = 0, h2B = 0, lyeF = 0, clF = 0, h2F = 0;
  const int total = std::max(moltenHold + 10, dissolve + brineHold + 10);
  for (int i = 0; i < total; i++) {
    std::vector<CellOp> feed;
    if (i < moltenHold) feed = feedMolten;
    if (i >= dissolve && i < dissolve + brineHold)
      feed.insert(feed.end(), feedWater.begin(), feedWater.end());
    ticker({}, feed);
    if (i < moltenHold + 10 && (i % 2) == 1)
      for (int k = 0; k < 3; k++) {
        const std::vector<uint32_t> h = Census(c, grow(mp[k].in));
        naM[k] = std::max(naM[k], h[sodium]);
        clM[k] = std::max(clM[k], h[chlorine]);
        moltenLeft[k] = h[molten];
      }
    if (i >= dissolve && i < dissolve + brineHold + 10 && (i % 2) == 1) {
      const std::vector<uint32_t> hb = Census(c, grow(brine.in)), hf = Census(c, grow(fresh.in));
      lyeB = std::max(lyeB, hb[lye]);
      clB = std::max(clB, hb[chlorine]);
      h2B = std::max(h2B, hb[hydrogen]);
      lyeF = std::max(lyeF, hf[lye]);
      clF = std::max(clF, hf[chlorine]);
      h2F = std::max(h2F, hf[hydrogen]);
    }
  }
  const std::vector<uint32_t> hdr = ElecHeader(c.ctx, c.world);
  ElecNoteRun(hdr.data());
  Regenerate(c);
  bool molOk = true;
  std::string pools;
  for (int k = 0; k < 3; k++) {
    const bool split = naM[k] > 0 && clM[k] > 0;
    molOk = molOk && split;
    pools += Format("%s y %d (y%%3 %d): sodium %u, chlorine %u, molten left %u %s", k ? " |" : "",
                    mp[k].in.y0, ((mp[k].in.y0 % 3) + 3) % 3, naM[k], clM[k], moltenLeft[k],
                    split ? "split" : "NO ELECTROLYSIS");
  }
  const bool brineOk = clB > 0 && (lyeB > 0 || h2B > 0);
  const bool freshOk = lyeF == 0 && clF == 0 && h2F == 0;
  RecordObserved("elecElectrolysis.sodiumPeak0", (double)naM[0]);
  RecordObserved("elecElectrolysis.brineChlorinePeak", (double)clB);
  const bool ok = molOk && brineOk && freshOk;
  detail = Format(
      "molten salt fed through a buried copper electrode (no spark touches it), %d ticks:%s "
      "%s; brine (16 grains, %d ticks dissolving) fed %d ticks: lye %u, chlorine %u, hydrogen "
      "%u %s; fresh water: lye %u, chlorine %u, hydrogen %u %s; field P peak %u, refused %u, "
      "purges %u",
      moltenHold, pools.c_str(), molOk ? "OK" : "FAIL", dissolve, brineHold, lyeB, clB, h2B,
      brineOk ? "OK" : "FAIL", lyeF, clF, h2F, freshOk ? "OK" : "REACTED", hdr[kEmPPeak],
      hdr[kEmRefused], hdr[kEmPurges]);
  std::printf("elec-electrolysis: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- elec-ignite --------------------------------------------------------------
struct IgniteRun {
  int lightningTick = -1;   // hold tick the far-end wood block first lost a cell
  int charTick = -1;        // hold tick the buried wood became charcoal
  uint32_t buriedEnd = 0;   // its material at the end
  uint32_t copper0 = 0, copperEnd = 0;
  uint32_t sparkIgnited = 0, sparkUnits = 0;
  uint64_t hash = 0;
};

IgniteRun RunIgnite(Ctx& c, int lightHold, int sparkHold, int sparkUnits) {
  const uint32_t copper = MatId(c, "copper_bar"), wood = MatId(c, "wood"),
                 spark = MatId(c, "spark"), lightning = MatId(c, "lightning");
  Regenerate(c);
  IgniteRun r;
  r.sparkUnits = (uint32_t)sparkUnits;
  // ROOM L: lightning pocket at x 41, the bar x 42..81 on the floor (y 130,
  // z 111), a 2x2x3 wood block against its far end, one wood cell BURIED in
  // the floor under bar cell x 60 (stone round it and under it).
  constexpr int kLy = 130, kLz = 111, kBar0 = 42, kBarN = 40, kBuriedX = 60;
  const Box roomL{38, kLy, kLz - 2, kBar0 + kBarN + 5, kLy + 4, kLz + 2};
  const Box block{kBar0 + kBarN, kLy, kLz - 1, kBar0 + kBarN + 1, kLy + 1, kLz + 1};
  // ROOM S: units at x = 40 + 3i, z 121: spark pocket (y 138) -> copper in the
  // floor (y 139) -> one wood cell on the floor (y 140) in open air.
  constexpr int kSy = 140, kSz = 121;
  const Box roomS{37, kSy, kSz - 2, 40 + 3 * (sparkUnits - 1) + 3, kSy + 3, kSz + 2};
  std::vector<CellOp> rooms, build, feed;
  Room(rooms, roomL);
  Room(rooms, roomS);
  for (int x = kBar0; x < kBar0 + kBarN; x++) Put(build, x, kLy, kLz, copper);
  Pocket(build, kBar0 - 1, kLy, kLz, 1, 0, 0);
  Put(feed, kBar0 - 1, kLy, kLz, lightning);
  Fill(build, block, wood);
  for (int dz = -1; dz <= 1; dz++)
    for (int dx = -1; dx <= 1; dx++) Put(build, kBuriedX + dx, kLy - 2, kLz + dz, kMatStone);
  Put(build, kBuriedX, kLy - 1, kLz, wood);
  for (int i = 0; i < sparkUnits; i++) {
    const int x = 40 + 3 * i;
    Pocket(build, x, kSy - 2, kSz, 0, 1, 0);
    Put(build, x, kSy - 1, kSz, copper);
    Put(build, x, kSy, kSz, wood);
    Put(feed, x, kSy - 2, kSz, spark);
  }
  auto woodAt = [&](int x, int y, int z) {
    std::vector<uint32_t> vox(kChunkVol);
    ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({x >> 4, y >> 4, z >> 4}), 1,
                   vox.data(), "elecIgniteVox");
    return vox[(((uint32_t)z & 15) * 16 + ((uint32_t)y & 15)) * 16 + ((uint32_t)x & 15)] & 0xFFFu;
  };
  const Box barBox{kBar0, kLy, kLz, kBar0 + kBarN - 1, kLy, kLz};
  uint32_t t = 63000;
  support::TickCursor ticker{c, t, {64 >> 4, kLy >> 4, kLz >> 4}};
  ticker({}, rooms);
  ticker({}, build);
  r.copper0 = Census(c, barBox)[copper];
  const int hold = std::max(lightHold, sparkHold);
  for (int i = 0; i < hold; i++) {
    std::vector<CellOp> ops;
    for (const CellOp& o : feed) {
      const bool isLight = (o.word & 0xFFFu) == lightning;
      if ((isLight && i < lightHold) || (!isLight && i < sparkHold)) ops.push_back(o);
    }
    ticker({}, ops);
    if (r.lightningTick < 0 && Census(c, block)[wood] < (uint32_t)(2 * 2 * 3)) r.lightningTick = i + 1;
    if (r.charTick < 0 && woodAt(kBuriedX, kLy - 1, kLz) != wood) r.charTick = i + 1;
  }
  // The spark units' charge fades (~140 / decay ticks) before they are counted.
  for (int i = 0; i < 30; i++) ticker();
  r.buriedEnd = woodAt(kBuriedX, kLy - 1, kLz);
  r.copperEnd = Census(c, barBox)[copper];
  for (int i = 0; i < sparkUnits; i++) r.sparkIgnited += woodAt(40 + 3 * i, kSy, kSz) != wood;
  uint64_t hl = 0, hs = 0;
  Census(c, roomL, &hl);
  Census(c, roomS, &hs);
  r.hash = hl ^ (hs * 31u);
  return r;
}

Status GateElecIgnite(Ctx& c, std::string& detail) {
  for (const char* m : {"copper_bar", "wood", "spark", "lightning", "charcoal", "ember"})
    if (MatId(c, m) == 0) {
      detail = Format("missing material %s", m);
      std::printf("elec-ignite: FAIL (%s)\n", detail.c_str());
      return Status::Fail;
    }
  const int lightHold = (int)BaselineNumber("elecIgnite.lightningHoldTicks", 40);
  const int lightMax = (int)BaselineNumber("elecIgnite.lightningTicksMax", 20);
  const int charMax = (int)BaselineNumber("elecIgnite.charTicksMax", 40);
  const int sparkHold = (int)BaselineNumber("elecIgnite.sparkHoldTicks", 30);
  const int units = (int)BaselineNumber("elecIgnite.sparkUnits", 24);
  const uint32_t sparkMax = (uint32_t)BaselineNumber("elecIgnite.sparkIgnitedMax", 8);
  ElecFixtureTuning tune;
  const IgniteRun a = RunIgnite(c, lightHold, sparkHold, units);
  const IgniteRun b = RunIgnite(c, lightHold, sparkHold, units);
  Regenerate(c);
  const uint32_t charcoal = MatId(c, "charcoal");
  RecordObserved("elecIgnite.lightningTickObserved", (double)a.lightningTick);
  RecordObserved("elecIgnite.charTickObserved", (double)a.charTick);
  RecordObserved("elecIgnite.sparkIgnitedObserved", (double)a.sparkIgnited);
  const bool lit = a.lightningTick > 0 && a.lightningTick <= lightMax;
  const bool charred = a.charTick > 0 && a.charTick <= charMax && a.buriedEnd == charcoal;
  const bool copperOk = a.copper0 == 40 && a.copperEnd == a.copper0;
  const bool sparkOk = a.sparkIgnited <= sparkMax;
  const bool twice = a.hash == b.hash && a.lightningTick == b.lightningTick &&
                     a.charTick == b.charTick && a.sparkIgnited == b.sparkIgnited;
  const bool ok = lit && charred && copperOk && sparkOk && twice;
  const std::string buried =
      a.buriedEnd < c.mats.size() ? c.mats[a.buriedEnd].name : std::string("?");
  detail = Format(
      "lightning down 40 cells of copper: the far-end wood block caught at tick %d (max %d) "
      "%s; the buried wood (no air face) changed at tick %d (max %d) and ended %s %s; copper "
      "%u -> %u %s; %u spark->copper->wood units held %d ticks: %u caught (max %u) %s; run "
      "twice: census %016llx vs %016llx, ticks %d/%d vs %d/%d, spark %u vs %u %s",
      a.lightningTick, lightMax, lit ? "OK" : "FAIL", a.charTick, charMax, buried.c_str(),
      charred ? "OK" : "FAIL", a.copper0, a.copperEnd, copperOk ? "OK" : "FAIL", a.sparkUnits,
      sparkHold, a.sparkIgnited, sparkMax, sparkOk ? "OK" : "TOO MANY",
      (unsigned long long)a.hash, (unsigned long long)b.hash, a.lightningTick, a.charTick,
      b.lightningTick, b.charTick, a.sparkIgnited, b.sparkIgnited,
      twice ? "OK" : "NONDETERMINISTIC");
  std::printf("elec-ignite: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- elec-crackle-bounded -----------------------------------------------------
Status GateElecCrackleBounded(Ctx& c, std::string& detail) {
  const uint32_t copper = MatId(c, "copper_bar"), spark = MatId(c, "spark"),
                 arc = MatId(c, "arc"), lightning = MatId(c, "lightning");
  if (!copper || !spark || !arc || !lightning) {
    detail = "missing one of copper_bar/spark/arc/lightning";
    std::printf("elec-crackle-bounded: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  // 0 = the game's own decay (floor + proportional); > 0 overrides the floor.
  const int decay = (int)BaselineNumber("elecCrackle.decay", 0);
  const int hold = (int)BaselineNumber("elecCrackle.holdTicks", 15);
  const int fadeMax = (int)BaselineNumber("elecCrackle.fadeTicksMax", 260);
  const int settle = (int)BaselineNumber("elecCrackle.settleTicks", 90);
  const uint32_t awakeMax = (uint32_t)BaselineNumber("elecCrackle.awakeMax", 32);
  ElecFixtureTuning tune(decay);
  const int decayFloor = CurrentTuning().sim.elecDecay;
  const int decayShift = CurrentTuning().sim.elecDecayShift;
  Regenerate(c);
  // A sealed room, a 32-cell bar on its floor, the lightning pocket at x 39.
  constexpr int kY = 150, kZ = 130, kX0 = 40, kN = 32;
  const Box room{36, kY, kZ - 3, kX0 + kN + 3, kY + 5, kZ + 3};
  std::vector<CellOp> rooms, build, feed;
  Room(rooms, room);
  for (int x = kX0; x < kX0 + kN; x++) Put(build, x, kY, kZ, copper);
  Pocket(build, kX0 - 1, kY, kZ, 1, 0, 0);
  Put(feed, kX0 - 1, kY, kZ, lightning);
  // The air of the room: every crackle lands here (the pocket is outside it).
  const Box air{kX0, kY, kZ - 3, room.x1, room.y1, room.z1};
  uint32_t t = 64000;
  support::TickCursor ticker{c, t, {kX0 >> 4, kY >> 4, kZ >> 4}};
  ticker({}, rooms);
  ticker({}, build);
  uint32_t arcsHold = 0, sparksHold = 0;
  for (int i = 0; i < hold; i++) {
    ticker({}, feed);
    const std::vector<uint32_t> h = Census(c, air);
    arcsHold += h[arc];
    sparksHold += h[spark];
  }
  // THE FADE: no source. Every crackle cell seen, the last tick one was, and
  // the tick every page is back.
  int fadeTick = -1, lastCrackle = 0;
  uint32_t arcsFade = 0, sparksFade = 0;
  for (int i = 0; i < fadeMax; i++) {
    ticker();
    const std::vector<uint32_t> h = Census(c, air);
    if (h[arc] + h[spark] != 0) lastCrackle = i + 1;
    arcsFade += h[arc];
    sparksFade += h[spark];
    const std::vector<uint32_t> eh = ElecHeader(c.ctx, c.world);
    if (eh[kEmNextFresh] - eh[kEmFreeTop] == 0) {
      fadeTick = i + 1;
      break;
    }
  }
  // After the field: nothing may crackle again, and the world sleeps.
  uint32_t lateCrackle = 0;
  for (int i = 0; i < settle; i++) {
    ticker();
    if (i < 20) {
      const std::vector<uint32_t> h = Census(c, air);
      lateCrackle += h[arc] + h[spark];
    }
  }
  const uint32_t awake = AwakeAll(c.ctx, c.sim);
  const std::vector<uint32_t> hdr = ElecHeader(c.ctx, c.world);
  const uint32_t pagesEnd = hdr[kEmNextFresh] - hdr[kEmFreeTop];
  ElecNoteRun(hdr.data());
  Regenerate(c);
  RecordObserved("elecCrackle.fadeTicksObserved", (double)fadeTick);
  RecordObserved("elecCrackle.arcsHoldObserved", (double)arcsHold);
  const bool crackled = arcsHold > 0;
  const bool faded = fadeTick > 0 && pagesEnd == 0;
  const bool quiet = lateCrackle == 0;
  const bool idle = awake <= awakeMax;
  const bool ok = crackled && faded && quiet && idle;
  detail = Format(
      "lightning down a %d-cell copper bar for %d ticks (sim.elecDecay %d, shift %d): crackle seen %u "
      "arc / %u spark cell-ticks while fed %s; source gone: %u arc / %u spark cell-ticks, the "
      "last at fade tick %d, every page freed at tick %d (max %d) %s; %u crackle cell-ticks in "
      "the 20 after %s; %u chunks awake %d ticks later (max %u) %s; pages %u, P peak %u, "
      "purges %u",
      kN, hold, decayFloor, decayShift, arcsHold, sparksHold, crackled ? "OK" : "NO CRACKLE", arcsFade,
      sparksFade, lastCrackle, fadeTick, fadeMax, faded ? "OK" : "NEVER FADED", lateCrackle,
      quiet ? "OK" : "SELF-SUSTAINING", awake, settle, awakeMax, idle ? "OK" : "FAIL",
      pagesEnd, hdr[kEmPPeak], hdr[kEmPurges]);
  std::printf("elec-crackle-bounded: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& ElecGates() {
  static const std::vector<Gate> g = {
      {"elec-field", "sim", {}, false, GateElecField},
      {"elec-electrolysis", "sim", {}, false, GateElecElectrolysis},
      {"elec-ignite", "sim", {}, false, GateElecIgnite},
      {"elec-crackle-bounded", "sim", {}, false, GateElecCrackleBounded},
  };
  return g;
}

}  // namespace selftest
