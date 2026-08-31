// selftest_playerkit.cpp — the player's kit: equipment, pack, spell bindings.
//
// CPU-ONLY AND CONTENT-INDEPENDENT, both on purpose.
//
// It touches no GPU, no world and no assets: it builds its own two-item
// library and its own three-glyph library, so it asserts on the RULES rather
// than on whatever happens to be in assets/items today. A gate that depended
// on the real content would start failing the day somebody renamed the sword,
// and that failure would say nothing about the code under test.
//
// It also runs in milliseconds, which is the property CLAUDE.md's "authoring
// cheap-to-verify work" section actually asks for: `--gate player-kit` alone
// is the whole verification loop for the equipment model, with no smoke pass
// and no reading numbers off stderr.
//
// WHAT IT PROTECTS, in order of how expensive the bug would be:
//
//   1. THE REFUSAL MATRIX. An equipment slot's accepted kinds are authored
//      data (game/equipment.h). This asserts both directions — what goes in
//      AND what is refused — because "it accepted the sword" is only half the
//      claim, and the half that will break when armour is added is the other.
//   2. SWAP, NOT OVERWRITE. Moving onto an occupied slot must never destroy an
//      item. This is the invariant that makes a mis-drop harmless.
//   3. BINDING BY OWNERSHIP. A glyph you do not own cannot be bound.
//   4. NAME RE-RESOLUTION ACROSS A LIBRARY REORDER. The one that would
//      otherwise be found by a player, months later, as "my sheathed sword
//      turned into a rock after I edited items.json".
//   5. 'PLYR' ROUND TRIP. Save, wipe, load, and compare BY NAME — the same
//      currency the format stores in, so the assertion cannot pass by
//      accidentally preserving indices the format is supposed to have thrown
//      away.

#include <cstdio>
#include <string>
#include <vector>

#include "game/caster.h"
#include "game/equipment.h"
#include "game/persist.h"
#include "game/spell.h"
#include "test/selftest.h"

namespace selftest {
namespace {

// A two-item library: one melee weapon (goes in a sheath) and one inert thing
// (goes nowhere). Two is the minimum that can express a refusal.
ItemLibrary MakeItems(bool swapped) {
  ItemDef blade;
  blade.name = "kit_blade";
  blade.kind = ItemKind::Melee;
  ItemDef rock;
  rock.name = "kit_rock";
  rock.kind = ItemKind::None;
  ItemLibrary lib;
  if (swapped) {
    lib.items.push_back(rock);
    lib.items.push_back(blade);
  } else {
    lib.items.push_back(blade);
    lib.items.push_back(rock);
  }
  return lib;
}

GlyphLibrary MakeGlyphs() {
  GlyphLibrary g;
  for (const char* id : {"kit_fire", "kit_bolt", "kit_trail"}) {
    GlyphDef d;
    d.id = id;
    g.glyphs.push_back(d);
  }
  return g;
}

int SheathSlot() {
  for (int i = 0; i < kEquipSlotCount; i++)
    if (EquipSlotAt(i).id == EquipSlotId::Sheath) return i;
  return -1;
}
int HeadSlot() {
  for (int i = 0; i < kEquipSlotCount; i++)
    if (EquipSlotAt(i).id == EquipSlotId::Head) return i;
  return -1;
}

Status GatePlayerKit(Ctx& c, std::string& detail) {
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("player-kit: FAILED %s\n", what);
    }
  };

  ItemLibrary items = MakeItems(false);
  const int blade = items.Find("kit_blade");
  const int rock = items.Find("kit_rock");
  const int sheath = SheathSlot(), head = HeadSlot();
  check(blade >= 0 && rock >= 0, "the fixture library resolves both items");
  check(sheath >= 0 && head >= 0, "the slot table has a sheath and a head");
  if (!ok) {
    detail = "fixture setup failed";
    return Status::Fail;
  }

