#pragma once
#include "sim/scale.h"  // MetresToCells / MetresToCellsI
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "game/ai_behavior.h"
#include "game/anim.h"
#include "game/equipment.h"
#include "game/impact.h"    // StrikeProfile / StrikeEffectorMode: what a blow IS
#include "game/melee.h"     // WeaponPose: the stroke driver's command to the rig
#include "game/selfclip.h"  // ClipReport: is this pose inside itself
#include "game/strokes.h"   // NpcStroke: one authored swing, live
#include "math3d.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/materials.h"
#include "net/mobsync.h"    // the four records mob ownership exchanges
#include "sim/bytestream.h"  // ByteWriter/ByteReader: the per-mob record
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
  // Normal of the blade's cutting plane, same local frame, unit. See the long
  // note on ItemDef::edgeFlat — this is the rig-part half of the same field,
  // so a blade that IS a limb (a claw, a mandible) rolls and scores the same
  // way a held sword does.
  bool hasEdgeFlat = false;
  Vec3 edgeFlat{0, 1, 0};
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
// RETIRED as a locomotion budget — see LocomotionDef::stepUpM (anim.h) and
// Mob::StepUpCells(). It survived as a flat 0.20 m against the player's 0.58 m
// and was most of why NPCs read as worse at moving than the player on the same
// ground. Kept only as the FLOOR a rig's authored value is clamped against
// being useful for, so nothing silently authors a body that cannot cross a
// pebble.
inline constexpr int kMobMinStepUpCells = 1;
// How far above the mob's origin a ground probe starts looking down.
inline constexpr int kMobProbeLiftCells = MetresToCellsI(0.30f);

inline constexpr int kColliderVoxelsPerMetre = 40;
inline constexpr uint32_t kMaxPhysScale = (uint32_t)(
    kColliderVoxelsPerMetre / kVoxelsPerMetre < 1
        ? 1
        : kColliderVoxelsPerMetre / kVoxelsPerMetre);

// ---- WHAT IS ALREADY MISSING (sidecar `rot`) --------------------------------
//
// A creature that is born having ALREADY been damaged. Each limb is bitten a
// few times at spawn, through the ordinary carve path (Mob::CarveLimb), so
// there is no second notion of "damage" anywhere: the voxels really are gone,
// the collider and the micro brick really are rebuilt around the holes, hp is
// charged for the volume exactly as a sword's would be, and `voxelsAtSpawn`
// stays the PRISTINE count — which is what makes a rotted zombie read as 78%
// of a body in the HUD instead of as a whole small one.
//
// The bites are correlated-noise blobs, the same ValueNoise3 field the blast
// crater tears with, for the reason the crater note gives at length: an
// independent draw per voxel has no feature size, so white noise produces
// speckle and only a smooth field produces a CHUNK. The owner asked for
// "chunks... largely in groups", and the feature size is `blob`.
//
// EVERY LENGTH HERE IS A FRACTION OF THE LIMB'S OWN EXTENT, never an absolute
// voxel count. A hand and a torso differ by two orders of magnitude in volume,
// and a radius authored in voxels would erase one and graze the other; the
// same numbers on a critter and on mina have to mean the same thing.
struct MobRotDef {
  bool enabled = false;
  // Bites per limb, inclusive. Rolled per (mob, limb), so two zombies from the
  // same def are missing different pieces — that is the point of the feature.
  int bitesMin = 0, bitesMax = 2;
  // Bite radius as a fraction of the limb's bounding-sphere radius.
  float radiusMin = 0.22f, radiusMax = 0.46f;
  // Ceiling on the fraction of a limb's volume the whole rot may take. Enforced
  // by SCALING THE RADII DOWN before carving, not by stopping part-way: a bite
  // abandoned half-done would leave a lopsided hole, and the cap has to hold on
  // a hand as firmly as on a torso. Well under Mob::kLimbCollapseFraction
  // (0.25 LEFT is a collapse) by default, so rot never severs at spawn.
  float maxLoss = 0.22f;
  // Correlated-noise feature size in SKIN voxels: how big one torn-away piece
  // is. Same units and same field as gore.carveBlobSize.
  float blob = 2.5f;
  // Radius multiplier on a `vital` limb. A chunk out of the skull is the look;
  // losing the head at spawn is a corpse that never walked.
  float vitalScale = 0.55f;
  // ---- HOW BLOODY THE HOLE IS ----------------------------------------------
  // Multiplier on `gore.craterStainRim`: how far past the removed cells the
  // wound soak reaches. A multiple rather than an absolute so retuning blood
  // globally still reaches the undead. Above 1 because these wounds are OLD —
  // one that has been open a while has bled around itself, where a blade's
  // kerf has not. 0 disables the soak entirely (a dry, bloodless rot: bone
  // creatures, husks, anything that never had blood in it).
  float stainScale = 2.0f;
  // ---- ...AND HOW MANY OF THE HOLES ARE BLOODY AT ALL (2026-09-15) ---------
  //
  // WETNESS IS A PROPERTY OF ONE BITE, not of the creature and not of the limb.
  // Staining the whole limb in one pass — which is what the first version did,
  // because `StainWound` was handed every cell the carve took — gives every
  // hole on a body the same amount of blood, and a body whose wounds are all
  // equally fresh reads as uniform however good any single hole looks. The
  // owner's word for what it should be instead is a MISHMASH: some holes dry
  // and old, showing the flesh and bone the anatomy put under the skin, and
  // some still wet.
  //
  // So every bite rolls its own `wet` in 0..1 and the roll is a SPECTRUM with
  // an atom at zero, not a coin flip:
  //
  //   u < dryFraction          -> wet 0, no soak at all. The hole is a hole:
  //                               skin, flesh, muscle and bone as baked, which
  //                               is what the dry version of this feature
  //                               looked like and what half of them should
  //                               still look like.
  //   otherwise                -> wet ramps linearly from `wetMin` to `wetMax`
  //                               across the rest of the range, so the bloody
  //                               half is itself a gradient from a trace to a
  //                               fresh wound rather than a second uniform.
  //
  // `wetMin` is deliberately small but NOT zero: it is the "minute amount of
  // blood" end of the spectrum, and floored at something the 0..15 stain scale
  // can still represent after the taper (below about 0.07 the smear rounds to
  // nothing and the bite may as well have been dry, which the dry roll already
  // provides on purpose).
  //
  // dryFraction 1 is the bloodless rot `stainScale` 0 also gives; 0 is the
  // every-hole-bleeds behaviour this replaced.
  float dryFraction = 0.5f;
  float wetMin = 0.12f, wetMax = 1.0f;
  // Limb NAMES or TAGS never bitten. A rig that would rather keep its hands.
  std::vector<std::string> skip;
};

// ---- A RIG PART THAT IS ITSELF A WEAPON (sidecar `natural`; plan §3) -------
//
// A fist and a set of jaws are weapons that nobody handed the creature, and
// before this they could not be expressed at all: the whole swing pipeline
// started at `heldPartIndex_`, so an unarmed creature had no edge, no arm
// claim and no way to ask for one. A natural weapon is exactly what an item
// already is — AN EDGE SEGMENT AND A `StrikeProfile` — with the segment
// living on a part of the rig instead of on a borrowed slot.
//
// WHY IT IS NOT AN ITEM. An item is a thing the creature can drop, parry
// with, wear out and lose; a fist is anatomy. Modelling jaws as an invisible
// held item would make a headless zombie still able to bite (the slot outlives
// the part), would put a fist in the parry table, and would mean every rig
// shipped two .vox files. The part is the authority: `StyleUsable` refuses a
// style whose natural weapon's part has been cut off, and that is one check
// rather than a sync problem.
//
// THE EDGE IS IN THE PART'S OWN ART FRAME — the Y-up, origin-at-the-model's-
// MIN-CORNER frame `MobLimbDef::edgeFrom/edgeTo` are stored in, and scaled by
// `ArtToWorld()` at load exactly as those are (mob.cpp, the anchors block).
// It is stated as two POINTS rather than as MobLimbDef's axis+from+to pair
// because a part has no hilt to measure an offset from: a fist's edge runs
// from the wrist to the knuckles and a jaw's from the throat to the teeth,
// and neither is an offset along one model axis.
struct MobNaturalWeaponDef {
  std::string name;            // "fist.R", "jaws" — what a style's `weapon` names
  std::string part;            // rig part it belongs to; resolved at load
  int partIndex = -1;
  Vec3 edgeFrom{}, edgeTo{};   // world voxels after the load conversion
  float edgeHalfWidth = 0;     // carve radius, world voxels
  StrikeProfile strike;        // what it does when it lands (game/impact.h)
};

// ---- WHAT THIS CREATURE'S BITE CARRIES (sidecar `bite`; plan §6) -----------
//
// ON THE CREATURE, NOT ON THE WEAPON, and that placement is the whole design.
// A zombie's jaws are the human's jaws — `zombie.json` inherits `natural`
// untouched through `extends` — and what makes its bite rot flesh is that IT
// is rotten, not that its teeth are shaped differently. Putting the infection
// here means a ghoul, a plague rat and a diseased wolf are each one key in one
// file, and `Mob::StrikeProfileFor` ORs it onto whichever natural weapon is
// actually biting (`strike.bite > 0`).
//
// Materials BY NAME in the JSON, resolved to ids at load with the same lookup
// `bleed.material` uses — ids are file-order and renumber on an R reload.
struct MobBiteDef {
  uint16_t infectMat = 0;      // what the tear rewrites exposed flesh to
  uint16_t infectStain = 0;    // the LIQUID whose stain it smears over the hole
  bool Any() const { return infectMat != 0 || infectStain != 0; }
};

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
  // DOES A CUT ON THIS CREATURE CLOSE, OR GO ON ROTTING? Sidecar
  // `bleed.woundHeals`, default true, ANDed with the global gore.woundHeals.
  //
  // True (living flesh): the soak around a cut reverts to the tissue it
  // covered as it dries, at gore.woundHealSlow times the authored rate — the
  // wound stops spreading, the limb keeps its voxels.
  //
  // False (UNDEAD): the soak decays to air like a pool of blood on the ground,
  // so the hole widens by itself and the part eventually drops off. This is
  // what every body did before 2026-09-14, and it is the setting to put on a
  // zombie: nothing else in the rig has to change for a corpse that comes
  // apart as it walks. See the long note on BurnLimbView's wound fields.
  //
  // DEFAULTS TO FALSE ON AN UNDEAD DEF (`MobDef::undead`), because that is the
  // whole content of the word — see the note there. An explicit
  // `bleed.woundHeals` still wins either way.
  bool woundHeals = true;
  // WHICH MATERIALS ARE TISSUE, per material id: the ones a wound soaks. A
  // material is tissue when its `rubble` is this creature's blood (skin, flesh
  // and muscle all crumble to blood in materials.json; bone crumbles to dust),
  // or when it already is the wound material. Bone is left as bone so the hole
  // a blade opens shows it through the blood around it, instead of turning
  // into one more red voxel. Empty when nothing qualifies (a mob whose blood
  // no material crumbles to), which StainWound reads as "soak everything",
  // the pre-anatomy behaviour.
  std::vector<uint8_t> tissue;
  // ---- THE GORE WEIGHTS, FLATTENED PER MATERIAL ID -------------------------
  //
  // Straight copies of MaterialDef::woundHp / brainHp / rotRate, resolved once
  // at load into vectors indexed by material id, exactly as `tissue` above is.
  // The point of flattening is that the damage and infection passes run per
  // VOXEL on a hot path and must not hold a material table or do a lookup by
  // name; the point of carrying them per DEF rather than globally is `rotRate`,
  // whose unauthored default is "whatever tissue said", and tissue is itself
  // per-creature.
  //
  // All three are empty on a def built before materials were available, and
  // every reader treats empty / past-the-end as the pre-feature default (weight
  // 1, not brain, rot per `tissue`).
  std::vector<float> woundHp;
  std::vector<uint8_t> brainMat;
  std::vector<float> rotRate;
  // Per-material weight, defaulting to 1 past the end of the table so a mob
  // whose def predates the weights behaves exactly as it used to.
  float WoundHpOf(uint32_t mat) const {
    return mat < woundHp.size() ? woundHp[mat] : 1.0f;
  }
  bool IsBrain(uint32_t mat) const {
    return mat < brainMat.size() && brainMat[mat] != 0;
  }
  // 0 = the rot cannot touch it. Past the end falls back to `tissue`, which is
  // the behaviour every caller had before rotRate existed.
  float RotRateOf(uint32_t mat) const {
    if (mat < rotRate.size()) return rotRate[mat];
    if (tissue.empty()) return 1.0f;   // "no anatomy" = soak/eat everything
    return (mat < tissue.size() && tissue[mat]) ? 1.0f : 0.0f;
  }
  float bleedPerDamage = 1.5f; // wound budget voxels per point of damage
  // ---- IS THIS THING ALIVE? (sidecar `undead`) -----------------------------
  //
  // One flag, and deliberately only two consequences, because "undead" here is
  // a CONTENT word and not an engine subsystem:
  //
  //   * `woundHeals` defaults to FALSE. Living flesh dries a cut back to meat;
  //     a corpse's cut goes on rotting outward until the part drops off. That
  //     is byte-for-byte the pre-2026-09-14 behaviour, which the owner asked to
  //     keep for the walking dead precisely because limbs coming off looked
  //     right on them and wrong on everything else.
  //   * `rot` is honoured at spawn. A living creature is born whole.
  //
  // NOTHING ELSE BRANCHES ON IT, and adding a branch here should feel like a
  // decision rather than a convenience: the slower walk, the paler skin and the
  // softer joints of a zombie are all ordinary sidecar numbers, and they stay
  // ordinary sidecar numbers so that "undead" does not quietly become the name
  // of a second creature pipeline. A ghoul that sprints is then one file.
  bool undead = false;
  // ---- WHAT THIS BODY GETS UP AS (sidecar `turn`) --------------------------
  //
  // The content half of "killed by a zombie, rises as one". A creature that
  // dies with the rot in it does not stay a corpse: after `afterSec` its
  // remains are taken out of the world and it stands up as
  // DefWithEffects(itself, {into}) — ITSELF, not a stock zombie, so a turned
  // villager keeps the villager's proportions, art and rig.
  //
  // `into` is an EFFECT name, not a creature name, and that is the whole
  // reason this is three fields instead of a subsystem: what a bite makes of
  // you is the same modifier the zombie in the file is already made of.
  // Empty `into` (or `"turn": null` in a patch, which is how the zombie effect
  // opts its own bodies out) means this creature never turns.
  struct Turn {
    std::string into;         // effect to pour on, "" = never turns
    float afterSec = 0.0f;    // how long the corpse lies there first
    int infectedLimbs = 1;    // how much of it has to be rotten to count
  } turn;
  // ---- WHAT IT IS CARRYING (sidecar `loot`) --------------------------------
  //
  // A creature's PACK, as opposed to what it is wearing and what is in its
  // fist. Worn and held gear is applied imperatively from outside (WearItem /
  // EquipItem, because a piece of worn kit is a rig slot and a rig slot is not
  // something a JSON file can append). This is the other half: things the body
  // has on it that are not on its body, rolled once at Spawn and lootable off
  // the corpse.
  //
  // ROLLED, not fixed, and rolled from the MOB ID (Mob::RollLoot) so that two
  // runs of the same seed produce the same villager with the same purse. A
  // load does NOT re-roll — the saved list is the truth, the same rule
  // `loading_` already enforces for spawn-time rot.
  //
  // `count` is a number or a [min, max] pair; `chance` is 0..1 and defaults to
  // certain. ITEMS BY NAME (item.h's index hazard). An entry naming an item
  // the library does not have is dropped loudly at load, not at spawn: a
  // content typo should be one line in the log, not one line per villager.
  //
  // An array REPLACES down an `extends` chain and `"loot": null` clears it
  // (RFC 7396, game/sidecar.cpp MergePatch), so a character inherits the
  // human's pack unless it says otherwise and an effect can empty it.
  struct LootEntry {
    std::string item;
    int countMin = 1, countMax = 1;
    float chance = 1.0f;
    uint32_t dye = 0;         // game/dye.h packed colour, 0 = as authored
  };
  std::vector<LootEntry> loot;
  // ---- WHAT THIS CREATURE IS A COMPOSITION OF ------------------------------
  //
  // The two keys a def keeps from its own sidecar after the resolver has
  // finished with it: whose body this started from, and which modifiers were
  // poured on it. `name` is the identity; these two are the RECIPE, and they
  // are what lets the engine answer "is the zombie of jujunud already here?"
  // without a filename convention doing the reasoning.
  //
  // A creature read off disk fills them from its own file (`extends`,
  // `effects`). A creature MobSystem::DefWithEffects composed at runtime fills
  // them from the request, and is named `<base>+<effect>` — the one place a
  // name carries structure, and the reason a turned mob round-trips through a
  // save with no format change: LoadState reads the name back, does not find a
  // file, and composes exactly the same def again.
  std::string extendsName;             // sidecar `extends`, "" at the root
  std::vector<std::string> effects;    // resolved+accumulated `effects`
  MobRotDef rot;
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
  // Optional AI profile name from the sidecar's "behavior" key, resolved
  // against assets/mobs/behaviors.json AT SPAWN rather than at load — the
  // behaviour library hot-reloads on its own (R), and a def holding an INDEX
  // into it would rot the moment a profile was added above it. By name, like
  // every other cross-asset reference in this engine (CLAUDE.md design rule 4).
  // Empty = no AI: the creature keeps the legacy wander-and-avoid.
  std::string behavior;
  // THE POSE A CREATURE HOLDS WHILE IT IS COMING FOR YOU. A clip name (sidecar
  // or assets/anims), played by MobSystem::UpdateAnimation whenever the brain
  // has a target and is not idling, and weight-ramped down to nothing when it
  // does not — see Mob::chasePose_. Empty on every def but the undead, which is
  // the point: "arms outstretched" is a zombie's ANIMATION and not a zombie's
  // special case in code, so anything else that should stalk with a held pose
  // (a charging beast lowering its head) names its own clip and costs no C++.
  // Additive clips compose with the gait, so the body still shambles under it.
  std::string chaseClip;

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
  // The parts of this rig that ARE weapons (mob.h MobNaturalWeaponDef).
  // Merge-patch REPLACES an array, so a creature that wants different jaws
  // restates the whole block; one that only wants a different bite says so in
  // `bite` below and inherits these untouched.
  std::vector<MobNaturalWeaponDef> natural;
  // ...and what those jaws carry (mob.h MobBiteDef).
  MobBiteDef bite;
  // How much of an AIM effector's yaw the SPINE takes (Mob::ApplyAimPart).
  // A rig fact, not a tuning one: a creature with a neck like a heron turns
  // its head and a bull turns its whole body, and that is anatomy. The
  // avatar's head-look keeps its own `avatar.headLookSpine` slider — a
  // player's idle glance is a FEEL question and belongs in tuning.json.
  float aimSpineShare = 0.35f;
  // Where the eyes sit, as an offset from the head limb's ANCHOR (neck joint)
  // in art voxels, engine frame. Authored in the sidecar's top-level "eyeLocal"
  // array; converted to world voxels at load by ArtToWorld(). The camera rides
  // the midpoint between the eyes, so this is the centroid of the two.
  Vec3 eyeLocal{};
  bool hasEyeLocal = false;
  // DERIVED at load from eyeLocal + the head's rest-pose anchor position: the
  // eye's height above the creature's bottom in world voxels. Replaces the
  // hardcoded kEyeOffset for any creature that declares eyeLocal, so different
  // characters get different camera heights automatically.
  float eyeRestHeight = 0;

  int FindNatural(const std::string& n) const {
    for (size_t i = 0; i < natural.size(); i++)
      if (natural[i].name == n) return (int)i;
    return -1;
  }

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
  // The EXPOSED voxels (an empty 6-neighbour), as cells of the index box: the
  // contact stain pass sweeps this list, so a limb standing in a pool costs
  // its surface and not its volume. Built by the first contact after an index
  // (re)build, dropped with the index.
  std::vector<uint32_t> surface;
  // Voxels burnt away since the last collider rebuild. That rebuild is the most
  // expensive single operation in the feature, so it is batched hard.
  uint32_t removed = 0;
  // Voxels fire has taken off this limb over its WHOLE life. Never reset:
  // `removed` is a flush counter and goes to zero every rebuild, and a limb's
  // burnt fraction (Mob::RecountBurn) needs the matter that is no longer there
  // to count as burnt — a limb burnt to a stub is more burnt, not less, for
  // having fewer charred voxels left to count. Survives DropBurnIndex like
  // `alight`, and for the same reason: it is a fact about the lattice, not an
  // index into it.
  uint32_t burntAway = 0;
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
  // ON FIRE, which is NARROWER than `front`. The front is every voxel whose
  // material carries a self decay/emit rule, and fire is only one of the things
  // that puts a voxel there: blood dries, charred flesh crumbles, cooked flesh
  // rots, ice melts. So `front.size()` is "how much of this limb is chemically
  // busy", and reporting it as "burning" is what left a bled-on, long since
  // extinguished limb reading BURNING forever in the health screen. This is the
  // subset whose material is tagged `hot` — the voxels that are actually
  // alight. Recomputed wherever `front` is (the full sweep in BuildBurnIndex is
  // the exact authority; BurnOneLimb's candidate rebuild is as approximate as
  // `front` itself is there), and it SURVIVES DropBurnIndex for the reason
  // `alight` does: it is a fact about the lattice, not an index into it, and a
  // burning limb carves constantly, so a count that reset on every drop would
  // flicker the alarm off and on for the whole fire.
  uint32_t hotVox = 0;
  bool OnFire() const { return hotVox > 0; }
};

// One limb or part, described in the terms the burn pass needs.
//
// Pointers rather than a base class: MobSystem::Limb and PlayerAvatar::Part are
// independent structs owned by different systems and neither is going to grow a
// vtable for this. Exactly one of `skin`/`coll` is the AUTHORITATIVE lattice —
// a body with a finer skin derives its collider from the skin by majority-fill,
// so burning the collider would be writing to derived data and the next
// re-derive would silently undo it.
// ---- HEAT ACROSS A JOINT ---------------------------------------------------
//
// A world cell one of a creature's OTHER limbs is alight in. A creature's
// limbs are separate lattices that cannot see each other, and a mob is not in
// the grid, so until 2026-09-03 the only heat that ever crossed a joint was the
// `fire` gas a burning voxel emits into the grid -- which rises, and which
// every ignition rule treats as a weak igniter. A burning torso never lit the
// legs (owner report). Mob::BuildCrossLimbHeat builds this list once per
// creature per tick from every limb's burn front, at WORLD pitch (a cell with
// any alight voxel in it is hot, the same granularity the grid's own fire has),
// widened by the six face neighbours of each such cell so a joint that does
// not quite touch at world pitch still conducts. BurnOneLimb reads it wherever
// the grid holds air, for limbs other than `limb`, and treats the material as
// the neighbour -- through the ordinary authored table, at
// combustion.crossLimbPct of the authored chance when this is the only thing
// arming the rule. Sorted by `key` for binary search; a few dozen entries on a
// fully engulfed human. Rebuilt every tick and never saved: derived data.
struct CrossHeatCell {
  uint64_t key;   // CrossHeatKey(cell)
  IVec3 cell;     // world cell
  uint32_t mat;    // a burning material found there (tag:hot)
  // WHICH LIMBS, AS A BITMASK, and it has to be a mask rather than an index.
  // A world cell is 8x8x8 skin voxels, so the cell just outside a surface
  // voxel usually holds MORE OF THAT SAME LIMB; attributing each cell to one
  // limb would let a burning limb read its own voxels back as an external hot
  // neighbour, which is a self-sustaining ignition loop (rule 2) dressed as a
  // feature. A limb reads a cell only if some OTHER limb's bit is set. Limb
  // indices past 63 fold onto bit 63: two such limbs stop seeing each other,
  // which is a false negative and therefore the safe direction, and no
  // authored rig comes near it.
  uint64_t limbs;
};
inline uint64_t CrossHeatKey(IVec3 c) {
  return ((uint64_t)((uint32_t)c.x & 0x1FFFFFu) << 42) |
         ((uint64_t)((uint32_t)c.y & 0x1FFFFFu) << 21) |
         (uint64_t)((uint32_t)c.z & 0x1FFFFFu);
}

struct BurnLimbView {
  std::vector<PrefabVoxel>* skin = nullptr;  // skinScale units, int16
  std::vector<DebrisVoxel>* coll = nullptr;  // physScale units, int8
  uint32_t scale = 1;                        // lattice units per world voxel
  const BodyTransform* xf = nullptr;         // pose as the ANIMATION left it
  IVec3 size{};                              // collider extents, physScale units
  // The collider box's LOW corner, same units. Zero on a live limb (its
  // lattice starts at its own corner); a debris body's lattice is centred and
  // runs negative, so the corpse stain pass sets it (MobSystem::StainCorpses).
  IVec3 sizeMin{};
  uint32_t physScale = 1;
  int* microModel = nullptr;   // null / -1 = cube path, no brick to poke
  bool* carved = nullptr;      // latched when the brick becomes copy-on-write
  int* flipbook = nullptr;     // cleared on first damage; a frame swap heals
  BodyBurnState* burn = nullptr;
  // The creature's other limbs' heat (see CrossHeatCell). Null on anything
  // that is not a creature (debris, a corpse); `selfLimb` is this limb's index
  // in that list's owner, so a limb never reads its own cells; `crossPct` is
  // combustion.crossLimbPct, 0 = the list is ignored.
  const std::vector<CrossHeatCell>* crossHeat = nullptr;
  int selfLimb = -1;
  uint32_t crossPct = 0;

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
  // Put a voxel's WHOLE authored word back, art slot included — the one thing
  // Set() above deliberately will not do. Its only caller is the wound revert
  // below, and there the art slot is the point: a cut that heals has to give
  // back the paint it covered, or a bloodied patch of a painted creature
  // clears to flat material colour and the wound is still visible as a smear
  // of the wrong pink.
  void SetWord(size_t i, uint32_t word, uint32_t color) const {
    if (skin) {
      (*skin)[i].material = (uint16_t)word;
      (*skin)[i].color = (uint8_t)color;
    } else {
      (*coll)[i].payload = (uint16_t)word;
      (*coll)[i].color = (uint8_t)color;
    }
  }
  // The body COAT word (voxload.h BodyStain*: material + amount), read and
  // written on the same authoritative lattice the burn does.
  uint16_t Stain(size_t i) const {
    return skin ? (*skin)[i].stain : (*coll)[i].stain;
  }
  void SetStain(size_t i, uint16_t st) const {
    if (skin) (*skin)[i].stain = st; else (*coll)[i].stain = st;
  }

  // ---- A WOUND IS WET FLESH, NOT A POOL OF BLOOD (gore.woundHeals) --------
  //
  // Mob::StainWound rewrites the flesh around a cut to the creature's wound
  // MATERIAL, and this pass runs the ordinary authored reaction table over a
  // body's voxels. Blood's own rule in reactions.json is `decay -> air` at 8
  // per-mille a tick, authored so that a pool on the ground dries up and the
  // chunk goes back to sleep (rule 2) — and it applied, unchanged, to blood
  // that is INSIDE a limb. So every sword cut opened a hole that then ate
  // itself outward at a ~3 s half-life: the soak evaporated, the anatomy under
  // it showed through, and once enough had gone the geometry rules took the
  // limb off. The owner's report is "the blood voxels just entirely evaporate
  // revealing the below structure, which causes limbs to fall off".
  //
  // A pool drying and a wound drying are different events. Blood soaked into
  // meat does not leave a void behind it; it leaves the meat. So on a body:
  //
  //   * `woundSlow` divides the wound material's own decay chance, because a
  //     wound settles over a slower clock than a puddle in the sun, and
  //   * `revive` hands back the word that voxel held BEFORE the soak covered
  //     it, so the decay REVERTS the soak instead of removing the voxel.
  //
  // Both are off (woundMat 0, revive null, woundSlow 1) for anything that is
  // not a live creature's own limb, and for a creature whose def says its
  // wounds do not close — see MobDef::woundHeals. THAT IS THE UNDEAD SETTING:
  // with it off this is byte-for-byte the old behaviour, and a zombie's cuts
  // go on rotting outward and shedding its limbs, which is the one place the
  // bug was worth keeping.
  // ---- A SMALL TABLE, NOT ONE MATERIAL (2026-09-15) -----------------------
  //
  // This was one `woundMat` + one `woundSlow` while blood was the only thing a
  // wound could rewrite flesh to. The impact model gives a wound a SOURCE
  // (game/impact.h): a cut leaves the victim's blood, a bruise leaves
  // gore.bruiseMat, a zombie's bite leaves its infectMat -- three materials in
  // one limb, each of which must revert rather than evaporate, and the rot one
  // at its own slower clock (gore.infectHealSlow: a zombie's rot in living
  // flesh settles over minutes, not seconds).
  //
  // So it is a fixed three-entry table rather than a std::vector: this view is
  // rebuilt PER LIMB PER TICK by BurnTick, and a heap allocation there is the
  // reason `revive` is a raw function pointer six lines down. Three is what
  // there are; a fourth source of wound material would widen it here and
  // nowhere else.
  //
  // Entry 0 is the creature's own woundMat, so every reader that used to spell
  // this `v.woundMat` is asking WoundSlot(m) >= 0 instead, and off (no entry,
  // revive null) is byte-for-byte the old behaviour.
  static constexpr int kWoundMats = 3;
  uint32_t woundMat[kWoundMats] = {0, 0, 0};  // 0 = this slot unused
  uint32_t woundSlow[kWoundMats] = {1, 1, 1}; // divisor on the authored decay
  // Which slot `m` is, or -1. Ordinary linear scan over three entries, which
  // is cheaper than any structure that could replace it.
  int WoundSlot(uint32_t m) const {
    if (m == 0) return -1;
    for (int i = 0; i < kWoundMats; i++)
      if (woundMat[i] == m) return i;
    return -1;
  }
  // Returns false (and writes nothing) when this cell is not a remembered
  // soak; true fills the authored word and art slot and FORGETS the cell, so
  // one soak reverts once.
  using ReviveFn = bool (*)(void* ctx, IVec3 p, uint32_t& word,
                            uint32_t& color);
  ReviveFn revive = nullptr;
  void* reviveCtx = nullptr;
};

