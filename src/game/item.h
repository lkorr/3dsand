#pragma once
#include "sim/scale.h"  // MetresToCells
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "game/anim.h"
#include "game/impact.h"
#include "game/iteminstance.h"
#include "sim/microbody.h"
#include "sim/voxload.h"

// ITEMS AND THE HOTBAR: what the player is carrying and what is in their hand.
//
// Deliberately shaped like game/caster.h's GlyphInventory, and for the same
// reasons: a plain data structure with no engine coupling, owned by main.cpp,
// that neither Player nor PlayerAvatar has to know about. Player stays a clean
// movement controller; the avatar is told which prop to show and nothing more.
//
// WHAT AN ITEM IS. An item is a STANDALONE ASSET: its own .vox, its own
// origin, its own sidecar (assets/items/sword.{vox,json}). It is not a part of
// any creature.
//
// WHY IT CHANGED. The sword used to be a limb of the wearer's rig — a
// `"tag": "prop"` entry parented to hand.R — and that is exactly what broke.
// Prefab-local space is rebased on the BODY's min corner, and props are
// deliberately excluded from that measurement, because a creature's size must
// not change with what it happens to be carrying. So when the handedness fix
// moved the right hand to model -X, the blade followed it to engine x -30:
// thirty micro of geometry at NEGATIVE prefab-local coordinates, which that
// space cannot represent. The body drew offset from where its own arm solved.
//
// HOW IT IS HELD: A BORROWED SLOT. Attaching does not weld a foreign object to
// a bone — it fills a real rig Part with the item's geometry. While worn, a
// held item IS a rig part, which is what preserves, unchanged and with no new
// code:
//   * severing with the parent limb (DetachPart recurses into children),
//   * dropping to debris (AdoptBody with the part's own voxels + micro ref),
//   * per-voxel carving of the item itself (CarveLimbRadial),
//   * micro-voxel detail as debris,
//   * "the pose is the hitbox" — WeaponEdge reads the live part transform,
//   * the weapon-arm IK, which derives the arm from the part's parent.
// The ENTITY outlives the attachment, so a ground/thrown/mounted item is a
// natural extension rather than a rewrite. The one real cost is a single
// entity<->slot sync seam, kept in one function (avatar.cpp EquipItem).
//
// WHERE THE OFFSET LIVES: ON THE ITEM. Engine-dominant practice is
// skeleton-side (Unreal's USkeletalMeshSocket, Source's $attachment), but the
// reasons for that do not hold here — one rig, rigs generated from Python,
// items authored in-house — and all three data-driven voxel games (Veloren,
// Minecraft Java, Bedrock) put the offset on the item. The rig says only WHERE
// THE FIST CLOSES (MobSocketDef); the item says how it sits in that fist. The
// runtime composes socket x grip, forward — explicitly NOT the inverse form
// VR rigs use, which inverts because there the hand pose is the constraint
// being solved for, and which drags in a scale hazard besides.
//
// SCOPE. This is a hotbar, not an inventory screen: 10 slots, one selected,
// no stacking beyond a count, no crafting, no grid UI. The interesting
// decisions are meant to be about what you have in hand right now.

// What the engine does with an item when it is held. Adding a kind means
// adding a case where the tick loop dispatches on Held(), not a new system.
//
// THE WORN KINDS ARE A CONTIGUOUS RANGE, and `ItemKindIsWorn` is the only
// place that knows it. They are not one "Armor" kind with a slot field because
// the slot table (game/equipment.h) validates by KIND — one kind per slot is
// what makes "a helm does not go on your feet" authored data rather than a
// branch, and it is the same shape the sheath already uses.
enum class ItemKind : uint8_t {
  None = 0,
  Melee,   // swung: drives game/melee.h, cuts with its part's authored `edge`
  // ---- worn: fills its slot's shells over the wearer's own limbs ----
  ArmorHead,
  ArmorChest,
  ArmorLegs,
  ArmorBoots,
  ArmorShoulders,
  ArmorHands,
  ArmorBelt,
  Trinket,
  // ---- carried, and used from the hotbar with the hands up ----
  // A vessel: scoops loose matter out of the grid and pours it back out
  // (game/container.h). WHAT it holds is data (`container.holds`), so a flask
  // and a pouch are two rows of one kind, not two systems.
  Container,
};

