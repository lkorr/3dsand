// selftest_altitude.cpp -- `altitude`: nothing absolute-Y that should be
// relative (docs/PLAN_map_v2.md A1).
//
// WHAT THIS GUARDS. Map v2 lets an author paint a 1,400 m massif, and every
// rule that used to hold one absolute Y -- the boot window at 0, the treeline
// at y 228, the sediment ceiling at y 264, the deep caves at y -4..-11 m, a
// climate that ignored altitude -- was a rule for a world 20 m tall. On the
// `peaks` fixture (scripts/gen_map_peaks.py; loaded here through the real
// LoadWorldMap / PackWorldMap / UploadEnvironment, whatever map the run has):
//
//   A. THE WINDOW. worldmap::WindowChunkYForGround (what SpawnWindowOrigin and
//      every scripted scene use) holds the spawn's ground on a 900 m valley
//      with room above and below -- and is still 0 for the harness map's
//      spawn, so no scene's window moved.
//   B. THE TREELINE. worldmap::TreelineAt (the height mirror's treelineAt):
//      every pine column's treeline is above every tundra column's (the
//      biomes' treelineScale), each within +-treelineJitter of its biome's
//      line, and the jitter makes it no ruled contour.
//   C. THE LAPSE RATE. HeatAmbientCpu (the GPU heatAmbient's twin): the same
//      column at 900 m is colder than at the sea by exactly
//      sim.heatLapsePerKm's cooling, and at 1,200 m (above every treeline on
//      this map: the snowline climate) colder still.
//   D. LAVA. The magma table is one flat plane (worldgen.wgsl LAVA_LEVEL) and
//      no column within LAVA_CLEAR of it floods: the gorge floor, the lowest
//      ground the massif's flank has, stands clear of it by the bound.
//   E. SEDIMENT ON A HIGH VALLEY, CPU and GPU: the wedge's ceiling is relative
//      to the authored ground, so the 900 m valley floor carries a wedge (the
//      twin's Land.sed) and the generated world has powder under its skin.
//   F. THE CLIFF BAND: a window on the 300 m escarpment (~7 voxels a column)
//      skins its steep columns as rock (no grass, no snow, no powder) and
//      settles to <= 32 active chunks (rule 2).
//
// Leaves the pristine world behind: the real environment reloaded from disk,
// the window put back, the world regenerated and the far cascades refilled
// (the `clearing` gate's exit).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "sim/biomes.h"
#include "sim/heat.h"
#include "sim/stream.h"
#include "sim/treeatlas.h"
#include "sim/tuning.h"
#include "sim/world.h"
#include "sim/worldgen_run.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// worldgen.wgsl's magma table and its clearance (LAVA_LEVEL, LAVA_CLEAR), in
// voxels at the shipped 10 voxels / metre (the peaks map's reference scale:
// vlen is the identity there). Restated, not shared: they are shader consts.
constexpr int kLavaLevel = -80;
constexpr int kLavaClear = 200;

// One window row of chunks per (cy, cz), read once (selftest_clearing's RowRead).
struct Rows {
  Ctx* c = nullptr;
  std::map<std::pair<int, int>, std::vector<uint32_t>> rows;
  uint32_t Mat(int x, int y, int z) {
    if (!c->world.CellInWindow({x, y, z})) return 0xFFFFFFFFu;
    const int cy = y >> 4, cz = z >> 4;
    auto it = rows.find({cy, cz});
    if (it == rows.end()) {
      std::vector<uint32_t> run(size_t(kNChunk) * kChunkVol);
      ReadVoxelsSync(c->ctx, c->world, World::SlotChunkIndex({0, cy, cz}), kNChunk, run.data(), "altitude");
      it = rows.emplace(std::make_pair(cy, cz), std::move(run)).first;
    }
    const int m = static_cast<int>(kNChunk) - 1;
    const uint32_t local = (uint32_t(z & 15) * kChunk + uint32_t(y & 15)) * kChunk + uint32_t(x & 15);
    return it->second[size_t((x >> 4) & m) * kChunkVol + local] & 0xFFFu;
  }
};

