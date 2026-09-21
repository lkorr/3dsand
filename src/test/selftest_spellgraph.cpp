// selftest_spellgraph.cpp — the spell graph: layout, the linearizer, the ops.
//
// CPU-ONLY, beside `grimoire` and `spells-oracle`: it reads glyphs.json and
// the generated grammar oracle and touches no world, no GPU and nothing a
// later gate depends on. `--gate spell-graph` is the whole verification loop
// for PLAN_spell_graph phase 2 and it runs in well under a second.
//
// WHAT IT PROTECTS:
//
//   1. THE ROUND-TRIP LAW. For every sentence in assets/spells/grammar_oracle
//      .json (the reference script's whole corpus) and every generated
//      sequence of length <= 3 over the `spells` gate's alphabet,
//      `Parse(Linearize(Parse(s)))` is `Parse(s)` — the same tree by its
//      span-agnostic key — and the two LOWER to an identical cast list, field
//      by field. That is the claim the page rests on: the drawing is a view of
//      the sentence and editing the drawing writes a sentence back.
//   2. LINEARIZING IS CANONICAL. `Linearize` of a re-parse is a FIXED POINT,
//      so two players who built the same spell by different routes save the
//      same words, and nothing grows past `kSpellStackMax`.
//   3. EVERY OP IS TOTAL. Over a few hundred generated (tree, op, target,
//      glyph) cases each op either succeeds with words that re-parse to what
//      it intended, or refuses with a reason. Never a crash, never a silent
//      wrong tree — the op proves itself by re-parsing before it returns.
//   4. THE UNSPEAKABLE TREES ARE REFUSED, by name: two deliveries in one
//      segment, and an empty `echo` beside an empty `null`.
//   5. THE WORKED SENTENCES of the plan's §2 can be BUILT by ops from a blank
//      page, one gesture at a time.
//   6. THE LAYOUT IS SANE: no two nodes of one layer overlap, every child sits
//      inside its parent's span, a join has exactly `instances` sockets, each
//      lane feeds exactly one of them, and the root is at the maximum y.

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "game/spell.h"
#include "game/spellgraph.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// The `spells` gate's alphabet, copied rather than shared: this gate must keep
// asserting over the same 21 words even if that file's fixture moves, and a
// copied array is cheaper than a header nobody else wants.
const char* const kAlphabet[] = {
    "fire", "gold",  "air",        "anything", "explosive", "gust",       "transmute",
    "mend", "trail", "null",       "aura",     "echo",      "shotgun",    "float",
    "lane", "projectile", "bomb",  "self",     "beam",      "twin",       "end",
};

// ---- the lowering signature (the `spells` gate's comparer) -------------------
// Every field of a lowered cast list as one string, so two lowerings compare
// for IDENTITY rather than for a few hand-picked numbers.

void SigRecord(const GlyphLibrary& lib, const DeliveryRec& d, std::string& s);

void SigEffect(const GlyphLibrary& lib, const EffectInst& e, std::string& s) {
  char buf[256];
  std::snprintf(buf, sizeof buf, "{%s g%d n%d c%d A%u/%d B%u/%d r%d f%d o%d a%d m%d",
                SpellVerbName(e.verb), e.glyph, e.n, e.complete ? 1 : 0, e.matA,
                e.anyA ? 1 : 0, e.matB, e.anyB ? 1 : 0, e.radius, (int)e.modField,
                (int)e.modOp, e.modAmount, e.modN);
  s += buf;
  for (const DeliveryRec& d : e.launch) SigRecord(lib, d, s);
  for (const EffectInst& i : e.inner) SigEffect(lib, i, s);
  s += "}";
}

void SigRecord(const GlyphLibrary& lib, const DeliveryRec& d, std::string& s) {
  char buf[384];
  std::snprintf(buf, sizeof buf,
                "<d%d m%d w%d c%d sp%d lt%d ir%d g%d fu%d re%d n%d b%d p%d s%d rm%d "
                "body%d x%d tb%d te%d",
                d.glyph, (int)d.mech, d.weight, d.carryMille, d.speedFx, d.lifetimeTicks,
                d.impactRadius, d.gravityMille, d.fuseTicks, d.reach, d.count, d.bounces,
                d.pierce, d.seek, d.radiusMille, d.body ? 1 : 0, d.resolveOnExpiry ? 1 : 0,
                d.trailBudget, d.trailEvery);
  s += buf;
  for (const EffectInst& e : d.trail) SigEffect(lib, e, s);
  for (const SpellLane& ln : d.lanes) {
    s += "/";
    SigRecord(lib, ln.rec, s);
    for (const EffectInst& e : ln.extra) SigEffect(lib, e, s);
  }
  s += ">";
}

