#include "sim/worldedit.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <tuple>

#include "sim/faredits.h"
#include "sim/worldmap.h"

namespace sandvox {
namespace {

uint32_t Rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
void Push32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back((uint8_t)v);
  b.push_back((uint8_t)(v >> 8));
  b.push_back((uint8_t)(v >> 16));
  b.push_back((uint8_t)(v >> 24));
}
void Put32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
  b[o] = (uint8_t)v;
  b[o + 1] = (uint8_t)(v >> 8);
  b[o + 2] = (uint8_t)(v >> 16);
  b[o + 3] = (uint8_t)(v >> 24);
}

bool WriteBytes(const std::string& path, const std::vector<uint8_t>& b,
                std::string& err) {
  // Write-then-rename, like every other authored file the tuner saves: a crash
  // mid-write must leave the previous layer, not half of a new one.
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { err = "cannot write " + tmp; return false; }
    f.write((const char*)b.data(), (std::streamsize)b.size());
    if (!f) { err = "short write to " + tmp; return false; }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, ec);
  }
  if (ec) { err = "cannot move " + tmp + " over " + path; return false; }
  return true;
}

// A stored word never carries the transient CPU->GPU flag nor the per-tick
// scratch (stamp, excite); a file is not trusted to have got that right.
inline uint32_t CleanWord(uint32_t w) { return w & ~(kCellOpIfAir | 0x00FF0000u); }

}  // namespace

// ---- the 'SVED' binary ----------------------------------------------------
//
// Twin of EditLayer.serialize in assets/worldview.js (v1 only: the viewer
// works in absolute cells and tuner_server.py converts through --voxserve's
// EDITS command, which calls the writers below).
//
//   0  'SVED'          12 u32 voxelCount    24 u32 reserved[2]
//   4  u32 version     16 u32 seed
//   8  u32 count       20 u32 flags (v2: bit 0 = ground-relative)
//
//   v1, count = chunks:  per chunk  i32 cx, cy, cz, u32 n, n * {u32 localIdx, u32 word}
//   v2, count = columns: per column i32 x, z, u32 n, n * {i32 dy, u32 word}
//
// The seed is RECORDED, not enforced. For v2 it is the seed the offsets were
// taken against, kept so a diagnostic can say where a layer was built; the
// offsets themselves mean the same thing over any seed.
bool WorldEdits::Load(const std::string& path, std::string& err) {
  Clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    err = "cannot open " + path;
    return false;
  }
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
  return LoadBytes(b, path, err);
}

bool WorldEdits::LoadBytes(const std::vector<uint8_t>& b, const std::string& path,
                           std::string& err) {
  Clear();
  if (b.size() < 32 || Rd32(b.data()) != kMagic) {
    err = path + ": not an SVED edit layer";
    return false;
  }
  const uint32_t ver = Rd32(b.data() + 4);
  if (ver != 1 && ver != 2) {
    err = path + ": SVED version " + std::to_string(ver) + ", expected 1 or 2";
    return false;
  }
  const uint32_t count = Rd32(b.data() + 8);
  fileSeed_ = Rd32(b.data() + 16);
  const uint32_t flags = Rd32(b.data() + 20);
  relative_ = ver == 2 && (flags & kFlagRelative) != 0;
  size_t p = 32;
  auto fail = [&](const std::string& why) {
    err = path + ": " + why;
    Clear();
    return false;
  };
  for (uint32_t i = 0; i < count; i++) {
    if (ver == 1) {
      if (p + 16 > b.size()) return fail("truncated chunk header");
      const IVec3 wc{(int32_t)Rd32(b.data() + p), (int32_t)Rd32(b.data() + p + 4),
                     (int32_t)Rd32(b.data() + p + 8)};
      const uint32_t n = Rd32(b.data() + p + 12);
      p += 16;
      if (p + (size_t)n * 8 > b.size()) return fail("truncated cell list");
      for (uint32_t j = 0; j < n; j++) {
        const uint32_t li = Rd32(b.data() + p);
        const uint32_t w = Rd32(b.data() + p + 4);
        p += 8;
        // A local index past the chunk would address another chunk's memory
        // through SlotCellIndex. Refuse the file rather than clamp: a layer
        // that is wrong here was not written by anything that understood it.
        if (li >= kChunkVol) return fail("cell index out of range");
        const int x = wc.x * (int)kChunk + (int)(li % kChunk);
        const int y = wc.y * (int)kChunk + (int)((li / kChunk) % kChunk);
        const int z = wc.z * (int)kChunk + (int)(li / (kChunk * kChunk));
        cols_[{x, z}].push_back({y, CleanWord(w)});
        voxelCount_++;
      }
    } else {
      if (p + 12 > b.size()) return fail("truncated column header");
      const int32_t x = (int32_t)Rd32(b.data() + p);
      const int32_t z = (int32_t)Rd32(b.data() + p + 4);
      const uint32_t n = Rd32(b.data() + p + 8);
      p += 12;
      if (p + (size_t)n * 8 > b.size()) return fail("truncated cell list");
      std::vector<ColCell>& col = cols_[{x, z}];
      for (uint32_t j = 0; j < n; j++) {
        const int32_t dy = (int32_t)Rd32(b.data() + p);
        const uint32_t w = Rd32(b.data() + p + 4);
        p += 8;
        // A sanity bound, not a design one: an exported save may hold a
        // tunnel kilometres under a mountain (measured: -4146), but not a
        // cell a million voxels from the ground — that is a corrupt offset.
        if (dy < -(1 << 20) || dy > (1 << 20)) return fail("column offset out of range");
        col.push_back({dy, CleanWord(w)});
        voxelCount_++;
      }
    }
  }
  name_ = path;
  return true;
}

