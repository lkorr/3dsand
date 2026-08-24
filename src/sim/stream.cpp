#include "sim/stream.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gpu/context.h"
#include "gpu/resources.h"
#include "sim/pagetable.h"
#include "sim/pass_table.h"  // pass::Buf::Voxels for the tracked eviction copy
#include "sim/simulation.h"

namespace {
constexpr uint64_t kChunkBytes = kChunkVol * 4;
constexpr int kHysteresis = 2;        // chunks past center before a shift
constexpr size_t kEvictBatch = 256;   // staging bound: 4 MB per readback batch
// THE RING MUST BE DEEPER THAN ONE SHIFT, or every shift blocks.
//
// A shift plane is kNChunk^2 = 1,024 slots = exactly 4 batches of kEvictBatch.
// At the old depth of 4 the ring was full the instant a shift finished
// evicting, so the next AcquireStaging - the paged demote path's, or the next
// axis's - called CompleteOldest and waited on a map submitted microseconds
// earlier. Moving diagonally shifts up to 3 axes in ONE frame (kHysteresis is
// per-axis), which is 12 batches through those 4 slots: the measured 10 fps.
//
// 16 covers three axes of shift (12) plus the demote path's own batches with
// slack, so the maps land ticks later on the harvest path in Update() as the
// design intends. Cost is bounded and paid only if actually used: staging
// buffers are pooled and allocated lazily by AcquireStaging, at 4 MiB each
// (kEvictBatch * kChunkBytes), so this is a 64 MiB ceiling on a 512 MiB pool
// budget, not a 64 MiB reservation.
constexpr size_t kMaxPendingEvicts = 16;  // in-flight batches before we block

// SANDVOX_PT_DEBUG attribution clock, matching pagetable.cpp's. Only read
// inside a getenv guard.
inline double PtNowMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch())
      .count();
}
inline bool PtDbg() {
  static const bool on = getenv("SANDVOX_PT_DEBUG") != nullptr;
  return on;
}
}  // namespace

// The persisted word is 32-bit, not 16. It was 16 while the low half was the
// only durable state, but the STAIN layer (bits 24..30) is written by a sim
// kernel, is folded into the determinism hash by sim_occupancy, and therefore
// has to survive a round trip: a 16-bit store silently dropped every stain on
// save, so a saved-and-reloaded world hashed differently from the one saved.
// That was invisible while blood was the only stainer (nothing in the selftest
// world was stained at save time) and became a hard save/load failure the
// moment worldgen ponds started wetting their banks.
//
// Only the STAMP byte (bits 16..23) is stripped, which is what the mask keeps
// out — it is per-tick scheduling scratch, not state, and is deliberately
// excluded from the hash for the same reason.
//
// `kPersistMask` now lives in stream.h: the Vulkan port's cross-backend smoke
// must reproduce this exact round-trip, and a second copy of the literal is the
// "two places that must agree" bug this repo has a checker for.

void RleEncodeChunk(const uint32_t* words, std::vector<uint32_t>& out) {
  out.clear();
  uint32_t i = 0;
  while (i < kChunkVol) {
    uint32_t w = words[i] & kPersistMask;
    uint32_t run = 1;
    while (i + run < kChunkVol && (words[i + run] & kPersistMask) == w &&
           run < 0xFFFFFFFFu)
      run++;
    out.push_back(run);
    out.push_back(w);
    i += run;
  }
}

// The RLE of a SENTINEL chunk, without ever materializing its 4,096 words.
//
// Produces byte-identical output to synthesizing the chunk with SynthWordAt and
// running RleEncodeChunk over it — that equality is the whole point, because it
// is what keeps the save format unchanged (§4.2) and the round-trip lossless.
// It is a fusion of those two loops, not a new encoding.
//
// The two sentinel shapes cost wildly different amounts, and separating them is
// most of the win:
//
//   EMPTY / UNIFORM — every cell is the same word by definition, so the RLE is
//   exactly one {kChunkVol, w} pair. The old path computed that one pair by
//   calling SynthWordAt 4,096 times and then run-comparing 4,096 words. This
//   returns it in two pushes.
//
//   JITTER — cells differ in the state nibble, so the run structure is real and
//   the walk is unavoidable. It is done in ROW order so JitterRowSeed can hoist
//   the y/z half of the hash out of the inner loop (see world.h), and the RLE
//   run is extended in the same pass rather than over a staged 16 KiB buffer.
//
// Measured motivation: eviction synthesized 1.37 M sentinel chunks over one
// --autofly-hard run — 31.6 s of synth plus 24.4 s of RLE, together 55% of the
// paged frame cost, and the large majority of those chunks are the EMPTY/
// UNIFORM shape that needs no loop at all.
void RleEncodeSentinelChunk(uint32_t entry, IVec3 wc, uint32_t seed,
                            std::vector<uint32_t>& out) {
  out.clear();
  const uint32_t mat = entry & kPtMatMask;
  if ((entry & kPtJitterBit) == 0u) {
    // One word everywhere. kPersistMask is applied for the same reason
    // RleEncodeChunk applies it: the stamp byte does not persist. SynthWord
    // already writes kStampNever, so this is a no-op in practice and is kept
    // so the two encoders cannot drift.
    out.push_back(kChunkVol);
    out.push_back(SynthWord(entry) & kPersistMask);
    return;
  }
  const int bx = wc.x * (int)kChunk, by = wc.y * (int)kChunk,
            bz = wc.z * (int)kChunk;
  const uint32_t stampBits = kStampNever << kStampShift;
  uint32_t run = 0, cur = 0;
  bool have = false;
  for (int lz = 0; lz < (int)kChunk; lz++)
    for (int ly = 0; ly < (int)kChunk; ly++) {
      const uint32_t rowSeed = JitterRowSeed(by + ly, bz + lz, seed);
      for (int lx = 0; lx < (int)kChunk; lx++) {
        const uint32_t w =
            (mat | (JitterStateInRow(rowSeed, bx + lx, seed) << 12) | stampBits) &
            kPersistMask;
        if (have && w == cur) {
          run++;
          continue;
        }
        if (have) {
          out.push_back(run);
          out.push_back(cur);
        }
        cur = w;
        run = 1;
        have = true;
      }
    }
  if (have) {
    out.push_back(run);
    out.push_back(cur);
  }
}

bool RleDecodeChunk(const uint32_t* rle, size_t pairs, uint32_t* out) {
  uint32_t i = 0;
  for (size_t p = 0; p < pairs; p++) {
    uint32_t run = rle[p * 2];
    uint32_t w = rle[p * 2 + 1];
    if (i + run > kChunkVol) return false;
    // kStampNever = "hasn't acted": everything may move on the first tick
    for (uint32_t k = 0; k < run; k++)
      out[i++] = w | (kStampNever << kStampShift);
  }
  return i == kChunkVol;
}

