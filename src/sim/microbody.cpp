#include "sim/microbody.h"

#include <algorithm>
#include <unordered_set>

namespace {

// Word count for a brick of `cellCount` micro voxels (2 per word).
//
// 16 bits per micro voxel: the low byte is the material id (what the voxel IS
// — meat, wood) and the high byte is an art palette slot (what it LOOKS like,
// 0 = just use the material's colour). A mob is one material all over and
// painted per voxel, so the two cannot share a channel.
//
// This halves the pool's voxel capacity, which is affordable at real asset
// sizes: every .vox in assets/ together occupies ~49k of the 1 MiW pool at
// 4 bpw, so ~98k at 2 bpw — under 10% either way.
inline size_t WordsFor(size_t cellCount) { return (cellCount + 1) / 2; }
// Word count for a brick's STAIN lattice: one byte per micro voxel, 4 per word.
inline size_t StainWordsFor(size_t cellCount) { return (cellCount + 3) / 4; }
inline size_t CellsOf(uint32_t dimsWord) {
  return (size_t)(dimsWord & 1023) * ((dimsWord >> 10) & 1023) *
         ((dimsWord >> 20) & 1023);
}
inline bool HasStain(uint32_t dimsWord) {
  return (dimsWord & kMicroBodyDimsStainBit) != 0u;
}

// Pack/unpack the 16-bit micro voxel.
inline uint16_t MicroVox(uint8_t mat, uint8_t color) {
  return (uint16_t)mat | ((uint16_t)color << 8);
}

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

// Flatten `voxels` into the pool block at `base`, 2 packed 16-bit micro voxels
// per word (see WordsFor). Cells not covered by a voxel read 0 (empty).
//
// The material is MASKED to its 12 bits before the range check. Callers hand
// us skinVoxels straight out of a limb, and those carry a cosmetic palette
// variant in bits 12-13 (mob.cpp) — testing the raw uint16 against 255 made
// every variant>=1 voxel silently vanish from the re-skinned body.
//
// `withStain`: the block is payload + stain lattice (see
// MicroBodyModelGpu::dims bit 30) and every voxel's stain byte is written after
// its material. The lattice is cleared with the payload, so a voxel that has
// been washed clean or carved away reads 0 without a separate erase.
void WriteBrick(MicroBodySet& set, uint32_t base, IVec3 dims,
                const std::vector<PrefabVoxel>& voxels, IVec3 origin,
                bool withStain) {
  const size_t cellCount = (size_t)dims.x * dims.y * dims.z;
  const size_t words = WordsFor(cellCount);
  const size_t total = words + (withStain ? StainWordsFor(cellCount) : 0);
  std::fill(set.pool.begin() + base, set.pool.begin() + base + total, 0u);
  const uint32_t sbase = base + (uint32_t)words;
  for (const PrefabVoxel& v : voxels) {
    int x = v.x - origin.x, y = v.y - origin.y, z = v.z - origin.z;
    if (x < 0 || y < 0 || z < 0 || x >= dims.x || y >= dims.y || z >= dims.z)
      continue;
    const uint16_t mat = v.material & 0xFFF;
    if (mat == 0 || mat > 255) continue;
    size_t idx = ((size_t)z * dims.y + y) * dims.x + x;
    set.pool[base + idx / 2] |=
        (uint32_t)MicroVox((uint8_t)mat, v.color) << ((idx % 2) * 16);
    if (withStain && v.stain)
      set.pool[sbase + idx / 4] |= (uint32_t)v.stain << ((idx % 4) * 8);
  }
  set.MarkPool(base, base + (uint32_t)total);
}

}  // namespace

void MicroBodySet::MarkPool(uint32_t lo, uint32_t hi) {
  dirty = true;
  if (poolDirtyAll || hi <= lo) return;
  for (auto& r : dirtyRanges) {
    // Merge when the new span overlaps, abuts, or sits within the slack gap of
    // an existing one. Scanning all of them (<= 24) rather than only the last
    // matters: a tick that burns two limbs alternates between two blocks, and
    // a last-only merge would open a fresh range on every alternation.
    if (lo <= r.second + kDirtyMergeGap && r.first <= hi + kDirtyMergeGap) {
      r.first = std::min(r.first, lo);
      r.second = std::max(r.second, hi);
      return;
    }
  }
  if (dirtyRanges.size() >= kMaxDirtyRanges) {
    // Too fragmented to track: fall back to the whole pool. Slower, never
    // wrong — see the header comment.
    dirtyRanges.clear();
    poolDirtyAll = true;
    return;
  }
  dirtyRanges.push_back({lo, hi});
}

