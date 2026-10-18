#pragma once
// THE MIRROR FIRST, THEN ONE CHUNK MEMO over the on-demand fetch cache: what a
// body in the water asks of the world (game/mob_swim.cpp, game/mob_afloat.cpp).
//
// The 3x3x3 mirror round the player is refreshed every tick, so near the
// player (where a body is being watched) the answer is the water as it is NOW
// -- a pool dug this second, a lake drained. Past the mirror, the fetch cache
// the walk drive's ground probe reads (Mob::GroundHeightAt): a body forty
// voxels from the camera must still know where the water is. A cache miss
// asks for the chunk and answers Unknown; outside the residency window is
// Solid, the sim's own rule -- UNLESS `outside` is set (D11, below).
//
// PAST THE WINDOW FACE, for a body that may leave the window (MobSystem::
// OutsideGroundFor; docs/PLAN_distant_npcs.md section 8.1), the cell is the
// DISTANT tier's: Solid where DistantGround carries weight, Liquid below its
// water surface (WaterTop: the first cell above the liquid), Air above. The
// same water a distant swimmer swims in, so a fish or a swimmer crosses the
// face into the water it will swim in as a distant creature instead of
// meeting a wall of "solid" at the face. Asked lazily (the first outside
// cell), so a body that never nears a face never asks.
//
// (mob_swim.cpp used to carry its own copy of this struct in its anonymous
// namespace; since D11 it uses this one, so the two swim readers cannot
// disagree about the face.)

#include <climits>
#include <cstdint>
#include <vector>

#include "game/distant.h"   // DistantGround: the ground past the face (D11)
#include "math3d.h"
#include "sim/world.h"

class Mob;
class MobSystem;

namespace mobwater {

struct CellCache {
  World& world;
  const std::vector<uint32_t>& cls;
  // Ask for a missing chunk? A body's own sense does: it needs the water. A
  // WATCHER's question about somebody else does not -- it answers from what is
  // already known, and must not put a single request into the one FIFO every
  // ground probe in the world waits on (World::FetchSource).
  bool fetch = true;
  // D11: the body whose question this is, and its system -- set together, or
  // past the face stays Solid. `outside` is resolved on the first cell past
  // the face (MobSystem::OutsideGroundFor; null = the old answer).
  const Mob* body = nullptr;
  MobSystem* sys = nullptr;
  DistantGround* outside = nullptr;
  bool outsideAsked = false;
  const CachedChunk* cc = nullptr;
  int kx = INT_MIN, ky = INT_MIN, kz = INT_MIN;
  // Out of line (mob_swim.cpp): it needs MobSystem whole.
  DistantGround* Outside();
  CellKind At(IVec3 c) {
    if (!world.CellInWindow(c)) {
      DistantGround* g = Outside();
      if (g == nullptr) return CellKind::Solid;
      if (g->Solid(c.x, c.y, c.z)) return CellKind::Solid;
      const int wt = g->WaterTop(c.x, c.z);
      return wt != DistantGround::kNoWater && c.y < wt ? CellKind::Liquid : CellKind::Air;
    }
    if (const CellKind m = world.KindAt(c, cls); m != CellKind::Unknown) return m;
    const int x = c.x >> 4, y = c.y >> 4, z = c.z >> 4;
    if (x != kx || y != ky || z != kz) {
      kx = x;
      ky = y;
      kz = z;
      cc = world.Cached({x, y, z});
      if (cc != nullptr && cc->voxels.size() != kChunkVol) cc = nullptr;
      if (cc == nullptr && fetch)
        world.RequestChunkFetch({x, y, z}, World::FetchSource::Mob);
    }
    if (cc == nullptr) return CellKind::Unknown;
    const uint32_t li =
        ((uint32_t)(c.z & 15) * kChunk + (uint32_t)(c.y & 15)) * kChunk +
        (uint32_t)(c.x & 15);
    return World::KindOfWord(cc->voxels[li], cls);
  }
  CellKind At(Vec3 p) { return At(IVec3{ifloor(p.x), ifloor(p.y), ifloor(p.z)}); }
};

}  // namespace mobwater

// (The cache a body's question uses -- its classes, and past the face the
// distant tier's ground when it may swim out -- is MobSystem::WaterCache.)
