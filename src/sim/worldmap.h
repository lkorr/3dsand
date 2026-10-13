// worldmap.h -- the AUTHORED WORLD MAP: a finite, hand-painted overworld that
// worldgen samples instead of deriving biome and landform from noise.
//
// WHAT THIS IS. `assets/worldmap/<name>/` is one map: `map.json` (the
// diffable half -- version 2: the extent, the plane lattices, the biome
// roster that DEFINES the id space, hand-placed sites, seeded placement
// rules) beside `planes/<plane>/<tx>_<tz>.svt` (the painted half, TILED: an
// absolute HEIGHT plane in i16 voxels at 6.4 m posts and a u8 BIOME plane at
// 25.6 m, 256 x 256 samples a tile, an absent tile = the plane's default).
// It is authored on the tuner's Environment -> World map page and named by
// `world.mapLayer` in tuning.json (the map in turn names its `.svedit` edit
// layer, P7). docs/PLAN_map_v2.md "The v2 map format" is the contract; a v1
// map (map.svmap) is refused with the converter's name,
// scripts/convert_map_v2.py.
//
// WHY A MAP AND NOT MORE NOISE. docs/RESEARCH_worldgen.md §8 asked the
// question and answered it: a falling-sand sandbox's replay value is in the
// simulation, not the layout, so a world whose regions are FIXED ("go east
// until the sand starts") is strictly better here -- it is learnable,
// wiki-able, and it deletes the spawn-clearing/keep-out hacks that exist only
// because random terrain kept eating the test fixtures. §8.3 asked that the
// swap be "one function so that replacing analytic climate noise with a
// bilinear lookup into a coarse map buffer is a one-function change". This
// header is that buffer; `mapBiomeAt`/`mapHeightAt` in worldgen.wgsl are
// that function.
//
// THE THREE TIERS (RESEARCH_worldgen §8.2), because the seed discipline is
// the whole design and it is easy to break by accident:
//
//   Tier A  the MAP        seed-INDEPENDENT. Where the mountains are, where
//                          the ocean is, which region is which. Same on every
//                          seed. Read from the planes here.
//   Tier B  the REGION     seeded. The boundary warp, which pond rolls,
//                          which sub-variant. hash3(seed ^ salt, ...).
//   Tier C  the LOCAL      seeded. Trees, cover, caves, boulders.
//
// A Tier-A read must never take `seed`. A Tier-B/C read must always salt it.
//
// FINITE MAP, INFINITE ENGINE. Nothing here bounds the world: i32 world
// coordinates, the toroidal residency window and `Stream::ShiftAxis` are all
// untouched and must stay that way. Outside the painted extent the authored
// height eases to the height plane's default over `edgeFadeVox` and the biome
// is the biome plane's default (ocean), so the world keeps going as open
// water rather than ending at a wall.
//
// DETERMINISM (rule 1). The map is a new INPUT beside the seed, not a source
// of runtime state: every read is a pure function of (world coords, map
// content), integer throughout, so worldgen stays reproducible. Because it is
// an input, changing a painted cell moves the world hash the same way a
// material or a re-baked tree does -- that is a rebaseline, not a regression.
// The map's content hash is reported at boot so a moved hash can be attributed
// to "the map changed" without a bisect.
//
// STATUS. Live: the buffer at binding 31 in both sim layouts carries the
// header, the painted planes, the biome / cover / water-row records, the water
// presets and the site table; worldgen.wgsl reads all of it (WM_* consts,
// held to the enums below by check_invariants.py). History:
// docs/PLAN_world_map.md, docs/PLAN_map_overhaul.md.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "sim/heat.h"

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
  // THE SITE INDEX LATTICE (map.json `siteCellLog2`): the per-cell site lists
  // (kHSiteIndex) and nothing else. Since map v2 the height and biome planes
  // have their own lattices (kHHeight* / kHBiome* below); this one stays
  // coarse because a site list is per cell, not per sample.
  kHSiteCellLog2 = 2, // log2 of the site cell in voxels (10 = 1024 vox = 102.4 m)
  kHSiteW = 3,        // site index plane width in site cells
  kHSiteH = 4,        // site index plane height in site cells
  kHSiteOriginX = 5,  // i32: the site-plane CELL INDEX of the cell whose low
  kHSiteOriginZ = 6,  //   corner is world voxel (0, 0), i.e. -(extent.min >> log2)
  kHSeaLevelY = 7,    // world voxel Y of the sea plane
  kHEdgeFade = 8,     // voxels over which the authored height eases to the
                      // height plane's default outside the extent (map.json edgeFadeVox)
  kHWarpAmp = 9,      // biome-boundary warp amplitude, in voxels
  kHBiomeCount = 10,
  kHBiomeRecords = 11,  // word offset: per-biome record table
  kHBiomePlane = 12,    // word offset: biome plane, 4 posts per word, 0 = no planes
  kHHeightPlane = 13,   // word offset: height plane, 2 i16 posts per word, 0 = no planes
  kHHeightDefault = 14, // i32: planes.height.default, the height an absent tile and the far ocean hold
  kHSiteIndex = 15,     // word offset: per-site-cell site index
  kHSiteTable = 16,     // word offset: site records
  kHSiteCount = 17,
  kHStampRuns = 18,     // word offset: stamp template run-lists
  kHContentHash = 19,   // FNV-1a of the map's files; reported at boot
  kHMaxCoverH = 20,     // max over every biome of kB_MaxCoverH: the far
                        // cascade's blocker band has no biome in hand
  kHBiomeDefault = 21,  // biome id returned outside the extent (planes.biome.default)
  // The harness site (map.json sites[], kind "pad"): a world-voxel box that
  // keeps the selftest fixtures' ground clear of trees, tarns and cover. It
  // replaced the spawn clearing / fixture pads / pond keep-out literals in
  // P2b; P5's site table generalises it. i32 in u32 words.
  kHPadX0 = 22,
  kHPadZ0 = 23,
  kHPadX1 = 24,
  kHPadZ1 = 25,
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
  // THE ONE POND LATTICE (P-F, docs/PLAN_environment_truth.md): rolled ponds
  // sit on one lattice of kHPondTile voxels -- the FINEST `water.features[]
  // .tile` among the biomes that author water -- and each biome's rows are
  // thinned on it to the density the page predicts, exactly as trees are
  // (kR_ChanceQ16 below). 0 = no biome authors water and worldgen rolls
  // none. kHPondBand is the widest max(shore.band, berm.width) any preset
  // asks for, in voxels: the height mirror's pondNear scans that far for a
  // disc before it knows which preset the disc wears.
  kHPondTile = 30,
  kHPondBand = 31,
  // ---- THE TERRAIN (P-G, docs/PLAN_environment_truth.md): per MAP -------------
  // What used to be worldgen.* tuning rows is map.json `terrain`, packed here
  // so the height mirror on both sides reads the same words (worldgen.wgsl
  // wmTerrain(WM_H_TERRAIN_*) / world.cpp wmTerrain(WM_H_TERRAIN_*), token-
  // identical inside the mirror). Lengths are VOXELS at the live voxel size
  // (LoadWorldMap rescaled them from the map's refVoxelsPerMetre); log2 cells
  // are shifts; fbmAtten / sedFraction / sedSlope are dimensionless.
  kHTerrainBaseHeight = 32,      // the authored height when NO plane is loaded (tools, P0/P1)
  // Map v2 A1 (docs/PLAN_map_v2.md): word 33 was kHTerrainLandformRange (the
  // v1 u8 landform plane's span). The treeline's seeded wobble: treelineAt
  // adds +-this many voxels of 25.6 m value noise so the treeline is not a
  // ruled contour. 0 = a ruled line (the harness map).
  kHTerrainTreelineJitter = 33,
  kHTerrainRangeAmplitude = 34,  // the seeded octave ladder under the plane
  kHTerrainRangeLog2 = 35,
  kHTerrainHillAmplitude = 36,
  kHTerrainHillLog2 = 37,
  kHTerrainDetailAmplitude = 38,
  kHTerrainDetailLog2 = 39,
  kHTerrainGrainAmplitude = 40,
  kHTerrainGrainLog2 = 41,
  kHTerrainFbmAtten = 42,        // Q8 derivative attenuation, 0..256
  // Map v2 A1: word 43 was kHTerrainHomeY, the height the calm home area
  // faded the coarse relief toward. The authored plane is the author's now and
  // the calm fade flattens only the range rung's NOISE, so no word reads a Y.
  // STEEP ROCK: a column whose landform slope (Land.slope, Q8) is at or above
  // this skins as the biome's rock (coverFirmMat) and wears no snow cap and no
  // cover row -- powder on a cliff never sleeps (rule 2), and grass on one
  // does not read as a cliff. 0 = off.
  kHTerrainRockSlope = 43,
  kHTerrainHomeR = 44,        // the calm home area (was spawnPlainR/Fade)
  kHTerrainHomeFade = 45,
  // The sediment wedge (was sed*). SED_ABOVE is RELATIVE (map v2 A1): the
  // wedge fills where the noise sits below the AUTHORED ground (plane + sculpt)
  // + this many voxels, so a high valley carries sediment like a low one. It
  // was the absolute kHTerrainSedCeil (y 264), which put every column above
  // 26 m dry.
  kHTerrainSedAbove = 46,
  kHTerrainSedFraction = 47,
  kHTerrainSedStrip = 48,
  kHTerrainSedSlope = 49,
  kHTerrainSedMax = 50,
  kHTerrainSedTopsoil = 51,
  // THE MAP'S TREELINE, absolute ground Y. A column's own (treelineAt) is
  // sea + (this - sea) * the biome's treelineScale, blended like the relief,
  // + the jitter: snow and no trees at or above it.
  kHTerrainTreeline = 52,
  kHTerrainRefVpm = 53,          // the voxels-per-metre the map's terrain was authored at
  // THE SCULPT LAYER (P5 of docs/PLAN_map_overhaul.md): word offset of the
  // packed sculpt block (kSculpt* / kSc_* below), 0 = the map has none. 0 is
  // what every map without a sculpt.svsculpt packs, and the samplers return a
  // zero offset for it, which is the whole "empty layer = byte-identical
  // world" argument.
  kHSculpt = 54,
  // ---- MAP V2: the extent and the two fine planes (docs/PLAN_map_v2.md) ------
  // The authored extent, world voxels: [X0, X0 + W] x [Z0, Z0 + H]. A plane of
  // log2 L has (W >> L) + 1 x (H >> L) + 1 POSTS, post (i, k) at world
  // (X0 + i << L, Z0 + k << L): the last post sits ON the max edge. Packed
  // dense, z-major (index = k * postsX + i): the height plane two i16 per
  // word (low half first), the biome plane four u8 per word (little-endian).
  // The height plane is interpolated (Catmull-Rom over the 4 x 4 posts,
  // worldgen.wgsl mapHeightAt); a biome post is the CELL centred half a cell
  // past it (biomeCellAt), and biomeMixAt blends cell centres bilinearly.
  kHExtentX0 = 55,      // i32
  kHExtentZ0 = 56,      // i32
  kHExtentW = 57,
  kHExtentH = 58,
  kHHeightLog2 = 59,    // 3..6 (the Catmull-Rom's i32 bounds are argued for f < 64)
  kHHeightPostsX = 60,
  kHHeightPostsZ = 61,
  kHBiomeLog2 = 62,
  kHBiomePostsX = 63,
  kHBiomePostsZ = 64,
  // Map v2 A1: the highest any column's treeline can be (PackWorldMap, over
  // every biome's treelineScale and the jitter) -- the world-wide bound
  // treeMaxTop gives the sky early-outs (cacti: per column, worldgen.wgsl
  // cactusColTop). Read by the GPU only.
  kHTreelineMax = 65,
  // THE DUNGEON BLOCK (docs/PLAN_dungeons.md, sim/dungeon.h PackDungeons): the
  // word offset of the placed dungeons' records, prims, templates and sparse
  // 64^3 chunk index; 0 = the map places none, and then worldgen's dungeon
  // layer is this one word per chunk (worldgen_dungeon.wgsli).
  kHDungeon = 66,
  kHeaderWords = 72,    // padded, like treeatlas::kFileHeaderWords
};