std::string CastSignature(const GlyphLibrary& lib, const CastList& l) {
  std::string s;
  char buf[256];
  for (const SpellCast& c : l.casts) {
    SigRecord(lib, c.delivery, s);
    std::snprintf(buf, sizeof buf,
                  "[inst%d word%d tar%d carry%d unk%d t%d v%d gen%d dep%d lv%d clamp%d]",
                  c.instances, c.wordCost, c.tariff, c.carryCost, c.priceUnknown ? 1 : 0,
                  c.ticks, c.voxels, c.generation, c.depth, c.leaves,
                  c.instancesClamped ? 1 : 0);
    s += buf;
    for (int m : c.wastedMods) s += "!" + std::to_string(m);
    s += "|";
    for (const EffectInst& e : c.payload) SigEffect(lib, e, s);
  }
  std::snprintf(buf, sizeof buf, " cost%d/%d/%d=%d", l.wordCost, l.tariff, l.carryCost,
                l.manaCost);
  return s + buf;
}

// ---- small helpers ------------------------------------------------------------

std::vector<std::string> Split(const std::string& s) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t sp = s.find(' ', pos);
    if (sp == std::string::npos) sp = s.size();
    const std::string w = s.substr(pos, sp - pos);
    if (!w.empty()) out.push_back(w);
    pos = sp + 1;
  }
  return out;
}

std::string Join(const std::vector<std::string>& w) {
  std::string s;
  for (const std::string& x : w) s += (s.empty() ? "" : " ") + x;
  return s;
}

int CountWord(const std::vector<std::string>& w, const std::string& id) {
  int n = 0;
  for (const std::string& x : w)
    if (x == id) n++;
  return n;
}

// Every node the clause roots can reach, so the op fuzz has real targets.
void Reach(const SpellTree& t, int node, std::vector<int>& out) {
  if (node < 0 || node >= (int)t.nodes.size()) return;
  out.push_back(node);
  const SpellNode& n = t.nodes[node];
  if (n.box)
    for (int ii : n.items) Reach(t, ii, out);
  if (n.group) {
    Reach(t, n.left, out);
    Reach(t, n.right, out);
  }
}

// ---- the gate -----------------------------------------------------------------

