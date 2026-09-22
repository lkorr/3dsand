#pragma once
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "phys/physics.h"
#include "sim/rng.h"
#include "sim/voxload.h"

// The bridge between a body's two voxel lattices (DESIGN.md §9).
//
// A micro body may carry a SKIN finer than its COLLIDER: the skin is int16
// PrefabVoxel at `skinScale` units per world voxel, the collider is int8
// DebrisVoxel at `physScale`. They are decoupled because their costs are
// unrelated — the brick march is a fragment shader over one OBB, so skin cost
// tracks screen area, while the collider is Jolt boxes and tracks voxel count.
//
// This header holds the ONE function that relates them, so that the relation
// can be tested without a GPU (tests/lattice_test.cpp). Data flows skin ->
// collider and never back: the skin is render state, and a collider that read
// from it in the other direction would pull render state into physics.

// Majority-fill a fine skin lattice down to a coarse collider lattice, `ratio`
// skin voxels per collider voxel along each axis. A block survives when at
// least half of it is solid and takes its plurality material — the same rule
// DownsampleMicro applies for settle-back, so a carved shape reads the same way
// to physics as it does to the eye.
//
// The plurality is over the (material, ART COLOUR) PAIR, not over the material
// alone, so the coarse lattice keeps the paint. It has to: the collider lattice
// is what settle-back and the particle conversion read when a body's matter
// re-enters the grid, and that is where a body voxel's 8-bit art colour is
// quantized to its material's nearest authored tint (sim/materials.h
// kMatFlagTinted). Voting on the material alone zeroed the colour here, which
// made every fine-skinned body — i.e. every character — land in the world
// undyed while plain cube debris kept its paint.
//
// Pairing rather than voting twice is also what keeps the two answers
// CONSISTENT: an independent colour vote could hand a block the material of the
// flesh under a robe and the colour of the robe over it.
//
// Coordinates are assumed non-negative (bodies are rebased to their min
// corner). A block index past 127 cannot be represented in a DebrisVoxel and is
// DROPPED with `*overflow` set, rather than wrapping the way an (int8_t) cast
// would — that silent wrap is exactly the failure this bound has to prevent,
// since it would teleport part of a limb to the far side of the body.
inline std::vector<DebrisVoxel> DownsampleSkin(
    const std::vector<PrefabVoxel>& src, uint32_t ratio, bool* overflow) {
  const int s = (int)std::max(1u, ratio);
  const uint32_t full = (uint32_t)(s * s * s);
  constexpr int kMaxDistinct = 8;
  struct Blk {
    uint32_t count = 0;
    int n = 0;
    uint32_t pair[kMaxDistinct]{};  // material | color << 16
    uint32_t hits[kMaxDistinct]{};
    // The heaviest body coat among the block's skin voxels (voxload.h
    // BodyStain*), carried WHOLE -- material and amount together, so the
    // coarse lattice can still say what is on it and not merely how much. A
    // max, not a vote: the coarse lattice is what a fragment split off a fine
    // limb draws its own brick from, and a gobbet of a bloodied arm should
    // look bloodied.
    uint16_t stain = 0;
  };
  std::unordered_map<uint64_t, Blk> blocks;
  bool over = false;
  for (const PrefabVoxel& v : src) {
    const int bx = (int)v.x / s, by = (int)v.y / s, bz = (int)v.z / s;
    if (bx < 0 || by < 0 || bz < 0 || bx > 127 || by > 127 || bz > 127) {
      over = true;
      continue;
    }
    // 21 bits per axis. The index is bounded at 127 above, but keeping the
    // fields wide means a future bound change cannot alias two blocks onto one
    // key the way a packed-byte key would.
    uint64_t key = ((uint64_t)bx << 42) | ((uint64_t)by << 21) | (uint64_t)bz;
    const uint32_t pair = (uint32_t)v.material | ((uint32_t)v.color << 16);
    Blk& blk = blocks[key];
    blk.count++;
    int k = 0;
    for (; k < blk.n; k++)
      if (blk.pair[k] == pair) break;
    if (k == blk.n && blk.n < kMaxDistinct) blk.pair[blk.n++] = pair;
    if (k < kMaxDistinct) blk.hits[k]++;
    if (BodyStainAmt(v.stain) > BodyStainAmt(blk.stain)) blk.stain = v.stain;
  }
  if (overflow) *overflow = over;

  std::vector<DebrisVoxel> out;
  out.reserve(blocks.size());
  for (const auto& [key, blk] : blocks) {
    if (blk.count * 2 < full) continue;  // majority-fill: mostly air -> air
    int best = 0;
    for (int k = 1; k < blk.n; k++)
      if (blk.hits[k] > blk.hits[best]) best = k;
    out.push_back({(int8_t)((key >> 42) & 0x1FF), (int8_t)((key >> 21) & 0x1FF),
                   (int8_t)(key & 0x1FF), (uint8_t)(blk.pair[best] >> 16),
                   (uint16_t)(blk.pair[best] & 0xFFFFu), blk.stain});
  }
  return out;
}

