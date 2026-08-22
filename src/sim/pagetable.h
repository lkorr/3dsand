// pagetable.h — the CPU half of the software page table (docs/PLAN_page_table.md).
//
// DERIVED DATA ONLY: the page table is a physical-layout index, not world
// state. It is not hashed, not persisted, and not replicated. It is rebuilt
// from the chunk contents on every load, stream-in and worldgen (§4.2). Two
// different page assignments for the same logical world are the same world.
//
// WHAT THIS OWNS, and why it is one object rather than fields on World:
//
//   1. THE ALLOCATOR. A LIFO stack of free page indices. No fragmentation is
//      possible — every allocation is exactly one page and every page is
//      exactly kChunkVol words — which is the single biggest reason the flat
//      table beats anything hierarchical (§3.7).
//   2. `cpuDirty`, the CONSERVATIVE MIRROR of the GPU dirty set, and the §3.2
//      recurrence that maintains it. This is the crux of the whole phase: a
//      GPU kernel cannot allocate, so every page a kernel might write must
//      exist before the command buffer is submitted, which turns the problem
//      into "at encode time, what is the set of chunks this tick could write?"
//   3. The MATERIALIZATION decision and the queued page fills that realize it.
//
// They live together because they are one invariant: `cpuDirty` is exactly
// what decides materialization, and materialization is exactly what makes a
// page-fault impossible. Splitting them would be two things that must agree.
//
// NOT DETERMINISM STATE. Nothing here feeds the world hash. Page assignment
// depends on the snapshot cadence — which readbacks arrive on which tick, and
// whether the ring declined — and that cadence is a function of GPU timing,
// not of tick inputs. So two runs of the same seed can assign different page
// indices to the same chunk, and that is harmless BECAUSE the table is not
// hashed and not saved. A debugger comparing two runs should compare table
// entries by SLOT, never page indices (§3.7, review m4).

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "gpu/rhi.h"
#include "math3d.h"
#include "sim/world.h"

// Global scope, matching world.h and sim/world.h's World — this type is a
// peer of World, not a game-layer type.

// A set of chunk slots, as a bitset plus the member list. Both, because the
// two operations this type exists for want different shapes: membership tests
// are O(1) on the bitset, and the N26 dilation iterates MEMBERS — never all
// 32,768 slots. That is the rule-2 story for the whole mechanism: a settled
// world's cpuDirty is empty and dilating it iterates zero elements.
class SlotSet {
 public:
  SlotSet() : bits_((kNumChunks + 63) / 64, 0) {}

  bool Has(uint32_t s) const { return (bits_[s >> 6] >> (s & 63)) & 1u; }
  bool Add(uint32_t s) {
    uint64_t& w = bits_[s >> 6];
    const uint64_t m = 1ull << (s & 63);
    if (w & m) return false;
    w |= m;
    members_.push_back(s);
    return true;
  }
  void Clear() {
    for (uint32_t s : members_) bits_[s >> 6] = 0;
    members_.clear();
  }
  void SetAll() {
    members_.clear();
    members_.reserve(kNumChunks);
    for (uint32_t i = 0; i < kNumChunks; i++) members_.push_back(i);
    for (uint64_t& w : bits_) w = ~0ull;
  }
  const std::vector<uint32_t>& Members() const { return members_; }
  size_t Size() const { return members_.size(); }
  bool Empty() const { return members_.empty(); }

  // this <- this INTERSECT other. The only way an estimate is ever narrowed
  // (§3.2 step 2) — never an assignment, because both operands are supersets
  // of the true dirty set and only their intersection is guaranteed to be one.
  void IntersectWith(const SlotSet& other);
  void UnionWith(const SlotSet& other);

 private:
  std::vector<uint64_t> bits_;
  std::vector<uint32_t> members_;
};

// Adds every window-resident chunk adjacent to a member of `in` — INCLUDING
// diagonals — to `out`, along with `in` itself. The 26-neighbourhood and not
// the 6: a cell at a chunk CORNER writes to a destination differing on all
// three axes, landing in the corner-diagonal chunk, and markDirty marks
// exactly that chunk (§3.2). The face-only version loses a voxel on ~1 in
// 4,096 cells at a chunk corner — far too rare to catch by looking and
// immediately fatal to the hash.
void DilateN26(const SlotSet& in, SlotSet& out);

class PageTable {
 public:
  // `residency` decides the pool size and whether sentinels ever exist.
  void Init(const rhi::Device& device, World& world);

  // ---- the §3.2 recurrence, in the order the normative definitions give ----

  // Step (0)+(1): propagate. cpuDirty(N+1) = N26(cpuDirty(N)) u C(N), where
  // C(N) = opTargets(N) u particleChunks(N). Called once per tick BEFORE
  // materialization, with the tick's op targets already declared.
  void BeginTick(uint32_t tick);

  // Contributor (a) to C(N): a chunk touched by a brush / cell / explosion op.
  // Declared as the op vectors are assembled, which is where the CPU already
  // computes opsCount/expCount/cellCount.
  void AddOpTarget(uint32_t slot);
  void AddOpSphere(IVec3 centerCell, int radius, const World& world);
  void AddOpBox(IVec3 centerCell, int halfExtent, const World& world);