// ---- the sculpt layer (P5) ---------------------------------------------------
// A SPARSE, TILED, SIGNED HEIGHT OFFSET in whole voxels, authored with the
// World map page's raise / lower / smooth / flatten / erase brushes and stored
// beside the map as assets/worldmap/<name>/sculpt.svsculpt. The height mirror
// adds it to the ground in landAt (both sides) after the octave ladder and the
// calm-area fade and BEFORE the sediment wedge, the ponds, the sea test and the
// site pads, so everything downstream reacts to the sculpted ground. It is the
// tier between the 6.4 m height post and the single voxel (.svedit).
//
// GEOMETRY. One sample every 2^kSculptSpacingLog2 = 8 voxels (0.8 m), bilinear
// between samples; a TILE is 32 x 32 samples = 256 voxels (25.6 m) square; a
// directory REGION is 4 x 4 tiles = 1024 voxels. Why these numbers is in
// DESIGN.md §9d ("THE SCULPT LAYER"): 8 voxels is the grain octave's cell, so a brush
// can cancel every seeded octave but the +-2 voxel grain, and 256-voxel tiles
// make a sculpted spot cost 2 KB on disk / 2.2 KB on the GPU while an unsculpted
// 20 km map costs nothing at all.
//
// FILE (little-endian): u32 kSculptFileMagic, u32 kSculptVersion, u32 spacing
// log2 (== kSculptSpacingLog2), u32 tile log2 (== kSculptTileLog2), u32 tile
// count N, 3 x u32 reserved; then N records of { i32 tx, i32 tz, 32*32 i16
// samples, z-major }. (tx, tz) is the WORLD tile: tile (tx, tz) owns sample
// (sx, sz) = (tx*32 + i, tz*32 + j), i.e. world column (sx*8, sz*8). All-zero
// tiles are dropped at load; a file with none is no layer.
//
// PACKED BLOCK (PackSculpt; every offset RELATIVE to the block's first word):
//   [kSc_RegionX0, kSc_RegionZ0]  i32: region coords of directory entry 0
//   [kSc_RegionW, kSc_RegionH]    directory extent in regions
//   [kSculptHdrWords ..)          W*H region entries: 0 = no tile, else the
//                                 offset of a 16-word tile directory (tile
//                                 (tx & 3) + 4 * (tz & 3)): 0 = empty tile,
//                                 else the offset of a tile body
//   tile body                     kSculptTileSide^2 i16 samples, two per word,
//                                 low half first, z-major, INCLUDING the +1
//                                 row/column of the east/south neighbours so
//                                 a column's bilinear never leaves its tile.
// A tile body is materialised wherever any of its 33 x 33 samples is non-zero
// (the loader adds the west / north / north-west neighbours of every authored
// tile for that reason), so the sampler's "no tile = offset 0" is exact.
enum : uint32_t {
  kSculptSpacingLog2 = 3,   // voxels between samples, log2 (8 vox = 0.8 m)
  kSculptTileLog2 = 5,      // samples per tile side, log2 (32 -> 256 vox)
  kSculptRegionLog2 = 2,    // tiles per directory region side, log2 (4 -> 1024 vox)
  kSculptTileSide = 33,     // stored samples per tile side: 32 + the shared edge
  kSculptTileWords = 545,   // ceil(33 * 33 / 2)
  kSculptHdrWords = 4,
  kSc_RegionX0 = 0,
  kSc_RegionZ0 = 1,
  kSc_RegionW = 2,
  kSc_RegionH = 3,
};
inline constexpr uint32_t kSculptFileMagic = 0x43535653u;   // 'SVSC'
inline constexpr uint32_t kSculptVersion = 1u;
inline constexpr int kSculptTileSamples = 1 << kSculptTileLog2;   // 32
/** The directory's size ceiling, in region entries (16 MiB of words): a tile
 *  set whose bounding box needs more is refused, not silently allocated. The
 *  whole shipped 196 x 196-cell map is 38,416. */
inline constexpr int64_t kSculptMaxRegions = int64_t(1) << 22;
// The biome plane's bytes in the BUFFER are biome IDS (map.json's palette
// resolved by name at load), never palette indices, so the shader does one
// lookup. Its tiles on disk hold palette indices.

// The magic of the buffer header: 'SVMP', little-endian. Version 2 = map v2.
inline constexpr uint32_t kMagic = 0x504D5653u;
inline constexpr uint32_t kVersion = 2u;

// ---- the plane tiles on disk (map v2) -----------------------------------------
// assets/worldmap/<name>/planes/<plane>/<tx>_<tz>.svt, one per 256 x 256 posts
// (tile index = post index >> 8). Little-endian: u32 kTileMagic, u32 plane kind
// (kPlaneHeight | kPlaneBiome), i32 tx, i32 tz, u32 w, u32 h (both kTileSide,
// edge tiles too: samples past the plane are ignored), u32 sample bytes
// (2 | 1), u32 reserved 0; then w*h samples, z-major. Absent tile = every
// sample is the plane's `default`.
inline constexpr uint32_t kTileMagic = 0x31545653u;   // 'SVT1'
inline constexpr uint32_t kPlaneHeight = 1u;
inline constexpr uint32_t kPlaneBiome = 2u;
inline constexpr int kTileSide = 256;
inline constexpr int kTileHeaderBytes = 32;
// The authored height's bound, voxels (+-3.2 km): every encoding downstream
// (farMap u16 biased, wind i16, sculpt i16, chunk keys) holds it, and the
// Catmull-Rom's i32 products are argued for it (worldgen.wgsl crStep).
inline constexpr int kHeightLimit = 32000;
// A terrain noise amplitude's ceiling, voxels (map.json terrain.*Amplitude):
// with a biome multiplier of at most 16x (kB_*Mul <= 4096 Q8) the octave's
// n.dx * amp product stays under 2^31 (24575 * 65536 = 1.6e9; worldgen.wgsl
// octave()).
inline constexpr int kNoiseAmpMax = 4096;
// The treeline jitter's ceiling, voxels (map.json terrain.treelineJitter):
// treelineAt's ((n - 8192) * jitter) >> 13 stays under 2^26.
inline constexpr int kTreelineJitterMax = 4096;

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
  kBiomeRecWords = 48,
  kB_Skin = 0,            // material id of the y == h skin
  kB_Subsoil = 1,         // material id under the skin (the wedge's topsoil)
  kB_SkinDepth = 2,       // cells of skin, >= 1
  kB_PatchThreshold = 3,  // 0..255 gate on the biome's patch field; 0 = no mask
  kB_PatchCellLog2 = 4,   // log2 of the patch field's cell, in voxels
  kB_TreeTileVox = 5,     // authored tree tile, voxels (the page's number; the
                          // shader places on ONE lattice -- see kB_TreeChanceQ16)
  kB_TreeDensity = 6,     // authored percent of the biome's OWN tiles (informational)
  kB_CoverCount = 7,
  kB_CoverOff = 8,        // word offset of this biome's first cover row
  kB_CaveThreshold1 = 9,  // near-surface cave band gate (0..255)
  kB_CaveThreshold2 = 10, // deep cave band gate
  // 11 unused (was kB_SedMax, never read: the wedge's cap is the map's
  // terrain.sedMax, kHTerrainSedMax)
  kB_Flags = 12,          // kBF_* below
  kB_MaxCoverH = 13,      // tallest cover row (voxels, jitter included): the
                          // sky-skip and far-blocker ceilings MUST include it,
                          // or a plant above the old fixed margin is never
                          // written by a skipped chunk and sits above the far
                          // field's flagged top (far-fog gate, 2026-09-04)
  // P-D (docs/PLAN_environment_truth.md): the biome's thinning chance on the
  // world's ONE tree lattice, Q16 (65536 = every lattice tile). Computed by
  // biomes.h TreeChanceQ16 from density and tile against the finest authored
  // tile, so trees per hectare match the page's densityStats by construction.
  // This is the word treeInfoAt rolls against; kB_TreeDensity is not read.
  kB_TreeChanceQ16 = 14,
  // 15 reserved
  // ---- P-E: the flora that used to be worldgen.* knobs ----
  // P-F: the biome's water rows (assets/biomes/<name>.json water.features[]),
  // packed after the cover rows as kWaterRowWords records: which preset, the
  // thinning chance on the one pond lattice, and the row's conditions. A pond
  // wears the preset of the ROW that rolled it (or of the authored site that
  // placed it), never a biome-wide one.
  kB_WaterCount = 16,
  kB_CaveMushroomChance = 17, // 1-in-N floor cells of the near band; 0 = never
  kB_CaveCrystalChance = 18,  // 1-in-N floor/ceiling cells of the deep band; 0 = never
  kB_CactusChance = 19,       // percent of CACTUS_TILE tiles that grow one (kBF_Cacti gates it)
  kB_SaguaroFraction = 20,    // percent of those that are saguaro columns, not barrels
  kB_WaterOff = 21,           // word offset of this biome's first water row (P-F)
  // ---- P-G: the biome's TERRAIN record (assets/biomes/<name>.json terrain) ----
  // Nine Q14 relief-curve knots (i32 in u32; the identity is -16384 + i*4096)
  // and three Q8 multipliers on the map's hill / detail / grain amplitudes
  // (256 = the map's own). The height mirror reads them through biomeCurve /
  // biomeReliefAt, BILINEARLY over the four biome cells around the column (the
  // biome plane's own lattice, map v2), so two biomes' curves crossfade over a
  // whole 25.6 m cell and never meet on a cliff. Packed by PackBiomeTerrain,
  // the one conversion both the packer and the CPU twin use.
  kB_CurveKnot0 = 22,         // ..30: knots 0..8
  kB_HillMul = 31,
  kB_DetailMul = 32,
  kB_GrainMul = 33,
  // FIRM COVER (cover.firmSkin): the SOLID this biome's loose cover turns
  // into where the ground is too steep to hold a powder. genCellIn ramps a
  // sand cap's loose depth to zero on the same slope gate the sediment wedge
  // uses (kHTerrainSedSlope) and lays this material in the rest of the cap --
  // and lays it over the WHOLE cap on a shore column, where the berm wall is a
  // cliff the analytic noise gradient cannot see. Same treatment for a POWDER
  // skin (desert / ocean author `sand`); a solid skin is untouched.
  // 0 = unset: the biome's subsoil if that is solid, else stone.
  kB_FirmCover = 34,
  // Map v2 A1: the biome's TREELINE SCALE, Q8 (256 = the map's treeline; a
  // biome file's terrain.treelineScale). A column's treeline is
  // sea + (map treeline - sea) * scale, the scale blended over the four biome
  // cells like the relief (worldgen.wgsl biomeTreelineAt): tundra 0.85 puts
  // its trees out 15% lower on a 1,000 m mountain and 1.7 m lower on the
  // harness's 11.6 m one -- a fraction, so one biome file serves both maps.
  kB_TreelineQ8 = 35,
  // 36..47 reserved
  kCoverRowWords = 13,
  kC_Mat = 0,
  kC_Head = 1,
  kC_Chance = 2,          // 1 in N surface columns; 0 = row is off
  kC_HeightVox = 3,       // >= 1
  kC_MinY = 4,            // -1 = unbounded (stored as u32, read as i32)
  kC_MaxY = 5,
  kC_MaxSlope = 6,        // Q8; 0 or >= 1024 = unbounded
  kC_PatchThreshold = 7,  // per-row extra gate on the biome patch field
  kC_NearWaterMax = 8,    // voxels from a pond rim, -1 = unbounded (P-D)
  kC_NearWaterMin = 9,    // at least this many voxels from a rim, 0 = off
  // P-G: the CANOPY condition, what turned the shader's hard-coded
  // undergrowth / flower chain into rows. undergrowthSite's cover 0..255
  // (0 = open sky, 255 = deep under overlapping crowns) must lie in
  // [canopyMin, canopyMax]; 0 / 255 = no bound. A biome with any bounded row
  // carries kBF_CanopyRows so the stack pays the 25-tile scan only there.
  kC_CanopyMin = 10,
  kC_CanopyMax = 11,
  // Map v2 A1: the TREELINE condition (conditions.treeline): 0 = any ground,
  // 1 = only below the column's treeline, 2 = only at or above it. What the
  // shipped rows' absolute maxY 227 / minY 228 meant on a map whose treeline
  // was y 228; relative, it holds on any map at any altitude.
  kC_Treeline = 12,
  // ---- the biome's water rows (P-F) ----
  // Rolled in authored order per pond tile, first hit wins (the cover stack's
  // rule): a row's chance is `(T / tile)^2 / rarity` in Q16 on the shared
  // lattice T (kHPondTile), so bodies per km^2 match the page's rarityStats
  // by construction. Conditions are tested at the pond's CENTRE column.
  kWaterRowWords = 8,
  // At most FOUR live rows per biome: the shader rolls them unrolled, not in
  // a loop (a buffer-bounded loop inside a function the driver inlines ~160
  // times per column path stalled its compile). The packer drops the rest
  // and ValidateBiomeSet says so.
  kWaterRowsMax = 4,
  kR_Preset = 0,          // 1 + index into the water preset table
  kR_ChanceQ16 = 1,       // 65536 = every lattice tile
  kR_MinY = 2,            // -1 = unbounded (i32 in u32)
  kR_MaxY = 3,
  kR_MaxSlope = 4,        // Q8; 0 or >= 1024 = unbounded
  // 5..7 reserved (a water row's conditions.patchThreshold is not packed:
  // the pond roll has no patch gate)
};
// kB_Flags bits: which of genCellIn's fixed-function blocks a biome gets.
inline constexpr uint32_t kBF_GroundFlora = 1u << 0;  // the tile plants (fern banks, big toadstools)
inline constexpr uint32_t kBF_Cacti = 1u << 1;        // the cactus proc shape
inline constexpr uint32_t kBF_SandCap = 1u << 2;      // loose sand cap under the skin (the old desert rule)
inline constexpr uint32_t kBF_CanopyRows = 1u << 3;   // some cover row bounds the canopy cover (P-G): scan the trees once per column
// Water freezes here day and night (biomes::BiomeDef::frozen): worldgen lays
// ice on every water surface, so a cold lake is never born liquid and does not
// freeze over (a world-wide wake) on its first tick (docs/PLAN_temperature.md §8).
inline constexpr uint32_t kBF_Frozen = 1u << 4;

