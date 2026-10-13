#pragma once
// flightprobe.h -- WHAT A CPU FLIGHT CAN HIT IN THE GRID (2026-10-12).
//
// One question every CPU projectile asks once per half-voxel sub-step: is
// this cell solid? It used to be a lambda inside SpellSystem::Tick (spell
// bolts); arrows (game/arrows.h) ask exactly the same thing, so it lives here
// and both call it (docs/PLAN_projectiles.md C: "factor the spell's terrain
// probe into a shared helper rather than copying it").
//
// THREE TIERS OF KNOWLEDGE. The CPU mirror is only the 3x3x3 chunks around the
// PLAYER (~48 voxels); everything a flight does happens past it. Reading
// Unknown as solid detonated every bolt in the caster's face; reading it as
// passable meant a bolt never hit anything at all once it left the mirror.
//   1. the mirror, when the cell is in it (one tick latent, the truth);
//   2. the on-demand chunk cache (World::Cached), which a flight keeps warm
//      by asking for the chunks ahead of it (World::RequestChunkFetch);
//   3. the ground contract, World::TerrainHeight == genColumn(x,z).h exactly,
//      for a cell nobody has fetched yet -- so at worst a flight lands ON THE
//      GROUND, never under it, and never through it.
// Out-of-window space is solid (DESIGN.md section 3). Solid and powder both
// stop a flight, exactly as World::KindAt says.

#include <cstdint>
#include <vector>

#include "sim/world.h"

inline bool FlightSolidAt(World& world, const std::vector<uint32_t>& classOf,
                          uint32_t seed, int32_t x, int32_t y, int32_t z) {
  if (!world.CellInWindow({(int)x, (int)y, (int)z})) return true;
  const CellKind k = world.KindAt({(int)x, (int)y, (int)z}, classOf);
  if (k != CellKind::Unknown) return k == CellKind::Solid;
  const IVec3 wc{x >> 4, y >> 4, z >> 4};
  if (const CachedChunk* cc = world.Cached(wc);
      cc && cc->voxels.size() == kChunkVol) {
    const uint32_t mat =
        cc->voxels[(size_t)(((z & 15) * (int)kChunk + (y & 15)) * (int)kChunk +
                            (x & 15))] &
        0xFFFu;
    if (mat == 0 || mat >= classOf.size()) return false;
    return classOf[mat] == 0u || classOf[mat] == 1u;
  }
  world.RequestChunkFetch(wc);
  return y <= World::TerrainHeight(x, z, seed);
}