// Is this kind WORN (a set of shells over the body) rather than merely carried?
// The one predicate that knows the enum's layout; everything else asks this.
inline bool ItemKindIsWorn(ItemKind k) {
  return k >= ItemKind::ArmorHead && k <= ItemKind::Trinket;
}

// Kind <-> name. Kinds cross items.json in both directions and a mis-parsed
// kind is a silently unequippable item, so the table lives once and both
// directions read it.
inline const char* ItemKindName(ItemKind k) {
  switch (k) {
    case ItemKind::Melee: return "melee";
    case ItemKind::ArmorHead: return "armor_head";
    case ItemKind::ArmorChest: return "armor_chest";
    case ItemKind::ArmorLegs: return "armor_legs";
    case ItemKind::ArmorBoots: return "armor_boots";
    case ItemKind::ArmorShoulders: return "armor_shoulders";
    case ItemKind::ArmorHands: return "armor_hands";
    case ItemKind::ArmorBelt: return "armor_belt";
    case ItemKind::Trinket: return "trinket";
    case ItemKind::Container: return "container";
    case ItemKind::None: break;
  }
  return "none";
}

inline ItemKind ItemKindFromName(const std::string& s) {
  for (int i = 1; i <= (int)ItemKind::Container; i++) {
    const ItemKind k = (ItemKind)i;
    if (s == ItemKindName(k)) return k;
  }
  return ItemKind::None;
}

// How an item sits in one particular attachment context. Two rules are stolen
// verbatim from Minecraft's display block, because both are the kind of thing
// you only learn by getting them wrong:
//
//   * TRANSLATION APPLIES BEFORE ROTATION.
//   * A CONTEXT THAT OMITS A SUB-KEY DOES NOT INHERIT IT from another context.
//     Every context is explicit. Inheritance here means an item that looks
//     right in the hand silently drifts when someone adds a ground pose.
struct ItemGrip {
  // Item-local, WORLD voxels at runtime; authored in the item's own MICRO
  // units and divided by `scale` at load, like every other length in the
  // sidecar. A residual nudge ON TOP of the hilt/socket alignment below, so
  // for a well-authored item it is zero.
  Vec3 translation{};
  Quat rotation{};
  float scale = 1.0f;
};

// WHERE THE FIST CLOSES ON THIS ITEM, in the item's own frame.
//
// The limb system is the model here, deliberately. A rig says what a hand IS
// by declaring a named box (MobLimbDef's model box) — not by measuring the
// Jolt collider, which is a greedy box merge inflated by a convex radius and
// therefore describes what is COLLIDED AGAINST rather than what was authored.
// An item says where its hilt is the same way: an authored box, in the same
// micro units as the art, emitted from the same constants that build the mesh.
//
// The runtime puts this box's CENTRE on the socket point. That is what makes
// the alignment self-correcting: re-author the hilt or resize the hand and the
// sword stays in the fist, because neither side is a hand-tuned constant. It
// also generalizes past one-end grips — a staff held mid-shaft or a shield
// gripped at its boss declares a hilt box in the middle of itself and needs no
// new code.
struct ItemHilt {
  bool has = false;
  Vec3 center{};           // item-local, WORLD voxels (converted at load)
  Vec3 halfExtents{};      // ditto; kept for the debug overlay and tests
};

// ONE SHELL OF A WORN PIECE — the covering over a single body part.
//
// A worn piece is not one mesh: a robe is a torso panel, two sleeves and a
// skirt, and each of those has to move with the limb it covers. So a piece is
// a LIST of shells, and wearing it appends one rig slot per entry — the same
// borrowed-slot trick a held weapon uses (see the note at the top of this
// file), applied N times instead of once. Everything that already works for a
// held item therefore works per shell with no new code: it burns, dissolves,
// carves, severs with the arm it is strapped to, drops as debris, and renders
// through the same micro path.
//
// BOUND BY LIMB NAME, never by index. Every humanoid rig in the repo names its
// parts the same way ("head", "armU.L", "hips"), so a helmet authored for the
// stock human finds the right part on any of them, and a wearer that simply
// has no such limb skips that shell — a sleeve on a one-armed mob attaches to
// the arm that exists. That is what makes "goblin helmets look right on
// anyone" CONTENT rather than code.
struct ItemCover {
  std::string part;        // body limb name this shell attaches to ("torso")
  std::string model;       // model name inside the item's own .vox