// ---- the water preset table (P-E) --------------------------------------------
// One fixed-stride record per assets/water/<name>.json at kHWaterRecords, in
// the loader's order (sorted file name), followed by the shore plant rows the
// records point into. This is the FLORA half of a preset -- what grows on the
// wet fringe outside the bowl (shore.plants[], shore.mossChance), and what
// grows in the water by depth band (aquatic.emergent / floating / submerged).
// The GEOMETRY half (P-F) follows at words 22..: the disc radius band, the
// bowl depth and its 17-knot profile, the berm, the shore band and the bed.
// Mirrored by worldgen.wgsl's WM_W_* / WM_P_* consts; check_invariants.py
// holds the two together.
//
// THE PROFILE IS PARAMETRISED BY d^2 / r^2, NOT BY d / r. The bowl is
// evaluated per column from `dx*dx + dz*dz` and the shader has no sqrt it may
// use (rule 1); so the preset's `bathymetry.profile` curve (watergen.js
// profileAt, monotone cubic over u = d/r) is sampled at LOAD at the seventeen
// non-uniform radii u_k = sqrt(k / 16), k = 0..16, into Q8 depth fractions
// (256 = the full centre depth, 0 = the rim). The shader then interpolates
// linearly in d^2 between knot floor(16 d^2 / r^2) and the next -- integer
// throughout, exact on both sides of the mirror, and the shape it draws is
// the authored curve within a knot's width. Knots are packed two per word,
// low half first.
//
// Depths are voxels of water over the bed, heights voxels from the bed (the
// aquatic rows) or from the ground (the shore rows); metres in the JSON.
// Every chance is 1-in-N with 0 = never (the shader guards the modulo).
enum : uint32_t {
  kWaterRecWords = 64,
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
  // ---- the geometry half (P-F); voxels unless said otherwise ----
  kW_RadiusMin = 22,           // footprint.radius - radiusV, >= 4
  kW_RadiusSpan = 23,          // 2 * radiusV + 1, >= 1: a rolled disc is min + hash % span
  kW_Depth = 24,               // bathymetry.depth at the centre
  kW_RimDepth = 25,            // bathymetry.rimDepth at the shoreline
  kW_BermH = 26,               // berm.height: the annulus core is forced to surf + this
  kW_BermW = 27,               // berm.width: columns over which the lift ramps out
  kW_ShoreBand = 28,           // shore.band: how far past the rim the wet fringe reaches
  kW_ShoreLift = 29,           // shore.lift: ground this far above the waterline is bluff
  kW_MudWidth = 30,            // shore.mudWidth: the inner ring whose skin is mud
  kW_MudMat = 31,              // shore.mudMaterial id; 0 = the biome skin
  kW_BedShallow = 32,          // bed.shallow id, on the bowl floor under shallow water
  kW_BedDeep = 33,             // bed.deep id, elsewhere on the floor
  kW_BedShallowDepth = 34,     // water shallower than this wears bed.shallow
  kW_BedThickness = 35,        // cells of bed on the floor, >= 1
  kW_BedSubstrate = 36,        // bed.substrate id: the SOLID a face too steep for a powder bed wears
  // 37..39 unused: a preset's placement.maxSlope / minY / maxY are the
  // DEFAULTS the biome page seeds a new water row with; what rolls is the
  // biome row's own conditions (kR_*), so they are not packed
  kW_Band = 40,                // max(kW_ShoreBand, kW_BermW): pondNear's per-disc reach
  kW_Knots = 41,               // 17 Q8 knots, two per word (low half first): words 41..49
  // 50..63 reserved
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
// per-cell SITE INDEX plane (kHSiteIndex) of ONE WORD PER CELL: 0 = no site
// touches this cell, else the word offset of that cell's SITE LIST,
// `[n, id_1 .. id_n]` with 1 <= n <= kSiteCellMax and id = 1 + table index,
// ascending (table order is map.json order, then rule-placed stamps). A site
// is listed in every cell its REACH touches (SiteIndexReach below); cells
// with the same list share one copy. A cell more than kSiteCellMax sites
// reach is a LOAD ERROR naming them -- until P6 (docs/PLAN_map_overhaul.md)
// this was one byte per cell, "first site wins", capped at 254 sites, and a
// second site in a cell silently lost its pad, its keep-out and its lake.
// The no-site column still costs one read: `wmSiteList(x, z)` == 0.
//
// Every reader walks the list and tests the site's own FOOTPRINT, never the
// cell (worldgen.wgsl siteKeepOut / siteBlocksTrunk / wmPadSiteAt /
// wmWaterSiteAt, and their world.cpp twins):
//   stamp  the pad: Chebyshev radius + padMargin around the centre
//   water  the disc + its shore/berm band (Euclidean)
//   tree   the trunk: Chebyshev kSiteTreeKeepOut; the procedural lattice
//          additionally keeps its trunks a crown's width away (reach + reach)
//
// Kinds: kSitePad is the harness box and is NOT in the table (it is the
// WM_H_HARNESS_* header words); kSiteStamp is an authored .vox placed with
// its footprint centred on (x, z), its bottom voxel row at the pad height
// + 1, rotated rot*90 degrees at PACK time so the shader's overlay is one
// column lookup in a run-list -- the tree atlas's encoding (mat | y0 << 16 |
// len << 27), because trees are the proof that a per-cell overlay of
// authored voxels is correct in the far cascades at any distance.
// kSiteWater (P-F): an AUTHORED LAKE, Tier A -- `{kind: "water", preset,
// at: [x, z], radius?}` in map.json. The same record: kS_X/Z is the disc
// centre, kS_Radius the disc radius (the preset's, or the site's override),
// kS_PadMargin the preset's shore/berm band (what the site index plane
// reaches), kS_Preset the water preset. The height mirror's waterSiteNear
// finds it through the same per-cell index plane the stamps use, and from
// there it is a Pond like any rolled one -- same bowl, same berm, same shore,
// same flora -- with its centre from the map instead of the seed. It is not
// padded (sitePadAt skips the kind) and its keep-out is the DISC plus the
// band, not its cells, so a lake does not bald a whole cell of forest around
// it (until P6 the TRUNK test still did: treeInfo refused every trunk in the
// lake's cells).
// kSiteTree (P6): ONE AUTHORED TREE -- `{kind: "tree", species, at: [x, z],
// variant?, rot?}`. A species from the atlas (resolved by name at load to the
// atlas's index, treeatlas.h ReadTreeSpeciesHeaders), standing on the ground
// baked at its trunk (kS_PadY), drawn by the same candidate path as the
// lattice's trees (worldgen.wgsl siteTreesInto), so it is in the near field,
// the far cascades' canopy and the undergrowth cover like any other tree.
// Not grown at or above the treeline: treeMaxTop() is a world-wide bound
// every sky skip relies on.
// kSiteClearing: A FOREST CLEARING -- `{kind: "clearing", min: [x, z],
// max: [x, z], feather?}` in map.json. An axis-aligned box (world voxels,
// inclusive) that no LATTICE tree's CROWN reaches over: a trunk is refused
// when its species' reach square meets the box, and across the `feather`
// band past that (Chebyshev, crown edge to box) trunks are thinned
// deterministically -- kept with probability gap / feather, a counter hash of
// the trunk column -- so the forest edge is ragged, not ruled. That is ALL it
// does: terrain height, ground skin, cover rows, tile plants and ponds are
// untouched (cover under an open sky grows the open-sky rows, because it is
// placed by canopy cover), and it is not a keep-out (siteFootprintHas says no).
// The bush and the dead tree are atlas species, so they are thinned like any
// tree; authored tree sites are the author's and stand. Many per map; the
// record's kS_X / kS_Z are the box's MIN corner, kS_BoxX1 / kS_BoxZ1 its MAX,
// kS_PadMargin the feather. Listed in every cell the box + feather + the
// widest crown reaches, because the lattice asks from the trunk's cell
// (worldgen.wgsl siteBlocksTrunk -> clearingRefuses; the far cascades draw
// trees through the same treeInfoBare, so they agree by construction).
// kSiteSoften (2026-09-29): SOFTENED GROUND -- `{kind: "soften", min: [x, z],
// max: [x, z], feather?, hills?, bumps?, grain?}` in map.json. A box (world
// voxels, inclusive) inside which the three LOCAL octaves of the terrain are
// scaled: `hills` (the 12.8 m swells), `bumps` (the 3 m detail) and `grain`
// (the sub-metre grain), each a percent 0..100 of the biome's own amplitude
// (defaults 40 / 20 / 0). The landform and the coarse range rung are left
// alone, so a softened village keeps its place on the hillside; the scale
// eases back to 100 % across the feather (smoothstep, Chebyshev from the
// box). Applied inside the height mirror (landAt, both sides) through
// softenAt, so the ground, the trees on it, the CPU heights and the far
// cascades all agree. kS_X / kS_Z the MIN corner, kS_BoxX1 / kS_BoxZ1 the
// max, kS_PadMargin the feather, kS_SoftHill / Bump / Grain the three Q8
// scales (256 = untouched), kS_SoftPath the hill scale ON the map's walking
// routes (`paths`, default 10 %: the waynode links, kSitePath, within
// kRouteSoftInner of the line and easing out by kRouteSoftOuter) -- the
// tracks between the doors are flatter than the green round them, but not
// planed. What it is for: a village that keeps the
// biome's rolling ground at a walkable amplitude with no sub-metre lumps,
// instead of a flattened plateau (docs/EDITOR_GUIDE.md section 14).
// kSitePath (2026-09-29): A TRUNK KEEP-OUT ALONG A WALKING ROUTE -- not in
// map.json: LoadWorldMap derives one per outdoor waynode LINK of the map's
// refs (both ends resolved the way refs_npc.cpp's graph resolves them, slot
// children of structures included), so the procedural forest never plants a
// trunk on the line a villager walks. kS_X / kS_Z one end, kS_BoxX1 /
// kS_BoxZ1 the other, kS_PadMargin the half-width (kPathTrunkKeepOut). A
// trunk whose column is within that distance of the segment is refused
// (siteBlocksTrunk); nothing else reads it.
// kSiteRoad (map v2 R1, 2026-10-08): ONE PIECE OF AN AUTHORED ROAD -- not a
// sites[] kind: map.json `roads[]` (`{id, name?, points: [[x, z], ...],
// width, shoulder, surface, maxGrade, kind?: road|track|path, depth?}`) is
// read, its profile BAKED at load (densified, the bare ground sampled, low-
// passed, grade-limited to maxGrade: the midpoint of the largest
// grade-Lipschitz profile under the ground and the smallest over it, so bumps
// are cut and dips filled by the same amount), and split into pieces of at
// most kSiteRoadPieceMax voxels (routeDist2's i32 bound) wherever a chord
// strays more than half a voxel from the baked profile. kS_X / kS_Z one end,
// kS_BoxX1 / kS_BoxZ1 the other (a route's layout, so routeDist2 reads it);
// kS_RoadYA / kS_RoadYB the baked road surface at the two ends, Q8 voxels;
// kS_Radius the half-width, kS_PadMargin the shoulder (widened at load so a
// cut or a fill never steps more than a voxel a column, like a stamp's ramp,
// up to kSiteRoadShoulderMax); kS_RoadMat the SOLID surface material,
// kS_RoadDepth its cells. Readers: roadPadAt (the height mirror: the road Y
// lerped along the piece, weight 256 over the half-width, a smoothstep to the
// bare ground across the shoulder), roadColAt (genColumn: the surface, with
// a ragged edge), siteFootprintHas (no cover / tile plant / tarn / cactus
// on the carriageway), siteBlocksTrunk (no trunk within half-width + the
// larger of the crown's reach and the shoulder), sitePadNear (no rolled pond
// within half-width + kSiteRoadShoulderMax of the line -- a FIXED distance,
// so the bare ground the profile is baked from cannot depend on the baked
// shoulder), and farSurfaceMat (a road narrower than a far cell still tints
// the cell its line crosses).
// STAMPS carry their FOOTPRINT RECT (2026-09-29): kS_BoxX0 / kS_BoxZ0 ..
// kS_BoxX1 / kS_BoxZ1 the template's own rotated nx x nz box (inclusive,
// what wmStampCell draws into), kS_Apron the flat ring round it. The pad is
// flat over rect + apron and eases back to the terrain over kS_PadMargin more
// with a smoothstep (sitePadAt), and every stamp reaching a column blends in
// turn, so two ramps that meet stay continuous. A lattice trunk is refused
// on the whole pad (rect + apron + margin: the tree's base is the bare
// ground) AND wherever its crown square would come within kStampTreeClear of
// the rect -- the per-tree building rule, so no house needs a clearing.
enum : uint32_t {
  kSiteRecWords = 16,
  kS_Kind = 0,          // kSiteStamp | kSiteWater | kSiteTree | kSiteClearing
  kS_X = 1,             // world voxel centre (i32 in u32)
  kS_Z = 2,
  kS_Radius = 3,        // Chebyshev footprint radius in voxels: keep-out + pad
                        // (a water site: the disc radius; a tree site: the
                        // species' reach, what the lattice keeps clear)
  kS_PadMargin = 4,     // columns over which the pad ramps back to terrain
                        // (a water site: its preset's shore/berm band; a
                        // tree site: 0)
  kS_Rot = 5,           // 0..3 (a stamp: informational, baked into its block;
                        // a tree: kSiteRotRolled = rolled from the seed)
  kS_Salt = 6,
  kS_StampOff = 7,      // word offset of the stamp block, 0 = none
  kS_Preset = 8,        // water site: 1 + index into the water preset table
  kS_PadY = 9,          // pad / stamp / tree site: the BARE ground at (kS_X,
                        // kS_Z) -- landColumnBare's h there, baked by
                        // LoadWorldMap for the seed it loads with
                        // (StampSite::padY). The pad, the stamp and its sky
                        // ceiling, and a tree's trunk read it instead of
                        // re-running landColumnBare per column / per voxel.
  kS_Species = 10,      // tree site: the atlas species index
  kS_Variant = 11,      // tree site: 1 + variant (mod the species' count), 0 = rolled
  kS_Sink = 12,         // stamp site: rows of the template BELOW the pad top + 1
                        // (a structure's footing under GRADE, PLAN_world_editor
                        // P4). 0 for a map.json stamp: its bottom row sits on
                        // the pad, as it always has. <= kStampSinkMax.
  kS_BoxX1 = 13,        // clearing / soften site: the box's MAX corner, inclusive. A
  kS_BoxZ1 = 14,        // clearing's kS_X / kS_Z are its MIN corner (not a
                        // centre) and kS_PadMargin its feather, in voxels.
                        // A stamp: its footprint rect's max; a path: its
                        // second end.
  kS_BoxX0 = 10,        // stamp site: its footprint rect's MIN corner (the
  kS_BoxZ0 = 11,        // words a tree site spends on species / variant)
  kS_Apron = 15,        // stamp site: the flat ring round the rect, voxels
  kS_SoftHill = 8,      // soften site: Q8 scale of the hill octave (256 = untouched)
  kS_SoftBump = 10,     // soften site: Q8 scale of the detail octave
  kS_SoftGrain = 11,    // soften site: Q8 scale of the grain octave
  kS_SoftPath = 12,     // soften site: Q8 scale of the hill octave ON the routes
  kS_RoadYA = 9,        // road piece: the baked surface Y at (kS_X, kS_Z), Q8 voxels
  kS_RoadYB = 10,       // road piece: ... at (kS_BoxX1, kS_BoxZ1), Q8 voxels
  kS_RoadMat = 11,      // road piece: the surface material id (a SOLID)
  kS_RoadDepth = 12,    // road piece: surface cells from the ground's top down
  kS_RoadFlags = 15,    // road piece: 1 = end A open, 2 = end B open (a joint with
                        // the next piece on the same straight: no round cap)
  kS_RoadCut = 8,       // road piece: the JOINT CUT normal of its straight, i16 x |
                        // i16 z << 16 (|n| ~ 1024), shared by every piece of it:
                        // an open end hands over on (c - joint) . n, so two pieces
                        // whose integer ends point a hair apart still split the
                        // plane exactly (bug 2026-10-08_23-42-45: their own
                        // directions left 1-voxel walls of bare ground)
  kStampHdrWords = 4,
  kStamp_NX = 0, kStamp_NY = 1, kStamp_NZ = 2, kStamp_Columns = 3,
  // columns: nx*nz pairs of (runOff, runCount), absolute word offsets; runs:
  // mat (12 bits) | y0 << 16 | len << 27, y0 < 2048, len <= 31, y0 ascending
  kSitePad = 0,
  kSiteStamp = 1,
  kSiteWater = 2,
  kSiteTree = 3,
  kSiteRotRolled = 4,   // kS_Rot of a tree site the map did not turn
  kSiteClearing = 5,    // a forest clearing (kind "clearing"): no crown over
                        // its box, trunks thinned across its feather band
  kSiteSoften = 6,      // softened ground (kind "soften"): the local octaves scaled
  kSitePath = 7,        // a waynode link's trunk keep-out (derived from the refs)
  kSiteRoad = 8,        // one piece of a map.json roads[] entry (map v2 R1)
  // ---- RIVERS AND LAKES (docs/PLAN_worldbuilder.md W1; worldmap.h "rivers") ----
  kSiteRiver = 9,       // one piece of a map.json rivers[] entry: a straight
                        // chord of one REACH (one flat water surface)
  kSiteLakeEdge = 10,   // one edge of a map.json lakes[] polygon (<= kLakeEdgeMax)
  kSiteLakeCell = 11,   // a lake's per-site-cell record: the row parity bitset
  // River piece words (kSiteRiver). kS_X / kS_Z end A (upstream), kS_BoxX1 /
  // kS_BoxZ1 end B; kS_Radius the channel half-width at A, kS_PadMargin the
  // bank width (both ends). Word 7 (kS_StampOff) stays 0: wmSiteTopAt reads it
  // for every kind.
  kS_RivHwB = 5,        // the channel half-width at end B
  kS_RivDepthA = 6,     // water depth at the centreline, end A
  kS_RivDepthB = 8,     // ... end B
  kS_RivSurf = 9,       // the REACH'S water surface Y (flat over the piece)
  kS_RivBed = 10,       // bed material (bits 0..11) | kRiv* flags << 12 | length << 16
  kS_RivLipY = 11,      // a step-start piece: the UPSTREAM reach's surface (the weir it holds)
  kS_RivCutA = 12,      // a flat end A: the shared cut normal, i16 x | i16 z << 16 (|n| ~ 1024)
  kS_RivCutB = 15,      // a flat end B: likewise
  // Lake words (kSiteLakeEdge / kSiteLakeCell). An edge: kS_X / kS_Z and
  // kS_BoxX1 / kS_BoxZ1 its ends. A cell record: kS_X / kS_Z the site cell's
  // MIN corner, kS_BoxX1 / kS_BoxZ1 its max (inclusive). kS_PadMargin the
  // shore band, both kinds.
  kS_LakeId = 8,        // 1 + index into WorldMapData::lakes
  kS_LakeLevel = 9,     // the water surface Y
  kS_LakeBed = 10,      // bed material id (submerged ground)
  kS_LakeShore = 11,    // shore material id (dry ground within the band)
  kS_LakeBits = 12,     // a cell record: word offset of its 32-word row bitset, 0 = none
  kS_LakeFlags = 15,    // a cell record: kLakeCellInside
  kRivFlatA = 1,        // end A is a STEP START (flat cut on kS_RivCutA, weir strip)
  kRivFlatB = 2,        // end B is a STEP END (flat cut on kS_RivCutB)
  kRivProfileV = 4,     // V cross-section (else the parabolic U)
  kLakeCellInside = 1,  // the cell has no edge of this lake and lies inside it
  // How far a weir strip reaches downstream of a step (voxels, along the cut
  // normal), and how far past a channel's edge a different reach's water
  // still forces a column to hold it (the containment margin).
  kRiverLip = 4,
  kRiverPieceMax = 256,      // a piece's longest side (routeDist2's i32 bound)
  kRiverWidthMin = 8, kRiverWidthMax = 1024,
  kRiverDepthMin = 2, kRiverDepthMax = 120,
  kRiverBankMax = 1024,      // the bank never reaches past hw + this
  kRiverStepDefault = 4, kRiverStepMax = 64,
  kRiverWaterfall = 20,      // a step that drops more than this (2 m) is a waterfall
  kLakeEdgeMax = 256,        // a polygon edge's longest side (split at load)
  kLakeBandDefault = 24, kLakeBandMax = 128,
  kWaterBedDepth = 2,        // cells of bed / shore material a water column lays
  // A road piece is at most this long (Chebyshev, voxels): routeDist2's
  // projection products stay inside i32 after its box reject.
  kSiteRoadPieceMax = 256,
  // The widest a road's shoulder may be (authored or widened), voxels; also
  // the FIXED distance past the half-width that keeps rolled ponds off a road.
  kSiteRoadShoulderMax = 64,
  // How far past the half-width a road tints a far cascade cell its line
  // crosses (farSurfaceMat), voxels: the road's index reach covers it.
  kSiteRoadFarTint = 32,
  // Surface cells a road lays when it names no depth, and the most it may.
  kSiteRoadDepthDefault = 3,
  kSiteRoadDepthMax = 8,
  // At most this many sites may reach one map cell (the per-cell list); more
  // is a load error. Was 4 ("two neighbouring buildings, a lake and a tree")
  // until the world editor's P4: a village is ~10 structure refs plus its
  // well, trees and pond in ONE 102 m cell, so 32. 128 since map v2 R1
  // (2026-10-08), measured on `default`: Harrowby's cell held 28 (5 houses,
  // the soften box, 22 waynode routes); its 8 paths as roads made it 42-44,
  // and a 3-road test network through the village 55..93 depending on how
  // finely the profiles cut (the `roads` gate's H line reports it). The shader's list walks iterate the list's own n,
  // bounded by this, and test the kind first, so a long list costs its length
  // in that cell only; a cell no site reaches still costs one read.
  kSiteCellMax = 128,
  // The deepest a stamp may sink (kS_Sink): genCell overlays template cells
  // from h - 1 - kStampSinkMax up, so a deeper footing would be cut off.
  kStampSinkMax = 8,
  // A tree site's own keep-out (siteKeepOut): the trunk and a little, in
  // voxels. No tarn centre, cactus, tile plant or cover row inside it; the
  // ground under the crown keeps its undergrowth like a lattice tree's does.
  kSiteTreeKeepOut = 6,
  // A clearing's feather band when map.json names none, and the widest it
  // may be, in voxels (worldmap.cpp clamps).
  kClearingFeatherDefault = 64,
  kClearingFeatherMax = 512,
  // A soften site's feather when map.json names none (voxels), and its
  // default scales in percent of the biome's own octave amplitudes.
  kSoftenFeatherDefault = 96,
  kSoftenHillDefault = 40,
  kSoftenBumpDefault = 20,
  kSoftenGrainDefault = 0,
  kSoftenPathsDefault = 10,
  // A route flattens the softened ground fully within this many voxels of its
  // line and not at all past kRouteSoftOuter (a smoothstep between).
  kRouteSoftInner = 12,
  kRouteSoftOuter = 36,
  // No lattice tree's CROWN comes within this many voxels of a stamp's
  // footprint rect (the per-tree building rule, siteBlocksTrunk).
  kStampTreeClear = 10,
  // A waynode link keeps trunks this far from its line (voxels, 1.5 m).
  kPathTrunkKeepOut = 15,
};

}  // namespace worldmap

