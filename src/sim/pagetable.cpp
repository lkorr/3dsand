#include "sim/pagetable.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "sim/pass_table.h"  // pass::Buf::Voxels for the tracked page fills
#include "sim/rng_simd.h"    // rng::Pcg8 — the JITTER verify is 80% of Classify
#include "sim/scan.h"        // scan::AllEqualMasked — the whole-chunk predicates
#include "sim/tuning.h"      // TUNE_PART_MAX_VEL, for the spawn-ring radius

namespace {
// SANDVOX_PT_DEBUG attribution clock. Only ever read inside a getenv guard, so
// a shipped run pays nothing beyond the branch.
inline double PtNowMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

// Global scope, matching world.h and sim/world.h's World — this type is a
// peer of World, not a game-layer type.

// ---------------------------------------------------------------- SlotSet --

void SlotSet::IntersectWith(const SlotSet& other) {
  std::vector<uint32_t> keep;
  keep.reserve(members_.size());
  for (uint32_t s : members_) {
    if (other.Has(s)) keep.push_back(s);
    else bits_[s >> 6] &= ~(1ull << (s & 63));
  }
  members_.swap(keep);
}

void SlotSet::UnionWith(const SlotSet& other) {
  for (uint32_t s : other.Members()) Add(s);
}

void DilateN26(const SlotSet& in, SlotSet& out) {
  // Slot coords, not world coords, and that is correct here: the residency
  // window is toroidal, so slot-adjacency IS world-adjacency for every pair of
  // chunks that are both resident. (The colour lattice is the case where slot
  // coords are WRONG — it must be global in world coords — but this is a
  // neighbourhood question about resident memory, not a scheduling colour.)
  const int n = (int)kNChunk;
  const int m = n - 1;
  for (uint32_t s : in.Members()) {
    out.Add(s);
    const int sx = (int)(s % kNChunk);
    const int sy = (int)((s / kNChunk) % kNChunk);
    const int sz = (int)(s / (kNChunk * kNChunk));
    for (int dz = -1; dz <= 1; dz++)
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          if (!dx && !dy && !dz) continue;
          const uint32_t t =
              (uint32_t)((((sz + dz) & m) * n + ((sy + dy) & m)) * n +
                         ((sx + dx) & m));
          out.Add(t);
        }
  }
}

// -------------------------------------------------------------- PageTable --

void PageTable::Init(const rhi::Device& device, World& world) {
  world_ = &world;
  paged_ = world.residency == World::Residency::Paged;
  // SANDVOX_NO_JITTER=1 turns JITTER promotion off, which makes Classify behave
  // exactly as it did before the sentinel existed. That is the differential
  // oracle for the feature: paged-with-JITTER must hash identically to
  // paged-without, because a sentinel is a storage decision and storage cannot
  // change the world.
  if (const char* nj = getenv("SANDVOX_NO_JITTER"))
    jitterEnabled_ = !(nj[0] == '1');
  auditEnabled_ = getenv("SANDVOX_PT_AUDIT") != nullptr;
  poolPages_ = world.PoolPages();
  tableDirtyMark_.assign(kNumChunks, 0);
  ResetIdentity(device.GetQueue());
}

void PageTable::ResetAllEmpty(const rhi::Queue& queue) {
  auto& t = world_->pageTableCpuMutable();
  t.assign(kNumChunks, kPtEmpty);
  freePages_.clear();
  freePages_.reserve(poolPages_);
  // Pushed high-to-low so the LIFO pop order starts at page 0, which keeps a
  // freshly generated world's pages roughly in slot order — nicer to read in a
  // debugger and marginally friendlier to the cache. Not load-bearing: page
  // assignment is not part of the world (§3.7, review m4).
  for (uint32_t i = poolPages_; i-- > 0;) freePages_.push_back(i);
  pagesInUse_ = 0;
  queue.WriteBuffer(world_->pageTable, 0, t.data(), (uint64_t)kNumChunks * 4);
  // ZERO THE FAULT COUNTER. It is a permanently-bound atomic that nothing else
  // ever resets, and CreateBuffer does not zero — so without this it starts at
  // whatever the driver left behind (measured 134,217,728 == 2^27 on a 3060
  // Ti). Every gate asserts this counter is zero, so an unzeroed counter made
  // that assertion unreadable; combined with the reporting bug below it made
  // "pageFaults == 0" vacuous for the whole phase.
  const uint32_t faultZero[4] = {0u, 0u, 0u, 0u};
  queue.WriteBuffer(world_->pageFaults, 0, faultZero, sizeof(faultZero));
  tableDirty_.clear();
  std::fill(tableDirtyMark_.begin(), tableDirtyMark_.end(), (uint8_t)0);
  cpuDirty_.Clear();
  particleChunks_.Clear();
  fluidChunks_.Clear();
  opTargets_.Clear();
  refilled_.Clear();
  shell_.Clear();
  shellSeed_.Clear();
  shellPending_ = false;
  shellActive_ = false;
  shellLinger_ = false;
  lastSpawnTick_ = -1;
  settleAnchor_ = -1;
  cRing_.clear();
  pendingFills_.clear();
  pendingJitterFills_.clear();
  retire_.clear();
  zeroStreak_.assign(kNumChunks, 0);
}

void PageTable::ResetIdentity(const rhi::Queue& queue) {
  // Dense: page i for slot i, so the table is the identity map and translation
  // is a no-op that still executes. Paged: the same starting point, because
  // worldgen writes every slot and its post-pass compaction is what turns the
  // ~84.8% all-air chunks into PT_EMPTY sentinels (§3.5c). Starting dense and
  // demoting is what lets worldgen be a plain whole-world dispatch.
  auto& t = world_->pageTableCpuMutable();
  t.assign(kNumChunks, 0u);
  freePages_.clear();
  pagesInUse_ = 0;
  for (uint32_t i = 0; i < kNumChunks && i < poolPages_; i++) {
    t[i] = i;
    pagesInUse_++;
  }
  // Slots past the pool (paged mode) start EMPTY rather than resident.
  for (uint32_t i = poolPages_; i < kNumChunks; i++) t[i] = kPtEmpty;
  // PAGES PAST THE SLOT COUNT ARE THE RETIRE HEADROOM, and they must be seeded
  // onto the free list HERE or they are unreachable for the life of the
  // process. The identity seeding above can only hand out pages 0..kNumChunks-1
  // (one per slot), and nothing else ever invents a page index — Alloc only
  // pops what is on this list. So without this loop the headroom is allocated
  // in VRAM, counted by kPoolPages, and never usable: raising the pool would
  // be a silent no-op on every path that starts from the identity map, which
  // is worldgen and LoadWorld, i.e. all of them.
  //
  // This spot previously held a comment asserting there were no such pages.
  // That was true only while kPoolPages was capped at kNumChunks, and it is
  // exactly the kind of invariant-by-comment that survives the change which
  // invalidates it. Pushed high-to-low so the LIFO pops the lowest first,
  // matching ResetAllEmpty.
  for (uint32_t p = poolPages_; p-- > kNumChunks;) freePages_.push_back(p);
  //
  // pagesHighWater_ is deliberately NOT latched here. Identity seeding claims
  // min(kNumChunks, poolPages_) pages BY CONSTRUCTION — in paged mode a
  // transient state that batched worldgen immediately replaces via
  // ResetAllEmpty. Latching it made every high-water report read the pool
  // size regardless of real demand, which is what invalidated the first
  // kPoolPages sizing attempt. The high-water is real demand only if its sole
  // writer is Alloc().
  queue.WriteBuffer(world_->pageTable, 0, t.data(), (uint64_t)kNumChunks * 4);
  // ZERO THE FAULT COUNTER. It is a permanently-bound atomic that nothing else
  // ever resets, and CreateBuffer does not zero — so without this it starts at
  // whatever the driver left behind (measured 134,217,728 == 2^27 on a 3060
  // Ti). Every gate asserts this counter is zero, so an unzeroed counter made
  // that assertion unreadable; combined with the reporting bug below it made
  // "pageFaults == 0" vacuous for the whole phase.
  const uint32_t faultZero[4] = {0u, 0u, 0u, 0u};
  queue.WriteBuffer(world_->pageFaults, 0, faultZero, sizeof(faultZero));
  tableDirty_.clear();
  std::fill(tableDirtyMark_.begin(), tableDirtyMark_.end(), (uint8_t)0);
  cpuDirty_.Clear();
  particleChunks_.Clear();
  fluidChunks_.Clear();
  opTargets_.Clear();
  refilled_.Clear();
  shell_.Clear();
  shellSeed_.Clear();
  shellPending_ = false;
  shellActive_ = false;
  shellLinger_ = false;
  lastSpawnTick_ = -1;
  settleAnchor_ = -1;
  cRing_.clear();
  pendingFills_.clear();
  pendingJitterFills_.clear();
  retire_.clear();
  zeroStreak_.assign(kNumChunks, 0);
}