  // ---- authored placement, converted micro -> world voxels at load ---------
  // The shell's MIN CORNER measured from the covered limb's own min corner,
  // in the item's authored micro units (divided by `ItemDef::scale` at load,
  // like every other length in the sidecar).
  Vec3 offset{};
  // The covered limb's world-voxel box this shell was DRAWN AGAINST. The fit
  // resample (P6) divides the wearer's actual limb box by this to get the
  // per-axis ratio; a zero box means "no fit data, wear it as authored".
  Vec3 fitBox{};
  // Per-shell durability. A piece's toughness is the sum of its shells, and
  // losing one strap does not cost you the pauldron on the other shoulder.
  float hp = 10.0f;

  // ---- geometry, filled by LoadItemAsset from the named model -------------
  IVec3 size{};                    // model box, MICRO units
  IVec3 modelOffset{};             // model min corner within the .vox, MICRO
  int microModel = -1;             // index into the shared micro-body pool
  std::vector<PrefabVoxel> voxels;
  // Which of this panel's six boundary planes another panel of the SAME
  // garment is pressed against (sim/microbody.h MicroBodyCutFaces). Measured
  // once at load off the item's own prefab and kept here because the FIT
  // resample re-packs the brick on the wearer (mob.cpp AppendWornShell) and
  // must hand the same mask back: a resample moves cells, never the question
  // of which plane is a seam.
  uint32_t cutFaces = 0;
};

// PER-AXIS NEAREST-NEIGHBOUR RESAMPLE — how one authored helmet fits heads it
// was not drawn for.
//
// THE ONLY NON-UNIFORM SCALE IN THE ENGINE, and deliberately the smallest one
// that does the job. Everything else here scales by an integer lattice divisor,
// which cannot express "this goblin's head is wider than it is tall relative to
// the mannequin's". A shell lattice is a small integer box, so a per-axis NN
// resample is:
//
//   * INTEGER, and therefore exactly reproducible — `src = (i * srcDim) /
//     dstDim` and nothing else. Same inputs, same bytes, every time.
//   * HOLE-FREE BY CONSTRUCTION when growing. Every target cell maps to
//     exactly one source cell, so a solid source cannot produce a gap; several
//     target cells sharing a source is what "bigger" means.
//   * single-pass, and it matches the blocky aesthetic up to about 2x, which
//     is far past any humanoid-to-humanoid difference.
//
// Rejected: generating the shell procedurally from the wearer's own surface
// (dilate by one, which is exactly how the stock set is authored). Perfect fit,
// but it loses the authored silhouette — the hood's peak stops being a peak —
// and an armour system whose pieces cannot look like anything in particular is
// not worth having. Revisit for a generic "cloth drape".
//
// Voxels outside `srcDims` are ignored rather than clamped: a lattice that
// disagrees with its own declared box is a content bug, and folding it onto
// the edge would hide it as a smear.
inline std::vector<PrefabVoxel> ResampleLattice(
    const std::vector<PrefabVoxel>& src, IVec3 srcDims, IVec3 dstDims) {
  std::vector<PrefabVoxel> out;
  if (srcDims.x <= 0 || srcDims.y <= 0 || srcDims.z <= 0 || dstDims.x <= 0 ||
      dstDims.y <= 0 || dstDims.z <= 0)
    return out;
  if (srcDims.x == dstDims.x && srcDims.y == dstDims.y &&
      srcDims.z == dstDims.z)
    return src;

  // Dense source, because the sample is a random access and a sparse scan per
  // target cell would be O(n*m). One byte pair per source cell; a shell box is
  // a few tens of thousands of cells at most.
  const size_t n = (size_t)srcDims.x * srcDims.y * srcDims.z;
  std::vector<uint16_t> mat(n, 0);
  std::vector<uint8_t> col(n, 0);
  for (const PrefabVoxel& v : src) {
    if (v.x < 0 || v.y < 0 || v.z < 0 || v.x >= srcDims.x ||
        v.y >= srcDims.y || v.z >= srcDims.z)
      continue;
    const size_t i =
        ((size_t)v.z * srcDims.y + (size_t)v.y) * srcDims.x + (size_t)v.x;
    mat[i] = v.material;
    col[i] = v.color;
  }
  out.reserve(src.size());
  for (int z = 0; z < dstDims.z; z++) {
    const int sz = (int)(((int64_t)z * srcDims.z) / dstDims.z);
    for (int y = 0; y < dstDims.y; y++) {
      const int sy = (int)(((int64_t)y * srcDims.y) / dstDims.y);
      for (int x = 0; x < dstDims.x; x++) {
        const int sx = (int)(((int64_t)x * srcDims.x) / dstDims.x);
        const size_t i =
            ((size_t)sz * srcDims.y + (size_t)sy) * srcDims.x + (size_t)sx;
        if (!mat[i]) continue;
        out.push_back(PrefabVoxel{(int16_t)x, (int16_t)y, (int16_t)z, mat[i],
                                  col[i]});
      }
    }
  }
  return out;
}

