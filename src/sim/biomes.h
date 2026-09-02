// biomes.h — the authored biome and water-body preset files, loaded and
// validated on the CPU.
//
// WHAT THIS IS. assets/biomes/<name>.json is a biome: the ground cover it
// wears, the tree species that grow in it and at what weight, the water-body
// presets that appear in it and how rarely, the cave bands under it — each
// row with the same placement chain (rarity, then conditions). assets/water/
// <name>.json is a body-of-water preset: footprint, bathymetry, fill, berm,
// bed, shoreline and aquatic vegetation. Both are authored in the tuner's
// Environment tab (assets/editor/biome.js, water.js) and previewed there by
// the pure generators (biomegen.js, watergen.js).
//
// WHAT THE ENGINE DOES WITH THEM TODAY: reads and VALIDATES them, in the
// `biomes` selftest gate. Every species a biome names must be a baked atlas,
// every preset a file, every material a materials.json entry, and each
// biome's index must be the worldgen B_* id its name implies — because the
// tree atlas bakes per-biome weights into words indexed by that id, and a
// biome file that disagreed with worldgen about which id it is would be an
// authoring surface lying about what it authors.
//
// WHAT IT WILL DO: become the table worldgen reads for cover, water and cave
// placement. docs/PLAN_biomes.md §5 lists the seams in the order of least
// risk; the loader here is deliberately shaped so that step is "upload a
// BiomeSet" rather than "write a loader".
//
// The tree WEIGHTS are not duplicated here on purpose. The biome file is where
// they are edited, the species file carries a mirror (placement.biomes) the
// .svtree bake reads, and `ValidateBiomeSet` asserts the two agree — so there
// is one authority and one check, not two authorities.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sim/materials.h"

namespace biomes {

// worldgen.wgsl B_FOREST..B_DESERT, in id order. Mirrored by treegen.js
// BIOME_ORDER and biomegen.js ENGINE_BIOMES; scripts/check_invariants.py
// holds the three together.
inline constexpr int kEngineBiomeCount = 4;
extern const char* const kEngineBiomes[kEngineBiomeCount];

/** The placement chain every feature row carries. -1 / 0 = unbounded. */
struct Conditions {
  int minY = -1;            // lowest ground Y (world voxels)
  int maxY = -1;            // highest
  int maxSlope = 1024;      // Q8 landform slope gate (256 = 45 deg)
  float nearWaterMaxM = -1; // metres; only within this distance of water
  float nearWaterMinM = 0;  // metres; at least this far from water
  int patchThreshold = 0;   // 0..255 patch-noise gate
};

struct CoverRow {
  std::string material, head;
  uint32_t materialId = 0, headId = 0;   // resolved; 0 = unresolved / none
  int chance = 0;                        // 1 in N columns
  float heightM = 0.3f;
  Conditions cond;
};

struct TreeRow {
  std::string species;
  int weight = 0;
  Conditions cond;
};

struct WaterRow {
  std::string preset;
  float tileM = 44.8f;
  int rarity = 0;                        // 1 in N tiles
  Conditions cond;
};

struct CaveRow {
  std::string preset;                    // near_surface | deep
  int threshold = 0;
  int rarity = 0;
  Conditions cond;
};

struct BiomeDef {
  std::string name, displayName;
  int index = -1;                        // worldgen id, -1 = not an engine biome
  float temperature = 0.5f, moisture = 0.5f;
  std::string skin, subsoil;
  uint32_t skinId = 0, subsoilId = 0;
  int skinDepth = 1;
  int patchThreshold = 0, patchCellLog2 = 5;
  std::vector<CoverRow> cover;
  float treeTileM = 14.4f;
  int treeDensity = 0;                   // percent of tiles
  std::vector<TreeRow> trees;
  std::vector<WaterRow> water;
  std::vector<CaveRow> caves;
  std::map<std::string, double> terrainOverrides;   // worldgen.<key> -> value
  std::string file;
};

struct WaterPresetDef {
  std::string name, displayName, kind;
  float radiusM = 0, radiusVM = 0, depthM = 0, rimDepthM = 0, levelM = 0;
  float bermHeightM = 0, bermWidthM = 0, shoreBandM = 0, shoreLiftM = 0;
  std::string fill;                      // material name; "" or "none" = dry
  uint32_t fillId = 0;
  std::vector<std::string> materials;    // every material name the preset uses
  std::vector<std::string> unresolved;   // the subset materials.json does not have
  float tileM = 0;
  int rarity = 0, maxSlope = 0;
  std::string file;
};

/** One species file's mirrored weights, for the sync check. */
struct SpeciesMirror {
  std::string name;
  int biome[kEngineBiomeCount] = {0, 0, 0, 0};
  bool hasAtlas = false;                 // <name>.svtree exists beside the .json
};

struct BiomeSet {
  std::vector<BiomeDef> biomes;
  std::vector<WaterPresetDef> water;
  std::vector<SpeciesMirror> species;
  int problems = 0;                      // counted by ValidateBiomeSet
};

/**
 * Load assets/biomes/*.json, assets/water/*.json and the placement block of
 * assets/trees/*.json under `assetDir`. Material names resolve against `mats`
 * by NAME (index == engine id, air at 0). Returns false only if a file will
 * not parse; missing directories load as empty and validation says so.
 */
bool LoadBiomeSet(const std::string& assetDir, const std::vector<MaterialDef>& mats,
                  BiomeSet& out, std::string& log);

/**
 * Every check the tuner's `biomes` page makes, on the engine's side of the
 * fence. Appends one line per problem to `out`; returns the count.
 */
int ValidateBiomeSet(const BiomeSet& set, std::vector<std::string>& out);

/** Find a biome by engine id; nullptr if the set has no file for it. */
const BiomeDef* BiomeById(const BiomeSet& set, int id);

}  // namespace biomes
