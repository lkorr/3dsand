#include "ui/spellgraph_ui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ui/theme.h"

// THE PAGE, DRAWN (docs/PLAN_spell_graph.md §4-§5).
//
// Everything here is a pure function of `UIState::spellGraph` plus the drag in
// flight. The layout arrived already solved — `BuildGraph` laid every node out
// in integer CHROME PIXELS (2x), so this file only picks an integer scale, maps
// graph coordinates to screen ones, and draws. No node position is computed
// here, which is why a hit test is exact and why the gate that tests the layout
// (`--gate spell-graph`) tests what the player sees.
//
// THE SCALE IS A LADDER - 0.5x, 1x, 2x, 3x, 4x - AND NOTHING BETWEEN. A smooth
// zoom would put chrome on half pixels, which is the one thing the pixel-art
// rule forbids. The view opens on the FIT (1x if the drawing stands in the
// band, else 0.5x) and stays there until you touch it: the wheel steps the
// ladder about the cursor, a drag on the dark between the nodes pans, and a
// double-click on it puts the fit back. The scrollbars this replaced could
// only ever show you the drawing at the size it had already failed to fit at.
//
// THE CANVAS CARRIES NO WORDS (2026-09-21). Every node is a SYMBOL — the sort's
// engraving over the glyph's own colour — and every name, price, multiplicity
// note and field edit is a HOVER away in the info box that already existed.
// The tree used to spell each leaf under its cell, caps the delivery's noun
// across every bar and hang a "tariff 441 + carry 882  x3 = 1323" line under
// it, and at any width the panel actually gets, all three collided, clipped and
// overlapped the strokes. The drawing is the SHAPE of the spell; the numbers
// belong on the price bar under the canvas, where nothing can clip them, and
// the names belong to the thing under the cursor.
//
// AND IT IS CENTRED. A drawing narrower than the band sits in the middle of it,
// not hard against the left edge — the tree is a figure, and a figure hugging
// one wall of its frame reads as a layout bug. Only a drawing too big to fit
// scrolls, and only then does the view frame the foot.
//
// IT READS UPWARD, IN WORD ORDER (2026-09-21). `BuildGraph` puts a spoken
// delivery's cell ABOVE the pile it boxes, because that is where its word sits
// in the sentence — you type `fire projectile`, so FIRE is the lower cell. The
// implicit `hand` is the one delivery nobody speaks, so it is the pedestal at
// the bottom and everything stands on it. Two things here follow from that and
// nothing else does: an edge picks its anchors from the two bands rather than
// assuming the child is on top, and it leaves from the child's SPAN bottom
// (`baseLayer`) so a box hands off at the foot of its own stack instead of
// dropping a stroke from its cell through everything it contains.

namespace ui {

namespace {

// Graph node kinds, mirrored from game/spellgraph.h's GraphKind. The mirror
// carries the enum as an int because ui/overlay.h is spell-free.
enum Kind {
  kWord = 0, kOperator, kJoin, kModTag, kRoot, kSocket, kBus, kSplit, kHole
};
// Edge kinds, from GraphEdge.
enum EKind { kTrunk = 0, kEBus, kESocket, kFan, kSlot };
// Glyph sorts, from GlyphSort.
enum Sort { kMatter = 0, kEffect, kDelivery, kMod, kOpSort, kSeparator };

const char* kSortLabels[6] = {"matter", "effect",   "delivery",
                              "mod",    "operator", "separator"};

// The two layout constants this file reasons about, mirrored from
// game/spellgraph.h rather than included from it (the canvas draws a mirror and
// may not reach into the VM). They are the CELL's edge and the LAYER's pitch in
// chrome pixels; `check_invariants.py` has no rule for them, so if
// `kGraphCell` / `kGraphPitch` ever move, these move with them.
constexpr int kCellPx = 64;
constexpr int kPitchPx = 96;
// `kGraphGap`: the space between siblings. The canvas needs it because a
// socket's DROP TARGET is its column plus half a gap on each side, which is
// what makes the socket row tile with no dead pixels between the pips.
constexpr int kGapPx = 32;
// The air under a cell inside its band. There used to be a 13 px line here for
// the word's NAME; the names are in the info box now, and the band is tighter
// by exactly that much — which is most of why a three-layer tree stands at 1x
// in a composer that could only ever show two.
constexpr float kBandAir = 10.0f;

// ONE SILHOUETTE PER SORT. Three of the five used to fall back to
// `glyph_modifier`, so an effect, an operator and a mod were the same picture
// and only the rim colour told them apart — survivable while every cell was
// captioned, fatal the moment the captions went.
const char* GlyphIcon(int type) {
  switch (type) {
    case kMatter: return "glyph_element";     // a droplet: the noun
    case kEffect: return "glyph_spark";       // a four-point star: the verb
    case kDelivery: return "glyph_form";      // an arrow: what it becomes
    case kOpSort: return "glyph_bond";        // brackets round a bar
    default: return "glyph_modifier";         // a spiral tick: mod, separator
  }
}

// ---- THE LEAF ---------------------------------------------------------------
//
// The ink a sort is drawn in ON VELLUM, which is not the colour it is drawn in
// on the obsidian chrome next door (`SortColour`, still used by the word row
// and the arsenal). Half that palette - parchment for mod, pale gold for the
// operators - is invisible on a light page; these are the pigments an
// illuminator actually ground, chosen so the five sorts stay apart at the 16
// px a cell gets at the overview rung.
ImU32 SortPigment(int sort) {
  switch (sort) {
    case kMatter: return ColVerdigris();   // matter: verdigris green
    case kEffect: return ColRubric();      // effect: vermilion, the rubricator's
    case kDelivery: return ColAzurite();   // delivery: azurite blue
    case kMod: return ColOrpiment();       // mod: orpiment ochre
    default: return ColIronGall();         // operator, separator: the plain ink
  }
}

// A deterministic 2D hash, for everything on the page that should look
// accidental and be identical every frame: the foxing, the pinholes, the
// fibre. Keyed on PAGE cells, so a blemish belongs to the sheet and travels
// with it under a drag - which is the whole point of drawing the ground in
// page space rather than in window space.
uint32_t Hash2(int x, int y) {
  uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
  h ^= h >> 13;
  h *= 0x85ebca6bu;
  h ^= h >> 16;
  return h;
}

// THE GROUND, in page coordinates. `org` is where the drawing's top-left sits
// on screen and `sc` is the rung, so everything here is authored in LAYOUT
// units and lands wherever the pan has put them - the ruled grid, the pricked
// margin dots, the foxing, and the great figure behind the tree all slide
// under the drawing when it is dragged, and the page reads as a sheet being
// moved rather than as a tree sliding over a static recess.
void PaintLeaf(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 org, float sc,
               float gw, float gh) {
  // The sheet itself is the theme's (`VellumSheet`): the index column beside
  // this one is drawn on the same stock, and the two of them together are what
  // makes the book read as an open SPREAD - index on the verso, diagram on the
  // recto - rather than as a page with a list bolted to it.
  VellumSheet(dl, a, b);

  // ---- the ruling ----------------------------------------------------------
  // A scribe rules the sheet before writing on it, and the diagrams in a
  // quadrivium manuscript are laid out on that ruling. The pitch is the
  // LAYOUT's own 32 px module, so the drawing sits on the grid it was laid out
  // against instead of floating over a decorative one.
  const float mod = 32.0f * sc;
  if (mod >= 8.0f) {
    const ImU32 rule = Fade(ColIronSoft(), 0.10f);
    const ImU32 ruleHeavy = Fade(ColIronSoft(), 0.20f);
    float x0 = org.x - std::ceil((org.x - a.x) / mod) * mod;
    for (float x = x0; x <= b.x; x += mod) {
      if (x < a.x - 1) continue;
      const int col = (int)std::lround((x - org.x) / mod);
      dl->AddRectFilled(ImVec2(std::floor(x), a.y), ImVec2(std::floor(x) + 1, b.y),
                        (col % 4 == 0) ? ruleHeavy : rule);
    }
    float y0 = org.y - std::ceil((org.y - a.y) / mod) * mod;
    for (float y = y0; y <= b.y; y += mod) {
      if (y < a.y - 1) continue;
      const int row = (int)std::lround((y - org.y) / mod);
      dl->AddRectFilled(ImVec2(a.x, std::floor(y)), ImVec2(b.x, std::floor(y) + 1),
                        (row % 4 == 0) ? ruleHeavy : rule);
    }
  }

  // ---- the great figure ----------------------------------------------------
  // The circle behind the diagram: concentric rings, a ring of houses ticked
  // off round the rim, and the axis and equator crossing at the centre. It is
  // the manuscript's own furniture (the tonary wheel, the armillary, the
  // alchemist's circle) and it does one honest job besides being handsome -
  // it is a fixed landmark on the sheet, so a pan has something to move
  // against and you can tell how far you have dragged.
  const ImVec2 c(std::floor(org.x + gw * 0.5f), std::floor(org.y + gh * 0.5f));
  const float R = std::max(48.0f, std::floor(std::max(gw, gh) * 0.58f));
  const ImU32 faint = Fade(ColRubric(), 0.10f);
  const ImU32 faintInk = Fade(ColIronSoft(), 0.14f);
  PixelRing(dl, c, R, faint, 2.0f);
  PixelRing(dl, c, std::floor(R * 0.97f), Fade(ColRubric(), 0.06f), 2.0f);
  PixelRing(dl, c, std::floor(R * 0.62f), faintInk, 2.0f);
  PixelRing(dl, c, std::floor(R * 0.30f), faintInk, 2.0f);
  for (int i = 0; i < 12; i++) {
    const float ang = (float)i * 0.5235988f;
    const float cs = std::cos(ang), sn = std::sin(ang);
    dl->AddRectFilled(ImVec2(std::floor(c.x + cs * R * 0.90f),
                             std::floor(c.y + sn * R * 0.90f)),
                      ImVec2(std::floor(c.x + cs * R * 0.90f) + 2,
                             std::floor(c.y + sn * R * 0.90f) + 2),
                      Fade(ColRubric(), 0.22f));
    DottedRule(dl, ImVec2(std::floor(c.x + cs * R * 0.62f),
                          std::floor(c.y + sn * R * 0.62f)),
               ImVec2(std::floor(c.x + cs * R), std::floor(c.y + sn * R)),
               Fade(ColIronSoft(), 0.10f), 6.0f, 2.0f);
  }
  // Two intersecting lesser circles, the figure that says "these two things
  // are in proportion" in every diagram of this kind ever drawn.
  PixelArc(dl, ImVec2(c.x, c.y - R * 0.31f), R * 0.31f, 0.0f, 6.2831853f, faint, 2.0f);
  PixelArc(dl, ImVec2(c.x, c.y + R * 0.31f), R * 0.31f, 0.0f, 6.2831853f, faint, 2.0f);
  DottedRule(dl, ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y),
             Fade(ColIronSoft(), 0.16f), 8.0f, 2.0f);

  // ---- the margin ----------------------------------------------------------
  // Pricking: the holes a scribe makes down the edge of the sheet to rule
  // against, one every half module, in two columns a hand's width outside the
  // drawing. The reference page has them and they are half of why it reads as
  // a page rather than as a diagram.
  for (int side = 0; side < 2; side++) {
    const float x = side == 0 ? org.x - 26.0f * sc - 8.0f
                              : org.x + gw + 26.0f * sc + 8.0f;
    if (x < a.x - 4 || x > b.x + 4) continue;
    DottedRule(dl, ImVec2(x, org.y - 40.0f), ImVec2(x, org.y + gh + 40.0f),
               Fade(ColIronSoft(), 0.30f), 16.0f * std::max(0.5f, sc), 2.0f);
  }

  // ---- age -----------------------------------------------------------------
  // Foxing and fly-specks, one decision per 96 px page cell. Sparse on purpose:
  // the sheet should look old, not dirty, and anything denser than this reads
  // as noise over the diagram rather than as the sheet under it.
  const float cell = 96.0f;
  const int cx0 = (int)std::floor((a.x - org.x) / cell) - 1;
  const int cx1 = (int)std::floor((b.x - org.x) / cell) + 1;
  const int cy0 = (int)std::floor((a.y - org.y) / cell) - 1;
  const int cy1 = (int)std::floor((b.y - org.y) / cell) + 1;
  for (int gy = cy0; gy <= cy1; gy++) {
    for (int gx = cx0; gx <= cx1; gx++) {
      const uint32_t h = Hash2(gx, gy);
      if ((h & 3u) != 0u) continue;   // a quarter of the cells carry a mark
      const float px = org.x + (float)gx * cell + (float)((h >> 8) & 63u);
      const float py = org.y + (float)gy * cell + (float)((h >> 14) & 63u);
      if (px < a.x || px > b.x - 4 || py < a.y || py > b.y - 4) continue;
      const int n = 1 + (int)((h >> 20) & 3u);
      const ImU32 col = (h & 4u) ? Fade(ColIronSoft(), 0.13f)
                                 : Fade(ColRubric(), 0.10f);
      for (int k = 0; k < n; k++) {
        const uint32_t hk = Hash2(gx * 31 + k, gy * 17 - k);
        const float ox = (float)((hk >> 3) & 7u) * 2.0f;
        const float oy = (float)((hk >> 9) & 7u) * 2.0f;
        dl->AddRectFilled(ImVec2(std::floor(px + ox), std::floor(py + oy)),
                          ImVec2(std::floor(px + ox) + 2, std::floor(py + oy) + 2),
                          col);
      }
    }
  }
}

// ---- a figure on the page ---------------------------------------------------
//
// A WORD IS A ROUNDEL. Every cell on the canvas used to be the inventory's
// slot: a recessed obsidian square with a bevel and a pool of the glyph's
// colour behind the engraving. That is the right picture for a thing you own
// and a wrong one for a thing you have WRITTEN - a page of squares in rows is
// an inventory, and a page of ruled circles joined by strokes is a diagram,
// which is what a spell is. The shape carries the sort as well as the colour
// does: a roundel for a word, a tablet for an operator, a double ring for a
// delivery, a lozenge for a bead on the trunk.
//
// `mul` is the canvas's rung, passed so the engraving inside shrinks with the
// figure instead of overflowing it.
void LeafRoundel(ImDrawList* dl, ImVec2 a, ImVec2 b, uint32_t color, int sort,
                 const char* icon, float mul, bool dim) {
  const ImVec2 mid(std::floor((a.x + b.x) * 0.5f), std::floor((a.y + b.y) * 0.5f));
  const float r = std::floor(std::min(b.x - a.x, b.y - a.y) * 0.5f) - 1.0f;
  const ImU32 pig = SortPigment(sort);
  // The wash: the glyph's OWN colour, thinned the way a pigment is thinned
  // with water, so `fire` and `water` are told apart before the engraving is
  // read. Kept faint - this is a tint on a page, not a filled swatch.
  const ImU32 sw =
      color ? IM_COL32(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, 255)
            : ColVellumHi();
  // AT THE OVERVIEW RUNG THE FIGURE IS A DIFFERENT FIGURE (2026-09-22). A cell
  // is 16 px there; a wash, two rings and a 16 px engraving inside it are four
  // pictures fighting over the same nine pixels, and the first version of the
  // overview fit came out as a column of red smudges. So below half rung a
  // word is ONE filled disc in its sort's pigment with a rim of ink - which is
  // what you actually read at that size: the SHAPE of the spell and the colour
  // of the things in it. The engraving comes back the moment the cell is big
  // enough to hold it.
  if (mul < 0.5f) {
    PixelDisc(dl, mid, r, Mix(sw, pig, 0.55f));
    PixelRing(dl, mid, r, Fade(ColIronGall(), dim ? 0.4f : 0.85f), 2.0f);
    return;
  }
  if (color) {
    PixelDisc(dl, mid, r - 2.0f, Fade(sw, dim ? 0.14f : 0.30f));
    PixelDisc(dl, mid, r - 6.0f, Fade(sw, dim ? 0.10f : 0.22f));
  } else {
    PixelDisc(dl, mid, r - 2.0f, Fade(ColVellumHi(), 0.5f));
  }
  // Two rings: the sort's pigment outside, a hairline of ink inside it. Drawn
  // with the compass, which is why they are stepped and not anti-aliased.
  PixelRing(dl, mid, r, Fade(pig, dim ? 0.45f : 0.95f), mul < 1.0f ? 2.0f : 3.0f);
  PixelRing(dl, mid, r - 4.0f, Fade(ColIronGall(), dim ? 0.25f : 0.45f), 2.0f);
  // The engraving, with a vellum emboss under it so it reads as cut INTO the
  // page rather than as a dark blob on it.
  DrawSpriteCenteredAt(dl, icon, ImVec2(mid.x + 1, mid.y + 1), mul,
                       Fade(ColVellumHi(), 0.75f));
  DrawSpriteCenteredAt(dl, icon, mid, mul,
                       dim ? Fade(ColIronSoft(), 0.55f) : ColIronGall());
}

// AN OPERATOR IS A TABLET, not a roundel: it is the one sort that BINDS two
// other things, so it keeps its corners and gets the bracket rule down each
// side - the same mark the grammar writes it with.
void LeafTablet(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* icon, float mul,
                bool dim) {
  if (mul < 0.5f) {
    // The overview mark, for the same reason `LeafRoundel` has one: at 16 px
    // a bordered tablet with a double rule, four serifs and an engraving in it
    // is nine pixels of mud. A filled square with a light core is a SQUARE at
    // any size, which is the only thing this rung has to say about it.
    dl->AddRectFilled(a, b, Fade(ColIronGall(), dim ? 0.45f : 0.9f));
    dl->AddRectFilled(ImVec2(a.x + 3, a.y + 3), ImVec2(b.x - 3, b.y - 3),
                      Fade(ColVellumHi(), 0.85f));
    return;
  }
  dl->AddRectFilled(a, b, Fade(ColVellumHi(), 0.55f));
  const ImU32 ink = Fade(ColIronGall(), dim ? 0.45f : 0.95f);
  dl->AddRect(a, b, ink, 0.0f, 0, 2.0f);
  dl->AddRect(ImVec2(a.x + 4, a.y + 4), ImVec2(b.x - 4, b.y - 4),
              Fade(ColIronGall(), dim ? 0.18f : 0.35f), 0.0f, 0, 2.0f);
  // The brackets: a serif at each corner of the inner rule.
  const float t = 6.0f;
  for (int i = 0; i < 4; i++) {
    const float x = (i & 1) ? b.x - 4 - t : a.x + 4;
    const float y = (i & 2) ? b.y - 6 : a.y + 4;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + t, y + 2), ink);
  }
  DrawSpriteCenteredAt(dl, icon,
                       ImVec2(std::floor((a.x + b.x) * 0.5f) + 1,
                              std::floor((a.y + b.y) * 0.5f) + 1),
                       mul, Fade(ColVellumHi(), 0.75f));
  DrawSpriteCenteredAt(dl, icon,
                       ImVec2(std::floor((a.x + b.x) * 0.5f),
                              std::floor((a.y + b.y) * 0.5f)),
                       mul, dim ? Fade(ColIronSoft(), 0.55f) : ColIronGall());
}

