// selftest_rivers.cpp -- `rivers`: map.json rivers[] and lakes[] in the engine
// (docs/PLAN_worldbuilder.md W1), and the smooth roads that share their spline.
//
// WHAT THIS GUARDS. A river is a RUNNING surface held at rest (worldwater.cpp
// and docs/PLAN_running_water.md say why): the bake makes its surface monotone
// and ramps it in eighths, keeping only waterfalls as hard steps; worldgen
// carves the channel, holds the banks and stands a crest and a curtain at
// every fall; sim_step.wgsl's held-water rule keeps all of it still, and the
// whole thing must be generated AT REST -- the sim must sleep over it
// (CLAUDE.md rule 2), which is the claim this gate exists for. A river is
// also an INFINITE reservoir: a hole in it refills and a breach in its bank
// floods (pass R). One synthetic
// map in a TEMP asset dir (the `roads` seam): ground falling 7 m across the
// window with hills either side and a basin, carrying
//   main   a smooth 4 m river down the valley (stepVox 4: many small steps),
//   falls  a tributary off the north hill with stepVox 24 (waterfalls) that
//          `joins` main,
//   creek  a 1.2 m smooth stream into the lake,
//   lake   a smooth polygon round the basin, level 10 voxels over its floor,
//   ford   a smooth road (smooth: true) across main.
// Then:
//   S. THE SPLINE: worldmap::SplineTessellate == tests/spline_vectors.json
//      (the table assets/editor/spline.js is held to) within 2e-3.
//   A. LOAD: three rivers, one lake; main has reaches, falls a waterfall, the
//      join resolves with the mouth on main's channel, the creek drains into
//      the lake; a join naming no river and a join cycle are REFUSED by name.
//   B. THE BAKE (CPU): every river's surface is monotone non-increasing
//      source -> mouth and every piece starts on the profile; the tributary's surface at the
//      junction is >= main's there; the creek ends at the lake's level; the
//      smooth road went through the spline and its baked grade respects
//      maxGrade (the walked grade is the `roads` gate's; this road fords).
//      A road crossing a river keeps its deck one voxel under the water.
//   C. LAKE (CPU): over the lake's box, water stands to the level exactly on
//      the columns inside the polygon whose ground is under it, and nowhere
//      else outside a channel.
//   D. CPU == GPU (per column, every 2nd): the mirror's ground is the world's
//      ground, its water the world's water (to the cell).
//   E. CONTAINED (GPU, every water voxel in the window band): no water voxel
//      has AIR under it, and AIR beside it only over a HELD column whose own
//      surface is below that air (a ramp's step, a rapid, a curtain): the
//      held rule refuses exactly that move, so nothing can flow on tick 1.
//      The held faces are counted; the curtains of `falls` must be among them.
//   F. FAR: the level-5 cascade cells (3.2 m) along the 2 m `falls` are
//      water (the narrow-water tint); the horizon's water plane covers main
//      and the lake.
//   G. SLEEP, under a sky pinned clear (rain wakes every exposed cell, river
//      or meadow): ticked to <= rivers.maxAwake (32) awake chunks and HELD
//      there rivers.holdTicks more, at most rivers.maxAwakeWater of them over
//      water or banks; WHERE they are and WHICH field moves when it fails.
//   R. RUNNING, after G: a CRATER scooped out of main's surface refills to
//      the profile, and a TRENCH cut through main's bank fills from the river
//      to the river's level -- the reservoir is infinite and the held rule
//      lets water into a held column under its target and into a column
//      that is not held at all.
//   S. A STREAM IS HELD, after R: a channel dug from the lake down to main
//      runs as water, is adopted column by column (sim_stream.wgsl) and
//      sleeps; its mouth plugged with stone, it is released and drains.
//   V. SOLUTE IS CARRIED, after S: ink poured above `falls`' first waterfall
//      goes down the curtain into the reach below (sim_solute.wgsl THE HELD
//      FLOW CARRIES WHAT IS DISSOLVED IN IT), the ledger balances and its
//      chunks fall asleep (PassSoluteCarried).
//   I. CURRENT: the map-river arm of the current field puts several stream
//      primitives on main and the field there runs downstream.
//   H. THE SITE INDEX on `default` with a 10 km test river + a lake: loads,
//      the worst cell list and the bake's time reported.
// A top-down picture of the window as generated goes to build/rivers_top.bmp
// (Read it: a river must look like a ribbon with steps, not a trench).
// Leaves the pristine world behind (the `roads` exit).

#include <algorithm>
#include <bit>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "game/camera.h"
#include "gpu/resources.h"
#include "sim/biomes.h"
#include "sim/celestial.h"
#include "sim/currentprim.h"
#include "sim/flowfield.h"
#include "sim/horizon.h"
#include "sim/solutes.h"
#include "sim/treeatlas.h"
#include "sim/tuning.h"
#include "sim/weather.h"
#include "sim/world.h"
#include "sim/worldgen_run.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

namespace worldmap {
// world.cpp: what every river piece said about a column (the E leak report).
std::string WaterColumnTrace(int x, int z, uint32_t seed);
}  // namespace worldmap

namespace selftest {
namespace {

namespace fs = std::filesystem;
using sandvox::kDefaultSeed;

bool WriteTileR(const fs::path& p, uint32_t kind, int tx, int tz, const std::vector<int>& samples) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  if (!f) return false;
  const uint32_t sb = kind == worldmap::kPlaneHeight ? 2u : 1u;
  const uint32_t hdr[8] = {worldmap::kTileMagic, kind, static_cast<uint32_t>(tx), static_cast<uint32_t>(tz),
                           static_cast<uint32_t>(worldmap::kTileSide), static_cast<uint32_t>(worldmap::kTileSide), sb, 0u};
  f.write(reinterpret_cast<const char*>(hdr), sizeof hdr);
  for (int v : samples) {
    if (sb == 2) { const int16_t s = static_cast<int16_t>(v); f.write(reinterpret_cast<const char*>(&s), 2); }
    else { const uint8_t s = static_cast<uint8_t>(v); f.write(reinterpret_cast<const char*>(&s), 1); }
  }
  return static_cast<bool>(f);
}

// ---- S. the spline vectors ---------------------------------------------------------
bool CheckSplineVectors(std::string& why, int& cases, int& points) {
  const fs::path assets(sandvox::AssetDir());
  fs::path p = assets.parent_path() / "tests" / "spline_vectors.json";
  if (!fs::exists(p)) p = "tests/spline_vectors.json";
  std::ifstream f(p);
  nlohmann::json j;
  try { f >> j; } catch (...) { why = "tests/spline_vectors.json does not parse (" + p.string() + ")"; return false; }
  const double step = j.value("step", 16.0);
  cases = points = 0;
  for (const auto& c : j["cases"]) {
    std::vector<std::array<double, 2>> pts;
    for (const auto& q : c["points"]) pts.push_back({q[0].get<double>(), q[1].get<double>()});
    const bool closed = c.value("closed", false), smooth = c.value("smooth", true);
    const auto got = smooth ? worldmap::SplineTessellate(pts, closed, step) : worldmap::PolylineResample(pts, closed, step);
    const auto& want = c["out"];
    cases++;
    if (got.size() != want.size()) {
      why = Format("case \"%s\": %zu points, the table has %zu", c.value("name", "?").c_str(), got.size(), want.size());
      return false;
    }
    for (size_t i = 0; i < got.size(); i++, points++) {
      const double ex = want[i][0].get<double>(), ez = want[i][1].get<double>();
      if (std::fabs(got[i].x - ex) > 2e-3 || std::fabs(got[i].z - ez) > 2e-3) {
        why = Format("case \"%s\" point %zu: (%.4f, %.4f), the table says (%.3f, %.3f)", c.value("name", "?").c_str(), i,
                     got[i].x, got[i].z, ex, ez);
        return false;
      }
    }
  }
  return cases > 0;
}

// ---- V. THE HELD FLOW CARRIES SOLUTE (ticket 2026-10-16_11-11-51) ----------------
// sim_solute.wgsl "THE HELD FLOW CARRIES WHAT IS DISSOLVED IN IT": a held
// river does not move, so the CA never carried what was dissolved in it --
// blood poured at the top of a waterfall stayed at the top. Pass V pours INK
// (conserved: no halfLife, density 0 so nothing sinks it, diffusivity 8 so
// diffusion alone would not get it far) into `falls` just above its first
// waterfall, and asserts
//   (a) CARRIED: after rivers.soluteCarryTicks, at least rivers.soluteBelowMin
//       of what was poured stands in columns whose held surface is more than
//       4 voxels under the upper reach's -- it went down the curtain and out
//       into the reach below (and on into main);
//   (b) CONSERVED: the layer's mass moved by exactly what the ledger says
//       (poured - discarded - precipitated - converted), and the floor
//       discarded at most rivers.soluteDiscardMax of it;
//   (c) SETTLED: within rivers.soluteSettleTicks either no awake chunk carries
//       solute (held at 0 for two samples), or (rivers.soluteRequireSleep 0)
//       the carry has TERMINATED -- no carried pair has work left anywhere:
//       it ran down to where nothing lower has room and stopped (the
//       potential argument in the shader).
// A failure names the awake solute chunks, their mass, stall clocks, dirty
// reasons and the CPU oracle's pending pairs by kind; a CONTROL counts the
// awake chunks with moving water and no solute at all.
struct SolLayerR {
  std::vector<uint32_t> table, pool, hdr, stall;
  uint32_t Value(uint32_t slot, uint32_t local) const {
    const uint32_t e = table[slot];
    if (e == 0u) return 0u;
    if (e & kSolUniformBit) return e & 0xFFFFu;
    const uint32_t w = pool[size_t(e & kSolPageMask) * kSolWordsPerPage + (local >> 1)];
    return (w >> ((local & 1u) * 16u)) & 0xFFFFu;
  }
};
void ReadSolLayerR(Ctx& c, SolLayerR& L) {
  c.ctx.WaitIdle();
  L.table.assign(kNumSlots, 0u);
  L.pool.assign(size_t(kSolutePoolPages) * kSolWordsPerPage, 0u);
  L.hdr.assign(kSolMetaHdrWords, 0u);
  L.stall.assign(kNumSlots, 0u);
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.solTable, 0, L.table.data(), L.table.size() * 4, "rvSolTable");
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.solPool, 0, L.pool.data(), L.pool.size() * 4, "rvSolPool");
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.solMeta, 0, L.hdr.data(), L.hdr.size() * 4, "rvSolMeta");
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.solMeta, uint64_t(kSolMStall) * 4, L.stall.data(),
                        L.stall.size() * 4, "rvSolStall");
}

// THE CPU ORACLE for an awake solute chunk (CLAUDE.md verification rule 6: a
// bare "13 chunks awake" names no cause). It re-evaluates, from the read-back
// voxels, solute layer and still table, every pair sim_solute.wgsl's
// solPairExchange could see with its lower cell in the chunk -- 3 axes x 4
// strides, both parities -- and counts those with work left by KIND: a CARRY
// (both held, ranks differ), a level DIFFUSION (|d| >= 1), a one-way held ->
// unheld diffusion. The rank is the shader's (stillLevel8 / solDropDist /
// solHeldOrder, written out again here as the test's own oracle); the solvent
// test is "water" (the fixture holds no other liquid). "carry 0" with the
// chunk still awake means the carry has finished and what is left is the
// diffusion's own pace or the CA.
struct SolOracle {
  Ctx& c;
  const SolLayerR& L;
  std::vector<uint32_t> still;
  std::map<uint32_t, std::vector<uint32_t>> vox;
  uint32_t mWater = 0, cap8 = 80;
  IVec3 org{};
  int relax = 1;
  uint32_t Word(int x, int y, int z) {
    const uint32_t s = World::SlotChunkIndex(IVec3{x >> 4, y >> 4, z >> 4});
    auto it = vox.find(s);
    if (it == vox.end()) {
      std::vector<uint32_t> v(kChunkVol);
      sandvox::ReadVoxelsSync(c.ctx, c.world, s, 1, v.data(), "rvSolOracle");
      it = vox.emplace(s, std::move(v)).first;
    }
    return it->second[(uint32_t(z & 15) * kChunk + uint32_t(y & 15)) * kChunk + uint32_t(x & 15)];
  }
  bool InWin(int x, int y, int z) const {
    const int n = int(kWorldN);
    const int dx = x - org.x * 16, dy = y - org.y * 16, dz = z - org.z * 16;
    return dx >= 0 && dy >= 0 && dz >= 0 && dx < n && dy < n && dz < n;
  }
  uint32_t Mass(int x, int y, int z) const {
    return L.Value(World::SlotChunkIndex(IVec3{x >> 4, y >> 4, z >> 4}),
                   (uint32_t(z & 15) * kChunk + uint32_t(y & 15)) * kChunk + uint32_t(x & 15)) & 0xFFu;
  }
  static constexpr int kUnheld = -1073741824;
  int Level8(int x, int z) const {
    if (relax == 0) return kUnheld;
    const int sh = std::countr_zero(kWorldN);
    const size_t si = (size_t(uint32_t(z) & (kWorldN - 1)) * kWorldN + size_t(uint32_t(x) & (kWorldN - 1))) * 4u;
    const uint32_t tag = 1u | ((uint32_t(x >> sh) & 0x7FFFu) << 1) | ((uint32_t(z >> sh) & 0x7FFFu) << 16);
    if (still[si] != tag) return kUnheld;
    const uint32_t v = still[si + 1];
    if (v != 0u) return (v >> 20) != mWater ? kUnheld : int32_t(v << 12) >> 12;
    const uint32_t d = still[si + 2];
    if (d == 0u || (still[si + 3] & 0xFFFu) != mWater) return kUnheld;
    return int32_t(d << 12) >> 12;
  }
  int DropDist(int x, int z, int t8, int reach) const {
    int best = reach + 1;
    const int dd[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& d : dd)
      for (int j = 1; j < best; j++) {
        const int t = Level8(x + d[0] * j, z + d[1] * j);
        if (t == kUnheld) break;
        if (t <= t8 - 8) { best = j; break; }
        if (t != t8) break;
      }
    return best;
  }
  int Order(int ax, int ay, int az, int bx, int by, int bz) const {
    const int tA = Level8(ax, az), tB = Level8(bx, bz);
    if (tA == kUnheld && tB == kUnheld) return 0;
    if (tB == kUnheld) return 1;
    if (tA == kUnheld) return -1;
    if (tA != tB) return tA > tB ? 2 : -2;
    if (ax == bx && az == bz) return DropDist(ax, az, tA, 1) == 1 ? -2 : 0;
    const int rA = DropDist(ax, az, tA, 4), rB = DropDist(bx, bz, tB, 4);
    return rA == rB ? 0 : (rA > rB ? 2 : -2);
  }
  // Pending pairs whose LOWER cell is in chunk `wc`: carry / diffuse / one-way.
  void Pending(IVec3 wc, int& carry, int& diff, int& oneway) {
    carry = diff = oneway = 0;
    for (int lz = 0; lz < 16; lz++)
      for (int ly = 0; ly < 16; ly++)
        for (int lx = 0; lx < 16; lx++) {
          const int x = wc.x * 16 + lx, y = wc.y * 16 + ly, z = wc.z * 16 + lz;
          const uint32_t wA = Word(x, y, z);
          // sim_solute.wgsl solStepCarry: a stride-1 lateral pair, one wet and
          // one dry, carries from the wet cell into the cell under the dry one
          // when the wet cell stands on a non-liquid and outranks it.
          for (int axis = 0; axis < 3; axis += 2) {
            const int bx = x + (axis == 0 ? 1 : 0), bz = z + (axis == 2 ? 1 : 0);
            if (!InWin(bx, y, bz)) continue;
            const bool aWet = (wA & 0xFFFu) == mWater, bWet = (Word(bx, y, bz) & 0xFFFu) == mWater;
            if (aWet == bWet) continue;
            const int hx = aWet ? x : bx, hz = aWet ? z : bz, dx = aWet ? bx : x, dz = aWet ? bz : z;
            if (!InWin(dx, y - 1, dz) || (Word(dx, y - 1, dz) & 0xFFFu) != mWater) continue;
            if (InWin(hx, y - 1, hz) && (Word(hx, y - 1, hz) & 0xFFFu) == mWater) continue;
            if (Order(hx, y, hz, dx, y - 1, dz) != 2) continue;
            const uint32_t xh = Mass(hx, y, hz), xl = Mass(dx, y - 1, dz);
            const uint32_t fl = ((Word(dx, y - 1, dz) >> 12) & 7u) + 1u;
            if (xh > 0 && xl < std::min(cap8 * fl / 8u, 255u)) carry++;
          }
          if ((wA & 0xFFFu) != mWater) continue;
          const uint32_t xA = Mass(x, y, z);
          for (int axis = 0; axis < 3; axis++)
            for (int k = 1; k <= 8; k <<= 1) {
              const int bx = x + (axis == 0 ? k : 0), by = y + (axis == 1 ? k : 0), bz = z + (axis == 2 ? k : 0);
              if (!InWin(bx, by, bz)) continue;
              const uint32_t wB = Word(bx, by, bz);
              if ((wB & 0xFFFu) != mWater) continue;
              const uint32_t xB = Mass(bx, by, bz);
              if (xA == 0 && xB == 0) continue;
              bool through = true;
              for (int i = 1; i < k && through; i++)
                through = (Word(x + (axis == 0 ? i : 0), y + (axis == 1 ? i : 0), z + (axis == 2 ? i : 0)) & 0xFFFu) == mWater;
              if (!through) continue;
              const uint32_t fA = ((wA >> 12) & 7u) + 1u, fB = ((wB >> 12) & 7u) + 1u;
              const int ord = Order(x, y, z, bx, by, bz);
              if (ord == 2 || ord == -2) {
                const uint32_t xh = ord > 0 ? xA : xB, xl = ord > 0 ? xB : xA, fl = ord > 0 ? fB : fA;
                const uint32_t cap = std::min(cap8 * fl / 8u, 255u);
                if (xh > 0 && xl < cap) carry++;
                continue;
              }
              const int num = int(xA * fB) - int(xB * fA), den = int(fA + fB);
              int d = (2 * std::abs(num) + den - 1) / (2 * den);
              if (num < 0) d = -d;
              if (d == 0) continue;
              if (ord != 0) {
                if ((d > 0) == (ord > 0)) oneway++;
              } else {
                diff++;
              }
            }
        }
  }
};

// ---- X. dissolved mass drifts downstream on a level reach ------------------
// Ticket 2026-10-17_00-01-44: "blood solute in water should also slowly
// drift in the direction of water current". THE DRIFT (sim_solute.wgsl): at
// one (T8, drop distance) the column nearer the river's mouth (the still
// table's MOUTH words, worldgen `cols`) ranks lower. Ink poured on main an
// eighth of the way down; the mass-weighted network distance to the mouth of
// the ink near the pour must fall by rivers.driftMin voxels in driftTicks.
// Measured over a disc round the pour only, so V's ink further down is not
// counted.
bool PassSoluteDrift(Ctx& c, sandvox::support::TickCursor& tick, const worldmap::WorldMapData::River& main,
                     std::vector<std::string>& problems) {
  const SoluteDef* ink = CurrentSoluteNamed("ink");
  if (!ink || main.line.size() < 16) {
    problems.push_back("X: needs the `ink` species and main's line");
    std::printf("rivers: X solute drift: FAIL (no ink or no line)\n");
    return false;
  }
  const auto& P = main.line[main.line.size() / 8];
  const int px = P[0], pz = P[1], py = P[2];
  const int radius = static_cast<int>(BaselineNumber("rivers.driftRadius", 90));
  const size_t L = size_t(kWorldN) * kWorldN * 4u + 4u;
  std::vector<uint32_t> tab(L + size_t(kWorldN) * kWorldN);
  auto census = [&](double& mean, uint64_t& mass, int& mouthCols) {
    rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.StillWaterBuffer(), 0, tab.data(), tab.size() * 4,
                          "rvDriftStill");
    SolLayerR S;
    ReadSolLayerR(c, S);
    double sum = 0;
    mass = 0;
    mouthCols = 0;
    for (int dz = -radius; dz <= radius; dz += 4)
      for (int dx = -radius; dx <= radius; dx += 4) {
        const size_t ci = size_t((pz + dz) & int(kWorldN - 1)) * kWorldN + size_t((px + dx) & int(kWorldN - 1));
        if (tab[L + ci] != 0u) mouthCols++;
      }
    for (uint32_t sl = 0; sl < kNumSlots; sl++) {
      if (S.table[sl] == 0u) continue;
      const IVec3 wc = c.world.SlotToWorldChunk(sl);
      for (uint32_t k = 0; k < kChunkVol; k++) {
        const uint32_t m = S.Value(sl, k) & 0xFFu;
        if (m == 0u) continue;
        const int x = wc.x * 16 + int(k % 16), z = wc.z * 16 + int(k / 256);
        if ((x - px) * (x - px) + (z - pz) * (z - pz) > radius * radius) continue;
        const size_t ci = size_t(z & int(kWorldN - 1)) * kWorldN + size_t(x & int(kWorldN - 1));
        const uint32_t mo = tab[L + ci];
        if (mo == 0u) continue;
        sum += double(m) * double(mo - 1u);
        mass += m;
      }
    }
    mean = mass ? sum / double(mass) : 0.0;
  };
  const uint32_t units = static_cast<uint32_t>(BaselineNumber("rivers.soluteUnits", 1600));
  sandvox::support::TickOps pour;
  pour.cells.push_back(CellOp{World::SlotCellIndex(IVec3{px, py + 2, pz}), CellOpSolute(ink->species, units)});
  tick(pour);
  for (int k = 0; k < 30; k++) tick();
  double m0 = 0, m1 = 0;
  uint64_t mass0 = 0, mass1 = 0;
  int cols0 = 0, cols1 = 0;
  census(m0, mass0, cols0);
  const int driftTicks = static_cast<int>(BaselineNumber("rivers.driftTicks", 900));
  std::string curve;
  for (int k = 1; k <= driftTicks; k++) {
    tick();
    if (k % 300 == 0) {
      double mk = 0;
      uint64_t massk = 0;
      int colsk = 0;
      census(mk, massk, colsk);
      curve += Format(" %d:%.1f/%llu", k, mk, (unsigned long long)massk);
    }
  }
  census(m1, mass1, cols1);
  const double driftMin = BaselineNumber("rivers.driftMin", 8.0);
  const bool xOk = cols0 > 0 && mass0 > 0 && m0 - m1 >= driftMin;
  std::printf("rivers: X solute drift: %s (ink at main's eighth (%d,%d) y%d, %d river columns with a mouth word in "
              "the disc; mean distance to the mouth %.1f -> %.1f after %d ticks (fell %.1f >= %.1f), mass in the disc "
              "%llu -> %llu; tick:mean/mass%s)\n",
              xOk ? "PASS" : "FAIL", px, pz, py, cols0, m0, m1, driftTicks, m0 - m1, driftMin,
              (unsigned long long)mass0, (unsigned long long)mass1, curve.c_str());
  RecordObserved("rivers.observedDrift", Format("%.1f", m0 - m1));
  if (!xOk) problems.push_back(Format("X: drift %.1f voxels (>= %.1f), %d mouth columns", m0 - m1, driftMin, cols0));
  return xOk;
}

