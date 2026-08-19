#include "sim/world.h"

#include <algorithm>
#include <cstring>

#include "gpu/resources.h"

// Readback slot layout (offsets in bytes).
constexpr uint64_t kChunkBytes = kChunkVol * 4;                 // 16 KB
constexpr uint64_t kMirrorBytes = 27 * kChunkBytes;             // 432 KB
constexpr uint64_t kDirtyOff = kMirrorBytes;
constexpr uint64_t kDirtyBytes = kNumChunks * 4;
constexpr uint64_t kOccOff = kDirtyOff + kDirtyBytes;
constexpr uint64_t kOccBytes = kNumChunks * 4;
constexpr uint64_t kHashOff = kOccOff + kOccBytes;
constexpr uint64_t kPickOff = kHashOff + 256;
constexpr uint64_t kPCountOff = kPickOff + 256;
constexpr uint64_t kFetchOff = kPCountOff + 256;
constexpr uint64_t kSlotBytes = kFetchOff + (uint64_t)World::kFetchPerTick * kChunkBytes;

void World::Init(const wgpu::Device& device) {
  using U = wgpu::BufferUsage;
  voxels = CreateBuffer(device, kVoxelCount * 4,
                        U::Storage | U::CopySrc | U::CopyDst, "voxels");
  dirty[0] = CreateBuffer(device, kDirtyBytes, U::Storage | U::CopySrc | U::CopyDst, "dirtyA");
  dirty[1] = CreateBuffer(device, kDirtyBytes, U::Storage | U::CopySrc | U::CopyDst, "dirtyB");
  dirtyList = CreateBuffer(device, kNumChunks * 4, U::Storage, "dirtyList");
  argsStage = CreateBuffer(device, 12, U::Storage | U::CopySrc | U::CopyDst, "argsStage");
  dispatchArgs = CreateBuffer(device, 12, U::Indirect | U::CopyDst, "dispatchArgs");
  occupancy = CreateBuffer(device, kOccBytes, U::Storage | U::CopySrc | U::CopyDst, "occupancy");
  hash = CreateBuffer(device, 16, U::Storage | U::CopySrc | U::CopyDst, "worldHash");
  tickUBO = CreateBuffer(device, sizeof(TickParams), U::Uniform | U::CopyDst, "tickUBO");
  passUBO = CreateBuffer(device, 54 * 256, U::Uniform | U::CopyDst, "passUBO");
  opsBuf = CreateBuffer(device, kMaxOpsPerTick * sizeof(BrushOp),
                        U::Storage | U::CopyDst, "brushOps");
  renderUBO = CreateBuffer(device, sizeof(RenderParams), U::Uniform | U::CopyDst, "renderUBO");
  pick = CreateBuffer(device, 32, U::Storage | U::CopySrc | U::CopyDst, "pick");

  particles[0] = CreateBuffer(device, (uint64_t)kParticleCap * 32, U::Storage, "particlesA");
  particles[1] = CreateBuffer(device, (uint64_t)kParticleCap * 32, U::Storage, "particlesB");
  particleCounts = CreateBuffer(device, 16, U::Storage | U::CopySrc | U::CopyDst,
                                "particleCounts");
  claim = CreateBuffer(device, (uint64_t)kClaimSize * 4, U::Storage | U::CopyDst, "claim");
  pArgsStage = CreateBuffer(device, 32, U::Storage | U::CopySrc, "pArgsStage");
  pDispatchArgs = CreateBuffer(device, 12, U::Indirect | U::CopyDst, "pDispatchArgs");
  drawArgs = CreateBuffer(device, 16, U::Indirect | U::CopyDst, "drawArgs");
  expOps = CreateBuffer(device, kMaxExplosionsPerTick * sizeof(ExplosionOp),
                        U::Storage | U::CopyDst, "explosionOps");
  expMask = CreateBuffer(device, (uint64_t)kMaxExplosionsPerTick * 68928 * 4,
                         U::Storage | U::CopyDst, "explosionMask");
  cellOps = CreateBuffer(device, kMaxCellOpsPerTick * sizeof(CellOp),
                         U::Storage | U::CopyDst, "cellOps");
  sprites = CreateBuffer(device, kMaxSprites * sizeof(Sprite), U::Storage | U::CopyDst,
                         "sprites");
  bodyInstances = CreateBuffer(device, 262144ull * 16, U::Storage | U::CopyDst,
                               "bodyInstances");
  bodyXforms = CreateBuffer(device, 256ull * 32, U::Storage | U::CopyDst, "bodyXforms");

  for (auto& s : slots_) {
    s.buf = CreateBuffer(device, kSlotBytes, U::MapRead | U::CopyDst, "readback");
    s.inFlight = false;
  }
  snap_.mirror.assign(27 * kChunkVol, 0);
  snap_.dirtyFlags.assign(kNumChunks, 0);
  fetchQueued_.assign(kNumChunks, 0);
}

