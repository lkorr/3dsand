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
  void Init(Physics* phys, World* world, const std::vector<MaterialDef>& mats,
            const std::vector<ReactionGpu>& reactions);
  void OnMaterialsReloaded(const std::vector<MaterialDef>& mats,
                           const std::vector<ReactionGpu>& reactions);
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
  // island detections (appends exact-cell ops), burns bodies, maintains
  // terrain collision meshes around live bodies. `cellOps` and `spawns`
  // (body fragments re-entering the world as ballistic voxels) must both be
  // submitted this tick.
  void PreTick(uint32_t tick, World& world, std::vector<CellOp>& cellOps,
               std::vector<ParticleSpawn>& spawns);

  // Mob limbs need marching-cubes terrain too: register extra positions for
  // this tick's ManageTerrain sweep (call before PreTick; cleared after).
  void AddTerrainAnchor(Vec3 posVoxel, float radiusVoxels);

  // Take ownership of an existing physics body (severed limb, ragdoll piece):
  // it becomes ordinary debris — culling, despawn, terrain upkeep. Any joints
  // still attached die when the body is eventually removed.
  void AdoptBody(uint64_t handle, std::vector<DebrisVoxel> voxels,
                 const BodyTransform& xf);

  // Laser body cut (PLAN §C2): partition a body's voxels by the world-space
  // plane (point, normal), destroy it, spawn both halves at the same pose
  // with inherited velocity. False when the cut misses or a half is too
  // small to be worth a body (< 4 voxels).
  bool SplitBody(uint64_t handle, Vec3 planePointVoxel, Vec3 planeNormal);

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
  uint32_t SettledBack() const { return settledBack_; }

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
    uint32_t inactiveTicks = 0;  // settle-back countdown (PLAN §B6)
    // body burn (fire continuity on rigidbodies):
    uint32_t serial = 0;          // stable RNG stream id (bodies_ reshuffles)
    uint16_t activeCount = 0;     // voxels with self-driven rules (decay/emit)
    uint16_t pairCount = 0;       // voxels with pair rules (ignitable/dousable)
    uint32_t burnCursor = 0;      // rotating scan window into voxels
    uint32_t burnedSinceRebuild = 0;  // batched collider refresh threshold
    uint32_t burnedSinceShatter = 0;  // batched connectivity re-check
  };
  struct TerrainEntry {
    uint64_t handle = 0;
    IVec3 wc{};          // world chunk (streaming recycles slots, not chunks)
    uint32_t builtVersion = 0;
    uint32_t lastNeeded = 0;
    uint32_t lastRefreshReq = 0;
    uint64_t meshHash = 0;  // collision-surface identity: liquids flowing
                            // through a chunk must not rebuild-and-wake
  };

  bool EventReady(const Event& e, World& world, uint32_t required) const;
  void RunIslandDetection(const Event& e, uint32_t tick, World& world,
                          std::vector<CellOp>& cellOps,
                          std::vector<ParticleSpawn>& spawns);
  void ManageTerrain(uint32_t tick, World& world);
  // Body burn: a CPU mirror of the reaction table over body voxel payloads,
  // so detached matter keeps burning (embers advance to ash, emit real fire
  // into the grid via fill-air-only ops, and grid fire ignites cold bodies
  // through the chunk cache). Idle bodies cost nothing (see impl comment).
  void BurnBodies(uint32_t tick, World& world, std::vector<CellOp>& cellOps,
                  std::vector<ParticleSpawn>& spawns);
  void RecountBurn(Body& b) const;
  bool AnyDirtyNear(const Body& b, const WorldSnapshot& snap, World& world) const;
  // Break a body whose voxels no longer form one 6-connected component: the
  // largest piece keeps the body, fragments >= `minFragment` voxels become
  // bodies of their own while `budget` allows (parent collider rebuilt
  // immediately), everything else re-enters the world as ballistic particles
  // with the body's point velocity — break a body enough and it just turns
  // back into loose voxels. `budget` is decremented per body created and is
  // shared across all bodies in a tick, so a disintegrating object cannot
  // spawn an unbounded fleet of fragments.
  void ShatterBody(Body& b, World& world, std::vector<Body>& fragments,
                   std::vector<ParticleSpawn>& spawns, uint32_t minFragment,
                   uint32_t& budget);
  void VoxelsToParticles(const Body& b, const std::vector<DebrisVoxel>& voxels,
                         Vec3 lin, Vec3 ang, World& world,
                         std::vector<ParticleSpawn>& spawns) const;
  // Settle-back (PLAN §B6): a long-asleep, near-axis-aligned body converts
  // its voxels to CellOps (fill-air-only: grid content wins deterministically
  // on the GPU) and frees its body. At most one body per tick.
  void SettleBodies(uint32_t tick, World& world, std::vector<CellOp>& cellOps);

  Physics* phys_ = nullptr;
  World* world_ = nullptr;
  std::vector<uint32_t> classOf_;
  std::vector<float> densityOf_;
  std::vector<uint32_t> rubbleOf_;
  std::vector<uint8_t> foliageOf_;  // tag:foliage — sub-8 floaters vanish, no rubble
  // body burn tables (rebuilt on materials hot-reload; data-driven, no
  // hardcoded material IDs — the JSON stays the single source of behavior)
  std::vector<MaterialGpu> matGpu_;
  std::vector<ReactionGpu> reactions_;
  std::vector<uint8_t> matSelfActive_;  // material has decay/emit rules
  std::vector<uint8_t> matHasPair_;     // material has pair rules
  uint32_t nextSerial_ = 1;
  std::deque<Event> events_;
  // support-loss plumbing: flagged chunks wait here until the event queue has
  // room (never dropped — a missed final event is a floating island forever).
  // Keyed by WORLD chunk: the window can shift before a flag drains.
  std::deque<IVec3> pendingSupport_;
  std::unordered_map<uint64_t, uint8_t> supportPending_;    // dedup (packed key)
  std::unordered_map<uint64_t, uint32_t> supportCooldown_;  // chunk -> last tick
  uint32_t lastSupportSnapTick_ = 0;
  std::vector<Body> bodies_;
  std::vector<std::pair<Vec3, float>> extraAnchors_;    // mob limbs, this tick
  std::unordered_map<uint64_t, TerrainEntry> terrain_;  // packed world chunk key
  uint32_t lastCellWriteTick_ = 0;
  bool instancesDirty_ = false;
  uint32_t instanceCount_ = 0;
  uint32_t settledBack_ = 0;
};
