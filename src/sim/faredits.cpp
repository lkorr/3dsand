#include "sim/faredits.h"

#include <algorithm>

#include "sim/chunkstore.h"
#include "sim/stream.h"  // RleDecodeChunk
#include "sim/treeatlas.h"  // the run word (a stamp block uses the tree atlas layout)
#include "sim/world.h"   // World::TerrainHeight, the ground slot's h
#include "sim/worldmap.h"  // the stamps' container cells (StampContainers)

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
    // iz/ix/iy ascending == ColumnKey ascending (z, then x, then y), so one
    // fine chunk's run arrives already in Lookup's order and a level chunk
    // fed by exactly one chunk needs no compaction at all.
    for (int iz = 0; iz < n[2]; iz++)
      for (int ix = 0; ix < n[0]; ix++)
        for (int iy = 0; iy < n[1]; iy++) {
          const int fine[3] = {first[0] + ix * step, first[1] + iy * step,
                               first[2] + iz * step};
          uint32_t mat = sample(fine[0] - base[0], fine[1] - base[1], fine[2] - base[2]);
          // THE CONTAINER RULE (faredits.h kContainMaxShift): the cell lies
          // inside this chunk at these shifts; its face neighbours may not.
          if (shift <= kContainMaxShift && !containKind_.empty()) {
            const int c0[3] = {fine[0] - half - base[0], fine[1] - half - base[1],
                               fine[2] - half - base[2]};
            mat = ContainedBy(mat & 0xFFFu, step, c0, [&](int x, int y, int z) {
              const int k = (int)kChunk;
              if (x < 0 || y < 0 || z < 0 || x >= k || y >= k || z >= k) return kMatAtUnseen;
              return (int)(sample(x, y, z) & 0xFFFu);
            });
          }
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

  // ---- the GROUND words (kGroundShift): every sample column of this chunk
  // whose ground voxel lies in it. Separate from the centre sweep above
  // because the ground needs the XZ columns only -- a chunk can hold a column's
  // ground while holding no cell centre at a coarse level.
  if (groundSlot_.empty() || !groundWorld_) return;
  std::unordered_map<LKey, std::vector<uint32_t>, LKeyHash> ground;
  for (uint32_t level = 1; level <= kFarLevels; level++) {
    const int shift = (int)(level + kFarShiftBase);
    if (shift >= (int)kGroundMaxShift) break;
    const int step = 1 << shift;
    const int half = 1 << (shift - 1);
    const int fx = FirstCenter(base[0], step, half);
    const int fz = FirstCenter(base[2], step, half);
    for (int z = fz; z < base[2] + (int)kChunk; z += step)
      for (int x = fx; x < base[0] + (int)kChunk; x += step) {
        const int h = GroundHeight(x, z);
        if (h < base[1] || h >= base[1] + (int)kChunk) continue;
        GroundWord((uint32_t)level, x, h, z,
                   sample(x - base[0], h - base[1], z - base[2]), ground);
      }
  }
  for (auto& kv : ground) {
    std::sort(kv.second.begin(), kv.second.end(),
              [](uint32_t a, uint32_t b) { return ColumnKey(a) < ColumnKey(b); });
    Append(kv.first, kv.second);
  }
}

int FarEdits::GroundHeight(int x, int z) {
  const uint32_t seed = groundWorld_->WorldSeed();
  const uint64_t key = ((uint64_t)seed << 40) ^ ((uint64_t)(uint32_t)x << 20) ^
                       (uint64_t)(uint32_t)z ^ ((uint64_t)(uint32_t)z << 52);
  auto it = groundH_.find(key);
  if (it != groundH_.end()) return it->second;
  if (groundH_.size() > (1u << 20)) groundH_.clear();
  const int h = World::TerrainHeight(x, z, seed);
  groundH_.emplace(key, h);
  return h;
}

void FarEdits::GroundWord(uint32_t level, int x, int h, int z, uint32_t mat,
                          std::unordered_map<LKey, std::vector<uint32_t>, LKeyHash>& out) const {
  const int shift = (int)(level + kFarShiftBase);
  const int step = 1 << shift;
  const int half = 1 << (shift - 1);
  // The cell worldgen's farSurfaceMat colours from this ground: its centre is
  // the highest one at or below h (centre <= h < centre + step).
  const int cyFine = FDiv(h - half, step) * step + half;
  const int cx = x >> shift, cy = cyFine >> shift, cz = z >> shift;
  const uint32_t ci = (uint32_t)(((cz & 15) * (int)kChunk + (cy & 15)) * (int)kChunk + (cx & 15));
  out[LKey{cx >> 4, cy >> 4, cz >> 4, level}].push_back(
      (GroundSlotOf(mat & 0xFFFu) << kGroundShift) | (kMatUntouched << kCellBits) | ci);
}

