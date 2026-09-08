// selftest_envtruth.cpp — the number the Environment tab shows is the number
// the world has (docs/PLAN_environment_truth.md P-H).
//
// WHAT THIS GUARDS. The biome pages predict trees per hectare, "1 in N
// columns" of each cover row, and a ground skin. Until this gate nothing
// measured the GPU worldgen against those predictions: the page's tile was
// ignored for months (P-B §0.3) and it promised 153 trees/ha where the engine
// grew 23, and no gate could have said so. This one packs a SYNTHETIC one-
// biome map in memory for every biome file — flat landform, that biome on
// every cell, no sites, the calm home area centred on the residency window —
// uploads it through the same seam an F7 takes (Simulation::UploadEnvironment
// + SubmitWorldgen), reads the surface band back, and MEASURES:
//
//   * trees: at every lattice site the window contains (treeSite() replayed
//     on the CPU with rng::Hash3, the ONE mirror of the shader's hash3), is
//     the trunk-base voxel there? Counted per SITE, never per trunk voxel.
//   * cover: the material one voxel above the ground of every eligible
//     column, classified into the biome's cover rows (material or head),
//     a tree's ground-level cell, or UNAUTHORED — a plant no row placed.
//   * skin / subsoil: the material at y == h and y == h - skinDepth.
//
// against an EXPECTATION built from the packed worldMap words the shader
// itself reads (kB_TreeChanceQ16, the cover rows) plus the CPU height twin
// per column / per site for the minY/maxY gates. Where the engine's rule has
// no CPU twin the expectation is an INTERVAL, never a guess: a nearWater
// condition (its 60-voxel water walk is outside World::PondNearColumn's band)
// puts the row's whole share in `hi` and none in `lo`; the biome PATCH MASK
// is modelled in distribution (PatchPassFraction: the exact smoothstep-
// bilinear of four uniform corners, which is what vnoise2d is) rather than
// per column, because the noise twin is file-static in world.cpp. The slope
// gates are assumed to pass: the synthetic ground is the calm home area, and
// every authored maxSlope is at or above the angle of repose.
//
// TWO PREDICTIONS, TWO COMPARISONS. The NOMINAL numbers (densityStats /
// "1 in N" — what the page prints) are computed here from the loaded biome
// set and held to the JS: scripts/test_environment.mjs writes them to
// tests/env_predictions.json with an FNV over assets/biomes/*.json, and this
// gate fails if the file is stale or its numbers differ from the port's. The
// EXPECTED numbers (nominal, thinned onto the engine's one lattice, gated by
// height, patch and order) are what the GPU is held to.
//
// WHAT IT DOES NOT ASSERT, AND SAYS SO. Since P-G the shader's undergrowth /
// flower chain is COVER ROWS (with a canopy condition), so every biome's rows
// are asserted -- with two exceptions that are still a second source of a
// row's material: the TILE plants (plantColumnAt's ferns and big toadstools,
// PLANT_* in common.wgsl, an analytic footprint the renderer rebuilds and
// not a row) and the desert's proc cactus shape. A row naming one of those
// materials is REPORTED, not asserted, and a tile-plant cell at h + 1 that no
// row names is counted as `tilePlant`, beside `unauthored` -- which is now
// what its name says: a plant NOTHING authored, and its recorded value
// (envTruthUnauthoredPct_*) is the number that should stay at zero. A row
// with a CANOPY condition puts its whole share in `hi` and none in `lo`, like
// a nearWater row: the canopy cover has no CPU twin (it is the 25-tile tree
// scan), so the row is bounded, not predicted. Water bodies per km² is
// P-F's row: `TODO(P-F)` below.
//
// Thresholds live in tests/baseline.json (envTruth*), not here. Every run
// writes build/env_truth.json (the per-biome table, predicted and measured)
// so nobody re-runs the gate to read a number. Runs after `env-reload`: it
// regenerates once per biome and leaves the pristine world it found, through
// the same ReloadEnvironment + SubmitWorldgen that gate uses.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/biomes.h"
#include "sim/rng.h"
#include "sim/treeatlas.h"
#include "sim/tuning.h"
#include "sim/world.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"

