// demon_circle.cpp — the salt circle detector. See demon_circle.h.
#include "game/demon_circle.h"

#include <algorithm>
#include <cmath>

#include "sim/world.h"   // PowderMassOfState

const char* CircleVerdictName(CircleVerdict v) {
  switch (v) {
    case CircleVerdict::Closed: return "closed";
    case CircleVerdict::Open: return "open";
    case CircleVerdict::OnRing: return "on the ring";
    case CircleVerdict::Unknown: return "unknown";
  }
  return "?";
}

bool CircleShape::Inside(float x, float z) const {
  return InsideCell((int32_t)std::floor(x), (int32_t)std::floor(z));
}

IVec3 CircleShape::ChunkLo() const {
  return IVec3{(x0 - 1) >> 4, (feetY - 1) >> 4, (z0 - 1) >> 4};
}
IVec3 CircleShape::ChunkHi() const {
  return IVec3{(x0 + w) >> 4, (feetY + 1) >> 4, (z0 + h) >> 4};
}

bool CircleFindFeet(const CircleProbe& probe, IVec3 at, int32_t search, int32_t& feetY,
                    bool& known) {
  known = true;
  // From two above the impact (a lob resolves in the air cell it hit from,
  // or one into the floor) down to `search` below it.
  for (int32_t y = at.y + 2; y >= at.y - search; y--) {
    bool k1 = false, k2 = false;
    const uint32_t below = probe.wordAt(probe.ctx, at.x, y - 1, at.z, k1) & 0xFFFu;
    const uint32_t here = probe.wordAt(probe.ctx, at.x, y, at.z, k2) & 0xFFFu;
    if (!k1 || !k2) {
      known = false;
      return false;
    }
    if (probe.passable(probe.ctx, here) && !probe.passable(probe.ctx, below)) {
      feetY = y;
      return true;
    }
  }
  return false;
}

CircleShape ScanCircle(const CircleProbe& probe, const CircleParams& p, int32_t sx,
                       int32_t feetY, int32_t sz) {
  CircleShape out;
  out.startX = sx;
  out.startZ = sz;
  out.feetY = feetY;
  const int32_t R = std::max(p.radiusMax, 1);
  const int32_t side = 2 * R + 3;   // the fill box, plus a ring of margin
  const int32_t bx = sx - R - 1, bz = sz - R - 1;
  // 0 = unvisited, 1 = inside, 2 = wall (salt), 3 = queued
  std::vector<uint8_t> st((size_t)side * side, 0);
  auto at = [&](int32_t x, int32_t z) -> uint8_t& {
    return st[(size_t)(z - bz) * side + (x - bx)];
  };
  // Is the column a wall? -1 unknown.
  auto wall = [&](int32_t x, int32_t z) -> int {
    for (int32_t y = feetY - p.slabBelow; y <= feetY + p.slabAbove; y++) {
      bool k = false;
      const uint32_t w = probe.wordAt(probe.ctx, x, y, z, k);
      if (!k) {
        out.unknownAt = IVec3{x, y, z};
        return -1;
      }
      if ((w & 0xFFFu) == p.saltMat && PowderMassOfState((w >> 12) & 15u) >= p.minEighths)
        return 1;
    }
    return 0;
  };
  {
    const int w0 = wall(sx, sz);
    if (w0 < 0) {
      out.verdict = CircleVerdict::Unknown;
      return out;
    }
    if (w0 == 1) {
      out.verdict = CircleVerdict::OnRing;
      return out;
    }
  }
  std::vector<IVec3> q;   // (x, z, 0), FIFO by index: deterministic order
  q.reserve(256);
  q.push_back(IVec3{sx, sz, 0});
  at(sx, sz) = 3;
  int32_t minX = sx, maxX = sx, minZ = sz, maxZ = sz;
  int64_t sumX = 0, sumZ = 0;
  for (size_t qi = 0; qi < q.size(); qi++) {
    const int32_t x = q[qi].x, z = q[qi].y;
    const int64_t dx = x - sx, dz = z - sz;
    if (dx * dx + dz * dz > (int64_t)R * R) {
      out.verdict = CircleVerdict::Open;   // escaped: no ring round the start
      out.visited = (int32_t)qi + 1;
      return out;
    }
    at(x, z) = 1;
    out.cells++;
    sumX += x;
    sumZ += z;
    minX = std::min(minX, x);
    maxX = std::max(maxX, x);
    minZ = std::min(minZ, z);
    maxZ = std::max(maxZ, z);
    static const int32_t kD[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& d : kD) {
      const int32_t nx = x + d[0], nz = z + d[1];
      // Inside the fill box by construction (R + 1 of margin, and a column
      // past R ends the scan before its neighbours are pushed).
      uint8_t& s = at(nx, nz);
      if (s != 0) continue;
      const int wv = wall(nx, nz);
      if (wv < 0) {
        out.verdict = CircleVerdict::Unknown;
        out.visited = (int32_t)qi + 1;
        return out;
      }
      if (wv == 1) {
        s = 2;
        out.ringCells++;
        continue;
      }
      s = 3;
      q.push_back(IVec3{nx, nz, 0});
    }
  }
  out.visited = (int32_t)q.size();
  out.verdict = CircleVerdict::Closed;
  out.x0 = minX;
  out.z0 = minZ;
  out.w = maxX - minX + 1;
  out.h = maxZ - minZ + 1;
  out.mask.assign((size_t)out.w * out.h, 0);
  for (const IVec3& c : q) out.mask[(size_t)(c.y - minZ) * out.w + (c.x - minX)] = 1;
  out.cx = (float)((double)sumX / out.cells) + 0.5f;
  out.cz = (float)((double)sumZ / out.cells) + 0.5f;
  float r2 = 0;
  for (const IVec3& c : q) {
    const float ddx = c.x + 0.5f - out.cx, ddz = c.y + 0.5f - out.cz;
    r2 = std::max(r2, ddx * ddx + ddz * ddz);
  }
  out.radius = std::sqrt(r2);
  return out;
}