bool PassSoluteCarried(Ctx& c, sandvox::support::TickCursor& tick, const worldmap::WorldMapData::River& falls,
                       std::vector<std::string>& problems) {
  const SoluteDef* ink = CurrentSoluteNamed("ink");
  if (!ink || falls.falls.empty()) {
    problems.push_back("V: needs the `ink` species and a waterfall on `falls`");
    std::printf("rivers: V solute carried: FAIL (no `ink` species or no waterfall)\n");
    return false;
  }
  // V measures the CARRY: the drift (sim.soluteDrift, pass X) is a long-running
  // current by design -- ink keeps moving downstream until it reaches the
  // window's face -- so with it on, (c)'s "the carry has terminated" can never
  // be read. Off for this pass, restored on the way out.
  struct DriftOff {
    Tuning saved = CurrentTuning();
    DriftOff() { Tuning t = saved; t.sim.soluteDrift = 0; SetCurrentTuning(t); }
    ~DriftOff() { SetCurrentTuning(saved); }
  } driftOff;
  // The first fall: its sample is the first of the reach BELOW (worldwater.cpp
  // step 9); the one before it is the upper reach's last.
  const auto& F = falls.falls[0];
  size_t iF = 0;
  for (size_t i = 1; i < falls.line.size(); i++)
    if (falls.line[i][0] == F[0] && falls.line[i][1] == F[1]) { iF = i; break; }
  if (iF == 0) {
    problems.push_back("V: the first waterfall is not on falls' baked line");
    std::printf("rivers: V solute carried: FAIL (fall not on the line)\n");
    return false;
  }
  const auto& up = falls.line[iF - 1];
  const auto& dn = falls.line[iF];
  const int yU = up[2], yL = dn[2];
  // "Below the fall": a column whose held surface stands more than 4 voxels
  // under the upper reach's (the reach below it, and main past the junction).
  auto below = [&](int x, int z) {
    const worldmap::WaterColumn wc = worldmap::WaterColumnAt(x, z, kDefaultSeed);
    return wc.water >= 0 && wc.water < yU - 4;
  };
  struct Census { uint64_t mass = 0, under = 0; };
  auto census = [&](const SolLayerR& L) {
    Census r;
    for (uint32_t s = 0; s < kNumSlots; s++) {
      const uint32_t e = L.table[s];
      if (e == 0u) continue;
      const IVec3 wc = c.world.SlotToWorldChunk(s);
      for (uint32_t k = 0; k < kChunkVol; k++) {
        const uint32_t m = L.Value(s, k) & 0xFFu;
        if (m == 0u) continue;
        r.mass += m;
        if (below(wc.x * 16 + int(k % 16), wc.z * 16 + int(k / 256))) r.under += m;
      }
    }
    return r;
  };
  // The awake chunks that carry solute (the claim of (c)), and who they are:
  // the chunk, its mass and stall clock, its dirty REASONS, and the oracle's
  // pending pairs (carry / diffuse / one-way). `totals` = the oracle's sums.
  const uint32_t mWaterId = [&] {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == "water") return static_cast<uint32_t>(i);
    return 0u;
  }();
  auto makeOracle = [&](const SolLayerR& L) {
    std::unique_ptr<SolOracle> orc(new SolOracle{c, L, {}, {}, mWaterId, ink->saturation, c.world.WindowOrigin(),
                                                 CurrentTuning().sim.stillWaterRelax});
    orc->still.assign(size_t(kWorldN) * kWorldN * 4u, 0u);
    rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.StillWaterBuffer(), 0, orc->still.data(),
                          orc->still.size() * 4, "rvSolStill");
    return orc;
  };
  // The oracle over EVERY chunk that holds solute, awake or not: pending
  // carry / diffuse / one-way pairs, and the carry pairs left in chunks over
  // columns ABOVE the fall (not `below`). A chunk asleep with a carry left
  // would be the sleep mirror disagreeing with the work test.
  struct Pend { int carry = 0, diff = 0, oneway = 0, carryAbove = 0; };
  auto pendingAll = [&](const SolLayerR& L) {
    Pend r;
    auto orc = makeOracle(L);
    for (uint32_t s = 0; s < kNumChunks; s++) {
      if (L.table[s] == 0u) continue;
      const IVec3 wc = c.world.SlotToWorldChunk(s);
      int pc = 0, pd = 0, po = 0;
      orc->Pending(wc, pc, pd, po);
      r.carry += pc;
      r.diff += pd;
      r.oneway += po;
      if (!below(wc.x * 16 + 8, wc.z * 16 + 8)) r.carryAbove += pc;
    }
    return r;
  };
  // Awake solute chunks whose WATER is moving (the CA's MOVE bit), counted by
  // the last awakeSol call that asked for `who`: ink on a river that is not at
  // rest is stirred by the CA, which no potential argument covers.
  int movingAwake = 0;
  auto awakeSol = [&](const SolLayerR& L, std::string* who, std::string* totals) {
    std::vector<uint32_t> fl(kNumSlots, 0);
    rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.DirtyActive(), 0, fl.data(), kNumSlots * 4, "rvSolAwake");
    std::unique_ptr<SolOracle> orc;
    if (who) orc = makeOracle(L);
    int n = 0, tc = 0, td = 0, to = 0;
    movingAwake = 0;
    for (uint32_t s = 0; s < kNumChunks; s++) {
      if (fl[s] == 0u || L.table[s] == 0u) continue;
      n++;
      if (fl[s] & DirtyReasonBit("MOVE")) movingAwake++;
      if (!who) continue;
      uint64_t m = 0;
      uint32_t cells = 0, hi = 0;
      for (uint32_t k = 0; k < kChunkVol; k++) {
        const uint32_t v = L.Value(s, k) & 0xFFu;
        m += v;
        cells += v != 0u;
        hi = std::max(hi, v);
      }
      const IVec3 wc = c.world.SlotToWorldChunk(s);
      int pc = 0, pd = 0, po = 0;
      orc->Pending(wc, pc, pd, po);
      tc += pc;
      td += pd;
      to += po;
      std::string why;
      for (int b = 0; b < kDirtyReasonBits; b++)
        if (fl[s] & (1u << b)) why += std::string(why.empty() ? "" : "+") + kDirtyReasonName[b];
      if (who->size() < 1400)
        *who += Format(" (%d,%d,%d)%s mass %llu in %u cells (max %u) stall %u [%s] pending carry %d diffuse %d one-way %d;",
                       wc.x, wc.y, wc.z, below(wc.x * 16 + 8, wc.z * 16 + 8) ? " below" : "", (unsigned long long)m,
                       cells, hi, L.stall[s], why.c_str(), pc, pd, po);
    }
    if (totals)
      *totals = Format("pending pairs in the awake solute chunks: carry %d, diffuse %d, one-way %d; %d of them with "
                       "their water moving", tc, td, to, movingAwake);
    return n;
  };
  SolLayerR L;
  ReadSolLayerR(c, L);
  const Census c0 = census(L);
  const uint32_t poured0 = L.hdr[kSolMPoured], powder0 = L.hdr[kSolMPourPowder], lost0 = L.hdr[kSolMPourLost],
                 disc0 = L.hdr[kSolMDiscarded], prec0 = L.hdr[kSolMPrecip], conv0 = L.hdr[kSolMConverted];
  // THE FALL AS THE CARRY SEES IT: along the line through the fall's sample,
  // from 8 voxels upstream to 10 down, each column's held target T8, its drop
  // distance r (the shader's rank, from the oracle) and the mirror's water
  // top. A crest that stood over the reach above it (a higher T8) would carry
  // backwards; this line says so without a second run.
  {
    auto orc = makeOracle(L);
    const double fx = dn[0] - up[0], fz = dn[1] - up[1];
    const double fl = std::max(std::hypot(fx, fz), 1e-6);
    std::string prof;
    for (int t = -8; t <= 10; t++) {
      const int x = dn[0] + static_cast<int>(std::lround(fx / fl * t));
      const int z = dn[1] + static_cast<int>(std::lround(fz / fl * t));
      const int t8 = orc->Level8(x, z);
      const int r = t8 == SolOracle::kUnheld ? -1 : orc->DropDist(x, z, t8, 4);
      prof += Format(" %d:%s/%d/y%d", t, t8 == SolOracle::kUnheld ? "-" : std::to_string(t8).c_str(), r,
                     worldmap::WaterColumnAt(x, z, kDefaultSeed).water);
    }
    std::printf("rivers:   V the fall along its line (offset:T8/drop distance/water top):%s\n", prof.c_str());
  }
  // Two pours, on the upper reach's last sample and half way to the fall's:
  // the pour patch (5 x 5, 3 deep) reaches the crest.
  const uint32_t units = static_cast<uint32_t>(BaselineNumber("rivers.soluteUnits", 1600));
  sandvox::support::TickOps pour;
  pour.cells.push_back(CellOp{World::SlotCellIndex(IVec3{up[0], yU + 2, up[1]}), CellOpSolute(ink->species, units)});
  pour.cells.push_back(CellOp{World::SlotCellIndex(IVec3{(up[0] + dn[0]) / 2, yU + 2, (up[1] + dn[1]) / 2}),
                              CellOpSolute(ink->species, units)});
  tick(pour);
  const int carryTicks = static_cast<int>(BaselineNumber("rivers.soluteCarryTicks", 600));
  std::string curve;
  for (int k = 1; k <= carryTicks; k++) {
    tick();
    if (k % 150 == 0) {
      ReadSolLayerR(c, L);
      const Census ck = census(L);
      curve += Format(" %d:%llu/%llu", k, (unsigned long long)(ck.under - std::min(ck.under, c0.under)),
                      (unsigned long long)(ck.mass - std::min(ck.mass, c0.mass)));
    }
  }
  ReadSolLayerR(c, L);
  const Census c1 = census(L);
  const uint32_t poured = L.hdr[kSolMPoured] - poured0;
  const uint32_t disc1 = L.hdr[kSolMDiscarded] - disc0, prec1 = L.hdr[kSolMPrecip] - prec0,
                 conv1 = L.hdr[kSolMConverted] - conv0;
  const int64_t moved1 = int64_t(c1.mass) - int64_t(c0.mass);
  const int64_t ledger1 = int64_t(poured) - int64_t(disc1) - int64_t(prec1) - int64_t(conv1);
  const uint64_t under1 = c1.under - std::min(c1.under, c0.under);
  const double fracBelow = poured ? double(under1) / poured : 0.0;
  // (c): tick on until no awake chunk carries solute, twice in a row. Every
  // 500 ticks the oracle's pending pairs over all solute chunks go on the
  // curve too (awake/carry/diffuse), so a chunk that will not sleep says
  // whether the carry or the diffusion is what is left.
  const int settleMax = static_cast<int>(BaselineNumber("rivers.soluteSettleTicks", 4000));
  int settledAt = -1, zeros = 0, k = 0;
  std::string curve2;
  for (; k < settleMax && zeros < 2; k++) {
    tick();
    if (k % 100 == 99) {
      ReadSolLayerR(c, L);
      const int n = awakeSol(L, nullptr, nullptr);
      if ((k + 1) % 500 == 0) {
        const Pend p = pendingAll(L);
        curve2 += Format(" %d:%d/%d/%d/%d", carryTicks + k + 1, n, movingAwake, p.carry, p.diff);
      }
      if (n == 0) {
        if (zeros++ == 0) settledAt = carryTicks + k + 1;
      } else {
        zeros = 0;
        settledAt = -1;
      }
    }
  }
  ReadSolLayerR(c, L);
  std::string who, totals;
  const int awakeEnd = awakeSol(L, &who, &totals);
  // THE CONTROL INSIDE THE RUN (CLAUDE.md verification rule 6): is the water
  // under the ink moving because of the ink? Every awake chunk of the window,
  // split by "carries solute" and "its water is MOVING" (the CA's MOVE bit),
  // and a 20-tick voxel diff of the awake solute chunks by FIELD -- material
  // pairs that changed, fullness-only words, stain-only words. Moving water
  // in chunks with NO solute is the fixture's own motion, not the carry's.
  std::string control;
  {
    std::vector<uint32_t> fl(kNumSlots, 0);
    rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.DirtyActive(), 0, fl.data(), kNumSlots * 4, "rvSolCtl");
    int all = 0, sol = 0, solMove = 0, cleanMove = 0;
    std::vector<uint32_t> slots;
    std::string cleanWhere;
    for (uint32_t sl = 0; sl < kNumChunks; sl++) {
      if (fl[sl] == 0u) continue;
      all++;
      const bool mv = (fl[sl] & DirtyReasonBit("MOVE")) != 0u;
      if (L.table[sl] != 0u) {
        sol++;
        solMove += mv;
        if (slots.size() < 24) slots.push_back(sl);
      } else if (mv) {
        cleanMove++;
        if (cleanWhere.size() < 300) {
          const IVec3 wc = c.world.SlotToWorldChunk(sl);
          cleanWhere += Format(" (%d,%d,%d)", wc.x, wc.y, wc.z);
        }
      }
    }
    auto grab = [&](std::vector<uint32_t>& out) {
      out.assign(slots.size() * kChunkVol, 0u);
      for (size_t q = 0; q < slots.size(); q++)
        sandvox::ReadVoxelsSync(c.ctx, c.world, slots[q], 1, out.data() + q * kChunkVol, "rvSolDiff");
    };
    std::vector<uint32_t> va, vb;
    grab(va);
    for (int q = 0; q < 20; q++) tick();
    c.ctx.WaitIdle();
    grab(vb);
    std::map<std::pair<uint32_t, uint32_t>, int> pairs;
    int fullOnly = 0, stainOnly = 0;
    for (size_t q = 0; q < va.size(); q++) {
      const uint32_t wa = va[q] & ~0x00FF0000u, wb = vb[q] & ~0x00FF0000u;
      if (wa == wb) continue;
      if ((wa & 0xFFFu) != (wb & 0xFFFu)) pairs[{wa & 0xFFFu, wb & 0xFFFu}]++;
      else if ((wa & 0xF000u) != (wb & 0xF000u)) fullOnly++;
      else stainOnly++;
    }
    std::string mp;
    for (const auto& [k2, v2] : pairs)
      if (mp.size() < 300)
        mp += Format(" %s->%s x%d", k2.first < c.mats.size() ? c.mats[k2.first].name.c_str() : "?",
                     k2.second < c.mats.size() ? c.mats[k2.second].name.c_str() : "?", v2);
    control = Format("; CONTROL: %d chunks awake in the window, %d carry solute (%d of them MOVING), %d MOVING with "
                     "no solute%s; the awake solute chunks over 20 ticks: %zu material pairs changed%s, %d "
                     "fullness-only, %d stain-only words",
                     all, sol, solMove, cleanMove, cleanWhere.c_str(), pairs.size(), mp.c_str(), fullOnly, stainOnly);
  }
  const Pend pEnd = pendingAll(L);
  const Census c2 = census(L);
  const uint32_t powder = L.hdr[kSolMPourPowder] - powder0, lost = L.hdr[kSolMPourLost] - lost0,
                 disc = L.hdr[kSolMDiscarded] - disc0, prec = L.hdr[kSolMPrecip] - prec0,
                 conv = L.hdr[kSolMConverted] - conv0, poured2 = L.hdr[kSolMPoured] - poured0;
  const int64_t moved = int64_t(c2.mass) - int64_t(c0.mass);
  const int64_t ledger = int64_t(poured2) - int64_t(disc) - int64_t(prec) - int64_t(conv);
  const double discMax = BaselineNumber("rivers.soluteDiscardMax", 0.05);
  const double belowMin = BaselineNumber("rivers.soluteBelowMin", 0.5);
  // 1: (c) is "every solute chunk asleep"; 0: asleep, OR THE CARRY HAS
  // TERMINATED: no carried pair (step-downs included) has work left anywhere,
  // awake chunk or asleep -- the claim this pass makes about the carry. What
  // may still be awake is then the LEVEL diffusion at the species' own pace
  // (ink's diffusivity is 8/256, set by the owner so a cloud hangs; it ends by
  // solDiffuse's own d == 0 argument) and the fixture's water itself: the
  // CONTROL in the report counts awake chunks with moving water and NO solute.
  // Measured 2026-10-17: ~4,100 ticks into V the falls reach and main woke on
  // their own (73 chunks moving with no solute in them, evaporation and steam
  // in the diff), so a strict sleep claim there measures the fixture's clock,
  // not the carry.
  const bool needSleep = BaselineNumber("rivers.soluteRequireSleep", 0) != 0;
  const double fracBelow2 = poured2 ? double(c2.under - std::min(c2.under, c0.under)) / poured2 : 0.0;
  const bool aOk = poured == 2u * units && powder == 0 && lost == 0 && fracBelow >= belowMin;
  // (b) at the end of the carry: the layer moved by exactly the ledger and the
  // floor had taken at most discMax of it; at the end, still exact.
  const bool bOk = moved1 == ledger1 && disc1 <= discMax * poured && moved == ledger;
  const bool asleep = settledAt >= 0 && awakeEnd == 0;
  // A few carried pairs may be left where the fixture's own water is still
  // moving (it re-ranks cells it stirs: measured 0 and 2 of the 746 at the
  // start, on two trees whose solute code is identical), never over the fall.
  const int carryLeftMax = static_cast<int>(BaselineNumber("rivers.soluteCarryLeftMax", 4));
  const bool cOk = asleep || (!needSleep && pEnd.carry <= carryLeftMax && pEnd.carryAbove == 0);
  const bool vOk = aOk && bOk && cOk;
  std::printf("rivers: V solute carried: %s (ink poured %u units at (%d,%d) y%d over a %d-voxel fall to y%d (%u as "
              "powder, %u lost); (a) after %d ticks %llu units (%.2f >= %.2f) stand below the fall, tick:below/total%s; "
              "(b) at tick %d the layer moved %lld, ledger %lld (poured %u - discarded %u - precipitated %u - converted "
              "%u), discarded %.3f <= %.2f; at the end moved %lld, ledger %lld (discarded %u, precipitated %u); (c) %s: "
              "solute chunks asleep from tick %d (%d awake at the end), carried pairs with work left %d (%d over or above "
              "the fall), level diffusion %d, one-way %d; %.2f below by then%s%s%s%s%s)\n",
              vOk ? "PASS" : "FAIL", poured, up[0], up[1], yU, yU - yL, yL, powder, lost, carryTicks,
              (unsigned long long)under1, fracBelow, belowMin, curve.c_str(), carryTicks, (long long)moved1,
              (long long)ledger1, poured, disc1, prec1, conv1, poured ? double(disc1) / poured : 0.0, discMax,
              (long long)moved, (long long)ledger, disc, prec, asleep ? "ASLEEP" : (cOk ? "carry TERMINATED (level diffusion and the fixture's own water still awake)" : "NOT settled"), settledAt,
              awakeEnd, pEnd.carry, pEnd.carryAbove, pEnd.diff, pEnd.oneway, fracBelow2,
              curve2.empty() ? "" : "; tick:awake/moving/carry/diffuse", curve2.c_str(), totals.empty() ? "" : "; awake ",
              (totals + control).c_str(), who.empty() ? "" : (" -- awake:" + who).c_str());
  RecordObserved("rivers.observedSoluteBelow", Format("%.3f", fracBelow));
  RecordObserved("rivers.observedSoluteSettledAt", std::to_string(settledAt));
  if (!vOk)
    problems.push_back(Format("V: solute below the fall %.2f (>= %.2f), ledger %lld vs %lld (end %lld vs %lld), "
                              "discarded %u of %u by tick %d, settled at %d (%d awake), carry left %d (%d above)",
                              fracBelow, belowMin, (long long)moved1, (long long)ledger1, (long long)moved,
                              (long long)ledger, disc1, poured, carryTicks, settledAt, awakeEnd, pEnd.carry,
                              pEnd.carryAbove));
  return vOk;
}

}  // namespace

