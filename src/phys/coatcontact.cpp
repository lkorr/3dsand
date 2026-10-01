#include "phys/coatcontact.h"

#include <algorithm>
#include <cmath>

#include "sim/coatrule.h"
#include "sim/microbody.h"

namespace {

bool Owned(MicroBodySet* micro, int model) {
  return micro && model >= 0 && (size_t)model < micro->owned.size() &&
         micro->owned[(size_t)model];
}

float CellDist(const IVec3& v, const Vec3& p) {
  const Vec3 d{(float)v.x + 0.5f - p.x, (float)v.y + 0.5f - p.y,
               (float)v.z + 0.5f - p.z};
  return std::sqrt(d.dot(d));
}

// Nearest first; equal distances by lattice index, so the order never depends
// on anything but the lattice and the point.
void SortCells(std::vector<CoatCell>& c) {
  std::sort(c.begin(), c.end(), [](const CoatCell& a, const CoatCell& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.idx < b.idx;
  });
}

void Write(const StainLattice& L, size_t i, uint16_t next, MicroBodySet* micro,
           int model, bool poke) {
  L.SetStain(i, next);
  if (poke) {
    const IVec3 v = L.At(i);
    MicroBodyPokeStain(*micro, (uint32_t)model, v.x, v.y, v.z, next);
  }
}

}  // namespace

bool LatticeOcc::At(int x, int y, int z) const {
  x -= lo.x;
  y -= lo.y;
  z -= lo.z;
  if (x < 0 || y < 0 || z < 0 || x >= dim.x || y >= dim.y || z >= dim.z)
    return false;
  return occ[(size_t)x + (size_t)y * dim.x + (size_t)z * dim.x * dim.y] != 0;
}

LatticeOcc BuildLatticeOcc(const StainLattice& L) {
  LatticeOcc o;
  const size_t n = L.Size();
  if (n == 0) return o;
  IVec3 lo{INT32_MAX, INT32_MAX, INT32_MAX}, hi{INT32_MIN, INT32_MIN, INT32_MIN};
  for (size_t i = 0; i < n; i++) {
    if (L.Mat(i) == 0) continue;  // tombstone
    const IVec3 v = L.At(i);
    lo.x = std::min(lo.x, v.x); hi.x = std::max(hi.x, v.x);
    lo.y = std::min(lo.y, v.y); hi.y = std::max(hi.y, v.y);
    lo.z = std::min(lo.z, v.z); hi.z = std::max(hi.z, v.z);
  }
  if (lo.x > hi.x) return o;
  const IVec3 dim{hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1};
  if ((uint64_t)dim.x * dim.y * dim.z > (1u << 22)) return o;  // absurd box
  o.lo = lo;
  o.dim = dim;
  o.occ.assign((size_t)dim.x * dim.y * dim.z, 0);
  for (size_t i = 0; i < n; i++) {
    if (L.Mat(i) == 0) continue;
    const IVec3 v = L.At(i);
    o.occ[(size_t)(v.x - lo.x) + (size_t)(v.y - lo.y) * dim.x +
          (size_t)(v.z - lo.z) * dim.x * dim.y] = 1;
  }
  return o;
}

std::vector<CoatCell> CoatContactCells(const StainLattice& L,
                                       const LatticeOcc& occ, Vec3 p,
                                       float reach, bool splitDepth) {
  std::vector<CoatCell> out;
  const size_t n = L.Size();
  if (n == 0 || !occ.Valid() || reach < 0.0f) return out;
  // The nearest exposed voxel: where the two surfaces met.
  float near = 1e30f;
  for (size_t i = 0; i < n; i++) {
    if (L.Mat(i) == 0) continue;
    const IVec3 v = L.At(i);
    const float d = CellDist(v, p);
    if (d < near && occ.Exposed(v)) near = d;
  }
  if (near >= 1e29f) return out;
  const float lim = near + reach;
  std::vector<uint8_t> in;   // lattice index -> already taken (split depth)
  if (splitDepth) in.assign(n, 0);
  for (size_t i = 0; i < n; i++) {
    if (L.Mat(i) == 0) continue;
    const IVec3 v = L.At(i);
    const float d = CellDist(v, p);
    if (d > lim || !occ.Exposed(v)) continue;
    out.push_back(CoatCell{(uint32_t)i, d});
    if (splitDepth) in[i] = 1;
  }
  if (splitDepth) {
    // Under a SPLIT surface cell the coat goes one cell further in: its
    // occupied face neighbours that are not surface themselves. One pass over
    // the lattice against a small set of split cells; a body that has never
    // been beaten open has none and pays only the scan above.
    std::vector<IVec3> split;
    for (const CoatCell& c : out)
      if (BruiseBroken(L.Bruise(c.idx))) split.push_back(L.At(c.idx));
    if (!split.empty()) {
      for (size_t i = 0; i < n; i++) {
        if (in[i] || L.Mat(i) == 0) continue;
        const IVec3 v = L.At(i);
        for (const IVec3& s : split) {
          const int m = std::abs(v.x - s.x) + std::abs(v.y - s.y) +
                        std::abs(v.z - s.z);
          if (m != 1) continue;
          out.push_back(CoatCell{(uint32_t)i, CellDist(v, p)});
          in[i] = 1;
          break;
        }
      }
    }
  }
  SortCells(out);
  return out;
}