uint32_t PageTable::Alloc() {
  if (!freePages_.empty()) {
    const uint32_t p = freePages_.back();
    freePages_.pop_back();
    pagesInUse_++;
    pagesHighWater_ = std::max(pagesHighWater_, pagesInUse_);
    return p;
  }
  // LAST CHANCE BEFORE THE ABORT: hand back anything the retire queue has
  // already aged out. kPoolPages is sized so this can never be REQUIRED, but
  // it costs nothing and it removes a whole ORDERING hazard from the guarantee
  // — RetirePages runs at the END of a tick, after Materialize, so a page that
  // aged out during this tick is sitting in the queue, reusable, and simply
  // has not been drained yet. Relying on the sizing alone would make the
  // proof depend on call order as well as on arithmetic.
  //
  // It cannot free anything early: the same age test applies, so the in-flight
  // eviction-copy guarantee (risk 5) is untouched. tick_ may lag the tick
  // RetirePages is normally called with, which makes this drain strictly more
  // conservative, never less.
  DrainRetired(tick_);
  if (!freePages_.empty()) {
    const uint32_t p = freePages_.back();
    freePages_.pop_back();
    pagesInUse_++;
    pagesHighWater_ = std::max(pagesHighWater_, pagesInUse_);
    return p;
  }
  // EXHAUSTION IS FATAL, in every mode (§3.8, settled by the user). The
  // reasoning is short and better than a graceful degradation: if the pool can
  // exhaust in normal play, the pool is MIS-SIZED. That is a bug in
  // kPoolPages, and the right response to a bug is to fail loudly at the
  // moment of detection, not to invent a behaviour that hides it and mutates
  // the world while doing so.
  //
  // WHAT THIS REPORT MUST ANSWER, and did not for two crashes on 2026-08-30:
  // "0 free" is a bare count, and a bare count buys one hypothesis per
  // reproduction (CLAUDE.md rule 6). There are four distinct ways the free
  // list can empty, they call for four different fixes, and they are told
  // apart only by numbers that exist right here and nowhere else:
  //
  //   1. GENUINE DEMAND — every slot wants a real page. Then resident ==
  //      poolPages_ and the pool is honestly too small for the window.
  //   2. RETIRE STARVATION — pages freed within the last kRetireTicks are
  //      parked, so `retired` is large and `resident + retired` saturates the
  //      pool while thousands of slots are still sentinels. Fix: overprovision
  //      by the queue ceiling, or drain on demand.
  //   3. A BOOKKEEPING LEAK — pagesInUse_ disagrees with the table's own
  //      count of non-sentinel slots. Fix: the mis-accounted path.
  //   4. A LOST PAGE — a page index reachable from neither the table, nor the
  //      free list, nor the retire queue. Fix: whoever dropped it.
  //
  // So the audit below RECOUNTS from the authoritative structures rather than
  // trusting the counters, and reports the residual. AuditPages() is shared
  // with the SANDVOX_PT_AUDIT per-tick check, which is what lets cases 3 and 4
  // be caught at the tick they FIRST occur rather than thousands of ticks
  // later when the leak finally reaches the bottom of the pool.
  const PageAudit a = AuditPages();
  // The oldest retire entry's age says whether the queue is DRAINING or
  // STUCK: anything at or past kRetireTicks should already have been handed
  // back by RetirePages, so a large age means RetirePages is not running on
  // this path (or is being fed a different tick counter than the one the
  // entries were stamped with).
  const uint32_t oldestAge =
      retire_.empty() ? 0u : (tick_ - retire_.front().tick);
  std::fflush(stdout);
  std::fprintf(
      stderr,
      "\nFATAL: page pool exhausted: 1 page needed, 0 free.\n"
      "  pool           %u pages (%.1f MiB)\n"
      "  counters       inUse %u, highWater %u, freed %llu, "
      "allocs mat %llu / ovr %llu\n"
      "  free list      %zu\n"
      "  retire queue   %zu parked (oldest age %u ticks, kRetireTicks %u)\n"
      "  table recount  %u resident | sentinels: %u empty, %u uniform, "
      "%u jitter, %u other\n"
      "  page audit     %u lost, %u aliased, %u out-of-range\n"
      "  demand         materialize set %zu, cpuDirty %zu, tick %u\n",
      poolPages_, (double)poolPages_ * kChunkVol * 4.0 / (1024.0 * 1024.0),
      pagesInUse_, pagesHighWater_, (unsigned long long)pagesFreed_,
      (unsigned long long)allocsMat_, (unsigned long long)allocsOvr_,
      freePages_.size(), retire_.size(), oldestAge, kRetireTicks, a.resident,
      a.sEmpty, a.sUniform, a.sJitter, a.sOther, a.lost, a.aliased,
      a.outOfRange, materialized_.Size(), cpuDirty_.Size(), tick_);
  // The one-line verdict, so the cause does not have to be re-derived from the
  // table above every time this fires.
  if (a.lost || a.aliased || a.outOfRange)
    std::fprintf(stderr,
                 "  VERDICT: PAGES LOST/ALIASED — an allocator bug, not a "
                 "sizing problem. Raising kPoolPages only delays it.\n");
  else if (a.resident != pagesInUse_)
    std::fprintf(stderr,
                 "  VERDICT: BOOKKEEPING LEAK — pagesInUse_ (%u) disagrees "
                 "with the table's own resident count (%u).\n",
                 pagesInUse_, a.resident);
  else if (a.resident >= poolPages_)
    std::fprintf(stderr,
                 "  VERDICT: GENUINE DEMAND — every page is held by a live "
                 "slot. The pool is too small for this window.\n");
  else
    std::fprintf(stderr,
                 "  VERDICT: RETIRE STARVATION — %zu pages are parked in the "
                 "retire queue while %u slots are still sentinels. The pool "
                 "needs %zu pages of headroom over the window, not a bigger "
                 "window budget.\n",
                 retire_.size(), kNumChunks - a.resident, retire_.size());
  std::fprintf(stderr,
               "See docs/PLAN_page_table.md §3.8: exhaustion is a fatal error "
               "in every mode, deliberately.\n");
  std::fflush(stderr);
  std::abort();
}

// ONE definition of the page accounting, read by the exhaustion report above
// and by the per-tick invariant check below. Two copies of this walk would be
// exactly the "two places must agree" bug the rest of this file is careful to
// avoid, and the whole point of the check is that it is trustworthy.
//
// It recounts from the AUTHORITATIVE structures — the page table, the free
// list, the retire queue — and never from pagesInUse_, which is the counter
// under suspicion whenever this runs.
PageTable::PageAudit PageTable::AuditPages() const {
  PageAudit a;
  const auto& t = world_->pageTableCpu();
  // One byte per page: 1 = the table points at it, 2 = the free list holds it,
  // 4 = the retire queue holds it. A page with 0 is LOST; a page with more
  // than one bit set is ALIASED, which is worse than losing one — two slots
  // would be sharing 16 KiB of storage and silently overwriting each other.
  std::vector<uint8_t> owner(poolPages_, 0u);
  auto mark = [&](uint32_t page, uint8_t bit) {
    if (page >= poolPages_) { a.outOfRange++; return; }
    owner[page] |= bit;
  };
  for (uint32_t s = 0; s < kNumChunks; s++) {
    const uint32_t e = t[s];
    if ((e & kPtSentinelBit) == 0u) { a.resident++; mark(e, 1u); continue; }
    if (e == kPtEmpty) a.sEmpty++;
    else if ((e & kPtJitterBit) != 0u) a.sJitter++;
    else if ((e & kPtMatMask) != kMatAir) a.sUniform++;
    else a.sOther++;
  }
  for (uint32_t p : freePages_) mark(p, 2u);
  for (const Retired& r : retire_) mark(r.page, 4u);
  for (uint8_t o : owner) {
    if (o == 0u) a.lost++;
    else if (o != 1u && o != 2u && o != 4u) a.aliased++;
  }
  return a;
}

// SANDVOX_PT_AUDIT=1: check the page-accounting invariant every tick and abort
// on the FIRST violation.
//
// Why this exists rather than just reading the exhaustion report: a leak of a
// few pages per window shift takes thousands of ticks to reach the bottom of a
// 32,768-page pool, and by then the report describes the aftermath, not the
// cause. This fires on the tick the accounting first breaks, with the tick
// number, which is the difference between "something leaks" and "the shift at
// tick N leaks". Off by default — it is an O(kNumChunks) walk per tick.
void PageTable::AuditInvariant(const char* where) {
  if (!paged_) return;
  if (!auditEnabled_) return;
  const PageAudit a = AuditPages();
  const size_t accounted = (size_t)a.resident + freePages_.size() + retire_.size();
  const bool bad = a.lost || a.aliased || a.outOfRange ||
                   a.resident != pagesInUse_ || accounted != poolPages_;
  if (!bad) return;
  std::fflush(stdout);
  std::fprintf(stderr,
               "\nFATAL: page accounting broken at %s, tick %u.\n"
               "  resident %u (pagesInUse_ says %u), free %zu, retired %zu, "
               "sum %zu of %u\n"
               "  lost %u, aliased %u, out-of-range %u\n"
               "  sentinels: %u empty, %u uniform, %u jitter, %u other\n"
               "This is an allocator bug. A bigger kPoolPages would only "
               "postpone the exhaustion it causes.\n",
               where, tick_, a.resident, pagesInUse_, freePages_.size(),
               retire_.size(), accounted, poolPages_, a.lost, a.aliased,
               a.outOfRange, a.sEmpty, a.sUniform, a.sJitter, a.sOther);
  std::fflush(stderr);
  std::abort();
}

void PageTable::Free(uint32_t slot) {
  auto& t = world_->pageTableCpuMutable();
  const uint32_t e = t[slot];
  if ((e & kPtSentinelBit) != 0u) return;  // already a sentinel
  freePages_.push_back(e);                 // LIFO: cache locality
  pagesInUse_--;
  pagesFreed_++;
}

