#pragma once
// wornfit.h -- A WORN PIECE'S DAMAGE, MOVED ONTO A BODY IT WAS NOT FITTED TO
// (ticket 2026-10-14_22-29-47: Zeus's jerkin, looted off his corpse slightly
// damaged and put on by the player, came up as a box half again too big,
// standing off the player's chest in torn patches).
//
// THE BUG. A shell is fitted to its wearer by a per-axis resample of the
// authored lattice (Mob::AppendWornShell, ResampleLattice in game/item.h): the
// jerkin's torso panel is authored for the stock human's 20 x 36 x 12 micro
// torso and is resampled to Zeus's 32 x 46 x 16. A damaged piece carries its
// EXACT lattice (WornShellDamage::lattice, "the holes round-trip") -- and that
// lattice is in the frame of the fit it was recorded on. Mob::WearOnRig put
// it back whole on whoever wore the piece next, so on a smaller body the
// player's shell slot was rebuilt from Zeus's fitted lattice: Zeus's size, at
// the player's corner.
//
// THE FIX. The holes are kept exact on the body they were made on (the common
// case: your own clothes off and on again), and MOVED through the authored
// lattice onto any other fit:
//   - LatticeFitsShell: is the record in this wearer's fit? Every recorded
//     cell is a cell of the fresh shell (damage only ever takes cells away)
//     and the record's own spawn count is the fresh shell's.
//   - otherwise RefitShellLattice: both fits are recovered (InferFitDims --
//     the resample's target box, which the record does not carry; it is
//     found as the box whose resample of the authored lattice holds every
//     recorded cell AND has the recorded spawn count), the record's damage is
//     tallied per AUTHORED cell (an authored cell is lost when most of the
//     cells it became on the old body are gone; its survivors lend their
//     material, art colour and coat), and the fresh shell keeps exactly the
//     cells whose authored cell survived, wearing what it wore.
// So the piece is always THIS body's fit, and the burn, the cut and the blood
// are where they were on the garment, at the authored lattice's resolution
// (1/80 m) -- no finer than the two fits share.
//
// Pure functions over integer lattices (no rig, no physics): the `armor-fit`
// gate asserts them directly.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "game/item.h"           // ItemCover, ResampleLattice

