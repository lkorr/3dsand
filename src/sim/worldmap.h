// worldmap.h -- the AUTHORED WORLD MAP: a finite, hand-painted overworld that
// worldgen samples instead of deriving biome and landform from noise.
//
// WHAT THIS IS. `assets/worldmap/<name>/` is one map: `map.json` (the
// diffable half -- cell size, extent, the biome roster that DEFINES the id
// space, hand-placed sites, seeded placement rules) beside `map.svmap` (the
// painted half -- three u8 planes: biome, landform, moisture). It is authored
// in the tuner's World Map tab and named by `worldgen.mapLayer` in
// tuning.json, exactly as `worldgen.editLayer` names a `.svedit`.
//
// WHY A MAP AND NOT MORE NOISE. docs/RESEARCH_worldgen.md §8 asked the
// question and answered it: a falling-sand sandbox's replay value is in the
// simulation, not the layout, so a world whose regions are FIXED ("go east
// until the sand starts") is strictly better here -- it is learnable,
// wiki-able, and it deletes the spawn-clearing/keep-out hacks that exist only
// because random terrain kept eating the test fixtures. §8.3 asked that the
// swap be "one function so that replacing analytic climate noise with a
// bilinear lookup into a coarse map buffer is a one-function change". This
// header is that buffer; `mapBiomeAt`/`mapLandformQ8` in worldgen.wgsl are
// that function.
//
// THE THREE TIERS (RESEARCH_worldgen §8.2), because the seed discipline is
// the whole design and it is easy to break by accident:
//
//   Tier A  the MAP        seed-INDEPENDENT. Where the mountains are, where
//                          the ocean is, which region is which. Same on every
//                          seed. Read from the planes here.
//   Tier B  the REGION     seeded. The boundary warp, which lake, which ruin,
//                          which sub-variant. hash3(seed ^ salt, ...).
//   Tier C  the LOCAL      seeded. Trees, cover, caves, boulders.
//
// A Tier-A read must never take `seed`. A Tier-B/C read must always salt it.
//
// FINITE MAP, INFINITE ENGINE. Nothing here bounds the world: i32 world
// coordinates, the toroidal residency window and `Stream::ShiftAxis` are all
// untouched and must stay that way. Outside the painted extent the landform
// plane fades to zero over `oceanFadeCells` and the biome is `ocean`, so the
// world keeps going as open water rather than ending at a wall.
//
// DETERMINISM (rule 1). The map is a new INPUT beside the seed, not a source
// of runtime state: every read is a pure function of (world coords, map
// content), integer throughout, so `genCell` stays reproducible. Because it is
// an input, changing a painted cell moves the world hash the same way a
// material or a re-baked tree does -- that is a rebaseline, not a regression.
// The map's content hash is reported at boot so a moved hash can be attributed
// to "the map changed" without a bisect.
//
// STATUS. P0 (this commit) defines the GPU header layout and binds an empty
// buffer at binding 31 in both sim layouts, with no reader, so the plumbing --
// bind groups, pass table rows, check_pass_table sets -- lands with the world
// hash provably unmoved. The loader, the samplers and the editor arrive in
// P1/P2/P3. See docs/PLAN_world_map.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace worldmap {

