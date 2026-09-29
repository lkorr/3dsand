// selftest_clearing.cpp — the FOREST CLEARING site kind (map.json kind
// "clearing", worldmap.h kSiteClearing, worldgen.wgsl clearingRefuses).
//
//   clearing  On the harness map, with ONE synthetic clearing added in memory
//             (the `sculpt` gate's seam: LoadWorldMap with a map.json text,
//             packed, uploaded, regenerated, read back; no file touched), over
//             forest near the map's spawn, the window moved to put it near
//             the window's low corner. Two
//             arms, WITHOUT the clearing (A) and WITH it (B), same window:
//               1. the box has trees in A (else the spot proves nothing) and
//                  NOT ONE tree-material voxel in B, crowns included;
//               2. trunks stand OUTSIDE it in B: past feather + two of the
//                  widest crowns (a refused tree's low branches reach a crown
//                  out) the trunk bases A had are still there (>= 90 %,
//                  reported exactly), and within feather + a crown of the box
//                  B has no MORE trunk columns than A (the band only thins);
//               3. the ground cover grows INSIDE it in B (non-tree matter in
//                  the few voxels over the ground);
//               4. the terrain is identical: every voxel from 8 below the
//                  ground to the ground, over the box and its feather band,
//                  has the same word in A and B (tree matter excepted -- a
//                  root is a tree's), and the CPU height mirror agrees;
//               5. the loader: a box with min > max is REFUSED naming the
//                  site, and a clearing over the spawn is a WARNING, not an
//                  error.
//             The far cascades draw their trees through the same
//             treeInfoBare (worldgen.wgsl), so they agree by construction.
//             Exit: the environment reloaded from disk, the window put back
//             where it was, regenerated, the far field refilled.

#include <algorithm>
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

// Every material a worldgen tree is made of (assets/trees/<name>.json bark /
// leaf / autumnLeaf), and, separately, its trunk (bark) materials.
void TreeMats(const std::vector<MaterialDef>& mats, std::vector<uint8_t>& any, std::vector<uint8_t>& bark) {
  std::set<std::string> all, trunk;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(AssetDir() + "/trees", ec)) {
    if (e.path().extension() != ".json") continue;
    std::ifstream f(e.path());
    nlohmann::json j;
    try { f >> j; } catch (...) { continue; }
    for (const char* k : {"bark", "leaf", "autumnLeaf"})
      if (j.contains(k) && j[k].is_array())
        for (const auto& n : j[k])
          if (n.is_string()) {
            all.insert(n.get<std::string>());
            if (std::string(k) == "bark") trunk.insert(n.get<std::string>());
          }
  }
  any.assign(mats.size() + 1, 0);
  bark.assign(mats.size() + 1, 0);
  for (size_t i = 0; i < mats.size(); i++) {
    any[i] = all.count(mats[i].name) ? 1 : 0;
    bark[i] = trunk.count(mats[i].name) ? 1 : 0;
  }
}

// Window voxels by whole slot ROWS (every chunk of one (cy, cz), the sculpt
// gate's read), cached: a column scan over the window reads each row once.
struct RowRead {
  Ctx* c = nullptr;
  std::map<std::pair<int, int>, std::vector<uint32_t>> rows;
  uint32_t Word(int x, int y, int z) {
    if (!c->world.CellInWindow({x, y, z})) return 0xFFFFFFFFu;
    const int cy = y >> 4, cz = z >> 4;
    auto it = rows.find({cy, cz});
    if (it == rows.end()) {
      std::vector<uint32_t> run(size_t(kNChunk) * kChunkVol);
      ReadVoxelsSync(c->ctx, c->world, World::SlotChunkIndex({0, cy, cz}), kNChunk, run.data(), "clearing");
      it = rows.emplace(std::make_pair(cy, cz), std::move(run)).first;
    }
    const int m = static_cast<int>(kNChunk) - 1;
    const uint32_t local = (uint32_t(z & 15) * kChunk + uint32_t(y & 15)) * kChunk + uint32_t(x & 15);
    return it->second[size_t((x >> 4) & m) * kChunkVol + local];
  }
  uint32_t Mat(int x, int y, int z) { return Word(x, y, z) & 0xFFFu; }
};

// Chebyshev distance from a column to the box, 0 inside.
int BoxDist(int x, int z, int x0, int z0, int x1, int z1) {
  return std::max(std::max(std::max(x0 - x, x - x1), std::max(z0 - z, z - z1)), 0);
}

