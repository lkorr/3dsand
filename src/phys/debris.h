#pragma once
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "math3d.h"
#include "phys/physics.h"
#include "sim/materials.h"
#include "sim/world.h"

// Debris pipeline (DESIGN.md §7, Grimorium devlog mWdlTZ_FoBc):
//   destruction event -> async region readback -> bounded island detection ->
//   islands leave the grid via exact-cell MutationQueue ops and become Jolt
//   rigidbodies carrying their voxel payload; sub-8-voxel islands crumble to
//   their powder ("rubble") form and stay in the CA. Bodies collide against
//   localized marching-cubes terrain meshes cached per chunk and invalidated
//   from the dirty-flag snapshot.
//
// Determinism note: bodies are CPU-float gameplay state, outside the hashed
// grid domain by design. Their grid interactions travel exclusively through
// the op stream, so recording that stream still replays the grid exactly.

// GPU instance layouts — must match debris.wgsl.
struct BodyVoxInst {
  float lx, ly, lz;   // body-local voxel min corner
  uint32_t packed;    // bits 0..15 payload, bits 16..27 body slot
};
struct BodyXformGpu {
  float pos[3];
  float pad = 0;
  float quat[4];
};
constexpr uint32_t kMaxBodyVoxInstances = 262144;
constexpr uint32_t kMaxBodies = 200;

class DebrisSystem {
 public:
  void Init(Physics* phys, World* world, const std::vector<MaterialDef>& mats);
  void OnMaterialsReloaded(const std::vector<MaterialDef>& mats);
  // Remove all bodies, terrain patches and pending events (world regen).
  void Reset();

  // Register a destruction event; the box is expanded by `margin` and clamped
  // to the bounded fill region (DESIGN.md §7: ~32k voxel abort). Returns false
  // if the event queue is full (caller may retry next tick).
  bool AddDestructionEvent(uint32_t tick, IVec3 lo, IVec3 hi, int margin = 10);

  // Convert the snapshot's GPU support-loss flags (sim_step saw a supporting
  // voxel vacate next to a solid) into island-check events, per-chunk
  // cooldown'd and drained a few per tick from PreTick. This is what catches
  // floating structures whose support was removed by the CA itself — burnt
  // stems, dissolved rock, sand flowing out from under a slab — which no
  // explosion or brush event covers.
  void QueueSupportEvents(const WorldSnapshot& snap);

  // Once per tick BEFORE SubmitTick: requests chunk fetches, runs any ready
  // island detections (appends exact-cell ops), maintains terrain collision
  // meshes around live bodies. `cellOps` must be submitted this tick.
  void PreTick(uint32_t tick, World& world, std::vector<CellOp>& cellOps);

  // Once per tick AFTER Physics::Step: refresh transforms, cull fallen /
  // excess bodies.
  void PostStep();

  // Render plumbing. Instances change only when bodies spawn/despawn.
  bool InstancesDirty() const { return instancesDirty_; }
  void BuildInstances(std::vector<BodyVoxInst>& out);  // clears dirty flag
  void BuildXforms(std::vector<BodyXformGpu>& out) const;
  uint32_t InstanceCount() const { return instanceCount_; }
  uint32_t BodyCount() const { return (uint32_t)bodies_.size(); }
  uint32_t ActiveBodyCount() const;
  uint32_t PendingEvents() const { return (uint32_t)events_.size(); }

 private:
  struct Event {
    uint32_t tick;
    IVec3 lo, hi;  // voxel box (inclusive), already expanded + clamped
  };
  struct Body {
    uint64_t handle = 0;
    std::vector<DebrisVoxel> voxels;
    BodyTransform xf{};
    float radiusVoxels = 0;
  };
  struct TerrainEntry {
    uint64_t handle = 0;
    IVec3 wc{};          // world chunk (streaming recycles slots, not chunks)
    uint32_t builtVersion = 0;
    uint32_t lastNeeded = 0;
    uint32_t lastRefreshReq = 0;
  };

  bool EventReady(const Event& e, World& world, uint32_t required) const;
  void RunIslandDetection(const Event& e, uint32_t tick, World& world,
                          std::vector<CellOp>& cellOps);
  void ManageTerrain(uint32_t tick, World& world);

  Physics* phys_ = nullptr;
  World* world_ = nullptr;
  std::vector<uint32_t> classOf_;
  std::vector<float> densityOf_;
  std::vector<uint32_t> rubbleOf_;
  std::deque<Event> events_;
  // support-loss plumbing: flagged chunks wait here until the event queue has
  // room (never dropped — a missed final event is a floating island forever).
  // Keyed by WORLD chunk: the window can shift before a flag drains.
  std::deque<IVec3> pendingSupport_;
  std::unordered_map<uint64_t, uint8_t> supportPending_;    // dedup (packed key)
  std::unordered_map<uint64_t, uint32_t> supportCooldown_;  // chunk -> last tick
  uint32_t lastSupportSnapTick_ = 0;
  std::vector<Body> bodies_;
  std::unordered_map<uint64_t, TerrainEntry> terrain_;  // packed world chunk key
  uint32_t lastCellWriteTick_ = 0;
  bool instancesDirty_ = false;
  uint32_t instanceCount_ = 0;
};
