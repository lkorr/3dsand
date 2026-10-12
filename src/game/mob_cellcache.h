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
// Solid, the sim's own rule.
//
// (mob_swim.cpp still carries its own copy in its anonymous namespace; it was
// claimed by a parallel session when this header was split out. Switch it to
// this one when that file is next free.)

#include <climits>
#include <cstdint>
#include <vector>

#include "sim/world.h"

namespace mobwater {

struct CellCache {
  World& world;
  const std::vector<uint32_t>& cls;
  // Ask for a missing chunk? A body's own sense does: it needs the water. A
  // WATCHER's question about somebody else does not -- it answers from what is
  // already known, and must not put a single request into the one FIFO every
  // ground probe in the world waits on (World::FetchSource).
  bool fetch = true;
  const CachedChunk* cc = nullptr;
  int kx = INT_MIN, ky = INT_MIN, kz = INT_MIN;
  CellKind At(IVec3 c) {
    if (!world.CellInWindow(c)) return CellKind::Solid;
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
};

}  // namespace mobwater
