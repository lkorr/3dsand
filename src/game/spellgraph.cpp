#include "game/spellgraph.h"

#include <algorithm>
#include <cstdio>
#include <functional>

// See spellgraph.h for the three things this file owns and why they are
// imgui-free. The comments below are about the NON-OBVIOUS half: why the
// linearizer orders a segment the way it does, and which trees it refuses.

namespace {

// ---- small helpers -----------------------------------------------------------

int RootOf(const SpellTree& t) {
  return t.clauses.empty() ? -1 : t.clauses[0].root;
}

bool IsBox(const SpellTree& t, int n) {
  return n >= 0 && n < (int)t.nodes.size() && t.nodes[n].box;
}
bool IsGroup(const SpellTree& t, int n) {
  return n >= 0 && n < (int)t.nodes.size() && t.nodes[n].group;
}

// The two scope marks are CONTENT (glyphs.json), not constants: a library
// without them cannot spell a lane, which is a refusal and not a crash.
bool FindMarks(const GlyphLibrary& lib, int& laneG, int& endG) {
  laneG = endG = -1;
  for (size_t i = 0; i < lib.glyphs.size(); i++) {
    if (lib.glyphs[i].sort != GlyphSort::Separator) continue;
    if (lib.glyphs[i].scopeClose) {
      if (endG < 0) endG = (int)i;
    } else if (laneG < 0) {
      laneG = (int)i;
    }
  }
  return laneG >= 0 && endG >= 0;
}

// A word only merges with the one before it when the parser would merge them:
// deliveries and scope marks never do (each boxes or opens something of its
// own), so an adjacency between those is always safe.
bool Mergeable(const GlyphLibrary& lib, int glyph) {
  const GlyphDef* g = lib.At(glyph);
  if (!g) return false;
  return g->sort != GlyphSort::Delivery && g->sort != GlyphSort::Separator;
}

// DOES THIS ITEM SPEAK A DELIVERY IN ITS OWN SCOPE? A box's delivery word
// boxes whatever is in the pile at that moment, so everything emitted before
// it in the same segment would be swallowed into it. That is why a segment
// holds at most one such item and it is emitted FIRST.
bool ContainsBox(const SpellTree& t, int node) {
  if (node < 0 || node >= (int)t.nodes.size()) return false;
  const SpellNode& n = t.nodes[node];
  if (n.box) return true;
  if (!n.group) return false;
  return ContainsBox(t, n.left) || ContainsBox(t, n.right);
}

bool EmptyLeft(const GlyphLibrary& lib, const SpellTree& t, int node) {
  if (!IsGroup(t, node)) return false;
  const GlyphDef* g = lib.At(t.nodes[node].glyph);
  return g && g->hasLeft && t.nodes[node].left < 0;
}
bool EmptyRight(const GlyphLibrary& lib, const SpellTree& t, int node) {
  if (!IsGroup(t, node)) return false;
  const GlyphDef* g = lib.At(t.nodes[node].glyph);
  return g && g->hasRight && t.nodes[node].right < 0;
}

// The sort a slot mask would see for a RAW WORD: the parser's right slot only
// ever looks at the next merged word node, so this is the whole test.
bool MaskAccepts(uint8_t mask, GlyphSort s) {
  if (mask == kSortAny) return true;
  return (mask & (uint8_t)(1u << (int)s)) != 0;
}

// ---- the linearizer ----------------------------------------------------------

struct Lin {
  const GlyphLibrary& lib;
  const SpellTree& t;
  int laneG = -1, endG = -1;
  // How many lane scopes are open where we currently are. A wall word is an
  // `end`, so it may only be spoken where there is no lane for it to close.
  int laneDepth = 0;
  // Set when an item NEEDED a wall where none could be spoken. The words are
  // still produced (Linearize is total); `Speakable` reads this to refuse.
  bool wallBlocked = false;
  bool MayWall() const { return laneDepth == 0 && endG >= 0; }

