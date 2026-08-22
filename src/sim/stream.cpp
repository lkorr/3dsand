#include "sim/stream.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gpu/context.h"
#include "gpu/resources.h"
#include "sim/pass_table.h"  // pass::Buf::Voxels for the tracked eviction copy
#include "sim/simulation.h"

namespace {
constexpr uint64_t kChunkBytes = kChunkVol * 4;
constexpr int kHysteresis = 2;        // chunks past center before a shift
constexpr size_t kEvictBatch = 256;   // staging bound: 4 MB per readback batch
constexpr size_t kMaxPendingEvicts = 4;  // in-flight batches before we block
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

void Stream::Update(IVec3 playerChunk) {
  // harvest evictions whose readback completed since last tick (non-blocking)
  while (!pending_.empty() && pending_.front().map.Ready())
    CompleteOldest(/*discard=*/false);

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
  for (int a = 0; a < 3; a++) {
    if (d[a] >= kHysteresis) ShiftAxis(a, 1);
    else if (d[a] <= -kHysteresis) ShiftAxis(a, -1);
  }
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

  EvictSlots(slots, /*filter=*/true);

  IVec3 no = o;
  if (axis == 0) no.x += dir;
  else if (axis == 1) no.y += dir;
  else no.z += dir;
  world_->SetWindowOrigin(no);
  FillSlots(slots);
  shifts_++;
}

void Stream::EvictSlots(const std::vector<uint32_t>& slots, bool filter) {
  const WorldSnapshot& snap = world_->Snap();
  // Everything the completion needs is captured NOW: the slots are refilled
  // (and modified_ reset) before the readback lands.
  std::vector<std::pair<uint32_t, PendingEvict::Item>> toSave;
  toSave.reserve(slots.size());
  for (uint32_t s : slots) {
    bool worth = true;
    if (filter && snap.valid)
      worth = snap.occupancy[s] > 0 || modified_[s] != 0;
    if (!worth) continue;
    // all-air and never modified => procgen reproduces it; don't store
    // (only trustable with a live snapshot — flushes store everything)
    uint8_t dropIfAir = filter && snap.valid && modified_[s] == 0;
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
      enc.CopyTracked(pass::Buf::Voxels, world_->voxels,
                      (uint64_t)toSave[off + i].first * kChunkBytes,
                      p.staging, i * kChunkBytes, kChunkBytes);
      p.items.push_back(toSave[off + i].second);
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
  PendingEvict p = std::move(pending_.front());
  pending_.pop_front();
  p.map.Wait();  // resolves the map

  if (p.map.Succeeded()) {
    if (!discard) {
      const uint8_t* ptr = (const uint8_t*)p.map.Data();
      if (ptr) {
        std::vector<uint32_t> data(kChunkVol);
        std::vector<uint32_t> rle;
        for (size_t i = 0; i < p.items.size(); i++) {
          std::memcpy(data.data(), ptr + i * kChunkBytes, kChunkBytes);
          RleEncodeChunk(data.data(), rle);
          bool air = rle.size() == 2 && rle[1] == 0;
          if (air && p.items[i].dropIfAir) continue;
          store_.Put(p.items[i].wc, rle);
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
}

void Stream::DrainEvictions(bool discard) {
  while (!pending_.empty()) CompleteOldest(discard);
}

void Stream::FillSlots(const std::vector<uint32_t>& slots) {
  std::vector<uint32_t> data(kChunkVol);
  std::vector<uint32_t> genSlots;
  const uint32_t one = 1;
  for (uint32_t s : slots) {
    modified_[s] = 0;
    IVec3 wc = world_->SlotToWorldChunk(s);
    // the chunk's own eviction may still be in flight (player doubled back
    // within the map latency): complete it so the store lookup sees it
    while (pendingChunks_.count(World::PackChunkKey(wc)))
      CompleteOldest(/*discard=*/false);
    const std::vector<uint32_t>* rle = store_.Get(wc);
    if (rle && RleDecodeChunk(rle->data(), rle->size() / 2, data.data())) {
      ctx_->queue.WriteBuffer(world_->voxels, (uint64_t)s * kChunkBytes,
                              data.data(), kChunkBytes);
      uint32_t occ = 0, blockers = 0;
      for (uint32_t w : data) {
        uint32_t m = w & 0xFFFu;
        if (m == 0) continue;
        occ++;
        if (m < blockerOf_.size() && blockerOf_[m]) blockers++;
      }
      occ |= blockers << 16;  // packing per common.wgsl packOcc
      ctx_->queue.WriteBuffer(world_->occupancy, (uint64_t)s * 4, &occ, 4);
      // wake once: neighbors may have changed since this chunk was saved
      ctx_->queue.WriteBuffer(world_->dirty[0], (uint64_t)s * 4, &one, 4);
      ctx_->queue.WriteBuffer(world_->dirty[1], (uint64_t)s * 4, &one, 4);
    } else {
      genSlots.push_back(s);
    }
  }
  if (!genSlots.empty()) {
    ctx_->queue.WriteBuffer(world_->genList, 0, genSlots.data(),
                            genSlots.size() * 4);
    TickParams tp{};
    tp.seed = seed_;
    tp.genCount = (uint32_t)genSlots.size();
    IVec3 o = world_->WindowOrigin();
    tp.origin[0] = o.x; tp.origin[1] = o.y; tp.origin[2] = o.z;
    ctx_->queue.WriteBuffer(world_->tickUBO, 0, &tp, sizeof(tp));
    rhi::CommandEncoder enc = ctx_->device.CreateCommandEncoder();
    sim_->EncodeGenList(enc, (uint32_t)genSlots.size());
    ctx_->queue.Submit(enc.Finish());
  }
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
  world_->SetWindowOrigin(origin);
  modified_.assign(kNumChunks, 0);
  std::vector<uint32_t> slots(kNumChunks);
  for (uint32_t i = 0; i < kNumChunks; i++) slots[i] = i;
  FillSlots(slots);
}