void PageTable::MarkTableDirty(uint32_t slot) {
  if (tableDirtyMark_[slot]) return;
  tableDirtyMark_[slot] = 1;
  tableDirty_.push_back(slot);
}

void PageTable::SetSentinel(uint32_t slot, uint32_t entry) {
  auto& t = world_->pageTableCpuMutable();
  if (t[slot] == entry) return;
  Free(slot);
  t[slot] = entry;
  MarkTableDirty(slot);
}

uint64_t PageTable::EnsurePageForOverwrite(uint32_t slot) {
  auto& t = world_->pageTableCpuMutable();
  if ((t[slot] & kPtSentinelBit) == 0u)
    return (uint64_t)t[slot] * kChunkVol * 4;
  const uint32_t p = Alloc();
  allocsOvr_++;
  t[slot] = p;
  MarkTableDirty(slot);
  return (uint64_t)p * kChunkVol * 4;
}

// The stainless-air test: material bits must be air and stain/bit31 clear;
// the state nibble and stamp byte are passenger bits on air (audited — see
// Classify and the free-path comment in ConsumeOccupancy). ONE definition,
// shared by classification and the hysteresis free probe, because two copies
// of this mask would be a "two places must agree" bug in waiting.
constexpr uint32_t kAirDemoteMask = 0xFF000FFFu;  // material | stain | bit31

uint32_t PageTable::Classify(uint32_t slot, const uint32_t* words) const {
  // EMPTY first, by STAINLESS-AIR masking rather than exact zeros: material
  // must be air and stain/bit31 clear in every cell, while the state nibble
  // (12..15) and the stamp byte (16..23) are ignored — they are audited
  // passenger bits on air. sim_mutate paints EVERY voxel with a palette
  // jitter, air included (`state = rnd % 3`, sim_mutate.wgsl:80), and the CA
  // stamps vacated cells, so an all-air chunk from any played or saved world
  // is virtually never all-zeros. Exact-zero EMPTY made the store-hit refill
  // classify every such chunk as needing a page — a whole-window load from
  // world.svd re-materialized ~half the window and exhausted the pool at game
  // startup. Soundness of ignoring those bits (hash-blind: the hash skips
  // MAT_AIR cells; behavior-inert: the only stamp reader is the acting cell's
  // no-op act, and every neighbour state-nibble read is behind a same-material
  // guard; the stamp byte is save-stripped anyway): the free-path comment in
  // ConsumeOccupancy carries the line-level citations.
  // ONE definition of the stainless-air predicate, shared with the hysteresis
  // free probe below — these were two verbatim copies of the same loop.
  if (scan::AllEqualMasked(words, kChunkVol, kAirDemoteMask, 0u))
    return kPtEmpty;
  // UNIFORM: whole-WORD equality, never material equality (§2.3, risk 3). A
  // UNIFORM sentinel carries only 12 bits of material, so a chunk of one
  // material whose cells differ in their state nibble cannot be represented —
  // and worldgen assigns a `rnd % 3` palette variant per cell, so a stone
  // chunk almost certainly is not single-word. Commit 0 measured this: 41
  // chunks of 32,768 are whole-word uniform, against 2,115 that are one
  // material with mixed state.
  const uint32_t w0 = words[0];
  if (scan::AllEqual(words + 1, kChunkVol - 1, w0)) {
    // Only promote to UNIFORM when the word is EXACTLY what synthWord would
    // produce for it. A chunk of one word that carries a live tick stamp or a
    // stain cannot round-trip through a 12-bit sentinel, and promoting it would
    // silently rewrite those bits.
    const uint32_t mat = w0 & kPtMatMask;
    const uint32_t entry = kPtSentinelBit | mat;
    if (SynthWord(entry) == w0) return entry;
    return kNeedsPage;
  }
  // ---- JITTER: one material, worldgen's per-cell palette variant ----------
  // The chunk is not single-word, which is the common case underground: every
  // solid cell carries `hash3(...) % 3` in its state nibble. If EVERY cell is
  // the same material AND its word is exactly what a JITTER sentinel would
  // synthesize at that cell's world position, the whole 16 KiB page is
  // describable by 4 bytes plus the formula.
  //
  // This is what the phase-7 measurement was pointing at: 41 chunks are
  // whole-word uniform against 2,115 that are one material with mixed state,
  // and the 2,115 are the buried bulk the player never touches.
  //
  // THE COMPARISON IS EXACT, INCLUDING THE TICK STAMP — and the stamp is the
  // subtle one, so it is spelled out here.
  //
  // A JITTER sentinel synthesizes kStampNever for every cell. sim_step's
  // "already acted this substep" gate (sim_step.wgsl:802,
  // `voxStamp(w) == stampFor(...)`) reads that stamp, so a chunk promoted while
  // ANY of its cells carried a live stamp would come back with that stamp
  // erased, and those cells would be free to act a second time in the same
  // substep. Measured: it moved the determinism hash with no other symptom.
  //
  // This is the opposite of the EMPTY rule's kAirDemoteMask, and deliberately
  // so: that mask ignores the stamp because it only ever applies to AIR cells,
  // whose act is a no-op either way. These cells are SOLID and act, so the
  // stamp is behaviour, not a passenger bit. Do not "unify" the two masks.
  //
  // Practically this costs nothing: a settled buried chunk carries
  // kStampNever everywhere, which is exactly what worldgen wrote and what
  // SynthWordAt reproduces. A chunk the CA has just swept is refused for a few
  // ticks and promotes once it settles — which is the correct behaviour, since
  // an actively simulating chunk is not one to compress.
  //
  // Needs the WORLD position of the chunk, which a slot index alone does not
  // give (the window is toroidal) — hence the world_ lookup.
  if (!jitterEnabled_) return kNeedsPage;
  const uint32_t mat = w0 & kPtMatMask;
  if (mat == kMatAir) return kNeedsPage;   // air is EMPTY's business
  const uint32_t entry = kPtSentinelBit | kPtJitterBit | mat;
  const IVec3 wc = world_->SlotToWorldChunk(slot);
  const IVec3 base{wc.x * (int)kChunk, wc.y * (int)kChunk, wc.z * (int)kChunk};
  // ROW-ORDERED, and cheap-rejecting on the material FIRST. Two changes, both
  // pure strength reduction over the flat SynthWordAt loop that was here:
  //
  //   - the material test is a mask compare, so a chunk that is not one
  //     material at all (the overwhelmingly common refusal) is rejected
  //     without ever hashing;
  //   - the surviving JITTER verification walks rows, where Pcg(y) and the z
  //     term are loop-invariant (JitterRowSeed), removing one of three PCG
  //     rounds per cell.
  //
  // The ACCEPT condition is unchanged and still exact-word: every cell must
  // equal what a JITTER sentinel synthesizes there, stamp included. See the
  // stamp note above — this loop decides a promotion, so it may not relax.
  //
  // Measured: this test ran 1,024 times per window shift under --autofly-hard
  // at ~25 us each, 36.6 s of the 130 s paged frame budget over one run — the
  // single largest CPU item after eviction's synth+RLE.
  const uint32_t wantMat = mat;
  if (!scan::AllEqualMasked(words, kChunkVol, kPtMatMask, wantMat))
    return kNeedsPage;
  const uint32_t stampBits = kStampNever << kStampShift;
  // THE HOT LOOP OF THE PAGED PATH. Measured before this pass: 6,007 ms over
  // 425,890 chunks in one --autofly-hard run (14.1 us each), of which the two
  // remaining PCG rounds per cell are ~80% — the masked scans around it are
  // ~6%. kChunk is 16, so a row is exactly two 8-lane vectors and the inner
  // loop has NO TAIL; that is the whole reason this vectorizes cleanly.
#if defined(__AVX2__)
  {
    const __m256i lane0 = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const __m256i lane8 = _mm256_setr_epi32(8, 9, 10, 11, 12, 13, 14, 15);
    const __m256i bxv = _mm256_set1_epi32(base.x);
    const __m256i x0 = _mm256_add_epi32(bxv, lane0);
    const __m256i x1 = _mm256_add_epi32(bxv, lane8);
    const __m256i vmat = _mm256_set1_epi32((int)(wantMat | stampBits));
    uint32_t i = 0;
    for (int lz = 0; lz < (int)kChunk; lz++)
      for (int ly = 0; ly < (int)kChunk; ly++, i += kChunk) {
        const uint32_t rowSeed = JitterRowSeed(base.y + ly, base.z + lz, seed_);
        const __m256i s0 = rng::JitterStateInRow8(rowSeed, x0, seed_);
        const __m256i s1 = rng::JitterStateInRow8(rowSeed, x1, seed_);
        const __m256i w0v =
            _mm256_or_si256(vmat, _mm256_slli_epi32(s0, 12));
        const __m256i w1v =
            _mm256_or_si256(vmat, _mm256_slli_epi32(s1, 12));
        const __m256i g0 = _mm256_loadu_si256((const __m256i*)(words + i));
        const __m256i g1 = _mm256_loadu_si256((const __m256i*)(words + i + 8));
        // Whole-row test. The return value is a classification, not a
        // position, so coarsening the early exit from 1 cell to 16 changes
        // nothing observable — and every load stays inside the row that was
        // going to be read anyway.
        const __m256i ne = _mm256_or_si256(_mm256_xor_si256(g0, w0v),
                                           _mm256_xor_si256(g1, w1v));
        if (!_mm256_testz_si256(ne, ne)) return kNeedsPage;
      }
    return entry;
  }
#else
  uint32_t i = 0;
  for (int lz = 0; lz < (int)kChunk; lz++)
    for (int ly = 0; ly < (int)kChunk; ly++) {
      const uint32_t rowSeed = JitterRowSeed(base.y + ly, base.z + lz, seed_);
      // Accumulate per row and test once, mirroring the vector path's
      // granularity so the two are structurally the same comparison.
      uint32_t bad = 0;
      for (int lx = 0; lx < (int)kChunk; lx++, i++) {
        const uint32_t want =
            wantMat | (JitterStateInRow(rowSeed, base.x + lx, seed_) << 12) |
            stampBits;
        bad |= words[i] ^ want;
      }
      if (bad) return kNeedsPage;
    }
  return entry;
#endif
}

