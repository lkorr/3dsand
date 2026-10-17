// lookpick.cpp — see lookpick.h.

#include "game/lookpick.h"

#include <algorithm>
#include <cmath>

#include "game/anim.h"   // Quat, QuatRotate / QuatRotateInv
#include "game/mob.h"
#include "game/worlditems.h"
#include "phys/physics.h"

namespace lookpick {

namespace {

constexpr float kRadToDeg = 57.29577951308232f;

Vec3 ClampToBox(Vec3 p, Vec3 lo, Vec3 hi) {
  return Vec3{std::clamp(p.x, lo.x, hi.x), std::clamp(p.y, lo.y, hi.y),
              std::clamp(p.z, lo.z, hi.z)};
}

struct Cand {
  uint64_t body = 0;
  Kind kind = Kind::None;
  Vec3 q{}, ctr{};
  float topY = 0.0f;   // the top of its world box: the third sight point
  float angle = 0.0f, dist = 0.0f, score = 0.0f;
  uint64_t hostMob = 0;   // an embedded arrow's creature: it does not hide the arrow
};

}  // namespace

float BoxAngleDeg(Vec3 from, Vec3 dir, float maxT, Vec3 lo, Vec3 hi, Vec3& q) {
  // THROUGH THE BOX: the slab test. A ray that passes through the box at all
  // is dead on it (angle 0), and the point is where it goes in.
  {
    float t0 = 0.0f, t1 = maxT;
    const float o[3] = {from.x, from.y, from.z}, d[3] = {dir.x, dir.y, dir.z};
    const float bl[3] = {lo.x, lo.y, lo.z}, bh[3] = {hi.x, hi.y, hi.z};
    bool through = true;
    for (int a = 0; a < 3 && through; a++) {
      if (std::fabs(d[a]) < 1e-8f) {
        if (o[a] < bl[a] || o[a] > bh[a]) through = false;
        continue;
      }
      float ta = (bl[a] - o[a]) / d[a], tb = (bh[a] - o[a]) / d[a];
      if (ta > tb) std::swap(ta, tb);
      t0 = std::max(t0, ta);
      t1 = std::min(t1, tb);
      if (t0 > t1) through = false;
    }
    if (through) {
      q = from + dir * t0;
      return 0.0f;
    }
  }
  // BESIDE IT: the box point nearest the ray segment, by alternating
  // projection (segment <-> box, both convex, so it converges on the closest
  // pair; eight rounds is far past where it stops moving for a box this size).
  const Vec3 c = (lo + hi) * 0.5f;
  float t = std::clamp((c - from).dot(dir), 0.0f, maxT);
  for (int i = 0; i < 8; i++) {
    const Vec3 b = ClampToBox(from + dir * t, lo, hi);
    t = std::clamp((b - from).dot(dir), 0.0f, maxT);
  }
  q = ClampToBox(from + dir * t, lo, hi);
  const Vec3 v = q - from;
  const float along = v.dot(dir);
  if (along <= 1e-4f) return -1.0f;   // behind the eye
  const float perp = (v - dir * along).len();
  return std::atan2(perp, along) * kRadToDeg;
}

bool SegmentBlocked(Vec3 a, Vec3 b, float stopShort,
                    const std::function<CellKind(IVec3)>& kindAt) {
  if (!kindAt) return false;
  const Vec3 dv = b - a;
  const float len = dv.len();
  const float end = len - stopShort;
  if (end <= 0.0f) return false;
  const Vec3 dir = dv * (1.0f / len);
  // Amanatides-Woo, ContainerPourPoint's walk: every cell the segment enters
  // after the eye's own, up to `end`.
  IVec3 c{ifloor(a.x), ifloor(a.y), ifloor(a.z)};
  const int step[3] = {dir.x >= 0 ? 1 : -1, dir.y >= 0 ? 1 : -1,
                       dir.z >= 0 ? 1 : -1};
  const float d[3] = {dir.x, dir.y, dir.z};
  const float o[3] = {a.x, a.y, a.z};
  int* ci[3] = {&c.x, &c.y, &c.z};
  float tMax[3], tDelta[3];
  for (int k = 0; k < 3; k++) {
    if (std::fabs(d[k]) < 1e-6f) {
      tMax[k] = tDelta[k] = 1e30f;
      continue;
    }
    const float edge = (float)*ci[k] + (step[k] > 0 ? 1.0f : 0.0f);
    tMax[k] = (edge - o[k]) / d[k];
    tDelta[k] = std::fabs(1.0f / d[k]);
  }
  for (int guard = 0; guard < 512; guard++) {
    const int k = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2)
                                    : (tMax[1] < tMax[2] ? 1 : 2);
    if (tMax[k] >= end) break;
    *ci[k] += step[k];
    tMax[k] += tDelta[k];
    if (kindAt(c) == CellKind::Solid) return true;
  }
  return false;
}

