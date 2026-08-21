#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "game/anim.h"
#include "math3d.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/materials.h"
#include "sim/microbody.h"
#include "sim/voxload.h"
#include "sim/world.h"

// Articulated mobs (PLAN_voxel_art_and_mobs.md §B): a mob is a set of Jolt
// bodies (one per limb, partitioned by the .vox scene graph) joined by
// constraints from a sidecar JSON. Bodies are CPU-float gameplay state,
// deliberately outside the hashed grid domain (debris.h note): every grid
// interaction — blood, severed limbs settling — travels through the
// MutationQueue op stream, so determinism rule #1 is untouched.
//
// Limbs are kinematic while the mob is alive (keyframe-ish walk drive) and
// flip to dynamic ragdoll on death. Severed limbs are handed to DebrisSystem
// (AdoptBody) and become ordinary debris — culling, terrain upkeep and
// settle-back apply with no mob-specific code.

struct MobLimbDef {
  std::string name;            // matches a .vox scene-graph model name
  std::string parent;          // empty for the root
  Physics::JointType joint = Physics::JointType::Ball;
  float hp = 20;
  bool severable = true;
  bool vital = false;          // severing/destroying this kills the mob
  Vec3 axis{1, 0, 0};          // hinge axis
  float minAngle = -1.2f, maxAngle = 1.2f;
  // anchor override in prefab-local voxels; auto-derived from the AABB gap
  // between limb and parent when absent (anchorAuto)
  Vec3 anchor{};
  bool anchorAuto = true;
  // walk-cycle swing (radians) about X through the joint anchor; phase in
  // half-turns so arm.L/leg.R can counter-swing arm.R/leg.L. Kept as the
  // no-IK fallback for rigs without `chains` (dummy.json) — it now runs as a
  // procedural layer inside the same pose pipeline rather than a side path.
  float swingAmp = 0;
  float swingPhase = 0;
  // ---- extended sidecar (PLAN_voxel_editor.md); all optional ----
  std::string tag;             // "leg"/"arm"/... — gait and chains query by tag
  float severImpactSpeed = 0;  // 0 = absent: a fast hit severs regardless of hp
  bool hasSpring = false;      // non-null "spring" ⇒ jiggled, never keyed
  SpringDef spring;
  // ---- cutting edge (a weapon part; game/melee.h) ----
  // The segment along which this part cuts, in the part's OWN local frame.
  // Authored in the sidecar's `edge` block by whatever generated the art, so
  // the hitbox comes from the same constants as the mesh rather than being
  // re-measured by eye in C++ (see scripts/gen_mina.py sword_vox).
  //
  // Stored in WORLD voxels like every other piece of rig geometry — converted
  // from the .vox's micro units at load, at the same point anchors are, so
  // nothing downstream needs scale awareness.
  bool hasEdge = false;
  Vec3 edgeFrom{}, edgeTo{};   // base (ricasso) and tip, local
  float edgeHalfWidth = 0;     // carve radius at the blade, world voxels
  // Index into the shared micro-body model pool (sim/microbody.h), or -1 for
  // the cube path. Only ever set for defs with scale > 1; a limb whose model
  // failed to pack keeps -1 and simply does not render (cube instances are one
  // WORLD voxel each, so they would draw it at scale times its real size).
  int microModel = -1;
};