namespace selftest {
namespace {

using sandvox::kDefaultSeed;

// ---- the patch mask, in distribution -----------------------------------------
// worldgen.wgsl's patch test is `vnoise2d(...).n >> 6 > thresh`, where .n is
// vbilerp(c00, c10, c01, c11, vsmooth(tx), vsmooth(tz)) over four hash corners
// uniform in [0, 16383] and the column's Q15 position inside a (1 << log2)
// cell. Over a window many cells wide the positions are exactly uniform on
// the in-cell grid and the corners are independent uniforms, so P(pass) is a
// pure function of (thresh, log2) — integrated here with the shader's own
// integer smoothstep/bilerp formulas and a fixed LCG for the corners. The
// arithmetic below restates two four-line formulas; the NOISE (hash corners,
// cell walk) is not restated, which is why this is a distribution and not a
// per-column twin. Sampling error ~0.1 % absolute, far under the realised
// fraction's own cell-to-cell variance (~3 % on 256 cells), which the
// envTruthCoverRelTol bound is sized for.
int Vsmooth(int t) {
  const int t2 = (t * t) >> 15;
  const int t3 = (t2 * t) >> 15;
  return 3 * t2 - 2 * t3;
}
int Q15Frac(int f, uint32_t csl) {
  if (csl <= 15u) return f << (15u - csl);
  return f >> (csl - 15u);
}
int Vbilerp(int c00, int c10, int c01, int c11, int sx, int sz) {
  const int a = c00 + (((c10 - c00) * sx) >> 15);
  const int b = c01 + (((c11 - c01) * sx) >> 15);
  return a + (((b - a) * sz) >> 15);
}
double PatchPassFraction(int thresh, uint32_t cellLog2) {
  if (thresh <= 0) return 1.0;
  static std::map<std::pair<int, uint32_t>, double> cache;
  const auto key = std::make_pair(thresh, cellLog2);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  const int cell = 1 << cellLog2;
  const int step = std::max(1, cell / 32);   // <= 32 positions per axis
  uint32_t lcg = 0x9E3779B9u;
  auto corner = [&]() {
    lcg = lcg * 1664525u + 1013904223u;
    return static_cast<int>((lcg >> 8) & 0x3FFFu);
  };
  uint64_t pass = 0, total = 0;
  for (int fz = 0; fz < cell; fz += step)
    for (int fx = 0; fx < cell; fx += step) {
      const int sx = Vsmooth(Q15Frac(fx, cellLog2));
      const int sz = Vsmooth(Q15Frac(fz, cellLog2));
      for (int k = 0; k < 256; k++) {
        const int c00 = corner(), c10 = corner(), c01 = corner(), c11 = corner();
        const int pm = Vbilerp(c00, c10, c01, c11, sx, sz) >> 6;
        pass += pm > thresh;
        total++;
      }
    }
  const double p = total ? static_cast<double>(pass) / static_cast<double>(total) : 1.0;
  cache[key] = p;
  return p;
}

// ---- the nominal port: what the page prints ------------------------------------
// biomegen.js densityStats(tileM, pct) and the cover row's "1 in N", held to
// tests/env_predictions.json (written by scripts/test_environment.mjs).
double NominalTreesPerHa(const biomes::BiomeDef& b) {
  if (b.treeTileM <= 0) return 0.0;
  const double tilesPerHa = 10000.0 / (static_cast<double>(b.treeTileM) * b.treeTileM);
  return tilesPerHa * b.treeDensity / 100.0;
}
double NominalCoverPct(const biomes::CoverRow& r) {
  return r.chance > 0 ? 100.0 / r.chance : 0.0;
}

struct RowStat {
  std::string material;
  bool exact = true;         // asserted, or report-only (tile plant / proc cactus)
  int group = -1;            // index of the FIRST row naming this material (P-G): a voxel
                             // cannot say which of two rows of one material placed it, so
                             // rows sharing a material are asserted as ONE sum
  double nominalPct = 0;     // the page's number
  double expLo = 0, expHi = 0, sigma = 0;   // expected COUNT over eligible columns
  int measured = 0;
};
struct BiomeStat {
  std::string name;
  int index = -1;
  int columns = 0;           // eligible surface columns
  int sites = 0;             // eligible lattice sites
  double areaHa = 0;         // eligible ground, hectares
  double nominalTreesPerHa = 0;
  double treeLo = 0, treeHi = 0, treeSigma = 0;
  int treesMeasured = 0;
  double treesPerHa = 0;
  int skinOk = 0, subOk = 0, unauthored = 0, treeGround = 0, tilePlant = 0;
  bool flora = false, cacti = false;
  double tGen = 0, tTwin = 0, tRead = 0;   // seconds: pack+upload+worldgen, the CPU twins, the readback
  std::vector<RowStat> rows;
  std::vector<std::string> problems;
};

std::string Pct(double f) { return Format("%.2f%%", f * 100.0); }
// The tile plants (worldgen.wgsl plantColumnAt, common.wgsl PLANT_*): placed
// by footprint, not by row, so a row naming them is a second source.
bool IsTilePlant(const std::string& material) { return material == "fern" || material == "mushroom_large"; }

}  // namespace

Status GateEnvTruth(Ctx& c, std::string& detail) {
  using sandvox::ReadVoxelsSync;
  using sandvox::ReloadEnvironment;
  using sandvox::SubmitWorldgen;
  namespace fs = std::filesystem;
  const std::string dir = sandvox::AssetDir();
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("env-truth: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  };

  // ---- inputs -----------------------------------------------------------------
  biomes::BiomeSet set;
  std::string log;
  if (!biomes::LoadBiomeSet(dir, c.mats, set, log)) return fail("biome files did not load: " + log);
  TreeAtlas atlas;
  if (!LoadTreeAtlas(dir + "/trees", c.mats, set, atlas, log)) return fail("tree atlas did not load: " + log);
  const worldmap::WorldMapData real = worldmap::CurrentWorldMap();   // a COPY: the synthetic maps derive from it
  if (!real.Loaded()) return fail("no world map loaded (world.mapLayer = " + CurrentTuning().world.mapLayer + ")");
  const int treeline = worldmap::CurrentTerrain().treeline;

  // ---- thresholds (tests/baseline.json) -----------------------------------------
  const double sigmas = BaselineNumber("envTruthSigmas", 3.0);
  const double treeSlack = BaselineNumber("envTruthTreeSlack", 1.0);
  const double coverRelTol = BaselineNumber("envTruthCoverRelTol", 0.25);
  const double coverAbsTolPct = BaselineNumber("envTruthCoverAbsTolPct", 0.2);
  const double skinMinPct = BaselineNumber("envTruthSkinMinPct", 99.0);
  const double subsoilMinPct = BaselineNumber("envTruthSubsoilMinPct", 0.0);

  // ---- A. the nominal port, held to the JS export -------------------------------
  // tests/env_predictions.json sits beside tests/baseline.json (resolved from
  // the asset dir the same way). Absent = the export was never run: FAIL, the
  // file is part of the contract.
  std::vector<std::string> problems;
  {
    const fs::path pj = fs::path(dir).parent_path() / "tests" / "env_predictions.json";
    std::ifstream f(pj);
    if (!f) {
      problems.push_back("tests/env_predictions.json is missing -- run `node scripts/test_environment.mjs`");
    } else {
      nlohmann::json j;
      try { f >> j; } catch (const std::exception& e) {
        problems.push_back(std::string("tests/env_predictions.json did not parse: ") + e.what());
        j = nlohmann::json::object();
      }
      const uint32_t nowHash = biomes::HashFileSet(dir + "/biomes", {".json"});
      const std::string want = Format("%08x", nowHash);
      const std::string have = j.value("biomesHash", std::string("?"));
      if (have != want)
        problems.push_back("tests/env_predictions.json is STALE: biomesHash " + have + " but assets/biomes hashes " +
                           want + " -- run `node scripts/test_environment.mjs` and commit the file");
      const nlohmann::json jb = j.value("biomes", nlohmann::json::object());
      for (const biomes::BiomeDef& b : set.biomes) {
        if (!jb.contains(b.name)) { problems.push_back("predictions have no entry for biome '" + b.name + "'"); continue; }
        const nlohmann::json& e = jb[b.name];
        const double perHa = NominalTreesPerHa(b);
        const double jsPerHa = e.value("treesPerHa", -1.0);
        if (std::fabs(perHa - jsPerHa) > 1e-6 * std::max(1.0, std::fabs(perHa)))
          problems.push_back(Format("%s: the C++ port says %.4f trees/ha, the JS export says %.4f", b.name.c_str(), perHa, jsPerHa));
        if (e.value("skin", std::string()) != b.skin || e.value("subsoil", std::string()) != b.subsoil)
          problems.push_back(b.name + ": skin/subsoil differ between the port and the export");
        const nlohmann::json rows = e.value("cover", nlohmann::json::array());
        if (rows.size() != b.cover.size())
          problems.push_back(Format("%s: %zu cover rows in the port, %zu in the export", b.name.c_str(), b.cover.size(), rows.size()));
        for (size_t i = 0; i < b.cover.size() && i < rows.size(); i++) {
          const double pct = NominalCoverPct(b.cover[i]);
          const double jsPct = rows[i].value("pct", -1.0);
          if (rows[i].value("material", std::string()) != b.cover[i].material || std::fabs(pct - jsPct) > 1e-6)
            problems.push_back(Format("%s cover row %zu (%s): port %.4f%% vs export %.4f%%", b.name.c_str(), i,
                                      b.cover[i].material.c_str(), pct, jsPct));
        }
      }
    }
  }

  // ---- the atlas, decoded for the measurement ----------------------------------
  // Trunk-base materials: what each variant puts at its anchor column one
  // voxel above the ground (local y 0). That voxel at the replayed site IS the
  // tree, whichever way it was rotated or mirrored (treeLocalXZ rotates about
  // the anchor). And every material any variant puts at ground level, so a low
  // crown at h + 1 is classed as a tree's cell rather than as unauthored cover.
  // A variant whose anchor column starts above local y 0 (a lifted crown, a
  // bush on a stem) is caught at the LOWEST non-air cell of that column, so
  // (ly, material) pairs rather than materials; the readback band reaches
  // h + 1 + the deepest such ly.
  std::set<std::pair<int, uint32_t>> trunkBase;
  std::set<uint32_t> treeGroundMats;
  int trunkMaxLy = 0;
  std::string trunkNote;
  {
    const uint32_t* W = atlas.words.data();
    for (int sp = 0; sp < atlas.speciesCount; sp++) {
      const uint32_t* s = W + W[treeatlas::kHSpeciesDir] + sp * treeatlas::kSpeciesWords;
      const int vc = static_cast<int>(s[treeatlas::kSVariantCount]);
      std::string per;
      for (int v = 0; v < vc; v++) {
        const uint32_t* d = W + s[treeatlas::kSVariantDir] + v * treeatlas::kVariantWords;
        const int nx = static_cast<int>(d[treeatlas::kVNx]), ny = static_cast<int>(d[treeatlas::kVNy]),
                  nz = static_cast<int>(d[treeatlas::kVNz]);
        const int ax = static_cast<int>(d[treeatlas::kVAnchorX]), az = static_cast<int>(d[treeatlas::kVAnchorZ]);
        int ly = 0;
        uint32_t m = 0;
        for (; ly < ny && !m; ly++) m = TreeAtlasCellAt(atlas, sp, v, ax, ly, az);
        if (m) {
          ly--;
          trunkBase.insert({ly, m});
          trunkMaxLy = std::max(trunkMaxLy, ly);
          per += Format("%s%s@%d", per.empty() ? "" : ",", m < c.mats.size() ? c.mats[m].name.c_str() : "?", ly);
        } else {
          per += std::string(per.empty() ? "" : ",") + "NONE";
        }
        for (int lz = 0; lz < nz; lz++)
          for (int lx = 0; lx < nx; lx++) {
            const uint32_t g = TreeAtlasCellAt(atlas, sp, v, lx, 0, lz);
            if (g) treeGroundMats.insert(g);
          }
      }
      trunkNote += (sp ? " " : "") + (sp < static_cast<int>(atlas.species.size()) ? atlas.species[sp].name : Format("sp%d", sp)) + "[" + per + "]";
    }
  }
  std::printf("env-truth: trunk bases (material@local y, per variant): %s\n", trunkNote.c_str());
  const treeatlas::TreeLattice& lat = treeatlas::CurrentTreeLattice();
  const int T = lat.tile;

  // ---- the window, the authored rim ---------------------------------------------
  const IVec3 org = c.world.WindowOrigin();
  const int x0 = org.x * static_cast<int>(kChunk), y0 = org.y * static_cast<int>(kChunk),
            z0 = org.z * static_cast<int>(kChunk);
  const int N = static_cast<int>(kWorldN);
  // The authored pool (world.h AuthoredPool) is worldgen's, not the map's, so
  // it is in every synthetic world; the shader keeps trees and cover out of
  // its rim ring (`!inRim`, radius vlen(80)). Both are excluded from the
  // measured ground, and so is everything World::PondNearColumn calls a tarn
  // or its shore.
  World::AuthoredPool pools[World::kAuthoredPools];
  World::AuthoredPoolList(pools);
  const int pRim = (80 * kVoxelsPerMetre) / std::max(1, worldmap::CurrentTerrain().refVoxelsPerMetre);
  auto inRim = [&](int x, int z) {
    for (const World::AuthoredPool& p : pools) {
      const long long dx = x - p.cx, dz = z - p.cz;
      if (dx * dx + dz * dz < static_cast<long long>(pRim) * pRim) return true;
    }
    return false;
  };

  std::vector<BiomeStat> stats;
  std::vector<int16_t> hcol(static_cast<size_t>(N) * N);
  std::vector<uint8_t> elig(static_cast<size_t>(N) * N);
  bool gpuState = false;   // an environment other than the real one is uploaded

  std::vector<const biomes::BiomeDef*> order;
  for (const biomes::BiomeDef& b : set.biomes) if (b.index >= 0) order.push_back(&b);
  std::sort(order.begin(), order.end(), [](const biomes::BiomeDef* a, const biomes::BiomeDef* b) { return a->index < b->index; });

  for (const biomes::BiomeDef* bp : order) {
    const biomes::BiomeDef& b = *bp;
    BiomeStat st;
    st.name = b.name;
    st.index = b.index;
    st.flora = b.groundFlora;
    st.cacti = b.cacti;
    st.nominalTreesPerHa = NominalTreesPerHa(b);

    // ---- B. the synthetic one-biome map --------------------------------------
    worldmap::WorldMapData syn = real;
    std::fill(syn.biome.begin(), syn.biome.end(), static_cast<uint8_t>(b.index));
    std::fill(syn.landform.begin(), syn.landform.end(), static_cast<uint8_t>(128));   // mapLandformQ8's "flat"
    std::fill(syn.moisture.begin(), syn.moisture.end(), static_cast<uint8_t>(128));
    syn.sites.clear();
    syn.siteIndex.assign(syn.siteIndex.size(), 0);
    // The harness box, FAR AWAY rather than empty: crownMeetsHarness has no
    // "no box" case (an empty x1 < x0 box still catches crowns spanning the
    // origin), while a box a million voxels off matches nothing and reports
    // "far" to the home-area fade exactly as no box would.
    syn.harnessX0 = syn.harnessZ0 = 1 << 20;
    syn.harnessX1 = syn.harnessZ1 = (1 << 20) + 1;
    syn.spawnX = x0 + N / 2;
    syn.spawnZ = z0 + N / 2;
    syn.spawnAuthored = true;
    syn.name = "env-truth:" + b.name;
    syn.contentHash = 0x9E3779B9u * static_cast<uint32_t>(b.index + 1);
    std::vector<uint32_t> words;
    double tMark = sandvox::NowSeconds();
    if (!worldmap::PackWorldMap(set, syn, words, log)) { st.problems.push_back("pack failed: " + log); stats.push_back(st); continue; }
    worldmap::SetCurrentWorldMap(syn);            // the CPU twins see the same map
    c.ctx.WaitIdle();
    c.sim.UploadEnvironment(c.ctx.device, c.ctx.queue, atlas, words);
    gpuState = true;
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    st.tGen = sandvox::NowSeconds() - tMark;
    tMark = sandvox::NowSeconds();

    // The biome record and its cover rows, as the shader reads them.
    const uint32_t* R = words.data() + words[worldmap::kHBiomeRecords] + static_cast<size_t>(b.index) * worldmap::kBiomeRecWords;
    const uint32_t skinId = R[worldmap::kB_Skin], subsoilId = R[worldmap::kB_Subsoil];
    const int skinDepth = std::max(1, static_cast<int>(R[worldmap::kB_SkinDepth]));
    const int bThresh = static_cast<int>(R[worldmap::kB_PatchThreshold]);
    const uint32_t pLog2 = R[worldmap::kB_PatchCellLog2];
    const uint32_t nRows = R[worldmap::kB_CoverCount];
    const uint32_t* rows = words.data() + R[worldmap::kB_CoverOff];
    const double chanceQ16 = R[worldmap::kB_TreeChanceQ16] / 65536.0;
    struct Row { uint32_t mat, head, chance; int minY, maxY, nwMax, nwMin, patch, canopyMin, canopyMax; double pf; };
    std::vector<Row> rowv;
    for (uint32_t i = 0; i < nRows; i++) {
      const uint32_t* r = rows + static_cast<size_t>(i) * worldmap::kCoverRowWords;
      Row row;
      row.mat = r[worldmap::kC_Mat]; row.head = r[worldmap::kC_Head]; row.chance = r[worldmap::kC_Chance];
      row.minY = static_cast<int32_t>(r[worldmap::kC_MinY]); row.maxY = static_cast<int32_t>(r[worldmap::kC_MaxY]);
      row.nwMax = static_cast<int32_t>(r[worldmap::kC_NearWaterMax]); row.nwMin = static_cast<int32_t>(r[worldmap::kC_NearWaterMin]);
      row.canopyMin = static_cast<int>(r[worldmap::kC_CanopyMin]); row.canopyMax = static_cast<int>(r[worldmap::kC_CanopyMax]);
      row.patch = std::max(bThresh, static_cast<int>(r[worldmap::kC_PatchThreshold]));
      row.pf = PatchPassFraction(row.patch, pLog2);
      rowv.push_back(row);
      RowStat rs;
      rs.material = row.mat < c.mats.size() ? c.mats[row.mat].name : Format("id%u", row.mat);
      rs.nominalPct = i < b.cover.size() ? NominalCoverPct(b.cover[i]) : 0.0;
      // Exact only where nothing but the row can place its material: not a
      // tile plant (plantColumnAt places ferns and big toadstools by
      // footprint) and not a cactus material beside the proc cactus.
      rs.exact = !IsTilePlant(rs.material) && !(st.cacti && rs.material.find("cactus") != std::string::npos);
      st.rows.push_back(rs);
    }
    auto rowOf = [&](uint32_t m) -> int {
      for (size_t i = 0; i < rowv.size(); i++)
        if (m == rowv[i].mat || (rowv[i].head && m == rowv[i].head)) return static_cast<int>(i);
      return -1;
    };

    // ---- C. the columns: the twin height, eligibility, the cover expectation --
    int hmin[kNChunk], hmax[kNChunk];
    for (uint32_t i = 0; i < kNChunk; i++) { hmin[i] = 1 << 30; hmax[i] = -(1 << 30); }
    st.columns = 0;
    for (int z = 0; z < N; z++)
      for (int x = 0; x < N; x++) {
        const int wx = x0 + x, wz = z0 + z;
        const size_t idx = static_cast<size_t>(z) * N + x;
        const int h = World::TerrainHeight(wx, wz, kDefaultSeed);
        hcol[idx] = static_cast<int16_t>(h);
        bool ok = h < treeline && h - skinDepth >= y0 && h + 1 < y0 + N && !inRim(wx, wz);
        if (ok) {
          const World::PondQuery pq = World::PondNearColumn(wx, wz, kDefaultSeed);
          ok = !pq.inDisc && !pq.near;
        }
        elig[idx] = ok ? 1 : 0;
        if (!ok) continue;
        st.columns++;
        const uint32_t cz = static_cast<uint32_t>(z) / kChunk;
        hmin[cz] = std::min(hmin[cz], h);
        hmax[cz] = std::max(hmax[cz], h);
        // Rows roll in order, first hit wins: row i's share is its own pass
        // probability times the product of every earlier row's miss. Height
        // gates are exact (the twin's h); a nearWater row contributes to hi
        // only; the patch is the modelled fraction.
        double remainLo = 1.0, remainHi = 1.0;
        for (size_t i = 0; i < rowv.size(); i++) {
          const Row& r = rowv[i];
          if (r.chance == 0) continue;
          double p = 1.0 / r.chance;
          if (r.minY >= 0 && h < r.minY) p = 0;
          if (r.maxY >= 0 && h > r.maxY) p = 0;
          p *= r.pf;
          // A nearWater or a canopy condition has no CPU twin: hi only.
          const bool bounded = r.nwMax >= 0 || r.nwMin > 0 || r.canopyMin > 0 || r.canopyMax < 255;
          const double pLo = bounded ? 0.0 : p, pHi = p;
          st.rows[i].expLo += remainLo * pLo;
          st.rows[i].expHi += remainHi * pHi;
          st.rows[i].sigma += remainHi * pHi * (1.0 - remainHi * pHi);
          remainLo *= (1.0 - pHi);     // the earliest a later row can be reached: every earlier row present
          remainHi *= (1.0 - pLo);
        }
      }
    for (RowStat& rs : st.rows) rs.sigma = std::sqrt(rs.sigma);
    st.areaHa = st.columns * (kVoxelMeters * kVoxelMeters) / 10000.0;

    // ---- D. the lattice sites: treeSite() replayed, the expectation per site ---
    {
      const uint32_t* W = atlas.words.data();
      const int ns = atlas.speciesCount;
      const uint32_t bt = W[treeatlas::kHBiomeTable] + static_cast<uint32_t>(b.index) * (1u + static_cast<uint32_t>(ns));
      const uint32_t total = ns > 0 && b.index < atlas.biomeCount ? W[bt] : 0u;
      std::vector<double> weight(std::max(ns, 0), 0.0);
      for (int i = 0; i < ns && total; i++)
        weight[i] = static_cast<double>(W[bt + 1 + i]) - (i ? static_cast<double>(W[bt + i]) : 0.0);
      auto fdiv = [](int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); };
      const int txLo = fdiv(x0, T), txHi = fdiv(x0 + N - 1, T);
      const int tzLo = fdiv(z0, T), tzHi = fdiv(z0 + N - 1, T);
      const int inset = T / 4;
      const uint32_t span = static_cast<uint32_t>(T / 2);
      std::vector<std::pair<IVec3, double>> sitesHi;   // (wx, h, wz), p_hi -- measured in pass E
      for (int tz = tzLo; tz <= tzHi; tz++)
        for (int tx = txLo; tx <= txHi; tx++) {
          const uint32_t hsh = rng::Hash3(kDefaultSeed ^ 0x7BEE5u, static_cast<uint32_t>(tx), static_cast<uint32_t>(tz));
          const int wx = tx * T + inset + static_cast<int>((hsh >> 3u) % span);
          const int wz = tz * T + inset + static_cast<int>((hsh >> 9u) % span);
          if (wx < x0 || wx >= x0 + N || wz < z0 || wz >= z0 + N) continue;
          const int h = hcol[static_cast<size_t>(wz - z0) * N + (wx - x0)];
          if (h >= treeline || h + 1 < y0 || h + 1 >= y0 + N || inRim(wx, wz)) continue;
          if (World::PondNearColumn(wx, wz, kDefaultSeed).inDisc) continue;
          st.sites++;
          {   // the readback band must reach the site's trunk cell too
            const uint32_t cz = static_cast<uint32_t>(wz - z0) / kChunk;
            hmin[cz] = std::min(hmin[cz], h);
            hmax[cz] = std::max(hmax[cz], h);
          }
          double passLo = 0, passHi = 0;
          for (int sp = 0; sp < ns && total; sp++) {
            if (weight[sp] <= 0) continue;
            const uint32_t* s = W + W[treeatlas::kHSpeciesDir] + sp * treeatlas::kSpeciesWords;
            const int minY = static_cast<int32_t>(s[treeatlas::kSMinY]), maxY = static_cast<int32_t>(s[treeatlas::kSMaxY]);
            if (minY >= 0 && h < minY) continue;
            if (maxY >= 0 && h > maxY) continue;
            const uint32_t* cb = W + W[treeatlas::kHCondTable] +
                                 (static_cast<uint32_t>(b.index) * static_cast<uint32_t>(ns) + static_cast<uint32_t>(sp)) * treeatlas::kCondWords;
            const int rMinY = static_cast<int32_t>(cb[treeatlas::kCMinY]), rMaxY = static_cast<int32_t>(cb[treeatlas::kCMaxY]);
            if (rMinY >= 0 && h < rMinY) continue;
            if (rMaxY >= 0 && h > rMaxY) continue;
            const int nwMax = static_cast<int32_t>(cb[treeatlas::kCNearWaterMax]), nwMin = static_cast<int32_t>(cb[treeatlas::kCNearWaterMin]);
            const int rPatch = static_cast<int32_t>(cb[treeatlas::kCPatchThreshold]);
            const double pf = rPatch > 0 ? PatchPassFraction(rPatch, pLog2) : 1.0;
            const double share = weight[sp] / total * pf;
            passHi += share;
            if (!(nwMax >= 0 || nwMin > 0)) passLo += share;
          }
          const double pLo = chanceQ16 * passLo, pHi = chanceQ16 * passHi;
          st.treeLo += pLo;
          st.treeHi += pHi;
          st.treeSigma += pHi * (1.0 - pHi);
          sitesHi.push_back({IVec3{wx, h, wz}, pHi});
        }
      st.treeSigma = std::sqrt(st.treeSigma);
      st.tTwin = sandvox::NowSeconds() - tMark;
      tMark = sandvox::NowSeconds();

      // ---- E. the readback: one x-run of chunks per (cy, cz) ---------------------
      // SlotChunkIndex is contiguous in cx, so a whole window row is one call.
      std::vector<std::vector<uint32_t>> bufs;
      std::vector<int> bufCy;
      std::vector<uint8_t> siteMark(static_cast<size_t>(N) * N, 0);
      for (const auto& s : sitesHi) siteMark[static_cast<size_t>(s.first.z - z0) * N + (s.first.x - x0)] = 1;
      for (uint32_t cz = 0; cz < kNChunk; cz++) {
        if (hmin[cz] > hmax[cz]) continue;   // no eligible column in this row
        const int cyLo = std::max(org.y, (hmin[cz] - skinDepth) >> 4);
        const int cyHi = std::min(org.y + static_cast<int>(kNChunk) - 1, (hmax[cz] + 1 + trunkMaxLy) >> 4);
        bufs.clear(); bufCy.clear();
        for (int cy = cyLo; cy <= cyHi; cy++) {
          bufs.emplace_back(static_cast<size_t>(kNChunk) * kChunkVol, 0u);
          ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({org.x, cy, org.z + static_cast<int>(cz)}), kNChunk,
                         bufs.back().data(), "env-truth");
          bufCy.push_back(cy);
        }
        auto wordAt = [&](int x, int y, int z) -> uint32_t {   // window-local x, z; world y
          const int cy = y >> 4;
          const size_t k = static_cast<size_t>(cy - cyLo);
          if (k >= bufs.size()) return 0u;
          const size_t chunk = static_cast<size_t>(x >> 4);
          const uint32_t local = (static_cast<uint32_t>(z & 15) * kChunk + static_cast<uint32_t>(y & 15)) * kChunk + static_cast<uint32_t>(x & 15);
          return bufs[k][chunk * kChunkVol + local] & 0xFFFu;
        };
        for (int lz = 0; lz < static_cast<int>(kChunk); lz++) {
          const int z = static_cast<int>(cz) * static_cast<int>(kChunk) + lz;
          for (int x = 0; x < N; x++) {
            const size_t idx = static_cast<size_t>(z) * N + x;
            const int h = hcol[idx];
            if (siteMark[idx] && h + 1 >= y0 && h + 1 + trunkMaxLy < y0 + N && h < treeline) {
              for (const auto& tb : trunkBase)
                if (wordAt(x, h + 1 + tb.first, z) == tb.second) { st.treesMeasured++; break; }
            }
            if (!elig[idx]) continue;
            if (wordAt(x, h, z) == skinId) st.skinOk++;
            if (wordAt(x, h - skinDepth, z) == subsoilId) st.subOk++;
            const uint32_t above = wordAt(x, h + 1, z);
            if (above == 0) continue;
            const int ri = rowOf(above);
            if (ri >= 0) st.rows[ri].measured++;
            else if (treeGroundMats.count(above)) st.treeGround++;
            else if (above < c.mats.size() && IsTilePlant(c.mats[above].name)) st.tilePlant++;
            else st.unauthored++;
          }
        }
      }
      st.tRead = sandvox::NowSeconds() - tMark;
      st.treesPerHa = st.areaHa > 0 ? st.treesMeasured / st.areaHa : 0.0;
    }

    // ---- F. the verdicts, per biome -------------------------------------------
    if (st.columns < N * N / 4)
      st.problems.push_back(Format("only %d of %d columns were measurable ground (treeline %d, window y %d..%d)",
                                   st.columns, N * N, treeline, y0, y0 + N - 1));
    {
      const double lo = st.treeLo - sigmas * st.treeSigma - treeSlack;
      const double hi = st.treeHi + sigmas * st.treeSigma + treeSlack;
      if (st.treesMeasured < lo || st.treesMeasured > hi)
        st.problems.push_back(Format("trees: %d at %d sites, expected %.1f..%.1f (+-%.1f sigma x %.0f, slack %.0f)",
                                     st.treesMeasured, st.sites, st.treeLo, st.treeHi, st.treeSigma, sigmas, treeSlack));
    }
    // Rows are asserted per MATERIAL (P-G): the seeded chain rows and a
    // biome's own rows can name the same plant (forest has a canopy
    // mushroom_cluster row AND its old one), and the voxel at h + 1 cannot
    // say which row placed it -- rowOf credits the first. So the group's
    // expectation is the SUM over its rows (lo, hi, sigma in quadrature) and
    // the group is exact only if every row in it is.
    for (size_t i = 0; i < st.rows.size(); i++) {
      st.rows[i].group = static_cast<int>(i);
      for (size_t j = 0; j < i; j++)
        if (st.rows[j].material == st.rows[i].material) { st.rows[i].group = st.rows[j].group; break; }
    }
    for (size_t i = 0; i < st.rows.size(); i++) {
      if (st.rows[i].group != static_cast<int>(i) || st.columns == 0) continue;
      double lo = 0, hi = 0, var = 0;
      bool exact = true;
      int measured = 0;
      for (size_t j = i; j < st.rows.size(); j++) {
        if (st.rows[j].group != static_cast<int>(i)) continue;
        lo += st.rows[j].expLo; hi += st.rows[j].expHi; var += st.rows[j].sigma * st.rows[j].sigma;
        exact = exact && st.rows[j].exact;
        measured += st.rows[j].measured;
      }
      if (!exact) continue;
      const double sigma = std::sqrt(var);
      const double tol = std::max(std::max(sigmas * sigma, coverRelTol * hi), coverAbsTolPct / 100.0 * st.columns);
      if (measured < lo - tol || measured > hi + tol)
        st.problems.push_back(Format("cover %s: %d columns (%s), expected %.0f..%.0f (%s..%s), tol %.0f",
                                     st.rows[i].material.c_str(), measured, Pct(static_cast<double>(measured) / st.columns).c_str(),
                                     lo, hi, Pct(lo / st.columns).c_str(), Pct(hi / st.columns).c_str(), tol));
    }
    if (st.columns > 0) {
      const double skinPct = 100.0 * st.skinOk / st.columns, subPct = 100.0 * st.subOk / st.columns;
      if (skinPct < skinMinPct)
        st.problems.push_back(Format("skin: %.2f%% of columns wear %s at y == h (min %.1f%%)", skinPct,
                                     skinId < c.mats.size() ? c.mats[skinId].name.c_str() : "?", skinMinPct));
      if (subPct < subsoilMinPct)
        st.problems.push_back(Format("subsoil: %.2f%% of columns have %s at y == h - %d (min %.1f%%)", subPct,
                                     subsoilId < c.mats.size() ? c.mats[subsoilId].name.c_str() : "?", skinDepth, subsoilMinPct));
    }
    RecordObserved(("envTruthTreesPerHa_" + b.name).c_str(), Format("%.1f", st.treesPerHa));
    RecordObserved(("envTruthUnauthoredPct_" + b.name).c_str(),
                   Format("%.2f", st.columns ? 100.0 * st.unauthored / st.columns : 0.0));
    stats.push_back(st);
  }

