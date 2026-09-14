// The tree-fell gate's fixture tree, exported so the LIVE harness can plant
// the same one. `--fell-tree` (main.cpp) plants it ahead of the player under
// `--frames`, cuts it, and lets the windowed frame loop record what the gate
// cannot: the instance rebuild, the drawBodies GPU span, and the whole-frame
// time while a 28k-voxel body falls. One fixture in two harnesses is the point
// — a number from the gate and a number from the game are then about the same
// tree.
#pragma once
#include <cstdint>
#include <vector>

#include "sim/world.h"

namespace selftest {

struct TreeFixture {
  IVec3 base{};            // trunk foot, world cells (base.y is the ground cell)
  int height = 0;          // trunk top, cells above base.y
  int crownR = 0;          // crown radius, cells
  IVec3 lo{}, hi{};        // the whole tree's bounding box
  uint32_t woodCells = 0;  // what was actually written
  uint32_t leafCells = 0;
};

// Author one tree as exact-cell ops (trunk, four limbs, dithered crown),
// pruned to the one 6-connected component that reaches `base`. Appends to
// `ops`, capped at kMaxCellOpsPerTick. Defined in selftest_floaters.cpp.
TreeFixture BuildTree(const World& world, IVec3 base, uint32_t wood,
                      uint32_t leaves, std::vector<CellOp>& ops);

}  // namespace selftest
