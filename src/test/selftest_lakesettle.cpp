// selftest_lakesettle.cpp -- `lake-settle`: a map lake goes FLAT again after
// it is disturbed (sim_step.wgsl's RESERVOIR stage; bug report
// 2026-10-10_03-17-54).
//
// THE BUG. A blast at the bottom of a map lake (map.json `lakes[]`) left a
// terraced mound of water standing on the surface, plus dents, for good. Map
// lakes are not water BODIES (only disc sites and tarns are), so the body
// relevel never ran on them; the reach-1 CA was the only leveller, and its
// fixed point is a ramp of one eighth per two cells with a frozen one-eighth
// rim (memory note gotcha-ca-liquid-levelling-limit). The reservoir stage
// holds such water to its AUTHORED level: once the CA has nothing left to do
// for a free-surface cell above the level it gives an eighth back, and a cell
// short of it takes one.
//
// ONE synthetic map in a TEMP asset dir (the `rivers` seam and exit): flat
// meadow with a basin and one lake polygon round it, its level 10 voxels over
// the basin floor. Then:
//   A. REST: after worldgen and a settle, every lake column's top water cell
//      is full at the level with air over it (the generated lake is already
//      what the stage converges to, so a still lake never enters it).
//   B. CONTROL (sim.stillWaterRelax = 0): a blast on the lake bed, ticked
//      `lakeSettle.ticks`. The surface must still be OFF level -- otherwise
//      the fixture proves nothing about the stage (the CA alone fixed it).
//   C. LIVE, same blast on a fresh worldgen: flat (<= lakeSettle.maxOffCols
//      columns off level) within the same ticks, and asleep after
//      (<= lakeSettle.maxAwake chunks).
//   D. MOUND: a 12x12x3 block of water dropped on the flat surface -- the
//      excess half on its own, no MPM.
//   E. DENT: two whole layers dug out of the surface -- the deficit half.
//   F. DETERMINISM: C's world at tick lakeSettle.detTick, hashed, against a
//      second worldgen + the same blast + the same ticks.
// Every arm prints its off-level curve (columns off / eighths off / highest /
// lowest), so a FAIL says which half of the rule did not finish.
// Leaves the pristine world behind (the `rivers` exit).

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/biomes.h"
#include "sim/treeatlas.h"
#include "sim/tuning.h"
#include "sim/weather.h"
#include "sim/world.h"
#include "sim/worldgen_run.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

namespace selftest {
namespace {

namespace fs = std::filesystem;
using sandvox::kDefaultSeed;

bool WriteTileL(const fs::path& p, uint32_t kind, int tx, int tz, const std::vector<int>& samples) {
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

// How far the lake's surface is from its level, over the lake's columns.
struct Surface {
  int cols = 0;        // lake columns measured
  int off = 0;         // columns whose top water is not (level, full, air above)
  long long eighths = 0;  // SUM |s - (8 level + 8)|
  int hi = 0, lo = 0;  // the highest and lowest s - (8 level + 8)
  int holes = 0;       // columns with no water at all in the band
  uint64_t hash = 0;   // FNV over the band's words (stamp + excite bits masked)
};

}  // namespace

Status GateLakeSettle(Ctx& c, std::string& detail) {
  using sandvox::ReadVoxelsSync;
  using sandvox::ReloadEnvironment;
  using sandvox::SubmitWorldgen;
  const std::string dir = sandvox::AssetDir();
  std::vector<std::string> problems;
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("lake-settle: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  };

  biomes::BiomeSet set;
  std::string log;
  if (!biomes::LoadBiomeSet(dir, c.mats, set, log)) return fail("biome files did not load: " + log);
  TreeAtlas atlas;
  if (!LoadTreeAtlas(dir + "/trees", c.mats, set, atlas, log)) return fail("tree atlas did not load: " + log);
  const worldmap::WorldMapData real = worldmap::CurrentWorldMap();   // a COPY
  if (!real.Loaded()) return fail("no world map loaded");
  uint32_t mWater = 0, mIce = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "water") mWater = static_cast<uint32_t>(i);
    if (c.mats[i].name == "ice") mIce = static_cast<uint32_t>(i);
  }
  if (mWater == 0) return fail("no `water` material");

