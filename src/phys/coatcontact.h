#pragma once
#include <cstdint>
#include <vector>

#include "math3d.h"
#include "phys/bodystain.h"

struct MicroBodySet;

// ---- A COAT MOVES ON CONTACT (DESIGN.md §7, "A coat moves on contact") ------
//
// The lattice half of the rule, beside the cut's soak and the bruise ladder
// (phys/bodystain.h) because it is the same kind of thing: per-voxel coat
// arithmetic over ONE StainLattice, with every write going through the
// precedence rule (sim/coatrule.h stainPrecedence via the bodystain writers).
// Who struck whom, and which slot of which rig is the lattice, is the
// caller's (MobSystem::CoatOnContact); nothing here knows about creatures,
// weapons or materials by name.
//
// A contact moves coat in four steps, each a function below:
//
//   1. WHICH VOXELS TOUCHED -- CoatContactCells (a surface patch round a
//      point) or CoatWoundWall (the walls of a carve). Sorted nearest-first,
//      ties by lattice index, so every step after is a pure function of the
//      lattice and the point (rule 1's discipline, off the grid).
//   2. WHAT IS OFFERED -- CoatOffer: per material on those voxels, a share of
//      what they carry, capped per contact. Reads only.
//   3. WHAT LANDS -- CoatLay: the offer laid on the other side's touched
//      voxels, more on the nearest, through AddBodyStain (or WashBodyStain for
//      a washer) so precedence decides every voxel. Each parcel's `levels` is
//      rewritten to what was ACTUALLY written.
//   4. WHAT IS SPENT -- CoatSpend: exactly those levels taken back off the
//      giving side. Amounts are conserved: a coat that could not land (a
//      corrosive coat in the way, nowhere to put it) costs the giver nothing.
//
// Distances are in LATTICE CELLS of the lattice being walked.

struct CoatCell {
  uint32_t idx = 0;   // into the StainLattice
  float dist = 0.0f;  // lattice cells from the contact point
};

// Occupancy over a lattice's own bounding box, built once per contact. The
// same bitmap SoakCut and SoakBruise build for "is this voxel exposed".
struct LatticeOcc {
  IVec3 lo{}, dim{};
  std::vector<uint8_t> occ;
  bool Valid() const { return !occ.empty(); }
  bool At(int x, int y, int z) const;
  // Has at least one empty face neighbour in this lattice.
  bool Exposed(const IVec3& v) const {
    return !At(v.x - 1, v.y, v.z) || !At(v.x + 1, v.y, v.z) ||
           !At(v.x, v.y - 1, v.z) || !At(v.x, v.y + 1, v.z) ||
           !At(v.x, v.y, v.z - 1) || !At(v.x, v.y, v.z + 1);
  }
};
LatticeOcc BuildLatticeOcc(const StainLattice& L);

// The SURFACE patch a contact at `p` (continuous lattice coords; a cell's
// centre is v + 0.5) touches: every exposed voxel no further than `reach`
// cells beyond the nearest exposed voxel's own distance. Measured from the
// nearest surface rather than from `p` itself because a probe reports its
// contact a little inside or outside the matter, and the patch is where the
// two surfaces met. `splitDepth`: an exposed voxel whose skin has SPLIT
// (BruiseBroken) also lets the patch one cell in, through it -- the blunt
// rule's "deeper only where the skin split". Empty for an empty lattice.
std::vector<CoatCell> CoatContactCells(const StainLattice& L,
                                       const LatticeOcc& occ, Vec3 p,
                                       float reach, bool splitDepth);

// THE WOUND WALL: every surviving voxel face-adjacent to a `removed` cell
// (CarveReport::cells, the authoritative lattice's coords), with its distance
// to `p`. What a blade drags its coat across on the way in.
std::vector<CoatCell> CoatWoundWall(const StainLattice& L,
                                    const std::vector<IVec3>& removed, Vec3 p);

// One material's share of a contact.
struct CoatParcel {
  uint32_t mat = 0;
  uint32_t levels = 0;  // coat levels (0..15 scale), summed over voxels
  uint32_t peak = 0;    // the thickest voxel's amount: how deep it lays
};
// What `cells` of `L` offer: per coat material on them, `frac` of the levels
// they carry (at least one level when there is any and frac > 0), the whole
// contact capped at `maxLevels`. Heaviest material first, ties by id. Reads
// only.
std::vector<CoatParcel> CoatOffer(const StainLattice& L,
                                  const std::vector<CoatCell>& cells,
                                  float frac, uint32_t maxLevels);

// Lay `parcels` on `cells` of `L`, nearest first: a cell takes up to the
// parcel's peak at the contact, tapering to half of it at `reach`, and the
// precedence rule decides whether it lands. Each parcel's `levels` becomes
// what was written. `micro`/`model`: poke the brick when it is OWNED (SoakCut's
// contract). Returns the voxels whose coat changed.
// `layerMin` (gear.coatLayerMin) SPREADS a parcel: no cell takes more than
// max(layerMin, an even share of the parcel over the patch), so a dose covers
// many cells at a working thickness instead of soaking the first two to 15
// (2026-10-01: a venom cut put 24 levels on 2 of a wound's 38 wall cells).
// 0 = the old peak taper only.
uint32_t CoatLay(const StainLattice& L, const std::vector<CoatCell>& cells,
                 float reach, std::vector<CoatParcel>& parcels,
                 MicroBodySet* micro, int model, uint32_t layerMin = 0);

// Take exactly each parcel's `levels` of its material back off `cells`,
// nearest first. Returns the levels actually removed.
uint32_t CoatSpend(const StainLattice& L, const std::vector<CoatCell>& cells,
                   const std::vector<CoatParcel>& parcels, MicroBodySet* micro,
                   int model);

// A SMEAR that is not a transfer: `amt` of `mat` (a wound's own fluid) laid
// on `cells` by stainPrecedence's UNPAID branch -- it fills a clean voxel or
// its own coat, and displaces a foreign one only when strictly heavier. That
// is the difference between being bled on and being dipped: a blade's thick
// coat survives the first dirty hit and wears off under the next few.
// Returns the voxels whose coat changed.
uint32_t CoatSmear(const StainLattice& L, const std::vector<CoatCell>& cells,
                   uint32_t mat, uint32_t amt, MicroBodySet* micro, int model);
