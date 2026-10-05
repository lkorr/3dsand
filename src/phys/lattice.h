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
  // Per block: how many skin voxels landed (`count`), the distinct
  // (material, art colour) pairs and how often each was seen, and the
  // heaviest body coat among them (voxload.h BodyStain*), carried WHOLE --
  // material and amount together, so the coarse lattice can still say what
  // is on it and not merely how much. A max, not a vote: the coarse lattice
  // is what a fragment split off a fine limb draws its own brick from, and a
  // gobbet of a bloodied arm should look bloodied.
  //
  // ---- A FLAT LATTICE, NOT A HASH MAP (PLAN_fight64_perf Q2) ---------------
  //
  // The blocks used to live in an unordered_map keyed on the packed block
  // coordinate, and every blade carve re-derives a limb's collider through
  // here: in the 64-creature brawl the map's node allocations and probes were
  // ~1.8 ms of the ~5 ms carve. The block lattice is bounded (<= 128 a side,
  // the DebrisVoxel range) and a limb's is small, so it is a dense index over
  // the blocks' own bounding box -- one int32 per block cell, -1 = no block --
  // pointing into a packed list of the blocks actually touched. Both live in
  // thread-local scratch (carves run on the mob work pool), reused across
  // calls, so the steady state allocates only the returned vector.
  //
  // THE OUTPUT ORDER IS NOW THE LATTICE'S (z, then y, then x), where it was the
  // hash map's bucket order. Same voxel SET, same per-block answer; only the
  // order of the collider list moved -- which reorders the greedy box merge's
  // walk, so a carved limb's compound can come out as a different (equally
  // exact) box set and the world hash moves with it. Lattice order is also the
  // first order this list has had that is a property of the shape rather than
  // of a container's internals.
  //
  // ---- ...AND NO BLOCK CONSTRUCTED (fight64 round 3, package X) ----------
  // Each touched block used to be a 76-byte struct zero-built on first touch.
  // Now it is an 8-byte header (count, distinct pairs, heaviest coat) in one
  // packed list and its pair/hit table in two more that are never cleared: a
  // pair slot is written before it is read, and its hit count is set when
  // the slot is first taken. Same votes, same lattice order out.
  struct Head {
    uint32_t count;
    uint16_t stain;
    uint16_t n;
  };
  struct Scratch {
    std::vector<int32_t> at;      // dense block index -> head[], -1 = none
    std::vector<Head> head;       // per touched block
    std::vector<uint32_t> pair;   // per touched block * kMaxDistinct
    std::vector<uint32_t> hits;   // ditto
  };
  thread_local Scratch sc;
  bool over = false;
  // ---- A SHIFT, NOT A DIVIDE (PLAN_fight64_perf round 3, package X) --------
  // Every carve and every wound stain re-derives the collider through here,
  // and the sampler put the two `/ s` passes (an integer divide per axis per
  // skin voxel, by a ratio only known at run time) at the top of this
  // function. Every authored ratio is a power of two (skin 8 / collider 4 or
  // 2), so the block index is a shift -- with C++'s truncate-toward-zero
  // division reproduced exactly for a negative coordinate, so a voxel at -1
  // still lands in block 0 as `/` put it. A ratio that is not a power of two
  // keeps the divide. Same blocks, same order, same output.
  int sh = -1;
  if ((s & (s - 1)) == 0) {
    sh = 0;
    while ((1 << sh) < s) sh++;
  }
  auto blk = [s, sh](int c) -> int {
    if (sh < 0) return c / s;
    return c >= 0 ? (c >> sh) : -((-c) >> sh);
  };
  // Pass 1: the block bounding box (in-range blocks only).
  int lo[3] = {128, 128, 128}, hi[3] = {-1, -1, -1};
  for (const PrefabVoxel& v : src) {
    const int bx = blk(v.x), by = blk(v.y), bz = blk(v.z);
    if (bx < 0 || by < 0 || bz < 0 || bx > 127 || by > 127 || bz > 127) {
      over = true;
      continue;
    }
    lo[0] = std::min(lo[0], bx); hi[0] = std::max(hi[0], bx);
    lo[1] = std::min(lo[1], by); hi[1] = std::max(hi[1], by);
    lo[2] = std::min(lo[2], bz); hi[2] = std::max(hi[2], bz);
  }
  if (overflow) *overflow = over;
  std::vector<DebrisVoxel> out;
  if (hi[0] < 0) return out;
  const int ex = hi[0] - lo[0] + 1, ey = hi[1] - lo[1] + 1, ez = hi[2] - lo[2] + 1;
  const size_t cells = (size_t)ex * ey * ez;
  sc.at.assign(cells, -1);
  size_t touched = 0;
  // Pass 2: vote.
  for (const PrefabVoxel& v : src) {
    const int bx = blk(v.x), by = blk(v.y), bz = blk(v.z);
    if (bx < 0 || by < 0 || bz < 0 || bx > 127 || by > 127 || bz > 127) continue;
    int32_t& slot =
        sc.at[((size_t)(bz - lo[2]) * ey + (size_t)(by - lo[1])) * ex + (size_t)(bx - lo[0])];
    if (slot < 0) {
      slot = (int32_t)touched++;
      if (sc.head.size() < touched) {
        sc.head.resize(touched * 2);
        sc.pair.resize(touched * 2 * kMaxDistinct);
        sc.hits.resize(touched * 2 * kMaxDistinct);
      }
      sc.head[(size_t)slot] = Head{0u, 0u, 0u};
    }
    const size_t c = (size_t)slot;
    Head& h = sc.head[c];
    h.count++;
    const uint32_t pair = (uint32_t)v.material | ((uint32_t)v.color << 16);
    uint32_t* pr = &sc.pair[c * kMaxDistinct];
    uint32_t* ht = &sc.hits[c * kMaxDistinct];
    int k = 0;
    const int n = (int)h.n;
    for (; k < n; k++)
      if (pr[k] == pair) break;
    if (k == n && n < kMaxDistinct) {
      pr[k] = pair;
      ht[k] = 0;
      h.n = (uint16_t)(n + 1);
    }
    if (k < kMaxDistinct) ht[k]++;
    if (BodyStainAmt(v.stain) > BodyStainAmt(h.stain)) h.stain = v.stain;
  }
  // Pass 3: emit in lattice order.
  out.reserve(touched);
  size_t i = 0;
  for (int z = 0; z < ez; z++)
    for (int y = 0; y < ey; y++)
      for (int x = 0; x < ex; x++, i++) {
        const int32_t slot = sc.at[i];
        if (slot < 0) continue;
        const Head& h = sc.head[(size_t)slot];
        if (h.count * 2 < full) continue;  // majority-fill: mostly air -> air
        const uint32_t* pr = &sc.pair[(size_t)slot * kMaxDistinct];
        const uint32_t* ht = &sc.hits[(size_t)slot * kMaxDistinct];
        int best = 0;
        for (int k = 1; k < (int)h.n; k++)
          if (ht[k] > ht[best]) best = k;
        out.push_back({(int8_t)(x + lo[0]), (int8_t)(y + lo[1]), (int8_t)(z + lo[2]),
                       (uint8_t)(pr[best] >> 16),
                       (uint16_t)(pr[best] & 0xFFFFu), h.stain});
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
  // ---- A BITMAP OVER THE BLOW'S BOX, NOT A HASH SET (fight64 round 3 X) ----
  // `live` is only ever asked about cells within `reach` of the centre (the
  // six neighbours of a cell inside the radius), and only cells that pass
  // `nearBlow` are ever put in it, so a bit per cell of the box around that
  // sphere answers every question the set answered, identically: a cell
  // outside the box was never in the set either. Rebuilt per round exactly as
  // the set was (a full scan, so a duplicate cell left behind by the erase
  // stays live, as it did). The set rebuilt a node per voxel per round and
  // was the spall's whole cost in the 64-creature brawl's blade carves.
  // Thread-local scratch: carves can run on the mob work pool. A box too big
  // for a bitmap (a blast on a felled tree) keeps the set.
  const int boxLo[3] = {(int)std::floor(p.centre.x - reach) - 2,
                        (int)std::floor(p.centre.y - reach) - 2,
                        (int)std::floor(p.centre.z - reach) - 2};
  const int boxN[3] = {(int)std::ceil(p.centre.x + reach) + 3 - boxLo[0],
                       (int)std::ceil(p.centre.y + reach) + 3 - boxLo[1],
                       (int)std::ceil(p.centre.z + reach) + 3 - boxLo[2]};
  const uint64_t boxCells = (uint64_t)std::max(boxN[0], 1) *
                            (uint64_t)std::max(boxN[1], 1) *
                            (uint64_t)std::max(boxN[2], 1);
  const bool useBits = boxCells <= (1ull << 24);
  thread_local std::vector<uint64_t> bits;
  std::unordered_set<uint64_t> live;
  auto bitOf = [&](int x, int y, int z, size_t& at) -> bool {
    const int bx = x - boxLo[0], by = y - boxLo[1], bz = z - boxLo[2];
    if (bx < 0 || by < 0 || bz < 0 || bx >= boxN[0] || by >= boxN[1] ||
        bz >= boxN[2])
      return false;
    at = ((size_t)bz * (size_t)boxN[1] + (size_t)by) * (size_t)boxN[0] +
         (size_t)bx;
    return true;
  };
  auto isLive = [&](int x, int y, int z) -> bool {
    if (!useBits) return live.count(key(x, y, z)) != 0;
    size_t at = 0;
    if (!bitOf(x, y, z, at)) return false;
    return (bits[at >> 6] >> (at & 63u)) & 1ull;
  };
  auto rebuild = [&] {
    if (!useBits) {
      live.clear();
      for (const Vox& v : cells)
        if (nearBlow(v)) live.insert(key(v.x, v.y, v.z));
      return;
    }
    bits.assign((size_t)((boxCells + 63) >> 6), 0ull);
    for (const Vox& v : cells) {
      if (!nearBlow(v)) continue;
      size_t at = 0;
      // A near-blow cell is inside the box by construction (the box pads the
      // reach by two cells); the test is the belt to that brace.
      if (bitOf(v.x, v.y, v.z, at)) bits[at >> 6] |= 1ull << (at & 63u);
    }
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
        if (!isLive(x + n[0], y + n[1], z + n[2])) open++;
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