void Stream::Init(GpuContext* ctx, World* world, Simulation* sim, uint32_t seed) {
  ctx_ = ctx;
  world_ = world;
  sim_ = sim;
  seed_ = seed;
  // The JITTER sentinel's palette-variant formula keys on this same seed
  // (world.h's JITTER block). Pushed here, at the one point the world's seed is
  // established, so the page table cannot disagree with worldgen about which
  // world it is classifying.
  if (world_->pages) world_->pages->SetWorldSeed(seed);
  world_->SetMirrorSeed(seed);
  modified_.assign(kNumChunks, 0);
}

void Stream::OnMaterialsReloaded(const std::vector<MaterialDef>& mats) {
  // must match isRayBlocker in common.wgsl: solids, powders, opaque liquids,
  // MINUS micro-detail materials (a grass cell is mostly air, so it must not
  // stop a chunk-skipping shadow ray — see the comment there).
  blockerOf_.clear();
  for (const auto& m : mats)
    blockerOf_.push_back((m.gpu.flags & kMatFlagMicro) == 0 &&
                         (m.gpu.klass == CLASS_SOLID || m.gpu.klass == CLASS_POWDER ||
                          (m.gpu.klass == CLASS_LIQUID &&
                           (m.gpu.flags & kMatFlagOpaque) != 0)));
}

void Stream::Update(IVec3 playerChunk, uint32_t tick) {
  lastTick_ = tick;
  // harvest evictions whose readback completed since last tick (non-blocking)
  while (!pending_.empty() && pending_.front().map.Ready())
    CompleteOldest(/*discard=*/false);
  // harvest completed shift-demote batches (non-blocking; see HarvestDemotes)
  HarvestDemotes(tick);

  // sticky modified set from the latest snapshot (slot-indexed, ~2 ticks
  // latent; see the accepted-race note in stream.h)
  const WorldSnapshot& snap = world_->Snap();
  if (snap.valid) {
    for (uint32_t i = 0; i < kNumChunks; i++) modified_[i] |= snap.dirtyFlags[i];
  }

  IVec3 o = world_->WindowOrigin();
  int half = (int)kNChunk / 2;
  int d[3] = {playerChunk.x - (o.x + half), playerChunk.y - (o.y + half),
              playerChunk.z - (o.z + half)};
  // ONE AXIS PER CALL, and the axis that is furthest out of centre first.
  //
  // A shift cannot be split across frames - the whole plane must be evicted
  // and refilled before the next tick observes the new origin, which is why
  // Update is documented as a between-ticks call. What CAN be spread is the
  // number of AXES: moving diagonally puts two or three axes past kHysteresis
  // on the same frame, and shifting all of them here meant up to 3,072 slots
  // (12 eviction batches, 3 genChunk dispatches, 3 demote passes) inside one
  // frame. That is a 3x spike on top of an already expensive frame.
  //
  // Deferring the other axes costs nothing in correctness: kHysteresis is 2
  // chunks, so an axis that is 2 out stays resident and correct for another
  // 2 chunks of travel - a whole chunk of slack at any sane speed - and the
  // next frame's Update picks it up. Taking the LARGEST |d| first is what
  // keeps that true under sustained diagonal flight: the deferred axes cannot
  // starve, because each frame serves whichever has drifted furthest.
  int best = -1, bestMag = kHysteresis - 1;
  for (int a = 0; a < 3; a++) {
    const int mag = d[a] < 0 ? -d[a] : d[a];
    if (mag > bestMag) { bestMag = mag; best = a; }
  }
  if (best >= 0) ShiftAxis(best, d[best] > 0 ? 1 : -1);
}

void Stream::MarkModifiedBox(IVec3 lo, IVec3 hi) {
  for (int cz = lo.z >> 4; cz <= (hi.z >> 4); cz++)
    for (int cy = lo.y >> 4; cy <= (hi.y >> 4); cy++)
      for (int cx = lo.x >> 4; cx <= (hi.x >> 4); cx++) {
        IVec3 wc{cx, cy, cz};
        if (world_->ChunkInWindow(wc)) modified_[World::SlotChunkIndex(wc)] = 1;
      }
}

void Stream::ShiftAxis(int axis, int dir) {
  IVec3 o = world_->WindowOrigin();
  // the leaving plane's slots are the entering plane's slots (mod NCHUNK)
  int leaveCoord = dir > 0 ? (axis == 0 ? o.x : axis == 1 ? o.y : o.z)
                           : (axis == 0 ? o.x : axis == 1 ? o.y : o.z) + (int)kNChunk - 1;
  std::vector<uint32_t> slots;
  slots.reserve(kNChunk * kNChunk);
  for (int u = 0; u < (int)kNChunk; u++)
    for (int v = 0; v < (int)kNChunk; v++) {
      IVec3 wc = o;
      if (axis == 0) wc = {leaveCoord, o.y + u, o.z + v};
      else if (axis == 1) wc = {o.x + u, leaveCoord, o.z + v};
      else wc = {o.x + u, o.y + v, leaveCoord};
      slots.push_back(World::SlotChunkIndex(wc));
    }

  // Mark the plane as "evicted by THIS shift" before evicting it, so the
  // refill below can tell its own eviction (whose bytes it does not need)
  // from a genuine earlier doubled-back one (whose bytes it does). See the
  // shiftEvicted_ note in stream.h.
  if (shiftEvicted_.size() != kNumChunks) shiftEvicted_.assign(kNumChunks, 0);
  for (uint32_t s : slots) shiftEvicted_[s] = 1;

  EvictSlots(slots, /*filter=*/true);

  IVec3 no = o;
  if (axis == 0) no.x += dir;
  else if (axis == 1) no.y += dir;
  else no.z += dir;
  world_->SetWindowOrigin(no);
  FillSlots(slots);
  // Cleared per shift, not per frame: a multi-axis frame runs ShiftAxis once
  // per axis, and axis 2's fill must still be able to wait on axis 1's
  // eviction if they happen to share a slot (the planes intersect along an
  // edge). Scoping the exemption to one shift keeps that case correct.
  for (uint32_t s : slots) shiftEvicted_[s] = 0;
  shifts_++;
}