// ---- the GPU buffer header ------------------------------------------------
// Word offsets into the `worldMap` storage buffer (binding 31). Everything
// downstream of the header is addressed through an offset word recorded HERE
// rather than by a compile-time layout, so the biome roster, the plane extent
// and the site count are all data -- adding a biome must never be a shader
// edit. Mirrored by worldgen.wgsl's WM_H_* consts; check_invariants.py holds
// the two together.
//
// Offsets are WORD indices, matching the treeAtlas precedent (TA_H_*).
enum : uint32_t {
  kHMagic = 0,        // 'SVMP'
  kHVersion = 1,
  kHCellLog2 = 2,     // log2 of the map cell in voxels (10 = 1024 vox = 102.4 m)
  kHWidth = 3,        // plane width in cells
  kHHeight = 4,       // plane height in cells
  kHOriginX = 5,      // i32: world voxel X of plane cell (0,0)
  kHOriginZ = 6,      // i32: world voxel Z of plane cell (0,0)
  kHSeaLevelY = 7,    // world voxel Y of the sea plane
  kHOceanFade = 8,    // cells over which landform fades to 0 outside the plane
  kHWarpAmp = 9,      // biome-boundary warp amplitude, in voxels
  kHBiomeCount = 10,
  kHBiomeRecords = 11,  // word offset: per-biome record table
  kHBiomePlane = 12,    // word offset: biome plane, 4 cells per word
  kHLandformPlane = 13, // word offset: landform plane, 4 cells per word
  kHMoisturePlane = 14, // word offset: moisture plane, 4 cells per word
  kHSiteIndex = 15,     // word offset: per-cell site index
  kHSiteTable = 16,     // word offset: site records
  kHSiteCount = 17,
  kHStampRuns = 18,     // word offset: stamp template run-lists
  kHContentHash = 19,   // FNV-1a of both files; reported at boot
  kHMaxCoverH = 20,     // max over every biome of kB_MaxCoverH: the far
                        // cascade's blocker band has no biome in hand
  kHOceanBiome = 21,    // biome id returned outside the painted planes
  // The harness site (map.json sites[], kind "pad"): a world-voxel box that
  // keeps the selftest fixtures' ground clear of trees, tarns and cover. It
  // replaced the spawn clearing / fixture pads / pond keep-out literals in
  // P2b; P5's site table generalises it. i32 in u32 words.
  kHHarnessX0 = 22,
  kHHarnessZ0 = 23,
  kHHarnessX1 = 24,
  kHHarnessZ1 = 25,
  // The water preset table (P-E): one kWaterRecWords record per
  // assets/water/<name>.json, in the loader's (sorted file name) order, plus
  // the shore plant rows the records point into. kHMaxCoverH includes every
  // preset's kW_MaxPlantH, for the same reason it includes the cover rows.
  kHWaterRecords = 26,  // word offset: per-preset record table
  kHWaterCount = 27,
  // The spawn site (map.json sites[], kind "spawn", `at: [x, z]`): the world
  // column the player starts on, and the centre of the calm home area
  // (landAt's coarse-octave fade, PLAN_environment_truth P-C). One per map;
  // a map without one defaults to (140, 140), the pre-P-C literal, so old
  // maps keep starting where they did. i32 in u32 words, like the box above.
  kHSpawnX = 28,
  kHSpawnZ = 29,
  kHeaderWords = 32,    // padded, like treeatlas::kFileHeaderWords
};
// Planes are packed FOUR CELLS PER WORD, little-endian: cell i of a plane at
// word (off + (i >> 2)), byte (i & 3); i = cz * width + cx. The biome plane's
// bytes are biome IDS (map.json's palette resolved by name at load), never
// palette indices, so the shader does one lookup.

// The magic in both the file and the buffer header: 'SVMP', little-endian.
inline constexpr uint32_t kMagic = 0x504D5653u;
inline constexpr uint32_t kVersion = 1u;

