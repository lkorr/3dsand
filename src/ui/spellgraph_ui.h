#pragma once
#include <imgui.h>

#include "ui/overlay.h"

// THE SPELL PAGE (docs/PLAN_spell_graph.md §4-§5, phase 3).
//
// The canvas that sits above the grimoire composer's word row and draws
// `UIState::spellGraph` — the mirror main.cpp fills each frame from
// `ParseWords -> LowerSpell -> BuildGraph` of the composed words. The row stays
// under it as the SPOKEN FORM: what a bound key will actually say.
//
// THE CANVAS NEVER EDITS WORDS. Every gesture latches a `GraphEditIntent`
// naming a TREE OP; main.cpp applies the op from game/spellgraph.h, linearizes
// the new tree back to words, and writes those into `grimoireEditWords`. Two
// views, one truth, and the composer's existing 32-deep undo covers both —
// which is why the canvas calls `PushGrimoireUndo` before it latches.
//
// A UI-INTERNAL header, like ui/theme.h: it names ImGui types and is included
// only from src/ui/. It deliberately does NOT include game/spellgraph.h — the
// canvas draws a mirror of plain ints and strings and holds no glyph index
// across a frame (DESIGN §8b: glyph indices die on every R reload).
namespace ui {

// ---- the drag payloads the composer and the canvas share --------------------
// A glyph and a page both travel by NAME; a graph node travels as its index
// into `UIState::spellGraph.nodes`, which is rebuilt every frame from the same
// words the drop will edit, so it cannot go stale across the release.
constexpr const char* kPayloadGlyph = "SVGLY";
constexpr const char* kPayloadPage = "SVPGE";
constexpr const char* kPayloadGraphNode = "SVGND";

// Peeking at a payload BEFORE the button is released: what lets a drop target
// draw the edit it is about to make instead of performing it silently.
constexpr ImGuiDragDropFlags kPeekFlags =
    ImGuiDragDropFlags_AcceptBeforeDelivery |
    ImGuiDragDropFlags_AcceptNoDrawDefaultRect;

// The composer's undo, pushed before every mutation of the row from wherever
// it comes — the word row, the page list, or the canvas. One owner, because a
// canvas edit and a row edit must land on the same stack or ctrl+Z walks back
// through half a history.
void PushGrimoireUndo(UIState& s);

// The glyph cell's art at any size (the composer's 44 px cell is one case, the
// canvas's 64 px chrome cell another): the sort's colour as a pool of light
// under the engraving, not as a flat swatch.
void GlyphArt(ImDrawList* dl, ImVec2 at, float size, uint32_t color, int type);
// The sort's colour (matter, effect, delivery, mod, operator, separator).
ImU32 SortColour(int sort);
// The sort's name, as the grammar spells it.
const char* SortLabel(int sort);

// THE INFO BOX (PLAN_magic_grammar §9). Every field is read from the glyph's
// JSON entry through the mirror, so the box is never wrong about the glyph and
// a modder's glyph gets one for free. Shared: the arsenal table, the word row
// and the canvas all open the same one.
void GlyphInfoBox(const UIState::GlyphUI& g, const char* sortName);

// The canvas band's height in the composer.
constexpr float kSpellCanvasH = 236.0f;

// What the canvas wants said on the composer's status line this frame, and
// whether it is a refusal (drawn in blood rather than gold). `note` points into
// static storage or into `s`; it is read and dropped in the same frame.
struct GraphCanvasResult {
  const char* note = nullptr;
  bool refused = false;
};

// Draw the canvas into [at, at+size). `readOnly` is an authored starter: every
// drop is refused with the composer's own sentence and nothing latches.
GraphCanvasResult SpellGraphCanvas(UIState& s, ImVec2 at, ImVec2 size,
                                   bool readOnly);

}  // namespace ui