Status GateAltitude(Ctx& c, std::string& detail) {
  const std::string dir = AssetDir();
  std::vector<std::string> problems;
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("altitude: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  };
  biomes::BiomeSet set;
  std::string log;
  if (!biomes::LoadBiomeSet(dir, c.mats, set, log)) return fail("biome files did not load: " + log);
  TreeAtlas atlas;
  if (!LoadTreeAtlas(dir + "/trees", c.mats, set, atlas, log)) return fail("tree atlas did not load: " + log);
  const worldmap::WorldMapData real = worldmap::CurrentWorldMap();   // a COPY
  if (!real.Loaded()) return fail("no world map loaded");
  // A, the harness half: the run's own map keeps the window it had.
  const int homeGround = World::TerrainHeight(real.spawnX, real.spawnZ, kDefaultSeed);
  const int homeWy = worldmap::WindowChunkYForGround(homeGround);

  worldmap::WorldMapData pk;
  std::vector<uint32_t> words;
  if (!worldmap::LoadWorldMap(dir, "peaks", set, c.mats.size(), kDefaultSeed, pk, log) ||
      !worldmap::PackWorldMap(set, pk, words, log))
    return fail("the peaks map did not load: " + log);
  const IVec3 savedOrigin = c.world.WindowOrigin();
  c.ctx.WaitIdle();
  worldmap::SetCurrentWorldMap(pk);
  c.sim.UploadEnvironment(c.ctx.device, c.ctx.queue, atlas, words);
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  const int sea = m.seaLevelY;

  // ---- A. the window ---------------------------------------------------------------
  const int sx = m.spawnX, sz = m.spawnZ;
  const int sh = World::TerrainHeight(sx, sz, kDefaultSeed);
  const int wy = worldmap::WindowChunkYForGround(sh);
  const int wlo = wy * static_cast<int>(kChunk), whi = wlo + static_cast<int>(kWorldN);
  const bool aOk = homeWy == 0 && sh > sea + 8000 && sh - wlo >= 64 && whi - sh >= 128;
  std::printf("altitude: A window: %s (peaks spawn ground y%d -> window y%d..y%d, %d below / %d above; "
              "the run's own map's spawn ground y%d -> window chunk %d)\n",
              aOk ? "PASS" : "FAIL", sh, wlo, whi - 1, sh - wlo, whi - sh, homeGround, homeWy);
  if (!aOk) problems.push_back(Format("A: spawn ground y%d in window y%d..y%d (home window chunk %d)", sh, wlo, whi - 1, homeWy));

  // ---- B. the treeline ---------------------------------------------------------------
  {
    int pineId = -1, tundraId = -1, qPine = 256, qTundra = 256;
    for (const biomes::BiomeDef& b : set.biomes) {
      if (b.name == "pine") { pineId = b.index; qPine = worldmap::BiomeTreelineQ8(b); }
      if (b.name == "tundra") { tundraId = b.index; qTundra = worldmap::BiomeTreelineQ8(b); }
    }
    const int J = m.terrain.treelineJitter, T = m.terrain.treeline;
    const int expPine = sea + (((T - sea) * qPine) >> 8), expTundra = sea + (((T - sea) * qTundra) >> 8);
    // Cell centres whose 3 x 3 cells are all one biome: biomeMixAt is exactly
    // that biome's scale there.
    const int half = 1 << (m.biomeLog2 - 1);
    std::vector<int> tlPine, tlTundra;
    int worst = 0;
    for (int k = 1; k + 1 < m.biomePostsZ && (tlPine.size() < 64 || tlTundra.size() < 64); k += 3)
      for (int i = 1; i + 1 < m.biomePostsX; i += 3) {
        const int b = static_cast<int>(m.BiomePost(i, k));
        if (b != pineId && b != tundraId) continue;
        bool uniform = true;
        for (int dk = -1; dk <= 1 && uniform; dk++)
          for (int di = -1; di <= 1 && uniform; di++) uniform = static_cast<int>(m.BiomePost(i + di, k + dk)) == b;
        if (!uniform) continue;
        const int x = m.extentX0 + (i << m.biomeLog2) + half, z = m.extentZ0 + (k << m.biomeLog2) + half;
        const int tl = worldmap::TreelineAt(x, z, kDefaultSeed);
        std::vector<int>& v = b == pineId ? tlPine : tlTundra;
        if (v.size() >= 64) continue;
        v.push_back(tl);
        worst = std::max(worst, std::abs(tl - (b == pineId ? expPine : expTundra)));
      }
    const auto [pMin, pMax] = tlPine.empty() ? std::pair<int, int>{0, 0}
                                             : std::pair<int, int>{*std::min_element(tlPine.begin(), tlPine.end()),
                                                                   *std::max_element(tlPine.begin(), tlPine.end())};
    const int tMax = tlTundra.empty() ? 0 : *std::max_element(tlTundra.begin(), tlTundra.end());
    const bool bOk = tlPine.size() >= 16 && tlTundra.size() >= 16 && pMin > tMax && worst <= J &&
                     J > 0 && pMax - pMin >= J / 4;
    std::printf("altitude: B treeline: %s (map y%d +-%d; pine x%.2f -> y%d, %zu columns y%d..y%d; tundra x%.2f -> y%d, "
                "%zu columns, highest y%d; worst off its biome's line %d)\n",
                bOk ? "PASS" : "FAIL", T, J, qPine / 256.0, expPine, tlPine.size(), pMin, pMax, qTundra / 256.0, expTundra,
                tlTundra.size(), tMax, worst);
    if (!bOk) problems.push_back("B: the per-biome jittered treeline");
  }

  // ---- C. the lapse rate --------------------------------------------------------------
  {
    const int lapse = CurrentTuning().sim.heatLapsePerKm;
    const int yHigh = sea + 9000, yTop = sea + 12000;
    const int snowY = HeatSnowlineYAt(sx, sz, kDefaultSeed);
    const int aSea = HeatAmbientCpu(sx, sea, sz, kDefaultSeed, true);
    const int aHigh = HeatAmbientCpu(sx, yHigh, sz, kDefaultSeed, true);
    const int aTop = HeatAmbientCpu(sx, yTop, sz, kDefaultSeed, true);
    const int want = HeatLapseAt(yHigh) - HeatLapseAt(sea);
    const bool cOk = lapse > 0 && want > 0 && yHigh < snowY && aSea - aHigh == want && yTop >= snowY && aTop < aSea;
    std::printf("altitude: C lapse: %s (%d heat units / km; spawn column by day: %d at the sea, %d at 900 m (%d colder, "
                "want %d), %d at 1,200 m (snowline y%d))\n",
                cOk ? "PASS" : "FAIL", lapse, aSea, aHigh, aSea - aHigh, want, aTop, snowY);
    if (!cOk) problems.push_back("C: the lapse rate");
  }

  // ---- D. lava under the gorge ---------------------------------------------------------
  {
    // The gorge's centreline (gen_map_peaks.py): x = 14000 + 1800 sin(z / 3000).
    int floorMin = 1 << 30, floorZ = 0;
    for (int z = 3000; z <= 28000; z += 125) {
      const int x = static_cast<int>(std::lround(14000.0 + 1800.0 * std::sin(z / 3000.0)));
      for (int dx = -160; dx <= 160; dx += 16) {
        const int h = World::TerrainHeight(x + dx, z, kDefaultSeed);
        if (h < floorMin) { floorMin = h; floorZ = z; }
      }
    }
    const bool dOk = floorMin - kLavaLevel >= kLavaClear;
    std::printf("altitude: D lava: %s (gorge floor at its lowest y%d (z %d); the magma table y%d is %.1f m under it, "
                "bound %.1f m)\n",
                dOk ? "PASS" : "FAIL", floorMin, floorZ, kLavaLevel, (floorMin - kLavaLevel) / 10.0, kLavaClear / 10.0);
    if (!dOk) problems.push_back("D: lava within the bound under the gorge");
  }

  // ---- G. a uniform raise translates the ground exactly --------------------------------
  // Every rule the bare ground (BareGroundHeight: what a locale's anchor delta
  // and a site pad are computed from) reads is relative to the authored
  // height or a gradient, and a uniform raise moves no gradient -- so +200 on
  // every post must move the bare ground by exactly +200, in the calm home
  // area as anywhere. Before A1 the absolute sedCeil took a quarter of the
  // raise back out of the wedge (L1 measured +183 for +200). The one absolute
  // the ground still has is the sea (no wedge on ground below it), so the
  // columns stand well above it.
  {
    constexpr int kRaise = 200;
    struct P { int x, z; const char* what; };
    const P pts[] = {{sx, sz, "spawn"}, {sx + 150, sz - 90, "home area"}, {sx - 280, sz + 200, "home edge"},
                     {sx + 900, sz + 1700, "valley"}, {0, -20000, "foothills"}, {0, -7790, "cliff band"},
                     {9000, 10000, "massif flank"}, {-2000, 16000, "knife ridge"}};
    int before[8];
    for (int i = 0; i < 8; i++) before[i] = worldmap::BareGroundHeight(pts[i].x, pts[i].z, kDefaultSeed);
    worldmap::WorldMapData up = pk;
    for (int16_t& h : up.height) h = static_cast<int16_t>(h + kRaise);
    up.heightDefault += kRaise;
    worldmap::SetCurrentWorldMap(up);
    int exact = 0;
    std::string why;
    for (int i = 0; i < 8; i++) {
      const int after = worldmap::BareGroundHeight(pts[i].x, pts[i].z, kDefaultSeed);
      if (after - before[i] == kRaise) exact++;
      else why += Format(" %s (%d,%d) y%d -> y%d (%+d);", pts[i].what, pts[i].x, pts[i].z, before[i], after, after - before[i]);
    }
    worldmap::SetCurrentWorldMap(pk);
    const bool gOk = exact == 8;
    std::printf("altitude: G uniform raise: %s (+%d on every post: %d / 8 columns' bare ground moved by exactly +%d, "
                "spawn y%d, home area y%d)%s\n",
                gOk ? "PASS" : "FAIL", kRaise, exact, kRaise, before[0], before[1], why.c_str());
    if (!gOk) problems.push_back("G: a uniform raise did not translate the bare ground:" + why);
  }

  // ---- E. sediment on the high valley (CPU twin, then the generated world) --------------
  {
    c.stream.OnRegen();
    c.world.SetWindowOrigin({(sx >> 4) - static_cast<int>(kNChunk) / 2, wy, (sz >> 4) - static_cast<int>(kNChunk) / 2});
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    Rows rd{&c, {}};
    int cols = 0, wedge = 0, powder = 0;
    std::string why;
    for (int z = sz - 200; z <= sz + 200; z += 9)
      for (int x = sx - 200; x <= sx + 200; x += 9) {
        const World::Column col = World::TerrainColumn(x, z, kDefaultSeed);
        const World::PondQuery pq = World::PondNearColumn(x, z, kDefaultSeed);
        if (pq.inDisc || pq.near) continue;
        cols++;
        if (col.sed < 4) continue;
        wedge++;
        const uint32_t mat = rd.Mat(x, col.h - 2, z);
        if (mat < c.mats.size() && c.mats[mat].gpu.klass == CLASS_POWDER) powder++;
        else if (why.empty())
          why = Format(" first miss (%d,%d): wedge %d, y%d holds %s", x, z, col.sed, col.h - 2,
                       mat < c.mats.size() ? c.mats[mat].name.c_str() : "out of window");
      }
    const bool eOk = wedge >= 16 && powder * 4 >= wedge * 3;
    std::printf("altitude: E valley sediment: %s (%d dry columns round the spawn at y~%d, %d carry a wedge >= 4 "
                "(twin), %d of them powder 2 under the ground in the world)%s\n",
                eOk ? "PASS" : "FAIL", cols, sh, wedge, powder, why.c_str());
    if (!eOk) problems.push_back(Format("E: %d wedge columns, %d powder in the world%s", wedge, powder, why.c_str()));
  }

  // ---- F. the cliff band: steep rock, and the world settles ------------------------------
  {
    // The escarpment (gen_map_peaks.py): centred on z = -8000 + 900 sin(x / 5000),
    // 3,000 voxels over ~420. At x = 0 its middle is z ~ -7790.
    const int fx = 0, fz = -7790;
    const int fh = World::TerrainHeight(fx, fz, kDefaultSeed);
    const IVec3 org{(fx >> 4) - static_cast<int>(kNChunk) / 2, worldmap::WindowChunkYForGround(fh),
                    (fz >> 4) - static_cast<int>(kNChunk) / 2};
    c.stream.OnRegen();
    c.world.SetWindowOrigin(org);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    const int rock = m.terrain.rockSlope;
    Rows rd{&c, {}};
    int steep = 0, rocky = 0, steepest = 0;
    std::string why;
    for (int z = fz - 240; z <= fz + 240; z += 11)
      for (int x = fx - 240; x <= fx + 240; x += 11) {
        const World::Column col = World::TerrainColumn(x, z, kDefaultSeed);
        steepest = std::max(steepest, col.slope);
        if (rock <= 0 || col.slope < rock) continue;
        const uint32_t mat = rd.Mat(x, col.h, z);
        if (mat >= c.mats.size()) continue;   // out of the window's height
        steep++;
        const std::string& n = c.mats[mat].name;
        if (c.mats[mat].gpu.klass == CLASS_SOLID && n != "snow" && n != "grass") rocky++;
        else if (why.empty()) why = Format(" first miss (%d,%d): slope %.2f, the ground cell is %s", x, z, col.slope / 256.0, n.c_str());
      }
    // Settle. DIRECT PHASE CALLS ON PURPOSE (W2-O): this measures worldgen
    // and the CA coming to rest on a cliff, like `sleep` -- no body, no mob,
    // no op, so THE tick's other phases have nothing to do here.
    uint32_t t = 0, active = kNumSlots;
    int settled = 0;
    const IVec3 mid{org.x + static_cast<int>(kNChunk) / 2, org.y + static_cast<int>(kNChunk) / 2,
                    org.z + static_cast<int>(kNChunk) / 2};
    const int cap = static_cast<int>(BaselineNumber("altitude.settleTicksMax", 1500));
    for (int i = 1; i <= cap; i++) {
      c.stream.TicketTick(t + 1);
      SubmitTick(c.ctx, c.world, c.sim, ++t, kDefaultSeed, {}, {}, {}, false, mid, false, false);
      if (i >= 300 && i % 100 == 0) {
        c.ctx.WaitIdle();
        active = ReadActiveChunksSync(c.ctx, c.world, c.sim);
        settled = i;
        if (active <= 32) break;
      }
    }
    const uint32_t maxActive = static_cast<uint32_t>(BaselineNumber("altitude.settleMaxActive", 32));
    const bool fOk = steep >= 32 && rocky == steep && active <= maxActive;
    std::printf("altitude: F cliff band: %s (window at (%d,%d) y%d..; steepest landform slope %.2f vox/vox; %d columns "
                "at or past rockSlope %.2f, %d wear rock; %u active chunks after %d ticks, max %u)%s\n",
                fOk ? "PASS" : "FAIL", fx, fz, org.y * static_cast<int>(kChunk), steepest / 256.0, steep, rock / 256.0,
                rocky, active, settled, maxActive, why.c_str());
    if (!fOk) problems.push_back(Format("F: %d of %d steep columns rock, %u active chunks at rest%s", rocky, steep, active, why.c_str()));
  }

  // ---- the pristine world back --------------------------------------------------------------
  {
    biomes::EnvironmentStamp stamp;
    std::string l;
    if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, l)) {
      worldmap::SetCurrentWorldMap(real);
      problems.push_back("ReloadEnvironment REFUSED afterwards: " + l);
    }
    c.stream.OnRegen();
    c.world.SetWindowOrigin(savedOrigin);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    DrainFullRefill(c.ctx, c.world, c.sim, IVec3{108 >> 4, 122 >> 4, 108 >> 4});
  }
  if (!problems.empty()) {
    detail = problems[0];
    for (size_t i = 1; i < problems.size(); i++) detail += "; " + problems[i];
    std::printf("altitude: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  detail = "window, per-biome jittered treeline, lapse rate, lava clearance, valley sediment, cliff rock + settle";
  std::printf("altitude: PASS\n");
  return Status::Pass;
}

}  // namespace

const std::vector<Gate>& AltitudeGates() {
  static const std::vector<Gate> g = {
      {"altitude", "sim", {}, false, GateAltitude},
  };
  return g;
}

}  // namespace selftest
