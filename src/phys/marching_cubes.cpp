#include "phys/marching_cubes.h"

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

void PolygonizeChunk(IVec3 o, const std::function<bool(int, int, int)>& solid,
                     std::vector<float>& outVerts, std::vector<uint32_t>& outIndices) {
  const int n = (int)kChunk;
  for (int z = 0; z < n; z++) {
    for (int y = 0; y < n; y++) {
      for (int x = 0; x < n; x++) {
        int cx = o.x + x, cy = o.y + y, cz = o.z + z;
        int cubeIndex = 0;
        for (int c = 0; c < 8; c++) {
          if (solid(cx + kCorner[c][0], cy + kCorner[c][1], cz + kCorner[c][2]))
            cubeIndex |= 1 << c;
        }
        int edges = mc::kEdgeTable[cubeIndex];
        if (edges == 0) continue;

        // vertex on each crossed edge, at the midpoint (binary occupancy)
        float vx[12], vy[12], vz[12];
        for (int e = 0; e < 12; e++) {
          if (!(edges & (1 << e))) continue;
          const int* a = kCorner[kEdge[e][0]];
          const int* b = kCorner[kEdge[e][1]];
          vx[e] = (float)cx + 0.5f * (float)(a[0] + b[0]);
          vy[e] = (float)cy + 0.5f * (float)(a[1] + b[1]);
          vz[e] = (float)cz + 0.5f * (float)(a[2] + b[2]);
        }

        const int* tri = mc::kTriTable[cubeIndex];
        for (int t = 0; tri[t] != -1; t += 3) {
          uint32_t base = (uint32_t)(outVerts.size() / 3);
          for (int k = 0; k < 3; k++) {
            int e = tri[t + k];
            outVerts.push_back(vx[e]);
            outVerts.push_back(vy[e]);
            outVerts.push_back(vz[e]);
          }
          outIndices.push_back(base);
          outIndices.push_back(base + 1);
          outIndices.push_back(base + 2);
        }
      }
    }
  }
}
