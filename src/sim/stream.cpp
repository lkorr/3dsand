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
#include "sim/worldedit.h"   // the authored layer re-applies on every refill

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

// ---- THE genList ENTRY WORD (docs/RESEARCH_streaming_hitch.md R2) ---------
// Mirror of the block above `fn list` in assets/shaders/worldgen.wgsl, which
// is where the encoding is stated in full. Checked by
// scripts/check_invariants.py --genbatch, because these are the two places
// that must agree.
constexpr uint32_t kGenSlotSkip = 0xFFFFu;        // GEN_SLOT_SKIP
constexpr uint32_t kGenBatchStubBit = 0x80000000u;  // GEN_BATCH_STUB_BIT
inline uint32_t GenEntry(uint32_t slot, uint32_t mat) {
  return (slot & 0xFFFFu) | ((mat & 0xFFFu) << 16);
}

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
  // The decoded word is the stored word VERBATIM, and only because the
  // never-stamp is zero. If that ever changes this must OR it back in, so
  // assert it rather than leaving a `| 0` in the inner loop to imply it.
  static_assert(kStampNever == 0,
                "RleDecodeChunk emits the stored word unchanged because "
                "kStampNever is 0 (= 'hasn't acted', so everything may move on "
                "the first tick); a non-zero never-stamp must be OR'd back in");
  uint32_t i = 0;
  for (size_t p = 0; p < pairs; p++) {
    const uint32_t run = rle[p * 2];
    const uint32_t w = rle[p * 2 + 1];
    // NO-WRAP form of `i + run > kChunkVol`. That test was 32-bit arithmetic
    // on an attacker-influenced length: `run` comes off disk via the region
    // store, and i=1, run=0xFFFFFFFF wraps the sum to 0, passes the guard, and
    // fills ~4 G words past a 16 KiB buffer. `i <= kChunkVol` is an invariant
    // here (it only ever advances by an accepted run), so the subtraction
    // cannot underflow.
    if (run > kChunkVol - i) return false;
    std::fill_n(out + i, run, w);
    i += run;
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
  // Publish the far-field edit index so FarField can reach it through World
  // (world.h's `farEdits`, forward-declared exactly like `pages`).
  world_->farEdits = &farEdits_;
  modified_.assign(kNumChunks, 0);
}

void Stream::OnMaterialsReloaded(const std::vector<MaterialDef>& mats) {
  // must match isRayBlocker in common.wgsl: solids, powders, opaque liquids,
  // MINUS micro-detail materials (a grass cell is mostly air, so it must not
  // stop a chunk-skipping shadow ray — see the comment there).
  blockerOf_.clear();
  // The placeholder's solid material, by NAME (materials are data, and a
  // hardcoded id here would be the thing CLAUDE.md's convention forbids). It
  // has to be inert — a placeholder that can ACT would wake the CA over a
  // plane the mirror has not been told about — and "stone" is the one material
  // worldgen already fills buried bulk with, which is what makes it the right
  // stand-in for ground the engine has not generated yet.
  stoneMat_ = 0;
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == "stone") { stoneMat_ = (uint32_t)i; break; }
  for (const auto& m : mats)
    blockerOf_.push_back((m.gpu.flags & kMatFlagMicro) == 0 &&
                         (m.gpu.klass == CLASS_SOLID || m.gpu.klass == CLASS_POWDER ||
                          (m.gpu.klass == CLASS_LIQUID &&
                           (m.gpu.flags & kMatFlagOpaque) != 0)));
}

void Stream::Update(IVec3 playerChunk, uint32_t tick) {
  // Unconditional, not behind PtDbg(): see the Timing comment in stream.h.
  // Six clock reads against a pass whose p99 is 42 ms is not a measurement
  // cost, and gating them on an env var is what let the prose about this
  // function's cost go 8x stale without anyone noticing.
  const double uT0 = PtNowMs();
  lastTick_ = tick;
  // harvest evictions whose readback completed since last tick (non-blocking)
  while (!pending_.empty() && pending_.front().map.Ready())
    CompleteOldest(/*discard=*/false);
  // harvest completed shift-demote batches (non-blocking; see HarvestDemotes)
  HarvestDemotes(tick);
  const double uT1 = PtNowMs();
  timing_.harvestMs += uT1 - uT0;

  // sticky modified set from the latest snapshot (slot-indexed, ~2 ticks
  // latent; see the accepted-race note in stream.h)
  const WorldSnapshot& snap = world_->Snap();
  if (snap.valid) {
    for (uint32_t i = 0; i < kNumChunks; i++) modified_[i] |= snap.dirtyFlags[i];
  }
  timing_.dirtyFoldMs += PtNowMs() - uT1;

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
  // ---- R1: enact any deferred wake whose T + kWakeLatency has arrived ----
  //
  // BEFORE the shift below, for a reason that is correctness and not tidiness:
  // a shift on a different axis regenerates the 32 slots where the two planes
  // intersect, and an older entry's verdict for those slots would then
  // describe a chunk that no longer lives there. Completing first means the
  // only entries InvalidatePendingSlots has to blank are ones whose deadline
  // has genuinely not arrived yet.
  //
  // Also before this tick's SubmitTick, which is what makes the wake legal at
  // all: RefilledSlot here is consumed by the Materialize inside SubmitTick,
  // so the woken chunks' 26-neighbourhoods get pages in the SAME tick the CA
  // first dispatches them.
  // R2: one batch of every in-flight plane's remaining worldgen, BEFORE the
  // completion below, so a plane whose deadline is this tick has certainly had
  // its last batch submitted (and therefore its readback issued) first.
  PumpGenBatches(tick);
  CompleteDueShifts(tick);

  // R4: at most one shift per FRAME (see BeginFrame in stream.h). Ungated for
  // callers with no frame loop, where one Update is one tick anyway.
  if (best >= 0 && !(frameGated_ && shiftedThisFrame_)) {
    ShiftAxis(best, d[best] > 0 ? 1 : -1);
    shiftedThisFrame_ = true;
  }
  timing_.totalMs += PtNowMs() - uT0;
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

  // A pending entry from an EARLIER shift may name some of these slots (two
  // planes on different axes intersect in a 32-slot line). Its verdict for
  // them is about to become stale; blank it. See InvalidatePendingSlots.
  InvalidatePendingSlots(slots);

  const double sT0 = PtNowMs();
  EvictSlots(slots, /*filter=*/true);
  timing_.evictMs += PtNowMs() - sT0;

  IVec3 no = o;
  if (axis == 0) no.x += dir;
  else if (axis == 1) no.y += dir;
  else no.z += dir;
  world_->SetWindowOrigin(no);
  FillSlots(slots, /*deferWake=*/true);
  // Cleared per shift, not per frame: a multi-axis frame runs ShiftAxis once
  // per axis, and axis 2's fill must still be able to wait on axis 1's
  // eviction if they happen to share a slot (the planes intersect along an
  // edge). Scoping the exemption to one shift keeps that case correct.
  for (uint32_t s : slots) shiftEvicted_[s] = 0;
  shifts_++;
  timing_.shifts++;
}

