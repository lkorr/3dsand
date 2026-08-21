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
  // the cube path. Only ever set for defs with skinScale > 1; a limb whose
  // model failed to pack keeps -1 and simply does not render (cube instances
  // are one WORLD voxel each, so they would draw it at skinScale times its
  // real size).
  int microModel = -1;
};

// An attachment point on a rig: the frame a held item is placed in.
//
// Deliberately tiny. A socket is a POINT AND A FRAME on one part, nothing
// more — no item knowledge, no grip data. Anything about how a particular
// weapon sits in a hand belongs to that weapon (ItemDef::grip), so the same
// socket serves a sword, a torch and an empty hand without edits.
struct MobSocketDef {
  // The CONTEXT this socket serves — "held_right" — matching the key an item
  // uses in its own `grip` map. Deliberately NOT the limb's name: an item asks
  // to be held in a context, and which limb provides that context is the rig's
  // business. A left-handed creature puts "held_right" on its hand.L and every
  // item still hangs correctly with no per-item edits.
  std::string name;
  std::string part;            // rig part it rides; resolved to an index at load
  int partIndex = -1;
  // Offset from the part's own model corner, in WORLD voxels (converted from
  // the sidecar's micro units at load, with the anchors).
  Vec3 offset{};
  // Extra rotation of the socket frame. Normally identity: the hand's frame IS
  // the socket frame, and putting a rotation here as well as in the item's
  // grip would mean two places encode "which way does a held thing point",
  // which is exactly how those two drift apart.
  Quat rotation{};
};

// Ceiling on the DERIVED collider resolution (MobDef::physScale).
//
// The int8 bound alone is not the whole constraint. Collider cost is a voxel
// COUNT — Jolt greedy-merges the lattice into boxes and then solves contacts
// against them — so it grows as the cube of the resolution, while the skin is
// one OBB whose cost is screen area. Deriving "the finest collider that fits
// in ±120" would hand a 68-voxel limb an 8× collider purely because it is
// small enough to get away with, which is the coupling this split exists to
// break: the cheap axis would once again be paying the expensive one's price.
//
// 4 keeps every current rig at or below the resolution it already shipped
// with, so no existing creature's mass, contacts or ground probes move, while
// leaving the skin free to go to 8. Raising this is a physics-budget decision,
// not an art one.
inline constexpr uint32_t kMaxPhysScale = 4;

struct MobDef {
  std::string name;
  Prefab prefab;               // one model per limb
  int rootLimb = -1;           // index into limbs
  uint32_t bleedMat = 0;       // 0 = mob does not bleed
  float bleedPerDamage = 1.5f; // wound budget voxels per point of damage
  float speed = 4.0f;          // voxels/sec walk speed
  // Micro-voxel AUTHORING scale (docs/PLAN_voxel_editor.md §C): 1 = the legacy
  // path (limb .vox coords ARE world voxels, drawn as instanced cubes), 2|4|8 =
  // limb .vox coords are MICRO units, `skinScale` of them per world voxel. The
  // renderer marches the limb's brick instead of emitting a cube per voxel
  // (sim/microbody.h).
  //
  // This is the ART's resolution and the units the .vox file is authored in.
  // It is NOT the collider's resolution — see `physScale` below. The two were
  // one field until the skin/collider split, and separating them is what lets
  // an 8x limb exist at all: DebrisVoxel is int8, so a single lattice capped a
  // limb at 120/scale world voxels, and mina is 17 world voxels tall.
  //
  // NOTE FOR CALLERS: at skinScale>1 the limb voxel coordinates and sizes
  // stored in MobSystem::Limb stay in COLLIDER (physScale) units, but
  // everything the rig and the simulation touch — anchors, rest transforms,
  // restOffset, world positions — is divided into WORLD voxels at load. One
  // conversion point, so the animation runtime and the gait code need no scale
  // awareness at all.
  uint32_t skinScale = 1;
  // Collider resolution, in collider voxels per world voxel. DERIVED at load,
  // never authored: the finest of {8,4,2,1} whose limb extents still fit the
  // DebrisVoxel int8 bound of +-120. Always <= skinScale.
  //
  // Engine-picked because the bound it has to satisfy is a property of the art
  // (how big the limbs are), not a choice an author can make usefully. That
  // makes collider resolution an EMERGENT property, which is a real downside —
  // it silently changes mass, contacts and ground probes — so LoadMobDefs logs
  // the value it picked for every def rather than letting it move unnoticed.
  uint32_t physScale = 1;
  // Prefab bounding box in WORLD voxels (= prefab.size / skinScale). Every
  // piece of gameplay geometry — gait pivots, terrain anchors, ground probes —
  // reads this rather than `prefab.size`, which stays in the .vox's own (micro)
  // units.
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
  // Where a held ITEM attaches. The rig states only WHERE THE FIST CLOSES; the
  // item states how it sits in that fist (its own `grip` block), and the
  // runtime composes socket x grip — see game/item.h.
  //
  // The split is the whole point. A weapon used to be a limb of the rig, and
  // that is what broke: prefab-local space is rebased on the BODY's min corner
  // (props are excluded from that measurement, because a creature's size must
  // not change with its luggage), so a blade reaching past that corner landed
  // at NEGATIVE prefab-local coordinates, which the space cannot represent.
  // An item owning its own origin cannot do that to the body wearing it, and
  // one sword now fits any rig that publishes a hand socket.
  std::vector<MobSocketDef> sockets;

