#include "sim/stream.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gpu/context.h"
#include "gpu/resources.h"
#include "sim/simulation.h"

namespace {
constexpr uint64_t kChunkBytes = kChunkVol * 4;
constexpr int kHysteresis = 2;        // chunks past center before a shift
constexpr size_t kEvictBatch = 256;   // staging bound: 4 MB per readback batch
constexpr size_t kMaxPendingEvicts = 4;  // in-flight batches before we block
}  // namespace

void RleEncodeChunk(const uint32_t* words, std::vector<uint16_t>& out) {
  out.clear();
  uint32_t i = 0;
  while (i < kChunkVol) {
    uint16_t w = (uint16_t)(words[i] & 0xFFFF);  // stamp byte stripped
    uint32_t run = 1;
    while (i + run < kChunkVol && (uint16_t)(words[i + run] & 0xFFFF) == w &&
           run < 0xFFFF)
      run++;
    out.push_back((uint16_t)run);
    out.push_back(w);
    i += run;
  }
}

bool RleDecodeChunk(const uint16_t* rle, size_t pairs, uint32_t* out) {
  uint32_t i = 0;
  for (size_t p = 0; p < pairs; p++) {
    uint32_t run = rle[p * 2];
    uint32_t w = rle[p * 2 + 1];
    if (i + run > kChunkVol) return false;
    // stamp 0xFF = "hasn't acted": everything may move on the first tick
    for (uint32_t k = 0; k < run; k++) out[i++] = w | 0xFF0000u;
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
  // must match isRayBlocker in common.wgsl: solids, powders, opaque liquids
  blockerOf_.clear();
  for (const auto& m : mats)
    blockerOf_.push_back(m.gpu.klass == CLASS_SOLID || m.gpu.klass == CLASS_POWDER ||
                         (m.gpu.klass == CLASS_LIQUID &&
                          (m.gpu.flags & kMatFlagOpaque) != 0));
}

void Stream::Update(IVec3 playerChunk) {
  // harvest evictions whose readback completed since last tick (non-blocking)
  while (!pending_.empty() &&
         ctx_->instance.WaitAny(pending_.front().future, 0) ==
             wgpu::WaitStatus::Success)
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
    p.mapStatus = std::make_shared<uint32_t>(0);
    p.items.reserve(n);
    wgpu::CommandEncoder enc = ctx_->device.CreateCommandEncoder();
    for (size_t i = 0; i < n; i++) {
      enc.CopyBufferToBuffer(world_->voxels,
                             (uint64_t)toSave[off + i].first * kChunkBytes,
                             p.staging, i * kChunkBytes, kChunkBytes);
      p.items.push_back(toSave[off + i].second);
      pendingChunks_[World::PackChunkKey(toSave[off + i].second.wc)]++;
    }
    wgpu::CommandBuffer cmd = enc.Finish();
    // submit BEFORE FillSlots writes: queue order makes the copy read the
    // leaving plane's data even though the map completes ticks later
    ctx_->queue.Submit(1, &cmd);
    p.future = p.staging.MapAsync(
        wgpu::MapMode::Read, 0, n * kChunkBytes, wgpu::CallbackMode::WaitAnyOnly,
        [st = p.mapStatus](wgpu::MapAsyncStatus status, wgpu::StringView) {
          *st = status == wgpu::MapAsyncStatus::Success ? 1u : 2u;
        });
    pending_.push_back(std::move(p));
  }
}

wgpu::Buffer Stream::AcquireStaging() {
  if (stagingPool_.empty() && pending_.size() >= kMaxPendingEvicts)
    CompleteOldest(/*discard=*/false);  // ring full: recycle the oldest
  if (!stagingPool_.empty()) {
    wgpu::Buffer b = stagingPool_.back();
    stagingPool_.pop_back();
    return b;
  }
  return CreateBuffer(ctx_->device, kEvictBatch * kChunkBytes,
                      wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                      "evictStaging");
}

void Stream::CompleteOldest(bool discard) {
  if (pending_.empty()) return;
  PendingEvict p = std::move(pending_.front());
  pending_.pop_front();
  ctx_->instance.WaitAny(p.future, UINT64_MAX);  // fires the map callback

  if (*p.mapStatus == 1) {
    if (!discard) {
      const uint8_t* ptr = (const uint8_t*)p.staging.GetConstMappedRange(
          0, p.items.size() * kChunkBytes);
      if (ptr) {
        std::vector<uint32_t> data(kChunkVol);
        std::vector<uint16_t> rle;
        for (size_t i = 0; i < p.items.size(); i++) {
          std::memcpy(data.data(), ptr + i * kChunkBytes, kChunkBytes);
          RleEncodeChunk(data.data(), rle);
          bool air = rle.size() == 2 && rle[1] == 0;
          if (air && p.items[i].dropIfAir) continue;
          store_.Put(p.items[i].wc, rle);
        }
      }
    }
    p.staging.Unmap();
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
    const std::vector<uint16_t>* rle = store_.Get(wc);
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
    wgpu::CommandEncoder enc = ctx_->device.CreateCommandEncoder();
    sim_->EncodeGenList(enc, (uint32_t)genSlots.size());
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx_->queue.Submit(1, &cmd);
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
