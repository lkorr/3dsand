#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "math3d.h"
#include "phys/bodystain.h"  // BodyStainMat/Amt: coats a blow leaves
#include "phys/damagecause.h"  // DamageCause: shared with Mob
#include "phys/kerf.h"   // KerfCut/KerfSlot: the shape a blade takes out
#include "phys/lattice.h"
#include "phys/physics.h"
#include "sim/bodyreact.h"
#include "sim/materials.h"
#include "sim/microbody.h"
// THE WIRE RECORDS the ownership seam below names in its signatures.
// The arrow points ONE WAY: debrissync.h describes bodies out of
// phys/physics.h and sim/voxload.h and knows nothing about this file, so
// including it here costs a header of PODs and no cycle. It is included
// rather than forward-declared because DebrisSystem holds QUEUES of two of
// the records, and a std::deque of an incomplete type is not a thing C++
// promises (only vector/list/forward_list are, and even those would push
// the completeness requirement onto every TU that destroys a DebrisSystem).
#include "net/debrissync.h"
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
  // Returns false when the event queue is full. THE RETURN IS ADVISORY, NOT A
  // LOSS: on a full queue the region's chunks are spilled onto
  // `pendingSupport_`, which is the queue that never drops, so the scan
  // happens late rather than never. Callers therefore do not have to check it
  // — which is what they were already doing, only now it is correct.
  //
  // `spillOnFull` exists for exactly one caller: the pendingSupport_ drain
  // itself, which must not re-enqueue what it is in the middle of draining.
  bool AddDestructionEvent(uint32_t tick, IVec3 lo, IVec3 hi, int margin = 10,
                           bool spillOnFull = true);

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
  //
  // `velVoxPerSec` is optional and is the difference between a creature
  // standing and a creature falling: a patch takes a chunk fetch plus a slot
  // in the per-tick build budget to appear, so a body moving fast has to ask
  // for the ground it is ABOUT to reach, not the ground it is standing on.
  // See the lookahead sweep in ManageTerrain.
  //
  // `horizonVoxels` is the creature's PLANNING horizon (navRadius + 4 while it
  // has a target), kept apart from the body radius on purpose: the body's own
  // chunks are collision and are listed every tick, the horizon is what the
  // A* planner reads through the mirror and is listed on a stride (a mob
  // with a 30-voxel horizon is ~216 chunks, and re-scanning all of them every
  // tick when nothing moved was most of a standing creature's terrain cost).
  void AddTerrainAnchor(Vec3 posVoxel, float radiusVoxels,
                        Vec3 velVoxPerSec = Vec3{}, float horizonVoxels = 0.0f);

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
  // A WOUND ON A BODY. Debris that was once flesh bleeds from the place it
  // was cut, not from nowhere: a corpse's neck stump, the head that came off
  // it, and the stump left on a carved corpse each carry one of these, and
  // BleedBodies drains it every tick from wherever the body has rolled to.
  // `local` and `dir` are in WORLD-voxel units in the body frame (the same
  // convention MobLimb::woundLocal uses, so a limb's wound hands over as is).
  // `budget` is whole blood voxels the wound still owes (the drip, under
  // gore.bleedBudgetCap); `gushTicks` counts the dismemberment gout down from
  // gore.severDecayTicks. A corpse does not pump: nothing tops the budget up,
  // and a wound that has paid out closes. TRANSIENT: not saved, not loaded.
  struct BodyWound {
    bool open = false;
    Vec3 local{};
    Vec3 dir{0, 1, 0};
    float budget = 0.0f;
    int gushTicks = 0;
  };

  void AdoptBody(uint64_t handle, std::vector<DebrisVoxel> voxels,
                 const BodyTransform& xf, MicroBodyRef micro = {},
                 uint32_t physScale = 0,
                 std::vector<PrefabVoxel> skinVoxels = {},
                 uint32_t bleedMat = 0, BodyWound wound = {},
                 bool dead = false, int defIndex = -1, uint64_t creature = 0);

  // ---- WHAT A CORPSE SAYS WHEN YOU CUT IT (2026-09-20) --------------------
  //
  // THE SAME NOISES A LIVING BODY MAKES, MINUS THE VOICE. A blow on a corpse
  // used to read as "a chip" — main.cpp's tier logic infers what was hit from
  // the sever and voice queues, a corpse fills neither, and the fallback is
  // the cue a crate gets. So hacking a body apart on the ground sounded like
  // hitting a box, which is the one thing it is not.
  //
  // This is the debris twin of MobSystem::SeverEvent and it exists for the
  // identical reason, stated there: the event happens inside the tick loop
  // (0..4 times a frame) and audio is a per-frame job, and the DEF INDEX rides
  // on the event because the mob that owned this flesh is long despawned — the
  // def outlives it, so a corpse can still make its own species' wet
  // dismemberment sound rather than a generic one.
  //
  // `severed` separates the two things a blade does to a corpse: opening it
  // (flesh + the wet cut) and taking a piece OFF (the dismember take as well).
  struct GoreEvent {
    Vec3 posVoxel;         // where, world voxels
    int defIndex = -1;     // the creature this was part of, or -1
    float severity = 1.0f; // 0..1, the blow's own power
    bool severed = false;  // a piece came off, not merely a wound
    bool byBlade = false;  // an edge did it, not a blast or a fire
  };
  const std::vector<GoreEvent>& GoreEvents() const { return gore_; }
  void ClearGoreEvents() { gore_.clear(); }
  // ---- WHERE THIS BODY'S MATTER ACTUALLY IS, NEAREST A POINT ---------------
  //
  // A body's transform says where its ORIGIN is; a joint anchor, a blow aimed
  // at a joint, or anything else expressed in world space needs to know where
  // the nearest real voxel is, and a lattice is not a sphere. Used by the
  // dismemberment gate to chop AT a corpse's neck rather than at the empty
  // space the anchor sits in — aiming at the anchor itself put 56 of 60 blows
  // in the air. Returns `worldPoint` unchanged when the handle is not a body.
  Vec3 NearestVoxelWorld(uint64_t handle, Vec3 worldPoint) const;
  // How much of this body's matter is within `radiusVox` WORLD voxels of a
  // world point, on its authoritative lattice. The joint rule's own question
  // (PartJointsAt), exposed so a gate can report the number that decides
  // instead of reporting that nothing happened.
  uint32_t VoxelsNearWorld(uint64_t handle, Vec3 worldPoint,
                           float radiusVox) const;
  // True when this body came off a creature (Body::dead). The melee sweep asks
  // so a blow can be reported as FLESH rather than as a chip.
  bool BodyIsDeadFlesh(uint64_t handle) const {
    const int i = IndexOfHandle(handle);
    return i >= 0 && bodies_[i].dead && bodies_[i].bleedMat != 0;
  }
  // Rewrite up to `maxCount` voxels of `fromMat` on one body to `toMat`, on
  // its authoritative lattice, poking the brick and recounting what the burn
  // pass keys on. A fixture's way of setting a corpse piece alight (gate
  // corpse-crossheat) without a world fire that would light everything else
  // too. Returns how many changed; 0 for an unknown handle.
  uint32_t RewriteBodyMaterial(uint64_t handle, uint32_t fromMat, uint32_t toMat,
                               uint32_t maxCount);

  // ---- DEAD FLESH IS THE LIVING'S BUSINESS (2026-09-22) ---------------------
  //
  // SEVERED FLESH is these bodies: limbs cut off a rig (living or dead),
  // carved gobbets, and the limbs of a corpse the dead cap decayed to debris
  // (Mob::ReleaseRigToDebris). A corpse itself is a dead Mob and never here
  // (PLAN_corpse_is_a_mob.md). Everything that happens TO this flesh
  // -- the coat it wears (blood, water), the splatter that lands on it, and
  // the fire and acid that eat it -- runs through MobSystem's passes, the same
  // BurnOneLimb / StainOneLimb / SplatterView a living limb goes through, not
  // through a debris twin of them. The twin is how corpses lost cross-joint
  // heat and armour once: BurnBodies was forked from the living
  // pass, and every improvement since had to be ported by hand, or was not.
  //
  // What stays HERE is what only a body has: compaction, shatter, the
  // below-body-worthiness particle handoff and the batched collider rebuild
  // (BurnTail, shared with BurnBodies so there is one of those too).
  //
  // FleshLattice is one such body described in the terms MobSystem's view
  // needs; `creature` groups one creature's pieces (DetachLimb and
  // ReleaseRigToDebris stamp the mob id on each, fragments inherit it) the way
  // a creature's limbs are grouped, and `shells` are the bodies strapped to it
  // (DetachLimb / ReleaseRigToDebris -> StrapBody): its armour.
  struct FleshShell {
    uint64_t id = 0;  // global id, the key a march index is cached under
    const std::vector<PrefabVoxel>* skin = nullptr;
    const std::vector<DebrisVoxel>* coll = nullptr;
    const BodyTransform* xf = nullptr;
    uint32_t scale = 1;
    uint32_t geomGen = 0;  // Body::geomGen: the march index's second key
  };
  struct FleshLattice {
    uint64_t id = 0;         // global id: survives a collider rebuild's new handle
    uint64_t creature = 0;   // the mob it came off; 0 = its own group
    std::vector<PrefabVoxel>* skin = nullptr;  // authoritative when non-null
    std::vector<DebrisVoxel>* coll = nullptr;  // ...else this
    uint32_t scale = 1;      // units of the authoritative lattice / world voxel
    uint32_t physScale = 1;  // units of the collider lattice
    const BodyTransform* xf = nullptr;
    IVec3 lo{}, hi{};        // collider box, physScale units, hi exclusive
    uint32_t* microModel = nullptr;
    // Something on it has an ungated self rule (Body::activeCount): the
    // criterion BuildBurnIndex's sweep uses for "alight". Seeds the view's
    // burn state the first time MobSystem meets the body, so a corpse whose
    // only fire is its own embers is not turned away by the cheap gate.
    bool selfActive = false;
    uint32_t bleedMat = 0;   // Body::bleedMat: the blood a bared bone may wear
    // Body::geomGen. With the voxel count, the key every index derived from
    // lattice COORDINATES is cached on (MobSystem::FleshView).
    uint32_t geomGen = 0;
    std::vector<FleshShell> shells;  // filled by BurnFleshBodies only
  };
  // A corpse piece MobSystem burns, rather than BurnBodies. Followers are not:
  // a strapped garment is not flesh, it burns here, and the flesh under it
  // reads it through the occlusion probe exactly as a living limb reads its
  // worn shell.
  // (A template only because Body is declared further down this class.)
  template <class B>
  static bool IsFlesh(const B& b) {
    return b.dead && b.bleedMat != 0 && !b.Follower();
  }
  template <class Fn>
  void ForEachDeadFlesh(Fn&& fn) {
    for (Body& b : bodies_) {
      if (!IsFlesh(b) || !OwnedLocally(b) || b.voxels.empty()) continue;
      FleshLattice f = FleshOf(b);
      fn(f);
    }
  }
  // One burn tick over every dead-flesh body. `burn` gets them all at once
  // (so it can build a per-creature heat snapshot before any of them burns)
  // and fills one FleshBurn per lattice: how many voxels it tombstoned
  // (material 0, compacted here) and whether anything changed. The tail then
  // runs per body, and bodies that burned away / fragments that split off are
  // settled after the whole list, the way BurnBodies settles them.
  struct FleshBurn {
    uint32_t removed = 0;
    bool changed = false;
  };
  void BurnFleshBodies(
      const std::function<void(std::vector<FleshLattice>&,
                               std::vector<FleshBurn>&)>& burn,
      World& world, std::vector<ParticleSpawn>& spawns);
  // Open a wound on an adopted body at its voxel nearest `woundW` (world),
  // owing `budget` blood voxels, with `gushTicks` of dismemberment gout.
  // False when no such body, or it has no blood.
  bool WoundBody(uint64_t handle, Vec3 woundW, float budget, int gushTicks);

  // ---- A BLADE ON A LOOSE BODY (2026-09-19) --------------------------------
  //
  // The debris twin of Mob::CutLimb, and it exists because the melee sweep's
  // old answer for anything without an owner was `MeltBodyAt` — a SPHERE of
  // the blade's half-width, bored out per swing tick, with the beam's
  // eject=false so not one gobbet came off it. Three owner reports, one cause:
  // "swords shouldn't delete so many voxels off of corpses", "no blood comes
  // out", and a sphere that cannot sever anything because it is not a cut.
  //
  // Same KerfCut a live limb takes, same slot shape out of phys/kerf.h, same
  // entry snap (so repeated blows to one place DEEPEN instead of saturating),
  // and the ordinary DamageBody tail — which already soaks the cut faces, arms
  // the wound, splits the connectivity and throws the pieces. False when the
  // handle is not an adopted body.
  bool CutBody(uint64_t handle, const KerfCut& cut, World& world,
               std::vector<ParticleSpawn>& spawns);
  // WHAT DID THIS. Carried through the carve so the noise at the far end can
  // tell a sword from a fire: only a blade makes the wet dismemberment sound
  // (an explosion that takes the same arm off did not saw through anything),
  // and only a blow makes any of them at all.
  //
  // ONE ENUM FOR THE LIVING AND THE DEAD (phys/damagecause.h, W2-G): Mob
  // passes the same value through Damage/CarveLimb/Sever. The alias keeps
  // `DebrisSystem::DamageCause::Blade` spelling what it always did.
  using DamageCause = ::DamageCause;
  // ---- ...AND THE MARK A BLOW LEAVES BEFORE IT TAKES ANYTHING -------------
  //
  // THE BRUISE LADDER ON DEAD TISSUE (phys/bodystain.h SoakBruise), which is
  // the same ladder the living climb: a cell darkens, a saturated cell breaks
  // and goes bloody, a cell bloody at depth is PULPED and will crumble over
  // the next few seconds (PulpTick). Until 2026-09-20 a mace on a corpse went
  // straight to an instant crater — the very shape the living path was given a
  // bruise to stop using — so a beating marked a creature and did nothing
  // visible to its corpse.
  //
  // Returns the RIPENESS of the contact core (0..1): how much of what this
  // blow landed on was ALREADY pulped when it arrived, which is what decides
  // whether it takes matter as well as marking it. `hp` scales the coat the
  // way it does for the living, `unarmed` picks up the gore block's unarmed
  // overrides.
  float BruiseBody(uint64_t handle, Vec3 atVoxel, float radiusVoxels,
                   uint32_t seed, float power, float hp, bool unarmed);
  // One tick of the dissolution the third rung arms. Pulped voxels crumble at
  // gore.pulpRotRate, surface first, so a caved-in corpse keeps coming apart
  // after the blows stop instead of vanishing in one swing. A body nothing has
  // beaten pays one bool test.
  void PulpTick(uint32_t tick, World& world,
                std::vector<ParticleSpawn>& spawns);
  // ...and the mace. A POINT with a magnitude, where a kerf is a slot with a
  // direction (game/mob.h BluntHit draws the same distinction for the living):
  // a shallow ragged dent at `radiusVoxels`, which is what caves a corpse's
  // skull in. `seed` keys the rim so a replayed blow dents identically.
  bool BluntBody(uint64_t handle, Vec3 atVoxel, float radiusVoxels,
                 uint32_t seed, World& world,
                 std::vector<ParticleSpawn>& spawns,
                 DamageCause cause = DamageCause::Blunt);

  // ---- A GARMENT ON A CORPSE IS A FOLLOWER, NOT A JOINTED BODY -------------
  //
  // `shell` stops being simulated and is teleported onto `host` at the rigid
  // offset the two hold RIGHT NOW, every PostStep, for as long as both exist.
  // Kinematic, so it has no mass, no gravity and no contact response of its
  // own: the pair's dynamics are the HOST's dynamics, entire.
  //
  // WHY THIS EXISTS, stated where it is used rather than in a plan doc. A worn
  // shell is geometrically INSIDE the limb it wraps, at an iron-against-flesh
  // mass ratio. There are exactly three things two such bodies can be, and two
  // of them are motors:
  //   - roped together by a Fixed joint: a stiff constraint across a deep
  //     overlap between very unequal masses is the textbook way to make a
  //     sequential-impulse solver GAIN energy every step. This is what
  //     Mob::Die used to build, and it is the armoured-corpse blow-up the
  //     owner reported on 2026-09-13 (`corpse-armor`): every limb pinned at
  //     Jolt's own 47.12 rad/s clamp, the rig flying apart while still
  //     attached, then slowly hauled back together by the same joints.
  //   - two free dynamic bodies sharing the same space: the solver resolves
  //     the penetration by firing them apart.
  //   - one body's pose DERIVED from the other's, which is this. The shell
  //     contributes nothing to the solver at all, so an armoured corpse has
  //     the same body count, the same constraint graph and the same mass as
  //     the naked one — which is the property the gate asserts.
  //
  // It is the same relationship Mob::DriveWornShells maintains on a living
  // creature (MobLimb::wornHost); death hands it over rather than trading it
  // for a constraint, so a garment is a follower in EVERY phase and there is
  // no transition at which the pair can gain energy. False when either handle
  // is not an adopted body of this system.
  bool StrapBody(uint64_t shell, uint64_t host);
  // Cut one strap by handle: the body becomes ordinary dynamic debris, keeping
  // the velocity it was being driven at. For the drag-out-of-the-loot-panel
  // path (ShedCorpseLoot) — a plate pulled off a corpse has to fall off it, and
  // "the body is already lying there" stops being true the moment the body is
  // glued to the limb. False when there was no strap to cut.
  bool UnstrapBody(uint64_t handle);
  // The host a strapped body follows, or 0. For callers and for the gate: a
  // shell body with a host is one the solver never sees.
  uint64_t WornHostOf(uint64_t handle) const;
  // APPENDS every follower of `host` to `out`. For the one caller that has to
  // treat a piece and its gear as one set — the collision-group re-tie when a
  // severed limb leaves its sever hold (Mob::TickSeveredHolds). Appends rather
  // than assigns so the caller can seed the list with the host itself, which is
  // the shape DisableCollisionsAmong wants.
  void FollowersOf(uint64_t host, std::vector<uint64_t>& out) const;

  // ======= OWNERSHIP AND GHOST BODIES (PLAN_multiplayer_m9.md M9.4-C) =======
  //
  // EVERY LOOSE BODY IS SIMULATED ONCE AND SEEN TWICE. One machine owns a
  // body: it steps it in Jolt, burns it, bleeds it, settles it back into the
  // grid and is the only machine that authors an op about it. Every other
  // machine holds a GHOST of the same body -- a kinematic collider posed from
  // the wire, solid to walk into, which emits nothing at all.
  //
  // WHY KINEMATIC RATHER THAN "SKIP THE SIMULATION". A body that is not
  // stepped is not a body: it falls through the floor of the far machine's
  // solver, or worse, is stepped there TOO and the two copies diverge within a
  // second (a rigid body's trajectory is not a function of its inputs the way
  // the CA's is -- Jolt's solver is order- and contact-manifold-dependent). A
  // kinematic body has no gravity, no mass in any contact and no depenetration
  // response, so its pose is exactly what was sent and nothing else. That is
  // the SAME argument StrapBody makes for a garment following a limb, and the
  // two paths share one implementation (DriveKinematicTo) for that reason.
  //
  // A GHOST IS NOT A FOLLOWER. The strap derives a pose from ANOTHER LOCAL
  // BODY every PostStep; a ghost's pose comes off the wire and has no host
  // here at all. They are the same mechanism below the pose and different
  // questions above it, which is why `wornHost` stays 0 on a ghost -- a ghost
  // whose "host" went missing must not be released into dynamic debris the
  // way DriveStraps releases an orphaned garment.
  //
  // WHAT HAPPENS WITH NO OWNERSHIP FUNCTION SET, which is every single-player
  // run and every gate but `debris-ghost`: `ownershipFn_` is null, every body
  // is owned by `localPlayerId_` (0), every owner test passes, and the tick is
  // byte-for-byte today's. That is the property the unmoved world hash proves.

  // Session 0's player id, and the default owner of everything. A
  // single-player process never changes it.
  static constexpr uint32_t kLocalOwner = 0;

  // WHICH PLAYER THIS PROCESS IS. Set once at join (M9.4-D). Changing it does
  // NOT retroactively re-mint existing bodies' `ownerAtCreate` -- those ids are
  // already out on the wire -- so it is called before any body exists.
  void SetLocalPlayerId(uint32_t id);
  uint32_t LocalPlayerId() const { return localPlayerId_; }

  // WHO OWNS A BODY AT THIS POSITION (net::EntityAuthority over the peer list,
  // wired by M9.4-D). Null = every body is mine, which is today's behaviour.
  //
  // Asked per body per tick from PreTick, with the body's GLOBAL ID and its
  // own position. It must be a PURE function of the peer views both machines
  // hold, or the two would disagree about who is stepping what -- M9.4-A's
  // hysteresis is what keeps a body straddling a chunk edge from flapping
  // between them.
  //
  // THE ID IS THERE SO THE HYSTERESIS CAN BE KEYED ON THE BODY (M9.4-E).
  // M9.4-C passed a position and nothing else, which left `net::EntitySync`
  // no choice but to key its incumbent table on the CHUNK -- and a chunk-keyed
  // memory is one incumbent shared by every body in the chunk, so a sword
  // jittering across a boundary re-homed the crate beside it as well. The id
  // is `(ownerAtCreate << 48) | serial` (net::MakeGlobalBodyId), the same
  // identity every wire record for the body carries, and it is stable across
  // a handoff.
  void SetOwnershipFn(std::function<uint32_t(uint64_t globalBodyId,
                                             Vec3 posVoxel)> fn);
  // POSITION-ONLY, for a caller that has no use for the identity. Kept as its
  // own overload rather than making everybody write an ignored parameter: a
  // "put everything back on this machine" binding (main.cpp's disconnect path)
  // genuinely does not care which body it is being asked about.
  void SetOwnershipFn(std::function<uint32_t(Vec3 posVoxel)> fn);
  // ...and the null case, which is a NAMED call because `nullptr` would be
  // ambiguous between the two overloads above. Single player, and every gate
  // but `debris-ghost`, ends here.
  void ClearOwnershipFn();

  // WHO OWNS A CHUNK (net::ChunkAuthority). Null = every chunk is mine.
  //
  // Separate from the body question because the island scan is not about a
  // body at all: it is about WHICH MACHINE LOOKS AT A REGION OF GRID for
  // matter that has lost its support. A peer's explosion in my chunk needs MY
  // scan (the plan's M9.4-D note), so this is keyed on the region and not on
  // whoever caused it.
  void SetChunkOwnedFn(std::function<bool(IVec3 wc)> fn);

  // The global id of a body by handle, or 0 if the handle is not one of ours.
  // `(ownerAtCreate << 48) | serial` -- see net::MakeGlobalBodyId.
  uint64_t GlobalIdOf(uint64_t handle) const;
  // ...and back. 0 when no body here carries that id.
  uint64_t HandleOfGlobalId(uint64_t globalId) const;
  // The current owner of a body by handle, or kLocalOwner for an unknown one
  // (an unknown handle is not a thing this machine has to stop stepping).
  uint32_t OwnerOfBody(uint64_t handle) const;
  // Is this body posed from the wire rather than stepped here?
  bool IsGhost(uint64_t handle) const;
  // How many ghosts exist. For the `--frames` net report and for the gate.
  uint32_t GhostCount() const;

  // ---- receiving ----------------------------------------------------------

  // Apply a received pose to the ghost it names. False when no body carries
  // that id (an announce has not arrived yet, or the body is gone), or when
  // the pose is OLDER than the one already held, or when the named body is one
  // of MINE -- a peer does not get to move a body I am stepping, and silently
  // accepting one would be a desync with no symptom until the next settle.
  bool ApplyBodyPose(const net::BodyPose& pose);

  // Create (or refresh) the ghost an announce describes and return its handle,
  // 0 on failure. Idempotent by global id: a re-announce of a body we already
  // hold updates its pose rather than making a second copy.
  uint64_t ApplyBodyAnnounce(const net::BodyAnnounce& a);

  // Take ownership of a body a peer has handed to me. The body is recreated
  // DYNAMIC at the announce's pose, given the handed-over velocities, and
  // adopted. Returns the new handle, 0 on failure.
  uint64_t ApplyBodyHandoff(const net::BodyHandoff& h);

  // The owner let go of it. Destroys the ghost; false if we did not hold one.
  bool ApplyBodyGone(const net::BodyGone& g);

  // ---- sending ------------------------------------------------------------

  // Fill an announce/pose/handoff for one of MY bodies. False for a handle I
  // do not own (there is nothing truthful to say about a body I am not
  // stepping). `BuildHandoff` ALSO flips my copy to a ghost -- a handoff is not
  // a query, it is the moment authority moves, and leaving the old owner
  // stepping it until an ack came back is how both machines end up simulating
  // the same sword.
  bool BuildAnnounce(uint64_t handle, net::BodyAnnounce& out) const;
  bool BuildPose(uint64_t handle, uint32_t tick, net::BodyPose& out) const;
  bool BuildHandoff(uint64_t handle, uint32_t newOwner, net::BodyHandoff& out);

  // INTEREST: the global ids of my bodies inside a peer's residency window,
  // grown by `marginChunks`. What M9.4-D sends a pose for this tick.
  //
  // A SLEEPING BODY IS SENT ONCE AND THEN NOT AGAIN. `Body::inactiveTicks` is
  // already the settle countdown and it is reset the moment anything moves, so
  // it is exactly the right signal: a battlefield of two hundred settled
  // corpses costs no bandwidth, and the first one somebody kicks resumes at
  // full rate. That is rule 2 applied to the wire.
  void OwnedBodiesNear(IVec3 peerWindowOrigin, int marginChunks,
                       std::vector<uint64_t>& outGlobalIds) const;

  // ---- ground items -------------------------------------------------------
  //
  // THE ITEM REGISTRY IS NOT MINE (game/worlditems.h lives a layer up and
  // includes this header), so the two facts this system needs about a
  // body-as-item arrive as callbacks. That keeps the layering one-way and
  // keeps "which item is this" in the one place that owns it.
  //
  // `lookup` fills the item a body handle IS (the whole ItemInstance: name,
  // dye, fill, damage) and returns false if the body is not an item. `take`
  // removes the body from the world exactly as the local E key does and
  // returns true if it really was taken.
  void SetItemLookupFn(
      std::function<bool(uint64_t handle, ItemInstance& item)> fn);
  void SetItemTakeFn(std::function<bool(uint64_t handle)> fn);

  // PICK UP THE BODY NAMED BY A GLOBAL ID -- the ONE entry point for E, on both
  // machines. If I own it, the pickup happens now and a grant appears on the
  // grant queue this tick. If I do not, an ItemTake is queued for the
  // transport to send and the grant arrives when the owner replies. Either
  // way the caller's code is `RequestItemTake(id)` then drain `PopItemGrant`,
  // which is what stops "the owner's pickup" and "the peer's pickup" from
  // being two code paths that drift.
  //
  // Returns false only when there is nothing to ask about (unknown id).
  bool RequestItemTake(uint64_t globalId);

  // Drain one pending outgoing request (M9.4-D sends it). False when empty.
  bool PopItemTake(net::ItemTake& out);
  // A peer asked me for one of my items: perform the pickup and queue the
  // reply. The reply is a REFUSAL (`granted == 0`) when the body is gone, is
  // not an item, or is not mine -- see the note on ItemGrant.
  void ApplyItemTake(const net::ItemTake& t);
  // Drain one grant addressed to this machine. Both the owner's own pickups
  // and a peer's replies (fed in through ApplyItemGrant) come out here.
  bool PopItemGrant(net::ItemGrant& out);
  void ApplyItemGrant(const net::ItemGrant& g);

  // ---- probe (gate `debris-ghost`) ----------------------------------------
  //
  // Counters, not booleans: "the island scan skipped N chunks" is a number a
  // gate can assert a DIFFERENTIAL on, while "it skipped some" is not.
  struct OwnerProbe {
    uint32_t ghostBodies = 0;      // ghosts alive at the last PreTick
    uint32_t ghostsDriven = 0;     // poses applied to colliders, cumulative
    uint32_t posesRefused = 0;     // stale / not-a-ghost / unknown id
    uint32_t chunksSkipped = 0;    // island-scan regions another machine owns
    uint32_t emittersSkipped = 0;  // per-body emitter entries a ghost skipped
    uint32_t handoffsOut = 0, handoffsIn = 0;
    uint32_t itemsGranted = 0, itemsRefused = 0;
  };
  const OwnerProbe& Owner() const { return ownerProbe_; }
  void ResetOwnerProbe() { ownerProbe_ = OwnerProbe{}; }

  // TAKE A BODY OUT OF THE WORLD. Not damage and not a cull: the thing has
  // been picked up, and it stops existing as matter. Goes through the same
  // ReleaseBody every other disposal does, so the brick is freed and the
  // "gone" notification fires exactly once (see SetOnBodyGone) — which is what
  // keeps the ground-item registry from outliving the handle it names.
  bool DestroyBody(uint64_t handle);

  // A body's AUTHORITATIVE lattice, by handle — the skin where it has one, the
  // collider where it does not, promoted to PrefabVoxel so the caller does not
  // have to know which. `outScale` is the units that lattice is in.
  //
  // For the ground-item save (game/persist.cpp 'ITMS'): a dropped robe that
  // has burned through has to come back burnt, and the only copy of those
  // holes is the body's own lattice. Reading the DERIVED side instead would
  // save something the next re-derive overwrites (phys/lattice.h).
  bool BodyLatticeOf(uint64_t handle, std::vector<PrefabVoxel>& out,
                     uint32_t& outScale) const;

  // IS THIS BODY ONE OF MINE? The ownership question, asked positively.
  //
  // A Jolt handle off a ray cast says nothing about who is driving it: a
  // living creature's limbs are MobSystem's and are posed kinematically every
  // tick, while everything here is free matter. The physics grab
  // (game/grab.h) is the caller — it must refuse a limb that still belongs to
  // somebody — and a handle that fails this may also simply be DEAD, since
  // damage REPLACES a body's handle (see BodyHandle above).
  bool HasBody(uint64_t handle) const {
    if (!handle) return false;
    for (const Body& b : bodies_)
      if (b.handle == handle) return true;
    return false;
  }

  // Per-material density, as every body-creating call here already takes it.
  // Exposed so a caller that builds a body and hands it straight over (a
  // dropped item — game/worlditems.h) uses the SAME table this system does,
  // rather than threading a second copy through from wherever materials were
  // loaded and having the two disagree after an R reload.
  const std::vector<float>& DensityOf() const { return densityOf_; }

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

  // ---- art colour -> GRID tint (sim/materials.h kMatFlagTinted) ------------
  //
  // A body voxel carries an 8-bit ART colour, which is render-only: nothing in
  // the world grid can hold it, so a green-fleshed corpse used to land in the
  // world the raw colour of `skin`. A MATF_TINTED material can hold 16 GRID
  // colours in the state nibble, and this is the map between the two — built
  // once, on the CPU, at load.
  //
  // Nearest by INTEGER RGB distance over the merged art palette
  // (MicroBodySet::artColors, whose 1-based indices `DebrisVoxel::color` and
  // `PrefabVoxel::color` hold). Integer and load-time, so rule 1 is untouched
  // twice over: no float ever reaches a sim decision, and the result is a
  // constant for the whole run of a given materials.json + prefab set.
  //
  // REBUILT ON DEMAND rather than pushed by a setter, because the two inputs
  // arrive from different places at different times — materials from
  // OnMaterialsReloaded, the merged palette from LoadMobDefs (and again from
  // the item loader, and again on every R hot-reload, which CLEARS the palette
  // and renumbers it). A stamp over the palette's CONTENTS, not its length,
  // is what makes "drop one mob, add another" invalidate it: same count,
  // different colours, and a length check would have kept the stale table and
  // repainted everything with the previous load's dyes.
  void RefreshTintMap();
  // The state nibble to write into the grid for `mat` carrying merged art
  // index `art` (0 = unpainted). Returns `fallback` — the cosmetic jitter
  // variant every caller already computed — when `mat` is not tinted, so an
  // untinted material behaves exactly as it did before this existed.
  uint32_t GridStateFor(uint32_t mat, uint32_t art, uint32_t fallback) const;
  // ---- "this body is gone" -------------------------------------------------
  //
  // Called for every body this system releases, whatever released it: culled
  // out of the window, burned to nothing, blown apart, or reset. It exists for
  // ONE caller — the dropped-item registry (game/worlditems.h), which maps a
  // body handle to an item name and must not outlive the handle. Jolt reuses
  // handles, so a stale entry would eventually re-match a NEW body and hand
  // the player a sword they picked up off a rock.
  //
  // A callback rather than the registry being a member here: DebrisSystem has
  // no business knowing what an item is, and this way "a burned robe on the
  // ground is GONE" needs no code on this side to be true.
  void SetOnBodyGone(std::function<void(uint64_t)> cb) {
    onBodyGone_ = std::move(cb);
  }
  // (No day phase / rain word here any more: the reaction gates that read them
  // run in MobSystem::BurnOneLimb, which has its own, set beside this.)

  // ---- THE ONE BODY-REACTION EVALUATOR (W2-I, 2026-09-24) -----------------
  // Every non-flesh body BurnBodies offers is evaluated by this function, the
  // same MobSystem::BurnOneLimb the living, the dead and severed flesh go
  // through (MobSystem::Init installs it). In: the body as a lattice view and
  // this body's share of the candidate budget (spent down), the tick's grid
  // op budget and the tick's pot of world-walk cells (both spent down). Out:
  // whether anything changed, how many voxels it tombstoned (material 0,
  // compacted by BurnTail), and whether the visit was DEFERRED whole because
  // its walk did not fit the pot (then nothing was read or changed, and the
  // scheduler starts there next tick). `end` runs once per
  // BurnBodies call after the last body, so the evaluator can retire the state
  // of bodies it has not been offered for a while -- and with `forgetAll` from
  // Reset(), which restarts the serials that state is keyed on.
  using BodyReactFn = std::function<bool(
      FleshLattice& lat, uint32_t tick, World& world,
      std::vector<CellOp>& cellOps, uint32_t& frontBudget,
      uint32_t& opsBudget, uint32_t& walkBudget, uint32_t& removed,
      bool& deferred)>;
  using BodyReactEndFn = std::function<void(uint32_t tick, bool forgetAll)>;
  void SetBodyReactor(BodyReactFn fn, BodyReactEndFn end) {
    bodyReact_ = std::move(fn);
    bodyReactEnd_ = std::move(end);
  }
  // THE HOOK IS BORROWED, AND HANDED BACK. The installer's closures capture
  // it; a second MobSystem stood up against this DebrisSystem (mob-handoff's
  // `far`) replaced the first one's hook and then died, leaving the next
  // BurnBodies calling into a destroyed object (found 2026-09-24, W2-K).
  // Install with an owner token; the owner restores what it displaced on its
  // way out, and only if it is still the one installed.
  struct BodyReactor {
    BodyReactFn fn;
    BodyReactEndFn end;
    const void* owner = nullptr;
  };
  BodyReactor InstallBodyReactor(BodyReactFn fn, BodyReactEndFn end,
                                 const void* owner) {
    BodyReactor prev{std::move(bodyReact_), std::move(bodyReactEnd_),
                     bodyReactOwner_};
    bodyReact_ = std::move(fn);
    bodyReactEnd_ = std::move(end);
    bodyReactOwner_ = owner;
    return prev;
  }
  void RestoreBodyReactor(const void* owner, BodyReactor prev) {
    if (bodyReactOwner_ != owner) return;
    bodyReact_ = std::move(prev.fn);
    bodyReactEnd_ = std::move(prev.end);
    bodyReactOwner_ = prev.owner;
  }

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
  // Every brick record this system holds, drawn or not (sim/microbody.h
  // MicroHolder). Used by the aliasing audit, never by the frame path.
  void AppendMicroHolders(std::vector<MicroHolder>& out) const;
  uint32_t InstanceCount() const { return instanceCount_; }
  uint32_t BodyCount() const { return (uint32_t)bodies_.size(); }
  // HOW MANY SLOTS THIS SYSTEM OCCUPIES IN THE SHARED BODY SLOT SPACE, which
  // is NOT BodyCount(): the three render walks all stop at kMaxBodies, and
  // `bodies_` is allowed past it between an adoption and the next PostStep
  // cull (AdoptBody takes no cap — a decayed corpse hands over fifteen limbs
  // at once, Mob::ReleaseRigToDebris).
  //
  // Everything downstream of this system indexes ONE shared transform array,
  // so a slot base taken from BodyCount() while the walks emitted fewer puts
  // every mob limb and every avatar part at somebody else's transform. That is
  // a body-swap, not a missing body, and it is invisible in any test that does
  // not push past two hundred bodies. game/bodyreg.cpp is the only caller that
  // should ever need this.
  uint32_t SlotCount() const {
    return (uint32_t)std::min<size_t>(bodies_.size(), kMaxBodies);
  }
  // Voxels across every body, counted on each one's AUTHORITATIVE lattice.
  //
  // The obvious alternative — counting the cube instances BuildInstances emits
  // — measures nothing at all on a micro body, which emits none: it renders as
  // one OBB with a brick marched inside it. So a test that watched instance
  // counts to see a corpse burn away would watch a number that was zero before
  // the fire started (the burn subtests in selftest_mob.cpp).
  uint32_t TotalBodyVoxels() const {
    uint32_t n = 0;
    for (const Body& b : bodies_)
      n += (uint32_t)(b.HasFineSkin() ? b.skinVoxels.size() : b.voxels.size());
    return n;
  }
  // Voxels of ONE material across every body, on the same authoritative
  // lattice TotalBodyVoxels counts. "Is anything in the world still on fire"
  // is a question about MATERIAL, and after a creature dies its limbs are
  // bodies here rather than limbs on a mob — so a burn test that only censused
  // the mob would go quiet the moment the mob did.
  uint32_t TotalBodyMaterial(uint32_t mat) const {
    uint32_t n = 0;
    for (const Body& b : bodies_) {
      if (b.HasFineSkin()) {
        for (const PrefabVoxel& v : b.skinVoxels)
          if ((v.material & 0xFFFu) == (mat & 0xFFFu)) n++;
      } else {
        for (const DebrisVoxel& v : b.voxels)
          if ((v.payload & 0xFFFu) == (mat & 0xFFFu)) n++;
      }
    }
    return n;
  }
  // Physics handle of body `i`. Damage rebuilds a body's collider, which
  // REPLACES its handle, so anything holding one across a damage call (the
  // selftest's repeated laser kerf) must re-read it here. 0 if out of range.
  uint64_t BodyHandle(uint32_t i) const {
    return i < bodies_.size() ? bodies_[i].handle : 0;
  }
  uint32_t ActiveBodyCount() const;
  // Per-body identity, for a test that has to say WHICH body is left rather
  // than how many. "1 still a body" is the same bare count as "1 awake": the
  // useful question is whether it is the body the test made (same size, same
  // place) or one that appeared from somewhere else, and those have completely
  // different fixes.
  Vec3 BodyPosition(uint32_t i) const {
    return i < bodies_.size() ? bodies_[i].xf.pos : Vec3{};
  }
  // ---- severed-flesh wounds, for a gate that has to say WHICH piece bleeds --
  uint32_t WoundedBodyCount() const {
    uint32_t n = 0;
    for (const Body& b : bodies_)
      if (b.wound.open) n++;
    return n;
  }
  bool BodyWoundOpen(uint32_t i) const {
    return i < bodies_.size() && bodies_[i].wound.open;
  }
  float BodyWoundBudget(uint32_t i) const {
    return i < bodies_.size() ? bodies_[i].wound.budget : 0.0f;
  }
  Vec3 BodyWoundWorld(uint32_t i) const;  // the wound, in world voxels
  uint32_t BodyVoxelCount(uint32_t i) const {
    if (i >= bodies_.size()) return 0;
    const Body& b = bodies_[i];
    return (uint32_t)(b.HasFineSkin() ? b.skinVoxels.size() : b.voxels.size());
  }
  // ---- corpse burn, for a gate that has to say WHICH piece stopped ---------
  // TotalBodyMaterial for ONE body. "The corpse has 900 embers left" is a
  // bare count; "the left foot has all 80 it died with" names the body the
  // burn pass never reached.
  uint32_t BodyMaterialCount(uint32_t i, uint32_t mat) const {
    if (i >= bodies_.size()) return 0;
    const Body& b = bodies_[i];
    uint32_t n = 0;
    if (b.HasFineSkin()) {
      for (const PrefabVoxel& v : b.skinVoxels)
        if ((v.material & 0xFFFu) == (mat & 0xFFFu)) n++;
    } else {
      for (const DebrisVoxel& v : b.voxels)
        if ((v.payload & 0xFFFu) == (mat & 0xFFFu)) n++;
    }
    return n;
  }
  // Coat LEVELS of `coatMat` summed over ONE body's authoritative lattice
  // (BodyStainAmt of every voxel wearing it). Gate coat-parity reads it
  // against the grid's stain amounts: same scenario, same unit.
  uint32_t BodyCoatLevels(uint32_t i, uint32_t coatMat) const {
    if (i >= bodies_.size()) return 0;
    const Body& b = bodies_[i];
    uint32_t n = 0;
    auto add = [&](uint16_t s) {
      if (BodyStainAmt(s) && BodyStainMat(s) == (coatMat & 0xFFFu))
        n += BodyStainAmt(s);
    };
    if (b.HasFineSkin()) {
      for (const PrefabVoxel& v : b.skinVoxels) add(v.stain);
    } else {
      for (const DebrisVoxel& v : b.voxels) add(v.stain);
    }
    return n;
  }
  // The brick this body is DRAWN from (kMicroBodyNoModel on the cube path).
  // A gate reads the brick back through MicroSet() and censuses it against
  // the lattice above: the lattice is what burns, the brick is what the
  // player sees, and "the corpse keeps glowing" is the two disagreeing.
  uint32_t BodyMicroModel(uint32_t i) const {
    return i < bodies_.size() ? bodies_[i].micro.model : kMicroBodyNoModel;
  }
  // The creature def this body was part of (Body::defIndex), -1 for matter
  // that never lived. MobSystem::EvictUnusedDefs counts it as a holder.
  int BodyDefIndex(uint32_t i) const {
    return i < bodies_.size() ? bodies_[i].defIndex : -1;
  }
  // ---- corpse ground, for a gate that has to say WHY a body fell ----------
  // Of the chunks ManageTerrain wanted on its last sweep: how many carry a
  // collision mesh, how many are still waiting on their chunk fetch, and how
  // many were polygonized EMPTY. "The corpse fell out of the window" is a
  // bare observation; these three numbers are its three causes.
  void TerrainCensus(uint32_t& built, uint32_t& unfetched,
                     uint32_t& empty) const {
    built = unfetched = empty = 0;
    for (const auto& [key, t] : terrain_) {
      if (t.lastNeeded != lastTerrainTick_) continue;
      if (t.handle) built++;
      else if (t.builtVersion == 0) unfetched++;
      else empty++;
    }
  }

  // ---- A BODY MAY NOT ENTER SPACE NO COLLIDER DESCRIBES -------------------
  //
  // The Jolt twin of Player::KnownDrop (game/player.cpp), and the other half
  // of the anti-tunnelling guarantee that Physics' LinearCast motion quality
  // starts (see the note above Physics::CreateDebrisBody). A shape cast can
  // only hit a triangle that EXISTS, and the patch under a body is built by
  // ManageTerrain some ticks after something asks for it: a chunk fetch has to
  // land first, then a slot in kTerrainBuildsPerTick. A body that outruns that
  // arrives in a chunk with no mesh in it at all, passes clean through
  // whatever the voxels say is there, and ends the step further ahead of the
  // patches than it started -- the fall feeds itself, exactly the way the
  // player's blind fall did before 378972e.
  //
  // `prevPosVoxel` is where the body was BEFORE Physics::Step, which both
  // callers already hold (Body::xf and MobLimb::xf are overwritten only after
  // this has run). If the step ENDED in an unvouched chunk the body is put
  // back at the last point along the segment that was vouched, keeping both
  // velocities so it resumes at its real speed the moment the patch lands.
  // The position tested is the body ORIGIN, so the test carries up to a
  // body's-worth of slop -- which is the right scale, because this exists for
  // errors measured in chunks and LinearCast owns the ones measured in voxels.
  //
  // IT IS A CLAMP, NOT A VETO, in two ways, and both matter:
  //   - a body ALREADY in unvouched space is let through, or anything that
  //     spawned out there (or whose patch was evicted under it) would be
  //     pinned where it stands forever;
  //   - a body held for kUntunnelHoldTicks consecutive steps is released with
  //     a report, so no amount of streaming starvation can leave something
  //     hovering in mid-air indefinitely.
  //
  // Returns true if the body was moved.
  bool UntunnelBody(uint64_t handle, const Vec3& prevPosVoxel);

  // ---- ...AND A JOINTED RIG IS ONE BODY FOR THAT PURPOSE ------------------
  //
  // WHY THE PER-BODY CLAMP ABOVE IS WRONG FOR A RAGDOLL. Owner report,
  // 2026-09-13: a clothed human knocked limp by a long fall "becomes a crazy
  // tangled mess ball of limbs and clothes that aren't actually connected
  // properly... the limbs move to different noisily placed locations on
  // different frames". That is this clamp, seen from the outside.
  //
  // A human rig is ~15 limb bodies (~35 dressed) spanning two or three chunks,
  // and the clamp's decision is per body and per chunk: falling fast, the
  // LEADING bodies -- the feet -- enter the unvouched chunk several ticks
  // before the head does. Each one is teleported back to its own last vouched
  // sample WITH ITS VELOCITY INTACT while its neighbours keep travelling, so
  // every tick of the starvation window opens a fresh multi-voxel violation at
  // every joint across the rig. The solver then does what a sequential-impulse
  // solver does with a constraint stretched 30 voxels in one step: it closes it
  // by force, in a different direction for every limb, for up to
  // kUntunnelHoldTicks ticks. The clamp is not lying about the terrain; it is
  // answering a question that only makes sense for ONE rigid body.
  //
  // So a rig is clamped as a unit: ONE vouched fraction of the step, the
  // smallest any member can honestly claim, applied to every member along its
  // own displacement. f = 1 is "the whole rig landed in vouched space" and
  // nothing moves; f = 0 puts the rig back exactly as it was, still rigid,
  // still at speed, and it resumes the tick the patch lands. Every
  // intermediate value is a lerp between two poses the solver itself produced
  // one step apart, which is the strongest statement available here that does
  // not need the joint graph.
  //
  // The escape hatches stay, at RIG scope for the same reasons they exist at
  // body scope: if ANY member started the step in unvouched space the whole rig
  // is let through (clamping the half of a rig that is still over a patch is
  // itself a tear), and the hold counter is keyed on `handles[0]` so the rig is
  // held and released together.
  //
  // `handles` and `prevPosVoxel` are parallel and must stay so. Returns true if
  // the rig was moved.
  bool UntunnelRig(const std::vector<uint64_t>& handles,
                   const std::vector<Vec3>& prevPosVoxel);

  // Does this world chunk have a collision representation right now? A built
  // patch vouches for it whether or not it produced triangles (a chunk of
  // nothing but air, or nothing but solid, polygonizes EMPTY and is still a
  // complete answer). So does being outside the residency window: there is no
  // sim and no body out there, and a body that reaches it despawns.
  bool ColliderVouched(IVec3 worldChunk) const;

  // ---- why a body stopped in mid-air, or did not (CLAUDE.md rule 6) -------
  // A clamp that fires is not a bug and a clamp that releases IS one, so the
  // two are counted apart; `lastChunk` names the chunk with no patch in it,
  // which is the only thing that turns "a body hung in the air" into a cause.
  struct UntunnelProbe {
    uint32_t holds = 0;        // steps a body was put back at the boundary
    uint32_t bodiesHeld = 0;   // distinct bodies that have ever been held
    uint32_t released = 0;     // held past kUntunnelHoldTicks and let through
    float maxStepVox = 0.0f;   // longest single step this ever caught, voxels
    IVec3 lastChunk{};         // ...and the unvouched chunk it was entering
    // UntunnelRig only: rig-steps clamped as a unit, and the smallest fraction
    // of a step any of them was cut back to. `rigHolds` is counted apart from
    // `holds` because one rig-step moves a dozen bodies and adding it to the
    // per-body total would make the two incomparable across a change like the
    // one that introduced it.
    uint32_t rigHolds = 0;
    float minRigFrac = 1.0f;
  };
  const UntunnelProbe& Untunnel() const { return untunnel_; }
  void ResetUntunnelProbe() { untunnel_ = UntunnelProbe{}; }

  bool BodyActive(uint32_t i) const;
  uint32_t PendingEvents() const { return (uint32_t)events_.size(); }
  uint32_t SettledBack() const { return settledBack_; }

  // ---- why a body did not sleep (CLAUDE.md rule 6) ------------------------
  //
  // "N bodies awake" is a bare count, and it is exactly the shape of number
  // that rule 6 says not to bisect: the `debris` and `settle-back` gates were
  // handed over as known-failing with a paragraph of ruled-out hypotheses and
  // no cause, after the terrain overhaul spent a dozen elimination runs on
  // them. A body cannot sleep through a terrain patch rebuilding under it —
  // ManageTerrain wakes everything within 24 voxels whenever a chunk's
  // collision SURFACE changes, and SettleBodies needs 60 CONSECUTIVE inactive
  // ticks — so record the wake WHERE IT HAPPENS, name the chunk that did it,
  // and the question stops needing a bisect. Two counters and an IVec3; no
  // allocation, no readback, nothing that costs anything when nothing wakes.
  struct SettleProbe {
    uint32_t terrainWakes = 0;      // ManageTerrain: collision surface moved
    uint32_t blastWakes = 0;        // PreTick: an island event fired
    uint32_t lastWakeTick = 0;      // the most recent of either
    IVec3 lastWakeChunk{};          // ...and the chunk whose mesh changed
    uint32_t maxInactiveTicks = 0;  // longest quiet run ANY body managed
    // ManageTerrain's budget, observed: real rebuilds, rebuilds pushed to a
    // later tick by kTerrainBuildsPerTick, and stale entries whose occupancy
    // box hashed identical (no mesh, no Jolt, no wake).
    uint32_t terrainBuilds = 0;
    uint32_t terrainDeferred = 0;
    uint32_t terrainSame = 0;
    // ...and stale entries that did not even get their occupancy read, held by
    // kTerrainGatherPerTick. Counted APART from terrainDeferred because the
    // two mean different things: a build deferral is a mesh that is late, a
    // gather deferral is a chunk whose surface has not been LOOKED at yet.
    uint32_t terrainGatherDeferred = 0;
  };
  const SettleProbe& Settle() const { return settle_; }
  // Diagnostic: the mirror version the terrain collider for `wc` was last
  // built from (0 = no entry). A body whose chunk reads older than the tick
  // its cells were vacated is standing inside a collider of its own shape.
  uint32_t TerrainBuiltVersion(IVec3 wc) const {
    auto it = terrain_.find(World::PackChunkKey(wc));
    return it == terrain_.end() ? 0u : it->second.builtVersion;
  }
  void ResetSettleProbe() { settle_ = SettleProbe{}; }

  // ---- WHERE THE TICK WENT (CLAUDE.md rule 6, applied to TIME) ------------
  //
  // "2 fps for five seconds when a tree enters the rigidbody system" is a bare
  // number of exactly the kind rule 6 says not to bisect, and this system has
  // eight plausible suspects inside one PreTick: the event drain, the flood,
  // the Jolt body build, the burn/bleed/settle sweeps, and four separable
  // halves of ManageTerrain (the need sweep, the per-chunk staleness scan, the
  // 18^3 occupancy gather, the marching cubes, the Jolt mesh tree). Turning
  // them off one at a time buys one hypothesis per run. Timing them where they
  // happen buys all of them in ONE run, with denominators attached — which is
  // what turns "a tick cost 90 ms" into "728 chunks needed, 722 gathers, 6
  // polygonizes".
  //
  // OFF by default and gated on a bool, so the clock reads (two per phase per
  // tick, plus two per chunk in the terrain loop) cost nothing in the game.
  // Turn on with SANDVOX_DEBRIS_PROFILE=1 or SetProfiling(true); the
  // `tree-fell` gate does the latter around its cut.
  enum class Phase : uint8_t {
    EventDrain,     // queue probe + EventReady, minus the scans themselves
    IslandScan,     // RunIslandDetection: the flood, the shard dice, the ops
    BodyCreate,     // Physics::CreateDebrisBody* + welds, inside a scan
    Burn,
    Bleed,
    Settle,
    TerrainNeed,    // ManageTerrain: needAhead sweep + sort/unique
    TerrainScan,    // ...per-chunk cache lookup, refresh logic, vacate key
    TerrainGather,  // ...the 18^3 occupancy gather + its hash
    TerrainPoly,    // ...marching cubes
    TerrainJolt,    // ...RemoveBody + CreateTerrainMesh + WakeNear
    TerrainEvict,
    Instances,      // BuildInstances (render-side, billed apart)
    Count
  };
  static constexpr int kPhaseCount = (int)Phase::Count;
  static const char* PhaseName(Phase p);
  struct PhaseProfile {
    bool on = false;
    // Print + reset every 300 ticks. Set ONLY by SANDVOX_DEBRIS_PROFILE, never
    // by SetProfiling: a gate drives its own window and would find the numbers
    // it came for wiped on the last tick of it.
    bool autoReport = false;
    double totalUs[kPhaseCount] = {};
    uint64_t calls[kPhaseCount] = {};
    double curUs[kPhaseCount] = {};   // this tick, rolled up by PreTick's tail
    // The single most expensive PreTick seen since the last reset, with its
    // whole breakdown: the hitch IS the worst tick, and an average over 300
    // ticks hides it completely.
    double worstTickUs = 0;
    uint32_t worstTick = 0;
    double worstUs[kPhaseCount] = {};
    uint32_t ticks = 0;
    double tickUsTotal = 0;
    uint32_t ticksOver8ms = 0, ticksOver16ms = 0, ticksOver33ms = 0;
    // Denominators (rule 6 again: a duration with no count beside it is as
    // bare as a count with no duration).
    uint64_t chunksNeeded = 0, fetchesAsked = 0, gathers = 0, polys = 0,
             joltMeshes = 0, bodiesCreated = 0, bodyVoxCreated = 0;
    uint32_t maxNeededOneTick = 0, maxFetchOneTick = 0;
    // The need sweep's own denominators: occupied lattice blocks transformed
    // (bodies) and chunks the mob anchors listed (core every tick, horizon on
    // its stride) -- so "chunks needed" can be split between the two.
    uint64_t needBlocks = 0, anchorChunks = 0;
  };
  const PhaseProfile& Profile() const { return prof_; }
  void SetProfiling(bool on) { prof_.on = on; }
  void ResetProfile() {
    const bool on = prof_.on, auto_ = prof_.autoReport;
    prof_ = PhaseProfile{};
    prof_.on = on;
    prof_.autoReport = auto_;
  }
  // One line per phase that cost anything, worst tick first. Never empty.
  std::string ProfileReport() const;

  // ---- floater attribution (the grid <-> body handoff's leak sites) -------
  //
  // WHY THIS EXISTS, and why it is counters rather than a bisect.
  //
  // A CLASS_SOLID voxel never moves in the CA (sim_step returns early for it),
  // so island detection is the ONLY mechanism that makes solid matter fall.
  // Every component this file declines to convert stays exactly where it is,
  // forever, with nothing downstream to catch it — which makes "something is
  // floating" a bare symptom with eight distinct possible causes and no way to
  // tell them apart from the outside. That is precisely the number CLAUDE.md
  // rule 6 says not to bisect: elimination buys one hypothesis per run, while
  // recording the cause AT THE SITE buys all of them at once.
  //
  // So every path that declines a component increments the counter naming WHY.
  // The `floaters` gate prints these beside its own sweep of the world, and
  // the pair answers "what is floating" and "which door did it leave by" in a
  // single run. Increments only; no allocation, no readback, nothing that
  // costs anything when nothing is declined.
  //
  // The distinction that matters when reading them: `deferred*` counters are
  // the FIXES working (matter stayed in the grid on purpose and the event was
  // re-queued to come back for it), while `*GaveUp` and `solidRubbleInPlace`
  // are genuine leaks. A non-zero `deferred*` is healthy; a non-zero
  // `*GaveUp` is a floater that got away.
  struct FloaterProbe {
    // --- leaks: matter left in the grid with no path back ---
    uint32_t oversizeBboxSkipped = 0;   // component wider than the int8 lattice
    uint32_t solidRubbleInPlace = 0;    // a SOLID scrap written back where it hung
    uint32_t stuckEventDropped = 0;     // region left the window: nothing there
    uint32_t deferGaveUp = 0;           // re-queue budget exhausted
    uint32_t eventQueueFullDropped = 0; // queue full AND the spill also failed
    // NOT a leak: the pendingSupport_ drain found the event queue full and left
    // the chunk queued for next tick. Back-pressure, which is the queue working.
    uint32_t drainBackpressure = 0;
    // --- deferrals: the fixes doing their job ---
    uint32_t deferredCellOpBudget = 0;  // op budget ran out mid-scan
    uint32_t deferredSpawnRing = 0;     // particle ring full, cells left alone
    uint32_t deferredOversize = 0;      // oversize component held for a re-scan
    uint32_t eventQueueFullSpilled = 0; // queue full -> pendingSupport_ instead
    uint32_t settleWithoutSupport = 0;  // a settle refused for want of ground
    // An event whose chunks never arrived, spilled back to pendingSupport_
    // instead of being thrown away. In the deferral column, not the leak
    // column: the region is still resident, so the scan is late, not lost.
    uint32_t stuckEventRequeued = 0;
    // Support flags the per-chunk cooldown suppressed, and how many of those
    // were later promoted once it expired. `held` with a zero `rearmed` means
    // the re-arm is not running; both zero means the cooldown never fires and
    // is not the reason anything floats.
    uint32_t supportLateHeld = 0;
    uint32_t supportLateRearmed = 0;
    // Scan cost, so a latency claim can be checked against the work it did:
    // cells the flood actually visited, against cells the regions covered. The
    // ratio is what seeding from the changed box and stopping at the first
    // anchor buy; if it drifts back toward 1.0 the scan is labelling terrain
    // again.
    uint64_t scanCellsVisited = 0;
    uint64_t scanCellsCovered = 0;
    uint32_t scans = 0;
    // --- why components were judged anchored (context, not a verdict) ---
    // A component can be anchored for a good reason (the structure really does
    // continue outside the scan box) or a conservative one (we could not see
    // what is out there). The second is the documented source of the "large
    // floating sections survive" flaw, and separating them is what makes the
    // gate's number diagnosable instead of merely alarming.
    uint32_t anchoredByRegionBoundary = 0;  // solid genuinely continues outside
    uint32_t anchoredByUnknownChunk = 0;    // unfetched / out-of-window: assumed
    uint32_t anchoredByOversizeFlood = 0;   // over kMaxIslandVoxels: unjudgeable
    // ---- the same three, for components too SMALL to be anything -----------
    //
    // The counters above are dominated by bulk: a scan over terrain anchors the
    // ground every time and reports tens of thousands, which is correct and
    // says nothing. A component of one to seven voxels is a different claim
    // entirely — a lone burnt twig is not "structure that continues outside the
    // box", and if one is being anchored then the anchor test is what is
    // leaving specks in the air. Split out so the two questions stop sharing a
    // number (CLAUDE.md rule 6), and cheap: three increments on a branch that
    // only runs for components under the body floor.
    uint32_t smallAnchoredBoundary = 0;
    uint32_t smallAnchoredUnknown = 0;
    uint32_t smallAnchoredPowder = 0;  // resting on powder: a legitimate anchor
    uint32_t smallUnanchored = 0;      // ...and how many were freed, for scale
    uint32_t deferredUnfetched = 0;    // components held for a chunk fetch
    uint32_t deferGaveUpFetch = 0;     // ...of deferGaveUp, those waiting on one
    uint32_t fetchWaitNoCache = 0;     // a flood met a chunk never fetched
    uint32_t fetchWaitNoVoxels = 0;    // ...or cached without a voxel copy
    uint32_t fetchWaitStale = 0;       // ...or cached older than the event
    uint32_t sharedSeedCells = 0;      // seed cells an earlier scan this tick had labelled
    uint32_t speculativeFetches = 0;   // ring / column chunks asked for ahead of the flood
    IVec3 gaveUpChunk{}, gaveUpSeedLo{}, gaveUpSeedHi{};  // the last fetch give-up
    uint32_t gaveUpChunkInWindow = 0, gaveUpChunkCached = 0;
    uint32_t deferredBodyCap = 0;      // an assembly did not fit kMaxBodies
    uint32_t shardsMade = 0;           // bodies cut from islands
    uint32_t weldsMade = 0;            // fixed joints between shards
    // ---- buoyancy (FloatBodies) -------------------------------------------
    // `floatedBodies` is impulses applied, not bodies afloat: one body being
    // pushed for 40 ticks counts 40, which is what a rate question wants.
    // `floatProbesWet` against `floatProbes` is the cost claim — the dry case
    // is meant to be two mirror reads and out, so a ratio near 1 means the
    // early-out is not working and every body in the world is scanning a
    // column it will never be in.
    uint32_t floatedBodies = 0;
    uint32_t floatProbes = 0;
    uint32_t floatProbesWet = 0;
    // The last waterline FloatBodies acted on, so a gate can say WHERE a body
    // is floating rather than only that something was pushed.
    float floatLastSurfaceY = 0;
  };
  const FloaterProbe& Floaters() const { return floaters_; }
  void ResetFloaterProbe() { floaters_ = FloaterProbe{}; }

  // ---- persistence (sim/worldio.h, entities.sve section 'DBRS') -----------
  // Everything a body IS travels: collider + skin lattices, transform, scales,
  // bleed material. Jolt handles do NOT survive a session — load recreates
  // each body at its saved pose with zero velocity, ASLEEP (worldio.h's
  // rigidbody rule), so a settled pile reloads settled. The micro brick is
  // NOT serialized: it is derived render state, re-packed on load from the
  // authoritative lattice the same way ReskinMicro derives it after a carve
  // (CLAUDE.md architecture guideline 3: derived data is reconstructible).
  //
  // 2 (2026-09-13): DebrisVoxel::stain went from a byte holding a palette slot
  // to the 16-bit coat word holding a MATERIAL. The lattices are written as
  // PODs, so the stride moved and a version-1 section would load garbage
  // coordinates — old sections are refused, as they already are.
  //
  // 3 (2026-09-13): the STRAP (StrapBody) — host index + rigid offset per body.
  // A worn shell on a corpse is a follower of the limb it covers, and a section
  // that did not carry the relationship reloaded an armoured corpse as a pile
  // of free bodies sharing the same space, which is the motor by another route.
  //
  // 4 (2026-09-15): the DYE (game/dye.h). One word per body, beside the scales
  // it belongs with. It is render state like the brick above — but UNLIKE the
  // brick it is not derivable from the lattice, because the whole point of a
  // dye is that the art it colours is a neutral greyscale weave. A dropped red
  // shirt that reloaded grey would be the one visible way this system could
  // silently lose data, so the word travels.
  //
  // 5 (2026-09-26): THE BRUISE BYTE (voxload.h PrefabVoxel::bruise), in what
  // was the skin voxel's padding byte. Same stride, so a v4 section still
  // LOADS -- with that byte zeroed, because in v4 it was never written.
  static constexpr uint32_t kSaveVersion = 5;
  void SaveState(std::vector<uint8_t>& out) const;
  // Contract (worldio LoadEntities): Reset() has already run.
  bool LoadState(const uint8_t* data, size_t len, uint32_t version);

  // ---- break events -------------------------------------------------------
  // One entry per island that detached into a rigidbody this tick. Reported
  // rather than voiced here: this layer knows nothing about audio, the same
  // way it hands cellOps back instead of writing the grid itself. main.cpp
  // drains these into audio::Cues::Break after the tick, which also keeps the
  // cue out of the headless selftest path for free.
  struct BreakEvent {
    Vec3 posVoxel;        // centre of the piece, world voxels
    uint32_t material;    // dominant material of the piece
    int32_t sizeVoxels;   // how big it was — drives pitch
  };
  const std::vector<BreakEvent>& BreakEvents() const { return breaks_; }
  void ClearBreakEvents() { breaks_.clear(); }

  // ---- impact events ------------------------------------------------------
  // One entry per debris body that STRUCK something hard enough to be heard
  // this step. Same reporting shape as BreakEvent, and for the same reason:
  // this layer resolves the material and the energy, main.cpp turns it into
  // audio::Cues::Impact, and nothing here knows that audio exists.
  //
  // Three separate bounds keep a collapsing wall from machine-gunning the
  // mixer (CLAUDE.md rule 2 — bound every emergent process):
  //   1. Physics' contact listener drops anything below the speed gate, so a
  //      settled pile reports nothing at all and costs nothing.
  //   2. Per body: one impact per `impactMinGap` seconds. A tumbling rock
  //      strikes four times in a bounce; it should be one thud, not four.
  //   3. Per step: the loudest kMaxImpactsPerStep survive. Thirty pieces of a
  //      blown wall landing together is not thirty sounds under any sound
  //      design, and the voice pool would drop the tail anyway — better to
  //      choose WHICH ones are dropped than to let arrival order decide.
  struct ImpactEvent {
    Vec3 posVoxel;      // contact point, world voxels
    uint32_t material;  // the material STRUCK, resolved below
    float energy;       // 0..1, mapped from approach speed
  };
  const std::vector<ImpactEvent>& ImpactEvents() const { return impacts_; }
  void ClearImpactEvents() { impacts_.clear(); }

 private:
  struct Event {
    uint32_t tick;
    IVec3 lo, hi;  // voxel box (inclusive), already expanded + clamped
    // How many times this event has been DEFERRED and re-queued because a
    // budget ran out mid-scan (see RunIslandDetection's `deferEvent`). Bounded
    // so a region whose components never fit cannot cycle forever; on
    // exhaustion the give-up is COUNTED rather than silent, which is the whole
    // point of the floater probe below.
    uint8_t retries = 0;
    // Deferred because the flood reached a chunk the mirror does not hold yet
    // (requested; the scan runs again when it lands). Its own counter, with a
    // higher ceiling, because a tree crown is fetched in rounds and none of
    // those rounds is a budget failure.
    uint8_t fetchRetries = 0;
    // The box the caller actually named, BEFORE the margin was added: the
    // erased cells, the flagged chunk. The flood seeds only from solids in
    // (and one cell around) this box -- see RunIslandDetection -- because a
    // component that does not touch what changed did not lose its support
    // here, and labelling the whole terrain slab in a 64^3 region for every
    // scan was most of what a scan cost.
    IVec3 seedLo{}, seedHi{};
    // Deferred for a FETCH: the first chunk the flood reached that the mirror
    // did not hold. EventReady holds the event until that chunk's copy has
    // landed, and only then does the re-scan (and the next fetchRetries
    // increment) happen. Without this the re-queued event was ready again
    // the same tick -- its SEED box was cached, only the crown was not -- and
    // the drain loop ran it 32 times in two ticks, long before any readback
    // could land: in the live game every scan that left the 3x3x3 mirror gave
    // up on the spot, and a cut tree 48 voxels from the player never fell.
    // The gates never saw it because they force-fetch their whole fixture box
    // every tick (--fell-tree, 2026-09-12: 99 scans, 3 give-ups, 0 bodies).
    bool waiting = false;
    IVec3 waitChunk{};
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
    BodyWound wound;             // where, and how much (see BodyWound)
    // ---- THE STRAP (StrapBody) ------------------------------------------
    // Nonzero = this body is a FOLLOWER of that one: kinematic, pose derived
    // every PostStep, invisible to the solver. `wornRel*` is its own frame
    // expressed in the host's, captured once when the strap was tied, so the
    // pair is rigid rather than re-fitted per tick.
    uint64_t wornHost = 0;
    Vec3 wornRelPos{};
    float wornRelQuat[4] = {0, 0, 0, 1};
    bool Follower() const { return wornHost != 0; }
    uint32_t inactiveTicks = 0;  // settle-back countdown (PLAN §B6)
    // Nonzero = one shard of a welded island (PLAN_rigidbody_islands.md §5):
    // every body sharing the id was cut from the same component and is joined
    // to its neighbours by fixed joints. Settle-back treats the set as a unit.
    uint32_t assembly = 0;
    // Impact cue bookkeeping. `domMat` is this body's most common material,
    // cached because it is the fallback the impact cue reaches for when the
    // struck side is another body rather than the grid, and recounting a
    // 32k-voxel lattice per contact would not be free. Refreshed wherever the
    // voxel list is rewritten (adoption, damage, shatter, load).
    uint32_t domMat = 0;
    uint32_t lastImpactStep = 0;  // rate limit, in PostStep counts (30 Hz)
    // body burn (fire continuity on rigidbodies):
    uint32_t serial = 0;          // stable RNG stream id (bodies_ reshuffles)
    // ---- WHO STEPS THIS BODY (M9.4-C) -------------------------------------
    //
    // `owner` is the player id whose machine simulates it RIGHT NOW; every
    // other machine holds a kinematic GHOST at the same global id, posed from
    // the wire and authoring nothing. `ownerAtCreate` is who MINTED it and
    // never changes, because it is half of the global id: `(ownerAtCreate <<
    // 48) | serial` names this body on both machines with no handshake and no
    // allocator (net/debrissync.h MakeGlobalBodyId). Handing a body over moves
    // `owner` and leaves `ownerAtCreate` alone — a sword you threw is still a
    // sword you threw whoever is stepping it this second.
    //
    // Both default to kLocalOwner, so a single-player process has every body
    // owned by the one player and takes exactly today's path: the ownership
    // function is null, nothing ever flips, and the world hash does not move.
    uint32_t owner = kLocalOwner;
    uint32_t ownerAtCreate = kLocalOwner;
    // The newest pose received for a ghost, and the tick it was stamped with.
    // `poseTick` is what makes an out-of-order datagram harmless: a pose older
    // than the one already held is dropped rather than dragging the body
    // backwards. Meaningless on an owned body and never read there.
    BodyTransform ghostXf{};
    Vec3 ghostVel{};
    uint32_t poseTick = 0;
    bool hasPose = false;
    uint16_t activeCount = 0;     // voxels with UNCONDITIONAL self rules: alight
    uint16_t pairCount = 0;       // voxels with pair rules (ignitable/dousable)
    // voxels whose only self rules are neighbour-count gated (charred flesh
    // relighting, cooked flesh catching): they need something hot NEXT to
    // them, which is either this body's own alight voxels (activeCount) or
    // the world's fire (a dirty chunk). Counted apart so a body that is
    // nothing but char sleeps -- see BurnBodies' willScan.
    uint16_t scaledCount = 0;
    // voxels the GRID can act on through somebody else's rule (acid eating
    // flesh). Counted apart from pairCount because it answers a different
    // question: pairCount asks what this body can do, this asks what can be
    // done TO it, and only the second one justifies the threat probe below.
    uint16_t inboundCount = 0;
    // ---- CAN THIS BODY REACT WITH ITSELF? ----------------------------------
    // True when some material present in the lattice authors a pair rule whose
    // neighbour predicate matches another material that is ALSO present. That
    // is rot on a corpse: `rotflesh + skin -> rotflesh` needs no fire, no
    // dirty chunk and nothing outside the body at all, and because `willScan`
    // demanded a dirty chunk nearby for every non-alight body, a corpse lying
    // in a settled world stopped rotting the moment the world went quiet.
    //
    // It is what keeps that fix inside rule 2. A body of plain skin, flesh and
    // bone sets this FALSE (skin's only pair rule wants tag:hot, and no bone
    // is hot), so an ordinary corpse still sleeps; a rotting one scans until
    // the front runs out of tissue, at which point the next RecountBurn clears
    // the flag and it sleeps too. Recomputed wherever the lattice is rewritten.
    bool internalPair = false;
    // ---- THIS BODY WAS ALIVE (2026-09-19) ----------------------------------
    // Set by the adopting caller for anything that came off a creature: a
    // severed limb, and every piece of a corpse. Inherited by fragments, so
    // half a head cut off a corpse is still dead flesh rather than debris.
    //
    // Nothing gates on it today, deliberately. It exists so the rules that
    // WILL want to know ("rot spreads on the dead and not on the living",
    // "the dead do not flinch") have a fact to ask rather than having to
    // infer one from `bleedMat` — which means "bleeds when cut" and is
    // already true of a live limb.
    //
    // 2026-09-20: two rules ask now — the melee sweep reports a blow on dead
    // flesh as FLESH rather than as a chip, and `GoreEvent` gives a corpse its
    // own species' wet noises.
    bool dead = false;
    // The mob this was part of (its id), stamped by Mob::Die and inherited by
    // fragments like `dead`. Groups a corpse's pieces for the heat that
    // crosses its joints (MobSystem::BurnDeadFlesh). 0 = no creature.
    uint64_t creature = 0;
    // ---- WHAT WAS HOLDING EACH JOINT WHEN THE BLOWS STARTED ---------------
    // One entry per joint on this body (joint handle -> flesh count within
    // gore.corpseJointHold of its anchor), captured the first time a carve
    // asks and never re-raised. The denominator of the parting rule, and the
    // debris twin of MobLimb::neckAtSpawn — taken lazily for the same reason
    // that one is: at spawn there is nothing to measure against yet.
    std::vector<std::pair<uint64_t, uint32_t>> jointHold;
    // Something beat this body hard enough to pulp tissue: PulpTick is eating
    // it. Cleared when nothing pulped is left (the same self-clearing flag
    // Mob::BluntPulpTick uses, and for the same rule-2 reason).
    bool pulp = false;
    // WHICH CREATURE THIS WAS, an index into MobSystem::Defs(), or -1 for
    // matter that was never alive. Carried for the same reason the sever event
    // carries one: the mob is despawned long before anything asks, and a
    // corpse being hacked apart should make ITS OWN species' sounds.
    int defIndex = -1;
    uint32_t burnedSinceRebuild = 0;  // batched collider refresh threshold
    uint32_t burnedSinceShatter = 0;  // batched connectivity re-check
    // ---- THE COLLIDER FOOTPRINT (ManageTerrain) --------------------------
    // The local lattice's own AABB, in `voxels` units. ManageTerrain asks for
    // terrain patches around THIS rather than around `radiusVoxels`, and the
    // difference is the whole reason a felled tree used to cost 900 chunks a
    // tick: a bounding sphere of a 59 x 81 x 59 crown has radius 60, its
    // radius+6 box is 133 voxels a side (~900 chunks), and the box the body
    // can actually touch is 150.
    //
    // CACHED ON THE VOXEL COUNT, which is what every geometry edit in this
    // file changes: adoption, carve, shatter, split and settle all add or
    // remove voxels. Burning rewrites PAYLOADS in place and moves nothing, so
    // it correctly does not invalidate this. A REBASE moves every coordinate
    // and keeps the count, so it bumps `geomGen`, the second key. Recomputed lazily by
    // RefreshLocalBounds, so no creation site has to remember to call it.
    int8_t lmin[3] = {0, 0, 0};
    int8_t lmax[3] = {0, 0, 0};
    uint32_t boundsCount = 0xFFFFFFFFu;  // != voxels.size() => recompute
    uint32_t boundsGen = 0xFFFFFFFFu;    // != geomGen => recompute
    // ---- THE LATTICE'S COORDINATES MOVED WITHOUT ITS COUNT MOVING ----------
    // Bumped wherever every voxel coordinate is shifted in place: ReskinMicro
    // taking MicroBodyEdit's new min corner, and DamageBody's RebaseVoxels.
    // The voxel count cannot witness that (nothing was added or removed), so
    // every cache over lattice POSITIONS keys on (count, geomGen): the local
    // bounds above, MobSystem's corpse burn index (FleshView) and the armour
    // march index (MarchShell). Before this those three read neighbours and
    // boxes at the pre-shift coordinates until something changed the count.
    uint32_t geomGen = 0;
    // ...AND WHICH PARTS OF THAT BOX HOLD ANYTHING. The AABB of a rotated
    // 81-voxel trunk with a crown at one end is mostly empty, and the sweep
    // sat at kTerrainNeedCeiling (505 of 512 chunks a tick) for as long as
    // the oak tumbled. So the lattice box is diced into kNeedBlock^3 blocks
    // and one bit per block says whether any voxel lands in it; ManageTerrain
    // transforms only the set blocks and asks for the chunks THEY cover.
    // Same cache key as lmin/lmax (refreshed in the same call). `needDim` is
    // the block-grid size per axis, blocks indexed (z * dimY + y) * dimX + x.
    std::vector<uint64_t> needBlocks;
    uint8_t needDim[3] = {0, 0, 0};
  };
  static constexpr int kNeedBlock = 8;  // collider voxels per block side
  struct TerrainEntry {
    uint64_t handle = 0;
    IVec3 wc{};          // world chunk (streaming recycles slots, not chunks)
    uint32_t builtVersion = 0;
    uint32_t lastNeeded = 0;
    uint32_t lastRefreshReq = 0;
    // Identity of the pending-vacate lists (this chunk and its 26 neighbours)
    // the collider was last meshed against; a change forces a rebuild.
    uint64_t vacateKey = 0;
    uint64_t occHash = 0;   // collision-surface identity: the hash of the 18^3
                            // occupancy box the mesh is a pure function of, so
                            // liquids flowing through a chunk neither rebuild
                            // nor wake, and the compare runs BEFORE the mesh
  };

  // Are every one of this region's chunks cached at or past `required`? When
  // `requestFetch`, missing ones are asked for — plus the one-chunk RING around
  // the region, which `solidOutside` reads and which nothing used to fetch, so
  // a component touching a scan-box face was anchored on a guess. The ring is
  // requested but never waited for; see the comment at the call site.
  bool EventReady(const Event& e, World& world, bool requestFetch = true) const;
  uint32_t RequiredVersion(IVec3 wc, uint32_t eventTick) const;
  void NoteGridWrite(const IVec3& c, uint32_t tick, uint32_t word);
  // Push every world chunk the (already clamped) region covers onto
  // pendingSupport_. The overflow path for a full event queue; deduped by
  // supportPending_ exactly like a GPU support flag, so a region spilled twice
  // costs one entry.
  void SpillRegionToSupport(const Event& e);
  // Promote chunks the cooldown suppressed, once it has expired.
  void RearmLateSupport(uint32_t tick);
  // Is any voxel of a settle candidate's snapped footprint resting on grid
  // solid/powder? See the long note at the call site in SettleBodies — this is
  // what stops a body that was resting on ANOTHER BODY from stamping itself
  // into the world as permanently floating stone.
  //
  // `snap` is the signed-permutation basis SettleBodies derived, `base` the
  // rounded body origin. Chunks that are not cached are REQUESTED and read as
  // "no support", so a settle near an unfetched chunk waits for the fetch
  // instead of guessing — the same self-healing shape EventReady has.
  bool SettleFootprintSupported(const std::vector<DebrisVoxel>& src,
                                const int (*snap)[3], IVec3 base,
                                World& world) const;
  void RunIslandDetection(const Event& e, uint32_t tick, World& world,
                          std::vector<CellOp>& cellOps,
                          std::vector<ParticleSpawn>& spawns);
  void ManageTerrain(uint32_t tick, World& world);
  // Refresh Body::lmin/lmax if the voxel count moved. See the note there.
  static void RefreshLocalBounds(Body& b);
  // Body burn: the reaction table over the voxel payloads of every NON-flesh
  // body, so detached matter keeps burning (embers advance to ash, emit real
  // fire into the grid via fill-air-only ops, and grid fire ignites cold
  // bodies through the chunk cache). This pass only SCHEDULES -- which bodies,
  // what share of the budget -- and runs the tail; the evaluator is the
  // installed body reactor (SetBodyReactor). Idle bodies cost nothing.
  void BurnBodies(uint32_t tick, World& world, std::vector<CellOp>& cellOps,
                  std::vector<ParticleSpawn>& spawns);
  // The body tail over one burn pass's bodies (indices into bodies_, with
  // what the evaluator reported for each): BurnTail per body, then erase the
  // ones that burned away and append the fragments. `changedOnly` skips the
  // tail for a body the pass did not change (the flesh pass's rule).
  void FinishBurn(const std::vector<size_t>& which,
                  const std::vector<FleshBurn>& out, bool changedOnly,
                  World& world, std::vector<ParticleSpawn>& spawns);
  void RecountBurn(Body& b) const;
  FleshLattice FleshOf(Body& b);
  // Everything a burn pass leaves for a BODY to do once it has tombstoned
  // `removed` voxels: compact, shatter, hand a sub-body remnant to particles,
  // rebuild the collider on the batched cadence. Shared by BurnBodies and
  // BurnFleshBodies. True = the body burned below body-worthiness and has been
  // released; the caller erases it.
  bool BurnTail(Body& b, uint32_t removed, bool changed, World& world,
                std::vector<Body>& fragments, std::vector<ParticleSpawn>& spawns,
                uint32_t& newBodyBudget, bool& rebuiltOne);
  // Corpse bleeding: every body with an open wound drips and gouts from it,
  // on the live wound's own tuning (gore.sever* for the gout, gore.bleed* for
  // the drip). Idle bodies cost two field reads.
  void BleedBodies(uint32_t tick, World& world,
                   std::vector<ParticleSpawn>& spawns);
  // Open (or deepen) a wound at the body voxel nearest `woundW` (world), with
  // blood leaving along `dirW` when the body's own shape gives no direction.
  // Reads b.xf as it stands, so call it AFTER any rebase. `budget` goes
  // through AddBleedBudget; `gushTicks` is a max.
  void ArmWound(Body& b, Vec3 woundW, Vec3 dirW, float budget,
                int gushTicks) const;
  bool AnyDirtyNear(const Body& b, const WorldSnapshot& snap, World& world) const;
  // Is a material that REWRITES ITS NEIGHBOUR (acid, a solvent) sitting in one
  // of the world cells this body occupies? The still-pool companion to
  // AnyDirtyNear — see the implementation for why dirtiness is the wrong proxy
  // for a solvent and why this is a wake signal rather than a containment test.
  bool ThreatNear(const Body& b, World& world) const;
  // Break a body whose voxels no longer form one 6-connected component: the
  // largest piece keeps the body, fragments >= `minFragment` voxels become
  // bodies of their own while `budget` allows (parent collider rebuilt
  // immediately), everything else re-enters the world as ballistic particles
  // with the body's point velocity — break a body enough and it just turns
  // back into loose voxels. `budget` is decremented per body created and is
  // shared across all bodies in a tick, so a disintegrating object cannot
  // spawn an unbounded fleet of fragments.
  // `fineConnectivity` lets the split ESCALATE to the skin lattice when the
  // collider says the body is still in one piece. Only a CARVE passes it: a
  // blade's kerf is narrower than a collider block and can cut clean through
  // the art without ever emptying one (see the note in ShatterBody), while
  // burning erodes at every scale at once and disconnects the collider on its
  // own. The distinction is a budget one — the fine flood is `skinScale^3`
  // times the nodes, and the burn path runs the check every few voxels lost.
  void ShatterBody(Body& b, World& world, std::vector<Body>& fragments,
                   std::vector<ParticleSpawn>& spawns, uint32_t minFragment,
                   uint32_t& budget, bool fineConnectivity = false);
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
                  bool eject, const CarveFactory& carveAt,
                  DamageCause cause = DamageCause::Other,
                  float severity = 1.0f, const SpallParams* spall = nullptr);
  // ---- A CORPSE COMES APART WHERE IT WAS CUT (2026-09-20) -----------------
  //
  // Severing on this side used to be connectivity INSIDE one body, so a blade
  // could part a corpse's neck completely and the head stayed on: the joints
  // Mob::Die leaves are what hold a corpse together and nothing ever cut one.
  // This is the same rule the living sever by, one population later — the
  // flesh AT the joint is gone, so there is nothing left to hold — asked of
  // every joint on a body that was just carved. Returns how many let go, and
  // pushes a GoreEvent for each.
  uint32_t PartJointsAt(Body& b, DamageCause cause, float severity);
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
  // Put every follower back on its host, LAST in PostStep — after the cull, so
  // a host that left the window this tick is already gone and its garment
  // unstraps instead of being driven off a dead handle, and after the readback,
  // so the host transform it derives from is this tick's final one.
  void DriveStraps();
  // ---- THE ONE KINEMATIC DRIVE -------------------------------------------
  //
  // Put a kinematic body EXACTLY on a pose and give it the rigid-body velocity
  // that pose implies. Both callers are "this body's pose is decided
  // elsewhere": DriveStraps derives it from a local host limb, DriveGhosts
  // takes it off the wire. Factored because the two must agree about the three
  // non-obvious parts, each of which was a bug on the strap path first:
  //   - SetBodyTransform, not MoveKinematicBody: the latter aims a body at a
  //     pose it reaches at the END of the next step, and one tick of lag on a
  //     body falling at 40 m/s is four voxels of daylight.
  //   - the velocity must be written too, or a kinematic body integrates away
  //     from where it was just put and reports every contact as a standing hit.
  //   - Body::xf must be updated to match, because the render instance, the
  //     terrain sweep and every world-space query read it rather than Jolt.
  // False when physics refused the handle (it is dead); the caller decides
  // what that means, which is the ONE thing the two paths do differently.
  bool DriveKinematicTo(Body& b, Vec3 pos, const float quat[4], Vec3 lin,
                        Vec3 ang);
  // Pose every ghost from its newest received pose. Called from PreTick,
  // before anything reads a body position this tick.
  void DriveGhosts();
  // Re-ask the ownership function for every body and act on the answers:
  // a body that became someone else's is flipped to a kinematic ghost, one
  // that came back is released to dynamic. No-op when no function is set,
  // which is what keeps a single-player tick identical to today's.
  void RefreshOwnership();
  // Turn a body into a ghost / back into simulated matter, in place.
  void MakeGhost(Body& b, uint32_t newOwner);
  void MakeOwned(Body& b);
  // IS THIS BODY STEPPED BY THIS MACHINE? The one question every emitter
  // asks. Inline and branch-free-cheap on purpose: it is in five per-body
  // loops and a single-player run must not pay for it.
  bool OwnedLocally(const Body& b) const { return b.owner == localPlayerId_; }
  // IS THIS REGION OF GRID MINE TO SCAN? Null function = yes, always.
  bool ChunkOwned(IVec3 wc) const {
    return !chunkOwnedFn_ || chunkOwnedFn_(wc);
  }
  // The world chunk an event's SEED box sits in -- the box that actually
  // changed, not the margin-grown region, because the margin can reach across
  // an authority seam while the change itself did not.
  IVec3 EventChunk(const Event& e) const {
    // `>> 5` is (lo+hi)/2 then /kChunk in one step -- the same expression the
    // stuck-event path below already uses. kChunk is 16, so the halving and
    // the divide are four bits each.
    return IVec3{(e.seedLo.x + e.seedHi.x) >> 5,
                 (e.seedLo.y + e.seedHi.y) >> 5,
                 (e.seedLo.z + e.seedHi.z) >> 5};
  }
  // Cut one strap: the body becomes ordinary dynamic debris carrying the
  // velocity it had as a follower (the rigid-body velocity at its own origin,
  // so a pauldron whose shoulder was spinning leaves along the tangent). Called
  // when the host stops existing — culled, settled back, burned away, looted.
  void UnstrapBody(Body& b);
  // A collider rebuild mints a new handle; the strap is keyed on handles. Call
  // beside every `b.handle = nh` the way ReplaceBody is called beside it for
  // joints — followers of `oldHandle` re-point, and a follower that was ITSELF
  // rebuilt goes back to kinematic (ReplaceBody does not carry motion type).
  void CarryStrap(uint64_t oldHandle, uint64_t newHandle);
  std::function<void(uint64_t)> onBodyGone_;
  // Settle-back (PLAN §B6): a long-asleep, near-axis-aligned body converts
  // its voxels to CellOps (fill-air-only: grid content wins deterministically
  // on the GPU) and frees its body. At most one body per tick.
  void SettleBodies(uint32_t tick, World& world, std::vector<CellOp>& cellOps);
  // Archimedes for bodies (docs/PLAN_debris_buoyancy.md phase 3). A felled
  // trunk is not particles — it is one Jolt body, and liquids are not in the
  // collider, so before this it sank through a lake like a stone. Reads the
  // waterline out of the CPU mirror and hands Jolt a surface plane; the
  // buoyancy factor is rhoLiquid/rhoBody off the same materials.json density
  // the particle kernel uses, so a chip blown off a log and the log agree.
  void FloatBodies(uint32_t tick, World& world);

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
  std::vector<std::string> matNames_;   // id -> name, for authored-by-name coats
  std::vector<uint8_t> foliageOf_;  // tag:foliage — sub-8 floaters vanish, no rubble
  // body burn tables (rebuilt on materials hot-reload; data-driven, no
  // hardcoded material IDs — the JSON stays the single source of behavior)
  std::vector<MaterialGpu> matGpu_;
  std::vector<ReactionGpu> reactions_;
  // What each material can do on a body: the ONE builder MobSystem uses too
  // (sim/bodyreact.h). Read here by RecountBurn (activeCount / scaledCount /
  // pairCount / inboundCount / internalPair) and the solvent probe
  // (ThreatNear); the evaluation itself is MobSystem::BurnOneLimb's.
  BodyReactFlags react_;
  // THE ONE BODY-REACTION EVALUATOR, installed by MobSystem (SetBodyReactor).
  // Null in a process with no MobSystem: then non-flesh bodies do not burn.
  BodyReactFn bodyReact_;
  BodyReactEndFn bodyReactEnd_;
  const void* bodyReactOwner_ = nullptr;   // see InstallBodyReactor
  // Where BurnBodies starts next tick: the first body the last pass could
  // not afford (a walk deferred, or the candidate budget spent), so the
  // bodies behind a budget-limited pass are first in line rather than
  // starved while a tick-rotated start creeps past them one body a tick.
  // Reset with the bodies (Reset).
  size_t burnNext_ = 0;
  // Scratch for RecountBurn's internal-pair test, one byte per material id.
  // A member rather than a local so a per-carve recount does not allocate.
  mutable std::vector<uint8_t> presentScratch_;
  mutable std::vector<uint32_t> foundScratch_;
  // Authored GRID tints per material id, 0x00RRGGBB (MaterialDef::tints).
  // Empty for everything that is not MATF_TINTED, which is almost everything.
  std::vector<std::vector<uint32_t>> matTints_;
  // Per material, merged-art-index -> tint index. Entry 0 is the unpainted
  // case and is always tint 0, the material's natural colour by convention —
  // so a body voxel nobody painted lands undyed rather than as whichever dye
  // happens to sit closest to nothing. Empty for an untinted material; see
  // RefreshTintMap.
  std::vector<std::vector<uint8_t>> tintOfArt_;
  uint64_t tintArtStamp_ = 0;   // content stamp of the palette it was built from
  bool tintMapValid_ = false;   // cleared by OnMaterialsReloaded
  uint32_t nextSerial_ = 1;
  // ---- ownership (M9.4-C) -------------------------------------------------
  // All defaulted so that a process that never calls the setters behaves
  // exactly as it did before this landed: one player, id 0, owning everything.
  uint32_t localPlayerId_ = kLocalOwner;
  std::function<uint32_t(uint64_t globalBodyId, Vec3 posVoxel)> ownershipFn_;
  std::function<bool(IVec3 wc)> chunkOwnedFn_;
  std::function<bool(uint64_t, ItemInstance&)> itemLookupFn_;
  std::function<bool(uint64_t)> itemTakeFn_;
  // Requests this machine has to SEND (E pressed on a ghost item) and grants
  // it has to CONSUME (its own pickups plus peers' replies). Plain deques:
  // both are drained to empty every tick by the caller, and a request that is
  // never drained is a transport that is not running, not a leak.
  std::deque<net::ItemTake> itemTakeOut_;
  std::deque<net::ItemGrant> itemGrantIn_;
  OwnerProbe ownerProbe_;
  std::deque<Event> events_;
  // support-loss plumbing: flagged chunks wait here until the event queue has
  // room (never dropped — a missed final event is a floating island forever).
  // Keyed by WORLD chunk: the window can shift before a flag drains.
  std::deque<IVec3> pendingSupport_;
  std::unordered_map<uint64_t, uint8_t> supportPending_;    // dedup (packed key)
  std::unordered_map<uint64_t, uint32_t> supportCooldown_;  // chunk -> last tick
  // Chunks whose flag the cooldown SUPPRESSED, held until it expires. Without
  // this the cooldown is a drop rather than a delay and the final state of a
  // burnt-out region — the one the player is looking at — is the one state no
  // scan ever runs on. Deduped per chunk and capped like pendingSupport_.
  std::deque<IVec3> supportLateQueue_;
  std::unordered_map<uint64_t, uint8_t> supportLate_;
  uint32_t lastSupportSnapTick_ = 0;
  std::vector<Body> bodies_;
  // ---- handle -> index into bodies_, SELF-VALIDATING ------------------------
  // The first index whose Body::handle is `handle`, or -1 — exactly what a
  // `for (b : bodies_) if (b.handle == handle)` scan returns. `handleIdx_` is
  // a HINT, not an index kept in step with bodies_: bodies_ is reshaped at
  // ~20 sites (swap-and-pop, erase, split, adopt), and a map maintained at
  // every one of them is a map that one day misses one. So a hit is trusted
  // only after checking `bodies_[i].handle == handle`, and anything else falls
  // back to the scan and re-learns. Handles are Jolt body ids and unique among
  // live bodies, which is what makes "the hinted index" and "the first match"
  // the same body. Lookups only: nothing iterates this map, so no order —
  // hashed or otherwise — depends on it. Handle 0 is never hinted.
  int IndexOfHandle(uint64_t handle) const;
  mutable std::unordered_map<uint64_t, uint32_t> handleIdx_;
  // Scan scratch, REUSED across scans. A 64^3 region is 262,144 cells and
  // RunIslandDetection used to construct three vectors over it per scan -- 2.4
  // MB allocated, zeroed and freed for every support-loss chunk, which is the
  // cost that capped the scan rate. Kept between calls and `assign`ed, so the
  // zeroing stays and the allocator leaves the hot path.
  std::vector<std::pair<IVec3, float>> terrainNeed_;  // ManageTerrain scratch
  std::vector<float> terrainNeedGrid_;  // per-body chunk grid, min distance
  // ---- THE FLOOD'S LABEL MAP: dense per chunk, shared across one tick -----
  //
  // Was an unordered_map keyed by packed world cell: a hash probe per cell
  // visited and per neighbour examined, 1-2.5 us a cell over a 28k-voxel oak.
  // Now a 4,096-entry int32 page per chunk the flood touches, allocated on
  // first touch (16 KiB), recycled across ticks, and addressed through the
  // chunk lookup the flood already caches (RunIslandDetection's `chunkOf`).
  // Sparse in CHUNKS rather than in cells, which is the granularity the
  // mirror has anyway: a scan still costs what it walks, not the 256^3 region.
  //
  // Cleared once per PreTick, NOT per scan, and that is the other half of the
  // point. Three events flood the same felled tree every round (the brush's
  // destruction event plus a GPU support flag for each chunk the cut touched)
  // and each used to walk all of it from its own seed. A seed cell an earlier
  // scan of this tick already labelled INHERITS that component's verdict from
  // tickComps_ instead of re-flooding it; a flood that runs into such a
  // component adopts its verdict the same way.
  //
  // Bounded by pages as the old map was bounded by kMaxIslandVoxels: past
  // kMaxLabelPages a component is anchored as oversize (unjudgeable), exactly
  // what the cell cap does. 2,048 pages is 32 MiB at a peak nothing real
  // reaches -- one oak is ~112 chunks -- and the pages persist only at the
  // high-water mark, not at the cap.
  //
  // `index` rides beside the label: the cell's position in its component's
  // cell list, valid only where `label` is that component's. The shard dice
  // and the weld pass used to rebuild two hash maps over a 28k-voxel tree to
  // answer "which cell is my neighbour" -- more than the flood itself cost.
  static constexpr uint32_t kMaxLabelPages = 2048;
  struct LabelPageData {
    std::array<int32_t, kChunkVol> label;  // -1 = unlabelled this tick
    std::array<int32_t, kChunkVol> index;  // meaningful only under a live label
  };
  std::unordered_map<uint64_t, uint32_t> labelPageOf_;  // chunk key -> page
  std::vector<std::unique_ptr<LabelPageData>> labelPages_;
  uint32_t labelPagesUsed_ = 0;
  LabelPageData* LabelPage(IVec3 wc);  // this tick's page for a chunk; nullptr past the cap
  // One verdict per component labelled this tick, indexed by label, for a
  // later scan of the same tick that meets the component's cells.
  enum : uint8_t { TICK_ANCHORED = 1, TICK_WAIT, TICK_HELD, TICK_MADE };
  struct TickComp {
    uint8_t verdict = 0;
    uint8_t anchorFlags = 0;  // 1 boundary, 2 unknown, 4 oversize, 8 powder
    IVec3 waitChunk{};        // TICK_WAIT: the chunk its event waits for
  };
  std::vector<TickComp> tickComps_;
  void BeginTickLabels();  // PreTick: forget the last tick's labels + verdicts
  struct Anchor {
    Vec3 pos;
    float radius = 0.0f;
    Vec3 vel{};  // voxels/s, for the lookahead sweep (may be zero)
    // The planning horizon (navRadius + 4 for a creature with a target), 0
    // when there is none. Listed on a stride, not every tick: see
    // kTerrainHorizonStride in ManageTerrain.
    float horizon = 0.0f;
  };
  std::vector<Anchor> extraAnchors_;                    // mob limbs, this tick
  std::unordered_map<uint64_t, TerrainEntry> terrain_;  // packed world chunk key
  uint32_t lastTerrainTick_ = 0;  // the sweep TerrainCensus reports on
  // Per chunk, the tick of the last grid write this system made into it
  // (island removal, rubble, settle-back). A scan may not read the chunk from
  // a mirror copy older than that, and ManageTerrain re-fetches it until the
  // copy catches up, whatever the dirty flags say. Pruned once satisfied.
  std::unordered_map<uint64_t, uint32_t> chunkWriteTick_;
  // THE OVERLAY: per chunk, every cell this system wrote since the mirror
  // last caught up, with the word it wrote. The flood and the collider read
  // the mirror THROUGH it, so neither ever sees a body's former cells as
  // matter (a second body from the same cells, or a mesh the body is born
  // inside -- gate `cactus-fell`), and neither has to wait for a fresh copy of
  // a chunk a fire is rewriting every tick. Dropped once the mirror's copy is
  // at or past `tick`.
  struct PendingVacate {
    uint32_t tick = 0;
    uint32_t stamp = 0;
    std::unordered_map<uint16_t, uint32_t> cells;  // local cell -> word
  };
  std::unordered_map<uint64_t, PendingVacate> pendingVacate_;
  uint32_t nextVacateStamp_ = 1;
  uint32_t nextAssembly_ = 1;
  PhaseProfile prof_{};
  // Scoped accumulator for PhaseProfile. Two clock reads when profiling is on,
  // one predictable branch when it is not.
  struct PhaseTimer {
    PhaseProfile* p;
    Phase ph;
    std::chrono::steady_clock::time_point t0;
    PhaseTimer(PhaseProfile& prof, Phase phase) : p(&prof), ph(phase) {
      if (p->on) t0 = std::chrono::steady_clock::now();
    }
    ~PhaseTimer() {
      if (!p->on) return;
      const double us =
          std::chrono::duration<double, std::micro>(
              std::chrono::steady_clock::now() - t0).count();
      p->curUs[(int)ph] += us;
      p->calls[(int)ph]++;
    }
    PhaseTimer(const PhaseTimer&) = delete;
    PhaseTimer& operator=(const PhaseTimer&) = delete;
  };
  // A timer that changes WHICH phase it is charging partway through. The
  // terrain loop's four stages (staleness scan, occupancy gather, marching
  // cubes, Jolt) are separated by `continue`s, so nested scopes cannot express
  // them without hoisting half the loop's locals; this charges what has
  // elapsed to the current phase and starts the next, and its destructor
  // charges the remainder however the iteration ended.
  struct PhaseSwitch {
    PhaseProfile* p;
    Phase ph;
    std::chrono::steady_clock::time_point t0;
    PhaseSwitch(PhaseProfile& prof, Phase phase) : p(&prof), ph(phase) {
      if (p->on) t0 = std::chrono::steady_clock::now();
    }
    void To(Phase next) {
      if (!p->on) { ph = next; return; }
      Charge();
      ph = next;
    }
    ~PhaseSwitch() { if (p->on) Charge(); }
    void Charge() {
      const auto now = std::chrono::steady_clock::now();
      p->curUs[(int)ph] +=
          std::chrono::duration<double, std::micro>(now - t0).count();
      p->calls[(int)ph]++;
      t0 = now;
    }
    PhaseSwitch(const PhaseSwitch&) = delete;
    PhaseSwitch& operator=(const PhaseSwitch&) = delete;
  };
  bool instancesDirty_ = false;
  std::vector<GoreEvent> gore_;   // drained per frame by main.cpp
  uint32_t instanceCount_ = 0;
  uint32_t settledBack_ = 0;
  SettleProbe settle_{};
  FloaterProbe floaters_{};
  UntunnelProbe untunnel_{};
  // Consecutive steps each body has been held at a collider boundary. Only
  // bodies actually being held have an entry (erased the step they stop being
  // held), so this is empty in every ordinary frame.
  std::unordered_map<uint64_t, uint8_t> untunnelHold_;
  // Drained by main.cpp each frame; bounded by the same per-tick body budget
  // that bounds island creation, so this cannot grow without limit.
  std::vector<BreakEvent> breaks_;
  // Drained by main.cpp each frame. Bounded per STEP by kMaxImpactsPerStep and
  // per body by the gap limiter, so a frame that ran four ticks can carry at
  // most 4 * kMaxImpactsPerStep entries.
  std::vector<ImpactEvent> impacts_;
  // PostStep counter, i.e. sim ticks: the unit the per-body impact gap is
  // measured in. Kept here rather than taking a tick parameter because every
  // one of the eight PostStep call sites would otherwise have to be taught
  // about a clock it does not need.
  uint32_t stepCount_ = 0;
  // Dominant material of a voxel lattice: the piece's MOST COMMON material,
  // not the first voxel's. Shared by the break and impact cues — see the
  // comment at the break site for why the first voxel is the wrong answer.
  static uint32_t DominantMaterial(const std::vector<DebrisVoxel>& voxels);
  // Turn this step's Jolt contacts into ImpactEvents. Called from PostStep.
  void CollectImpacts();
};