std::vector<CoatCell> CoatWoundWall(const StainLattice& L,
                                    const std::vector<IVec3>& removed,
                                    Vec3 p) {
  std::vector<CoatCell> out;
  const size_t n = L.Size();
  if (n == 0 || removed.empty()) return out;
  // The removed set as a bitmap over its own box, padded by one so a wall
  // voxel's face lookup never leaves it.
  IVec3 lo{INT32_MAX, INT32_MAX, INT32_MAX}, hi{INT32_MIN, INT32_MIN, INT32_MIN};
  for (const IVec3& v : removed) {
    lo.x = std::min(lo.x, v.x); hi.x = std::max(hi.x, v.x);
    lo.y = std::min(lo.y, v.y); hi.y = std::max(hi.y, v.y);
    lo.z = std::min(lo.z, v.z); hi.z = std::max(hi.z, v.z);
  }
  lo = IVec3{lo.x - 1, lo.y - 1, lo.z - 1};
  hi = IVec3{hi.x + 1, hi.y + 1, hi.z + 1};
  const IVec3 dim{hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1};
  if ((uint64_t)dim.x * dim.y * dim.z > (1u << 22)) return out;
  std::vector<uint8_t> gone((size_t)dim.x * dim.y * dim.z, 0);
  auto at = [&](int x, int y, int z) -> size_t {
    return (size_t)(x - lo.x) + (size_t)(y - lo.y) * dim.x +
           (size_t)(z - lo.z) * dim.x * dim.y;
  };
  for (const IVec3& v : removed) gone[at(v.x, v.y, v.z)] = 1;
  auto isGone = [&](int x, int y, int z) -> bool {
    if (x < lo.x || y < lo.y || z < lo.z || x > hi.x || y > hi.y || z > hi.z)
      return false;
    return gone[at(x, y, z)] != 0;
  };
  for (size_t i = 0; i < n; i++) {
    if (L.Mat(i) == 0) continue;
    const IVec3 v = L.At(i);
    if (v.x < lo.x || v.y < lo.y || v.z < lo.z || v.x > hi.x || v.y > hi.y ||
        v.z > hi.z)
      continue;
    if (isGone(v.x, v.y, v.z)) continue;  // a stale cell, not a survivor
    if (isGone(v.x - 1, v.y, v.z) || isGone(v.x + 1, v.y, v.z) ||
        isGone(v.x, v.y - 1, v.z) || isGone(v.x, v.y + 1, v.z) ||
        isGone(v.x, v.y, v.z - 1) || isGone(v.x, v.y, v.z + 1))
      out.push_back(CoatCell{(uint32_t)i, CellDist(v, p)});
  }
  SortCells(out);
  return out;
}

std::vector<CoatParcel> CoatOffer(const StainLattice& L,
                                  const std::vector<CoatCell>& cells,
                                  float frac, uint32_t maxLevels) {
  std::vector<CoatParcel> per;
  if (cells.empty() || !(frac > 0.0f) || maxLevels == 0) return per;
  for (const CoatCell& c : cells) {
    const uint16_t s = L.Stain(c.idx);
    const uint32_t amt = BodyStainAmt(s), mat = BodyStainMat(s);
    if (amt == 0 || mat == 0) continue;
    CoatParcel* p = nullptr;
    for (CoatParcel& q : per)
      if (q.mat == mat) p = &q;
    if (!p) {
      per.push_back(CoatParcel{mat, 0, 0});
      p = &per.back();
    }
    p->levels += amt;
    p->peak = std::max(p->peak, amt);
  }
  std::sort(per.begin(), per.end(), [](const CoatParcel& a, const CoatParcel& b) {
    if (a.levels != b.levels) return a.levels > b.levels;
    return a.mat < b.mat;
  });
  const float f = std::min(frac, 1.0f);
  uint32_t left = maxLevels;
  std::vector<CoatParcel> out;
  for (CoatParcel p : per) {
    if (left == 0) break;
    uint32_t give = (uint32_t)std::lround((float)p.levels * f);
    give = std::max(give, 1u);
    give = std::min({give, p.levels, left});
    left -= give;
    p.levels = give;
    out.push_back(p);
  }
  return out;
}

