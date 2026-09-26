#pragma once
#include <cstdint>

#include "sim/world.h"

// Turns the GPU pick result + UI state into MutationQueue ops.
class Brush {
 public:
  int radius = 4;          // voxels, clamped to [1,7] (mutate kernel box limit)
  uint32_t material = kMatSand;
  // Grain size for a POWDER brush, in eighths of a cell (1..8; 8 = whole
  // cells, the old brush). Carried in BrushOp.pad1 bits 4..7 -- see
  // kBrushGrainShift -- and ignored for anything that is not a powder.
  uint32_t grainEighths = 8;

  // Build a paint (mode 0) or erase op at the picked location. Returns false
  // if there is nothing sensible to target.
  bool BuildOp(const WorldSnapshot& snap, const Vec3& eye, const Vec3& fwd,
               bool erase, BrushOp& out) const;
};