  // Contributor (b) to C(N): the conservative swept-particle set (§3.4).
  void UpdateParticles(bool particlesActive, uint32_t liveCount,
                       const std::vector<IVec3>& spawnCells,
                       const std::vector<IVec3>& explosionCenters,
                       const World& world);

  // Step (2): tighten against an arriving snapshot, by INTERSECTION only.
  // `snapTick` is the tick the snapshot was stamped at; `encodeTick` the tick
  // being encoded. Skips entirely when the gap exceeds what the C(j) ring can
  // cover — never tightens with an incomplete superset (§9 open question 6).
  void TightenFromSnapshot(const std::vector<uint8_t>& dirtyFlags,
                           uint32_t snapTick, uint32_t encodeTick);

  // Step (3), contributor (c): the daylight wake-all. Applied strictly AFTER
  // the tightening — a union after an intersection cannot be undone by it, so
  // the wake's 32,768 chunks survive regardless of when the snapshot landed.
  void WakeAll();
  // Step (3), contributor (d): a slot refilled by Stream::FillSlots, which
  // writes dirty[0] AND dirty[1] for it. Same ordering rule as (c).
  void RefilledSlot(uint32_t slot);

  // Step (4): materialize. Allocates a page for every chunk in
  //   [(cpuDirty n nonSentinel) u N26(cpuDirty n nonSentinel)]
  //     u opTargets(N) u particleChunks(N)
  // and queues its initialization fill. The two unfiltered terms are writes
  // into cells the CPU CHOSE, and a CPU op genuinely reaches isolated empty
  // sky — filtering them would make a brush silently no-op in open sky, which
  // is the single most visible thing a player can do (§3.2 step 4).
  //
  // Aborts the process on pool exhaustion (§3.8, settled): if the pool can
  // exhaust in normal play the pool is mis-sized, and the right response to a
  // bug is to fail loudly at the moment of detection.
  void Materialize(const rhi::Queue& queue);

  // ---- classification: the paths that already hold the words ---------------
  // A chunk whose 4,096 words the CPU has in hand is classified for free —
  // the caller is already looping over every word. Returns the table entry to
  // install: PT_EMPTY, a UNIFORM sentinel, or kNeedsPage.
  static constexpr uint32_t kNeedsPage = 0u;  // never a valid sentinel
  static uint32_t Classify(const uint32_t* words);

  // Install a sentinel for a slot, freeing any page it held. Used by
  // worldgen's post-pass compaction and by streaming/LoadWorld classification.
  void SetSentinel(uint32_t slot, uint32_t entry);
  // Ensure a slot has a page, allocating if needed; returns the byte offset.
  // Queues no fill — the caller is about to overwrite the whole chunk.
  uint64_t EnsurePageForOverwrite(uint32_t slot);

  // Push the dirtied span of the CPU table to the GPU. Small per-slot writes
  // ride the pending-upload queue and drain at the head of the next command
  // buffer, exactly like the three existing per-slot streaming writes.
  void FlushTableWrites(const rhi::Queue& queue);

  // ---- deallocation with hysteresis (§3.6) --------------------------------
  //
  // Driven from the occupancy readback the CPU ALREADY receives every tick in
  // the snapshot. No new readback, no new scan, no new GPU work — that is the
  // rule-2 answer: a settled world pays nothing new, because the data was
  // already arriving and the decision is a comparison against a per-slot
  // counter the CPU already keeps.
  //
  //   A resident page is freed when it has reported occTotal == 0 on
  //   kPageFreeTicks CONSECUTIVE snapshots AND its slot is not in cpuDirty.
  //
  // BOTH conjuncts are needed and each blocks a different thrash. The
  // consecutive count blocks the SAMPLING thrash (a chunk that empties and
  // refills within the readback latency). The !cpuDirty conjunct blocks the
  // CAUSAL thrash (a chunk empty right now but adjacent to activity and about
  // to be written), and it is what makes materialize/demote oscillation
  // STRUCTURALLY impossible rather than merely unlikely: cpuDirty is exactly
  // the materialization set, so a chunk cannot be eligible to free and
  // scheduled to materialize on the same tick (risk 6).
  //
  // Note `occTotal == 0` is itself STALE — it comes from the same snapshot
  // whose staleness C1 is about. That is safe HERE AND ONLY HERE, because the
  // condition is a conjunction with !cpuDirty: a chunk that became non-empty
  // since the snapshot was stamped was written by something, and anything that
  // writes it puts it in cpuDirty. The staleness of occTotal is covered by the
  // FRESHNESS of cpuDirty, not by luck — which is why cpuDirty must be the
  // conservative mirror and not the snapshot's own dirty flags.
  void ConsumeOccupancy(const std::vector<uint32_t>& occupancy, uint32_t tick);

  // Retire the free list: pages parked by ConsumeOccupancy become reusable
  // once enough ticks have passed for any in-flight eviction copy referencing
  // them to have completed (risk 5).
  void RetirePages(uint32_t tick);

