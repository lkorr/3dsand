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
// THE AVATAR IS A MOB. PlayerAvatar derives from Mob (game/mob.h) and inherits
// every body MECHANIC from it — damage, severing, dying, per-voxel carving,
// burning/dissolution, bleeding, item holding, rendering. A chemical reaction,
// a blast or a blade that works on an NPC works on the player by the same
// single implementation, with no second copy to drift.
//
// What this class adds is the DRIVER and the player-only surface:
//   - position and facing come from Player (input + the voxel sweeps in
//     player.cpp), not from MobSystem's sense/steer/drive AI;
//   - its own animation pass (gait tuned for a player-driven body, ledge-hang
//     arm IK, head look, weapon-arm IK, footfall events);
//   - the health API the caster VM spends (TotalHealth/SpendHealth);
//   - fall/impact damage driven by Player::impactDeltaV;
//   - first-person part hiding, camera transforms, persistence ('AVTR').
//
// Everything else it does differently is an EXPLICIT override of Mob's
// virtual seam (AvatarLayer, OnBodyReleasedToWorld, DropLimbListOnDeath,
// MarkInstancesDirty) — never a parallel copy of shared mechanics.
//
// DETERMINISM (CLAUDE.md rule 1). Every field here is CPU-float PRESENTATION
// state, exactly like Mob's: poses, springs, camera offsets and the ragdoll
// are never hashed and never touch the grid. The only grid contact travels
// through the same BrushOp/CellOp/ParticleSpawn queues mobs use (rule 3).
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
  // Lower arms (forearms). Needed as their own fields because the first-person
  // keep list is built from THIS struct: with only the upper arms and the
  // hands named, a first-person view showed a floating hand and a stub of
  // bicep with the forearm missing between them.
  int armLL = -1, armLR = -1;
  int footL = -1, footR = -1;
  int legUL = -1, legUR = -1;
  int staff = -1;
};

// Locomotion clip indices, resolved once per def load. These are looked up on
// the per-tick path (PreTick runs four times a frame) and FindClip is a linear
// string scan — see the note at ResolveParts.
struct AvatarLocoClips {
  int idle = -1, walk = -1, run = -1, fall = -1, hang = -1;
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

class PlayerAvatar : public Mob {
 public:
  PlayerAvatar() { id_ = 0x5A11EDU; }  // stable seed for gore variance

  // `mobs` is the shared-services system (Mob::sys_): the def list, the one
  // compiled reaction table, the micro brick pool and the event sinks. The
  // avatar borrows all of it rather than keeping copies — the player must not
  // burn by a second reading of the reaction table. Required for a spawned
  // avatar; the default exists only for legacy call sites.
  void Init(Physics* phys, World* world, DebrisSystem* debris,
            const std::vector<MaterialDef>& mats, MobSystem* mobs = nullptr);
  // Material tables now live on MobSystem; kept for call-site compatibility.
  void OnMaterialsReloaded(const std::vector<MaterialDef>& mats);
  // Points the avatar at a def by name. Safe to call repeatedly (hot reload):
  // despawns first, so limb bodies never leak across a reload.
  //
  // LIFETIME: `defs` must outlive the avatar, and MUST be re-published after
  // any MobSystem::SetDefs — that call replaces the vector's contents, so
  // every MobDef* into it (including def_) dangles afterwards.
  void SetDefs(const std::vector<MobDef>* defs, const std::string& defName);
  bool HasDef() const { return def_ != nullptr; }

  // Creates the limb bodies at the player's current position. No-op if already
  // spawned. Returns false if the def is missing or physics refused a body.
  bool Spawn(const Player& player, float headingRad);
  void Despawn();                 // world regen / teleport / mode change
  bool Spawned() const { return spawned_; }