void MicroBodySet::ClearDirty() {
  dirty = false;
  poolDirtyAll = false;
  dirtyRanges.clear();
}

std::vector<uint8_t> MicroBodyMergeArt(MicroBodySet& set,
                                       const std::vector<uint32_t>& artColors,
                                       const std::string& label,
                                       std::string& log) {
  // Identity by default, so a prefab that painted nothing costs nothing and
  // every unpainted voxel keeps color 0.
  //
  // INDEXED BY .vox PALETTE SLOT (128..255), VALUED IN MERGED 1-BASED INDICES.
  // The two sides of this table are deliberately different numbering systems
  // and that IS the conversion: the caller applies it once
  // (`if (v.color) v.color = remap[v.color]`) and from that point on a
  // PrefabVoxel/DebrisVoxel `color` is a merged index, never a .vox slot again.
  std::vector<uint8_t> remap(256, 0);
  if (artColors.empty()) return remap;

  uint32_t dropped = 0;
  // The SOURCE bound is the per-file limit (a .vox addresses art at 128..255);
  // the MERGED bound below is kArtPaletteSlotsGpu. Conflating the two is what
  // used to cap the whole cast at 128 colours — see the note in world.h.
  for (size_t i = 0; i < artColors.size() && i < (size_t)kArtPaletteSlots; i++) {
    const uint32_t rgb = artColors[i];
    const int srcSlot = kArtPaletteBase + (int)i;
    if (srcSlot > kArtPaletteTop) break;
    // Colours are deduplicated across prefabs: two mobs painted the same red
    // share one slot, which is what keeps the merged palette small enough for a
    // whole cast plus its wardrobe.
    auto it = std::find(set.artColors.begin(), set.artColors.end(), rgb);
    size_t at;
    if (it != set.artColors.end()) {
      at = (size_t)(it - set.artColors.begin());
    } else {
      if (set.artColors.size() >= (size_t)kArtPaletteSlotsGpu) { dropped++; continue; }
      at = set.artColors.size();
      set.artColors.push_back(rgb);
    }
    // 1-BASED: 0 is reserved for "unpainted, use the material's own colour", so
    // merged entry `at` is stored as `at + 1`. This is also why the merged
    // ceiling is 255 and not 256 — the byte has to hold at+1.
    remap[srcSlot] = (uint8_t)(at + 1);
  }
  if (dropped)
    log += label + ": art palette full (" + std::to_string(kArtPaletteSlotsGpu) +
           " colours across all loaded models); " + std::to_string(dropped) +
           " colour(s) fall back to the material colour\n";
  return remap;
}

