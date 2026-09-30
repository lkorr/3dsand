#pragma once
// rainexpo.h — THE RAIN LATTICE and THE RAIN EXPOSURE MAP, CPU side
// (DESIGN.md §9.w "Where the rain lands"; assets/shaders/sim_rain_expo.wgsl,
// sim_mutate.wgsl rainFall and sim_step.wgsl rainExposed are the GPU side).
//
// Rain falls along a SLOPE: TickParams rainSlopeQx / rainSlopeQz, Q16 and
// always a multiple of 4096 (weather::SimRain quantises it to sixteenths), so
// n = slopeQ >> 12 is the drift DOWNWIND in sixteenths of a cell per cell
// fallen. A fall line is an integer KEY per axis:
//
//     key(c) = c.x + D(n, c.y),   D(n, y) = (n * y + 8) >> 4   (floor)
//
// and the line's MAIN cell at level y is x = key - D(n, y). Anchored to
// ABSOLUTE y, not the window: the lattice of lines is a property of the world
// and the tick, not of where the residency window happens to sit. Exact in
// 32-bit WGSL: |n| <= kRainSlopeMaxN (44) and world y is far below 2^31 / 44.
//
// Every cell lies on exactly ONE line per axis (key is a function of the
// cell), which is the whole of rainFall's determinism argument: lines with
// different keys never share a main cell.
//
// THE PATH of a line between two levels is an L: from main(y+1) straight down
// to (x(y+1), y, z(y+1)), then along x to x(y), then along z to z(y) = main(y).
// A |n| > 16 slope moves more than one cell a level, and testing only main
// cells would let a steep line step clean through a one-voxel wall; testing
// the L does not. Cells outside the window read as AIR (the rain comes in from
// outside; the sim knows nothing there).
//
// THE EXPOSURE MAP holds, per 4-key TEXEL (both axes), the level of the first
// ray-blocking cell met on the texel's REPRESENTATIVE line (key 4t + 2) coming
// down from the window top, or kRainExpoOpen. A cell is exposed to the rain if
// it is not below that level less kRainExpoMargin. Built in full on every tick
// that can read it (rain or wetness > 0) by sim_rain_expo, before the CA; the
// CA reads it read-only. No history, so nothing to carry across a save, a
// load, a replay start or a window move.

#include <algorithm>
#include <cstdint>

