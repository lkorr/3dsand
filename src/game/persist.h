#pragma once

#include "game/avatar.h"
#include "game/caster.h"
#include "game/equipment.h"
#include "game/worlditems.h"
#include "game/mob.h"
#include "phys/debris.h"
#include "sim/worldio.h"

// The ONE place the entity systems register into the save format
// (sim/worldio.h entities.sve). Both the frame loop and the selftest build
// their EntityIO here, so a system added in one place is persistable in both —
// and a system that is NOT registered here is structurally unable to persist,
// which is exactly the audit finding this closes.
//
// Extension path: give the new system SaveState/LoadState (its bytes are its
// own business), pick a fresh FourCC, append a section here. The container
// never changes; older builds skip the unknown section.
//
// `avatar` is nullable (headless paths without a player body): its section is
// simply absent, and a save without it loads the avatar fresh.
//
// LIFETIME: the returned sections capture the systems by reference; the
// EntityIO must not outlive them.

// THE PLAYER'S KIT ('PLYR'): what they are carrying, wearing and have bound.
//
// It is a BUNDLE OF REFERENCES rather than a system with its own SaveState,
// because unlike debris/mobs/avatar there is no object that owns all of it:
// the hotbar predates the pack, the caster is deliberately separate from the
// player, and both libraries are needed to turn indices into names. Rather
// than invent a container just to have something to call SaveState on, the
// section is built from the pieces main.cpp already holds.
//
// EVERYTHING IS STORED BY NAME. Item and glyph slots hold indices into
// ItemLibrary::items / GlyphLibrary::glyphs, both of which are FILE-ORDER
// dependent and change whenever content is edited. A save that stored indices
// would silently hand back a different sword — or a different spell — after
// any edit to items.json or glyphs.json. Names that no longer resolve drop the
// slot with a log line, because content legitimately disappears between saves
// and that is the contract, not a failure.
//
// Nullable: a headless path with no player writes no section, and a save
// without one loads a fresh kit.
struct PlayerKitRefs {
  PlayerCaster* caster = nullptr;
  const GlyphLibrary* glyphs = nullptr;
  Inventory* hotbar = nullptr;
  PlayerKit* kit = nullptr;
  const ItemLibrary* items = nullptr;

  bool Complete() const {
    return caster && glyphs && hotbar && kit && items;
  }
};

// Version 2 of the 'PLYR' payload. v1 carried the containers; v2 appends the
// WORN DAMAGE map (game/equipment.h), so a burnt robe comes back burnt.
//
// The bump is real rather than an append-and-hope: v1 payloads are REFUSED,
// not partially applied. A half-loaded kit is worse than a refused one, the
// existing round-trip test asserts exactly that refusal, and a save format
// that silently accepts a shorter payload is how a truncated file turns into
// a player's inventory quietly emptying.
// Version 3 adds the CONDITION SUMMARY to each shell record (voxels at spawn,
// voxels still live). Derivable from the lattice only while the piece is on a
// body, which is exactly when it is not in this file — see WornShellDamage.
constexpr uint32_t kPlayerKitSaveVersion = 3;

// ITEMS ON THE GROUND ('ITMS'): what is lying around, by name and pose.
//
// A SEPARATE SECTION FROM 'DBRS', even though every ground item IS a debris
// body. Debris saves SHAPES; this saves IDENTITY, and the two have different
// lifetimes — the body may be destroyed and rebuilt by a load, but "that was a
// sword" has to survive. Storing the name in the debris record instead would
// put an item concept inside the physics layer, which is the coupling
// game/worlditems.h exists to avoid.
//
// The bodies themselves are RE-CREATED on load from the item library, not
// restored from DBRS: a ground item is fully described by its name and its
// transform, so re-dropping it is both simpler and immune to a DBRS format
// change. The cost is that a ground item's carved lattice does not survive a
// save — see the note in worlditems.h; it round-trips as authored.
struct WorldItemRefs {
  WorldItems* reg = nullptr;
  Physics* phys = nullptr;
  DebrisSystem* debris = nullptr;
  MicroBodySet* micro = nullptr;
  const ItemLibrary* items = nullptr;

  bool Complete() const { return reg && phys && debris && items; }
};

constexpr uint32_t kWorldItemSaveVersion = 1;

EntityIO MakeEntityIO(DebrisSystem& debris, MobSystem& mobs,
                      PlayerAvatar* avatar,
                      const PlayerKitRefs* player = nullptr,
                      const WorldItemRefs* ground = nullptr);
