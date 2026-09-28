#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "game/hand.h"
#include "game/item.h"
#include "game/kitref.h"

// WHAT THE CHARACTER IS WEARING AND CARRYING — the equipment slots, the bag,
// and (since W2-M) the Kit that bundles them with the hotbar.
//
// Deliberately shaped like game/item.h's Inventory and game/caster.h's
// GlyphInventory, and for the same reasons: plain structs with no engine
// coupling. The Kit is owned by the creature that carries it (Mob::kit_).
// Nothing here reaches into the rig, physics or the grid — an
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
//   * No visual sheathing. Sheath holds a melee item as DATA, and since
//     2026-09-23 nothing draws from it: the hand is the selected hotbar slot,
//     the way a flask is held. Drawing it on the BACK while stowed needs a
//     `sheath_back` socket in the rig plus a matching grip context on the item
//     (game/item.h's ItemGrip map already anticipates exactly this), which is
//     content, not code.
//   * No weight, no armour class, no set bonuses. Protection here is geometric
//     occlusion plus material identity — a steel plate stops acid because
//     `steel` carries no `tag:dissolvable`, not because it has a resist field.
//     Durability IS real, but it is the shell's own hp and its own voxels
//     being carved away, not a number counting down.
//
// THE INDEX HAZARD IS GONE (W2-M). A slot used to hold an index into
// ItemLibrary::items, which is FILE-ORDER dependent and invalidated by every R
// hot-reload, and every save, packet and reload had to translate. A slot is
// now an ItemInstance and holds the item's NAME; ItemLibrary::Of resolves it
// at the moment a def is wanted.

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
  // THE HANDS (dual wielding, 2026-09-27). What is IN each hand, as a real
  // kit slot rather than "whichever hotbar slot is selected": Q moves the
  // selected hotbar stack into the left hand and E into the right (a swap, so
  // what was held goes back into that hotbar slot), and the rig holds
  // exactly these two (Mob::HoldFromKit). Being kit slots is the point — the
  // save, the loot list, a knock-out, the character screen and an NPC all
  // name the thing in a hand by the one address they already use for a helm.
  // Not WORN (EquipSlotIsWorn stops at Trinket): a held item is the borrowed
  // rig slot Mob::EquipItem makes, not a set of shells.
  HandR,
  HandL,
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
  // The hand the item would go into is too hurt to grip it (Mob::KitMove,
  // melee.injuredArmDrop). Refused rather than equipped and then dropped by
  // Mob::GripFails on the next tick.
  HandTooHurt,
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
      // What a hand can hold is what the rig can grip: a weapon or a vessel.
      // Armour is worn, not held, and a trinket is worn round the neck.
      {EquipSlotId::HandR, "Right hand", "slot_hands",
       {ItemKind::Melee, ItemKind::Container}, "requires: something to hold"},
      {EquipSlotId::HandL, "Left hand", "slot_hands",
       {ItemKind::Melee, ItemKind::Container}, "requires: something to hold"},
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

// The kit slot that IS a hand, and back. -1 / false for every other slot.
inline int EquipSlotOfHand(Hand h) {
  return h == Hand::Left ? (int)EquipSlotId::HandL : (int)EquipSlotId::HandR;
}
inline bool EquipSlotIsHand(int slot, Hand* out = nullptr) {
  if (slot == (int)EquipSlotId::HandR) { if (out) *out = Hand::Right; return true; }
  if (slot == (int)EquipSlotId::HandL) { if (out) *out = Hand::Left; return true; }
  return false;
}

inline bool EquipSlotAccepts(int slot, ItemKind kind) {
  if (slot < 0 || slot >= kEquipSlotCount) return false;
  if (kind == ItemKind::None) return false;
  const EquipSlotDef& d = EquipSlotAt(slot);
  for (ItemKind k : d.accepts)
    if (k == kind) return true;
  return false;
}

struct Equipment;   // defined below; EquipSlotFor needs only a forward decl

