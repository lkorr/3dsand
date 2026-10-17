// worldgen_twin.h -- THE CPU TWIN OF WORLDGEN'S FEATURES ABOVE THE GROUND
// (docs/PLAN_distant_npcs.md §8.2, package D10).
//
// World::TerrainHeight is the HEIGHT CONTRACT: the ground, mirrored line for
// line from worldgen.wgsl's landColumn. Everything genCellIn stands ON that
// ground -- the lattice trees, the authored tree sites, the cacti, the stamps --
// had no CPU form, because nothing on the CPU needed to know where a trunk is.
// The distant tier does (a creature 60 m away must walk round a tree, not
// through it), and it may read nothing the GPU produces (rule 1: a readback's
// arrival tick is the scheduler's), so the features are re-derived here from
// the same integers the shader reads.
//
// WHAT IS EXACT AND WHAT IS NOT. Every function below is a twin of a named
// worldgen.wgsl function and reads the same tables (the tree atlas, the
// packed biome records, the CPU's copy of the site table), so where the
// shader's inputs are known on the CPU the answer is the shader's. The places
// it is not are stated where they occur (the cactus's column gates that read
// the tile plant and the shore cover, which the twin does not model), and the
// `distant-obstacles` gate measures the whole against real generated boxes.
//
// THE SPLIT. The functions that need world.cpp's file-static mirror (landAt,
// treelineAt, the pond scan, the site footprints) are DEFINED in world.cpp, at
// the end, so that file stays the one home of the mirrored terrain. What the
// overlay does with them (game/distant_obstacles.cpp) is the consumer's.
//
// Pure functions of (seed, the current world map, the uploaded environment);
// single-threaded callers only where noted (CurrentGenEnv's slot is a plain
// shared_ptr swap, written only by Simulation's environment upload).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "sim/worldmap.h"

struct TreeAtlas;

namespace wgtwin {

// ---- WHAT THE GPU'S WORLDGEN WAS HANDED -------------------------------------------
// The tree atlas and the packed worldMap words, as Simulation::Init /
// UploadEnvironment last uploaded them (copies: a gate's environment is a
// local that dies with it). `serial` changes on every upload, so a cache
// built from one environment knows when it is stale.
//
// The biome RECORDS are read from these words (tree chance, patch log2,
// flags, cactus chance): they are the BIOME SET's, not the map's, so a gate
// that swaps the CPU map without an upload (distant-walk's `peaks` arm) reads
// the right ones. The SITE table is never read from here -- worldmap::
// CurrentWorldMap() is, exactly as the height mirror reads it.
struct GenEnv {
  std::shared_ptr<const TreeAtlas> atlas;            // null until the first upload
  std::shared_ptr<const std::vector<uint32_t>> map;  // the packed worldMap buffer
  uint64_t serial = 0;
};
void SetGenEnv(const TreeAtlas& atlas, const std::vector<uint32_t>& mapWords);
const GenEnv& CurrentGenEnv();

// A packed biome record word (worldgen.wgsl wmBiome): biome `b`'s word `w`
// (worldmap.h kB_*), biome 0's for an id past the table, 0 for no table.
uint32_t BiomeWord(const std::vector<uint32_t>& W, uint32_t b, uint32_t w);

// ---- ONE TREE ---------------------------------------------------------------------
// worldgen.wgsl's `Tree`, the fields the geometry reads.
struct Tree {
  bool present = false;
  int sp = -1;            // atlas species index
  uint32_t varOff = 0;    // atlas word offset of the variant's directory entry
  int wx = 0, wz = 0;     // the trunk column
  int base = 0;           // ground at the trunk: local y 0 sits at base + 1
  uint32_t rot = 0;       // quarter turns
  bool mir = false;       // mirrored in x
  int reach = 0;          // the species' horizontal reach, voxels
  bool autumn = false;    // this tree wears the autumn leaf ramp
  int nwMax = -1, nwMin = 0;   // the row's near-water bounds (treePondOk)
};
// treeSite(tx, tz): the trunk column of lattice tile (tx, tz) (one hash).
void LatticeTreeSite(int tx, int tz, uint32_t seed, int& wx, int& wz);
// treeSite + treeInfoBare: lattice tile (tx, tz)'s tree with every gate but
// the ASKING column's pond set (AskColumnAt). `dg` is a worldMap-shaped word
// vector carrying the dungeon block (dgSurfNear); `bio` the packed biome
// records (GenEnv::map).
Tree LatticeTree(int tx, int tz, uint32_t seed, const TreeAtlas& atlas,
                 const std::vector<uint32_t>& bio, const std::vector<uint32_t>& dg);
// siteTree: the authored tree of site `sid` (1-based, worldmap.h).
Tree SiteTree(uint32_t sid, uint32_t seed, const TreeAtlas& atlas);

// ---- ONE CACTUS (cactusInfoBare / cactusCell) ----------------------------------------
struct Cactus {
  bool present = false;
  uint32_t species = 0;   // 0 saguaro, 1 barrel
  int wx = 0, wz = 0, base = 0, height = 0, radius = 0, arms = 0;
  uint32_t rnd = 0;
};
int CactusTile();   // CACTUS_TILE: vlen(40)
Cactus CactusAt(int tx, int tz, uint32_t seed, const std::vector<uint32_t>& bio,
                const std::vector<uint32_t>& dg);
int CactusReach(const Cactus& c);    // cactusReach
int CactusVTop(const Cactus& c);     // cactusVTop
// cactusCell: the material at world cell (x, y, z), 0 for air.
uint32_t CactusCell(const Cactus& c, int x, int y, int z);
// The ids worldgen.wgsl places the cactus with (M_CACTUS* in that file: array
// positions in materials.json, the shader's own constants).
inline constexpr uint32_t kMatCactus = 70u, kMatCactusRib = 71u, kMatCactusBloom = 72u;

// ---- THE ASKING COLUMN (genColumn's gates + treePondOk / cactusInfo's) -----------
// A feature's trunk point and the column-dependent bounds it carries.
struct PondAsk {
  int wx = 0, wz = 0;
  int nwMax = -1, nwMin = 0;
};
struct AskColumn {
  int tl = 0;            // treelineAt(x, z): no tree or cactus cell where h >= tl
  bool inPond = false;   // genColumn's `pond >= 0`: a gated tarn disc covers it
  uint32_t biome = 0;    // biomeAt(x, z)
};
// One column's pond scan, ONCE, and every feature's pond gate against it:
// bit i of `okMask` = asks[i] passes (not in a candidate disc; the row's
// near-water bounds hold). `n` <= 64.
AskColumn AskColumnAt(int x, int z, uint32_t seed, const PondAsk* asks, int n, uint64_t& okMask);

// ---- THE STAMPS (wmStampCell / wmStampsCell) --------------------------------------------
// The template voxel stamp site `s` puts at world cell (x, y, z), 0 if none
// (template air, or outside the template). Over the CPU's copy of the block
// (StampSite::words, offsets relative to its start). `padY` is sitePadY.
uint32_t StampTemplateCell(const worldmap::WorldMapData::StampSite& s, int x, int y, int z, int padY);
// sitePadY(sid): the baked pad height (the lab slab in the lab world).
int SitePadY(uint32_t sid);

}  // namespace wgtwin
