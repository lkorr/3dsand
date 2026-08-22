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
  uint32_t PrepareTick(const rhi::Queue& queue);

  // Re-derive every origin around the player and refill all levels, coarsest
  // first so a horizon exists immediately (startup, load, regen).
  void FullRefill(IVec3 playerChunk);

  size_t PendingFills() const { return queue_.size(); }

  // Radius (meters from the player) out to which cascade data is known to be
  // FILLED — the adaptive-fog input (plan phase 3B).
  //
  // Buffers are zero-initialized and a queued fill has not run yet, so a level
  // with outstanding entries may contain air where terrain belongs; rays march
  // straight through it and hit sky. Rather than let that read as holes in the
  // horizon, the renderer fogs everything past the last KNOWN-GOOD band out.
  //
  // The bound is the INNERMOST incomplete level: cascade boxes are nested, so
  // if level k has pending work, level k's half-extent and everything past it
  // is suspect, but levels 1..k-1 are complete and cover out to level k-1's
  // half-extent. (Levels coarser than k may also be complete — FullRefill
  // fills coarsest-first exactly so the horizon exists early — but their data
  // is only reachable through the gap at level k, so it cannot be trusted to
  // be visible.) With no pending work anywhere this is the full outermost
  // half-extent, which is what kFarFogDensity was pinned to.
  float SafeRadiusMeters() const;

 private:
  void ResetLevel(uint32_t k, IVec3 desired);        // origin jump + full refill
  void EnqueuePlane(uint32_t k, int axis, int wcoord);  // one incoming plane
  void Enqueue(uint32_t k, uint32_t slot);           // queue + per-level counter

  World* world_ = nullptr;
  IVec3 origins_[kFarLevels] = {};  // per level, level-chunk units
  std::deque<uint32_t> queue_;      // packed (level-1) << 12 | slot
  // Outstanding entries per level, mirroring queue_ (the queue is FIFO across
  // levels, so scanning it per frame would be O(24576); these counters make
  // SafeRadiusMeters O(kFarLevels)). Incremented on every enqueue, decremented
  // as PrepareTick pops.
  uint32_t pending_[kFarLevels] = {};
  bool uboDirty_ = true;
};