void Stream::EvictSlots(const std::vector<uint32_t>& slots, bool filter) {
  const WorldSnapshot& snap = world_->Snap();
  // Everything the completion needs is captured NOW: the slots are refilled
  // (and modified_ reset) before the readback lands.
  std::vector<std::pair<uint32_t, PendingEvict::Item>> toSave;
  toSave.reserve(slots.size());
  std::vector<uint32_t> sentRle;
  uint32_t unmodReal = 0;
  for (uint32_t s : slots) {
    bool worth = true;
    if (filter && snap.valid)
      worth = snap.occupancy[s] > 0 || modified_[s] != 0;
    if (!worth) continue;
    // ---- AN UNMODIFIED CHUNK IS ALREADY REPRODUCIBLE ----------------------
    //
    // This is the SAME argument the sentinel branch below makes, and nothing
    // in it depended on the slot being a sentinel. If nothing has written a
    // chunk since it entered the window, whatever it holds is recoverable
    // without the store: either genChunk produced it, and genChunk is a pure
    // function of (world chunk, seed) that reproduces it exactly on re-entry;
    // or FillSlots decoded it FROM the store, which still holds those bytes
    // (Get does not consume). A chunk changed since either event has
    // modified_ set, because every writer declares itself — CPU ops through
    // MarkModifiedBox immediately, CA activity through the snapshot's dirty
    // flags — and modified_ is STICKY, never cleared until the slot is
    // refilled, so one dirty report is enough forever.
    //
    // Measured under --autofly-surface: 363 of the 397 real-page slots on a
    // leaving plane are unmodified — 91%. Every one of them was paying a
    // 16 KiB GPU copy, a 4,096-word RLE encode and a store insert, and the
    // RLE of a mixed surface chunk EXPANDS to 32 KiB because worldgen's
    // per-cell palette jitter makes nearly every word its own run. Flying
    // across untouched terrain was filling the store, and the frame, with a
    // re-derivable copy of the world.
    //
    // This REPLACES `dropIfAir`, which was the same idea reached one step too
    // late: it copied and encoded the chunk first and only then dropped it if
    // the words turned out to be air. Every case it caught is caught here
    // without the copy, and the non-air unmodified chunks it had to keep are
    // exactly the ones the argument above says are re-derivable too.
    //
    // Gated on `filter` and a live snapshot, exactly as dropIfAir was: a
    // FlushResident (the save path) passes filter=false and still stores
    // every resident chunk, air included.
    if (filter && snap.valid && modified_[s] == 0) {
      unmodReal++;
      continue;
    }

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
    // An UNMODIFIED sentinel needs no store write at all either — that test
    // used to live here and has moved UP, above this branch, now that the same
    // argument is known to hold for real pages too. Skipping the Put is what
    // keeps JITTER planes from bloating the store with per-cell RLE they never
    // needed; it now keeps mixed surface planes out of it as well.
    const uint64_t off0 = world_->PageOffsetOfSlot(s);
    if (off0 == World::kNoPage) {
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
      // ...and into the far-field edit index. A sentinel is ONE material
      // everywhere (JITTER varies only the palette nibble, which the far field
      // does not store), so its cascade samples need no words at all.
      //
      // Gated on modified_ and NOT on `filter`, unlike the store write above:
      // FlushResident (the save path) passes filter=false and keeps the whole
      // resident window, pristine chunks included, and those are re-derivable
      // by the sieve. Indexing them would add ~73 no-op entries per chunk over
      // 32,768 slots on every save, for nothing.
      if (modified_[s] != 0)
        farEdits_.NoteUniformChunk(wc, world_->PageEntryOfSlot(s) & kPtMatMask);
      sentRle.reserve(kChunkVol * 2);  // see the re-reserve in CompleteOldest
      continue;
    }
    toSave.push_back({s, {world_->SlotToWorldChunk(s), modified_[s] != 0}});
  }
  // What the leaving plane actually costs: how many of its slots still need a
  // GPU copy and a store insert, against how many the re-derivability test
  // above skipped. Under --autofly-surface the skipped count is ~91% of the
  // real pages, which is the whole reason that test exists.
  if (PtDbg())
    std::printf("[pt-time] evict issue: slots=%zu stored=%zu skipUnmod=%u\n",
                slots.size(), toSave.size(), unmodReal);

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
          // Moved for the same reason as the sentinel Put above: this is the
          // site that hurts most, because a mixed SURFACE chunk is exactly the
          // one whose RLE does not compress (PLAN_surface_flight_perf.md B5).
          // Both encoders clear() their `out` first, so the scratch buffer is
          // safe to move from inside the loop.
          store_.Put(p.items[i].wc, std::move(rle));
          // The far-field edit index takes the SAME words, from the same
          // harvest — this is the one place the CPU ever sees an edited
          // chunk's content, and it is what lets a sieve refill put the
          // player's crater back after the chunk has left the window.
          if (p.items[i].edited) {
            if (e != 0u)
              farEdits_.NoteUniformChunk(p.items[i].wc, e & kPtMatMask);
            else
              farEdits_.NoteChunk(p.items[i].wc,
                                  (const uint32_t*)(ptr + i * kChunkBytes));
          }
          // RE-RESERVE AFTER THE MOVE, or the move costs more than the copy it
          // replaced. std::move leaves the scratch with NO CAPACITY, so the
          // next chunk regrows it geometrically from zero — ~13 reallocations
          // copying ~64 KiB, against the ONE exact-size allocation an lvalue
          // copy would have cost. The trade was assumed to favour the move and
          // without this it does not, worst exactly on the mixed surface chunks
          // the move was added for: their per-cell palette jitter makes almost
          // every word its own run, so the RLE is 8,192 words (32 KiB) and
          // actually EXPANDS the 16 KiB chunk. One reserve makes it one
          // allocation and no copying either way.
          rle.reserve(kChunkVol * 2);
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

void Stream::FillSlots(const std::vector<uint32_t>& slots, bool deferWake) {
  const double fT0 = PtNowMs();
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
      // A store hit is a chunk the store thought worth keeping, and its words
      // are decoded right here — cheaper than waiting for it to be evicted
      // again, and it is what re-seeds the index for chunks that were only
      // ever loaded from disk in this session.
      farEdits_.NoteChunk(wc, data.data());
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
      // Sub-chunk occupancy bitmask, built in the SAME sweep (world.h
      // kSubOccShift; layout per common.wgsl subOccIndex): [0..1] TOTAL,
      // [2..3] BLOCKERS. This is the third producer of the mask and it must
      // agree with sim_occupancy and genChunk exactly — a bit this path leaves
      // clear over matter is a chunk the raymarcher skips through.
      uint32_t sub[kSubOccStride] = {};
      // Indexed rather than range-for because the sub-chunk bit is a function
      // of the CHUNK-LINEAR INDEX, which the `continue` below would desync from
      // a counter incremented at the bottom of the loop.
      for (uint32_t li = 0; li < kChunkVol; li++) {
        const uint32_t w = data[li];
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
        // Mirror of common.wgsl subOccBitLocal: (bz * DIM + by) * DIM + bx over
        // the chunk-linear layout (lz * CHUNK + ly) * CHUNK + lx.
        const uint32_t bx = (li % kChunk) >> kSubOccShift;
        const uint32_t by = ((li / kChunk) % kChunk) >> kSubOccShift;
        const uint32_t bz = (li / (kChunk * kChunk)) >> kSubOccShift;
        const uint32_t sbit = (bz * kSubOccDim + by) * kSubOccDim + bx;
        const uint32_t sm = 1u << (sbit & 31u);
        sub[sbit >> 5] |= sm;
        if (m < blockerOf_.size() && blockerOf_[m]) {
          blockers++;
          sub[kSubOccWords + (sbit >> 5)] |= sm;
        }
      }
      // packing per common.wgsl packOccStain
      occ |= blockers << 16;
      occ |= anyStain << 31;
      ctx_->queue.WriteBuffer(world_->occupancy, (uint64_t)s * 4, &occ, 4);
      // ...and the sub-chunk mask in the tail of the same buffer (world.h).
      ctx_->queue.WriteBuffer(world_->occupancy,
                              ((uint64_t)kNumChunks + (uint64_t)s * kSubOccStride) * 4,
                              sub, sizeof(sub));
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
  // The store-hit branch is everything above: RLE decode, Classify, and up to
  // five deferred WriteBuffer calls per slot. Split from the gen path because
  // the two scale with different things — store hits with how much of the
  // plane the player has visited before, gen with how much is new.
  const double fT1 = PtNowMs();
  timing_.fillStoreMs += fT1 - fT0;
  if (!genSlots.empty()) {
    if (deferWake) {
      // ---- R2: THE PLANE IS QUEUED, NOT GENERATED ------------------------
      //
      // docs/RESEARCH_streaming_hitch.md R2. Everything below this line used
      // to happen on the shift tick: a page for all 1,024 slots, one genList
      // upload, one `worldgenList` dispatch of 1,024 workgroups writing 16 MiB,
      // one submit. That dispatch is 98% of the streaming path's entire GPU
      // bill (P3-F §2) and its tail is 11.3 ms on a plane full of trees and
      // caves — on a frame that is already at the refresh period.
      //
      // R1 made the plane inert for kWakeLatency ticks, which means nothing
      // needs it generated on THIS tick; it needs to be generated by the tick
      // the wake lands. So BeginGenPlane orders the plane, gives the slots it
      // is not generating yet a deterministic placeholder, and submits the
      // first kGenChunksPerTick chunks. Update pumps the rest, one batch a
      // tick, and the readback follows the last one.
      BeginGenPlane(genSlots);
      timing_.fillGenMs += PtNowMs() - fT1;
    } else {
      // ---- THE SYNCHRONOUS WHOLE-WINDOW PATH (ReloadWindow only) ---------
      //
      // A load or a regen refills the WHOLE window (32,768 slots, thirty-two
      // times the genList region a shift owns) and genChunk wakes the slots
      // in-kernel, so the mirror has to learn the act set on this tick or the
      // very first CA dispatch faults. There is no frame to protect here and
      // no plane-sized bound to respect, so this keeps both the single
      // dispatch and the old fence.

      // genChunk overwrites the WHOLE chunk of every slot in the list, so
      // every one of them needs a page before the dispatch — a kernel cannot
      // allocate (§3.5c at batch size = the genList count). No fill is queued:
      // the kernel is about to write all 4,096 words.
      if (world_->residency == World::Residency::Paged) {
        for (uint32_t gs : genSlots) world_->pages->EnsurePageForOverwrite(gs);
        world_->pages->FlushTableWrites(ctx_->queue);
        // ---- THE genChunk PRECONDITION, CHECKED HERE (P3-E) -------------
        // genChunk resolves through voxWordInChunk(genList[...] & 0xFFFF, ...),
        // so every slot in the list must hold a PAGE or all 4,096 of its
        // stores are dropped. EnsurePageForOverwrite one line above is
        // supposed to be that guarantee. If this fires, the CPU table lost the
        // page; if it never fires while the kernel still faults, the loss is on
        // the GPU side of the deferred table write. Those are different bugs
        // and a fault count cannot tell them apart.
        if (PtDbg() || getenv("SANDVOX_PT_FREELOG")) {
          for (uint32_t gs : genSlots) {
            if (world_->PageOffsetOfSlot(gs) != World::kNoPage) continue;
            const IVec3 wc = world_->SlotToWorldChunk(gs);
            std::printf("[pt-bad] tick %u genlist slot %u chunk (%d,%d,%d): "
                        "table says sentinel 0x%08x\n",
                        lastTick_, gs, wc.x * (int)kChunk, wc.y * (int)kChunk,
                        wc.z * (int)kChunk, world_->PageEntryOfSlot(gs));
          }
        }
      }
      // GenEntry(slot, 0) == slot: the placeholder-material field of the entry
      // word is only read in stub mode, which this path never selects. Written
      // raw so the whole-window list needs no repacking pass.
      ctx_->queue.WriteBuffer(world_->genList, 0, genSlots.data(),
                              genSlots.size() * 4);
      TickParams tp{};
      tp.seed = seed_;
      tp.genCount = (uint32_t)genSlots.size();
      IVec3 o = world_->WindowOrigin();
      tp.origin[0] = o.x; tp.origin[1] = o.y; tp.origin[2] = o.z;
      // Fluid-lab slab (world.h kLabSlabY): a lab window shift must refill
      // with the SAME slab genColumn produced at startup, not default terrain.
      tp.labMode = World::LabWorld() ? 1u : 0u;
      tp.genDeferWake = 0u;   // wake in-kernel; this path is not deferred
      tp.genBatch = 0u;       // one dispatch from the base of genList
      ctx_->queue.WriteBuffer(world_->tickUBO, 0, &tp, sizeof(tp));

      rhi::CommandEncoder enc = ctx_->device.CreateCommandEncoder();
      sim_->EncodeGenList(enc, (uint32_t)genSlots.size());
      ctx_->queue.Submit(enc.Finish());

      // genChunk has just overwritten these slots with PRISTINE procgen, which
      // un-does the authored edit layer exactly the way the far-field sieve
      // un-does a crater (see faredits.h). Re-queue every refilled chunk the
      // layer touches; the ops go out through the MutationQueue on the
      // following ticks. Cheap when there is no layer — QueueChunk returns on
      // an empty map before it hashes anything.
      if (!sandvox::WorldEditLayer().Empty())
        for (uint32_t gs : genSlots)
          sandvox::WorldEditLayer().QueueChunk(world_->SlotToWorldChunk(gs));
      timing_.fillGenMs += PtNowMs() - fT1;

      const double dT0 = PtNowMs();
      const uint64_t occBytes = (uint64_t)kNumChunks * 4;
      std::vector<uint32_t>& occ = genOccScratch_;
      if (occ.size() != kNumChunks) occ.assign(kNumChunks, 0);
      bool occValid = false;
      if (world_->residency == World::Residency::Paged) {
        if (!genOccStaging_)
          genOccStaging_ = CreateBuffer(
              ctx_->device, occBytes,
              rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "genOcc");
        rhi::CommandEncoder oenc = ctx_->device.CreateCommandEncoder();
        oenc.CopyTracked(pass::Buf::Occupancy, world_->occupancy, 0,
                         genOccStaging_, 0, occBytes);
        ctx_->queue.Submit(oenc.Finish());
        rhi::MapTicket omap =
            rhi::MapReadDeferred(ctx_->device, genOccStaging_, 0, occBytes);
        omap.Wait();
        occValid = omap.Succeeded() && omap.Data();
        if (occValid)
          std::memcpy(occ.data(), omap.Data(), (size_t)kNumChunks * 4);
        else
          std::fill(occ.begin(), occ.end(), 0u);  // failed: fall back to all
        omap.Unmap();
        // No `act`: the kernel already wrote dirtyIn/dirtyOut itself.
        ApplyGenVerdict(genSlots, {}, occ, occValid, {}, lastTick_);
      }
      timing_.demoteMs += PtNowMs() - dT0;
    }
  }
}

// ---- the deferred wake's second half -------------------------------------

rhi::Buffer Stream::AcquireShiftStaging() {
  if (!shiftStagingPool_.empty()) {
    rhi::Buffer b = shiftStagingPool_.back();
    shiftStagingPool_.pop_back();
    return b;
  }
  return CreateBuffer(
      ctx_->device, ((uint64_t)kNumChunks + (uint64_t)kNChunk * kNChunk) * 4,
      rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "shiftVerdict");
}


// ---- R2: the plane's worldgen, spread over ticks --------------------------

uint32_t Stream::PlaceholderMat(IVec3 wc, int* dist) {
  // ONE World::TerrainHeight per COLUMN, memoized for the plane. It is ~25
  // hash3 a call, so a per-slot call would be 1,024 of them on every shift;
  // a Z- or X-axis plane has only kNChunk distinct columns and reuses each
  // height for the 32 chunks stacked on it.
  //
  // The column CENTRE, not a corner: the value is a placeholder that lives at
  // most kGenBatches ticks and is replaced by the real thing, so what matters
  // is that it is a pure function of (world chunk, seed) — which is what makes
  // paged and dense agree and the world hash reproducible — and that it is
  // right about the SIDE of the ground the chunk is on.
  const uint64_t key = ((uint64_t)(uint32_t)wc.x << 32) | (uint32_t)wc.z;
  auto it = colHeight_.find(key);
  int h;
  if (it != colHeight_.end()) {
    h = it->second;
  } else {
    h = World::TerrainHeight(wc.x * (int)kChunk + (int)kChunk / 2,
                             wc.z * (int)kChunk + (int)kChunk / 2, seed_);
    colHeight_.emplace(key, h);
  }
  const int bot = wc.y * (int)kChunk;
  const int top = bot + (int)kChunk - 1;
  // ABOVE the ground: air. DESIGN.md's "unloaded space is solid and inert" is
  // about matter the CA could fall into, and there is none up here — while a
  // stone placeholder in open sky would be a wall of rock 25 m in front of a
  // sprinting player, and would block the shadow and openness walks with it.
  if (bot > h) {
    *dist = bot - h;
    return 0u;
  }
  // AT OR BELOW the ground: solid, which is the DESIGN rule in the place it
  // was written for. The neighbouring plane's sand and water are ACTIVE at the
  // window edge, and a void here would take a grain that the generation two
  // ticks later then deletes. Stone refuses it; the grain rests one chunk
  // early and falls when the real terrain arrives.
  *dist = (top < h) ? (h - top) : 0;
  return stoneMat_;
}

void Stream::BeginGenPlane(std::vector<uint32_t>& genSlots) {
  const bool paged = world_->residency == World::Residency::Paged;
  // genAct gives each in-flight plane ONE region of kShiftPlaneChunks entries.
  // A caller that defers more than a plane would run off the end of its own
  // region and corrupt the next plane's verdict, so this is an abort and not a
  // clamp.
  if (genSlots.size() > (size_t)kShiftPlaneChunks) {
    std::fprintf(stderr,
                 "stream: deferred gen list of %zu exceeds a genAct region "
                 "(%u); only a window shift may defer\n",
                 genSlots.size(), kShiftPlaneChunks);
    std::abort();
  }

  // BACKSTOP, not a throughput knob (IssueDemoteCopies' shape), and it runs
  // BEFORE the region below is claimed rather than after the push, which is the
  // whole of why it is here and not at the end. genRegion_ cycles modulo
  // kGenPlaneRing, so an entry queued while kGenPlaneRing are already in flight
  // would be handed the FRONT entry's region — and would then overwrite the
  // genAct words that entry's completion is about to read. Draining first keeps
  // the in-flight set strictly smaller than the ring, which is what makes
  // "distinct modulo kGenPlaneRing" true.
  //
  // It is a backstop because entries retire on a TICK deadline: a caller that
  // drives Update without advancing `tick` would queue them forever.
  // kWakeLatency + 1 is the steady-state ceiling at one shift per tick, so this
  // is slack on top of it. Force the front's remaining batches out before
  // waiting on it — its map does not exist until its last batch is submitted.
  while (pendingShifts_.size() >= kMaxPendingShifts) {
    PendingShift& front = pendingShifts_.front();
    FinishGenPlane(front);
    front.map.Wait();
    CompleteShift(front, lastTick_);
    shiftStagingPool_.push_back(front.staging);
    pendingShifts_.pop_front();
  }

  PendingShift ps;
  ps.tick = lastTick_;
  ps.lastBatchTick = lastTick_;   // batch 0 goes out below; not twice this tick
  ps.region = genRegion_;
  genRegion_ = (genRegion_ + 1u) % kGenPlaneRing;
  ps.staging = AcquireShiftStaging();

  // ---- THE ORDER: SURFACE OUTWARD, AND WHY IT IS THE ORDER ---------------
  //
  // Determinism first: the key is the chunk's distance from World::
  // TerrainHeight at its own column, and the tie-break is the slot index, so
  // the order is a pure function of the plane and the seed. Nothing here reads
  // a clock, a frame counter or a readback (Godot Voxel budgets in ms and
  // documents the non-repeatability that buys; this budgets in CHUNKS).
  //
  // Then the reason it is THAT function. The placeholder a slot holds until
  // its batch runs is only a lie near the ground: deep sky really is air and
  // buried bulk really is stone, but the surface band is where the terrain,
  // the trees, the water and every acting neighbour are. Generating it first
  // means the only chunks that spend ticks as a placeholder are the ones the
  // placeholder describes correctly.
  struct Keyed {
    int d;
    uint32_t slot;
    uint16_t mat;
  };
  colHeight_.clear();
  std::vector<Keyed> keyed;
  keyed.reserve(genSlots.size());
  for (uint32_t s : genSlots) {
    int d = 0;
    const uint32_t m = PlaceholderMat(world_->SlotToWorldChunk(s), &d);
    keyed.push_back({d, s, (uint16_t)m});
  }
  std::sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
    return a.d != b.d ? a.d < b.d : a.slot < b.slot;
  });
  const size_t n = keyed.size();
  ps.genSlots.reserve(n);
  ps.sentMat.reserve(n);
  for (const Keyed& k : keyed) {
    ps.genSlots.push_back(k.slot);
    ps.sentMat.push_back(k.mat);
  }
  ps.stale.assign(n, 0);

  const uint32_t first = std::min<uint32_t>(kGenChunksPerTick, (uint32_t)n);

  // ---- THE PLACEHOLDERS, CPU HALF ---------------------------------------
  //
  // Under paged, a slot that is already a SENTINEL only needs its table entry
  // changed — no page, no memory, no fill. That is most of a shift plane, and
  // it is why this costs the pool LESS than the old "a page for all 1,024
  // slots on the shift tick" did rather than more.
  //
  // A slot that is RESIDENT keeps its page and the stub dispatch overwrites
  // the words instead. Demoting it here would hand ~300 pages a shift to the
  // retire queue, which kPoolPages is not sized for (world.h's kPageRetireCeiling
  // assertion in RetirePages says so in as many words) — and it would buy
  // nothing, because the slot's own batch allocates a page again within
  // kGenBatches ticks.
  //
  // Under dense there are no sentinels and every slot takes the stub's words.
  if (paged) {
    for (size_t i = first; i < n; i++) {
      const uint32_t gs = ps.genSlots[i];
      if (world_->PageOffsetOfSlot(gs) != World::kNoPage) continue;
      world_->pages->SetSentinel(
          gs, ps.sentMat[i] == 0 ? kPtEmpty
                                 : (kPtSentinelBit | (uint32_t)ps.sentMat[i]));
    }
    world_->pages->FlushTableWrites(ctx_->queue);
  }

  // ---- THE PLACEHOLDERS, GPU HALF ---------------------------------------
  //
  // One stub dispatch over every slot this tick is NOT generating: it writes
  // the placeholder's words into the slots that still hold a page, and writes
  // the occupancy / sub-occupancy words and the cleared dirty flags for all of
  // them. The occupancy half is not optional — occupancyDirty only revisits
  // chunks the CA touched, so a slot left alone would keep the DEPARTED
  // chunk's counts, and those describe terrain 32 chunks away. The renderer
  // would skip solid chunks and march empty ones.
  //
  // Its own submit, because tickUBO is one buffer and two deferred writes to
  // it inside one command buffer would leave the LAST one in force for both
  // dispatches (rhi_vulkan's issue-order rule).
  if (n > first) {
    const uint32_t stubCount = (uint32_t)(n - first);
    genEntryScratch_.clear();
    genEntryScratch_.reserve(stubCount);
    for (size_t i = first; i < n; i++)
      genEntryScratch_.push_back(GenEntry(ps.genSlots[i], ps.sentMat[i]));
    const uint64_t base = (uint64_t)ps.region * kShiftPlaneChunks + first;
    ctx_->queue.WriteBuffer(world_->genList, base * 4, genEntryScratch_.data(),
                            (uint64_t)stubCount * 4);
    TickParams tp{};
    tp.seed = seed_;
    tp.genCount = stubCount;
    const IVec3 o = world_->WindowOrigin();
    tp.origin[0] = o.x; tp.origin[1] = o.y; tp.origin[2] = o.z;
    tp.labMode = World::LabWorld() ? 1u : 0u;
    tp.genDeferWake = 1u;
    tp.genBatch = (uint32_t)base | kGenBatchStubBit;
    ctx_->queue.WriteBuffer(world_->tickUBO, 0, &tp, sizeof(tp));
    rhi::CommandEncoder senc = ctx_->device.CreateCommandEncoder();
    sim_->EncodeGenList(senc, stubCount);
    ctx_->queue.Submit(senc.Finish());
  }

  pendingShifts_.push_back(std::move(ps));
  DispatchGenBatch(pendingShifts_.back());

}

