#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/equipment.h"
#include "game/item.h"
#include "game/mob.h"

// LOOTING A CORPSE — the loot panel's intents, executed against a DEAD MOB.
//
// A corpse is a Mob now (docs/PLAN_corpse_is_a_mob.md): its rig, its worn
// shells, the sword in its hand and its pack are all still where they were,
// so there is no registry here any more. The panel addresses a dead Mob by id
// (PlayerSession::lootCorpse) and its entries are read LIVE off the body
// (Mob::LootPieces: worn, then held, then pack — the order the panel and "take
// all" have always used). A robe that burns off the corpse is simply not in
// the next list; nothing has to be told that its body went.
//
// NOT FOR THE AVATAR (Mob::Lootable): the player's kit stays on the avatar
// (Mob::kit_) and re-dresses the respawned rig, so their own corpse holding a
// second copy would be a duplication machine.

// ---- taking a piece off a corpse --------------------------------------------
//
// Executes the character screen's loot intents against the real containers,
// beside Kit::Move rather than inside it: a corpse is not one of the
// player's containers and its entry has no ItemStack to swap. What it shares
// with Move is the discipline — the kind check on an equip destination is
// EquipSlotAccepts, the refusal is a sentence, and nothing is ever destroyed by
// a mis-drop: an occupied destination sends its stack to the pack rather than
// overwriting it, and a full pack refuses with the piece still on the corpse.
enum class LootResult : uint8_t {
  Ok = 0,
  NoSuchPiece,   // index past the list (a stale mirror, reported not clamped)
  UnknownItem,   // the library no longer has this item: the entry is dropped
  NoRoom,        // nowhere to put it — pack full, or the swap has no home
  WrongKind,     // the equip destination refuses this kind
  BadSlot,       // a destination that does not resolve
};

inline const char* LootResultText(LootResult r, const KitRef& to) {
  switch (r) {
    case LootResult::Ok: return "";
    case LootResult::NoSuchPiece: return "there is nothing there";
    case LootResult::UnknownItem: return "that is not a thing any more";
    case LootResult::NoRoom: return "you have no room for that";
    case LootResult::WrongKind:
      return to.space == KitSpace::Equip ? EquipSlotAt(to.index).why
                                         : "that does not go there";
    case LootResult::BadSlot:
    default: return "no such slot";
  }
}

