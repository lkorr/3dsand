#pragma once
#include <cstdint>
#include <deque>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

// Far-field cascade manager (render-only LOD — DESIGN.md §9,
// docs/PLAN_far_field_cascades.md). Keeps kFarLevels nested toroidal 256^3
// volumes centered on the player: recenters each level toward the player at
// most one level-chunk per axis per tick (2-chunk hysteresis, mirroring
// Stream), enqueues incoming planes for GPU fill, and drains the queue at
// kFarListCap level-chunks per tick through worldgen.wgsl `far`.
//
// Derived data only: no readbacks, no sim interaction, not hashed. Fill
// entries name SLOTS; the kernel maps slot -> world level-chunk under the
// origins uploaded the same tick, so a backlogged entry always fills whatever
// is currently resident in that slot — never stale space.
//
// EDIT PERSISTENCE (src/sim/faredits.h). The cascades are still DERIVED and
// DISPOSABLE, but they are no longer derived from the seed alone: they are
// derived from (seed + persisted edits). The sieve regenerates pristine
// terrain, and PrepareTick hands each fill entry the cells the CPU's FarEdits
// index says the player changed, which the same `far` workgroup re-applies.
// Without it every refill — an incoming plane, a teleport, a world load —
// healed the horizon back to procgen and the player's crater vanished.
// FarEdits itself reconstructs from the ChunkStore region files, so nothing
// here is authoritative and nothing here is saved.
class FarField {
 public:
  void Init(World* world) { world_ = world; }

  // Recenter toward the player's FINE-chunk coord. Call between ticks (after
  // Stream::Update), before PrepareTick. A level whose window has gone
  // entirely stale (teleport, load) resets and refills wholesale.
  void Update(IVec3 playerChunk);

  // Pop up to kFarListCap queued fills, upload farList + farPatch (+ farUBO
  // when origins changed), and return the dispatch count for this tick's
  // TickParams.farCount / EncodeFarFill. May pop FEWER than kFarListCap when
  // this tick's patch payload budget (kFarPatchCap) is spent — the rest stay
  // queued, exactly as they do when the dispatch cap is the binding one.
  uint32_t PrepareTick(const rhi::Queue& queue);

  // Patch words uploaded by the last PrepareTick (diagnostics / selftest).
  uint32_t LastPatchWords() const { return lastPatchWords_; }

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
  std::deque<uint32_t> queue_;  // packed (level-1) << kFarSlotShift | slot
  // Outstanding entries per level, mirroring queue_ (the queue is FIFO across
  // levels, so scanning it per frame would be O(24576); these counters make
  // SafeRadiusMeters O(kFarLevels)). Incremented on every enqueue, decremented
  // as PrepareTick pops.
  uint32_t pending_[kFarLevels] = {};
  bool uboDirty_ = true;
  // Reused across ticks so a fill-heavy frame does not reallocate: the header
  // is 2 u32 per dispatched entry, the payload is the concatenated patch runs.
  std::vector<uint32_t> patchHeader_;
  std::vector<uint32_t> patchPayload_;
  uint32_t lastPatchWords_ = 0;
};