void Stream::EvictSlots(const std::vector<uint32_t>& slots, bool filter) {
  const WorldSnapshot& snap = world_->Snap();
  // Everything the completion needs is captured NOW: the slots are refilled
  // (and modified_ reset) before the readback lands.
  std::vector<std::pair<uint32_t, PendingEvict::Item>> toSave;
  toSave.reserve(slots.size());
  std::vector<uint32_t> sentRle;
  for (uint32_t s : slots) {
    bool worth = true;
    if (filter && snap.valid)
      worth = snap.occupancy[s] > 0 || modified_[s] != 0;
    if (!worth) continue;
    // all-air and never modified => procgen reproduces it; don't store
    // (only trustable with a live snapshot — flushes store everything)
    uint8_t dropIfAir = filter && snap.valid && modified_[s] == 0;

    // ---- SENTINEL SLOTS NEVER TOUCH THE GPU ------------------------------
    //
    // A sentinel slot's content IS its table entry — a kernel cannot write
    // through a sentinel (that is a counted page fault), so every legitimate
    // write path materializes first, which means the words a sentinel stands
    // for are the synth pattern, always, unconditionally. There is nothing to
    // copy and nothing to wait for.
    //
    // They used to ride the staging-batch machinery anyway, as zero-copy
    // items — and a batch of 256 all-sentinel items still acquired a 4 MiB
    // staging buffer, submitted a command buffer and eventually map-waited on
    // the GPU timeline. Under sustained flight the leaving plane is ~95%
    // sentinels, so that was ~4 pointless map-waits per shift, measured as
    // the single largest block of paged frame time (118 s of map.Wait over
    // one --autofly-hard run).
    //
    // Better still, an UNMODIFIED sentinel needs no store write at all, under
    // exactly dropIfAir's trust conditions. Whatever the store holds for this
    // chunk is already right: a store-hit slot became a sentinel only by
    // Classify's exact-word test against the loaded bytes (store == synth ==
    // content), a procgen slot has no store entry and genChunk reproduces its
    // own deterministic output on re-entry, and a chunk changed SINCE either
    // event cannot be an unmodified sentinel — the change either kept it
    // resident or is >= the demote hysteresis old, far beyond modified_'s
    // snapshot lag. Skipping the Put here is what keeps JITTER planes from
    // bloating the store with per-cell RLE they never needed.
    const uint64_t off0 = world_->PageOffsetOfSlot(s);
    if (off0 == World::kNoPage) {
      if (filter && snap.valid && modified_[s] == 0) continue;
      const IVec3 wc = world_->SlotToWorldChunk(s);
      RleEncodeSentinelChunk(world_->PageEntryOfSlot(s), wc, seed_, sentRle);
      // A modified (or unfiltered-flush) sentinel is stored synchronously:
      // pure CPU, ~4 us a chunk, and the store is current before FillSlots
      // could possibly look this chunk up again.
      // std::move: Put takes the vector BY VALUE and moves it into the region
      // map, so an lvalue here copied the whole RLE (allocate + memcpy) for
      // nothing. Moving is safe even though sentRle is a reused scratch buffer
      // — RleEncodeSentinelChunk clear()s and refills `out` on every call, so
      // the next iteration never reads the moved-from state. The trade is one
      // copy for one regrow, and the regrow is the cheaper half.
      store_.Put(wc, std::move(sentRle));
      continue;
    }
    toSave.push_back({s, {world_->SlotToWorldChunk(s), dropIfAir}});
  }

  for (size_t off = 0; off < toSave.size(); off += kEvictBatch) {
    size_t n = std::min(kEvictBatch, toSave.size() - off);
    PendingEvict p;
    p.staging = AcquireStaging();
    p.items.reserve(n);
    rhi::CommandEncoder enc = ctx_->device.CreateCommandEncoder();
    for (size_t i = 0; i < n; i++) {
      // Tracked (pass::Buf id): in this dedicated command buffer the source was
      // written only by previous submits (the head barrier covers that), but
      // declaring the read keeps every off-table voxels copy on the same
      // tracker path (barrier_graph §8) rather than special-casing this one.
      //
      // THE CPU SEAM (§2.1a): the source offset resolves through
      // PageOffsetOfSlot, never through slot * kChunkBytes. Getting this wrong
      // saves the WRONG CHUNK to disk, silently and permanently.
      //
      // §4.2's fast path for a sentinel slot is therefore not an optimization
      // but MANDATORY: there is nothing to copy, because the CPU already knows
      // the chunk's entire content from the table entry and synthesizes its
      // RLE directly (see the sentinel branch in CompleteOldest). That also
      // removes the largest single source of streaming traffic on a shift
      // plane that is mostly sky — and it drops the copy from the tracked
      // path, which is why the recorded copy count moves (§5.6).
      const uint64_t srcOff = world_->PageOffsetOfSlot(toSave[off + i].first);
      p.items.push_back(toSave[off + i].second);
      p.sentinel.push_back(srcOff == World::kNoPage
                               ? world_->PageEntryOfSlot(toSave[off + i].first)
                               : 0u);
      if (srcOff != World::kNoPage) {
        enc.CopyTracked(pass::Buf::Voxels, world_->voxels, srcOff, p.staging,
                        i * kChunkBytes, kChunkBytes);
      }
      pendingChunks_[World::PackChunkKey(toSave[off + i].second.wc)]++;
    }
    // Submit BEFORE FillSlots writes, so the copy reads the leaving plane's
    // data even though the map completes ticks later.
    //
    // THE MECHANISM, precisely (docs/vulkan_barrier_graph.md §4.3, corrected
    // when the Vulkan streaming path landed in phase 3c). It is tempting to say
    // "both are submits and submits are ordered", but that is not what carries
    // the guarantee: FillSlots does NOT necessarily submit. Its per-slot writes
    // are deferred to the next submit from any path, and when every slot hits
    // the store it issues no submit at all. What actually orders them is that
    // EvictSlots submits EAGERLY, right here, while FillSlots only ENQUEUES —
    // so the copy-out is already on the queue before the overwrite is even
    // enqueued, let alone recorded.
    //
    // The memory half of the dependency comes from the head-of-command-buffer
    // global barrier (§3.4) in whichever command buffer later drains the fill.
    ctx_->queue.Submit(enc.Finish());
    p.map = rhi::MapReadDeferred(ctx_->device, p.staging, 0, n * kChunkBytes);
    pending_.push_back(std::move(p));
  }
}

rhi::Buffer Stream::AcquireStaging() {
  if (stagingPool_.empty() && pending_.size() >= kMaxPendingEvicts)
    CompleteOldest(/*discard=*/false);  // ring full: recycle the oldest
  if (!stagingPool_.empty()) {
    rhi::Buffer b = stagingPool_.back();
    stagingPool_.pop_back();
    return b;
  }
  return CreateBuffer(ctx_->device, kEvictBatch * kChunkBytes,
                      rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                      "evictStaging");
}