struct ItemDef {
  std::string name;        // display name, and the id other data refers to
  ItemKind kind = ItemKind::None;

  // ---- the item's own art -------------------------------------------------
  // Loaded from assets/items/<name>.vox. Held geometry is per-ITEM, never a
  // part of the wearer's prefab — see the note at the top of this file.
  Prefab prefab;
  // Micro voxels per WORLD voxel. DERIVED at load from artVoxelsPerMetre and
  // kVoxelsPerMetre — the same change, and for the same reason, as
  // MobDef::skinScale. It used to be the authored `scale` field, which fixed a
  // physical size only because the world happened to be 10 cm, so a sword
  // halved in metres the moment kVoxelMeters did while its grip socket (rig
  // geometry, in the wearer's frame) did not: a blade that no longer reached
  // its own hilt.
  uint32_t scale = 1;
  // The art's authoring resolution, in art voxels per METRE. See
  // MobDef::artVoxelsPerMetre — items and mobs share the whole mechanism
  // because a worn robe is measured against a wearer's limb box and the two
  // must land in one frame.
  int artVoxelsPerMetre = 0;
  uint32_t artUpsample = 1;   // block replication applied when art < world
  // Authored art units -> world voxels, for every length the sidecar states in
  // the art lattice (hilt, edge, cover offset/fitBox).
  float ArtToWorld() const {
    return (float)artUpsample / (float)(scale ? scale : 1u);
  }
  IVec3 size{};            // model box, MICRO units
  IVec3 offset{};          // model min corner within the .vox, MICRO units
  int microModel = -1;     // index into the shared micro-body pool, -1 = none
  std::vector<PrefabVoxel> voxels;

  // ---- how it is held -----------------------------------------------------
  // Keyed by CONTEXT ("held_right", later "held_left"/"ground"/...). A map
  // rather than a single offset because one offset never covers held, ground,
  // GUI and head — Minecraft carries nine display slots for that reason — and
  // a map makes adding one DATA rather than a schema migration.
  //
  // A MISSING GRIP MUST FAIL LOUDLY. An item held through a context it does
  // not declare is a content bug that would otherwise park the blade at the
  // wearer's origin, sticking out of their navel, which reads as a physics
  // glitch rather than the missing JSON key it is.
  std::map<std::string, ItemGrip> grip;

  const ItemGrip* Grip(const char* context) const {
    auto it = grip.find(context);
    return it == grip.end() ? nullptr : &it->second;
  }

  // ---- how it is WORN -----------------------------------------------------
  // Empty for everything that is not an armour kind. Non-empty is what makes
  // Mob::WearItem have anything to do; a worn kind with no cover entries is a
  // content error the loader reports, since it would equip into the slot and
  // then be invisible.
  std::vector<ItemCover> cover;

  // The hilt box (see ItemHilt). One per item rather than one per context: a
  // sword is gripped by the same part of itself whichever hand holds it, and
  // the CONTEXT differences (which way it points, where it rides when stowed)
  // are already the grip rotation's job.
  ItemHilt hilt;

  // ---- CAN THIS PIECE TAKE A DYE? -----------------------------------------
  // Authored in the sidecar (`"dyeable": true`, emitted by
  // scripts/gen_peasant_clothes.py). True means THE ART IS A GREYSCALE WEAVE
  // whose cells are multipliers, so a colour word applied to it is the colour
  // you see (game/dye.h).
  //
  // A FACT ABOUT THE ART, not a permission. Nothing refuses to dye an
  // un-dyeable item — the shader would happily tint the wizard's robe — but
  // the result is meaningless, because the robe's art is already black and
  // multiplying black by red is black. So this is what the wardrobe UI offers a
  // colour for, and it is authored beside the art that makes it true rather
  // than being a list of item names anywhere.
  bool dyeable = false;

