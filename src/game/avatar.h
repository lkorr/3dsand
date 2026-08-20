#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/anim.h"
#include "game/mob.h"
#include "game/player.h"
#include "math3d.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/microbody.h"
#include "sim/world.h"

// The PLAYER AVATAR: a visible, dismemberable body for the player.
//
// WHY THIS IS NOT A MOB. The avatar reuses the mob DATA format (assets/mobs/
// wizard.{vox,json}) and the whole animation runtime in anim.h — clips, IK,
// gait, springs and, most importantly, the AnimStateRule dismemberment
// locomotion table. What it does not reuse is MobSystem's DRIVER: a mob picks
// its own heading and wanders, whereas the avatar's position and facing come
// from Player (which is itself driven by input and by the voxel sweeps in
// player.cpp). Trying to express that as "a mob the player possesses" would
// mean threading player input through the wander drive, the despawn sweep and
// kMaxMobs; keeping the driver separate costs one file and leaves MobSystem
// completely untouched.
//
// So: one schema, two drivers. Everything an animator authors for a mob works
// on the avatar and vice versa.
//
// DETERMINISM (CLAUDE.md rule 1). Every field here is CPU-float PRESENTATION
// state, exactly like MobSystem's: poses, springs, camera offsets and the
// ragdoll are never hashed and never touch the grid. The only grid contact is
// bleeding, which travels through the same BrushOp/ParticleSpawn queues mobs
// use, so it lands via the MutationQueue like every other world edit (rule 3).
//
// COST WHEN IDLE (rule 2). One avatar exists, its limbs are kinematic, and a
// standing player runs the same pose pipeline a standing mob does. Severed
// parts are handed to DebrisSystem, which already culls and sleeps them.

// Which parts the camera and the movement coupling care about, resolved once
// at load so the per-frame code never does string lookups.
struct AvatarParts {
  int head = -1, torso = -1, hips = -1;
  int handL = -1, handR = -1;
  int armUL = -1, armUR = -1;
  int footL = -1, footR = -1;
  int legUL = -1, legUR = -1;
  int staff = -1;
};

// What the avatar wants the rest of the game to do about its current state.
// Movement and camera read this instead of querying part liveness themselves,
// so "what does losing a leg do" is answered in exactly one place.
struct AvatarLocomotion {
  // Multiplier on walk/sprint speed, from the active AnimStateRule's
  // speedScale (1.0 when intact). Player applies it to its target speed.
  float speedScale = 1.0f;
  // Multiplier on jump impulse. Derived from leg liveness rather than authored
  // per state: you cannot jump on no legs, and one leg jumps weakly.
  float jumpScale = 1.0f;
  // Eye height multiplier — a crawling wizard's eyes are near the floor. Taken
  // from the state's bodyYOffset so the camera and the pose agree by
  // construction rather than by two tables being kept in sync.
  float eyeHeightScale = 1.0f;
  // Index into the def's `states` list, -1 = intact/normal. Purely for UI and
  // the selftest; nothing branches on the NAME.
  int stateIndex = -1;
  const char* stateName = "normal";
  bool canJump = true;
  bool alive = true;
};

class PlayerAvatar {
 public:
  // `defs` is MobSystem's loaded def list; the avatar picks `defName` out of it
  // rather than loading its own copy, so a hot reload (R) rebuilds both at
  // once and the micro-body pool stays a single shared allocation.
  //
  // LIFETIME: `defs` must outlive the avatar, and MUST be re-published through
  // SetDefs after any MobSystem::SetDefs — that call replaces the vector's
  // contents, so every MobDef* into it (including def_) dangles afterwards.
  // Re-publishing despawns first, which is also what stops a reload from
  // leaving limb bodies pointing at a freed def.
  void Init(Physics* phys, World* world, DebrisSystem* debris,
            const std::vector<MaterialDef>& mats);
  // Rebuild the per-material density table after a materials reload (R).
  void OnMaterialsReloaded(const std::vector<MaterialDef>& mats);
  // Points the avatar at a def by name. Safe to call repeatedly (hot reload):
  // despawns first, so limb bodies never leak across a reload.
  void SetDefs(const std::vector<MobDef>* defs, const std::string& defName);
  bool HasDef() const { return def_ != nullptr; }
  const MobDef* Def() const { return def_; }

  // Creates the limb bodies at the player's current position. No-op if already
  // spawned. Returns false if the def is missing or physics refused a body.
  bool Spawn(const Player& player, float headingRad);
  void Despawn();                 // world regen / teleport / mode change
  bool Spawned() const { return spawned_; }

  // Once per tick, BEFORE Physics::Step and debris.PreTick — mirrors the
  // ordering MobSystem::PreTick relies on. Drives the rig from the player's
  // state, submits kinematic limb targets, and appends bleeding to `ops` /
  // `spawns` exactly as mobs do.
  //
  // `heading` is the direction the BODY faces, which is not the camera yaw:
  // in third person the body turns toward its motion and only snaps to the
  // camera when the player aims. main.cpp owns that policy and passes the
  // result in.
  void PreTick(uint32_t tick, const Player& player, float heading, float dt,
               World& world, std::vector<BrushOp>& ops,
               std::vector<ParticleSpawn>& spawns);
  // After Physics::Step: refresh limb transforms from Jolt.
  void PostStep();