  // ---- 1. the refusal matrix ----------------------------------------------
  {
    PlayerKit kit;
    Inventory hb;
    hb.slots[0] = {blade, 1};
    hb.slots[1] = {rock, 1};

    const KitRef fromBlade{KitSpace::Hotbar, 0};
    const KitRef fromRock{KitSpace::Hotbar, 1};
    const KitRef toSheath{KitSpace::Equip, sheath};
    const KitRef toHead{KitSpace::Equip, head};
    const KitRef toBag{KitSpace::Bag, 0};

    check(kit.Move(fromBlade, toSheath, hb, items) == MoveResult::Ok,
          "a melee weapon goes in the sheath");
    check(kit.equip.At(sheath).def == blade, "and it is actually there");
    check(hb.slots[0].Empty(), "and it left the hotbar slot it came from");

    // The armour slots accept NOTHING today, which is the scaffolding being
    // honest. When ItemKind::ArmorHead exists this assertion is what will
    // notice that the head slot's row changed.
    check(kit.Move(fromRock, toHead, hb, items) == MoveResult::WrongKind,
          "the head slot refuses a rock");
    check(kit.equip.At(head).Empty(), "and nothing landed in it");
    check(!hb.slots[1].Empty(), "and the rock stayed where it was");

    // A refusal must carry a REASON the panel can show. An empty string here
    // would be a slot that declines silently, which is the failure mode this
    // whole enum exists to prevent.
    const char* why = MoveResultText(MoveResult::WrongKind, toHead);
    check(why && *why, "a refused move explains itself");

    check(kit.Move(fromRock, toBag, hb, items) == MoveResult::Ok,
          "the pack takes anything");
    check(kit.Move(fromRock, toBag, hb, items) == MoveResult::Empty,
          "moving from a slot you already emptied is refused, not repeated");

    // ---- 2. swap, never overwrite ----
    hb.slots[0] = {rock, 3};
    const int before = kit.equip.At(sheath).def;
    check(kit.Move(KitRef{KitSpace::Equip, sheath}, fromBlade, hb, items) ==
              MoveResult::WrongKind,
          "a swap validates the RETURNING item against the slot it goes into");
    check(kit.equip.At(sheath).def == before,
          "and a refused swap changes nothing on either side");
    check(hb.slots[0].def == rock && hb.slots[0].count == 3,
          "including the count of the item that would have moved");

    // A legal swap: bag <-> sheath, both holding blades' worth of nothing
    // special, so the only thing under test is that both stacks survive.
    kit.bag.slots[5] = {rock, 2};
    kit.Move(KitRef{KitSpace::Bag, 5}, KitRef{KitSpace::Bag, 6}, hb, items);
    check(kit.bag.At(6).def == rock && kit.bag.At(6).count == 2,
          "a move carries the whole stack, count included");
    check(kit.bag.At(5).Empty(), "and empties the source");
  }

  // ---- 3. glyph binding ----------------------------------------------------
  GlyphLibrary glyphs = MakeGlyphs();
  {
    PlayerCaster caster;
    caster.inventory.Grant(glyphs.Find("kit_fire"));
    check(caster.inventory.Bind(0, glyphs.Find("kit_fire")),
          "an owned glyph binds");
    check(!caster.inventory.Bind(1, glyphs.Find("kit_bolt")),
          "an UNOWNED glyph is refused");
    check(caster.inventory.At(1) < 0, "and leaves the slot empty");
    check(caster.inventory.Bind(0, -1), "and a slot can be cleared");
    check(caster.inventory.At(0) < 0, "which really clears it");
    check(!caster.inventory.Bind(kGlyphSlots, glyphs.Find("kit_fire")),
          "a slot index past the end is refused rather than clamped");
  }

