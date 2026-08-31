#include "game/ai_nav.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace ai {
namespace {

// Diagonal cost. sqrt(2) exactly, so a diagonal never looks cheaper than the
// two cardinals it replaces and the search cannot be talked into a staircase.
constexpr float kDiag = 1.41421356f;

// Neighbour offsets: four cardinals first, then four diagonals. The order is
// load-bearing only for tie-breaking, but it is FIXED, which is what makes two
// runs of the same fight produce the same path (see the determinism note in
// ai_behavior.h — nav is not hashed, but a path that flickers between two equal
// routes reads as a twitching creature).
constexpr int kNbrX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr int kNbrZ[8] = {0, 0, 1, -1, 1, -1, 1, -1};

// Per-column state, memoized for the life of one search. The downward ground
// scan is the expensive part of the whole planner (it walks the chunk cache),
// and eight neighbours plus the smoothing pass all ask about the same column —
// so it is paid at most once each.
enum : uint8_t { kUnprobed = 0, kOpenCol = 1, kBlockedCol = 2, kUnknownCol = 3 };

// One search's scratch. Static and reused: a replan happens on a cadence for at
// most kMaxMobs creatures, and re-heap-allocating ~2400 entries every time is
// pure garbage for no benefit. Not thread-safe on purpose — the AI runs inside
// the single-threaded tick.
struct Scratch {
  int side = 0, radius = 0;
  int baseX = 0, baseZ = 0;      // world column of grid cell (0,0)
  std::vector<uint8_t> kind;     // kUnprobed / kOpenCol / kBlockedCol / kUnknownCol
  std::vector<int32_t> colY;     // standing Y of the column (valid when kOpenCol)
  std::vector<float> g;
  std::vector<float> f;
  std::vector<int32_t> came;
  std::vector<uint8_t> closed;
  std::vector<int32_t> heap;     // binary min-heap of grid indices, keyed on f

