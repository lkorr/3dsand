#pragma once
#include <cstdarg>
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
               PlayerAvatar* avatar /*nullable*/,
               const MicroBodySet* microSet = nullptr)
      : debris_(debris), mobs_(mobs), avatar_(avatar), microSet_(microSet) {}

  // Per-slot transforms, refreshed every frame (cheap).
  void BuildXforms(std::vector<BodyXformGpu>& out) const;
  // Per-voxel cube instances. Non-const: clears each system's dirty flag, so
  // call it only when AnyInstancesDirty() (or on a one-shot harness path).
  void BuildInstances(std::vector<BodyVoxInst>& out);
  // Compacted micro-body draw list; `out.size()` IS the draw's instance count
  // and an empty result means the pass is skipped entirely (sim/microbody.h).
  void BuildMicroInsts(std::vector<MicroBodyInstGpu>& out) const;

  // ---- the brick-record audit ----------------------------------------------
  // "Does any record have two holders, or a holder pointing at a record that
  // has already been freed?" Returns the number of faults reported; 0 is
  // clean. Cheap when clean (one index sweep, no strings), so the frame loop
  // can call it every tick — see the two-phase note in the .cpp.
  //
  // Needs `microSet` in the constructor; without one it is a no-op.
  uint32_t AuditMicroModels() const;

  // ---- the teardown a model-table rebuild REQUIRES ---------------------------
  // A micro model index is a POSITION in MicroBodySet::models, and LoadMobDefs
  // rebuilds that vector wholesale on every asset reload. Anything still
  // holding an index across that line draws somebody else's brick — and frees a
  // record it does not own when it finally lets go. The holder populations are
  // exactly the three the audit above walks, which is why this lives here and
  // not at the reload site: a fourth system added to the walk is a fourth
  // system that has to be dropped, and the two lists cannot drift if they are
  // the same list. Call immediately BEFORE the set is replaced, never after.
  void ReleaseMicroHolders();
  // How many holders of a model index exist right now, across every system.
  // ZERO IS THE ONLY STATE IN WHICH THE MODEL TABLE MAY BE REBUILT, so this is
  // both what ReleaseMicroHolders establishes and what the `limb-alias` gate
  // asserts about it — a count, unlike the audit, is meaningful when the fault
  // has not happened yet.
  uint32_t MicroHolderCount() const;

  // Total slots the walk currently occupies (== xform count).
  uint32_t TotalSlots() const;
  // Any system's instance list changed since it was last built. Slot bases
  // shift when ANY system's population changes, so one flag serves all three.
  bool AnyInstancesDirty() const;

 private:
  // Rate-limited fault report: stderr AND build/microbody_audit.log, one line
  // per distinct message ever (a body-swap persists, so an unthrottled report
  // is thousands of identical lines a second).
  void Report(const char* fmt, ...) const;

  DebrisSystem& debris_;
  MobSystem& mobs_;
  PlayerAvatar* avatar_;
  const MicroBodySet* microSet_ = nullptr;
  // Audit scratch. A BodyRegistry is constructed per frame at every call site,
  // so these buy nothing across frames — they exist so the audit's cheap phase
  // allocates nothing on the frames where it finds nothing, which is all of
  // them until something is wrong.
  mutable std::vector<uint32_t> scratch_;
  mutable std::vector<MicroHolder> holders_;
};