// ---- the §3.2 recurrence --------------------------------------------------

void PageTable::BeginTick(uint32_t tick) {
  if (getenv("SANDVOX_PT_DEBUG") && (allocsMat_ | allocsOvr_))
    std::printf("[pt] tick %u..%u allocs: mat=%u ovr=%u refills=%u inUse=%u origin=%d,%d,%d\n",
                tick_, tick, allocsMat_, allocsOvr_, refills_, pagesInUse_,
                world_->WindowOrigin().x, world_->WindowOrigin().y,
                world_->WindowOrigin().z);
  if (settleAnchor_ < 0) settleAnchor_ = (int64_t)tick;
  tick_ = tick;
  refills_ = 0;
  allocsMat_ = 0;
  allocsOvr_ = 0;
  opTargets_.Clear();
  materialized_.Clear();
}

void PageTable::AddOpTarget(uint32_t slot) { opTargets_.Add(slot); }

void PageTable::AddOpSphere(IVec3 c, int radius, const World& world) {
  // The sphere's bounding-box CHUNKS, which over-approximates by a few chunks
  // per op. Accepted: ops are bounded (kMaxOpsPerTick = 64,
  // kMaxExplosionsPerTick = 8) and the over-approximation is freed by
  // hysteresis a few ticks later (§3.3).
  AddOpBox(c, radius, world);
}

void PageTable::AddOpBox(IVec3 c, int half, const World& world) {
  const IVec3 lo{c.x - half, c.y - half, c.z - half};
  const IVec3 hi{c.x + half, c.y + half, c.z + half};
  for (int cz = lo.z >> 4; cz <= (hi.z >> 4); cz++)
    for (int cy = lo.y >> 4; cy <= (hi.y >> 4); cy++)
      for (int cx = lo.x >> 4; cx <= (hi.x >> 4); cx++) {
        const IVec3 wc{cx, cy, cz};
        if (!world.ChunkInWindow(wc)) continue;  // out-of-window is inert
        opTargets_.Add(World::SlotChunkIndex(wc));
      }
}

void PageTable::UpdateSpawnRing(const std::vector<IVec3>& spawnCells,
                                const std::vector<IVec3>& explosionCenters,
                                const World& world) {
  // particleSpawnChunks(N) = chunks(spawnOps(N)) u chunks(explosionCenters(N)),
  //                          dilated ceil(PART_MAX_VEL / CHUNK) + 1 = 1 ring.
  //
  // RECOMPUTED FROM SCRATCH EVERY TICK from CPU-known inputs, never carried.
  // That is the whole change: the old formula tracked where a particle might
  // BE, which grows with flight time; this tracks where a particle might
  // WRITE, which is pinned to matter that already exists.
  //
  // Why one ring of the spawn sites is enough, and why nothing else is needed
  // (§3.4, the adjacency argument):
  //
  //   - A STAIN write targets a non-air cell DIRECTLY. resolve guards on
  //     `hit == MAT_AIR` and then on class (sim_particle.wgsl:229,:234), and
  //     the buried branch is behind `blocksParticle(startCell)` (:130-139). A
  //     stained cell therefore HAS MATTER by construction, so its own chunk is
  //     in (cpuDirty n hasMatter) and is materialized by the bracketed half.
  //   - A REINSERTION targets `lastAir`, which is <= 1 cell from a blocking
  //     sample because the sweep subdivides to <= half a voxel
  //     (`n = max(1, (maxc + 127) / 128)`, :153-154). The blocking sample has
  //     matter, so lastAir's chunk is within N26 of a hasMatter chunk — again
  //     the bracketed half.
  //   - The ONLY gap is the FIRST TICK OF FLIGHT, before a particle has
  //     encountered anything, and that is exactly what this one-ring spawn set
  //     covers: a particle cannot travel more than PART_MAX_VEL in the tick it
  //     is born.
  //
  // Bounded by kMaxParticleSpawnsPerTick and kMaxExplosionsPerTick,
  // INDEPENDENT OF FLIGHT DURATION. That is what makes it not grow.
  //
  // It remains contributor (b) to C(N) in §3.1a: resolve's markDirtyNext still
  // dirties the 26-neighbourhood of a GPU-decided location, and step (1)'s N26
  // of C(N) covers that.
  particleChunks_.Clear();
  if (spawnCells.empty() && explosionCenters.empty()) return;
  // Any particle-spawning input arms the flight shell (contributor (e)) until
  // a snapshot that postdates this tick reports zero live particles. Set even
  // if every spawn lands out of window — a dead-cheap over-approximation.
  lastSpawnTick_ = (int64_t)tick_;

  SlotSet seeds;
  auto seed = [&](const IVec3& c) {
    if (!world.CellInWindow(c)) return;   // out-of-window is inert
    seeds.Add(World::SlotChunkIndex({c.x >> 4, c.y >> 4, c.z >> 4}));
  };
  for (const IVec3& c : spawnCells) seed(c);
  for (const IVec3& c : explosionCenters) seed(c);

  // The ring radius is DERIVED from the live tuning rather than restated, so
  // raising TUNE_PART_MAX_VEL automatically widens it — and because TUNE_*
  // values are hot-reloadable (F5), it is recomputed here on every tick rather
  // than cached (§3.4 fallback (ii)). ceil(6/16) + 1 = 1 at the shipped value.
  const int velVox = (int)(CurrentTuning().sim.partMaxVel / 256);  // 24.8 fixed
  const int rings = (velVox + (int)kChunk - 1) / (int)kChunk + 1;
  for (int r = 0; r < rings; r++) {
    scratch_.Clear();
    DilateN26(seeds, scratch_);
    seeds.Clear();
    seeds.UnionWith(scratch_);
  }
  particleChunks_.UnionWith(seeds);
}

void PageTable::UpdateFluidChunks(const std::vector<uint32_t>& blockSlots,
                                  const std::vector<IVec3>& spawnCells,
                                  const World& world) {
  // Contributor for the MPM fluid seam, and the same shape as UpdateSpawnRing
  // for the same reason: RECOMPUTED FROM SCRATCH each call from CPU-known
  // inputs, never carried, so it is bounded by the live block count plus this
  // tick's spawns and cannot grow with time.
  //
  //   - blockSlots is the one-tick-latent block-list readback: every chunk
  //     the excite/settle converters may write voxels into. The latency is
  //     safe because the settle converter only fires after >= 8 consecutive
  //     calm ticks (fluidSettleTicks is clamped to >= 8 for exactly this), so
  //     a block that is about to write voxels has been in the readback for
  //     many ticks already.
  //   - spawnCells covers the latency's other direction — blocks born THIS
  //     tick, which no readback can know yet.
  //   - ONE N26 ring covers sub-chunk particle drift since the snapshot was
  //     stamped: a fluid particle cannot cross more than a chunk per tick.
  fluidChunks_.Clear();
  if (blockSlots.empty() && spawnCells.empty()) return;

  SlotSet seeds;
  for (uint32_t s : blockSlots)
    if (s < kNumChunks) seeds.Add(s);
  for (const IVec3& c : spawnCells) {
    if (!world.CellInWindow(c)) continue;   // out-of-window is inert
    seeds.Add(World::SlotChunkIndex({c.x >> 4, c.y >> 4, c.z >> 4}));
  }
  scratch_.Clear();
  DilateN26(seeds, scratch_);
  seeds.Clear();
  seeds.UnionWith(scratch_);
  fluidChunks_.UnionWith(seeds);
}