  // ---- 4. names survive a library reorder ---------------------------------
  // THE BUG THIS EXISTS FOR: every item slot stores an index into
  // ItemLibrary::items, and that order is the order of items.json. Editing
  // that file — even just moving an entry — renumbers everything. The
  // crossing has to go through names in BOTH directions.
  {
    PlayerKit kit;
    Inventory hb;
    kit.equip.slots[sheath] = {blade, 1};
    hb.slots[3] = {rock, 4};

    // Snapshot by name, "reload" a library with the entries swapped, restore.
    const std::string sheathName = KitItemName(kit.equip.At(sheath), items);
    const std::string hbName = KitItemName(hb.slots[3], items);
    const int hbCount = hb.slots[3].count;

    const ItemLibrary reordered = MakeItems(true);
    check(reordered.Find("kit_blade") != blade,
          "the fixture reorder actually MOVED the blade's index");

    kit.equip.slots[sheath] = KitItemFromName(sheathName, 1, reordered);
    hb.slots[3] = KitItemFromName(hbName, hbCount, reordered);
    check(KitItemName(kit.equip.At(sheath), reordered) == "kit_blade",
          "the sheathed weapon is still the weapon after a reorder");
    check(KitItemName(hb.slots[3], reordered) == "kit_rock" &&
              hb.slots[3].count == hbCount,
          "and so is the hotbar stack, count and all");

    // A name that is GONE drops the slot rather than resolving to a neighbour.
    ItemLibrary shrunk;
    shrunk.items.push_back(reordered.items[0]);
    const ItemStack lost = KitItemFromName("kit_blade", 1, shrunk);
    check(lost.Empty(),
          "an item removed from the library empties its slot instead of "
          "pointing at whatever took its index");
  }

