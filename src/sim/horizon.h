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
  kHTreeline = 9,    // i32: the map's terrain.treeline (the fallback; kHTlPlane is per sample)
  kHMaxY = 10,       // i32: the highest effective surface anywhere
  kHSamples = 11,    // word offset: nx * nz words, z-major (MAX | AVG << 16)
  kHBiome = 12,      // word offset: nx * nz biome ids, four per word, little-endian
  kHMatStone = 13,   // material ids the shading paints: bare rock on steep ground,
  kHMatSnow = 14,    //   snow over the treeline, the sea
  kHMatWater = 15,
  kHMip = 16,        // kMaxLevels words: word offset of level l's cells (MAX | AVG << 16)
  kHSkin = 32,       // kSkinSlots words: the skin material of biome id b
  kSkinSlots = 256,
  // word offset: nx * nz i16 per-sample TREELINES, two per word, little-endian
  // (worldmap::TreelineAt at the sample, map v2 A1): snow at and above it.
  // 0 = no plane (old header): the shader falls back to kHTreeline.
  kHTlPlane = kHSkin + kSkinSlots,
  kHeaderWords = kHTlPlane + 1,
  kMaxLevels = 16,
};

// ---- THE TERRAIN-SCALE LIGHT MAP (map v2 S1; horizon_light.wgsl) -----------
// A second, GPU-WRITTEN buffer beside the pyramid (Simulation::hzLightBuf_,
// shadow group 33 read-write, renderBGL 46 read-only), on the pyramid's own
// sample lattice. Per sample, two words:
//   w0  the SUN'S HORIZON in the key light's azimuth: the blocker that makes
//       the largest elevation angle seen from the sample's ground -- its top
//       Y (i16, voxels, absolute) | its distance (u16, units of kLightDistUnit
//       voxels) << 16. 0xFFFF distance = nothing blocks. A receiver at height
//       y is lit while tan(sun elevation) > (top - y) / distance, so the one
//       word serves a valley floor and the ridge crest above it.
//   w1  SKY VISIBILITY, f32 0..1: the cosine-weighted share of the upper
//       hemisphere the sample's ground sees past the terrain, from horizon
//       angles in kSkyDirs azimuths (mean of cos^2 of each horizon angle).
// Both march the pyramid from kLightMinDistSamples samples out: the near
// field's own shadow cache and openness grid own the first ~50 m.
// Header (kLightHeaderWords): words written by the KERNELS, never the CPU
// (a CPU write lands at the head of the NEXT command buffer):
enum : uint32_t {
  kLSkyValid = 0,    // 1 = w1 holds this map's sky visibility
  kLSunValid = 1,    // 1 = w0 holds a sun horizon
  kLSunDirX = 2,     // f32: the key light azimuth (unit xz) w0 was built for
  kLSunDirZ = 3,
  kLightHeaderWords = 8,
  kLightDistUnit = 8,        // voxels per w0 distance unit (65534 x 8 = 52 km)
  kLightMinDistSamples = 2,  // the first sample out the march looks at
  kSkyDirs = 8,
};
// Words the light buffer needs for a packed pyramid `words` (header + 2 per
// sample; the header alone when there is no pyramid).
uint64_t LightWords(const std::vector<uint32_t>& words);

// 2^8 = 256 voxels = 25.6 m: the biome plane's own cell, and the level-8
// cascade cell, so the horizon starts at the resolution the cascade ends at.
inline constexpr int kSampleLog2 = 8;

/**
 * The packed buffer for `map`. `worldMapWords` is PackWorldMap's output for the
 * same map (the biome records' skin words are read from it, so the horizon
 * paints exactly the materials worldgen lays). `seed` is the world seed the
 * per-sample treeline's jitter is read at (worldmap::TreelineAt, which reads
 * the INSTALLED map: `map` must be CurrentWorldMap()). A map with no height
 * plane packs a header with kHValid = 0.
 */
void Build(const worldmap::WorldMapData& map, const std::vector<uint32_t>& worldMapWords,
           uint32_t seed, std::vector<uint32_t>& out);

}  // namespace horizon