void PageTable::TightenFromSnapshot(const std::vector<uint8_t>& dirtyFlags,
                                    uint32_t snapTick, uint32_t encodeTick) {
  if (!paged_) return;
  if (encodeTick <= snapTick) return;
  const uint32_t gap = encodeTick - snapTick;  // M - S
  // dirtyFlags(S) == dirtyIn(S+1) EXACTLY, so rolling it forward the
  // (M-S-1) ticks since gives a SECOND superset of dirtyIn(M). Both operands
  // of the intersection are supersets, so the result is a superset and is at
  // least as tight as either — that is the whole correctness argument, and it
  // is why this is an intersection and NEVER an assignment (§3.2, review C1).
  const uint32_t rolls = gap - 1;
  // The C(j) union for j in [S+1, M-1] is MANDATORY: a CPU op issued at tick
  // S+1 marks chunks the snapshot never saw, and intersecting them away loses
  // them. If the ring cannot cover the span, SKIP the tightening entirely and
  // let step (1) carry — never tighten with an incomplete superset.
  if (rolls > kCRing) {
    if (getenv("SANDVOX_PT_DEBUG"))
      std::printf("[pt] tick %u SKIP tighten: snapshot %u is %u rolls old\n",
                  encodeTick, snapTick, rolls);
    return;
  }
  uint32_t oldest = encodeTick;
  for (const CEntry& e : cRing_) oldest = std::min(oldest, e.tick);
  if (rolls > 0 && oldest > snapTick + 1) {
    if (getenv("SANDVOX_PT_DEBUG"))
      std::printf("[pt] tick %u SKIP tighten: C ring starts at %u, need %u\n",
                  encodeTick, oldest, snapTick + 1);
    return;
  }

  SlotSet snap;
  for (uint32_t i = 0; i < kNumChunks; i++)
    if (dirtyFlags[i]) snap.Add(i);
  SlotSet rolled;
  for (uint32_t r = 0; r < rolls; r++) {
    rolled.Clear();
    DilateN26(snap, rolled);
    snap.Clear();
    snap.UnionWith(rolled);
  }
  for (const CEntry& e : cRing_)
    if (e.tick > snapTick && e.tick < encodeTick)
      for (uint32_t s : e.slots) snap.Add(s);

  const size_t before = cpuDirty_.Size();
  // ---- THE MIRROR HAS TO BE ABLE TO GROW ----------------------------------
  //
  // The intersection below is the TIGHTNESS argument and it is right: both
  // operands are supersets of dirtyIn(M), so their intersection is too. What it
  // is not is a way to LEARN. Every other contributor (opTargets, particles,
  // fluid, refills, the flight shell) is something the CPU itself caused, so a
  // mirror that starts empty and can only shrink is a superset of "writes the
  // CPU asked for" — not of "writes the GPU will make".
  //
  // Those differ whenever the GPU is running activity the CPU never declared,
  // and the loudest source of that is WORLDGEN: a generated world that is not
  // perfectly at rest wakes hundreds of chunks the CPU has no record of.
  // Measured on the `terrain` gate after the scale pass: `cpuDirty=0` on every
  // tick against a snapshot dirty set of 200-390 chunks, for the whole 120-tick
  // settle. Nothing outside worldgen's own residency was materialized, and the
  // one write that crossed into a chunk worldgen had left as a JITTER(stone)
  // sentinel — a tarn's water staining the rock behind its bank, `water`'s
  // stain rule at 260 per-mille — was dropped 58 times and never landed,
  // because a lost write leaves the chunk uniform, so it is never made
  // non-uniform, so it is never materialized. A closed loop.
  //
  // `snap` IS dirtyFlags(S) rolled forward to M and unioned with the C-ring, so
  // it is exactly the superset this needs and it is already computed. Unioning
  // it in makes the mirror what its name says. It cannot dilate without bound:
  // the next tick's tightening intersects against the next snapshot, so the
  // fixed point is (GPU-active) u N26(GPU-active) — the same shape the particle
  // flight shell already settles at, and zero when the world is asleep, which
  // is the state rule 2 keeps it in.
  cpuDirty_.IntersectWith(snap);
  cpuDirty_.UnionWith(snap);
  if (getenv("SANDVOX_PT_DEBUG"))
    std::printf("[pt] tick %u tighten from snap %u (%u rolls): %zu -> %zu "
                "(snap set %zu)\n",
                encodeTick, snapTick, rolls, before, cpuDirty_.Size(),
                snap.Size());
}

void PageTable::WakeAll() {
  // Contributor (c), applied at step (3) — strictly AFTER the tightening. A
  // union after an intersection cannot be undone by it, so the wake's 32,768
  // chunks survive regardless of when the snapshot happened to land. That is
  // why this ordering was chosen over putting the wake in C(j): it needs no
  // ordering reasoning at all, and C(j) would have to carry a 32,768-entry
  // all-ones set (§3.1a).
  cpuDirty_.SetAll();
  if (getenv("SANDVOX_PT_DEBUG"))
    std::printf("[pt] tick %u WAKE-ALL: inUse=%u highWater=%u\n", tick_,
                pagesInUse_, pagesHighWater_);
}

void PageTable::RefilledSlot(uint32_t slot) {
  refills_++;
  // Contributor (d): Stream::FillSlots writes dirty[0] AND dirty[1] for a
  // store-hit or freshly generated slot. Its target was just written by
  // streaming so it is materialized anyway, but it must still enter cpuDirty or
  // a tightening would intersect the refilled chunk (and its NEIGHBOURS) away,
  // and the CA frontier a stream-in creates would be invisible to the mirror.
  //
  // BUFFERED, NOT ADDED TO cpuDirty HERE — and this is the fix for a real page
  // fault, not a tidiness point. The old code did `cpuDirty_.Add(slot)` from
  // this function, which runs from Stream::FillSlots BETWEEN ticks, so the add
  // landed BEFORE the next SubmitTick's TightenFromSnapshot — on the wrong
  // side of step (2)'s intersection. Refilled slots are also not in the C(j)
  // ring (only opTargets and particleChunks are), so a snapshot predating the
  // refill has the slot clear in BOTH operands and tightens it straight out.
  //
  // What that cost, end to end: the slot leaves cpuDirty, so Materialize never
  // allocates it a page and it stays a sentinel — but FillSlots already wrote
  // dirty[0]/dirty[1] = 1 for it, so the CA dispatches on it anyway, voxStore
  // hits PT_NO_WORD (common.wgsl) and the write is silently DROPPED. That is
  // the streaming gate's 217 page faults. Dense never saw it (no sentinels
  // exist) and SANDVOX_NO_JITTER=1 never saw it (the chunk fails UNIFORM
  // classification and stays resident), which is exactly the differential the
  // diagnosis used.
  //
  // Materialize consumes the buffer into cpuDirty AFTER the tightening (the
  // (c)/(e) ordering rule: a union applied after an intersection cannot be
  // undone by it) and records it in the C(j) ring so a LATER tightening
  // rolling a pre-refill snapshot forward re-adds it. Both halves are needed;
  // missing either loses the slot to an intersection.
  //
  // THE CALLER DECLARES ONLY THE ACT SET, and that narrowing is what makes
  // the mirror an affordable home for this contributor at all: declaring the
  // whole shift plane (1,024 slots, ~all JITTER-demotable stone under
  // --autofly-hard, every one of it hasMatter) ran the carried, N26-dilated
  // set away to a FATAL pool exhaustion, twice, in two different homes
  // (32,148 in cpuDirty, 31,691 in the materialization set). A chunk that
  // cannot act creates no frontier, so pure sky and air-isolated full bulk
  // never enter — see the act-set note in Stream::FillSlots for the per-chunk
  // rule and its soundness.
  // Dense has no mirror to maintain — every slot is permanently resident and
  // Materialize returns before the consuming union — so buffering here would
  // grow a set nothing ever drains. cpuDirty is inert under dense by design.
  if (!paged_) return;
  refilled_.Add(slot);
}