  // ---- G. the real environment back, from disk, and the pristine world ----------
  if (gpuState) {
    biomes::EnvironmentStamp stamp;
    if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, log))
      problems.push_back("ReloadEnvironment REFUSED after the sweep: " + log);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  }

  // ---- H. the table, the file, the verdict ------------------------------------
  std::string js = "{\n  \"treeTileVox\": " + std::to_string(T) + ",\n  \"biomes\": {\n";
  for (size_t bi = 0; bi < stats.size(); bi++) {
    const BiomeStat& st = stats[bi];
    std::printf("env-truth: %-7s %6d cols %5.2f ha | trees %3d / %3d sites, exp %5.1f..%5.1f (+-%.1f) = %6.1f/ha (page %6.1f) | skin %s sub %s | tree-ground %s tile-plant %s unauthored %s | gen %.2f twin %.2f read %.2f s%s\n",
                st.name.c_str(), st.columns, st.areaHa, st.treesMeasured, st.sites, st.treeLo, st.treeHi, st.treeSigma,
                st.treesPerHa, st.nominalTreesPerHa,
                Pct(st.columns ? static_cast<double>(st.skinOk) / st.columns : 0).c_str(),
                Pct(st.columns ? static_cast<double>(st.subOk) / st.columns : 0).c_str(),
                Pct(st.columns ? static_cast<double>(st.treeGround) / st.columns : 0).c_str(),
                Pct(st.columns ? static_cast<double>(st.tilePlant) / st.columns : 0).c_str(),
                Pct(st.columns ? static_cast<double>(st.unauthored) / st.columns : 0).c_str(),
                st.tGen, st.tTwin, st.tRead,
                st.flora ? "  [tile plants: fern / mushroom_large rows reported, not asserted]" : (st.cacti ? "  [proc cactus: cactus rows reported, not asserted]" : ""));
    for (size_t i = 0; i < st.rows.size(); i++) {
      const RowStat& rs = st.rows[i];
      const bool first = rs.group == static_cast<int>(i);
      int others = 0;
      for (const RowStat& o : st.rows) if (&o != &rs && o.group == rs.group) others++;
      std::printf("env-truth:   %-8s cover %-18s page %5.2f%%  expected %5.2f..%5.2f%%  measured %5.2f%% (%d)%s%s\n", "",
                  rs.material.c_str(), rs.nominalPct,
                  st.columns ? 100.0 * rs.expLo / st.columns : 0.0, st.columns ? 100.0 * rs.expHi / st.columns : 0.0,
                  st.columns ? 100.0 * rs.measured / st.columns : 0.0, rs.measured, rs.exact ? "" : "  (report only)",
                  others ? (first ? "  (measured: this material over all its rows)" : "  (counted with the first row of this material)") : "");
    }
    std::printf("env-truth:   %-8s water bodies per km2: TODO(P-F) -- the pond table is the water preset's from P-F on\n", "");
    for (const std::string& p : st.problems) std::printf("env-truth:   %s: %s\n", st.name.c_str(), p.c_str());
    js += "    \"" + st.name + "\": {\"index\": " + std::to_string(st.index) + ", \"columns\": " + std::to_string(st.columns) +
          Format(", \"areaHa\": %.4f, \"sites\": %d, \"treesMeasured\": %d, \"treesExpectedLo\": %.3f, \"treesExpectedHi\": %.3f, \"treesSigma\": %.3f, \"treesPerHa\": %.2f, \"treesPerHaPage\": %.2f",
                 st.areaHa, st.sites, st.treesMeasured, st.treeLo, st.treeHi, st.treeSigma, st.treesPerHa, st.nominalTreesPerHa) +
          Format(", \"skinPct\": %.3f, \"subsoilPct\": %.3f, \"treeGroundPct\": %.3f, \"tilePlantPct\": %.3f, \"unauthoredPct\": %.3f, \"groundFlora\": %s",
                 st.columns ? 100.0 * st.skinOk / st.columns : 0.0, st.columns ? 100.0 * st.subOk / st.columns : 0.0,
                 st.columns ? 100.0 * st.treeGround / st.columns : 0.0, st.columns ? 100.0 * st.tilePlant / st.columns : 0.0,
                 st.columns ? 100.0 * st.unauthored / st.columns : 0.0,
                 st.flora ? "true" : "false") +
          ", \"cover\": [";
    for (size_t i = 0; i < st.rows.size(); i++) {
      const RowStat& rs = st.rows[i];
      js += Format("%s{\"material\": \"%s\", \"pagePct\": %.4f, \"expectedLoPct\": %.4f, \"expectedHiPct\": %.4f, \"measuredPct\": %.4f, \"measured\": %d, \"asserted\": %s}",
                   i ? ", " : "", rs.material.c_str(), rs.nominalPct,
                   st.columns ? 100.0 * rs.expLo / st.columns : 0.0, st.columns ? 100.0 * rs.expHi / st.columns : 0.0,
                   st.columns ? 100.0 * rs.measured / st.columns : 0.0, rs.measured, rs.exact ? "true" : "false");
    }
    js += "], \"waterBodiesPerKm2\": null, \"problems\": [";
    for (size_t i = 0; i < st.problems.size(); i++) {
      std::string e;
      for (char ch : st.problems[i]) { if (ch == '"' || ch == '\\') e += '\\'; e += ch; }
      js += (i ? ", \"" : "\"") + e + "\"";
    }
    js += "]}" + std::string(bi + 1 < stats.size() ? "," : "") + "\n";
  }
  js += "  }\n}\n";
  {
    std::ofstream f("build/env_truth.json");
    if (f) f << js;
  }

  int failing = 0;
  for (const BiomeStat& st : stats) failing += !st.problems.empty();
  const bool ok = problems.empty() && failing == 0;
  std::string summary = Format("%zu biomes on a %d-voxel lattice: %d with a miss", stats.size(), T, failing);
  for (const std::string& p : problems) summary += "; " + p;
  for (const BiomeStat& st : stats)
    for (const std::string& p : st.problems) summary += "; " + st.name + ": " + p;
  detail = summary;
  std::printf("env-truth: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

const std::vector<Gate>& EnvTruthGates() {
  static const std::vector<Gate> g = {
      {"env-truth", "sim", {}, false, GateEnvTruth},
  };
  return g;
}

}  // namespace selftest