// ---- WHAT IS ON A BODY, AS A LEDGER -----------------------------------------
//
// The coat word is per VOXEL, and every question anyone actually asks about it
// is per LIMB or per BODY: is this creature covered in blood, is it soaked,
// how much of it is wet. Walking a 25k-voxel torso to answer that is a pass
// per question per tick, so it is walked ONCE at a bounded cadence
// (Mob::RecountCoat, tune.coat.recountTicks) and only when something actually
// changed a coat byte since the last walk. A clean body pays nothing (rule 2).
//
// The two HEAVIEST materials are kept and the rest is folded into the totals.
// Two rather than all of them because the ledger rides on every limb of every
// creature and a map per limb is not worth the allocation: a body is realistically
// bloody, or wet, or bloody and wet. Anything reading a specific substance off
// this reads it off `top`; anything reading "how coated is this" reads Frac().
struct CoatEntry {
  uint32_t mat = 0;      // the substance (a material id, not a palette slot)
  uint32_t sumAmt = 0;   // total amount of it over the limb, 0..15 per voxel
  uint32_t voxels = 0;   // voxels carrying it
};

struct LimbCoat {
  uint32_t voxels = 0;   // occupied lattice voxels — the denominator
  uint32_t stained = 0;  // of those, how many carry any coat at all
  uint32_t sumAmt = 0;   // total amount over EVERY material, `top` or not
  CoatEntry top[2]{};    // the two heaviest, by sumAmt, descending
  // 0 = clean, 1 = every voxel saturated with something.
  float Frac() const {
    return voxels ? (float)sumAmt / (float)(kBodyStainAmtMax * voxels) : 0.0f;
  }
};

// ---- ONE WORN SHELL, MARCHED -----------------------------------------------
//
// "Is something worn between this flesh and that fire?" asked of ONE shell: a
// dense index over its lattice (rebuilt whenever the voxel count moves --
// carving and burning both compact it), and a straight march from `from`
// along `dir` in the shell's own frame. Returns the material met, 0 for none.
//
// One implementation for both populations. A living creature's shells are rig
// slots (Mob::WornShellAlong walks them); a corpse's are bodies strapped to
// the piece they covered (MobSystem::CorpseWornAlong walks those). The
// question and the march are the same, so the code is.
struct ShellMarchIndex {
  IVec3 min{}, dims{};
  std::vector<uint16_t> mat;     // 0 = no voxel here
  size_t builtFor = (size_t)-1;  // voxel count the index was built from
};
uint32_t MarchShell(ShellMarchIndex& ix, const std::vector<PrefabVoxel>* skin,
                    const std::vector<DebrisVoxel>* coll,
                    const BodyTransform& xf, uint32_t scale, const Vec3& from,
                    const Vec3& dir, float dist, int maxSteps, Vec3* outAt);

// ---- A SPLASH OF BLOOD LOOKING FOR SOMETHING TO LAND ON ---------------------
//
// One tick's worth of a gout or a drip's spray, as the OTHER bodies see it.
// The droplets themselves are GPU particles that know nothing about limbs;
// this is the CPU's account of the same burst -- origin, axis, cone, speed
// and count -- queued by Mob::BleedTick and replayed by MobSystem::StainLimbs
// against every limb of every creature within `reach` (and by
// PlayerAvatar::PreTick against the player's own). The replay flies the SAME
// ARC the particle kernel does (speed, then sim.partGravity per tick), so a
// body is marked where the droplets are seen to land and nowhere else: a
// spray too slow to reach a face never marks it, however close. Never saved;
// lives for two ticks at most.
struct SplatterEvent {
  Vec3 origin{};
  Vec3 axis{0, 1, 0};   // unit-ish direction the burst is thrown along
  float cone = 0.5f;    // lateral spread as a fraction of the axis (gore.*Cone)
  float reach = 0.0f;   // world voxels a droplet is followed for (a cap)
  float speed = 0.0f;   // nominal launch speed, world voxels per SECOND
  int life = 0;         // ticks a droplet flies for (gore.microLifeTicks)
  int count = 0;        // droplets this tick
  // The MATERIAL being thrown (the bleeder's blood), not its palette slot: a
  // landed droplet writes a coat word, and a coat names a substance.
  uint32_t mat = 0;
  uint32_t amount = 0;  // amount per landed droplet
  uint64_t sourceMob = 0;  // the bleeder; its own bleeding limb is skipped
  int sourceLimb = -1;
  uint32_t tick = 0;
  uint32_t seed = 0;
  bool doneMobs = false;    // applied to MobSystem's creatures
  bool doneAvatar = false;  // applied to the player's avatar
};

// Everything BuildMobDef reads that is not the sidecar in front of it: the mob
// directory, the material table and the shared clip library. Defined in
// mob.cpp, because a MobSource and a clip-library entry are the loader's own
// vocabulary; held here only as a handle, so MobSystem can build ONE more def
// after the load without re-reading the directory (MobSystem::DefWithEffects).
struct MobDefFactory;

// Loads assets/mobs/*.vox + matching .json sidecars. Appends problems to log;
// defs that fail validation are skipped. Limb models of defs with "skinScale" > 1
// are packed into `micro` (which the caller uploads); `micro` is CLEARED first,
// so a hot reload rebuilds the whole pool rather than growing it forever.
//
// `factoryOut`, when given, receives the handle above. Hand it to
// MobSystem::SetDefFactory beside SetDefs: without it the system can spawn only
// the creatures that have files, and nobody can BECOME anything.
bool LoadMobDefs(const std::string& dir, const std::vector<MaterialDef>& mats,
                 std::vector<MobDef>& out, MicroBodySet& micro, std::string& log,
                 std::shared_ptr<MobDefFactory>* factoryOut = nullptr);

// EVERY MODIFIER THE CONTENT PUBLISHES: the stems of `<dir>/effects/*.json`,
// sorted, which is the vocabulary MobSystem::DefWithEffects' `fx` is drawn from.
// Not part of a def and not held anywhere — an effect is a file that gets poured
// on a body, so the only honest answer to "which ones exist" is the directory.
//
// It exists for the AUTHORING surfaces (the F1 NPC panel's effect checkboxes):
// a creature is a body plus modifiers now, so the thing that offers you a body
// has to be able to offer you the modifiers too, without a hand-kept list that
// goes stale the first time somebody writes `effects/burning.json`.
std::vector<std::string> MobEffectNames(const std::string& dir);

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
// A limb-lattice cell as one sortable key. The lattices are int16 (skin) and
// int8 (collider), so the bias covers both with room to spare.
inline uint64_t WoundWasKey(int x, int y, int z) {
  return ((uint64_t)(uint32_t)(x + 32768) << 34) |
         ((uint64_t)(uint32_t)(y + 32768) << 17) | (uint64_t)(uint32_t)(z + 32768);
}
// Ceiling on MobLimb::woundWas. A blade's soak is a few hundred voxels and
// each is remembered at most once (StainWound skips a voxel that is already
// the wound material), so this is only reached by a body that has been cut
// dozens of times in dozens of places — at which point the oldest soaks
// stopping being able to revert is the right way to run out.
inline constexpr size_t kWoundWasMax = 8192;

struct MobLimb {
  uint64_t body = 0;         // 0 = severed or never spawned
  uint64_t joint = 0;        // to parent
  // ---- A GARMENT IS NOT A SEPARATE OBJECT ----------------------------------
  //
  // >= 0 on an appended WORN SHELL: the index of the body limb it is strapped
  // to. A shell with a host is a FOLLOWER — kinematic in every phase, no joint
  // to its host, teleported onto the host's exact rigid offset by
  // Mob::DriveWornShells once per PostStep. It keeps its own slot, lattice, hp
  // and Jolt body (that is what a sword ray hits and what carries its damage),
  // it simply has no dynamics of its own while it is being worn.
  //
  // WHY, measured: as a dynamic body on a Fixed constraint it drifted off the
  // limb whenever the rig went limp at speed — a stiff constraint between two
  // deeply interpenetrating bodies of very different mass is the textbook way
  // to make a sequential-impulse solver both lag and gain energy, and the owner
  // report it comes from is "all of the clothes separate from limbs and it
  // becomes a crazy tangled mess ball". A follower cannot separate: its pose is
  // not solved for, it is derived.
  //
  // WHAT THIS COSTS, stated because it is a real behaviour change and not an
  // oversight: a kinematic body has no mass as far as the solver is concerned,
  // so an armoured ragdoll now tumbles with its FLESH inertia rather than with
  // 489 kg of iron. Weight still tells everywhere it is authored to — BodyMassKg
  // sums the shells, so a blast launches a plated body far slower — and the
  // alternative (folding each shell's mass into its host's mass properties) is a
  // separate change with its own gate. What was lost is a mass ratio across a
  // stiff constraint, which is the thing that was breaking.
  //
  // -1 on every body limb, on a held item (which is a foreign object aligned
  // hilt-to-socket, not a shell — see AppendHeldItem), and on a shell that has
  // left the creature. Also -1 with SANDVOX_NO_RIGWELD=1, the A/B arm, which
  // restores the jointed dynamic shell: "is a follower" and "has a host" are
  // deliberately the same question, so one env read switches every test of it.
  // Stable across RemoveAppendedSlots: a host is always a BASE limb, and the
  // shift only renumbers appended slots.
  int wornHost = -1;
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
  // Takes the SKIN scale: a MicroBodyRef is a render description, and so is
  // the dye — which is why a severed garment keeps its colour without anything
  // on the hand-off path knowing it exists.
  MicroBodyRef MicroRef(uint32_t skinScale) const {
    return microModel < 0
               ? MicroBodyRef{}
               : MicroBodyRef{(uint32_t)microModel, skinScale, dye};
  }
  Vec3 restOffset{};         // limb min corner from creature min corner (rest)
  Vec3 anchorRoot{};         // joint anchor from creature min corner (rest)
  Vec3 anchorLimb{};         // joint anchor in limb-local coords
  BodyTransform xf{};
  float bleedBudget = 0;
  // A child of this limb was cut off and the stump was never closed: the
  // wound at `woundLocal` is topped back up every tick (gore.stumpBleedsOpen)
  // so it drips hp until the creature is dead. Set on the PARENT by Sever(),
  // cleared when this limb itself comes off (its own parent opens instead).
  bool stumpOpen = false;
  Vec3 woundLocal{};
  // Dismemberment gout: counts DOWN from gore.severDecayTicks, emission
  // proportional to it, so the burst is front-loaded and tails off. Lives on
  // the PARENT limb (the stump), not on the piece that came off.
  int gushTicks = 0;
  Vec3 gushLocal{};
  Vec3 gushDir{0, 1, 0};
  // HIT FLASH: a briefly-lit limb, in LINEAR HDR units, added on top of the
  // material's own emission by the micro-body pass at shade time.
  //
  // PRESENTATION ONLY, and it lives here for the same reason `gushTicks` does:
  // the thing that knows a limb was struck is the limb. Set in Damage() and in
  // Sever(), decayed once per frame by DecayHitFlash() (frame time, not ticks —
  // a flash is a length of time the player perceives, not a number of
  // simulation steps), and read by AppendMicroInsts, which packs it into the
  // spare word the GPU instance already had (sim/microbody.h
  // MicroBodyInstGpu::pad0). Nothing in the sim reads it, it is not in the
  // voxel word (there are no spare bits), it is not saved, and it is not
  // hashed.
  float hitFlash = 0;
  // THE DYE this shell was worn in (game/dye.h). 0 on every body limb and on
  // every undyed piece, which is what the GPU's default word already means, so
  // nothing that does not wear a dyed garment changes at all.
  //
  // ON THE LIMB rather than on the WornPiece, even though it is a property of
  // the PIECE and every shell of one garment shares it. Two reasons, and the
  // second is the load-bearing one:
  //   * AppendMicroInsts walks limbs_ and has no piece in hand; going back to
  //     worn_ per limb per frame to ask "which garment is this shell" is a
  //     search inside a render loop for a value that never changes.
  //   * A SEVERED SHELL STOPS BEING PART OF A PIECE. It is handed to
  //     DebrisSystem with its lattice and its micro ref (DetachLimb), and the
  //     colour has to travel with it — a sleeve that turns grey the moment it
  //     is cut off is the exact bug the per-instance design is otherwise free
  //     of. The limb is the thing that survives that hand-off.
  uint32_t dye = 0;
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
  // ---- THE SAME PAIR, WEIGHTED BY WHAT THE VOXELS WERE MADE OF -------------
  //
  // `voxelsAtSpawn` / `voxelsCharged` count voxels; these sum
  // MobDef::WoundHpOf over the same set, so a bone voxel weighs three times a
  // skin one. The hp charge in Mob::CarveLimb is driven by THESE, and the raw
  // counts above stay exactly as they were because SEVERING still reads them:
  // a limb collapses on the fraction of its VOLUME that is gone, which is a
  // structural question and has nothing to do with what the volume was made
  // of. Keeping both is what let the depth weights land without touching
  // dismemberment at all.
  //
  // `brainCharged` is the count of BRAIN voxels already charged, and it is a
  // separate running total rather than a weight because brain is the one
  // material that costs an ABSOLUTE hp amount per voxel
  // (gore.brainHpPerVoxel) on top of its share of the volume.
  //
  // All three are deltas against the previous carve, for the same reason
  // `voxelsCharged` is: removals arrive from the carve predicate, the spall
  // rounds, the connectivity split and the collider re-derive, and a delta on
  // a total recomputed from the live lattice catches every one of them
  // without any of those paths knowing this accounting exists.
  float weightAtSpawn = 0;
  float weightCharged = 0;
  uint32_t brainAtSpawn = 0;
  uint32_t brainCharged = 0;
  // How many non-tissue voxels (bone) this limb's infection has COATED over
  // its life. Cumulative and never decremented, because the rot eats bone now
  // and so a census of coated bone falls while the coat is working perfectly
  // -- see the note at the increment in Mob::InfectStep. Diagnostic only:
  // nothing reads it but the `bite-infect` gate.
  uint32_t infectBoneCoated = 0;
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
  // ---- HOW MUCH FLESH THE PARENT HAS AT THIS JOINT -------------------------
  // The OTHER HALF of the same question, and the one no count taken on this
  // limb can answer: an arm is held on by the shoulder of the TORSO as much as
  // by the shoulder of the arm, and a rot that eats the torso's shoulder pocket
  // leaves this limb 100% intact, 100% "necked", and attached to nothing. Owner
  // report 2026-09-19: "all of the voxels connecting a shoulder to the torso
  // can get rotted off but the limb is still attached".
  //
  // Voxels of the PARENT within gore.woundNeckRadius of the joint, measured on
  // the parent's lattice with the centre CLAMPED into the parent's bounding box
  // (the shoulder anchor of an arm sits several cells outside the torso's voxel
  // cloud — the same clamp Mob::InfectAcrossJoint makes, for the same reason).
  // Held on the CHILD because the joint is the child's: a parent has many.
  //
  // 0 = not taken yet; kSocketUnmeasured = taken and found empty at spawn, i.e.
  // this rig's geometry gives no parent-side sample and the test is skipped
  // rather than being permanently satisfied.
  static constexpr uint32_t kSocketUnmeasured = 0xFFFFFFFFu;
  uint32_t socketAtSpawn = 0;
  // ---- HOW MUCH SKIN THIS LIMB HAS ------------------------------------------
  // Burnable voxels with at least one open face, on the authoritative lattice,
  // taken lazily on the first burn recount (0 = not yet taken; floored at 1).
  // The DENOMINATOR of the burnt fraction (Mob::RecountBurn), and a surface
  // rather than a volume for the reason burns are graded by body-surface area
  // in the first place: fire chars the outer layer and the char is inert, so
  // it shields everything under it — measured, a human stood in a fire for
  // 1200 ticks converged at 30% of its burnable VOLUME with 5,900 raw skin and
  // 7,000 raw flesh voxels left under a black shell, and would have stood
  // there forever. Against its surface that same body is burnt through.
  uint32_t surfaceAtSpawn = 0;
  // ---- WHAT THE BLOOD COVERED (BurnLimbView's wound-revert note) ----------
  // One entry per voxel Mob::StainWound rewrote to the wound material, holding
  // the word and art slot that voxel had BEFORE the soak. The wound's decay
  // then puts flesh back instead of leaving a hole. Sorted by WoundWasKey, so
  // the lookup on the decay path is a binary search over a few hundred
  // entries; an entry is erased the moment it is spent, so a limb that has
  // finished drying carries nothing.
  //
  // A SPARSE AUXILIARY LAYER, for the reason the design guidelines give: the
  // voxel word is full (there is nowhere to put "what I used to be"), this is
  // keyed by limb, it is reconstructible-as-nothing (losing it only means a
  // soak stops being able to revert, never that geometry is wrong), and it is
  // bounded by kWoundWasMax whatever a player does with a sword.
  //
  // NOT SAVED, and not severed with the limb. Both are the same trade and both
  // fail the same way: a soak with no entry behind it goes back to decaying to
  // air, which is the OLD behaviour -- bounded, and it lets the limb go quiet
  // again (rule 2), which is what a "keep the voxel forever" fallback would
  // not. So the cost of losing the table is the few seconds of one wound's
  // drying, in two narrow cases: saving within about six seconds of being cut,
  // and a limb that was severed with its soak still wet (a part on the ground
  // is debris and has never had a table at all).
  struct WoundWas {
    int16_t x, y, z;
    uint16_t word;   // material | variant<<12, exactly as the lattice held it
    uint8_t color;   // the art slot StainWound zeroes
  };
  std::vector<WoundWas> woundWas;
  // ---- WHAT BIT THIS LIMB ---------------------------------------------------
  //
  // The material a BITE rewrote this limb's exposed flesh to (0 = none). It is
  // remembered on the limb rather than derived, because the wound-revert table
  // has to know which materials on THIS limb are "a wound settling" as opposed
  // to "a material decaying", and the only thing that knows a zombie's rot is
  // in this arm is the bite that put it there. Latched: a second bite by
  // something else overwrites it, which is the honest answer -- the table can
  // only carry so many, and the freshest infection is the one still spreading.
  //
  // Read by Mob::ViewOf, which arms the revive for it at gore.infectHealSlow.
  // Never saved: a loaded body's rot is already in its lattice, and it heals
  // at the ordinary rate from then on rather than not at all.
  uint16_t infectMat = 0;
  // ...AND WHAT IT LOOKS LIKE WHERE IT HAS EATEN THROUGH TO THE BONE.
  //
  // The LIQUID the bite smeared over the hole (a zombie's `bite.stain`, i.e.
  // ichor -- 0 = none / not a liquid the palette can draw). Latched beside
  // `infectMat` for one reason: the rot EATS FLESH AND LEAVES BONE, and bone
  // is the one thing in the lattice it can neither convert nor remove, so a
  // limb rotted through used to end up as a white anatomical bone sitting in a
  // green wound. The infection pass coats what it exposes (Mob::InfectStep),
  // and the substance it coats it with is a mix of this and the victim's own
  // blood. Without the latch the pass would have to guess, because by the time
  // the rot reaches bone the bite that carried the stain is long gone.
  uint16_t infectStain = 0;
  // THE INFECTION CARRIES NO CLOCK STATE, deliberately. The first version held
  // two fractional accumulators here and spent them in BURSTS of ~32 lattice
  // voxels, to amortise the O(limb) sweep each conversion needs. That is a
  // defensible cost argument and it produced a visibly wrong result: a third of
  // a second's worth of rot appearing all at once, every few seconds, in one
  // slab. A disease does not advance in steps. Mob::InfectTick now rolls an
  // independent chance every tick instead and holds nothing between them -- see
  // the note above that function for why the rate still comes out exact.
  // Per-voxel burning / dissolution (see BodyBurnState above).
  BodyBurnState burn;
  // PULPED TISSUE IS DISSOLVING. Set by Mob::BluntHit when a blow earns a
  // dent (ripeness > pulpCarveFrom) and cleared by BluntPulpTick when no
  // pulped voxels remain. While true the dissolution tick eats blood-coated
  // voxels at gore.pulpRotRate, one at a time, in a noisy pattern -- the
  // same per-tick Bernoulli draw the infection uses. No clock state for the
  // same reason InfectTick carries none (see the note above that function).
  bool bluntPulp = false;
  // What is ON this limb, recounted at a bounded cadence (see LimbCoat).
  // Index-parallel by construction because it rides the limb itself, which is
  // what RemoveAppendedSlots moves wholesale.
  LimbCoat coat;
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
// THE SHAPE ITSELF MOVED OUT (2026-09-19, phys/kerf.h). It is not a fact about
// living flesh: a corpse is a DebrisSystem body and a sword meeting one used to
// bore the sphere this type exists to replace. Both carves now build the slot
// from one place, and this stays the name every call site here already uses.
using BladeCut = KerfCut;

// ---- ONE BLUNT HIT ----------------------------------------------------------
//
// The argument to Mob::BluntHit, and the sister of BladeCut above. Where a
// kerf is a SLOT with a direction, trauma is a POINT with a magnitude: a mace
// does not care which way its head was travelling, only how fast and how much
// of it there was. So there is no edge axis and no cut direction here, and
// that absence is the whole difference between the two wounds.
//
// WHAT IT DOES AND DOES NOT DO (docs/PLAN_impact_unarmed.md §2 step 4):
//
//   * hp falls, the flinch fires, the creature cries out -- through the
//     ordinary Mob::Damage, so nothing new decides when something dies.
//   * a BRUISE is stained on, in gore.bruiseMat rather than in the victim's
//     blood: the material a wound rewrites flesh to is a property of the
//     STRIKE, not of the struck (plan S6).
//   * the bleed budget is topped up at gore.bluntBleedScale of a cut's rate.
//     A punch does not open you.
//   * only if `carve` > 0 does any voxel LEAVE, and then as a shallow radial
//     DENT inside a BluntCarveScope, which is what refuses the collapse sever
//     and the blade rules. A blunt hit NEVER takes a limb off, however many
//     land -- that was the owner's spec in one line.
//   * against a WORN shell it breaks shell voxels in proportion to
//     `armorBreak` and TRANSMITS gear.bluntThrough of itself to the limb
//     underneath. This is what makes a mace the answer to plate.
struct BluntHit {
  Vec3 at{};           // contact point, world voxels
  float hp = 0.0f;     // trauma to charge, ALREADY scaled by swing power
  float power = 0.0f;  // 0..1 speed x edge alignment, for the radii below
  float carve = 0.0f;  // 0..1 of gore.bluntCarveRadius dented out of FLESH
  float armorBreak = 0.0f;  // 0..1 of gear.bluntDentRadius broken off a SHELL
  float impactSpeed = 0.0f; // world voxels/sec, for the knock-loose rule
  uint32_t seed = 0;        // bruise draw key; see BladeCut::seed
  bool unarmed = false;     // true for natural weapons (fist/jaw), selects
                            // the unarmed overrides in Tuning::Gore
};

// ---- ONE BITE ---------------------------------------------------------------
//
// The argument to Mob::BiteHit. A TEAR: a correlated-noise blob torn out of
// the limb (the same predicate Mob::RotAtSpawn draws the undead's holes with,
// shared through Mob::CarveBlob so there is one blob and not two), bleeding
// like a cut, severing only by COLLAPSE -- enough bites really do take a hand
// off, which a punch must never do, and that difference is exactly one rule.
//
// THE INFECTION IS THE INTERESTING PART. If the biter carries one and the tear
// reached FLESH -- not a shell, not bone -- the exposed tissue is rewritten to
// `infectMat` and `infectStain`'s stain is smeared over the hole. Armour in
// the way means no infection at all, which is the entire reason the struck
// slot is classified (impact.h StruckKind) before any of this runs.
struct BiteHit {
  Vec3 at{};           // contact point, world voxels
  float hp = 0.0f;     // damage to charge, ALREADY scaled by swing power
  float power = 0.0f;  // 0..1, scales the tear's radius
  uint16_t infectMat = 0;    // material the tear rewrites exposed flesh to
  uint16_t infectStain = 0;  // LIQUID whose stain it smears over the hole
  uint32_t seed = 0;         // tear + stain draw key
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
// ---- ONE STACK IN A CREATURE'S PACK -----------------------------------------
//
// Deliberately NOT an ItemStack (game/item.h). An ItemStack holds a LIBRARY
// INDEX, which is file order and dies on an R reload — fine for the player's
// kit, which main.cpp re-validates on every reload, and wrong for something
// that travels through a corpse report, a save file and a network packet. A
// name resolves late, once, at the moment somebody actually wants the item.
//
// The same three fields the hotbar and the bag merge on, for the same reason:
// a stack is one item in one colour (game/dye.h).
struct CarriedItem {
  std::string item;
  int count = 1;
  uint32_t dye = 0;
};

// ---- WHAT A CORPSE STILL HAS ON IT --------------------------------------------
//
// Die() hands every limb to DebrisSystem and the husk is swept out of mobs_ on
// the next PreTick, so one tick after a creature falls there is no Mob left to
// ask "what was it wearing". The shells are still THERE — a robe on a corpse is
// a debris body jointed to the torso it fell with — but debris carries no
// identity (game/worlditems.h's argument), so without this a corpse is a heap
// you can carve and burn but not loot.
//
// This is the one-time report Die() makes the instant before the handover,
// while the rig still knows which appended slot was the robe: the bodies the
// corpse became (any of them under the crosshair means "this corpse"), and one
// entry per piece of gear with the body that IS the piece — the identity shell
// for a worn piece (the same panel ItemGroundVoxels drops, IdentityShellOf),
// the borrowed slot for a held item — plus the piece's other shells, which are
// rags that leave with it, and its damage captured one call before the shells
// forget it (CaptureWorn). Delivered through MobSystem::SetOnCorpse to whoever
// keeps the registry (game/corpses.h); the Mob keeps nothing.
//
// NOT fired for the avatar: the player's kit lives in PlayerKit and the wear
// loop re-dresses the respawned rig from it, so their own corpse holding a
// second copy would be a duplication machine.
struct CorpseReport {
  uint64_t mobId = 0;
  std::string def;                  // MobDef::name, for the prompt
  std::vector<uint64_t> bodies;     // every limb body, shells included
  struct Piece {
    std::string item;               // by NAME (item.h's index hazard)
    int equipSlot = -1;             // -1 for the held item
    bool held = false;
    // THE BODY THAT IS THE PIECE — or 0, WHICH MEANS IT WAS IN THE PACK.
    //
    // A worn or held piece is a real object lying in the heap: you loot it by
    // taking its body OUT of the world. A carried stack (MobDef::loot) never
    // had a body — it was a line in the creature's pack — so it is looted by
    // deleting the entry and nothing else.
    //
    // One list rather than two because the LOOT PANEL ADDRESSES ENTRIES BY
    // INDEX (KitRef{KitSpace::Loot, i}, ui/inventory_ui.cpp): a second vector
    // would mean every consumer — the mirror, the take, the take-all loop, the
    // drag-out — splitting one index space across two containers, which is
    // four places to get an off-by-one wrong for no gain. The branch is
    // `if (piece.body)`, and it appears exactly where a body is touched.
    uint64_t body = 0;
    int identityCover = -1;         // that body's ItemCover index (worn only)
    std::vector<uint64_t> rags;     // the piece's other shells
    WornDamage damage;              // as it was at death
    // How many. Always 1 for worn and held gear — a rig slot is one garment —
    // and the rolled count for a carried stack.
    int count = 1;
    // The COLOUR it was (game/dye.h), read off the identity shell. Travels for
    // the same reason `damage` does: what you loot off a body has to be the
    // thing that was on it. Without this a red tunic on a corpse loots into
    // your pack undyed, which reads as the item having been swapped.
    uint32_t dye = 0;
  };
  std::vector<Piece> gear;
};

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
  bool Swinging() const { return swinging_; }
  void SetSwinging(bool v) { swinging_ = v; }
  // WHY it died, as a static string, or "" while alive. Set at every Die()
  // call site that knows (a vital limb lost, blood loss, the burn cap), so a
  // gate that finds a corpse can say what killed it instead of guessing
  // between four mechanisms that all end in the same ragdoll.
  const char* DeathCause() const { return deathCause_; }
  const MobDef* Def() const { return def_; }
  // The shared services this creature was spawned against. Public for the same
  // reason `StyleUsable`/`StyleReachOn` are free functions declared in
  // strokes.h: the style vocabulary asks questions of a creature, and the
  // answers happen to live one indirection away. Never null on a spawned mob.
  MobSystem* Sys() const { return sys_; }
  Vec3 Origin() const { return origin_; }
  float BodyY() const { return bodyY_; }