struct MobDef {
  std::string name;
  Prefab prefab;               // one model per limb
  int rootLimb = -1;           // index into limbs
  uint32_t bleedMat = 0;       // 0 = mob does not bleed
  float bleedPerDamage = 1.5f; // wound budget voxels per point of damage
  float speed = 4.0f;          // voxels/sec walk speed
  // Micro-voxel authoring scale (docs/PLAN_voxel_editor.md §C): 1 = the legacy
  // path (limb .vox coords ARE world voxels, drawn as instanced cubes), 2|4 =
  // limb .vox coords are MICRO units, `scale` of them per world voxel. Physics
  // builds those limbs at voxel pitch 1/scale and the renderer marches their
  // brick instead of emitting a cube per voxel (sim/microbody.h).
  //
  // NOTE FOR CALLERS: at scale>1 the limb voxel coordinates and sizes stored in
  // MobSystem::Limb stay in MICRO units, but everything the rig and the
  // simulation touch — anchors, rest transforms, restOffset, world positions —
  // is divided into WORLD voxels at load. One conversion point, so the
  // animation runtime and the gait code need no scale awareness at all.
  uint32_t scale = 1;
  // Prefab bounding box in WORLD voxels (= prefab.size / scale). Every piece of
  // gameplay geometry — gait pivots, terrain anchors, ground probes — reads
  // this rather than `prefab.size`, which stays in the .vox's own (micro) units.
  Vec3 worldSize{};
  // Sound sets for this creature, keyed by SLOT ("hurt", "death", "sever",
  // ...), from the sidecar's "sounds" object. Values name a set relative to
  // the slot's namespace exactly as materials do — "hurt": "goblin/hurt"
  // resolves to "mobs/goblin/hurt". assets/sound_schema.js lists the slots the
  // tuner offers.
  //
  // Unlike materials there is NO fallback: an unbound slot is silent, because
  // one creature borrowing another's voice is always wrong. Presentation only;
  // an unknown set name is a diagnostic, never a load failure.
  std::map<std::string, std::string> sounds;

  const std::string& Sound(const char* slot) const {
    static const std::string kNone;
    auto it = sounds.find(slot);
    return it == sounds.end() ? kNone : it->second;
  }

  std::vector<MobLimbDef> limbs;
  // Rig for the animation runtime. `skel.parts` is index-parallel to `limbs`
  // and stored parent-before-child (AnimFlatten's one-pass requirement); the
  // loader topologically sorts `limbs` to guarantee it.
  AnimSkeleton skel;
};

// Loads assets/mobs/*.vox + matching .json sidecars. Appends problems to log;
// defs that fail validation are skipped. Limb models of defs with "scale" > 1
// are packed into `micro` (which the caller uploads); `micro` is CLEARED first,
// so a hot reload rebuilds the whole pool rather than growing it forever.
bool LoadMobDefs(const std::string& dir, const std::vector<MaterialDef>& mats,
                 std::vector<MobDef>& out, MicroBodySet& micro, std::string& log);

class MobSystem {
 public:
  void Init(Physics* phys, World* world, DebrisSystem* debris,
            const std::vector<MaterialDef>& mats);
  // Carving a micro limb clones its brick copy-on-write out of the SAME pool the
  // renderer uploads, so the owner hands it over once at startup (as it already
  // does for DebrisSystem). Not owned. Without it, micro limbs still take real
  // damage — they just cannot show it.
  void SetMicroSet(MicroBodySet* set) { microSet_ = set; }
  void OnMaterialsReloaded(const std::vector<MaterialDef>& mats);
  void SetDefs(std::vector<MobDef> defs);           // hot reload
  const std::vector<MobDef>& Defs() const { return defs_; }
  void Reset();                                      // world regen

  // Spawn def at a world cell (mob min corner; caller picks ground). 0 = fail.
  uint64_t Spawn(int defIndex, IVec3 atVoxel);

  // Once per tick BEFORE debris.PreTick: kinematic walk drive, terrain
  // anchors, bleeding (bounded BrushOps into `ops`), despawn out-of-window.
  //
  // `spawns` receives the VISUAL half of bleeding: micro blood droplets that
  // fly and stain but never re-enter the grid. The grid half (real blood
  // voxels) still goes through `ops`, so the authoritative liquid is unchanged
  // and the spray is pure addition. Dismemberment bursts queued by Sever()
  // drain here too — Sever is called from damage handling all over the frame,
  // and emitting hundreds of particles from inside it would both bypass the
  // per-tick budget and put spawn order at the mercy of hit order.
  void PreTick(uint32_t tick, World& world, std::vector<BrushOp>& ops,
               std::vector<ParticleSpawn>& spawns);
  // After Physics::Step: refresh limb transforms from Jolt.
  void PostStep();

