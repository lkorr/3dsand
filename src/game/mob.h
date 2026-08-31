#pragma once
#include "sim/scale.h"  // MetresToCells / MetresToCellsI
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "game/anim.h"
#include "game/equipment.h"
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
  // ---- ball-joint (swing-twist) limits; see Physics::JointDesc ------------
  // Ball joints used to be bare point constraints with NO angular limit, and a
  // mob's limbs are deliberately excluded from colliding with each other, so
  // nothing at all stopped a corpse folding its thigh up through its pelvis.
  //
  // `boneAxis` is DERIVED at load (anchor -> the limb model's centre) rather
  // than authored: it is the same rig geometry the anchors are, and asking a
  // sidecar to restate it is asking for the two to disagree.
  Vec3 boneAxis{0, -1, 0};
  // Defaults come from the limb's `tag` (DefaultJointLimits in mob.cpp), so no
  // existing sidecar needs an edit; "cone"/"coneSide"/"twist"/"jointFriction"
  // override per limb, in radians like every other angle here.
  float coneFwd = 1.5707963f;
  float coneSide = 1.5707963f;
  float twistLimit = 1.0471976f;
  float jointFriction = 0.15f;
  // anchor override in prefab-local voxels; auto-derived from the AABB gap
  // between limb and parent when absent (anchorAuto)
  Vec3 anchor{};
  bool anchorAuto = true;
  // ---- POSE-SPACE range of motion (sidecar "poseLimit"; optional) ----------
  // THE LIMITS ABOVE DO NOT BIND AN ANIMATED LIMB. `minAngle`/`maxAngle` and
  // the swing-twist cone are handed to Jolt, and Jolt only enforces them on a
  // DYNAMIC body — a live limb is kinematic, re-posed every tick by the
  // animation pipeline, so the IK could put a thigh anywhere it liked and no
  // constraint in this struct had a word to say about it. "Legs raking out
  // behind" and "legs folded up inside the torso" were both that.
  //
  // This is the animation's own range of motion, clamped on the solved pose
  // (AnimClampPoseLimits) about the part's REST frame. Authored in DEGREES in
  // the sidecar, stored in radians here like every other angle.
  //
  // Three shapes, because three joints: a bounded one-axis clamp (the knee and
  // hip), a true one-DOF `hinge` (the elbow), and the ball form (the shoulder).
  // AnimPart carries the same fields and anim.h documents what each one is for.
  bool hasPoseLimit = false;
  Vec3 poseAxis{1, 0, 0};
  float poseMin = -3.14159265f, poseMax = 3.14159265f;
  bool poseHinge = false;
  PoseBallLimit poseBall;
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

// The one place a limb def turns into a physics joint.
//
// There are four call sites (mob spawn, mob limb rebuild, avatar spawn, and
// the joint a rebuild re-makes for each CHILD), and before the limits existed
// each of them spelled the conversion out by hand. Adding a field then meant
// finding all four; missing one meant a joint that silently kept the old,
// unlimited behaviour, which is invisible until a corpse folds in half.
inline Physics::JointDesc JointDescFor(const MobLimbDef& ld, Vec3 anchorWorld) {
  Physics::JointDesc d;
  d.type = ld.joint;
  d.anchorVoxel = anchorWorld;
  d.axis = ld.axis;
  d.minAngle = ld.minAngle;
  d.maxAngle = ld.maxAngle;
  d.boneAxis = ld.boneAxis;
  d.coneFwd = ld.coneFwd;
  d.coneSide = ld.coneSide;
  d.twist = ld.twistLimit;
  d.friction = ld.jointFriction;
  return d;
}

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
//
// AND IT IS A PHYSICAL RESOLUTION, NOT A RATIO. `physScale` counts collider
// voxels per WORLD voxel, so a fixed 4 means a different real cell size at
// every kVoxelMeters: 40 collider voxels/metre at 10 cm, 80 at 5 cm, 160 at
// 2.5 cm. Since the cost is cubic, holding the ratio would have handed the same
// physical limb 8x the boxes each time the voxel halved — a physics budget
// silently multiplied by an art-side decision, which is the exact coupling the
// skin/collider split exists to break.
//
// So the ceiling is authored as a real cell size (2.5 cm collider voxels, the
// resolution every current rig shipped at) and the ratio is derived. At 10 cm
// this is 4 and nothing moves; at 5 cm it is 2 and a limb keeps the collider it
// always had.
// Mob locomotion budgets, authored in METRES and resolved to cells.
//
// The player's equivalents have been metres-derived since v0.2
// (Player::kStepUpM, player.h:189); the mob's were bare cell counts, so at
// 5 cm voxels a mob could step up half as far as it used to and probe half
// as high for its own footing while the player was unaffected. Same world,
// two different physics.
inline constexpr int kMobStepUpCells = MetresToCellsI(0.20f);
// How far above the mob's origin a ground probe starts looking down.
inline constexpr int kMobProbeLiftCells = MetresToCellsI(0.30f);

inline constexpr int kColliderVoxelsPerMetre = 40;
inline constexpr uint32_t kMaxPhysScale = (uint32_t)(
    kColliderVoxelsPerMetre / kVoxelsPerMetre < 1
        ? 1
        : kColliderVoxelsPerMetre / kVoxelsPerMetre);

struct MobDef {
  std::string name;
  Prefab prefab;               // one model per limb
  int rootLimb = -1;           // index into limbs
  uint32_t bleedMat = 0;       // 0 = mob does not bleed
  // What the FLESH AROUND A CUT becomes — the soak, not the drip. Sidecar
  // `bleed.woundMaterial`, defaulting to `bleedMat` so no existing mob needs a
  // new key: a creature's own blood is already the right colour for meat it is
  // running out of. Separate from bleedMat because they are different
  // questions ("what pools on the floor" vs "what the wound looks like") and
  // an insect that leaks sap should be able to answer them differently.
  //
  // A MATERIAL rather than a colour, for the same reason charring is one: a
  // nonzero art slot overrides the material colour on a painted surface
  // (BurnLimbView::Set says so), so a stain carried as paint would be
  // invisible on exactly the clothed limbs it matters most on.
  uint32_t woundMat = 0;
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
  // The AUTHORING resolution of the .vox art, in art voxels per METRE, and the
  // thing that gives a grid of voxels a physical size at all. This is the field
  // sidecars declare; `skinScale` above is DERIVED from it and kVoxelsPerMetre.
  //
  // A .vox is a bare lattice: 136 voxels tall is 1.7 m only because the art is
  // 80 voxels/metre. That fact used to live nowhere — `skinScale: 8` was
  // authored directly, meaning "8 art voxels per WORLD voxel", which silently
  // encoded "and the world is 10 cm". So when kVoxelMeters went to 5 cm the
  // human stayed 17 world voxels and became 0.85 m: exactly half the height of
  // the collision capsule it drives, with no test anywhere asserting otherwise.
  //
  // Declaring the metre scale instead makes the derivation automatic and the
  // art file untouched: human at 80 art vox/m is skinScale 8 at 10 cm, 4 at
  // 5 cm, 2 at 2.5 cm — same 1.7 m every time, and the world-cell model simply
  // gets finer. Re-authoring the art at a higher resolution later is then a
  // pure data change: bump this number, change no code.
  int artVoxelsPerMetre = 0;
  // Block replication applied to the art grid at load, when the art is COARSER
  // than the world (`artVoxelsPerMetre < kVoxelsPerMetre`) and no integer
  // skinScale could exist. Exact and lossless — it preserves world size and
  // adds no detail — so it is always safe in this direction.
  //
  // Authored offsets (anchors, sockets, cutting edges, clip keys) are in the
  // ORIGINAL art grid and are NOT rescaled by it; they convert through
  // ArtToWorld() below, which divides that out. Keeping them in the authored
  // frame is what lets the upsample stay invisible to every sidecar.
  uint32_t artUpsample = 1;
  // Authored art units -> world voxels. The ONE conversion for every length a
  // sidecar states in the art's lattice.
  //
  // Not simply `1/skinScale`: after an upsample the voxel grid is `artUpsample`
  // times finer than the numbers in the JSON, and skinScale describes the
  // GRID. This divides that back out, so `anchor * ArtToWorld()` is correct
  // whether or not the art was replicated.
  float ArtToWorld() const {
    return (float)artUpsample / (float)(skinScale ? skinScale : 1u);
  }
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

// ---- per-voxel body reactivity (docs/PLAN_body_reactivity.md) --------------
//
// A creature is not "on fire": individual voxels of it are. The STATE that
// takes lives here rather than on MobSystem::Limb, because a limb is not the
// only thing that burns — PlayerAvatar::Part is the same thing under a
// different driver (the avatar IS a MobDef; see avatar.h), and a dropped item
// and a corpse are DebrisSystem bodies. The player must burn exactly as an NPC
// does, and the only way to be sure of that is for there to be one
// implementation, not two that happen to agree.
//
// So: the state is here, and the PASS is MobSystem::BurnOneLimb, which takes a
// BurnLimbView over whatever struct the caller owns.
struct BodyBurnState {
  // Dense neighbour index over the limb's bounding box: entry = lattice index
  // + 1, 0 = empty. Allocated the first time something reactive comes near and
  // released when the front goes cold, because rule 2 applies to a new
  // population exactly as it does to chunks — a creature that is not burning
  // must cost zero, and one that is burning must cost in proportion to how much
  // of it is alight, never to how many voxels it has.
  //
  // DERIVED and disposable; the sparse lattice stays authoritative. Reusing the
  // GPU brick as this index is tempting (it IS dense) and is wrong: the brick
  // is render-only derived data, and reading it back for sim purposes is the
  // first step toward the unowned-diverging-representation failure.
  IVec3 min{}, dims{};
  std::vector<uint32_t> idx;
  // Cells whose material carries a decay/emit rule — the voxels actually
  // alight. Spread is pushed OUTWARD from these to their six lattice
  // neighbours, never pulled by scanning candidates, which is what keeps the
  // cost on the front instead of on the volume. Fire lives on a SURFACE, so
  // this is a 2D front over a 3D body.
  std::vector<uint32_t> front;
  // Voxels burnt away since the last collider rebuild. That rebuild is the most
  // expensive single operation in the feature, so it is batched hard.
  uint32_t removed = 0;
  // Consecutive ticks with an empty front, so a body walking in and out of a
  // campfire does not rebuild its index every other tick.
  uint32_t quiet = 0;
  // "This limb still carries burning matter", and the ONE piece of burn state
  // that SURVIVES DropBurnIndex. `front` cannot: its entries are cells of the
  // index box that was just thrown away. Every carve drops the index (the
  // lattice compacted underneath it) and burning carves constantly, so without
  // this the cheap gate at the top of BurnOneLimb — "nothing hot nearby and an
  // empty front, so exit" — fired on the tick after the last flush and the
  // limb's flesh_burning voxels never rolled their decay again. That is a
  // character left permanently coated in flame that has nothing left to burn.
  bool alight = false;
  bool Burning() const { return !front.empty() || alight; }
};

// One limb or part, described in the terms the burn pass needs.
//
// Pointers rather than a base class: MobSystem::Limb and PlayerAvatar::Part are
// independent structs owned by different systems and neither is going to grow a
// vtable for this. Exactly one of `skin`/`coll` is the AUTHORITATIVE lattice —
// a body with a finer skin derives its collider from the skin by majority-fill,
// so burning the collider would be writing to derived data and the next
// re-derive would silently undo it.
struct BurnLimbView {
  std::vector<PrefabVoxel>* skin = nullptr;  // skinScale units, int16
  std::vector<DebrisVoxel>* coll = nullptr;  // physScale units, int8
  uint32_t scale = 1;                        // lattice units per world voxel
  const BodyTransform* xf = nullptr;         // pose as the ANIMATION left it
  IVec3 size{};                              // collider extents, physScale units
  uint32_t physScale = 1;
  int* microModel = nullptr;   // null / -1 = cube path, no brick to poke
  bool* carved = nullptr;      // latched when the brick becomes copy-on-write
  int* flipbook = nullptr;     // cleared on first damage; a frame swap heals
  BodyBurnState* burn = nullptr;

