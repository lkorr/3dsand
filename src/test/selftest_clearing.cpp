// selftest_clearing.cpp — the FOREST CLEARING site kind (map.json kind
// "clearing", worldmap.h kSiteClearing, worldgen.wgsl clearingRefuses), and
// a house in the forest WITHOUT one (structure-ground, below: the per-tree
// building rule, the soft pad, the `soften` site kind).
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


// ---- structure-ground ------------------------------------------------------------------
// A house in the forest with no clearing: the PER-TREE BUILDING RULE and the
// SOFT PAD (worldmap.h "STAMPS carry their FOOTPRINT RECT", kSiteSoften).
// Three arms on the harness map, same window, over forest near its spawn:
//   A  the map as it is;
//   S  + a `soften` box round the spot (hills 40 %, bumps 20 %, grain 0);
//   B  S + one structure (samples/smithy) whose floor sits a few voxels
//      above the softened ground, padMargin 30, padApron 8.
// Asserted:
//   1. S has tree voxels within kStampTreeClear of the house rect (else the
//      spot proves nothing) and B has NONE there, at any height -- no crown
//      clips or overhangs the house;
//   2. trunk bases in S further than the widest crown + the clearance from the
//      rect still stand in B (>= 90 %; a refused tree's low branch can read as a
//      trunk base a crown out), and B keeps at least one trunk inside that
//      band -- the rule is per tree, not a clearing;
//   3. the pad: every column of rect + apron is at the floor's ground top
//      (pos.y - 1); across the ramp every column lies between S's ground and
//      the floor, and the ramp has no MORE 4-neighbour steps over 1 voxel
//      than S's ground had there (it adds no step to climb); past the ramp
//      B's ground equals S's; the GPU's ground top is the CPU mirror's there;
//   4. the soften box: its roughness (mean |h - 5x5 box mean|) in S is at
//      most structureGround.roughRatio (tests/baseline.json, 0.8) of A's, and
//      its columns stepping over 1 voxel at most structureGround.stepRatio
//      (0.5) of A's -- on the harness hillside, where the landform the box
//      leaves alone keeps some of both.
// Exit: the override cleared, the environment reloaded from disk, the window
// put back, regenerated, the far field refilled -- the `clearing` exit.
Status GateStructureGround(Ctx& c, std::string& detail) {
  const std::string dir = AssetDir();
  std::vector<std::string> bad;
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("structure-ground: FAIL (%s)\n", detail.c_str());
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
  std::vector<uint8_t> body(c.mats.size() + 1, 0);   // the terrain's own body materials
  for (size_t i = 0; i < c.mats.size(); i++)
    for (const char* n : {"grass", "dirt", "stone", "gravel", "sand", "shore_mud", "snow"})
      if (c.mats[i].name == n) body[i] = 1;
  auto isBody = [&](uint32_t m) { return m < body.size() && body[m]; };

  // ---- the spot: forest on every side, off the pad, dry, below the treeline ----------
  const int treeline = worldmap::CurrentTerrain().treeline;
  int forestId = -1;
  for (size_t i = 0; i < set.biomes.size(); i++)
    if (set.biomes[i].name == "forest") forestId = set.biomes[i].index;
  if (forestId < 0) return fail("no biome named forest in the set");
  int cx = 0, cz = 0;
  bool found = false;
  for (int ring = 1; ring < 24 && !found; ring++)
    for (int k = 0; k < 8 && !found; k++) {
      static const int dx[8] = {0, 1, 1, 1, 0, -1, -1, -1}, dz[8] = {1, 1, 0, -1, -1, -1, 0, 1};
      const int x = real.spawnX + dx[k] * ring * 300, z = real.spawnZ + dz[k] * ring * 300;
      bool ok = true;
      int lo = 1 << 30, hi = -(1 << 30);
      for (int s = 0; s <= 2 && ok; s++)
        for (int t = 0; t <= 2 && ok; t++) {
          const int px = x - 200 + s * 200, pz = z - 200 + t * 200;
          const int h = World::TerrainHeight(px, pz, kDefaultSeed);
          lo = std::min(lo, h);
          hi = std::max(hi, h);
          ok = (int)World::MapBiomeAt(px, pz, kDefaultSeed) == forestId && h < treeline - 32 &&
               h > real.seaLevelY + 4 && !World::InPadBox(px, pz) && !World::PondNearColumn(px, pz, kDefaultSeed).near;
        }
      (void)lo; (void)hi;
      if (ok) { cx = x; cz = z; found = true; }
    }
  if (!found) return fail("no forest spot near the spawn to build in");

  // ---- the three maps ------------------------------------------------------------------
  constexpr int kSoftHalf = 200, kSoftFeather = 48, kMargin = 30, kApron = 8;
  nlohmann::json js = base;
  js["sites"].push_back({{"id", "sg_soften"}, {"kind", "soften"}, {"min", {cx - kSoftHalf, cz - kSoftHalf}},
                         {"max", {cx + kSoftHalf, cz + kSoftHalf}}, {"feather", kSoftFeather},
                         {"hills", 40}, {"bumps", 20}, {"grain", 0}});
  worldmap::WorldMapData mapS, mapB;
  {
    std::string t = js.dump();
    if (!worldmap::LoadWorldMap(dir, real.name, set, c.mats.size(), kDefaultSeed, mapS, log, &t))
      return fail("the synthetic map (soften) was REFUSED: " + log);
  }
  worldmap::SetCurrentWorldMap(mapS);
  const int groundS = World::TerrainHeight(cx, cz, kDefaultSeed);
  worldmap::SetCurrentWorldMap(real);
  std::vector<worldmap::StructurePlacement> list(1);
  list[0].id = "gate/sg_house";
  list[0].base = "samples/smithy";
  list[0].file = "refs/gate.json";
  list[0].x = cx;
  list[0].z = cz;
  list[0].y = groundS + 1 + 4;   // the floor 4 voxels above the ground: the ramp has work
  list[0].yaw = 0;
  list[0].padMargin = kMargin;
  list[0].padApron = kApron;
  worldmap::SetStructureOverride(&list);
  {
    std::string t = js.dump();
    const bool ok = worldmap::LoadWorldMap(dir, real.name, set, c.mats.size(), kDefaultSeed, mapB, log, &t);
    worldmap::SetStructureOverride(nullptr);
    if (!ok) return fail("the synthetic map (soften + house) was REFUSED: " + log);
  }
  const worldmap::WorldMapData::StampSite* house = nullptr;
  for (const auto& s : mapB.sites)
    if (s.structure && s.id == list[0].id) house = &s;
  if (house == nullptr) return fail("the house was not placed: " + (mapB.warnings.empty() ? log : mapB.warnings[0]));
  const int rx0 = house->bx0, rz0 = house->bz0, rx1 = house->x1, rz1 = house->z1;
  const int padY = list[0].y - 1;
  // The ramp the loader settled on: the authored margin, widened when the
  // ground round the pad is further from the floor than it can ease
  // (worldmap.cpp "A RAMP NEVER STEPS MORE THAN A VOXEL").
  const int margin = house->padMargin;
  std::vector<uint32_t> wordsA, wordsS, wordsB;
  if (!worldmap::PackWorldMap(set, real, wordsA, log) || !worldmap::PackWorldMap(set, mapS, wordsS, log) ||
      !worldmap::PackWorldMap(set, mapB, wordsB, log))
    return fail("pack failed: " + log);
  // Chebyshev and the pad's own octagon distance to the house rect.
  auto cheb = [&](int x, int z) { return BoxDist(x, z, rx0, rz0, rx1, rz1); };
  auto oct = [&](int x, int z) {
    const int gx = std::max(std::max(rx0 - x, x - rx1), 0), gz = std::max(std::max(rz0 - z, z - rz1), 0);
    return std::max(gx, gz) + (std::min(gx, gz) >> 1);
  };

  // The window centred on the house.
  const IVec3 savedOrigin = c.world.WindowOrigin();
  auto floorDiv16 = [](int v) { return v >= 0 ? v / 16 : -((-v + 15) / 16); };
  const IVec3 origin{floorDiv16(cx - (int)kWorldN / 2), 0, floorDiv16(cz - (int)kWorldN / 2)};
  const int wx0 = origin.x * (int)kChunk, wz0 = origin.z * (int)kChunk;
  const int wx1 = wx0 + (int)kWorldN - 1, wz1 = wz0 + (int)kWorldN - 1;
  auto heights = [&](const worldmap::WorldMapData& m, std::vector<int>& hv) {
    worldmap::SetCurrentWorldMap(m);
    hv.assign(size_t(kWorldN) * kWorldN, 0);
    for (int z = wz0; z <= wz1; z++)
      for (int x = wx0; x <= wx1; x++) hv[size_t(z - wz0) * kWorldN + size_t(x - wx0)] = World::TerrainHeight(x, z, kDefaultSeed);
    worldmap::SetCurrentWorldMap(real);
  };
  std::vector<int> hA, hS, hB;
  heights(real, hA);
  heights(mapS, hS);
  heights(mapB, hB);
  auto at = [&](const std::vector<int>& hv, int x, int z) { return hv[size_t(z - wz0) * kWorldN + size_t(x - wx0)]; };

  struct Arm {
    long long clip = 0;                 // tree voxels within the clearance of the rect
    std::set<std::pair<int, int>> trunks;
    long long gpuBad = 0, gpuChecked = 0;
  };
  const int clear = (int)worldmap::kStampTreeClear;
  const int farD = maxReach + clear + 1;
  c.stream.OnRegen();
  c.world.SetWindowOrigin(origin);
  auto run = [&](const std::vector<uint32_t>& words, const worldmap::WorldMapData& m, const std::vector<int>& hv,
                 bool checkGround, Arm& a) {
    worldmap::SetCurrentWorldMap(m);
    c.ctx.WaitIdle();
    c.sim.UploadEnvironment(c.ctx.device, c.ctx.queue, atlas, words);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    RowRead rd{&c, {}};
    for (int z = wz0; z <= wz1; z++)
      for (int x = wx0; x <= wx1; x++) {
        const int h = at(hv, x, z);
        const int d = cheb(x, z);
        if (d < clear)
          for (int y = std::min(h, padY) - 2; y < (int)kWorldN; y++)
            if (isTree(rd.Mat(x, y, z))) a.clip++;
        bool trunk = false;
        for (int y = h + 1; y <= h + 2 && !trunk; y++) trunk = isBark(rd.Mat(x, y, z));
        if (trunk) a.trunks.insert({x, z});
        // The GPU's ground top is the mirror's, over the pad and its ramp
        // (outside the house: inside it, the house's own floor is on top).
        if (checkGround && d > 0 && oct(x, z) <= kApron + margin + 4) {
          a.gpuChecked++;
          if (!isBody(rd.Mat(x, h, z)) || isBody(rd.Mat(x, h + 1, z))) a.gpuBad++;
        }
      }
    worldmap::SetCurrentWorldMap(real);
  };
  Arm S, B;
  run(wordsS, mapS, hS, false, S);
  run(wordsB, mapB, hB, true, B);

  // ---- the pristine world back -------------------------------------------------------
  {
    worldmap::SetStructureOverride(nullptr);
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

  // 1. no crown near the house
  if (S.clip == 0) bad.push_back("1: without the house no tree reaches within the clearance of its rect -- the spot proves nothing");
  if (B.clip != 0) bad.push_back(Format("1: %lld tree-material voxels within %d vox of the house rect", B.clip, clear));
  // 2. trees further out stand; some stand inside the band
  int farS = 0, farKept = 0, nearB = 0;
  for (const auto& p : S.trunks)
    if (cheb(p.first, p.second) > farD && cheb(p.first, p.second) <= farD + 60) {
      farS++;
      farKept += B.trunks.count(p) ? 1 : 0;
    }
  for (const auto& p : B.trunks)
    if (cheb(p.first, p.second) > 0 && cheb(p.first, p.second) <= farD) nearB++;
  if (farS == 0 || farKept * 10 < farS * 9)
    bad.push_back(Format("2: past the widest crown + clearance (%d vox) only %d of %d trunk bases survive the house", farD, farKept, farS));
  if (nearB == 0) bad.push_back(Format("2: no trunk at all within %d vox of the house -- that is a clearing, not a per-tree rule", farD));
  // 3. the pad
  long long coreBad = 0, rampStep = 0, rampStepS = 0, rampOut = 0, beyondBad = 0, rampCols = 0;
  for (int z = wz0 + 1; z < wz1; z++)
    for (int x = wx0 + 1; x < wx1; x++) {
      const int e = oct(x, z);
      const int b = at(hB, x, z), s = at(hS, x, z);
      if (e <= kApron) { coreBad += b != padY ? 1 : 0; continue; }
      if (e < kApron + margin) {
        rampCols++;
        if (b < std::min(s, padY) || b > std::max(s, padY)) rampOut++;
        if (std::abs(at(hB, x + 1, z) - b) > 1 || std::abs(at(hB, x, z + 1) - b) > 1) rampStep++;
        if (std::abs(at(hS, x + 1, z) - s) > 1 || std::abs(at(hS, x, z + 1) - s) > 1) rampStepS++;
        continue;
      }
      if (e < kApron + margin + 40) beyondBad += b != s ? 1 : 0;
    }
  if (coreBad) bad.push_back(Format("3: %lld columns of rect + apron are not at the floor's ground top %d", coreBad, padY));
  // The ramp adds no step a walker has to climb: where the ground it eases
  // into has none, it has none (the harness forest is a hillside, and the
  // soften box leaves the landform alone by design, so S keeps a few).
  if (rampStep > rampStepS)
    bad.push_back(Format("3: the ramp steps more than 1 voxel in %lld columns, the ground without the house in %lld",
                         rampStep, rampStepS));
  if (rampOut) bad.push_back(Format("3: %lld ramp columns are outside [ground, floor]", rampOut));
  if (beyondBad) bad.push_back(Format("3: %lld columns past the ramp differ from the ground without the house", beyondBad));
  if (B.gpuChecked == 0 || B.gpuBad * 100 > B.gpuChecked)
    bad.push_back(Format("3: the GPU's ground top differs from the CPU mirror in %lld of %lld pad columns", B.gpuBad, B.gpuChecked));
  // 4. the soften box
  auto rough = [&](const std::vector<int>& hv, long long* steps) {
    double sum = 0;
    long long n = 0;
    *steps = 0;
    for (int z = cz - kSoftHalf + 2; z <= cz + kSoftHalf - 2; z++)
      for (int x = cx - kSoftHalf + 2; x <= cx + kSoftHalf - 2; x++) {
        if (x - 2 < wx0 || x + 2 > wx1 || z - 2 < wz0 || z + 2 > wz1) continue;
        if (oct(x, z) < kApron + margin + 2) continue;   // the pad is not the soften's
        double m = 0;
        for (int dz = -2; dz <= 2; dz++)
          for (int dx = -2; dx <= 2; dx++) m += at(hv, x + dx, z + dz);
        sum += std::fabs(at(hv, x, z) - m / 25.0);
        n++;
        if (std::abs(at(hv, x + 1, z) - at(hv, x, z)) > 1 || std::abs(at(hv, x, z + 1) - at(hv, x, z)) > 1) (*steps)++;
      }
    return n ? sum / (double)n : 0.0;
  };
  long long stepsA = 0, stepsS = 0;
  const double rA = rough(hA, &stepsA), rS = rough(hS, &stepsS);
  const double ratio = BaselineNumber("structureGround.roughRatio", 0.8);
  const double stepRatio = BaselineNumber("structureGround.stepRatio", 0.5);
  if (!(rA > 0.0 && rS <= rA * ratio))
    bad.push_back(Format("4: the soften box is not smoother: roughness %.3f with it, %.3f without (want <= %.2f x)", rS, rA, ratio));
  if (!(stepsA > 0 && (double)stepsS <= (double)stepsA * stepRatio))
    bad.push_back(Format("4: the soften box keeps its steps: %lld columns step > 1 voxel with it, %lld without (want <= %.2f x)",
                         stepsS, stepsA, stepRatio));
  RecordObserved("structureGround.roughness", rS);

  detail = Format("house samples/smithy rect (%d,%d)..(%d,%d) floor %d (ground %d) apron %d ramp %d (authored %d), widest crown %d | "
                  "tree voxels within %d vox: %lld soft only, %lld with the house | trunks %d..%d vox out: %d kept of %d; "
                  "trunks within %d vox with the house: %d | pad: core off %lld, ramp cols %lld (steps>1 %lld, %lld "
                  "without the house; out of range %lld), past the ramp moved %lld, GPU ground %lld/%lld off | soften: "
                  "roughness %.3f -> %.3f, steps>1 %lld -> %lld",
                  rx0, rz0, rx1, rz1, padY + 1, groundS, kApron, margin, kMargin, maxReach, clear, S.clip, B.clip, farD,
                  farD + 60, farKept, farS, farD, nearB, coreBad, rampCols, rampStep, rampStepS, rampOut, beyondBad, B.gpuBad,
                  B.gpuChecked, rA, rS, stepsA, stepsS);
  for (const std::string& b : bad) detail += " | FAIL: " + b;
  std::printf("structure-ground: %s (%s)\n", bad.empty() ? "PASS" : "FAIL", detail.c_str());
  return bad.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& ClearingGates() {
  static const std::vector<Gate> g = {
      {"clearing", "sim", {}, false, GateClearing, /*needsRender=*/false},
      {"structure-ground", "sim", {}, false, GateStructureGround, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
