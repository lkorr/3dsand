#include "phys/bodystain.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "sim/microbody.h"
#include "sim/rng.h"
#include "sim/coatrule.h"

namespace {
// The coat CLASS of every material (kBodyCoat*), published by the owner of the
// material tables on every load (MobSystem::SetMaterials). Derived data, one
// row per material; empty = no class known, so only weight decides.
std::vector<uint8_t> gCoatClass;
bool ClassHas(uint32_t mat, uint8_t bit) {
  return mat < gCoatClass.size() && (gCoatClass[mat] & bit) != 0;
}
// The one precedence rule (sim/coatrule.h stainPrecedence) for a body coat.
// Every body coat is substance (DESIGN.md §6 rule 0), so every level is PAID.
uint32_t BodyPrecedence(uint16_t cur, uint32_t mat, uint32_t amt) {
  const uint32_t curAmt = BodyStainAmt(cur), curMat = BodyStainMat(cur);
  return stainPrecedence(curAmt, curMat == mat, amt,
                         ClassHas(mat, kBodyCoatWashes),
                         ClassHas(curMat, kBodyCoatWashes),
                         ClassHas(mat, kBodyCoatCorrodes),
                         ClassHas(curMat, kBodyCoatCorrodes), /*paid=*/true);
}
}  // namespace

void SetBodyCoatClasses(std::vector<uint8_t> classes) {
  gCoatClass = std::move(classes);
}

uint32_t BodyCoatClassOf(uint32_t mat) {
  return mat < gCoatClass.size() ? gCoatClass[mat] : 0u;
}

uint16_t RaiseBodyStain(uint16_t cur, uint32_t mat, uint32_t amt) {
  if (amt == 0 || mat == 0) return cur;
  switch (BodyPrecedence(cur, mat, amt)) {
    case kStainPrecOwn:
      return PackBodyStain(mat, std::max<uint32_t>(BodyStainAmt(cur), amt));
    case kStainPrecOver:
      return PackBodyStain(mat, amt);
    default:  // a rinse is WashBodyStain's road; a refusal leaves it be
      return cur;
  }
}

uint16_t WashBodyStain(uint16_t cur, uint32_t washMat, uint32_t wetAmt,
                       uint32_t rinse) {
  if (washMat == 0) return cur;
  const uint32_t curAmt = BodyStainAmt(cur), curMat = BodyStainMat(cur);
  // The precedence rule says RINSE for any foreign coat (the caller is a
  // washer by definition) and OWN for clean or already wet.
  if (curAmt != 0 && curMat != washMat) {
    if (curAmt > rinse) return PackBodyStain(curMat, curAmt - rinse);
    return PackBodyStain(washMat, wetAmt);  // amount 0 packs as clean
  }
  if (wetAmt == 0) return cur;
  return PackBodyStain(washMat, std::max(curAmt, wetAmt));
}

uint16_t AddBodyStain(uint16_t cur, uint32_t mat, uint32_t add, uint32_t cap) {
  if (add == 0 || mat == 0) return cur;
  cap = std::min<uint32_t>(cap, kBodyStainAmtMax);
  if (cap == 0) return cur;
  const uint32_t curAmt = BodyStainAmt(cur);
  switch (BodyPrecedence(cur, mat, add)) {
    case kStainPrecOwn:
      if (BodyStainMat(cur) != mat) return PackBodyStain(mat, std::min(add, cap));
      if (curAmt >= cap) return cur;   // already at the ceiling: nothing to add
      return PackBodyStain(mat, std::min(curAmt + add, cap));
    case kStainPrecOver:
      return PackBodyStain(mat, std::min(add, cap));
    default:
      return cur;
  }
}

