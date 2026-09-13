#include "phys/bodystain.h"

#include <algorithm>
#include <cmath>

#include "sim/microbody.h"
#include "sim/rng.h"

uint8_t RaiseBodyStain(uint8_t cur, uint32_t type, uint32_t amt) {
  if (amt == 0 || type == 0) return cur;
  const uint32_t curAmt = BodyStainAmt(cur), curType = BodyStainType(cur);
  if (curAmt == 0 || curType == type) return PackBodyStain(type, std::max(curAmt, amt));
  return amt > curAmt ? PackBodyStain(type, amt) : cur;
}

uint32_t SoakCut(const StainLattice& L, Vec3 centre, const CutSoak& p,
                 uint32_t seed, MicroBodySet* micro, int model) {
  if (p.type == 0 || p.radius <= 0.0f) return 0;
  if (p.amountExposed <= 0 && p.amountBuried <= 0 && p.boneMin <= 0) return 0;
  const size_t n = L.Size();
  if (n == 0) return 0;

  // WHAT IS EXPOSED: an occupancy bitmap over the lattice's box, built here on
  // the cut tick only -- the same O(voxels) the loop below already pays. A
  // buried voxel mostly does not take the stain (it only shows if a later cut
  // reaches it, and a soaked interior would hide the anatomy under a uniform
  // red); an exposed one always does.
  IVec3 lo{INT32_MAX, INT32_MAX, INT32_MAX}, hi{INT32_MIN, INT32_MIN, INT32_MIN};
  for (size_t i = 0; i < n; i++) {
    const IVec3 v = L.At(i);
    lo.x = std::min(lo.x, v.x); hi.x = std::max(hi.x, v.x);
    lo.y = std::min(lo.y, v.y); hi.y = std::max(hi.y, v.y);
    lo.z = std::min(lo.z, v.z); hi.z = std::max(hi.z, v.z);
  }
  const int dx = hi.x - lo.x + 1, dy = hi.y - lo.y + 1, dz = hi.z - lo.z + 1;
  if (dx <= 0 || dy <= 0 || dz <= 0) return 0;
  if ((uint64_t)dx * dy * dz > (1u << 22)) return 0;  // absurd box: refuse
  std::vector<uint8_t> occ((size_t)dx * dy * dz, 0);
  auto occAt = [&](int x, int y, int z) -> bool {
    x -= lo.x; y -= lo.y; z -= lo.z;
    if (x < 0 || y < 0 || z < 0 || x >= dx || y >= dy || z >= dz) return false;
    return occ[(size_t)x + (size_t)y * dx + (size_t)z * dx * dy] != 0;
  };
  for (size_t i = 0; i < n; i++) {
    if (L.Mat(i) == 0) continue;  // tombstone
    const IVec3 v = L.At(i);
    occ[(size_t)(v.x - lo.x) + (size_t)(v.y - lo.y) * dx +
        (size_t)(v.z - lo.z) * dx * dy] = 1;
  }
  auto exposed = [&](IVec3 v) -> bool {
    return !occAt(v.x - 1, v.y, v.z) || !occAt(v.x + 1, v.y, v.z) ||
           !occAt(v.x, v.y - 1, v.z) || !occAt(v.x, v.y + 1, v.z) ||
           !occAt(v.x, v.y, v.z - 1) || !occAt(v.x, v.y, v.z + 1);
  };

  // The brick only takes pokes when it is this body's own (COW). A shared
  // model is left alone: the caller owns it before calling, or re-skins.
  bool poke = false;
  if (micro && model >= 0 && (size_t)model < micro->owned.size() &&
      micro->owned[(size_t)model])
    poke = true;

  const float r2 = p.radius * p.radius;
  static const std::vector<uint8_t> kNoTissue;
  const std::vector<uint8_t>& tissue = p.tissue ? *p.tissue : kNoTissue;
  uint32_t changed = 0;
  for (size_t i = 0; i < n; i++) {
    const uint32_t mat = L.Mat(i);
    if (mat == 0) continue;
    const IVec3 v = L.At(i);
    const Vec3 d{(float)v.x + 0.5f - centre.x, (float)v.y + 0.5f - centre.y,
                 (float)v.z + 0.5f - centre.z};
    const float d2 = d.dot(d);
    if (d2 >= r2) continue;
    const float t = std::sqrt(d2 / r2);  // 0 at the cut, 1 at the rim
    const uint32_t h = rng::Hash3(seed, (uint32_t)v.x * 73856093u,
                                  (uint32_t)v.y * 19349663u ^
                                      (uint32_t)v.z * 83492791u);
    const float roll = (float)(h & 0xFFFFu) / 65535.0f;
    // Per-voxel jitter on the amount, 0.6..1.0, so the smear is uneven the way
    // blood over a surface is: a flat gradient reads as an airbrushed decal.
    const float jitter = 0.6f + 0.4f * (float)((h >> 16) & 0xFFu) / 255.0f;
    int amt = 0;
    if (exposed(v)) {
      // Tapers with the SQUARE of the distance: near-full over the inner half,
      // then thinning fast, so the rim is a spatter rather than a gradient.
      amt = (int)std::lround((float)p.amountExposed * (1.0f - t * t) * jitter);
      const bool isTissue =
          tissue.empty() || (mat < tissue.size() && tissue[mat]);
      if (!isTissue) {
        // Bone is always shown bloodied: at least the floor, jittered so it
        // varies from voxel to voxel rather than reading as a uniform coat.
        const int floorAmt = (int)std::lround((float)p.boneMin * (0.7f + 0.3f * jitter));
        amt = std::max(amt, floorAmt);
      }
    } else {
      if (roll >= p.buriedChance * (1.0f - t * t)) continue;
      amt = (int)std::lround((float)p.amountBuried * jitter);
    }
    if (amt <= 0) continue;
    const uint8_t cur = L.Stain(i);
    const uint8_t next = RaiseBodyStain(cur, p.type, (uint32_t)amt);
    if (next == cur) continue;
    L.SetStain(i, next);
    changed++;
    if (poke) MicroBodyPokeStain(*micro, (uint32_t)model, v.x, v.y, v.z, next);
  }
  return changed;
}