// WHERE A PIECE GOES WHEN NOBODY AIMED — the destination the right-click
// "just put this on" gesture picks. -1 when nothing on the body takes the kind.
//
// It lives here rather than in the frame loop for the reason everything else in
// this header does: it is a RULE about the slot table, so it can be asserted
// with no window, no GPU and no input stack (`--gate player-kit`). The version
// that was inline in main.cpp could only be tested by a human clicking.
//
// PREFERS AN EMPTY SLOT over the first that merely accepts. Both are legal
// destinations — a full slot swaps, which is the same thing a drag onto it
// does — but right-clicking two rings should put them on two fingers, not put
// one on and then knock it off with the other. The fallback still returns the
// first accepting slot so a full set can be swapped a piece at a time.
inline int EquipSlotFor(ItemKind kind, const Equipment& eq);

// ---- WHAT A WORN PIECE HAS BEEN THROUGH -------------------------------------
//
// WornShellDamage / WornDamage live in game/iteminstance.h since W2-M: the
// damage is a field of the ItemInstance (`ItemInstance::damage`), carried by
// the slot the piece is in, so two robes in one pack are two robes with their
// own holes. It used to be a map on the player's kit keyed by item NAME, which
// made "your robe" the unit and let a second robe share the first one's
// wounds.
//
// While a piece is ON a body its shells are the live truth and the stack's
// `damage` is what it was when it went on; Mob::KitFlushWorn writes the shells
// back into the stack at every moment the stack is about to be read without
// the body — a move out of the slot, a recolour, a save (game/mob.h, "THE
// KIT").

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
  // What is in a hand (EquipSlotId::HandR/HandL).
  ItemStack& InHand(Hand h) { return slots[EquipSlotOfHand(h)]; }
  const ItemStack& InHand(Hand h) const { return slots[EquipSlotOfHand(h)]; }
  bool Empty() const {
    for (const ItemStack& s : slots)
      if (!s.Empty()) return false;
    return true;
  }
};

inline int EquipSlotFor(ItemKind kind, const Equipment& eq) {
  int first = -1;
  for (int s = 0; s < kEquipSlotCount; s++) {
    if (!EquipSlotAccepts(s, kind)) continue;
    // The hands are filled by Q/E (or a drag), never by "put this on": a
    // right-click on a sword means stow it, and a flask right-clicked into
    // your fist would be a grab nobody asked for.
    if (EquipSlotIsHand(s)) continue;
    if (eq.At(s).Empty()) return s;
    if (first < 0) first = s;
  }
  return first;
}

// THE BAG: general storage, 4 rows of 8. Unlike the hotbar it has no selection
// and no keys — it is where things live when they are not in hand, which is
// the whole distinction between this and Inventory (game/item.h). It is also
// a creature's PACK (Mob::Carried): an NPC's loot-table stacks live here.
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
  // Merges by the ItemInstance::StacksWith rule (same plain item, same dye),
  // else the first free slot; -1 when full.
  int Add(const ItemStack& s) { return AddToSlots(slots, kSlots, s); }
  int Count() const {
    int n = 0;
    for (const ItemStack& s : slots)
      if (!s.Empty()) n++;
    return n;
  }
  // The slot holding the `n`th non-empty stack in slot order, or -1. A pack's
  // entries are addressed that way (a loot list, a save record) so the holes a
  // take leaves never become entries.
  int NthUsed(int n) const {
    if (n < 0) return -1;
    for (int i = 0; i < kSlots; i++)
      if (!slots[i].Empty() && n-- == 0) return i;
    return -1;
  }
};

// The slot ADDRESS (KitSpace/KitRef) lives in its own dependency-free header
// so the UI can name a slot without pulling the item system in — see
// game/kitref.h for why that separation is load-bearing.

// ---- THE KIT: EVERYTHING ONE CREATURE CARRIES (W2-M) ------------------------
//
// Equipment, the bag and the hotbar, as ONE thing, owned by the creature
// (Mob::kit_, game/mob.h). It was `PlayerKit` on the session plus a separate
// hotbar, while the rig kept its own copy of what was worn (`Mob::worn_`) and a
// per-tick loop in session.cpp reconciled the two through latches. Now the
// Kit is the truth and the rig's worn shells are DERIVED from its equipment
// (Mob::DressFromKit); an NPC's pack is its Kit's bag.
//
// A plain struct: nothing here reaches into the rig. The one rule the rig
// needs from its callers — a stack leaving a WORN equip slot must take the
// shells' live damage with it — is why a creature's kit is moved through
// Mob::KitMove / Mob::KitTake rather than through Move below when the
// creature is wearing it.
struct Kit {
  Equipment equip;
  Bag bag;
  Inventory hotbar;

