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
  void Init(Physics* phys, const std::vector<MaterialDef>& mats);
  void OnMaterialsReloaded(const std::vector<MaterialDef>& mats);
  // Remove all bodies, terrain patches and pending events (world regen).
  void Reset();

  // Register a destruction event; the box is expanded internally and clamped
  // to the bounded fill region (DESIGN.md §7: ~32k voxel abort).
  void AddDestructionEvent(uint32_t tick, IVec3 lo, IVec3 hi);

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
    uint32_t builtVersion = 0;
    uint32_t lastNeeded = 0;
    uint32_t lastRefreshReq = 0;
  };

  bool EventReady(const Event& e, World& world, uint32_t required) const;
  void RunIslandDetection(const Event& e, uint32_t tick, World& world,
                          std::vector<CellOp>& cellOps);
  void ManageTerrain(uint32_t tick, World& world);

  Physics* phys_ = nullptr;
  std::vector<uint32_t> classOf_;
  std::vector<float> densityOf_;
  std::vector<uint32_t> rubbleOf_;
  std::deque<Event> events_;
  std::vector<Body> bodies_;
  std::unordered_map<uint32_t, TerrainEntry> terrain_;
  uint32_t lastCellWriteTick_ = 0;
  bool instancesDirty_ = false;
  uint32_t instanceCount_ = 0;
};