void Stream::DispatchGenBatch(PendingShift& ps) {
  const double t0 = PtNowMs();
  const bool paged = world_->residency == World::Residency::Paged;
  const uint32_t n = (uint32_t)ps.genSlots.size();
  const uint32_t base = ps.dispatched;
  const uint32_t cnt = std::min(kGenChunksPerTick, n - base);
  const bool last = (base + cnt) >= n;

  // A page for every slot this batch generates, and ONLY those. genChunk
  // overwrites all 4,096 words of a chunk and a kernel cannot allocate
  // (PLAN_page_table.md §3.5c); the batch is the allocation unit now, which is
  // what makes a plane's page cost arrive a quarter at a time.
  genEntryScratch_.clear();
  genEntryScratch_.reserve(cnt);
  for (uint32_t i = base; i < base + cnt; i++) {
    // A later shift repurposed this slot under a new origin. Its verdict is
    // already blanked (InvalidatePendingSlots) and the NEW shift's own entry
    // covers it; generating it here would write the wrong chunk over a refill
    // that may have come from the store. The position is KEPT so genAct stays
    // aligned with ps.genSlots.
    if (ps.stale[i]) {
      genEntryScratch_.push_back(kGenSlotSkip);
      continue;
    }
    const uint32_t gs = ps.genSlots[i];
    if (paged) world_->pages->EnsurePageForOverwrite(gs);
    genEntryScratch_.push_back(GenEntry(gs, 0));
  }
  if (paged) {
    world_->pages->FlushTableWrites(ctx_->queue);
    // ---- THE genChunk PRECONDITION, CHECKED HERE (P3-E) -----------------
    // genChunk resolves through voxWordInChunk(genList[idx] & 0xFFFF, ...), so
    // every slot in the batch must hold a PAGE or all 4,096 of its stores are
    // dropped. EnsurePageForOverwrite above is supposed to be that guarantee.
    // If this fires, the CPU table lost the page; if it never fires while the
    // kernel still faults, the loss is on the GPU side of the deferred table
    // write. Those are different bugs and a fault count cannot tell them apart.
    if (PtDbg() || getenv("SANDVOX_PT_FREELOG")) {
      for (uint32_t i = base; i < base + cnt; i++) {
        if (ps.stale[i]) continue;
        const uint32_t gs = ps.genSlots[i];
        if (world_->PageOffsetOfSlot(gs) != World::kNoPage) continue;
        const IVec3 wc = world_->SlotToWorldChunk(gs);
        std::printf("[pt-bad] tick %u genlist slot %u chunk (%d,%d,%d): "
                    "table says sentinel 0x%08x\n",
                    lastTick_, gs, wc.x * (int)kChunk, wc.y * (int)kChunk,
                    wc.z * (int)kChunk, world_->PageEntryOfSlot(gs));
      }
    }
  }

  const uint64_t region = (uint64_t)ps.region * kShiftPlaneChunks;
  ctx_->queue.WriteBuffer(world_->genList, (region + base) * 4,
                          genEntryScratch_.data(), (uint64_t)cnt * 4);
  TickParams tp{};
  tp.seed = seed_;
  tp.genCount = cnt;
  // THE CURRENT ORIGIN, not the origin at T, and that is not a bug. genChunk
  // derives its world chunk from (slot, origin), and a shift on ANOTHER axis
  // changes which world chunk a slot holds for exactly the 32 slots the two
  // planes share — which are precisely the ones InvalidatePendingSlots has
  // just marked stale and this batch skips. Every other slot of this plane
  // still holds the same world chunk it did at T.
  const IVec3 o = world_->WindowOrigin();
  tp.origin[0] = o.x; tp.origin[1] = o.y; tp.origin[2] = o.z;
  // Fluid-lab slab (world.h kLabSlabY): a lab window shift must refill with
  // the SAME slab genColumn produced at startup, not default terrain.
  tp.labMode = World::LabWorld() ? 1u : 0u;
  // R1: publish the act verdict instead of acting on it (world.h's
  // genDeferWake, the genChunk tail in worldgen.wgsl).
  tp.genDeferWake = 1u;
  tp.genBatch = (uint32_t)(region + base);
  ctx_->queue.WriteBuffer(world_->tickUBO, 0, &tp, sizeof(tp));

  rhi::CommandEncoder enc = ctx_->device.CreateCommandEncoder();
  sim_->EncodeGenList(enc, cnt);
  // ---- THE READBACK, AFTER THE LAST BATCH -------------------------------
  //
  // R1 queued this at T, when the plane was one dispatch. Under R2 the plane
  // is not complete until here, and the CPU's demote classification and its
  // "is any neighbour air" test both read occupancy for the WHOLE plane — so
  // the copy has to be behind the last batch in queue order. Recorded in the
  // same command buffer as that batch: the tracker derives the compute ->
  // transfer barrier from the row's W(Occupancy) / W(GenAct) exactly as it
  // does for the eviction copies.
  //
  // ONE COPY, TWO REGIONS. occupancy (128 KiB, needed for the demote
  // classification and for the "is any neighbour air" test that decides
  // whether a FULL chunk is inert) then this plane's genAct region (one u32
  // per generated slot). Both are read in both residency modes: dense skips
  // the page-table half of the completion but must take the WAKE at the same
  // tick as paged, or the two modes would not hash identically — and
  // `--residency dense` is the only live differential oracle this system has.
  const uint64_t occBytes = (uint64_t)kNumChunks * 4;
  if (last) {
    enc.CopyTracked(pass::Buf::Occupancy, world_->occupancy, 0, ps.staging, 0,
                    occBytes);
    enc.CopyTracked(pass::Buf::GenAct, world_->genAct, region * 4, ps.staging,
                    occBytes, (uint64_t)n * 4);
  }
  ctx_->queue.Submit(enc.Finish());
  if (last) {
    ps.map = rhi::MapReadDeferred(ctx_->device, ps.staging, 0,
                                  occBytes + (uint64_t)n * 4);
    ps.mapIssued = true;
  }

  // genChunk has just overwritten these slots with PRISTINE procgen, which
  // un-does the authored edit layer exactly the way the far-field sieve un-does
  // a crater (see faredits.h). Re-queue the chunks THIS BATCH refilled; the ops
  // go out through the MutationQueue on the following ticks. Per batch and not
  // per plane, because an op queued at T for a chunk generated at T+3 would
  // drain first and be overwritten by the generation it was meant to patch.
  if (!sandvox::WorldEditLayer().Empty())
    for (uint32_t i = base; i < base + cnt; i++)
      if (!ps.stale[i])
        sandvox::WorldEditLayer().QueueChunk(
            world_->SlotToWorldChunk(ps.genSlots[i]));

  ps.dispatched = base + cnt;
  timing_.genBatchMs += PtNowMs() - t0;
  timing_.genBatches++;
}