void WorldEdits::Clear() {
  cols_.clear();
  relative_ = false;
  fileSeed_ = 0;
  resolved_ = false;
  byChunk_.clear();
  coords_.clear();
  pending_.clear();
  cursor_ = within_ = 0;
  voxelCount_ = 0;
  name_.clear();
  applied_.clear();
  appliedNext_ = 0;
}

void WorldEdits::Resolve(uint32_t seed) {
  const uint32_t mapHash = worldmap::CurrentWorldMap().contentHash;
  // An ABSOLUTE layer does not depend on either, so it resolves once.
  if (resolved_ && (!relative_ || (seed == resolvedSeed_ && mapHash == resolvedMap_)))
    return;
  byChunk_.clear();
  coords_.clear();
  pending_.clear();
  cursor_ = within_ = 0;
  for (const auto& kv : cols_) {
    const int x = kv.first.first, z = kv.first.second;
    // TerrainHeight is the ground CONTRACT (== genColumn(x, z, seed).h, and
    // since P5 it includes the sculpt layer), so an offset means "this many
    // cells above the ground the world was generated with" on both sides.
    const int base = relative_ ? World::TerrainHeight(x, z, seed) : 0;
    for (const ColCell& c : kv.second) {
      const int y = base + c.y;
      const IVec3 wc{x >> 4, y >> 4, z >> 4};
      const uint32_t li = (uint32_t)(x & 15) + (uint32_t)(y & 15) * kChunk +
                          (uint32_t)(z & 15) * kChunk * kChunk;
      byChunk_[Key(wc)].push_back({li, c.word});
      coords_[Key(wc)] = wc;
    }
  }
  // Ascending local index per chunk, so the op stream is a pure function of the
  // file's CONTENT rather than of its authoring order — two layers with the
  // same edits written in a different order produce the same world. A cell
  // named twice keeps its LAST word (stable sort, then keep the last of a run).
  for (auto& kv : byChunk_) {
    std::vector<Cell>& v = kv.second;
    std::stable_sort(v.begin(), v.end(),
                     [](const Cell& a, const Cell& b) { return a.localIdx < b.localIdx; });
    size_t w = 0;
    for (size_t r = 0; r < v.size(); r++) {
      if (r + 1 < v.size() && v[r].localIdx == v[r + 1].localIdx) continue;
      v[w++] = v[r];
    }
    v.resize(w);
  }
  resolved_ = true;
  resolvedSeed_ = seed;
  resolvedMap_ = mapHash;
}

void WorldEdits::ResolvedCells(std::vector<AbsCell>& out) const {
  out.clear();
  // In chunk-key order so the output is a function of content, not of the
  // hash map's iteration order.
  std::vector<uint64_t> keys;
  keys.reserve(byChunk_.size());
  for (const auto& kv : byChunk_) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  for (uint64_t k : keys) {
    const IVec3 wc = coords_.at(k);
    for (const Cell& c : byChunk_.at(k))
      out.push_back({wc.x * (int)kChunk + (int)(c.localIdx % kChunk),
                     wc.y * (int)kChunk + (int)((c.localIdx / kChunk) % kChunk),
                     wc.z * (int)kChunk + (int)(c.localIdx / (kChunk * kChunk)),
                     c.word});
  }
}

