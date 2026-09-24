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
//
//      THE DRAWING READS IN WORD ORDER, BOTTOM TO TOP (2026-09-21). A box's
//      bar sits ABOVE its contents, because that is where its word sits in the
//      sentence: you say `fire projectile`, so `fire` is the lower cell and
//      PROJECTILE is the one over it. Containment used to drive the stack, so
//      the same sentence drew PROJECTILE underneath FIRE and the picture read
//      backwards from the words that made it. Same for an operator: its
//      operands are the row BELOW its cell.
//
//      THE HAND IS THE ONE EXCEPTION, AND IT IS NOT A SPECIAL CASE. `hand` is
//      the implicit outermost box (spell.cpp rule 3) — the one delivery nobody
//      ever speaks — so it has no place in a word-order stack. It is the
//      PEDESTAL: its bar at layer 0, its sockets and bus just above it, and
//      the whole spoken sentence standing on top. `flip` in `Place` is exactly
//      "is this box a spoken delivery".
//
//      A SPLIT OPENS FORWARD, WHICHEVER END THAT IS (2026-09-22). A box with
//      more than one instance synthesizes a SPLIT junction, placed immediately
//      below its socket row in BOTH orders, and the fan is `socket -> split`.
//      The fan used to be `socket -> bar` with the pips hard against the bar,
//      which on the pedestal drew three strokes rising out of the hand and on
//      a spoken delivery drew the same three LEAVING THE CELL going down - one
//      construct, two pictures, and the second one said the projectile split.
//      It did not: the shotgun did, and now the drawing says so.
//
//      ...AND THE SHOTGUN *IS* THE JUNCTION (2026-09-22, second pass). The
//      word that opens the fan was a 96x32 bead one band below a 32x16 blot -
//      14 screen px and 4 at the fit rung, between two full 64 px cells - so
//      the branch point of the spell was the least legible mark on the page.
//      A record-wide `count` mod is not a thing that EDITS the junction, it is
//      the junction, so the Split node carries that mod's tree node, glyph,
//      edit and span and is drawn at CELL size. Fusing rather than growing the
//      bead is what keeps law (2) - a junction stands on the box's axis - true
//      for free: a junction is placed there by construction and a bead row of
//      two is not. Every WINNER fuses, at every depth: a fan opened by lanes
//      alone has no word to wear and stays the blot, a count inside a lane is
//      that BRANCH's junction (see the cascade below), and a losing count word
//      - `twin` beside a `shotgun` - stays a slashed bead, which is the whole
//      point of drawing it.
//
//      ...AND THE FAN CASCADES (2026-09-22, third pass). `shotgun lane twin
//      end` drew three sockets with a `twin` bead over the middle one and
//      nothing above it: the second split was in the words, in the price and in
//      the number of bolts that fly, and the page said nothing about it (the
//      tally it used to wear lives on a delivery cell, and the `hand` has none).
//      A branch that fires more than one bolt now gets its OWN junction and its
//      OWN socket row inside its column, by the same three rules - junction on
//      the column's axis, sockets one band forward, `Fan` pointing back - and
//      its delivery is drawn once per BOLT. Depth beyond that comes from
//      nesting BOXES, which recurses through `Place` already. `laneSplits[k]`
//      is the per-branch count and the VM has always had it.
//
//      ...AND A SPOKEN DELIVERY IS DRAWN ONCE PER BRANCH - once per BOLT where
//      a branch split again (2026-09-22). Everything above the
//      socket row belongs to one branch each, so each branch rises through its
//      own payload to its own copy of the delivery cell. They are one word and
//      one record: the instance-0 copy is the PRIMARY and owns `sockets`,
//      `bus`, `split`, the price and the box's whole span; the rest carry
//      `primary`. The `hand` needs no copies - it is the pedestal the branches
//      have not diverged from yet - so its one cell stays at the bottom.
//
//      AN EMPTY SLOT IS A HOLE IN THE OPERAND ROW (2026-09-22). `_ trail` and
//      `_ mend` are the words that do nothing until something is spoken before
//      them, and the page used to say so with a 21-px hollow ring hung off the
//      LEFT EDGE of the cell - the smallest mark in the drawing, in the one
//      place the layout otherwise uses for nothing, carrying the one fact the
//      player most needs. An operand is the row BELOW its operator, so a
//      MISSING operand is a missing cell in that row: a `Hole` node, cell-sized,
//      on the operand band, with its own Slot edge up into the word it belongs
//      to. The operator and its row then read as one fused figure with a gap in
//      it rather than as a finished cell with chrome bolted to its side.
//
//      A BOX HANDS OFF TO ITS PARENT FROM THE BOTTOM OF ITS SPAN, not from its
//      bar. `baseLayer` is that layer. The bar of a spoken box is at the TOP of
//      its own span, so an edge drawn from the bar itself would fall the whole
//      height of the subtree and cross every cell in it; anchored at
//      `baseLayer` the stroke is the short one between two neighbouring bands,
//      and the drawing reads as one spine running up the page.
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
// A DELIVERY IS A CELL, NOT A SLAB (2026-09-21). The bar used to be 128x64 and
// to stretch across its whole socket row, which made the two most common nodes
// on the page — PROJECTILE and the hand — the only rectangles in a drawing
// otherwise made of 64 px squares, and made them read as chrome rather than as
// words. They are square cells now like every other glyph; the reach the slab
// used to show is a thin rule the CANVAS draws behind the cell, spanning the
// socket row (`SpellGraphNode::sockets` gives it the extent, so no field).
constexpr int kGraphSocketW = 32;     // a socket column's minimum width
constexpr int kGraphSocketH = 32;
constexpr int kGraphBusH = 16;        // the shared bus stroke's box
constexpr int kGraphTagW = 96;        // a mod tag, a bead on the bar's trunk
constexpr int kGraphTagH = 32;
constexpr int kGraphSplitW = 32;      // the junction a fan diverges out of
constexpr int kGraphSplitH = 16;
// THE WIDEST ROW THE PAGE WILL DRAW. `maxInstances` is 27, and a split branch
// now ends in a 64 px cell rather than a 32 px pip, so 27 columns is 2560
// layout px - readable at the 0.25x overview rung. What has to be bounded is
// NESTING: a split inside a split inside a split multiplies columns, and a
// 32-word sentence buys enough of them to make a drawing nobody can find the
// spell in. THE FAN CASCADES (2026-09-22): a branch that splits again gets its
// own junction and its own socket row inside its column, and one cell per BOLT
// - so the columns to bound are `sum(laneSplits)`, not `instances`. A box whose
// sub-fans would push it past this keeps the older drawing, one cell wearing
// its tally; no legal 32-word sentence reaches that, and it is a guard, not a
// path.
constexpr int kGraphMaxColumns = 27;

