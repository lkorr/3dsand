#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/equipment.h"
#include "game/item.h"
#include "game/mob.h"
#include "game/worlditems.h"
#include "phys/debris.h"
#include "phys/physics.h"

// CORPSES YOU CAN LOOT — the registry side of Mob::Die's CorpseReport.
//
// THE SAME SHAPE AS WorldItems, AND FOR THE SAME REASON. A fallen creature is
// a heap of ordinary debris bodies (Mob::Die hands every limb over), and the
// only thing debris cannot carry is identity. WorldItems adds "this body is a
// sword"; this adds "these bodies are one corpse, and these of them are its
// gear". Nothing here is a second physics owner — a robe on a corpse burns,
// gets carved and gets blown apart exactly as it did the frame before the
// creature died, and this registry only finds out afterwards.
//
// THE REGISTRY MUST NOT OUTLIVE THE BODIES. Jolt reuses handles, so an entry
// naming a freed one would eventually match a NEW body: "I looted a rock and
// got a cuirass". DebrisSystem fires one OnBodyGone per released body and
// main.cpp fans it out to both registries; a piece whose identity body goes
// (burnt off the corpse, culled, blown up) leaves the loot list, and a corpse
// with no bodies left is forgotten. The list is CAPPED, oldest first, because
// a battlefield is exactly the case where nothing else bounds it.
//
// NOT SAVED. The debris a corpse became persists through DebrisSystem's own
// save, but as anonymous bodies: a loaded world has heaps, not corpses. The
// ground-item path made the same call ('ITMS' re-drops items rather than
// matching handles), and a corpse's gear list is a smaller loss than a sword.
class Corpses {
 public:
  static constexpr size_t kMax = 64;

  void Add(CorpseReport r) {
    if (!r.mobId || r.bodies.empty()) return;
    Remove(r.mobId);
    if (corpses_.size() >= kMax) corpses_.erase(corpses_.begin());
    corpses_.push_back(std::move(r));
  }
  CorpseReport* Find(uint64_t mobId) {
    for (CorpseReport& c : corpses_)
      if (c.mobId == mobId) return &c;
    return nullptr;
  }
  const CorpseReport* Find(uint64_t mobId) const {
    for (const CorpseReport& c : corpses_)
      if (c.mobId == mobId) return &c;
    return nullptr;
  }
  // The corpse ANY of whose bodies is `body` — the crosshair's question. A
  // sword lying in the hand of a corpse answers as the corpse, which is what
  // makes "E to loot" cover the whole heap rather than the torso only.
  const CorpseReport* FindByBody(uint64_t body) const {
    if (!body) return nullptr;
    for (const CorpseReport& c : corpses_)
      for (uint64_t b : c.bodies)
        if (b == body) return &c;
    return nullptr;
  }
  bool Remove(uint64_t mobId) {
    for (size_t i = 0; i < corpses_.size(); i++)
      if (corpses_[i].mobId == mobId) {
        corpses_.erase(corpses_.begin() + i);
        return true;
      }
    return false;
  }
  // DebrisSystem's release seam. A body that is gone is gone from the hover
  // set, from every rag list, and — if it was the body that IS a piece — takes
  // the piece with it. Invalidates any CorpseReport pointer the caller holds.
  void OnBodyGone(uint64_t body) {
    if (!body) return;
    for (size_t ci = 0; ci < corpses_.size();) {
      CorpseReport& c = corpses_[ci];
      for (size_t i = 0; i < c.bodies.size();)
        if (c.bodies[i] == body) c.bodies.erase(c.bodies.begin() + i);
        else i++;
      for (size_t g = 0; g < c.gear.size();) {
        CorpseReport::Piece& p = c.gear[g];
        for (size_t i = 0; i < p.rags.size();)
          if (p.rags[i] == body) p.rags.erase(p.rags.begin() + i);
          else i++;
        if (p.body == body) c.gear.erase(c.gear.begin() + g);
        else g++;
      }
      // A corpse with nothing left of it is forgotten — AND SO IS ITS PACK.
      // Carried stacks (CorpseReport::Piece with no body) have no handle of
      // their own to keep the entry alive, so a body that burns away entirely
      // takes its purse with it. That is the reading, not an oversight: what
      // is left of a man who burned to nothing is nothing.
      if (c.bodies.empty()) corpses_.erase(corpses_.begin() + ci);
      else ci++;
    }
  }
  void Clear() { corpses_.clear(); }
  const std::vector<CorpseReport>& All() const { return corpses_; }
  size_t Count() const { return corpses_.size(); }

 private:
  std::vector<CorpseReport> corpses_;
};

