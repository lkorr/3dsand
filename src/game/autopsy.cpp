#include "game/autopsy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "sim/materials.h"
#include "ui/overlay.h"

namespace autopsy {

void MeasureDepth(const MicroBodySet& set, uint32_t model, DepthField& out) {
  const IVec3 d = MicroBodyDims(set, model);
  out.dx = d.x; out.dy = d.y; out.dz = d.z;
  out.maxDepth = -1;
  const size_t cells = (size_t)std::max(0, d.x) * std::max(0, d.y) * std::max(0, d.z);
  out.depth.assign(cells, kEmpty);
  if (cells == 0) return;
  const uint32_t cut = model < set.models.size() ? (set.models[model].cutFaces & 0x3Fu) : 0u;

  // Solid = a non-zero material byte. Read once into the depth array as a
  // sentinel (kEmpty / 0xFE "solid, not yet reached").
  constexpr uint8_t kUnseen = 0xFE;
  const MicroBodyModelGpu& m = set.models[model];
  for (size_t i = 0; i < cells; i++) {
    const uint32_t w = m.base + (uint32_t)(i >> 1);
    if (w >= set.pool.size()) break;
    const uint32_t v = (set.pool[w] >> ((i & 1u) * 16u)) & 0xFFFFu;
    if ((v & 0xFFu) != 0) out.depth[i] = kUnseen;
  }

  // Is the cell across face (axis, positive) of (x,y,z) solid? Outside the
  // brick it is solid only through a JOINT (cutFaces bit axis*2+positive).
  auto idx = [&](int x, int y, int z) { return ((size_t)z * d.y + y) * d.x + x; };
  auto solidAcross = [&](int x, int y, int z, int axis, int pos) {
    int c[3] = {x, y, z};
    const int dim[3] = {d.x, d.y, d.z};
    c[axis] += pos ? 1 : -1;
    if (c[axis] < 0 || c[axis] >= dim[axis])
      return ((cut >> (axis * 2 + pos)) & 1u) != 0;
    return out.depth[idx(c[0], c[1], c[2])] != kEmpty;
  };

  // Seeds: every solid cell with an exposed face.
  std::vector<uint32_t> frontier, next;
  for (int z = 0; z < d.z; z++)
    for (int y = 0; y < d.y; y++)
      for (int x = 0; x < d.x; x++) {
        const size_t i = idx(x, y, z);
        if (out.depth[i] == kEmpty) continue;
        bool exposed = false;
        for (int a = 0; a < 3 && !exposed; a++)
          for (int p = 0; p < 2 && !exposed; p++)
            if (!solidAcross(x, y, z, a, p)) exposed = true;
        if (exposed) {
          out.depth[i] = 0;
          frontier.push_back((uint32_t)i);
        }
      }
  // Multi-source BFS inward. Order-independent: the step count to the nearest
  // seed is the same however the frontier is walked.
  int depth = 0;
  while (!frontier.empty()) {
    out.maxDepth = depth;
    next.clear();
    const uint8_t nd = (uint8_t)std::min(depth + 1, 0xFD);
    for (uint32_t i : frontier) {
      const int x = (int)(i % (uint32_t)d.x);
      const int y = (int)((i / (uint32_t)d.x) % (uint32_t)d.y);
      const int z = (int)(i / ((uint32_t)d.x * (uint32_t)d.y));
      const int nb[6][3] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z},
                            {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};
      for (const auto& c : nb) {
        if (c[0] < 0 || c[1] < 0 || c[2] < 0 || c[0] >= d.x || c[1] >= d.y ||
            c[2] >= d.z)
          continue;
        const size_t j = idx(c[0], c[1], c[2]);
        if (out.depth[j] != kUnseen) continue;
        out.depth[j] = nd;
        next.push_back((uint32_t)j);
      }
    }
    frontier.swap(next);
    depth++;
  }
  // A solid cell no seed reaches (a sealed brick whose every face is a joint)
  // is as deep as it gets.
  for (uint8_t& v : out.depth)
    if (v == kUnseen) v = (uint8_t)std::max(0, out.maxDepth);
}