namespace wornfit {

// One past the largest coordinate on each axis (0 for an empty lattice).
inline IVec3 LatticeExtent(const std::vector<PrefabVoxel>& lat) {
  IVec3 hi{0, 0, 0};
  for (const PrefabVoxel& v : lat) {
    hi.x = std::max(hi.x, (int)v.x + 1);
    hi.y = std::max(hi.y, (int)v.y + 1);
    hi.z = std::max(hi.z, (int)v.z + 1);
  }
  return hi;
}

// A dense box of lattice indices (-1 = no cell), for random lookups.
struct LatticeBox {
  IVec3 dims{0, 0, 0};
  std::vector<int32_t> at;
  void Build(const std::vector<PrefabVoxel>& lat, IVec3 d) {
    dims = d;
    at.assign((size_t)std::max(0, d.x) * std::max(0, d.y) * std::max(0, d.z), -1);
    for (size_t i = 0; i < lat.size(); i++) {
      const PrefabVoxel& v = lat[i];
      if (v.x < 0 || v.y < 0 || v.z < 0 || v.x >= d.x || v.y >= d.y || v.z >= d.z)
        continue;
      at[((size_t)v.z * d.y + v.y) * d.x + v.x] = (int32_t)i;
    }
  }
  int32_t Find(int x, int y, int z) const {
    if (x < 0 || y < 0 || z < 0 || x >= dims.x || y >= dims.y || z >= dims.z)
      return -1;
    return at[((size_t)z * dims.y + y) * dims.x + x];
  }
};

// Is `rec` (a WornShellDamage::lattice, `recAtSpawn` its spawn count, 0 =
// never measured) in the same fit as `fresh`, the shell this wearer was just
// given? Then it goes back whole -- the exact path.
inline bool LatticeFitsShell(const std::vector<PrefabVoxel>& rec,
                             uint32_t recAtSpawn,
                             const std::vector<PrefabVoxel>& fresh) {
  if (recAtSpawn != 0 && recAtSpawn != (uint32_t)fresh.size()) return false;
  LatticeBox box;
  box.Build(fresh, LatticeExtent(fresh));
  for (const PrefabVoxel& v : rec)
    if (box.Find(v.x, v.y, v.z) < 0) return false;
  return true;
}

// The resample target box `lat` was fitted at: the box D whose
// ResampleLattice(cover.voxels, cover.size, D) holds every cell of `lat` and
// has `count` cells (0 = unknown: the smallest that holds them). Searched from
// the lattice's own extent up three cells per axis -- damage that took a whole
// outer layer away is why the extent alone is not the answer. Falls back to
// the extent when nothing matches (a record from a different authored cover).
inline IVec3 InferFitDims(const ItemCover& cover,
                          const std::vector<PrefabVoxel>& lat, uint32_t count) {
  const IVec3 src = cover.size;
  const IVec3 hi = LatticeExtent(lat);
  if (lat.empty() || src.x <= 0 || src.y <= 0 || src.z <= 0) return src;
  constexpr int kSlack = 3;
  IVec3 best = hi;
  int64_t bestDiff = -1;
  for (int dz = 0; dz <= kSlack; dz++)
    for (int dy = 0; dy <= kSlack; dy++)
      for (int dx = 0; dx <= kSlack; dx++) {
        const IVec3 d{hi.x + dx, hi.y + dy, hi.z + dz};
        const std::vector<PrefabVoxel> ref = ResampleLattice(cover.voxels, src, d);
        LatticeBox box;
        box.Build(ref, d);
        bool holds = true;
        for (const PrefabVoxel& v : lat)
          if (box.Find(v.x, v.y, v.z) < 0) {
            holds = false;
            break;
          }
        if (!holds) continue;
        const int64_t diff =
            count ? std::llabs((int64_t)ref.size() - (int64_t)count) : (int64_t)ref.size();
        if (diff == 0 && count) return d;
        if (bestDiff < 0 || diff < bestDiff) {
          bestDiff = diff;
          best = d;
        }
      }
  return best;
}

// The fresh shell (`fresh`, this wearer's fit of `cover`, undamaged) carrying
// the damage of `rec` (recorded on another fit; `recAtSpawn` its spawn count).
// See the header note for the rule.
inline std::vector<PrefabVoxel> RefitShellLattice(
    const ItemCover& cover, const std::vector<PrefabVoxel>& rec,
    uint32_t recAtSpawn, const std::vector<PrefabVoxel>& fresh) {
  const IVec3 src = cover.size;
  if (src.x <= 0 || src.y <= 0 || src.z <= 0 || fresh.empty()) return fresh;
  const IVec3 ds = InferFitDims(cover, rec, recAtSpawn);
  const IVec3 dd = InferFitDims(cover, fresh, (uint32_t)fresh.size());
  // The authored cell a fitted cell came from: ResampleLattice's own mapping.
  auto authoredOf = [&](const PrefabVoxel& v, IVec3 d) -> int64_t {
    const int ax = (int)(((int64_t)v.x * src.x) / std::max(1, d.x));
    const int ay = (int)(((int64_t)v.y * src.y) / std::max(1, d.y));
    const int az = (int)(((int64_t)v.z * src.z) / std::max(1, d.z));
    if (ax < 0 || ay < 0 || az < 0 || ax >= src.x || ay >= src.y || az >= src.z)
      return -1;
    return ((int64_t)az * src.y + ay) * src.x + ax;
  };
  const size_t n = (size_t)src.x * src.y * src.z;
  // How many cells each authored cell became on the old body, how many of
  // them are still there, and one of the survivors.
  std::vector<uint16_t> had(n, 0), kept(n, 0);
  std::vector<int32_t> rep(n, -1);
  for (const PrefabVoxel& v : ResampleLattice(cover.voxels, src, ds)) {
    const int64_t a = authoredOf(v, ds);
    if (a >= 0 && had[(size_t)a] < 0xFFFFu) had[(size_t)a]++;
  }
  for (size_t i = 0; i < rec.size(); i++) {
    const int64_t a = authoredOf(rec[i], ds);
    if (a < 0) continue;
    if (kept[(size_t)a] < 0xFFFFu) kept[(size_t)a]++;
    if (rep[(size_t)a] < 0) rep[(size_t)a] = (int32_t)i;
  }
  std::vector<PrefabVoxel> out;
  out.reserve(fresh.size());
  for (const PrefabVoxel& f : fresh) {
    const int64_t a = authoredOf(f, dd);
    if (a < 0 || had[(size_t)a] == 0) {
      out.push_back(f);   // the old fit never had this cell: nothing to say
      continue;
    }
    if (2u * kept[(size_t)a] < had[(size_t)a] || rep[(size_t)a] < 0) continue;
    const PrefabVoxel& r = rec[(size_t)rep[(size_t)a]];
    PrefabVoxel v = f;
    // Its matter (a scorch is a material) and its art colour from the worn
    // garment; the palette jitter (bits 12..15) is this cell's own position.
    v.material = (uint16_t)((r.material & 0x0FFFu) | (f.material & 0xF000u));
    v.color = r.color;
    v.stain = r.stain;
    out.push_back(v);
  }
  return out;
}

}  // namespace wornfit