  std::vector<int> Item(int node);
  std::vector<int> Scope(int boxNode);
  std::vector<int> Segment(const std::vector<int>& items);
};

// The words of ONE item, in spoken order: a word is its id x n; a group is
// left, the operator x n, right; a box is its whole scope then its delivery.
std::vector<int> Lin::Item(int node) {
  std::vector<int> out;
  if (node < 0 || node >= (int)t.nodes.size()) return out;
  const SpellNode& n = t.nodes[node];
  if (n.box) {
    out = Scope(node);
    // A box with n > 1 cannot be spoken (saying the delivery twice NESTS it),
    // so the delivery goes out once and `Speakable` is what refuses the tree.
    if (n.glyph >= 0) out.push_back(n.glyph);
    return out;
  }
  if (n.group) {
    // THE RUN MERGE BITES INSIDE AN ITEM TOO. `null end null` is one `null`
    // taking another; without the wall it says `null null`, which merges into
    // ONE operator of multiplicity 2 with an empty slot — a different spell.
    // Same wall, same rule, same reason it may only be spoken outside a lane.
    auto wall = [&](int next) {
      if (out.empty() || out.back() != next || !Mergeable(lib, next)) return;
      if (MayWall()) out.push_back(endG);
      else wallBlocked = true;
    };
    if (n.left >= 0) {
      const std::vector<int> l = Item(n.left);
      out.insert(out.end(), l.begin(), l.end());
    }
    wall(n.glyph);
    for (int32_t k = 0; k < n.n; k++) out.push_back(n.glyph);
    if (n.right >= 0) {
      const std::vector<int> r = Item(n.right);
      if (!r.empty()) wall(r.front());
      out.insert(out.end(), r.begin(), r.end());
    }
    return out;
  }
  for (int32_t k = 0; k < n.n; k++) out.push_back(n.glyph);
  return out;
}

// A box's PILE: the shared segment, then one `lane` ... `end` pair per lane,
// in lane order. `end` is emitted even for the last lane, so a delivery spoken
// after the box closes the scope AROUND the lanes and not the lane itself.
std::vector<int> Lin::Scope(int boxNode) {
  std::vector<int> out;
  const SpellNode& b = t.nodes[boxNode];
  const int32_t L = (int32_t)b.laneAt.size() / 2;
  std::vector<int> shared;
  for (int ii : b.items)
    if (t.nodes[ii].lane == 0) shared.push_back(ii);
  const std::vector<int> sw = Segment(shared);
  out.insert(out.end(), sw.begin(), sw.end());
  for (int32_t k = 1; k <= L; k++) {
    std::vector<int> seg;
    for (int ii : b.items)
      if (t.nodes[ii].lane == k) seg.push_back(ii);
    if (laneG < 0) continue;   // a library with no marks cannot say a lane
    out.push_back(laneG);
    laneDepth++;
    const std::vector<int> lw = Segment(seg);
    laneDepth--;
    out.insert(out.end(), lw.begin(), lw.end());
    out.push_back(endG);
  }
  return out;
}

// ONE SEGMENT OF A PILE, ordered so that re-speaking it rebuilds it.
//
// The pile is a SET (rule 1), so the order is ours to choose — and it is a
// SEARCH, not a sort, because four rules constrain which item may follow
// which. Getting this wrong is silent: `trail echo` and `echo trail` are two
// different trees, and the second binds what the first leaves empty.
//
//   1. THE ITEM THAT SPEAKS A DELIVERY OF ITS OWN MUST BE FIRST. Its delivery
//      word boxes whatever is in the pile at that moment, so anything already
//      emitted in this segment would be swallowed into it.
//   2. A GROUP WITH AN EMPTY LEFT SLOT may only follow an item its left mask
//      REJECTS (or nothing at all). `echo` after `trail` takes it, because
//      `trail`'s result is a Mod and `echo` wants an Effect — but `trail`
//      after `echo` does not, so that pair has exactly one legal order.
//   3. A GROUP WITH AN EMPTY RIGHT SLOT may only be followed by a word its
//      right mask REJECTS, which in practice means last, or before a `lane` /
//      `end` / delivery word.
//   4. Two adjacent identical words MERGE into one item of multiplicity 2, so
//      an item may not start with the word the previous one ended with.
//
// Candidates are tried in canonical `NodeKey` order and the FIRST complete
// arrangement wins, so the answer is a pure function of the tree — two players
// who built the same spell by different routes save the same words.

struct SegEntry {
  int node;
  std::string key;
  std::vector<int> words;
  bool boxy = false;
  bool emptyLeft = false;
  uint8_t leftMask = 0;
  bool emptyRight = false;
  uint8_t rightMask = 0;
  GlyphSort sort = GlyphSort::Effect;
};

// THE WALL. A stray `end` with no lane open is a charged no-op that changes
// nothing — and the grammar already uses it as a wall: `transmute end fire`
// is how a sentence says "transmute's right slot stays empty and the fire is
// beside it, not inside it". The linearizer may therefore spend one (it costs
// zero mana; `end`'s word cost is 0) to break a binding the order alone cannot
// — but ONLY where no lane is open, because there an `end` would close it.
enum class Follow : uint8_t { Ok = 0, NeedWall, No };

// May `e` be spoken with `prev` on top of the pile? `prev == nullptr` is an
// empty pile (the start of the segment). Rules 3 and 4 are about the word
// LITERALLY next in the sentence, so a wall between them answers both; rules 1
// and 2 are about the PILE, which a no-op does not touch, so nothing saves
// them but a different order.
Follow CanFollow(const GlyphLibrary& lib, const SegEntry* prev, const SegEntry& e) {
  if (e.boxy && prev) return Follow::No;                     // rule 1
  if (e.emptyLeft && prev && MaskAccepts(e.leftMask, prev->sort))
    return Follow::No;                                       // rule 2
  if (!prev) return Follow::Ok;
  if (prev->emptyRight) {                                    // rule 3
    const GlyphDef* g = lib.At(e.words.front());
    if (g && MaskAccepts(prev->rightMask, g->sort)) return Follow::NeedWall;
  }
  const int w = e.words.front();
  if (w == prev->words.back() && Mergeable(lib, w)) return Follow::NeedWall;  // rule 4
  return Follow::Ok;
}

// Backtracking over the arrangement, lowest key first. `budget` bounds the
// search so a pathological segment cannot cost more than a refusal. `walls`
// comes back parallel to `out`: 1 where a wall word has to be spoken first.
bool SearchOrder(const GlyphLibrary& lib, const std::vector<SegEntry>& es,
                 std::vector<char>& used, std::vector<int>& out, std::vector<char>& walls,
                 int prev, bool mayWall, int& budget) {
  if (out.size() == es.size()) return true;
  for (size_t i = 0; i < es.size(); i++) {
    if (used[i]) continue;
    if (--budget < 0) return false;
    const Follow f = CanFollow(lib, prev < 0 ? nullptr : &es[(size_t)prev], es[i]);
    if (f == Follow::No) continue;
    if (f == Follow::NeedWall && !mayWall) continue;
    used[i] = 1;
    out.push_back((int)i);
    walls.push_back(f == Follow::NeedWall ? 1 : 0);
    if (SearchOrder(lib, es, used, out, walls, (int)i, mayWall, budget)) return true;
    walls.pop_back();
    out.pop_back();
    used[i] = 0;
  }
  return false;
}

// Try it without spending a wall first, so the canonical form is the shortest
// one: a wall is a real word on the page and two spells that differ only in
// one would look different for no reason.
bool ArrangeSegment(const GlyphLibrary& lib, const std::vector<SegEntry>& es,
                    bool mayWall, std::vector<int>& order, std::vector<char>& walls) {
  for (int pass = 0; pass < (mayWall ? 2 : 1); pass++) {
    std::vector<char> used(es.size(), 0);
    order.clear();
    walls.clear();
    int budget = 40000;
    if (SearchOrder(lib, es, used, order, walls, -1, pass == 1, budget)) return true;
  }
  return false;
}

std::vector<SegEntry> SegEntries(const GlyphLibrary& lib, const SpellTree& t,
                                 const std::vector<int>& items,
                                 const std::function<std::vector<int>(int)>& wordsOf) {
  std::vector<SegEntry> es;
  for (int ii : items) {
    SegEntry e;
    e.node = ii;
    e.key = NodeKey(lib, t, ii);
    e.words = wordsOf(ii);
    if (e.words.empty()) continue;
    e.boxy = ContainsBox(t, ii);
    e.emptyLeft = EmptyLeft(lib, t, ii);
    e.emptyRight = EmptyRight(lib, t, ii);
    const GlyphDef* g = lib.At(t.nodes[ii].glyph);
    if (g) {
      e.leftMask = g->leftMask;
      e.rightMask = g->rightMask;
    }
    e.sort = NodeSort(lib, t, ii);
    es.push_back(std::move(e));
  }
  std::sort(es.begin(), es.end(),
            [](const SegEntry& a, const SegEntry& b) { return a.key < b.key; });
  return es;
}

std::vector<int> Lin::Segment(const std::vector<int>& items) {
  std::vector<int> out;
  if (items.empty()) return out;
  const std::vector<SegEntry> es =
      SegEntries(lib, t, items, [this](int ii) { return Item(ii); });
  std::vector<int> order;
  std::vector<char> walls;
  if (!ArrangeSegment(lib, es, MayWall(), order, walls)) {
    // No arrangement rebuilds this segment. Linearize stays TOTAL: it says the
    // canonical order anyway, and `Speakable` (which runs the same search) is
    // what refuses the tree.
    order.clear();
    walls.assign(es.size(), 0);
    for (size_t i = 0; i < es.size(); i++) order.push_back((int)i);
  }
  for (size_t k = 0; k < order.size(); k++) {
    if (walls[k] && endG >= 0) out.push_back(endG);
    for (int w : es[(size_t)order[k]].words) out.push_back(w);
  }
  return out;
}

// ---- canonical identity ------------------------------------------------------

std::string CanonOf(const GlyphLibrary& lib, const SpellTree& t, int node) {
  if (node < 0 || node >= (int)t.nodes.size()) return "";
  const SpellNode& n = t.nodes[node];
  if (!n.box) return NodeKey(lib, t, node);
  std::vector<std::string> items;
  for (int ii : n.items)
    items.push_back(CanonOf(lib, t, ii) + "#" + std::to_string(t.nodes[ii].n));
  std::sort(items.begin(), items.end());
  std::string s = "[";
  for (const std::string& i : items) s += i + ",";
  return s + "|" + lib.Delivery(n.glyph).id + "|" + std::to_string(n.laneAt.size() / 2) +
         "]" + (n.lane > 0 ? "@" + std::to_string(n.lane) : std::string()) + "#" +
         std::to_string(n.n);
}

// ---- tree surgery ------------------------------------------------------------

// Where every reachable node hangs: its parent and which slot of it.
struct Nav {
  std::vector<int> parent;
  std::vector<int> slot;   // -1 = a box item, 0 = left, 1 = right
};

void NavWalk(const SpellTree& t, int node, int parent, int slot, Nav& nav) {
  if (node < 0 || node >= (int)t.nodes.size()) return;
  nav.parent[node] = parent;
  nav.slot[node] = slot;
  const SpellNode& n = t.nodes[node];
  if (n.box)
    for (int ii : n.items) NavWalk(t, ii, node, -1, nav);
  if (n.group) {
    NavWalk(t, n.left, node, 0, nav);
    NavWalk(t, n.right, node, 1, nav);
  }
}

Nav BuildNav(const SpellTree& t) {
  Nav nav;
  nav.parent.assign(t.nodes.size(), -1);
  nav.slot.assign(t.nodes.size(), -1);
  for (const SpellClause& c : t.clauses) NavWalk(t, c.root, -1, -1, nav);
  return nav;
}

bool InSubtree(const SpellTree& t, int root, int needle) {
  if (root < 0 || needle < 0) return false;
  if (root == needle) return true;
  const SpellNode& n = t.nodes[root];
  if (n.box)
    for (int ii : n.items)
      if (InSubtree(t, ii, needle)) return true;
  if (n.group)
    return InSubtree(t, n.left, needle) || InSubtree(t, n.right, needle);
  return false;
}

// Deep-copy a subtree into the same arena (for Move with `copy`).
int CloneSubtree(SpellTree& t, int node) {
  if (node < 0) return -1;
  SpellNode n = t.nodes[node];
  if (n.box) {
    std::vector<int> items;
    for (int ii : n.items) items.push_back(CloneSubtree(t, ii));
    n.items = items;
  }
  if (n.group) {
    n.left = CloneSubtree(t, n.left);
    n.right = CloneSubtree(t, n.right);
  }
  t.nodes.push_back(n);
  return (int)t.nodes.size() - 1;
}

// THE MERGE THE PARSER WOULD DO. `MergePile` collapses identical items of a
// segment; an edited tree has to do the same or its own words would re-parse
// to something with a different multiplicity. Bottom-up, because a box's key
// depends on the items under it.
void Recanonicalize(const GlyphLibrary& lib, SpellTree& t, int node) {
  if (node < 0 || node >= (int)t.nodes.size()) return;
  SpellNode& n = t.nodes[node];
  if (n.group) {
    Recanonicalize(lib, t, n.left);
    Recanonicalize(lib, t, n.right);
    return;
  }
  if (!n.box) return;
  std::vector<int> items = n.items;
  for (int ii : items) Recanonicalize(lib, t, ii);
  const int32_t cap = lib.budgets.maxMultiplicity;
  std::vector<int> kept;
  std::vector<std::string> keys;
  for (int ii : items) {
    const std::string k = NodeKey(lib, t, ii);
    bool merged = false;
    for (size_t j = 0; j < keys.size(); j++) {
      if (keys[j] != k) continue;
      t.nodes[kept[j]].n = std::min(t.nodes[kept[j]].n + t.nodes[ii].n, cap);
      merged = true;
      break;
    }
    if (merged) continue;
    keys.push_back(k);
    kept.push_back(ii);
  }
  t.nodes[node].items = kept;
}

void RecanonicalizeTree(const GlyphLibrary& lib, SpellTree& t) {
  for (SpellClause& c : t.clauses) {
    Recanonicalize(lib, t, c.root);
    if (c.root >= 0) c.bag = t.nodes[c.root].items;
  }
}

// An edited tree's spoken positions are meaningless (the words move), so they
// are cleared rather than left lying: only the lane COUNT survives an edit,
// and it is what the linearizer reads.
void ForgetSpans(SpellTree& t, int node) {
  if (node < 0 || node >= (int)t.nodes.size()) return;
  SpellNode& n = t.nodes[node];
  n.first = n.last = n.at = -1;
  if (n.box) {
    for (int& la : n.laneAt) la = -1;
    for (int ii : n.items) ForgetSpans(t, ii);
  }
  if (n.group) {
    ForgetSpans(t, n.left);
    ForgetSpans(t, n.right);
  }
}

// ---- the caps ----------------------------------------------------------------

bool ClampedAnywhere(const CastList& l) {
  for (const SpellCast& c : l.casts)
    if (c.instancesClamped) return true;
  for (const BoxPrice& p : l.boxPrice)
    if (p.instancesClamped) return true;
  return false;
}

// THE ONE EXIT every op takes: canonicalize, check the caps, check the tree is
// sayable, say it, and then PROVE the saying by parsing it back. An op that
// cannot prove itself is refused, never returned.
EditResult Finish(const GlyphLibrary& lib, const SpellTree& before, SpellTree& after,
                  const char* intent) {
  EditResult r;
  RecanonicalizeTree(lib, after);
  for (SpellClause& c : after.clauses) ForgetSpans(after, c.root);

  std::string why;
  if (!Speakable(lib, after, why)) {
    r.why = why;
    return r;
  }
  const std::vector<std::string> words = Linearize(lib, after);
  if ((int)words.size() > kSpellStackMax) {
    r.why = "that would take " + std::to_string(words.size()) + " words; a spell holds " +
            std::to_string(kSpellStackMax);
    return r;
  }
  const CastList lb = LowerSpell(lib, before);
  const CastList la = LowerSpell(lib, after);
  if (ClampedAnywhere(la) && !ClampedAnywhere(lb)) {
    r.why = "that would fan past the instance cap (" +
            std::to_string(lib.budgets.maxInstances) + ")";
    return r;
  }
  // THE PROOF. Two trees are the same spell when their span-agnostic keys are.
  // Silence is the one asymmetry: an empty word list parses to NO clause (a
  // spell is never nothing), while the edited tree is still an empty hand.
  const SpellTree re = words.empty() ? EmptyTree() : ParseWords(lib, words);
  if (GraphTreeKey(lib, re) != GraphTreeKey(lib, after)) {
    r.why = std::string(intent) + " cannot be written back as words; "
            "say it with a lane of its own";
    return r;
  }
  r.ok = true;
  r.words = words;
  return r;
}

// Put a node into a box's segment, merging it with an identical item the way
// the parser's pile does.
void PlaceItem(const GlyphLibrary& lib, SpellTree& t, int boxNode, int32_t lane,
               int node) {
  t.nodes[node].lane = lane;
  t.nodes[boxNode].items.push_back(node);
}

// Detach a node from wherever it hangs. Returns false when it is a clause root.
bool Detach(SpellTree& t, const Nav& nav, int node) {
  const int p = nav.parent[node];
  if (p < 0) return false;
  if (nav.slot[node] < 0) {
    std::vector<int>& items = t.nodes[p].items;
    items.erase(std::remove(items.begin(), items.end(), node), items.end());
    return true;
  }
  if (nav.slot[node] == 0) t.nodes[p].left = -1;
  else t.nodes[p].right = -1;
  t.nodes[p].complete = false;
  return true;
}

// A lane index the caller may use: 0..L exist, and anything past L OPENS every
// lane up to it.
//
// EVERY SOCKET IS A TARGET (2026-09-21). Lanes used to open strictly one at a
// time, which is fine when the sockets ARE the lanes — but a count mod gives a
// box more instances than it has lanes, so `fire shotgun projectile` draws
// THREE sockets over a box with no lanes at all. Two of those three refused
// every drop with "give the sockets before this one a payload first", and
// which one was "before" was invisible: instance 0 is the aim and the aim is
// drawn in the CENTRE of the row, so the order was middle, then right, then
// left. Nothing in the drawing said so, and the result was a panel that
// behaved differently depending on which of three identical pips you aimed at.
//
// The lanes in between now open EMPTY, which is exactly what those sockets
// already were — an instance carrying the shared payload alone. `instances` is
// max(count, lanes), so filling lanes up to an existing socket cannot move it,
// and the lowered cast is unchanged: this buys reachability and nothing else.
bool OpenLaneIfNeeded(SpellTree& t, int boxNode, int32_t lane, int32_t cap,
                      std::string& why) {
  const int32_t L = (int32_t)t.nodes[boxNode].laneAt.size() / 2;
  if (lane < 0) {
    why = "there is no such lane";
    return false;
  }
  if (lane <= L) return true;
  if (cap > 0 && lane > cap) {
    why = "a box fans to at most " + std::to_string(cap) + " instances";
    return false;
  }
  for (int32_t k = L; k < lane; k++) {
    t.nodes[boxNode].laneAt.push_back(-1);
    t.nodes[boxNode].laneAt.push_back(-1);
  }
  return true;
}

// A LANE THE LAST ITEM LEFT IS NOT A LANE THE PLAYER ASKED FOR (2026-09-22).
//
// Lanes are POSITIONAL — lane k is instance k-1, and instance 0 is the aim — so
// a lane cannot simply be deleted from the middle of a box without moving every
// lane after it onto a different bolt. That is why a lane used to survive its
// last item: keeping it was the only way to keep the lanes above it where the
// player put them.
//
// The cost was that a fan, once built, could not be taken down. Open three
// lanes, fill them, then delete the three payloads and the box still spells
// `lane end lane end lane end`: three instances, no content, three pairs of
// words on the page that nothing in the drawing asked for. Opening a lane is
// also a SIDE EFFECT of dropping on a far socket (`OpenLaneIfNeeded` opens the
// ones before it), so a player could accumulate lanes they never chose at all
// and then find the sentence would not shrink back.
//
// The asymmetry is the answer: a TRAILING empty lane has nothing above it to
// hold in place, so dropping it moves nothing. Cascading from the top, a fan
// unwinds exactly the way it was wound — delete the last payload and the last
// socket goes with it — while an INTERIOR empty lane stays, because it is a
// real bare instance standing between two the player aimed at. `CloseLane` is
// the explicit gesture for that one.
void PruneTrailingEmptyLanes(SpellTree& t, int boxNode) {
  if (boxNode < 0 || boxNode >= (int)t.nodes.size() || !t.nodes[boxNode].box) return;
  SpellNode& b = t.nodes[boxNode];
  for (int32_t L = (int32_t)b.laneAt.size() / 2; L > 0; L--) {
    bool occupied = false;
    for (int ii : b.items)
      if (t.nodes[ii].lane == L) {
        occupied = true;
        break;
      }
    if (occupied) break;
    b.laneAt.pop_back();
    b.laneAt.pop_back();
  }
}

// The box a node sits IN, which is the one whose lanes its departure can empty:
// its parent when that is a box, and nothing when it is an operator's operand
// (an operand keeps lane 0 and its group holds the lane).
int OwningBox(const SpellTree& t, const Nav& nav, int node) {
  const int p = node >= 0 ? nav.parent[node] : -1;
  return IsBox(t, p) ? p : -1;
}

SpellNode MakeWord(int glyph) {
  SpellNode n;
  n.glyph = glyph;
  n.n = 1;
  return n;
}

// A glyph as a pile ITEM: a noun is a word, an operator is a group with empty
// pips, a delivery is an EMPTY BOX (the kinetic hit of plan §5).
int MakeItemFor(const GlyphLibrary& lib, SpellTree& t, int glyphId, std::string& why) {
  const GlyphDef* g = lib.At(glyphId);
  if (!g) {
    why = "that word is not in the library";
    return -1;
  }
  if (g->sort == GlyphSort::Separator) {
    why = "`" + g->id + "` is structure, not an item; open a lane instead";
    return -1;
  }
  if (g->sort == GlyphSort::Delivery) {
    SpellNode b;
    b.box = true;
    b.glyph = glyphId;
    b.n = 1;
    t.nodes.push_back(b);
    return (int)t.nodes.size() - 1;
  }
  if (g->sort == GlyphSort::Operator) {
    SpellNode grp;
    grp.glyph = glyphId;
    grp.n = 1;
    grp.group = true;
    grp.complete = !g->hasLeft && !g->hasRight;
    t.nodes.push_back(grp);
    return (int)t.nodes.size() - 1;
  }
  t.nodes.push_back(MakeWord(glyphId));
  return (int)t.nodes.size() - 1;
}

// ---- layout ------------------------------------------------------------------

// THE VISUAL SLOT OF EACH INSTANCE. Instance 0 is the aim, so it takes the
// CENTRE slot of the socket row and the rest fan outward from it — which is
// what rule 4 promises about the first lane you open being the middle bolt.
std::vector<int32_t> SlotOrder(int32_t instances) {
  std::vector<int32_t> slots((size_t)std::max<int32_t>(instances, 1), -1);
  const int32_t n = (int32_t)slots.size();
  const int32_t centre = n / 2;
  slots[(size_t)centre] = 0;
  int32_t r = centre + 1, l = centre - 1;
  for (int32_t i = 1; i < n; i++) {
    if (r < n) {
      slots[(size_t)r] = i;
      r++;
    } else {
      slots[(size_t)l] = i;
      l--;
    }
  }
  return slots;
}

struct Builder {
  const GlyphLibrary& lib;
  const CastList& list;
  SpellGraph g;
  int maxLayer = 0;

