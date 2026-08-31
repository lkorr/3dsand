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

#include <cstdint>
#include <string>
#include <vector>

#include "sim/materials.h"

// ---------------------------------------------------------------------------
// The GPU buffer's layout. WGSL reads the same offsets through the TA_* consts
// at the top of worldgen.wgsl's tree section; scripts/check_invariants.py holds
// the two sides together.
// ---------------------------------------------------------------------------
//
//   header            16 words (below)
//   species directory kTreeSpeciesWords per species
//   biome table       4 x (1 + speciesCount)   cumulative weights
//   per species:      its .svtree payload, offsets rebased into this buffer
//
namespace treeatlas {

inline constexpr uint32_t kMagic = 0x41545653u;   // 'SVTA'
inline constexpr uint32_t kVersion = 1u;
inline constexpr int kHeaderWords = 16;
inline constexpr int kSpeciesWords = 24;
inline constexpr int kVariantWords = 12;
inline constexpr int kBiomeCount = 4;   // worldgen.wgsl B_FOREST..B_DESERT

// header word indices
enum : int {
  kHMagic = 0, kHVersion = 1, kHSpeciesCount = 2, kHTotalWords = 3,
  kHMaxReach = 4, kHMaxAbove = 5, kHBiomeTable = 6, kHSpeciesDir = 7,
  kHBiomeCount = 8
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

// THE CANDIDATE-SET BOUND, and why it is checked at load rather than assumed.
//
// worldgen scans a fixed +-TREE_SCAN tile neighbourhood and keeps at most
// TREE_CAND_MAX = 9 trees per column, because the arithmetic says nine is
// enough: a site sits in the middle half of its tile, so at most three tiles
// per axis can reach any column. That derivation depends on the widest species'
// REACH, which is now asset data -- an artist widening a crown could silently
// break it, and the failure is a canopy that is simply missing on some columns.
//
//   count per axis <= (2*reach + span - 1) / tile + 1,   span = tile/2
//
// and that has to stay under 4 for the count to be at most 3, i.e.
// `2*reach + span - 1 < 3*tile`. At the shipped tile of 144 that is reach <=
// 180, comfortably above the widest shipped species (the great oak, 115).
// LoadTreeAtlas refuses anything wider.
inline int MaxReachForNineCandidates(int treeTile) {
  return (3 * treeTile - treeTile / 2) / 2;
}

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
  int biome[treeatlas::kBiomeCount] = {0, 0, 0, 0};
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
  int maxReachXZ = 0;
  int maxAbove = 0;
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
 */
bool LoadTreeAtlas(const std::string& dir, const std::vector<MaterialDef>& mats,
                   TreeAtlas& out, std::string& log);

/** Decode one cell of one variant on the CPU, through the same column/run path
 *  the shader takes. Exists for the `tree-atlas` selftest gate — nothing on the
 *  frame path calls it. `lx`/`ly`/`lz` are VARIANT-LOCAL grid coordinates.
 *  Returns an engine material id, or 0 for air. */
uint32_t TreeAtlasCellAt(const TreeAtlas& atlas, int species, int variant,
                         int lx, int ly, int lz);