  // Damage a limb by physics body handle (laser, explosions). Returns true
  // if the handle belonged to a live mob limb. Severs / kills at 0 hp, and a
  // hit whose impact speed (voxels/sec) exceeds the limb's severImpactSpeed
  // severs regardless of remaining hp. A non-fatal hit triggers the "attack"
  // flinch clip when the rig defines one.
  bool Damage(uint64_t bodyHandle, float amount, Vec3 hitWorldVoxel,
              float impactSpeed = 0.0f);

  // ---- per-voxel carving (docs/DESIGN.md §7 "Carving living bodies") ---------
  //
  // Removes actual voxels from a LIVE limb, the same way DamageBody carves a
  // rigidbody. This is the substrate for precise wounds: a laser bores a real
  // channel through a torso, a blast scoops a crater out of a shoulder, and in
  // time a scalpel takes out one micro voxel of brain. Missing voxels are
  // cosmetic — the limb keeps its identity, hp, joints and animation — until
  // the carve actually disconnects it, which is when dismemberment becomes a
  // geometric consequence rather than an hp threshold.
  //
  // `eject` spawns the removed matter as ballistic particles (blast) rather
  // than vaporizing it (laser). Returns true when the handle was a live limb.
  bool CarveLimbRadial(uint64_t bodyHandle, Vec3 centerWorldVoxel,
                       float radiusVoxels, bool ragged, bool eject, World& world,
                       std::vector<ParticleSpawn>& spawns);
  // Every live limb of every mob within the blast — the explosion entry point.
  void CarveMobsRadial(Vec3 centerWorldVoxel, float radiusVoxels, World& world,
                       std::vector<ParticleSpawn>& spawns);
  // Detach a limb now (laser crossing the joint). Root/vital kills instead.
  void Sever(uint64_t mobId, int limbIndex);
  // Nearest live limb of any mob to a body handle; -1 if none.
  bool FindLimb(uint64_t bodyHandle, uint64_t& mobId, int& limbIndex) const;

  // Re-draw every live mob's gore profile against the current tuning. Called
  // on tuning reload (F5) so a variance edit is visible on the mobs already
  // standing there, instead of only on the next ones spawned.
  void RefreshGoreProfiles();

  // Render plumbing: limbs append after the debris bodies' slots.
  bool InstancesDirty() const { return instancesDirty_; }
  void AppendInstances(std::vector<BodyVoxInst>& out, uint32_t slotBase);
  void AppendXforms(std::vector<BodyXformGpu>& out) const;
  // Append this system's micro limbs to the COMPACTED draw list, using the same
  // slot walk as AppendXforms/AppendInstances so the recorded slot is the one
  // the limb's transform lands in — sim/microbody.h.
  void AppendMicroInsts(std::vector<MicroBodyInstGpu>& out,
                        uint32_t slotBase) const;
  uint32_t LimbBodyCount() const;
  uint32_t MobCount() const { return (uint32_t)mobs_.size(); }

