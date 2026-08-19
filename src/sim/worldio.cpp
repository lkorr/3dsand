#include "sim/worldio.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "gpu/resources.h"

namespace {
constexpr uint32_t kMagic = 0x31585653;  // 'SVX1'
}

bool SaveWorld(GpuContext& ctx, World& world, const std::string& path) {
  const uint64_t bytes = kVoxelCount * 4;
  wgpu::Buffer staging = CreateBuffer(ctx.device, bytes,
                                      wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                      "saveStaging");
  wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  enc.CopyBufferToBuffer(world.voxels, 0, staging, 0, bytes);
  wgpu::CommandBuffer cmd = enc.Finish();
  ctx.queue.Submit(1, &cmd);

  std::vector<uint32_t> data(kVoxelCount);
  bool got = false;
  wgpu::Future f = staging.MapAsync(
      wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
        if (status == wgpu::MapAsyncStatus::Success) {
          std::memcpy(data.data(), staging.GetConstMappedRange(0, bytes), bytes);
          staging.Unmap();
          got = true;
        }
      });
  ctx.instance.WaitAny(f, UINT64_MAX);
  if (!got) return false;

  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;
  uint32_t header[3] = {kMagic, kWorldN, kNumChunks};
  std::fwrite(header, 4, 3, fp);

  std::vector<uint16_t> rle;
  for (uint32_t c = 0; c < kNumChunks; c++) {
    rle.clear();
    const uint32_t* chunk = &data[(size_t)c * kChunkVol];
    uint32_t i = 0;
    while (i < kChunkVol) {
      uint16_t w = (uint16_t)(chunk[i] & 0xFFFF);  // stamp byte stripped
      uint32_t run = 1;
      while (i + run < kChunkVol && (uint16_t)(chunk[i + run] & 0xFFFF) == w &&
             run < 0xFFFF)
        run++;
      rle.push_back((uint16_t)run);
      rle.push_back(w);
      i += run;
    }
    uint32_t pairs = (uint32_t)(rle.size() / 2);
    std::fwrite(&pairs, 4, 1, fp);
    std::fwrite(rle.data(), 2, rle.size(), fp);
  }
  long size = std::ftell(fp);
  std::fclose(fp);
  std::printf("saved %s (%.2f MB, %.1fx RLE)\n", path.c_str(), size / 1e6,
              (double)bytes / size);
  return true;
}

bool LoadWorld(GpuContext& ctx, World& world, Simulation& sim, const std::string& path) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;
  uint32_t header[3] = {};
  if (std::fread(header, 4, 3, fp) != 3 || header[0] != kMagic ||
      header[1] != kWorldN || header[2] != kNumChunks) {
    std::fprintf(stderr, "load: %s is not a compatible world file\n", path.c_str());
    std::fclose(fp);
    return false;
  }

  std::vector<uint32_t> data(kVoxelCount, 0);
  std::vector<uint16_t> rle;
  for (uint32_t c = 0; c < kNumChunks; c++) {
    uint32_t pairs = 0;
    if (std::fread(&pairs, 4, 1, fp) != 1 || pairs > kChunkVol) goto corrupt;
    rle.resize((size_t)pairs * 2);
    if (std::fread(rle.data(), 2, rle.size(), fp) != rle.size()) goto corrupt;
    {
      uint32_t* chunk = &data[(size_t)c * kChunkVol];
      uint32_t i = 0;
      for (uint32_t p = 0; p < pairs; p++) {
        uint32_t run = rle[p * 2];
        uint32_t w = rle[p * 2 + 1];
        if (i + run > kChunkVol) goto corrupt;
        // stamp 0xFF = "hasn't acted": everything may move on the first tick
        for (uint32_t k = 0; k < run; k++) chunk[i++] = w | 0xFF0000u;
      }
      if (i != kChunkVol) goto corrupt;
    }
  }
  std::fclose(fp);

  {
    // Snapshot restore (worldgen-equivalent), not a live mutation: the direct
    // upload is sanctioned the same way worldgen's direct writes are. All
    // GAMEPLAY writes still flow through the MutationQueue (CLAUDE.md rule 3).
    ctx.queue.WriteBuffer(world.voxels, 0, data.data(), data.size() * 4);
    std::vector<uint32_t> allDirty(kNumChunks, 1);
    ctx.queue.WriteBuffer(world.dirty[0], 0, allDirty.data(), allDirty.size() * 4);
    ctx.queue.WriteBuffer(world.dirty[1], 0, allDirty.data(), allDirty.size() * 4);
    wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeLoadReset(enc);
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx.queue.Submit(1, &cmd);
  }
  std::printf("loaded %s\n", path.c_str());
  return true;

corrupt:
  std::fprintf(stderr, "load: %s is corrupt\n", path.c_str());
  std::fclose(fp);
  return false;
}