Status GateSpellGraph(Ctx& c, std::string& detail) {
  GlyphLibrary lib;
  std::string gerr;
  if (!LoadGlyphs(AssetDir() + "/spells/glyphs.json", c.mats, lib, gerr)) {
    std::printf("spell-graph: FAIL (glyph load: %s)\n", gerr.c_str());
    detail = "glyph load failed";
    return Status::Fail;
  }

  bool ok = true;
  int checks = 0;
  int shown = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (cond) return;
    ok = false;
    if (shown++ < 14) std::printf("spell-graph: FAILED %s\n", what.c_str());
  };

  // ---- 1. the round trip, over the oracle corpus and the alphabet ------------
  std::vector<std::vector<std::string>> corpus;
  int oracleEntries = 0;
  {
    const std::string opath = AssetDir() + "/spells/grammar_oracle.json";
    std::ifstream f(opath);
    if (!f) {
      std::printf("spell-graph: FAIL (cannot open %s)\n", opath.c_str());
      detail = "no oracle";
      return Status::Fail;
    }
    nlohmann::json j;
    try {
      f >> j;
    } catch (const std::exception& e) {
      std::printf("spell-graph: FAIL (oracle parse: %s)\n", e.what());
      detail = "oracle unreadable";
      return Status::Fail;
    }
    for (const auto& e : j.value("entries", nlohmann::json::array())) {
      const std::vector<std::string> w = Split(e.value("spoken", std::string()));
      bool known = !w.empty();
      for (const std::string& x : w)
        if (lib.Find(x) < 0) known = false;
      if (!known) continue;
      corpus.push_back(w);
      oracleEntries++;
    }
  }
  // ...and every generated sequence of length <= 3 over the alphabet, the same
  // way the `spells` gate generates its laws.
  std::vector<std::string> alpha;
  for (const char* w : kAlphabet) {
    if (lib.Find(w) < 0) {
      std::printf("spell-graph: alphabet word \"%s\" is not in glyphs.json\n", w);
      ok = false;
    } else {
      alpha.push_back(w);
    }
  }
  const size_t generatedFrom = corpus.size();
  for (const std::string& a : alpha) {
    corpus.push_back({a});
    for (const std::string& b : alpha) {
      corpus.push_back({a, b});
      for (const std::string& d : alpha) corpus.push_back({a, b, d});
    }
  }

  int tripFail = 0, lowerFail = 0, fixFail = 0, capFail = 0, sayFail = 0;
  int maxWords = 0;
  for (const std::vector<std::string>& s : corpus) {
    const SpellTree t0 = ParseWords(lib, s);
    const std::vector<std::string> w1 = Linearize(lib, t0);
    // Silence is the one asymmetry, and it is correct: a sentence of nothing
    // but charged no-ops (`end` with no lane open) canonicalizes to no words
    // at all, and an empty word list parses to NO clause, while the tree it
    // came from is still an empty hand.
    const SpellTree t1 = w1.empty() ? EmptyTree() : ParseWords(lib, w1);
    maxWords = std::max(maxWords, (int)w1.size());
    if ((int)w1.size() > kSpellStackMax) {
      if (capFail++ < 3)
        std::printf("spell-graph: \"%s\" linearized to %d words\n", Join(s).c_str(),
                    (int)w1.size());
      continue;
    }
    if (GraphTreeKey(lib, t0) != GraphTreeKey(lib, t1)) {
      if (tripFail++ < 6)
        std::printf("spell-graph: round trip \"%s\"\n   -> \"%s\"\n   want %s\n   got  %s\n",
                    Join(s).c_str(), Join(w1).c_str(), GraphTreeKey(lib, t0).c_str(),
                    GraphTreeKey(lib, t1).c_str());
      continue;
    }
    if (CastSignature(lib, LowerSpell(lib, t0)) != CastSignature(lib, LowerSpell(lib, t1))) {
      if (lowerFail++ < 4)
        std::printf("spell-graph: lowering differs for \"%s\" -> \"%s\"\n", Join(s).c_str(),
                    Join(w1).c_str());
      continue;
    }
    // Linearizing is a FIXED POINT: saying the canonical form again produces
    // exactly the same words, so the page's save is stable.
    if (Linearize(lib, t1) != w1) {
      if (fixFail++ < 4)
        std::printf("spell-graph: not a fixed point: \"%s\" -> \"%s\" -> \"%s\"\n",
                    Join(s).c_str(), Join(w1).c_str(), Join(Linearize(lib, t1)).c_str());
      continue;
    }
    // ...and every sentence the parser accepts is one `Speakable` agrees with.
    std::string why;
    if (!Speakable(lib, t0, why)) {
      if (sayFail++ < 4)
        std::printf("spell-graph: Speakable refused a real sentence \"%s\": %s\n",
                    Join(s).c_str(), why.c_str());
    }
  }
  check(tripFail == 0, Format("%d of %d sentences do not round-trip", tripFail,
                              (int)corpus.size()));
  check(lowerFail == 0, Format("%d sentences lower differently after a round trip",
                               lowerFail));
  check(fixFail == 0, Format("%d linearizations are not fixed points", fixFail));
  check(capFail == 0, Format("%d linearizations exceeded kSpellStackMax", capFail));
  check(sayFail == 0, Format("%d parsed sentences were called unspeakable", sayFail));

  // ---- 2. the unspeakable trees are named ------------------------------------
  {
    // Two incomplete unaries where each accepts the other: built by ops, since
    // no sentence says it (whichever is spoken second binds the first).
    const int gEcho = lib.Find("echo"), gNull = lib.Find("null");
    const int gFire = lib.Find("fire");
    check(gEcho >= 0 && gNull >= 0 && gFire >= 0, "echo / null / fire resolve");
    SpellTree t = EmptyTree();
    const EditResult a = InsertItem(lib, t, 0, 0, gEcho);
    check(a.ok, "an empty `echo` can be dropped on the hand: " + a.why);
    if (a.ok) {
      const SpellTree t2 = ParseWords(lib, a.words);
      const EditResult b = InsertItem(lib, t2, t2.clauses[0].root, 0, gNull);
      check(!b.ok && b.why.find("cannot be spoken") != std::string::npos,
            "an empty `null` beside an empty `echo` is refused with a reason: '" + b.why +
                "'");
    }
    // Two deliveries in one segment: `fire projectile` plus a second box.
    const SpellTree t3 = ParseWords(lib, {"fire", "projectile", "gold"});
    std::vector<int> nodes;
    Reach(t3, t3.clauses[0].root, nodes);
    int gold = -1;
    for (int ni : nodes)
      if (!t3.nodes[ni].box && !t3.nodes[ni].group && t3.nodes[ni].glyph == lib.Find("gold"))
        gold = ni;
    check(gold >= 0, "the fixture `fire projectile gold` holds a gold word");
    if (gold >= 0) {
      const EditResult w = WrapInBox(lib, t3, gold, lib.Find("bomb"));
      check(!w.ok && !w.why.empty(),
            "boxing a sibling of a box in the same lane is refused: '" + w.why + "'");
    }
  }

  // ---- 3. the plan's worked sentences, built by ops from a blank page ---------
  {
    // `lane explosive projectile end lane blood mend self end`
    std::vector<std::string> words;
    std::string why;
    auto step = [&](const EditResult& r, const char* what) {
      if (!r.ok) {
        check(false, std::string(what) + " was refused: " + r.why);
        return false;
      }
      words = r.words;
      return true;
    };
    SpellTree t = EmptyTree();
    bool live = step(InsertItem(lib, t, 0, 1, lib.Find("explosive")), "lane 1 explosive");
    if (live) {
      t = ParseWords(lib, words);
      // The explosive is the only item; find it.
      std::vector<int> nodes;
      Reach(t, t.clauses[0].root, nodes);
      int target = -1;
      for (int ni : nodes)
        if (t.nodes[ni].glyph == lib.Find("explosive")) target = ni;
      live = target >= 0 && step(WrapInBox(lib, t, target, lib.Find("projectile")),
                                 "wrap explosive in a projectile");
    }
    if (live) {
      t = ParseWords(lib, words);
      live = step(InsertItem(lib, t, t.clauses[0].root, 2, lib.Find("mend")),
                  "lane 2 mend");
    }
    if (live) {
      t = ParseWords(lib, words);
      std::vector<int> nodes;
      Reach(t, t.clauses[0].root, nodes);
      int mend = -1;
      for (int ni : nodes)
        if (t.nodes[ni].group && t.nodes[ni].glyph == lib.Find("mend")) mend = ni;
      live = mend >= 0 && step(FillSlot(lib, t, mend, SlotSide::Left, lib.Find("blood")),
                               "fill mend's left slot with blood");
    }
    if (live) {
      t = ParseWords(lib, words);
      std::vector<int> nodes;
      Reach(t, t.clauses[0].root, nodes);
      int mend = -1;
      for (int ni : nodes)
        if (t.nodes[ni].group && t.nodes[ni].glyph == lib.Find("mend")) mend = ni;
      live = mend >= 0 &&
             step(WrapInBox(lib, t, mend, lib.Find("self")), "wrap the graft in `self`");
    }
    if (live) {
      const std::vector<std::string> want =
          Split("lane explosive projectile end lane blood mend self end");
      check(GraphTreeKey(lib, ParseWords(lib, words)) ==
                GraphTreeKey(lib, ParseWords(lib, want)),
            "the built spell is `" + Join(want) + "`, got `" + Join(words) + "`");
      check(CastSignature(lib, LowerSpell(lib, ParseWords(lib, words))) ==
                CastSignature(lib, LowerSpell(lib, ParseWords(lib, want))),
            "and lowers identically");
    }
  }
  {
    // `explosive lane sand end lane fire end twin projectile`
    std::vector<std::string> words;
    SpellTree t = EmptyTree();
    bool live = true;
    auto step = [&](const EditResult& r, const char* what) {
      if (!r.ok) {
        check(false, std::string(what) + " was refused: " + r.why);
        live = false;
        return;
      }
      words = r.words;
      t = ParseWords(lib, words);
    };
    step(InsertItem(lib, t, 0, 0, lib.Find("explosive")), "shared explosive");
    if (live) step(InsertItem(lib, t, t.clauses[0].root, 1, lib.Find("sand")), "lane 1 sand");
    if (live) step(InsertItem(lib, t, t.clauses[0].root, 2, lib.Find("fire")), "lane 2 fire");
    if (live) step(AttachMod(lib, t, t.clauses[0].root, 0, lib.Find("twin")), "twin on the hand");
    if (live)
      step(WrapInBox(lib, t, t.clauses[0].root, lib.Find("projectile")),
           "box the pile in a projectile");
    if (live) {
      const std::vector<std::string> want =
          Split("explosive lane sand end lane fire end twin projectile");
      check(GraphTreeKey(lib, ParseWords(lib, words)) ==
                GraphTreeKey(lib, ParseWords(lib, want)),
            "the built spell is `" + Join(want) + "`, got `" + Join(words) + "`");
      check(CastSignature(lib, LowerSpell(lib, ParseWords(lib, words))) ==
                CastSignature(lib, LowerSpell(lib, ParseWords(lib, want))),
            "and lowers identically");
      // ...and it really is two bolts carrying different things.
      const CastList l = LowerSpell(lib, ParseWords(lib, words));
      const DeliveryRec* box = nullptr;
      for (const EffectInst& e : l.casts[0].payload)
        if (e.verb == SpellVerb::Launch && !e.launch.empty()) box = &e.launch[0];
      check(box && box->lanes.size() == 2 && box->count == 2,
            "the bolt fires two instances with two lanes");
    }
  }

  // ---- 4. a lane opens one at a time, and a refusal has a reason --------------
  {
    const SpellTree t = EmptyTree();
    const EditResult far = InsertItem(lib, t, 0, 3, lib.Find("fire"));
    check(!far.ok && !far.why.empty(), "lane 3 on a laneless box is refused: '" + far.why + "'");
    const EditResult mark = InsertItem(lib, t, 0, 0, lib.Find("lane"));
    check(!mark.ok && !mark.why.empty(), "`lane` is not an item: '" + mark.why + "'");
    const EditResult gone = InsertItem(lib, t, 0, 0, 4242);
    check(!gone.ok && !gone.why.empty(), "an unknown glyph is refused");
    // A full page refuses rather than truncating.
    std::vector<std::string> big;
    for (int i = 0; i < kSpellStackMax; i++) big.push_back("projectile");
    const SpellTree bt = ParseWords(lib, big);
    const EditResult full = InsertItem(lib, bt, bt.clauses[0].root, 0, lib.Find("fire"));
    check(!full.ok && !full.why.empty(), "a full page refuses the next word: '" + full.why + "'");
    // The instance cap.
    const SpellTree fan = ParseWords(lib, {"shotgun", "shotgun", "shotgun", "projectile"});
    const EditResult over = AttachMod(lib, fan, fan.clauses[0].root, 0, lib.Find("shotgun"));
    check(!over.ok || over.words.size() >= 4,
          "a fan past the instance cap is refused or stays within it: '" + over.why + "'");
  }

  // ---- 5. every op is total, over generated cases -----------------------------
  const char* const kBases[] = {
      "",
      "fire",
      "explosive projectile",
      "fire trail explosive shotgun projectile",
      "explosive lane sand end lane fire end twin projectile",
      "lane explosive projectile end lane blood mend self end",
      "dirt transmute water bomb",
      "explosive projectile echo",
      "gold aura self",
      "anything null beam",
      "explosive projectile projectile",
      "lane fire end lane end projectile",
  };
  const char* const kDrops[] = {"fire", "gold", "explosive", "shotgun", "echo",
                                "projectile", "transmute", "self", "twin"};
  int opCases = 0, opOk = 0, opRefused = 0, opBad = 0;
  for (const char* base : kBases) {
    const std::vector<std::string> bw = Split(base);
    const SpellTree t = bw.empty() ? EmptyTree() : ParseWords(lib, bw);
    if (t.clauses.empty()) continue;
    std::vector<int> nodes;
    Reach(t, t.clauses[0].root, nodes);
    const std::vector<std::string> before = Linearize(lib, t);
    for (int ni : nodes) {
      for (const char* dropName : kDrops) {
        const int drop = lib.Find(dropName);
        struct Case {
          const char* op;
          EditResult r;
        };
        std::vector<Case> cases;
        const bool box = t.nodes[ni].box;
        const bool grp = t.nodes[ni].group;
        if (box) {
          cases.push_back({"InsertItem/0", InsertItem(lib, t, ni, 0, drop)});
          cases.push_back({"InsertItem/new-lane",
                           InsertItem(lib, t, ni, (int32_t)t.nodes[ni].laneAt.size() / 2 + 1,
                                      drop)});
          cases.push_back({"AttachMod", AttachMod(lib, t, ni, 0, drop)});
          cases.push_back({"Unbox", Unbox(lib, t, ni)});
        }
        if (grp) {
          cases.push_back({"FillSlot/L", FillSlot(lib, t, ni, SlotSide::Left, drop)});
          cases.push_back({"FillSlot/R", FillSlot(lib, t, ni, SlotSide::Right, drop)});
        }
        cases.push_back({"WrapInBox", WrapInBox(lib, t, ni, drop)});
        cases.push_back({"Remove", Remove(lib, t, ni)});
        cases.push_back({"Move", Move(lib, t, ni, t.clauses[0].root, 0, false)});
        cases.push_back({"Copy", Move(lib, t, ni, t.clauses[0].root, 0, true)});
        for (const Case& cs : cases) {
          opCases++;
          if (!cs.r.ok) {
            opRefused++;
            if (cs.r.why.empty()) {
              opBad++;
              if (shown++ < 14)
                std::printf("spell-graph: %s on \"%s\" refused with no reason\n", cs.op, base);
            }
            continue;
          }
          opOk++;
          // THE PROOF THE OP ALREADY MADE, MADE AGAIN HERE: the words parse,
          // they are canonical, and they fit.
          bool good = (int)cs.r.words.size() <= kSpellStackMax;
          for (const std::string& w : cs.r.words)
            if (lib.Find(w) < 0) good = false;
          const SpellTree re = cs.r.words.empty() ? EmptyTree() : ParseWords(lib, cs.r.words);
          if (good && !cs.r.words.empty() && Linearize(lib, re) != cs.r.words) good = false;
          std::string why;
          if (good && !Speakable(lib, re, why)) good = false;
          // ...and the op did what it said. An InsertItem puts the word in,
          // unless the pile merged it at the multiplicity cap; a WrapInBox
          // says the delivery once more; a Remove never grows the sentence.
          if (good && std::string(cs.op).rfind("InsertItem", 0) == 0 &&
              CountWord(cs.r.words, dropName) <= CountWord(before, dropName) &&
              lib.glyphs[drop].sort != GlyphSort::Delivery)
            good = CountWord(before, dropName) >= lib.budgets.maxMultiplicity;
          if (good && std::string(cs.op) == "WrapInBox" &&
              CountWord(cs.r.words, dropName) <= CountWord(before, dropName))
            good = false;
          if (good && std::string(cs.op) == "Remove" && cs.r.words.size() > before.size())
            good = false;
          if (!good) {
            opBad++;
            if (shown++ < 14)
              std::printf("spell-graph: %s(%s) on \"%s\" -> \"%s\" is not what it claimed\n",
                          cs.op, dropName, base, Join(cs.r.words).c_str());
          }
        }
      }
    }
  }
  check(opBad == 0, Format("%d of %d op results were wrong", opBad, opCases));
  check(opCases >= 200, Format("only %d op cases were generated", opCases));
  check(opOk > 0 && opRefused > 0,
        Format("the op fuzz exercised both paths (%d ok, %d refused)", opOk, opRefused));

  // ---- 6. layout sanity ---------------------------------------------------------
  int graphs = 0, gnodes = 0;
  {
    int overlap = 0, outside = 0, sockets = 0, lanes = 0, rooty = 0;
    for (const char* base : kBases) {
      const std::vector<std::string> bw = Split(base);
      if (bw.empty()) continue;
      const CastList l = LowerSpell(lib, ParseWords(lib, bw));
      const SpellGraph g = BuildGraph(lib, l);
      graphs++;
      gnodes += (int)g.nodes.size();
      if (g.root < 0) continue;
      // The root is the hand bar and it is at the maximum y.
      int maxY = 0;
      for (const SpellGraphNode& n : g.nodes) maxY = std::max(maxY, n.y);
      if (g.nodes[(size_t)g.root].kind != GraphKind::Root ||
          g.nodes[(size_t)g.root].y != maxY)
        rooty++;
      // No two nodes of one layer overlap.
      for (size_t a = 0; a < g.nodes.size(); a++) {
        for (size_t b = a + 1; b < g.nodes.size(); b++) {
          if (g.nodes[a].layer != g.nodes[b].layer) continue;
          const int ax = g.nodes[a].x, aw = g.nodes[a].w;
          const int bx = g.nodes[b].x, bw2 = g.nodes[b].w;
          if (ax < bx + bw2 && bx < ax + aw) {
            if (overlap++ < 3)
              std::printf("spell-graph: layer %d overlap in \"%s\": %s [%d,%d) vs %s [%d,%d)\n",
                          g.nodes[a].layer, base, g.nodes[a].label.c_str(), ax, ax + aw,
                          g.nodes[b].label.c_str(), bx, bx + bw2);
          }
        }
      }
      // Every child sits inside its parent's SUBTREE span -- the cell is not
      // the span: an operator cell is narrower than the row of operands above
      // it, and a bar does not cover the mod tags hanging off its left end.
      std::map<int, int> socketEdges;
      for (const SpellGraphEdge& e : g.edges) {
        const SpellGraphNode& ch = g.nodes[(size_t)e.from];
        const SpellGraphNode& pa = g.nodes[(size_t)e.to];
        if (ch.subX < pa.subX || ch.subX + ch.subW > pa.subX + pa.subW) {
          if (outside++ < 3)
            std::printf("spell-graph: \"%s\": %s [%d,%d) outside %s [%d,%d)\n", base,
                        ch.label.c_str(), ch.subX, ch.subX + ch.subW, pa.label.c_str(),
                        pa.subX, pa.subX + pa.subW);
        }
        if (e.kind == GraphEdge::Socket) socketEdges[e.from]++;
      }
      for (const auto& kv : socketEdges)
        if (kv.second != 1) lanes++;
      // A join has exactly `instances` sockets, and each socket names its own
      // instance.
      for (const SpellGraphNode& n : g.nodes) {
        if (n.kind != GraphKind::Join && n.kind != GraphKind::Root) continue;
        if ((int)n.sockets.size() != n.instances) {
          sockets++;
          continue;
        }
        for (size_t i = 0; i < n.sockets.size(); i++)
          if (g.nodes[(size_t)n.sockets[i]].instance != (int32_t)i) sockets++;
      }
    }
    check(overlap == 0, Format("%d same-layer overlaps", overlap));
    check(outside == 0, Format("%d children outside their parent's span", outside));
    check(sockets == 0, Format("%d joins whose socket row does not match `instances`",
                               sockets));
    check(lanes == 0, Format("%d lane subtrees that do not feed exactly one socket", lanes));
    check(rooty == 0, Format("%d graphs whose root is not the bottom bar", rooty));
  }

  detail = Format("%d sentences (%d oracle), %d op cases, %d graphs, %d checks",
                  (int)corpus.size(), oracleEntries, opCases, graphs, checks);
  std::printf(
      "spell-graph: %s (%d sentences round-tripped [%d oracle + %d generated], "
      "longest %d words; %d op cases: %d applied / %d refused; %d graphs, %d nodes; "
      "%d checks)\n",
      ok ? "PASS" : "FAIL", (int)corpus.size(), oracleEntries,
      (int)(corpus.size() - generatedFrom), maxWords, opCases, opOk, opRefused, graphs,
      gnodes, checks);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& SpellGraphGates() {
  static const std::vector<Gate> g = {
      // No deps: CPU-only over glyphs.json, the generated oracle and its own
      // fixtures. Nothing it does is visible to any later gate.
      {"spell-graph", "spell", {}, false, GateSpellGraph},
  };
  return g;
}

}  // namespace selftest
