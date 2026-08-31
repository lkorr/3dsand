#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "game/item.h"
#include "game/kitref.h"

// WHAT THE CHARACTER IS WEARING AND CARRYING — the half of the player's kit
// that is NOT the hotbar.
//
// Deliberately shaped like game/item.h's Inventory and game/caster.h's
// GlyphInventory, and for the same reasons: plain structs with no engine
// coupling, owned by main.cpp, that neither Player nor PlayerAvatar has to
// know about. Nothing here reaches into the rig, physics or the grid — an
// equipped item becomes visible on the body only through the SAME
// Mob::EquipItem borrowed-slot path a hotbar weapon already uses.
//
// WHY A SLOT TABLE RATHER THAN A BAG WITH TAGS. The interesting decision an
// equipment system makes is "what may go here", and that decision is DATA:
// `EquipSlotDef::accepts` is a list of ItemKinds, and when ItemKind::ArmorHead
// arrived the change WAS one row in kEquipSlots and no new branch anywhere —
// the claim this file made while the armour rows were still empty, now spent.
// A slot that refuses still says why out loud (MoveResult::WrongKind carries
// the reason string the UI shows) rather than declining silently.
//
// WHAT IS NOT HERE, ON PURPOSE:
//   * No stack limits. ItemStack::count is already a count and nothing in the
//     game produces more than one of anything yet; a cap invented before the
//     first stackable item exists would be a guess.
//   * No visual sheathing. Sheath holds a melee item as DATA; the draw key
//     takes it OUT of the sheath and into the hand through the ordinary
//     Mob::EquipItem path. Drawing it on the BACK while stowed needs a
//     `sheath_back` socket in the rig plus a matching grip context on the item
//     (game/item.h's ItemGrip map already anticipates exactly this), which is
//     content, not code.
//   * No weight, no armour class, no set bonuses. Protection here is geometric
//     occlusion plus material identity — a steel plate stops acid because
//     `steel` carries no `tag:dissolvable`, not because it has a resist field.
//     Durability IS real, but it is the shell's own hp and its own voxels
//     being carved away, not a number counting down.
//
// INDEX HAZARD (the one real trap). ItemStack::def is an index into
// ItemLibrary::items, which is FILE-ORDER dependent and invalidated by every
// R hot-reload. Anything that persists or must survive a reload stores the
// item's NAME and re-resolves through ItemLibrary::Find — see
// EquipmentSaveNames / EquipmentResolveNames below and the 'PLYR' section in
// game/persist.cpp.

// Which equipment slots exist. The ORDER is the save format's order and the
// panel's draw order; append new slots at the end, before Count.
enum class EquipSlotId : uint8_t {
  Head = 0,
  Chest,
  Legs,
  Boots,
  Shoulders,
  Hands,
  Belt,
  Trinket,
  // The sheath is where a melee weapon rides when it is not in hand. Data-only
  // for now (see the note above): equipping here does not put the blade on the
  // avatar's back, it only says the character owns it in a place that is not
  // the hotbar.
  Sheath,
  // Quick slots: four "on my person, one gesture away" slots. They exist as
  // the seam a potion/tool belt lands in, and they accept the same kinds the
  // hotbar does so the pipe is provable today with the one item that exists.
  Quick0,
  Quick1,
  Quick2,
  Quick3,
  Count,
};

constexpr int kEquipSlotCount = (int)EquipSlotId::Count;

// Why a move was refused. The UI shows the reason rather than the slot merely
// refusing to light up — a silent refusal is the failure mode that makes an
// inventory feel broken, and this enum is also the seam future armour
// validation extends (level requirements, cursed items, two-handed conflicts).
enum class MoveResult : uint8_t {
  Ok = 0,
  Empty,        // nothing in the source slot
  WrongKind,    // the destination refuses this ItemKind
  SameSlot,     // dragged onto itself: a no-op, not an error
  BadSlot,      // out-of-range index (a UI bug, reported rather than clamped)
};

