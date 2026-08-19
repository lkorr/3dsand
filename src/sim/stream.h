#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

class Simulation;
struct GpuContext;

// ---- chunk RLE (16-bit voxel words, stamp bytes stripped) ----
// Shared by streaming eviction and the .svx save format (worldio.cpp).
void RleEncodeChunk(const uint32_t* words, std::vector<uint16_t>& out);
// out must hold kChunkVol words; returns false on malformed input.
// Decoded voxels get stamp 0xFF ("hasn't acted"): everything may move.
bool RleDecodeChunk(const uint16_t* rle, size_t pairs, uint32_t* out);

// Chunks that left the residency window (DESIGN.md §3 streaming). v1 keeps the
// store in RAM — falling-sand chunks RLE to ~tens-to-hundreds of bytes, so
// exploring 100x the residency costs tens of MB — and it serializes wholesale
// into the .svx save. This interface is the contract; a region-file disk
// backend can replace the map without touching callers.
class ChunkStore {
 public:
  struct Entry {
    IVec3 wc;
    std::vector<uint16_t> rle;
  };
  void Put(IVec3 wc, std::vector<uint16_t> rle) {
    Entry& e = map_[World::PackChunkKey(wc)];
    e.wc = wc;
    e.rle = std::move(rle);
  }
  const std::vector<uint16_t>* Get(IVec3 wc) const {
    auto it = map_.find(World::PackChunkKey(wc));
    return it == map_.end() ? nullptr : &it->second.rle;
  }
  void Clear() { map_.clear(); }
  size_t Count() const { return map_.size(); }
  const std::unordered_map<uint64_t, Entry>& Map() const { return map_; }

 private:
  std::unordered_map<uint64_t, Entry> map_;
};

// Toroidal residency manager (M2/M7): recenters the resident cube on the
// player one chunk at a time. A shift synchronously reads the leaving plane
// back (save-worthy chunks only), RLEs it into the store, then fills the
// recycled slots from the store or by compute procgen (worldgen.wgsl `list`).
//
// Known accepted race (v1): eviction save-worthiness comes from the latest
// snapshot's occupancy/dirty flags, which lag the GPU by the readback ring
// depth. Sim activity that starts on the trailing plane in those last ~2
// ticks can be evicted as "boring" and lost on re-entry. The trailing plane
// is >= 6 chunks behind the player, so only self-propelled fronts (fire,
// liquids) can be there — bounded, cosmetic, revisit with async eviction.
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

  // World regen: the old world's chunks are gone.
  void OnRegen() {
    store_.Clear();
    modified_.assign(kNumChunks, 0);
  }

  ChunkStore& Store() { return store_; }
  uint32_t ShiftCount() const { return shifts_; }

 private:
  void ShiftAxis(int axis, int dir);
  // Read slots back and Put into the store. filter=true applies the
  // occupancy/modified save-worthiness test (streaming); filter=false saves
  // everything including air (whole-window flush).
  void EvictSlots(const std::vector<uint32_t>& slots, bool filter);
  // Fill slots from store/procgen under the CURRENT window origin.
  void FillSlots(const std::vector<uint32_t>& slots);

  GpuContext* ctx_ = nullptr;
  World* world_ = nullptr;
  Simulation* sim_ = nullptr;
  uint32_t seed_ = 0;
  uint32_t shifts_ = 0;
  ChunkStore store_;
  std::vector<uint8_t> modified_;   // per slot, sticky since last recycle
};