void Stream::CompleteOldest(bool discard) {
  if (pending_.empty()) return;
  const bool dbg = PtDbg();
  const double t0 = dbg ? PtNowMs() : 0.0;
  PendingEvict p = std::move(pending_.front());
  pending_.pop_front();
  p.map.Wait();  // resolves the map
  const double tWait = dbg ? PtNowMs() : 0.0;
  double synthMs = 0.0, rleMs = 0.0;
  uint32_t synthChunks = 0;

  if (p.map.Succeeded()) {
    if (!discard) {
      const uint8_t* ptr = (const uint8_t*)p.map.Data();
      if (ptr) {
        // ---- BOUNCE THE BATCH OUT OF MAPPED MEMORY FIRST ------------------
        //
        // p.map.Data() is the PERSISTENTLY MAPPED staging allocation, which is
        // write-combined host memory: sequential writes are fast, and reads are
        // uncached and roughly an order of magnitude slower than RAM, with no
        // prefetching to hide it. RleEncodeChunk is the worst possible consumer
        // of that — a branchy word-at-a-time run scan, two dependent loads per
        // word, 4,096 words a chunk.
        //
        // One sequential memcpy pulls the whole batch into cached RAM at
        // streaming bandwidth and the encoder then runs on ordinary memory.
        // HarvestDemotes beside it has always done this ("one sequential copy
        // out of write-combined map memory; Classify then reads cached RAM");
        // this path simply never did, and it is the more expensive of the two —
        // measured at 4.4 ms per 256-chunk harvest under --autofly-surface,
        // ~25% of the whole run, second only to the shift fence.
        if (evictScratch_.size() < p.items.size() * kChunkVol)
          evictScratch_.resize(p.items.size() * kChunkVol);
        std::memcpy(evictScratch_.data(), ptr, p.items.size() * kChunkBytes);
        ptr = (const uint8_t*)evictScratch_.data();
        std::vector<uint32_t> rle;
        for (size_t i = 0; i < p.items.size(); i++) {
          // A sentinel slot was never copied (§4.2's mandatory fast path): the
          // CPU already knows its whole content from the table entry, so the
          // RLE is produced directly from it. RLE compresses a uniform chunk to
          // a single {4096, w} pair anyway, so a sentinel chunk and a
          // materialized uniform chunk produce BYTE-IDENTICAL RLE — which is
          // what makes the save format need no change at all (§4.2).
          //
          // A JITTER sentinel does NOT compress to one RLE pair — its cells
          // differ — so its run structure is real. Both shapes go through
          // RleEncodeSentinelChunk, which FUSES the synthesis into the run scan
          // instead of materializing 4,096 words into a staging buffer first,
          // and short-circuits the EMPTY/UNIFORM shape to two pushes. The saved
          // bytes are exactly what a materialized page would have produced —
          // the page-roundtrip gate asserts that equality against
          // SynthWordAt + RleEncodeChunk directly.
          const uint32_t e = i < p.sentinel.size() ? p.sentinel[i] : 0u;
          const double ts = dbg ? PtNowMs() : 0.0;
          if (e != 0u) {
            RleEncodeSentinelChunk(e, p.items[i].wc, seed_, rle);
            if (dbg) { synthMs += PtNowMs() - ts; synthChunks++; }
          } else {
            RleEncodeChunk((const uint32_t*)(ptr + i * kChunkBytes), rle);
            if (dbg) rleMs += PtNowMs() - ts;
          }
          bool air = rle.size() == 2 && rle[1] == 0;
          if (air && p.items[i].dropIfAir) continue;
          // Moved for the same reason as the sentinel Put above: this is the
          // site that hurts most, because a mixed SURFACE chunk is exactly the
          // one whose RLE does not compress (PLAN_surface_flight_perf.md B5).
          // Both encoders clear() their `out` first, so the scratch buffer is
          // safe to move from inside the loop.
          store_.Put(p.items[i].wc, std::move(rle));
        }
      }
    }
    p.map.Unmap();
    stagingPool_.push_back(p.staging);
  }
  // a failed map (device error) loses the batch AND retires the buffer; keep going

  for (const PendingEvict::Item& it : p.items) {
    auto pc = pendingChunks_.find(World::PackChunkKey(it.wc));
    if (pc != pendingChunks_.end() && --pc->second == 0) pendingChunks_.erase(pc);
  }
  if (dbg)
    std::printf("[pt-time] evict harvest: %zu items total %.2f ms (wait %.2f, "
                "synth %.2f/%u, rle %.2f)\n",
                p.items.size(), PtNowMs() - t0, tWait - t0, synthMs,
                synthChunks, rleMs);
}

void Stream::DrainEvictions(bool discard) {
  while (!pending_.empty()) CompleteOldest(discard);
}

