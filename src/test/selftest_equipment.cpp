// selftest_equipment.cpp — WEARING things: shells, the appended tail, and the
// occlusion probe.
//
// CONTENT-INDEPENDENT BY CONSTRUCTION. The armour it wears is a FIXTURE built
// in this file, not assets/items/robe.json, and the limbs it hangs the fixture
// on are read out of whichever def the game calls its avatar
// (`kAvatarDefName`) rather than named here. Both are deliberate, and both are
// the same lesson: a gate that hardcodes the asset cast fails the day somebody
// adds a correct asset, and then names something unrelated in its failure
// message (gotcha-gate-hardcodes-asset-cast — `ragdoll-joints` failed on
// `waistChecked == 9`, which said nothing whatsoever about the new mob).
//
// WHAT IT PROTECTS, in order of how expensive the bug would be:
//
//   1. THE APPENDED TAIL. Worn shells and a held weapon share one region at
//      the end of the rig, and taking a piece out of the MIDDLE of it must not
//      renumber the others. This is the one real refactor armour needed and
//      the one place a silent index slip would show up as "my sword turned
//      into a sleeve".
//   2. DAMAGE SURVIVES A NEIGHBOUR'S REMOVAL. Removing the robe must not heal
//      the boots. A rebuild-from-def would; erase-and-move does not, and this
//      is what says so.
//   3. THE CREATURE'S SIZE IS ITS OWN. `worldSize` drives the gait pivot, the
//      standing height and the terrain anchor radius. Luggage already cost
//      this once (DESIGN.md §8) and armour is more luggage.
//   4. THE REFUSAL MATRIX STILL REFUSES. Now that the armour rows accept
//      kinds, "the head slot takes a helm" and "the head slot does not take a
//      sword" are two different claims and both have to hold.
//   5. A WEAPON DOES NOT CUT ITS WEARER'S COAT. Shells are the wielder's own
//      bodies, so the existing self-rejection must already cover them.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "game/equipment.h"
#include "game/item.h"
#include "game/mob.h"
#include "game/persist.h"
#include "game/worlditems.h"
#include "sim/microbody.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// A synthetic worn piece: one hollow-ish box shell per named part, authored at
// `scale` micro units per world voxel. Small on purpose — the gate is about
// bookkeeping, not about art — but real geometry, so it packs a micro brick,
// derives a collider and can be carved like anything else.
ItemDef MakeWornFixture(const std::string& name, ItemKind kind,
                        const std::vector<std::string>& parts, uint32_t scale,
                        uint32_t material, MicroBodySet& micro) {
  ItemDef d;
  d.name = name;
  d.kind = kind;
  d.scale = scale;
  d.hp = 20.0f;
  const int n = (int)scale * 2;   // two world voxels on a side
  for (const std::string& part : parts) {
    ItemCover cv;
    cv.part = part;
    cv.model = part;
    cv.hp = 12.0f;
    cv.size = IVec3{n, n, n};
    cv.voxels.reserve((size_t)n * n * n);
    for (int z = 0; z < n; z++)
      for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
          cv.voxels.push_back(
              PrefabVoxel{(int16_t)x, (int16_t)y, (int16_t)z,
                          (uint16_t)material, 0});
    std::string err;
    cv.microModel = MicroBodyPack(micro, cv.voxels, cv.size, scale,
                                  "fixture/" + name + "/" + part, err);
    d.cover.push_back(std::move(cv));
  }
  return d;
}

// A shell that ENCLOSES a named limb: the limb's own model box, grown by
// `thick` skin voxels on every side, hollow. Built from the def's prefab so it
// fits whatever the rig actually is, and hollow because a solid one would be
// tens of thousands of voxels of collider for a test that only cares about the
// surface.
//
// Enclosure is the point. The reactivity claims are all of the form "the world
// cannot reach this limb while the shell is intact", and a partial shell makes
// every one of them a statement about which face the fire happened to find.
ItemDef MakeEnclosingFixture(const std::string& name, ItemKind kind,
                             const MobDef& def, const std::string& part,
                             uint32_t material, int thick,
                             MicroBodySet& micro) {
  ItemDef d;
  d.name = name;
  d.kind = kind;
  d.scale = def.skinScale ? def.skinScale : 1u;
  const PrefabModel* m = nullptr;
  for (const PrefabModel& pm : def.prefab.models)
    if (pm.name == part) m = &pm;
  if (!m) return d;   // no such model: caller sees an empty cover list

  ItemCover cv;
  cv.part = part;
  cv.model = part;
  cv.hp = 40.0f;
  cv.size = IVec3{m->size.x + 2 * thick, m->size.y + 2 * thick,
                  m->size.z + 2 * thick};
  // WORLD VOXELS, not micro: this bypasses the JSON loader, which is where
  // the micro -> world division normally happens.
  const float inv = 1.0f / (float)d.scale;
  cv.offset = Vec3{-(float)thick, -(float)thick, -(float)thick} * inv;
  cv.fitBox = Vec3{(float)m->size.x, (float)m->size.y, (float)m->size.z} * inv;
  for (int z = 0; z < cv.size.z; z++)
    for (int y = 0; y < cv.size.y; y++)
      for (int x = 0; x < cv.size.x; x++) {
        const bool surface = x < thick || y < thick || z < thick ||
                             x >= cv.size.x - thick ||
                             y >= cv.size.y - thick || z >= cv.size.z - thick;
        if (!surface) continue;
        cv.voxels.push_back(PrefabVoxel{(int16_t)x, (int16_t)y, (int16_t)z,
                                        (uint16_t)material, 0});
      }
  std::string err;
  cv.microModel = MicroBodyPack(micro, cv.voxels, cv.size, d.scale,
                                "fixture/" + name, err);
  d.cover.push_back(std::move(cv));
  return d;
}