// A rectangle in DOTS. Used for the one shape on the page that is a space
// rather than a thing; a dashed rule is how a scribe rules a blank he means to
// come back and fill.
void DashRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float t, float dash) {
  for (float x = a.x; x < b.x; x += dash * 2.0f) {
    const float x1 = std::min(x + dash, b.x);
    dl->AddRectFilled(ImVec2(x, a.y), ImVec2(x1, a.y + t), col);
    dl->AddRectFilled(ImVec2(x, b.y - t), ImVec2(x1, b.y), col);
  }
  for (float y = a.y; y < b.y; y += dash * 2.0f) {
    const float y1 = std::min(y + dash, b.y);
    dl->AddRectFilled(ImVec2(a.x, y), ImVec2(a.x + t, y1), col);
    dl->AddRectFilled(ImVec2(b.x - t, y), ImVec2(b.x, y1), col);
  }
}

// AN OPERATOR'S EMPTY SLOT, STANDING IN THE OPERAND ROW (2026-09-22). It used
// to be a 21 px hollow ring hung off the LEFT EDGE of the operator's cell -
// eleven screen pixels at the fit rung, the smallest mark in the drawing,
// carrying the single most important fact about a `_ trail`: that it is not
// finished and takes the word spoken before it. An operand is the row BELOW its
// operator, so the missing operand is drawn there, at a cell's size, and the
// mark is the one the grammar itself writes: a tablet ruled in dots with the
// underscore across it.
void LeafBlank(ImDrawList* dl, ImVec2 a, ImVec2 b, float mul) {
  const ImU32 ink = Fade(ColRubric(), 0.85f);
  const float uy = std::floor((a.y + b.y) * 0.5f + (b.y - a.y) * 0.16f);
  const float ut = std::max(2.0f, std::floor((b.y - a.y) * 0.06f));
  if (mul < 0.5f) {
    dl->AddRect(a, b, ink, 0.0f, 0, 2.0f);
    dl->AddRectFilled(ImVec2(a.x + 4, uy), ImVec2(b.x - 4, uy + ut), ink);
    return;
  }
  // The hollow is PALER than the vellum around it, not darker: a hole in the
  // page is an absence of ink, and a grey plate would read as a word drawn in
  // a colour nobody else uses.
  dl->AddRectFilled(a, b, Fade(ColVellumHi(), 0.35f));
  DashRect(dl, a, b, ink, 2.0f, std::max(4.0f, std::floor((b.x - a.x) * 0.09f)));
  dl->AddRectFilled(ImVec2(a.x + 12, uy), ImVec2(b.x - 12, uy + ut), ink);
}

// The hover mark on a page is not a glow - light does not come out of ink.
// It is what a reader does: a bracket round the passage, pricked in minium.
void LeafHover(ImDrawList* dl, ImVec2 a, ImVec2 b) {
  const float o = 5.0f, t = 10.0f;
  const ImU32 c = Fade(ColRubric(), 0.9f);
  for (int i = 0; i < 4; i++) {
    const float x = (i & 1) ? b.x + o - t : a.x - o;
    const float y = (i & 2) ? b.y + o - 2 : a.y - o;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + t, y + 2), c);
    const float vy = (i & 2) ? b.y + o - t : a.y - o;
    const float vx = (i & 1) ? b.x + o - 2 : a.x - o;
    dl->AddRectFilled(ImVec2(vx, vy), ImVec2(vx + 2, vy + t), c);
  }
}

// The book's own window onto the sheet: a shadow round the opening, so the
// page reads as lying UNDER the binding rather than as a bright rectangle
// pasted on it. Drawn last, in window coordinates - it belongs to the book.
void LeafFrame(ImDrawList* dl, ImVec2 a, ImVec2 b) { VellumFrame(dl, a, b); }

const UIState::GlyphUI* FindGlyph(const UIState& s, const std::string& id) {
  for (const UIState::GlyphUI& g : s.glyphsOwned)
    if (g.id == id) return &g;
  return nullptr;
}

const UIState::GrimoirePageUI* FindPage(const UIState& s, const std::string& name) {
  for (const UIState::GrimoirePageUI& p : s.grimoirePages)
    if (p.name == name) return &p;
  return nullptr;
}

// What sort a payload denotes: a glyph's own sort, or - for a PAGE, which is
// one glyph on the tree (spell.h, "A PAGE USED AS ONE GLYPH") - the sort it
// stands as: a delivery for a carrier page, else what its body holds. -1 for a
// page that is no usable page.
int PayloadSort(const UIState& s, bool page, const char* name) {
  if (page) {
    const UIState::GrimoirePageUI* pg = FindPage(s, name ? name : "");
    return pg ? pg->shapeSort : -1;
  }
  const UIState::GlyphUI* g = FindGlyph(s, name ? name : "");
  return g ? g->type : -1;
}
// A payload that BOXES what it is dropped round: a delivery word, or a page
// that is one (a carrier).
bool DeliveryPayload(const UIState& s, const ImGuiPayload* p) {
  if (!p) return false;
  const bool page = p->IsDataType(kPayloadPage);
  if (!page && !p->IsDataType(kPayloadGlyph)) return false;
  return PayloadSort(s, page, (const char*)p->Data) == kDelivery;
}

// A PAGE'S SEAL: the cell that stands for a whole written spell wears the
// page's name on a slip under it and a folded corner on it, so it can never
// be mistaken for the one word its engraving shows. Not at the overview rung,
// where the slip would be a smudge.
void PageSeal(ImDrawList* dl, ImVec2 a, ImVec2 b, const std::string& name, float scale) {
  // The folded corner: a stepped triangle in the top right, in gold leaf.
  const float f = std::max(6.0f, std::floor((b.x - a.x) * 0.22f));
  for (float k = 0; k < f; k += 2.0f)
    dl->AddRectFilled(ImVec2(b.x - f + k, a.y), ImVec2(b.x, a.y + k + 2.0f),
                      Fade(ColGoldHi(), 0.85f));
  dl->AddRect(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), Fade(ColGoldHi(), 0.8f), 0.0f,
              0, 2.0f);
  if (scale < 0.5f || name.empty()) return;
  std::string t = name;
  if (t.size() > 16) t = t.substr(0, 15) + "~";
  const ImVec2 ts = FontSmall()->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, t.c_str());
  const float cx = std::floor((a.x + b.x) * 0.5f);
  const ImVec2 p(std::floor(cx - ts.x * 0.5f), std::floor(b.y - 2.0f));
  dl->AddRectFilled(ImVec2(p.x - 3, p.y - 1), ImVec2(p.x + ts.x + 3, p.y + ts.y),
                    Fade(ColVellumHi(), 0.97f));
  dl->AddRect(ImVec2(p.x - 3, p.y - 1), ImVec2(p.x + ts.x + 3, p.y + ts.y),
              Fade(ColGoldHi(), 0.95f));
  dl->AddText(FontSmall(), 13.0f, p, ColIronGall(), t.c_str());
}

// A 2 px stepped ring, the panel's vocabulary for "this is where it goes" and
// for "not here". Never a rounded rect: zero rounding, everywhere.
void Ring(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, int steps) {
  for (int i = 0; i < steps; i++) {
    const float o = (float)(i * 2);
    dl->AddRect(ImVec2(a.x - o, a.y - o), ImVec2(b.x + o, b.y + o),
                Fade(col, 1.0f - (float)i * 0.28f), 0.0f, 0, 2.0f);
  }
}

// A 2 px stepped diagonal bar across a cell: an incomplete operator is CHARGED
// and does nothing, and a slash through it says so without a word. Stepped
// rather than a true diagonal, because an anti-aliased line beside 2x chrome is
// the one thing that says "different program".
void SlashOut(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
  const float w = b.x - a.x, h = b.y - a.y;
  const int steps = (int)std::max(4.0f, std::floor(w / 4.0f));
  for (int i = 0; i < steps; i++) {
    const float x = std::floor(a.x + w * (float)i / (float)steps);
    const float y = std::floor(b.y - h * (float)i / (float)steps);
    dl->AddRectFilled(ImVec2(x, y - 4), ImVec2(x + 4, y), col);
  }
}

// The multiplicity pip row: `n` as that many 4 px ticks along a node's top
// edge, up to five, then a solid bar. "How many of this" without a numeral.
void CountPips(ImDrawList* dl, ImVec2 a, ImVec2 b, int n, ImU32 col) {
  if (n <= 1) return;
  const float mid = std::floor((a.x + b.x) * 0.5f);
  // TALLY MARKS, which is how a page counts: upright strokes over the figure,
  // and past five the gate that a tally always draws - four and a stroke
  // across them.
  if (n <= 5) {
    const float span = (float)n * 6.0f - 3.0f;
    for (int i = 0; i < n; i++) {
      const float x = std::floor(mid - span * 0.5f + (float)i * 6.0f);
      dl->AddRectFilled(ImVec2(x, a.y - 9), ImVec2(x + 2, a.y - 1), col);
    }
    return;
  }
  for (int i = 0; i < 4; i++) {
    const float x = std::floor(mid - 12 + (float)i * 6.0f);
    dl->AddRectFilled(ImVec2(x, a.y - 9), ImVec2(x + 2, a.y - 1), col);
  }
  dl->AddRectFilled(ImVec2(mid - 15, a.y - 6), ImVec2(mid + 9, a.y - 4), col);
  if (n > 9) dl->AddRectFilled(ImVec2(mid + 12, a.y - 9), ImVec2(mid + 18, a.y - 1), col);
}

}  // namespace

// ---- the shared bits the composer and the canvas both draw ------------------

ImU32 SortColour(int sort) {
  switch (sort) {
    case kMatter: return IM_COL32(120, 190, 120, 255);   // matter: green
    case kEffect: return IM_COL32(232, 138, 46, 255);    // effect: ember
    case kDelivery: return IM_COL32(76, 132, 200, 255);  // delivery: mana blue
    case kMod: return IM_COL32(200, 184, 138, 255);      // mod: parchment
    default: return IM_COL32(217, 190, 110, 255);        // operator, separator: gold
  }
}

const char* SortLabel(int sort) {
  return (sort >= 0 && sort < 6) ? kSortLabels[sort] : "?";
}

void GlyphArt(ImDrawList* dl, ImVec2 at, float size, uint32_t color, int type) {
  const ImVec2 mid(at.x + size * 0.5f, at.y + size * 0.5f);
  if (color) {
    const ImU32 sw =
        IM_COL32((color) & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, 255);
    // Three nested rects at rising alpha: a stepped radial glow, the same one
    // the composer's 44 px cell draws, expressed as fractions of the cell so a
    // 64 px chrome cell gets the same picture rather than a thinner rim.
    const float k = size / 44.0f;
    dl->AddRectFilled(ImVec2(at.x + 4 * k, at.y + 4 * k),
                      ImVec2(at.x + size - 4 * k, at.y + size - 4 * k), Fade(sw, 0.16f));
    dl->AddRectFilled(ImVec2(at.x + 8 * k, at.y + 8 * k),
                      ImVec2(at.x + size - 8 * k, at.y + size - 8 * k), Fade(sw, 0.22f));
    dl->AddRectFilled(ImVec2(at.x + 12 * k, at.y + 12 * k),
                      ImVec2(at.x + size - 12 * k, at.y + size - 12 * k), Fade(sw, 0.30f));
    dl->AddRectFilled(ImVec2(at.x + 6 * k, at.y + size - 6 * k),
                      ImVec2(at.x + size - 6 * k, at.y + size - 4 * k), Fade(sw, 0.9f));
  }
  DrawSpriteCentered(dl, GlyphIcon(type), ImVec2(mid.x + 1, mid.y + 1),
                     Fade(ColInk(), 0.7f));
  DrawSpriteCentered(dl, GlyphIcon(type), mid);
}

void GlyphRoundel(ImDrawList* dl, ImVec2 a, ImVec2 b, uint32_t color, int sort,
                  float mul, bool dim) {
  LeafRoundel(dl, a, b, color, sort, GlyphIcon(sort), mul, dim);
}

void PageHover(ImDrawList* dl, ImVec2 a, ImVec2 b) { LeafHover(dl, a, b); }

void GlyphInfoBox(const UIState::GlyphUI& g, const char* sortName, const char* magnitude) {
  BeginTip();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ColGoldHi()));
  if (g.owned) {
    std::string up = g.id;
    for (char& c : up) c = (char)toupper((unsigned char)c);
    ImGui::TextUnformatted(up.c_str());
  } else {
    ImGui::TextUnformatted("? ? ?");
  }
  ImGui::PopStyleColor();
  if (magnitude && *magnitude) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ColGoldHi()));
    ImGui::TextUnformatted(magnitude);
    ImGui::PopStyleColor();
  }
  ImGui::TextDisabled("%s . %s", sortName, g.valence.c_str());
  if (!g.owned) {
    ImGui::TextDisabled("a word you have not learned");
    EndTip();
    return;
  }
  if (!g.desc.empty()) ImGui::TextWrapped("\"%s\"", g.desc.c_str());
  if (!g.emptyNote.empty()) ImGui::TextDisabled("%s", g.emptyNote.c_str());
  ImGui::TextDisabled("word %d%s%s", g.mana, g.tariff.empty() ? "" : " . tariff: ",
                      g.tariff.c_str());
  if (!g.axis.empty()) ImGui::TextDisabled("again: %s", g.axis.c_str());
  if (!g.delivers.empty()) ImGui::TextDisabled("delivers by: %s", g.delivers.c_str());
  if (!g.example.empty()) ImGui::TextDisabled("example: %s", g.example.c_str());
  ImGui::TextDisabled("drag onto a key to bind it, or into a page to write with it");
  ImGui::TextDisabled("right-click to write it onto the end of the open page");
  EndTip();
}

constexpr int kUndoDepth = 32;
void PushGrimoireUndo(UIState& s) {
  s.grimoireUndo.push_back(s.grimoireEditWords);
  if ((int)s.grimoireUndo.size() > kUndoDepth)
    s.grimoireUndo.erase(s.grimoireUndo.begin());
  // A new edit is a new future: whatever ctrl+Z had set aside is gone.
  s.grimoireRedo.clear();
}

// ---- magnitude (docs/PLAN_spell_magnitude.md §2.5) ----------------------------
//
// THE WHEEL OVER A GRADED CELL SETS ITS MAGNITUDE; over anything else it zooms.
// The zoom is decided before the cells are laid out this frame, so it reads
// whether the cursor was over a graded cell LAST frame - one frame of lag on
// a hover boundary, which a wheel notch cannot feel.
static bool gOverGradedCell = false;

