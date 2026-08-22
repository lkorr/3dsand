#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/anim.h"
#include "game/item.h"
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

  // ---- head look ------------------------------------------------------------
  // Where the character is LOOKING, as an offset from where the body is
  // FACING. Pushed in once per tick by main.cpp before PreTick, same division
  // of labour as heading and the weapon pose: main.cpp owns the policy (it is
  // the thing that knows the camera), the avatar owns the rig.
  //
  // `yawRel` is camera yaw minus body heading, radians, already wrapped to
  // (-pi, pi]; `pitch` is the camera pitch, radians, positive up. Both are
  // CLAMPED here against the neck limits in tuning.json rather than trusted:
  // main.cpp's turn policy uses the same limit to decide when the BODY has to
  // start turning, and a rig that silently over-rotates when those two
  // disagree is a much worse failure than a head that stops at its stop.
  void SetLook(float yawRel, float pitch);

  // ---- footfall events (presentation only) --------------------------------
  // A foot touching down, produced by the gait's own plant moment rather than
  // by a distance accumulator — so a step sounds exactly when the art shows
  // the foot land, at whatever cadence the gait chose, and a leg lost to
  // dismemberment simply stops producing them.
  //
  // These QUEUE rather than fire directly because PreTick runs inside the
  // fixed-tick loop (up to 4 ticks per frame): the consumer drains them once
  // per frame. Presentation only — nothing here may feed back into the sim.
  struct Footfall {
    Vec3 posVox{};      // where the foot landed
    uint32_t mat = 0;   // material id of the supporting voxel (0 = unknown)
    float speed = 0;    // walker speed at touchdown, voxels/sec
    int foot = 0;       // chain index, so left/right can be pitched apart
    bool landing = false;  // true when this is a touchdown from a fall
    float fallSpeed = 0;   // downward speed on a landing, voxels/sec
  };
  const std::vector<Footfall>& Footfalls() const { return footfalls_; }
  void ClearFootfalls() { footfalls_.clear(); }

  // ---- held weapon / melee (game/melee.h) ---------------------------------
  // The swing pose, pushed in once per tick by main.cpp before PreTick. The
  // avatar does NOT own the swing state machine and does not read input: it is
  // told where the weapon hand should be and which way the blade points, and
  // it aims the arm chain there. Same division as heading — main.cpp owns the
  // policy, the avatar owns the rig.
  //
  // `handOffset` is relative to the weapon shoulder, in world voxels, in WORLD
  // space (the caller built it from the camera basis). `weight` fades the
  // whole thing against the ordinary animation pose, so sheathing eases out
  // instead of snapping.
  void SetWeaponPose(Vec3 handOffset, Vec3 bladeDir, Vec3 bladeUp,
                     float weight);
  // ---- holding an item ----------------------------------------------------
  //
  // THE ENTITY<->SLOT SYNC SEAM, and deliberately the only one. Equipping
  // BORROWS A RIG SLOT: the item's own geometry fills a real Part parented to
  // the socket's limb, so while worn it is a rig part in every respect —
  // animated, rendered, severable with the arm that holds it, droppable as
  // debris, carvable per voxel. Nothing downstream needs an "is this an item"
  // branch, which is precisely why the item is not welded on as a special case.
  //
  // Placement composes SOCKET x GRIP, forward: the rig says where the fist
  // closes (MobDef::sockets), the item says how it sits in that fist
  // (ItemDef::grip). Pass nullptr to unequip.
  //
  // Returns false and changes nothing if the item cannot be held — no such
  // socket, or the item declares no grip for this context. Both are content
  // bugs that must be loud: the silent alternative parks the blade at the
  // wearer's origin, which reads as a physics glitch rather than missing JSON.
  bool EquipItem(const ItemDef* item, const char* context = "held_right");
  const std::string& HeldItem() const { return heldItem_; }
  // The rig slot the held item occupies, or -1. Exposed so the melee damage
  // path can exclude it from its own sweep.
  int HeldSlot() const { return heldSlot_; }

  // The held weapon's cutting edge in WORLD voxels, from the part's authored
  // `edge` block through its live transform. False when nothing is held, the
  // part is severed, or the def declares no edge. This is what the melee
  // damage sweep carves along — reading it from the same transform the
  // renderer draws means the hitbox cannot drift from the visible blade.
  bool WeaponEdge(Vec3& outBase, Vec3& outTip, float& outHalfWidth) const;

  // Is this Jolt body one of the avatar's own parts? A held weapon starts
  // inside its wielder's hand and sweeps across their front, so the melee
  // sweep would otherwise carve the arm holding it on every guard.
  bool OwnsBody(uint64_t bodyHandle) const;

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

  // ---- health, as the caster VM sees it (game/spell.h) --------------------
  // The player has no single hp field, and deliberately gains none here:
  // health effectively lives on the per-part hp below, so the spell system
  // reads and spends THAT rather than inventing a parallel number that would
  // immediately drift from the visible damage state.
  //
  // Health is the summed hp of every LIVE part, rounded to an integer because
  // the caster VM is integer throughout (thesis 3 in spell.h).
  int32_t TotalHealth() const;
  // The authored total across every limb — the HUD bar's denominator. Health
  // NEVER regenerates, so this is a ceiling the player only moves away from;
  // it is deliberately not lowered by severing, so a lost limb reads as a
  // permanently short bar rather than as a full one on a smaller body.
  int32_t HealthMax() const;
  // Spend health across live parts, proportionally to what each still has.
  // Distributing rather than draining one part keeps a mana overdraw from
  // arbitrarily severing whichever limb happens to be first in the list.
  // Parts driven to zero are severed through the ordinary Sever() path, so an
  // overcast dismembers you with no new gore code.
  void SpendHealth(int32_t amount);
  // Kill the caster spectacularly at `atWorldVoxel`: carve every part within
  // `radiusVox` and then Die(). Used by a FATAL overcast, whose thematic
  // flavour comes entirely from the spell's own effect payload running at the
  // caster (spell.h thesis 2) — this is only the body's half of it, and it
  // reuses CarveLimbRadial + the existing gore pipeline rather than adding any.
  void SelfDestruct(Vec3 atWorldVoxel, float radiusVox, World& world,
                    std::vector<ParticleSpawn>& spawns);

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
  // Total parts on the live rig, INCLUDING a borrowed item slot. Callers
  // building a per-part array (the first-person hide list) must size against
  // this rather than the def's limb count, which does not know about items.
  int PartCount() const { return (int)parts.size(); }
  // Collision-box debug overlay (world.h DebugBox, the dev panel's "collision
  // boxes" toggle). One oriented wireframe per LIVE part body, read from the
  // body's actual Jolt collider via Physics::GetLocalBounds — not from the
  // part's art, so a collider that has drifted from the model it represents
  // shows up as exactly that. Appends; stops at `limit` total.
  void AppendDebugBoxes(std::vector<DebugBox>& out, size_t limit,
                        uint32_t color) const;

  // introspection (overlay / selftest)
  // Jolt body of a part, or 0 when it is severed or never spawned. Mirrors
  // MobSystem::LimbBody so a test can reach the same handle the damage paths
  // are handed.
  uint64_t PartBody(int part) const;
  int PartIndex(const std::string& name) const;
  int LivePartCount() const;
  int ActiveClips() const { return (int)anim_.clips.size(); }
  // Measured planar speed in world voxels/sec (presentation/diagnostics only).
  float SpeedNow() const { return speedNow_; }
  // The AUTHORED model-space pose, before it is handed to Jolt. Comparing this
  // against PartWorldTransform (which reads back what the solver did) is how
  // you tell an animation bug from physics fighting the animation.
  bool PartModelTransform(int part, Vec3& outPos, Quat& outRot) const {
    if (part < 0 || part >= (int)anim_.model.size()) return false;
    outPos = anim_.model[part].pos;
    outRot = anim_.model[part].rot;
    return true;
  }
  // Name of the i'th active clip, for diagnostics and the selftest.
  const char* ActiveClipName(int i) const {
    if (!def_ || i < 0 || i >= (int)anim_.clips.size()) return "?";
    int c = anim_.clips[i].clip;
    if (c < 0 || c >= (int)def_->skel.clips.size()) return "?";
    return def_->skel.clips[c].name.c_str();
  }
  int LocoState() const { return anim_.locoState; }
  Vec3 Origin() const { return origin_; }
  float BodyY() const { return bodyY_; }

 private:
  struct Part {
    uint64_t body = 0;
    uint64_t joint = 0;
    float hp = 0;
    std::vector<DebrisVoxel> voxels;   // COLLIDER units (MobDef::physScale)
    // The SKIN lattice (MobDef::skinScale), int16 — the brick source, and what
    // travels to DebrisSystem on sever so a severed part keeps its detail.
    // Empty when the two lattices coincide (mob.h Limb::skinVoxels).
    std::vector<PrefabVoxel> skinVoxels;
    bool HasFineSkin() const { return !skinVoxels.empty(); }
    IVec3 size{};
    int microModel = -1;
    // Takes the SKIN scale: a MicroBodyRef is a render description.
    MicroBodyRef MicroRef(uint32_t skinScale) const {
      return microModel < 0 ? MicroBodyRef{}
                            : MicroBodyRef{(uint32_t)microModel, skinScale};
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
  // `grounded` comes from the player's own collision sweep. The gait is a
  // WALKING system — it only means anything when there is a floor under the
  // feet — so it is a parameter here rather than something UpdateAnimation
  // infers, and the airborne pose is handled explicitly (see UpdateAirPose).
  //
  // `playerVel` is the player's TRUE velocity (world voxels/sec). It is passed
  // in rather than differenced from origin_ because the player moves once per
  // FRAME while this runs 0..4 times per frame — so a position delta over
  // kTickDt measures the wrong interval every frame and reads zero on the
  // second tick of a double-tick frame. See the note at the top of
  // UpdateAnimation; that mismeasurement was the source of the limb jitter.
  void UpdateAnimation(float dt, World& world, bool grounded,
                       const Vec3& playerVel);
  void UpdateGait(float dt, World& world);
  // Airborne leg pose: relaxes the legs toward their rest hang instead of
  // leaving IK chasing a stale world-space foot plant.
  void UpdateAirPose(float dt);
  // `outMat` receives the material id of the supporting voxel — the probe
  // already reads it to decide what carries weight, so returning it costs
  // nothing and is what lets a footstep know which surface it landed on.
  bool GroundHeightAt(World& world, int wx, int wz, int yFrom, int& outY,
                      uint32_t* outMat = nullptr) const;
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
  std::vector<Footfall> footfalls_;  // drained once per frame by the caller

  Vec3 origin_{};                    // prefab min corner, world voxels
  float heading_ = 0;
  float bodyY_ = 0;
  Vec3 bodyUp_{0, 1, 0};
  float speedNow_ = 0;
  bool footInit_ = false;
  // How strongly the leg IK is applied, 0..1. Eased rather than switched: on
  // bumpy ground `grounded` is genuinely ragged, and gating the IK on it as a
  // bool made the legs and arms snap between the IK pose and the rest hang on
  // every bump — the "arms shoot straight up going uphill" tweaking. See the
  // note in UpdateAnimation.
  float gaitWeight_ = 0.0f;
  bool wasGrounded_ = true;
  // Seconds spent continuously off the ground. `grounded` flickers false for a
  // tick at a time crossing bumpy terrain, and the air-state clips used to fire
  // on that raw edge — retriggering the arms-up `jump` one-shot on every bump.
  // This debounces it; see the note in PreTick.
  float airOffTime_ = 0.0f;
  // Downward speed on the last airborne tick, voxels/sec. Sampled while still
  // falling because the collision sweep zeroes vel.y before `grounded` flips.
  float lastFallSpeed_ = 0;
  bool running_ = false;
  float airTime_ = 0;
  uint64_t id_ = 0x5A11EDU;          // stable seed for gore variance
  // ---- the rig this instance actually animates ----------------------------
  //
  // A COPY of def_->skel plus def_->limbs, owned per instance, because a held
  // ITEM borrows a real rig slot: equipping appends a part, and the shared def
  // must not grow a sword every time somebody picks one up. Everything that
  // used to read def_->skel / def_->limbs reads these instead, so an item slot
  // is indistinguishable from a limb to the animation runtime, the IK, the
  // renderer and the damage path — which is the entire point of the borrowed
  // slot, and what preserves severing, dropping and per-voxel carving for free.
  //
  // Rebuilt from the def on spawn and on hot reload; `parts`, `skel_.parts`
  // and `limbs_` stay index-parallel, which several loops depend on.
  AnimSkeleton skel_;
  std::vector<MobLimbDef> limbs_;
  const AnimSkeleton& Skel() const { return skel_; }

  // The equipped item's slot, or -1. This is the ONE piece of entity<->slot
  // state; keeping the sync in EquipItem alone is what stops the two views of
  // "what is in the hand" from drifting.
  int heldSlot_ = -1;
  std::string heldItem_;

  // Held weapon + swing pose, pushed in by main.cpp (SetWeaponPose). Pure
  // presentation, like everything else here.
  std::string heldPart_;
  int heldPartIndex_ = -1;
  // Where the fist closes on the held item, in that item's BODY frame (i.e.
  // after Jolt's centre-of-mass recentring, so it can be used directly against
  // a live body transform). Set by EquipItem; meaningless when heldSlot_ < 0.
  //
  // This exists because the held slot is placed DIRECTLY from the hand's live
  // transform rather than through the anchorLimb/restOffset pair the rig's
  // other parts use. Those fields describe a joint between two limbs of one
  // prefab; an item is a foreign object whose only relationship to the rig is
  // "this point of me sits at that point of the hand", and saying that in one
  // subtraction is both shorter and impossible to get subtly wrong.
  Vec3 gripBody_{};
  Vec3 weaponHand_{}, weaponDir_{0, 1, 0}, weaponUp_{0, 0, 1};
  float weaponWeight_ = 0;

  // Head look: the goal set by SetLook, and the smoothed value the rig is
  // actually posed at. Two of them so the head EASES onto the mouse rather
  // than stepping with it — the same reason the body yaw has a half-life.
  float lookYawGoal_ = 0, lookPitchGoal_ = 0;
  float lookYaw_ = 0, lookPitch_ = 0;

  static constexpr float kSeverHoldSeconds = 0.25f;
  // (the drip op budget is now gore.bleedOpsPerTick in tuning.json)
};
