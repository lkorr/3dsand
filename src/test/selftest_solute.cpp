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
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <cmath>

#include "game/camera.h"
#include "game/composition.h"
#include "game/container.h"
#include "game/item.h"
#include "gpu/resources.h"
#include "sim/celestial.h"
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
  std::vector<uint32_t> stall;   // per slot: the diffusion stall counter
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
  L.stall.assign(kNumSlots, 0u);
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.solMeta,
                        (uint64_t)kSolMStall * 4, L.stall.data(), L.stall.size() * 4,
                        "solStall");
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

// `altMat` (0 = none) is a LIQUID form of the same matter (molten salt is
// salt): its cells count into powderEighths at their fullness, so a balance
// across melting and freezing holds.
BoxCensus Census(Ctx& c, const Box& box, const SoluteLayer& L, uint32_t powderMat,
                 uint32_t altMat = 0) {
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
      if (m == altMat && altMat != 0) {
        r.powderCells++;
        r.powderEighths += ((w >> 12) & 0x7u) + 1u;
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
    // SANDVOX_SOLUTE_TRACE=1: every 10 ticks, the mass, entry and stall
    // counter of every box chunk carrying solute (the plume's decay, chunk by
    // chunk -- what found the face-ownership stall).
    if (i >= 30 && (i % 60 == 0 || (getenv("SANDVOX_SOLUTE_TRACE") && i % 10 == 0))) {
      SoluteLayer L;
      ReadSoluteLayer(c, L);
      uint64_t m = 0;
      for (uint32_t slot : box.Chunks())
        for (uint32_t k = 0; k < kChunkVol; k++) m += L.Value(slot, k) & 0xFFu;
      peakMass = std::max(peakMass, m);
      if (m == 0 && L.hdr[kSolMDissolved] != 0 && goneAt < 0) goneAt = i;
      if (m != 0) goneAt = -1;
      if (getenv("SANDVOX_SOLUTE_TRACE")) {
        std::printf("dilute t%d: mass %llu disc %u |", i, (unsigned long long)m,
                    L.hdr[kSolMDiscarded]);
        for (uint32_t slot : box.Chunks()) {
          uint32_t cm = 0;
          for (uint32_t q = 0; q < kChunkVol; q++) cm += L.Value(slot, q) & 0xFFu;
          if (L.table[slot] || cm)
            std::printf(" %u:%08x m%u st%u", slot, L.table[slot], cm, L.stall[slot]);
        }
        std::printf("\n");
      }
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
  if (k.mass != 0) {
    // What is left, cell by cell: a residue that will not decay is a question
    // about the cells holding it (a stuck pair, a non-solvent, a partial
    // cell under the floor), and a count alone cannot say which.
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    int listed = 0;
    for (uint32_t slot : box.Chunks()) {
      ReadVoxelsSync(c.ctx, c.world, slot, 1, cbuf.data(), "diluteResidue");
      const IVec3 wc = c.world.SlotToWorldChunk(slot);
      if (L.table[slot] == 0u) continue;
      detail += Format("; slot (%d,%d,%d) entry %08x stall %u", wc.x, wc.y, wc.z,
                       L.table[slot], L.stall[slot]);
      for (uint32_t q = 0; q < kChunkVol && listed < 24; q++) {
        const uint32_t v = L.Value(slot, q);
        if (v == 0) continue;
        listed++;
        detail += Format(" [%d,%d,%d m%u s%u w%08x]", wc.x * 16 + (int)(q % 16),
                         wc.y * 16 + (int)((q / 16) % 16), wc.z * 16 + (int)(q / 256),
                         v & 0xFFu, v >> 8, cbuf[q]);
      }
      if (listed >= 24) break;
    }
  }
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
                 lava = MatNamed(c, "lava"), molten = MatNamed(c, "molten_salt");
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
  // Salt that fell back on the lava melts (salt + tag:hot -> molten_salt) and
  // refreezes, and molten salt quenched by brine turns the brine to steam
  // (reactions.json): the same matter, counted in both forms.
  const BoxCensus k = Census(c, box, L, salt, molten);
  std::string latch;
  const bool latchOk = LatchesClean(L, latch);
  const uint64_t discarded = L.hdr[kSolMDiscarded];
  const uint64_t accounted = k.powderEighths * y8 + k.mass + discarded;
  const bool balanceOk = accounted == placedUnits;
  const bool precipOk = L.hdr[kSolMPrecip] > 0 && k.powderCells > 0;
  // Discarded is the sub-eighth remainder of a precipitation a NEIGHBOUR rule
  // forced (molten salt quenching brine to steam: the brine cell's mass can
  // only leave as whole eighths of salt in place -- a neighbour's neighbour is
  // past the lattice's reach). Bounded by 31 units an event; pinned loosely.
  const double maxDiscard = BaselineNumber("solute-evap.maxDiscard", 2048);
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

// ---- solute-seam -------------------------------------------------------------
// THE MPM SEAM IS REFUSED, and asserted (PLAN_solutes §7.2). A FluidParticle
// has no solute payload yet, so exciting a brine cell into the MPM solver
// would turn the liquid into particles and leave its mass behind on an air
// cell -- deleted. sim_fluid_seam.wgsl exciteDetect therefore refuses any cell
// carrying solute (counted, SOLM_SEAM_REFUSED) and the CA carries it instead.
//
// Fixture: brine made on a stone shelf (salt dissolved in 3 layers of water),
// then a hole opened in the shelf so the pond DRAINS into the chamber below --
// the seam's by-fall excitation fires on exactly this -- with the shipped
// excite mode on. Asserted: the seam refused solute cells (it saw them and
// kept its hands off), and the dissolved mass is EXACTLY what went in (nothing
// discarded: a seam that took a brine cell would show up as mass lost).
Status GateSoluteSeam(Ctx& c, std::string& detail) {
  const uint32_t water = MatNamed(c, "water"), salt = MatNamed(c, "salt");
  const SoluteDef* sd = CurrentSoluteNamed("salt");
  if (!water || !salt || !sd) {
    detail = "needs materials water + salt and a `salt` species in solutes.json";
    return Status::Fail;
  }
  FixtureTuning tune(/*exciteMode=*/std::max(1, CurrentTuning().sim.fluidExciteMode));
  const int px = 96, py = 120, pz = 96;
  const Box box{px - 5, px + 4, pz - 5, pz + 4, py, py + 16, true};
  const int shelfY = py + 6;
  const int kTicks = (int)BaselineNumber("solute-seam.ticks", 900);
  const int openAt = 400;
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  std::vector<CellOp> build = box.Build((uint32_t)kMatStone);
  for (int z = box.z0; z <= box.z1; z++)
    for (int x = box.x0; x <= box.x1; x++)
      build.push_back({World::SlotCellIndex({x, shelfY, z}), (uint32_t)kMatStone});
  const std::vector<CellOp> pond = box.Layers(water, shelfY + 1, 3, 7u);
  std::vector<CellOp> grains;
  for (int z = pz - 1; z <= pz + 1; z++)
    for (int x = px - 1; x <= px + 1; x++)
      grains.push_back({World::SlotCellIndex({x, shelfY + 4, z}), salt & 0xFFFu});
  const uint64_t y8 = sd->yieldPerVoxel / 8u;
  const uint64_t placedUnits = (uint64_t)grains.size() * 8u * y8;
  std::vector<CellOp> hole;
  for (int z = pz - 2; z <= pz + 1; z++)
    for (int x = px - 2; x <= px + 1; x++)
      hole.push_back({World::SlotCellIndex({x, shelfY, z}), 0u});

  uint32_t t = 55000;
  support::TickCursor ticker{c, t, {px >> 4, py >> 4, pz >> 4}};
  uint32_t refusedBefore = 0;
  for (int i = 0; i < kTicks; i++) {
    if (i == openAt) {
      SoluteLayer L0;
      ReadSoluteLayer(c, L0);
      refusedBefore = L0.hdr[kSolMSeamRefused];
    }
    ticker({}, i == 0   ? build
               : i == 2 ? pond
               : i == 4 ? grains
               : i == openAt ? hole
                        : std::vector<CellOp>{});
  }
  SoluteLayer L;
  ReadSoluteLayer(c, L);
  const BoxCensus k = Census(c, box, L, salt);
  std::string latch;
  const bool latchOk = LatchesClean(L, latch);
  const uint32_t refused = L.hdr[kSolMSeamRefused];
  // Over the whole run: the salt landing in the pond disturbs the surface
  // (excite candidates beside fresh brine) and the drain does again. Either
  // is the seam SEEING a solute cell; the mass line is what says it kept its
  // hands off every one of them.
  const bool refusedOk = refused > 0;
  const bool massOk = k.mass + k.powderEighths * y8 == placedUnits && L.hdr[kSolMDiscarded] == 0;
  const bool strayOk = k.strayMass == 0;
  RecordObserved("solute-seam.refused", (double)refused);
  detail = Format(
      "%zu salt voxels (%llu units) dissolved on a shelf, drained through a hole "
      "at tick %d with fluidExciteMode %d: the seam refused %u solute cells "
      "(%u before the hole), mass %s (%llu dissolved + %llu units still powder, "
      "ledger discarded %u), %u solvent cells in the box, stray %u%s%s",
      grains.size(), (unsigned long long)placedUnits, openAt,
      CurrentTuning().sim.fluidExciteMode, refused - refusedBefore, refusedBefore,
      massOk ? "EXACT" : "LOST", (unsigned long long)k.mass,
      (unsigned long long)(k.powderEighths * y8), L.hdr[kSolMDiscarded], k.solventCells,
      k.strayMass, latchOk ? "" : ", ", latch.c_str());
  const bool ok = refusedOk && massOk && strayOk && latchOk;
  std::printf("solute-seam: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- solute-electrolysis -----------------------------------------------------
// A CONCENTRATION CONDITION on a reaction (PLAN_solutes §4.1, the rule side
// array): reactions.json's brine-electrolysis rules fire on WATER beside a
// spark only while the water carries salt at cMin or more. Two identical boxes
// side by side, one brine and one fresh, the same sparks laid over both:
// asserted, the brine box makes lye and chlorine and the fresh box makes
// neither -- the condition both enables and refuses, which is the whole claim
// (a rule that ignored the side array would fire in both).
Status GateSoluteElectrolysis(Ctx& c, std::string& detail) {
  const uint32_t water = MatNamed(c, "water"), salt = MatNamed(c, "salt"),
                 spark = MatNamed(c, "spark"), lye = MatNamed(c, "lye"),
                 chlorine = MatNamed(c, "chlorine"), hydrogen = MatNamed(c, "hydrogen");
  if (!water || !salt || !spark || !lye || !chlorine || !CurrentSoluteNamed("salt")) {
    detail = "needs water, salt, spark, lye, chlorine and the `salt` species";
    return Status::Fail;
  }
  FixtureTuning tune;
  const int py = 120, pz = 96;
  const Box brine{80, 85, pz - 3, pz + 2, py, py + 10, true};
  const Box fresh{100, 105, pz - 3, pz + 2, py, py + 10, true};
  const int kTicks = (int)BaselineNumber("solute-electrolysis.ticks", 420);
  const int sparkFrom = 300, sparkTo = 312;
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  std::vector<CellOp> build = brine.Build((uint32_t)kMatStone);
  for (const CellOp& o : fresh.Build((uint32_t)kMatStone)) build.push_back(o);
  std::vector<CellOp> pond = brine.Layers(water, py + 1, 2, 7u);
  for (const CellOp& o : fresh.Layers(water, py + 1, 2, 7u)) pond.push_back(o);
  // 16 grains into 72 cells of water: ~56 units a cell, over cMin 24.
  std::vector<CellOp> grains;
  for (int z = brine.z0; z <= brine.z1; z++)
    for (int x = brine.x0; x <= brine.x1; x++)
      if (grains.size() < 16 && ((x + z) % 2 == 0 || grains.size() < 4))
        grains.push_back({World::SlotCellIndex({x, py + 3, z}), salt & 0xFFFu});
  auto sparks = [&](const Box& b) {
    std::vector<CellOp> v;
    for (int z = b.z0; z <= b.z1; z++)
      for (int x = b.x0; x <= b.x1; x++)
        v.push_back({World::SlotCellIndex({x, py + 3, z}), (spark & 0xFFFu) | kCellOpIfAir});
    return v;
  };
  std::vector<CellOp> zap = sparks(brine);
  for (const CellOp& o : sparks(fresh)) zap.push_back(o);

  auto count = [&](const Box& b, uint32_t mat) {
    uint32_t n = 0;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (uint32_t slot : b.Chunks()) {
      ReadVoxelsSync(c.ctx, c.world, slot, 1, cbuf.data(), "electroVox");
      const IVec3 wc = c.world.SlotToWorldChunk(slot);
      for (uint32_t q = 0; q < kChunkVol; q++) {
        const int x = wc.x * 16 + (int)(q % 16), y = wc.y * 16 + (int)((q / 16) % 16),
                  z = wc.z * 16 + (int)(q / 256);
        if (b.Inside(x, y, z) && (cbuf[q] & 0xFFFu) == mat) n++;
      }
    }
    return n;
  };
  uint32_t t = 56000;
  support::TickCursor ticker{c, t, {90 >> 4, py >> 4, pz >> 4}};
  // Products are counted at their PEAK over the spark window (chlorine and
  // hydrogen are gases that rise and fade; lye is a liquid and stays), the
  // way chem-electrolysis counts sodium.
  uint32_t lyeB = 0, lyeF = 0, clB = 0, clF = 0, h2B = 0;
  for (int i = 0; i < kTicks; i++) {
    // Consecutive ticks, IF AIR (a spark only where nothing is), exactly as
    // chem-electrolysis electrifies its molten pool.
    const bool zapNow = i >= sparkFrom && i < sparkTo;
    std::vector<CellOp> ops = i == 0 ? build : i == 2 ? pond : i == 4 ? grains
                                                 : zapNow ? zap : std::vector<CellOp>{};
    ticker({}, ops);
    if (i >= sparkFrom && i < sparkTo + 20 && (i % 2) == 0) {
      lyeB = std::max(lyeB, count(brine, lye));
      lyeF = std::max(lyeF, count(fresh, lye));
      clB = std::max(clB, count(brine, chlorine));
      clF = std::max(clF, count(fresh, chlorine));
      if (hydrogen) h2B = std::max(h2B, count(brine, hydrogen));
    }
  }
  SoluteLayer L;
  ReadSoluteLayer(c, L);
  std::string latch;
  const bool latchOk = LatchesClean(L, latch);
  const bool ok = lyeB > 0 && lyeF == 0 && clF == 0 && latchOk;
  detail = Format(
      "sparks laid over brine (16 salt grains in 72 cells) and fresh water from "
      "tick %d to %d: brine box %u lye, %u chlorine, %u hydrogen cells; fresh box "
      "%u lye, %u chlorine (both must be 0)%s%s",
      sparkFrom, sparkTo, lyeB, clB, h2B, lyeF, clF, latchOk ? "" : ", ", latch.c_str());
  // Which of water's compiled rules carry the condition (the side array's
  // source), and the brine's concentration where the sparks land.
  {
    const MaterialDef& wm = c.mats[water];
    std::string conds;
    for (uint32_t k = 0; k < wm.ruleFx.size(); k++)
      if (!wm.ruleFx[k].solute.empty())
        conds += Format(" rule %u (gpu %u) %s>=%u", k, wm.gpu.reactOffset + k,
                        wm.ruleFx[k].solute.c_str(), wm.ruleFx[k].soluteMin);
    uint32_t maxC = 0, nMass = 0;
    for (uint32_t slot : brine.Chunks())
      for (uint32_t q = 0; q < kChunkVol; q++) {
        const uint32_t v = L.Value(slot, q);
        if (v) { nMass++; maxC = std::max(maxC, v & 0xFFu); }
      }
    detail += Format("; water has %u rules, conditioned:%s; brine %u cells, max mass %u",
                     wm.gpu.reactCount, conds.c_str(), nMass, maxC);
  }
  std::printf("solute-electrolysis: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- solute-vessel ------------------------------------------------------------
// THE WORLD HALF OF THE VESSEL SEAM, scoop direction (PLAN_alchemy_chemistry
// contract 2.5): a flask scooping BRINE is paid the water AND the salt. The
// clears go through the real MutationQueue and the real tick; sim_solute.wgsl
// solScoop moves each cleared cell's mass to the per-species scoop ledger; the
// snapshot carries it; ContainerSoluteObserve / Take turn it into whole
// dissolved eighths. Asserted EXACT: every unit the pond lost is in the flask
// as dissolved salt, in the overflow (what did not fit, which the live tick
// spills), or in the ledger's sub-eighth remainder -- and no mass is left on a
// cleared (air) cell. The water half is the vessel gates' claim, not this one's.
//
// The ContainerSettle / ContainerSoluteTake calls below are the single-memo
// gate form of what session.cpp's PhaseG does (the pure functions; the live
// deposit loop over hands / hotbar / pack is glue around them).
Status GateSoluteVessel(Ctx& c, std::string& detail) {
  const uint32_t water = MatNamed(c, "water"), salt = MatNamed(c, "salt");
  const SoluteDef* sd = CurrentSoluteNamed("salt");
  const int flaskI = c.items.Find("flask");
  const ItemDef* flask = c.items.At(flaskI);
  if (!water || !salt || !sd || !flask) {
    detail = "needs water, salt, a `salt` species and the flask item";
    return Status::Fail;
  }
  FixtureTuning tune;
  const int px = 96, py = 120, pz = 96;
  const Box box{px - 4, px + 3, pz - 4, pz + 3, py, py + 6, false};
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  const std::vector<CellOp> build = box.Build((uint32_t)kMatStone);
  const std::vector<CellOp> pond = box.Layers(water, py + 1, 2, 7u);
  std::vector<CellOp> grains;
  for (int z = pz - 3; z < pz + 3; z++)
    for (int x = px - 2; x < px + 2; x++)
      grains.push_back({World::SlotCellIndex({x, py + 3, z}), salt & 0xFFFu});
  const uint32_t y8 = std::max<uint32_t>(1, sd->yieldPerVoxel / 8);

  uint32_t t = 58000;
  support::TickCursor ticker{c, t, {px >> 4, py >> 4, pz >> 4}};
  // Dissolve it all first: a grain still dissolving during the scoop would
  // move the pond's mass for a reason that is not the scoop.
  SoluteLayer L;
  BoxCensus k0;
  int dissolvedAt = -1;
  for (int i = 0; i < 900 && dissolvedAt < 0; i++) {
    ticker({}, i == 0 ? build : i == 2 ? pond : i == 4 ? grains : std::vector<CellOp>{});
    if (i >= 100 && i % 50 == 0) {
      ReadSoluteLayer(c, L);
      k0 = Census(c, box, L, salt);
      if (k0.powderCells == 0 && k0.mass > 0) dissolvedAt = i;
    }
  }
  if (dissolvedAt < 0) {
    detail = Format("the salt never finished dissolving (%u powder cells, mass %llu)",
                    k0.powderCells, (unsigned long long)k0.mass);
    return Status::Fail;
  }

  // THE SCOOP: aimed each tick at the highest water the snapshot shows.
  ItemStack st = StackOf(c.items, flaskI, 1);
  ContainerScoopMemo memo;
  ContainerSoluteLedger led;
  int dissolvedIn = 0, overflow = 0, scoopTicks = 0;
  auto settle = [&]() {
    const WorldSnapshot& sn = c.world.Snap();
    if (!sn.valid) return;
    ContainerSettle(memo, sn.tick, sn.scoopEighths, flask, &st);
    ContainerSoluteObserve(led, sn.tick, sn.solScoopedBy);
    const int n = ContainerSoluteTake(led, sd->species, y8);
    if (n <= 0) return;
    const int put = ContainerDeposit(*flask, st, (uint16_t)(alchemy::kDissolvedBit | salt), n);
    dissolvedIn += put;
    overflow += n - put;
  };
  for (int i = 0; i < 60 && st.FillTotal() < (uint32_t)flask->container.capacity; i++) {
    IVec3 aim{0, 0, 0};
    bool found = false;
    for (int y = py + 3; y > py && !found; y--)
      for (int z = box.z0; z <= box.z1 && !found; z++)
        for (int x = box.x0; x <= box.x1 && !found; x++) {
          uint32_t w = 0;
          if (ContainerSnapWord(c.world, {x, y, z}, w) && (w & 0xFFFu) == water) {
            aim = {x, y, z};
            found = true;
          }
        }
    std::vector<CellOp> cells;
    if (found) {
      ContainerScoop(*flask, st, aim,
                     [&](IVec3 p, uint32_t& w) { return ContainerSnapWord(c.world, p, w); },
                     c.world, c.mats, cells, nullptr, &memo, t + 1);
      scoopTicks++;
    }
    ticker({}, cells);
    settle();
  }
  for (int i = 0; i < 12; i++) {   // let the last claims' snapshots arrive
    ticker({}, {});
    settle();
  }
  ReadSoluteLayer(c, L);
  const BoxCensus k1 = Census(c, box, L, salt);
  const uint32_t remainder = led.pot[sd->species - 1];
  const uint64_t lost = k0.mass - std::min(k0.mass, k1.mass);
  const uint64_t paid = (uint64_t)(dissolvedIn + overflow) * y8 + remainder;
  const uint32_t ledgerUnits = sd->species <= kSolScoopSpecies
                                   ? L.hdr[kSolMScoopBySpecies + sd->species - 1] : 0u;
  std::string latch;
  const bool latchOk = LatchesClean(L, latch);
  const bool exact = k1.mass <= k0.mass && lost == paid && ledgerUnits == lost;
  const bool credited = dissolvedIn > 0 && st.contents.AmountOf(water) > 0;
  const bool ok = exact && credited && k1.strayMass == 0 && memo.claims.empty() && latchOk;
  RecordObserved("solute-vessel.dissolvedEighths", (double)dissolvedIn);
  detail = Format(
      "pond mass %llu (dissolved by tick %d) -> %llu after %d scoop ticks: lost %llu, "
      "ledger %u, paid %d dissolved eighths into the flask + %d overflow at %u units "
      "+ %u remainder = %llu (%s); flask holds %u water + %u dissolved salt of %d; "
      "stray %u, open claims %zu%s%s",
      (unsigned long long)k0.mass, dissolvedAt, (unsigned long long)k1.mass, scoopTicks,
      (unsigned long long)lost, ledgerUnits, dissolvedIn, overflow, y8, remainder,
      (unsigned long long)paid, exact ? "EXACT" : "LEAK", st.contents.AmountOf(water),
      st.contents.AmountOf((uint16_t)(alchemy::kDissolvedBit | salt)),
      flask->container.capacity, k1.strayMass, memo.claims.size(), latchOk ? "" : ", ",
      latch.c_str());
  std::printf("solute-vessel: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- solute-look ------------------------------------------------------------
// The RENDER half (package H; raymarch.wgsl solLookAt): six stone pools of
// water side by side on the ground at noon -- plain, brine, fairy, vitriol,
// lumen and ink -- each POURED (CellOpSolute, package G) to a concentration a
// little under the one its colour is authored "at saturation" for, then drawn
// from above into build/solute_look.bmp.
//
// Asserted: every seeded pool carries dissolved mass (the fixture did what it
// says, so a pixel verdict is about the shader and not an empty layer), the
// latches are clean, and at the projected centre of each pool the frame moved
// the way the authored tint says it must against the plain-water pool: ink
// darker, vitriol bluer, fairy more violet, lumen brighter (its glow). Brine is
// only reported: it is authored FAINT (tintStrength 40). Thresholds in
// tests/baseline.json (soluteLook.*). Render-only: the world hash does not see
// any of it.
Status GateSoluteLook(Ctx& c, std::string& detail) {
  // Each pool is POURED to a target concentration (package G's CellOpSolute
  // ops: mass straight into the water, no floating powder in the picture), a
  // little under the species' look reference -- the lowest of its saturation
  // and its water `converts` cMin (salt 90, fairy 48, vitriol 200, lumen 96,
  // ink 80) -- so nothing converts and every pool is still that solution.
  struct PoolDef { const char* species; uint32_t conc; };
  const PoolDef defs[6] = {{nullptr, 0},     {"salt", 70},    {"fairy", 40},
                           {"vitriol", 150}, {"lumen", 80},   {"ink", 64}};
  const char* names[6] = {"water", "brine", "fairy", "vitriol", "lumen", "ink"};
  const uint32_t water = MatNamed(c, "water");
  uint32_t species[6] = {0, 0, 0, 0, 0, 0};
  for (int i = 1; i < 6; i++) {
    const SoluteDef* sd = CurrentSoluteNamed(defs[i].species);
    if (!sd) {
      detail = Format("needs solutes.json species %s", defs[i].species);
      return Status::Fail;
    }
    species[i] = sd->species;
  }
  if (!water) { detail = "needs water"; return Status::Fail; }
  FixtureTuning tune;
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  // 3 x 2 pools, 6 x 6 inside, 3 deep, walls one cell thick (two between).
  const int bx = 160, bz = 160, kDeep = 3;
  const int fy = FixtureYOver(bx - 1, bz - 1, bx + 22, bz + 14, kDefaultSeed, 1);
  std::vector<Box> pools;
  for (int i = 0; i < 6; i++) {
    const int x0 = bx + (i % 3) * 8, z0 = bz + (i / 3) * 8;
    pools.push_back(Box{x0, x0 + 5, z0, z0 + 5, fy, fy + 4, false});
  }
  std::vector<CellOp> build, fill, pour[6];
  for (const Box& b : pools) {
    const std::vector<CellOp> v = b.Build((uint32_t)kMatStone);
    build.insert(build.end(), v.begin(), v.end());
    const std::vector<CellOp> w = b.Layers(water, fy + 1, kDeep, 7u);
    fill.insert(fill.end(), w.begin(), w.end());
  }
  // One pour per water CELL, of that cell's share: a whole column's worth
  // poured onto one cell would sit above a `converts` cMin there (fairy,
  // lumen) and convert before diffusion could spread it.
  for (int i = 1; i < 6; i++)
    for (int y = fy + 1; y <= fy + kDeep; y++)
      for (int z = pools[i].z0; z <= pools[i].z1; z++)
        for (int x = pools[i].x0; x <= pools[i].x1; x++)
          pour[i].push_back({World::SlotCellIndex({x, y, z}),
                             CellOpSolute(species[i], defs[i].conc)});
  const int kTicks = (int)BaselineNumber("soluteLook.ticks", 120);
  uint32_t t = 61000;
  support::TickCursor ticker{c, t, {(bx + 11) >> 4, fy >> 4, (bz + 7) >> 4}};
  for (int i = 0; i < kTicks; i++)
    // One pool's pours a tick: all 540 at once overran the cell-op stream cap.
    ticker({}, i == 0 ? build : i == 2 ? fill
                   : (i >= 6 && i < 11) ? pour[i - 5] : std::vector<CellOp>{});

  SoluteLayer L;
  ReadSoluteLayer(c, L);
  std::string latch;
  const bool latchOk = LatchesClean(L, latch);
  // Mass and mean concentration INSIDE each pool (the shared Census counts
  // whole chunks, and these pools share chunks).
  uint64_t mass[6] = {0, 0, 0, 0, 0, 0};
  bool massOk = true;
  for (int i = 0; i < 6; i++) {
    const Box& b = pools[i];
    for (int y = fy + 1; y <= fy + kDeep; y++)
      for (int z = b.z0; z <= b.z1; z++)
        for (int x = b.x0; x <= b.x1; x++) {
          const uint32_t slot = World::SlotChunkIndex({x >> 4, y >> 4, z >> 4});
          const uint32_t local = (uint32_t)(((z & 15) * 16 + (y & 15)) * 16 + (x & 15));
          const uint32_t v = L.Value(slot, local);
          if ((v >> 8) == species[i] || i == 0) mass[i] += v & 0xFFu;
        }
    if ((i > 0) == (mass[i] == 0)) massOk = false;   // tinted pools carry mass, water none
  }

  // ---- the frame: noon, from the south, looking down into the pools ----
  const Tuning base = CurrentTuning();
  uint32_t noonTick = 0;
  {
    float bestUp = -2.0f;
    for (uint32_t s = 0; s < 200000u; s += 64u) {
      const float up = ComputeSky(base, (double)s).sunDir[1];
      if (up > bestUp) { bestUp = up; noonTick = s; }
    }
  }
  const uint32_t W = c.width, H = c.height;
  const Vec3 eye{(float)bx + 10.5f, (float)fy + 24.0f, (float)bz - 9.0f};
  Camera cam;
  cam.yaw = 1.5707963f;   // +Z
  cam.pitch = -0.95f;
  rhi::Buffer shot = CreateBuffer(c.ctx.device, (uint64_t)W * H * 4,
                                  rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                  "soluteLookShot");
  const int kFrames = 8;
  for (int f = 0; f < kFrames; f++) {
    // SUN SHADOWS OFF: the claim is the TINT, and the pools' own rims cast a
    // staircase shadow across each pool that the centre sample straddles. It
    // landed (713dacf, 2026-09-27) just clear of that edge; the shadow-path
    // changes of 09-28 (staggered 16x4 refresh 531f2de, nearest patch below
    // 4 px 65d5992) moved the edge onto the far-row vitriol pool's centre
    // and read it as 'vitriol bluer by -8' against a lit near-row water pool.
    // With the key light's shadow term off every pool is lit alike.
    WriteRenderParams(c.ctx.queue, c.world, eye, cam, (float)W / H, false, 0.0f,
                      kFarFogDensity, (float)H, noonTick);
    rhi::CommandEncoder enc = c.ctx.device.CreateCommandEncoder();
    c.sim.EncodeShadowResolve(enc);
    rhi::RenderPass rp = c.sim.BeginRenderPass(enc, c.view, rhi::TextureFormat::RGBA8Unorm, W, H);
    c.sim.DrawWorld(rp);
    rp.End();
    if (f + 1 == kFrames) {
      rhi::TexelCopyTexture srcT{};
      srcT.texture = c.offscreen;
      rhi::TexelCopyBuffer dstB{};
      dstB.buffer = shot;
      dstB.bytesPerRow = W * 4;
      dstB.rowsPerImage = H;
      rhi::Extent3D ext{W, H, 1};
      enc.CopyTextureToBuffer(srcT, dstB, ext);
    }
    c.ctx.queue.Submit(enc.Finish());
  }
  std::vector<uint8_t> px((size_t)W * H * 4, 0);
  const bool got = rhi::ReadBufferBlocking(c.ctx.device, shot, 0, px.data(), px.size());
  if (got) WriteBmpFile("build/solute_look.bmp", px, W, H);

  // Each pool's mean colour over a small window at its projected centre (the
  // water surface, fy + 3.9). Pinhole projection with the camera's own basis.
  const Vec3 F = cam.Forward(), R = cam.Right(), U = cam.Up();
  const float th = std::tan(cam.fovY * 0.5f), aspect = (float)W / H;
  double rgb[6][3] = {};
  bool projOk = got;
  for (int i = 0; i < 6 && got; i++) {
    const Box& b = pools[i];
    const Vec3 p{(b.x0 + b.x1 + 1) * 0.5f, (float)fy + 3.9f, (b.z0 + b.z1 + 1) * 0.5f};
    const Vec3 d{p.x - eye.x, p.y - eye.y, p.z - eye.z};
    const float zf = d.dot(F);
    const float sx = d.dot(R) / (zf * th * aspect), sy = d.dot(U) / (zf * th);
    const int cx = (int)((sx * 0.5f + 0.5f) * W), cy = (int)((0.5f - sy * 0.5f) * H);
    int n = 0;
    for (int y = cy - 6; y <= cy + 6; y++)
      for (int x = cx - 6; x <= cx + 6; x++) {
        if (x < 0 || y < 0 || x >= (int)W || y >= (int)H) continue;
        const size_t k = ((size_t)y * W + x) * 4;
        for (int ch = 0; ch < 3; ch++) rgb[i][ch] += px[k + ch];
        n++;
      }
    if (n == 0) { projOk = false; continue; }
    for (int ch = 0; ch < 3; ch++) rgb[i][ch] /= n;
  }
  auto lum = [&](int i) { return 0.2126 * rgb[i][0] + 0.7152 * rgb[i][1] + 0.0722 * rgb[i][2]; };
  const double inkDark = lum(0) - lum(5);
  const double vitBlue = (rgb[3][2] - rgb[3][0]) - (rgb[0][2] - rgb[0][0]);
  const double fairyViolet = ((rgb[2][0] + rgb[2][2]) * 0.5 - rgb[2][1]) -
                             ((rgb[0][0] + rgb[0][2]) * 0.5 - rgb[0][1]);
  const double lumenBright = lum(4) - lum(0);
  const bool lookOk = projOk &&
                      inkDark >= BaselineNumber("soluteLook.minInkDark", 20) &&
                      vitBlue >= BaselineNumber("soluteLook.minVitriolBlue", 7) &&
                      fairyViolet >= BaselineNumber("soluteLook.minFairyViolet", 8) &&
                      lumenBright >= BaselineNumber("soluteLook.minLumenBright", 15);
  RecordObserved("soluteLook.inkDark", inkDark);
  RecordObserved("soluteLook.vitriolBlue", vitBlue);
  RecordObserved("soluteLook.fairyViolet", fairyViolet);
  RecordObserved("soluteLook.lumenBright", lumenBright);

  std::string pools6;
  for (int i = 0; i < 6; i++)
    pools6 += Format("%s%s c %.0f rgb (%.0f,%.0f,%.0f)", i ? "; " : "", names[i],
                     (double)mass[i] / (36.0 * kDeep), rgb[i][0], rgb[i][1], rgb[i][2]);
  const bool ok = massOk && latchOk && lookOk;
  detail = Format("%s | vs water: ink darker by %.1f, vitriol bluer by %.1f, fairy more "
                  "violet by %.1f, lumen brighter by %.1f; frame %s%s%s",
                  pools6.c_str(), inkDark, vitBlue, fairyViolet, lumenBright,
                  got ? "build/solute_look.bmp" : "NOT READ", latchOk ? "" : ", ",
                  latch.c_str());
  std::printf("solute-look: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- solute-pour ----------------------------------------------------------------
// THE WORLD HALF OF THE VESSEL SEAM, pour direction (contract 2.5, package G).
// A flask of BRINE poured into a basin of fresh water: the salt leaves the
// flask with the water (ContainerTakeDissolvedShare), is queued as solute
// pours landing where the stream lands (ContainerSolutePour), goes out as
// CellOpSolute ops through the real MutationQueue and the real tick, and
// sim_mutate.wgsl solPour lays it into the water. Asserted:
//   A. every unit that left the flask is IN SOLUTION in the basin (the census
//      of the box == the units poured, exactly), the GPU's own ledger agrees
//      (poured + as-powder == sent, nothing lost), none of it came down as
//      powder (the basin had water to take it), the flask kept none;
//   B. THE FALLBACK: a pour op onto DRY stone precipitates its powder there
//      (the counted SOLM_POUR_POWDER), and when water arrives the crust
//      dissolves -- conserved across both;
//   C. the CPU fallbacks: with sim.soluteMode 0 the share comes out as grains
//      (nothing queued), and the MPM fluid road sends its share to the pour
//      queue instead of back into the flask as powder (contract 2.5 item 2).
Status GateSolutePour(Ctx& c, std::string& detail) {
  const uint32_t water = MatNamed(c, "water"), salt = MatNamed(c, "salt");
  const SoluteDef* sd = CurrentSoluteNamed("salt");
  const int flaskI = c.items.Find("flask");
  const ItemDef* flask = c.items.At(flaskI);
  if (!water || !salt || !sd || !flask) {
    detail = "needs water, salt, a `salt` species and the flask item";
    return Status::Fail;
  }
  FixtureTuning tune;
  const uint32_t y8 = std::max<uint32_t>(1, sd->yieldPerVoxel / 8);
  const uint16_t dSalt = (uint16_t)(alchemy::kDissolvedBit | salt);
  const int px = 96, py = 120, pz = 96;
  const Box basin{px - 3, px + 2, pz - 3, pz + 2, py, py + 8, false};
  // In chunks of its own (x 120..123 is chunk 7; the basin's walls end at 99),
  // so neither census counts the other's mass.
  const Box dry{px + 24, px + 27, pz - 2, pz + 1, py, py + 8, false};
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  std::vector<CellOp> build = basin.Build((uint32_t)kMatStone);
  {
    const std::vector<CellOp> b2 = dry.Build((uint32_t)kMatStone);
    build.insert(build.end(), b2.begin(), b2.end());
  }
  const std::vector<CellOp> pond = basin.Layers(water, py + 1, 2, 7u);

  uint32_t t = 60000;
  support::TickCursor ticker{c, t, {px >> 4, py >> 4, pz >> 4}};
  for (int i = 0; i < 40; i++)
    ticker({}, i == 0 ? build : i == 2 ? pond : std::vector<CellOp>{});
  SoluteLayer L;
  ReadSoluteLayer(c, L);
  const uint32_t poured0 = L.hdr[kSolMPoured], powder0 = L.hdr[kSolMPourPowder],
                 lost0 = L.hdr[kSolMPourLost], disc0 = L.hdr[kSolMDiscarded];
  const BoxCensus k0 = Census(c, basin, L, salt);

  // ---- A: pour a flask of brine into the basin -----------------------------
  ItemStack st = StackOf(c.items, flaskI, 1);
  st.contents.Add((uint16_t)water, 64);
  st.contents.Add(dSalt, 16);
  const Vec3 target{px + 0.0f, py + 2.5f, pz + 0.0f};
  const Vec3 mouth{target.x, target.y + 6.0f, target.z};
  std::vector<ContainerSolutePour> pours;
  int pourTicks = 0, opsSent = 0;
  uint32_t left = 0;
  for (int i = 0; i < 120; i++) {
    const uint32_t now = t + 1;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cells;
    if (st.Filled() && st.contents.AmountOf((uint16_t)water) > 0) {
      const WorldSnapshot& sn = c.world.Snap();
      if (ContainerPour(*flask, st, mouth, Vec3{0, -1, 0}, &target,
                        CurrentTuning().sim.partGravity, now, 0x5017u, spawns, nullptr,
                        ContainerParticleRoom(sn.valid, sn.particleCount, 0), &c.mats,
                        &pours) > 0)
        pourTicks++;
    }
    opsSent += ContainerSolutePoursDue(pours, now, c.world, cells);
    support::TickOps ops;
    ops.cells = cells;
    ops.spawns = spawns;
    ticker(ops);
    if (i > 40 && pours.empty() && st.contents.AmountOf((uint16_t)water) == 0) break;
  }
  left = st.contents.AmountOf(dSalt);
  for (int i = 0; i < 240; i++) ticker({}, {});
  ReadSoluteLayer(c, L);
  const BoxCensus kA = Census(c, basin, L, salt);
  const uint32_t sent = 16u * y8;
  const uint32_t pouredA = L.hdr[kSolMPoured] - poured0, powderA = L.hdr[kSolMPourPowder] - powder0,
                 lostA = L.hdr[kSolMPourLost] - lost0, discA = L.hdr[kSolMDiscarded] - disc0;
  const uint64_t inBox = kA.mass + kA.powderEighths * y8 - (k0.mass + k0.powderEighths * y8);
  const bool okA = left == 0 && pours.empty() && pouredA + powderA == sent && lostA == 0 &&
                   powderA == 0 && inBox == sent && discA == 0 && kA.strayMass == 0 &&
                   kA.mass > 0;

  // ---- B: a pour onto dry stone precipitates, and water dissolves it -------
  ReadSoluteLayer(c, L);
  const uint32_t powderB0 = L.hdr[kSolMPourPowder], lostB0 = L.hdr[kSolMPourLost];
  const uint32_t eB = 5;
  {
    std::vector<CellOp> op{{World::SlotCellIndex({px + 25, py + 4, pz - 1}),
                            CellOpSolute(sd->species, eB * y8)}};
    ticker({}, op);
  }
  for (int i = 0; i < 6; i++) ticker({}, {});
  ReadSoluteLayer(c, L);
  const BoxCensus kB0 = Census(c, dry, L, salt);
  const uint32_t powderB = L.hdr[kSolMPourPowder] - powderB0, lostB = L.hdr[kSolMPourLost] - lostB0;
  {
    std::vector<CellOp> flood;
    for (int z = dry.z0; z <= dry.z1; z++)
      for (int x = dry.x0; x <= dry.x1; x++)
        flood.push_back({World::SlotCellIndex({x, py + 3, z}),
                         (water & 0xFFFu) | (7u << 12) | kCellOpIfAir});
    ticker({}, flood);
  }
  for (int i = 0; i < 400; i++) ticker({}, {});
  ReadSoluteLayer(c, L);
  const BoxCensus kB1 = Census(c, dry, L, salt);
  const bool okB = powderB == eB * y8 && lostB == 0 && kB0.powderEighths == eB && kB0.mass == 0 &&
                   kB1.mass + kB1.powderEighths * y8 == (uint64_t)eB * y8 && kB1.powderEighths == 0;

  // ---- C: the CPU fallbacks ---------------------------------------------------
  bool okC1 = false, okC2 = false;
  std::string noteC;
  {
    Tuning off = CurrentTuning();
    off.sim.soluteMode = 0;
    const Tuning keep = CurrentTuning();
    SetCurrentTuning(off);
    std::vector<ContainerSolutePour> q;
    std::vector<ParticleSpawn> parts;
    const int out = ContainerDissolvedToWorld(dSalt, 6, {0, 0, 0}, {0, 0, 0}, 1u, 1u, c.mats, parts,
                                              0xFFFFFFFFu, &q, {0, 0, 0}, 5u);
    SetCurrentTuning(keep);
    okC1 = out == 6 && q.empty() && parts.size() == 6;
    // The fluid road: its share goes to the queue, not back into the flask.
    ItemStack f = StackOf(c.items, flaskI, 1);
    f.contents.Add((uint16_t)water, 32);
    f.contents.Add(dSalt, 8);
    std::vector<FluidSpawnOp> fl;
    std::vector<ContainerSolutePour> q2;
    const Vec3 tgt{0, 0, 0}, mo{0, 5, 0};
    const int n = ContainerPourFluid(*flask, f, mo, Vec3{0, -1, 0}, &tgt, 7u, 3u, 4096, fl, nullptr,
                                     &c.mats, &q2);
    uint32_t queued = 0;
    for (const ContainerSolutePour& p : q2) queued += p.units;
    const uint32_t shareE = (uint32_t)((uint64_t)8 * (uint32_t)n / 32u);
    okC2 = n > 0 && f.contents.AmountOf((uint16_t)salt) == 0 &&
           f.contents.AmountOf(dSalt) + queued / y8 == 8 && queued == shareE * y8 && queued > 0;
    noteC = Format("layer off: %d eighths as %zu grains, %zu queued; fluid road: %d eighths of "
                   "water poured, %u units queued (%u eighths of salt, flask keeps %u dissolved, "
                   "%u as powder)",
                   out, parts.size(), q.size(), n, queued, queued / y8, f.contents.AmountOf(dSalt),
                   f.contents.AmountOf((uint16_t)salt));
  }

  std::string latch;
  const bool latchOk = LatchesClean(L, latch);
  const bool ok = okA && okB && okC1 && okC2 && latchOk;
  RecordObserved("solute-pour.units", (double)pouredA);
  detail = Format(
      "A (brine into a basin of water): %u units in %d pour ticks, %d ops; GPU: %u into "
      "solution + %u as powder + %u lost (want %u + 0 + 0); basin gained %llu units (%llu "
      "dissolved, %llu as powder), %u discarded, stray %u, flask keeps %u dissolved -> %s; "
      "B (onto dry stone): %u units as powder (%llu eighths in the box, want %u), %u lost; "
      "flooded: %llu units dissolved + %llu eighths powder -> %s; C: %s -> %s%s%s",
      sent, pourTicks, opsSent, pouredA, powderA, lostA, sent, (unsigned long long)inBox,
      (unsigned long long)(kA.mass - k0.mass), (unsigned long long)kA.powderEighths, discA,
      kA.strayMass, left, okA ? "OK" : "FAIL", powderB, (unsigned long long)kB0.powderEighths, eB,
      lostB, (unsigned long long)kB1.mass, (unsigned long long)kB1.powderEighths,
      okB ? "OK" : "FAIL", noteC.c_str(), okC1 && okC2 ? "OK" : "FAIL", latchOk ? "" : ", ",
      latch.c_str());
  std::printf("solute-pour: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- solute-payout ------------------------------------------------------------
// THE SESSION'S PAYOUT of scooped dissolved salt (session.cpp PhaseG, after
// ContainerSettle), through THE tick: the gate scoops brine with the rig
// session's own memo, and TickAuthority pays the water into the session's
// vessels (hands, then hotbar, then pack) and the salt into the first of
// them that HOLDS A LIQUID. The kit: the right hand holds a flask with room
// for under two scoops' worth, the hotbar's first slot a pouch of sand (a vessel
// that must never be paid salt: it holds no liquid), its second an empty
// flask (where the overflow goes). Asserted: the hand flask is filled and
// salted, the overflow flask gets water AND salt, the pouch is untouched,
// and the salt is conserved exactly: what the pond lost == what the vessels
// hold as dissolved eighths + the ledger's sub-eighth remainder.
Status GateSolutePayout(Ctx& c, std::string& detail) {
  const uint32_t water = MatNamed(c, "water"), salt = MatNamed(c, "salt"),
                 sand = MatNamed(c, "sand");
  const SoluteDef* sd = CurrentSoluteNamed("salt");
  const int flaskI = c.items.Find("flask"), pouchI = c.items.Find("pouch");
  const ItemDef* flask = c.items.At(flaskI);
  if (!water || !salt || !sand || !sd || !flask || pouchI < 0) {
    detail = "needs water, salt, sand, a `salt` species, the flask and the pouch";
    return Status::Fail;
  }
  FixtureTuning tune;
  const uint32_t y8 = std::max<uint32_t>(1, sd->yieldPerVoxel / 8);
  const uint16_t dSalt = (uint16_t)(alchemy::kDissolvedBit | salt);
  const int px = 96, py = 120, pz = 96;
  const Box box{px - 4, px + 3, pz - 4, pz + 3, py, py + 6, false};
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  const std::vector<CellOp> build = box.Build((uint32_t)kMatStone);
  const std::vector<CellOp> pond = box.Layers(water, py + 1, 2, 7u);
  std::vector<CellOp> grains;
  for (int z = pz - 3; z < pz + 3; z++)
    for (int x = px - 2; x < px + 2; x++)
      grains.push_back({World::SlotCellIndex({x, py + 3, z}), salt & 0xFFFu});

  uint32_t t = 62000;
  support::TickCursor ticker{c, t, {px >> 4, py >> 4, pz >> 4}};
  SoluteLayer L;
  BoxCensus k0;
  int dissolvedAt = -1;
  for (int i = 0; i < 900 && dissolvedAt < 0; i++) {
    ticker({}, i == 0 ? build : i == 2 ? pond : i == 4 ? grains : std::vector<CellOp>{});
    if (i >= 100 && i % 50 == 0) {
      ReadSoluteLayer(c, L);
      k0 = Census(c, box, L, salt);
      if (k0.powderCells == 0 && k0.mass > 0) dissolvedAt = i;
    }
  }
  if (dissolvedAt < 0) {
    detail = Format("the salt never finished dissolving (%u powder cells, mass %llu)",
                    k0.powderCells, (unsigned long long)k0.mass);
    return Status::Fail;
  }

  // THE KIT, on the rig's session (the body TickAuthority pays).
  PlayerSession& s = ticker.Rig().Session();
  TickAuthorityCtx& w = ticker.Rig().Authority();
  Kit& kit = s.kit();
  ItemStack& hand = kit.equip.InHand(Hand::Right);
  const ItemStack handWas = hand, slot0Was = kit.hotbar.slots[0], slot1Was = kit.hotbar.slots[1];
  hand = StackOf(c.items, flaskI, 1);
  hand.contents.Add((uint16_t)water, (uint32_t)flask->container.capacity - 56u);
  kit.hotbar.slots[0] = StackOf(c.items, pouchI, 1);
  kit.hotbar.slots[0].contents.Add((uint16_t)sand, 8);
  kit.hotbar.slots[1] = StackOf(c.items, flaskI, 1);
  const uint32_t handWater0 = hand.contents.AmountOf((uint16_t)water);
  const uint32_t pot0 = w.soluteLedger.pot[sd->species - 1];

  // THE SCOOP, filed on the session's memo (what PhaseG settles): sized by a
  // scratch flask so the request never depends on where the payout lands.
  ItemStack sizer = StackOf(c.items, flaskI, 1);
  int scoopTicks = 0;
  for (int i = 0; i < 40 && sizer.FillTotal() < 96u; i++) {
    IVec3 aim{0, 0, 0};
    bool found = false;
    for (int y = py + 3; y > py && !found; y--)
      for (int z = box.z0; z <= box.z1 && !found; z++)
        for (int x = box.x0; x <= box.x1 && !found; x++) {
          uint32_t wd = 0;
          if (ContainerSnapWord(c.world, {x, y, z}, wd) && (wd & 0xFFFu) == water) {
            aim = {x, y, z};
            found = true;
          }
        }
    std::vector<CellOp> cells;
    if (found) {
      ContainerScoop(*flask, sizer, aim,
                     [&](IVec3 p, uint32_t& wd) { return ContainerSnapWord(c.world, p, wd); },
                     c.world, c.mats, cells, nullptr, &s.scoopMemo, t + 1);
      scoopTicks++;
    }
    ticker({}, cells);
  }
  for (int i = 0; i < 16; i++) ticker({}, {});   // the last claims' snapshots
  ReadSoluteLayer(c, L);
  const BoxCensus k1 = Census(c, box, L, salt);
  const uint32_t handSalt = hand.contents.AmountOf(dSalt),
                 handWater = hand.contents.AmountOf((uint16_t)water);
  const ItemStack& pouch = kit.hotbar.slots[0];
  const ItemStack& over = kit.hotbar.slots[1];
  const uint32_t overSalt = over.contents.AmountOf(dSalt),
                 overWater = over.contents.AmountOf((uint16_t)water);
  const bool pouchClean = pouch.contents.AmountOf(dSalt) == 0 &&
                          pouch.contents.AmountOf((uint16_t)water) == 0 &&
                          pouch.contents.AmountOf((uint16_t)sand) == 8;
  const uint32_t pot = w.soluteLedger.pot[sd->species - 1];
  const uint64_t lost = k0.mass - std::min(k0.mass, k1.mass);
  // The pot may have held a remainder from before; what it holds now minus
  // that is what this scoop left in it (negative when it paid an old one out).
  const int64_t paid = (int64_t)(handSalt + overSalt) * y8 + (int64_t)pot - (int64_t)pot0;
  std::string latch;
  const bool latchOk = LatchesClean(L, latch);
  const bool exact = k1.mass <= k0.mass && (int64_t)lost == paid && lost > 0;
  const bool ok = exact && handWater > handWater0 && handSalt > 0 && overWater > 0 &&
                  overSalt > 0 && pouchClean && s.scoopMemo.claims.empty() &&
                  w.vesselSpills.empty() && latchOk;
  detail = Format(
      "pond mass %llu -> %llu after %d scoop ticks: lost %llu; paid: hand flask %u -> %u "
      "water + %u dissolved eighths, overflow flask (hotbar 1) %u water + %u dissolved, "
      "pouch of sand (hotbar 0) %s; %u units in the pot (was %u) = %lld (%s); open claims %zu, "
      "spills %zu%s%s",
      (unsigned long long)k0.mass, (unsigned long long)k1.mass, scoopTicks,
      (unsigned long long)lost, handWater0, handWater, handSalt, overWater, overSalt,
      pouchClean ? "untouched" : "WAS PAID", pot, pot0, (long long)paid,
      exact ? "EXACT" : "LEAK", s.scoopMemo.claims.size(), w.vesselSpills.size(),
      latchOk ? "" : ", ", latch.c_str());
  hand = handWas;
  kit.hotbar.slots[0] = slot0Was;
  kit.hotbar.slots[1] = slot1Was;
  std::printf("solute-payout: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& SoluteGates() {
  static const std::vector<Gate> g = {
      {"solute", "sim", {}, false, GateSolute, false},
      {"solute-dilute", "sim", {}, false, GateSoluteDilute, false},
      {"solute-evap", "sim", {}, false, GateSoluteEvap, false},
      {"solute-seam", "sim", {}, false, GateSoluteSeam, false},
      {"solute-electrolysis", "sim", {}, false, GateSoluteElectrolysis, false},
      {"solute-vessel", "sim", {}, false, GateSoluteVessel, false},
      {"solute-look", "render", {}, false, GateSoluteLook, true},
      {"solute-pour", "sim", {}, false, GateSolutePour, false},
      {"solute-payout", "sim", {}, false, GateSolutePayout, false},
  };
  return g;
}

}  // namespace selftest
