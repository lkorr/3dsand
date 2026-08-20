#include "sim/microbody.h"

int MicroBodyPack(MicroBodySet& set, const std::vector<PrefabVoxel>& voxels,
                  IVec3 dims, uint32_t scale, const std::string& label,
                  std::string& log) {
  if (voxels.empty()) return -1;
  if (dims.x <= 0 || dims.y <= 0 || dims.z <= 0) return -1;
  // 10 bits per axis in MicroBodyModelGpu.dims.
  if (dims.x > 1023 || dims.y > 1023 || dims.z > 1023) {
    log += label + ": micro body model is " + std::to_string(dims.x) + "x" +
           std::to_string(dims.y) + "x" + std::to_string(dims.z) +
           ", max 1023 per axis\n";
    return -1;
  }
  if (set.models.size() >= kMaxMicroBodyModels) {
    log += label + ": micro body model table full (" +
           std::to_string(kMaxMicroBodyModels) + ")\n";
    return -1;
  }

  const size_t cellCount = (size_t)dims.x * dims.y * dims.z;
  const size_t words = (cellCount + 3) / 4;
  if (set.pool.size() + words > kMicroBodyPoolWordsWorld) {
    log += label + ": micro body brick pool full (" +
           std::to_string(kMicroBodyPoolWordsWorld) + " words)\n";
    return -1;
  }

  std::vector<uint8_t> cells(cellCount, 0);
  for (const PrefabVoxel& v : voxels) {
    if (v.x < 0 || v.y < 0 || v.z < 0 || v.x >= dims.x || v.y >= dims.y ||
        v.z >= dims.z)
      continue;  // loader guarantees in-box; a stray voxel is just dropped
    if (v.material == 0 || v.material > 255) {
      // 8 bits per micro voxel is what makes the pool affordable; naming the
      // limit beats a silently truncated id painting the wrong colour.
      log += label + ": micro body voxel material id " +
             std::to_string(v.material) + " out of range 1..255\n";
      return -1;
    }
    cells[((size_t)v.z * dims.y + v.y) * dims.x + v.x] = (uint8_t)v.material;
  }

  const uint32_t base = (uint32_t)set.pool.size();
  for (size_t w = 0; w < words; w++) {
    uint32_t word = 0;
    for (size_t b = 0; b < 4; b++) {
      size_t idx = w * 4 + b;
      if (idx < cellCount) word |= (uint32_t)cells[idx] << (b * 8);
    }
    set.pool.push_back(word);
  }

  MicroBodyModelGpu m{};
  m.base = base;
  m.dims = (uint32_t)dims.x | ((uint32_t)dims.y << 10) | ((uint32_t)dims.z << 20);
  m.scale = scale;
  m._pad = 0;
  set.models.push_back(m);
  return (int)set.models.size() - 1;
}
