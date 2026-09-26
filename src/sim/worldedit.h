// worldedit.h — the AUTHORED EDIT LAYER over worldgen.
//
// WHAT IT IS. A sparse patch of world cell -> voxel word, authored in the
// tuner's voxel view (assets/worldview.js) and saved to
// assets/worldedits/<name>.svedit. The MAP names it (map.json `editLayer`,
// since map-overhaul P7 — it used to be tuning.json `world.editLayer`), and
// the engine applies it to every chunk it generates — at startup worldgen and
// on every streaming refill — so a hand-built structure is part of the world
// rather than part of one session.
//
// WHY A LAYER AND NOT A SAVE. The world already has two places edits can live
// and neither one does this job:
//
//   * ChunkStore (`world.svd/`) is a LIVE WORLD, snapshotted per region. It
//     records what a played world became, so it is pinned to one seed and one
//     history and it cannot compose with a worldgen change — the whole point of
//     the map page is that you are still moving the sliders.
//   * FarEdits is DERIVED, disposable cascade state, rebuilt from the store.
//
// This is neither: it is authored content, small, diffable and
// version-controlled next to the map that names it.
//
// GROUND-RELATIVE (SVED v2, P7). A v2 file stores each edited COLUMN's cells as
// offsets from the procedural ground at that column, `World::TerrainHeight`
// (which includes the P5 sculpt layer), and the offsets are converted to world
// y at RESOLVE time for the seed and map in force. So a path painted into the
// grass stays in the grass when the ground under it is re-sculpted or the seed
// changes, instead of floating or burying. The cost is per-column shear: a
// structure standing across a slope that CHANGES shape follows each column's
// ground separately. Structures that must stay rigid are stamps (map sites),
// not layer cells. v1 files (absolute cells, chunk-keyed) still load and apply
// exactly as they always did.
//
// IT IS WORLDGEN, NOT A PLAYER EDIT (P7). What that means, concretely:
//
//   * RULE 3: the layer still writes nothing itself. It emits CellOps through
//     the MutationQueue on the tick after the chunk was generated — the op
//     path, not a new writer, and not one of rule 3's exceptions.
//   * SAVES: the ops wake the chunk (DIRTY_R_MUTATE), and a dirty report is
//     what makes Stream mark a chunk modified and store it on eviction and in
//     a save. `AppliedSlotsAt` tells Stream which slots the layer's ops touched
//     on which submit, and Stream ignores a MUTATE-ONLY dirty report there
//     (World::kDirtyMutateOnly). Anything the CA does afterwards — sand that
//     falls, water that finds a gap — reports its own reason bits and marks the
//     chunk modified as usual. So an untouched layer chunk is re-derived from
//     worldgen + layer on re-entry and load, and an edit to the layer file
//     reaches an existing save's untouched chunks.
//   * FAR FIELD: the sieve only knows procgen, so the layer's cells are handed
//     to FarEdits as a BASE patch (FarEdits::SetBase) the cascades get under
//     every stored edit — the horizon shows the layer from the first frame
//     after load, not only after each chunk has been evicted once.
//   * VOXSERVE: BuildVoxRegion patches the generated words with ApplyToChunk,
//     so the tuner draws what the game spawns into (it used to overlay the
//     layer client-side).
//
// DETERMINISM. Ops are emitted in a fixed order — chunks in the order they were
// queued, cells in ascending local index — and a chunk is queued exactly once
// per generation of that chunk. Two runs of the same seed with the same layer
// therefore see the same op stream on the same ticks, so the world hash is
// reproducible. A layer that is present at all MOVES the hash, which is correct
// and is why the shipped maps name none and no gate's map sets one.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

class FarEdits;   // sim/faredits.h (global namespace)

namespace sandvox {

class WorldEdits {
 public:
  // One edited cell in WORLD coordinates (the resolved form, and the input to
  // the writers below).
  struct AbsCell {
    int32_t x, y, z;
    uint32_t word;
  };

  // Reads a .svedit (v1 absolute or v2 ground-relative). False + `err` on a
  // missing or malformed file; the layer is left empty, which is the safe
  // direction — a world with no edits rather than a world with half of them.
  bool Load(const std::string& path, std::string& err);
  bool LoadBytes(const std::vector<uint8_t>& b, const std::string& what,
                 std::string& err);
  void Clear();

  bool Empty() const { return cols_.empty(); }
  bool Relative() const { return relative_; }
  size_t VoxelCount() const { return voxelCount_; }
  size_t ColumnCount() const { return cols_.size(); }
  // Chunks the RESOLVED layer touches (0 before the first Resolve).
  size_t ChunkCount() const { return byChunk_.size(); }
  const std::string& Name() const { return name_; }

  // Converts the file's columns to world cells for `seed` and the map in
  // force (worldmap::CurrentWorldMap). A no-op when neither changed since the
  // last call. A change drops the pending queue: its chunks were queued
  // against cells that no longer exist.
  void Resolve(uint32_t seed);
  // Every resolved cell, chunk by chunk (Resolve first).
  void ResolvedCells(std::vector<AbsCell>& out) const;

  // Queue one world chunk's edits for application, if it has any. Called with
  // every chunk that worldgen has just (re)written, and the seed it used.
  void QueueChunk(IVec3 wc, uint32_t seed);
  // Every chunk of the current residency window that the layer touches, at
  // world.WorldSeed(). The startup path: worldgen fills the whole window.
  void QueueWindow(const World& world);

