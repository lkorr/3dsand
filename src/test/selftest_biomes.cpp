// selftest_biomes.cpp — the authored biome and water-body files, before the
// world is asked to grow them.
//
// WHAT THIS GUARDS. The tuner's Environment tab writes three kinds of file the
// engine reads or will read: assets/biomes/<name>.json (which species, water
// presets, cover and caves a biome has), assets/water/<name>.json (the shape of
// a body of water) and the placement.biomes mirror inside assets/trees/*.json
// (the per-biome tree weights the .svtree bake actually consumes). Each of
// them can be wrong in a way that no gate downstream would name:
//
//   * a biome naming a species with no atlas   -> a biome that grows nothing
//   * a species mirror the biome files disagree with -> the forest the engine
//     grows is not the forest the page shows, and `tree-atlas` cannot tell
//   * a biome file whose `index` is not worldgen's id for that name -> the
//     authoring surface lies about which biome it authors
//   * a preset naming a material that does not exist -> reeds that become air
//
// Pure CPU, no world, no GPU, nothing left behind, so it runs with the other
// cheap front-loaded checks. The JS twin is `node scripts/test_environment.mjs`
// (which additionally asserts the generators' determinism); this is the one
// that fails a `--selftest` run, because the engine is the consumer.

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/biomes.h"
#include "sim/treeatlas.h"
#include "sim/tuning.h"
#include "sim/world.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"