// ---- the biome record table (P1) -------------------------------------------
// One fixed-stride record per biome id, at `kHBiomeRecords`, followed by the
// cover rows every record points into. This is what `assets/biomes/*.json`
// becomes on the GPU: the ground skin, the tree density, the per-biome cover
// stack, the cave thresholds. Mirrored by worldgen.wgsl's WM_B_* / WM_C_*
// consts; check_invariants.py holds the two together.
//
// Everything is an integer the shader can use as-is: material IDS (resolved
// by name at load, like the tree atlas), heights in VOXELS (the JSON authors
// metres), chances as 1-in-N, slopes in Q8. A cover row's `heightVox` is the
// stalk height; `head` caps the top cell when non-zero.
enum : uint32_t {
  kBiomeRecWords = 32,
  kB_Skin = 0,            // material id of the y == h skin
  kB_Subsoil = 1,         // material id under the skin (the wedge's topsoil)
  kB_SkinDepth = 2,       // cells of skin, >= 1
  kB_PatchThreshold = 3,  // 0..255 gate on the biome's patch field; 0 = no mask
  kB_PatchCellLog2 = 4,   // log2 of the patch field's cell, in voxels
  kB_TreeTileVox = 5,     // authored tree tile (informational; TREE_TILE is global)
  kB_TreeDensity = 6,     // percent of tree tiles that grow a tree
  kB_CoverCount = 7,
  kB_CoverOff = 8,        // word offset of this biome's first cover row
  kB_CaveThreshold1 = 9,  // near-surface cave band gate (0..255)
  kB_CaveThreshold2 = 10, // deep cave band gate
  kB_SedMax = 11,         // reserved for P4 (0 = use the global knob)
  kB_Flags = 12,          // kBF_* below
  kB_MaxCoverH = 13,      // tallest cover row (voxels, jitter included): the
                          // sky-skip and far-blocker ceilings MUST include it,
                          // or a plant above the old fixed margin is never
                          // written by a skipped chunk and sits above the far
                          // field's flagged top (far-fog gate, 2026-09-04)
  // 14..15 reserved (P-D takes 14 for its per-biome tree chance)
  // ---- P-E: the flora that used to be worldgen.* knobs ----
  // P-E INTERIM: a pond has no preset of its own until P-F drives the bowl
  // from the water table, so every pond and shore in a biome wears the
  // preset of the biome's FIRST water.features row. 0 = the biome authors no
  // water rows and grows no shore, pond or moss flora at all.
  kB_WaterPreset = 16,    // 1 + index into the water preset table; 0 = none
  kB_CaveMushroomChance = 17, // 1-in-N floor cells of the near band; 0 = never
  kB_CaveCrystalChance = 18,  // 1-in-N floor/ceiling cells of the deep band; 0 = never
  kB_CactusChance = 19,       // percent of CACTUS_TILE tiles that grow one (kBF_Cacti gates it)
  kB_SaguaroFraction = 20,    // percent of those that are saguaro columns, not barrels
  // 21..31 reserved
  kCoverRowWords = 8,
  kC_Mat = 0,
  kC_Head = 1,
  kC_Chance = 2,          // 1 in N surface columns; 0 = row is off
  kC_HeightVox = 3,       // >= 1
  kC_MinY = 4,            // -1 = unbounded (stored as u32, read as i32)
  kC_MaxY = 5,
  kC_MaxSlope = 6,        // Q8; 1024 = unbounded
  kC_PatchThreshold = 7,  // per-row extra gate on the biome patch field
};
// kB_Flags bits. These replace the `biome == B_DESERT` / `== B_PINE` tests
// that used to gate whole blocks of genCellIn on a hard-coded id.
inline constexpr uint32_t kBF_GroundFlora = 1u << 0;  // the canopy-inverted undergrowth + flower layer
inline constexpr uint32_t kBF_Cacti = 1u << 1;        // the cactus proc shape
inline constexpr uint32_t kBF_SandCap = 1u << 2;      // loose sand cap under the skin (the old desert rule)

