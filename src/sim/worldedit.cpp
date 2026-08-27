#include "sim/worldedit.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "sim/tuning.h"

namespace sandvox {
namespace {

uint32_t Rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

}  // namespace

// ---- the 'SVED' binary ----------------------------------------------------
//
// Twin of EditLayer.serialize in assets/worldview.js; keep the two together.
//
//   0  'SVED'          12 u32 voxelCount    24 u32 reserved[2]
//   4  u32 version     16 u32 seed
//   8  u32 chunkCount  20 u32 flags
//   32 per chunk: i32 cx, cy, cz, u32 n, then n * { u32 localIdx, u32 word }
//
// The seed is RECORDED, not enforced. A layer authored against 1337 is still
// meaningful over 42 — the voxels land where they were placed and the terrain
// under them is different, which is the whole reason this is a layer and not a
// save. It is here so a diagnostic can say "this was built somewhere else".
bool WorldEdits::Load(const std::string& path, std::string& err) {
  Clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    err = "cannot open " + path;
    return false;
  }
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
  if (b.size() < 32 || Rd32(b.data()) != 0x44455653u) {
    err = path + ": not an SVED edit layer";
    return false;
  }
  const uint32_t ver = Rd32(b.data() + 4);
  if (ver != 1) {
    err = path + ": SVED version " + std::to_string(ver) + ", expected 1";
    return false;
  }
  const uint32_t chunks = Rd32(b.data() + 8);
  size_t p = 32;
  for (uint32_t i = 0; i < chunks; i++) {
    if (p + 16 > b.size()) { err = path + ": truncated chunk header"; Clear(); return false; }
    IVec3 wc{(int32_t)Rd32(b.data() + p), (int32_t)Rd32(b.data() + p + 4),
             (int32_t)Rd32(b.data() + p + 8)};
    const uint32_t n = Rd32(b.data() + p + 12);
    p += 16;
    if (p + (size_t)n * 8 > b.size()) { err = path + ": truncated cell list"; Clear(); return false; }
    std::vector<Cell>& list = byChunk_[Key(wc)];
    coords_[Key(wc)] = wc;
    list.reserve(list.size() + n);
    for (uint32_t j = 0; j < n; j++) {
      const uint32_t li = Rd32(b.data() + p);
      const uint32_t w = Rd32(b.data() + p + 4);
      p += 8;
      // A local index past the chunk would address another chunk's memory
      // through SlotCellIndex. Refuse the file rather than clamp: a layer that
      // is wrong here was not written by anything that understood the format.
      if (li >= kChunkVol) { err = path + ": cell index out of range"; Clear(); return false; }
      // Bit 31 is kCellOpIfAir, a transient CPU->GPU flag. A stored word must
      // never carry it and a file is not trusted to have got that right.
      list.push_back({li, w & ~kCellOpIfAir});
      voxelCount_++;
    }
  }
  // Ascending local index per chunk, so the op stream is a pure function of the
  // file's CONTENT rather than of its authoring order — two layers with the
  // same edits written in a different order produce the same world.
  for (auto& kv : byChunk_)
    std::sort(kv.second.begin(), kv.second.end(),
              [](const Cell& a, const Cell& b) { return a.localIdx < b.localIdx; });
  name_ = path;
  return true;
}

void WorldEdits::Clear() {
  byChunk_.clear();
  coords_.clear();
  pending_.clear();
  cursor_ = within_ = 0;
  voxelCount_ = 0;
  name_.clear();
}

void WorldEdits::QueueChunk(IVec3 wc) {
  if (byChunk_.empty()) return;
  if (byChunk_.find(Key(wc)) == byChunk_.end()) return;
  pending_.push_back(wc);
}

void WorldEdits::QueueWindow(const World& world) {
  if (byChunk_.empty()) return;
  // Walk the LAYER, not the window: a layer is a few chunks and the window is
  // 32,768, so asking each edited chunk "are you resident" is three orders of
  // magnitude less work than asking each resident chunk "are you edited".
  for (const auto& kv : coords_)
    if (world.ChunkInWindow(kv.second)) pending_.push_back(kv.second);
}

uint32_t WorldEdits::Drain(const World& world, std::vector<CellOp>& out,
                           uint32_t max) {
  uint32_t n = 0;
  while (cursor_ < pending_.size() && n < max) {
    const IVec3 wc = pending_[cursor_];
    auto it = byChunk_.find(Key(wc));
    if (it == byChunk_.end() || !world.ChunkInWindow(wc)) {
      // Scrolled out (or vanished under a reload) between queue and drain. A
      // cell index is WINDOW RELATIVE, so applying it now would patch whichever
      // world chunk currently owns that slot — a hole punched a kilometre away.
      cursor_++;
      within_ = 0;
      continue;
    }
    const std::vector<Cell>& list = it->second;
    const uint32_t base = World::SlotChunkIndex(wc) * kChunkVol;
    while (within_ < list.size() && n < max) {
      const Cell& c = list[within_++];
      out.push_back(CellOp{base + c.localIdx, c.word});
      n++;
    }
    if (within_ >= list.size()) { cursor_++; within_ = 0; }
  }
  // Compact once the backlog has drained, so a long session does not keep every
  // chunk it ever queued.
  if (cursor_ >= pending_.size()) { pending_.clear(); cursor_ = within_ = 0; }
  return n;
}

WorldEdits& WorldEditLayer() {
  static WorldEdits g;
  return g;
}

void LoadWorldEditLayerFromTuning(const std::string& assetDir) {
  const std::string& name = CurrentTuning().worldgen.editLayer;
  if (name.empty()) {
    if (!WorldEditLayer().Empty()) {
      std::printf("world edits: layer cleared\n");
      WorldEditLayer().Clear();
    }
    return;
  }
  const std::string path = assetDir + "/worldedits/" + name + ".svedit";
  std::string err;
  if (!WorldEditLayer().Load(path, err)) {
    std::fprintf(stderr, "world edits: %s\n", err.c_str());
    return;
  }
  std::printf("world edits: layer '%s' — %zu voxels in %zu chunks\n",
              name.c_str(), WorldEditLayer().VoxelCount(),
              WorldEditLayer().ChunkCount());
}

}  // namespace sandvox
