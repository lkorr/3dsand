#pragma once
#include <cstdint>
#include <functional>
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
//   - the INPUTS to the one pose pipeline every creature runs (Mob::
//     PosePipeline, game/pose.h): the player's velocity and debounced support,
//     a driver-owned height, the camera's head-look goal, the held crouch and
//     the ledge-hang lip — plus the player-only clip family (jump/land/fall/
//     hang) and footfall events;
//   - the health API the caster VM spends (TotalHealth/SpendHealth);
//   - fall/impact damage driven by Player::impactDeltaV;
//   - first-person part hiding, camera transforms, persistence ('AVTR').
//
// Everything else it does differently is an EXPLICIT override of Mob's
// virtual seam (AvatarLayer, OnDying, MarkInstancesDirty) — never
// a parallel copy of shared mechanics.
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

// ONE SHAPE OF THE AIRBORNE POSE is game/pose.h AirKeyPose (AvatarAirKey is
// its alias), since every creature runs the air pose (W2-L).

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
  // THE ID IS A SEED, NOT A SLOT. `id_` keys every per-creature RNG draw the
  // body makes (`Hash3(id ^ salt, tick, i)` — gore variance, wound placement,
  // tempo jitter), so two avatars sharing it would bleed in lockstep. Session 0
  // keeps 0x5A11ED exactly, because that constant is baked into every pinned
  // world hash and every gate's expected gore spread; session i passes
  // 0x5A11ED + i (M9.1, docs/PLAN_multiplayer_m9.md §2 P2). The default
  // argument is what keeps the ~30 existing one-player construction sites
  // byte-identical.
  //
  // NOT the ACTOR id — that is ai::kPlayerActorId + index, a different space
  // (see ai_behavior.h). This one is a MOB id, the key MobSystem::AvatarById
  // and every handle-keyed lookup use.
  // A player's body is driven by a player from birth (Mob::Controller);
  // MobSystem::SetAvatars re-stamps a peer's as RemoteGhost.
  explicit PlayerAvatar(uint64_t id = 0x5A11EDU) {
    id_ = id;
    controller_ = Controller::LocalPlayer;
  }

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
  // Voxels of `part` currently ON FIRE, for the burn gate, the debug overlay
  // and the health screen. The burn FRONT is a wider set — every voxel with a
  // self decay/emit rule, which includes drying blood and crumbling char — and
  // reporting it here is what made a charred, long-extinguished limb read
  // BURNING for the rest of the session. See BodyBurnState::hotVox.
  uint32_t PartBurningCount(int part) const {
    return part >= 0 && part < (int)limbs_.size() ? limbs_[part].burn.hotVox
                                                  : 0u;
  }
  uint32_t PartMaterialCount(int part, uint32_t mat) const;
  // PartMaterialCount for MANY materials in ONE walk of the part's voxels:
  // `binMask[mat & 0xFFF]` names the bins (bit k -> `bins[k]`) a voxel of
  // that material counts into; bins are ADDED to, not cleared. The HUD asks
  // ~10 materials per limb per frame, and ten walks of a fine skin is the
  // cost this exists to remove (docs/PLAN_perf_audit_2026-09-23.md P6.1).
  void PartMaterialTally(int part, const uint16_t* binMask,
                         uint32_t* bins) const;
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
  // The controller this body is driven by. Mob::AddBodyVelocity on a live
  // player-driven body writes Player::vel through it (a `float aura` on the
  // caster), and Mob::BodyVelocity reads it (the go-limp velocity). The owner
  // binds it once; it must outlive the avatar or be rebound to null.
  void BindPlayer(Player* p) { player_ = p; }

  // ---- footfall events (presentation only) --------------------------------
  // `Footfall`, `Footfalls()` and `ClearFootfalls()` are inherited from Mob:
  // every creature plants feet, and P2's coat shedding runs off the same
  // moment, so the event could not stay player-only. main.cpp still drains
  // and clears the avatar's queue once per frame — that is the avatar's half
  // of the contract and it is unchanged.

  // (EquipItem / HeldItem / HeldSlot / WeaponEdge / WeaponArmPose / OwnsBody /
  // SetWeaponPose are inherited from Mob — item holding is base-class
  // scaffolding now, so a mob can wield a sword through the identical path.)

  // ---- damage / dismemberment ----
  // Damage(bodyHandle, ...) and Sever(limbIndex) are inherited from Mob:
  // the player takes hits through the same code as any creature.
  // Debug/testing: sever by authored part name. Returns false on an unknown
  // name or an already-severed part.
  bool SeverByName(const std::string& name);
  void Revive(const Player& player, float heading);  // heal + respawn all parts

  // ---- the death screen's photograph --------------------------------------
  // Whoever mirrors this body into the HUD registers here, and is called back
  // ONCE, from inside Die(), while the rig is still whole (Mob::OnDying).
  // That instant is the whole point: at the top of the next MobSystem PreTick
  // the rig moves into mobs_ as the player's corpse (AdoptDeadAvatar), the
  // avatar keeps a husk, `anim_.partAlive` is zeroed and the per-limb
  // readouts all answer "severed, empty, zero hp" — so a mirror taken after
  // the fact can only ever describe a husk, whatever killed it.
  //
  // A callback rather than a snapshot struct here because the thing worth
  // photographing is the UI mirror main.cpp already knows how to build
  // (FillBodyUI), and a second per-limb copy in this header would be a second
  // thing to keep in step with it.
  void SetDyingObserver(std::function<void()> fn) { onDying_ = std::move(fn); }
  void OnDying() override { if (onDying_) onDying_(); }

  // ---- health, as the caster VM sees it (game/spell.h) --------------------
  // The player has no single hp field, and deliberately gains none here:
  // health IS the summed per-part hp the dismemberment system maintains,
  // rounded to an integer because the caster VM is integer throughout.
  int32_t TotalHealth() const;
  // The authored total across every limb — the HUD bar's denominator. Health
  // NEVER regenerates; deliberately not lowered by severing, so a lost limb
  // reads as a permanently short bar.
  int32_t HealthMax() const;
  // HealthMax() x the burn cap (Mob::BurnHealthCap): the most health this body
  // can HOLD in its current state. Equal to HealthMax() unburnt; the HUD bar
  // draws the span between the two as charred off, and nothing may heal past
  // it (sim/tuning.h Gore §G).
  int32_t HealthCap() const;
  // Spend health across live parts, proportionally to what each still has:
  // Mob::SpendHp with the Other cause, so hp reaching zero means what it means
  // everywhere else (a vital part dies in place; an arm stays on).
  void SpendHealth(int32_t amount);
  // Kill the caster spectacularly at `atWorldVoxel` (FATAL overcast): the
  // ordinary radial carve (Mob::CarveRadialAll), then death.
  void SelfDestruct(Vec3 atWorldVoxel, float radiusVox, World& world,
                    std::vector<ParticleSpawn>& spawns);
  // Explosion damage to the avatar's body. Same call shape as
  // MobSystem::CarveMobsRadial so the explosion loop in main.cpp treats the
  // avatar and mobs identically — and since the refactor it IS the same code:
  // real per-voxel carving via Mob::CarveRadialAll, not an hp approximation.
  void CarveRadial(Vec3 centerWorldVoxel, float radiusVoxels, World& world,
                   std::vector<ParticleSpawn>& spawns);

  // ---- live ragdoll (Mob::StartRagdoll), the player's side ----------------
  // While the body is limp or getting up THE PLAYER FOLLOWS THE BODY, not the
  // other way round: main.cpp skips Player::Update and moves the capsule to
  // wherever this says every tick. Limp: the pelvis, wherever Jolt has flung
  // it. Getting up: the standing spot BeginGetUp chose, so the controller
  // resumes exactly where the animation ends. False when not ragdolled.
  //
  // The capsule itself is inert meanwhile — no gravity, no sweeps, no
  // impact latch — which is also why a body knocked flying by a blast does
  // not take fall damage on landing: the controller never saw the fall.
  bool RagdollFollow(Vec3& outPlayerPos) const;

  // Impact damage is Mob::ApplyFallDamage (W2-H), driven for the player by
  // Player::impactDeltaV — the velocity a collision sweep refused, so falls
  // and horizontal wall slams share one path — and by the limp arrest. The
  // player's `centerWorldVoxel` is the AABB centre (mid-torso), NOT origin_.

  // ---- persistence (sim/worldio.h, entities.sve section 'AVTR') -----------
  // Per-part hp and sever state, def by NAME. LoadState runs while the avatar
  // is despawned, so it only RECORDS the state; the next Spawn() applies it.
  // A dead avatar is saved as absent: its corpse is a dead Mob in mobs_
  // (MobSystem::AdoptDeadAvatar), not part of the avatar record.
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
  // The part's collider CENTRE in world voxels (PartWorldTransform's position
  // is the lattice corner): where a spell cast "on the hand" resolves.
  bool PartCentreWorld(int part, Vec3& out) const;
  // Where the ANIMATION puts a part this tick, in the same frame and form as
  // PartWorldTransform (lattice corner + rotation) — answered for a severed or
  // dead part too, because the pose is flattened over the whole skeleton. The
  // spell in an amputated hand sits where the hand would be (SpellHandPoint).
  bool PartPosedWorld(int part, Vec3& outPos, Quat& outRot) const;
  // World position of a part's joint anchor — where the camera boom pivots and
  // where a first-person eye sits.
  bool PartAnchorWorld(int part, Vec3& out) const;
  // Eye height from the creature's bottom in world voxels, derived from the
  // sidecar's eyeLocal + the head's rest-pose anchor. 0 when no eyeLocal was
  // authored (caller falls back to a constant).
  float EyeRestHeight() const { return def_ ? def_->eyeRestHeight : 0.0f; }
  bool HasEyeLocal() const { return def_ && def_->hasEyeLocal; }
  // WHERE THE EYES ARE THIS TICK, world voxels: the head's POSED transform
  // (PartPosedWorld -- the animation's answer, not Jolt's one-tick-latent
  // readback) carried out to the sidecar's eyeLocal. The first-person
  // camera rides this, so a crawl puts the eye near the floor where the head
  // is. False with no head, no eyeLocal, or a dead body.
  bool HeadEyeWorld(Vec3& out) const;

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
  uint32_t PartBrainAtSpawn(int part) const {
    return part >= 0 && part < (int)limbs_.size() ? limbs_[part].brainAtSpawn
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
  // Rig index of this part's parent, or -1 for the root. The first-person hide
  // mask needs it: a worn shell has to be kept or hidden with the part it
  // covers, and the keep-list is written in terms of BODY parts (an armoured
  // sleeve would otherwise vanish off an arm you can still see).
  int PartParent(int part) const {
    return part >= 0 && part < (int)skel_.parts.size()
               ? skel_.parts[part].parent
               : -1;
  }
  int ActiveClips() const { return (int)anim_.clips.size(); }
  // (SpeedNow is Mob's: the pose pipeline's smoothed planar speed.)
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
  // ---- stride clock / crouch / air-pose readouts ---------------------------
  // GaitPhase, StrideRate, StanceCrouch, CrouchHold, AirPoseWeight, AirPoseVy
  // and AirPoseLand are Mob's now (game/pose.h PoseDrive): every creature runs
  // the stages that produce them. The bob/sway and the arm clips run off the
  // phase and the feet advance it, so counting its cycles against footfalls
  // is how the mob gate asserts the two clocks are one.
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
  // The avatar renders through its own slot range with its own dirty flag.
  void MarkInstancesDirty() override {
    instancesDirty_ = true;
    twinDirty_ = true;  // Mob::SyncJointTwins: a lattice may have changed
  }

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
  // Called once from Die(), before the rig comes apart (SetDyingObserver).
  std::function<void()> onDying_;

  // ---- the PLAYER DRIVER's half of the pose (W2-L) -------------------------
  // The pipeline itself is Mob::PosePipeline (game/pose.cpp), shared with
  // every NPC. What stays here is FILLING ITS INPUTS: the player's true
  // velocity and debounced support, the head-look goal, the held crouch and
  // the ledge-hang lip — everything a PoseInputs field names and only the
  // player knows.
  PoseInputs AvatarPoseInputs(bool grounded, const Vec3& playerVel) const;
  // THE LIVE HALF OF Mob::AddBodyVelocity / BodyVelocity for a player-driven
  // body: the controller's own velocity. Bound by the owner (BindPlayer); an
  // unbound avatar (a harness that never runs the controller) has none.
  Vec3 DriverVelocity() const override;
  void AddDriverVelocity(Vec3 velVoxPerSec) override;
  void ResolveParts();
  // Per-voxel burning under the avatar's PRIVATE budget — a crowd of burning
  // NPCs must not starve the fire on the player character.
  void BurnParts(uint32_t tick, World& world, std::vector<CellOp>& cellOps,
                 std::vector<ParticleSpawn>& spawns);

  const std::vector<MobDef>* defs_ = nullptr;
  std::string defName_ = "human";

  AvatarParts parts_;
  AvatarLocoClips locoClips_;
  bool spawned_ = false;
  bool instancesDirty_ = false;

  bool footfallInit_ = false;
  // (The IK fade, gaitWeight, is Mob::pose_ now — every creature fades it.)
  bool wasGrounded_ = true;
  // (The airborne pose's state is Mob::pose_ — every creature runs it.)
  // Mirrored out of Player: the states that are airborne by the gait's reckoning
  // but must NOT play a jump/fall pose — a hang and a mantle are held by the
  // hands, a swimmer is carried by the water, and fly mode is not falling at
  // all. The pipeline reads PoseInputs, not Player, so this comes across
  // the same way hangActive_ does.
  bool airPoseEligible_ = false;
  // Was the support a LEDGE HANG last tick? Losing that support must not fire
  // the "jump" clip — the arms are already up in the hang pose.
  bool wasHanging_ = false;
  // ---- ledge-hang arm IK ----
  // Mirrored out of Player each PreTick so the pipeline (whose inputs
  // deliberately stays player-free) can pin the palms to the held lip.
  bool hangActive_ = false;
  IVec3 hangLipW_{};        // the held lip voxel, world
  Vec3 hangDirW_{1, 0, 0};  // horizontal facing at grab time, toward the wall
  // ---- the ledge climb out of that hang (pose.cpp, "the ledge climb") ----
  bool climbActive_ = false;
  float climbRise_ = 0.0f;  // Player::LedgeClimbRise, 0 hang .. 1 standing

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

  // (The stride clock and the stance/held crouch are Mob::pose_ — see
  // Mob::SyncStrideClock. `crouchWant_` below is the player's REQUEST, mirrored
  // from Player::crouching into PoseInputs::crouch.)
  bool crouchWant_ = false;

  // Head look: the GOAL set by SetLook (the avatar's release-band policy).
  // The smoothed angles the rig is actually posed at are Mob::pose_'s, eased
  // inside the pipeline so the head EASES onto the mouse.
  float lookYawGoal_ = 0, lookPitchGoal_ = 0;
  // The controller whose velocity this body's live integrator IS (BindPlayer),
  // and the velocity PreTick last saw, for an avatar nobody bound.
  Player* player_ = nullptr;
  Vec3 playerVel_{};
};
