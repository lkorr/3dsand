#pragma once
// treeatlas.h — the baked tree atlas: assets/trees/*.svtree -> one GPU buffer.
//
// WHAT THIS REPLACES. Every tree the engine grew used to be an implicit shape
// re-derived per cell inside worldgen.wgsl (`treeCell`): an ellipsoid for oak,
// a diamond cone for pine, a hand-unrolled five-limb skeleton for birch. That
// is the only thing a pure per-cell GPU function CAN do, and it is why the
// forest read as lollipops on sticks.
//
// The trees are now voxelized ONCE, offline, by assets/editor/treegen.js — a
// Weber-Penn skeleton stamped as round-cone SDFs with smooth-min'd leaf clumps
// and a baked shade ramp. This file reads the result and hands worldgen a
// buffer to sample. `worldgen.wgsl`'s tree code becomes a bounds check, a
// column lookup and a short run scan.
//
// THE EDITOR IS THE ONLY VOXELIZER. There is deliberately no C++ or WGSL copy
// of the generation algorithm — a second implementation that has to agree with
// the first is the drift this arrangement exists to prevent (see the same rule
// at assets/editor/treegen.js's header and tuner.html's model bridge). The
// consequence is that editing a species and re-baking MOVES THE WORLD HASH,
// exactly as editing tuning.json does. That is one `--selftest --rebaseline`.
//
// AUTHORED BY NAME, RESOLVED AT LOAD (design guideline 4). A .svtree stores a
// material NAME TABLE and its run words carry LOCAL palette indices; this
// loader maps them to engine material ids. Renumbering materials.json can
// therefore never silently recolour a forest — it either resolves or reports.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/biomes.h"
#include "sim/materials.h"