Status GateRivers(Ctx& c, std::string& detail) {
  using sandvox::ReadVoxelsSync;
  using sandvox::ReloadEnvironment;
  using sandvox::SubmitWorldgen;
  const std::string dir = sandvox::AssetDir();
  std::vector<std::string> problems;
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("rivers: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  };

  // ---- S. the spline ----
  {
    std::string why;
    int nc = 0, np = 0;
    const bool ok = CheckSplineVectors(why, nc, np);
    std::printf("rivers: S spline vectors: %s (%d cases, %d points vs tests/spline_vectors.json)%s%s\n",
                ok ? "PASS" : "FAIL", nc, np, ok ? "" : " -- ", why.c_str());
    if (!ok) problems.push_back("S: " + why);
  }

  biomes::BiomeSet set;
  std::string log;
  if (!biomes::LoadBiomeSet(dir, c.mats, set, log)) return fail("biome files did not load: " + log);
  TreeAtlas atlas;
  if (!LoadTreeAtlas(dir + "/trees", c.mats, set, atlas, log)) return fail("tree atlas did not load: " + log);
  const worldmap::WorldMapData real = worldmap::CurrentWorldMap();   // a COPY
  if (!real.Loaded()) return fail("no world map loaded");
  auto matIdOf = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return static_cast<uint32_t>(i);
    return 0u;
  };
  const uint32_t mWater = matIdOf("water"), mIce = matIdOf("ice");

  // ---- the synthetic map ----
  const fs::path tmp = fs::temp_directory_path() /
                       ("sv_rivers_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path mdir = tmp / "worldmap" / "syn";
  const IVec3 org = c.world.WindowOrigin();
  const int ox = org.x * static_cast<int>(kChunk), oz = org.z * static_cast<int>(kChunk);
  const int ylo = org.y * static_cast<int>(kChunk);
  constexpr int L = 6, P = 1 << L, T = worldmap::kTileSide;
  const int X0 = ((ox - 8192) >> 10) << 10, Z0 = ((oz - 8192) >> 10) << 10;
  const int G = ylo + 100;
  const int lakeX = ox + 120, lakeZ = oz + 410;
  auto authored = [&](int i, int k) -> int {
    const double px = X0 + i * P, pz = Z0 + k * P;
    const double u = std::clamp((px - ox) / 512.0, -0.5, 1.5);
    const double v = std::clamp((pz - oz - 256) / 256.0, -1.5, 1.5);
    double h = G + 60 - 70 * u + 40 * std::fabs(v);
    const double d = std::hypot(px - lakeX, pz - lakeZ) / 140.0;
    if (d < 1) h -= 22 * (1 - d * d) * (1 - d * d);
    return static_cast<int>(std::lround(h));
  };
  std::vector<int> hs(size_t(T) * T), bs(size_t(T) * T, 1);
  for (int k = 0; k < T; k++)
    for (int i = 0; i < T; i++) hs[size_t(k) * T + i] = authored(i, k);
  nlohmann::json te = nlohmann::json::object();
  {
    std::ifstream f(dir + "/worldmap/" + real.name + "/map.json");
    nlohmann::json rj;
    try { f >> rj; } catch (...) {}
    if (rj.contains("terrain")) te = rj["terrain"];
  }
  nlohmann::json mj = {
      {"version", 2}, {"name", "syn"},
      {"extent", {{"min", {X0, Z0}}, {"size", {32768, 32768}}}},
      {"siteCellLog2", 10},
      {"planes", {{"height", {{"log2", L}, {"tile", T}, {"default", -300}}},
                  {"biome", {{"log2", 8}, {"tile", T}, {"default", "ocean"}}}}},
      // The sea just under the window's ground, NOT far below it: the heat
      // layer's lapse rate counts altitude from the sea, and a sea 2 km down
      // (the `roads` gate's -20000) puts this valley's nights under freezing --
      // every pool skins over with ice at dusk, which is the climate waking
      // the world, not the rivers (measured: water->ice, 459 chunks @ 500).
      {"seaLevelY", ylo + 40}, {"edgeFadeVox", 2048}, {"warpAmpVox", 0},
      {"biomes", {"ocean", "meadow"}},
      {"terrain", te},
      {"sites", {{{"id", "pad"}, {"kind", "pad"}, {"min", {ox - 256, oz - 256}}, {"max", {ox + 768, oz + 768}}},
                 {{"id", "spawn"}, {"kind", "spawn"}, {"at", {ox + 480, oz + 60}}}}},
      {"rules", nlohmann::json::array()}};
  bool wrote = WriteTileR(mdir / "planes" / "height" / "0_0.svt", worldmap::kPlaneHeight, 0, 0, hs) &&
               WriteTileR(mdir / "planes" / "biome" / "0_0.svt", worldmap::kPlaneBiome, 0, 0, bs);
  {
    std::ofstream f(mdir / "map.json");
    f << mj.dump(2);
    wrote = wrote && static_cast<bool>(f);
  }
  if (!wrote) return fail("could not write the synthetic map under " + tmp.string());
  const std::vector<worldmap::StructurePlacement>* prevOverride = worldmap::StructureOverrideSlot();
  const std::vector<worldmap::StructurePlacement> prevList = prevOverride ? *prevOverride : std::vector<worldmap::StructurePlacement>{};
  static const std::vector<worldmap::StructurePlacement> kNone;
  worldmap::SetStructureOverride(&kNone);
  auto restoreOverride = [&] { worldmap::SetStructureOverride(prevOverride ? &prevList : nullptr); };
  const std::string tdir = tmp.string();
  auto load = [&](const nlohmann::json* j, worldmap::WorldMapData& out, std::string& l) {
    const std::string text = j ? j->dump() : std::string();
    return worldmap::LoadWorldMap(tdir, "syn", set, c.mats.size(), kDefaultSeed, out, l, j ? &text : nullptr);
  };
  auto cleanup = [&] {
    restoreOverride();
    std::error_code ec;
    fs::remove_all(tmp, ec);
  };

  // The lake's level from the dry ground: its floor + 10.
  int lakeLevel = 0;
  {
    worldmap::WorldMapData dry;
    std::string l;
    if (!load(nullptr, dry, l)) { cleanup(); return fail("the dry synthetic map was REFUSED: " + l); }
    worldmap::SetCurrentWorldMap(dry);
    lakeLevel = worldmap::BareGroundHeight(lakeX, lakeZ, kDefaultSeed) + 10;
  }
  // main's centreline, tessellated, so the tributary's mouth lands ON it.
  using KP = std::vector<std::array<double, 2>>;
  const KP mainKnots = {{double(ox + 12), double(oz + 262)}, {double(ox + 170), double(oz + 236)},
                        {double(ox + 330), double(oz + 282)}, {double(ox + 500), double(oz + 256)}};
  const auto mainTess = worldmap::SplineTessellate(mainKnots, false, 16.0);
  std::array<int, 2> joinAt{ox + 262, oz + 256};
  {
    double best = 1e30;
    for (const auto& q : mainTess)
      if (std::fabs(q.x - (ox + 262)) < best) { best = std::fabs(q.x - (ox + 262)); joinAt = {int(std::lround(q.x)), int(std::lround(q.z))}; }
  }
  nlohmann::json rivers = nlohmann::json::array();
  rivers.push_back({{"id", "main"}, {"smooth", true}, {"bed", "gravel"}, {"stepVox", 4},
                    {"points", {{ox + 12, oz + 262, 36, 8}, {ox + 170, oz + 236}, {ox + 330, oz + 282}, {ox + 500, oz + 256, 44, 10}}}});
  rivers.push_back({{"id", "falls"}, {"smooth", false}, {"bed", "gravel"}, {"stepVox", 24}, {"joins", "main"},
                    {"points", {{joinAt[0] + 8, oz + 16, 20, 6}, {joinAt[0] + 14, oz + 120}, {joinAt[0], joinAt[1], 20, 6}}}});
  rivers.push_back({{"id", "creek"}, {"smooth", true}, {"bed", "sand"}, {"stepVox", 4},
                    {"points", {{ox + 36, oz + 506, 12, 3}, {ox + 80, oz + 476}, {ox + 112, oz + 436, 12, 3}}}});
  nlohmann::json lakePts = nlohmann::json::array();
  for (int k = 0; k < 8; k++) {
    const double a = k * 3.14159265358979 / 4;
    const double r = 92 + 10 * std::sin(3 * a);
    lakePts.push_back({lakeX + int(std::lround(r * std::cos(a))), lakeZ + int(std::lround(r * std::sin(a)))});
  }
  nlohmann::json lakes = nlohmann::json::array();
  lakes.push_back({{"id", "mere"}, {"level", lakeLevel}, {"smooth", true}, {"bed", "shore_mud"}, {"shore", "sand"},
                   {"points", lakePts}});
  nlohmann::json roads = nlohmann::json::array();
  roads.push_back({{"id", "ford"}, {"kind", "road"}, {"surface", "cobble"}, {"width", 20}, {"shoulder", 8},
                   {"maxGrade", 15}, {"smooth", true},
                   {"points", {{ox + 430, oz + 40}, {ox + 410, oz + 250}, {ox + 440, oz + 470}}}});
  mj["rivers"] = rivers;
  mj["lakes"] = lakes;
  mj["roads"] = roads;

  // ---- A. load ----
  worldmap::WorldMapData on;
  std::string lon;
  const auto tLoad0 = std::chrono::steady_clock::now();
  if (!load(&mj, on, lon)) { cleanup(); return fail("A: the synthetic river map was REFUSED: " + lon); }
  const double loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tLoad0).count();
  bool unknownRefused = false, cycleRefused = false;
  {
    nlohmann::json bad = mj;
    bad["rivers"][1]["joins"] = "nowhere";
    worldmap::WorldMapData m;
    std::string l;
    unknownRefused = !load(&bad, m, l) && l.find("falls") != std::string::npos && l.find("nowhere") != std::string::npos;
    nlohmann::json bad2 = mj;
    bad2["rivers"][0]["joins"] = "falls";
    std::string l2;
    cycleRefused = !load(&bad2, m, l2) && l2.find("cycle") != std::string::npos;
  }
  auto riverOf = [&](const char* id) -> const worldmap::WorldMapData::River* {
    for (const auto& r : on.rivers)
      if (r.id == id) return &r;
    return nullptr;
  };
  const auto* rMain = riverOf("main");
  const auto* rFalls = riverOf("falls");
  const auto* rCreek = riverOf("creek");
  bool offChannel = false;
  for (const std::string& w : on.warnings) offChannel = offChannel || w.find("outside its channel") != std::string::npos;
  // main RUNS: one reach (no fall), its surface over several voxels of ramp.
  int mainLevels = 0;
  if (rMain) {
    std::set<int> lv;
    for (const auto& p : rMain->line) lv.insert(p[2]);
    mainLevels = static_cast<int>(lv.size());
  }
  const bool aOk = rMain && rFalls && rCreek && on.lakes.size() == 1 && rMain->falls.empty() && mainLevels >= 3 &&
                   !rFalls->falls.empty() &&
                   rFalls->joinIdx >= 0 && rCreek->lakeOut == 0 && !offChannel && unknownRefused && cycleRefused;
  std::printf("rivers: A load: %s (%zu rivers, %zu lakes in %.0f ms; main %d reach(es) / %d pieces over %d surface levels, "
              "falls %d reaches, %zu waterfall(s)%s, creek -> lake %s; unknown join refused %d, cycle refused %d)\n",
              aOk ? "PASS" : "FAIL", on.rivers.size(), on.lakes.size(), loadMs, rMain ? rMain->reaches : -1,
              rMain ? rMain->pieces : -1, mainLevels, rFalls ? rFalls->reaches : -1, rFalls ? rFalls->falls.size() : 0,
              (rFalls && !rFalls->falls.empty()) ? Format(" (first drops %d)", rFalls->falls[0][2]).c_str() : "",
              (rCreek && rCreek->lakeOut == 0) ? "yes" : "NO", unknownRefused, cycleRefused);
  for (const std::string& w : on.warnings) std::printf("rivers:   load warning: %s\n", w.c_str());
  if (!aOk) problems.push_back("A: load / bake shape / refusals");
  if (!rMain || !rFalls || !rCreek || on.lakes.empty()) {
    cleanup();
    worldmap::SetCurrentWorldMap(real);
    return fail(problems.front());
  }

  // ---- B. the bake ----
  {
    int nonMono = 0, notFlat = 0;
    std::string why;
    for (const auto& rv : on.rivers)
      for (size_t i = 1; i < rv.line.size(); i++)
        if (rv.line[i][2] > rv.line[i - 1][2]) {
          if (!nonMono++) why = Format(" %s rises %d -> %d at sample %zu", rv.id.c_str(), rv.line[i - 1][2], rv.line[i][2], i);
        }
    // Every piece starts ON the profile: its end-A surface (eighths) is the
    // top water cell its start sample reports, and its fall is downhill.
    for (const auto& st : on.sites) {
      if (st.kind != worldmap::kSiteRiver) continue;
      const auto& rv = on.rivers[static_cast<size_t>(st.water)];
      if (st.surfB > st.surf && !notFlat++)
        why += Format(" %s: piece %s rises %d -> %d eighths", rv.id.c_str(), st.id.c_str(), st.surf, st.surfB);
      for (const auto& p : rv.line)
        if ((p[0] == st.x && p[1] == st.z && (st.wflags & worldmap::kRivFlatA) == 0 && p[2] != ((st.surf - 1) >> 3))) {
          if (!notFlat++) why += Format(" %s: piece %s surface %d/8, its start sample %d", rv.id.c_str(), st.id.c_str(), st.surf, p[2]);
          break;
        }
    }
    // ...and a reach ENDS on its own surface: the last piece before a
    // waterfall's cut stands at the crest's level there (the next piece's lip),
    // not at the reach below -- lerping to that dug a pit as deep as the fall
    // in front of every crest (worldwater.cpp step 9).
    int pits = 0, fallEnds = 0;
    for (size_t i = 0; i + 1 < on.sites.size(); i++) {
      const auto& st = on.sites[i];
      const auto& nx = on.sites[i + 1];
      if (st.kind != worldmap::kSiteRiver || nx.kind != worldmap::kSiteRiver || st.water != nx.water) continue;
      if ((st.wflags & worldmap::kRivFlatB) == 0 || (nx.wflags & worldmap::kRivFlatA) == 0) continue;
      fallEnds++;
      if (((st.surfB - 1) >> 3) != nx.lipY && !pits++)
        why += Format(" %s: piece %s ends at %d/8 (top cell %d) before a fall whose crest is y%d",
                      on.rivers[static_cast<size_t>(st.water)].id.c_str(), st.id.c_str(), st.surfB, (st.surfB - 1) >> 3, nx.lipY);
    }
    notFlat += pits;
    // The junction: falls' last surface >= main's surface at its nearest sample.
    int mainAtJoin = 0;
    {
      int64_t best = INT64_MAX;
      for (const auto& p : rMain->line) {
        const int64_t dx = p[0] - rFalls->line.back()[0], dz = p[1] - rFalls->line.back()[1];
        if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; mainAtJoin = p[2]; }
      }
    }
    const bool joinOk = rFalls->line.back()[2] >= mainAtJoin;
    const bool creekOk = rCreek->line.back()[2] == lakeLevel;
    // The smooth road: its baked grade (the walked grade is the `roads`
    // gate's claim; here the road crosses a river, whose channel is not road).
    const auto& rd = on.roads.at(0);
    const bool roadOk = rd.smooth && rd.points.size() > 3 && rd.worstGradeQ8 <= rd.maxGrade * 256 + 64;
    const bool bOk = nonMono == 0 && notFlat == 0 && fallEnds > 0 && joinOk && creekOk && roadOk;
    std::printf("rivers: B bake: %s (surfaces monotone: %d rises; pieces on the profile: %d off, %d of %d reach ends "
                "before a fall off their own surface; junction falls %d >= main %d; creek "
                "ends at %d, the lake's level %d; smooth road %zu vertices, baked grade %.2f %% <= %d %%)%s\n",
                bOk ? "PASS" : "FAIL", nonMono, notFlat, pits, fallEnds, rFalls->line.back()[2], mainAtJoin,
                rCreek->line.back()[2], lakeLevel, rd.points.size(), rd.worstGradeQ8 / 256.0, rd.maxGrade, why.c_str());
    if (!bOk) problems.push_back("B: bake" + why);
  }

  // ---- C. the lake, on the CPU ----
  worldmap::SetCurrentWorldMap(on);
  {
    const auto& lk = on.lakes[0];
    int inWet = 0, inWrong = 0, outWet = 0;
    std::string why;
    // Is the column in some river's channel (+2)? Its water is the river's.
    auto inChannel = [&](int x, int z) {
      for (const auto& rv : on.rivers)
        for (const auto& p : rv.line)
          if ((p[0] - x) * (p[0] - x) + (p[1] - z) * (p[1] - z) <= (p[3] + 2) * (p[3] + 2)) return true;
      return false;
    };
    for (int z = lk.z0 - lk.band - 8; z <= lk.z1 + lk.band + 8; z++)
      for (int x = lk.x0 - lk.band - 8; x <= lk.x1 + lk.band + 8; x++) {
        const worldmap::WaterColumn wc = worldmap::WaterColumnAt(x, z, kDefaultSeed);
        const bool in = lk.Inside(x, z);
        // A river channel (the creek's mouth) may hold water too; its surface
        // is the lake's level there by construction.
        if (in) {
          if (wc.h < lakeLevel) {
            inWet++;
            if (wc.water != lakeLevel && !inWrong++) why = Format(" (%d,%d) inside, ground %d, water %d", x, z, wc.h, wc.water);
          } else if (wc.water >= 0 && wc.water != lakeLevel) {
            if (!inWrong++) why = Format(" (%d,%d) inside, dry ground %d holds water %d", x, z, wc.h, wc.water);
          }
        } else if (wc.water >= 0 && !inChannel(x, z)) {
          // Outside the polygon only a river's channel holds water.
          if (!outWet++) why += Format(" (%d,%d) outside the polygon holds water %d", x, z, wc.water);
        }
      }
    const bool cOk = inWet > 1000 && inWrong == 0 && outWet == 0;
    std::printf("rivers: C lake: %s (%d columns inside under level %d hold it, %d wrong; %d wet outside)%s\n", cOk ? "PASS" : "FAIL",
                inWet, lakeLevel, inWrong, outWet, why.c_str());
    if (!cOk) problems.push_back(Format("C: lake inside %d wrong %d outside %d%s", inWet, inWrong, outWet, why.c_str()));
  }

  // ---- I. the current runs downstream (sim/flowfield.h) ----
  // A window centred on main's middle sample: the terrain flow field, solved
  // from the channel's shape, must run along the centreline, downstream, at
  // no less than the authored floor just under the surface there -- and be
  // zero in the air over the river.
  {
    using sandvox::WaterFlow;
    const auto& L = rMain->line;
    const size_t mid = L.size() / 2;
    const size_t ta = mid >= 4 ? mid - 4 : 0, tb = std::min(L.size() - 1, mid + 4);
    float tx = static_cast<float>(L[tb][0] - L[ta][0]), tz = static_cast<float>(L[tb][1] - L[ta][1]);
    const float tl = std::max(std::sqrt(tx * tx + tz * tz), 1e-3f);
    tx /= tl;
    tz /= tl;
    WaterFlow().Clear();
    const int bx = (L[mid][0] - static_cast<int>(kWorldN) / 2) & ~15;
    const int bz = (L[mid][1] - static_cast<int>(kWorldN) / 2) & ~15;
    WaterFlow().UpdateAt(bx, bz, kDefaultSeed);
    const Vec3 at{L[mid][0] + 0.5f, L[mid][2] - 1.5f, L[mid][1] + 0.5f};
    const Vec3 v = WaterFlow().Sample(at);
    const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    const float along = v.x * tx + v.z * tz;
    const float floorCells = sandvox::kRiverMinFlowMs / kVoxelMeters;
    const Vec3 sky = WaterFlow().Sample(Vec3{L[mid][0] + 0.5f, L[mid][2] + 24.5f, L[mid][1] + 0.5f});
    const float skySpeed = std::sqrt(sky.x * sky.x + sky.y * sky.y + sky.z * sky.z);
    const bool iOk = WaterFlow().BlocksSolved() >= 1 && speed >= floorCells * 0.9f &&
                     along >= 0.9f * speed && skySpeed == 0.0f;
    std::printf("rivers: I current: %s (%u flow block(s) solved; at main's middle %.1f cells/s, %.2f of it "
                "downstream, floor %.1f; 24 cells over the surface %.2f)\n",
                iOk ? "PASS" : "FAIL", WaterFlow().BlocksSolved(), speed, speed > 0.0f ? along / speed : 0.0f,
                floorCells, skySpeed);
    if (!iOk) problems.push_back(Format("I: current %.1f cells/s, along %.1f", speed, along));

    // ---- K. the SHAPE decides the speed ----
    // Across the channel: v ~ h^(2/3), so most of the way to the bank (the U
    // profile's shallows) the water runs well under the thalweg's speed.
    {
      const float hw = static_cast<float>(L[mid][3]);
      const Vec3 bank{at.x - tz * 0.8f * hw, at.y, at.z + tx * 0.8f * hw};
      const Vec3 vb = WaterFlow().Sample(bank);
      const float sb = std::sqrt(vb.x * vb.x + vb.z * vb.z);
      const bool kOk = speed > 0.0f && sb < 0.8f * speed;
      std::printf("rivers: K cross-section: %s (thalweg %.1f cells/s, 0.8 of the half-width out %.1f)\n",
                  kOk ? "PASS" : "FAIL", speed, sb);
      if (!kOk) problems.push_back(Format("K: bank %.1f vs thalweg %.1f", sb, speed));
    }

    // ---- J. a window move does not touch the river under the player ----
    // (ticket 2026-10-11_18-39-38: "strobing in the water while moving in it
    // every second or so".) The field is solved on WORLD-anchored blocks and
    // blended by world position, so a window move only changes which nodes
    // are uploaded, never their values: the sample must be bit-identical on
    // the tick of each of three moves along both axes.
    const int moves[4][2] = {{0, 0}, {16, 0}, {16, 16}, {0, 16}};
    Vec3 v0{};
    float worst = 0.0f, firstSpeed = 0.0f;
    std::string jTrace;
    for (int k = 0; k < 4; k++) {
      WaterFlow().UpdateAt(bx + moves[k][0], bz + moves[k][1], kDefaultSeed);
      const Vec3 vk = WaterFlow().Sample(at);
      const float sk = std::sqrt(vk.x * vk.x + vk.y * vk.y + vk.z * vk.z);
      if (k == 0) {
        v0 = vk;
        firstSpeed = sk;
      }
      const float dx = vk.x - v0.x, dy = vk.y - v0.y, dz = vk.z - v0.z;
      worst = std::max(worst, std::sqrt(dx * dx + dy * dy + dz * dz));
      jTrace += Format(" %.2f", sk);
    }
    const bool jOk = firstSpeed >= floorCells * 0.9f && worst == 0.0f;
    std::printf("rivers: J window move: %s (speed at main's middle on each move's own tick:%s cells/s; "
                "largest change %.4f)\n",
                jOk ? "PASS" : "FAIL", jTrace.c_str(), worst);
    if (!jOk) problems.push_back(Format("J: window move changed the current by %.3f (speeds%s)", worst, jTrace.c_str()));
    WaterFlow().Clear();
  }

  // ---- the world on the GPU ----
  std::vector<uint32_t> words;
  bool gpuState = false;
  if (!worldmap::PackWorldMap(set, on, words, log)) {
    problems.push_back("the synthetic map did not pack: " + log);
  } else {
    c.ctx.WaitIdle();
    c.sim.UploadEnvironment(c.ctx.device, c.ctx.queue, atlas, words);
    gpuState = true;
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    std::map<std::pair<int, int>, std::vector<uint32_t>> rows;
    const int m = static_cast<int>(kNChunk) - 1;
    auto voxRaw = [&](int x, int y, int z) -> uint32_t {
      const int cy = y >> 4, cz = z >> 4;
      if (cy < org.y || cy >= org.y + static_cast<int>(kNChunk)) return 0u;
      if (x < ox || z < oz || x >= ox + static_cast<int>(kWorldN) || z >= oz + static_cast<int>(kWorldN)) return 0u;
      auto it = rows.find({cy, cz});
      if (it == rows.end()) {
        std::vector<uint32_t> run(size_t(kNChunk) * kChunkVol);
        ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({0, cy, cz}), kNChunk, run.data(), "rivers");
        it = rows.emplace(std::make_pair(cy, cz), std::move(run)).first;
      }
      const uint32_t local = (uint32_t(z & 15) * kChunk + uint32_t(y & 15)) * kChunk + uint32_t(x & 15);
      return it->second[size_t((x >> 4) & m) * kChunkVol + local];
    };
    auto vox = [&](int x, int y, int z) -> uint32_t { return voxRaw(x, y, z) & 0xFFFu; };
    auto nameOf = [&](uint32_t id) { return id < c.mats.size() ? c.mats[id].name : std::string("?"); };
    auto isWater = [&](uint32_t id) { return id == mWater || id == mIce; };
    auto isBody = [&](uint32_t id) {
      return id != 0u && id < c.mats.size() && (c.mats[id].gpu.klass == CLASS_SOLID || c.mats[id].gpu.klass == CLASS_POWDER);
    };

    // ---- D. CPU == GPU ----
    {
      int cols = 0, hollow = 0, buried = 0, wetCols = 0, waterBad = 0, rampCols = 0;
      std::string why;
      for (int z = oz + 2; z < oz + static_cast<int>(kWorldN) - 2; z += 2)
        for (int x = ox + 2; x < ox + static_cast<int>(kWorldN) - 2; x += 2) {
          const worldmap::WaterColumn wc = worldmap::WaterColumnAt(x, z, kDefaultSeed);
          if (!wc.wet) continue;   // the rivers' and lake's columns: the claim of this gate
          const int h = wc.h;
          if ((h - 1) >> 4 < org.y || (std::max(h, wc.water) + 2) >> 4 >= org.y + static_cast<int>(kNChunk)) continue;
          cols++;
          if (!isBody(vox(x, h, z)) || !isBody(vox(x, h - 1, z))) {
            if (!hollow++ && why.empty())
              why = Format(" (%d,%d): mirror ground y%d, world %s there, %s below", x, z, h, nameOf(vox(x, h, z)).c_str(),
                           nameOf(vox(x, h - 1, z)).c_str());
          }
          if (wc.water > h) {
            wetCols++;
            if (!isWater(vox(x, wc.water, z)) || isWater(vox(x, wc.water + 1, z)) || !isWater(vox(x, h + 1, z))) {
              if (!waterBad++ && why.empty())
                why = Format(" (%d,%d): mirror water y%d..%d, world %s at the top, %s above, %s at the bottom", x, z, h + 1,
                             wc.water, nameOf(vox(x, wc.water, z)).c_str(), nameOf(vox(x, wc.water + 1, z)).c_str(),
                             nameOf(vox(x, h + 1, z)).c_str());
            } else if (vox(x, wc.water, z) == mWater) {
              // The running surface's top cell holds the ramp's eighths.
              const uint32_t f = ((voxRaw(x, wc.water, z) >> 12) & 15u) + 1u;
              if (wc.gap > 0) rampCols++;
              if (f != 8u - static_cast<uint32_t>(wc.gap) && !waterBad++ && why.empty())
                why = Format(" (%d,%d): top water cell y%d holds %u eighths, the mirror says %d", x, z, wc.water, f, 8 - wc.gap);
            }
          } else if (isBody(vox(x, h + 2, z)) && !isBody(vox(x, h + 1, z))) {
            if (!buried++ && why.empty()) why = Format(" (%d,%d): matter at y%d over the mirror's y%d", x, z, h + 2, h);
          }
        }
      const bool dOk = cols > 5000 && wetCols > 2000 && hollow == 0 && buried == 0 && waterBad == 0 && rampCols > 50;
      std::printf("rivers: D GPU vs twin: %s (%d river/lake columns, %d under water, %d with a part-full top on the ramp; "
                  "%d hollow, %d buried, %d water mismatches)%s\n",
                  dOk ? "PASS" : "FAIL", cols, wetCols, rampCols, hollow, buried, waterBad, why.c_str());
      if (!dOk) problems.push_back(Format("D: %d/%d/%d mismatches of %d columns%s", hollow, buried, waterBad, cols, why.c_str()));
    }

    // ---- E. contained ----
    // Every water voxel over every column the mirror gives water (D holds the
    // mirror to the world on those columns): no AIR beside it or under it.
    {
      int water = 0, open = 0, held = 0, heldFalls = 0;
      std::string why;
      // Within a few voxels of a `falls` waterfall's crest (the curtain).
      auto nearFall = [&](int x, int z) {
        for (const auto& f : rFalls->falls)
          if (std::abs(f[0] - x) <= 12 && std::abs(f[1] - z) <= 12) return true;
        return false;
      };
      for (int z = oz + 1; z < oz + static_cast<int>(kWorldN) - 1; z++)
        for (int x = ox + 1; x < ox + static_cast<int>(kWorldN) - 1; x++) {
          const worldmap::WaterColumn wc = worldmap::WaterColumnAt(x, z, kDefaultSeed);
          if (wc.water <= wc.h) continue;
          for (int y = wc.h + 1; y <= wc.water + 1; y++) {
            if (!isWater(vox(x, y, z))) continue;
            water++;
            const int nb[5][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}};
            bool face = false;
            for (const auto& d : nb) {
              if (vox(x + d[0], y + d[1], z + d[2]) != 0u) continue;
              // HELD: air beside, over a held column whose own water stands
              // below it -- the move the held rule refuses.
              if (d[1] == 0) {
                const worldmap::WaterColumn b = worldmap::WaterColumnAt(x + d[0], z + d[2], kDefaultSeed);
                if (b.water >= 0 && b.water > b.h && y > b.water) { face = true; continue; }
              }
              if (!open++) {
                const worldmap::WaterColumn b = worldmap::WaterColumnAt(x + d[0], z + d[2], kDefaultSeed);
                why = Format(" first: water (%d,%d,%d) has air at (%d,%d,%d); mirror there ground %d water %d, next ground %d "
                             "water %d; there:%s; next:%s",
                             x, y, z, x + d[0], y + d[1], z + d[2], wc.h, wc.water, b.h, b.water,
                             worldmap::WaterColumnTrace(x, z, kDefaultSeed).c_str(),
                             worldmap::WaterColumnTrace(x + d[0], z + d[2], kDefaultSeed).c_str());
              }
              break;
            }
            if (face) {
              held++;
              if (nearFall(x, z)) heldFalls++;
            }
          }
        }
      const bool eOk = water > 20000 && open == 0 && heldFalls > 20;
      std::printf("rivers: E contained: %s (%d water voxels over the window, %d with air beside or under that no held "
                  "column refuses; %d held faces (ramp steps, rapids, curtains), %d of them at the falls)%s\n",
                  eOk ? "PASS" : "FAIL", water, open, held, heldFalls, why.c_str());
      if (!eOk) problems.push_back(Format("E: %d of %d water voxels touch air, %d held at the falls%s", open, water, heldFalls, why.c_str()));
    }

    // ---- the picture: build/rivers_top.bmp ----
    {
      const uint32_t W = kWorldN;
      std::vector<uint8_t> rgba(size_t(W) * W * 4, 0);
      for (uint32_t zi = 0; zi < W; zi++)
        for (uint32_t xi = 0; xi < W; xi++) {
          const int x = ox + int(xi), z = oz + int(zi);
          const worldmap::WaterColumn wc = worldmap::WaterColumnAt(x, z, kDefaultSeed);
          int y = std::max(wc.h, wc.water) + 24;
          uint32_t mm = 0;
          for (; y >= G - 60; y--) {
            mm = vox(x, y, z);
            if (mm != 0u) break;
          }
          uint8_t r = 0, g = 0, b = 0;
          if (mm != 0u && mm < c.mats.size()) {
            const uint32_t col = c.mats[mm].gpu.color0;
            r = col & 0xFF; g = (col >> 8) & 0xFF; b = (col >> 16) & 0xFF;
            if (isWater(mm)) {
              int dep = 0;
              while (dep < 40 && isWater(vox(x, y - dep - 1, z))) dep++;
              r = 30; g = static_cast<uint8_t>(std::max(60, 160 - dep * 6)); b = static_cast<uint8_t>(std::max(120, 235 - dep * 4));
            }
            // relief shading from the height, so steps and banks read
            const double sh = 0.75 + 0.5 * std::clamp((y - (G - 20)) / 160.0, 0.0, 1.0);
            r = static_cast<uint8_t>(std::min(255.0, r * sh));
            g = static_cast<uint8_t>(std::min(255.0, g * sh));
            b = static_cast<uint8_t>(std::min(255.0, b * sh));
            // a contour every 4 voxels on land
            if (!isWater(mm) && xi > 0) {
              const int yl = worldmap::WaterColumnAt(x - 1, z, kDefaultSeed).h;
              if ((yl >> 2) != (wc.h >> 2)) { r = r * 3 / 4; g = g * 3 / 4; b = b * 3 / 4; }
            }
          }
          uint8_t* px = &rgba[(size_t(zi) * W + xi) * 4];
          px[0] = r; px[1] = g; px[2] = b; px[3] = 255;
        }
      const bool w = sandvox::WriteBmpFile("build/rivers_top.bmp", rgba, W, W);
      std::printf("rivers: the window from above -> build/rivers_top.bmp%s\n", w ? "" : " (WRITE FAILED)");
    }

    // ---- F. far field + horizon ----
    {
      // `falls` is 2 m wide: at level 5 (32-voxel cells) narrower than a cell,
      // wider than a sixteenth -- the cells its line crosses must be water.
      const auto& fl = rFalls->line;
      const int zc = (fl.front()[1] + fl.back()[1]) / 2;
      DrainFullRefill(c.ctx, c.world, c.sim, IVec3{fl.front()[0] >> 4, G >> 4, zc >> 4});
      constexpr uint32_t kLevel = 5;
      const int sh = static_cast<int>(kLevel + kFarShiftBase), s = 1 << sh;
      const uint32_t slot = (c.mats[mWater].gpu.flags >> kMatFarPalShift) & kMatFarPalMask;
      int on4 = 0, tint = 0;
      std::string why;
      std::set<std::pair<int, int>> seen;
      for (const auto& p : fl) {
        const int cx = p[0] >> sh, cz = p[1] >> sh;
        if (!seen.insert({cx, cz}).second) continue;
        const int xc = (cx << sh) + s / 2, zcc = (cz << sh) + s / 2;
        // the cell's centre within the tint's reach of the line (the rule's own test)
        int64_t best = INT64_MAX;
        for (const auto& q : fl) best = std::min<int64_t>(best, int64_t(q[0] - xc) * (q[0] - xc) + int64_t(q[1] - zcc) * (q[1] - zcc));
        const int reach = p[3] + std::min(s / 2, static_cast<int>(worldmap::kSiteRoadFarTint)) - 2;
        if (best >= int64_t(reach) * reach) continue;
        const worldmap::WaterColumn wcc = worldmap::WaterColumnAt(xc, zcc, kDefaultSeed);
        const int cy = (wcc.h - s / 2) >> sh;
        on4++;
        bool any = false;
        for (int dy = 0; dy <= 1 && !any; dy++)
          any = FarCellSlot(FarVoxByte(c.ctx, c.world, kLevel, {cx, cy + dy, cz})) == slot;
        if (any) tint++;
        else if (why.empty()) why = Format(" first dry: cell (%d,%d,%d), its centre's ground y%d", cx, cy, cz, wcc.h);
      }
      std::vector<uint32_t> hz;
      horizon::Build(on, words, kDefaultSeed, hz);
      auto hzCov = [&](int x, int z) -> int {
        if (hz.size() <= horizon::kHWater || hz[horizon::kHWater] == 0u) return -1;
        const int S = 1 << horizon::kSampleLog2;
        const int i = static_cast<int>(std::lround(double(x - static_cast<int32_t>(hz[horizon::kHX0])) / S));
        const int j = static_cast<int>(std::lround(double(z - static_cast<int32_t>(hz[horizon::kHZ0])) / S));
        return static_cast<int>(hz[hz[horizon::kHWater] + size_t(j) * hz[horizon::kHNX] + size_t(i)] & 0xFFu);
      };
      const int covMain = hzCov(rMain->line[rMain->line.size() / 2][0], rMain->line[rMain->line.size() / 2][1]);
      const int covLake = hzCov(lakeX, lakeZ);
      const bool fOk = on4 >= 4 && tint * 10 >= on4 * 8 && covMain > 0 && covLake > 0;
      std::printf("rivers: F far field: %s (level %u, %d-voxel cells: %d / %d along the 2 m `falls` are water; horizon water "
                  "coverage over main %d / 255, over the lake %d / 255)%s\n",
                  fOk ? "PASS" : "FAIL", kLevel, s, tint, on4, covMain, covLake, fOk ? "" : why.c_str());
      if (!fOk) problems.push_back(Format("F: %d / %d far cells water, horizon %d / %d%s", tint, on4, covMain, covLake, why.c_str()));
    }

    // ---- G. sleep ----
    {
      // A CLEAR sky for the claim: rain wets every exposed cell and dries
      // again, waking the whole window on the weather's clock (measured:
      // 270 awake @ 700, stain-only words) -- the weather's business, and
      // the same over a river or a meadow. The pin is a recorded input.
      const std::string prevPin = weather::Override();
      weather::SetOverride("clear");
      uint32_t t = 94000;
      sandvox::support::TickCursor tick{c, t, IVec3{(ox + 256) >> 4, G >> 4, (oz + 256) >> 4}};
      const uint32_t maxAwake = static_cast<uint32_t>(BaselineNumber("rivers.maxAwake", 32));
      const int maxTicks = static_cast<int>(BaselineNumber("rivers.settleTicks", 3000));
      uint32_t active = kNumSlots;
      int ticks = 0;
      for (; ticks < maxTicks; ticks++) {
        tick();
        if (ticks >= 200 && ticks % 100 == 0) {
          c.ctx.WaitIdle();
          active = sandvox::ReadActiveChunksSync(c.ctx, c.world, c.sim);
          if (active <= maxAwake) break;
        }
      }
      // ...and HOLDS there: a river that only dips under the budget between
      // two wakes is not asleep. Tick on and look again.
      const int hold = static_cast<int>(BaselineNumber("rivers.holdTicks", 600));
      std::string trace;
      for (int k = 0; k < hold; k++) {
        tick();
        if (k % 100 == 99) {
          c.ctx.WaitIdle();
          trace += Format(" %u@%d", sandvox::ReadActiveChunksSync(c.ctx, c.world, c.sim), ticks + k + 1);
        }
      }
      ticks += hold;
      std::printf("rivers:   awake while held:%s\n", trace.c_str());
      c.ctx.WaitIdle();
      active = sandvox::ReadActiveChunksSync(c.ctx, c.world, c.sim);
      std::vector<uint32_t> fl(kNumSlots, 0);
      rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.DirtyActive(), 0, fl.data(), kNumSlots * 4, "riversActive");
      int nearWater = 0, elsewhere = 0;
      std::string where;
      const int mm = static_cast<int>(kNChunk) - 1;
      const int shc = std::countr_zero(kNChunk);
      for (uint32_t i = 0; i < kNumChunks; i++) {
        if (fl[i] == 0) continue;
        const int sx = static_cast<int>(i) & mm, sy = (static_cast<int>(i) >> shc) & mm, sz = static_cast<int>(i) >> (2 * shc);
        const IVec3 wcn{org.x + ((sx - org.x) & mm), org.y + ((sy - org.y) & mm), org.z + ((sz - org.z) & mm)};
        const int cxv = wcn.x * 16 + 8, czv = wcn.z * 16 + 8;
        const worldmap::WaterColumn wc = worldmap::WaterColumnAt(cxv, czv, kDefaultSeed);
        (wc.wet ? nearWater : elsewhere)++;
        if (where.size() < 500)
          where += Format(" (%d,%d,%d)%s dy%+d", wcn.x, wcn.y, wcn.z, wc.wet ? "W" : "", wcn.y * 16 + 8 - wc.h);
      }
      // WHAT is moving in the awake water chunks (CLAUDE.md verification rule
      // 6): their voxels now and 20 ticks on, diffed by field -- the material
      // pairs that changed, and how many words changed only in state / stain.
      if (nearWater > 0) {
        std::vector<uint32_t> slots;
        for (uint32_t i = 0; i < kNumChunks && slots.size() < 48; i++) {
          if (fl[i] == 0) continue;
          const int sx = static_cast<int>(i) & mm, sy = (static_cast<int>(i) >> shc) & mm, sz = static_cast<int>(i) >> (2 * shc);
          const IVec3 wcn{org.x + ((sx - org.x) & mm), org.y + ((sy - org.y) & mm), org.z + ((sz - org.z) & mm)};
          if (worldmap::WaterColumnAt(wcn.x * 16 + 8, wcn.z * 16 + 8, kDefaultSeed).wet) slots.push_back(i);
        }
        auto grab = [&](std::vector<uint32_t>& out) {
          out.assign(slots.size() * kChunkVol, 0u);
          for (size_t k = 0; k < slots.size(); k++)
            ReadVoxelsSync(c.ctx, c.world, slots[k], 1, out.data() + k * kChunkVol, "riversDiff");
        };
        std::vector<uint32_t> a, b;
        grab(a);
        for (int k = 0; k < 20; k++) tick();
        c.ctx.WaitIdle();
        grab(b);
        std::map<std::pair<uint32_t, uint32_t>, int> pairs;
        int stateOnly = 0, stainOnly = 0;
        for (size_t k = 0; k < a.size(); k++) {
          const uint32_t wa = a[k] & ~0x00FF0000u, wb = b[k] & ~0x00FF0000u;   // stamp + excite bits ignored
          if (wa == wb) continue;
          if ((wa & 0xFFFu) != (wb & 0xFFFu)) pairs[{wa & 0xFFFu, wb & 0xFFFu}]++;
          else if ((wa & 0xF000u) != (wb & 0xF000u)) stateOnly++;
          else stainOnly++;
        }
        std::string what;
        std::vector<std::pair<int, std::pair<uint32_t, uint32_t>>> top;
        for (const auto& [k, v] : pairs) top.push_back({v, k});
        std::sort(top.rbegin(), top.rend());
        for (size_t k = 0; k < top.size() && k < 8; k++)
          what += Format(" %s->%s x%d", nameOf(top[k].second.first).c_str(), nameOf(top[k].second.second).c_str(), top[k].first);
        std::printf("rivers:   the %zu awake water chunks over 20 ticks: %zu material pairs changed%s; %d state-only, %d "
                    "stain-only words\n",
                    slots.size(), pairs.size(), what.c_str(), stateOnly, stainOnly);
      }
      const int maxWater = static_cast<int>(BaselineNumber("rivers.maxAwakeWater", 4));
      const bool gOk = active <= maxAwake && nearWater <= maxWater;
      std::printf("rivers: G sleep: %s (%u active chunks after %d ticks, <= %u allowed; %d over water or banks (<= %d), %d "
                  "elsewhere)%s%s\n",
                  gOk ? "PASS" : "FAIL", active, ticks, maxAwake, nearWater, maxWater, elsewhere, nearWater ? " --" : "",
                  nearWater ? where.c_str() : "");
      RecordObserved("rivers.observedAwake", std::to_string(active));
      // A DUG CHANNEL, LINED WITH STONE. Cut into this fixture's ground, a
      // trench's walls are the sediment wedge and the dirt skin, which slide
      // into it at their angle of repose and dam it within a few cells (the
      // first R and S runs: 48% and 10% reached, page faults 0). That is soil
      // mechanics, not the subject; so the gate's channels get a stone floor
      // and a stone wall two cells thick from the floor up WALL_H, and are
      // cleared above the floor DIG_H. The river's own columns are left as
      // they are. `floorOf` is every channel column with its floor's y.
      auto channelOps = [&](const std::map<std::pair<int, int>, int>& floorOf, std::vector<CellOp>& walls,
                            std::vector<CellOp>& dig) {
        constexpr int WALL_H = 24, DIG_H = 30;
        const uint32_t stoneId = matIdOf("stone");
        std::map<std::pair<int, int>, int> wallFloor;
        for (const auto& [xz, fy] : floorOf)
          for (int dz = -2; dz <= 2; dz++)
            for (int dx = -2; dx <= 2; dx++) {
              const std::pair<int, int> q{xz.first + dx, xz.second + dz};
              if (floorOf.count(q)) continue;
              auto it = wallFloor.find(q);
              if (it == wallFloor.end()) wallFloor[q] = fy;
              else it->second = std::min(it->second, fy);
            }
        for (const auto& [q, fy] : wallFloor) {
          if (worldmap::WaterColumnAt(q.first, q.second, kDefaultSeed).water >= 0) continue;   // the river's own
          for (int y = fy - 1; y <= fy + WALL_H; y++)
            walls.push_back(CellOp{World::SlotCellIndex(IVec3{q.first, y, q.second}), stoneId});
        }
        for (const auto& [xz, fy] : floorOf) {
          if (worldmap::WaterColumnAt(xz.first, xz.second, kDefaultSeed).water < 0)
            walls.push_back(CellOp{World::SlotCellIndex(IVec3{xz.first, fy - 1, xz.second}), stoneId});
          for (int y = fy; y <= fy + DIG_H; y++) dig.push_back(CellOp{World::SlotCellIndex(IVec3{xz.first, y, xz.second}), 0u});
        }
      };

      // ---- R. running: a crater refills, a breach floods ----
      // The reservoir is infinite (sim_step.wgsl HELD WATER): main's surface
      // scooped out must come back to the profile, and a trench cut through
      // its bank must fill from the river to the river's level. Both after G,
      // so the world they disturb is the settled one G just measured.
      {
        const auto& Lm = rMain->line;
        // The crater: a 7 x 7 box down to 4 cells under the surface, at the
        // middle of main.
        const auto& pc = Lm[Lm.size() / 2];
        const int cx = pc[0], cz = pc[1], cTop = pc[2];
        std::vector<CellOp> crater;
        for (int dz = -3; dz <= 3; dz++)
          for (int dx = -3; dx <= 3; dx++)
            for (int y = cTop - 3; y <= cTop; y++)
              crater.push_back(CellOp{World::SlotCellIndex(IVec3{cx + dx, y, cz + dz}), 0u});
        // The breach: from inside the channel, out across the bank and 12
        // voxels past it, 3 wide, its floor 2 cells under main's surface at the
        // river and falling away from it (a dead end the river runs into, fills
        // and then rests against).
        const auto& pb = Lm[(Lm.size() * 3) / 4];
        const auto& pb0 = Lm[(Lm.size() * 3) / 4 - 2];
        const auto& pb1 = Lm[(Lm.size() * 3) / 4 + 2];
        double tx = pb1[0] - pb0[0], tz = pb1[1] - pb0[1];
        const double tl = std::max(std::hypot(tx, tz), 1e-6);
        tx /= tl;
        tz /= tl;
        const double nx = -tz, nz = tx;   // across the channel
        const int bTop = pb[2], bFloor = pb[2] - 2;
        const int bankW = static_cast<int>(std::lround(1.5 * 2.0 * pb[3]));
        const int r0 = pb[3] - 3, r1 = pb[3] + bankW + 12;
        std::set<std::pair<int, int>> trenchCols;
        std::map<std::pair<int, int>, int> trenchFloor;
        for (int r = r0; r <= r1; r++)
          for (int w = -1; w <= 1; w++) {
            const int x = pb[0] + static_cast<int>(std::lround(nx * r + tx * w));
            const int z = pb[1] + static_cast<int>(std::lround(nz * r + tz * w));
            if (!trenchCols.insert({x, z}).second) continue;
            // Falling away from the river one voxel in six, as a breach does:
            // the river runs into it under gravity, not by levelling.
            trenchFloor[{x, z}] = bFloor - (r - r0) / 6;
          }
        (void)bTop;
        std::vector<CellOp> trenchWalls, trench;
        channelOps(trenchFloor, trenchWalls, trench);
        // Columns of the trench that are NOT the river's own (outside the
        // channel and the crest): where any water must have come from the
        // breach.
        std::vector<std::pair<int, int>> outCols;
        for (const auto& [x, z] : trenchCols) {
          const worldmap::WaterColumn wc = worldmap::WaterColumnAt(x, z, kDefaultSeed);
          if (wc.water < 0) outCols.push_back({x, z});
        }
        sandvox::support::TickOps first;
        sandvox::support::TickOps lining;
        lining.cells = trenchWalls;
        tick(lining);
        first.cells = crater;
        first.cells.insert(first.cells.end(), trench.begin(), trench.end());
        tick(first);
        const int runTicks = static_cast<int>(BaselineNumber("rivers.runTicks", 600));
        uint32_t awakeMax = 0;
        for (int k = 1; k < runTicks; k++) {
          tick();
          if (k % 60 == 0) {
            c.ctx.WaitIdle();
            awakeMax = std::max(awakeMax, sandvox::ReadActiveChunksSync(c.ctx, c.world, c.sim));
          }
        }
        c.ctx.WaitIdle();
        rows.clear();
        // The crater, cell by cell, against the profile it was cut from.
        int craterCells = 0, craterBack = 0;
        for (int dz = -3; dz <= 3; dz++)
          for (int dx = -3; dx <= 3; dx++) {
            const worldmap::WaterColumn wc = worldmap::WaterColumnAt(cx + dx, cz + dz, kDefaultSeed);
            for (int y = cTop - 3; y <= cTop; y++) {
              if (y <= wc.h || y > wc.water) continue;
              craterCells++;
              if (vox(cx + dx, y, cz + dz) == mWater) craterBack++;
            }
          }
        // The trench: the columns the river does not own, and how many of
        // them the river has reached (water on the trench floor). A flat
        // dead end fills by the CA's levelling, which is slow at the far end
        // (diffusive), so the claim is REACH, not the brim.
        int outCells = 0, outWet = 0;
        for (const auto& [x, z] : outCols) {
          const uint32_t w = voxRaw(x, trenchFloor[{x, z}], z);
          if ((w & 0xFFFu) != 0u && (w & 0xFFFu) != mWater) continue;   // not dug (the floor is the ground there)
          outCells++;
          if ((w & 0xFFFu) == mWater) outWet++;
        }
        const double craterFrac = craterCells ? double(craterBack) / craterCells : 0.0;
        const double trenchFrac = outCells ? double(outWet) / outCells : 0.0;
        const double craterMin = BaselineNumber("rivers.craterRefillMin", 0.95);
        const double trenchMin = BaselineNumber("rivers.breachReachMin", 0.8);
        const bool rOk = craterCells > 50 && craterFrac >= craterMin && outCells > 40 && trenchFrac >= trenchMin;
        std::printf("rivers: R running: %s (after %d ticks: the crater %d / %d cells water again (%.2f >= %.2f); the "
                    "breach reached %d / %d trench columns past the bank (%.2f >= %.2f); peak %u chunks awake)\n",
                    rOk ? "PASS" : "FAIL", runTicks, craterBack, craterCells, craterFrac, craterMin, outWet, outCells,
                    trenchFrac, trenchMin, awakeMax);
        RecordObserved("rivers.observedBreachFill", Format("%.3f", trenchFrac));
        if (!rOk)
          problems.push_back(Format("R: crater %d/%d, breach %d/%d", craterBack, craterCells, outWet, outCells));
      }
      // ---- S. a stream becomes held (sim_stream.wgsl) ----
      // A CHANNEL dug from the lake down to main -- the player's case: a lake
      // drained into a river. The lake stands ~15 voxels over main and the
      // land between them is ~50 columns, so the channel RUNS (a couple of
      // eighths of fall per column, where the CA moves water by gravity, not
      // by levelling). Fed by the lake's reservoir at the top and drained into
      // main's at the bottom, it is the conveyor phase 1 took off the authored
      // rivers: a stream that never sleeps on its own. It must (a) run,
      // (b) be ADOPTED column by column and fall asleep, and (c) once its
      // mouth is plugged with stone, be RELEASED and drain.
      {
        const auto& Lm = rMain->line;
        size_t im = 0;
        for (size_t i = 0; i < Lm.size(); i++)
          if (std::abs(Lm[i][0] - lakeX) < std::abs(Lm[im][0] - lakeX)) im = i;
        const int zMain = Lm[im][1], hwM = Lm[im][3], topM = Lm[im][2];
        const int zLake = lakeZ - 70;   // inside the polygon (radius >= 82)
        const int dir = zLake > zMain ? 1 : -1;
        const int zA = zLake, zB = zMain + dir * (hwM - 3);
        const int yA = lakeLevel - 2, yB = topM - 1;
        struct PP { int x, z, y; };
        std::vector<PP> path;
        const int n = std::abs(zB - zA);
        for (int k = 0; k <= n; k++)
          path.push_back({lakeX, zA - dir * k, yA + static_cast<int>(std::lround(double(yB - yA) * k / std::max(n, 1)))});
        std::map<std::pair<int, int>, int> floorOf;
        for (const PP& p : path)
          for (int dx = -1; dx <= 1; dx++) {
            auto it = floorOf.find({p.x + dx, p.z});
            if (it == floorOf.end()) floorOf[{p.x + dx, p.z}] = p.y;
            else it->second = std::min(it->second, p.y);
          }
        std::vector<CellOp> byWalls, dig;
        channelOps(floorOf, byWalls, dig);
        std::vector<std::pair<int, int>> byCols;   // the channel's own columns (not the lake's, not main's)
        for (const auto& [xz, fy] : floorOf)
          if (worldmap::WaterColumnAt(xz.first, xz.second, kDefaultSeed).water < 0) byCols.push_back(xz);
        // The mouth to plug later: the first channel rows past the lake's water.
        std::set<std::pair<int, int>> plugCols;
        {
          int rows = 0;
          for (const PP& p : path) {
            if (worldmap::WaterColumnAt(p.x, p.z, kDefaultSeed).water >= 0) continue;
            for (int dx = -1; dx <= 1; dx++) plugCols.insert({p.x + dx, p.z});
            if (++rows >= 3) break;
          }
        }
        // The chunks the channel's own columns lie in, at their floors.
        std::set<uint32_t> byChunks;
        for (const auto& xz : byCols) {
          const int fy = floorOf[xz];
          for (int y = fy - 1; y <= fy + 3; y++)
            byChunks.insert(World::SlotChunkIndex(IVec3{xz.first >> 4, y >> 4, xz.second >> 4}));
        }
        // The still table's dynamic words for the channel's columns. `floodMax`:
        // the highest FLOOD COUNT among the adopted ones (sim_stream.wgsl, [3]
        // bits 12..23, released at 4000) -- the reporter for a column that
        // stands over its target (a release by flood names itself here, not as
        // a bare drop in the adopted count).
        const rhi::Buffer& still = c.sim.StillWaterBuffer();
        uint32_t floodMax = 0;
        // ATTRIBUTION (phase 3, CLAUDE.md rule 6): how many of the adopted
        // columns are POOL streams' ([3] bit 24: rooted on a finite source,
        // THE CONDUIT), and the most-flooded column's record -- where it is,
        // its hop, POOL or map, its target and the flood count.
        int poolN = 0;
        std::string floodWho;
        // CYCLING (2026-10-17, sim_stream.wgsl STREAMS ADOPT ONLY WHEN
        // NATURAL): sim_stream's own per-column counters (STREAM_OBS word 6:
        // releases, bits 0..7 | adoptions, 8..15; word 7 the release reasons, a byte
        // each: support | flood | event | drained), the most-released column.
        int relMaxS = 0, relSumS = 0, adoptSumS = 0;
        std::string relWhoS, whyS, relWhyS;
        std::map<std::pair<int, int>, int> evPrevS;
        auto adoptedCount = [&]() {
          std::vector<uint32_t> tab(size_t(kWorldN) * kWorldN * 4u);
          rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, still, 0, tab.data(), tab.size() * 4, "riversStill");
          const size_t kObsS = size_t(kWorldN) * kWorldN * 7u + 4u;
          std::vector<uint32_t> obs(size_t(kWorldN) * kWorldN * 8u);
          rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, still, kObsS * 4u, obs.data(), obs.size() * 4, "riversStillObs");
          relMaxS = relSumS = adoptSumS = 0;
          relWhoS.clear();
          // WHY NOT (sim_stream.wgsl STREAM_OBS [6] bits 24..31, the first test
          // the column's last visit failed): a histogram over the channel's
          // unheld columns, the longest steady run (windows) and the newest
          // event at or below any of them (ticks ago).
          int whyN[16] = {};
          uint32_t runMax = 0, ageMin = 0xFFFFFFFFu;
          const uint32_t now16 = std::max((tick.t >> 4) & 0xFFFFu, 1u);
          for (const auto& xz : byCols) {
            const size_t ci = size_t(xz.second & int(kWorldN - 1)) * kWorldN + size_t(xz.first & int(kWorldN - 1));
            if (tab[ci * 4 + 2] == 0u) {
              whyN[(obs[ci * 8 + 6] >> 24) & 15u]++;
              runMax = std::max(runMax, obs[ci * 8] >> 20);
              const uint32_t f = obs[ci * 8 + 2] & 0xFFFFu;
              if (f != 0u) ageMin = std::min(ageMin, ((now16 - f) & 0xFFFFu) * 16u);
            }
          }
          whyS.clear();
          for (int w = 0; w < 16; w++)
            if (whyN[w]) whyS += Format(" %d:%d", w, whyN[w]);
          // ...and WHO made the events since the last sample (each column's own
          // fine-event count, STREAM_OBS [6] bits 16..23, diffed): along the
          // channel from the lake (0) to main (n-1), its distance from the lake.
          {
            std::string who;
            int nWho = 0;
            for (size_t i = 0; i < byCols.size(); i++) {
              const auto& xz = byCols[i];
              const size_t ci = size_t(xz.second & int(kWorldN - 1)) * kWorldN + size_t(xz.first & int(kWorldN - 1));
              const int n = static_cast<int>((obs[ci * 8 + 6] >> 16) & 0xFFu);
              int& p = evPrevS[xz];
              if (n > p && nWho++ < 4) who += Format(" (%d,%d)+%d", xz.first, xz.second, n - p);
              p = n;
            }
            if (nWho) whyS += Format(" events from %d:%s", nWho, who.c_str());
          }
          whyS += Format(" run<=%u newest-event %s", runMax, ageMin == 0xFFFFFFFFu ? "none" : std::to_string(ageMin).c_str());
          // ...and every release by reason (STREAM_OBS [7]: support | flood | - |
          // drained, a byte each) plus, for the support releases, what the
          // column's neighbours were at the last one (byte 2, streamSupportWhy:
          // bit 0 a record under the target, 1 a hop not under ours, 2 a wall at
          // the row, 3 no record at all; high nibble the column's own hop),
          // lake-side first.
          relWhyS.clear();
          {
            int byReason[4] = {};
            std::map<uint32_t, int> lostWhy;
            int firstRelZ = 0;
            uint32_t firstRel = 0;
            for (const auto& xz : byCols) {
              const size_t ci = size_t(xz.second & int(kWorldN - 1)) * kWorldN + size_t(xz.first & int(kWorldN - 1));
              const uint32_t o7 = obs[ci * 8 + 7];
              for (int b = 0; b < 4; b++) byReason[b] += static_cast<int>((o7 >> (8 * b)) & 0xFFu);
              if (o7 & 0xFFu) lostWhy[(o7 >> 16) & 0xFFu]++;
              const int dz = std::abs(xz.second - zA);
              if ((obs[ci * 8 + 6] & 0xFFu) != 0u && (firstRel == 0u || dz < firstRelZ)) {
                firstRelZ = dz;
                firstRel = o7;
                relWhyS = Format(" lake-most (%d,%d) 0x%08x;", xz.first, xz.second, o7);
              }
            }
            relWhyS += Format(" by reason support %d flood %d drained %d; support lost as", byReason[0], byReason[1],
                              byReason[3]);
            for (const auto& [k, v] : lostWhy) relWhyS += Format(" %02x:%d", k, v);
            // WHERE the unheld columns are: (x, distance from the lake end)
            // why/run, lake-side first, at most 24.
            std::vector<std::pair<int, std::string>> un;
            for (const auto& xz : byCols) {
              const size_t ci = size_t(xz.second & int(kWorldN - 1)) * kWorldN + size_t(xz.first & int(kWorldN - 1));
              if (tab[ci * 4 + 2] != 0u) continue;
              un.push_back({std::abs(xz.second - zA) * 4 + (xz.first - lakeX + 1),
                            Format(" (%d,+%d)%u/%u", xz.first, std::abs(xz.second - zA), (obs[ci * 8 + 6] >> 24) & 15u,
                                   obs[ci * 8] >> 20)});
            }
            std::sort(un.begin(), un.end());
            relWhyS += Format("; %zu unheld (x,+dz)why/run:", un.size());
            for (size_t i = 0; i < un.size() && i < 24; i++) relWhyS += un[i].second;
          }
          for (const auto& xz : byCols) {
            const size_t ci = size_t(xz.second & int(kWorldN - 1)) * kWorldN + size_t(xz.first & int(kWorldN - 1));
            const int rel = static_cast<int>(obs[ci * 8 + 6] & 0xFFu);
            relSumS += rel;
            adoptSumS += static_cast<int>((obs[ci * 8 + 6] >> 8) & 0xFFu);
            if (rel > relMaxS) {
              relMaxS = rel;
              relWhoS = Format(" (%d,%d) reasons 0x%08x", xz.first, xz.second, obs[ci * 8 + 7]);
            }
          }
          int k = 0;
          floodMax = 0;
          poolN = 0;
          floodWho.clear();
          for (const auto& xz : byCols) {
            const size_t si = (size_t(xz.second & int(kWorldN - 1)) * kWorldN + size_t(xz.first & int(kWorldN - 1))) * 4u;
            if (tab[si + 2] != 0u) {
              k++;
              if (tab[si + 3] & (1u << 24)) poolN++;
              const uint32_t fl = (tab[si + 3] >> 12) & 4095u;
              if (fl > floodMax || floodWho.empty()) {
                floodWho = Format("(%d,%d) hop %u %s t8 %d", xz.first, xz.second, tab[si + 2] >> 20,
                                  (tab[si + 3] & (1u << 24)) ? "POOL" : "map", static_cast<int>(tab[si + 2] << 12) >> 12);
              }
              floodMax = std::max(floodMax, fl);
            }
          }
          return k;
        };
        auto wetCount = [&]() {
          rows.clear();
          int k = 0;
          for (const auto& xz : byCols)
            if (vox(xz.first, floorOf[xz], xz.second) == mWater) k++;
          return k;
        };
        uint32_t awakeWhy = 0;   // the dirty reasons of the awake channel chunks (OR)
        auto awakeOn = [&]() {
          std::vector<uint32_t> fl2(kNumSlots, 0);
          rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.DirtyActive(), 0, fl2.data(), kNumSlots * 4, "riversActiveS");
          int k = 0;
          awakeWhy = 0;
          for (uint32_t ci : byChunks)
            if (ci < kNumSlots && fl2[ci] != 0u) {
              k++;
              awakeWhy |= fl2[ci];
            }
          return k;
        };
        // The MPM seam's held-water census (sim_fluid_seam.wgsl STILL_DIAG
        // [8..14]: held water at rest a trigger fired on, by trigger, and the
        // targets it stood in), read before and after the run: which body's
        // water the seam's held rule refuses while the stream runs.
        uint32_t seamDiag0[7] = {}, seamDiag1[7] = {};
        const uint64_t seamDiagOff = (uint64_t(kWorldN) * kWorldN * 4u + 4u + 11u * uint64_t(kWorldN) * kWorldN + 8u) * 4u;
        c.ctx.WaitIdle();
        rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.StillWaterBuffer(), seamDiagOff, seamDiag0,
                              sizeof seamDiag0, "riversSeamDiag");
        sandvox::support::TickOps byLining;
        byLining.cells = byWalls;
        tick(byLining);
        sandvox::support::TickOps first;
        first.cells = dig;
        tick(first);
        std::printf("rivers:   S channel: lake (%d,%d) y%d -> main (%d,%d) y%d, %zu columns (%zu its own), %zu wall ops, "
                    "%zu dig ops\n",
                    lakeX, zA, yA, lakeX, zB, yB, floorOf.size(), byCols.size(), byWalls.size(), dig.size());
        const int streamTicks = static_cast<int>(BaselineNumber("rivers.streamTicks", 1800));
        std::string curve;
        int wet = 0, adopted = 0, awake = 0;
        // FLAPPING: the columns the adopted count LOSES between samples while
        // the stream runs. Adoption runs from the source down and nothing is
        // plugged yet, so a drop is a release cascade (one release cascades
        // down the whole stream) -- measured monotone (0 lost) when it works;
        // a first cut of the flood release took it to 22, 45, 27, 15, 69, 12 ...
        int lost = 0;
        for (int k = 1; k < streamTicks; k++) {
          tick();
          if (k % 150 == 0) {
            c.ctx.WaitIdle();
            wet = wetCount();
            const int prevAdopted = adopted;
            adopted = adoptedCount();
            lost += std::max(0, prevAdopted - adopted);
            awake = awakeOn();
            curve += Format(" %d:%d/%d(%dP)/%d/%u [why%s]", k, wet, adopted, poolN, awake, floodMax, whyS.c_str());
          }
        }
        c.ctx.WaitIdle();
        wet = wetCount();
        adopted = adoptedCount();
        awake = awakeOn();
        const int relMaxRun = relMaxS;
        const std::string relRun = Format("; while running %d releases / %d adoptions, at most %d of one column%s;%s", relSumS,
                                          adoptSumS, relMaxS, relWhoS.c_str(), relWhyS.c_str());
        std::string runWho = Format("; %d POOL; the most flooded: %s; awake by", poolN, floodWho.c_str());
        for (int b = 0; b < kDirtyReasonBits; b++)
          if ((awakeWhy >> b) & 1u) runWho += Format(" %s", kDirtyReasonName[b]);
        const int nBy = static_cast<int>(byCols.size());
        // SANDVOX_RIVERS_SHOT=1: pictures of the ADOPTED stream before the
        // plug -- the look iteration of raymarch.wgsl's THE FROZEN STREAM
        // PLAYS BACK and heldSideKind (2026-10-17), which needs a held
        // player-made stream on screen and has no other fixture. From over
        // the channel's upper third, looking down it: noon at render time
        // 10.0 s and 10.25 s (two samples of the flicker apart), and
        // midnight. build/rivers_stream_{day,dayb,night}.bmp. Draws only.
        if (std::getenv("SANDVOX_RIVERS_SHOT") && !path.empty()) {
          using namespace sandvox;
          // The middle of the channel's own run (not the lake's, not main's).
          std::vector<size_t> own;
          for (size_t i = 0; i < path.size(); i++)
            if (worldmap::WaterColumnAt(path[i].x, path[i].z, kDefaultSeed).water < 0) own.push_back(i);
          const PP& p = path[own.empty() ? path.size() / 2 : own[own.size() / 2]];
          // High enough to clear the cut's banks (the channel is dug through
          // the hill between the lake and main), looking steeply down it.
          // The value is "back,up,pitchDeg" (voxels upstream of that point,
          // voxels over the channel floor, degrees; "1" = the defaults), so
          // the framing can be moved without a rebuild.
          float back = 24.0f, up = 48.0f, pitchDeg = -58.0f;
          std::sscanf(std::getenv("SANDVOX_RIVERS_SHOT"), "%f,%f,%f", &back, &up, &pitchDeg);
          if (back < 2.0f && up == 48.0f) back = 24.0f;   // "1"
          const Vec3 eye{static_cast<float>(p.x) + 0.5f, static_cast<float>(p.y) + up,
                         static_cast<float>(p.z) + 0.5f + static_cast<float>(dir) * back};
          Camera cam;
          // Camera::Forward is (cos yaw, ., sin yaw): downstream, -dir in z.
          cam.yaw = dir > 0 ? -1.5707963f : 1.5707963f;
          cam.pitch = pitchDeg * 0.0174532925f;
          uint32_t noonT = 0, midT = 0;
          float bestUp = -2.0f, bestDown = 2.0f;
          for (uint32_t t = 0; t < 200000u; t += 64u) {
            const float up = ComputeSky(CurrentTuning(), (double)t).sunDir[1];
            if (up > bestUp) { bestUp = up; noonT = t; }
            if (up < bestDown) { bestDown = up; midT = t; }
          }
          const uint32_t W = c.width, H = c.height;
          auto shoot = [&](uint32_t skyTick, float time, const char* file) {
            rhi::Buffer buf = CreateBuffer(c.ctx.device, (uint64_t)W * H * 4,
                                           rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "riversShot");
            for (uint32_t f = 0; f < 6; f++) {
              WriteRenderParams(c.ctx.queue, c.world, eye, cam, (float)W / H, true, time, kFarFogDensity, (float)H,
                                skyTick);
              rhi::CommandEncoder enc = c.ctx.device.CreateCommandEncoder();
              c.sim.EncodeShadowResolve(enc);
              rhi::RenderPass rp = c.sim.BeginRenderPass(enc, c.view, rhi::TextureFormat::RGBA8Unorm, W, H);
              c.sim.DrawWorld(rp);
              rp.End();
              if (f == 5) {
                rhi::TexelCopyTexture srcT{};
                srcT.texture = c.offscreen;
                rhi::TexelCopyBuffer dstB{};
                dstB.buffer = buf;
                dstB.bytesPerRow = W * 4;
                dstB.rowsPerImage = H;
                rhi::Extent3D ext{W, H, 1};
                enc.CopyTextureToBuffer(srcT, dstB, ext);
              }
              c.ctx.queue.Submit(enc.Finish());
            }
            std::vector<uint8_t> px((size_t)W * H * 4, 0);
            if (rhi::ReadBufferBlocking(c.ctx.device, buf, 0, px.data(), px.size()))
              WriteBmpFile(file, px, W, H);
          };
          shoot(noonT, 10.0f, "build/rivers_stream_day.bmp");
          shoot(noonT, 10.25f, "build/rivers_stream_dayb.bmp");
          shoot(midT, 10.0f, "build/rivers_stream_night.bmp");
          std::printf("rivers: S shots at eye (%.1f, %.1f, %.1f), %d of %d columns adopted\n", eye.x, eye.y, eye.z,
                      adopted, nBy);
        }
        // (c) the plug: stone over the mouth, from below the channel floor up.
        std::vector<CellOp> plug;
        const uint32_t stoneW = matIdOf("stone");
        for (const auto& xz : plugCols)
          for (int y = yA - 3; y <= yA + 8; y++) plug.push_back(CellOp{World::SlotCellIndex(IVec3{xz.first, y, xz.second}), stoneW});
        sandvox::support::TickOps plugOps;
        plugOps.cells = plug;
        tick(plugOps);
        const int drainTicks = static_cast<int>(BaselineNumber("rivers.streamDrainTicks", 900));
        std::string curve2;
        for (int k = 1; k < drainTicks; k++) {
          tick();
          if (k % 150 == 0) {
            c.ctx.WaitIdle();
            const int wc2 = wetCount(), ac2 = adoptedCount();
            curve2 += Format(" %d:%d/%d(%dP)", k, wc2, ac2, poolN);
          }
        }
        c.ctx.WaitIdle();
        const int adoptedAfter = adoptedCount();
        const int wetAfter = wetCount();
        const double adoptMin = BaselineNumber("rivers.streamAdoptMin", 0.5);
        const int awakeMaxS = static_cast<int>(BaselineNumber("rivers.streamAwakeMax", 2));
        const double releaseMax = BaselineNumber("rivers.streamReleaseMax", 0.2);
        const double flapMax = BaselineNumber("rivers.streamFlapMax", 0.1);
        const int relPerCol = static_cast<int>(BaselineNumber("rivers.streamReleasePerColMax", 1));
        const bool sOk = nBy > 50 && wet >= nBy / 2 && adopted >= static_cast<int>(adoptMin * nBy) && awake <= awakeMaxS &&
                         lost <= static_cast<int>(flapMax * nBy) && adoptedAfter <= static_cast<int>(releaseMax * nBy) &&
                         relMaxRun <= relPerCol;
        runWho += relRun;
        std::printf("rivers: S stream held: %s (channel %d columns: after %d ticks %d wet, %d adopted (>= %.2f), %d of its "
                    "%zu chunks awake (<= %d), %d released while running (<= %.2f); plugged, %d ticks on: %d adopted "
                    "(<= %.2f), %d wet; tick:wet/adopted/awake/floodMax%s%s; after the plug tick:wet/adopted%s)\n",
                    sOk ? "PASS" : "FAIL", nBy, streamTicks, wet, adopted, adoptMin, awake, byChunks.size(), awakeMaxS, lost,
                    flapMax, drainTicks, adoptedAfter, releaseMax, wetAfter, curve.c_str(), runWho.c_str(), curve2.c_str());
        c.ctx.WaitIdle();
        rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.StillWaterBuffer(), seamDiagOff, seamDiag1,
                              sizeof seamDiag1, "riversSeamDiag");
        std::printf("rivers:   S held water at rest a trigger fired on (eighths): fall %u perch %u step %u shell %u wake %u; "
                    "targets T8 %u..%u (the lake's %d, main's %d)\n",
                    seamDiag1[0] - seamDiag0[0], seamDiag1[1] - seamDiag0[1], seamDiag1[2] - seamDiag0[2],
                    seamDiag1[3] - seamDiag0[3], seamDiag1[4] - seamDiag0[4], 0x7FFFFFFFu - seamDiag1[5], seamDiag1[6],
                    8 * lakeLevel + 8, 8 * topM + 8);
        RecordObserved("rivers.observedStreamAdopted", std::to_string(adopted));
        if (!sOk)
          problems.push_back(Format("S: channel %d cols, %d wet, %d adopted, %d awake, %d released while running; after plug "
                                    "%d adopted", nBy, wet, adopted, awake, lost, adoptedAfter));
      }
      // ---- V. the held flow carries solute (sim_solute.wgsl), after S ----
      PassSoluteCarried(c, tick, *rFalls, problems);
      // ---- X. ... and drifts downstream on a level reach ----
      PassSoluteDrift(c, tick, *rMain, problems);
      // ---- W. gravel poured into the river does not lift it onto the pile ----
      // (After S: a pile in main beside S's mouth floods the stream's mouth.)
      // Ticket 2026-10-17_00-02-59: a grain sinking into held water swapped
      // that water UP onto the pile, over the target, where nothing takes it
      // back, while the reservoir refilled the river -- a sheet of water
      // climbed every pile poured into a river. A 5 x 5 column of gravel 14
      // cells tall dropped half on main's bank, half in it, at a quarter of
      // its length; after the pile settles, count the water standing ON
      // gravel above main's own surface there.
      {
        const auto& Lm = rMain->line;
        const auto& pg = Lm[Lm.size() / 4];
        const auto& pg0 = Lm[Lm.size() / 4 - 2];
        const auto& pg1 = Lm[Lm.size() / 4 + 2];
        double tx = pg1[0] - pg0[0], tz = pg1[1] - pg0[1];
        const double tl = std::max(std::hypot(tx, tz), 1e-6);
        const double nx = -tz / tl, nz = tx / tl;
        const int gx = pg[0] + static_cast<int>(std::lround(nx * pg[3])), gz = pg[1] + static_cast<int>(std::lround(nz * pg[3]));
        const int gTop = pg[2];
        const uint32_t mGravel = matIdOf("gravel");
        std::vector<CellOp> pour;
        for (int dz = -2; dz <= 2; dz++)
          for (int dx = -2; dx <= 2; dx++)
            for (int y = gTop + 2; y <= gTop + 15; y++)
              pour.push_back(CellOp{World::SlotCellIndex(IVec3{gx + dx, y, gz + dz}), mGravel | 0x80000000u});
        sandvox::support::TickOps gops;
        gops.cells = pour;
        tick(gops);
        const int gTicks = static_cast<int>(BaselineNumber("rivers.gravelTicks", 400));
        for (int k = 1; k < gTicks; k++) tick();
        c.ctx.WaitIdle();
        rows.clear();
        int onPile = 0, pileCells = 0;
        for (int dz = -10; dz <= 10; dz++)
          for (int dx = -10; dx <= 10; dx++)
            for (int y = gTop + 1; y <= gTop + 16; y++) {
              const uint32_t m = vox(gx + dx, y, gz + dz);
              if (m == mGravel) pileCells++;
              if (m == mWater && vox(gx + dx, y - 1, gz + dz) == mGravel) onPile++;
            }
        const int onPileMax = static_cast<int>(BaselineNumber("rivers.gravelWaterMax", 4));
        const bool wOk = pileCells > 20 && onPile <= onPileMax;
        std::printf("rivers: W gravel into the river: %s (%d gravel cells over main's surface after %d ticks; %d water "
                    "cells standing on gravel over it (<= %d))\n",
                    wOk ? "PASS" : "FAIL", pileCells, gTicks, onPile, onPileMax);
        if (!wOk) problems.push_back(Format("W: %d water cells on the gravel pile (%d gravel over the surface)", onPile, pileCells));
      }
      weather::SetOverride(prevPin);
      if (!gOk) problems.push_back(Format("G: %u chunks awake after %d ticks (%d at the water)", active, ticks, nearWater));
    }
  }

  // ---- H. the site index on `default` with a 10 km river and a lake ----
  restoreOverride();
  {
    std::ifstream f(dir + "/worldmap/default/map.json");
    nlohmann::json dj;
    try { f >> dj; } catch (...) {}
    worldmap::WorldMapData d0, d1;
    std::string l0, l1;
    const std::string t0 = dj.dump();
    const auto ta = std::chrono::steady_clock::now();
    const bool ok0 = worldmap::LoadWorldMap(dir, "default", set, c.mats.size(), kDefaultSeed, d0, l0, &t0);
    const double ms0 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ta).count();
    // A 10 km river wandering east past Harrowby, 6 m wide, and a 600 m lake.
    nlohmann::json pts = nlohmann::json::array();
    for (int k = 0; k <= 20; k++) {
      const int x = -5000 + k * 500;
      const int z = 3000 + static_cast<int>(std::lround(900 * std::sin(k * 0.7)));
      if (k == 0 || k == 20) pts.push_back({x, z, 60, 12}); else pts.push_back({x, z});
    }
    if (!dj.contains("rivers")) dj["rivers"] = nlohmann::json::array();
    dj["rivers"].push_back({{"id", "test_long_river"}, {"smooth", true}, {"points", pts}});
    nlohmann::json lp = nlohmann::json::array();
    for (int k = 0; k < 10; k++) {
      const double a = k * 6.2831853 / 10;
      lp.push_back({2600 + int(3000 * std::cos(a) / 10), 800 + int(3000 * std::sin(a) / 10)});
    }
    if (!dj.contains("lakes")) dj["lakes"] = nlohmann::json::array();
    dj["lakes"].push_back({{"id", "test_lake"}, {"level", 150}, {"smooth", true}, {"points", lp}});
    const std::string t1 = dj.dump();
    const auto tb = std::chrono::steady_clock::now();
    const bool ok1 = worldmap::LoadWorldMap(dir, "default", set, c.mats.size(), kDefaultSeed, d1, l1, &t1);
    const double ms1 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tb).count();
    int pieces = 0, reaches = 0;
    for (const auto& r : d1.rivers) { pieces += r.pieces; reaches += r.reaches; }
    const bool hOk = ok0 && ok1;
    std::printf("rivers: H site index on default: %s (shipped: worst cell %d, load %.0f ms; with a 10 km river (%d reaches, "
                "%d pieces) and a lake (%d cells): worst cell %d, load %.0f ms; the cap is %d)%s%s\n",
                hOk ? "PASS" : "FAIL", d0.siteCellWorst, ms0, reaches, pieces, d1.lakes.empty() ? 0 : d1.lakes[0].cells,
                d1.siteCellWorst, ms1, static_cast<int>(worldmap::kSiteCellMax),
                ok0 ? "" : (" shipped refused: " + l0.substr(0, 200)).c_str(), ok1 ? "" : (" with water refused: " + l1.substr(0, 300)).c_str());
    RecordObserved("rivers.defaultWorstCell", std::to_string(d1.siteCellWorst));
    if (!hOk) problems.push_back("H: default " + std::string(ok0 ? "" : "REFUSED: " + l0.substr(0, 200)) +
                                 (ok1 ? "" : " with a river REFUSED: " + l1.substr(0, 300)));

    // ---- L. default's waterfalls contained (CPU twin; 2026-10-16) ----
    // E holds the fixture's ONE fall; the shipped map has hundreds, in every
    // shape the bake makes, and two classes of leak got past E there: a dry
    // channel edge beside a curtain's END stood at the lower reach's level
    // (every curtain of Hollowcrown Beck spilled down its cliff for ever, a
    // breach that never slept), and two rivers' overlapping channels stood
    // one's water on the other's floor (66-voxel slabs). A LEAK here = a
    // water column standing over a DRY 4-neighbour whose ground is under that
    // water's top: air beside held water with no held column under it. Every
    // fall of every river, a box around it, waterApply's own answer per
    // column (no GPU).
    if (ok0) {
      worldmap::SetCurrentWorldMap(d0);
      const int halfBox = static_cast<int>(BaselineNumber("rivers.fallBoxHalf", 20));
      const int fallStride = std::max(1, static_cast<int>(BaselineNumber("rivers.fallStride", 1)));
      int fallsSeen = 0, leaks = 0, leakFalls = 0;
      std::string firstLeaks;
      const auto tl = std::chrono::steady_clock::now();
      for (const auto& rv : d0.rivers) {
        for (size_t fi = 0; fi < rv.falls.size(); fi += static_cast<size_t>(fallStride)) {
          const int fx = rv.falls[fi][0], fz = rv.falls[fi][1];
          const int n = 2 * halfBox + 1;
          std::vector<worldmap::WaterColumn> box(static_cast<size_t>(n * n));
          for (int dz = 0; dz < n; dz++)
            for (int dx = 0; dx < n; dx++)
              box[static_cast<size_t>(dz * n + dx)] = worldmap::WaterColumnAt(fx - halfBox + dx, fz - halfBox + dz, kDefaultSeed);
          int here = 0;
          for (int dz = 1; dz + 1 < n; dz++) {
            for (int dx = 1; dx + 1 < n; dx++) {
              const worldmap::WaterColumn& a = box[static_cast<size_t>(dz * n + dx)];
              if (a.water < 0 || a.water <= a.h) continue;
              static constexpr int kN[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
              for (const auto& d : kN) {
                const worldmap::WaterColumn& b = box[static_cast<size_t>((dz + d[1]) * n + dx + d[0])];
                // The sea holds a fall into it (water -1 = none above the sea's own).
                if (b.water >= 0 || b.h >= a.water || b.h < d0.seaLevelY) continue;
                here++;
                if (firstLeaks.size() < 400)
                  firstLeaks += Format(" [%s (%d,%d) water %d over (%d,%d) ground %d]", rv.id.c_str(), fx - halfBox + dx,
                                       fz - halfBox + dz, a.water, fx - halfBox + dx + d[0], fz - halfBox + dz + d[1], b.h);
              }
            }
          }
          fallsSeen++;
          leaks += here;
          if (here) leakFalls++;
        }
      }
      const double msL = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tl).count();
      const int leakMax = static_cast<int>(BaselineNumber("rivers.fallLeakMax", 0));
      const bool lOk = fallsSeen > 0 && leaks <= leakMax;
      std::printf("rivers: L default falls contained: %s (%d falls, a %d-column box round each: %d leaking faces at %d "
                  "falls (<= %d), %.0f ms)%s\n",
                  lOk ? "PASS" : "FAIL", fallsSeen, 2 * halfBox + 1, leaks, leakFalls, leakMax, msL, firstLeaks.c_str());
      RecordObserved("rivers.observedFallLeaks", std::to_string(leaks));
      if (!lOk) problems.push_back(Format("L: %d leaking faces at %d of %d default falls", leaks, leakFalls, fallsSeen));
    }
  }

  // ---- the pristine world back ----
  if (gpuState) {
    biomes::EnvironmentStamp stamp;
    if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, log)) {
      worldmap::SetCurrentWorldMap(real);
      problems.push_back("ReloadEnvironment REFUSED afterwards: " + log);
    }
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    DrainFullRefill(c.ctx, c.world, c.sim, IVec3{108 >> 4, 122 >> 4, 108 >> 4});
  } else {
    worldmap::SetCurrentWorldMap(real);
  }
  cleanup();
  if (!problems.empty()) {
    detail = problems[0];
    for (size_t i = 1; i < problems.size(); i++) detail += "; " + problems[i];
    std::printf("rivers: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  detail = "spline, load, bake, lake, CPU == GPU, contained + held, far + horizon, sleep, running, stream held, solute carried, site index";
  std::printf("rivers: PASS\n");
  return Status::Pass;
}

// ============================================================================
// `stream-natural`: a stream is held only when holding it looks natural
// (sim_stream.wgsl STREAMS ADOPT ONLY WHEN NATURAL; tickets
// 2026-10-17_11-26-13 and 11-37-20; docs/PLAN_running_water.md phase 2b).
//
// THE CLAIM. A breach in map water feeds its stream for ever, so phase 2
// held such a stream once its surface had stayed within a voxel for 120
// visits -- which a FLOOD spreading over flat ground, or a basin filling
// below a stream, also does: their columns were held part way up, at
// whatever height they stood at that moment, and refilled to it (the
// owner's "vertical waterfall column things", the rise and the collapse).
// Now a column is held only when its stream is fed from the map's water, its
// own MEAN surface has held within an eighth or two for sim.streamSteadyTicks
// with its water resting on its bed, and nothing DOWNSTREAM of it has changed
// for as long (no new column wet, no surface moved).
//
// THE FIXTURE: a synthetic map (the `rivers` seam: a TEMP asset dir, its own
// height plane with the terrain noise off) -- flat ground with two map lakes,
// A high and B low -- and stone-lined cuts written by cell ops:
//   FLOOD: a 3-wide notch through A's north shore (floor 2 under A's level)
//     drops 3 voxels onto a walled flat PLAIN (floodX x floodZ) 5 under A's
//     level, which fills from A until level with it;
//   CHANNEL: a 3-wide channel out of A's east shore falling 1 in chanRun into
//     a BASIN basinDepth under its last floor, which overflows over a sill
//     sillH above its floor into an OUTLET that runs down into lake B: once
//     the basin is full the whole thing is a through-flow from A to B;
//   EXTENSION: a dry basin beside the first, behind a wall (phase 2).
// Then, with MPM excitation and water-body discovery off and a clear dawn
// pinned (their own systems; the claim is the held stream's):
//   A. no column of the FLOOD (notch + plain) is held while the plain is still
//      filling (its water moved by more than fillTolPerCol eighths a column over
//      the last steady window), and the plain pools and its chunks sleep (<= floodAwakeMax);
//   B. no PILLAR four samples running (200 ticks): a held column whose target
//      stands more than a voxel over every wet neighbour (or over open dry
//      ones);
//   C. no CHANNEL column is held while the basin is still filling, and once it
//      is a through-flow the channel is held (>= adoptMin) and its chunks
//      sleep (<= chanAwakeMax);
//   D. no cycling: at most releaseMax releases per column over phase 1
//      (sim_stream's own per-column counter, STREAM_OBS word 6), and the held
//      channel never loses columns between samples while nothing changes;
//   E. DELIVERY: the extension is opened under the held stream, and in a
//      CONTROL world under the same stream never held (sim.streamSteadyTicks
//      at its ceiling, the same ticks, the door opened at the same tick); the
//      water the basin region gains per tick over rateTicks (from rateDelay
//      after the opening) held is within [rateLo, rateHi] of live;
//   F. DETERMINISM: detRuns runs of the first detTicks of phase 1 from a fresh
//      worldgen hash the fixture's voxels and still-table words equal.
// Every sample prints the volumes, held counts, awake chunks and the largest
// release count, so a failure names its part (CLAUDE.md verification rule 6).
// ============================================================================
Status GateStreamNatural(Ctx& c, std::string& detail) {
  using sandvox::ReadVoxelsSync;
  using sandvox::ReloadEnvironment;
  using sandvox::SubmitWorldgen;
  const std::string dir = sandvox::AssetDir();
  std::vector<std::string> problems;
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("stream-natural: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  };
  biomes::BiomeSet set;
  std::string log;
  if (!biomes::LoadBiomeSet(dir, c.mats, set, log)) return fail("biome files did not load: " + log);
  TreeAtlas atlas;
  if (!LoadTreeAtlas(dir + "/trees", c.mats, set, atlas, log)) return fail("tree atlas did not load: " + log);
  const worldmap::WorldMapData real = worldmap::CurrentWorldMap();
  if (!real.Loaded()) return fail("no world map loaded");
  uint32_t mWater = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { mWater = static_cast<uint32_t>(i); break; }
  if (mWater == 0) return fail("no `water` material");
  auto num = [](const char* k, double d) { return static_cast<int>(BaselineNumber(k, d)); };
  const int floodX = num("streamNat.floodX", 12), floodZ = num("streamNat.floodZ", 12);
  const int notchLen = num("streamNat.notchLen", 6);
  const int chanLen = num("streamNat.chanLen", 32), chanRun = std::max(1, num("streamNat.chanRun", 4));
  const int basinX = num("streamNat.basinX", 12), basinZ = num("streamNat.basinZ", 12);
  const int basinDepth = num("streamNat.basinDepth", 6), sillH = num("streamNat.sillH", 4);
  const int extZ = num("streamNat.extZ", 12);
  const int sampleTicks = std::max(10, num("streamNat.sampleTicks", 50));
  const int minTicks = num("streamNat.minTicks", 2400), maxTicks = num("streamNat.maxTicks", 9000);
  const int extTicks = num("streamNat.extTicks", 900);
  const int rateDelay = num("streamNat.rateDelay", 30), rateTicks = num("streamNat.rateTicks", 150);
  // "Still filling": the region's water moved by more than this many eighths
  // per column over the steady window: half a voxel, so a level wandering
  // inside a few eighths is not "filling" and a flood rising half a voxel is.
  const long long fillTolPerCol = num("streamNat.fillTolPerCol", 4);
  const int floodAwakeMax = num("streamNat.floodAwakeMax", 0), chanAwakeMax = num("streamNat.chanAwakeMax", 1);
  const double adoptMin = BaselineNumber("streamNat.adoptMin", 0.6);
  const int releaseMax = num("streamNat.releaseMax", 1);
  const double rateLo = BaselineNumber("streamNat.rateLo", 0.5), rateHi = BaselineNumber("streamNat.rateHi", 2.0);
  const int detRuns = num("streamNat.detRuns", 3), detTicks = num("streamNat.detTicks", 600);
  const int steadyTicks = std::clamp(CurrentTuning().sim.streamSteadyTicks, 32, 65535);
  const int steadySamples = std::max(1, steadyTicks / sampleTicks);

  // ---- the synthetic map: flat ground, two lakes ----
  const fs::path tmp = fs::temp_directory_path() /
                       ("sv_snat_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path mdir = tmp / "worldmap" / "snat";
  const IVec3 org = c.world.WindowOrigin();
  const int ox = org.x * static_cast<int>(kChunk), oz = org.z * static_cast<int>(kChunk);
  const int ylo = org.y * static_cast<int>(kChunk);
  constexpr int L = 6, P = 1 << L, T = worldmap::kTileSide;
  const int X0 = ((ox - 8192) >> 10) << 10, Z0 = ((oz - 8192) >> 10) << 10;
  const int G = ylo + 100;
  // Lake centres ON height posts, so the depression's floor is the post's.
  auto onPost = [&](int v, int o) { return o + ((v - o + P / 2) / P) * P; };
  const int xA = onPost(ox + 150, X0), zA = onPost(oz + 300, Z0);
  // B small, deep and apart from A: the two depressions must not overlap (the
  // third cut overlapped them, B's lowered A's rim under A's floor and A
  // held no water at all).
  const int xB = onPost(ox + 340, X0), zB = zA;
  const double R0 = 120.0, R0B = 100.0;
  auto authored = [&](int i, int k) -> int {
    const double px = X0 + i * P, pz = Z0 + k * P;
    double h = G;
    const double dA = std::hypot(px - xA, pz - zA) / R0, dB = std::hypot(px - xB, pz - zB) / R0B;
    if (dA < 1) h -= 40 * (1 - dA * dA) * (1 - dA * dA);
    if (dB < 1) h -= 100 * (1 - dB * dB) * (1 - dB * dB);
    return static_cast<int>(std::lround(h));
  };
  std::vector<int> hs(size_t(T) * T), bs(size_t(T) * T, 1);
  for (int k = 0; k < T; k++)
    for (int i = 0; i < T; i++) hs[size_t(k) * T + i] = authored(i, k);
  nlohmann::json te = nlohmann::json::object();
  {
    std::ifstream f(dir + "/worldmap/" + real.name + "/map.json");
    nlohmann::json rj;
    try { f >> rj; } catch (...) {}
    if (rj.contains("terrain")) te = rj["terrain"];
  }
  // FLAT: the octave ladder off, so the ground is the plane.
  for (const char* k : {"rangeAmplitude", "hillAmplitude", "detailAmplitude", "grainAmplitude"}) te[k] = 0;
  nlohmann::json mj = {
      {"version", 2}, {"name", "snat"},
      {"extent", {{"min", {X0, Z0}}, {"size", {32768, 32768}}}},
      {"siteCellLog2", 10},
      {"planes", {{"height", {{"log2", L}, {"tile", T}, {"default", -300}}},
                  {"biome", {{"log2", 8}, {"tile", T}, {"default", "ocean"}}}}},
      // The sea well under every cut (B's pit reaches y0, and a sea over it
      // flooded B and the outlet: the first stream-natural runs), but not km
      // below (the `rivers` note: the lapse rate counts from the sea).
      {"seaLevelY", ylo - 100}, {"edgeFadeVox", 2048}, {"warpAmpVox", 0},
      {"biomes", {"ocean", "meadow"}},
      {"terrain", te},
      {"sites", {{{"id", "pad"}, {"kind", "pad"}, {"min", {ox - 256, oz - 256}}, {"max", {ox + 768, oz + 768}}},
                 {{"id", "spawn"}, {"kind", "spawn"}, {"at", {ox + 480, oz + 60}}}}},
      {"rules", nlohmann::json::array()}};
  bool wrote = WriteTileR(mdir / "planes" / "height" / "0_0.svt", worldmap::kPlaneHeight, 0, 0, hs) &&
               WriteTileR(mdir / "planes" / "biome" / "0_0.svt", worldmap::kPlaneBiome, 0, 0, bs);
  {
    std::ofstream f(mdir / "map.json");
    f << mj.dump(2);
    wrote = wrote && static_cast<bool>(f);
  }
  if (!wrote) return fail("could not write the synthetic map under " + tmp.string());
  const std::vector<worldmap::StructurePlacement>* prevOverride = worldmap::StructureOverrideSlot();
  const std::vector<worldmap::StructurePlacement> prevList = prevOverride ? *prevOverride : std::vector<worldmap::StructurePlacement>{};
  static const std::vector<worldmap::StructurePlacement> kNone;
  worldmap::SetStructureOverride(&kNone);
  const std::string tdir = tmp.string();
  auto load = [&](const nlohmann::json* j, worldmap::WorldMapData& out, std::string& l) {
    const std::string text = j ? j->dump() : std::string();
    return worldmap::LoadWorldMap(tdir, "snat", set, c.mats.size(), kDefaultSeed, out, l, j ? &text : nullptr);
  };
  auto cleanup = [&] {
    worldmap::SetStructureOverride(prevOverride ? &prevList : nullptr);
    std::error_code ec;
    fs::remove_all(tmp, ec);
  };
  // Each lake's level: deep enough over its floor, and two under the lowest
  // ground on its polygon (so no lake stands over the ground outside it and
  // spills for ever -- the first cut put A's level 4 over the plain).
  const int rA = 96, rB = 40;
  int LA = 0, LB = 0, floorA = 0, floorB = 0, rimA = INT_MAX, rimB = INT_MAX;
  {
    worldmap::WorldMapData dry;
    std::string l;
    if (!load(nullptr, dry, l)) { cleanup(); return fail("the dry synthetic map was REFUSED: " + l); }
    worldmap::SetCurrentWorldMap(dry);
    floorA = worldmap::BareGroundHeight(xA, zA, kDefaultSeed);
    floorB = worldmap::BareGroundHeight(xB, zB, kDefaultSeed);
    for (int k = 0; k < 64; k++) {
      const double a = k * 3.14159265358979 / 32;
      rimA = std::min(rimA, worldmap::BareGroundHeight(xA + int(std::lround(rA * std::cos(a))), zA + int(std::lround(rA * std::sin(a))), kDefaultSeed));
      rimB = std::min(rimB, worldmap::BareGroundHeight(xB + int(std::lround(rB * std::cos(a))), zB + int(std::lround(rB * std::sin(a))), kDefaultSeed));
    }
    LA = std::min(floorA + 14, rimA - 2);
    // B sits under A's channel, basin and sill: 16 under A (the outlet's sill
    // is 11 under A's level).
    LB = std::min({floorB + 14, rimB - 2, LA - 30});
  }
  auto ring = [&](int cx, int cz, double r) {
    nlohmann::json pts = nlohmann::json::array();
    for (int k = 0; k < 16; k++) {
      const double a = k * 3.14159265358979 / 8;
      pts.push_back({cx + int(std::lround(r * std::cos(a))), cz + int(std::lround(r * std::sin(a)))});
    }
    return pts;
  };
  nlohmann::json lakes = nlohmann::json::array();
  lakes.push_back({{"id", "high"}, {"level", LA}, {"smooth", true}, {"bed", "shore_mud"}, {"shore", "sand"}, {"points", ring(xA, zA, rA)}});
  lakes.push_back({{"id", "low"}, {"level", LB}, {"smooth", true}, {"bed", "shore_mud"}, {"shore", "sand"}, {"points", ring(xB, zB, rB)}});
  mj["lakes"] = lakes;
  worldmap::WorldMapData on;
  {
    std::string l;
    if (!load(&mj, on, l)) { cleanup(); worldmap::SetCurrentWorldMap(real); return fail("the lake map was REFUSED: " + l); }
  }
  worldmap::SetCurrentWorldMap(on);
  auto wetAt = [&](int x, int z) { return worldmap::WaterColumnAt(x, z, kDefaultSeed).water >= 0; };
  // A's north and east shores and B's west shore, along the lakes' axes.
  int zN = zA, xE = xA, xW = xB;
  while (zN > zA - 140 && wetAt(xA, zN)) zN--;
  while (xE < xA + 140 && wetAt(xE, zA)) xE++;
  while (xW > xB - 140 && wetAt(xW, zB)) xW--;

  // ---- the fixture: kinds per column ----
  enum Kind { K_NONE = 0, K_WALL, K_NOTCH, K_PLAIN, K_CHAN, K_BASIN, K_OUTLET, K_EXT, K_DOOR, K_COUNT };
  static const char* kKindName[K_COUNT] = {"none", "wall", "notch", "plain", "channel", "basin", "outlet", "ext", "door"};
  std::map<std::pair<int, int>, std::pair<int, int>> interior;   // (x,z) -> (kind, floor y = stone top)
  std::set<std::pair<int, int>> gateCols, doorCols;              // built as wall, opened later
  // FLOOD: notch rows zN, zN-1, ... (zN is the first dry row north of A).
  for (int k = 0; k < notchLen; k++)
    for (int dx = -1; dx <= 1; dx++) {
      interior[{xA + dx, zN - k}] = {K_NOTCH, LA - 2};
      if (k == 0) gateCols.insert({xA + dx, zN});
    }
  const int zp1 = zN - notchLen, zp0 = zp1 - floodZ + 1;
  const int xp0 = xA - floodX / 2, xp1 = xp0 + floodX - 1;
  for (int z = zp0; z <= zp1; z++)
    for (int x = xp0; x <= xp1; x++) interior[{x, z}] = {K_PLAIN, LA - 5};
  // CHANNEL out of A's east shore.
  auto chanFloor = [&](int k) { return LA - 2 - k / chanRun; };
  for (int k = 0; k < chanLen; k++)
    for (int dz = -1; dz <= 1; dz++) {
      interior[{xE + k, zA + dz}] = {K_CHAN, chanFloor(k)};
      if (k == 0) gateCols.insert({xE, zA + dz});
    }
  const int xb0 = xE + chanLen, xb1 = xb0 + basinX - 1;
  const int zb0 = zA - basinZ / 2, zb1 = zb0 + basinZ - 1;
  const int yBasin = chanFloor(chanLen - 1) - basinDepth;
  for (int z = zb0; z <= zb1; z++)
    for (int x = xb0; x <= xb1; x++) interior[{x, z}] = {K_BASIN, yBasin};
  // OUTLET: from the sill over the basin floor, a straight ramp down to two
  // OVER B's level at B's shore, where it falls into B. Never a flat stretch
  // standing in B's water: a reach-1 CA moves water along a level surface only
  // by levelling (a ramp of an eighth per two cells is its fixed point), so a
  // long flat or flooded outlet is a slow pipe the whole through-flow backs up
  // behind (measured: the first two fixtures' outlets, 79 and 52 columns
  // ending in B's standing water, were still backing up -- the basin rising
  // three voxels -- at tick 9000).
  const int ySill = yBasin + sillH;
  const int outLen = std::max(1, xW - xb1);
  int outCols = 0;
  for (int x = xb1 + 1; x <= xW; x++) {
    const int k = x - xb1 - 1;
    const int fy = ySill - (k * (ySill - (LB + 2)) + (outLen - 1) / 2) / std::max(1, outLen - 1);
    for (int dz = -1; dz <= 1; dz++) interior[{x, zA + dz}] = {K_OUTLET, fy};
    outCols++;
  }
  // EXTENSION south of the basin, behind its wall; the DOOR is that wall.
  const int ze0 = zb1 + 3, ze1 = ze0 + extZ - 1;
  for (int z = ze0; z <= ze1; z++)
    for (int x = xb0; x <= xb1; x++) interior[{x, z}] = {K_EXT, yBasin};
  for (int z = zb1 + 1; z <= zb1 + 2; z++)
    for (int x = xb0 + 3; x <= xb1 - 3; x++) {
      interior[{x, z}] = {K_DOOR, yBasin};
      doorCols.insert({x, z});
    }
  // Interior columns must be dry ground (map water is the lakes'): a cut
  // through a lake column would hold the lake's own target. A wet one (a
  // shoreline that bulges into a cut's first row) is the lake's, not cut.
  int wetInterior = 0;
  for (auto it = interior.begin(); it != interior.end();) {
    if (wetAt(it->first.first, it->first.second)) {
      wetInterior++;
      gateCols.erase(it->first);
      it = interior.erase(it);
    } else {
      ++it;
    }
  }
  const int wallTop = std::max(G, LA + 4) + 2;
  const int yBase = std::min({LB - 4, yBasin - 3});
  const int yTop = G + 40;
  std::map<std::pair<int, int>, int> kindOf;   // every footprint column, walls included
  for (const auto& [xz, kf] : interior) kindOf[xz] = kf.first;
  for (const auto& [xz, kf] : interior)
    for (int dz = -2; dz <= 2; dz++)
      for (int dx = -2; dx <= 2; dx++) {
        const std::pair<int, int> q{xz.first + dx, xz.second + dz};
        if (!kindOf.count(q) && !wetAt(q.first, q.second)) kindOf[q] = K_WALL;
      }
  int bx0 = INT_MAX, bx1 = INT_MIN, bz0 = INT_MAX, bz1 = INT_MIN;
  for (const auto& [xz, k] : kindOf) {
    bx0 = std::min(bx0, xz.first); bx1 = std::max(bx1, xz.first);
    bz0 = std::min(bz0, xz.second); bz1 = std::max(bz1, xz.second);
  }
  bx0 -= 2; bx1 += 2; bz0 -= 2; bz1 += 2;
  const int by0 = yBase, by1 = wallTop + 8;
  const int wx0 = org.x * static_cast<int>(kChunk), wy0 = ylo, wz0 = org.z * static_cast<int>(kChunk), W = static_cast<int>(kWorldN);
  std::printf("stream-natural: map: ground y%d, lake A (%d,%d) floor y%d rim y%d level y%d (shores: north z%d, east "
              "x%d), lake B (%d,%d) floor y%d rim y%d level y%d (west shore x%d); plain %dx%d floor y%d; channel %d long floor y%d -> y%d (1 in %d); basin "
              "%dx%d floor y%d, sill y%d; outlet %d long; ext %dx%d; %zu footprint columns (%d left to the lakes), box "
              "x%d..%d y%d..%d z%d..%d\n",
              G, xA, zA, floorA, rimA, LA, zN, xE, xB, zB, floorB, rimB, LB, xW, floodX, floodZ, LA - 5, chanLen, chanFloor(0), chanFloor(chanLen - 1),
              chanRun, basinX, basinZ, yBasin, ySill, outCols, basinX, extZ, kindOf.size(), wetInterior, bx0, bx1, by0, by1, bz0, bz1);
  if (LA - floorA < 4 || LB - floorB < 4 || LA - LB < 14 || ySill - (LB + 2) < outLen / 4 || zN <= zA - 140 || xE >= xA + 140 || xW <= xB - 140 || outCols < 8 || LB >= ySill ||
      bx0 < wx0 + 8 || bx1 >= wx0 + W - 8 || bz0 < wz0 + 8 || bz1 >= wz0 + W - 8 || by0 < wy0 || yTop >= wy0 + W) {
    cleanup();
    worldmap::SetCurrentWorldMap(real);
    return fail(Format("the fixture does not fit: lake depths %d / %d, A over B %d, shores z%d x%d x%d, outlet %d "
                       "columns, B level y%d vs sill y%d, box vs window (%d,%d,%d)",
                       LA - floorA, LB - floorB, LA - LB, zN, xE, xW, outCols, LB, ySill, wx0, wy0, wz0));
  }

  // ---- build ops (top down), opening ops ----
  const uint32_t stoneW = kMatStone;
  std::vector<CellOp> build, openGate, openDoor;
  for (int y = yTop; y >= yBase; y--)
    for (const auto& [xz, kd] : kindOf) {
      int top = wallTop;
      if (kd != K_WALL) {
        const int fy = interior[xz].second;
        top = (gateCols.count(xz) || doorCols.count(xz)) ? wallTop : fy;
      }
      build.push_back({World::SlotCellIndex({xz.first, y, xz.second}), y <= top ? stoneW : 0u});
    }
  for (const auto& xz : gateCols)
    for (int y = interior[xz].second + 1; y <= wallTop; y++) openGate.push_back({World::SlotCellIndex({xz.first, y, xz.second}), 0u});
  for (const auto& xz : doorCols)
    for (int y = interior[xz].second + 1; y <= wallTop; y++) openDoor.push_back({World::SlotCellIndex({xz.first, y, xz.second}), 0u});

  // ---- chunk sets per kind (the chunks holding each interior's water band) ----
  std::set<uint32_t> chunksOf[K_COUNT];
  for (const auto& [xz, kf] : interior)
    for (int y = kf.second; y <= std::min(kf.second + 8, LA + 1); y++)
      chunksOf[kf.first].insert(World::SlotChunkIndex({xz.first >> 4, y >> 4, xz.second >> 4}));

  // ---- the readers ----
  const size_t kSN = size_t(kWorldN) * kWorldN;
  const size_t kEcho = kSN * 4 + 4 + kSN, kObs = kEcho + 2 * kSN;
  const int mask = static_cast<int>(kWorldN) - 1;
  const rhi::Buffer& still = c.sim.StillWaterBuffer();
  const int nbx = bx1 - bx0 + 1, nbz = bz1 - bz0 + 1, nby = by1 - by0 + 1;
  // Rows of a per-column region over the box's z span.
  auto readRows = [&](size_t base, size_t per, std::vector<uint32_t>& out) {
    out.assign(size_t(nbz) * kWorldN * per, 0u);
    int z = bz0;
    while (z <= bz1) {
      int z2 = z;
      while (z2 + 1 <= bz1 && ((z2 + 1) & mask) == ((z2 & mask) + 1)) z2++;
      const size_t first = (size_t(z & mask) * kWorldN) * per;
      rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, still, (base + first) * 4u, out.data() + size_t(z - bz0) * kWorldN * per,
                            size_t(z2 - z + 1) * kWorldN * per * 4u, "streamNatStill");
      z = z2 + 1;
    }
  };
  struct Snap {
    std::vector<uint32_t> vox, rec, obs, echo;
    uint32_t At(int x, int y, int z) const {
      return vox[(size_t(z - bz0s) * nbys + size_t(y - by0s)) * nbxs + size_t(x - bx0s)];
    }
    int bx0s = 0, by0s = 0, bz0s = 0, nbxs = 0, nbys = 0;
  };
  auto snapshot = [&](Snap& s) {
    c.ctx.WaitIdle();
    s.bx0s = bx0; s.by0s = by0; s.bz0s = bz0; s.nbxs = nbx; s.nbys = nby;
    s.vox.assign(size_t(nbx) * nby * nbz, 0u);
    std::vector<uint32_t> run;
    for (int cz = bz0 >> 4; cz <= (bz1 >> 4); cz++)
      for (int cy = by0 >> 4; cy <= (by1 >> 4); cy++) {
        int cx = bx0 >> 4;
        while (cx <= (bx1 >> 4)) {
          int cx2 = cx;
          while (cx2 + 1 <= (bx1 >> 4) && (((cx2 + 1) & (int(kNChunk) - 1)) == ((cx2 & (int(kNChunk) - 1)) + 1))) cx2++;
          const uint32_t n = static_cast<uint32_t>(cx2 - cx + 1);
          run.assign(size_t(n) * kChunkVol, 0u);
          ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({cx, cy, cz}), n, run.data(), "streamNat");
          for (uint32_t k = 0; k < n; k++)
            for (uint32_t li = 0; li < kChunkVol; li++) {
              const int x = (cx + int(k)) * 16 + int(li & 15), y = cy * 16 + int((li >> 4) & 15), z = cz * 16 + int(li >> 8);
              if (x < bx0 || x > bx1 || y < by0 || y > by1 || z < bz0 || z > bz1) continue;
              s.vox[(size_t(z - bz0) * nby + size_t(y - by0)) * nbx + size_t(x - bx0)] = run[size_t(k) * kChunkVol + li];
            }
          cx = cx2 + 1;
        }
      }
    readRows(0, 4, s.rec);
    readRows(kObs, 8, s.obs);
    readRows(kEcho, 2, s.echo);
  };
  auto klassOf = [&](uint32_t w) -> int {
    const uint32_t id = w & 0xFFFu;
    if (id == 0u || id >= c.mats.size()) return -1;   // air
    return static_cast<int>(c.mats[id].gpu.klass);
  };
  // The RESTING surface (sim_stream.wgsl restingSurface's twin): the top of
  // the first run of liquid, walking down, that stands on solid or powder;
  // runs with air or gas under them are in flight. INT_MIN = none.
  auto restS8 = [&](const Snap& s, int x, int z, bool* flight = nullptr) -> int {
    // sim_stream.wgsl restingSurface's twin, with its STEP gap (1 open cell).
    constexpr int kStepGap = 1;
    int runTop = INT_MIN, gap = -1;
    bool fl = false;
    bool inRun = false;
    int result = INT_MIN;
    for (int y = by1; y >= by0; y--) {
      const uint32_t w = s.At(x, y, z);
      const int k = klassOf(w);
      if (k == CLASS_LIQUID) {
        if (gap >= 0) {
          if (gap <= kStepGap) { result = runTop; break; }
          fl = true;
          gap = -1;
          inRun = false;
        }
        if (!inRun) { inRun = true; runTop = 8 * y + static_cast<int>(((w >> 12) & 15u) + 1u); }
        continue;
      }
      const bool open = k == -1 || k == CLASS_GAS;
      if (inRun) {
        if (open) { inRun = false; gap = 1; continue; }
        result = runTop;
        break;
      }
      if (open) { if (gap >= 0) gap++; continue; }
      if (gap >= 0 && gap <= kStepGap) result = runTop;
      else if (gap >= 0) fl = true;
      gap = -2;   // ended on a floor
      break;
    }
    if (result == INT_MIN && inRun) result = runTop;   // liquid to the bottom of the box
    if (gap >= 0) fl = true;
    if (flight) *flight = fl;
    return result;
  };
  auto recAt = [&](const Snap& s, int x, int z, int w) { return s.rec[(size_t(z - bz0) * kWorldN + size_t(x & mask)) * 4 + size_t(w)]; };
  auto obsAt = [&](const Snap& s, int x, int z, int w) { return s.obs[(size_t(z - bz0) * kWorldN + size_t(x & mask)) * 8 + size_t(w)]; };
  auto echoAt = [&](const Snap& s, int x, int z, int w) { return s.echo[(size_t(z - bz0) * kWorldN + size_t(x & mask)) * 2 + size_t(w)]; };

  struct Census {
    long long vol[K_COUNT] = {};
    int held[K_COUNT] = {}, pool = 0, awake[K_COUNT] = {};
    uint32_t awakeWhy[K_COUNT] = {};   // OR of the awake chunks' dirty reasons
    std::string awakeWhere;            // the awake channel chunks (slot -> world chunk)
    int pillars = 0, relMax = 0, relTotal = 0, adoptTotal = 0;
    // CLIFF: a held column over a voxel above ANY open neighbour's water (a
    // sheer held face); UNDER: a held column whose water stands over a voxel
    // under its target (being refilled: the RISE).
    int cliffs = 0, under = 0;
    // Footprint columns whose observer words belong to them (STREAM_OBS [4]
    // == the still tag, written by sim_stream's `commit`): 0 after a run means
    // the stream kernels never ran -- a shader that failed to compile prints
    // and the run goes on without them.
    int observed = 0;
    uint32_t relWhy = 0;   // OR of the reason bytes of every column that released
    std::string pillarWho, relWho;
    std::set<std::pair<int, int>> heldCols, pillarCols;
  };
  auto census = [&](Snap& s) -> Census {
    snapshot(s);
    Census r;
    for (const auto& [xz, kd] : kindOf) {
      const int x = xz.first, z = xz.second;
      for (int y = by0; y <= by1; y++) {
        const uint32_t w = s.At(x, y, z);
        if ((w & 0xFFFu) == mWater) r.vol[kd] += ((w >> 12) & 15u) + 1u;
      }
      if (recAt(s, x, z, 1) == 0u && recAt(s, x, z, 2) != 0u) {
        r.held[kd]++;
        r.heldCols.insert(xz);
        if (recAt(s, x, z, 3) & (1u << 24)) r.pool++;
        // PILLAR: the target more than a voxel over every wet neighbour, or
        // over open dry neighbours only.
        const int t8 = static_cast<int>(recAt(s, x, z, 2) << 12) >> 12;
        const int yt = (t8 - 1) >> 3;
        int best = INT_MIN;
        bool openDry = false;
        const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : d4) {
          const int nx = x + d[0], nz = z + d[1];
          const int k = klassOf(s.At(nx, yt, nz));
          if (k != -1 && k != CLASS_GAS && k != CLASS_LIQUID) continue;   // a wall at the target row
          const int ns = restS8(s, nx, nz);
          if (ns == INT_MIN) openDry = true;
          else best = std::max(best, ns);
        }
        int lowest = INT_MAX;
        for (const auto& d : d4) {
          const int k = klassOf(s.At(x + d[0], yt, z + d[1]));
          if (k != -1 && k != CLASS_GAS && k != CLASS_LIQUID) continue;
          const int ns = restS8(s, x + d[0], z + d[1]);
          lowest = std::min(lowest, ns == INT_MIN ? 8 * by0 : ns);
        }
        if (lowest != INT_MAX && t8 > lowest + 8) r.cliffs++;
        const int own = restS8(s, x, z);
        if (own == INT_MIN || own < t8 - 8) r.under++;
        if ((best != INT_MIN && t8 > best + 8) || (best == INT_MIN && openDry)) {
          r.pillarCols.insert(xz);
          if (!r.pillars++)
            r.pillarWho = Format(" first (%d,%d) %s t8 %d (y%d), highest wet neighbour %d", x, z, kKindName[kd], t8, yt, best);
        }
      }
      {
        const uint32_t tx = static_cast<uint32_t>(x >> std::countr_zero(kWorldN)) & 0x7FFFu;
        const uint32_t tz = static_cast<uint32_t>(z >> std::countr_zero(kWorldN)) & 0x7FFFu;
        if (obsAt(s, x, z, 4) == (1u | (tx << 1u) | (tz << 16u))) r.observed++;
      }
      const uint32_t o6 = obsAt(s, x, z, 6);
      const int rel = static_cast<int>(o6 & 0xFFu);
      r.relTotal += rel;
      r.adoptTotal += static_cast<int>((o6 >> 8) & 0xFFu);
      if (rel > 0) r.relWhy |= obsAt(s, x, z, 7);
      if (rel > r.relMax) {
        r.relMax = rel;
        r.relWho = Format("(%d,%d) %s: %d releases, %u adoptions, reasons 0x%08x", x, z, kKindName[kd], rel, (o6 >> 8) & 0xFFu, obsAt(s, x, z, 7));
      }
    }
    std::vector<uint32_t> fl(kNumSlots, 0);
    rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.DirtyActive(), 0, fl.data(), kNumSlots * 4, "streamNatAwake");
    for (int k = 0; k < K_COUNT; k++)
      for (uint32_t sl : chunksOf[k])
        if (sl < kNumSlots && fl[sl] != 0u) {
          r.awake[k]++;
          r.awakeWhy[k] |= fl[sl];
          if (k == K_CHAN) {
            const IVec3 w = c.world.SlotToWorldChunk(sl);
            r.awakeWhere += Format(" (%d,%d,%d)", w.x, w.y, w.z);
          }
        }
    return r;
  };

  // ---- tuning + weather pins (the stream-pool pins) ----
  const Tuning saved = CurrentTuning();
  const std::string prevPin = weather::Override();
  auto restoreTuning = [&] {
    SetCurrentTuning(saved);
    weather::SetOverride(prevPin);
  };
  {
    Tuning dawn = saved;
    dawn.dayNight.freeze = 1;
    dawn.dayNight.freezePhase = static_cast<int>(kDaySunrise + 1024u);
    dawn.sim.fluidExciteMode = 0;
    dawn.sim.waterDiscoverMinEighths = 0;
    SetCurrentTuning(dawn);
  }
  weather::SetOverride("clear");
  std::vector<uint32_t> words;
  if (!worldmap::PackWorldMap(set, on, words, log)) {
    restoreTuning();
    cleanup();
    worldmap::SetCurrentWorldMap(real);
    return fail("the synthetic map did not pack: " + log);
  }
  c.ctx.WaitIdle();
  c.sim.UploadEnvironment(c.ctx.device, c.ctx.queue, atlas, words);
  uint32_t t = 98000;
  const IVec3 fixture{xb0 >> 4, yBasin >> 4, zA >> 4};
  // Fresh world, the fixture built (gates and door shut), 60 ticks; returns
  // nothing -- the census after it is the caller's.
  auto setup = [&]() {
    c.stream.OnRegen();
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    sandvox::support::TickCursor tick{c, t, fixture};
    const size_t per = 60000;
    for (size_t i = 0; i < build.size(); i += per) {
      sandvox::support::TickOps ops;
      ops.cells.assign(build.begin() + i, build.begin() + std::min(build.size(), i + per));
      tick(ops);
    }
    for (int k = 0; k < 60; k++) tick();
  };
  auto line = [&](int tk, const Census& s) {
    return Format(" [%d: plain %lld notch %lld | chan %lld basin %lld outlet %lld ext %lld | held notch %d plain %d chan %d "
                  "basin %d outlet %d ext %d (POOL %d; %d cliffs, %d under) | awake flood %d+%d chan %d basin %d outlet %d | "
                  "pillars %d | releases %d (max %d) adoptions %d]",
                  tk, s.vol[K_PLAIN], s.vol[K_NOTCH], s.vol[K_CHAN], s.vol[K_BASIN], s.vol[K_OUTLET], s.vol[K_EXT] + s.vol[K_DOOR],
                  s.held[K_NOTCH], s.held[K_PLAIN], s.held[K_CHAN], s.held[K_BASIN], s.held[K_OUTLET], s.held[K_EXT],
                  s.pool, s.cliffs, s.under, s.awake[K_NOTCH], s.awake[K_PLAIN], s.awake[K_CHAN], s.awake[K_BASIN], s.awake[K_OUTLET],
                  s.pillars, s.relTotal, s.relMax, s.adoptTotal);
  };

  int chanCols = 0, plainCols = 0, basinCols = 0;
  for (const auto& [xz, kf] : interior) {
    if (kf.first == K_CHAN) chanCols++;
    if (kf.first == K_PLAIN) plainCols++;
    if (kf.first == K_BASIN) basinCols++;
  }
  const long long fillTolPlain = fillTolPerCol * plainCols, fillTolBasin = fillTolPerCol * basinCols;
  // ---- phase 1: open both gates, run until the flood has pooled and slept
  // and the channel is a held through-flow (or maxTicks) ----
  setup();
  Snap snap;
  std::string curve;
  struct Hist { int tick; long long plain, basin; int heldFlood, heldChan; };
  std::vector<Hist> hist;
  int floodEarly = 0, chanEarly = 0, pillarsMax = 0, lost = 0, done = 0;
  std::set<std::pair<int, int>> prevPillars;
  std::map<std::pair<int, int>, int> pillarAge;   // samples in a row each pillar has stood
  std::string floodEarlyWho, chanEarlyWho, pillarWho;
  long long liveRate10 = 0;   // the basin region's gain per 10 ticks while the LIVE stream fills it
  int liveFrom = -1, liveTo = -1;
  long long liveVol0 = 0, liveVol1 = 0;
  int poolAt = -1, chanHeldAt = -1;
  // WHO made each event (CLAUDE.md verification rule 6): every unheld
  // footprint column's own fine-event count (STREAM_OBS [6] >> 16), diffed
  // sample to sample -- the columns that just told everything above them
  // "something moved here".
  std::map<std::pair<int, int>, int> evPrev;
  Census s{};
  {
    sandvox::support::TickCursor tick{c, t, fixture};
    sandvox::support::TickOps o;
    o.cells = openGate;
    tick(o);
    done = 1;
    int prevHeldChan = 0;
    const long long basinCap = 8LL * basinX * basinZ * sillH;
    for (; done < maxTicks;) {
      for (int k = 0; k < sampleTicks; k++) tick();
      done += sampleTicks;
      s = census(snap);
      curve += "\n " + line(done, s);
      {
        std::string evWho;
        int evCols = 0;
        for (const auto& [xz, kd] : kindOf) {
          if (recAt(snap, xz.first, xz.second, 2) != 0u) continue;
          const int n = static_cast<int>((obsAt(snap, xz.first, xz.second, 6) >> 16) & 0xFFu);
          int& p = evPrev[xz];
          if (n > p) {
            if (evCols++ < 6) evWho += Format(" (%d,%d)%s+%d", xz.first, xz.second, kKindName[kd], n - p);
          }
          p = n;
        }
        if (evCols > 0) curve += Format(" events from %d columns:%s%s", evCols, evWho.c_str(), evCols > 6 ? " ..." : "");
      }
      // THE LANE (every 500 ticks): the channel's, basin's and outlet's centre
      // row, A's shore to B's, one entry a column: its resting surface in
      // eighths over B's level ('.' dry), '*' after it when held -- where a
      // through-flow backs up, and which part of it is held.
      if (done % 500 == 1) {
        std::string lane;
        for (int x = xE; x <= xW; x++) {
          const int r8 = restS8(snap, x, zA);
          const bool h = recAt(snap, x, zA, 1) == 0u && recAt(snap, x, zA, 2) != 0u;
          lane += r8 == INT_MIN ? std::string(" .") : Format(" %d", r8 - (8 * LB + 8));
          if (h) lane += "*";
          if (x == xb0 - 1 || x == xb1) lane += " |";
        }
        curve += "\n    lane (eighths over B's level, A -> basin | outlet -> B):" + lane;
        // ...and each lane column's observer (STREAM_OBS): the steady run in
        // windows, and how long ago the newest event at or below it was, in
        // 16-tick units ('-' never) -- which of the two holds adoption back.
        std::string obsLane;
        const uint32_t now16 = std::max((t >> 4) & 0xFFFFu, 1u);
        for (int x = xE; x <= xW; x++) {
          const uint32_t o0 = obsAt(snap, x, zA, 0), o2 = obsAt(snap, x, zA, 2);
          const uint32_t f = o2 & 0xFFFFu;
          obsLane += Format(" %u/%s/%u", o0 >> 20, f == 0u ? "-" : std::to_string((now16 - f) & 0xFFFFu).c_str(),
                            obsAt(snap, x, zA, 6) >> 24);
          if (x == xb0 - 1 || x == xb1) obsLane += " |";
        }
        curve += "\n    observer (run in windows / ticks/16 since the newest event at or below / why not held, "
                 "sim_stream.wgsl STREAM_OBS [6]):" + obsLane;
        // WHO makes the events (STREAM_OBS [6] >> 16, unheld columns: their
        // own fine events so far): the five busiest footprint columns.
        std::vector<std::pair<int, std::string>> who;
        for (const auto& [xz, kd] : kindOf) {
          if (recAt(snap, xz.first, xz.second, 2) != 0u) continue;
          const int n = static_cast<int>((obsAt(snap, xz.first, xz.second, 6) >> 16) & 0xFFu);
          if (n > 0) who.push_back({n, Format("(%d,%d) %s %d", xz.first, xz.second, kKindName[kd], n)});
        }
        std::sort(who.rbegin(), who.rend());
        curve += "\n    own events, busiest:";
        for (size_t i = 0; i < who.size() && i < 6; i++) curve += " " + who[i].second;
      }
      const int heldFlood = s.held[K_NOTCH] + s.held[K_PLAIN];
      const int heldChan = s.held[K_CHAN];
      hist.push_back({done, s.vol[K_PLAIN] + s.vol[K_NOTCH], s.vol[K_BASIN], heldFlood, heldChan});
      const size_t n = hist.size();
      const Hist& back = hist[n > size_t(steadySamples) ? n - 1 - size_t(steadySamples) : 0];
      // A: a held flood column while the plain is still filling.
      if (heldFlood > 0 && std::llabs(hist[n - 1].plain - back.plain) > fillTolPlain) {
        if (!floodEarly++)
          floodEarlyWho = Format(" first at tick %d: %d held while the plain moved %lld eighths in %d ticks", done, heldFlood,
                                 hist[n - 1].plain - back.plain, done - back.tick);
      }
      // C: a newly held channel column while the basin is still filling.
      if (heldChan > prevHeldChan && std::llabs(hist[n - 1].basin - back.basin) > fillTolBasin) {
        if (!chanEarly++)
          chanEarlyWho = Format(" first at tick %d: %d -> %d held while the basin moved %lld eighths in %d ticks", done,
                                prevHeldChan, heldChan, hist[n - 1].basin - back.basin, done - back.tick);
      }
      if (heldChan < prevHeldChan && poolAt >= 0) lost += prevHeldChan - heldChan;
      prevHeldChan = heldChan;
      // A PILLAR that stands FOUR samples running (200 ticks): the unheld
      // water round a held column flickers, and the adoption front leaves a
      // held column over a neighbour it has not reached yet for a few ticks --
      // neither is what the owner saw (a column standing for good).
      {
        int persistent = 0;
        for (const auto& xz : s.pillarCols)
          if (pillarAge[xz] >= 3) persistent++;
        std::map<std::pair<int, int>, int> nextAge;
        for (const auto& xz : s.pillarCols) nextAge[xz] = prevPillars.count(xz) ? pillarAge[xz] + 1 : 1;
        pillarAge = nextAge;
        if (persistent > pillarsMax) {
          pillarsMax = persistent;
          pillarWho = Format(" at tick %d:%s", done, s.pillarWho.c_str());
        }
        prevPillars = s.pillarCols;
      }
      // The live fill: from the first water in the basin to half its capacity
      // under the sill.
      if (liveFrom < 0 && s.vol[K_BASIN] > 0) { liveFrom = done; liveVol0 = s.vol[K_BASIN] + s.vol[K_CHAN]; }
      if (liveFrom >= 0 && liveTo < 0 && s.vol[K_BASIN] * 2 >= basinCap) { liveTo = done; liveVol1 = s.vol[K_BASIN] + s.vol[K_CHAN]; }
      // Pooled: the plain moved under fillTol over the steady window.
      if (poolAt < 0 && n > size_t(steadySamples) && s.vol[K_PLAIN] > 0 && std::llabs(hist[n - 1].plain - back.plain) <= fillTolPlain)
        poolAt = done;
      if (chanHeldAt < 0 && heldChan >= static_cast<int>(adoptMin * chanCols)) chanHeldAt = done;
      if (done >= minTicks && poolAt >= 0 && done - poolAt >= steadyTicks && s.awake[K_NOTCH] + s.awake[K_PLAIN] <= floodAwakeMax &&
          chanHeldAt >= 0 && done - chanHeldAt >= 300 && s.awake[K_CHAN] <= chanAwakeMax)
        break;
    }
  }
  if (liveTo > liveFrom && liveFrom >= 0) liveRate10 = (liveVol1 - liveVol0) * 10 / (liveTo - liveFrom);
  const Census end1 = s;
  // WHAT MOVES in the awake channel chunks (CLAUDE.md rule 6): the channel's
  // water cells now and 16 ticks on, every changed one with its column's held
  // target -- an inner conveyor names its columns.
  if (end1.awake[K_CHAN] > chanAwakeMax) {
    Snap a = snap;
    {
      sandvox::support::TickCursor tick{c, t, fixture};
      for (int k = 0; k < 16; k++) tick();
    }
    Snap b;
    snapshot(b);
    std::string moved;
    int nMoved = 0;
    for (const auto& [xz, kf] : interior) {
      if (kf.first != K_CHAN) continue;
      for (int y = kf.second; y <= kf.second + 6; y++) {
        const uint32_t wa = a.At(xz.first, y, xz.second) & 0xFF00FFFFu, wb = b.At(xz.first, y, xz.second) & 0xFF00FFFFu;
        if (wa == wb) continue;
        if (nMoved++ < 12) {
          const int t8 = recAt(b, xz.first, xz.second, 2) != 0u ? static_cast<int>(recAt(b, xz.first, xz.second, 2) << 12) >> 12 : -1;
          moved += Format(" (%d,%d,%d) %u/%u->%u/%u t8 %d (y%d+%d)", xz.first, y, xz.second, wa & 0xFFFu, ((wa >> 12) & 15u) + 1u,
                          wb & 0xFFFu, ((wb >> 12) & 15u) + 1u, t8, t8 >= 0 ? (t8 - 1) >> 3 : -1,
                          t8 >= 0 ? t8 - 8 * ((t8 - 1) >> 3) : 0);
        }
      }
    }
    std::printf("stream-natural:   the channel over 16 more ticks: %d water words changed:%s\n", nMoved, moved.c_str());
  }
  std::printf("stream-natural: phase 1 (tick: volumes in eighths | held columns | awake chunks | pillars | sim_stream's "
              "release / adoption counters):%s\n",
              curve.c_str());
  // G. ECHO (the render's record, STILL_ECHO, sim_stream.wgsl echoSample):
  // what the held columns kept for raymarch.wgsl to play back. A held column
  // with both words 0 plays the procedural flicker instead (not wrong, but
  // then the recorder did nothing); a sample further than echoRangeMax
  // eighths from the target (other than -128, "no water") is not the surface
  // the column was held at -- a recording taken against the wrong anchor, or
  // not rebased at adoption. Asserted: at least echoRecordedMin of the held
  // columns carry a recording, and no sample is out of range.
  {
    const double echoRecMin = BaselineNumber("streamNat.echoRecordedMin", 0.5);
    const int echoRange = num("streamNat.echoRangeMax", 24);
    int heldN = 0, recorded = 0, moving = 0, dry = 0, outOfRange = 0, vMin = 127, vMax = -127;
    std::string oorWho;
    for (const auto& xz : end1.heldCols) {
      heldN++;
      bool mv = false, dr = false;
      if ((echoAt(snap, xz.first, xz.second, 0) | echoAt(snap, xz.first, xz.second, 1)) != 0u) recorded++;
      for (int w = 0; w < 2; w++)
        for (int b = 0; b < 4; b++) {
          const int8_t v = static_cast<int8_t>((echoAt(snap, xz.first, xz.second, w) >> (8 * b)) & 0xFFu);
          if (v == -128) { dr = true; continue; }
          if (v != 0) mv = true;
          vMin = std::min(vMin, int(v));
          vMax = std::max(vMax, int(v));
          if (std::abs(int(v)) > echoRange && outOfRange++ < 4)
            oorWho += Format(" (%d,%d) slot %d: %d", xz.first, xz.second, w * 4 + b, int(v));
        }
      moving += mv;
      dry += dr;
    }
    const bool gOk = heldN > 0 && recorded >= echoRecMin * heldN && outOfRange == 0;
    std::printf("stream-natural: G echo: %s (of %d held columns %d carry a recording (>= %.2f), %d of them a moving "
                "surface, %d a dry sample; samples %d..%d eighths, %d past +-%d%s)\n",
                gOk ? "PASS" : "FAIL", heldN, recorded, echoRecMin, moving, dry, vMin, vMax, outOfRange, echoRange,
                oorWho.c_str());
    if (!gOk) problems.push_back(Format("G: %d / %d held columns recorded, %d samples out of range", recorded, heldN, outOfRange));
  }
  if (end1.observed == 0) problems.push_back("sim_stream never observed a footprint column (did its kernels compile?)");
  const int heldFloodEnd = end1.held[K_NOTCH] + end1.held[K_PLAIN];
  const bool aOk = floodEarly == 0 && poolAt >= 0 && end1.awake[K_NOTCH] + end1.awake[K_PLAIN] <= floodAwakeMax;
  std::printf("stream-natural: A flood: %s (held while the plain filled: %d samples%s; pooled at tick %d; at the end %d "
              "flood columns held, %d + %d flood chunks awake (<= %d))\n",
              aOk ? "PASS" : "FAIL", floodEarly, floodEarlyWho.c_str(), poolAt, heldFloodEnd, end1.awake[K_NOTCH],
              end1.awake[K_PLAIN], floodAwakeMax);
  if (!aOk) problems.push_back(Format("A: flood held early %d, pooled at %d, %d awake", floodEarly, poolAt, end1.awake[K_NOTCH] + end1.awake[K_PLAIN]));
  const bool bOk = pillarsMax == 0;
  std::printf("stream-natural: B pillars: %s (worst sample %d%s)\n", bOk ? "PASS" : "FAIL", pillarsMax, pillarWho.c_str());
  if (!bOk) problems.push_back(Format("B: %d pillars%s", pillarsMax, pillarWho.c_str()));
  const bool cOk = chanEarly == 0 && chanHeldAt >= 0 && end1.awake[K_CHAN] <= chanAwakeMax;
  std::string chanWhy;
  for (int b = 0; b < kDirtyReasonBits; b++)
    if ((end1.awakeWhy[K_CHAN] >> b) & 1u) chanWhy += Format(" %s", kDirtyReasonName[b]);
  std::printf("stream-natural: C channel: %s (held while the basin filled: %d samples%s; >= %.2f of %d held at tick %d; "
              "at the end %d held, %d channel chunks awake (<= %d):%s, by%s)\n",
              cOk ? "PASS" : "FAIL", chanEarly, chanEarlyWho.c_str(), adoptMin, chanCols, chanHeldAt, end1.held[K_CHAN],
              end1.awake[K_CHAN], chanAwakeMax, end1.awakeWhere.c_str(), chanWhy.c_str());
  if (!cOk) problems.push_back(Format("C: channel held early %d, held at %d, %d awake", chanEarly, chanHeldAt, end1.awake[K_CHAN]));
  const bool dOk = end1.relMax <= releaseMax && lost == 0;
  std::printf("stream-natural: D cycling: %s (most releases of one column %d (<= %d)%s%s; %d releases, %d adoptions in all, "
              "reasons seen 0x%08x; held channel columns lost between samples after the flood pooled: %d)\n",
              dOk ? "PASS" : "FAIL", end1.relMax, releaseMax, end1.relWho.empty() ? "" : ": ", end1.relWho.c_str(),
              end1.relTotal, end1.adoptTotal, end1.relWhy, lost);
  if (!dOk) problems.push_back(Format("D: %d releases of one column, %d lost", end1.relMax, lost));

  // ---- phase 2: the extension opened -- under the held stream, and (E's
  // CONTROL) under the same stream never held: a fresh world, the same
  // build, the same ticks with sim.streamSteadyTicks at its ceiling (65535
  // ticks: nothing is adopted), the door opened at the same tick. What the
  // extension and the basin region gain per tick after the opening is the
  // stream's DELIVERY; held must deliver what live does. ----
  auto region = [&](const Census& q) { return q.vol[K_CHAN] + q.vol[K_BASIN] + q.vol[K_EXT] + q.vol[K_DOOR]; };
  struct Arm {
    long long rate10 = 0, gainEnd = 0;
    int t0 = -1, t1 = -1, heldAtOpen = 0, heldEnd = 0, relEnd = 0, relMax = 0;
    std::string curve, relWho;
  };
  auto openAndMeasure = [&](Arm& arm) {
    sandvox::support::TickCursor tick{c, t, fixture};
    s = census(snap);
    const long long vOpen = region(s);
    arm.heldAtOpen = s.held[K_CHAN] + s.held[K_BASIN];
    sandvox::support::TickOps o;
    o.cells = openDoor;
    tick(o);
    long long v0 = -1, v1 = -1;
    const int step = std::max(10, std::min(sampleTicks, rateDelay));
    for (int k = 1; k <= extTicks;) {
      for (int j = 0; j < step && k <= extTicks; j++, k++) tick();
      const int at = k - 1;
      if (at >= rateDelay && v0 < 0) { s = census(snap); v0 = region(s); arm.t0 = at; arm.curve += "\n " + line(at, s); continue; }
      if (arm.t0 >= 0 && v1 < 0 && at - arm.t0 >= rateTicks) { s = census(snap); v1 = region(s); arm.t1 = at; arm.curve += "\n " + line(at, s); continue; }
      if (at % (step * 5) == 0) { s = census(snap); arm.curve += "\n " + line(at, s); }
    }
    s = census(snap);
    arm.gainEnd = region(s) - vOpen;
    arm.heldEnd = s.held[K_CHAN] + s.held[K_BASIN];
    arm.relEnd = s.relTotal;
    arm.relMax = s.relMax;
    arm.relWho = s.relWho;
    arm.rate10 = (arm.t1 > arm.t0 && arm.t0 >= 0) ? (v1 - v0) * 10 / (arm.t1 - arm.t0) : 0;
  };
  Arm heldArm, liveArm;
  openAndMeasure(heldArm);
  std::printf("stream-natural: phase 2, the extension opened under the held stream:%s\n", heldArm.curve.c_str());
  const int phase1Ticks = done;
  {
    Tuning never = CurrentTuning();
    never.sim.streamSteadyTicks = 65535;
    SetCurrentTuning(never);
    setup();
    sandvox::support::TickCursor tick{c, t, fixture};
    sandvox::support::TickOps o;
    o.cells = openGate;
    tick(o);
    for (int k = 1; k < phase1Ticks; k++) tick();
    s = census(snap);
    std::printf("stream-natural:   CONTROL at the opening (never held):%s\n", line(phase1Ticks, s).c_str());
    openAndMeasure(liveArm);
    std::printf("stream-natural: phase 2 CONTROL, the extension opened under the live stream:%s\n", liveArm.curve.c_str());
    Tuning back = CurrentTuning();
    back.sim.streamSteadyTicks = steadyTicks;
    SetCurrentTuning(back);
  }
  {
    const double ratio = liveArm.rate10 > 0 ? double(heldArm.rate10) / double(liveArm.rate10) : 0.0;
    const double ratioEnd = liveArm.gainEnd > 0 ? double(heldArm.gainEnd) / double(liveArm.gainEnd) : 0.0;
    const bool eOk = liveArm.rate10 > 0 && heldArm.heldAtOpen > 0 && ratio >= rateLo && ratio <= rateHi;
    std::printf("stream-natural: E delivery: %s (the basin region's gain after the extension opened, per 10 ticks over "
                "ticks %d..%d: HELD (%d channel + basin columns held at the opening, %d at the end) %lld, LIVE (never "
                "held) %lld, ratio %.2f in [%.2f, %.2f]; over all %d ticks %lld vs %lld eighths, ratio %.2f; for reference "
                "the live stream first filled the basin at %lld / 10 ticks)\n",
                eOk ? "PASS" : "FAIL", heldArm.t0, heldArm.t1, heldArm.heldAtOpen, heldArm.heldEnd, heldArm.rate10,
                liveArm.rate10, ratio, rateLo, rateHi, extTicks, heldArm.gainEnd, liveArm.gainEnd, ratioEnd, liveRate10);
    // D, after the opening: a dug extension drains the held basin and lets
    // its columns go -- once each; nothing may be taken and let go again.
    const bool d2Ok = heldArm.relMax <= releaseMax;
    std::printf("stream-natural: D cycling after the opening: %s (most releases of one column %d (<= %d)%s%s; %d releases "
                "in all)\n",
                d2Ok ? "PASS" : "FAIL", heldArm.relMax, releaseMax, heldArm.relWho.empty() ? "" : ": ", heldArm.relWho.c_str(),
                heldArm.relEnd);
    if (!d2Ok) problems.push_back(Format("D: %d releases of one column after the opening", heldArm.relMax));
    RecordObserved("streamNat.observedLiveRate10", static_cast<double>(liveArm.rate10));
    RecordObserved("streamNat.observedHeldRate10", static_cast<double>(heldArm.rate10));
    if (!eOk)
      problems.push_back(Format("E: delivery ratio %.2f (held %lld, live %lld per 10 ticks; %d held at the opening)", ratio,
                                heldArm.rate10, liveArm.rate10, heldArm.heldAtOpen));
  }
  RecordObserved("streamNat.observedPoolAt", static_cast<double>(poolAt));
  RecordObserved("streamNat.observedChanHeldAt", static_cast<double>(chanHeldAt));

  // ---- F. determinism: detRuns runs of the first detTicks ----
  if (detRuns >= 2 && detTicks > 0) {
    const uint32_t tE = t;
    std::vector<uint64_t> hashes;
    for (int r = 0; r < detRuns; r++) {
      t = tE;
      setup();
      sandvox::support::TickCursor tick{c, t, fixture};
      sandvox::support::TickOps o;
      o.cells = openGate;
      tick(o);
      for (int k = 1; k < detTicks; k++) tick();
      snapshot(snap);
      uint64_t h = 1469598103934665603ull;
      for (uint32_t w : snap.vox) h = (h ^ (w & ~0x00FF0000u)) * 1099511628211ull;
      for (const auto& [xz, kd] : kindOf) {
        for (int w = 0; w < 4; w++) h = (h ^ recAt(snap, xz.first, xz.second, w)) * 1099511628211ull;
        for (int w = 0; w < 8; w++) h = (h ^ obsAt(snap, xz.first, xz.second, w)) * 1099511628211ull;
        for (int w = 0; w < 2; w++) h = (h ^ echoAt(snap, xz.first, xz.second, w)) * 1099511628211ull;
      }
      hashes.push_back(h);
    }
    bool same = true;
    std::string hl;
    for (size_t r = 0; r < hashes.size(); r++) {
      hl += Format(" %016llx", static_cast<unsigned long long>(hashes[r]));
      if (r >= 1 && hashes[r] != hashes[1]) same = false;   // run 0 follows the long phase: 1 vs 2 is the rule-1 pair
    }
    if (hashes.size() == 2) same = hashes[0] == hashes[1];
    std::printf("stream-natural: F determinism: %s (%d runs of %d ticks from a fresh worldgen:%s; runs 2.. must agree)\n",
                same ? "PASS" : "FAIL", detRuns, detTicks, hl.c_str());
    if (!same) problems.push_back("F: runs of the same fixture differ --" + hl);
  }

  // ---- the pristine world back ----
  restoreTuning();
  {
    biomes::EnvironmentStamp stamp;
    if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, log)) {
      worldmap::SetCurrentWorldMap(real);
      problems.push_back("ReloadEnvironment REFUSED afterwards: " + log);
    }
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    DrainFullRefill(c.ctx, c.world, c.sim, IVec3{108 >> 4, 122 >> 4, 108 >> 4});
  }
  cleanup();
  if (!problems.empty()) {
    detail = problems[0];
    for (size_t i = 1; i < problems.size(); i++) detail += "; " + problems[i];
    std::printf("stream-natural: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  detail = "flood stays live and pools, no pillars, channel held only after the basin filled, no cycling, delivery kept, twice-run identical";
  std::printf("stream-natural: PASS\n");
  return Status::Pass;
}

const std::vector<Gate>& RiverGates() {
  static const std::vector<Gate> g = {
      {"rivers", "sim", {}, false, GateRivers},
      {"stream-natural", "sim", {}, false, GateStreamNatural},
  };
  return g;
}

}  // namespace selftest
