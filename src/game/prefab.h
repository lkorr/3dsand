#pragma once
#include <cstdint>
#include <deque>
#include <vector>

#include "sim/materials.h"
#include "sim/voxload.h"
#include "sim/world.h"

// Stamps prefab models into the grid as exact-cell MutationQueue ops
// (PLAN_voxel_art_and_mobs.md §A2). Follows the DebrisSystem::PreTick shape:
// append bounded ops each tick, defer the rest. Pending voxels are kept in
// WORLD coords and converted to slot indices only at drain time — the
// residency window can shift mid-placement.
class PrefabPlacer {
 public:
  // Queue `pf` for stamping with its min corner at `at`, rotated rotY*90°
  // about +Y. overwrite=false fills air only (kCellOpIfAir). Returns the
  // world-space box that will be written (for MarkModifiedBox).
  void Place(const Prefab& pf, IVec3 at, int rotY, bool overwrite,
             const std::vector<MaterialDef>& mats, IVec3& boxLo, IVec3& boxHi);

  // Drain pending voxels into this tick's cellOps. Ops already in `cellOps`
  // (debris island removal) win same-cell conflicts: duplicate cellIdx in one
  // dispatch would race on GPU write order, so colliding voxels retry next
  // tick. Voxels whose chunk streamed out are dropped.
  void PreTick(const World& world, std::vector<CellOp>& cellOps);

  size_t PendingCount() const { return pending_.size(); }

  // Rotated footprint of a prefab (for anchoring the stamp under the cursor).
  static IVec3 RotatedSize(const Prefab& pf, int rotY);

 private:
  struct PendingCell {
    IVec3 cell;     // world coords
    uint32_t word;  // final voxel word (may carry kCellOpIfAir)
  };
  std::deque<PendingCell> pending_;

  // placer share of the 65536 cellOp budget: keeps tick cost smooth and
  // leaves headroom for island events landing the same tick
  static constexpr uint32_t kPlacePerTick = 16384;
};
