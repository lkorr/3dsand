// selftest_heat.cpp — gates for THE TEMPERATURE LAYER (docs/PLAN_temperature.md).
//
//   heat-melt     a lava basin behind a stone wall melts the snow and ice near
//                 it (mass exact), not the snow beyond its reach; the field
//                 rises gradually and never past its target; with the lava
//                 gone every page is freed and the fixture sleeps.
//   heat-ignite   wood across an air gap from lava catches, wood beyond reach
//                 and stone / glass at the same heat never do; embers cannot
//                 heat a block past their own temperature; and the same lava
//                 after a SAVE and LOAD still lights the wood within the bound.
//   heat-freeze   water in a frozen climate skins over (one frozen cell a
//                 column, never two, gradually, mass exact); a hole melted by
//                 lava refreezes once the lava is gone; the fixture sleeps.
//   heat-bound    one grove burned twice in one process, heat off then on:
//                 heat may add to the fire, not run it away.
//   heat-ambient  a frozen and a hot climate side by side through a real
//                 dusk and dawn (the SubmitTick wake): the frozen side keeps
//                 its snow, the hot side melts all of it and sleeps; the
//                 world settles after each boundary; bad climates are refused;
//                 a fresh world fires nothing thermal on its first ticks.
//   heat-idle     an active world with nothing hot: the layer does nothing.
//   heat-plume    heat rises: a campfire lights foliage above it, never the
//                 foliage beside or below it, and burning foliage never heats
//                 foliage alight (ticked on THE tick: TickCursor).
//
// Every fixture is a SEALED STONE ROOM (floor, walls, roof): no sky, so no
// sun rule touches it, and its climate is PINNED (Simulation::
// SetHeatColumnPins) so the gates do not depend on which biome the harness
// map puts under them. The melt, ignite and freeze gates run their fixture
// TWICE from a fresh worldgen and compare the cells that changed AND a hash
// of the heat pool: heat nondeterminism must not hide until it moves a voxel.
//
// DIRECT PHASE CALLS ON PURPOSE (W2-O): sim-only CA fixtures like oil-fire --
// they test the temperature layer and the reaction path in a sealed room, and
// nothing the rest of the tick grows (mobs, debris, the player) may touch it.
// SubmitTick IS the tick's sim half, including the day/night wake heat-ambient
// depends on.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "gpu/resources.h"
#include "sim/biomes.h"
#include "sim/heat.h"
#include "sim/stream.h"
#include "sim/worldgen_run.h"
#include "sim/worldio.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- fixture plumbing ------------------------------------------------------
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

void Fill(std::vector<CellOp>& ops, const Box& b, uint32_t word) {
  for (int z = b.z0; z <= b.z1; z++)
    for (int y = b.y0; y <= b.y1; y++)
      for (int x = b.x0; x <= b.x1; x++) ops.push_back({World::SlotCellIndex({x, y, z}), word});
}
// A sealed room: stone shell one cell thick round `in`, air inside.
void Room(std::vector<CellOp>& ops, const Box& in) {
  for (int z = in.z0 - 1; z <= in.z1 + 1; z++)
    for (int y = in.y0 - 1; y <= in.y1 + 1; y++)
      for (int x = in.x0 - 1; x <= in.x1 + 1; x++)
        ops.push_back({World::SlotCellIndex({x, y, z}),
                       in.Has(x, y, z) ? 0u : (uint32_t)kMatStone});
}
std::vector<uint32_t> ChunksOf(const Box& b) {
  std::vector<uint32_t> out;
  for (int cz = (b.z0 - 1) >> 4; cz <= ((b.z1 + 1) >> 4); cz++)
    for (int cy = (b.y0 - 1) >> 4; cy <= ((b.y1 + 1) >> 4); cy++)
      for (int cx = (b.x0 - 1) >> 4; cx <= ((b.x1 + 1) >> 4); cx++)
        out.push_back(World::SlotChunkIndex({cx, cy, cz}));
  return out;
}

// The voxels of a box, read back chunk by chunk.
struct Vox {
  std::map<uint32_t, std::vector<uint32_t>> chunks;
  void Read(GpuContext& ctx, World& world, const Box& b) {
    chunks.clear();
    for (uint32_t ci : ChunksOf(b)) {
      std::vector<uint32_t>& v = chunks[ci];
      v.assign(kChunkVol, 0);
      ReadVoxelsSync(ctx, world, ci, 1, v.data(), "heatVox");
    }
  }
  uint32_t At(int x, int y, int z) const {
    auto it = chunks.find(World::SlotChunkIndex({x >> 4, y >> 4, z >> 4}));
    if (it == chunks.end()) return 0;
    return it->second[(((z & 15) * 16 + (y & 15)) * 16) + (x & 15)];
  }
};

// Eighths of H2O a cell holds: water by fullness, ice whole, snow by mass.
uint32_t WaterEighths(uint32_t w, uint32_t water, uint32_t ice, uint32_t snow) {
  const uint32_t m = w & 0xFFFu, st = (w >> 12) & 0xFu;
  if (m == water) return st + 1;
  if (m == ice) return 8;
  if (m == snow) return st < 3 ? 8 : std::min<uint32_t>(st - 2, 8);  // powder mass (common.wgsl)
  return 0;
}

// The whole heatMeta record, and the heat of one world cell.
struct HeatView {
  std::vector<uint32_t> meta, pool;
  void Read(GpuContext& ctx, World& world, bool withPool) {
    ctx.WaitIdle();
    meta.assign(kHmWords, 0);
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.heatMeta, 0, meta.data(),
                          (uint64_t)kHmWords * 4, "heatMetaRead");
    if (withPool) {
      pool.assign((size_t)kHeatPoolPages * kHeatPageWords, 0);
      rhi::ReadbackBlocking(ctx.device, ctx.queue, world.heatPool, 0, pool.data(),
                            pool.size() * 4, "heatPoolRead");
    }
  }
  uint32_t PagesInUse() const { return meta[kHmNextFresh] - meta[kHmFreeTop]; }
  void Note() const { HeatNoteRun(meta.data()); }
  // Plane-0 word of the block holding world cell (x, y, z); 0 if unpaged.
  uint32_t Word(int x, int y, int z) const {
    const uint32_t e = meta[kHmEntry + World::SlotChunkIndex({x >> 4, y >> 4, z >> 4})];
    if ((e & kHeatEntryHas) == 0 || pool.empty()) return 0;
    const uint32_t b = ((((uint32_t)z & 15) >> 1) * 8 + (((uint32_t)y & 15) >> 1)) * 8 +
                       (((uint32_t)x & 15) >> 1);
    return pool[(size_t)(e & kHeatEntryPage) * kHeatPageWords + b];
  }
  uint32_t X(int x, int y, int z) const { return Word(x, y, z) & 0xFF; }
  uint32_t Xs(int x, int y, int z) const { return (Word(x, y, z) >> 8) & 0xFF; }
  // WHY pages are still out: per paged slot, what keeps it -- an excess, a
  // target, a source, a partial sum -- and its summary / flag bits. The first
  // few, so a leak names its cause on one line instead of as a bare count.
  std::string Leftovers(const World& w, uint32_t maxList = 4) const {
    std::string out;
    uint32_t n = 0, withX = 0, withXs = 0, withE = 0, withPlane = 0, srcBit = 0;
    for (uint32_t s = 0; s < kNumChunks; s++) {
      const uint32_t e = meta[kHmEntry + s];
      if ((e & kHeatEntryHas) == 0) continue;
      n++;
      const uint32_t* p = pool.data() + (size_t)(e & kHeatEntryPage) * kHeatPageWords;
      uint32_t mx = 0, mxs = 0, me = 0, pl = 0;
      for (uint32_t b = 0; b < kHeatBlocks; b++) {
        mx = std::max(mx, p[b] & 0xFF);
        mxs = std::max(mxs, (p[b] >> 8) & 0xFF);
        me = std::max(me, (p[b] >> 16) & 0xFF);
        pl |= p[kHeatBlocks + b] | p[2 * kHeatBlocks + b];
      }
      withX += mx != 0; withXs += mxs != 0; withE += me != 0; withPlane += pl != 0;
      srcBit += (meta[kHmSummary + s] & kHsSource) != 0;
      if (n <= maxList) {
        const IVec3 wc = w.SlotToWorldChunk(s);
        out += Format(" [slot %u chunk (%d,%d,%d): X %u X* %u E %u planes %s summary %u flags %u]",
                      s, wc.x, wc.y, wc.z, mx, mxs, me, pl ? "set" : "zero",
                      meta[kHmSummary + s], meta[kHmFlags + s]);
      }
    }
    return Format("%u paged: %u with X, %u with X*, %u with E, %u with partial sums, %u "
                  "summary-source bits;", n, withX, withXs, withE, withPlane, srcBit) + out;
  }
  // A hash of the layer's CONTENT keyed by SLOT (never by page index, which
  // depends on free-stack order and is unobservable by design).
  uint64_t Hash() const {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t s = 0; s < kNumChunks; s++) {
      const uint32_t e = meta[kHmEntry + s];
      if ((e & kHeatEntryHas) == 0) continue;
      h = (h ^ s) * 1099511628211ull;
      const uint32_t* p = pool.data() + (size_t)(e & kHeatEntryPage) * kHeatPageWords;
      for (uint32_t i = 0; i < kHeatPageWords; i++) h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
  }
};

// X of the block holding world cell (x, y, z), 0 if unpaged: two 4-byte
// readbacks (the entry, then the one pool word), cheap enough to take every
// tick round a boundary.
uint32_t ProbeX(GpuContext& ctx, World& world, int x, int y, int z) {
  ctx.WaitIdle();
  const uint32_t slot = World::SlotChunkIndex({x >> 4, y >> 4, z >> 4});
  uint32_t e = 0;
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.heatMeta, (uint64_t)(kHmEntry + slot) * 4, &e,
                        4, "heatProbeEntry");
  if ((e & kHeatEntryHas) == 0) return 0;
  const uint32_t b = ((((uint32_t)z & 15) >> 1) * 8 + (((uint32_t)y & 15) >> 1)) * 8 +
                     (((uint32_t)x & 15) >> 1);
  uint32_t w = 0;
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.heatPool,
                        ((uint64_t)(e & kHeatEntryPage) * kHeatPageWords + b) * 4, &w, 4,
                        "heatProbeWord");
  return w & 0xFFu;
}

int FloorDiv16(int v) { return v >= 0 ? v / 16 : -((-v + 15) / 16); }

