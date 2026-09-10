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

  // ---- THE VALID BOX (2026-09-10): what the RENDERER may march ------------
  // A level is toroidal: when its origin steps one level chunk toward the
  // player, the incoming face's SLOTS are the outgoing face's, and they keep
  // the outgoing face's bytes until the sieve refills them. That was invisible
  // while a plane landed in the tick it was queued; under kPlayFillCap a
  // plane takes ~16 ticks and sprint flight backlogs planes deep, so the
  // renderer, which marched the full box from `origins` the tick they moved,
  // drew the hillside BEHIND the player ahead of them and the underground of
  // the bottom face in the sky when the box stepped up.
  //
  // So the box the far readers march is the full box LESS the faces with
  // planes still queued (and empty while a reset is in flight), published in
  // FarParams.origins[k].w — the word every other reader ignores — as six
  // 4-bit counts, one per face (-x,+x,-y,+y,-z,+z, low nibble first; the
  // count saturates at 15), plus bit 24 = whole level pending. raymarch.wgsl
  // `farBox` unpacks it; a ray in an excluded slab leaves the level at the
  // shrunken face and the next coarser level, which is filled, picks up at the
  // same t (the seam contract traceFar already keeps for TUNE_FAR_STEPS).
  // Nothing on the render path reads a slot before the sieve has written it.
  //
  // Counted per PLANE, not per entry: each EnqueuePlane / ResetLevel pushes a
  // record onto recs_ (FIFO, parallel to queue_) with its entry count, and a
  // face count drops only when that plane's LAST entry is popped — the queue
  // is FIFO so a plane's entries are contiguous and drain in order. A reset
  // bumps the level's epoch and zeroes its faces: plane records queued before
  // it still pop (the queue is not reordered) but no longer own a face.
  // Always conservative — a face that reversed direction is excluded on both
  // sides until both records drain, and a landed face is published one tick
  // late (the UBO is written at the top of the NEXT PrepareTick), never early.
  uint32_t FaceWord(uint32_t k) const;   // the packed w (the selftest reads it)

 private:
  void ResetLevel(uint32_t k, IVec3 desired);        // origin jump + full refill
  void EnqueuePlane(uint32_t k, int axis, int wcoord);  // one incoming plane
  void Enqueue(uint32_t k, uint32_t slot);           // queue + per-level counter

  World* world_ = nullptr;
  IVec3 origins_[kFarLevels] = {};  // per level, level-chunk units
  std::deque<uint32_t> queue_;  // packed (level-1) << kFarSlotShift | slot
  // One record per EnqueuePlane / ResetLevel, in queue order (see FaceWord).
  // axis < 0 is a reset: it drains like any other record but owns no face.
  struct PlaneRec {
    uint32_t level;      // 0-based
    int axis;            // 0..2, or -1 for a reset
    int side;            // 0 = low face (-), 1 = high face (+)
    uint32_t remaining;  // entries of this plane still in queue_
    uint32_t epoch;      // epoch_[level] when queued; stale after a reset
  };
  std::deque<PlaneRec> recs_;
  uint32_t faces_[kFarLevels][3][2] = {};  // planes queued per level face
  uint32_t epoch_[kFarLevels] = {};
  // Outstanding entries per level, mirroring queue_ (the queue is FIFO across
  // levels, so scanning it per frame would be O(24576); these counters make
  // SafeRadiusMeters O(kFarLevels)). Incremented on every enqueue, decremented
  // as PrepareTick pops.
  uint32_t pending_[kFarLevels] = {};
  // Of those, how many came from a RESET (ResetLevel / FullRefill: a whole
  // level, 32,768 entries) rather than from an incoming plane. Popped first,
  // because the queue is FIFO and a reset's entries precede any plane queued
  // after it. Two consumers: PrepareTick's cap (a reset takes kFarListCap per
  // tick — the horizon has to exist — while planes take kPlayFillCap), and
  // SafeRadiusMeters (a plane-incomplete level is trusted out to its edge
  // minus the plane; a reset-incomplete one only to the level inside it).
  uint32_t bulkPending_[kFarLevels] = {};
  // THE PLAY CAP (2026-09-09). A far entry is a 16^3-cell sieve — 4,096
  // genCell evaluations plus the surface skin — and measured ~0.2 ms of GPU
  // each on an RTX 3060 Ti: a level's incoming plane is 1,024 of them, so
  // walking across a level-1 chunk boundary (every 32 voxels) put 180-240 ms
  // of GPU into ONE tick. `--frames 900 --autowalk`: farField p50 0.002 ms,
  // p99 184 ms, max 206 ms — the largest GPU spike in the walking frame, and
  // what the CPU saw as a present wait or, before the tick throttle, a
  // snapshot stall. 64 per tick is ~12 ms of GPU, spreads a plane over 16
  // ticks, and covers walking demand (~50 entries/tick across all levels)
  // with headroom. Sprint flight demands ~700/tick and backlogs at ANY cap the
  // frame can afford; that is the render-only horizon lagging the player,
  // which the adaptive fog already covers, and it drains the moment the
  // player slows. Resets are exempt (see bulkPending_).
  //
  // The MEAN is inherent, only the shape moves: walking costs ~8.5 ms/tick of
  // sieve whatever the cap (an L1 plane per 32 voxels ~ 5 ms per voxel of
  // travel, plus the coarser levels). Measured `--autowalk --duel-dummy`,
  // whole-frame p50 / p95 / p99 / max, >33 ms: uncapped 19.8 / 28 / 52 / 153,
  // 2.5%; cap 64: 20.6 / 43 / 54 / 73, 19%; cap 32: 29.1 / 48 / 64 / 81, 42%.
  // 32 put ~5 ms on nearly every frame and the median frame lost a vblank;
  // 64 keeps the median and only cuts the tail. The next lever is the sieve's
  // per-entry cost, not this number.
  static constexpr uint32_t kPlayFillCap = 64;
  bool uboDirty_ = true;
  // Reused across ticks so a fill-heavy frame does not reallocate: the header
  // is 2 u32 per dispatched entry, the payload is the concatenated patch runs.
  std::vector<uint32_t> patchHeader_;
  std::vector<uint32_t> patchPayload_;
  uint32_t lastPatchWords_ = 0;
};