  // ---- reporting ----
  uint32_t PagesInUse() const { return pagesInUse_; }
  uint32_t PagesHighWater() const { return pagesHighWater_; }
  uint32_t PoolPages() const { return poolPages_; }
  uint64_t FillsIssued() const { return fillsIssued_; }
  uint64_t PagesFreed() const { return pagesFreed_; }
  const SlotSet& CpuDirty() const { return cpuDirty_; }

  // Reset to a fully dense identity map (worldgen/LoadWorld entry, and the
  // only state --residency dense ever has).
  void ResetIdentity(const rhi::Queue& queue);
  // Every slot EMPTY, every page free — the starting state for BATCHED paged
  // worldgen (§3.5c), which then materializes one batch at a time and demotes
  // the all-air result before the next.
  void ResetAllEmpty(const rhi::Queue& queue);

 private:
  uint32_t Alloc();                       // pops the LIFO free list; may abort
  void Free(uint32_t slot);
  void MarkTableDirty(uint32_t slot);

  World* world_ = nullptr;
  uint32_t poolPages_ = kNumChunks;
  bool paged_ = false;

  std::vector<uint32_t> freePages_;       // LIFO stack of page indices
  uint32_t pagesInUse_ = 0;
  uint32_t pagesHighWater_ = 0;
  uint64_t fillsIssued_ = 0;
  uint64_t pagesFreed_ = 0;

  SlotSet cpuDirty_;
  SlotSet scratch_;                       // dilation target, reused
  SlotSet opTargets_;                     // C(N) contributor (a), this tick
  SlotSet particleChunks_;                // C(N) contributor (b), carried
  SlotSet materialized_;                  // step (4)'s result, this tick

  // The C(j) retention ring for the snapshot roll-forward (§3.2 step 2).
  // Sized to the readback ring depth x max ticks/frame = 3 x 4 = 12; when a
  // snapshot is older than the ring can cover, the tightening is SKIPPED
  // rather than applied with an incomplete superset. Sizing is a performance
  // knob, not a correctness one: an undersized ring costs extra materialized
  // pages, never a missed one.
  static constexpr size_t kCRing = 12;
  struct CEntry { uint32_t tick; std::vector<uint32_t> slots; };
  std::deque<CEntry> cRing_;

  // Table entries changed since the last flush, as (slot) — small and bounded
  // by the materialization set, so a settled world flushes nothing.
  std::vector<uint32_t> tableDirty_;
  std::vector<uint8_t> tableDirtyMark_;

  // Pages whose initialization fill is queued for the next command buffer.
  struct PendingFill { uint32_t page; uint32_t word; };
  std::vector<PendingFill> pendingFills_;

  // Consecutive snapshots reporting occTotal == 0, per slot. Maintained in the
  // loop that already walks all kNumChunks occupancy entries, so a settled
  // world does one extra uint8 increment per chunk inside a loop it was
  // already running and takes NO further action. That is the rule-2 story, and
  // it is honest: not free, but not a new scan either.
  std::vector<uint8_t> zeroStreak_;
  static constexpr uint8_t kPageFreeTicks = 8;   // ~a quarter second at 30 Hz

  // THE RETIRE QUEUE (risk 5). The existing code is safe against
  // free-then-reallocate only because SLOTS NEVER MOVE: a slot's 16 KiB is at
  // a fixed offset forever, so an eviction copy reads that offset and gets
  // that slot's data whatever happened to it. Paging breaks that assumption —
  // a chunk's physical location becomes mutable — so a page freed and
  // reallocated while an eviction copy of the OLD chunk is outstanding would
  // have the copy read the NEW chunk's data and store the wrong world.
  //
  // A freed page is therefore PARKED with the tick that freed it and only
  // becomes reusable once enough ticks have passed for any in-flight copy to
  // have completed. kRetireTicks bounds that: the eviction ring holds
  // kMaxPendingEvicts (4) batches and CompleteOldest is called when it fills,
  // so a copy cannot outlive that many drains — 16 is comfortable headroom and
  // costs only that many pages of latency in the free list.
  //
  // §4.2's sentinel fast path sidesteps most of this entirely: a sentinel slot
  // is not copied at all, so it has no in-flight reference to have.
  struct Retired { uint32_t page; uint32_t tick; };
  std::deque<Retired> retire_;
  static constexpr uint32_t kRetireTicks = 16;

  uint32_t tick_ = 0;

 public:
  // Drain the queued page-initialization fills into `enc`. MUST be called at
  // the HEAD of the command buffer, before Recorder::RecordTable begins
  // (§5.4): FillTracked declares TransferWrite on Voxels, and the first row
  // with RW(Voxels) then gets a derived TRANSFER->COMPUTE barrier. A fill
  // recorded after a dispatch that reads the page is the hazard this ordering
  // exists to prevent.
  void DrainFills(const rhi::CommandEncoder& enc);
  bool HasPendingFills() const { return !pendingFills_.empty(); }
};

