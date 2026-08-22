#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

#include "math3d.h"
#include "phys/physics.h"
#include "sim/materials.h"
#include "sim/microbody.h"
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
//
// `packed` bit budget, and why it is laid out this way:
//   0..11   material id (12 bits, the world-cell convention)
//   12..15  state nibble — the cosmetic 3-variant palette index
//   16..27  body slot (12 bits; kMaxBodySlots is 512, so 3 bits spare)
//   28..31  ART COLOUR, 4 bits (0 = unpainted, 1..15 = art slot 1..15)
// The art field is deliberately NARROW here. This is the coincident-skin path
// (skinScale == 1) — plain debris and the one test mob; every real character
// has skinScale > 1 and renders through microbody.wgsl, which carries a full
// 8-bit art channel. Widening this instead would cost a megabyte on a 262144-
// instance buffer for art nobody paints at this resolution, so a model that
// uses more than 15 colours AND renders as cubes gets its extra colours
// clamped, with a warning at load rather than silence.
struct BodyVoxInst {
  float lx, ly, lz;   // body-local voxel min corner
  uint32_t packed;
};
// Art slots representable on the cube path (1..kCubeArtMax).
constexpr uint32_t kCubeArtMax = 15;
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
  //
  // `micro` describes the body's microvoxel rendering, if any (sim/microbody.h).
  // Defaulted, so plain-debris callers are unchanged; a severed micro limb keeps
  // its detail purely by the caller passing what it already knows, with no
  // mob-specific code on this side.
  //
  // `physScale` is the units of `voxels` (the collider lattice). 0 means "same
  // as the skin", which is the pre-split behaviour and correct whenever the two
  // coincide. A body whose skin is FINER than its collider passes the coarser
  // value here and hands over `skinVoxels` — the fine lattice, in skinScale
  // units — which then becomes the authoritative shape for carving.
  void AdoptBody(uint64_t handle, std::vector<DebrisVoxel> voxels,
                 const BodyTransform& xf, MicroBodyRef micro = {},
                 uint32_t physScale = 0,
                 std::vector<PrefabVoxel> skinVoxels = {},
                 uint32_t bleedMat = 0);

  // Laser body cut (PLAN §C2): partition a body's voxels by the world-space
  // plane (point, normal), destroy it, spawn both halves at the same pose
  // with inherited velocity. False when the cut misses or a half is too
  // small to be worth a body (< 4 voxels).
  bool SplitBody(uint64_t handle, Vec3 planePointVoxel, Vec3 planeNormal);

  // ---- direct damage (explosions, laser kerf) --------------------------------
  //
  // Bodies used to be immune to everything except fire: an explosion shoved
  // them as a rigid whole and the laser could only bisect them along a chosen
  // plane. Both now remove actual voxels through the shared DamageBody core,
  // which is the same "erase, re-skin, rebuild collider, maybe shatter" path
  // burning already used — so a blown-apart body splits into real bodies and
  // loose particles for free.

  // Explosion damage: erase body voxels within `radiusVoxels` of the centre,
  // with probability falling off toward the rim so the crater edge is ragged
  // rather than a clean sphere. Ejected voxels become ballistic particles.
  // Runs before the radial impulse so the surviving shape takes the push.
  void DamageBodiesRadial(Vec3 centerVoxel, float radiusVoxels, World& world,
                          std::vector<ParticleSpawn>& spawns);

  // Laser kerf: melt body voxels within `radiusVoxels` of a world point. The
  // beam eats a channel tick by tick and the body splits when the channel
  // actually severs it — no cutting plane is ever chosen (DESIGN.md §8).
  // Returns true when the body was hit. Melted voxels vanish (no particles):
  // a laser vaporizes, and spraying debris from every tick of a held beam
  // would flood the particle ring.
  bool MeltBodyAt(uint64_t handle, Vec3 pointVoxel, float radiusVoxels,
                  World& world, std::vector<ParticleSpawn>& spawns);

  // Micro bodies render from a shared brick pool; damaging one clones its model
  // copy-on-write, so the pool changes and must be re-uploaded. The owner polls
  // this after PreTick and clears it via TakeMicroSet.
  bool MicroDirty() const;
  // The live micro set, so the caller can re-upload it. Non-const because the
  // dirty flag is cleared here.
  MicroBodySet* MicroSet() { return microSet_; }
  // Micro bodies must allocate out of the same set the renderer uploads, so
  // the owner hands it over once at startup. Not owned.
  void SetMicroSet(MicroBodySet* set) { microSet_ = set; }

  // Once per tick AFTER Physics::Step: refresh transforms, cull fallen /
  // excess bodies.
  void PostStep();

  // Render plumbing. Instances change only when bodies spawn/despawn.
  bool InstancesDirty() const { return instancesDirty_; }
  void BuildInstances(std::vector<BodyVoxInst>& out);  // clears dirty flag
  void BuildXforms(std::vector<BodyXformGpu>& out) const;
  // Append this system's micro bodies to the COMPACTED draw list, one entry per
  // micro body, with slot indices matching the transforms BuildXforms writes.
  // Appending straight into the compacted list (rather than building a dense
  // per-slot array to filter afterwards) is what makes a world with no micro
  // bodies cost one loop and no allocation — sim/microbody.h.
  void AppendMicroInsts(std::vector<MicroBodyInstGpu>& out) const;
  uint32_t InstanceCount() const { return instanceCount_; }
  uint32_t BodyCount() const { return (uint32_t)bodies_.size(); }
  // Physics handle of body `i`. Damage rebuilds a body's collider, which
  // REPLACES its handle, so anything holding one across a damage call (the
  // selftest's repeated laser kerf) must re-read it here. 0 if out of range.
  uint64_t BodyHandle(uint32_t i) const {
    return i < bodies_.size() ? bodies_[i].handle : 0;
  }
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
    // The COLLIDER lattice, in `physScale` units per world voxel. int8, so
    // +-120 per axis — this is the bound that decides how fine physics can be,
    // and it is why physScale is chosen to fit rather than authored.
    std::vector<DebrisVoxel> voxels;
    // The SKIN lattice, in `micro.skinScale` units per world voxel. int16, so
    // it has room the collider does not (voxload.h PrefabVoxel).
    //
    // Empty when skinScale == physScale: the two lattices coincide, `voxels`
    // serves both, and every pre-split body behaves exactly as it did. Only a
    // body whose skin is genuinely finer pays the second array. `SkinSrc()`
    // hides the distinction from readers.
    //
    // When populated this is the AUTHORITATIVE shape: carving edits it, and
    // `voxels` is re-derived from it by majority-fill. Deriving rather than
    // carving both in parallel is what makes skin/collider drift
    // unrepresentable instead of merely tested.
    std::vector<PrefabVoxel> skinVoxels;
    BodyTransform xf{};
    float radiusVoxels = 0;
    // Microvoxel rendering, handed over by the adopting caller and OWNED here
    // from then on — so a severed micro limb keeps its detail as ordinary
    // debris, and the description cannot outlive the body it describes.
    MicroBodyRef micro{};
    // Collider voxels per world voxel: the units of `voxels`, the pitch the
    // Jolt collider is built at, and the divisor for every world-space
    // quantity derived from a body-local coordinate (radius, particle
    // positions, settle-back downsample).
    uint32_t physScale = 1;
    // True when the skin is finer than the collider and `skinVoxels` is live.
    bool HasFineSkin() const {
      return !skinVoxels.empty() && micro.skinScale > physScale;
    }
    uint32_t bleedMat = 0;       // nonzero => body bleeds when carved
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

  // ---- shared damage core ----------------------------------------------------
  //
  // A carve is described ONCE, in world space, and asked to express itself at
  // whichever lattice resolution is being tested: `carveAt(scale)` returns a
  // `keep(x, y, z)` predicate over body-local coordinates at `scale` units per
  // world voxel. A body with a finer skin is carved twice from the same
  // description — once on the skin, and the collider re-derived from it — which
  // is why the caller hands over a factory rather than a fixed predicate.
  //
  // Coordinates are floats rather than a voxel struct so one predicate serves
  // both int8 DebrisVoxel and int16 PrefabVoxel without a template.
  using CarveKeep = std::function<bool(float, float, float)>;
  using CarveFactory = std::function<CarveKeep(float)>;
  // Removed voxels become particles when `eject`, else vanish. Handles the
  // micro re-skin, the collider rebuild, the connectivity split and the
  // dissolve-to-rubble floor, so explosions, the laser and (in time) any other
  // damage source all behave identically. Returns false when the body was
  // destroyed outright, in which case `bi` now indexes a DIFFERENT body
  // (swap-and-pop) and the caller must not advance.
  bool DamageBody(size_t bi, World& world, std::vector<ParticleSpawn>& spawns,
                  std::vector<Body>& fragments, uint32_t& newBodyBudget,
                  bool eject, const CarveFactory& carveAt);
  // Rebuild a body's Jolt collider from its current voxels, preserving pose and
  // velocity. Micro bodies pass voxelPitch = 1/scale — a scale-2 body's voxels
  // are half-size, and building it at pitch 1 would double its physical volume
  // and its mass. Returns false if Jolt refused (body left untouched).
  bool RebuildCollider(Body& b);
  // Re-skin a damaged micro body: clone-on-first-damage, rewrite the brick from
  // the surviving voxels, and shift the transform so the art stays on the
  // collider. No-op for cube-path bodies. Returns false if the pool is full,
  // in which case the body keeps its stale skin but is still really damaged.
  bool ReskinMicro(Body& b);
  // Re-derive the collider lattice from the authoritative skin lattice by
  // majority-fill. No-op unless the body actually has a finer skin. Data flows
  // skin -> collider and never the other way; see the impl comment.
  void DeriveColliderFromSkin(Body& b);
  // The pose fix-up both damage and split need: rebasing voxels to a new min
  // corner moves the body origin, so the transform must move the other way.
  static void RebaseVoxels(std::vector<DebrisVoxel>& voxels, BodyTransform& xf);
  // Tear down one body: drops its Jolt body AND returns its copy-on-write
  // micro brick to the pool. Every removal site must go through this — a body
  // culled, settled, dissolved or split without freeing its brick leaks pool
  // words that nothing will ever reclaim, and the pool is a hard ceiling.
  void ReleaseBody(Body& b);
  // Settle-back (PLAN §B6): a long-asleep, near-axis-aligned body converts
  // its voxels to CellOps (fill-air-only: grid content wins deterministically
  // on the GPU) and frees its body. At most one body per tick.
  void SettleBodies(uint32_t tick, World& world, std::vector<CellOp>& cellOps);

  Physics* phys_ = nullptr;
  World* world_ = nullptr;
  // Shared micro brick pool (owned by main, uploaded by Simulation). Damaged
  // micro bodies allocate private models out of it — see SetMicroSet.
  MicroBodySet* microSet_ = nullptr;
  // One-shot: a skin lattice that overflowed the collider's int8 bound is an
  // authoring problem, and repeating it every carve would bury the diagnostic
  // it is trying to deliver.
  bool skinOverflowWarned_ = false;
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
