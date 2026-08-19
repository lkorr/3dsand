#include "sim/worldio.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t kMagic = 0x32585653;  // 'SVX2'
}

bool SaveWorld(GpuContext& ctx, World& world, Stream& stream, const std::string& path) {
  // resident window -> store (unfiltered: air chunks too, so a re-fill needs
  // no snapshot trust), then the store IS the file
  stream.FlushResident();

  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;
  IVec3 o = world.WindowOrigin();
  uint32_t header[4] = {kMagic, kWorldN, kChunk, (uint32_t)stream.Store().Count()};
  int32_t origin[3] = {o.x, o.y, o.z};
  std::fwrite(header, 4, 4, fp);
  std::fwrite(origin, 4, 3, fp);

  uint64_t bytes = 0;
  for (const auto& [key, e] : stream.Store().Map()) {
    int32_t wc[3] = {e.wc.x, e.wc.y, e.wc.z};
    uint32_t pairs = (uint32_t)(e.rle.size() / 2);
    std::fwrite(wc, 4, 3, fp);
    std::fwrite(&pairs, 4, 1, fp);
    std::fwrite(e.rle.data(), 2, e.rle.size(), fp);
    bytes += 16 + e.rle.size() * 2;
  }
  std::fclose(fp);
  std::printf("saved %s (%.2f MB, %zu chunks)\n", path.c_str(), bytes / 1e6,
              stream.Store().Count());
  return true;
}

bool LoadWorld(GpuContext& ctx, World& world, Simulation& sim, Stream& stream,
               const std::string& path) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;
  uint32_t header[4] = {};
  int32_t origin[3] = {};
  if (std::fread(header, 4, 4, fp) != 4 || std::fread(origin, 4, 3, fp) != 3 ||
      header[0] != kMagic || header[1] != kWorldN || header[2] != kChunk) {
    std::fprintf(stderr, "load: %s is not a compatible world file\n", path.c_str());
    std::fclose(fp);
    return false;
  }

  ChunkStore& store = stream.Store();
  store.Clear();
  std::vector<uint16_t> rle;
  for (uint32_t c = 0; c < header[3]; c++) {
    int32_t wc[3];
    uint32_t pairs = 0;
    if (std::fread(wc, 4, 3, fp) != 3 || std::fread(&pairs, 4, 1, fp) != 1 ||
        pairs > kChunkVol)
      goto corrupt;
    rle.resize((size_t)pairs * 2);
    if (std::fread(rle.data(), 2, rle.size(), fp) != rle.size()) goto corrupt;
    store.Put({wc[0], wc[1], wc[2]}, rle);
  }
  std::fclose(fp);

  {
    // Snapshot restore (worldgen-equivalent), not a live mutation: the direct
    // upload path in FillSlots is sanctioned the same way worldgen's direct
    // writes are. All GAMEPLAY writes still flow through the MutationQueue.
    stream.ReloadWindow({origin[0], origin[1], origin[2]});
    wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeLoadReset(enc);
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx.queue.Submit(1, &cmd);
  }
  std::printf("loaded %s (%zu chunks)\n", path.c_str(), stream.Store().Count());
  return true;

corrupt:
  std::fprintf(stderr, "load: %s is corrupt\n", path.c_str());
  std::fclose(fp);
  store.Clear();
  return false;
}
