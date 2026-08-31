#pragma once
#include <cstdint>
#include <vector>

#include "math3d.h"

// LOCAL COMBAT NAVIGATION — A* over walkable COLUMNS inside the CPU mirror.
//
// WHAT THIS IS FOR, AND WHAT IT DELIBERATELY IS NOT. This is the "get around
// the rock between me and the thing I am fighting" planner. Its whole world is
// the few dozen voxels a creature can actually see, because that is all the CPU
// mirror holds: `World::Cached` covers the chunks the terrain anchors keep
// fetched, and past that every probe reports UNKNOWN. A long-range planner
// (roads, regions, a persistent graph) is a different system with a different
// data source; the intent layer above this one takes a GOAL POINT, so that
// planner slots in later as another producer of goals rather than as a rewrite
// of this file.
//
// THE THREE RULES THIS PLANNER IS BUILT AROUND
//
//   1. UNKNOWN IS WALKABLE. The probe reaches past the mirror long before it
//      reaches anything interesting, and a planner that treats "I cannot see
//      it" as "there is a wall" builds a cage around the creature out of its
//      own ignorance. That is the mob-scale twin of the projectile rule in
//      CLAUDE.md, and it already bit this engine once at the steering layer
//      (DESIGN.md, "Unknown footing must read as WALKABLE"). An unknown column
//      inherits the height of the column the search reached it from, so the
//      path stays continuous and the DRIVE's own ground check — which does
//      require real footing — is what stops anyone walking off into space.
//
//   2. THE PROBE IS INJECTED, NOT REIMPLEMENTED. "Where is the ground under
//      this column" already has exactly one implementation in this engine
//      (`Mob::GroundHeightAt`), and a planner with a second copy of it is a
//      planner that eventually disagrees with the locomotion it is steering.
//      So `NavProbe` is a pair of C function pointers plus a context; the
//      caller binds the real thing. Function pointers rather than
//      std::function because the search calls them thousands of times per
//      replan and there is nothing to capture that a void* cannot carry.
//
//   3. COST SCALES WITH ACTIVITY (CLAUDE.md rule 2). A path is planned on a
//      CADENCE, never per tick, and every search is bounded twice: by
//      `maxNodes` expansions and by the grid's own extent. Column heights are
//      memoized in the grid, so the expensive part — the downward scan through
//      the chunk cache — is paid at most once per column per replan even
//      though eight neighbours ask about it.
//
// UNITS. Everything here is WORLD VOXELS, integer for columns and float for
// waypoints, in the same frame as `Mob::origin_`. Nothing in this file is
// hashed: navigation is CPU-float gameplay state exactly like the gait, and it
// reaches the world only by changing which direction a mob walks.

namespace ai {

// A planned route: a polyline of world-voxel waypoints, already shortcut.
//
// `cursor` is the waypoint currently being walked toward. The follower advances
// it on arrival rather than the planner pre-splitting the path into steps, so a
// mob shoved sideways mid-path steers back to the same waypoint instead of
// silently skipping it.
struct NavPath {
  std::vector<Vec3> pts;
  size_t cursor = 0;
  bool valid = false;

  void Clear() {
    pts.clear();
    cursor = 0;
    valid = false;
  }
  bool Done() const { return !valid || cursor >= pts.size(); }
  const Vec3& Current() const { return pts[cursor]; }
  // The end of the route as planned — NOT the live goal. Comparing the two is
  // how the follower notices the target has walked away from the path it was
  // given (see NavPathStale).
  Vec3 Goal() const { return pts.empty() ? Vec3{} : pts.back(); }
};

// The terrain queries the search needs, bound to whatever owns the mirror.
//
// `ground` returns FALSE for unknown, which the search reads as "walkable at
// the height I came from" (rule 1 above) — it must NOT be conflated with
// "blocked". `blocked` answers the headroom question and must likewise report
// unknown space as clear.
struct NavProbe {
  bool (*ground)(void* ctx, int x, int z, int yFrom, int& outY) = nullptr;
  bool (*blocked)(void* ctx, int x, int y, int z) = nullptr;
  void* ctx = nullptr;
};

// Search shape. Defaults match what the existing locomotion can physically do:
// `SenseGround` calls a rise of more than 2 voxels unclimbable, so a planner
// that routes over a 3-voxel step hands the drive a path it will refuse to
// walk — the mob then grinds against the step forever while the planner insists
// the way is clear. Keeping these two numbers agreed is load-bearing.
struct NavParams {
  int radius = 24;        // half-extent of the local grid, world voxels
  int maxStepUp = 2;      // must not exceed what DriveLocomotion will climb
  int maxStepDown = 5;    // a drop is survivable; a big one is merely disliked
  int headroom = 3;       // clear voxels a body needs above the ground
  // How far ABOVE the start's own ground a column probe begins. The ground
  // query scans downward a fixed depth, so this plus that depth is the
  // vertical band the planner can see at all; outside it a column reads as
  // unknown and therefore (rule 1) as walkable at the inherited height. For
  // local combat nav a band of roughly +-12 voxels is the whole fight.
  int probeUp = 12;
  int maxNodes = 1200;    // expansion budget; the hard bound on one search
  float dropPenalty = 0.35f;  // extra cost per voxel of fall, so paths prefer flat
  float climbPenalty = 0.5f;  // ...and per voxel of climb, which costs more
};

// Plan from `fromVox` to `toVox` (world voxels; only X/Z are used to pick
// columns, Y is resolved by the probe). Returns false and leaves `out` invalid
// when the goal is outside the grid, unreachable, or the budget ran out — the
// caller is expected to fall back to direct steering, NOT to stand still.
//
// On success `out.pts` is the SHORTCUT path: the raw grid route with every
// waypoint removed that a straight walkable line could skip. Grid paths are
// staircases and a creature walking one reads as a robot; string-pulling is
// what turns it back into "walk to the corner, then walk to the target".
bool FindPath(const NavProbe& probe, const NavParams& p, Vec3 fromVox,
              Vec3 toVox, NavPath& out);

// Is the straight segment a..b walkable end to end, under the same step rules
// the search used? Exposed because it answers two questions for the caller:
// "do I need a path at all" (if the target is in the open, skip the search
// entirely) and "has my path gone stale" (terrain here is destructible, so the
// route planned four ticks ago may now cross a crater).
bool LineWalkable(const NavProbe& probe, const NavParams& p, Vec3 aVox,
                  Vec3 bVox);

}  // namespace ai
