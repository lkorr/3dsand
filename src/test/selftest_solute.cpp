// selftest_solute.cpp — the solute layer's gates (docs/PLAN_solutes.md §6,
// package B of docs/PLAN_alchemy_chemistry.md).
//
// Dissolved matter is MASS per liquid cell in a sparse aux layer (world.h
// kSol*, sim_solute.wgsl, sim_step.wgsl's advection). These gates assert the
// three properties the plan names and that fail DIFFERENTLY, plus the phase
// change and the MPM seam:
//
//   solute        a pond held ABOVE the dilution floor: mass is conserved
//                 exactly, the box goes back to sleep, and it settles UNIFORM
//                 (a diffusion that stalls in stripes fails the spread line).
//   solute-dilute the other half, and the one that catches "every lake is
//                 permanently pink after a battle": a pinch in a big body is
//                 below the floor, and after N ticks it is GONE -- no mass, no
//                 page, no awake chunk.
//   solute-evap   brine boiled off a lava floor concentrates and PRECIPITATES
//                 salt, and the salt that went in balances across the phase
//                 change against what came out (powder + mass + the counted
//                 sub-eighth rounding).
//
// Every gate asserts the two bug latches too: no solute store refused for want
// of a page (solFaults) and no pool exhaustion.
//
// THRESHOLDS LIVE IN tests/baseline.json (CLAUDE.md), read through
// BaselineNumber with the value measured when the gate landed as the fallback.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sim/solutes.h"
#include "sim/tuning.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

uint32_t MatNamed(const Ctx& c, const char* name) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == name) return (uint32_t)i;
  return 0;
}

// The whole layer, read back (blocking; selftest only). 32 MiB of pool, which
// is fine for a gate that reads it a handful of times.
struct SoluteLayer {
  std::vector<uint32_t> table;
  std::vector<uint32_t> pool;
  std::vector<uint32_t> hdr;
  uint32_t Value(uint32_t slot, uint32_t local) const {
    const uint32_t e = table[slot];
    if (e == 0u) return 0u;
    if (e & kSolUniformBit) return e & 0xFFFFu;
    const uint32_t w = pool[(size_t)(e & kSolPageMask) * kSolWordsPerPage + (local >> 1)];
    return (w >> ((local & 1u) * 16u)) & 0xFFFFu;
  }
};

void ReadSoluteLayer(Ctx& c, SoluteLayer& L) {
  c.ctx.WaitIdle();
  L.table.assign(kNumSlots, 0u);
  L.pool.assign((size_t)kSolutePoolPages * kSolWordsPerPage, 0u);
  L.hdr.assign(kSolMetaHdrWords, 0u);
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.solTable, 0, L.table.data(),
                        L.table.size() * 4, "solTable");
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.solPool, 0, L.pool.data(),
                        L.pool.size() * 4, "solPool");
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.solMeta, 0, L.hdr.data(),
                        L.hdr.size() * 4, "solMeta");
}

// A sealed (or open-topped) stone box: interior [x0,x1] x (floorY, roofY) x
// [z0,z1], walls one cell thick. The interior is cleared to air.
struct Box {
  int x0, x1, z0, z1, floorY, roofY;
  bool roof = true;
  std::vector<CellOp> Build(uint32_t stone) const {
    std::vector<CellOp> v;
    for (int z = z0 - 1; z <= z1 + 1; z++)
      for (int x = x0 - 1; x <= x1 + 1; x++)
        for (int y = floorY; y <= roofY; y++) {
          const bool wall = x < x0 || x > x1 || z < z0 || z > z1;
          const bool solid = wall || y <= floorY || (roof && y >= roofY);
          v.push_back({World::SlotCellIndex({x, y, z}), solid ? stone : 0u});
        }
    return v;
  }
  std::vector<CellOp> Layers(uint32_t mat, int y0, int n, uint32_t state) const {
    std::vector<CellOp> v;
    for (int y = y0; y < y0 + n; y++)
      for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++)
          v.push_back({World::SlotCellIndex({x, y, z}), (mat & 0xFFFu) | (state << 12)});
    return v;
  }
  std::vector<uint32_t> Chunks() const {
    std::vector<uint32_t> v;
    for (int cz = (z0 - 1) >> 4; cz <= ((z1 + 1) >> 4); cz++)
      for (int cy = floorY >> 4; cy <= (roofY >> 4); cy++)
        for (int cx = (x0 - 1) >> 4; cx <= ((x1 + 1) >> 4); cx++)
          v.push_back(World::SlotChunkIndex({cx, cy, cz}));
    return v;
  }
  bool Inside(int x, int y, int z) const {
    return x >= x0 && x <= x1 && z >= z0 && z <= z1 && y > floorY &&
           (y < roofY || !roof);
  }
};