void PageTable::ApplyParticleShell(const WorldSnapshot& snap,
                                   bool particlesActive) {
  if (!paged_) return;
  // No particle pass will be encoded this tick (particlesActive gates the
  // whole particle pipeline in EncodeTick), or nothing was ever spawned: no
  // resolve can run, so no GPU-decided write and no landing mark exist to
  // cover. The guard drops too — a stale shell must not block demotion after
  // the particle pipeline stops. This is the settled-world exit and the
  // rule-2 story: a world with no particles pays one branch here, nothing
  // else.
  if (!particlesActive || lastSpawnTick_ < 0) {
    shellActive_ = false;
    shellLinger_ = false;
    return;
  }
  // The OFF condition, and both conjuncts matter: a snapshot counting zero
  // live particles proves nothing about spawns submitted after it was
  // stamped, so it only lowers the shell when it POSTDATES the last spawn.
  //
  // The shell LINGERS one application past the off condition. The last
  // landing (tick Z) puts blood in a chunk that first shows occupancy in the
  // snapshot stamped Z — the same snapshot whose particleCount==0 turns the
  // shell off. One more application seeded from THAT snapshot is what carries
  // the landed chunk into cpuDirty; from then on it survives every tightening
  // on its own merits (genuinely dirty, so in both operands) and the ordinary
  // recurrence tracks the flow it seeds. Without the linger, the intersection
  // could never ADD it, and the flow would fault the tick the shell dropped.
  if (snap.valid && snap.particleCount == 0 &&
      (int64_t)snap.tick >= lastSpawnTick_) {
    if (!shellLinger_) {
      shellActive_ = false;
      return;
    }
    shellLinger_ = false;
  } else {
    shellLinger_ = true;
  }
  shellActive_ = true;

  // occMatter: every chunk whose latest-snapshot occupancy is non-zero.
  //
  // SEEDED FROM OCCUPANCY, NOT FROM THE PAGE TABLE, and this is load-bearing:
  // hasMatter (= any non-EMPTY entry) cannot tell a resident-but-all-air
  // chunk from one holding matter, so a residency-seeded shell FEEDS BACK on
  // itself — materializing the ring makes it resident, next tick's shell
  // rings the ring, and the set dilates one ring per tick for as long as a
  // particle flies. That is exactly the unbounded growth §3.4's amendment
  // deleted. Occupancy counts actual non-air cells, which materializing an
  // empty page cannot change, so the shell is pinned to real matter and its
  // fixed point is one ring around it.
  //
  // Blockers need matter, and occupancy sees ALL matter: occupancyFull reads
  // through voxWordAt, so UNIFORM sentinels report their synthesized count. A
  // stain-only chunk reports zero, and correctly so — stained air does not
  // block a particle. Matter created SINCE the snapshot was stamped is not
  // here, and does not need to be: op-created matter is covered by
  // N26(opTargets) (the induction base case), and landed/CA-moved matter is
  // in cpuDirty by the marks the tightening keeps, resident, and therefore
  // ringed by the bracketed half.
  //
  // The scan below is O(kNumChunks) per tick WHILE PARTICLES FLY — bounded by
  // the same activity that pays for the particle passes themselves, and zero
  // when the world settles.
  scratch_.Clear();
  if (snap.valid && snap.occupancy.size() == kNumChunks) {
    for (uint32_t s = 0; s < kNumChunks; s++)
      if (snap.occupancy[s] != 0u) scratch_.Add(s);
  } else {
    // No snapshot yet (startup, or the ring declined every slot so far):
    // fall back to the page table, which is a strict superset of occMatter (a
    // chunk holding matter is never PT_EMPTY). Wider, and it would feed back
    // as above if sustained — but it is bounded by the first snapshot's
    // arrival, after which the occupancy seed takes over.
    const auto& t = world_->pageTableCpu();
    for (uint32_t s = 0; s < kNumChunks; s++) {
      const uint32_t e = t[s];
      const bool empty =
          (e & kPtSentinelBit) != 0u && (e & kPtMatMask) == kMatAir;
      if (!empty) scratch_.Add(s);
    }
  }
  // What goes WHERE, and why the split keeps the shell at ONE ring:
  //
  //   - occMatter (the SEED, no ring) is unioned into the MIRROR — deferred
  //     to Materialize, strictly AFTER step (1)'s propagate. The bracketed
  //     half then materializes N26(occMatter) by its own rule: the seed is in
  //     (cpuDirty n hasMatter), so its 26-ring is exactly the shell. Nothing
  //     rings the ring: the tightening cuts the seed back every tick before
  //     it is re-applied, so the resident fixed point during flight is matter
  //     + ONE ring. (Earlier shapes of this fix put the ringed shell in the
  //     mirror BEFORE propagate; the three compounding dilations — shell
  //     ring, propagate ring, bracket ring — ran the fixed point to matter +
  //     three rings, past an 8,192-page pool on flung-liquid alone.)
  //   - shell_ = occMatter u N26(occMatter) is kept as the DEMOTION GUARD for
  //     ConsumeOccupancy. The ring chunks are all-air and not in cpuDirty, so
  //     without the guard hysteresis would free them mid-flight — which is
  //     the exact measured failure (the chunks under the flung-liquid slab
  //     demoted while the blood was airborne). The guard preserves §3.6's
  //     structural property: nothing in the materialization set is eligible
  //     to free.
  //
  // The mirror entry is what closes the POST-flight story: the landed chunk
  // survives every later tightening on its own merits (genuinely dirty, so in
  // both operands), and step (1)'s N26 tracks the flow frontier from then on.
  // Materializing alone would cover the landing write but leave the landed
  // chunk invisible to cpuDirty, and the CA flow the landing seeds would
  // fault the moment the shell came down.
  shellSeed_.Clear();
  shellSeed_.UnionWith(scratch_);
  shell_.Clear();
  DilateN26(scratch_, shell_);
  shellPending_ = true;
  if (getenv("SANDVOX_PT_DEBUG"))
    std::printf("[pt] tick %u flight shell: occMatter=%zu shell=%zu\n",
                tick_, scratch_.Size(), shell_.Size());
}

void PageTable::Materialize(const rhi::Queue& queue) {
  const bool matDbg = getenv("SANDVOX_PT_DEBUG") != nullptr;
  const double matT0 = matDbg ? PtNowMs() : 0.0;
  // Step (1) of the normative definitions: propagate. Done here rather than in
  // BeginTick because opTargets_/particleChunks_ (= C(N)) are only complete
  // once the caller has declared this tick's ops.
  if (paged_) {
    scratch_.Clear();
    DilateN26(cpuDirty_, scratch_);
    cpuDirty_.Clear();
    cpuDirty_.UnionWith(scratch_);
  }
  cpuDirty_.UnionWith(opTargets_);
  cpuDirty_.UnionWith(particleChunks_);
  cpuDirty_.UnionWith(fluidChunks_);

  // Retain C(N) for the snapshot roll-forward.
  CEntry ce;
  ce.tick = tick_;
  ce.slots.reserve(opTargets_.Size() + particleChunks_.Size() +
                   fluidChunks_.Size() + refilled_.Size());
  for (uint32_t s : opTargets_.Members()) ce.slots.push_back(s);
  for (uint32_t s : particleChunks_.Members()) ce.slots.push_back(s);
  for (uint32_t s : fluidChunks_.Members()) ce.slots.push_back(s);
  // Contributor (d) rides the C-ring too, and its absence here was half of
  // the 217-fault bug: a snapshot stamped BEFORE the refill has the slot
  // clear, the C(j) union is what re-adds CPU-decided contributions the
  // snapshot never saw, and refills were the one CPU-decided contribution
  // not recorded. A tightening rolling such a snapshot forward intersected
  // the refill away however it entered the mirror.
  for (uint32_t s : refilled_.Members()) ce.slots.push_back(s);
  cRing_.push_back(std::move(ce));
  while (cRing_.size() > kCRing) cRing_.pop_front();

  if (!paged_) return;  // dense: every slot is already resident

  // Contributor (e), the particle flight shell SEED, applied AFTER the
  // propagate above so it is not dilated in the tick it is computed — see the
  // fixed-point note in ApplyParticleShell. The seed reaches the bracketed
  // half below (that is the point: its 26-ring is the shell), and next tick's
  // propagate dilates whatever of it the next tightening keeps.
  if (shellPending_) {
    cpuDirty_.UnionWith(shellSeed_);
    shellPending_ = false;
  }

  // Contributor (d), the REFILLED ACT SET — same placement and same fixed-
  // point argument as the shell seed above: unioned AFTER step (1)'s
  // propagate so it is not dilated in the tick it arrives, and AFTER the
  // tightening (Materialize runs after TightenFromSnapshot by SubmitTick's
  // ordering) so the intersection cannot undo it. The bracketed half below
  // then materializes each member plus its 26-ring by its own rule — a mixed
  // refill chunk is hasMatter, and a full-under-air JITTER chunk is hasMatter
  // too (a non-air sentinel holds matter), so the ring covers every write the
  // first tick of post-stream CA can make. From the next tick the recurrence
  // owns it: the member is carried in cpuDirty, step (1) tracks its frontier,
  // and the next postdating snapshot either confirms it (genuinely dirty, in
  // both operands) or tightens it away (settled — the common case).
  //
  // What makes this affordable where the whole-plane version fatally was not:
  // the CALLER (Stream::FillSlots) narrowed the set to chunks that can ACT —
  // free surfaces and full-chunks-touching-air — before declaring them.
  // Under --autofly-hard that is ~zero of a buried plane instead of all
  // 1,024; the buried JITTER bulk never enters the mirror at all, which IS
  // the sentinel's memory win being preserved.
  if (!refilled_.Empty()) {
    cpuDirty_.UnionWith(refilled_);
    refilled_.Clear();
  }

  auto& t = world_->pageTableCpuMutable();

  // Step (4): the materialization set.
  //
  //   [ (cpuDirty n hasMatter) u N26(cpuDirty n hasMatter) ]
  //     u opTargets(N) u particleSpawnChunks(N)
  //
  // hasMatter = a resident page OR a UNIFORM(mat) sentinel with mat != AIR.
  // ONLY PT_EMPTY is excluded. That matters and is not pedantry: a UNIFORM
  // sentinel HOLDS MATTER, and blocksParticle reads through voxWordAt, so a
  // particle can legitimately come to rest against a chunk of uniform water
  // and a CA write can land against it. Excluding it would lose those writes.
  //
  // It is also a strict WIDENING of the old `n nonSentinel`, so §3.2a's
  // wake-all argument is untouched: the ~4,974 non-empty chunks ARE the
  // hasMatter set, and a wake-all still collapses to them plus a ring rather
  // than demanding 32,768 pages from an 8,192-page pool.
  //
  // The filter is sound because DIRTY != HAS MATTER: a dirty EMPTY chunk holds
  // nothing, so nothing in it can move, and the only way it can RECEIVE matter
  // is from a neighbour that has some — every such neighbour is in
  // (cpuDirty n hasMatter), whose 26-ring is materialized.
  //
  // ORDERING, and this is what closes the induction: the bracketed half is
  // evaluated against hasMatter AT ENCODE TIME FOR TICK N. A tick-N
  // reinsertion places matter that is present at N+1, and markDirtyNext marks
  // its chunk, so N+1's (cpuDirty n hasMatter) includes it.
  //
  // opTargets and particleSpawnChunks are unioned in OUTSIDE the filter: a CPU
  // op genuinely writes into isolated empty sky (a mode-0 brush paints
  // wherever it reads air), and a particle's FIRST tick of flight is before it
  // has encountered anything. Every LATER particle write is adjacent to matter
  // that already exists and is covered by the bracketed half — see the
  // adjacency argument in UpdateSpawnRing.
  scratch_.Clear();
  for (uint32_t s : cpuDirty_.Members()) {
    const uint32_t e = t[s];
    const bool empty = (e & kPtSentinelBit) != 0u && (e & kPtMatMask) == kMatAir;
    if (!empty) scratch_.Add(s);   // hasMatter
  }
  const size_t hasMatterCount = scratch_.Size();  // for the debug line below
  materialized_.Clear();
  DilateN26(scratch_, materialized_);
  // opTargets is dilated by ONE RING — N26(opChunks(N)) — and this is the
  // INDUCTION BASE CASE the ordering argument above does not cover.
  //
  // The ordering argument closes for tick N+1 onward: a write at N places
  // matter that is present at N+1, markDirtyNext marks its chunk, so N+1's
  // (cpuDirty n hasMatter) sees it. It does NOT close for the tick in which an
  // op FIRST creates matter in previously-empty sky, because the bracketed
  // half is evaluated against hasMatter AT ENCODE TIME, when those chunks are
  // still PT_EMPTY. The CA then runs in that SAME tick and moves the new
  // matter one cell — off the op chunk and into a neighbour that is in neither
  // half of the set. Measured: the loud scenario's WATER brush at (176,150,176)
  // r5 spans y=9 chunks only; water falls into the y=8 chunks within tick 8 and
  // that write was lost (one page fault, slot 11531).
  //
  // Sand does not expose it because it is painted next to existing matter, so
  // the bracketed half already covers its neighbourhood; water spreads and
  // falls on tick one.
  //
  // SOUNDNESS: the CA this tick acts only on chunks dirty at compact time =
  // previously-dirty u op-marked. Previously-dirty chunks have matter (or are
  // covered by the bracketed half and its ring). Op-marked acting cells write
  // at reach <= 1 cell (rule 1's lattice bound), so every write they can make
  // lands within N26(opChunks). One ring is therefore sufficient, and a second
  // is not needed: reach is 1, not 2.
  //
  // BOUNDED: op counts are capped (kMaxOpsPerTick = 64, kMaxExplosionsPerTick
  // = 8) and this is recomputed from CPU-known inputs every tick, never
  // carried, so it cannot grow with time. It mirrors particleSpawnChunks's
  // reviewed 1-ring treatment exactly — same shape, same reason.
  scratch_.Clear();
  DilateN26(opTargets_, scratch_);
  materialized_.UnionWith(scratch_);
  materialized_.UnionWith(particleChunks_);
  // The fluid seam's chunks join OUTSIDE the hasMatter filter: settled water
  // writes into empty-sky chunks (a block's underside is air), same argument
  // as ops.
  materialized_.UnionWith(fluidChunks_);

  if (getenv("SANDVOX_PT_DEBUG")) {
    std::printf("[pt] tick %u cpuDirty=%zu hasMatter=%zu mat=%zu ops=%zu part=%zu fluid=%zu inUse=%u aM=%u aO=%u\n",
                tick_, cpuDirty_.Size(), hasMatterCount, materialized_.Size(),
                opTargets_.Size(), particleChunks_.Size(), fluidChunks_.Size(),
                pagesInUse_, allocsMat_, allocsOvr_);
  }
  for (uint32_t s : materialized_.Members()) {
    const uint32_t e = t[s];
    if ((e & kPtSentinelBit) == 0u) continue;  // already resident
    // Re-arm the hysteresis counter: it means "consecutive empty snapshots
    // SINCE RESIDENCY BEGAN". Without this, a chunk materialized while
    // all-air (every ring chunk is) whose counter had already saturated at
    // 255 during its long sentinel life can never pass through the exact-8
    // trigger again — occupancy stays 0, so nothing resets it — and its page
    // leaks for the rest of the run. Measured: ~14,400 pages resident after
    // the streaming+spells gates, most of them exactly this.
    zeroStreak_[s] = 0;
    const uint32_t page = Alloc();
    allocsMat_++;
    // A freshly allocated page must hold the synthesized content of the
    // sentinel it replaces, BEFORE the consuming dispatch. vkCmdFillBuffer
    // takes a 32-bit pattern, so EMPTY (4,096 zeros) and UNIFORM (4,096
    // synthWords) are ONE fill command each — UNIFORM costs exactly what EMPTY
    // costs, which is a small real argument for the sentinel design over
    // hardware sparse, which can only produce zeros (§3.7).
    //
    // JITTER is the exception and the reason DrainFills has two halves: its
    // words vary per cell, so no 32-bit pattern describes them and the page is
    // filled by the `pagefill` dispatch instead. The SLOT and the ENTRY both
    // travel, because the table row is about to be overwritten with the page.
    if ((e & kPtJitterBit) != 0u) {
      pendingJitterFills_.push_back({s, e});
    } else {
      pendingFills_.push_back({page, SynthWord(e)});
    }
    t[s] = page;
    MarkTableDirty(s);
  }
  FlushTableWrites(queue);
  if (matDbg)
    std::printf("[pt-time] tick %u materialize: %.2f ms (set %zu)\n", tick_,
                PtNowMs() - matT0, materialized_.Size());
}