void Stream::PumpGenBatches(uint32_t tick) {
  // Every plane that still owes work gets exactly ONE batch this tick. At the
  // ~1 shift/tick of sprint flight that is kGenBatches planes contributing
  // kGenChunksPerTick chunks each — the same per-tick total the single
  // dispatch used to produce in one lump, which is the honest reading of what
  // spreading buys: the same mean, a quarter of the maximum.
  for (PendingShift& ps : pendingShifts_) {
    if (ps.dispatched >= ps.genSlots.size()) continue;
    if (ps.lastBatchTick == tick) continue;   // BeginGenPlane already did one
    ps.lastBatchTick = tick;
    DispatchGenBatch(ps);
  }
}

void Stream::FinishGenPlane(PendingShift& ps) {
  while (ps.dispatched < ps.genSlots.size()) DispatchGenBatch(ps);
}

void Stream::InvalidatePendingSlots(const std::vector<uint32_t>& slots) {
  if (pendingShifts_.empty()) return;
  // A plane is 1,024 of 32,768 slots and the intersection with another axis's
  // plane is 32 of them, so a membership bitmap beats a per-entry sort.
  std::vector<uint8_t>& mark = shiftMark_;
  if (mark.size() != kNumChunks) mark.assign(kNumChunks, 0);
  for (uint32_t s : slots) mark[s] = 1;
  for (PendingShift& ps : pendingShifts_)
    for (size_t i = 0; i < ps.genSlots.size(); i++)
      if (mark[ps.genSlots[i]]) ps.stale[i] = 1;
  for (uint32_t s : slots) mark[s] = 0;  // left all-zero for the next call
}