// Is any body of the corpse within `maxDist` voxels of `from`? The loot panel
// closes when the player walks away, and "away" is measured against where the
// heap actually lies now, not where the creature stood when it fell.
inline bool CorpseWithin(const CorpseReport& c, const Physics& phys, Vec3 from,
                         float maxDist) {
  for (uint64_t b : c.bodies) {
    Vec3 at{};
    if (!phys.BodyCenterOfMass(b, at)) continue;
    const Vec3 d{at.x - from.x, at.y - from.y, at.z - from.z};
    if (d.x * d.x + d.y * d.y + d.z * d.z <= maxDist * maxDist) return true;
  }
  return false;
}

// ---- taking a piece off a corpse --------------------------------------------
//
// Executes the character screen's loot intents against the real containers,
// beside PlayerKit::Move rather than inside it: a corpse is not one of the
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

// Take corpse.gear[index] into `dest` — or, with dest.space == None, into the
// first place it fits (pack, then hotbar). On Ok the piece's identity body and
// rags are DESTROYED (the robe leaves the corpse), its damage is filed in the
// kit by name with the identity shell's lattice refreshed off the body as it
// is NOW (a robe that went on burning after its wearer died comes off burnt),
// and `outItem` names what was taken.
//
// AFTER A SUCCESSFUL CALL `corpse` MAY BE GONE: destroying the last body fires
// OnBodyGone, and the registry drops an empty corpse from under the reference.
// Callers re-find the corpse by id before touching it again.
inline LootResult TakeCorpseLoot(CorpseReport& corpse, int index, KitRef dest,
                                 PlayerKit& kit, Inventory& hotbar,
                                 const ItemLibrary& lib, DebrisSystem& debris,
                                 std::string* outItem = nullptr) {
  if (index < 0 || index >= (int)corpse.gear.size())
    return LootResult::NoSuchPiece;
  const int di = lib.Find(corpse.gear[index].item);
  const ItemDef* def = lib.At(di);
  if (!def) {
    // Content that was removed between the death and the loot. Not a thing
    // any more, so not a thing on the corpse either — the shells stay as the
    // rags they are.
    corpse.gear.erase(corpse.gear.begin() + index);
    return LootResult::UnknownItem;
  }

  // ---- find it a home, without touching anything yet ----------------------
  //
  // THE WHOLE STACK MOVES, AND THE PROBE DOES NOT CHANGE FOR IT. A carried
  // entry (CorpseReport::Piece::count) can be a dozen of something, and the
  // one-unit probe below is still the right question because NO STACK IN THIS
  // GAME HAS A MAXIMUM: Bag::Add and Inventory::Add merge by `count += count`
  // with no ceiling, so N units fit in exactly the places one unit fits. If a
  // stack limit is ever added, this is the comment that has to stop being
  // true — the probe would then need capacity for N, and a partial take.
  if (dest.space == KitSpace::None) {
    // Probe only: Bag::Add would place it, and a refusal must leave the
    // corpse untouched. First free bag slot or a stack of the same item.
    bool room = kit.bag.FirstFree() >= 0;
    for (const ItemStack& s : kit.bag.slots)
      if (!s.Empty() && s.def == di) room = true;
    if (!room)
      for (const ItemStack& s : hotbar.slots)
        if (s.Empty() || s.def == di) room = true;
    if (!room) return LootResult::NoRoom;
  } else {
    if (dest.space == KitSpace::Loot) return LootResult::WrongKind;
    ItemStack* d = kit.Resolve(dest, hotbar);
    if (!d) return LootResult::BadSlot;
    if (dest.space == KitSpace::Equip &&
        !EquipSlotAccepts(dest.index, def->kind))
      return LootResult::WrongKind;
    // A slot holding the same def in a DIFFERENT colour is not a stack this
    // can grow (game/dye.h): it swaps, so it needs the bag free exactly as a
    // different item would.
    if (!d->Empty() &&
        (d->def != di || d->dye != corpse.gear[index].dye) &&
        kit.bag.FirstFree() < 0)
      return LootResult::NoRoom;
  }

  // ---- commit ----------------------------------------------------------------
  CorpseReport::Piece piece = std::move(corpse.gear[index]);
  corpse.gear.erase(corpse.gear.begin() + index);
  if (outItem) *outItem = piece.item;

  // THE COLOUR COMES WITH IT (game/dye.h). It is carried on the piece rather
  // than re-derived, because there is nothing to re-derive it from: a dye is
  // not recoverable from the greyscale art it colours.
  const int take = piece.count > 0 ? piece.count : 1;
  if (dest.space == KitSpace::None) {
    int where = kit.bag.Add(di, take, piece.dye);
    if (where < 0) where = hotbar.Add(di, take, piece.dye);
    (void)where;   // proven above
  } else {
    ItemStack* d = kit.Resolve(dest, hotbar);
    if (d->Empty()) {
      *d = ItemStack{di, take, piece.dye};
    } else if (d->def == di && d->dye == piece.dye) {
      d->count += take;
    } else {
      // SWAP-NEVER-OVERWRITE, with the pack standing in for the corpse as the
      // other end: what was in the slot goes to the first free bag slot.
      const int free = kit.bag.FirstFree();
      kit.bag.slots[free] = *d;
      *d = ItemStack{di, take, piece.dye};
    }
  }

  // ---- the damage, as the piece is NOW ---------------------------------------
  // CaptureWorn recorded the shells at death. The identity shell has been a
  // debris body since, and may have burned or been carved further; its lattice
  // is the truth for that one cover entry, exactly as the 'ITMS' save reads a
  // dropped item's holes off its body. The rags' entries keep the death-time
  // record: they are destroyed below, and a rag's damage is invisible until the
  // piece is worn again.
  if (ItemKindIsWorn(def->kind) && piece.identityCover >= 0) {
    std::vector<PrefabVoxel> lat;
    uint32_t scale = 1;
    if (debris.BodyLatticeOf(piece.body, lat, scale)) {
      if ((int)piece.damage.shells.size() <= piece.identityCover)
        piece.damage.shells.resize((size_t)piece.identityCover + 1);
      WornShellDamage& sh = piece.damage.shells[(size_t)piece.identityCover];
      if (sh.atSpawn == 0) sh.atSpawn = (uint32_t)lat.size();
      sh.live = (uint32_t)lat.size();
      if (sh.live < sh.atSpawn) sh.lattice = std::move(lat);
      else sh.lattice.clear();
    }
    kit.SetDamage(piece.item, std::move(piece.damage));
  }

  // ---- and the corpse loses it -----------------------------------------------
  // Last, because the release hook may erase `corpse` out from under us on the
  // final body (see the header note).
  //
  // A CARRIED STACK HAS NO BODY AND THERE IS NOTHING TO DESTROY. Erasing the
  // entry above WAS the whole removal — it was never an object in the world.
  // Which also means a take of a pack item cannot invalidate `corpse`: no body
  // leaves, so OnBodyGone does not fire.
  for (uint64_t r : piece.rags) debris.DestroyBody(r);
  if (piece.body) debris.DestroyBody(piece.body);
  return LootResult::Ok;
}