Result Pick(const Query& qu, const Params& p, const Physics& phys,
            MobSystem& mobs, const WorldItems& ground) {
  Result r;
  const float dl = qu.dir.len();
  if (dl < 1e-6f) return r;
  const Vec3 dir = qu.dir * (1.0f / dl);
  const Vec3 from = qu.from, hand = qu.hand;
  const float reach = std::max(0.0f, p.reach);
  // The ray is long enough to reach `reach` past the head from wherever the
  // eye is (the boom is metres behind it in third person).
  const float castLen = reach + (from - hand).len();
  static const std::vector<uint64_t> kNone;
  const std::vector<uint64_t>& ignore = qu.ignore ? *qu.ignore : kNone;
  auto ignored = [&](uint64_t h) {
    return std::find(ignore.begin(), ignore.end(), h) != ignore.end();
  };

  // ---- THE BARE CENTRE RAY: what is exactly under the reticle -------------
  float exactDist = 0.0f;
  {
    float frac = 1.0f;
    const uint64_t hit = phys.CastRayBody(from, dir, castLen, frac, ignore);
    const Vec3 at = from + dir * (frac * castLen);
    // Jolt's dynamic layers do not see the voxel grid, so the bare ray goes
    // through a wall: the grid is asked separately (a corpse behind rock is
    // not "under the reticle").
    if (hit && (at - hand).len() <= reach && !SegmentBlocked(from, at, 0.5f, qu.kindAt)) {
      r.exactBody = hit;
      exactDist = frac * castLen;
    }
  }

  // ---- THE CANDIDATES ------------------------------------------------------
  std::vector<Cand> cands;
  auto consider = [&](uint64_t body, Kind kind, uint64_t hostMob) {
    if (!body || ignored(body)) return;
    // THE BODY'S OWN BOX, in its own frame (collider bounds about the body
    // origin, posed by the body transform): an arrow lying diagonally is a
    // thin stick here, where its world AABB would be a fat square that both
    // reads as "not thin" and claims empty floor around it.
    BodyTransform xf{};
    Vec3 lmin, lmax;
    if (!phys.GetTransform(body, xf) || !phys.GetLocalBounds(body, lmin, lmax))
      return;
    const Quat rq{xf.quat[0], xf.quat[1], xf.quat[2], xf.quat[3]};
    // Within reach of the HEAD at all: the nearest point of the thing.
    const Vec3 handL = QuatRotateInv(rq, hand - xf.pos);
    if ((ClampToBox(handL, lmin, lmax) - handL).len() > reach) return;
    r.inReach++;
    Cand c;
    Vec3 qL;
    c.angle = BoxAngleDeg(QuatRotateInv(rq, from - xf.pos), QuatRotateInv(rq, dir),
                          castLen, lmin, lmax, qL);
    if (c.angle < 0.0f) return;
    c.q = xf.pos + QuatRotate(rq, qL);
    c.ctr = xf.pos + QuatRotate(rq, (lmin + lmax) * 0.5f);
    float mn[3], mx[3];
    c.topY = phys.BodyWorldBoundsM(body, mn, mx) ? mx[1] / kVoxelMeters : c.ctr.y;
    // THIN = the box's MIDDLE extent: an arrow or a blade lying flat is long
    // in one axis and a voxel or two in the other two, whichever way it lies.
    float e[3] = {lmax.x - lmin.x, lmax.y - lmin.y, lmax.z - lmin.z};
    std::sort(e, e + 3);
    const float cone = e[1] < p.smallSize ? p.coneSmallDeg : p.coneDeg;
    if (c.angle > cone) return;
    r.inCone++;
    c.body = body;
    c.kind = kind;
    c.hostMob = hostMob;
    c.dist = (c.q - from).len();
    c.score = c.angle + p.distWeightDeg * c.dist;
    cands.push_back(c);
  };
  for (const WorldItem& w : ground.All()) consider(w.body, Kind::GroundItem, 0);
  // Arrows standing in creatures this machine owns (IsEmbeddedArrowBody's
  // rule: a ghost's arrow is the peer's to pull).
  mobs.ForEachCreature([&](Mob& m) {
    if (!MobSystem::OwnedHere(m)) return;
    for (int li = 0; li < m.LimbCount(); li++)
      if (m.IsArrowSlot(li))
        consider(mobs.LimbBody(m.Id(), li), Kind::EmbeddedArrow, m.Id());
  });
  // What the bare ray landed on, when G acts on it and it is not already a
  // candidate (a corpse with loot): it scores `exactBiasDeg`, not 0, so a
  // pickable right on the reticle beside or in it still wins.
  if (r.exactBody != 0 && qu.exactActionable && qu.exactActionable(r.exactBody)) {
    bool already = false;
    for (const Cand& c : cands) already |= c.body == r.exactBody;
    if (!already) {
      Cand c;
      c.body = r.exactBody;
      c.kind = Kind::Exact;
      c.angle = p.exactBiasDeg;
      c.dist = exactDist;
      c.q = from + dir * exactDist;
      c.score = c.angle + p.distWeightDeg * c.dist;
      cands.push_back(c);
    }
  }
  if (cands.empty()) return r;
  // Score, then the body handle: two candidates never compare equal, so the
  // pick does not depend on the registry's order.
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    if (a.score != b.score) return a.score < b.score;
    return a.body < b.body;
  });

  // ---- LINE OF SIGHT, best first --------------------------------------------
  auto visible = [&](const Cand& c) {
    const Vec3 pts[3] = {c.q, c.ctr,
                         Vec3{c.ctr.x, std::max(c.ctr.y, c.topY - 0.5f), c.ctr.z}};
    for (const Vec3& t : pts) {
      const Vec3 d = t - from;
      const float len = d.len();
      if (len < 1e-3f) return true;
      // Rock: the last voxel before the point is the floor it lies on.
      if (SegmentBlocked(from, t, 1.0f, qu.kindAt)) continue;
      // A body in front of it. Reaching the thing itself, or arriving within
      // a voxel of the point, is seen; so is the creature an arrow stands in.
      float f = 1.0f;
      const uint64_t h = phys.CastRayBody(from, d * (1.0f / len), len, f, ignore);
      if (h == 0 || h == c.body || f * len >= len - 1.0f) return true;
      if (c.hostMob != 0) {
        const Mob* hm = mobs.FindOwner(h);
        if (hm != nullptr && hm->Id() == c.hostMob) return true;
      }
    }
    return false;
  };
  const int tests = std::min((int)cands.size(), std::max(1, p.maxLosTests));
  for (int i = 0; i < tests; i++) {
    const Cand& c = cands[(size_t)i];
    // The bare ray reached the exact body: it is seen by construction.
    if (c.kind != Kind::Exact && !visible(c)) {
      r.occluded++;
      continue;
    }
    r.body = c.body;
    r.kind = c.kind;
    r.angleDeg = c.angle;
    r.dist = c.dist;
    break;
  }
  return r;
}

}  // namespace lookpick