// One equipment slot's authored rules. `accepts` empty means REFUSE
// EVERYTHING, which is the honest state of every armour slot until armour
// exists as content; `why` is what the tooltip says when it refuses.
struct EquipSlotDef {
  EquipSlotId id = EquipSlotId::Head;
  const char* label = "";
  // Short engraving key the chrome atlas draws in the empty slot, so an empty
  // helm slot reads as a helm rather than as a hole. Names a sprite, never a
  // pixel offset — see assets/ui/chrome.json.
  const char* icon = "";
  // ItemKinds this slot will take. Terminated by ItemKind::None, which is why
  // ItemKind::None can never itself be an accepted kind.
  ItemKind accepts[4] = {ItemKind::None, ItemKind::None, ItemKind::None,
                         ItemKind::None};
  // What the refusal tooltip says. Written for the player, not the developer.
  const char* why = "";
};

// THE SLOT TABLE. This is the whole of the armour system's schema today: add
// a kind to a row and that slot accepts it, with no other code change.
inline const EquipSlotDef* EquipSlots() {
  static const EquipSlotDef k[kEquipSlotCount] = {
      {EquipSlotId::Head, "Head", "slot_head", {ItemKind::ArmorHead},
       "requires: a helm"},
      {EquipSlotId::Chest, "Chest", "slot_chest", {ItemKind::ArmorChest},
       "requires: a cuirass"},
      {EquipSlotId::Legs, "Legs", "slot_legs", {ItemKind::ArmorLegs},
       "requires: greaves"},
      {EquipSlotId::Boots, "Boots", "slot_boots", {ItemKind::ArmorBoots},
       "requires: boots"},
      {EquipSlotId::Shoulders, "Shoulders", "slot_shoulders",
       {ItemKind::ArmorShoulders}, "requires: pauldrons"},
      {EquipSlotId::Hands, "Hands", "slot_hands", {ItemKind::ArmorHands},
       "requires: gauntlets"},
      {EquipSlotId::Belt, "Belt", "slot_belt", {ItemKind::ArmorBelt},
       "requires: a girdle"},
      {EquipSlotId::Trinket, "Trinket", "slot_trinket", {ItemKind::Trinket},
       "requires: a trinket"},
      // The two rows that accept something today. A sword dragged here proves
      // the whole move/validate/persist pipe end to end, which is the point of
      // shipping them live while the armour rows are scaffolding.
      {EquipSlotId::Sheath, "Sheath", "slot_sheath", {ItemKind::Melee},
       "requires: a melee weapon"},
      {EquipSlotId::Quick0, "Quick I", "slot_quick", {ItemKind::Melee},
       "requires: a carryable item"},
      {EquipSlotId::Quick1, "Quick II", "slot_quick", {ItemKind::Melee},
       "requires: a carryable item"},
      {EquipSlotId::Quick2, "Quick III", "slot_quick", {ItemKind::Melee},
       "requires: a carryable item"},
      {EquipSlotId::Quick3, "Quick IV", "slot_quick", {ItemKind::Melee},
       "requires: a carryable item"},
  };
  return k;
}

inline const EquipSlotDef& EquipSlotAt(int i) {
  static const EquipSlotDef kBad{};
  const EquipSlotDef* t = EquipSlots();
  return (i >= 0 && i < kEquipSlotCount) ? t[i] : kBad;
}

// Does this slot take this kind? ItemKind::None is never accepted, so an empty
// `accepts` list refuses everything by construction rather than by a special
// case.
// Does an item in this slot go ON THE BODY? The sheath and the quick slots
// carry an item without wearing it, so the wear/unwear sync in the tick loop
// must not fire for them — asked here rather than by testing the slot id at
// the call site, because that test would then live in main.cpp AND in the
// gate AND in the save path.
inline bool EquipSlotIsWorn(int slot) {
  if (slot < 0 || slot >= kEquipSlotCount) return false;
  return EquipSlotAt(slot).id <= EquipSlotId::Trinket;
}

inline bool EquipSlotAccepts(int slot, ItemKind kind) {
  if (slot < 0 || slot >= kEquipSlotCount) return false;
  if (kind == ItemKind::None) return false;
  const EquipSlotDef& d = EquipSlotAt(slot);
  for (ItemKind k : d.accepts)
    if (k == kind) return true;
  return false;
}