  // ---- WHO STEPS THIS CREATURE (docs/PLAN_multiplayer_m9.md M9.4-B) --------
  //
  // A playerId. `kLocalOwner` (0) is session 0's id and the default, so a
  // single-player process is every mob owned by the only player there is and
  // the whole of this feature is inert — which is what keeps the world hash
  // where it was: with no ownership function set (MobSystem::SetOwnershipFn),
  // `IsGhost()` is false everywhere and PreTick runs exactly the branches it
  // ran before.
  //
  // A GHOST is a mob somebody else steps. It keeps its rig, its bodies and
  // its place in the crowd, and it is posed from received transforms alone —
  // no AI, no locomotion, no animation, no bleeding, no burning, no staining,
  // and above all NO OPS. Two machines both stepping one creature would
  // author two sets of blood ops for one wound, which is the entity half of
  // "one producer per cell" (net::Authority, M9.4-A).
  uint32_t Owner() const { return owner_; }
  // Out-of-line: the answer is `owner_ != sys_->LocalPlayerId()` and MobSystem
  // is not complete yet here. An unparented mob (no `sys_`) is never a ghost —
  // the fixtures that build a Mob by hand have nobody to be a ghost of.
  bool IsGhost() const;

  // ---- THE PER-MOB RECORD --------------------------------------------------
  //
  // One creature's damage, carve state and rig geometry, in the bytes
  // `MobSystem::SaveState` has always written — this IS the body of that
  // loop, lifted out so the handoff and the save file cannot drift apart
  // (`save-entities` gates the format; `mob-handoff` gates the round trip).
  //
  // What it does NOT carry, on purpose: the id, the brain, the gear. The id
  // because a save re-spawns into a fresh counter; the other two because a
  // save has no use for them and widening the format would move the world
  // hash for a network feature. The handoff carries all three beside the
  // record (net/mobsync.h).
  void SaveOne(ByteWriter& w) const;

  // ---- LIVE RAGDOLL: limp, then back on its feet (sim/tuning.h Ragdoll) -----
  //
  // A creature that is knocked down is not a corpse. Die() flips every limb
  // dynamic and HANDS IT TO DebrisSystem — there is no way back from that.
  // This is the same flip with the limbs kept: every body stays owned by the
  // mob, its joints stay, its collision group stays, and the animation
  // pipeline simply stops re-posing it (SubmitPose skips a limp limb the way
  // it skips a sever hold), so PostStep's read-back is what places it. When
  // the pelvis has come to rest the limbs are made kinematic again where they
  // lie and a procedural get-up blends each one from there into the ordinary
  // standing pose (RagdollPhase::GetUp, the SubmitPose blend).
  //
  // Reachable from three places and nothing else: a blast (BlastRadial),
  // freefall past ragdoll.fallSeconds (MobSystem::UpdateFall / the avatar's
  // own air clock), and the dev panel. Rendering, bleeding, burning and
  // carving all keep working on a limp body because none of them ever asked
  // whether a limb was kinematic.
  enum class RagdollPhase : uint8_t { None, Limp, GetUp };
  RagdollPhase Ragdoll() const { return ragdoll_; }
  bool Ragdolled() const { return ragdoll_ != RagdollPhase::None; }
  // Go limp. `minSeconds` is how long the body stays down before the settle
  // test may stand it up (a blast passes ragdoll.minSeconds, the dev button
  // ragdoll.devSeconds). No-op on a dead creature or one already limp; a
  // creature mid-get-up drops again. `why` is a static string for the log.
  void StartRagdoll(float minSeconds, const char* why);
  // Set every live limb's linear velocity (voxels/s). The blast launch and
  // the mid-air flip both use it: one uniform velocity across the rig, so the
  // joints are not yanked by a per-limb impulse/mass spread.
  void SetLimbVelocities(Vec3 velVoxPerSec);
  // The blast entry point. `impulseKgMs` is the impulse at the centre,
  // falling off linearly to zero at `radiusVoxels`; launch speed is that over
  // the rig's mass, clamped to ragdoll.maxLaunchSpeed and ignored below
  // ragdoll.blastMinSpeed. Returns true if the creature was knocked down.
  bool BlastRadial(Vec3 centerWorldVoxel, float radiusVoxels, float impulseKgMs);
  // Sum of the live limbs' Jolt masses, kg.
  float BodyMassKg() const;
  // ---- WORN SHELLS RIDE THEIR LIMBS, IN EVERY PHASE -----------------------
  //
  // Put every attached shell exactly on its host limb (MobLimb::wornHost), by
  // the same two steps AppendWornShell placed it with: reach the host's anchor
  // through the host's live transform, then back off to this shell's own
  // corner. A teleport, not a kinematic drive, and it carries the host's
  // velocities so the garment's contacts and its ray proxy agree with the limb
  // inside it mid-step as well as at the end of one.
  //
  // Called once per tick from PostStep, AFTER the read-back, so the host
  // transform it derives from is the one that will be rendered this frame —
  // whether Jolt placed it (limp) or the pose pipeline did (everything else).
  // This is the ONLY thing that poses a shell; SubmitPose skips them.
  void DriveWornShells();
  // The limb this slot is strapped to, or -1 if it is not a follower at all —
  // MobLimb::wornHost, for a caller outside the class. NOT a synonym for
  // IsWornSlot: that asks "is this wardrobe rather than anatomy" (by tag, true
  // of a shed rag mid-hold too), this asks "is this slot's pose derived from
  // another slot's, and from which".
  int WornHostOf(int slot) const {
    return slot >= 0 && slot < (int)limbs_.size() ? limbs_[slot].wornHost : -1;
  }
  // Where the creature IS: the root limb's live body origin, or origin_ when
  // the rig has no root body. Mob::origin_ is the walk driver's anchor and
  // stops meaning anything while the body is limp.
  Vec3 RootWorldPos() const;
  // Seconds in the current ragdoll phase.
  float RagdollSeconds() const { return ragdollT_; }

  // ---- THE ARREST: how much velocity a limp body just lost ----------------
  //
  // The ragdoll twin of Player::impactDeltaV. A limp body has no controller and
  // no sweep — the capsule is teleported onto the pelvis with its velocity
  // zeroed every tick (PlayerAvatar::RagdollFollow) — so the ONLY thing that
  // knows a ragdoll hit the ground is the solver, and the only trace it leaves
  // is the velocity it refused. DECELERATION ONLY, so neither gravity nor a
  // blast launch can be read as a landing; summed over one short braking RUN
  // rather than held per tick, because a jointed rig takes two or three ticks
  // to stop where the player's AABB takes one. The full reasoning, the two
  // bounds that keep it from becoming an accumulator, and why it is measured on
  // the rig's centre of mass are at Mob::TickRagdollArrest in mob.cpp.
  //
  // ONE LANDING, ONE BILL. A rig does not hit the floor once: measured, a
  // 15 m/s limp landing braked hard, folded, and braked hard AGAIN as the torso
  // came down after the legs -- two events of 14.8 and 15.0 m/s, billed
  // separately, for three times the damage the same speed costs a walking
  // player. So the peak is HELD until the rig has stopped braking for
  // kArrestSettleTicks and only then handed over; asking mid-impact returns
  // nothing. 0.1 s of latency on a fall-damage number nobody can see arrive.
  //
  // VOXELS per second, the same units ApplyFallDamage wants.
  static constexpr uint8_t kArrestSettleTicks = 3;
  Vec3 TakeRagdollImpact() {
    if (ragdollArrestQuiet_ < kArrestSettleTicks) return Vec3{};
    const Vec3 v = ragdollImpact_;
    ragdollImpact_ = Vec3{};
    return v;
  }
  // Continuous freefall so far (NPC gravity; the avatar keeps its own clock).
  float AirSeconds() const { return airTime_; }
  // Rig size INCLUDING a borrowed item slot (limbDefs_ tracks limbs_).
  int LimbCount() const { return (int)limbDefs_.size(); }
  const MobLimbDef& LimbDefAt(int i) const { return limbDefs_[i]; }
  // LIVE hp of one rig slot, INCLUDING the appended ones. A held weapon is a
  // borrowed slot with its own hp (item.h ItemDef::hp), which is what a parry
  // charges, and there was no way to read it from outside. -1 for a slot that
  // does not exist, so a caller that does not check cannot mistake "gone" for
  // "unhurt". PlayerAvatar::PartHp is the same reading, kept for its callers.
  float LimbHpAt(int i) const {
    return i >= 0 && i < (int)limbs_.size() ? limbs_[i].hp : -1.0f;
  }

  // ---- blood is health; burns cap it (sim/tuning.h Gore §F/§G) -------------
  // Summed hp of the creature's live AUTHORED limbs — the thing that bleeds
  // out and the thing the burn cap clamps. A held item or a worn shell has hp
  // of its own (a parry charges it) and is not life, so it is excluded, which
  // is what makes "total hp reaches zero" mean "dead" and not "sword broke".
  float TotalHp() const;
  // Blood that has left this body in its life, in whole-voxel equivalents
  // (a micro droplet is 1/microScale^3 of one). Diagnostic and gate readout.
  float BloodLost() const { return bloodLost_; }
  // Take `voxels` of blood out of the creature: charges
  // voxels * gore.bleedHpPerVoxel across the live authored limbs in proportion
  // to what each still has, and kills the creature through Die() when the
  // total is gone. Returns false if it died. Every caller that emits blood —
  // the drip, its spray, the arterial gout, the thrown sever voxels — goes
  // through here, and NOTHING else may: "every drop is hp" is only true if
  // there is one door.
  bool DrainBlood(float voxels);
  // Burnt fraction of the body (0..1) as last recounted, and the health cap it
  // sets (1 = unburnt, 0 = dead of burns). Both derived from the lattice by
  // RecountBurn; neither is saved.
  float BurnFraction() const { return burnFrac_; }
  float BurnHealthCap() const { return burnCap_; }
  // The curve itself, exposed so a gate can assert the arithmetic without a
  // fixture and the HUD can draw it: piecewise-linear through (0, 1),
  // (burnCapMidFraction, burnCapMidHealth), (burnDeathFraction, 0).
  static float BurnHealthCapFor(float burntFraction);
  // How burnt a MATERIAL reads, by name: 0 = intact, 1 = half (cooked /
  // alight), 2 = whole (charred / ash / cinder). The one list every consumer
  // of "is this voxel burnt" reads — MobSystem's per-id table, and the HUD's
  // limb readout in main.cpp — so the two cannot disagree about ash.
  static uint8_t BurnStageOfMaterialName(const std::string& name);

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
  // ---- THE BLUNT PATH (game/impact.h, BluntHit above) ----------------------
  // Charge trauma to a live limb without opening it: hp, a bruise, at most a
  // shallow dent, and never a sever. On a WORN slot it breaks shell voxels and
  // transmits a share to the host limb underneath. Returns true when the
  // handle was one of this creature's live limbs.
  bool BluntHit(uint64_t bodyHandle, const ::BluntHit& hit, World& world,
                std::vector<ParticleSpawn>& spawns);
  // ---- THE BITE PATH (game/impact.h, BiteHit above) ------------------------
  // Tear a blob out of a live limb, bleeding like a cut and severing only by
  // collapse; rewrite what it exposed to the biter's infection when there is
  // one and the tear reached flesh. Returns true when the handle was one of
  // this creature's live limbs.
  bool BiteHit(uint64_t bodyHandle, const ::BiteHit& hit, World& world,
               std::vector<ParticleSpawn>& spawns);

  // ---- "THIS CARVE IS A DENT" ----------------------------------------------
  //
  // RAII around a CarveLimb reached from BluntHit. Two rules read it, and both
  // are the same statement: A BLUNT HIT NEVER TAKES A LIMB OFF.
  //
  //   * the COLLAPSE sever (CarveLimb's "carved down past being a limb at
  //     all") is skipped, so a face can be caved in past the point where a
  //     blast would have shed it;
  //   * the blade rules are unreachable anyway (`inBladeCut_` is false), and
  //     that is deliberately NOT restated here -- one flag, one meaning.
  //
  // What it does NOT suppress is the crater STAIN: the dent is still soaked in
  // the victim's own woundMat, because "deletes voxels and replaces them with
  // gore" is exactly what the owner asked a gauntlet to do. The BRUISE is a
  // separate, wider stain in gore.bruiseMat laid on before the dent.
  //
  // hp reaching zero on a vital limb still kills (a caved-in skull), because
  // that path is HpZeroSevers and is about DEATH rather than about amputation.
  struct BluntCarveScope {
    bool& f;
    bool prev;
    bool& u;
    bool uprev;
    BluntCarveScope(Mob& m, bool unarmed = false)
        : f(m.inBluntCarve_), prev(m.inBluntCarve_),
          u(m.inUnarmedBlunt_), uprev(m.inUnarmedBlunt_) {
      f = true;
      u = unarmed;
    }
    ~BluntCarveScope() { f = prev; u = uprev; }
  };

  // ---- "THIS CARVE IS A TEAR" ----------------------------------------------
  //
  // RAII around a CarveLimb reached from BiteHit. The blade rules (cut-through
  // and the neck) stay off, because a mouth is not an edge and neither rule has
  // a direction to read -- but the COLLAPSE sever is deliberately LEFT ON:
  // enough bites DO take a hand off, and that is the one line separating a bite
  // from a punch.
  //
  // It exists as a flag at all rather than as nothing because the bite must not
  // silently inherit a scope somebody else left standing, and because the
  // infection rewrite below needs to know the carve it is soaking was a tear.
  struct BiteScope {
    bool& f;
    bool prev;
    explicit BiteScope(Mob& m) : f(m.inBite_), prev(m.inBite_) { f = true; }
    ~BiteScope() { f = prev; }
  };

  // Detach a limb now. Root/vital kills instead.
  void Sever(int limbIndex);
  void Die();
  // LAST LOOK AT A LIVING RIG. Called from Die() after the cause is recorded
  // and before a single limb is handed to DebrisSystem — the only instant at
  // which "what state was this body in when it died" is still answerable, and
  // it lasts one statement: the loop below zeroes `anim_.partAlive`, moves the
  // skin voxels out and drops every body handle, after which every per-limb
  // readout in the engine reports a severed, empty, zero-hp limb.
  //
  // Default does nothing (an NPC corpse is read through CorpseReport instead).
  // PlayerAvatar overrides it to photograph the HUD mirror for the death
  // screen (game/avatar.h).
  virtual void OnDying() {}
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
  // Age THIS creature's hit flashes (MobLimb::hitFlash). Split out of
  // MobSystem::DecayHitFlash because the avatar is a Mob that lives OUTSIDE
  // mobs_ (main.cpp owns it directly): the system loop never reached it, so a
  // severed player limb left its stump lit at combatfx.flashSever forever.
  // Every driver that runs the other upkeep above must run this beside it.
  void DecayHitFlash(float dt);
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

  // ---- WHAT IT IS CARRYING (MobDef::loot) ---------------------------------
  //
  // The pack: stacks that are on the creature without being on its body. No
  // rig slot, no shell, no brick, no physics — which is the whole difference
  // between this and `worn_`, and the reason a loot table can be pure data
  // while a suit of armour cannot (MobDef::LootEntry).
  //
  // Everything a body does with these is a list operation: Die() copies them
  // into the corpse report, a rising carries them to the creature that gets
  // up, and the save writes them. Nothing here is drawn or simulated.
  const std::vector<CarriedItem>& Carried() const { return carried_; }
  // Merges into an existing stack of the same item AND DYE, exactly as the
  // bag and the hotbar do (item.h ItemStack::dye: a stack is one colour).
  // Refused past kMaxCarried, like every other bounded per-mob list here.
  bool AddCarried(const std::string& item, int count = 1, uint32_t dye = 0);
  // Takes `count` off entry `index` (all of it by default), erasing the entry
  // when it empties. Returns what actually came off — 0 for a bad index, so a
  // stale mirror is reported rather than clamped (the rule corpses.h's
  // LootResult::NoSuchPiece states).
  int TakeCarried(int index, int count = -1);
  void ClearCarried() { carried_.clear(); }
  void SetCarried(std::vector<CarriedItem> v) { carried_ = std::move(v); }
  // Roll this creature's def-authored loot table into `carried_`. Called once
  // by MobSystem::Spawn and keyed on the mob id alone, so it is a pure
  // function of identity: a replay, a reload-from-seed and the other machine
  // all produce the same purse. NOT called on a load — the saved list is the
  // truth there, the same rule `loading_` already enforces for spawn rot.
  void RollLoot();

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
  // `dye` is the colour this particular garment is (game/dye.h), 0 for undyed
  // — which is every piece that is not a commoner weave, and every caller that
  // predates the wardrobe. It is carried onto each shell rather than stored on
  // the piece; see MobLimb::dye for why.
  bool WearItem(const ItemDef* item, int equipSlot,
                const WornDamage* damage = nullptr, uint32_t dye = 0);
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

  // ---- GEAR THAT LEFT THE BODY BY FORCE ---------------------------------
  //
  // A worn piece whose IDENTITY shell (the panel a dropped copy of the item
  // is made of — ItemGroundVoxels, game/worlditems.h) is cut loose leaves the
  // wardrobe: its other shells fall with it as rags, the WornPiece entry is
  // gone, and the shell on the ground is registered as the ITEM through
  // MobSystem::SetOnItemShed so `E` can pick it up. A sword knocked from the
  // hand takes the same path. Either way the creature's OWNER has bookkeeping
  // to do that this class cannot see — the player's equipment slot, sheath
  // and damage record live in PlayerKit — so the loss is reported here and
  // drained by whoever owns the kit (main.cpp for the avatar). NPCs are
  // never drained; the list is capped so it cannot grow.
  //
  // `damage` is the piece as it was the instant before it came off, so a
  // piece picked back up and re-worn has exactly the holes it had. A sleeve
  // cut off EARLIER is not here: that shell is already a rag on the floor,
  // and the piece went on being worn without it.
  struct LostGear {
    int equipSlot = -1;     // -1 for the held item
    std::string item;
    bool held = false;
    WornDamage damage;      // empty for the held item
    uint32_t dye = 0;       // the colour it was (game/dye.h), 0 for undyed
  };
  const std::vector<LostGear>& LostGearEvents() const { return lostGear_; }
  void ClearLostGear() { lostGear_.clear(); }
  // Which worn piece owns rig slot `slot`, or -1.
  int WornPieceOfSlot(int slot) const;
  // The rig slot of a piece's identity shell (see LostGear), or -1.
  int IdentityShellOf(int pieceIndex) const;
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
  // The same march, answering WHICH shell (its rig slot, -1 for none) and
  // WHERE it was met, for a caller that wants to redirect a blow onto it
  // rather than merely know the flesh is covered — the melee sweep, which
  // asks from a flesh hit back along the blade's travel (melee.cpp, "armour
  // defends from cuts"). `maxSteps` is the caller's own bound on the lattice
  // march: the burn pass keeps its short kWornMarchMax because it asks per
  // voxel per tick; a sweep asks a few times per swing and has to cross half
  // a torso to find the entry side of the coat.
  int WornShellAlong(int bodyLimb, const Vec3& from, const Vec3& dir,
                     float dist, int maxSteps, uint32_t* outMat, Vec3* outAt);
  // Does any shell cover this body limb at all? The cheap gate in front of the
  // probe, so an undressed creature never even transforms a point.
  bool LimbHasShells(int bodyLimb) const;
  // Rig slots that are NOT part of the authored rig: worn shells and a held
  // item, always at the tail. `LimbCount() - AppendedBase()` of them.
  int AppendedBase() const { return baseLimbs_; }
  // ---- THE LIVE SWING (game/strokes.h) -------------------------------------
  // An NPC's stroke program, or Idle. On the BASE class for the same reason
  // SetWeaponPose and ApplyWeaponArm are: the machinery is shared and only the
  // DRIVER differs. The player avatar never uses this one — main.cpp owns a
  // MeleeState fed by the mouse — but nothing here would break if it did.
  //
  // Pure presentation state: never saved, never hashed, rebuilt from nothing.
  NpcStroke& Stroke() { return stroke_; }
  const NpcStroke& Stroke() const { return stroke_; }
  // WHICH WAY THIS BODY FACES. The same `AxisAngle({0,1,0}, heading) * +Z` the
  // walk step and the limb submit use -- one convention, one formula, so a
  // caller cannot drift from the direction the mob actually translates along.
  // MobSystem::MobFacing is the id-keyed wrapper for callers holding only an id.
  Vec3 Facing() const;
  float Heading() const { return heading_; }
  // +1 when the weapon is in a hand on the body's RIGHT, -1 on its left. Read
  // off the RIG (the socket part's name), not off the equip context string, so
  // a left-handed def needs no second spelling of the same fact.
  float HandSign() const;