  int Add(SpellGraphNode n) {
    g.nodes.push_back(std::move(n));
    return (int)g.nodes.size() - 1;
  }
  void Edge(int from, int to, GraphEdge k) { g.edges.push_back({from, to, k}); }

  std::string ModEdit(const GlyphDef& gd, int32_t n) const;
  bool IsWasted(int glyph) const;
  bool IsWastedNode(int treeNode) const;
  // Places the subtree rooted at `node` with its left edge at `x` and its
  // BOTTOM band on `layer`, and returns its width. `outTop`, when given, gets
  // the highest layer the subtree consumed — which is how a caller stacks the
  // next thing directly on top of a subtree whose depth it cannot know in
  // advance. (It used to not need one: every box put its bar on the layer it
  // was handed and grew upward, so a parent could hand out fixed layers. Now a
  // spoken box's bar is at the TOP of its span, so the parent has to be told.)
  int Place(int node, int32_t lane, int layer, int x, int& outIdx,
            int* outTop = nullptr);
};

std::string Builder::ModEdit(const GlyphDef& gd, int32_t n) const {
  const char* f = ModFieldName(gd.field);
  // COMPOSED, not per word: `speed x2` said twice is x4, and the tag says so.
  // A COUNT DOES NOT COMPOSE (2026-09-22): one split per scope, applied once
  // however many times it was said, so a `shotgun x3` bead saying "count x27"
  // would be describing a spell nobody can cast.
  int64_t amount = gd.amount;
  if (gd.field == ModField::Count) {
    n = 1;
  }
  if (gd.op == ModOp::Add) {
    amount = (int64_t)gd.amount * n;
  } else {
    int64_t a = 1;
    for (int32_t i = 0; i < n && a < (1 << 20); i++) a *= gd.amount;
    amount = a;
  }
  char buf[96];
  const char* sym = gd.op == ModOp::Mul ? "x" : (gd.op == ModOp::Div ? "/" : "");
  if (gd.op == ModOp::Add)
    std::snprintf(buf, sizeof buf, "%s %+lld", f, (long long)amount);
  else
    std::snprintf(buf, sizeof buf, "%s %s%lld", f, sym, (long long)amount);
  return buf;
}

bool Builder::IsWastedNode(int treeNode) const {
  for (const SpellCast& c : list.casts)
    for (int nd : c.wastedNodes)
      if (nd == treeNode) return true;
  return false;
}

bool Builder::IsWasted(int glyph) const {
  for (const SpellCast& c : list.casts)
    for (int m : c.wastedMods)
      if (m == glyph) return true;
  return false;
}

int Builder::Place(int node, int32_t lane, int layer, int x, int& outIdx,
                   int* outTop) {
  const SpellTree& t = list.tree;
  const SpellNode& n = t.nodes[node];
  maxLayer = std::max(maxLayer, layer);
  if (outTop) *outTop = layer;

  // ---- a plain word ----------------------------------------------------------
  if (!n.box && !n.group) {
    SpellGraphNode gn;
    gn.kind = GraphKind::Word;
    gn.treeNode = node;
    gn.glyph = n.glyph;
    gn.n = n.n;
    gn.sort = NodeSort(lib, t, node);
    gn.lane = lane;
    const GlyphDef* wd = lib.At(n.glyph);
    gn.label = (wd ? wd->id : "?") + (n.n > 1 ? "x" + std::to_string(n.n) : "");
    gn.x = x;
    gn.w = kGraphCell;
    gn.h = kGraphCell;
    gn.layer = layer;
    gn.baseLayer = layer;
    gn.spanFirst = n.first;
    gn.spanLast = n.last;
    gn.subX = x;
    gn.subW = kGraphCell;
    outIdx = Add(gn);
    return kGraphCell;
  }

  // ---- an operator group ------------------------------------------------------
  if (n.group) {
    const GlyphDef* gd = lib.At(n.glyph);
    std::vector<int> kids;
    int kidW = 0;
    if (n.left >= 0) kids.push_back(n.left);
    if (n.right >= 0) kids.push_back(n.right);
    // Measure by placing into a scratch pass: the children are laid out first
    // at a provisional origin and then shifted, which is exact because every
    // width here is an integer.
    std::vector<int> kidIdx(kids.size(), -1);
    std::vector<int> kidWidth(kids.size(), 0);
    const size_t markNodes = g.nodes.size(), markEdges = g.edges.size();
    for (size_t i = 0; i < kids.size(); i++) {
      int idx = -1;
      kidWidth[i] = Place(kids[i], lane, layer, 0, idx);
      kidIdx[i] = idx;
      kidW += kidWidth[i] + (i + 1 < kids.size() ? kGraphGap : 0);
    }
    g.nodes.resize(markNodes);
    g.edges.resize(markEdges);
    const int total = std::max(kGraphCell, kidW);
    int cx = x + (total - kidW) / 2;
    std::vector<int> placed;
    // THE OPERANDS ARE THE ROW BELOW, because that is where they are in the
    // sentence: `fire transmute` fills transmute's left slot with the word
    // SPOKEN BEFORE it. They used to sit above the cell, which drew every
    // operator group upside down with respect to its own words.
    int kidTop = layer - 1;
    for (size_t i = 0; i < kids.size(); i++) {
      int idx = -1, top = layer;
      Place(kids[i], lane, layer, cx, idx, &top);
      kidTop = std::max(kidTop, top);
      placed.push_back(idx);
      cx += kidWidth[i] + kGraphGap;
    }
    const int cellLayer = kids.empty() ? layer : kidTop + 1;
    SpellGraphNode gn;
    gn.kind = GraphKind::Operator;
    gn.treeNode = node;
    gn.glyph = n.glyph;
    gn.n = n.n;
    gn.sort = NodeSort(lib, t, node);
    gn.lane = lane;
    gn.label = (gd ? gd->id : "?") + (n.n > 1 ? "x" + std::to_string(n.n) : "");
    gn.hasLeft = gd && gd->hasLeft;
    gn.hasRight = gd && gd->hasRight;
    gn.leftFilled = n.left >= 0;
    gn.rightFilled = n.right >= 0;
    gn.complete = n.complete;
    gn.x = x + (total - kGraphCell) / 2;
    gn.w = kGraphCell;
    gn.h = kGraphCell;
    gn.layer = cellLayer;
    gn.baseLayer = layer;
    gn.spanFirst = n.first;
    gn.spanLast = n.last;
    gn.subX = x;
    gn.subW = total;
    outIdx = Add(gn);
    maxLayer = std::max(maxLayer, cellLayer);
    if (outTop) *outTop = cellLayer;
    for (int p : placed) Edge(p, outIdx, GraphEdge::Slot);
    return total;
  }

  // ---- a box: the bar, its sockets, its bus, its tags -------------------------
  const int32_t L = (int32_t)n.laneAt.size() / 2;
  const BoxPrice* price = list.PriceOf(node);
  int32_t instances = price ? price->instances : std::max<int32_t>(1, L);
  if (instances < 1) instances = 1;

  // A LANE THAT IS NOT DRAWN IS A WORD THAT VANISHED. Only lanes 1..instances
  // get a socket, so anything in a lane past the last instance — which happens
  // when the fan was clamped — would be placed nowhere and edged to nothing.
  // Shown in the shared pile instead: misfiled beats invisible, and the bar
  // already wears the clamp notch that explains it.
  const int32_t drawnLanes = std::min<int32_t>(L, instances);

  // WHICH MODS RIDE THE TRUNK AND WHICH RIDE A LANE. A mod in a lane edits that
  // instance's record alone, so it belongs over the socket it edits and NOT in
  // the shared bead row, which is where all of them used to land regardless of
  // lane: two mods on two different lanes drew as one indistinguishable centred
  // row.
  //
  // `count` USED TO BE THE EXCEPTION and is not any more (2026-09-22). It was
  // record-wide wherever it was spoken, so a `shotgun` inside lane 2 trebled
  // the whole box and drawing it over one socket would have been a lie. It now
  // splits the scope it was spoken in, so it draws exactly where it was said:
  // on the trunk when shared, on a branch's neck when that branch is the thing
  // it splits.
  std::vector<int> mods, shared;
  std::vector<std::vector<int>> laneItems((size_t)L + 1);
  std::vector<std::vector<int>> laneMods((size_t)L + 1);
  for (int ii : n.items) {
    const int32_t ln = t.nodes[ii].lane;
    const bool inLane = ln > 0 && ln <= drawnLanes;
    if (NodeSort(lib, t, ii) == GlyphSort::Mod) {
      if (inLane) laneMods[(size_t)ln].push_back(ii);
      else mods.push_back(ii);
      continue;
    }
    if (inLane) laneItems[(size_t)ln].push_back(ii);
    else shared.push_back(ii);
  }

  // A MOD IS A BEAD ON A STROKE, NOT A LABEL BESIDE IT (2026-09-21). The tags
  // used to hang off the bar's LEFT end on the bar's own layer, which put
  // `shotgun` out in the margin with a stroke running sideways-and-backwards
  // into the bar, beside the thing it edits rather than on the line between the
  // bar and the fan it modifies. A mod edits a record, so it belongs on the
  // line that feeds that record: a record-wide mod on the trunk between the bar
  // and its sockets, a lane's mod on the neck between that lane's items and the
  // socket they feed. Several mods on one stroke share a layer as a centred row.
  // A MOD THAT IS A GROUP IS A DRAWING, NOT A BEAD (2026-09-22). `fire trail`
  // is ONE operator with `fire` in its left slot and a RESULT that is a mod, so
  // it landed in the bead row — flattened to a lone `trail` tag whose operand
  // was never placed at all. What the page drew for `fire trail projectile` was
  // a bead hanging off the trunk beside the socket fan: two strokes into the
  // bar, and no `fire` anywhere on the sheet. A group goes through `Place` like
  // every other item now, which puts its operands under its cell with the slot
  // strokes that say which pip each one fills, and only the CELL edges on to
  // the trunk (or to its lane's socket). The sentence reads as one line again.
  auto isGroup = [&](int mi) { return t.nodes[(size_t)mi].group; };
  // A row entry's width: the tag's fixed width for a bead, the group's whole
  // measured span for a group. Measured by a scratch placement thrown away
  // again, exactly as `measureRow` does — a width never depends on the layer.
  auto modW = [&](int mi) {
    if (!isGroup(mi)) return kGraphTagW;
    const size_t mn = g.nodes.size(), me = g.edges.size();
    int idx = -1;
    const int w = Place(mi, 0, layer, 0, idx);
    g.nodes.resize(mn);
    g.edges.resize(me);
    return w;
  };
  auto modRowWOf = [&](const std::vector<int>& row) {
    int w = 0;
    for (size_t i = 0; i < row.size(); i++)
      w += modW(row[i]) + (i + 1 < row.size() ? kGraphGap : 0);
    return w;
  };
  const int modRowW = modRowWOf(mods);
  bool anyLaneMod = false, anyLaneItem = false;
  for (const std::vector<int>& lm : laneMods) anyLaneMod |= !lm.empty();
  for (size_t k = 1; k < laneItems.size(); k++)
    anyLaneItem |= (int32_t)k <= drawnLanes && !laneItems[k].empty();

  // ONE SHAPE, ALWAYS. There used to be two — with no lanes the sockets fanned
  // out CENTRED under the shared column, and with any lane at all the shared
  // block was shoved to the LEFT and the socket row bunched to the right of it
  // — so opening a single lane on a `shotgun` box teleported the whole drawing
  // sideways and the fan stopped being centred under its own bar. The two
  // shapes existed because the shared items and the lane items shared ONE
  // layer, and two rows that both want the middle cannot both have it.
  //
  // They get a layer each now: the bar, the record-wide beads, the socket row,
  // each lane's beads, each lane's items, the bus, and the shared pile. Every
  // stroke in that stack is short and vertical — a lane's items reach their own
  // socket without crossing the bus, the shared pile reaches the bus without
  // crossing anything — and the one long stroke left is the bus's own trunk
  // into the bar, which is the trunk.
  //
  // WHICH END THE BAR IS ON IS THE WHOLE INVERSION (2026-09-21). A SPOKEN
  // delivery is a word, and its word comes after the pile it boxes, so its bar
  // is the TOP of that stack and the pile is underneath: `fire projectile`
  // draws fire with PROJECTILE over it, which is the order you typed. The
  // implicit `hand` is the one box with no word to place, so it keeps the old
  // order and is the pedestal the sentence stands on — bar at the bottom, its
  // fan just above it, the spoken spell above that.
  //
  // ...AND A SPLIT OPENS FORWARD, WHICHEVER END THAT IS (2026-09-22). The fan
  // used to be `socket -> bar`, with the socket row hard against the bar. On
  // the `hand` that draws three strokes rising out of the pedestal and reads
  // as "the hand makes three"; on a spoken delivery the same edge runs DOWN
  // out of the cell and reads as "the projectile splits", pointing backwards.
  // One construct, two pictures, and only one of them true — because the split
  // point was FUSED to the delivery cell, so the geometry had to follow
  // wherever the delivery word happened to sit.
  //
  // They are unfused. A box with more than one instance synthesizes a SPLIT
  // junction, the fan is `socket -> split`, and the junction is placed
  // immediately below the socket row in BOTH orders. Everything above the
  // socket row then belongs to one branch each — which is why a spoken
  // delivery is drawn ONCE PER BRANCH, a cell capping every column, instead of
  // one bar with a row of pips under it. The `hand` needs no copies: it is the
  // pedestal the branches have not diverged from yet, so its one cell stays at
  // the bottom and its fan still rises out of the junction over it. One rule,
  // and the hand's picture does not move.
  //
  // And the layers are handed out by WALKING, not by counting: a stage asks the
  // subtrees it just placed how tall they turned out and puts the next stage on
  // top. Fixed layer arithmetic worked only while every box's bar was the
  // bottom of its own span, and it was already fragile — a box sitting in a
  // LANE grew upward through the layers its parent had reserved for the bus and
  // the shared pile, which is a same-layer collision waiting for the sentence
  // that triggers it.
  const bool flip = n.glyph >= 0;

  // Measure a row by a scratch placement (exact, integers only). A width does
  // not depend on the layer, so the scratch runs at the box's base and is
  // thrown away.
  auto measureRow = [&](const std::vector<int>& items) {
    int w = 0;
    const size_t mn = g.nodes.size(), me = g.edges.size();
    for (size_t i = 0; i < items.size(); i++) {
      int idx = -1;
      w += Place(items[i], t.nodes[items[i]].lane, layer, 0, idx);
      if (i + 1 < items.size()) w += kGraphGap;
    }
    g.nodes.resize(mn);
    g.edges.resize(me);
    return w;
  };
  const int sharedW = measureRow(shared);

  // A BRANCH THAT ENDS IN A CELL NEEDS A CELL'S WIDTH. A spoken delivery with
  // more than one instance caps every column with a copy of itself, so the
  // column floor is the cell, not the pip. The `hand` and the unsplit box keep
  // the 32 px floor and are laid out exactly as before.
  const bool hasCopies = flip && instances > 1;
  const int colFloor = hasCopies ? kGraphCell : kGraphSocketW;
  const std::vector<int32_t> slots = SlotOrder(instances);
  std::vector<int> socketColW((size_t)instances, colFloor);
  for (int32_t inst = 0; inst < instances; inst++) {
    if (inst >= drawnLanes) continue;
    // The column has to hold the WIDER of its item row and its bead row: a
    // bead is 96 and a cell 64, so a lane whose only content is one mod still
    // needs more room than the 32 px pip.
    const int w = std::max(measureRow(laneItems[(size_t)inst + 1]),
                           modRowWOf(laneMods[(size_t)inst + 1]));
    socketColW[(size_t)inst] = std::max(colFloor, w);
  }

  // The socket row, tiled in SLOT order (instance 0 — the aim — in the middle)
  // and centred in the content; the shared pile centred over it.
  int socketsW = 0;
  for (int32_t s = 0; s < instances; s++)
    socketsW += socketColW[(size_t)slots[(size_t)s]] + (s + 1 < instances ? kGraphGap : 0);
  const int contentW = std::max(std::max(sharedW, socketsW), colFloor);
  const int sharedX = (contentW - sharedW) / 2;
  std::vector<int> colX((size_t)instances, 0);
  {
    int cx = (contentW - socketsW) / 2;
    for (int32_t s = 0; s < instances; s++) {
      const int32_t inst = slots[(size_t)s];
      colX[(size_t)inst] = cx;
      cx += socketColW[(size_t)inst] + kGraphGap;
    }
  }

  // A DELIVERY IS A CELL. The bar used to be `max(contentW, 128)` wide and to
  // stretch over its whole socket row; it is a 64 px square like every other
  // glyph now, centred on the box's axis, and the REACH the slab used to show
  // is a rule the canvas draws behind it across the socket row.
  const int barW = kGraphCell;
  // The box's own width is the widest of the content row, the bar and the bead
  // row, and all three are centred in it, so the trunk, the beads and the fan
  // share one axis.
  const int boxW = std::max(std::max(contentW, barW), modRowW);
  const int barX = x + (boxW - barW) / 2;
  const int contentX = x + (boxW - contentW) / 2;

  // ---- the stages -------------------------------------------------------------
  //
  // Each one puts its row on `cur` and leaves `cur` on the first free layer
  // above whatever it placed. They are run bottom-up in one of two orders (see
  // `flip`), and no stage knows which — that is the point.
  int cur = layer;
  int barIdx = -1, busIdx = -1;
  std::vector<int> socketIdx((size_t)instances, -1);
  std::vector<int> sharedPlaced;                     // shared item -> bus
  std::vector<std::pair<int, int>> laneLinks;        // (node, instance) -> socket
  // A RECORD-WIDE BEAD EDGES TO *THIS* BAR, AND THE BAR MAY NOT EXIST YET
  // (2026-09-22). These used to be pushed as edges with `to == -1` and patched
  // by a sweep over the WHOLE edge list once `barIdx` was known - which is a
  // global claim made from inside a recursive function. On the implicit `hand`
  // box the stage order is bar-first, so its beads were already sitting in the
  // list with `to == -1` while `stageItems` recursed into every box in the
  // sentence, and the FIRST nested box to finish patched the hand's beads onto
  // ITS bar. A `shotgun` on the hand then drew its bead hanging off a delivery
  // several layers up, outside that bar's span, with the fan it opened left
  // stranded at the foot of the page: the gate's "children outside their
  // parent's span" law, 145 times over the op fuzz. The pending list is local,
  // so a nested box cannot reach it.
  std::vector<int> trunkPending;                     // record-wide bead -> anchor
  int splitIdx = -1;                                 // the junction, -1 unsplit
  // The cell per branch, and the top of each branch's own stack. `deliveryIdx`
  // is empty on the `hand` (one pedestal, no copies) and on an unsplit box
  // (one cell, which IS `barIdx`).
  std::vector<int> deliveryIdx;
  std::vector<int> branchTop((size_t)instances, -1);

  // One bead. `instance` is which socket it edits, -1 for a record-wide one:
  // the tip says "this instance alone" or "the whole record" off that, so the
  // drawing and the words agree about a thing the picture alone cannot show.
  auto Bead = [&](int mi, int lyr, int cx, int32_t instance) {
    const SpellNode& mn = t.nodes[mi];
    SpellGraphNode tg;
    tg.kind = GraphKind::ModTag;
    tg.treeNode = mi;
    tg.glyph = mn.glyph;
    tg.n = mn.n;
    tg.sort = GlyphSort::Mod;
    tg.lane = mn.lane;
    tg.instance = instance;
    const GlyphDef* gd = lib.At(mn.glyph);
    tg.label = gd ? gd->id : "?";
    // A `trail` GROUP is a Mod too, and it has no field to compose.
    tg.edit = (gd && !mn.group && gd->field != ModField::None) ? ModEdit(*gd, mn.n)
                                                              : std::string();
    tg.wasted = (gd && IsWasted(mn.glyph)) || IsWastedNode(mi);
    tg.x = cx;
    tg.w = kGraphTagW;
    tg.h = kGraphTagH;
    tg.layer = lyr;
    tg.baseLayer = lyr;
    tg.spanFirst = mn.first;
    tg.spanLast = mn.last;
    tg.subX = tg.x;
    tg.subW = tg.w;
    return Add(tg);
  };

  // The shared pile: everything every instance carries.
  auto stageItems = [&]() {
    if (shared.empty()) return;
    int top = cur, cx = contentX + sharedX;
    for (size_t i = 0; i < shared.size(); i++) {
      int idx = -1, itop = cur;
      const int w = Place(shared[i], 0, cur, cx, idx, &itop);
      if (idx >= 0) sharedPlaced.push_back(idx);
      top = std::max(top, itop);
      cx += w + kGraphGap;
    }
    cur = top + 1;
  };
  // The bus: what the shared items feed, spanning every socket.
  auto stageBus = [&]() {
    if (shared.empty()) return;
    SpellGraphNode b;
    b.kind = GraphKind::Bus;
    b.treeNode = -1;
    b.x = contentX;
    b.w = contentW;
    b.h = kGraphBusH;
    b.layer = cur;
    b.baseLayer = cur;
    b.subX = b.x;
    b.subW = b.w;
    busIdx = Add(b);
    cur++;
  };
  // Each lane's own items, in that lane's column.
  auto stageLaneItems = [&]() {
    if (!anyLaneItem) return;
    int top = cur;
    for (int32_t k = 1; k <= drawnLanes; k++) {
      const int32_t inst = k - 1;
      const int colW = socketColW[(size_t)inst];
      const int rowW = measureRow(laneItems[(size_t)k]);
      int cx = contentX + colX[(size_t)inst] + (colW - rowW) / 2;
      for (size_t i = 0; i < laneItems[(size_t)k].size(); i++) {
        int idx = -1, itop = cur;
        const int w = Place(laneItems[(size_t)k][i], k, cur, cx, idx, &itop);
        if (idx >= 0) laneLinks.push_back({idx, (int)inst});
        top = std::max(top, itop);
        cx += w + kGraphGap;
      }
    }
    cur = top + 1;
  };
  // A lane's beads, on the neck between that lane's items and its socket.
  auto stageLaneMods = [&]() {
    if (!anyLaneMod) return;
    int top = cur;
    for (int32_t k = 1; k <= drawnLanes; k++) {
      const int32_t inst = k - 1;
      const int colW = socketColW[(size_t)inst];
      const int rowW = modRowWOf(laneMods[(size_t)k]);
      int cx = contentX + colX[(size_t)inst] + (colW - rowW) / 2;
      for (int mi : laneMods[(size_t)k]) {
        const int w = modW(mi);
        if (isGroup(mi)) {
          int idx = -1, itop = cur;
          Place(mi, k, cur, cx, idx, &itop);
          if (idx >= 0) laneLinks.push_back({idx, (int)inst});
          top = std::max(top, itop);
        } else {
          laneLinks.push_back({Bead(mi, cur, cx, inst), (int)inst});
        }
        cx += w + kGraphGap;
      }
    }
    cur = top + 1;
  };
  // The socket row, one pip per instance, in instance order.
  auto stageSockets = [&]() {
    for (int32_t inst = 0; inst < instances; inst++) {
      SpellGraphNode s;
      s.kind = GraphKind::Socket;
      s.treeNode = -1;
      s.lane = inst < L ? inst + 1 : 0;
      s.instance = inst;
      s.x = contentX + colX[(size_t)inst];
      s.w = socketColW[(size_t)inst];
      s.h = kGraphSocketH;
      s.pipW = kGraphSocketW;
      s.layer = cur;
      s.baseLayer = cur;
      s.label = "";
      s.subX = s.x;
      s.subW = s.w;
      socketIdx[(size_t)inst] = Add(s);
    }
    cur++;
  };
  // The record-wide beads, on the trunk between the bar and its sockets.
  auto stageMods = [&]() {
    if (mods.empty()) return;
    int cx = x + (boxW - modRowW) / 2;
    int top = cur;
    for (int mi : mods) {
      const int w = modW(mi);
      // The bar does not exist yet in the flipped order, so the bead is
      // remembered and edged to `barIdx` once every stage has run.
      if (isGroup(mi)) {
        int idx = -1, itop = cur;
        Place(mi, 0, cur, cx, idx, &itop);
        if (idx >= 0) trunkPending.push_back(idx);
        top = std::max(top, itop);
      } else {
        trunkPending.push_back(Bead(mi, cur, cx, -1));
      }
      cx += w + kGraphGap;
    }
    cur = top + 1;
  };
  // THE JUNCTION. One synthesized node, on its own layer immediately below the
  // socket row in BOTH orders, and the only thing a `Fan` edge ever points at.
  // It exists exactly when the box has more than one instance — an unsplit box
  // has nothing to diverge and gets no junction, which is what keeps its
  // drawing byte-identical to what it was.
  auto stageSplit = [&]() {
    if (instances <= 1) return;
    SpellGraphNode sp;
    sp.kind = GraphKind::Split;
    sp.treeNode = -1;
    sp.instances = instances;
    sp.x = x + (boxW - kGraphSplitW) / 2;   // ON THE BOX AXIS, always
    sp.w = kGraphSplitW;
    sp.h = kGraphSplitH;
    sp.layer = cur;
    sp.baseLayer = cur;
    // The junction SPANS THE WHOLE BOX, so every socket AND every record-wide
    // bead is inside its parent - a bead row is 96 wide and can be wider than
    // the content it sits over. Rect and span are decoupled on purpose (see
    // the header note): the drawn mark is 32 px on the axis.
    sp.subX = x;
    sp.subW = boxW;
    sp.spanFirst = n.first;
    sp.spanLast = n.last;
    splitIdx = Add(sp);
    cur++;
  };

  // ONE DELIVERY CELL. `inst` is which branch it caps, -1 when the box is not
  // split; the PRIMARY (instance 0, or the only cell) owns the record — the
  // price, the socket list, the bus, the junction and the whole box's span —
  // and every copy carries `primary` and its own column's span.
  auto Cell = [&](int32_t inst, int cx, int cw, bool primary, int32_t bolts = 1) {
    SpellGraphNode bar;
    bar.kind = n.glyph < 0 ? GraphKind::Root : GraphKind::Join;
    bar.treeNode = node;
    bar.glyph = n.glyph;
    bar.n = n.n;
    bar.sort = GlyphSort::Delivery;
    bar.lane = lane;
    bar.label = lib.Delivery(n.glyph).id;
    bar.instances = instances;
    bar.laneCount = L;
    bar.instance = inst;
    bar.bolts = bolts;
    bar.x = cx + (cw - kGraphCell) / 2;
    bar.w = kGraphCell;
    bar.h = kGraphCell;
    bar.layer = cur;
    // THE BOX HANDS OFF FROM THE BOTTOM OF ITS SPAN, not from its cell: on a
    // spoken delivery the cell is the TOP of the stack, and an edge leaving it
    // would fall the whole height of the subtree through every cell in it.
    // Only the primary hands off; a copy is not anybody's child.
    bar.baseLayer = primary ? layer : cur;
    bar.spanFirst = n.first;
    bar.spanLast = n.last;
    // The primary's span is the WHOLE BOX, so a parent's containment law sees
    // the box and the trunk leaves on the box axis; a copy's span is its own
    // column, so that branch's payload is inside the cell that caps it.
    bar.subX = primary ? x : cx;
    bar.subW = primary ? boxW : cw;
    if (primary && price) {
      bar.price = *price;
      bar.hasPrice = true;
      bar.subtotal = price->tariff + price->carryCost;
    }
    const int idx = Add(bar);
    if (primary) barIdx = idx;
    return idx;
  };

  // The `hand`, and any unsplit box: one cell, centred, exactly as before.
  auto stageBar = [&]() {
    Cell(-1, barX, barW, true);
    cur++;
  };
  // A SPOKEN DELIVERY WITH A FAN IS DRAWN ONCE PER BRANCH. Three bolts are
  // three cells, one capping each column, all on one layer so the drawing has
  // a flat top edge. They are one word and one record: instance 0 is the
  // primary and the rest carry `primary` pointing at it.
  auto stageDeliveries = [&]() {
    if (instances <= 1) {
      stageBar();
      return;
    }
    deliveryIdx.assign((size_t)instances, -1);
    for (int32_t inst = 0; inst < instances; inst++)
      deliveryIdx[(size_t)inst] =
          Cell(inst, contentX + colX[(size_t)inst], socketColW[(size_t)inst],
               inst == 0,
               price && inst < (int32_t)price->laneSplits.size()
                   ? price->laneSplits[(size_t)inst]
                   : 1);
    cur++;
  };

  // THE ORDER. Read either column downward and it is the same sentence: the
  // shared pile, what it all rides on, the split, one column per branch, and
  // the delivery word wherever the player said it. The two lists differ ONLY
  // in where the delivery lands, which is the one thing word order decides.
  if (flip) {
    stageItems();
    stageBus();
    stageMods();
    stageSplit();
    stageSockets();
    stageLaneMods();
    stageLaneItems();
    stageDeliveries();
  } else {
    stageBar();
    stageMods();
    stageSplit();
    stageSockets();
    stageLaneMods();
    stageLaneItems();
    stageBus();
    stageItems();
  }
  maxLayer = std::max(maxLayer, cur - 1);
  if (outTop) *outTop = cur - 1;

  // ---- the edges, once every node exists --------------------------------------
  // Wired here rather than inside the stages because in the flipped order half
  // of them are placed before the thing they connect to. Everything deferred is
  // deferred in a LOCAL list; nothing about this box is left in the shared edge
  // list for a recursive call to trip over.
  // THE ANCHOR is what the shared trunk ends at: the junction when the box has
  // one, the delivery cell when it has not. Everything shared — the bus, the
  // record-wide beads, the fan itself — meets there, which is what makes the
  // junction read as the place the one becomes many.
  const int anchorIdx = splitIdx >= 0 ? splitIdx : barIdx;
  for (int mi : trunkPending) Edge(mi, anchorIdx, GraphEdge::Trunk);
  g.nodes[(size_t)barIdx].sockets = socketIdx;
  g.nodes[(size_t)barIdx].split = splitIdx;
  for (int32_t inst = 0; inst < instances; inst++)
    Edge(socketIdx[(size_t)inst], anchorIdx, GraphEdge::Fan);
  // The `hand` is the pedestal everything stands on, so its junction hands off
  // downward to it. A spoken delivery's junction does not: its cells are at the
  // TOP of the branches, and the path to them already runs through the sockets.
  if (splitIdx >= 0 && !flip) Edge(splitIdx, barIdx, GraphEdge::Trunk);
  if (busIdx >= 0) {
    Edge(busIdx, anchorIdx, GraphEdge::Trunk);
    g.nodes[(size_t)barIdx].bus = busIdx;
  }
  for (int idx : sharedPlaced)
    Edge(idx, busIdx >= 0 ? busIdx : anchorIdx,
         busIdx >= 0 ? GraphEdge::Bus : GraphEdge::Trunk);
  for (const std::pair<int, int>& lk : laneLinks) {
    Edge(lk.first, socketIdx[(size_t)lk.second], GraphEdge::Socket);
    // ...and remember the HIGHEST thing in the column, because that is what
    // hands the branch on to the cell that caps it.
    int& top = branchTop[(size_t)lk.second];
    if (top < 0 || g.nodes[(size_t)lk.first].layer > g.nodes[(size_t)top].layer)
      top = lk.first;
  }
  // EVERY BRANCH ENDS IN ITS OWN CELL. The column runs socket -> its payload ->
  // the delivery copy; a branch carrying nothing of its own runs straight from
  // its pip into the cell.
  for (size_t k = 0; k < deliveryIdx.size(); k++) {
    const int from = branchTop[k] >= 0 ? branchTop[k] : socketIdx[k];
    Edge(from, deliveryIdx[k], GraphEdge::Trunk);
  }
  for (size_t k = 0; k < deliveryIdx.size(); k++)
    if ((int)k != 0) g.nodes[(size_t)deliveryIdx[k]].primary = barIdx;

  outIdx = barIdx;
  return boxW;
}

}  // namespace

