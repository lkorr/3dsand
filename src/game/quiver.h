#pragma once
// quiver.h -- A QUIVER AND THE ARROWS IN IT (2026-10-15, docs/PLAN_poison_arrows.md).
//
// ONE CONCEPT: a quiver is an item (ItemKind::Quiver) whose instance HOLDS
// other instances (ItemInstance::held). Every arrow in it is an object of its
// own -- its own lattice, its own coat (game/itemcoat.h) -- so a poisoned arrow
// stays poisoned, and a bloody one pulled out of a man stays bloody, while the
// quiver is still one slot in the kit. Plain arrows merge into one counted
// entry by the stack rule (ItemInstance::StacksWith); a coated one is an
// entry of count 1. Nothing here knows about coats beyond that rule.
//
// WHERE IT IS WORN: EquipSlotId::Back. The worn quiver is the ONLY thing the
// bow draws from (owner, 2026-10-15: a quiver is required; loose arrows in the
// bag are carried, not shot). The arrow it gives is RANDOM -- weighted by
// count, keyed on the caller's stateless hash (seed, tick, shooter), so a
// replay or a peer draws the same one. Keep a poisoned quiver and a plain one
// and swap them on your back to choose.
//
// A STORED ARROW IS FROZEN (owner rule 2026-10-01, the bag's): nothing ticks
// the arrows in a quiver, worn or not, so a coat neither dries nor spreads.
//
// One level deep: a quiver never goes in a quiver, and an entry never holds
// anything itself (iteminstance.h's reader refuses it).

#include <cstdint>
#include <vector>

#include "game/item.h"
#include "game/iteminstance.h"
#include "game/renderonly.h"

struct Kit;
class Mob;

// ---- ONE QUIVER ---------------------------------------------------------------
// Arrows in it (every entry's count).
int QuiverCount(const ItemInstance& q);
// How many more it takes (capacity - count, never negative).
int QuiverRoom(const ItemInstance& q, const ItemDef& qd);
// Does this quiver take `a`? Its ammo by name, a single level (an instance
// holding things never goes in).
bool QuiverTakes(const ItemDef& qd, const ItemInstance& a);
// Put up to `n` (all of it when n < 0) of the stack `from` into the quiver,
// as far as there is room: a plain stack merges into the plain entry, a
// coated one goes in one entry per arrow. `from.count` is reduced (emptied at
// 0). Returns how many went in; 0 when the quiver refuses it or is full.
int QuiverLoad(ItemInstance& q, const ItemDef& qd, ItemInstance& from, int n = -1);
// Put ONE arrow back (or in) with no capacity check -- a shot that could not
// be launched returns to where it was, never into nothing.
void QuiverPut(ItemInstance& q, ItemInstance one);
// THE DRAW: which entry the arrow numbered `key % count` falls in (entries in
// order, each as wide as its count). -1 for an empty quiver or no `ammo` in
// it. Only entries named `ammo` are counted ("" = any).
int QuiverPick(const ItemInstance& q, uint32_t key, const std::string& ammo = {});
// One arrow out of entry `i` (count 1, the entry's lattice and coat with it).
// Empty for a bad index.
ItemInstance QuiverTakeOne(ItemInstance& q, int i);

// ---- THE QUIVER ON YOUR BACK ---------------------------------------------------
// The stack in the kit's Back slot when it is a loaded-or-not quiver, else
// null (`def` = its def).
ItemInstance* KitWornQuiver(Kit& kit, const ItemLibrary& lib,
                            const ItemDef** def = nullptr);
const ItemInstance* KitWornQuiver(const Kit& kit, const ItemLibrary& lib,
                                  const ItemDef** def = nullptr);

// THE KIT TAKES AN ARROW IT WAS HANDED (a pickup, a pull out of a body): into
// the worn quiver if it takes it and has room, else the bag, else the hotbar.
// False with nothing changed when there is no room anywhere -- nothing a take
// does can lose an item. `intoQuiver` says which happened.
bool KitReceiveArrow(Kit& kit, const ItemLibrary& lib, const ItemInstance& one,
                     bool* intoQuiver = nullptr);

// ---- THE QUIVER AS SEEN --------------------------------------------------------
// The quiver on a creature's back (its kit's Back slot, the rig's `back`
// socket): its own art, and the tails of the arrows it holds standing out of
// its mouth -- one per arrow up to kQuiverDrawnArrows. Render-only bodies
// (game/renderonly.h), pushed onto `out`; nothing the tick reads. Nothing for
// a creature with no quiver, no back socket, or a torso that is gone.
constexpr int kQuiverDrawnArrows = 12;
void PresentQuiver(const Mob& m, const ItemLibrary& items,
                   std::vector<RenderOnlyBody>& out);
