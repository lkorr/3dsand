#pragma once
#include <string>
#include <vector>

#include "game/spell.h"

// spellgraph — THE SPELL AS A DRAWING, AND THE DRAWING AS A SENTENCE
// (docs/PLAN_spell_graph.md §4–§6, phase 2).
//
// Three things live here and nothing else:
//
//   1. LAYOUT. `BuildGraph` turns a lowered `CastList` into a rooted, layered
//      drawing: nodes with integer chrome-pixel boxes and edges between them.
//      The hand box is the root at the BOTTOM (largest y) and the tree grows
//      upward, which is the direction the user asked for.
//   2. THE LINEARIZER. `Linearize` turns a `SpellTree` back into a word list.
//      THE ROUND-TRIP LAW: for every sentence s the grammar can say,
//      `Parse(Linearize(Parse(s)))` is `Parse(s)` node for node and lowers to
//      the same cast. The `spell-graph` gate asserts it over the whole oracle
//      corpus and over every generated sequence of length <= 3.
//   3. TREE EDIT OPS. Every gesture the page will offer is one TOTAL function
//      over (tree, target, glyph) that returns either the new WORD LIST or a
//      reason it was refused. The tree is never the saved thing: words are.
//
// IMGUI-FREE AND VM-SIDE ON PURPOSE. Nothing here draws, and nothing here is
// allowed to reach into `src/ui`. That is what makes the whole surface
// testable from a CPU-only gate in milliseconds (`--gate spell-graph`) instead
// of from a screenshot.
//
// ORDERING A SEGMENT IS A SEARCH, NOT A SORT, and that is the subtle part.
// Four rules say which item may follow which inside one segment of a pile (the
// shared segment, or one lane), and they are spelled out over `CanFollow` in
// the .cpp: the one item that speaks a DELIVERY of its own must be first
// (its delivery word boxes whatever is already in the pile); a group with an
// empty LEFT slot may only follow an item its mask REJECTS; a group with an
// empty RIGHT slot may only be followed by a word its mask rejects; and no
// item may start with the word the previous one ended with. `trail echo` and
// `echo trail` are two different trees for exactly this reason.
//
// THE GRAMMAR HAS A WALL WORD AND THE LINEARIZER SPENDS IT. An `end` with no
// lane open is a charged no-op of word cost 0, and a sentence already uses one
// that way: `transmute end fire` is how you say "transmute's right slot stays
// empty and the fire is beside it, not inside it", and `null end null` is how
// you say one `null` took another rather than `null` twice. Rules 3 and 4 are
// about the word LITERALLY next in the sentence, so a wall answers both; rules
// 1 and 2 are about the PILE, which a no-op does not touch, so nothing saves
// those but a different order. A wall is only spoken where no lane is open,
// because inside one an `end` would close it — which is itself a refusal.
//
// WHAT THE GRAMMAR CANNOT SAY is then simply: a segment for which no
// arrangement satisfies those four. Two shapes reach it in practice — two
// items containing a delivery in one segment (the second box would swallow
// the first; two independent deliveries are two LANES, which is what rule 4
// exists for), and two incomplete unary operators where EACH accepts the
// other's result sort (`(_ echo)` beside `(_ null)`), since whichever is
// spoken second binds the first. `Speakable()` runs the same search and names
// the collision; every op checks it and then PROVES itself by re-parsing its
// own output.

// ---- chrome geometry ---------------------------------------------------------
// Integer pixels at 2x (the UI authors sprites at 1x and draws them doubled;
// see PLAN_spell_graph §4). The UI may scale these; it may not round them.
constexpr int kGraphCell = 64;        // a word / operator cell, square
constexpr int kGraphGap = 32;         // minimum gap between sibling subtrees
constexpr int kGraphPitch = 96;       // vertical distance between layers
constexpr int kGraphBarH = 64;        // a join bar's height
constexpr int kGraphMinBarW = 128;    // a join bar is never narrower than this
constexpr int kGraphSocketW = 32;     // a socket column's minimum width
constexpr int kGraphSocketH = 32;
constexpr int kGraphBusH = 16;        // the shared bus stroke's box
constexpr int kGraphTagW = 96;        // a mod tag hanging off a bar's left end
constexpr int kGraphTagH = 32;

// ---- the graph ----------------------------------------------------------------

