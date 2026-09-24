#include "game/selfclip.h"

#include <algorithm>
#include <cmath>

// See selfclip.h for what this is and why it counts VOXELS rather than boxes.

void ClipShape::Alloc(IVec3 d, float s) {
  dim = d;
  scale = s > 1e-4f ? s : 1.0f;
  solid = 0;
  const size_t cells = (size_t)std::max(0, d.x) * (size_t)std::max(0, d.y) *
                       (size_t)std::max(0, d.z);
  bits.assign((cells + 63) / 64, 0ull);
}

void ClipShape::Set(int x, int y, int z) {
  if (x < 0 || y < 0 || z < 0 || x >= dim.x || y >= dim.y || z >= dim.z) return;
  const size_t i =
      ((size_t)z * (size_t)dim.y + (size_t)y) * (size_t)dim.x + (size_t)x;
  const uint64_t m = 1ull << (i & 63);
  if ((bits[i >> 6] & m) == 0ull) solid++;
  bits[i >> 6] |= m;
}

namespace {

// Index of the lowest set bit. Written out rather than reached for through
// <bit> because this file is compiled by MSVC and the obvious builtin is not.
inline int LowestBit(uint64_t w) {
  int n = 0;
  while ((w & 1ull) == 0ull) {
    w >>= 1;
    n++;
  }
  return n;
}

// A limb's own lattice box, as an axis-aligned box in the CALLER'S frame. Used
// only to reject pairs that cannot possibly meet, so a loose bound is correct
// and a tight one would only cost time: the eight corners of the oriented box,
// min/maxed.
struct Aabb {
  Vec3 lo{}, hi{};
  bool valid = false;
};

Aabb LimbAabb(const ClipLimb& l) {
  Aabb b;
  if (l.shape == nullptr || l.shape->Empty()) return b;
  const float k = 1.0f / l.shape->scale;
  const Vec3 e{(float)l.shape->dim.x * k, (float)l.shape->dim.y * k,
               (float)l.shape->dim.z * k};
  for (int c = 0; c < 8; c++) {
    const Vec3 local{(c & 1) ? e.x : 0.0f, (c & 2) ? e.y : 0.0f,
                     (c & 4) ? e.z : 0.0f};
    const Vec3 p = l.corner + QuatRotate(l.rot, local);
    if (!b.valid) {
      b.lo = b.hi = p;
      b.valid = true;
      continue;
    }
    b.lo = Vec3{std::min(b.lo.x, p.x), std::min(b.lo.y, p.y),
                std::min(b.lo.z, p.z)};
    b.hi = Vec3{std::max(b.hi.x, p.x), std::max(b.hi.y, p.y),
                std::max(b.hi.z, p.z)};
  }
  return b;
}

bool AabbsMeet(const Aabb& a, const Aabb& b) {
  if (!a.valid || !b.valid) return false;
  return a.lo.x <= b.hi.x && a.hi.x >= b.lo.x && a.lo.y <= b.hi.y &&
         a.hi.y >= b.lo.y && a.lo.z <= b.hi.z && a.hi.z >= b.lo.z;
}

}  // namespace

int ClipOverlapCount(const ClipLimb& a, const ClipLimb& b, Vec3* outCentroid) {
  if (a.shape == nullptr || b.shape == nullptr) return 0;
  if (a.shape->Empty() || b.shape->Empty()) return 0;
  const float ka = 1.0f / a.shape->scale;   // world voxels per A cell
  const ClipShape& sa = *a.shape;
  const ClipShape& sb = *b.shape;
  int hits = 0;
  Vec3 sum{};
  // A's cells, centre by centre, into B's lattice. Walking the BITSET rather
  // than the voxel list keeps this independent of how the caller stored the
  // art, and the empty-word skip is what makes a mostly-hollow limb cheap.
  const size_t cells = (size_t)sa.dim.x * (size_t)sa.dim.y * (size_t)sa.dim.z;
  for (size_t w = 0; w < sa.bits.size(); w++) {
    uint64_t word = sa.bits[w];
    while (word != 0ull) {
      const int bit = LowestBit(word);
      word &= word - 1ull;
      const size_t i = (w << 6) + (size_t)bit;
      if (i >= cells) break;
      const int x = (int)(i % (size_t)sa.dim.x);
      const int t = (int)(i / (size_t)sa.dim.x);
      const int y = t % sa.dim.y;
      const int z = t / sa.dim.y;
      // A cell's CENTRE, in A's own frame, in world voxels.
      const Vec3 localA{((float)x + 0.5f) * ka, ((float)y + 0.5f) * ka,
                        ((float)z + 0.5f) * ka};
      const Vec3 p = a.corner + QuatRotate(a.rot, localA);
      const Vec3 localB = QuatRotateInv(b.rot, p - b.corner) * sb.scale;
      if (!sb.At(ifloor(localB.x), ifloor(localB.y), ifloor(localB.z))) continue;
      hits++;
      sum += p;
    }
  }
  if (outCentroid != nullptr && hits > 0) *outCentroid = sum * (1.0f / hits);
  return hits;
}