// ---------------------------------------------------------------------------
// The GPU buffer's layout. WGSL reads the same offsets through the TA_* consts
// at the top of worldgen.wgsl's tree section; scripts/check_invariants.py holds
// the two sides together.
// ---------------------------------------------------------------------------
//
//   header            16 words (below)
//   species directory kTreeSpeciesWords per species
//   biome table       biomeCount x (1 + speciesCount)   cumulative weights
//   condition table   biomeCount x speciesCount x kCondWords   the biome file's
//                     per-row `conditions` (minY/maxY/maxSlope/nearWater/patch)
//   per species:      its .svtree payload, offsets rebased into this buffer
//
namespace treeatlas {

inline constexpr uint32_t kMagic = 0x41545653u;   // 'SVTA'
inline constexpr uint32_t kVersion = 1u;
inline constexpr int kHeaderWords = 16;
inline constexpr int kSpeciesWords = 24;
inline constexpr int kVariantWords = 12;
// There is no kBiomeCount any more. The biome id space is the set of
// assets/biomes/*.json files (biomes.h), the weight table has one row per
// loaded biome, and the shader reads the row count from the atlas header --
// see TreeAtlas::biomeCount. The .svtree's baked weight words (12..15) are
// no longer read; the biome files are the one authority (P1 of the world map).

// header word indices
enum : int {
  kHMagic = 0, kHVersion = 1, kHSpeciesCount = 2, kHTotalWords = 3,
  kHMaxReach = 4, kHMaxAbove = 5, kHBiomeTable = 6, kHSpeciesDir = 7,
  kHBiomeCount = 8,
  kHCondTable = 9    // word offset: biomeCount x speciesCount x kCondWords
};

// The per-(biome, species) placement conditions: what a biome file's tree row
// authors under `conditions` (biomes.h Conditions), packed as the shader reads
// them. Row (b, s) is at kHCondTable + (b * speciesCount + s) * kCondWords.
// Distances are VOXELS (the JSON authors metres); -1 = unbounded where the
// comment says so. A gated-out pick grows NOTHING rather than re-rolling --
// the same rule the species-file gates follow, and for the same reason: a
// re-roll would substitute a different tree and keep the density flat exactly
// where the author asked for it to thin.
enum : int {
  kCondWords = 8,
  kCMinY = 0,           // lowest ground Y, -1 = unbounded
  kCMaxY = 1,           // highest ground Y, -1 = unbounded
  kCMaxSlope = 2,       // Q8 landform slope; 0 or >= 1024 = unbounded
  kCNearWaterMax = 3,   // only within this many voxels of a pond rim; -1 = off
  kCNearWaterMin = 4,   // at least this many voxels from a pond rim; 0 = off
  kCPatchThreshold = 5  // 0..255 gate on the biome's patch field; 0 = off
  // 6..7 reserved
};

// species directory word indices (relative to the species' entry)
enum : int {
  kSVariantDir = 0, kSVariantCount = 1, kSReach = 2, kSAbove = 3,
  kSCrownY = 4, kSCrownR = 5, kSMinY = 6, kSMaxY = 7, kSMaxSlope = 8,
  kSSparsity = 9, kSCanopyMat = 10, kSShade = 11,
  // Autumn: a per-tree material SUBSTITUTION, leaf ramp -> autumn ramp, rolled
  // 1-in-kSAutumnChance by the tree's own hash. A ramp rather than one colour
  // because substituting a flat autumn material for a shaded green one would
  // throw the shading bake away on exactly the trees the eye goes to.
  kSAutumnChance = 12, kSLeaf0 = 13, kSAutumn0 = 16, kSFlags = 19
};

// THE TREE LATTICE, and the candidate-set bound derived from it.
//
// worldgen places trunk sites on ONE lattice of TREE_TILE voxels (the finest
// authored biome tile -- biomes.h FinestTreeTileVox), scans a +-TREE_SCAN tile
// neighbourhood around every column, and keeps at most TREE_CAND_MAX trees per
// column. All three used to be literals (144 / 2 / 9); they are now DERIVED at
// load from the lattice and the atlas's widest REACH, and reach the shader as
// prelude constants (gpu/resources.cpp ShaderConstantPrelude, mirrored by
// scripts/check_shaders.sh through scripts/tree_lattice.py).
//
// The arithmetic. A site sits in the middle half of its tile: tile t puts its
// trunk in [T*t + T/4, T*t + 3T/4). It can reach a column x only if that range
// meets [x - reach, x + reach], so
//
//   tiles to SCAN per side  = floor((reach + T/4 + T/2 - 1) / T)
//   CANDIDATES per axis    <= floor((2*reach + T/2 - 1) / T) + 1
//
// and the candidate cap is the square of the second. The per-axis count is
// CAPPED at kTreeCandPerAxisCap: the candidate set is per-thread scratch in
// genChunk (eight words per candidate), and a 7x7 set is 1.5 KiB of it. Past
// the cap LoadTreeAtlas REFUSES, naming the reach and the tile,
// because the shader would silently drop a candidate and the symptom is a
// canopy missing from some columns and present on others. The fix the plan
// names is a species `reach` cap authored in the tree page, never a wider
// global tile.
inline constexpr int kTreeCandPerAxisCap = 5;   // 25 candidates
struct TreeLattice {
  int tile = 144;      // TREE_TILE, voxels
  int scan = 2;        // TREE_SCAN, tiles per side
  int candMax = 9;     // TREE_CAND_MAX
  int perAxis = 3;     // candidates per axis the reach can produce
  int maxReach = 0;    // the atlas's widest species, voxels
};
/** Largest reach for which at most `perAxis` tiles per axis can reach one
 *  column on a `tile` lattice: 2*reach + tile/2 - 1 < perAxis*tile. */
inline int MaxReachForCandidates(int tile, int perAxis) {
  return (perAxis * tile - tile / 2) / 2;
}
/** The lattice for a tile and a widest reach. Pure; the loader and the
 *  shader checker both derive from it. Not clamped to the cap -- the caller
 *  compares `perAxis` against kTreeCandPerAxisCap and refuses. */
inline TreeLattice TreeLatticeFor(int tile, int maxReach) {
  TreeLattice l;
  l.tile = tile;
  l.maxReach = maxReach;
  const int inset = tile / 4, span = tile / 2;
  l.scan = (maxReach + inset + span - 1) / tile;
  l.perAxis = (2 * maxReach + span - 1) / tile + 1;
  const int scanSide = 2 * l.scan + 1;
  l.candMax = std::min(l.perAxis * l.perAxis, scanSide * scanSide);
  return l;
}
/** The process-wide lattice the shaders were compiled against. Set by
 *  Simulation::Init from the atlas it uploads, BEFORE the first LoadShader;
 *  the default (144 / 2 / 9) is what a tool with no atlas (--vk-info) gets. */
const TreeLattice& CurrentTreeLattice();
void SetCurrentTreeLattice(const TreeLattice& l);

// variant directory word indices
enum : int {
  kVNx = 0, kVNy = 1, kVNz = 2, kVAnchorX = 3, kVAnchorZ = 4,
  kVColumns = 5, kVRuns = 6, kVReach = 7, kVAbove = 8, kVCrownY = 9,
  kVCrownR = 10
};

// The .svtree file's own header, for the reader below.
inline constexpr uint32_t kFileMagic = 0x52545653u;   // 'SVTR'
inline constexpr int kFileHeaderWords = 32;
// v2 spends header word 29 on the BAKE SCALE in voxels/metre. Species files are
// authored in METRES; the atlas is voxels; this word is the only record of
// which conversion produced it, and ParseFile refuses a file whose scale is not
// the engine's kVoxelsPerMetre. v1 had no such word and was implicitly 10.
inline constexpr uint32_t kFileVersion = 2;
inline constexpr int kFileWordBakeVpm = 29;

// Run word layout, mirrored in assets/editor/treegen.js (packRun) and
// assets/shaders/worldgen.wgsl (treeCellFrom). Three places, one layout —
// scripts/check_invariants.py asserts they agree.
//
// Y0 was 9 bits, which capped a variant at 512 voxels: fine at 10 cm, but a
// 22 m redwood needs ~520 at 5 cm and clipped. Two bits moved from LEN to Y0.
inline constexpr int kRunY0Bits = 11;
inline constexpr int kRunLenBits = 5;
inline constexpr uint32_t kRunMaxY0 = (1u << kRunY0Bits) - 1u;    // 2047
inline constexpr uint32_t kRunMaxLen = (1u << kRunLenBits) - 1u;  // 31

}  // namespace treeatlas

