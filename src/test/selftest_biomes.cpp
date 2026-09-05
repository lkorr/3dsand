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

#include <cstdio>
#include <string>
#include <vector>

#include "sim/biomes.h"
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

}  // namespace

const std::vector<Gate>& BiomeGates() {
  static const std::vector<Gate> g = {
      {"biomes", "sim", {}, false, GateBiomes},
      {"worldmap", "sim", {}, false, GateWorldMap},
  };
  return g;
}

}  // namespace selftest