CellDist BuildCellDist(const std::vector<IVec3>& seeds, int pad) {
  CellDist f;
  if (seeds.empty() || pad < 0) return f;
  IVec3 lo{INT32_MAX, INT32_MAX, INT32_MAX}, hi{INT32_MIN, INT32_MIN, INT32_MIN};
  for (const IVec3& v : seeds) {
    lo.x = std::min(lo.x, v.x); hi.x = std::max(hi.x, v.x);
    lo.y = std::min(lo.y, v.y); hi.y = std::max(hi.y, v.y);
    lo.z = std::min(lo.z, v.z); hi.z = std::max(hi.z, v.z);
  }
  f.lo = IVec3{lo.x - pad, lo.y - pad, lo.z - pad};
  f.dim = IVec3{hi.x - lo.x + 1 + 2 * pad, hi.y - lo.y + 1 + 2 * pad,
                hi.z - lo.z + 1 + 2 * pad};
  const int dx = f.dim.x, dy = f.dim.y, dz = f.dim.z;
  if (dx <= 0 || dy <= 0 || dz <= 0) return CellDist{};
  if ((uint64_t)dx * dy * dz > (1u << 22)) return CellDist{};  // absurd box
  // kFar must survive `+5` without wrapping the u16 and must stay far larger
  // than any rim a caller asks about.
  constexpr uint16_t kFar = 60000;
  f.d.assign((size_t)dx * dy * dz, kFar);
  auto idx = [&](int x, int y, int z) -> size_t {
    return ((size_t)z * dy + y) * dx + x;
  };
  for (const IVec3& v : seeds)
    f.d[idx(v.x - f.lo.x, v.y - f.lo.y, v.z - f.lo.z)] = 0;
  // The 13 neighbours that precede (z,y,x) in the forward sweep; the backward
  // sweep uses their negations. Weight 3 across a face, 4 an edge, 5 a corner.
  static const int kPred[13][3] = {
      {-1, -1, -1}, {-1, -1, 0}, {-1, -1, 1}, {-1, 0, -1}, {-1, 0, 0},
      {-1, 0, 1},   {-1, 1, -1}, {-1, 1, 0},  {-1, 1, 1},  {0, -1, -1},
      {0, -1, 0},   {0, -1, 1},  {0, 0, -1}};
  auto weight = [](const int o[3]) -> int {
    const int m = std::abs(o[0]) + std::abs(o[1]) + std::abs(o[2]);
    return m == 1 ? 3 : m == 2 ? 4 : 5;
  };
  auto sweep = [&](bool forward) {
    for (int zi = 0; zi < dz; zi++) {
      const int z = forward ? zi : dz - 1 - zi;
      for (int yi = 0; yi < dy; yi++) {
        const int y = forward ? yi : dy - 1 - yi;
        for (int xi = 0; xi < dx; xi++) {
          const int x = forward ? xi : dx - 1 - xi;
          uint16_t best = f.d[idx(x, y, z)];
          if (best == 0) continue;
          for (const auto& o : kPred) {
            const int s = forward ? 1 : -1;
            const int nz = z + s * o[0], ny = y + s * o[1], nx = x + s * o[2];
            if (nx < 0 || ny < 0 || nz < 0 || nx >= dx || ny >= dy || nz >= dz)
              continue;
            const uint16_t cand =
                (uint16_t)std::min<int>(kFar, f.d[idx(nx, ny, nz)] + weight(o));
            if (cand < best) best = cand;
          }
          f.d[idx(x, y, z)] = best;
        }
      }
    }
  };
  sweep(true);
  sweep(false);
  return f;
}

uint32_t SoakCut(const StainLattice& L, Vec3 centre, const CutSoak& p,
                 uint32_t seed, MicroBodySet* micro, int model) {
  if (p.mat == 0 || p.radius <= 0.0f) return 0;
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
    // 0 at the cut, 1 at the rim. Measured to the nearest cell the carve
    // REMOVED when the caller has that set (see CellDist), else to `centre`.
    float t;
    if (p.from && !p.from->Empty()) {
      const float dc = p.from->At(v.x, v.y, v.z);
      if (dc >= p.radius) continue;
      t = dc / p.radius;
    } else {
      const Vec3 d{(float)v.x + 0.5f - centre.x, (float)v.y + 0.5f - centre.y,
                   (float)v.z + 0.5f - centre.z};
      const float d2 = d.dot(d);
      if (d2 >= r2) continue;
      t = std::sqrt(d2 / r2);
    }
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
    const uint16_t cur = L.Stain(i);
    const uint16_t next = RaiseBodyStain(cur, p.mat, (uint32_t)amt);
    if (next == cur) continue;
    L.SetStain(i, next);
    changed++;
    if (poke) MicroBodyPokeStain(*micro, (uint32_t)model, v.x, v.y, v.z, next);
  }
  return changed;
}