// ---- public: words and trees --------------------------------------------------

std::vector<int> WordsToGlyphs(const GlyphLibrary& lib,
                               const std::vector<std::string>& words) {
  std::vector<int> out;
  for (const std::string& w : words) {
    const int gi = lib.Find(w);
    if (gi >= 0) out.push_back(gi);
  }
  return out;
}

std::vector<std::string> GlyphsToWords(const GlyphLibrary& lib,
                                       const std::vector<int>& glyphs) {
  std::vector<std::string> out;
  for (int g : glyphs) {
    const GlyphDef* d = lib.At(g);
    out.push_back(d ? d->id : "?");
  }
  return out;
}

SpellTree ParseWords(const GlyphLibrary& lib, const std::vector<std::string>& words) {
  SpellStack st;
  st.spoken = WordsToGlyphs(lib, words);
  return ParseSpell(lib, st);
}

SpellTree EmptyTree() {
  SpellTree t;
  SpellNode hand;
  hand.box = true;
  hand.glyph = -1;
  hand.n = 1;
  t.nodes.push_back(hand);
  SpellClause c;
  c.root = 0;
  c.delivery = -1;
  c.weight = 1;
  t.clauses.push_back(c);
  return t;
}

std::string GraphTreeKey(const GlyphLibrary& lib, const SpellTree& tree) {
  std::string s;
  for (const SpellClause& c : tree.clauses) s += "<" + CanonOf(lib, tree, c.root) + ">";
  return s;
}