void World::RequestChunkFetch(uint32_t chunkIdx) {
  if (chunkIdx >= kNumChunks || fetchQueued_[chunkIdx]) return;
  fetchQueued_[chunkIdx] = 1;
  fetchQueue_.push_back(chunkIdx);
}

const CachedChunk* World::Cached(uint32_t chunkIdx) const {
  auto it = cache_.find(chunkIdx);
  return it == cache_.end() ? nullptr : &it->second;
}

static uint32_t ChunkIndex(int cx, int cy, int cz) {
  return (uint32_t)((cz * (int)kNChunk + cy) * (int)kNChunk + cx);
}

bool World::EncodeReadbacks(const wgpu::Device&, const wgpu::CommandEncoder& enc,
                            IVec3 playerChunkBase, uint32_t particleLivePage,
                            uint32_t tick) {
  int slot = -1;
  for (int i = 0; i < kSlots; i++) {
    if (!slots_[i].inFlight) { slot = i; break; }
  }
  if (slot < 0) return false;
  Slot& s = slots_[slot];
  s.particleLivePage = particleLivePage;
  s.tick = tick;

  // drain queued chunk fetches into this slot (bounded per tick)
  s.fetchIds.clear();
  while (!fetchQueue_.empty() && s.fetchIds.size() < kFetchPerTick) {
    uint32_t ci = fetchQueue_.front();
    fetchQueue_.erase(fetchQueue_.begin());
    fetchQueued_[ci] = 0;
    s.fetchIds.push_back(ci);
  }
  for (size_t i = 0; i < s.fetchIds.size(); i++) {
    enc.CopyBufferToBuffer(voxels, (uint64_t)s.fetchIds[i] * kChunkBytes, s.buf,
                           kFetchOff + i * kChunkBytes, kChunkBytes);
  }

  // clamp 3x3x3 window to the chunk grid
  auto clampBase = [](int v) {
    if (v < 0) v = 0;
    if (v > (int)kNChunk - 3) v = (int)kNChunk - 3;
    return v;
  };
  s.base = {clampBase(playerChunkBase.x), clampBase(playerChunkBase.y),
            clampBase(playerChunkBase.z)};

  for (int dz = 0; dz < 3; dz++)
    for (int dy = 0; dy < 3; dy++)
      for (int dx = 0; dx < 3; dx++) {
        uint32_t ci = ChunkIndex(s.base.x + dx, s.base.y + dy, s.base.z + dz);
        uint64_t dst = (uint64_t)((dz * 3 + dy) * 3 + dx) * kChunkBytes;
        enc.CopyBufferToBuffer(voxels, (uint64_t)ci * kChunkBytes, s.buf, dst,
                               kChunkBytes);
      }
  // dirty buffer note: caller copies the *next-tick* dirty buffer; we take a
  // buffer reference at encode time via these explicit copies instead.
  enc.CopyBufferToBuffer(occupancy, 0, s.buf, kOccOff, kOccBytes);
  enc.CopyBufferToBuffer(hash, 0, s.buf, kHashOff, 16);
  enc.CopyBufferToBuffer(pick, 0, s.buf, kPickOff, 32);
  enc.CopyBufferToBuffer(particleCounts, 0, s.buf, kPCountOff, 16);
  lastSlot_ = slot;
  return true;
}

void World::EncodeDirtyCopy(const wgpu::CommandEncoder& enc, const wgpu::Buffer& dirtyNext) {
  if (lastSlot_ < 0) return;
  enc.CopyBufferToBuffer(dirtyNext, 0, slots_[lastSlot_].buf, kDirtyOff, kDirtyBytes);
}