Status GateClearing(Ctx& c, std::string& detail) {
  const std::string dir = AssetDir();
  std::vector<std::string> bad;
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("clearing: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  };
  biomes::BiomeSet set;
  std::string log;
  if (!biomes::LoadBiomeSet(dir, c.mats, set, log)) return fail("biome files did not load: " + log);
  TreeAtlas atlas;
  if (!LoadTreeAtlas(dir + "/trees", c.mats, set, atlas, log)) return fail("tree atlas did not load: " + log);
  std::vector<TreeSpeciesHeader> species;
  if (!ReadTreeSpeciesHeaders(dir + "/trees", species, log)) return fail("tree species headers: " + log);
  int maxReach = 0;
  for (const TreeSpeciesHeader& h : species) maxReach = std::max(maxReach, h.reach);
  const worldmap::WorldMapData real = worldmap::CurrentWorldMap();
  if (!real.Loaded()) return fail("no world map loaded");
  nlohmann::json base;
  {
    std::ifstream f(dir + "/worldmap/" + real.name + "/map.json");
    try { f >> base; } catch (const std::exception& e) { return fail(real.name + "/map.json did not parse: " + e.what()); }
  }
  std::vector<uint8_t> treeAny, treeBark;
  TreeMats(c.mats, treeAny, treeBark);
  auto isTree = [&](uint32_t m) { return m < treeAny.size() && treeAny[m]; };
  auto isBark = [&](uint32_t m) { return m < treeBark.size() && treeBark[m]; };

  // ---- 5. the loader, CPU only -------------------------------------------------------
  {
    nlohmann::json j = base;
    j["sites"].push_back({{"id", "cl_backwards"}, {"kind", "clearing"}, {"min", {900, 900}}, {"max", {800, 1000}}});
    worldmap::WorldMapData m;
    std::string l, t = j.dump();
    const bool refused = !worldmap::LoadWorldMap(dir, real.name, set, c.mats.size(), kDefaultSeed, m, l, &t);
    if (!(refused && l.find("cl_backwards") != std::string::npos))
      bad.push_back("5: a clearing with min x > max x was not refused naming it (" + l + ")");
    nlohmann::json j2 = base;
    j2["sites"].push_back({{"id", "cl_spawn"}, {"kind", "clearing"},
                           {"min", {real.spawnX - 10, real.spawnZ - 10}}, {"max", {real.spawnX + 10, real.spawnZ + 10}}});
    worldmap::WorldMapData m2;
    std::string l2, t2 = j2.dump();
    const bool loaded = worldmap::LoadWorldMap(dir, real.name, set, c.mats.size(), kDefaultSeed, m2, l2, &t2);
    bool warned = false;
    for (const std::string& w : m2.warnings) warned |= w.find("cl_spawn") != std::string::npos && w.find("spawn") != std::string::npos;
    if (!(loaded && warned)) bad.push_back("5: a clearing over the spawn did not load with a warning (" + l2 + ")");
    std::printf("clearing: 5 loader: backwards box %s, clearing over the spawn %s\n", refused ? "refused" : "LOADED",
                loaded ? (warned ? "loads with a warning" : "loads SILENTLY") : "REFUSED");
  }

  // ---- the spot: forest near the spawn, off the pad, dry, below the treeline ---------
  const int treeline = worldmap::CurrentTerrain().treeline;
  int forestId = -1;
  for (size_t i = 0; i < set.biomes.size(); i++)
    if (set.biomes[i].name == "forest") forestId = set.biomes[i].index;
  if (forestId < 0) return fail("no biome named forest in the set");
  constexpr int kHalf = 40, kFeather = 32;   // an 81 x 81 box
  int cx = 0, cz = 0;
  bool found = false;
  for (int ring = 1; ring < 12 && !found; ring++)
    for (int k = 0; k < 8 && !found; k++) {
      static const int dx[8] = {0, 1, 1, 1, 0, -1, -1, -1}, dz[8] = {1, 1, 0, -1, -1, -1, 0, 1};
      const int x = real.spawnX + dx[k] * ring * 300, z = real.spawnZ + dz[k] * ring * 300;
      bool ok = true;
      // The box sits near the window's low corner and the far strip runs to
      // its high one (below), so the forest must cover C-80 .. C+420.
      for (int s = 0; s <= 2 && ok; s++)
        for (int t = 0; t <= 2 && ok; t++) {
          const int px = x - 80 + s * 240, pz = z - 80 + t * 240;
          const int h = World::TerrainHeight(px, pz, kDefaultSeed);
          ok = (int)World::MapBiomeAt(px, pz, kDefaultSeed) == forestId && h < treeline - 32 &&
               h > real.seaLevelY + 4 && !World::InPadBox(px, pz) && !World::PondNearColumn(px, pz, kDefaultSeed).near;
        }
      if (ok) { cx = x; cz = z; found = true; }
    }
  if (!found) return fail("no forest spot near the spawn (" + std::to_string(real.spawnX) + "," + std::to_string(real.spawnZ) + ") to clear");
  const int bx0 = cx - kHalf, bz0 = cz - kHalf, bx1 = cx + kHalf, bz1 = cz + kHalf;

  nlohmann::json js = base;
  js["sites"].push_back({{"id", "cl_gate"}, {"kind", "clearing"}, {"min", {bx0, bz0}}, {"max", {bx1, bz1}}, {"feather", kFeather}});
  worldmap::WorldMapData syn;
  {
    std::string t = js.dump();
    if (!worldmap::LoadWorldMap(dir, real.name, set, c.mats.size(), kDefaultSeed, syn, log, &t))
      return fail("the synthetic map (with the clearing) was REFUSED: " + log);
  }
  std::vector<uint32_t> wordsA, wordsB;
  if (!worldmap::PackWorldMap(set, real, wordsA, log) || !worldmap::PackWorldMap(set, syn, wordsB, log))
    return fail("pack failed: " + log);

  // The window with the box near its LOW corner (box + feather + 16 in), so
  // the far side holds columns more than feather + TWO crowns from the box:
  // a trunk the clearing refused can reach a crown's width out with its
  // branches, and a low branch reads as a trunk base, so "untouched" starts
  // at feather + 2 x the widest crown. Ground in the lower half.
  const IVec3 savedOrigin = c.world.WindowOrigin();
  auto floorDiv16 = [](int v) { return v >= 0 ? v / 16 : -((-v + 15) / 16); };
  const IVec3 origin{floorDiv16(bx0 - kFeather - 16), 0, floorDiv16(bz0 - kFeather - 16)};
  const int wx0 = origin.x * (int)kChunk, wz0 = origin.z * (int)kChunk;
  const int wx1 = wx0 + (int)kWorldN - 1, wz1 = wz0 + (int)kWorldN - 1;

  // The CPU height mirror (4): the clearing moves no column.
  int mirrorMoved = 0;
  std::vector<int> hv(size_t(kWorldN) * kWorldN);
  for (int z = wz0; z <= wz1; z++)   // the real map's heights, the whole window
    for (int x = wx0; x <= wx1; x++) hv[size_t(z - wz0) * kWorldN + size_t(x - wx0)] = World::TerrainHeight(x, z, kDefaultSeed);
  auto H = [&](int x, int z) { return hv[size_t(z - wz0) * kWorldN + size_t(x - wx0)]; };
  worldmap::SetCurrentWorldMap(syn);
  for (int z = bz0 - kFeather; z <= bz1 + kFeather; z += 3)
    for (int x = bx0 - kFeather; x <= bx1 + kFeather; x += 3)
      if (World::TerrainHeight(x, z, kDefaultSeed) != H(x, z)) mirrorMoved++;
  worldmap::SetCurrentWorldMap(real);

  // One arm: upload, regenerate, and measure.
  struct Arm {
    long long boxTree = 0, coverIn = 0;
    int trunksFar = 0, trunksBand = 0;
    std::set<std::pair<int, int>> farTrunks;
    std::vector<uint32_t> ground;   // words y in [h-8, h] over box + band, row-major
  };
  c.stream.OnRegen();
  c.world.SetWindowOrigin(origin);
  const int farD = kFeather + 2 * maxReach + 16;   // past this a trunk base is untouched
  auto run = [&](const std::vector<uint32_t>& words, const worldmap::WorldMapData& m, Arm& a) {
    worldmap::SetCurrentWorldMap(m);
    c.ctx.WaitIdle();
    c.sim.UploadEnvironment(c.ctx.device, c.ctx.queue, atlas, words);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    RowRead rd{&c, {}};
    for (int z = wz0; z <= wz1; z++)
      for (int x = wx0; x <= wx1; x++) {
        const int h = H(x, z);
        const int d = BoxDist(x, z, bx0, bz0, bx1, bz1);
        if (d == 0) {
          for (int y = h - 8; y < (int)kWorldN; y++) {
            const uint32_t mat = rd.Mat(x, y, z);
            if (isTree(mat)) a.boxTree++;
            else if (y > h && y <= h + 6 && mat != 0) a.coverIn++;
          }
        }
        // A trunk base: bark in the first voxels over the ground.
        bool trunk = false;
        for (int y = h + 1; y <= h + 2 && !trunk; y++) trunk = isBark(rd.Mat(x, y, z));
        if (trunk && d > farD) { a.trunksFar++; a.farTrunks.insert({x, z}); }
        if (trunk && d > 0 && d <= kFeather + maxReach) a.trunksBand++;
        if (d <= kFeather)
          for (int y = h - 8; y <= h; y++) a.ground.push_back(rd.Word(x, y, z));
      }
  };
  Arm A, B;
  run(wordsA, real, A);
  run(wordsB, syn, B);
  // 4. terrain, voxel by voxel (a tree's own matter excepted)
  size_t groundDiff = 0;
  for (size_t i = 0; i < std::min(A.ground.size(), B.ground.size()); i++)
    if (A.ground[i] != B.ground[i] && !isTree(A.ground[i] & 0xFFFu) && !isTree(B.ground[i] & 0xFFFu)) groundDiff++;
  int farKept = 0;
  for (const auto& p : A.farTrunks) farKept += B.farTrunks.count(p) ? 1 : 0;

  // ---- the pristine world back -------------------------------------------------------
  {
    biomes::EnvironmentStamp stamp;
    std::string l;
    if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, l)) {
      worldmap::SetCurrentWorldMap(real);
      bad.push_back("ReloadEnvironment REFUSED afterwards: " + l);
    }
    c.stream.OnRegen();
    c.world.SetWindowOrigin(savedOrigin);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    DrainFullRefill(c.ctx, c.world, c.sim, IVec3{108 >> 4, 122 >> 4, 108 >> 4});
  }

  if (A.boxTree == 0) bad.push_back("1: the chosen spot has no tree in the box even without the clearing -- it proves nothing");
  if (B.boxTree != 0) bad.push_back(Format("1: %lld tree-material voxels inside the clearing's box", B.boxTree));
  if (A.trunksFar == 0 || farKept * 10 < A.trunksFar * 9)
    bad.push_back(Format("2: past feather + two widest crowns (%d vox) only %d of %d trunk bases survive the clearing",
                         farD, farKept, A.trunksFar));
  if (B.trunksBand > A.trunksBand) bad.push_back(Format("2: the feather band has MORE trunk columns with the clearing (%d > %d)", B.trunksBand, A.trunksBand));
  if (B.coverIn == 0) bad.push_back("3: no ground cover grows inside the clearing");
  if (groundDiff != 0 || A.ground.size() != B.ground.size() || mirrorMoved != 0)
    bad.push_back(Format("4: the terrain moved: %zu ground voxels differ, %d mirror columns", groundDiff, mirrorMoved));

  detail = Format("box (%d,%d)..(%d,%d) feather %d at a forest spot near the spawn, widest crown %d | tree voxels in the "
                  "box: %lld without, %lld with | trunk columns past %d vox: %d without, %d with (%d the same) | "
                  "trunk columns within feather + crown of the box: %d without, %d with | cover voxels in the box: "
                  "%lld without, %lld with | ground voxels differing: %zu of %zu, mirror columns moved %d",
                  bx0, bz0, bx1, bz1, kFeather, maxReach, A.boxTree, B.boxTree, farD, A.trunksFar, B.trunksFar, farKept,
                  A.trunksBand, B.trunksBand, A.coverIn, B.coverIn, groundDiff, A.ground.size(), mirrorMoved);
  for (const std::string& b : bad) detail += " | FAIL: " + b;
  std::printf("clearing: %s (%s)\n", bad.empty() ? "PASS" : "FAIL", detail.c_str());
  return bad.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& ClearingGates() {
  static const std::vector<Gate> g = {
      {"clearing", "sim", {}, false, GateClearing, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
