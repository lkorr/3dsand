#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "phys/physics.h"  // DebrisVoxel
#include "sim/voxload.h"   // PrefabVoxel

// WHAT A BODY FLOATS AS (2026-10-14). ONE rule for everything Jolt floats:
// a live limp rig and a corpse (Mob::FloatLimpLimbs), a severed limb, a corpse
// released to debris, and plain debris (DebrisSystem::FloatBodies).
//
// Owner, 2026-10-14: "corpses will float but their limbs will slowly sink. we
// need consistency in how corpses and limbs float under the same rules." They
// did not: the rig used the volume-weighted MEAN density of a limb's matter
// (worn shells folded in) less ragdoll.corpseAirFraction, while debris used
// the density of the body's single most COMMON material and no air. A human
// arm's plurality is muscle (1060) or skin (1010), both heavier than water, so
// the arm cut off a floating corpse sank -- the same matter, two answers.
//
// The density is Archimedes' own: total mass / total volume, each voxel
// weighted by the volume of a cell on ITS lattice (1/scale^3 of a world
// voxel), so a scale-4 breastplate strapped to a scale-8 forearm counts for the
// space it fills. Callers accumulate the body and everything strapped to it
// into one FloatRho, then ask Rho().
struct FloatRho {
  double mass = 0, vol = 0;

  void Add(const std::vector<DebrisVoxel>& voxels, uint32_t scale,
           const std::vector<float>& rhoOf) {
    const double cell = CellVolume(scale);
    for (const DebrisVoxel& v : voxels) AddCell(v.payload & 0xFFFu, cell, rhoOf);
  }
  void Add(const std::vector<PrefabVoxel>& voxels, uint32_t scale,
           const std::vector<float>& rhoOf) {
    const double cell = CellVolume(scale);
    for (const PrefabVoxel& v : voxels) AddCell(v.material & 0xFFFu, cell, rhoOf);
  }
  // kg/m^3; water's 1000 when there was nothing to weigh.
  float Rho() const { return vol > 0 ? (float)(mass / vol) : 1000.0f; }

 private:
  static double CellVolume(uint32_t scale) {
    const double s = (double)std::max(1u, scale);
    return 1.0 / (s * s * s);
  }
  void AddCell(uint32_t mat, double cell, const std::vector<float>& rhoOf) {
    if (mat == 0 || mat >= rhoOf.size()) return;
    mass += rhoOf[mat] * cell;
    vol += cell;
  }
};

// The density a body FLOATS at: its matter, less the air a thing that was
// alive carries in it (ragdoll.corpseAirFraction -- lungs and gut). Matter
// that never lived (a log, a sword, a rock) has no air to lose. Clamped off
// zero because the caller divides the fluid's density by it.
inline float FloatingDensity(float matterRho, bool wasAlive, float airFraction) {
  return std::max(1.0f, matterRho * (wasAlive ? 1.0f - airFraction : 1.0f));
}
