#pragma once
#include <cstring>
#include <vector>

#include "math3d.h"
#include "phys/physics.h"
#include "sim/microbody.h"
#include "sim/world.h"

// Shared GPU-append walks for the two rigs: MobSystem's Mob/Limb tree and
// PlayerAvatar's flat Part list. Both feed the same four buffers with the same
// packing, and both used to carry their own copy of each walk — four pairs of
// functions that were identical apart from the loop header and which had
// already drifted once (the avatar's debug boxes lost the comment explaining
// the centre-of-mass offset that the mob's kept).
//
// THE CONTRACT, and the reason these four belong together: all four walks must
// visit slots in the SAME ORDER, because the slot a transform lands in
// (AppendXforms) is the slot an instance records (AppendInstances,
// AppendMicroInsts, AppendDebugBoxes). A slot consumed by one walk and skipped
// by another shifts every limb after it onto the wrong transform. That is why
// `slot++` happens for every part with a body even when the part draws
// nothing, and why the skip conditions below are expressed as "what to draw",
// never as "whether to advance".
//
// Each caller supplies its own iteration (flat vs nested) by calling these per
// part, and its own policy through the arguments — the avatar's hidden-part
// mask and the mob's flipbook frame swap are real behavioural differences, not
// duplication, so they stay at the call sites.

namespace rigrender {

// One slot's worth of the cube-instance pass. `voxels` is the voxel list to
// draw, already resolved by the caller (the mob swaps in a flipbook frame).
// Returns false once the instance buffer is full.
inline bool AppendVoxInsts(std::vector<BodyVoxInst>& out, uint32_t slot,
                           const std::vector<DebrisVoxel>& voxels) {
  for (const DebrisVoxel& v : voxels) {
    if (out.size() >= kMaxBodyVoxInstances) return false;
    out.push_back({(float)v.x, (float)v.y, (float)v.z,
                   (uint32_t)v.payload | (slot << 16)});
  }
  return true;
}

// One slot's worth of the transform pass.
inline void AppendXform(std::vector<BodyXformGpu>& out,
                        const BodyTransform& xf) {
  BodyXformGpu x{};
  x.pos[0] = xf.pos.x;
  x.pos[1] = xf.pos.y;
  x.pos[2] = xf.pos.z;
  std::memcpy(x.quat, xf.quat, sizeof(x.quat));
  out.push_back(x);
}

// One collision box, in world space.
//
// The bounds come from Physics::GetLocalBounds, i.e. from the JOLT SHAPE, not
// from the voxel list that built it. That is the whole point of the overlay:
// the collider is a greedy box merge of those voxels (capped, and inflated by a
// convex radius), so drawing the voxels back would show what we MEANT to build
// while this shows what is actually collided against. When they disagree, that
// disagreement is the thing you opened the overlay to find.
//
// The shape's local bounds are centred on the body's own origin, which for
// these colliders is the centre of mass — so the box centre is the body
// position plus the bounds' own (usually tiny) offset, rotated into world
// space. Skipping that offset is what makes a wireframe sit a fraction off the
// limb it belongs to.
inline void AppendDebugBox(std::vector<DebugBox>& out, const Vec3& lo,
                           const Vec3& hi, const BodyTransform& xf,
                           uint32_t color) {
  DebugBox b{};
  const Vec3 mid{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f,
                 (lo.z + hi.z) * 0.5f};
  const Quat q{xf.quat[0], xf.quat[1], xf.quat[2], xf.quat[3]};
  const Vec3 c = xf.pos + QuatRotate(q, mid);
  b.pos[0] = c.x; b.pos[1] = c.y; b.pos[2] = c.z;
  b.half[0] = (hi.x - lo.x) * 0.5f;
  b.half[1] = (hi.y - lo.y) * 0.5f;
  b.half[2] = (hi.z - lo.z) * 0.5f;
  std::memcpy(b.quat, xf.quat, sizeof(b.quat));
  b.color = color;
  out.push_back(b);
}

// One body's worth of the overlay, preferring the COMPOUND's individual
// sub-shapes over the whole-shape AABB.
//
// A limb collider is a greedy box merge, so a single AABB around the compound
// is a strictly worse answer than the boxes themselves: it is the bound of what
// Jolt collides against rather than the thing. Drawing the sub-shapes is what
// makes the overlay able to show a merge that went wrong. Non-compound bodies
// (spheres) have no sub-shapes to walk and fall back to AppendDebugBox above.
//
// `limit` is the caller's total cap; the sub-shape walk is asked for no more
// than the remaining room so one many-boxed limb cannot blow past it.
inline void AppendDebugBoxesFor(std::vector<DebugBox>& out, Physics& phys,
                                uint64_t body, const BodyTransform& xf,
                                size_t limit, uint32_t color,
                                std::vector<SubShapeBox>& scratch) {
  const Quat bodyQ{xf.quat[0], xf.quat[1], xf.quat[2], xf.quat[3]};
  scratch.clear();
  if (phys.GetSubShapeBoxes(body, scratch, limit - out.size())) {
    for (const SubShapeBox& ss : scratch) {
      DebugBox b{};
      const Vec3 c = xf.pos + QuatRotate(bodyQ, ss.center);
      b.pos[0] = c.x; b.pos[1] = c.y; b.pos[2] = c.z;
      b.half[0] = ss.halfExtents.x;
      b.half[1] = ss.halfExtents.y;
      b.half[2] = ss.halfExtents.z;
      const Quat q = QuatNormalize(
          QuatMul(bodyQ, Quat{ss.quat[0], ss.quat[1], ss.quat[2], ss.quat[3]}));
      b.quat[0] = q.x; b.quat[1] = q.y; b.quat[2] = q.z; b.quat[3] = q.w;
      b.color = color;
      out.push_back(b);
    }
    return;
  }
  Vec3 lo, hi;
  if (!phys.GetLocalBounds(body, lo, hi)) return;
  AppendDebugBox(out, lo, hi, xf, color);
}

}  // namespace rigrender
