#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "math3d.h"
#include "sim/chunkstore.h"
#include "sim/materials.h"
#include "sim/world.h"

class Simulation;
struct GpuContext;

// ---- chunk RLE (32-bit voxel words, stamp bytes stripped) ----
// Shared by streaming eviction and the region-file save format (chunkstore /
// worldio). The word is 32-bit so the STAIN layer (bits 24..30) round-trips:
// it is hashed sim state, and a 16-bit store dropped it silently on save.
//
// What the store keeps. Everything EXCEPT the tick-stamp byte (bits 16..23),
// which is per-tick scheduling scratch rather than state — it is excluded from
// the world hash for the same reason, and a restored voxel is re-stamped
// kStampNever so it is born "has never acted" (CLAUDE.md's tick-stamp rule).
//
// This lives in the header rather than as a file-static in stream.cpp because
// the Vulkan port needed a second consumer that reproduced the store round-trip
// EXACTLY, and a copy of the literal there was a "two places that must agree"
// bug in waiting — it silently produced a divergence that read like a barrier
// race (docs/PLAN_vulkan_port.md, phase 3c). That episode is the reason; the
// rule outlives it.
inline constexpr uint32_t kPersistMask = 0xFF00FFFFu;  // everything but the stamp byte

void RleEncodeChunk(const uint32_t* words, std::vector<uint32_t>& out);
// out must hold kChunkVol words; returns false on malformed input.
// Decoded voxels get kStampNever ("hasn't acted"): everything may move.
bool RleDecodeChunk(const uint32_t* rle, size_t pairs, uint32_t* out);

// Toroidal residency manager (M2/M7): recenters the resident cube on the
// player one chunk at a time. A shift reads the leaving plane back
// ASYNCHRONOUSLY (save-worthy chunks only): the copy into a pooled staging
// buffer is encoded before the slots are refilled (queue order makes the copy
// see pre-fill data), the mapAsync completes ticks later, and only then is
// the plane RLE'd into the store. The frame never blocks on eviction.
//
// The pending-eviction set is the correctness half: FillSlots force-completes
// any in-flight eviction of a chunk it is about to load (player doubled back
// inside the map latency), and save/load/regen drain (or discard) the queue.
// Chunk-level ordering falls out of that: a chunk cannot be re-evicted until
// it was refilled, and refilling drained its previous eviction.
//
// Known accepted race (unchanged from v1): eviction save-worthiness comes
// from the latest snapshot's occupancy/dirty flags, which lag the GPU by the
// readback ring depth. Sim activity that starts on the trailing plane in
// those last ~2 ticks can be evicted as "boring" and lost on re-entry. The
// trailing plane is >= 6 chunks behind the player, so only self-propelled
// fronts (fire, liquids) can be there — bounded, cosmetic.
class Stream {
 public:
  void Init(GpuContext* ctx, World* world, Simulation* sim, uint32_t seed);

  // Rebuild the per-material ray-blocker table (occupancy high-16 packing —
  // see common.wgsl). Call after Init and again on material hot-reload.
  void OnMaterialsReloaded(const std::vector<MaterialDef>& mats);

  // Recenter toward playerChunk: at most one 1-chunk shift per axis per call,
  // 2-chunk hysteresis. Call BETWEEN ticks only — a shift must complete before
  // the next tick sees the new origin. Also folds the latest snapshot's dirty
  // flags into the sticky per-slot modified set.
  void Update(IVec3 playerChunk);

  // CPU-known writes (brush, explosions) mark chunks modified immediately —
  // the dirty-flag snapshot is ticks latent and eviction can't wait for it.
  // lo/hi are world VOXEL coords (inclusive box).
  void MarkModifiedBox(IVec3 lo, IVec3 hi);

  // Save every resident chunk (air included — no snapshot trust needed) into
  // the store. Used by SaveWorld before serializing the store.
  void FlushResident();