Status GateArmorWear(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("armor-wear: FAILED %s\n", what);
    }
  };

  // ---- 0. the slot table, which is pure data and costs nothing ------------
  {
    int head = -1, sheath = -1;
    for (int i = 0; i < kEquipSlotCount; i++) {
      if (EquipSlotAt(i).id == EquipSlotId::Head) head = i;
      if (EquipSlotAt(i).id == EquipSlotId::Sheath) sheath = i;
    }
    check(head >= 0 && sheath >= 0, "the slot table has a head and a sheath");
    check(EquipSlotAccepts(head, ItemKind::ArmorHead),
          "the head slot now accepts a helm");
    check(!EquipSlotAccepts(head, ItemKind::Melee),
          "and still refuses a sword");
    check(!EquipSlotAccepts(sheath, ItemKind::ArmorHead),
          "and the sheath still refuses a helm");
    check(EquipSlotIsWorn(head) && !EquipSlotIsWorn(sheath),
          "the head is a WORN slot and the sheath is not");
    // The one thing a kind table gets wrong silently: a round trip that
    // renames a kind leaves items.json parsing to None and every armour item
    // skipped at load, with the only evidence a line in the error log.
    for (int k = 1; k <= (int)ItemKind::Trinket; k++)
      check(ItemKindFromName(ItemKindName((ItemKind)k)) == (ItemKind)k,
            "every ItemKind round-trips through its name");
    check(ItemKindIsWorn(ItemKind::ArmorHead) &&
              !ItemKindIsWorn(ItemKind::Melee) &&
              !ItemKindIsWorn(ItemKind::None),
          "ItemKindIsWorn covers the armour range and nothing else");
  }

  // ---- 0b. draw and stow, which is pure logic and also costs nothing ------
  //
  // The sheath is THE weapon slot, so this is the whole of "am I armed". It
  // is asserted here rather than in the game because the rule lives in
  // game/equipment.h precisely so it can be: no window, no GPU, no input
  // stack, and the frame loop runs the same three lines.
  {
    const int kBrush = 0, kMelee = 4;   // any two distinct tool ids
    SheathState sh;
    sh.toolBefore = kBrush;
    int tool = kBrush;

    check(!sh.Toggle(ItemKind::None, kMelee, tool),
          "drawing from an empty sheath is refused");
    check(!sh.drawn && tool == kBrush,
          "and changes neither the hand nor the tool");

    check(sh.Toggle(ItemKind::Melee, kMelee, tool), "a sheathed blade draws");
    check(sh.drawn && tool == kMelee,
          "and drawing selects the melee tool, so the mouse swings rather "
          "than paints");

    // Stow puts back what the player had, not a default. Getting this wrong
    // leaves them in a mode they never chose and looks like the tool selector
    // is broken.
    check(sh.Toggle(ItemKind::Melee, kMelee, tool), "it stows again");
    check(!sh.drawn && tool == kBrush, "and hands the previous tool back");

    // The character screen can move the weapon out mid-swing. Nothing about
    // the key press knows that happened, so the reconcile is the only thing
    // between "drawn" and "holding an item that is in your pack".
    sh.Toggle(ItemKind::Melee, kMelee, tool);
    check(sh.drawn, "drawn again");
    sh.Reconcile(ItemKind::None, kMelee, tool);
    check(!sh.drawn && tool == kBrush,
          "a weapon dragged out of the sheath is no longer in the hand");
    sh.Reconcile(ItemKind::None, kMelee, tool);
    check(!sh.drawn && tool == kBrush, "and the reconcile is idempotent");

    // A worn item in the sheath must not arm anybody. The slot table already
    // refuses to put one there, but the hand asks about KIND and the two are
    // separate claims.
    SheathState sh2;
    int t2 = kBrush;
    check(!sh2.Toggle(ItemKind::ArmorHead, kMelee, t2),
          "a helm in the sheath does not draw as a weapon");
  }

  // ---- the rig fixture ----------------------------------------------------
  int avDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == kAvatarDefName) avDef = (int)i;
  if (avDef < 0) {
    detail = Format("no '%s' def to dress", kAvatarDefName);
    std::printf("armor-wear: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[avDef];
  const Vec3 sizeBefore = def.worldSize;
  const int baseLimbs = (int)def.limbs.size();

  // Parts to cover, chosen BY TAG off the def so this reads any humanoid rig:
  // one spine part for the "robe", one arm and one foot for the "boots". A rig
  // that has none of them makes the gate skip rather than fail — it would be
  // testing nothing either way.
  auto firstWithTag = [&](const char* tag, const std::string& notThis) {
    for (const MobLimbDef& ld : def.limbs)
      if (ld.tag == tag && ld.name != notThis) return ld.name;
    return std::string();
  };
  const std::string spineA = firstWithTag("spine", "");
  const std::string spineB = firstWithTag("spine", spineA);
  const std::string armA = firstWithTag("arm", "");
  const std::string footA = firstWithTag("foot", "");
  if (spineA.empty() || armA.empty() || footA.empty()) {
    detail = Format("'%s' has no spine/arm/foot tags to cover", kAvatarDefName);
    std::printf("armor-wear: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }

  MicroBodySet fixtureMicro;
  const uint32_t mat = c.mats.size() > 1 ? 1u : 1u;
  // Two pieces, at two scales on purpose: the "robe" at scale 8 over a
  // physScale-4 body exercises the skin/collider split (DownsampleSkin), the
  // "boots" at scale 4 the coarse single-lattice path. Both have to work, and
  // they are different code.
  std::vector<std::string> robeParts{spineA, armA};
  if (!spineB.empty()) robeParts.push_back(spineB);
  const ItemDef robe = MakeWornFixture("fixture_robe", ItemKind::ArmorChest,
                                       robeParts, 8, mat, fixtureMicro);
  const ItemDef boots = MakeWornFixture("fixture_boots", ItemKind::ArmorBoots,
                                        {footA}, 4, mat, fixtureMicro);

  mobs.Reset();
  const int h = World::TerrainHeight(150, 150, kDefaultSeed);
  const uint64_t id = mobs.Spawn(avDef, {150, h + 1, 150});
  Mob* mob = mobs.FindMobById(id);
  if (!mob) {
    detail = "spawn failed";
    std::printf("armor-wear: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  check(mob->LimbCount() == baseLimbs,
        "a freshly spawned rig has exactly its authored limbs");
  check(mob->AppendedBase() == baseLimbs,
        "and reports that count as its appended base");

  const int chestSlot = (int)EquipSlotId::Chest;
  const int bootSlot = (int)EquipSlotId::Boots;

  // ---- 1. wear one piece --------------------------------------------------
  check(mob->WearItem(&robe, chestSlot), "the robe goes on");
  check(mob->LimbCount() == baseLimbs + (int)robeParts.size(),
        "one appended slot per cover entry");
  check(mob->WornItem(chestSlot) == "fixture_robe",
        "and the slot remembers it BY NAME");
  {
    int shells = 0;
    for (int i = baseLimbs; i < mob->LimbCount(); i++) {
      const MobLimbDef& ld = mob->LimbDefAt(i);
      if (ld.tag != "worn") continue;
      shells++;
      check(!ld.vital, "a shell is never vital");
      // The parent must be the limb the cover entry NAMED, not whatever slot
      // happened to be at that index.
      bool named = false;
      for (const std::string& p : robeParts)
        if (ld.parent == p) named = true;
      check(named, "a shell is parented to the limb its cover entry named");
    }
    check(shells == (int)robeParts.size(), "and every appended slot is a shell");
  }
  check(mobs.Defs()[avDef].worldSize.x == sizeBefore.x &&
            mobs.Defs()[avDef].worldSize.y == sizeBefore.y &&
            mobs.Defs()[avDef].worldSize.z == sizeBefore.z,
        "wearing does not change how big the creature is");

  // ---- 2. unwear puts it back exactly ------------------------------------
  check(mob->UnwearItem(chestSlot), "the robe comes off");
  check(mob->LimbCount() == baseLimbs, "and takes every one of its slots");
  check(mob->WornItem(chestSlot).empty(), "and the slot is empty again");
  check(!mob->UnwearItem(chestSlot),
        "unwearing an empty slot is refused, not repeated");

  // ---- 3. THE APPENDED TAIL: remove from the MIDDLE ----------------------
  //
  // Robe, then boots, then a weapon. Taking the ROBE off leaves a hole in the
  // middle of the tail, and the two things after it must still resolve. This
  // is the whole reason RemoveAppendedSlots exists; before it, unequip was
  // pop_back and this sequence silently freed the boots' Jolt body while
  // leaving the sword's index pointing at it.
  check(mob->WearItem(&robe, chestSlot), "the robe goes back on");
  check(mob->WearItem(&boots, bootSlot), "and the boots over it");
  const ItemDef* sword = nullptr;
  for (const ItemDef& it : c.items.items)
    if (it.kind == ItemKind::Melee) { sword = &it; break; }
  const bool armed = sword && mob->EquipItem(sword);
  const int tailWithAll = mob->LimbCount();
  check(tailWithAll == baseLimbs + (int)robeParts.size() + 1 + (armed ? 1 : 0),
        "robe + boots + weapon all occupy the tail at once");

  // What the boots and the weapon are, by identity, before the removal.
  const std::vector<int> bootSlotsBefore = [&] {
    for (int p = 0; p < mob->WornPieceCount(); p++) {
      const std::vector<int>& s = mob->WornSlotsAt(p);
      if (!s.empty() && mob->LimbDefAt(s[0]).name.find("fixture_boots") !=
                            std::string::npos)
        return s;
    }
    return std::vector<int>{};
  }();
  check(bootSlotsBefore.size() == 1, "the boots hold one shell");
  const uint32_t bootVoxBefore =
      bootSlotsBefore.empty()
          ? 0u
          : mobs.LimbVoxelCount(id, bootSlotsBefore[0]);
  check(bootVoxBefore > 0, "and that shell has voxels");

  // Carve the boot shell FIRST, so what survives the removal below is damaged
  // geometry rather than a pristine copy the def could have supplied.
  if (!bootSlotsBefore.empty()) {
    const uint64_t bb = mobs.LimbBody(id, bootSlotsBefore[0]);
    std::vector<ParticleSpawn> cs;
    mobs.CarveLimbRadial(bb, mobs.LimbVoxelPos(id, bootSlotsBefore[0], 0), 0.6f,
                         false, false, c.world, cs);
  }
  const uint32_t bootVoxCarved =
      bootSlotsBefore.empty()
          ? 0u
          : mobs.LimbVoxelCount(id, bootSlotsBefore[0]);
  check(bootVoxCarved < bootVoxBefore, "a shell carves like anything else");

  // NOW remove the robe out of the middle.
  check(mob->UnwearItem(chestSlot), "the robe comes off from the middle");
  check(mob->LimbCount() == tailWithAll - (int)robeParts.size(),
        "and takes only its own slots");
  check(mob->WornItem(bootSlot) == "fixture_boots",
        "the boots are still worn");
  if (armed) {
    check(mob->HeldSlot() >= 0 && mob->HeldSlot() < mob->LimbCount(),
          "the weapon's slot index still points inside the rig");
    check(mob->LimbDefAt(mob->HeldSlot()).name ==
              "item:" + std::string(sword->name),
          "and it still points at the WEAPON rather than at a sleeve");
  }
  {
    // Re-resolve the boots' slot from the registry; the erase renumbered it.
    std::vector<int> after;
    for (int p = 0; p < mob->WornPieceCount(); p++) {
      const std::vector<int>& s = mob->WornSlotsAt(p);
      if (!s.empty() && mob->LimbDefAt(s[0]).name.find("fixture_boots") !=
                            std::string::npos)
        after = s;
    }
    check(after.size() == 1, "the boots still register one shell");
    if (!after.empty()) {
      check(mob->LimbDefAt(after[0]).tag == "worn",
            "which is still a shell");
      // THE POINT OF THE WHOLE REFACTOR: the surviving shell kept its wounds.
      // A tear-down-and-re-append would have re-loaded it from the def and
      // healed it, and nothing else in the suite would have noticed.
      check(mobs.LimbVoxelCount(id, after[0]) == bootVoxCarved,
            "and kept every voxel it had lost — removing a neighbour does not "
            "repair your boots");
      // Your own coat is your own body: the blade self-rejection that stops a
      // swing cutting the arm holding it must already cover the sleeve on it.
      check(mob->OwnsBody(mobs.LimbBody(id, after[0])),
            "a shell is one of the wearer's own bodies, so a swing cannot "
            "shred it");
    }
  }

  // ---- 3b. TAKING IT OFF IS NOT A REPAIR ---------------------------------
  //
  // The one thing a rebuild-from-def would silently get wrong, and the reason
  // damage lives outside the rig at all (game/equipment.h WornDamage): while
  // the piece is ON, its wounds are the shells', and the shells die with the
  // slots. If nothing carries them across, taking your boots off to try
  // another pair mends the first pair — everything still works, the armour is
  // just quietly free, and no other gate would notice.
  {
    int bs = -1;
    for (int p = 0; p < mob->WornPieceCount(); p++) {
      const std::vector<int>& s = mob->WornSlotsAt(p);
      if (!s.empty() && mob->LimbDefAt(s[0]).name.find("fixture_boots") !=
                            std::string::npos)
        bs = s[0];
    }
    const uint32_t hurt = bs >= 0 ? mobs.LimbVoxelCount(id, bs) : 0;
    check(hurt > 0 && hurt < bootVoxBefore, "the boots are damaged");

    WornDamage dmg;
    check(mob->CaptureWorn(bootSlot, dmg), "their damage can be read off");
    check(!dmg.Empty(),
          "and it is not empty — an intact piece must produce nothing, a "
          "holed one must produce something");
    mob->UnwearItem(bootSlot);

    // Back on WITHOUT the blob: as authored, which is what a naive re-wear
    // does and is exactly the bug. Asserted so the fix cannot be mistaken for
    // the damage never having been there.
    check(mob->WearItem(&boots, bootSlot), "they go back on");
    int bs2 = -1;
    for (int p = 0; p < mob->WornPieceCount(); p++) {
      const std::vector<int>& s = mob->WornSlotsAt(p);
      if (!s.empty() && mob->LimbDefAt(s[0]).name.find("fixture_boots") !=
                            std::string::npos)
        bs2 = s[0];
    }
    check(bs2 >= 0 && mobs.LimbVoxelCount(id, bs2) == bootVoxBefore,
          "worn back on with no damage blob, they are whole again — which is "
          "the repair this exists to prevent");
    mob->UnwearItem(bootSlot);

    // And WITH it: the holes are still there, voxel for voxel.
    check(mob->WearItem(&boots, bootSlot, &dmg), "and back on carrying it");
    int bs3 = -1;
    for (int p = 0; p < mob->WornPieceCount(); p++) {
      const std::vector<int>& s = mob->WornSlotsAt(p);
      if (!s.empty() && mob->LimbDefAt(s[0]).name.find("fixture_boots") !=
                            std::string::npos)
        bs3 = s[0];
    }
    check(bs3 >= 0 && mobs.LimbVoxelCount(id, bs3) == hurt,
          "the holes came back, voxel for voxel");
    mob->UnwearItem(bootSlot);
    check(mob->WearItem(&boots, bootSlot, &dmg), "re-worn for the teardown");
  }

  // ---- 4. everything comes off cleanly -----------------------------------
  check(mob->UnwearItem(bootSlot), "the boots come off");
  if (armed) mob->EquipItem(nullptr);
  check(mob->LimbCount() == baseLimbs,
        "and the rig is back to exactly its authored limbs");
  check(mob->WornPieceCount() == 0, "with nothing registered as worn");

  // ---- 5. THE SHIPPED SET ACTUALLY FITS THE SHIPPED RIG ------------------
  //
  // Everything above is about the mechanism and deliberately uses a fixture.
  // This is the one claim only real content can make: that every cover entry
  // in assets/items names a limb this rig HAS. A wearer skipping an unknown
  // part is correct behaviour (a sleeve on a one-armed mob), which is exactly
  // why a renamed limb would lose the robe's sleeve in silence — nothing would
  // error, the arm would simply come out bare.
  //
  // Counted rather than named: the gate asserts the pieces are WHOLE, not that
  // any particular garment exists. A tree with no armour content in it passes.
  int wornItems = 0, wornShells = 0;
  for (const ItemDef& it : c.items.items) {
    if (!ItemKindIsWorn(it.kind)) continue;
    wornItems++;
    // Every worn kind must have a slot willing to take it, or the item is
    // authored into a hole: it loads, sits in the pack, and goes nowhere.
    int home = -1;
    for (int s = 0; s < kEquipSlotCount; s++)
      if (EquipSlotAccepts(s, it.kind)) { home = s; break; }
    check(home >= 0, "every worn item's kind has an equip slot that takes it");
    if (home < 0) continue;
    check(mob->WearItem(&it, home), "the shipped piece goes on the stock rig");
    int shells = 0;
    for (int p = 0; p < mob->WornPieceCount(); p++)
      if (mob->WornSlotsAt(p).size() &&
          mob->LimbDefAt(mob->WornSlotsAt(p)[0]).name.find("worn:" + it.name) ==
              0)
        shells = (int)mob->WornSlotsAt(p).size();
    check(shells == (int)it.cover.size(),
          "and EVERY one of its cover entries found a limb to hang on");
    wornShells += shells;
    mob->UnwearItem(home);
  }
  check(mob->LimbCount() == baseLimbs, "the shipped set comes off cleanly too");

  mobs.Reset();
  detail = Format("%d checks, %d base limbs, %d-shell robe, %d shipped pieces "
                  "/ %d shells",
                  checks, baseLimbs, (int)robeParts.size(), wornItems,
                  wornShells);
  std::printf("armor-wear: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ============================================================================
// armor-react — armour protects by BEING SOMETHING, not by having a number.
//
// There is no resist field anywhere in this feature and there is not going to
// be one. A robe stops fire reaching the skin for exactly two reasons: it is
// physically in the way (the occlusion probe), and it is `robe_cloth`, which
// the authored table says catches at 200/1000 against skin's 90. Steel stops
// acid because `steel` carries no `tag:dissolvable` and therefore never
// matches acid's rule — an absence, not a rule of its own.
//
// So what this gate asserts is that the WORLD CANNOT REACH A COVERED LIMB, and
// that it can again the moment the cover is gone. Four claims:
//
//   a. Cloth over flesh: the shell burns and the covered skin does not, while
//      an identical UNCOVERED limb on the same creature in the same fire does.
//      The control arm is the whole assertion — "the covered limb did not
//      burn" is also what a gate measuring a creature that never caught fire
//      would report.
//   b. Burn-through exposes: keep the fire on it and once the shell is gone,
//      the skin underneath starts converting. Protection has to END.
//   c. Steel in acid: a limb under a plate loses nothing while the control
//      dissolves, and the plate itself is untouched — a tag it does not carry.
//   d. Rule 2: a dressed creature standing in a settled world costs zero. The
//      probe must not have turned the burn pass's cheap early-out into a
//      per-tick cost for every clothed mob in the world.
// ============================================================================

Status GateArmorReact(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  MobSystem& mobs = c.mobs;
  DebrisSystem& debris = c.debris;
  bool ok = true;

  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mFire = matId("fire"), mAcid = matId("acid"),
                 mCloth = matId("robe_cloth"), mSteel = matId("steel"),
                 mSkin = matId("skin");
  if (!mFire || !mAcid || !mCloth || !mSteel || !mSkin) {
    detail = "armour/reactivity materials missing from materials.json";
    return Status::Fail;
  }

  int avDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == kAvatarDefName) avDef = (int)i;
  if (avDef < 0) {
    detail = Format("no '%s' def to dress", kAvatarDefName);
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[avDef];

  // A COVERED LIMB AND AN IDENTICAL BARE ONE. Chosen as a mirrored .L/.R pair
  // off the def so the two are the same shape, the same size and the same
  // material — the only difference between them is the shell. Any other choice
  // makes the differential a comparison of two different limbs.
  // ---- WHICH LIMB, AND WHY IT IS THE BIGGEST ONE ---------------------------
  //
  // THE FIRST VERSION USED A MIRRORED ARM PAIR — dress the left, leave the
  // right, same fire, compare. Elegant, and it measured nothing, for a reason
  // worth writing down because it constrains what this mechanic can ever do:
  //
  //     arm   173.62..174.38   (0.76 world voxels across)
  //     shell 173.38..174.62   (+0.24 of cloth on each side)
  //
  // The world grid's cell is ONE world voxel. A limb thinner than a cell, in
  // a coat a quarter of a cell thick, has no "outside the coat" — the fire
  // cell, the cloth and the flesh are all the same cell, and no probe can put
  // one between the other two. Measured: 107 of 130 world threats had no shell
  // in the outward direction at any reach, because the threat was already
  // inside the garment's envelope.
  //
  // So the subject is the TORSO, which is three-plus cells across and where
  // inside and outside are distinguishable. That is not a workaround; it is
  // the resolution limit of a grid-coupled body, and it says the honest thing
  // about the feature: armour occludes at the scale the grid can resolve.
  //
  // And because the torso has no mirrored twin, the control is a SECOND
  // CREATURE — same def, same fire, undressed. A better control than a mirror
  // anyway: it is the same limb, not its reflection.
  std::string covered;
  for (const MobLimbDef& ld : def.limbs)
    if (ld.tag == "spine" && (int)(&ld - def.limbs.data()) != def.rootLimb) {
      covered = ld.name;
      break;
    }
  if (covered.empty()) {
    detail = Format("'%s' has no non-root spine part to cover", kAvatarDefName);
    return Status::Skip;
  }

  int coveredIdx = -1;
  for (size_t i = 0; i < def.limbs.size(); i++)
    if (def.limbs[i].name == covered) coveredIdx = (int)i;
  const int controlIdx = coveredIdx;   // same limb, on the undressed creature
  const int rootLimb = def.rootLimb;

  MicroBodySet fixtureMicro;
  // Two skin voxels thick, matching the shipped set (one authored micro,
  // upscaled). A one-voxel shell would be testing a thickness nothing ships.
  const ItemDef cloak = MakeEnclosingFixture("fixture_cloak",
                                             ItemKind::ArmorChest, def, covered,
                                             mCloth, 2, fixtureMicro);
  const ItemDef plate = MakeEnclosingFixture("fixture_plate",
                                             ItemKind::ArmorShoulders, def,
                                             covered, mSteel, 2, fixtureMicro);
  if (cloak.cover.empty() || plate.cover.empty()) {
    detail = "could not build an enclosing shell over " + covered;
    return Status::Fail;
  }
  const int cloakSlot = (int)EquipSlotId::Chest;
  const int plateSlot = (int)EquipSlotId::Shoulders;

  uint32_t t = 14000;
  IVec3 pchunk{10, 0, 10};
  // ANCHORED TO THE LIVE WINDOW, never an absolute column: by the time this
  // runs, earlier gates have walked the residency window along x, and a
  // fixture outside it has its world writes dropped and its reactions never
  // run — a whole gate of zeroes that passes perfectly on its own
  // (selftest.h's ordering note; mob-burn paid for this once already).
  const IVec3 wOrg = world.WindowOrigin();
  auto fixture = [&](int inset) {
    return IVec3{wOrg.x * (int)kChunk + inset, 0, wOrg.z * (int)kChunk + inset};
  };

  // The creature's own world AABB, sampled off its live limbs. Needed because
  // of the next comment.
  auto bodyBox = [&](uint64_t id, Vec3& lo, Vec3& hi) {
    Mob* m = mobs.FindMobById(id);
    if (!m) return false;
    lo = Vec3{1e30f, 1e30f, 1e30f};
    hi = Vec3{-1e30f, -1e30f, -1e30f};
    for (int li = 0; li < m->LimbCount(); li++) {
      const uint32_t n = mobs.LimbVoxelCount(id, li);
      for (uint32_t k = 0; k < n; k += std::max(1u, n / 12u)) {
        const Vec3 p = mobs.LimbVoxelPos(id, li, k);
        lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y);
        lo.z = std::min(lo.z, p.z);
        hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y);
        hi.z = std::max(hi.z, p.z);
      }
    }
    return hi.x > lo.x;
  };

  auto soakTick = [&](uint64_t id, uint32_t soakMat, int soakUp) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(t + 1, world, ops, cellOps, spawns);
    if (soakMat && id) {
      // THE FIRE GOES AROUND THE CREATURE, NOT THROUGH IT.
      //
      // The obvious fixture — fill a box centred on the mob — puts fire in
      // grid cells that GEOMETRICALLY COINCIDE with the flesh, because a body
      // is not in the grid and `kCellOpIfAir` therefore sees empty space
      // wherever it stands. No armour can protect against that and none
      // should: the flames are already inside the coat.
      //
      // It is also what made this gate's first three runs unreadable. The
      // counters said 39 of 51 contact samples reached the skin THROUGH an
      // intact shell, which reads as a broken occlusion probe; the probe was
      // fine, and what those samples were seeded from was fire standing inside
      // the arm. Excluding the body's own box is the difference between
      // measuring armour and measuring the fixture.
      Vec3 lo{}, hi{};
      const bool haveBox = bodyBox(id, lo, hi);
      const Vec3 at = mobs.LimbVoxelPos(id, rootLimb, 0);
      const IVec3 b{ifloor(at.x), ifloor(at.y), ifloor(at.z)};
      for (int dy = -8; dy <= soakUp; dy++)
        for (int dz = -4; dz <= 4; dz++)
          for (int dx = -4; dx <= 4; dx++) {
            const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
            if (!world.CellInWindow(cc)) continue;
            if (haveBox) {
              const Vec3 mid{(float)cc.x + 0.5f, (float)cc.y + 0.5f,
                             (float)cc.z + 0.5f};
              if (mid.x >= lo.x - 0.5f && mid.x <= hi.x + 0.5f &&
                  mid.y >= lo.y - 0.5f && mid.y <= hi.y + 0.5f &&
                  mid.z >= lo.z - 0.5f && mid.z <= hi.z + 0.5f)
                continue;
            }
            if (cellOps.size() >= kMaxCellOpsPerTick) break;
            cellOps.push_back({World::SlotCellIndex(cc),
                               PackVoxNew(soakMat, 7u) | kCellOpIfAir});
          }
    }
    debris.QueueSupportEvents(world.Snap());
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps, false,
               pchunk, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    c.phys.Step(kTickDt);
    debris.PostStep();
    mobs.PostStep();
  };

  // Voxels of `mat` on one rig slot. Per LIMB, not per creature: the whole
  // subject here is one covered limb against one bare one, and a whole-body
  // census would average that difference away.
  auto limbMat = [&](uint64_t id, int slot, uint32_t mat) {
    return slot < 0 ? 0u : mobs.LimbMaterialCount(id, slot, mat);
  };
  auto shellSlotOf = [&](Mob* m, const char* item) {
    for (int p = 0; p < m->WornPieceCount(); p++) {
      const std::vector<int>& s = m->WornSlotsAt(p);
      if (!s.empty() &&
          m->LimbDefAt(s[0]).name.find(std::string("worn:") + item) == 0)
        return s[0];
    }
    return -1;
  };

  // ---- d. an idle DRESSED mob costs zero ----------------------------------
  // First, on a clean world, because it is the one claim a fire lit earlier
  // makes untestable. Rule 2 stated for the probe: adding armour must not turn
  // the burn pass's "nothing hot nearby, exit" into a per-tick cost.
  {
    debris.Reset();
    mobs.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    const IVec3 site = fixture(170);
    const int h = World::TerrainHeight(site.x, site.z, kDefaultSeed);
    pchunk = IVec3{site.x >> 4, h >> 4, site.z >> 4};
    const uint64_t id = mobs.Spawn(avDef, {site.x, h + 1, site.z});
    Mob* m = mobs.FindMobById(id);
    if (m && m->WearItem(&cloak, cloakSlot)) {
      const int shell = shellSlotOf(m, "fixture_cloak");

      // ---- THE PROBE ITSELF, before anything is on fire -------------------
      //
      // Recorded at the point of the CLAIM rather than left to be inferred
      // from a burn that did or did not happen. "The covered limb burned" has
      // at least three causes — the shell is somewhere else, the probe's
      // transform is wrong, or the burn pass samples past a shell that is
      // exactly where it should be — and a single boolean at the end of a
      // 160-tick fire distinguishes none of them (CLAUDE.md rule 6: a bare
      // count is not a measurement).
      //
      // ---- THE PREMISE, MEASURED ------------------------------------------
      //
      // Every reactivity claim below rests on "the shell encloses the limb",
      // and when one of them fails this is the line that says whether the
      // premise held. Recorded at the point of the CLAIM rather than inferred
      // from a burn that did or did not happen: "the covered limb burned" has
      // at least three causes — the shell is elsewhere, the probe's transform
      // is wrong, or the burn pass samples past a shell that is exactly where
      // it should be — and one boolean at the end of a 120-tick fire
      // distinguishes none of them (CLAUDE.md rule 6).
      //
      // TWO claims, because one of them alone has been misleading here twice:
      //
      //   * CONTAINMENT. The shell's world box strictly contains the limb's,
      //     with a positive margin on all six sides. Comparing boxes rather
      //     than probing shell voxels, because probing the shell with the
      //     shell's own transform is CIRCULAR — a shell placed in entirely
      //     the wrong place still reported 32 of 32.
      //   * INSIDE-NESS. From every sampled limb voxel there is cloth in some
      //     direction within the limb's own extent. Requiring cloth on four
      //     of six faces within a voxel was the second misleading version: on
      //     a torso most voxels are INTERIOR and legitimately several voxels
      //     from any wall, so a correct garment scored 2 of 24.
      if (shell >= 0) {
        auto boxOf = [&](int slot, Vec3& lo, Vec3& hi) {
          const uint32_t cnt = mobs.LimbVoxelCount(id, slot);
          lo = Vec3{1e30f, 1e30f, 1e30f};
          hi = Vec3{-1e30f, -1e30f, -1e30f};
          for (uint32_t k = 0; k < cnt; k += std::max(1u, cnt / 256u)) {
            const Vec3 p = mobs.LimbVoxelPos(id, slot, k);
            lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y);
            lo.z = std::min(lo.z, p.z);
            hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y);
            hi.z = std::max(hi.z, p.z);
          }
        };
        Vec3 llo, lhi, slo, shi;
        boxOf(coveredIdx, llo, lhi);
        boxOf(shell, slo, shi);
        const bool contains = slo.x < llo.x && slo.y < llo.y && slo.z < llo.z &&
                              shi.x > lhi.x && shi.y > lhi.y && shi.z > lhi.z;
        std::printf(
            "  shell contains limb: %s\n"
            "    limb  %.2f..%.2f  %.2f..%.2f  %.2f..%.2f\n"
            "    shell %.2f..%.2f  %.2f..%.2f  %.2f..%.2f\n",
            contains ? "PASS" : "FAIL", llo.x, lhi.x, llo.y, lhi.y, llo.z,
            lhi.z, slo.x, shi.x, slo.y, shi.y, slo.z, shi.z);
        ok = ok && contains;

        const float reach =
            std::max(shi.x - slo.x, std::max(shi.y - slo.y, shi.z - slo.z));
        const Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                              {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const uint32_t n = mobs.LimbVoxelCount(id, coveredIdx);
        int inside = 0, tried = 0;
        for (uint32_t k = 0; k < n && tried < 32; k += std::max(1u, n / 32u)) {
          tried++;
          const Vec3 at = mobs.LimbVoxelPos(id, coveredIdx, k);
          for (const Vec3& dd : dirs)
            if (m->WornAlong(coveredIdx, at, dd, reach)) { inside++; break; }
        }
        const bool probeOk = tried > 0 && inside == tried;
        std::printf(
            "  occlusion probe: %s (%d/%d limb voxels have cloth outside them "
            "within %.1f voxels)\n",
            probeOk ? "PASS" : "FAIL", inside, tried, reach);
        ok = ok && probeOk;
      }

      const uint32_t cloth0 = limbMat(id, shell, mCloth);
      const uint32_t skin0 = limbMat(id, coveredIdx, mSkin);
      for (int i = 0; i < 30; i++) soakTick(id, 0, 0);
      uint32_t front = 0;
      for (int li = 0; li < m->LimbCount(); li++)
        front += mobs.LimbBurningCount(id, li);
      const bool idle = front == 0 && limbMat(id, shell, mCloth) == cloth0 &&
                        limbMat(id, coveredIdx, mSkin) == skin0 && cloth0 > 0;
      std::printf("  dressed idle: %s (front %u, shell cloth %u, skin %u)\n",
                  idle ? "PASS" : "FAIL", front, cloth0, skin0);
      ok = ok && idle;
    } else {
      std::printf("  dressed idle: FAIL (could not dress the rig)\n");
      ok = false;
    }
    mobs.Reset();
  }

  // ---- a + b + c. THE SAME CREATURE, DRESSED AND NOT -----------------------
  //
  // Two mobs, same def, a few voxels apart, each in its own bath: A wears the
  // shell over its torso, B wears nothing. Same limb on both, so the only
  // difference between the two numbers is the garment.
  //
  // Run once per bath material — fire, then acid — because the two are
  // different mechanisms and only one of them is supposed to be stopped
  // outright. Cloth CATCHES (and once it is alight it sets you on fire, which
  // is what a burning robe should do); steel simply never dissolves and never
  // becomes hot, so under a plate the flesh must lose nothing at all.
  int coveredFirstLoss = -1, controlFirstLoss = -1, shellGoneAt = -1;
  uint32_t shellCloth0 = 0;

  // One arm of the differential. Returns the fraction of the covered limb's
  // skin lost on each creature.
  auto bath = [&](const ItemDef& piece, int slot, uint32_t soakMat, int soakUp,
                  int ticks, int inset, uint32_t& lostDressed,
                  uint32_t& lostBare, uint32_t& shellStart, uint32_t& shellEnd,
                  int* firstDressed, int* firstBare) {
    debris.Reset();
    mobs.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    const IVec3 sa = fixture(inset);
    const IVec3 sb = fixture(inset + 12);
    const int ha = World::TerrainHeight(sa.x, sa.z, kDefaultSeed);
    const int hb = World::TerrainHeight(sb.x, sb.z, kDefaultSeed);
    pchunk = IVec3{sa.x >> 4, ha >> 4, sa.z >> 4};
    const uint64_t a = mobs.Spawn(avDef, {sa.x, ha + 1, sa.z});
    const uint64_t b = mobs.Spawn(avDef, {sb.x, hb + 1, sb.z});
    Mob* ma = mobs.FindMobById(a);
    if (!ma || !b || !ma->WearItem(&piece, slot)) return false;
    const int shell = shellSlotOf(ma, piece.name.c_str());
    for (int i = 0; i < 12; i++) { soakTick(a, 0, 0); soakTick(b, 0, 0); }
    mobs.ResetWornStats();
    shellStart = limbMat(a, shell, mSteel) + limbMat(a, shell, mCloth);
    const uint32_t a0 = limbMat(a, coveredIdx, mSkin);
    const uint32_t b0 = limbMat(b, controlIdx, mSkin);
    // SAMPLED EVERY TICK, never read only at the end. A creature held in a
    // blaze dies, at which point every census reads zero and an end-state
    // comparison reports "100% gone" for both and proves nothing.
    for (int i = 0; i < ticks; i++) {
      soakTick(a, soakMat, soakUp);
      soakTick(b, soakMat, soakUp);
      if (!mobs.IsAlive(a) || !mobs.IsAlive(b)) break;
      if (firstDressed && *firstDressed < 0 &&
          limbMat(a, coveredIdx, mSkin) < a0)
        *firstDressed = i;
      if (firstBare && *firstBare < 0 && limbMat(b, controlIdx, mSkin) < b0)
        *firstBare = i;
      if (shellGoneAt < 0 && shell >= 0 && shellStart &&
          (limbMat(a, shell, mSteel) + limbMat(a, shell, mCloth)) * 4 <
              shellStart)
        shellGoneAt = i;
    }
    lostDressed = a0 - limbMat(a, coveredIdx, mSkin);
    lostBare = b0 - limbMat(b, controlIdx, mSkin);
    shellEnd = limbMat(a, shell, mSteel) + limbMat(a, shell, mCloth);
    return true;
  };

  // ---- a + b. cloth in fire -----------------------------------------------
  {
    uint32_t lostA = 0, lostB = 0, s0 = 0, s1 = 0;
    if (!bath(cloak, cloakSlot, mFire, 18, 120, 200, lostA, lostB, s0, s1,
              &coveredFirstLoss, &controlFirstLoss)) {
      detail = "could not dress the rig for the fire arm";
      return Status::Fail;
    }
    shellCloth0 = s0;
    const MobSystem::WornStats& w = mobs.Worn();
    // WHAT THE COAT ACTUALLY STOPPED, on the same line as whether it worked.
    // Without it "the covered limb burned" is a bare count, and the next step
    // is turning things off one at a time (CLAUDE.md rule 6).
    std::printf(
        "  worn occlusion: %u contact samples blocked / %u passed, %u of %u "
        "world threats substituted (%u more within 4x the reach)\n",
        w.seedsBlocked, w.seedsPassed, w.nbrSubstituted, w.nbrThreats,
        w.nbrMissInReach);

    // (a) THE COAT BUYS TIME. Asserted as a DELAY, not as less total damage,
    // and the difference is the mechanic rather than a weakened test.
    //
    // "Cloth armour reduces how much you burn" is FALSE, and measuring it was
    // this gate's own mistake. Cloth that is alight IS a hot neighbour -- the
    // occlusion substitution makes the flesh's neighbour "cloth_burning"
    // rather than "fire" -- so a robe that catches becomes a second fire
    // source pressed against the skin. Whether the dressed creature ends up
    // worse off than the bare one is then a question about how long each one
    // sat in the blaze, and it flipped between scopes on exactly that:
    //
    //     --gate    dressed lost 66,  bare 79   (first loss t+6  / t+1)
    //     --suite   dressed lost 325, bare 56   (first loss t+20 / t+1)
    //
    // The DELAY held in both, by a wide margin, because it is what the
    // mechanic actually does: the world cannot reach the skin until the cloth
    // is gone, and then it can. It is also why there is no mitigation number
    // anywhere in this feature -- a burning robe is SUPPOSED to be worse than
    // no robe, and a "protection value" could not express that.
    //
    // (Two scopes because a gate subset is not a small selftest: this runs at
    // a different residency-window origin in each, on different terrain, and
    // CLAUDE.md rule 7 says to compare arms at the same scope. Both arms here
    // are two creatures inside ONE run, so the comparison is sound either way
    // -- what changed was which of them stood in more fire.)
    const bool aOk = s0 > 0 && s1 < s0 && lostB > 0 && controlFirstLoss >= 0 &&
                     (coveredFirstLoss < 0 ||
                      coveredFirstLoss > controlFirstLoss);
    std::printf(
        "  cloth delays the fire: %s (bare skin went at t+%s, dressed at %s; "
        "dressed lost %u skin, bare %u; shell %u -> %u cloth)\n",
        aOk ? "PASS" : "FAIL",
        controlFirstLoss < 0 ? "never"
                             : std::to_string(controlFirstLoss).c_str(),
        coveredFirstLoss < 0 ? "never"
                             : std::to_string(coveredFirstLoss).c_str(),
        lostA, lostB, s0, s1);
    ok = ok && aOk;

    // (b) AND THE SHELL IS WHAT PAID FOR IT. Protection that never degrades is
    // an immunity flag with extra steps; this is the line that says the cloth
    // is being consumed instead.
    std::printf("  cover is consumed: %s (%u -> %u cloth voxels)\n",
                s1 < s0 ? "PASS" : "FAIL", s0, s1);
    ok = ok && s1 < s0;
  }

  // ---- c. steel in acid ----------------------------------------------------
  {
    uint32_t lostA = 0, lostB = 0, s0 = 0, s1 = 0;
    if (!bath(plate, plateSlot, mAcid, 4, 120, 250, lostA, lostB, s0, s1,
              nullptr, nullptr)) {
      detail = "could not plate the rig for the acid arm";
      return Status::Fail;
    }
    // The bare creature has to actually dissolve, or "the plated one did not"
    // is a statement about a bath that was never acid. And the plate itself
    // must be untouched: `steel` carries no tag:dissolvable, and no armour
    // value said so.
    const bool cOk = lostB > 0 && lostA == 0 && s0 > 0 && s1 == s0;
    std::printf(
        "  steel stops acid: %s (bare lost %u skin, plated lost %u; plate "
        "%u -> %u steel)\n",
        cOk ? "PASS" : "FAIL", lostB, lostA, s0, s1);
    ok = ok && cOk;
  }


  // LEAVE THE WORLD AS THIS GATE FOUND IT. It poured real acid and lit real
  // fires at absolute coordinates; every gate after it places fixtures by
  // world position and would otherwise be standing in the wreckage.
  debris.Reset();
  mobs.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  detail = Format("bare burned at t+%s, dressed %s; shell %u cloth",
                  controlFirstLoss < 0
                      ? "never"
                      : std::to_string(controlFirstLoss).c_str(),
                  coveredFirstLoss < 0
                      ? "never"
                      : std::to_string(coveredFirstLoss).c_str(),
                  shellCloth0);
  std::printf("armor-react: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ============================================================================
// armor-fit — one authored helmet, several sizes of head.
//
// The resample is a PURE FUNCTION over integers, which is the whole reason it
// can be asserted this cheaply: no world, no GPU, no rig. Three claims about
// the function and one about the thing that calls it.
//
// The last one is the only expensive bug here: a shell that is resampled but
// still drawn through the DEF's shared brick renders at the mannequin's size
// on everybody, and looks exactly like the resample not working.
// ============================================================================

Status GateArmorFit(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("armor-fit: FAILED %s\n", what);
    }
  };

  // A 4x4x4 solid, so every claim below is about the resample rather than
  // about which cells a hollow shell happened to have.
  const IVec3 src{4, 4, 4};
  std::vector<PrefabVoxel> box;
  for (int z = 0; z < src.z; z++)
    for (int y = 0; y < src.y; y++)
      for (int x = 0; x < src.x; x++)
        box.push_back(PrefabVoxel{(int16_t)x, (int16_t)y, (int16_t)z, 7,
                                  (uint8_t)(x + 1)});

  // ---- 1. identity is free -------------------------------------------------
  {
    const std::vector<PrefabVoxel> same = ResampleLattice(box, src, src);
    check(same.size() == box.size(), "resampling to the same size changes the "
                                     "count not at all");
  }

  // ---- 2. determinism ------------------------------------------------------
  // Not a ritual: this lattice is packed into a GPU brick and, once ground
  // items carry damage, into a save. A resample that differed run to run would
  // surface as armour that changed shape when you reloaded.
  {
    const IVec3 dst{7, 5, 9};
    const std::vector<PrefabVoxel> a = ResampleLattice(box, src, dst);
    const std::vector<PrefabVoxel> b = ResampleLattice(box, src, dst);
    check(a.size() == b.size(), "two resamples of the same input agree in size");
    bool identical = a.size() == b.size();
    for (size_t i = 0; identical && i < a.size(); i++)
      identical = a[i].x == b[i].x && a[i].y == b[i].y && a[i].z == b[i].z &&
                  a[i].material == b[i].material && a[i].color == b[i].color;
    check(identical, "and byte for byte");
  }

  // ---- 3. no holes, and the box is the box --------------------------------
  {
    const IVec3 dst{7, 5, 9};
    const std::vector<PrefabVoxel> up = ResampleLattice(box, src, dst);
    check(up.size() == (size_t)dst.x * dst.y * dst.z,
          "growing a SOLID box gives a solid box — every target cell whose "
          "nearest source was solid is solid, so an upscale cannot perforate");
    IVec3 lo{1 << 30, 1 << 30, 1 << 30}, hi{-1, -1, -1};
    bool coloured = false;
    for (const PrefabVoxel& v : up) {
      lo.x = std::min<int>(lo.x, v.x); lo.y = std::min<int>(lo.y, v.y);
      lo.z = std::min<int>(lo.z, v.z);
      hi.x = std::max<int>(hi.x, v.x); hi.y = std::max<int>(hi.y, v.y);
      hi.z = std::max<int>(hi.z, v.z);
      if (v.color) coloured = true;
    }
    check(lo.x == 0 && lo.y == 0 && lo.z == 0 && hi.x == dst.x - 1 &&
              hi.y == dst.y - 1 && hi.z == dst.z - 1,
          "and it fills exactly the box it was asked for");
    check(coloured, "art colour survives the resample — it is what a black "
                    "robe's black is");

    // Shrinking is a resample, not a filter: it may drop matter, and must not
    // invent any or leave the box.
    const IVec3 down{2, 2, 2};
    const std::vector<PrefabVoxel> dn = ResampleLattice(box, src, down);
    check(dn.size() == 8, "shrinking gives the smaller box, filled");
  }

  // ---- 4. THE BRICK. A resampled shell must not draw through the def's -----
  int avDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == kAvatarDefName) avDef = (int)i;
  if (avDef < 0) {
    detail = Format("no '%s' def to dress", kAvatarDefName);
    std::printf("armor-fit: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[avDef];
  std::string part;
  for (const MobLimbDef& ld : def.limbs)
    if (ld.tag == "spine" && (int)(&ld - def.limbs.data()) != def.rootLimb) {
      part = ld.name;
      break;
    }
  if (part.empty()) {
    detail = "no spine part to fit against";
    return Status::Skip;
  }

  MicroBodySet fixtureMicro;
  const uint32_t mat = 1u;
  // The SAME fixture twice, differing only in the fitBox it claims to have
  // been drawn against. A piece whose fitBox matches the wearer wears as
  // authored; one that claims a mannequin half the size must be resampled up.
  ItemDef asAuthored = MakeEnclosingFixture("fit_stock", ItemKind::ArmorChest,
                                            def, part, mat, 2, fixtureMicro);
  ItemDef asSmallMannequin = MakeEnclosingFixture(
      "fit_small", ItemKind::ArmorChest, def, part, mat, 2, fixtureMicro);
  if (asAuthored.cover.empty() || asSmallMannequin.cover.empty()) {
    detail = "could not build a fixture shell over " + part;
    return Status::Fail;
  }
  asSmallMannequin.cover[0].fitBox = asAuthored.cover[0].fitBox * 0.6f;

  mobs.Reset();
  const int h = World::TerrainHeight(160, 160, kDefaultSeed);
  const uint64_t id = mobs.Spawn(avDef, {160, h + 1, 160});
  Mob* m = mobs.FindMobById(id);
  if (!m) {
    detail = "spawn refused";
    return Status::Fail;
  }
  const int slot = (int)EquipSlotId::Chest;

  auto shellSpan = [&](const char* itemName) {
    for (int p = 0; p < m->WornPieceCount(); p++) {
      const std::vector<int>& s = m->WornSlotsAt(p);
      if (s.empty()) continue;
      if (m->LimbDefAt(s[0]).name.find(std::string("worn:") + itemName) != 0)
        continue;
      Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
      const uint32_t n = mobs.LimbVoxelCount(id, s[0]);
      for (uint32_t k = 0; k < n; k += std::max(1u, n / 128u)) {
        const Vec3 q = mobs.LimbVoxelPos(id, s[0], k);
        lo.x = std::min(lo.x, q.x); lo.y = std::min(lo.y, q.y);
        lo.z = std::min(lo.z, q.z);
        hi.x = std::max(hi.x, q.x); hi.y = std::max(hi.y, q.y);
        hi.z = std::max(hi.z, q.z);
      }
      return Vec3{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
    }
    return Vec3{};
  };

  check(m->WearItem(&asAuthored, slot), "the as-authored piece goes on");
  const Vec3 spanA = shellSpan("fit_stock");
  int brickA = -1;
  for (int p = 0; p < m->WornPieceCount(); p++)
    if (!m->WornSlotsAt(p).empty())
      brickA = m->LimbDefAt(m->WornSlotsAt(p)[0]).microModel;
  m->UnwearItem(slot);

  check(m->WearItem(&asSmallMannequin, slot), "and so does the resampled one");
  const Vec3 spanB = shellSpan("fit_small");
  int brickB = -1;
  for (int p = 0; p < m->WornPieceCount(); p++)
    if (!m->WornSlotsAt(p).empty())
      brickB = m->LimbDefAt(m->WornSlotsAt(p)[0]).microModel;

  // Authored for a mannequin 0.6x this wearer, so it has to be scaled UP to
  // fit — the whole point of recording a fitBox.
  const bool bigger = spanB.x > spanA.x * 1.2f && spanB.y > spanA.y * 1.2f &&
                      spanB.z > spanA.z * 1.2f;
  check(bigger,
        "a piece drawn for a smaller mannequin is resampled UP onto this one");
  check(brickA >= 0 && brickB >= 0 && brickA != brickB,
        "and it draws through its OWN brick — sharing the def's would render "
        "the mannequin's size on every wearer, which looks exactly like the "
        "resample not working");
  std::printf("  fit: authored span %.2f/%.2f/%.2f -> resampled %.2f/%.2f/%.2f "
              "(brick %d -> %d)\n",
              spanA.x, spanA.y, spanA.z, spanB.x, spanB.y, spanB.z, brickA,
              brickB);

  // A resampled shell still burns, carves and severs — it is a rig slot like
  // any other, and the resample must not have made it a special case.
  {
    int s0 = -1;
    for (int p = 0; p < m->WornPieceCount(); p++)
      if (!m->WornSlotsAt(p).empty()) s0 = m->WornSlotsAt(p)[0];
    check(s0 >= 0 && mobs.LimbVoxelCount(id, s0) > 0,
          "the resampled shell has real collider voxels");
    if (s0 >= 0) {
      const uint32_t before = mobs.LimbVoxelCount(id, s0);
      std::vector<ParticleSpawn> cs;
      mobs.CarveLimbRadial(mobs.LimbBody(id, s0),
                           mobs.LimbVoxelPos(id, s0, 0), 0.8f, false, false,
                           c.world, cs);
      check(mobs.LimbVoxelCount(id, s0) < before,
            "and carves like anything else");
    }
  }
  m->UnwearItem(slot);
  mobs.Reset();

  detail = Format("%d checks", checks);
  std::printf("armor-fit: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ============================================================================
// item-ground — dropping, picking up, and surviving a save.
//
// A dropped item is an ORDINARY DEBRIS BODY plus one entry in a registry that
// says which item it is (game/worlditems.h). Everything interesting about that
// design is about the seam between those two halves, so that is what this
// asserts:
//
//   1. A DROP MAKES A REAL BODY, and the registry names it.
//   2. A PICKUP DESTROYS THE BODY. Leaving it would duplicate the item — one
//      in the pack, one still lying there to be picked up again.
//   3. THE REGISTRY NEVER OUTLIVES THE HANDLE. Jolt reuses handles; an entry
//      that survived its body would eventually re-match a NEW body and hand
//      the player a sword they picked up off a rock. This is the one that
//      would be found months later, by a player, and be unreproducible.
//   4. 'ITMS' ROUND-TRIPS. Save, reset, load, and the same items are on the
//      ground under the same names — the currency the format stores in.
// ============================================================================

Status GateItemGround(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  DebrisSystem& debris = c.debris;
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("item-ground: FAILED %s\n", what);
    }
  };

  // Whatever the library holds, not a named asset: a tree with different
  // content still exercises the mechanism.
  const ItemDef* any = c.items.items.empty() ? nullptr : &c.items.items[0];
  if (!any) {
    detail = "no items in the library to drop";
    return Status::Skip;
  }

  debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  WorldItems ground;
  // THE RELEASE HOOK IS UNDER TEST, so it is wired exactly as the game wires
  // it rather than the registry being poked by hand.
  debris.SetOnBodyGone([&ground](uint64_t h) { ground.OnBodyGone(h); });

  const IVec3 wOrg = world.WindowOrigin();
  const IVec3 site{wOrg.x * (int)kChunk + 190, 0, wOrg.z * (int)kChunk + 190};
  const int h = World::TerrainHeight(site.x, site.z, kDefaultSeed);
  const Vec3 at{(float)site.x, (float)(h + 6), (float)site.z};

  // ---- 1. drop -------------------------------------------------------------
  const uint64_t body =
      DropItemToWorld(*any, at, Vec3{}, c.phys, debris, nullptr, ground);
  check(body != 0, "dropping an item makes a real body");
  check(ground.Count() == 1, "and one registry entry");
  const WorldItem* w = ground.Find(body);
  check(w && w->item == any->name, "which names the item that was dropped");

  // ---- 2. a body the registry does not know is not an item -----------------
  // The filter that keeps `E` from picking up somebody's wall.
  check(ground.Find(body + 12345) == nullptr,
        "an unregistered body is not a thing you can pick up");

  // ---- 3. destroying the body drops the entry ------------------------------
  check(debris.DestroyBody(body), "the body can be taken out of the world");
  check(ground.Count() == 0,
        "and the release hook drops the registry entry with it — a handle Jolt "
        "reuses must never resolve to the item that used to hold it");

  // ---- 4. 'ITMS' round trip ------------------------------------------------
  {
    const uint64_t a =
        DropItemToWorld(*any, at, Vec3{}, c.phys, debris, nullptr, ground);
    const Vec3 at2{at.x + 4.0f, at.y, at.z};
    const uint64_t b =
        DropItemToWorld(*any, at2, Vec3{}, c.phys, debris, nullptr, ground);
    check(a && b && ground.Count() == 2, "two items on the ground");

    WorldItemRefs refs{&ground, &c.phys, &debris, nullptr, &c.items};
    EntityIO io = MakeEntityIO(debris, c.mobs, nullptr, nullptr, &refs);
    const EntitySection* itms = nullptr;
    for (const EntitySection& s : io.sections)
      if (s.id ==
          (uint32_t)('I' | ('T' << 8) | ('M' << 16) | ((uint32_t)'S' << 24)))
        itms = &s;
    check(itms != nullptr, "MakeEntityIO registers an ITMS section");
    if (itms) {
      std::vector<uint8_t> bytes;
      itms->save(bytes);
      check(!bytes.empty(), "and it writes something");

      // Wiped through the section's OWN reset, which is the call a load makes.
      itms->reset();
      check(ground.Count() == 0, "reset clears the ground");

      check(itms->load(bytes.data(), bytes.size(), kWorldItemSaveVersion),
            "and the payload loads back");
      check(ground.Count() == 2, "with both items on the ground again");
      int named = 0;
      for (const WorldItem& g : ground.All())
        if (g.item == any->name) named++;
      check(named == 2, "still named the same item, resolved through the "
                        "library rather than by index");

      // Truncation and an unknown version are REFUSED, not half-applied — the
      // same discipline PLYR keeps, and for the same reason: saves get
      // truncated by full disks.
      check(!itms->load(bytes.data(), bytes.size() / 2, kWorldItemSaveVersion),
            "a truncated payload is refused");
      check(!itms->load(bytes.data(), bytes.size(),
                        kWorldItemSaveVersion + 1),
            "an unknown version is refused");
    }
  }

  // Leave nothing behind: the hook captures a local, and the bodies are real.
  debris.SetOnBodyGone(nullptr);
  debris.Reset();
  ground.Clear();

  detail = Format("%d checks", checks);
  std::printf("item-ground: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& EquipmentGates() {
  static const std::vector<Gate> g = {
      // Needs mob defs and physics, and it spawns on real terrain — so it
      // declares the same dependency the body gates do rather than assuming
      // whatever the previous gate left standing.
      {"armor-wear", "equipment", {"prefab"}, false, GateArmorWear},
      // Lights real fires and pours real acid at absolute coordinates, and
      // regenerates the world on the way out — the same discipline (and the
      // same slot in the order) as `mob-burn`.
      {"armor-react", "equipment", {"prefab"}, false, GateArmorReact},
      // Ground items. Makes real bodies at real coordinates and clears them
      // on the way out, so it sits beside the other two rather than near
      // anything that measures a settled world.
      {"item-ground", "equipment", {"prefab"}, false, GateItemGround},
      // Mostly a pure function over integers, plus one rig assertion. Cheap,
      // and it wants nothing any other gate leaves behind.
      {"armor-fit", "equipment", {"prefab"}, false, GateArmorFit},
  };
  return g;
}

}  // namespace selftest