  // ---- 5. the 'PLYR' section round-trips ----------------------------------
  // Driven through MakeEntityIO rather than through the serializer directly,
  // so the REGISTRATION is under test too: a section that is written but never
  // registered persists nothing, which is exactly the audit finding
  // game/persist.h exists to close.
  {
    PlayerCaster caster;
    Inventory hb;
    PlayerKit kit;
    for (int i = 0; i < (int)glyphs.glyphs.size(); i++)
      caster.inventory.Grant(i);
    caster.inventory.Bind(0, glyphs.Find("kit_trail"));
    caster.inventory.Bind(4, glyphs.Find("kit_fire"));
    hb.slots[2] = {blade, 1};
    hb.slots[7] = {rock, 9};
    hb.Select(7);
    kit.bag.slots[0] = {rock, 2};
    kit.bag.slots[Bag::kSlots - 1] = {blade, 1};
    kit.equip.slots[sheath] = {blade, 1};
    // v2: WORN DAMAGE. The owner's decision is that armour damage persists
    // EXACTLY, so this is a lattice and not a percentage — and a lattice is
    // the one thing a format bump has to be able to carry without silently
    // truncating. Two shells, one holed and one intact, because "the empty
    // entries survive too" is the half that a save writing only the damaged
    // shells would get wrong by re-indexing them.
    {
      WornDamage d;
      d.shells.resize(2);
      d.shells[0].hp = 4.5f;
      d.shells[0].lattice.push_back(PrefabVoxel{1, 2, 3, 48, 200});
      d.shells[0].lattice.push_back(PrefabVoxel{4, 5, 6, 49, 0});
      kit.SetDamage("kit_blade", std::move(d));
    }

    PlayerKitRefs refs{&caster, &glyphs, &hb, &kit, &items};
    EntityIO io = MakeEntityIO(c.debris, c.mobs, nullptr, &refs);
    const EntitySection* plyr = nullptr;
    for (const EntitySection& s : io.sections)
      if (s.id == (uint32_t)('P' | ('L' << 8) | ('Y' << 16) | ((uint32_t)'R' << 24)))
        plyr = &s;
    check(plyr != nullptr, "MakeEntityIO registers a PLYR section");
    if (plyr) {
      std::vector<uint8_t> bytes;
      plyr->save(bytes);
      check(!bytes.empty(), "and it writes something");

      // Wipe through the section's OWN reset — the same call a load performs,
      // so a reset that forgot a container fails here rather than in a save
      // that quietly kept last session's pack.
      plyr->reset();
      check(hb.slots[2].Empty() && kit.bag.At(0).Empty() &&
                kit.equip.At(sheath).Empty() &&
                caster.inventory.owned.empty(),
            "reset clears the hotbar, the pack, the equipment and the glyphs");
      check(kit.wornDamage.empty(), "and the worn damage with them");

      check(plyr->load(bytes.data(), bytes.size(), kPlayerKitSaveVersion),
            "and the payload loads back");
      // Compared BY NAME, which is the currency the format stores in: an
      // index-to-index comparison could pass on a format that never converted.
      check(KitItemName(hb.slots[2], items) == "kit_blade",
            "the hotbar came back");
      check(KitItemName(hb.slots[7], items) == "kit_rock" &&
                hb.slots[7].count == 9,
            "with its counts");
      check(hb.selected == 7, "and the selected slot");
      check(KitItemName(kit.bag.At(0), items) == "kit_rock" &&
                KitItemName(kit.bag.At(Bag::kSlots - 1), items) == "kit_blade",
            "the pack came back, including its last slot");
      check(KitItemName(kit.equip.At(sheath), items) == "kit_blade",
            "and the sheathed weapon");
      check((int)caster.inventory.owned.size() == (int)glyphs.glyphs.size(),
            "every known glyph came back");
      check(caster.inventory.At(0) == glyphs.Find("kit_trail") &&
                caster.inventory.At(4) == glyphs.Find("kit_fire"),
            "and the bindings landed on the right keys");
      check(caster.inventory.At(1) < 0, "leaving the unbound keys unbound");

      // v2: the holes came back where they were. Compared cell for cell,
      // because "the right number of voxels" is what a format that dropped
      // the coordinates would also report.
      const WornDamage* d = kit.Damage("kit_blade");
      check(d != nullptr, "worn damage came back");
      if (d) {
        check(d->shells.size() == 2,
              "including the intact shell, so the per-shell indices still line "
              "up with the item's cover order");
        check(d->shells[0].hp == 4.5f, "and the hp that was left");
        check(d->shells[0].lattice.size() == 2, "and both damaged cells");
        if (d->shells[0].lattice.size() == 2) {
          const PrefabVoxel& v0 = d->shells[0].lattice[0];
          const PrefabVoxel& v1 = d->shells[0].lattice[1];
          check(v0.x == 1 && v0.y == 2 && v0.z == 3 && v0.material == 48 &&
                    v0.color == 200,
                "cell for cell, material and art colour included");
          check(v1.x == 4 && v1.y == 5 && v1.z == 6 && v1.material == 49,
                "and so does the second");
        }
        check(d->shells[1].Empty(),
              "an undamaged shell round-trips as empty rather than as a "
              "full copy of itself");
      }

      // A truncated payload must be REFUSED, not half-applied. Saves get
      // truncated by full disks and interrupted writes, and a half-applied
      // kit is worse than a refused one.
      const bool refusedShort =
          !plyr->load(bytes.data(), bytes.size() / 2, kPlayerKitSaveVersion);
      check(refusedShort, "a truncated payload is refused");
      const bool refusedVersion =
          !plyr->load(bytes.data(), bytes.size(), kPlayerKitSaveVersion + 1);
      check(refusedVersion, "an unknown version is refused");
    }
  }

  detail = Format("%d checks", checks);
  std::printf("player-kit: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& PlayerKitGates() {
  static const std::vector<Gate> g = {
      // No deps: it builds its own fixtures and never touches the world, so it
      // neither needs nor disturbs any other gate's state.
      {"player-kit", "player", {}, false, GatePlayerKit},
  };
  return g;
}

}  // namespace selftest
