#include "sim/worldio.h"

#include <cstdio>

namespace {
constexpr uint32_t kMetaMagic = 0x334D5653;  // 'SVM3'

std::string MetaPath(const std::string& dir) { return dir + "/meta.svm"; }
}  // namespace

bool SaveWorld(GpuContext& ctx, World& world, Stream& stream, const std::string& path) {
  ChunkStore& store = stream.Store();
  if (store.Bound() && store.Dir() != path) {
    std::fprintf(stderr, "save: store is bound to %s (one world dir per session)\n",
                 store.Dir().c_str());
    return false;
  }

  // resident window -> store (unfiltered: air chunks too, so a re-fill needs
  // no snapshot trust; drains in-flight async evictions)
  stream.FlushResident();

  if (!store.BindSave(path)) return false;
  size_t regions = 0;
  uint64_t bytes = 0;
  if (!store.Flush(&regions, &bytes)) return false;

  // meta last: its presence marks a completed save
  FILE* fp = std::fopen(MetaPath(path).c_str(), "wb");
  if (!fp) return false;
  IVec3 o = world.WindowOrigin();
  uint32_t header[3] = {kMetaMagic, kWorldN, kChunk};
  int32_t origin[3] = {o.x, o.y, o.z};
  std::fwrite(header, 4, 3, fp);
  std::fwrite(origin, 4, 3, fp);
  std::fclose(fp);
  std::printf("saved %s (%.2f MB across %zu regions)\n", path.c_str(),
              bytes / 1e6, regions);
  return true;
}

bool LoadWorld(GpuContext& ctx, World& world, Simulation& sim, Stream& stream,
               const std::string& path) {
  FILE* fp = std::fopen(MetaPath(path).c_str(), "rb");
  if (!fp) {
    std::fprintf(stderr, "load: %s has no meta.svm\n", path.c_str());
    return false;
  }
  uint32_t header[3] = {};
  int32_t origin[3] = {};
  bool ok = std::fread(header, 4, 3, fp) == 3 && std::fread(origin, 4, 3, fp) == 3 &&
            header[0] == kMetaMagic && header[1] == kWorldN && header[2] == kChunk;
  std::fclose(fp);
  if (!ok) {
    std::fprintf(stderr, "load: %s is not a compatible world dir\n", path.c_str());
    return false;
  }

  ChunkStore& store = stream.Store();
  if (!store.BindLoad(path)) {
    std::fprintf(stderr, "load: store is bound to %s (one world dir per session)\n",
                 store.Bound() ? store.Dir().c_str() : "?");
    return false;
  }

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
  std::printf("loaded %s (%zu chunks in RAM after window fill)\n", path.c_str(),
              stream.Store().Count());
  return true;
}
