#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

class ChunkStore;

// ---- far-field EDIT INDEX (cascade edit persistence) ----------------------
//
// THE GAP THIS CLOSES. The cascades have two producers and they disagree about
// history:
//
//   * worldgen.wgsl `fardown` (plan phase 2) downsamples the LIVE voxel grid
//     for every chunk on the dirty list, so an edit made inside the residency
//     window immediately leaves a ghost in every cascade level. That ghost is
//     durable — until something refills its level chunk.
//   * worldgen.wgsl `far` (the sieve) fills a level chunk from PRISTINE
//     procgen. It runs on every incoming plane (the level's window recentering
//     past the edit and back), on every ResetLevel (teleport / load) and on
//     every FullRefill (startup, load, regen).
//
// So the sieve un-does the downsample. Walk far enough that a level's box no
// longer contains your crater and walk back, or reload the world you dug it
// in, and the horizon heals itself. This index is the memory that survives
// that: it records, per cascade level, the material each cascade cell's SAMPLE
// VOXEL held the last time the CPU saw the chunk that owns it, and FarField
// hands those cells to the sieve as a patch list so the fill lands
// edited-then-pristine instead of pristine.
//
// STILL DERIVED, STILL DISPOSABLE (DESIGN.md §9). The index is reconstructible
// from the ChunkStore region files — the same "seed + persisted edits" the
// world itself reconstructs from — and RebuildFromStore does exactly that on
// load. It is never the authority for a voxel, never read by the sim, never
// hashed.
//
// WHAT THE CPU CAN AND CANNOT COMPUTE. The far-field cell rule is
//   byte = farSurfaceMat(material at the cell's center voxel, ...)
// and `farSurfaceMat` needs `genCell`, which exists only in WGSL. So this
// index deliberately stores the RAW MATERIAL at the sample voxel and nothing
// else; the surface-skin recolor is applied by the same GPU function the sieve
// and the downsample already call. That is what keeps a patched cell
// byte-identical to what `fardown` would have written for it — the
// sieve/downsample agreement invariant the `far-downsample` gate protects
// extends to the patch for free, instead of becoming a third rule that has to
// be kept in sync.
//
// RESOLUTION (the honest part). The sample rule is unchanged: a level-k cell
// is the single fine voxel at the center of the 2^(k + kFarShiftBase)-wide
// region it covers. An edit is therefore representable at level k only if it
// changes a voxel at a cell center — in practice, only if its linear extent
// reaches a whole cell. See DESIGN.md §9 for the per-level table.
class FarEdits {
 public:
  // GPU patch word, matching the `far` kernel's unpack: cell index inside its
  // level chunk (0..kChunkVol-1, chunk-linear x-fastest) in the low 12 bits,
  // raw material id in the next 12.
  static constexpr uint32_t kCellBits = 12;
  static constexpr uint32_t kCellMask = (1u << kCellBits) - 1u;
  static_assert(kChunkVol <= (1u << kCellBits), "cell index must fit 12 bits");
  static_assert(kMaterialSlots <= (1u << 12), "material id must fit 12 bits");

  void Clear() {
    byChunk_.clear();
    cells_ = 0;
  }
  bool Empty() const { return byChunk_.empty(); }
  size_t LevelChunks() const { return byChunk_.size(); }
  // Samples recorded (before compaction — a cell re-noted by a second
  // eviction of the same chunk counts twice until its level chunk is read).
  // Diagnostics only.
  size_t Cells() const { return cells_; }

  // Record one fine chunk's contribution to every cascade level. `words` is
  // kChunkVol voxel words in chunk-linear order (the same layout genChunk and
  // RleDecodeChunk use). Re-noting a chunk REPLACES its previous entries.
  void NoteChunk(IVec3 wc, const uint32_t* words);
  // Same, for a page-table sentinel slot whose whole content is one material
  // (EMPTY / UNIFORM / JITTER — JITTER varies only the palette nibble, which
  // the far field does not store).
  void NoteUniformChunk(IVec3 wc, uint32_t mat);

  // Patch words for one level chunk (1-based level, coord in level-CHUNK
  // units), or nullptr when that chunk holds no edits. Sorted by cell index,
  // one word per cell.
  //
  // NOT const, because this is where a level chunk's appended runs are
  // COMPACTED. Merging on every NoteChunk would be quadratic on a bulk build:
  // a level-1 chunk is fed by 64 fine chunks in arbitrary order, so 64 sorted
  // merges into a vector growing to 4,096 is ~130k element copies per level
  // chunk, and RebuildFromStore over a fully-flushed save does that for every
  // level chunk of the window. Appending and sorting once, on first read, is
  // O(n log n) total and moves the cost onto the chunks the fill queue
  // actually asks about.
  const std::vector<uint32_t>* Lookup(uint32_t level, IVec3 levelChunk);

  // Re-derive the whole index from the persisted world. Returns the number of
  // stored chunks visited. O(store) — a load-time cost, never a frame cost.
  size_t RebuildFromStore(ChunkStore& store);

  // The level chunk that owns fine chunk `wc` at 1-based `level`. One fine
  // chunk always lies inside exactly one level chunk: a level-k chunk is
  // 2^(k + kFarShiftBase + 4) fine voxels wide and aligned, which is a
  // multiple of the 16-voxel fine chunk for every k >= 1.
  static IVec3 LevelChunkOf(IVec3 wc, uint32_t level) {
    const int s = (int)(level + kFarShiftBase);
    return {wc.x >> s, wc.y >> s, wc.z >> s};
  }

 private:
  struct LKey {
    int32_t x, y, z;
    uint32_t level;
    bool operator==(const LKey& o) const {
      return x == o.x && y == o.y && z == o.z && level == o.level;
    }
  };
  struct LKeyHash {
    size_t operator()(const LKey& k) const noexcept {
      uint64_t h = 1469598103934665603ull;
      auto mix = [&h](uint32_t v) { h ^= v; h *= 1099511628211ull; };
      mix((uint32_t)k.x);
      mix((uint32_t)k.y);
      mix((uint32_t)k.z);
      mix(k.level);
      return (size_t)(h ^ (h >> 32));
    }
  };

  // One level chunk's patch words. `sorted` is false while unread appends are
  // outstanding; Lookup compacts (stable sort by cell index, last write per
  // cell wins) and sets it.
  struct Chunk {
    std::vector<uint32_t> words;
    bool sorted = true;
  };
  // Append `add` (itself sorted, one word per cell) to the level chunk's list.
  // Later appends win on a repeated cell — re-noting the same fine chunk must
  // not grow the compacted list, and the newer read is the truth.
  void Append(const LKey& key, const std::vector<uint32_t>& add);

  // The per-level sample sweep shared by NoteChunk / NoteUniformChunk: calls
  // `sample(fineOffsetInChunk)` for every cascade cell center that lands in
  // this chunk and returns the raw material for it.
  void Sweep(IVec3 wc, const std::function<uint32_t(int, int, int)>& sample);

  std::unordered_map<LKey, Chunk, LKeyHash> byChunk_;
  size_t cells_ = 0;                // appended samples, before compaction
  std::vector<uint32_t> scratch_;   // per-level `add` list
};
