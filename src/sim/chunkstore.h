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

  // ---- THE TICK TAG (M9.5-A) ---------------------------------------------
  //
  // `tick` is the sim tick the stored bytes were DECIDED at, not the tick they
  // were written at, and it exists for exactly one question: when two machines
  // both hold a copy of a chunk, whose is newer? The host's store is the
  // truth and a `ChunkPut` that carries an older tick than `TickOf(wc)` is
  // refused rather than applied (PLAN_multiplayer_m9.md M9.5-B).
  //
  // ZERO MEANS UNKNOWN, and that is the single-player default: every existing
  // caller passes no tick, no tag is recorded, `manifest.svt` is never
  // written, and the `.svr` region format is untouched — which is why
  // `save-load` and `region-store` see no change at all. A tagged Put
  // followed by an untagged one DROPS the tag: the untagged bytes are newer
  // and "unknown" is the honest answer for them, where keeping the stale
  // number would claim a freshness the content does not have.
  void Put(IVec3 wc, std::vector<uint32_t> rle, uint32_t tick = 0);
  // The tag, or 0 for an untagged/absent chunk. Deliberately does NOT go
  // through Get(): asking "how old is this?" must not pull the region into
  // RAM and LRU-spill something else out (the ForEachStored argument, applied
  // to a lookup a manifest walk makes thousands of times).
  uint32_t TickOf(IVec3 wc) const;
  // Every stored chunk with its tag, in one pass: the payload of the
  // `ChunkManifest` a late join receives. Built on ForEachStored for the same
  // no-thrash reason; O(store), join-time only.
  void Manifest(std::vector<std::pair<IVec3, uint32_t>>& out);
  // Pointer is valid only until the next Put/Get/Clear: either may LRU-spill
  // the region that owns it. Use immediately.
  const std::vector<uint32_t>* Get(IVec3 wc);

  // Forget everything in RAM and detach from any bound directory. Files are
  // left on disk untouched (a regen must not destroy the last explicit save;
  // the next BindSave overwrites them).
  void Clear() {
    regions_.clear();
    tickTags_.clear();
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

  // manifest.svt: the tick tags, as a flat (wc, tick) list beside the region
  // files. A SIDE MAP rather than a field on Entry, for two reasons that are
  // both about not paying for it in single player: the `.svr` format stays
  // byte-identical (no save-format change, §M9.5-A), and reading a tag costs
  // no region load — which is what lets a manifest of a disk-resident world
  // be answered without dragging every region through RAM. Only NON-ZERO tags
  // are held, so an untagged world's map is empty and its file is never
  // written.
  static constexpr uint32_t kManifestMagic = 0x31545653;  // 'SVT1'
  std::string ManifestPath() const { return dir_ + "/manifest.svt"; }
  void LoadManifest();
  bool WriteManifest();

  std::string dir_;
  std::unordered_map<uint64_t, Region> regions_;  // packed region key
  // Value carries the COORDINATE as well as the tick, because the manifest
  // writer has to emit (wc, tick) and World::PackChunkKey has no inverse —
  // writing one here would be a second copy of that packing to keep in step
  // with the original, which is the "two places must agree" bug this repo
  // keeps paying for. Twelve bytes per tagged chunk, and only tagged ones.
  struct Tag {
    IVec3 wc;
    uint32_t tick = 0;
  };
  std::unordered_map<uint64_t, Tag> tickTags_;  // packed chunk key -> tag
  size_t chunkCount_ = 0;
  uint64_t useCounter_ = 0;
};
