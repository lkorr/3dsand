#include "sim/farplumes.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

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

// ---- the LONG-RANGE box (world.h kGasFarOuterN) ---------------------------
// Origin FLOORED to the cell, which gasOuter's is not and does not need to be:
// the window origin is a multiple of 16 voxels and this cell is 64, so without
// the floor the whole lattice would slide sideways on every window shift. The
// same expression lives in sim_gas.wgsl's gasFarOuterOrigin and in
// raymarch.wgsl's gasFarOuterOriginVox — three copies of an identity world.h
// static_asserts, exactly as the fine box already has.
inline int FloorToCell(int v, uint32_t shift) { return (v >> shift) << shift; }

inline IVec3 WideOriginVox(IVec3 windowOriginChunks) {
  const int o = kGasFarOuterOffsetVox;
  return {FloorToCell(windowOriginChunks.x * (int)kChunk - o, kGasFarOuterShift),
          FloorToCell(windowOriginChunks.y * (int)kChunk - o, kGasFarOuterShift),
          FloorToCell(windowOriginChunks.z * (int)kChunk - o, kGasFarOuterShift)};
}

inline bool InWideBox(IVec3 v, IVec3 originVox) {
  const int n = (int)(kGasFarOuterN << kGasFarOuterShift);
  const int dx = v.x - originVox.x, dy = v.y - originVox.y,
            dz = v.z - originVox.z;
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

namespace {
// One candidate emitter with its rank. Distance is squared euclidean from the
// window centre; the BAND it falls in is decided by the max norm, which is a
// different metric on purpose (see the header).
struct Ranked {
  int64_t d2;
  FarPlumes::Emitter e;
};
bool RankLess(const Ranked& a, const Ranked& b) {
  if (a.d2 != b.d2) return a.d2 < b.d2;
  if (a.e.x != b.e.x) return a.e.x < b.e.x;
  if (a.e.y != b.e.y) return a.e.y < b.e.y;
  return a.e.z < b.e.z;
}
// Cap to `max` nearest, then SORT — and the sort is not cosmetic. The emitter
// INDEX is one of the inputs to the shader's animation hash, so an unordered
// rebuild would re-roll every plume's turbulence whenever an unrelated chunk
// was evicted. Sorting by distance means a rebuild that did not change which
// fires are near does not change their indices either.
void CapAndSort(std::vector<Ranked>& v, size_t max) {
  if (v.size() > max) {
    std::nth_element(v.begin(), v.begin() + max, v.end(), RankLess);
    v.resize(max);
  }
  std::sort(v.begin(), v.end(), RankLess);
}
void WriteSection(std::vector<uint32_t>& words, size_t base,
                  const std::vector<Ranked>& v) {
  for (size_t i = 0; i < v.size(); i++) {
    uint32_t* w = words.data() + base + i * kGasFarEmitStride;
    w[0] = (uint32_t)v[i].e.x;
    w[1] = (uint32_t)v[i].e.y;
    w[2] = (uint32_t)v[i].e.z;
    w[3] = v[i].e.strength;
  }
}
}  // namespace

void FarPlumes::Build(IVec3 windowOriginChunks, int32_t rangeVox) {
  if (!dirty_ && windowOriginChunks.x == builtOrigin_.x &&
      windowOriginChunks.y == builtOrigin_.y &&
      windowOriginChunks.z == builtOrigin_.z && rangeVox == builtRange_)
    return;
  dirty_ = false;
  builtOrigin_ = windowOriginChunks;
  builtRange_ = rangeVox;

  const IVec3 ov = BoxOriginVox(windowOriginChunks);
  const IVec3 wov = WideOriginVox(windowOriginChunks);
  // The window CENTRE in voxels — the same point the voxel/parcel crossfade
  // measures from, and the right origin here for the same reason: what the
  // player can see is bounded by the window, not by where the camera looks.
  const int cx = windowOriginChunks.x * (int)kChunk + (int)kWorldN / 2;
  const int cy = windowOriginChunks.y * (int)kChunk + (int)kWorldN / 2;
  const int cz = windowOriginChunks.z * (int)kChunk + (int)kWorldN / 2;
  // The handover distance, in the MAX NORM: the fine box's own half-extent.
  // Inside it a fire is drawn at 0.8 m cells, outside at 6.4 m. Strict, so no
  // emitter feeds both boxes and "no double-brightening" is a property of the
  // data rather than of a blend weight.
  const int fineHalf = (int)kWorldN;

  std::vector<Ranked> fine;
  fine.reserve(byChunk_.size() * 2);
  // The wide section is aggregated AGAIN, per COARSE column: a 6.4 m cell
  // holds up to 8x8 fine columns in x/z and four chunks in y, so without this
  // one burning hillside would spend the whole 256-emitter budget on a patch
  // 64 voxels across and everything else in the band would be silent.
  std::unordered_map<Key, Emitter, KeyHash> wide;

  for (const auto& kv : byChunk_) {
    const IVec3 wc{kv.first.x, kv.first.y, kv.first.z};
    // RESIDENT CHUNKS ARE DROPPED. A fire inside the window is a running fire:
    // the CA makes real smoke voxels for it, sim_step splats those into the
    // fine box, and an emitter on top would draw the plume twice. This is also
    // what retires an emitter when the player walks back to a fire — no event
    // is needed, the test simply stops passing.
    if (ChunkInWindow(wc, windowOriginChunks)) continue;
    for (const Emitter& e : kv.second) {
      const int dx = e.x - cx, dy = e.y - cy, dz = e.z - cz;
      const int mx = std::max(std::abs(dx), std::max(std::abs(dy), std::abs(dz)));
      const int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz;
      if (mx < fineHalf) {
        if (InBox({e.x, e.y, e.z}, ov)) fine.push_back({d2, e});
        continue;
      }
      if (rangeVox <= 0 || mx > rangeVox) continue;
      if (!InWideBox({e.x, e.y, e.z}, wov)) continue;
      // Bucket by the COARSE cell the emitter stands in. Strengths add and the
      // topmost hot voxel wins, so a whole burning hillside becomes ONE
      // emitter whose strength says how much of it is alight — which is what
      // the shader turns into a taller, denser column.
      const Key k{(e.x - wov.x) >> kGasFarOuterShift,
                  (e.y - wov.y) >> kGasFarOuterShift,
                  (e.z - wov.z) >> kGasFarOuterShift};
      auto it = wide.find(k);
      if (it == wide.end()) {
        wide.emplace(k, e);
      } else {
        // Saturating, because the record's strength is a u32 and a hillside
        // could in principle overflow one.
        it->second.strength =
            (uint32_t)std::min<uint64_t>((uint64_t)it->second.strength + e.strength,
                                         0xFFFFFFFFull);
        if (e.y > it->second.y) {
          it->second.y = e.y;
          it->second.x = e.x;
          it->second.z = e.z;
        }
      }
    }
  }

  std::vector<Ranked> wideRanked;
  wideRanked.reserve(wide.size());
  for (const auto& kv : wide) {
    const int dx = kv.second.x - cx, dy = kv.second.y - cy,
              dz = kv.second.z - cz;
    wideRanked.push_back(
        {(int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz, kv.second});
  }

  CapAndSort(fine, kGasFarEmitMax);
  CapAndSort(wideRanked, kGasFarEmitMaxWide);

  count_ = (uint32_t)fine.size();
  countWide_ = (uint32_t)wideRanked.size();
  // The image is always full size: the wide section starts at a FIXED word so
  // the shader can address it without reading the fine count first, and a
  // short buffer would leave the previous list's tail on the GPU.
  words_.assign(kGasFarEmitWords, 0u);
  words_[0] = count_;
  words_[1] = kGasOuterShift;       // which box the fine section was built for
  words_[2] = countWide_;
  words_[3] = kGasFarOuterShift;    // ...and the wide one
  WriteSection(words_, kGasFarEmitHdr, fine);
  WriteSection(words_, kGasFarEmitWideBase, wideRanked);
  version_++;
}

bool FarPlumes::TakeUpload(const uint32_t** words, uint32_t* wordCount) {
  if (version_ == uploadedVersion_) return false;
  uploadedVersion_ = version_;
  if (words) *words = words_.data();
  if (wordCount) *wordCount = (uint32_t)words_.size();
  return true;
}