void WorldEdits::QueueChunk(IVec3 wc, uint32_t seed) {
  if (cols_.empty()) return;
  Resolve(seed);
  if (byChunk_.find(Key(wc)) == byChunk_.end()) return;
  pending_.push_back(wc);
}

void WorldEdits::QueueWindow(const World& world) {
  // Every whole-window (re)generation also (re)seeds the far field's base, so
  // the horizon agrees with the layer the window is about to receive — and
  // loses a layer the map no longer names.
  if (cols_.empty()) {
    if (world.farEdits && world.farEdits->BaseLevelChunks()) world.farEdits->SetBase({});
    return;
  }
  Resolve(world.WorldSeed());
  if (world.farEdits) SeedFarField(*world.farEdits);
  // Walk the LAYER, not the window: a layer is a few chunks and the window is
  // 32,768, so asking each edited chunk "are you resident" is three orders of
  // magnitude less work than asking each resident chunk "are you edited".
  // Sorted by key so the queue order is a function of content.
  std::vector<uint64_t> keys;
  for (const auto& kv : coords_)
    if (world.ChunkInWindow(kv.second)) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  for (uint64_t k : keys) pending_.push_back(coords_.at(k));
}

uint32_t WorldEdits::Drain(const World& world, std::vector<CellOp>& out,
                           uint32_t max, uint32_t submitSeq) {
  uint32_t n = 0;
  std::vector<uint32_t>* touched = nullptr;
  if (submitSeq != 0 && cursor_ < pending_.size()) {
    // One ring entry per submit; a second Drain for the same submit extends it.
    for (Applied& a : applied_)
      if (a.seq == submitSeq) touched = &a.slots;
    if (!touched) {
      if (applied_.size() < kAppliedRing) applied_.emplace_back();
      Applied& a = applied_[appliedNext_];
      appliedNext_ = (appliedNext_ + 1) % kAppliedRing;
      a.seq = submitSeq;
      a.slots.clear();
      touched = &a.slots;
    }
  }
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
    if (touched && within_ < list.size()) {
      // The chunk and its 26 neighbours: sim_mutate's dirtyFanSlot marks the
      // neighbours of a face cell, and a neighbour that reports MUTATE-only
      // was woken by this layer and nothing else.
      for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            const IVec3 nc{wc.x + dx, wc.y + dy, wc.z + dz};
            if (world.ChunkInWindow(nc)) touched->push_back(World::SlotChunkIndex(nc));
          }
    }
    while (within_ < list.size() && n < max) {
      const Cell& c = list[within_++];
      out.push_back(CellOp{base + c.localIdx, c.word});
      n++;
    }
    if (within_ >= list.size()) { cursor_++; within_ = 0; }
  }
  if (touched) {
    std::sort(touched->begin(), touched->end());
    touched->erase(std::unique(touched->begin(), touched->end()), touched->end());
  }
  // Compact once the backlog has drained, so a long session does not keep every
  // chunk it ever queued.
  if (cursor_ >= pending_.size()) { pending_.clear(); cursor_ = within_ = 0; }
  return n;
}

const std::vector<uint32_t>* WorldEdits::AppliedSlotsAt(uint32_t submitSeq) const {
  if (submitSeq == 0) return nullptr;
  for (const Applied& a : applied_)
    if (a.seq == submitSeq && !a.slots.empty()) return &a.slots;
  return nullptr;
}

uint32_t WorldEdits::ApplyToChunk(IVec3 wc, uint32_t* words) const {
  auto it = byChunk_.find(Key(wc));
  if (it == byChunk_.end()) return 0;
  for (const Cell& c : it->second) words[c.localIdx] = c.word;
  return (uint32_t)it->second.size();
}

void WorldEdits::SeedFarField(::FarEdits& edits) const {
  std::vector<FarEdits::BaseCell> cells;
  cells.reserve(voxelCount_);
  for (const auto& kv : byChunk_) {
    const IVec3 wc = coords_.at(kv.first);
    for (const Cell& c : kv.second)
      cells.push_back({wc.x * (int)kChunk + (int)(c.localIdx % kChunk),
                       wc.y * (int)kChunk + (int)((c.localIdx / kChunk) % kChunk),
                       wc.z * (int)kChunk + (int)(c.localIdx / (kChunk * kChunk)),
                       c.word & 0xFFFu});
  }
  edits.SetBase(cells);
}

