// selftest_grimoire.cpp — the grimoire: macros as saved word lists.
//
// CPU-ONLY AND CONTENT-INDEPENDENT, like player-kit beside it: it builds its
// own four-glyph library and its own pages, so it asserts on the RULES of
// docs/PLAN_magic_grammar.md §12b/§12c rather than on whatever glyphs.json
// says today. It runs in milliseconds and `--gate grimoire` is the whole
// verification loop.
//
// WHAT IT PROTECTS:
//   1. EXPANSION IS SPEAKING. A page's expansion compiles to the same cast
//      list as speaking its words; a page bound to a slot speaks the same.
//   2. NESTING EXPANDS TO DEPTH and stops at the cap rather than recursing.
//   3. A CYCLE IS REFUSED AT SAVE, with a reason a panel can show.
//   4. THE OVERFLOW CAP truncates and reports; nothing expands past the stack.
//   5. A MISSING NAME drops one word and keeps the page.
//   6. 'PLYR' v4 ROUND-TRIPS the grimoire and the twenty slots BY NAME; a v3
//      payload loads with bank A intact; a truncated payload is refused.
//   7. CAPTURE names the page from the sentence.

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

GlyphLibrary MakeLib() {
  GlyphLibrary lib;
  auto add = [&](const char* id, GlyphSort sort) {
    GlyphDef d;
    d.id = id;
    d.sort = sort;
    d.word = 1;
    return d;
  };
  GlyphDef fire = add("kit_fire", GlyphSort::Matter);
  fire.material = 7;
  GlyphDef boom = add("kit_boom", GlyphSort::Effect);
  boom.verb = SpellVerb::Explode;
  boom.radius = 3;
  GlyphDef bolt = add("kit_bolt", GlyphSort::Delivery);
  bolt.mech = DeliveryMech::Flight;
  GlyphDef fan = add("kit_fan", GlyphSort::Mod);
  fan.field = ModField::Count;
  fan.op = ModOp::Mul;
  fan.amount = 3;
  lib.glyphs = {fire, boom, bolt, fan};
  lib.hand.id = "hand";
  lib.hand.sort = GlyphSort::Delivery;
  // An authored starter, the way glyphs.json's conjoined block arrives.
  ConjoinedGlyph starter;
  starter.id = "kit_starter";
  starter.glyphs = {0, 2};   // kit_fire kit_bolt
  lib.conjoined.push_back(starter);
  return lib;
}

std::string Sig(const GlyphLibrary& lib, const std::vector<int>& spoken) {
  SpellStack st;
  st.spoken = spoken;
  const CastList l = CompileSpell(lib, st);
  return BracketSpell(lib, l.tree) + "|" + std::to_string(l.manaCost) + "|" +
         std::to_string(l.casts.size());
}

