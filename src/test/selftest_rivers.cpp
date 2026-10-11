// selftest_rivers.cpp -- `rivers`: map.json rivers[] and lakes[] in the engine
// (docs/PLAN_worldbuilder.md W1), and the smooth roads that share their spline.
//
// WHAT THIS GUARDS. A river is a staircase of STILL pools (worldwater.cpp says
// why): the bake makes its surface monotone and flat per reach, worldgen
// carves the channel, holds the banks and builds a weir at every step, and
// the whole thing must be generated AT REST -- the sim must sleep over it
// (CLAUDE.md rule 2), which is the claim this gate exists for. One synthetic
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
//      source -> mouth and flat per piece; the tributary's surface at the
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
//      has AIR beside it or under it -- nothing can flow on tick 1.
//   F. FAR: the level-5 cascade cells (3.2 m) along the 2 m `falls` are
//      water (the narrow-water tint); the horizon's water plane covers main
//      and the lake.
//   G. SLEEP, under a sky pinned clear (rain wakes every exposed cell, river
//      or meadow): ticked to <= rivers.maxAwake (32) awake chunks and HELD
//      there rivers.holdTicks more, at most rivers.maxAwakeWater of them over
//      water or banks; WHERE they are and WHICH field moves when it fails.
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
#include "sim/horizon.h"
#include "sim/treeatlas.h"
#include "sim/weather.h"
#include "sim/world.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

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
  const bool aOk = rMain && rFalls && rCreek && on.lakes.size() == 1 && rMain->reaches >= 3 && !rFalls->falls.empty() &&
                   rFalls->joinIdx >= 0 && rCreek->lakeOut == 0 && !offChannel && unknownRefused && cycleRefused;
  std::printf("rivers: A load: %s (%zu rivers, %zu lakes in %.0f ms; main %d reaches / %d pieces, falls %d reaches, %zu "
              "waterfall(s)%s, creek -> lake %s; unknown join refused %d, cycle refused %d)\n",
              aOk ? "PASS" : "FAIL", on.rivers.size(), on.lakes.size(), loadMs, rMain ? rMain->reaches : -1,
              rMain ? rMain->pieces : -1, rFalls ? rFalls->reaches : -1, rFalls ? rFalls->falls.size() : 0,
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
    // Flat per piece: the piece's surface is the surface at both its ends' samples.
    for (const auto& st : on.sites) {
      if (st.kind != worldmap::kSiteRiver) continue;
      const auto& rv = on.rivers[static_cast<size_t>(st.water)];
      for (const auto& p : rv.line)
        if ((p[0] == st.x && p[1] == st.z && (st.wflags & worldmap::kRivFlatA) == 0 && p[2] != st.surf)) {
          if (!notFlat++) why += Format(" %s: piece %s surface %d, its start sample %d", rv.id.c_str(), st.id.c_str(), st.surf, p[2]);
          break;
        }
    }
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
    const bool bOk = nonMono == 0 && notFlat == 0 && joinOk && creekOk && roadOk;
    std::printf("rivers: B bake: %s (surfaces monotone: %d rises; flat pieces: %d off; junction falls %d >= main %d; creek "
                "ends at %d, the lake's level %d; smooth road %zu vertices, baked grade %.2f %% <= %d %%)%s\n",
                bOk ? "PASS" : "FAIL", nonMono, notFlat, rFalls->line.back()[2], mainAtJoin, rCreek->line.back()[2], lakeLevel,
                rd.points.size(), rd.worstGradeQ8 / 256.0, rd.maxGrade, why.c_str());
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

  // ---- I. the current field runs downstream (currentprim.cpp SeedMapRivers) ----
  // A window centred on main's middle sample: the map-river arm must put more
  // than one stream primitive there (the lattice arm's shared owner handle
  // once collapsed every stream to ONE), and the CPU field just under the
  // surface at that sample must point along the centreline, downstream, at no
  // less than the authored floor.
  {
    using sandvox::CurrentPrims;
    const auto& L = rMain->line;
    const size_t mid = L.size() / 2;
    const size_t ta = mid >= 4 ? mid - 4 : 0, tb = std::min(L.size() - 1, mid + 4);
    float tx = static_cast<float>(L[tb][0] - L[ta][0]), tz = static_cast<float>(L[tb][1] - L[ta][1]);
    const float tl = std::max(std::sqrt(tx * tx + tz * tz), 1e-3f);
    tx /= tl;
    tz /= tl;
    CurrentPrims().Clear();
    const int bx = L[mid][0] - static_cast<int>(kWorldN) / 2;
    const int bz = L[mid][1] - static_cast<int>(kWorldN) / 2;
    const int n = CurrentPrims().SeedMapRivers(bx, bz, kDefaultSeed, 100, 1.2f);
    CurrentPrims().Tick(100 + sandvox::kCurrentPrimAttack);
    const Vec3 v = sandvox::CurrentAtCpu(
        Vec3{L[mid][0] + 0.5f, L[mid][2] - 1.5f, L[mid][1] + 0.5f});
    const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    const float along = v.x * tx + v.z * tz;
    const float floorCells = sandvox::kRiverMinFlowMs / kVoxelMeters;
    // The slab: nothing in the air over the river (it once reached 18 m up).
    // 24 cells, not 4: the reach upstream stands a weir or two higher, and its
    // slab is flat over its own 64 voxels of channel.
    const Vec3 sky = sandvox::CurrentAtCpu(Vec3{L[mid][0] + 0.5f, L[mid][2] + 24.5f, L[mid][1] + 0.5f});
    const float skySpeed = std::sqrt(sky.x * sky.x + sky.y * sky.y + sky.z * sky.z);
    const bool iOk = n >= 2 && speed >= floorCells * 0.9f && along >= 0.9f * speed && skySpeed == 0.0f;
    std::printf("rivers: I current: %s (%d river primitive(s) in the window; at main's middle %.1f cells/s, %.2f of it "
                "downstream, floor %.1f; 24 cells over the surface %.2f)\n",
                iOk ? "PASS" : "FAIL", n, speed, speed > 0.0f ? along / speed : 0.0f, floorCells, skySpeed);
    if (!iOk) problems.push_back(Format("I: current %d prims, %.1f cells/s, along %.1f", n, speed, along));
    CurrentPrims().Clear();
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
    auto vox = [&](int x, int y, int z) -> uint32_t {
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
      return it->second[size_t((x >> 4) & m) * kChunkVol + local] & 0xFFFu;
    };
    auto nameOf = [&](uint32_t id) { return id < c.mats.size() ? c.mats[id].name : std::string("?"); };
    auto isWater = [&](uint32_t id) { return id == mWater || id == mIce; };
    auto isBody = [&](uint32_t id) {
      return id != 0u && id < c.mats.size() && (c.mats[id].gpu.klass == CLASS_SOLID || c.mats[id].gpu.klass == CLASS_POWDER);
    };

    // ---- D. CPU == GPU ----
    {
      int cols = 0, hollow = 0, buried = 0, wetCols = 0, waterBad = 0;
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
            }
          } else if (isBody(vox(x, h + 2, z)) && !isBody(vox(x, h + 1, z))) {
            if (!buried++ && why.empty()) why = Format(" (%d,%d): matter at y%d over the mirror's y%d", x, z, h + 2, h);
          }
        }
      const bool dOk = cols > 5000 && wetCols > 2000 && hollow == 0 && buried == 0 && waterBad == 0;
      std::printf("rivers: D GPU vs twin: %s (%d river/lake columns, %d under water; %d hollow, %d buried, %d water "
                  "mismatches)%s\n",
                  dOk ? "PASS" : "FAIL", cols, wetCols, hollow, buried, waterBad, why.c_str());
      if (!dOk) problems.push_back(Format("D: %d/%d/%d mismatches of %d columns%s", hollow, buried, waterBad, cols, why.c_str()));
    }

    // ---- E. contained ----
    // Every water voxel over every column the mirror gives water (D holds the
    // mirror to the world on those columns): no AIR beside it or under it.
    {
      int water = 0, open = 0;
      std::string why;
      for (int z = oz + 1; z < oz + static_cast<int>(kWorldN) - 1; z++)
        for (int x = ox + 1; x < ox + static_cast<int>(kWorldN) - 1; x++) {
          const worldmap::WaterColumn wc = worldmap::WaterColumnAt(x, z, kDefaultSeed);
          if (wc.water <= wc.h) continue;
          for (int y = wc.h + 1; y <= wc.water + 1; y++) {
            if (!isWater(vox(x, y, z))) continue;
            water++;
            const int nb[5][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}};
            for (const auto& d : nb) {
              if (vox(x + d[0], y + d[1], z + d[2]) != 0u) continue;
              if (!open++) {
                const worldmap::WaterColumn b = worldmap::WaterColumnAt(x + d[0], z + d[2], kDefaultSeed);
                why = Format(" first: water (%d,%d,%d) has air at (%d,%d,%d); mirror there ground %d water %d, next ground %d "
                             "water %d",
                             x, y, z, x + d[0], y + d[1], z + d[2], wc.h, wc.water, b.h, b.water);
              }
              break;
            }
          }
        }
      const bool eOk = water > 20000 && open == 0;
      std::printf("rivers: E contained: %s (%d water voxels over the window, %d with air beside or under)%s\n",
                  eOk ? "PASS" : "FAIL", water, open, why.c_str());
      if (!eOk) problems.push_back(Format("E: %d of %d water voxels touch air%s", open, water, why.c_str()));
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
  detail = "spline, load, bake, lake, CPU == GPU, contained, far + horizon, sleep, site index";
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