namespace biomes { struct BiomeSet; struct BiomeDef; struct WaterPresetDef; }
namespace dungeon { struct Shell; struct Resolved; }

namespace worldmap {

// ---- the map's terrain (P-G): map.json `terrain`, as the engine holds it ----
// Every field is the kHTerrain* word of the same name, in the units the
// header comment gives (voxels at the LIVE voxel size, log2 shifts, Q8).
// `TerrainWords` is the ONE packing; LoadWorldMap keeps the result on
// WorldMapData::terrainWords for the CPU twin, PackWorldMap writes it into
// the header, so the two sides read the same integers by construction.
struct TerrainParams {
  int refVoxelsPerMetre = 10;     // the scale the map's lengths are authored at
  int baseHeight = 200;
  int rangeAmplitude = 256, rangeLog2 = 9;
  int hillAmplitude = 64, hillLog2 = 7;
  int detailAmplitude = 16, detailLog2 = 5;
  int grainAmplitude = 4, grainLog2 = 3;
  int fbmAtten = 256;
  int treelineJitter = 0;
  int rockSlope = 640;
  int homeR = 320, homeFade = 2048;
  int sedAbove = 64, sedFraction = 64, sedStrip = 6, sedSlope = 96, sedMax = 32, sedTopsoil = 4;
  int treeline = 228;
};
inline constexpr uint32_t kTerrainWords = kHTerrainRefVpm + 1 - kHTerrainBaseHeight;
/** The terrain as header words kHTerrainBaseHeight..kHTerrainRefVpm. */
void TerrainWords(const TerrainParams& t, uint32_t out[kTerrainWords]);
/** The map's terrain numbers (its treeline, home area, reference scale), for
 *  the CPU code that used to read worldgen.treeline / refVoxelsPerMetre.
 *  The loaded map's, or the defaults above when no map is loaded. A COLUMN's
 *  treeline is TreelineAt, not `treeline`. */
const TerrainParams& CurrentTerrain();
/** A biome's treeline scale in Q8 (terrain.treelineScale, 1.0 = 256),
 *  clamped 0..1024. The packer and the loader both call it. */
int BiomeTreelineQ8(const biomes::BiomeDef& b);
/** The window's chunk Y for a view centred on ground at `groundY` (map v2 A1):
 *  quantised to half a window (256 voxels) so that ground in [72, 328) -- the
 *  harness map's ~y 200 -- is window 0 exactly as every scene had it, and a
 *  valley at y 9,000 gets a window that holds it with >= 72 voxels below and
 *  >= 184 above. Every boot / scripted-scene / perfsuite origin uses it. */
int WindowChunkYForGround(int groundY);

// ---- a biome's terrain record (P-G): the kB_CurveKnot0..kB_GrainMul words ----
inline constexpr uint32_t kBiomeTerrainWords = kB_GrainMul + 1 - kB_CurveKnot0;
struct BiomeTerrainPacked { uint32_t w[kBiomeTerrainWords] = {}; };

// ---- the geometry half of a water preset, as the engine holds it (P-F) ----
// ONE conversion from assets/water/<name>.json to voxel integers + the sampled
// profile, used by the packer (PackBiomeTable -> the kW_* words) AND kept on
// the loaded map (WorldMapData::water) for the CPU twin of the height mirror,
// so World::TerrainHeight and the shader read the same integers by
// construction. Every field is what the kW_* word of the same name holds.
struct WaterGeom {
  int radiusMin = 4, radiusSpan = 1;
  int depth = 1, rimDepth = 0;
  int bermH = 0, bermW = 1;
  int shoreBand = 0, shoreLift = 0, mudWidth = 0;
  int bedShallowDepth = 0, bedThickness = 1;
  int band = 1;                          // max(shoreBand, bermW)
  uint32_t fill = 0, mudMat = 0, bedShallow = 0, bedDeep = 0, bedSubstrate = 0;
  int knots[17] = {};                    // Q8 depth fractions at u_k = sqrt(k/16), non-increasing
};
/** The preset's geometry in voxels, profile sampled, every ceiling applied. */
WaterGeom WaterGeomOf(const biomes::WaterPresetDef& w);
/** The steepest per-column drop of the sampled bowl at radius `r`, in Q8
 *  voxels per voxel of radius (256 = one voxel per column, the CA's angle
 *  of repose). A face steeper than that gets the substrate instead of the
 *  powder bed (genCellIn), so this is a report, never a refusal. */
int WaterGeomSteepestQ8(const WaterGeom& g, int r);
/** One packed water row of a biome (kR_*). */
struct WaterRowPacked { uint32_t w[kWaterRowWords] = {}; };
/** The pond lattice: the finest `water.features[].tile` among biomes that
 *  author a row that can roll, in voxels; 0 when none does. Pure. */
int PondLatticeVox(const biomes::BiomeSet& set);
/** The widest max(shore.band, berm.width) over every preset, in voxels. */
int PondBandVox(const biomes::BiomeSet& set);
/** A biome's water rows packed for the lattice (chance thinned, conditions
 *  in voxels). Rows that name no loaded preset or cannot roll are dropped. */
std::vector<WaterRowPacked> PackWaterRows(const biomes::BiomeSet& set, const biomes::BiomeDef& b, int latticeVox);
/** 1 + index of the preset by name, 0 if the set has none. */
uint32_t WaterPresetIndex(const biomes::BiomeSet& set, const std::string& name);
/** A biome's terrain record (curve knots clamped to +-16384, Q8 multipliers
 *  clamped to 0..4096). Pure; the packer and the loader both call it. */
BiomeTerrainPacked PackBiomeTerrain(const biomes::BiomeDef& b);
/**
 * An .svsculpt file's bytes -> the packed sculpt block (layout above). An
 * empty input, a file with no tiles, or tiles that are all zero give an EMPTY
 * block and true: no layer. False, with `log`, on a malformed file (bad magic,
 * version, geometry, size, a duplicate tile, or a directory past
 * kSculptMaxRegions). Pure; LoadWorldMap and the `sculpt` gate call it.
 */
bool PackSculpt(const uint8_t* data, size_t n, std::vector<uint32_t>& block,
                int* tilesOut, std::string& log);

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

// ---- locales (docs/PLAN_map_v2.md L1) ----------------------------------------
// A LOCALE is a hand-made place on the map (a village, a witch's hut):
// map.json `locales[]`, {id, name, icon, bounds: {min: [x, z], max: [x, z]},
// anchor: {at: [x, z], y}, editLayer?}. Two things belong to it:
//
//   * its REF GROUP, refs/<id>.json (group name == locale id). Those refs are
//     STORED with pos.y relative to anchor.y. At load the locale's delta is
//     BareGroundHeight(anchor.at, seed) - anchor.y, and every ref of the group
//     lands at file y + anchor.y + delta == file y + groundY: structures (their
//     stamp padY), waynodes, NPCs, markers. Reshaping the world under a locale
//     moves the whole locale RIGIDLY with the ground at its anchor.
//   * its EDIT LAYER, locales/<id>/<editLayer or "ground">.svedit (SVED v2,
//     ground-relative per column like the map's own; absent = none), applied
//     after the map's layer in table order (sim/worldedit.h).
//
// The sculpt layer is an offset and edit-layer cells are per-column relative,
// so neither needs the delta.
struct LocaleDef {
  std::string id, name, icon;
  int x0 = 0, z0 = 0, x1 = 0, z1 = 0;   // bounds, world voxels, inclusive
  int ax = 0, az = 0;                   // anchor.at
  int ay = 0;                           // anchor.y: the ground there when it was authored
  std::string editLayer;                // file stem under locales/<id>/ ("ground" when unnamed)
  // RESOLVED by LoadWorldMap for its seed: the bare ground at the anchor.
  int groundY = 0;
  bool resolved = false;
  int Delta() const { return resolved ? groundY - ay : 0; }
  // What a ref of this locale's group adds to its FILE y.
  int RefYOffset() const { return ay + Delta(); }
};
// map.json's `locales` of <assetDir>/worldmap/<map> (unresolved), table order.
// False + `err` on a malformed entry; a missing file or key is zero locales.
bool ReadLocales(const std::string& assetDir, const std::string& mapName,
                 std::vector<LocaleDef>& out, std::string& err);
// group -> the y offset its refs' FILE y take (LocaleDef::RefYOffset), for
// every locale of the map. Resolved from CurrentWorldMap() when it is that
// map; otherwise anchor.y (delta 0) and a line in `warn` saying the locale
// sits at its authored height because the world map is not loaded.
std::map<std::string, int> LocaleRefYOffsets(const std::string& assetDir, const std::string& mapName,
                                             std::vector<std::string>* warn = nullptr);
// Every edit-layer FILE a map applies, in application order: the map's own
// (assets/worldedits/<editLayer>.svedit) when named, then each locale's that
// exists on disk. (path, name) -- `name` is what the save fingerprint and the
// environment stamp hash the bytes under ("<layer>.svedit",
// "<locale>/<stem>.svedit").
// Then (PLAN_dungeons §3.6) each dungeon ASSET's detail layer
// (assets/dungeons/<asset>/detail.svedit, SVED v3) that exists, in
// `dungeonAssets` order (duplicates once): named "dungeon:<asset>/detail.svedit".
std::vector<std::pair<std::string, std::string>> MapEditLayerFiles(const std::string& assetDir,
                                                                   const std::string& mapName,
                                                                   const std::string& editLayer,
                                                                   const std::vector<LocaleDef>& locales,
                                                                   const std::vector<std::string>& dungeonAssets = {});
// The asset dir names of map.json's dungeons[] (PLAN_dungeons §3.1), table
// order, read from disk ("" entries skipped; a malformed list reads as none).
std::vector<std::string> ReadDungeonAssets(const std::string& assetDir, const std::string& mapName);
// The same for a loaded map: its dungeons[]' assets, placed or not.
struct WorldMapData;
std::vector<std::string> DungeonAssetsOf(const WorldMapData& m);
// sandvox::HashOneFile over those files, folded in order (h * FNV prime ^ next):
// exactly HashOneFile of the map layer when the map has no locale layers.
uint32_t HashEditLayerFiles(const std::vector<std::pair<std::string, std::string>>& files);

// ---- dungeons (docs/PLAN_dungeons.md §3.1; sim/dungeon.h) --------------------
// map.json `dungeons[]`: {id, name?, dungeon?, at: [x, z], yOffset?, yaw?}. The
// asset assets/dungeons/<dungeon or id>/ (shell.bin, else shell.json) is
// loaded and its materials resolved by LoadWorldMap; a missing or refused
// asset is a LOAD WARNING and the dungeon is not placed. The anchor resolves
// like a locale's: the bare ground at `at` for the load seed; the shell's
// anchor cell lands on (at.x, groundY + yOffset, at.z) and every local cell at
// that + R(yaw) (local - anchor), R the LINEAR quarter turn (dungeon.h RotX),
// the rule D3's refs use (PLAN §3.7).
struct DungeonDef {
  std::string id, name, asset;
  int ax = 0, az = 0, yOffset = 0, yawQ = 0;
  // RESOLVED by LoadWorldMap for its seed.
  int groundY = 0;
  bool resolved = false;
  std::array<int, 3> origin{};               // world cell of the shell's local (0,0,0)
  std::array<int, 3> boxMin{}, boxMax{};     // world, inflated, inclusive
  std::array<int, 3> keepMin{}, keepMax{};   // caves + lava off in here (max < min: none)
  std::vector<std::array<int, 4>> surf;      // surface rects x0 z0 x1 z1 (dungeon.h Placed::surf)
  std::string shellPath;                     // what was loaded
  uint32_t shellHash = 0;                    // FNV of its bytes (the map's content hash folds it)
  std::shared_ptr<const dungeon::Shell> shell;   // null = not placed
  std::shared_ptr<const dungeon::Resolved> res;
  bool Placed() const { return shell != nullptr && res != nullptr && resolved; }
};
// THE SCRATCH DUNGEON (voxserve DUNGEON, PLAN §5.0): when set, LoadWorldMap
// places this program too -- under `id` (replacing a map entry of that id, at
// that entry's anchor, when `replaceMapEntry`) or at its own anchor. The
// tuner's preview path; null clears it.
struct ScratchDungeon {
  DungeonDef def;                            // id, at, yOffset, yawQ (+ shell / res set)
  bool replaceMapEntry = false;
  // Anchor y given absolutely (ISOLATE): the anchor cell lands at this world y
  // instead of groundY + 1 + yOffset.
  bool absoluteY = false;
  int anchorY = 0;
};
void SetScratchDungeon(const ScratchDungeon* s);
const ScratchDungeon* ScratchDungeonSlot();
// Place `g` (its shell + res set) for `seed` on the CURRENT map's bare ground:
// origin, boxes, surface rects, `resolved`. False + `warning` when the
// program cannot be packed (then it is left unplaced). LoadWorldMap's rule.
bool AnchorDungeon(DungeonDef& g, uint32_t seed, const std::string& mapName, std::string& warning);
// The voxel server's dungeon preview without a map reload: `m` is a copy of a
// map LoadWorldMap built with a scratch program of the SAME shape installed
// (same id, same replaceMapEntry); its entry takes the scratch now installed,
// is re-anchored, and the content hash is re-folded -- the map LoadWorldMap
// would build, at ~1/30 of the cost (the plane tiles, roads and rivers are
// what a full load spends 1.4 s on, and none of them depend on a dungeon).
// False when `m` has no entry of the scratch's id.
bool ReplaceScratchDungeon(WorldMapData& m, uint32_t seed, std::string& log);
// The gates' switch: when false, PackWorldMap packs the dungeon block with its
// EVALUATOR OFF (dungeon.h kDgFlagEvalOff): the keep boxes still turn the caves
// off and the sky ceilings still rise, but no cell is carved -- the
// `dungeon-gen` gate's terrain arm, the exact input the CPU twin is fed.
// Default true.
void SetDungeonsEnabled(bool on);
bool DungeonsEnabled();
// The `dungeon-gen` gate's other switch: when false the surface rects are
// packed with dungeon.h kDgFlagSurfOff, so trees and cover grow over the
// dungeons' surface footprints as if there were none (the "what would have
// been cut" arm). Default true.
void SetDungeonSurfaceKeep(bool on);
bool DungeonSurfaceKeep();

// ---- the loaded map (P2) ---------------------------------------------------
/** assets/worldmap/<name>/{map.json,map.svmap}, parsed and resolved. */
struct WorldMapData {
  // Even an UNLOADED map carries the default terrain words: the CPU twin reads
  // them through CurrentWorldMap() before any map is loaded (tools, the
  // heightmap route), and a zero homeFade there is a division by zero.
  WorldMapData() { TerrainWords(terrain, terrainWords); }
  std::string name;
  // ---- map v2: the extent and the planes (worldmap.h kHExtent* / kHHeight* /
  // kHBiome*). Posts (i, k) of a plane of log2 L sit at world
  // (extentX0 + (i << L), extentZ0 + (k << L)), i = 0 .. extentW >> L inclusive.
  int extentX0 = 0, extentZ0 = 0, extentW = 0, extentH = 0;
  int heightLog2 = 6, heightPostsX = 0, heightPostsZ = 0;
  int heightDefault = 0;                  // planes.height.default, voxels
  std::vector<int16_t> height;            // heightPostsX * heightPostsZ, z-major, absolute voxels
  int biomeLog2 = 8, biomePostsX = 0, biomePostsZ = 0;
  std::vector<uint8_t> biome;             // biomePostsX * biomePostsZ, RESOLVED to biome ids
  int biomeDefault = 0;                   // id of planes.biome.default (outside the extent too)
  int edgeFadeVox = 0;
  int heightTiles = 0, biomeTiles = 0;    // authored (present) tile files
  // ---- the site index lattice (map.json siteCellLog2) ----
  int siteCellLog2 = 10;
  int siteW = 0, siteH = 0;               // the extent in site cells
  int siteOriginX = 0, siteOriginZ = 0;   // site cell index of the cell whose low corner is world (0,0)
  int seaLevelY = 0;
  int warpAmpVox = 0;
  // The calm pad box, world voxels, inclusive (worldgen.wgsl inPadBox). From
  // map.json sites[] kind "pad"; a map without one gets an empty box (x1 < x0).
  int padX0 = 0, padZ0 = 0, padX1 = -1, padZ1 = -1;
  bool InPadBox(int x, int z) const {
    return x >= padX0 && x <= padX1 && z >= padZ0 && z <= padZ1;
  }
  // The spawn site (kind "spawn"). `spawnAuthored` says the map named it;
  // otherwise these are the (140, 140) default and the loader said so.
  int spawnX = 140, spawnZ = 140;
  bool spawnAuthored = false;
  std::vector<std::string> palette;       // map.json biomes[]: plane byte -> name
  // Authored sites (P5, P-F, P6), resolved: a stamp's template loaded,
  // rotated and packed into columns of runs at load; a lake's preset; a
  // tree's species. `siteIndex` / `siteLists` are the per-cell lists.
  struct StampSite {
    std::string id, templateName;         // templateName: stamp .vox / water preset / tree species
    int kind = kSiteStamp;                // kSiteStamp | kSiteWater (P-F) | kSiteTree (P6) | kSiteClearing
    // A CLEARING: (x, z) is the box's MIN corner and (x1, z1) its max,
    // inclusive; padMargin is the feather. Every other kind: (x, z) is the centre.
    int x = 0, z = 0, radius = 0, padMargin = 8, rot = 0;
    int x1 = 0, z1 = 0;
    // A stamp: its footprint rect's min corner (x1 / z1 are its max) and the
    // flat apron round it. A soften site: the three Q8 octave scales.
    int bx0 = 0, bz0 = 0, apron = 0;
    int softHill = 256, softBump = 256, softGrain = 256, softPath = 256;
    uint32_t salt = 0;
    int preset = 0;                       // water site: 1 + preset index
    int species = 0, variant = 0;         // tree site: kS_Species / kS_Variant
    // Pad / stamp / tree site: the bare ground at the centre for the LOAD
    // seed (kS_PadY). Position-fixed and seed-dependent, so LoadWorldMap bakes
    // it once with World::BareGroundHeight; 0 on a water site (never read).
    int padY = 0;
    // How far past (x, z) the site is LISTED in the per-cell index, Chebyshev
    // voxels: every reader's footprint test must fit inside it. Not packed.
    int indexReach = 0;
    int nx = 0, ny = 0, nz = 0;
    std::vector<uint32_t> words;          // the packed stamp block, offsets RELATIVE to its start
    // A STRUCTURE REF's site (PLAN_world_editor P4, world/structures.h): its
    // padY is the AUTHORED floor (ref pos.y - 1), not the baked bare ground,
    // and its template sinks `sink` rows below the pad top + 1. `base`,
    // `refPos` and `refYaw` are the ref's own fields, kept so the live
    // re-apply can tell an in-step site from a stale one.
    bool structure = false;
    int sink = 0;
    std::string base;
    int refX = 0, refY = 0, refZ = 0, refYaw = 0;
    int refPadMargin = -1, refPadApron = -1;   // the ref's props as written (-1 = default)
    // L1: index into `locales` when the structure ref is in a locale's group
    // (its refY / padY then carry the locale's offset), else -1.
    int locale = -1;
    // A ROAD PIECE (kSiteRoad): (x, z) .. (x1, z1) its two ends, radius the
    // half-width, padMargin the shoulder; the baked surface Y at the ends in
    // Q8 voxels, the surface material and its depth in cells. `road` is the
    // index into WorldMapData::roads it was cut from.
    int roadYA = 0, roadYB = 0;
    uint32_t roadMat = 0;
    int roadDepth = 0;
    int roadFlags = 0;                    // kS_RoadFlags
    int roadCut = 0;                      // kS_RoadCut (packed like cutA)
    int road = -1;
    // A RIVER PIECE (kSiteRiver, W1): radius = the half-width at A, padMargin
    // the bank; these are the kS_Riv* words. A LAKE EDGE / CELL: `surf` the
    // level, `bed` / `shore` the materials, `bits` a cell's row parity.
    int hwB = 0, depthA = 0, depthB = 0, surf = 0, lipY = 0, len = 0;
    int wflags = 0;                       // kRiv* / kLakeCellInside
    int cutA = 0, cutB = 0;               // packed i16 x | i16 z << 16
    uint32_t bed = 0, shore = 0;
    int water = -1;                       // index into rivers / lakes
    std::vector<uint32_t> bits;           // a lake cell: 32 words, bit z = row parity
  };
  std::vector<StampSite> sites;           // stamps, water and tree sites, in site-id order
  // map.json roads[] (map v2 R1), as authored plus what the load baked: the
  // profile at every polyline VERTEX (the surface Y, voxels, and the bare
  // ground there) -- what the map page's profile preview shows, the engine's
  // bake being the truth. The pieces themselves are kSiteRoad sites, at the
  // END of `sites` (after every other kind), in road order.
  struct Road {
    std::string id, name, kind = "road", surface;
    bool smooth = false;                       // the points went through THE SPLINE (W1)
    std::vector<std::pair<int, int>> points;   // world voxels
    int width = 30, shoulder = 16, maxGrade = 12, depth = kSiteRoadDepthDefault;
    uint32_t surfaceId = 0;
    std::vector<int> vertexY;                  // baked surface Y at each point
    std::vector<int> vertexGround;             // bare ground at each point
    int pieces = 0;
    int worstGradeQ8 = 0;                      // steepest baked step, percent * 256
    // The baked profile along the whole polyline, decimated to at most
    // kRoadProfileMax samples (the bake's own every-4-voxel samples, every
    // k-th one, the last always kept): arc length from the first point,
    // the road surface Y and the bare ground there, voxels. --mapcheck hands
    // it to the map page's profile chart.
    std::vector<int> profS, profY, profG;
  };
  std::vector<Road> roads;
  // map.json rivers[] (docs/PLAN_worldbuilder.md 2.1, W1), as authored plus
  // what the load baked. The pieces are kSiteRiver sites at the END of
  // `sites`, after the roads, in river order. See "RIVERS AND LAKES" above
  // BakeWater for the bake.
  struct River {
    std::string id, name, bedName, joins, profile = "U";
    // [x, z, w, d] per knot; w / d = -1 where the knot named neither
    // (interpolated by arc length at load).
    std::vector<std::array<int, 4>> knots;
    bool smooth = false;
    int stepVox = kRiverStepDefault;
    int bank = -1;                       // -1 = 1.5 x the local width
    uint32_t bedId = 0;
    // ---- baked ----
    int joinIdx = -1;                    // index of the parent in `rivers`
    int lakeIn = -1, lakeOut = -1;       // the lake its source / mouth lies in
    int endLevel = 0;                    // the surface it drains to
    int pieces = 0, reaches = 0;
    std::vector<std::array<int, 3>> falls;   // waterfalls: x, z, drop (voxels)
    // The bake along the centreline, decimated to <= kRoadProfileMax samples:
    // arc length, the water surface, the bare ground (the map page's chart).
    std::vector<int> profS, profY, profG;
    // The full-resolution baked centreline (every 4 voxels): x, z, surface,
    // half-width. Not reported; the horizon and the gate read it.
    std::vector<std::array<int, 4>> line;
  };
  std::vector<River> rivers;
  // map.json lakes[] (2.2): a polygon filled to `level`. `poly` is the
  // tessellated outline as the engine reads it (closed, edges <= kLakeEdgeMax).
  struct Lake {
    std::string id, name, bedName, shoreName;
    int level = 0, band = kLakeBandDefault;
    bool smooth = false;
    std::vector<std::pair<int, int>> points, poly;
    uint32_t bedId = 0, shoreId = 0;
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;    // poly's bounding box
    int cells = 0, edges = 0;
    /** Even-odd inside test at column (x, z) -- the predicate the shader's
     *  lake cell records reproduce exactly (worldgen.wgsl lakeInside). */
    bool Inside(int x, int z) const;
  };
  std::vector<Lake> lakes;
  // map.json regions[] (map v2 E3): NAMED POLYGONS, data only -- nothing in
  // worldgen reads them; RegionAt answers "which region is this column in"
  // for later systems (quest text, map labels, spawn tables). Points are world
  // voxels, the polygon closes itself, colour is the map page's "#rrggbb".
  struct Region {
    std::string id, name, colour;
    std::vector<std::pair<int, int>> points;
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;        // bounding box, inclusive
  };
  std::vector<Region> regions;
  // map.json markers[] (map v2 E3): map ANNOTATIONS, `{id, name, icon, at}`;
  // nothing in the engine reads them. `icon` is one of kMarkerIcons (an
  // unknown one is a warning; the page draws a plain pin).
  struct Marker {
    std::string id, name, icon;
    int x = 0, z = 0;
  };
  std::vector<Marker> markers;
  /** The region holding world column (x, z) -- the LAST in table order whose
   *  polygon contains it (the map page draws later regions on top), even-odd
   *  rule, integer arithmetic; nullptr when none does. */
  const Region* RegionAt(int x, int z) const;
  // LOAD WARNINGS (P6): everything the loader skipped or thinks is a mistake
  // but will not refuse a boot over -- an unknown site kind, a stamp .vox or
  // tree species that is not there, two footprints that overlap, a tree at
  // the treeline, the spawn inside a site. Printed at load ("world map
  // WARNING: ...") and surfaced on the tuner's World map page (--mapcheck).
  std::vector<std::string> warnings;
  // The water table the height mirror's CPU twin reads (worldgen.wgsl reads
  // the kW_* / kR_* words; these are the same integers): one WaterGeom per
  // preset in loader order, each biome's packed rows by biome id, the lattice
  // and the scan band. Filled by LoadWorldMap from the set it resolved the
  // palette against, so the two sides cannot see different presets.
  std::vector<WaterGeom> water;
  std::vector<std::vector<WaterRowPacked>> biomeWater;
  int pondTile = 0, pondBand = 0;
  // P-G: the map's terrain (map.json `terrain`), rescaled to the live voxel
  // size, and the same as header words; each biome's terrain record by id.
  TerrainParams terrain;
  uint32_t terrainWords[kTerrainWords] = {};
  std::vector<BiomeTerrainPacked> biomeTerrain;
  // Each biome's treeline scale by id (BiomeTreelineQ8; kB_TreelineQ8), for
  // the CPU twin of worldgen.wgsl biomeTreelineAt.
  std::vector<int> biomeTreelineQ8;
  // Each biome's climate by id (biomes::BiomeDef::ambient), for heat.cpp, and
  // its name by id (the F1 temperature readout: `palette` is indexed by the
  // map's plane byte, not by biome id).
  std::vector<HeatClimate> biomeClimate;
  std::vector<std::string> biomeName;
  // P5: the sculpt layer as the PACKED BLOCK both sides read (PackSculpt;
  // offsets relative to its first word). Empty = the map has no layer, and
  // PackWorldMap then leaves kHSculpt at 0. The CPU twin (world.cpp
  // sculptOctave) reads these words with the shader's arithmetic.
  std::vector<uint32_t> sculpt;
  int sculptTiles = 0;                    // authored non-zero tiles in the file
  // The per-cell site lists (see "the site table" above), CPU form: one entry
  // per cell, 0 = no site, else an offset into `siteLists`, where the list is
  // [n, id_1 .. id_n] (id = 1 + index into `sites`). siteLists[0] is a pad
  // word so 0 can mean "none". PackWorldMap rebases the offsets onto the
  // buffer; the CPU twins read these directly.
  std::vector<uint32_t> siteIndex;        // siteW * siteH
  int siteCellWorst = 0;                  // the longest cell list (<= kSiteCellMax), for --mapcheck
  std::vector<uint32_t> siteLists;
  /** The list of cell (cx, cz): nullptr = no site, else [n, ids...]. */
  const uint32_t* SiteList(int cx, int cz) const {
    if (siteIndex.empty()) return nullptr;
    const uint32_t off = siteIndex[static_cast<size_t>(cz) * siteW + cx];
    return off == 0u ? nullptr : siteLists.data() + off;
  }
  uint32_t contentHash = 0;               // FNV-1a over map.json, every tile, the sculpt layer, the refs
  uint32_t contentHashSansDungeons = 0;   // the same, before the dungeons' programs are folded in
  // P4: the structure refs' placements + their assets' bytes (0 = none, and
  // then contentHash is exactly what it was before structures existed).
  uint32_t structureHash = 0;
  // map.json `editLayer` (P7): assets/worldedits/<name>.svedit, applied over
  // this map's worldgen (sim/worldedit.h). "" = none. A bare name, never a path.
  std::string editLayer;
  // map.json `locales[]` (L1), table order, RESOLVED for the load seed.
  std::vector<LocaleDef> locales;
  // map.json `dungeons[]` (PLAN_dungeons §3.1), table order, then the scratch
  // dungeon if one is set. Only Placed() ones are packed.
  std::vector<DungeonDef> dungeons;
  bool Loaded() const { return extentW > 0 && extentH > 0; }
  /** SITE cell coordinates of a world voxel column (arithmetic shift: floors). */
  void CellOf(int x, int z, int* cx, int* cz) const {
    *cx = (x >> siteCellLog2) + siteOriginX;
    *cz = (z >> siteCellLog2) + siteOriginZ;
  }
  bool Inside(int cx, int cz) const {
    return cx >= 0 && cz >= 0 && cx < siteW && cz < siteH;
  }
  /** Is world column (x, z) inside the authored extent (max edge exclusive)? */
  bool InExtent(int x, int z) const {
    return x >= extentX0 && z >= extentZ0 && x < extentX0 + extentW && z < extentZ0 + extentH;
  }
  /** The biome id of biome post (i, k), clamped to the plane. */
  uint8_t BiomePost(int i, int k) const {
    i = i < 0 ? 0 : (i >= biomePostsX ? biomePostsX - 1 : i);
    k = k < 0 ? 0 : (k >= biomePostsZ ? biomePostsZ - 1 : k);
    return biome[static_cast<size_t>(k) * biomePostsX + i];
  }
  /** The biome CELL holding world column (x, z), no warp: the post whose cell
   *  [X0 + i << L, X0 + (i + 1) << L) holds it; outside the extent, the default.
   *  worldgen.wgsl biomeCellAt's arithmetic. */
  uint8_t BiomeCellAt(int x, int z) const {
    if (!Loaded() || biome.empty()) return 0;
    if (!InExtent(x, z)) return static_cast<uint8_t>(biomeDefault);
    return BiomePost((x - extentX0) >> biomeLog2, (z - extentZ0) >> biomeLog2);
  }
  /** The authored height post (i, k), clamped to the plane. */
  int HeightPost(int i, int k) const {
    i = i < 0 ? 0 : (i >= heightPostsX ? heightPostsX - 1 : i);
    k = k < 0 ? 0 : (k >= heightPostsZ ? heightPostsZ - 1 : k);
    return height[static_cast<size_t>(k) * heightPostsX + i];
  }
};

/**
 * Load assets/worldmap/<name>/ and resolve its palette against the biome
 * set. False, with `log`, on a missing directory, a malformed file, a v1 map
 * (map.svmap / no "version": 2 -- the message names scripts/convert_map_v2.py),
 * a palette name no biome file has, a tile whose header disagrees with its
 * plane, or a cell more than kSiteCellMax sites reach -- the caller ABORTS
 * on false (a world with no map is not a world, see the header comment).
 * What it skips or doubts without refusing is in `out.warnings` (and stderr).
 *
 * `mapJson`, when given, is used INSTEAD of <name>/map.json (the planes and
 * the sculpt layer still come from <name>/): the `worldmap` gate's synthetic
 * site cases load that way, so they touch no file.
 */
bool LoadWorldMap(const std::string& assetDir, const std::string& name,
                  const biomes::BiomeSet& set, size_t materialCount, uint32_t seed,
                  WorldMapData& out, std::string& log,
                  const std::string* mapJson = nullptr);

// ---- structure refs as sites (PLAN_world_editor P4, world/structures.h) ------
// One `structure` ref, as LoadWorldMap reads it out of the map's refs group
// files: the placement only; the asset is resolved (and refused with a
// warning when missing / mis-yawed) by the loader.
struct StructurePlacement {
  std::string id, base, file;   // file: "refs/<group>.json", for warnings
  std::string group;            // the group file's stem (== a locale id when it is one)
  int x = 0, y = 0, z = 0;      // the ref's pos: the asset's origin lands here
  int yaw = 0;
  int padMargin = -1;           // props.padMargin, -1 = the default
  int padApron = -1;            // props.padApron, -1 = the default
};
// Default pad ramp for a structure, voxels (props.padMargin overrides, 1..64):
// the smoothstep from the flat apron back to the terrain.
constexpr int kStructurePadMargin = 24;
// Default flat ring round a structure's footprint, voxels (props.padApron
// overrides, 0..32): the doorsteps, level with the floor's threshold.
constexpr int kStructurePadApron = 8;
// THE GATES' SEAM: when set, LoadWorldMap uses this list INSTEAD of reading
// the map's refs directory -- so a gate can place a house on the harness map
// (whose refs dir is the refs gates' fixture) through the real loader, the
// real ReloadEnvironment and the real re-apply. Null restores the files.
void SetStructureOverride(const std::vector<StructurePlacement>* list);
bool StructureOverrideActive();
const std::vector<StructurePlacement>* StructureOverrideSlot();   // null = off

/**
 * `--mapcheck <name>`'s answer, one line of JSON: {"ok", "map", "sites",
 * "warnings": [...], "error"} -- what LoadWorldMap said about the map (its
 * refusal in `error` when ok is false). The tuner's World map page shows it.
 */
std::string MapCheckJson(const std::string& name, bool ok, const WorldMapData& m,
                         const std::string& error);

/**
 * The whole `worldMap` buffer: PackBiomeTable's words plus the map header
 * fields, the dense height and biome planes, the site index and table and the
 * sculpt block. `map` may be unloaded (P0/P1 tools), in which case the plane
 * offsets stay 0, the samplers return biome 0 and the authored height is the
 * terrain's baseHeight.
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

/** The NAME of the region of CurrentWorldMap() holding world column (x, z)
 *  (WorldMapData::RegionAt; its id when it has no name), "" when none does. */
std::string RegionAt(int x, int z);

// The marker icons the map page draws (pixel art, assets/editor/mapvector.js
// ICONS): a marker naming another one loads with a warning.
inline constexpr const char* kMarkerIcons[] = {"hut",    "village", "castle", "tower", "cave",
                                               "ruin",   "shrine",  "camp",   "mine",  "port"};
// The most samples a road's reported profile carries (WorldMapData::Road::profS).
inline constexpr int kRoadProfileMax = 256;

/**
 * The CPU height mirror's BARE ground (landColumnBare's h, no site pad) at
 * (x, z) against CurrentWorldMap(). Defined in world.cpp beside the mirror;
 * declared here because its one caller is LoadWorldMap's site-pad bake
 * (StampSite::padY), which runs it with the map being loaded installed.
 */
int BareGroundHeight(int x, int z, uint32_t seed);

/**
 * The column as World::TerrainHeight composes it, with what its rivers and
 * lakes did (W1; world.cpp waterApply, the twin of worldgen.wgsl's): the
 * ground `h`, the surface water stands to over it (-1 = none above the sea
 * plane's own), the bed / shore material laid on it (0 = none) and whether a
 * river or lake shaped it. Against CurrentWorldMap().
 */
struct WaterColumn {
  int h = 0;
  int water = -1;
  uint32_t bed = 0;
  bool wet = false;
};
WaterColumn WaterColumnAt(int x, int z, uint32_t seed);

/**
 * The AUTHORED height at (x, z) against CurrentWorldMap(): the height plane's
 * Catmull-Rom (world.cpp mapHeightAt, the shader's twin), absolute voxels, no
 * noise, no sculpt; the edge fade outside the extent. `gx` / `gz` (optional)
 * get its gradient in the octave ladder's Q8 per voxel.
 */
int AuthoredHeightAt(int x, int z, int* gx = nullptr, int* gz = nullptr);

/**
 * THE COLUMN'S TREELINE (map v2 A1) against CurrentWorldMap(): the ground Y at
 * and above which the column wears the snow cap and grows no tree, and at and
 * above whose `- 1` the heat layer gives it the snowline climate. The map's
 * terrain.treeline, scaled above the sea by the biome's treelineScale (blended
 * over the four biome cells) plus the seeded +-treelineJitter. The CPU twin of
 * worldgen.wgsl treelineAt (the height mirror); defined in world.cpp.
 */
int TreelineAt(int x, int z, uint32_t seed);

/** The highest ground Y any column's treeline can reach on `map` (kHTreelineMax). */
int TreelineMaxOf(const WorldMapData& map);

/**
 * The keep-out every generated feature avoids (worldgen.wgsl siteKeepOut),
 * against CurrentWorldMap(): the pad box plus every listed site's own
 * footprint -- a stamp's pad, a lake's disc + band, a tree's trunk. Defined
 * in world.cpp beside the other CPU twins of the site readers.
 */
bool SiteKeepOut(int x, int z);

/**
 * WHICH MAP A RUN LOADS. `SANDVOX_MAP=<name>` in the environment wins (an
 * explicit request: "show me the harness ground in the game"), then the
 * process override (SetMapOverride; main.cpp sets it to the harness map for
 * --selftest / --verify / --suite / --vk-smoke / the fluid benches / --shot, so
 * no gate depends on the shipped map), then `tuned` -- world.mapLayer, the
 * game's map. Every loader (boot, F7 / ReloadEnvironment, vk_smoke, the save
 * fingerprint) asks here rather than reading world.mapLayer directly.
 */
std::string ActiveMapName(const std::string& tuned);
void SetMapOverride(const std::string& name);

// ---- THE SPLINE (docs/PLAN_worldbuilder.md 2.1 / 2.3; assets/editor/spline.js
// is the JS twin, tests/spline_vectors.json the shared table) -----------------
// CENTRIPETAL CATMULL-ROM (alpha 0.5, Barry-Goldman) through `pts`, every
// knot ON the curve. Open: phantom end points P[-1] = 2 P0 - P1 and
// P[n] = 2 P[n-1] - P[n-2]. Closed: the controls wrap. Each segment Pi ->
// Pi+1 is evaluated at nd = clamp(ceil(|Pi+1 - Pi| / 2), 8, 4096) uniform
// steps of its knot parameter into a dense polyline, whose arc length L is
// then resampled at m = max(1, ceil(L / step)) EQUAL arc steps: outputs
// Pi exactly, then the m - 1 interior points; the open curve's last point is
// P[n-1] exactly (a closed one does not repeat P0). Knot distances below
// 1e-4 count as 1e-4. `seg` / `f` say where each output lies (segment i,
// arc fraction k / m) so per-knot attributes interpolate linearly along it.
struct SplinePt {
  double x = 0, z = 0;
  int seg = 0;
  double f = 0;
};
std::vector<SplinePt> SplineTessellate(const std::vector<std::array<double, 2>>& pts, bool closed, double step);
/** The same resampling with straight segments (smooth: false). */
std::vector<SplinePt> PolylineResample(const std::vector<std::array<double, 2>>& pts, bool closed, double step);

}  // namespace worldmap
