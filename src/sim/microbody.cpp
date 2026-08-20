#include "sim/microbody.h"

#include <algorithm>

namespace {

// Word count for a brick of `cellCount` micro voxels (4 per word).
inline size_t WordsFor(size_t cellCount) { return (cellCount + 3) / 4; }

// Take `words` from the free list if an exact-size block is waiting, else bump
// the pool's high-water mark. Returns UINT32_MAX when the ceiling is hit.
//
// Exact-size only, deliberately: splitting a larger block leaves a remainder
// that nothing can use without a real allocator, and the reuse case that
// actually matters (a body re-editing at the same or smaller dims) is exact by
// construction because MicroBodyEdit reuses the block in place when it fits.
uint32_t PoolAlloc(MicroBodySet& set, size_t words) {
  if (words == 0) return UINT32_MAX;
  for (auto& [sz, blocks] : set.freeList) {
    if (sz == words && !blocks.empty()) {
      uint32_t base = blocks.back();
      blocks.pop_back();
      return base;
    }
  }
  if (set.pool.size() + words > kMicroBodyPoolWordsWorld) return UINT32_MAX;
  uint32_t base = (uint32_t)set.pool.size();
  set.pool.resize(set.pool.size() + words, 0u);
  return base;
}

void PoolFree(MicroBodySet& set, uint32_t base, size_t words) {
  if (words == 0) return;
  for (auto& [sz, blocks] : set.freeList) {
    if (sz == words) {
      blocks.push_back(base);
      return;
    }
  }
  set.freeList.push_back({(uint32_t)words, {base}});
}

// Flatten `voxels` into the pool block at `base`, 4 packed 8-bit material ids
// per word. Cells not covered by a voxel read 0 (empty).
void WriteBrick(MicroBodySet& set, uint32_t base, IVec3 dims,
                const std::vector<PrefabVoxel>& voxels, IVec3 origin) {
  const size_t cellCount = (size_t)dims.x * dims.y * dims.z;
  const size_t words = WordsFor(cellCount);
  std::fill(set.pool.begin() + base, set.pool.begin() + base + words, 0u);
  for (const PrefabVoxel& v : voxels) {
    int x = v.x - origin.x, y = v.y - origin.y, z = v.z - origin.z;
    if (x < 0 || y < 0 || z < 0 || x >= dims.x || y >= dims.y || z >= dims.z)
      continue;
    if (v.material == 0 || v.material > 255) continue;
    size_t idx = ((size_t)z * dims.y + y) * dims.x + x;
    set.pool[base + idx / 4] |= (uint32_t)(uint8_t)v.material << ((idx % 4) * 8);
  }
}

}  // namespace

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
  // Shared, not owned: load-time models back every instance of their def and
  // must survive any one instance being destroyed.
  set.owned.resize(set.models.size(), 0);
  set.blockWords.resize(set.models.size(), 0);
  set.blockWords.back() = (uint32_t)words;
  set.dirty = true;
  return (int)set.models.size() - 1;
}

int MicroBodyOwn(MicroBodySet& set, uint32_t model) {
  if (model >= set.models.size()) return -1;
  set.owned.resize(set.models.size(), 0);
  set.blockWords.resize(set.models.size(), 0);
  if (set.owned[model]) return (int)model;  // already private to one body

  // Clone only the words the payload actually occupies: a shared model is
  // always exactly its dims, so this is the same as its block size.
  IVec3 dims{(int)(set.models[model].dims & 1023),
             (int)((set.models[model].dims >> 10) & 1023),
             (int)((set.models[model].dims >> 20) & 1023)};
  const size_t words = WordsFor((size_t)dims.x * dims.y * dims.z);

  uint32_t slot;
  if (!set.freeModels.empty()) {
    slot = set.freeModels.back();
    set.freeModels.pop_back();
  } else {
    if (set.models.size() >= kMaxMicroBodyModels) return -1;
    slot = (uint32_t)set.models.size();
    set.models.push_back(MicroBodyModelGpu{});
    set.owned.resize(set.models.size(), 0);
    set.blockWords.resize(set.models.size(), 0);
  }

  uint32_t base = PoolAlloc(set, words);
  if (base == UINT32_MAX) {
    set.freeModels.push_back(slot);  // give the record back; nothing changed
    return -1;
  }
  // Re-read: the push_back above may have reallocated `models`.
  const MicroBodyModelGpu s = set.models[model];
  std::copy(set.pool.begin() + s.base, set.pool.begin() + s.base + words,
            set.pool.begin() + base);

  MicroBodyModelGpu& dst = set.models[slot];
  dst = s;
  dst.base = base;
  set.owned[slot] = 1;
  set.blockWords[slot] = (uint32_t)words;
  set.dirty = true;
  return (int)slot;
}

bool MicroBodyEdit(MicroBodySet& set, uint32_t model,
                   const std::vector<PrefabVoxel>& voxels, IVec3& originShift) {
  originShift = IVec3{0, 0, 0};
  if (model >= set.models.size() || voxels.empty()) return false;
  if (model >= set.owned.size() || !set.owned[model]) return false;

  // Re-derive the tight box: a body that lost half its voxels should draw a
  // smaller OBB, not the original one full of air.
  IVec3 mn{1 << 30, 1 << 30, 1 << 30}, mx{-(1 << 30), -(1 << 30), -(1 << 30)};
  for (const PrefabVoxel& v : voxels) {
    mn.x = std::min<int>(mn.x, v.x); mn.y = std::min<int>(mn.y, v.y);
    mn.z = std::min<int>(mn.z, v.z);
    mx.x = std::max<int>(mx.x, v.x); mx.y = std::max<int>(mx.y, v.y);
    mx.z = std::max<int>(mx.z, v.z);
  }
  IVec3 dims{mx.x - mn.x + 1, mx.y - mn.y + 1, mx.z - mn.z + 1};
  if (dims.x > 1023 || dims.y > 1023 || dims.z > 1023) return false;

  MicroBodyModelGpu& m = set.models[model];
  const size_t haveWords = set.blockWords[model];
  const size_t words = WordsFor((size_t)dims.x * dims.y * dims.z);

  if (words > haveWords) {
    // Grew past the reserved block (a split half re-based into a wider box):
    // needs a new one. Free at the size actually reserved, not at the dims.
    uint32_t base = PoolAlloc(set, words);
    if (base == UINT32_MAX) return false;
    PoolFree(set, m.base, haveWords);
    m.base = base;
    set.blockWords[model] = (uint32_t)words;
  }
  // Fitting in the existing block reuses it in place and KEEPS the surplus
  // attached (blockWords is unchanged), so repeated carving of one body never
  // touches the allocator and the free at teardown returns the whole block.
  m.dims = (uint32_t)dims.x | ((uint32_t)dims.y << 10) | ((uint32_t)dims.z << 20);
  WriteBrick(set, m.base, dims, voxels, mn);
  originShift = mn;
  set.dirty = true;
  return true;
}

void MicroBodyFree(MicroBodySet& set, uint32_t model) {
  if (model >= set.models.size()) return;
  if (model >= set.owned.size() || !set.owned[model]) return;
  MicroBodyModelGpu& m = set.models[model];
  PoolFree(set, m.base, set.blockWords[model]);
  set.blockWords[model] = 0;
  m.base = kMicroBodyNoModel;
  m.dims = 0;
  set.owned[model] = 0;
  set.freeModels.push_back(model);
  set.dirty = true;
}
