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
  kHeaderWords = 32,    // padded, like treeatlas::kFileHeaderWords
};

// The magic in both the file and the buffer header: 'SVMP', little-endian.
inline constexpr uint32_t kMagic = 0x504D5653u;
inline constexpr uint32_t kVersion = 1u;

}  // namespace worldmap
