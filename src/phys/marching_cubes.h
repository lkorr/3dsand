#pragma once
#include <cstdint>
#include <vector>

#include "math3d.h"

// Localized marching cubes over binary voxel occupancy (DESIGN.md §7): turns
// one 16^3 chunk of terrain into a collision triangle mesh with sloped
// normals, so debris rolls and settles naturally instead of snagging on cube
// faces. Tables from Paul Bourke's "Polygonising a Scalar Field".
//
// Cells are owned by the chunk containing their min corner, so adjacent chunks
// tile without cracks or duplicate faces. Output vertices are in world VOXEL
// units (caller converts to meters for the physics).
//
// Occupancy arrives pre-sampled as an 18^3 bitmask covering the chunk's cells
// plus the +1 border they read (index via McOccIndex). Sampling once up front
// instead of per-corner turns 32768 predicate calls — each formerly an
// indirect std::function hop into a chunk-cache hash lookup — into 5832 direct
// ones, and lets the inner loop read bits out of L1.
constexpr int kMcOccDim = 18;  // 16 cells + 1 border on each side
constexpr int kMcOccWords = (kMcOccDim * kMcOccDim * kMcOccDim + 31) / 32;

// (x,y,z) are chunk-local coords offset by +1 (so -1 maps to 0).
inline int McOccIndex(int x, int y, int z) {
  return (z * kMcOccDim + y) * kMcOccDim + x;
}
inline void McOccSet(uint32_t* occ, int x, int y, int z) {
  int i = McOccIndex(x, y, z);
  occ[i >> 5] |= 1u << (i & 31);
}
inline void McOccClear(uint32_t* occ, int x, int y, int z) {
  int i = McOccIndex(x, y, z);
  occ[i >> 5] &= ~(1u << (i & 31));
}
inline bool McOccGet(const uint32_t* occ, int x, int y, int z) {
  int i = McOccIndex(x, y, z);
  return (occ[i >> 5] >> (i & 31)) & 1u;
}

// Vertices are welded: marching-cubes vertices sit at edge midpoints, i.e. on
// a half-integer lattice, so identical positions dedupe exactly by integer key
// with no epsilon compare. Roughly 4x fewer vertices reach Jolt's MeshShape
// build, which is the dominant cost of a terrain patch rebuild.
//
// MASS-WEIGHTED SURFACES (docs/PLAN_powder_mass.md P4). `dens`, when given, is
// one byte per occ sample: how many EIGHTHS of the cell are matter (8 = a
// solid or a full powder, 0 = open, 1..7 = a partial powder cell). An edge
// vertex then sits where the density crosses one half, interpolated between
// its two samples, instead of at the midpoint -- so a 3/8 dusting lies lower
// than a full cell and a 7/8 one higher, and a slope of partial cells is a
// slope rather than a staircase. The weld key is still the EDGE (its doubled
// midpoint), and both cubes that share an edge compute the same position
// from the same two samples, so welding stays exact. `occ` must agree:
// a sample is occupied iff dens >= 4. Null `dens` is the binary surface.
void PolygonizeChunk(IVec3 chunkOriginVoxel, const uint32_t* occ,
                     std::vector<float>& outVertsXYZ,
                     std::vector<uint32_t>& outIndices,
                     const uint8_t* dens = nullptr);
constexpr int kMcOccCells = kMcOccDim * kMcOccDim * kMcOccDim;