// ---- the joint mask (microbody.h MicroBodyCutFaces) ------------------------
//
// THE PROBLEM THIS SOLVES. A part is rendered as its own tightly-bounded brick
// and the fragment shader can see no other part, so when the smooth normal
// differentiates the occupancy field it has to assume something about what lies
// one cell outside the brick. "Air" is right past the SIDE of an arm — that is
// what rounds the silhouette — and wrong past its END, where the forearm
// actually is. Assuming air everywhere rounded the caps too, putting a hard
// light/dark ring at every shoulder, elbow, hip and knee that swung with the
// joint as it animated.
//
// WHY IT CANNOT BE MEASURED FROM THE BRICK ALONE. The two cases are locally
// identical — a solid boundary plane with unknown beyond — and they are not
// reliably told apart by shape either: on chunky voxel art the flat SIDE of an
// arm is as solid a boundary plane as its cap is. The only thing that knows is
// the art: every part of a multi-model prefab lives in ONE shared coordinate
// frame, and a joint is exactly a face that another model is pressed against.
// So that is what this measures, once, at load.
uint32_t MicroBodyCutFaces(const Prefab& pf, int self) {
  if (self < 0 || self >= (int)pf.models.size()) return 0;
  const PrefabModel& m = pf.models[self];
  if (m.size.x <= 0 || m.size.y <= 0 || m.size.z <= 0) return 0;

  // Prefab-frame occupancy of every OTHER model. `voxels` are rebased to the
  // model's own zero, so `offset` is what puts them back in the shared frame.
  auto key = [](int x, int y, int z) {
    return (uint64_t)(uint16_t)(int16_t)x | ((uint64_t)(uint16_t)(int16_t)y << 16) |
           ((uint64_t)(uint16_t)(int16_t)z << 32);
  };
  std::unordered_set<uint64_t> others;
  for (size_t i = 0; i < pf.models.size(); i++) {
    if ((int)i == self) continue;
    const PrefabModel& o = pf.models[i];
    for (const PrefabVoxel& v : o.voxels) {
      if ((v.material & 0xFFF) == 0) continue;
      others.insert(key(v.x + o.offset.x, v.y + o.offset.y, v.z + o.offset.z));
    }
  }
  if (others.empty()) return 0;

  // This model's own occupancy, local coords.
  const int dim[3] = {m.size.x, m.size.y, m.size.z};
  std::vector<uint8_t> solid((size_t)dim[0] * dim[1] * dim[2], 0);
  auto at = [&](int x, int y, int z) -> uint8_t& {
    return solid[((size_t)z * dim[1] + y) * dim[0] + x];
  };
  for (const PrefabVoxel& v : m.voxels) {
    if ((v.material & 0xFFF) == 0) continue;
    if (v.x < 0 || v.y < 0 || v.z < 0 || v.x >= dim[0] || v.y >= dim[1] ||
        v.z >= dim[2])
      continue;
    at(v.x, v.y, v.z) = 1;
  }

  uint32_t mask = 0;
  for (int axis = 0; axis < 3; axis++) {
    const int ta = (axis + 1) % 3, tb = (axis + 2) % 3;
    for (int side = 0; side < 2; side++) {
      const int plane = side ? dim[axis] - 1 : 0;
      const int step = side ? 1 : -1;
      int n = 0, covered = 0;
      for (int u = 0; u < dim[ta]; u++)
        for (int v = 0; v < dim[tb]; v++) {
          int c[3];
          c[axis] = plane;
          c[ta] = u;
          c[tb] = v;
          if (!at(c[0], c[1], c[2])) continue;
          n++;
          c[axis] = plane + step;
          if (others.count(key(c[0] + m.offset.x, c[1] + m.offset.y,
                               c[2] + m.offset.z)))
            covered++;
        }
      // Face encoding is axis * 2 + positive — the same one shadowFaceOf and
      // microbody.wgsl's bodySolidAt use.
      if (n > 0 && covered * 2 >= n) mask |= 1u << (axis * 2 + side);
    }
  }
  return mask;
}