void RigClipRestBaseline(const std::vector<ClipLimb>& rest,
                         std::vector<int>& out) {
  const size_t n = rest.size();
  out.assign(n * n, 0);
  std::vector<Aabb> boxes(n);
  for (size_t i = 0; i < n; i++) boxes[i] = LimbAabb(rest[i]);
  for (size_t i = 0; i < n; i++) {
    for (size_t j = i + 1; j < n; j++) {
      if (!AabbsMeet(boxes[i], boxes[j])) continue;
      const int v = std::max(ClipOverlapCount(rest[i], rest[j], nullptr),
                             ClipOverlapCount(rest[j], rest[i], nullptr));
      out[i * n + j] = v;
      out[j * n + i] = v;
    }
  }
}

void RigSelfClip(const std::vector<ClipLimb>& posed,
                 const std::vector<int>* baseline, ClipReport& out,
                 int maxHits) {
  out = ClipReport{};
  out.limbs = (int)posed.size();
  if (maxHits < 1) maxHits = 1;

  // The AABBs once, not once per pair: the reject is the hot part and a limb's
  // box does not change between the pairs it is in.
  std::vector<Aabb> boxes(posed.size());
  for (size_t i = 0; i < posed.size(); i++) boxes[i] = LimbAabb(posed[i]);

  for (size_t i = 0; i < posed.size(); i++) {
    if (posed[i].shape == nullptr || posed[i].shape->Empty()) continue;
    for (size_t j = i + 1; j < posed.size(); j++) {
      if (posed[j].shape == nullptr || posed[j].shape->Empty()) continue;
      if (!AabbsMeet(boxes[i], boxes[j])) continue;
      out.pairsTested++;
      // BOTH DIRECTIONS, LARGER WINS. "How many of A's cells are inside B" is
      // not the same number as the reverse when the two lattices differ in
      // pitch — a coarse limb driven into a fine one reports almost nothing
      // from its own side — and a detector that could miss half its cases by
      // an accident of authoring resolution would be worth nothing.
      Vec3 atA{}, atB{};
      const int nAB = ClipOverlapCount(posed[i], posed[j], &atA);
      const int nBA = ClipOverlapCount(posed[j], posed[i], &atB);
      ClipPair pr;
      pr.a = posed[i].slot;
      pr.b = posed[j].slot;
      pr.voxels = std::max(nAB, nBA);
      pr.at = nAB >= nBA ? atA : atB;
      if (pr.voxels == 0) continue;
      if (baseline != nullptr &&
          baseline->size() == posed.size() * posed.size())
        pr.rest = (*baseline)[i * posed.size() + j];
      pr.excess = std::max(0, pr.voxels - pr.rest);
      if (pr.excess <= 0) continue;
      pr.jointed = (posed[i].parent == posed[j].slot) ||
                   (posed[j].parent == posed[i].slot);
      out.pairsHit++;
      if (pr.jointed) {
        if (pr.excess > out.worstJointExcess) {
          out.worstJointExcess = pr.excess;
          out.worstJoint = pr;
        }
      } else if (pr.excess > out.worstExcess) {
        out.worstExcess = pr.excess;
        out.worst = pr;
      }
      out.hits.push_back(pr);
    }
  }
  std::sort(out.hits.begin(), out.hits.end(),
            [](const ClipPair& a, const ClipPair& b) {
              return a.excess > b.excess;
            });
  if ((int)out.hits.size() > maxHits) out.hits.resize((size_t)maxHits);
}