  // ---- damage / dismemberment ----
  // Same contract as MobSystem::Damage: returns true if the handle was one of
  // the avatar's limbs. A hit crossing a joint anchor, or one past the limb's
  // severImpactSpeed, takes the part off.
  bool Damage(uint64_t bodyHandle, float amount, Vec3 hitWorldVoxel,
              float impactSpeed = 0.0f);
  // Detach a part now. A vital part (head/torso) kills the avatar instead.
  void Sever(int partIndex);
  // Debug/testing: sever by authored part name. Returns false on an unknown
  // name or an already-severed part.
  bool SeverByName(const std::string& name);
  void Revive(const Player& player, float heading);  // heal + respawn all parts

  bool IsAlive() const { return alive_; }
  bool PartAlive(int i) const {
    return i >= 0 && i < (int)anim_.partAlive.size() && anim_.partAlive[i];
  }
  const AvatarParts& Parts() const { return parts_; }
  // What movement and the camera should do about the current damage state.
  AvatarLocomotion Locomotion() const;

  // Model-space pose of a part (the same frame anim_.model is in), or identity
  // when the part is gone. The camera uses this to ride the head.
  bool PartWorldTransform(int part, Vec3& outPos, Quat& outRot) const;
  // World position of a part's joint anchor — where the camera boom pivots and
  // where a first-person eye sits.
  bool PartAnchorWorld(int part, Vec3& out) const;

  // ---- render plumbing (identical slot walk to MobSystem's) ----
  bool InstancesDirty() const { return instancesDirty_; }
  void AppendInstances(std::vector<BodyVoxInst>& out, uint32_t slotBase);
  void AppendXforms(std::vector<BodyXformGpu>& out) const;
  void AppendMicroInsts(std::vector<MicroBodyInstGpu>& out,
                        uint32_t slotBase) const;
  uint32_t LimbBodyCount() const;
  // Parts to SKIP when drawing — first person hides the body but keeps the
  // arms, so the wizard can see their own hands and staff. Empty in third
  // person. Set by main.cpp from the camera mode.
  void SetHiddenParts(const std::vector<uint8_t>& hidden);

  // introspection (overlay / selftest)
  int PartIndex(const std::string& name) const;
  int LivePartCount() const;
  int ActiveClips() const { return (int)anim_.clips.size(); }
  int LocoState() const { return anim_.locoState; }
  Vec3 Origin() const { return origin_; }
  float BodyY() const { return bodyY_; }

 private:
  struct Part {
    uint64_t body = 0;
    uint64_t joint = 0;
    float hp = 0;
    std::vector<DebrisVoxel> voxels;   // authoring (micro) units
    IVec3 size{};
    int microModel = -1;
    MicroBodyRef MicroRef(uint32_t s) const {
      return microModel < 0 ? MicroBodyRef{}
                            : MicroBodyRef{(uint32_t)microModel, s};
    }
    Vec3 restOffset{};
    Vec3 anchorRoot{};
    Vec3 anchorLimb{};
    BodyTransform xf{};
    float bleedBudget = 0;
    Vec3 woundLocal{};
    int gushTicks = 0;
    Vec3 gushLocal{};
    Vec3 gushDir{0, 1, 0};
    uint64_t holdBody = 0;
    float holdSeconds = 0;
  };

  void DetachPart(int index, bool adopt);
  void Die();
  void PlayClip(const std::string& name);
  void UpdateAnimation(float dt, World& world);
  void UpdateGait(float dt, World& world);
  bool GroundHeightAt(World& world, int wx, int wz, int yFrom, int& outY) const;
  void ResolveParts();

  Physics* phys_ = nullptr;
  World* world_ = nullptr;
  DebrisSystem* debris_ = nullptr;
  std::vector<float> densityOf_;   // material id -> density, for collider mass
  std::vector<uint32_t> classOf_;  // material id -> class, for the ground probe
  const std::vector<MobDef>* defs_ = nullptr;
  const MobDef* def_ = nullptr;
  std::string defName_ = "wizard";

  std::vector<Part> parts;
  AvatarParts parts_;
  AnimState anim_;
  bool spawned_ = false;
  bool alive_ = true;
  bool instancesDirty_ = false;
  std::vector<uint8_t> hidden_;      // per-part render suppression
  std::vector<ParticleSpawn> pendingSpawns_;

  Vec3 origin_{};                    // prefab min corner, world voxels
  float heading_ = 0;
  float bodyY_ = 0;
  Vec3 bodyUp_{0, 1, 0};
  float speedNow_ = 0;
  bool footInit_ = false;
  bool wasGrounded_ = true;
  float airTime_ = 0;
  uint64_t id_ = 0x5A11EDU;          // stable seed for gore variance

  static constexpr float kSeverHoldSeconds = 0.25f;
  static constexpr int kBleedOpsPerTick = 6;
};