  bool HasPending() const { return cursor_ < pending_.size(); }
  size_t PendingChunks() const { return pending_.size() - cursor_; }

  // Append up to `max` CellOps for queued chunks into `out`, and report how
  // many were appended. Chunks that have scrolled out of the window since they
  // were queued are DROPPED rather than clamped: a cell index is window
  // relative, so applying one for a chunk that is no longer resident would
  // write into whatever now occupies that slot.
  //
  // `submitSeq` is the World::TicksEncoded() value the submit carrying these
  // ops will have (TicksEncoded() + 1 at drain time); 0 = do not record. See
  // AppliedSlotsAt.
  uint32_t Drain(const World& world, std::vector<CellOp>& out, uint32_t max,
                 uint32_t submitSeq = 0);

  // The residency SLOTS whose dirty flag the layer's ops set on submit
  // `submitSeq` — every chunk an op landed in plus its 26 neighbours (the
  // dirtyFanSlot reach of sim_mutate) — sorted, or nullptr. Stream's modified
  // fold reads it (see the header comment). Remembers the last kAppliedRing
  // submits, which covers World::kSnapshotLatency with room to spare.
  const std::vector<uint32_t>* AppliedSlotsAt(uint32_t submitSeq) const;
  // The submit seqs the ring holds, oldest first (diagnostics: the worldedit
  // gate names them when the exemption did not fire).
  std::vector<uint32_t> AppliedSeqs() const {
    std::vector<uint32_t> s;
    for (const Applied& a : applied_) s.push_back(a.seq);
    return s;
  }

  // Patch one generated chunk's kChunkVol words (chunk-linear, x fastest) with
  // the resolved layer. Returns the cells written. The voxel server's path.
  uint32_t ApplyToChunk(IVec3 wc, uint32_t* words) const;
  bool HasChunk(IVec3 wc) const { return byChunk_.count(Key(wc)) != 0; }

  // Hands the resolved layer to `edits` as its BASE patch (FarEdits::SetBase),
  // replacing any earlier base. An empty layer clears the base.
  void SeedFarField(::FarEdits& edits) const;

  // ---- writers --------------------------------------------------------------
  // v1: absolute, chunk-keyed. What the tuner's viewer reads and writes.
  static bool WriteAbsolute(const std::string& path,
                            const std::vector<AbsCell>& cells, uint32_t seed,
                            std::string& err);
  // v2: ground-relative, column-keyed, dy = y - TerrainHeight(x, z, seed)
  // under the map in force. What assets/worldedits/ holds from P7 on.
  static bool WriteRelative(const std::string& path,
                            const std::vector<AbsCell>& cells, uint32_t seed,
                            std::string& err);

  static constexpr uint32_t kMagic = 0x44455653u;   // 'SVED'
  static constexpr uint32_t kFlagRelative = 1u;
  static constexpr size_t kAppliedRing = 16;

 private:
  static uint64_t Key(IVec3 wc) { return World::PackChunkKey(wc); }

  struct Cell {
    uint32_t localIdx;  // 0..kChunkVol-1, chunk-linear, x fastest
    uint32_t word;
  };
  struct ColCell {
    int32_t y;          // dy when relative_, absolute y otherwise
    uint32_t word;
  };
  // The FILE, as read: per column, ordered (x, z) so resolution order is a
  // function of content.
  std::map<std::pair<int32_t, int32_t>, std::vector<ColCell>> cols_;
  bool relative_ = false;
  uint32_t fileSeed_ = 0;

  // The RESOLVED layer.
  bool resolved_ = false;
  uint32_t resolvedSeed_ = 0;
  uint32_t resolvedMap_ = 0;
  std::unordered_map<uint64_t, std::vector<Cell>> byChunk_;
  std::unordered_map<uint64_t, IVec3> coords_;

  std::vector<IVec3> pending_;
  size_t cursor_ = 0;       // next pending chunk
  size_t within_ = 0;       // next cell inside pending_[cursor_]
  size_t voxelCount_ = 0;
  std::string name_;

  struct Applied {
    uint32_t seq = 0;
    std::vector<uint32_t> slots;
  };
  std::vector<Applied> applied_;   // ring, kAppliedRing entries
  size_t appliedNext_ = 0;
};

// The process-wide layer. One, because the layer is a property of the WORLD and
// every producer of chunks (startup worldgen, the streamer, a regen) has to
// consult the same one — three owners would mean three answers to "has this
// chunk been patched".
WorldEdits& WorldEditLayer();

// Loads the layer the current map names (worldmap::CurrentWorldMap().editLayer)
// from the asset directory, or clears it if the map names none, and reports
// what happened. Call after the map is loaded; safe to call again after an
// environment reload. Cheap when nothing changed (the file's size and mtime
// are compared first), which is what lets the voxel server call it per
// request.
void LoadWorldEditLayerForMap(const std::string& assetDir);
// The same, for an explicit layer name ("" clears). The voxel server's LAYER
// command: the tuner may be editing a layer the map does not name yet.
void LoadWorldEditLayerNamed(const std::string& assetDir, const std::string& name);

}  // namespace sandvox