  // ---- the synthetic map (the `rivers` gate's shape, one lake, no rivers) ----
  const fs::path tmp = fs::temp_directory_path() /
                       ("sv_lakesettle_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path mdir = tmp / "worldmap" / "syn";
  const IVec3 org = c.world.WindowOrigin();
  const int ox = org.x * static_cast<int>(kChunk), oz = org.z * static_cast<int>(kChunk);
  const int ylo = org.y * static_cast<int>(kChunk);
  constexpr int L = 6, P = 1 << L, T = worldmap::kTileSide;
  const int X0 = ((ox - 8192) >> 10) << 10, Z0 = ((oz - 8192) >> 10) << 10;
  const int G = ylo + 100;
  const int lakeX = ox + 256, lakeZ = oz + 256;
  auto authored = [&](int i, int k) -> int {
    const double px = X0 + i * P, pz = Z0 + k * P;
    double h = G + 20;
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
      // The sea just under the ground, for the `rivers` gate's reason: the
      // heat layer's lapse rate counts altitude from it, and a sea far below
      // would skin this lake over with ice at dusk.
      {"seaLevelY", ylo + 40}, {"edgeFadeVox", 2048}, {"warpAmpVox", 0},
      {"biomes", {"ocean", "meadow"}},
      {"terrain", te},
      {"sites", {{{"id", "pad"}, {"kind", "pad"}, {"min", {ox - 256, oz - 256}}, {"max", {ox + 768, oz + 768}}},
                 {{"id", "spawn"}, {"kind", "spawn"}, {"at", {ox + 480, oz + 60}}}}},
      {"rules", nlohmann::json::array()}};
  bool wrote = WriteTileL(mdir / "planes" / "height" / "0_0.svt", worldmap::kPlaneHeight, 0, 0, hs) &&
               WriteTileL(mdir / "planes" / "biome" / "0_0.svt", worldmap::kPlaneBiome, 0, 0, bs);
  {
    std::ofstream f(mdir / "map.json");
    f << mj.dump(2);
    wrote = wrote && static_cast<bool>(f);
  }
  if (!wrote) return fail("could not write the synthetic map under " + tmp.string());
  const std::vector<worldmap::StructurePlacement>* prevOverride = worldmap::StructureOverrideSlot();
  const std::vector<worldmap::StructurePlacement> prevList =
      prevOverride ? *prevOverride : std::vector<worldmap::StructurePlacement>{};
  static const std::vector<worldmap::StructurePlacement> kNone;
  worldmap::SetStructureOverride(&kNone);
  const std::string tdir = tmp.string();
  auto load = [&](const nlohmann::json* j, worldmap::WorldMapData& out, std::string& l) {
    const std::string text = j ? j->dump() : std::string();
    return worldmap::LoadWorldMap(tdir, "syn", set, c.mats.size(), kDefaultSeed, out, l, j ? &text : nullptr);
  };
  const Tuning savedTuning = CurrentTuning();
  const std::string prevPin = weather::Override();
  auto cleanup = [&] {
    worldmap::SetStructureOverride(prevOverride ? &prevList : nullptr);
    SetCurrentTuning(savedTuning);
    weather::SetOverride(prevPin);
    std::error_code ec;
    fs::remove_all(tmp, ec);
  };

  int level = 0;
  {
    worldmap::WorldMapData dry;
    std::string l;
    if (!load(nullptr, dry, l)) { cleanup(); return fail("the dry synthetic map was REFUSED: " + l); }
    worldmap::SetCurrentWorldMap(dry);
    level = worldmap::BareGroundHeight(lakeX, lakeZ, kDefaultSeed) + 10;
  }
  const int floorY = level - 10;
  nlohmann::json pts = nlohmann::json::array();
  for (int k = 0; k < 8; k++) {
    const double a = k * 3.14159265358979 / 4;
    pts.push_back({lakeX + int(std::lround(92 * std::cos(a))), lakeZ + int(std::lround(92 * std::sin(a)))});
  }
  mj["lakes"] = nlohmann::json::array(
      {{{"id", "mere"}, {"level", level}, {"smooth", true}, {"bed", "shore_mud"}, {"shore", "sand"}, {"points", pts}}});
  worldmap::WorldMapData on;
  {
    std::string l;
    if (!load(&mj, on, l) || on.lakes.size() != 1) {
      worldmap::SetCurrentWorldMap(real);
      cleanup();
      return fail("the lake map was REFUSED: " + l);
    }
  }
  worldmap::SetCurrentWorldMap(on);

  // The lake's columns, from the CPU twin of waterApply: inside the polygon,
  // ground under the level, holding water to it.
  std::vector<std::pair<int, int>> cols;
  for (int z = lakeZ - 100; z <= lakeZ + 100; z++)
    for (int x = lakeX - 100; x <= lakeX + 100; x++) {
      const worldmap::WaterColumn wc = worldmap::WaterColumnAt(x, z, kDefaultSeed);
      if (wc.water == level) cols.push_back({x, z});
    }
  if (cols.size() < 2000) {
    worldmap::SetCurrentWorldMap(real);
    cleanup();
    return fail(Format("the lake holds only %zu columns at level %d -- the fixture did not generate", cols.size(), level));
  }

  // ---- the world on the GPU ----
  std::vector<uint32_t> words;
  if (!worldmap::PackWorldMap(set, on, words, log)) {
    worldmap::SetCurrentWorldMap(real);
    cleanup();
    return fail("the synthetic map did not pack: " + log);
  }
  c.ctx.WaitIdle();
  c.sim.UploadEnvironment(c.ctx.device, c.ctx.queue, atlas, words);
  weather::SetOverride("clear");

  // The band the measure reads: from under the deepest dent to over the
  // highest mound the arms make.
  const int yBot = level - 6, yTop = level + 12;
  const int mm = static_cast<int>(kNChunk) - 1;
  auto measure = [&]() -> Surface {
    c.ctx.WaitIdle();
    std::map<std::pair<int, int>, std::vector<uint32_t>> rows;
    auto word = [&](int x, int y, int z) -> uint32_t {
      const int cy = y >> 4, cz = z >> 4;
      auto it = rows.find({cy, cz});
      if (it == rows.end()) {
        std::vector<uint32_t> run(size_t(kNChunk) * kChunkVol);
        ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({0, cy, cz}), kNChunk, run.data(), "lakeSettle");
        it = rows.emplace(std::make_pair(cy, cz), std::move(run)).first;
      }
      const uint32_t local = (uint32_t(z & 15) * kChunk + uint32_t(y & 15)) * kChunk + uint32_t(x & 15);
      return it->second[size_t((x >> 4) & mm) * kChunkVol + local];
    };
    Surface s;
    uint64_t h = 1469598103934665603ull;
    const int want = 8 * level + 8;
    for (const auto& [x, z] : cols) {
      s.cols++;
      int got = INT32_MIN;
      for (int y = yTop; y >= yBot; y--) {
        const uint32_t w = word(x, y, z);
        h = (h ^ (w & ~0x00FF0000u)) * 1099511628211ull;
        if (got != INT32_MIN) continue;
        const uint32_t m = w & 0xFFFu;
        if (m == mWater) got = 8 * y + int((w >> 12) & 15u) + 1;
        else if (m == mIce) got = 8 * y + 8;
      }
      if (got == INT32_MIN) { s.holes++; s.off++; s.eighths += 8 * (level - yBot + 1); continue; }
      const int d = got - want;
      if (d != 0) {
        s.off++;
        s.eighths += d < 0 ? -d : d;
        s.hi = std::max(s.hi, d);
        s.lo = std::min(s.lo, d);
      }
    }
    s.hash = h;
    return s;
  };
  auto show = [](const Surface& s) {
    return Format("%d/%d off, %lld eighths, hi %+d lo %+d, %d holes", s.off, s.cols, s.eighths, s.hi, s.lo, s.holes);
  };

