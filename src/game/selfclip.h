#pragma once
#include <cstdint>
#include <vector>

#include "game/anim.h"   // Quat
#include "math3d.h"

// ============================================================================
// DOES THIS RIG PASS THROUGH ITSELF? (2026-09-21)
//
// A mob's limbs are DELIBERATELY excluded from colliding with each other
// (mob.h MobLimbDef, the ball-joint note: "a mob's limbs are deliberately
// excluded from colliding with each other"), and a live limb is KINEMATIC, so
// Jolt has no opinion about a pose at all. `AnimClampPoseLimits` bounds each
// joint's own range of motion — which is a claim about ONE joint and says
// nothing about whether the shape on the end of it ends up inside the chest.
// So there has never been anything in the engine that could answer "is this
// arm inside the body", and the only detector was a person looking at it.
//
// This is that detector. It is ART-LEVEL, not box-level, and that distinction
// is the whole design:
//
//   * TWO BOXES OVERLAPPING IS NOT CLIPPING. Every limb's box overlaps its
//     parent's at the joint by construction — the shoulder ball IS inside the
//     chest, in the bind pose, in every rig ever authored. A box test reports
//     that as a finding on tick 0 and is useless from then on.
//   * WHAT A PLAYER SEES is solid voxels of one part standing inside solid
//     voxels of another. That is what is counted here: limb A's collider cells
//     whose centres land in an occupied cell of limb B.
//   * ...AND IT IS MEASURED AGAINST THE BIND POSE. The shoulder ball is inside
//     the chest at rest and must stay allowed to be; what is a defect is
//     interpenetration the POSE created. Every pair's count is differenced
//     against the same pair's count with the rig in its rest pose, so the
//     baseline is the art's own and no rig needs a hand-authored allowance.
//     A rig whose arm is modelled half inside the torso is therefore not
//     permanently red, and one whose forearm swings through the ribs is.
//
// COST. The occupancy bitsets are built once per limb and reused (the shape
// does not change while the limb is intact, only the transform), pairs are
// rejected by a model-space AABB first, and only surviving pairs walk voxels.
// On the human rig that is ~15 limbs, ~100 pairs, a handful surviving. Cheap
// enough for a gate to run it on every tick of every authored style; not free
// enough to run it in the frame loop, and nothing does.
//
// PRESENTATION STATE ONLY (CLAUDE.md rule 1). It reads the animation pose and
// the collider lattices, reports numbers, and writes nothing. It cannot move
// the world hash by any route.
// ============================================================================

// A limb's solid shape as a bitset over its OWN lattice. Built once from the
// collider voxels; `scale` is lattice cells per world voxel, which is the
// limb's physScale (mob.h Mob::PhysScaleOf) and not the def's, because a worn
// shell or a held weapon is authored at its own resolution.
struct ClipShape {
  IVec3 dim{};
  float scale = 1.0f;
  std::vector<uint64_t> bits;
  int solid = 0;

  bool Empty() const { return solid == 0; }
  void Alloc(IVec3 d, float s);
  void Set(int x, int y, int z);
  bool At(int x, int y, int z) const {
    if (x < 0 || y < 0 || z < 0 || x >= dim.x || y >= dim.y || z >= dim.z)
      return false;
    const size_t i = ((size_t)z * (size_t)dim.y + (size_t)y) * (size_t)dim.x +
                     (size_t)x;
    return ((bits[i >> 6] >> (i & 63)) & 1ull) != 0ull;
  }
};

// One posed limb, in whatever frame the caller is working in (Mob builds these
// in MODEL space, which is the frame the anim pose is already in and the one a
// failure is readable in — a world-space report would move with the creature).
//
// `corner` is the lattice's (0,0,0) corner, which is `anim.model[i].pos -
// rot * anchorLimb` — exactly what Mob::LimbTargetFor states the relation to
// be, and the same rebasing WeaponStrokePose does for a natural weapon's edge.
struct ClipLimb {
  int slot = -1;
  int parent = -1;
  Vec3 corner{};
  Quat rot{};
  const ClipShape* shape = nullptr;
};

// One pair that met.
struct ClipPair {
  int a = -1, b = -1;
  int voxels = 0;   // solid cells of one inside the other, THIS pose
  int rest = 0;     // ...the same count with the rig in its BIND pose
  int excess = 0;   // voxels - rest, floored at 0: what the pose created
  Vec3 at{};        // centroid of the overlapping cells, the caller's frame
  // ---- ARE THESE TWO ON THE SAME JOINT? ----------------------------------
  //
  // True when one is the other's PARENT — and only then, never for a
  // grandparent. The distinction decides what a number means, so it is made
  // by the rig rather than guessed by a reader:
  //
  //   * A DIRECTLY JOINTED PAIR SHARES A PIVOT, and every rotation about it
  //     drives one box further into the other. A shoulder abducted 80 degrees
  //     genuinely buries more of the upper arm in the chest than the bind pose
  //     does, and the bind-pose baseline cannot subtract that because the bind
  //     pose has the joint at rest. That is a fact about round shapes meeting
  //     at a hinge, not a limb passing through a limb.
  //   * TWO HOPS IS ALREADY A DIFFERENT CLAIM. The forearm and the chest do
  //     not share a joint, and there is no pose of the shoulder or the elbow
  //     that legitimately puts one inside the other. Neither do a sword and a
  //     forearm. Those are the findings this detector exists for, and they
  //     must not be excused by a rule written for the wrist.
  bool jointed = false;
};

struct ClipReport {
  int limbs = 0;
  int pairsTested = 0;   // pairs that survived the AABB reject and walked voxels
  int pairsHit = 0;      // ...with excess > 0
  // The worst pair that does NOT share a joint — the real finding, and what a
  // gate states its claim on.
  int worstExcess = 0;
  ClipPair worst{};
  // ...and the worst that does, reported separately rather than dropped: a
  // shoulder driven right through the chest is still worth seeing, it simply
  // cannot be held to the same number.
  int worstJointExcess = 0;
  ClipPair worstJoint{};
  // Every pair with excess > 0, worst first, capped. A cap rather than a
  // vector that can grow without bound because a badly broken pose puts every
  // limb inside every other one and the useful information is the top of the
  // list either way.
  std::vector<ClipPair> hits;
};

// How many of `a`'s solid cells stand inside a solid cell of `b`. Asymmetric
// by construction (the two lattices need not have the same pitch), so
// RigSelfClip takes the larger of the two directions and says so.
int ClipOverlapCount(const ClipLimb& a, const ClipLimb& b, Vec3* outCentroid);

// THE BIND POSE'S OWN OVERLAP, once. `rest` is the same limbs in the same
// order with the rig in its rest pose; the result is an n*n table indexed
// `i * n + j` that `RigSelfClip` subtracts. Split out and CACHED by the caller
// because it is a fact about the ART — it cannot change while the rig is
// intact, and recomputing it beside every posed tick would triple the cost of
// a gate that replays thousands of them.
void RigClipRestBaseline(const std::vector<ClipLimb>& rest,
                         std::vector<int>& out);

// Every unordered pair of `posed`, differenced against `baseline` when it is
// given (RigClipRestBaseline's table, same order, same slots). Pairs sharing a
// joint are tested like any other: the bind-pose baseline is what makes them
// meaningful rather than permanently red.
//
// `maxHits` caps ClipReport::hits; the tallies are complete regardless.
void RigSelfClip(const std::vector<ClipLimb>& posed,
                 const std::vector<int>* baseline, ClipReport& out,
                 int maxHits = 8);