// ---- the water preset table (P-E) --------------------------------------------
// One fixed-stride record per assets/water/<name>.json at kHWaterRecords, in
// the loader's order (sorted file name), followed by the shore plant rows the
// records point into. This is the FLORA half of a preset -- what grows on the
// wet fringe outside the bowl (shore.plants[], shore.mossChance), and what
// grows in the water by depth band (aquatic.emergent / floating / submerged).
// The GEOMETRY half (footprint, bathymetry, berm, shore band/lift) stays on
// the worldgen.pond* / shore* knobs until P-F; words 22..31 are reserved for
// it. Mirrored by worldgen.wgsl's WM_W_* / WM_P_* consts; check_invariants.py
// holds the two together.
//
// Depths are voxels of water over the bed, heights voxels from the bed (the
// aquatic rows) or from the ground (the shore rows); metres in the JSON.
// Every chance is 1-in-N with 0 = never (the shader guards the modulo).
enum : uint32_t {
  kWaterRecWords = 32,
  kW_Fill = 0,                 // fill material id; 0 = a dry preset (informational until P-F)
  kW_ShoreCount = 1,
  kW_ShoreOff = 2,             // word offset of this preset's first shore plant row
  kW_MossChance = 3,           // 1-in-N shore-band stone surface cells wear kW_MossMat
  kW_MossMat = 4,
  kW_EmergentMat = 5,          // reeds: stand on the bed, break the surface
  kW_EmergentChance = 6,
  kW_EmergentMinDepth = 7,     // water column depth band, inclusive
  kW_EmergentMaxDepth = 8,
  kW_EmergentHeight = 9,       // cells above the bed
  kW_FloatingMat = 10,         // lilypads: one cell ON the surface
  kW_FloatingFlower = 11,      // the blossom one cell above a pad (0 = none)
  kW_FloatingChance = 12,
  kW_FloatingFlowerChance = 13,// 1-in-N pads carry the flower
  kW_FloatingMinDepth = 14,
  kW_FloatingMaxDepth = 15,
  kW_SubmergedMat = 16,        // kelp: from the bed, held under the surface
  kW_SubmergedChance = 17,
  kW_SubmergedMinDepth = 18,   // only where the water is DEEPER than this
  kW_SubmergedHeight = 19,     // cells above the bed
  kW_SubmergedClearance = 20,  // cells of water kept clear above the top
  kW_MaxPlantH = 21,           // tallest thing this preset puts above ground or bed
                               // (jitter and head included): folded into
                               // kB_MaxCoverH / kHMaxCoverH so the sky-skip and
                               // far-blocker ceilings cover it
  // 22..31 reserved (P-F geometry)
  kShoreRowWords = 8,
  kP_Mat = 0,
  kP_Head = 1,                 // caps the top cells when non-zero
  kP_Chance = 2,               // 1 in N shore columns within reach; 0 = row is off
  kP_Reach = 3,                // voxels past the waterline the row still grows
  kP_Height = 4,               // stalk cells above the ground, >= 1
  // 5..7 reserved
};

// ---- the site table (P5) ----------------------------------------------------
// One record per authored site at kHSiteTable (kHSiteCount of them), plus a
// per-cell SITE INDEX plane (kHSiteIndex; four cells per word like the
// others) holding `site id + 1`, 0 = no site touches this cell. A site marks
// every cell its footprint + pad margin reaches, first site wins. That plane
// is what keeps the shader's per-column cost at one read: `wmSiteAt(x, z)`.
//
// Kinds: kSitePad is the harness box and is NOT in the table (it is the
// WM_H_HARNESS_* header words); kSiteStamp is an authored .vox placed with
// its footprint centred on (x, z), its bottom voxel row at the pad height
// + 1, rotated rot*90 degrees at PACK time so the shader's overlay is one
// column lookup in a run-list -- the tree atlas's encoding (mat | y0 << 16 |
// len << 27), because trees are the proof that a per-cell overlay of
// authored voxels is correct in the far cascades at any distance.
enum : uint32_t {
  kSiteRecWords = 16,
  kS_Kind = 0,          // kSiteStamp
  kS_X = 1,             // world voxel centre (i32 in u32)
  kS_Z = 2,
  kS_Radius = 3,        // Chebyshev footprint radius in voxels: keep-out + pad
  kS_PadMargin = 4,     // columns over which the pad ramps back to terrain
  kS_Rot = 5,           // 0..3, informational (baked into the stamp block)
  kS_Salt = 6,
  kS_StampOff = 7,      // word offset of the stamp block, 0 = none
  // 8..15 reserved
  kStampHdrWords = 4,
  kStamp_NX = 0, kStamp_NY = 1, kStamp_NZ = 2, kStamp_Columns = 3,
  // columns: nx*nz pairs of (runOff, runCount), absolute word offsets; runs:
  // mat (12 bits) | y0 << 16 | len << 27, y0 < 2048, len <= 31, y0 ascending
  kSitePad = 0,
  kSiteStamp = 1,
};

}  // namespace worldmap

namespace biomes { struct BiomeSet; }