int MicroBodyPack(MicroBodySet& set, const std::vector<PrefabVoxel>& voxels,
                  IVec3 dims, uint32_t scale, const std::string& label,
                  std::string& log, uint32_t cutFaces) {
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
  // A model packed from stained voxels -- a gobbet split off a bloodied limb,
  // a corpse reloaded -- keeps its stain: the block grows the lattice and the
  // dims word says so. A def's shared model never has one (nothing authored
  // is stained), which is what keeps the load-time pool cost unchanged.
  bool withStain = false;
  for (const PrefabVoxel& v : voxels)
    if (v.stain) { withStain = true; break; }
  const size_t words =
      WordsFor(cellCount) + (withStain ? StainWordsFor(cellCount) : 0);
  if (set.pool.size() + words > kMicroBodyPoolWordsWorld) {
    log += label + ": micro body brick pool full (" +
           std::to_string(kMicroBodyPoolWordsWorld) + " words)\n";
    return -1;
  }

  std::vector<uint16_t> cells(cellCount, 0);
  std::vector<uint8_t> stains(withStain ? cellCount : 0, 0);
  for (const PrefabVoxel& v : voxels) {
    if (v.x < 0 || v.y < 0 || v.z < 0 || v.x >= dims.x || v.y >= dims.y ||
        v.z >= dims.z)
      continue;  // loader guarantees in-box; a stray voxel is just dropped
    // Mask off the cosmetic variant nibble before range-checking (WriteBrick).
    const uint16_t mat = v.material & 0xFFF;
    if (mat == 0 || mat > 255) {
      // 8 bits of the 16 go to the material id; naming the limit beats a
      // silently truncated id painting the wrong colour.
      log += label + ": micro body voxel material id " + std::to_string(mat) +
             " out of range 1..255\n";
      return -1;
    }
    cells[((size_t)v.z * dims.y + v.y) * dims.x + v.x] =
        MicroVox((uint8_t)mat, v.color);
    if (withStain) stains[((size_t)v.z * dims.y + v.y) * dims.x + v.x] = v.stain;
  }

  const uint32_t base = (uint32_t)set.pool.size();
  const size_t payloadWords = WordsFor(cellCount);
  for (size_t w = 0; w < payloadWords; w++) {
    uint32_t word = 0;
    for (size_t b = 0; b < 2; b++) {
      size_t idx = w * 2 + b;
      if (idx < cellCount) word |= (uint32_t)cells[idx] << (b * 16);
    }
    set.pool.push_back(word);
  }
  for (size_t w = payloadWords; w < words; w++) {
    uint32_t word = 0;
    for (size_t b = 0; b < 4; b++) {
      size_t idx = (w - payloadWords) * 4 + b;
      if (idx < cellCount) word |= (uint32_t)stains[idx] << (b * 8);
    }
    set.pool.push_back(word);
  }

  MicroBodyModelGpu m{};
  m.base = base;
  m.dims = (uint32_t)dims.x | ((uint32_t)dims.y << 10) | ((uint32_t)dims.z << 20) |
           (withStain ? kMicroBodyDimsStainBit : 0u);
  m.scale = scale;
  m.cutFaces = cutFaces & 0x3Fu;
  set.models.push_back(m);
  // Shared, not owned: load-time models back every instance of their def and
  // must survive any one instance being destroyed.
  set.owned.resize(set.models.size(), 0);
  set.blockWords.resize(set.models.size(), 0);
  set.blockWords.back() = (uint32_t)words;
  set.MarkPool(base, base + (uint32_t)words);
  return (int)set.models.size() - 1;
}

int MicroBodyOwn(MicroBodySet& set, uint32_t model) {
  if (model >= set.models.size()) return -1;
  set.owned.resize(set.models.size(), 0);
  set.blockWords.resize(set.models.size(), 0);
  if (set.owned[model]) return (int)model;  // already private to one body

  // Clone the payload, and give the clone a STAIN LATTICE whether or not the
  // source had one: an owned model is one something is about to mark (carve,
  // burn, bloody), and allocating the lattice here rather than on the first
  // stain poke means the block is never reallocated underneath a body that
  // is being edited in place. Words = payload + stain, from the dims.
  const size_t cells = CellsOf(set.models[model].dims);
  const size_t payloadWords = WordsFor(cells);
  const size_t words = payloadWords + StainWordsFor(cells);
  const bool srcStain = HasStain(set.models[model].dims);

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
  const size_t copyWords = srcStain ? words : payloadWords;
  std::copy(set.pool.begin() + s.base, set.pool.begin() + s.base + copyWords,
            set.pool.begin() + base);
  if (!srcStain)
    std::fill(set.pool.begin() + base + payloadWords,
              set.pool.begin() + base + words, 0u);

  MicroBodyModelGpu& dst = set.models[slot];
  dst = s;
  dst.base = base;
  dst.dims |= kMicroBodyDimsStainBit;
  set.owned[slot] = 1;
  set.blockWords[slot] = (uint32_t)words;
  set.MarkPool(base, base + (uint32_t)words);
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
  // Owned blocks always carry the stain lattice after the payload (see
  // MicroBodyOwn); a model MicroBodyPack made and the caller marked owned may
  // not yet, and grows one here through the same "does not fit" path.
  const size_t cells = (size_t)dims.x * dims.y * dims.z;
  const size_t words = WordsFor(cells) + StainWordsFor(cells);

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
  m.dims = (uint32_t)dims.x | ((uint32_t)dims.y << 10) | ((uint32_t)dims.z << 20) |
           kMicroBodyDimsStainBit;
  WriteBrick(set, m.base, dims, voxels, mn, true);  // marks the block dirty
  originShift = mn;
  set.dirty = true;
  return true;
}