uint32_t AwakeIn(GpuContext& ctx, Simulation& sim, const std::vector<uint32_t>& chunks) {
  ctx.WaitIdle();
  std::vector<uint32_t> flags(kNumSlots, 0);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0, flags.data(),
                        kNumSlots * 4, "heatAwake");
  uint32_t n = 0;
  for (uint32_t ci : chunks) n += flags[ci] != 0;
  return n;
}
uint32_t AwakeAll(GpuContext& ctx, Simulation& sim) {
  ctx.WaitIdle();
  std::vector<uint32_t> flags(kNumSlots, 0);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0, flags.data(),
                        kNumSlots * 4, "heatAwakeAll");
  uint32_t n = 0;
  for (uint32_t f : flags) n += f != 0;
  return n;
}

// Dim dawn, frozen (as oil-fire): only the fixture changes the room.
struct DawnPin {
  Tuning saved;
  explicit DawnPin(int heatMode = 1) : saved(CurrentTuning()) {
    Tuning t = saved;
    t.dayNight.freeze = 1;
    t.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
    t.sim.heatMode = heatMode;
    // CA water only: an excited (MPM) splash is not a voxel, and the mass
    // audits here count voxels. The seam has its own gates.
    t.sim.fluidExciteMode = 0;
    SetCurrentTuning(t);
  }
  ~DawnPin() { SetCurrentTuning(saved); }
};
struct PinGuard {
  Simulation& sim;
  PinGuard(Simulation& s, const std::vector<Simulation::HeatColumnPin>& p) : sim(s) {
    sim.SetHeatColumnPins(p);
  }
  ~PinGuard() { sim.SetHeatColumnPins({}); }
};

void Tick(Ctx& c, uint32_t& t, const std::vector<CellOp>& ops = {}) {
  SubmitTick(c.ctx, c.world, c.sim, ++t, kDefaultSeed, {}, {}, ops, false, {6, 7, 6}, false,
             false);
}

// ---------------------------------------------------------------------------
// heat-melt
// ---------------------------------------------------------------------------
// FIXTURE (room interior x 96..139, y 120..131, z 96..111): a lava basin
// (x 96..103, y 120..127, the full depth) sealed by a stone wall at x 104 up to
// the roof; one air cell (x 105); a snow bank two high (x 106..109, z 96..102;
// snow is a powder and a taller face would slump) and an ice block four high
// (x 106..109, z 105..111); a second snow bank at x 132..138 -- 28 cells (14
// blocks, past sim.heatRadius) from the lava face. Snow is COUNTED BY ZONE
// (east of x 104 and west of x 120 is near, east of x 125 is far), so a grain
// that slides is still counted where it lies; the surface-first check is on
// the ice, which cannot slide.
// The climate is pinned at exactly 0: neither melt (above 0) nor freeze (below
// 0) can fire from the ambient alone, so every change is the lava's.
struct MeltResult {
  uint32_t nearSnow0 = 0, nearIce0 = 0, farSnow0 = 0, h2o0 = 0;
  uint32_t nearLeft = 0, farLeft = 0, h2oEnd = 0, meltTick = 0;
  uint32_t xEarly = 0, xsEarly = 0, xLate = 0, xsLate = 0, overshoot = 0;
  uint32_t interiorFirst = 0, firstMelted = 0;
  uint32_t pagesAfter = 0, awakeAfter = 0, melts = 0, refused = 0, pagesPeak = 0;
  std::string leak;
  std::vector<uint32_t> finalCells;
  uint64_t heatHash = 0;
};

MeltResult RunMelt(Ctx& c, int kTicks, int kCool) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const uint32_t lava = MatId(c, "lava"), snow = MatId(c, "snow"), ice = MatId(c, "ice"),
                 water = MatId(c, "water");
  const Box room{96, 120, 96, 139, 131, 111};
  const Box lavaB{96, 120, 96, 103, 127, 111}, wallB{104, 120, 96, 104, 131, 111};
  const Box snowB{106, 120, 96, 109, 121, 102}, iceB{106, 120, 105, 109, 123, 111};
  const Box farB{132, 120, 96, 138, 121, 102};
  const Box nearZone{105, 120, 96, 119, 131, 111}, farZone{126, 120, 96, 139, 131, 111};
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  MeltResult r;
  // Separate ticks: two cell ops on ONE cell in one tick keep the FIRST
  // (oprecord.h), so the room's air would eat whatever is placed in it.
  std::vector<CellOp> shell, build;
  Room(shell, room);
  Fill(build, wallB, kMatStone);
  Fill(build, snowB, snow);
  Fill(build, iceB, ice);
  Fill(build, farB, snow);
  std::vector<CellOp> pour;
  Fill(pour, lavaB, lava | kFull);
  uint32_t t = 52000;
  Tick(c, t, shell);
  Tick(c, t, build);
  ctx.WaitIdle();
  Vox v;
  v.Read(ctx, world, room);
  auto count = [&](const Box& b, uint32_t mat) {
    uint32_t n = 0;
    for (int z = b.z0; z <= b.z1; z++)
      for (int y = b.y0; y <= b.y1; y++)
        for (int x = b.x0; x <= b.x1; x++) n += (v.At(x, y, z) & 0xFFFu) == mat;
    return n;
  };
  auto h2o = [&]() {
    uint32_t n = 0;
    for (int z = room.z0; z <= room.z1; z++)
      for (int y = room.y0; y <= room.y1; y++)
        for (int x = wallB.x1 + 1; x <= room.x1; x++)
          n += WaterEighths(v.At(x, y, z), water, ice, snow);
    return n;
  };
  r.nearSnow0 = count(snowB, snow);
  r.nearIce0 = count(iceB, ice);
  r.farSnow0 = count(farZone, snow);
  r.h2o0 = h2o();
  // Which cells of the ice block were INTERIOR (all six faces ice) at the start.
  auto interior = [&](int x, int y, int z) {
    const int d[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (auto& q : d)
      if (!iceB.Has(x + q[0], y + q[1], z + q[2])) return false;
    return true;
  };
  HeatView hv;
  Tick(c, t, pour);
  for (int i = 1; i < kTicks; i++) {
    Tick(c, t);
    if (i == 3 || i == 120) {
      hv.Read(ctx, world, true);
      const uint32_t x = hv.X(106, 120, 100), xs = hv.Xs(106, 120, 100);
      if (i == 3) { r.xEarly = x; r.xsEarly = xs; } else { r.xLate = x; r.xsLate = xs; }
      if (x > xs && i == 120) r.overshoot++;
    }
    if (i % 15 == 0) {
      v.Read(ctx, world, room);
      const uint32_t left = count(nearZone, snow) + count(nearZone, ice);
      if (r.firstMelted == 0 && count(iceB, ice) < r.nearIce0) {
        for (int z = iceB.z0; z <= iceB.z1; z++)
          for (int y = iceB.y0; y <= iceB.y1; y++)
            for (int x = iceB.x0; x <= iceB.x1; x++)
              if ((v.At(x, y, z) & 0xFFFu) != ice) {
                r.firstMelted++;
                r.interiorFirst += interior(x, y, z);
              }
      }
      if (left == 0 && r.meltTick == 0) r.meltTick = (uint32_t)i;
    }
  }
  v.Read(ctx, world, room);
  r.nearLeft = count(nearZone, snow) + count(nearZone, ice);
  r.farLeft = count(farZone, snow);
  r.h2oEnd = h2o();
  hv.Read(ctx, world, true);
  hv.Note();
  r.melts = hv.meta[kHmMelts];
  r.refused = hv.meta[kHmRefused];
  r.pagesPeak = hv.meta[kHmPagesPeak];
  r.heatHash = hv.Hash();
  for (int z = room.z0; z <= room.z1; z++)
    for (int y = room.y0; y <= room.y1; y++)
      for (int x = room.x0; x <= room.x1; x++) r.finalCells.push_back(v.At(x, y, z) & 0xFFFFu);
  // The lava goes (to stone, through the queue like any edit): every page
  // must come back and the room must sleep.
  std::vector<CellOp> quench;
  Fill(quench, lavaB, kMatStone);
  Tick(c, t, quench);
  for (int i = 0; i < kCool; i++) Tick(c, t);
  hv.Read(ctx, world, true);
  r.pagesAfter = hv.PagesInUse();
  if (r.pagesAfter) r.leak = hv.Leftovers(world);
  r.awakeAfter = AwakeIn(ctx, sim, ChunksOf(room));
  return r;
}