  // The held weapon's cutting edge in WORLD voxels, from its live transform.
  // ---- THE STRIKE EFFECTOR: WHICH PART IS SWINGING (plan §4) --------------
  //
  // Everything below this line used to start at `heldPartIndex_`, which is a
  // way of saying the engine could only swing a sword. The effector is the
  // generalisation: ONE part index plus a MODE (game/impact.h
  // StrikeEffectorMode), read by `ApplyWeaponArm`, `WeaponEdge`,
  // `WeaponStrokePose`, `WeaponArmPose` and `HeadKeepOut` in place of the held
  // slot. `EquipItem` sets `Held` on the item's own slot, so the sword path is
  // byte-for-byte what it was; `BeginStroke` sets `Chain` or `Aim` from the
  // style's `weapon` and clears it when the stroke ends.
  //
  // `naturalIndex` is an index into `def_->natural` — an ADDITION to the
  // plan's two-argument signature, because a part may carry more than one
  // natural weapon (a hand that can punch and claw) and "which of them is
  // swinging" is not recoverable from the part alone. -1 means "the held item
  // is the weapon", which is what Held always means.
  void SetStrikeEffector(int partIndex, StrikeEffectorMode mode,
                         int naturalIndex = -1);
  void ClearStrikeEffector();
  // POINT THE DRIVER AT WHATEVER A STYLE'S `weapon` NAMES, on this creature.
  // The mode is derived from the RIG rather than authored: a part an IK chain
  // can serve is a `Chain`, anything else is an `Aim`. That is one rule
  // instead of a mode field in every style, and it means a rig that later
  // grows a neck chain starts biting through the IK with no content edit.
  // False when the style names something this body has not got — which the
  // callers (MobSystem::BeginStroke, main.cpp's player strike) turn into "do
  // not start a stroke", never into a stroke with no weapon on the end of it.
  bool ArmForStyle(const AttackStyle& sty);
  // THE EFFECTIVE EFFECTOR, DERIVED rather than mirrored: the explicit one a
  // stroke set, else the held item as `Held`, else none. Five call sites
  // clear or renumber `heldPartIndex_` (spawn, rig rebuild, disarm, shed on
  // detach, appended-slot shift) and every one of them would otherwise have to
  // remember a second field — which is precisely the "two representations, one
  // owner" failure design rule 3 names.
  bool ResolveEffector(int& outPart, StrikeEffectorMode& outMode,
                       int& outNatural) const;
  int StrikeEffectorPart() const {
    int p = -1, n = -1;
    StrikeEffectorMode m = StrikeEffectorMode::None;
    return ResolveEffector(p, m, n) ? p : -1;
  }
  StrikeEffectorMode StrikeEffectorKind() const {
    int p = -1, n = -1;
    StrikeEffectorMode m = StrikeEffectorMode::None;
    ResolveEffector(p, m, n);
    return m;
  }
  int StrikeEffectorNatural() const {
    int p = -1, n = -1;
    StrikeEffectorMode m = StrikeEffectorMode::None;
    ResolveEffector(p, m, n);
    return n;
  }
  // The def's natural weapon by index/name, or null. Public because the gates
  // and the tuner state their expectations in terms of the authored block.
  const MobNaturalWeaponDef* NaturalWeapon(int i) const;
  const MobNaturalWeaponDef* NaturalWeaponNamed(const std::string& n) const;
  // The natural weapon the effector currently names, or null — which is what
  // "the thing in its fist is doing the hitting" answers. The one call every
  // damage-side caller makes to decide whose numbers a sweep carries.
  const MobNaturalWeaponDef* EffectorWeapon() const;
  // Can this creature still use that weapon? The part is alive and, for a
  // chain effector, so is every part of the chain that serves it. The one
  // question `StyleUsable` asks, exposed because the gates ask it too.
  bool NaturalWeaponUsable(const MobNaturalWeaponDef& nw) const;
  // WHAT THIS FIST/JAW DOES WHEN IT LANDS, after the two overlays the plan
  // describes: a WORN item covering the weapon's part whose own `strike` is
  // non-empty REPLACES the profile (that is a gauntlet), and the creature's
  // `bite` block is ORed onto anything that bites (that is a zombie).
  StrikeProfile StrikeProfileFor(const MobNaturalWeaponDef& nw) const;
  // ---- BALLISTIC: a body given a velocity it did not walk into ------------
  // `vel` is world voxels/sec. Sets `airborne_`, takes the vertical as
  // `fallVel_` and keeps the horizontal in `airVel_` for `UpdateFall` to
  // integrate against the same wall test the walk drive uses. Cleared on
  // landing. The `launched_` latch is what stops UpdateFall's "just left the
  // ground" branch zeroing the very velocity that put the body there.
  void Launch(Vec3 vel);
  bool Launched() const { return launched_; }
  // ---- A SUSTAINED LIFT: what `float aura` does to a body that is NOT the
  // caster (spell.h `SpellBodyImpulse`, 2026-09-22) ------------------------
  //
  // `vps` is the per-tick velocity change the status asks for, world
  // voxels/sec, exactly the number the player's own controller adds to
  // `player.vel`. The owner routes it here because the spell VM cannot reach a
  // body (spell.h thesis 4) and because "add a velocity" means two different
  // things to this class:
  //
  //   LIMP  the rig is Jolt's, so it is an impulse at each live limb's centre
  //         of mass (mass x dv, so every limb gains the same speed and the
  //         joints are not yanked; at the COM, so a lift does not spin it).
  //   LIVE  the rig is ours, so it goes into the ballistic state `UpdateFall`
  //         integrates - the same one `Launch` fills.
  //
  // A LIFT IS NOT A FALL, and that is the whole reason this is not `Launch`:
  // it must not land the body on the ground it is rising off (the `launched_`
  // latch) and it must not go limp after `ragdoll.fallSeconds` of being held
  // up (`airTime_`). Both are reset only for an UPWARD lift, so `heavy aura`
  // still drives a body down onto the floor and stops there.
  void AddLift(Vec3 velVoxPerSec);
  Vec3 AirVelocity() const { return airVel_; }
  // Off the ground at all — walked off a ledge, blasted, or lunging. Public so
  // the `lunge` gate can state "it left the ground" as the fact it is rather
  // than inferring it from a height that also moves when the body walks
  // uphill.
  bool Airborne() const { return airborne_; }
  float FallVelocity() const { return fallVel_; }
  // ---- ROTATE A PART (AND A SHARE OF THE SPINE) TOWARD A DIRECTION -------
  //
  // MOVED DOWN FROM PlayerAvatar (avatar.cpp's head-look block), because a
  // zombie aiming its jaws at your throat and a player looking at a torch are
  // the same operation and there is no reason for two of them. The spine
  // carries `spineShare` of the yaw so the chest twists into the aim instead
  // of a head swivelling on a rigid torso, split across however many "spine"
  // parts the rig has and EXCLUDING the root (rotating the root yaws the whole
  // creature, legs and all — see the long note the avatar left behind).
  //
  // Angles, not a direction, in the deliberate `lookYaw_` convention: yaw is a
  // HEADING DELTA in the rig's own heading convention and pitch is positive
  // UP. `AimAnglesTo` converts a world direction into that pair; the avatar
  // keeps calling the angle form because its look is already smoothed there.
  void ApplyAimPart(const AnimSkeleton& sk, AnimState& st, int part, float yaw,
                    float pitch, float weight, float spineShare) const;
  bool AimAnglesTo(const Vec3& dirWorld, float& outYaw, float& outPitch) const;
  // ---- ...AND THE SAME THING SOLVED IN THE FRAME THE PART IS ACTUALLY IN --
  //
  // `ApplyAimPart` takes a yaw/pitch pair and multiplies it onto the part's
  // LOCAL rotation, which silently assumes the part's parents are upright: it
  // is a delta about the part's own axes, and those axes are wherever the
  // chain above the part has put them. That is fine for a standing creature
  // whose torso is within a few degrees of vertical and it is WRONG BY NINETY
  // DEGREES for a crawler — the `crawl` clip pitches the hips 74 deg forward
  // (override mode, so it is the pose and not a lean on top of one), so a
  // commanded yaw came out as pitch, a commanded pitch came out as yaw, and a
  // legless zombie's bite went wherever that arithmetic sent it. Reported as
  // "zombie bites when crawling are extremely inaccurate".
  //
  // So this one takes the DIRECTION and solves for the local rotation that
  // actually points `edgeLocal` (the natural weapon's own axis, in the part's
  // model frame) along `dirWorld`, walking the parent chain to get the frame
  // right. `spineShare` of the YAW error is still handed to the spine first,
  // so the chest turns into the bite exactly as it did; the part then solves
  // for whatever is left, which is what makes the result exact rather than
  // approximately exact. Model space maps to world with the plain heading yaw
  // here, the same map `RecordWeaponClamp` and the weapon-arm solve use.
  void AimPartAlong(const AnimSkeleton& sk, AnimState& st, int part,
                    Vec3 edgeLocal, Vec3 dirWorld, float weight,
                    float spineShare) const;
  // Stage 3.5 of the pose pipeline (see the long note at the definition): the
  // Aim effector's drive, or — with no stroke live — a look at whatever this
  // creature has decided to fight. PRE-FLATTEN; the driver calls it beside
  // AnimApplySpineTwist.
  void ApplyStrikeAim(const AnimSkeleton& sk, AnimState& st) const;
  // ---- THE HIT REACTION: WHICH WAY THE BLOW CAME FROM ---------------------
  //
  // A landed blow already says "something hit" (the hit-stop dip) and "here"
  // (the hit flash). Neither says WHICH WAY, and a hit with no direction in it
  // reads as a light going on rather than as a thing striking a body. This is
  // the third channel and it is the one the player feels: the struck creature
  // rocks AWAY from the blade's own travel and settles back inside a quarter
  // second.
  //
  // WHAT IT IS NOT. Nothing here moves `origin_`, `heading_`, `bodyY_` or a
  // collider. It writes `st.local[]` — the same pre-flatten locals the gait
  // bob, the spine twist and the aim write — so the planted feet, the personal
  // space, the A* plan and every hitbox are exactly where they were. That is
  // what lets it fire on EVERY hit with no animation to author, no budget to
  // charge and no recovery state for the AI to know about. A stagger that
  // actually displaces a creature is a different feature with different
  // consequences, and this is deliberately not it.
  //
  // THREE PARTS, one impulse (game/impact.h's law: a profile of numbers, not a
  // kind). The body LEANS away, the root is SHOVED, and the limb that was
  // actually struck FLICKS about its own joint — the last being the only one
  // that says which arm. All three ride one critically damped spring apiece
  // (anim.h AnimSpringStep, Holden's closed form: exact at any dt, so a frame
  // spike cannot make it explode) driven by an initial VELOCITY with the goal
  // at rest, which is the shape that goes out and comes back rather than
  // easing to a new home.
  //
  // WHY THE SPRING IS CLAMPED AS WELL AS DAMPED. A cut is CONTINUOUS
  // (melee.h EdgeSweep::struck): the blade is still in the wound next tick and
  // the sweep lands again, so the impulse arrives once per cut tick and would
  // pump the spring four times for one swing. `SpringDef::maxAngle` bounds the
  // displacement at the authored peak, so a long cut holds the lean instead of
  // multiplying it — which is what a blade dwelling in a wound looks like.
  // The velocity is PEAK-HELD within a tick for the reason the flash and the
  // dip are: several probes of one sweep meeting one limb are one blow.
  void HitReact(int limbIndex, Vec3 dirWorld, float hp, float power);
  // Stage 3.7 of the pose pipeline, PRE-FLATTEN and after the aim: steps the
  // springs and writes the lean, the shove and the flick into `st.local`.
  // Costs nothing at all while no reaction is live — the whole layer is behind
  // one bool that clears itself when the last spring goes quiet (CLAUDE.md
  // rule 2, applied to a presentation layer).
  void ApplyHitReact(const AnimSkeleton& sk, AnimState& st, float dt);
  // ---- THE ONE-FOOTED DRAG: the drivers' half (anim.h AnimApplyStumpDrag) --
  //
  // Stage 3.55, called by both drivers next to ApplyHitReact and PRE-FLATTEN.
  // Eases `dragW_` toward what the body's footing and speed ask for, fades the
  // loco clip out under it (the authored `limp` hop and a drag key the same
  // pelvis, and playing both is the worst of the two), and applies the pose.
  // Returns the live stump so the caller can hand it to TrackStumpContact.
  //
  // `grounded` is the caller's own debounced view — losing the drag for the one
  // tick a bump crest costs would be the pop this layer exists to avoid — and
  // AIR IS WHAT ENDS IT: a jump eases the drag out on the way up, which is how
  // the player opts out of it by hopping.
  AnimStump TickStumpDrag(float dt, bool grounded, bool clipOwnsPose);
  // ...and where the stump is scrubbing, for the smear. Probes the ground under
  // the dead leg's hip and meters the distance travelled; BleedTick spends it.
  void TrackStumpContact(World& world, const AnimStump& stump, float dt);
  // How committed to the drag this body is, 0..1. For the drivers (the stance
  // crouch is low-passed under a drag) and for the gates.
  float StumpDragWeight() const { return dragW_; }
  // Is a reaction running? For the gates and the dev readout; also the one
  // thing `--shot-mob` can assert without reaching into the springs.
  bool HitReactLive() const { return hitReact_.live; }
  // WHAT TO LOOK AT while not swinging, in world voxels. Set by the AI seam
  // each tick it has a target and cleared when it does not, so a creature that
  // loses sight of you stops staring through the wall.
  void SetAimLook(const Vec3& at) {
    aimLook_ = at;
    aimLookValid_ = true;
  }
  void ClearAimLook() { aimLookValid_ = false; }
  // The live loco state's `groundAlign` (0 when upright or stateless) and the
  // body's own up. MobBasis reads both: a prone creature's punch has to be
  // solved in the frame it is actually lying in, or the stroke is expressed
  // about a vertical this body does not have.
  float LocoGroundAlign() const;
  Vec3 BodyUp() const { return bodyUp_; }
  // `outFlat`, when asked for, is the normal of the blade's cutting plane in
  // world space — how edge-on a cut was is a property of the pose, so it is
  // read off the same live transform the segment is.
  bool WeaponEdge(Vec3& outBase, Vec3& outTip, float& outHalfWidth,
                  Vec3* outFlat = nullptr) const;
  // Is this Jolt body one of this creature's own parts?
  bool OwnsBody(uint64_t bodyHandle) const;
  // WHERE A LIMB'S JOINT IS, in world voxels, off its LIVE transform.
  //
  // `xf.pos` alone is the limb's min CORNER, which is not the joint and which
  // swings around it as the limb rotates — so anything that used the corner as
  // a centre of rotation would read a pure rotation as a translation. This runs
  // the same `xf.pos + rot * anchorLimb` composition the kinematic submit path
  // inverts, which is the joint itself. The `swing-plane` gate measures the
  // sword's arc about the shoulder with it.
  bool PartJointWorld(int part, Vec3& out) const;
  // WHERE A LIMB'S MIDDLE IS, in world voxels, off the same live transform.
  //
  // THE JOINT IS THE WRONG POINT TO AIM AT, which is the whole reason this
  // exists beside `PartJointWorld`: a leg's joint is its HIP, and a crawling
  // zombie told to bite a leg would be aimed at the top of a standing victim's
  // thigh — a place its jaws are nowhere near. The centre of the limb's own
  // collider box is the part as a target rather than as a pivot.
  bool LimbCentreWorld(int part, Vec3& out) const;
  // THE STROKE DRIVER'S COMMAND TO THE RIG (game/melee.h). Presentation only;
  // consumed by the driver's own animation pass through ApplyWeaponArm.
  void SetWeaponPose(const WeaponPose& pose);
  // The legacy form: drive the arm at a bare point and leave the blade at
  // whatever grip angle the fist gives it. Kept because that IS the right
  // contract for a caller with no opinion about the weapon — the pose-limit
  // gates aim the hand at hostile targets and must not have a blade steering
  // the wrist underneath them — and because it is the behaviour every mob had
  // before the stroke driver existed.
  void SetWeaponPose(Vec3 handOffset, Vec3 bladeDir, Vec3 bladeFlat,
                     float weight);
  // The INVERSE of SetWeaponPose's hand offset: where the weapon arm's hand is
  // RIGHT NOW, shoulder-relative, in the same frame that call speaks, plus the
  // arm's own reach (its two bone lengths). False when there is no weapon arm
  // to read — no item held, no "arm" chain ending at that hand, or the pose has
  // not been flattened yet. The point of it is that a driver can take control
  // of the arm from wherever the animation had it instead of snapping it to a
  // pose of its own (game/melee.h).
  bool WeaponArmPose(Vec3& outHandFromShoulder, float& outReach) const;
  // THE WHOLE SEED THE STROKE DRIVER NEEDS, in one read and in one frame.
  //
  // WeaponArmPose's inverse round-trip, extended to the blade: the point and
  // the flat as well as the hand, all expressed in the frame SetWeaponPose
  // speaks. The driver steers the TIP, so a take-over that only knew where the
  // HAND was would still have to guess the blade's own orientation — and
  // guessing it is a visible pop at the instant of the click, which is the one
  // thing the take-over model exists to prevent.
  //
  // Derived from the MODEL pose rather than from the physics transform on
  // purpose: WeaponEdge reads the submitted kinematic body, which is a tick
  // behind and in world space, whereas this has to agree exactly with the
  // target the IK will be handed on the very next tick.
  bool WeaponStrokePose(Vec3& outHandFromShoulder, Vec3& outTipFromShoulder,
                        Vec3& outFlat, float& outReach) const;
  // WHICHEVER JOINT THE STROKE ACTUALLY PIVOTS ABOUT, in world voxels — the
  // point the driver's azimuth and elevation are a bearing FROM, and therefore
  // the point an aim has to be measured about. Not always a shoulder: for a
  // chain effector it is the chain's ROOT (a shoulder, or a hip if something
  // ever kicks), and for an aim effector it is the part's OWN joint (the neck,
  // for jaws). Both are exactly the pivot `WeaponArmPose` measured its hand
  // offset from, which is the agreement that stops the aim and the pose
  // speaking different frames.
  //
  // False when there is nothing armed, or the rig cannot answer; the callers
  // (MobSystem::StepStroke, main.cpp's player strike) fall back to a body-box
  // estimate rather than aiming from the origin.
  bool StrokePivotWorld(Vec3& out) const;
  // THE WIELDER'S OWN HEAD as a keep-out sphere for the stroke driver
  // (MeleeState::SetKeepOut): centre relative to the weapon arm's LIVE chain
  // root (the same shoulder anchor WeaponStrokePose speaks, same anim_.model
  // frame, same yaw), radius = half the head model's largest axis in world
  // voxels. Composed HERE because the head limb's size and skin scale are the
  // rig's protected facts. False when there is no head to clear (severed, no
  // tag, no weapon arm) — the caller Clears rather than clamping stale.
  bool HeadKeepOut(Vec3& outCenterFromShoulder, float& outRadius) const;
  // ...AND THE WIELDER'S OWN TORSO, as a keep-out CAPSULE (2026-09-21).
  //
  // The head sphere above stops an authored windup laying the blade through
  // the skull, and nothing whatsoever stopped it laying the ARM through the
  // chest. A cut across the body drives the commanded point to the far
  // azimuth stop, and the hand is that point minus a WHOLE BLADE — which puts
  // it inside the ribcage, with the forearm following it there. Reported as
  // arms clipping through the body, and the self-clip detector
  // (game/selfclip.h) is what turned it from a report into a number.
  //
  // A CAPSULE WITH AN ELLIPTICAL CROSS-SECTION, not a sphere and not a round
  // cylinder: `outA`/`outB` are the spine's two ends, `outWide` is the body's
  // half-width across and `outDeep` its half-depth front-to-back. Never its
  // height — a radius off a torso's longest axis would forbid every pose a
  // human arm has — and never one number for both, because a chest is half as
  // deep as it is wide and a round keep-out sized off the width reaches out
  // to where a guard is held. Same frame as HeadKeepOut: relative
  // to the weapon arm's live chain root, in the yawed anim_.model frame, so
  // the two clamps and the stroke seed cannot disagree by a leaning spine.
  //
  // False when the rig has no spine-tagged part, or no weapon arm — the
  // caller Clears rather than clamping against a stale capsule.
  bool BodyKeepOut(Vec3& outA, Vec3& outB, float& outWide,
                   float& outDeep) const;

  // ---- IS THIS POSE INSIDE ITSELF? (game/selfclip.h) -----------------------
  //
  // Counts solid collider voxels of each limb standing inside another limb,
  // differenced against the bind pose so the shoulder ball that is ALWAYS in
  // the chest is not a finding and a forearm swung through the ribs is.
  // Model space, so the numbers read the same wherever the creature stands.
  //
  // Gate-grade, not frame-grade: the shapes are cached but the pair walk is
  // voxels, and nothing in the frame loop calls it. False = the rig could not
  // be read (no def, no anim pose).
  bool SelfClipCheck(ClipReport& out) const;

  // WHY THE SWORD IS NOT WHERE THE STROKE ASKED, in four numbers.
  //
  // There are three independent ways for a weapon pose to come out wrong — the
  // IK cannot reach the hand, the wrist cannot take the blade angle, or the
  // pose-limit clamp undoes the solve — and every one of them shows up
  // downstream as the same thing: a sword somewhere else. Chasing that with A/B
  // elimination costs one hypothesis per run (CLAUDE.md rule 6); recording the
  // three at the point of failure costs nothing and prints the answer on one
  // line. `swing-plane`'s follow pass reads exactly this.
  struct WeaponArmDiag {
    bool ran = false;
    float ikMiss = 0;        // hand target -> solved hand, world voxels
    float wristWant = 0;     // radians the blade asked the wrist to travel
    float wristApplied = 0;  // ...and what the limit AND the throttle allowed
    // 0..1, WeaponPose::steerAmount as the rig received it. The two above are
    // uninterpretable without it: `wristWant 2.9, wristApplied 0.4` is a wrist
    // clamped to nothing when the throttle is 1 and a blade correctly resting
    // at its grip angle when the throttle is 0.15.
    float steerAmount = 1;
    // Radians the steered elbow hinge axis ended up from the forearm's
    // AUTHORED one, after the shoulder-twist cone (mob.cpp step 2). At the
    // cone it is the bound binding; well inside it the plane is free.
    float elbowAxisTurn = 0;
    float clampMove = 0;     // radians AnimClampPoseLimits then took back
    float clampShift = 0;    // ...and how far it moved the HAND, world voxels
    float shoulderClamp = 0; // radians the ball limit took off the shoulder
    float elbowClamp = 0;    // radians the hinge took off the elbow
    float roundTrip = 0;     // commanded hand -> WeaponArmPose's read of it
    Vec3 cmdHand{}, gotHand{};  // the two ends of that comparison
  };
  const WeaponArmDiag& WeaponArmDiagnostics() const { return weaponDiag_; }

  // WHY THE JAWS ARE NOT WHERE THE STROKE ASKED, in four numbers.
  //
  // The aim effector's equivalent of WeaponArmDiag, and it exists for the same
  // reason: "the head barely follows the driver" is invisible from outside as
  // anything but a bite that looks limp, and it has at least three causes --
  // the aim was never applied, it was applied and diluted by a weight or a
  // spine share, or it was applied in full about a pivot that could not serve
  // it. The COMMANDED pair is recorded where the ask is made and the POSED
  // pair where the flatten and the clamp have finished with it, so the two are
  // an honest before/after rather than a round trip (a circular probe asserts
  // nothing -- the memory file has that one written down).
  //
  // Radians, in `AimAnglesTo`'s convention: yaw is a HEADING DELTA in the
  // rig's own convention and pitch is positive UP.
  struct AimDiag {
    bool ran = false;
    int part = -1;          // the rig slot being aimed
    int natural = -1;       // its natural weapon, for the forward vector
    float cmdYaw = 0, cmdPitch = 0;
    float gotYaw = 0, gotPitch = 0;
    // World voxels the aimed part was carried FORWARD along its aim this tick
    // (the neck extending into a bite; Mob::ApplyStrikeAim). Zero outside a cut,
    // and the one number that says whether the jaws were given the reach to
    // close the last half-body of the gap a pounce cannot.
    float lean = 0;
  };
  const AimDiag& AimDiagnostics() const { return aimDiag_; }

  // ---- render plumbing (per creature; MobSystem chains these over its list) -
  // The Append* walks MUST visit slots in the same order: the slot a transform
  // lands in is the slot the instance records. Each returns the next slot.
  uint32_t AppendInstances(std::vector<BodyVoxInst>& out, uint32_t slotBase);
  void AppendXforms(std::vector<BodyXformGpu>& out) const;
  uint32_t AppendMicroInsts(std::vector<MicroBodyInstGpu>& out,
                            uint32_t slotBase) const;

  // ---- render-only rigid offset --------------------------------------------
  // Added to every limb transform this creature emits, and to NOTHING else:
  // not to the colliders, not to the reach tests, not to a strike's geometry.
  //
  // It exists because a body is posed once per 30 Hz TICK while the frame loop
  // draws at whatever the display does. The player's art used to stair-step at
  // the tick rate while the camera glided (Player::RenderBodyOffset supplies
  // the interpolation and the step-smoothing here), and a step-up moved the
  // figure a whole voxel in one frame. Only the player avatar sets it today;
  // an NPC or a remote player wanting the same treatment sets the same field.
  //
  // Set it every frame or not at all — it is not decayed here.
  void SetRenderOffset(const Vec3& v) { renderOffset_ = v; }
  const Vec3& RenderOffset() const { return renderOffset_; }

  // Every brick record this creature holds, DRAWN OR NOT (sim/microbody.h
  // MicroHolder). A limb keeps `microModel` after a sever hands the brick to
  // DebrisSystem and after Die() does, so the slots that no longer draw are
  // precisely the stale holders worth naming. Audit path only.
  void AppendMicroHolders(std::vector<MicroHolder>& out) const;
  void AppendDebugBoxes(std::vector<DebugBox>& out, size_t limit,
                        uint32_t color) const;
  uint32_t LimbBodyCount() const;
  // Every body this LIVING creature still owns (attached limbs, worn shells,
  // the held item; not a severed piece in its hold). The explosion loop hands
  // these to Physics::ApplyRadialImpulse as its skip list: a rig is launched
  // as one thing by BlastRadial, never limb by limb.
  void AppendLiveLimbBodies(std::vector<uint64_t>& out) const;

  // `outUnknown`, when given, is set true when the answer is "I cannot see"
  // (cell outside the window, chunk not yet cached) rather than "no ground
  // within the scan" — the two used to be one `false`, and the gravity added
  // for ragdolls must not drop a creature through terrain it merely has not
  // fetched yet (the projectile trap in CLAUDE.md, mob edition).
  // `outBlocked` is the THIRD outcome, and conflating it with the second is a
  // bug this engine has now paid for twice in the same shape. A probe that
  // starts inside weight-bearing matter and cannot climb out of it has NOT
  // failed to see anything — it has seen rock, and merely does not know how
  // tall the rock is. Reporting that as "unknown", which every caller correctly
  // reads as OPEN (ai_nav.h rule 1), let a duelist walk through the middle of
  // an eight-voxel stone wall whenever the chunk above the wall happened not to
  // be cached yet. "I cannot see" and "I can see something and cannot see past
  // it" are opposite answers.
  bool GroundHeightAt(World& world, int wx, int wz, int yFrom, int& outY,
                      uint32_t* outMat = nullptr, bool* outUnknown = nullptr,
                      bool* outBlocked = nullptr) const;

  // This body's terrain budgets in CELLS, resolved from its rig (see the
  // members). Public because the AI layer plans with them: the planner and the
  // drive must refuse the same wall, and the only way to guarantee that is for
  // both to read the creature's own number.
  int StepUpCells() const { return stepUpCells_; }
  int StepDownCells() const { return stepDownCells_; }
  int HeadroomCells() const { return headroomCells_; }

  // ---- THE ONE PLACE A WALKING BODY MEETS THE TERRAIN ---------------------
  //
  // What the ground under a body's FOOTPRINT is doing, as one answer. A body is
  // a box, and every question the locomotion asks about terrain is really about
  // that box rather than about the single column under its middle.
  //
  //   groundY  The height the body would REST at: the highest surface under the
  //            box that it could actually step onto. Walking onto a slope, the
  //            uphill corner is already inside the hill while the centre column
  //            still reads a voxel lower, and settling to the centre reading is
  //            what walked the whole box into the ground (see the long note in
  //            GroundHeightAt for what happens next).
  //   wall     A column under the box stands MORE than a step above `fromY`.
  //            This is the distinction that makes the two useful together, and
  //            it cost a regression to learn: fold a wall's top into `groundY`
  //            as if it were footing and a creature whose shoulder grazes a
  //            wall is lifted onto it. `ai-approach`'s mob levitated over an
  //            eight-voxel barrier it is supposed to walk around.
  //   fits     Headroom above `groundY` is clear.
  //
  // `known` is false when NO sampled column could be answered, which the caller
  // must read as WALKABLE (ai_nav.h rule 1) and never as blocked.
  struct Footing {
    bool known = false;
    int groundY = 0;
    bool wall = false;
    bool fits = true;
  };
  Footing FootprintFooting(World& world, const MobDef& def, float cx, float cz,
                           float fromY) const;

  // ---- THE GROUND UNDER A BODY THAT IS LYING ON IT ------------------------
  //
  // A LEAST-SQUARES PLANE, NOT A DIFFERENCE OF TWO PROBES. `UpdateGait`'s slope
  // lean takes one forward probe and one back probe and divides; that is the
  // right instrument for a WALKER, whose torso only leans a few degrees into a
  // grade and whose contact with the ground is two small feet. It is the wrong
  // one for a body LYING on the terrain, for a reason that is about noise
  // rather than about accuracy: two samples have no redundancy, so every voxel
  // either endpoint steps onto moves the answer by a full voxel over the
  // baseline. Real ground is not a plane — it is a mix of grades — and a
  // two-point estimate crawling over ground that alternates between, say, 1:1
  // and 2:1 reports first one and then the other and re-orients the WHOLE
  // CREATURE each time. The eye reads that as the character glitching, not as
  // the terrain being rough.
  //
  // Fitting a plane through a grid of samples answers it properly: the fit IS
  // the average grade over the body's own length, it degrades one sample at a
  // time instead of one endpoint at a time, and its residual is a free measure
  // of how rough the ground is under there. The temporal ease on `bodyUp_`
  // stays on top of it — smoothing in time cannot fix a spatially wrong
  // estimate, it can only lag it.
  //
  // `spanFwd`/`spanSide` are the FULL extents of the sample grid in voxels,
  // measured in the body's own frame about (cx, cz). For a prone body the
  // right forward span is the creature's STANDING HEIGHT: that is how much
  // ground it covers when it lies down, and it is what the user sees the
  // character's angle being "roughly the slope over".
  struct GroundPlane {
    bool valid = false;     // false = fewer than 3 columns could be answered
    float height = 0;       // fitted ground Y at (cx, cz), world voxels
    Vec3 up{0, 1, 0};       // unit normal of the fit
    float gradeFwd = 0;     // rise per voxel of run, along the body's facing
    float gradeSide = 0;    // ... and along its right
    float roughness = 0;    // RMS residual in voxels: how un-plane-like it is
    int samples = 0;
  };
  GroundPlane FitGroundPlane(World& world, float cx, float cz, float fromY,
                             float spanFwd, float spanSide, int nFwd,
                             int nSide) const;

  // The lowest point of the POSED body, in the frame `bodyY_` is measured in:
  // add `bodyY_` and you have the world Y of the lowest voxel the creature is
  // currently drawing. Built from exactly the transforms SubmitPose will use
  // (`LimbTargetFor`, same `bodyRot`), so "put the body on the ground" is one
  // subtraction rather than a per-rig guess at where its hips are.
  //
  // THE CORE, NOT EVERY LIMB. Parts belonging to an IK chain (arms and legs)
  // are skipped: a crawl's arms swing through a large arc, and grounding on
  // whichever hand is lowest would lift and drop the entire body once per
  // stroke — the pose would drive the terrain instead of the terrain driving
  // the pose. The core (pelvis, torso, head) moves smoothly under any sane
  // clip, which is exactly what a contact height has to do. A rig with no
  // chains at all (dummy.json) falls back to every alive limb, since there
  // every limb IS core.
  //
  // `planeDir` is the direction "up off the ground" is measured along, and it
  // is what makes this work on a slope. Pass {0,1,0} and you get the lowest
  // point in plain world Y, which is what "how high is it drawn" means on the
  // flat. Pass the ground plane's own (unnormalized) normal
  // `{0,1,0} - fwd*gradeFwd - rgt*gradeSide` and you get the CONTACT
  // clearance: how far the body's nearest point is off the tilted surface.
  //
  // The two are not the same and the difference is not small. A prone body is
  // as long as the creature is tall; laid along a 1:1 grade, its lowest
  // VERTICAL point is its downhill end, and placing that end at the ground
  // height under the body's MIDDLE hangs the whole creature half a body length
  // in the air — which is the same "floats above the voxels" bug in a new
  // costume. Measured along the plane normal, every point of a body lying flat
  // on the slope reports the same clearance, which is what "lying on it" means.
  //
  // `outPoint`, when given, is WHERE that nearest point is — offsets from the
  // body's own centre column in world axes, y relative to bodyY_. A clearance
  // is only meaningful against the ground under the point it was measured at,
  // and a prone body is long enough that the ground under its nose and under
  // its hips are metres apart.
  //
  // Returns false when nothing is poseable yet (no limbs, no flattened pose).
  bool PosedCoreLowY(Quat bodyRot, Vec3 planeDir, float& outLowY,
                     Vec3* outPoint = nullptr) const;

  // WORLD Y OF THE LOWEST POINT THIS CREATURE IS DRAWING, through its own
  // current tilt and heading. `MobOrigin` is the collider and `MobBodyY` is the
  // frame the pose is built in; neither of them is where the creature LOOKS
  // like it is touching, which is the only number a "does it float" assertion
  // can be made against. `outPoint`, when given, is that point in WORLD voxels
  // — the column a "does it float" check has to look up the surface in.
  // Returns false when there is no pose yet.
  bool DrawnLowY(float& outY, Vec3* outPoint = nullptr) const;

  // WHERE A BODY WHOSE POSE AN AUTHORED CLIP OWNS GOES THIS TICK. The whole of
  // "lying on the ground" in one call, so the NPC loop and the avatar cannot
  // drift apart about what a crawl is. Handles the upright clip-owned states
  // (a hop) too — they are the `false` return, and the outputs are then the
  // historical behaviour, so a caller needs no branch of its own.
  //
  // IT WRITES `bodyUp_` AND ONLY RETURNS THE HEIGHT, and the asymmetry is load
  // bearing rather than sloppy. The tilt has to be eased BEFORE the clearance
  // is measured, because the clearance is a statement about the rotation the
  // body will actually be DRAWN at — measure it against the target tilt, draw
  // it at the eased one, and the difference goes straight into the hillside
  // (a few degrees across a body as long as the creature is tall is most of a
  // voxel at each end). The height, by contrast, is genuinely eased at
  // different rates by the two drivers (MobSystem::EaseBodyY vs the avatar's
  // own clamp), so unifying that would change behaviour nobody asked about.
  //
  // `rule` may be null (a legacy rig with no states at all): the body settles
  // to the walk drive's ground and the tilt eases flat.
  //
  // `dt` is only used to low-pass the posed clearance over a stroke cycle
  // (`proneClear_`); the placement is otherwise stateless in time.
  bool SettleClipOwnedBody(World& world, const AnimStateRule* rule, float dt,
                           float& outTargetY);

  // Does this cell carry a body's weight? THE definition of "solid" for
  // locomotion, shared by the ground probe, the footprint collider and the
  // navigator's `blocked` adapter, so "walkable" means one thing in this
  // engine. A cell the CPU mirror cannot answer for is NOT solid — unknown is
  // open, everywhere, always (ai_nav.h rule 1).
  bool CellSupportsWeight(World& world, IVec3 cell) const;

  // ---- footfall events (presentation only) --------------------------------
  // A foot touching down, produced by the gait's own plant moment rather than
  // by a distance accumulator. These QUEUE because PreTick runs inside the
  // fixed-tick loop (up to 4 ticks per frame): the consumer drains them once
  // per frame. Presentation only — nothing here may feed back into the sim.
  //
  // The TICK-SIDE consumer of a plant is not this queue: ShedCoat is called at
  // the plant itself, inside the tick, so what a bloody foot tracks onto the
  // floor is keyed on the tick and not on how many frames the renderer got.
  // The queue stays exactly what it was — sound and dust.
  //
  // On the AVATAR main.cpp drains and clears this every frame. On an NPC
  // nothing drains it yet, so PreTick clears it and it is capped at
  // kMaxFootfalls (oldest dropped) — an undrained queue may not grow (rule 2).
  struct Footfall {
    Vec3 posVox{};      // where the foot landed
    uint32_t mat = 0;   // material id of the supporting voxel (0 = unknown)
    float speed = 0;    // walker speed at touchdown, voxels/sec
    int foot = 0;       // chain index, so left/right can be pitched apart
    bool landing = false;  // true when this is a touchdown from a fall
    float fallSpeed = 0;   // downward speed on a landing, voxels/sec
  };
  static constexpr size_t kMaxFootfalls = 8;
  const std::vector<Footfall>& Footfalls() const { return footfalls_; }
  void ClearFootfalls() { footfalls_.clear(); }
  // Queue one, dropping the oldest past the cap.
  void PushFootfall(const Footfall& ff) {
    if (footfalls_.size() >= kMaxFootfalls) footfalls_.erase(footfalls_.begin());
    footfalls_.push_back(ff);
  }

  // Release a body's burn index and front (lattice compacted / rig torn down).
  static void DropBurnIndex(BodyBurnState& st);
  // Draw the entity-scoped gore variance for one creature id.
  static GoreProfile MakeGoreProfile(uint64_t id);

 protected:
  // ---- THE WEAPON ARM, for BOTH animation drivers --------------------------
  //
  // Stage 5.5 of the pose pipeline: run on the FLATTENED pose, after the gait's
  // leg IK and before AnimClampPoseLimits. Aims the weapon arm's two-bone chain
  // at the driver's hand target with the driver's own bend pole, then orients
  // the HAND so the blade points where the stroke says. Returns the elbow's
  // hinge-axis override for the clamp that follows (anim.h PoseAxisOverride);
  // `ov.part` is -1 when there is nothing to override.
  //
  // SHARED, not duplicated, and that is the point: the avatar and an NPC swing
  // through one implementation, so a mob driven by an authored stroke curve
  // (Phase C) gets exactly the arm the player gets, and a fix to one is a fix
  // to both. It lives on Mob rather than on PlayerAvatar for the same reason
  // SetWeaponPose does.
  void ApplyWeaponArm(const AnimSkeleton& sk, AnimState& st,
                      PoseAxisOverride& ov) const;
  // The IK chain that serves an effector part, and the chain's own effector
  // (what the arm code calls "the hand"). ONE RULE FOR BOTH MODES: the chain
  // whose effector IS the part (a fist — the hand is the effector) or whose
  // effector is the part's PARENT (a held item — the item is a child of the
  // hand the chain ends at). Null when nothing serves it, which is what an
  // Aim effector always answers.
  const IkChain* ChainForEffector(const AnimSkeleton& sk, int part,
                                  int& outHandPart) const;
  // Called by both drivers straight after AnimClampPoseLimits: fills in
  // WeaponArmDiag::clampMove, the one piece of attribution that cannot be
  // collected inside ApplyWeaponArm because the clamp has not run yet.
  void RecordWeaponClamp(const AnimSkeleton& sk, const AnimState& st) const;

