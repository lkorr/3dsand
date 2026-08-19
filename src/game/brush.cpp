#include "game/brush.h"

#include <algorithm>

bool Brush::BuildOp(const WorldSnapshot& snap, const Vec3& eye, const Vec3& fwd,
                    bool erase, BrushOp& out) const {
  int r = std::clamp(radius, 1, 7);
  IVec3 target;
  if (snap.valid && snap.pick[0] != 0) {
    if (erase) {
      target = {(int)snap.pick[2], (int)snap.pick[3], (int)snap.pick[4]};
    } else {
      // last empty cell before the hit, nudged up so spheres sit on surfaces
      target = {(int)snap.pick[5], (int)snap.pick[6] + r / 2, (int)snap.pick[7]};
    }
  } else {
    // nothing under the crosshair: paint in the air ahead
    Vec3 p = eye + fwd * 48.0f;
    target = {ifloor(p.x), ifloor(p.y), ifloor(p.z)};
    if (erase) return false;
  }
  out = BrushOp{target.x, target.y, target.z, r,
                erase ? kMatAir : material,
                erase ? 1u : 0u, 0, 0};
  return true;
}
