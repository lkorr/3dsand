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
  kHTerrainBaseHeight = 32,      // the world datum: mean ground height
  kHTerrainLandformRange = 33,   // what a painted landform 0..255 spans, voxels (was contAmplitude)
  kHTerrainRangeAmplitude = 34,  // the seeded octave ladder under the plane
  kHTerrainRangeLog2 = 35,
  kHTerrainHillAmplitude = 36,
  kHTerrainHillLog2 = 37,
  kHTerrainDetailAmplitude = 38,
  kHTerrainDetailLog2 = 39,
  kHTerrainGrainAmplitude = 40,
  kHTerrainGrainLog2 = 41,
  kHTerrainFbmAtten = 42,        // Q8 derivative attenuation, 0..256
  kHTerrainHomeY = 43,           // the calm home area (was spawnPlainY/R/Fade)
  kHTerrainHomeR = 44,
  kHTerrainHomeFade = 45,
  kHTerrainSedCeil = 46,         // the sediment wedge (was sed*)
  kHTerrainSedFraction = 47,
  kHTerrainSedStrip = 48,
  kHTerrainSedSlope = 49,
  kHTerrainSedMax = 50,
  kHTerrainSedTopsoil = 51,
  kHTerrainTreeline = 52,        // snow and no trees at or above this ground Y
  kHTerrainRefVpm = 53,          // the voxels-per-metre the map's terrain was authored at
  kHeaderWords = 64,    // padded, like treeatlas::kFileHeaderWords
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
  kB_SedMax = 11,         // reserved for P4 (0 = use the global knob)
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
  // biomeReliefAt, BILINEARLY over the four map cells around the column (the
  // landform plane's own lattice), so two biomes' curves crossfade over a
  // whole 102 m cell and never meet on a cliff. Packed by PackBiomeTerrain,
  // the one conversion both the packer and the CPU twin use.
  kB_CurveKnot0 = 22,         // ..30: knots 0..8
  kB_HillMul = 31,
  kB_DetailMul = 32,
  kB_GrainMul = 33,
  // 34..47 reserved
  kCoverRowWords = 12,
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
  kR_PatchThreshold = 5,  // 0..255 gate on the biome patch field at the centre
  // 6..7 reserved
};
// kB_Flags bits. These replace the `biome == B_DESERT` / `== B_PINE` tests
// that used to gate whole blocks of genCellIn on a hard-coded id.
inline constexpr uint32_t kBF_GroundFlora = 1u << 0;  // the canopy-inverted undergrowth + flower layer
inline constexpr uint32_t kBF_Cacti = 1u << 1;        // the cactus proc shape
inline constexpr uint32_t kBF_SandCap = 1u << 2;      // loose sand cap under the skin (the old desert rule)
inline constexpr uint32_t kBF_CanopyRows = 1u << 3;   // some cover row bounds the canopy cover (P-G): scan the trees once per column

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
  kW_MaxSlope = 37,            // placement.maxSlope, Q8 (the biome row's own gate is what rolls)
  kW_MinY = 38,                // placement.minY (i32; informational)
  kW_MaxY = 39,                // placement.maxY (i32; informational)
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
// kSiteWater (P-F): an AUTHORED LAKE, Tier A -- `{kind: "water", preset,
// at: [x, z], radius?}` in map.json. The same record: kS_X/Z is the disc
// centre, kS_Radius the disc radius (the preset's, or the site's override),
// kS_PadMargin the preset's shore/berm band (what the site index plane
// reaches), kS_Preset the water preset. The height mirror's waterSiteNear
// finds it through the same per-cell index plane the stamps use, and from
// there it is a Pond like any rolled one -- same bowl, same berm, same shore,
// same flora -- with its centre from the map instead of the seed. It is not
// padded (sitePadAt skips the kind) and its keep-out is the DISC plus the
// band, not its cells (siteKeepOut tests the kind), so a lake does not bald
// four whole cells of forest around it.
enum : uint32_t {
  kSiteRecWords = 16,
  kS_Kind = 0,          // kSiteStamp | kSiteWater
  kS_X = 1,             // world voxel centre (i32 in u32)
  kS_Z = 2,
  kS_Radius = 3,        // Chebyshev footprint radius in voxels: keep-out + pad
                        // (a water site: the disc radius)
  kS_PadMargin = 4,     // columns over which the pad ramps back to terrain
                        // (a water site: its preset's shore/berm band)
  kS_Rot = 5,           // 0..3, informational (baked into the stamp block)
  kS_Salt = 6,
  kS_StampOff = 7,      // word offset of the stamp block, 0 = none
  kS_Preset = 8,        // water site: 1 + index into the water preset table
  // 9..15 reserved
  kStampHdrWords = 4,
  kStamp_NX = 0, kStamp_NY = 1, kStamp_NZ = 2, kStamp_Columns = 3,
  // columns: nx*nz pairs of (runOff, runCount), absolute word offsets; runs:
  // mat (12 bits) | y0 << 16 | len << 27, y0 < 2048, len <= 31, y0 ascending
  kSitePad = 0,
  kSiteStamp = 1,
  kSiteWater = 2,
};

}  // namespace worldmap