  // ---- THE EXPLICIT-EXCEPTION SEAM ------------------------------------------
  // Everything the avatar does differently from an NPC goes through one of
  // these. Adding avatar behaviour anywhere else in the shared mechanics is
  // the bug this class was built to make impossible.
  //
  // Limbs of the player's body live on the AVATAR physics layer (they sit
  // inside the player capsule and must not push it — see avatar.cpp Spawn).
  virtual bool AvatarLayer() const { return false; }
  // (There used to be an OnBodyReleasedToWorld here, where the avatar put a
  // severed piece back on the normal layer after its hold. Gone: EVERY body
  // that leaves ANY rig now goes through Physics::ReleaseToWorldWhenClear,
  // which keeps it off the player until it has fallen clear — an NPC's
  // severed arm inside the player's capsule launched the player exactly as
  // the avatar's own used to, and the fix belongs to the body, not to who it
  // came off.)
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

  // ---- live ragdoll internals -----------------------------------------------
  // One tick of the limp phase: settle test, then BeginGetUp. Called by the
  // NPC loop and the avatar driver in place of their locomotion stages.
  void TickRagdollLimp(World& world, float dt);
  // One limp tick of the arrest measurement TakeRagdollImpact drains. See the
  // long note at its definition.
  void TickRagdollArrest(float dt);
  // Re-derive origin_/heading_ from where the pelvis lies, make every limb
  // kinematic again where it is, and start the blend (RagdollPhase::GetUp).
  void BeginGetUp(World& world);
  // One tick of the get-up clock; ends the ragdoll when the blend completes.
  void TickGetUp(float dt);
  // The kinematic target SubmitPose would give limb `i` for a body placed at
  // `bodyOrigin` with rotation `bodyRot` — factored out so BeginGetUp can ask
  // "where would the root be if I stood here" with the same arithmetic.
  void LimbTargetFor(size_t i, Vec3 bodyOrigin, Quat bodyRot, Vec3 yawPivot,
                     Vec3& outPos, Quat& outRot) const;

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
  // ---- WHERE THE CARVE ACTUALLY LANDED -------------------------------------
  // A predicate-driven carve knows the volume it SEARCHED; only CarveLimb
  // knows which voxels were really in it, because it owns the limb's voxel
  // list and it is what runs the spall rounds. The blast path needs that to
  // bloody the CRATER rather than the sphere it searched — the two differ by
  // everything once a blast is bigger than the arm it grazed. Centroid and
  // radius are in limb-local WORLD voxels, the frame `woundLocal` is read in;
  // the radius is an RMS spread (outlier-proof, and ~0.78 R for a full sphere
  // of radius R) rather than a max, so one stray spalled voxel cannot inflate
  // it. `count == 0` means the carve found nothing on this limb.
  struct CarveReport {
    uint32_t count = 0;
    Vec3 centreLocal{};
    float radiusLocal = 0.0f;
    // The removed cells themselves, in the AUTHORITATIVE lattice's coords —
    // the set a crater's blood is measured from (phys/bodystain.h CellDist).
    // Collected only when a report is asked for, so the burn flush (which
    // carves dozens of times a second and wants none of this) pays nothing.
    std::vector<IVec3> cells;
  };
  bool CarveLimb(int limbIndex, World& world,
                 std::vector<ParticleSpawn>& spawns, bool eject,
                 const LimbCarveFactory& carveAt,
                 const CarveSpall* spall = nullptr,
                 CarveReport* report = nullptr);
  // Sum MobDef::WoundHpOf over a limb's CURRENT authoritative lattice, and
  // count its brain voxels, in one pass. This is what MobLimb::weightCharged /
  // brainCharged are deltas of; see the note on those fields for why the
  // accounting is a recomputed total rather than a running subtraction.
  void LimbWoundTotals(const MobLimb& limb, float& weight,
                       uint32_t& brain) const;
  // ---- ONE BLOB TORN OUT OF A LIMB ----------------------------------------
  //
  // The correlated-noise bite Mob::RotAtSpawn draws the undead's holes with,
  // available to anything that wants ONE of them: a `blob`-sized value-noise
  // field thresholded against a `(1 - t^2)^2` radial falloff, recentred on its
  // own centre value so the bite always lands. Every word of why it is shaped
  // that way is at the predicate itself (mob.cpp BlobCarveFactory).
  //
  // The SHARED PART is the predicate, not this function: RotAtSpawn hands
  // BlobCarveFactory its whole list of bites in ONE CarveLimb call (that is
  // what makes its volume cap and its single hp charge correct), and this
  // hands it a list of one. So there is one blob and two callers, rather than
  // two blobs -- which is what the extraction was for.
  //
  // `centreLocal` and `radiusWorld` are limb-local WORLD voxels, like every
  // other radius here. `report` receives the removed cells so a caller can
  // soak exactly the hole it made. Returns false when the limb did not survive
  // (CarveLimb's contract: nothing may touch `limbs_` after that).
  bool CarveBlob(int limbIndex, Vec3 centreLocal, float radiusWorld, float blob,
                 uint32_t seed, World& world,
                 std::vector<ParticleSpawn>& spawns, CarveReport* report);
  // The blob predicate itself, over a LIST of bites. A member (rather than the
  // free function it reads as) only because LimbCarveFactory is protected here;
  // static because it captures nothing of the creature. Mob::RotAtSpawn hands
  // it every hole a zombie was born with in one call, CarveBlob hands it one.
  static LimbCarveFactory BlobCarveFactory(std::vector<Vec3> centres,
                                           std::vector<float> radii, float blob,
                                           uint32_t nseed, uint32_t skinScale);
  bool ReskinLimbMicro(MobLimb& limb, uint32_t skinScale, uint32_t physScale);
  bool RebuildLimbBody(int limbIndex);
  // ---- BORN BITTEN (MobRotDef) ---------------------------------------------
  // Runs ONCE, from MobSystem::Spawn, immediately after BuildRig and before the
  // creature is published. Carves the def's `rot` blobs out of every eligible
  // limb through CarveLimb, so the holes are real geometry and the hp charge is
  // the ordinary one. Returns the number of voxels it removed across the rig
  // (0 for anything that is not rotted), which is what the `undead` gate reads.
  //
  // NOT called from the save-load overlay: a saved zombie already has its holes
  // in its saved lattice, and rotting it again on every load would eat it.
  uint32_t RotAtSpawn(World& world);

 public:
  // ---- MEND (docs/PLAN_magic_grammar.md §7; game/spell.h verb `mend`) -------
  // The anatomy .vox IS the recipe of what should be there, so "missing" is
  // well-defined and so is "which cell next": RestoreVoxels fills up to
  // `count` missing cells of a LIVE limb with `material`, nearest the joint
  // anchor first (a stump regrows outward), re-derives the collider and the
  // brick, and credits hp for the volume put back. The restored cell IS that
  // material: wood burns, steel does not. Returns how many landed. A severed
  // limb has no lattice to fill and is not regrown.
  int RestoreVoxels(int limbIndex, uint32_t material, int count);
  uint32_t MissingVoxelCount(int limbIndex) const;
  // Root-first across the rig: the def's limb order is parent-before-child.
  int RestoreBody(uint32_t material, int count);
  // THE CAUTERISE RULE: a charred cell is not a bleed source. True when the
  // limb's voxel nearest its wound is at burn stage 2 (charred / ash), which
  // the bleed tick reads to close the wound for good.
  bool WoundCharred(const MobLimb& limb) const;

 protected:
  // ---- the wound model's two helpers (game/mob.cpp, and the notes there) ----
  // Voxels of `limb` within `radiusWorld` world voxels of its joint anchor, on
  // whichever lattice is authoritative. ONE pass, no allocation. This is the
  // measure MobLimb::neckAtSpawn records and the neck sever compares against.
  uint32_t NeckCount(const MobLimb& limb, float radiusWorld) const;
  // The same count about an ARBITRARY point of the limb's own frame, in
  // limb-local world voxels. NeckCount is this with the joint anchor; the
  // socket measure is this with the joint expressed in the PARENT's frame.
  uint32_t NeckCountAt(const MobLimb& limb, Vec3 centreLimb,
                       float radiusWorld) const;
  // A point in a limb's local frame (world voxels) pulled inside that limb's
  // lattice bounding box. A joint anchor is a rig point and may sit outside the
  // voxel cloud entirely; this is what makes a joint measurement sample the
  // flesh the joint is seated against instead of the empty space beside it.
  Vec3 ClampToLimbBox(const MobLimb& limb, Vec3 pLimb) const;
  // Index of `limbIndex`'s parent limb, or -1 (root, no parent, or a parent
  // name that names nothing). Parentage is by NAME in limbDefs_, which is the
  // only place it exists.
  int ParentLimbIndex(int limbIndex) const;
  // Where this limb's joint sits in its PARENT's local frame, clamped into the
  // parent's lattice bounding box. Limb-local world voxels, ready for
  // NeckCountAt.
  Vec3 SocketCentreInParent(const MobLimb& parent, const MobLimb& child) const;
  // Take MobLimb::neckAtSpawn for `limbIndex` and MobLimb::socketAtSpawn for
  // every child of it, if they have not been taken. Called at the top of a
  // carve, BEFORE it removes anything — the children's sockets are measured on
  // THIS limb, so they have to be recorded before this limb is the one being
  // eaten.
  void EnsureJointCounts(int limbIndex);
  // Forget every joint baseline, so the next carve re-takes it off the lattice
  // the creature is actually standing there with. Called once, at the end of
  // RotAtSpawn: a body born bitten did not arrive with the pristine sockets
  // EnsureJointCounts would otherwise have recorded on the way in.
  void RebaseJointCounts();
  // May the joint-attachment rule take a limb off for the carve in progress?
  // Blunt never amputates; fire keeps its own account; spawn rot is the damage
  // the creature ARRIVED with and may not dismember it; everything else —
  // blade, blast, and live rot even though rot rides the burn flush — may.
  bool JointRuleApplies(int limbIndex) const;
  // Is `limbIndex` still held on by flesh — on BOTH sides of its joint? False
  // when either side has fallen below gore.woundNeckFraction of what it had.
  bool JointAttached(int limbIndex) const;
  // Sever any child of `parentIndex` whose socket in it has been eaten away.
  // Returns whether the creature is still alive (severing a vital child kills).
  bool DropDisconnectedChildren(int parentIndex);
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
  //
  // `crater`: the cells this carve actually REMOVED, in the limb's
  // authoritative lattice. When given, the taper is measured to the nearest of
  // them instead of to `centreLocal`, and `rimCells` (lattice cells) replaces
  // `radiusWorld` as how far past the hole the blood reaches. A kerf is one
  // shape and a ball round it is a fair description of it; a crater is not —
  // see the note on CellDist in phys/bodystain.h.
  //
  // `wetness` (0..1) is HOW MUCH BLOOD THERE IS, as distinct from how far it
  // reaches. 1 is a fresh wound and is what every blade, blast and bite passes;
  // below 1 both halves of the soak thin together — fewer voxels rewritten,
  // a lighter tint, bone shown only faintly — and the reach comes in as
  // sqrt(wetness), more slowly than the amount, because a stain that fades
  // to nothing over one cell is a stain nobody can see at this resolution.
  // 0 returns immediately and leaves the hole dry. Only `Mob::RotAtSpawn`
  // passes anything else today, per BITE (MobRotDef::dryFraction).
  uint32_t StainWound(int limbIndex, Vec3 centreLocal, float radiusWorld,
                      uint32_t seed,
                      const std::vector<IVec3>* crater = nullptr,
                      float rimCells = 0.0f, float wetness = 1.0f);
  // ---- ...AND THE SAME SOAK IN SOMEBODY ELSE'S SUBSTANCE -------------------
  //
  // StainWound is this, with `rewriteMat` bound to the victim's own woundMat
  // and `smearMat` to the chain that derives the tint from it. THE MATERIAL A
  // WOUND REWRITES TISSUE TO IS A PROPERTY OF THE STRIKE, NOT OF THE STRUCK
  // (plan S6), and that is the entire content of this split:
  //
  //   a cut   -> the victim's woundMat (blood). Today's behaviour, unchanged.
  //   a bruise-> gore.bruiseMat, and NO smear: a punch does not bloody you.
  //   a bite  -> the biter's infectMat, smeared with its infectStain.
  //
  // `smearMat` is the OVERLAY half (phys/bodystain.h SoakCut) and is a
  // separate argument rather than derived from `rewriteMat`, which is a
  // deliberate departure from the plan's six-argument sketch: the two halves
  // genuinely disagree for two of the three callers above. A bruise rewrites
  // something with no stain block and must smear nothing; a bite rewrites
  // rotflesh (a solid, which cannot stain by construction -- only liquids may)
  // and must smear ichor. Deriving one from the other would have made both
  // wrong, and in opposite directions.
  //
  // Everything else is StainWound's contract verbatim, including that both
  // rewrites go into MobLimb::woundWas, so a bruise and an infection HEAL back
  // through ReviveWoundVoxel exactly as blood does.
  // `wetness` threads through from StainWound unchanged (every strike-kind
  // caller passes the fresh default; only RotAtSpawn's per-bite roll differs).
  uint32_t StainWoundAs(int limbIndex, Vec3 centreLocal, float radiusWorld,
                        uint32_t seed, uint32_t rewriteMat, uint32_t smearMat,
                        const std::vector<IVec3>* crater = nullptr,
                        float rimCells = 0.0f, float wetness = 1.0f);
  // ---- THE BLUNT MARK, WHICH IS NOT A REWRITE (2026-09-16) ----------------
  //
  // Lays an ACCUMULATING body coat over every tissue voxel in range instead of
  // repainting a hash-picked subset of them, so a bruise is an alpha that
  // deepens by `gore.bruiseStep` a blow to a `gore.bruiseMax` ceiling and then
  // may break into blood. See the long note at the definition for why the
  // rewrite could never look right. Returns voxels whose coat changed.
  //
  // `hp` is the blunt part of the blow that is laying this mark, and it is
  // what scales the step (Tuning::Gore::bruiseHpRef): a fist and a mace used
  // to bruise identically, which made "the same thing with fists, only slower"
  // impossible to say. Pass 0 for "a full blow".
  //
  // ---- ...AND WHAT IT FOUND ALREADY THERE ---------------------------------
  //
  // `report` is how the caller reaches the third rung of the blunt ladder
  // (Tuning::Gore::pulpCarveFrom). The sweep over the contact sphere already
  // reads every voxel's coat, so it is free to say how much of the CORE has
  // already been beaten open — and that share, not the weapon alone, is what
  // decides whether this blow removes anything. A separate probe pass would
  // walk the same lattice twice to learn the same fact.
  //
  // The core is the inner half-radius, deliberately: the taper means the rim
  // of a bruise never saturates however many blows land, so a share measured
  // over the whole sphere would be permanently small and no weapon would ever
  // earn a dent.
  struct BruiseReport {
    uint32_t marked = 0;  // coat words changed by THIS blow
    uint32_t core = 0;    // tissue voxels inside the contact core
    uint32_t pulped = 0;  // of those, already wearing blood at gore.pulpAmt or deeper
    // 0 on clean skin, 1 on a core that is wholly pulp. The number the dent
    // radius is scaled by.
    float Ripeness() const {
      return core ? (float)pulped / (float)core : 0.0f;
    }
  };
  uint32_t BruiseLimb(int limbIndex, Vec3 centreLocal, float radiusWorld,
                      uint32_t seed, uint32_t bruiseMat, float power,
                      float hp = 0.0f, BruiseReport* report = nullptr,
                      bool unarmed = false);
  // The substance StainWound smears when nobody has said otherwise: the
  // creature's wound material if the palette can draw it, else its blood, else
  // nothing. One function because three call sites wanted the same chain.
  uint32_t DefaultSmearMat() const;
  // ---- and the other half: the soak DRIES BACK TO FLESH -------------------
  // BurnLimbView::ReviveFn over one limb's `woundWas` table. A raw function
  // pointer for the same reason WornAlong is one: the view is rebuilt per limb
  // per tick and a capturing std::function would heap-allocate for it. `ctx`
  // is the MobLimb. Erases the entry it answers with, so one soak reverts once
  // and a voxel cut again is remembered again, as the new thing it is.
  static bool ReviveWoundVoxel(void* ctx, IVec3 p, uint32_t& word,
                               uint32_t& color);
  // Does this creature's flesh close over a cut? def AND the global switch.
  bool WoundsHeal() const;
  // ---- BLOOD ON A BODY, from the world and from other bodies -------------
  // Contact: every limb reads the world cells around it once per tick (the
  // same walk the burn pass makes) and takes the stain of any staining
  // liquid, dry stain or drip it is touching on its exposed voxels -- or has
  // it rinsed off by a washing liquid. Sleeps at the cost of the walk when
  // nothing is near. `budget` is lattice cells this call may visit.
  void StainTick(uint32_t tick, World& world, uint32_t& budget);
  // Replay one queued burst against this creature's limbs: each droplet that
  // would land on a limb marks the voxel where it lands.
  void ApplySplatter(const SplatterEvent& e);
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
                      int bodyLimb, const std::string& partName,
                      uint32_t dye = 0);
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
  // Rebuild `crossHeat_` from every limb's burn front (see CrossHeatCell).
  // Costs nothing on a creature with no limb alight. `tick` only rotates which
  // limb's front is walked first when the scan budget cannot cover them all --
  // the same fairness BurnTick and BurnLimbs apply, for the same reason.
  void BuildCrossLimbHeat(uint32_t tick);
  bool FlushBurn(int limbIndex, World& world,
                 std::vector<ParticleSpawn>& spawns, bool force);
  void StripBurnTombstones(MobLimb& limb);

  // ---- the infection's clock (gore.infectSpreadRate / infectRotRate) --------
  //
  // Runs at the tail of BurnTick, so the player reaches it through exactly the
  // same seam an NPC does (PlayerAvatar::BurnParts -> Mob::BurnTick). Costs one
  // `infectMat != 0` test per limb on a creature nothing has bitten, which is
  // every creature in the world until a zombie gets its teeth into one.
  //
  // MAY RESHAPE limbs_: the rot's removals go out through FlushBurn, which
  // expresses itself as a carve and can sever the limb or kill the creature.
  // Returns false when that has happened and the caller must touch nothing.
  bool InfectTick(uint32_t tick, World& world,
                  std::vector<ParticleSpawn>& spawns);
  // PULPED TISSUE DISSOLVES (the blunt counterpart of InfectTick). One tick
  // older, same Bernoulli draw, same FlushBurn tail. Eats voxels wearing the
  // victim's own blood at gore.pulpAmt depth or deeper. Same return contract
  // as InfectTick: false means limbs_ was reshaped and the caller must stop.
  bool BluntPulpTick(uint32_t tick, World& world,
                     std::vector<ParticleSpawn>& spawns);
  // One limb, on a tick whose dice came up non-zero. Same return contract.
  bool InfectStep(int limbIndex, uint32_t tick, uint32_t nSpread,
                  uint32_t nRot, World& world,
                  std::vector<ParticleSpawn>& spawns);
  // Cross-joint infection: converts tissue voxels on `toLimb` that are
  // world-space adjacent to the infected voxel positions in `srcWorld`.
  // Returns the number of voxels converted (0 if nothing touched).
  uint32_t InfectAcrossJoint(int fromLimb, uint32_t tick, int toLimb,
                             const std::vector<Vec3>& srcWorld);

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
  bool swinging_ = false;
  GoreProfile gore_;           // this creature's own bleed character
  // ---- blood loss and the burn cap (see the public block above) -----------
  float bloodLost_ = 0.0f;
  float burnFrac_ = 0.0f;
  float burnCap_ = 1.0f;
  // The lattice changed since burnFrac_ was taken. Set by the burn pass and by
  // every carve; consumed by RecountBurn at a bounded cadence (rule 2: a
  // creature that is not changing costs nothing, one that is burning pays one
  // pass over its body every kBurnRecountTicks, never one per burn step).
  bool burnFracDirty_ = false;
  uint32_t burnRecountTick_ = 0;
  const char* deathCause_ = "";
  static constexpr uint32_t kBurnRecountTicks = 8;
  // One pass over the authored limbs' lattices -> burnFrac_/burnCap_, then
  // ApplyBurnCap. `force` ignores the cadence (a sever or a gate wants the
  // answer now).
  void RecountBurn(uint32_t tick, bool force = false);

  // ---- the coat ledger (see LimbCoat) --------------------------------------
  // A coat byte changed since the ledger was taken. Set by every writer —
  // contact, splatter, the cut soak, decay, a deposit — and consumed by
  // RecountCoat at tune.coat.recountTicks. Exactly the burnFracDirty_ pattern
  // and for exactly its reason: a body nothing is happening to costs nothing.
  bool coatDirty_ = false;
  uint32_t coatRecountTick_ = 0;
  bool coatCounted_ = false;   // has the ledger ever been computed?
  LimbCoat bodyCoat_;          // over the BASE limbs only (no worn, no held)
  // One pass over every live limb's authoritative lattice -> MobLimb::coat and
  // bodyCoat_. Called at the tail of StainTick, exactly as RecountBurn is
  // called at the tail of BurnTick. `force` ignores the cadence.
  void RecountCoat(uint32_t tick, bool force = false);
  // Track one footfall's worth of a coat onto the ground: ONE micro droplet of
  // `mat`, born inside `groundCell`, which the particle kernel resolves into
  // that cell's stain bits on the tick it drains. Refuses when the material
  // has no stain palette slot, when the system's per-tick shed budget is
  // spent, or when this creature's spawn queue is full. Returns whether it
  // queued. Does NOT touch the foot's own lattice — the caller subtracts what
  // left, because only the caller knows which voxels those were.
  bool DepositCoat(uint32_t mat, IVec3 groundCell, uint32_t tick);
  // ---- A BLOODY FOOT LEAVES A PRINT ----------------------------------------
  // Called AT THE PLANT — both gait drivers, and the avatar's fall landing —
  // with the chain's effector limb and the world position the foot came down
  // on. Rolls the coat material's own authored `coat.shed` (per mille), and on
  // a hit tracks one droplet into each distinct ground cell of the sole's
  // footprint (tune.coat.shedCells), then takes the same amount back OFF the
  // sole so the substance is moved rather than copied.
  //
  // A CLEAN FOOT PAYS NOTHING: the limb's ledger is read first, and a limb
  // carrying nothing that declares a shed rate returns before any probe.
  // Returns how many droplets it actually put down (0 = a clean foot, a
  // failed roll, no ground, or a refused budget).
  uint32_t ShedCoat(int footLimb, Vec3 footPosVox, uint32_t tick, World& world);
  // Burnable voxels of a limb with at least one open face, on its
  // authoritative lattice. One hash pass; taken once per limb (surfaceAtSpawn).
  uint32_t SurfaceCount(const MobLimb& limb) const;
  // Clamp every live authored limb's hp to its authored max x burnCap_, and
  // die if the cap is gone or a vital limb has nothing left under it.
  void ApplyBurnCap();

  // ---- steering: intent vs actuation (NPC driver state; the avatar writes
  // heading_ directly from the camera and ignores the rest) ------------------
  // `heading_` is where the BODY actually points — the only thing the pose,
  // the gait and MobFacing ever read. Nothing outside MobSystem::Steer (or the
  // avatar's driver) may write it; behaviours write desiredHeading_ and the
  // gap closes at a bounded rate.
  float heading_ = 0;
  float desiredHeading_ = 0;
  float turnVel_ = 0;
  // Local drive velocity, as multipliers on def.speed in the mob's OWN frame:
  // `driveScale_` forward (now SIGNED — a back-pedal is not a rout), and
  // `driveStrafe_` to the mob's right. The lateral term is what makes footwork
  // possible at all: a duelist that has to turn its back to give ground reads
  // as fleeing, and circling a target while facing it is pure strafe.
  //
  // This does NOT weaken the steering invariant. Steer remains the only writer
  // of `heading_`; these two only change which direction the body translates
  // RELATIVE to that heading, which is the drive stage's own job.
  float driveScale_ = 1.0f;
  float driveStrafe_ = 0.0f;
  uint32_t blockedTicks_ = 0;
  // The behaviour layer's per-creature memory (game/ai_behavior.h). Pure
  // gameplay state: never hashed, never saved, rebuilt from the def on spawn.
  ai::Brain ai_;
  // ---- ownership (M9.4-B) --------------------------------------------------
  // The playerId that STEPS this creature. Default 0 = session 0 = the only
  // player a single-player process has, so nothing below ever fires until
  // MobSystem::SetOwnershipFn is set by the network layer.
  uint32_t owner_ = 0;
  // The last MobPose received for this creature, and whether one ever was.
  // A ghost with no pose yet is left exactly where the announce/handoff put
  // it rather than being snapped to the origin — the first stream packet may
  // be a tick or two behind the announce and a creature that teleports to
  // (0,0,0) for two frames is a worse answer than one that stands still.
  ::net::MobPose ghostPose_;
  bool haveGhostPose_ = false;
  // ---- A CREATURE THIS MACHINE HAS NEVER HELD (M9.4-E) --------------------
  //
  // Set by `MobSystem::ApplyAnnounce`, cleared by `ApplyHandoff`. While it is
  // set, `RefreshOwnership` leaves `owner_` ALONE: the ownership function may
  // not promote this creature to local, and only an explicit `MobHandoff` --
  // which carries the record -- can.
  //
  // WHY, MEASURED. An announce carries the SHAPE and nothing else: no wounds,
  // no carve state, no brain, no target. Before this flag, a ghost spawned
  // from an announce was an ordinary ghost, so the derived authority promoted
  // it the first tick this machine's player was nearer -- and the two-process
  // smoke of 2026-09-21 then showed the host posing those three humans for
  // 187 ticks (`handoffs out=0`: its own authority had never flipped) while
  // the client posed the same three for 157. Both machines stepping one
  // creature is the single-producer rule broken in the open, and the client's
  // copy was a PRISTINE one -- it had never been told what the creature's
  // wounds were.
  //
  // The handoff is what makes a creature you have never held yours, because
  // the handoff is the only record that says what it IS. Until it arrives the
  // safe answer is "the peer's", and a ghost held one extra beat costs a
  // creature that is drawn but not stepped -- which is exactly what a ghost is
  // for.
  bool announceOnly_ = false;
  // ONE tick of a creature somebody else owns: place the limbs where the
  // owner says they are and stop. The fourth branch of PreTick's loop, beside
  // Limp / GetUp / live.
  void TickGhost(float dt);
  float phase_ = 0;            // walk cycle (legacy swing fallback)
  uint32_t lastTurnTick_ = 0;

