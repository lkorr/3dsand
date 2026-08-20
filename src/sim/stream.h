#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "math3d.h"
#include "sim/chunkstore.h"
#include "sim/world.h"

class Simulation;
struct GpuContext;

// ---- chunk RLE (16-bit voxel words, stamp bytes stripped) ----
// Shared by streaming eviction and the region-file save format (chunkstore /
// worldio).
void RleEncodeChunk(const uint32_t* words, std::vector<uint16_t>& out);
// out must hold kChunkVol words; returns false on malformed input.
// Decoded voxels get stamp 0xFF ("hasn't acted"): everything may move.
bool RleDecodeChunk(const uint16_t* rle, size_t pairs, uint32_t* out);

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
  // One in-flight eviction batch: a staging buffer whose mapAsync has been
  // kicked but not yet consumed. mapStatus lives on the heap because the
  // callback outlives deque reshuffles (0 pending, 1 ok, 2 failed).
  struct PendingEvict {
    wgpu::Buffer staging;
    wgpu::Future future{};
    std::shared_ptr<uint32_t> mapStatus;
    struct Item {
      IVec3 wc;
      uint8_t dropIfAir;  // all-air + never modified => procgen reproduces it
    };
    std::vector<Item> items;
  };

  void ShiftAxis(int axis, int dir);
  // Encode readback copies for the slots and queue them as pending evictions.
  // filter=true applies the occupancy/modified save-worthiness test
  // (streaming); filter=false saves everything including air (whole-window
  // flush). Returns without blocking.
  void EvictSlots(const std::vector<uint32_t>& slots, bool filter);
  // Fill slots from store/procgen under the CURRENT window origin.
  void FillSlots(const std::vector<uint32_t>& slots);
  // Grab a pooled staging buffer, recycling the oldest pending batch if the
  // ring is full (bounds staging memory to kMaxPendingEvicts batches).
  wgpu::Buffer AcquireStaging();
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
  std::vector<uint8_t> modified_;   // per slot, sticky since last recycle
  std::deque<PendingEvict> pending_;
  std::vector<wgpu::Buffer> stagingPool_;
  std::unordered_map<uint64_t, uint32_t> pendingChunks_;  // packed wc -> count
};
