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
#include <string>
#include <vector>

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
Status GateWorldMap(Ctx& c, std::string& detail) {
  using sandvox::kDefaultSeed;
  using sandvox::ReadVoxelsSync;
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (!m.Loaded()) {
    detail = "no world map loaded (worldgen.mapLayer = " + CurrentTuning().worldgen.mapLayer + ")";
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
  const int treeline = CurrentTuning().worldgen.treeline;
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
  const bool ok = centreOk == centreN && skinN >= 3 && skinOk == skinN;
  char buf[320];
  std::snprintf(buf, sizeof buf,
                "map '%s' %dx%d @%d vox, %zu palette, content %08x; twin at cell centres %d/%d; "
                "GPU skin == twin's biome skin %d/%d (%d skipped)%s",
                m.name.c_str(), m.width, m.height, 1 << m.cellLog2, m.palette.size(),
                m.contentHash, centreOk, centreN, skinOk, skinN, skipped, first.c_str());
  detail = buf;
  std::printf("worldmap: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- spawn-site -----------------------------------------------------------
// The map's kind "spawn" site is somewhere a player can start
// (docs/PLAN_environment_truth.md P-C). Five claims, all CPU (the height
// mirror and the loaded map), nothing left behind:
//   A. the map AUTHORED a spawn (the loader's (140,140) default is the pad);
//   B. it is outside the harness box, and on no stamp site's cells -- i.e.
//      siteKeepOut(spawn) is false, so trees, tarns and cover may grow there;
//   C. the ground there is above the map's sea level;
//   D. it is not under a tarn (World::PondNearColumn);
//   E. the biome there is not the ocean.
// The distance from the harness box and the calm-area residual
// (ground - spawnPlainY) are reported, not asserted: the crown reach that
// decides how close the forest gets is the atlas's, and the residual is the
// fine octaves plus the wedge by design.
Status GateSpawnSite(Ctx& c, std::string& detail) {
  using sandvox::kDefaultSeed;
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (!m.Loaded()) {
    detail = "no world map loaded (worldgen.mapLayer = " + CurrentTuning().worldgen.mapLayer + ")";
    std::printf("spawn-site: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  biomes::BiomeSet set;
  std::string log;
  const bool haveSet = biomes::LoadBiomeSet(sandvox::AssetDir(), c.mats, set, log);
  const int sx = m.spawnX, sz = m.spawnZ;
  const int h = World::TerrainHeight(sx, sz, kDefaultSeed);
  const World::PondQuery pq = World::PondNearColumn(sx, sz, kDefaultSeed);
  const bool inBox = World::InHarness(sx, sz);
  int cx = 0, cz = 0;
  m.CellOf(sx, sz, &cx, &cz);
  const int siteCell = m.Inside(cx, cz) ? (int)m.SiteCell(cx, cz) : 0;
  const uint32_t b = World::MapBiomeAt(sx, sz, kDefaultSeed);
  const biomes::BiomeDef* def = haveSet ? biomes::BiomeById(set, (int)b) : nullptr;
  const std::string bname = def ? def->name : ("id " + std::to_string(b));
  const int boxDist = std::max(std::max(std::max(m.harnessX0 - sx, sx - m.harnessX1),
                                        std::max(m.harnessZ0 - sz, sz - m.harnessZ1)), 0);
  std::string why;
  if (!m.spawnAuthored) why += "; map.json sites[] has no kind \"spawn\" (loader defaulted)";
  if (inBox) why += "; inside the harness box";
  // A water site's cells are not a keep-out (its DISC + band is, P-F), so a
  // spawn on a lake's cells is fine as long as it is not under the water
  // (pq.inDisc, below) or on its wet fringe.
  const bool waterCell = siteCell && m.sites[static_cast<size_t>(siteCell - 1)].kind == worldmap::kSiteWater;
  if (siteCell && !waterCell) why += "; on stamp site " + std::to_string(siteCell - 1) + "'s cells";
  if (waterCell && pq.near) why += "; on authored lake \"" + m.sites[static_cast<size_t>(siteCell - 1)].id + "\"'s shore";
  if (h <= m.seaLevelY) why += "; ground y" + std::to_string(h) + " is under seaLevelY " + std::to_string(m.seaLevelY);
  if (pq.inDisc) why += "; under a tarn (surface y" + std::to_string(pq.surf) + ")";
  if ((int)b == m.oceanBiome) why += "; biome is the ocean";
  const bool ok = why.empty();
  char buf[320];
  std::snprintf(buf, sizeof buf,
                "spawn (%d,%d) on %s: ground y%d (sea y%d, home y%d), %d vox past the harness box, "
                "site cell %d, tarn %s%s",
                sx, sz, bname.c_str(), h, m.seaLevelY, CurrentTuning().worldgen.spawnPlainY,
                boxDist, siteCell, pq.inDisc ? "YES" : (pq.near ? "near" : "no"), why.c_str());
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
  const int treeline = CurrentTuning().worldgen.treeline;
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