std::vector<std::string> Linearize(const GlyphLibrary& lib, const SpellTree& tree) {
  const int root = RootOf(tree);
  if (root < 0) return {};
  Lin lin{lib, tree};
  FindMarks(lib, lin.laneG, lin.endG);
  // The hand root emits its items and NO delivery word: it is implicit.
  return GlyphsToWords(lib, lin.Scope(root));
}

bool Speakable(const GlyphLibrary& lib, const SpellTree& tree, std::string& why) {
  why.clear();
  const int root = RootOf(tree);
  if (root < 0) return true;
  int laneG = -1, endG = -1;
  const bool marks = FindMarks(lib, laneG, endG);

  // EVERY BOX, EVERY SEGMENT OF IT, carrying the lane depth down — because a
  // wall word is an `end` and whether one may be spoken here depends on
  // whether a lane is open around us. Depth follows the linearizer exactly:
  // a box's shared segment is at its own depth, each of its lanes one deeper.
  std::function<bool(int, int)> walk = [&](int node, int depth) -> bool {
    const SpellNode& n = tree.nodes[node];
    if (n.group) {
      if (n.left >= 0 && !walk(n.left, depth)) return false;
      if (n.right >= 0 && !walk(n.right, depth)) return false;
      return true;
    }
    if (!n.box) return true;
    // A BOX SAID TWICE IS A BOX INSIDE A BOX. Multiplicity on a delivery has
    // no spelling, so a merged pair of identical boxes is not a sentence.
    if (n.n > 1 && n.glyph >= 0) {
      why = "two identical `" + lib.Delivery(n.glyph).id +
            "` boxes side by side cannot be spoken; give one its own lane";
      return false;
    }
    const int32_t L = (int32_t)n.laneAt.size() / 2;
    if (L > 0 && !marks) {
      why = "this library has no `lane` word";
      return false;
    }
    for (int32_t k = 0; k <= L; k++) {
      const int inner = depth + (k == 0 ? 0 : 1);
      std::vector<int> seg;
      for (int ii : n.items)
        if (tree.nodes[ii].lane == k) seg.push_back(ii);
      for (int ii : seg)
        if (!walk(ii, inner)) return false;
      {
        // An ITEM may need a wall of its own (`null end null`), and inside a
        // lane there is none to spend.
        Lin one{lib, tree, laneG, endG, inner};
        for (int ii : seg) one.Item(ii);
        if (one.wallBlocked) {
          why = "that pile needs a wall word, and `end` inside a lane would "
                "close the lane; say it outside the lane";
          return false;
        }
      }
      if (seg.size() < 2) continue;
      // THE SAME SEARCH THE LINEARIZER RUNS. A segment is speakable exactly
      // when some arrangement of it rebuilds it; anything else is a tree no
      // sentence says, however the words are shuffled.
      Lin lin{lib, tree, laneG, endG, inner};
      const std::vector<SegEntry> es =
          SegEntries(lib, tree, seg, [&lin](int ii) { return lin.Item(ii); });
      std::vector<int> order;
      std::vector<char> walls;
      if (ArrangeSegment(lib, es, lin.MayWall(), order, walls)) continue;
      // Name the collision rather than saying "no". These are the ones
      // PLAN_spell_graph §5 calls out, and the ones a player meets.
      std::vector<const SegEntry*> empties;
      int boxy = 0;
      for (const SegEntry& e : es) {
        if (e.boxy) boxy++;
        if (e.emptyLeft) empties.push_back(&e);
      }
      if (empties.size() >= 2) {
        why = "an empty `" + lib.Delivery(tree.nodes[empties[0]->node].glyph).id +
              "` beside an empty `" +
              lib.Delivery(tree.nodes[empties[1]->node].glyph).id +
              "` cannot be spoken; fill one first";
      } else if (boxy > 1) {
        why = "two deliveries in one lane would swallow each other; "
              "give the second a lane of its own";
      } else if (boxy > 0 && !empties.empty()) {
        why = "an empty `" + lib.Delivery(tree.nodes[empties[0]->node].glyph).id +
              "` beside a delivery cannot be spoken; fill it first";
      } else {
        why = "no word order says that pile; give one of its items a lane";
      }
      return false;
    }
    return true;
  };
  return walk(root, 0);
}