namespace rainlat {

constexpr int32_t kExpoTexShift = 2;          // 4-key texels (sim_rain_expo RX_TEX_SHIFT)
constexpr int32_t kExpoOpen = INT32_MIN;      // "met nothing in the window"
constexpr int32_t kExpoMargin = 1;            // sim_step RX_MARGIN
constexpr int32_t kExpoRepOffset = 2;         // the representative line of a texel
// The largest per-axis texel extent: the window's 512 keys plus the drift
// over its height at the steepest slope, in texels, plus two for alignment.
constexpr int32_t kExpoMaxAxis = (512 + 44 * 32) / 4 + 2;   // 482
static_assert(kExpoMaxAxis == 482, "sim_rain_expo.wgsl RX_MAX_AXIS agrees");

inline int32_t FloorDiv(int32_t a, int32_t b) {   // b > 0
  return a >= 0 ? a / b : -((-a + b - 1) / b);
}
inline int32_t CeilDiv(int32_t a, int32_t b) { return -FloorDiv(-a, b); }
// D(n, y): the drift of every line at level y, sixteenths rounded to nearest.
inline int32_t Drift(int32_t n, int32_t y) { return FloorDiv(n * y + 8, 16); }
inline int32_t N(int32_t slopeQ) { return slopeQ >> 12; }

// The levels y at which line `k` (slope n) has its main cell inside [lo, hi]
// on one axis, as [yLo, yHi] (empty when yLo > yHi). x(y) = k - D(n, y).
inline void AxisSpan(int32_t k, int32_t n, int32_t lo, int32_t hi,
                     int32_t& yLo, int32_t& yHi) {
  const int32_t A = k - hi, B = k - lo;   // need A <= D(n, y) <= B
  if (n == 0) {
    const bool in = A <= 0 && 0 <= B;
    yLo = in ? INT32_MIN / 2 : 1;
    yHi = in ? INT32_MAX / 2 : 0;
    return;
  }
  if (n > 0) {
    yLo = CeilDiv(16 * A - 8, n);
    yHi = FloorDiv(16 * B + 7, n);
  } else {
    const int32_t m = -n;
    yLo = FloorDiv(8 - 16 * B - 16, m) + 1;
    yHi = FloorDiv(8 - 16 * A, m);
  }
}

// The window the lattice is clipped to, in world cells.
struct Box {
  int32_t lo[3], hi[3];   // inclusive
};

// The exposure map's texel domain for this window and slope: texel `lo` and
// extent per axis (x, z). sim_rain_expo.wgsl rxDomain agrees.
inline void ExpoDomain(const Box& b, int32_t nx, int32_t nz, int32_t lo[2], int32_t ext[2]) {
  const int32_t n[2] = {nx, nz};
  const int32_t a[2] = {0, 2};
  for (int i = 0; i < 2; i++) {
    const int32_t d0 = Drift(n[i], b.lo[1]), d1 = Drift(n[i], b.hi[1]);
    const int32_t klo = b.lo[a[i]] + std::min(d0, d1);
    const int32_t khi = b.hi[a[i]] + std::max(d0, d1);
    lo[i] = klo >> kExpoTexShift;
    ext[i] = (khi >> kExpoTexShift) - lo[i] + 1;
  }
}

// The level of the first cell on line (kx, kz) for which `blocks(x, y, z)` is
// true, walking the path from the window top down (see the header), or
// kExpoOpen. `blocks` is called only for cells inside `b`. THE REFERENCE the
// GPU march (sim_rain_expo rxMarch) is written against; it walks every level,
// where the shader skips empty sentinel chunks — an exact skip, since an
// EMPTY chunk is all air.
template <class Blocks>
int32_t FirstBlockLevel(const Box& b, int32_t nx, int32_t nz, int32_t kx, int32_t kz,
                        Blocks&& blocks) {
  auto inBox = [&](int32_t x, int32_t y, int32_t z) {
    return x >= b.lo[0] && x <= b.hi[0] && y >= b.lo[1] && y <= b.hi[1] &&
           z >= b.lo[2] && z <= b.hi[2];
  };
  auto test = [&](int32_t x, int32_t y, int32_t z) { return inBox(x, y, z) && blocks(x, y, z); };
  int32_t px = 0, pz = 0;
  for (int32_t y = b.hi[1]; y >= b.lo[1]; y--) {
    const int32_t x = kx - Drift(nx, y), z = kz - Drift(nz, y);
    if (y == b.hi[1]) {
      if (test(x, y, z)) return y;
    } else {
      // The L: down, along x, along z (the last cell is the main one).
      const int32_t sx = x >= px ? 1 : -1, sz = z >= pz ? 1 : -1;
      for (int32_t cx = px;; cx += sx) {
        if (test(cx, y, pz)) return y;
        if (cx == x) break;
      }
      for (int32_t cz = pz + sz; z != pz; cz += sz) {
        if (test(x, y, cz)) return y;
        if (cz == z) break;
      }
    }
    px = x;
    pz = z;
  }
  return kExpoOpen;
}

// THE CELL QUESTION: is `c` exposed by the map? The representative line of
// c's texel is walked for a blocker ABOVE c.y + kExpoMargin; there is none iff
// the map's level for that texel is <= c.y + kExpoMargin. The upward form of
// the same predicate, for a caller that can only see voxels near c: `blocks`
// returns 1 = blocker, 0 = clear, -1 = unknown, and an unknown cell ends the
// walk EXPOSED (today's "unknown = open"). Walks from c upward to the window
// top, testing exactly the path cells the GPU march tests at those levels.
template <class Blocks3>
bool ExposedWalkUp(const Box& b, int32_t nx, int32_t nz, int32_t cx, int32_t cy,
                   int32_t cz, Blocks3&& blocks) {
  const int32_t kx = (((cx + Drift(nx, cy)) >> kExpoTexShift) << kExpoTexShift) + kExpoRepOffset;
  const int32_t kz = (((cz + Drift(nz, cy)) >> kExpoTexShift) << kExpoTexShift) + kExpoRepOffset;
  auto inBox = [&](int32_t x, int32_t y, int32_t z) {
    return x >= b.lo[0] && x <= b.hi[0] && y >= b.lo[1] && y <= b.hi[1] &&
           z >= b.lo[2] && z <= b.hi[2];
  };
  for (int32_t y = std::max(cy + kExpoMargin + 1, b.lo[1]); y <= b.hi[1]; y++) {
    const int32_t x = kx - Drift(nx, y), z = kz - Drift(nz, y);
    auto t = [&](int32_t ax, int32_t az) -> int {
      if (!inBox(ax, y, az)) return 0;
      return blocks(ax, y, az);
    };
    int r = 0;
    if (y == b.hi[1]) {
      r = t(x, z);
    } else {
      const int32_t px = kx - Drift(nx, y + 1), pz = kz - Drift(nz, y + 1);
      const int32_t sx = x >= px ? 1 : -1, sz = z >= pz ? 1 : -1;
      for (int32_t ax = px; r == 0; ax += sx) {
        r = t(ax, pz);
        if (ax == x) break;
      }
      for (int32_t az = pz + sz; r == 0 && z != pz; az += sz) {
        r = t(x, az);
        if (az == z) break;
      }
    }
    if (r < 0) return true;    // unknown: open, as OpenToSky has always said
    if (r > 0) return false;
  }
  return true;
}

}  // namespace rainlat