  // Once per tick, BEFORE Physics::Step and debris.PreTick — mirrors the
  // ordering MobSystem::PreTick relies on. Drives the rig from the player's
  // state (THE driver seam — this is what replaces MobSystem's AI stages),
  // submits kinematic limb targets, and runs the shared Mob body upkeep
  // (burning, bleeding, severed holds) exactly as MobSystem does for NPCs.
  //
  // `heading` is the direction the BODY faces, which is not the camera yaw:
  // main.cpp owns that policy and passes the result in.
  void PreTick(uint32_t tick, const Player& player, float heading, float dt,
               World& world, std::vector<BrushOp>& ops,
               std::vector<CellOp>& cellOps,
               std::vector<ParticleSpawn>& spawns);
  // Set fire to up to `count` of a part's surface voxels; returns how many
  // took. Thin wrapper over Mob::Ignite, kept for API stability.
  uint32_t IgnitePart(int partIndex, uint32_t count, uint32_t onlyMat = 0);
  // Voxels of `part` currently alight, for the burn gate and the debug overlay.
  uint32_t PartBurningCount(int part) const {
    return part >= 0 && part < (int)limbs_.size()
               ? (uint32_t)limbs_[part].burn.front.size()
               : 0u;
  }
  uint32_t PartMaterialCount(int part, uint32_t mat) const;
  // Cells in a part's dense burn index; 0 = the index does not exist, i.e.
  // nothing reactive has come near it. Diagnostic.
  uint32_t PartBurnIndexCells(int part) const {
    return part >= 0 && part < (int)limbs_.size()
               ? (uint32_t)limbs_[part].burn.idx.size()
               : 0u;
  }

  // ---- head look ------------------------------------------------------------
  // Where the character is LOOKING, as an offset from where the body is
  // FACING. Pushed in once per tick by main.cpp before PreTick. `yawRel` is
  // camera yaw minus body heading, radians, wrapped to (-pi, pi]; `pitch` is
  // camera pitch, radians, positive up. Both are CLAMPED here against the
  // neck limits in tuning.json rather than trusted.
  void SetLook(float yawRel, float pitch);

  // ---- footfall events (presentation only) --------------------------------
  // A foot touching down, produced by the gait's own plant moment rather than
  // by a distance accumulator. These QUEUE because PreTick runs inside the
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

  // (EquipItem / HeldItem / HeldSlot / WeaponEdge / OwnsBody / SetWeaponPose
  // are inherited from Mob — item holding is base-class scaffolding now, so a
  // mob can wield a sword through the identical path.)

  // ---- damage / dismemberment ----
  // Damage(bodyHandle, ...) and Sever(limbIndex) are inherited from Mob:
  // the player takes hits through the same code as any creature.
  // Debug/testing: sever by authored part name. Returns false on an unknown
  // name or an already-severed part.
  bool SeverByName(const std::string& name);
  void Revive(const Player& player, float heading);  // heal + respawn all parts

  // ---- health, as the caster VM sees it (game/spell.h) --------------------
  // The player has no single hp field, and deliberately gains none here:
  // health IS the summed per-part hp the dismemberment system maintains,
  // rounded to an integer because the caster VM is integer throughout.
  int32_t TotalHealth() const;
  // The authored total across every limb — the HUD bar's denominator. Health
  // NEVER regenerates; deliberately not lowered by severing, so a lost limb
  // reads as a permanently short bar.
  int32_t HealthMax() const;
  // Spend health across live parts, proportionally to what each still has.
  // Parts driven to zero are severed through the ordinary Sever() path, so an
  // overcast dismembers you with no new gore code.
  void SpendHealth(int32_t amount);
  // Kill the caster spectacularly at `atWorldVoxel` (FATAL overcast).
  void SelfDestruct(Vec3 atWorldVoxel, float radiusVox, World& world,
                    std::vector<ParticleSpawn>& spawns);
  // Explosion damage to the avatar's body. Same call shape as
  // MobSystem::CarveMobsRadial so the explosion loop in main.cpp treats the
  // avatar and mobs identically — and since the refactor it IS the same code:
  // real per-voxel carving via Mob::CarveRadialAll, not an hp approximation.
  void CarveRadial(Vec3 centerWorldVoxel, float radiusVoxels, World& world,
                   std::vector<ParticleSpawn>& spawns);

  // Impact damage, driven by Player::impactDeltaV — the velocity a collision
  // sweep refused. Covers falls and horizontal wall slams with one path.
  // `centerWorldVoxel` is the player AABB centre (mid-torso), NOT origin_.
  void ApplyFallDamage(Vec3 impactDeltaV, Vec3 centerWorldVoxel, uint32_t tick,
                       World& world, std::vector<BrushOp>& ops,
                       std::vector<ParticleSpawn>& spawns);