// ---- public: layout -----------------------------------------------------------

SpellGraph BuildGraph(const GlyphLibrary& lib, const CastList& list) {
  SpellGraph out;
  const int root = RootOf(list.tree);
  if (root < 0) return out;
  Builder b{lib, list};
  int idx = -1;
  const int w = b.Place(root, 0, 0, 0, idx);
  b.g.root = idx;
  b.g.width = w;
  b.g.layers = b.maxLayer + 1;
  b.g.height = b.g.layers * kGraphPitch;
  // LAYER -> Y, once, at the end: the root is at the maximum y and the tree
  // grows upward, which is the direction the page draws.
  for (SpellGraphNode& n : b.g.nodes) n.y = (b.maxLayer - n.layer) * kGraphPitch;
  return std::move(b.g);
}

// ---- public: the edit ops -----------------------------------------------------

EditResult InsertItem(const GlyphLibrary& lib, const SpellTree& tree, int boxNode,
                      int32_t lane, int glyphId) {
  EditResult r;
  SpellTree t = tree;
  if (!IsBox(t, boxNode)) {
    r.why = "that is not a box";
    return r;
  }
  if (!OpenLaneIfNeeded(t, boxNode, lane, lib.budgets.maxInstances, r.why)) return r;
  const int item = MakeItemFor(lib, t, glyphId, r.why);
  if (item < 0) return r;
  PlaceItem(lib, t, boxNode, lane, item);
  return Finish(lib, tree, t, "that item");
}

