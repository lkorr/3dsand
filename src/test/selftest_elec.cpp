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
//                     at most `elec.dryReachMax` cells (resist 60 vs 200).
//                  C  a 32 x 32 water pool two cells deep, the electrode in one
//                     corner: at least `elec.waterChargedMin` water cells
//                     charged, and the far corner (Manhattan 61) never.
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
  const bool wet = a.dryReach >= 1 && a.dryReach <= dryMax && a.wetReach > a.dryReach;
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

}  // namespace

const std::vector<Gate>& ElecGates() {
  static const std::vector<Gate> g = {
      {"elec-field", "sim", {}, false, GateElecField},
  };
  return g;
}

}  // namespace selftest
