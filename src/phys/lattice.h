#pragma once
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "phys/physics.h"
#include "sim/voxload.h"

// The bridge between a body's two voxel lattices (DESIGN.md §9).
//
// A micro body may carry a SKIN finer than its COLLIDER: the skin is int16
// PrefabVoxel at `skinScale` units per world voxel, the collider is int8
// DebrisVoxel at `physScale`. They are decoupled because their costs are
// unrelated — the brick march is a fragment shader over one OBB, so skin cost
// tracks screen area, while the collider is Jolt boxes and tracks voxel count.
//
// This header holds the ONE function that relates them, so that the relation
// can be tested without a GPU (tests/lattice_test.cpp). Data flows skin ->
// collider and never back: the skin is render state, and a collider that read
// from it in the other direction would pull render state into physics.

// Majority-fill a fine skin lattice down to a coarse collider lattice, `ratio`
// skin voxels per collider voxel along each axis. A block survives when at
// least half of it is solid and takes its plurality material — the same rule
// DownsampleMicro applies for settle-back, so a carved shape reads the same way
// to physics as it does to the eye.
//
// Coordinates are assumed non-negative (bodies are rebased to their min
// corner). A block index past 127 cannot be represented in a DebrisVoxel and is
// DROPPED with `*overflow` set, rather than wrapping the way an (int8_t) cast
// would — that silent wrap is exactly the failure this bound has to prevent,
// since it would teleport part of a limb to the far side of the body.
inline std::vector<DebrisVoxel> DownsampleSkin(
    const std::vector<PrefabVoxel>& src, uint32_t ratio, bool* overflow) {
  const int s = (int)std::max(1u, ratio);
  const uint32_t full = (uint32_t)(s * s * s);
  constexpr int kMaxDistinct = 8;
  struct Blk {
    uint32_t count = 0;
    int n = 0;
    uint16_t mat[kMaxDistinct]{};
    uint32_t hits[kMaxDistinct]{};
  };
  std::unordered_map<uint64_t, Blk> blocks;
  bool over = false;
  for (const PrefabVoxel& v : src) {
    const int bx = (int)v.x / s, by = (int)v.y / s, bz = (int)v.z / s;
    if (bx < 0 || by < 0 || bz < 0 || bx > 127 || by > 127 || bz > 127) {
      over = true;
      continue;
    }
    // 21 bits per axis. The index is bounded at 127 above, but keeping the
    // fields wide means a future bound change cannot alias two blocks onto one
    // key the way a packed-byte key would.
    uint64_t key = ((uint64_t)bx << 42) | ((uint64_t)by << 21) | (uint64_t)bz;
    Blk& blk = blocks[key];
    blk.count++;
    int k = 0;
    for (; k < blk.n; k++)
      if (blk.mat[k] == v.material) break;
    if (k == blk.n && blk.n < kMaxDistinct) blk.mat[blk.n++] = v.material;
    if (k < kMaxDistinct) blk.hits[k]++;
  }
  if (overflow) *overflow = over;

  std::vector<DebrisVoxel> out;
  out.reserve(blocks.size());
  for (const auto& [key, blk] : blocks) {
    if (blk.count * 2 < full) continue;  // majority-fill: mostly air -> air
    int best = 0;
    for (int k = 1; k < blk.n; k++)
      if (blk.hits[k] > blk.hits[best]) best = k;
    out.push_back({(int8_t)((key >> 42) & 0x1FF), (int8_t)((key >> 21) & 0x1FF),
                   (int8_t)(key & 0x1FF), 0, blk.mat[best]});
  }
  return out;
}