  // introspection (selftest / overlay)
  uint64_t LimbBody(uint64_t mobId, int limbIndex) const;
  bool IsAlive(uint64_t mobId) const;
  Vec3 MobOrigin(uint64_t mobId) const;
  // The mob's facing direction — the SAME `fwd` the kinematic walk translates
  // along and the same yaw the limb submit applies, so a test written against
  // this cannot drift from the convention. A mob must move along +facing; if a
  // model is authored nose-backwards it will walk in reverse (see the critter
  // generator's axis note in scripts/gen_critter_mob.py).
  Vec3 MobFacing(uint64_t mobId) const;
  // Gait introspection: how many chains currently have a swinging foot, and
  // how far the planted feet have travelled. The selftest asserts the
  // one-group-swinging invariant per tick rather than comparing step rates
  // (see the frontier-rule testing note: rate comparisons prove nothing).
  int SwingingFeet(uint64_t mobId) const;
  int PlantedFeet(uint64_t mobId) const;
  int ActiveClips(uint64_t mobId) const;
  // Active dismemberment locomotion state: index into the def's authored
  // `states` list (AnimSkeleton::states), -1 for normal locomotion.
  int LocoState(uint64_t mobId) const;
  // Pose introspection for --shot-mob: a limb's local +Y (post-blend, stage 3)
  // and model +Y (post-flatten/IK, stage 4-5). Comparing these against the
  // limb's WORLD transform is what localizes a pose bug to a stage instead of
  // guessing from a screenshot.
  Vec3 LimbLocalUp(uint64_t mobId, int limbIndex) const;
  Vec3 LimbModelUp(uint64_t mobId, int limbIndex) const;
  // Live clip instances as "name:weight" pairs — the one view that tells a
  // stuck crossfade (two clips still blending) apart from a mis-authored key.
  std::vector<std::pair<std::string, float>> ClipWeights(uint64_t mobId) const;
  // Live voxel count of one limb, and what it was authored with. The carve
  // selftest asserts against these rather than against rendered instance
  // counts: a micro limb emits no cube instances at all, so counting draws
  // would silently measure nothing on exactly the rigs carving matters most on.
  uint32_t LimbVoxelCount(uint64_t mobId, int limbIndex) const;
  uint32_t LimbVoxelsAtSpawn(uint64_t mobId, int limbIndex) const;
  // World position of one of a limb's SURVIVING voxels — the `n`th, wrapped.
  // Deliberately not the centroid: once a carve has hollowed a limb, its
  // centroid is in the cavity, and a tool aimed there eats nothing. Anything
  // that wants to keep cutting (the selftest, a future aim assist) has to aim
  // at flesh that is actually still present.
  Vec3 LimbVoxelPos(uint64_t mobId, int limbIndex, uint32_t n) const;

 private:
  struct Limb {
    uint64_t body = 0;         // 0 = severed or never spawned
    uint64_t joint = 0;        // to parent
    float hp = 0;
    // Voxels + size stay in the DEF'S AUTHORING UNITS: micro voxels at
    // scale>1 (that is what both the collider's 1/scale pitch and the
    // renderer's brick march expect). Everything positional below is in
    // WORLD voxels.
    std::vector<DebrisVoxel> voxels;
    IVec3 size{};
    int microModel = -1;       // copy of MobLimbDef::microModel, -1 = cube path
    // What this limb's body needs to keep rendering as microvoxels once it is
    // no longer a limb — handed to DebrisSystem::AdoptBody on sever/death.
    MicroBodyRef MicroRef(uint32_t defScale) const {
      return microModel < 0 ? MicroBodyRef{}
                            : MicroBodyRef{(uint32_t)microModel, defScale};
    }
    Vec3 restOffset{};         // limb min corner from mob min corner (rest)
    Vec3 anchorRoot{};         // joint anchor from mob min corner (rest)
    Vec3 anchorLimb{};         // joint anchor in limb-local coords
    BodyTransform xf{};
    float bleedBudget = 0;
    Vec3 woundLocal{};
    // Dismemberment gout, drained over several ticks by PreTick. `gushTicks`
    // counts DOWN from gore.severDecayTicks, and emission is proportional to
    // it, so the burst is front-loaded and tails off by itself — that decay is
    // what makes a cut read as arterial rather than as a running tap.
    //
    // It lives on the PARENT limb (the stump), not on the piece that came off:
    // the severed limb is debris the moment it detaches, and a detached limb
    // that kept bleeding would trail spray from a body MobSystem no longer
    // owns or tracks.
    int gushTicks = 0;
    Vec3 gushLocal{};          // wound point, parent-limb local
    Vec3 gushDir{0, 1, 0};     // outward spray axis, parent-limb local
    // A severed part is handed to DebrisSystem immediately (counts, rendering
    // and terrain upkeep all move over on the same frame) but holds its last
    // animated pose KINEMATICALLY for a beat before flipping dynamic — cutting
    // straight to ragdoll on the hit frame reads as a teleport. `body` is
    // cleared at once so the mob no longer owns it; `holdBody` is the handle
    // the countdown still has to flip.
    uint64_t holdBody = 0;
    float holdSeconds = 0;
    // Flipbook: >=0 selects an alternate .vox model's voxels for RENDERING
    // only (the Jolt shape stays the rest model — a frame swap must not
    // rebuild collision every 100 ms). Instances rebuild on frame change.
    int flipbookModel = -1;
    std::vector<std::vector<DebrisVoxel>> frameVoxels;  // per .vox model index
    // Per-voxel carving state. `carved` latches the first time this limb loses
    // a voxel; from then on its micro model is a copy-on-write clone this limb
    // OWNS and must free (ReleaseLimbMicro), and its flipbooks are disabled —
    // a frame swap re-points rendering at an intact authored model, which would
    // silently heal the wounds the player just carved.
    bool carved = false;
    // Voxel count the limb was authored with, so damage can be expressed as a
    // FRACTION of the limb rather than an absolute count. A carve that removes
    // half a scale-4 arm and half a scale-1 arm should read as the same injury.
    uint32_t voxelsAtSpawn = 0;
  };
  // Per-mob gore profile: the entity-scoped variance draws, resolved ONCE when
  // the mob is created and then held for its whole life. Every mob that is made
  // gets its own, so one NPC can be a heavy bleeder from spawn to corpse while
  // its neighbour bleeds normally.
  //
  // Resolved at spawn rather than re-drawn at each use for two reasons: the
  // draw is only stable if nothing about it varies per call site, and holding
  // it means a mid-session tuning reload does not change the character of mobs
  // already standing in the world (RefreshGoreProfiles re-rolls them on demand).
  // Values are absolute (already multiplied by the whole-wound gain), so the
  // spray sites just read them.
  struct GoreProfile {
    float bleedSprayPerDrip = 0;
    float bleedSpraySpeed = 0, bleedSprayCone = 0;
    int severSpray = 0, severDecayTicks = 1;
    float severSpraySpeed = 0, severSprayCone = 0;
    int severVoxels = 0;
    float severVoxelSpeed = 0;
    int microLifeTicks = 1;
    float bleedGain = 1.0f;   // kept for display/debug; already folded in above
  };
  // Draws the entity-scoped variance for one mob id against the current tuning.
  static GoreProfile MakeGoreProfile(uint64_t mobId);