  void Reset(int r, int cx, int cz) {
    radius = r;
    side = 2 * r + 1;
    baseX = cx - r;
    baseZ = cz - r;
    const size_t n = (size_t)side * (size_t)side;
    kind.assign(n, kUnprobed);
    colY.assign(n, 0);
    g.assign(n, 1e30f);
    f.assign(n, 1e30f);
    came.assign(n, -1);
    closed.assign(n, 0);
    heap.clear();
  }
  bool In(int gx, int gz) const {
    return gx >= 0 && gz >= 0 && gx < side && gz < side;
  }
  int Idx(int gx, int gz) const { return gz * side + gx; }
};

Scratch& Pad() {
  static Scratch s;
  return s;
}

void HeapPush(Scratch& s, int idx) {
  s.heap.push_back(idx);
  size_t i = s.heap.size() - 1;
  while (i > 0) {
    size_t p = (i - 1) / 2;
    if (s.f[s.heap[p]] <= s.f[s.heap[i]]) break;
    std::swap(s.heap[p], s.heap[i]);
    i = p;
  }
}

int HeapPop(Scratch& s) {
  const int top = s.heap.front();
  s.heap.front() = s.heap.back();
  s.heap.pop_back();
  size_t i = 0;
  for (;;) {
    size_t l = 2 * i + 1, r = l + 1, m = i;
    if (l < s.heap.size() && s.f[s.heap[l]] < s.f[s.heap[m]]) m = l;
    if (r < s.heap.size() && s.f[s.heap[r]] < s.f[s.heap[m]]) m = r;
    if (m == i) break;
    std::swap(s.heap[m], s.heap[i]);
    i = m;
  }
  return top;
}

// Resolve one column, memoized. Returns its kind and (for kOpenCol) writes the
// standing height.
//
// THE UNKNOWN CASE IS THE WHOLE DESIGN (ai_nav.h rule 1). A column the mirror
// cannot answer for is NOT blocked; it is a column whose height we will inherit
// from whoever reaches it. Reporting it as blocked would fence the creature in
// with its own ignorance, which is exactly the bug the steering layer already
// paid for once.
uint8_t Column(Scratch& s, const NavProbe& probe, const NavParams& p, int gx,
               int gz, int probeFromY, int32_t& outY) {
  const int i = s.Idx(gx, gz);
  if (s.kind[i] != kUnprobed) {
    outY = s.colY[i];
    return s.kind[i];
  }
  const int wx = s.baseX + gx, wz = s.baseZ + gz;
  int y = 0;
  if (!probe.ground(probe.ctx, wx, wz, probeFromY, y)) {
    s.kind[i] = kUnknownCol;
    s.colY[i] = 0;
    outY = 0;
    return kUnknownCol;
  }
  // Standing room. A creature is not a point: a ledge with a ceiling one voxel
  // above it is ground you cannot occupy, and a planner that ignores that sends
  // bodies head-first into overhangs.
  for (int k = 0; k < p.headroom; k++) {
    if (probe.blocked(probe.ctx, wx, y + k, wz)) {
      s.kind[i] = kBlockedCol;
      s.colY[i] = y;
      outY = y;
      return kBlockedCol;
    }
  }
  s.kind[i] = kOpenCol;
  s.colY[i] = y;
  outY = y;
  return kOpenCol;
}

// Can a body standing at height `fromY` move onto this column? Returns the
// height it would stand at. Unknown inherits `fromY`, which keeps the path
// continuous across the edge of knowledge.
bool StepTo(Scratch& s, const NavProbe& probe, const NavParams& p, int gx,
            int gz, int fromY, int probeFromY, int& outY, float& extraCost) {
  if (!s.In(gx, gz)) return false;
  int32_t y = 0;
  const uint8_t k = Column(s, probe, p, gx, gz, probeFromY, y);
  if (k == kBlockedCol) return false;
  if (k == kUnknownCol) {
    outY = fromY;
    extraCost = 0.0f;
    return true;
  }
  const int rise = (int)y - fromY;
  if (rise > p.maxStepUp) return false;
  if (-rise > p.maxStepDown) return false;
  outY = (int)y;
  extraCost = rise > 0 ? p.climbPenalty * (float)rise
                       : p.dropPenalty * (float)(-rise);
  return true;
}

float Heuristic(int dx, int dz) {
  const int ax = std::abs(dx), az = std::abs(dz);
  const int lo = std::min(ax, az), hi = std::max(ax, az);
  return (float)(hi - lo) + kDiag * (float)lo;
}

// Straight-line walkability using the search's own column cache. Used only by
// the shortcut pass, where the same columns have already been paid for.
bool LineWalkableCached(Scratch& s, const NavProbe& probe, const NavParams& p,
                        Vec3 a, Vec3 b, int probeFromY) {
  const float dx = b.x - a.x, dz = b.z - a.z;
  const float len = std::sqrt(dx * dx + dz * dz);
  const int steps = std::min(96, (int)(len * 2.0f) + 1);  // half-voxel sampling
  if (steps <= 1) return true;
  int prevGx = INT32_MIN, prevGz = INT32_MIN;
  int curY = INT32_MIN;
  for (int i = 0; i <= steps; i++) {
    const float t = (float)i / (float)steps;
    const int wx = ifloor(a.x + dx * t), wz = ifloor(a.z + dz * t);
    const int gx = wx - s.baseX, gz = wz - s.baseZ;
    if (gx == prevGx && gz == prevGz) continue;
    prevGx = gx;
    prevGz = gz;
    if (!s.In(gx, gz)) return false;
    int y = 0;
    float cost = 0;
    if (curY == INT32_MIN) {
      int32_t y0 = 0;
      const uint8_t k = Column(s, probe, p, gx, gz, probeFromY, y0);
      if (k == kBlockedCol) return false;
      curY = k == kOpenCol ? (int)y0 : ifloor(a.y);
      continue;
    }
    if (!StepTo(s, probe, p, gx, gz, curY, probeFromY, y, cost)) return false;
    curY = y;
  }
  return true;
}

}  // namespace

