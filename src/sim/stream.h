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

  // ---- R4: AT MOST ONE WINDOW SHIFT PER FRAME -----------------------------
  //
  // Update() is a per-TICK call and the frame loop runs up to four ticks on a
  // catch-up frame (main.cpp's tick clamp), so a frame that was already slow
  // used to pay two or three shifts and get slower — the self-sustaining loop
  // that put 19 frames of a 540-frame surface flight over 100 ms
  // (docs/RESEARCH_streaming_hitch.md §1).
  //
  // Deferring the extra shifts to the next frame is safe by exactly the
  // argument the "one axis per call" rule already uses: kHysteresis is 2
  // chunks, the player is 13+ chunks from the window edge, and an axis that is
  // 2 out stays resident and correct for another 2 chunks of travel.
  //
  // CALLERS WITHOUT A FRAME LOOP ARE UNAFFECTED. The selftest gates and
  // vk_smoke drive Update() directly, one call per tick, and never call this;
  // `frameGated_` stays false and every Update may shift, which is what those
  // harnesses already did (one Update = one tick = at most one shift anyway).
  // Gating them would silently change every streaming gate's shift schedule.
  //
  // Pending-shift COMPLETION (the T+K half of the deferred wake) is NOT gated
  // by this: it runs per tick, from Update, because its deadline is measured
  // in ticks.
  void BeginFrame() { frameGated_ = true; shiftedThisFrame_ = false; }

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
    DiscardPendingShifts();  // the verdicts describe the REPLACED world
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
    double demoteMs = 0;   // the T+K completion: act set, sky demote, copy issue
    // The T+K poll that was NOT ready (docs/RESEARCH_streaming_hitch.md R1).
    // The deferred wake replaces a per-shift fence with a poll kWakeLatency
    // ticks later; if the GPU is still that far behind, the poll degrades to
    // the old Wait(). Counted and printed so "the fence came back" is visible
    // rather than inferred from a frame-time distribution.
    double wakeWaitMs = 0;
    uint32_t wakeWaits = 0;
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
  //
  // `deferWake` selects R1's two-phase pipeline for the generated slots: the
  // kernel publishes its act verdict to `genAct` instead of writing dirtyIn,
  // the occupancy/genAct readback is queued WITHOUT a Wait, and a PendingShift
  // records the work owed at tick T + kWakeLatency. Only ShiftAxis passes
  // true. ReloadWindow passes false — it fills the WHOLE window (32,768 slots,
  // far past genAct's one-plane size) and a load has no frame to protect.
  void FillSlots(const std::vector<uint32_t>& slots, bool deferWake);
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

  // ---- R1: the deferred shift wake (docs/RESEARCH_streaming_hitch.md) ------
  //
  // WHAT THIS REPLACED. FillSlots used to submit genChunk and then block on a
  // readback of `occupancy` so the CPU page-table mirror could learn the act
  // set in the SAME tick — Materialize has to give a woken chunk's
  // 26-neighbourhood pages before the CA runs on it. MapReadDeferred borrows
  // the fence of the LAST submit, and a fence on one queue waits for
  // everything queued before it, so that wait sat behind the previous frame's
  // render and the previous ticks' CA: 33.06 ms of a 34.80 ms shift, 618
  // shifts in 540 frames.
  //
  // THE PIPELINE. At tick T the plane is evicted, paged, generated and
  // rendered — and left INERT (genChunk clears dirtyIn/dirtyOut for it and
  // writes its act verdict to `genAct` instead). The readback is queued with
  // no wait. At tick T + kWakeLatency the ticket is polled, today's CPU logic
  // runs on the data, and only then is the act set put into dirtyIn.
  //
  // The world hash moves once, because a plane now first acts at T+K instead
  // of T. K is a CONSTANT, so "when does a plane first act" is still a pure
  // function of (inputs, tick) and the twice-run determinism comparison is
  // untouched. What is NOT allowed to differ is paged vs dense, which is why
  // the deferral applies in BOTH residency modes: dense skips the page-table
  // half of the completion and takes the same wake at the same tick.
  struct PendingShift {
    rhi::Buffer staging;
    rhi::MapTicket map;
    // genList as submitted: parallel to the genAct entries in the readback.
    std::vector<uint32_t> genSlots;
    // Slots dropped because a LATER shift re-generated them under a new
    // origin (see the intersection rule in CompleteShift): the entry keeps
    // its readback offsets intact and simply skips these.
    std::vector<uint8_t> stale;
    uint32_t tick = 0;   // T, the tick the plane was generated on
  };
  // K, AND WHAT ACTUALLY BOUNDS IT.
  //
  // The obvious bound is the one the research doc reaches for: K ticks of
  // travel must stay under kHysteresis so the plane cannot scroll back out
  // before its wake lands (0.67 chunk/tick at sprint => K < 3). That bound is
  // real but it is NOT the one in force here, because the reversal it worries
  // about is handled explicitly rather than made improbable:
  // InvalidatePendingSlots blanks any pending verdict for a slot a later shift
  // re-generates, and the later shift's own entry then covers those slots. A
  // reversal is therefore correct at any K.
  //
  // What K actually costs is (a) the plane stays INERT for K ticks — 133 ms at
  // K=4, on terrain 6+ chunks (9.6 m) from the player, where nothing is
  // visible and nothing is reachable — and (b) ~586 sky pages per plane are
  // held K ticks longer.
  //
  // K=2 was measured and was not enough. `--frames 600 --autofly-surface`:
  // the CPU work fell exactly as designed (the shift's `demote` term went
  // 33.06 -> 0.31 ms) but the T+K poll was still not ready on 296 of 390
  // shifts and blocked 16.86 ms each. The engine is GPU-bound at ~21 ms and
  // runs under one tick per frame at that rate, so two ticks is not two
  // frames of drain — it is not even one. The fence has to cover a whole
  // frame's render plus the GI passes plus the intervening CA submits.
  //
  // Blocking is the ONLY legal answer when the poll misses: deferring the
  // completion another tick would make the tick a plane first acts on a
  // function of fence timing, and that is the determinism rule. So K is
  // raised until the miss is rare instead.
  static constexpr uint32_t kWakeLatency = 4;
  // Finish every pending shift whose T + kWakeLatency deadline has arrived.
  // Called from Update BEFORE the shift below it, so a plane's verdict is
  // consumed before a new shift can invalidate any of its slots.
  void CompleteDueShifts(uint32_t tick);
  void CompleteShift(PendingShift& ps, uint32_t tick);
  // Mark every pending entry's copy of `slots` stale: a new shift is about to
  // regenerate them under a different origin, so the older entry's verdict for
  // them describes a chunk that no longer lives there. Dropping is the simple
  // correct option — the NEW shift's own entry covers the same slots two ticks
  // later, and every action the old entry would have taken (RefilledSlot, the
  // PT_EMPTY demote, the demote copy) is one the new entry takes instead. It
  // is also mandatory rather than tidy: a stale "pure sky" verdict applied to
  // a slot that now holds fresh stone would SetSentinel(PT_EMPTY) over it.
  void InvalidatePendingSlots(const std::vector<uint32_t>& slots);
  // Drop every pending entry, bytes and all. Mandatory on regen / LoadWorld:
  // the verdicts belong to the replaced world (the DiscardDemotes argument,
  // verbatim). The planes stay inert, which is correct — ReloadWindow refills
  // and wakes the whole window itself.
  void DiscardPendingShifts();
  // The CPU half of a streamed-in plane's verdict, shared by both callers:
  // the SYNCHRONOUS fill (ReloadWindow, which reads occupancy right there) and
  // the DEFERRED one (a shift, K ticks later). `act` is genChunk's per-genList
  // act verdict and is empty for the synchronous path, where the kernel woke
  // the slots itself; a non-empty `act` means this call owes the dirty writes.
  void ApplyGenVerdict(const std::vector<uint32_t>& genSlots,
                       const std::vector<uint8_t>& stale,
                       const std::vector<uint32_t>& occ, bool occValid,
                       const std::vector<uint32_t>& act, uint32_t tick);
  std::deque<PendingShift> pendingShifts_;
  // Staging for the deferred readbacks: occupancy (kNumChunks u32) followed by
  // one genAct u32 per generated slot, in ONE buffer so a shift costs one
  // submit and one map. Pooled because up to kWakeLatency + 1 are in flight.
  std::vector<rhi::Buffer> shiftStagingPool_;
  rhi::Buffer AcquireShiftStaging();
  std::vector<uint32_t> genActScratch_;   // mapped-memory bounce, reused
  std::vector<uint8_t> shiftMark_;        // InvalidatePendingSlots' membership bitmap
  bool frameGated_ = false;      // BeginFrame has been called at least once
  bool shiftedThisFrame_ = false;

  std::deque<PendingDemote> demotes_;
  std::vector<uint32_t> demoteScratch_;  // mapped-memory bounce, reused
  std::vector<uint32_t> evictScratch_;   // same, for the eviction harvest
  // The last tick Update() saw: FillSlots stamps demote copies with it.
  // ReloadWindow runs before any Update, so its copies carry a stale tick and
  // take the harvest's re-copy path — lazily correct, never wrong.
  uint32_t lastTick_ = 0;
};