void Stream::FillSlots(const std::vector<uint32_t>& slots) {
  if (world_->residency == World::Residency::Paged)
    world_->pages->ResetStreaks(slots);
  std::vector<uint32_t> data(kChunkVol);
  std::vector<uint32_t> genSlots;
  const uint32_t one = 1;
  for (uint32_t s : slots) {
    modified_[s] = 0;
    IVec3 wc = world_->SlotToWorldChunk(s);
    // The chunk's own eviction may still be in flight (player doubled back
    // within the map latency): complete it so the store lookup below sees it.
    //
    // EXCEPT when THIS shift is what evicted it. Then the store lookup does
    // not want the pending bytes at all: the slot is being refilled for a
    // DIFFERENT world chunk under the new origin, so the pending eviction's
    // data belongs to the chunk that just left and is only owed to the SAVE,
    // which the pending batch already owns and will complete asynchronously.
    // Draining here bought nothing and cost the whole frame - it fired on the
    // first slot of every shift, because EvictSlots had just inserted all
    // 1,024 of them. This is the residency-INDEPENDENT half of the shift
    // hitch: it runs identically under dense and paged.
    if (!(s < shiftEvicted_.size() && shiftEvicted_[s]))
      while (pendingChunks_.count(World::PackChunkKey(wc)))
        CompleteOldest(/*discard=*/false);
    const std::vector<uint32_t>* rle = store_.Get(wc);
    if (rle && RleDecodeChunk(rle->data(), rle->size() / 2, data.data())) {
      // ---- store-hit classification (PLAN_page_table.md §3.5d) ------------
      //
      // The CPU has the decoded 16 KiB in `data` before it uploads, so it
      // knows for FREE — it is already looping over every word below to
      // compute occ/blockers — whether the chunk is all-air or all-one-word.
      // All-air or uniform => install the sentinel and SKIP THE 16 KiB UPLOAD
      // ENTIRELY, which is a bandwidth win on top of the memory win.
      //
      // This is also the ONE place UNIFORM discovery lives (§3.6, with commit
      // 0's measurement behind it): the paths that already hold the words get
      // demotion, and the tick path does not get a GPU uniformity scan.
      const uint32_t entry = world_->residency == World::Residency::Paged
                                 ? world_->pages->Classify(s, data.data())
                                 : PageTable::kNeedsPage;
      if (entry != PageTable::kNeedsPage) {
        world_->pages->SetSentinel(s, entry);
      } else {
        // THE CPU SEAM (§2.1a): without translation this writes the decoded
        // RLE into ANOTHER chunk's page. EnsurePageForOverwrite allocates when
        // the slot is a sentinel — the same branch that classifies is the one
        // that allocates, so allocation and offset come from one place.
        const uint64_t dstOff = world_->pages->EnsurePageForOverwrite(s);
        ctx_->queue.WriteBuffer(world_->voxels, dstOff, data.data(), kChunkBytes);
      }
      world_->pages->FlushTableWrites(ctx_->queue);
      // Contributor (d) to the CPU dirty mirror (§3.1a): the two dirty writes
      // below wake this slot on the next tick, in BOTH pages, decided by
      // streaming rather than by the tick loop. Its own chunk is materialized
      // by the branch above either way, but it must still enter cpuDirty or a
      // tightening in the same tick would intersect the refilled chunk's
      // NEIGHBOURS away and the CA frontier a stream-in creates would be
      // invisible to the mirror.
      //
      // EXCEPT pure stainless sky (PT_EMPTY): nothing in it can act, so it
      // creates no frontier — the act-set rule the gen branch applies below,
      // in its cheapest form. A stained-air or unclassifiable chunk returns
      // kNeedsPage and still wakes; a full UNIFORM/JITTER store hit is rare
      // enough (doubled-back player) that it wakes conservatively rather than
      // paying the neighbour test here without the occupancy buffer in hand.
      if (entry != kPtEmpty) world_->pages->RefilledSlot(s);
      uint32_t occ = 0, blockers = 0, anyStain = 0;
      for (uint32_t w : data) {
        // OUTSIDE the air test, like sim_occupancy: a restored chunk can be
        // entirely air and still carry stain, and that chunk must NOT be
        // demotable. This path decodes REAL SAVED WORLDS, so unlike worldgen
        // it genuinely can produce stain — getting this wrong would let the
        // free path drop a stained chunk's page and lose hashed state on the
        // first reload of a world that had ever bled or been soaked.
        if ((w & kStainBits) != 0u) anyStain = 1;
        uint32_t m = w & 0xFFFu;
        if (m == 0) continue;
        occ++;
        if (m < blockerOf_.size() && blockerOf_[m]) blockers++;
      }
      // packing per common.wgsl packOccStain
      occ |= blockers << 16;
      occ |= anyStain << 31;
      ctx_->queue.WriteBuffer(world_->occupancy, (uint64_t)s * 4, &occ, 4);
      // wake once: neighbors may have changed since this chunk was saved
      ctx_->queue.WriteBuffer(world_->dirty[0], (uint64_t)s * 4, &one, 4);
      ctx_->queue.WriteBuffer(world_->dirty[1], (uint64_t)s * 4, &one, 4);
      // This is the ONE waking path that writes dirtyIn without going through
      // a Simulation::Encode* entry point, so it declares itself to the §3.4
      // settled-skip latch here. Without it a store-hit refill into a settled
      // world would wake chunks the CA had been skipped for, and the refilled
      // terrain would sit frozen until something else happened to dirty the
      // world — which is exactly the silent-state-loss failure the latch's
      // conservative direction exists to prevent.
      sim_->NoteWakeAll();
    } else {
      genSlots.push_back(s);
    }
  }
  if (!genSlots.empty()) {
    // genChunk overwrites the WHOLE chunk of every slot in the list, so every
    // one of them needs a page before the dispatch — a kernel cannot allocate
    // (§3.5c at batch size = the genList count, which the shift plane already
    // bounds). No fill is queued: the kernel is about to write all 4,096 words.
    if (world_->residency == World::Residency::Paged) {
      for (uint32_t gs : genSlots) world_->pages->EnsurePageForOverwrite(gs);
      world_->pages->FlushTableWrites(ctx_->queue);
      // Contributor (d) — the RefilledSlot calls — moved to AFTER the demote
      // pass below, where the post-genChunk occupancy is in hand: only the
      // slots that can ACT are declared, not the whole plane. See the act-set
      // note at that site.
    }
    ctx_->queue.WriteBuffer(world_->genList, 0, genSlots.data(),
                            genSlots.size() * 4);
    TickParams tp{};
    tp.seed = seed_;
    tp.genCount = (uint32_t)genSlots.size();
    IVec3 o = world_->WindowOrigin();
    tp.origin[0] = o.x; tp.origin[1] = o.y; tp.origin[2] = o.z;
    ctx_->queue.WriteBuffer(world_->tickUBO, 0, &tp, sizeof(tp));

    // ---- DEBUG-ONLY fence decomposition (SANDVOX_PT_DEBUG) ---------------
    // The occupancy map below waits on the fence of the LAST submit, which
    // covers EVERY command buffer queued so far — the previous tick's
    // SubmitTick, the frame's render, this shift's eviction copies — not just
    // genChunk. So "occ 30 ms" does not say whether the GPU is busy with work
    // that already existed or with the shift's own worldgen, and those two
    // readings call for opposite fixes. Draining the backlog here first splits
    // it: `pre` is what was already outstanding, `occ` is then genChunk + its
    // copy alone. Gated because the drain is itself a stall.
    double preMs = 0.0;
    if (world_->residency == World::Residency::Paged && PtDbg()) {
      if (!genOccStaging_)
        genOccStaging_ = CreateBuffer(
            ctx_->device, (uint64_t)kNumChunks * 4,
            rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "genOcc");
      rhi::MapTicket pre =
          rhi::MapReadDeferred(ctx_->device, genOccStaging_, 0, 4);
      const double p0 = PtNowMs();
      pre.Wait();
      preMs = PtNowMs() - p0;
      pre.Unmap();
    }

    rhi::CommandEncoder enc = ctx_->device.CreateCommandEncoder();
    sim_->EncodeGenList(enc, (uint32_t)genSlots.size());
    ctx_->queue.Submit(enc.Finish());

    // ---- and DEMOTE the result (§3.5c's compaction, on the streaming path) --
    //
    // Without this the shift plane LEAKS: genChunk needs a page for every slot
    // it writes, but ~85% of a shift plane generates as pure sky, and a page
    // that is never demoted is never freed either — §3.6's free condition only
    // fires for slots reporting occTotal == 0 on kPageFreeTicks CONSECUTIVE
    // snapshots, and a slot that scrolled out stops being reported at all.
    // Measured before this landed: ~880 pages leaked per window shift
    // (5832 -> 6711 -> 7584 over three shifts of the loud scenario), which
    // exhausted the pool while the materialization set itself sat flat at
    // ~1,200. It is the same classification the store-hit branch does and that
    // batched worldgen does; this was the one path missing it.
    //
    // CLASSIFY ON THE WORDS, never on `occupancy`. Two reasons, both learned
    // the hard way here:
    //   - occupancy counts NON-AIR cells, but the hash also covers the STAIN
    //     layer (bits 24..30, sim_occupancy.wgsl). A chunk can be all-air and
    //     still carry stain, and demoting it to PT_EMPTY would silently drop
    //     hashed state — which is exactly gotcha-save-format-drops-stain in a
    //     new place.
    //   - PageTable::Classify is the ONE promotion rule (whole-word equality,
    //     §2.3), so using it here keeps a single definition rather than a
    //     second predicate that must agree with it.
    //
    // The read must not start before genChunk's submit completes: reading early
    // returns the PREVIOUS contents — for a freshly scrolled-in slot, zeros, so
    // every generated chunk would be demoted and its matter lost. The batch's
    // own copy is submitted after genChunk's on the same queue, so queue order
    // carries that dependency; the only block is the single map at the end.
    //
    // ONE COPY PER BATCH, NOT ONE PER SLOT. This loop is the whole cost of a
    // window shift. Per-slot rhi::ReadbackBlocking creates a buffer, submits a
    // command buffer, waits the queue idle and maps — once per chunk — and a
    // shift plane is kNChunk^2 = 1,024 chunks: measured at 92 ms of readback
    // plus a 14 ms WaitIdle per shift, ~105 ms total, against 0.5 ms for the
    // same shift under dense residency. Sprint-flying shifts on consecutive
    // frames, so that stall landed on nearly every frame and pinned the game at
    // ~7 fps while moving. Batching into kEvictBatch-sized copies (the bound
    // the eviction path already uses for exactly this reason, 4 MB of staging)
    // makes it one submit and one map per 256 chunks.
    if (world_->residency == World::Residency::Paged) {
      const bool dbg = PtDbg();
      const double dT0 = dbg ? PtNowMs() : 0.0;
      double occMs = 0.0;

      // ---- PREFILTER ON OCCUPANCY, so the voxel read is sized to the ANSWER --
      //
      // genChunk computes each chunk's occupancy in-kernel and writes it in the
      // same dispatch (worldgen.wgsl), so the demote candidates are known from a
      // 128 KiB buffer instead of 16 MiB of voxels.
      //
      // TWO occupancy values can demote, not one, and the second is the whole
      // point of the JITTER sentinel (world.h's JITTER block):
      //   occ == 0          all air         -> PT_EMPTY
      //   occ == CHUNK_VOL  completely full -> UNIFORM or JITTER
      // The original form of this prefilter tested `occ == 0` only, on the
      // reasoning that "a UNIFORM non-air chunk is not something worldgen
      // produces, because a solid-stone chunk is uniform in MATERIAL but its
      // state nibble carries per-cell palette jitter". That reasoning was exactly
      // right and is exactly what JITTER now represents — so the chunks it
      // excluded are the ones worth compressing. A partially-full chunk still
      // cannot demote: no sentinel form can describe a mix of air and matter.
      //
      // The stain caveat that makes `occ == 0` unsafe on the tick path does NOT
      // apply here: worldgen writes no stain bits at all, so a freshly
      // generated all-air chunk carries no hashed state. The words are still
      // read and Classify still decides — this only narrows WHICH chunks are
      // read, never what the rule is (PageTable::Classify stays the one
      // promotion rule, §2.3). Measured on a shift plane: ~1,024 candidates
      // down to the ~350 that actually demote.
      // A failed read assigns all zeros, which is the CONSERVATIVE direction
      // here and only here: a zeroed entry fails the `nonAir != 0` test below,
      // so every slot falls through into `paged` and gets its words read. The
      // prefilter degrades to "test everything", never to "demote everything".
      // ONE POOLED STAGING BUFFER, not a fresh allocation per shift.
      // rhi::ReadbackBlocking creates a buffer, submits, waits the whole queue
      // idle and maps, every single shift — and shifts land on consecutive
      // frames under flight. Keeping the 128 KiB buffer alive across shifts
      // removes the create/destroy; the copy+map is still queue-ordered behind
      // genChunk's submit, which is the dependency that matters (reading early
      // returns the PREVIOUS contents and would demote every generated chunk).
      // ---- WHY THIS WAIT IS STILL HERE (PLAN_surface_flight_perf.md B2) ----
      // Measured under --autofly-surface with SANDVOX_PT_DEBUG=1, this is THE
      // remaining cost of the surface band and it is not close:
      //
      //   [pt-time] shift demote: gen=1024 cands=815 total 39.78 ms (occ 39.55)
      //
      // 39.55 of 39.78 ms is this map wait, against materialize 0.3-0.4 ms,
      // freeprobe 1-6 ms and evict harvest 4-6 ms. It is a fence behind a
      // saturated queue on a band where the CA has real work, so it is
      // absorbing that work rather than adding its own.
      //
      // The obvious fix -- defer the wait a shift and filter on last shift's
      // occupancy -- was analysed and is UNSAFE, for a reason worth writing
      // down because the buffer has TWO consumers with OPPOSITE staleness
      // requirements:
      //
      //   * the DEMOTE prefilter below (the `paged` loop) is stale-TOLERANT.
      //     It only picks CANDIDATES; HarvestDemotes re-verifies every one
      //     from the actually-copied words (identity via PackChunkKey, still
      //     paged, kDemoteFreshTicks, CpuDirty, then Classify). A stale occ
      //     can only add a candidate Classify then rejects, or omit one that
      //     demotes a shift later. This is the same stale-filter/exact-verify
      //     shape as the batched free probe in ConsumeOccupancy.
      //
      //   * the ACT SET wake above it is stale-FATAL, and has NO downstream
      //     verification at all. `nonAir == 0u -> continue` SKIPS
      //     RefilledSlot, so one chunk that stale data calls "pure sky" while
      //     it holds matter is a chunk that never wakes -- silent voxel loss,
      //     which is precisely the 217-page-fault bug. The two conservative
      //     directions are opposites (zeros mean "test everything" for demote
      //     and "wake nothing" for the act set), so there is no single stale
      //     read that is safe for both.
      //
      // Waking the whole plane instead, so the act set needs no occupancy, is
      // the other exit and it is closed too: that is the blanket form the
      // comment below describes, measured TWICE as fatal pool exhaustion.
      //
      // So B2 does not reduce to "make it async". It needs the act set to get
      // fresh occupancy by a route that is not a CPU fence -- computing the
      // act set on the GPU beside genChunk and reading back only the demote
      // filter, most likely. That is a real design change, not a deferral,
      // and it is left undone deliberately rather than shipped racy.
      std::vector<uint32_t>& occ = genOccScratch_;
      if (occ.size() != kNumChunks) occ.assign(kNumChunks, 0);
      bool occValid = false;
      const double occT0 = dbg ? PtNowMs() : 0.0;
      if (!genOccStaging_)
        genOccStaging_ = CreateBuffer(
            ctx_->device, (uint64_t)kNumChunks * 4,
            rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "genOcc");
      {
        rhi::CommandEncoder oenc = ctx_->device.CreateCommandEncoder();
        oenc.CopyTracked(pass::Buf::Occupancy, world_->occupancy, 0,
                         genOccStaging_, 0, (uint64_t)kNumChunks * 4);
        ctx_->queue.Submit(oenc.Finish());
        rhi::MapTicket omap = rhi::MapReadDeferred(ctx_->device, genOccStaging_,
                                                   0, (uint64_t)kNumChunks * 4);
        omap.Wait();
        occValid = omap.Succeeded() && omap.Data();
        if (occValid)
          std::memcpy(occ.data(), omap.Data(), (size_t)kNumChunks * 4);
        else
          std::fill(occ.begin(), occ.end(), 0u);  // failed: fall back to all
        omap.Unmap();
      }
      if (dbg) occMs = PtNowMs() - occT0;

      // ---- contributor (d), the ACT SET: wake only what can act -----------
      //
      // The blanket form of this — RefilledSlot for every slot of the plane —
      // is what ran the mirror away: under --autofly-hard the plane is ~all
      // JITTER-demotable stone, every slot of it hasMatter, so the whole
      // plane plus its 26-ring (~3,072 chunks) materialized every shift on
      // consecutive frames, against a free path on an 8-snapshot hysteresis.
      // Measured twice as a FATAL pool exhaustion (32,148 and 31,691 of
      // 32,768). Filtering by hasMatter cannot help: a buried stone chunk IS
      // matter. The right question is not "does it hold matter" but "can any
      // cell in it ACT" — and with the post-genChunk occupancy in hand the
      // CPU can answer it per chunk:
      //
      //   - pure sky (nonAir == 0): nothing in it can move, and matter can
      //     only ARRIVE from an acting neighbour, which is covered by that
      //     neighbour's own ring (the DIRTY != HAS MATTER argument of
      //     Materialize's bracketed half, verbatim);
      //   - mixed (0 < nonAir < CHUNK_VOL): a free surface. Wakes.
      //   - full (nonAir == CHUNK_VOL): cells can act only toward air, and a
      //     CA write reaches <= 1 cell (rule 1), so a full chunk with NO air
      //     anywhere in its 26-neighbourhood is inert — that is the buried
      //     bulk, and it is exactly the JITTER win being protected here. Air
      //     in any neighbour (a cave wall, the surface, a full sand column
      //     under sky) wakes it. Out-of-window neighbours are inert by
      //     definition (not simulated). Full-vs-full liquid gradients need no
      //     wake of their own: the acting side writes the passive side's
      //     boundary cell, whose chunk is in the actor's ring, and from then
      //     on the written chunk is genuinely dirty and the recurrence
      //     tracks it.
      //
      // A failed occupancy read flips the conservative direction here: for
      // DEMOTION zeros are safe (test everything), for WAKING they would be
      // silent voxel loss (wake nothing). So a failed read wakes the whole
      // plane — the pre-act-set behaviour, degraded not broken.
      for (uint32_t gs : genSlots) {
        bool wake = true;
        if (occValid) {
          const uint32_t nonAir = occ[gs] & 0xFFFFu;
          if (nonAir == 0u) continue;         // pure sky cannot act
          wake = nonAir != kChunkVol;         // mixed: free surface
          if (!wake) {
            const IVec3 wc = world_->SlotToWorldChunk(gs);
            for (int dz = -1; dz <= 1 && !wake; dz++)
              for (int dy = -1; dy <= 1 && !wake; dy++)
                for (int dx = -1; dx <= 1 && !wake; dx++) {
                  if (!dx && !dy && !dz) continue;
                  const IVec3 nc{wc.x + dx, wc.y + dy, wc.z + dz};
                  if (!world_->ChunkInWindow(nc)) continue;
                  if ((occ[World::SlotChunkIndex(nc)] & 0xFFFFu) != kChunkVol)
                    wake = true;
                }
          }
        }
        if (wake) world_->pages->RefilledSlot(gs);
      }

      // Collect the candidates: a sentinel slot has nothing to read and is
      // already in its demoted form; a PARTIALLY-full slot cannot demote.
      // ---- ALL-AIR DEMOTES WITHOUT READING ANY WORDS ----------------------
      //
      // The occupancy word now carries "this chunk has stain" in bit 31
      // (packOccStain), which was the only thing `nonAir == 0` could not
      // establish on its own. An all-air, stainless chunk is by definition
      // PT_EMPTY's content, so it can be demoted straight from the occupancy
      // read we already did — no voxel copy, no map, no wait.
      //
      // That matters because this is the COMMON case by a wide margin: a shift
      // plane is mostly sky, and every one of those slots was allocated a page
      // by EnsurePageForOverwrite just above (genChunk cannot allocate), so
      // without this they all round-trip allocate -> fill -> read back 16 KiB
      // -> demote, every shift. Measured on the adversarial descent: ovr=1,024
      // allocations per tick, the entire plane.
      //
      // The FULL case (nonAir == CHUNK_VOL -> UNIFORM or JITTER) still needs
      // the words: those sentinels must reproduce the resident content
      // bit-exactly, which is Classify's exact-word rule and not something an
      // occupancy count can decide.
      // WHY THIS IS STILL A READBACK, having just added a stain bit that looks
      // like it should remove one.
      //
      // The tempting move is to demote `nonAir == 0 && !anyStain` straight from
      // the occupancy word, skipping the voxel copy for the ~85% of a shift
      // plane that generates as sky. It was tried and it LOSES VOXELS: the
      // streaming gate went 217 -> 240 page faults. Occupancy answers "how many
      // non-air cells" and now "any stain", but Classify's demote test is
      // kAirDemoteMask, which ALSO covers bit 31 (kCellOpIfAir) — a transient
      // "CPU write in flight" flag that occupancy does not and cannot report.
      // A chunk with a pending op reads as empty by count and is not empty.
      //
      // The occupancy prefilter therefore stays what it was: a way to narrow
      // WHICH chunks are read, never a substitute for reading them. The words
      // keep deciding, through the one promotion rule (§2.3).
      //
      // The stain bit still pays for itself where it CAN be trusted, on the
      // tick path in PageTable::ConsumeOccupancy: that path already required
      // the slot to be out of cpuDirty and quiet for kPageFreeTicks, which is
      // exactly the condition an in-flight op violates.
      std::vector<uint32_t> paged;
      paged.reserve(genSlots.size());
      for (uint32_t gs : genSlots) {
        if (world_->PageOffsetOfSlot(gs) == World::kNoPage) continue;
        const uint32_t nonAir = occ[gs] & 0xFFFFu;  // low 16 = non-air count
        if (nonAir != 0u && nonAir != kChunkVol) continue;
        paged.push_back(gs);
      }

      // ---- ISSUE the copies now, CLASSIFY on a later frame's harvest -------
      //
      // The copies must be encoded here — queue order behind genChunk's submit
      // is what guarantees they read post-gen data — but nothing about the
      // DECISION is urgent: with residency at ~1.2k pages of a 32,768-page
      // pool, a chunk that stays resident a few frames longer costs pages the
      // pool has thirty-fold spare, while the map.Wait (5.5 s/run) and the
      // JITTER word-verify (29.6 s/run) were the shift frame's two largest
      // remaining stalls after the eviction fix. HarvestDemotes applies the
      // staleness and identity guards at classify time.
      std::vector<uint64_t> keys;
      keys.reserve(paged.size());
      for (uint32_t gs : paged)
        keys.push_back(World::PackChunkKey(world_->SlotToWorldChunk(gs)));
      IssueDemoteCopies(paged, keys, lastTick_);
      if (dbg)
        std::printf("[pt-time] shift demote: gen=%zu cands=%zu issued "
                    "total %.2f ms (pre %.2f, occ %.2f)\n",
                    genSlots.size(), paged.size(), PtNowMs() - dT0, preMs, occMs);
    }
  }
}