// Pull corpse.gear[index] OFF the corpse and leave it on the floor — the drag
// out of the loot panel. The piece's body is already lying there, so nothing
// is created: the rags go, and the identity body is registered as a ground
// item so `E` sees it as the thing it is. Does not invalidate `corpse`.
inline bool ShedCorpseLoot(CorpseReport& corpse, int index,
                           DebrisSystem& debris, WorldItems& ground,
                           std::string* outItem = nullptr) {
  if (index < 0 || index >= (int)corpse.gear.size()) return false;
  // A CARRIED STACK CANNOT BE SHED, because this function creates nothing: its
  // whole premise is that the body is already lying there and only needs its
  // strap cut and its identity registered. A pack item never had a body, so
  // shedding it would have to SPAWN one, which is the drop path main.cpp
  // already owns for dragging something out of the bag. Refused here so the
  // caller falls back to that rather than silently deleting the stack.
  if (corpse.gear[index].body == 0) return false;
  CorpseReport::Piece piece = std::move(corpse.gear[index]);
  corpse.gear.erase(corpse.gear.begin() + index);
  if (outItem) *outItem = piece.item;
  // OFF THE CORPSE MEANS OFF IT. A worn shell on a corpse is strapped to the
  // limb it covers (DebrisSystem::StrapBody), so "the body is already lying
  // there" is only true once the strap is cut — otherwise a breastplate dragged
  // out of the loot panel went on riding the chest it was dragged off.
  debris.UnstrapBody(piece.body);
  // The body stays in the corpse's hover set on purpose: the crosshair asks
  // the ground registry FIRST, so the shed robe reads as a robe, and when it
  // is picked up its release fires OnBodyGone, which is what takes it out of
  // the heap. One door out, the same one every other body uses.
  // ...in the colour it was worn in: the body already renders dyed (its
  // MicroBodyRef carries the word), and this is the copy that makes picking it
  // up hand back the same garment (game/dye.h).
  ground.Add(piece.body, piece.item, piece.dye);
  for (uint64_t r : piece.rags) debris.DestroyBody(r);
  return true;
}