uint32_t FarEdits::CombineCell(const uint32_t* w, size_t n) {
  uint32_t mat = kMatUntouched, gs = kGroundUnknown;
  for (size_t i = 0; i < n; i++) {
    const uint32_t m = (w[i] >> kCellBits) & 0xFFFu;
    const uint32_t g = w[i] >> kGroundShift;
    if (m != kMatUntouched) mat = m;
    if (g != kGroundUnknown) gs = g;
  }
  return (gs << kGroundShift) | (mat << kCellBits) | (w[0] & kCellMask);
}

uint32_t FarEdits::ContainedBy(uint32_t centre, int step, const int c0[3],
                               const std::function<int(int, int, int)>& matAt) const {
  if (ContainKindOf(centre) == kContainWall) return centre;
  // A neighbour's class. Unseen counts as OPEN, the GPU's answer for a cell
  // outside the window; kMatAtGround is a wall whose material is unknown.
  auto kindAt = [&](int x, int y, int z) -> uint8_t {
    const int m = matAt(x, y, z);
    if (m == kMatAtUnseen) return kContainNone;
    if (m == kMatAtGround) return kContainWall;
    return ContainKindOf((uint32_t)m);
  };
  // +x, -x, +y, -y, +z, -z: worldgen.wgsl farFaceDir's order (the result is
  // an OR over the six, so the order only has to be complete).
  static constexpr int kDir[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                     {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (int i = 0, n3 = step * step * step; i < n3; i++) {
    const int x = c0[0] + i % step, y = c0[1] + (i / step) % step, z = c0[2] + i / (step * step);
    const int m = matAt(x, y, z);
    // A candidate must be a wall this side can NAME: an unseen voxel or
    // procgen ground under the edit layer is skipped (the GPU, which sees
    // it, may take it; a documented downsample-only answer).
    if (m < 0 || ContainKindOf((uint32_t)m) != kContainWall) continue;
    bool liquid = false, open = false;
    for (const auto& d : kDir) {
      const uint8_t k = kindAt(x + d[0], y + d[1], z + d[2]);
      liquid = liquid || k == kContainLiquid;
      open = open || k == kContainNone;
    }
    if (liquid && open) return (uint32_t)m;
  }
  return centre;
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
  merged_.erase(key);
  auto it = byChunk_.find(key);
  if (it == byChunk_.end()) {
    // The cap refuses only NEW level chunks (see kMaxCells): one already
    // indexed is bounded by the compaction below, and refusing its update
    // would leave a stale edit on the horizon rather than a missing one.
    if (cells_ + add.size() > kMaxCells) { refused_++; return; }
    Chunk& c = byChunk_[key];
    c.words = add;          // already in ColumnKey order, one word per cell
    c.sorted = true;
    cells_ += add.size();
    return;
  }
  Chunk& c = it->second;
  c.words.insert(c.words.end(), add.begin(), add.end());
  c.sorted = false;
  cells_ += add.size();
  // THE BOUND ON A RE-NOTED CHUNK. Appending and compacting on first read is
  // what keeps a bulk build O(n log n) (see Lookup), but a level chunk that is
  // re-noted over and over — the same edited fine chunks evicted and
  // re-entered every time the player paces past a window face — and never
  // read grew without limit. A level chunk has at most kChunkVol distinct
  // cells, so past twice that the list is mostly superseded words: compact.
  if (c.words.size() > 2 * (size_t)kChunkVol) Compact(c);
}

void FarEdits::Compact(Chunk& c) {
  // STABLE, so equal cells keep append order and the LAST of a run is the
  // newest read. The dedupe below therefore keeps the last of each run (it
  // skips an entry whose successor is the same cell) rather than the first,
  // which std::unique would have kept.
  std::stable_sort(c.words.begin(), c.words.end(), [](uint32_t a, uint32_t b) {
    return ColumnKey(a) < ColumnKey(b);
  });
  // Each run of one cell COMBINES (CombineCell): its centre and its ground
  // can come from different fine chunks, so "the last word" would drop one.
  size_t w = 0;
  for (size_t r = 0; r < c.words.size();) {
    size_t e = r + 1;
    while (e < c.words.size() && (c.words[e] & kCellMask) == (c.words[r] & kCellMask)) e++;
    c.words[w++] = CombineCell(&c.words[r], e - r);
    r = e;
  }
  cells_ -= c.words.size() - w;
  c.words.resize(w);
  c.sorted = true;
}

const std::vector<uint32_t>* FarEdits::Lookup(uint32_t level,
                                              IVec3 levelChunk) {
  const LKey key{levelChunk.x, levelChunk.y, levelChunk.z, level};
  auto bit = base_.find(key);
  auto it = byChunk_.find(key);
  if (it == byChunk_.end())
    return bit == base_.end() ? nullptr : &bit->second;
  Chunk& c = it->second;
  if (!c.sorted) Compact(c);
  if (bit == base_.end()) return &c.words;
  // Both: merge once, noted words winning per cell. Two ColumnKey-sorted
  // lists with one word per cell, so a linear merge keeps the order Lookup
  // promises.
  auto mit = merged_.find(key);
  if (mit != merged_.end()) return &mit->second;
  std::vector<uint32_t>& m = merged_[key];
  const std::vector<uint32_t>& a = bit->second;
  const std::vector<uint32_t>& b = c.words;
  m.reserve(a.size() + b.size());
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    if (j >= b.size() || (i < a.size() && ColumnKey(a[i]) < ColumnKey(b[j]))) {
      m.push_back(a[i++]);
    } else if (i >= a.size() || ColumnKey(b[j]) < ColumnKey(a[i])) {
      m.push_back(b[j++]);
    } else {
      // The same cell: the noted edit is newer, field by field.
      const uint32_t pair[2] = {a[i++], b[j++]};
      m.push_back(CombineCell(pair, 2));
    }
  }
  return &m;
}

void FarEdits::StampContainers(std::unordered_map<LKey, std::vector<uint32_t>, LKeyHash>& out) {
  if (containKind_.empty() || World::LabWorld()) return;
  using worldmap::WorldMapData;
  const WorldMapData& map = worldmap::CurrentWorldMap();
  std::vector<const WorldMapData::StampSite*> stamps;
  for (const auto& s : map.sites)
    if (s.kind == worldmap::kSiteStamp && !s.words.empty()) stamps.push_back(&s);
  if (stamps.empty()) return;
  // worldgen.wgsl wmStampCell over the CPU's copy of the block (offsets
  // relative to its start): the template voxel at a world cell, 0 if none.
  // Run word: material 0..11, y0 16..26, length 27..31 (treeatlas.h).
  auto stampAt = [](const WorldMapData::StampSite& s, int x, int y, int z) -> uint32_t {
    const int lx = x - (s.x - s.nx / 2), lz = z - (s.z - s.nz / 2);
    const int ly = y - s.padY - 1 + s.sink;
    if (lx < 0 || lz < 0 || ly < 0 || lx >= s.nx || lz >= s.nz || ly >= s.ny) return 0;
    const size_t ci = worldmap::kStampHdrWords + ((size_t)lz * s.nx + lx) * 2;
    const uint32_t runOff = s.words[ci], cnt = s.words[ci + 1];
    for (uint32_t k = 0; k < cnt; k++) {
      const uint32_t r = s.words[runOff + k];
      const int y0 = (int)((r >> 16) & treeatlas::kRunMaxY0);
      if (ly < y0) return 0;
      if (ly < y0 + (int)(r >> 27)) return r & 0xFFFu;
    }
    return 0;
  };
  // What genCellIn puts at a cell, as far as this side can say: the first
  // stamp's template voxel (wmStampsCell); under template air the PAD's
  // ground (levelled to padY over the footprint rect) or, off every
  // footprint, the ground contract -- a wall of unknown material at or under
  // it, air over it. Trees, cover and caves under template air are beyond it.
  auto matAt = [&](int x, int y, int z) -> int {
    for (const auto* s : stamps)
      if (const uint32_t m = stampAt(*s, x, y, z)) return (int)m;
    for (const auto* s : stamps)
      if (x >= s->bx0 && x <= s->x1 && z >= s->bz0 && z <= s->z1)
        return y <= s->padY ? kMatAtGround : (int)kMatAir;
    if (!groundWorld_) return kMatAtUnseen;
    return y <= GroundHeight(x, z) ? kMatAtGround : (int)kMatAir;
  };
  for (const auto* sp : stamps) {
    const WorldMapData::StampSite& s = *sp;
    // The world box of the template's LIQUID: a container wall touches it, so
    // every cell the rule can change meets the box grown by one.
    int lo[3] = {INT32_MAX, INT32_MAX, INT32_MAX}, hi[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
    for (int lz = 0; lz < s.nz; lz++)
      for (int lx = 0; lx < s.nx; lx++) {
        const size_t ci = worldmap::kStampHdrWords + ((size_t)lz * s.nx + lx) * 2;
        for (uint32_t k = 0; k < s.words[ci + 1]; k++) {
          const uint32_t r = s.words[s.words[ci] + k];
          if (ContainKindOf(r & 0xFFFu) != kContainLiquid) continue;
          const int y0 = (int)((r >> 16) & treeatlas::kRunMaxY0) + s.padY + 1 - s.sink;
          const int w[3] = {lx + s.x - s.nx / 2, y0, lz + s.z - s.nz / 2};
          for (int a = 0; a < 3; a++) {
            lo[a] = std::min(lo[a], w[a] - 1);
            hi[a] = std::max(hi[a], w[a] + 1 + (a == 1 ? (int)(r >> 27) - 1 : 0));
          }
        }
      }
    if (lo[0] > hi[0]) continue;
    for (uint32_t level = 1; level <= kFarLevels; level++) {
      const int shift = (int)(level + kFarShiftBase);
      if (shift > kContainMaxShift) break;
      const int step = 1 << shift, half = 1 << (shift - 1);
      for (int cz = lo[2] >> shift; cz <= hi[2] >> shift; cz++)
        for (int cy = lo[1] >> shift; cy <= hi[1] >> shift; cy++)
          for (int cx = lo[0] >> shift; cx <= hi[0] >> shift; cx++) {
            const int c0[3] = {cx * step, cy * step, cz * step};
            int centre = matAt(c0[0] + half, c0[1] + half, c0[2] + half);
            if (centre == kMatAtGround) continue;   // ground: already a wall
            if (centre < 0) centre = (int)kMatAir;
            const uint32_t mat = ContainedBy((uint32_t)centre, step, c0, matAt);
            if (mat == (uint32_t)centre) continue;
            const uint32_t ci =
                (uint32_t)(((cz & 15) * (int)kChunk + (cy & 15)) * (int)kChunk + (cx & 15));
            out[LKey{cx >> 4, cy >> 4, cz >> 4, level}].push_back((mat << kCellBits) | ci);
          }
    }
  }
}

void FarEdits::SetBase(const std::vector<BaseCell>& cells) {
  base_.clear();
  merged_.clear();
  // The stamps' container cells first, so the layer's own words (appended
  // below, and the compaction keeps the last of a cell) win where both speak.
  StampContainers(base_);
  // The container rule needs a liquid centre's cell-mates. The layer is
  // sparse, so they come from a position index of the layer's own cells; a
  // cell-mate the layer does not hold is procgen, which the CPU cannot see,
  // and does not count (the trough this exists for is all layer).
  std::unordered_map<uint64_t, uint32_t> at;
  auto posKey = [](int x, int y, int z) {
    return ((uint64_t)(uint32_t)(x & 0x1FFFFF) << 42) |
           ((uint64_t)(uint32_t)(y & 0x1FFFFF) << 21) | (uint64_t)(uint32_t)(z & 0x1FFFFF);
  };
  if (!containKind_.empty()) {
    at.reserve(cells.size());
    for (const BaseCell& c : cells) at[posKey(c.x, c.y, c.z)] = c.mat & 0xFFFu;
  }
  for (const BaseCell& c : cells) {
    for (uint32_t level = 1; level <= kFarLevels; level++) {
      const int shift = (int)(level + kFarShiftBase);
      const int step = 1 << shift;
      const int half = 1 << (shift - 1);
      // A cell speaks for a cascade sample only if it IS that sample's center
      // voxel (Sweep's rule): c = m*step + half on every axis. Two's-complement
      // masking makes this right for negative coordinates too.
      if (((c.x - half) & (step - 1)) != 0 || ((c.y - half) & (step - 1)) != 0 ||
          ((c.z - half) & (step - 1)) != 0)
        continue;
      const uint32_t cx = (uint32_t)((c.x >> shift) & 15);
      const uint32_t cy = (uint32_t)((c.y >> shift) & 15);
      const uint32_t cz = (uint32_t)((c.z >> shift) & 15);
      const uint32_t ci = (cz * kChunk + cy) * kChunk + cx;
      const LKey key{c.x >> (shift + 4), c.y >> (shift + 4), c.z >> (shift + 4), level};
      base_[key].push_back(((c.mat & 0xFFFu) << kCellBits) | ci);
    }
  }
  // THE CONTAINER RULE over the layer (faredits.h kContainMaxShift). The
  // candidate cells are the ones holding a layer WALL -- only a voxel this
  // side can name may win (ContainedBy) -- and each is decided once, after the centre
  // words above, so its word is the newer one at compaction. A centre the
  // layer does not hold is procgen: ground at or under the column's h, air
  // above it (a tree or a cover stalk there is beyond what the CPU can see).
  if (!containKind_.empty()) {
    // A voxel the layer does not hold is procgen: guessed from the ground
    // contract as above (air over h, ground at or under it), unseen without it.
    auto matAt = [&](int x, int y, int z) {
      auto it = at.find(posKey(x, y, z));
      if (it != at.end()) return (int)it->second;
      if (!groundWorld_) return kMatAtUnseen;
      return y <= GroundHeight(x, z) ? kMatAtGround : (int)kMatAir;
    };
    for (uint32_t level = 1; level <= kFarLevels; level++) {
      const int shift = (int)(level + kFarShiftBase);
      if (shift > kContainMaxShift) break;
      const int step = 1 << shift;
      const int half = 1 << (shift - 1);
      std::unordered_map<uint64_t, bool> seen;
      for (const BaseCell& c : cells) {
        if (ContainKindOf(c.mat & 0xFFFu) != kContainWall) continue;
        const int cell[3] = {c.x >> shift, c.y >> shift, c.z >> shift};
        if (!seen.emplace(posKey(cell[0], cell[1], cell[2]), true).second) continue;
        const int c0[3] = {cell[0] * step, cell[1] * step, cell[2] * step};
        const int fx = c0[0] + half, fy = c0[1] + half, fz = c0[2] + half;
        int centre = matAt(fx, fy, fz);
        if (centre == kMatAtGround) continue;   // procgen ground: already a wall
        if (centre < 0) centre = (int)kMatAir;
        const uint32_t mat = ContainedBy((uint32_t)centre, step, c0, matAt);
        if (mat == (uint32_t)centre) continue;
        const uint32_t ci = (uint32_t)(((cell[2] & 15) * (int)kChunk + (cell[1] & 15)) *
                                           (int)kChunk +
                                       (cell[0] & 15));
        base_[LKey{cell[0] >> 4, cell[1] >> 4, cell[2] >> 4, level}].push_back(
            (mat << kCellBits) | ci);
      }
    }
  }
  // The layer's cells AT the ground of a sample column also speak for the
  // ground slot (kGroundShift): a flush track is exactly the case the centre
  // rule above cannot see.
  if (!groundSlot_.empty() && groundWorld_) {
    for (const BaseCell& c : cells) {
      for (uint32_t level = 1; level <= kFarLevels; level++) {
        const int shift = (int)(level + kFarShiftBase);
        if (shift >= (int)kGroundMaxShift) break;
        const int step = 1 << shift;
        const int half = 1 << (shift - 1);
        if (((c.x - half) & (step - 1)) != 0 || ((c.z - half) & (step - 1)) != 0) continue;
        if (c.y != GroundHeight(c.x, c.z)) continue;
        GroundWord(level, c.x, c.y, c.z, c.mat, base_);
      }
    }
  }
  for (auto& kv : base_) {
    // Compact's sort + last-wins dedupe, without touching the noted-cell
    // counter it maintains (the base is not part of the cap).
    Chunk tmp;
    tmp.words = std::move(kv.second);
    const size_t before = cells_;
    cells_ += tmp.words.size();
    Compact(tmp);
    cells_ = before;
    kv.second = std::move(tmp.words);
  }
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