bool MicroBodyPoke(MicroBodySet& set, uint32_t model, int x, int y, int z,
                   uint8_t mat, uint8_t art) {
  if (model >= set.models.size()) return false;
  // OWNED only. A shared model backs every instance of its def, so poking one
  // would char every wizard in the world at once — the same reason
  // MicroBodyEdit refuses, and the reason the burn path calls MicroBodyOwn
  // before its first write exactly as the carve path does.
  if (model >= set.owned.size() || !set.owned[model]) return false;
  const MicroBodyModelGpu& m = set.models[model];
  const int dx = (int)(m.dims & 1023), dy = (int)((m.dims >> 10) & 1023),
            dz = (int)((m.dims >> 20) & 1023);
  if (x < 0 || y < 0 || z < 0 || x >= dx || y >= dy || z >= dz) return false;

  const size_t idx = ((size_t)z * dy + y) * dx + x;
  const uint32_t w = m.base + (uint32_t)(idx / 2);
  if (w >= set.pool.size()) return false;
  const uint32_t shift = (uint32_t)(idx % 2) * 16u;
  uint32_t word = set.pool[w];
  word &= ~(0xFFFFu << shift);
  word |= (uint32_t)MicroVox(mat, art) << shift;
  if (word == set.pool[w]) return true;  // already that value: no upload debt
  set.pool[w] = word;
  set.MarkPool(w, w + 1);
  return true;
}

bool MicroBodyPokeStain(MicroBodySet& set, uint32_t model, int x, int y, int z,
                        uint8_t stain) {
  if (model >= set.models.size()) return false;
  if (model >= set.owned.size() || !set.owned[model]) return false;  // shared
  MicroBodyModelGpu& m = set.models[model];
  const int dx = (int)(m.dims & 1023), dy = (int)((m.dims >> 10) & 1023),
            dz = (int)((m.dims >> 20) & 1023);
  if (x < 0 || y < 0 || z < 0 || x >= dx || y >= dy || z >= dz) return false;
  const size_t cells = (size_t)dx * dy * dz;
  const size_t payloadWords = WordsFor(cells);
  if (!HasStain(m.dims)) {
    // A Pack-made owned model (a fragment, a reloaded corpse) without a stain
    // lattice: move it to a block that has room for one. Same free-at-
    // reserved-size discipline MicroBodyEdit follows.
    const size_t words = payloadWords + StainWordsFor(cells);
    const uint32_t base = PoolAlloc(set, words);
    if (base == UINT32_MAX) return false;
    std::copy(set.pool.begin() + m.base, set.pool.begin() + m.base + payloadWords,
              set.pool.begin() + base);
    std::fill(set.pool.begin() + base + payloadWords,
              set.pool.begin() + base + words, 0u);
    PoolFree(set, m.base, set.blockWords[model]);
    m.base = base;
    m.dims |= kMicroBodyDimsStainBit;
    set.blockWords[model] = (uint32_t)words;
    set.MarkPool(base, base + (uint32_t)words);
  }
  const size_t idx = ((size_t)z * dy + y) * dx + x;
  const uint32_t w = m.base + (uint32_t)payloadWords + (uint32_t)(idx / 4);
  if (w >= set.pool.size()) return false;
  const uint32_t shift = (uint32_t)(idx % 4) * 8u;
  uint32_t word = set.pool[w];
  word &= ~(0xFFu << shift);
  word |= (uint32_t)stain << shift;
  if (word == set.pool[w]) return true;
  set.pool[w] = word;
  set.MarkPool(w, w + 1);
  return true;
}

IVec3 MicroBodyDims(const MicroBodySet& set, uint32_t model) {
  if (model >= set.models.size()) return IVec3{0, 0, 0};
  const uint32_t d = set.models[model].dims;
  return IVec3{(int)(d & 1023), (int)((d >> 10) & 1023), (int)((d >> 20) & 1023)};
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