uint32_t CoatLay(const StainLattice& L, const std::vector<CoatCell>& cells,
                 float reach, std::vector<CoatParcel>& parcels,
                 MicroBodySet* micro, int model) {
  const bool poke = Owned(micro, model);
  uint32_t changed = 0;
  const float near = cells.empty() ? 0.0f : cells.front().dist;
  const float span = std::max(reach, 1e-3f);
  for (CoatParcel& p : parcels) {
    uint32_t left = p.levels;
    uint32_t written = 0;
    const bool washes = (BodyCoatClassOf(p.mat) & kBodyCoatWashes) != 0;
    for (const CoatCell& c : cells) {
      if (left == 0) break;
      // Thickest at the contact, half of it at the patch's rim.
      const float t = std::clamp((c.dist - near) / span, 0.0f, 1.0f);
      uint32_t want = (uint32_t)std::lround((float)p.peak * (1.0f - 0.5f * t));
      want = std::clamp<uint32_t>(want, 1u, kBodyStainAmtMax);
      want = std::min(want, left);
      const uint16_t cur = L.Stain(c.idx);
      uint16_t next;
      uint32_t gained = 0;
      if (washes) {
        // A washer rinses what is there first and only then wets (the body's
        // own washing rule, WashBodyStain): every level of it that touched
        // the voxel did something, and is what it costs.
        next = WashBodyStain(cur, p.mat, want, want);
        if (next != cur) gained = want;
      } else {
        next = AddBodyStain(cur, p.mat, want, kBodyStainAmtMax);
        const uint32_t before =
            BodyStainMat(cur) == p.mat ? BodyStainAmt(cur) : 0u;
        const uint32_t after =
            BodyStainMat(next) == p.mat ? BodyStainAmt(next) : 0u;
        gained = after > before ? after - before : 0u;
      }
      if (next == cur) continue;
      gained = std::min(gained, left);
      Write(L, c.idx, next, micro, model, poke);
      changed++;
      left -= gained;
      written += gained;
    }
    p.levels = written;
  }
  return changed;
}

uint32_t CoatSpend(const StainLattice& L, const std::vector<CoatCell>& cells,
                   const std::vector<CoatParcel>& parcels, MicroBodySet* micro,
                   int model) {
  const bool poke = Owned(micro, model);
  uint32_t spent = 0;
  for (const CoatParcel& p : parcels) {
    uint32_t left = p.levels;
    for (const CoatCell& c : cells) {
      if (left == 0) break;
      const uint16_t cur = L.Stain(c.idx);
      if (BodyStainMat(cur) != p.mat) continue;
      const uint32_t amt = BodyStainAmt(cur);
      if (amt == 0) continue;
      const uint32_t take = std::min(amt, left);
      Write(L, c.idx, PackBodyStain(p.mat, amt - take), micro, model, poke);
      left -= take;
      spent += take;
    }
  }
  return spent;
}

uint32_t CoatSmear(const StainLattice& L, const std::vector<CoatCell>& cells,
                   uint32_t mat, uint32_t amt, MicroBodySet* micro, int model) {
  if (mat == 0 || amt == 0) return 0;
  amt = std::min(amt, kBodyStainAmtMax);
  const bool poke = Owned(micro, model);
  const uint32_t cls = BodyCoatClassOf(mat);
  uint32_t changed = 0;
  for (const CoatCell& c : cells) {
    const uint16_t cur = L.Stain(c.idx);
    const uint32_t curAmt = BodyStainAmt(cur), curMat = BodyStainMat(cur);
    const uint32_t verdict = stainPrecedence(
        curAmt, curMat == mat, amt, (cls & kBodyCoatWashes) != 0,
        (cls & kBodyCoatCorrodes) != 0,
        (BodyCoatClassOf(curMat) & kBodyCoatCorrodes) != 0, /*paid=*/false);
    uint16_t next = cur;
    if (verdict == kStainPrecOwn)
      next = PackBodyStain(mat, std::max(curAmt, amt));
    else if (verdict == kStainPrecOver)
      next = PackBodyStain(mat, amt);
    if (next == cur) continue;
    Write(L, c.idx, next, micro, model, poke);
    changed++;
  }
  return changed;
}