  // ---- IS SOMETHING WORN IN THE WAY? --------------------------------------
  //
  // The burn pass reads the WORLD around a limb — to seed fire from contact,
  // and to let a world neighbour's rule (acid) rewrite the limb. Neither
  // knows about armour: a worn shell's voxels are in neither the grid nor
  // this limb's lattice, so fire lapping at a sleeve reads to the arm
  // underneath exactly as fire lapping at the arm.
  //
  // This closes that, and it is the ONE genuinely new mechanic armour needed.
  // It returns the occluding shell's MATERIAL rather than a bool, which is
  // what keeps the behaviour emergent: the flesh's neighbour simply becomes
  // "cloth" instead of "fire", cloth-over-flesh semantics fall out of the
  // ordinary authored table, and when the shell burns through the probe
  // returns 0 there and the skin is exposed. No integrity threshold, no
  // resist value, nothing to tune.
  //
  // A SEGMENT, NOT A POINT, and that distinction is the whole difference
  // between this working and appearing to.
  //
  // The obvious test — "is the cell one lattice step outside this voxel inside
  // a shell?" — reads correct and leaks completely, because a limb is a
  // ROUNDED TUBE inside a garment cut to its box. On any diagonal there are
  // several empty cells between the flesh and the cloth, so the point one step
  // out lands in the gap, the probe says "nothing there", and fire walks
  // straight through intact armour. Measured: a fully enclosed arm caught fire
  // three ticks after the bare one beside it.
  //
  // So the question is asked as a RAY: is there anything worn between me and
  // the world, within `dist` world voxels along `dir`. Marching costs one
  // transform and a handful of array reads, because a shell rides its limb and
  // therefore shares its rotation — the direction in the shell's lattice is
  // fixed for the whole march.
  //
  // A raw function pointer and a context, NOT a std::function: this view is
  // built per limb per tick, and a capturing std::function would heap-allocate
  // once per limb per tick for a feature most creatures do not use. Null on
  // anything that cannot be wearing armour (debris, a corpse).
  using OccludeFn = uint32_t (*)(void* ctx, const Vec3& from, const Vec3& dir,
                                 float dist);
  OccludeFn occlude = nullptr;
  void* occludeCtx = nullptr;
  // dist 0 is a point test at `from`.
  uint32_t WornAlong(const Vec3& from, const Vec3& dir, float dist) const {
    return occlude ? occlude(occludeCtx, from, dir, dist) : 0u;
  }

  size_t Size() const { return skin ? skin->size() : coll->size(); }
  IVec3 At(size_t i) const {
    return skin ? IVec3{(*skin)[i].x, (*skin)[i].y, (*skin)[i].z}
                : IVec3{(*coll)[i].x, (*coll)[i].y, (*coll)[i].z};
  }
  uint32_t Mat(size_t i) const {
    return skin ? (uint32_t)((*skin)[i].material & 0xFFFu)
                : (uint32_t)((*coll)[i].payload & 0xFFFu);
  }
  // Rewrite a voxel's material, keeping a cosmetic variant and ZEROING the art
  // slot. The art zero is not tidiness: microbody.wgsl lets a nonzero art
  // colour override the material colour (a creature is one material all over
  // and painted per voxel), so a charred voxel that kept its slot goes on being
  // painted robe-purple — charring would be invisible on exactly the painted
  // surfaces it matters most on.
  void Set(size_t i, uint32_t mat, uint32_t variant) const {
    const uint16_t w = (uint16_t)((mat & 0xFFFu) | ((variant & 3u) << 12));
    if (skin) {
      (*skin)[i].material = w;
      (*skin)[i].color = 0;
    } else {
      (*coll)[i].payload = w;
      (*coll)[i].color = 0;
    }
  }
};

// Loads assets/mobs/*.vox + matching .json sidecars. Appends problems to log;
// defs that fail validation are skipped. Limb models of defs with "skinScale" > 1
// are packed into `micro` (which the caller uploads); `micro` is CLEARED first,
// so a hot reload rebuilds the whole pool rather than growing it forever.
bool LoadMobDefs(const std::string& dir, const std::vector<MaterialDef>& mats,
                 std::vector<MobDef>& out, MicroBodySet& micro, std::string& log);

// Per-creature gore profile: the entity-scoped variance draws, resolved ONCE
// when the creature is created and then held for its whole life — one NPC can
// be a heavy bleeder from spawn to corpse while its neighbour bleeds normally,
// and the PLAYER draws one too (the avatar is a Mob; see class Mob below).
// Values are absolute (already multiplied by the whole-wound gain).
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

// One rig part of a live creature — a limb, or a borrowed item slot. ONE
// struct for mobs and the avatar: it used to be MobSystem::Limb and
// PlayerAvatar::Part, two runtime spellings of the same idea that had already
// drifted (the avatar's copy lacked flipbooks; its `burnOwnsBrick` was
// `carved` under another name). Everything positional is in WORLD voxels;
// `voxels` is the COLLIDER lattice (physScale units, int8), `skinVoxels` the
// SKIN lattice (skinScale units, int16, empty when the two coincide).
struct MobLimb {
  uint64_t body = 0;         // 0 = severed or never spawned
  uint64_t joint = 0;        // to parent
  float hp = 0;
  std::vector<DebrisVoxel> voxels;
  IVec3 size{};
  std::vector<PrefabVoxel> skinVoxels;
  bool HasFineSkin() const { return !skinVoxels.empty(); }
  int microModel = -1;       // -1 = cube path
  // ---- LATTICE PITCH FOR A LIMB THAT IS NOT THE CREATURE'S OWN ART --------
  // A body limb's voxels are authored in the def's skin lattice, so the def's
  // skinScale/physScale describe them and there is nothing to store. A worn
  // shell and a held item are not: they come out of an ITEM's .vox at the
  // ITEM's scale, which need not be the wearer's — a scale-4 sword on a
  // skinScale-8 human. Every scale-taking operation on a limb (burn, carve,
  // re-skin, collider rebuild, drop-to-debris) has to use the lattice the
  // voxels are ACTUALLY on, or it converts the geometry by the wrong divisor
  // and the wound lands somewhere else on the item than where it was struck.
  //
  // 0 means "the def's", which is every authored body limb. Read through
  // Mob::SkinScaleOf / PhysScaleOf, never directly.
  uint32_t ownSkinScale = 0;
  uint32_t ownPhysScale = 0;
  // Takes the SKIN scale: a MicroBodyRef is a render description.
  MicroBodyRef MicroRef(uint32_t skinScale) const {
    return microModel < 0 ? MicroBodyRef{}
                          : MicroBodyRef{(uint32_t)microModel, skinScale};
  }
  Vec3 restOffset{};         // limb min corner from creature min corner (rest)
  Vec3 anchorRoot{};         // joint anchor from creature min corner (rest)
  Vec3 anchorLimb{};         // joint anchor in limb-local coords
  BodyTransform xf{};
  float bleedBudget = 0;
  Vec3 woundLocal{};
  // Dismemberment gout: counts DOWN from gore.severDecayTicks, emission
  // proportional to it, so the burst is front-loaded and tails off. Lives on
  // the PARENT limb (the stump), not on the piece that came off.
  int gushTicks = 0;
  Vec3 gushLocal{};
  Vec3 gushDir{0, 1, 0};
  // A severed part is handed to DebrisSystem immediately but holds its last
  // animated pose KINEMATICALLY for a beat before flipping dynamic.
  uint64_t holdBody = 0;
  float holdSeconds = 0;
  // Flipbook: >=0 selects an alternate .vox model's voxels for RENDERING only.
  int flipbookModel = -1;
  std::vector<std::vector<DebrisVoxel>> frameVoxels;
  // Latched the first time this limb loses a voxel (carve OR burn): its micro
  // model is a copy-on-write clone this limb OWNS and must free
  // (ReleaseLimbMicro), and its flipbooks are disabled.
  bool carved = false;
  // Voxel count the limb was authored with, so damage is a FRACTION of it.
  uint32_t voxelsAtSpawn = 0;
  // Voxel count the last carve already CHARGED to hp. `voxelsAtSpawn` is the
  // denominator of the fraction; this is the previous numerator, and without it
  // every carve re-charges everything the limb has ever lost — see the
  // incremental-loss note in Mob::CarveLimb.
  uint32_t voxelsCharged = 0;
  // ---- HOW MUCH FLESH THIS LIMB HAS AT ITS JOINT ---------------------------
  // Voxel count inside a sphere of gore.woundNeckRadius around `anchorLimb`,
  // on whichever lattice is authoritative. It is what "hanging by a thread"
  // is measured against (Mob::CarveLimb): a limb can be 60% intact overall and
  // still be attached by four voxels, and no whole-limb fraction can see that.
  //
  // 0 = NOT YET TAKEN, and it is taken lazily at the top of the first carve
  // rather than at spawn. Two reasons: spawn happens in three places (BuildRig,
  // the save load overlay, the worn-shell append) and a count taken in only two
  // of them is a silent zero; and the count costs a pass over the lattice,
  // which a creature that is never cut should not pay. The cost of laziness is
  // that a limb first cut when it is already half burnt records the burnt
  // count — which is CONSERVATIVE (a lower denominator makes the sever harder,
  // never easier), so it fails safe.
  uint32_t neckAtSpawn = 0;
  // Per-voxel burning / dissolution (see BodyBurnState above).
  BodyBurnState burn;
};

// ---- ONE BLADE HIT, AS GEOMETRY --------------------------------------------
//
// The argument to Mob::CutLimb, and the whole of what a sword knows about a
// wound. Everything is in WORLD VOXELS and world directions; the limb converts
// into its own frame and into whichever lattice it is testing.
//
// WHY A KERF AND NOT A SPHERE. A blast is a point with a radius, and the
// radial carve (CarveLimbRadial) is the right shape for it. An edge is not: it
// enters at a point, travels in the direction the swing is going, and cuts a
// SLOT the length of the part of the blade that made contact. Carving a sphere
// for it is what made a sword's touch remove a spherical bite out of an arm —
// which, at any radius large enough to feel like a sword, is most of the arm.
//
// The three axes are independent and all three are measured rather than tuned:
// `edgeAxis` comes from the item's authored edge segment through its live pose,
// `cutDir` from how far the tip actually travelled this tick, and `halfWidth`
// from the blade's own authored thickness. Only the DEPTH is a tuning
// question, because only the depth depends on how hard you swung.
struct BladeCut {
  Vec3 at{};          // contact point, world voxels
  Vec3 edgeAxis{};    // unit, along the blade's edge (the slot's long axis)
  Vec3 cutDir{};      // unit, the direction the edge is travelling
  float halfWidth = 0.25f;  // kerf half-thickness, world voxels
  float depth = 0.5f;       // how far in the edge bit, world voxels
  float length = 1.0f;      // half-length of the slot along the edge
  float power = 1.0f;       // 0..1 swing commitment, for the audio severity
  uint32_t seed = 0;        // ragged-rim / stain draw key; see the note in
                            // Mob::CutLimb about why this is not a tick
};

class MobSystem;
struct ItemDef;
struct ItemCover;

// ============================================================================
// ONE CREATURE. The base class of every articulated body in the game: NPCs
// are plain Mobs driven by MobSystem's AI stages, and the PLAYER AVATAR is a
// subclass driven by player input (game/avatar.h).
//
// THE RULE (the reason this class exists): every body MECHANIC — damage,
// severing, dying, per-voxel carving, burning/dissolving, bleeding, item
// holding, rendering — has exactly ONE implementation, here. Anything that
// applies to a mob applies to the player by default; the avatar's differences
// are EXPLICIT overrides of the small virtual seam below, not parallel copies.
// Two implementations that agree today are two that disagree after the next
// tuning change — that is how the avatar's old copy of this code rotted.
//
// What is NOT here is the DRIVER: who decides where the body goes. MobSystem
// senses/steers/drives NPCs and owns their lifecycle (spawn caps, despawn,
// husk removal); the avatar follows the Player. One schema, one mechanics
// implementation, two drivers.
//
// DETERMINISM (CLAUDE.md rule 1): everything here is CPU-float presentation
// state, never hashed. Grid contact — blood, fire, gore particles — travels
// through the BrushOp/CellOp/ParticleSpawn streams like every other mutation,
// and every RNG draw that reaches those streams is counter-based (id, tick,
// index), never keyed on a Jolt float.
class Mob {
 public:
  Mob() = default;
  virtual ~Mob() = default;
  Mob(Mob&&) = default;
  Mob& operator=(Mob&&) = default;
  Mob(const Mob&) = delete;
  Mob& operator=(const Mob&) = delete;