// Chunks of `box` awake NEXT tick (the dirty set the CA will run).
uint32_t AwakeIn(Ctx& c, const Box& box) {
  c.ctx.WaitIdle();
  std::vector<uint32_t> flags(kNumSlots, 0);
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.DirtyActive(), 0,
                        flags.data(), kNumSlots * 4, "soluteAwake");
  uint32_t n = 0;
  for (uint32_t ci : box.Chunks())
    if (flags[ci] != 0) n++;
  return n;
}

// What the box holds: dissolved mass (by species), the powder of `powderMat`
// (in eighths), and the spread of concentration over FULL solvent cells.
struct BoxCensus {
  uint64_t mass = 0;
  uint64_t powderEighths = 0;
  uint32_t powderCells = 0;
  uint32_t fullCells = 0;
  uint32_t cMin = 0xFFFFFFFFu, cMax = 0;
  uint32_t solventCells = 0;
  uint32_t pageSlots = 0, uniformSlots = 0, emptySlots = 0;
  uint32_t strayMass = 0;   // mass on a cell that is not a liquid (must be 0)
};

uint32_t PowderEighths(uint32_t word, bool hasMass) {
  if (!hasMass) return 8u;
  const uint32_t s = (word >> 12) & 0xFu;
  // common.wgsl powderMassOfState: a partial grain is state e + 2 (e 1..7 -> 3..9);
  // anything else is a full cell.
  if (s >= 3u && s <= 9u) return s - 2u;
  return 8u;
}

BoxCensus Census(Ctx& c, const Box& box, const SoluteLayer& L, uint32_t powderMat) {
  BoxCensus r;
  const bool powderHasMass =
      powderMat < c.mats.size() && c.mats[powderMat].gpu.klass == CLASS_POWDER &&
      (c.mats[powderMat].gpu.flags & kMatFlagWander) == 0;
  std::vector<uint32_t> cbuf((size_t)kChunkVol);
  for (uint32_t slot : box.Chunks()) {
    const uint32_t e = L.table[slot];
    if (e == 0u) r.emptySlots++;
    else if (e & kSolUniformBit) r.uniformSlots++;
    else r.pageSlots++;
    ReadVoxelsSync(c.ctx, c.world, slot, 1, cbuf.data(), "soluteVox");
    const IVec3 wc = c.world.SlotToWorldChunk(slot);
    for (uint32_t k = 0; k < kChunkVol; k++) {
      const int x = wc.x * 16 + (int)(k % 16), y = wc.y * 16 + (int)((k / 16) % 16),
                z = wc.z * 16 + (int)(k / 256);
      const uint32_t w = cbuf[k];
      const uint32_t m = w & 0xFFFu;
      const uint32_t v = L.Value(slot, k);
      const bool liquid = m != 0 && m < c.mats.size() && c.mats[m].gpu.klass == CLASS_LIQUID;
      if (v != 0u && !liquid) r.strayMass += v & 0xFFu;
      if (!box.Inside(x, y, z)) {
        // Mass outside the interior (in the walls' slots) still counts.
        r.mass += v & 0xFFu;
        continue;
      }
      r.mass += v & 0xFFu;
      if (m == powderMat && powderMat != 0) {
        r.powderCells++;
        r.powderEighths += PowderEighths(w, powderHasMass);
      }
      if (liquid) {
        r.solventCells++;
        const uint32_t f = ((w >> 12) & 0x7u) + 1u;
        if (f == 8u) {
          r.fullCells++;
          const uint32_t conc = (v & 0xFFu);  // full cell: mass IS concentration
          r.cMin = std::min(r.cMin, conc);
          r.cMax = std::max(r.cMax, conc);
        }
      }
    }
  }
  if (r.fullCells == 0) r.cMin = 0;
  return r;
}

