#include "phys/marching_cubes.h"

#include <unordered_map>

#include "phys/mc_tables.h"
#include "sim/world.h"  // kChunk

namespace {

// Bourke's corner numbering for one cell at (x,y,z):
//   0:(0,0,0) 1:(1,0,0) 2:(1,0,1) 3:(0,0,1)   bottom (y)
//   4:(0,1,0) 5:(1,1,0) 6:(1,1,1) 7:(0,1,1)   top
// (his j axis mapped to z, k to y — any consistent mapping works with the
// tables as long as edges connect the right corner pairs)
constexpr int kCorner[8][3] = {
    {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1},
    {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1},
};
// edge -> the two corners it connects
constexpr int kEdge[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0},
    {4, 5}, {5, 6}, {6, 7}, {7, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7},
};

}  // namespace

void PolygonizeChunk(IVec3 o, const uint32_t* occ, std::vector<float>& outVerts,
                     std::vector<uint32_t>& outIndices) {
  const int n = (int)kChunk;

  // Weld key: edge midpoints land on halves, so 2*coord is an exact integer.
  // Local doubled coords span 0..2n, which fits 6 bits per axis.
  std::unordered_map<uint32_t, uint32_t> weld;
  weld.reserve(1024);
  auto emit = [&](int e, int x, int y, int z) -> uint32_t {
    const int* a = kCorner[kEdge[e][0]];
    const int* b = kCorner[kEdge[e][1]];
    int dx = 2 * x + a[0] + b[0];
    int dy = 2 * y + a[1] + b[1];
    int dz = 2 * z + a[2] + b[2];
    uint32_t key = (uint32_t)dx | ((uint32_t)dy << 8) | ((uint32_t)dz << 16);
    auto it = weld.find(key);
    if (it != weld.end()) return it->second;
    uint32_t idx = (uint32_t)(outVerts.size() / 3);
    outVerts.push_back((float)o.x + 0.5f * (float)dx);
    outVerts.push_back((float)o.y + 0.5f * (float)dy);
    outVerts.push_back((float)o.z + 0.5f * (float)dz);
    weld.emplace(key, idx);
    return idx;
  };

  for (int z = 0; z < n; z++) {
    for (int y = 0; y < n; y++) {
      for (int x = 0; x < n; x++) {
        // occ is stored with a +1 border offset
        int cubeIndex = 0;
        for (int c = 0; c < 8; c++) {
          if (McOccGet(occ, x + kCorner[c][0] + 1, y + kCorner[c][1] + 1,
                       z + kCorner[c][2] + 1))
            cubeIndex |= 1 << c;
        }
        int edges = mc::kEdgeTable[cubeIndex];
        if (edges == 0) continue;

        uint32_t vi[12];
        for (int e = 0; e < 12; e++)
          if (edges & (1 << e)) vi[e] = emit(e, x, y, z);

        const int* tri = mc::kTriTable[cubeIndex];
        for (int t = 0; tri[t] != -1; t += 3) {
          outIndices.push_back(vi[tri[t]]);
          outIndices.push_back(vi[tri[t + 1]]);
          outIndices.push_back(vi[tri[t + 2]]);
        }
      }
    }
  }
}
