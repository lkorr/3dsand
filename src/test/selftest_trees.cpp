// selftest_trees.cpp — the baked tree atlas, before the world is asked to grow it.
//
// WHAT THIS GUARDS THAT `determinism` DOES NOT. The world hash says the forest
// is the same forest as yesterday. It says nothing about whether the atlas is
// the one the artist baked, whether the C++ reader and the WGSL sampler agree
// on the column/run layout, or whether the metadata worldgen's candidate reject
// TRUSTS is actually true. Each of those fails silently, and differently:
//
//   * a stale .svtree       -> a hash that moved for a reason nobody can name
//   * a reader/shader skew  -> trees sheared along an invisible plane
//   * reach understated     -> canopies missing from SOME columns, not others
//   * an unresolved name    -> voxels that quietly became air
//
// None of them needs the GPU, so this gate runs FIRST, touches no world state,
// and costs about as long as reading the files. Being first is deliberate: when
// the atlas is wrong, every later gate is measuring a world nobody authored,
// and a failure here should be the first thing on the terminal rather than the
// twentieth (CLAUDE.md rule 7 in the other direction — a gate that leaves no
// state behind can safely be the cheapest thing in the run).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sim/treeatlas.h"
#include "sim/tuning.h"
#include "test/selftest.h"
#include "test/support.h"