  uint64_t Id() const { return id_; }
  bool Alive() const { return alive_; }
  const MobDef* Def() const { return def_; }
  Vec3 Origin() const { return origin_; }
  float BodyY() const { return bodyY_; }
  // Rig size INCLUDING a borrowed item slot (limbDefs_ tracks limbs_).
  int LimbCount() const { return (int)limbDefs_.size(); }
  const MobLimbDef& LimbDefAt(int i) const { return limbDefs_[i]; }

  // ---- damage / dismemberment (shared; see MobSystem for the id-keyed API) --
  // Damage a limb by physics body handle. Returns true if the handle belonged
  // to one of this creature's limbs.
  //
  // WHAT IT NO LONGER DOES: sever. Three thresholds used to fire a Sever()
  // from here — a hit within 1.75 voxels of a joint anchor, a hit past the
  // limb's severImpactSpeed, and hp reaching zero — and between them a sword
  // took a limb off on FIRST CONTACT, anywhere, every time. Dismemberment is
  // now structural (see Mob::CutLimb and the connectivity block in
  // Mob::CarveLimb): a limb comes off when its lattice has been cut through.
  //
  // What survives here: hp still falls, the wound is still recorded, the
  // flinch and the hurt cry still fire, and hp reaching zero on a VITAL or
  // ROOT limb still kills the creature — death is a consequence of damage, a
  // missing arm is a consequence of geometry. `severImpactSpeed` survives as
  // an extreme-speed exception, scaled by gore.woundImpactSeverScale so an
  // ordinary swing cannot reach it.
  bool Damage(uint64_t bodyHandle, float amount, Vec3 hitWorldVoxel,
              float impactSpeed = 0.0f);
  // ---- THE BLADE PATH ------------------------------------------------------
  // Cut a live limb along the swept edge: a narrow slot, not a spherical bite.
  // Removes voxels, ejects them as gore, soaks the exposed flesh in the
  // creature's wound material, and — when the lattice has genuinely parted —
  // routes the dismemberment through the ordinary Sever(). Returns true when
  // the handle was one of this creature's live limbs.
  bool CutLimb(uint64_t bodyHandle, const BladeCut& cut, World& world,
               std::vector<ParticleSpawn>& spawns);
  // Detach a limb now. Root/vital kills instead.
  void Sever(int limbIndex);
  void Die();
  // Per-voxel carving: remove real voxels from a live limb (docs/DESIGN.md §7).
  bool CarveLimbRadial(uint64_t bodyHandle, Vec3 centerWorldVoxel,
                       float radiusVoxels, bool ragged, bool eject, World& world,
                       std::vector<ParticleSpawn>& spawns);
  // Every live limb of THIS creature within the blast — the explosion path.
  void CarveRadialAll(Vec3 centerWorldVoxel, float radiusVoxels, World& world,
                      std::vector<ParticleSpawn>& spawns);

  // ---- per-voxel burning ----------------------------------------------------
  // Set fire to up to `count` of a limb's surface voxels; returns how many took.
  uint32_t Ignite(int limbIndex, uint32_t count, uint32_t onlyMat = 0);
  // One tick of burning across this creature's limbs. Budgets are in/out and
  // may be shared across creatures (MobSystem) or private (the avatar).
  void BurnTick(uint32_t tick, World& world, std::vector<CellOp>& cellOps,
                std::vector<ParticleSpawn>& spawns, uint32_t& frontBudget,
                uint32_t& opsBudget);

  // ---- per-tick body upkeep (called by the driver) --------------------------
  void TickSeveredHolds(float dt);
  void DrainPendingSpawns(World& world, std::vector<ParticleSpawn>& spawns);
  // Bleeding: decaying wound budgets, dismemberment gouts, bounded ops.
  // `bleedOps` is the shared per-tick drip budget counter.
  void BleedTick(uint32_t tick, World& world, std::vector<BrushOp>& ops,
                 std::vector<ParticleSpawn>& spawns, int& bleedOps);
  // Model-space pose -> world, submit kinematic targets to Jolt. `writeXf`
  // also stores the submitted pose into limb.xf immediately — the avatar needs
  // that (its held-item placement reads the hand's fresh pose this tick); the
  // NPC path deliberately keeps xf as PostStep left it so its bleed positions
  // are unchanged by the refactor.
  void SubmitPose(float dt, bool writeXf);
  void PostStep();
  // Keeps the chunks around the body fetched+refreshed in the CPU mirror; the
  // burn pass reads that mirror to find out whether it is standing in a fire.
  void RegisterTerrainAnchor();
  void PlayClip(const std::string& name);
  void PlayClipIndex(int ci);

  // ---- holding an item (THE ENTITY<->SLOT SYNC SEAM; see game/avatar.h) ----
  // Equipping BORROWS A RIG SLOT: the item's geometry fills a real MobLimb
  // parented to the socket's limb — animated, severable, droppable, carvable,
  // with no "is this an item" branch downstream. Lives on the BASE class so a
  // mob can hold a sword exactly as the player does (mob combat scaffolding).
  bool EquipItem(const ItemDef* item, const char* context = "held_right");
  const std::string& HeldItem() const { return heldItem_; }
  int HeldSlot() const { return heldSlot_; }

