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
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/biomes.h"
#include "sim/currentprim.h"
#include "sim/flowfield.h"
#include "sim/horizon.h"
#include "sim/treeatlas.h"
#include "sim/weather.h"
#include "sim/world.h"
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
        auto adoptedCount = [&]() {
          std::vector<uint32_t> tab(size_t(kWorldN) * kWorldN * 4u);
          rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, still, 0, tab.data(), tab.size() * 4, "riversStill");
          int k = 0;
          floodMax = 0;
          for (const auto& xz : byCols) {
            const size_t si = (size_t(xz.second & int(kWorldN - 1)) * kWorldN + size_t(xz.first & int(kWorldN - 1))) * 4u;
            if (tab[si + 2] != 0u) {
              k++;
              floodMax = std::max(floodMax, (tab[si + 3] >> 12) & 4095u);
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
        auto awakeOn = [&]() {
          std::vector<uint32_t> fl2(kNumSlots, 0);
          rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.DirtyActive(), 0, fl2.data(), kNumSlots * 4, "riversActiveS");
          int k = 0;
          for (uint32_t ci : byChunks)
            if (ci < kNumSlots && fl2[ci] != 0u) k++;
          return k;
        };
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
            curve += Format(" %d:%d/%d/%d/%u", k, wet, adopted, awake, floodMax);
          }
        }
        c.ctx.WaitIdle();
        wet = wetCount();
        adopted = adoptedCount();
        awake = awakeOn();
        const int nBy = static_cast<int>(byCols.size());
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
            curve2 += Format(" %d:%d/%d", k, wetCount(), adoptedCount());
          }
        }
        c.ctx.WaitIdle();
        const int adoptedAfter = adoptedCount();
        const int wetAfter = wetCount();
        const double adoptMin = BaselineNumber("rivers.streamAdoptMin", 0.5);
        const int awakeMaxS = static_cast<int>(BaselineNumber("rivers.streamAwakeMax", 2));
        const double releaseMax = BaselineNumber("rivers.streamReleaseMax", 0.2);
        const double flapMax = BaselineNumber("rivers.streamFlapMax", 0.1);
        const bool sOk = nBy > 50 && wet >= nBy / 2 && adopted >= static_cast<int>(adoptMin * nBy) && awake <= awakeMaxS &&
                         lost <= static_cast<int>(flapMax * nBy) && adoptedAfter <= static_cast<int>(releaseMax * nBy);
        std::printf("rivers: S stream held: %s (channel %d columns: after %d ticks %d wet, %d adopted (>= %.2f), %d of its "
                    "%zu chunks awake (<= %d), %d released while running (<= %.2f); plugged, %d ticks on: %d adopted "
                    "(<= %.2f), %d wet; tick:wet/adopted/awake/floodMax%s; after the plug tick:wet/adopted%s)\n",
                    sOk ? "PASS" : "FAIL", nBy, streamTicks, wet, adopted, adoptMin, awake, byChunks.size(), awakeMaxS, lost,
                    flapMax, drainTicks, adoptedAfter, releaseMax, wetAfter, curve.c_str(), curve2.c_str());
        RecordObserved("rivers.observedStreamAdopted", std::to_string(adopted));
        if (!sOk)
          problems.push_back(Format("S: channel %d cols, %d wet, %d adopted, %d awake, %d released while running; after plug "
                                    "%d adopted", nBy, wet, adopted, awake, lost, adoptedAfter));
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
  detail = "spline, load, bake, lake, CPU == GPU, contained + held, far + horizon, sleep, running, stream held, site index";
  std::printf("rivers: PASS\n");
  return Status::Pass;
}

const std::vector<Gate>& RiverGates() {
  static const std::vector<Gate> g = {
      {"rivers", "sim", {}, false, GateRivers},
  };
  return g;
}

}  // namespace selftest