namespace selftest {
namespace {

Status GateTreeAtlas(Ctx& c, std::string& detail) {
  TreeAtlas atlas;
  std::string log;
  const std::string dir = sandvox::AssetDir() + "/trees";
  if (!LoadTreeAtlas(dir, c.mats, atlas, log)) {
    detail = "atlas did not load: " + log;
    std::printf("tree-atlas: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  if (atlas.speciesCount == 0) {
    detail = "no species in " + dir + " -- run `node scripts/bake_trees.mjs`";
    std::printf("tree-atlas: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }

  bool ok = true;
  std::string why;
  const uint32_t* W = atlas.words.data();

  // ---- 1. the candidate-set bound -----------------------------------------
  // worldgen keeps nine trees per column on an arithmetic argument that reads
  // the widest species' reach (treeatlas.h). LoadTreeAtlas refuses past it;
  // assert it here too, because a refusal is exactly the thing that gets
  // "temporarily" relaxed to land a wider crown.
  const int tile = std::max(16, CurrentTuning().worldgen.treeTile);
  const int bound = treeatlas::MaxReachForNineCandidates(tile);
  if (atlas.maxReachXZ > bound) {
    ok = false;
    why += Format(" | widest reach %d > %d, the nine-candidate bound at "
                  "treeTile %d", atlas.maxReachXZ, bound, tile);
  }

  // ---- 2. every run resolved, and the metadata is TRUE --------------------
  // Walk every variant's every column. No run may carry material 0 (a name that
  // did not resolve became air), and each species' declared reach/above must
  // DOMINATE what its voxels actually occupy — an understated bound shears the
  // canopy only on the columns past it, which is why "it looked fine" is not
  // evidence.
  size_t runs = 0, voxels = 0;
  int airRuns = 0, trunkless = 0;
  for (int sp = 0; sp < atlas.speciesCount; sp++) {
    const uint32_t* s = W + W[treeatlas::kHSpeciesDir] + sp * treeatlas::kSpeciesWords;
    const int declReach = (int)s[treeatlas::kSReach];
    const int declAbove = (int)s[treeatlas::kSAbove];
    for (uint32_t v = 0; v < s[treeatlas::kSVariantCount]; v++) {
      const uint32_t* d = W + s[treeatlas::kSVariantDir] + v * treeatlas::kVariantWords;
      const int nx = (int)d[treeatlas::kVNx], ny = (int)d[treeatlas::kVNy],
                nz = (int)d[treeatlas::kVNz];
      const int ax = (int)d[treeatlas::kVAnchorX], az = (int)d[treeatlas::kVAnchorZ];
      int maxR = 0, maxY = 0;
      bool trunk = false;
      for (int z = 0; z < nz; z++) {
        for (int x = 0; x < nx; x++) {
          const uint32_t* col = W + d[treeatlas::kVColumns] + ((size_t)z * nx + x) * 2;
          for (uint32_t k = 0; k < col[1]; k++) {
            const uint32_t r = W[col[0] + k];
            const int y0 = (int)((r >> 16) & treeatlas::kRunMaxY0);
            const int len = (int)((r >> (16 + treeatlas::kRunY0Bits)) & treeatlas::kRunMaxLen);
            runs++;
            voxels += (size_t)len;
            if ((r & 0xFFFu) == 0) airRuns++;
            if (y0 + len > maxY) maxY = y0 + len;
            const int rr = std::max(std::abs(x - ax), std::abs(z - az));
            if (rr > maxR) maxR = rr;
            // Something has to stand on the ground near the trunk anchor, or
            // the tree floats above whatever it was planted on.
            if (y0 == 0 && std::abs(x - ax) <= 3 && std::abs(z - az) <= 3) trunk = true;
          }
        }
      }
      if (maxR > declReach || maxY > declAbove) {
        ok = false;
        why += Format(" | %s variant %u occupies reach %d / above %d but "
                      "declares %d / %d", atlas.species[sp].name.c_str(), v,
                      maxR, maxY, declReach, declAbove);
      }
      if (!trunk) trunkless++;
      if (ny > 512) {
        ok = false;
        why += Format(" | %s variant %u is %d tall; the run encoder's y0 field "
                      "is 9 bits", atlas.species[sp].name.c_str(), v, ny);
      }
    }
  }
  if (airRuns) {
    ok = false;
    why += Format(" | %d runs carry material 0 -- a name in a .svtree did not "
                  "resolve against materials.json", airRuns);
  }
  if (trunkless) {
    ok = false;
    why += Format(" | %d variants put nothing on the ground under the trunk "
                  "anchor (a floating tree)", trunkless);
  }

  // ---- 3. somebody wants at least one biome -------------------------------
  // A biome column that sums to zero grows no trees at all — a plausible
  // authoring intent for the desert, and a bug if it is true of every biome.
  const uint32_t bt = W[treeatlas::kHBiomeTable];
  const int stride = 1 + atlas.speciesCount;
  int emptyBiomes = 0;
  for (int b = 0; b < treeatlas::kBiomeCount; b++)
    if (W[bt + b * stride] == 0) emptyBiomes++;
  if (emptyBiomes >= treeatlas::kBiomeCount) {
    ok = false;
    why += " | no biome has a single species that wants it";
  }

  // ---- 4. the decode agrees with the runs it walked -----------------------
  // TreeAtlasCellAt is the function worldgen.wgsl's `treeCellFrom` is a
  // transcription of, so checking it against an independent expansion of the
  // same run list is what catches a shift or an off-by-one in either. Cheap
  // enough to be exhaustive over variant 0 of every species.
  int decodeBad = 0;
  for (int sp = 0; sp < atlas.speciesCount && decodeBad == 0; sp++) {
    const uint32_t* s = W + W[treeatlas::kHSpeciesDir] + sp * treeatlas::kSpeciesWords;
    const uint32_t* d = W + s[treeatlas::kSVariantDir];
    const int nx = (int)d[treeatlas::kVNx], ny = (int)d[treeatlas::kVNy],
              nz = (int)d[treeatlas::kVNz];
    std::vector<uint32_t> want((size_t)ny, 0u);
    for (int z = 0; z < nz && decodeBad == 0; z++) {
      for (int x = 0; x < nx && decodeBad == 0; x++) {
        std::fill(want.begin(), want.end(), 0u);
        const uint32_t* col = W + d[treeatlas::kVColumns] + ((size_t)z * nx + x) * 2;
        for (uint32_t k = 0; k < col[1]; k++) {
          const uint32_t r = W[col[0] + k];
          const int y0 = (int)((r >> 16) & treeatlas::kRunMaxY0);
          const int len = (int)((r >> (16 + treeatlas::kRunY0Bits)) & treeatlas::kRunMaxLen);
          for (int y = y0; y < y0 + len && y < ny; y++) want[(size_t)y] = r & 0xFFFu;
        }
        for (int y = 0; y < ny; y++) {
          if (TreeAtlasCellAt(atlas, sp, 0, x, y, z) != want[(size_t)y]) {
            decodeBad++;
            why += Format(" | %s decode mismatch at local (%d,%d,%d)",
                          atlas.species[sp].name.c_str(), x, y, z);
            break;
          }
        }
      }
    }
  }
  if (decodeBad) ok = false;

  // ---- 5. the atlas BYTES, pinned -----------------------------------------
  // The one check that says "these are the trees that were baked". Re-baking a
  // species moves the world hash, and this names WHICH file did it instead of
  // leaving a hash diff with no cause attached. A pinned-value-only difference
  // is not a real failure — MarkPinnedOnly is what lets --rebaseline update it
  // without letting a rebaseline paper over anything above.
  uint64_t h = 1469598103934665603ull;
  for (uint32_t w : atlas.words) {
    for (int b = 0; b < 4; b++) {
      h ^= (w >> (b * 8)) & 0xFFu;
      h *= 1099511628211ull;
    }
  }
  const std::string got = Format("%016llx", (unsigned long long)h);
  RecordObserved("treeAtlasHash", got);
  const std::string* pinned = BaselineValue("treeAtlasHash");
  if (pinned && !pinned->empty() && *pinned != got) {
    std::printf("tree-atlas: atlas hash %s, baseline says %s -- a species was "
                "re-baked. Check the list below is what you meant, then "
                "--rebaseline.\n", got.c_str(), pinned->c_str());
    if (ok) MarkPinnedOnly();
    ok = false;
  }

  for (const TreeSpeciesInfo& sp : atlas.species) {
    const std::string autumn =
        sp.autumnChance ? Format("  autumn 1-in-%d", sp.autumnChance) : std::string();
    std::printf("tree-atlas:   %-12s %dv  reach %3d  above %3d  crown y%3d r%3d"
                "  shade %3d  biomes f%d/m%d/p%d/d%d%s\n",
                sp.name.c_str(), sp.variants, sp.reachXZ, sp.above, sp.crownY,
                sp.crownR, sp.shade, sp.biome[0], sp.biome[1], sp.biome[2],
                sp.biome[3], autumn.c_str());
  }

  detail = Format("%d species, %zu runs, %zu voxels, %.2f MiB, reach<=%d "
                  "(bound %d), hash %s%s", atlas.speciesCount, runs, voxels,
                  atlas.Bytes() / 1048576.0, atlas.maxReachXZ, bound,
                  got.c_str(), why.c_str());
  std::printf("tree-atlas: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& TreeGates() {
  static const std::vector<Gate> g = {
      {"tree-atlas", "sim", {}, false, GateTreeAtlas},
  };
  return g;
}

}  // namespace selftest
