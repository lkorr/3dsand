#include "ui/spellgraph_ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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
// The 13 px small font's line, which every band reserves under a cell for the
// word's NAME. At 0.5x a cell is 32 px of engraving and the colour of a sort;
// which sort is not which WORD, and the name is the difference between reading
// the tree and guessing at it.
constexpr float kLabelH = 13.0f;

const char* GlyphIcon(int type) {
  switch (type) {
    case kDelivery: return "glyph_form";
    case kMatter: return "glyph_element";
    default: return "glyph_modifier";
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

// Text struck through: an incomplete operator is CHARGED and does nothing, and
// a number with a line through it says that in one glance.
void StruckText(ImDrawList* dl, ImVec2 at, ImU32 col, const char* text) {
  dl->AddText(FontSmall(), 13.0f, at, col, text);
  const ImVec2 ts = ImGui::CalcTextSize(text);
  const float y = std::floor(at.y + ts.y * 0.5f);
  dl->AddRectFilled(ImVec2(at.x, y), ImVec2(at.x + ts.x, y + 2), col);
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
                        ImGuiWindowFlags_HorizontalScrollbar |
                        ImGuiWindowFlags_AlwaysVerticalScrollbar);
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
    dl->AddText(FontSmall(), 13.0f, ImVec2(viewMin.x + 12, viewMin.y + 12),
                Fade(ColParchDim(), 0.8f),
                "an empty page - drag a word onto the hand bar below");
    ImGui::EndChild();
    return res;
  }

  // INTEGER SCALE, 1x OR 0.5x. Nothing between, and never a fractional one:
  // 0.5 halves a 2x sprite back to its authored size, which is still exact.
  //
  // THE LAYER PITCH IS THE UI'S TO CHOOSE, and it is not the layout's 96.
  // `BuildGraph` spaces layers at `kGraphPitch` so a bar, its price line and
  // the row above never collide at 1x; on screen the pitch only has to clear
  // the TALLEST thing in a band, which is a 64 px cell (or a bar plus its
  // 13 px price). Tightening it to that is what lets a two-box spell — leaves,
  // bus, sockets, bar — stand in a composer band instead of scrolling. It is a
  // spacing decision, not a rounding one: every pitch below is a whole even
  // number of screen pixels, and no node's SIZE is touched.
  // A band holds a 64 px cell plus a 13 px NAME under it, or a join's bar plus
  // its 13 px price line. Even pixels only.
  auto pitchFor = [](float sc) {
    const float snug =
        std::floor(((float)kCellPx * sc + kLabelH + 8.0f) * 0.5f) * 2.0f;
    const float loose = std::floor((float)kPitchPx * sc * 0.42f) * 2.0f;
    return std::max(snug, loose);
  };
  // FIT FIRST, THEN SCROLL. 1x if the whole drawing stands in the band, else
  // 0.5x, else 0.5x with the scrollbars the child already has. Never anything
  // between: a fractional scale puts a 2x sprite on half pixels, which is the
  // one thing the pixel-art rule forbids. The margins subtracted here are the
  // drawing's own padding plus a scrollbar's width, so "it fits" means it fits
  // with nothing clipped rather than fits-until-a-bar-appears.
  const float sbar = ImGui::GetStyle().ScrollbarSize;
  const float avail = size.x - 16.0f - sbar, availY = size.y - 8.0f - sbar;
  float scale = 1.0f;
  if ((float)g.width * 1.0f > avail || (float)g.layers * pitchFor(1.0f) > availY)
    scale = 0.5f;
  const float pitch = pitchFor(scale);
  const float gw = std::floor((float)g.width * scale);
  const float gh = (float)g.layers * pitch;

  // A Dummy of the drawing's size gives the child its scroll range for free.
  const ImVec2 base = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(gw + 16.0f, gh + 4.0f));
  // FRAME THE DEEPEST JOIN, once, when the drawing changes shape.
  //
  // A tree that does not fit opened hard against its top-left corner, which put
  // the hand off the side and the innermost delivery — the bar carrying the
  // sockets, the bus and the price, which is what you are actually editing —
  // off the bottom. So the view is centred horizontally and scrolled to put
  // that bar's price line at the foot of the band, with its leaves above it.
  // Set ONCE rather than every frame, or the wheel would fight it.
  {
    static int lastW = -1, lastH = -1;
    if (g.width != lastW || g.height != lastH) {
      lastW = g.width;
      lastH = g.height;
      int deep = -1, deepLayer = -1;
      for (const UIState::SpellGraphUI::Node& n : g.nodes)
        if (n.kind == kJoin && n.layer > deepLayer) {
          deepLayer = n.layer;
          deep = (int)(&n - g.nodes.data());
        }
      float wantY = 0.0f;
      if (deep >= 0) {
        const UIState::SpellGraphUI::Node& dn = g.nodes[(size_t)deep];
        wantY = ((float)(g.layers - 1 - dn.layer) * pitch + (float)dn.h * scale +
                 20.0f) - (size.y - 4.0f);
      }
      ImGui::SetScrollX(std::max(0.0f, std::floor((gw + 16.0f - size.x) * 0.5f)));
      ImGui::SetScrollY(std::max(0.0f, std::floor(wantY)));
    }
  }
  const ImVec2 org(std::floor(base.x + 8.0f), std::floor(base.y + 2.0f));
  // X is the layout's, scaled. Y is the node's LAYER at the screen pitch, plus
  // an offset INSIDE the node (which is scaled): a node's own geometry is the
  // layout's, its band is the canvas's.
  auto X = [&](int gx) { return std::floor(org.x + (float)gx * scale); };
  auto Y = [&](const UIState::SpellGraphUI::Node& n, int off) {
    return std::floor(org.y + (float)(g.layers - 1 - n.layer) * pitch +
                      (float)off * scale);
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
        // The bar: a lit slab with a gold underline, the delivery's noun in
        // caps across it (the same caps the HUD brackets use).
        PanelStyle st;
        st.darkMix = root ? 0.42f : 0.62f;
        st.shadow = 0.0f;
        st.grainAlpha = 0.05f;
        PanelBody(dl, a, b, st);
        dl->AddRectFilled(ImVec2(a.x, b.y - 2), ImVec2(b.x, b.y),
                          Fade(root ? ColGoldPale() : ColGold(), 0.9f));
        dl->AddRect(a, b, Fade(ColBronze(), 0.9f), 0.0f, 0, 2.0f);
        std::string caps = n.label;
        for (char& c : caps) c = (char)toupper((unsigned char)c);
        if (n.n > 1) caps += " x" + std::to_string(n.n);
        const float tw = TrackedTextWidth(caps.c_str(), 2.0f);
        TrackedText(dl, ImVec2(std::floor(a.x + ((b.x - a.x) - tw) * 0.5f),
                               std::floor(a.y + ((b.y - a.y) - 13.0f) * 0.5f)),
                    root ? ColGoldPale() : ColParch(), caps.c_str(), 2.0f);
        // Under the bar: the price, at THIS level. `LowerBox` priced every box
        // on the way out and `BoxPrice` carried it here, which is the whole of
        // "the cost is legible at every level".
        char line[160];
        float ly = b.y + 4;
        if (root) {
          std::snprintf(line, sizeof line, "word %d   tariff %d   carry %d%s",
                        g.wordCost, g.tariff, g.carryCost,
                        g.priceUnknown ? " + ?" : "");
          dl->AddText(FontSmall(), 13.0f, ImVec2(a.x, ly), ColParch(), line);
          ly += 15;
          // What it takes out of the pool, as the bar the HUD draws.
          const float frac = s.manaMax > 0
                                 ? std::min(1.0f, (float)g.manaCost / (float)s.manaMax)
                                 : 0.0f;
          const float bw = std::min(b.x - a.x, 160.0f);
          ValueBar(dl, ImVec2(a.x, ly), ImVec2(a.x + bw, ly + 10), frac, ColMana(),
                   true);
          std::snprintf(line, sizeof line, " %d / %d mana", g.manaCost, s.manaMax);
          dl->AddText(FontSmall(), 13.0f, ImVec2(a.x + bw + 4, ly - 2),
                      g.manaCost > s.mana ? ColEmber() : ColParchDim(), line);
        } else if (n.hasPrice) {
          std::snprintf(line, sizeof line, "tariff %d + carry %d  x%d = %d",
                        n.tariff, n.carryCost, n.priceInstances, n.subtotal);
          // CENTRED UNDER ITS BAR, because it belongs to that bar and a
          // left-aligned number under a narrow join reads as the neighbour's.
          const ImVec2 ts = ImGui::CalcTextSize(line);
          const float px = std::floor((a.x + b.x - ts.x) * 0.5f);
          // On its own dark plate: the price hangs into the band below the bar,
          // where a parent's bus stroke runs, and a number on a gold line is a
          // number nobody reads.
          dl->AddRectFilled(ImVec2(px - 4, ly - 1), ImVec2(px + ts.x + 4, ly + 14),
                            Fade(ColInk(), 0.82f));
          dl->AddText(FontSmall(), 13.0f, ImVec2(px, ly), Fade(ColParch(), 0.95f), line);
          ly += 15;
          if (n.instancesClamped) {
            std::snprintf(line, sizeof line, "fan cut to %d", n.priceInstances);
            const ImVec2 cs = ImGui::CalcTextSize(line);
            dl->AddText(FontSmall(), 13.0f,
                        ImVec2(std::floor((a.x + b.x - cs.x) * 0.5f), ly),
                        ColBloodHi(), line);
          }
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##bar", ImVec2(std::max(8.0f, b.x - a.x),
                                               std::max(8.0f, b.y - a.y)));
        const bool hov = ImGui::IsItemHovered();
        if (hov && !live) {
          BeginTip();
          ImGui::PushStyleColor(ImGuiCol_Text,
                                ImGui::ColorConvertU32ToFloat4(ColGoldHi()));
          ImGui::TextUnformatted(caps.c_str());
          ImGui::PopStyleColor();
          if (root)
            ImGui::TextDisabled("the hand: what you are holding when you cast");
          else
            ImGui::TextDisabled("a delivery: %d instance%s, %d lane%s",
                                n.instances, n.instances == 1 ? "" : "s",
                                n.laneCount, n.laneCount == 1 ? "" : "s");
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
        const ImU32 col = n.wasted ? ColBronze() : SortColour(kMod);
        dl->AddRectFilled(a, b, Fade(ColDeep(), 0.92f));
        dl->AddRectFilled(a, ImVec2(a.x + 2, b.y), Fade(col, 0.9f));
        dl->AddRect(a, b, Fade(ColBronze(), 0.8f), 0.0f, 0, 2.0f);
        const std::string txt = n.edit.empty() ? n.label : n.edit;
        dl->PushClipRect(a, b, true);
        dl->AddText(FontSmall(), 13.0f, ImVec2(a.x + 6, a.y + 4),
                    n.wasted ? Fade(ColParchDim(), 0.55f) : ColParch(), txt.c_str());
        dl->PopClipRect();
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##tag", ImVec2(std::max(8.0f, b.x - a.x),
                                               std::max(8.0f, b.y - a.y)));
        const bool hov = ImGui::IsItemHovered();
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
        if (n.n > 1) {
          char badge[16];
          std::snprintf(badge, sizeof badge, "x%d", n.n);
          CountBadge(dl, b, badge);
        }
        if (dimNode) dl->AddRectFilled(a, b, Fade(ColInk(), 0.45f));
        // The word cost, struck through when the operator is charged and does
        // nothing.
        if (dimNode && n.wordCostOf > 0) {
          char c[16];
          std::snprintf(c, sizeof c, "%d", n.wordCostOf);
          StruckText(dl, ImVec2(a.x + 3, a.y + 2), ColBloodHi(), c);
        }
        // THE WORD'S NAME, under the cell, in the 13 px small font at every
        // scale — the band reserves a line for it (`kLabelH`). A cell is an
        // engraving and a sort colour, and neither of those is which WORD; at
        // 0.5x the engraving is 32 px and the tree read as a row of diamonds.
        // `label` already carries the multiplicity, so it says `gustx2`.
        if (!n.label.empty()) {
          const ImVec2 ls = ImGui::CalcTextSize(n.label.c_str());
          const float lx = std::floor((a.x + b.x - ls.x) * 0.5f);
          dl->PushClipRect(ImVec2(a.x - 10, b.y), ImVec2(b.x + 10, b.y + kLabelH + 2),
                           true);
          dl->AddText(FontSmall(), 13.0f, ImVec2(lx + 1, b.y + 2),
                      Fade(ColInk(), 0.8f), n.label.c_str());
          dl->AddText(FontSmall(), 13.0f, ImVec2(lx, b.y + 1),
                      dimNode ? Fade(ColParchDim(), 0.6f) : ColParch(),
                      n.label.c_str());
          dl->PopClipRect();
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##cell", ImVec2(std::max(8.0f, cw),
                                                std::max(8.0f, b.y - a.y)));
        const bool hov = ImGui::IsItemHovered();
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
              const ImVec2 ts = ImGui::CalcTextSize("_");
              dl->AddText(FontSmall(), 13.0f,
                          ImVec2(std::floor((pa.x + pb.x - ts.x) * 0.5f),
                                 std::floor(pa.y + ph * 0.5f - 9)),
                          ColGoldHi(), "_");
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