  struct Mob {
    uint64_t id = 0;
    int defIndex = 0;
    bool alive = true;
    GoreProfile gore;          // this mob's own bleed character
    float heading = 0;         // radians about +Y
    float phase = 0;           // walk cycle
    Vec3 origin{};             // mob prefab min corner, world voxels
    uint32_t lastTurnTick = 0;
    std::vector<Limb> limbs;
    AnimState anim;            // float presentation state (never hashed)
    float speedNow = 0;        // measured planar speed, voxels/sec
    Vec3 bodyUp{0, 1, 0};      // foot-plane normal (slope tilt)
    float bodyY = 0;           // body height derived from the foot average
    bool footInit = false;
  };

  void Die(Mob& mob);          // ragdoll: limbs go dynamic, adopt into debris
  void DetachLimb(Mob& mob, int limbIndex, bool adopt);

  // ---- carving internals -----------------------------------------------------
  // Shared carve core, the live-limb twin of DebrisSystem::DamageBody: erase the
  // voxels `keep` rejects, re-skin, rebuild the collider, and hand any piece the
  // carve disconnected to DebrisSystem as ordinary debris. Returns false when
  // the limb was destroyed outright (severed or the mob died), in which case
  // `mob` may no longer be alive and the caller must not touch the limb again.
  bool CarveLimb(Mob& mob, int limbIndex, World& world,
                 std::vector<ParticleSpawn>& spawns, bool eject,
                 const std::function<bool(const DebrisVoxel&)>& keep);
  // Re-skin a carved limb: clone-on-first-carve, rewrite the brick from the
  // surviving voxels, shift the transform so the art stays on the collider, and
  // shift restOffset/anchorLimb with it so the RIG follows too — the difference
  // from the debris version, whose bodies answer to nobody. False = pool full
  // (limb keeps a stale skin but is really carved).
  bool ReskinLimbMicro(Mob& mob, Limb& limb, uint32_t defScale);
  // Rebuild a carved limb's Jolt body and re-create its joint to its parent.
  // A collider rebuild REPLACES the handle, so the joint must be rebuilt or the
  // limb falls off; children's joints anchor to this body and are rebuilt too.
  bool RebuildLimbBody(Mob& mob, int limbIndex);
  // Return a carved limb's owned micro brick to the pool. Must be called on
  // every path that stops the mob owning the limb (sever, death, despawn,
  // reset) or the pool leaks words nothing reclaims.
  void ReleaseLimbMicro(Limb& limb);
  // Hand one disconnected chunk of a limb to DebrisSystem as a free body, with
  // its own COW brick. Falls back to particles when a body or brick can't be
  // made, exactly as ShatterBody does.
  void EmitCarvedFragment(Mob& mob, const Limb& src, uint32_t defScale,
                          std::vector<DebrisVoxel> part, World& world,
                          std::vector<ParticleSpawn>& spawns);
  void LimbVoxelsToParticles(const Limb& limb, uint32_t defScale,
                             const std::vector<DebrisVoxel>& voxels, World& world,
                             std::vector<ParticleSpawn>& spawns) const;
  bool GroundHeightAt(World& world, int wx, int wz, int yFrom, int& outY) const;
  // Animation pipeline stages 1-5 plus the procedural gait layer; leaves the
  // model-space pose in mob.anim.model. Pure float, no grid contact.
  void UpdateAnimation(Mob& mob, const MobDef& def, World& world, float dt);
  void UpdateGait(Mob& mob, const MobDef& def, World& world, float dt);
  void PlayClip(Mob& mob, const MobDef& def, const std::string& name);