  // ---- persistence (sim/worldio.h, entities.sve section 'AVTR') -----------
  // Per-part hp and sever state, def by NAME. LoadState runs while the avatar
  // is despawned, so it only RECORDS the state; the next Spawn() applies it.
  // A dead avatar is saved as absent: its corpse is debris ('DBRS').
  static constexpr uint32_t kSaveVersion = 1;
  void SaveState(std::vector<uint8_t>& out) const;
  bool LoadState(const uint8_t* data, size_t len, uint32_t version);
  void ClearPendingRestore() { restore_.valid = false; }

  bool IsAlive() const { return alive_; }
  bool PartAlive(int i) const { return LimbAlive(i); }
  const AvatarParts& Parts() const { return parts_; }
  // What movement and the camera should do about the current damage state.
  AvatarLocomotion Locomotion() const;

  // Model-space pose of a part, or identity when the part is gone. The camera
  // uses this to ride the head.
  bool PartWorldTransform(int part, Vec3& outPos, Quat& outRot) const;
  // World position of a part's joint anchor — where the camera boom pivots and
  // where a first-person eye sits.
  bool PartAnchorWorld(int part, Vec3& out) const;

  // ---- render plumbing ----
  // Inherited from Mob (identical slot walk to MobSystem's), except that the
  // avatar owns its instance-dirty flag: shadow AppendInstances to clear it.
  bool InstancesDirty() const { return instancesDirty_; }
  uint32_t AppendInstances(std::vector<BodyVoxInst>& out, uint32_t slotBase) {
    const uint32_t next = Mob::AppendInstances(out, slotBase);
    instancesDirty_ = false;
    return next;
  }
  // Parts to SKIP when drawing — first person hides the body but keeps the
  // arms. Empty in third person. Set by main.cpp from the camera mode.
  void SetHiddenParts(const std::vector<uint8_t>& hidden);
  // Total parts on the live rig, INCLUDING a borrowed item slot.
  int PartCount() const { return (int)limbs_.size(); }

  // introspection (overlay / selftest)
  // Jolt body of a part, or 0 when it is severed or never spawned.
  uint64_t PartBody(int part) const;
  int PartIndex(const std::string& name) const;
  int LivePartCount() const;
  // ---- per-part condition, for the HUD body readout -----------------------
  // `part` indexes the same array PartAlive/PartBody take, which INCLUDES the
  // borrowed item slot past the def's limb count.
  float PartHp(int part) const {
    return part >= 0 && part < (int)limbs_.size() ? limbs_[part].hp : 0.0f;
  }
  // Authored ceiling for one part (MobLimbDef::hp), the denominator of the
  // damage tint. Like HealthMax() it is never lowered by damage.
  float PartHpMax(int part) const {
    return part >= 0 && part < (int)limbDefs_.size() ? limbDefs_[part].hp : 0.0f;
  }
  // ---- how much of a part is still THERE ----------------------------------
  // hp answers "how hurt is this limb"; these answer "how much of it is left",
  // and they are genuinely different questions the moment carving exists — a
  // laser can bore a limb hollow without ever driving its hp to zero, and a
  // blast can shave hp off a limb that has lost no geometry at all. The
  // character panel's inspect view reports the ratio, so "62% intact" is a
  // measurement of the actual voxels rather than a restatement of the hp bar.
  //
  // COUNTED ON THE LATTICE `voxelsAtSpawn` WAS COUNTED ON — the SKIN whenever
  // the limb has one, the collider otherwise. Mob::CarveLimbRadial says why in
  // as many words: the two lattices differ by (skinScale/physScale)^3, so
  // mixing them scales the fraction by that factor. Measured, on mina
  // (skinScale 8, physScale 4): an untouched limb reported "12% intact",
  // which is 1/8 — the ratio, not the damage.
  uint32_t PartVoxelCount(int part) const {
    if (part < 0 || part >= (int)limbs_.size()) return 0u;
    const MobLimb& l = limbs_[part];
    return (uint32_t)(l.HasFineSkin() ? l.skinVoxels.size() : l.voxels.size());
  }
  uint32_t PartVoxelsAtSpawn(int part) const {
    return part >= 0 && part < (int)limbs_.size() ? limbs_[part].voxelsAtSpawn
                                                 : 0u;
  }