// ---- the graph ----------------------------------------------------------------

enum class GraphKind : uint8_t {
  Word = 0,   // a Matter / Effect leaf
  Operator,   // an operator cell with its pips
  Join,       // a spoken delivery box: a bar, its sockets, its bus
  ModTag,     // a pending Mod, a bead on the trunk over the join it stuck to
  Root,       // the hand bar
  Socket,     // synthesized: one instance of a join (treeNode == -1)
  Bus,        // synthesized: what the shared items feed (treeNode == -1)
  Split,      // synthesized: the junction a fan diverges out of (treeNode == -1)
  Hole,       // synthesized: an operator's EMPTY slot, standing in the operand
              // row as a cell-sized blank. `treeNode` is the OPERATOR whose slot
              // it is (not -1: a drop on it fills that slot) and `instance` is
              // the side, 0 left / 1 right.
};

enum class GraphEdge : uint8_t {
  Trunk = 0,  // child -> parent (a bus or a mod bead into its join)
  Bus,        // a shared item -> the bus
  Socket,     // a lane's subtree -> its socket
  Fan,        // a socket -> the SPLIT it diverges out of
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
  // THE LAYER THIS NODE HANDS OFF TO ITS PARENT ON: the LOWEST layer its whole
  // subtree occupies. Equal to `layer` for anything a single band tall; for a
  // spoken box (bar at the top of its span) and for an operator (cell above its
  // operands) it is the bottom of the span, which is where the edge into the
  // parent starts. See the header note.
  int baseLayer = 0;
  // THE WHOLE SUBTREE's horizontal extent, which is what a drop test and the
  // parent-containment law want: an operator CELL is narrower than the row of
  // operands above it, and a join's BAR is narrower than a mod bead row wider
  // than it, so `x`/`w` alone answers neither question.
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
  BoxPrice price;             // copied from CastList::PriceOf(treeNode)
  bool hasPrice = false;
  int32_t subtotal = 0;       // price.tariff + price.carryCost
  // ONE RECORD, SEVERAL CELLS (2026-09-22). A split delivery is drawn once per
  // BRANCH - three bolts are three cells, each capping its own column - and all
  // of those cells are one word, one `treeNode`, one record. Exactly one of
  // them is the PRIMARY: it carries `sockets`, `bus`, `split`, `price` and the
  // whole box's `subX`/`subW`, and it is the one a parent edges into. A copy
  // carries `primary` = the primary's graph index and `instance` = which branch
  // it caps; everything else on it is empty. An UNSPLIT box has one cell with
  // `primary == -1` and `instance == -1`, which is byte-identical to what this
  // struct held before the field existed.
  int primary = -1;           // -1 = this cell owns the record
  int split = -1;             // graph index of this box's Split junction, or -1
  // ---- Socket, Bus, Split ---------------------------------------------------
  // THE BOX THIS SYNTHESIZED MARK BELONGS TO, as a graph index of its PRIMARY
  // cell. A back-pointer rather than a search, because a sub-fan's pip is in no
  // box's `sockets` list and the search could not find it (2026-09-22).
  int owner = -1;
  // HOW MANY BOLTS THIS BRANCH FIRES, when the sub-fan could NOT be drawn as
  // its own row of cells (past the column budget): the branch is collapsed to
  // one cell wearing a tally. 1 everywhere else - including every branch of a
  // drawn sub-fan, where each bolt has a cell of its own and the fan says it.
  int32_t bolts = 1;

