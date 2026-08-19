#pragma once
#include <functional>
#include <vector>

#include "math3d.h"

// Localized marching cubes over binary voxel occupancy (DESIGN.md §7): turns
// one 16^3 chunk of terrain into a collision triangle mesh with sloped
// normals, so debris rolls and settles naturally instead of snagging on cube
// faces. Tables from Paul Bourke's "Polygonising a Scalar Field".
//
// `solid(x,y,z)` samples world-voxel coordinates (the chunk's +1 border reads
// neighbor chunks). Cells are owned by the chunk containing their min corner,
// so adjacent chunks tile without cracks or duplicate faces. Output vertices
// are in world VOXEL units (caller converts to meters for the physics).
void PolygonizeChunk(IVec3 chunkOriginVoxel,
                     const std::function<bool(int, int, int)>& solid,
                     std::vector<float>& outVertsXYZ,
                     std::vector<uint32_t>& outIndices);