EditResult AttachMod(const GlyphLibrary& lib, const SpellTree& tree, int boxNode,
                     int32_t lane, int glyphId) {
  EditResult r;
  const GlyphDef* g = lib.At(glyphId);
  if (!g) {
    r.why = "that word is not in the library";
    return r;
  }
  if (g->sort != GlyphSort::Mod) {
    r.why = "`" + g->id + "` is not a mod";
    return r;
  }
  // ONE SPLIT PER SCOPE, AND THE EDITOR WILL NOT BUILD A SECOND (2026-09-22).
  // Lowering is total and marks the loser wasted - anything the player TYPES
  // has to compile - but a drop is a gesture the editor can decline, and a
  // gesture whose whole effect is "charged, does nothing" is one it should.
  // Same precedent as `ClampedAnywhere`: the page does not help you write a
  // word that cannot do anything.
  if (g->field == ModField::Count && IsBox(tree, boxNode)) {
    for (int ii : tree.nodes[boxNode].items) {
      const GlyphDef* m = lib.At(tree.nodes[ii].glyph);
      if (tree.nodes[ii].group || !m || m->field != ModField::Count) continue;
      if (tree.nodes[ii].lane != lane) continue;
      r.why = lane > 0 ? "that branch already splits; one split per branch"
                       : "this already fans; put the next one on a branch";
      return r;
    }
  }
  return InsertItem(lib, tree, boxNode, lane, glyphId);
}