bool FindPath(const NavProbe& probe, const NavParams& p, Vec3 fromVox,
              Vec3 toVox, NavPath& out) {
  out.Clear();
  if (probe.ground == nullptr || probe.blocked == nullptr) return false;

  const int radius = std::clamp(p.radius, 4, 40);
  const int sx = ifloor(fromVox.x), sz = ifloor(fromVox.z);
  Scratch& s = Pad();
  s.Reset(radius, sx, sz);

  // The vertical band every column probe is taken in. Anchored on the START,
  // not on each node, so a column has ONE answer no matter which direction the
  // search reached it from — a cache whose contents depend on approach order is
  // a cache that makes the same fight plan two different paths.
  const int probeFromY = ifloor(fromVox.y) + p.probeUp;

  const int gxStart = radius, gzStart = radius;
  int32_t startY = 0;
  const uint8_t sk = Column(s, probe, p, gxStart, gzStart, probeFromY, startY);
  // A creature standing in a spot the planner calls blocked is not a reason to
  // refuse to plan — it is standing there. Take its own Y as truth.
  const int startHeight =
      sk == kOpenCol ? (int)startY : ifloor(fromVox.y);

  int gxGoal = ifloor(toVox.x) - s.baseX, gzGoal = ifloor(toVox.z) - s.baseZ;
  // A goal outside the grid is clamped to the nearest cell inside it rather
  // than refused: "walk as far toward them as I can see" is the useful answer
  // when a duelist is chasing something that just stepped out of range, and the
  // replan cadence re-asks a few ticks later from further along.
  gxGoal = std::clamp(gxGoal, 0, s.side - 1);
  gzGoal = std::clamp(gzGoal, 0, s.side - 1);

  const int startIdx = s.Idx(gxStart, gzStart);
  const int goalIdx = s.Idx(gxGoal, gzGoal);
  if (startIdx == goalIdx) return false;   // already there; caller steers direct

  // colY of the start is authoritative for the walk that follows.
  s.colY[startIdx] = startHeight;
  if (s.kind[startIdx] == kBlockedCol || s.kind[startIdx] == kUnknownCol)
    s.kind[startIdx] = kOpenCol;

  s.g[startIdx] = 0;
  s.f[startIdx] = Heuristic(gxGoal - gxStart, gzGoal - gzStart);
  HeapPush(s, startIdx);

  int expanded = 0;
  bool reached = false;
  while (!s.heap.empty()) {
    const int cur = HeapPop(s);
    if (s.closed[cur]) continue;
    s.closed[cur] = 1;
    if (cur == goalIdx) {
      reached = true;
      break;
    }
    if (++expanded > p.maxNodes) break;

    const int cgx = cur % s.side, cgz = cur / s.side;
    const int cy = (int)s.colY[cur];
    for (int n = 0; n < 8; n++) {
      const int ngx = cgx + kNbrX[n], ngz = cgz + kNbrZ[n];
      if (!s.In(ngx, ngz)) continue;
      const int ni = s.Idx(ngx, ngz);
      if (s.closed[ni]) continue;

      // No corner cutting: a diagonal is only legal when both of the cardinals
      // it is composed of are legal. Without this a body slips through the
      // shared edge of two blocks, which the collider then refuses, and the mob
      // grinds on a corner while the path insists it is walking.
      if (n >= 4) {
        int tmpY = 0;
        float tmpC = 0;
        if (!StepTo(s, probe, p, cgx + kNbrX[n], cgz, cy, probeFromY, tmpY,
                    tmpC))
          continue;
        if (!StepTo(s, probe, p, cgx, cgz + kNbrZ[n], cy, probeFromY, tmpY,
                    tmpC))
          continue;
      }

      int ny = 0;
      float extra = 0;
      if (!StepTo(s, probe, p, ngx, ngz, cy, probeFromY, ny, extra)) continue;

      const float step = (n >= 4 ? kDiag : 1.0f) + extra;
      const float ng = s.g[cur] + step;
      if (ng >= s.g[ni]) continue;
      s.g[ni] = ng;
      s.came[ni] = cur;
      s.colY[ni] = (int32_t)ny;   // the height REACHED, which is what we walk
      s.f[ni] = ng + Heuristic(gxGoal - ngx, gzGoal - ngz);
      HeapPush(s, ni);
    }
  }

  if (!reached) return false;

  // ---- back-trace, then shortcut ------------------------------------------
  // The raw route is a grid staircase and a creature walking one reads as a
  // robot. String-pulling turns it back into "walk to the corner, then walk to
  // the target": keep an anchor, advance as far as a straight walkable line
  // reaches, emit that point, repeat.
  static std::vector<Vec3> raw;
  raw.clear();
  for (int i = goalIdx; i >= 0; i = s.came[i]) {
    const int gx = i % s.side, gz = i / s.side;
    raw.push_back(Vec3{(float)(s.baseX + gx) + 0.5f, (float)s.colY[i],
                       (float)(s.baseZ + gz) + 0.5f});
    if (i == startIdx) break;
  }
  std::reverse(raw.begin(), raw.end());
  if (raw.size() < 2) return false;

  out.pts.clear();
  size_t anchor = 0;
  out.pts.push_back(raw[0]);
  while (anchor + 1 < raw.size()) {
    // Cap the lookahead: the shortcut test is O(distance) and doing it against
    // every remaining waypoint makes smoothing quadratic in the path length for
    // a route that is already only tens of cells long.
    size_t best = anchor + 1;
    const size_t limit = std::min(raw.size() - 1, anchor + 16);
    for (size_t j = anchor + 2; j <= limit; j++) {
      if (!LineWalkableCached(s, probe, p, raw[anchor], raw[j], probeFromY))
        break;
      best = j;
    }
    out.pts.push_back(raw[best]);
    anchor = best;
  }
  // Drop the start point: the follower steers toward waypoints it has not
  // reached, and the first entry is where it already stands.
  if (out.pts.size() > 1) out.pts.erase(out.pts.begin());
  out.cursor = 0;
  out.valid = !out.pts.empty();
  return out.valid;
}