enum class GraphKind : uint8_t {
  Word = 0,   // a Matter / Effect leaf
  Operator,   // an operator cell with its pips
  Join,       // a spoken delivery box: a bar, its sockets, its bus
  ModTag,     // a pending Mod, hanging off the join it stuck to
  Root,       // the hand bar
  Socket,     // synthesized: one instance of a join (treeNode == -1)
  Bus,        // synthesized: what the shared items feed (treeNode == -1)
};

enum class GraphEdge : uint8_t {
  Trunk = 0,  // child -> parent (a bus or a tag into its join)
  Bus,        // a shared item -> the bus
  Socket,     // a lane's subtree -> its socket
  Fan,        // a socket -> the bar
  Slot,       // an operand -> its operator's pip
};

struct SpellGraphNode {
  GraphKind kind = GraphKind::Word;
  int treeNode = -1;          // index into SpellTree::nodes; -1 = synthesized
  int glyph = -1;             // library index; on a join, the delivery (-1 = hand)
  int32_t n = 1;              // multiplicity
  std::string label;          // what the cell says ("fire", "PROJECTILE", "hand")
  GlyphSort sort = GlyphSort::Matter;
  int32_t lane = 0;           // which lane of its parent box it sits in (0 = shared)

  // Box, in chrome pixels. y grows DOWNWARD: the root is at the maximum y.
  int x = 0, y = 0, w = 0, h = 0;
  int layer = 0;              // 0 = the root's layer, increasing upward
  // THE WHOLE SUBTREE's horizontal extent, which is what a drop test and the
  // parent-containment law want: an operator CELL is narrower than the row of
  // operands above it, and a join's BAR does not cover the mod tags hanging
  // off its left end, so `x`/`w` alone answers neither question.
  int subX = 0, subW = 0;

  // ---- Operator -------------------------------------------------------------
  bool hasLeft = false, hasRight = false;      // which slots the word declares
  bool leftFilled = false, rightFilled = false;  // hollow pip where false
  bool complete = true;       // a required slot is empty: charged, does nothing

  // ---- Join / Root ----------------------------------------------------------
  int32_t instances = 1;      // max(count, lanes), clamped: how many sockets
  int32_t laneCount = 0;      // how many lanes the box closed
  std::vector<int> sockets;   // graph indices, in INSTANCE order (0..instances-1)
  int bus = -1;               // graph index of the bus, -1 when nothing is shared
  // The bar itself, which is narrower than the node when mod tags hang off its
  // left end. `x`/`w` is the bar; the tags occupy [x - tagStripW, x).
  int tagStripW = 0;
  BoxPrice price;             // copied from CastList::PriceOf(treeNode)
  bool hasPrice = false;
  int32_t subtotal = 0;       // price.tariff + price.carryCost

  // ---- Socket ---------------------------------------------------------------
  // Which instance this socket is. Instance 0 is the aim, and it is drawn in
  // the CENTRE slot of the row (exactly on the bar's centre line when
  // `instances` is odd), so the first lane the player opened is the middle
  // bolt — which is what rule 4 promises.
  int32_t instance = -1;
  int pipW = kGraphSocketW;   // the drawn pip; `w` is the socket's whole column

  // ---- ModTag ---------------------------------------------------------------
  std::string edit;           // the field edit, composed: "speed x2", "count x9"
  bool wasted = false;        // the cast listed it in wastedMods: charged no-op

  // The spoken span this node covers, for the HUD word highlight.
  int spanFirst = -1, spanLast = -1;
};

struct SpellGraphEdge {
  int from = -1, to = -1;
  GraphEdge kind = GraphEdge::Trunk;
};

struct SpellGraph {
  std::vector<SpellGraphNode> nodes;
  std::vector<SpellGraphEdge> edges;
  int root = -1;              // the hand bar, at the maximum y
  int width = 0, height = 0;  // the drawing's extent, chrome pixels
  int layers = 0;
};

// The drawing of a lowered spell. Reads `list.tree` for the shape and
// `list.boxPrice` / `list.casts` for the numbers under every bar. Total: an
// empty cast list draws an empty graph.
SpellGraph BuildGraph(const GlyphLibrary& lib, const CastList& list);

// ---- the linearizer ----------------------------------------------------------