// ---- THE HOLE GROWS INTO ITS OWN RIM ---------------------------------------
//
// SPALL, and it is the mechanism that makes "sustained hits dismember" work at
// all. A carve predicate is a fixed shape: a second cut into an existing gash
// tests the same volume, finds the space already gone, and takes nothing — so
// a wound stipples fresh matter beside itself instead of DEEPENING, and
// nothing ever comes apart. Spall fixes that by removing surviving cells that
// already have enough missing face-neighbours, weighted toward the centre,
// which turns a rim of isolated survivors into a torn edge.
//
// WHY IT LIVES HERE. It was written inside `Mob::CarveLimb` and was one of the
// two behaviours that made a corpse a different substance from the creature it
// was a second ago: the same blade, on the same lattice, one function call
// later, left a groove that never widened (docs/PLAN_struck_matter.md). It is
// pure lattice arithmetic — no rig, no joints, no hp — so it belongs next to
// the other function that relates a body's two lattices, and both populations
// call it.
//
// TEMPLATED ON THE CELL rather than on a lattice view, because the two callers
// hold different vectors (`PrefabVoxel` skin, `DebrisVoxel` collider) and both
// want the erase to happen in place. `onLost(cell)` is called with every cell
// taken — the CELL, not its coordinates, because the collider path turns what
// it lost into particles and a gobbet needs the material it was made of.
// Returns the number removed.
struct SpallParams {
  Vec3 centre{};        // in LATTICE units, already scaled by the caller
  float radius = 0.0f;  // ditto
  float strength = 0.0f;  // 0..1, 0 disables
  int rounds = 0;
  uint32_t seed = 0;
};

template <class Vox, class OnLost>
inline size_t SpallGrow(std::vector<Vox>& cells, const SpallParams& p,
                        OnLost onLost) {
  if (p.rounds <= 0 || p.strength <= 0.0f || p.radius <= 0.0f) return 0;
  const float r2 = p.radius * p.radius;
  // Packed key over the lattice. int16 skin coords, so 21 bits per axis with a
  // bias is ample and collision-free (unlike hashing the position, which would
  // make occupancy probabilistic — the one thing this pass must not be).
  auto key = [](int x, int y, int z) -> uint64_t {
    return ((uint64_t)(uint32_t)(x + 32768) << 42) |
           ((uint64_t)(uint32_t)(y + 32768) << 21) |
           (uint64_t)(uint32_t)(z + 32768);
  };
  // ONLY THE CELLS NEAR THE BLOW. `live` is asked about the six neighbours of
  // a cell inside the sphere, so nothing further than radius + 1 from the
  // centre is ever looked up; the margin below is that plus a cell of slack.
  // It used to hold the whole lattice, rebuilt every round, which on a felled
  // tree was a 35k-entry hash set per round per sword probe for a crater a few
  // voxels across. Same set of answers, so the same spall.
  const float reach = p.radius + 2.0f;
  const float reach2 = reach * reach;
  auto nearBlow = [&](const Vox& v) {
    const float dx = (float)v.x + 0.5f - p.centre.x,
                dy = (float)v.y + 0.5f - p.centre.y,
                dz = (float)v.z + 0.5f - p.centre.z;
    return dx * dx + dy * dy + dz * dz < reach2;
  };
  std::unordered_set<uint64_t> live;
  auto rebuild = [&] {
    live.clear();
    for (const Vox& v : cells)
      if (nearBlow(v)) live.insert(key(v.x, v.y, v.z));
  };
  rebuild();
  // A voxel on an intact surface already has one open face, so "eroded" starts
  // at three: fewer and this eats the whole skin from the outside in rather
  // than widening the crater.
  constexpr int kMinOpenFaces = 3;
  size_t total = 0;
  for (int round = 0; round < p.rounds; round++) {
    auto doomed = [&](int x, int y, int z) {
      const Vec3 d{(float)x + 0.5f - p.centre.x, (float)y + 0.5f - p.centre.y,
                   (float)z + 0.5f - p.centre.z};
      const float d2 = d.dot(d);
      if (d2 >= r2) return false;   // outside the blast
      int open = 0;
      static const int kN[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
      for (const auto& n : kN)
        if (!live.count(key(x + n[0], y + n[1], z + n[2]))) open++;
      if (open < kMinOpenFaces) return false;
      // Proximity-weighted, so the tearing is concentrated at the blow and
      // fades out rather than eroding the rim uniformly. Keyed on the caller's
      // seed plus the round, so a replay spalls identically.
      const float t = std::sqrt(d2 / r2);
      const float chance = p.strength * (1.0f - t) * ((float)open / 6.0f);
      const uint32_t h =
          rng::Hash3(p.seed + 0x5BF03635u * (uint32_t)(round + 1),
                     (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663),
                     (uint32_t)(z * 83492791));
      return (float)(h & 0xFFFFu) / 65535.0f < chance;
    };
    const size_t before = cells.size();
    cells.erase(std::remove_if(cells.begin(), cells.end(),
                               [&](const Vox& v) {
                                 if (!doomed(v.x, v.y, v.z)) return false;
                                 onLost(v);
                                 return true;
                               }),
                cells.end());
    const size_t took = before - cells.size();
    total += took;
    // Nothing left to grow into: stop rather than paying for empty passes.
    if (took == 0) break;
    // The NEXT round must see the hole this one opened, or every round tests
    // the same rim and the growth is one round wide however many are asked
    // for. This is the whole mechanism.
    rebuild();
  }
  return total;
}