Status GateHeatMelt(Ctx& c, std::string& detail) {
  DawnPin dawn;
  PinGuard pins(c.sim, {{90, 90, 145, 117, 0, 0}});
  const int kTicks = (int)BaselineNumber("heat.meltRunTicks", 900);
  const int kCool = (int)BaselineNumber("heat.coolTicks", 400);
  const MeltResult a = RunMelt(c, kTicks, kCool);
  const MeltResult b = RunMelt(c, kTicks, kCool);
  const double meltMax = BaselineNumber("heat.meltTicksMax", 900);
  RecordObserved("heat.meltTicksObserved", (double)a.meltTick);
  RecordObserved("heat.meltPagesPeak", (double)a.pagesPeak);
  const bool melted = a.nearLeft == 0 && a.meltTick > 0 && a.meltTick <= meltMax;
  const bool farKept = a.farLeft == a.farSnow0;
  const bool massExact = a.h2oEnd == a.h2o0;
  const bool gradual = a.xEarly < a.xsEarly && a.xsEarly > 0 && a.xLate == a.xsLate &&
                       a.overshoot == 0;
  const bool surfaceFirst = a.firstMelted > 0 && a.interiorFirst * 4 <= a.firstMelted;
  const bool freed = a.pagesAfter == 0 && a.awakeAfter == 0;
  const bool twice = a.finalCells == b.finalCells && a.heatHash == b.heatHash &&
                     a.melts == b.melts;
  const bool ok = a.nearSnow0 > 0 && a.nearIce0 > 0 && melted && farKept && massExact &&
                  gradual && surfaceFirst && freed && twice && a.refused == 0;
  detail = Format(
      "near %u snow + %u ice melted by tick %u (max %.0f) %s; far bank %u/%u kept %s; "
      "H2O eighths %u -> %u %s; X at the bank t3 %u of target %u, t120 %u of %u %s; "
      "first melt %u cells, %u interior %s; %u melts; after the lava: %u pages, %u "
      "chunks awake %s; run twice: cells %s, heat hash %016llx vs %016llx %s; refused %u",
      a.nearSnow0, a.nearIce0, a.meltTick, meltMax, melted ? "OK" : "FAIL", a.farLeft,
      a.farSnow0, farKept ? "OK" : "FAIL", a.h2o0, a.h2oEnd, massExact ? "EXACT" : "LOST/MINTED",
      a.xEarly, a.xsEarly, a.xLate, a.xsLate, gradual ? "gradual" : "NOT GRADUAL",
      a.firstMelted, a.interiorFirst, surfaceFirst ? "surface-first" : "INTERIOR",
      a.melts, a.pagesAfter, a.awakeAfter, freed ? "freed+asleep" : "LEAK",
      a.finalCells == b.finalCells ? "same" : "DIFFER", (unsigned long long)a.heatHash,
      (unsigned long long)b.heatHash, twice ? "OK" : "NONDETERMINISTIC", a.refused);
  if (!a.leak.empty()) detail += "; LEFT OUT: " + a.leak;
  std::printf("heat-melt: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// heat-ignite
// ---------------------------------------------------------------------------
// FIXTURE (room interior x 96..147, y 120..131, z 96..119), split by a stone
// partition at x 128. WEST: the lava basin of heat-melt (x 96..103, eight
// high, the full depth) behind its wall (x 104); one air cell (x 105); a 2x2
// wood post six high at x 106..107, z 106..107 (never touching lava: the wall
// is between, so `lava + tag:organic` cannot light it); a stone post and a
// glass post at the same distance (z 98..99 and z 114..115); a wood post at
// x 124..125 (21 cells, past sim.heatRadius, from the lava face). EAST: an
// EMBER block (x 131..136, z 112..117, six high) with a wood post one cell
// beside it (x 138): the embers' heat may not lift that block past their own
// 140 (heat.h ceiling), so wood's 150 is out of reach of heat however long
// they burn. Climate pinned at 10.
struct IgniteResult {
  uint32_t lightTick = 0, farWood0 = 0, farWood = 0, stoneGlass0 = 0, stoneGlass = 0;
  int tNear = -999, tEmber = -999;
  uint32_t ignites = 0, refused = 0;
  std::vector<uint32_t> finalCells;
  uint64_t heatHash = 0;
  // The F1 probe (heatBegin), pointed at a block beside the lava wall whose X
  // has long settled: the readout must name that block and carry its X / X*.
  bool probeOk = false;
  uint32_t probeWord = 0, probeX = 0;
};

const Box kIgRoom{96, 120, 96, 147, 131, 119};
const Box kIgLava{96, 120, 96, 103, 127, 119}, kIgWall{104, 120, 96, 104, 131, 119};
const Box kIgWood{106, 120, 106, 107, 125, 107}, kIgStone{106, 120, 98, 107, 125, 99};
const Box kIgGlass{106, 120, 114, 107, 125, 115}, kIgFar{124, 120, 106, 125, 125, 107};
const Box kIgPart{128, 120, 96, 128, 131, 119};
const Box kIgEmber{131, 120, 112, 136, 125, 117}, kIgEmberWood{138, 120, 114, 138, 125, 115};

void BuildIgnite(Ctx& c, std::vector<CellOp>& shell, std::vector<CellOp>& build,
                 std::vector<CellOp>& pour) {
  const uint32_t wood = MatId(c, "wood"), glass = MatId(c, "glass"), ember = MatId(c, "ember"),
                 lava = MatId(c, "lava");
  Room(shell, kIgRoom);
  Fill(build, kIgWall, kMatStone);
  Fill(build, kIgPart, kMatStone);
  Fill(build, kIgWood, wood);
  Fill(build, kIgStone, kMatStone);
  Fill(build, kIgGlass, glass);
  Fill(build, kIgFar, wood);
  Fill(build, kIgEmberWood, wood);
  Fill(pour, kIgLava, lava | kFull);
  Fill(pour, kIgEmber, ember);
}

IgniteResult RunIgnite(Ctx& c, int kTicks) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const uint32_t wood = MatId(c, "wood"), glass = MatId(c, "glass");
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  std::vector<CellOp> shell, build, pour;
  BuildIgnite(c, shell, build, pour);
  uint32_t t = 54000;
  Tick(c, t, shell);
  Tick(c, t, build);
  Tick(c, t, pour);
  ctx.WaitIdle();
  IgniteResult r;
  Vox v;
  auto count = [&](const Box& b, uint32_t mat) {
    uint32_t n = 0;
    for (int z = b.z0; z <= b.z1; z++)
      for (int y = b.y0; y <= b.y1; y++)
        for (int x = b.x0; x <= b.x1; x++) n += (v.At(x, y, z) & 0xFFFu) == mat;
    return n;
  };
  v.Read(ctx, world, kIgRoom);
  r.farWood0 = count(kIgFar, wood);
  r.stoneGlass0 = count(kIgStone, kMatStone) + count(kIgGlass, glass);
  const uint32_t near0 = count(kIgWood, wood);
  HeatView hv;
  int tMax = -999, tEmberMax = -999;
  const int px = 105, py = 124, pz = 102;   // the air gap, beside the wall
  sim.SetHeatProbe(true, px, py, pz);
  for (int i = 0; i < kTicks; i++) {
    Tick(c, t);
    if (i % 10 == 0) {
      hv.Read(ctx, world, true);
      // The post's HOTTEST block. Since heat rises (sim.heatUpGain), the
      // post's bottom block -- level with the lava's floor, every lava
      // block level with it or above -- reads far cooler than its top,
      // which has lava below it: the claim is that the post catches.
      for (int y = kIgWood.y0; y <= kIgWood.y1; y += 2)
        tMax = std::max(tMax, (int)hv.X(106, y, 106) + 10);
      tEmberMax = std::max(tEmberMax, (int)hv.X(138, 121, 114) + 10);
      if (r.lightTick == 0) {
        v.Read(ctx, world, kIgRoom);
        if (count(kIgWood, wood) < near0) r.lightTick = (uint32_t)i + 1;
      }
    }
  }
  v.Read(ctx, world, kIgRoom);
  r.farWood = count(kIgFar, wood);
  r.stoneGlass = count(kIgStone, kMatStone) + count(kIgGlass, glass);
  r.tNear = tMax;
  r.tEmber = tEmberMax;
  hv.Read(ctx, world, true);
  hv.Note();
  {
    const uint32_t w = hv.meta[kHmProbeX];
    const uint32_t tag = ((uint32_t)(px >> 1) & 31u) | (((uint32_t)(py >> 1) & 31u) << 5) |
                         (((uint32_t)(pz >> 1) & 31u) << 10);
    r.probeWord = w;
    r.probeX = hv.X(px, py, pz);
    r.probeOk = (w >> kHeatProbeTagShift) == tag && (w & 0x10000u) != 0 &&
                (w & 0xFFu) == r.probeX && ((w >> 8) & 0xFFu) == hv.Xs(px, py, pz) && r.probeX > 0;
  }
  sim.SetHeatProbe(false, 0, 0, 0);
  r.ignites = hv.meta[kHmIgnites];
  r.refused = hv.meta[kHmRefused];
  r.heatHash = hv.Hash();
  for (int z = kIgRoom.z0; z <= kIgRoom.z1; z++)
    for (int y = kIgRoom.y0; y <= kIgRoom.y1; y++)
      for (int x = kIgRoom.x0; x <= kIgRoom.x1; x++)
        r.finalCells.push_back(v.At(x, y, z) & 0xFFFFu);
  return r;
}

// The same lava and wood through a SAVE and a LOAD before the wood has caught:
// heat is not saved, so the loaded world's layer is EMPTY -- the chunks are
// woken by their fill, caMask finds the lava, and the wood must still light
// within the same bound (docs/PLAN_temperature.md §9, the owner's requirement).
uint32_t RunIgniteSaveLoad(Ctx& c, int kTicks, uint32_t& pagesAfterLoad) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Stream& stream = c.stream;
  const char* kPath = "selftest_heat.svd";
  std::filesystem::remove_all(kPath);
  stream.Store().Unbind();
  stream.OnRegen();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  std::vector<CellOp> shell, build, pour;
  BuildIgnite(c, shell, build, pour);
  uint32_t t = 56000;
  Tick(c, t, shell);
  stream.FoldSnapshot();
  Tick(c, t, build);
  stream.FoldSnapshot();
  Tick(c, t, pour);
  stream.FoldSnapshot();
  for (int i = 0; i < 4; i++) {
    Tick(c, t);
    stream.FoldSnapshot();
  }
  ctx.WaitIdle();
  bool ok = SaveWorld(ctx, world, stream, kPath, c.mats);
  ok = ok && LoadWorld(ctx, world, sim, stream, kPath, c.mats);
  const uint32_t wood = MatId(c, "wood");
  Vox v;
  auto nearWood = [&]() {
    v.Read(ctx, world, kIgRoom);
    uint32_t n = 0;
    for (int z = kIgWood.z0; z <= kIgWood.z1; z++)
      for (int y = kIgWood.y0; y <= kIgWood.y1; y++)
        for (int x = kIgWood.x0; x <= kIgWood.x1; x++) n += (v.At(x, y, z) & 0xFFFu) == wood;
    return n;
  };
  const uint32_t near0 = ok ? nearWood() : 0;
  uint32_t lit = 0;
  pagesAfterLoad = 0;
  HeatView hv;
  for (int i = 0; ok && i < kTicks && lit == 0; i++) {
    Tick(c, t);
    stream.FoldSnapshot();
    if (i == 10) {
      hv.Read(ctx, world, false);
      pagesAfterLoad = hv.PagesInUse();
    }
    if (i % 10 == 9 && nearWood() < near0) lit = (uint32_t)i + 1;
  }
  stream.Store().Unbind();
  std::filesystem::remove_all(kPath);
  return ok && near0 > 0 ? lit : 0;
}

