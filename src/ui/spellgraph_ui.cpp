#include "ui/spellgraph_ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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
// THE SCALE IS 1x OR 0.5x AND NOTHING ELSE. A smooth zoom would put chrome on
// half pixels, which is the one thing the pixel-art rule forbids; a graph that
// does not fit at 0.5x SCROLLS.
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
// scrolls, and only then does the view frame the deepest join.

namespace ui {

namespace {

// Graph node kinds, mirrored from game/spellgraph.h's GraphKind. The mirror
// carries the enum as an int because ui/overlay.h is spell-free.
enum Kind { kWord = 0, kOperator, kJoin, kModTag, kRoot, kSocket, kBus };
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

const UIState::GlyphUI* FindGlyph(const UIState& s, const std::string& id) {
  for (const UIState::GlyphUI& g : s.glyphsOwned)
    if (g.id == id) return &g;
  return nullptr;
}

// What sort a payload denotes: a glyph's own sort, or -1 for a page (a page is
// a noun as far as the tree is concerned — it expands to words).
int PayloadSort(const UIState& s, bool page, const char* name) {
  if (page) return -1;
  const UIState::GlyphUI* g = FindGlyph(s, name ? name : "");
  return g ? g->type : -1;
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
  if (n <= 5) {
    const float span = (float)n * 8.0f - 4.0f;
    for (int i = 0; i < n; i++) {
      const float x = std::floor(mid - span * 0.5f + (float)i * 8.0f);
      dl->AddRectFilled(ImVec2(x, a.y - 6), ImVec2(x + 4, a.y - 2), col);
    }
    return;
  }
  dl->AddRectFilled(ImVec2(mid - 18, a.y - 6), ImVec2(mid + 18, a.y - 2), col);
  dl->AddRectFilled(ImVec2(mid - 18, a.y - 10), ImVec2(mid - 14, a.y - 6), col);
  dl->AddRectFilled(ImVec2(mid + 14, a.y - 10), ImVec2(mid + 18, a.y - 6), col);
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

void GlyphInfoBox(const UIState::GlyphUI& g, const char* sortName) {
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

// ---- the canvas ---------------------------------------------------------------

GraphCanvasResult SpellGraphCanvas(UIState& s, ImVec2 at, ImVec2 size, bool readOnly) {
  GraphCanvasResult res;
  const UIState::SpellGraphUI& g = s.spellGraph;

  ImGui::SetCursorScreenPos(at);
  ImGui::BeginChild("##spellcanvas", size, ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground |
                        ImGuiWindowFlags_HorizontalScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 viewMin = ImGui::GetWindowPos();
  const ImVec2 viewMax(viewMin.x + size.x, viewMin.y + size.y);

  // The recess the tree sits in. Drawn in WINDOW coordinates rather than at the
  // scrolled cursor, so it stays put while the drawing slides under it.
  {
    dl->AddRectFilled(viewMin, viewMax, Fade(ColInk(), 0.5f));
    InnerShadow(dl, viewMin, viewMax, 8.0f, 0.45f);
    Grain(dl, viewMin, viewMax, 0.05f);
    dl->AddRectFilled(viewMin, ImVec2(viewMin.x + 2, viewMax.y), Fade(ColGoldDim(), 0.7f));
  }

  if (g.nodes.empty()) {
    // CENTRED in the recess, wrapped to it and clipped to it: the empty state
    // is the whole picture when there is nothing to draw, and a hint hard
    // against the top-left corner of a big dark box reads as a stray label.
    const char* hint = "an empty page - drag a word here";
    ImGui::PushFont(FontSmall());
    const ImVec2 ts = ImGui::CalcTextSize(hint);
    ImGui::PopFont();
    const ImVec2 c(std::floor((viewMin.x + viewMax.x) * 0.5f),
                   std::floor((viewMin.y + viewMax.y) * 0.5f));
    dl->PushClipRect(viewMin, viewMax, true);
    // A dotted frame the size of a cell, and the sentence under it: the shape
    // of the thing that is missing, where it would go.
    for (int i = 0; i < 8; i++) {
      const float t = (float)i * 8.0f;
      dl->AddRectFilled(ImVec2(c.x - 32 + t, c.y - 46), ImVec2(c.x - 28 + t, c.y - 44),
                        Fade(ColBronze(), 0.8f));
      dl->AddRectFilled(ImVec2(c.x - 32 + t, c.y + 14), ImVec2(c.x - 28 + t, c.y + 16),
                        Fade(ColBronze(), 0.8f));
      dl->AddRectFilled(ImVec2(c.x - 32, c.y - 46 + t), ImVec2(c.x - 30, c.y - 42 + t),
                        Fade(ColBronze(), 0.8f));
      dl->AddRectFilled(ImVec2(c.x + 30, c.y - 46 + t), ImVec2(c.x + 32, c.y - 42 + t),
                        Fade(ColBronze(), 0.8f));
    }
    DrawSpriteCentered(dl, "glyph_ring", ImVec2(c.x, c.y - 15),
                       Fade(ColBronze(), 0.9f));
    dl->AddText(FontSmall(), 13.0f,
                ImVec2(std::floor(c.x - ts.x * 0.5f), c.y + 26),
                Fade(ColParchDim(), 0.8f), hint);
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
  for (const UIState::SpellGraphUI::Node& n : g.nodes)
    if (n.layer >= 0 && n.layer < g.layers)
      layerH[(size_t)n.layer] = std::max(layerH[(size_t)n.layer], (float)n.h);
  auto gapFor = [](float sc) {
    return std::max(kBandAir, std::floor((float)kPitchPx * sc * 0.28f * 0.5f) * 2.0f);
  };
  auto heightFor = [&](float sc) {
    float h = (float)(g.layers - 1) * gapFor(sc);
    for (float lh : layerH) h += std::floor(lh * sc);
    return h;
  };
  // FIT FIRST, THEN SCROLL. 1x if the whole drawing stands in the band, else
  // 0.5x, else 0.5x with the scrollbars the child already has. Never anything
  // between: a fractional scale puts a 2x sprite on half pixels, which is the
  // one thing the pixel-art rule forbids. The margins subtracted here are the
  // drawing's own padding plus a scrollbar's width, so "it fits" means it fits
  // with nothing clipped rather than fits-until-a-bar-appears.
  // The margin the drawing needs on every side: a mod tag hangs off a bar's
  // LEFT end and the layout already counts it in `width`, but a node's hover
  // glow, its count pips and an accept ring all bleed a few pixels past the
  // box, and a figure touching its frame reads as a figure that has been cut.
  constexpr float kMargin = 10.0f;
  const float sbar = ImGui::GetStyle().ScrollbarSize;
  const float avail = size.x - kMargin * 2 - sbar;
  const float availY = size.y - kMargin * 2;
  float scale = 1.0f;
  if ((float)g.width > avail || heightFor(1.0f) > availY) scale = 0.5f;
  const float gap = gapFor(scale);
  const float gw = std::floor((float)g.width * scale);
  const float gh = heightFor(scale);
  // Where each layer's TOP sits inside the drawing, walking down from the
  // topmost layer (the highest index; the root is layer 0 at the bottom).
  std::vector<float> layerTop((size_t)std::max(1, g.layers), 0.0f);
  {
    float t = 0.0f;
    for (int L = g.layers - 1; L >= 0; L--) {
      layerTop[(size_t)L] = t;
      t += std::floor(layerH[(size_t)L] * scale) + gap;
    }
  }

  // CENTRED WHEN IT FITS, SCROLLED WHEN IT DOES NOT. The pad is the slack the
  // drawing does not use, halved — so a two-cell spell stands in the middle of
  // the band and a tree wider than the band still starts at the margin. The
  // Dummy that follows is sized to pad + drawing + pad, which is what gives the
  // child a scroll range that can actually reach the far edge.
  const bool fits = gw + kMargin * 2 <= size.x - sbar && gh + kMargin * 2 <= size.y;
  const float padX = std::max(kMargin, std::floor((size.x - sbar - gw) * 0.5f));
  const float padY = std::max(kMargin, std::floor((size.y - gh) * 0.5f));
  const ImVec2 base = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(gw + padX * 2, gh + padY * 2));
  // WHEN IT FITS, THE VIEW IS PINNED. The pad above already centres the
  // drawing, so any scroll at all is a drawing pushed off its own frame — and a
  // scroll offset SURVIVES the composer changing height under it, which is how
  // a tree that fits ended up with its top row cut off by the band's top edge.
  if (fits) {
    ImGui::SetScrollX(0.0f);
    ImGui::SetScrollY(0.0f);
  }
  // FRAME THE DEEPEST JOIN, once, when the drawing changes shape — and ONLY
  // when it does not fit. A tree too tall for the band used to open hard
  // against its top-left corner, which put the hand off the bottom and the
  // innermost delivery — the bar carrying the sockets and the bus, which is
  // what you are actually editing — out of sight. Set ONCE rather than every
  // frame, or the wheel would fight it.
  {
    static int lastW = -1, lastH = -1;
    if (!fits && (g.width != lastW || g.height != lastH)) {
      lastW = g.width;
      lastH = g.height;
      float wantY = 0.0f;
      {
        int deep = -1, deepLayer = -1;
        for (size_t di = 0; di < g.nodes.size(); di++)
          if (g.nodes[di].kind == kJoin && g.nodes[di].layer > deepLayer) {
            deepLayer = g.nodes[di].layer;
            deep = (int)di;
          }
        if (deep >= 0) {
          const UIState::SpellGraphUI::Node& dn = g.nodes[(size_t)deep];
          wantY = (padY + layerTop[(size_t)dn.layer] + (float)dn.h * scale +
                   kMargin) - size.y;
        }
      }
      ImGui::SetScrollX(std::max(0.0f, std::floor((gw + padX * 2 - size.x) * 0.5f)));
      ImGui::SetScrollY(std::max(0.0f, std::floor(wantY)));
    }
  }
  const ImVec2 org(std::floor(base.x + padX), std::floor(base.y + padY));
  // X is the layout's, scaled. Y is the node's LAYER at the screen pitch, plus
  // an offset INSIDE the node (which is scaled): a node's own geometry is the
  // layout's, its band is the canvas's.
  auto X = [&](int gx) { return std::floor(org.x + (float)gx * scale); };
  auto Y = [&](const UIState::SpellGraphUI::Node& n, int off) {
    const size_t L = (size_t)std::clamp(n.layer, 0, g.layers - 1);
    return std::floor(org.y + layerTop[L] + (float)off * scale);
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

  ImGui::PushFont(FontSmall());

  // ---- what is in flight ----------------------------------------------------
  const ImGuiPayload* live = ImGui::GetDragDropPayload();
  const bool copyMod = ImGui::GetIO().KeyCtrl;
  const bool full = (int)s.grimoireEditWords.size() >= s.grimoireMaxWords;
  // Set by whichever target is under the cursor; drawn after every node, so a
  // marker on a cell's edge is never buried by the next cell's recess.
  ImVec2 acceptA, acceptB, refuseA, refuseB, ghostA, ghostB;
  bool hasAccept = false, hasRefuse = false, hasGhost = false;
  static std::string sNote;
  sNote.clear();

  // THE ONE DROP ROUTINE. Every target on the canvas calls it: it peeks at the
  // payload, decides which tree op the gesture means, describes it to the
  // marker layer, and only latches the intent on the actual release. The UI
  // never applies an op — main.cpp does, from game/spellgraph.h, and answers a
  // refusal on the status line.
  enum class Tgt { Bar, Bus, Socket, Body, PipL, PipR };
  auto offer = [&](Tgt tgt, int nodeIdx, ImVec2 a, ImVec2 b) {
    if (!ImGui::BeginDragDropTarget()) return;
    const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadGraphNode, kPeekFlags);
    const bool fromGraph = p != nullptr;
    bool page = false;
    if (!p) p = ImGui::AcceptDragDropPayload(kPayloadGlyph, kPeekFlags);
    if (!p) {
      p = ImGui::AcceptDragDropPayload(kPayloadPage, kPeekFlags);
      page = p != nullptr;
    }
    if (!p) {
      ImGui::EndDragDropTarget();
      return;
    }
    const bool deliver = p->IsDelivery();
    const UIState::SpellGraphUI::Node& n = g.nodes[(size_t)nodeIdx];
    auto refuse = [&](const char* why) {
      refuseA = a;
      refuseB = b;
      hasRefuse = true;
      sNote = why;
      res.refused = true;
    };
    auto accept = [&](const char* what) {
      acceptA = a;
      acceptB = b;
      hasAccept = true;
      if (sNote.empty()) sNote = what;
    };
    if (readOnly) {
      refuse("an authored page cannot be changed - copy it to make one of your own");
      ImGui::EndDragDropTarget();
      return;
    }

    const char* name = fromGraph ? nullptr : (const char*)p->Data;
    const int srcGraph = fromGraph ? *(const int*)p->Data : -1;
    const int sort = fromGraph ? -2 : PayloadSort(s, page, name);
    if (sort == kSeparator) {
      refuse("`lane` and `end` are structure, not items - drop onto a socket "
             "instead");
      ImGui::EndDragDropTarget();
      return;
    }
    if (!fromGraph && full) {
      refuse("this page is full - remove a word to make room");
      ImGui::EndDragDropTarget();
      return;
    }

    UIState::GraphEditIntent e;
    const char* say = nullptr;
    switch (tgt) {
      case Tgt::PipL:
      case Tgt::PipR: {
        if (fromGraph) {
          refuse("a slot takes a word from the arsenal, not a branch");
          ImGui::EndDragDropTarget();
          return;
        }
        e.op = UIState::GraphEditIntent::FillSlot;
        e.treeNode = n.treeNode;
        e.side = tgt == Tgt::PipL ? 0 : 1;
        e.glyphId = name;
        say = tgt == Tgt::PipL ? "fill this operator's left slot"
                               : "fill this operator's right slot";
        break;
      }
      case Tgt::Body: {
        if (fromGraph) {
          refuse("drop a branch on a socket or a bus, not on another word");
          ImGui::EndDragDropTarget();
          return;
        }
        if (sort != kDelivery) {
          refuse("only a delivery boxes a branch - drop a word on the bus");
          ImGui::EndDragDropTarget();
          return;
        }
        e.op = UIState::GraphEditIntent::Wrap;
        e.treeNode = n.treeNode;
        e.glyphId = name;
        say = "box this branch in a delivery of its own";
        break;
      }
      case Tgt::Bar:
      case Tgt::Bus:
      case Tgt::Socket: {
        // Which BOX, and which LANE of it. A bar and a bus are the box's shared
        // segment (lane 0); a socket is instance i's own lane, and a socket
        // with no lane yet OPENS the next one (spellgraph.h: lane ==
        // laneCount + 1 opens, anything past that is refused).
        int boxIdx = nodeIdx;
        int lane = 0;
        if (tgt != Tgt::Bar) {
          // A bus and a socket are synthesized: their box is the bar that owns
          // them. Found by search rather than stored, because the mirror is a
          // flat list and the search is over a handful of nodes.
          boxIdx = -1;
          for (size_t bi = 0; bi < g.nodes.size(); bi++) {
            const UIState::SpellGraphUI::Node& bn = g.nodes[bi];
            if (bn.kind != kJoin && bn.kind != kRoot) continue;
            if (bn.bus == nodeIdx) { boxIdx = (int)bi; break; }
            for (int sk : bn.sockets)
              if (sk == nodeIdx) { boxIdx = (int)bi; break; }
            if (boxIdx >= 0) break;
          }
          if (boxIdx < 0) {
            refuse("that socket has no box");
            ImGui::EndDragDropTarget();
            return;
          }
          if (tgt == Tgt::Socket) {
            const UIState::SpellGraphUI::Node& bn = g.nodes[(size_t)boxIdx];
            lane = n.lane > 0 ? n.lane : n.instance + 1;
            if (lane > bn.laneCount + 1) {
              refuse("give the sockets before this one a payload first");
              ImGui::EndDragDropTarget();
              return;
            }
          }
        }
        const UIState::SpellGraphUI::Node& bn = g.nodes[(size_t)boxIdx];
        if (fromGraph) {
          if (srcGraph < 0 || srcGraph >= (int)g.nodes.size() ||
              g.nodes[(size_t)srcGraph].treeNode < 0) {
            ImGui::EndDragDropTarget();
            return;
          }
          e.op = UIState::GraphEditIntent::Move;
          e.treeNode = g.nodes[(size_t)srcGraph].treeNode;
          e.boxTreeNode = bn.treeNode;
          e.lane = lane;
          e.copy = copyMod;
          say = copyMod ? "ctrl: a copy of that branch lands here"
                        : "move that branch here";
        } else if (sort == kMod) {
          e.op = UIState::GraphEditIntent::AttachMod;
          e.treeNode = bn.treeNode;
          e.lane = lane;
          e.glyphId = name;
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
        break;
      }
    }
    accept(say);
    // The ghost: where the node will land. A socket's whole column, a pip's
    // ring, a bar's own rect - the marker layer draws the same shape as the
    // thing that is coming.
    ghostA = a;
    ghostB = b;
    hasGhost = true;
    if (deliver) {
      PushGrimoireUndo(s);
      e.pending = true;
      s.graphEdit = e;
    }
    ImGui::EndDragDropTarget();
  };

  // ---- the strokes, under every node ----------------------------------------
  //
  // Drawn FIRST so a stroke never crosses a cell, and with anti-aliasing off
  // (InkStroke owns that) so the curve is made of whole pixels.
  for (const UIState::SpellGraphUI::Edge& e : g.edges) {
    if (e.from < 0 || e.to < 0 || e.from >= (int)g.nodes.size() ||
        e.to >= (int)g.nodes.size())
      continue;
    const UIState::SpellGraphUI::Node& a = g.nodes[(size_t)e.from];
    const UIState::SpellGraphUI::Node& b = g.nodes[(size_t)e.to];
    const bool dim = !a.complete || a.wasted;
    ImU32 col = ColGold();
    float weight = 6.0f;
    switch (e.kind) {
      case kEBus:
      case kESocket:
        col = SortColour(a.sort);
        weight = 5.0f;
        break;
      case kSlot:
        col = SortColour(a.sort);
        weight = 4.0f;
        break;
      case kFan:
        col = ColGoldDim();
        weight = 5.0f;
        break;
      default:
        break;
    }
    if (dim) col = Fade(ColBronze(), 0.7f);
    ImVec2 from = BotC(a), to = TopC(b);
    if (e.kind == kEBus) {
      // A lane meets the BUS where it stands over it, not at the bus's centre:
      // the bus is one long stroke and the diamond is the junction.
      const int bx = std::min(std::max(a.x + a.w / 2, b.x), b.x + b.w);
      to = ImVec2(X(bx), Y(b, b.h / 2));
    } else if (e.kind == kSlot) {
      // Into the PIP the operand fills, so which side bound is visible.
      const bool leftSide = (a.x + a.w / 2) < (b.x + b.w / 2);
      to = ImVec2(X(leftSide ? b.x : b.x + b.w), Y(b, b.h / 2));
    }
    InkStroke(dl, from, to, col, weight * (scale < 1.0f ? 0.6f : 1.0f));
    if (e.kind == kEBus) {
      // The 4 px diamond at the junction.
      const float d = 4.0f * (scale < 1.0f ? 1.0f : 1.5f);
      dl->AddQuadFilled(ImVec2(to.x, to.y - d), ImVec2(to.x + d, to.y),
                        ImVec2(to.x, to.y + d), ImVec2(to.x - d, to.y), col);
    }
  }

  // ---- the nodes -------------------------------------------------------------
  for (size_t i = 0; i < g.nodes.size(); i++) {
    const UIState::SpellGraphUI::Node& n = g.nodes[i];
    const ImVec2 a = NodeMin(n), b = NodeMax(n);
    ImGui::PushID(9000 + (int)i);
    switch (n.kind) {
      case kBus: {
        // A straight stroke spanning every socket. Drawn here rather than as an
        // edge because it is a NODE with a width, and its ends are what tell you
        // how far the shared payload reaches.
        const ImVec2 m0(X(n.x), Y(n, n.h / 2)), m1(X(n.x + n.w), Y(n, n.h / 2));
        const float t = scale < 1.0f ? 2.0f : 4.0f;
        dl->AddRectFilled(ImVec2(m0.x, m0.y - t * 0.5f),
                          ImVec2(m1.x, m0.y + t * 0.5f), ColGold());
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
        const ImU32 col = n.instance == 0 ? ColGoldPale() : ColGold();
        if (own) {
          dl->AddRectFilled(pa, pb, Fade(col, 0.85f));
          dl->AddRect(pa, pb, ColInk(), 0.0f, 0, 2.0f);
        } else {
          dl->AddRectFilled(pa, pb, Fade(ColDeep(), 0.9f));
          dl->AddRect(pa, pb, Fade(col, 0.8f), 0.0f, 0, 2.0f);
        }
        ImGui::SetCursorScreenPos(pa);
        ImGui::InvisibleButton("##sock", ImVec2(std::max(8.0f, pb.x - pa.x),
                                                std::max(8.0f, pb.y - pa.y)));
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
          EndTip();
        }
        offer(Tgt::Socket, (int)i, pa, pb);
        break;
      }
      case kRoot:
      case kJoin: {
        const bool root = n.kind == kRoot;
        // The bar: a lit slab with a gold underline and the delivery's own
        // SYMBOL struck through the middle of it — the arrow for a form, the
        // ring for the hand. It used to carry the noun in tracked caps
        // ("PROJECTILE") and a price line under it; a bar is 128 px at its
        // narrowest and neither ever fit.
        PanelStyle st;
        st.darkMix = root ? 0.42f : 0.62f;
        st.shadow = 0.0f;
        st.grainAlpha = 0.05f;
        PanelBody(dl, a, b, st);
        dl->AddRectFilled(ImVec2(a.x, b.y - 2), ImVec2(b.x, b.y),
                          Fade(root ? ColGoldPale() : ColGold(), 0.9f));
        dl->AddRect(a, b, Fade(ColBronze(), 0.9f), 0.0f, 0, 2.0f);
        const ImVec2 mid(std::floor((a.x + b.x) * 0.5f),
                         std::floor((a.y + b.y) * 0.5f));
        // The mark is inlaid rather than stamped: a dark rebate behind it, the
        // engraving in its sort's colour, so a bar reads as metal with a sigil
        // cut into it and not as a label pasted on.
        const float mr = std::floor(16.0f * (scale < 1.0f ? 0.75f : 1.0f));
        dl->AddRectFilled(ImVec2(mid.x - mr, mid.y - mr), ImVec2(mid.x + mr, mid.y + mr),
                          Fade(ColInk(), 0.55f));
        // The delivery's OWN colour when its JSON entry gives it one, so a
        // projectile and a beam are not the same arrow in the same blue; the
        // sort's blue otherwise.
        const ImU32 markCol =
            root ? ColGoldPale()
                 : n.color ? IM_COL32(n.color & 0xFF, (n.color >> 8) & 0xFF,
                                      (n.color >> 16) & 0xFF, 255)
                           : SortColour(kDelivery);
        DrawSpriteCentered(dl, root ? "glyph_ring" : GlyphIcon(kDelivery),
                           ImVec2(mid.x + 1, mid.y + 1), Fade(ColInk(), 0.8f));
        DrawSpriteCentered(dl, root ? "glyph_ring" : GlyphIcon(kDelivery), mid,
                           markCol);
        // Two gold rules running out of the mark to the bar's ends: the bar is
        // a SPAN, and the eye needs to see how far this delivery reaches.
        dl->AddRectFilled(ImVec2(a.x + 6, mid.y - 1), ImVec2(mid.x - mr - 4, mid.y + 1),
                          Fade(markCol, 0.45f));
        dl->AddRectFilled(ImVec2(mid.x + mr + 4, mid.y - 1), ImVec2(b.x - 6, mid.y + 1),
                          Fade(markCol, 0.45f));
        // How MANY of it, as pips along the top edge rather than as "x3".
        CountPips(dl, a, b, n.n, Fade(ColGoldHi(), 0.9f));
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
        if (hov) Glow(dl, a, b, ColGoldHi(), 8.0f, 0.5f);
        if (hov && !live) {
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
            ImGui::TextDisabled("a delivery: %d instance%s, %d lane%s",
                                n.instances, n.instances == 1 ? "" : "s",
                                n.laneCount, n.laneCount == 1 ? "" : "s");
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
          if (!root) ImGui::TextDisabled("right-click drops this delivery (unbox)");
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
        if (!readOnly && !root && live && live->IsDataType(kPayloadGlyph) &&
            PayloadSort(s, false, (const char*)live->Data) == kDelivery) {
          offer(Tgt::Body, (int)i, a, b);
        } else {
          offer(Tgt::Bar, (int)i, a, b);
        }
        break;
      }
      case kModTag: {
        // A TAB, not a caption. The layout gives a tag 96x32 and the edit it
        // carries ("speed x2", "count x9") never fit in it — so the tag is the
        // mod's spiral in its own colour on a small plate, and what it edits is
        // in the tip. A WASTED mod (the record has no such field) is drawn as
        // an empty socket with a slash: charged, and it does nothing.
        const ImU32 col = n.wasted ? ColBronze() : SortColour(kMod);
        dl->AddRectFilled(a, b, Fade(ColDeep(), 0.92f));
        dl->AddRectFilled(a, ImVec2(a.x + 2, b.y), Fade(col, 0.9f));
        dl->AddRect(a, b, Fade(ColBronze(), 0.8f), 0.0f, 0, 2.0f);
        const ImVec2 tm(std::floor((a.x + b.x) * 0.5f), std::floor((a.y + b.y) * 0.5f));
        DrawSpriteCentered(dl, GlyphIcon(kMod), ImVec2(tm.x + 1, tm.y + 1),
                           Fade(ColInk(), 0.7f));
        DrawSpriteCentered(dl, GlyphIcon(kMod), tm,
                           n.wasted ? Fade(ColParchDim(), 0.5f) : col);
        if (n.wasted) SlashOut(dl, a, b, Fade(ColBloodHi(), 0.85f));
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##tag", ImVec2(std::max(8.0f, b.x - a.x),
                                               std::max(8.0f, b.y - a.y)));
        const bool hov = ImGui::IsItemHovered();
        if (hov) Glow(dl, a, b, ColGoldHi(), 6.0f, 0.45f);
        if (hov && !live) {
          BeginTip();
          ImGui::PushStyleColor(ImGuiCol_Text,
                                ImGui::ColorConvertU32ToFloat4(ColGoldHi()));
          ImGui::TextUnformatted(n.label.c_str());
          ImGui::PopStyleColor();
          if (!n.edit.empty()) ImGui::TextDisabled("%s", n.edit.c_str());
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
        break;
      }
      case kOperator:
      case kWord:
      default: {
        const bool op = n.kind == kOperator;
        const float cw = b.x - a.x;
        const bool dimNode = op && !n.complete;
        SlotSurface(dl, a, cw, SlotLook::Filled, false);
        const UIState::GlyphUI* gu = FindGlyph(s, n.glyphId);
        GlyphArt(dl, a, cw, gu ? gu->color : 0u, n.sort);
        // The sort's colour on the RIM, 2 px, all the way round: on the canvas
        // a cell has no neighbours to read a left-edge tag against.
        dl->AddRect(a, b, Fade(SortColour(n.sort), 0.9f), 0.0f, 0, 2.0f);
        // HOW MANY, as pips over the cell instead of an "x3" badge: the badge
        // was 13 px type in the corner of a cell that is 32 px at half scale.
        CountPips(dl, a, b, n.n, Fade(SortColour(n.sort), 0.95f));
        if (dimNode) {
          // An operator with an empty required slot is CHARGED and does
          // nothing. Dimmed and slashed — no number, because the number it used
          // to print (the word cost, struck through) was 13 px of type in the
          // corner of a cell and nobody read it. It is in the tip.
          dl->AddRectFilled(a, b, Fade(ColInk(), 0.45f));
          SlashOut(dl, a, b, Fade(ColBloodHi(), 0.9f));
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##cell", ImVec2(std::max(8.0f, cw),
                                                std::max(8.0f, b.y - a.y)));
        const bool hov = ImGui::IsItemHovered();
        // THE NAME IS THE HOVER. A cell carries no caption, so the ring under
        // the cursor has to be unmistakable: a full glow, not a rim tint.
        if (hov) Glow(dl, a, b, ColGoldHi(), 8.0f, 0.6f);
        if (hov && !live) {
          if (gu) GlyphInfoBox(*gu, SortLabel(n.sort));
          else Tip("a word that no longer exists");
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
        // The operator's PIPS: one per declared slot, hollow with a `_` when
        // empty. A hollow pip is the visual answer to "which words need a
        // prefix", and it is the drop target that fills it.
        if (op) {
          const float ph = std::floor(cw * 0.34f);
          for (int side = 0; side < 2; side++) {
            const bool hasIt = side == 0 ? n.hasLeft : n.hasRight;
            if (!hasIt) continue;
            const bool filled = side == 0 ? n.leftFilled : n.rightFilled;
            const float cx = side == 0 ? a.x : b.x;
            const ImVec2 pa(std::floor(cx - ph * 0.5f),
                            std::floor((a.y + b.y) * 0.5f - ph * 0.5f));
            const ImVec2 pb(pa.x + ph, pa.y + ph);
            if (filled) {
              dl->AddRectFilled(pa, pb, Fade(ColGold(), 0.95f));
              dl->AddRect(pa, pb, ColInk(), 0.0f, 0, 2.0f);
            } else {
              dl->AddRectFilled(pa, pb, Fade(ColInk(), 0.85f));
              dl->AddRect(pa, pb, Fade(ColGoldHi(), 0.95f), 0.0f, 0, 2.0f);
              // The empty slot's mark: a gold underscore drawn as a RECT, not
              // as the character "_" — a 13 px glyph in a pip that is 10 px
              // across at half scale is a smudge.
              dl->AddRectFilled(ImVec2(pa.x + 4, pb.y - 7), ImVec2(pb.x - 4, pb.y - 5),
                                ColGoldHi());
              ImGui::PushID(side);
              ImGui::SetCursorScreenPos(pa);
              ImGui::InvisibleButton("##pip", ImVec2(ph, ph));
              if (ImGui::IsItemHovered() && !live)
                Tip(side == 0 ? "This operator's left slot is empty: it takes "
                                "the word before it. Drop one here."
                              : "This operator's right slot is empty. Drop a "
                                "word here.");
              offer(side == 0 ? Tgt::PipL : Tgt::PipR, (int)i, pa, pb);
              ImGui::PopID();
            }
          }
        }
        break;
      }
    }
    ImGui::PopID();
  }

  // ---- the marker layer, over everything -------------------------------------
  if (hasGhost && !hasRefuse)
    dl->AddRectFilled(ghostA, ghostB, Fade(ColGoldHi(), 0.18f));
  if (hasAccept && !hasRefuse) Ring(dl, acceptA, acceptB, ColGoldHi(), 3);
  if (hasRefuse) {
    dl->AddRectFilled(refuseA, refuseB, Fade(ColBlood(), 0.35f));
    Ring(dl, refuseA, refuseB, ColBloodHi(), 2);
  }

  ImGui::PopFont();
  ImGui::EndChild();
  if (!sNote.empty()) res.note = sNote.c_str();
  return res;
}

}  // namespace ui