  // ---- WEARING an item (the same borrowed slot, N times) ------------------
  // A worn piece appends one rig slot per ItemCover entry — a SHELL: parented
  // to the covered limb by a fixed joint, tagged "worn", not vital, with its
  // own hp, its own voxels and its own micro brick. Everything the held-item
  // path already earns is inherited per shell: burning, dissolving, per-voxel
  // carving, severing with the limb it is strapped to, dropping as debris,
  // live-transform hitboxes, rendering.
  //
  // On the BASE class for the same reason EquipItem is: a goblin in a helmet
  // is one WearItem call, and the player wears exactly the same way.
  //
  // `equipSlot` is an EquipSlotId as an int — the key the piece is later
  // removed by, and the only thing tying a shell group back to the character
  // screen. Pass a null item to UnwearItem's slot instead of here.
  // `damage` puts a piece back on AS IT WAS. Null wears it as authored, which
  // is the common case; passing a blob captured by CaptureWorn is what makes
  // taking your boots off and putting them back on stop being a repair
  // (game/equipment.h WornDamage).
  bool WearItem(const ItemDef* item, int equipSlot,
                const WornDamage* damage = nullptr);
  bool UnwearItem(int equipSlot);
  // Read what a worn piece has been through, in its item's cover order. Call
  // it BEFORE UnwearItem: the shells are the only place the damage lives while
  // the piece is on, and they are destroyed with the slots.
  bool CaptureWorn(int equipSlot, WornDamage& out) const;
  // The item name worn in a slot, or empty. By NAME because library indices
  // are file-order and die on an R reload (item.h's index hazard).
  const std::string& WornItem(int equipSlot) const;
  int WornPieceCount() const { return (int)worn_.size(); }
  // HOW MUCH OF THE PIECE IS STILL THERE, 0..1, summed over its shells and
  // weighted by their volume — one hole in a pauldron must not read the same as
  // a robe burnt to rags. 1 for a slot wearing nothing, so a caller that does
  // not check can only be told "whole".
  //
  // Voxels, not hp, because voxels are what the armour mechanic is: protection
  // here is a shell being geometrically IN THE WAY, so the fraction of it that
  // is still in the way is the honest measure of condition. hp is a rig
  // durability number that also falls to blunt trauma and is not what the
  // occlusion probe reads.
  //
  // LIVE — off the shells themselves. The blob in PlayerKit::wornDamage is only
  // written when a piece comes OFF, so asking that while wearing it reports the
  // condition it was in the last time it was taken off.
  float WornCondition(int equipSlot) const;
  // Rig slots this piece occupies, for tests and for the occlusion probe.
  const std::vector<int>& WornSlotsAt(int pieceIndex) const;
  // Is `worldPos` inside a live voxel of any shell worn over `bodyLimb`?
  // Reports the occluding shell's MATERIAL (0 = not occluded), because the
  // burn pass does not want a bool — it wants to know what the flesh's
  // neighbour actually is once a robe is in the way (see the call sites in
  // BurnOneLimb).
  //
  // NON-CONST because it builds the per-shell occupancy index on first use;
  // the index is derived data and the answer is a pure function of the
  // lattice, so this is a cache fill, not a mutation of anything observable.
  uint32_t WornOccludes(int bodyLimb, const Vec3& worldPos) {
    return WornAlong(bodyLimb, worldPos, Vec3{0, 1, 0}, 0.0f);
  }
  // The same question asked along a SEGMENT: the first shell material met
  // between `from` and `from + dir * dist`, stepping one shell-lattice cell at
  // a time. `dist` 0 degenerates to the point test above.
  //
  // A ray rather than a point because a limb is a rounded tube inside a
  // garment cut to its box — see the long note on BurnLimbView::occlude for
  // what testing the point costs.
  uint32_t WornAlong(int bodyLimb, const Vec3& from, const Vec3& dir,
                     float dist);
  // Does any shell cover this body limb at all? The cheap gate in front of the
  // probe, so an undressed creature never even transforms a point.
  bool LimbHasShells(int bodyLimb) const;
  // Rig slots that are NOT part of the authored rig: worn shells and a held
  // item, always at the tail. `LimbCount() - AppendedBase()` of them.
  int AppendedBase() const { return baseLimbs_; }
  // The held weapon's cutting edge in WORLD voxels, from its live transform.
  bool WeaponEdge(Vec3& outBase, Vec3& outTip, float& outHalfWidth) const;
  // Is this Jolt body one of this creature's own parts?
  bool OwnsBody(uint64_t bodyHandle) const;
  // Swing pose, pushed in by the driver. Presentation only; consumed by the
  // driver's own animation pass.
  void SetWeaponPose(Vec3 handOffset, Vec3 bladeDir, Vec3 bladeUp, float weight);
  // The INVERSE of SetWeaponPose's hand offset: where the weapon arm's hand is
  // RIGHT NOW, shoulder-relative, in the same frame that call speaks, plus the
  // arm's own reach (its two bone lengths). False when there is no weapon arm
  // to read — no item held, no "arm" chain ending at that hand, or the pose has
  // not been flattened yet. The point of it is that a driver can take control
  // of the arm from wherever the animation had it instead of snapping it to a
  // pose of its own (game/melee.h).
  bool WeaponArmPose(Vec3& outHandFromShoulder, float& outReach) const;

  // ---- render plumbing (per creature; MobSystem chains these over its list) -
  // The Append* walks MUST visit slots in the same order: the slot a transform
  // lands in is the slot the instance records. Each returns the next slot.
  uint32_t AppendInstances(std::vector<BodyVoxInst>& out, uint32_t slotBase);
  void AppendXforms(std::vector<BodyXformGpu>& out) const;
  uint32_t AppendMicroInsts(std::vector<MicroBodyInstGpu>& out,
                            uint32_t slotBase) const;
  void AppendDebugBoxes(std::vector<DebugBox>& out, size_t limit,
                        uint32_t color) const;
  uint32_t LimbBodyCount() const;

  bool GroundHeightAt(World& world, int wx, int wz, int yFrom, int& outY,
                      uint32_t* outMat = nullptr) const;

  // Release a body's burn index and front (lattice compacted / rig torn down).
  static void DropBurnIndex(BodyBurnState& st);
  // Draw the entity-scoped gore variance for one creature id.
  static GoreProfile MakeGoreProfile(uint64_t id);

 protected:
  // ---- THE EXPLICIT-EXCEPTION SEAM ------------------------------------------
  // Everything the avatar does differently from an NPC goes through one of
  // these. Adding avatar behaviour anywhere else in the shared mechanics is
  // the bug this class was built to make impossible.
  //
  // Limbs of the player's body live on the AVATAR physics layer (they sit
  // inside the player capsule and must not push it — see avatar.cpp Spawn).
  virtual bool AvatarLayer() const { return false; }
  // A body leaving this rig for the world (severed-hold release, death
  // ragdoll). The avatar strips its avatar-layer exemption here.
  virtual void OnBodyReleasedToWorld(uint64_t /*bodyHandle*/) {}
  // NPC husks drop their limb list at death (PreTick reaps them); the avatar
  // keeps it so the HUD's per-part readout survives the death screen.
  virtual bool DropLimbListOnDeath() const { return true; }
  // Whose instance list went stale: MobSystem's shared one, or the avatar's.
  virtual void MarkInstancesDirty();
  // Per-limb render suppression (first-person hides the body, keeps the arms).
  bool LimbHidden(int i) const {
    return i >= 0 && i < (int)hidden_.size() && hidden_[i] != 0;
  }
  bool LimbAlive(int i) const {
    return i >= 0 && i < (int)anim_.partAlive.size() && anim_.partAlive[i] != 0;
  }
  // THE LATTICE A LIMB'S VOXELS ARE ACTUALLY ON (see MobLimb::ownSkinScale).
  // Every limb-scoped scale conversion goes through these two rather than
  // reading def_->skinScale directly, so a shell or a weapon authored at its
  // own resolution is converted by its own divisor.
  uint32_t SkinScaleOf(const MobLimb& l) const {
    return l.ownSkinScale ? l.ownSkinScale : (def_ ? def_->skinScale : 1u);
  }
  uint32_t PhysScaleOf(const MobLimb& l) const {
    return l.ownPhysScale ? l.ownPhysScale : (def_ ? def_->physScale : 1u);
  }

  // Build limbs/bodies/joints/anim state from the def at `origin` (min corner,
  // world voxels). Seeds the per-instance rig copy (skel_/limbDefs_). False =
  // physics refused a body; everything created so far is torn down.
  bool BuildRig(const MobDef& def, Vec3 origin);

  // ---- carving internals (docs/DESIGN.md §7) --------------------------------
  using LimbCarveKeep = std::function<bool(int, int, int)>;
  using LimbCarveFactory = std::function<LimbCarveKeep(float)>;
  // ---- SPALL: let a hole grow into its own rim -----------------------------
  // A predicate can only ask "should this voxel go", one voxel at a time, with
  // no idea what is left around it. "Take more where matter is already missing"
  // is a question about OCCUPANCY, and CarveLimb is the only place that knows
  // it — it owns the limb's authoritative voxel list. So the growth lives here
  // rather than in the crater predicate.
  //
  // Optional by construction: only the radial blast path fills one in, so the
  // burn flush and the laser's clean kerf are untouched without either of them
  // having to opt out.
  struct CarveSpall {
    Vec3 centerLocal{};   // blast centre in the limb's BODY frame, world voxels
    float radius = 0;     // world voxels
    float strength = 0;   // 0..1; 0 disables
    int rounds = 0;       // passes; each can only remove
    uint32_t seed = 0;    // same (mob, limb) key the crater noise uses
  };
  bool CarveLimb(int limbIndex, World& world,
                 std::vector<ParticleSpawn>& spawns, bool eject,
                 const LimbCarveFactory& carveAt,
                 const CarveSpall* spall = nullptr);
  bool ReskinLimbMicro(MobLimb& limb, uint32_t skinScale, uint32_t physScale);
  bool RebuildLimbBody(int limbIndex);
  // ---- the wound model's two helpers (game/mob.cpp, and the notes there) ----
  // Voxels of `limb` within `radiusWorld` world voxels of its joint anchor, on
  // whichever lattice is authoritative. ONE pass, no allocation. This is the
  // measure MobLimb::neckAtSpawn records and the neck sever compares against.
  uint32_t NeckCount(const MobLimb& limb, float radiusWorld) const;
  // Soak the flesh around a cut. Rewrites the MATERIAL of a hash-selected
  // fraction of the voxels within `radiusWorld` of `centreLocal` (limb-local
  // world voxels) to the creature's wound material, and pokes the micro brick
  // so it is visible without a full re-skin. Returns how many took the stain.
  //
  // Bounded by ONE pass over the limb's authoritative lattice, on hit ticks
  // only — the same bound the spall pass carries, and for the same reason: the
  // question is about occupancy, so only the owner of the voxel list can ask
  // it. Refuses worn slots and item slots outright (a garment has no blood in
  // it — see IsWornSlot).
  uint32_t StainWound(int limbIndex, Vec3 centreLocal, float radiusWorld,
                      uint32_t seed);
  // Does hp reaching zero take this limb OFF, or merely kill the creature?
  // See the note at the call sites: geometry dismembers, damage kills.
  bool HpZeroSevers(int limbIndex) const;
  void EmitCarvedFragment(const MobLimb& src, uint32_t physScale,
                          std::vector<DebrisVoxel> part, World& world,
                          std::vector<ParticleSpawn>& spawns);
  void LimbVoxelsToParticles(const MobLimb& limb, uint32_t physScale,
                             const std::vector<DebrisVoxel>& voxels, World& world,
                             std::vector<ParticleSpawn>& spawns) const;
  void ReleaseLimbMicro(MobLimb& limb);
  void DetachLimb(int limbIndex, bool adopt);
  // Tear down every body/joint/brick this rig still owns (despawn, reset).
  void ReleaseRig();

