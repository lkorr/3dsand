#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/materials.h"
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
  // half-turns so arm.L/leg.R can counter-swing arm.R/leg.L
  float swingAmp = 0;
  float swingPhase = 0;
};

struct MobDef {
  std::string name;
  Prefab prefab;               // one model per limb
  int rootLimb = -1;           // index into limbs
  uint32_t bleedMat = 0;       // 0 = mob does not bleed
  float bleedPerDamage = 1.5f; // wound budget voxels per point of damage
  float speed = 4.0f;          // voxels/sec walk speed
  std::vector<MobLimbDef> limbs;
};

// Loads assets/mobs/*.vox + matching .json sidecars. Appends problems to log;
// defs that fail validation are skipped.
bool LoadMobDefs(const std::string& dir, const std::vector<MaterialDef>& mats,
                 std::vector<MobDef>& out, std::string& log);

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
  // if the handle belonged to a live mob limb. Severs / kills at 0 hp.
  bool Damage(uint64_t bodyHandle, float amount, Vec3 hitWorldVoxel);
  // Detach a limb now (laser crossing the joint). Root/vital kills instead.
  void Sever(uint64_t mobId, int limbIndex);
  // Nearest live limb of any mob to a body handle; -1 if none.
  bool FindLimb(uint64_t bodyHandle, uint64_t& mobId, int& limbIndex) const;

  // Render plumbing: limbs append after the debris bodies' slots.
  bool InstancesDirty() const { return instancesDirty_; }
  void AppendInstances(std::vector<BodyVoxInst>& out, uint32_t slotBase);
  void AppendXforms(std::vector<BodyXformGpu>& out) const;
  uint32_t LimbBodyCount() const;
  uint32_t MobCount() const { return (uint32_t)mobs_.size(); }

  // introspection (selftest / overlay)
  uint64_t LimbBody(uint64_t mobId, int limbIndex) const;
  bool IsAlive(uint64_t mobId) const;
  Vec3 MobOrigin(uint64_t mobId) const;

 private:
  struct Limb {
    uint64_t body = 0;         // 0 = severed or never spawned
    uint64_t joint = 0;        // to parent
    float hp = 0;
    std::vector<DebrisVoxel> voxels;
    IVec3 size{};
    Vec3 restOffset{};         // limb min corner from mob min corner (rest)
    Vec3 anchorRoot{};         // joint anchor from mob min corner (rest)
    Vec3 anchorLimb{};         // joint anchor in limb-local coords
    BodyTransform xf{};
    float bleedBudget = 0;
    Vec3 woundLocal{};
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
  };

  void Die(Mob& mob);          // ragdoll: limbs go dynamic, adopt into debris
  void DetachLimb(Mob& mob, int limbIndex, bool adopt);
  bool GroundHeightAt(World& world, int wx, int wz, int yFrom, int& outY) const;

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
};