void Stream::DiscardDemotes() {
  for (PendingDemote& d : demotes_) {
    d.map.Wait();
    d.map.Unmap();
    stagingPool_.push_back(d.staging);
  }
  demotes_.clear();
}

void Stream::IssueDemoteCopies(const std::vector<uint32_t>& slots,
                               const std::vector<uint64_t>& keys,
                               uint32_t tick) {
  // Backstop, not a throughput knob: 32 in-flight batches is 128 MiB of
  // staging and far beyond the ~4-8 a saturated flight keeps queued. Hitting
  // it means the GPU is pathologically behind; forcing the oldest batch
  // through (Wait + harvest) is the bounded-memory answer, and the harvest's
  // own retry logic keeps correctness.
  constexpr size_t kMaxPendingDemotes = 32;
  while (demotes_.size() >= kMaxPendingDemotes) {
    demotes_.front().map.Wait();
    HarvestDemotes(tick);
  }
  for (size_t off = 0; off < slots.size(); off += kEvictBatch) {
    const size_t n = std::min(kEvictBatch, slots.size() - off);
    PendingDemote d;
    d.staging = AcquireStaging();
    d.copyTick = tick;
    d.slots.reserve(n);
    d.keys.reserve(n);
    d.copied.reserve(n);
    rhi::CommandEncoder enc = ctx_->device.CreateCommandEncoder();
    for (size_t i = 0; i < n; i++) {
      const uint32_t s = slots[off + i];
      // A re-issued slot may have been demoted or evicted since it was first
      // queued: no source page, no copy. The `copied` flag is what stops the
      // harvest from ever classifying the stale staging bytes at that index —
      // a garbage match against the exact-word rule is astronomically unlikely
      // and still not a risk worth carrying on a hash-critical path.
      const uint64_t srcOff = world_->PageOffsetOfSlot(s);
      d.slots.push_back(s);
      d.keys.push_back(keys[off + i]);
      d.copied.push_back(srcOff != World::kNoPage);
      if (srcOff != World::kNoPage)
        enc.CopyTracked(pass::Buf::Voxels, world_->voxels, srcOff, d.staging,
                        i * kChunkBytes, kChunkBytes);
    }
    ctx_->queue.Submit(enc.Finish());
    d.map = rhi::MapReadDeferred(ctx_->device, d.staging, 0, n * kChunkBytes);
    demotes_.push_back(std::move(d));
  }
}