namespace selftest {
namespace {

Status GateBiomes(Ctx& c, std::string& detail) {
  biomes::BiomeSet set;
  std::string log;
  const std::string dir = sandvox::AssetDir();
  if (!biomes::LoadBiomeSet(dir, c.mats, set, log)) {
    detail = "a file did not parse: " + log;
    std::printf("biomes: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  std::vector<std::string> problems;
  const int n = biomes::ValidateBiomeSet(set, problems);
  for (const std::string& p : problems) std::printf("biomes:   %s\n", p.c_str());

  int rows = 0;
  for (const auto& b : set.biomes) rows += (int)(b.cover.size() + b.trees.size() + b.water.size() + b.caves.size());
  char buf[256];
  std::snprintf(buf, sizeof buf, "%zu biomes (%d engine), %zu water presets, %zu species mirrors, %d feature rows, %d problem%s",
                set.biomes.size(),
                [&] { int e = 0; for (int i = 0; i < biomes::kEngineBiomeCount; i++) e += biomes::BiomeById(set, i) != nullptr; return e; }(),
                set.water.size(), set.species.size(), rows, n, n == 1 ? "" : "s");
  detail = buf;
  std::printf("biomes: %s (%s)\n", n ? "FAIL" : "PASS", detail.c_str());
  return n ? Status::Fail : Status::Pass;
}

// ---- worldmap -------------------------------------------------------------
// The painted map reaches the kernel. Three claims, cheapest first:
//   A. a map is loaded (a world with no map refuses to start, so this is
//      "the harness went through the same door main.cpp does");
//   B. the CPU twin World::MapBiomeAt returns the plane's own cell at cell
//      CENTRES -- the seeded boundary warp is at most cell/4 (the loader
//      refuses more), so a centre sample can never cross into a neighbour,
//      and a twin that disagreed here would be reading the wrong plane;
//   C. the GPU agrees with the twin: at columns inside the residency window
//      the voxel AT ground level is the skin material of the biome the twin
//      names. Ground skin is the first thing genCellIn takes from the record,
//      so a kernel reading a different biome shows a different skin. Runs
//      after `terrain` (the world is generated and pristine at the origin).
//   D. SITES YOU CAN TRUST (P6 of docs/PLAN_map_overhaul.md), CPU: the loaded
//      map's own map.json plus synthetic sites, loaded through LoadWorldMap's
//      in-memory door -- an unknown kind, two lakes whose footprints meet and
//      a tree standing in one are WARNINGS, the load still succeeds; a tree
//      site resolves to the atlas's species index and is listed in its cell;
//      its keep-out is its trunk, not its cell; five sites in one cell are a
//      load ERROR, not a silently dropped site.
//   E. the same tree site on the GPU: regenerated with the synthetic map, the
//      first cell of its trunk column above the ground is the atlas's own
//      voxel for that species / variant 0 / no turn. The real environment is
//      reloaded and regenerated afterwards (the env-truth gate's discipline).
bool SiteCases(Ctx& c, const biomes::BiomeSet& set, std::string& out) {
  using sandvox::kDefaultSeed;
  using sandvox::ReadVoxelsSync;
  const std::string dir = sandvox::AssetDir();
  const worldmap::WorldMapData real = worldmap::CurrentWorldMap();   // a COPY
  std::vector<std::string> bad;
  auto say = [&](bool okk, const std::string& what) { if (!okk) bad.push_back(what); };

  nlohmann::json base;
  {
    std::ifstream f(dir + "/worldmap/" + real.name + "/map.json");
    try { f >> base; } catch (const std::exception& e) {
      out = "D: " + real.name + "/map.json did not parse: " + e.what();
      return false;
    }
  }
  std::vector<TreeSpeciesHeader> species;
  std::string log;
  if (!ReadTreeSpeciesHeaders(dir + "/trees", species, log) || species.empty() || set.water.empty()) {
    out = "D: no tree species / water preset to place (" + log + ")";
    return false;
  }
  const TreeSpeciesHeader* sp = &species[0];
  for (const TreeSpeciesHeader& h : species) if (h.name == "oak") sp = &h;

  // The tree's column: in the window, below the treeline, dry (the C sweep).
  const IVec3 org = c.world.WindowOrigin();
  const int treeline = worldmap::CurrentTerrain().treeline;
  int tx = 0, tz = 0, th = 0;
  bool found = false;
  for (int i = 0; i < 12 && !found; i++) {
    const int x = org.x * (int)kChunk + 64 + i * 37;
    const int z = org.z * (int)kChunk + 300 + i * 11;
    const int h = World::TerrainHeight(x, z, kDefaultSeed);
    const World::PondQuery pq = World::PondNearColumn(x, z, kDefaultSeed);
    if (h >= treeline - 1 || pq.inDisc || pq.near) continue;
    if (!c.world.ChunkInWindow(IVec3{x >> 4, (h + 1) >> 4, z >> 4})) continue;
    tx = x; tz = z; th = h; found = true;
  }
  if (!found) { out = "D: no dry in-window column below the treeline for the tree site"; return false; }

  // D. The synthetic sites, far from anything the harness authored except the tree.
  const std::string preset = set.water[0].name;
  nlohmann::json j = base;
  nlohmann::json& sites = j["sites"];
  sites.push_back({{"id", "p6_unknown"}, {"kind", "volcano"}, {"at", {9000, 9000}}});
  sites.push_back({{"id", "p6_lake_a"}, {"kind", "water"}, {"preset", preset}, {"at", {12000, 12000}}, {"radius", 60}});
  sites.push_back({{"id", "p6_lake_b"}, {"kind", "water"}, {"preset", preset}, {"at", {12090, 12000}}, {"radius", 60}});
  sites.push_back({{"id", "p6_wet_tree"}, {"kind", "tree"}, {"species", sp->name}, {"at", {12000, 12010}}});
  sites.push_back({{"id", "p6_tree"}, {"kind", "tree"}, {"species", sp->name}, {"at", {tx, tz}},
                   {"variant", 0}, {"rot", 0}});
  // Off the pad box (the in-window tree may stand on it, where everything is
  // keep-out anyway): the keep-out claim is asked here.
  sites.push_back({{"id", "p6_lone_tree"}, {"kind", "tree"}, {"species", sp->name}, {"at", {20000, 5000}}});
  worldmap::WorldMapData syn;
  std::string text = j.dump();
  const bool loaded = worldmap::LoadWorldMap(dir, real.name, set, c.mats.size(), kDefaultSeed, syn, log, &text);
  say(loaded, "D: the synthetic map was REFUSED: " + log);
  auto warned = [&](const std::string& a, const std::string& b) {
    for (const std::string& w : syn.warnings)
      if (w.find(a) != std::string::npos && w.find(b) != std::string::npos) return true;
    return false;
  };
  const worldmap::WorldMapData::StampSite* tree = nullptr;
  const worldmap::WorldMapData::StampSite* lone = nullptr;
  for (const auto& s : syn.sites) {
    if (s.id == "p6_tree") tree = &s;
    if (s.id == "p6_lone_tree") lone = &s;
  }
  if (loaded) {
    say(warned("p6_unknown", "unknown kind"), "D: no warning for the unknown kind");
    say(warned("p6_lake_a", "p6_lake_b"), "D: no overlap warning for the two lakes");
    say(warned("p6_wet_tree", "p6_lake_a"), "D: no overlap warning for the tree in the lake");
    say(tree && tree->kind == worldmap::kSiteTree && tree->species == sp->index && tree->variant == 1 &&
            tree->rot == 0 && tree->radius == sp->reach,
        "D: the tree site did not resolve to " + sp->name + " (index " + std::to_string(sp->index) + ")");
  }
  int listed = 0;
  bool keepIn = false, keepOut = true;
  if (tree && lone) {
    int cx = 0, cz = 0;
    syn.CellOf(tree->x, tree->z, &cx, &cz);
    const uint32_t* lst = syn.Inside(cx, cz) ? syn.SiteList(cx, cz) : nullptr;
    for (uint32_t k = 0; lst && k < lst[0]; k++)
      if (syn.sites[lst[1 + k] - 1].id == "p6_tree") listed++;
    worldmap::SetCurrentWorldMap(syn);
    keepIn = worldmap::SiteKeepOut(lone->x + 1, lone->z - 1);
    const int past = static_cast<int>(worldmap::kSiteTreeKeepOut) + 2;
    keepOut = worldmap::SiteKeepOut(lone->x + past, lone->z);
    worldmap::SetCurrentWorldMap(real);
    say(listed == 1, "D: the tree site is listed " + std::to_string(listed) + "x in its own cell");
    say(keepIn && !keepOut, "D: the tree's keep-out is not its trunk square");
  } else if (loaded) {
    bad.push_back("D: a tree site did not load");
  }
  // Overflow: kSiteCellMax + 1 sites reaching one cell refuse the load,
  // naming the cell. (Was five against a cap of 4; the world editor's P4
  // raised the cap to 32 for a village, so the crowd follows the constant.)
  {
    nlohmann::json jo = base;
    const int crowd = static_cast<int>(worldmap::kSiteCellMax) + 1;
    for (int k = 0; k < crowd; k++)
      jo["sites"].push_back({{"id", "p6_crowd_" + std::to_string(k)}, {"kind", "tree"}, {"species", sp->name},
                             {"at", {14400 + k * 20, 15000}}});
    worldmap::WorldMapData over;
    std::string olog, ot = jo.dump();
    const bool refused = !worldmap::LoadWorldMap(dir, real.name, set, c.mats.size(), kDefaultSeed, over, olog, &ot);
    say(refused && olog.find("at most") != std::string::npos,
        "D: " + std::to_string(crowd) + " sites in one cell did not refuse the load (" + olog + ")");
  }

  // E. The tree on the GPU.
  uint32_t want = 0, got = 0;
  int wantLy = -1;
  if (loaded && tree) {
    TreeAtlas atlas;
    std::vector<uint32_t> words;
    if (!LoadTreeAtlas(dir + "/trees", c.mats, set, atlas, log) || !worldmap::PackWorldMap(set, syn, words, log)) {
      bad.push_back("E: atlas / pack failed: " + log);
    } else {
      // The atlas's own trunk column: variant 0, no turn, so the trunk
      // (0, 0) is the variant's anchor column; its first non-air cell.
      const uint32_t* W = atlas.words.data();
      const uint32_t* s = W + W[treeatlas::kHSpeciesDir] + sp->index * treeatlas::kSpeciesWords;
      const uint32_t* d = W + s[treeatlas::kSVariantDir];
      const int ax = static_cast<int>(d[treeatlas::kVAnchorX]), az = static_cast<int>(d[treeatlas::kVAnchorZ]);
      for (int ly = 0; ly < 8 && wantLy < 0; ly++) {
        const uint32_t v = TreeAtlasCellAt(atlas, sp->index, 0, ax, ly, az);
        if (v) { want = v; wantLy = ly; }
      }
      worldmap::SetCurrentWorldMap(syn);
      c.ctx.WaitIdle();
      c.sim.UploadEnvironment(c.ctx.device, c.ctx.queue, atlas, words);
      sandvox::SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
      c.ctx.WaitIdle();
      const int y = th + 1 + std::max(wantLy, 0);
      const IVec3 cc{tx >> 4, y >> 4, tz >> 4};
      if (c.world.ChunkInWindow(cc)) {
        std::vector<uint32_t> chunk(kChunkVol, 0);
        ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex(cc), 1, chunk.data(), "worldmap");
        got = chunk[((uint32_t)(tz & 15) * kChunk + (uint32_t)(y & 15)) * kChunk + (uint32_t)(tx & 15)] & 0xFFFu;
      }
      say(wantLy >= 0 && got == want, "E: the tree site's trunk cell is " +
          (got < c.mats.size() ? c.mats[got].name : std::string("?")) + ", the atlas says " +
          (want < c.mats.size() ? c.mats[want].name : std::string("?")));
      biomes::EnvironmentStamp stamp;
      if (!sandvox::ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, log)) {
        worldmap::SetCurrentWorldMap(real);
        bad.push_back("E: ReloadEnvironment REFUSED afterwards: " + log);
      }
      sandvox::SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
      // ...and the far cascades refilled from it, the `sculpt` gate's exit:
      // the synthetic tree reached them through the sieve too.
      DrainFullRefill(c.ctx, c.world, c.sim, IVec3{108 >> 4, 122 >> 4, 108 >> 4});
    }
  }
  char buf[400];
  std::snprintf(buf, sizeof buf,
                "sites: synthetic load %s with %zu warnings, %s tree at (%d,%d) h %d listed %d, keep-out trunk %s / "
                "past %s, GPU trunk cell %s (atlas %s)",
                loaded ? "ok" : "REFUSED", syn.warnings.size(), sp->name.c_str(), tx, tz, th, listed,
                keepIn ? "yes" : "NO", keepOut ? "YES" : "no",
                got < c.mats.size() ? c.mats[got].name.c_str() : "?", want < c.mats.size() ? c.mats[want].name.c_str() : "?");
  out = buf;
  for (const std::string& b : bad) out += "; " + b;
  return bad.empty();
}

Status GateWorldMap(Ctx& c, std::string& detail) {
  using sandvox::kDefaultSeed;
  using sandvox::ReadVoxelsSync;
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (!m.Loaded()) {
    detail = "no world map loaded (world.mapLayer = " + CurrentTuning().world.mapLayer + ")";
    std::printf("worldmap: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  biomes::BiomeSet set;
  std::string log;
  if (!biomes::LoadBiomeSet(sandvox::AssetDir(), c.mats, set, log)) {
    detail = "biome files did not load: " + log;
    std::printf("worldmap: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }

  // B: sixteen cell centres spread over the plane.
  int centreOk = 0, centreN = 0;
  const int half = 1 << (m.cellLog2 - 1);
  for (int k = 0; k < 16; k++) {
    const int cx = (m.width * (k % 4) + m.width / 8) / 4;
    const int cz = (m.height * (k / 4) + m.height / 8) / 4;
    const int x = ((cx - m.originCellX) << m.cellLog2) + half;
    const int z = ((cz - m.originCellZ) << m.cellLog2) + half;
    centreN++;
    if (World::MapBiomeAt(x, z, kDefaultSeed) == m.BiomeCell(cx, cz)) centreOk++;
  }

  // C: the skin at ground level, at columns inside the window, off the
  // fixture pads / arena / pools (all near the x==z diagonal or past x 350).
  const IVec3 org = c.world.WindowOrigin();
  int skinOk = 0, skinN = 0, skipped = 0;
  std::string first;
  const int treeline = worldmap::CurrentTerrain().treeline;
  for (int i = 0; i < 6; i++) {
    const int x = org.x * (int)kChunk + 40 + i * 52;
    const int z = org.z * (int)kChunk + 400 + i * 13;
    const int h = World::TerrainHeight(x, z, kDefaultSeed);
    if (h >= treeline) { skipped++; continue; }   // snow cap, not the skin
    const IVec3 cc{x >> 4, h >> 4, z >> 4};
    if (!c.world.ChunkInWindow(cc)) { skipped++; continue; }
    std::vector<uint32_t> chunk(kChunkVol, 0);
    ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex(cc), 1, chunk.data(), "worldmap");
    const uint32_t local = ((uint32_t)(z & 15) * kChunk + (uint32_t)(h & 15)) * kChunk + (uint32_t)(x & 15);
    const uint32_t mat = chunk[local] & 0xFFFu;
    const uint32_t b = World::MapBiomeAt(x, z, kDefaultSeed);
    const biomes::BiomeDef* def = biomes::BiomeById(set, (int)b);
    const uint32_t want = def ? def->skinId : 0;
    skinN++;
    if (mat == want) skinOk++;
    else if (first.empty())
      first = " first mismatch at (" + std::to_string(x) + "," + std::to_string(z) + ") h " +
              std::to_string(h) + ": voxel " + (mat < c.mats.size() ? c.mats[mat].name : "?") +
              " but twin says biome " + (def ? def->name : "?") + " (skin " +
              (want < c.mats.size() ? c.mats[want].name : "?") + ")";
  }
  const std::string mapName = m.name;   // SiteCases replaces the current map for a while
  std::string p6;
  const bool p6ok = SiteCases(c, set, p6);
  const bool ok = centreOk == centreN && skinN >= 3 && skinOk == skinN && p6ok;
  char buf[320];
  std::snprintf(buf, sizeof buf,
                "map '%s' %dx%d @%d vox, %zu palette, content %08x; twin at cell centres %d/%d; "
                "GPU skin == twin's biome skin %d/%d (%d skipped)%s",
                mapName.c_str(), m.width, m.height, 1 << m.cellLog2, m.palette.size(),
                m.contentHash, centreOk, centreN, skinOk, skinN, skipped, first.c_str());
  detail = std::string(buf) + "; " + p6;
  std::printf("worldmap: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- spawn-site -----------------------------------------------------------
// The GAME's map's (world.mapLayer, not the harness map the gates run on)
// kind "spawn" site is somewhere a player can start
// (docs/PLAN_environment_truth.md P-C). Five claims, all CPU (the height
// mirror and that map), nothing left behind:
//   A. the map AUTHORED a spawn (not the loader's (140,140) default);
//   B. it is outside any pad box and every site's footprint (a stamp's pad,
//      a lake's disc + shore band, a tree's trunk) -- i.e. siteKeepOut(spawn)
//      is false, so trees, tarns and cover may grow there;
//   C. the ground there is above the map's sea level;
//   D. it is not under a tarn (World::PondNearColumn);
//   E. the biome there is not the ocean.
// The distance from the pad box and the calm-area residual
// (ground - spawnPlainY) are reported, not asserted: the crown reach that
// decides how close the forest gets is the atlas's, and the residual is the
// fine octaves plus the wedge by design.
Status GateSpawnSite(Ctx& c, std::string& detail) {
  using sandvox::kDefaultSeed;
  // THE GAME'S MAP, not the loaded one. Gates generate the harness map
  // (sim/worlddefaults.h); this gate validates the map a player starts on,
  // world.mapLayer, so it loads that one and points the CPU twins at it for
  // the duration (all its claims are CPU), then puts the harness map back.
  const std::string gameMap = CurrentTuning().world.mapLayer;
  biomes::BiomeSet set;
  std::string log;
  const bool haveSet = biomes::LoadBiomeSet(sandvox::AssetDir(), c.mats, set, log);
  worldmap::WorldMapData game;
  if (!haveSet || !worldmap::LoadWorldMap(sandvox::AssetDir(), gameMap, set, c.mats.size(),
                                          kDefaultSeed, game, log) || !game.Loaded()) {
    detail = "the game's map '" + gameMap + "' did not load: " + log;
    std::printf("spawn-site: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  const worldmap::WorldMapData loaded = worldmap::CurrentWorldMap();   // a COPY, restored below
  worldmap::SetCurrentWorldMap(std::move(game));
  struct Restore {
    const worldmap::WorldMapData& m;
    ~Restore() { worldmap::SetCurrentWorldMap(m); }
  } restore{loaded};
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  const int sx = m.spawnX, sz = m.spawnZ;
  const int h = World::TerrainHeight(sx, sz, kDefaultSeed);
  const World::PondQuery pq = World::PondNearColumn(sx, sz, kDefaultSeed);
  const bool inBox = World::InPadBox(sx, sz);
  int cx = 0, cz = 0;
  m.CellOf(sx, sz, &cx, &cz);
  const uint32_t* lst = m.Inside(cx, cz) ? m.SiteList(cx, cz) : nullptr;
  const int siteCount = lst ? (int)lst[0] : 0;
  const bool keepOut = worldmap::SiteKeepOut(sx, sz);
  const uint32_t b = World::MapBiomeAt(sx, sz, kDefaultSeed);
  const biomes::BiomeDef* def = haveSet ? biomes::BiomeById(set, (int)b) : nullptr;
  const std::string bname = def ? def->name : ("id " + std::to_string(b));
  const int boxDist = std::max(std::max(std::max(m.padX0 - sx, sx - m.padX1),
                                        std::max(m.padZ0 - sz, sz - m.padZ1)), 0);
  std::string why;
  if (!m.spawnAuthored) why += "; map.json sites[] has no kind \"spawn\" (loader defaulted)";
  if (inBox) why += "; inside the map's pad box";
  // A site's CELLS are not a keep-out since P6 (its footprint is: a stamp's
  // pad, a lake's disc + band -- its shore included --, a tree's trunk), so
  // the claim is the same siteKeepOut every generated feature reads.
  if (keepOut && !inBox) {
    std::string whose;
    for (int k = 0; k < siteCount; k++) {
      const worldmap::WorldMapData::StampSite& s = m.sites[lst[1 + k] - 1];
      whose += (whose.empty() ? "\"" : ", \"") + s.id + "\"";
    }
    why += "; inside the footprint of a site in its cell (" + whose + ")";
  }
  if (h <= m.seaLevelY) why += "; ground y" + std::to_string(h) + " is under seaLevelY " + std::to_string(m.seaLevelY);
  if (pq.inDisc) why += "; under a tarn (surface y" + std::to_string(pq.surf) + ")";
  if ((int)b == m.oceanBiome) why += "; biome is the ocean";
  const bool ok = why.empty();
  char buf[320];
  std::snprintf(buf, sizeof buf,
                "map '%s': spawn (%d,%d) on %s: ground y%d (sea y%d, home y%d), %d vox past the pad box, "
                "%d site%s in its cell, keep-out %s, tarn %s%s",
                m.name.c_str(), sx, sz, bname.c_str(), h, m.seaLevelY, worldmap::CurrentTerrain().homeY,
                boxDist, siteCount, siteCount == 1 ? "" : "s", keepOut ? "YES" : "no",
                pq.inDisc ? "YES" : (pq.near ? "near" : "no"), why.c_str());
  detail = buf;
  std::printf("spawn-site: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- env-reload -----------------------------------------------------------
// The hot path an Environment-tab save takes (docs/PLAN_environment_truth.md
// P-A): a biome table edited AFTER boot reaches the next worldgen. Three
// claims on one in-window forest column:
//   A. the pristine world has the forest's authored skin at ground level;
//   B. with the forest's skin swapped IN MEMORY (no file touched), packed and
//      pushed through Simulation::UploadEnvironment, a regen shows the swap --
//      the kernel read the new table, not a cached one;
//   C. ReloadEnvironment (the real F7 / Apply / --voxserve RELOAD call) reads
//      the files back and a regen shows the authored skin again -- which is
//      also what leaves the suite the pristine world it had.
Status GateEnvReload(Ctx& c, std::string& detail) {
  using sandvox::kDefaultSeed;
  using sandvox::ReadVoxelsSync;
  using sandvox::ReloadEnvironment;
  using sandvox::SubmitWorldgen;
  const std::string dir = sandvox::AssetDir();
  biomes::BiomeSet set;
  std::string log;
  if (!biomes::LoadBiomeSet(dir, c.mats, set, log)) {
    detail = "biome files did not load: " + log;
    std::printf("env-reload: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (!m.Loaded()) {
    detail = "no world map loaded";
    std::printf("env-reload: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }

  // The column: in-window, below the treeline, whose twin biome has a skin
  // that is not what we will swap it to. Same sweep the worldmap gate uses.
  const IVec3 org = c.world.WindowOrigin();
  const int treeline = worldmap::CurrentTerrain().treeline;
  int cx = 0, cz = 0, ch = 0;
  biomes::BiomeDef* target = nullptr;
  for (int i = 0; i < 12 && !target; i++) {
    const int x = org.x * (int)kChunk + 40 + i * 52;
    const int z = org.z * (int)kChunk + 400 + i * 13;
    const int h = World::TerrainHeight(x, z, kDefaultSeed);
    if (h >= treeline) continue;
    if (!c.world.ChunkInWindow(IVec3{x >> 4, h >> 4, z >> 4})) continue;
    const int b = (int)World::MapBiomeAt(x, z, kDefaultSeed);
    for (biomes::BiomeDef& d : set.biomes)
      if (d.index == b && d.skinId != 0) { target = &d; cx = x; cz = z; ch = h; }
  }
  if (!target) {
    detail = "no in-window column with a skinned biome below the treeline";
    std::printf("env-reload: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  // The swap material: the biome's own subsoil if it differs, else stone (1).
  const uint32_t authored = target->skinId;
  const uint32_t swapped = (target->subsoilId && target->subsoilId != authored) ? target->subsoilId : 1u;
  auto name = [&](uint32_t id) { return id < c.mats.size() ? c.mats[id].name : std::string("?"); };
  auto skinAt = [&]() -> uint32_t {
    const IVec3 cc{cx >> 4, ch >> 4, cz >> 4};
    std::vector<uint32_t> chunk(kChunkVol, 0);
    ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex(cc), 1, chunk.data(), "env-reload");
    const uint32_t local = ((uint32_t)(cz & 15) * kChunk + (uint32_t)(ch & 15)) * kChunk + (uint32_t)(cx & 15);
    return chunk[local] & 0xFFFu;
  };

  // A.
  const uint32_t before = skinAt();
  // B. In-memory edit, packed, uploaded, regenerated.
  target->skinId = swapped;
  std::vector<uint32_t> words;
  TreeAtlas atlas;
  if (!worldmap::PackWorldMap(set, m, words, log) ||
      !LoadTreeAtlas(dir + "/trees", c.mats, set, atlas, log)) {
    detail = "pack failed: " + log;
    std::printf("env-reload: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  c.ctx.WaitIdle();
  c.sim.UploadEnvironment(c.ctx.device, c.ctx.queue, atlas, words);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  const uint32_t during = skinAt();
  // C. The real reload, from disk.
  biomes::EnvironmentStamp stamp;
  const bool reloaded = ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, log);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  const uint32_t after = skinAt();

  const bool ok = reloaded && before == authored && during == swapped && after == authored;
  char buf[400];
  std::snprintf(buf, sizeof buf,
                "%s column (%d,%d) h %d: skin %s -> uploaded %s -> reloaded %s (authored %s, swap %s)%s%s",
                target->name.c_str(), cx, cz, ch, name(before).c_str(), name(during).c_str(),
                name(after).c_str(), name(authored).c_str(), name(swapped).c_str(),
                reloaded ? "" : "; ReloadEnvironment REFUSED: ", reloaded ? "" : log.c_str());
  detail = buf;
  std::printf("env-reload: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& BiomeGates() {
  static const std::vector<Gate> g = {
      {"biomes", "sim", {}, false, GateBiomes},
      {"worldmap", "sim", {}, false, GateWorldMap},
      {"spawn-site", "sim", {}, false, GateSpawnSite},
      {"env-reload", "sim", {}, false, GateEnvReload},
  };
  return g;
}

}  // namespace selftest
