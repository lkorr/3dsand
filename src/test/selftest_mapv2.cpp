// selftest_mapv2.cpp -- `worldmap-v2`: the map v2 format and the authored
// height plane (docs/PLAN_map_v2.md F1); `roads`: map.json roads[] in the
// engine (R1), below.
//
// WHAT THIS GUARDS. Map v2 moved the world's shape from a seeded octave under a
// 102 m u8 landform plane to an AUTHORED absolute height plane (6.4 m posts,
// i16 voxels, Catmull-Rom) with the noise as texture on top. The promise to
// the author is "the summit I painted at 1,100 m is 1,100 m", and the promise
// to the engine is that the CPU twin and the GPU still agree to the voxel. A
// synthetic map is written to a TEMP asset directory (map.json + tiles), so
// nothing here depends on, or touches, the shipped maps:
//
//   A. FORMAT. The tiles load; an ABSENT tile is the plane's default (height
//      and biome); a tile whose header disagrees with its name is refused; a
//      v1 map.json is refused with a message naming the converter.
//   B. POST EXACTNESS, zero noise. With every terrain amplitude 0 and no
//      sediment, World::TerrainHeight at a post IS the post value -- across a
//      +14,000 voxel peak and a -8,000 pit (the i16 range the bounds in
//      worldgen.wgsl crStep are argued for).
//   C. CPU == GPU, noise on, per voxel (the `terrain` gate's C1 claim: the
//      mirror's ground exists in the world and nothing bulk buries it) over a
//      box that crosses post edges on both axes and a ~2 voxel/voxel ramp.
//   D. C1 CONTINUITY. The authored gradient does not jump at post edges (no
//      larger change across an edge than inside a cell, + the floors), and the
//      noise-on ground does not step at them (no step across an edge larger
//      than the steepest step inside a cell + 1 voxel).
//   E. THE EDGE FADE. Past the extent the authored height eases to the
//      default: exactly the default beyond edgeFadeVox, and no step at the
//      extent's edge larger than the fade's own slope allows.
//
// Leaves the pristine world behind: the real environment reloaded from disk,
// the world regenerated and the far cascades refilled (the `sculpt` gate's
// exit), and the temp directory deleted.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/biomes.h"
#include "sim/treeatlas.h"
#include "sim/world.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

