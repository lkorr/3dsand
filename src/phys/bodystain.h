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
// A body stain is the byte on PrefabVoxel::stain / DebrisVoxel::stain (amount
// 0..15, type = the world's stain palette slot), mirrored into the micro
// brick's stain lattice (sim/microbody.h) for rendering. It is PRESENTATION
// AND GAMEPLAY STATE that never touches the hashed grid (rule 1): it decides
// what a creature looks like, travels with its voxels into every fragment,
// and is never saved.

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
  uint8_t Stain(size_t i) const {
    return skin ? (*skin)[i].stain : (*coll)[i].stain;
  }
  void SetStain(size_t i, uint8_t s) const {
    if (skin) (*skin)[i].stain = s; else (*coll)[i].stain = s;
  }
};

// Raise a voxel's stain toward `amt` of `type`: same type (or clean) keeps the
// larger amount; a different type is overwritten only by a stronger one, so a
// splash of blood over a wet patch wins and a splash of water over blood does
// not repaint it (washing is a separate, subtractive rule). Returns the byte.
uint8_t RaiseBodyStain(uint8_t cur, uint32_t type, uint32_t amt);

// The soak round a cut. `centre` and `radius` are in the LATTICE's own units.
//
// Every voxel in range takes a stain, bone included: an EXPOSED voxel (one
// with an empty 6-neighbour in this lattice, i.e. the walls of the hole and
// the skin round its mouth) takes `amountExposed` at the centre tapering to
// the rim with a per-voxel jitter; a buried one takes `amountBuried` with
// `buriedChance`. `boneMin` floors the exposed amount on any voxel that is
// NOT tissue (`tissue[mat]` false; an empty table means everything is
// tissue), so bone is always shown bloodied to some degree. Amounts are the
// 0..15 world scale, `type` the stain palette slot.
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
  uint32_t type = 0;
  float radius = 0.0f;
  int amountExposed = 15;
  int amountBuried = 6;
  float buriedChance = 0.35f;
  int boneMin = 5;
  const std::vector<uint8_t>* tissue = nullptr;
};
uint32_t SoakCut(const StainLattice& L, Vec3 centre, const CutSoak& p,
                 uint32_t seed, MicroBodySet* micro, int model);
