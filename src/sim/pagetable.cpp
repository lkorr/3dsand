#include "sim/pagetable.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "sim/pass_table.h"  // pass::Buf::Voxels for the tracked page fills
#include "sim/tuning.h"      // TUNE_PART_MAX_VEL, for the spawn-ring radius

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
  tableDirty_.clear();
  std::fill(tableDirtyMark_.begin(), tableDirtyMark_.end(), (uint8_t)0);
  cpuDirty_.Clear();
  particleChunks_.Clear();
  opTargets_.Clear();
  cRing_.clear();
  pendingFills_.clear();
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
  // Pages past the slot count would be free; there are none, since
  // poolPages_ <= kNumChunks in both modes.
  pagesHighWater_ = std::max(pagesHighWater_, pagesInUse_);
  queue.WriteBuffer(world_->pageTable, 0, t.data(), (uint64_t)kNumChunks * 4);
  tableDirty_.clear();
  std::fill(tableDirtyMark_.begin(), tableDirtyMark_.end(), (uint8_t)0);
  cpuDirty_.Clear();
  particleChunks_.Clear();
  opTargets_.Clear();
  cRing_.clear();
  pendingFills_.clear();
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
  // EXHAUSTION IS FATAL, in every mode (§3.8, settled by the user). The
  // reasoning is short and better than a graceful degradation: if the pool can
  // exhaust in normal play, the pool is MIS-SIZED. That is a bug in
  // kPoolPages, and the right response to a bug is to fail loudly at the
  // moment of detection, not to invent a behaviour that hides it and mutates
  // the world while doing so.
  std::fflush(stdout);
  std::fprintf(stderr,
               "\nFATAL: page pool exhausted: %u pages needed, 0 free "
               "(kPoolPages = %u, %u in use, high water %u).\n"
               "The pool is mis-sized for this scenario — raise kPoolPages in "
               "src/sim/world.h.\n"
               "See docs/PLAN_page_table.md §3.8: exhaustion is a fatal error "
               "in every mode, deliberately.\n",
               pagesInUse_ + 1, poolPages_, pagesInUse_, pagesHighWater_);
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
  t[slot] = p;
  MarkTableDirty(slot);
  return (uint64_t)p * kChunkVol * 4;
}

uint32_t PageTable::Classify(const uint32_t* words) {
  // Whole-WORD equality, never material equality (§2.3, risk 3). A UNIFORM
  // sentinel carries only 12 bits of material, so a chunk of one material
  // whose cells differ in their state nibble cannot be represented — and
  // worldgen assigns a `rnd % 3` palette variant per cell, so a stone chunk
  // almost certainly is not single-word. Commit 0 measured this: 41 chunks of
  // 32,768 are whole-word uniform, against 2,115 that are one material with
  // mixed state.
  const uint32_t w0 = words[0];
  for (uint32_t i = 1; i < kChunkVol; i++)
    if (words[i] != w0) return kNeedsPage;
  if (w0 == 0u) return kPtEmpty;
  // Only promote to UNIFORM when the word is EXACTLY what synthWord would
  // produce for it. A chunk of one word that carries a live tick stamp or a
  // stain cannot round-trip through a 12-bit sentinel, and promoting it would
  // silently rewrite those bits.
  const uint32_t mat = w0 & kPtMatMask;
  const uint32_t entry = kPtSentinelBit | mat;
  if (SynthWord(entry) != w0) return kNeedsPage;
  return entry;
}

// ---- the §3.2 recurrence --------------------------------------------------

void PageTable::BeginTick(uint32_t tick) {
  tick_ = tick;
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
  cpuDirty_.IntersectWith(snap);
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
}

void PageTable::RefilledSlot(uint32_t slot) {
  // Contributor (d): Stream::FillSlots writes dirty[0] AND dirty[1] for a
  // store-hit slot. Its target was just written by streaming so it is
  // materialized anyway, but it must still enter cpuDirty or a tightening in
  // the same tick would intersect the refilled chunk's NEIGHBOURS away, and
  // the CA frontier a stream-in creates would be invisible to the mirror.
  cpuDirty_.Add(slot);
}