bool WorldEdits::WriteAbsolute(const std::string& path,
                               const std::vector<AbsCell>& cells, uint32_t seed,
                               std::string& err) {
  std::map<std::tuple<int, int, int>, std::vector<std::pair<uint32_t, uint32_t>>> byChunk;
  for (const AbsCell& c : cells) {
    const uint32_t li = (uint32_t)(c.x & 15) + (uint32_t)(c.y & 15) * kChunk +
                        (uint32_t)(c.z & 15) * kChunk * kChunk;
    byChunk[{c.x >> 4, c.y >> 4, c.z >> 4}].push_back({li, CleanWord(c.word)});
  }
  std::vector<uint8_t> b(32, 0);
  Put32(b, 0, kMagic);
  Put32(b, 4, 1);
  Put32(b, 8, (uint32_t)byChunk.size());
  Put32(b, 12, (uint32_t)cells.size());
  Put32(b, 16, seed);
  for (const auto& kv : byChunk) {
    Push32(b, (uint32_t)std::get<0>(kv.first));
    Push32(b, (uint32_t)std::get<1>(kv.first));
    Push32(b, (uint32_t)std::get<2>(kv.first));
    Push32(b, (uint32_t)kv.second.size());
    for (const auto& c : kv.second) { Push32(b, c.first); Push32(b, c.second); }
  }
  return WriteBytes(path, b, err);
}

bool WorldEdits::WriteRelative(const std::string& path,
                               const std::vector<AbsCell>& cells, uint32_t seed,
                               std::string& err) {
  std::map<std::pair<int, int>, std::vector<std::pair<int, uint32_t>>> byCol;
  for (const AbsCell& c : cells) byCol[{c.x, c.z}].push_back({c.y, CleanWord(c.word)});
  std::vector<uint8_t> b(32, 0);
  Put32(b, 0, kMagic);
  Put32(b, 4, 2);
  Put32(b, 8, (uint32_t)byCol.size());
  Put32(b, 12, (uint32_t)cells.size());
  Put32(b, 16, seed);
  Put32(b, 20, kFlagRelative);
  for (auto& kv : byCol) {
    const int h = World::TerrainHeight(kv.first.first, kv.first.second, seed);
    std::sort(kv.second.begin(), kv.second.end());
    Push32(b, (uint32_t)kv.first.first);
    Push32(b, (uint32_t)kv.first.second);
    Push32(b, (uint32_t)kv.second.size());
    for (const auto& c : kv.second) {
      Push32(b, (uint32_t)(c.first - h));
      Push32(b, c.second);
    }
  }
  return WriteBytes(path, b, err);
}

WorldEdits& WorldEditLayer() {
  static WorldEdits g;
  return g;
}

void LoadWorldEditLayerNamed(const std::string& assetDir, const std::string& name) {
  // What was last loaded, so a caller that asks per request (the voxel
  // server) pays a stat, not a parse.
  static std::string lastPath;
  static std::filesystem::file_time_type lastTime{};
  static uintmax_t lastSize = 0;

  if (name.empty()) {
    if (!WorldEditLayer().Empty()) {
      std::printf("world edits: layer cleared\n");
      WorldEditLayer().Clear();
    }
    lastPath.clear();
    return;
  }
  // A NAME, never a path: anything with a separator is refused.
  if (name.find_first_of("/\\:.") != std::string::npos) {
    std::fprintf(stderr, "world edits: '%s' is not a bare layer name\n", name.c_str());
    WorldEditLayer().Clear();
    lastPath.clear();
    return;
  }
  const std::string path = assetDir + "/worldedits/" + name + ".svedit";
  std::error_code ec;
  const auto t = std::filesystem::last_write_time(path, ec);
  const uintmax_t sz = ec ? 0 : std::filesystem::file_size(path, ec);
  if (!ec && path == lastPath && t == lastTime && sz == lastSize) return;
  std::string err;
  if (!WorldEditLayer().Load(path, err)) {
    std::fprintf(stderr, "world edits: %s\n", err.c_str());
    lastPath.clear();
    return;
  }
  lastPath = path;
  lastTime = t;
  lastSize = sz;
  std::printf("world edits: layer '%s' (SVED %s) - %zu voxels in %zu columns\n",
              name.c_str(), WorldEditLayer().Relative() ? "v2 ground-relative" : "v1 absolute",
              WorldEditLayer().VoxelCount(), WorldEditLayer().ColumnCount());
}

void LoadWorldEditLayerForMap(const std::string& assetDir) {
  LoadWorldEditLayerNamed(assetDir, worldmap::CurrentWorldMap().editLayer);
}

}  // namespace sandvox