namespace biomes { struct BiomeSet; struct BiomeDef; struct WaterPresetDef; }

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
  int landformRangeVox = 1024;    // a painted 0..255 spans this many voxels, centred on 128
  int rangeAmplitude = 256, rangeLog2 = 9;
  int hillAmplitude = 64, hillLog2 = 7;
  int detailAmplitude = 16, detailLog2 = 5;
  int grainAmplitude = 4, grainLog2 = 3;
  int fbmAtten = 256;
  int homeY = 200, homeR = 320, homeFade = 2048;
  int sedCeil = 264, sedFraction = 64, sedStrip = 6, sedSlope = 96, sedMax = 32, sedTopsoil = 4;
  int treeline = 228;
};
inline constexpr uint32_t kTerrainWords = kHTerrainRefVpm + 1 - kHTerrainBaseHeight;
/** The terrain as header words kHTerrainBaseHeight..kHTerrainRefVpm. */
void TerrainWords(const TerrainParams& t, uint32_t out[kTerrainWords]);
/** The world's treeline, home height and reference scale, for the CPU code
 *  that used to read worldgen.treeline / spawnPlainY / refVoxelsPerMetre.
 *  The loaded map's, or the defaults above when no map is loaded. */
const TerrainParams& CurrentTerrain();

// ---- a biome's terrain record (P-G): the kB_CurveKnot0..kB_GrainMul words ----
inline constexpr uint32_t kBiomeTerrainWords = kB_GrainMul + 1 - kB_CurveKnot0;
struct BiomeTerrainPacked { uint32_t w[kBiomeTerrainWords] = {}; };

// ---- an authored LANDFORM site (P-G, Tier A): a declared mountain ----------
// `{kind: "landform", shape: peak | ridge | basin | plateau, at: [x, z],
// radius, heightVox, rotation?}` in map.json sites[]. Overlaid onto the
// packed landform PLANE at load (OverlayLandformSites), so the shader reads
// the plane exactly as before and nothing new enters the height mirror. Not
// in the site table: a landform keeps nothing out, it only shapes the ground.
struct LandformSite {
  std::string id, shape;
  int x = 0, z = 0;               // world voxels
  int radius = 1024;              // voxels: a peak's footprint, a ridge's half-length
  int heightVox = 0;              // signed: how far the plane is lifted (or, for a basin, sunk)
  int rotation = 0;               // degrees, ridges only
};

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
  int maxSlope = 1024, minY = -1, maxY = -1;
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
/** Overlay `sites` onto a landform plane of `width` x `height` cells whose
 *  cell (cx, cz) is centred on world column ((cx - ox) << log2) + half.
 *  Cone / elliptical ridge / plateau / inverted cone in landform units,
 *  heightVox * 256 / landformRangeVox per voxel; clamped 0..255. */
void OverlayLandformSites(const std::vector<LandformSite>& sites, int landformRangeVox,
                          int cellLog2, int width, int height, int originCellX, int originCellZ,
                          std::vector<uint8_t>& landform);

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
  // Even an UNLOADED map carries the default terrain words: the CPU twin reads
  // them through CurrentWorldMap() before any map is loaded (tools, the
  // heightmap route), and a zero homeFade there is a division by zero.
  WorldMapData() { TerrainWords(terrain, terrainWords); }
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
    int kind = kSiteStamp;                // kSiteStamp | kSiteWater (P-F)
    int x = 0, z = 0, radius = 0, padMargin = 8, rot = 0;
    uint32_t salt = 0;
    int preset = 0;                       // water site: 1 + preset index
    int nx = 0, ny = 0, nz = 0;
    std::vector<uint32_t> words;          // the packed stamp block, offsets RELATIVE to its start
  };
  std::vector<StampSite> sites;           // stamps AND water sites, in site-id order
  // The water table the height mirror's CPU twin reads (worldgen.wgsl reads
  // the kW_* / kR_* words; these are the same integers): one WaterGeom per
  // preset in loader order, each biome's packed rows by biome id, the lattice
  // and the scan band. Filled by LoadWorldMap from the set it resolved the
  // palette against, so the two sides cannot see different presets.
  std::vector<WaterGeom> water;
  std::vector<std::vector<WaterRowPacked>> biomeWater;
  int pondTile = 0, pondBand = 0;
  // P-G: the map's terrain (map.json `terrain`), rescaled to the live voxel
  // size, and the same as header words; each biome's terrain record by id;
  // the authored landform sites (already overlaid onto `landform`).
  TerrainParams terrain;
  uint32_t terrainWords[kTerrainWords] = {};
  std::vector<BiomeTerrainPacked> biomeTerrain;
  std::vector<LandformSite> landformSites;
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
