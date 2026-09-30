#include "sim/farplumes.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Which material ids make smoke, by id (materials.h SmokeSourceTable). Rebuilt
// whole by SetMaterials, so there is no stale half-state after a reload that
// renamed or removed one.
std::vector<uint8_t> g_hot;

// SANDVOX_PLUME_DEBUG=1: one stderr line per change to what the index holds
// for a chunk (which harvest replaced it, and with how many emitters) and per
// change to which LIST a chunk's emitters land in on a rebuild. The report it
// exists for: "two distant burning trees swap their smoke as I step". That is
// either the index (a harvest zeroing a chunk, or a chunk crossing a band) or
// it is the renderer, and this prints which — CLAUDE.md rule 6, attribution
// before elimination.
bool PlumeDebug() {
  static const bool on = std::getenv("SANDVOX_PLUME_DEBUG") != nullptr;
  return on;
}
const char* const kBandName[] = {"none", "IN-WINDOW", "fine", "wide",
                                 "out-of-range", "outside-box", "fine+wide"};

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

// The cell is ANISOTROPIC (world.h kGasFarOuterShiftY): 64 voxels in x/z, 8
// in y, so every per-axis expression here takes the y shift on its y line.
inline IVec3 WideOriginVox(IVec3 windowOriginChunks) {
  const int o = kGasFarOuterOffsetVox;
  const int oy = kGasFarOuterOffsetVoxY;
  return {FloorToCell(windowOriginChunks.x * (int)kChunk - o, kGasFarOuterShift),
          FloorToCell(windowOriginChunks.y * (int)kChunk - oy, kGasFarOuterShiftY),
          FloorToCell(windowOriginChunks.z * (int)kChunk - o, kGasFarOuterShift)};
}

inline bool InWideBox(IVec3 v, IVec3 originVox) {
  const int n = (int)(kGasFarOuterN << kGasFarOuterShift);
  const int ny = (int)(kGasFarOuterN << kGasFarOuterShiftY);
  const int dx = v.x - originVox.x, dy = v.y - originVox.y,
            dz = v.z - originVox.z;
  return dx >= 0 && dy >= 0 && dz >= 0 && dx < n && dy < ny && dz < n;
}

}  // namespace

void FarPlumes::SetMaterials(const std::vector<MaterialDef>& mats,
                             const std::vector<ReactionGpu>& reactions) {
  // NEAR AND FAR AGREE (rule-unification W1-B1). "On fire" at distance is
  // exactly "puts smoke into the sky up close": the material's own compiled
  // bucket decays to or emits fire/smoke (materials.h SmokeSourceTable). The
  // old rule — tagged `hot`, emissive, not a gas — was a second definition of
  // the same fact and disagreed with the first: lava and molten glass are hot
  // and glow but have no smoke rule, so a lava lake plumed at distance and
  // never up close.
  //
  // `fire` itself is IN now (its own decay-to-smoke rule puts it there). It
  // used to be excluded because the far cascade never draws a gas; but this
  // index is harvested from the fine chunk's WORDS at eviction, not from the
  // cascade, and a chunk evicted mid-blaze does hold flames that are smoking.
  g_hot = SmokeSourceTable(mats, reactions);
}

bool FarPlumes::SmokeSource(uint32_t mat) {
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
  if (PlumeDebug()) {
    const size_t old = it == byChunk_.end() ? 0 : it->second.size();
    uint32_t str = 0;
    for (uint32_t i = 0; i < n; i++) str += e[i].strength;
    if (old != n || n != 0)
      std::fprintf(stderr, "plume: note chunk (%d,%d,%d) emitters %zu -> %u "
                   "(hot voxels %u)\n", wc.x, wc.y, wc.z, old, n, str);
  }
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
        if (!SmokeSource(mat)) continue;
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
  if (!SmokeSource(mat & 0xFFFu)) { Replace(wc, nullptr, 0); return; }
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
// One candidate emitter with its rank (FarPlumes::Ranked, in the header only
// because Build keeps its scratch vectors as members).
using Ranked = FarPlumes::Ranked;
// A smoothstep in the 0..255 the record's top byte carries. Used for BOTH
// crossfade shells, because a linear weight leaves a slope discontinuity at
// each end of a shell and a slope discontinuity in a plume's brightness is
// exactly the ring the shell exists to remove (world.h kGasFarBlendVox).
// `double` is fine here and everywhere else in this file: the far-plume index
// is render-only derived data, never hashed and never saved.
inline uint32_t Smooth255(int32_t num, int32_t den) {
  if (den <= 0) return 255;
  const double x = std::min(std::max((double)num / (double)den, 0.0), 1.0);
  return (uint32_t)(255.0 * x * x * (3.0 - 2.0 * x) + 0.5);
}

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
    w[3] = (v[i].e.strength & kGasFarEmitStrengthMask) |
           (std::min(v[i].w8, 255u) << kGasFarEmitWeightShift);
  }
}
}  // namespace