  // Fill the whole window at `origin` from the store (misses -> procgen) and
  // reset residency bookkeeping. Used by LoadWorld and world regen.
  void ReloadWindow(IVec3 origin);

  // World regen: the old world's chunks are gone (in-flight evictions too).
  // Clear() detaches from any bound save dir but leaves its files — the last
  // explicit save must survive a regen; the next save overwrites it.
  void OnRegen() {
    DrainEvictions(/*discard=*/true);
    store_.Clear();
    modified_.assign(kNumChunks, 0);
  }

  ChunkStore& Store() { return store_; }
  uint32_t ShiftCount() const { return shifts_; }
  size_t PendingEvictions() const { return pending_.size(); }

 private:
  // One in-flight eviction batch: a staging buffer whose map has been kicked
  // but not yet consumed. The ticket owns its own completion state on the heap,
  // so it survives deque reshuffles.
  struct PendingEvict {
    rhi::Buffer staging;
    // The map is issued at eviction time and consumed ticks later: Ready() is
    // the per-tick non-blocking harvest, Wait() the ring-full / drain path.
    rhi::MapTicket map;
    struct Item {
      IVec3 wc;
      uint8_t dropIfAir;  // all-air + never modified => procgen reproduces it
    };
    std::vector<Item> items;
    // Parallel to `items`: the page-table entry of a slot that was a SENTINEL
    // at eviction time and therefore had NO copy issued (§2.1a / §4.2). 0 means
    // a real copy landed in the staging buffer for that index.
    std::vector<uint32_t> sentinel;
  };

  void ShiftAxis(int axis, int dir);
  // Encode readback copies for the slots and queue them as pending evictions.
  // filter=true applies the occupancy/modified save-worthiness test
  // (streaming); filter=false saves everything including air (whole-window
  // flush). Returns without blocking.
  void EvictSlots(const std::vector<uint32_t>& slots, bool filter);
  // The SHIFT-LOCAL eviction snapshot (the same-frame stall fix).
  //
  // EvictSlots inserts every slot of the leaving plane into pendingChunks_,
  // and FillSlots is then handed THAT SAME PLANE. Its "player doubled back"
  // drain therefore hit on the first slot of every shift and blocked the frame
  // on maps submitted microseconds earlier — four batches through a four-slot
  // ring, so the ring was always full too. Both stalls have one cause: the
  // refill asks the GPU for bytes the CPU is about to overwrite anyway.
  //
  // The resolution is that a shift does not NEED those bytes. A slot leaving
  // the window is refilled from the store or from procgen under the NEW
  // origin — its old contents matter only to the SAVE, which the pending batch
  // already owns. So the fill path must not consult pendingChunks_ for slots
  // this shift itself evicted; it may only wait for an eviction from an
  // EARLIER shift, which is the genuine doubled-back case the drain was
  // written for. This set is those slots, live for the duration of one shift.
  std::vector<uint8_t> shiftEvicted_;
  // Fill slots from store/procgen under the CURRENT window origin.
  void FillSlots(const std::vector<uint32_t>& slots);
  // Grab a pooled staging buffer, recycling the oldest pending batch if the
  // ring is full (bounds staging memory to kMaxPendingEvicts batches).
  rhi::Buffer AcquireStaging();
  // Block on the oldest pending batch, RLE it into the store (unless
  // discarding), return its buffer to the pool.
  void CompleteOldest(bool discard);
  void DrainEvictions(bool discard = false);

  GpuContext* ctx_ = nullptr;
  World* world_ = nullptr;
  Simulation* sim_ = nullptr;
  uint32_t seed_ = 0;
  uint32_t shifts_ = 0;
  ChunkStore store_;
  std::vector<uint8_t> blockerOf_;  // per material: stops a ray (occ high 16)
  std::vector<uint8_t> modified_;   // per slot, sticky since last recycle
  std::deque<PendingEvict> pending_;
  std::vector<rhi::Buffer> stagingPool_;
  std::unordered_map<uint64_t, uint32_t> pendingChunks_;  // packed wc -> count
};