bool PickBrick(const MicroBodySet& set, uint32_t model, const float ro[3],
               const float rd[3], float& tHit, int cell[3]) {
  const IVec3 d = MicroBodyDims(set, model);
  const int dim[3] = {d.x, d.y, d.z};
  if (d.x <= 0 || d.y <= 0 || d.z <= 0) return false;
  // Slab test against the brick box.
  float t0 = 0.0f, t1 = 1e30f;
  for (int a = 0; a < 3; a++) {
    if (std::fabs(rd[a]) < 1e-12f) {
      if (ro[a] < 0.0f || ro[a] > (float)dim[a]) return false;
      continue;
    }
    float ta = (0.0f - ro[a]) / rd[a], tb = ((float)dim[a] - ro[a]) / rd[a];
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
  }
  if (t1 < t0) return false;
  // DDA (Amanatides-Woo) from just inside the entry.
  float t = t0 + 1e-4f;
  int c[3], step[3];
  float tMax[3], tDelta[3];
  for (int a = 0; a < 3; a++) {
    const float p = ro[a] + rd[a] * t;
    c[a] = std::clamp((int)std::floor(p), 0, dim[a] - 1);
    step[a] = rd[a] > 0 ? 1 : -1;
    if (std::fabs(rd[a]) < 1e-12f) {
      tMax[a] = tDelta[a] = 1e30f;
    } else {
      const float edge = (float)(c[a] + (rd[a] > 0 ? 1 : 0));
      tMax[a] = (edge - ro[a]) / rd[a];
      tDelta[a] = std::fabs(1.0f / rd[a]);
    }
  }
  for (int guard = 0; guard < 4096; guard++) {
    if (MicroBodyCell(set, model, c[0], c[1], c[2]) & 0xFFu) {
      tHit = t;
      cell[0] = c[0]; cell[1] = c[1]; cell[2] = c[2];
      return true;
    }
    int a = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
    t = tMax[a];
    c[a] += step[a];
    if (c[a] < 0 || c[a] >= dim[a] || t > t1) return false;
    tMax[a] += tDelta[a];
  }
  return false;
}

uint32_t CellStain(const MicroBodySet& set, uint32_t model, int x, int y, int z) {
  if (model >= set.models.size()) return 0;
  const MicroBodyModelGpu& m = set.models[model];
  if ((m.dims & kMicroBodyDimsStainBit) == 0) return 0;
  const IVec3 d = MicroBodyDims(set, model);
  if (x < 0 || y < 0 || z < 0 || x >= d.x || y >= d.y || z >= d.z) return 0;
  const size_t cells = (size_t)d.x * d.y * d.z;
  const size_t idx = ((size_t)z * d.y + y) * d.x + x;
  const size_t w = m.base + (cells + 1) / 2 + idx;
  return w < set.pool.size() ? set.pool[w] : 0u;
}

uint32_t Peeler::DrawModel(uint32_t src) const {
  auto it = entries_.find(src);
  if (it == entries_.end()) return src;
  return it->second.clone;
}

void Peeler::Release(MicroBodySet& set) {
  if (set.epoch == setEpoch_)
    for (auto& [src, e] : entries_)
      if (e.clone != kHidden) MicroBodyFree(set, e.clone);
  entries_.clear();
  measuredKey_.clear();
  body_.Clear();
  for (Strata& s : slots_) s.Clear();
  gen_++;
}

