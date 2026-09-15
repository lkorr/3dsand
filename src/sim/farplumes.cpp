#include "sim/farplumes.h"

#include <algorithm>
#include <cstdint>

namespace {

// Which material ids are frozen fire, by id. Rebuilt whole by SetMaterials, so
// there is no stale half-state after a reload that renamed or removed one.
std::vector<uint8_t> g_hot;

// The gasOuter box, restated from world.h's one expression:
//   originVox = windowOriginChunks * kChunk - kWorldN/2
//   cell      = (worldVoxel - originVox) >> kGasOuterShift
// The same two lines live in sim_gas.wgsl's gasOuterOrigin/gasOuterCell and in
// support.cpp's reader. They are three copies of an IDENTITY that world.h
// static_asserts (kGasOuterN << kGasOuterShift == 2 * kWorldN), not three
// independent rules — and check_invariants.py pins the constants they are
// built from.
inline IVec3 BoxOriginVox(IVec3 windowOriginChunks) {
  const int h = (int)(kWorldN / 2);
  return {windowOriginChunks.x * (int)kChunk - h,
          windowOriginChunks.y * (int)kChunk - h,
          windowOriginChunks.z * (int)kChunk - h};
}

inline bool InBox(IVec3 v, IVec3 originVox) {
  const int n = (int)(kGasOuterN << kGasOuterShift);   // 2 * kWorldN
  const int dx = v.x - originVox.x, dy = v.y - originVox.y,
            dz = v.z - originVox.z;
  return dx >= 0 && dy >= 0 && dz >= 0 && dx < n && dy < n && dz < n;
}

inline bool ChunkInWindow(IVec3 wc, IVec3 wo) {
  const int n = (int)kNChunk;
  const int dx = wc.x - wo.x, dy = wc.y - wo.y, dz = wc.z - wo.z;
  return dx >= 0 && dy >= 0 && dz >= 0 && dx < n && dy < n && dz < n;
}

}  // namespace

void FarPlumes::SetMaterials(const std::vector<MaterialDef>& mats) {
  g_hot.assign(mats.size(), 0u);
  for (size_t i = 1; i < mats.size(); i++) {   // 0 is air
    const MaterialDef& m = mats[i];
    // A GAS is excluded and the exclusion is load-bearing rather than tidy:
    // `fire` is CLASS_GAS, and farCellIsSolid (worldgen.wgsl) never writes a
    // gas into the cascade — so a frozen fire's actual flames are not out
    // there to see. What IS out there is the ember/lava/charred foliage under
    // them, which is exactly this set.
    if (m.gpu.klass == CLASS_GAS) continue;
    if (m.gpu.emission == 0) continue;
    for (const std::string& t : m.tags)
      if (t == "hot") { g_hot[i] = 1; break; }
  }
}

bool FarPlumes::HotEmissive(uint32_t mat) {
  return mat < g_hot.size() && g_hot[mat] != 0;
}

void FarPlumes::Clear() {
  if (!byChunk_.empty()) dirty_ = true;
  byChunk_.clear();
  refusedChunks_ = 0;
}

void FarPlumes::Replace(IVec3 wc, const Emitter* e, uint32_t n) {
  const Key k{wc.x, wc.y, wc.z};
  auto it = byChunk_.find(k);
  if (n == 0) {
    // The fire went out and the chunk was evicted again. Removing the entry is
    // the whole of "a plume stops" — there is no per-emitter lifetime, because
    // the index mirrors the world rather than modelling it.
    if (it != byChunk_.end()) { byChunk_.erase(it); dirty_ = true; }
    return;
  }
  if (it == byChunk_.end()) {
    if (byChunk_.size() >= kChunkCap) { refusedChunks_++; return; }
    byChunk_.emplace(k, std::vector<Emitter>(e, e + n));
    dirty_ = true;
    return;
  }
  it->second.assign(e, e + n);
  dirty_ = true;
}

void FarPlumes::NoteChunk(IVec3 wc, const uint32_t* words) {
  // 2x2 gasOuter column footprints per fine chunk (world.h's static_assert that
  // kChunk tiles the cell is what makes this exact rather than approximate).
  constexpr uint32_t kCell = 1u << kGasOuterShift;   // 8
  constexpr uint32_t kSub = kChunk / kCell;          // 2 per axis in x/z
  uint32_t count[kSub * kSub] = {};
  int topY[kSub * kSub];
  for (uint32_t i = 0; i < kSub * kSub; i++) topY[i] = -1;

  for (uint32_t z = 0; z < kChunk; z++)
    for (uint32_t y = 0; y < kChunk; y++)
      for (uint32_t x = 0; x < kChunk; x++) {
        const uint32_t mat = words[(z * kChunk + y) * kChunk + x] & 0xFFFu;
        if (!HotEmissive(mat)) continue;
        const uint32_t s = (z / kCell) * kSub + (x / kCell);
        count[s]++;
        topY[s] = (int)y;   // ascending y, so the last write is the topmost
      }

  Emitter e[kSub * kSub];
  uint32_t n = 0;
  for (uint32_t sz = 0; sz < kSub; sz++)
    for (uint32_t sx = 0; sx < kSub; sx++) {
      const uint32_t s = sz * kSub + sx;
      if (count[s] == 0) continue;
      e[n].x = wc.x * (int)kChunk + (int)(sx * kCell + kCell / 2);
      e[n].y = wc.y * (int)kChunk + topY[s];
      e[n].z = wc.z * (int)kChunk + (int)(sz * kCell + kCell / 2);
      e[n].strength = std::min(count[s], kGasFarEmitStrengthMax);
      n++;
    }
  Replace(wc, e, n);
}