void Stream::DiscardPendingShifts() {
  for (PendingShift& ps : pendingShifts_) {
    // Under R2 an entry spends most of its life with no ticket at all (the
    // readback follows its LAST batch), and the un-dispatched batches simply
    // never happen: both callers refill the whole window themselves, so the
    // placeholders those slots hold are overwritten before anything reads them.
    if (ps.mapIssued) {
      ps.map.Wait();  // release the fence borrow before the buffer goes back
      ps.map.Unmap();
    }
    shiftStagingPool_.push_back(ps.staging);
  }
  pendingShifts_.clear();
}

void Stream::CompleteDueShifts(uint32_t tick) {
  while (!pendingShifts_.empty()) {
    PendingShift& ps = pendingShifts_.front();
    // Unsigned-safe: a caller that rewinds `tick` (the gates each start their
    // own tick base) must not wrap into "not due for four billion ticks".
    if (tick >= ps.tick && tick - ps.tick < kWakeLatency) break;
    // Structurally unreachable (PumpGenBatches above retires kGenChunksPerTick
    // a tick and the last batch lands at T + kGenBatches - 1, which is
    // kWakeDrainTicks before this deadline) — but the ONE thing that must never
    // happen here is reading a ticket that was never issued, so force the
    // remainder out rather than trusting the arithmetic.
    if (!ps.mapIssued) FinishGenPlane(ps);
    if (!ps.map.Ready()) {
      // The fence covers work submitted kWakeLatency ticks ago and has
      // essentially always retired by now. When it has not, this degrades to
      // the pre-R1 behaviour for ONE shift — counted and printed, because "the
      // fence came back" must be visible rather than inferred from a frame
      // histogram.
      const double w0 = PtNowMs();
      ps.map.Wait();
      timing_.wakeWaitMs += PtNowMs() - w0;
      timing_.wakeWaits++;
    }
    CompleteShift(ps, tick);
    shiftStagingPool_.push_back(ps.staging);
    pendingShifts_.pop_front();
  }
}

