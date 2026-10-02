#include "sim/stream.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gpu/context.h"
#include "gpu/resources.h"
#include "sim/oprecord.h"    // the gen list is part of the tick's recorded input
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
  // ...and so does the region codec's state predictor (SVR3). Compression
  // only: each region file records the seed it was encoded with.
  store_.SetSeed(seed);
  world_->SetMirrorSeed(seed);
  // Publish the far-field edit index so FarField can reach it through World
  // (world.h's `farEdits`, forward-declared exactly like `pages`).
  world_->farEdits = &farEdits_;
  // ...and the far fire-plume index beside it, for SubmitTick to build the
  // emitter list from (world.h's `farPlumes`).
  world_->farPlumes = &farPlumes_;
  // ...and the chunk tickets (src/sim/tickets.h), for SubmitWorldgen to drop
  // before a regen (world.h's `tickets`).
  tickets_.Init(world_, this);
  world_->tickets = &tickets_;
  modified_.assign(kNumSlots, 0);
  // The window is about to be generated from nothing: the delta save's
  // coverage proof starts here (FlushResident in stream.h).
  ResetSaveTracking();
  // Sized once beside modified_ so every later path can index it without a
  // bounds dance (M9.5-A). All-zero is "no slot is waiting on anybody",
  // which is the only state a single-player run ever reaches.
  awaitingRemote_.assign(kNumSlots, 0);
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

