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

void Stream::Update(IVec3 playerChunk) {
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
  std::vector<std::pair<uint32_t, IVec3>> toSave;
  toSave.reserve(slots.size());
  for (uint32_t s : slots) {
    bool worth = true;
    if (filter && snap.valid)
      worth = snap.occupancy[s] > 0 || modified_[s] != 0;
    if (worth) toSave.push_back({s, world_->SlotToWorldChunk(s)});
  }
  if (toSave.empty()) return;

  std::vector<uint32_t> data(kChunkVol);
  std::vector<uint16_t> rle;
  for (size_t off = 0; off < toSave.size(); off += kEvictBatch) {
    size_t n = std::min(kEvictBatch, toSave.size() - off);
    wgpu::Buffer staging = CreateBuffer(
        ctx_->device, n * kChunkBytes,
        wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst, "evictStaging");
    wgpu::CommandEncoder enc = ctx_->device.CreateCommandEncoder();
    for (size_t i = 0; i < n; i++)
      enc.CopyBufferToBuffer(world_->voxels,
                             (uint64_t)toSave[off + i].first * kChunkBytes,
                             staging, i * kChunkBytes, kChunkBytes);
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx_->queue.Submit(1, &cmd);
    const uint8_t* p = nullptr;
    wgpu::Future f = staging.MapAsync(
        wgpu::MapMode::Read, 0, n * kChunkBytes, wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
          if (status == wgpu::MapAsyncStatus::Success)
            p = (const uint8_t*)staging.GetConstMappedRange(0, n * kChunkBytes);
        });
    ctx_->instance.WaitAny(f, UINT64_MAX);
    if (!p) continue;  // lost this batch (device error); keep going
    for (size_t i = 0; i < n; i++) {
      std::memcpy(data.data(), p + i * kChunkBytes, kChunkBytes);
      RleEncodeChunk(data.data(), rle);
      uint32_t slot = toSave[off + i].first;
      // all-air and never modified => procgen reproduces it; don't store
      // (only trustable with a live snapshot — flushes store everything)
      bool air = rle.size() == 2 && rle[1] == 0;
      if (air && filter && snap.valid && modified_[slot] == 0) continue;
      store_.Put(toSave[off + i].second, rle);
    }
    staging.Unmap();
  }
}

void Stream::FillSlots(const std::vector<uint32_t>& slots) {
  std::vector<uint32_t> data(kChunkVol);
  std::vector<uint32_t> genSlots;
  const uint32_t one = 1;
  for (uint32_t s : slots) {
    modified_[s] = 0;
    IVec3 wc = world_->SlotToWorldChunk(s);
    const std::vector<uint16_t>* rle = store_.Get(wc);
    if (rle && RleDecodeChunk(rle->data(), rle->size() / 2, data.data())) {
      ctx_->queue.WriteBuffer(world_->voxels, (uint64_t)s * kChunkBytes,
                              data.data(), kChunkBytes);
      uint32_t occ = 0;
      for (uint32_t w : data)
        if ((w & 0xFFFu) != 0) occ++;
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
}

void Stream::ReloadWindow(IVec3 origin) {
  world_->SetWindowOrigin(origin);
  modified_.assign(kNumChunks, 0);
  std::vector<uint32_t> slots(kNumChunks);
  for (uint32_t i = 0; i < kNumChunks; i++) slots[i] = i;
  FillSlots(slots);
}