std::string MagDecimal(int mag) {
  const bool neg = mag < 0;
  if (neg) mag = -mag;
  std::string s = (neg ? "-" : "") + std::to_string(mag / 1000);
  int frac = mag % 1000;
  if (frac) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%03d", frac);
    std::string f = buf;
    while (!f.empty() && f.back() == '0') f.pop_back();
    s += "." + f;
  }
  return s;
}

// The tip's magnitude line: "x0.5 . gravity -0.50 g   (wheel 0.25..4)".
std::string MagnitudeLine(const UIState::SpellGraphUI::Node& n) {
  if (!n.graded) return std::string();
  std::string s = (n.magMin < 0 ? std::string() : std::string("x")) + MagDecimal(n.mag);
  if (!n.magLabel.empty()) s += " . " + n.magLabel;
  s += "   (wheel: " + MagDecimal(n.magMin) + " to " + MagDecimal(n.magMax) + ")";
  return s;
}

// ---- timing (docs/PLAN_spell_magnitude.md §4, M3) -------------------------------
//
// WHEN AN ITEM FIRES is a click away: a plain left click on a cell (not a drag)
// opens a small menu of the five moments and a row of delays, and a choice is
// one `SetTiming` edit. A cell that does not fire on hit wears a tag at its top
// edge - where its stroke leaves for the carrier - naming the moment.
static int gTimingTree = -1;          // the tree node the open menu edits
static int gTimingCur[3] = {0, 0, 0};  // its trigger / every / delay when opened
static bool gTimingOpen = false;       // a click asked for the menu this frame
// THE PAGE THE INDEX MEANS SOMETHING IN. `gTimingTree` is a node index into
// the tree of these words, and an index outlives nothing: an undo, a reload or
// a page switch while the menu is open rebuilds the tree, and the same number
// then names some other word. The menu closes the frame its words change.
static std::vector<std::string> gTimingWords;
static std::string gTimingPage;

const char* TriggerTag(int trig) {
  switch (trig) {
    case 1: return "BOUNCE";
    case 2: return "EXPIRE";
    case 3: return "LAUNCH";
    case 4: return "EVERY";
    default: return "";
  }
}

void TimingTag(ImDrawList* dl, ImVec2 a, ImVec2 b, const UIState::SpellGraphUI::Node& n,
               float scale) {
  if ((n.trigger == 0 && n.delay == 0) || scale < 0.5f) return;
  std::string t = TriggerTag(n.trigger);
  if (n.trigger == 4) t += " " + std::to_string(n.every);
  if (n.delay > 0) t += (t.empty() ? "" : " ") + std::string("+") + std::to_string(n.delay) + "t";
  const ImVec2 ts = FontSmall()->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, t.c_str());
  const float cx = std::floor((a.x + b.x) * 0.5f);
  const ImVec2 p(std::floor(cx - ts.x * 0.5f), std::floor(a.y - ts.y + 2.0f));
  dl->AddRectFilled(ImVec2(p.x - 3, p.y - 1), ImVec2(p.x + ts.x + 3, p.y + ts.y),
                    Fade(ColVellumHi(), 0.95f));
  dl->AddRect(ImVec2(p.x - 3, p.y - 1), ImVec2(p.x + ts.x + 3, p.y + ts.y),
              Fade(ColAzurite(), 0.9f));
  dl->AddText(FontSmall(), 13.0f, p, ColAzurite(), t.c_str());
}

// A CLICK, not a drag: the button came up over the cell having moved less than
// the drag threshold since it went down.
void TimingClick(const UIState::SpellGraphUI::Node& n, bool hov, bool readOnly, bool live) {
  if (readOnly || live || !hov || n.treeNode < 0) return;
  if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
  const float th = ImGui::GetIO().MouseDragThreshold;
  if (ImGui::GetIO().MouseDragMaxDistanceSqr[0] > th * th) return;
  gTimingTree = n.treeNode;
  gTimingCur[0] = n.trigger;
  gTimingCur[1] = n.every;
  gTimingCur[2] = n.delay;
  // OPENED LATER, by TimingPopup: the cells sit under a per-node PushID and a
  // popup id is relative to the id stack, so one opened here would never be
  // found by the BeginPopup outside the loop.
  gTimingOpen = true;
}

void TimingPopup(UIState& s) {
  if (gTimingOpen) {
    gTimingOpen = false;
    gTimingWords = s.grimoireEditWords;
    gTimingPage = s.grimoireSelected;
    ImGui::OpenPopup("##timing");
  }
  if (!ImGui::BeginPopup("##timing")) return;
  if (s.grimoireEditWords != gTimingWords || s.grimoireSelected != gTimingPage) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    gTimingTree = -1;
    return;
  }
  auto apply = [&](int trig, int every, int delay) {
    PushGrimoireUndo(s);
    s.graphEdit = {};
    s.graphEdit.pending = true;
    s.graphEdit.op = UIState::GraphEditIntent::SetTiming;
    s.graphEdit.treeNode = gTimingTree;
    s.graphEdit.trigger = trig;
    s.graphEdit.every = every;
    s.graphEdit.delay = delay;
    ImGui::CloseCurrentPopup();
  };
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ColGoldHi()));
  ImGui::TextUnformatted("WHEN DOES IT FIRE?");
  ImGui::PopStyleColor();
  static const char* kNames[5] = {"when it hits", "at each bounce", "when its life runs out",
                                  "at launch", "every"};
  for (int t = 0; t < 5; t++) {
    if (t == 4) {
      ImGui::TextUnformatted("every");
      for (int period : {5, 10, 20, 40}) {
        ImGui::SameLine();
        char lbl[24];
        std::snprintf(lbl, sizeof lbl, "%d%s##ev%d", period, period == 40 ? " ticks" : "",
                      period);
        const bool on = gTimingCur[0] == 4 && gTimingCur[1] == period;
        if (ImGui::Selectable(lbl, on, 0, ImVec2(on ? 0 : 0, 0))) apply(4, period, gTimingCur[2]);
      }
      continue;
    }
    if (ImGui::Selectable(kNames[t], gTimingCur[0] == t)) apply(t, 0, gTimingCur[2]);
  }
  ImGui::Separator();
  ImGui::TextUnformatted("then wait");
  for (int d : {0, 5, 10, 20, 40, 80}) {
    ImGui::SameLine();
    char lbl[24];
    std::snprintf(lbl, sizeof lbl, d == 0 ? "no##d%d" : "%d##d%d", d, d);
    if (d == 0) std::snprintf(lbl, sizeof lbl, "none##d0");
    if (ImGui::Selectable(lbl, gTimingCur[2] == d, 0, ImVec2(0, 0)))
      apply(gTimingCur[0], gTimingCur[1], d);
  }
  ImGui::TextDisabled("ticks (30 a second). Only a flying carrier");
  ImGui::TextDisabled("bounces, expires or launches; a delay works anywhere.");
  ImGui::EndPopup();
}

// Called for a hovered cell: consume the wheel as a magnitude step (ctrl = four
// steps) and latch the edit. Past the end of the range the op REFUSES and the
// status line names the limit, which is the feedback a silent clamp would eat.
void WheelMagnitude(UIState& s, const UIState::SpellGraphUI::Node& n, bool readOnly) {
  if (readOnly || !n.graded || n.treeNode < 0) return;
  gOverGradedCell = true;
  const float wheel = ImGui::GetIO().MouseWheel;
  if (wheel == 0.0f || ImGui::GetDragDropPayload()) return;
  const int steps = (wheel > 0.0f ? 1 : -1) * (ImGui::GetIO().KeyCtrl ? 4 : 1);
  PushGrimoireUndo(s);
  s.graphEdit = {};
  s.graphEdit.pending = true;
  s.graphEdit.op = UIState::GraphEditIntent::SetMagnitude;
  s.graphEdit.treeNode = n.treeNode;
  s.graphEdit.mag = n.mag + steps * std::max(1, n.magStep);
}

// The magnitude written on the cell: a small numeral at its foot, only when it
// is not the word's default, so a page of plain words looks exactly as it did.
// A SIGNED component (M2: `lift`, `wind`) writes its sign rather than an `x`,
// because -1 of a lift is a direction, not a multiplier.
void MagnitudeNumeral(ImDrawList* dl, ImVec2 a, ImVec2 b, const UIState::SpellGraphUI::Node& n,
                      float scale) {
  if (!n.graded || n.mag == n.magDefault || scale < 0.5f) return;
  const std::string t =
      n.magMin < 0 ? (n.mag > 0 ? "+" : "") + MagDecimal(n.mag) : "x" + MagDecimal(n.mag);
  const ImVec2 ts = FontSmall()->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, t.c_str());
  const ImVec2 p(std::floor(b.x - ts.x - 2.0f), std::floor(b.y - ts.y + 1.0f));
  dl->AddRectFilled(ImVec2(p.x - 2, p.y), ImVec2(b.x, b.y + 1), Fade(ColVellumHi(), 0.9f));
  dl->AddText(FontSmall(), 13.0f, p, ColRubric(), t.c_str());
}

// ---- the canvas ---------------------------------------------------------------