// The shared fixture tuning: a pinned dim dawn (evaporation and freezing are
// authored mass sinks, exactly ca-level's reason) and the CA alone (the MPM
// seam refuses solute cells, but it would also make plain-water mass audits
// inexact while particles are in flight).
struct FixtureTuning {
  Tuning saved;
  explicit FixtureTuning(int exciteMode = 0) : saved(CurrentTuning()) {
    Tuning t = saved;
    t.dayNight.freeze = 1;
    t.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
    t.sim.fluidExciteMode = exciteMode;
    SetCurrentTuning(t);
  }
  ~FixtureTuning() { SetCurrentTuning(saved); }
};

bool LatchesClean(const SoluteLayer& L, std::string& why) {
  const uint32_t f = L.hdr[kSolMFaults], x = L.hdr[kSolMExhausted];
  if (f == 0 && x == 0) return true;
  why = Format("solute latches: %u refused stores (first slot %u tick %u), %u "
               "exhausted pops",
               f, L.hdr[kSolMFaultSlot] ? L.hdr[kSolMFaultSlot] - 1u : 0u,
               L.hdr[kSolMFaultTick], x);
  return false;
}

// ---- solute ---------------------------------------------------------------
// A sealed pond ABOVE the floor. 16 salt voxels onto 576 cells of water is
// 4,096 units over 576 cells, ~7 units a cell at equilibrium against salt's
// dilution floor of 2 -- comfortably above it, which is the point: below the
// floor mass is discarded ON PURPOSE, and a conservation gate written without
// that scope would go red the first time the floor did its job
// (PLAN_solutes §6). solute-dilute is the other half.
Status GateSolute(Ctx& c, std::string& detail) {
  const uint32_t water = MatNamed(c, "water"), salt = MatNamed(c, "salt");
  const SoluteDef* sd = CurrentSoluteNamed("salt");
  if (!water || !salt || !sd) {
    detail = "needs materials water + salt and a `salt` species in solutes.json";
    return Status::Fail;
  }
  FixtureTuning tune;
  const int px = 96, py = 120, pz = 96;
  const Box box{px - 6, px + 5, pz - 6, pz + 5, py, py + 9, true};
  const int layers = 4;
  const int kTicks = (int)BaselineNumber("solute.ticks", 900);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const std::vector<CellOp> build = box.Build((uint32_t)kMatStone);
  const std::vector<CellOp> pond = box.Layers(water, py + 1, layers, 7u);
  std::vector<CellOp> grains;
  for (int z = pz - 2; z < pz + 2; z++)
    for (int x = px - 2; x < px + 2; x++)
      grains.push_back({World::SlotCellIndex({x, py + 1 + layers, z}), salt & 0xFFFu});
  const uint64_t placedUnits = (uint64_t)grains.size() * 8u * (sd->yieldPerVoxel / 8u);

  uint32_t t = 52000;
  support::TickCursor ticker{c, t, {px >> 4, py >> 4, pz >> 4}};
  int quietAt = -1;
  uint32_t awake = 0;
  for (int i = 0; i < kTicks; i++) {
    ticker({}, i == 0 ? build : i == 2 ? pond : i == 4 ? grains : std::vector<CellOp>{});
    if (i >= 60 && i % 30 == 0) {
      awake = AwakeIn(c, box);
      if (awake == 0 && quietAt < 0) quietAt = i;
      else if (awake != 0) quietAt = -1;
    }
  }
  awake = AwakeIn(c, box);
  SoluteLayer L;
  ReadSoluteLayer(c, L);
  const BoxCensus k = Census(c, box, L, salt);
  const uint64_t y8 = sd->yieldPerVoxel / 8u;
  const uint64_t accounted = k.mass + k.powderEighths * y8;

  std::string latch;
  const bool latchOk = LatchesClean(L, latch);
  const bool massOk = accounted == placedUnits && L.hdr[kSolMDiscarded] == 0;
  // The integer fixpoint is PER PAIR (|dm| <= 1); the four strides bound the
  // slope, so the whole-body spread is the hop diameter of the pond, not 1.
  const double maxSpread = BaselineNumber("solute.maxSpread", 4);
  const bool spreadOk = k.fullCells > 0 && (double)(k.cMax - k.cMin) <= maxSpread;
  const bool idleOk = awake == 0;
  const bool strayOk = k.strayMass == 0;
  // Every grain dissolved: the pond has 50x the capacity, so powder left over
  // means the dissolve never ran (a gate that passes on an empty layer proves
  // nothing -- the first run of this one did exactly that).
  const bool dissolvedOk = k.powderEighths == 0 && k.mass == placedUnits;
  RecordObserved("solute.quietAt", (double)quietAt);
  RecordObserved("solute.spread", (double)(k.cMax - k.cMin));

  detail = Format(
      "%zu salt voxels (%llu units) into %d layers of %d x %d water: mass %s "
      "(%llu dissolved + %llu units still powder in %u cells = %llu; ledger "
      "dissolved %u discarded %u), concentration over %u full cells %u..%u "
      "(spread %u, allow %.0f), %u of %zu box chunks awake at tick %d (quiet "
      "from %d), slots page %u / uniform %u / empty %u, stray %u%s%s",
      grains.size(), (unsigned long long)placedUnits, layers, box.x1 - box.x0 + 1,
      box.z1 - box.z0 + 1, massOk ? "EXACT" : "LEAK", (unsigned long long)k.mass,
      (unsigned long long)(k.powderEighths * y8), k.powderCells,
      (unsigned long long)accounted, L.hdr[kSolMDissolved], L.hdr[kSolMDiscarded],
      k.fullCells, k.cMin, k.cMax, k.cMax - k.cMin, maxSpread, awake,
      box.Chunks().size(), kTicks, quietAt, k.pageSlots, k.uniformSlots,
      k.emptySlots, k.strayMass, latchOk ? "" : ", ", latch.c_str());
  std::printf("solute: %s (%s)\n",
              massOk && spreadOk && idleOk && latchOk && strayOk && dissolvedOk ? "PASS" : "FAIL",
              detail.c_str());
  return massOk && spreadOk && idleOk && latchOk && strayOk && dissolvedOk ? Status::Pass
                                                                           : Status::Fail;
}