bool LineWalkable(const NavProbe& probe, const NavParams& p, Vec3 aVox,
                  Vec3 bVox) {
  if (probe.ground == nullptr || probe.blocked == nullptr) return false;
  const float dx = bVox.x - aVox.x, dz = bVox.z - aVox.z;
  const float len = std::sqrt(dx * dx + dz * dz);
  const int steps = std::min(64, (int)len + 1);
  if (steps <= 1) return true;
  const int probeFromY = ifloor(aVox.y) + p.probeUp;
  int prevX = INT32_MIN, prevZ = INT32_MIN;
  int curY = INT32_MIN;
  for (int i = 0; i <= steps; i++) {
    const float t = (float)i / (float)steps;
    const int wx = ifloor(aVox.x + dx * t), wz = ifloor(aVox.z + dz * t);
    if (wx == prevX && wz == prevZ) continue;
    prevX = wx;
    prevZ = wz;
    int y = 0;
    if (!probe.ground(probe.ctx, wx, wz, probeFromY, y)) continue;  // unknown = open
    for (int k = 0; k < p.headroom; k++)
      if (probe.blocked(probe.ctx, wx, y + k, wz)) return false;
    if (curY != INT32_MIN) {
      const int rise = y - curY;
      if (rise > p.maxStepUp || -rise > p.maxStepDown) return false;
    }
    curY = y;
  }
  return true;
}

}  // namespace ai