uint32_t SoakBruise(const StainLattice& L, const BruiseSoak& p,
                    BruiseTally* out, MicroBodySet* micro, int model) {
  if (out) *out = BruiseTally{};
  if (p.bruiseMat == 0 || p.radius <= 0.0f || p.cap == 0 || p.step <= 0.0f)
    return 0;
  const size_t n = L.Size();
  if (n == 0) return 0;
  // The brick only takes pokes when it is this body's own (COW), exactly as
  // SoakCut above: a shared model is left alone and the caller re-skins.
  const bool poke = micro && model >= 0 &&
                    (size_t)model < micro->owned.size() &&
                    micro->owned[(size_t)model];
  static const std::vector<uint8_t> kNoTissue;
  const std::vector<uint8_t>& tissue = p.tissue ? *p.tissue : kNoTissue;
  const float r2 = p.radius * p.radius;
  // The inner half-radius, in the squared metric the sweep already works in.
  const float core2 = r2 * 0.25f;

  // ---- SKIN BREAKS AT THE SURFACE (2026-09-20) ----------------------------
  //
  // Rung 2 turns a saturated bruise into blood and rung 3 eats whatever is
  // bloody AT DEPTH. Applied to every cell in the radius, that makes the
  // INTERIOR of a beaten limb one solid reservoir of pulp: each voxel the
  // dissolution takes exposes more of it, the front never runs out, and a
  // beating eats the entire limb. Measured on both populations the same day -
  // the living gate `impact-blunt` reporting "1344 voxels -> 0 after 3 ticks
  // dissolving (100.0% gone, cap 60%)", and a corpse beaten with a mace
  // vanishing inside four seconds.
  //
  // A contusion breaks where there IS a surface to break. Gating the rung on
  // exposure bounds the reservoir to the skin the blow actually landed on,
  // which is both the physical statement and the thing that makes a mace cave
  // a body IN instead of deleting it. The occupancy map is the one SoakCut
  // already builds, for the same O(voxels) this loop pays anyway.
  IVec3 blo{INT32_MAX, INT32_MAX, INT32_MAX};
  IVec3 bhi{INT32_MIN, INT32_MIN, INT32_MIN};
  for (size_t i = 0; i < n; i++) {
    const IVec3 v = L.At(i);
    blo.x = std::min(blo.x, v.x); bhi.x = std::max(bhi.x, v.x);
    blo.y = std::min(blo.y, v.y); bhi.y = std::max(bhi.y, v.y);
    blo.z = std::min(blo.z, v.z); bhi.z = std::max(bhi.z, v.z);
  }
  const int bdx = bhi.x - blo.x + 1;
  const int bdy = bhi.y - blo.y + 1;
  const int bdz = bhi.z - blo.z + 1;
  const bool haveOcc = bdx > 0 && bdy > 0 && bdz > 0 &&
                       (uint64_t)bdx * bdy * bdz <= (1u << 22);
  std::vector<uint8_t> occ;
  if (haveOcc) {
    occ.assign((size_t)bdx * bdy * bdz, 0);
    for (size_t i = 0; i < n; i++) {
      if (L.Mat(i) == 0) continue;
      const IVec3 v = L.At(i);
      occ[(size_t)(v.x - blo.x) + (size_t)(v.y - blo.y) * bdx +
          (size_t)(v.z - blo.z) * bdx * bdy] = 1;
    }
  }
  auto occAt = [&](int x, int y, int z) -> bool {
    if (!haveOcc) return true;
    x -= blo.x; y -= blo.y; z -= blo.z;
    if (x < 0 || y < 0 || z < 0 || x >= bdx || y >= bdy || z >= bdz)
      return false;
    return occ[(size_t)x + (size_t)y * bdx + (size_t)z * bdx * bdy] != 0;
  };
  auto exposedAt = [&](IVec3 v) -> bool {
    return !occAt(v.x - 1, v.y, v.z) || !occAt(v.x + 1, v.y, v.z) ||
           !occAt(v.x, v.y - 1, v.z) || !occAt(v.x, v.y + 1, v.z) ||
           !occAt(v.x, v.y, v.z - 1) || !occAt(v.x, v.y, v.z + 1);
  };
  uint32_t marked = 0, coreCells = 0, pulpedCells = 0;
  for (size_t i = 0; i < n; i++) {
    const uint32_t mat = L.Mat(i);
    if (mat == 0) continue;   // tombstone
    // Bone does not bruise: a contusion is a burst capillary bed, and the
    // hole-shows-bone rule (MobDef::tissue) is the same one that governs here.
    if (!tissue.empty() && (mat >= tissue.size() || !tissue[mat])) continue;
    const IVec3 v = L.At(i);
    const Vec3 d{(float)v.x + 0.5f - p.centre.x, (float)v.y + 0.5f - p.centre.y,
                 (float)v.z + 0.5f - p.centre.z};
    const float d2 = d.dot(d);
    if (d2 >= r2) continue;
    const float t = std::sqrt(d2 / r2);
    const uint16_t curStain = L.Stain(i);
    const uint32_t h = rng::Hash3(p.seed, (uint32_t)(v.x * 73856093),
                                  (uint32_t)(v.y * 19349663) ^
                                      (uint32_t)(v.z * 83492791));
    // ---- THE READING FOR RUNG 3, taken BEFORE this blow changes anything ---
    //
    // OVER THE CORE'S SURFACE, not its volume. Rung 2 only breaks skin that is
    // exposed (see above), so pulp can only ever exist on the surface — and a
    // fraction whose denominator counts buried cells that are structurally
    // incapable of being pulped can never reach a threshold. Measured: with
    // the volume denominator the ripeness stalled under gore.pulpCarveFrom
    // forever and a beating stopped taking anything at all, which is the exact
    // opposite failure to the one the exposure gate fixed.
    if (d2 < core2 && exposedAt(v)) {
      coreCells++;
      if (p.bloodMat != 0 && BodyStainMat(curStain) == p.bloodMat &&
          BodyStainAmt(curStain) >= p.pulpAt)
        pulpedCells++;
    }
    // ---- THE FULL STEP AT THE CONTACT, AND A SPECTRUM OUT TO THE RIM -------
    //
    // ONE multiplier below 1, not three: the taper, plus a narrow jitter to
    // break the patch up, plus `blowScale` (the difference between a fist and
    // a mace, floored so it cannot vanish). Power is deliberately NOT here —
    // it already scales the radius at the call site, and charging it twice
    // made a glancing blow both smaller AND fainter.
    const float jitter = 0.85f + 0.15f * (float)((h >> 16) & 0xFFu) / 255.0f;
    const float taper = 1.0f - t * t;
    const uint32_t add = (uint32_t)std::lround(p.step * taper * jitter *
                                               p.blowScale);
    if (add == 0) continue;
    // ---- AND THE RIM HAS ITS OWN CEILING ----------------------------------
    //
    // The taper is a CEILING as well as a rate: a cell at the rim cannot be
    // driven past a light mark by this blow however many land, and only the
    // middle can reach the depth that breaks. Without it, enough blows on one
    // spot crawl every cell in the radius to the global ceiling and the whole
    // mark goes wet — a patch of blood with a hard edge and no bruise around
    // it, which is not what a beating looks like. PER BLOW, not per voxel
    // forever: a second blow landing closer legitimately raises this cell's
    // ceiling, which is how a beating walks across a limb.
    const uint32_t voxCap = (uint32_t)std::lround((float)p.cap * taper);
    if (voxCap == 0) continue;
    const uint32_t curAmt = BodyStainAmt(curStain);
    const uint32_t curMat = BodyStainMat(curStain);
    uint16_t next = curStain;
    // ---- RUNG 2: where it has already gone as dark as a bruise gets -------
    bool broke = false;
    if (p.bloodMat != 0 && p.bleedChance > 0.0f && curMat == p.bruiseMat &&
        curAmt >= p.bleedFrom && exposedAt(v)) {
      const float roll = (float)(h & 0xFFFFu) / 65535.0f;
      if (roll < p.bleedChance) {
        // Blood goes on at the bruise's own depth, not at a cut's: a deep
        // contusion has broken the skin, and it should read as continuous with
        // the mark around it rather than as a splash. A DELIBERATE OVERWRITE
        // rather than a raise — Raise's cross-material rule ("only a strictly
        // larger amount repaints") can never fire here by construction, which
        // is how this rung once shipped disabled.
        next = PackBodyStain(
            p.bloodMat, std::min(std::max(curAmt, add), (uint32_t)kBodyStainAmtMax));
        broke = true;
      }
    }
    if (!broke) next = AddBodyStain(curStain, p.bruiseMat, add, voxCap);
    if (next == curStain) continue;
    L.SetStain(i, next);
    if (poke)
      MicroBodyPokeStain(*micro, (uint32_t)model, v.x, v.y, v.z, next);
    marked++;
  }
  if (out) {
    out->marked = marked;
    out->core = coreCells;
    out->pulped = pulpedCells;
  }
  return marked;
}
