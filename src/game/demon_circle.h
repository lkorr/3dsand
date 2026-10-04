// demon_circle.h — THE SALT CIRCLE DETECTOR (docs/PLAN_demons.md D1).
//
// WHAT A CIRCLE IS. From the arrival column, a 2D flood fill over a short
// SLAB of cells (one below the demon's feet to one above them: the floor a
// ring may be inlaid into, the row a poured ring lies on, the row above it)
// in which a COLUMN is a WALL iff any of its slab cells is salt -- and only
// salt: stone, wood and water are air to the fill, so a walled room is not a
// circle and a moat is not a circle. 4-connected, so a ring drawn on the
// diagonal is closed and a single missing cell is a gap.
//
//   * the fill reaches a column further than `radiusMax` from the start:
//     OPEN (no circle, or a broken one);
//   * the start column is itself salt (the name was cast onto the ring):
//     ON RING, which is not a circle either;
//   * the fill needs a cell no store holds: UNKNOWN (the caller holds its last
//     answer -- see demon.h for what "hold" means at a summoning);
//   * otherwise CLOSED: the fill region (a bitmask over its bounding box) is
//     the inside, the salt it ran into is the ring.
//
// SALT BY NAME, resolved at load (assets/demons/circle.json). Brine is not
// salt (dissolved salt is solute on a water cell, not a salt cell), molten
// salt is not salt, scattered salt is salt cells that no longer make a loop.
//
// PURE. The voxels come in through `CircleProbe` (the caller binds the T-4
// snapshot mirror + the fetch cache), so the scan is a function of that
// tick-latched state and nothing else (rule 1). BOUNDED: at most
// (2 radiusMax + 1)^2 columns, three cells each.
#pragma once

#include <cstdint>
#include <vector>

#include "math3d.h"

struct CircleProbe {
  // The material at a cell, or `known = false` if no store holds it.
  uint32_t (*matAt)(void* ctx, int32_t x, int32_t y, int32_t z, bool& known) = nullptr;
  void* ctx = nullptr;
  // Can a body stand in this material (air, gas) -- for the floor search.
  bool (*passable)(void* ctx, uint32_t mat) = nullptr;
};

struct CircleParams {
  uint32_t saltMat = 0;       // the one material that makes a ring
  int32_t radiusMax = 60;     // cells: the fill escaping this far = no circle
  int32_t slabBelow = 1;      // slab rows below the feet row
  int32_t slabAbove = 1;      // ...and above it
};

enum class CircleVerdict : uint8_t { Closed = 0, Open, OnRing, Unknown };
const char* CircleVerdictName(CircleVerdict v);

struct CircleShape {
  CircleVerdict verdict = CircleVerdict::Unknown;
  int32_t startX = 0, startZ = 0, feetY = 0;
  // THE INSIDE: a bitmask over [x0, x0 + w) x [z0, z0 + h), world cells.
  int32_t x0 = 0, z0 = 0, w = 0, h = 0;
  std::vector<uint8_t> mask;
  int32_t cells = 0;          // columns inside
  int32_t ringCells = 0;      // salt columns the fill ran into
  int32_t visited = 0;        // columns the fill touched (cost)
  float cx = 0, cz = 0;       // centroid of the inside, world voxels
  float radius = 0;           // furthest inside column from the centroid
  // UNKNOWN: the first cell no store held (the attribution for "why").
  IVec3 unknownAt{};
  bool Closed() const { return verdict == CircleVerdict::Closed; }
  bool InsideCell(int32_t x, int32_t z) const {
    const int32_t ix = x - x0, iz = z - z0;
    return Closed() && ix >= 0 && iz >= 0 && ix < w && iz < h && mask[(size_t)iz * w + ix] != 0;
  }
  bool Inside(float x, float z) const;
  // The world chunks the inside plus its ring touch (for the dirty test).
  IVec3 ChunkLo() const;
  IVec3 ChunkHi() const;
};

// The cell a body arriving at `at` stands on: down from two above `at`
// through passable cells to the first one that is not, up to `search`
// cells. Returns false (and `known` false) when a store missed on the way.
bool CircleFindFeet(const CircleProbe& probe, IVec3 at, int32_t search, int32_t& feetY,
                    bool& known);

// The scan. `feetY` is the row the demon stands in.
CircleShape ScanCircle(const CircleProbe& probe, const CircleParams& p, int32_t sx,
                       int32_t feetY, int32_t sz);