  // ---- Socket, and a lane's ModTag ------------------------------------------
  // Which instance this socket is — or, on a ModTag, which instance the mod
  // edits (-1 when it is record-wide), or, on a Join, which branch this cell
  // caps (-1 when the box is unsplit). Instance 0 is the aim, and it is drawn in
  // the CENTRE slot of the row (exactly on the bar's centre line when
  // `instances` is odd), so the first lane the player opened is the middle
  // bolt — which is what rule 4 promises.
  int32_t instance = -1;
  int pipW = kGraphSocketW;   // the drawn pip; `w` is the socket's whole column

  // ---- ModTag ---------------------------------------------------------------
  std::string edit;           // the field edit, composed: "speed x2", "count x9"
  // The cast listed this word as a charged no-op - either by GLYPH (its field
  // means nothing on this record) or by NODE (a second count word in a scope
  // that already fans, 2026-09-22). The second needs the node, because the
  // whole point is that one `shotgun` works and its twin does not.
  bool wasted = false;

  // ---- Word, Operator, ModTag: MAGNITUDE (PLAN_spell_magnitude §2.5) ------
  // The word's magnitude, per-mille; whether the wheel may set it and over
  // what lattice; and what it reads as in units ("gravity -0.50 g").
  int32_t mag = kMagOne;
  bool graded = false;
  int32_t magMin = kMagOne, magMax = kMagOne, magStep = kMagOne;
  int32_t magDefault = kMagOne;
  std::string magLabel;
  // Word / Operator / Join: WHEN it fires in its carrier (M3).
  SpellTiming timing;

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
// Serialized words (`float@0.5`) -> a spoken stack with its magnitudes;
// unknown names dropped, magnitudes clamped to each glyph's range.
SpellStack WordsToStack(const GlyphLibrary& lib, const std::vector<std::string>& words);
// Parse a word list. `Linearize(ParseWords(lib, w))` is the canonical
// respelling of `w`, magnitudes included.
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
// Remove a subtree. Removing the last item of a lane drops the lane too WHEN IT
// IS THE LAST ONE, cascading down through any bare lanes under it — a fan comes
// apart the way it went together. An INTERIOR lane survives its last item,
// because lanes are positional and closing one would move every bolt above it
// onto a different socket; `CloseLane` is the gesture that says to do that.
EditResult Remove(const GlyphLibrary& lib, const SpellTree& tree, int node);
// Close one lane of a box, with whatever is in it, and slide the lanes above it
// down. Refused on lane 0 (the shared pile is not a lane) and on an instance
// that has no lane of its own — a `count` mod's extra sockets are not lanes and
// what closes them is the mod.
EditResult CloseLane(const GlyphLibrary& lib, const SpellTree& tree, int boxNode,
                     int32_t lane);
// Put a whole WORD LIST (a grimoire page, serialized) into a box's lane as the
// subtree it parses to: `fire projectile` arrives as a bolt that sprays fire,
// not as `fire` beside an empty bolt, and every word keeps its magnitude and
// timing. A page's own root lanes become new lanes of the box when it lands on
// the shared pile (lane 0), and fold into the lane otherwise.
EditResult InsertWords(const GlyphLibrary& lib, const SpellTree& tree, int boxNode,
                       int32_t lane, const std::vector<std::string>& words);
// SET A WORD'S MAGNITUDE (PLAN_spell_magnitude §2.5): the wheel over a cell.
// `mag` is per-mille and is clamped and snapped to the glyph's range; refused
// on a box, on an ungraded word, and when the clamp leaves it where it was
// (the end of the range), with the reason naming the limit.
EditResult SetMagnitude(const GlyphLibrary& lib, const SpellTree& tree, int node,
                        int32_t mag);
// SET AN ITEM'S TIMING (M3): when a payload item - a word, an operator group,
// or a nested box - fires in the carrier that holds it. Refused on anything
// that is not an item of a box, on a mod (it edits its carrier), and for any
// trigger but `hit` when the carrier does not fly (a delay is fine anywhere).
EditResult SetTiming(const GlyphLibrary& lib, const SpellTree& tree, int node,
                     SpellTiming timing);
// Move (or, with `copy`, duplicate) a subtree into another box's lane. A move
// that empties the lane it came from unwinds it exactly as `Remove` does.
EditResult Move(const GlyphLibrary& lib, const SpellTree& tree, int node,
                int boxNode, int32_t lane, bool copy);