void Stream::CompleteShift(PendingShift& ps, uint32_t tick) {
  const double dT0 = PtNowMs();
  const uint64_t occBytes = (uint64_t)kNumChunks * 4;
  std::vector<uint32_t>& occ = genOccScratch_;
  std::vector<uint32_t>& act = genActScratch_;
  if (occ.size() != kNumChunks) occ.assign(kNumChunks, 0);
  act.assign(ps.genSlots.size(), 0u);
  bool occValid = false;
  if (ps.map.Succeeded() && ps.map.Data()) {
    const uint8_t* base = (const uint8_t*)ps.map.Data();
    std::memcpy(occ.data(), base, (size_t)kNumChunks * 4);
    std::memcpy(act.data(), base + occBytes, ps.genSlots.size() * 4);
    occValid = true;
  } else {
    // A failed read flips two conservative directions at once and they are
    // OPPOSITES: for demotion, zeros mean "read every chunk's words", which is
    // safe; for the act set, zeros would mean "wake nothing", which is silent
    // voxel loss. So the verdict degrades to "wake the whole plane" — the
    // pre-act-set behaviour, wide but never wrong — and ApplyGenVerdict's
    // occValid=false branch does exactly that.
    std::fill(occ.begin(), occ.end(), 0u);
    std::fill(act.begin(), act.end(), 1u);
  }
  ps.map.Unmap();
  ApplyGenVerdict(ps.genSlots, ps.stale, occ, occValid, act, tick);
  timing_.demoteMs += PtNowMs() - dT0;
  if (PtDbg())
    std::printf("[pt-time] shift wake T+%u: gen=%zu %.2f ms\n",
                tick - ps.tick, ps.genSlots.size(), PtNowMs() - dT0);
}