void World::KickReadback() {
  if (lastSlot_ < 0) return;
  Slot& s = slots_[lastSlot_];
  s.inFlight = true;
  int slot = lastSlot_;
  lastSlot_ = -1;
  s.buf.MapAsync(
      wgpu::MapMode::Read, 0, kSlotBytes, wgpu::CallbackMode::AllowProcessEvents,
      [this, slot](wgpu::MapAsyncStatus status, wgpu::StringView) {
        Slot& sl = slots_[slot];
        if (status == wgpu::MapAsyncStatus::Success) {
          const uint8_t* p = (const uint8_t*)sl.buf.GetConstMappedRange(0, kSlotBytes);
          if (p) {
            std::memcpy(snap_.mirror.data(), p, kMirrorBytes);
            snap_.mirrorBase = sl.base;
            const uint32_t* dirtyW = (const uint32_t*)(p + kDirtyOff);
            const uint32_t* occW = (const uint32_t*)(p + kOccOff);
            uint32_t active = 0;
            uint64_t total = 0;
            for (uint32_t i = 0; i < kNumChunks; i++) {
              snap_.dirtyFlags[i] = dirtyW[i] != 0 ? 1 : 0;
              active += snap_.dirtyFlags[i];
              total += occW[i];
            }
            snap_.activeChunks = active;
            snap_.voxelTotal = total;
            std::memcpy(&snap_.worldHash, p + kHashOff, 4);
            std::memcpy(snap_.pick, p + kPickOff, 32);
            uint32_t pcounts[2];
            std::memcpy(pcounts, p + kPCountOff, 8);
            snap_.particleCount =
                std::min(pcounts[sl.particleLivePage & 1], kParticleCap);
            snap_.tick = sl.tick;
            snap_.valid = true;

            // fetched chunks land in the CPU cache, stamped with their tick
            for (size_t i = 0; i < sl.fetchIds.size(); i++) {
              CachedChunk& cc = cache_[sl.fetchIds[i]];
              if (cc.version <= sl.tick) {
                cc.version = sl.tick;
                cc.voxels.assign(
                    (const uint32_t*)(p + kFetchOff + i * kChunkBytes),
                    (const uint32_t*)(p + kFetchOff + (i + 1) * kChunkBytes));
              }
            }
            // bound the cache (drop chunks far in the past)
            if (cache_.size() > 1024) {
              for (auto it = cache_.begin(); it != cache_.end();) {
                if (it->second.version + 600 < sl.tick) it = cache_.erase(it);
                else ++it;
              }
            }
          }
          sl.buf.Unmap();
        }
        sl.inFlight = false;
      });
}

CellKind World::KindAt(IVec3 cell, const std::vector<uint32_t>& classOf) const {
  if (!snap_.valid) return CellKind::Unknown;
  if (cell.x < 0 || cell.y < 0 || cell.z < 0 || cell.x >= (int)kWorldN ||
      cell.y >= (int)kWorldN || cell.z >= (int)kWorldN)
    return CellKind::Solid;  // out of world = solid (matches sim rule)
  int cx = cell.x / (int)kChunk - snap_.mirrorBase.x;
  int cy = cell.y / (int)kChunk - snap_.mirrorBase.y;
  int cz = cell.z / (int)kChunk - snap_.mirrorBase.z;
  if (cx < 0 || cy < 0 || cz < 0 || cx >= 3 || cy >= 3 || cz >= 3)
    return CellKind::Unknown;
  int lx = cell.x % (int)kChunk, ly = cell.y % (int)kChunk, lz = cell.z % (int)kChunk;
  uint32_t w = snap_.mirror[(size_t)((cz * 3 + cy) * 3 + cx) * kChunkVol +
                            (lz * (int)kChunk + ly) * (int)kChunk + lx];
  uint32_t mat = w & 0xFFF;
  if (mat == 0) return CellKind::Air;
  if (mat >= classOf.size()) return CellKind::Air;
  switch (classOf[mat]) {
    case 0: case 1: return CellKind::Solid;  // solid + powder both carry weight
    case 2: return CellKind::Liquid;
    default: return CellKind::Gas;
  }
}

// ---- exact CPU mirror of worldgen.wgsl (integer-only, keep in sync) ----
static uint32_t pcg(uint32_t v) {
  uint32_t s = v * 747796405u + 2891336453u;
  uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
  return (w >> 22u) ^ w;
}
static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
  return pcg(a ^ pcg(b ^ pcg(c)));
}
static int vnoise(int x, int z, int cs, uint32_t seed) {
  int gx = x / cs, gz = z / cs;
  int fx = x % cs, fz = z % cs;
  int h00 = (int)(hash3(seed, (uint32_t)gx, (uint32_t)gz) & 0xFFu);
  int h10 = (int)(hash3(seed, (uint32_t)(gx + 1), (uint32_t)gz) & 0xFFu);
  int h01 = (int)(hash3(seed, (uint32_t)gx, (uint32_t)(gz + 1)) & 0xFFu);
  int h11 = (int)(hash3(seed, (uint32_t)(gx + 1), (uint32_t)(gz + 1)) & 0xFFu);
  int v0 = h00 * (cs - fx) + h10 * fx;
  int v1 = h01 * (cs - fx) + h11 * fx;
  return (v0 * (cs - fz) + v1 * fz) / (cs * cs);
}
int World::TerrainHeight(int x, int z, uint32_t seed) {
  return 44 + (vnoise(x, z, 64, seed ^ 1u) * 36) / 255 +
         (vnoise(x, z, 16, seed ^ 2u) * 10) / 255;
}