  // ---- the APPENDED TAIL: shells and the held item -------------------------
  //
  // THE REMOVAL SEAM, and the one invariant everything about wearing rests on.
  //
  // Before armour there was exactly one appended slot and unequipping it was
  // `pop_back` on four parallel vectors — index-parallel by construction,
  // nothing to renumber. Several worn pieces plus a weapon break that: taking
  // off the robe while the boots and the sword stay on removes a group from
  // the MIDDLE of the tail.
  //
  // What this does is ERASE that range and FIX UP the indices that referred
  // past it, rather than tearing the whole tail down and re-appending the
  // survivors. Two reasons, and the second is the one that matters:
  //
  //   * An appended slot is never a PARENT. Shells hang off body limbs and a
  //     held item off a hand, both of which live below baseLimbs_, so erasing
  //     one can orphan nothing and no parent index inside the base rig moves.
  //     Only the appended indices after the hole shift, and there are exactly
  //     three kinds of reference to them: heldSlot_/heldPartIndex_ and each
  //     WornPiece::slots. All three are fixed here.
  //   * A survivor's LATTICE IS NOT REBUILT, because it is never let go of. A
  //     re-append would have to carry the carved voxels, the owned brick index
  //     and the current hp across by hand, and re-loading any one of them from
  //     the def would silently HEAL a damaged pauldron — a bug that looks like
  //     nothing until somebody notices their armour repairing itself when they
  //     take off an unrelated piece. Moving the MobLimb wholesale makes that
  //     unrepresentable instead of merely tested for.
  //
  // Destroys the joints and bodies of the removed range only. `count` slots
  // starting at `first`, which must be a range wholly inside the tail.
  void RemoveAppendedSlots(int first, int count);
  // Append one shell of a worn piece over `bodyLimb`. Returns the new slot, or
  // -1. Split out of WearItem so the per-cover loop reads as a list of shells
  // rather than as one 200-line function.
  int AppendWornShell(const ItemDef& item, const ItemCover& cover,
                      int bodyLimb, const std::string& partName);
  // Replace a shell's authoritative lattice with a saved one and re-derive
  // everything downstream of it (collider, brick, Jolt body). The same three
  // steps MobSystem::LoadState takes for a carved limb, and for the same
  // reason: the lattice is the truth and the rest is derived from it.
  void RestoreShellLattice(int slot, const WornShellDamage& d);
  // Every appended vector is the same length, and several loops assume it.
  // Cheap enough to assert after every structural change.
  bool AppendedInvariantHolds() const;
  // Binds Mob::WornOccludes to BurnLimbView's plain function-pointer hook.
  // A struct rather than a lambda because the hook must be a raw pointer (see
  // BurnLimbView::occlude) and the context has to outlive the call.
  struct WornProbe {
    Mob* mob;
    int limb;
    static uint32_t Call(void* ctx, const Vec3& from, const Vec3& dir,
                         float dist) {
      WornProbe* s = static_cast<WornProbe*>(ctx);
      return s->mob->WornAlong(s->limb, from, dir, dist);
    }
  };

  // ---- burn internals -------------------------------------------------------
  BurnLimbView ViewOf(MobLimb& limb);
  bool FlushBurn(int limbIndex, World& world,
                 std::vector<ParticleSpawn>& spawns, bool force);
  void StripBurnTombstones(MobLimb& limb);

  // Shared services, borrowed from MobSystem (burn tables, micro pool,
  // material tables, event sinks). Never null on a spawned creature.
  MicroBodySet* MicroSet() const;
  const std::vector<float>& DensityOf() const;
  const std::vector<uint32_t>& ClassOf() const;

  MobSystem* sys_ = nullptr;
  Physics* phys_ = nullptr;
  World* world_ = nullptr;
  DebrisSystem* debris_ = nullptr;

  uint64_t id_ = 0;
  int defIndex_ = -1;          // into MobSystem's def list (events, persistence)
  const MobDef* def_ = nullptr;
  bool alive_ = true;
  GoreProfile gore_;           // this creature's own bleed character

  // ---- steering: intent vs actuation (NPC driver state; the avatar writes
  // heading_ directly from the camera and ignores the rest) ------------------
  // `heading_` is where the BODY actually points — the only thing the pose,
  // the gait and MobFacing ever read. Nothing outside MobSystem::Steer (or the
  // avatar's driver) may write it; behaviours write desiredHeading_ and the
  // gap closes at a bounded rate.
  float heading_ = 0;
  float desiredHeading_ = 0;
  float turnVel_ = 0;
  float driveScale_ = 1.0f;
  uint32_t blockedTicks_ = 0;
  float phase_ = 0;            // walk cycle (legacy swing fallback)
  uint32_t lastTurnTick_ = 0;

  Vec3 origin_{};              // prefab min corner, world voxels
  std::vector<MobLimb> limbs_;
  AnimState anim_;             // float presentation state (never hashed)
  float speedNow_ = 0;         // measured planar speed, voxels/sec
  Vec3 bodyUp_{0, 1, 0};       // foot-plane normal (slope tilt)
  float bodyY_ = 0;            // prefab MIN CORNER height (same frame as origin_.y)
  float restSoleY_ = 0;        // rest sole height above the min corner
  // Rest height of the leg chain's ROOT (the hip anchor) above the min corner,
  // measured off the rig in BuildRig beside restSoleY_. The pair of them is the
  // rig's standing leg SPAN: `restHipY_ - restSoleY_` is how far the hip sits
  // above the ankle in the authored pose, and comparing that against the
  // chain's summed bone lengths is the only way to know how much reach a stride
  // has left to spend. On this human it is 6.75 against a 6.79 chain — a
  // standing figure's legs are all but straight, which is why the avatar has to
  // crouch to walk at all (see the stance note in PlayerAvatar::UpdateGait).
  float restHipY_ = 0;
  // Horizontal distance from that hip anchor to its own ankle anchor in the
  // REST pose. Not zero: this human's ankle sits 0.5 world voxels in front of
  // its hip (the shank leans forward), and the gait's stance point inherits
  // that offset — so the foot starts half a voxel into its own forward reach
  // before the velocity lead adds anything. Left out of the stance crouch it
  // consumed the entire reach reserve and the IK sat on its clamp.
  float restFootAhead_ = 0;
  bool footInit_ = false;

  // The rig this instance actually animates: a COPY of def_->skel/limbs,
  // owned per creature, because a held ITEM borrows a real rig slot by
  // APPENDING a part — the shared def must not grow a sword every time
  // somebody picks one up. `limbs_`, `skel_.parts` and `limbDefs_` stay
  // index-parallel, which several loops depend on.
  AnimSkeleton skel_;
  std::vector<MobLimbDef> limbDefs_;
  std::vector<uint8_t> hidden_;      // per-limb render suppression

  // How many slots the AUTHORED rig has. Everything at or past this index was
  // APPENDED — a worn shell or a held item — and is the only thing
  // RemoveAppendedSlots is allowed to touch. Recorded once in BuildRig rather
  // than re-derived from def_->limbs.size() at each use, because a hot reload
  // can swap the def under a live rig and the two would then disagree.
  int baseLimbs_ = 0;

  // ONE WORN PIECE: which equip slot it came from, what it is by name, and the
  // appended rig slots it occupies (one per ItemCover entry that found a limb).
  //
  // By NAME, not by library index: item indices are file-order and every R
  // hot-reload renumbers them (item.h's index hazard). The slot LIST is
  // index-into-limbs_ and is fixed up by RemoveAppendedSlots whenever anything
  // ahead of it in the appended tail goes away.
  struct WornPiece {
    int equipSlot = -1;
    std::string item;
    std::vector<int> slots;
    // Which COVER ENTRY each slot came from, parallel to `slots`. Not the same
    // as the slot's position in the list: a cover entry that finds no limb on
    // this wearer is skipped, so a one-armed goblin's robe has three shells
    // from cover entries 0, 1 and 4. Damage is indexed by cover entry, and
    // without this the blob would be applied to the wrong shells.
    std::vector<int> cover;
    // ---- the occlusion index (one per shell, parallel to `slots`) ----------
    // Dense material-by-cell over the shell's own lattice box, so
    // WornOccludes is an O(1) transform-and-look-up rather than a walk of a
    // few thousand voxels per probe. DERIVED and disposable: rebuilt whenever
    // the shell's voxel count changes, which is the only thing that can move
    // a hole into it (carve and burn both compact the lattice). Built LAZILY,
    // so a dressed creature standing in a field costs nothing at all — the
    // probe is only ever reached from the burn pass's `scanHot` branch.
    struct ShellIndex {
      IVec3 min{}, dims{};
      std::vector<uint16_t> mat;   // 0 = no voxel here
      size_t builtFor = (size_t)-1;  // voxel count the index was built from
    };
    std::vector<ShellIndex> index;
  };
  std::vector<WornPiece> worn_;

