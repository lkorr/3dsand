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

}  // namespace

const std::vector<Gate>& BiomeGates() {
  static const std::vector<Gate> g = {
      {"biomes", "sim", {}, false, GateBiomes},
  };
  return g;
}

}  // namespace selftest