// ---- solute-dilute ----------------------------------------------------------
// ONE salt voxel into a 40 x 40 x 2 pool: 256 units over 3,200 cells is 0.08 a
// cell, far below salt's floor. Integer diffusion quantizes to a halt long
// before that, so without the floor the plume would freeze as a faint blotch
// forever; with it, the blotch decays and the chunk collapses back to
// SOL_EMPTY. Asserted: no mass anywhere in the box, no slot of it holding a
// sentinel or a page, the ledger says everything dissolved was discarded, and
// the box is asleep.
Status GateSoluteDilute(Ctx& c, std::string& detail) {
  const uint32_t water = MatNamed(c, "water"), salt = MatNamed(c, "salt");
  const SoluteDef* sd = CurrentSoluteNamed("salt");
  if (!water || !salt || !sd) {
    detail = "needs materials water + salt and a `salt` species in solutes.json";
    return Status::Fail;
  }
  FixtureTuning tune;
  const int px = 96, py = 120, pz = 96;
  const Box box{px - 20, px + 19, pz - 20, pz + 19, py, py + 6, true};
  const int kTicks = (int)BaselineNumber("solute-dilute.ticks", 3000);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  const std::vector<CellOp> build = box.Build((uint32_t)kMatStone);
  const std::vector<CellOp> pond = box.Layers(water, py + 1, 2, 7u);
  const std::vector<CellOp> grain = {
      {World::SlotCellIndex({px, py + 3, pz}), salt & 0xFFFu}};

  uint32_t t = 53000;
  support::TickCursor ticker{c, t, {px >> 4, py >> 4, pz >> 4}};
  int goneAt = -1;
  uint64_t peakMass = 0;
  for (int i = 0; i < kTicks; i++) {
    ticker({}, i == 0 ? build : i == 2 ? pond : i == 4 ? grain : std::vector<CellOp>{});
    if (i >= 30 && i % 60 == 0) {
      SoluteLayer L;
      ReadSoluteLayer(c, L);
      uint64_t m = 0;
      for (uint32_t slot : box.Chunks())
        for (uint32_t k = 0; k < kChunkVol; k++) m += L.Value(slot, k) & 0xFFu;
      peakMass = std::max(peakMass, m);
      if (m == 0 && L.hdr[kSolMDissolved] != 0 && goneAt < 0) goneAt = i;
      if (m != 0) goneAt = -1;
    }
  }
  const uint32_t awake = AwakeIn(c, box);
  SoluteLayer L;
  ReadSoluteLayer(c, L);
  const BoxCensus k = Census(c, box, L, salt);
  std::string latch;
  const bool latchOk = LatchesClean(L, latch);
  const bool goneOk = k.mass == 0 && k.pageSlots == 0 && k.uniformSlots == 0;
  const bool ledgerOk = L.hdr[kSolMDissolved] != 0 &&
                        L.hdr[kSolMDissolved] == L.hdr[kSolMDiscarded];
  const bool idleOk = awake == 0;
  RecordObserved("solute-dilute.goneAt", (double)goneAt);
  detail = Format(
      "1 salt voxel into a %d x %d x 2 pool: peak dissolved mass %llu, now %llu "
      "(%s) in %u page / %u uniform slots, gone from tick %d; ledger dissolved "
      "%u discarded %u (%s), %u powder eighths left, %u of %zu chunks awake%s%s",
      box.x1 - box.x0 + 1, box.z1 - box.z0 + 1, (unsigned long long)peakMass,
      (unsigned long long)k.mass, goneOk ? "GONE" : "RESIDUE", k.pageSlots,
      k.uniformSlots, goneAt, L.hdr[kSolMDissolved], L.hdr[kSolMDiscarded],
      ledgerOk ? "balanced" : "UNBALANCED", (uint32_t)k.powderEighths, awake,
      box.Chunks().size(), latchOk ? "" : ", ", latch.c_str());
  const bool ok = goneOk && ledgerOk && idleOk && latchOk && k.powderEighths == 0;
  if (k.powderEighths != 0) {
    // Where the grain came to rest and what it touches: a grain that never
    // dissolved is a contact question before it is a solute one.
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (uint32_t slot : box.Chunks()) {
      ReadVoxelsSync(c.ctx, c.world, slot, 1, cbuf.data(), "dilutePeek");
      const IVec3 wc = c.world.SlotToWorldChunk(slot);
      for (uint32_t q = 0; q < kChunkVol; q++) {
        if ((cbuf[q] & 0xFFFu) != salt) continue;
        const int x = wc.x * 16 + (int)(q % 16), y = wc.y * 16 + (int)((q / 16) % 16),
                  z = wc.z * 16 + (int)(q / 256);
        detail += Format("; grain at (%d,%d,%d) word %08x, faces", x, y, z, cbuf[q]);
        const int d[6][3] = {{0, -1, 0}, {0, 1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
        for (auto& o : d) {
          const IVec3 n{x + o[0], y + o[1], z + o[2]};
          std::vector<uint32_t> nb((size_t)kChunkVol);
          const uint32_t ns = World::SlotChunkIndex({n.x >> 4, n.y >> 4, n.z >> 4});
          ReadVoxelsSync(c.ctx, c.world, ns, 1, nb.data(), "dilutePeekN");
          const uint32_t li = (uint32_t)(((n.z & 15) * 16 + (n.y & 15)) * 16 + (n.x & 15));
          detail += Format(" %08x", nb[li]);
        }
      }
    }
  }
  std::printf("solute-dilute: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- solute-evap ------------------------------------------------------------
// Brine boiled off a LAVA floor. The water touching lava boils (water +
// tag:hot -> steam); the boiling cell's mass is pushed into the brine above it
// (the pool concentrates), and once the remaining brine is saturated the
// boiling cell PRECIPITATES salt instead of turning to steam. The floor is
// re-laid as lava every 25 ticks (lava quenches to stone against water).
//
// The balance across the phase change is exact by construction of the ledger,
// and the gate checks it against the WORLD rather than trusting the ledger:
//   salt still powder (eighths x units) + mass still dissolved + units
//   discarded == the salt that went in.
// Discarded is the sub-eighth remainder of each precipitation (a cell can only
// leave whole eighths of salt): bounded, and pinned in tests/baseline.json.
Status GateSoluteEvap(Ctx& c, std::string& detail) {
  const uint32_t water = MatNamed(c, "water"), salt = MatNamed(c, "salt"),
                 lava = MatNamed(c, "lava");
  const SoluteDef* sd = CurrentSoluteNamed("salt");
  if (!water || !salt || !lava || !sd) {
    detail = "needs materials water + salt + lava and a `salt` species";
    return Status::Fail;
  }
  FixtureTuning tune;
  const int px = 96, py = 120, pz = 96;
  const Box box{px - 3, px + 2, pz - 3, pz + 2, py, py + 14, false};
  const int kTicks = (int)BaselineNumber("solute-evap.ticks", 1700);
  const int boilFrom = 400, boilTo = 1400;
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  const std::vector<CellOp> build = box.Build((uint32_t)kMatStone);
  const std::vector<CellOp> pond = box.Layers(water, py + 1, 2, 7u);
  std::vector<CellOp> grains;
  for (int z = box.z0; z <= box.z1; z++)
    for (int x = box.x0; x <= box.x1; x++)
      if (((x + z) & 1) == 0 && grains.size() < 12)
        grains.push_back({World::SlotCellIndex({x, py + 3, z}), salt & 0xFFFu});
  const uint64_t y8 = sd->yieldPerVoxel / 8u;
  const uint64_t placedUnits = (uint64_t)grains.size() * 8u * y8;
  // The floor, as lava (only the interior: the walls stay stone).
  std::vector<CellOp> hot;
  for (int z = box.z0; z <= box.z1; z++)
    for (int x = box.x0; x <= box.x1; x++)
      hot.push_back({World::SlotCellIndex({x, py, z}), (lava & 0xFFFu) | (7u << 12)});

  uint32_t t = 54000;
  support::TickCursor ticker{c, t, {px >> 4, py >> 4, pz >> 4}};
  for (int i = 0; i < kTicks; i++) {
    const bool relay = i >= boilFrom && i < boilTo && (i - boilFrom) % 25 == 0;
    ticker({}, i == 0   ? build
               : i == 2 ? pond
               : i == 4 ? grains
               : relay  ? hot
                        : std::vector<CellOp>{});
  }
  SoluteLayer L;
  ReadSoluteLayer(c, L);
  const BoxCensus k = Census(c, box, L, salt);
  std::string latch;
  const bool latchOk = LatchesClean(L, latch);
  const uint64_t discarded = L.hdr[kSolMDiscarded];
  const uint64_t accounted = k.powderEighths * y8 + k.mass + discarded;
  const bool balanceOk = accounted == placedUnits;
  const bool precipOk = L.hdr[kSolMPrecip] > 0 && k.powderCells > 0;
  const double maxDiscard = BaselineNumber("solute-evap.maxDiscard", 512);
  const bool roundOk = (double)discarded <= maxDiscard;
  RecordObserved("solute-evap.precip", (double)L.hdr[kSolMPrecip]);
  RecordObserved("solute-evap.discarded", (double)discarded);
  detail = Format(
      "%zu salt voxels (%llu units) dissolved into 2 layers of %d x %d, boiled "
      "off lava from tick %d to %d: %u liquid cells left, %llu units still "
      "dissolved, %u salt cells (%llu eighths = %llu units) in the box, ledger "
      "dissolved %u precipitated %u discarded %llu (allow %.0f): %llu of %llu "
      "units accounted for (%s)%s%s",
      grains.size(), (unsigned long long)placedUnits, box.x1 - box.x0 + 1,
      box.z1 - box.z0 + 1, boilFrom, boilTo, k.solventCells,
      (unsigned long long)k.mass, k.powderCells, (unsigned long long)k.powderEighths,
      (unsigned long long)(k.powderEighths * y8), L.hdr[kSolMDissolved],
      L.hdr[kSolMPrecip], (unsigned long long)discarded, maxDiscard,
      (unsigned long long)accounted, (unsigned long long)placedUnits,
      balanceOk ? "BALANCED" : "UNBALANCED", latchOk ? "" : ", ", latch.c_str());
  const bool ok = balanceOk && precipOk && roundOk && latchOk;
  std::printf("solute-evap: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& SoluteGates() {
  static const std::vector<Gate> g = {
      {"solute", "sim", {}, false, GateSolute, false},
      {"solute-dilute", "sim", {}, false, GateSoluteDilute, false},
      {"solute-evap", "sim", {}, false, GateSoluteEvap, false},
  };
  return g;
}

}  // namespace selftest
