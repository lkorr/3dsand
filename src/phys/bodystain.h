#pragma once
#include <cstdint>
#include <vector>

#include "math3d.h"
#include "phys/physics.h"
#include "sim/voxload.h"

struct MicroBodySet;

// ---- BLOOD ON A BODY (DESIGN.md section 7) ----------------------------------
//
// The one place that knows how to smear a stain over a body's lattice, shared
// by the live-limb cut (Mob::StainWound), the blast crater
// (Mob::CarveLimbRadial) and the corpse cut (DebrisSystem::DamageBody), so a
// limb cut off and cut again is bloodied by the same rule both times.
//
// A body stain is the 16-bit COAT word on PrefabVoxel::stain /
// DebrisVoxel::stain (the MATERIAL on the voxel in bits 0..11, amount 0..15 in
// bits 12..15), mirrored into the micro brick's stain lattice
// (sim/microbody.h) for rendering -- which is the only consumer that narrows
// it back to the world's 3-bit palette slot. It is PRESENTATION AND GAMEPLAY
// STATE that never touches the hashed grid (rule 1): it decides what a
// creature looks like, travels with its voxels into every fragment, and IS
// SAVED -- both body savers write the lattices as PODs, so the coat rides
// along (this comment claimed the opposite until 2026-09-13, and the code
// never agreed with it).

// Whichever of a body's two lattices is authoritative: the skin when it is a
// separate, finer lattice, else the collider. Same rule BurnLimbView applies.
struct StainLattice {
  std::vector<PrefabVoxel>* skin = nullptr;
  std::vector<DebrisVoxel>* coll = nullptr;
  size_t Size() const { return skin ? skin->size() : coll->size(); }
  IVec3 At(size_t i) const {
    return skin ? IVec3{(*skin)[i].x, (*skin)[i].y, (*skin)[i].z}
                : IVec3{(*coll)[i].x, (*coll)[i].y, (*coll)[i].z};
  }
  uint32_t Mat(size_t i) const {
    return skin ? (uint32_t)((*skin)[i].material & 0xFFFu)
                : (uint32_t)((*coll)[i].payload & 0xFFFu);
  }
  uint16_t Stain(size_t i) const {
    return skin ? (*skin)[i].stain : (*coll)[i].stain;
  }
  void SetStain(size_t i, uint16_t s) const {
    if (skin) (*skin)[i].stain = s; else (*coll)[i].stain = s;
  }
};

// Raise a voxel's coat toward `amt` of `mat`: the same material (or a clean
// voxel) keeps the larger amount; a DIFFERENT material overwrites only with a
// strictly larger amount, so a splash of blood over a wet patch wins and a
// splash of water over blood does not repaint it (washing is a separate,
// subtractive rule). Returns the coat word.
uint16_t RaiseBodyStain(uint16_t cur, uint32_t mat, uint32_t amt);

// ---- DISTANCE TO WHAT THE CARVE ACTUALLY TOOK -------------------------------
//
// A BALL ROUND A CENTROID IS NOT A CRATER. The blast crater's predicate removes
// with a chance that falls off to zero at the rim, so a GRAZE takes a scatter
// of voxels across the whole sphere rather than a clean bite out of one side.
// Its centroid is then somewhere in the middle of the limb and its RMS spread
// is most of the blast radius, so "soak a ball of that size" bloodies a quarter
// of the limb for eight lost voxels -- measured, `blast-stain`, 305 of 1344.
//
// What the owner asked for instead is blood "concentrated at areas where actual
// voxels are removed", and that is a distance to a SET, not to a point. This is
// that distance: a 3-4-5 chamfer over a box round the removed cells, two sweeps,
// O(cells), accurate to a few percent of true Euclidean at the two or three
// cells a wound rim actually uses. Distances are in LATTICE CELLS.
struct CellDist {
  IVec3 lo{}, dim{};
  std::vector<uint16_t> d;  // chamfer units, 3 per face step
  bool Empty() const { return d.empty(); }
  float At(int x, int y, int z) const {
    x -= lo.x;
    y -= lo.y;
    z -= lo.z;
    if (x < 0 || y < 0 || z < 0 || x >= dim.x || y >= dim.y || z >= dim.z)
      return 1e9f;
    return (float)d[((size_t)z * dim.y + y) * dim.x + x] * (1.0f / 3.0f);
  }
};
// `pad` cells of margin round the seeds' own bounds -- the field only has to
// cover the rim the caller means to stain, so it does not pay for the limb.
// Empty (and Empty() true) for no seeds or an absurd box.
CellDist BuildCellDist(const std::vector<IVec3>& seeds, int pad);

// The soak round a cut. `centre` and `radius` are in the LATTICE's own units.
//
// Every voxel in range takes a stain, bone included: an EXPOSED voxel (one
// with an empty 6-neighbour in this lattice, i.e. the walls of the hole and
// the skin round its mouth) takes `amountExposed` at the centre tapering to
// the rim with a per-voxel jitter; a buried one takes `amountBuried` with
// `buriedChance`. `boneMin` floors the exposed amount on any voxel that is
// NOT tissue (`tissue[mat]` false; an empty table means everything is
// tissue), so bone is always shown bloodied to some degree. Amounts are the
// 0..15 world scale, `mat` the MATERIAL doing the staining (this body's blood).
//
// `micro`/`model`: when the model is OWNED, every stained voxel is also poked
// into the brick's stain lattice, brick-local coordinates being the lattice's
// (the caller guarantees the brick was last rebased to the same frame -- a
// re-skin after the carve, before this). Pass model -1 to write the lattice
// only, e.g. when a re-skin is about to rewrite the whole brick anyway.
//
// Keyed on `seed` and the lattice position, so a replay stains the same
// voxels. Returns the number of voxels whose stain changed.
struct CutSoak {
  uint32_t mat = 0;  // the staining material (this body's blood), 0 = no soak
  float radius = 0.0f;
  int amountExposed = 15;
  int amountBuried = 6;
  float buriedChance = 0.35f;
  int boneMin = 5;
  const std::vector<uint8_t>* tissue = nullptr;
  // When set, the taper is measured from the nearest cell of `from` instead of
  // from `centre`, and `radius` is how far past it the smear reaches. See the
  // note on CellDist: a crater is a set, not a point.
  const CellDist* from = nullptr;
};
uint32_t SoakCut(const StainLattice& L, Vec3 centre, const CutSoak& p,
                 uint32_t seed, MicroBodySet* micro, int model);