void PageTable::ConsumeOccupancy(const std::vector<uint32_t>& occupancy,
                                 const std::vector<uint8_t>& occStain,
                                 uint32_t tick) {
  if (!paged_) return;
  if (zeroStreak_.size() != kNumChunks) zeroStreak_.assign(kNumChunks, 0);
  const auto& t = world_->pageTableCpu();
  const bool haveStainFlags = occStain.size() == kNumChunks;
  std::vector<uint32_t> candidates;
  for (uint32_t s = 0; s < kNumChunks; s++) {
    if (occupancy[s] != 0) { zeroStreak_[s] = 0; continue; }
    if (zeroStreak_[s] < 255) zeroStreak_[s]++;
    // The free DECISION iterates only slots whose counter just reached the
    // threshold, which in a settled world is ZERO slots after the first
    // quarter second and stays zero forever.
    if (zeroStreak_[s] != kPageFreeTicks) continue;
    if ((t[s] & kPtSentinelBit) != 0u) continue;   // already a sentinel
    // THE STAIN TEST, and it is now free (packOccStain, common.wgsl).
    //
    // occupancy counts NON-AIR cells, but the world hash also covers the stain
    // layer, so `nonAir == 0` alone cannot authorize a demotion to PT_EMPTY: a
    // chunk can be entirely air and still carry stain, and demoting it drops
    // hashed state silently. Establishing that used to mean reading the
    // chunk's 16 KiB of words back with a blocking WaitIdle, PER CANDIDATE,
    // which is why this path was capped at kMaxFreeProbesPerTick = 128 slots
    // per tick — against a measured 1,270 allocations per tick under flight.
    // Reclamation could not keep up by an order of magnitude, and the 128
    // probes it did run were themselves a large part of the frame.
    //
    // sim_occupancy now folds "any cell here carries stain" into bit 31 of the
    // occupancy word, so that question is answered from the snapshot the CPU
    // already has — for the REJECT direction only, which is the next test.
    //
    // THE FLAG IS A REJECTION TEST, NEVER AN AUTHORIZATION, and the asymmetry
    // is not fussiness. The flag rides the SNAPSHOT, which lags the GPU by the
    // readback ring depth. Rejecting on stale data is conservative: a chunk
    // that carried stain as of the snapshot keeps its page, and if it has since
    // been cleaned a later snapshot frees it. ACCEPTING on stale data loses
    // voxels — a chunk stained since the snapshot still reads clean, and
    // freeing it drops hashed state. Measured directly: trusting the flag to
    // authorize a free took the streaming gate from 217 to 240 page faults.
    // The live words keep the final say, through the deferred probe below.
    if (haveStainFlags && occStain[s] != 0) { zeroStreak_[s] = 0; continue; }
    // THE PER-TICK CAP IS GONE, and that is the actual fix for the leak.
    //
    // It bounded RECLAMATION while nothing bounded ALLOCATION: measured under
    // flight, 1,270 pages allocated per tick against 128 reclaimed, so the pool
    // lost ~1,140 every tick until it either exhausted (pre-JITTER, a fatal
    // abort) or degraded paged play to a p50 of 99 ms against dense's 16 ms. A
    // reclaim path that cannot outrun its allocator is a leak with extra steps.
    //
    // What made the cap necessary was the COST of a probe, and the stain test
    // above has already removed most probes before they are queued. What is
    // left is slots that are quiet, out of cpuDirty, and stainless as of the
    // last snapshot — in a settled world, none at all.
    candidates.push_back(s);
  }

  // ---- HARVEST last tick's deferred probe, then SUBMIT this tick's ----------
  //
  // The old shape was: collect candidates, copy 2 MiB from the GPU, WAIT for
  // the map, scan the words, all in one tick. The Wait is a fence behind
  // whatever GPU work is queued (~1.6 ms/tick measured, 3.2 ms/frame at 2
  // ticks/frame under surface flight).
  //
  // Deferring the readback removes the fence: submit on tick N, harvest on
  // tick N+1 when the map has had a full frame to complete. The candidates
  // are 1 tick staler, which is immaterial against an 8-tick hysteresis.
  //
  // IDENTITY CHECK: between submit and harvest a shift can reassign the slot
  // to a different world chunk. The slot-to-worldchunk key recorded at submit
  // time is compared at harvest time; a mismatch skips the slot (conservative:
  // keeps the page, exactly like a vanished-page skip).
  const bool ptDbg = getenv("SANDVOX_PT_DEBUG") != nullptr;
  const double probeT0 = ptDbg ? PtNowMs() : 0.0;
  uint32_t probesRun = 0, probesHarvested = 0;

  // ---- HARVEST phase: process the previous tick's probe -------------------
  if (probePending_ && probeHarvest_) {
    if (freeProbe_.size() < pendingProbeSlots_.size() * (size_t)kChunkVol)
      freeProbe_.assign(pendingProbeSlots_.size() * (size_t)kChunkVol, 0);
    if (probeHarvest_(freeProbe_.data())) {
      probesHarvested = (uint32_t)pendingProbeSlots_.size();
      for (size_t ci = 0; ci < pendingProbeSlots_.size(); ci++) {
        const uint32_t s = pendingProbeSlots_[ci];
        if (ci < pendingProbeOk_.size() && !pendingProbeOk_[ci]) continue;
        if ((t[s] & kPtSentinelBit) != 0u) continue;  // already freed
        // Identity: the slot must still map to the same world chunk it did
        // when the copy was issued. A shift between submit and harvest
        // reassigns the slot; classifying the old chunk's words against the
        // new chunk's page would silently free the wrong data.
        if (ci < pendingProbeKeys_.size() &&
            World::PackChunkKey(world_->SlotToWorldChunk(s)) !=
                pendingProbeKeys_[ci])
          continue;
        const uint32_t* words = freeProbe_.data() + ci * (size_t)kChunkVol;
        // Same stainless-air predicate as Classify's EMPTY test — one
        // definition now, and the index the debug print wants comes back from
        // the same call instead of a second hand-rolled rescan.
        const size_t at = scan::FirstIndexWhereMasked(words, kChunkVol,
                                                      kAirDemoteMask, 0u);
        if (at != kChunkVol) {
          if (ptDbg) {
            std::printf("[pt] tick %u free REFUSED slot %u: word %08x at %u\n",
                        tick_, s, words[at], (uint32_t)at);
          }
          zeroStreak_[s] = 0;
          continue;
        }
        if (cpuDirty_.Has(s)) { zeroStreak_[s] = kPageFreeTicks - 1; continue; }
        if (shellActive_ && shell_.Has(s)) {
          zeroStreak_[s] = kPageFreeTicks - 1;
          continue;
        }
        const uint32_t page = t[s];
        world_->pageTableCpuMutable()[s] = kPtEmpty;
        MarkTableDirty(s);
        retire_.push_back({page, tick});
        pagesInUse_--;
        pagesFreed_++;
      }
      pendingProbeSlots_.clear();
      probePending_ = false;
    }
    // If not ready yet, keep probePending_ true — harvest again next tick.
  }

  // ---- SUBMIT phase: kick a new probe for this tick's candidates ----------
  if (probeSubmit_ && !candidates.empty() && !probePending_) {
    const size_t n = std::min(candidates.size(), kMaxFreeProbesPerTick);
    pendingProbeSlots_.assign(candidates.begin(), candidates.begin() + n);
    pendingProbeKeys_.resize(n);
    for (size_t i = 0; i < n; i++)
      pendingProbeKeys_[i] =
          World::PackChunkKey(world_->SlotToWorldChunk(pendingProbeSlots_[i]));
    pendingProbeOk_ = probeSubmit_(pendingProbeSlots_);
    probePending_ = true;
    probesRun = (uint32_t)n;
    for (size_t i = n; i < candidates.size(); i++)
      zeroStreak_[candidates[i]] = kPageFreeTicks - 1;
  }
  if (ptDbg && (probesRun || probesHarvested))
    std::printf("[pt-time] tick %u freeprobe: cands=%zu submitted=%u "
                "harvested=%u %.2f ms\n",
                tick, candidates.size(), probesRun, probesHarvested,
                PtNowMs() - probeT0);
}