Status GateHeatIgnite(Ctx& c, std::string& detail) {
  DawnPin dawn;
  PinGuard pins(c.sim, {{90, 90, 152, 125, 10, 0}});
  const int kTicks = (int)BaselineNumber("heat.igniteRunTicks", 600);
  const IgniteResult a = RunIgnite(c, kTicks);
  const IgniteResult b = RunIgnite(c, kTicks);
  uint32_t pagesAfterLoad = 0;
  const uint32_t litAfterLoad = RunIgniteSaveLoad(c, kTicks, pagesAfterLoad);
  const double litMax = BaselineNumber("heat.igniteTicksMax", 450);
  RecordObserved("heat.igniteTicksObserved", (double)a.lightTick);
  RecordObserved("heat.igniteAfterLoadObserved", (double)litAfterLoad);
  int woodAbove = 150;
  for (const HeatTransition& h : c.mats[MatId(c, "wood")].thermal.transitions)
    if (h.kind == kHeatKindIgnite) woodAbove = h.threshold;
  int emberEmit = (int)c.mats[MatId(c, "ember")].thermal.emit;
  const bool lit = a.lightTick > 0 && a.lightTick <= litMax;
  const bool hotEnough = a.tNear > woodAbove;
  const bool farKept = a.farWood == a.farWood0;
  const bool inertKept = a.stoneGlass == a.stoneGlass0;
  const bool emberCap = a.tEmber <= emberEmit && emberEmit < woodAbove;
  const bool loadOk = litAfterLoad > 0 && litAfterLoad <= litMax && pagesAfterLoad > 0;
  const bool twice = a.finalCells == b.finalCells && a.heatHash == b.heatHash;
  const bool ok = lit && hotEnough && farKept && inertKept && emberCap && loadOk && twice &&
                  a.refused == 0 && a.probeOk;
  detail = Format(
      "wood across a gap from lava lit at tick %u (max %.0f) %s, its block reached %d "
      "(wood ignites above %d) %s; far wood %u/%u kept %s; stone+glass %u/%u kept %s; "
      "wood beside embers peaked at %d (embers emit %d) %s; after save+load: %u pages by "
      "tick 10, lit at tick %u %s; run twice: cells %s, heat hash %016llx vs %016llx %s; "
      "%u heat ignitions, refused %u; F1 probe word %08x (pool X %u) %s",
      a.lightTick, litMax, lit ? "OK" : "FAIL", a.tNear, woodAbove, hotEnough ? "OK" : "FAIL",
      a.farWood, a.farWood0, farKept ? "OK" : "FAIL", a.stoneGlass, a.stoneGlass0,
      inertKept ? "OK" : "FAIL", a.tEmber, emberEmit, emberCap ? "capped" : "OVER THE CAP",
      pagesAfterLoad, litAfterLoad, loadOk ? "OK" : "FAIL",
      a.finalCells == b.finalCells ? "same" : "DIFFER", (unsigned long long)a.heatHash,
      (unsigned long long)b.heatHash, twice ? "OK" : "NONDETERMINISTIC", a.ignites, a.refused,
      a.probeWord, a.probeX, a.probeOk ? "OK" : "FAIL");
  std::printf("heat-ignite: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// heat-freeze
// ---------------------------------------------------------------------------
// FIXTURE (room interior x 96..131, y 120..131, z 96..111): a basin of full
// water (x 110..125, z 98..109, four deep) with open air above it under the
// roof, and the lava basin of heat-melt at x 96..103 behind its wall (x 104),
// six cells from the water. Climate pinned at -20 (frozen). Phase 1, lava
// present: the water near it stays open, the rest skins over. Phase 2, lava
// replaced with stone: the hole skins over too. Asserted: ONE frozen cell on
// top of every column (never two), the frozen fraction early is small
// (gradual), H2O eighths exact, the room asleep at the end.
struct FreezeResult {
  uint32_t cols = 0, frozenTop = 0, doubleFrozen = 0, earlyFrozen = 0, h2o0 = 0, h2oEnd = 0;
  uint32_t holeOpen = 0, holeAfter = 0, awake = 0, pages = 0, freezes = 0;
  std::vector<uint32_t> finalCells;
  uint64_t heatHash = 0;
};

FreezeResult RunFreeze(Ctx& c, int kPhase1, int kPhase2, int kEarly) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const uint32_t lava = MatId(c, "lava"), ice = MatId(c, "ice"), water = MatId(c, "water"),
                 snow = MatId(c, "snow");
  const Box room{96, 120, 96, 131, 131, 111};
  const Box lavaB{96, 120, 96, 103, 127, 111}, wallB{104, 120, 96, 104, 131, 111};
  const Box poolB{110, 120, 98, 125, 123, 109};
  const Box walls{109, 120, 97, 126, 124, 110};  // the basin's stone rim
  const Box hollowB{110, 120, 98, 125, 124, 109};  // open to the room above
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  std::vector<CellOp> shell, build, hollow, pour;
  Room(shell, room);
  Fill(build, wallB, kMatStone);
  Fill(build, walls, kMatStone);
  Fill(hollow, hollowB, 0u);
  Fill(pour, poolB, water | kFull);
  Fill(pour, lavaB, lava | kFull);
  uint32_t t = 58000;
  Tick(c, t, shell);
  Tick(c, t, build);
  Tick(c, t, hollow);
  Tick(c, t, pour);
  ctx.WaitIdle();
  FreezeResult r;
  Vox v;
  auto h2o = [&]() {
    uint32_t n = 0;
    for (int z = room.z0; z <= room.z1; z++)
      for (int y = room.y0; y <= room.y1; y++)
        for (int x = wallB.x1 + 1; x <= room.x1; x++)
          n += WaterEighths(v.At(x, y, z), water, ice, snow);
    return n;
  };
  auto frozen = [&](uint32_t w) {
    const uint32_t m = w & 0xFFFu;
    return m == ice || m == snow;
  };
  auto topFrozen = [&]() {
    uint32_t n = 0;
    for (int z = poolB.z0; z <= poolB.z1; z++)
      for (int x = poolB.x0; x <= poolB.x1; x++) n += frozen(v.At(x, poolB.y1, z));
    return n;
  };
  v.Read(ctx, world, room);
  r.h2o0 = h2o();
  for (int i = 0; i < kPhase1; i++) {
    Tick(c, t);
    if (i == kEarly) {
      v.Read(ctx, world, room);
      r.earlyFrozen = topFrozen();
    }
  }
  v.Read(ctx, world, room);
  // The hole: top cells within 2 cells of the wall side still liquid.
  for (int z = poolB.z0; z <= poolB.z1; z++)
    for (int x = poolB.x0; x <= poolB.x0 + 1; x++) r.holeOpen += !frozen(v.At(x, poolB.y1, z));
  std::vector<CellOp> quench;
  Fill(quench, lavaB, kMatStone);
  Tick(c, t, quench);
  for (int i = 0; i < kPhase2; i++) Tick(c, t);
  v.Read(ctx, world, room);
  for (int z = poolB.z0; z <= poolB.z1; z++)
    for (int x = poolB.x0; x <= poolB.x1; x++) {
      r.cols++;
      r.frozenTop += frozen(v.At(x, poolB.y1, z));
      for (int y = poolB.y0; y < poolB.y1; y++) r.doubleFrozen += frozen(v.At(x, y, z));
    }
  for (int z = poolB.z0; z <= poolB.z1; z++)
    for (int x = poolB.x0; x <= poolB.x0 + 1; x++) r.holeAfter += !frozen(v.At(x, poolB.y1, z));
  r.h2oEnd = h2o();
  r.awake = AwakeIn(ctx, sim, ChunksOf(room));
  HeatView hv;
  hv.Read(ctx, world, true);
  r.pages = hv.PagesInUse();
  hv.Note();
  r.freezes = hv.meta[kHmFreezes];
  r.heatHash = hv.Hash();
  for (int z = room.z0; z <= room.z1; z++)
    for (int y = room.y0; y <= room.y1; y++)
      for (int x = room.x0; x <= room.x1; x++) r.finalCells.push_back(v.At(x, y, z) & 0xFFFFu);
  return r;
}

Status GateHeatFreeze(Ctx& c, std::string& detail) {
  DawnPin dawn;
  PinGuard pins(c.sim, {{90, 90, 140, 117, -20, 0}});
  const int kPhase1 = (int)BaselineNumber("heat.freezePhase1Ticks", 1500);
  const int kPhase2 = (int)BaselineNumber("heat.freezePhase2Ticks", 1500);
  const int kEarly = (int)BaselineNumber("heat.freezeEarlyTick", 60);
  const FreezeResult a = RunFreeze(c, kPhase1, kPhase2, kEarly);
  const FreezeResult b = RunFreeze(c, kPhase1, kPhase2, kEarly);
  const double topMin = BaselineNumber("heat.freezeTopMin", 0.97);
  const double earlyMax = BaselineNumber("heat.freezeEarlyMax", 0.5);
  const double topFrac = a.cols ? (double)a.frozenTop / a.cols : 0.0;
  const double earlyFrac = a.cols ? (double)a.earlyFrozen / a.cols : 1.0;
  RecordObserved("heat.freezeTopObserved", topFrac);
  RecordObserved("heat.freezeEarlyObserved", earlyFrac);
  const bool skinned = topFrac >= topMin && a.doubleFrozen == 0;
  const bool gradual = earlyFrac <= earlyMax;
  const bool massExact = a.h2oEnd == a.h2o0;
  const bool hole = a.holeOpen > 0 && a.holeAfter * 4 <= a.holeOpen;
  const bool asleep = a.awake == 0 && a.pages == 0;
  const bool twice = a.finalCells == b.finalCells && a.heatHash == b.heatHash;
  const bool ok = a.cols > 0 && skinned && gradual && massExact && hole && asleep && twice;
  detail = Format(
      "%u/%u columns frozen on top (%.2f, need %.2f), %u frozen cells below a top %s; "
      "%.2f frozen at tick %d (max %.2f) %s; H2O eighths %u -> %u %s; lava kept %u top "
      "cells open, %u still open after it went %s; %u chunks awake, %u pages %s; %u "
      "freezes; run twice: cells %s, heat hash %016llx vs %016llx %s",
      a.frozenTop, a.cols, topFrac, topMin, a.doubleFrozen, skinned ? "OK" : "FAIL",
      earlyFrac, kEarly, earlyMax, gradual ? "gradual" : "TOO FAST", a.h2o0, a.h2oEnd,
      massExact ? "EXACT" : "LOST/MINTED", a.holeOpen, a.holeAfter, hole ? "OK" : "FAIL",
      a.awake, a.pages, asleep ? "asleep" : "AWAKE", a.freezes,
      a.finalCells == b.finalCells ? "same" : "DIFFER", (unsigned long long)a.heatHash,
      (unsigned long long)b.heatHash, twice ? "OK" : "NONDETERMINISTIC");
  std::printf("heat-freeze: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// heat-bound
// ---------------------------------------------------------------------------
// FIXTURE (room interior x 96..143, y 120..139, z 96..143, a 48 x 20 x 48 hall):
// a 4 x 4 grove -- 2x2 wood trunks ten high on a grass floor, 6x6x4 leaf
// crowns -- lit by ONE ember at the foot of a corner tree. Burned to the end
// twice in one process, sim.heatMode 0 then 1, climate pinned at 12, every
// trunk lit at its foot (a large fire). Heat may ADD ignitions, but the fire
// may not run away: the WINDOW's peak awake chunks within
// heat.boundAwakeRatioMax of the heat-off burn (heat keeps a one-chunk halo of
// relaxing blocks awake round a fire, so this is not 1.0), the fuel burnt
// within heat.boundFuelRatioMax of it, heat ignitions at most
// heat.boundHeatShareMax of all the fuel burnt, NO pool refusal, and after the
// burn and a cool-down every page is back and the hall is asleep.
struct BoundResult {
  uint32_t fuel0 = 0, fuelEnd = 0, peakAwake = 0, burnTicks = 0, ignites = 0;
  uint32_t refused = 0, pagesPeak = 0, pagesAfter = 0, awakeAfter = 0, srcPeak = 0;
  uint32_t coolTicks = 0;
  std::string leak;
};

BoundResult RunBound(Ctx& c, int heatMode, int kMax, int kCool) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Tuning tn = CurrentTuning();
  tn.sim.heatMode = heatMode;
  SetCurrentTuning(tn);
  const uint32_t wood = MatId(c, "wood"), leaves = MatId(c, "leaves"), grass = MatId(c, "grass"),
                 ember = MatId(c, "ember"), dirt = MatId(c, "dirt");
  const Box hall{96, 120, 96, 143, 139, 143};
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  std::vector<CellOp> build, grove;
  Room(build, hall);
  Fill(grove, {hall.x0, hall.y0, hall.z0, hall.x1, hall.y0, hall.z1}, dirt);
  Fill(grove, {hall.x0, hall.y0 + 1, hall.z0, hall.x1, hall.y0 + 1, hall.z1}, grass);
  for (int tz = 0; tz < 4; tz++)
    for (int tx = 0; tx < 4; tx++) {
      const int x = hall.x0 + 6 + tx * 11, z = hall.z0 + 6 + tz * 11;
      // The trunk FIRST: where it runs up through the crown the first op wins.
      Fill(grove, {x - 1, hall.y0 + 2, z - 1, x, hall.y0 + 11, z}, wood);
      Fill(grove, {x - 3, hall.y0 + 9, z - 3, x + 2, hall.y0 + 12, z + 2}, leaves);
    }
  // A LARGE fire: a 2x2 of embers against the foot of every one of the 16
  // trunks, so the whole grove burns at once.
  std::vector<CellOp> light;
  for (int tz = 0; tz < 4; tz++)
    for (int tx = 0; tx < 4; tx++) {
      const int x = hall.x0 + 6 + tx * 11, z = hall.z0 + 6 + tz * 11;
      Fill(light, {x + 1, hall.y0 + 2, z - 1, x + 2, hall.y0 + 3, z}, ember);
    }
  uint32_t t = 60000;
  Tick(c, t, build);
  Tick(c, t, grove);
  ctx.WaitIdle();
  const std::vector<uint32_t> chunks = ChunksOf(hall);
  Vox v;
  auto fuel = [&]() {
    v.Read(ctx, world, hall);
    uint32_t n = 0;
    for (int z = hall.z0; z <= hall.z1; z++)
      for (int y = hall.y0; y <= hall.y1; y++)
        for (int x = hall.x0; x <= hall.x1; x++) {
          const uint32_t m = v.At(x, y, z) & 0xFFFu;
          n += m == wood || m == leaves || m == grass;
        }
    return n;
  };
  BoundResult r;
  r.fuel0 = fuel();
  HeatView hv;
  hv.Read(ctx, world, false);
  const uint32_t ign0 = hv.meta[kHmIgnites];
  Tick(c, t, light);
  uint32_t quiet = 0;
  for (int i = 0; i < kMax; i++) {
    Tick(c, t);
    if (i % 20 == 0) {
      r.peakAwake = std::max(r.peakAwake, AwakeAll(ctx, sim));
      const uint32_t f = fuel();
      quiet = f == r.fuelEnd ? quiet + 1 : 0;
      r.fuelEnd = f;
      if (quiet >= 10) {  // 200 ticks with no fuel lost: the fire is out
        r.burnTicks = (uint32_t)i;
        break;
      }
    }
  }
  if (r.burnTicks == 0) r.burnTicks = (uint32_t)kMax;
  hv.Read(ctx, world, false);
  hv.Note();
  r.ignites = hv.meta[kHmIgnites] - ign0;
  r.refused = hv.meta[kHmRefused];
  r.pagesPeak = hv.meta[kHmPagesPeak];
  r.srcPeak = hv.meta[kHmSrcPeak];
  // Cool down until the hall sleeps (smoke and ash settle for a while after
  // the last flame), then a margin: every page must be back by then.
  for (int i = 0; i < kCool; i++) {
    Tick(c, t);
    if (i % 50 == 49 && AwakeIn(ctx, sim, chunks) == 0) {
      r.coolTicks = (uint32_t)i + 1;
      break;
    }
  }
  for (int i = 0; i < 60; i++) Tick(c, t);
  hv.Read(ctx, world, true);
  r.pagesAfter = hv.PagesInUse();
  if (r.pagesAfter) r.leak = hv.Leftovers(world);
  r.awakeAfter = AwakeIn(ctx, sim, chunks);
  return r;
}

Status GateHeatBound(Ctx& c, std::string& detail) {
  DawnPin dawn;
  PinGuard pins(c.sim, {{90, 90, 150, 150, 12, 0}});
  const int kMax = (int)BaselineNumber("heat.boundMaxTicks", 4000);
  const int kCool = (int)BaselineNumber("heat.boundCoolTicksMax", 4000);
  const BoundResult off = RunBound(c, 0, kMax, kCool);
  const BoundResult on = RunBound(c, 1, kMax, kCool);
  const double awakeMax = BaselineNumber("heat.boundAwakeRatioMax", 1.5);
  const double fuelMax = BaselineNumber("heat.boundFuelRatioMax", 1.25);
  const double shareMax = BaselineNumber("heat.boundHeatShareMax", 0.25);
  const double awakeRatio = off.peakAwake ? (double)on.peakAwake / off.peakAwake : 99.0;
  const uint32_t burnt = on.fuel0 > on.fuelEnd ? on.fuel0 - on.fuelEnd : 0;
  const uint32_t burntOff = off.fuel0 > off.fuelEnd ? off.fuel0 - off.fuelEnd : 0;
  const double fuelRatio = burntOff ? (double)burnt / burntOff : 99.0;
  RecordObserved("heat.boundFuelRatioObserved", fuelRatio);
  const double share = burnt ? (double)on.ignites / burnt : 0.0;
  RecordObserved("heat.boundAwakeRatioObserved", awakeRatio);
  RecordObserved("heat.boundHeatShareObserved", share);
  RecordObserved("heat.boundPagesPeak", (double)on.pagesPeak);
  const bool spread = off.fuel0 > 0 && burnt > 0;
  const bool bounded = awakeRatio <= awakeMax && share <= shareMax && fuelRatio <= fuelMax;
  const bool clean = on.refused == 0 && on.pagesAfter == 0 && on.awakeAfter == 0 &&
                     off.awakeAfter == 0;
  const bool ok = spread && bounded && clean;
  detail = Format(
      "grove of %u fuel cells: heat off burnt %u in %u ticks, peak %u chunks awake in the "
      "window; heat on burnt %u (ratio %.2f, max %.2f) in %u ticks, peak %u awake (ratio "
      "%.2f, max %.2f), %u heat ignitions (%.2f of the fuel burnt, max %.2f) %s; pool peak "
      "%u pages, refused %u, source list peak %u; asleep after %u/%u cooling ticks: %u "
      "pages, %u/%u hall chunks awake %s",
      off.fuel0, burntOff, off.burnTicks, off.peakAwake, burnt, fuelRatio, fuelMax,
      on.burnTicks, on.peakAwake, awakeRatio, awakeMax, on.ignites, share, shareMax,
      bounded ? "BOUNDED" : "RUNAWAY", on.pagesPeak, on.refused, on.srcPeak, on.coolTicks,
      off.coolTicks, on.pagesAfter, on.awakeAfter, off.awakeAfter,
      clean ? "clean" : "LEAK/REFUSED");
  if (!on.leak.empty()) detail += "; LEFT OUT: " + on.leak;
  std::printf("heat-bound: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// heat-ambient
// ---------------------------------------------------------------------------
// (A) One room split in two by a stone wall: the WEST half pinned to a frozen
// climate (-24 +- 12: day -12, night -36), the EAST half to a hot one (24 +-
// 20: day 44, night 4). Snow and ice in each. The day is shortened to one
// minute and NOT frozen, so SubmitTick's dusk/dawn wake-all runs for real.
// Asserted: the west keeps every cell of its snow and ice through dusk, night
// and dawn; the east melts ALL of its (it is above 0 day and night) and its
// chunks sleep after; one tick past each boundary plus heat.ambientSettleTicks
// the whole window is back under heat.ambientAwakeMax awake chunks.
// (B) The shipped biome files pass the climate checks, and a biome whose
// night freezes and day thaws, and one whose day reaches an ignition point,
// are REFUSED by the loader.
// (C) A fresh harness world fires no thermal transition in its first
// heat.freshTicks ticks -- the worldgen ice rule and the climate checks are
// what make that true (no cold lake freezes over on load, no snow melts).
// (D) The same over the REAL map's frozen north: the harness window holds no
// frozen climate at all, so (C) alone never sees the tundra's snow skin, its
// iced tarns or a snow cap. The default map is loaded, the window centred on
// its densest tundra, a day pinned (the warm phase, the one that would melt
// what the biome generates) and heat.freshTicks ticked: zero firings, or a
// biome melts its own landscape / a lake freezes over on load.
// (E) THE CEILING THROUGH A DAWN: a lava pocket in a desert-pinned room (night
// 4, day 44) behind a stone wall. Its neighbour block's X settles at night to
// ~(230 - 4); were X carried unchanged into the day, the CA would read
// 44 + 226 = 270 > 230 at dawn -- hotter than the hottest source in reach.
// Sampled every tick round the dawn: ambient + X never passes the lava's emit,
// and the night X was high enough that the check had teeth.
Status GateHeatAmbient(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const uint32_t snow = MatId(c, "snow"), ice = MatId(c, "ice");
  // ---- (C) first: a fresh world, nothing pinned, the real climates ----
  uint32_t freshFires = 0, tundraCols = 0;
  {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    HeatView hv;
    hv.Read(ctx, world, false);
    const uint32_t f0 = hv.meta[kHmMelts] + hv.meta[kHmIgnites] + hv.meta[kHmFreezes];
    const int kFresh = (int)BaselineNumber("heat.freshTicks", 150);
    uint32_t t = 62000;
    for (int i = 0; i < kFresh; i++) Tick(c, t);
    hv.Read(ctx, world, false);
    freshFires = hv.meta[kHmMelts] + hv.meta[kHmIgnites] + hv.meta[kHmFreezes] - f0;
    const IVec3 o = world.WindowOrigin();
    for (int z = 0; z < (int)kWorldN; z += 16)
      for (int x = 0; x < (int)kWorldN; x += 16) {
        const uint32_t b = World::MapBiomeAt(o.x * 16 + x, o.z * 16 + z, kDefaultSeed);
        tundraCols += HeatBiomeClimate(b).Day() < 0;
      }
  }
  // ---- (D) the real map's frozen north ----
  uint32_t northFires = 0, northCols = 0, northKind[3] = {0, 0, 0};
  std::string northLog;
  bool northFound = false;
  std::string northWhy;
  {
    const std::string prevMap = worldmap::ActiveMapName(CurrentTuning().world.mapLayer);
    const IVec3 savedOrigin = world.WindowOrigin();
    worldmap::SetMapOverride("default");
    biomes::EnvironmentStamp stamp;
    std::string log;
    if (!ReloadEnvironment(ctx, sim, c.mats, stamp, log)) {
      northWhy = "default map did not load: " + log;
    } else {
      const std::vector<std::string>& names = worldmap::CurrentWorldMap().biomeName;
      int tundra = -1;
      for (size_t i = 0; i < names.size(); i++)
        if (names[i] == "tundra") tundra = (int)i;
      // The window-sized box with the most tundra (8 x 8 samples a box).
      int bestN = 0, bx = 0, bz = 0;
      for (int z = -16384; tundra >= 0 && z <= 16384; z += 512)
        for (int x = -16384; x <= 16384; x += 512) {
          int n = 0;
          for (int sz = 0; sz < 8; sz++)
            for (int sx = 0; sx < 8; sx++)
              n += (int)World::MapBiomeAt(x + sx * 64 + 32, z + sz * 64 + 32, kDefaultSeed) == tundra;
          if (n > bestN) { bestN = n; bx = x; bz = z; }
        }
      northFound = bestN >= 32;
      if (!northFound) northWhy = Format("no tundra-dominated box on the default map (best %d/64)", bestN);
      if (northFound) {
        DawnPin day;   // sunrise + 1024: the warm phase
        c.stream.OnRegen();
        world.SetWindowOrigin({FloorDiv16(bx), 0, FloorDiv16(bz)});
        SubmitWorldgen(ctx, world, sim, kDefaultSeed);
        ctx.WaitIdle();
        HeatView hv;
        hv.Read(ctx, world, false);
        const uint32_t f0 = hv.meta[kHmMelts] + hv.meta[kHmIgnites] + hv.meta[kHmFreezes];
        const int kFresh = (int)BaselineNumber("heat.freshTicks", 150);
        uint32_t t = 62000;
        for (int i = 0; i < kFresh; i++) Tick(c, t);
        const uint32_t k0[3] = {hv.meta[kHmMelts], hv.meta[kHmIgnites], hv.meta[kHmFreezes]};
        hv.Read(ctx, world, false);
        northFires = hv.meta[kHmMelts] + hv.meta[kHmIgnites] + hv.meta[kHmFreezes] - f0;
        for (int k = 0; k < 3; k++) northKind[k] = hv.meta[kHmMelts + k] - k0[k];
        // WHERE: the firing log's cells, and the column round the first one
        // (what is above and below it), so a failure names its cause.
        const uint32_t nLog = std::min(hv.meta[kHmFireLog], kHeatFireLogMax);
        for (uint32_t i = 0; i < nLog; i++) {
          const uint32_t* e = &hv.meta[kHmFireLog + 1 + 4 * i];
          northLog += Format(" (%d,%d,%d k%u biome %u)", (int)e[0], (int)e[1], (int)e[2], e[3],
                             World::MapBiomeAt((int)e[0], (int)e[2], kDefaultSeed));
        }
        if (nLog > 0) {
          const int fx = (int)hv.meta[kHmFireLog + 1], fy = (int)hv.meta[kHmFireLog + 2],
                    fz = (int)hv.meta[kHmFireLog + 3];
          Vox col;
          col.Read(ctx, world, {fx, fy - 4, fz, fx, fy + 4, fz});
          northLog += " | column at the first, y-4..y+4:";
          for (int y = fy - 4; y <= fy + 4; y++) {
            const uint32_t w = col.At(fx, y, fz);
            const uint32_t m = w & 0xFFFu;
            northLog += Format(" %s/%u", m < c.mats.size() ? c.mats[m].name.c_str() : "?",
                               (w >> 12) & 0xFu);
          }
          northLog += Format(" | window origin chunk (%d,%d,%d)", world.WindowOrigin().x,
                             world.WindowOrigin().y, world.WindowOrigin().z);
        }
        const IVec3 o = world.WindowOrigin();
        for (int z = 0; z < (int)kWorldN; z += 16)
          for (int x = 0; x < (int)kWorldN; x += 16)
            northCols += HeatBiomeClimate(World::MapBiomeAt(o.x * 16 + x, o.z * 16 + z,
                                                            kDefaultSeed)).Day() < 0;
      }
    }
    worldmap::SetMapOverride(prevMap);
    biomes::EnvironmentStamp s2;
    std::string l2;
    ReloadEnvironment(ctx, sim, c.mats, s2, l2);
    c.stream.OnRegen();
    world.SetWindowOrigin(savedOrigin);
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
  }
  // ---- (B) the climate checks refuse bad data ----
  auto refused = [&](int base, int swing, const char* what) {
    const std::string dir = sandvox::AssetDir();
    std::ifstream f(dir + "/biomes/tundra.json");
    nlohmann::json j;
    try { f >> j; } catch (...) { return false; }
    j["climate"]["ambient"]["base"] = base;
    j["climate"]["ambient"]["swing"] = swing;
    const std::string tmp = "selftest_heat_biome.json";
    { std::ofstream o(tmp); o << j.dump(2); }
    biomes::BiomeSet set;
    std::string log;
    const bool ok = biomes::LoadBiomeSet(dir, c.mats, set, log, "tundra", tmp);
    std::filesystem::remove(tmp);
    return !ok && log.find(what) != std::string::npos;
  };
  biomes::BiomeSet shipped;
  std::string shippedLog;
  const bool shippedOk = biomes::LoadBiomeSet(sandvox::AssetDir(), c.mats, shipped, shippedLog);
  const bool refusesStraddle = refused(-2, 8, "freezes at night");
  const bool refusesIgnite = refused(63, 63, "ignition point");
  // Day 3 / night 1: no freeze straddle, no ignition -- but the tundra's snow
  // skin melts above 0, so the biome would melt its own landscape.
  const bool refusesSelfMelt = refused(2, 1, "own landscape");
  // ---- (A) the cycle ----
  Tuning saved = CurrentTuning();
  Tuning tn = saved;
  tn.dayNight.freeze = 0;
  tn.dayNight.cycleMinutes = 1;   // 1,800 ticks a day
  SetCurrentTuning(tn);
  const Box room{96, 120, 96, 139, 127, 111};
  const Box west{96, 120, 96, 116, 127, 111}, east{119, 120, 96, 139, 127, 111};
  const Box mid{117, 120, 96, 118, 127, 111};
  const Box wSnow{100, 120, 98, 105, 122, 103}, wIce{100, 120, 105, 105, 122, 109};
  const Box eSnow{124, 120, 98, 129, 122, 103}, eIce{124, 120, 105, 129, 122, 109};
  // (E): its own sealed room south of the east half, inside the east pin.
  // 26 cells (13 blocks) deep in z round the probe, so the tent covers the
  // probe block's whole reach along z and its coverage saturates (c = 1): the
  // night X is the full 230 - 4. The probe is on the lava's TOP row: heat
  // rises (sim.heatUpGain), so the block with lava below it saturates, while
  // the bottom row -- every lava block level with it or above -- reads ~30
  // and the check would have no teeth.
  const Box room2{120, 120, 114, 139, 127, 139};
  const Box lava2{120, 120, 114, 127, 127, 139}, wall2{128, 120, 114, 128, 127, 139};
  const int probeX = 130, probeY = 127, probeZ = 126;
  const int eDay = 24 + 20;   // the east pin: day 44, night 4
  const int lavaEmit = (int)c.mats[MatId(c, "lava")].thermal.emit;
  int dawnTMax = -999, nightX = 0;
  uint32_t wLeft = 0, w0 = 0, eLeft = 0, e0 = 0, awakeE = 0, maxAwakeBoundary = 0;
  uint32_t dusk = 0, dawn = 0, pagesEnd = 0;
  {
    PinGuard pins(sim, {{90, 90, 117, 117, -24, 12}, {118, 90, 145, 145, 24, 20}});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> shell, build, pour;
    Room(shell, room);
    Room(shell, room2);
    Fill(build, mid, kMatStone);
    Fill(build, wSnow, snow);
    Fill(build, wIce, ice);
    Fill(build, eSnow, snow);
    Fill(build, eIce, ice);
    Fill(build, wall2, kMatStone);
    Fill(pour, lava2, MatId(c, "lava") | kFull);
    // Start just before dusk: the phase is a function of the tick.
    const uint32_t ticksPerDay = TicksPerDay(tn);
    uint32_t t = 0;
    while (DaylightStrengthCpu(DayPhaseForTick(t + 200, ticksPerDay, false, 0)) == 0 ||
           DaylightStrengthCpu(DayPhaseForTick(t + 260, ticksPerDay, false, 0)) != 0)
      t += 10;
    Tick(c, t, shell);
    Tick(c, t, build);
    Tick(c, t, pour);
    ctx.WaitIdle();
    Vox v;
    auto count = [&](const Box& b, uint32_t m) {
      uint32_t n = 0;
      for (int z = b.z0; z <= b.z1; z++)
        for (int y = b.y0; y <= b.y1; y++)
          for (int x = b.x0; x <= b.x1; x++) n += (v.At(x, y, z) & 0xFFFu) == m;
      return n;
    };
    v.Read(ctx, world, room);
    w0 = count(wSnow, snow) + count(wIce, ice);
    e0 = count(eSnow, snow) + count(eIce, ice);
    const int settle = (int)BaselineNumber("heat.ambientSettleTicks", 120);
    int sinceBoundary = -1;
    bool prevDay = DaylightStrengthCpu(DayPhaseForTick(t, ticksPerDay, false, 0)) > 0;
    const uint32_t kTicks = ticksPerDay + 600;
    int sinceDawn = -1;
    for (uint32_t i = 0; i < kTicks; i++) {
      Tick(c, t);
      const bool day = DaylightStrengthCpu(DayPhaseForTick(t, ticksPerDay, false, 0)) > 0;
      if (day != prevDay) {
        (day ? dawn : dusk) = i;
        sinceBoundary = 0;
        if (day) sinceDawn = 0;
      }
      // (E): the night's X (every 10th tick), then every tick for 60 after
      // the dawn. T = the ambient of the tick that produced X + X.
      const bool nearDawn = sinceDawn >= 0 && sinceDawn < 60;
      if (nearDawn || (!day && i % 10 == 0)) {
        const int x = (int)ProbeX(ctx, world, probeX, probeY, probeZ);
        if (!day) nightX = std::max(nightX, x);
        else dawnTMax = std::max(dawnTMax, eDay + x);
      }
      if (sinceDawn >= 0) sinceDawn++;
      prevDay = day;
      if (sinceBoundary >= 0 && ++sinceBoundary == settle) {
        maxAwakeBoundary = std::max(maxAwakeBoundary, AwakeAll(ctx, sim));
        sinceBoundary = -1;
      }
    }
    v.Read(ctx, world, room);
    wLeft = count(wSnow, snow) + count(wIce, ice);
    eLeft = count(eSnow, snow) + count(eIce, ice);
    awakeE = AwakeIn(ctx, sim, ChunksOf(east));
    // The lava goes; every page must come back.
    std::vector<CellOp> quench;
    Fill(quench, lava2, kMatStone);
    Tick(c, t, quench);
    const int kCool = (int)BaselineNumber("heat.coolTicks", 400);
    for (int i = 0; i < kCool; i++) Tick(c, t);
    HeatView hv;
    hv.Read(ctx, world, false);
    pagesEnd = hv.PagesInUse();
  }
  SetCurrentTuning(saved);
  const double awakeMax = BaselineNumber("heat.ambientAwakeMax", 32);
  RecordObserved("heat.ambientAwakeObserved", (double)maxAwakeBoundary);
  const bool coldKept = w0 > 0 && wLeft == w0;
  const bool hotMelted = e0 > 0 && eLeft == 0 && awakeE == 0;
  const bool settled = dusk > 0 && dawn > 0 && maxAwakeBoundary <= awakeMax;
  const bool checks = shippedOk && refusesStraddle && refusesIgnite && refusesSelfMelt;
  const bool fresh = freshFires == 0;
  const bool north = northFound && northCols > 0 && northFires == 0;
  // Teeth: without the dawn step the CA's first read would be day + night X.
  const bool teeth = eDay + nightX > lavaEmit;
  const bool ceiling = dawnTMax > -999 && dawnTMax <= std::max(lavaEmit, eDay) && teeth;
  RecordObserved("heat.dawnTMaxObserved", (double)dawnTMax);
  RecordObserved("heat.northFiresObserved", (double)northFires);
  const bool ok = coldKept && hotMelted && settled && checks && fresh && north && ceiling &&
                  pagesEnd == 0;
  detail = Format(
      "frozen side kept %u/%u snow+ice through dusk (tick %u) and dawn (tick %u) %s; hot "
      "side %u of %u left, %u chunks awake %s; window %u chunks awake %d ticks after a "
      "boundary (max %.0f) %s; shipped biomes %s, straddling climate %s, igniting climate "
      "%s, self-melting climate %s; fresh world fired %u thermal transitions (%u window columns frozen-climate) %s; "
      "%u pages at the end; default map's north: %s%u window columns frozen-climate, %u "
      "thermal transitions on a fresh day (%u melts, %u ignitions, %u freezes) %s; dawn beside lava: night X %d, ambient + X "
      "peaked at %d after the dawn (lava %d, night X + day would be %d) %s",
      wLeft, w0, dusk, dawn, coldKept ? "OK" : "FAIL", eLeft, e0, awakeE,
      hotMelted ? "melted+asleep" : "FAIL", maxAwakeBoundary,
      (int)BaselineNumber("heat.ambientSettleTicks", 120), awakeMax, settled ? "OK" : "FAIL",
      shippedOk ? "pass" : ("FAIL: " + shippedLog).c_str(), refusesStraddle ? "refused" : "ACCEPTED",
      refusesIgnite ? "refused" : "ACCEPTED", refusesSelfMelt ? "refused" : "ACCEPTED", freshFires, tundraCols, fresh ? "OK" : "FAIL",
      pagesEnd, northWhy.empty() ? "" : (northWhy + "; ").c_str(), northCols, northFires,
      northKind[0], northKind[1], northKind[2], (north ? "OK" : ("FAIL; logged:" + northLog).c_str()), nightX, dawnTMax, lavaEmit, eDay + nightX,
      ceiling ? "ceiling held" : (teeth ? "OVER THE CEILING" : "NO TEETH (night X too low)"));
  std::printf("heat-ambient: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// heat-idle
// ---------------------------------------------------------------------------
// An ACTIVE world with nothing hot: water poured through a sealed room for 200
// ticks keeps the CA busy, and the temperature layer must do nothing -- no
// page, no source list, no recompute, no relax, no DIRTY_R_HEAT. The CA's
// dispatch list still feeds heatWant (one group a dirty chunk that returns
// after two loads); every other heat row dispatches zero groups. A settled
// world records no heat row at all (C_CAACTIVE) -- `sleep` covers that.
Status GateHeatIdle(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  DawnPin dawn;
  const uint32_t water = MatId(c, "water");
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  const Box room{96, 120, 96, 111, 135, 111};
  std::vector<CellOp> build;
  Room(build, room);
  uint32_t t = 64000;
  Tick(c, t, build);
  HeatView hv;
  hv.Read(ctx, world, false);
  const uint32_t pages0 = hv.PagesInUse(), src0 = hv.meta[kHmSrcPeak],
                 relax0 = hv.meta[kHmRelaxPeak], allocs0 = hv.meta[kHmAllocs];
  uint32_t heatMarks = 0;
  for (int i = 0; i < 200; i++) {
    std::vector<CellOp> pour;
    if (i % 4 == 0)
      Fill(pour, {100 + (i / 4) % 8, 134, 100, 100 + (i / 4) % 8, 134, 101}, water | kFull);
    Tick(c, t, pour);
    if (i % 25 == 0) {
      ctx.WaitIdle();
      std::vector<uint32_t> flags(kNumSlots, 0);
      rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0, flags.data(),
                            kNumSlots * 4, "heatIdleFlags");
      for (uint32_t f : flags) heatMarks += (f & DirtyReasonBit("heat")) != 0;
    }
  }
  hv.Read(ctx, world, false);
  const uint32_t awake = AwakeIn(ctx, sim, ChunksOf(room));
  const bool idle = hv.PagesInUse() == pages0 && pages0 == 0 && hv.meta[kHmAllocs] == allocs0 &&
                    hv.meta[kHmSrcPeak] == src0 && hv.meta[kHmRelaxPeak] == relax0 &&
                    heatMarks == 0;
  detail = Format("200 ticks of pouring water (%u room chunks awake at the end): %u pages, "
                  "%u allocations, source peak %u, relax peak %u, %u heat dirty marks %s",
                  awake, hv.PagesInUse(), hv.meta[kHmAllocs] - allocs0, hv.meta[kHmSrcPeak],
                  hv.meta[kHmRelaxPeak], heatMarks, idle ? "IDLE" : "NOT IDLE");
  std::printf("heat-idle: %s (%s)\n", idle ? "PASS" : "FAIL", detail.c_str());
  return idle ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// heat-plume
// ---------------------------------------------------------------------------
// HEAT RISES (owner, 2026-10-02: "a campfire must not scorch a bush beside
// it, but should ignite a bush directly above it"). The direction gains
// (sim.heatUpGain / heatSideGain / heatDownGain, sim_heat.wgsl heatTent).
//
// FIXTURE: three sealed stone rooms 48 cells apart in x (far past
// sim.heatRadius, so no room's heat reaches another), each holding a sealed
// stone CHAMBER of 4x4x4 cells (2x2x2 heat blocks, block-aligned) against the
// room's back wall, refilled every tick through cell ops so the source never
// flickers or burns out. Nothing burning can touch a plate (the chamber is
// sealed), so every ignition here is the HEAT field's -- the firing log is
// checked to say so.
//   A  flames (the campfire); a 4x4 leaf plate two cells thick directly above
//      it, `heat.plumeAboveCells` over the flame top: must ignite within
//      `heat.plumeIgniteTicksMax`.
//   B  flames; a 4x4 leaf wall `heat.plumeSideCells` to the side of the
//      flame (its full height) and a 4x4 leaf plate `heat.plumeBelowCells`
//      below the flame: neither may ever ignite over `heat.plumeRunTicks`.
//   C  burning leaves (leaf_burning, emit 130 < foliage's 135); the same
//      plate above as A: may never ignite (no foliage-to-foliage chain) and
//      its block may never read past foliage's threshold. (Its burn rule
//      turns some of the chamber to flame each tick before the heat pass
//      reads it, so the source is a 130 / 140 mix, not a pure 130.)
// Climate pinned at 10, dim dawn frozen. Ticked on THE tick (TickCursor). Run
// twice from a fresh worldgen; the final cells and the heat pool must agree.
struct PlumeResult {
  uint32_t lightTick = 0, abovePlate0 = 0, aboveLeft = 0;
  uint32_t side0 = 0, sideLeft = 0, below0 = 0, belowLeft = 0, chain0 = 0, chainLeft = 0;
  int tAbove = -999, tSide = -999, tBelow = -999, tChain = -999;
  uint32_t ignites = 0, refused = 0, logged = 0, loggedOutsideA = 0;
  std::string log;
  std::vector<uint32_t> finalCells;
  uint64_t heatHash = 0;
};

constexpr int kPlCy = 120, kPlCz = 100;   // chamber interior y 120..123, z 100..103
constexpr int kPlRoomX[3] = {104, 152, 200};
constexpr int kPlAmbient = 10;
Box PlChamber(int k) { return {kPlRoomX[k], kPlCy, kPlCz, kPlRoomX[k] + 3, kPlCy + 3, kPlCz + 3}; }
// The room's back wall (z 99) is the chamber's back wall: everything inside
// hangs off it.
Box PlRoom(int k) {
  return {kPlRoomX[k] - 6, kPlCy - 7, kPlCz, kPlRoomX[k] + 11, kPlCy + 11, kPlCz + 11};
}

PlumeResult RunPlume(Ctx& c, int kTicks, int above, int side, int below) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  const uint32_t leaves = MatId(c, "leaves"), fire = MatId(c, "fire"),
                 leafBurning = MatId(c, "leaf_burning");
  SubmitWorldgen(ctx, world, c.sim, kDefaultSeed);
  ctx.WaitIdle();
  // The plates, in world cells (inclusive).
  const Box chA = PlChamber(0), chB = PlChamber(1), chC = PlChamber(2);
  const Box plA{chA.x0, chA.y1 + above, chA.z0, chA.x1, chA.y1 + above + 1, chA.z1};
  const Box plSide{chB.x1 + side, chB.y0, chB.z0, chB.x1 + side, chB.y1, chB.z1};
  const Box plBelow{chB.x0, chB.y0 - below, chB.z0, chB.x1, chB.y0 - below, chB.z1};
  const Box plC{chC.x0, chC.y1 + above, chC.z0, chC.x1, chC.y1 + above + 1, chC.z1};
  std::vector<CellOp> rooms, build;
  for (int k = 0; k < 3; k++) {
    Room(rooms, PlRoom(k));
    // The chamber's shell only (its interior is the source, written each tick).
    const Box ch = PlChamber(k);
    for (int z = ch.z0 - 1; z <= ch.z1 + 1; z++)
      for (int y = ch.y0 - 1; y <= ch.y1 + 1; y++)
        for (int x = ch.x0 - 1; x <= ch.x1 + 1; x++)
          if (!ch.Has(x, y, z))
            build.push_back({World::SlotCellIndex({x, y, z}), (uint32_t)kMatStone});
  }
  Fill(build, plA, leaves);
  Fill(build, plSide, leaves);
  Fill(build, plBelow, leaves);
  Fill(build, plC, leaves);
  std::vector<CellOp> feed;
  Fill(feed, chA, fire);
  Fill(feed, chB, fire);
  Fill(feed, chC, leafBurning);
  uint32_t t = 58000;
  support::TickCursor ticker{c, t, {kPlRoomX[1] >> 4, kPlCy >> 4, kPlCz >> 4}};
  // Rooms in one tick, their contents in the next (two cell ops on one cell
  // in one tick: the first wins, so a room's air would eat its contents).
  ticker({}, rooms);
  ticker({}, build);
  PlumeResult r;
  Vox v;
  const Box all{PlRoom(0).x0 - 1, PlRoom(0).y0 - 1, PlRoom(0).z0 - 1, PlRoom(2).x1 + 1,
                PlRoom(2).y1 + 1, PlRoom(2).z1 + 1};
  auto count = [&](const Box& b) {
    uint32_t n = 0;
    for (int z = b.z0; z <= b.z1; z++)
      for (int y = b.y0; y <= b.y1; y++)
        for (int x = b.x0; x <= b.x1; x++) n += (v.At(x, y, z) & 0xFFFu) == leaves;
    return n;
  };
  v.Read(ctx, world, all);
  r.abovePlate0 = count(plA);
  r.side0 = count(plSide);
  r.below0 = count(plBelow);
  r.chain0 = count(plC);
  // The hottest block a plate spans (every block row of it), as T.
  auto peak = [&](const Box& b) {
    uint32_t m = 0;
    for (int y = b.y0; y <= b.y1; y++) m = std::max(m, ProbeX(ctx, world, b.x0, y, b.z0));
    return kPlAmbient + (int)m;
  };
  for (int i = 0; i < kTicks; i++) {
    ticker({}, feed);
    if (i % 10 == 9) {
      r.tAbove = std::max(r.tAbove, peak(plA));
      r.tSide = std::max(r.tSide, peak(plSide));
      r.tBelow = std::max(r.tBelow, peak(plBelow));
      r.tChain = std::max(r.tChain, peak(plC));
      if (r.lightTick == 0) {
        v.Read(ctx, world, plA);
        if (count(plA) < r.abovePlate0) r.lightTick = (uint32_t)i + 1;
      }
    }
  }
  v.Read(ctx, world, all);
  r.aboveLeft = count(plA);
  r.sideLeft = count(plSide);
  r.belowLeft = count(plBelow);
  r.chainLeft = count(plC);
  HeatView hv;
  hv.Read(ctx, world, true);
  hv.Note();
  r.ignites = hv.meta[kHmIgnites];
  r.refused = hv.meta[kHmRefused];
  r.heatHash = hv.Hash();
  // The firing log: every heat transition it caught must be a leaf of plate A.
  r.logged = std::min(hv.meta[kHmFireLog], kHeatFireLogMax);
  for (uint32_t i = 0; i < r.logged; i++) {
    const uint32_t* e = &hv.meta[kHmFireLog + 1 + 4 * i];
    const int x = (int)e[0], y = (int)e[1], z = (int)e[2];
    if (!plA.Has(x, y, z)) {
      r.loggedOutsideA++;
      if (r.log.size() < 120) r.log += Format(" (%d,%d,%d kind %u)", x, y, z, e[3]);
    }
  }
  for (int z = all.z0; z <= all.z1; z++)
    for (int y = all.y0; y <= all.y1; y++)
      for (int x = all.x0; x <= all.x1; x++) r.finalCells.push_back(v.At(x, y, z) & 0xFFFFu);
  return r;
}

Status GateHeatPlume(Ctx& c, std::string& detail) {
  DawnPin dawn;
  PinGuard pins(c.sim, {{88, 92, 222, 120, kPlAmbient, 0}});
  const int kTicks = (int)BaselineNumber("heat.plumeRunTicks", 900);
  const double litMax = BaselineNumber("heat.plumeIgniteTicksMax", 300);
  const int above = (int)BaselineNumber("heat.plumeAboveCells", 3);
  const int side = (int)BaselineNumber("heat.plumeSideCells", 2);
  const int below = (int)BaselineNumber("heat.plumeBelowCells", 2);
  const PlumeResult a = RunPlume(c, kTicks, above, side, below);
  const PlumeResult b = RunPlume(c, kTicks, above, side, below);
  RecordObserved("heat.plumeIgniteTicksObserved", (double)a.lightTick);
  RecordObserved("heat.plumeAboveTObserved", (double)a.tAbove);
  RecordObserved("heat.plumeSideTObserved", (double)a.tSide);
  int leafAbove = 135;
  for (const HeatTransition& h : c.mats[MatId(c, "leaves")].thermal.transitions)
    if (h.kind == kHeatKindIgnite) leafAbove = h.threshold;
  const int burnEmit = (int)c.mats[MatId(c, "leaf_burning")].thermal.emit;
  const int fireEmit = (int)c.mats[MatId(c, "fire")].thermal.emit;
  const bool lit = a.lightTick > 0 && a.lightTick <= litMax && a.tAbove > leafAbove;
  const bool sideKept = a.side0 > 0 && a.sideLeft == a.side0 && a.tSide <= leafAbove;
  const bool belowKept = a.below0 > 0 && a.belowLeft == a.below0 && a.tBelow <= leafAbove;
  // Not tChain <= burnEmit: leaf_burning's own burn rule turns some of the
  // chamber to flame inside the CA before the heat pass reads it, so the
  // source is a 130 / 140 mix (measured 132). The claim is the owner's: it
  // never lifts the foliage above it past its threshold.
  const bool chainKept = a.chain0 > 0 && a.chainLeft == a.chain0 && a.tChain <= leafAbove &&
                         burnEmit < leafAbove;
  const bool ceiling = a.tAbove <= fireEmit;
  const bool onlyA = a.logged > 0 && a.loggedOutsideA == 0;
  const bool twice = a.finalCells == b.finalCells && a.heatHash == b.heatHash;
  const bool ok = lit && sideKept && belowKept && chainKept && ceiling && onlyA && twice &&
                  a.refused == 0;
  detail = Format(
      "campfire (flames emit %d), foliage (ignites above %d): %d cells ABOVE peaked at %d, lit "
      "at tick %u (max %.0f) %s; %d cells to the SIDE peaked at %d, %u/%u kept %s; %d cells "
      "BELOW peaked at %d, %u/%u kept %s; burning leaves (emit %d) with foliage %d above: "
      "peaked at %d, %u/%u kept %s; ceiling %s; %u heat ignitions, firing log %u entries, %u "
      "outside the plate above%s %s; run twice: cells %s, heat hash %016llx vs %016llx %s; "
      "refused %u",
      fireEmit, leafAbove, above, a.tAbove, a.lightTick, litMax, lit ? "OK" : "FAIL", side,
      a.tSide, a.sideLeft, a.side0, sideKept ? "OK" : "FAIL", below, a.tBelow, a.belowLeft,
      a.below0, belowKept ? "OK" : "FAIL", burnEmit, above, a.tChain, a.chainLeft, a.chain0,
      chainKept ? "OK" : "FAIL", ceiling ? "OK" : "OVER THE FLAME", a.ignites, a.logged,
      a.loggedOutsideA, a.log.c_str(), onlyA ? "OK" : "FAIL",
      a.finalCells == b.finalCells ? "same" : "DIFFER", (unsigned long long)a.heatHash,
      (unsigned long long)b.heatHash, twice ? "OK" : "NONDETERMINISTIC", a.refused);
  std::printf("heat-plume: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& HeatGates() {
  static const std::vector<Gate> g = {
      {"heat-idle", "sim", {}, false, GateHeatIdle},
      {"heat-melt", "sim", {}, false, GateHeatMelt},
      {"heat-ignite", "sim", {}, false, GateHeatIgnite},
      {"heat-freeze", "sim", {}, false, GateHeatFreeze},
      {"heat-ambient", "sim", {}, false, GateHeatAmbient},
      {"heat-bound", "sim", {}, false, GateHeatBound},
      {"heat-plume", "sim", {}, false, GateHeatPlume},
  };
  return g;
}

}  // namespace selftest