// ---- WHAT A WORN PIECE HAS BEEN THROUGH -------------------------------------
//
// A shell on the body is a rig slot and carries its own damage: burnt-through
// cloth, a hole a blade opened, the hp that is left. The moment the piece comes
// OFF, that rig slot is destroyed — and with it every trace of what happened to
// the garment, unless something outside the body is holding it.
//
// This is that something. One blob per equip slot, one entry per cover entry in
// the item's own order.
//
// WHY NOT REBUILD FROM THE DEF. Because the def is the PRISTINE piece. Taking
// off your boots to put on a different pair and finding the first pair mended
// is the bug this exists to make unrepresentable, and it is a bug nothing else
// in the suite would notice: everything still works, the armour is just
// quietly free.
//
// THE LATTICE IS THE AUTHORITATIVE ONE — the skin where a shell has a separate
// skin, the collider where it does not. Storing the derived side would be
// storing something the next re-derive overwrites (phys/lattice.h's one-way
// rule), which is a subtler way of losing the damage.
//
// KEYED BY ITEM NAME, not by slot and not by instance.
//
// Not by SLOT, because dragging the robe to the pack and back would then mend
// it — the slot emptied, and the blob went with it. Not by INSTANCE, because
// an ItemStack has no identity: it is a library index and a count, and giving
// stacks instance ids is a far larger change than armour needed.
//
// So the unit is "your robe". Two robes in one pack would share one set of
// holes, which is a real limitation and an acceptable one while nothing in the
// game produces two of anything. What DOES hold — and is what the owner asked
// for — is that damage survives unequip, re-equip, a move through the pack,
// and a save.
struct WornShellDamage {
  float hp = -1.0f;                  // < 0 = as authored
  // ---- CONDITION, as two counts rather than as a fraction -------------------
  // `live` of `atSpawn` voxels are still there. Both 0 means "never measured",
  // which is what an entry for a piece that has not been worn since this was
  // added looks like, and reads as whole.
  //
  // Kept ALONGSIDE the exact lattice, not instead of it, and the distinction is
  // the one the save format's own note makes: the lattice is the truth (it puts
  // the holes back where they were), these are a SUMMARY for a piece that is
  // not on a body, where there is no shell to count and re-deriving the
  // denominator would mean re-running the per-axis fit resample against a
  // wearer who is not wearing it.
  uint32_t atSpawn = 0;
  uint32_t live = 0;
  std::vector<PrefabVoxel> lattice;  // empty = as authored
  bool Empty() const {
    return hp < 0.0f && lattice.empty() && live >= atSpawn;
  }
};

struct WornDamage {
  std::vector<WornShellDamage> shells;
  bool Empty() const {
    for (const WornShellDamage& s : shells)
      if (!s.Empty()) return false;
    return true;
  }
  void Clear() { shells.clear(); }
  // How much of the whole piece is still there, 0..1. Volume-weighted across
  // its shells (Mob::WornCondition computes the same thing off the live rig and
  // the two must agree): a robe is a torso shell and two sleeves, and a mean of
  // per-shell fractions would let a burnt-off sleeve weigh as much as the body
  // of the garment.
  float Condition() const {
    uint64_t a = 0, l = 0;
    for (const WornShellDamage& s : shells) { a += s.atSpawn; l += s.live; }
    if (!a) return 1.0f;
    const float f = (float)l / (float)a;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
  }
};

// ---- RUINED ------------------------------------------------------------------
//
// Past a point a piece is not damaged gear, it is rags: too little of it is
// left to be worth mending, and — because protection here is geometric — too
// little of it is left to be in the way of anything either. That threshold is a
// TUNING VALUE (`gear.ruinedCondition`), not a constant, because the right
// number is a feel judgement and hard-coding it would cost a rebuild to try 0.3
// instead of 0.4.
//
// The comparison lives here as a named function so "what counts as ruined" has
// exactly one spelling, and the threshold is PASSED IN rather than read: this
// header deliberately has no engine coupling (see the note at the top), and
// pulling sim/tuning.h in for one float would be the first crack in that.
//
// A piece can still be WORN while ruined. Nothing here refuses to equip it —
// the shells that remain still occlude what they cover, and taking your last
// scorched hood off because the game decided it was scrap would be worse than
// wearing it. Ruin is what a repair, and anything that scales with condition,
// is expected to refuse.
inline bool GearRuined(float condition, float ruinedAt) {
  return condition < ruinedAt;
}