/** One loaded species, for reporting and for the selftest gate. */
struct TreeSpeciesInfo {
  std::string name;      // the file's stem: "oak", "great_oak", ...
  int variants = 0;
  int reachXZ = 0;       // farthest voxel from the trunk column, either axis
  int above = 0;         // tallest voxel above the trunk's ground
  int crownY = 0;
  int crownR = 0;
  std::vector<int> biome;  // weight per biome id, from assets/biomes/*.json
  int minY = -1, maxY = -1, maxSlope = 0, sparsity = 1;
  uint32_t canopyMat = 0;  // engine material id the far cascades paint, 0 = none
  int shade = 0;           // 0..255 canopy cover this species casts
  int autumnChance = 0;    // 1-in-N trees turn; 0 = this species never does
  size_t words = 0;        // its share of the buffer
};

struct TreeAtlas {
  /** The buffer, exactly as uploaded. Always at least kHeaderWords long, even
   *  with no species on disk — a zero-length storage buffer is not bindable and
   *  "no trees" has to be a legal world, not a crash. */
  std::vector<uint32_t> words;
  int speciesCount = 0;
  int biomeCount = 0;      // rows in the weight table = loaded biome files
  int maxReachXZ = 0;
  int maxAbove = 0;
  treeatlas::TreeLattice lattice;   // from the biome set's finest tile + maxReachXZ
  std::vector<TreeSpeciesInfo> species;

  size_t Bytes() const { return words.size() * sizeof(uint32_t); }
};

/**
 * Read every assets/trees/<name>.svtree into one atlas.
 *
 * Species order is the SORTED FILE NAME, and that is load-bearing: the species
 * index reaches the world through a hash roll, so a directory-order-dependent
 * index would make the forest depend on the filesystem. Nothing else in the
 * engine names a species by index.
 *
 * Returns false only on a malformed file (bad magic, version, truncation, an
 * offset that leaves the file). A MISSING or EMPTY directory is a success with
 * zero species: worldgen then places no trees, which is a legal world and the
 * state a fresh checkout of the tools would be in before the first bake.
 * Unresolvable material names are reported into `log` and mapped to air.
 *
 * `set` is the loaded biome set (biomes.h): the per-biome weight table is
 * built from each biome file's `trees.species[]` by species NAME, one row per
 * biome id, so the forest the engine grows is the one the Environment tab
 * shows with no bake in between. A species no biome lists simply never grows.
 */
bool LoadTreeAtlas(const std::string& dir, const std::vector<MaterialDef>& mats,
                   const biomes::BiomeSet& set, TreeAtlas& out, std::string& log);

/** Decode one cell of one variant on the CPU, through the same column/run path
 *  the shader takes. Exists for the `tree-atlas` selftest gate — nothing on the
 *  frame path calls it. `lx`/`ly`/`lz` are VARIANT-LOCAL grid coordinates.
 *  Returns an engine material id, or 0 for air. */
uint32_t TreeAtlasCellAt(const TreeAtlas& atlas, int species, int variant,
                         int lx, int ly, int lz);
