#pragma once
#include <cstdint>
#include <deque>

#include "math3d.h"
#include "sim/world.h"

// Far-field cascade manager (render-only LOD — DESIGN.md §9,
// docs/PLAN_far_field_cascades.md). Keeps kFarLevels nested toroidal 256^3
// volumes centered on the player: recenters each level toward the player at
// most one level-chunk per axis per tick (2-chunk hysteresis, mirroring
// Stream), enqueues incoming planes for GPU fill, and drains the queue at
// kFarListCap level-chunks per tick through worldgen.wgsl `far`.
//
// Derived data only: no readbacks, no persistence, no sim interaction, not
// hashed. Fill entries name SLOTS; the kernel maps slot -> world level-chunk
// under the origins uploaded the same tick, so a backlogged entry always
// fills whatever is currently resident in that slot — never stale space.
class FarField {
 public:
  void Init(World* world) { world_ = world; }

  // Recenter toward the player's FINE-chunk coord. Call between ticks (after
  // Stream::Update), before PrepareTick. A level whose window has gone
  // entirely stale (teleport, load) resets and refills wholesale.
  void Update(IVec3 playerChunk);

  // Pop up to kFarListCap queued fills, upload farList (+ farUBO when origins
  // changed), and return the dispatch count for this tick's
  // TickParams.farCount / EncodeFarFill.
  uint32_t PrepareTick(const wgpu::Queue& queue);

  // Re-derive every origin around the player and refill all levels, coarsest
  // first so a horizon exists immediately (startup, load, regen).
  void FullRefill(IVec3 playerChunk);

  size_t PendingFills() const { return queue_.size(); }

 private:
  void ResetLevel(uint32_t k, IVec3 desired);        // origin jump + full refill
  void EnqueuePlane(uint32_t k, int axis, int wcoord);  // one incoming plane

  World* world_ = nullptr;
  IVec3 origins_[kFarLevels] = {};  // per level, level-chunk units
  std::deque<uint32_t> queue_;      // packed (level-1) << 12 | slot
  bool uboDirty_ = true;
};