  Physics* phys_ = nullptr;
  World* world_ = nullptr;
  DebrisSystem* debris_ = nullptr;
  MicroBodySet* microSet_ = nullptr;  // shared brick pool; see SetMicroSet
  std::vector<float> densityOf_;
  std::vector<uint32_t> classOf_;
  std::vector<MobDef> defs_;
  std::vector<Mob> mobs_;
  uint64_t nextId_ = 1;
  bool instancesDirty_ = false;
  // Particles authored outside PreTick — Sever() is reached from damage
  // handling at several points in the frame, and appending straight to the
  // caller's spawn list from there would mean Sever needs it threaded through
  // every one of those paths. Drained (and cleared) at the top of PreTick.
  std::vector<ParticleSpawn> pendingSpawns_;

  static constexpr uint32_t kMaxMobs = 16;
  // (the drip op budget is now gore.bleedOpsPerTick in tuning.json)
  // how long a severed piece holds its last animated pose before ragdolling
  static constexpr float kSeverHoldSeconds = 0.25f;
  // ---- carving ---------------------------------------------------------------
  // A carved chunk needs this many voxels to become its own rigidbody; smaller
  // pieces spray as particles. Deliberately lower than the debris island floor
  // (8): flesh comes off in gobbets, and a severed finger IS the interesting
  // object even though a 4-voxel rock chip is not.
  static constexpr uint32_t kMinFragmentVoxels = 4;
  // Fragments one carve may spawn, so a point-blank blast on a crowd cannot
  // flood the body table (rule 2: bound every emergent process).
  static constexpr uint32_t kMaxCarveFragments = 3;
  // Below this fraction of its authored volume a limb is no longer a limb: it
  // severs. This is what makes "shoot the arm until it falls off" work through
  // pure geometry, without an hp bar deciding it.
  static constexpr float kLimbCollapseFraction = 0.25f;
  // Carve damage per voxel removed, as a fraction of the limb's authored
  // volume: losing all of a limb's matter costs this multiple of its full hp.
  // > 1 so a limb that is being visibly minced dies a little before it is
  // wholly gone, which reads better than a limb hanging on at one voxel.
  static constexpr float kCarveDamagePerVolume = 1.5f;
};