namespace worldmap {

/**
 * Pack the loaded biome set into the `worldMap` buffer's words: header +
 * biome records + cover rows + water preset records + shore plant rows.
 * Biome ids must be contiguous 0..N-1
 * (ValidateBiomeSet enforces it); the record for id i is at
 * words[kHBiomeRecords] + i * kBiomeRecWords. Returns false, with `log`,
 * only on an id-space that cannot be laid out. The content hash goes in
 * kHContentHash so a moved world hash can be attributed to the table.
 */
bool PackBiomeTable(const biomes::BiomeSet& set, std::vector<uint32_t>& words,
                    std::string& log);

// ---- the loaded map (P2) ---------------------------------------------------
/** assets/worldmap/<name>/{map.json,map.svmap}, parsed and resolved. */
struct WorldMapData {
  std::string name;
  int cellLog2 = 10;
  int width = 0, height = 0;
  int originCellX = 0, originCellZ = 0;   // plane cell whose low corner is world (0,0)
  int seaLevelY = 0;
  int oceanFadeCells = 0;
  int warpAmpVox = 0;
  int oceanBiome = 0;                     // id of "ocean" in the set, or 0
  // The harness pad box, world voxels, inclusive. From map.json sites[] with
  // kind "pad"; a map without one gets an empty box (x1 < x0).
  int harnessX0 = 0, harnessZ0 = 0, harnessX1 = -1, harnessZ1 = -1;
  bool InHarness(int x, int z) const {
    return x >= harnessX0 && x <= harnessX1 && z >= harnessZ0 && z <= harnessZ1;
  }
  // The spawn site (kind "spawn"). `spawnAuthored` says the map named it;
  // otherwise these are the (140, 140) default and the loader said so.
  int spawnX = 140, spawnZ = 140;
  bool spawnAuthored = false;
  std::vector<std::string> palette;       // map.json biomes[]: plane byte -> name
  // Authored stamp sites (P5), resolved: the template loaded, rotated, and
  // packed into columns of runs at load. `siteIndex` is the per-cell plane.
  struct StampSite {
    std::string id, templateName;
    int x = 0, z = 0, radius = 0, padMargin = 8, rot = 0;
    uint32_t salt = 0;
    int nx = 0, ny = 0, nz = 0;
    std::vector<uint32_t> words;          // the packed stamp block, offsets RELATIVE to its start
  };
  std::vector<StampSite> sites;
  std::vector<uint8_t> siteIndex;         // width*height, site id + 1, 0 = none
  uint8_t SiteCell(int cx, int cz) const {
    return siteIndex.empty() ? 0 : siteIndex[static_cast<size_t>(cz) * width + cx];
  }
  std::vector<uint8_t> biome;             // RESOLVED to biome ids, width*height
  std::vector<uint8_t> landform;
  std::vector<uint8_t> moisture;
  uint32_t contentHash = 0;               // FNV-1a over both files
  bool Loaded() const { return width > 0 && height > 0; }
  /** Cell coordinates of a world voxel column (arithmetic shift: floors). */
  void CellOf(int x, int z, int* cx, int* cz) const {
    *cx = (x >> cellLog2) + originCellX;
    *cz = (z >> cellLog2) + originCellZ;
  }
  bool Inside(int cx, int cz) const {
    return cx >= 0 && cz >= 0 && cx < width && cz < height;
  }
  uint8_t BiomeCell(int cx, int cz) const {
    return biome[static_cast<size_t>(cz) * width + cx];
  }
};

/**
 * Load assets/worldmap/<name>/ and resolve its palette against the biome
 * set. False, with `log`, on a missing directory, a malformed file, a
 * palette name no biome file has, or planes whose size disagrees with
 * map.json -- the caller ABORTS on false (a world with no map is not a
 * world, see the header comment).
 */
bool LoadWorldMap(const std::string& assetDir, const std::string& name,
                  const biomes::BiomeSet& set, size_t materialCount, uint32_t seed,
                  WorldMapData& out, std::string& log);

/**
 * The whole `worldMap` buffer: PackBiomeTable's words plus the map header
 * fields and the three planes. `map` may be unloaded (P0/P1 tools), in which
 * case the plane offsets stay 0 and the samplers return biome 0.
 */
bool PackWorldMap(const biomes::BiomeSet& set, const WorldMapData& map,
                  std::vector<uint32_t>& words, std::string& log);

/**
 * The process-wide loaded map, for the CPU twins (World::MapBiomeAt and,
 * from P4, the height mirror). One, like CurrentTuning(): every reader of
 * "what biome is this column" must see the same planes. Set by the loader
 * paths in main.cpp / vk_smoke before the first World is built.
 */
const WorldMapData& CurrentWorldMap();
void SetCurrentWorldMap(WorldMapData map);

}  // namespace worldmap