  const IVec3 fixture{lakeX >> 4, level >> 4, lakeZ >> 4};
  const int ticks = static_cast<int>(BaselineNumber("lakeSettle.ticks", 1800));
  const int step = std::max(1, static_cast<int>(BaselineNumber("lakeSettle.sampleTicks", 300)));
  const int maxOff = static_cast<int>(BaselineNumber("lakeSettle.maxOffCols", 0));
  const uint32_t maxAwake = static_cast<uint32_t>(BaselineNumber("lakeSettle.maxAwake", 32));
  const int detTick = static_cast<int>(BaselineNumber("lakeSettle.detTick", 300));
  const int relax = std::max(1, savedTuning.sim.stillWaterRelax);
  uint32_t t = 94000;

  // Fresh world at `rate`, ticked to rest.
  auto regen = [&](int rate) -> Surface {
    Tuning tt = savedTuning;
    tt.sim.stillWaterRelax = rate;
    SetCurrentTuning(tt);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    sandvox::support::TickCursor tick{c, t, fixture};
    for (int k = 0; k < 240; k++) tick();
    return measure();
  };
  const ExplosionOp blast{lakeX, floorY + 2, lakeZ, 12, 256, 0, 0, 0};
  // One disturbance, then up to `ticks` ticks, sampling every `step`; stops
  // early once flat AND asleep. Returns the last sample; `curve` the trace.
  auto run = [&](const sandvox::support::TickOps& first, std::string& curve, uint32_t& awake,
                 int& flatAt) -> Surface {
    sandvox::support::TickCursor tick{c, t, fixture};
    tick(first);
    Surface s;
    flatAt = -1;
    awake = kNumSlots;
    for (int done = 1; done < ticks;) {
      const int n = std::min(step, ticks - done);
      for (int k = 0; k < n; k++) tick();
      done += n;
      s = measure();
      awake = sandvox::ReadActiveChunksSync(c.ctx, c.world, c.sim);
      curve += Format(" [%d: %d off %lldE %+d/%+d, %u awake]", done, s.off, s.eighths, s.hi, s.lo, awake);
      if (s.off <= maxOff && flatAt < 0) flatAt = done;
      if (s.off <= maxOff && awake <= maxAwake) break;
    }
    return s;
  };
  // One arm: report, assert flat + asleep.
  auto arm = [&](const char* tag, const char* what, const sandvox::support::TickOps& ops) {
    std::string curve;
    uint32_t awake = 0;
    int flatAt = -1;
    const Surface s = run(ops, curve, awake, flatAt);
    const bool ok = s.off <= maxOff && awake <= maxAwake;
    std::printf("lake-settle: %s %s: %s (flat at tick %d; %s; %u awake <= %u)\n  curve:%s\n", tag, what,
                ok ? "PASS" : "FAIL", flatAt, show(s).c_str(), awake, maxAwake, curve.c_str());
    if (!ok) problems.push_back(Format("%s %s: %s, %u awake", tag, what, show(s).c_str(), awake));
    return flatAt;
  };

