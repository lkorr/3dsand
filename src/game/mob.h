#pragma once
#include <cstdint>
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
  void OnMaterialsReloaded(const std::vector<MaterialDef>& mats);
  void SetDefs(std::vector<MobDef> defs);           // hot reload
  const std::vector<MobDef>& Defs() const { return defs_; }
  void Reset();                                      // world regen

  // Spawn def at a world cell (mob min corner; caller picks ground). 0 = fail.
  uint64_t Spawn(int defIndex, IVec3 atVoxel);

  // Once per tick BEFORE debris.PreTick: kinematic walk drive, terrain
  // anchors, bleeding (bounded BrushOps into `ops`), despawn out-of-window.
  void PreTick(uint32_t tick, World& world, std::vector<BrushOp>& ops);
  // After Physics::Step: refresh limb transforms from Jolt.
  void PostStep();

  // Damage a limb by physics body handle (laser, explosions). Returns true
  // if the handle belonged to a live mob limb. Severs / kills at 0 hp, and a
  // hit whose impact speed (voxels/sec) exceeds the limb's severImpactSpeed
  // severs regardless of remaining hp. A non-fatal hit triggers the "attack"
  // flinch clip when the rig defines one.
  bool Damage(uint64_t bodyHandle, float amount, Vec3 hitWorldVoxel,
              float impactSpeed = 0.0f);
  // Detach a limb now (laser crossing the joint). Root/vital kills instead.
  void Sever(uint64_t mobId, int limbIndex);
  // Nearest live limb of any mob to a body handle; -1 if none.
  bool FindLimb(uint64_t bodyHandle, uint64_t& mobId, int& limbIndex) const;

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
  };
  struct Mob {
    uint64_t id = 0;
    int defIndex = 0;
    bool alive = true;
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
  bool GroundHeightAt(World& world, int wx, int wz, int yFrom, int& outY) const;
  // Animation pipeline stages 1-5 plus the procedural gait layer; leaves the
  // model-space pose in mob.anim.model. Pure float, no grid contact.
  void UpdateAnimation(Mob& mob, const MobDef& def, World& world, float dt);
  void UpdateGait(Mob& mob, const MobDef& def, World& world, float dt);
  void PlayClip(Mob& mob, const MobDef& def, const std::string& name);

  Physics* phys_ = nullptr;
  World* world_ = nullptr;
  DebrisSystem* debris_ = nullptr;
  std::vector<float> densityOf_;
  std::vector<uint32_t> classOf_;
  std::vector<MobDef> defs_;
  std::vector<Mob> mobs_;
  uint64_t nextId_ = 1;
  bool instancesDirty_ = false;

  static constexpr uint32_t kMaxMobs = 16;
  static constexpr int kBleedOpsPerTick = 6;  // of the 64-op tick budget
  // how long a severed piece holds its last animated pose before ragdolling
  static constexpr float kSeverHoldSeconds = 0.25f;
};