GraphCanvasResult SpellGraphCanvas(UIState& s, ImVec2 at, ImVec2 size, bool readOnly) {
  GraphCanvasResult res;
  const UIState::SpellGraphUI& g = s.spellGraph;

  ImGui::SetCursorScreenPos(at);
  // NO SCROLLBARS, because the view is DRIVEN now (2026-09-22): a drag on the
  // background pans it and the wheel zooms it, so a pair of bars taking 14 px
  // off a band that is half the composer's width would only be a second, worse
  // way to do the same thing. `NoScrollWithMouse` is what hands the wheel to
  // the zoom instead of to a scroll nobody asked for.
  ImGui::BeginChild("##spellcanvas", size, ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground |
                        ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 viewMin = ImGui::GetWindowPos();
  const ImVec2 viewMax(viewMin.x + size.x, viewMin.y + size.y);

  // THE SHEET IS DRAWN LATER, not here (2026-09-22). It used to be a recess -
  // a dark rectangle in window coordinates, painted before anything else, with
  // the tree sliding over it. That is precisely backwards for a page: what a
  // drag moves is the SHEET, so the ground has to be drawn in the drawing's
  // own coordinates, which are not known until the rung and the pan have been
  // resolved a few dozen lines below. `PaintLeaf` is called there, once, for
  // both the empty page and the full one.
  dl->PushClipRect(viewMin, viewMax, true);

  // ---- THE VIEW'S OWN INPUT, submitted FIRST ---------------------------------
  //
  // A button the size of the whole recess, so a drag that starts on empty ink
  // pans the drawing. It is submitted BEFORE any node because ImGui resolves
  // an overlap in favour of the item submitted LAST: every cell, pip, bar and
  // socket below takes the cursor off this one, which is exactly the rule you
  // want — a drag ON a node still moves that node's word, and a drag on the
  // dark between them moves the view.
  //
  // Middle mouse pans from anywhere, node or not, for the same reason every
  // other canvas in the world does it.
  ImGui::SetCursorScreenPos(viewMin);
  ImGui::InvisibleButton("##panview", size,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonMiddle);
  const bool panHeld =
      ImGui::IsItemActive() ||
      (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
       ImGui::IsMouseDown(ImGuiMouseButton_Middle));
  if (panHeld && !ImGui::GetDragDropPayload()) {
    const ImVec2 d = ImGui::GetIO().MouseDelta;
    s.spellGraphPanX += d.x;
    s.spellGraphPanY += d.y;
  }
  if (panHeld) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
  // BACK TO THE FIT, on a double-click of the background. The one gesture that
  // undoes every other one, so a view driven somewhere useless is never a trap.
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    s.spellGraphZoom = 0.0f;
    s.spellGraphPanX = s.spellGraphPanY = 0.0f;
  }

  if (g.nodes.empty()) {
    // AN EMPTY PAGE IS STILL A PAGE. The sheet, its ruling and its great
    // figure are drawn exactly as they are for a spell - the pan still moves
    // them, so the one thing you can do to an empty canvas still answers - and
    // the only thing missing is the drawing. Where it would go is said by a
    // roundel of pricked dots the size of a cell, which is the same mark the
    // drop targets use for "here".
    const ImVec2 c(std::floor((viewMin.x + viewMax.x) * 0.5f + s.spellGraphPanX),
                   std::floor((viewMin.y + viewMax.y) * 0.5f + s.spellGraphPanY));
    PaintLeaf(dl, viewMin, viewMax, ImVec2(c.x - 32, c.y - 32), 1.0f, 64.0f, 64.0f);
    const char* hint = "an empty page - drag a word here";
    ImGui::PushFont(FontSmall());
    const ImVec2 ts = ImGui::CalcTextSize(hint);
    ImGui::PopFont();
    for (int i = 0; i < 36; i++) {
      const float ang = (float)i * 0.1745329f;
      PixelArc(dl, ImVec2(c.x, c.y - 12), 30.0f, ang, ang + 0.10f,
               Fade(ColIronSoft(), 0.55f), 2.0f);
    }
    DrawSpriteCentered(dl, "glyph_ring", ImVec2(c.x, c.y - 12),
                       Fade(ColIronGall(), 0.55f));
    dl->AddText(FontSmall(), 13.0f,
                ImVec2(std::floor(c.x - ts.x * 0.5f), c.y + 26),
                Fade(ColIronSoft(), 0.95f), hint);
    LeafFrame(dl, viewMin, viewMax);
    dl->PopClipRect();
    ImGui::EndChild();
    return res;
  }

  // INTEGER SCALE, 1x OR 0.5x. Nothing between, and never a fractional one:
  // 0.5 halves a 2x sprite back to its authored size, which is still exact.
  //
  // THE LAYER PITCH IS THE UI'S TO CHOOSE, and it is not the layout's 96 — nor
  // is it one number. `BuildGraph` spaces every layer at `kGraphPitch` because
  // a uniform grid is the easy thing to lay out; on screen a layer only has to
  // clear THE TALLEST THING IN IT, and the layers are not the same height at
  // all. A cell is 64, a join's bar is 64, a socket row is 32 and a BUS is 16 —
  // and a uniform pitch spends a 64 px band on a 16 px stroke, twice, in every
  // spell that shares anything. Measured on `duststorm-mine`: six layers at a
  // uniform 42 px (half scale) is 252 px and did not fit; the same six at their
  // own heights is 198 and does.
  //
  // It is a spacing decision, not a rounding one: every height and every gap
  // below is a whole even number of screen pixels, and no node's SIZE is
  // touched. The bus ends up hugging the bar it feeds, which is also what it
  // means.
  std::vector<float> layerH((size_t)std::max(1, g.layers), 0.0f);
  // WHAT KIND OF ROW THIS LAYER IS, which is not deducible from its height:
  // a socket row and a mod bead row are both 32 layout px and want completely
  // different bands, and squashing them the same way is why a `shotgun` bead
  // came out an 8 px sliver sitting on the aim pip's ring. Ordered by weight:
  // a layer is as tall as the heaviest thing standing in it.
  enum Band { kBRule = 0, kBSplit, kBSocket, kBBead, kBCell };
  std::vector<int> layerB((size_t)std::max(1, g.layers), kBRule);
  // WHICH LAYERS HOLD A JUNCTION, which is no longer answerable from the band:
  // a junction that wears its split word is a 64 px CELL and lands in kBCell
  // with every other cell on the page (2026-09-22). The fan's gap rule below
  // is about the junction, not about its height.
  std::vector<char> layerSplit((size_t)std::max(1, g.layers), 0);
  for (const UIState::SpellGraphUI::Node& n : g.nodes) {
    if (n.layer < 0 || n.layer >= g.layers) continue;
    if (n.kind == kSplit) layerSplit[(size_t)n.layer] = 1;
    layerH[(size_t)n.layer] = std::max(layerH[(size_t)n.layer], (float)n.h);
    const int band = n.h >= 64.0f      ? kBCell
                     : n.kind == kModTag ? kBBead
                     : n.kind == kSocket ? kBSocket
                     : n.kind == kSplit  ? kBSplit
                                         : kBRule;
    layerB[(size_t)n.layer] = std::max(layerB[(size_t)n.layer], band);
  }
  // FURNITURE IS SQUASHED, AND IT HUGS WHAT IT BELONGS TO (2026-09-22). A cell
  // is 64 layout px and gets its full share; a socket row (32) is a row of
  // pips, a bus (16) is one rule, and a mod bead (32) is a lozenge - none of
  // the three needs its layout height on screen, and none of them needs a
  // cell's worth of air around it either, because a pip that is not touching
  // the bar it belongs to is a pip you have to trace a line to.
  //
  // This is not decoration, it is the difference between fitting and not.
  // Measured on `fire projectile` - the SMALLEST spell the page can draw, two
  // words - the old uniform bands and gaps came to 216 px in a band of 192:
  // the two-word case was clipped at both ends on a 1600x900 screen, and every
  // picture in the gallery was cut. The same tree in squashed bands is 168.
  auto bandFor = [](int band, float h, float sc) {
    switch (band) {
      case kBCell: return std::floor(h * sc);                 // a cell: its size
      // A BEAD IS A LOZENGE, AND A LOZENGE NEEDS A SHORT AXIS (2026-09-22). At
      // 0.55 a 32 px bead came out 8 px tall against 48 px wide, which draws as
      // a hairline with a 16 px engraving stamped over it - the `shotgun` on
      // every fanned spell on the page. 0.75 with a 14 px floor keeps it a
      // bead at every rung and costs six pixels.
      case kBBead: return std::max(14.0f, std::floor(h * sc * 0.75f));
      case kBSocket: return std::max(8.0f, std::floor(h * sc * 0.55f));
      default: return std::max(4.0f, std::floor(h * sc));     // a bus, a junction
    }
  };
  auto gapFor = [](float sc) {
    return std::max(kBandAir, std::floor((float)kPitchPx * sc * 0.22f * 0.5f) * 2.0f);
  };
  // The air BETWEEN two layers: the full gap only where two cells meet.
  auto gapAt = [&](int upper, float sc) {
    const float gFull = gapFor(sc);
    if (upper <= 0 || upper >= g.layers) return gFull;
    const int ba = layerB[(size_t)upper], bb = layerB[(size_t)(upper - 1)];
    if (ba == kBCell && bb == kBCell) return gFull;
    // A FAN NEEDS ROOM TO BE A FAN. The one gap on the page that is not air is
    // the one between a junction and the socket row it opens into: those
    // strokes ARE the split, and at the 2 px hug below they come out a
    // horizontal bar with two blobs on it. Everything else can touch.
    // It gets exactly the gap two CELLS get - no more: measured on this scene,
    // a gap of 42 at 1x put `sand gust shotgun projectile` five pixels over the
    // band and dropped the whole page to the overview rung to buy a steeper V.
    const bool sa = layerSplit[(size_t)upper] != 0,
               sb = layerSplit[(size_t)(upper - 1)] != 0;
    if ((ba == kBSocket && sb) || (sa && bb == kBSocket)) return gFull;
    // ...and a bead may not land on a pip, which after the reorder is the layer
    // next to it in both orders.
    if ((ba == kBSocket && bb == kBBead) || (ba == kBBead && bb == kBSocket))
      return std::max(6.0f, std::floor(10.0f * sc * 0.5f) * 2.0f);
    // Furniture TOUCHES what it belongs to: a pip two pixels off the bus it
    // feeds is a pip on the bus, which is what it means, and every 10 px of
    // air between two rows of marks is 10 px the cells above do not get. It
    // still follows the rung - at 2x a two-pixel gap is a hairline nobody
    // drew on purpose.
    // TIGHT WHERE IT HAS TO FIT, OPEN WHERE IT DOES NOT. At the fit rungs
    // (half and below) every pixel of air is a pixel the cells lose and the
    // drawing is read as a shape, so furniture hugs. From 1x up you have
    // deliberately zoomed in to READ it, panning is expected, and the strokes
    // need room to be strokes - at a two-pixel gap a quill ribbon between a
    // pip and its bus is not a stroke, it is a smudge.
    if (sc <= 0.5f) return 2.0f;
    return std::floor(10.0f * sc * 0.5f) * 2.0f;
  };
  auto heightFor = [&](float sc) {
    float h = 0.0f;
    for (int L = g.layers - 1; L >= 0; L--) {
      h += bandFor(layerB[(size_t)L], layerH[(size_t)L], sc);
      if (L > 0) h += gapAt(L, sc);
    }
    return h;
  };
  // The margin the drawing needs on every side, and the only thing it is used
  // for is deciding the FIT: the layout already counts every node's box in
  // `width`, but a node's hover glow, its count pips and an accept ring all
  // bleed a few pixels past the box, and a figure touching its frame reads as
  // a figure that has been cut.
  // SIX, NOT TEN (2026-09-22). The margin is only ever used to decide the
  // FIT, and four pixels on each side is the difference between a four-word
  // spell standing at the readable rung and the same spell dropped to the
  // overview one. The bleed it was protecting - a hover bracket, a tally - is
  // a few pixels, not ten.
  constexpr float kMargin = 6.0f;
  const float avail = size.x - kMargin * 2;
  const float availY = size.y - kMargin * 2;
  // THE FIT: what the view shows when nobody has touched it. 1x if the whole
  // drawing stands in the band, else 0.5x — and if 0.5x still does not fit, it
  // is drawn at 0.5x anyway and the wheel and the drag are how you read the
  // rest. (They replaced a pair of scrollbars, which is the same offer made
  // worse: a bar can only ever show you a drawing at the one size it already
  // did not fit at.)
  float autoScale = 1.0f;
  if ((float)g.width > avail || heightFor(1.0f) > availY) autoScale = 0.5f;
  // AND DOWN TO THE OVERVIEW RUNG IF IT MUST (2026-09-22). The fit used to
  // stop at 0.5x and let anything bigger be CUT by the band's clip - which
  // meant a ten-word page opened with its top cell and its pedestal sliced
  // through the middle, and nothing on screen said that was a view and not a
  // rendering fault. A whole drawing at 0.25x is a worse READ and a better
  // PICTURE: you can see the shape of the spell, and one wheel click back is
  // the size you can read it at. Only the fit falls this far; the wheel still
  // goes wherever you put it.
  if (autoScale == 0.5f &&
      ((float)g.width * 0.5f > avail || heightFor(0.5f) > availY))
    autoScale = 0.25f;

  // ---- THE ZOOM LADDER -------------------------------------------------------
  //
  // Quarters, then halves, then whole numbers. NOTHING between, ever: a 2x
  // sprite at 0.5x is its authored size, at 0.25x it is an exact 2:1 decimation
  // of it, and at 2x/3x/4x it is whole pixels — while a scale of, say, 1.3
  // would land every engraving in this file on a half pixel. The wheel steps
  // the ladder; it does not multiply anything.
  //
  // 0.25x IS THE OVERVIEW RUNG (2026-09-22, asked for): a 64 px cell is 16 px
  // on screen and the engravings are a smudge, which is the point — at that
  // size you are reading the SHAPE of a spell too tall for the band, not its
  // words, and the fit rung is one wheel click back. The fit never chooses it;
  // only you can.
  static const float kLadder[] = {0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f};
  constexpr int kLadderN = (int)(sizeof(kLadder) / sizeof(kLadder[0]));
  auto rungOf = [&](float sc) {
    int best = 0;
    for (int i = 1; i < kLadderN; i++)
      if (std::fabs(kLadder[i] - sc) < std::fabs(kLadder[best] - sc)) best = i;
    return best;
  };
  float scale = s.spellGraphZoom > 0.0f ? s.spellGraphZoom : autoScale;
  // Where the drawing's top-left sits, for a given scale and the pan in hand:
  // CENTRED in the view, then moved by the pan. One expression, used once for
  // the wheel's anchor and once for the frame, so the two cannot disagree.
  auto originFor = [&](float sc, float px, float py) {
    const float w = std::floor((float)g.width * sc), h = heightFor(sc);
    return ImVec2(std::floor(viewMin.x + (size.x - w) * 0.5f + px),
                  std::floor(viewMin.y + (size.y - h) * 0.5f + py));
  };
  // WHEEL: zoom about the CURSOR, not about the centre. Zooming about the
  // centre makes you chase the thing you are looking at across the band with
  // the other hand; zooming about the pointer keeps whatever is under it
  // exactly where it is, which is the whole reason a zoom is usable at all.
  {
    const float wheel = ImGui::GetIO().MouseWheel;
    const bool overGraded = gOverGradedCell;
    gOverGradedCell = false;   // the cells below set it again if still hovered
    if (wheel != 0.0f && !overGraded &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        !ImGui::GetDragDropPayload()) {
      const int want = std::clamp(rungOf(scale) + (wheel > 0.0f ? 1 : -1), 0,
                                  kLadderN - 1);
      const float next = kLadder[want];
      if (next != scale) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const ImVec2 o0 = originFor(scale, s.spellGraphPanX, s.spellGraphPanY);
        // The point of the DRAWING under the cursor, in layout units, and the
        // pan that puts it back under the cursor at the new scale.
        const float qx = (m.x - o0.x) / scale, qy = (m.y - o0.y) / scale;
        const float w1 = std::floor((float)g.width * next), h1 = heightFor(next);
        s.spellGraphPanX = m.x - qx * next - (viewMin.x + (size.x - w1) * 0.5f);
        s.spellGraphPanY = m.y - qy * next - (viewMin.y + (size.y - h1) * 0.5f);
        s.spellGraphZoom = next;
        scale = next;
      }
    }
  }
  const float gap = gapFor(scale);
  const float gw = std::floor((float)g.width * scale);
  const float gh = heightFor(scale);
  // THE PAN IS CLAMPED so a hand of the drawing is always on screen. Without
  // it the one gesture that moves the view is also the one that loses it, and
  // an empty recess with a tree somewhere off to the left of it looks exactly
  // like a bug.
  {
    const float slackX = std::max(0.0f, (gw + size.x) * 0.5f - 64.0f);
    const float slackY = std::max(0.0f, (gh + size.y) * 0.5f - 48.0f);
    s.spellGraphPanX = std::clamp(s.spellGraphPanX, -slackX, slackX);
    s.spellGraphPanY = std::clamp(s.spellGraphPanY, -slackY, slackY);
  }
  // Where each layer's TOP sits inside the drawing, walking down from the
  // topmost layer (the highest index; the root is layer 0 at the bottom).
  std::vector<float> layerTop((size_t)std::max(1, g.layers), 0.0f);
  // How a node's OWN geometry maps into its band. A cell's band is its size,
  // so this is just the rung; a squashed band's is smaller, and every offset
  // inside the node (a pip's top, a bus's mid-line) follows it - which is what
  // keeps the hit test, the art and the strokes agreeing about where a pip is.
  std::vector<float> layerY((size_t)std::max(1, g.layers), scale);
  {
    float t = 0.0f;
    for (int L = g.layers - 1; L >= 0; L--) {
      layerTop[(size_t)L] = t;
      const float bh = bandFor(layerB[(size_t)L], layerH[(size_t)L], scale);
      layerY[(size_t)L] =
          layerH[(size_t)L] > 0.0f ? bh / layerH[(size_t)L] : scale;
      t += bh + (L > 0 ? gapAt(L, scale) : 0.0f);
    }
  }

  {
    static const bool dbg = std::getenv("SANDVOX_UI_DEBUG") != nullptr;
    static int sLast = -1;
    if (dbg && (int)g.nodes.size() != sLast) {
      sLast = (int)g.nodes.size();
      std::printf("[canvas] view=%.0fx%.0f scale=%.2f gw=%.0f gh=%.0f avail=%.0f "
                  "layers=%d nodes=%d gap=%.0f\n",
                  size.x, size.y, (double)scale, gw, gh, availY, g.layers,
                  (int)g.nodes.size(), gap);
      for (int L = g.layers - 1; L >= 0; L--)
        std::printf("   layer %d: h=%.0f top=%.0f\n", L, layerH[(size_t)L],
                    layerTop[(size_t)L]);
      for (const UIState::SpellGraphUI::Node& n : g.nodes)
        std::printf("   node k=%d layer=%d base=%d x=%d w=%d h=%d '%s'\n", n.kind,
                    n.layer, n.baseLayer, n.x, n.w, n.h, n.label.c_str());
    }
  }
  const ImVec2 org = originFor(scale, s.spellGraphPanX, s.spellGraphPanY);
  // X is the layout's, scaled. Y is the node's LAYER at the screen pitch, plus
  // an offset INSIDE the node (which is scaled): a node's own geometry is the
  // layout's, its band is the canvas's.
  auto X = [&](int gx) { return std::floor(org.x + (float)gx * scale); };
  auto Y = [&](const UIState::SpellGraphUI::Node& n, int off) {
    const size_t L = (size_t)std::clamp(n.layer, 0, g.layers - 1);
    return std::floor(org.y + layerTop[L] + (float)off * layerY[L]);
  };
  auto NodeMin = [&](const UIState::SpellGraphUI::Node& n) {
    return ImVec2(X(n.x), Y(n, 0));
  };
  auto NodeMax = [&](const UIState::SpellGraphUI::Node& n) {
    return ImVec2(X(n.x + n.w), Y(n, n.h));
  };
  auto TopC = [&](const UIState::SpellGraphUI::Node& n) {
    return ImVec2(X(n.x + n.w / 2), Y(n, 0));
  };
  auto BotC = [&](const UIState::SpellGraphUI::Node& n) {
    return ImVec2(X(n.x + n.w / 2), Y(n, n.h));
  };
  // The bottom of a BAND, which is not the bottom of any one node in it: a
  // spoken delivery's edge into its parent leaves from the bottom of its whole
  // SPAN (`baseLayer`), and the node standing in that band is some cell three
  // layers below the bar.
  auto BandBot = [&](int layer) {
    const size_t L = (size_t)std::clamp(layer, 0, g.layers - 1);
    return std::floor(org.y + layerTop[L] + bandFor(layerB[L], layerH[L], scale));
  };

  // ---- THE SHEET, under everything ------------------------------------------
  // In page coordinates, so the ruling, the marginal pricking, the foxing and
  // the great figure all travel with the drawing under a drag. That is the
  // whole reason the ground moved down here from the top of the function.
  PaintLeaf(dl, viewMin, viewMax, org, scale, gw, gh);

  ImGui::PushFont(FontSmall());

  // ---- what is in flight ----------------------------------------------------
  const ImGuiPayload* live = ImGui::GetDragDropPayload();
  // LOOK-ITERATION ONLY: `SANDVOX_UI_FAKE_DRAG=<glyph>` draws the page as if
  // that glyph were held, so `--shot-spellpage` can photograph the candidate
  // layer without a scripted mouse. Never delivered: a fake payload reaches
  // `judge` and the markers, and no ImGui target ever accepts it.
  {
    static const char* fakeName = std::getenv("SANDVOX_UI_FAKE_DRAG");
    static ImGuiPayload fake;
    static char fakeBuf[64];
    if (!live && fakeName && *fakeName) {
      fake.Clear();
      std::snprintf(fakeBuf, sizeof fakeBuf, "%s", fakeName);
      std::snprintf(fake.DataType, sizeof fake.DataType, "%s", kPayloadGlyph);
      fake.Data = fakeBuf;
      fake.DataSize = (int)sizeof fakeBuf;
      // IsDataType() is false on a payload with no frame stamp (Clear sets -1).
      fake.DataFrameCount = ImGui::GetFrameCount();
      live = &fake;
    }
  }
  const bool copyMod = ImGui::GetIO().KeyCtrl;
  const bool full = (int)s.grimoireEditWords.size() >= s.grimoireMaxWords;
  // Set by whichever target is under the cursor; drawn after every node, so a
  // marker on a cell's edge is never buried by the next cell's recess.
  ImVec2 acceptA, acceptB, refuseA, refuseB, ghostA, ghostB;
  bool hasAccept = false, hasRefuse = false, hasGhost = false;
  static std::string sNote;
  sNote.clear();

  // THE BAR THAT OWNS A SYNTHESIZED NODE. A bus and a socket have no tree node
  // of their own, so every gesture on one has to find the join that lists it.
  // By search rather than by a stored back-pointer, because the mirror is a flat
  // list and the search is over a handful of nodes.
  // A BACK-POINTER, NOT A SEARCH (2026-09-22). It used to scan every join's
  // socket list, which cannot see a SUB-fan's pip — a branch that splits again
  // has sockets of its own and they are in nobody's list — so every drop on one
  // refused with "that mark has no box". `owner` is set by the layout, where
  // the answer is known, on every socket, bus and junction at every depth.
  auto boxOf = [&](int nodeIdx) {
    if (nodeIdx < 0 || nodeIdx >= (int)g.nodes.size()) return -1;
    const int o = g.nodes[(size_t)nodeIdx].owner;
    return (o >= 0 && o < (int)g.nodes.size()) ? o : -1;
  };
  // A cell that is a copy answers for the cell that owns the record.
  auto primaryOf = [&](int nodeIdx) {
    if (nodeIdx < 0 || nodeIdx >= (int)g.nodes.size()) return nodeIdx;
    const int p = g.nodes[(size_t)nodeIdx].primary;
    return p >= 0 ? p : nodeIdx;
  };

  // WHICH BOX AN ITEM STANDS IN (2026-10-01). A word cell carries its `lane`
  // but not the box it is in, and the drop that matters most - "put this next
  // to THAT word" - needs both. The edges know: every item's FIRST outgoing
  // stroke runs toward its box (into the bus, a socket, the anchor, or the
  // operator it is an operand of, which is itself an item), so the box is the
  // first piece of box furniture up that chain. `up` is built once a frame.
  std::vector<int> up(g.nodes.size(), -1);
  for (const UIState::SpellGraphUI::Edge& e : g.edges)
    if (e.from >= 0 && e.from < (int)up.size() && e.to >= 0 &&
        e.to < (int)g.nodes.size() && up[(size_t)e.from] < 0)
      up[(size_t)e.from] = e.to;
  auto itemBox = [&](int idx) {
    int cur = primaryOf(idx);
    for (int step = 0; step < 64 && cur >= 0; step++) {
      const int to = up[(size_t)cur];
      if (to < 0) return -1;
      const int k = g.nodes[(size_t)to].kind;
      if (k == kBus || k == kSocket || k == kSplit) {
        const int o = boxOf(to);
        return o >= 0 ? primaryOf(o) : -1;
      }
      if (k == kJoin || k == kRoot) return primaryOf(to);
      cur = to;
    }
    return -1;
  };
  // Is box `inner` the box `outer` or anywhere inside it? A branch moved into
  // its own contents has nowhere to be, and the candidate layer must not offer
  // the drop only for main.cpp to refuse it after the release.
  auto boxWithin = [&](int inner, int outer) {
    for (int b = inner, step = 0; b >= 0 && step < 64; b = itemBox(b), step++)
      if (b == outer) return true;
    return false;
  };

  // THE ONE DROP DECISION. Pure: given a target and a payload, which tree op
  // the gesture means and what to say about it - or why not. Called by the
  // hovered target (which latches the op on release) AND by every other
  // target on the page while a drag is in flight, which is what lets the page
  // mark EVERY place the held thing could go instead of only the one under
  // the cursor. The UI never applies an op — main.cpp does, from
  // game/spellgraph.h, and answers a refusal on the status line.
  //
  // `say == nullptr` on a refusal means SILENT: a drop that would change
  // nothing (a word onto itself), which deserves neither a marker nor a
  // sentence.
  enum class Tgt { Bar, Bus, Socket, Split, Body, PipL, PipR, Cell, Ahead, Beside };
  struct Verdict {
    bool ok = false;
    const char* say = nullptr;
    UIState::GraphEditIntent e;
  };
  auto judge = [&](Tgt tgt, int nodeIdx, const ImGuiPayload* p) -> Verdict {
    Verdict v;
    auto no = [&](const char* why) {
      v.ok = false;
      v.say = why;
      return v;
    };
    if (!p || nodeIdx < 0 || nodeIdx >= (int)g.nodes.size()) return no(nullptr);
    const bool fromGraph = p->IsDataType(kPayloadGraphNode);
    const bool page = p->IsDataType(kPayloadPage);
    if (!fromGraph && !page && !p->IsDataType(kPayloadGlyph)) return no(nullptr);
    const UIState::SpellGraphUI::Node& n = g.nodes[(size_t)nodeIdx];
    if (readOnly)
      return no("an authored page cannot be changed - copy it to make one of your own");

    const char* name = fromGraph ? nullptr : (const char*)p->Data;
    const int srcGraph = fromGraph ? *(const int*)p->Data : -1;
    if (fromGraph && (srcGraph < 0 || srcGraph >= (int)g.nodes.size() ||
                      g.nodes[(size_t)srcGraph].treeNode < 0))
      return no(nullptr);
    const int sort = fromGraph ? -2 : PayloadSort(s, page, name);
    if (sort == kSeparator)
      return no("`lane` and `end` are structure, not items - drop onto a socket "
                "instead");
    if (!fromGraph && full) return no("this page is full - remove a word to make room");

    UIState::GraphEditIntent& e = v.e;
    const char* say = nullptr;
    // Which BOX, and which LANE of it, for every target that puts something
    // INTO a box's pile; resolved by the cases below, then shared.
    int boxIdx = -1, lane = 0;
    switch (tgt) {
      case Tgt::Cell: {
        // The body of an operator: fill the first EMPTY slot, left before
        // right. Left is "the word before it", which is the one a player
        // dragging at a hollow pip is usually aiming for; the pips themselves
        // are still there to say "the other one".
        if (fromGraph) return no("drop a branch on a socket or a bus, not into an operator");
        const int side = (n.hasLeft && !n.leftFilled) ? 0
                       : (n.hasRight && !n.rightFilled) ? 1 : -1;
        if (side < 0) return no("both of this operator's slots are filled");
        e.op = UIState::GraphEditIntent::FillSlot;
        e.treeNode = n.treeNode;
        e.side = side;
        e.glyphId = name;
        v.ok = true;
        v.say = side == 0 ? "fill this operator's left slot"
                          : "fill this operator's right slot";
        return v;
      }
      case Tgt::PipL:
      case Tgt::PipR: {
        if (fromGraph) return no("a slot takes a word from the arsenal, not a branch");
        e.op = UIState::GraphEditIntent::FillSlot;
        e.treeNode = n.treeNode;
        e.side = tgt == Tgt::PipL ? 0 : 1;
        e.glyphId = name;
        v.ok = true;
        v.say = tgt == Tgt::PipL ? "fill this operator's left slot"
                                 : "fill this operator's right slot";
        return v;
      }
      case Tgt::Ahead: {
        // The empty slot off the top of the page: the whole sentence goes in
        // this delivery. `nodeIdx` is the HAND, and `WrapInBox` on a box with
        // no parent is exactly "a delivery word at the end of a sentence".
        if (fromGraph || sort != kDelivery)
          return no("only a delivery goes here - it boxes the whole spell");
        e.op = UIState::GraphEditIntent::Wrap;
        e.treeNode = n.treeNode;
        e.glyphId = name;
        v.ok = true;
        v.say = "box the WHOLE spell in this delivery";
        return v;
      }
      case Tgt::Body: {
        if (fromGraph) return no(nullptr);
        if (sort != kDelivery)
          return no("only a delivery boxes a branch - drop a word on the bus");
        e.op = UIState::GraphEditIntent::Wrap;
        e.treeNode = n.treeNode;
        e.glyphId = name;
        v.ok = true;
        v.say = "box this branch in a delivery of its own";
        return v;
      }
      case Tgt::Beside: {
        // ONTO ANOTHER WORD = BESIDE IT (2026-10-01). A word cell used to
        // refuse everything but a delivery, so most of a drawing was dead
        // ground under a drag and "put it next to fire" meant hunting for the
        // bus fire hangs off. The obvious reading of dropping one word on
        // another is "into the same pile", and that is what it does: the box
        // and lane the target word stands in.
        if (fromGraph && primaryOf(srcGraph) == primaryOf(nodeIdx)) return no(nullptr);
        boxIdx = itemBox(nodeIdx);
        if (boxIdx < 0) return no("that word stands in no box");
        lane = n.lane;
        break;
      }
      case Tgt::Bar:
      case Tgt::Bus:
      case Tgt::Split:
      case Tgt::Socket: {
        // Which BOX, and which LANE of it. A bar and a bus are the box's shared
        // segment (lane 0); a socket is instance i's own lane, and a socket
        // with no lane yet OPENS the next one (spellgraph.h: lane ==
        // laneCount + 1 opens, anything past that is refused).
        boxIdx = primaryOf(nodeIdx);
        if (tgt == Tgt::Bar) {
          // A SPLIT DELIVERY IS DRAWN ONCE PER BRANCH, and a drop on branch k's
          // cell is a drop on branch k — which is a 64 px target for the
          // gesture that used to need a 32 px pip. `instance` is -1 on the one
          // cell an unsplit box has, and that is the shared segment as before.
          const int32_t inst = g.nodes[(size_t)nodeIdx].instance;
          if (inst >= 0) lane = inst + 1;
        } else {
          // A bus, a socket and the junction are synthesized: their box is the
          // cell that owns them. The junction is a SHARED target (lane 0), like
          // the bus - it is the one line every branch comes out of.
          boxIdx = boxOf(nodeIdx);
          if (boxIdx < 0) return no("that mark has no box");
          if (tgt == Tgt::Socket) {
            // EVERY SOCKET IS A TARGET. This used to refuse any socket past
            // `laneCount + 1` with "give the sockets before this one a payload
            // first" — true of the grammar, invisible in the drawing, and
            // maddening on a `shotgun` box, where three sockets stand over ZERO
            // lanes and instance 0 (the aim) is the MIDDLE one, so the order
            // was middle, right, left with nothing saying so. `InsertItem`
            // opens the lanes in between empty now, which is exactly what those
            // sockets already were.
            lane = n.lane > 0 ? n.lane : n.instance + 1;
          }
        }
        break;
      }
    }
    {
        const UIState::SpellGraphUI::Node& bn = g.nodes[(size_t)boxIdx];
        if (fromGraph) {
          const int src = primaryOf(srcGraph);
          // NOWHERE TO GO: already in this pile (a move that is no move), or
          // into its own contents.
          if (!copyMod && itemBox(src) == boxIdx &&
              g.nodes[(size_t)src].lane == lane)
            return no(nullptr);
          if ((g.nodes[(size_t)src].kind == kJoin) && boxWithin(boxIdx, src))
            return no("a branch cannot go inside itself");
          e.op = UIState::GraphEditIntent::Move;
          e.treeNode = g.nodes[(size_t)srcGraph].treeNode;
          e.boxTreeNode = bn.treeNode;
          e.lane = lane;
          e.copy = copyMod;
          say = copyMod ? "ctrl: a copy of that branch lands here"
                        : "move that branch here";
        } else if (sort == kMod) {
          const UIState::GlyphUI* mg = FindGlyph(s, name ? name : "");
          // A SPLIT MAY NOT BE ANSWERED BY OPENING A LANE (2026-09-22).
          //
          // A socket drop asks for lane `instance + 1`, and a socket with no
          // lane of its own OPENS one. That is right for a payload and wrong for
          // a `shotgun` or a `twin`, because a count is the word that MAKES
          // sockets: dropping one on a bare socket wrapped it in `lane ... end`,
          // which moved the split down into a branch that did not exist until
          // the drop invented it. On a blank page the whole gesture came out as
          // `lane shotgun end` - one bare instance carrying a count - and the
          // three branches the player dragged the word in for were never drawn.
          //
          // So a count lands in the scope the socket BELONGS TO unless that
          // socket is a lane the player actually opened, and a split word
          // always fans what you dropped it on.
          //
          // ...ONCE. A box that ALREADY fans has had that drop; a second count
          // aimed at one of its sockets means "split THAT branch" - a twin of a
          // twin, contained - so the clamp stands down and the lane opens after
          // all. `bn.split` is the junction and it wears the count word that
          // opened it, which is exactly "the trunk already holds a split".
          //
          // `AttachMod` IS THE TRUTH and enforces both for every caller; the
          // same test is made here so the sentence under the cursor and the
          // ghost describe the drop that is actually going to happen.
          const bool trunkSplits =
              bn.split >= 0 && bn.split < (int)g.nodes.size() &&
              g.nodes[(size_t)bn.split].treeNode >= 0;
          if (mg && mg->splits && lane > bn.laneCount && !trunkSplits) lane = 0;
          e.op = UIState::GraphEditIntent::AttachMod;
          e.treeNode = bn.treeNode;
          e.lane = lane;
          e.glyphId = name;
          if (mg && mg->splits)
            say = lane > 0 ? "this splits that branch again - a fan inside a fan"
                           : "this splits the whole box into its branches";
          else
            say = lane > 0 ? "this mod edits that instance alone"
                           : "this mod edits the whole record";
        } else {
          e.op = UIState::GraphEditIntent::Insert;
          e.treeNode = bn.treeNode;
          e.lane = lane;
          e.glyphId = name;
          say = lane > 0 ? "this word rides that instance alone"
                         : "this word joins the shared payload";
        }
    }
    v.ok = true;
    v.say = say;
    return v;
  };

  // EVERY TARGET ON THE PAGE, this frame, so the candidate layer can ask
  // `judge` about each of them once the drawing is done.
  struct Target {
    Tgt tgt;
    int node;
    ImVec2 a, b;
  };
  std::vector<Target> targets;

  // THE HOVERED TARGET: peek at the payload, ask `judge`, describe the answer
  // to the marker layer, and only latch the intent on the actual release.
  auto offer = [&](Tgt tgt, int nodeIdx, ImVec2 a, ImVec2 b) {
    targets.push_back({tgt, nodeIdx, a, b});
    if (!ImGui::BeginDragDropTarget()) return;
    const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadGraphNode, kPeekFlags);
    if (!p) p = ImGui::AcceptDragDropPayload(kPayloadGlyph, kPeekFlags);
    if (!p) p = ImGui::AcceptDragDropPayload(kPayloadPage, kPeekFlags);
    if (!p) {
      ImGui::EndDragDropTarget();
      return;
    }
    Verdict v = judge(tgt, nodeIdx, p);
    if (!v.ok) {
      if (v.say) {
        refuseA = a;
        refuseB = b;
        hasRefuse = true;
        sNote = v.say;
        res.refused = true;
      }
      ImGui::EndDragDropTarget();
      return;
    }
    acceptA = a;
    acceptB = b;
    hasAccept = true;
    if (sNote.empty() && v.say) sNote = v.say;
    // The ghost: where the node will land. A socket's whole column, a pip's
    // ring, a bar's own rect - the marker layer draws the same shape as the
    // thing that is coming.
    ghostA = a;
    ghostB = b;
    hasGhost = true;
    if (p->IsDelivery()) {
      PushGrimoireUndo(s);
      v.e.pending = true;
      s.graphEdit = v.e;
    }
    ImGui::EndDragDropTarget();
  };

  // ---- THE CLASP: an operator and its row are ONE word -----------------------
  //
  // `_ trail`, `_ mend`, `_ null` do nothing on their own: each is half a word,
  // FUSED to whatever stands in its slot, and the pair is one thing the sentence
  // says once. Nothing on the page said so - an operand was a cell with a
  // stroke, exactly like an item feeding a bus, and the two mean completely
  // different things. So the operator's cell and the row it binds are drawn
  // inside one faint enclosure: the reader's box round a phrase that has to be
  // read together. Drawn UNDER the strokes and the cells, in the palest ink the
  // page has, because it is a grouping and not a mark.
  for (const UIState::SpellGraphUI::Node& n : g.nodes) {
    if (n.kind != kOperator || !(n.hasLeft || n.hasRight)) continue;
    const float pad = std::max(3.0f, std::floor(6.0f * scale));
    const ImVec2 ca(X(n.subX) - pad, Y(n, 0) - pad);
    const ImVec2 cb(X(n.subX + n.subW) + pad, BandBot(n.baseLayer) + pad);
    if (cb.x <= ca.x || cb.y <= ca.y) continue;
    dl->AddRectFilled(ca, cb, Fade(ColIronSoft(), 0.07f), 8.0f);
    dl->AddRect(ca, cb, Fade(ColIronSoft(), n.complete ? 0.28f : 0.42f), 8.0f, 0,
                2.0f);
  }

  // ---- the strokes, under every node ----------------------------------------
  //
  // Drawn FIRST so a stroke never crosses a cell, and as quill ribbons rather
  // than as thick polylines (see `InkStroke`): continuous, tapered, and with a
  // hand's tremor in them, which between them are the whole difference between
  // a drawn figure and a wire diagram.
  //
  // COLOUR IS A CLAIM, NOT DECORATION. On the page every stroke is iron gall,
  // the ink the figure is drawn in - except the two that say something the
  // shape does not: a BUS lane and a SOCKET lane are rubricated, because what
  // they mark is "this payload is shared" and "this instance has one of its
  // own", and that is exactly the kind of thing a rubricator picked out in
  // vermilion. A stroke out of an incomplete or wasted node is drawn in the
  // pale ink of an unfinished passage.
  for (const UIState::SpellGraphUI::Edge& e : g.edges) {
    if (e.from < 0 || e.to < 0 || e.from >= (int)g.nodes.size() ||
        e.to >= (int)g.nodes.size())
      continue;
    const UIState::SpellGraphUI::Node& a = g.nodes[(size_t)e.from];
    const UIState::SpellGraphUI::Node& b = g.nodes[(size_t)e.to];
    const bool dim = !a.complete || a.wasted;
    ImU32 col = ColIronGall();
    float weight = 8.0f;
    switch (e.kind) {
      case kEBus:
        // The one stroke that stays rubricated: what reaches the bus is
        // SHARED, and picking out the thing that governs the whole passage is
        // exactly what a rubricator's red is for.
        col = ColRubric();
        weight = 5.0f;
        break;
      case kESocket:
        // ...and the one that stopped being. A page where the trunk, the
        // lanes, the buses, the pips and the great figure were all vermilion
        // was a red drawing with brown accidents in it - the opposite of the
        // manuscript it is quoting, where the red is what your eye lands on
        // BECAUSE everything else is brown.
        col = ColIronGall();
        weight = 6.0f;
        break;
      case kSlot:
        col = SortPigment(a.sort);
        weight = 5.0f;
        break;
      case kFan:
        col = ColIronSoft();
        weight = 5.0f;
        break;
      default:
        break;
    }
    if (dim) col = Fade(ColIronSoft(), 0.55f);
    // WHICH WAY THIS ONE RUNS. Since the inversion a child is not always above
    // its parent: a spoken delivery's sockets, beads and bus all sit UNDER its
    // bar, while the hand's sit over it and every item sits over the bus it
    // feeds. So the anchors are chosen from the two bands, not assumed - and
    // the upward one leaves from the child's SPAN bottom, so a box hands off at
    // the foot of its own stack instead of dropping a stroke from its bar
    // through every cell it contains.
    const bool above = a.baseLayer > b.layer;
    ImVec2 from = above ? ImVec2(X(a.x + a.w / 2), BandBot(a.baseLayer)) : TopC(a);
    ImVec2 to = above ? TopC(b) : BotC(b);
    if (e.kind == kEBus) {
      // A lane meets the BUS where it stands over it, not at the bus's centre:
      // the bus is one long stroke and the lozenge is the junction.
      const int bx = std::min(std::max(a.x + a.w / 2, b.x), b.x + b.w);
      to = ImVec2(X(bx), Y(b, b.h / 2));
    } else if (e.kind == kSlot) {
      // Into the PIP the operand fills, so which side bound is visible.
      const bool leftSide = (a.x + a.w / 2) < (b.x + b.w / 2);
      to = ImVec2(X(leftSide ? b.x : b.x + b.w), Y(b, b.h / 2));
    }
    InkStroke(dl, from, to, col, weight * (scale < 1.0f ? 0.62f : 1.0f));
    // The BLOT where a stroke meets the thing it came out of: a quill leaves
    // one, and it is what stops a taper from reading as a gap between the
    // stroke and the cell above it.
    PixelDisc(dl, from, scale < 1.0f ? 2.0f : 3.0f, col);
    if (e.kind == kEBus) PixelLozenge(dl, to, scale < 1.0f ? 4.0f : 6.0f, col, true, 2.0f);
  }

  // ---- the nodes -------------------------------------------------------------
  for (size_t i = 0; i < g.nodes.size(); i++) {
    const UIState::SpellGraphUI::Node& n = g.nodes[i];
    const ImVec2 a = NodeMin(n), b = NodeMax(n);
    ImGui::PushID(9000 + (int)i);
    switch (n.kind) {
      case kSplit: {
        // THE JUNCTION. One blot on the box's axis, where the trunk stops being
        // one line and the fan starts being several. It is drawn rather than
        // implied because the thing the player needs to see is WHERE the split
        // happens - the fan used to leave the delivery cell, which said the
        // delivery did it, and it does not: the mod bead sitting on this blot
        // does.
        //
        // ...AND WHEN A WORD OPENED IT, THE JUNCTION IS THAT WORD (2026-09-22).
        // `shotgun` used to be a bead one band below this blot: 14 screen px of
        // lozenge and 4 of junction, between two full cells, for the single
        // most important point in the whole drawing. They are one node now and
        // it is a CELL - the spell reads "sand and gust funnel into ONE
        // shotgun, and the shotgun funnels into three", which is what the
        // sentence says and was never what the page showed.
        const ImVec2 c(std::floor((a.x + b.x) * 0.5f),
                       std::floor((a.y + b.y) * 0.5f));
        if (n.treeNode >= 0) {
          const ImU32 col = ColOrpiment();
          const float lr = std::floor(std::min(b.x - a.x, b.y - a.y) * 0.5f);
          if (scale < 0.5f) {
            PixelLozenge(dl, c, lr, Fade(col, 0.95f), true, 2.0f);
          } else {
            PixelLozenge(dl, c, lr, Fade(ColVellumHi(), 0.85f), true, 2.0f);
            PixelLozenge(dl, c, lr, Fade(col, 0.95f), false, 2.0f);
            PixelLozenge(dl, c, lr - 4.0f, Fade(ColIronGall(), 0.35f), false, 2.0f);
            DrawSpriteCenteredAt(dl, GlyphIcon(kMod), ImVec2(c.x + 1, c.y + 1),
                                 scale * 0.5f, Fade(ColVellumHi(), 0.7f));
            DrawSpriteCenteredAt(dl, GlyphIcon(kMod), c, scale * 0.5f, ColIronGall());
          }
          // NO TALLY HERE. A cell wears one to say what its picture cannot -
          // a collapsed sub-fan, a word said twice - and this junction's
          // picture says it outright: the strokes leaving its point ARE the
          // count. Drawn, the ticks land in the fan's own convergence and read
          // as a fourth and fifth stroke.
          ImGui::SetCursorScreenPos(a);
          ImGui::InvisibleButton("##splitword",
                                 ImVec2(std::max(8.0f, b.x - a.x),
                                        std::max(8.0f, b.y - a.y)));
          const bool hov = ImGui::IsItemHovered();
          if (hov) LeafHover(dl, a, b);
          if (hov && !live) {
            BeginTip();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::ColorConvertU32ToFloat4(ColGoldHi()));
            ImGui::TextUnformatted(n.label.c_str());
            ImGui::PopStyleColor();
            if (!n.edit.empty()) ImGui::TextDisabled("%s", n.edit.c_str());
            ImGui::TextDisabled("THE SPLIT: one of it becomes %d here.",
                                n.instances);
            ImGui::TextDisabled("Drop a word on it and every branch gets it.");
            ImGui::TextDisabled("right-click to take it off");
            EndTip();
          }
          if (!readOnly && hov && !live &&
              ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            PushGrimoireUndo(s);
            s.graphEdit = {};
            s.graphEdit.pending = true;
            s.graphEdit.op = UIState::GraphEditIntent::Remove;
            s.graphEdit.treeNode = n.treeNode;
          }
          offer(Tgt::Split, (int)i, a, b);
          break;
        }
        const float r = scale < 1.0f ? 4.0f : 6.0f;
        PixelDisc(dl, c, r, ColIronGall());
        PixelRing(dl, c, r + 2.0f, Fade(ColIronSoft(), 0.5f), 2.0f);
        ImGui::SetCursorScreenPos(ImVec2(c.x - r - 6, c.y - r - 6));
        ImGui::InvisibleButton("##split", ImVec2(r * 2 + 12, r * 2 + 12));
        if (ImGui::IsItemHovered() && !live)
          Tip("THE SPLIT: one of it becomes several here. Drop a word on it and "
              "every branch gets it.");
        offer(Tgt::Split, (int)i, ImVec2(c.x - r - 6, c.y - r - 6),
              ImVec2(c.x + r + 6, c.y + r + 6));
        break;
      }
      case kBus: {
        // A straight stroke spanning every socket. Drawn here rather than as an
        // edge because it is a NODE with a width, and its ends are what tell you
        // how far the shared payload reaches.
        const ImVec2 m0(X(n.x), Y(n, n.h / 2)), m1(X(n.x + n.w), Y(n, n.h / 2));
        const float t = scale < 1.0f ? 2.0f : 4.0f;
        // A RULED LINE IN MINIUM, with a serif closing each end. The bus is
        // the one piece of furniture that says "everything on this row shares
        // this", which is a rubricator's kind of claim - and a rule that stops
        // in a serif reads as a measured span rather than as a stroke that ran
        // out of page.
        dl->AddRectFilled(ImVec2(m0.x, m0.y - t * 0.5f),
                          ImVec2(m1.x, m0.y + t * 0.5f), ColRubric());
        const float serif = scale < 0.5f ? 3.0f : 5.0f;
        for (int e = 0; e < 2; e++) {
          const float sx = e == 0 ? m0.x : m1.x - 2;
          dl->AddRectFilled(ImVec2(sx, m0.y - serif), ImVec2(sx + 2, m0.y + serif),
                            ColRubric());
        }
        // No second rule under it. It had a dotted shadow for a while and
        // the two together read as two buses a pixel apart.
        ImGui::SetCursorScreenPos(ImVec2(m0.x, m0.y - 8));
        ImGui::InvisibleButton("##bus", ImVec2(std::max(8.0f, m1.x - m0.x), 16));
        if (ImGui::IsItemHovered() && !live)
          Tip("THE BUS: what every instance carries. Drop a word here and all "
              "of them get it.");
        offer(Tgt::Bus, (int)i, ImVec2(m0.x, m0.y - 8), ImVec2(m1.x, m0.y + 8));
        break;
      }
      case kSocket: {
        // The pip, centred in its column. Instance 0 is the aim and is drawn
        // solid on the bar's centre line; an instance with a lane of its own is
        // filled, one that only carries the shared payload is hollow.
        const int pw = n.pipW;
        const ImVec2 pa(X(n.x + (n.w - pw) / 2), Y(n, 0));
        const ImVec2 pb(X(n.x + (n.w - pw) / 2 + pw), Y(n, n.h));
        const bool own = n.lane > 0;
        const ImU32 col = n.instance == 0 ? ColRubric() : ColIronGall();
        // THE TARGET IS THE COLUMN, NOT THE PIP (2026-09-21). A pip is 32 chrome
        // pixels — SIXTEEN on screen at half scale — and the columns of a
        // `shotgun` box are 32 apart, so two thirds of the socket row hit
        // nothing at all and aiming a glyph at one of three outputs was a game
        // of darts. The column plus half the gap on each side TILES the row
        // exactly: every pixel between the outer edges belongs to the nearest
        // socket, and the accept ring shows you which one you got.
        const float ha0 = X(n.x - kGapPx / 2), ha1 = X(n.x + n.w + kGapPx / 2);
        const float hb0 = Y(n, 0) - 8.0f, hb1 = Y(n, n.h) + 8.0f;
        const ImVec2 ha(ha0, hb0), hb(ha1, hb1);
        // A POINT PRICKED WITH THE COMPASS. Filled when this instance has a
        // lane of its own, hollow when it only carries what the bus carries -
        // the same distinction the squares made, in the figure the rest of the
        // page is drawn with. The aim (instance 0) gets a ring round it, since
        // it is the one socket that is not optional.
        const ImVec2 pc(std::floor((pa.x + pb.x) * 0.5f),
                        std::floor((pa.y + pb.y) * 0.5f));
        const float pr = std::max(3.0f, std::floor(std::min(pb.x - pa.x,
                                                            pb.y - pa.y) * 0.5f));
        // A HOLLOW PIP IS A DONUT. A 2 px ring at the radius a socket
        // actually gets - four or five pixels - quantises to a diamond, and a
        // row of diamonds where the drawing's whole vocabulary is circles
        // reads as a different mark meaning something else. Two discs, the
        // inner one in the page's own colour, is a circle at any size.
        if (own) {
          PixelDisc(dl, pc, pr, col);
        } else {
          PixelDisc(dl, pc, pr, col);
          PixelDisc(dl, pc, std::max(1.0f, pr - 2.0f), ColVellumHi());
        }
        // The aim (instance 0) is the socket that is not optional, and it is
        // the only one that gets a ring round it.
        if (n.instance == 0) PixelRing(dl, pc, pr + 3.0f, Fade(col, 0.6f), 2.0f);
        ImGui::SetCursorScreenPos(ha);
        ImGui::InvisibleButton("##sock", ImVec2(std::max(8.0f, hb.x - ha.x),
                                                std::max(8.0f, hb.y - ha.y)));
        // The column lights up under the cursor: with an invisible target this
        // much bigger than its pip, "which one am I on" has to be answered.
        if (ImGui::IsItemHovered())
          dl->AddRectFilled(ha, hb, Fade(ColRubricHi(), 0.14f));
        if (ImGui::IsItemHovered() && !live) {
          BeginTip();
          ImGui::PushStyleColor(ImGuiCol_Text,
                                ImGui::ColorConvertU32ToFloat4(ColGoldHi()));
          ImGui::Text("SOCKET %d%s", n.instance,
                      n.instance == 0 ? "  (the aim)" : "");
          ImGui::PopStyleColor();
          ImGui::TextDisabled(own ? "this instance has a lane of its own"
                                  : "this instance carries the shared payload");
          ImGui::TextDisabled("drop a word here to give it a payload of its own");
          if (own) ImGui::TextDisabled("right-click closes this lane and its words");
          EndTip();
        }
        // RIGHT-CLICK TAKES THE WHOLE LANE OUT, which is the only way to lose a
        // socket that is not the last one: emptying a lane by deleting its words
        // one at a time cannot drop it, because every lane above it is numbered
        // from it (game/spellgraph.h, `CloseLane`). An instance a count mod made
        // has no lane to close and the op says so on the status line.
        if (!readOnly && ImGui::IsItemHovered() && !live &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          const int boxIdx = boxOf((int)i);
          if (boxIdx >= 0) {
            PushGrimoireUndo(s);
            s.graphEdit = {};
            s.graphEdit.pending = true;
            s.graphEdit.op = UIState::GraphEditIntent::CloseLane;
            s.graphEdit.treeNode = g.nodes[(size_t)boxIdx].treeNode;
            // A socket with no lane is one a count mod made, and its index is
            // past the last lane — which is the case `CloseLane` names.
            s.graphEdit.lane = n.lane > 0 ? n.lane : n.instance + 1;
          }
        }
        offer(Tgt::Socket, (int)i, ha, hb);
        break;
      }
      case kRoot:
      case kJoin: {
        const bool root = n.kind == kRoot;
        // A DELIVERY IS A CELL THE SIZE OF A WORD (2026-09-21). It used to be a
        // 128x64 slab stretched across its whole socket row, which made the two
        // commonest nodes on the page — PROJECTILE and the hand — the only
        // rectangles in a drawing otherwise made of 64 px squares, and made
        // them read as chrome rather than as words. They are words.
        //
        // What the slab's WIDTH said — how far this delivery reaches — is said
        // by a rule behind the cell instead, spanning the socket row. Drawn
        // FIRST so the cell sits on it.
        //
        // ...ON THE HAND ONLY (2026-09-22). A spoken delivery with a fan is now
        // drawn once per BRANCH, and those cells ARE the reach: standing over
        // the whole row, they say what the rule said and say it in the
        // drawing's own vocabulary. The rule behind them was a dotted line
        // running through two cells to reach a third.
        if (root && !n.sockets.empty()) {
          int lo = INT32_MAX, hi = INT32_MIN;
          for (int sk : n.sockets) {
            if (sk < 0 || sk >= (int)g.nodes.size()) continue;
            const UIState::SpellGraphUI::Node& sn = g.nodes[(size_t)sk];
            lo = std::min(lo, sn.x);
            hi = std::max(hi, sn.x + sn.w);
          }
          if (hi > lo) {
            const float my = std::floor((a.y + b.y) * 0.5f);
            // THE REACH, as a measured span: a hairline rule with a full stop
            // at each end, the way a diagram marks a distance it means.
            DottedRule(dl, ImVec2(X(lo), my - 1), ImVec2(X(hi), my - 1),
                       Fade(ColIronSoft(), 0.75f), 6.0f, 2.0f);
            for (int ex = 0; ex < 2; ex++) {
              const float sx = X(ex == 0 ? lo : hi);
              dl->AddRectFilled(ImVec2(sx - 1, my - 6), ImVec2(sx + 1, my + 6),
                                Fade(ColIronGall(), 0.75f));
            }
          }
        }
        const ImVec2 mid(std::floor((a.x + b.x) * 0.5f),
                         std::floor((a.y + b.y) * 0.5f));
        // A DELIVERY IS THE FIGURE'S OWN ORBIT: a double ring, because it is
        // the node everything else is carried BY and a single ring would make
        // it one more word. The hand - the pedestal every spell stands on -
        // gets a third ring and the pricked circle between them, which is the
        // mark this page reserves for the thing that is not spoken.
        const ImU32 markCol =
            root ? ColIronGall()
                 : n.color ? IM_COL32(n.color & 0xFF, (n.color >> 8) & 0xFF,
                                      (n.color >> 16) & 0xFF, 255)
                           : ColAzurite();
        const float rr = std::floor(std::min(b.x - a.x, b.y - a.y) * 0.5f) - 1.0f;
        if (scale < 0.5f) {
          // THE OVERVIEW FIGURE: a ring with its core filled, which is the
          // oldest way there is to draw "a thing that carries other things".
          // Same reason the word cells lose their engraving at this rung - see
          // `LeafRoundel`.
          PixelDisc(dl, mid, rr, Fade(ColVellumHi(), 0.9f));
          PixelRing(dl, mid, rr, Fade(root ? ColIronGall() : markCol, 0.95f), 2.0f);
          PixelDisc(dl, mid, std::max(2.0f, rr - 4.0f),
                    Fade(root ? ColIronGall() : markCol, 0.65f));
        } else {
          PixelDisc(dl, mid, rr - 2.0f, Fade(ColVellumHi(), 0.65f));
          if (!root) PixelDisc(dl, mid, rr - 5.0f, Fade(markCol, 0.20f));
          PixelRing(dl, mid, rr, Fade(root ? ColIronGall() : markCol, 0.95f),
                    scale < 1.0f ? 2.0f : 3.0f);
          PixelRing(dl, mid, rr - 5.0f, Fade(ColIronGall(), 0.55f), 2.0f);
          if (root) {
            PixelRing(dl, mid, rr - 10.0f, Fade(ColRubric(), 0.55f), 2.0f);
            for (int i = 0; i < 8; i++) {
              const float ang = (float)i * 0.7853982f;
              PixelArc(dl, mid, rr - 2.5f, ang - 0.06f, ang + 0.06f,
                       Fade(ColRubric(), 0.8f), 2.0f);
            }
          }
          DrawSpriteCenteredAt(dl, root ? "glyph_ring" : GlyphIcon(kDelivery),
                               ImVec2(mid.x + 1, mid.y + 1), scale,
                               Fade(ColVellumHi(), 0.75f));
          DrawSpriteCenteredAt(dl, root ? "glyph_ring" : GlyphIcon(kDelivery), mid,
                               scale,
                               root ? ColIronGall() : Mix(markCol, ColIronGall(), 0.45f));
        }
        // How MANY of it, as tally strokes rather than as "x3" - and not at
        // all at the overview rung, where a 4 px tally is a dirty pixel.
        //
        // A BRANCH THAT SPLITS AGAIN IS DRAWN COLLAPSED, and the tally is how
        // it says so (2026-09-22). A count mod inside a lane splits THAT
        // branch, so the branch fires `bolts` of itself; until the page draws
        // that sub-fan as a row of its own, one cell wearing its count is the
        // honest drawing and an uncounted cell is a lie - the branch fires
        // three and the picture showed one.
        if (scale >= 0.5f)
          CountPips(dl, a, b, std::max(n.n, n.bolts), Fade(ColIronGall(), 0.9f));
        // A fan the grammar had to cut is the one thing on a bar that is not
        // reversible by looking harder, so it keeps a mark of its own: a blood
        // notch at the bar's right end. What it was cut TO is in the tip.
        if (!root && n.instancesClamped)
          dl->AddRectFilled(ImVec2(b.x - 8, a.y + 4), ImVec2(b.x - 4, b.y - 4),
                            ColBloodHi());
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##bar", ImVec2(std::max(8.0f, b.x - a.x),
                                               std::max(8.0f, b.y - a.y)));
        const bool hov = ImGui::IsItemHovered();
        if (hov) LeafHover(dl, a, b);
        if (!root && !n.page.empty()) PageSeal(dl, a, b, n.page, scale);
        if (!root) TimingTag(dl, a, b, n, scale);
        if (!root && n.page.empty()) TimingClick(n, hov, readOnly, live != nullptr);
        if (hov && !live && !n.page.empty()) {
          BeginTip();
          ImGui::PushStyleColor(ImGuiCol_Text,
                                ImGui::ColorConvertU32ToFloat4(ColGoldHi()));
          ImGui::Text("[%s]", n.page.c_str());
          ImGui::PopStyleColor();
          ImGui::TextDisabled("a written spell that is a DELIVERY, used as one glyph:");
          ImGui::TextDisabled("it carries what stands under it, with its own mods");
          if (n.hasPrice)
            ImGui::TextDisabled("tariff %d + carry %d  x%d = %d", n.tariff, n.carryCost,
                                n.priceInstances, n.subtotal);
          ImGui::TextDisabled("right-click: take the carrier off, keep what it held");
          EndTip();
        } else if (hov && !live) {
          std::string caps = n.label;
          for (char& c : caps) c = (char)toupper((unsigned char)c);
          BeginTip();
          ImGui::PushStyleColor(ImGuiCol_Text,
                                ImGui::ColorConvertU32ToFloat4(ColGoldHi()));
          if (n.n > 1) ImGui::Text("%s  x%d", caps.c_str(), n.n);
          else ImGui::TextUnformatted(caps.c_str());
          ImGui::PopStyleColor();
          if (root) {
            ImGui::TextDisabled("the hand: what you are holding when you cast");
            ImGui::TextDisabled("word %d  .  tariff %d  .  carry %d%s", g.wordCost,
                                g.tariff, g.carryCost, g.priceUnknown ? " + ?" : "");
            ImGui::TextDisabled("%d / %d mana", g.manaCost, s.manaMax);
          } else {
            // WHICH BRANCH THIS CELL IS, because there is one per branch now
            // and they are otherwise identical marks.
            if (n.instance >= 0)
              ImGui::TextDisabled("branch %d of %d%s", n.instance + 1, n.instances,
                                  n.instance == 0 ? "  (the aim)" : "");
            ImGui::TextDisabled("a delivery: %d instance%s, %d lane%s",
                                n.instances, n.instances == 1 ? "" : "s",
                                n.laneCount, n.laneCount == 1 ? "" : "s");
            // ...and if this branch splits again, the tally over it is what
            // that means. Said in words because the sub-fan is not drawn yet.
            if (n.bolts > 1)
              ImGui::TextDisabled("this branch splits again: %d bolts from it",
                                  n.bolts);
            // THE PRICE AT THIS LEVEL, which is the whole of "the cost is
            // legible at every level" — said here, where it has a line of its
            // own, instead of under a 128 px bar where it never had one.
            if (n.hasPrice)
              ImGui::TextDisabled("tariff %d + carry %d  x%d = %d", n.tariff,
                                  n.carryCost, n.priceInstances, n.subtotal);
            if (n.instancesClamped) {
              ImGui::PushStyleColor(ImGuiCol_Text,
                                    ImGui::ColorConvertU32ToFloat4(ColBloodHi()));
              ImGui::Text("the fan was cut to %d", n.priceInstances);
              ImGui::PopStyleColor();
            }
          }
          ImGui::TextDisabled("drop a word to share it, a mod to edit the record,");
          ImGui::TextDisabled("a delivery to nest this whole box inside it");
          if (!root && !n.timingPhrase.empty())
            ImGui::TextDisabled("fires %s", n.timingPhrase.c_str());
          if (!root) ImGui::TextDisabled("click: when it fires . right-click: unbox");
          EndTip();
        }
        if (!readOnly && !root && hov && !live &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          PushGrimoireUndo(s);
          s.graphEdit = {};
          s.graphEdit.pending = true;
          s.graphEdit.op = UIState::GraphEditIntent::Unbox;
          s.graphEdit.treeNode = n.treeNode;
        }
        // A delivery dropped on a bar NESTS the whole box; anything else joins
        // its shared pile. Both are `Tgt::Bar`, because which op it is depends
        // on the payload's sort and that is decided in one place.
        if (!readOnly && !root && live && DeliveryPayload(s, live)) {
          offer(Tgt::Body, (int)i, a, b);
        } else {
          offer(Tgt::Bar, (int)i, a, b);
        }
        break;
      }
      case kModTag: {
        // A BEAD ON THE TRUNK, not a caption and no longer a tab. The layout
        // puts a mod on its own layer between the bar and the fan it edits
        // (2026-09-21), centred on the same axis, so it is a plate the line
        // runs THROUGH — hence a rim all the way round in the mod's colour
        // rather than the left-edge stripe a tab wanted. The edit it carries
        // ("speed x2", "count x9") never fit in 96x32, so the plate is the
        // mod's spiral and the words are in the tip. A WASTED mod (the record
        // has no such field) is slashed: charged, and it does nothing.
        const ImU32 col = n.wasted ? ColIronSoft() : ColOrpiment();
        const ImVec2 tm(std::floor((a.x + b.x) * 0.5f), std::floor((a.y + b.y) * 0.5f));
        // A LOZENGE, and the trunk runs through it: a mod is a bead threaded
        // on the line, not a plate bolted beside it. The diamond is the one
        // shape on the page that is neither a circle nor a square, which is
        // exactly what a thing that EDITS another thing should be.
        const float lr = std::floor(std::min(b.x - a.x, b.y - a.y) * 0.5f);
        if (scale < 0.5f) {
          // A solid bead at the overview rung. The engraving inside it is a
          // 16 px sprite whatever the rung - it cannot go below its authored
          // size - so at 0.25x it was drawn OVER a bead half its height and
          // the trunk wore a yellow smudge at every mod.
          PixelLozenge(dl, tm, lr, Fade(col, 0.95f), true, 2.0f);
        } else {
          PixelLozenge(dl, tm, lr, Fade(ColVellumHi(), 0.85f), true, 2.0f);
          PixelLozenge(dl, tm, lr, Fade(col, 0.95f), false, 2.0f);
          PixelLozenge(dl, tm, lr - 4.0f, Fade(ColIronGall(), 0.35f), false, 2.0f);
          DrawSpriteCenteredAt(dl, GlyphIcon(kMod), ImVec2(tm.x + 1, tm.y + 1),
                               scale * 0.5f, Fade(ColVellumHi(), 0.7f));
          DrawSpriteCenteredAt(dl, GlyphIcon(kMod), tm, scale * 0.5f,
                               n.wasted ? Fade(ColIronSoft(), 0.6f) : ColIronGall());
        }
        if (n.wasted) SlashOut(dl, a, b, Fade(ColBloodHi(), 0.85f));
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##tag", ImVec2(std::max(8.0f, b.x - a.x),
                                               std::max(8.0f, b.y - a.y)));
        const bool hov = ImGui::IsItemHovered();
        if (hov) LeafHover(dl, a, b);
        MagnitudeNumeral(dl, a, b, n, scale);
        if (hov && !live) {
          WheelMagnitude(s, n, readOnly);
          BeginTip();
          ImGui::PushStyleColor(ImGuiCol_Text,
                                ImGui::ColorConvertU32ToFloat4(ColGoldHi()));
          ImGui::TextUnformatted(n.label.c_str());
          ImGui::PopStyleColor();
          if (!n.edit.empty()) ImGui::TextDisabled("%s", n.edit.c_str());
          if (n.graded) ImGui::TextDisabled("%s", MagnitudeLine(n).c_str());
          // WHICH RECORD, always — it is the one thing a bead's position shows
          // and its picture does not, and `shotgun` spoken inside a lane is
          // record-wide anyway, so the answer is not guessable from the words.
          // WHICH SCOPE, and for a count word, that a SPLIT is what it is:
          // a count on the trunk fans the box, a count on a branch splits that
          // branch, and one split per scope either way.
          const bool isSplit = n.edit.rfind("count", 0) == 0;
          if (n.instance >= 0)
            ImGui::TextDisabled(isSplit ? "splits branch %d alone%s"
                                        : "edits instance %d alone%s",
                                n.instance, n.instance == 0 ? " (the aim)" : "");
          else
            ImGui::TextDisabled(isSplit ? "splits the whole box into its branches"
                                        : "edits the whole record: every instance");
          if (n.wasted)
            ImGui::TextDisabled("WASTED: this record has no such field. "
                                "Charged, and it does nothing.");
          ImGui::TextDisabled("right-click to take it off");
          EndTip();
        }
        if (!readOnly && hov && !live && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          PushGrimoireUndo(s);
          s.graphEdit = {};
          s.graphEdit.pending = true;
          s.graphEdit.op = UIState::GraphEditIntent::Remove;
          s.graphEdit.treeNode = n.treeNode;
        }
        // A bead is in its box's pile like any word, so a drop on it lands
        // beside it - one less dead patch on the trunk under a drag.
        if (!readOnly && n.treeNode >= 0 && live) offer(Tgt::Beside, (int)i, a, b);
        break;
      }
      case kHole: {
        // THE BLANK an operator is waiting on. It is not a node of its own: it
        // carries the OPERATOR's tree node and `instance` is the side, so a drop
        // on it is `FillSlot(thatOperator, thatSide)` - the same op the hollow
        // pip used to latch, on a target 64 chrome pixels wide instead of 21.
        LeafBlank(dl, a, b, scale);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##hole", ImVec2(std::max(8.0f, b.x - a.x),
                                                std::max(8.0f, b.y - a.y)));
        const bool hov = ImGui::IsItemHovered();
        if (hov) LeafHover(dl, a, b);
        if (hov && !live && !n.page.empty()) {
          BeginTip();
          ImGui::PushStyleColor(ImGuiCol_Text,
                                ImGui::ColorConvertU32ToFloat4(ColGoldHi()));
          ImGui::Text("an input of [%s]", n.page.c_str());
          ImGui::PopStyleColor();
          ImGui::TextDisabled(n.instance == 0
                                  ? "`%s` inside the page left this slot empty, so the page "
                                    "takes the word SPOKEN BEFORE it here."
                                  : "`%s` inside the page left this slot empty, so the page "
                                    "takes the word spoken AFTER it here.",
                              n.glyphId.c_str());
          ImGui::TextDisabled("Empty, that part of the page is charged and does nothing.");
          ImGui::TextDisabled("Drop a word here to give it to the page.");
          EndTip();
        } else if (hov && !live) {
          BeginTip();
          ImGui::PushStyleColor(ImGuiCol_Text,
                                ImGui::ColorConvertU32ToFloat4(ColGoldHi()));
          ImGui::Text("_ %s", n.glyphId.c_str());
          ImGui::PopStyleColor();
          ImGui::TextDisabled(n.instance == 0
                                  ? "`%s` takes the word SPOKEN BEFORE it, and "
                                    "this is where that word stands."
                                  : "`%s` takes a second word after it, and this "
                                    "is where that word stands.",
                              n.glyphId.c_str());
          ImGui::TextDisabled("Empty, the pair is charged and does nothing.");
          ImGui::TextDisabled("Drop a word here to fuse it.");
          EndTip();
        }
        offer(n.instance == 0 ? Tgt::PipL : Tgt::PipR, (int)i, a, b);
        break;
      }
      case kOperator:
      case kWord:
      default: {
        const bool op = n.kind == kOperator;
        const float cw = b.x - a.x;
        const bool dimNode = op && !n.complete;
        const UIState::GlyphUI* gu = FindGlyph(s, n.glyphId);
        // A word is a roundel and an operator is a tablet: the shape says
        // which of the two it is before the colour or the engraving does.
        if (op)
          LeafTablet(dl, a, b, GlyphIcon(n.sort), scale, dimNode);
        else
          LeafRoundel(dl, a, b, gu ? gu->color : 0u, n.sort, GlyphIcon(n.sort),
                      scale, false);
        // HOW MANY, as tally strokes over the figure.
        if (scale >= 0.5f)
          CountPips(dl, a, b, n.n, Fade(SortPigment(n.sort), 0.95f));
        if (dimNode) {
          // An operator with an empty required slot is CHARGED and does
          // nothing - the passage is written but struck out. Hatched in the
          // corrector's red rather than dimmed, because a page has no
          // backlight to take away. What it costs is in the tip.
          dl->AddRectFilled(a, b, Fade(ColVellumLo(), 0.45f));
          SlashOut(dl, a, b, Fade(ColBlood(), 0.9f));
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##cell", ImVec2(std::max(8.0f, cw),
                                                std::max(8.0f, b.y - a.y)));
        const bool hov = ImGui::IsItemHovered();
        // THE NAME IS THE HOVER. A cell carries no caption, so the mark under
        // the cursor has to be unmistakable: the reader's bracket round the
        // whole figure, in minium.
        if (hov) LeafHover(dl, a, b);
        MagnitudeNumeral(dl, a, b, n, scale);
        TimingTag(dl, a, b, n, scale);
        if (!n.page.empty()) PageSeal(dl, a, b, n.page, scale);
        if (hov && !live && !n.page.empty()) {
          BeginTip();
          ImGui::PushStyleColor(ImGuiCol_Text,
                                ImGui::ColorConvertU32ToFloat4(ColGoldHi()));
          ImGui::Text("[%s]", n.page.c_str());
          ImGui::PopStyleColor();
          ImGui::TextDisabled("a whole written spell, used here as ONE glyph");
          ImGui::TextDisabled("stands as: %s%s", SortLabel(n.sort),
                              n.outputs > 1 ? "  (several things at once)" : "");
          if (n.inputs > 0)
            ImGui::TextDisabled("%d input%s: the slot%s in the row under it", n.inputs,
                                n.inputs == 1 ? "" : "s", n.inputs == 1 ? "" : "s");
          else
            ImGui::TextDisabled("no inputs: it is complete as written");
          if (!n.complete)
            ImGui::TextDisabled("an input is empty: that part is charged and does nothing");
          ImGui::TextDisabled("edit the page itself to change what it says");
          ImGui::TextDisabled("right-click: take it off");
          EndTip();
        } else if (hov && !live) {
          std::string ml = MagnitudeLine(n);
          if (!n.timingPhrase.empty()) ml += (ml.empty() ? "" : "\n") + std::string("fires ") + n.timingPhrase;
          if (gu) GlyphInfoBox(*gu, SortLabel(n.sort), ml.c_str());
          else Tip("a word that no longer exists");
          WheelMagnitude(s, n, readOnly);
        }
        if (n.page.empty()) TimingClick(n, hov, readOnly, live != nullptr);
        // THE CELL ITSELF TAKES A DROP (2026-09-21). It used to take none: a
        // word cell was a drag SOURCE and nothing else, and an operator's only
        // targets were its two hollow pips, so a glyph aimed anywhere at a
        // `transmute` but its ~21 px pip landed on the canvas and evaporated
        // with no marker and no sentence. Now the body of a cell means the
        // obvious thing:
        //   a DELIVERY  -> box this one word (Wrap), same as dropping one on a
        //                  bar, which is where the gesture already existed;
        //   anything else on an operator with an empty slot -> fill it, left
        //                  first, because the left slot is "the word before it"
        //                  and that is the one a player is usually aiming at.
        // The pips are submitted AFTER this and so still win where they
        // overlap, which is what keeps "that slot, specifically" sayable.
        //   anything else, anywhere else -> BESIDE this word, in its pile.
        if (!readOnly && n.treeNode >= 0 && live) {
          const bool deliveryDrag = DeliveryPayload(s, live);
          const bool fromGraph = live->IsDataType(kPayloadGraphNode);
          if (deliveryDrag) offer(Tgt::Body, (int)i, a, b);
          else if (!fromGraph && op &&
                   ((n.hasLeft && !n.leftFilled) || (n.hasRight && !n.rightFilled)))
            offer(Tgt::Cell, (int)i, a, b);
          else
            offer(Tgt::Beside, (int)i, a, b);
        }
        if (!readOnly && n.treeNode >= 0 && ImGui::BeginDragDropSource()) {
          int idx = (int)i;
          ImGui::SetDragDropPayload(kPayloadGraphNode, &idx, sizeof idx);
          ImGui::Dummy(ImVec2(44, 44));
          ImDrawList* pd = ImGui::GetWindowDrawList();
          const ImVec2 q = ImGui::GetItemRectMin();
          SlotSurface(pd, q, 44.0f, SlotLook::Filled, false);
          GlyphArt(pd, q, 44.0f, gu ? gu->color : 0u, n.sort);
          ImGui::SameLine(0, 8);
          ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 15);
          ImGui::TextUnformatted(n.label.c_str());
          ImGui::EndDragDropSource();
        }
        if (!readOnly && hov && !live && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          PushGrimoireUndo(s);
          s.graphEdit = {};
          s.graphEdit.pending = true;
          s.graphEdit.op = UIState::GraphEditIntent::Remove;
          s.graphEdit.treeNode = n.treeNode;
        }
        // The operator's PIPS, one per FILLED slot: the stud the operand's
        // stroke lands on, which is what says which side bound it. An EMPTY
        // slot has no pip here any more - it is a `Hole` cell standing in the
        // operand row (see LeafBlank), because a hollow ring hung off the
        // cell's edge was the smallest mark on the page carrying the biggest
        // fact on it.
        if (op) {
          const float ph = std::floor(cw * 0.34f);
          for (int side = 0; side < 2; side++) {
            const bool hasIt = side == 0 ? n.hasLeft : n.hasRight;
            const bool filled = side == 0 ? n.leftFilled : n.rightFilled;
            if (!hasIt || !filled) continue;
            const float cx = side == 0 ? a.x : b.x;
            const ImVec2 pa(std::floor(cx - ph * 0.5f),
                            std::floor((a.y + b.y) * 0.5f - ph * 0.5f));
            const ImVec2 pb(pa.x + ph, pa.y + ph);
            const ImVec2 sc2(std::floor((pa.x + pb.x) * 0.5f),
                             std::floor((pa.y + pb.y) * 0.5f));
            const float sr = std::max(3.0f, std::floor(ph * 0.5f));
            PixelDisc(dl, sc2, sr, ColIronGall());
            PixelRing(dl, sc2, sr + 2.0f, Fade(ColVellumHi(), 0.8f), 2.0f);
          }
        }
        break;
      }
    }
    ImGui::PopID();
  }

  if (!readOnly) TimingPopup(s);

  // ---- AHEAD OF THE SPELL: the empty slot a new delivery goes in -------------
  //
  // From the owner's report: "to add a projectile I have to drag it into an
  // empty slot that occurs BEHIND it". True, and it was the only way. The
  // drawing grows UPWARD - the hand is the pedestal and the last word spoken is
  // the top cell - so the place a new outer delivery belongs is off the top of
  // the page, and the only target that did it was the hand's own bar at the
  // FOOT of the drawing, which is the opposite end from where the word lands.
  // (Dropping on the top cell works too, and means the same thing, but it reads
  // as "into that word" rather than "after it".)
  //
  // So: while a DELIVERY is in flight, one empty slot is drawn one band above
  // the top of the spell, on the box axis, and a drop in it boxes the whole
  // sentence. It exists only during that drag - an always-drawn empty socket
  // over every spell is a node the page does not have - and it is UI-only: no
  // graph node, no layout law, nothing for the gate to hold.
  if (!readOnly && live && DeliveryPayload(s, live)) {
    int rootIdx = -1, topIdx = -1;
    for (size_t i = 0; i < g.nodes.size(); i++) {
      const UIState::SpellGraphUI::Node& n = g.nodes[i];
      if (n.kind == kRoot) rootIdx = (int)i;
      if (n.h >= 64 && (topIdx < 0 || n.layer > g.nodes[(size_t)topIdx].layer))
        topIdx = (int)i;
    }
    if (rootIdx >= 0) {
      const UIState::SpellGraphUI::Node& rn = g.nodes[(size_t)rootIdx];
      // The axis is the hand's, because that is the axis the whole sentence
      // stands on; the band is one cell above whatever is highest.
      const float cx = X(rn.x + rn.w / 2);
      const float half = std::floor(32.0f * scale);
      const float h = half * 2.0f;
      const size_t topL = (size_t)std::clamp(
          topIdx >= 0 ? g.nodes[(size_t)topIdx].layer : 0, 0, g.layers - 1);
      const float top = org.y + layerTop[topL] - gapFor(scale) - h;
      const ImVec2 a(cx - half, top), b(cx + half, top + h);
      // An EMPTY socket in the page's own hand: a pricked ring with nothing in
      // it, and a short stroke down to the spell it would box.
      InkStroke(dl, ImVec2(cx, b.y), ImVec2(cx, b.y + gapFor(scale)),
                Fade(ColIronSoft(), 0.55f), scale < 1.0f ? 2.0f : 3.0f);
      PixelRing(dl, ImVec2(cx, std::floor((a.y + b.y) * 0.5f)), half - 2.0f,
                Fade(ColRubric(), 0.75f), 2.0f);
      DrawSpriteCenteredAt(dl, GlyphIcon(kDelivery),
                           ImVec2(cx, std::floor((a.y + b.y) * 0.5f)),
                           scale * 0.5f, Fade(ColIronSoft(), 0.45f));
      ImGui::SetCursorScreenPos(a);
      ImGui::PushID(8999);
      ImGui::InvisibleButton("##ahead", ImVec2(h, h));
      offer(Tgt::Ahead, rootIdx, a, b);
      ImGui::PopID();
    }
  }

  // ---- THE CANDIDATE LAYER: every place the held thing can go -----------------
  //
  // (2026-10-01, asked for.) A drop target used to be invisible until the
  // cursor was already on it, so placing a word was a hunt: sweep the drawing
  // and wait for a ring to appear. Now the moment a drag is in flight, EVERY
  // target `judge` would accept is marked at once - a faint minium wash and
  // four pulsing corner ticks - and the one under the cursor keeps its full
  // marker below. Targets that would refuse are left alone: the page shows
  // where you CAN go, and only the spot you are actually on explains a "no".
  // When nothing on the page can take the payload, the reason the hand (the
  // one target every page has) gives is put on the status line instead, so a
  // drag with nowhere to land still says why.
  int candidates = 0;
  const char* noWhere = nullptr;
  if (live && !readOnly) {
    const float pulse =
        0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 5.0f);
    for (const Target& t : targets) {
      const Verdict v = judge(t.tgt, t.node, live);
      if (!v.ok) {
        if (!noWhere && v.say && g.nodes[(size_t)t.node].kind == kRoot) noWhere = v.say;
        continue;
      }
      candidates++;
      if (hasAccept && t.a.x == acceptA.x && t.a.y == acceptA.y &&
          t.b.x == acceptB.x && t.b.y == acceptB.y)
        continue;   // the hovered one gets the full marker below
      if (t.tgt == Tgt::Socket) {
        // A SOCKET'S TARGET IS ITS WHOLE COLUMN, and the columns tile, so
        // marking each one's rect drew the socket row as one long smudge. The
        // pip is what the eye is looking for: ring it.
        const ImVec2 c(std::floor((t.a.x + t.b.x) * 0.5f), std::floor((t.a.y + t.b.y) * 0.5f));
        const float r = std::max(7.0f, std::floor((t.b.y - t.a.y) * 0.5f) - 2.0f);
        PixelDisc(dl, c, r, Fade(ColRubricHi(), 0.10f + 0.08f * pulse));
        PixelRing(dl, c, r, Fade(ColRubric(), 0.55f + 0.4f * pulse), 2.0f);
        continue;
      }
      const ImVec2 a(std::floor(t.a.x) - 2, std::floor(t.a.y) - 2);
      const ImVec2 b(std::floor(t.b.x) + 2, std::floor(t.b.y) + 2);
      dl->AddRectFilled(a, b, Fade(ColRubricHi(), 0.08f + 0.06f * pulse));
      const ImU32 c = Fade(ColRubric(), 0.55f + 0.4f * pulse);
      const float tl = std::max(4.0f, std::min(10.0f, std::floor((b.x - a.x) * 0.25f)));
      for (int k = 0; k < 4; k++) {
        const float x = (k & 1) ? b.x - tl : a.x;
        const float y = (k & 2) ? b.y - 2 : a.y;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + tl, y + 2), c);
        const float vx = (k & 1) ? b.x - 2 : a.x;
        const float vy = (k & 2) ? b.y - tl : a.y;
        dl->AddRectFilled(ImVec2(vx, vy), ImVec2(vx + 2, vy + tl), c);
      }
    }
    // Off every target: say what the marks mean, or why there are none. Only
    // while the cursor is over the sheet - the canvas speaks first on the
    // status line, and over the word row it is the row's turn.
    const ImVec2 m = ImGui::GetIO().MousePos;
    const bool overSheet = m.x >= viewMin.x && m.x < viewMax.x && m.y >= viewMin.y &&
                           m.y < viewMax.y;
    if (overSheet && !hasAccept && !hasRefuse) {
      if (candidates > 0) {
        sNote = "drop it on any marked place";
      } else if (noWhere) {
        sNote = noWhere;
        res.refused = true;
      }
    }
  }

  // ---- the marker layer, over everything -------------------------------------
  // Where a drop will land, said in the page's own hand: a wash of minium over
  // the cell that will take it and a pricked ring round it, or - for a refusal
  // - the cross-hatch a corrector strikes a bad passage out with.
  if (hasGhost && !hasRefuse)
    dl->AddRectFilled(ghostA, ghostB, Fade(ColRubricHi(), 0.22f));
  if (hasAccept && !hasRefuse) {
    for (int i = 0; i < 3; i++) {
      const float o = (float)(i * 3);
      const ImU32 c = Fade(ColRubric(), 0.95f - (float)i * 0.28f);
      DottedRule(dl, ImVec2(acceptA.x - o, acceptA.y - o),
                 ImVec2(acceptB.x + o, acceptA.y - o), c, 6.0f, 2.0f);
      DottedRule(dl, ImVec2(acceptA.x - o, acceptB.y + o),
                 ImVec2(acceptB.x + o, acceptB.y + o), c, 6.0f, 2.0f);
      DottedRule(dl, ImVec2(acceptA.x - o, acceptA.y - o),
                 ImVec2(acceptA.x - o, acceptB.y + o), c, 6.0f, 2.0f);
      DottedRule(dl, ImVec2(acceptB.x + o, acceptA.y - o),
                 ImVec2(acceptB.x + o, acceptB.y + o), c, 6.0f, 2.0f);
    }
  }
  if (hasRefuse) {
    dl->AddRectFilled(refuseA, refuseB, Fade(ColBlood(), 0.28f));
    // Struck out, the way a scribe strikes out: hatching across the passage.
    const float w = refuseB.x - refuseA.x, h = refuseB.y - refuseA.y;
    for (float o = -h; o < w; o += 8.0f) {
      const float x0 = std::max(refuseA.x, refuseA.x + o);
      const float y0 = refuseA.y + std::max(0.0f, -o);
      const float len = std::min(w - (x0 - refuseA.x), h - (y0 - refuseA.y));
      if (len <= 0) continue;
      DottedRule(dl, ImVec2(x0, y0), ImVec2(x0 + len, y0 + len),
                 Fade(ColBloodHi(), 0.85f), 3.0f, 2.0f);
    }
    Ring(dl, refuseA, refuseB, ColBlood(), 2);
  }

  // ---- WHAT THE BAND CANNOT SHOW ---------------------------------------------
  //
  // A drawing taller or wider than the sheet's window used to be CUT: the top
  // cell and the pedestal both sliced through the middle by the child's clip,
  // which reads as a rendering bug and not as "there is more page here". Now
  // the sheet darkens toward the edge it continues past, and a pricked chevron
  // points that way. Both are drawn only on the sides that overflow, so a
  // drawing that fits has no furniture round it at all.
  {
    const float top = org.y, bot = org.y + gh;
    const float lef = org.x, rig = org.x + gw;
    // Three nested chevrons pointing `dir` (-1 up, +1 down) at `tip`.
    auto chevron = [&](ImVec2 tip, float dir) {
      for (int i = 0; i < 3; i++) {
        const float o = (float)i * 5.0f;
        const ImU32 c = Fade(ColVellumHi(), 0.85f - (float)i * 0.22f);
        DottedRule(dl, ImVec2(tip.x - 12, tip.y + dir * (8 + o)),
                   ImVec2(tip.x, tip.y + dir * o), c, 4.0f, 2.0f);
        DottedRule(dl, ImVec2(tip.x, tip.y + dir * o),
                   ImVec2(tip.x + 12, tip.y + dir * (8 + o)), c, 4.0f, 2.0f);
      }
    };
    if (top < viewMin.y - 2) {
      GradientV(dl, viewMin, ImVec2(viewMax.x, viewMin.y + 28),
                Fade(ColIronGall(), 0.42f), Fade(ColIronGall(), 0.0f));
      chevron(ImVec2(viewMax.x - 30, viewMin.y + 6), 1.0f);
    }
    if (bot > viewMax.y + 2) {
      GradientV(dl, ImVec2(viewMin.x, viewMax.y - 28), viewMax,
                Fade(ColIronGall(), 0.0f), Fade(ColIronGall(), 0.42f));
      chevron(ImVec2(viewMax.x - 30, viewMax.y - 8), -1.0f);
    }
    if (lef < viewMin.x - 2)
      GradientH(dl, viewMin, ImVec2(viewMin.x + 24, viewMax.y),
                Fade(ColIronGall(), 0.38f), Fade(ColIronGall(), 0.0f));
    if (rig > viewMax.x + 2)
      GradientH(dl, ImVec2(viewMax.x - 24, viewMin.y), viewMax,
                Fade(ColIronGall(), 0.0f), Fade(ColIronGall(), 0.38f));
  }

  // The binding's own shadow round the opening, over everything on the sheet.
  LeafFrame(dl, viewMin, viewMax);

  // ---- THE VIEW'S OWN READOUT, bottom-right, only when it has been driven ----
  //
  // A view you can move needs to say that it has been moved, or the first
  // stray wheel click over the sheet reads as the tree having changed size on
  // its own. It also says the way back. Nothing is drawn while the view is
  // still on the fit, which is the state every page opens in.
  if (s.spellGraphZoom > 0.0f || s.spellGraphPanX != 0.0f || s.spellGraphPanY != 0.0f) {
    char vz[48];
    std::snprintf(vz, sizeof vz, "%gx  -  double-click to fit", (double)scale);
    const ImVec2 ts = ImGui::CalcTextSize(vz);
    const ImVec2 ta(std::floor(viewMax.x - ts.x - 10), std::floor(viewMax.y - 20));
    // Written in the margin, in the hand a scribe annotates with: no plate
    // behind it, because a solid panel over the sheet is the one thing on
    // this page that could not be on a page.
    dl->AddRectFilled(ImVec2(ta.x - 6, ta.y - 3), ImVec2(viewMax.x - 2, ta.y + 16),
                      Fade(ColVellum(), 0.55f));
    dl->AddText(FontSmall(), 13.0f, ta, Fade(ColIronSoft(), 0.9f), vz);
  }

  dl->PopClipRect();
  ImGui::PopFont();
  ImGui::EndChild();
  if (!sNote.empty()) res.note = sNote.c_str();
  return res;
}

}  // namespace ui