EditResult FillSlot(const GlyphLibrary& lib, const SpellTree& tree, int groupNode,
                    SlotSide side, int glyphId) {
  EditResult r;
  SpellTree t = tree;
  if (!IsGroup(t, groupNode)) {
    r.why = "that is not an operator";
    return r;
  }
  const GlyphDef* op = lib.At(t.nodes[groupNode].glyph);
  const GlyphDef* g = lib.At(glyphId);
  if (!op || !g) {
    r.why = "that word is not in the library";
    return r;
  }
  const bool left = side == SlotSide::Left;
  if (!(left ? op->hasLeft : op->hasRight)) {
    r.why = "`" + op->id + "` has no " + (left ? "left" : "right") + " slot";
    return r;
  }
  if ((left ? t.nodes[groupNode].left : t.nodes[groupNode].right) >= 0) {
    r.why = "that slot is already filled";
    return r;
  }
  const int item = MakeItemFor(lib, t, glyphId, r.why);
  if (item < 0) return r;
  if (!MaskAccepts(left ? op->leftMask : op->rightMask, NodeSort(lib, t, item))) {
    r.why = "`" + op->id + "` does not take a " +
            GlyphSortName(NodeSort(lib, t, item)) + " there";
    return r;
  }
  if (left) t.nodes[groupNode].left = item;
  else t.nodes[groupNode].right = item;
  // AN OPERAND IS NOT A PILE ITEM. The parser pops it off the pile before
  // `CloseBox` stamps lanes, so it keeps lane 0 whatever lane its group sits
  // in - and `NodeKey` spells the lane, so stamping it here would make the
  // edited tree disagree with its own words.
  t.nodes[item].lane = 0;
  t.nodes[groupNode].complete =
      (!op->hasLeft || t.nodes[groupNode].left >= 0) &&
      (!op->hasRight || t.nodes[groupNode].right >= 0);
  return Finish(lib, tree, t, "that slot");
}

EditResult WrapInBox(const GlyphLibrary& lib, const SpellTree& tree, int node,
                     int deliveryGlyphId) {
  EditResult r;
  SpellTree t = tree;
  if (node < 0 || node >= (int)t.nodes.size()) {
    r.why = "there is nothing there to box";
    return r;
  }
  const GlyphDef* d = lib.At(deliveryGlyphId);
  if (!d || d->sort != GlyphSort::Delivery) {
    r.why = "that is not a delivery";
    return r;
  }
  const Nav nav = BuildNav(t);
  const int parent = nav.parent[node];
  if (parent < 0) {
    // A DELIVERY DROPPED ON THE ROOT BOXES THE WHOLE PILE, which is what a
    // delivery word at the end of a sentence does: the hand keeps exactly one
    // item, the new box, and the lanes go with it because they were the pile's.
    if (!IsBox(t, node)) {
      r.why = "there is nothing there to box";
      return r;
    }
    SpellNode b;
    b.box = true;
    b.glyph = deliveryGlyphId;
    b.n = 1;
    b.lane = 0;
    b.items = t.nodes[node].items;
    b.laneAt = t.nodes[node].laneAt;
    t.nodes.push_back(b);
    const int boxIdx = (int)t.nodes.size() - 1;
    t.nodes[node].items.assign(1, boxIdx);
    t.nodes[node].laneAt.clear();
    return Finish(lib, tree, t, "that box");
  }
  const int32_t lane = t.nodes[node].lane;
  SpellNode b;
  b.box = true;
  b.glyph = deliveryGlyphId;
  b.n = 1;
  b.lane = lane;
  b.items.push_back(node);
  t.nodes.push_back(b);
  const int boxIdx = (int)t.nodes.size() - 1;
  t.nodes[node].lane = 0;   // it is the new box's shared pile now
  if (nav.slot[node] < 0) {
    std::vector<int>& items = t.nodes[parent].items;
    for (int& ii : items)
      if (ii == node) ii = boxIdx;
  } else if (nav.slot[node] == 0) {
    t.nodes[parent].left = boxIdx;
  } else {
    t.nodes[parent].right = boxIdx;
  }
  return Finish(lib, tree, t, "that box");
}

EditResult Unbox(const GlyphLibrary& lib, const SpellTree& tree, int boxNode) {
  EditResult r;
  SpellTree t = tree;
  if (!IsBox(t, boxNode)) {
    r.why = "that is not a box";
    return r;
  }
  const Nav nav = BuildNav(t);
  const int parent = nav.parent[boxNode];
  if (parent < 0) {
    r.why = "the hand has no delivery to drop";
    return r;
  }
  if (nav.slot[boxNode] >= 0) {
    r.why = "that box is an operator's operand; remove it instead";
    return r;
  }
  const int32_t lane = t.nodes[boxNode].lane;
  const std::vector<int> lifted = t.nodes[boxNode].items;
  std::vector<int>& items = t.nodes[parent].items;
  items.erase(std::remove(items.begin(), items.end(), boxNode), items.end());
  // Its own lanes flatten into the scope it is lifted into, exactly the way an
  // unboxed lane flattens when it closes: there is no record left for them to
  // be columns of.
  for (int ii : lifted) {
    t.nodes[ii].lane = lane;
    items.push_back(ii);
  }
  // An EMPTY box lifted out of a lane leaves that lane with nothing in it.
  PruneTrailingEmptyLanes(t, parent);
  return Finish(lib, tree, t, "unboxing that");
}

EditResult Remove(const GlyphLibrary& lib, const SpellTree& tree, int node) {
  EditResult r;
  SpellTree t = tree;
  if (node < 0 || node >= (int)t.nodes.size()) {
    r.why = "there is nothing there to remove";
    return r;
  }
  const Nav nav = BuildNav(t);
  if (nav.parent[node] < 0) {
    r.why = "the hand cannot be removed";
    return r;
  }
  const int owner = OwningBox(t, nav, node);
  Detach(t, nav, node);
  PruneTrailingEmptyLanes(t, owner);
  return Finish(lib, tree, t, "removing that");
}

EditResult CloseLane(const GlyphLibrary& lib, const SpellTree& tree, int boxNode,
                     int32_t lane) {
  EditResult r;
  SpellTree t = tree;
  if (!IsBox(t, boxNode)) {
    r.why = "that is not a box";
    return r;
  }
  const int32_t L = (int32_t)t.nodes[boxNode].laneAt.size() / 2;
  if (lane < 1) {
    r.why = "the shared pile is not a lane; take its words off instead";
    return r;
  }
  if (lane > L) {
    // The socket is real, but it is an instance a `count` mod made, not a lane:
    // there is no `lane` / `end` pair to take out, and the thing that would
    // close it is the mod.
    r.why = "this instance has no lane of its own; it comes from a count mod";
    return r;
  }
  // THE LANE AND WHAT IS IN IT, and then every lane above it slides down one —
  // which is the whole reason this is a gesture and not a side effect. Closing
  // lane 2 of three moves the third bolt onto the second socket, and only the
  // player can say that is what they meant.
  std::vector<int>& items = t.nodes[boxNode].items;
  std::vector<int> kept;
  for (int ii : items) {
    const int32_t ln = t.nodes[ii].lane;
    if (ln == lane) continue;
    if (ln > lane) t.nodes[ii].lane = ln - 1;
    kept.push_back(ii);
  }
  items = kept;
  t.nodes[boxNode].laneAt.pop_back();
  t.nodes[boxNode].laneAt.pop_back();
  // Closing the last full lane can leave bare ones behind it with nothing left
  // to hold them in place.
  PruneTrailingEmptyLanes(t, boxNode);
  return Finish(lib, tree, t, "closing that lane");
}

EditResult Move(const GlyphLibrary& lib, const SpellTree& tree, int node, int boxNode,
                int32_t lane, bool copy) {
  EditResult r;
  SpellTree t = tree;
  if (node < 0 || node >= (int)t.nodes.size()) {
    r.why = "there is nothing there to move";
    return r;
  }
  if (!IsBox(t, boxNode)) {
    r.why = "that is not a box";
    return r;
  }
  if (InSubtree(t, node, boxNode)) {
    r.why = "a spell cannot be moved inside itself";
    return r;
  }
  const Nav nav = BuildNav(t);
  if (!copy && nav.parent[node] < 0) {
    r.why = "the hand cannot be moved";
    return r;
  }
  if (!OpenLaneIfNeeded(t, boxNode, lane, lib.budgets.maxInstances, r.why)) return r;
  int moved = node;
  const int owner = copy ? -1 : OwningBox(t, nav, node);
  if (copy) {
    moved = CloneSubtree(t, node);
  } else {
    Detach(t, nav, node);
  }
  PlaceItem(lib, t, boxNode, lane, moved);
  // AFTER the placement, never before: dragging the last item of a box's top
  // lane into another lane of the SAME box empties the one it left, and pruning
  // first would renumber the destination out from under `lane`.
  PruneTrailingEmptyLanes(t, owner);
  return Finish(lib, tree, t, "that move");
}
