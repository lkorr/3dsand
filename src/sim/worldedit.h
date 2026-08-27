// worldedit.h — the AUTHORED EDIT LAYER over worldgen.
//
// WHAT IT IS. A sparse, chunk-keyed patch of world cell -> voxel word, authored
// in the tuner's Worldgen tab (assets/worldview.js) and saved to
// assets/worldedits/<name>.svedit. When a layer is named by
// `worldgen.editLayer` in tuning.json, the engine applies it to every chunk it
// generates — at startup worldgen and on every streaming refill — so a
// hand-built structure is part of the world rather than part of one session.
//
// WHY A LAYER AND NOT A SAVE. The world already has two places edits can live
// and neither one does this job:
//
//   * ChunkStore (`world.svd/`) is a LIVE WORLD, snapshotted per region. It
//     records what a played world became, so it is pinned to one seed and one
//     history and it cannot compose with a worldgen change — the whole point of
//     the Worldgen tab is that you are still moving the sliders.
//   * FarEdits is DERIVED, disposable cascade state, rebuilt from the store.
//
// This is neither: it is authored content, small, diffable, seed-independent
// and version-controlled next to the prefabs and mob defs it sits beside. The
// same file is what the viewer draws and what the game loads, so what you built
// in the browser is what you spawn into.
//
// RULE 3 (CLAUDE.md): it does NOT write voxels. It emits CellOps and they go
// through the MutationQueue like every other mutation, which is what keeps the
// layer inside the save/replay/network stream for free. That is also why
// application is deferred by a tick rather than folded into genChunk: the queue
// is the seam, and a second writer into the voxel buffer would be a second
// truth about what a chunk contains.
//
// DETERMINISM. Ops are emitted in a fixed order — chunks in the order they were
// queued, cells in ascending local index — and a chunk is queued exactly once
// per generation of that chunk. Two runs of the same seed with the same layer
// therefore see the same op stream on the same ticks, so the world hash is
// reproducible. A layer that is present at all MOVES the hash, which is correct
// and is why `worldgen.editLayer` is empty by default and no gate sets it.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

namespace sandvox {

class WorldEdits {
 public:
  // Reads a .svedit. False + `err` on a missing or malformed file; the layer is
  // left empty, which is the safe direction — a world with no edits rather than
  // a world with half of them.
  bool Load(const std::string& path, std::string& err);
  void Clear();

  bool Empty() const { return byChunk_.empty(); }
  size_t ChunkCount() const { return byChunk_.size(); }
  size_t VoxelCount() const { return voxelCount_; }
  const std::string& Name() const { return name_; }

  // Queue one world chunk's edits for application, if it has any. Called with
  // every chunk that worldgen has just (re)written.
  void QueueChunk(IVec3 wc);
  // Every chunk of the current residency window that the layer touches. The
  // startup path: worldgen fills the whole window at once.
  void QueueWindow(const World& world);

  bool HasPending() const { return cursor_ < pending_.size(); }
  size_t PendingChunks() const { return pending_.size() - cursor_; }

  // Append up to `max` CellOps for queued chunks into `out`, and report how
  // many were appended. Chunks that have scrolled out of the window since they
  // were queued are DROPPED rather than clamped: a cell index is window
  // relative, so applying one for a chunk that is no longer resident would
  // write into whatever now occupies that slot.
  uint32_t Drain(const World& world, std::vector<CellOp>& out, uint32_t max);

 private:
  static uint64_t Key(IVec3 wc) { return World::PackChunkKey(wc); }

  struct Cell {
    uint32_t localIdx;  // 0..kChunkVol-1, chunk-linear, x fastest
    uint32_t word;
  };
  std::unordered_map<uint64_t, std::vector<Cell>> byChunk_;
  std::unordered_map<uint64_t, IVec3> coords_;
  std::vector<IVec3> pending_;
  size_t cursor_ = 0;       // next pending chunk
  size_t within_ = 0;       // next cell inside pending_[cursor_]
  size_t voxelCount_ = 0;
  std::string name_;
};

// The process-wide layer. One, because the layer is a property of the WORLD and
// every producer of chunks (startup worldgen, the streamer, a regen) has to
// consult the same one — three owners would mean three answers to "has this
// chunk been patched".
WorldEdits& WorldEditLayer();

// Loads `worldgen.editLayer` from the asset directory if it names one, and
// reports what happened. Safe to call again after a tuning reload.
void LoadWorldEditLayerFromTuning(const std::string& assetDir);

}  // namespace sandvox