bool Peeler::Update(MicroBodySet& set, const std::vector<Part>& parts, int peel,
                    double now, int slotCount, double throttle) {
  // A replaced set: every index held is meaningless. Forget, never free.
  if (set.epoch != setEpoch_) {
    entries_.clear();
    measuredKey_.clear();
    setEpoch_ = set.epoch;
  }
  if ((int)slots_.size() != slotCount) slots_.assign((size_t)std::max(0, slotCount), Strata{});

  // ---- the strata ------------------------------------------------------------
  // Keyed on every limb's (model, editGen), so a still-changing corpse is
  // re-measured — throttled like the clones are.
  std::vector<uint64_t> key;
  key.reserve(parts.size());
  for (const Part& p : parts)
    if (p.isLimb)
      key.push_back(((uint64_t)p.model << 32) | MicroBodyEditGen(set, p.model));
  if (key != measuredKey_ && (measuredKey_.empty() || now - measuredAt_ >= throttle)) {
    measuredKey_ = key;
    measuredAt_ = now;
    body_.Clear();
    for (Strata& s : slots_) s.Clear();
    for (const Part& p : parts) {
      if (!p.isLimb || p.model >= set.models.size()) continue;
      MeasureDepth(set, p.model, scratch_);
      if (scratch_.maxDepth < 0) continue;
      Strata* slot = p.figSlot >= 0 && p.figSlot < (int)slots_.size()
                         ? &slots_[(size_t)p.figSlot] : nullptr;
      const MicroBodyModelGpu& m = set.models[p.model];
      for (size_t i = 0; i < scratch_.depth.size(); i++) {
        const uint8_t dep = scratch_.depth[i];
        if (dep == kEmpty) continue;
        const uint32_t w = m.base + (uint32_t)(i >> 1);
        if (w >= set.pool.size()) break;
        const uint32_t mat = (set.pool[w] >> ((i & 1u) * 16u)) & 0xFFu;
        const int L = std::min<int>(dep, kMaxLayers - 1);
        body_.layers[L].total++;
        body_.layers[L].byMat[mat]++;
        body_.maxDepth = std::max(body_.maxDepth, L);
        if (slot) {
          slot->layers[L].total++;
          slot->layers[L].byMat[mat]++;
          slot->maxDepth = std::max(slot->maxDepth, L);
        }
      }
    }
    gen_++;
  }

  // ---- the clones -----------------------------------------------------------
  bool changed = false;
  // Which sources are drawn this frame; anything else held is stale.
  std::unordered_map<uint32_t, bool> want;
  if (peel > 0)
    for (const Part& p : parts)
      if (p.isLimb && p.model < set.models.size()) want[p.model] = true;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (want.count(it->first) == 0) {
      if (it->second.clone != kHidden) MicroBodyFree(set, it->second.clone);
      it = entries_.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  for (const auto& [src, unused] : want) {
    (void)unused;
    const MicroBodyModelGpu& sm = set.models[src];
    const uint32_t g = MicroBodyEditGen(set, src);
    auto it = entries_.find(src);
    if (it != entries_.end()) {
      const Entry& e = it->second;
      const bool sameSrc = e.srcBase == sm.base && e.srcDims == sm.dims;
      if (sameSrc && e.peel == peel && e.srcGen == g) continue;
      // Same source, only its content moved (still burning): throttle.
      if (sameSrc && e.peel == peel && now - e.madeAt < throttle) continue;
      if (e.clone != kHidden) MicroBodyFree(set, e.clone);
      entries_.erase(it);
    }
    Entry e;
    e.srcGen = g;
    e.srcBase = sm.base;
    e.srcDims = sm.dims;
    e.peel = peel;
    e.madeAt = now;
    MeasureDepth(set, src, scratch_);
    if (scratch_.maxDepth < peel) {
      e.clone = kHidden;  // nothing left at this depth
    } else {
      const int c = MicroBodyClone(set, src);
      if (c < 0) {
        // The pool refused: draw the unpeeled source rather than nothing, and
        // count it (the panel says so).
        refusals_++;
        continue;
      }
      e.clone = (uint32_t)c;
      for (int z = 0; z < scratch_.dz; z++)
        for (int y = 0; y < scratch_.dy; y++)
          for (int x = 0; x < scratch_.dx; x++) {
            const uint8_t dep = scratch_.At(x, y, z);
            if (dep != kEmpty && dep < peel)
              MicroBodyPoke(set, e.clone, x, y, z, 0, 0);
          }
    }
    entries_[src] = e;
    changed = true;
  }
  return changed;
}

void FillUI(const Peeler& p, const uint8_t* kinds,
            const std::vector<MaterialDef>& mats, UIState& ui) {
  UIState::AutopsyUI& a = ui.autopsy;
  a.clonesLive = p.ClonesLive();
  a.refusals = p.Refusals();
  if (a.gen == p.Generation() && a.maxDepth == p.MaxDepth() && a.legendCount > 0)
    return;
  a.gen = p.Generation();
  a.maxDepth = p.MaxDepth();
  using A = UIState::AutopsyUI;

  // ---- the legend: every material in the body, damage first, then by count.
  uint32_t total[256] = {};
  const Strata& body = p.Body();
  for (int L = 0; L <= body.maxDepth && L < kMaxLayers; L++)
    for (int m = 0; m < 256; m++) total[m] += body.layers[L].byMat[m];
  int order[256];
  int n = 0;
  for (int m = 1; m < 256; m++)
    if (total[m]) order[n++] = m;
  auto rank = [&](int m) {
    const uint8_t k = kinds ? kinds[m] : 0;
    return k == kInfection ? 0 : k == kBurn ? 1 : k == kTissue ? 2 : 3;
  };
  std::sort(order, order + n, [&](int x, int y) {
    if (rank(x) != rank(y)) return rank(x) < rank(y);
    return total[x] > total[y];
  });
  uint8_t legOf[256];
  std::memset(legOf, 0xFF, sizeof legOf);
  a.legendCount = 0;
  for (int i = 0; i < n && a.legendCount < A::kLegend; i++) {
    const int m = order[i];
    A::Legend& l = a.legend[a.legendCount];
    l = A::Legend{};
    l.count = total[m];
    l.kind = kinds ? kinds[m] : 0;
    l.color = (size_t)m < mats.size() ? (0xFF000000u | (mats[(size_t)m].gpu.color0 & 0x00FFFFFFu))
                                      : 0xFF808080u;
    std::string name = (size_t)m < mats.size()
                           ? (!mats[(size_t)m].infectLabel.empty() ? mats[(size_t)m].infectLabel
                                                                   : mats[(size_t)m].name)
                           : std::string("material ") + std::to_string(m);
    for (char& c : name)
      if (c == '_') c = ' ';
    std::snprintf(l.label, sizeof l.label, "%s", name.c_str());
    legOf[m] = (uint8_t)a.legendCount;
    a.legendCount++;
  }

  // ---- a layer's composition: its heaviest materials, as legend indices.
  auto fillLayer = [&](const Layer& src, A::Layer& dst) {
    dst = A::Layer{};
    dst.total = src.total;
    for (int m = 1; m < 256; m++) {
      const uint32_t c = src.byMat[m];
      if (!c || legOf[m] == 0xFF) continue;
      int at = dst.n;
      while (at > 0 && dst.count[at - 1] < c) at--;
      if (at >= A::kLayerMats) continue;
      for (int j = std::min<int>(dst.n, A::kLayerMats - 1); j > at; j--) {
        dst.leg[j] = dst.leg[j - 1];
        dst.count[j] = dst.count[j - 1];
      }
      dst.leg[at] = legOf[m];
      dst.count[at] = c;
      dst.n = (uint8_t)std::min<int>(dst.n + 1, A::kLayerMats);
    }
  };
  a.bodyDepth = std::min(body.maxDepth, A::kLayers - 1);
  for (int L = 0; L < A::kLayers; L++) fillLayer(body.layers[L], a.body[L]);
  for (int s = 0; s < UIState::kSlotCount; s++) {
    if (s >= p.SlotCount()) {
      a.partDepth[s] = -1;
      continue;
    }
    const Strata& st = p.Slot(s);
    a.partDepth[s] = std::min(st.maxDepth, A::kLayers - 1);
    for (int L = 0; L < A::kLayers; L++) fillLayer(st.layers[L], a.part[s][L]);
  }

  // ---- findings: damage materials per part, shallowest..deepest layer.
  std::vector<A::Finding> f;
  for (int s = 0; s < std::min(p.SlotCount(), (int)UIState::kSlotCount); s++) {
    const Strata& st = p.Slot(s);
    for (int m = 1; m < 256; m++) {
      const uint8_t k = kinds ? kinds[m] : 0;
      if ((k != kBurn && k != kInfection) || legOf[m] == 0xFF) continue;
      A::Finding fi;
      fi.leg = legOf[m];
      fi.slot = (int8_t)s;
      int lo = -1, hi = -1;
      for (int L = 0; L <= st.maxDepth && L < kMaxLayers; L++) {
        const uint32_t c = st.layers[L].byMat[m];
        if (!c) continue;
        fi.count += c;
        if (lo < 0) lo = L;
        hi = L;
      }
      if (fi.count == 0) continue;
      fi.shallowest = (uint8_t)lo;
      fi.deepest = (uint8_t)hi;
      f.push_back(fi);
    }
  }
  // Infection before burn; deeper before shallower; bigger before smaller.
  std::sort(f.begin(), f.end(), [&](const A::Finding& x, const A::Finding& y) {
    const uint8_t kx = a.legend[x.leg].kind, ky = a.legend[y.leg].kind;
    if (kx != ky) return kx > ky;
    if (x.deepest != y.deepest) return x.deepest > y.deepest;
    return x.count > y.count;
  });
  a.findingCount = std::min((int)f.size(), A::kFindings);
  for (int i = 0; i < a.findingCount; i++) a.findings[i] = f[(size_t)i];
}

}  // namespace autopsy