  // Rig durability while worn: the borrowed slot takes these, so an item is
  // as severable and as knock-loose-able as the limb it replaces.
  float hp = 30.0f;
  bool severable = true;
  float severImpactSpeed = 0.0f;
  bool hasSpring = false;
  SpringDef spring;

  // ---- cutting edge -------------------------------------------------------
  // The segment this item cuts along, in its OWN local frame, world voxels.
  // Authored in the sidecar from the same constants that build the mesh, so
  // the hitbox cannot drift from the art. FLOAT, not fixed point: melee is
  // presentation state (melee.h), and damage reaches the world only through
  // the ordinary MutationQueue paths.
  bool hasEdge = false;
  Vec3 edgeFrom{}, edgeTo{};
  float edgeHalfWidth = 0;
  // WHICH WAY THE FLAT FACES, in the same local frame: the normal of the
  // blade's cutting plane, so the EDGE is perpendicular to both this and the
  // edge segment. Unit, authored beside `axis` in the sidecar's `edge` block.
  //
  // Needed by two separate things and derivable by neither. The stroke driver
  // rolls the blade so the edge leads the travel (melee.h), and the damage
  // sweep scales a hit by how edge-on it was (MeleeEdgeAlign) — and "which of
  // the two axes perpendicular to the blade is the thin one" is a fact about
  // the ART that only the art's own generator knows. A blade with none
  // authored falls back to an arbitrary perpendicular, which costs nothing but
  // makes its roll meaningless: it will always read as half-aligned.
  bool hasEdgeFlat = false;
  Vec3 edgeFlat{0, 1, 0};

  // ---- melee: WHAT A HIT IS MADE OF (game/impact.h) -----------------------
  //
  // THREE NUMBERS, NOT ONE. Until 2026-09-15 an item carried a single
  // `damage` float and every weapon in the game therefore arrived as the same
  // thing — a kerf — which is precisely why there was no mace and no fist.
  // A profile says how much of a hit is a CUT, how much is BLUNT trauma, and
  // (for a set of jaws) how much is a TEAR, and one resolver in
  // MeleeSweepDamage does the right thing for all of them.
  //
  // `damage` IS `strike.cut` and the items.json KEY IS NOT RENAMED: four rows
  // and a gate already spell it that way, and "damage" is still the honest
  // name for the part of a blow that opens a wound. Everything at full swing
  // speed, scaled by how fast the edge is really travelling, exactly as
  // `damage` always was — melee.h note 2: SPEED IS THE DAMAGE.
  //
  // A WORN piece may carry one too (a gauntlet's `strike` block in its own
  // sidecar): that is the profile that REPLACES the fist's when the fist
  // under it swings, which is what makes "iron gauntlets cave a face in"
  // content rather than a weapon kind.
  StrikeProfile strike;
  // The one number the readers that only ever wanted "how hard does this cut"
  // still want (the hotbar tooltip, a gate's fabricated sweep). A method
  // rather than a mirrored field, because a mirror is a second source of
  // truth that goes stale the first time somebody sets one and not the other.
  float Damage() const { return strike.cut; }
  // Extra carve radius beyond the blade's own authored halfWidth, world
  // voxels. This is the difference between a cut and a cleave; keep it small,
  // since the blade geometry is supposed to be what decides the wound.
  float carveBonus = 0.0f;

  // Which player strike compass this weapon selects: "dagger" uses
  // playerDagger, anything else (or empty) uses player. Authored in
  // items.json; absent defaults to the armed compass.
  std::string weaponClass;