  // Actively losing blood: either an arterial gush from a fresh stump or an
  // ordinary wound still owing whole voxels of blood.
  bool PartBleeding(int part) const {
    if (part < 0 || part >= (int)limbs_.size()) return false;
    return limbs_[part].gushTicks > 0 || limbs_[part].bleedBudget >= 1.0f;
  }
  // Authored name / tag ("head", "arm", ...) of a limb, or "" for a borrowed
  // item slot. The HUD lays the figure out by TAG so it works for any
  // humanoid rig rather than only for one rig's part names.
  const char* PartName(int part) const {
    return part >= 0 && part < (int)limbDefs_.size()
               ? limbDefs_[part].name.c_str()
               : "";
  }
  const char* PartTag(int part) const {
    return part >= 0 && part < (int)limbDefs_.size()
               ? limbDefs_[part].tag.c_str()
               : "";
  }
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
    if (c < 0 || c >= (int)skel_.clips.size()) return "?";
    return skel_.clips[c].name.c_str();
  }
  int LocoState() const { return anim_.locoState; }
  // ---- stride clock readouts (diagnostics and the mob gate) ----------------
  // The pelvis bob/sway and the arm clips run off THIS phase, and the feet are
  // what advance it. Counting its cycles against the footfalls is the only way
  // to assert the two clocks are one — a bob at 2.6x the footfall rate looks
  // fine in every per-part pose measure and is exactly the reported jitter.
  float GaitPhase() const { return anim_.gaitPhase; }
  float StrideRate() const { return strideRate_; }   // strides/sec, 0 = parked
  // How far the pelvis is dropped below the player's AABB sole, world voxels.
  float StanceCrouch() const { return stanceCrouch_; }
  // Requested weight of the named clip's live instance, or 0 if not running.
  // The fall flail is a RAMP now, so "is the fall clip active" is no longer the
  // question — "how far in is it" is.
  float ClipWeight(const char* name) const {
    if (!def_) return 0.0f;
    const int c = skel_.FindClip(name);
    if (c < 0) return 0.0f;
    for (const ClipInstance& inst : anim_.clips)
      if (inst.clip == c && !inst.stopping) return inst.weight;
    return 0.0f;
  }
  bool ClipActive(const char* name) const {
    if (!def_) return false;
    const int c = skel_.FindClip(name);
    if (c < 0) return false;
    for (const ClipInstance& inst : anim_.clips)
      if (inst.clip == c && !inst.stopping) return true;
    return false;
  }

 protected:
  // ---- THE EXPLICIT EXCEPTIONS (Mob's virtual seam) -------------------------
  // The player's limbs live on the AVATAR physics layer: they sit inside the
  // player capsule by construction, and on the normal layer the solver fights
  // an unresolvable contact whose ejection vector swings with the gait — the
  // "walking forward drifts backwards" bug. The layer is identical in every
  // other respect and stays visible to rays.
  bool AvatarLayer() const override { return true; }
  // A body leaving the rig for the world stops being "you": back on the
  // normal layer it can bump the player like any other debris.
  void OnBodyReleasedToWorld(uint64_t bodyHandle) override;
  // Keep the limb list on death so the HUD's per-part readout survives the
  // death screen; Despawn/Revive tears it down instead of the husk sweep.
  bool DropLimbListOnDeath() const override { return false; }
  // The avatar renders through its own slot range with its own dirty flag.
  void MarkInstancesDirty() override { instancesDirty_ = true; }

 private:
  // Damage state read from a save (LoadState), applied at the end of the next
  // Spawn() — the rig it applies to only exists once Spawn has built it.
  struct SavedState {
    bool valid = false;
    std::string defName;
    struct P {
      uint8_t alive = 1;
      float hp = 0;
    };
    std::vector<P> parts;
  };
  SavedState restore_;

