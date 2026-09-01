#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "math3d.h"
#include "sim/chunkstore.h"
#include "sim/faredits.h"
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
// The RLE a SENTINEL chunk would produce, computed without materializing its
// 4,096 words. BYTE-IDENTICAL to synthesizing with SynthWordAt then calling
// RleEncodeChunk — a fusion of those two loops, never a second encoding. That
// equality is what keeps the save format unchanged (§4.2), and the
// page-roundtrip selftest asserts it directly.
void RleEncodeSentinelChunk(uint32_t entry, IVec3 wc, uint32_t seed,
                            std::vector<uint32_t>& out);
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
  // flags into the sticky per-slot modified set, and harvests completed
  // shift-demote batches (see PendingDemote).
  //
  // `tick` is the sim tick about to be encoded. The demote harvest needs it
  // for its staleness bound — a copied chunk may only be classified while the
  // copy is provably younger than the mirror's write→settle→tighten-out
  // latency (see HarvestDemotes) — so it must be the REAL tick, not 0.
  void Update(IVec3 playerChunk, uint32_t tick);

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
    DiscardDemotes();
    store_.Clear();
    farEdits_.Clear();
    modified_.assign(kNumChunks, 0);
  }

  ChunkStore& Store() { return store_; }
  // The far-field edit index. LoadWorld rebuilds it from the store it just
  // bound; nothing else outside Stream writes it.
  FarEdits& Edits() { return farEdits_; }
  uint32_t ShiftCount() const { return shifts_; }
  size_t PendingEvictions() const { return pending_.size(); }

  // ---- WHERE A WINDOW SHIFT'S TIME GOES ------------------------------------
  //
  // Streaming is by a wide margin the largest CPU item in the live frame — 93%
  // of CPU busy under --autofly-hard, p50 0.00 ms and p99 42 ms, because a
  // shift is all-or-nothing and lands on one frame. Until this struct existed
  // the whole of it was ONE number on the Performance tab, and that number was
  // not even labelled `stream`: Update runs inside the tick body, which had no
  // timer, so it fell into the frame residual and the residual was billed to
  // `input`. The user-visible symptom was "input spikes to 30 ms when flying".
  //
  // One number for a 40 ms stall is the failure CLAUDE.md rule 6 names, and it
  // had already cost a wrong answer here: the comment at the demote map wait in
  // FillSlots records `occ 39.55 ms` and calls it "THE remaining cost of the
  // surface band, and it is not close". Re-measured with the same
  // SANDVOX_PT_DEBUG=1 probe, that pass is now 2.4-9.3 ms. The cost MOVED and
  // the prose did not, so a session reading it would have spent its budget
  // optimising a pass that is already 8x cheaper than advertised.
  //
  // Accumulated always (a handful of clock reads against a 40 ms stall) and
  // reported by `--frames`, so re-checking it is one non-interactive command.
  struct Timing {
    double evictMs = 0;    // EvictSlots: RLE encode, store insert, copy encode
    double fillStoreMs = 0;// FillSlots' store-hit branch: decode + per-slot uploads
    double fillGenMs = 0;  // genList upload + EncodeGenList + submit
    double demoteMs = 0;   // the post-genChunk occupancy map wait + candidate scan
    double harvestMs = 0;  // HarvestDemotes + CompleteOldest at the top of Update
    double dirtyFoldMs = 0;// the per-tick kNumChunks fold of snapshot dirty flags
    double totalMs = 0;    // the whole of Update, so the parts can be checked
    uint32_t shifts = 0;
  };
  const Timing& Timings() const { return timing_; }

 private:
  // One in-flight eviction batch: a staging buffer whose map has been kicked
  // but not yet consumed. The ticket owns its own completion state on the heap,
  // so it survives deque reshuffles.
  struct PendingEvict {
    rhi::Buffer staging;
    // The map is issued at eviction time and consumed ticks later: Ready() is
    // the per-tick non-blocking harvest, Wait() the ring-full / drain path.
    rhi::MapTicket map;
    // No `dropIfAir` any more: "never modified => it is re-derivable, don't
    // store it" is now decided in EvictSlots BEFORE the copy is issued, for
    // every slot rather than only for the ones whose words turn out to be air.
    // Nothing that reaches this struct is droppable.
    struct Item {
      IVec3 wc;
      // Was this chunk MODIFIED, or is it here only because a save asked for
      // every resident chunk (FlushResident's filter=false)? The far-field
      // edit index (src/sim/faredits.h) wants the first kind and not the
      // second: a pristine chunk's patch is a no-op on the GPU (the sieve
      // would have produced the same byte) but 73 index entries per chunk on
      // the CPU, and a save touches the whole 32,768-slot window.
      bool edited = false;
    };
    std::vector<Item> items;
    // Parallel to `items`: the page-table entry of a slot that was a SENTINEL
    // at eviction time and therefore had NO copy issued (§2.1a / §4.2). 0 means
    // a real copy landed in the staging buffer for that index.
    std::vector<uint32_t> sentinel;
  };

  // One in-flight SHIFT-DEMOTE batch: voxel copies of the entering plane's
  // demote candidates, issued right after genChunk's submit (queue order makes
  // the copy read post-gen data) but CLASSIFIED on a later frame's harvest —
  // the map.Wait and the JITTER word-verify were the shift frame's two largest
  // remaining stalls, and neither is needed synchronously: with residency at
  // ~1.2k of a 32,768-page pool, demotion is allowed to lag by frames.
  struct PendingDemote {
    rhi::Buffer staging;
    rhi::MapTicket map;
    std::vector<uint32_t> slots;   // slot indices at copy time
    std::vector<uint64_t> keys;    // PackChunkKey(world chunk) at copy time
    std::vector<uint8_t> copied;   // a real copy landed for this index
    uint32_t copyTick = 0;         // sim tick the copy was issued on
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
  Timing timing_;
  ChunkStore store_;
  // Fed by the same eviction path that fills store_, and reconstructible from
  // it (FarEdits::RebuildFromStore). Owned here rather than by World because
  // "which chunks diverged from procgen" is streaming's answer, not the
  // renderer's; World just holds the pointer so FarField can reach it.
  FarEdits farEdits_;
  std::vector<uint8_t> blockerOf_;  // per material: stops a ray (occ high 16)
  std::vector<uint8_t> modified_;   // per slot, sticky since last recycle
  std::deque<PendingEvict> pending_;
  std::vector<rhi::Buffer> stagingPool_;
  std::unordered_map<uint64_t, uint32_t> pendingChunks_;  // packed wc -> count
  // The shift-demote occupancy prefilter's staging buffer and host copy, kept
  // alive across shifts. Both were re-created per shift by rhi::ReadbackBlocking
  // (a fresh 128 KiB buffer + a full queue drain), and shifts land on
  // consecutive frames under flight.
  rhi::Buffer genOccStaging_;
  std::vector<uint32_t> genOccScratch_;

  // ---- deferred shift-demote pipeline ----
  // Issue copies for demote candidates (batched, deferred maps, no wait).
  void IssueDemoteCopies(const std::vector<uint32_t>& slots,
                         const std::vector<uint64_t>& keys, uint32_t tick);
  // Classify+demote every COMPLETED batch; never blocks. Stale batches are
  // re-copied rather than trusted (see the staleness note at the definition).
  void HarvestDemotes(uint32_t tick);
  // Throw away every queued demote batch, bytes and all. MANDATORY on regen /
  // LoadWorld: the bytes belong to the REPLACED world, and while the identity
  // key skips most of them, an EMPTY/UNIFORM classification is seed-blind — a
  // same-coordinate chunk of the new world could demote on the old world's
  // bytes and lose voxels.
  void DiscardDemotes();
  std::deque<PendingDemote> demotes_;
  std::vector<uint32_t> demoteScratch_;  // mapped-memory bounce, reused
  std::vector<uint32_t> evictScratch_;   // same, for the eviction harvest
  // The last tick Update() saw: FillSlots stamps demote copies with it.
  // ReloadWindow runs before any Update, so its copies carry a stale tick and
  // take the harvest's re-copy path — lazily correct, never wrong.
  uint32_t lastTick_ = 0;
};
