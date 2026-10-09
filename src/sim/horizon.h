// horizon.h (src/sim, render-only) -- THE FAR HORIZON: the authored height
// plane as a 2D heightfield pyramid, raymarched for every ray that leaves the
// far-field cascades (docs/PLAN_map_v2.md H1; DESIGN.md §9 "The far horizon").
//
// WHY. The cascades end at the outermost level's box, 6.55 km from the eye
// (world.h kFarHalfExtentMeters(kFarLevels)). A 1,400 m massif 10 km away was
// sky. The map already holds every post of its height plane on the CPU
// (worldmap::WorldMapData), so the horizon is that plane, downsampled once at
// load and drawn by a second, much cheaper march: a 2D max-mip traversal over
// the map extent plus its edge fade, and the flat sea / default floor beyond.
//
// WHAT IS STORED, per SAMPLE on a 2^kSampleLog2-voxel lattice (25.6 m), which
// covers the extent widened by edgeFadeVox (rounded up to a sample):
//   lo i16  MAX: the highest EFFECTIVE surface (max(ground, sea)) over the
//           sample's footprint posts -- the silhouette, so a knife ridge or a
//           summit between two samples is not shaved;
//   hi i16  AVG: the mean GROUND over the same posts -- the shading and the
//           water test (a sample whose mean ground is under the sea is sea).
// and one biome id per sample (the biome plane's cell under it, the default
// outside). The surface the march intersects is the bilinear blend of the MAX
// samples, eased to AVG over the first stretch past the cascade edge (so the
// horizon meets the cascade's centre-sampled cells without a lip). The cell
// MAX PYRAMID bounds it: level 0 cell (i, j) = max of its four corner
// samples' MAX (an exact bound of the bilinear), level l = max of 2 x 2 level
// l-1 cells. Each pyramid word also carries the cell's AVG ground (mean of the
// four children) in its high half, which is what a distant pixel's normal is
// taken from (a 25 m sample is sub-pixel at 30 km).
//
// Everything here is DERIVED, RENDER-ONLY data: built from the map at load and
// at every environment reload (Simulation::Init / UploadEnvironment), never
// hashed, never saved, read by no sim kernel. The world hash cannot see it.
//
// GPU: one read-only storage buffer at renderBGL 45 (Simulation::horizonBuf_).
// raymarch.wgsl's HZ_* consts mirror the header below; only raymarch reads it,
// so the two are kept together by hand (boltfx.h's precedent).
#pragma once

#include <cstdint>
#include <vector>

namespace worldmap { struct WorldMapData; }

namespace horizon {

// Header word offsets. Mirrored by raymarch.wgsl HZ_H_*.
enum : uint32_t {
  kHValid = 0,       // 1 = a pyramid is packed; 0 = none (every ray misses)
  kHX0 = 1,          // i32: world voxel x of sample 0
  kHZ0 = 2,          // i32: world voxel z of sample 0
  kHNX = 3,          // samples along x
  kHNZ = 4,          // samples along z
  kHLog2 = 5,        // sample spacing, log2 voxels (kSampleLog2)
  kHLevels = 6,      // levels of the cell pyramid (level 0 = cells between samples)
  kHSeaY = 7,        // i32: the sea plane, world voxel y
  kHOuterY = 8,      // i32: the surface past the grid: max(height default, sea)
  kHTreeline = 9,    // i32: snow at and above this ground y (map terrain.treeline)
  kHMaxY = 10,       // i32: the highest effective surface anywhere
  kHSamples = 11,    // word offset: nx * nz words, z-major (MAX | AVG << 16)
  kHBiome = 12,      // word offset: nx * nz biome ids, four per word, little-endian
  kHMatStone = 13,   // material ids the shading paints: bare rock on steep ground,
  kHMatSnow = 14,    //   snow over the treeline, the sea
  kHMatWater = 15,
  kHMip = 16,        // kMaxLevels words: word offset of level l's cells (MAX | AVG << 16)
  kHSkin = 32,       // kSkinSlots words: the skin material of biome id b
  kSkinSlots = 256,
  kHeaderWords = kHSkin + kSkinSlots,
  kMaxLevels = 16,
};

// 2^8 = 256 voxels = 25.6 m: the biome plane's own cell, and the level-8
// cascade cell, so the horizon starts at the resolution the cascade ends at.
inline constexpr int kSampleLog2 = 8;

/**
 * The packed buffer for `map`. `worldMapWords` is PackWorldMap's output for the
 * same map (the biome records' skin words are read from it, so the horizon
 * paints exactly the materials worldgen lays). A map with no height plane
 * packs a header with kHValid = 0. Pure.
 */
void Build(const worldmap::WorldMapData& map, const std::vector<uint32_t>& worldMapWords,
           std::vector<uint32_t>& out);

}  // namespace horizon