Status GateGrimoire(Ctx& c, std::string& detail) {
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("grimoire: FAILED %s\n", what);
    }
  };
  const GlyphLibrary lib = MakeLib();
  const int gFire = lib.Find("kit_fire"), gBoom = lib.Find("kit_boom"),
            gBolt = lib.Find("kit_bolt"), gFan = lib.Find("kit_fan");
  check(gFire == 0 && gBoom == 1 && gBolt == 2 && gFan == 3, "the fixture library resolves");

  // ---- 1. expansion is speaking ----------------------------------------------
  Grimoire g;
  g.pages.push_back({"hellfire", {"kit_fire", "kit_boom", "kit_fan", "kit_fan"}});
  {
    const GrimoireExpansion ex = ExpandWords(lib, g, {"hellfire"}, kSpellStackMax);
    check(ex.spoken == std::vector<int>{gFire, gBoom, gFan, gFan} && ex.dropped == 0 &&
              !ex.truncated,
          "a page expands to its words");
    check(Sig(lib, ex.spoken) == Sig(lib, {gFire, gBoom, gFan, gFan}),
          "and compiles to the same cast list as speaking them");
    // A fragment: `hellfire kit_bolt` is a live sentence.
    const GrimoireExpansion ex2 = ExpandWords(lib, g, {"hellfire", "kit_bolt"}, kSpellStackMax);
    check(Sig(lib, ex2.spoken) == Sig(lib, {gFire, gBoom, gFan, gFan, gBolt}),
          "a page is a fragment: a delivery after it closes the clause");
    // And `hellfire hellfire` merges by R1: fan x4.
    const GrimoireExpansion ex3 = ExpandWords(lib, g, {"hellfire", "hellfire"}, kSpellStackMax);
    const CastList l3 = CompileSpell(lib, SpellStack{ex3.spoken});
    check(l3.casts.size() == 1 &&
              l3.casts[0].delivery.count == std::min(81, lib.budgets.maxInstances),
          "two expansions merge by R1 (fan x4 = 81 instances, capped by budgets.maxInstances)");
    // The authored starter is a page too, read-only.
    GrimoirePage scratch;
    const GrimoirePage* st = FindPage(lib, g, "kit_starter", scratch);
    check(st && st->readOnly && st->words.size() == 2, "an authored starter resolves as a read-only page");
    const GrimoireExpansion ex4 = ExpandWords(lib, g, {"kit_starter"}, kSpellStackMax);
    check(ex4.spoken == std::vector<int>{gFire, gBolt}, "and expands");
  }

  // ---- 2. nesting expands to depth, and stops at the cap ---------------------
  {
    Grimoire n;
    n.pages.push_back({"a", {"kit_fire"}});
    n.pages.push_back({"b", {"a", "kit_boom"}});
    n.pages.push_back({"c", {"b", "b"}});
    n.pages.push_back({"d", {"c", "kit_bolt"}});
    n.pages.push_back({"e", {"d"}});
    n.pages.push_back({"f", {"e"}});
    const GrimoireExpansion ed = ExpandWords(lib, n, {"d"}, kSpellStackMax);
    check(ed.spoken == std::vector<int>{gFire, gBoom, gFire, gBoom, gBolt} && !ed.tooDeep,
          "a nest four deep expands fully");
    const GrimoireExpansion ef = ExpandWords(lib, n, {"f"}, kSpellStackMax);
    // f -> e -> d -> c is four levels; b is the fifth and expands to nothing.
    check(ef.tooDeep && ef.spoken == std::vector<int>{gBolt},
          "a nest past budgets.maxMacroDepth stops at the cap and says so");
  }

  // ---- 3. a cycle is refused at save -----------------------------------------
  {
    Grimoire cy;
    cy.pages.push_back({"x", {"y"}});
    cy.pages.push_back({"y", {"kit_fire"}});
    std::string why;
    check(!GrimoireWouldCycle(lib, cy, "y", {"kit_fire", "kit_bolt"}, why),
          "an acyclic edit is allowed");
    check(GrimoireWouldCycle(lib, cy, "y", {"x"}, why) && !why.empty(),
          "y = [x] with x = [y] is refused with a reason");
    check(GrimoireWouldCycle(lib, cy, "z", {"kit_fire", "z"}, why),
          "a page that names itself is refused");
    // And an expansion that meets a cycle anyway (content edited under it)
    // is bounded, not infinite.
    cy.pages[1].words = {"x"};
    const GrimoireExpansion ex = ExpandWords(lib, cy, {"x"}, kSpellStackMax);
    check(ex.tooDeep && ex.spoken.empty(), "expanding a cycle stops rather than recursing");
  }

  // ---- 4. the overflow cap ------------------------------------------------------
  {
    Grimoire big;
    std::vector<std::string> twenty(20, "kit_fire");
    big.pages.push_back({"twenty", twenty});
    const GrimoireExpansion ex = ExpandWords(lib, big, {"twenty"}, kSpellStackMax);
    check(ex.truncated && (int)ex.spoken.size() == kSpellStackMax,
          "a page past the stack bound speaks as much as fits and reports it");
    PlayerCaster pc;
    pc.grimoire = big;
    pc.inventory.Grant(gFire);
    pc.inventory.BindPage(0, "twenty");
    pc.SpeakSlot(lib, 0);
    check((int)pc.stack.spoken.size() == kSpellStackMax && !pc.note.empty(),
          "speaking it fills the stack exactly and leaves a note");
    check(!pc.SpeakSlot(lib, 0), "and a full stack refuses the next word");
  }

  // ---- 5. a missing name drops one word and keeps the page ---------------------
  {
    Grimoire m;
    m.pages.push_back({"holey", {"kit_fire", "gone_glyph", "kit_bolt"}});
    const GrimoireExpansion ex = ExpandWords(lib, m, {"holey"}, kSpellStackMax);
    check(ex.dropped == 1 && ex.spoken == std::vector<int>{gFire, gBolt},
          "a word that names nothing drops, the rest speak");
    check(ex.readout.size() == 3 && ex.readout[1] == "?", "and the readout shows ? in its place");
    check(m.Find("holey") == 0 && m.pages[0].words.size() == 3, "the page itself is untouched");
  }

  // ---- 6. PLYR v4 round trip, v3 still loads, truncation refused ----------------
  {
    ItemLibrary items;
    ItemDef rock;
    rock.name = "kit_rock";
    rock.kind = ItemKind::None;
    items.items.push_back(rock);
    PlayerCaster caster;
    Inventory hb;
    PlayerKit kit;
    for (int i = 0; i < (int)lib.glyphs.size(); i++) caster.inventory.Grant(i);
    caster.grimoire.pages.push_back({"hellfire", {"kit_fire", "kit_boom", "kit_fan"}});
    caster.grimoire.pages.push_back({"nested", {"hellfire", "kit_bolt"}});
    caster.inventory.Bind(0, gFire);
    caster.inventory.Bind(12, gBoom);          // bank B
    caster.inventory.BindPage(3, "nested");    // a macro on a key
    hb.slots[1] = {0, 2};

    PlayerKitRefs refs{&caster, &lib, &hb, &kit, &items};
    EntityIO io = MakeEntityIO(c.debris, c.mobs, nullptr, &refs);
    const EntitySection* plyr = nullptr;
    for (const EntitySection& s : io.sections)
      if (s.id == (uint32_t)('P' | ('L' << 8) | ('Y' << 16) | ((uint32_t)'R' << 24)))
        plyr = &s;
    check(plyr != nullptr, "MakeEntityIO registers PLYR");
    if (plyr) {
      std::vector<uint8_t> v4;
      plyr->save(v4);
      std::vector<uint8_t> v3;
      SavePlayerKit(refs, v3, 3);
      check(v3.size() < v4.size(), "a v3 payload is the v4 one without the grimoire block");

      plyr->reset();
      check(caster.grimoire.pages.empty() && caster.inventory.KindAt(3) == SlotKind::None &&
                caster.inventory.At(12) < 0,
            "reset clears the grimoire and every slot of both banks");
      check(plyr->load(v4.data(), v4.size(), kPlayerKitSaveVersion), "v4 loads");
      check(caster.grimoire.pages.size() == 2 && caster.grimoire.Find("nested") == 1 &&
                caster.grimoire.pages[1].words == std::vector<std::string>{"hellfire", "kit_bolt"},
            "the pages came back by name, words and all");
      check(caster.inventory.KindAt(3) == SlotKind::Page && caster.inventory.PageAt(3) == "nested",
            "the macro is still on its key");
      check(caster.inventory.At(0) == gFire && caster.inventory.At(12) == gBoom,
            "and both banks' glyph keys");
      check(hb.slots[1].count == 2, "beside the older sections");

      plyr->reset();
      check(plyr->load(v3.data(), v3.size(), 3), "a v3 payload still loads");
      check(caster.grimoire.pages.empty(), "with an empty grimoire");
      check(caster.inventory.At(0) == gFire, "and bank A intact");

      check(!plyr->load(v4.data(), v4.size() / 2, kPlayerKitSaveVersion),
            "a truncated v4 payload is refused");
      check(!plyr->load(v4.data(), v4.size(), kPlayerKitSaveVersion + 1),
            "an unknown version is refused");
      check(!plyr->load(v4.data(), v4.size(), 2), "v2 stays refused");
    }
  }

  // ---- 7. a bound macro speaks its expansion; capture names the page ---------
  {
    PlayerCaster pc;
    pc.grimoire.pages.push_back({"hellfire", {"kit_fire", "kit_boom", "kit_fan", "kit_fan"}});
    for (int i = 0; i < (int)lib.glyphs.size(); i++) pc.inventory.Grant(i);
    pc.inventory.BindPage(5, "hellfire");
    pc.inventory.Bind(2, gBolt);
    pc.SpeakSlot(lib, 5);
    pc.SpeakSlot(lib, 2);
    const GrimoireExpansion ex = ExpandWords(lib, pc.grimoire, {"hellfire", "kit_bolt"}, 16);
    check(Sig(lib, pc.stack.spoken) == Sig(lib, ex.spoken),
          "speaking a bound macro then a glyph is the same cast list as the expansion");
    // Capture: fire fire bolt -> "kit_fire2-kit_bolt"
    pc.Clear(lib);
    pc.stack.spoken = {gFire, gFire, gBolt};
    const std::string name = pc.CaptureStack(lib);
    check(name == "kit_fire2-kit_bolt", "capture derives the name from the readout");
    check(pc.grimoire.Find(name) >= 0 &&
              pc.grimoire.pages[pc.grimoire.Find(name)].words ==
                  std::vector<std::string>{"kit_fire", "kit_fire", "kit_bolt"},
          "and saves the words by name");
    const std::string again = pc.CaptureStack(lib);
    check(again == "kit_fire2-kit_bolt-2", "a second capture of the same sentence gets a suffix");
  }

  detail = Format("%d checks", checks);
  std::printf("grimoire: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& GrimoireGates() {
  static const std::vector<Gate> g = {
      {"grimoire", "player", {}, false, GateGrimoire},
  };
  return g;
}

}  // namespace selftest
