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
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "game/caster.h"
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

  // ---- 4. a far lane opens the ones between, and a refusal has a reason -------
  {
    const SpellTree t = EmptyTree();
    // EVERY SOCKET IS A TARGET (2026-09-21). Lanes used to open strictly one at
    // a time, which is right when the sockets ARE the lanes — but a count mod
    // gives a box more instances than lanes, so two of `shotgun`'s three
    // sockets refused every drop. The lanes in between open EMPTY, which is
    // what those instances already were, so nothing about the cast moves.
    const EditResult far = InsertItem(lib, t, 0, 3, lib.Find("fire"));
    check(far.ok, "lane 3 on a laneless box opens the two before it: '" + far.why + "'");
    if (far.ok) {
      const SpellTree ft = ParseWords(lib, far.words);
      const int fr = ft.clauses.empty() ? -1 : ft.clauses[0].root;
      check(fr >= 0 && (int)ft.nodes[(size_t)fr].laneAt.size() / 2 == 3,
            "...and the box has three lanes: [" + Join(far.words) + "]");
    }
    const EditResult past =
        InsertItem(lib, t, 0, lib.budgets.maxInstances + 1, lib.Find("fire"));
    check(!past.ok && !past.why.empty(),
          "a lane past the instance cap is still refused: '" + past.why + "'");
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

  // ---- 4b. THE SHOTGUN SOCKET WALK ---------------------------------------------
  //
  // The gesture the player actually makes, end to end, because two separate
  // defects lived in exactly this shape until 2026-09-21 and the fuzz above
  // reached neither:
  //
  //   * `shotgun` gives the bolt THREE sockets over ZERO lanes, and only the
  //     socket for instance 0 took a drop. Instance 0 is the aim and the aim is
  //     drawn in the MIDDLE of the row, so the working order was middle, right,
  //     left, and nothing in the drawing said so.
  //   * A mod that did land was stripped into the box's shared bead row
  //     REGARDLESS of its lane, so two mods on two different instances drew as
  //     one indistinguishable centred row.
  //
  // So: drop `heavy` on the LEFT socket first (instance 2, the one that always
  // refused), then the middle, then the right; all three must land, and each
  // bead must end up drawn inside the column of the socket it edits.
  {
    const int32_t order[3] = {2, 0, 1};
    std::vector<std::string> words = Split("fire shotgun projectile");
    bool walkOk = true;
    std::string why;
    auto barOf = [&](const SpellGraph& sg) {
      int bar = -1;
      for (size_t i = 0; i < sg.nodes.size(); i++)
        if (sg.nodes[i].kind == GraphKind::Join && sg.nodes[i].primary < 0)
        bar = (int)i;
      return bar;
    };
    for (int step = 0; step < 3 && walkOk; step++) {
      const CastList cl = LowerSpell(lib, ParseWords(lib, words));
      const SpellGraph sg = BuildGraph(lib, cl);
      const int bar = barOf(sg);
      if (bar < 0 || sg.nodes[(size_t)bar].sockets.size() != 3) {
        walkOk = false;
        why = "the bolt does not have three sockets";
        break;
      }
      // Exactly what the canvas computes for a socket drop.
      const SpellGraphNode& sk =
          sg.nodes[(size_t)sg.nodes[(size_t)bar].sockets[(size_t)order[step]]];
      const int32_t lane = sk.lane > 0 ? sk.lane : sk.instance + 1;
      const EditResult r =
          AttachMod(lib, cl.tree, sg.nodes[(size_t)bar].treeNode, lane, lib.Find("heavy"));
      if (!r.ok) {
        walkOk = false;
        why = "socket for instance " + std::to_string(order[step]) + ": " + r.why;
        break;
      }
      words = r.words;
    }
    check(walkOk, "a mod lands on all three shotgun sockets, left one first (" + why + ")");
    if (walkOk) {
      const CastList cl = LowerSpell(lib, ParseWords(lib, words));
      const SpellGraph sg = BuildGraph(lib, cl);
      const int bar = barOf(sg);
      int over = 0;
      if (bar >= 0)
        for (int si : sg.nodes[(size_t)bar].sockets) {
          const SpellGraphNode& sk = sg.nodes[(size_t)si];
          for (const SpellGraphNode& n : sg.nodes)
            // ABOVE the socket (2026-09-22). The stack used to run items / bus
            // / lane items / lane beads / sockets / bar, which put a lane's
            // bead UNDER the pip it edits. Now the split is below the socket
            // row and everything above the row belongs to one branch each, so a
            // lane's bead is on the neck the branch RISES through: items / bus
            // / record-wide beads / split / sockets / lane beads / lane items /
            // one delivery cell per branch.
            if (n.kind == GraphKind::ModTag && n.instance == sk.instance &&
                n.layer > sk.layer && n.x >= sk.x && n.x + n.w <= sk.x + sk.w)
              over++;
        }
      check(over == 3, Format("%d of 3 lane beads are drawn over the socket they edit "
                              "in [%s]", over, Join(words).c_str()));
    }
    // ...and `shotgun` inside a lane is THAT BRANCH'S JUNCTION (2026-09-22). It
    // used to be record-wide wherever it was spoken, so it drew on the trunk
    // however it was said; a count now splits the scope it was spoken in. And
    // since the sub-fan is drawn, that count is not a bead at all any more — it
    // IS the branch's junction, wearing its word, exactly as the record-wide one
    // is the box's.
    {
      const CastList cl = LowerSpell(lib, ParseWords(lib, Split("fire lane shotgun end projectile")));
      const SpellGraph sg = BuildGraph(lib, cl);
      int wide = 0, perLane = 0, beads = 0;
      for (const SpellGraphNode& n : sg.nodes) {
        if (n.label != "shotgun") continue;
        if (n.kind == GraphKind::ModTag) {
          beads++;
          continue;
        }
        if (n.kind != GraphKind::Split) continue;
        if (n.instance < 0) wide++;
        else perLane++;
      }
      check(perLane == 1 && wide == 0 && beads == 0,
            Format("`shotgun` inside a lane is that branch's junction, not a bead "
                   "and not on the trunk (%d wide, %d per-lane, %d beads)",
                   wide, perLane, beads));
    }
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
      // DEEP, AND FANNED DEEP. A `shotgun` on the INNERMOST box of a stack of
      // boxes is the shape the hand-written bases above never reached: the fan
      // it opens has to fit in layers the boxes above it have already spent.
      "fire shotgun projectile projectile",
      "fire shotgun projectile projectile projectile",
      "fire projectile shotgun projectile projectile",
      "fire projectile projectile shotgun projectile",
      "explosive lane fire end lane gold end shotgun twin projectile projectile",
      "gold transmute fire shotgun projectile bomb",
      "fire trail explosive shotgun projectile echo projectile",
      // THE REPORTED SENTENCE, and the target picture: a split whose branches
      // each carry a shared payload, and one that carries its own as well.
      "sand shotgun projectile",
      "sand gust shotgun projectile",
  };
  const char* const kDrops[] = {"fire", "gold", "explosive", "shotgun", "echo",
                                "projectile", "transmute", "self", "twin"};
  // ---- the layout law, as a function --------------------------------------------
  //
  // Stated once and applied twice: to the hand-written bases of section 6, and
  // to EVERY sentence the op fuzz below produces. The bases are shallow by
  // construction - somebody had to type them - and the shape that broke the
  // page (2026-09-22) only exists a few edits in: a `shotgun` attached to a box
  // that is itself deep inside another box fans three sockets into layers its
  // ancestors have already spent, and the fan lands UNDER the drawing instead
  // of in front of it. A law only the shallow corpus is held to is a law that
  // ends at the depth its author happened to type.
  int graphs = 0, gnodes = 0;
  int overlap = 0, outside = 0, sockets = 0, lanes = 0, rooty = 0, badEdge = 0;
  int fanBad = 0, axisBad = 0, cellBad = 0, wideBad = 0;
  int shownLayout = 0;
  auto LayoutLaw = [&](const std::vector<std::string>& words, const std::string& what) {
    if (words.empty()) return;
    const CastList l = LowerSpell(lib, ParseWords(lib, words));
    const SpellGraph g = BuildGraph(lib, l);
    graphs++;
    gnodes += (int)g.nodes.size();
    if (g.root < 0) return;
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
          if (overlap++ < 6 && shownLayout++ < 12)
            std::printf("spell-graph: layer %d overlap in \"%s\": %s [%d,%d) vs %s [%d,%d)\n",
                        g.nodes[a].layer, what.c_str(), g.nodes[a].label.c_str(), ax,
                        ax + aw, g.nodes[b].label.c_str(), bx, bx + bw2);
        }
      }
    }
    // Every child sits inside its parent's SUBTREE span -- the cell is not
    // the span: an operator cell is narrower than the row of operands above
    // it, and a bar is narrower than a mod bead row wider than the bar.
    std::map<int, int> socketEdges;
    for (const SpellGraphEdge& e : g.edges) {
      // AN EDGE THAT LEADS NOWHERE is the other half of the same bug: the
      // stages leave `to == -1` on a bead until the bar exists, and the patch
      // that fills it in has to reach that box's beads and no others.
      if (e.from < 0 || e.from >= (int)g.nodes.size() || e.to < 0 ||
          e.to >= (int)g.nodes.size()) {
        if (badEdge++ < 6 && shownLayout++ < 12)
          std::printf("spell-graph: \"%s\": edge %d -> %d leads nowhere\n", what.c_str(),
                      e.from, e.to);
        continue;
      }
      const SpellGraphNode& ch = g.nodes[(size_t)e.from];
      const SpellGraphNode& pa = g.nodes[(size_t)e.to];
      if (ch.subX < pa.subX || ch.subX + ch.subW > pa.subX + pa.subW) {
        if (outside++ < 6 && shownLayout++ < 12)
          std::printf("spell-graph: \"%s\": %s [%d,%d) outside %s [%d,%d)\n", what.c_str(),
                      ch.label.c_str(), ch.subX, ch.subX + ch.subW, pa.label.c_str(),
                      pa.subX, pa.subX + pa.subW);
      }
      if (e.kind == GraphEdge::Socket) socketEdges[e.from]++;
    }
    for (const auto& kv : socketEdges)
      if (kv.second != 1) lanes++;
    // A join has exactly `instances` sockets, and each socket names its own
    // instance. SOCKETS ARE BRANCHES, NEVER BOLTS: a branch that splits again
    // has its own junction and its own sockets one level in, and this row still
    // counts the branches of THIS box. Scoped to the primary, because a split
    // delivery is drawn once per branch and a copy owns no socket row.
    for (const SpellGraphNode& n : g.nodes) {
      if (n.kind != GraphKind::Join && n.kind != GraphKind::Root) continue;
      if (n.primary >= 0) continue;
      // ...WHEN THE ROW IS DRAWN AT ALL (2026-09-22). A box with one instance
      // and no lane has nothing a socket row could say — one pip over one bolt
      // — so it draws none, and the payload runs straight into the cell. The
      // law is then: no row, or a full one. A row that is drawn and SHORT is
      // still a word that vanished.
      if (n.sockets.empty()) {
        if (n.instances > 1 || n.laneCount > 0) sockets++;
        continue;
      }
      if ((int)n.sockets.size() != n.instances) {
        sockets++;
        continue;
      }
      for (size_t i = 0; i < n.sockets.size(); i++)
        if (g.nodes[(size_t)n.sockets[i]].instance != (int32_t)i) sockets++;
    }

    // ---- THE SPLIT LAWS (2026-09-22) ------------------------------------------
    //
    // The fan used to run `socket -> bar` with the socket row hard against the
    // bar, so on a spoken delivery - whose cell is the TOP of its span - the
    // three strokes left the cell going DOWN and the drawing said "the
    // projectile splits", pointing backwards. These four laws are the statement
    // that a split is a thing of its own and that it opens forward in both
    // orders, and they run over every graph the fuzz builds.

    // (1) EVERY FAN DIVERGES UPWARD OUT OF A JUNCTION. A `Fan` edge runs from a
    // socket to the Split one band below it, and the junction stands on the
    // box's axis. One statement; it covers the `hand` and the spoken box.
    for (const SpellGraphEdge& e : g.edges) {
      if (e.kind != GraphEdge::Fan) continue;
      if (e.from < 0 || e.to < 0 || e.from >= (int)g.nodes.size() ||
          e.to >= (int)g.nodes.size())
        continue;
      const SpellGraphNode& sk = g.nodes[(size_t)e.from];
      const SpellGraphNode& sp = g.nodes[(size_t)e.to];
      const bool ok = sk.kind == GraphKind::Socket &&
                      (sp.kind == GraphKind::Split
                           ? sk.layer == sp.layer + 1
                           // An unsplit box has no junction and one socket; its
                           // pip still sits on the cell's own band.
                           : sp.instances == 1);
      if (!ok && fanBad++ < 6 && shownLayout++ < 12)
        std::printf("spell-graph: \"%s\": fan %s(layer %d) -> %s(kind %d, layer %d)\n",
                    what.c_str(), sk.label.c_str(), sk.layer, sp.label.c_str(),
                    (int)sp.kind, sp.layer);
    }
    // (2) A JUNCTION IS ON THE AXIS OF THE BOX IT SPLITS.
    for (const SpellGraphNode& sp : g.nodes) {
      if (sp.kind != GraphKind::Split) continue;
      if (std::abs((sp.subX + sp.subW / 2) - (sp.x + sp.w / 2)) > kGraphSplitW &&
          axisBad++ < 6 && shownLayout++ < 12)
        std::printf("spell-graph: \"%s\": junction at %d is off the axis of "
                    "[%d,%d)\n", what.c_str(), sp.x + sp.w / 2, sp.subX,
                    sp.subX + sp.subW);
    }
    // (3) ONE RECORD OWNER PER BOX, AND EVERY BRANCH ENDS IN A CELL. A split
    // spoken delivery is drawn once per branch: N cells on ONE layer, their
    // `instance`s a permutation of 0..N-1, exactly one of them the primary, and
    // every copy pointing at it.
    std::map<int, std::vector<int>> cells;
    for (size_t i = 0; i < g.nodes.size(); i++) {
      const SpellGraphNode& c = g.nodes[i];
      if ((c.kind == GraphKind::Join || c.kind == GraphKind::Root) &&
          c.treeNode >= 0)
        cells[c.treeNode].push_back((int)i);
    }
    for (const auto& kv : cells) {
      const std::vector<int>& cs = kv.second;
      // The `hand` is the pedestal the branches have not diverged from yet, so
      // it is ONE cell however wide its fan; a spoken delivery gets one per
      // BOLT — a branch that splits again caps several columns, so the count is
      // `laneSplits` summed, not `instances` (2026-09-22). The fallback is the
      // collapsed drawing, where a wide sub-fan is a tally on one cell: the
      // law is "one per bolt, or one per branch", and nothing in between.
      const bool hand = g.nodes[(size_t)cs[0]].kind == GraphKind::Root;
      const int branches = g.nodes[(size_t)cs[0]].instances;
      const BoxPrice* bp = l.PriceOf(kv.first);
      int bolts = 0;
      if (bp)
        for (int32_t b : bp->laneSplits) bolts += std::max<int32_t>(1, b);
      if (bolts < branches) bolts = branches;
      int owners = 0;
      const int lyr = g.nodes[(size_t)cs[0]].layer;
      std::vector<int> seen;
      bool ok = hand ? (int)cs.size() == 1
                     : ((int)cs.size() == bolts || (int)cs.size() == branches);
      for (int ci : cs) {
        const SpellGraphNode& c = g.nodes[(size_t)ci];
        if (c.primary < 0) owners++;
        else if (c.primary >= (int)g.nodes.size() ||
                 g.nodes[(size_t)c.primary].primary >= 0 ||
                 g.nodes[(size_t)c.primary].treeNode != c.treeNode)
          ok = false;                       // a copy must point at THE owner
        if (c.layer != lyr) ok = false;     // one flat top edge
        if (c.hasPrice && c.primary >= 0) ok = false;   // the record is the owner's
        seen.push_back((int)c.instance);
      }
      if (owners != 1) ok = false;
      if (cs.size() > 1) {
        // ...and every BRANCH is capped: the cells name 0..N-1, each at least
        // once, in non-decreasing order (a branch's bolts are adjacent, which
        // is what keeps a column a column).
        std::vector<int> once = seen;
        std::sort(once.begin(), once.end());
        once.erase(std::unique(once.begin(), once.end()), once.end());
        if ((int)once.size() != branches) ok = false;
        for (size_t i = 0; i < once.size(); i++)
          if (once[i] != (int)i) ok = false;
        for (size_t i = 1; i < seen.size(); i++)
          if (seen[i] < seen[i - 1]) ok = false;
      }
      if (!ok && cellBad++ < 6 && shownLayout++ < 12)
        std::printf("spell-graph: \"%s\": tree node %d drew %d cells (%d owners), "
                    "want %d branches or %d bolts\n", what.c_str(), kv.first,
                    (int)cs.size(), owners, hand ? 1 : branches, hand ? 1 : bolts);
    }
    // (4) THE DRAWING IS BOUNDED. A split branch ends in a cell, so columns are
    // 64 wide now; nesting multiplies them and the page has to stay findable.
    if (g.width > kGraphMaxColumns * (kGraphCell + kGraphGap) &&
        wideBad++ < 6 && shownLayout++ < 12)
      std::printf("spell-graph: \"%s\": the drawing is %d px wide\n", what.c_str(),
                  g.width);
  };

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
          // A mod on a lane PAST the last one, which is the socket gesture on a
          // count-modded box and the one shape the fuzz never reached.
          cases.push_back({"AttachMod/far-lane",
                           AttachMod(lib, t, ni,
                                     (int32_t)t.nodes[ni].laneAt.size() / 2 + 2, drop)});
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
          // ...and the page can DRAW what the op produced. This is where the
          // deep shapes come from: every op result, at whatever depth the op
          // left the tree at, goes through the layout law.
          LayoutLaw(cs.r.words, std::string(cs.op) + "(" + dropName + ") on \"" + base + "\"");
        }
      }
    }
  }
  check(opBad == 0, Format("%d of %d op results were wrong", opBad, opCases));
  check(opCases >= 200, Format("only %d op cases were generated", opCases));
  check(opOk > 0 && opRefused > 0,
        Format("the op fuzz exercised both paths (%d ok, %d refused)", opOk, opRefused));

  // ---- 6. layout sanity ---------------------------------------------------------
  //
  // THE DRAWING READS IN WORD ORDER, BOTTOM TO TOP (2026-09-21). You type
  // `fire projectile`, so `fire` is the lower cell and PROJECTILE stands over
  // it; the implicit `hand` is the one delivery nobody speaks, so it is the
  // pedestal underneath both. Until this day the stack was driven by
  // CONTAINMENT and drew the same sentence exactly upside down.
  {
    const CastList cl = LowerSpell(lib, ParseWords(lib, Split("fire projectile")));
    const SpellGraph sg = BuildGraph(lib, cl);
    int hand = -1, proj = -1, fire = -1;
    for (size_t i = 0; i < sg.nodes.size(); i++) {
      const SpellGraphNode& gn = sg.nodes[i];
      if (gn.kind == GraphKind::Root) hand = (int)i;
      else if (gn.kind == GraphKind::Join) proj = (int)i;
      else if (gn.kind == GraphKind::Word && gn.label == "fire") fire = (int)i;
    }
    // y grows DOWNWARD, so "lower on the page" is a larger y.
    const bool order = hand >= 0 && proj >= 0 && fire >= 0 &&
                       sg.nodes[(size_t)hand].y > sg.nodes[(size_t)fire].y &&
                       sg.nodes[(size_t)fire].y > sg.nodes[(size_t)proj].y;
    check(order, Format("`fire projectile` stacks hand / fire / PROJECTILE upward "
                        "(y %d / %d / %d)",
                        hand >= 0 ? sg.nodes[(size_t)hand].y : -1,
                        fire >= 0 ? sg.nodes[(size_t)fire].y : -1,
                        proj >= 0 ? sg.nodes[(size_t)proj].y : -1));
    // ...and the box hands off to the hand from the FOOT of its own span, not
    // from its cell at the top: `baseLayer` is what keeps that one edge from
    // falling through every cell the box contains.
    check(proj < 0 || fire < 0 ||
              sg.nodes[(size_t)proj].baseLayer <= sg.nodes[(size_t)fire].layer,
          "the projectile box hands off from the bottom of its span");
    // The delivery cell is a CELL, the size of a word, not a slab.
    check(proj < 0 || (sg.nodes[(size_t)proj].w == kGraphCell &&
                       sg.nodes[(size_t)proj].h == kGraphCell),
          Format("a delivery is a %dx%d cell",
                 proj >= 0 ? sg.nodes[(size_t)proj].w : -1,
                 proj >= 0 ? sg.nodes[(size_t)proj].h : -1));
  }
  // A GROUP WHOSE RESULT IS A MOD IS STILL A GROUP (2026-09-22). `fire trail`
  // is one operator with `fire` in its left slot; because its result sort is
  // `mod` it used to be flattened into a bead on the trunk and its operand was
  // never placed, so the page drew `fire trail projectile` as a tag and a
  // socket fan meeting at the bar - two strokes, and no `fire`. The line has to
  // run fire -> trail -> the trunk, with one stroke out of each.
  {
    const CastList cl = LowerSpell(lib, ParseWords(lib, Split("fire trail projectile")));
    const SpellGraph sg = BuildGraph(lib, cl);
    int fire = -1, trail = -1, beads = 0;
    for (size_t i = 0; i < sg.nodes.size(); i++) {
      const SpellGraphNode& n = sg.nodes[i];
      if (n.label == "trail") {
        if (n.kind == GraphKind::ModTag) beads++;
        else trail = (int)i;
      } else if (n.kind == GraphKind::Word && n.label == "fire") {
        fire = (int)i;
      }
    }
    int slotIn = 0, outOfFire = 0, outOfTrail = 0;
    for (const SpellGraphEdge& e : sg.edges) {
      if (e.from == fire) outOfFire++;
      if (e.from == trail) outOfTrail++;
      if (e.from == fire && e.to == trail && e.kind == GraphEdge::Slot) slotIn++;
    }
    check(fire >= 0 && trail >= 0 && beads == 0,
          Format("`fire trail projectile` draws fire and a trail CELL, not a bead "
                 "(fire %d, trail %d, %d beads)", fire, trail, beads));
    check(slotIn == 1 && outOfFire == 1 && outOfTrail == 1,
          Format("fire -> trail -> the trunk is one line (%d slot strokes, %d out of "
                 "fire, %d out of trail)", slotIn, outOfFire, outOfTrail));
    // ...and it stands UNDER its operator, which is where the word it fills the
    // slot with is spoken.
    check(fire < 0 || trail < 0 || sg.nodes[(size_t)fire].y > sg.nodes[(size_t)trail].y,
          "the operand sits under the operator cell it fills");
  }
  {
    for (const char* base : kBases) {
      const std::vector<std::string> bw = Split(base);
      if (bw.empty()) continue;
      LayoutLaw(bw, base);
    }
    check(overlap == 0, Format("%d same-layer overlaps", overlap));
    check(outside == 0, Format("%d children outside their parent's span", outside));
    check(badEdge == 0, Format("%d edges that lead nowhere", badEdge));
    check(sockets == 0, Format("%d joins whose socket row does not match `instances`",
                               sockets));
    check(lanes == 0, Format("%d lane subtrees that do not feed exactly one socket", lanes));
    check(rooty == 0, Format("%d graphs whose root is not the bottom bar", rooty));
    check(fanBad == 0, Format("%d fans that do not diverge out of a junction", fanBad));
    check(axisBad == 0, Format("%d junctions off their box's axis", axisBad));
    check(cellBad == 0, Format("%d boxes whose branches do not each end in a cell",
                               cellBad));
    check(wideBad == 0, Format("%d drawings past the column budget", wideBad));
  }

  // ---- 7. A FAN COMES APART THE WAY IT WENT TOGETHER (2026-09-22) ------------
  //
  // A lane used to outlive its last item, always — the instance was real and
  // the lanes above it are numbered from it. The price was a fan that could not
  // be taken down: build three lanes, delete the three payloads, and the box
  // still spelt `lane end lane end lane end`, three sockets of nothing and six
  // words nobody asked for. Opening a lane is also a SIDE EFFECT of dropping on
  // a far socket, so the leftovers accumulated from gestures the player never
  // made at a lane at all.
  //
  // The rule is the asymmetry: a TRAILING empty lane holds nothing in place, so
  // it goes; an INTERIOR one stays, because closing it would move every bolt
  // above it onto a different socket — and that is `CloseLane`, a gesture.
  {
    auto findNode = [&](const SpellTree& t, const char* name, bool box) {
      std::vector<int> ns;
      if (t.clauses.empty()) return -1;
      Reach(t, t.clauses[0].root, ns);
      for (int ni : ns)
        if (t.nodes[(size_t)ni].box == box && t.nodes[(size_t)ni].glyph == lib.Find(name))
          return ni;
      return -1;
    };
    auto lanesOf = [&](const SpellTree& t, int b) {
      return b < 0 ? -1 : (int)t.nodes[(size_t)b].laneAt.size() / 2;
    };
    // Two lanes on a `fire projectile`, built the way the canvas builds them.
    std::vector<std::string> words;
    SpellTree t = ParseWords(lib, Split("fire projectile"));
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
    step(InsertItem(lib, t, findNode(t, "projectile", true), 1, lib.Find("sand")),
         "lane 1 sand");
    if (live)
      step(InsertItem(lib, t, findNode(t, "projectile", true), 2, lib.Find("gold")),
           "lane 2 gold");
    const SpellTree two = t;             // the two-lane fixture, reused below
    const std::vector<std::string> twoW = words;
    check(!live || lanesOf(t, findNode(t, "projectile", true)) == 2,
          "the fixture has two lanes: [" + Join(words) + "]");
    // Delete the TOP lane's word: the lane goes with it.
    if (live) step(Remove(lib, t, findNode(t, "gold", false)), "remove the gold");
    check(!live || lanesOf(t, findNode(t, "projectile", true)) == 1,
          "deleting the last word of the top lane closes it: [" + Join(words) + "]");
    // Delete the other one: no lanes left, and no marks left on the page.
    if (live) step(Remove(lib, t, findNode(t, "sand", false)), "remove the sand");
    if (live) {
      check(lanesOf(t, findNode(t, "projectile", true)) == 0 &&
                CountWord(words, "lane") == 0 && CountWord(words, "end") == 0,
            "the fan unwinds to no lanes and no marks: [" + Join(words) + "]");
      check(GraphTreeKey(lib, t) ==
                GraphTreeKey(lib, ParseWords(lib, Split("fire projectile"))),
            "...and what is left is the spell it was built from: [" + Join(words) + "]");
    }
    // AN INTERIOR LANE SURVIVES ITS LAST ITEM, because lane 2 is numbered from
    // it: the bolt the player aimed at socket 1 must not slide onto socket 0.
    {
      const int gone = findNode(two, "sand", false);
      const EditResult r = Remove(lib, two, gone);
      check(r.ok, "the interior lane's word can be removed: " + r.why);
      if (r.ok) {
        const SpellTree after = ParseWords(lib, r.words);
        const int b = findNode(after, "projectile", true);
        const int gold = findNode(after, "gold", false);
        check(lanesOf(after, b) == 2 && gold >= 0 &&
                  after.nodes[(size_t)gold].lane == 2,
              "emptying lane 1 of 2 keeps both lanes and leaves the gold in lane 2: [" +
                  Join(r.words) + "]");
        // ...and `CloseLane` is what takes it out, sliding lane 2 down.
        const EditResult c = CloseLane(lib, after, b, 1);
        check(c.ok, "closing the bare lane is allowed: " + c.why);
        if (c.ok) {
          const SpellTree ct = ParseWords(lib, c.words);
          const int cg = findNode(ct, "gold", false);
          check(lanesOf(ct, findNode(ct, "projectile", true)) == 1 && cg >= 0 &&
                    ct.nodes[(size_t)cg].lane == 1,
                "closing lane 1 slides the gold down into it: [" + Join(c.words) + "]");
        }
      }
    }
    // CLOSING A LANE TAKES ITS WORDS WITH IT.
    {
      const int b = findNode(two, "projectile", true);
      const EditResult c = CloseLane(lib, two, b, 2);
      check(c.ok, "a full lane can be closed: " + c.why);
      if (c.ok)
        check(CountWord(c.words, "gold") == 0 && CountWord(c.words, "sand") == 1 &&
                  c.words.size() < twoW.size(),
              "closing lane 2 takes the gold with it: [" + Join(c.words) + "]");
    }
    // The two refusals, each with a reason: the shared pile is not a lane, and
    // an instance a count mod made has no lane to close.
    {
      const SpellTree ft = ParseWords(lib, Split("shotgun projectile"));
      const int fb = findNode(ft, "projectile", true);
      const EditResult zero = CloseLane(lib, ft, fb, 0);
      const EditResult none = CloseLane(lib, ft, fb, 1);
      check(!zero.ok && !zero.why.empty(),
            "lane 0 is the shared pile and is refused: '" + zero.why + "'");
      check(!none.ok && !none.why.empty(),
            "a count mod's socket has no lane to close: '" + none.why + "'");
    }
    // THE SIDE-EFFECT LANES GO BACK TOO. Dropping on the third socket of a
    // laneless box opens the two before it; removing that one word must leave
    // the box exactly as it was found.
    {
      const EditResult far = InsertItem(lib, EmptyTree(), 0, 3, lib.Find("fire"));
      check(far.ok, "the far socket still opens three lanes: " + far.why);
      if (far.ok) {
        const SpellTree ft = ParseWords(lib, far.words);
        const EditResult back = Remove(lib, ft, findNode(ft, "fire", false));
        check(back.ok, "and the word can be taken off again: " + back.why);
        if (back.ok)
          check(back.words.empty() ||
                    (CountWord(back.words, "lane") == 0 && CountWord(back.words, "end") == 0),
                "removing it closes all three: [" + Join(back.words) + "]");
      }
    }
  }

  // ---- 7b. FURNITURE ONLY WHERE IT SAYS SOMETHING (2026-09-22) ---------------
  //
  // From the owner, looking at a page holding the single word `projectile` and
  // finding five bands on it: "what does the horizontal line actually represent
  // here, and why are there two sockets". Nothing, and they were one socket
  // each on two different boxes. A socket row over ONE instance is a pip saying
  // "one" and a bus under ONE item is a rule sharing it with nobody.
  {
    struct Furn { const char* words; int buses; int sockets; const char* why; };
    const Furn kFurn[] = {
        // One word. Two cells, no furniture at all: the hand, and the bolt.
        {"projectile", 0, 0, "one bare delivery"},
        // The smallest real spell: hand, fire, PROJECTILE. Still nothing.
        {"fire projectile", 0, 0, "one item on one bolt"},
        // Two shared items DO collect on something: one bus, on the box that
        // holds them. The hand, holding only the box, still gets none.
        {"sand gust projectile", 1, 0, "two items collect"},
        // A fan: three bolts, so the row says which is which and the bus says
        // what all three carry.
        {"sand shotgun projectile", 1, 3, "three bolts share one item"},
        // A lane distinguishes its instance from the shared pile even at one
        // instance, so the row is drawn and the bus is not.
        {"fire lane sand end projectile", 0, 1, "one lane, one instance"},
    };
    for (const Furn& f : kFurn) {
      const SpellGraph g =
          BuildGraph(lib, LowerSpell(lib, ParseWords(lib, Split(f.words))));
      int buses = 0, socks = 0;
      for (const SpellGraphNode& n : g.nodes) {
        if (n.kind == GraphKind::Bus) buses++;
        if (n.kind == GraphKind::Socket) socks++;
      }
      check(buses == f.buses && socks == f.sockets,
            Format("[%s] draws %d bus / %d sockets, want %d / %d (%s)", f.words,
                   buses, socks, f.buses, f.sockets, f.why));
    }
    // ...and the words are untouched by any of it: furniture is synthesized, so
    // suppressing it cannot change what the page says.
    const SpellTree t = ParseWords(lib, Split("fire projectile"));
    check(Join(Linearize(lib, t)) == "fire projectile",
          "the sentence is the same either way: [" + Join(Linearize(lib, t)) + "]");
  }

  // ---- 7c. THE FAN CASCADES (2026-09-22) ------------------------------------
  //
  // From the owner, on `shotgun lane twin end`: "heres a shotgun into a twin,
  // the twin doesnt split into 2 more. it should just fractal cascade into more
  // and more." It did not: a branch's own split was drawn as a TALLY on the
  // delivery cell that capped it, and the `hand` has no cell per branch, so on
  // the reported sentence the second split was drawn as nothing at all.
  //
  // A split is a split at every depth. A branch that fires more than one bolt
  // gets its own junction and its own socket row inside its column, and its
  // delivery is drawn once per BOLT.
  {
    // (a) The reported sentence, on the hand. Two junctions, 3 + 2 sockets, and
    // the second junction wears the word that opened it.
    {
      const SpellGraph g = BuildGraph(
          lib, LowerSpell(lib, ParseWords(lib, Split("shotgun lane twin end"))));
      int junctions = 0, branchPips = 0, boltPips = 0, twinJunction = 0;
      for (const SpellGraphNode& n : g.nodes) {
        if (n.kind == GraphKind::Split) {
          junctions++;
          if (n.label == "twin" && n.instances == 2) twinJunction++;
        }
        if (n.kind != GraphKind::Socket) continue;
        // A branch pip is in the hand's socket list; a bolt pip is not.
        bool branch = false;
        for (const SpellGraphNode& b : g.nodes)
          for (int si : b.sockets)
            if (si == (int)(&n - g.nodes.data())) branch = true;
        (branch ? branchPips : boltPips)++;
      }
      check(junctions == 2 && twinJunction == 1,
            Format("`shotgun lane twin end` draws TWO junctions and the second "
                   "wears `twin` (%d junctions, %d of them the twin)",
                   junctions, twinJunction));
      check(branchPips == 3 && boltPips == 2,
            Format("...fanning 3 branches and then 2 bolts out of one of them "
                   "(%d branch pips, %d bolt pips)", branchPips, boltPips));
    }
    // (b) A SPOKEN delivery is drawn once per BOLT. Three branches, one of them
    // split in two, is FOUR bolts and four cells - and the price agrees, which
    // is the point: the drawing counts what flies.
    {
      const char* w = "sand shotgun lane twin end projectile";
      const SpellTree t = ParseWords(lib, Split(w));
      const CastList cl = LowerSpell(lib, t);
      const SpellGraph g = BuildGraph(lib, cl);
      int cells = 0;
      for (const SpellGraphNode& n : g.nodes)
        if (n.kind == GraphKind::Join && n.label == "projectile") cells++;
      const BoxPrice* bp =
          t.clauses.empty() ? nullptr : cl.PriceOf(t.clauses[0].root);
      int32_t bolts = 0;
      // The hand's one item is the projectile box; its price is the one with
      // more than one instance.
      for (const BoxPrice& p : cl.boxPrice)
        if (p.instances > 1) bolts = std::max(bolts, p.bolts);
      check(cells == 4 && bolts == 4,
            Format("[%s] draws one cell per BOLT: %d cells for %d bolts", w,
                   cells, (int)bolts));
      (void)bp;
    }
    // (c) IT NESTS. A fan inside a fan by nesting BOXES is the other way depth
    // arrives, and it goes through the same recursion.
    {
      const SpellGraph g = BuildGraph(
          lib, LowerSpell(lib, ParseWords(
                                   lib, Split("fire shotgun projectile twin projectile"))));
      int junctions = 0, inner = 0, outer = 0;
      for (const SpellGraphNode& n : g.nodes) {
        if (n.kind == GraphKind::Split) junctions++;
        if (n.kind != GraphKind::Join) continue;
        if (n.instances == 3) inner++;
        if (n.instances == 2) outer++;
      }
      check(junctions == 2 && inner == 3 && outer == 2,
            Format("a fan feeding a fan draws both (%d junctions, %d inner "
                   "cells, %d outer)", junctions, inner, outer));
    }
    // (d) AND NOTHING WITHOUT A SUB-SPLIT GREW A JUNCTION. The ordinary fan is
    // exactly what it was: one junction, one socket row, one cell per branch.
    {
      const SpellGraph g = BuildGraph(
          lib, LowerSpell(lib, ParseWords(lib, Split("sand shotgun projectile"))));
      int junctions = 0, socks = 0, cells = 0;
      for (const SpellGraphNode& n : g.nodes) {
        if (n.kind == GraphKind::Split) junctions++;
        if (n.kind == GraphKind::Socket) socks++;
        if (n.kind == GraphKind::Join) cells++;
      }
      check(junctions == 1 && socks == 3 && cells == 3,
            Format("a plain fan is untouched (%d junctions, %d sockets, %d "
                   "cells)", junctions, socks, cells));
    }
  }

  // ---- 8. A SPLIT WORD ALWAYS FANS WHAT IT LANDS ON (2026-09-22) -------------
  //
  // From the owner: "if I just drag a single shotgun or twin glyph into an empty
  // spellbook it gets sandwiched with a lane and end insertion which removes the
  // 3 branching nodes it's supposed to make."
  //
  // A socket drop asks for lane `instance + 1`, and a socket with no lane of its
  // own opens one — right for a payload, wrong for the word that MAKES sockets.
  // A blank page draws one bare socket over the hand, so the only gesture there
  // was `lane shotgun end`: one empty instance carrying a split, and no fan.
  {
    auto dropOnSocket = [&](const SpellTree& t, int box, const char* word) {
      // What the canvas asks for when you aim at the LAST socket of a box, read
      // the way `spellgraph_ui.cpp` reads it off the mirror.
      const int32_t lanes = (int32_t)t.nodes[(size_t)box].laneAt.size() / 2;
      const CastList cl = LowerSpell(lib, t);
      const BoxPrice* p = cl.PriceOf(box);
      const int32_t instances = std::max<int32_t>(p ? p->instances : 1, lanes);
      return AttachMod(lib, t, box, instances, lib.Find(word));
    };
    for (const char* word : {"shotgun", "twin"}) {
      const SpellTree blank = EmptyTree();
      const int hand = blank.clauses.empty() ? -1 : blank.clauses[0].root;
      const EditResult r = dropOnSocket(blank, hand, word);
      check(r.ok, Format("`%s` can be dropped on a blank page: ", word) + r.why);
      if (!r.ok) continue;
      check(CountWord(r.words, "lane") == 0 && CountWord(r.words, "end") == 0,
            Format("`%s` on a blank page says no lane and no end: [", word) +
                Join(r.words) + "]");
      // ...and the drawing it makes has one cell per branch, which is the thing
      // the player dragged it in for.
      const SpellTree after = ParseWords(lib, r.words);
      const SpellGraph g = BuildGraph(lib, LowerSpell(lib, after));
      const int want = lib.glyphs[(size_t)lib.Find(word)].amount;
      int socketsDrawn = 0, split = 0;
      for (const SpellGraphNode& n : g.nodes) {
        if (n.kind == GraphKind::Socket) socketsDrawn++;
        if (n.kind == GraphKind::Split) split++;
      }
      check(socketsDrawn == want && split == 1,
            Format("`%s` fans the hand into %d branches out of one junction "
                   "(%d sockets, %d junctions)", word, want, socketsDrawn, split));
    }
    // A TWIN OF A TWIN. The clamp above is right exactly ONCE per box: aim a
    // second count at a socket of a box that already fans and you are asking
    // for a fan INSIDE a fan, contained in that branch — not to fan the trunk
    // twice, which is one split per scope and would do nothing. From the owner:
    // "it also wont let me split a twin into a twin, which should totally be
    // allowed and contained."
    {
      SpellTree t = EmptyTree();
      EditResult first =
          AttachMod(lib, t, t.clauses[0].root, 1, lib.Find("twin"));
      check(first.ok, "the first twin fans the hand: " + first.why);
      if (first.ok) {
        t = ParseWords(lib, first.words);
        // ...aimed at the FIRST socket, which is the aim branch and has no lane
        // of its own yet. (The far socket works too and opens the lanes before
        // it, which is `EVERY SOCKET IS A TARGET` and not this law.)
        const EditResult second =
            AttachMod(lib, t, t.clauses[0].root, 1, lib.Find("twin"));
        check(second.ok, "a twin can be split into a twin: " + second.why);
        if (second.ok) {
          check(CountWord(second.words, "twin") == 2 &&
                    CountWord(second.words, "lane") == 1 &&
                    CountWord(second.words, "end") == 1,
                "...and it is CONTAINED in that branch: [" +
                    Join(second.words) + "]");
          // The branch really fires more than the others: `laneSplits` is what
          // the drawing's per-branch tally reads.
          const SpellTree st = ParseWords(lib, second.words);
          const CastList cl = LowerSpell(lib, st);
          const BoxPrice* p =
              st.clauses.empty() ? nullptr : cl.PriceOf(st.clauses[0].root);
          int split = 0;
          if (p)
            for (int32_t b : p->laneSplits)
              if (b > 1) split++;
          check(split == 1, Format("one of the two branches fires more than one "
                                   "bolt (%d do)", split));
        }
        // Aiming it at the TRUNK, where the split already is, is still the one
        // refusal: that word would be charged and do nothing.
        const EditResult trunk =
            AttachMod(lib, t, t.clauses[0].root, 0, lib.Find("twin"));
        check(!trunk.ok && !trunk.why.empty(),
              "a second twin on the trunk is still refused, with a reason: '" +
                  trunk.why + "'");
      }
    }
    // A LANE THE PLAYER ACTUALLY OPENED still takes one: that is a real
    // sub-split and the page has a bead for it.
    {
      const EditResult lane1 =
          InsertItem(lib, EmptyTree(), 0, 1, lib.Find("fire"));
      check(lane1.ok, "a lane can be opened with a word in it: " + lane1.why);
      if (lane1.ok) {
        const SpellTree t = ParseWords(lib, lane1.words);
        const EditResult r =
            AttachMod(lib, t, t.clauses[0].root, 1, lib.Find("shotgun"));
        check(r.ok && CountWord(r.words, "lane") == 1,
              "a count aimed at a lane the player opened stays in it: [" +
                  Join(r.words) + "] " + r.why);
      }
    }
  }

  // ---- 9. AN EMPTY SLOT IS A HOLE IN THE ROW BELOW (2026-09-22) --------------
  //
  // Also from the owner: a `_ trail` drew its empty left slot as a ring hung off
  // the LEFT EDGE of the cell. An operand belongs UNDER its operator, so the
  // missing one is a cell-sized blank standing in that row with its own stroke
  // into the word it is waiting for.
  {
    const SpellTree t = ParseWords(lib, Split("trail projectile"));
    const SpellGraph g = BuildGraph(lib, LowerSpell(lib, t));
    int hole = -1, op = -1;
    for (size_t i = 0; i < g.nodes.size(); i++) {
      if (g.nodes[i].kind == GraphKind::Hole) hole = (int)i;
      else if (g.nodes[i].kind == GraphKind::Operator) op = (int)i;
    }
    check(hole >= 0 && op >= 0,
          Format("`trail projectile` draws its empty slot as a hole (hole %d, "
                 "operator %d)", hole, op));
    if (hole >= 0 && op >= 0) {
      const SpellGraphNode& h = g.nodes[(size_t)hole];
      const SpellGraphNode& o = g.nodes[(size_t)op];
      // y grows DOWNWARD: UNDERNEATH is a larger y.
      check(h.y > o.y, Format("the hole sits under the cell it belongs to "
                              "(hole y %d, operator y %d)", h.y, o.y));
      check(h.w == kGraphCell && h.h == kGraphCell,
            Format("the hole is a whole cell, not a pip (%dx%d)", h.w, h.h));
      // It is the OPERATOR's slot, not a word: a drop on it is FillSlot on that
      // operator's left side.
      check(h.treeNode == o.treeNode && h.instance == 0,
            Format("the hole names its operator's left slot (node %d vs %d, "
                   "side %d)", h.treeNode, o.treeNode, (int)h.instance));
      int slotIn = 0;
      for (const SpellGraphEdge& e : g.edges)
        if (e.from == hole && e.to == op && e.kind == GraphEdge::Slot) slotIn++;
      check(slotIn == 1, Format("one stroke runs from the hole into its "
                                "operator (%d)", slotIn));
      // ...and filling it makes the hole a word: the same drawing, no blanks.
      const EditResult f =
          FillSlot(lib, t, o.treeNode, SlotSide::Left, lib.Find("fire"));
      check(f.ok, "the hole takes a word: " + f.why);
      if (f.ok) {
        const SpellGraph g2 =
            BuildGraph(lib, LowerSpell(lib, ParseWords(lib, f.words)));
        int holes = 0;
        for (const SpellGraphNode& n : g2.nodes)
          if (n.kind == GraphKind::Hole) holes++;
        check(holes == 0, Format("...and then there is no hole left (%d): [",
                                 holes) + Join(f.words) + "]");
      }
    }
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


// ---- spell-magnitude (docs/PLAN_spell_magnitude.md §2.6) ----------------------
//
// CPU-only, beside `spell-graph`: a word's MAGNITUDE — the number the wheel
// sets on a cell — reaches the lowered cast in the units the plan promises,
// prices convexly, survives the page's serialization (`float@0.5`) through
// every path a page takes (ParseWords / Linearize / ExpandWords / the graph
// op), and changes nothing for a word spoken without one.

// The first effect of `verb` anywhere under `v`, launches and inners included.
const EffectInst* FindVerb(const std::vector<EffectInst>& v, SpellVerb verb) {
  for (const EffectInst& e : v) {
    if (e.verb == verb) return &e;
    if (const EffectInst* x = FindVerb(e.inner, verb)) return x;
    for (const DeliveryRec& d : e.launch)
      for (const SpellLane& ln : d.lanes)
        if (const EffectInst* x = FindVerb(ln.extra, verb)) return x;
  }
  return nullptr;
}

Status GateSpellMagnitude(Ctx& c, std::string& detail) {
  GlyphLibrary lib;
  std::string gerr;
  if (!LoadGlyphs(AssetDir() + "/spells/glyphs.json", c.mats, lib, gerr)) {
    std::printf("spell-magnitude: FAIL (glyph load: %s)\n", gerr.c_str());
    detail = "glyph load failed";
    return Status::Fail;
  }
  bool ok = true;
  int checks = 0, shown = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (cond) return;
    ok = false;
    if (shown++ < 20) std::printf("spell-magnitude: FAILED %s\n", what.c_str());
  };
  auto compile = [&](const std::vector<std::string>& w) {
    return LowerSpell(lib, ParseWords(lib, w));
  };
  // The projectile record a `... projectile` sentence flies with.
  auto flight = [&](const CastList& l) -> const DeliveryRec* {
    if (l.casts.empty()) return nullptr;
    const EffectInst* e = FindVerb(l.casts[0].payload, SpellVerb::Launch);
    return e && !e->launch.empty() ? &e->launch[0] : nullptr;
  };
  const GlyphDef* proj = lib.At(lib.Find("projectile"));
  if (!proj) {
    std::printf("spell-magnitude: FAIL (no projectile glyph)\n");
    return Status::Fail;
  }

  // 1. COMPATIBILITY: `word@1` IS `word`, over every pair of the alphabet -
  // same tree key, same price, same readout, same words back.
  int compat = 0;
  for (const char* a : kAlphabet)
    for (const char* b : kAlphabet) {
      const std::vector<std::string> plain = {a, b, "projectile"};
      const std::vector<std::string> one = {std::string(a) + "@1", std::string(b) + "@1.000",
                                            "projectile"};
      const SpellTree tp = ParseWords(lib, plain), t1 = ParseWords(lib, one);
      const CastList lp = LowerSpell(lib, tp), l1 = LowerSpell(lib, t1);
      const bool same = GraphTreeKey(lib, tp) == GraphTreeKey(lib, t1) &&
                        lp.manaCost == l1.manaCost &&
                        DescribeSpell(lib, lp).text == DescribeSpell(lib, l1).text &&
                        Linearize(lib, tp) == Linearize(lib, t1);
      if (!same) check(false, Format("`%s %s projectile` differs at @1", a, b));
      compat++;
    }

  // 2. WHAT IT SCALES, in the units the plan's table promises.
  {
    const DeliveryRec* h = flight(compile({"float@0.5", "projectile"}));
    const DeliveryRec* f = flight(compile({"float", "projectile"}));
    const DeliveryRec* d = flight(compile({"float@2", "projectile"}));
    check(h && f && d, "float projectiles lower to a flight");
    if (h && f && d) {
      check(f->gravityMille == proj->gravityMille - 1000,
            Format("float: gravity %d, want %d", f->gravityMille, proj->gravityMille - 1000));
      check(h->gravityMille == proj->gravityMille - 500,
            Format("float@0.5: gravity %d, want %d", h->gravityMille, proj->gravityMille - 500));
      check(d->gravityMille == proj->gravityMille - 2000,
            Format("float@2: gravity %d, want %d", d->gravityMille, proj->gravityMille - 2000));
    }
    const DeliveryRec* s15 = flight(compile({"swift@1.5", "projectile"}));
    check(s15 && s15->speedFx == proj->speedFx * 3,
          Format("swift@1.5: speed %d, want %d", s15 ? s15->speedFx : -1, proj->speedFx * 3));
    const DeliveryRec* sl = flight(compile({"slow@2", "projectile"}));
    check(sl && sl->speedFx == proj->speedFx / 4,
          Format("slow@2: speed %d, want %d", sl ? sl->speedFx : -1, proj->speedFx / 4));
    const DeliveryRec* s0 = flight(compile({"swift@0.25", "projectile"}));
    check(s0 && s0->speedFx >= proj->speedFx, "swift never slows: its magnitude floor is x1");
    const DeliveryRec* b3 = flight(compile({"bounce@3", "projectile"}));
    check(b3 && b3->bounces == 3, "bounce@3 bounces three times");
    const DeliveryRec* bh = flight(compile({"bounce@0.5", "projectile"}));
    check(bh && bh->bounces == 1, "bounce is whole units: @0.5 snaps to 1");

    // Tariff follows the world effect: a bigger blast costs more.
    const int32_t e05 = compile({"explosive@0.5", "projectile"}).tariff;
    const int32_t e1 = compile({"explosive", "projectile"}).tariff;
    const int32_t e2 = compile({"explosive@2", "projectile"}).tariff;
    const int32_t ex2 = compile({"explosive", "explosive", "projectile"}).tariff;
    check(e05 < e1 && e1 < e2, Format("explosive tariff %d < %d < %d", e05, e1, e2));
    check(e2 == ex2, Format("explosive@2 prices as explosive x2 (%d vs %d)", e2, ex2));
    const CastList sp = compile({"fire@2", "projectile"});
    const EffectInst* spray =
        sp.casts.empty() ? nullptr : FindVerb(sp.casts[0].payload, SpellVerb::Spray);
    check(spray && spray->mag == 2000 &&
              EffectVolume(lib, *spray) == lib.budgets.sprayVoxels * 2,
          "fire@2 sprays twice the voxels");

    // A sustained mod carries its magnitude to the body.
    const CastList au = compile({"float@0.5", "aura", "self"});
    const EffectInst* su =
        au.casts.empty() ? nullptr : FindVerb(au.casts[0].payload, SpellVerb::Sustain);
    check(su && su->modField == ModField::Gravity && su->modMag == 500,
          "float@0.5 aura carries magnitude 0.5 to the status");
  }

  // 3. THE WORD PRICE IS CONVEX AND MONOTONE for every graded glyph, and at 1
  // it is the authored word cost exactly.
  int graded = 0;
  for (const GlyphDef& g : lib.glyphs) {
    check(MagnitudeWordCost(g.word, kMagOne) == g.word, g.id + ": word cost at 1 moved");
    if (!g.graded) continue;
    graded++;
    int32_t prev = -1, prevDelta = -1;
    // A SIGNED component prices by |magnitude|: walk its non-negative half.
    for (int32_t m = std::max(g.magMin, 0); m <= g.magMax; m += g.magStep) {
      const int32_t w = MagnitudeWordCost(g.word, m);
      if (prev >= 0) {
        check(w >= prev, Format("%s: word cost falls at %d", g.id.c_str(), m));
        // Convex up to the ceil rounding: each step costs at least what the
        // previous one did, less one.
        if (prevDelta >= 0)
          check(w - prev >= prevDelta - 1,
                Format("%s: word cost not convex at %d", g.id.c_str(), m));
        prevDelta = w - prev;
      }
      prev = w;
    }
  }
  for (const char* id : {"shotgun", "twin", "projectile", "lane"}) {
    const GlyphDef* g = lib.At(lib.Find(id));
    check(g && !g->graded, std::string(id) + " must be ungraded");
  }

  // 3b. M2 - ONE SIGNED COMPONENT PER QUANTITY. `lift` at its default is
  // `float`, below zero it is `heavy`; `speed` at its default is `swift` and
  // below 1 it slows; `wind` at its default pushes and at -1 is `implode`. The
  // replaced words still load (old pages) and are hidden from the column.
  {
    const DeliveryRec* fl = flight(compile({"float", "projectile"}));
    const DeliveryRec* li = flight(compile({"lift", "projectile"}));
    const DeliveryRec* hv = flight(compile({"heavy", "projectile"}));
    const DeliveryRec* ln = flight(compile({"lift@-1", "projectile"}));
    const DeliveryRec* lh = flight(compile({"lift@0.5", "projectile"}));
    check(fl && li && fl->gravityMille == li->gravityMille, "lift at its default is float");
    check(hv && ln && hv->gravityMille == ln->gravityMille, "lift@-1 is heavy");
    check(lh && lh->gravityMille == proj->gravityMille - 500, "lift@0.5 takes half a g");
    const DeliveryRec* sw = flight(compile({"swift", "projectile"}));
    const DeliveryRec* sd = flight(compile({"speed", "projectile"}));
    const DeliveryRec* sh = flight(compile({"speed@0.5", "projectile"}));
    check(sw && sd && sw->speedFx == sd->speedFx, "speed at its default is swift");
    check(sh && sh->speedFx == proj->speedFx / 2, "speed@0.5 halves the speed");
    auto windOf = [&](const std::vector<std::string>& w) -> int32_t {
      const CastList l = compile(w);
      if (l.casts.empty()) return 0;
      SpellEmission out;
      ApplySpellEffect(lib, l.casts[0].payload, {0, 0, 0}, {kSpellFxOne, 0, 0}, 1000, out);
      return out.winds.empty() ? 0 : out.winds[0].strengthQ;
    };
    const int32_t wp = windOf({"wind"}), wn = windOf({"wind@-1"}), wi = windOf({"implode"});
    check(wp > 0 && wn < 0 && wn == -wp, Format("wind pushes, wind@-1 pulls (%d, %d)", wp, wn));
    check(wn == wi, Format("wind@-1 is implode (%d vs %d)", wn, wi));
    check(compile({"wind@-1"}).tariff == compile({"wind"}).tariff,
          "a pull costs what the push does");
    for (const char* id : {"float", "heavy", "swift", "slow", "implode"}) {
      const GlyphDef* g = lib.At(lib.Find(id));
      check(g && g->hidden, std::string(id) + " is hidden from the word column");
    }
    for (const char* id : {"lift", "speed", "wind"}) {
      const GlyphDef* g = lib.At(lib.Find(id));
      check(g && !g->hidden && g->graded, std::string(id) + " is offered and graded");
    }
    check(Linearize(lib, ParseWords(lib, {"lift", "projectile"})) ==
              std::vector<std::string>({"lift", "projectile"}),
          "a signed component at its default writes no suffix");
    check(CountWord(Linearize(lib, ParseWords(lib, {"lift@-1.5", "projectile"})), "lift@-1.5") == 1,
          "a negative magnitude round-trips");
  }

  // 4. SERIALIZATION. Round trip, merge rule, ungraded suffix, clamp, snap.
  {
    const std::vector<std::string> w = {"float@0.5", "explosive@2", "projectile"};
    const SpellTree t = ParseWords(lib, w);
    const std::vector<std::string> back = Linearize(lib, t);
    check(CountWord(back, "float@0.5") == 1 && CountWord(back, "explosive@2") == 1,
          "linearize keeps magnitudes: [" + Join(back) + "]");
    check(GraphTreeKey(lib, ParseWords(lib, back)) == GraphTreeKey(lib, t),
          "a magnitude round-trips through words");
    const SpellTree merged = ParseWords(lib, {"float@0.5", "float@0.5", "projectile"});
    const SpellTree apart = ParseWords(lib, {"float@0.5", "float", "projectile"});
    const std::vector<std::string> lm = Linearize(lib, merged), la = Linearize(lib, apart);
    check(CountWord(lm, "float@0.5") == 2, "equal magnitudes merge: [" + Join(lm) + "]");
    check(CountWord(la, "float@0.5") == 1 && CountWord(la, "float") == 1,
          "unequal magnitudes stay two items: [" + Join(la) + "]");
    const DeliveryRec* ap = flight(LowerSpell(lib, apart));
    check(ap && ap->gravityMille == proj->gravityMille - 1500, "float@0.5 + float = -1.5 g");
    check(Linearize(lib, ParseWords(lib, {"shotgun@3", "projectile@2"})) ==
              std::vector<std::string>({"shotgun", "projectile"}),
          "an ungraded word's suffix is dropped");
    check(CountWord(Linearize(lib, ParseWords(lib, {"explosive@99", "projectile"})),
                    "explosive@4") == 1,
          "a magnitude past the range clamps to its max");
    check(CountWord(Linearize(lib, ParseWords(lib, {"explosive@0.3", "projectile"})),
                    "explosive@0.25") == 1,
          "a magnitude off the lattice snaps to the step");
    check(CountWord(Linearize(lib, ParseWords(lib, {"explosive@banana", "projectile"})),
                    "explosive") == 1,
          "a malformed magnitude reads as 1");
    // A grimoire page carries magnitudes through its expansion.
    Grimoire gr;
    GrimoirePage pg;
    pg.name = "mag-test";
    pg.words = {"fire@2", "projectile"};
    gr.pages.push_back(pg);
    const GrimoireExpansion ex = ExpandWords(lib, gr, {"mag-test"}, kSpellStackMax);
    check(ex.spoken.size() == 2 && ex.mags.size() == 2 && ex.mags[0] == 2000 &&
              ex.mags[1] == kMagOne,
          "ExpandWords carries a page's magnitudes");
  }

  // 5. THE PAGE OP: the wheel's SetMagnitude is total and self-proving.
  {
    const SpellTree t = ParseWords(lib, {"explosive", "projectile"});
    int word = -1, box = -1;
    for (size_t i = 0; i < t.nodes.size(); i++) {
      if (t.nodes[i].box && t.nodes[i].glyph >= 0) box = (int)i;
      const GlyphDef* gd = lib.At(t.nodes[i].glyph);
      if (!t.nodes[i].box && !t.nodes[i].group && gd && gd->id == "explosive") word = (int)i;
    }
    const EditResult up = SetMagnitude(lib, t, word, 2000);
    check(up.ok && CountWord(up.words, "explosive@2") == 1,
          "SetMagnitude writes explosive@2: " + (up.ok ? Join(up.words) : up.why));
    const EditResult onBox = SetMagnitude(lib, t, box, 2000);
    check(!onBox.ok && !onBox.why.empty(), "SetMagnitude on a box refuses with a reason");
    const SpellTree top = ParseWords(lib, {"explosive@4", "projectile"});
    int w4 = -1;
    for (size_t i = 0; i < top.nodes.size(); i++)
      if (!top.nodes[i].box && top.nodes[i].mag == 4000) w4 = (int)i;
    const EditResult past = SetMagnitude(lib, top, w4, 4250);
    check(!past.ok && past.why.find("higher") != std::string::npos,
          "SetMagnitude past the max refuses naming the limit: " + past.why);
    // And the graph cell shows it.
    if (up.ok) {
      const SpellGraph g = BuildGraph(lib, LowerSpell(lib, ParseWords(lib, up.words)));
      bool seen = false;
      for (const SpellGraphNode& n : g.nodes)
        if (n.kind == GraphKind::Word && n.mag == 2000 && n.graded &&
            n.magLabel.find("power") != std::string::npos)
          seen = true;
      check(seen, "the graph's explosive cell carries magnitude 2 and reads as power");
    }
  }

  detail = Format("%d compat pairs, %d graded glyphs, %d checks", compat, graded, checks);
  std::printf("spell-magnitude: %s (%d @1 pairs identical, %d graded glyphs priced, %d checks)\n",
              ok ? "PASS" : "FAIL", compat, graded, checks);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& SpellGraphGates() {
  static const std::vector<Gate> g = {
      // No deps: CPU-only over glyphs.json, the generated oracle and its own
      // fixtures. Nothing it does is visible to any later gate.
      {"spell-graph", "spell", {}, false, GateSpellGraph},
      // CPU-only as well: magnitudes, over glyphs.json and its own fixtures.
      {"spell-magnitude", "spell", {}, false, GateSpellMagnitude},
  };
  return g;
}

}  // namespace selftest