  // ---- HEFT: how much weapon is behind the edge ---------------------------
  //
  // DERIVED FROM THE ART, because the art is right there. `heftVolume` is the
  // item's own voxel count expressed in WORLD voxels — voxels / scale^3 — and
  // it is a fact about the .vox rather than a number in a sidecar that
  // somebody has to keep true when the blade changes. A greatsword is heavier
  // than a knife because it IS bigger, and re-authoring the mesh re-derives
  // the weight in the same edit.
  //
  // Filled at load (melee.cpp LoadItemAsset) for the HELD model only: a worn
  // piece has no single model and never swings, so its heft stays 0.
  //
  // The dimensionless factor the wound model consumes is this against
  // `gore.woundHeftRef` (sim/tuning.h §E2), so retuning the reference rescales
  // every weapon at once with no rebuild. `heft` is the AUTHORED OVERRIDE for
  // the case the geometry cannot state — a hollow ceremonial blade, a
  // lead-cored club — and 0 means "derive it".
  float heftVolume = 0.0f;
  float heft = 0.0f;
  // `refVolume` is gore.woundHeftRef; `maxFactor` is gore.woundHeftMax. Kept
  // as arguments rather than reaching for CurrentTuning() so item.h stays free
  // of the tuning header, which the loader and the tests both depend on.
  float HeftFactor(float refVolume, float maxFactor) const {
    if (heft > 0.0f) return heft < maxFactor ? heft : maxFactor;
    if (heftVolume <= 0.0f || refVolume <= 0.0f) return 1.0f;
    const float f = heftVolume / refVolume;
    // Floored well below 1 rather than at it: a dagger SHOULD cut shallower
    // than an arming sword, and that is most of what makes "a knife needs
    // sustained work" true without a second rule.
    return f < 0.2f ? 0.2f : (f > maxFactor ? maxFactor : f);
  }
  // Reach in world voxels from the shoulder, used to size the swing arc.
  // METRES-derived: 0.9 m from the shoulder. A bare 9 cells halved the
  // player's effective reach the moment kVoxelMeters did.
  float reach = MetresToCells(0.9f);

  // ---- ItemKind::Container only (game/container.h) -------------------------
  // items.json's `container` block. Amounts are in EIGHTHS OF A CELL, the
  // grid's own liquid unit (the state nibble's fullness), so scooping a
  // half-drained puddle cell takes exactly what was there. A powder cell is
  // always a whole one: 8.
  struct ContainerSpec {
    // Bit per material CLASS (1u << CLASS_LIQUID, 1u << CLASS_POWDER): what
    // the vessel can take up. Authored by name ("liquid", "powder").
    uint32_t holds = 0;
    int capacity = 0;      // eighths; 128 cells authored = 1024
    int scoopPerTick = 4;  // cells lifted per tick while the button is held
    int pourPerTick = 2;   // cells thrown per tick while the button is held
    float pourRange = MetresToCells(2.0f);  // farthest AIMED target, voxels
    float pourSpeed = MetresToCells(1.5f);  // launch speed, world voxels/s
    float aimDist = MetresToCells(1.0f);    // the pour point, along the look
                                            // (ContainerPourPoint)
    int applyCells = 4;    // cells/s of the portrait pour brush at its default
                           // size; scales with area (PourBrushCellsPerSec)
    // THROWING (game/container.h ContainerThrowSpeed). Q held charges, release
    // throws: the launch speed ramps from throwMinSpeed to throwSpeed over
    // throwChargeSec. throwSpeed 0 = this vessel is not thrown.
    float throwSpeed = 0.0f;     // world voxels/s at full charge
    float throwMinSpeed = 0.0f;  // world voxels/s on a tap
    float throwChargeSec = 1.0f;
    // BREAKING (ContainerShouldBreak): a body of this vessel that meets
    // anything at this closing speed, or whose velocity jumps by it in one
    // tick, shatters and spills everything it held. 0 = never breaks.
    float breakSpeed = 0.0f;     // world voxels/s
  } container;
  bool IsContainer() const {
    return kind == ItemKind::Container && container.capacity > 0;
  }
};

// Cells <-> the eighths a ContainerSpec counts in.
constexpr int kContainerUnitsPerCell = 8;

// The item library. Loaded once. A slot stores an ItemInstance BY NAME
// (game/iteminstance.h), and `Of` is how a slot asks for its def: indices are
// file order and change on every R hot-reload, so nothing keeps one.
struct ItemLibrary {
  std::vector<ItemDef> items;