namespace selftest {
namespace {

namespace fs = std::filesystem;
using sandvox::kDefaultSeed;

// One SVT1 tile (worldmap.h kTileMagic): 256 x 256 samples, z-major.
bool WriteTile(const fs::path& p, uint32_t kind, int tx, int tz, const std::vector<int>& samples) {
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

Status GateWorldMapV2(Ctx& c, std::string& detail) {
  using sandvox::ReadVoxelsSync;
  using sandvox::ReloadEnvironment;
  using sandvox::SubmitWorldgen;
  const std::string dir = sandvox::AssetDir();
  std::vector<std::string> problems;
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("worldmap-v2: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  };
  biomes::BiomeSet set;
  std::string log;
  if (!biomes::LoadBiomeSet(dir, c.mats, set, log)) return fail("biome files did not load: " + log);
  TreeAtlas atlas;
  if (!LoadTreeAtlas(dir + "/trees", c.mats, set, atlas, log)) return fail("tree atlas did not load: " + log);
  const worldmap::WorldMapData real = worldmap::CurrentWorldMap();   // a COPY
  if (!real.Loaded()) return fail("no world map loaded");

  // ---- the synthetic map: a temp asset dir, one height + one biome tile ------
  const fs::path tmp = fs::temp_directory_path() /
                       ("sv_mapv2_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path mdir = tmp / "worldmap" / "syn";
  const IVec3 org = c.world.WindowOrigin();
  const int ox = org.x * static_cast<int>(kChunk), oz = org.z * static_cast<int>(kChunk);
  const int ylo = org.y * static_cast<int>(kChunk);
  constexpr int L = 6, S = 1 << L, T = worldmap::kTileSide;
  // The extent: 32 x 1024 voxels a side, its min a site-cell corner 8 km
  // west/south of the window, so the window sits inside tile (0, 0)'s posts
  // and tiles (1..2, *) are absent.
  const int X0 = ((ox - 8192) >> 10) << 10, Z0 = ((oz - 8192) >> 10) << 10;
  const int W = 32768;
  const int kDefault = -300;
  // The authored surface, at post (i, k) of tile (0, 0):
  //   * near the window, a RAMP in x: G + 220 * smoothstep over 160 voxels
  //     (steepest ~2.1 voxels per voxel), plus a gentle z swell -- all inside
  //     the window's 512 voxels of height;
  //   * far from it, a 14,000-voxel PEAK and an -8,000-voxel PIT (posts only:
  //     the CPU half reads them, no chunk is resident there).
  const int G = ylo + 180, rampX = ox + 160, rampW = 160;
  auto authored = [&](int i, int k) -> int {
    const double px = X0 + i * S, pz = Z0 + k * S;
    if (i >= 60 && i < 80 && k >= 60 && k < 80) return 14000 - 300 * (std::abs(i - 70) + std::abs(k - 70));
    if (i >= 100 && i < 110 && k >= 60 && k < 70) return -8000 + 100 * (std::abs(i - 105) + std::abs(k - 65));
    double t = std::clamp((px - rampX) / rampW, 0.0, 1.0);
    t = t * t * (3 - 2 * t);
    return static_cast<int>(std::lround(G + 220 * t + 24 * std::sin(pz / 300.0)));
  };
  std::vector<int> hs(size_t(T) * T), bs(size_t(T) * T, 0);
  for (int k = 0; k < T; k++)
    for (int i = 0; i < T; i++) {
      hs[size_t(k) * T + i] = authored(i, k);
      if (i < 10 && k < 10) bs[size_t(k) * T + i] = 1;   // a forest corner, far from the window
    }
  nlohmann::json mj = {
      {"version", 2}, {"name", "syn"},
      {"extent", {{"min", {X0, Z0}}, {"size", {W, W}}}},
      {"siteCellLog2", 10},
      {"planes", {{"height", {{"log2", L}, {"tile", T}, {"default", kDefault}}},
                  {"biome", {{"log2", 8}, {"tile", T}, {"default", "ocean"}}}}},
      {"seaLevelY", -20000}, {"edgeFadeVox", 2048}, {"warpAmpVox", 0},
      {"biomes", {"ocean", "forest"}},
      // The spawn on the GPU box: its calm area fades the range rung there, so
      // the noise-on ground stays inside the window's 512 voxels of height.
      {"sites", {{{"id", "spawn"}, {"kind", "spawn"}, {"at", {rampX, oz + 256}}}}},
      {"rules", nlohmann::json::array()}};
  // Noise ON: the loaded map's own terrain, the same numbers the real world
  // uses; noise OFF: every amplitude 0 and no sediment wedge.
  nlohmann::json te = nlohmann::json::object();
  {
    std::ifstream f(dir + "/worldmap/" + real.name + "/map.json");
    nlohmann::json rj;
    try { f >> rj; } catch (...) {}
    if (rj.contains("terrain")) te = rj["terrain"];
  }
  nlohmann::json mjOn = mj, mjOff = mj;
  mjOn["terrain"] = te;
  nlohmann::json teOff = te;
  for (const char* k : {"rangeAmplitude", "hillAmplitude", "detailAmplitude", "grainAmplitude", "sedMax", "sedTopsoil"})
    teOff[k] = 0;
  mjOff["terrain"] = teOff;
  bool wrote = WriteTile(mdir / "planes" / "height" / "0_0.svt", worldmap::kPlaneHeight, 0, 0, hs) &&
               WriteTile(mdir / "planes" / "biome" / "0_0.svt", worldmap::kPlaneBiome, 0, 0, bs);
  {
    std::ofstream f(mdir / "map.json");
    f << mjOn.dump(2);
    wrote = wrote && static_cast<bool>(f);
  }
  if (!wrote) return fail("could not write the synthetic map under " + tmp.string());
  // The loader would read <tmp>/worldmap/syn/refs; the override keeps the
  // load to the files above (and is put back as it was).
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

  // ---- A. format ----------------------------------------------------------------
  worldmap::WorldMapData on, off;
  std::string lon, loff;
  const bool loadedOn = load(nullptr, on, lon);
  const bool loadedOff = load(&mjOff, off, loff);
  if (!loadedOn || !loadedOff) {
    restoreOverride();
    fs::remove_all(tmp);
    return fail("A: the synthetic v2 map was REFUSED: " + lon + loff);
  }
  int oceanId = -1, forestId = -1;
  for (const biomes::BiomeDef& b : set.biomes) {
    if (b.name == "ocean") oceanId = b.index;
    if (b.name == "forest") forestId = b.index;
  }
  const bool absentH = on.HeightPost(300, 300) == kDefault && on.HeightPost(2 * T + 3, 5) == kDefault;
  const bool presentH = on.HeightPost(30, 30) == hs[size_t(30) * T + 30] && on.HeightPost(5, 7) == hs[size_t(7) * T + 5];
  const bool absentB = on.BiomePost(300, 300) == oceanId && on.BiomePost(5, T + 2) == oceanId;
  const bool presentB = on.BiomePost(3, 4) == forestId && on.BiomePost(40, 40) == oceanId;
  const bool tilesCounted = on.heightTiles == 1 && on.biomeTiles == 1;
  // a tile whose header names another tile
  bool badRefused = false;
  {
    std::vector<int> z(size_t(T) * T, kDefault);
    WriteTile(mdir / "planes" / "height" / "1_1.svt", worldmap::kPlaneHeight, 1, 2, z);
    worldmap::WorldMapData bad;
    std::string lb;
    badRefused = !load(nullptr, bad, lb) && lb.find("1_1.svt") != std::string::npos;
    fs::remove(mdir / "planes" / "height" / "1_1.svt");
  }
  // a v1 map.json
  bool v1Refused = false;
  {
    nlohmann::json v1 = {{"name", "syn"}, {"cellLog2", 10}, {"size", {196, 196}}, {"originCell", {98, 98}},
                         {"biomes", {"ocean"}}, {"sites", nlohmann::json::array()}};
    worldmap::WorldMapData m1;
    std::string l1;
    v1Refused = !load(&v1, m1, l1) && l1.find("convert_map_v2.py") != std::string::npos;
  }
  const bool aOk = absentH && presentH && absentB && presentB && tilesCounted && badRefused && v1Refused;
  std::printf("worldmap-v2: A format: %s (absent tile = default: height %d biome %d; present tile read: height %d biome %d; "
              "%d/%d tiles counted; mislabelled tile refused %d; v1 map refused naming the converter %d)\n",
              aOk ? "PASS" : "FAIL", absentH, absentB, presentH, presentB, on.heightTiles, on.biomeTiles, badRefused, v1Refused);
  if (!aOk) problems.push_back("A: the tile format");

  // ---- B. post exactness, zero noise ---------------------------------------------
  worldmap::SetCurrentWorldMap(off);
  int bPosts = 0, bExact = 0, bMin = 1 << 30, bMax = -(1 << 30);
  std::string bWhy;
  for (int k = 1; k < T - 2; k += 3)
    for (int i = 1; i < T - 2; i += 3) {
      // The forest corner (biome cells 0..9 = height posts 0..39, + the
      // blend): forest rolls ponds, and a bowl is not the authored height.
      if (i < 44 && k < 44) continue;
      const int x = X0 + i * S, z = Z0 + k * S;
      const int want = hs[size_t(k) * T + i];
      const int got = World::TerrainHeight(x, z, kDefaultSeed);
      bPosts++;
      bMin = std::min(bMin, want);
      bMax = std::max(bMax, want);
      if (got == want && worldmap::AuthoredHeightAt(x, z) == want) bExact++;
      else if (bWhy.empty()) bWhy = Format(" first miss post (%d,%d) at (%d,%d): authored %d, TerrainHeight %d", i, k, x, z, want, got);
    }
  const bool bOk = bPosts > 5000 && bExact == bPosts && bMax >= 13000 && bMin <= -7000;
  std::printf("worldmap-v2: B post exactness (noise 0): %s (%d / %d posts exact, y%d..y%d)%s\n", bOk ? "PASS" : "FAIL",
              bExact, bPosts, bMin, bMax, bWhy.c_str());
  if (!bOk) problems.push_back(Format("B: %d of %d posts are not the authored height%s", bPosts - bExact, bPosts, bWhy.c_str()));

  // ---- D. continuity (CPU, noise on) ---------------------------------------------
  worldmap::SetCurrentWorldMap(on);
  {
    // Along x through the ramp and along z through the swell: the change of the
    // authored gradient between neighbouring columns, at post edges vs inside
    // cells; and the noise-on ground's step, the same split.
    int gEdge = 0, gIn = 0, hEdge = 0, hIn = 0, edges = 0;
    auto scan = [&](bool alongX) {
      const int fixed = alongX ? oz + 256 : rampX + 40;
      const int a0 = alongX ? ox + 8 : oz + 8;
      for (int t = a0; t < a0 + 480; t++) {
        const int x0 = alongX ? t : fixed, z0 = alongX ? fixed : t;
        const int x1 = alongX ? t + 1 : fixed, z1 = alongX ? fixed : t + 1;
        int gx0, gz0, gx1, gz1;
        worldmap::AuthoredHeightAt(x0, z0, &gx0, &gz0);
        worldmap::AuthoredHeightAt(x1, z1, &gx1, &gz1);
        const int dg = std::abs(alongX ? gx1 - gx0 : gz1 - gz0);
        const int dh = std::abs(World::TerrainHeight(x1, z1, kDefaultSeed) - World::TerrainHeight(x0, z0, kDefaultSeed));
        const bool edge = (((alongX ? x1 - X0 : z1 - Z0) & (S - 1)) == 0);
        if (edge) { edges++; gEdge = std::max(gEdge, dg); hEdge = std::max(hEdge, dh); }
        else { gIn = std::max(gIn, dg); hIn = std::max(hIn, dh); }
      }
    };
    scan(true);
    scan(false);
    const bool dOk = edges >= 12 && gEdge <= gIn + 16 && hEdge <= hIn + 1;
    std::printf("worldmap-v2: D continuity: %s (%d post edges crossed; authored gradient change, Q8: <= %d across an edge, "
                "<= %d inside a cell; noise-on ground step: <= %d across an edge, <= %d inside a cell)\n",
                dOk ? "PASS" : "FAIL", edges, gEdge, gIn, hEdge, hIn);
    if (!dOk) problems.push_back(Format("D: post edges step (gradient %d vs %d, ground %d vs %d)", gEdge, gIn, hEdge, hIn));
  }

  // ---- E. the edge fade -------------------------------------------------------------
  {
    const int zf = Z0 + 5000;
    const int fade = on.edgeFadeVox;
    const int far = worldmap::AuthoredHeightAt(X0 - fade - 1, zf), farer = worldmap::AuthoredHeightAt(X0 - 3 * fade, zf);
    const int edgeH = worldmap::AuthoredHeightAt(X0, zf);
    int worst = 0;
    for (int x = X0 - fade - 2; x < X0 + 8; x++)
      worst = std::max(worst, std::abs(worldmap::AuthoredHeightAt(x + 1, zf) - worldmap::AuthoredHeightAt(x, zf)));
    // the smoothstep's steepest slope is 1.5 * |default - edge| / fade, plus the floors
    const int allow = (3 * std::abs(kDefault - edgeH)) / (2 * fade) + 2;
    const bool eOk = far == kDefault && farer == kDefault && worst <= allow;
    std::printf("worldmap-v2: E edge fade: %s (edge y%d -> default y%d over %d voxels; worst column step %d, allowed %d)\n",
                eOk ? "PASS" : "FAIL", edgeH, far, fade, worst, allow);
    if (!eOk) problems.push_back(Format("E: the edge fade (far %d, step %d > %d)", far, worst, allow));
  }

  // ---- C. CPU == GPU, per voxel, noise on ----------------------------------------
  std::vector<uint32_t> words;
  bool gpuState = false;
  if (!worldmap::PackWorldMap(set, on, words, log)) {
    problems.push_back("C: the synthetic map did not pack: " + log);
  } else {
    c.ctx.WaitIdle();
    c.sim.UploadEnvironment(c.ctx.device, c.ctx.queue, atlas, words);
    gpuState = true;
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    static const char* kBody[] = {"stone", "dirt", "sand", "grass", "snow", "mud", "gravel", "sandstone"};
    static const char* kBulk[] = {"stone", "dirt", "mud", "gravel"};
    std::vector<uint8_t> body(c.mats.size(), 0), bulk(c.mats.size(), 0);
    for (size_t i = 0; i < c.mats.size(); i++) {
      for (const char* n : kBody) if (c.mats[i].name == n) body[i] = 1;
      for (const char* n : kBulk) if (c.mats[i].name == n) bulk[i] = 1;
    }
    // The box: across the ramp in x (and several post edges), 64 rows in z.
    const int xa = rampX - 96, xb = rampX + rampW + 64, za = oz + 224, zb = oz + 287;
    const int nx = xb - xa + 1, nz = zb - za + 1;
    std::vector<int> h(size_t(nx) * nz);
    int hmin = 1 << 30, hmax = -(1 << 30), steep = 0;
    for (int z = za; z <= zb; z++)
      for (int x = xa; x <= xb; x++) {
        const int v = World::TerrainHeight(x, z, kDefaultSeed);
        h[size_t(z - za) * nx + (x - xa)] = v;
        hmin = std::min(hmin, v);
        hmax = std::max(hmax, v);
        int gx = 0;
        worldmap::AuthoredHeightAt(x, z, &gx);
        steep = std::max(steep, std::abs(gx));
      }
    const int cyLo = std::max(org.y, (hmin - 2) >> 4);
    const int cyHi = std::min(org.y + static_cast<int>(kNChunk) - 1, (hmax + 6) >> 4);
    const int m = static_cast<int>(kNChunk) - 1;
    std::vector<uint32_t> run(size_t(kNChunk) * kChunkVol);
    std::vector<uint8_t> atH(h.size(), 0), atB(h.size(), 0), atA(h.size(), 0);
    for (int cz = za >> 4; cz <= zb >> 4; cz++)
      for (int cy = cyLo; cy <= cyHi; cy++) {
        ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({0, cy, cz}), kNChunk, run.data(), "worldmap-v2");
        for (int lz = 0; lz < static_cast<int>(kChunk); lz++) {
          const int z = (cz << 4) + lz;
          if (z < za || z > zb) continue;
          for (int x = xa; x <= xb; x++) {
            const size_t ci = size_t(z - za) * nx + (x - xa);
            const int hh = h[ci];
            for (int ly = 0; ly < static_cast<int>(kChunk); ly++) {
              const int y = (cy << 4) + ly;
              const uint32_t local = (uint32_t(lz) * kChunk + uint32_t(ly)) * kChunk + uint32_t(x & 15);
              const uint32_t mat = run[size_t((x >> 4) & m) * kChunkVol + local] & 0xFFFu;
              if (mat == 0 || mat >= c.mats.size()) continue;
              if (y == hh && body[mat]) atH[ci] = 1;
              if (y == hh - 1 && body[mat]) atB[ci] = 1;
              if (y >= hh + 2 && bulk[mat]) atA[ci] = 1;
            }
          }
        }
      }
    int cols = 0, hollow = 0, buried = 0;
    std::string why;
    for (int z = za; z <= zb; z++)
      for (int x = xa; x <= xb; x++) {
        const size_t ci = size_t(z - za) * nx + (x - xa);
        const int hh = h[ci];
        if ((hh - 1) >> 4 < cyLo || (hh + 2) >> 4 > cyHi) continue;
        cols++;
        if (!atH[ci] || !atB[ci]) {
          if (!hollow++ && why.empty())
            why = Format("(%d,%d): mirror says ground at y%d, the world has %s there and %s below", x, z, hh,
                         atH[ci] ? "terrain" : "none", atB[ci] ? "terrain" : "none");
        }
        if (atA[ci]) {
          if (!buried++ && why.empty()) why = Format("(%d,%d): bulk terrain at y>=%d over the mirror's y%d", x, z, hh + 2, hh);
        }
      }
    const bool cOk = cols >= nx * nz * 9 / 10 && hollow == 0 && buried == 0 && steep >= 384;
    std::printf("worldmap-v2: C GPU vs twin: %s (%d columns x%d..%d z%d..%d, y%d..y%d, steepest authored slope %.2f vox/vox; "
                "%d hollow, %d buried)%s%s\n",
                cOk ? "PASS" : "FAIL", cols, xa, xb, za, zb, hmin, hmax, steep / 256.0, hollow, buried,
                why.empty() ? "" : " ", why.c_str());
    if (!cOk) problems.push_back(Format("C: %d hollow / %d buried of %d columns%s%s", hollow, buried, cols,
                                        why.empty() ? "" : ": ", why.c_str()));
  }

  // ---- the pristine world back ------------------------------------------------------
  restoreOverride();
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
  std::error_code ec;
  fs::remove_all(tmp, ec);
  if (!problems.empty()) {
    detail = problems[0];
    for (size_t i = 1; i < problems.size(); i++) detail += "; " + problems[i];
    std::printf("worldmap-v2: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  detail = "format, post exactness, CPU == GPU, continuity, edge fade";
  std::printf("worldmap-v2: PASS\n");
  return Status::Pass;
}

// ---- roads (map v2 R1, docs/PLAN_map_v2.md R1 item 8) -----------------------------
//
// WHAT THIS GUARDS. A map.json `roads[]` entry is baked at load into a graded
// profile and cut into kSiteRoad pieces; the height mirror levels the ground
// to it (roadPadAt), genCellIn lays its SOLID surface, the keep-outs keep the
// forest and the cover off it, and the far field tints the cells it crosses.
// A synthetic forest map (TEMP asset dir, the worldmap-v2 seam) with a 44-voxel
// ridge and a 14-voxel dip under three roads -- a cobbled road at 8 %, a 1.2 m
// packed-earth path, a bent gravel track -- and:
//   A. LOAD: the roads load and cut into pieces; a powder surface and an
//      unknown kind are refused naming the road; the baked grade <= maxGrade.
//   B. GRADE ON THE GROUND (CPU): along every centreline the ground climbs no
//      faster than maxGrade (over 16 voxels, + the rounding), and the ridge is
//      CUT and the dip FILLED (so A and B are not vacuous).
//   C. SURFACE (GPU): the top `depth` cells on every centreline column are the
//      road's material.
//   D. KEEP-OUTS (GPU): nothing -- no trunk, crown, cover row or tile plant --
//      stands over the carriageway (half-width - 1), while the forest stands
//      further out (the check is not vacuous).
//   E. CPU == GPU over the cut, the shoulders included (the worldmap-v2 C test).
//   F. FAR: the level-4 cascade cells along the 1.2 m path take its surface;
//      cells 64 voxels off it do not.
//   G. SLEEP: the road world settles to <= 32 active chunks.
//   H. THE SITE INDEX on `default`: the worst per-cell site list with the
//      shipped roads (Harrowby's paths) and with a test network through the
//      village, against kSiteCellMax -- a number, reported.
// Leaves the pristine world behind (the worldmap-v2 exit).

// The distance from a column to a polyline, voxels (a test measure only).
double PolyDist(const std::vector<std::pair<int, int>>& pts, double x, double z) {
  double best = 1e30;
  for (size_t i = 0; i + 1 < pts.size(); i++) {
    const double ax = pts[i].first, az = pts[i].second, bx = pts[i + 1].first, bz = pts[i + 1].second;
    const double vx = bx - ax, vz = bz - az, l2 = vx * vx + vz * vz;
    double t = l2 > 0 ? ((x - ax) * vx + (z - az) * vz) / l2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double dx = x - (ax + t * vx), dz = z - (az + t * vz);
    best = std::min(best, std::sqrt(dx * dx + dz * dz));
  }
  return best;
}

Status GateRoads(Ctx& c, std::string& detail) {
  using sandvox::ReadVoxelsSync;
  using sandvox::ReloadEnvironment;
  using sandvox::SubmitWorldgen;
  const std::string dir = sandvox::AssetDir();
  std::vector<std::string> problems;
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("roads: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  };
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

  // ---- the synthetic map ------------------------------------------------------------
  const fs::path tmp = fs::temp_directory_path() /
                       ("sv_roads_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path mdir = tmp / "worldmap" / "syn";
  const IVec3 org = c.world.WindowOrigin();
  const int ox = org.x * static_cast<int>(kChunk), oz = org.z * static_cast<int>(kChunk);
  const int ylo = org.y * static_cast<int>(kChunk);
  constexpr int L = 6, S = 1 << L, T = worldmap::kTileSide;
  const int X0 = ((ox - 8192) >> 10) << 10, Z0 = ((oz - 8192) >> 10) << 10;
  const int G = ylo + 150;
  const int ridgeX = ox + 340, dipX = ox + 150;
  auto authored = [&](int i, int /*k*/) -> int {
    const double px = X0 + i * S;
    double t = std::max(0.0, 1.0 - std::abs(px - ridgeX) / 90.0);
    double u = std::max(0.0, 1.0 - std::abs(px - dipX) / 64.0);
    t = t * t * (3 - 2 * t);
    u = u * u * (3 - 2 * u);
    return static_cast<int>(std::lround(G + 44 * t - 14 * u));
  };
  std::vector<int> hs(size_t(T) * T), bs(size_t(T) * T, 1);
  for (int k = 0; k < T; k++)
    for (int i = 0; i < T; i++) hs[size_t(k) * T + i] = authored(i, k);
  using P = std::vector<std::pair<int, int>>;
  struct RoadCase { const char* id; const char* kind; const char* surface; int width, shoulder, grade, depth; P pts; };
  const std::vector<RoadCase> cases = {
      {"r1_road", "road", "cobble", 30, 8, 8, 3, {{ox + 24, oz + 200}, {ox + 488, oz + 200}}},
      {"r2_path", "path", "packed_earth", 12, 4, 25, 2, {{ox + 24, oz + 330}, {ox + 488, oz + 330}}},
      {"r3_track", "track", "packed_gravel", 20, 8, 15, 2, {{ox + 60, oz + 40}, {ox + 220, oz + 110}, {ox + 420, oz + 60}}},
  };
  nlohmann::json roads = nlohmann::json::array();
  for (const RoadCase& rc : cases) {
    nlohmann::json pts = nlohmann::json::array();
    for (const auto& [x, z] : rc.pts) pts.push_back({x, z});
    roads.push_back({{"id", rc.id}, {"kind", rc.kind}, {"surface", rc.surface}, {"width", rc.width},
                     {"shoulder", rc.shoulder}, {"maxGrade", rc.grade}, {"depth", rc.depth}, {"points", pts}});
  }
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
      {"seaLevelY", -20000}, {"edgeFadeVox", 2048}, {"warpAmpVox", 0},
      {"biomes", {"ocean", "forest"}},
      {"terrain", te},
      {"sites", {{{"id", "spawn"}, {"kind", "spawn"}, {"at", {ox + 256, oz + 450}}}}},
      {"rules", nlohmann::json::array()},
      {"roads", roads}};
  bool wrote = WriteTile(mdir / "planes" / "height" / "0_0.svt", worldmap::kPlaneHeight, 0, 0, hs) &&
               WriteTile(mdir / "planes" / "biome" / "0_0.svt", worldmap::kPlaneBiome, 0, 0, bs);
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

  // ---- A. load ------------------------------------------------------------------------
  worldmap::WorldMapData on;
  std::string lon;
  if (!load(nullptr, on, lon)) {
    restoreOverride();
    fs::remove_all(tmp);
    return fail("A: the synthetic road map was REFUSED: " + lon);
  }
  bool powderRefused = false, kindRefused = false;
  {
    nlohmann::json bad = mj;
    bad["roads"][0]["surface"] = "gravel";
    worldmap::WorldMapData m;
    std::string l;
    powderRefused = !load(&bad, m, l) && l.find("r1_road") != std::string::npos && l.find("not a solid") != std::string::npos;
    nlohmann::json bad2 = mj;
    bad2["roads"][1]["kind"] = "motorway";
    std::string l2;
    kindRefused = !load(&bad2, m, l2) && l2.find("r2_path") != std::string::npos;
  }
  int nPieces = 0, gradeBad = 0;
  std::string gradeWhy;
  for (size_t r = 0; r < on.roads.size(); r++) {
    const worldmap::WorldMapData::Road& rd = on.roads[r];
    nPieces += rd.pieces;
    if (rd.worstGradeQ8 > rd.maxGrade * 256 + 64) {
      gradeBad++;
      gradeWhy = Format(" %s baked %.2f %% > %d %%", rd.id.c_str(), rd.worstGradeQ8 / 256.0, rd.maxGrade);
    }
  }
  const bool aOk = on.roads.size() == cases.size() && nPieces >= 4 && powderRefused && kindRefused && gradeBad == 0;
  std::printf("roads: A load: %s (%zu roads -> %d pieces; powder surface refused %d, unknown kind refused %d; "
              "baked grades %.2f / %.2f / %.2f %%%s)\n",
              aOk ? "PASS" : "FAIL", on.roads.size(), nPieces, powderRefused, kindRefused,
              on.roads.size() > 0 ? on.roads[0].worstGradeQ8 / 256.0 : 0.0,
              on.roads.size() > 1 ? on.roads[1].worstGradeQ8 / 256.0 : 0.0,
              on.roads.size() > 2 ? on.roads[2].worstGradeQ8 / 256.0 : 0.0, gradeWhy.c_str());
  if (!aOk) problems.push_back("A: load / validation / baked grade" + gradeWhy);

  // ---- B. the grade on the ground (CPU) ---------------------------------------------------
  worldmap::SetCurrentWorldMap(on);
  // Every centreline as 1-voxel steps (positions rounded), with the ground there.
  struct Walk { std::vector<int> x, z, h; };
  std::vector<Walk> walks(cases.size());
  int cut = 0, fill = 0, steep = 0;
  std::string steepWhy;
  for (size_t r = 0; r < cases.size(); r++) {
    const P& pts = cases[r].pts;
    Walk& w = walks[r];
    for (size_t i = 0; i + 1 < pts.size(); i++) {
      const double ax = pts[i].first, az = pts[i].second, bx = pts[i + 1].first, bz = pts[i + 1].second;
      const int n = static_cast<int>(std::ceil(std::hypot(bx - ax, bz - az)));
      for (int k = (i == 0 ? 0 : 1); k <= n; k++) {
        const int x = static_cast<int>(std::lround(ax + (bx - ax) * k / n));
        const int z = static_cast<int>(std::lround(az + (bz - az) * k / n));
        w.x.push_back(x); w.z.push_back(z);
        w.h.push_back(World::TerrainHeight(x, z, kDefaultSeed));
        if (r == 0) {
          const int bare = worldmap::BareGroundHeight(x, z, kDefaultSeed);
          cut = std::max(cut, bare - w.h.back());
          fill = std::max(fill, w.h.back() - bare);
        }
      }
    }
    const int allow = (cases[r].grade * 16 + 99) / 100 + 2;
    for (size_t i = 0; i + 16 < w.h.size(); i++)
      if (std::abs(w.h[i + 16] - w.h[i]) > allow) {
        if (!steep++)
          steepWhy = Format(" first: %s (%d,%d) y%d -> (%d,%d) y%d over 16 voxels, allowed %d", cases[r].id, w.x[i], w.z[i],
                            w.h[i], w.x[i + 16], w.z[i + 16], w.h[i + 16], allow);
      }
  }
  const bool bOk = steep == 0 && cut >= 10 && fill >= 3;
  std::printf("roads: B grade on the ground: %s (%d over-steep 16-voxel spans; the 8 %% road cuts the ridge %d voxels deep, "
              "fills the dip %d)%s\n",
              bOk ? "PASS" : "FAIL", steep, cut, fill, steepWhy.c_str());
  if (!bOk) problems.push_back(Format("B: %d over-steep spans, cut %d, fill %d%s", steep, cut, fill, steepWhy.c_str()));

  // ---- the world on the GPU ------------------------------------------------------------------
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
    // One row of chunks (all x) per (cy, cz), read once.
    std::map<std::pair<int, int>, std::vector<uint32_t>> rows;
    const int m = static_cast<int>(kNChunk) - 1;
    auto vox = [&](int x, int y, int z) -> uint32_t {
      const int cy = y >> 4, cz = z >> 4;
      if (cy < org.y || cy >= org.y + static_cast<int>(kNChunk)) return 0u;
      auto it = rows.find({cy, cz});
      if (it == rows.end()) {
        std::vector<uint32_t> run(size_t(kNChunk) * kChunkVol);
        ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({0, cy, cz}), kNChunk, run.data(), "roads");
        it = rows.emplace(std::make_pair(cy, cz), std::move(run)).first;
      }
      const uint32_t local = (uint32_t(z & 15) * kChunk + uint32_t(y & 15)) * kChunk + uint32_t(x & 15);
      return it->second[size_t((x >> 4) & m) * kChunkVol + local] & 0xFFFu;
    };
    auto nameOf = [&](uint32_t id) { return id < c.mats.size() ? c.mats[id].name : std::string("?"); };

    // ---- C. the surface ----
    int cCols = 0, cBad = 0;
    std::string cWhy;
    for (size_t r = 0; r < cases.size(); r++) {
      const uint32_t want = matIdOf(cases[r].surface);
      const Walk& w = walks[r];
      for (size_t i = 0; i < w.h.size(); i += 4) {
        cCols++;
        for (int d = 0; d < cases[r].depth; d++) {
          const uint32_t got = vox(w.x[i], w.h[i] - d, w.z[i]);
          if (got != want) {
            if (!cBad++)
              cWhy = Format(" first: %s (%d,%d) y%d is %s, not %s", cases[r].id, w.x[i], w.z[i], w.h[i] - d,
                            nameOf(got).c_str(), cases[r].surface);
            break;
          }
        }
      }
    }
    const bool cOk = cCols > 200 && cBad == 0;
    std::printf("roads: C surface: %s (%d centreline columns, %d without the road's top cells)%s\n", cOk ? "PASS" : "FAIL",
                cCols, cBad, cWhy.c_str());
    if (!cOk) problems.push_back(Format("C: %d of %d centreline columns lack the surface%s", cBad, cCols, cWhy.c_str()));

    // ---- D. keep-outs ----
    int dCols = 0, dBad = 0, forest = 0;
    std::string dWhy;
    for (size_t r = 0; r < cases.size(); r++) {
      const double hw = cases[r].width / 2;
      int x0 = 1 << 30, x1 = -(1 << 30), z0 = 1 << 30, z1 = -(1 << 30);
      for (const auto& [x, z] : cases[r].pts) { x0 = std::min(x0, x); x1 = std::max(x1, x); z0 = std::min(z0, z); z1 = std::max(z1, z); }
      const int reach = static_cast<int>(hw) + 140;
      for (int z = std::max(z0 - reach, oz + 1); z <= std::min(z1 + reach, oz + static_cast<int>(kWorldN) - 2); z += 2)
        for (int x = std::max(x0 - reach, ox + 1); x <= std::min(x1 + reach, ox + static_cast<int>(kWorldN) - 2); x += 2) {
          const double d = PolyDist(cases[r].pts, x, z);
          const bool onRoad = d <= hw - 1;
          const bool band = d >= hw + 80 && d <= hw + 140;
          if (!onRoad && !band) continue;
          const int h = World::TerrainHeight(x, z, kDefaultSeed);
          if (onRoad) dCols++;
          for (int y = h + 1; y <= h + 40; y++) {
            const uint32_t got = vox(x, y, z);
            if (got == 0u) continue;
            if (onRoad) {
              if (!dBad++)
                dWhy = Format(" first: %s (%d,%d) y%d holds %s, %.1f voxels off the line (half-width %.0f)", cases[r].id,
                              x, z, y, nameOf(got).c_str(), d, hw);
            } else {
              forest++;
            }
            break;
          }
        }
    }
    const bool dOk = dCols > 500 && dBad == 0 && forest > 0;
    std::printf("roads: D keep-outs: %s (%d carriageway columns, %d with something standing on them; %d columns out past "
                "the shoulders carry plants or trees)%s\n",
                dOk ? "PASS" : "FAIL", dCols, dBad, forest, dWhy.c_str());
    if (!dOk) problems.push_back(Format("D: %d of %d carriageway columns hold matter (forest columns %d)%s", dBad, dCols, forest,
                                        dWhy.c_str()));

    // ---- E. CPU == GPU over the cut ----
    {
      static const char* kBody[] = {"stone", "dirt", "sand", "grass", "snow", "mud", "gravel", "sandstone",
                                    "cobble", "packed_earth", "packed_gravel"};
      static const char* kBulk[] = {"stone", "dirt", "mud", "gravel", "cobble", "packed_earth", "packed_gravel"};
      std::vector<uint8_t> body(c.mats.size(), 0), bulk(c.mats.size(), 0);
      for (size_t i = 0; i < c.mats.size(); i++) {
        for (const char* n : kBody) if (c.mats[i].name == n) body[i] = 1;
        for (const char* n : kBulk) if (c.mats[i].name == n) bulk[i] = 1;
      }
      int cols = 0, hollow = 0, buried = 0;
      std::string why;
      for (int z = oz + 200 - 60; z <= oz + 200 + 60; z++)
        for (int x = ridgeX - 150; x <= ridgeX + 150; x++) {
          const int h = World::TerrainHeight(x, z, kDefaultSeed);
          if ((h - 1) >> 4 < org.y || (h + 6) >> 4 >= org.y + static_cast<int>(kNChunk)) continue;
          cols++;
          if (!body[vox(x, h, z)] || !body[vox(x, h - 1, z)]) {
            if (!hollow++ && why.empty())
              why = Format(" (%d,%d): mirror says ground at y%d, the world has %s there and %s below", x, z, h,
                           nameOf(vox(x, h, z)).c_str(), nameOf(vox(x, h - 1, z)).c_str());
          }
          for (int y = h + 2; y <= h + 6; y++)
            if (bulk[vox(x, y, z)]) {
              if (!buried++ && why.empty()) why = Format(" (%d,%d): %s at y%d over the mirror's y%d", x, z,
                                                          nameOf(vox(x, y, z)).c_str(), y, h);
              break;
            }
        }
      const bool eOk = cols > 30000 && hollow == 0 && buried == 0;
      std::printf("roads: E GPU vs twin: %s (%d columns over the cut and its shoulders, %d hollow, %d buried)%s\n",
                  eOk ? "PASS" : "FAIL", cols, hollow, buried, why.c_str());
      if (!eOk) problems.push_back(Format("E: %d hollow / %d buried of %d columns%s", hollow, buried, cols, why.c_str()));
    }

    // ---- F. the far field over the 1.2 m path ----
    {
      const int zp = oz + 330;
      DrainFullRefill(c.ctx, c.world, c.sim, IVec3{(ox + 256) >> 4, G >> 4, zp >> 4});
      constexpr uint32_t kLevel = 4;
      const int sh = static_cast<int>(kLevel + kFarShiftBase), s = 1 << sh;
      const uint32_t pe = matIdOf("packed_earth");
      const uint32_t slot = (c.mats[pe].gpu.flags >> kMatFarPalShift) & kMatFarPalMask;
      int on4 = 0, tint = 0, off4 = 0, offTint = 0;
      std::string why;
      for (int cx = ((ox + 40) >> sh) + 1; cx < ((ox + 470) >> sh); cx++) {
        for (int pass = 0; pass < 2; pass++) {
          const int cz = ((zp + (pass ? 64 : 0)) >> sh);
          const int xc = (cx << sh) + s / 2, zc = (cz << sh) + s / 2;
          const int hc = World::TerrainHeight(xc, zc, kDefaultSeed);
          const int cy = (hc - s / 2) >> sh;
          const uint32_t b = FarCellSlot(FarVoxByte(c.ctx, c.world, kLevel, {cx, cy, cz}));
          if (pass == 0) {
            on4++;
            if (b == slot) tint++;
            else if (why.empty()) why = Format(" first untinted: cell (%d,%d,%d) slot %u, the path's %u", cx, cy, cz, b, slot);
          } else {
            off4++;
            if (b == slot) offTint++;
          }
        }
      }
      const bool fOk = on4 >= 20 && tint * 10 >= on4 * 9 && offTint == 0;
      std::printf("roads: F far field: %s (level %u, %d-voxel cells: %d / %d cells along the 12-voxel path take its surface; "
                  "%d / %d cells 64 voxels off it do)%s\n",
                  fOk ? "PASS" : "FAIL", kLevel, s, tint, on4, offTint, off4, fOk ? "" : why.c_str());
      if (!fOk) problems.push_back(Format("F: %d / %d far cells tinted, %d off-road%s", tint, on4, offTint, why.c_str()));
    }

    // ---- G. sleep ----
    {
      uint32_t t = 93000;
      sandvox::support::TickCursor tick{c, t, IVec3{(ox + 256) >> 4, G >> 4, (oz + 200) >> 4}};
      uint32_t active = kNumSlots;
      int ticks = 0;
      for (; ticks < 2000; ticks++) {
        tick();
        if (ticks >= 200 && ticks % 100 == 0) {
          c.ctx.WaitIdle();
          active = sandvox::ReadActiveChunksSync(c.ctx, c.world, c.sim);
          if (active <= 32) break;
        }
      }
      const bool gOk = active <= 32;
      std::printf("roads: G sleep: %s (%u active chunks after %d ticks)\n", gOk ? "PASS" : "FAIL", active, ticks);
      if (!gOk) problems.push_back(Format("G: %u chunks still awake after %d ticks", active, ticks));
    }
  }

  // ---- H. the site index on `default`, shipped and with a test network ------------------
  restoreOverride();
  {
    std::ifstream f(dir + "/worldmap/default/map.json");
    nlohmann::json dj;
    try { f >> dj; } catch (...) {}
    worldmap::WorldMapData d0, d1;
    std::string l0, l1;
    const std::string t0 = dj.dump();
    const bool ok0 = worldmap::LoadWorldMap(dir, "default", set, c.mats.size(), kDefaultSeed, d0, l0, &t0);
    if (!dj.contains("roads")) dj["roads"] = nlohmann::json::array();
    // A king's road east-west through the village, a north road from its
    // green to the home lake, a track south past the spawn: three roads
    // meeting in Harrowby's site cell, on top of its houses, routes, paths.
    dj["roads"].push_back({{"id", "test_kings_road"}, {"kind", "road"}, {"surface", "cobble"}, {"width", 40},
                           {"points", {{-3000, 3330}, {420, 3360}, {900, 3320}, {4000, 3520}}}});
    dj["roads"].push_back({{"id", "test_north_road"}, {"kind", "track"}, {"surface", "packed_gravel"},
                           {"points", {{600, 3200}, {650, 1600}, {1240, 1000}}}});
    dj["roads"].push_back({{"id", "test_south_track"}, {"kind", "track"}, {"surface", "packed_earth"},
                           {"points", {{580, 3500}, {600, 4200}, {560, 6000}}}});
    const std::string t1 = dj.dump();
    const bool ok1 = worldmap::LoadWorldMap(dir, "default", set, c.mats.size(), kDefaultSeed, d1, l1, &t1);
    int pieces1 = 0;
    for (const auto& r : d1.roads) pieces1 += r.pieces;
    const bool hOk = ok0 && ok1;
    std::printf("roads: H site index on default: %s (shipped: %zu roads, worst cell %d sites; with a 3-road test network "
                "through Harrowby (%d pieces in all): worst cell %d; the cap is %d)%s%s\n",
                hOk ? "PASS" : "FAIL", d0.roads.size(), d0.siteCellWorst, pieces1, d1.siteCellWorst,
                static_cast<int>(worldmap::kSiteCellMax), ok0 ? "" : (" shipped refused: " + l0).c_str(),
                ok1 ? "" : (" network refused: " + l1).c_str());
    if (!hOk) problems.push_back("H: default " + std::string(ok0 ? "" : "REFUSED: " + l0) + (ok1 ? "" : " with the network REFUSED: " + l1));
  }

  // ---- the pristine world back ------------------------------------------------------
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
  std::error_code ec;
  fs::remove_all(tmp, ec);
  if (!problems.empty()) {
    detail = problems[0];
    for (size_t i = 1; i < problems.size(); i++) detail += "; " + problems[i];
    std::printf("roads: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  detail = "load, grade, surface, keep-outs, CPU == GPU, far tint, sleep, site index";
  std::printf("roads: PASS\n");
  return Status::Pass;
}

}  // namespace

const std::vector<Gate>& MapV2Gates() {
  static const std::vector<Gate> g = {
      {"worldmap-v2", "sim", {}, false, GateWorldMapV2},
      {"roads", "sim", {}, false, GateRoads},
  };
  return g;
}

}  // namespace selftest