void Stream::Update(const InterestSet& interest, uint32_t tick) {
  // Unconditional, not behind PtDbg(): see the Timing comment in stream.h.
  // Six clock reads against a pass whose p99 is 42 ms is not a measurement
  // cost, and gating them on an env var is what let the prose about this
  // function's cost go 8x stale without anyone noticing.
  const double uT0 = PtNowMs();
  // ONE window, so one centre. Every other point in the set is a residency
  // OBLIGATION this function cannot discharge (see interest.h) — reading
  // Primary() here is what makes today's behaviour identical to the old
  // Update(IVec3, tick), not an accident to be tidied away later.
  const IVec3 playerChunk = interest.Primary();
  lastTick_ = tick;
  // harvest evictions whose readback completed since last tick (non-blocking).
  // A ticket batch whose keep decision has not been made yet waits (it is
  // made within kSnapshotLatency ticks; see Tickets, THE RELEASE IS TWO-PHASE).
  while (!pending_.empty() && pending_.front().map.Ready() &&
         !pending_.front().keepPending)
    CompleteOldest(/*discard=*/false);
  // harvest completed shift-demote batches (non-blocking; see HarvestDemotes)
  HarvestDemotes(tick);
  // ...and completed solute evictions (non-blocking; see EvictSolutes)
  HarvestSoluteEvicts(/*wait=*/false);
  const double uT1 = PtNowMs();
  timing_.harvestMs += uT1 - uT0;

  // sticky modified set from the latest snapshot (slot-indexed, ~2 ticks
  // latent; see the accepted-race note in stream.h)
  FoldSnapshot();
  timing_.dirtyFoldMs += PtNowMs() - uT1;

  // ---- CHUNK TICKETS (src/sim/tickets.h) ---------------------------------
  // The between-ticks step: fold the published snapshot, finalize / release /
  // activate / re-centre. Here, after the eviction harvest and before the
  // deferred wakes and the shift, because a ticket's fill and release are
  // submits of the same kind a shift makes, and the window-overlap release
  // below must see the tickets this step left live.
  TicketTick(tick);

  IVec3 o = world_->WindowOrigin();
  int half = (int)kNChunk / 2;
  // No point of interest => no shift, but the harvests above and the pending
  // shift completions below still run: they are owed work from EARLIER ticks
  // and their deadlines are in ticks, not in player motion. Skipping them
  // would leave a plane inert past its T+kWakeLatency wake.
  int d[3] = {0, 0, 0};
  if (!interest.Empty()) {
    d[0] = playerChunk.x - (o.x + half);
    d[1] = playerChunk.y - (o.y + half);
    d[2] = playerChunk.z - (o.z + half);
  }
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
  CompleteDueShifts(tick);

  // ---- slots a DeliverMiss released (M9.5-A) ------------------------------
  //
  // AFTER CompleteDueShifts and BEFORE the shift below, which is the same
  // slot the deferred wake occupies and for the same reason: this is a refill
  // that submits, so it must run where a submit is legal and where a shift
  // cannot then invalidate it in the same call.
  //
  // deferWake=false, like ReloadWindow and unlike a shift. These are a
  // handful of scattered slots rather than a plane, genAct is sized to one
  // plane, and there is no frame-time cliff to protect here - the cost the
  // deferral buys back is a shift's, and a miss is not a shift.
  if (!genAfterMiss_.empty()) {
    std::vector<uint32_t> miss;
    miss.swap(genAfterMiss_);
    // See fillIgnoresExchange_ in stream.h: the exchange has already answered
    // "miss" for these chunks and must not be asked again, or the hold and
    // the miss would ping-pong.
    fillIgnoresExchange_ = true;
    FillSlots(miss, /*deferWake=*/false);
    fillIgnoresExchange_ = false;
  }

  // R4: at most one shift per FRAME (see BeginFrame in stream.h). Ungated for
  // callers with no frame loop, where one Update is one tick anyway.
  if (best >= 0 && !(frameGated_ && shiftedThisFrame_)) {
    // THE WINDOW-OVERLAP RULE (docs/PLAN_chunk_tickets.md §2.4). A ticket box
    // the shifted window would cover is RELEASED first: its chunks go to the
    // store (the batch is forced by the plane fill's drain, keep-all), so the
    // plane decodes the ticket's state instead of regenerating over it, and
    // no chunk is ever resident twice.
    IVec3 no = o;
    const int dir = d[best] > 0 ? 1 : -1;
    if (best == 0) no.x += dir;
    else if (best == 1) no.y += dir;
    else no.z += dir;
    tickets_.ReleaseOverlapping(no, tick);
    ShiftAxis(best, dir);
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
        // ...and break the chunk's QUIET STREAK (M9.3-A). `modified_` is
        // sticky-ever ("this chunk has been edited since it was generated")
        // and is the wrong signal for "settled enough to compare digests
        // with a peer". The streak is the right one, and a CPU edit is
        // activity the snapshot's dirty flags only report K ticks later.
        world_->NoteChunkTouched(wc);
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
  if (shiftEvicted_.size() != kNumSlots) shiftEvicted_.assign(kNumSlots, 0);
  for (uint32_t s : slots) shiftEvicted_[s] = 1;

  // A pending entry from an EARLIER shift may name some of these slots (two
  // planes on different axes intersect in a 32-slot line). Its verdict for
  // them is about to become stale; blank it. See InvalidatePendingSlots.
  InvalidatePendingSlots(slots);

  const double sT0 = PtNowMs();
  EvictSlots(slots, /*filter=*/true);
  // The plane's dissolved mass leaves with it (before the origin moves, while
  // SlotToWorldChunk still names the chunks that are leaving).
  EvictSolutes(slots);
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

void Stream::EvictSlots(const std::vector<uint32_t>& slots, bool filter,
                        const std::vector<uint8_t>* keep) {
  const WorldSnapshot& snap = world_->Snap();
  // Everything the completion needs is captured NOW: the slots are refilled
  // (and modified_ reset) before the readback lands.
  std::vector<std::pair<uint32_t, PendingEvict::Item>> toSave;
  toSave.reserve(slots.size());
  std::vector<uint32_t> sentRle;
  uint32_t unmodReal = 0;
  for (uint32_t s : slots) {
    // The delta save's mask (FlushResident): a slot it does not name is
    // reproducible without the store, by exactly the argument below.
    if (keep && (*keep)[s] == 0) continue;
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
    // Gated on `filter` and a live snapshot, exactly as dropIfAir was. The
    // save path (FlushResident) passes filter=false and applies the SAME rule
    // through `keep` above instead, with the unpublished snapshot tail folded
    // in — a save cannot accept the ~kSnapshotLatency-tick race this test
    // accepts for a trailing plane.
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
      // THE FIRST OF THE TWO EVICTION HOOKS (M9.5-A). BEFORE the Put, because
      // the Put MOVES the RLE out of `sentRle` - and before rather than after
      // for the stronger reason too: the exchange is handed the exact bytes
      // the store is about to keep, so a forwarded chunk and a re-entered one
      // can never disagree. `lastTick_` is the tick this eviction was decided
      // on, which for the sentinel path is also now.
      if (exchange_) exchange_->OnEvicted(wc, lastTick_, sentRle);
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
      if (modified_[s] != 0) {
        farEdits_.NoteUniformChunk(wc, world_->PageEntryOfSlot(s) & kPtMatMask);
        farPlumes_.NoteUniformChunk(wc, world_->PageEntryOfSlot(s) & kPtMatMask);
      }
      sentRle.reserve(kChunkVol * 2);  // see the re-reserve in CompleteOldest
      continue;
    }
    toSave.push_back({s, {world_->SlotToWorldChunk(s), modified_[s] != 0}});
  }
  // What the leaving plane actually costs: how many of its slots still need a
  // GPU copy and a store insert, against how many the re-derivability test
  // above skipped. Under --autofly-surface the skipped count is ~91% of the
  // real pages, which is the whole reason that test exists.
  if (PtDbg()) {
    size_t modCount = 0;
    for (uint8_t m : modified_) modCount += (m != 0);
    std::printf("[pt-time] evict issue: slots=%zu stored=%zu skipUnmod=%u "
                "snapValid=%d snapTick=%u modified=%zu\n",
                slots.size(), toSave.size(), unmodReal, snap.valid ? 1 : 0,
                snap.valid ? snap.tick : 0u, modCount);
  }

  for (size_t off = 0; off < toSave.size(); off += kEvictBatch) {
    size_t n = std::min(kEvictBatch, toSave.size() - off);
    PendingEvict p;
    p.staging = AcquireStaging();
    p.tick = lastTick_;  // see PendingEvict::tick (M9.5-A)
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
          // A TICKET batch whose keep decision was made: a chunk no published
          // snapshot ever showed dirty is what procgen or the store already
          // reproduces, so it is not put (EvictSlots' re-derivability
          // argument, decided exactly). Forced before the decision, it keeps
          // everything — never lossy.
          if (p.keepHandle != 0 && !p.keepPending && i < p.keep.size() &&
              p.keep[i] == 0)
            continue;
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
          // THE SECOND EVICTION HOOK (M9.5-A). Same ordering rule as the
          // sentinel one (the Put moves `rle` away), and the tick is
          // `p.tick` - the tick EvictSlots ran on, not `lastTick_`. The
          // readback that produced these bytes is several ticks latent, and
          // a peer comparing tags has to see when the chunk LEFT, or a busy
          // machine would appear to hold newer data than the one that
          // actually edited the chunk.
          if (exchange_) exchange_->OnEvicted(p.items[i].wc, p.tick, rle);
          store_.Put(p.items[i].wc, std::move(rle));
          // The far-field edit index takes the SAME words, from the same
          // harvest — this is the one place the CPU ever sees an edited
          // chunk's content, and it is what lets a sieve refill put the
          // player's crater back after the chunk has left the window.
          if (p.items[i].edited) {
            if (e != 0u) {
              farEdits_.NoteUniformChunk(p.items[i].wc, e & kPtMatMask);
              farPlumes_.NoteUniformChunk(p.items[i].wc, e & kPtMatMask);
            } else {
              farEdits_.NoteChunk(p.items[i].wc,
                                  (const uint32_t*)(ptr + i * kChunkBytes));
              // The SAME words, at the same instant: this is where a chunk
              // that was on fire when the window left it becomes a plume the
              // renderer can still see (src/sim/farplumes.h).
              farPlumes_.NoteChunk(p.items[i].wc,
                                   (const uint32_t*)(ptr + i * kChunkBytes));
            }
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
    if (p.keepHandle != 0) {
      auto tk = ticketKeys_.find(World::PackChunkKey(it.wc));
      if (tk != ticketKeys_.end() && --tk->second == 0) ticketKeys_.erase(tk);
    }
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

// ---- THE STORE-HIT BRANCH, AS ONE FUNCTION (M9.3-C) -----------------------
//
// Verbatim the body FillSlots ran inline until 2026-09-21, lifted whole so
// that Stream::ReplaceChunk (the chunk-resync install door, stream.h) and the
// refill cannot drift apart. Eleven side effects, and the comments on each are
// the originals because the reasons did not change; what changed is that there
// is now a second caller and therefore exactly one copy.
//
// `words` is kChunkVol entries and `s` is World::SlotChunkIndex(wc). Nothing
// here submits: every GPU touch is a deferred queue write, ordered before the
// next submit, which is the guarantee both callers rely on.
void Stream::InstallChunkWords(uint32_t s, IVec3 wc, const uint32_t* words) {
  const uint32_t one = 1;
    // ---- store-hit classification (PLAN_page_table.md §3.5d) ------------
    //
    // The CPU has the decoded 16 KiB in `words` before it uploads, so it
    // knows for FREE — it is already looping over every word below to
    // compute occ/blockers — whether the chunk is all-air or all-one-word.
    // All-air or uniform => install the sentinel and SKIP THE 16 KiB UPLOAD
    // ENTIRELY, which is a bandwidth win on top of the memory win.
    //
    // This is also the ONE place UNIFORM discovery lives (§3.6, with commit
    // 0's measurement behind it): the paths that already hold the words get
    // demotion, and the tick path does not get a GPU uniformity scan.
    //
    // NOT for a ticket slot: a ticket holds a real page in every slot for its
    // whole life (tickets.h), because nothing materializes a ticket chunk on
    // demand — the mirror's N26 ring is window arithmetic — so a sentinel
    // there would turn the first write into a page fault.
    const uint32_t entry = world_->residency == World::Residency::Paged &&
                                   World::IsWindowSlot(s)
                               ? world_->pages->Classify(s, words)
                               : PageTable::kNeedsPage;
    if (entry != PageTable::kNeedsPage) {
      world_->pages->SetSentinel(s, entry);
    } else {
      // THE CPU SEAM (§2.1a): without translation this writes the decoded
      // RLE into ANOTHER chunk's page. EnsurePageForOverwrite allocates when
      // the slot is a sentinel — the same branch that classifies is the one
      // that allocates, so allocation and offset come from one place.
      const uint64_t dstOff = world_->pages->EnsurePageForOverwrite(s);
      ctx_->queue.WriteBuffer(world_->voxels, dstOff, words, kChunkBytes);
    }
    world_->pages->FlushTableWrites(ctx_->queue);
    // A store hit is a chunk the store thought worth keeping, and its words
    // are decoded right here — cheaper than waiting for it to be evicted
    // again, and it is what re-seeds the index for chunks that were only
    // ever loaded from disk in this session.
    farEdits_.NoteChunk(wc, words);
    // A store hit is a chunk coming BACK, so its plume entry is refreshed
    // rather than dropped: Build() is what drops it while the chunk is
    // resident, and refreshing here means the emitter is correct the moment
    // the window leaves it again.
    farPlumes_.NoteChunk(wc, words);
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
      const uint32_t w = words[li];
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
                            ((uint64_t)kNumSlots + (uint64_t)s * kSubOccStride) * 4,
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
}

// ---- Stream::ReplaceChunk (M9.3-C) ----------------------------------------
// The doc comment is in stream.h beside the declaration.
bool Stream::ReplaceChunk(IVec3 wc, const uint32_t* words) {
  if (!world_->ChunkInWindow(wc)) {
    // The window shifted between the peer's ChunkRequest and its reply. NOT a
    // silent drop: it is counted and the --frames net report names it, because
    // "the sync never arrived" and "the sync arrived for a chunk we no longer
    // hold" are different bugs and only the first one is a bug at all.
    replacesRefused_++;
    return false;
  }
  const uint32_t s = World::SlotChunkIndex(wc);
  // The demotion streak is a claim about how long this slot has looked
  // uniform, and the words are about to change. FillSlots resets it for the
  // whole plane at its top for the same reason; here it is one slot.
  if (world_->residency == World::Residency::Paged) {
    const std::vector<uint32_t> justThis{s};
    world_->pages->ResetStreaks(justThis);
  }
  InstallChunkWords(s, wc, words);
  // ...and the two things a REFILL does not do, because a refill is installing
  // what the store or procgen already agreed on and this is installing what
  // the PEER decided. Sticky-modified so eviction saves it instead of letting
  // procgen regenerate the pre-sync contents; quiet streak reset because the
  // contents just changed and the streak describes the contents.
  modified_[s] = 1;
  world_->NoteChunkTouched(wc);
  replacesApplied_++;
  return true;
}

// ---- THE INERT HOLD (M9.5-A) ---------------------------------------------
//
// Everything a refill does, MINUS the two things that would be wrong here:
// it installs no content (the content is somebody else's and has not arrived)
// and it does not wake (the CA must not run on air that is about to be
// replaced, and a chunk of pure sky has nothing to act anyway - the same
// act-set argument InstallChunkWords makes for its PT_EMPTY case).
//
// Both residency modes, because `--residency dense` is the only live
// differential oracle this engine has and a hold that showed different
// contents in the two modes would poison it. Paged installs the PT_EMPTY
// sentinel and frees the page; dense has no sentinel to install - its table
// is the identity map - so the air has to be real words.
void Stream::HoldSlotInert(uint32_t s, IVec3 wc) {
  (void)wc;
  if (awaitingRemote_.size() != kNumSlots) awaitingRemote_.assign(kNumSlots, 0);
  if (world_->residency == World::Residency::Paged) {
    world_->pages->SetSentinel(s, kPtEmpty);
    world_->pages->FlushTableWrites(ctx_->queue);
  } else {
    static const std::vector<uint32_t> kAir(kChunkVol, 0u);
    const uint64_t off = world_->PageOffsetOfSlot(s);
    if (off != World::kNoPage)
      ctx_->queue.WriteBuffer(world_->voxels, off, kAir.data(), kChunkBytes);
  }
  // Occupancy AND the sub-chunk mask, for the reason InstallChunkWords states
  // at the same two writes: a bit left set over a slot that holds nothing is
  // a chunk the raymarcher thinks is solid, and the demote classifier reads
  // the same word.
  const uint32_t zero = 0;
  ctx_->queue.WriteBuffer(world_->occupancy, (uint64_t)s * 4, &zero, 4);
  uint32_t sub[kSubOccStride] = {};
  ctx_->queue.WriteBuffer(world_->occupancy,
                          ((uint64_t)kNumSlots + (uint64_t)s * kSubOccStride) * 4,
                          sub, sizeof(sub));
  // ...and CLEAR both dirty pages rather than leaving whatever the chunk that
  // just left had. This is the "does not wake" half, and it has to be an
  // explicit clear: eviction does not clear dirty flags (the gen path relies
  // on genChunk doing it), so a held slot would otherwise inherit the departed
  // chunk's wake and dispatch the CA over air.
  ctx_->queue.WriteBuffer(world_->dirty[0], (uint64_t)s * 4, &zero, 4);
  ctx_->queue.WriteBuffer(world_->dirty[1], (uint64_t)s * 4, &zero, 4);
}

bool Stream::DeliverRemote(IVec3 wc, uint32_t tick,
                           const std::vector<uint32_t>& rle) {
  std::vector<uint32_t> words(kChunkVol);
  if (rle.size() < 2 || (rle.size() & 1u) ||
      !RleDecodeChunk(rle.data(), rle.size() / 2, words.data())) {
    // A malformed payload is a WIRE bug, not a world bug: refuse it and leave
    // the hold standing, so the slot stays air rather than becoming garbage.
    exchangeStats_.rejected++;
    return false;
  }
  // The store FIRST, and tagged. The point of the tag is that the next time
  // this chunk leaves and re-enters the window it comes back from the store
  // with no round trip at all, and that a later ChunkPut carrying an older
  // tick can be refused rather than applied (chunkstore.h).
  store_.Put(wc, rle, tick);
  // ...then the words, through the ONE install door. ReplaceChunk refuses a
  // chunk the window no longer holds and counts it; that is the race where
  // the reply lost to a shift, and it is why the store write above happens
  // unconditionally - the bytes are still worth keeping.
  if (!ReplaceChunk(wc, words.data())) {
    exchangeStats_.rejected++;
    if (world_->ChunkInWindow(wc)) {
      const uint32_t s = World::SlotChunkIndex(wc);
      if (s < awaitingRemote_.size()) awaitingRemote_[s] = 0;
    }
    return false;
  }
  const uint32_t s = World::SlotChunkIndex(wc);
  if (s < awaitingRemote_.size()) awaitingRemote_[s] = 0;
  exchangeStats_.delivered++;
  return true;
}

bool Stream::DeliverMiss(IVec3 wc) {
  if (!world_->ChunkInWindow(wc)) return false;  // the window outran the ask
  const uint32_t s = World::SlotChunkIndex(wc);
  if (s >= awaitingRemote_.size() || !awaitingRemote_[s]) return false;
  awaitingRemote_[s] = 0;
  // Queued, not generated here: procgen is a buffer write plus a dispatch
  // plus a submit, and this is called from whatever pumps the socket. Update
  // is the between-ticks point where a submit is already legal.
  genAfterMiss_.push_back(s);
  exchangeStats_.missed++;
  return true;
}

void Stream::FillSlots(const std::vector<uint32_t>& slots, bool deferWake,
                       bool ticket) {
  const double fT0 = PtNowMs();
  if (world_->residency == World::Residency::Paged)
    world_->pages->ResetStreaks(slots);
  std::vector<uint32_t> data(kChunkVol);
  std::vector<uint32_t> genSlots;
  for (uint32_t s : slots) {
    // A HOLD THE WINDOW OUTRAN (M9.5-A). This slot was waiting for a peer's
    // copy of the chunk it used to hold, and the shift has just given it a
    // different chunk: there is nothing left to install into, so the hold is
    // dropped here and counted. It is not an error - the reply, if it comes,
    // is refused by ReplaceChunk's residency test and counted there - but it
    // is the number that separates "the peer never answered" from "we moved".
    if (s < awaitingRemote_.size() && awaitingRemote_[s]) {
      awaitingRemote_[s] = 0;
      exchangeStats_.forgotten++;
    }
    modified_[s] = 0;
    // The slot is about to hold a DIFFERENT world chunk, so whatever quiet
    // streak it had belonged to the chunk that left (M9.3-A). Reset by SLOT
    // and not by world chunk: SlotToWorldChunk below still reports the new
    // one, but the streak is a property of the memory, not of the coordinate.
    world_->NoteSlotTouched(s);
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
    //
    // ...UNLESS a TICKET batch holds the chunk (ticketKeys_): a ticket the
    // shift released for overlapping this plane (Tickets::ReleaseOverlapping)
    // is not this shift's eviction, and its bytes are the only copy of the
    // chunk the plane is about to read. Forcing it keeps all 125 (stream.h).
    const uint64_t key = World::PackChunkKey(wc);
    if (!(s < shiftEvicted_.size() && shiftEvicted_[s]) || ticketKeys_.count(key))
      while (pendingChunks_.count(key))
        CompleteOldest(/*discard=*/false);
    const std::vector<uint32_t>* rle = store_.Get(wc);
    if (rle && RleDecodeChunk(rle->data(), rle->size() / 2, data.data())) {
      InstallChunkWords(s, wc, data.data());
    } else if (!ticket && exchange_ && !fillIgnoresExchange_ &&
               exchange_->Wanted(wc)) {
      // THE REFILL HOOK (M9.5-A). My store has never seen this chunk, but the
      // exchange knows somebody else has a MODIFIED copy of it - so procgen
      // would be the wrong answer, not merely a slow one: it would paint
      // pristine terrain over another player's edit and then, when the reply
      // arrived, visibly undo it.
      //
      // So the slot is held INERT instead of generated. It is NOT pushed to
      // genSlots, which is the whole point: no genChunk dispatch, no wake,
      // and a PT_EMPTY sentinel that costs no page.
      HoldSlotInert(s, wc);
      awaitingRemote_[s] = 1;
      exchangeStats_.held++;
      exchange_->Request(wc);
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
    // genChunk overwrites the WHOLE chunk of every slot in the list, so every
    // one of them needs a page before the dispatch — a kernel cannot allocate
    // (§3.5c at batch size = the genList count, which the shift plane already
    // bounds). No fill is queued: the kernel is about to write all 4,096 words.
    if (world_->residency == World::Residency::Paged) {
      for (uint32_t gs : genSlots) world_->pages->EnsurePageForOverwrite(gs);
      world_->pages->FlushTableWrites(ctx_->queue);
      // ---- THE genChunk PRECONDITION, CHECKED HERE (P3-E) -----------------
      // genChunk resolves through voxWordInChunk(genList[wg.x], ...), so every
      // slot in the list must hold a PAGE or all 4,096 of its stores are
      // dropped. EnsurePageForOverwrite one line above is supposed to be that
      // guarantee. If this fires, the CPU table lost the page; if it never
      // fires while the kernel still faults, the loss is on the GPU side of the
      // deferred table write. Those are different bugs and a fault count
      // cannot tell them apart.
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
      // Contributor (d) — the RefilledSlot calls — lives in ApplyGenVerdict,
      // where the post-genChunk occupancy is in hand: only the slots that can
      // ACT are declared, not the whole plane. Under the deferred wake that is
      // kWakeLatency ticks from now, which is exactly as early as it needs to
      // be — nothing dispatches the plane until the same call wakes it.
    }
    // The streamer's own TickParams write (below) is a SECOND per-tick input
    // the op record has to carry: a replay that did not know which slots were
    // regenerated on this tick would rebuild different planes and diverge on
    // the first window shift. Stashed here, folded into this tick's frame by
    // SubmitTick. Free unless a record or a replay is armed.
    // A TICKET fill is recorded as its TicketOp instead (tickets.h): stashing
    // its list here would overwrite the same tick's shift plane.
    if (!ticket) sandvox::opstream::NoteGenList(lastTick_, genSlots);
    // genList plus the column cache's chunk-column table, against the window
    // origin written into tickUBO below (Simulation::WriteGenList).
    sim_->WriteGenList(ctx_->queue, genSlots);
    TickParams tp{};
    tp.seed = seed_;
    tp.genCount = (uint32_t)genSlots.size();
    IVec3 o = world_->WindowOrigin();
    tp.origin[0] = o.x; tp.origin[1] = o.y; tp.origin[2] = o.z;
    // Fluid-lab slab (world.h kLabSlabY): a lab window shift must refill with
    // the SAME slab genColumn produced at startup, not default terrain.
    tp.labMode = World::LabWorld() ? 1u : 0u;
    // R1: publish the act verdict instead of acting on it (world.h's
    // genDeferWake, the genChunk tail in worldgen.wgsl).
    tp.genDeferWake = deferWake ? 1u : 0u;
    ctx_->queue.WriteBuffer(world_->tickUBO, 0, &tp, sizeof(tp));

    rhi::CommandEncoder enc = ctx_->device.CreateCommandEncoder();
    sim_->EncodeGenList(enc, (uint32_t)genSlots.size());
    ctx_->queue.Submit(enc.Finish());

    // genChunk has just overwritten these slots with PRISTINE procgen, which
    // un-does the authored edit layer exactly the way the far-field sieve
    // un-does a crater (see faredits.h). Re-queue every refilled chunk the
    // layer touches; the ops go out through the MutationQueue on the following
    // ticks. Cheap when there is no layer — QueueChunk returns on an empty map
    // before it hashes anything. ONLY the generated slots: a slot FillSlots
    // decoded from the store already holds whatever the layer became there,
    // and re-stamping it would undo the player's edits of it.
    //
    // Stays at tick T under the deferred wake: the ops are CPU mutations that
    // travel the MutationQueue and target chunks by WORLD coordinate, so they
    // are unaffected by whether the CA has been told about the plane yet.
    if (!sandvox::WorldEditLayer().Empty())
      for (uint32_t gs : genSlots)
        sandvox::WorldEditLayer().QueueChunk(world_->SlotToWorldChunk(gs), seed_);
    timing_.fillGenMs += PtNowMs() - fT1;

    // ---- THE READBACK, AND WHY IT NO LONGER WAITS ------------------------
    //
    // What the CPU wants out of the GPU here is two things, and only one of
    // them was ever urgent:
    //
    //   (a) the ACT SET — which generated chunks can act — because
    //       PageTable::Materialize must give their 26-neighbourhoods pages
    //       before the CA runs on them. Waking a chunk the mirror has not
    //       heard of is a voxStore into a sentinel, which common.wgsl drops
    //       silently: the streaming gate's 217 page faults.
    //   (b) the DEMOTE classification (sky -> PT_EMPTY, full -> UNIFORM /
    //       JITTER), which is bookkeeping and has always been allowed to lag.
    //
    // (a) was urgent only because the CA acted on the plane in the SAME tick,
    // and it was urgent expensively: MapReadDeferred borrows the fence of the
    // last submit, and a fence on one queue waits for everything queued before
    // it — the previous frame's render and GI passes, the previous ticks' CA,
    // this shift's eviction copies, and only then genChunk. Measured
    // 2026-09-03 under `--frames 600 --autofly-surface`: 33.06 ms of a
    // 34.80 ms shift, 618 shifts in 540 frames, 50% of frames over 33 ms.
    //
    // So the CA is told to wait instead. genChunk leaves the plane out of
    // dirtyIn and publishes its verdict to `genAct`; this queues the copy with
    // NO Wait and records what is owed; and Update polls the ticket
    // kWakeLatency ticks later, runs the identical CPU logic, and only then
    // wakes the act set. See the PendingShift block in stream.h.
    //
    // ONE COPY, TWO REGIONS. occupancy (128 KiB, needed for the demote
    // classification and for the "is any neighbour air" test that decides
    // whether a FULL chunk is inert) then genAct (one u32 per generated slot).
    // Both are read in both residency modes: dense skips the page-table half
    // of the completion but must take the WAKE at the same tick as paged, or
    // the two modes would not hash identically — and `--residency dense` is
    // the only live differential oracle this system has.
    const uint64_t occBytes = (uint64_t)kNumSlots * 4;
    const uint64_t actBytes = (uint64_t)genSlots.size() * 4;
    if (ticket) {
      // ---- A TICKET'S GEN: NO VERDICT, NO DEMOTE, NO READBACK -------------
      // genChunk woke its act set in-kernel (genDeferWake 0), every slot has
      // a page for the ticket's life (no sky demotion to undo), and the
      // page-table mirror needs no act set: nothing it materializes reaches a
      // ticket slot. What remains is the settled-skip latch's declaration —
      // the one waking path here that is not an Encode* entry point.
      sim_->NoteWakeAll();
    } else if (deferWake) {
      // genAct is sized to ONE PLANE, which is the only thing that defers.
      if (genSlots.size() > (size_t)kNChunk * kNChunk) {
        std::fprintf(stderr,
                     "stream: deferred gen list of %zu exceeds genAct (%u); "
                     "waking in-kernel instead\n",
                     genSlots.size(), (uint32_t)(kNChunk * kNChunk));
        std::abort();
      }
      PendingShift ps;
      ps.staging = AcquireShiftStaging();
      ps.genSlots = genSlots;
      ps.stale.assign(genSlots.size(), 0);
      ps.tick = lastTick_;
      rhi::CommandEncoder oenc = ctx_->device.CreateCommandEncoder();
      oenc.CopyTracked(pass::Buf::Occupancy, world_->occupancy, 0, ps.staging,
                       0, occBytes);
      oenc.CopyTracked(pass::Buf::GenAct, world_->genAct, 0, ps.staging,
                       occBytes, actBytes);
      ctx_->queue.Submit(oenc.Finish());
      ps.map = rhi::MapReadDeferred(ctx_->device, ps.staging, 0,
                                    occBytes + actBytes);
      pendingShifts_.push_back(std::move(ps));
      // BACKSTOP, not a throughput knob (IssueDemoteCopies' shape). Entries
      // retire on a TICK deadline, so a caller that drives Update without
      // advancing `tick` would queue them forever. The steady state is at
      // most one entry per tick for the kWakeLatency ticks each one lives;
      // twice that is slack. It is a kWakeLatency quantity, not a readback-
      // ring one: entries retire on the WAKE deadline, not on a fence.
      constexpr size_t kMaxPendingShifts = 2 * (size_t)kWakeLatency;
      while (pendingShifts_.size() > kMaxPendingShifts) {
        PendingShift& front = pendingShifts_.front();
        front.map.Wait();
        CompleteShift(front, lastTick_);
        shiftStagingPool_.push_back(front.staging);
        pendingShifts_.pop_front();
      }
    } else {
      // ---- THE SYNCHRONOUS PATH (ReloadWindow only) ---------------------
      // A load or a regen refills the WHOLE window and genChunk woke the slots
      // in-kernel, so the mirror has to learn the act set on this tick or the
      // very first CA dispatch faults. There is no frame to protect here and
      // no plane-sized bound to respect, so this keeps the old fence.
      const double dT0 = PtNowMs();
      std::vector<uint32_t>& occ = genOccScratch_;
      if (occ.size() != kNumSlots) occ.assign(kNumSlots, 0);
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
          std::memcpy(occ.data(), omap.Data(), (size_t)kNumSlots * 4);
        else
          std::fill(occ.begin(), occ.end(), 0u);  // failed: fall back to all
        omap.Unmap();
        // No `act`: the kernel already wrote dirtyIn/dirtyOut itself.
        ApplyGenVerdict(genSlots, {}, occ, occValid, {}, lastTick_, lastTick_);
      }
      timing_.demoteMs += PtNowMs() - dT0;
    }
  }
  // Whatever dissolved mass is kept for the chunks now in these slots goes
  // back onto the GPU (after the voxels, so the solvent it rides is there).
  RestoreSolutes(slots);
  // Far landings parked on a chunk that just became resident are re-thrown
  // (P2, tickets.h): the window arriving is one of the two ways a landing
  // materializes; a ticket activating over it is the other.
  for (uint32_t s : slots) tickets_.OnChunkResident(world_->SlotToWorldChunk(s));
}

// ---- the deferred wake's second half -------------------------------------

rhi::Buffer Stream::AcquireShiftStaging() {
  if (!shiftStagingPool_.empty()) {
    rhi::Buffer b = shiftStagingPool_.back();
    shiftStagingPool_.pop_back();
    return b;
  }
  return CreateBuffer(
      ctx_->device, ((uint64_t)kNumSlots + (uint64_t)kNChunk * kNChunk) * 4,
      rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "shiftVerdict");
}

void Stream::InvalidatePendingSlots(const std::vector<uint32_t>& slots) {
  if (pendingShifts_.empty()) return;
  // A plane is 1,024 of 32,768 slots and the intersection with another axis's
  // plane is 32 of them, so a membership bitmap beats a per-entry sort.
  std::vector<uint8_t>& mark = shiftMark_;
  if (mark.size() != kNumSlots) mark.assign(kNumSlots, 0);
  for (uint32_t s : slots) mark[s] = 1;
  for (PendingShift& ps : pendingShifts_)
    for (size_t i = 0; i < ps.genSlots.size(); i++)
      if (mark[ps.genSlots[i]]) ps.stale[i] = 1;
  for (uint32_t s : slots) mark[s] = 0;  // left all-zero for the next call
}

void Stream::DiscardPendingShifts() {
  for (PendingShift& ps : pendingShifts_) {
    ps.map.Wait();  // release the fence borrow before the buffer goes back
    ps.map.Unmap();
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
  const uint64_t occBytes = (uint64_t)kNumSlots * 4;
  std::vector<uint32_t>& occ = genOccScratch_;
  std::vector<uint32_t>& act = genActScratch_;
  if (occ.size() != kNumSlots) occ.assign(kNumSlots, 0);
  act.assign(ps.genSlots.size(), 0u);
  bool occValid = false;
  if (ps.map.Succeeded() && ps.map.Data()) {
    const uint8_t* base = (const uint8_t*)ps.map.Data();
    std::memcpy(occ.data(), base, (size_t)kNumSlots * 4);
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
  ApplyGenVerdict(ps.genSlots, ps.stale, occ, occValid, act, ps.tick, tick);
  timing_.demoteMs += PtNowMs() - dT0;
  if (PtDbg())
    std::printf("[pt-time] shift wake T+%u: gen=%zu %.2f ms\n",
                tick - ps.tick, ps.genSlots.size(), PtNowMs() - dT0);
}

void Stream::ApplyGenVerdict(const std::vector<uint32_t>& genSlots,
                             const std::vector<uint8_t>& stale,
                             const std::vector<uint32_t>& occ, bool occValid,
                             const std::vector<uint32_t>& act,
                             uint32_t genTick, uint32_t tick) {
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
    // Bit 0 of the verdict word; the rest is the page-table class, used by
    // the demote pass below. A failed map fills the words with 1 (act, no
    // published class), which is "wake everything, demote nothing new".
    const bool canAct = enactWake && (act[i] & kGenVerdictAct) != 0u;
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
    // genChunk dispatch whose own in-kernel count this is: genCellIn returns
    // packVox(mat, state, STAMP_NEVER), so it can set neither bit 31
    // (kCellOpIfAir) nor a stain bit, and `nonAir == 0` on a freshly generated
    // slot does not merely suggest PT_EMPTY's content, it IS PT_EMPTY's
    // content.
    std::vector<uint32_t> cands;
    cands.reserve(genSlots.size());
    uint32_t skyDemoted = 0, skyHeld = 0, fullDemoted = 0;
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
      // of air and matter. The FULL case needs Classify's exact-word rule —
      // a sentinel must reproduce the resident content bit-exactly, which a
      // count cannot decide — and gets it from the kernel's verdict below
      // when the chunk is provably untouched, or from a word copy otherwise.
      if (nonAir != 0u && nonAir != kChunkVol) continue;
      // ---- THE KERNEL'S VERDICT FOR A FULL CHUNK -----------------------
      //
      // genChunk already ran Classify's three tests over the words it wrote
      // (worldgen.wgsl, world.h kGenVerdict*), so a FULL chunk nothing has
      // touched since generation needs no word copy: its words now ARE the
      // words the verdict describes. "Nothing has touched it" is the sky
      // share's guard above, plus the write-reach clock — every writer marks
      // reachTick_ (Materialize for the CA and op rings,
      // EnsurePageForOverwrite for the CPU seam), so ReachTick < genTick
      // proves no write since the plane was generated. A chunk that fails
      // any of them takes the word copy exactly as before; so does one whose
      // verdict was not published (a failed map fills act with 1s).
      const uint32_t v = i < act.size() ? act[i] : 0u;
      if ((v & kGenVerdictValid) != 0u &&
          !world_->pages->CpuDirty().Has(gs) &&
          !(snap.valid && snap.dirtyFlags[gs]) &&
          world_->pages->ReachTick(gs) < genTick) {
        const uint32_t e = world_->pages->ClassifyGenVerdict(v, seed_);
        if (e != PageTable::kNeedsPage) {
          world_->pages->SetSentinel(gs, e);
          fullDemoted++;
        }
        continue;
      }
      cands.push_back(gs);
    }
    if (skyDemoted || fullDemoted) world_->pages->FlushTableWrites(ctx_->queue);

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
      std::printf("[pt-time] shift demote: gen=%zu cands=%zu sky=%u held=%u "
                  "full-by-verdict=%u\n",
                  genSlots.size(), cands.size(), skyDemoted, skyHeld,
                  fullDemoted);
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
                               uint32_t tick, uint32_t generation) {
  // Backstop, not a throughput knob: 32 in-flight batches is 128 MiB of
  // staging and far beyond the ~4-8 a saturated flight keeps queued. Hitting
  // it means the GPU is pathologically behind; forcing the oldest batch
  // through (Wait + harvest) is the bounded-memory answer, and the harvest's
  // own retry logic keeps correctness. TWICE the readback ring's depth: a
  // batch is outstanding for as long as the ticks in flight ahead of it
  // (World::kReadbackSlots is exactly that count), so a backlog past two
  // ring-depths is the GPU being behind, not the pipeline being deep.
  constexpr size_t kMaxPendingDemotes = 2 * (size_t)World::kReadbackSlots;
  while (demotes_.size() >= kMaxPendingDemotes) {
    demotes_.front().map.Wait();
    HarvestDemotes(tick);
  }
  for (size_t off = 0; off < slots.size(); off += kEvictBatch) {
    const size_t n = std::min(kEvictBatch, slots.size() - off);
    PendingDemote d;
    d.staging = AcquireStaging();
    d.copyTick = tick;
    d.generation = generation;
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
  //
  // ---- AND WHY AGE ALONE RETRIED FOREVER --------------------------------
  //
  // A batch is only harvested once its map is Ready, i.e. once the GPU has
  // executed it, and the pipeline lets the GPU run up to kSnapshotLatency /
  // kPagedSnapshotMaxGap ticks behind the CPU. A copy issued at Update(T) is
  // therefore only GUARANTEED ready around T + 5 — past the 3-tick bound. On a
  // GPU-bound stretch (several ticks per frame, the GPU at the lag ceiling)
  // every batch came back "stale", was re-copied at the current tick, came
  // back stale again, and nothing ever classified: 4 MiB of copies per batch
  // per tick, no demotions.
  //
  // So age is no longer the only way in. THE WRITE-REACH CLOCK
  // (PageTable::ReachTick) answers the question the age bound approximates:
  // every writer that could reach a slot stamps it — Materialize for the CA,
  // op, particle and fluid rings, EnsurePageForOverwrite for the CPU seam —
  // so `ReachTick(s) < copyTick` PROVES nothing has written the slot since
  // the copy was issued (the copy is submitted before tick copyTick's work,
  // and a write at copyTick stamps copyTick itself). Such a slot's bytes are
  // current however late they are harvested. The age bound stays as the
  // other door, and a slot that passes neither is re-copied at most
  // kDemoteMaxRetries times — after that it keeps its page (bounded: the
  // slot scrolls out eventually, and an all-air one is freed by the
  // hysteresis in ConsumeOccupancy) rather than paying a copy per tick.
  constexpr uint32_t kDemoteFreshTicks = 3;
  constexpr uint32_t kDemoteMaxRetries = 4;
  const bool dbg = PtDbg();
  uint32_t demoted = 0, retried = 0, harvested = 0, dropped = 0;
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
        const bool untouched = world_->pages->ReachTick(s) < d.copyTick;
        if (!fresh && !untouched) {
          if (d.generation < kDemoteMaxRetries) {
            retry.push_back(s);
            retryKeys.push_back(d.keys[i]);
          } else {
            dropped++;
          }
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
        IssueDemoteCopies(retry, retryKeys, tick, d.generation + 1);
      }
    } else {
      d.map.Unmap();
    }
    stagingPool_.push_back(d.staging);
  }
  if (demoted) world_->pages->FlushTableWrites(ctx_->queue);
  if (dbg && (harvested || retried || dropped))
    std::printf("[pt-time] demote harvest: batches=%u demoted=%u retried=%u "
                "dropped=%u queued=%zu (memcpy %.2f ms, classify %.2f ms)\n",
                harvested, demoted, retried, dropped, demotes_.size(), cpyMs,
                clsMs);
}

void Stream::FoldSnapshot() {
  const WorldSnapshot& snap = world_->Snap();
  if (!snap.valid) return;
  // The fold itself is unconditional: eviction reads modified_, so what goes
  // in here must stay a pure function of the published snapshot (and of the
  // edit layer's op record for that same submit, below).
  //
  // THE EDIT LAYER IS WORLDGEN (sim/worldedit.h, map-overhaul P7). Its ops
  // wake the chunks they land in, and a wake is a dirty report. Where the
  // layer's ops landed on this snapshot's submit and the ONLY reason the chunk
  // reported was DIRTY_R_MUTATE, the report is the layer's own and is not a
  // modification: genChunk + the layer re-derive that chunk on re-entry. Any
  // CA consequence (a grain that falls, a stain that spreads) sets its own
  // reason bit on its own tick and is folded like every other.
  if (const std::vector<uint32_t>* lay =
          sandvox::WorldEditLayer().AppliedSlotsAt(snap.submitSeq)) {
    for (uint32_t i = 0; i < kNumSlots; i++) {
      if (!snap.dirtyFlags[i]) continue;
      if (snap.dirtyFlags[i] == World::kDirtyMutateOnly &&
          std::binary_search(lay->begin(), lay->end(), i)) {
        layerWakesIgnored_++;
        continue;
      }
      modified_[i] |= snap.dirtyFlags[i];
    }
  } else {
    for (uint32_t i = 0; i < kNumSlots; i++) modified_[i] |= snap.dirtyFlags[i];
  }
  if (!trackOk_) return;
  const uint32_t ep = world_->SnapshotEpoch();
  if (!trackEpochKnown_) {
    trackEpoch_ = ep;
    trackEpochKnown_ = true;
  } else if (ep != trackEpoch_) {
    trackOk_ = false;
    trackWhy_ = "the snapshot pipeline was reset behind the stream (a world "
                "regenerated or a tick rewind without Stream::OnRegen/ReloadWindow)";
    return;
  }
  // Already counted, or from before the last wholesale refill (a window
  // re-pull that did not invalidate the pipeline): folded above, harmlessly
  // over-inclusive, but it proves nothing about the new window.
  if (snap.submitSeq <= trackSeq_) return;
  if (snap.submitSeq != trackSeq_ + 1) {
    trackOk_ = false;
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "ticks %u..%u were encoded but their dirty flags never reached "
                  "the modified set (FoldSnapshot not called for them)",
                  trackSeq_ + 1, snap.submitSeq - 1);
    trackWhy_ = buf;
    return;
  }
  trackSeq_ = snap.submitSeq;
}

void Stream::ResetSaveTracking() {
  trackOk_ = true;
  trackWhy_.clear();
  trackEpochKnown_ = false;
  // Every tick encoded so far belongs to the window being replaced.
  trackSeq_ = world_ ? world_->TicksEncoded() : 0u;
}

bool Stream::BuildSaveMask(std::vector<uint8_t>& mask, FlushReport& rep) {
  if (!trackOk_) {
    rep.why = trackWhy_.empty() ? "the modified set has not been tracked since "
                                  "the window was filled"
                                : trackWhy_;
    return false;
  }
  const uint32_t ep = world_->SnapshotEpoch();
  if (!trackEpochKnown_) {
    // Nothing folded since the refill. Adopting here is only sound if no tick
    // ran in between, which the seq test below checks.
    trackEpoch_ = ep;
    trackEpochKnown_ = true;
  } else if (ep != trackEpoch_) {
    rep.why = "the snapshot pipeline was reset since the last fold (a world "
              "regenerated behind the stream)";
    return false;
  }
  // THE TAIL. Every tick encoded so far has a readback in flight or parsed;
  // wait for the in-flight ones (bounded by the ring: each wait retires one
  // slot and nothing here arms another). This is the save path, off the frame
  // loop, and it already blocks on DrainEvictions below — one more fence of
  // the same kind is not a new class of stall.
  for (int i = 0; i < World::kReadbackSlots + 1 &&
                  world_->ReadbackPendingAtOrBefore(0xFFFFFFFFu);
       i++)
    if (!ctx_->WaitOldestPendingMap()) break;
  ctx_->ProcessEvents();
  if (world_->ReadbackPendingAtOrBefore(0xFFFFFFFFu)) {
    rep.why = "a snapshot readback did not land";
    return false;
  }
  mask = modified_;
  uint32_t seq = trackSeq_;
  for (const WorldSnapshot& sn : world_->DeliveredUnpublished()) {
    if (!sn.valid || sn.submitSeq <= trackSeq_) continue;
    if (sn.submitSeq != seq + 1) {
      char buf[128];
      std::snprintf(buf, sizeof buf,
                    "snapshot tail has a hole: expected seq %u, found %u", seq + 1,
                    sn.submitSeq);
      rep.why = buf;
      return false;
    }
    // The same edit-layer exemption FoldSnapshot makes (see there).
    const std::vector<uint32_t>* lay =
        sandvox::WorldEditLayer().AppliedSlotsAt(sn.submitSeq);
    for (uint32_t i = 0; i < kNumSlots; i++) {
      if (sn.dirtyFlags[i] && !mask[i]) {
        if (lay && sn.dirtyFlags[i] == World::kDirtyMutateOnly &&
            std::binary_search(lay->begin(), lay->end(), i))
          continue;
        mask[i] = 1;
        rep.tailOnly++;
      }
    }
    seq = sn.submitSeq;
    rep.tailTicks++;
  }
  if (seq != world_->TicksEncoded()) {
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "ticks %u..%u have no dirty flags in the mask (encoded %u, "
                  "covered through %u)",
                  seq + 1, world_->TicksEncoded(), world_->TicksEncoded(), seq);
    rep.why = buf;
    return false;
  }
  return true;
}

Stream::FlushReport Stream::FlushResident(bool forceFull) {
  FlushReport rep;
  std::vector<uint32_t> slots(kNumChunks);
  for (uint32_t i = 0; i < kNumChunks; i++) slots[i] = i;
  // ...and every LIVE TICKET's slots (docs/PLAN_chunk_tickets.md §2.4): a
  // ticket is resident world the window does not cover, and a save that
  // skipped it would lose whatever settled there. The ticket keeps running;
  // a load does not restore it (tickets are dropped on load), but its chunks
  // come back from the store when the window or a new ticket reaches them.
  for (uint32_t s = kNumChunks; s < kNumSlots; s++)
    if (world_->TicketSlotLive(s)) slots.push_back(s);
  std::vector<uint8_t> mask;
  if (forceFull) {
    rep.why = "full flush forced by the caller";
  } else {
    rep.delta = BuildSaveMask(mask, rep);
  }
  if (rep.delta) {
    for (uint32_t i = 0; i < kNumChunks; i++) rep.stored += mask[i] != 0;
    rep.skipped = kNumChunks - rep.stored;
    EvictSlots(slots, /*filter=*/false, &mask);
  } else {
    rep.tailOnly = 0;
    rep.stored = kNumChunks;
    EvictSlots(slots, /*filter=*/false);
  }
  DrainEvictions();  // a save wants the store complete NOW
  return rep;
}

void Stream::ReloadWindow(IVec3 origin) {
  // in-flight evictions belong to the world being replaced
  DrainEvictions(/*discard=*/true);
  DiscardDemotes();  // same: old-world bytes must never classify the new one
  DiscardPendingShifts();  // and so do any un-enacted shift verdicts
  // ...and the tickets: the evictions they would release into were just
  // discarded with the rest, so they are dropped, not released (tickets.h).
  DropTickets();
  // The solute layer's slots describe the window being replaced: keep what
  // they hold (a teleport must not delete the salt it leaves behind), then
  // clear the layer. The refill below restores whatever is kept for the
  // chunks of the NEW window. (LoadWorld resets the layer BEFORE calling
  // this, so for a load the capture finds nothing of the old world.)
  CaptureResidentSolutes();
  world_->ResetSolutes(ctx_->queue);
  world_->SetWindowOrigin(origin);
  modified_.assign(kNumSlots, 0);
  // Every slot is refilled below from the store or genChunk, so the delta
  // save's coverage proof restarts with the window (FlushResident).
  ResetSaveTracking();
  // Holds and miss-requeues belong to the window being replaced (M9.5-A). The
  // refill below re-asks the exchange for every slot under the NEW origin, so
  // any chunk still genuinely wanted is held again on the way through; not
  // clearing here would leave a stale hold on a slot the refill then fills.
  awaitingRemote_.assign(kNumSlots, 0);
  genAfterMiss_.clear();
  std::vector<uint32_t> slots(kNumChunks);
  for (uint32_t i = 0; i < kNumChunks; i++) slots[i] = i;
  // deferWake=false: this is the WHOLE window (32,768 slots, far past genAct's
  // one-plane size), it is not on a frame path worth protecting, and a load
  // must be simulable on the tick it lands rather than two ticks later.
  FillSlots(slots, /*deferWake=*/false);
}

uint32_t Stream::RegenerateChunks(const std::vector<IVec3>& chunks, uint32_t* dropped) {
  // See stream.h. Store first, for every listed chunk, resident or not: an
  // eviction still in flight for one of them is completed so its bytes land
  // (and are then dropped) rather than landing AFTER the drop and bringing
  // the old house back on the next re-entry.
  uint32_t drop = 0;
  std::vector<uint32_t> slots;
  std::vector<uint8_t> seen(kNumSlots, 0);
  for (const IVec3& wc : chunks) {
    while (pendingChunks_.count(World::PackChunkKey(wc))) CompleteOldest(/*discard=*/false);
    if (store_.Erase(wc)) drop++;
    if (!world_->ChunkInWindow(wc)) continue;
    const uint32_t s = World::SlotChunkIndex(wc);
    if (s >= kNumSlots || seen[s]) continue;
    // A slot held for a peer's copy keeps waiting for it (M9.5-A): the
    // authority's bytes are the truth there, not our generator.
    if (s < awaitingRemote_.size() && awaitingRemote_[s]) continue;
    seen[s] = 1;
    slots.push_back(s);
  }
  if (dropped) *dropped = drop;
  // Batched like SubmitWorldgen: one batch's transient page demand is what
  // the pool's headroom is sized for (world.h kWorldgenBatch).
  fillIgnoresExchange_ = true;
  for (size_t i = 0; i < slots.size(); i += kWorldgenBatch) {
    const size_t n = std::min<size_t>(kWorldgenBatch, slots.size() - i);
    std::vector<uint32_t> batch(slots.begin() + (ptrdiff_t)i, slots.begin() + (ptrdiff_t)(i + n));
    FillSlots(batch, /*deferWake=*/false);
  }
  fillIgnoresExchange_ = false;
  return (uint32_t)slots.size();
}

// ============================================================================
// THE SOLUTE LAYER'S HALF OF STREAMING (docs/PLAN_solutes.md §7.1)
//
// The GPU owns the live layer and its page indices (world.h kSol*), so the
// CPU never addresses a solute page: both doors are GPU passes over a slot
// list, recorded in their own command buffers between ticks, exactly where
// the voxel eviction and refill run.
//
//   EvictSolutes   before a shift moves the origin: solEvict copies each
//                  listed slot's entry (and page) into the staging, frees the
//                  page and empties the slot; the staging is read back
//                  asynchronously (the voxel eviction's pattern) and harvested
//                  into solutes_ keyed by the chunk that LEFT.
//   RestoreSolutes after the refill: every refilled slot whose chunk solutes_
//                  holds gets its entry/page back through solRestore, and the
//                  CPU copy is dropped -- the GPU owns it again.
//
// A chunk that leaves and comes straight back (the player doubling back
// inside the map latency) forces its eviction to complete first, so the
// restore always sees the newest bytes.
// ============================================================================
void Stream::DiscardSoluteState() {
  while (!solPending_.empty()) {
    PendingSolEvict& p = solPending_.front();
    p.map.Wait();
    p.map.Unmap();
    solStagingPool_.push_back(p.staging);
    solPending_.pop_front();
  }
  solPendingKeys_.clear();
  solutes_.clear();
  solLost_ = 0;
}

void Stream::EvictSolutes(const std::vector<uint32_t>& slots) {
  if (slots.empty() || !sim_ || !ctx_) return;
  // IN BATCHES OF kSolEvictRecords SLOTS, one pass + staging each. A shift
  // evicts a whole plane (up to kSolMEvictMax = 1,024 slots) and the staging
  // holds kSolEvictRecords records: past that, WHICH slots won a record was
  // decided by solEvict's atomicAdd order -- a scheduling-dependent loss of
  // solute (rule 1), not merely a counted one. A batch can never hold more
  // solute-carrying slots than it has records, so nothing is refused.
  const uint32_t total = std::min<uint32_t>((uint32_t)slots.size(), kSolMEvictMax);
  for (uint32_t at = 0; at < total; at += kSolEvictRecords)
    EvictSoluteBatch(slots.data() + at, std::min(kSolEvictRecords, total - at));
}

void Stream::EvictSoluteBatch(const uint32_t* slots, uint32_t n) {
  PendingSolEvict p;
  p.slots.assign(slots, slots + n);
  p.keys.reserve(n);
  for (uint32_t i = 0; i < n; i++)
    p.keys.push_back(World::PackChunkKey(world_->SlotToWorldChunk(p.slots[i])));
  static const uint32_t kZeroHdr[kSolStageHdrWords] = {};
  ctx_->queue.WriteBuffer(world_->solMeta, (uint64_t)kSolMEvictList * 4, p.slots.data(),
                          (uint64_t)n * 4);
  ctx_->queue.WriteBuffer(world_->solStage, 0, kZeroHdr, sizeof kZeroHdr);
  if (!solStagingPool_.empty()) {
    p.staging = solStagingPool_.back();
    solStagingPool_.pop_back();
  } else {
    p.staging = CreateBuffer(ctx_->device, (uint64_t)kSolStageWords * 4,
                             rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                             "solEvictStaging");
  }
  rhi::CommandEncoder enc = ctx_->device.CreateCommandEncoder();
  sim_->EncodeSoluteEvict(enc, n);
  enc.CopyTracked(pass::Buf::SolStage, world_->solStage, 0, p.staging, 0,
                  (uint64_t)kSolStageWords * 4);
  ctx_->queue.Submit(enc.Finish());
  p.map = rhi::MapReadDeferred(ctx_->device, p.staging, 0, (uint64_t)kSolStageWords * 4);
  for (uint64_t k : p.keys) solPendingKeys_[k]++;
  solPending_.push_back(std::move(p));
}

void Stream::HarvestSoluteEvicts(bool wait) {
  while (!solPending_.empty()) {
    PendingSolEvict& p = solPending_.front();
    if (!wait && !p.map.Ready()) break;
    p.map.Wait();
    const uint32_t* w = p.map.Succeeded() ? (const uint32_t*)p.map.Data() : nullptr;
    if (w) {
      // One sequential copy out of write-combined memory, of only the records
      // the pass wrote (CompleteOldest's bounce, for the same reason).
      const uint32_t count = std::min(w[0], kSolEvictRecords);
      const uint32_t refused = w[1];
      std::vector<uint32_t> buf(kSolStageHdrWords + (size_t)count * kSolRecWords);
      std::memcpy(buf.data(), w, buf.size() * 4);
      // Every chunk that LEFT forgets any older copy first: one that left
      // carrying nothing must not come back with salt a previous visit (or a
      // save's capture) recorded for it.
      for (uint64_t k : p.keys) solutes_.erase(k);
      for (uint32_t r = 0; r < count; r++) {
        const uint32_t* rec = &buf[kSolStageHdrWords + (size_t)r * kSolRecWords];
        const uint32_t slot = rec[0] & 0xFFFFFFu;
        uint64_t key = 0;
        bool found = false;
        for (size_t i = 0; i < p.slots.size(); i++)
          if (p.slots[i] == slot) { key = p.keys[i]; found = true; break; }
        if (!found) continue;
        SoluteChunk sc;
        sc.entry = rec[1];
        sc.stall = rec[0] >> 24;
        if (sc.entry & kSolPageBit) sc.words.assign(rec + 2, rec + 2 + kSolWordsPerPage);
        solutes_[key] = std::move(sc);
      }
      if (refused) {
        solLost_ += refused;
        std::fprintf(stderr,
                     "solute: %u evicted chunks did not fit the %u-record "
                     "staging; their dissolved mass is lost (world.h "
                     "kSolEvictRecords)\n",
                     refused, kSolEvictRecords);
      }
    }
    p.map.Unmap();
    for (uint64_t k : p.keys) {
      auto it = solPendingKeys_.find(k);
      if (it != solPendingKeys_.end() && --it->second == 0) solPendingKeys_.erase(it);
    }
    solStagingPool_.push_back(p.staging);
    solPending_.pop_front();
  }
}

void Stream::RestoreSolutes(const std::vector<uint32_t>& slots) {
  if (!sim_ || !ctx_) return;
  HarvestSoluteEvicts(/*wait=*/false);
  if (solutes_.empty() && solPending_.empty()) return;  // nothing kept: the common case
  std::vector<uint32_t> recs;
  uint32_t n = 0;
  auto flush = [&] {
    if (n == 0) return;
    ctx_->queue.WriteBuffer(world_->solStage, (uint64_t)kSolStageHdrWords * 4, recs.data(),
                            recs.size() * 4);
    rhi::CommandEncoder enc = ctx_->device.CreateCommandEncoder();
    sim_->EncodeSoluteRestore(enc, n);
    ctx_->queue.Submit(enc.Finish());
    recs.clear();
    n = 0;
  };
  for (uint32_t s : slots) {
    const uint64_t key = World::PackChunkKey(world_->SlotToWorldChunk(s));
    if (solPendingKeys_.count(key)) HarvestSoluteEvicts(/*wait=*/true);
    auto it = solutes_.find(key);
    if (it == solutes_.end()) continue;
    const SoluteChunk& sc = it->second;
    const size_t at = recs.size();
    recs.resize(at + kSolRecWords, 0u);
    recs[at] = s | (std::min<uint32_t>(sc.stall, 255u) << 24);
    recs[at + 1] = sc.entry;
    if ((sc.entry & kSolPageBit) && sc.words.size() == kSolWordsPerPage)
      std::memcpy(&recs[at + 2], sc.words.data(), kSolWordsPerPage * 4);
    else if (sc.entry & kSolPageBit)
      recs[at + 1] = 0u;   // a malformed page record restores as EMPTY
    solutes_.erase(it);
    if (++n == kSolEvictRecords) flush();
  }
  flush();
}

void Stream::CaptureResidentSolutes() { CaptureResidentSolutes(solutes_); }

void Stream::CaptureResidentSolutes(SoluteMap& into) {
  if (!ctx_ || !world_) return;
  HarvestSoluteEvicts(/*wait=*/true);
  std::vector<uint32_t> table(kNumSlots, 0u);
  rhi::ReadbackBlocking(ctx_->device, ctx_->queue, world_->solTable, 0, table.data(),
                        table.size() * 4, "solCaptureTable");
  bool any = false, pages = false;
  for (uint32_t s = 0; s < kNumChunks; s++) {
    any = any || table[s] != 0u;
    pages = pages || (table[s] & kSolPageBit) != 0u;
  }
  if (!any) return;
  std::vector<uint32_t> stall(kNumSlots, 0u);
  rhi::ReadbackBlocking(ctx_->device, ctx_->queue, world_->solMeta,
                        (uint64_t)kSolMStall * 4, stall.data(), stall.size() * 4,
                        "solCaptureStall");
  std::vector<uint32_t> pool;
  if (pages) {
    pool.resize((size_t)kSolutePoolPages * kSolWordsPerPage);
    rhi::ReadbackBlocking(ctx_->device, ctx_->queue, world_->solPool, 0, pool.data(),
                          pool.size() * 4, "solCapturePool");
  }
  for (uint32_t s = 0; s < kNumChunks; s++) {
    const uint32_t e = table[s];
    if (e == 0u) continue;
    SoluteChunk sc;
    sc.entry = e;
    sc.stall = stall[s];
    if (e & kSolPageBit) {
      const size_t base = (size_t)(e & kSolPageMask) * kSolWordsPerPage;
      sc.words.assign(pool.begin() + base, pool.begin() + base + kSolWordsPerPage);
    }
    into[World::PackChunkKey(world_->SlotToWorldChunk(s))] = std::move(sc);
  }
}

// ============================================================================
// CHUNK TICKETS: the four doors (src/sim/tickets.h, docs/PLAN_chunk_tickets.md)
//
// Tickets decides; these execute. Every one runs BETWEEN ticks (from
// Tickets::Tick, inside Stream::Update or TicketTick), so each submit and
// deferred write lands before the tick that first sees the new table.
// ============================================================================
void Stream::TicketTick(uint32_t tick) {
  if (!ctx_ || !world_) return;
  lastTick_ = tick;
  tickets_.Tick(tick);
}

void Stream::DropTickets() {
  if (ctx_) tickets_.DropAll(ctx_->queue);
}

void Stream::FillTicketSlots(const std::vector<uint32_t>& slots) {
  if (slots.empty()) return;
  // FillSlots' own store-hit and gen branches (one implementation of a
  // refill), with `ticket` set: no exchange hold, no sentinel, no verdict.
  FillSlots(slots, /*deferWake=*/false, /*ticket=*/true);
}

uint64_t Stream::EvictTicketSlots(const std::vector<uint32_t>& slots) {
  if (slots.empty()) return 0;
  // The dissolved mass leaves with the chunks, read back like a shift's.
  EvictSolutes(slots);
  // ALL of them, unfiltered: the keep decision needs snapshots that have not
  // been published yet (tickets.h, THE RELEASE IS TWO-PHASE), and a copy taken
  // later would see a slot the next ticket may already be refilling. One
  // batch: a ticket is 125 slots, under kEvictBatch.
  const uint64_t handle = ++evictHandleSeq_;
  for (size_t off = 0; off < slots.size(); off += kEvictBatch) {
    const size_t n = std::min(kEvictBatch, slots.size() - off);
    PendingEvict p;
    p.staging = AcquireStaging();
    p.tick = lastTick_;
    p.keepHandle = handle;
    p.keepPending = true;
    p.items.reserve(n);
    rhi::CommandEncoder enc = ctx_->device.CreateCommandEncoder();
    for (size_t i = 0; i < n; i++) {
      const uint32_t s = slots[off + i];
      const IVec3 wc = world_->SlotToWorldChunk(s);
      // `edited` true: a kept chunk is by construction one some snapshot saw
      // written, which is exactly what the far-field edit index wants.
      p.items.push_back({wc, true});
      const uint64_t srcOff = world_->PageOffsetOfSlot(s);
      p.sentinel.push_back(srcOff == World::kNoPage ? world_->PageEntryOfSlot(s) : 0u);
      if (srcOff != World::kNoPage)
        enc.CopyTracked(pass::Buf::Voxels, world_->voxels, srcOff, p.staging,
                        i * kChunkBytes, kChunkBytes);
      const uint64_t key = World::PackChunkKey(wc);
      pendingChunks_[key]++;
      ticketKeys_[key]++;
    }
    // Submitted EAGERLY, for EvictSlots' reason: the slots are cleared (or
    // refilled, on a re-centre) by deferred writes queued after this, so the
    // copy reads what the ticket held through the previous tick.
    ctx_->queue.Submit(enc.Finish());
    p.map = rhi::MapReadDeferred(ctx_->device, p.staging, 0, n * kChunkBytes);
    pending_.push_back(std::move(p));
  }
  return handle;
}

void Stream::SetEvictKeep(uint64_t handle, const std::vector<uint8_t>& keep) {
  if (handle == 0) return;
  size_t at = 0;
  for (PendingEvict& p : pending_) {
    if (p.keepHandle != handle) continue;
    // Keep is parallel to the slot list across the handle's batches.
    p.keep.assign(p.items.size(), 1u);
    for (size_t i = 0; i < p.items.size() && at + i < keep.size(); i++)
      p.keep[i] = keep[at + i];
    at += p.items.size();
    p.keepPending = false;
  }
  // Not found = already forced (a fill wanted the chunk first, a save drained
  // it): it kept every item, which is the conservative answer.
}

void Stream::ClearTicketSlots(const std::vector<uint32_t>& slots) {
  if (slots.empty()) return;
  static const std::vector<uint32_t> kAir(kChunkVol, 0u);
  const uint32_t zero = 0;
  const uint32_t sub[kSubOccStride] = {};
  for (uint32_t s : slots) {
    if (world_->residency == World::Residency::Paged) {
      // The page goes back to the free list. Safe against the eviction copy
      // that just read it: that copy was submitted before any later write
      // that reuses the page can be (queue order, EvictSlots' argument).
      world_->pages->SetSentinel(s, kPtEmpty);
    } else {
      // Dense has no sentinel: the identity map's page must hold real air,
      // or the whole-world hash would keep counting the departed chunk and
      // paged == dense would fail (docs/PLAN_chunk_tickets.md §2.8).
      const uint64_t off = world_->PageOffsetOfSlot(s);
      if (off != World::kNoPage)
        ctx_->queue.WriteBuffer(world_->voxels, off, kAir.data(), kChunkBytes);
    }
    ctx_->queue.WriteBuffer(world_->occupancy, (uint64_t)s * 4, &zero, 4);
    ctx_->queue.WriteBuffer(world_->occupancy,
                            ((uint64_t)kNumSlots + (uint64_t)s * kSubOccStride) * 4,
                            sub, sizeof(sub));
    ctx_->queue.WriteBuffer(world_->dirty[0], (uint64_t)s * 4, &zero, 4);
    ctx_->queue.WriteBuffer(world_->dirty[1], (uint64_t)s * 4, &zero, 4);
    modified_[s] = 0;
    world_->NoteSlotTouched(s);
  }
  if (world_->residency == World::Residency::Paged)
    world_->pages->FlushTableWrites(ctx_->queue);
}