  // Held item state — ONE piece of entity<->slot sync, kept only in EquipItem.
  int heldSlot_ = -1;
  std::string heldItem_;
  std::string heldPart_;
  int heldPartIndex_ = -1;
  Vec3 gripBody_{};            // grip point in the item's BODY frame
  // Swing pose pushed in by the driver (SetWeaponPose). Pure presentation.
  Vec3 weaponHand_{}, weaponDir_{0, 1, 0}, weaponUp_{0, 0, 1};
  float weaponWeight_ = 0;

  // Particles authored outside the tick (Sever is reached from damage handling
  // all over the frame); drained by the driver's PreTick.
  std::vector<ParticleSpawn> pendingSpawns_;
  // Re-entrancy guard: FlushBurn expresses itself as a CarveLimb, and
  // CarveLimb flushes before it reads the lattice.
  bool inBurnFlush_ = false;
  // ---- "THIS CARVE IS AN EDGE, NOT A BLAST OR A FIRE" ------------------------
  //
  // Set for the duration of Mob::CutLimb's carve, and read by exactly two
  // rules in CarveLimb: the cut-through sever and the neck sever. Both are
  // deliberately NOT applied to the other carve causes, and the reason is
  // asymmetric risk rather than principle.
  //
  //   * BURNING already has a documented, tested account of what happens when
  //     a limb comes apart (the anchor component keeps the identity, the rest
  //     leaves as fragments, and `mob-burn` asserts a limb never leaves while
  //     it is still mostly there). Routing a burn-through into Sever() would
  //     re-open exactly the "a burning body dismembers instead of charring"
  //     bug that block was written to close.
  //   * A BLAST is a sphere and has no direction; "cut through" is not a thing
  //     it does. Its existing behaviour — crater, fragments, collapse when too
  //     little is left — is the right account of it.
  //
  // A blade is the one cause where the geometry says something the fraction
  // cannot, so it is the one cause that gets to ask.
  //
  // Note this is a MOB-level flag, not MobSystem::bladeCut_ (which exists for
  // the AUDIO cause and is set by the caller around a whole sweep). They mean
  // different things and the avatar has no MobSystem at all, which is what
  // Phase C's "an NPC cuts the player" path needs.
  bool inBladeCut_ = false;

  // ---- IS THIS RIG SLOT A GARMENT? -------------------------------------------
  //
  // The one question the gore path has to ask and could not. Armour is "a set
  // of borrowed rig slots" (DESIGN.md 8c), which is what makes a shell burn,
  // dissolve, carve and sever through the body's own machinery with no armour
  // code -- and is exactly why the body's own machinery then treated a robe
  // burning off the shoulders as a shoulder coming off: CarveLimb charged a
  // wound and a drip budget, and Sever armed an arterial gout on the PARENT
  // limb. Three reported symptoms, one missing distinction:
  //
  //   * "fire burning clothes off triggers blood spurts/dismemberment"
  //   * "clothes burning off launches the player" (the shell was handed to
  //     DebrisSystem as a dynamic body overlapping the wearer's own capsule)
  //   * a burning garment reading as an injury on the body HUD
  //
  // Asked by TAG, not by index: `ld.tag == "worn"` is what AppendWornShell
  // stamps and what main.cpp's BodySlotFor already keys the HUD off, so there
  // is one spelling of "this is wardrobe, not anatomy" rather than two. The
  // held item (an appended slot with the item's own tag) is deliberately NOT
  // worn -- a sword cut out of the hand is not a garment and never bled.
  bool IsWornSlot(int limbIndex) const {
    return limbIndex >= baseLimbs_ && limbIndex < (int)limbDefs_.size() &&
           limbDefs_[limbIndex].tag == "worn";
  }

  // how long a severed piece holds its last animated pose before ragdolling
  static constexpr float kSeverHoldSeconds = 0.25f;
  // A carved chunk needs this many voxels to become its own rigidbody.
  static constexpr uint32_t kMinFragmentVoxels = 4;
  // Fragments one carve may spawn (rule 2: bound every emergent process).
  static constexpr uint32_t kMaxCarveFragments = 3;
  // Below this fraction of its authored volume a limb severs.
  static constexpr float kLimbCollapseFraction = 0.25f;
  // Carve damage per voxel removed, as a fraction of the limb's volume.
  static constexpr float kCarveDamagePerVolume = 1.5f;
  // Voxels a limb may burn away before its collider is re-derived:
  // max(floor, voxels >> shift).
  static constexpr uint32_t kBurnRebuildFloor = 12;
  static constexpr uint32_t kBurnRebuildShift = 6;

  friend class MobSystem;
};

class MobSystem {
 public:
  // `reactions` is the compiled reaction table. MobSystem needs it for the
  // same reason DebrisSystem does: limb voxels are CPU state no CA pass
  // touches, so per-voxel burning and dissolution run the authored table on
  // this side (sim/reactcpu.h holds the half that must not diverge).
  void Init(Physics* phys, World* world, DebrisSystem* debris,
            const std::vector<MaterialDef>& mats,
            const std::vector<ReactionGpu>& reactions);
  // Carving a micro limb clones its brick copy-on-write out of the SAME pool the
  // renderer uploads, so the owner hands it over once at startup (as it already
  // does for DebrisSystem). Not owned. Without it, micro limbs still take real
  // damage — they just cannot show it.
  void SetMicroSet(MicroBodySet* set) { microSet_ = set; }
  void OnMaterialsReloaded(const std::vector<MaterialDef>& mats,
                           const std::vector<ReactionGpu>& reactions);
  // This tick's integer day phase, so a day/night-gated reaction behaves the
  // same on a limb as it does in the grid. Unset means night; see the same
  // setter on DebrisSystem.
  void SetDayPhase(uint32_t phase) { dayPhase_ = phase; }
  void SetDefs(std::vector<MobDef> defs);           // hot reload
  const std::vector<MobDef>& Defs() const { return defs_; }
  // Tear down every mob. `rewindIds` also restarts the id counter, which is a
  // TEST-ONLY seam: mob ids seed gore variance and the blast crater's noise, so
  // rewinding them changes how the next creature bleeds and tears. See the note
  // at the definition.
  void Reset(bool rewindIds = false);                                      // world regen

