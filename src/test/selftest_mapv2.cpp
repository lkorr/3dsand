// selftest_mapv2.cpp -- `worldmap-v2`: the map v2 format and the authored
// height plane (docs/PLAN_map_v2.md F1).
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
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/biomes.h"
#include "sim/treeatlas.h"
#include "sim/world.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"

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
    if (i >= 20 && i < 40 && k >= 20 && k < 40) return 14000 - 300 * (std::abs(i - 30) + std::abs(k - 30));
    if (i >= 60 && i < 70 && k >= 20 && k < 30) return -8000 + 100 * (std::abs(i - 65) + std::abs(k - 25));
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
      if (i < 14 && k < 14) continue;   // the forest corner: forest rolls ponds
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

}  // namespace

const std::vector<Gate>& MapV2Gates() {
  static const std::vector<Gate> g = {
      {"worldmap-v2", "sim", {}, false, GateWorldMapV2},
  };
  return g;
}

}  // namespace selftest