void Stream::HarvestDemotes(uint32_t tick) {
  // THE STALENESS BOUND, and why kDemoteFreshTicks is a correctness constant
  // rather than a tuning knob. The staging bytes are a snapshot at copyTick;
  // classifying them later demotes on words that may have been overwritten
  // since. The guard against that is `!cpuDirty.Has(slot)` — anything that
  // writes a chunk puts it in the mirror within a tick (op targets directly,
  // CA writes via the writer's membership + propagate, particle landings via
  // the flight shell) — but cpuDirty is TIGHTENED over time: a chunk written
  // after the copy, settled, and then confirmed clean by a POSTDATING
  // snapshot leaves the mirror again, and from that point the stale bytes
  // would pass every guard while missing the write.
  //
  // That exit takes a floor of ~5 ticks: >=1 tick for the write to land and
  // settle, plus the readback ring's >=2-tick snapshot lag, plus the
  // postdating requirement. A batch classified within 3 ticks of its copy is
  // therefore strictly inside the window where any intervening write is STILL
  // in the mirror and refuses the demote. Older batches are NOT trusted and
  // NOT dropped — the surviving slots are re-copied fresh, which costs one
  // more 4 MiB GPU copy and converges as soon as the GPU keeps up. Dropping
  // them instead would leak resident pages until the slot scrolls out, which
  // is bounded but is also how the pre-JITTER pool exhausted.
  constexpr uint32_t kDemoteFreshTicks = 3;
  const bool dbg = PtDbg();
  uint32_t demoted = 0, retried = 0, harvested = 0;
  while (!demotes_.empty() && demotes_.front().map.Ready()) {
    PendingDemote d = std::move(demotes_.front());
    demotes_.pop_front();
    d.map.Wait();  // Ready() above: resolves without blocking
    if (d.map.Succeeded() && d.map.Data()) {
      harvested++;
      if (demoteScratch_.size() < d.slots.size() * kChunkVol)
        demoteScratch_.resize(d.slots.size() * kChunkVol);
      // One sequential copy out of write-combined map memory; Classify then
      // reads cached RAM. Reading the mapped pointer directly is legal but
      // pays uncached-read cost per word.
      std::memcpy(demoteScratch_.data(), d.map.Data(),
                  d.slots.size() * kChunkBytes);
      d.map.Unmap();
      const bool fresh = tick >= d.copyTick && tick - d.copyTick <= kDemoteFreshTicks;
      std::vector<uint32_t> retry;
      std::vector<uint64_t> retryKeys;
      for (size_t i = 0; i < d.slots.size(); i++) {
        const uint32_t s = d.slots[i];
        if (!d.copied[i]) continue;  // no bytes for this index: never classify
        // Identity: the slot must still be the world chunk the bytes belong
        // to. A later shift repurposes slots for different world chunks, and
        // classifying chunk A's bytes into chunk B's table row is silent
        // corruption of the worst kind.
        if (World::PackChunkKey(world_->SlotToWorldChunk(s)) != d.keys[i])
          continue;
        if (world_->PageOffsetOfSlot(s) == World::kNoPage) continue;  // demoted already
        if (!fresh) {
          retry.push_back(s);
          retryKeys.push_back(d.keys[i]);
          continue;
        }
        if (world_->pages->CpuDirty().Has(s)) continue;  // written since the copy
        const uint32_t e =
            world_->pages->Classify(s, demoteScratch_.data() + i * kChunkVol);
        if (e == PageTable::kNeedsPage) continue;
        world_->pages->SetSentinel(s, e);
        demoted++;
      }
      if (!retry.empty()) {
        retried += (uint32_t)retry.size();
        IssueDemoteCopies(retry, retryKeys, tick);
      }
    } else {
      d.map.Unmap();
    }
    stagingPool_.push_back(d.staging);
  }
  if (demoted) world_->pages->FlushTableWrites(ctx_->queue);
  if (dbg && (harvested || retried))
    std::printf("[pt-time] demote harvest: batches=%u demoted=%u retried=%u "
                "queued=%zu\n",
                harvested, demoted, retried, demotes_.size());
}

void Stream::FlushResident() {
  std::vector<uint32_t> slots(kNumChunks);
  for (uint32_t i = 0; i < kNumChunks; i++) slots[i] = i;
  EvictSlots(slots, /*filter=*/false);
  DrainEvictions();  // a save wants the store complete NOW
}

void Stream::ReloadWindow(IVec3 origin) {
  // in-flight evictions belong to the world being replaced
  DrainEvictions(/*discard=*/true);
  DiscardDemotes();  // same: old-world bytes must never classify the new one
  world_->SetWindowOrigin(origin);
  modified_.assign(kNumChunks, 0);
  std::vector<uint32_t> slots(kNumChunks);
  for (uint32_t i = 0; i < kNumChunks; i++) slots[i] = i;
  FillSlots(slots);
}
