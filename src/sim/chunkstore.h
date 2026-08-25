#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

// Evicted-chunk store (DESIGN.md §3 streaming). Chunks are grouped into
// 16^3-chunk REGIONS. Unbound (the default) it is pure RAM — the v1 behavior.
// Bound to a directory it becomes a region-file store: regions lazy-load on
// Get, dirty regions are written by Flush() and by LRU spill once more than
// kMaxRamRegions sit in RAM, so long journeys stream to disk instead of
// growing RAM without bound, and saves are per-region instead of monolithic.
//
// A bound directory is a LIVE world store (Minecraft-style), not a snapshot:
// LRU spills may persist chunks between explicit saves. Binding is one
// directory per store lifetime — disk-resident chunks can't follow a rebind —
// so BindSave/BindLoad refuse a different directory until Unbind()/Clear().
class ChunkStore {
 public:
  struct Entry {
    IVec3 wc;
    std::vector<uint32_t> rle;
  };
  static constexpr int kRegionShift = 4;  // 16 chunks (256 voxels) per axis
  static constexpr size_t kMaxRamRegions = 64;

  void Put(IVec3 wc, std::vector<uint32_t> rle);
  // Pointer is valid only until the next Put/Get/Clear: either may LRU-spill
  // the region that owns it. Use immediately.
  const std::vector<uint32_t>* Get(IVec3 wc);

  // Forget everything in RAM and detach from any bound directory. Files are
  // left on disk untouched (a regen must not destroy the last explicit save;
  // the next BindSave overwrites them).
  void Clear() {
    regions_.clear();
    chunkCount_ = 0;
    dir_.clear();
  }
  void Unbind() { dir_.clear(); }
  bool Bound() const { return !dir_.empty(); }
  const std::string& Dir() const { return dir_; }
  size_t Count() const { return chunkCount_; }  // chunks resident in RAM

  // Bind for saving: creates the directory; if the store was unbound, any
  // region files from an earlier session are wiped (this RAM store is the
  // whole world) and every RAM region is marked dirty so the next Flush
  // writes a complete store. Binding to the already-bound dir is a no-op.
  bool BindSave(const std::string& dir);
  // Bind for loading: the directory must exist; RAM contents are discarded
  // in favor of the disk's.
  bool BindLoad(const std::string& dir);
  // Write every dirty region. False if any write failed.
  bool Flush(size_t* regionsOut = nullptr, uint64_t* bytesOut = nullptr);

  // Visit every stored chunk exactly once — RAM first, then whatever else is
  // on disk under a bound directory. `rle`/`pairs` are the chunk's encoded
  // words; the pointer is valid only for the duration of the callback.
  //
  // DELIBERATELY DOES NOT GO THROUGH Get(): a full walk of a large save would
  // pull every region into RAM and then LRU-spill it straight back out, which
  // is both slow and a write amplification on a read-only traversal. Disk
  // regions are streamed with their own FILE handle and never enter regions_.
  //
  // For rebuilding DERIVED indexes over the persisted world (the far-field
  // edit index — see FarEdits::RebuildFromStore). O(store); load-time only.
  using Visitor = std::function<void(IVec3 wc, const uint32_t* rle, size_t pairs)>;
  void ForEachStored(const Visitor& fn);

 private:
  struct Region {
    IVec3 rc{};                                  // region coord (file name)
    std::unordered_map<uint64_t, Entry> chunks;  // packed chunk key
    bool dirty = false;
    bool loaded = false;  // disk contents merged (or known absent)
    uint64_t lastUse = 0;
  };
  static IVec3 RegionOf(IVec3 wc) {
    return {wc.x >> kRegionShift, wc.y >> kRegionShift, wc.z >> kRegionShift};
  }
  std::string RegionPath(IVec3 rc) const;
  Region& Touch(IVec3 rc);
  // Merge the region's disk file into RAM (RAM entries win: they are newer).
  void EnsureLoaded(IVec3 rc, Region& r);
  bool WriteRegion(IVec3 rc, Region& r, uint64_t* bytesOut);
  void SpillOverBudget();

  std::string dir_;
  std::unordered_map<uint64_t, Region> regions_;  // packed region key
  size_t chunkCount_ = 0;
  uint64_t useCounter_ = 0;
};