// The tree as WORDS (glyph ids), post-order, in the canonical order of
// PLAN_spell_graph §5. Total: it always returns a word list, even for a tree
// no sentence could have produced — ask `Speakable` first if you need to know.
std::vector<std::string> Linearize(const GlyphLibrary& lib, const SpellTree& tree);

// Is this tree one that some sentence says? Fills `why` with a player-readable
// reason when it is not. The two unspeakable shapes are named in the file
// comment above.
bool Speakable(const GlyphLibrary& lib, const SpellTree& tree, std::string& why);

// Words -> glyph indices (unknown names dropped) and back, so callers do not
// each write the loop.
std::vector<int> WordsToGlyphs(const GlyphLibrary& lib,
                               const std::vector<std::string>& words);
std::vector<std::string> GlyphsToWords(const GlyphLibrary& lib,
                                       const std::vector<int>& glyphs);
// Parse a word list. `Linearize(ParseWords(lib, w))` is the canonical
// respelling of `w`.
SpellTree ParseWords(const GlyphLibrary& lib, const std::vector<std::string>& words);
// A blank page: one clause whose root is the empty hand box. `ParseSpell` of
// silence has no clause at all (silence is not a spell), so the editor needs
// this to have something to drop the first word onto.
SpellTree EmptyTree();

// THE SPAN-AGNOSTIC IDENTITY OF A TREE: every box as its delivery plus its pile
// SORTED plus its lane count, all the way down, with each item's lane. Two
// trees with the same key are the same spell however the words were ordered —
// which is what the round-trip law compares.
std::string GraphTreeKey(const GlyphLibrary& lib, const SpellTree& tree);

// ---- the edit ops (PLAN_spell_graph §5) ---------------------------------------
//
// EVERY OP IS TOTAL: it either returns `ok` with the new word list, or `ok ==
// false` with a reason the status line can show. None of them mutates the tree
// they are given; each works on a copy, checks the caps and `Speakable`, and
// then PROVES itself by re-parsing its own words and comparing `GraphTreeKey`.
// An op whose output does not re-parse to what it intended is refused rather
// than returned, so a caller can never write words back that mean something
// else.

struct EditResult {
  bool ok = false;
  std::string why;                  // non-empty exactly when !ok
  std::vector<std::string> words;   // the new word list, on success
};

// Which side of an operator a slot is.
enum class SlotSide : uint8_t { Left = 0, Right };

// Put a glyph into a box's pile. `lane` 0 is the shared segment; `lane ==
// laneCount + 1` OPENS a new lane, and anything past that is refused. A
// Delivery glyph goes in as an EMPTY BOX (the kinetic hit); an Operator goes in
// as a group with empty slots; a Separator is refused (lanes are structure, not
// items).
EditResult InsertItem(const GlyphLibrary& lib, const SpellTree& tree, int boxNode,
                      int32_t lane, int glyphId);
// The same op, named for the gesture: a Mod dropped on a join or a socket.
EditResult AttachMod(const GlyphLibrary& lib, const SpellTree& tree, int boxNode,
                     int32_t lane, int glyphId);
// Fill an operator's empty pip. Refused when the slot does not exist, is
// already filled, or the glyph's sort is outside the slot's mask.
EditResult FillSlot(const GlyphLibrary& lib, const SpellTree& tree, int groupNode,
                    SlotSide side, int glyphId);
// Box ONE item under a delivery; its siblings stay in the outer scope. When the
// target is itself a box this NESTS (a bolt that fires this).
EditResult WrapInBox(const GlyphLibrary& lib, const SpellTree& tree, int node,
                     int deliveryGlyphId);
// Drop a box's delivery and lift its items into the scope around it, keeping
// the box's own lane. Its lanes flatten into that scope the way an unboxed
// lane flattens when it closes.
EditResult Unbox(const GlyphLibrary& lib, const SpellTree& tree, int boxNode);
// Remove a subtree. Removing the last item of a lane KEEPS the (now empty)
// lane: the instance still exists and carries the shared payload.
EditResult Remove(const GlyphLibrary& lib, const SpellTree& tree, int node);
// Move (or, with `copy`, duplicate) a subtree into another box's lane.
EditResult Move(const GlyphLibrary& lib, const SpellTree& tree, int node,
                int boxNode, int32_t lane, bool copy);
