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
    // ...and `shotgun` inside a lane is THAT LANE'S bead (2026-09-22). It used
    // to be record-wide wherever it was spoken, so it drew on the trunk however
    // it was said; a count now splits the scope it was spoken in, so the bead
    // is over the branch it splits and the drawing and the cast agree again.
    {
      const CastList cl = LowerSpell(lib, ParseWords(lib, Split("fire lane shotgun end projectile")));
      const SpellGraph sg = BuildGraph(lib, cl);
      int wide = 0, perLane = 0;
      for (const SpellGraphNode& n : sg.nodes) {
        if (n.kind != GraphKind::ModTag || n.label != "shotgun") continue;
        if (n.instance < 0) wide++;
        else perLane++;
      }
      check(perLane >= 1 && wide == 0,
            Format("`shotgun` spoken inside a lane draws on that lane (%d wide, %d per-lane)",
                   wide, perLane));
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
      // branch.
      const bool hand = g.nodes[(size_t)cs[0]].kind == GraphKind::Root;
      const int want = hand ? 1 : g.nodes[(size_t)cs[0]].instances;
      int owners = 0;
      const int lyr = g.nodes[(size_t)cs[0]].layer;
      std::vector<int> seen;
      bool ok = (int)cs.size() == want;
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
        // ...and the cells name branches 0..N-1, once each.
        std::sort(seen.begin(), seen.end());
        for (size_t i = 0; i < seen.size(); i++)
          if (seen[i] != (int)i) ok = false;
      }
      if (!ok && cellBad++ < 6 && shownLayout++ < 12)
        std::printf("spell-graph: \"%s\": tree node %d drew %d cells (%d owners), "
                    "want %d\n", what.c_str(), kv.first, (int)cs.size(), owners,
                    want);
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