  // ---- the PLAYER DRIVER's animation pass ----------------------------------
  // These are the avatar's own: a player-driven body needs its gait fed by
  // the player's true velocity and grounded state, ledge-hang arm IK, head
  // look and the weapon arm. The MECHANICS underneath (clips, IK solver,
  // springs, dismemberment states) are the shared anim runtime.
  void UpdateAnimation(float dt, World& world, bool grounded,
                       const Vec3& playerVel);
  void UpdateGait(float dt, World& world);
  // Airborne leg pose: relaxes the legs toward their rest hang instead of
  // leaving IK chasing a stale world-space foot plant.
  void UpdateAirPose(float dt);
  // Lock the pelvis/arm clock onto a foot touchdown on leg chain `chain`.
  // The ONLY writer of strideRate_ and of anim_.gaitPhase's correction term.
  void SyncStrideClock(int chain);
  void ResolveParts();
  // Per-voxel burning under the avatar's PRIVATE budget — a crowd of burning
  // NPCs must not starve the fire on the player character.
  void BurnParts(uint32_t tick, World& world, std::vector<CellOp>& cellOps,
                 std::vector<ParticleSpawn>& spawns);

  const std::vector<MobDef>* defs_ = nullptr;
  std::string defName_ = "wizard";

  AvatarParts parts_;
  AvatarLocoClips locoClips_;
  bool spawned_ = false;
  bool instancesDirty_ = false;
  std::vector<Footfall> footfalls_;  // drained once per frame by the caller

  bool footfallInit_ = false;
  // How strongly the leg IK is applied, 0..1. Eased rather than switched: on
  // bumpy ground `grounded` is genuinely ragged, and gating the IK on it as a
  // bool made the legs and arms snap between the IK pose and the rest hang on
  // every bump. See the note in UpdateAnimation.
  float gaitWeight_ = 0.0f;
  bool wasGrounded_ = true;
  // Was the support a LEDGE HANG last tick? Losing that support must not fire
  // the "jump" clip — the arms are already up in the hang pose.
  bool wasHanging_ = false;
  // ---- ledge-hang arm IK ----
  // Mirrored out of Player each PreTick so UpdateAnimation (whose signature
  // deliberately stays player-free) can pin the palms to the held lip.
  bool hangActive_ = false;
  IVec3 hangLipW_{};        // the held lip voxel, world
  Vec3 hangDirW_{1, 0, 0};  // horizontal facing at grab time, toward the wall
  float hangIkWeight_ = 0.0f;  // faded like gaitWeight_, never a hard switch
  // Seconds spent continuously off the ground; debounces the flickering
  // `grounded` bit so air-state clips fire once per real takeoff.
  float airOffTime_ = 0.0f;
  // Downward speed on the last airborne tick, voxels/sec. Sampled while still
  // falling because the collision sweep zeroes vel.y before `grounded` flips.
  float lastFallSpeed_ = 0;
  bool running_ = false;
  float airTime_ = 0;
  // Previous tick's Player::jumped, so the jump clip fires on the LATCH'S
  // rising edge. The latch is sticky across a whole frame's tick batch and a
  // one-shot rewinds when retriggered, so the raw flag would freeze it at t=0.
  bool prevJumpLatch_ = false;
  // Body height (origin_.y, world voxels) the last time anything was holding
  // the body up. `supportY_ - origin_.y` is how far this fall has actually
  // dropped, which is the question "is this a fall" really asks — air time
  // alone answers it wrong for every step-down.
  float supportY_ = 0.0f;

  // ---- the stride clock (PlayerAvatar::SyncStrideClock) --------------------
  // The pelvis bob/sway/roll and the walk/run arm clips are driven from the
  // FEET, not from a free-running oscillator. These are its whole state:
  // seconds since the last touchdown, the smoothed step period it measures,
  // the stride rate derived from that (strides/sec, 0 = not walking) and which
  // leg landed last.
  float sinceTouchdown_ = 0.0f;
  float stepPeriod_ = 0.0f;
  float strideRate_ = 0.0f;
  int lastFootDown_ = -1;
  // How far the pelvis is dropped below the player's AABB sole, world voxels.
  // Derived from the rig and the current speed — this is the reach the stride
  // is spent out of. See the stance note in UpdateGait.
  float stanceCrouch_ = 0.0f;

  // Head look: the goal set by SetLook, and the smoothed value the rig is
  // actually posed at. Two of them so the head EASES onto the mouse rather
  // than stepping with it — the same reason the body yaw has a half-life.
  float lookYawGoal_ = 0, lookPitchGoal_ = 0;
  float lookYaw_ = 0, lookPitch_ = 0;
};