void FarPlumes::Build(IVec3 windowOriginChunks, int32_t rangeVox) {
  // The eye earns a rebuild only once it has moved kGasFarEyeStepVox, and an
  // EMPTY index earns none at all: a world nobody has set alight must not pay
  // for the eye moving. Both guards matter now that the eye is an input — it
  // changes almost every tick, where the window origin changed almost never.
  const bool eyeMoved =
      hasEye_ && (std::abs(eye_.x - builtEye_.x) >= kGasFarEyeStepVox ||
                  std::abs(eye_.y - builtEye_.y) >= kGasFarEyeStepVox ||
                  std::abs(eye_.z - builtEye_.z) >= kGasFarEyeStepVox);
  // ---- WHAT AN EYE-ONLY MOVE MAY SKIP (2026-09-24) -------------------------
  // Everything but the crossfade WEIGHTS is a function of (index, window
  // origin, range): which chunks are resident, each emitter's distance from
  // the window centre and which box it is inside. The eye moves every tick a
  // player walks and earned a rebuild every kGasFarEyeStepVox, which re-walked
  // the whole hash map and redid all of that for weights alone. `geo_` caches
  // it; only a changed index, origin or range rebuilds it. Its order is the
  // map's iteration order at build time, and the map is unchanged until
  // dirty_ is raised, so the candidates arrive in exactly the order the
  // full walk produced them — the output image is byte-identical.
  const bool geoStale = dirty_ || windowOriginChunks.x != builtOrigin_.x ||
                        windowOriginChunks.y != builtOrigin_.y ||
                        windowOriginChunks.z != builtOrigin_.z ||
                        rangeVox != builtRange_;
  if (!geoStale && !(eyeMoved && !byChunk_.empty())) return;
  dirty_ = false;
  builtOrigin_ = windowOriginChunks;
  builtRange_ = rangeVox;
  builtEye_ = eye_;

  const IVec3 ov = BoxOriginVox(windowOriginChunks);
  const IVec3 wov = WideOriginVox(windowOriginChunks);
  // The window CENTRE in voxels — the same point the voxel/parcel crossfade
  // measures from, and the right origin here for the same reason: what the
  // player can see is bounded by the window, not by where the camera looks.
  const int cx = windowOriginChunks.x * (int)kChunk + (int)kWorldN / 2;
  const int cy = windowOriginChunks.y * (int)kChunk + (int)kWorldN / 2;
  const int cz = windowOriginChunks.z * (int)kChunk + (int)kWorldN / 2;
  // The handover distance, in the MAX NORM: the fine box's own half-extent.
  // Inside it a fire is drawn at 0.8 m cells, outside at 6.4 m. It is no longer
  // a strict split — an emitter within kGasFarBlendVox of it feeds BOTH lists
  // with complementary weights, and since that shell is now the whole band the
  // split is a crossfade everywhere rather than a bevelled edge at one radius.
  // "No double-brightening" is still a property of the data: the two weights
  // sum to 255 at every distance.
  const int fineHalf = (int)kWorldN;
  // Where the wide list itself ends, and therefore where its outer fade lands:
  // the smaller of the authored range and the box's own x/z half-extent (the
  // hard InWideBox clip below). Approximate in y, where the box is shorter —
  // deliberately, because this is a fade to nothing and a metre of it either
  // way is not observable.
  const int wideEnd =
      std::min(rangeVox, (int32_t)((kGasFarOuterN << kGasFarOuterShift) / 2));

  // ---- THE WEIGHT ORIGIN: the EYE, not the window centre -------------------
  // world.h kGasFarEyeSlackVox has the argument. In one line: list membership
  // is decided from the centre because the centre is exactly the half-extent
  // at every face, and the weight is decided from the eye because the centre
  // only moves when the window SHIFTS, so a weight keyed on it steps ~6% every
  // 1.6 m the player walks. Every ramp below ends kGasFarEyeSlackVox short of
  // its list boundary, which by the max-norm triangle inequality
  // (eyeDist >= centreDist - |eye - centre|) is what guarantees the weight has
  // already reached 0 by the time the record is dropped.
  const int ex = hasEye_ ? eye_.x : cx;
  const int ey = hasEye_ ? eye_.y : cy;
  const int ez = hasEye_ ? eye_.z : cz;
  // ...and with no eye set the slack is 0, because then the two origins ARE
  // the same point and the shells can run their full width. That is the case
  // every gate and headless harness is in.
  const int slack = hasEye_ ? kGasFarEyeSlackVox : 0;
  const int fineFadeIn = fineHalf - kGasFarBlendVox;   // the window face
  const int fineFadeOut = fineHalf - slack;
  const int wideFadeEnd = wideEnd - slack;
  const int wideFadeStart = wideFadeEnd - kGasFarRangeFadeVox;

  // ---- THE GEOMETRY: rebuilt only when the index, origin or range moved ---
  if (geoStale) {
    geo_.clear();
    geo_.reserve(byChunk_.size() * 2);
    resident_.clear();
    for (const auto& kv : byChunk_) {
      const IVec3 wc{kv.first.x, kv.first.y, kv.first.z};
      // RESIDENT CHUNKS ARE DROPPED. A fire inside the window is a running
      // fire: the CA makes real smoke voxels for it, sim_step splats those
      // into the fine box, and an emitter on top would draw the plume twice.
      // This is also what retires an emitter when the player walks back to a
      // fire -- no event is needed, the test simply stops passing.
      if (ChunkInWindow(wc, windowOriginChunks)) {
        if (PlumeDebug()) resident_.push_back(kv.first);
        continue;
      }
      for (const Emitter& e : kv.second) {
        const int dx = e.x - cx, dy = e.y - cy, dz = e.z - cz;
        const int mx =
            std::max(std::abs(dx), std::max(std::abs(dy), std::abs(dz)));
        Geo g;
        g.e = e;
        g.key = kv.first;
        g.d2 = (int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz;
        g.nearFine = mx < fineHalf;
        g.inFine = g.nearFine && InBox({e.x, e.y, e.z}, ov);
        g.inWide = rangeVox > 0 && mx <= rangeVox && InWideBox({e.x, e.y, e.z}, wov);
        g.outOfRange = rangeVox <= 0 || mx > rangeVox;
        geo_.push_back(g);
      }
    }
  }

  // ---- PASS 1: one candidate per emitter, no caps applied yet -------------
  // The caps used to be applied AFTER the wide aggregation had already run
  // inline in this loop, which made the fine cap a silent DELETE: the 257th
  // nearest column simply stopped existing. A burning tree is 40-100 columns,
  // so three or four trees exhausted the fine list and every other fire in the
  // world went out. Deciding membership first and aggregating second is what
  // lets an overflowing fine list fall back to the WIDE one instead.
  std::vector<Cand>& cand = cand_;
  cand.clear();
  if (PlumeDebug())
    for (const Key& k : resident_) dbgBand_[k] = 1;
  uint8_t band = 0;
  for (size_t gi = 0; gi < geo_.size(); gi++) {
    const Geo& g = geo_[gi];
    const Emitter& e = g.e;
    // The debug band is per CHUNK, and a chunk's emitters are contiguous in
    // geo_ (the map is walked chunk by chunk): start each chunk at 0.
    if (gi == 0 || !(geo_[gi - 1].key == g.key)) band = 0;
    // The WEIGHT's distance, from the eye. Membership was decided from the
    // window centre (geo_); `dv` decides how much. They are the same number
    // when no eye is set.
    const int dv = std::max(std::abs(e.x - ex),
                            std::max(std::abs(e.y - ey), std::abs(e.z - ez)));
    // THE CROSSFADE SHELL (world.h kGasFarBlendVox): inside the fine box's
    // face by less than the shell, an emitter is in BOTH lists with weights
    // that sum to one. Past the face it is wide only; deeper in, fine only.
    uint32_t wFine = 0;
    if (g.inFine) {
      wFine = dv > fineFadeIn
                  ? 255u - Smooth255(dv - fineFadeIn, fineFadeOut - fineFadeIn)
                  : 255u;
      if (wFine != 0) band = 2;
    } else if (g.nearFine) {
      band = 5;
    }
    // ...and the WIDE side of the same emitter, which is the complement of
    // that times its own outer fade. `outer` is kept SEPARATE from the
    // complement because the promotion below raises the complement to 255
    // and must not lose the fade along with it.
    uint32_t outer = 0;
    if (g.inWide) {
      // THE OUTER SHELL (world.h kGasFarRangeFadeVox): the wide list's own
      // far edge, faded to nothing instead of clipped. Applied BEFORE the
      // aggregation so a coarse cell straddling the edge fades by how much
      // of its fire is past it, which is the same rule the inner shell's
      // strength-weighted mean follows.
      outer = dv > wideFadeStart
                  ? 255u - Smooth255(dv - wideFadeStart, kGasFarRangeFadeVox)
                  : 255u;
      if (outer != 0 && wFine != 255) band = band == 2 ? 6 : 3;
    } else if (band != 2) {
      band = g.outOfRange ? 4 : 5;
    }
    if (PlumeDebug()) dbgBand_[g.key] = band;
    if (wFine == 0 && outer == 0) continue;
    cand.push_back({e, g.d2, wFine, outer});
  }

  if (PlumeDebug()) {
    // Membership transitions since the last build, chunk by chunk. Recorded
    // BEFORE the promotion below, so a chunk that reads `fine` here may still
    // have been served by the wide list because the fine cap was full; the
    // cap report at the end of Build is what says so.
    for (const auto& kv : byChunk_) {
      const uint8_t now = dbgBand_[kv.first];
      auto it = dbgLastBand_.find(kv.first);
      const uint8_t was = it == dbgLastBand_.end() ? 0 : it->second;
      if (was != now)
        std::fprintf(stderr, "plume: build origin (%d,%d,%d): chunk (%d,%d,%d) "
                     "%s -> %s (%zu emitters)\n", windowOriginChunks.x,
                     windowOriginChunks.y, windowOriginChunks.z, kv.first.x,
                     kv.first.y, kv.first.z, kBandName[was], kBandName[now],
                     kv.second.size());
    }
    dbgLastBand_ = dbgBand_;
    dbgBand_.clear();
  }

  // ---- THE FINE CAP, and what OVERFLOWING it now means --------------------
  // The nearest kGasFarEmitMax candidates keep their fine weight; the rest
  // have it ZEROED, which by the complement above hands them to the WIDE list
  // at full mass. That is a real LOD step-down -- a tree that cannot afford a
  // 0.8 m column still gets a 6.4 m one -- where before it was silence, and it
  // is why "a big fire deletes every other fire's smoke" is no longer
  // reachable through this cap.
  std::vector<Ranked>& fine = fine_;
  fine.clear();
  fine.reserve(cand.size());
  for (size_t i = 0; i < cand.size(); i++)
    if (cand[i].wFine != 0) fine.push_back({cand[i].d2, cand[i].e, (uint32_t)i});
  uint32_t promoted = 0;
  const size_t fineWanted = fine.size();
  // `w8` carries the CANDIDATE INDEX through the cap, not a weight; the weight
  // is written back from the candidate once the promotion has run. RankLess is
  // a total order on (distance, position), so the selection is deterministic
  // whatever order the index happened to iterate in.
  CapAndSort(fine, kGasFarEmitMax);
  if (fineWanted > fine.size()) {
    std::vector<uint8_t> kept(cand.size(), 0);
    for (const Ranked& r : fine) kept[r.w8] = 1;
    for (size_t i = 0; i < cand.size(); i++)
      if (cand[i].wFine != 0 && !kept[i]) { cand[i].wFine = 0; promoted++; }
  }
  for (Ranked& r : fine) r.w8 = cand[r.w8].wFine;

  // ---- THE WIDE AGGREGATION, and what OVERFLOWING *it* now means ----------
  // Bucket by the COARSE cell the emitter stands in. Strengths add and the
  // topmost hot voxel wins, so a whole burning hillside becomes ONE emitter
  // whose strength says how much of it is alight -- which is what the shader
  // turns into a taller, denser column. Per cell: the aggregate emitter, plus
  // the WEIGHTED strength sum, so the record's crossfade weight can be the
  // strength-weighted mean of its columns'.
  //
  // AND IF THAT STILL DOES NOT FIT, THE BUCKET GETS COARSER rather than the
  // list getting truncated. Doubling the bucket edge quarters the cell count
  // in x/z, so a few rounds cover any landscape; the record carries a world
  // position and a strength, so nothing downstream knows or cares how wide the
  // bucket was. The shader's own clamps (hMul = sqrt(cols) capped at 4, stack
  // clamped to CHORD^2) are what keep a huge aggregate from drawing a huge
  // plume -- it draws ONE fat column instead of many, which is the LOD answer
  // to "more fires than the list can name".
  std::vector<Ranked>& wideRanked = wideRanked_;
  uint32_t extraShift = 0;
  for (;; extraShift++) {
    // ---- CLUSTERS, NOT GRID CELLS (2026-09-29) ----------------------------
    // Owner report: a burning house gave next to no distant smoke while a tree
    // beside it billowed. Same fire, different SPREAD: a roof is thin per
    // column and spans many chunks, a crown is dense in a few. A fixed grid
    // cut the house into many faint records (each under the renderer's
    // erosion threshold, GAS_CORE_COUNT_WIDE) and could split even a compact
    // fire across a cell edge. The rule now: fire within kClusterVox of a
    // cluster's SEED is one plume, so 1000 burning voxels over five chunks
    // draw what 1000 in one chunk draw.
    //
    // Greedy, strongest first: the strongest unclaimed emitter seeds a
    // cluster and claims every unclaimed emitter within the radius of IT (max
    // norm). No chaining -- a cluster's diameter is bounded by 2x the radius,
    // so a forest fire does not collapse into one record standing somewhere
    // none of its trees is (round seven's "cloud from nowhere"). Ranked by raw
    // strength and position, never by the eye-dependent weight, so walking
    // does not reshuffle clusters and make plumes hop.
    //
    // The record stands at the strength-weighted CENTROID in x/z and the
    // topmost member's y (smoke leaves the top of the fire). The radius
    // doubles with extraShift, the same overflow ladder the grid had.
    constexpr int32_t kClusterVox = 64;   // 6.4 m: a house, a stand of trees
    const int32_t rad = kClusterVox << extraShift;
    std::vector<uint32_t> order;
    order.reserve(cand.size());
    for (uint32_t i = 0; i < (uint32_t)cand.size(); i++)
      if ((255u - cand[i].wFine) * cand[i].outer / 255u != 0) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      const Emitter& ea = cand[a].e;
      const Emitter& eb = cand[b].e;
      if (ea.strength != eb.strength) return ea.strength > eb.strength;
      if (ea.x != eb.x) return ea.x < eb.x;
      if (ea.y != eb.y) return ea.y < eb.y;
      return ea.z < eb.z;
    });
    // Spatial hash at the radius, so a seed only visits the 27 cells round it.
    auto cellOf = [&](const Emitter& e) {
      auto fl = [&](int32_t v) { return v >= 0 ? v / rad : -((-v + rad - 1) / rad); };
      return Key{fl(e.x), fl(e.y), fl(e.z)};
    };
    std::unordered_map<Key, std::vector<uint32_t>, KeyHash> grid;
    for (uint32_t oi = 0; oi < (uint32_t)order.size(); oi++)
      grid[cellOf(cand[order[oi]].e)].push_back(oi);
    std::vector<uint8_t> taken(order.size(), 0);
    wideRanked.clear();
    for (uint32_t oi = 0; oi < (uint32_t)order.size(); oi++) {
      if (taken[oi]) continue;
      const Emitter seed = cand[order[oi]].e;
      const Key sc = cellOf(seed);
      uint64_t str = 0, sw = 0;
      int64_t sx = 0, sz = 0;
      int32_t topY = seed.y;
      for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            auto it = grid.find(Key{sc.x + dx, sc.y + dy, sc.z + dz});
            if (it == grid.end()) continue;
            for (uint32_t mj : it->second) {
              if (taken[mj]) continue;
              const Cand& c = cand[order[mj]];
              if (std::abs(c.e.x - seed.x) > rad || std::abs(c.e.y - seed.y) > rad ||
                  std::abs(c.e.z - seed.z) > rad)
                continue;
              taken[mj] = 1;
              const uint32_t w = (255u - c.wFine) * c.outer / 255u;
              str += c.e.strength;
              sw += (uint64_t)c.e.strength * w;
              sx += (int64_t)c.e.x * c.e.strength;
              sz += (int64_t)c.e.z * c.e.strength;
              topY = std::max(topY, c.e.y);
            }
          }
      Emitter e;
      e.x = str ? (int32_t)(sx / (int64_t)str) : seed.x;
      e.z = str ? (int32_t)(sz / (int64_t)str) : seed.z;
      e.y = topY;
      // Saturating at the 24 bits the record's strength field has.
      e.strength = (uint32_t)std::min<uint64_t>(str, (uint64_t)kGasFarEmitStrengthMask);
      const int ddx = e.x - cx, ddy = e.y - cy, ddz = e.z - cz;
      // Strength-weighted mean of the members' weights, over the UNSATURATED
      // total so it stays a mean.
      const uint32_t w8 = str ? (uint32_t)std::min<uint64_t>(sw / str, 255) : 255;
      wideRanked.push_back(
          {(int64_t)ddx * ddx + (int64_t)ddy * ddy + (int64_t)ddz * ddz, e, w8});
    }
    // TWO DOUBLINGS AND NO MORE, which is a correctness bound and not a
    // budget. Merging buckets CONCENTRATES MASS AT ONE POSITION: an aggregate
    // stands at its topmost member's coordinate and carries the SUM of its
    // members' strengths, and the shader reads that sum as "how big is this
    // fire" (hMul = sqrt(cols)). Coarsen without limit and a scatter of small
    // fires over a wide area collapses into one record that draws a four-times
    // -height column somewhere none of them is -- "billowing clouds from a
    // place with no fire" by a second route. At x4 the cluster radius is 25.6 m
    // (a village). Past that CapAndSort truncates as it always
    // did, and the cap report below says which bound bit.
    if (wideRanked.size() <= kGasFarEmitMaxWide || extraShift >= 2u)
      break;
  }
  const size_t wideWanted = wideRanked.size();

  CapAndSort(wideRanked, kGasFarEmitMaxWide);

  count_ = (uint32_t)fine.size();
  countWide_ = (uint32_t)wideRanked.size();
  capFinePromoted_ = promoted;
  capWideExtraShift_ = extraShift;
  capWideDropped_ = (uint32_t)(wideWanted - wideRanked.size());
  if (PlumeDebug())
    std::fprintf(stderr, "plume: build origin (%d,%d,%d) range %d vox: %u fine, "
                 "%u wide, index %zu chunks\n", windowOriginChunks.x,
                 windowOriginChunks.y, windowOriginChunks.z, rangeVox, count_,
                 countWide_, byChunk_.size());
  // ---- A BOUND CAP IS NEVER SILENT ----------------------------------------
  // Every one of these three used to fail by DELETING smoke and saying
  // nothing, which is how "a big fire is smoking and the trees beside it are
  // not" became a bug report instead of a printed line (CLAUDE.md rule 6:
  // attribution before elimination). Two of them are no longer fatal to a
  // plume — the fine overflow falls into the wide list and the wide bucket
  // coarsens — so this reports the DEGRADATION as well as the loss, and says
  // which of the three is the one actually biting. Throttled to one line per
  // 600 builds so a sustained overflow cannot flood a session log.
  if (promoted || extraShift || capWideDropped_ || refusedChunks_) {
    if (capReportSkip_ == 0) {
      capReportSkip_ = 600;
      std::fprintf(stderr,
                   "plume: caps bound — index %zu/%zu chunks (%llu refused%s), "
                   "fine %u/%u (%u promoted to wide), wide %u/%u at bucket x%u "
                   "(%u dropped%s)\n",
                   byChunk_.size(), kChunkCap,
                   (unsigned long long)refusedChunks_,
                   refusedChunks_ ? " — THESE ARE SILENT" : "", count_,
                   kGasFarEmitMax, promoted, countWide_, kGasFarEmitMaxWide,
                   1u << extraShift, capWideDropped_,
                   capWideDropped_ ? " — THESE ARE SILENT" : "");
    } else {
      capReportSkip_--;
    }
  }
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
  // ONLY A CHANGED IMAGE IS A NEW VERSION. Before the eye was an input, every
  // Build that got this far had something new to say — the index had changed
  // or the window had moved. Now Build runs on most ticks a player is walking,
  // and at 2 voxels of travel the 0..255 weight bytes very often round to the
  // same values, so bumping the version unconditionally would turn "uploaded
  // one tick per window shift" into 8.2 KiB every tick for the rest of the
  // session. The compare is 8 KiB of memcmp against a cost of a queue write
  // plus a barrier.
  if (prevWords_.size() != words_.size() ||
      std::memcmp(prevWords_.data(), words_.data(), words_.size() * 4) != 0) {
    prevWords_ = words_;
    version_++;
  }
}

bool FarPlumes::TakeUpload(const uint32_t** words, uint32_t* wordCount) {
  if (version_ == uploadedVersion_) return false;
  uploadedVersion_ = version_;
  if (words) *words = words_.data();
  if (wordCount) *wordCount = (uint32_t)words_.size();
  return true;
}
