#include "sim/faredits.h"

#include <algorithm>

#include "sim/chunkstore.h"
#include "sim/stream.h"  // RleDecodeChunk

namespace {
// Floor division, mirroring worldgen.wgsl's fdiv: C++ `/` truncates toward
// zero, which puts the sample lattice out of phase at negative coordinates —
// the same bug the WGSL side has a named helper for.
inline int FDiv(int a, int b) {
  int q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) q -= 1;
  return q;
}
// Smallest cascade sample point >= b on one axis. Centers sit at
// c = m*step + half, so m = ceil((b - half) / step). The CPU twin of
// worldgen.wgsl's farFirstCenter, and it must stay one — the patch and the
// sieve address the same cells or the patch lands on the wrong ones.
inline int FirstCenter(int b, int step, int half) {
  return FDiv(b - half + step - 1, step) * step + half;
}
}  // namespace

void FarEdits::Sweep(IVec3 wc,
                     const std::function<uint32_t(int, int, int)>& sample) {
  const int base[3] = {wc.x * (int)kChunk, wc.y * (int)kChunk,
                       wc.z * (int)kChunk};
  for (uint32_t level = 1; level <= kFarLevels; level++) {
    const int shift = (int)(level + kFarShiftBase);
    const int step = 1 << shift;
    const int half = 1 << (shift - 1);
    int first[3], n[3];
    for (int a = 0; a < 3; a++) {
      first[a] = FirstCenter(base[a], step, half);
      const int span = base[a] + (int)kChunk - first[a];
      n[a] = span <= 0 ? 0 : (span + step - 1) / step;
    }
    // At level 3 a cell is already 16 fine voxels wide, so most chunks
    // contribute one cell or none at the coarse levels — correct: the chunk
    // that OWNS the center voxel is the one that speaks for the cell.
    if (n[0] == 0 || n[1] == 0 || n[2] == 0) continue;

    scratch_.clear();
    // iz/iy/ix ascending == cell index ascending (chunk-linear is x-fastest),
    // so one fine chunk's run arrives already sorted and a level chunk fed by
    // exactly one chunk needs no compaction at all.
    for (int iz = 0; iz < n[2]; iz++)
      for (int iy = 0; iy < n[1]; iy++)
        for (int ix = 0; ix < n[0]; ix++) {
          const int fine[3] = {first[0] + ix * step, first[1] + iy * step,
                               first[2] + iz * step};
          const uint32_t mat =
              sample(fine[0] - base[0], fine[1] - base[1], fine[2] - base[2]);
          // level-cell coord -> its index inside the owning level chunk
          const uint32_t cx = (uint32_t)((fine[0] >> shift) & 15);
          const uint32_t cy = (uint32_t)((fine[1] >> shift) & 15);
          const uint32_t cz = (uint32_t)((fine[2] >> shift) & 15);
          const uint32_t ci = (cz * kChunk + cy) * kChunk + cx;
          scratch_.push_back(((mat & 0xFFFu) << kCellBits) | ci);
        }
    const IVec3 lc = LevelChunkOf(wc, level);
    Append({lc.x, lc.y, lc.z, level}, scratch_);
  }
}

void FarEdits::NoteChunk(IVec3 wc, const uint32_t* words) {
  Sweep(wc, [words](int lx, int ly, int lz) {
    return words[(uint32_t)lx + (uint32_t)ly * kChunk +
                 (uint32_t)lz * kChunk * kChunk] &
           0xFFFu;
  });
}

void FarEdits::NoteUniformChunk(IVec3 wc, uint32_t mat) {
  Sweep(wc, [mat](int, int, int) { return mat & 0xFFFu; });
}

void FarEdits::Append(const LKey& key, const std::vector<uint32_t>& add) {
  if (add.empty()) return;
  Chunk& c = byChunk_[key];
  if (c.words.empty()) {
    c.words = add;          // already sorted, one word per cell
    c.sorted = true;
  } else {
    c.words.insert(c.words.end(), add.begin(), add.end());
    c.sorted = false;
  }
  cells_ += add.size();
}

const std::vector<uint32_t>* FarEdits::Lookup(uint32_t level,
                                              IVec3 levelChunk) {
  auto it = byChunk_.find({levelChunk.x, levelChunk.y, levelChunk.z, level});
  if (it == byChunk_.end()) return nullptr;
  Chunk& c = it->second;
  if (!c.sorted) {
    // STABLE, so equal cell indices keep append order and the LAST of a run is
    // the newest read. The dedupe below therefore keeps the last of each run
    // (it skips an entry whose successor shares its cell index) rather than
    // the first, which std::unique would have kept.
    std::stable_sort(c.words.begin(), c.words.end(),
                     [](uint32_t a, uint32_t b) {
                       return (a & kCellMask) < (b & kCellMask);
                     });
    size_t w = 0;
    for (size_t r = 0; r < c.words.size(); r++) {
      // last of each run of equal cell indices
      if (r + 1 < c.words.size() &&
          (c.words[r] & kCellMask) == (c.words[r + 1] & kCellMask))
        continue;
      c.words[w++] = c.words[r];
    }
    cells_ -= c.words.size() - w;
    c.words.resize(w);
    c.sorted = true;
  }
  return &c.words;
}

size_t FarEdits::RebuildFromStore(ChunkStore& store) {
  Clear();
  std::vector<uint32_t> words(kChunkVol);
  size_t n = 0;
  store.ForEachStored([&](IVec3 wc, const uint32_t* rle, size_t pairs) {
    if (!RleDecodeChunk(rle, pairs, words.data())) return;
    NoteChunk(wc, words.data());
    n++;
  });
  return n;
}