// Take loot entry `index` of `corpse` into `looter`'s kit at `dest` — or, with
// dest.space == None, into the first place it fits (pack, then hotbar). On Ok
// the piece leaves the body (Mob::TakeLootPiece: a worn piece's shells go out
// of the world with it, the sword out of the hand, a pack stack off the list)
// and lands in the kit as the WHOLE ItemInstance it was (W2-M): its colour,
// what a vessel holds, and a worn piece's damage exactly as it is on the body
// NOW (a robe that went on burning after its wearer died comes off burnt).
// `outItem` names what was taken.
inline LootResult TakeCorpseLoot(Mob& corpse, int index, KitRef dest,
                                 Mob& looter, const ItemLibrary& lib,
                                 std::string* outItem = nullptr) {
  Kit& kit = looter.KitMut();
  std::vector<LootPiece> list;
  corpse.LootPieces(list);
  if (index < 0 || index >= (int)list.size()) return LootResult::NoSuchPiece;
  const LootPiece& want = list[(size_t)index];
  const ItemDef* def = lib.Of(want);
  if (!def) {
    // Content that was removed between the death and the loot. Not a thing
    // any more, so not a thing on the corpse either: the entry leaves.
    corpse.TakeLootPiece(index, nullptr);
    return LootResult::UnknownItem;
  }
  // What would land, as one stack: the entry's instance, with its count.
  ItemStack incoming = static_cast<const ItemInstance&>(want);
  if (incoming.count <= 0) incoming.count = 1;

  // ---- find it a home, without touching anything yet ----------------------
  //
  // THE WHOLE STACK MOVES, AND THE PROBE DOES NOT CHANGE FOR IT. A carried
  // entry can be a dozen of something, and the one-unit probe below is still
  // the right question because NO STACK IN THIS GAME HAS A MAXIMUM: Bag::Add
  // and Inventory::Add merge by `count += count` with no ceiling, so N units
  // fit in exactly the places one unit fits. If a stack limit is ever added,
  // this is the comment that has to stop being true — the probe would then
  // need capacity for N, and a partial take.
  if (dest.space == KitSpace::None) {
    // Probe only: Bag::Add would place it, and a refusal must leave the
    // corpse untouched. First free bag slot or a stack it would MERGE into
    // (ItemInstance::StacksWith) -- the rule Bag::Add and Inventory::Add apply.
    bool room = kit.bag.FirstFree() >= 0;
    for (const ItemStack& s : kit.bag.slots)
      if (!s.Empty() && s.StacksWith(incoming)) room = true;
    if (!room)
      for (const ItemStack& s : kit.hotbar.slots)
        if (s.Empty() || s.StacksWith(incoming)) room = true;
    if (!room) return LootResult::NoRoom;
  } else {
    if (dest.space == KitSpace::Loot) return LootResult::WrongKind;
    ItemStack* d = kit.Resolve(dest);
    if (!d) return LootResult::BadSlot;
    if (dest.space == KitSpace::Equip &&
        !EquipSlotAccepts(dest.index, def->kind))
      return LootResult::WrongKind;
    // A slot holding the same item in a DIFFERENT colour (or a different
    // object: damaged, filled) is not a stack this can grow (game/dye.h): it
    // swaps, so it needs the bag free exactly as a different item would.
    if (!d->Empty() && !d->StacksWith(incoming) && kit.bag.FirstFree() < 0)
      return LootResult::NoRoom;
  }

  // ---- commit ----------------------------------------------------------------
  // Off the body FIRST: a take that could not happen (the body changed under
  // a stale mirror) must not hand out a copy.
  LootPiece piece;
  if (!corpse.TakeLootPiece(index, &piece)) return LootResult::NoSuchPiece;
  if (outItem) *outItem = piece.name;

  // THE WHOLE INSTANCE COMES WITH IT. The colour is not recoverable from the
  // greyscale art it colours, a flask's contents are not recoverable from its
  // glass, and a worn piece's holes were captured off its live shells the
  // instant before they left the body (Mob::LootPieces -> CaptureWorn).
  ItemStack got = static_cast<const ItemInstance&>(piece);
  if (got.count <= 0) got.count = 1;
  if (!ItemKindIsWorn(def->kind)) got.damage.Clear();
  if (dest.space == KitSpace::None) {
    int where = kit.bag.Add(got);
    if (where < 0) where = kit.hotbar.Add(got);
    (void)where;   // proven above
  } else {
    // An equip destination may be WORN by the looter: its current occupant
    // leaves with its holes (Mob::KitTake flushes the shells), and the slot is
    // re-dressed from what lands in it (Mob::DressFromKit).
    if (dest.space == KitSpace::Equip) looter.KitFlushWorn(dest.index);
    ItemStack* d = kit.Resolve(dest);
    if (d->Empty()) {
      *d = got;
    } else if (d->StacksWith(got)) {
      d->count += got.count;
    } else {
      // SWAP-NEVER-OVERWRITE, with the pack standing in for the corpse as the
      // other end: what was in the slot goes to the first free bag slot.
      const int free = kit.bag.FirstFree();
      kit.bag.slots[free] = *d;
      *d = got;
    }
    if (dest.space == KitSpace::Equip) looter.KitWornStale(dest.index);
  }
  return LootResult::Ok;
}
// Pull loot entry `index` OFF the corpse and leave it on the floor — the drag
// out of the loot panel (Mob::ShedLootPiece: the identity shell or the sword
// is cut loose as debris and registered as a ground item through
// MobSystem::SetOnItemShed). A carried stack has no body to cut loose and is
// refused here; the caller drops it through the bag's own drop path.
inline bool ShedCorpseLoot(Mob& corpse, int index,
                           std::string* outItem = nullptr) {
  return corpse.ShedLootPiece(index, outItem);
}