  // Spawn def at a world cell (mob min corner; caller picks ground). 0 = fail.
  uint64_t Spawn(int defIndex, IVec3 atVoxel);
  // The live creature record, or null. The Mob API (damage, carve, ignite,
  // equip...) is the per-creature surface; the id-keyed wrappers below remain
  // for callers that only hold a body handle or an id.
  Mob* FindMobById(uint64_t id);
  // Mob combat scaffolding: hand any creature an item, exactly as the player
  // equips one (the implementation is Mob::EquipItem, shared with the avatar).
  bool EquipItem(uint64_t mobId, const ItemDef* item,
                 const char* context = "held_right");
  // The same scaffolding for ARMOUR: any creature can be dressed through the
  // identical path the player uses (Mob::WearItem). Nothing calls these yet —
  // AI that chooses to put a helmet on is out of scope — but the API is what
  // makes "the goblin is wearing the helmet it dropped" content rather than a
  // feature.
  bool WearItem(uint64_t mobId, const ItemDef* item, int equipSlot);
  bool UnwearItem(uint64_t mobId, int equipSlot);

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
  //
  // `cellOps` receives the grid half of per-voxel burning: a burning limb voxel
  // emits REAL fire into the world as a fill-air-only op, exactly as burning
  // debris does. That is the whole "a mob on fire runs into a bush and the bush
  // catches" mechanic, and it needs no mechanism of its own — the fire it emits
  // is an ordinary fire voxel that spreads by ordinary CA rules.
  void PreTick(uint32_t tick, World& world, std::vector<BrushOp>& ops,
               std::vector<CellOp>& cellOps,
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
  // THE BLADE ENTRY POINT — a kerf along the swept edge rather than a sphere,
  // plus the blood soak and the structural sever. See Mob::CutLimb and the
  // BladeCut struct above. Returns true when the handle was a live limb.
  bool CutLimb(uint64_t bodyHandle, const BladeCut& cut, World& world,
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

  // ---- persistence (sim/worldio.h, entities.sve section 'MOBS') -----------
  // Live mobs round-trip: def BY NAME (an index is load-order dependent and
  // rots — CLAUDE.md "author by name, resolve at load"), origin/heading, and
  // per-limb hp, sever state and carve lattices (with the rig offsets a carve
  // shifted, mob.h Limb notes). Load re-runs Spawn() so every derived quantity
  // — anim state, joints, rest sole, flipbooks — comes from the def exactly as
  // a fresh mob's does, then overlays the saved damage. DEAD mobs are not
  // saved: their limbs were adopted into DebrisSystem at death and travel in
  // the 'DBRS' section as the debris they already are.
  static constexpr uint32_t kSaveVersion = 1;
  void SaveState(std::vector<uint8_t>& out) const;
  // Contract (worldio LoadEntities): Reset() has already run.
  bool LoadState(const uint8_t* data, size_t len, uint32_t version);

  // ---- sever events -------------------------------------------------------
  // One entry per limb that came off, reported rather than voiced here: this
  // system knows nothing about audio, the same way it hands particle spawns
  // back instead of emitting them. main.cpp drains these after the tick.
  //
  // Sever() is reached from a dozen call sites (explosion, laser, hp loss,
  // carve collapse) and none of them know what CAUSED the cut, which is the
  // one thing the audio needs: only a BLADE plays the dismember sound. So the
  // cause is set by the caller around the call — see BladeCutScope — rather
  // than threaded through every signature.
  struct SeverEvent {
    Vec3 posVoxel;      // the cut point, world voxels
    uint64_t mobId = 0;
    int limbIndex = -1;
    // Index into Defs(). Carried on the event rather than looked up by mobId
    // afterwards, because a killing blow can despawn the mob before the frame
    // drains this — the def outlives it, the mob does not.
    int defIndex = -1;
    bool byBlade = false;  // a weapon edge did this, not a blast or a beam
    float severity = 1.0f; // 0..1, blade speed for a cut
  };
  const std::vector<SeverEvent>& SeverEvents() const { return severs_; }
  void ClearSeverEvents() { severs_.clear(); }

  // ---- creature voices ----------------------------------------------------
  // Everything a mob says that is NOT a sever, reported the same way and for
  // the same reason: this system knows nothing about audio, and a killing blow
  // can despawn the mob before the frame drains the list, so the def index
  // travels on the event rather than being looked up afterwards.
  //
  // Sever keeps its own list because it carries a cause (`byBlade`) that
  // nothing else needs, and because it already has a consumer.
  //
  // BOUNDED BY CONSTRUCTION. At most one entry per mob per kind per tick:
  // Hurt de-duplicates on push (an explosion carving six limbs of one creature
  // is one cry, not six), and Death can only fire once per mob because Die()
  // early-outs on `alive`. The tick loop runs at most 4 times per frame, so a
  // frame carries at most 4 * mobs entries even in a massacre.
  enum class VoiceKind { Hurt, Death };
  struct VoiceEvent {
    Vec3 posVoxel;       // where the creature is, world voxels
    uint64_t mobId = 0;  // rate-limiter key on the audio side
    int defIndex = -1;   // index into Defs(); outlives the mob
    VoiceKind kind = VoiceKind::Hurt;
    float intensity = 1.0f;  // 0..1, fraction of the limb's max hp removed
  };
  const std::vector<VoiceEvent>& VoiceEvents() const { return voices_; }
  void ClearVoiceEvents() { voices_.clear(); }

  // RAII: marks every Sever() reached inside its lifetime as a blade cut.
  // Scoped rather than a parameter because the melee sweep calls Damage() and
  // CarveLimbRadial(), each of which may sever internally several frames deep;
  // adding a `byBlade` argument to that whole chain would touch the laser and
  // explosion paths too, for a fact only the audio cares about.
  struct BladeCutScope {
    MobSystem& sys;
    float prevSeverity;
    bool prevBlade;
    BladeCutScope(MobSystem& s, float severity) : sys(s) {
      prevBlade = sys.bladeCut_;
      prevSeverity = sys.bladeSeverity_;
      sys.bladeCut_ = true;
      sys.bladeSeverity_ = severity;
    }
    ~BladeCutScope() {
      sys.bladeCut_ = prevBlade;
      sys.bladeSeverity_ = prevSeverity;
    }
  };

  // Wounds that are bleeding hard enough to be worth hearing. Rebuilt each
  // tick in PreTick; main.cpp turns each into a positioned loop.
  struct BleedSource {
    Vec3 posVoxel;
    uint64_t key = 0;      // stable per (mob, limb) so one loop tracks one wound
    float intensity = 0;   // 0..1 of the bleed budget cap
  };
  const std::vector<BleedSource>& BleedSources() const { return bleeds_; }

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
  // Id of the i'th mob record, 0 past the end. A LOADED mob gets a fresh id
  // (ids are session-local), so a test that saved one id needs this to find
  // the reincarnation.
  uint64_t MobIdAt(uint32_t i) const {
    return i < mobs_.size() ? mobs_[i].Id() : 0;
  }
  // ---- THE ID COUNTER, AND PUTTING IT BACK ----------------------------------
  //
  // A mob id is not a handle: it seeds the entity-scoped gore variance
  // (MakeGoreProfile), the blast crater's noise, and the per-limb RNG key the
  // burn/dissolve pass draws against. So a caller that merely SPAWNS creatures
  // shifts every later creature's randomness, which is a property gates share
  // one World for and nothing else in the suite can see.
  //
  // Measured: inserting four gates that each spawn a handful of fixtures ahead
  // of `armor-react` re-rolled its acid bath and the bare arm dissolved 0 skin
  // voxels in 120 ticks instead of 18, failing a `lostB > 0` sanity check about
  // whether the bath was acid at all. Nothing about acid had changed.
  //
  // Reset(rewindIds=true) is NOT the fix: setting the counter to 1 is a
  // different perturbation, not the absence of one. A gate that wants to leave
  // the suite as it found it saves this and puts it back.
  uint64_t NextIdCounter() const { return nextId_; }
  void SetNextIdCounter(uint64_t n) { nextId_ = n ? n : 1; }
  uint64_t LimbBody(uint64_t mobId, int limbIndex) const;
  bool IsAlive(uint64_t mobId) const;

  // THE MOST INTACT LIMB ANY SEVER HAS TAKEN since the last clear, as a
  // fraction of that limb's spawn volume, with its name. -1 = nothing severed.
  //
  // A limb is supposed to come off because it RAN OUT: the geometric floor is
  // kLimbCollapseFraction (25% left) and the hp floor at
  // kCarveDamagePerVolume 1.5 is 33% left. So a sever recorded well above
  // those is by construction not a limb that was eaten — it is a damage rule
  // over-charging, or the connectivity split giving the limb's identity to a
  // fragment. Both of those shipped, and from outside both read only as "a
  // limb came off". Sampling from a test is impossible without this: the
  // instant is one tick wide, and if the limb was vital the whole limb list is
  // gone by the time anyone could look.
  float WorstSeverFraction() const { return worstSeverFrac_; }
  const std::string& WorstSeverLimb() const { return worstSeverLimb_; }
  void ClearSeverStats() {
    worstSeverFrac_ = -1.0f;
    worstSeverLimb_.clear();
  }
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
  // How many of a limb's surviving voxels have at least `minOpen` of their six
  // face-neighbours missing — the roughness of what a carve LEFT BEHIND.
  //
  // A voxel count alone cannot tell a torn chunk from a fine sprinkle: remove
  // the same number of voxels as white noise and as a correlated blob and the
  // count is identical while the result looks nothing alike. Isolated spurs are
  // what speckle leaves and what a chunk does not, so this is the shape of the
  // crater expressed as a number the crater gate can assert on.
  uint32_t LimbOpenFaceCount(uint64_t mobId, int limbIndex, int minOpen) const;
  // ---- per-voxel burning introspection ---------------------------------------
  // How many voxels of this limb are currently ALIGHT (carry a decay/emit rule).
  // This is the size of the active front, and it is the number the burn gate
  // asserts on: "an idle mob in a settled world does zero burn work" is
  // literally "this is 0 and stays 0", and "a lone hot voxel gutters out" is
  // "this went to 0 without the limb losing matter".
  uint32_t LimbBurningCount(uint64_t mobId, int limbIndex) const;
  // How many of this limb's voxels are of material `mat`. The differential the
  // burn gate is built on — flesh charring is a MATERIAL transition, so
  // "cooked, then burnt" is visible as counts moving between slots rather than
  // as a state nobody can see.
  uint32_t LimbMaterialCount(uint64_t mobId, int limbIndex, uint32_t mat) const;
  // Set fire to up to `count` of a limb's SURFACE voxels and return how many
  // took. The product is resolved from the reaction table (the first rule whose
  // product is tag:hot), so a material with no path to burning — bone, steel —
  // simply refuses, with no list of exceptions to maintain.
  //
  // This is the direct-ignition entry point a fire spell or a thrown torch
  // wants; the ordinary route into burning is contact with something hot in the
  // world, which needs no call at all.
  // `onlyMat` restricts the choice to voxels of one material (0 = any), which
  // is what lets a caller say "light the CLOTH" on a limb whose surface is part
  // robe and part skin.
  uint32_t IgniteLimb(uint64_t mobId, int limbIndex, uint32_t count,
                      uint32_t onlyMat = 0);

  // ---- the burn pass, for a limb this system does NOT own -------------------
  //
  // PlayerAvatar drives its own rig out of PlayerAvatar::Part, and the player
  // has to burn exactly as an NPC does. These three are how it gets that
  // without a second implementation: MobSystem owns the compiled reaction
  // mirror (one table, built once on materials reload) and runs the pass over
  // whatever lattice the caller points at.
  //
  // The caller keeps what is genuinely its own — how a part that burnt through
  // is severed, how hp falls, how its collider is rebuilt — because those
  // differ between a mob and the player and always will.
  //
  // Returns true if anything changed. `frontBudget`/`opsBudget` are in/out and
  // shared across every body burning this tick (rule 2: bound the process, not
  // each participant).
  // ---- WHAT THE ARMOUR ACTUALLY STOPPED, counted -------------------------
  //
  // Not diagnostics-in-passing: this is the reporter CLAUDE.md's rule 6 asks
  // for. "The covered limb caught fire" has at least three causes — the shell
  // is somewhere else, the probe's transform is wrong, or the burn pass
  // samples past a shell that is exactly where it should be — and a boolean
  // at the end of a 160-tick fire distinguishes none of them. These do, on one
  // line, for the cost of three increments.
  struct WornStats {
    uint32_t seedsBlocked = 0;   // contact samples the coat refused
    uint32_t seedsPassed = 0;    // ...and ones that reached the skin anyway
    uint32_t nbrSubstituted = 0; // world neighbours replaced by cloth/steel
    uint32_t nbrThreats = 0;     // ...out of this many that could have acted
    // Misses a LONGER ray would have caught. Separates "the reach is too
    // short" from "there is genuinely no shell in that direction", which is
    // the one distinction a bare miss count cannot make and the one that
    // decides whether the fix is a constant or a redesign.
    uint32_t nbrMissInReach = 0;
  };
  const WornStats& Worn() const { return wornStats_; }
  void ResetWornStats() { wornStats_ = WornStats{}; }

  // ---- WHY THE FIRE STOPPED, counted --------------------------------------
  //
  // Rule 6 for the other half of the pass. "The body stopped charring" has two
  // causes that a material census at the end cannot tell apart: the fire never
  // reached the voxel, or it reached it and the neighbour-count ramp refused
  // it. This distinguishes them on one line.
  //
  // `hotFaces` is the distribution of `cnt` at the instant ReactScaledChance()
  // decides. A spectrum piled at 1 and 2 with nothing at 3 says an authored
  // minCount is geometrically UNREACHABLE rather than merely unlucky, which is
  // a different bug with a different fix than "the fire is too weak".
  //
  // `exposed` is that distribution one step earlier: how many of a candidate's
  // six faces have no lattice neighbour, i.e. how much of it the world can
  // touch at all. For a voxel whose only heat source is the world that number
  // is the CEILING on the ramp count, so the two histograms together say
  // whether the threshold or the fire is what is missing.
  struct BurnStats {
    uint32_t candidates = 0;    // voxels the pass evaluated a rule for
    uint32_t rampRolls = 0;     // scaled rules reached
    uint32_t rampRefused = 0;   // ...of which minCount refused outright
    uint32_t rampWidened = 0;   // ...and ones the world-pitch reading raised
    uint32_t hotFaces[7] = {};  // ramp count histogram, 0..6
    uint32_t exposed[7] = {};   // faces with no lattice neighbour, 0..6
  };
  const BurnStats& Burn() const { return burnStats_; }
  void ResetBurnStats() { burnStats_ = BurnStats{}; }

  bool BurnOneLimb(BurnLimbView& v, uint32_t tick, uint32_t rngKey, World& world,
                   std::vector<CellOp>& cellOps, uint32_t& frontBudget,
                   uint32_t& opsBudget);
  uint32_t IgniteOneLimb(BurnLimbView& v, uint32_t count, uint32_t onlyMat);
  // True once the reaction mirror has been built. A caller with no tables must
  // not burn: it would silently do nothing rather than fail.
  bool BurnTablesReady() const { return !reactions_.empty() && !matGpu_.empty(); }
  // World position of one of a limb's SURVIVING voxels — the `n`th, wrapped.
  // Deliberately not the centroid: once a carve has hollowed a limb, its
  // centroid is in the cavity, and a tool aimed there eats nothing. Anything
  // that wants to keep cutting (the selftest, a future aim assist) has to aim
  // at flesh that is actually still present.
  Vec3 LimbVoxelPos(uint64_t mobId, int limbIndex, uint32_t n) const;
  // The limb's JOINT ANCHOR in world voxels, through its live pose. Invariant
  // across carves (see the impl), which is what a test that keeps cutting one
  // cross-section needs and LimbVoxelPos cannot give — that one names the nth
  // SURVIVING voxel and therefore walks as the limb is eaten.
  Vec3 LimbAnchorPos(uint64_t mobId, int limbIndex) const;
  // Voxels on the AUTHORITATIVE lattice (skin when there is one), i.e. the
  // lattice LimbVoxelsAtSpawn counted. LimbVoxelCount reports the collider,
  // and mixing the two scales every fraction by (skinScale/physScale)^3.
  uint32_t LimbArtVoxelCount(uint64_t mobId, int limbIndex) const;

 private:
  // ---- per-voxel burning / dissolution (docs/PLAN_body_reactivity.md) --------
  //
  // The limb twin of DebrisSystem::BurnBodies, and deliberately NOT that
  // function generalized. BurnBodies is driven by a rotating CURSOR over the
  // whole voxel list, which is correct for a 200-voxel plank and does not
  // survive contact with a 31,456-voxel torso — one mina is fifteen times
  // kBurnScanPerTick all by herself. This pass is driven by an ACTIVE FRONT
  // instead: fire lives on a surface, so the burning set of a limb is a 2D
  // front over a 3D volume and its cost is bounded by that, not by the volume.
  //
  // Same RULES, different driver. Both evaluate the authored reaction table
  // through sim/reactcpu.h, so a chance authored once behaves the same on a
  // limb, on the severed version of that limb, and in the grid.
  //
  // The per-creature half lives on Mob (BurnTick/FlushBurn); this is the NPC
  // driver looping it over mobs_ under the shared budgets.
  void BurnLimbs(uint32_t tick, World& world, std::vector<CellOp>& cellOps,
                 std::vector<ParticleSpawn>& spawns);
  // Build the dense neighbour index over a limb's current lattice and seed the
  // front from whatever is already alight. O(voxels + boundingBox), paid once
  // when something reactive first comes near the limb.
  void BuildBurnIndex(BurnLimbView& v);
  // The material a voxel of `mat` becomes when it catches: the product of the
  // first rule in its bucket whose product carries tag:hot. 0 = cannot burn.
  // Resolved from the table at load, so no material id is ever named in code.
  uint32_t IgnitedForm(uint32_t mat) const {
    return mat < ignitedForm_.size() ? ignitedForm_[mat] : 0u;
  }

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

  Physics* phys_ = nullptr;
  World* world_ = nullptr;
  DebrisSystem* debris_ = nullptr;
  MicroBodySet* microSet_ = nullptr;  // shared brick pool; see SetMicroSet
  std::vector<float> densityOf_;
  std::vector<uint32_t> classOf_;
  // ---- burn tables, rebuilt on materials hot-reload --------------------------
  // Data-driven, exactly as DebrisSystem's are: no material id is hardcoded
  // anywhere in the burn path, so "flesh chars" and "cloth catches easily" stay
  // facts about assets/materials/*.json and not about this file.
  std::vector<MaterialGpu> matGpu_;
  std::vector<ReactionGpu> reactions_;
  std::vector<uint8_t> matSelfActive_;  // has decay/emit rules — i.e. is ALIGHT
  std::vector<uint8_t> matHasPair_;     // has pair rules — i.e. is ignitable
  WornStats wornStats_{};
  BurnStats burnStats_{};
  std::vector<uint8_t> matHot_;         // carries tag:hot
  // Material has a pair rule that REWRITES ITS NEIGHBOUR. This is the inbound
  // half of the world coupling and it is what makes acid work with no
  // acid-specific code: `acid + tag:dissolvable -> neighborBecomes air` is
  // already authored, skin/cloth/leather are already `dissolvable`, so a limb
  // standing in acid dissolves because the grid's rule is evaluated FROM the
  // grid cell onto the limb voxel — the same direction the GPU evaluates it.
  std::vector<uint8_t> matRewritesNbr_;
  // ...and the NARROWER question the wake gate asks: could that rewrite land on
  // a BODY? Narrower because the gate is what decides whether a limb allocates
  // its dense index at all, and `matRewritesNbr_` is far too generous for that
  // job — `grass + tag:soil -> grass` rewrites a neighbour, grass is under
  // every mob in the world, and gating on it would hold a dense index open for
  // every limb of every creature standing on a lawn, forever. That is precisely
  // the permanent per-limb allocation rule 2 forbids.
  //
  // "Could land on a body" is answered from the table, not from a list: every
  // material a creature is made of carries `dissolvable`, so a rule qualifies
  // if its neighbour predicate can match something dissolvable. tag:soil cannot;
  // tag:dissolvable can.
  std::vector<uint8_t> matAttacksBody_;
  // mat -> what it becomes when it catches (see IgnitedForm).
  std::vector<uint32_t> ignitedForm_;
  uint32_t dayPhase_ = 0;
  std::vector<MobDef> defs_;
  std::vector<Mob> mobs_;
  uint64_t nextId_ = 1;
  bool instancesDirty_ = false;
  // Particles authored outside PreTick — Sever() is reached from damage
  // handling at several points in the frame, and appending straight to the
  // caller's spawn list from there would mean Sever needs it threaded through
  // every one of those paths. Drained (and cleared) at the top of PreTick.
  std::vector<ParticleSpawn> pendingSpawns_;

  // Drained by main.cpp each frame. Bounded by the same limits that bound
  // severing itself: a mob has a fixed limb count and kMaxMobs of them exist.
  std::vector<SeverEvent> severs_;
  // See WorstSeverFraction. Written by Mob::Sever, cleared only on request —
  // this is a high-water mark over a whole test, not a per-tick event queue.
  float worstSeverFrac_ = -1.0f;
  std::string worstSeverLimb_;
  std::vector<BleedSource> bleeds_;
  std::vector<VoiceEvent> voices_;
  // Push a creature voice, de-duplicating Hurt per mob per drain window (see
  // VoiceEvent). Silently drops a mob with nothing bound is NOT this layer's
  // job — the audio side already skips an unbound slot — but the dedup is,
  // because it is a property of the EVENT, not of the sound.
  void PushVoice(const Mob& mob, VoiceKind kind, Vec3 posVoxel,
                 float intensity);
  // Set by BladeCutScope for the duration of the melee sweep, so Sever() can
  // record what caused it without every caller having to say.
  bool bladeCut_ = false;
  float bladeSeverity_ = 1.0f;
  friend struct BladeCutScope;

  static constexpr uint32_t kMaxMobs = 16;
  // (the drip op budget is now gore.bleedOpsPerTick in tuning.json)
  // ---- per-voxel burning -----------------------------------------------------
  // Front voxels examined per tick across EVERY limb of EVERY mob. A fully
  // engulfed mob's front is a few hundred to ~2000 voxels, so this covers
  // several burning creatures at full rate and degrades to round-robin past
  // that rather than to a frame spike (rule 2: bound every emergent process).
  // The avatar deliberately burns under its OWN budget (avatar.cpp BurnParts):
  // a crowd of burning NPCs must not starve the fire on the player character.
  static constexpr uint32_t kBurnFrontPerTick = 6000;
  // Fire/ash ops all burning limbs together may push into the grid per tick.
  static constexpr uint32_t kBurnOpsPerTick = 96;
  // World cells ONE limb's ignition scan may look at. A limb's world AABB is
  // order 128 cells; this is the guard against a pathological pose, not a
  // budget anything normal comes near.
  static constexpr uint32_t kBurnScanCells = 4096;
  // Face SAMPLES one limb may transform when seeding from world contact. A
  // reactive cell touching the limb costs S*S of these (64 at skinScale 8), and
  // the direction cull in BurnOneLimb means only faces that really do point at
  // the limb spend any. Sized so a limb SUBMERGED in acid — a few hundred
  // contact faces — seeds its whole wetted surface in one tick instead of a
  // tenth of it, which is what it was doing while this shared kBurnScanCells.
  static constexpr uint32_t kBurnSeedProbes = 32768;
  // Ticks a cold limb keeps its dense index before releasing it, so a limb
  // walking through a campfire does not rebuild the index every other tick.
  static constexpr uint32_t kBurnIndexGrace = 30;

  // Mob's shared mechanics reach this system's services (burn tables, micro
  // pool, event sinks, material tables) through this friendship — the same
  // seam the avatar used to reach BurnOneLimb through, made symmetrical.
  friend class Mob;
};