void Stream::ApplyGenVerdict(const std::vector<uint32_t>& genSlots,
                             const std::vector<uint8_t>& stale,
                             const std::vector<uint32_t>& occ, bool occValid,
                             const std::vector<uint32_t>& act, uint32_t tick) {
  const bool paged = world_->residency == World::Residency::Paged;
  const bool enactWake = !act.empty();
  const WorldSnapshot& snap = world_->Snap();
  auto isStale = [&](size_t i) { return i < stale.size() && stale[i] != 0; };

  // ---- contributor (d), the ACT SET: wake only what can act -------------
  //
  // The blanket form of this — RefilledSlot for every slot of the plane — is
  // what ran the mirror away: under --autofly-hard the plane is ~all
  // JITTER-demotable stone, every slot of it hasMatter, so the whole plane
  // plus its 26-ring (~3,072 chunks) materialized every shift on consecutive
  // frames, against a free path on an 8-snapshot hysteresis. Measured twice as
  // a FATAL pool exhaustion (32,148 and 31,691 of 32,768). Filtering by
  // hasMatter cannot help: a buried stone chunk IS matter. The right question
  // is not "does it hold matter" but "can any cell in it ACT":
  //
  //   - pure sky (nonAir == 0): nothing in it can move, and matter can only
  //     ARRIVE from an acting neighbour, which is covered by that neighbour's
  //     own ring (Materialize's bracketed half, verbatim);
  //   - genChunk says a cell can act: wake, unconditionally. THIS TERM IS NEW
  //     WITH R1 and it is the invariant the rest of the design rests on —
  //     cpuDirty must be a SUPERSET of dirtyIn, and `act` IS the set this
  //     function is about to write into dirtyIn. Declaring one and not the
  //     other is the 217-page-fault bug with the sides swapped.
  //   - mixed (0 < nonAir < CHUNK_VOL): a free surface. Wakes.
  //   - full (nonAir == CHUNK_VOL): cells can act only toward air, and a CA
  //     write reaches <= 1 cell (rule 1), so a full chunk with NO air anywhere
  //     in its 26-neighbourhood is inert — that is the buried bulk, and it is
  //     exactly the JITTER win being protected here. Air in any neighbour
  //     wakes it. Out-of-window neighbours are inert by definition.
  //
  // The occupancy read here is kWakeLatency ticks old for a deferred shift,
  // and the neighbour test is the only consumer that could care. It cannot go
  // wrong in the dangerous direction: if a neighbour became air in those ticks
  // it was written, so it is in cpuDirty, so Materialize gives IT a page — and
  // "it" is precisely the cell this chunk would write into.
  std::vector<uint32_t> wake;
  if (enactWake) wake.reserve(genSlots.size());
  for (size_t i = 0; i < genSlots.size(); i++) {
    if (isStale(i)) continue;
    const uint32_t gs = genSlots[i];
    const bool canAct = enactWake && act[i] != 0u;
    if (canAct) wake.push_back(gs);
    if (!paged) continue;
    bool w = true;
    if (occValid) {
      const uint32_t nonAir = occ[gs] & 0xFFFFu;
      if (nonAir == 0u && !canAct) continue;  // pure sky cannot act
      w = canAct || nonAir != kChunkVol;
      if (!w) {
        const IVec3 wc = world_->SlotToWorldChunk(gs);
        for (int dz = -1; dz <= 1 && !w; dz++)
          for (int dy = -1; dy <= 1 && !w; dy++)
            for (int dx = -1; dx <= 1 && !w; dx++) {
              if (!dx && !dy && !dz) continue;
              const IVec3 nc{wc.x + dx, wc.y + dy, wc.z + dz};
              if (!world_->ChunkInWindow(nc)) continue;
              if ((occ[World::SlotChunkIndex(nc)] & 0xFFFFu) != kChunkVol)
                w = true;
            }
      }
    }
    if (w) world_->pages->RefilledSlot(gs);
  }

  if (paged) {
    // ---- the demote candidates (§3.5c's compaction, on the streaming path) -
    //
    // Without this the shift plane LEAKS: genChunk needs a page for every slot
    // it writes, but ~85% of a shift plane generates as pure sky, and a page
    // that is never demoted is never freed either. Measured before this
    // landed: ~880 pages leaked per window shift.
    //
    // CLASSIFY ON THE WORDS, never on `occupancy` — occupancy counts non-air
    // cells but the hash also covers the STAIN layer, and PageTable::Classify
    // is the ONE promotion rule (§2.3). The occupancy read only narrows WHICH
    // chunks are read. The one exception is the sky share below, and it is an
    // exception because these slots were overwritten end to end by the
    // genChunk dispatch whose own in-kernel count this is: genCell returns
    // packVox(mat, state, STAMP_NEVER), so it can set neither bit 31
    // (kCellOpIfAir) nor a stain bit, and `nonAir == 0` on a freshly generated
    // slot does not merely suggest PT_EMPTY's content, it IS PT_EMPTY's
    // content.
    std::vector<uint32_t> cands;
    cands.reserve(genSlots.size());
    uint32_t skyDemoted = 0, skyHeld = 0;
    for (size_t i = 0; i < genSlots.size(); i++) {
      if (isStale(i)) continue;
      const uint32_t gs = genSlots[i];
      if (world_->PageOffsetOfSlot(gs) == World::kNoPage) continue;
      const uint32_t nonAir = occ[gs] & 0xFFFFu;  // low 16 = non-air count
      if (occValid && nonAir == 0u) {
        // ---- R1 RACE 1: the count is kWakeLatency ticks old ------------
        //
        // Between generation and now, an acting neighbour may have pushed a
        // voxel into this chunk. Those writes land on a real page (every gen
        // slot got one) and are perfectly legal; what is not legal is
        // demoting the chunk to PT_EMPTY afterwards, which frees the page and
        // deletes the grain. The writer was in dirtyIn at the write tick, so
        // it is in cpuDirty, and Materialize dilates cpuDirty by N26 — this
        // chunk is therefore in cpuDirty if anything could have written it.
        // The snapshot's own flag is the belt to that braces.
        //
        // A held page is NOT a leak: the slot is resident and reported, so
        // PageTable::ConsumeOccupancy's hysteresis frees it a few snapshots
        // later. The leak the demote pass exists to stop is the slot that
        // SCROLLED OUT and stopped being reported at all.
        if (world_->pages->CpuDirty().Has(gs) ||
            (snap.valid && snap.dirtyFlags[gs])) {
          skyHeld++;
          continue;
        }
        world_->pages->SetSentinel(gs, kPtEmpty);
        skyDemoted++;
        continue;
      }
      // A partially-full chunk cannot demote: no sentinel form describes a mix
      // of air and matter. The FULL case still needs the words — a sentinel
      // must reproduce the resident content bit-exactly, which is Classify's
      // exact-word rule and not something a count can decide.
      if (nonAir != 0u && nonAir != kChunkVol) continue;
      cands.push_back(gs);
    }
    if (skyDemoted) world_->pages->FlushTableWrites(ctx_->queue);

    // ISSUE the copies now, CLASSIFY on a later frame's harvest. Under R1
    // these are encoded kWakeLatency ticks AFTER genChunk rather than
    // immediately behind it, which is strictly safer: any neighbour write
    // during those ticks is already in the bytes, so Classify sees it and
    // refuses the demote rather than relying on the CpuDirty guard alone.
    std::vector<uint64_t> keys;
    keys.reserve(cands.size());
    for (uint32_t gs : cands)
      keys.push_back(World::PackChunkKey(world_->SlotToWorldChunk(gs)));
    IssueDemoteCopies(cands, keys, tick);
    if (PtDbg())
      std::printf("[pt-time] shift demote: gen=%zu cands=%zu sky=%u held=%u\n",
                  genSlots.size(), cands.size(), skyDemoted, skyHeld);
  }

  // ---- and only now, THE WAKE ------------------------------------------
  //
  // Exactly what genChunk's two atomicStores used to do, moved to the CPU and
  // kWakeLatency ticks later: dirtyIn and dirtyOut ARE dirty[page] and
  // dirty[1 - page], so writing both pages is the same set of flags whichever
  // page this tick is on (the store-hit branch above writes both for the same
  // reason). The write is deferred and drains at the HEAD of the next command
  // buffer, which is this tick's SubmitTick — ahead of sim_compact, and after
  // the RefilledSlot calls above have been handed to the same tick's
  // Materialize. That ordering is the whole correctness argument.
  //
  // Runs of consecutive slots are coalesced into one call. A Z-axis plane is a
  // single contiguous span of 1,024 slots; an X-axis plane strides by kNChunk
  // and coalesces into nothing, which is why this is a run-length walk and not
  // an unconditional one-call-per-slot loop.
  if (!wake.empty()) {
    static const std::vector<uint32_t> ones((size_t)kNChunk * kNChunk, 1u);
    std::sort(wake.begin(), wake.end());
    size_t i = 0;
    while (i < wake.size()) {
      size_t j = i + 1;
      while (j < wake.size() && wake[j] == wake[j - 1] + 1) j++;
      const size_t n = j - i;
      const uint64_t off = (uint64_t)wake[i] * 4;
      ctx_->queue.WriteBuffer(world_->dirty[0], off, ones.data(), n * 4);
      ctx_->queue.WriteBuffer(world_->dirty[1], off, ones.data(), n * 4);
      i = j;
    }
    // This is a waking path that writes dirtyIn without going through a
    // Simulation::Encode* entry point, so it declares itself to the §3.4
    // settled-skip latch here — the same self-declaration rule EncodeWakeAll
    // and EncodeGenList follow. EncodeGenList already did this at tick T, but
    // T is not when the dirty set actually moved any more.
    sim_->NoteWakeAll();
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
  double cpyMs = 0.0, clsMs = 0.0;
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
      const double c0 = dbg ? PtNowMs() : 0.0;
      std::memcpy(demoteScratch_.data(), d.map.Data(),
                  d.slots.size() * kChunkBytes);
      if (dbg) cpyMs += PtNowMs() - c0;
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
        const double k0 = dbg ? PtNowMs() : 0.0;
        const uint32_t e =
            world_->pages->Classify(s, demoteScratch_.data() + i * kChunkVol);
        if (dbg) clsMs += PtNowMs() - k0;
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
                "queued=%zu (memcpy %.2f ms, classify %.2f ms)\n",
                harvested, demoted, retried, demotes_.size(), cpyMs, clsMs);
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
  DiscardPendingShifts();  // and so do any un-enacted shift verdicts
  world_->SetWindowOrigin(origin);
  modified_.assign(kNumChunks, 0);
  std::vector<uint32_t> slots(kNumChunks);
  for (uint32_t i = 0; i < kNumChunks; i++) slots[i] = i;
  // deferWake=false: this is the WHOLE window (32,768 slots, far past genAct's
  // one-plane size), it is not on a frame path worth protecting, and a load
  // must be simulable on the tick it lands rather than two ticks later.
  FillSlots(slots, /*deferWake=*/false);
}
