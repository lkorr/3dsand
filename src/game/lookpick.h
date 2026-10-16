// lookpick.h — WHAT G TAKES: the forgiving pick under the reticle.
//
// THE COMPLAINT (owner ticket 2026-10-16_11-00-17): "picking up items off of
// the floor can be a nightmare, same with targeting/picking up arrows ... give
// it a little bit of wiggle room ... picking up items isn't reduced to pixel
// perfect clicks". Until this file the take was ONE Jolt ray down the camera
// forward: an item counted only if that infinitely thin line touched its
// collider. A sword lying flat is a voxel tall, an arrow stuck in the ground
// is a voxel wide, and from eye height both are a few pixels across — so the
// prompt flickered on and off as the view breathed, and a press a pixel off
// took nothing. In first person the ray also started at `player.EyePos()`
// while the picture (and its reticle) is drawn from the head-tracked render
// eye, so the line was not even quite through the centre of the screen.
//
// THE RULE (Skyrim/Oblivion-style activation, which is a cone, not a ray):
//
//   1. THE CENTRE RAY STARTS AT THE RENDER EYE (the caller passes last frame's
//      — the eye the reticle's picture was drawn from) along the camera
//      forward. Reach is still measured from the HEAD (`hand`), so arm's
//      length stays arm's length in third person.
//   2. EVERY PICKABLE within reach is a candidate: each ground item (the
//      WorldItems registry: dropped gear, arrows stuck in the ground, the
//      arrow on a severed arm) and each arrow embedded in a creature this
//      machine owns (IsEmbeddedArrowBody's rule). A candidate's ANGLE is the
//      angle off the centre ray of the nearest point of its box to that ray —
//      0 when the ray passes through the box at all. The box is the body's
//      OWN collider bounds, posed (an oriented box), not its world AABB: a
//      diagonal arrow stays a thin stick instead of a square of floor.
//   3. A candidate counts inside a CONE around the reticle: `coneDeg`, or the
//      wider `coneSmallDeg` for a THIN thing (its box's middle extent under
//      `smallSize` voxels — an arrow, a blade lying flat) because a thin thing
//      is the one a fixed cone would still leave a pixel hunt.
//   4. Best = smallest angle; distance breaks near-ties (`distWeightDeg` per
//      voxel, so ~1 deg over the whole reach: two things equally on the
//      reticle go to the nearer one, a nearer thing never steals the one you
//      are pointing at).
//   5. NOT THROUGH WALLS. The best candidate is taken only if the eye can see
//      it: no SOLID cell (passable vegetation and liquid do not block — an
//      item lying in tall grass or a pond is still taken) and no other body
//      between the eye and one of three points on it (the point nearest the
//      ray, the box centre, the box top). The arrow's own host does not hide
//      the arrow standing out of it. Otherwise the next best is tried.
//   6. WHAT THE BARE RAY HIT still competes when it is something G acts on
//      that is not a pickable (a lootable corpse: `exactActionable`), at
//      `exactBiasDeg` rather than 0 — so the corpse under the reticle wins
//      over a sword on the floor beside it, but an arrow standing out of that
//      corpse within the bias is the arrow.
//
// THE PROMPT AND THE PRESS ARE THE SAME ANSWER: main.cpp runs this once per
// frame into `lookBody`, the prompt is built from `lookBody`, and the tap
// takes `lookBody`. There is no second query on the press that could
// disagree with what the screen promised.
//
// A UI QUERY, NOT A SIM INPUT: it reads Jolt bodies and the CPU mirror, writes
// nothing, and the take it leads to is the same call it always was.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "math3d.h"
#include "sim/world.h"   // CellKind, IVec3

class Physics;
class MobSystem;
class WorldItems;

namespace lookpick {

enum class Kind : uint8_t {
  None,
  GroundItem,      // a WorldItems entry (dropped, stuck in the ground, on a severed arm)
  EmbeddedArrow,   // an arrow slot on a creature (IsEmbeddedArrowBody)
  Exact,           // the bare centre ray's body, `exactActionable` said yes
};

struct Params {
  float reach = 24.0f;          // voxels, head -> nearest point of the thing
  float coneDeg = 6.0f;         // half-angle around the reticle
  float coneSmallDeg = 10.0f;   // ...for a thin thing
  float smallSize = 3.0f;       // voxels: box middle extent below this = thin
  float distWeightDeg = 0.04f;  // score degrees per voxel of distance
  float exactBiasDeg = 1.5f;    // what the bare ray hit scores, if actionable
  int maxLosTests = 8;          // candidates tried for line of sight, best first
};

struct Query {
  Vec3 from{};   // the render eye (where the reticle's picture was drawn from)
  Vec3 dir{};    // the camera forward (normalised here)
  Vec3 hand{};   // the head: reach is measured from it
  const std::vector<uint64_t>* ignore = nullptr;   // the asker's own bodies
  // Terrain. Only Solid blocks (Unknown is passable, as for projectiles).
  // Null = no terrain occlusion.
  std::function<CellKind(IVec3)> kindAt;
  // Is the body the bare centre ray landed on something G acts on although
  // it is not a pickable (a corpse with loot)? Null = nothing is.
  std::function<bool(uint64_t)> exactActionable;
};

struct Result {
  uint64_t body = 0;   // the pick (0 = nothing in the cone that can be seen)
  Kind kind = Kind::None;
  float angleDeg = 0.0f, dist = 0.0f;
  // What the bare centre ray hit within reach and not behind rock, whatever
  // it is (0 = nothing).
  // The caller falls back to it when `body` is 0, for the prompts that are
  // about the thing exactly under the reticle (the grab, a bare corpse).
  uint64_t exactBody = 0;
  // Attribution, for a gate line: candidates in reach, in the cone, and how
  // many of the best were refused for line of sight.
  int inReach = 0, inCone = 0, occluded = 0;
};

Result Pick(const Query& q, const Params& p, const Physics& phys,
            MobSystem& mobs, const WorldItems& ground);

// ---- the geometry, exposed for the gate ------------------------------------
// The angle (degrees) off the ray (from, unit dir, length maxT) of the point
// of box [lo, hi] nearest the ray; `q` gets that point. 0 when the ray passes
// through the box. Returns a negative number when the box is wholly behind
// the eye.
float BoxAngleDeg(Vec3 from, Vec3 dir, float maxT, Vec3 lo, Vec3 hi, Vec3& q);
// True when a Solid cell lies on the segment from `a` to `b`, not counting the
// last `stopShort` voxels (the floor a thing lies on is not in its way).
bool SegmentBlocked(Vec3 a, Vec3 b, float stopShort,
                    const std::function<CellKind(IVec3)>& kindAt);

}  // namespace lookpick