struct Equipment {
  ItemStack slots[kEquipSlotCount];

  const ItemStack& At(int i) const {
    static const ItemStack kEmpty{};
    return (i >= 0 && i < kEquipSlotCount) ? slots[i] : kEmpty;
  }
  bool Empty() const {
    for (const ItemStack& s : slots)
      if (!s.Empty()) return false;
    return true;
  }
};

// THE BAG: general storage, 4 rows of 8. Unlike the hotbar it has no selection
// and no keys — it is where things live when they are not in hand, which is
// the whole distinction between this and Inventory (game/item.h).
struct Bag {
  static constexpr int kCols = 8;
  static constexpr int kRows = 4;
  static constexpr int kSlots = kCols * kRows;
  ItemStack slots[kSlots];

  const ItemStack& At(int i) const {
    static const ItemStack kEmpty{};
    return (i >= 0 && i < kSlots) ? slots[i] : kEmpty;
  }
  // First free slot, or -1 when full. The caller decides what full means (a
  // pickup refused, an unequip that stays equipped) — this never drops an item
  // on the floor behind the caller's back.
  int FirstFree() const {
    for (int i = 0; i < kSlots; i++)
      if (slots[i].Empty()) return i;
    return -1;
  }
  int Add(int defIndex, int count = 1) {
    if (defIndex < 0 || count <= 0) return -1;
    for (int i = 0; i < kSlots; i++)
      if (!slots[i].Empty() && slots[i].def == defIndex) {
        slots[i].count += count;
        return i;
      }
    int f = FirstFree();
    if (f >= 0) slots[f] = {defIndex, count};
    return f;
  }
};

// The slot ADDRESS (KitSpace/KitRef) lives in its own dependency-free header
// so the UI can name a slot without pulling the item system in — see
// game/kitref.h for why that separation is load-bearing.

// The player's whole non-magic kit, so main.cpp holds ONE thing rather than
// three and the move logic has one place to live.
struct PlayerKit {
  Equipment equip;
  Bag bag;
  // Damage carried by worn pieces that are not currently ON the body (see
  // WornDamage). Held here rather than on Equipment because it outlives any
  // one slot: a robe dragged to the pack and back is the same robe.
  std::map<std::string, WornDamage> wornDamage;

  // Read a piece's damage, or null when it has never been hurt. Const so a
  // caller cannot create an entry by asking.
  const WornDamage* Damage(const std::string& item) const {
    auto it = wornDamage.find(item);
    return it == wornDamage.end() ? nullptr : &it->second;
  }
  // Record it, or forget it when the piece is whole again (a re-wear of an
  // undamaged piece must not leave an empty blob behind to be saved).
  void SetDamage(const std::string& item, WornDamage d) {
    if (item.empty()) return;
    if (d.Empty())
      wornDamage.erase(item);
    else
      wornDamage[item] = std::move(d);
  }

  // Resolve a reference to the stack it names. `hotbar` is passed in rather
  // than owned: the hotbar predates this struct, main.cpp owns it, and moving
  // it in here would be a refactor of the melee path for no gain.
  ItemStack* Resolve(const KitRef& r, Inventory& hotbar) {
    switch (r.space) {
      case KitSpace::Bag:
        return (r.index >= 0 && r.index < Bag::kSlots) ? &bag.slots[r.index]
                                                       : nullptr;
      case KitSpace::Hotbar:
        return (r.index >= 0 && r.index < kItemSlots) ? &hotbar.slots[r.index]
                                                      : nullptr;
      case KitSpace::Equip:
        return (r.index >= 0 && r.index < kEquipSlotCount)
                   ? &equip.slots[r.index]
                   : nullptr;
      default:
        return nullptr;
    }
  }