  int FindSocket(const std::string& n) const {
    for (size_t i = 0; i < sockets.size(); i++)
      if (sockets[i].name == n) return (int)i;
    return -1;
  }
};

// Loads assets/mobs/*.vox + matching .json sidecars. Appends problems to log;
// defs that fail validation are skipped. Limb models of defs with "skinScale" > 1
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
  // Collision-box debug overlay (world.h DebugBox, the dev panel's "collision
  // boxes" toggle). One oriented wireframe per LIVE limb body, read from the
  // body's actual Jolt collider via Physics::GetLocalBounds — not from the
  // limb's art, so a collider that has drifted from the model it represents
  // shows up as exactly that. Appends; stops at `limit` total.
  void AppendDebugBoxes(std::vector<DebugBox>& out, size_t limit,
                        uint32_t color) const;
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
  // Steering introspection. `MobHeading` is the body's actual yaw and
  // `MobDesiredHeading` the intent layer's target; a test asserts the gap
  // closes at a bounded rate rather than instantly, which is the invariant
  // that "no 90-degree snap" actually means. Comparing the two is also how the
  // debug overlay shows a mob mid-turn.
  float MobHeading(uint64_t mobId) const;
  float MobDesiredHeading(uint64_t mobId) const;
  float MobTurnVel(uint64_t mobId) const;
  // Steering override for tests and (later) scripted behaviour: sets the
  // desired heading directly, leaving the turn-rate clamp fully in force. The
  // wander behaviour re-takes control as soon as it next wants to turn.
  void SetDesiredHeading(uint64_t mobId, float radians);
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
    // The COLLIDER lattice, in `MobDef::physScale` units per world voxel —
    // int8, which is the bound physScale is chosen to satisfy. `size` is in the
    // same units. Everything positional below is in WORLD voxels.
    std::vector<DebrisVoxel> voxels;
    IVec3 size{};
    // The SKIN lattice, in `MobDef::skinScale` units per world voxel. int16,
    // and the source the brick is packed from — so this is what the player
    // actually sees, and what a carve must edit.
    //
    // Empty when skinScale == physScale: the two lattices coincide and
    // `voxels` is the whole story, exactly as before the split. Every existing
    // def stays on that path and pays nothing.
    std::vector<PrefabVoxel> skinVoxels;
    bool HasFineSkin() const { return !skinVoxels.empty(); }
    int microModel = -1;       // copy of MobLimbDef::microModel, -1 = cube path
    // What this limb's body needs to keep rendering as microvoxels once it is
    // no longer a limb — handed to DebrisSystem::AdoptBody on sever/death.
    // Takes the SKIN scale: MicroBodyRef is a render description, and the brick
    // it points at was packed on the skin lattice.
    MicroBodyRef MicroRef(uint32_t skinScale) const {
      return microModel < 0 ? MicroBodyRef{}
                            : MicroBodyRef{(uint32_t)microModel, skinScale};
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
    // ---- steering: intent vs actuation ------------------------------------
    // `heading` is where the BODY actually points and is the only thing the
    // pose, the gait and MobFacing ever read. `desiredHeading` is where the
    // mob WANTS to point. Nothing outside MobSystem::Steer may write `heading`
    // — the whole reason a mob can move at a free angle with a believable turn
    // is that the two are separate and the gap is closed at a bounded rate.
    //
    // A behaviour layer (chase, flee, patrol, strafe) is expressed purely as
    // "set desiredHeading and driveScale this tick"; it cannot teleport the
    // facing even if it wants to, so no future AI can reintroduce the snap.
    float heading = 0;         // radians about +Y, body facing (actuation)
    float desiredHeading = 0;  // radians about +Y, steering target (intent)
    float turnVel = 0;         // current yaw rate, rad/s (ramped by turnAccel)
    // What the intent layer asked for this tick, 0..1 of def.speed. Reset to
    // the default each tick by the intent step, so a behaviour that stops
    // writing it does not leave the mob sprinting forever.
    float driveScale = 1.0f;
    // Ticks the mob has been unable to make forward progress. Wander uses it
    // to widen its avoidance turn rather than re-picking the same blocked
    // direction; a mob wedged in a corner needs to escalate, not oscillate.
    uint32_t blockedTicks = 0;
    float phase = 0;           // walk cycle
    Vec3 origin{};             // mob prefab min corner, world voxels
    uint32_t lastTurnTick = 0;
    std::vector<Limb> limbs;
    AnimState anim;            // float presentation state (never hashed)
    float speedNow = 0;        // measured planar speed, voxels/sec
    Vec3 bodyUp{0, 1, 0};      // foot-plane normal (slope tilt)
    // Prefab MIN CORNER height, in world voxels — the same frame as origin.y,
    // because that is what both the Jolt submit and the IK inverse consume.
    // The gait derives it from the foot plane; it is NOT a hip height.
    float bodyY = 0;
    // Rest height of the effector (sole) above the prefab min corner, measured
    // from the rig at spawn. This is what converts the gait's hip-frame
    // `rideHeight` into the min-corner frame bodyY is expressed in: standing at
    // the authored rest pose puts the sole exactly on the foot plane.
    float restSoleY = 0;
    bool footInit = false;
  };

  void Die(Mob& mob);          // ragdoll: limbs go dynamic, adopt into debris
  void DetachLimb(Mob& mob, int limbIndex, bool adopt);

  // ---- carving internals -----------------------------------------------------
  // A carve volume, expressed once and evaluated per lattice. The factory is
  // handed a lattice scale (units per world voxel) and returns the keep-test in
  // THAT lattice's coordinates — so a limb with a fine skin carves the same
  // world-space volume out of both its skin and its collider without the
  // caller describing the shape twice. Mirrors DebrisSystem::CarveFactory.
  using LimbCarveKeep = std::function<bool(int, int, int)>;
  using LimbCarveFactory = std::function<LimbCarveKeep(float)>;
  // Shared carve core, the live-limb twin of DebrisSystem::DamageBody: erase
  // the voxels the predicate rejects, re-skin, rebuild the collider, and hand
  // any piece the carve disconnected to DebrisSystem as ordinary debris.
  // Returns false when the limb was destroyed outright (severed or the mob
  // died), in which case `mob` may no longer be alive and the caller must not
  // touch the limb again.
  bool CarveLimb(Mob& mob, int limbIndex, World& world,
                 std::vector<ParticleSpawn>& spawns, bool eject,
                 const LimbCarveFactory& carveAt);
  // Re-skin a carved limb: clone-on-first-carve, rewrite the brick from the
  // surviving voxels, shift the transform so the art stays on the collider, and
  // shift restOffset/anchorLimb with it so the RIG follows too — the difference
  // from the debris version, whose bodies answer to nobody. False = pool full
  // (limb keeps a stale skin but is really carved).
  // Re-pack a carved limb's brick. Takes BOTH lattices: the brick and the
  // origin shift are skin-side, the re-derived collider is phys-side.
  bool ReskinLimbMicro(Mob& mob, Limb& limb, uint32_t skinScale,
                       uint32_t physScale);
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
  void EmitCarvedFragment(Mob& mob, const Limb& src, uint32_t physScale,
                          std::vector<DebrisVoxel> part, World& world,
                          std::vector<ParticleSpawn>& spawns);
  void LimbVoxelsToParticles(const Limb& limb, uint32_t physScale,
                             const std::vector<DebrisVoxel>& voxels, World& world,
                             std::vector<ParticleSpawn>& spawns) const;
  bool GroundHeightAt(World& world, int wx, int wz, int yFrom, int& outY) const;

  // ---- locomotion: sense -> intent -> steer -> drive -------------------------
  // Four stages, deliberately separated so an AI layer can be dropped in at
  // exactly one of them without disturbing the others. Today's "wander and
  // avoid walls" behaviour occupies only DecideIntent; a behaviour tree
  // replaces that one function and inherits working steering and locomotion.

  // What the mob can feel about the ground around it. Probed once per tick and
  // passed down, so intent and drive cannot disagree about the terrain (they
  // used to each run their own probe) and so a future sensor — a vision cone,
  // a sound event, a nav query — has one obvious place to join.
  struct GroundSense {
    bool haveGround = false;
    int groundY = 0;           // ground under the mob's centre column
    // Probes fanned around the mob at kProbeCount evenly spaced yaws, each at
    // the mob's own step-out radius. `clear[i]` is whether a body could walk
    // that way: known footing, and no step up taller than it can climb.
    static constexpr int kProbeCount = 8;
    bool clear[kProbeCount] = {};
    int stepUp[kProbeCount] = {};   // rise at that probe, voxels (INT_MAX = unknown)
  };
  GroundSense SenseGround(const Mob& mob, const MobDef& def, World& world) const;

  // Pick this tick's desired heading and drive scale. This is THE AI seam: the
  // only stage that gets to have an opinion, and it may write nothing except
  // mob.desiredHeading / mob.driveScale. Currently a wander-and-avoid.
  void DecideIntent(Mob& mob, const MobDef& def, const GroundSense& sense,
                    uint32_t tick, float dt);

  // Close the gap between heading and desiredHeading at a bounded rate, and
  // report how well aligned the body now is (1 = facing the target, 0 = past
  // driveAlignZero) so the drive can scale forward speed by it.
  float Steer(Mob& mob, const MobDef& def, float dt);

  // Apply the resulting motion: settle onto the ground and translate along the
  // ACTUAL facing (never the desired one — that is what makes a turn arc).
  void DriveLocomotion(Mob& mob, const MobDef& def, const GroundSense& sense,
                       float align, float dt);

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