void PageTable::Materialize(const rhi::Queue& queue) {
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

  // Retain C(N) for the snapshot roll-forward.
  CEntry ce;
  ce.tick = tick_;
  ce.slots.reserve(opTargets_.Size() + particleChunks_.Size());
  for (uint32_t s : opTargets_.Members()) ce.slots.push_back(s);
  for (uint32_t s : particleChunks_.Members()) ce.slots.push_back(s);
  cRing_.push_back(std::move(ce));
  while (cRing_.size() > kCRing) cRing_.pop_front();

  if (!paged_) return;  // dense: every slot is already resident

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
  materialized_.Clear();
  DilateN26(scratch_, materialized_);
  materialized_.UnionWith(opTargets_);
  materialized_.UnionWith(particleChunks_);

  if (getenv("SANDVOX_PT_DEBUG")) {
    std::printf("[pt] tick %u cpuDirty=%zu nonSent=%zu mat=%zu ops=%zu part=%zu inUse=%u\n",
                tick_, cpuDirty_.Size(), scratch_.Size(), materialized_.Size(),
                opTargets_.Size(), particleChunks_.Size(), pagesInUse_);
  }
  for (uint32_t s : materialized_.Members()) {
    const uint32_t e = t[s];
    if ((e & kPtSentinelBit) == 0u) continue;  // already resident
    const uint32_t page = Alloc();
    // A freshly allocated page must hold the synthesized content of the
    // sentinel it replaces, BEFORE the consuming dispatch. vkCmdFillBuffer
    // takes a 32-bit pattern, so EMPTY (4,096 zeros) and UNIFORM (4,096
    // synthWords) are ONE fill command each — UNIFORM costs exactly what EMPTY
    // costs, which is a small real argument for the sentinel design over
    // hardware sparse, which can only produce zeros (§3.7).
    pendingFills_.push_back({page, SynthWord(e)});
    t[s] = page;
    MarkTableDirty(s);
  }
  FlushTableWrites(queue);
}

void PageTable::ConsumeOccupancy(const std::vector<uint32_t>& occupancy,
                                 uint32_t tick) {
  if (!paged_) return;
  if (zeroStreak_.size() != kNumChunks) zeroStreak_.assign(kNumChunks, 0);
  const auto& t = world_->pageTableCpu();
  std::vector<uint32_t> candidates;
  for (uint32_t s = 0; s < kNumChunks; s++) {
    if (occupancy[s] != 0) { zeroStreak_[s] = 0; continue; }
    if (zeroStreak_[s] < 255) zeroStreak_[s]++;
    // The free DECISION iterates only slots whose counter just reached the
    // threshold, which in a settled world is ZERO slots after the first
    // quarter second and stays zero forever.
    if (zeroStreak_[s] != kPageFreeTicks) continue;
    if ((t[s] & kPtSentinelBit) != 0u) continue;   // already a sentinel
    //
    // `occTotal == 0` is NOT sufficient to demote to PT_EMPTY, and this cost a
    // debugging cycle. occupancy counts NON-AIR cells; the world hash ALSO
    // covers the STAIN layer (bits 24..30, sim_occupancy.wgsl). A chunk can be
    // entirely air and still carry stain — blood spray on a floor that then
    // erodes away, water that soaked a bank before evaporating — and demoting
    // it to PT_EMPTY makes the analytic branch report a clean chunk, silently
    // dropping hashed state. That is gotcha-save-format-drops-stain in a new
    // place, and it moved the loud scenario's hash from tick 15 onward.
    //
    // So the words decide, through the one promotion rule (§2.3). This is a
    // blocking read, but it happens only for a slot that has ALREADY reported
    // empty for kPageFreeTicks consecutive snapshots and is not in cpuDirty —
    // in a settled world, zero slots per tick.
    candidates.push_back(s);
  }

  // Second pass: confirm each candidate is REALLY empty by reading its words.
  // Kept out of the loop above so the readbacks are batched and so the common
  // case — no candidates at all, which is every tick of a settled world — costs
  // nothing beyond the increments.
  for (uint32_t s : candidates) {
    if (probeChunk_) {
      if (freeProbe_.size() != kChunkVol) freeProbe_.assign(kChunkVol, 0);
      if (!probeChunk_(s, freeProbe_.data())) continue;
      if (Classify(freeProbe_.data()) != kPtEmpty) {
        zeroStreak_[s] = 0;   // carries stain, or is not uniform: keep the page
        continue;
      }
    }
    // The second conjunct. Without it a chunk empty right now but adjacent to
    // activity would be freed and re-materialized on the very next tick —
    // and page fills are GPU commands, so an oscillating boundary means a
    // settled-looking world issuing fills forever (risk 6, a rule-2 violation
    // that presents as a perf mystery).
    if (cpuDirty_.Has(s)) { zeroStreak_[s] = kPageFreeTicks - 1; continue; }
    const uint32_t page = t[s];
    world_->pageTableCpuMutable()[s] = kPtEmpty;
    MarkTableDirty(s);
    // Parked, not pushed: see the retire-queue note in the header.
    retire_.push_back({page, tick});
    pagesInUse_--;
    pagesFreed_++;
  }
}

void PageTable::RetirePages(uint32_t tick) {
  while (!retire_.empty() && tick - retire_.front().tick >= kRetireTicks) {
    freePages_.push_back(retire_.front().page);
    retire_.pop_front();
  }
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