  // MOVE ONE STACK. Always a SWAP, never an overwrite: dropping a sword onto
  // an occupied slot puts what was there into the slot you came from, which is
  // what every inventory in the genre does and — more importantly — is the
  // only rule under which no item can be destroyed by a mis-drop.
  //
  // The kind check runs on BOTH ends, because a swap moves two items: dragging
  // a sword from the sheath onto a helm would otherwise put the helm in the
  // sheath without the sheath ever being asked.
  MoveResult Move(const KitRef& from, const KitRef& to, Inventory& hotbar,
                  const ItemLibrary& lib) {
    if (from == to) return MoveResult::SameSlot;
    ItemStack* a = Resolve(from, hotbar);
    ItemStack* b = Resolve(to, hotbar);
    if (!a || !b) return MoveResult::BadSlot;
    if (a->Empty()) return MoveResult::Empty;

    auto kindOf = [&](const ItemStack& s) {
      const ItemDef* d = lib.At(s.Empty() ? -1 : s.def);
      return d ? d->kind : ItemKind::None;
    };
    // Only EQUIP slots validate. Bag and hotbar take anything — they are
    // containers, not roles.
    if (to.space == KitSpace::Equip &&
        !EquipSlotAccepts(to.index, kindOf(*a)))
      return MoveResult::WrongKind;
    if (from.space == KitSpace::Equip && !b->Empty() &&
        !EquipSlotAccepts(from.index, kindOf(*b)))
      return MoveResult::WrongKind;

    ItemStack tmp = *a;
    *a = *b;
    *b = tmp;
    return MoveResult::Ok;
  }
};

// ---- DRAWN OR STOWED --------------------------------------------------------
//
// The Sheath slot is where a weapon lives; this is whether it is currently IN
// THE HAND. Two states and one key, which is the whole feature — but the rule
// has three cases that are easy to get subtly wrong, so it lives here as a
// plain struct rather than as a branch in the frame loop:
//
//   * Drawing forces the melee tool. Without it you pull a sword and the left
//     mouse button still paints stone.
//   * Stowing puts the PREVIOUS tool back. Forcing a mode and never
//     un-forcing it strands the player somewhere they did not choose.
//   * A weapon that LEAVES the sheath while drawn is no longer drawn. The
//     character screen can move it into the pack mid-swing, and nothing about
//     the key press knows that happened — so the reconcile runs every frame
//     and there is exactly one place the two facts have to agree.
//
// No engine coupling of any kind (`tool` is an int the caller owns), which is
// what lets --gate armor-wear assert on it with no window and no GPU.
struct SheathState {
  bool drawn = false;
  int toolBefore = 0;

  // The key press. False = nothing to draw; the caller shows a refusal.
  bool Toggle(ItemKind sheathKind, int meleeTool, int& tool) {
    if (drawn) {
      drawn = false;
      tool = toolBefore;
      return true;
    }
    if (sheathKind != ItemKind::Melee) return false;
    drawn = true;
    toolBefore = tool;
    tool = meleeTool;
    return true;
  }

  // Every frame, before the hand is read.
  void Reconcile(ItemKind sheathKind, int meleeTool, int& tool) {
    if (!drawn || sheathKind == ItemKind::Melee) return;
    drawn = false;
    if (tool == meleeTool) tool = toolBefore;
  }
};

// Why a move was refused, as the sentence the panel shows. Kept beside the
// enum so a new MoveResult cannot be added without a message.
inline const char* MoveResultText(MoveResult r, const KitRef& to) {
  switch (r) {
    case MoveResult::Ok:
    case MoveResult::SameSlot:
      return "";
    case MoveResult::Empty:
      return "nothing to move";
    case MoveResult::WrongKind:
      return to.space == KitSpace::Equip ? EquipSlotAt(to.index).why
                                         : "that does not go there";
    case MoveResult::BadSlot:
    default:
      return "no such slot";
  }
}

// ---- name <-> index, the reload/persistence seam ---------------------------
//
// Every stored index dies on an R hot-reload or a save/load across a content
// change, so both directions of the crossing go through these two helpers
// rather than being re-derived at each call site. An unresolvable name drops
// the stack and says so — content may legitimately have been removed between
// saves, and silently keeping an index into a shorter library is the failure
// mode that reads as "my sword turned into a rock".
inline std::string KitItemName(const ItemStack& s, const ItemLibrary& lib) {
  const ItemDef* d = lib.At(s.Empty() ? -1 : s.def);
  return d ? d->name : std::string();
}

inline ItemStack KitItemFromName(const std::string& name, int count,
                                 const ItemLibrary& lib) {
  if (name.empty() || count <= 0) return ItemStack{};
  int i = lib.Find(name);
  return i < 0 ? ItemStack{} : ItemStack{i, count};
}