void PageTable::ResetStreaks(const std::vector<uint32_t>& slots) {
  if (zeroStreak_.size() != kNumChunks) return;
  for (uint32_t s : slots) zeroStreak_[s] = 0;
}

// The drain itself, with no checks attached. Split out from RetirePages so
// Alloc's last-chance drain cannot re-enter the accounting audit MID-
// ALLOCATION: between a page leaving the free list and `t[slot]` receiving it
// the books are legitimately unbalanced for a few instructions, and a debug
// aid that aborts on that would be worse than the bug it hunts.
void PageTable::DrainRetired(uint32_t tick) {
  while (!retire_.empty() && tick - retire_.front().tick >= kRetireTicks) {
    freePages_.push_back(retire_.front().page);
    retire_.pop_front();
  }
}

void PageTable::RetirePages(uint32_t tick) {
  DrainRetired(tick);
  // THE PREMISE kPoolPages IS SIZED AGAINST, CHECKED RATHER THAN ASSUMED.
  //
  // The pool exceeds kNumChunks by exactly kPageRetireCeiling, on the argument
  // that at most kMaxFreeProbesPerTick pages are parked per tick and nothing
  // stays parked longer than kRetireTicks. That argument is a reading of three
  // constants and a call site — the same species of reasoning that produced a
  // confident and wrong "exhaustion is structurally impossible" before the
  // retire queue was accounted for. So it is asserted here, where a violation
  // is one queue away from its cause, instead of being discovered later as an
  // exhaustion abort that looks like a sizing problem all over again.
  //
  // The realistic way to break it is to make freeing burstier (raise the
  // per-tick cap, add a second producer, free outside the probe path) or to
  // drive the tick counter non-monotonically. Any of those wants the pool
  // resized to match, and this is what says so.
  if (retire_.size() > (size_t)kPageRetireCeiling) {
    std::fflush(stdout);
    std::fprintf(stderr,
                 "\nFATAL: retire queue holds %zu pages, over the %u ceiling "
                 "kPoolPages is sized against (tick %u, oldest age %u, "
                 "kRetireTicks %u, cap %zu/tick).\n"
                 "The pool's exhaustion guarantee assumed this could not "
                 "happen. Raise kPageFreeProbesPerTick / kPageRetireTicks in "
                 "world.h to match whatever now feeds this queue — kPoolPages "
                 "follows them automatically.\n",
                 retire_.size(), kPageRetireCeiling, tick,
                 retire_.empty() ? 0u : (tick - retire_.front().tick),
                 kRetireTicks, kMaxFreeProbesPerTick);
    std::fflush(stderr);
    std::abort();
  }
  // End of the tick's allocate/free/retire cycle — the one point where the
  // accounting is required to balance, so the one right place to check it.
  AuditInvariant("end of tick");
}

void PageTable::FlushTableWrites(const rhi::Queue& queue) {
  if (tableDirty_.empty()) return;
  const auto& t = world_->pageTableCpu();
  // Per-slot 4-byte writes: Class A vkCmdUpdateBuffer under the barrier
  // graph's size rule, joining the pending-upload queue and draining at the
  // head of the next command buffer in issue order — exactly like the three
  // existing per-slot streaming writes (occupancy, dirty[0], dirty[1]). Same
  // shape, same mechanism, no new machinery (§5.4 (1)).
  //
  // Sorted so contiguous runs coalesce into one write, which the
  // materialization set makes common (a dilated ring is mostly contiguous).
  std::sort(tableDirty_.begin(), tableDirty_.end());
  size_t i = 0;
  while (i < tableDirty_.size()) {
    size_t j = i + 1;
    while (j < tableDirty_.size() && tableDirty_[j] == tableDirty_[j - 1] + 1) j++;
    queue.WriteBuffer(world_->pageTable, (uint64_t)tableDirty_[i] * 4,
                      t.data() + tableDirty_[i], (j - i) * 4);
    i = j;
  }
  for (uint32_t s : tableDirty_) tableDirtyMark_[s] = 0;
  tableDirty_.clear();
}

// The JITTER half: upload this tick's (slot, entry) pairs into genList and
// return how many there are, so the caller can record the `pagefill` dispatch.
// Split from DrainFills because it needs the QUEUE (a buffer write) while the
// fill half needs only the encoder, and because the dispatch must be recorded
// through Simulation's pass table rather than as a raw command.
uint32_t PageTable::UploadJitterFills(const rhi::Queue& queue) {
  if (pendingJitterFills_.empty()) return 0;
  jitterUpload_.clear();
  jitterUpload_.reserve(pendingJitterFills_.size() * 2);
  for (const PendingJitterFill& f : pendingJitterFills_) {
    jitterUpload_.push_back(f.slot);
    jitterUpload_.push_back(f.entry);
  }
  const uint32_t count = (uint32_t)pendingJitterFills_.size();
  queue.WriteBuffer(world_->pageFillList, 0, jitterUpload_.data(),
                    jitterUpload_.size() * 4);
  jitterFillsIssued_ += count;
  pendingJitterFills_.clear();
  return count;
}

void PageTable::DrainFills(const rhi::CommandEncoder& enc) {
  if (pendingFills_.empty()) return;
  for (const PendingFill& f : pendingFills_) {
    // FillTracked (pass::Buf::Voxels + offset) is the phase-4a entry point
    // built for exactly this: an off-table GPU command whose destination
    // offset is chosen at runtime, so it cannot be a table row (a pass::Row
    // encodes offsets as literal constants). The hazard is DERIVED by the
    // tracker rather than hand-placed — FillTracked declares TransferWrite on
    // Voxels and the first row with RW(Voxels) gets a TRANSFER->COMPUTE
    // barrier automatically (§5.4 (2)).
    enc.FillTracked(pass::Buf::Voxels, world_->voxels,
                    (uint64_t)f.page * kChunkVol * 4,
                    (uint64_t)kChunkVol * 4, f.word);
    fillsIssued_++;
  }
  pendingFills_.clear();
}

