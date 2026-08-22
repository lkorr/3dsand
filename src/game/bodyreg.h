#pragma once
#include <cstdint>
#include <vector>

#include "game/avatar.h"
#include "game/mob.h"
#include "phys/debris.h"
#include "sim/microbody.h"

// BodyRegistry — owns the ONE definition of the body GPU slot-space.
//
// Debris bodies take slots [0, D), mob limbs stack after, and the player
// avatar's parts stack after those. Three parallel arrays are indexed by that
// slot: the transforms (bodyXforms), the cube instances (bodyInstances) and
// the compacted micro-body draw list. All three MUST agree on the walk order
// and the base offsets, and that agreement used to be spelled out by hand at
// every call site ("mobs.AppendInstances(inst, debris.BodyCount())") — which
// is exactly the sort of thing that rots when a fourth system arrives or one
// call site is updated and another is not. The --shot-mob harness had already
// drifted from the frame loop this way.
//
// So the walk lives HERE and nowhere else: every array indexed by body slot is
// built through this type, and no call site ever computes a slot base again.
//
// PLACEMENT: this lives in game/ (not test/support.*) because it depends on
// game/avatar.h and phys/debris.h — the natural top of that dependency stack —
// and because support.* is the sim/render plumbing shared with the selftest,
// which should CONSUME the slot walk like every other caller rather than own
// it. main.cpp, the shot harnesses and the gates all build through this.
//
// `avatar` is nullable: selftest paths and --shot-mob never spawn one, and the
// slot walk is then identical to what it was before the avatar existed.
class BodyRegistry {
 public:
  BodyRegistry(DebrisSystem& debris, MobSystem& mobs,
               PlayerAvatar* avatar /*nullable*/)
      : debris_(debris), mobs_(mobs), avatar_(avatar) {}

  // Per-slot transforms, refreshed every frame (cheap).
  void BuildXforms(std::vector<BodyXformGpu>& out) const;
  // Per-voxel cube instances. Non-const: clears each system's dirty flag, so
  // call it only when AnyInstancesDirty() (or on a one-shot harness path).
  void BuildInstances(std::vector<BodyVoxInst>& out);
  // Compacted micro-body draw list; `out.size()` IS the draw's instance count
  // and an empty result means the pass is skipped entirely (sim/microbody.h).
  void BuildMicroInsts(std::vector<MicroBodyInstGpu>& out) const;

  // Total slots the walk currently occupies (== xform count).
  uint32_t TotalSlots() const;
  // Any system's instance list changed since it was last built. Slot bases
  // shift when ANY system's population changes, so one flag serves all three.
  bool AnyInstancesDirty() const;

 private:
  DebrisSystem& debris_;
  MobSystem& mobs_;
  PlayerAvatar* avatar_;
};
