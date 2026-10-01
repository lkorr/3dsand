#pragma once
// ---- AN ITEM'S OWN VOXELS, AND WHAT THEY WEAR (DESIGN.md §7, "A coat moves
//      on contact"; docs/PLAN_weapon_coats.md A4) ------------------------------
//
// ONE CONCEPT: the item's exact voxel lattice, as an object carried by its
// ItemInstance. A coat lives where it lives on a body -- PrefabVoxel::stain,
// the 16-bit coat word (material | amount << 12) -- so everything that reads,
// writes, washes or dries a body coat speaks it already.
//
// WHERE IT IS STORED. In the instance's `damage.shells[i].lattice`
// (game/iteminstance.h WornShellDamage), the record worn armour already
// carried its holes in: one shell for a held item, one per cover entry for a
// worn piece. So it rides every path that record rides -- the kit, a pickup
// and a drop, the corpse's loot, the net and the MOBS/PLYR/ITMS saves -- with
// no new field. An EMPTY lattice means "as authored, clean"; that is what
// keeps a pristine sword a plain stack that merges with its twins.
//
// WHO IS AUTHORITATIVE WHEN. While the item is in a hand or on the body the
// RIG SLOT is the truth (it is what a blow carves and a coat lands on), and
// the instance is what it was when it went on; CaptureLimbToItem brings the
// instance up to date (Mob::KitFlushWorn does it at every move, take and
// save already). While the item is in the bag the instance is the only copy
// and nothing ticks it: a coat on a stored item is FROZEN (owner decision,
// 2026-10-01) -- it dries only while held or worn, at the material's own
// coat.decay, through the same passes as any limb.
//
// THE WRITE PATH (the item stage, package C): mutate ItemLatticeMut, then
// PushItemLatticeToLimb so a held or worn item shows it at once. As an INPUT
// carried into the tick, never from the UI directly (multiplayer authority).
//
// COORDINATES are the lattice's own cells, min corner 0, at the item's own
// scale (ItemDef::scale micro cells per world voxel). For a held item the
// material word may carry the palette variant in bits 12..15 (Mob::EquipItem
// adds it); mask with 0xFFF for the material id.

#include <vector>

#include "game/item.h"
#include "game/iteminstance.h"
#include "game/kitref.h"
#include "sim/voxload.h"

class Mob;
class MobSystem;

// How many lattices the item has: 1 for a held item, its cover count for a
// worn piece, 0 for an item with no voxels.
int ItemLatticeCount(const ItemDef& def);

// The authored lattice (as the library loaded it) and its box, for a viewer.
// `shell` indexes ItemLatticeCount. A worn piece's AUTHORED shell is the
// cover as drawn, before any wearer's fit resample. Null / false when absent.
const std::vector<PrefabVoxel>* ItemAuthoredLattice(const ItemDef& def,
                                                    int shell = 0);
bool ItemLatticeDims(const ItemDef& def, int& sx, int& sy, int& sz,
                     int shell = 0);

// The instance's exact lattice, MATERIALISED AS AUTHORED on first use (clean,
// complete, the held-item palette variant applied as Mob::EquipItem applies
// it, so a materialised lattice is byte-identical to what CaptureHeld would
// record of the same untouched item). For a worn piece that was captured off
// a body this is the wearer's FITTED lattice; one never worn is the authored
// cover. Returns a static empty vector for a bad shell index.
std::vector<PrefabVoxel>& ItemLatticeMut(ItemInstance& it, const ItemDef& def,
                                         int shell = 0);
// ...without materialising: the recorded lattice, or null for "as authored".
const std::vector<PrefabVoxel>* ItemLatticeIfAny(const ItemInstance& it,
                                                 int shell = 0);

// Drop every recorded lattice that is clean and complete (no voxel wears a
// coat, none is missing, hp untouched), so a blade washed spotless on the
// stage is a plain stack again. Returns true when the instance became plain.
bool ItemLatticeSettle(ItemInstance& it, const ItemDef& def);

// Push the kit stack at `ref`'s lattice onto the live rig slot(s) if that
// stack is what `wearer` is holding (a hand slot) or wearing (a worn slot):
// COATS ONLY, voxel for voxel where positions coincide, bricks poked (a
// shell with no recorded lattice is washed clean to match). Returns the
// voxels whose coat changed, or -1 when the item is not on the rig (in the
// bag, the hotbar, a borrowed bench prop) -- nothing to push to.
int PushItemLatticeToLimb(Mob& wearer, const KitRef& ref);
int PushItemLatticeToLimb(MobSystem& mobs, uint64_t mobId, const KitRef& ref);

// The reverse: the live rig slot(s) -> the kit stack at `ref` (the held
// item's or the worn piece's exact lattice, coats included; Mob::KitFlushWorn).
// False when `ref` is not an equipment slot whose item is on the rig.
bool CaptureLimbToItem(Mob& wearer, const KitRef& ref);
bool CaptureLimbToItem(MobSystem& mobs, uint64_t mobId, const KitRef& ref);