  // ---- A. rest ----
  const Surface rest = regen(relax);
  const bool aOk = rest.off == 0;
  std::printf("lake-settle: A rest: %s (lake 'mere' level %d over %d columns, floor %d: %s)\n", aOk ? "PASS" : "FAIL",
              level, rest.cols, floorY, show(rest).c_str());
  if (!aOk) problems.push_back("A: the generated lake is not at its level: " + show(rest));

  // ---- B. control: the CA alone ----
  {
    regen(0);
    std::string curve;
    uint32_t awake = 0;
    int flatAt = -1;
    sandvox::support::TickOps ops;
    ops.exps.push_back(blast);
    const Surface ctrl = run(ops, curve, awake, flatAt);
    const bool bOk = ctrl.off > maxOff;
    std::printf("lake-settle: B control (stillWaterRelax 0, lake-bed blast): %s (%s; %u awake)\n  curve:%s\n",
                bOk ? "PASS" : "FAIL -- the CA alone flattened it, so C proves nothing", show(ctrl).c_str(), awake,
                curve.c_str());
    if (!bOk) problems.push_back("B: the control arm went flat without the reservoir: the fixture proves nothing");
    RecordObserved("lakeSettle.observedControlOff", static_cast<double>(ctrl.off));
  }

  // ---- C. live blast ----
  {
    regen(relax);
    sandvox::support::TickOps ops;
    ops.exps.push_back(blast);
    const int flatAt = arm("C", Format("lake-bed blast (stillWaterRelax %d)", relax).c_str(), ops);
    RecordObserved("lakeSettle.observedBlastFlatTick", static_cast<double>(flatAt));
  }
  // ---- D. mound, on C's world ----
  {
    sandvox::support::TickOps ops;
    const uint32_t full = mWater | (7u << 12);
    for (int y = level + 1; y <= level + 3; y++)
      for (int z = lakeZ - 6; z < lakeZ + 6; z++)
        for (int x = lakeX + 24; x < lakeX + 36; x++) ops.cells.push_back({World::SlotCellIndex({x, y, z}), full});
    arm("D", "mound (12x12x3 dropped on the surface)", ops);
  }
  // ---- E. dent, on the same world ----
  {
    sandvox::support::TickOps ops;
    for (int y = level - 1; y <= level; y++)
      for (int z = lakeZ - 6; z < lakeZ + 6; z++)
        for (int x = lakeX - 36; x < lakeX - 24; x++) ops.cells.push_back({World::SlotCellIndex({x, y, z}), 0u});
    arm("E", "dent (two layers dug out of the surface)", ops);
  }
  // ---- F. determinism ----
  // Twice from the same worldgen at the SAME tick numbers, with one-layer
  // disturbances only (a one-cell step never excites the MPM and nothing here
  // throws debris), so the two runs start from the same engine state and any
  // difference is the rule's, not leftover particles from the arms above.
  {
    const uint32_t tF = t;
    uint64_t hash[2] = {0, 0};
    for (int r = 0; r < 2; r++) {
      t = tF;
      regen(relax);
      sandvox::support::TickCursor tick{c, t, fixture};
      sandvox::support::TickOps ops;
      const uint32_t full = mWater | (7u << 12);
      for (int z = lakeZ - 8; z < lakeZ + 8; z++)
        for (int x = lakeX - 8; x < lakeX + 8; x++) {
          ops.cells.push_back({World::SlotCellIndex({x + 30, level + 1, z}), full});
          ops.cells.push_back({World::SlotCellIndex({x - 30, level, z}), 0u});
        }
      tick(ops);
      for (int k = 1; k < detTick; k++) tick();
      hash[r] = measure().hash;
    }
    const bool fOk = hash[0] == hash[1];
    std::printf("lake-settle: F determinism: %s (lake band %d ticks after a one-layer mound + dent: %016llx vs %016llx)\n",
                fOk ? "PASS" : "FAIL", detTick, (unsigned long long)hash[0], (unsigned long long)hash[1]);
    if (!fOk) problems.push_back("F: two runs of the same disturbance differ -- RULE 1");
  }

  // ---- the pristine world back ----
  biomes::EnvironmentStamp stamp;
  if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, log)) {
    worldmap::SetCurrentWorldMap(real);
    problems.push_back("ReloadEnvironment REFUSED afterwards: " + log);
  }
  cleanup();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  DrainFullRefill(c.ctx, c.world, c.sim, IVec3{108 >> 4, 122 >> 4, 108 >> 4});
  if (!problems.empty()) {
    detail = problems[0];
    for (size_t i = 1; i < problems.size(); i++) detail += "; " + problems[i];
    std::printf("lake-settle: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  detail = "rest, control off-level, blast / mound / dent flat and asleep, twice-run identical";
  std::printf("lake-settle: PASS\n");
  return Status::Pass;
}

const std::vector<Gate>& LakeSettleGates() {
  static const std::vector<Gate> g = {
      {"lake-settle", "sim", {}, false, GateLakeSettle},
  };
  return g;
}

}  // namespace selftest