  int Find(const std::string& name) const {
    for (size_t i = 0; i < items.size(); i++)
      if (items[i].name == name) return (int)i;
    return -1;
  }
  const ItemDef* At(int i) const {
    return (i >= 0 && i < (int)items.size()) ? &items[i] : nullptr;
  }
  const ItemDef* Named(const std::string& name) const {
    return name.empty() ? nullptr : At(Find(name));
  }
  // The def an instance names, or null for an empty slot or a name this
  // library no longer has.
  const ItemDef* Of(const ItemInstance& it) const {
    return it.Empty() ? nullptr : Named(it.name);
  }
};

// Which slots exist. 10, on the number row, matching kGlyphSlots — the two
// hotbars are the same shape on purpose, since magic mode and item mode are
// the same hand reaching for the same keys.
constexpr int kItemSlots = 10;

// ONE SLOT'S CONTENTS IS AN ITEM INSTANCE (W2-M). It was a library index plus
// the fields that happened to have been needed so far (count, dye, fill), and
// every record the item crossed into (a pack, the ground, the wire, a save)
// had its own subset; now the slot and every one of those records carry the
// same struct, and a crossing copies it whole. See game/iteminstance.h for
// the fields and the stacking rule (ItemInstance::StacksWith).
using ItemStack = ItemInstance;

// An instance of library entry `def`, for the callers that found the item by
// index (a spawn menu, the starting kit, a test). Empty for a bad index.
inline ItemStack StackOf(const ItemLibrary& lib, int def, int count = 1,
                         uint32_t dye = 0) {
  const ItemDef* d = lib.At(def);
  if (!d || count <= 0) return ItemStack{};
  ItemStack s;
  s.name = d->name;
  s.count = count;
  s.dye = dye;
  return s;
}

// Put `s` into the first stack it merges with (ItemInstance::StacksWith), else
// the first empty slot. Returns the slot, or -1 when there is no room — the
// caller decides what that means (a pickup refused, an unequip that stays
// equipped); nothing is ever dropped behind its back. Shared by the hotbar
// and the bag so the merge rule is spelled once.
inline int AddToSlots(ItemStack* slots, int n, const ItemStack& s) {
  if (s.Empty()) return -1;
  for (int i = 0; i < n; i++)
    if (!slots[i].Empty() && slots[i].StacksWith(s)) {
      slots[i].count += s.count;
      return i;
    }
  for (int i = 0; i < n; i++)
    if (slots[i].Empty()) {
      slots[i] = s;
      return i;
    }
  return -1;
}

struct Inventory {
  ItemStack slots[kItemSlots];
  int selected = 0;

  const ItemStack& Selected() const { return slots[Clamp(selected)]; }

  void Select(int slot) { selected = Clamp(slot); }
  // Scroll wheel: wraps, because a hotbar that stops at the ends makes the
  // player look at it.
  void Scroll(int delta) {
    if (delta == 0) return;
    int n = kItemSlots;
    selected = ((selected + delta) % n + n) % n;
  }

  // Merges into a stack of the same plain item and dye, else the first empty
  // slot; -1 when the hotbar is full (AddToSlots).
  int Add(const ItemStack& s) { return AddToSlots(slots, kItemSlots, s); }

  // Removes one from a slot, emptying it at zero. Returns the one removed
  // (empty when the slot was).
  ItemStack TakeOne(int slot) {
    int s = Clamp(slot);
    if (slots[s].Empty()) return ItemStack{};
    ItemStack one = slots[s].One();
    if (--slots[s].count <= 0) slots[s] = ItemStack{};
    return one;
  }

 private:
  static int Clamp(int s) { return s < 0 ? 0 : (s >= kItemSlots ? kItemSlots - 1 : s); }
};
// Loads assets/items/items.json and, for each item, its own
// assets/items/<id>.{vox,json}. Errors are appended to `errors` and reported
// the way materials and glyphs are: a malformed item is skipped loudly, a
// missing file is a failure. Hot-reloadable (R) alongside them — nothing
// caches an index into it (every slot holds a NAME), so a reload only has to
// drop the slots whose item is gone (Kit::DropUnknown).
//
// `micro` receives each item's packed brick, exactly as mob defs pack theirs:
// a held item is drawn by the same micro-body path as the limb whose slot it
// borrows, so it must live in the same pool. `materialCount` gates the
// palette-index warnings the .vox loader emits.
bool LoadItems(const std::string& dir, size_t materialCount,
               MicroBodySet& micro, ItemLibrary& out, std::string& errors);