  Vec3 origin_{};              // prefab min corner, world voxels
  std::vector<MobLimb> limbs_;
  std::vector<CrossHeatCell> crossHeat_;  // this tick's heat across joints
  AnimState anim_;             // float presentation state (never hashed)
  float speedNow_ = 0;         // measured planar speed, voxels/sec
  Vec3 bodyUp_{0, 1, 0};       // foot-plane normal (slope tilt)
  float bodyY_ = 0;            // prefab MIN CORNER height (same frame as origin_.y)
  // LOW-PASSED CONTACT CLEARANCE for a prone body (SettleClipOwnedBody). The
  // posed core's clearance is not constant over a crawl stroke: the clip
  // pitches the ROOT a few degrees each cycle, and a few degrees about the hip
  // is over a voxel at the far end of a body as long as the creature is tall.
  // Grounding on the instantaneous minimum hands that straight to the body
  // height — the creature heaves once per arm stroke, and because the minimum
  // is an extreme it also sits high for most of the cycle. Averaged over the
  // stroke instead, the body holds still and the rocking stays where it was
  // authored: in the pose. Presentation only, reset whenever the body is not
  // prone so a state change never eases out of a stale clearance.
  float proneClear_ = 0;
  bool proneClearInit_ = false;
  // The eased half of the contact-probe floor beside it: how far the single
  // column under the body's contact point is currently asking the body to be
  // lifted out of the ground. Stepped by construction (one column, whole
  // voxels, re-chosen wherever the pose is touching), so it is the chaotic
  // component of a crawl's bob and the one the eye blames on the arms.
  float proneLift_ = 0;
  // ---- THE ONE-FOOTED DRAG (anim.h AnimApplyStumpDrag) --------------------
  //
  // `dragW_` is the eased commitment to the drag, 0 at a standstill and 1 at
  // walking pace, and it is the whole of "do not switch, blend": both drivers
  // drive it from speed and from footing, so standing up out of a drag, taking
  // off into a hop and landing back into one are all one number moving.
  //
  // The rest is the SMEAR. A stump scrubbing over ground leaves blood where it
  // touches, which is a distance-metered event, not a per-tick one: dragging
  // slowly must not paint the same voxel thirty times a second. So the pose
  // pass records where the stump is touching and how far the body has moved
  // since the last mark, and BleedTick — the one place that already knows this
  // creature's blood material, its op budget and what a drop costs in hp —
  // spends it. `dragStumpLimb_` is the limb the blood comes OUT of, so a
  // cauterised or garment-covered stump refuses for the same reasons the drip
  // does.
  float dragW_ = 0;
  // The direction the body is being towed, in the RIG'S OWN frame, low-passed:
  // what the dead limb swings away from. Lagged rather than instantaneous so a
  // turn whips the leg around behind the new heading instead of rotating a
  // fixed pose with the hips — see the note at its update.
  Vec3 dragLag_{};
  float dragTrailDist_ = 0;    // world voxels travelled since the last smear
  Vec3 dragContact_{};         // where the stump is scrubbing, world voxels
  bool dragContactValid_ = false;
  int dragStumpLimb_ = -1;
  // ---- live ragdoll state (see RagdollPhase above) ----
  RagdollPhase ragdoll_ = RagdollPhase::None;
  float ragdollT_ = 0;         // seconds in the current phase
  float ragdollMinT_ = 0;      // Limp: shortest stay before the settle test
  float ragdollStillT_ = 0;    // Limp: seconds the pelvis has been under settleSpeed
  // The arrest, see TakeRagdollImpact. `ragdollLastVel_` is the root limb's
  // velocity at the previous limp tick and is only meaningful while the flag
  // beside it is set — the first tick of a limp has nothing to difference
  // against, and SetLimbVelocities (a blast launch, the mid-air flip) reseeds
  // both so a launch can never be read as a landing.
  Vec3 ragdollLastVel_{};
  bool ragdollVelValid_ = false;
  Vec3 ragdollImpact_{};     // peak braking EVENT since last drained
  Vec3 ragdollArrestRun_{};  // the braking event in progress
  float ragdollArrestCap_ = 0.0f;  // ...and the speed it opened with
  uint8_t ragdollArrestTicks_ = 0;
  uint8_t ragdollArrestQuiet_ = 0;  // consecutive ticks nothing has braked
  // GetUp: each limb's world pose the moment it was made kinematic again —
  // the "from" side of the get-up blend, parallel to limbs_.
  std::vector<BodyTransform> getUpFrom_;
  // ---- gradual skin tint (the corpse-to-zombie palette transition) ----
  // One entry per art colour that differs between the body it was and the
  // body it rose as. Each names a dedicated slot in the shared art palette
  // whose RGB is lerped from humanRgb to zombieRgb over 60 seconds; the
  // voxels already reference these slots, so the brick data never changes.
  struct TurnTintSlot {
    size_t sharedIndex;
    uint32_t fromRgb, toRgb;
  };
  std::vector<TurnTintSlot> turnTintSlots_;
  float turnTintT_ = 0.0f;    // 0→1 over kTurnTintSeconds
  static constexpr float kTurnTintSeconds = 60.0f;
  // PostStep scratch for DebrisSystem::UntunnelRig: the limp rig's dynamic
  // bodies and where each of them was before the step. Members rather than
  // locals so a limp creature does not allocate twice a tick; cleared and
  // refilled each use, meaningless between calls.
  std::vector<uint64_t> rigHandles_;
  std::vector<Vec3> rigPrevPos_;
  // ---- NPC freefall (MobSystem::UpdateFall) ----
  // The walk driver snaps origin_.y to the probed ground; when the ground is
  // further below than a step, the creature falls under physics.gravity
  // instead of drifting (or, with nothing in reach, hanging). fallVel_ is
  // voxels/s, negative down; airTime_ is what the ragdoll rule reads.
  float fallVel_ = 0;
  float airTime_ = 0;
  bool airborne_ = false;
  // ---- ...and the PLANAR half of it, which only a LAUNCH ever fills -------
  // A walking NPC has no planar velocity state: the drive resolves a whole
  // step against the body's box every tick and there is nothing to carry. A
  // lunge is the one thing that needs one, so `Launch` fills it, `UpdateFall`
  // integrates it against the SAME `fits()` wall test the walk drive uses (a
  // pounce must not tunnel through a rock), and landing clears it.
  //
  // `launched_` is a one-shot latch, and it is not redundant with `airborne_`:
  // UpdateFall's "the ground fell away" branch zeroes `fallVel_`, which would
  // eat the rise of the very jump that set it on the tick the body leaves the
  // ground. The latch says "this air is mine" for that one edge.
  Vec3 airVel_{};
  bool launched_ = false;
  // What this creature is looking at between strokes (Mob::SetAimLook).
  Vec3 aimLook_{};
  bool aimLookValid_ = false;
  // ---- the directional flinch (Mob::HitReact / Mob::ApplyHitReact) --------
  //
  // `lean` is RADIANS about the rig's own axes: .x pitches the body about
  // model +X (positive takes +Y toward +Z, the facing direction) and .z rolls
  // it about model +Z (positive takes +Y toward -X). .y is unused — a blow
  // does not spin you about your own spine, and pretending it does reads as a
  // creature shrugging.
  //
  // `push` is PREFAB VOXELS in the rig's own frame, added to the root's local
  // position. The feet are IK'd to world points the gait planted, so the body
  // moves and the legs take it up: the lurch is absorbed rather than skated.
  //
  // `limb` is PER PART and parallel to `skel_.parts`, sized on first use. One
  // spring per limb rather than one spring and an index because a second blow
  // on a different arm would otherwise steal the first one's spring and snap
  // it home; 15 parts of 24 bytes is not worth a rule nobody can see on screen.
  struct HitReactState {
    SpringState lean;
    SpringState push;
    std::vector<SpringState> limb;
    // The gate on the whole layer. Set by HitReact, cleared by ApplyHitReact
    // the tick every spring is quiet — so a creature nobody is hitting pays
    // one bool test per tick and not one spring step.
    bool live = false;
  } hitReact_;
  // ---- this body's terrain budgets, in CELLS (anim.h LocomotionDef) -------
  // Authored in metres per rig and resolved once in BuildRig. THE ONE COPY:
  // the walk drive's footprint collider, the 8-way sense fan, the freefall
  // test and the A* planner all read these, so "how big a ledge is a wall" has
  // a single answer per creature. They were three unrelated constants —
  // kMobStepUpCells in the drive, a literal 2 in behaviors.json for the
  // planner, a literal 3 for headroom — and the first two disagreed with the
  // player's own step budget by 3x, which is why a mob crabbed sideways across
  // slopes the player walks straight up.
  int stepUpCells_ = 2;
  int stepDownCells_ = 5;
  int headroomCells_ = 3;
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
  // Touchdowns since the consumer last drained (see Footfall). Capped, so an
  // NPC nobody listens to costs eight entries and never more.
  std::vector<Footfall> footfalls_;

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
    using ShellIndex = ShellMarchIndex;
    std::vector<ShellIndex> index;
  };
  std::vector<WornPiece> worn_;
  std::vector<LostGear> lostGear_;
  static constexpr size_t kMaxLostGear = 16;
  // The pack (MobDef::loot). Bounded like every other per-mob list: a loot
  // table is content and content can be edited wrong, and a creature carrying
  // ten thousand stacks would be ten thousand strings in every save record and
  // every handoff packet.
  std::vector<CarriedItem> carried_;
  static constexpr size_t kMaxCarried = 32;
  // The gear half of DetachLimb: the held item or a worn piece's identity
  // shell leaving as debris. Runs BEFORE the lattice is handed over, because
  // CaptureWorn reads the shells, and returns the item name to register the
  // adopted body under (empty = not an item).
  std::string ShedGearBeforeDetach(int limbIndex);

  // Held item state — ONE piece of entity<->slot sync, kept only in EquipItem.
  int heldSlot_ = -1;
  std::string heldItem_;
  std::string heldPart_;
  int heldPartIndex_ = -1;
  // THE PART THE STROKE DRIVER IS MOVING (see SetStrikeEffector). Kept beside
  // the held slot rather than derived from it because they are different
  // facts: a zombie holding a sword and biting has both, and the sword must
  // keep hanging in its fist while the jaws do the work.
  int strikeEffector_ = -1;
  StrikeEffectorMode strikeMode_ = StrikeEffectorMode::None;
  int strikeNatural_ = -1;     // index into def_->natural, -1 = the held item
  Vec3 gripBody_{};            // grip point in the item's BODY frame
  // Swing pose pushed in by the driver (SetWeaponPose). Pure presentation.
  WeaponPose weapon_{};
  // The authored attack this creature is executing, if any (game/strokes.h).
  NpcStroke stroke_{};
  // HOW MUCH OF `MobDef::chaseClip` IS ON RIGHT NOW, 0..1, ramped in
  // MobSystem::UpdateAnimation. A weight and not a start/stop, because the
  // pose has to come off INSTANTLY-ish for a strike (an additive hold summed
  // onto an authored punch is a corrupted punch) and back on after it, and a
  // clip retired and restarted twice a second never gets past a fraction of
  // its blend-in — the same mechanism the walk/run hysteresis note in
  // avatar.cpp describes, which is what "arms held out stiff" was the first
  // time round. Driving the instance's `weight` instead keeps one instance
  // alive for the whole pursuit and moves only the number.
  float chasePose_ = 0;
  float weaponWeight_ = 0;     // weapon_.weight, clamped once on the way in
  mutable WeaponArmDiag weaponDiag_{};
  mutable AimDiag aimDiag_{};
  // Render-only rigid translation applied by AppendXforms — see
  // SetRenderOffset. Never read by anything that can feed the sim.
  Vec3 renderOffset_{0, 0, 0};
  mutable Quat weaponHandPreClamp_{}, weaponUpPreClamp_{}, weaponLoPreClamp_{};
  mutable Vec3 weaponHandPosPreClamp_{};
  mutable int weaponHandPart_ = -1, weaponUpPart_ = -1, weaponLoPart_ = -1;
  // ---- THE SELF-CLIP DETECTOR'S CACHE (game/selfclip.h) -------------------
  // Occupancy bitsets per limb and the bind pose's own pair overlaps. Both are
  // facts about the ART, so they are built on the first check and reused; a
  // carve changes a limb's voxels but not by enough to matter to "is this arm
  // in the chest", and `clipShapeGen_` is the limb count the cache was built
  // at, so a severed or appended slot rebuilds it rather than reading past it.
  mutable std::vector<ClipShape> clipShapes_;
  mutable std::vector<int> clipRest_;
  // WHICH SLOTS THE CACHED BASELINE IS FOR. The table is indexed by position
  // in the participating list, not by slot, so a severed limb renumbers every
  // entry after it: keying the cache on the list itself is what stops a stale
  // baseline being subtracted from the wrong pair.
  mutable std::vector<int> clipRestOrder_;
  mutable size_t clipShapeGen_ = (size_t)-1;

  // Particles authored outside the tick (Sever is reached from damage handling
  // all over the frame); drained by the driver's PreTick.
  std::vector<ParticleSpawn> pendingSpawns_;
  // Re-entrancy guard: FlushBurn expresses itself as a CarveLimb, and
  // CarveLimb flushes before it reads the lattice.
  bool inBurnFlush_ = false;
  // ---- "THESE HOLES ARE OLD" -------------------------------------------------
  // Set for the duration of RotAtSpawn's carves, and read by the same line that
  // already refuses to bleed a burning limb or a garment: a creature born
  // bitten is not bleeding from those bites. Without it a zombie would arrive
  // haemorrhaging from every hole it has ever had and paint the ground red the
  // moment it walked into view, which is the opposite of what "already
  // wounded" should look like. The HP CHARGE is deliberately outside it, for
  // the reason the burn exclusion gives: the damage is real, only the blood is
  // refused.
  bool inSpawnRot_ = false;
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
  // ---- "THIS CARVE IS A DENT" / "THIS CARVE IS A TEAR" ---------------------
  // Set by Mob::BluntCarveScope / Mob::BiteScope (see the notes there) for the
  // duration of the carve each entry point makes. Both are MOB-level for the
  // same reason `inBladeCut_` is: the avatar has no MobSystem at all, and an
  // NPC punching the player has to reach the same rules.
  bool inBluntCarve_ = false;
  bool inUnarmedBlunt_ = false;
  bool inBite_ = false;

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
  // ...and the readers, so a SECOND MobSystem can be stood up against the same
  // shared pools. Only the `mob-handoff` gate does that today, and it has to:
  // "the record is self-contained" is not a claim you can make against the
  // system that wrote it, which still holds the live creature.
  MicroBodySet* MicroSet() const { return microSet_; }
  const std::shared_ptr<MobDefFactory>& DefFactory() const {
    return defFactory_;
  }
  void OnMaterialsReloaded(const std::vector<MaterialDef>& mats,
                           const std::vector<ReactionGpu>& reactions);
  // This tick's integer day phase, so a day/night-gated reaction behaves the
  // same on a limb as it does in the grid. Unset means night; see the same
  // setter on DebrisSystem.
  void SetDayPhase(uint32_t phase) { dayPhase_ = phase; }
  void SetDefs(std::vector<MobDef> defs);           // hot reload
  const std::vector<MobDef>& Defs() const { return defs_; }
  // The loader's leftovers, so this system can build one more creature after
  // the load (see DefWithEffects). Handed over beside SetDefs; without it
  // every call below that would COMPOSE a def fails loudly instead.
  void SetDefFactory(std::shared_ptr<MobDefFactory> fac) {
    defFactory_ = std::move(fac);
  }
  int FindDef(const std::string& name) const;
  // The same lookup, except that a name of the form `<base>+<fx>+<fx>` that
  // matches no file is COMPOSED (DefWithEffects). This is the whole of the
  // save story for a creature that turned: SaveState writes the def name it
  // always wrote, and a name is enough to rebuild the recipe, so a world full
  // of zombies-of-somebody round-trips with no version bump and no field that
  // only one producer can set.
  int FindOrComposeDef(const std::string& name);

  // ---- BECOMING SOMETHING ELSE ---------------------------------------------
  //
  // `base`, with `fx` poured on it — the runtime form of what
  // `assets/mobs/jujunud_zombie.json` says on disk. Returns a def index, or -1.
  //
  // THE ORDER OF PREFERENCE IS THE WHOLE DESIGN:
  //
  //   1. `base` already carries every effect asked for -> `base` itself. A
  //      zombie bitten by a zombie is the same zombie.
  //   2. A def already loaded whose recipe IS this one (`extendsName` == base,
  //      same effect set) -> that def. An AUTHORED combination wins over a
  //      composed one, so a hand-tuned `jujunud_zombie.json` is what a turning
  //      jujunud becomes, and the content author keeps the last word.
  //   3. Otherwise compose it: resolve base's sidecar with the extra effects
  //      (sidecar::LoadWithEffects), build it through the same BuildMobDef
  //      every file goes through, and append it as `<base>+<fx>`.
  //
  // A composed def costs one entry in the shared micro pool per limb and never
  // goes away, so the number of them in a session is CAPPED (kDerivedDefs);
  // past the cap the call fails and says so rather than growing the pool
  // without bound. Deduplication by recipe is what keeps the cap generous: a
  // hundred villagers turning cost one def, not a hundred.
  int DefWithEffects(const std::string& base,
                     const std::vector<std::string>& fx, std::string* log);

  // A LIVE MOB BECOMES ITS OWN VARIANT. Same place, same facing, same limbs
  // gone, new def — the creature is respawned onto DefWithEffects(its def, fx)
  // and its sever state is carried across, which is exactly what LoadState
  // does for a saved mob and for the same reason: everything else about a body
  // is derived from the def, and this body's def just changed.
  //
  // Returns the NEW mob id (0 = nothing turned; the old mob is untouched).
  // The old mob leaves without a corpse: it did not die, it got up.
  uint64_t TurnMob(uint64_t mobId, const std::vector<std::string>& fx);

  // ---- THE CORPSES THAT ARE GOING TO GET UP --------------------------------
  //
  // A body that died with the rot in it (MobDef::Turn) books a rising here on
  // its way out, and PreTick services it when the clock comes round: the
  // remains are taken out of the world and the creature stands up as a variant
  // of ITSELF. The queue exists because a corpse is not a mob — its limbs
  // belong to DebrisSystem the moment it dies — and because nothing may spawn
  // a mob from inside Mob::Die, which is running on a Mob that lives in the
  // vector the spawn would push to.
  //
  // THE AVATAR BOOKS ONE TOO. The player's body goes to DebrisSystem through
  // exactly the same path, so "you die of the bite and your own corpse gets up
  // as a zombie of you" needs no player-specific code at all — you respawn,
  // and the thing wearing your face is an NPC.
  //
  // NOT SAVED, deliberately. A rising names debris handles, and the corpse it
  // names travels in the 'DBRS' section as the debris it already is — so a
  // save taken in the six seconds between the death and the rising loads back
  // as a world with a corpse in it and nothing pending. That is the reading
  // this queue is worth: the alternative is a save format for a body's
  // intentions, and the whole point of the design above is that a creature's
  // recipe lives in its NAME and not in a format.
  size_t PendingRisings() const { return rises_.size(); }
  void ClearRisings() { rises_.clear(); }  // test fixtures (Reset does it too)
  // Tear down every mob. `rewindIds` also restarts the id counter, which is a
  // TEST-ONLY seam: mob ids seed gore variance and the blast crater's noise, so
  // rewinding them changes how the next creature bleeds and tears. See the note
  // at the definition.
  void Reset(bool rewindIds = false);                                      // world regen

  // ---- NPC behaviour (game/ai_behavior.h) ---------------------------------
  // The profile library, hot-reloaded from assets/mobs/behaviors.json on R.
  // Setting it re-resolves every LIVE mob's profile by name, so an edit is
  // visible on the creatures already standing there rather than only on the
  // next ones spawned (the same contract RefreshGoreProfiles has).
  void SetBehaviors(ai::Library lib);
  const ai::Library& Behaviors() const { return behaviors_; }
  // Mutable, for the dev panel's live sliders: they write THROUGH to the
  // in-memory profile so every mob on it updates at once. There is deliberately
  // no per-mob override — "this one duelist is braver" is variance, and this
  // engine already has a place for that (DESIGN.md, per-instance tuning).
  ai::Library& BehaviorsMut() { return behaviors_; }
  // Switch one live creature's profile by name ("" clears it back to the legacy
  // wander). Returns false if the mob or the profile does not exist.
  bool SetMobBehavior(uint64_t mobId, const std::string& name);
  // The behaviour layer's live state for one creature, or null. The dev panel
  // and the debug viz read the whole Brain rather than a dozen accessors —
  // it IS the introspection surface, and every field on it is already
  // presentation state.
  const ai::Brain* MobBrain(uint64_t mobId) const;

  // WHO THE MOBS ARE FIGHTING. Pushed in once per tick by the frame loop with
  // the player's capsule; the selftest pushes a scripted point instead, which
  // is why this is a real seam and not a test hook. Mob-vs-mob targeting needs
  // no further API: PreTick already adds every live mob to the same actor list,
  // so two factions fight the moment two profiles disagree about `faction`.
  //
  // A LIST, NOT A SLOT (docs/PLAN_multiplayer_now.md N5). One capsule per
  // player: `players[i].id` is `ai::kPlayerActorBase + i`, a band disjoint from
  // mob ids (which are a monotonic counter from 1 and are NOT moved -- they are
  // in the save format). The line that used to stand here claimed "mob ids
  // still start at kMaxPlayers"; there is no kMaxPlayers and there never was,
  // and the low-band scheme it described collided player 1 with the first mob
  // spawned. See ai::kPlayerActorBase for why the band is high instead.
  // Nothing downstream asks what KIND of thing an actor entry is, so the whole
  // targeting layer is already multi-player; only the plumbing was singular.
  struct PlayerActorDesc {
    Vec3 centreVox;
    float radius = 0;
    float height = 0;
    bool alive = false;
  };
  void SetPlayerActors(std::span<const PlayerActorDesc> players);
  void ClearPlayerActors() { playerActors_.clear(); }
  // One player is a span of one. Kept because the harness fixtures and the
  // single-session frame loop read better this way, not because the storage
  // is singular.
  void SetPlayerActor(Vec3 centreVox, float radius, float height, bool alive) {
    const PlayerActorDesc one{centreVox, radius, height, alive};
    SetPlayerActors(std::span<const PlayerActorDesc>(&one, 1));
  }
  void ClearPlayerActor() { ClearPlayerActors(); }

  // ---- THE PLAYER AS A TARGET ---------------------------------------------
  //
  // The avatar is a Mob (game/avatar.h) but it is NOT in `mobs_`: it is owned
  // by main.cpp and driven by the player. Every body-handle-keyed entry point
  // below therefore missed it, and the frame loop worked around that by asking
  // `avatar.Damage(...)` first and `mobs.Damage(...)` second at each of its own
  // call sites. That workaround does not reach INSIDE MeleeSweepDamage — which
  // is one function with three callers by design — so an NPC's sweep found the
  // player's limbs, failed to recognise them, and melted them as debris.
  //
  // Registering the avatar here is the fix, and it is the smaller change: the
  // four handle-keyed lookups (FindLimb, FindOwner, Damage, CutLimb,
  // CarveLimbRadial) consult it AFTER the mob list, so every existing caller
  // that already checked the avatar first is bit-identical, and the one caller
  // that could not check it now works.
  //
  // Deliberately NOT extended to CarveMobsRadial: that one is position-keyed
  // (the explosion path) and the avatar already has its own CarveRadial call
  // beside it. Routing it here as well would carve the player twice.
  //
  // A LIST, NOT A SLOT (N5), for the same reason the actor list is: every one
  // of the five lookups below already walks `mobs_` and then falls through, so
  // "and then every registered avatar" is the same loop with a different
  // container. With one entry it is bit-identical to the single pointer it
  // replaces. The avatars are NOT owned: each is a member of a PlayerSession.
  void SetAvatars(std::span<Mob* const> avatars) {
    avatars_.assign(avatars.begin(), avatars.end());
  }
  void SetAvatar(Mob* avatar) {
    avatars_.clear();
    if (avatar) avatars_.push_back(avatar);
  }
  // The LOCAL player's avatar, or null. Still singular on purpose: the render
  // path and the character screen draw one body, and that body is this one.
  Mob* Avatar() const { return avatars_.empty() ? nullptr : avatars_[0]; }
  const std::vector<Mob*>& Avatars() const { return avatars_; }
  // The registered avatar carrying this mob id, or null. The id-keyed half of
  // the same "and then every registered avatar" fall-through the handle-keyed
  // lookups do; one function so the twelve callers cannot drift.
  Mob* AvatarById(uint64_t mobId) const {
    for (Mob* av : avatars_)
      if (av && av->Id() == mobId) return av;
    return nullptr;
  }
  // Is this body a PLAYER's rather than an NPC's? Asked where a rule must not
  // fire for the player (the corpse report, so far).
  bool IsAvatar(const Mob* m) const {
    for (Mob* av : avatars_)
      if (av == m) return true;
    return false;
  }
  // WHICH CREATURE OWNS THIS BODY, and which of its rig slots it is. The
  // three-way flesh/garment/weapon classification the parry needs is then the
  // rig's own: `< owner->AppendedBase()` is flesh, `== owner->HeldSlot()` is
  // the weapon, and anything else appended is a worn shell.
  Mob* FindOwner(uint64_t bodyHandle, int* outLimbIndex = nullptr);

  // ---- BLADE ON BLADE (game/melee.h BlockEvent) ---------------------------
  // Reported the way SeverEvents and VoiceEvents are, and for the same reason:
  // this system knows nothing about audio or particles. Phase D turns each into
  // a clang and a spark.
  //
  // ONE LIST FOR BOTH DIRECTIONS. MeleeSweepDamage pushes here whoever swung,
  // so an NPC parried by the player and a player parried by an NPC arrive
  // through the same drain — a second list keyed by "was it the player" is
  // exactly the kind of split that rots.
  //
  // Pushing also APPLIES the defender's shove, because MobSystem is the only
  // thing that can reach an NPC's stroke; a block whose defender is the avatar
  // is left for main.cpp, which owns that MeleeState.
  // ---- IS SOMEBODY ELSE'S BLADE IN THE PATH OF THIS SWEEP? ----------------
  //
  // GEOMETRY, NOT A RAY CAST, and the reason is measured rather than stylistic.
  // MeleeSweepDamage finds what it hits by casting rays along the swinging
  // blade's own axis, which is exactly right for a BODY and useless against
  // another BLADE: a sword's collider is the item's own art at the item's own
  // scale, about a QUARTER OF A VOXEL thick, and a zero-radius ray through a
  // quarter-voxel slab is a coincidence rather than a test. Measured in
  // `npc-block`: two edges passing within 0.28 voxels of each other, seven
  // sweeps run, zero rays finding the blade — and a ray fired deliberately
  // straight down the defender's own edge segment came back empty too.
  //
  // So the question is asked of the two EDGE SEGMENTS, which are the
  // authoritative hitboxes anyway (melee.h note 1, "the pose is the hitbox"):
  // if the quad this sweep covers passes within `gap` of another creature's
  // held edge, that blade is in the way. Cheap and bounded — kMaxMobs
  // creatures, a fixed sample grid, and only on a tick that is actually
  // cutting.
  //
  // `wielder` is excluded; so is anything not holding an item. Returns the
  // closest such blade, its body handle, and where the two met.
  bool FindParry(const Mob& wielder, const Vec3& aPrev, const Vec3& bPrev,
                 const Vec3& aNow, const Vec3& bNow, float gap, Mob*& outBlocker,
                 uint64_t& outBody, Vec3& outAt);
  void PushBlockEvent(const BlockEvent& ev, const MeleeTuning& t);
  const std::vector<BlockEvent>& BlockEvents() const { return blocks_; }
  void ClearBlockEvents() { blocks_.clear(); }

  // ---- THE OTHER TWO KINDS OF BLOW (game/impact.h) ------------------------
  // The system-level twins of Damage/CutLimb: resolve the handle against the
  // mob list and then against the registered avatar, so an NPC's mace reaches
  // the player for the same reason its sword does (see SetAvatar). Both return
  // true when the handle was somebody's live limb.
  bool BluntHit(uint64_t bodyHandle, const ::BluntHit& hit, World& world,
                std::vector<ParticleSpawn>& spawns);
  bool BiteHit(uint64_t bodyHandle, const ::BiteHit& hit, World& world,
               std::vector<ParticleSpawn>& spawns);
  // ---- A MATERIAL, BY NAME, AT RUNTIME ------------------------------------
  //
  // Tuning cannot hold a material id: tuning.json is hot-reloaded on F5 and
  // materials.json on R, independently, and a compiled-in id would be stale
  // after either. So `gore.bruiseMat` is a NAME (the one name-typed tuning row
  // in the file, and the note at its declaration says why), resolved here
  // against the table this system already mirrors for the burn pass.
  //
  // Linear over ~130 short strings, called at most once per blow. A map would
  // be a second thing to invalidate on reload for no measurable gain; the
  // MISS is what is cached instead, because a typo'd name would otherwise pay
  // the scan on every punch forever.
  uint32_t MaterialIdNamed(const std::string& name) const;

  // ---- authored attack styles (game/strokes.h) ----------------------------
  // Hot-reloaded from assets/mobs/attack_styles.json on R, exactly as the
  // behaviour library is. A live stroke keeps the style INDEX it started with,
  // which is safe for the same reason a live Brain keeps its profile index
  // between reloads: both are re-resolved on the next attack.
  void SetAttackStyles(StyleLibrary lib) { styles_ = std::move(lib); }
  const StyleLibrary& AttackStyles() const { return styles_; }

  // ---- DRIVING A SWING BY HAND --------------------------------------------
  //
  // The same kind of seam `SetDesiredHeading` already is, and documented the
  // same way: for the gates now, and for scripted encounters later. Neither
  // bypasses any mechanic — ForceAttack runs the identical stroke program the
  // AI's request would have started, and SetGuard is the windup's own drive
  // held open — so a test that uses them is testing the shipping path.
  //
  // ForceAttack: swing `style` at `targetPoint`, now. False if the creature has
  // no weapon, the style is not loaded, or a stroke is already live (a queued
  // swing is an unbounded backlog, rule 2 — see the note at PreTick's call).
  //
  // `seed`, when non-zero, REPLACES the (mob, tick) hash every draw in the
  // stroke keys off -- the style pick's bow and, crucially, the TEMPO JITTER
  // that sets the windup and cut tick counts. A gate that wants the same swing
  // whatever ran before it passes one; the game passes nothing and keeps the
  // variation it is there for. See the note at the call site.
  // `targetId`, when non-zero, is WHO is being hit -- which is what lets a
  // scripted swing draw the style's `target` limb (MobSystem::PickTargetLimb).
  // Zero aims at the point alone and takes today's chest, which is what a
  // caller with no victim in mind wants.
  bool ForceAttack(uint64_t mobId, const std::string& style, Vec3 targetPoint,
                   uint32_t tick, uint32_t seed = 0, uint64_t targetId = 0);
  // SetGuard: hold the blade at a stated azimuth/elevation in the creature's
  // OWN facing basis, with the point pushed out to `reachFrac` of the arm's
  // reach. Held until ClearGuard or until an attack replaces it. This is how a
  // defender comes to have its sword across a line without attacking — which
  // is the whole of emergent blocking from the defender's side.
  bool SetGuard(uint64_t mobId, float az, float el, float reachFrac);
  void ClearGuard(uint64_t mobId);
  // ---- HOW FAR THIS CREATURE CAN REACH, THIS TICK (plan §5) ---------------
  //
  // `max(profile.reach, the longest reach among the styles it can USE)`. The
  // AI layer stays style-ignorant (ai_behavior.h's AttackTuning::styles note
  // says why at length), so a lunging bite's 22-voxel reach cannot live in the
  // profile: the creature would stand off at 22 voxels with a sword too. It is
  // computed here, where both the library and the rig are visible, and handed
  // into `ai::Think` as an INPUT on SelfView.
  float AttackReachOf(const Mob& mob) const;
  // The same maximum WITHOUT the profile's `attack.reach` as a floor: what this
  // body can actually hit with right now, 0 for "no opinion". The footwork band
  // is placed on it (ai_behavior.h SelfView::strikeReach).
  float StrikeReachOf(const Mob& mob) const;
  // ---- HOW FAR THIS STYLE CAN ACTUALLY LAND (2026-09-15) -----------------
  //
  // World voxels, centre-to-centre, DERIVED FROM THE BODY: the effector's own
  // reach (an arm plus its edge, or a neck lean plus the jaws) plus whatever
  // the style's lunge closes, plus a body's half-depth for the victim it is
  // walking into. A style's authored `reach` OVERRIDES it when non-zero.
  //
  // WHY THE DEFAULT IS DERIVED AND THE STYLES NOW AUTHOR ZERO. Every natural
  // style shipped a hand-guessed number and every one of them was a lie the
  // rig could not keep: `punch_r` claimed 9 on an arm that reaches 5, so the
  // AI committed from nine voxels out and the fist stopped 6.7 short of the
  // victim -- measured through `--shot-strike`, which is what found it. A
  // number an author cannot check against the rig is a number that rots the
  // moment the art changes, and the rig already knows the answer.
  float StyleReachOn(const Mob& mob, const AttackStyle& sty) const;
  // ---- WHICH LIMB A BLOW IS AIMED AT (strokes.h StyleTargetWeight) --------
  //
  // Draws from the style's `target` weights over the victim's LIVE BASE limbs,
  // counter-based on (attacker id, tick) like every other variation in a
  // stroke. -1 = no table, no live limb wearing one of its tags, or nothing
  // left — which every caller reads as today's chest.
  //
  // PUBLIC AND STATIC because two things outside BeginStroke need the same
  // answer: the `bite-target` gate, which states its claim about the DRAW
  // rather than about where wounds ended up, and (later) the Attacks lane's
  // target-weight row. A MobSystem member rather than a free function so it
  // can ask `Mob::LimbAlive`, which is protected.
  // `attackerProne` is the ATTACKER's `Mob::LocoGroundAlign()` -- above 0 and
  // the style's `targetProne` table is drawn from instead of its `target` one,
  // so a crawler goes for what a crawler can reach. Pass 0 for "upright".
  static int PickTargetLimb(const AttackStyle& sty, const Mob& victim,
                            uint64_t attackerId, float attackerProne,
                            uint32_t tick);
  // The item library, so an NPC's sweep can read the damage, carve bonus and
  // HEFT of whatever is in its fist. By POINTER and not owned: items reload on
  // R and a copy here would be a second, stale library. The Mob stores its held
  // item BY NAME (item.h's index hazard), so the resolve happens per swing.
  void SetItems(const ItemLibrary* items) { items_ = items; }
  // ...and the read back, so a Mob can resolve the worn piece over its own
  // fist (Mob::StrikeProfileFor's gauntlet override). By POINTER and possibly
  // null: a gate that builds a MobSystem without a library must get "no
  // override" rather than a crash.
  const ItemLibrary* Items() const { return items_; }
  // A PIECE OF GEAR HITTING THE FLOOR. Called with the debris body a worn
  // piece's identity shell or a held item became on leaving a rig, and the
  // item's NAME — the one seam by which main.cpp's WorldItems learns that a
  // body it did not drop is a thing you can pick up (Mob::LostGear). Not
  // fired for a shell consumed by fire (there is no body) nor for the rags a
  // piece sheds beside its identity shell.
  // The third argument is the piece's DYE (game/dye.h), 0 for undyed and for
  // everything that is not a coloured garment. The body already RENDERS in it
  // (MicroBodyRef::dye travelled with the adopt); this is what lets the ground
  // registry hand the same colour back when somebody picks it up.
  void SetOnItemShed(
      std::function<void(uint64_t, const std::string&, uint32_t)> cb) {
    onItemShed_ = std::move(cb);
  }
  // A CREATURE FELL WITH THINGS ON IT. Called once from Mob::Die with the
  // report above, after the limbs have become debris, so the handles in it
  // are already bodies the receiver can destroy or look up. See CorpseReport.
  void SetOnCorpse(std::function<void(const CorpseReport&)> cb) {
    onCorpse_ = std::move(cb);
  }
  // ---- WHAT THE PLAYER IS CARRYING, ASKED AT THE MOMENT THEY DIE ----------
  //
  // The avatar's pack is not on the avatar. Bag, hotbar and equipment live in
  // PlayerKit on PlayerSession (game/session.h's rule: per-player state is not
  // a process global), and MobSystem cannot reach a session — deliberately,
  // since there may be two of them. So the seam is the same shape as
  // SetOnCorpse, pointing the other way: Die() ASKS, the session ANSWERS.
  //
  // Called only when the avatar's corpse is actually going to get up, and only
  // on the machine that owns it. `out` receives the stacks to hand the risen
  // body.
  //
  // THE CALLBACK DECIDES WHETHER THE PLAYER KEEPS THEM. With
  // `avatar.keepKitOnTurn` (the default, and the dev-mode reading) it copies
  // and the kit is untouched; with it off it MOVES — bag, hotbar and equipment
  // are emptied on the way out, so what gets up is wearing your armour and you
  // respawn with nothing. That flag is the whole of the difference between
  // "your death spawns a second copy of your gear" and "your gear walks away",
  // and it is a flag rather than a decision because the first one is what you
  // want while testing and the second is what you want in a game.
  void SetAvatarKitFn(std::function<void(std::vector<CarriedItem>&)> cb) {
    avatarKitFn_ = std::move(cb);
  }
  bool HasAvatarKitFn() const { return (bool)avatarKitFn_; }

  // ---- the attack seam (Phase C consumes this) ----------------------------
  // Requests issued this tick. The AI decides WHEN and WHERE; it never swings,
  // plays a clip, or deals damage. Drained by the consumer exactly as
  // SeverEvents/VoiceEvents are, and bounded by construction: at most one per
  // mob per tick, kMaxMobs mobs, and the attack cadence is ticks apart.
  const std::vector<ai::AttackRequest>& AttackRequests() const {
    return attacks_;
  }
  void ClearAttackRequests() { attacks_.clear(); }

  // Spawn def at a world cell (mob min corner; caller picks ground). 0 = fail.
  uint64_t Spawn(int defIndex, IVec3 atVoxel);
  // The live creature record, or null. The Mob API (damage, carve, ignite,
  // equip...) is the per-creature surface; the id-keyed wrappers below remain
  // for callers that only hold a body handle or an id.
  Mob* FindMobById(uint64_t id);
  // A creature by TARGET id, which includes the players: an avatar is a Mob
  // outside `mobs_` and its actor id is in the ai::kPlayerActorBase band. Use
  // this wherever the id came from the AI; `FindMobById` where it did not.
  // See the note on the definition for what the missing case cost.
  Mob* FindCombatantById(uint64_t id);
  // Mob combat scaffolding: hand any creature an item, exactly as the player
  // equips one (the implementation is Mob::EquipItem, shared with the avatar).
  bool EquipItem(uint64_t mobId, const ItemDef* item,
                 const char* context = "held_right");
  // The same scaffolding for ARMOUR: any creature can be dressed through the
  // identical path the player uses (Mob::WearItem). Nothing calls these yet —
  // AI that chooses to put a helmet on is out of scope — but the API is what
  // makes "the goblin is wearing the helmet it dropped" content rather than a
  // feature.
  bool WearItem(uint64_t mobId, const ItemDef* item, int equipSlot,
                uint32_t dye = 0);
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
  // severs regardless of remaining hp. Starts NO clip — the visible answer to
  // a blow is HitReact below, not a keyframed pose.
  bool Damage(uint64_t bodyHandle, float amount, Vec3 hitWorldVoxel,
              float impactSpeed = 0.0f);

  // THE DIRECTIONAL HALF OF A LANDED BLOW (Mob::HitReact). By body handle for
  // the reason Damage is: the melee sweep knows a Jolt body and a travel
  // direction, and has no business knowing which creature owns either. Routes
  // through FindOwner, so it reaches NPCs and the player's own avatar through
  // exactly one path. Returns true when the handle belonged to a live rig.
  //
  // `dirWorld` is the direction the WEAPON was travelling — not the vector
  // from attacker to victim. The two agree for a thrust and disagree for every
  // swing, and it is the swing that has to look right: a horizontal cut that
  // came across the body pushes you sideways, which is the read the vector to
  // the attacker throws away.
  bool HitReact(uint64_t bodyHandle, Vec3 dirWorld, float hp, float power);

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
  // The blast's OTHER half: knock every creature in reach off its feet
  // (Mob::BlastRadial). Runs after the carve, on what survived it. Returns
  // how many were knocked down.
  int BlastMobsRadial(Vec3 centerWorldVoxel, float radiusVoxels,
                      float impulseKgMs);
  // Mob::AppendLiveLimbBodies over every live NPC.
  void AppendLiveLimbBodies(std::vector<uint64_t>& out) const;
  // Dev panel / tests: put one creature (or every live one) on the floor.
  bool RagdollMob(uint64_t mobId, float minSeconds);
  int RagdollAll(float minSeconds);
  // Introspection for the gates: 0 none, 1 limp, 2 getting up; -1 no such mob.
  int RagdollPhaseOf(uint64_t mobId) const;
  Vec3 MobRootPos(uint64_t mobId) const;
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
  //
  // 2 (2026-09-13): the coat word. PrefabVoxel::stain and DebrisVoxel::stain
  // went from a byte holding a palette slot to 16 bits holding a MATERIAL, and
  // both lattices are written as PODs — the stride moved, so a version-1
  // section cannot be read and is refused as it already is.
  //
  // 3 (2026-09-22): the pack. Every record gained a count-prefixed list of
  // carried stacks (Mob::carried_, MobDef::loot) after its limbs, so this is
  // the first inventory the mob format has ever held — until now a creature's
  // gear was not saved at all and a reloaded villager was re-dressed from
  // outside. Appended at the end of the record, but a version-2 reader cannot
  // skip what it does not know is there, so the bump is real and the refusal
  // stays a refusal. THE HANDOFF PACKET MOVES WITH IT (see below): both sides
  // of a session must be the same build, which they already had to be.
  static constexpr uint32_t kSaveVersion = 3;
  void SaveState(std::vector<uint8_t>& out) const;
  // Contract (worldio LoadEntities): Reset() has already run.
  bool LoadState(const uint8_t* data, size_t len, uint32_t version);
  // ONE creature out of such a section: spawn it from its def name and overlay
  // the saved damage, exactly as LoadState's loop body did (it IS that loop
  // body). Returns the live mob, or null if the def is gone or the spawn was
  // refused — both of which it reports the way LoadState always has.
  //
  // Public because the handoff reads one record out of a wire packet rather
  // than a count-prefixed section: `SaveState`/`LoadState` are now loops over
  // this pair, and the network path is the same pair called once.
  Mob* LoadOne(ByteReader& r, uint32_t version);

  // ==== OWNERSHIP: who steps which creature (M9.4-B) ========================
  //
  // Default: every mob is local and nothing here is reachable. The network
  // layer (M9.4-D) sets the local id at join and installs an ownership
  // function computed from net::Authority; until then `ownershipFn_` is null,
  // `RefreshOwnership` is a no-op, and PreTick runs today's three branches for
  // every creature — which is why this package does not move the world hash.
  static constexpr uint32_t kLocalOwner = 0;
  void SetLocalPlayerId(uint32_t id) { localPlayerId_ = id; }
  uint32_t LocalPlayerId() const { return localPlayerId_; }
  // (mobId, feet position in world voxels) -> the playerId that should own it.
  // Evaluated once per creature at the top of PreTick, so a mob's ownership
  // cannot change underneath the tick that is stepping it.
  void SetOwnershipFn(std::function<uint32_t(uint64_t, Vec3)> fn) {
    ownershipFn_ = std::move(fn);
  }
  bool HasOwnershipFn() const { return (bool)ownershipFn_; }
  uint32_t MobOwner(uint64_t mobId) const;
  // Set one creature's owner directly. The handoff path and the gate use it;
  // the ownership function overrules it on the next PreTick, which is correct
  // — the function IS the shared answer both machines compute.
  bool SetMobOwner(uint64_t mobId, uint32_t owner);
  uint32_t GhostCount() const;
  // WOULD ANOTHER BODY STOP `mobId` STANDING AT (cx, cz)? The drive's own
  // hard test (`BlockedByMob`), asked from outside. An introspection surface
  // in the same sense `MobBrain` is one: it decides nothing, it reports what
  // the drive would decide, and it is the only way to ask "is that creature
  // SOLID" without walking something into it for forty ticks. The ghost gate
  // needs exactly that — a body somebody else drives must still be something
  // you cannot walk through.
  bool BlockedByMobAt(uint64_t mobId, float cx, float cz) const;
  // HOW MANY TIMES THE AI ARBITER HAS RUN, over the life of this system. A
  // diagnostic, and the one a ghost gate cannot do without: "the ghost did not
  // move" has a dozen causes and only one of them is "nothing stepped it", so
  // the gate asserts on the counter rather than on a position (CLAUDE.md rule
  // 6 — record it at the point of the decision, do not infer it later).
  uint64_t AiSteps() const { return aiSteps_; }

  // ---- MOB IDS ARE A PER-PLAYER BAND --------------------------------------
  //
  // `nextId_` is a monotonic counter from 1 and mob ids are in the SAVE
  // format, so two machines each spawning from 1 hand two different creatures
  // the same identity — and every wire record here is keyed by id. The client
  // starts its counter at `playerId << 40 | 1`: 2^40 ids per player, the host
  // (id 0) keeps today's numbering so a single-player save is unchanged, and
  // `ai::kPlayerActorBase` (1<<62) is above every band.
  //
  // max(), never assignment: a LoadState that restored a higher counter (a
  // long-lived host world) must not be pulled back into a range it has
  // already issued.
  void SetIdBand(uint32_t playerId);

  // ---- THE FOUR WIRE RECORDS (net/mobsync.h) ------------------------------
  //
  // Build on the owner, apply on the ghost. None of these touch a socket:
  // M9.4-D owns the sending, and keeping that out of here is what lets the
  // `mob-handoff` gate drive the whole feature with two MobSystems and a
  // vector of bytes.
  bool BuildAnnounce(uint64_t mobId, ::net::MobAnnounce& out) const;
  // `tick` is stamped on the record so the receiver can drop an out-of-order
  // pose rather than rewinding a creature.
  bool BuildPose(uint64_t mobId, uint32_t tick, ::net::MobPose& out) const;
  // Latches the pose on the ghost; it is applied in PreTick (TickGhost), not
  // here, so a packet that arrives mid-frame cannot move a body between the
  // physics step and the draw. Returns false for an unknown id, a pose OLDER
  // than the one already latched, or a mob this machine owns.
  bool ApplyPose(const ::net::MobPose& p);
  // I own it and I am giving it away: fills `out` with everything the new
  // owner needs (record + brain + gear) and makes it a ghost held at its last
  // pose. False if I do not own it or it does not exist.
  bool TakeHandoff(uint64_t mobId, uint32_t newOwner, ::net::MobHandoff& out);
  // The other end. An id I already hold as a ghost is overlaid IN PLACE (the
  // rig, bricks and Jolt bodies are kept — a handoff must not flicker); an id
  // I have never seen is spawned from the record. Returns the live mob.
  Mob* ApplyHandoff(const ::net::MobHandoff& h);
  // A CREATURE THE PEER HAS ALWAYS OWNED (M9.4-E).
  //
  // `ApplyHandoff` is the only other thing that spawns, and it spawns a mob
  // this machine then OWNS — so before this existed the only creature a peer
  // could show you was one it had given away. Everything it merely kept was a
  // stream of poses addressed to an id nobody held: the two-process smoke
  // counted 144 refused poses (`misses=144`) for exactly that.
  //
  // What an announce carries is the SHAPE — a def name and the gear by name —
  // and deliberately not a position, not a rig state and not a wound: the
  // pose that follows it in the SAME batch (net::EntitySync's field order is
  // the apply order) is what places it, and a `MobHandoff` is what later
  // fills in the damage if the creature ever changes hands. So the ghost is
  // born at the origin, unplaced and unposed, and `TickGhost` leaves it
  // exactly there until the first `ApplyPose` lands.
  //
  // IT IS A GHOST FROM BIRTH: `owner_` is the announce's owner, so `IsGhost()`
  // is true before the first PreTick can look at it and no AI step, no
  // locomotion and above all no op is ever authored for a creature this
  // machine does not own. An announce naming ME as the owner is REFUSED
  // (null) rather than spawning a local mob out of a message that carries no
  // state to step — the same disagreement `ApplyPose` refuses.
  //
  // An id I already hold re-applies the GEAR and nothing else (the ghost is
  // already there and re-spawning it would flicker the body); an id I hold
  // and OWN is refused.
  Mob* ApplyAnnounce(const ::net::MobAnnounce& a);
  // The owner says it is over. Drops the ghost and releases its rig; a mob I
  // own is NOT removed by this (the owner of a creature is the only one who
  // may kill it) and the call reports false.
  bool ApplyGone(const ::net::MobGone& g);
  // DRESS A CREATURE FROM AN ANNOUNCE'S GEAR LIST, by name, through the
  // ordinary WearItem/EquipItem. Factored out because `ApplyHandoff` and
  // `ApplyAnnounce` must dress identically — the announce lists gear in
  // RIG-SLOT ORDER (BuildAnnounce sorts it) precisely so that replaying it
  // appends the same slots in the same places on both machines, and two
  // copies of that replay would be two chances to get the order wrong.
  void ApplyWireGear(Mob& m, const std::vector<::net::WireGear>& gear);

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

  // ---- hit flash ----------------------------------------------------------
  // Age every limb's hit flash. Called from PreTick, so it runs wherever the
  // world runs — including in a gate, which is the whole reason it is not on
  // the frame clock (see the long note at the definition: a frame-driven decay
  // is never called by the selftest at all, so a gate that damages a limb
  // leaves it lit for the rest of the process).
  //
  // Exponential, with the halflife from combatfx.flashHalflife. Cost when
  // nothing has been hit is one float compare per limb, and it early-outs on
  // the whole system as soon as every flash has decayed to zero.
  //
  // Public rather than private because a fixture that wants to age a flash
  // without stepping the whole world should not have to run PreTick to do it.
  void DecayHitFlash(float dt);
  // Light one limb up, by BODY HANDLE — the same key Damage() resolves, so a
  // caller that already knows which body it hit does not have to find the limb
  // a second way. Peak-held: a bigger flash arriving while a smaller one is
  // still lit wins, and a smaller one never shortens a bigger one.
  void FlashBody(uint64_t bodyHandle, float amount);

  // Render plumbing: limbs append after the debris bodies' slots.
  bool InstancesDirty() const { return instancesDirty_; }
  // Both return THE NEXT FREE SLOT, which is not `slotBase + LimbBodyCount()`:
  // the walks stop at kMaxBodySlots and the count does not. A caller stacking
  // another system after this one must use the returned value, or it lays that
  // system's parts over somebody else's transforms (game/bodyreg.cpp).
  uint32_t AppendInstances(std::vector<BodyVoxInst>& out, uint32_t slotBase);
  void AppendXforms(std::vector<BodyXformGpu>& out) const;
  // Append this system's micro limbs to the COMPACTED draw list, using the same
  // slot walk as AppendXforms/AppendInstances so the recorded slot is the one
  // the limb's transform lands in — sim/microbody.h.
  uint32_t AppendMicroInsts(std::vector<MicroBodyInstGpu>& out,
                            uint32_t slotBase) const;
  // Every brick record every creature in this system holds, drawn or not.
  void AppendMicroHolders(std::vector<MicroHolder>& out) const;
  // Collision-box debug overlay (world.h DebugBox, the dev panel's "collision
  // boxes" toggle). One oriented wireframe per LIVE limb body, read from the
  // body's actual Jolt collider via Physics::GetLocalBounds — not from the
  // limb's art, so a collider that has drifted from the model it represents
  // shows up as exactly that. Appends; stops at `limit` total.
  void AppendDebugBoxes(std::vector<DebugBox>& out, size_t limit,
                        uint32_t color) const;
  uint32_t LimbBodyCount() const;
  uint32_t MobCount() const { return (uint32_t)mobs_.size(); }
  // The i'th mob record, or null past the end. Read-only: the frame loop uses
  // it to lay trample stamps under every creature (sim/trample.h), which
  // needs a position and a footprint and nothing else about the body.
  const Mob* MobAt(uint32_t i) const {
    return i < mobs_.size() ? &mobs_[i] : nullptr;
  }

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
  // The live stroke of one creature, for the gates and the dev readout. Null
  // when there is no such mob.
  const NpcStroke* MobStroke(uint64_t mobId) const;

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
  // THE BODY'S BOX in world voxels (min corner, max corner) - `origin_` plus
  // the def's `worldSize`. `MobOrigin` alone is the collider's MIN CORNER in
  // x/z and its FEET in y, so a distance measured to it is a distance to the
  // ground between a creature's ankles: that is what made `float aura
  // projectile` do nothing to an enemy (session.cpp's status probe looked for
  // a body within ~3 voxels of the point a bolt resolved at, which for a hit
  // anywhere above the shins is no body at all). Ask for the box and measure
  // to that.
  bool MobBodyBox(uint64_t mobId, Vec3& lo, Vec3& hi) const;
  // Per-tick sustained lift on one body by id (Mob::AddLift). False if no mob
  // has that id - which is how the owner tells a mob impulse from the
  // player's own.
  bool LiftMob(uint64_t mobId, Vec3 velVoxPerSec);
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
  // The body's slope lean (Mob::bodyUp_, MobSystem::UpdateGait). Exposed so a
  // gate can assert on it directly: "the creature is rotated for no reason on
  // a ramp" is an ANGLE, and a test that has to infer it from limb transforms
  // is a test nobody will keep honest.
  Vec3 MobBodyUp(uint64_t mobId) const;
  // The height the body is DRAWN at (Mob::bodyY_), which is not the collision
  // origin and is the one a player can see. A gate that asserts only on
  // `MobOrigin` is asserting the collider; ai-slope learned that the hard way.
  float MobBodyY(uint64_t mobId) const;
  // This body's resolved step-up budget in cells (anim.h LocomotionDef). A
  // gate that hard-codes 2 here is a gate that fails the day a rig is
  // re-authored, which is the trap `AiFlatSpot` exists to avoid for terrain.
  int MobStepUpCells(uint64_t mobId) const;
  // ...and its authored lean ceiling, for the same reason: a gate that pins the
  // tilt to a literal 20 degrees is asserting a number nobody authored. What
  // this engine actually promises is that the lean never exceeds the CAP the
  // rig asked for, so the test compares against the cap plus a slack.
  float MobTiltMaxDeg(uint64_t mobId) const;
  // Steering override for tests and (later) scripted behaviour: sets the
  // desired heading directly, leaving the turn-rate clamp fully in force. The
  // wander behaviour re-takes control as soon as it next wants to turn.
  void SetDesiredHeading(uint64_t mobId, float radians);
  // ...AND THE ONE THAT ACTUALLY TURNS THE BODY, instantly. The scripted-
  // placement seam: "this creature is standing here facing that", which a
  // desired heading cannot express.
  //
  // SetDesiredHeading is NOT a substitute and the difference is not academic.
  // Every intent in ai::Think writes `out.desiredHeading` every tick — Idle
  // writes the CURRENT heading, deliberately, so an idle creature does not keep
  // turning toward a target it had three seconds ago — so a desired heading
  // pushed in from outside is overwritten before the turn-rate clamp has moved
  // the body a degree. Measured: a `training_dummy` told to face its opponent
  // stood with its back to it and held its guard pointing the wrong way down
  // the field, and the block gate reported "0 block events" for a feature that
  // worked.
  void SetHeading(uint64_t mobId, float radians);
  // Gait introspection: how many chains currently have a swinging foot, and
  // how far the planted feet have travelled. The selftest asserts the
  // one-group-swinging invariant per tick rather than comparing step rates
  // (see the frontier-rule testing note: rate comparisons prove nothing).
  int SwingingFeet(uint64_t mobId) const;
  int PlantedFeet(uint64_t mobId) const;
  int ActiveClips(uint64_t mobId) const;
  // Start a named one-shot on a mob by id. False if the mob or the clip is
  // missing. For the gates: since a blow no longer fires a clip (Damage), this
  // is how the clip layer — sample / mask / blend-in / blend-out — gets
  // exercised without a mechanic existing to exercise it.
  bool PlayClip(uint64_t mobId, const std::string& name);
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
  // Mend on a creature, by id (the mob cast it, or it was the target): fills
  // missing anatomy cells with `material`, root-first. Returns how many.
  int RestoreMob(uint64_t mobId, uint32_t material, int count);
  uint32_t MissingVoxelCount(uint64_t mobId, int limbIndex) const;
  // The SKIN lattice's count when the limb has one (the collider above is a
  // majority-fill downsample of it, and a chip one skin voxel deep never
  // reaches it), else the same number as LimbVoxelCount.
  uint32_t LimbSkinVoxelCount(uint64_t mobId, int limbIndex) const;
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
  // How many of this limb's voxels carry a body stain of at least `minAmt`
  // (any type), and how many of THOSE are of material `mat` -- the second is
  // what "the bone is bloodied" is measured as.
  uint32_t LimbStainCount(uint64_t mobId, int limbIndex, uint32_t minAmt) const;
  uint32_t LimbStainedMatCount(uint64_t mobId, int limbIndex, uint32_t mat,
                               uint32_t minAmt) const;
  // Cumulative count of bone voxels this limb's rot has bloodied as it
  // uncovered them (MobLimb::infectBoneCoated). The census above cannot answer
  // that question any more, because the rot converts bone and a coated voxel
  // leaves the census the moment it does.
  uint32_t LimbInfectBoneCoated(uint64_t mobId, int limbIndex) const;
  // ...and the OTHER field on the same voxel: how many cells are WEARING a coat
  // of `coatMat` (0xFFFFFFFF = any) at `minAmt` or deeper. The one above filters
  // on what the voxel IS MADE OF; this filters on what is ON it, which is what
  // "how much of this limb is bruised" means. Mixing the two reports 0 for a
  // thoroughly bruised limb — see the note at the definition.
  uint32_t LimbCoatMatCount(uint64_t mobId, int limbIndex, uint32_t coatMat,
                            uint32_t minAmt) const;
  // The highest WORLD y (voxels) of any voxel of this limb carrying a stain
  // of at least `minAmt`, through the limb's live pose; -1e30 when none. What
  // "a shallow pool stains the ankles and not the thigh" is measured as.
  float LimbStainMaxWorldY(uint64_t mobId, int limbIndex, uint32_t minAmt) const;
  // The world-y span [lo, hi] of this limb's voxels carrying at least
  // `minAmt` (0 = every voxel). False when there are none.
  bool LimbStainWorldYRange(uint64_t mobId, int limbIndex, uint32_t minAmt,
                            float& lo, float& hi) const;
  // The stain palette slot a material leaves (materials.json `stain.type`),
  // 0 when it does not stain. Read off the material table this system already
  // mirrors for the burn pass.
  uint32_t StainTypeOf(uint32_t mat) const;
  // ...and the other direction: the first material registered against a
  // palette slot. The world's voxel word only carries the SLOT, so this is
  // what turns "there is dried blood on this floor" into a substance a body
  // can be coated in. Ambiguous by construction when two materials share a
  // stain name, which is what sharing one means: they look the same and the
  // ground cannot tell them apart either. 0 for an unused slot.
  uint32_t StainMaterialOfType(uint32_t slot) const {
    return slot < 8u ? matOfStainType_[slot] : 0u;
  }
  // ---- what is on a creature (see LimbCoat) --------------------------------
  // The limb's ledger, or nullptr for an unknown creature/limb. Valid until
  // the next recount; do not hold it across a tick.
  const LimbCoat* LimbCoatOf(uint64_t mobId, int limb) const;
  // The same over the creature's BASE limbs only — a robe soaked in blood is
  // not the wearer being covered in it (Mob::IsWornSlot, the rule StainWound
  // already applies).
  LimbCoat BodyCoat(uint64_t mobId) const;
  // The amount-weighted fraction of a creature carrying a coat whose material
  // declares `tag` in its materials.json `coat.effects`. `limbTag` restricts
  // the numerator AND the denominator to limbs with that MobLimbDef::tag
  // ("foot", "hand", ...); nullptr means the whole body. 0 for an unknown
  // creature, an unknown tag, or a clean one.
  //
  // Reads the two heaviest materials per limb, which is what the ledger keeps.
  // A tag carried only by a body's THIRD substance reads 0 — correct enough
  // for "is this hand bloody", and the alternative is a per-limb map.
  float CoatTagFraction(uint64_t mobId, const char* tag,
                        const char* limbTag) const;
  // Mob::DepositCoat by id, so a caller holding only the system can track a
  // coat onto the ground (the footfall wiring is P2's).
  bool DepositCoatOn(uint64_t mobId, uint32_t mat, IVec3 groundCell,
                     uint32_t tick);
  // Mob::ShedCoat by id — the footfall path the gait drivers take, reachable
  // from a gate that wants to plant one foot without walking a creature.
  uint32_t ShedCoatOn(uint64_t mobId, int footLimb, Vec3 footPosVox,
                      uint32_t tick, World& world);
  // Put `amount` of `mat` on every occupied voxel of one limb and recount. The
  // direct door onto the coat that the world's own paths -- contact, splatter,
  // the cut soak -- all reach the long way round: a caller (a gate, an
  // authoring tool) that wants a bloody FOOT and no other consequence has no
  // other way to ask for one. Returns the voxels marked.
  uint32_t SoakLimb(uint64_t mobId, int limb, uint32_t mat, uint32_t amount,
                    uint32_t tick);
  // SOMETHING POURED ON A LIMB BY HAND (game/container.h: the health panel's
  // "apply to this part"). SoakLimb's coat, then the material's authored
  // `coat.effects`, run ONCE, on that limb -- the first reader that list has
  // had. The vocabulary is what a remedy can do to state a limb already
  // carries, and nothing else:
  //   "stanch"    -- the wound stops owing blood (bleedBudget, the open
  //                  stump, a gout in progress): the cauterise rule's three
  //                  fields, reached by a salve instead of a burn;
  //   "disinfect" -- a bite's rot stops spreading (infectMat/infectStain).
  // An unknown tag does nothing, so content can name remedies ahead of the
  // code. Returns a bit per effect that CHANGED something (kRemedy*), so the
  // caller can say "the bleeding stops" only when it did.
  static constexpr uint32_t kRemedyStanch = 1u, kRemedyDisinfect = 2u;
  uint32_t DouseLimb(uint64_t mobId, int limb, uint32_t mat, uint32_t amount,
                     uint32_t tick, uint32_t* marked = nullptr);
  // Force the ledger's cadence (Mob::RecountCoat) for one creature, so a
  // caller that has just changed a coat can read the answer this instant
  // instead of waiting out tune.coat.recountTicks.
  void RecountCoatOn(uint64_t mobId, uint32_t tick);
  // Queue one tick of a gout / spray for every OTHER body to be splashed by
  // (see SplatterEvent). Bounded: past kSplatterMaxEvents the burst is not
  // remembered, which only loses cosmetics.
  void QueueSplatter(const SplatterEvent& e);
  size_t SplatterEventsQueued() const { return splatters_.size(); }
  // Apply every queued burst not yet applied to the avatar to `avatar`, and
  // mark them so. Called by PlayerAvatar::PreTick.
  void SplatterOnto(Mob& avatar);
  // The NPC driver for StainTick + splatter replay, under the shared budget.
  void StainLimbs(uint32_t tick, World& world);
  bool StainOneLimb(BurnLimbView& v, uint32_t tick, uint32_t rngKey,
                    World& world, uint32_t& budget);
  // The drying half of Mob::StainTick over a view: every substance in `led`
  // with an authored coat.decay loses a level on half its voxels once per
  // period. Shared by the living and the dead (StainCorpses).
  bool DryOneLimb(BurnLimbView& v, const LimbCoat& led, uint32_t tick,
                  uint32_t key, uint32_t& budget);
  // One lattice's coat ledger (Mob::RecountCoat's per-limb count).
  static void TallyCoat(const BurnLimbView& v, LimbCoat& out);
  // ---- THE DEAD TAKE A COAT TOO (2026-09-22) --------------------------------
  // Contact (blood stains, water rinses) and drying over every dead-flesh
  // debris body, through StainOneLimb / DryOneLimb — the passes the living
  // use, so a corpse in a river is washed at the rate a man in it is. Owner
  // report: "water doesn't clean the stains off corpses". Splatter onto the
  // dead is NOT here (Mob::ApplySplatter walks limbs, not bodies).
  void StainCorpses(uint32_t tick, World& world, uint32_t& budget);
  // One burst replayed against one lattice (Mob::ApplySplatter's per-limb
  // body): true when a voxel's coat changed. `salt` keys the draws — the limb
  // index on the living, a hash of the body id on the dead.
  bool SplatterView(const SplatterEvent& e, BurnLimbView& v, uint32_t salt);
  // ...and that replay over every dead-flesh body (StainLimbs' splatter loop).
  void SplatterCorpses(const SplatterEvent& e);
  // ---- THE DEAD BURN AS THE LIVING DO (2026-09-22) --------------------------
  // Every dead-flesh debris body through BurnOneLimb, the living limb pass,
  // with the two things only the living had until now: heat across the
  // corpse's joints (BuildCrossHeat, grouped by the creature the pieces came
  // off) and its armour (the occlusion hook, marching the bodies strapped to
  // each piece). DebrisSystem keeps only the body tail (BurnFleshBodies).
  void BurnCorpses(uint32_t tick, World& world, std::vector<CellOp>& cellOps,
                   std::vector<ParticleSpawn>& spawns);
  // The cross-joint heat snapshot over any set of lattices: `parts[i]` is
  // part i's view (null = no body), and a cell's limb bit is its index here.
  // Mob::BuildCrossLimbHeat passes a creature's limbs; BurnCorpses passes one
  // corpse's pieces.
  void BuildCrossHeat(const std::vector<BurnLimbView*>& parts, uint32_t tick,
                      std::vector<CrossHeatCell>& out);
  // The corpse twin of Mob::WornAlong: the first shell met marching from
  // `from` along `dir`, over the bodies strapped to one piece.
  uint32_t CorpseWornAlong(const std::vector<DebrisSystem::FleshShell>& shells,
                           const Vec3& from, const Vec3& dir, float dist,
                           uint32_t tick);
  struct CorpseWornProbe {
    MobSystem* sys;
    const std::vector<DebrisSystem::FleshShell>* shells;
    uint32_t tick;
    static uint32_t Call(void* ctx, const Vec3& from, const Vec3& dir,
                         float dist) {
      CorpseWornProbe* p = static_cast<CorpseWornProbe*>(ctx);
      return p->sys->CorpseWornAlong(*p->shells, from, dir, dist, p->tick);
    }
  };
  // The limb's AUTHORITATIVE lattice (the skin when it is finer, else the
  // collider re-expressed as PrefabVoxels), copied out for a gate that has
  // to ask WHICH voxels changed rather than how many (corpse-bleed asks where
  // the blood soak landed). Empty when there is no such limb.
  std::vector<PrefabVoxel> LimbLattice(uint64_t mobId, int limbIndex) const;
  // ---- blood is health; burns cap it (Mob::TotalHp and friends, by id) ----
  // -1 for an unknown id, so a gate cannot mistake "no such creature" for
  // "dead", which is the one confusion these readouts exist to prevent.
  float TotalHp(uint64_t mobId) const;
  float LimbHp(uint64_t mobId, int limbIndex) const;
  // ---- HOW MUCH BLOOD THIS WOUND STILL OWES (MobLimb::bleedBudget) --------
  //
  // Whole voxels the wound will yet drip, which is a DIFFERENT measurement
  // from how much it has already dripped (BloodLost) and from how loud it is
  // right now (BleedSources). The claim it exists for is a comparison: "a mace
  // charges a fraction of the blood a sword does for the same hp"
  // (gore.bluntBleedScale), and neither of the other two can state that
  // without letting a tick run and turning a one-line assertion into a timing
  // question. -1 for an unknown id or limb, so a gate cannot read "no such
  // creature" as "dry".
  float LimbBleedBudget(uint64_t mobId, int limbIndex) const;
  float BloodLost(uint64_t mobId) const;
  // MobLimb::surfaceAtSpawn (0 until the first burn recount takes it).
  uint32_t LimbSurfaceAtSpawn(uint64_t mobId, int limbIndex) const;
  // Mob::DeathCause by id; "" for a live or unknown creature.
  const char* DeathCause(uint64_t mobId) const;
  float BurnFraction(uint64_t mobId) const;
  float BurnHealthCap(uint64_t mobId) const;
  // How burnt a material reads (0 intact / 1 half / 2 whole), by id, from the
  // table OnMaterialsReloaded builds off Mob::BurnStageOfMaterialName.
  uint8_t BurnStageOf(uint32_t mat) const {
    return mat < burnStage_.size() ? burnStage_[mat] : 0u;
  }
  // Can this material burn at all — tag:flammable, or already a burn stage.
  // The DENOMINATOR of the burnt fraction: bone carries no `flammable` and no
  // fire will ever move it, so a body counted over every voxel could never
  // reach the death knot however black it was. Read off the tags, so a new
  // material joins the count by being authored flammable, not by being named.
  bool BurnableOf(uint32_t mat) const {
    return mat < burnable_.size() && burnable_[mat] != 0;
  }
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
    // Heat across a joint (CrossHeatCell). `crossCells` is how many world
    // cells the creature's other limbs offered, `crossFaces` how many faces
    // took one, `crossOnly` the rules a cross face was the ONLY thing arming
    // (i.e. the ones combustion.crossLimbPct scaled). A bare "the legs still
    // did not catch" buys one hypothesis per run; these say which of "no cells
    // were built", "no face found one" and "the roll refused" it was.
    uint32_t crossCells = 0, crossFaces = 0, crossOnly = 0;
  };
  const BurnStats& Burn() const { return burnStats_; }
  void ResetBurnStats() { burnStats_ = BurnStats{}; }

  bool BurnOneLimb(BurnLimbView& v, uint32_t tick, uint32_t rngKey, World& world,
                   std::vector<CellOp>& cellOps, uint32_t& frontBudget,
                   uint32_t& opsBudget);
  uint32_t IgniteOneLimb(BurnLimbView& v, uint32_t count, uint32_t onlyMat);
  // The dense lattice index, on demand. The infection pass (Mob::InfectBurst)
  // needs the same position -> voxel map the burn does, and for the same two
  // jobs: asking what a voxel's six neighbours are, and handing FlushBurn the
  // set of cells a removal cleared. Building it is MobSystem's, exactly as it
  // is for the burn — there is one index and one builder, not two that agree
  // until the lattice layout changes.
  //
  // NOT held open. The caller builds it inside a burst and lets the burn pass's
  // ordinary grace period drop it again; pinning it for every infected limb
  // would be the permanent per-limb allocation rule 2 forbids.
  void EnsureBurnIndex(BurnLimbView& v) {
    if (v.burn && v.burn->idx.empty()) BuildBurnIndex(v);
  }
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
    // No ground because the column is NOT KNOWN (outside the window or not
    // cached yet), as opposed to no ground within the scan. Gravity waits on
    // the first and acts on the second.
    bool groundUnknown = false;
    int groundY = 0;           // ground under the mob's centre column
    // Probes fanned around the mob at kProbeCount evenly spaced yaws, each at
    // the mob's own step-out radius. `clear[i]` is whether a body could walk
    // that way: known footing, and no step up taller than it can climb.
    static constexpr int kProbeCount = 8;
    bool clear[kProbeCount] = {};
    int stepUp[kProbeCount] = {};   // rise at that probe, voxels (INT_MAX = unknown)
    // The budget `clear[]` was decided against, copied out of the creature's
    // own rig (Mob::StepUpCells). Carried on the sense rather than re-read by
    // each consumer so the drive, the intent layer and the debug readout can
    // never be quoting a different number from the one the fan used.
    int stepUpCells = 2;
  };
  GroundSense SenseGround(const Mob& mob, const MobDef& def, World& world) const;

  // ---- executing an authored attack (game/strokes.h) ----------------------
  // Called from PreTick's per-mob loop, between the steering stages and the
  // animation. The long note at their definitions says why the order within a
  // tick is what it is.
  void BeginStroke(Mob& mob, const ai::AttackRequest& req, uint32_t tick);
  // THE ONE DOOR INTO A SWING: everything a stroke IS, with the decisions
  // about WHETHER left to its two callers (the AI's BeginStroke and the
  // scripted ForceAttack). See the long note at the definition for what
  // drifted apart before it existed.
  void StartStroke(Mob& mob, int styleIndex, uint64_t targetId,
                   Vec3 targetPoint, uint32_t tick, uint32_t seed);
  void StepStroke(Mob& mob, uint32_t tick, World& world,
                  std::vector<ParticleSpawn>& spawns);
  // Why an attack request was dropped, ONCE per (mob, reason) — see the long
  // note at the definition for why this is a ledger rather than a printf.
  void ReportNoStroke(const Mob& mob, const ai::Profile* pr, int reason,
                      const char* detail);
  std::map<uint64_t, uint8_t> strokeGripe_;

  // Pick this tick's desired heading and drive scale. This is THE AI seam: the
  // only stage that gets to have an opinion, and it may write nothing except
  // mob.desiredHeading / mob.driveScale.
  void DecideIntent(Mob& mob, const MobDef& def, const GroundSense& sense,
                    uint32_t tick, float dt);

  // Close the gap between heading and desiredHeading at a bounded rate, and
  // report how well aligned the body now is (1 = facing the target, 0 = past
  // driveAlignZero) so the drive can scale forward speed by it.
  float Steer(Mob& mob, const MobDef& def, float dt);

  // ---- CREATURES DO NOT STAND INSIDE EACH OTHER ----------------------------
  //
  // A body's own footprint radius in WORLD voxels: the mean half-extent of its
  // prefab box in x and z. Mean rather than max on purpose — `worldSize` is the
  // ART's bounding box, so a rig with its arms out would otherwise claim a
  // personal space the size of its wingspan.
  static float BodyRadius(const MobDef& def);
  // Centre of that footprint in world voxels (origin_ is the MIN CORNER).
  static Vec3 BodyCentre(const Mob& mob, const MobDef& def);
  // Sum of the crowding pushes from every other live mob inside the spacing
  // radius, as a WORLD-space xz vector of magnitude 0..1 pointing away from the
  // crowd. Zero when nothing is close, which is what keeps a lone creature —
  // and every existing single-mob fixture — bit-for-bit as it was.
  Vec3 CrowdPush(const Mob& self, const MobDef& selfDef) const;
  // Fold that push into the mob's DRIVE (never its heading — anim.h
  // LocomotionDef::spacingMul says why at length). Runs between DecideIntent
  // and Steer: the AI has had its say about where to go, and this is the body
  // declining to walk through another body on the way.
  void ApplyCrowdSpacing(Mob& mob, const MobDef& def);
  // Would a body of `def` centred at (cx, cz) be inside another mob? Used by
  // the drive's move resolve as a hard floor under the soft push above.
  // `allowIfFurther` is the escape hatch that makes this safe: a move which
  // INCREASES separation is always legal, so two bodies that somehow start
  // overlapped (a spawn on the same column, a teleport) push apart instead of
  // welding together, and nothing can ever be permanently stuck.
  bool BlockedByMob(const Mob& self, const MobDef& def, float cx,
                    float cz) const;

  // Apply the resulting motion: settle onto the ground and translate along the
  // ACTUAL facing (never the desired one — that is what makes a turn arc).
  void DriveLocomotion(Mob& mob, const MobDef& def, const GroundSense& sense,
                       float align, float dt);
  // NPC gravity, BEFORE the drive: true while the creature is in freefall
  // (the drive is skipped that tick). Flips it into a ragdoll once the fall
  // has lasted ragdoll.fallSeconds.
  bool UpdateFall(Mob& mob, const MobDef& def, const GroundSense& sense,
                  float dt);

  // Animation pipeline stages 1-5 plus the procedural gait layer; leaves the
  // model-space pose in mob.anim.model. Pure float, no grid contact.
  // `tick` reaches these only so the gait's PLANT can run Mob::ShedCoat, which
  // is tick-keyed like every other RNG in this engine. The pose itself is
  // still a pure function of dt.
  void UpdateAnimation(Mob& mob, const MobDef& def, World& world, float dt,
                       uint32_t tick);
  void UpdateGait(Mob& mob, const MobDef& def, World& world, float dt,
                  uint32_t tick);
  // Ease the DRAWN body height (Mob::bodyY_) toward `targetY`: a rate in
  // metres per second AND a hard bound on the lag. See the long note at the
  // definition for why the bound is the load-bearing half.
  static void EaseBodyY(Mob& mob, float targetY, float dt);

  // A creature by id, THE AVATAR INCLUDED. The player's body is a Mob that
  // does not live in `mobs_` (see SetAvatar), so every by-id query has to try
  // both lists; this is that lookup, factored out of the ones that grew it.
  const Mob* FindMob(uint64_t mobId) const;

  Physics* phys_ = nullptr;
  World* world_ = nullptr;
  DebrisSystem* debris_ = nullptr;
  MicroBodySet* microSet_ = nullptr;  // shared brick pool; see SetMicroSet
  // True only inside LoadState. Spawn() honours MobRotDef unless this is set:
  // a saved creature already carries the holes it was born with, in its saved
  // lattice, and biting it again on every load would eat it.
  bool loading_ = false;
  std::vector<float> densityOf_;
  std::vector<uint32_t> classOf_;
  // ---- burn tables, rebuilt on materials hot-reload --------------------------
  // Data-driven, exactly as DebrisSystem's are: no material id is hardcoded
  // anywhere in the burn path, so "flesh chars" and "cloth catches easily" stay
  // facts about assets/materials/*.json and not about this file.
  std::vector<MaterialGpu> matGpu_;
  // Material NAMES, in id order, mirrored beside matGpu_ on every reload. The
  // only consumer is MaterialIdNamed (a name-typed tuning row); kept as its own
  // vector rather than holding the MaterialDef list because a MaterialDef
  // carries voxel art, tints and a reaction chain this system has no use for.
  std::vector<std::string> matNames_;
  // The one-entry memo MaterialIdNamed keeps; see the note at its definition
  // for why one entry is enough and why the MISS is cached as hard as the hit.
  // Cleared with the tables on a materials reload, because an id is only valid
  // against the table it was resolved from.
  mutable std::string matNameLast_;
  mutable uint32_t matNameLastId_ = 0;
  // Per-material burn stage (BurnStageOf) and burnability (BurnableOf),
  // rebuilt with the rest on reload.
  std::vector<uint8_t> burnStage_;
  std::vector<uint8_t> burnable_;
  // The most recent death's id and cause, so DeathCause(id) can answer for a
  // husk that PreTick has already swept out of mobs_ (Mob::Die writes it).
  uint64_t lastDeathId_ = 0;
  const char* lastDeathCause_ = "";
  std::vector<ReactionGpu> reactions_;
  std::vector<uint8_t> matSelfActive_;  // has decay/emit rules — i.e. is ALIGHT
  std::vector<uint8_t> matHasPair_;     // has pair rules — i.e. is ignitable
  WornStats wornStats_{};
  BurnStats burnStats_{};
  std::vector<uint8_t> matHot_;         // carries tag:hot
  std::vector<uint8_t> matInfectious_; // carries tag:infectious
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
  // ---- the stain palette, both ways round ----------------------------------
  // mat -> its stain palette slot (1..7, 0 = does not stain), and slot -> the
  // FIRST material registered against it. The second exists because the voxel
  // word can only afford the slot: a dry stain on the floor says "blood" and
  // the body that rubs it off has to write a coat, which names a material.
  std::vector<uint8_t> stainSlotOfMat_;
  uint32_t matOfStainType_[8]{};
  // ---- what a substance does while it is on a body (materials.json "coat") -
  // Mirrors of MaterialDef::coat*, indexed by material id, rebuilt with the
  // rest of the tables. `coatEffects_` is the raw tag lists; nothing here
  // interprets them (rule 4).
  std::vector<float> coatDecay_;
  std::vector<uint32_t> coatDecayFloor_;
  std::vector<uint32_t> coatShed_;
  std::vector<std::vector<std::string>> coatEffects_;
  // Deposits every creature together may track onto the ground this tick
  // (tune.coat.shedPerTick), charged BEFORE the droplet is queued and reset at
  // the top of PreTick. A budget and not a rate, for the reason every other
  // gore budget is one: the number of feet in a crowd is not bounded by
  // anything this layer controls.
  uint32_t coatShedSpent_ = 0;
  // mat -> what it becomes when it catches (see IgnitedForm).
  std::vector<uint32_t> ignitedForm_;
  uint32_t dayPhase_ = 0;
  // EVERY LIVE BODY POINTS INTO THIS VECTOR (Mob::def_, and the avatar's too,
  // which this system cannot reach). So it is never allowed to reallocate
  // after a load: SetDefs reserves room for kDerivedDefs compositions up
  // front, and DefWithEffects refuses once that room is gone. A composed def
  // is therefore an append that moves nothing — which is also what lets one
  // turn mid-tick while the creature turning is standing in the middle of it.
  static constexpr size_t kDerivedDefs = 64;
  // One booked rising. Everything a body has to say on its way out for the
  // creature that gets up to be recognisably the same one, and nothing that
  // needs the rig to still exist.
  struct PendingRise {
    std::string def;                   // whose body it was
    std::vector<std::string> fx;       // what it rises as
    Vec3 at{};
    float heading = 0.0f, bodyY = 0.0f;
    std::vector<std::string> lost;     // limbs it had already lost
    std::vector<uint64_t> bodies;      // the remains, to take out of the world
    // Limb name → debris handle, so ServiceRisings can read the debris's
    // current transform right before destroying it and hand the zombie a
    // ragdoll pose to rise from.
    struct RiseBodyMap { std::string name; uint64_t body; };
    std::vector<RiseBodyMap> bodyMap;
    uint32_t atTick = 0;
    // WHOSE CORPSE IT WAS (M9.4-B). A rising SPAWNS a creature, which is an
    // authoring act: two machines servicing the same booking would stand two
    // zombies up in one grave. Captured at the booking rather than tested at
    // the service so that a body handed over between dying and rising rises
    // for the machine that owned it when it died — the one whose op stream
    // carried the wounds that killed it.
    uint32_t owner = 0;
    // ---- WHAT HAPPENED TO IT, AS GEOMETRY ---------------------------------
    //
    // One entry per limb whose lattice is no longer the def's — the arm a
    // sword opened, the shoulder a zombie tore at. Exactly the fields
    // MobSystem::SaveState writes per limb, for exactly the same reason: the
    // lattice is the truth and the collider, the brick and the rig offsets are
    // derived from it, so restoring it is the whole restore (LoadState's carve
    // pass, and ServiceRisings replays that pass verbatim).
    //
    // BY NAME, not by index, like `lost`: an effect may APPEND a limb even
    // though it may not rename or remove one (sidecar.h), so index parity is a
    // rule about this rig and not about every rig a later effect could
    // describe.
    struct RiseLimb {
      std::string name;
      float hp = 0;
      Vec3 restOffset{}, anchorRoot{}, anchorLimb{};
      IVec3 size{};
      std::vector<DebrisVoxel> voxels;
      std::vector<PrefabVoxel> skinVoxels;
    };
    std::vector<RiseLimb> limbs;
    // ---- AND WHAT IT WAS WEARING ------------------------------------------
    //
    // The remains are DESTROYED by the rising (a corpse that got up is not
    // still lying there), and its gear is part of the remains — so without
    // this, turning is a way to delete a suit of armour. Re-equipped through
    // the ordinary Mob::WearItem / Mob::EquipItem on the far side, by NAME
    // (item.h's index hazard) with the damage the piece had at death, so a
    // zombie rises in the same battered, dyed kit the villager fell in.
    struct RiseGear {
      std::string item;
      int equipSlot = -1;      // -1 for the held item
      bool held = false;
      uint32_t dye = 0;
      WornDamage damage;
    };
    std::vector<RiseGear> gear;
    // ---- AND WHAT IT HAD IN ITS PACK --------------------------------------
    //
    // The same argument as `gear`, one step simpler because there is no body
    // and no damage to capture: the remains are destroyed by the rising, so a
    // rising that dropped the pack would be a way to delete a purse by letting
    // its owner turn. Straight onto the creature that gets up, which is then
    // lootable for it in the ordinary way — a zombie carrying your dagger is a
    // zombie you can take your dagger back off.
    //
    // FOR THE AVATAR this is where the player's bag and hotbar arrive
    // (MobSystem::SetAvatarKitFn): Mob cannot reach PlayerSession, and the
    // kit's owner answers the question at the moment of death.
    std::vector<CarriedItem> carried;
  };
  // Bounded like every other emergent queue here (CLAUDE.md rule 2): a crowd
  // dying at once books a crowd of risings, and the cost of one is a spawn.
  static constexpr size_t kMaxRisings = 32;
  // ...and a rising is no longer a fixed-size booking now that it carries
  // lattices: a human's skin lattice is ~27k voxels of 8 bytes, per limb. Only
  // limbs that were actually damaged are captured (MobLimb::carved, the latch
  // that fires the first time a limb loses a voxel to a carve OR a burn), and
  // the whole capture is dropped past this ceiling — the body still gets up,
  // it simply gets up with the freshly-rotted lattice it used to get up with.
  // 256k voxels is ten times a whole human's skin, so a body reaches it only
  // by being cut to pieces, which is the case where the geometry has stopped
  // meaning anything anyway.
  static constexpr size_t kRiseVoxelBudget = 256 * 1024;
  void BookRising(PendingRise r);
  void ServiceRisings(uint32_t tick);
  std::vector<PendingRise> rises_;
  uint32_t tick_ = 0;                  // this tick, for booking a rising
  std::vector<MobDef> defs_;
  std::shared_ptr<MobDefFactory> defFactory_;
  std::vector<Mob> mobs_;
  // ---- behaviour layer ------------------------------------------------------
  ai::Library behaviors_;
  // Rebuilt at the top of every PreTick from the player actor plus every live
  // mob. Cheap (kMaxMobs + 1 entries) and it is what makes target selection a
  // scan over "things" rather than a special case for the player.
  std::vector<ai::Actor> actors_;
  // One entry per player pushed in by the frame layer (empty = no player on
  // the field, which is what the mob-vs-mob fixtures want).
  std::vector<ai::Actor> playerActors_;
  std::vector<ai::AttackRequest> attacks_;
  // ---- phase C: executing an attack ---------------------------------------
  // The authored style library, and the item library the swings resolve their
  // weapon against. Neither is owned by the creature: styles hot-reload with R
  // and item indices are file-order (item.h's index hazard), so a Mob stores
  // its weapon BY NAME and the swing looks it up.
  StyleLibrary styles_;
  const ItemLibrary* items_ = nullptr;
  std::function<void(uint64_t, const std::string&, uint32_t)> onItemShed_;
  std::function<void(const CorpseReport&)> onCorpse_;
  std::function<void(std::vector<CarriedItem>&)> avatarKitFn_;
  std::vector<BlockEvent> blocks_;
  // The players' bodies, registered by the frame layer so the handle-keyed
  // lookups can find them. NOT owned and NOT in `mobs_` — see SetAvatars.
  std::vector<Mob*> avatars_;
  uint64_t nextId_ = 1;
  // ---- ownership state (M9.4-B) -------------------------------------------
  // All three are PROCESS state, not world state: none is hashed, none is
  // saved, and with `ownershipFn_` null the first two are never read.
  // ---- one creature's record, parsed --------------------------------------
  // Defined in mob.cpp. The read and the overlay are separate because a save
  // load SPAWNS and then overlays, while a handoff of a creature this machine
  // already holds as a ghost overlays onto the rig that is already standing
  // there — rebuilding it would throw away its Jolt bodies and bricks and
  // flicker the body for nothing.
  struct MobRecord;
  static bool ReadMobRecord(ByteReader& r, MobRecord& out);
  // `placeLimbs` puts each limb back on the TRANSFORM the record carries
  // (and moves its Jolt body to match). The handoff wants it — a creature
  // that changed hands must not visibly snap to a rest pose for one tick —
  // and a save load must NOT have it: a loaded creature has always stood up
  // in its rest pose, and where its limbs are is where it bleeds from, which
  // is hashed state.
  void OverlayMobRecord(Mob& m, MobRecord& rec, bool placeLimbs = false);

  uint32_t localPlayerId_ = kLocalOwner;
  std::function<uint32_t(uint64_t, Vec3)> ownershipFn_;
  uint64_t aiSteps_ = 0;
  // Evaluated at the top of PreTick, before anything steps: one creature's
  // owner may not change halfway through its own tick.
  void RefreshOwnership();
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
  std::vector<SplatterEvent> splatters_;
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
  // ---- the stain pass (StainLimbs / StainOneLimb) ---------------------------
  // World cells one limb's walk may read per tick (its AABB, dilated by one)
  // before it decides nothing is against it.
  static constexpr uint32_t kStainScanCells = 2048;
  // Surface voxels one limb may sweep per tick, and all creatures together.
  // A human limb's surface at skinScale 8 is a few thousand, so a limb in a
  // pool is swept whole every tick or two.
  static constexpr uint32_t kStainLatticePerLimb = 6144;
  static constexpr uint32_t kStainLatticePerTick = 32768;
  // Per corpse body: the contact pass's index (derived, rebuilt when the
  // lattice's voxel count moves — every carve, burn flush and shatter does),
  // and the coat ledger drying reads, recounted only while something changed.
  // Keyed on the body's global id; an entry whose body is gone is dropped the
  // next tick the pass runs. Never saved, never hashed.
  struct CorpseCoat {
    BodyBurnState burn;
    size_t n = 0;
    LimbCoat led;
    bool dirty = true;
    uint32_t seen = 0;
  };
  std::map<uint64_t, CorpseCoat> corpseCoat_;
  // One march index per strapped shell on a corpse, keyed on the shell body's
  // global id; dropped when unasked-for for two seconds (the body left).
  struct CorpseShell {
    ShellMarchIndex ix;
    uint32_t seen = 0;
  };
  std::map<uint64_t, CorpseShell> corpseShellIdx_;
  BurnLimbView CorpseView(DebrisSystem::FleshLattice& f, CorpseCoat& cc,
                          int& model);
  static uint32_t CorpseKey(uint64_t id);
  static constexpr size_t kSplatterMaxEvents = 64;
  // Lattice steps one droplet's arc may take inside a limb's index box, and
  // the widest splat (lattice voxels) one landing may paint.
  static constexpr int kSplatterMarchSteps = 256;
  static constexpr int kSplatterSplatMaxL = 5;

  // Mob's shared mechanics reach this system's services (burn tables, micro
  // pool, event sinks, material tables) through this friendship — the same
  // seam the avatar used to reach BurnOneLimb through, made symmetrical.
  friend class Mob;
};
