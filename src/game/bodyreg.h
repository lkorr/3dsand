#pragma once
#include <cstdarg>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "game/avatar.h"
#include "game/mob.h"
#include "phys/debris.h"
#include "sim/microbody.h"

// ---- BodyInstanceArena: the cube-instance buffer as per-body SPANS ----------
//
// (rigidbody perf package, 2026-09-28.) BuildInstances emits one list for
// every body in the world, and it used to be uploaded WHOLE whenever any one
// body's flag went up — a burning body flags every tick, so a burning log
// re-sent every other body's instances with it (up to 4 MiB), and the draw
// was one Draw over all of them wherever they were.
//
// The arena keeps a CPU mirror of the GPU buffer in which each body (keyed by
// its physics handle, the one identity that survives a slot shift) owns a
// span with slack. Commit() takes the freshly built list, compares each
// body's run with what its span already holds and records only the
// instances that differ, so a burning body re-sends its own changed range and
// nothing else. A span that outgrows its slack moves to the end; holes are
// reclaimed by a full repack when they outweigh the live data. A span whose
// body is absent this commit is KEPT (not drawn) for kKeepCommits commits, so
// a mask toggle (the inventory portrait unhiding the avatar's torso) finds
// its old span and costs no upload at all.
//
// Ranges() is one entry per drawn slot with its local AABB, which is what
// CullBodyRanges needs to give each body its own frustum test. Everything
// here is RENDER-ONLY: no sim state, nothing hashed.
struct BodyDrawRange {
  uint32_t first = 0, count = 0, slot = 0;
  float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};  // body-local cube AABB
};
class BodyInstanceArena {
 public:
  // `inst` is BuildInstances' output (runs of one slot each, walk order);
  // `handles[s]` the physics body drawn at slot s (BuildHandles).
  void Commit(const std::vector<BodyVoxInst>& inst,
              const std::vector<uint64_t>& handles);
  // The draw list of the last Commit.
  const std::vector<BodyDrawRange>& Ranges() const { return ranges_; }
  // Instance ranges [lo, hi) of the mirror that changed since the last
  // TakeUploads, sorted and coalesced. The caller WriteBuffers each one from
  // Data() and the list is cleared.
  void TakeUploads(std::vector<std::pair<uint32_t, uint32_t>>& out);
  const BodyVoxInst* Data() const { return mirror_.data(); }
  uint32_t End() const { return end_; }
  // Instances refused because the buffer (kMaxBodyVoxInstances) was full even
  // after a repack — bodies that are NOT drawn. Last commit's count.
  uint32_t Dropped() const { return dropped_; }
  uint32_t Repacks() const { return repacks_; }
  // Forget everything; the next Commit re-sends all of it (device reset,
  // reload).
  void Reset();

 private:
  struct Span {
    uint32_t off = 0, cap = 0, count = 0, lastSeen = 0;
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};  // cached with the content
  };
  static constexpr uint32_t kKeepCommits = 120;
  bool CommitPass(const std::vector<BodyVoxInst>& inst,
                  const std::vector<uint64_t>& handles, bool repacking);
  void MarkDirty(uint32_t lo, uint32_t hi);

  std::vector<BodyVoxInst> mirror_;
  std::unordered_map<uint64_t, Span> spans_;  // keyed by physics handle
  std::vector<std::pair<uint32_t, uint32_t>> dirty_;
  std::vector<BodyDrawRange> ranges_;
  uint32_t end_ = 0, commit_ = 0, dropped_ = 0, repacks_ = 0;
  uint32_t lastDropped_ = 0;
};

// Per-body frustum cull of the arena's ranges against one camera (the main
// view or the portrait). Each range's local AABB is carried into world space
// as a bounding sphere through its slot's transform and tested against the
// four side planes and the near plane. Visible runs that sit back to back in
// the buffer are merged into one draw. `draws` gets [first, count] pairs.
void CullBodyRanges(const std::vector<BodyDrawRange>& ranges,
                    const std::vector<BodyXformGpu>& xforms, const Vec3& eye,
                    const Vec3& fwd, const Vec3& right, const Vec3& up,
                    float tanHalfFovY, float aspect,
                    std::vector<std::pair<uint32_t, uint32_t>>& draws,
                    uint32_t* instancesDrawn = nullptr);

// BodyRegistry — owns the ONE definition of the body GPU slot-space.
//
// Debris bodies take slots [0, D), the player avatar's parts stack after, and
// mob limbs (the dead included) stack after those — the avatar ahead of the
// mobs so a crowd of corpses can never be what starves the player's own body
// at kMaxBodySlots (bodyreg.cpp). Three parallel arrays are indexed by that
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
  // The physics handle of every slot BuildXforms writes, same walk, same
  // length: `out[s]` is the body drawn at bodyXforms[s]. What the death
  // portrait matches its frozen pose by — the player's corpse is a dead Mob
  // (mob slots), its severed parts are debris (debris slots).
  void BuildHandles(std::vector<uint64_t>& out) const;
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
  mutable std::vector<uint32_t> ids_;
  mutable std::vector<MicroHolder> holders_;
};
