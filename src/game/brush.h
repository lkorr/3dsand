#pragma once
#include <cstdint>

#include "sim/world.h"

// Turns the GPU pick result + UI state into MutationQueue ops.
class Brush {
 public:
  int radius = 4;          // voxels, clamped to [1,7] (mutate kernel box limit)
  uint32_t material = kMatSand;

  // Build a paint (mode 0) or erase op at the picked location. Returns false
  // if there is nothing sensible to target.
  bool BuildOp(const WorldSnapshot& snap, const Vec3& eye, const Vec3& fwd,
               bool erase, BrushOp& out) const;
};