  // Resolve a reference to the stack it names.
  ItemStack* Resolve(const KitRef& r) {
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
  const ItemStack* Resolve(const KitRef& r) const {
    return const_cast<Kit*>(this)->Resolve(r);
  }

  // MOVE ONE STACK. Always a SWAP, never an overwrite: dropping a sword onto
  // an occupied slot puts what was there into the slot you came from, which is
  // what every inventory in the genre does and — more importantly — is the
  // only rule under which no item can be destroyed by a mis-drop.
  //
  // The kind check runs on BOTH ends, because a swap moves two items: dragging
  // a sword from the sheath onto a helm would otherwise put the helm in the
  // sheath without the sheath ever being asked.
  //
  // Data only. On a creature that is WEARING this kit, call Mob::KitMove,
  // which flushes the shells into the stacks first and re-dresses after.
  MoveResult Move(const KitRef& from, const KitRef& to,
                  const ItemLibrary& lib) {
    if (from == to) return MoveResult::SameSlot;
    ItemStack* a = Resolve(from);
    ItemStack* b = Resolve(to);
    if (!a || !b) return MoveResult::BadSlot;
    if (a->Empty()) return MoveResult::Empty;

    auto kindOf = [&](const ItemStack& s) {
      const ItemDef* d = lib.Of(s);
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

    std::swap(*a, *b);
    return MoveResult::Ok;
  }

  // Everything in it, bag then hotbar then equipment — the order the old
  // avatar-kit callback handed a rising its stacks in.
  template <typename F>
  void ForEachStack(F&& f) {
    for (ItemStack& s : bag.slots) f(s);
    for (ItemStack& s : hotbar.slots) f(s);
    for (ItemStack& s : equip.slots) f(s);
  }

  // THE ONE RELOAD RULE. Every slot holds a NAME, so an R hot-reload that
  // reorders items.json changes nothing here; the only thing it can do to a
  // kit is REMOVE an item, and a slot naming one is emptied with a log line
  // rather than kept pointing at nothing. A vessel's contents are re-clamped
  // to the (possibly edited) capacity. Returns how many slots it emptied.
  int DropUnknown(const ItemLibrary& lib) {
    int dropped = 0;
    ForEachStack([&](ItemStack& s) {
      if (s.Empty()) return;
      const ItemDef* d = lib.Of(s);
      if (!d) {
        dropped++;
        s = ItemStack{};
        return;
      }
      if (!d->IsContainer()) {
        s.ClearFill();
      } else {
        // Over capacity (a def that shrank): the last portions give way.
        while ((int)s.FillTotal() > d->container.capacity && s.Filled()) {
          const uint32_t over = s.FillTotal() - (uint32_t)d->container.capacity;
          s.contents.Take(s.contents.p[s.contents.n - 1].mat, over);
        }
      }
    });
    return dropped;
  }
  void Clear() { *this = Kit{}; }
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
    case MoveResult::HandTooHurt:
      if (to.space == KitSpace::Equip && to.index == EquipSlotOfHand(Hand::Left))
        return "your left hand is too hurt to hold that";
      if (to.space == KitSpace::Equip && to.index == EquipSlotOfHand(Hand::Right))
        return "your right hand is too hurt to hold that";
      return "that hand is too hurt to hold anything";
    case MoveResult::BadSlot:
    default:
      return "no such slot";
  }
}

// ---- instance <-> library, the reload/persistence seam ---------------------
//
// A stack is an ItemInstance and already carries its NAME, so nothing crosses
// a reload or a save by index any more. What remains is resolving a name that
// arrived from outside (a save, a packet, a loot list) into a stack this
// library can back: an unresolvable name drops the stack and the caller says
// so — content may legitimately have been removed between saves, and keeping
// a name nothing can resolve is the failure mode that reads as "my sword
// turned into a rock".
inline ItemStack KitStackFrom(const ItemInstance& in, const ItemLibrary& lib) {
  if (in.Empty() || lib.Find(in.name) < 0) return ItemStack{};
  return in;
}