void FarPlumes::NoteUniformChunk(IVec3 wc, uint32_t mat) {
  if (!HotEmissive(mat & 0xFFFu)) { Replace(wc, nullptr, 0); return; }
  // A whole chunk of ember: every column is saturated and the top hot voxel is
  // the chunk's top. Rare (a sentinel chunk of burning matter takes a
  // deliberate fill) but it is the other door into the index and leaving it
  // shut would make the feature depend on which door a fire came through.
  constexpr uint32_t kCell = 1u << kGasOuterShift;
  constexpr uint32_t kSub = kChunk / kCell;
  Emitter e[kSub * kSub];
  uint32_t n = 0;
  for (uint32_t sz = 0; sz < kSub; sz++)
    for (uint32_t sx = 0; sx < kSub; sx++) {
      e[n].x = wc.x * (int)kChunk + (int)(sx * kCell + kCell / 2);
      e[n].y = wc.y * (int)kChunk + (int)kChunk - 1;
      e[n].z = wc.z * (int)kChunk + (int)(sz * kCell + kCell / 2);
      e[n].strength = kGasFarEmitStrengthMax;
      n++;
    }
  Replace(wc, e, n);
}

void FarPlumes::Build(IVec3 windowOriginChunks) {
  if (!dirty_ && windowOriginChunks.x == builtOrigin_.x &&
      windowOriginChunks.y == builtOrigin_.y &&
      windowOriginChunks.z == builtOrigin_.z)
    return;
  dirty_ = false;
  builtOrigin_ = windowOriginChunks;

  const IVec3 ov = BoxOriginVox(windowOriginChunks);
  // The window CENTRE in voxels — the same point the crossfade band measures
  // from, and the right tie-breaker here for the same reason: what the player
  // can see is bounded by the window, not by where the camera happens to look.
  const int64_t cx = (int64_t)windowOriginChunks.x * (int)kChunk + kWorldN / 2;
  const int64_t cy = (int64_t)windowOriginChunks.y * (int)kChunk + kWorldN / 2;
  const int64_t cz = (int64_t)windowOriginChunks.z * (int)kChunk + kWorldN / 2;

  struct Ranked { int64_t d2; Emitter e; };
  std::vector<Ranked> keep;
  keep.reserve(byChunk_.size() * 2);
  for (const auto& kv : byChunk_) {
    const IVec3 wc{kv.first.x, kv.first.y, kv.first.z};
    // RESIDENT CHUNKS ARE DROPPED. A fire inside the window is a running fire:
    // the CA makes real smoke voxels for it, sim_step splats those into the
    // same box, and an emitter on top would draw the plume twice. This is also
    // what retires an emitter when the player walks back to a fire — no event
    // is needed, the test simply stops passing.
    if (ChunkInWindow(wc, windowOriginChunks)) continue;
    for (const Emitter& e : kv.second) {
      if (!InBox({e.x, e.y, e.z}, ov)) continue;
      const int64_t dx = e.x - cx, dy = e.y - cy, dz = e.z - cz;
      keep.push_back({dx * dx + dy * dy + dz * dz, e});
    }
  }
  if (keep.size() > kGasFarEmitMax) {
    std::nth_element(keep.begin(), keep.begin() + kGasFarEmitMax, keep.end(),
                     [](const Ranked& a, const Ranked& b) { return a.d2 < b.d2; });
    keep.resize(kGasFarEmitMax);
  }
  // SORTED, and not merely truncated. The list is a stable identity for the
  // shader's animation hash (the emitter INDEX is one of its inputs), so an
  // unordered rebuild would re-roll every plume's turbulence whenever an
  // unrelated chunk was evicted. Sorting by distance means a rebuild that did
  // not change which fires are near does not change their indices either.
  std::sort(keep.begin(), keep.end(),
            [](const Ranked& a, const Ranked& b) {
              if (a.d2 != b.d2) return a.d2 < b.d2;
              if (a.e.x != b.e.x) return a.e.x < b.e.x;
              if (a.e.y != b.e.y) return a.e.y < b.e.y;
              return a.e.z < b.e.z;
            });

  count_ = (uint32_t)keep.size();
  words_.assign(kGasFarEmitHdr + (size_t)count_ * kGasFarEmitStride, 0u);
  words_[0] = count_;
  words_[1] = kGasOuterShift;   // see world.h: which box this list was built for
  for (uint32_t i = 0; i < count_; i++) {
    uint32_t* w = words_.data() + kGasFarEmitHdr + (size_t)i * kGasFarEmitStride;
    w[0] = (uint32_t)keep[i].e.x;
    w[1] = (uint32_t)keep[i].e.y;
    w[2] = (uint32_t)keep[i].e.z;
    w[3] = keep[i].e.strength;
  }
  version_++;
}

bool FarPlumes::TakeUpload(const uint32_t** words, uint32_t* wordCount) {
  if (version_ == uploadedVersion_) return false;
  uploadedVersion_ = version_;
  if (words) *words = words_.data();
  if (wordCount) *wordCount = (uint32_t)words_.size();
  return true;
}
