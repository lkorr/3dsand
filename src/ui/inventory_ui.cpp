#include "ui/inventory_ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include <imgui.h>

#include "ui/spellgraph_ui.h"
#include "ui/theme.h"

// HOW THIS IS DRAWN (2026-09-02). Every panel is three layers in a fixed
// order: ui::PanelBody (the lit, grained metal — drop shadow, base, bronze
// wash, sheen, ridge, inset, grain), then ui::Draw9 "panel" (the pixel-art
// riveted frame, HOLLOW so the body shows through), then the contents. Slots
// are the same idea one level down: ui::SlotSurface (recess + glow) under the
// hollow slot sprite under the engraving. See ui/theme.h for why the layers
// are banded and stepped instead of smooth.

namespace {

using ui::Fade;

// ---- geometry ---------------------------------------------------------------
// Every number here is a multiple of the 2x chrome scale, so nothing lands on
// a half pixel. A slot is the 22 px slot sprite doubled.
constexpr float kSlot = 44.0f;
constexpr float kSlotGap = 8.0f;
constexpr float kPad = 16.0f;
// The panel frame sprite's border is 8 source px = 16 screen px; content sits
// inside it by kPad, so the header bar is flush with the frame's inner edge.
constexpr float kFrame = 16.0f;
// The pitch of a cell in a row of them (slot + 4), the arsenal table's gutter
// of sort labels, how many cells a table row and a key row hold, the gap
// between panel columns, and the grimoire's page-list width.
constexpr float kCell = 48.0f;
constexpr float kGutter = 68.0f;
constexpr int kTableCols = 9;
constexpr int kKeyCols = 10;
constexpr float kColGap = 20.0f;
constexpr float kListW = 160.0f;
// Fallback portrait size, used only before main.cpp has created the offscreen
// target (or if it failed to). The REAL size is UIState::portraitW/H, mirrored
// out of the texture that was actually made — the image is displayed 1:1, so
// laying out against a different number than the texture was rendered at would
// scale it and lose the pixel-exactness the nearest sampler exists for.
constexpr float kPortraitWFallback = 320.0f;
constexpr float kPortraitHFallback = 448.0f;

// Drag payload ids. Two, because the two things that can be dragged live in
// different address spaces: an ITEM is a KitRef (game/kitref.h) and a GLYPH is
// a name (glyph indices die on every R reload, so a payload holding one could
// straddle a reload and bind the wrong spell).
constexpr const char* kPayloadItem = "SVKIT";
// A GLYPH and a PAGE travel by NAME, and a GRAPH NODE by its mirror index.
// Those three are declared in ui/spellgraph_ui.h because the canvas over the
// word row offers the same drops the row does, and one copy of a payload id is
// the only way two files can agree about what is in flight.
using ui::kPayloadGlyph;
using ui::kPayloadGraphNode;
using ui::kPayloadPage;
// A word's index inside the page being composed (reordering within the row). A
// BOUND key travels as its slot index: what is on it is already in a mirror
// both ends can read, and a slot is the one thing a key-to-key move needs to
// name.
constexpr const char* kPayloadWord = "SVWRD";
constexpr const char* kPayloadBound = "SVBND";

// Peeking at a payload BEFORE the button is released: what makes the row able
// to draw the drop it is about to perform instead of performing it silently.
constexpr ImGuiDragDropFlags kPeek = ui::kPeekFlags;

// Which chrome sprite carries an item of this kind. A worn piece borrows the
// engraving its own slot uses when empty, so the thing in your pack and the
// hole it belongs in are drawn with the same mark — which is most of what makes
// "where does this go" answerable without a tooltip.
const char* ItemIcon(const std::string& kind) {
  if (kind == "melee") return "item_melee";
  if (kind == "armor_head") return "slot_head";
  if (kind == "armor_chest") return "slot_chest";
  if (kind == "armor_legs") return "slot_legs";
  if (kind == "armor_boots") return "slot_boots";
  if (kind == "armor_shoulders") return "slot_shoulders";
  if (kind == "armor_hands") return "slot_hands";
  if (kind == "armor_belt") return "slot_belt";
  if (kind == "trinket") return "slot_trinket";
  if (kind == "container") return "item_container";
  return "item_unknown";
}

// WHAT IS IN A VESSEL, poured into its icon. The item_container sprite is an
// EMPTY flask (scripts/gen_ui_chrome.py), and this paints the inside of its
// belly and neck in the contents' own colour, from the bottom row up, as many
// pixels as the fill is of the whole inside -- so a half-full flask shows half
// its INSIDE (the wide belly fills slowly, the neck fast), not half its height.
// Whole sprite pixels at the sprite's own scale, snapped exactly as
// DrawSpriteCentered snaps, so the liquid sits in the glass rather than beside
// it. The top row is lifted toward white: that line is what reads as a liquid
// SURFACE instead of a painted band.
//
// THE MASK IS THE SPRITE'S BELLY, row by row (sprite y, first x, last x), one
// pixel inside the silhouette so the glass keeps its rim. gen_ui_chrome.py's
// "container" engraving is the other half; change one, change both.
//
// A GLOWING SUBSTANCE (`glow` = its material's emission, 0..1) is drawn lit:
// the liquid lifted toward white and a soft pool of its own colour under the
// flask, drawn by DrawVesselGlow BEFORE the sprite so the glass sits in it.
// Nothing here names lava -- anything with emission glows, anything without
// does not.
void DrawVesselGlow(ImDrawList* dl, ImVec2 mid, float fill, ImU32 col,
                    float glow) {
  if (fill <= 0.0f || !col || glow <= 0.0f) return;
  const float a = std::clamp(glow, 0.0f, 1.0f);
  // Stepped squares, not a gradient: the UI is pixel art.
  for (int r = 3; r >= 1; r--) {
    const float h = 5.0f + 4.0f * (float)r;
    dl->AddRectFilled(ImVec2(mid.x - h, mid.y + 6.0f - h),
                      ImVec2(mid.x + h, mid.y + 6.0f + h),
                      Fade(col, a * 0.10f * (float)(4 - r)));
  }
}

// A MIXTURE draws as bands (`bandCol`/`bandFrac`, bottom to top, fractions
// of the contents): each row of the belly takes the colour of the band its
// pixels fall in, so oil on water reads as two layers in the icon exactly as
// it settles on the alchemy bench. Only the top row is lifted to a surface.
void DrawVesselContents(ImDrawList* dl, ImVec2 mid, float fill, ImU32 col,
                        float glow = 0.0f,
                        const std::vector<uint32_t>* bandCol = nullptr,
                        const std::vector<float>* bandFrac = nullptr) {
  if (fill <= 0.0f || !col || !ui::Chrome("item_container")) return;
  static constexpr int kRows[][3] = {   // bottom -> top
      {13, 6, 9}, {12, 5, 10}, {11, 4, 11}, {10, 4, 11}, {9, 4, 11},
      {8, 4, 11}, {7, 5, 10},  {6, 6, 9},   {5, 7, 8},   {4, 7, 8}};
  int total = 0;
  for (const auto& r : kRows) total += r[2] - r[1] + 1;
  const float px = ui::kChromeScale;
  const float half = 16.0f * px * 0.5f;
  const ImVec2 o((float)(int)(mid.x - half), (float)(int)(mid.y - half));
  // Pixels to fill, never 0 while anything is in it.
  const int want = std::max(1, (int)std::lround(std::min(fill, 1.0f) * total));
  int done = 0, top = -1;
  for (int i = 0; i < (int)std::size(kRows) && done < want; i++) {
    done += kRows[i][2] - kRows[i][1] + 1;
    top = i;
  }
  // Emissive contents are lit from inside: lifted toward white by their glow.
  if (glow > 0.0f)
    col = ui::Mix(col, IM_COL32(255, 245, 200, 255), 0.35f * std::min(glow, 1.0f));
  const bool banded = bandCol && bandFrac && bandCol->size() > 1 &&
                      bandFrac->size() == bandCol->size();
  int below = 0;   // filled pixels under this row
  for (int i = 0; i <= top; i++) {
    const auto& r = kRows[i];
    ImU32 rc = col;
    if (banded) {
      // The band this row's middle pixel falls in, by share of the fill.
      const int w = r[2] - r[1] + 1;
      const float at = ((float)below + w * 0.5f) / (float)std::max(1, done);
      float acc = 0.0f;
      rc = (*bandCol)[bandCol->size() - 1];
      for (size_t b = 0; b < bandCol->size(); b++) {
        acc += (*bandFrac)[b];
        if (at <= acc) { rc = (*bandCol)[b]; break; }
      }
      if (glow > 0.0f)
        rc = ui::Mix(rc, IM_COL32(255, 245, 200, 255), 0.35f * std::min(glow, 1.0f));
      below += w;
    }
    const ImU32 surface = ui::Mix(rc, IM_COL32(255, 255, 255, 255), 0.35f);
    dl->AddRectFilled(ImVec2(o.x + r[1] * px, o.y + r[0] * px),
                      ImVec2(o.x + (r[2] + 1) * px, o.y + (r[0] + 1) * px),
                      i == top ? surface : rc);
  }
}

// The kind of whatever a KitRef names, off the panel's own mirrors. Used while
// a drag is in flight: the payload is a KitRef (see kPayloadItem's note on why
// it is not the item itself), so "what am I holding" is a lookup, not a field.
std::string KindOfRef(const UIState& s, const KitRef& r) {
  const std::vector<UIState::KitSlotUI>* v = nullptr;
  switch (r.space) {
    case KitSpace::Bag: v = &s.bagSlots; break;
    case KitSpace::Hotbar: v = &s.hotbarSlots; break;
    case KitSpace::Equip: v = &s.equipSlots; break;
    case KitSpace::Loot: v = &s.lootSlots; break;
    default: return std::string();
  }
  if (r.index < 0 || r.index >= (int)v->size()) return std::string();
  return (*v)[r.index].kind;
}

// The sort's colour (plan §12a): matter, effect, delivery, mod, operator. Used
// on the arsenal's column rules, the cells' edge tags, the live readout and the
// composer's canvas, so the sentence you are speaking, the tree you drew it on
// and the panel you bound it from all agree. One owner, in ui/spellgraph_ui.h.
using ui::SortColour;

// The frame sprite over a slot's recess, or a crisp outline when the atlas is
// missing. The recess itself is ui::SlotSurface; this is the rim.
void SlotRim(ImDrawList* dl, ImVec2 at, ui::SlotLook look) {
  const char* frame = look == ui::SlotLook::Refuse   ? "slot_refuse"
                      : look == ui::SlotLook::Hover  ? "slot_hover"
                      : look == ui::SlotLook::Accept ? "slot_hover"
                      : look == ui::SlotLook::Filled ? "slot_filled"
                                                     : "slot";
  if (ui::Chrome(frame)) {
    ui::DrawSprite(dl, frame, at);
    return;
  }
  const ImU32 edge = look == ui::SlotLook::Refuse  ? ui::ColBlood()
                     : look == ui::SlotLook::Hover ? ui::ColGold()
                                                   : ui::ColBronze();
  dl->AddRect(at, ImVec2(at.x + kSlot, at.y + kSlot), edge);
}

// ---- tooltips ---------------------------------------------------------------
// Every tooltip on this screen is set in the SMALL font. The screen's own type
// is the 26 px face beside 2x chrome, and a paragraph of it under the cursor
// covered a third of the panel it was describing; 13 px is the same pixel
// font at its native size — half the height, four times the words per box.
// Wrapped at 320 px so a long description is a block and not a banner.
// The tooltip vocabulary moved to ui/theme.h when the canvas needed the same
// one: 13 px, wrapped at 320, one owner.
using ui::BeginTip;
using ui::EndTip;
using ui::Tip;

// A panel: body, frame, and the header bar inside it. Returns the y where
// content starts (under the header, with a gap).
float PanelChrome(ImDrawList* dl, ImVec2 wp, ImVec2 ws, const char* title,
                  const char* right, const ui::PanelStyle& st) {
  const ImVec2 wb(wp.x + ws.x, wp.y + ws.y);
  ui::PanelBody(dl, wp, wb, st);
  ui::Draw9(dl, "panel", wp, wb);
  const float hb = ui::HeaderBar(dl, ImVec2(wp.x + kFrame, wp.y + kFrame),
                                 ws.x - kFrame * 2, title, right);
  return hb + 14;
}

// ---- one item slot ----------------------------------------------------------
//
// Draws the frame, the contents, the tooltip, and wires both ends of the drag.
// A refusal is SHOWN — the frame turns red under a payload the slot will not
// take, and the tooltip says why — because a slot that merely fails to light
// up is the thing that makes an inventory feel broken.
//
// `accepts` is the slot's authored rule mirrored out of game/equipment.h; the
// panel never re-derives it, so the day ItemKind::ArmorHead exists this
// function needs no change at all. Null for the bag and the hotbar, which are
// containers rather than roles and take anything.

void ItemSlot(UIState& s, const char* id, ImVec2 at,
              const UIState::KitSlotUI& item, KitRef ref,
              const char* emptyIcon, bool acceptsAnything, const char* whyNot,
              bool selected,
              const std::vector<std::string>* accepts = nullptr) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(at);
  ImGui::PushID(id);
  ImGui::InvisibleButton("##slot", ImVec2(kSlot, kSlot));
  s.RecordRect(id, at.x, at.y, at.x + kSlot, at.y + kSlot);
  const bool hovered = ImGui::IsItemHovered();
  const bool filled = !item.name.empty();

  // Is a drag in flight, and would this slot take it? Asked here rather than
  // inside the drop target so the frame can answer BEFORE the player lets go —
  // a refusal you find out about after committing is not a refusal, it is a
  // punishment.
  //
  // AND IT IS TWO ANSWERS, NOT ONE. This used to ask only "does this slot
  // accept anything at all", which is false for every armour row, so picking up
  // ANY item turned the whole figure red — including the one slot the piece
  // belonged in. Asking the real question (does this slot take THIS KIND)
  // costs the dragged item's kind, which is a lookup through the payload's
  // KitRef, and it turns a wall of refusals into one lit slot.
  bool refusing = false, inviting = false;
  if (const ImGuiPayload* p = ImGui::GetDragDropPayload()) {
    if (p->IsDataType(kPayloadItem) && ref.space == KitSpace::Equip) {
      KitRef from{};
      std::memcpy(&from, p->Data, sizeof(from));
      // The slot you picked it UP from is neither an invitation nor a refusal.
      if (!(from == ref)) {
        const std::string kind = KindOfRef(s, from);
        // The LIST is the rule. `acceptsAnything` is a misnomer inherited from
        // when every armour row was empty — it means "accepts at least one
        // kind", which is now true of every slot and answers nothing.
        bool takes = false;
        if (accepts && !kind.empty())
          for (const std::string& a : *accepts)
            if (a == kind) takes = true;
        if (takes)
          inviting = true;
        else
          refusing = true;
      }
    }
  }

  const ui::SlotLook look = refusing  ? ui::SlotLook::Refuse
                            : inviting ? ui::SlotLook::Accept
                            : hovered ? ui::SlotLook::Hover
                            : filled  ? ui::SlotLook::Filled
                                      : ui::SlotLook::Empty;
  ui::SlotSurface(dl, at, kSlot, look, selected);
  SlotRim(dl, at, look);
  const ImVec2 mid(at.x + kSlot * 0.5f, at.y + kSlot * 0.5f);

  if (filled) {
    // A soft pool of gold light under the icon: it is what makes a filled
    // slot read as "an object sitting in a recess" rather than a decal.
    dl->AddRectFilled(ImVec2(mid.x - 12, mid.y - 8), ImVec2(mid.x + 12, mid.y + 14),
                      Fade(ui::ColGold(), 0.08f));
    dl->AddRectFilled(ImVec2(mid.x - 8, mid.y - 4), ImVec2(mid.x + 8, mid.y + 12),
                      Fade(ui::ColGold(), 0.08f));
    DrawVesselGlow(dl, mid, item.fill, item.fillSwatch, item.fillGlow);
    ui::DrawSpriteCentered(dl, ItemIcon(item.kind), ImVec2(mid.x + 1, mid.y + 1),
                           Fade(ui::ColInk(), 0.7f));   // pixel drop shadow
    // THE ICON TAKES THE DYE. The atlas is keyed on kind, so three tunics in
    // three colours are one picture three times — tinting the sprite is what
    // makes them three items in the pack instead of a stack that will not
    // stack. Undyed items pass 0 and draw in the atlas's own ink, exactly as
    // before.
    ui::DrawSpriteCentered(dl, ItemIcon(item.kind), mid,
                           item.dyeSwatch ? item.dyeSwatch : IM_COL32_WHITE);
    DrawVesselContents(dl, mid, item.fill, item.fillSwatch, item.fillGlow,
                       &item.fillBandColor, &item.fillBandFrac);
    if (item.dyeSwatch) {
      // ...and a hard 4 px chip in the corner, because a tinted pixel-art icon
      // at this size reads as a lighting change rather than as a colour. The
      // chip is the unambiguous one.
      dl->AddRectFilled(ImVec2(at.x + 3, at.y + 3), ImVec2(at.x + 9, at.y + 9),
                        item.dyeSwatch);
      dl->AddRect(ImVec2(at.x + 3, at.y + 3), ImVec2(at.x + 9, at.y + 9),
                  Fade(ui::ColInk(), 0.8f));
    }
    if (item.coatSwatch) {
      // A COATED piece wears a drop in the lower-left corner, the coat's own
      // colour: a venomed blade must read as one before its tooltip is asked.
      const float dx = at.x + 4, dy = at.y + kSlot - 12;
      dl->AddRectFilled(ImVec2(dx + 2, dy), ImVec2(dx + 4, dy + 2), item.coatSwatch);
      dl->AddRectFilled(ImVec2(dx, dy + 2), ImVec2(dx + 6, dy + 8), item.coatSwatch);
      dl->AddRect(ImVec2(dx - 1, dy + 1), ImVec2(dx + 7, dy + 9), Fade(ui::ColInk(), 0.85f));
    }
    if (item.count > 1) {
      char buf[16];
      std::snprintf(buf, sizeof buf, "%d", item.count);
      ui::CountBadge(dl, ImVec2(at.x + kSlot - 2, at.y + kSlot - 2), buf);
    }
  } else if (emptyIcon && *emptyIcon) {
    // The engraving sits BEHIND whatever lands here, dim enough to read as a
    // label rather than as contents.
    ui::DrawSpriteCentered(dl, emptyIcon, mid,
                           Fade(IM_COL32_WHITE, hovered ? 0.5f : 0.30f));
  }

  if (filled && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
    ImGui::SetDragDropPayload(kPayloadItem, &ref, sizeof(ref));
    ui::DrawSpriteCentered(ImGui::GetForegroundDrawList(),
                           ItemIcon(item.kind),
                           ImVec2(ImGui::GetMousePos().x,
                                  ImGui::GetMousePos().y));
    ImGui::TextUnformatted(item.name.c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadItem)) {
      KitRef from{};
      std::memcpy(&from, p->Data, sizeof(from));
      // ALWAYS request the move, even into a slot that will refuse it. The
      // validation lives in game/equipment.h where the accepted-kinds table
      // is, and duplicating it here to pre-reject would be the second copy
      // that goes stale the day armour exists. main.cpp answers with the
      // reason, which becomes the flash under the panel.
      s.moveItem.pending = true;
      s.moveItem.from = from;
      s.moveItem.to = ref;
    }
    ImGui::EndDragDropTarget();
  }

  // RIGHT-CLICK: WEAR IT, OR TAKE IT OFF. The one gesture a drag cannot give
  // you, because a drag needs a destination and this is precisely the case
  // where the destination is not in doubt — a pair of boots has exactly one
  // slot. Routed as an intent for the same reason the drag is (see EquipIntent
  // in ui/overlay.h): the panel says WHICH item, not where it lands.
  //
  // Guarded on the drag payload being absent so that releasing a right button
  // mid-drag cannot fire it as well.
  // DOUBLE-CLICK A VESSEL: open it on the alchemy bench. Right-click already
  // means "take it in hand", and a double-click on a flask has no other
  // meaning, so the bench does not steal a gesture from anything.
  if (hovered && filled && item.fill >= 0.0f && ref.space != KitSpace::Loot &&
      !ImGui::GetDragDropPayload() &&
      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    s.alchemy.wantOpen = true;
    s.alchemy.openRef = ref;
  }
  // ...AND A WEAPON OR A WORN PIECE: open it on the ITEM STAGE (ui/item_stage.h),
  // where it can be turned and coated. Same gesture, same no-conflict reason.
  const bool stageable = filled && item.stageable;
  if (hovered && stageable && ref.space != KitSpace::Loot &&
      !ImGui::GetDragDropPayload() &&
      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    s.itemStage.wantOpen = true;
    s.itemStage.openRef = ref;
  }
  if (hovered && filled && !ImGui::GetDragDropPayload() &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    if (ref.space == KitSpace::Loot) {
      // A corpse's piece has one obvious destination - your pack - and the
      // corpse is not a place you put things on. Same latch shape, its own
      // intent, because main.cpp executes it against a different container.
      s.takeLoot.pending = true;
      s.takeLoot.index = ref.index;
      s.takeLoot.all = false;
    } else {
      s.equipItem.pending = true;
      s.equipItem.from = ref;
    }
  }

  if (hovered) {
    BeginTip();
    if (filled) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
                                               ui::ColGoldHi()));
      ImGui::TextUnformatted(item.name.c_str());
      ImGui::PopStyleColor();
      if (!item.kind.empty()) ImGui::TextDisabled("%s", item.kind.c_str());
      // The colour, named and shown. Named because "a rust tunic" is how
      // somebody refers to their own clothes and a hex triple is not; shown
      // because twelve names cannot separate two greens the player chose
      // deliberately (game/dye.h DyeName is lossy on purpose).
      if (!item.dyeName.empty()) {
        ImGui::ColorButton("##tipdye",
                           ImGui::ColorConvertU32ToFloat4(item.dyeSwatch), 0,
                           ImVec2(12, 12));
        ImGui::SameLine();
        ImGui::TextDisabled("dyed %s", item.dyeName.c_str());
      }
      if (!item.coatName.empty()) {
        ImGui::ColorButton("##tipcoat",
                           ImGui::ColorConvertU32ToFloat4(item.coatSwatch), 0,
                           ImVec2(12, 12));
        ImGui::SameLine();
        ImGui::TextUnformatted(("coated with " + item.coatName).c_str());
      }
      if (!item.tip.empty()) ImGui::TextUnformatted(item.tip.c_str());
      // CONDITION, and only for something that can be worn: a sword has no
      // shells to count, and printing "100%" against one would invent a
      // durability stat this game deliberately does not have.
      if (item.wearable) {
        ImGui::TextDisabled("condition %.0f%%", item.condition * 100.0f);
        if (item.ruined) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
                                                   ui::ColBloodHi()));
          ImGui::TextUnformatted("RUINED - too little left to mend");
          ImGui::PopStyleColor();
        }
        // Named for what it will actually do from HERE, which is not the same
        // sentence in both directions.
        if (ref.space != KitSpace::Loot)
          ImGui::TextDisabled(ref.space == KitSpace::Equip
                                  ? "right-click to take off"
                                  : "right-click to wear");
      }
      if (ref.space == KitSpace::Loot)
        ImGui::TextDisabled("right-click to take  .  drag onto a slot to wear");
      else if (stageable)
        ImGui::TextDisabled("double-click to inspect");
    } else if (whyNot && *whyNot && !acceptsAnything) {
      ImGui::TextDisabled("empty");
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
                                               ui::ColEmber()));
      ImGui::TextUnformatted(whyNot);
      ImGui::PopStyleColor();
    } else {
      ImGui::TextDisabled("empty");
    }
    EndTip();
  }
  ImGui::PopID();
}

const UIState::KitSlotUI& SlotOr(const std::vector<UIState::KitSlotUI>& v,
                                 int i) {
  static const UIState::KitSlotUI kEmpty{};
  return (i >= 0 && i < (int)v.size()) ? v[i] : kEmpty;
}

// A glyph in a cell: its colour as a pool of light under the engraving, not
// as a flat swatch — the swatch was the one thing on the old screen that
// looked like a debug readout.
// The art is ui::GlyphArt, parameterised by cell size, because the canvas draws
// the same cell at the graph's 64 px chrome pitch and two copies of this would
// be two looks.
void GlyphContents(ImDrawList* dl, ImVec2 at, const UIState::GlyphUI& g) {
  ui::GlyphArt(dl, at, kSlot, g.color, g.type);
}

// A grimoire page in a cell: the modifier engraving in gold over a ruled
// "page" of two lines, so a bound macro reads as a page and not as a glyph.
void PageContents(ImDrawList* dl, ImVec2 at) {
  const ImVec2 mid(at.x + kSlot * 0.5f, at.y + kSlot * 0.5f);
  const ImU32 gold = Fade(ui::ColGold(), 0.35f);
  dl->AddRectFilled(ImVec2(at.x + 8, at.y + kSlot - 12), ImVec2(at.x + kSlot - 8, at.y + kSlot - 10), gold);
  dl->AddRectFilled(ImVec2(at.x + 8, at.y + kSlot - 8), ImVec2(at.x + kSlot - 12, at.y + kSlot - 6), gold);
  ui::DrawSpriteCentered(dl, "glyph_modifier", ImVec2(mid.x + 1, mid.y - 1), Fade(ui::ColInk(), 0.7f));
  ui::DrawSpriteCentered(dl, "glyph_modifier", ImVec2(mid.x, mid.y - 2), ui::ColGoldHi());
}

// THE INFO BOX (plan §9). Every field is read from the glyph's JSON entry
// through the mirror, so the box is never wrong about the glyph and a
// modder's glyph gets one for free.
using ui::GlyphInfoBox;

// ---- the live portrait ------------------------------------------------------
//
// The image is whatever main.cpp rendered into the offscreen target this
// frame: the real rig, with its real damage, its real pose and whatever is in
// its hand. Nothing here knows any of that — which is the point, and is why a
// severed arm shows up in the panel with no UI code for severed arms.
// WHICH LIMB IS UNDER A SCREEN POINT: the smallest projected box containing
// it, so a hand wins over the arm it overlaps. The one answer every portrait
// click and every pick highlight uses, so what lights up is what a click hits.
int LimbAtPoint(const UIState& s, ImVec2 at, ImVec2 size, ImVec2 m) {
  const float mx = (m.x - at.x) / size.x;
  const float my = (m.y - at.y) / size.y;
  int best = -1;
  float bestArea = 1e9f;
  for (int i = 0; i < UIState::kSlotCount; i++) {
    const UIState::BodyPartUI& b = s.body[i];
    if (!b.present || b.severed || !b.projValid) continue;
    if (mx < b.projMin[0] || mx > b.projMax[0]) continue;
    if (my < b.projMin[1] || my > b.projMax[1]) continue;
    const float area = (b.projMax[0] - b.projMin[0]) *
                       (b.projMax[1] - b.projMin[1]);
    if (area < bestArea) { bestArea = area; best = i; }
  }
  return best;
}

void Portrait(UIState& s, ImVec2 at, ImVec2 size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 br(at.x + size.x, at.y + size.y);
  dl->AddRectFilled(at, br, IM_COL32(16, 12, 22, 255));
  ui::InnerShadow(dl, at, br, 10.0f, 0.5f);

  ImGui::SetCursorScreenPos(at);
  ImGui::InvisibleButton("##portrait", size,
                         ImGuiButtonFlags_MouseButtonLeft |
                         ImGuiButtonFlags_MouseButtonRight |
                         ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  // DEAD IS NOT STILL. The portrait keeps rendering after a death and keeps
  // orbiting with it — what is frozen is the body's POSE, not the picture
  // (main.cpp's DeathBody) — so every control below works on a corpse exactly
  // as it does on a living body. The one exception is the head look at the
  // bottom: there is no rig left to turn a head.
  const bool dead = s.deathScreen;
  // POUR MODE: a filled flask chosen on the FLASKS row turns the left button
  // into the brush. The camera moves to the other two buttons, the way a
  // modelling tool keeps its paint button free: right-drag orbits, middle-drag
  // pans. Choosing the flask is the explicit act, so it wins over a selected
  // spell: the spell bar's selection PERSISTS across casts (caster.h), and
  // gating on it left the brush dead for anyone who had ever picked a spell.
  const bool pourMode = s.bodyValid && !dead && !s.applyText.empty();
  const bool active = ImGui::IsItemActive();
  const bool dragL = active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f);
  const bool dragR = active && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f);
  const bool dragM = active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f);
  const bool orbit = pourMode ? dragR : dragL;
  const bool pan = pourMode ? dragM : (dragR || dragM);

  if (s.portraitTex) {
    dl->AddImage((ImTextureID)s.portraitTex, at, br);
  } else {
    const char* msg = "no body";
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(at.x + (size.x - ts.x) * 0.5f,
                       at.y + (size.y - ts.y) * 0.5f),
                Fade(ui::ColParchDim(), 0.6f), msg);
  }
  ui::InnerShadow(dl, at, br, 16.0f, 0.55f);
  ui::Grain(dl, at, br, 0.035f);

  // ORBIT (left drag; right drag while pouring).
  if (orbit) {
    const ImVec2 d = ImGui::GetIO().MouseDelta;
    s.portraitYaw -= d.x * 0.012f;
    s.portraitPitch = std::clamp(s.portraitPitch - d.y * 0.008f, -0.9f, 0.9f);
  }
  // PAN (right or middle drag; middle only while pouring): instant — writes
  // both current and target.
  if (pan) {
    const ImVec2 d = ImGui::GetIO().MouseDelta;
    const float dx = -d.x * 0.006f, dy = d.y * 0.006f;
    s.portraitPanX += dx;  s.portraitPanXTarget += dx;
    s.portraitPanY += dy;  s.portraitPanYTarget += dy;
  }
  // ZOOM (scroll wheel): instant — writes both current and target.
  // While pouring the wheel (and [ ]) is the brush size, and ctrl+wheel is
  // the zoom: size is what a pour changes most, and it is also how much the
  // flask gives up (PourBrushCellsPerSec).
  if (hovered && pourMode) {
    float grow = 0.0f;
    if (!ImGui::GetIO().KeyCtrl) grow = ImGui::GetIO().MouseWheel;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) grow -= 1.0f;
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) grow += 1.0f;
    if (grow != 0.0f)
      s.pourRadius = std::clamp(s.pourRadius * std::pow(1.25f, grow), 0.1f, 4.0f);
  }
  if (hovered && (!pourMode || ImGui::GetIO().KeyCtrl)) {
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
      const float factor = std::pow(1.15f, wheel);
      s.portraitZoom = std::clamp(s.portraitZoom * factor, 0.5f, 6.0f);
      s.portraitZoomTarget = s.portraitZoom;
    }
  }
  // DOUBLE-CLICK A LIMB to zoom in on it. Detected here rather than from a
  // separate button layer because the portrait's own InvisibleButton covers
  // the whole area and eats the click — a second button on top never sees it.
  if (hovered && s.bodyValid && !pourMode &&
      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    const int best = LimbAtPoint(s, at, size, ImGui::GetMousePos());
    if (best >= 0) {
      s.portraitFocusSlot = best;
      s.portraitPivotSlot = best;
    }
  }

  // SINGLE-CLICK to select a limb in inspect mode. Detected on deactivation
  // (release) so drags do not fire it. The distance gate separates a click
  // from an orbit that barely moved, and the click-count gate lets double-
  // click-to-frame through without also selecting.
  // A readied spell makes the click a cast with the health column shut too.
  const bool castReady = !s.spellText.empty() && !pourMode;
  if ((s.inspectMode || castReady) && s.bodyValid &&
      ImGui::IsItemDeactivated() &&
      ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
      ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 9.0f &&
      ImGui::GetIO().MouseClickedCount[0] < 2) {
    const int best = LimbAtPoint(s, at, size, ImGui::GetMousePos());
    if (s.inspectMode) s.inspectSelected = best;
    // The same click CASTS when there is a sentence to cast (InspectCastPicks).
    // Latched here and not by a button of its own: that would sit on top of
    // this one, and ImGui gives an overlapped click to whichever item was
    // submitted first, so it never saw it. A flask pours through the brush
    // below instead, not through a click on a part.
    if (best >= 0 && castReady) {
      s.castAtPart.pending = true;
      s.castAtPart.slot = best;
    }
  }

  // THE POUR BRUSH. Only WHERE, normalized; main.cpp owns the camera, casts
  // the ray and mirrors the hit back for the ring drawn here. Held, not
  // clicked: every tick the button is down pours a little more.
  s.pourBrush.hover = hovered && pourMode;
  s.pourBrush.active = s.pourBrush.hover && active &&
                       ImGui::IsMouseDown(ImGuiMouseButton_Left);
  if (s.pourBrush.hover) {
    const ImVec2 m = ImGui::GetMousePos();
    s.pourBrush.uv[0] = (m.x - at.x) / size.x;
    s.pourBrush.uv[1] = (m.y - at.y) / size.y;
  }

  // HEAD LOOK: only while not dragging at all, and never on a corpse — the
  // head is not going to follow the cursor, and main.cpp would be feeding a
  // look target to a rig that no longer exists.
  // Nor while pouring: a head that turned to follow the brush would move the
  // skin out from under it.
  s.portraitLookValid = hovered && !dragL && !dragR && !dragM && !dead && !pourMode;
  if (s.portraitLookValid) {
    const ImVec2 m = ImGui::GetMousePos();
    s.portraitLook[0] = std::clamp((m.x - at.x) / size.x * 2.0f - 1.0f, -1.0f,
                                   1.0f);
    s.portraitLook[1] = std::clamp(1.0f - (m.y - at.y) / size.y * 2.0f, -1.0f,
                                   1.0f);
  }

  if (hovered) ui::Glow(dl, at, br, ui::ColGold(), 6.0f, 0.22f);
  ui::Draw9(dl, "panel_inner", at, br);

  // The brush ring, where the ray met the skin, sized to the disc it will
  // coat. Pixel dots rather than an anti-aliased circle: the UI is pixel art.
  if (s.pourBrush.hover && s.pourCursorValid) {
    const ImVec2 c(at.x + s.pourCursorUV[0] * size.x,
                   at.y + s.pourCursorUV[1] * size.y);
    const float rpx = std::max(2.0f, s.pourCursorR * size.x);
    const ImU32 col = Fade(s.applyColor ? s.applyColor : ui::ColGold(),
                           s.pourBrush.active ? 1.0f : 0.75f);
    const int dots = std::clamp((int)(rpx * 0.9f), 8, 64);
    for (int k = 0; k < dots; k++) {
      const float a = 6.2831853f * (float)k / (float)dots;
      const float x = std::floor(c.x + std::cos(a) * rpx);
      const float y = std::floor(c.y + std::sin(a) * rpx);
      dl->AddRectFilled(ImVec2(x - 1, y - 1), ImVec2(x + 1, y + 1), col);
    }
    dl->AddRectFilled(ImVec2(std::floor(c.x) - 1, std::floor(c.y) - 1),
                      ImVec2(std::floor(c.x) + 1, std::floor(c.y) + 1), col);
  }
  // THE BRUSH'S SIZE AND WHAT IT COSTS, always up while pouring is possible:
  // a bigger disc empties the flask faster, and that should be read before
  // the button goes down, not discovered from the fill text afterwards.
  if (pourMode && hovered) {
    char line[64];
    if (s.applyStoppered)
      std::snprintf(line, sizeof line, "stoppered  .  unstop it on the bench");
    else
      std::snprintf(line, sizeof line, "brush %.2f  .  %.1f cells/s",
                    s.pourRadius, s.pourDrainPerSec);
    ImGui::PushFont(ui::FontSmall());
    ui::ShadowText(dl, ImVec2(at.x + 8, at.y + 8),
                   Fade(s.applyColor ? ui::Mix(s.applyColor, IM_COL32_WHITE, 0.4f)
                                     : ui::ColParchDim(), 0.9f),
                   line);
    ImGui::PopFont();
  }

  // RESET button — shown when the target differs from the default (not the
  // current, so it stays visible while the animation is still closing).
  const bool dirty = s.portraitZoomTarget != 1.0f ||
                     std::abs(s.portraitPanXTarget) > 0.001f ||
                     std::abs(s.portraitPanYTarget) > 0.001f;
  if (dirty) {
    if (ui::Button("##portraitreset", ImVec2(br.x - 54, br.y - 28), "reset",
                   false, 48))
      s.portraitReset = true;
  } else if (hovered && !dragL && !dragR && !dragM) {
    // On a corpse the hint says WHAT is being turned, because a body that
    // orbits normally while everything else about it is frozen invites the
    // reading that it is still live.
    const char* hint =
        dead       ? "as you fell  .  drag to turn  .  scroll to zoom"
        : pourMode ? "hold to pour  .  scroll size  .  right-drag turn"
                   : "drag to turn  .  scroll to zoom  .  right-drag to pan";
    ImGui::PushFont(ui::FontSmall());
    const ImVec2 ts = ImGui::CalcTextSize(hint);
    ui::ShadowText(dl, ImVec2(br.x - ts.x - 8, br.y - ts.y - 8),
                   Fade(ui::ColParchDim(), 0.7f), hint);
    ImGui::PopFont();
  }
}

// ---- the limb overlays, and why they all clip -------------------------------
//
// THE PORTRAIT IS A WINDOW ONTO THE RIG, AND A LIMB'S PROJECTED BOX IS NOT
// BOUNDED BY IT. main.cpp projects all eight corners of every limb box and
// keeps the result whenever all eight are in FRONT of the portrait camera
// (ProjectToPortrait) — which says nothing about whether they landed inside
// the picture. Zoom in on the head and the hands project a long way past the
// frame; their callouts were then drawn at that position, i.e. across the
// armour slots and the panel beside them, with nothing on screen to explain
// what they were pointing at.
//
// So every overlay here clips to the portrait rect. Drawing is cut at the
// frame (a limb half out of shot shows half its ticks, which is correct — it
// IS half out of shot), and anything with a hit box clamps the box too, so a
// click can never land on a limb that is not visible under the cursor.
bool ClipToPortrait(ImVec2 at, ImVec2 size, ImVec2& p0, ImVec2& p1) {
  const ImVec2 lo = at, hi(at.x + size.x, at.y + size.y);
  if (p1.x <= lo.x || p0.x >= hi.x || p1.y <= lo.y || p0.y >= hi.y) return false;
  p0.x = std::max(p0.x, lo.x);
  p0.y = std::max(p0.y, lo.y);
  p1.x = std::min(p1.x, hi.x);
  p1.y = std::min(p1.y, hi.y);
  return p1.x - p0.x > 1.0f && p1.y - p0.y > 1.0f;
}

// ---- the health inspector ---------------------------------------------------
//
// The same portrait, with the damaged limbs called out on it and a sorted
// list beside it. It exists because the HUD stick figure answers "is something
// broken" at a glance and this answers "what, and how badly" — two different
// questions, so two different readouts rather than one compromised one.
void InspectOverlay(const UIState& s, ImVec2 at, ImVec2 size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (!s.bodyValid) return;
  // Clipped to the picture, not to the panel: see ClipToPortrait above.
  dl->PushClipRect(at, ImVec2(at.x + size.x, at.y + size.y), true);
  struct Pop { ImDrawList* d; ~Pop() { d->PopClipRect(); } } pop{dl};
  // One wall-clock phase for the whole body, so wounds pulse together and read
  // as one alarm — the same choice DrawBodyFigure makes, for the same reason.
  const float flash = 0.5f + 0.5f * (float)std::sin(ImGui::GetTime() * 4.5);

  for (int i = 0; i < UIState::kSlotCount; i++) {
    const UIState::BodyPartUI& b = s.body[i];
    if (b.severed || !b.projValid) continue;
    const float worst = std::min(b.hpFrac, b.voxelFrac);
    const bool coated = b.stainFrac >= s.stainHudMin && b.stainColor != 0;
    const bool selected = s.inspectSelected == i;
    if (!selected && worst > 0.98f && b.burningVoxels == 0 && !b.bleeding && !coated)
      continue;
    ImU32 col = ui::ColEmber();
    if (selected)
      col = Fade(ui::ColGoldHi(), 0.7f + 0.3f * flash);
    else if (b.burningVoxels > 0)
      col = Fade(ui::ColEmber(), 0.5f + 0.5f * flash);
    else if (b.bleeding)
      col = Fade(ui::ColBloodHi(), 0.45f + 0.55f * flash);
    else if (worst > 0.98f && coated)
      // Nothing is WRONG with this one — it is covered in something. Called
      // out in the substance's OWN colour and never in the wound palette, so
      // a bloodied but unhurt arm cannot be misread as a bleeding one, and
      // steady rather than flashing, because a coat is not an alarm.
      col = Fade(ui::Mix(b.stainColor, IM_COL32_WHITE, 0.35f),
                 0.35f + 0.45f * b.stainFrac);
    else
      col = Fade(ui::ColBloodHi(), 0.35f + 0.45f * (1.0f - worst));
    const ImVec2 p0(at.x + b.projMin[0] * size.x, at.y + b.projMin[1] * size.y);
    const ImVec2 p1(at.x + b.projMax[0] * size.x, at.y + b.projMax[1] * size.y);
    // Corner ticks rather than a full box: a closed rectangle over a character
    // reads as a selection widget, and four brackets read as a callout.
    const float t = std::min(10.0f, std::min(p1.x - p0.x, p1.y - p0.y) * 0.4f);
    if (t <= 1.0f) continue;
    const ImVec2 cs[4] = {p0, ImVec2(p1.x, p0.y), ImVec2(p0.x, p1.y), p1};
    const float sx[4] = {1, -1, 1, -1}, sy[4] = {1, 1, -1, -1};
    for (int k = 0; k < 4; k++) {
      // Filled 2 px strips, not AddLine: pixels, not anti-aliased strokes.
      const float x0 = std::min(cs[k].x, cs[k].x + t * sx[k]);
      const float x1 = std::max(cs[k].x, cs[k].x + t * sx[k]);
      const float y0 = std::min(cs[k].y, cs[k].y + t * sy[k]);
      const float y1 = std::max(cs[k].y, cs[k].y + t * sy[k]);
      dl->AddRectFilled(ImVec2(x0, cs[k].y - (sy[k] < 0 ? 2 : 0)),
                        ImVec2(x1, cs[k].y + (sy[k] > 0 ? 2 : 0)), col);
      dl->AddRectFilled(ImVec2(cs[k].x - (sx[k] < 0 ? 2 : 0), y0),
                        ImVec2(cs[k].x + (sx[k] > 0 ? 2 : 0), y1), col);
    }
  }
}

// The hover frame both pick layers draw: the limb under the cursor, clamped to
// the picture, outlined in pixels, with a one-line tip. DRAW ONLY — the click
// is latched by Portrait's own button (see the note there), resolved with the
// same LimbAtPoint, so the framed limb is the one a click hits.
void PickHover(const UIState& s, ImVec2 at, ImVec2 size, ImU32 base,
                      const char* verb, const std::string& what) {
  const ImVec2 m = ImGui::GetMousePos();
  if (!ImGui::IsWindowHovered() ||
      !ImGui::IsMouseHoveringRect(at, ImVec2(at.x + size.x, at.y + size.y)))
    return;
  const int i = LimbAtPoint(s, at, size, m);
  if (i < 0) return;
  const UIState::BodyPartUI& b = s.body[i];
  ImVec2 p0(at.x + b.projMin[0] * size.x, at.y + b.projMin[1] * size.y);
  ImVec2 p1(at.x + b.projMax[0] * size.x, at.y + b.projMax[1] * size.y);
  if (!ClipToPortrait(at, size, p0, p1)) return;
  const float flash = 0.5f + 0.5f * (float)std::sin(ImGui::GetTime() * 3.0);
  const ImU32 col = Fade(base, 0.5f + 0.5f * flash);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y + 2), col);
  dl->AddRectFilled(ImVec2(p0.x, p1.y - 2), ImVec2(p1.x, p1.y), col);
  dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p0.x + 2, p1.y), col);
  dl->AddRectFilled(ImVec2(p1.x - 2, p0.y), ImVec2(p1.x, p1.y), col);
  BeginTip();
  ImGui::Text("%s %s here", verb, what.c_str());
  EndTip();
}

// CAST ON A PART. With a sentence on the stack, every present limb of the
// inspector becomes a target: click it and the spell resolves there with
// `self` (docs/PLAN_magic_grammar.md §7). The panel never touches the VM — a
// click latches the slot (in Portrait), and main.cpp turns the slot into a
// position and casts.
void InspectCastPicks(UIState& s, ImVec2 at, ImVec2 size) {
  if (!s.bodyValid || s.spellText.empty()) return;
  PickHover(s, at, size, IM_COL32(150, 200, 255, 255), "cast", s.spellText);
}

// WHICH WORN PIECE COVERS WHICH LIMB. The one place health and gear meet: a
// selected limb lights the slot that is protecting it, and a hovered slot
// outlines what it protects on the portrait. Indices are the EquipSlotId order
// from game/equipment.h, the same order the armour columns are laid out in.
//
// -1 is an honest answer and the common one: the lower arms have no piece in
// the set (pauldrons do not reach a forearm), and neither does anything below
// the belt line that boots do not already own.
int ArmorSlotForLimb(int slot) {
  switch (slot) {
    case UIState::kSlotHead:  return 0;  // head
    case UIState::kSlotTorso: return 1;  // chest
    case UIState::kSlotHips:  return 6;  // belt
    case UIState::kSlotArmUL:
    case UIState::kSlotArmUR: return 4;  // shoulders
    case UIState::kSlotHandL:
    case UIState::kSlotHandR: return 5;  // hands
    case UIState::kSlotLegUL:
    case UIState::kSlotLegLL:
    case UIState::kSlotLegUR:
    case UIState::kSlotLegLR: return 2;  // legs
    case UIState::kSlotFootL:
    case UIState::kSlotFootR: return 3;  // boots
    default: return -1;
  }
}

// WHAT THIS PIECE IS STANDING IN FRONT OF. Every limb the hovered armour slot
// covers, shaded on the portrait in steel. Deliberately NOT in the wound
// palette and deliberately not flashing: this is an answer to a question you
// asked with the cursor, not an alarm.
void CoverHighlight(const UIState& s, ImVec2 at, ImVec2 size, int equipIdx) {
  if (equipIdx < 0) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(at, ImVec2(at.x + size.x, at.y + size.y), true);
  struct Pop { ImDrawList* d; ~Pop() { d->PopClipRect(); } } pop{dl};
  for (int i = 0; i < UIState::kSlotCount; i++) {
    const UIState::BodyPartUI& b = s.body[i];
    if (!b.present || b.severed || !b.projValid) continue;
    if (ArmorSlotForLimb(i) != equipIdx) continue;
    ImVec2 p0(at.x + b.projMin[0] * size.x, at.y + b.projMin[1] * size.y);
    ImVec2 p1(at.x + b.projMax[0] * size.x, at.y + b.projMax[1] * size.y);
    if (!ClipToPortrait(at, size, p0, p1)) continue;
    if (p1.x - p0.x < 3.0f || p1.y - p0.y < 3.0f) continue;
    dl->AddRectFilled(p0, p1, Fade(ui::ColSteel(), 0.22f));
    // A 2 px pixel border rather than AddRect's stroke — see InspectOverlay.
    const ImU32 e = Fade(ui::ColSteel(), 0.75f);
    dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + 2), e);
    dl->AddRectFilled(ImVec2(p0.x, p1.y - 2), p1, e);
    dl->AddRectFilled(p0, ImVec2(p0.x + 2, p1.y), e);
    dl->AddRectFilled(ImVec2(p1.x - 2, p0.y), p1, e);
  }
}

// THE LIMB THE COLUMN SHOULD OPEN ON: the one in the most trouble. A health
// view whose detail section opens empty asks the player to go hunting for the
// thing it exists to tell them about; opening on the worst part means the
// first thing the column says is the answer. Falls back to the torso (then to
// whatever is present) on a body with nothing wrong at all.
int WorstLimb(const UIState& s) {
  int best = -1, fallback = -1;
  float bestScore = 1e9f;
  for (int i = 0; i < UIState::kSlotCount; i++) {
    const UIState::BodyPartUI& b = s.body[i];
    if (!b.present) continue;
    if (fallback < 0 || i == UIState::kSlotTorso) {
      if (fallback < 0 || fallback != UIState::kSlotTorso) fallback = i;
    }
    float score = b.severed ? -2.0f : std::min(b.hpFrac, b.voxelFrac);
    if (b.burningVoxels > 0) score -= 1.0f;
    if (b.bleeding) score -= 0.5f;
    if (score < bestScore) { bestScore = score; best = i; }
  }
  return bestScore < 0.999f && best >= 0 ? best : fallback;
}

// ONE wall-clock phase for everything urgent on this screen, so the portrait
// callouts, the triage rows and the status chips pulse together and read as a
// single alarm rather than as four things blinking at each other.
float AlarmPulse() {
  return 0.5f + 0.5f * (float)std::sin(ImGui::GetTime() * 4.5);
}

// A hero pool bar (health, mana): the recessed track, the burn cap drawn
// charred off past `cap`, the name tracked into the left end and the numbers
// at the right. Returns the height it used.
//
// FACTORED OUT because both the character column and the vitals column draw
// one, and a pool whose two drawings disagreed about where the cap line goes
// would be worse than either.
float PoolBar(ImDrawList* dl, ImVec2 at, float w, int32_t cur, int32_t max,
              int32_t cap, ImU32 fill, const char* name, float h = 22.0f,
              bool compact = false) {
  const ImVec2 a = at, b(at.x + w, at.y + h);
  const float f = max > 0 ? std::clamp((float)cur / (float)max, 0.0f, 1.0f) : 0.0f;
  ui::ValueBar(dl, a, b, f, fill, true);
  if (max > 0 && cap >= 0 && cap < max) {
    const float cf = std::clamp((float)cap / (float)max, 0.0f, 1.0f);
    const float cx = std::floor(a.x + (b.x - a.x) * cf);
    dl->AddRectFilled(ImVec2(cx, a.y), b, IM_COL32(40, 30, 26, 235));
    dl->AddLine(ImVec2(cx, a.y), ImVec2(cx, b.y), IM_COL32(120, 60, 40, 255));
  }
  // A short bar gets the 13 px face: the screen's 26 px display font is taller
  // than a 16 px track and would hang out of both ends of it. `compact` asks
  // for the same face on a full-height bar, which is what a 256 px column
  // needs — "HEALTH" and "1014 / 1273" set in the display face come to 270 px
  // between them and print straight through each other.
  const bool tiny = compact || h < 20.0f;
  if (tiny) ImGui::PushFont(ui::FontSmall());
  char buf[64];
  std::snprintf(buf, sizeof buf, "%d / %d", cur < 0 ? 0 : cur, max);
  const ImVec2 ts = ImGui::CalcTextSize(buf);
  const float ty = std::floor(a.y + (h - ts.y) * 0.5f);
  ui::TrackedText(dl, ImVec2(a.x + 9, ty + 1), Fade(ui::ColInk(), 0.9f), name, 2.0f);
  ui::TrackedText(dl, ImVec2(a.x + 8, ty), ui::ColGoldPale(), name, 2.0f);
  ui::ShadowText(dl, ImVec2(b.x - ts.x - 8, ty), ui::ColParch(), buf);
  if (tiny) ImGui::PopFont();
  return h;
}

// ---- the arsenal's pieces ----------------------------------------------------

// The sort's name, and the order the table lists the sorts in: matter, effect,
// operator, delivery, mod — the order a sentence is usually built in.
const char* kSortLabel[5] = {"matter", "effect", "delivery", "mod", "operator"};
const int kSortOrder[5] = {0, 1, 4, 2, 3};

// The drag preview for a word: the cell itself, with its name beside it in
// small type, so what is under the cursor is the thing you picked up and not
// a line of text standing in for it.
void WordDragPreview(const UIState::GlyphUI* g, const char* name) {
  ImGui::Dummy(ImVec2(kSlot, kSlot));
  ImDrawList* pd = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetItemRectMin();
  ui::SlotSurface(pd, p, kSlot, ui::SlotLook::Filled, false);
  if (g) GlyphContents(pd, p, *g);
  else PageContents(pd, p);
  ImGui::SameLine(0, 8);
  ImGui::PushFont(ui::FontSmall());
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((kSlot - 13.0f) * 0.5f));
  ImGui::TextUnformatted(name);
  ImGui::PopFont();
}

const UIState::GlyphUI* FindGlyph(const UIState& s, const std::string& id) {
  for (const UIState::GlyphUI& c : s.glyphsOwned)
    if (c.id == id) return &c;
  return nullptr;
}

const UIState::GrimoirePageUI* FindPageUI(const UIState& s, const std::string& name) {
  for (const UIState::GrimoirePageUI& p : s.grimoirePages)
    if (p.name == name) return &p;
  return nullptr;
}

// ---- the composer's undo ----------------------------------------------------
//
// Called BEFORE every mutation of the word row, from wherever it comes: a drop,
// a right-click, a drag out of the window. Deep enough to walk back a whole
// session of fiddling with a sentence, shallow enough that the stack is a few
// hundred bytes of short strings.
// The stack itself lives in ui/spellgraph_ui.cpp, because the CANVAS over this
// row pushes onto the same one: an undo that walked back through half a history
// would be worse than no undo at all.
using ui::PushGrimoireUndo;
void PushUndo(UIState& s) { PushGrimoireUndo(s); }
// The open page changed under the stacks (list click, save-with-rename, delete,
// duplicate — the last three happen in main.cpp, so this is checked every frame
// rather than wired to the click).
void DropUndo(UIState& s) {
  s.grimoireUndo.clear();
  s.grimoireRedo.clear();
  s.grimoireUndoPage = s.grimoireSelected;
}

// One word of the table: the cell, its sort tag, its valence mark, the drag
// out of it, and the §9 info box on hover.
// A word straight into the page being composed, at the end (the grimoire's
// right-click). Refused silently when the open page is authored or full; the
// composer's own text says why in both cases.
bool WriteWordIntoPage(UIState& s, const char* name) {
  const UIState::GrimoirePageUI* sel = FindPageUI(s, s.grimoireSelected);
  if (sel && sel->readOnly) return false;
  if ((int)s.grimoireEditWords.size() >= s.grimoireMaxWords) return false;
  PushUndo(s);
  s.grimoireEditWords.push_back(name);
  s.grimoireEditDirty = true;
  return true;
}

void GlyphCell(UIState& s, int index, const UIState::GlyphUI& g, ImDrawList* cd, ImVec2 at,
               int sort) {
  ImGui::SetCursorScreenPos(at);
  ImGui::PushID(2000 + index);
  ImGui::InvisibleButton("##kg", ImVec2(kSlot, kSlot));
  const bool hov = ImGui::IsItemHovered();
  // RIGHT-CLICK: the word goes straight onto the end of the open page, so a
  // sentence can be written by clicking down the table in order and then
  // tidied by dragging, instead of aimed cell by cell. Guarded on no drag in
  // flight so releasing a right button mid-drag cannot fire it as well.
  if (hov && g.owned && !ImGui::GetDragDropPayload() &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    WriteWordIntoPage(s, g.id.c_str());
  const ui::SlotLook look = hov ? ui::SlotLook::Hover : ui::SlotLook::Filled;
  ui::SlotSurface(cd, at, kSlot, look, false);
  SlotRim(cd, at, look);
  GlyphContents(cd, at, g);
  // The sort's colour as a 2 px tag on the left edge, and for an operator the
  // valence mark in the corner: < takes the word before it, >< is infix.
  cd->AddRectFilled(ImVec2(at.x + 2, at.y + 4), ImVec2(at.x + 4, at.y + kSlot - 4),
                    Fade(SortColour(sort), 0.9f));
  if (sort == 4) {
    ImGui::PushFont(ui::FontSmall());
    const char* mark = g.valence.find(" > ") != std::string::npos
                           ? (g.valence.find(" < ") != std::string::npos ? "><" : ">")
                           : "<";
    const ImVec2 ms = ImGui::CalcTextSize(mark);
    cd->AddText(ImVec2(at.x + kSlot - ms.x - 5, at.y + 3), Fade(ui::ColGoldPale(), 0.95f), mark);
    ImGui::PopFont();
  }
  if (!g.owned)
    cd->AddRectFilled(at, ImVec2(at.x + kSlot, at.y + kSlot), Fade(ui::ColInk(), 0.62f));
  if (g.owned && ImGui::BeginDragDropSource()) {
    char buf[64] = {};
    std::snprintf(buf, sizeof buf, "%s", g.id.c_str());
    ImGui::SetDragDropPayload(kPayloadGlyph, buf, sizeof(buf));
    WordDragPreview(&g, g.id.c_str());
    ImGui::EndDragDropSource();
  }
  if (hov) GlyphInfoBox(g, kSortLabel[sort]);
  ImGui::PopID();
}

// The table's height for a given number of cells per row, so the caller can
// size the child to the content and put things under it.
float GlyphTableHeight(const UIState& s, int perRow) {
  float h = 0;
  for (int c = 0; c < 5; c++) {
    int n = 0;
    for (const UIState::GlyphUI& g : s.glyphsOwned)
      if (g.type == kSortOrder[c] && !g.hidden) n++;
    if (n == 0) continue;
    h += ((n + perRow - 1) / perRow) * kCell + 8;
  }
  return std::max(h, 26.0f);
}

// EVERY WORD, as a table with one band of rows per sort: the sort's name in
// small type in a gutter on the left, its colour running down beside it, and
// its words at nine to a row. Fifteen matter words are two rows here; in the
// five-columns-by-sort version they were a fifteen-row column in a box that
// showed three.
void GlyphTable(UIState& s, ImVec2 at, ImVec2 size, int perRow) {
  ImGui::SetCursorScreenPos(at);
  ImGui::BeginChild("##known", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  {
    ImDrawList* cd = ImGui::GetWindowDrawList();
    const ImVec2 base = ImGui::GetCursorScreenPos();
    float y = base.y;
    for (int c = 0; c < 5; c++) {
      const int sort = kSortOrder[c];
      int n = 0;
      for (const UIState::GlyphUI& g : s.glyphsOwned)
        if (g.type == sort && !g.hidden) n++;
      if (n == 0) continue;
      const int rows = (n + perRow - 1) / perRow;
      const float bandH = rows * kCell - (kCell - kSlot);
      ImGui::PushFont(ui::FontSmall());
      cd->AddText(ImVec2(base.x, y + 2), Fade(SortColour(sort), 1.0f), kSortLabel[sort]);
      char cnt[16];
      std::snprintf(cnt, sizeof cnt, "%d", n);
      cd->AddText(ImVec2(base.x, y + 16), Fade(ui::ColParchDim(), 0.6f), cnt);
      ImGui::PopFont();
      cd->AddRectFilled(ImVec2(base.x + kGutter - 10, y + 2),
                        ImVec2(base.x + kGutter - 8, y + bandH - 2),
                        Fade(SortColour(sort), 0.55f));
      int k = 0;
      for (int i = 0; i < (int)s.glyphsOwned.size(); i++) {
        const UIState::GlyphUI& g = s.glyphsOwned[i];
        if (g.type != sort || g.hidden) continue;
        const ImVec2 p(base.x + kGutter + (k % perRow) * kCell, y + (k / perRow) * kCell);
        k++;
        GlyphCell(s, i, g, cd, p, sort);
      }
      y += rows * kCell + 8;
    }
    if (s.glyphsOwned.empty()) {
      ImGui::SetCursorScreenPos(base);
      ImGui::TextDisabled("You know no words.");
      y = base.y + 26;
    }
    ImGui::SetCursorScreenPos(base);
    ImGui::Dummy(ImVec2(size.x, y - base.y));
  }
  ImGui::EndChild();
}

// BOUND: the twenty keys, bank A on the number row and bank B on Shift, each
// a drop target for a glyph or a page and cleared by right-click. Returns the
// height used.
float BoundKeys(UIState& s, ImDrawList* dl, ImVec2 at, float width) {
  const float bankGap = 4.0f;
  for (int i = 0; i < (int)s.glyphSlots.size() && i < 20; i++) {
    const int bank = i / 10, col = i % 10;
    const float gx = at.x + col * kCell;
    const float gy = at.y + bank * (kSlot + bankGap);
    if (gx + kSlot > at.x + width) continue;
    const std::string& id = s.glyphSlots[i];
    const bool isPage = i < (int)s.glyphSlotKinds.size() && s.glyphSlotKinds[i] == 2;
    ImGui::SetCursorScreenPos(ImVec2(gx, gy));
    ImGui::PushID(1000 + i);
    ImGui::InvisibleButton("##gs", ImVec2(kSlot, kSlot));
    const bool hov = ImGui::IsItemHovered();
    const UIState::GlyphUI* g = isPage ? nullptr : FindGlyph(s, id);
    const bool filled = g || isPage;
    const ui::SlotLook look = hov ? ui::SlotLook::Hover
                              : filled ? ui::SlotLook::Filled
                                       : ui::SlotLook::Empty;
    ui::SlotSurface(dl, ImVec2(gx, gy), kSlot, look, false);
    SlotRim(dl, ImVec2(gx, gy), look);
    if (g) GlyphContents(dl, ImVec2(gx, gy), *g);
    if (isPage) PageContents(dl, ImVec2(gx, gy));
    // The bank Shift is holding is lit; the other rests.
    if ((bank == 1) != s.glyphBankB)
      dl->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + kSlot, gy + kSlot),
                        Fade(ui::ColInk(), 0.28f));
    {
      char k[6];
      std::snprintf(k, sizeof k, bank ? "S%d" : "%d", (col + 1) % 10);
      ui::KeyBadge(dl, ImVec2(gx + 2, gy + 2), k);
    }
    // EVERY KEY IS A PLACE IT CAN GO (2026-10-01): while a word, a page or a
    // bound key is held, each key that would take it wears a pulsing gold
    // rim, so binding is aiming at a lit row rather than at a guess. The key
    // the drag came from is the one place it cannot go.
    if (const ImGuiPayload* lp = ImGui::GetDragDropPayload()) {
      const bool fromKey = lp->IsDataType(kPayloadBound);
      const bool bindable = fromKey || lp->IsDataType(kPayloadGlyph) ||
                            lp->IsDataType(kPayloadPage);
      if (bindable && !(fromKey && *(const int*)lp->Data == i)) {
        const float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 5.0f);
        // Mouse-in-rect, not IsItemHovered: the drag source is the active
        // item, and a plain hover test is blocked by it.
        const ImVec2 m = ImGui::GetIO().MousePos;
        const bool over = m.x >= gx && m.x < gx + kSlot && m.y >= gy && m.y < gy + kSlot;
        const float o = over ? 3.0f : 1.0f;
        dl->AddRect(ImVec2(gx - o, gy - o), ImVec2(gx + kSlot + o, gy + kSlot + o),
                    Fade(ui::ColGoldHi(), over ? 1.0f : 0.35f + 0.4f * pulse), 0.0f, 0,
                    2.0f);
      }
    }
    // A bound key is a drag SOURCE as well as a target: dragging one onto
    // another moves the binding there and swaps with whatever that key held.
    // Rearranging the row used to mean finding both words in the table again
    // and re-dragging them, which is the one thing the table is worst at once
    // you have twenty keys and know what you want on them.
    if (filled && ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload(kPayloadBound, &i, sizeof i);
      WordDragPreview(g, id.c_str());
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadGlyph)) {
        s.bindGlyph.pending = true;
        s.bindGlyph.slot = i;
        s.bindGlyph.glyphId = (const char*)p->Data;
        s.bindGlyph.page = false;
      }
      if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadPage)) {
        s.bindGlyph.pending = true;
        s.bindGlyph.slot = i;
        s.bindGlyph.glyphId = (const char*)p->Data;
        s.bindGlyph.page = true;
      }
      if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadBound)) {
        int from = -1;
        std::memcpy(&from, p->Data, sizeof from);
        if (from >= 0 && from != i) {
          s.moveBind.pending = true;
          s.moveBind.from = from;
          s.moveBind.to = i;
        }
      }
      ImGui::EndDragDropTarget();
    }
    // Right-click unbinds. A bound slot has to be clearable without needing
    // somewhere to drag it TO.
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !id.empty()) {
      s.bindGlyph.pending = true;
      s.bindGlyph.slot = i;
      s.bindGlyph.glyphId.clear();
      s.bindGlyph.page = false;
    }
    if (hov) {
      BeginTip();
      char key[24];
      std::snprintf(key, sizeof key, bank ? "Shift+%d" : "%d", (col + 1) % 10);
      if (g) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::ColGoldHi()));
        ImGui::TextUnformatted(g->id.c_str());
        ImGui::PopStyleColor();
        if (!g->desc.empty()) ImGui::TextDisabled("%s", g->desc.c_str());
        ImGui::TextDisabled("%s casts it  .  right-click to unbind", key);
        ImGui::TextDisabled("drag it onto another key to move or swap it  .  drag it out to unbind");
      } else if (isPage) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::ColGoldHi()));
        ImGui::Text("[%s]  a page", id.c_str());
        ImGui::PopStyleColor();
        const std::string& ro = i < (int)s.glyphSlotReadouts.size() ? s.glyphSlotReadouts[i] : "";
        ImGui::TextDisabled("%s", ro.empty() ? "(a page that names nothing)" : ro.c_str());
        ImGui::TextDisabled("%s casts it  .  right-click to unbind", key);
        ImGui::TextDisabled("drag it onto another key to move or swap it  .  drag it out to unbind");
      } else {
        ImGui::TextDisabled("%s: unbound", key);
        ImGui::TextDisabled("drag a word or a page here");
      }
      EndTip();
    }
    ImGui::PopID();
  }
  return 2 * kSlot + bankGap;
}

// ---- the grimoire ---------------------------------------------------------
//
// A page list on the left; for the selected page a composer on the right: a
// name, a word row you drag glyphs and pages into and reorder, the derived
// readout, the price, and Save / Duplicate / Delete. The row is described by
// the same DescribeSpell the live sentence uses, so the panel can never
// disagree with the game about what a page means. Binding a page to a key is
// a drag from the list onto the key — the twenty-badge bind row this used to
// have set "S10" in 27 px cells of 26 px type, and was the panel's worst
// clipped text.
void GrimoireBody(UIState& s, ImVec2 at, ImVec2 size) {
  // ---- what the page MEANS, measured first and drawn last ----
  //
  // The readout box spans the WHOLE body, under both columns, because it is
  // the one thing here that must never be clipped and it is prose: at the
  // composer's width the bracket string plus DescribeSpell's verdict wrapped
  // to six lines and ate the canvas above it, and at the body's width the same
  // text is three. Measured here so both columns can be shortened by exactly
  // what it will take.
  const char* roText = s.grimoireEditWords.empty()
                           ? "an empty page - drop words onto the tree above"
                           : s.grimoireEditReadout.empty()
                                 ? "(says nothing)"
                                 : s.grimoireEditReadout.c_str();
  // The versal takes 26 px and its air another 18, and the height this box
  // is measured at has to be the height the text is DRAWN at or the last line
  // falls out of the strip.
  const float roWrapW = size.x - 20 - 38;
  ImGui::PushFont(ui::FontSmall());
  // PROSE ONLY. This box used to reserve a second line for "price N  n / 32
  // words", which the price band under the canvas now says in full and in a
  // place the eye is already on. One fact, one owner — and the 13 px it cost
  // goes to the canvas.
  const float roH = std::max(
      38.0f,
      std::max(13.0f, ImGui::CalcTextSize(roText, nullptr, false, roWrapW).y) + 12);
  ImGui::PopFont();
  const float bodyH = std::max(120.0f, size.y - roH - 6);

  // ---- the page list ----
  // THE INDEX IS THE VERSO (2026-09-22). It used to be a list of gold names
  // on the book's dark plate beside a bright page, which made the canvas look
  // like a window cut in a panel. Drawn on the same vellum, the two columns
  // read as what they are: an open spread, the index of pages on the left
  // leaf and the diagram of the open one on the right.
  {
    ImDrawList* pdl = ImGui::GetWindowDrawList();
    const ImVec2 la(at.x, at.y), lb(at.x + kListW, at.y + bodyH);
    ui::VellumSheet(pdl, la, lb);
    // The scribe's ruling: a single red line down the column, which is where
    // an index's names are written FROM.
    const float rx = std::floor(la.x + 18);
    pdl->AddRectFilled(ImVec2(rx, la.y + 4), ImVec2(rx + 1, lb.y - 4),
                       Fade(ui::ColRubric(), 0.35f));
    ui::DottedRule(pdl, ImVec2(lb.x - 8, la.y + 6), ImVec2(lb.x - 8, lb.y - 6),
                   Fade(ui::ColIronSoft(), 0.30f), 14.0f, 2.0f);
    ui::VellumFrame(pdl, la, lb);
  }
  ImGui::SetCursorScreenPos(at);
  ImGui::BeginChild("##pages", ImVec2(kListW, bodyH), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground);
  {
    ImDrawList* cd = ImGui::GetWindowDrawList();
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const float rowW = ImGui::GetContentRegionAvail().x;
    constexpr float kRow = 22.0f;
    // Names in small type: a captured page is named from its readout
    // ("fire2-trail-projectile"), which at 26 px is wider than this column.
    ImGui::PushFont(ui::FontSmall());
    float py = base.y;
    {
      ImGui::SetCursorScreenPos(ImVec2(base.x, py));
      ImGui::PushID("newpage");
      ImGui::InvisibleButton("##np", ImVec2(rowW, kRow));
      const bool hov = ImGui::IsItemHovered();
      const bool sel = s.grimoireSelected.empty();
      if (sel)
        cd->AddRectFilled(ImVec2(base.x, py), ImVec2(base.x + rowW, py + kRow),
                          Fade(ui::ColRubric(), 0.16f));
      cd->AddText(ImVec2(base.x + 24, py + 4),
                  hov || sel ? ui::ColRubric() : Fade(ui::ColIronSoft(), 0.95f),
                  "a blank leaf");
      // The mark a scribe leaves where a page is still to be written: a
      // pricked cross on the ruling line.
      ui::DottedRule(cd, ImVec2(base.x + 8, py + 11), ImVec2(base.x + 18, py + 11),
                     hov || sel ? ui::ColRubric() : Fade(ui::ColIronSoft(), 0.8f),
                     4.0f, 2.0f);
      ui::DottedRule(cd, ImVec2(base.x + 13, py + 6), ImVec2(base.x + 13, py + 16),
                     hov || sel ? ui::ColRubric() : Fade(ui::ColIronSoft(), 0.8f),
                     4.0f, 2.0f);
      if (ImGui::IsItemClicked()) {
        if (!s.armedPage.empty()) {
          s.armPage.pending = true;
          s.armPage.name.clear();
        }
        s.grimoireSelected.clear();
        s.grimoireEditName.clear();
        s.grimoireEditWords.clear();
        s.grimoireEditDirty = false;
        DropUndo(s);
      }
      if (hov) Tip("A blank page: drag words into the row, name it, save it.");
      ImGui::PopID();
      py += kRow + 4;
    }
    for (int i = 0; i < (int)s.grimoirePages.size(); i++) {
      const UIState::GrimoirePageUI& p = s.grimoirePages[i];
      ImGui::SetCursorScreenPos(ImVec2(base.x, py));
      ImGui::PushID(3000 + i);
      ImGui::InvisibleButton("##pg", ImVec2(rowW, kRow));
      const bool hov = ImGui::IsItemHovered();
      const bool sel = p.name == s.grimoireSelected;
      // THE OPEN PAGE IS THE ONE WITH THE MARKER IN IT: a wash of minium
      // across the line and a solid mark on the ruling, which is how you find
      // your place in a book. Hover is the same mark, hollow.
      if (sel)
        cd->AddRectFilled(ImVec2(base.x, py), ImVec2(base.x + rowW, py + kRow),
                          Fade(ui::ColRubric(), 0.18f));
      // An authored starter keeps a rubricated initial - it is somebody
      // else's page, written in a better hand than yours.
      const ImU32 nameCol = sel ? ui::ColIronGall()
                           : hov ? ui::ColRubric()
                                 : Fade(ui::ColIronSoft(), 0.95f);
      const ImVec2 mk(base.x + 13, py + 11);
      if (p.readOnly)
        ui::PixelDisc(cd, mk, 4.0f, sel || hov ? ui::ColRubric()
                                               : Fade(ui::ColRubric(), 0.6f));
      else
        ui::PixelRing(cd, mk, 4.0f,
                      sel || hov ? ui::ColIronGall() : Fade(ui::ColIronSoft(), 0.7f),
                      2.0f);
      cd->PushClipRect(ImVec2(base.x, py), ImVec2(base.x + rowW - 10, py + kRow), true);
      cd->AddText(ImVec2(base.x + 24, py + 4), nameCol, p.name.c_str());
      cd->PopClipRect();
      if (p.dropped > 0)
        cd->AddText(ImVec2(base.x + rowW - 12, py + 4), ui::ColBlood(), "?");
      // READIED: the portrait casts this page on the limb you click. The
      // portrait's cast frame is the same blue.
      if (p.name == s.armedPage)
        cd->AddRectFilled(ImVec2(base.x + 1, py + 3), ImVec2(base.x + 4, py + kRow - 3),
                          IM_COL32(150, 200, 255, 255));
      if (ImGui::IsItemClicked()) {
        // Opening a page also READIES it: click a limb on the portrait and
        // it is cast there (Portrait, session.cpp).
        s.armPage.pending = true;
        s.armPage.name = p.name;
        s.grimoireSelected = p.name;
        s.grimoireEditName = p.name;
        s.grimoireEditWords = p.words;
        s.grimoireEditDirty = false;
        DropUndo(s);
      }
      // Right-click: nest this page into the open one (never into itself —
      // the save would refuse the cycle anyway; this just does not offer it).
      if (hov && !ImGui::GetDragDropPayload() && p.name != s.grimoireSelected &&
          ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        WriteWordIntoPage(s, p.name.c_str());
      if (ImGui::BeginDragDropSource()) {
        char buf[64] = {};
        std::snprintf(buf, sizeof buf, "%s", p.name.c_str());
        ImGui::SetDragDropPayload(kPayloadPage, buf, sizeof(buf));
        WordDragPreview(nullptr, p.name.c_str());
        ImGui::EndDragDropSource();
      }
      if (hov) {
        BeginTip();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::ColGoldHi()));
        ImGui::Text("[%s]%s", p.name.c_str(), p.readOnly ? "  (authored, read-only)" : "");
        ImGui::PopStyleColor();
        std::string words;
        for (const std::string& w : p.words) words += (words.empty() ? "" : " ") + w;
        ImGui::TextUnformatted(words.c_str());
        ImGui::TextDisabled("%s", p.readout.c_str());
        if (p.priceUnknown) ImGui::TextDisabled("price %d + ?", p.price);
        else ImGui::TextDisabled("price %d", p.price);
        // ITS SHAPE AS ONE GLYPH: what it does when it is named inside
        // another spell (spell.h, "A PAGE USED AS ONE GLYPH").
        if (p.shapeSort >= 0) {
          const char* sorts[6] = {"matter", "an effect", "a delivery", "a mod", "an operator",
                                  "a separator"};
          const char* as = p.shapeSort < 6 ? sorts[p.shapeSort] : "?";
          if (p.carrier)
            ImGui::TextDisabled("as one glyph: a delivery - it carries what is before it");
          else if (p.inputs > 0)
            ImGui::TextDisabled("as one glyph: %s, taking %d input%s (%d before it, %d after)",
                                as, p.inputs, p.inputs == 1 ? "" : "s", p.leftInputs,
                                p.inputs - p.leftInputs);
          else
            ImGui::TextDisabled("as one glyph: %s%s", as,
                                p.outputs > 1 ? " - several things at once" : "");
        }
        ImGui::TextDisabled("click to open and ready it (then click a limb on your portrait to cast it there)");
        ImGui::TextDisabled("right-click to nest it in the open page  .  drag onto a key to bind it");
        EndTip();
      }
      ImGui::PopID();
      py += kRow;
    }
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(base);
    ImGui::Dummy(ImVec2(rowW, py - base.y));
  }
  ImGui::EndChild();

  // ---- the composer ----
  const float compX = at.x + kListW + 12;
  const float compW = size.x - kListW - 12;
  ImGui::SetCursorScreenPos(ImVec2(compX, at.y));
  ImGui::BeginChild("##compose", ImVec2(compW, bodyH), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground);
  {
    ImDrawList* cd = ImGui::GetWindowDrawList();
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const float innerW = ImGui::GetContentRegionAvail().x;
    const UIState::GrimoirePageUI* sel = FindPageUI(s, s.grimoireSelected);
    const bool readOnly = sel && sel->readOnly;
    float cy = base.y;
    // The undo stacks belong to the page that is open. Checked here, every
    // frame, rather than at the clicks that change it — Save-with-rename,
    // Delete and Duplicate all repoint `grimoireSelected` from main.cpp, and a
    // stack that outlived one of those would undo into the wrong page.
    if (s.grimoireUndoPage != s.grimoireSelected) DropUndo(s);
    // ctrl+Z / ctrl+Y (ctrl+shift+Z too, for the other half of the world).
    // Suppressed while any widget is active: the name field two lines below is
    // an InputText, and InputText has its own undo on the same chord.
    if (!readOnly && !ImGui::IsAnyItemActive()) {
      if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z) && !s.grimoireUndo.empty()) {
        s.grimoireRedo.push_back(s.grimoireEditWords);
        s.grimoireEditWords = s.grimoireUndo.back();
        s.grimoireUndo.pop_back();
        s.grimoireEditDirty = true;
      } else if ((ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
                  ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) &&
                 !s.grimoireRedo.empty()) {
        s.grimoireUndo.push_back(s.grimoireEditWords);
        s.grimoireEditWords = s.grimoireRedo.back();
        s.grimoireRedo.pop_back();
        s.grimoireEditDirty = true;
      }
    }
    // ---- THE TOP LINE: the name, and what you can do to the page -----------
    //
    // ONE line, not two. Save / copy / delete used to have a row of their own
    // under the word row, which cost the canvas 48 px for three buttons that
    // are only ever pressed at the END of composing — and put them as far from
    // the name they act on as the panel allows. They belong beside it.
    // MEASURED, not assumed: ui::Button is as wide as its label plus 24, and
    // "delete" in the 26 px chrome face is wider than the 80 px minimum — the
    // first version right-aligned against 3x80 and lost the last two letters
    // off the edge of the composer.
    const char* kBtnLabels[3] = {"save", "copy", "delete"};
    const float kBtnGap = 8.0f;
    float kBtnW = 80.0f;
    for (const char* l : kBtnLabels)
      kBtnW = std::max(kBtnW, ImGui::CalcTextSize(l).x + 24.0f);
    {
      const float btnStrip = kBtnW * 3 + kBtnGap * 2;
      // ONE LINE IF THE NAME STILL GETS A NAME'S WIDTH, two if it does not.
      // The composer is half of what it was since the word column took the
      // right of the panel (2026-09-22), and 3x96 of buttons plus a field with
      // a 120 px floor is wider than what is left — the field simply ran UNDER
      // the buttons, which is the one arrangement that is worse than either.
      const bool btnSide = innerW - btnStrip - 12 >= 120.0f;
      char buf[64];
      std::snprintf(buf, sizeof buf, "%s", s.grimoireEditName.c_str());
      ImGui::SetCursorScreenPos(ImVec2(base.x, cy));
      ImGui::PushItemWidth(btnSide ? innerW - btnStrip - 12 : innerW);
      if (readOnly) ImGui::BeginDisabled();
      if (ImGui::InputTextWithHint("##pagename", "name this page", buf, sizeof buf)) {
        s.grimoireEditName = buf;
        s.grimoireEditDirty = true;
      }
      if (readOnly) ImGui::EndDisabled();
      ImGui::PopItemWidth();
      if (ImGui::IsItemHovered())
        Tip(readOnly ? "An authored page: copy it to edit."
                     : "The page's name. Bind it to a number key and that key casts it.");
      const float rowH = ImGui::GetFrameHeight();
      float bx = base.x + innerW - btnStrip;
      const float btnY = btnSide ? cy : cy + rowH + 6;
      auto button = [&](const char* id, const char* label, bool enabled) {
        if (!enabled) ImGui::BeginDisabled();
        const bool clicked = ui::Button(id, ImVec2(bx, btnY), label, false, kBtnW);
        if (!enabled) {
          ImGui::EndDisabled();
          cd->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                            Fade(ui::ColInk(), 0.45f));
        }
        bx += kBtnW + kBtnGap;
        return clicked && enabled;
      };
      if (button("##save", "save", !readOnly)) {
        s.grimoireOp.pending = true;
        s.grimoireOp.op = UIState::GrimoireIntent::Save;
        s.grimoireOp.name = s.grimoireEditName;
        s.grimoireOp.words = s.grimoireEditWords;
      }
      if (ImGui::IsItemHovered())
        Tip(readOnly ? "An authored page cannot be changed."
                     : "Keep this page under this name. Refused if the name is the library's "
                       "or the page would contain itself.");
      if (button("##dup", "copy", sel != nullptr)) {
        s.grimoireOp.pending = true;
        s.grimoireOp.op = UIState::GrimoireIntent::Duplicate;
        s.grimoireOp.name = s.grimoireSelected;
      }
      if (ImGui::IsItemHovered()) Tip("A copy of this page, yours to edit.");
      if (button("##del", "delete", sel != nullptr && !readOnly)) {
        s.grimoireOp.pending = true;
        s.grimoireOp.op = UIState::GrimoireIntent::Delete;
        s.grimoireOp.name = s.grimoireSelected;
      }
      if (ImGui::IsItemHovered())
        Tip("Tear the page out. Keys bound to it keep its name and speak nothing.");
      cy += std::max(rowH, ImGui::GetItemRectMax().y - cy) + 8;
    }
    // ---- THE CANVAS (docs/PLAN_spell_graph.md §4) ----
    //
    // The spell as a TREE, above the row that spells it. This is the surface
    // the player builds on; the row under it is the spoken form — what a bound
    // key will actually say — and stays because it is the honest view of the
    // thing that gets saved. Every gesture up here latches a tree op that
    // main.cpp applies and linearizes back into the row, so the two can never
    // disagree about what the page means.
    //
    // It gets WHATEVER IS LEFT after the row and the footer under it, never a
    // fixed band: the tree is the thing worth the space, and a fixed height
    // either clips the buttons on a short screen or wastes a hand's width of
    // page on a tall one. The row's shape has to be known first, so it is
    // measured here and laid out below.
    const int nWords = (int)s.grimoireEditWords.size();
    // THE SPOKEN FORM IS A STRIP, not a grid. The row stopped being the
    // authoring surface when the canvas landed above it: it is the honest view
    // of what a bound key will say, so it is drawn in small cells that fit on
    // one line and gives the space back to the tree.
    constexpr float kWSlot = 26.0f, kWCell = 30.0f;
    // THE ROW AND THE PRICE SHARE ONE BAND. They are the two things you read
    // after the tree — what it says and what it costs — and stacking them cost
    // the canvas a whole line for a strip of 26 px cells that never filled half
    // the composer's width. Measured first, because the price group's width is
    // what the row has left.
    const UIState::SpellGraphUI& gg = s.spellGraph;
    char priceParts[128], manaTxt[64];
    std::snprintf(priceParts, sizeof priceParts, "word %d   tariff %d   carry %d%s",
                  gg.wordCost, gg.tariff, gg.carryCost,
                  gg.priceUnknown ? "  + ?" : "");
    std::snprintf(manaTxt, sizeof manaTxt, "%d / %d mana", gg.manaCost, s.manaMax);
    ImGui::PushFont(ui::FontSmall());
    const ImVec2 partsSz = ImGui::CalcTextSize(priceParts);
    const ImVec2 manaSz = ImGui::CalcTextSize(manaTxt);
    ImGui::PopFont();
    constexpr float kManaBarW = 110.0f;
    // BESIDE THE ROW IF IT FITS, ON A LINE OF ITS OWN IF IT DOES NOT. Beside
    // is the good arrangement and the one a wide composer gets; at half the
    // width (the word column, 2026-09-22) the same group is wider than the
    // whole composer, and laying it out from the right edge anyway put the
    // numbers UNDER the word cells and pushed their left half off the child.
    // The band the price falls back to drops the mana BAR — the number beside
    // it says the same thing and a bar is the first 110 px worth giving up.
    const float priceW = partsSz.x + 14 + kManaBarW + 8 + manaSz.x;
    const bool priceSide = innerW - priceW - 20 >= kWCell * 4;
    const float rowW = priceSide ? innerW - priceW - 20 : innerW;
    const int perRow = std::max(1, (int)((rowW + (kWCell - kWSlot)) / kWCell));
    // THE ROW IS AS LONG AS THE SENTENCE plus a couple of empty cells to drop
    // into, never the whole 32 the page can hold. It used to draw all of them,
    // which since the cap went to 32 (rule 4: a socket costs two words) is four
    // rows of mostly-empty slots standing where the canvas wants to be. The
    // capacity is still said, in words, by the "n / 32 words" line below.
    const int cells = std::min(s.grimoireMaxWords,
                               std::max(nWords + 2, std::min(perRow, 8)));
    const int rowLines = (cells + perRow - 1) / perRow;
    // What the canvas does NOT get: the spoken row with the price beside it,
    // and the status line. The buttons left this budget when they moved up
    // beside the name; the price joined the row instead of taking a line.
    const float bandH = std::max(26.0f, (float)rowLines * kWCell);
    const float priceH = priceSide ? 0.0f : 20.0f;
    const float footerH = bandH + priceH + 8 + 20;
    // THE CANVAS TAKES EVERYTHING ELSE. The readout came out of this budget
    // when it moved to the body's full width below both columns; what is left
    // is the tree's, which is the point of the page.
    // The floor is low on purpose: a canvas that refuses to shrink below what
    // it wants does not get bigger, it OVERFLOWS the child and takes the price
    // strip, the row and the status line off the bottom with it.
    const float canvasH =
        std::max(24.0f, std::floor(bodyH - (cy - base.y) - footerH));
    const ui::GraphCanvasResult canvas =
        ui::SpellGraphCanvas(s, ImVec2(base.x, cy), ImVec2(innerW, canvasH), readOnly);
    cy += canvasH + 6;

    // ---- WHAT IT COSTS, at the right end of the row's band -----------------
    //
    // It used to be drawn INSIDE the canvas, hanging off the left end of the
    // hand bar — inside a child that scrolls, at whatever x the tree happened
    // to put the bar, in a band 160 px wide. The mana count fell off the right
    // edge of the composer more often than not, and it is the one number you
    // have to read before you bind a page to a key. Here it is laid out from
    // the composer's RIGHT edge, so it cannot be pushed off one.
    {
      const bool over = gg.manaCost > s.mana;
      const ImU32 numCol =
          nWords ? ui::ColIronGall() : Fade(ui::ColIronSoft(), 0.7f);
      // The plate's rect, and the left edge the parts are written from: beside
      // the row it is a right-aligned group in the row's own band, under the
      // row it is a band of its own spanning the composer.
      const float pTop = priceSide ? cy : cy + bandH;
      const float pBot = priceSide ? cy + bandH : cy + bandH + 20;
      // INSET FROM THE SHEET'S EDGE, not from the composer's: the band is a
      // leaf with a 2 px frame round it now, and a mana count laid out from
      // the last pixel of the child sat ON that frame.
      constexpr float kSheetInset = 8.0f;
      const float px = priceSide ? std::floor(base.x + innerW - priceW - kSheetInset)
                                 : base.x + kSheetInset;
      const float ty = std::floor((pTop + pBot - 13.0f) * 0.5f);
      // THE FOOT OF THE SAME LEAF (2026-09-22). The row and the price used
      // to sit on a dark recessed plate between the canvas above and the
      // colophon below - both vellum - which read as a strip torn out of the
      // middle of the page. It is one sheet now: the diagram, the sentence
      // that speaks it and what it costs, written on the same stock.
      ui::VellumSheet(cd, ImVec2(base.x, cy),
                      ImVec2(base.x + innerW, cy + bandH + priceH));
      ui::VellumFrame(cd, ImVec2(base.x, cy),
                      ImVec2(base.x + innerW, cy + bandH + priceH));
      cd->AddText(ui::FontSmall(), 13.0f, ImVec2(px, ty), numCol, priceParts);
      const float frac = s.manaMax > 0
                             ? std::min(1.0f, (float)gg.manaCost / (float)s.manaMax)
                             : 0.0f;
      if (priceSide) {
        const float bx = px + partsSz.x + 14;
        ui::ValueBar(cd, ImVec2(bx, std::floor((pTop + pBot) * 0.5f - 6)),
                     ImVec2(bx + kManaBarW, std::floor((pTop + pBot) * 0.5f + 6)),
                     frac, over ? ui::ColBlood() : ui::ColMana(), false);
        cd->AddText(ui::FontSmall(), 13.0f, ImVec2(bx + kManaBarW + 8, ty),
                    over ? ui::ColRubric() : numCol, manaTxt);
      } else {
        // No bar: the count, hard against the right edge, where the eye is
        // already looking for the number that decides whether you can cast it.
        cd->AddText(ui::FontSmall(), 13.0f,
                    ImVec2(std::floor(base.x + innerW - manaSz.x - kSheetInset), ty),
                    over ? ui::ColRubric() : numCol, manaTxt);
      }
    }
    // The word row: every cell the page can hold, drawn whether or not it is
    // filled, so the page's capacity is visible and the drop target is the
    // whole row and not one trailing cell. A drop on any empty cell appends.
    //
    // WHAT A DROP DOES (2026-09-14). Every cell PEEKS at the payload rather
    // than waiting for the release, so the row draws the edit it is about to
    // make — a gold caret at the seam a word will land in, both cells lit when
    // the drop will exchange them, a red rim when it will be refused:
    //
    //   a word from the arsenal, a page from the list   inserted BEFORE the
    //       cell under the cursor (past the last word: appended)
    //   a word already in the row, onto a NEIGHBOUR     the two SWAP
    //   a word already in the row, anywhere else        moved to sit BEFORE
    //       the word it was dropped on
    //   ctrl held                                       a move becomes a COPY
    //
    // The swap is the case that used to be missing, and its absence read as the
    // row ignoring the drag: moving a word one place RIGHT is erase-then-
    // insert-before-its-old-right-neighbour, which lands it exactly where it
    // already was. (One place LEFT happened to work, which made it worse — the
    // same gesture answered in one direction and not the other.)
    const float rowY = cy;
    auto cellAt = [&](int i) {
      return ImVec2(base.x + (i % perRow) * kWCell, rowY + (i / perRow) * kWCell);
    };
    auto insertWord = [&](int at, const char* name) {
      if (readOnly) return;
      if ((int)s.grimoireEditWords.size() >= s.grimoireMaxWords) return;
      at = std::max(0, std::min(at, (int)s.grimoireEditWords.size()));
      PushUndo(s);
      s.grimoireEditWords.insert(s.grimoireEditWords.begin() + at, name);
      s.grimoireEditDirty = true;
    };
    // What is in flight, asked once for the whole row: the source cell is
    // ghosted while its word is held, and the marker layer under the loop
    // needs the same answer.
    const ImGuiPayload* live = ImGui::GetDragDropPayload();
    const bool dragWord = live && live->IsDataType(kPayloadWord);
    const int dragFrom = dragWord ? *(const int*)live->Data : -1;
    const bool copyMod = ImGui::GetIO().KeyCtrl;
    const bool full = nWords >= s.grimoireMaxWords;
    // Set by whichever cell is under the cursor, drawn after the row.
    int caretAt = -1;             // the seam a word would be inserted at
    int swapA = -1, swapB = -1;   // the pair a drop would exchange
    int refuseAt = -1;            // the cell that would refuse the drop
    const char* rowNote = nullptr;
    for (int i = 0; i < cells; i++) {
      const ImVec2 p = cellAt(i);
      ImGui::SetCursorScreenPos(p);
      ImGui::PushID(4000 + i);
      ImGui::InvisibleButton("##wd", ImVec2(kWSlot, kWSlot));
      const bool hov = ImGui::IsItemHovered();
      // The LIVE size, not `nWords`: a right-click below removes a word from
      // under the rest of this very loop, and the last cell would then index
      // one past the end of a vector that has already shrunk.
      const bool has = i < (int)s.grimoireEditWords.size();
      const ImVec2 pb2(p.x + kWSlot, p.y + kWSlot);
      // AN EMPTY CELL IS A PRICKED SQUARE - the place on the ruling where a
      // word is still to be written. A full one is the roundel the canvas
      // draws, at this size; nothing here is a slot any more, because nothing
      // here is a thing you own. It is a sentence.
      if (!has) {
        const ImU32 c = Fade(ui::ColIronSoft(), hov ? 0.85f : 0.45f);
        ui::DottedRule(cd, ImVec2(p.x + 3, p.y + 3), ImVec2(pb2.x - 3, p.y + 3), c, 5.0f, 2.0f);
        ui::DottedRule(cd, ImVec2(p.x + 3, pb2.y - 4), ImVec2(pb2.x - 3, pb2.y - 4), c, 5.0f, 2.0f);
        ui::DottedRule(cd, ImVec2(p.x + 3, p.y + 3), ImVec2(p.x + 3, pb2.y - 4), c, 5.0f, 2.0f);
        ui::DottedRule(cd, ImVec2(pb2.x - 4, p.y + 3), ImVec2(pb2.x - 4, pb2.y - 4), c, 5.0f, 2.0f);
      }
      if (hov) ui::PageHover(cd, p, pb2);
      if (has) {
        const std::string& w = s.grimoireEditWords[i];
        const UIState::GlyphUI* g = FindGlyph(s, w);
        const UIState::GrimoirePageUI* pg = g ? nullptr : FindPageUI(s, w);
        if (g) {
          ui::GlyphRoundel(cd, p, pb2, g->color, g->type, 0.5f, false);
        } else if (pg) {
          // A PAGE WRITTEN INTO A PAGE: a roundel with a second ring round it,
          // the mark for "this one word is a whole passage somewhere else".
          ui::GlyphRoundel(cd, p, pb2, 0u, 4 /* operator: plain ink */, 0.5f, false);
          ui::PixelRing(cd, ImVec2(p.x + kWSlot * 0.5f, p.y + kWSlot * 0.5f),
                        kWSlot * 0.5f - 5.0f, Fade(ui::ColRubric(), 0.9f), 2.0f);
        } else {
          cd->AddText(ui::FontSmall(), 13.0f,
                      ImVec2(p.x + kWSlot * 0.5f - 4, p.y + kWSlot * 0.5f - 7),
                      ui::ColBloodHi(), "?");
        }
        // Reorder: drag a word onto another cell. Drag it out of every panel
        // and it leaves the page (the drop handler at the end of the screen).
        if (!readOnly && ImGui::BeginDragDropSource()) {
          int idx = i;
          ImGui::SetDragDropPayload(kPayloadWord, &idx, sizeof idx);
          WordDragPreview(g, w.c_str());
          ImGui::EndDragDropSource();
        }
        // The cell a held word CAME from, ghosted: the preview under the
        // cursor is the word, so the row should read as having a hole in it
        // rather than as holding the word twice.
        if (dragFrom == i && !copyMod)
          cd->AddRectFilled(p, pb2, Fade(ui::ColVellum(), 0.75f));
        if (hov) {
          BeginTip();
          ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::ColGoldHi()));
          if (g) ImGui::TextUnformatted(w.c_str());
          else if (pg) ImGui::Text("[%s]  a page", w.c_str());
          else ImGui::Text("%s  (a word that no longer exists)", w.c_str());
          ImGui::PopStyleColor();
          if (g) ImGui::TextDisabled("%s  .  %s", ui::SortLabel(g->type), g->desc.c_str());
          else if (pg) ImGui::TextDisabled("%s", pg->readout.c_str());
          if (!readOnly)
            ImGui::TextDisabled("drag to reorder (a neighbour swaps)  .  ctrl+drag to copy  .  "
                                "right-click or drag out to remove");
          EndTip();
        }
        if (!readOnly && hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          PushUndo(s);
          s.grimoireEditWords.erase(s.grimoireEditWords.begin() + i);
          s.grimoireEditDirty = true;
        }
      } else if (hov) {
        Tip(readOnly ? "An authored page: copy it to edit."
                     : "Drop a word from the arsenal, or a page from the list, here.");
      }
      // THE DROP. Peeked rather than accepted, so the same code that performs
      // the edit also describes it to the marker layer below; `IsDelivery` is
      // the only thing separating "would" from "did".
      if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadWord, kPeek);
        const bool moving = p != nullptr;
        if (!p) p = ImGui::AcceptDragDropPayload(kPayloadGlyph, kPeek);
        if (!p) p = ImGui::AcceptDragDropPayload(kPayloadPage, kPeek);
        const bool deliver = p && p->IsDelivery();
        if (p && readOnly) {
          refuseAt = i;
          rowNote = "an authored page cannot be changed - copy it to make one of your own";
        } else if (p && moving) {
          const int from = *(const int*)p->Data;
          if (from < 0 || from >= nWords) {
            // A stale index (the row changed under the drag): drop nothing.
          } else if (copyMod) {
            // COPY: the word stays where it is and a second one lands at the
            // seam. `fire fire` is a real sentence and used to mean a second
            // trip to the table.
            if (full) {
              refuseAt = i;
              rowNote = "this page is full - remove a word to make room";
            } else {
              caretAt = std::min(i, nWords);
              rowNote = "ctrl: a copy - the word you are holding stays where it is";
              if (deliver) {
                // By VALUE: the source lives in the vector insertWord is about
                // to grow, and a c_str() into it would not survive the reallocation.
                const std::string dup = s.grimoireEditWords[from];
                insertWord(caretAt, dup.c_str());
              }
            }
          } else if ((i - from == 1 || from - i == 1) && i < nWords) {
            swapA = from;
            swapB = i;
            if (deliver) {
              PushUndo(s);
              std::swap(s.grimoireEditWords[from], s.grimoireEditWords[i]);
              s.grimoireEditDirty = true;
            }
          } else if (i != from) {
            // Before the word under the cursor: the target's index once the
            // dragged word has been lifted out of the row. Clamped, so a drop
            // on an empty cell past the end means "put it last".
            const int to = std::min(i > from ? i - 1 : i, nWords - 1);
            if (to != from) {
              caretAt = std::min(i, nWords);
              if (deliver) {
                PushUndo(s);
                const std::string w = s.grimoireEditWords[from];
                s.grimoireEditWords.erase(s.grimoireEditWords.begin() + from);
                s.grimoireEditWords.insert(s.grimoireEditWords.begin() + to, w);
                s.grimoireEditDirty = true;
              }
            }
          }
        } else if (p) {
          if (full) {
            refuseAt = i;
            rowNote = "this page is full - remove a word to make room";
          } else {
            caretAt = std::min(i, nWords);
            if (deliver) insertWord(caretAt, (const char*)p->Data);
          }
        }
        ImGui::EndDragDropTarget();
      }
      ImGui::PopID();
    }
    // EVERY SEAM A DROP COULD LAND IN, while a word or a page is in flight
    // (2026-10-01): a faint caret at each one, so the row says where it can
    // take the thing before the cursor gets there. The seam under the cursor
    // gets the full gold caret below. A held row word skips the two seams
    // beside its own cell - dropping it there would put it where it already is.
    {
      const bool rowDrag = live && !readOnly &&
                           (dragWord || live->IsDataType(kPayloadGlyph) ||
                            live->IsDataType(kPayloadPage));
      const bool canGrow = !full || (dragWord && !copyMod);
      if (rowDrag && canGrow) {
        const float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 5.0f);
        const ImU32 c = Fade(ui::ColGold(), 0.35f + 0.35f * pulse);
        for (int k = 0; k <= nWords && k < cells; k++) {
          if (k == caretAt) continue;
          if (dragWord && !copyMod && (k == dragFrom || k == dragFrom + 1)) continue;
          const ImVec2 cp = cellAt(k);
          const float x = std::floor(k % perRow == 0 ? cp.x + 1 : cp.x - 3);
          cd->AddRectFilled(ImVec2(x + 1, cp.y + 4), ImVec2(x + 3, cp.y + kWSlot - 4), c);
        }
      }
    }
    // THE MARKER LAYER, over the whole row so a marker at a cell's edge is
    // never buried by the next cell's recess.
    if (caretAt >= 0) {
      const ImVec2 cp = cellAt(caretAt);
      // In the first column the seam would fall outside the child's clip, so
      // it sits just inside the cell instead of just before it.
      const float x = std::floor(caretAt % perRow == 0 ? cp.x + 1 : cp.x - 3);
      cd->AddRectFilled(ImVec2(x, cp.y - 2), ImVec2(x + 4, cp.y + kWSlot + 2), ui::ColGoldHi());
      cd->AddRectFilled(ImVec2(x - 3, cp.y - 5), ImVec2(x + 7, cp.y - 1), ui::ColGoldHi());
      cd->AddRectFilled(ImVec2(x - 3, cp.y + kWSlot + 1), ImVec2(x + 7, cp.y + kWSlot + 5),
                        ui::ColGoldHi());
    }
    if (swapA >= 0) {
      const int pair[2] = {swapA, swapB};
      for (int k : pair) {
        const ImVec2 cp = cellAt(k);
        cd->AddRect(ImVec2(cp.x - 2, cp.y - 2), ImVec2(cp.x + kWSlot + 2, cp.y + kWSlot + 2),
                    ui::ColGoldHi(), 0.0f, 0, 2.0f);
      }
      // Two arrowheads back to back in the gap, pointing at where each word is
      // going. Only when the pair is side by side — across a row break there
      // is no gap to put them in, and the two lit cells say it alone.
      if (swapA / perRow == swapB / perRow) {
        const ImVec2 cp = cellAt(std::min(swapA, swapB));
        const float mx = cp.x + kWSlot + (kWCell - kWSlot) * 0.5f, my = cp.y + kWSlot * 0.5f;
        cd->AddTriangleFilled(ImVec2(mx - 7, my), ImVec2(mx - 1, my - 5), ImVec2(mx - 1, my + 5),
                              ui::ColRubric());
        cd->AddTriangleFilled(ImVec2(mx + 7, my), ImVec2(mx + 1, my - 5), ImVec2(mx + 1, my + 5),
                              ui::ColRubric());
      }
    }
    if (refuseAt >= 0) {
      const ImVec2 cp = cellAt(refuseAt);
      cd->AddRectFilled(cp, ImVec2(cp.x + kWSlot, cp.y + kWSlot), Fade(ui::ColBlood(), 0.35f));
      cd->AddRect(cp, ImVec2(cp.x + kWSlot, cp.y + kWSlot), ui::ColBlood(), 0.0f, 0, 2.0f);
    }
    cy += bandH + priceH + 8;

    // The status line gets a line of its own, and the page's LENGTH gets its
    // right end — the one number that belongs to the row rather than to the
    // price, parked where it cannot push the row onto a second line the way it
    // did from inside the price group.
    const float statusX = base.x + 2, statusY = cy;
    float statusW = innerW - 4;
    {
      char wc[32];
      std::snprintf(wc, sizeof wc, "%d / %d words", nWords, s.grimoireMaxWords);
      ImGui::PushFont(ui::FontSmall());
      const ImVec2 ws2 = ImGui::CalcTextSize(wc);
      ImGui::PopFont();
      cd->AddText(ui::FontSmall(), 13.0f,
                  ImVec2(std::floor(base.x + innerW - ws2.x), statusY),
                  Fade(nWords >= s.grimoireMaxWords ? ui::ColRubric()
                                                     : ui::ColParchDim(),
                       0.9f),
                  wc);
      statusW = std::max(80.0f, innerW - ws2.x - 16);
    }

    // The status line: what just happened, else what to do next.
    {
      ImGui::PushFont(ui::FontSmall());
      const char* msg = nullptr;
      ImU32 col = Fade(ui::ColParchDim(), 0.85f);
      // A live drag speaks FIRST: "this page is full" while you are still
      // holding the word is a refusal you can act on, and the same sentence
      // after the release is only a report.
      if (canvas.note) {
        // The CANVAS speaks first: it is where the gesture is happening, and
        // its refusal ("give the sockets before this one a payload first") is
        // the one you can act on without letting go.
        msg = canvas.note;
        col = canvas.refused ? ui::ColBloodHi() : ui::ColGoldHi();
      } else if (rowNote) {
        msg = rowNote;
        col = refuseAt >= 0 ? ui::ColBloodHi() : ui::ColGoldHi();
      } else if (!s.spellGraph.expandedNote.empty()) {
        msg = s.spellGraph.expandedNote.c_str();
        col = Fade(ui::ColParchDim(), 0.9f);
      } else if (!s.kitMessage.empty() && s.kitMessageAge < 4.0f) {
        msg = s.kitMessage.c_str();
        col = ui::ColEmber();
      } else if (readOnly) {
        // SHORT ENOUGH FOR THE NARROW COMPOSER. The status line is one band of
        // 20 px and it CLIPS rather than wraps; since the word column halved
        // the composer (2026-09-22) the long form lost its last three words
        // mid-phrase, which reads worse than saying less.
        msg = "authored page - copy it to edit";
      } else if (s.grimoireEditDirty) {
        msg = "unsaved";
        col = ui::ColGoldHi();
      } else if (!sel && nWords == 0) {
        msg = "drag words onto the tree, then bind the page to a key";
      }
      if (msg) {
        cd->PushClipRect(ImVec2(statusX, statusY - 2),
                         ImVec2(statusX + statusW, statusY + 30), true);
        cd->AddText(ui::FontSmall(), 13.0f, ImVec2(statusX, statusY), col, msg, nullptr,
                    statusW);
        cd->PopClipRect();
      }
      ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(base);
    ImGui::Dummy(ImVec2(innerW, cy - base.y));
  }
  ImGui::EndChild();

  // ---- WHAT THE PAGE MEANS, across the foot of the panel ----
  //
  // The bracket string the parser built, then DescribeSpell's verdict on its
  // own line — the sentence that says what the spell DOES, which the brackets
  // deliberately do not. Wrapped, never clipped. Drawn on the PANEL's list
  // rather than a child's, because it is the width of both columns.
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(ui::FontSmall());
    // THE COLOPHON. What the page MEANS, written across the foot of the
    // spread on the same stock as the spread, and opened by a rubricated
    // versal the way a passage in a book like this is opened. The letter is
    // the page's own initial - it changes with the page, which is what makes
    // it read as written for this passage rather than as an ornament.
    const ImVec2 a(at.x, at.y + bodyH + 6), b(at.x + size.x, a.y + roH);
    ui::VellumSheet(dl, a, b);
    char versal = '*';
    for (char c : s.grimoireEditName)
      if (isalpha((unsigned char)c)) { versal = (char)toupper((unsigned char)c); break; }
    if (versal == '*')
      for (const char* c = roText; *c; c++)
        if (isalpha((unsigned char)*c)) { versal = (char)toupper((unsigned char)*c); break; }
    const float vs = std::min(roH - 8.0f, 26.0f);
    ui::Versal(dl, ImVec2(a.x + 6, std::floor(a.y + (roH - vs) * 0.5f)), versal, vs);
    dl->AddText(ui::FontSmall(), 13.0f, ImVec2(a.x + 12 + vs, a.y + 6),
                s.grimoireEditWords.empty() ? Fade(ui::ColIronSoft(), 0.8f)
                                            : ui::ColIronGall(),
                roText, nullptr, roWrapW);
    ui::VellumFrame(dl, a, b);
    ImGui::PopFont();
  }
}

// ---- THE LOOT PANEL (game/corpses.h) ----------------------------------------
//
// A corpse's gear, drawn where the grimoire (wide) or the arsenal (narrow)
// otherwise sits: above or beside the pack, so a piece drags into your bag
// along one short line. Take-only - every slot here is a drag SOURCE and a
// right-click TAKE, and a drag INTO one is refused by main.cpp with the
// reason (a thing put on a corpse would have no body in the world).
// ---- THE ALCHEMY BENCH -------------------------------------------------------
constexpr float kBenchColW = 340.0f;   // the bench panel's left column
//
// The spellbook's column while a vessel is open on the bench: tools and
// readouts down the left, the bench's picture (game/alchemy_bench.h, drawn by
// main.cpp into s.alchemy.tex) at the largest INTEGER scale the rest of the
// column fits -- the picture is pixel art and a fractional scale would smear
// it. The pointer over the picture goes back as sim pixels (y up).
namespace {
// `t` cut to fit `maxW` pixels in the current font, ".." on the end when cut
// (owner, 2026-09-27: the bench column's text ran under the table).
std::string FitText(const std::string& t, float maxW) {
  if (ImGui::CalcTextSize(t.c_str()).x <= maxW) return t;
  std::string c = t;
  while (!c.empty() && ImGui::CalcTextSize((c + "..").c_str()).x > maxW) c.pop_back();
  while (!c.empty() && c.back() == ' ') c.pop_back();
  return c + "..";
}
void BenchParts(ImDrawList* dl, ImVec2 at, float w,
                const std::vector<UIState::AlchemyUI::Portion>& parts, int cap,
                float& y) {
  const float lineH = ImGui::GetTextLineHeight();
  int total = 0;
  for (const auto& p : parts) total += p.dissolved ? 0 : p.eighths;   // in solution takes no room
  if (parts.empty()) {
    dl->AddText(ImVec2(at.x, y), Fade(ui::ColParch(), 0.6f), "empty");
    y += lineH + 4;
  }
  // Listed TOP LAYER FIRST, the way the flask reads (parts come heaviest
  // first, the order they settle in).
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    const auto& p = *it;
    // A hard colour chip and the name, the amount in whole cells (rounded up:
    // a trace is not "0").
    dl->AddRectFilled(ImVec2(at.x, y + 2), ImVec2(at.x + 12, y + 14), p.color | 0xFF000000u);
    dl->AddRect(ImVec2(at.x, y + 2), ImVec2(at.x + 12, y + 14), ui::ColInk());
    char b[96];
    std::snprintf(b, sizeof b, "%d", (p.eighths + 7) / 8);
    const ImVec2 ts = ImGui::CalcTextSize(b);
    dl->AddText(ImVec2(at.x + 20, y), ui::ColParch(), FitText(p.name, w - 20 - ts.x - 10).c_str());
    dl->AddText(ImVec2(at.x + w - ts.x, y), ui::ColGoldPale(), b);
    y += lineH + 4;
  }
  // The whole fill as a bar in its layers, bottom = left.
  const float bh = 8;
  dl->AddRectFilled(ImVec2(at.x, y), ImVec2(at.x + w, y + bh), ui::ColInk());
  float x = at.x;
  for (const auto& p : parts) {
    if (p.dissolved) continue;
    const float pw = cap > 0 ? w * (float)p.eighths / (float)cap : 0.0f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + pw, y + bh), p.color | 0xFF000000u);
    x += pw;
  }
  dl->AddRect(ImVec2(at.x, y), ImVec2(at.x + w, y + bh), ui::ColBronze());
  char b[64];
  std::snprintf(b, sizeof b, "%d / %d", (total + 7) / 8, cap / 8);
  y += bh + 4;
  dl->AddText(ImVec2(at.x, y), Fade(ui::ColParch(), 0.7f), b);
  y += lineH + 10;
}
}  // namespace

void AlchemyPanel(UIState& s, ImVec2 pos, ImVec2 size, const ui::PanelStyle& st,
                  ImGuiWindowFlags flags) {
  UIState::AlchemyUI& A = s.alchemy;
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(size);
  ImGui::Begin("##alchemy", nullptr, flags);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const ImVec2 ws = ImGui::GetWindowSize();
  float y = PanelChrome(dl, wp, ws, "ALCHEMY BENCH", nullptr, st);
  const float contentTop = y;
  const float lineH = ImGui::GetTextLineHeight();
  {
    const ImVec2 ts = ImGui::CalcTextSize("done");
    const float bw = std::max(96.0f, ts.x + 24);
    const float by = wp.y + kFrame + std::floor((ui::kHeaderH - ts.y - 8) * 0.5f);
    if (ui::Button("##benchdone", ImVec2(wp.x + ws.x - kFrame - 10 - bw, by), "done", false, bw))
      A.wantClose = true;
    if (ImGui::IsItemHovered())
      Tip("Put everything away: whatever is in each vessel stays in it, and whatever "
          "was spilled lands at your feet.");
  }

  // ---- the left column: tools, the vessel in hand, every vessel you carry --
  const float colX = wp.x + kFrame + kPad;
  const float colW = kBenchColW;
  const float colBottom = wp.y + ws.y - kFrame - kPad;
  {
    const float bw = (colW - 8) * 0.5f;
    if (ui::Button("##benchhand", ImVec2(colX, y), "hand", A.tool == 0, bw)) A.tool = 0;
    if (ImGui::IsItemHovered())
      Tip("Pick up any vessel on the bench and carry it; the wheel (or Q / E) tilts it. "
          "Let go and it is set down upright. Click a vessel's MOUTH to put a stopper in "
          "or take it out. Click a vessel in the list to put it on "
          "the bench or take it off - two at a time, one for each hand.");
    if (ui::Button("##benchstick", ImVec2(colX + bw + 8, y), "stick", A.tool == 1, bw)) A.tool = 1;
    if (ImGui::IsItemHovered())
      Tip("Hold the button inside a vessel: the stick goes in through its neck and follows you.");
    y += 44;
    // THE CHEMISTRY TOOLS (docs/PLAN_alchemy_chemistry.md package C).
    if (ui::Button("##benchstopper", ImVec2(colX, y), "stopper", A.tool == 2, bw)) A.tool = 2;
    if (ImGui::IsItemHovered())
      Tip("Click a vessel to put a stopper in its mouth, or take it out. Stoppered, nothing "
          "leaves it - not liquid, not powder, not gas - and it keeps its stopper in your "
          "pack. Gas building up inside will pop the stopper, or burst hot glass.");
    if (ui::Button("##benchburner", ImVec2(colX + bw + 8, y), "burner", A.tool == 3, bw)) A.tool = 3;
    if (ImGui::IsItemHovered())
      Tip("Click a vessel standing on the bench to light a flame under it, or put it out. "
          "The glass heats up and whatever touches it feels the heat: water boils, salt "
          "melts, a flammable catches.");
    y += 44;
    if (ui::Button("##benchshock", ImVec2(colX, y), "electrify", false, colW)) A.shockReq = true;
    if (ImGui::IsItemHovered())
      Tip("A jolt of lightning through the vessel in your hand (or under the pointer, or the "
          "last one you touched): what is molten or in solution may split. Molten salt "
          "gives up sodium and chlorine.");
    y += 44;
  }
  // The vessel in hand (or last touched).
  if (!A.focusName.empty()) {
    dl->AddText(ImVec2(colX, y), ui::ColGold(), FitText(A.focusName, colW).c_str());
    y += lineH + 6;
    BenchParts(dl, ImVec2(colX, y), colW, A.focusParts, A.focusCap, y);
    // Its devices, in words, and the pressure when it is stoppered.
    std::string dev;
    if (A.focusStoppered) dev += "stoppered";
    if (A.focusBurner) dev += std::string(dev.empty() ? "" : ", ") + "over a flame";
    if (A.focusHeat > 0.05f) {
      char hb[48];
      std::snprintf(hb, sizeof hb, "%sglass %s", dev.empty() ? "" : ", ",
                    A.focusHeat > 0.75f ? "very hot" : A.focusHeat > 0.35f ? "hot" : "warm");
      dev += hb;
    }
    if (!dev.empty()) {
      dl->AddText(ImVec2(colX, y), A.focusHeat > 0.35f ? ui::ColEmber() : Fade(ui::ColParch(), 0.8f),
                  FitText(dev, colW).c_str());
      y += lineH + 4;
    }
    if (A.focusStoppered) {
      // Pressure as a fraction of where the stopper gives
      // (FlaskSim::PressureFraction: 1 = SimConfig::popAt).
      const float f = std::clamp(A.focusPressure, 0.0f, 1.0f);
      const float bh = 8;
      dl->AddRectFilled(ImVec2(colX, y), ImVec2(colX + colW, y + bh), IM_COL32(30, 24, 20, 255));
      dl->AddRectFilled(ImVec2(colX, y), ImVec2(colX + colW * f, y + bh),
                        f > 0.8f ? ui::ColEmber() : ui::ColBronze());
      dl->AddRect(ImVec2(colX, y), ImVec2(colX + colW, y + bh), ui::ColBronze());
      y += bh + 4;
      dl->AddText(ImVec2(colX, y), Fade(ui::ColParch(), 0.7f), "pressure");
      y += lineH + 8;
    }
  } else {
    // Two lines: one does not fit the column.
    dl->AddText(ImVec2(colX, y), Fade(ui::ColParch(), 0.6f), "point at a vessel");
    y += lineH + 2;
    dl->AddText(ImVec2(colX, y), Fade(ui::ColParch(), 0.6f), "to see inside it");
    y += lineH + 12;
  }
  dl->AddText(ImVec2(colX, y), ui::ColGold(), "YOUR VESSELS");
  y += lineH + 6;
  // The message at the foot of the column, measured WRAPPED: placed a fixed
  // two lines up, a third line ran off the panel ("pop - the stopper
  // flies out" was cut). The rows stop above it.
  const float msgH = A.message.empty()
                         ? lineH * 2
                         : ImGui::CalcTextSize(A.message.c_str(), nullptr, false, colW).y + 4;
  // Every vessel you carry: click to put it on the bench or take it off.
  const float rowH = kSlot + 12;
  for (size_t i = 0; i < A.rows.size(); i++) {
    const auto& r = A.rows[i];
    if (y + rowH > colBottom - msgH) break;
    ImGui::SetCursorScreenPos(ImVec2(colX, y));
    ImGui::PushID((int)i);
    const bool hit = ImGui::InvisibleButton("##row", ImVec2(colW, kSlot));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (r.onTable || hov)
      dl->AddRectFilled(ImVec2(colX - 4, y - 2), ImVec2(colX + colW + 4, y + kSlot + 2),
                        Fade(ui::ColGold(), r.onTable ? 0.14f : 0.07f));
    ui::SlotSurface(dl, ImVec2(colX, y), kSlot,
                    hov ? ui::SlotLook::Hover : ui::SlotLook::Filled, r.onTable);
    const ImVec2 mid(colX + kSlot * 0.5f, y + kSlot * 0.5f);
    ui::DrawSpriteCentered(dl, ItemIcon(r.slot.kind), mid, IM_COL32_WHITE);
    DrawVesselContents(dl, mid, r.slot.fill, r.slot.fillSwatch, r.slot.fillGlow,
                       &r.slot.fillBandColor, &r.slot.fillBandFrac);
    // Two lines: what it is (and whether it is on the bench), what is in it.
    const size_t colon = r.label.find(": ");
    const std::string name = colon == std::string::npos ? r.label : r.label.substr(0, colon);
    const std::string what = colon == std::string::npos ? std::string() : r.label.substr(colon + 2);
    // Fitted to the column, not clipped by it: "(on the bench)" on a row
    // was cut off under the table. On the bench is now the row's gold wash
    // and a small tag over the icon; the whole label is the tooltip.
    const float textX = colX + kSlot + 10, textW = colX + colW - textX;
    dl->AddText(ImVec2(textX, y - 1), hov ? ui::ColGoldPale() : ui::ColParch(),
                FitText(name, textW).c_str());
    dl->AddText(ImVec2(textX, y + lineH + 1), Fade(ui::ColParch(), 0.65f), FitText(what, textW).c_str());
    if (r.onTable) {
      ImGui::PushFont(ui::FontSmall());
      const char* tag = "on bench";
      const ImVec2 ts = ImGui::CalcTextSize(tag);
      const ImVec2 t0(colX + (kSlot - ts.x) * 0.5f, y + kSlot - ts.y + 2);
      dl->AddRectFilled(ImVec2(t0.x - 3, t0.y - 1), ImVec2(t0.x + ts.x + 3, t0.y + ts.y), Fade(ui::ColInk(), 0.85f));
      dl->AddText(t0, ui::ColGold(), tag);
      ImGui::PopFont();
    }
    if (hov) Tip(r.label.c_str());
    if (hit) {
      A.wantToggle = true;
      A.toggleRef = r.ref;
    }
    y += rowH;
  }
  if (A.rows.empty()) {
    dl->AddText(ImVec2(colX, y), Fade(ui::ColParch(), 0.6f), "you carry no vessels");
    y += lineH + 6;
  }
  if (!A.message.empty()) {
    ImGui::SetCursorScreenPos(ImVec2(colX, colBottom - msgH));
    ImGui::PushTextWrapPos(colX + colW - ImGui::GetWindowPos().x);  // window-local
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::ColEmber()));
    ImGui::TextUnformatted(A.message.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
  }

  // ---- the table ------------------------------------------------------------
  // Everything right of the column. The panel reports its room so the next
  // table is sized to fill it, at an integer scale that makes a flask about
  // half the picture's height.
  const float ax0 = colX + colW + kColGap, ax1 = wp.x + ws.x - kFrame - kPad;
  const float ay0 = contentTop, ay1 = colBottom;
  A.over = A.down = A.pressed = false;
  A.tiltReq = 0.0f;
  if (A.tableW > 0 && A.tableH > 0 && A.texW > 0 && A.texH > 0 && ax1 > ax0 && ay1 > ay0) {
    const int sc = std::max(1, (int)std::floor(std::min((ax1 - ax0) / A.tableW, (ay1 - ay0) / A.tableH)));
    const float iw = (float)(A.tableW * sc), ih = (float)(A.tableH * sc);
    const ImVec2 p0((float)(int)(ax0 + (ax1 - ax0 - iw) * 0.5f), (float)(int)(ay1 - ih));
    const ImVec2 p1(p0.x + iw, p0.y + ih);
    // The desk behind the glass: a dark field in stepped bands, a lit rule
    // for the table top the vessels stand on.
    for (int band = 0; band < 8; band++) {
      const float t0 = p0.y + ih * band / 8.0f, t1 = p0.y + ih * (band + 1) / 8.0f;
      dl->AddRectFilled(ImVec2(p0.x, t0), ImVec2(p1.x, t1),
                        ui::Mix(IM_COL32(18, 15, 26, 255), IM_COL32(34, 28, 44, 255), band / 7.0f));
    }
    dl->AddRectFilled(ImVec2(p0.x, p1.y - 4 * sc), ImVec2(p1.x, p1.y), IM_COL32(58, 44, 34, 255));
    dl->AddRectFilled(ImVec2(p0.x, p1.y - 4 * sc), ImVec2(p1.x, p1.y - 3 * sc), IM_COL32(92, 70, 50, 255));
    dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), ui::ColBronze());
    // THE PICTURE IS TALLER THAN THE BOX: the sim grid has HEADROOM above the
    // table (AlchemyBench::kLiftH), and a flask lifted into it is drawn there
    // -- over the panel's header and up to the top of the screen, out of the
    // window's own clip rect. Nothing but vessels, liquid and gas is opaque
    // in the picture, so above the box the chrome shows through. Drawn after
    // the box's rule so a flask held over the edge is in front of it.
    const int gh = std::max(A.tableH, A.gridH);
    if (A.texReady && A.tex) {
      const ImVec2 g0(p0.x, p1.y - (float)(gh * sc));
      dl->PushClipRect(ImVec2(p0.x, 0.0f), p1, false);
      dl->AddImage((ImTextureID)A.tex, g0, p1, ImVec2(0, 0),
                   ImVec2((float)A.tableW / A.texW, (float)gh / A.texH));
      dl->PopClipRect();
    }

    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##benchimg", ImVec2(iw, ih));
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    const ImVec2 m = ImGui::GetIO().MousePos;
    // While the button is held the pointer keeps driving the hand wherever it
    // goes -- above the box, over the header, off the panel -- in grid
    // pixels from the table's bottom edge (the bench clamps the vessel to the
    // grid, not to the box).
    A.over = hov || act;
    A.atX = (m.x - p0.x) / sc;
    A.atY = (p1.y - m.y) / sc;
    A.down = act && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    A.pressed = hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (hov || act) A.tiltReq += ImGui::GetIO().MouseWheel * 0.10f;
    const float dt = ImGui::GetIO().DeltaTime;
    if (ImGui::IsKeyDown(ImGuiKey_Q)) A.tiltReq += 2.1f * dt;
    if (ImGui::IsKeyDown(ImGuiKey_E)) A.tiltReq -= 2.1f * dt;
    bool any = false;
    for (const auto& r : A.rows) any |= r.onTable;
    if (!any) {
      const char* t = "the bench is empty - click a vessel on the left to put it here";
      const ImVec2 ts = ImGui::CalcTextSize(t);
      dl->AddText(ImVec2(p0.x + (iw - ts.x) * 0.5f, p0.y + ih * 0.4f), Fade(ui::ColParch(), 0.8f), t);
    }
  }
  ImGui::End();
}

// ---- THE ITEM STAGE -------------------------------------------------------------
//
// The spellbook's column while an item is open on the stage (ui/item_stage.h,
// game/itemstage.h): the bench's chrome and layout -- controls and readouts
// down the left, the picture at the largest INTEGER scale that fits on the
// right, an info strip under it. main.cpp draws the picture into
// s.itemStage.tex; this reports the pointer in STAGE pixels and the gestures.
constexpr float kStageColW = 300.0f;
void ItemStagePanel(UIState& s, ImVec2 pos, ImVec2 size, const ui::PanelStyle& st,
                    ImGuiWindowFlags flags) {
  UIState::ItemStageUI& T = s.itemStage;
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(size);
  ImGui::Begin("##itemstage", nullptr, flags);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const ImVec2 ws = ImGui::GetWindowSize();
  float y = PanelChrome(dl, wp, ws, "ITEM STAGE", nullptr, st);
  const float contentTop = y;
  const float lineH = ImGui::GetTextLineHeight();
  // The last widget's screen rectangle, by id, for a harness's mouse
  // (UIState::uiRects; off in the game).
  auto rec = [&](const char* id) {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    s.RecordRect(id, a.x, a.y, b.x, b.y);
  };
  {
    const ImVec2 ts = ImGui::CalcTextSize("done");
    const float bw = std::max(96.0f, ts.x + 24);
    const float by = wp.y + kFrame + std::floor((ui::kHeaderH - ts.y - 8) * 0.5f);
    const bool done =
        ui::Button("##stagedone", ImVec2(wp.x + ws.x - kFrame - 10 - bw, by), "done", false, bw);
    rec("##stagedone");
    if (done) T.wantClose = true;
    if (ImGui::IsItemHovered()) Tip("Put it back where it was. Whatever it wears stays on it.");
  }
  const bool hasBrush = !s.applyText.empty() && !s.applyStoppered && s.applyCoats;

  // ---- the left column ---------------------------------------------------------
  const float colX = wp.x + kFrame + kPad;
  const float colW = kStageColW;
  const float colBottom = wp.y + ws.y - kFrame - kPad;
  dl->AddText(ImVec2(colX, y), ui::ColGold(), FitText(T.name, colW).c_str());
  y += lineH + 4;
  if (!T.kindText.empty()) {
    dl->AddText(ImVec2(colX, y), Fade(ui::ColParch(), 0.7f), FitText(T.kindText, colW).c_str());
    y += lineH + 2;
  }
  if (!T.where.empty()) {
    dl->AddText(ImVec2(colX, y), Fade(ui::ColParch(), 0.55f), FitText(T.where, colW).c_str());
    y += lineH + 2;
  }
  if (T.frozen && !T.coats.empty()) {
    // Said where it matters: a coat put on a blade in the pack waits there.
    dl->AddText(ImVec2(colX, y), Fade(ui::ColGoldPale(), 0.75f),
                FitText("won't dry until drawn", colW).c_str());
    y += lineH + 2;
  }
  y += 10;
  // A worn piece is one shell per part it covers; the stage shows one at a time.
  if (!T.shellNames.empty()) {
    const int n = (int)T.shellNames.size();
    T.shell = std::clamp(T.shell, 0, n - 1);
    const float bw = 40.0f;
    const bool prev = ui::Button("##stageprev", ImVec2(colX, y), "<", false, bw);
    rec("##stageprev");
    if (ImGui::IsItemHovered()) Tip("The previous part this piece covers.");
    const bool next = ui::Button("##stagenext", ImVec2(colX + colW - bw, y), ">", false, bw);
    rec("##stagenext");
    if (ImGui::IsItemHovered()) Tip("The next part this piece covers.");
    if (prev) T.shell = (T.shell + n - 1) % n;
    if (next) T.shell = (T.shell + 1) % n;
    // The part's name between the arrows, and "k / n" under it in the small
    // font, so a long part name never pushes the count off the end.
    const std::string lab = FitText(T.shellNames[(size_t)T.shell], colW - 2 * bw - 16);
    const ImVec2 ts = ImGui::CalcTextSize(lab.c_str());
    dl->AddText(ImVec2(colX + (colW - ts.x) * 0.5f, y + 4), ui::ColParch(), lab.c_str());
    char cnt[24];
    std::snprintf(cnt, sizeof cnt, "%d / %d", T.shell + 1, n);
    ImGui::PushFont(ui::FontSmall());
    const ImVec2 cs = ImGui::CalcTextSize(cnt);
    dl->AddText(ImVec2(colX + (colW - cs.x) * 0.5f, y + 4 + ts.y), Fade(ui::ColParch(), 0.6f), cnt);
    ImGui::PopFont();
    y += 50;
  }
  if (ui::Button("##stagereset", ImVec2(colX, y), "reset view", false, colW)) T.resetView = true;
  if (ImGui::IsItemHovered()) Tip("Back to the first view: the item laid across, seen from its side.");
  y += 48;
  // THE BRUSH: the vessel chosen on the FLASKS row, and the brush's size.
  dl->AddText(ImVec2(colX, y), ui::ColGold(), "BRUSH");
  y += lineH + 6;
  if (hasBrush) {
    dl->AddRectFilled(ImVec2(colX, y + 2), ImVec2(colX + 12, y + 14), s.applyColor | 0xFF000000u);
    dl->AddRect(ImVec2(colX, y + 2), ImVec2(colX + 12, y + 14), ui::ColInk());
    dl->AddText(ImVec2(colX + 20, y), ui::ColParch(), FitText(s.applyText, colW - 20).c_str());
    y += lineH + 6;
  } else if (!s.applyText.empty() && !s.applyStoppered) {
    // A flask is chosen, but what pours from it does not coat anything.
    dl->AddText(ImVec2(colX, y), Fade(ui::ColParch(), 0.6f), FitText(s.applyText, colW).c_str());
    y += lineH + 2;
    dl->AddText(ImVec2(colX, y), ui::ColEmber(), "will not coat it");
    y += lineH + 6;
  } else {
    const char* why = s.applyStoppered ? "the chosen flask is stoppered"
                                       : "choose a filled flask on the";
    dl->AddText(ImVec2(colX, y), Fade(ui::ColParch(), 0.6f), why);
    y += lineH + 2;
    if (!s.applyStoppered) {
      dl->AddText(ImVec2(colX, y), Fade(ui::ColParch(), 0.6f), "FLASKS row to coat it");
      y += lineH + 2;
    }
    y += 4;
  }
  {
    const float bw = 40.0f;
    if (ui::Button("##stagesmaller", ImVec2(colX, y), "-", false, bw))
      T.radius = std::clamp(T.radius / 1.25f, 0.5f, 24.0f);
    if (ui::Button("##stagebigger", ImVec2(colX + colW - bw, y), "+", false, bw))
      T.radius = std::clamp(T.radius * 1.25f, 0.5f, 24.0f);
    char b[48];
    std::snprintf(b, sizeof b, "size %.1f", T.radius);
    const ImVec2 ts = ImGui::CalcTextSize(b);
    dl->AddText(ImVec2(colX + (colW - ts.x) * 0.5f, y + 8), ui::ColParch(), b);
    y += 50;
  }
  // WHAT IT WEARS, heaviest coverage first (the per-limb ledger's shape).
  dl->AddText(ImVec2(colX, y), ui::ColGold(), "COATS");
  y += lineH + 6;
  if (T.coats.empty()) {
    dl->AddText(ImVec2(colX, y), Fade(ui::ColParch(), 0.6f), "clean");
    y += lineH + 4;
  }
  for (const auto& c : T.coats) {
    if (y + lineH > colBottom - lineH * 3) break;
    dl->AddRectFilled(ImVec2(colX, y + 2), ImVec2(colX + 12, y + 14), c.color | 0xFF000000u);
    dl->AddRect(ImVec2(colX, y + 2), ImVec2(colX + 12, y + 14), ui::ColInk());
    char b[32];
    std::snprintf(b, sizeof b, "%.0f%%", std::max(c.frac * 100.0f, c.voxels > 0 ? 1.0f : 0.0f));
    const ImVec2 ts = ImGui::CalcTextSize(b);
    dl->AddText(ImVec2(colX + 20, y), ui::ColParch(), FitText(c.name, colW - 20 - ts.x - 10).c_str());
    dl->AddText(ImVec2(colX + colW - ts.x, y), ui::ColGoldPale(), b);
    y += lineH + 4;
  }
  {
    ImGui::PushFont(ui::FontSmall());
    const char* help = hasBrush ? "left: pour  .  right-drag: turn  .  wheel: zoom  .  shift+wheel: size"
                                : "drag: turn  .  wheel: zoom";
    ImGui::SetCursorScreenPos(ImVec2(colX, colBottom - lineH * 2.4f));
    ImGui::PushTextWrapPos(colX + colW - ImGui::GetWindowPos().x);  // window-local
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Fade(ui::ColParch(), 0.55f)));
    ImGui::TextUnformatted(help);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
  }

  // ---- the picture ------------------------------------------------------------
  const float stripH = lineH * 2 + 14;
  const float ax0 = colX + colW + kColGap, ax1 = wp.x + ws.x - kFrame - kPad;
  const float ay0 = contentTop, ay1 = colBottom - stripH;
  // The room, for main.cpp to size next frame's picture to (an integer
  // scale of at least 2 that fills it).
  T.areaW = std::max(0.0f, ax1 - ax0);
  T.areaH = std::max(0.0f, ay1 - ay0);
  T.over = T.paint = false;
  if (T.imgW > 0 && T.imgH > 0 && ax1 > ax0 && ay1 > ay0) {
    const int sc = std::max(1, (int)std::floor(std::min((ax1 - ax0) / T.imgW, (ay1 - ay0) / T.imgH)));
    const float iw = (float)(T.imgW * sc), ih = (float)(T.imgH * sc);
    const ImVec2 p0((float)(int)(ax0 + (ax1 - ax0 - iw) * 0.5f), (float)(int)(ay0 + (ay1 - ay0 - ih) * 0.5f));
    const ImVec2 p1(p0.x + iw, p0.y + ih);
    // The cloth the item lies on: a dark field in stepped bands (the bench's
    // desk), lit a little toward the middle.
    for (int band = 0; band < 8; band++) {
      const float t0 = p0.y + ih * band / 8.0f, t1 = p0.y + ih * (band + 1) / 8.0f;
      const float k = 1.0f - std::fabs(band - 3.5f) / 3.5f;
      dl->AddRectFilled(ImVec2(p0.x, t0), ImVec2(p1.x, t1),
                        ui::Mix(IM_COL32(16, 14, 22, 255), IM_COL32(40, 33, 48, 255), k));
    }
    dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), ui::ColBronze());
    if (T.texReady && T.tex)
      dl->AddImage((ImTextureID)T.tex, p0, p1, ImVec2(0, 0),
                   ImVec2((float)T.imgW / T.texW, (float)T.imgH / T.texH));
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##stageimg", ImVec2(iw, ih),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    rec("##stageimg");
    T.imgX = p0.x;
    T.imgY = p0.y;
    T.imgScale = (float)sc;
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    T.over = hov || act;
    T.at[0] = (m.x - p0.x) / sc;
    T.at[1] = (m.y - p0.y) / sc;
    // Turn it: right-drag always, left-drag when there is no brush.
    const bool orbit = act && (ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                               (!hasBrush && ImGui::IsMouseDown(ImGuiMouseButton_Left)));
    if (orbit) {
      T.yaw += io.MouseDelta.x * 0.012f;
      T.pitch = std::clamp(T.pitch + io.MouseDelta.y * 0.012f, -1.55f, 1.55f);
    }
    T.paint = hasBrush && act && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (hov) {
      float grow = 0.0f;
      if (io.KeyShift) grow = io.MouseWheel;
      else if (io.MouseWheel != 0.0f)
        T.zoom = std::clamp(T.zoom * std::pow(1.15f, io.MouseWheel), 0.5f, 8.0f);
      if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) grow -= 1.0f;
      if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) grow += 1.0f;
      if (grow != 0.0f) T.radius = std::clamp(T.radius * std::pow(1.25f, grow), 0.5f, 24.0f);
    }
    // The brush ring where the next pour lands (main.cpp's pick).
    if (hasBrush && T.over && T.cursorValid) {
      const ImVec2 c(p0.x + T.cursorPx[0] * sc, p0.y + T.cursorPx[1] * sc);
      const float r = std::max(2.0f, T.cursorR * sc);
      dl->AddCircle(c, r + 1, ui::ColInk(), 0, 3.0f);
      dl->AddCircle(c, r, s.applyColor ? (s.applyColor | 0xFF000000u) : ui::ColGold(), 0, 1.5f);
    }
    // ---- the info strip --------------------------------------------------------
    float sy = p1.y + 8;
    std::string head = T.name;
    if (!T.shellNames.empty()) head += " - " + T.shellNames[(size_t)std::clamp(T.shell, 0, (int)T.shellNames.size() - 1)];
    char vb[48];
    std::snprintf(vb, sizeof vb, "  (%d voxels)", T.voxels);
    head += vb;
    dl->AddText(ImVec2(p0.x, sy), ui::ColGoldPale(), FitText(head, iw).c_str());
    sy += lineH + 2;
    std::string sum;
    if (T.coats.empty()) sum = "clean";
    for (size_t i = 0; i < T.coats.size() && i < 3; i++) {
      char b[96];
      std::snprintf(b, sizeof b, "%s%s %.0f%%", i ? ",  " : "", T.coats[i].name.c_str(),
                    std::max(T.coats[i].frac * 100.0f, 1.0f));
      sum += b;
    }
    if (T.coats.size() > 3) sum += ",  ...";
    dl->AddText(ImVec2(p0.x, sy), Fade(ui::ColParch(), 0.8f), FitText(sum, iw).c_str());
  }
  ImGui::End();
}

void LootPanel(UIState& s, ImVec2 pos, ImVec2 size, const ui::PanelStyle& st,
               ImGuiWindowFlags flags) {
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(size);
  ImGui::Begin("##loot", nullptr, flags);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    const std::string title = "LOOT: " + s.lootTitle;
    // No subtitle: the header bar's right half belongs to the two buttons.
    float y = PanelChrome(dl, wp, ws, title.c_str(), nullptr, st);
    // Two header-bar buttons, right-aligned, sized off their labels the way
    // the character panel's gear/health toggle is (the pixel font is wide).
    {
      const ImVec2 tsClose = ImGui::CalcTextSize("close");
      const ImVec2 tsAll = ImGui::CalcTextSize("take all");
      const float by = wp.y + kFrame + std::floor((ui::kHeaderH - tsClose.y - 8) * 0.5f);
      const float bwClose = std::max(96.0f, tsClose.x + 24);
      const float bwAll = std::max(96.0f, tsAll.x + 24);
      float bx = wp.x + ws.x - kFrame - 10 - bwClose;
      if (ui::Button("##lootclose", ImVec2(bx, by), "close", false, bwClose))
        s.lootClose = true;
      if (ImGui::IsItemHovered()) Tip("Leave the rest where it lies.");
      if (!s.lootSlots.empty()) {
        bx -= bwAll + 8;
        if (ui::Button("##lootall", ImVec2(bx, by), "take all", false, bwAll)) {
          s.takeLoot.pending = true;
          s.takeLoot.index = -1;
          s.takeLoot.all = true;
        }
        if (ImGui::IsItemHovered())
          Tip("Everything into your pack, until it is full.");
      }
    }
    // The grid: as many columns as the panel is wide, one row per that many.
    const int cols = std::max(1, (int)((ws.x - kPad * 2 + kSlotGap) / (kSlot + kSlotGap)));
    const int n = (int)s.lootSlots.size();
    for (int i = 0; i < n; i++) {
      const int r = i / cols, c = i % cols;
      const float gx = wp.x + kPad + c * (kSlot + kSlotGap);
      const float gy = y + r * (kSlot + kSlotGap);
      if (gy + kSlot > wp.y + ws.y - kPad) break;
      char id[32];
      std::snprintf(id, sizeof id, "loot%d", i);
      ItemSlot(s, id, ImVec2(gx, gy), s.lootSlots[i], KitRef{KitSpace::Loot, i},
               nullptr, true, nullptr, false);
    }
    if (n == 0) {
      ImGui::PushFont(ui::FontSmall());
      dl->AddText(ImVec2(wp.x + kPad, y + 4), Fade(ui::ColParchDim(), 0.8f),
                  "Picked clean.");
      ImGui::PopFont();
    }
  }
  ImGui::End();
}

// ---- THE VITALS COLUMN ------------------------------------------------------
//
// The right-hand column of the character panel, live for as long as the health
// view is on. It is a WHOLE COLUMN and not a footnote under the portrait:
//
//   VITALS   how much of this body is still physically here, and every word
//            that would change what you do next
//   TRIAGE   the six parts of a person, worst first, each one summarising
//            everything wrong inside it and opening onto its own segments
//   THE LIMB the selected part in full — what it is made of, what happened to
//            it, what is on it, and what is worn over it
//
// Health and mana are NOT in this column: they stay where they are in the
// collapsed view, beside the gear, so opening the health column never moves
// the two bars it is named after.
//
// AND THE GEAR STAYS ON SCREEN BESIDE IT. Health used to be a separate page
// behind a toggle, so reading "what is hurt" meant losing sight of "what is
// protecting it" — the one comparison a character screen exists to make. The
// toggle now only adds this column and the portrait's callouts; it takes
// nothing away.
//
// The old INJURIES list is gone with it. It was a second, worse rendering of
// what TRIAGE and THE LIMB now say between them: every limb listed whether or
// not anything was wrong, two bars each, and the detail column repeating all
// of it the moment you clicked one.
void VitalsColumn(UIState& s, ImVec2 at, ImVec2 size) {
  ImGui::SetCursorScreenPos(at);
  ImGui::BeginChild("##vitals", size, ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Taken AFTER BeginChild, so it already carries the child's scroll offset:
  // everything below is laid out from `base`, which is what lets a column of
  // absolutely-positioned drawing scroll at all.
  const ImVec2 base = ImGui::GetCursorScreenPos();
  const float w = std::max(140.0f, ImGui::GetContentRegionAvail().x);
  const float pulse = AlarmPulse();
  float y = base.y;

  // The small font is this column's body face: it is a 260 px column and the
  // screen's 26 px display face fits about three words across it.
  auto small = [&](ImU32 col, ImVec2 p, const char* text) {
    ImGui::PushFont(ui::FontSmall());
    ui::ShadowText(dl, p, col, text);
    ImGui::PopFont();
  };

  // ======================= THE DEATH HOLD =====================================
  // Above VITALS, because when it is up it is the only thing on this column
  // that is about the present tense: everything below it is a photograph of
  // the body at the instant it died (UIState::deathScreen). The button is the
  // ONLY way back — there is no respawn timer running behind this panel — so
  // it is drawn full width and cannot be mistaken for a chip.
  if (s.deathScreen) {
    const float bannerH = 42.0f;
    dl->AddRectFilled(ImVec2(base.x, y), ImVec2(base.x + w, y + bannerH),
                      Fade(ui::ColBloodHi(), 0.18f));
    dl->AddRectFilled(ImVec2(base.x, y), ImVec2(base.x + w, y + 2),
                      Fade(ui::ColBloodHi(), 0.85f));
    dl->AddRectFilled(ImVec2(base.x, y + bannerH - 2),
                      ImVec2(base.x + w, y + bannerH),
                      Fade(ui::ColBloodHi(), 0.85f));
    {
      const char* dead = "YOU DIED";
      const ImVec2 ts = ImGui::CalcTextSize(dead);
      ui::ShadowText(dl,
                     ImVec2(std::floor(base.x + (w - ts.x) * 0.5f),
                            std::floor(y + (bannerH - ts.y) * 0.5f)),
                     Fade(ui::ColBloodHi(), 0.55f + 0.45f * pulse), dead);
    }
    y += bannerH + 4;
    // THE CAUSE, in the engine's own words, on its own line. It is the one
    // thing on this column that is not derivable from the limbs below it: four
    // mechanisms (blood loss, a vital limb gone, the burn cap, a limb burnt
    // away) all end in the same corpse, and only Die() knows which fired.
    ImGui::PushFont(ui::FontSmall());
    {
      char buf[96];
      if (!s.deathCause.empty())
        std::snprintf(buf, sizeof buf, "%s", s.deathCause.c_str());
      else
        std::snprintf(buf, sizeof buf, "cause unrecorded");
      small(ui::ColBloodHi(), ImVec2(base.x, y), buf);
      y += ImGui::GetTextLineHeight() + 2;
      std::snprintf(buf, sizeof buf,
                    "everything below is this body %.1f s ago, at death",
                    s.deathHoldSec);
      small(Fade(ui::ColParchDim(), 0.85f), ImVec2(base.x, y), buf);
      y += ImGui::GetTextLineHeight() + 6;
    }
    ImGui::PopFont();
    // The wait is tune.avatar.respawnDelay, and it is drawn as a disabled
    // button counting down rather than as no button at all — a control that
    // appears out of nowhere is a control you press by accident.
    const float wait = s.deathRespawnAfter - s.deathHoldSec;
    const bool ready = wait <= 0.0f;
    char blabel[32];
    if (ready) std::snprintf(blabel, sizeof blabel, "respawn");
    else std::snprintf(blabel, sizeof blabel, "respawn  %.0f", std::ceil(wait));
    if (!ready) ImGui::BeginDisabled();
    if (ui::Button("##respawn", ImVec2(base.x, y), blabel, false, w) && ready)
      s.respawnRequest = true;
    if (!ready) {
      ImGui::EndDisabled();
      dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                        Fade(ui::ColInk(), 0.45f));
    }
    if (ImGui::IsItemHovered()) {
      Tip("Build a new body and put it back in the world. Nothing here is\n"
          "kept: this is the last state of the body that died, and it is\n"
          "replaced the moment a new one exists.\n"
          "Nothing is on a timer: close this screen and look the body over\n"
          "first if you want. The readout is frozen and will still be here.");
    }
    y = ImGui::GetItemRectMax().y + 10;
  }

  // ======================= THE BODY ITSELF ====================================
  // Health and mana are NOT here: they stay in the character panel beside the
  // gear, in the same place whether this column is open or shut. What this
  // section adds is the thing a pool bar cannot say — how much of the body is
  // physically still present, and every word that would change what you do
  // next.
  y = ui::Subheading(dl, ImVec2(base.x, y), w, "VITALS") + 4;

  // WHAT IS STILL THERE. Integrity is live voxels over voxels at spawn, summed
  // across the body. The spawn count is not mirrored per limb — but voxelFrac
  // IS that ratio, so the denominator comes back out of it. A limb with no
  // fraction contributes to neither side rather than dividing by zero, and a
  // SEVERED limb is counted in its own tally below instead: its spawn size is
  // not knowable from a part that no longer exists.
  float liveVox = 0.0f, bornVox = 0.0f;
  int bleeders = 0, severedN = 0;
  uint32_t burningVox = 0;
  for (int i = 0; i < UIState::kSlotCount; i++) {
    const UIState::BodyPartUI& b = s.body[i];
    if (!b.present) continue;
    if (b.severed) { severedN++; continue; }
    if (b.bleeding) bleeders++;
    burningVox += b.burningVoxels;
    if (b.voxelTotal > 0 && b.voxelFrac > 0.01f) {
      liveVox += (float)b.voxelTotal;
      bornVox += (float)b.voxelTotal / b.voxelFrac;
    }
  }
  const float integrity = bornVox > 0.0f ? std::clamp(liveVox / bornVox, 0.0f, 1.0f)
                                         : 1.0f;
  {
    char buf[48];
    std::snprintf(buf, sizeof buf, "BODY INTACT  %.0f%%", integrity * 100.0f);
    const ImVec2 a(base.x, y), b2(base.x + w, y + 16);
    ui::ValueBar(dl, a, b2, integrity, ui::ColSteel(), false);
    ImGui::PushFont(ui::FontSmall());
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    ui::ShadowText(dl, ImVec2(a.x + 8, std::floor(a.y + (16 - ts.y) * 0.5f)),
                   ui::ColGoldPale(), buf);
    ImGui::PopFont();
    y += 16 + 6;
  }

  // ---- the status ribbon: every word that changes what you would do next ----
  {
    ImGui::PushFont(ui::FontSmall());
    float cx = base.x, cy = y;
    float lineH = ImGui::GetTextLineHeight();
    bool anyChip = false;
    auto chip = [&](ImU32 col, bool flash, const char* fmt, ...) {
      char buf[64];
      va_list ap;
      va_start(ap, fmt);
      std::vsnprintf(buf, sizeof buf, fmt, ap);
      va_end(ap);
      const ImVec2 ts = ImGui::CalcTextSize(buf);
      if (anyChip && cx + ts.x + 8 > base.x + w) {
        cx = base.x;
        cy += ts.y + 6;
      }
      anyChip = true;
      const ImU32 c = flash ? Fade(col, 0.55f + 0.45f * pulse) : col;
      dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + ts.x + 8, cy + ts.y),
                        Fade(ui::ColInk(), 0.72f));
      dl->AddRectFilled(ImVec2(cx, cy + ts.y - 2),
                        ImVec2(cx + ts.x + 8, cy + ts.y), Fade(c, 0.85f));
      ui::ShadowText(dl, ImVec2(cx + 4, cy), c, buf);
      cx += ts.x + 14;
      lineH = ts.y;
    };
    if (!s.playerAlive) chip(ui::ColBloodHi(), true, "DEAD");
    if (bleeders > 0) chip(ui::ColBloodHi(), true, "BLEEDING x%d", bleeders);
    if (burningVox > 0) chip(ui::ColEmber(), true, "ON FIRE %u", burningVox);
    if (severedN > 0) chip(ui::ColBloodHi(), false, "LOST x%d", severedN);
    if (s.healthCap > 0 && s.healthCap < s.healthMax)
      chip(ui::ColChar(), false, "BURNT CAP %d", s.healthCap);
    if (s.stainFrac >= s.stainHudMin && s.stainColor != 0) {
      // One chip per substance (UIState::coats), not one for the heaviest.
      int named = 0;
      for (int ci = 0; ci < s.coatCount; ci++) {
        const UIState::CoatName& c = s.coats[ci];
        if (c.color == 0 || c.frac < 0.005f) continue;
        char up[sizeof c.label];
        size_t k = 0;
        for (; k + 1 < sizeof up && c.label[k]; k++)
          up[k] = (char)std::toupper((unsigned char)c.label[k]);
        up[k] = '\0';
        chip(ui::Mix(c.color, IM_COL32_WHITE, 0.35f), false, "%s %.0f%%",
             k ? up : "COATED", c.frac * 100.0f);
        named++;
      }
      if (named == 0)
        chip(ui::Mix(s.stainColor, IM_COL32_WHITE, 0.35f), false,
             "COATED %.0f%%", s.stainFrac * 100.0f);
    }
    if (!s.locoState.empty())
      chip(ui::ColParchDim(), false, "%s", s.locoState.c_str());
    if (!anyChip) chip(ui::ColParchDim(), false, "steady");
    ImGui::PopFont();
    y = cy + lineH + 10;
  }

  // ======================= TRIAGE =============================================
  //
  // WHAT IS WRONG, WORST FIRST — BY THE PART OF THE BODY A PERSON ACTUALLY
  // THINKS IN. Six rows: head, torso, left arm, right arm, left leg, right
  // leg. Each one carries everything wrong anywhere inside it, and each one
  // OPENS onto its own segments when the summary is not enough.
  //
  // Two regroupings got it here, and both were the same mistake at different
  // scales. The first version pushed one row per (limb, effect) PAIR sorted by
  // severity, so a burning, bleeding, critical arm appeared three separate
  // times with its three lines split apart by other limbs' rows. The second
  // fixed that — one row per limb — and still listed "Left forearm", "Left
  // upper arm" and "Left hand" as three peers, which is not how a body is
  // read: you decide the LEFT ARM is the problem first, and only then care
  // which third of it. The group is the subject; the segment is a detail you
  // ask for.
  //
  // Every row is a button. A segment row selects that limb — lighting its
  // callout on the portrait, lighting the armour covering it, and filling the
  // section below. A group row opens the group, and selects the worst segment
  // in it if the selection is not already inside. That is the whole navigation
  // model of this column.
  y = ui::Subheading(dl, ImVec2(base.x, y), w, "TRIAGE") + 4;
  {
    // THE SIX PARTS OF A PERSON. Torso is chest + hips, because "the hips are
    // at 60%" is a sentence about the torso; the segments are still one click
    // away. Order is the anatomical one, used verbatim for groups that have
    // nothing to report — the severity sort only has to move the hurt ones.
    struct Group { const char* name; int n; int slot[3]; };
    static const Group kGroups[] = {
        {"HEAD", 1, {UIState::kSlotHead, -1, -1}},
        {"TORSO", 2, {UIState::kSlotTorso, UIState::kSlotHips, -1}},
        {"LEFT ARM", 3,
         {UIState::kSlotArmUL, UIState::kSlotArmLL, UIState::kSlotHandL}},
        {"RIGHT ARM", 3,
         {UIState::kSlotArmUR, UIState::kSlotArmLR, UIState::kSlotHandR}},
        {"LEFT LEG", 3,
         {UIState::kSlotLegUL, UIState::kSlotLegLL, UIState::kSlotFootL}},
        {"RIGHT LEG", 3,
         {UIState::kSlotLegUR, UIState::kSlotLegLR, UIState::kSlotFootR}},
    };
    const int kGroupN = (int)(sizeof kGroups / sizeof kGroups[0]);

    struct Effect { int sev; ImU32 col; char text[28]; };
    struct Line { int n; int sev; Effect eff[6]; };
    auto add = [](Line& r, int sev, ImU32 col, const char* fmt, ...) {
      if (r.n >= (int)(sizeof r.eff / sizeof r.eff[0])) return;
      Effect& e = r.eff[r.n++];
      e.sev = sev;
      e.col = col;
      va_list ap;
      va_start(ap, fmt);
      std::vsnprintf(e.text, sizeof e.text, fmt, ap);
      va_end(ap);
      if (sev < r.sev) r.sev = sev;
    };
    // ONE SUMMARY SHAPE FOR A LIMB AND FOR A WHOLE GROUP. A group is summed
    // and worst-cased into the same handful of numbers a single limb reports,
    // so the group line and the segment lines under it are built by the same
    // code and cannot drift into saying different things about the same arm.
    struct Agg {
      int present = 0, severed = 0, bleeders = 0;
      uint32_t burning = 0;
      float hp = 1.0f, vox = 1.0f, charred = 0.0f, rot = 0.0f;
      char rotLabel[16] = "rot";
      ImU32 rotColor = IM_COL32(150, 190, 100, 255);
    };
    auto fold = [](Agg& a, const UIState::BodyPartUI& b) {
      if (!b.present) return;
      a.present++;
      if (b.severed) { a.severed++; return; }
      if (b.bleeding) a.bleeders++;
      a.burning += b.burningVoxels;
      a.hp = std::min(a.hp, b.hpFrac);
      a.vox = std::min(a.vox, b.voxelFrac);
      a.charred = std::max(a.charred, b.charredFrac);
      // WORST SEGMENT, not the group's average. A hand three-quarters turned
      // inside an otherwise clean arm is the thing worth knowing; averaging it
      // against two healthy segments would report a calm 25% for an arm with a
      // dead hand on the end of it. Same rule charredFrac already uses.
      if (b.voxelTotal > 0) {
        const float f = (float)b.voxelRot / (float)b.voxelTotal;
        if (f > a.rot) {
          a.rot = f;
          // Named by the worst segment's HEAVIEST infection (its first row).
          if (b.infectCount > 0) {
            std::snprintf(a.rotLabel, sizeof a.rotLabel, "%s", b.infect[0].label);
            a.rotColor = b.infect[0].color;
          }
        }
      }
    };
    // Severity is the ORDER WITHIN a line as well as the line's own rank, so
    // the first words on it are always the worst news about that part.
    auto build = [&](const Agg& a, Line& r) {
      r.n = 0;
      r.sev = 99;
      if (a.present == 0) return;
      if (a.severed >= a.present) {
        add(r, 0, ui::ColBloodHi(), a.present > 1 ? "ALL GONE" : "SEVERED");
      } else {
        if (a.severed > 0) add(r, 0, ui::ColBloodHi(), "SEVERED x%d", a.severed);
        if (a.burning > 0) add(r, 1, ui::ColEmber(), "BURNING %u", a.burning);
        if (a.bleeders > 1)
          add(r, 2, ui::ColBloodHi(), "BLEEDING x%d", a.bleeders);
        else if (a.bleeders == 1)
          add(r, 2, ui::ColBloodHi(), "BLEEDING");
        // ROT RANKS ABOVE "CRITICAL" AT ANY AMOUNT ABOVE A SPECK. It is the
        // only line here that describes something still SPREADING — hp and
        // charred are both verdicts on damage already done — so a 4% arm is a
        // warning, not a footnote, and it reads before the percentages.
        // The floor is one part in fifty rather than zero because a single
        // grazing bite leaves a handful of voxels and a body that has fought
        // anything undead would otherwise wear "ROTTING 0%" on six rows.
        if (a.rot > 0.02f) {
          char up[16];
          size_t n = 0;
          for (; n + 1 < sizeof up && a.rotLabel[n]; n++)
            up[n] = (char)std::toupper((unsigned char)a.rotLabel[n]);
          up[n] = '\0';
          add(r, 2, a.rotColor, "%s %.0f%%", up, a.rot * 100.0f);
        }
        if (a.hp < 0.35f)
          add(r, 3, ui::ColBloodHi(), "CRITICAL %.0f%%", a.hp * 100.0f);
        else if (a.hp < 0.8f)
          add(r, 5, ui::ColBlood(), "hurt %.0f%%", a.hp * 100.0f);
        if (a.vox < 0.6f)
          add(r, 4, ui::ColSteel(), "HOLLOW %.0f%%", a.vox * 100.0f);
        if (a.charred > 0.25f)
          add(r, 6, ui::ColEmber(), "charred %.0f%%", a.charred * 100.0f);
      }
      // A PART WITH NOTHING WRONG STILL GETS A LINE, and the line says so.
      // The old column dropped healthy limbs entirely, which was right when it
      // was a flat list of fifteen symptoms and is wrong now: the six groups
      // are also the NAVIGATION, and an intact body has to be steerable too.
      if (r.n == 0) add(r, 90, Fade(ui::ColParchDim(), 0.8f), "ok");
      std::stable_sort(r.eff, r.eff + r.n,
                       [](const Effect& x, const Effect& z) { return x.sev < z.sev; });
    };

    struct GRow { int gi; Line line; int worst; int members; };
    GRow groups[8];
    int ng = 0;
    for (int gi = 0; gi < kGroupN; gi++) {
      Agg a;
      int worst = -1;
      float worstScore = 1e9f;
      int members = 0;
      for (int k = 0; k < kGroups[gi].n; k++) {
        const int sl = kGroups[gi].slot[k];
        if (sl < 0) continue;
        const UIState::BodyPartUI& b = s.body[sl];
        if (!b.present) continue;
        members++;
        fold(a, b);
        float score = b.severed ? -2.0f : std::min(b.hpFrac, b.voxelFrac);
        if (b.burningVoxels > 0) score -= 1.0f;
        if (b.bleeding) score -= 0.5f;
        // A rotting segment is the one to open the group on, even at full hp:
        // rot leaves hpFrac and voxelFrac untouched until it starts deleting
        // voxels, so without this the group row would say ROTTING and then
        // select a clean segment that shows none of it.
        if (b.voxelTotal > 0)
          score -= (float)b.voxelRot / (float)b.voxelTotal;
        if (score < worstScore) { worstScore = score; worst = sl; }
      }
      if (members == 0) continue;  // a rig without this part simply has none
      GRow& g = groups[ng++];
      g.gi = gi;
      g.worst = worst;
      g.members = members;
      build(a, g.line);
    }
    std::stable_sort(groups, groups + ng, [](const GRow& a, const GRow& b) {
      return a.line.sev < b.line.sev;
    });
    if (ng == 0) {
      small(Fade(ui::ColParchDim(), 0.85f), ImVec2(base.x + 4, y),
            "No body to inspect.");
      ImGui::PushFont(ui::FontSmall());
      y += ImGui::GetTextLineHeight() + 8;
      ImGui::PopFont();
    }

    ImGui::PushFont(ui::FontSmall());
    const float lineH = ImGui::GetTextLineHeight();
    // The separator is DRAWN, not typed: the 13 px pixel face has no middle
    // dot, and a fallback box between every effect is exactly the row of
    // hieroglyphs the tissue table above already had to be rid of once.
    const float sepW = 9.0f;
    const float kCaretW = 11.0f;  // the disclosure triangle's gutter
    const float kIndent = 10.0f;  // how far a segment hangs under its group
    // The name column, sized to the longest name actually being shown — group
    // names with their caret gutter, segment names with their indent — and
    // capped so a long label cannot squeeze the effects into one word a line.
    float labelW = 0.0f;
    for (int i = 0; i < ng; i++) {
      const Group& G = kGroups[groups[i].gi];
      labelW = std::max(labelW, kCaretW + ImGui::CalcTextSize(G.name).x);
      for (int k = 0; k < G.n; k++) {
        if (G.slot[k] < 0 || !s.body[G.slot[k]].present) continue;
        labelW = std::max(labelW,
                          kIndent + ImGui::CalcTextSize(s.body[G.slot[k]].label).x);
      }
    }
    labelW = std::min(labelW, w * 0.46f);
    const float tx0 = base.x + 9 + labelW + 8;
    const float right = base.x + w - 4;
    // Flow the effects left to right and wrap onto a continuation line that
    // hangs under the first one, never back under the name: the indent is what
    // keeps a three-effect part reading as one entry.
    auto flow = [&](const Line& r, float top, ImDrawList* out) {
      float cx = tx0, cy = top;
      for (int k = 0; k < r.n; k++) {
        const float tw = ImGui::CalcTextSize(r.eff[k].text).x;
        const float lead = k ? sepW : 0.0f;
        if (cx > tx0 && cx + lead + tw > right) {
          cx = tx0;
          cy += lineH;
        } else if (k) {
          if (out) {
            const float dy = std::floor(cy + lineH * 0.5f) - 1.0f;
            out->AddRectFilled(ImVec2(cx + 3.0f, dy), ImVec2(cx + 5.0f, dy + 2.0f),
                               Fade(ui::ColParchDim(), 0.55f));
          }
          cx += sepW;
        }
        if (out) {
          const ImU32 c = r.eff[k].sev <= 2
                              ? Fade(r.eff[k].col, 0.55f + 0.45f * pulse)
                              : r.eff[k].col;
          ui::ShadowText(out, ImVec2(cx, cy), c, r.eff[k].text);
        }
        cx += tw;
      }
      return cy - top + lineH;
    };
    // One row of the list, group or segment. `label` is drawn in the name
    // column at `indent`, the effects flow in the text column, and the WHOLE
    // width is the button — a row clickable only on its words is a row that
    // gets missed.
    auto drawRow = [&](const Line& r, const char* label, float indent, bool sel,
                       bool dim, int id) {
      const float rowH = flow(r, y + 2, nullptr) + 4;
      ImGui::SetCursorScreenPos(ImVec2(base.x, y));
      ImGui::PushID(id);
      ImGui::InvisibleButton("##row", ImVec2(w, rowH));
      const bool hit = ImGui::IsItemClicked();
      const bool hover = ImGui::IsItemHovered();
      ImGui::PopID();
      if (sel || hover)
        dl->AddRectFilled(ImVec2(base.x, y), ImVec2(base.x + w, y + rowH),
                          Fade(ui::ColGold(), sel ? 0.16f : 0.08f));
      // A 3 px severity stripe down the left edge: the column can be read for
      // "how bad is this body" without reading a word of it. It runs the WHOLE
      // row, so a part with three effects reads as one taller alarm.
      const ImU32 sc = r.eff[0].col;
      dl->AddRectFilled(ImVec2(base.x, y), ImVec2(base.x + 3, y + rowH),
                        r.sev <= 2 ? Fade(sc, 0.55f + 0.45f * pulse)
                                   : Fade(sc, r.sev >= 90 ? 0.35f : 0.75f));
      const float lx = base.x + 9 + indent;
      dl->PushClipRect(ImVec2(lx, y), ImVec2(tx0 - 4, y + rowH), true);
      ui::ShadowText(dl, ImVec2(lx, y + 2),
                     sel ? ui::ColGoldHi()
                         : dim ? Fade(ui::ColParch(), 0.75f) : ui::ColParch(),
                     label);
      dl->PopClipRect();
      flow(r, y + 2, dl);
      y += rowH;
      return hit;
    };

    for (int i = 0; i < ng; i++) {
      const GRow& g = groups[i];
      const Group& G = kGroups[g.gi];
      const bool splits = g.members > 1;
      const bool open = !splits || ((s.triageOpen >> g.gi) & 1) != 0;
      // The group is SELECTED when the selection is anywhere inside it, so the
      // summary line stays lit while one of its segments is being read.
      bool inside = false;
      for (int k = 0; k < G.n; k++)
        if (G.slot[k] >= 0 && G.slot[k] == s.inspectSelected) inside = true;
      if (splits) {
        // The disclosure triangle, drawn rather than typed for the same reason
        // the separator is: this face has no glyph for it.
        const float cx = base.x + 11, cy = y + 2 + std::floor(lineH * 0.5f);
        const ImU32 cc = inside ? ui::ColGoldHi() : Fade(ui::ColParchDim(), 0.9f);
        if (open)
          dl->AddTriangleFilled(ImVec2(cx - 3, cy - 2), ImVec2(cx + 3, cy - 2),
                                ImVec2(cx, cy + 3), cc);
        else
          dl->AddTriangleFilled(ImVec2(cx - 2, cy - 3), ImVec2(cx - 2, cy + 3),
                                ImVec2(cx + 3, cy), cc);
      }
      if (drawRow(g.line, G.name, kCaretW, inside, false, 6000 + g.gi)) {
        if (splits) s.triageOpen ^= 1u << g.gi;
        // Clicking a group still has to PICK something — the portrait callout
        // and the section below are both driven by the selection — but it must
        // not yank the selection off a segment of this same group that the
        // group was opened to read.
        if (!inside && g.worst >= 0) s.inspectSelected = g.worst;
      }
      if (!open) continue;
      // OPEN: every segment of the group, injured or not. A hand that is fine
      // is still the thing to click to read what a hand is made of and what is
      // worn over it. A one-part group (a head) is its own segment and is not
      // listed twice.
      if (!splits) continue;
      for (int k = 0; k < G.n; k++) {
        const int sl = G.slot[k];
        if (sl < 0 || !s.body[sl].present) continue;
        Agg a;
        fold(a, s.body[sl]);
        Line r;
        build(a, r);
        if (drawRow(r, s.body[sl].label, kIndent, s.inspectSelected == sl,
                    r.sev >= 90, 6100 + sl))
          s.inspectSelected = sl;
      }
    }
    ImGui::PopFont();
    y += 8;
  }

  // ======================= THE SELECTED LIMB ==================================
  const bool haveLimb = s.inspectSelected >= 0 &&
                        s.inspectSelected < UIState::kSlotCount &&
                        s.body[s.inspectSelected].present;
  if (!haveLimb) {
    y = ui::Subheading(dl, ImVec2(base.x, y), w, "A PART") + 4;
    ImGui::PushFont(ui::FontSmall());
    ImGui::SetCursorScreenPos(ImVec2(base.x + 4, y));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(Fade(ui::ColParchDim(), 0.85f)));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 8);
    ImGui::TextUnformatted("Click a limb on the portrait, or a line above, to open it: "
                           "what it is made of, what happened to it, and what you have "
                           "on over it.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    y = ImGui::GetItemRectMax().y + 6;
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(base);
    ImGui::Dummy(ImVec2(w, y - base.y));
    ImGui::EndChild();
    return;
  }

  const UIState::BodyPartUI& b = s.body[s.inspectSelected];
  const float kBarW = std::min(w - 120.0f, 120.0f);

  // The limb's own header: its name in small caps on a bronze rule, so the
  // section reads as part of the same column rather than as a popup that
  // happened to land in it.
  {
    char up[40];
    size_t k = 0;
    for (; k + 1 < sizeof up && b.label[k]; k++)
      up[k] = (char)std::toupper((unsigned char)b.label[k]);
    up[k] = '\0';
    y = ui::Subheading(dl, ImVec2(base.x, y), w, up) + 4;
  }

  if (b.severed) {
    ImGui::PushFont(ui::FontSmall());
    ui::ShadowText(dl, ImVec2(base.x + 4, y),
                   Fade(ui::ColBloodHi(), 0.6f + 0.4f * pulse),
                   "SEVERED - this part is gone.");
    y += ImGui::GetTextLineHeight() + 6;
    ImGui::PopFont();
  } else {
    // ---- TISSUE: what the limb IS right now --------------------------------
    //
    // NO PRINTF PADDING ON THE NAME. This read "%-8s" once, which padded
    // "skin" out with four trailing spaces — and the 13 px pixel face has no
    // glyph for them, so the atlas's fallback box got drawn four times and
    // every tissue row grew a little row of hieroglyphs. The column offsets
    // below already do the aligning; the padding was never load-bearing.
    auto tissueBar = [&](const char* name, uint32_t count, uint32_t total,
                         ImU32 col) {
      if (count == 0 && total == 0) return;
      ImGui::PushFont(ui::FontSmall());
      ui::ShadowText(dl, ImVec2(base.x + 4, y), Fade(ui::ColParchDim(), 0.9f), name);
      char buf[16];
      std::snprintf(buf, sizeof buf, "%u", count);
      const ImVec2 ts = ImGui::CalcTextSize(buf);
      ui::ShadowText(dl, ImVec2(base.x + 104 - ts.x, y), ui::ColParch(), buf);
      if (total > 0 && kBarW > 20) {
        const ImVec2 a(base.x + 112, std::floor(y + 3));
        const ImVec2 b2(a.x + kBarW, a.y + 8);
        const float frac = std::min(1.0f, (float)count / (float)total);
        dl->AddRectFilled(a, b2, Fade(ui::ColInk(), 0.5f));
        if (frac > 0.001f)
          dl->AddRectFilled(a, ImVec2(a.x + (b2.x - a.x) * frac, b2.y),
                            Fade(col, 0.85f));
        dl->AddRect(a, b2, Fade(IM_COL32_WHITE, 0.12f));
      }
      y += ImGui::GetTextLineHeight() + 2;
      ImGui::PopFont();
    };
    const uint32_t total = b.voxelTotal > 0 ? b.voxelTotal : 1;
    tissueBar("skin",   b.voxelSkin,   total, IM_COL32(210, 185, 155, 255));
    tissueBar("flesh",  b.voxelFlesh,  total, IM_COL32(180, 90,  90,  255));
    tissueBar("muscle", b.voxelMuscle, total, IM_COL32(160, 70,  80,  255));
    tissueBar("bone",   b.voxelBone,   total, IM_COL32(220, 215, 200, 255));
    // ROT ONLY APPEARS ONCE THERE IS SOME. The four above are unconditional
    // because a limb always has them (a zero bone count on an arm is news);
    // an uninfected body has no rot row at all, so the row's mere presence is
    // the alarm. Sick green, deliberately nothing like the two reds above it —
    // this bar growing while `flesh` shrinks is the whole story of a bite.
    // ONE ROW PER INFECTION, named by its authored label and drawn in its
    // material's colour: a venom wound says VENOM in bruise purple, the rot
    // says ROT in sick green.
    for (int k = 0; k < b.infectCount; k++) {
      char up[16];
      size_t n = 0;
      for (; n + 1 < sizeof up && b.infect[k].label[n]; n++)
        up[n] = (char)std::toupper((unsigned char)b.infect[k].label[n]);
      up[n] = '\0';
      tissueBar(up, b.infect[k].count, total, b.infect[k].color);
    }
    if (b.infectCount == 0 && b.voxelRot > 0)
      tissueBar("ROT", b.voxelRot, total, IM_COL32(120, 155, 80, 255));
    if (b.voxelBrain > 0 || b.voxelBrainMax > 0) {
      tissueBar("brain", b.voxelBrain, total, IM_COL32(225, 190, 195, 255));
      if (b.voxelBrainMax > 0 && b.voxelBrain < b.voxelBrainMax) {
        char miss[48];
        std::snprintf(miss, sizeof miss, "%u / %u brain missing",
                      b.voxelBrainMax - b.voxelBrain, b.voxelBrainMax);
        ImGui::PushFont(ui::FontSmall());
        ui::ShadowText(dl, ImVec2(base.x + 12, y), ui::ColBloodHi(), miss);
        y += ImGui::GetTextLineHeight() + 2;
        ImGui::PopFont();
      }
    }
    y += 6;

    // ---- CONDITION: hp and intactness, which are NOT the same number -------
    auto condBar = [&](const char* label, float frac, ImU32 fill) {
      ImGui::PushFont(ui::FontSmall());
      ui::ShadowText(dl, ImVec2(base.x + 4, y), Fade(ui::ColParchDim(), 0.9f), label);
      if (kBarW > 20) {
        const ImVec2 a(base.x + 112, std::floor(y + 3));
        const ImVec2 bv(a.x + kBarW, a.y + 8);
        ui::ValueBar(dl, a, bv, frac, fill, false);
      }
      y += ImGui::GetTextLineHeight() + 2;
      ImGui::PopFont();
    };
    {
      char cap[48];
      std::snprintf(cap, sizeof cap, "hp  %.0f / %.0f", b.hp, b.hpMax);
      condBar(cap, b.hpFrac, ui::ColBloodHi());
      std::snprintf(cap, sizeof cap, "intact  %.0f%%", b.voxelFrac * 100.0f);
      condBar(cap, b.voxelFrac, ui::ColSteel());
    }
    if (b.bleeding || b.burningVoxels > 0 || b.charredFrac > 0.02f) {
      ImGui::PushFont(ui::FontSmall());
      if (b.bleeding) {
        ui::ShadowText(dl, ImVec2(base.x + 12, y),
                       Fade(ui::ColBloodHi(), 0.6f + 0.4f * pulse), "BLEEDING");
        y += ImGui::GetTextLineHeight() + 2;
      }
      if (b.burningVoxels > 0) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "BURNING %u voxels", b.burningVoxels);
        ui::ShadowText(dl, ImVec2(base.x + 12, y),
                       Fade(ui::ColEmber(), 0.6f + 0.4f * pulse), buf);
        y += ImGui::GetTextLineHeight() + 2;
      }
      if (b.charredFrac > 0.02f) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "CHARRED %.0f%%", b.charredFrac * 100.0f);
        ui::ShadowText(dl, ImVec2(base.x + 12, y), ui::ColEmber(), buf);
        y += ImGui::GetTextLineHeight() + 2;
      }
      ImGui::PopFont();
    }
    y += 6;
  }

  // ---- COVER: the piece of gear standing between this limb and the world ---
  //
  // The reason the two views are one screen now. The slot itself is lit in the
  // armour column at the same time, so the sentence and the object agree.
  {
    y = ui::Subheading(dl, ImVec2(base.x, y), w, "COVER") + 4;
    const int eq = ArmorSlotForLimb(s.inspectSelected);
    ImGui::PushFont(ui::FontSmall());
    if (eq < 0) {
      small(Fade(ui::ColParchDim(), 0.8f), ImVec2(base.x + 4, y),
            "nothing in the set covers this");
      y += ImGui::GetTextLineHeight() + 2;
    } else if (eq < (int)s.equipSlots.size() && !s.equipSlots[eq].name.empty()) {
      const UIState::KitSlotUI& it = s.equipSlots[eq];
      if (it.dyeSwatch) {
        dl->AddRectFilled(ImVec2(base.x + 4, y + 3), ImVec2(base.x + 12, y + 11),
                          it.dyeSwatch);
        dl->AddRect(ImVec2(base.x + 4, y + 3), ImVec2(base.x + 12, y + 11),
                    Fade(ui::ColInk(), 0.8f));
      }
      ui::ShadowText(dl, ImVec2(base.x + (it.dyeSwatch ? 18.0f : 4.0f), y),
                     it.ruined ? ui::ColBloodHi() : ui::ColGoldHi(),
                     it.name.c_str());
      y += ImGui::GetTextLineHeight() + 2;
      if (it.wearable) {
        char cap[40];
        std::snprintf(cap, sizeof cap, "%s  %.0f%%",
                      it.ruined ? "RUINED" : "condition", it.condition * 100.0f);
        ui::ShadowText(dl, ImVec2(base.x + 12, y),
                       it.ruined ? ui::ColBloodHi() : Fade(ui::ColParchDim(), 0.9f),
                       cap);
        const ImVec2 a(base.x + 112, std::floor(y + 3));
        if (kBarW > 20)
          ui::ValueBar(dl, a, ImVec2(a.x + kBarW, a.y + 8), it.condition,
                       it.ruined ? ui::ColBlood() : ui::ColSteel(), false);
        y += ImGui::GetTextLineHeight() + 2;
      }
    } else {
      small(Fade(ui::ColBloodHi(), 0.85f), ImVec2(base.x + 4, y),
            "BARE - nothing worn here");
      y += ImGui::GetTextLineHeight() + 2;
      if (eq < (int)s.equipDefs.size() && !s.equipDefs[eq].label.empty()) {
        char cap[64];
        std::snprintf(cap, sizeof cap, "(%s slot is empty)",
                      s.equipDefs[eq].label.c_str());
        small(Fade(ui::ColParchDim(), 0.7f), ImVec2(base.x + 12, y), cap);
        y += ImGui::GetTextLineHeight() + 2;
      }
    }
    ImGui::PopFont();
    y += 6;
  }

  // ---- SURFACE: what is ON it, which is not damage -------------------------
  {
    y = ui::Subheading(dl, ImVec2(base.x, y), w, "SURFACE") + 4;
    ImGui::PushFont(ui::FontSmall());
    if (b.stainFrac >= s.stainHudMin && b.stainColor != 0) {
      dl->AddRectFilled(ImVec2(base.x + 4, y + 3), ImVec2(base.x + 12, y + 11),
                        b.stainColor | IM_COL32(0, 0, 0, 255));
      dl->AddRect(ImVec2(base.x + 4, y + 3), ImVec2(base.x + 12, y + 11),
                  Fade(ui::ColInk(), 0.8f));
      char buf[48];
      std::snprintf(buf, sizeof buf, "%s  %.0f%%",
                    b.stainLabel[0] ? b.stainLabel : "coated",
                    b.stainFrac * 100.0f);
      ui::ShadowText(dl, ImVec2(base.x + 18, y),
                     ui::Mix(b.stainColor, IM_COL32_WHITE, 0.35f), buf);
    } else {
      ui::ShadowText(dl, ImVec2(base.x + 4, y), Fade(ui::ColParchDim(), 0.8f),
                     "clean");
    }
    y += ImGui::GetTextLineHeight() + 4;
    ImGui::PopFont();
  }

  // The child's content extent, so a body with a long triage list scrolls
  // instead of running off the bottom of the panel.
  ImGui::SetCursorScreenPos(base);
  ImGui::Dummy(ImVec2(w, y - base.y));
  ImGui::EndChild();
}

}  // namespace

void DrawInventoryScreen(UIState& s) {
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 disp = io.DisplaySize;
  ImDrawList* bg = ImGui::GetBackgroundDrawList();

  // The world stays visible and stays RUNNING behind this — you can watch the
  // fire you set spread while you rummage. Dimmed only enough that the panels
  // are the thing being read, and vignetted so the eye is pulled in off the
  // edges of the screen onto them.
  ui::ScreenDim(bg, disp, 0.5f, 0.55f);

  // A DEATH OPENS THE HEALTH COLUMN, on the limb in the most trouble. main.cpp
  // opens the screen; which page of it you land on is this file's business,
  // and there is exactly one page worth landing on when the body is a corpse.
  // Done once per death (the column is free to be closed again afterwards)
  // because the frozen readout is a thing to READ, and a panel that reopened
  // itself every frame could not be closed at all.
  if (s.deathScreen && !s.deathScreenOpened) {
    s.deathScreenOpened = true;
    s.inspectMode = true;
    s.inspectSelected = WorstLimb(s);
    // A corpse is a thing to READ, so the triage groups all start open: the
    // question at a death is "what happened to this body", and that is the
    // one time the segments are worth more than the summary.
    s.triageOpen = 0xFFFFFFFFu;
  }

  // THE BENCH KEEPS ITS ROOM: the health column stays shut while a vessel is
  // on the alchemy bench (owner, 2026-09-27: opening it shrank the bench).
  // Shut here, its toggle hidden below, and opened again when the bench is
  // done if it was open when the bench came up.
  if (s.alchemy.open && s.inspectMode) {
    s.inspectMode = false;
    s.inspectResume = true;
  } else if (!s.alchemy.open && s.inspectResume) {
    s.inspectResume = false;
    s.inspectMode = true;
  }

  // ---- layout ---------------------------------------------------------------
  const float kPortraitW =
      s.portraitW > 0 ? (float)s.portraitW : kPortraitWFallback;
  const float kPortraitH =
      s.portraitH > 0 ? (float)s.portraitH : kPortraitHFallback;
  const float kDetailW = 272.0f;
  // THE WHOLE COLUMN, for as long as the health view is on — not only once a
  // limb is picked. The column leads with the body's pools and its triage,
  // which are worth reading before you have chosen anything, and a panel that
  // changed width on every limb click would shove the arsenal and the pack
  // sideways twice a second.
  const bool showDetail = s.inspectMode;
  const float leftBase = kPad * 2 + kSlot * 2 + kSlotGap * 2 + kPortraitW + 8;
  const float leftW = leftBase + (showDetail ? kDetailW + kColGap : 0.0f);
  const float top = 28.0f;
  // Room under the panels for the footer hint's tab.
  const float bottom = std::max(top + 200.0f, disp.y - 46.0f);
  const float leftX = 28.0f;
  // The arsenal is exactly as wide as its table: a gutter of sort labels and
  // nine cells. Ten bound keys at the same pitch fit inside that too.
  const float arsenalW = kPad * 2 + kGutter + (kTableCols - 1) * kCell + kSlot;
  const float midX = leftX + leftW + kColGap;
  // THE SPELLBOOK IS ONE WINDOW (2026-09-21). The grimoire and the arsenal were
  // two panels in two columns, which meant the composer — the screen's only
  // authoring surface — got a 508 px column of which the page list took 160,
  // and drew a tree in the ~310 px that were left while a whole column of glyph
  // table stood beside it at full height with a third of its pixels empty. They
  // are one panel now, the width of both: the grimoire on top (its canvas the
  // full width of the panel), the bindings and every word you know under it.
  // The tree went from ~300 px wide to ~850 and stopped scrolling sideways.
  const float bookX = midX;
  const float bookRoom = disp.x - 28.0f - bookX;
  // Ten cells of page + the arsenal's nine-cell table and its gutter, when the
  // window has it; whatever is there otherwise, down to the table's own width.
  // There is no narrow/wide FORK any more — one panel that is as wide as it can
  // be, because the toggle the old narrow path needed (arsenal or grimoire,
  // never both) is exactly the thing this change exists to delete.
  const float bookIdeal =
      (kPad * 2 + (kKeyCols - 1) * kCell + kSlot) + kColGap + arsenalW;
  // AN OPEN BOOK TAKES THE WHOLE DESK. Shut, it is as wide as it needs to be
  // for the ten bound keys and the word table beside them; open, it takes
  // every pixel the column has, because the page is the one surface here you
  // BUILD on and the tree is drawn at an integer rung - width it cannot use is
  // width the drawing spends on being half the size it could be.
  const float bookW = s.spellbookOpen
                          ? std::max(360.0f, bookRoom)
                          : std::max(360.0f, std::min(bookRoom, bookIdeal));
  // The pack is exactly as tall as its grid and its hotbar; whatever column
  // it shares gives it that and keeps the rest.
  const float lineH = ImGui::GetTextLineHeight();
  // THE PACK IS AS TALL AS WHAT IS IN IT, and no taller. It used to claim all
  // `bagRows` whether or not anything was in them, which on a 900 px screen
  // spent half the middle column on three rows of empty slots — and the column
  // above it is the GRIMOIRE, which since the spell page landed is the screen's
  // authoring surface and wants every pixel it can get (PLAN_spell_graph §0b:
  // the page is THE interface).
  //
  // Rows shown = the row holding the LAST filled slot, plus one empty row to
  // drop into. Keyed on the highest filled INDEX rather than on a count,
  // because nothing compacts the bag: one boot at slot 30 would otherwise
  // vanish. Nothing in the bag at all = no grid, just the in-hand strip.
  int packRows = 0;
  for (int i = (int)s.bagSlots.size() - 1; i >= 0; i--)
    if (!s.bagSlots[i].name.empty()) {
      packRows = std::min(s.bagRows, i / std::max(1, s.bagCols) + 2);
      break;
    }
  // THE PACK IS ONE BAND, NOT TWO. The bag grid and the in-hand strip stand
  // side by side now that the panel under the spellbook is the width of both
  // old columns: stacked they cost 86 px of height that the composer above
  // wants far more than the pack does, and at 900 px of screen that 86 px is
  // the difference between a tree at 1x and a tree at half scale.
  const float bagInnerW = s.bagCols * kSlot + (s.bagCols - 1) * kSlotGap;
  const float handInnerW = (kKeyCols - 1) * kCell + kSlot;
  const bool packSide = bookW - kPad * 2 >= bagInnerW + 24.0f + handInnerW;
  auto packHeightFor = [&](int rows) {
    const float grid = rows * (kSlot + kSlotGap);
    const float hand = lineH + 6 + 2 + kSlot;
    const float inner = packSide ? std::max(grid, hand)
                                 : grid + (rows > 0 ? 10.0f : 0.0f) + hand;
    return kFrame + ui::kHeaderH + 14 + inner + 12 + kFrame;
  };
  // THE SHUT BOOK'S HEIGHT: the header, the BOUND block (a subheading and two
  // rows of ten), and the frame. Nothing else is on the cover, so this is not
  // an estimate - it is the same three terms the body below draws, in the same
  // order, which is the rule this file already follows for the open panel.
  const float subHClosed = ImGui::GetTextLineHeight() + 6.0f;
  const float bookShutH = kFrame + ui::kHeaderH + 10 + subHClosed + 2 +
                          (2 * kSlot + 4) + 12 + kFrame;
  // A SHUT BOOK GIVES THE PACK EVERYTHING. The grid is content-sized while the
  // page is competing for the column; with the book on the desk there is
  // nothing to compete with, so the bag shows every row it has (and the
  // clamp below still trims it if the window is genuinely short).
  if (!s.spellbookOpen) packRows = s.bagRows;
  // The composer needs a name field, a canvas worth looking at, the row and the
  // footer, and the arsenal under it needs its bindings. Below this the page
  // stops being an interface, so the PACK gives way — down to the in-hand strip
  // alone if the window is genuinely short. Shut, the book asks for its cover
  // and nothing more.
  const float kBookMin = s.spellbookOpen ? 560.0f : bookShutH;
  while (packRows > 0 &&
         (bottom - top) - packHeightFor(packRows) - kColGap < kBookMin)
    packRows--;
  const float packH = packHeightFor(packRows);
  // COLUMN ORDER, left to right: character | spellbook over pack.
  //
  // The PACK stays next to the body that wears what is in it — bag to armour
  // slot, corpse to bag, the drag everyone makes constantly — and the SPELLBOOK
  // takes the whole of the rest, because it is the one surface on this screen
  // you BUILD something on rather than read.
  // OPEN: the whole column, and the pack is not drawn at all. SHUT: the cover,
  // and the pack takes the rest. (`packShown` is the one flag the pack block
  // reads - a panel that is not drawn has no slots, which is exactly what
  // "the book covers the desk" means.)
  // THE BENCH TAKES THE WHOLE COLUMN (game/alchemy_bench.h): no loot, no
  // book, no pack while a vessel is open on it -- the vessels you could pour
  // from are listed on the bench itself.
  const bool bench = s.alchemy.open;
  // ...and so does the ITEM STAGE (ui/item_stage.h), the bench's sibling.
  const bool stage = s.itemStage.open && !bench;
  const bool packShown = !s.spellbookOpen && !bench && !stage;
  // The bench's room, measured whether or not it is open: the table is sized
  // from it at the moment a vessel first goes on, which is before the bench
  // panel has ever been drawn. Must match AlchemyPanel's own layout. Measured
  // WITHOUT the health column, which the bench shuts (above): a vessel
  // double-clicked while the column was open used to get a table sized to
  // the narrower room.
  {
    const float benchX = leftX + leftBase + kColGap;
    const float panelW = std::max(360.0f, disp.x - 28.0f - benchX);
    const float panelH = bottom - top;
    s.alchemy.areaW = std::max(0.0f, panelW - 2 * (kFrame + kPad) - kBenchColW - kColGap);
    s.alchemy.areaH = std::max(0.0f, panelH - (kFrame + ui::kHeaderH + 14) - (kFrame + kPad));
    // The picture's bottom edge, from the top of the screen: all the room a
    // lifted flask has, since the panel draws the sim's headroom ABOVE its
    // box (over the header and up to the screen's edge).
    s.alchemy.roomH = bottom - (kFrame + kPad);
    // The scale: a flask about half the table (areaH / 260) -- but no larger
    // than lets the LIFT ROOM (AlchemyBench::kLiftH, a flask upside down
    // over another) fit on the screen, so a flask held that high is seen,
    // not clipped by the screen's top.
    // And no larger than leaves the table its least width (AlchemyBench::
    // kMinW, two vessels side by side): a table widened past the room it was
    // measured for is drawn a scale smaller than planned.
    int sc = (int)(s.alchemy.areaH / 260.0f);
    if (s.alchemy.liftH > 0) sc = std::min(sc, (int)(s.alchemy.roomH / (float)s.alchemy.liftH));
    if (s.alchemy.minW > 0) sc = std::min(sc, (int)(s.alchemy.areaW / (float)s.alchemy.minW));
    s.alchemy.scale = std::clamp(sc, 2, 5);
  }
  const float bookH =
      s.spellbookOpen
          ? (bottom - top)
          : std::min(bookShutH,
                     std::max(120.0f, bottom - top - packH - kColGap));

  const ImGuiWindowFlags kPanelFlags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoBringToFrontOnFocus;

  // Each panel is lit a little differently — the same trick xyzpan's paint()
  // uses so a row of panels reads as three objects and not one wallpaper.
  ui::PanelStyle stChar;
  stChar.darkMix = 0.30f;
  stChar.sheenPeak = 0.42f;
  ui::PanelStyle stArsenal;
  stArsenal.darkMix = 0.38f;
  stArsenal.sheenPeak = 0.58f;
  stArsenal.bronzeAlpha = 0.09f;
  ui::PanelStyle stBag;
  stBag.darkMix = 0.34f;
  stBag.sheenPeak = 0.35f;
  stBag.bronzeAlpha = 0.12f;
  ui::PanelStyle stGrim;
  stGrim.darkMix = 0.36f;
  stGrim.sheenPeak = 0.50f;
  stGrim.bronzeAlpha = 0.10f;

  // ==========================================================================
  // LEFT: the character panel
  // ==========================================================================
  ImGui::SetNextWindowPos(ImVec2(leftX, top));
  ImGui::SetNextWindowSize(ImVec2(leftW, bottom - top));
  ImGui::Begin("##character", nullptr, kPanelFlags);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    // No right-hand caption on this header: the toggle button already lives at
    // that end of the bar and a subtitle behind it is a subtitle sliced in
    // half ("co|health").
    float y = PanelChrome(dl, wp, ws, "CHARACTER", nullptr, stChar);
    // The toggle sits on the header bar, right-aligned. It ADDS the health
    // column and the portrait's callouts; it no longer swaps the panel for a
    // different page, so the gear never leaves the screen (see VitalsColumn).
    // Not while the alchemy bench is up: it would take the bench's room.
    if (!s.alchemy.open) {
      const char* label = "health";
      const ImVec2 ts = ImGui::CalcTextSize(label);
      const float bw = std::max(96.0f, ts.x + 24);
      if (ui::Button("##mode",
                     ImVec2(wp.x + ws.x - kFrame - 10 - bw,
                            wp.y + kFrame + std::floor((ui::kHeaderH - ts.y - 8) * 0.5f)),
                     label, s.inspectMode, bw)) {
        s.inspectMode = !s.inspectMode;
        s.inspectSelected = s.inspectMode ? WorstLimb(s) : -1;
      }
      if (ImGui::IsItemHovered())
        Tip(s.inspectMode
                ? "Close the health column. The gear stays either way."
                : "Open the health column beside your gear: the body's pools, "
                  "everything currently wrong with it worst-first, and - when you "
                  "click a limb on the portrait - what that limb is made of, what "
                  "happened to it, and what you have on over it.");
    }

    // The portrait, with a column of slots on each side.
    const float colL = wp.x + kPad;
    const float portX = colL + kSlot + kSlotGap;
    const float colR = portX + kPortraitW + kSlotGap;
    const float portY = y;

    // WHICH ARMOUR SLOT IS THE CURSOR OVER — asked GEOMETRICALLY and asked
    // BEFORE anything is drawn, because the answer is needed by the portrait,
    // which is drawn first. The rects below are the same ones the loop uses a
    // few lines down; there is no way to hover one of them and not the other.
    const int armourL[4] = {0, 1, 2, 3};   // head, chest, legs, boots
    const int armourR[4] = {4, 5, 6, 7};   // shoulders, hands, belt, trinket
    int hoverEquip = -1;
    {
      const ImVec2 m = ImGui::GetMousePos();
      for (int r = 0; r < 4 && hoverEquip < 0; r++) {
        const float sy = portY + r * (kSlot + kSlotGap);
        if (m.y < sy || m.y > sy + kSlot) continue;
        if (m.x >= colL && m.x <= colL + kSlot) hoverEquip = armourL[r];
        else if (m.x >= colR && m.x <= colR + kSlot) hoverEquip = armourR[r];
      }
    }

    ImGui::SetCursorScreenPos(ImVec2(portX, portY));
    Portrait(s, ImVec2(portX, portY), ImVec2(kPortraitW, kPortraitH));
    if (s.inspectMode)
      InspectOverlay(s, ImVec2(portX, portY), ImVec2(kPortraitW, kPortraitH));
    // The cast frame whenever a spell is readied (and no flask owns the
    // click), health column open or not: the click casts either way.
    if (s.applyText.empty())
      InspectCastPicks(s, ImVec2(portX, portY), ImVec2(kPortraitW, kPortraitH));
    // A hovered armour slot shades the limbs it is standing in front of. This
    // is the other half of the COVER section in the health column and the same
    // fact from the other end: "what does this piece protect" answered by
    // pointing at the body instead of by a list of part names.
    if (hoverEquip >= 0 && s.bodyValid)
      CoverHighlight(s, ImVec2(portX, portY), ImVec2(kPortraitW, kPortraitH),
                     hoverEquip);

    // Armour columns. Indices are the EquipSlotId order from game/equipment.h;
    // the labels and refusal reasons come from that same table through
    // equipDefs, never restated here.
    //
    // ALWAYS DRAWN, in both views. Health was a separate page until now and
    // opening it took the armour off the screen, which is exactly the moment
    // you most want to see it.
    for (int r = 0; r < 4; r++) {
      const float sy = portY + r * (kSlot + kSlotGap);
      for (int side = 0; side < 2; side++) {
        const int idx = side ? armourR[r] : armourL[r];
        const UIState::EquipSlotUI& d =
            idx < (int)s.equipDefs.size() ? s.equipDefs[idx]
                                          : UIState::EquipSlotUI{};
        char id[32];
        std::snprintf(id, sizeof id, "eq%d", idx);
        // The slot covering the SELECTED limb is lit as selected: the health
        // column names the piece, and this is the piece.
        const bool covers = s.inspectMode && s.inspectSelected >= 0 &&
                            ArmorSlotForLimb(s.inspectSelected) == idx;
        ItemSlot(s, id, ImVec2(side ? colR : colL, sy),
                 SlotOr(s.equipSlots, idx),
                 KitRef{KitSpace::Equip, idx}, d.icon.c_str(),
                 d.acceptsAnything, d.why.c_str(), covers, &d.accepts);
      }
    }
    // THE HANDS (dual wielding), at the foot of the two columns: the right
    // hand under the left column and the left under the right, the way the
    // figure faces you. Real kit slots (game/equipment.h HandR/HandL), so a
    // drag in or out is the same move Q and E make.
    for (int side = 0; side < 2; side++) {
      const int idx = s.handEquipSlot[side];
      if (idx < 0) continue;
      const float sy = portY + kPortraitH - kSlot;
      const float sx = side ? colR : colL;
      const UIState::EquipSlotUI& d =
          idx < (int)s.equipDefs.size() ? s.equipDefs[idx] : UIState::EquipSlotUI{};
      char id[32];
      std::snprintf(id, sizeof id, "eq%d", idx);
      ItemSlot(s, id, ImVec2(sx, sy), SlotOr(s.equipSlots, idx),
               KitRef{KitSpace::Equip, idx}, d.icon.c_str(), d.acceptsAnything,
               d.why.c_str(), false, &d.accepts);
      ImGui::PushFont(ui::FontSmall());
      const char* tag = side ? "L hand  (Q)" : "R hand  (E)";
      const ImVec2 ts = ImGui::CalcTextSize(tag);
      ui::ShadowText(dl, ImVec2(std::floor(sx + (kSlot - ts.x) * 0.5f), sy - ts.y - 2),
                     Fade(ui::ColParch(), 0.8f), tag);
      ImGui::PopFont();
    }
    y = portY + kPortraitH + 16;

    // FLASKS: every vessel you carry, hands first, then pack, then hotbar - the same
    // stacks, not copies, so a drag from here is a drag from where it lives.
    // CLICK ONE TO USE IT on yourself: the portrait turns into the pour brush
    // for that flask (Portrait's pourMode), whether or not it is in your hand.
    // "put away" hands the portrait back to the camera. The Sheath and Quick
    // equipment slots this row used to show still exist; nothing fills them.
    y = ui::Subheading(dl, ImVec2(wp.x + kPad, y), leftBase - kPad * 2,
                       "FLASKS");
    y += 2;
    {
      std::vector<KitRef> flasks;
      // A flask in a hand is a kit hand slot (dual wielding); the pour
      // (session.cpp) already resolves and splits one there.
      for (int i = 0; i < (int)s.equipSlots.size(); i++)
        if (s.equipSlots[i].kind == "container")
          flasks.push_back(KitRef{KitSpace::Equip, i});
      for (int i = 0; i < (int)s.bagSlots.size(); i++)
        if (s.bagSlots[i].kind == "container")
          flasks.push_back(KitRef{KitSpace::Bag, i});
      for (int i = 0; i < (int)s.hotbarSlots.size(); i++)
        if (s.hotbarSlots[i].kind == "container")
          flasks.push_back(KitRef{KitSpace::Hotbar, i});
      const bool inUse = s.activeVessel.Valid();
      const char* offLabel = "put away";
      const char* hint = "click one to use it";
      ImGui::PushFont(ui::FontSmall());
      const float offW = ImGui::CalcTextSize(offLabel).x + 24;
      // The right end of the row is kept for the button (or the hint in its
      // place), so choosing a flask never reflows the flasks.
      const float reserve = std::max(offW, ImGui::CalcTextSize(hint).x) + kSlotGap;
      ImGui::PopFont();
      const float rowW = leftBase - kPad * 2;
      const int fit =
          std::max(1, (int)((rowW - reserve + kSlotGap) / (kSlot + kSlotGap)));
      const int shown = std::min((int)flasks.size(), fit);
      for (int k = 0; k < shown; k++) {
        const KitRef r = flasks[k];
        const UIState::KitSlotUI& it =
            r.space == KitSpace::Equip ? s.equipSlots[r.index]
            : r.space == KitSpace::Bag ? s.bagSlots[r.index]
                                       : s.hotbarSlots[r.index];
        char id[32];
        std::snprintf(id, sizeof id, "flask%d_%d", (int)r.space, r.index);
        const ImVec2 at(wp.x + kPad + k * (kSlot + kSlotGap), y);
        const bool on = r == s.activeVessel;
        ItemSlot(s, id, at, it, r, "", false, "", on);
        // A click, not a press: the slot is also a drag source, and a drag
        // that ends back on it must not toggle the choice.
        if (!ImGui::GetDragDropPayload() &&
            ImGui::IsMouseHoveringRect(at, ImVec2(at.x + kSlot, at.y + kSlot)) &&
            ImGui::IsWindowHovered() &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 9.0f)
          s.activeVessel = on ? KitRef{} : r;
        // The fill, as the HUD hotbar draws it: the one number that matters
        // while pouring, readable without a tooltip.
        if (it.fill >= 0.0f) {
          const ImVec2 g0(at.x + 5, at.y + kSlot - 7);
          const ImVec2 g1(at.x + kSlot - 5, at.y + kSlot - 4);
          dl->AddRectFilled(g0, g1, Fade(ui::ColInk(), 0.8f));
          if (it.fill > 0.0f)
            dl->AddRectFilled(g0, ImVec2(std::floor(g0.x + (g1.x - g0.x) * it.fill), g1.y),
                              it.fillSwatch ? it.fillSwatch : ui::ColGoldDim());
        }
      }
      if (flasks.empty()) {
        ImGui::PushFont(ui::FontSmall());
        ui::ShadowText(dl, ImVec2(wp.x + kPad, y + kSlot * 0.5f - 7),
                       Fade(ui::ColParchDim(), 0.8f), "no flasks carried");
        ImGui::PopFont();
      } else if (shown < (int)flasks.size()) {
        char more[16];
        std::snprintf(more, sizeof more, "+%d", (int)flasks.size() - shown);
        ImGui::PushFont(ui::FontSmall());
        ui::ShadowText(dl, ImVec2(wp.x + kPad + shown * (kSlot + kSlotGap) - kSlotGap + 2, y),
                       ui::ColParchDim(), more);
        ImGui::PopFont();
      }
      if (inUse) {
        ImGui::PushFont(ui::FontSmall());
        const float bh = ImGui::GetTextLineHeight() + 8;
        if (ui::Button("##flaskoff",
                       ImVec2(wp.x + kPad + rowW - offW,
                              y + std::floor((kSlot - bh) * 0.5f)),
                       offLabel, true, offW))
          s.activeVessel = KitRef{};
        if (ImGui::IsItemHovered())
          Tip("Stop using this flask. The portrait goes back to turning, "
              "panning and zooming.");
        ImGui::PopFont();
      } else if (!flasks.empty()) {
        ImGui::PushFont(ui::FontSmall());
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        ui::ShadowText(dl, ImVec2(wp.x + kPad + rowW - ts.x, y + (kSlot - ts.y) * 0.5f),
                       Fade(ui::ColParchDim(), 0.8f), hint);
        ImGui::PopFont();
      }
    }
    y += kSlot + 18;

    // THE POOLS DO NOT MOVE. Health and mana are drawn here, at this width,
    // in both views — the health column no longer takes them over. They used
    // to migrate into that column when it opened, so pressing the one button
    // whose whole job is "tell me more about my health" picked the health bar
    // up and put it down 300 px to the right, and the eye had to go find it
    // again. The column below leads with what it ALONE can say (how much of
    // the body is physically still there, what is wrong with it, part by
    // part); it does not restate these two numbers.
    {
      const float poolW = leftBase - kPad * 2;
      y += PoolBar(dl, ImVec2(wp.x + kPad, y), poolW, s.health, s.healthMax,
                   s.healthCap, ui::ColBloodHi(), "HEALTH") + 8;
      y += PoolBar(dl, ImVec2(wp.x + kPad, y), poolW, s.mana, s.manaMax, -1,
                   ui::ColMana(), "MANA") + 8;
      // The locomotion state is the one-line answer to "what is this damage
      // actually costing me", which no bar can give: "crawling" says more
      // about a pair of lost legs than two empty hp bars do.
      if (!s.playerAlive) {
        ui::TrackedText(dl, ImVec2(wp.x + kPad, y + 2), ui::ColBloodHi(), "DEAD",
                        2.0f);
      } else if (!s.locoState.empty()) {
        ui::ShadowText(dl, ImVec2(wp.x + kPad, y + 2),
                       Fade(ui::ColParchDim(), 0.9f), s.locoState.c_str());
      }
      y += ImGui::GetTextLineHeight() + 10;
    }

    if (showDetail) {
      // The foot of this column answers the question the health column cannot:
      // how much of this body is behind metal at all, and what state that
      // metal is in.
      y = ui::Subheading(dl, ImVec2(wp.x + kPad, y), leftBase - kPad * 2,
                         "PROTECTION") + 4;
      {
        int worn = 0, wornMax = 0;
        float condSum = 0.0f, condN = 0.0f;
        for (int idx = 0; idx < 8; idx++) {
          if (idx >= (int)s.equipDefs.size()) continue;
          wornMax++;
          if (idx >= (int)s.equipSlots.size() || s.equipSlots[idx].name.empty())
            continue;
          worn++;
          if (s.equipSlots[idx].wearable) {
            condSum += s.equipSlots[idx].condition;
            condN += 1.0f;
          }
        }
        const float barW = leftBase - kPad * 2;
        char buf[64];
        y += PoolBar(dl, ImVec2(wp.x + kPad, y), barW, worn, wornMax, -1,
                     ui::ColSteel(), "WORN", 16.0f) + 4;
        if (condN > 0.0f) {
          const float avg = condSum / condN;
          std::snprintf(buf, sizeof buf, "GEAR  %.0f%%", avg * 100.0f);
          const ImVec2 a(wp.x + kPad, y);
          ui::ValueBar(dl, a, ImVec2(a.x + barW, a.y + 16), avg,
                       avg < 0.35f ? ui::ColBlood() : ui::ColGoldDim(), false);
          ImGui::PushFont(ui::FontSmall());
          const ImVec2 ts = ImGui::CalcTextSize(buf);
          ui::ShadowText(dl, ImVec2(a.x + 8, std::floor(a.y + (16 - ts.y) * 0.5f)),
                         ui::ColGoldPale(), buf);
          ImGui::PopFont();
          y += 20;
        }
      }
      // The column runs the FULL height of the panel, from the top of the
      // portrait to the bottom rule. It never draws health or mana: those are
      // above, in both views, and one screen saying "142 / 200" in two places
      // invites the two to disagree the day one of them is changed.
      const float detailX = wp.x + leftBase + kColGap;
      VitalsColumn(s, ImVec2(detailX, portY),
                   ImVec2(wp.x + ws.x - kPad - detailX,
                          wp.y + ws.y - kPad - portY));
    }
  }
  ImGui::End();

  // ==========================================================================
  // THE SPELLBOOK: one panel, grimoire over arsenal (plan §12a, §12b)
  // ==========================================================================
  // ONE BOOK, TWO HALVES. The grimoire on top — the page list, and beside it
  // the composer whose canvas is now the full width of the panel — and under a
  // rule, the arsenal: the twenty bound keys and every word you know. They were
  // two panels, and the seam between them was the whole problem: the table is
  // the drag SOURCE and the tree is the drop TARGET, and putting them in
  // separate windows meant the tree got one narrow column of the two.
  //
  // EVERY WORD is the RIGHT COLUMN (2026-09-22), full height, and it collapses
  // on the header's toggle — which now gives its width back to the tree rather
  // than its height. The tree is drawn at a scale that falls back to 0.5x and
  // is panned and zoomed by hand, so the column costs it detail, not content.
  ui::PanelStyle stLoot;
  stLoot.darkMix = 0.40f;
  stLoot.sheenPeak = 0.36f;
  stLoot.bronzeAlpha = 0.14f;
  // While a corpse is open it takes the book's place. Looting is a moment; the
  // words are not going anywhere, and a fourth column would not fit on most
  // screens.
  //
  // AS TALL AS THE CORPSE IS, and the book keeps the rest. It took the whole
  // column when the column was 528 px wide; over a thousand it was a hand of
  // pieces in an acre of empty plate, and it hid the page for no reason —
  // nothing about looting says you stop wanting to read your own spells.
  float bookTop = top, bookTall = bookH;
  // WHERE THE COLUMN ABOVE THE PACK ACTUALLY ENDS. It used to be `top + bookH`
  // by construction, because the book was the only thing in it; a corpse now
  // STACKS on top of a shut book (the spine is short enough to survive beside
  // one), so the pack has to be placed under whatever that stack came to or it
  // is drawn straight through the spine.
  float colBottom = top + bookH;
  if (bench) {
    ui::PanelStyle stBench;
    stBench.darkMix = 0.34f;
    stBench.sheenPeak = 0.40f;
    AlchemyPanel(s, ImVec2(bookX, top), ImVec2(std::max(360.0f, bookRoom), bottom - top),
                 stBench, kPanelFlags);
    bookTall = 0.0f;
  } else if (stage) {
    ui::PanelStyle stStage;
    stStage.darkMix = 0.36f;
    stStage.sheenPeak = 0.44f;
    stStage.bronzeAlpha = 0.11f;
    ItemStagePanel(s, ImVec2(bookX, top), ImVec2(std::max(360.0f, bookRoom), bottom - top),
                   stStage, kPanelFlags);
    bookTall = 0.0f;
  } else if (s.lootOpen) {
    const int lootCols =
        std::max(1, (int)((bookW - kPad * 2 + kSlotGap) / (kSlot + kSlotGap)));
    const int lootRows =
        std::max(1, ((int)s.lootSlots.size() + lootCols - 1) / lootCols);
    const float want = kFrame + ui::kHeaderH + 14 +
                       lootRows * (kSlot + kSlotGap) + 12 + kFrame;
    // THE CORPSE IS MEASURED AGAINST THE COLUMN, NOT AGAINST THE BOOK. With
    // the book shut its rect is a 190 px spine, and a corpse clamped to that
    // loses every row past the second — while most of the column under it is
    // empty desk. The room a corpse may have is everything above the pack.
    const float lootRoom =
        s.spellbookOpen ? bookH
                        : (bottom - top - (packShown ? packH + kColGap : 0.0f));
    const float lootH = std::min(want, lootRoom);
    // A page in less than 520 px is a canvas of 60 px under a name field:
    // worse than no page, because it is a BROKEN one. A SHUT book needs its
    // cover and nothing else, so it survives beside a corpse that an open one
    // would not. When what is left is under that, the corpse takes the whole
    // rect — which is the arrangement this panel always had — rather than
    // leaving a hole in the screen where a book too short to draw would be.
    const float bookWants = s.spellbookOpen ? 520.0f : bookShutH;
    const bool bookFits = lootRoom - lootH - kColGap >= bookWants;
    LootPanel(s, ImVec2(bookX, top), ImVec2(bookW, lootH), stLoot, kPanelFlags);
    bookTop = top + lootH + kColGap;
    bookTall = !bookFits ? 0.0f
               : s.spellbookOpen ? lootRoom - lootH - kColGap
                                 : bookShutH;
    colBottom = bookTall > 0.0f ? bookTop + bookTall : top + lootH;
  }
  if (bookTall > 0.0f) {   // zero only when a corpse took the whole rect
  ImGui::SetNextWindowPos(ImVec2(bookX, bookTop));
  ImGui::SetNextWindowSize(ImVec2(bookW, bookTall));
  ImGui::Begin("##spellbook", nullptr, kPanelFlags);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    float y = PanelChrome(dl, wp, ws, "SPELLBOOK", nullptr, stGrim);
    const float innerX = wp.x + kPad;
    const float innerW = ws.x - kPad * 2;
    const float innerBottom = wp.y + ws.y - kPad;
    // The toggles, on the header bar where the character panel's is, laid out
    // from the right edge: OPEN/CLOSE first because it is the one that changes
    // the screen, then `words` - which only means anything once the book is
    // open, so it is only there then.
    {
      float bx = wp.x + ws.x - kFrame - 10;
      const float by =
          wp.y + kFrame + std::floor((ui::kHeaderH - ImGui::GetTextLineHeight() - 8) * 0.5f);
      const char* openLabel = s.spellbookOpen ? "close" : "open the book";
      const ImVec2 os = ImGui::CalcTextSize(openLabel);
      const float obw = std::max(96.0f, os.x + 24);
      bx -= obw;
      if (ui::Button("##bookopen", ImVec2(bx, by), openLabel, s.spellbookOpen, obw))
        s.spellbookOpen = !s.spellbookOpen;
      if (ImGui::IsItemHovered())
        Tip(s.spellbookOpen
                ? "Shut the book. Your pack comes back and the bound keys stay."
                : "Open the book across the whole desk: the page, the index "
                  "and every word you know. Your pack goes under it.");
      if (s.spellbookOpen) {
        const char* label = "words";
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const float bw = std::max(96.0f, ts.x + 24);
        bx -= bw + 8;
        if (ui::Button("##wordsmode", ImVec2(bx, by), label, s.spellWordsOpen, bw))
          s.spellWordsOpen = !s.spellWordsOpen;
        if (ImGui::IsItemHovered())
          Tip(s.spellWordsOpen
                  ? "Fold the word column away and give its width back to the "
                    "tree. Your bound keys stay."
                  : "Every word you know, in one column beside the page: the "
                    "thing you drag onto the tree and onto a key.");
      }
    }

    // ---- THE COVER, when the book is shut ----------------------------------
    //
    // What is on a shut book: its bindings, and an invitation. The ten keys
    // stay because they are the only part of the spellbook that matters while
    // you are LOOKING at something else - they are literally the number row
    // the game is listening to - and everything that is about WRITING (the
    // index, the page, the tree, the word table) is behind the cover.
    //
    // The whole cover is the button, not just the one on the header: a shut
    // book you have to hunt for a 96 px target to open is a worse shut book
    // than one you can just click.
    if (!s.spellbookOpen) {
      const float coverBottom = wp.y + ws.y - kFrame;
      ImGui::SetCursorScreenPos(ImVec2(innerX, y));
      if (ImGui::InvisibleButton("##cover",
                                 ImVec2(std::max(8.0f, innerW),
                                        std::max(8.0f, coverBottom - y))))
        s.spellbookOpen = true;
      const bool coverHov = ImGui::IsItemHovered();
      if (coverHov && !ImGui::GetDragDropPayload())
        Tip("Open the book: the page you write spells on, across the whole "
            "desk.");
      float cy = y + 4;
      // The clasp: a rubricated line along the top of the cover, and the
      // sentence that says what is inside. It lights on hover, so the whole
      // panel reads as one pressable thing.
      const ImU32 clasp = Fade(ui::ColGoldDim(), coverHov ? 1.0f : 0.7f);
      dl->AddRectFilled(ImVec2(innerX, cy), ImVec2(innerX + innerW, cy + 2), clasp);
      cy += 10;
      float by2 = ui::Subheading(dl, ImVec2(innerX, cy),
                                 std::min((kKeyCols - 1) * kCell + kSlot, innerW),
                                 "BOUND   1-0 / Shift+1-0");
      by2 += 2;
      BoundKeys(s, dl, ImVec2(innerX, by2), (kKeyCols - 1) * kCell + kSlot);
      // The invitation, at the right end of the same band the keys are on, so
      // a wide panel does not leave a hand of empty plate beside them.
      {
        const char* hint = "the book is shut - click to open it";
        ImGui::PushFont(ui::FontSmall());
        const ImVec2 hs = ImGui::CalcTextSize(hint);
        ImGui::PopFont();
        const float hx = innerX + innerW - hs.x - 6;
        if (hx > innerX + (kKeyCols - 1) * kCell + kSlot + 20)
          dl->AddText(ui::FontSmall(), 13.0f,
                      ImVec2(std::floor(hx), std::floor(by2 + kSlot - 6)),
                      Fade(coverHov ? ui::ColGoldHi() : ui::ColParchDim(), 0.9f),
                      hint);
      }
    } else {

    // HOW THE PANEL IS SPLIT (2026-09-22): a COLUMN, not a band.
    //
    // EVERY WORD used to be a strip under the bound keys — the surface you
    // drag FROM at the bottom of the panel, the surface you drag ONTO at the
    // top, and the whole page between them. Worse, its height was whatever the
    // page did not want, quantised to whole rows, so on a short screen the
    // index of every word you know was one band of cells and a scrollbar.
    //
    // It is a COLUMN now, down the right edge, as tall as the book: the bands
    // stand beside the tree they feed and the drag is a short sideways one.
    //
    // THE SPACE COMES OUT OF THE CANVAS, halved. The composer's width is what
    // the tree gets, and the tree is drawn at an integer scale that falls back
    // to 0.5x, so it loses DETAIL rather than content — and the wheel-zoom and
    // the drag-to-pan the canvas grew on the same day give the whole drawing
    // back at any size the column leaves it.
    const float boundW = (kKeyCols - 1) * kCell + kSlot;
    const float wordsGap = 20.0f;
    // The composer's share of the body if the table took nothing: the page
    // list is a fixed column, so this is what "half the canvas" is half OF.
    const float compAvail = innerW - kListW - 12;
    // THE COLUMN IS A WHOLE NUMBER OF CELLS WIDE, and that is the whole sizing
    // rule. A share of the body (0.44, which was half) rounded DOWN to the
    // cells that fit in it: the table is bands of 44 px cells at a 48 px pitch
    // over a 68 px gutter of sort labels, so any width that is not gutter +
    // N*cell is slack — a strip of empty plate down the right of the column
    // that the canvas beside it would rather have. Six cells instead of seven
    // hands the tree ~66 px and costs the table one column of a band.
    //
    // A column narrower than four cells is not a table, and a left column
    // narrower than the bound rows would push the number keys off their own
    // panel. Both are refused rather than squeezed: the table folds away
    // exactly as the toggle folds it.
    auto colsToW = [](int cols) {
      return kGutter + (float)cols * kCell - (kCell - kSlot);
    };
    float wordsW = 0.0f;
    if (s.spellWordsOpen) {
      const int cols = std::clamp(
          (int)std::floor((compAvail * 0.44f - kGutter + (kCell - kSlot)) / kCell),
          4, 8);
      const float want = colsToW(cols);
      if (innerW - want - wordsGap >= boundW) wordsW = want;
    }
    const float grimW = innerW - (wordsW > 0.0f ? wordsW + wordsGap : 0.0f);
    const float tableX = innerX + grimW + wordsGap;
    const int arsPerRow =
        std::max(1, (int)((wordsW - kGutter + (kCell - kSlot)) / kCell));
    const float bodyH = innerBottom - y;
    // EVERY TERM BELOW IS A THING ACTUALLY DRAWN, in the order it is drawn, so
    // the block's height is the block's height. The first version guessed 22 px
    // for a subheading that is `GetTextLineHeight() + 6` — 32 in the 26 px
    // chrome face — and the arsenal ran 28 px past the panel's bottom rule with
    // the last row of cells sliced through the middle.
    const float subH = ImGui::GetTextLineHeight() + 6.0f;
    const float seamH = 14.0f;                       // the rule and its air
    const float boundH = subH + 2 + (2 * kSlot + 4) + 10;
    // The left column, top to bottom: the page, a seam, the bound keys. The
    // keys are a fixed block (two rows of ten), so the page gets the rest —
    // nothing under it competes for the height any more.
    const float arsH = seamH + boundH;
    const float grimH = std::max(180.0f, bodyH - arsH);

    GrimoireBody(s, ImVec2(innerX, y), ImVec2(grimW, grimH));

    // The seam: a 2 px bronze rule with a gold tick at each end, the same one
    // the subheadings use, so the two halves read as two sections of one page
    // rather than as two panels that happen to touch. It spans the LEFT column
    // only — the word column runs past it, top to bottom.
    float ay = y + grimH + 6;
    dl->AddRectFilled(ImVec2(innerX, ay), ImVec2(innerX + grimW, ay + 2),
                      Fade(ui::ColBronze(), 0.9f));
    dl->AddRectFilled(ImVec2(innerX, ay - 2), ImVec2(innerX + 10, ay + 4),
                      Fade(ui::ColGoldDim(), 0.9f));
    dl->AddRectFilled(ImVec2(innerX + grimW - 10, ay - 2),
                      ImVec2(innerX + grimW, ay + 4), Fade(ui::ColGoldDim(), 0.9f));
    ay += 8;

    // The bound rows: they are the thing that matters, and they are literally
    // the number row the game is listening to.
    float by = ui::Subheading(dl, ImVec2(innerX, ay), std::min(boundW, grimW),
                              "BOUND   1-0 / Shift+1-0");
    by += 2;
    by += BoundKeys(s, dl, ImVec2(innerX, by), boundW);

    // ---- EVERY WORD, the right column ----
    //
    // As tall as its CONTENT or as tall as the book, whichever is smaller: the
    // table's own child scrolls when the words outrun the column, and sizing it
    // to the content when they do not keeps the panel's bottom rule from having
    // a hand of empty plate above it.
    if (wordsW > 0.0f) {
      const float rule = std::floor(tableX - wordsGap * 0.5f);
      dl->AddRectFilled(ImVec2(rule, y), ImVec2(rule + 2, innerBottom),
                        Fade(ui::ColBronze(), 0.55f));
      float ty = ui::Subheading(dl, ImVec2(tableX, y), wordsW, "EVERY WORD") + 2;
      const float tall = std::min(innerBottom - ty, GlyphTableHeight(s, arsPerRow) + 8);
      GlyphTable(s, ImVec2(tableX, ty), ImVec2(wordsW, std::max(48.0f, tall)),
                 arsPerRow);
    }
    }
  }
  ImGui::End();
  }

  // ==========================================================================
  // THE PACK: bag + hotbar, under the spellbook - unless the book is open
  // across the desk, in which case there is no desk left to put it on.
  // ==========================================================================
  if (packShown) {
  const ImVec2 packPos(bookX, colBottom + kColGap);
  const float packW = bookW;
  ImGui::SetNextWindowPos(packPos);
  // AS TALL AS WHAT IS IN IT, not as tall as what is left. With the book shut
  // the pack shows every row it has and there is still most of a column under
  // it; a panel stretched to the bottom rule to swallow that is 300 px of
  // empty plate with a gold frame round it, which reads as a bag that is
  // mostly missing rather than as a desk with room on it.
  ImGui::SetNextWindowSize(ImVec2(packW, std::max(160.0f, packH)));
  ImGui::Begin("##bag", nullptr, kPanelFlags);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    float y = PanelChrome(dl, wp, ws, "PACK", "drag out to drop", stBag);

    // SIDE BY SIDE when the panel is wide enough for both: the bag grid on the
    // left, the in-hand strip on its own at the right. Stacked otherwise, which
    // is the old arrangement and the one a narrow window still gets. Both
    // halves are laid out from the same two x's, so nothing moves between the
    // two cases except which y the strip starts at.
    const float bagInner = s.bagCols * kSlot + (s.bagCols - 1) * kSlotGap;
    const float handInner = (kKeyCols - 1) * kCell + kSlot;
    const float contentW = ws.x - kPad * 2;
    const bool side = contentW >= bagInner + 24.0f + handInner;
    // Centred as a PAIR when they share the row, each centred in its own half
    // otherwise: eight cells hugging the left edge of a panel twice that wide
    // read as a mistake.
    const float pairW = side ? bagInner + 24.0f + handInner : bagInner;
    const float ox =
        std::max(0.0f, std::floor((contentW - pairW) * 0.25f) * 2.0f);
    const float bagX = wp.x + kPad + ox;
    const float handX = side ? bagX + bagInner + 24.0f
                             : wp.x + kPad +
                                   std::max(0.0f, std::floor((contentW - handInner) *
                                                             0.25f) * 2.0f);
    // Only the rows the geometry above decided on (filled + one to drop into).
    // A row that is not drawn is not a lost slot: it appears the moment the one
    // before it takes something, and the bag can only be filled through the
    // spare row.
    for (int r = 0; r < packRows; r++)
      for (int c = 0; c < s.bagCols; c++) {
        const int idx = r * s.bagCols + c;
        const float gx = bagX + c * (kSlot + kSlotGap);
        const float gy = y + r * (kSlot + kSlotGap);
        if (gx + kSlot > wp.x + ws.x - kPad) continue;
        char id[32];
        std::snprintf(id, sizeof id, "bag%d", idx);
        ItemSlot(s, id, ImVec2(gx, gy), SlotOr(s.bagSlots, idx),
                 KitRef{KitSpace::Bag, idx}, nullptr, true, nullptr, false);
      }
    float hy = y;
    if (!side && packRows > 0) hy += packRows * (kSlot + kSlotGap) + 10;

    hy = ui::Subheading(dl, ImVec2(handX, hy), side ? handInner : contentW,
                        "IN HAND  1-0");
    hy += 2;
    for (int i = 0; i < (int)s.hotbarSlots.size(); i++) {
      const float gx = handX + i * kCell;
      if (gx + kSlot > wp.x + ws.x - kPad) break;
      char id[32];
      std::snprintf(id, sizeof id, "hb%d", i);
      ItemSlot(s, id, ImVec2(gx, hy), s.hotbarSlots[i],
               KitRef{KitSpace::Hotbar, i}, nullptr, true, nullptr,
               i == s.itemSelected);
      char k[4];
      std::snprintf(k, sizeof k, "%d", (i + 1) % 10);
      ui::KeyBadge(dl, ImVec2(gx + 2, hy + 2), k);
    }
  }
  ImGui::End();
  }

  // ---- DRAGGED OUT OF EVERY PANEL --------------------------------------------
  //
  // Every slot is a drop TARGET, so a drag that ends anywhere else has been
  // refused by all of them and imgui simply forgets it. That silence is the
  // problem: "drag it out of the window" is the gesture every inventory in the
  // genre uses for discard, and having it do nothing reads as broken.
  //
  // Detected the only way imgui allows — the payload was still live last frame
  // and the mouse has now been released with nobody accepting it. Deliberately
  // AFTER every panel has had its chance, so a legal move is never mistaken
  // for a drop. An ITEM goes on the floor; a WORD dragged out of the page
  // being composed leaves the page — the same gesture, the same meaning.
  if (const ImGuiPayload* p = ImGui::GetDragDropPayload()) {
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
      if (p->IsDataType(kPayloadItem)) {
        KitRef from{};
        std::memcpy(&from, p->Data, sizeof(from));
        s.dropItem.pending = true;
        s.dropItem.from = from;
      } else if (p->IsDataType(kPayloadWord)) {
        int idx = -1;
        std::memcpy(&idx, p->Data, sizeof(idx));
        if (idx >= 0 && idx < (int)s.grimoireEditWords.size()) {
          PushUndo(s);
          s.grimoireEditWords.erase(s.grimoireEditWords.begin() + idx);
          s.grimoireEditDirty = true;
        }
      } else if (p->IsDataType(kPayloadGraphNode)) {
        // And once more on the CANVAS: a branch dragged off the tree is
        // removed, subtree and all (PLAN_spell_graph §5). The op goes through
        // the intent latch like every other graph edit — the canvas never
        // edits words, not even to delete one.
        int idx = -1;
        std::memcpy(&idx, p->Data, sizeof(idx));
        if (idx >= 0 && idx < (int)s.spellGraph.nodes.size() &&
            s.spellGraph.nodes[idx].treeNode >= 0) {
          PushUndo(s);
          s.graphEdit = {};
          s.graphEdit.pending = true;
          s.graphEdit.op = UIState::GraphEditIntent::Remove;
          s.graphEdit.treeNode = s.spellGraph.nodes[idx].treeNode;
        }
      } else if (p->IsDataType(kPayloadBound)) {
        // The same gesture again, one level up: a key dragged off the row
        // unbinds. "Drag it out" means "I do not want this here" everywhere
        // else on this screen, and a key was the one place it did nothing.
        int slot = -1;
        std::memcpy(&slot, p->Data, sizeof(slot));
        if (slot >= 0) {
          s.bindGlyph.pending = true;
          s.bindGlyph.slot = slot;
          s.bindGlyph.glyphId.clear();
          s.bindGlyph.page = false;
        }
      }
    }
  }

  // The one line at the foot of the screen, centred under everything on a
  // dark tab so it reads over whatever the world is doing down there: the
  // instruction, or — while one is fresh — the answer to what you just did
  // ("that won't fit there", "saved heal", "refused: ..."). The answer fades
  // over ~2.5 s rather than sticking, because an answer still on screen a
  // minute later reads as a persistent error state.
  {
    const bool fresh = !s.kitMessage.empty() && s.kitMessageAge < 2.5f;
    const char* idle = s.lootOpen
        ? "right-click to take   |   drag onto a slot to wear   |   drag out to leave it on the ground"
        : "I or Esc to close   |   drag out to drop";
    const char* text = fresh ? s.kitMessage.c_str() : idle;
    const float a = fresh ? std::clamp(1.6f - s.kitMessageAge * 0.7f, 0.0f, 1.0f) : 0.85f;
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 tp(std::floor((disp.x - ts.x) * 0.5f), disp.y - ts.y - 8);
    bg->AddRectFilled(ImVec2(tp.x - 12, tp.y - 2), ImVec2(tp.x + ts.x + 12, disp.y),
                      Fade(ui::ColInk(), 0.6f));
    bg->AddRectFilled(ImVec2(tp.x - 12, tp.y - 2), ImVec2(tp.x + ts.x + 12, tp.y),
                      Fade(fresh ? ui::ColEmber() : ui::ColGoldDim(), 0.5f));
    ui::ShadowText(bg, tp, Fade(fresh ? ui::ColEmber() : ui::ColParchDim(), a), text);
  }
}

// ---- the HUD hotbar (ui/inventory_ui.h) -------------------------------------
//
// The pack's slot, minus everything interactive: the same recess, rim, icon,
// dye chip and count badge, so a sword on the HUD is the sword in the pack.
// The number each slot answers to sits in its top-left corner, and the
// selected slot's name floats above the strip for a moment after it changes
// -- the way you learn what you just switched to without looking down.
// A SPELL'S LIGHT in a slot: a pool of its colour that breathes, under the
// engraving. The HUD's echo of the light in the fist (main.cpp, the hand
// glow) — same colour, same slow pulse.
static void SpellLight(ImDrawList* dl, ImVec2 mid, uint32_t color, float strength) {
  if (color == 0) return;
  const float t = (float)ImGui::GetTime();
  const float breathe = 0.75f + 0.25f * std::sin(t * 3.0f);
  const ImU32 c = (color & 0x00FFFFFFu);
  dl->AddCircleFilled(mid, kSlot * 0.42f, c | ((ImU32)(60 * strength * breathe) << 24), 20);
  dl->AddCircleFilled(mid, kSlot * 0.26f, c | ((ImU32)(110 * strength * breathe) << 24), 16);
}

// A bound spell drawn in a slot: its light, then a glyph's engraving or the
// page mark.
static void SpellContents(const UIState& s, ImDrawList* dl, ImVec2 at, const std::string& id,
                          bool page, int type, uint32_t color, float strength) {
  const ImVec2 mid(at.x + kSlot * 0.5f, at.y + kSlot * 0.5f);
  SpellLight(dl, mid, color, strength);
  if (page) {
    PageContents(dl, at);
  } else if (const UIState::GlyphUI* g = FindGlyph(s, id)) {
    GlyphContents(dl, at, *g);
  } else {
    ui::GlyphArt(dl, at, kSlot, color, type < 0 ? 0 : type);
  }
}

void DrawHudHotbar(const UIState& s, ImDrawList* dl) {
  const int n = (int)s.hotbarSlots.size();
  if (n == 0) return;
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  constexpr float kGap = 4.0f, kBottom = 14.0f;
  const float w = n * kSlot + (n - 1) * kGap;
  const float x0 = std::floor((disp.x - w) * 0.5f);
  const float y0 = std::floor(disp.y - kBottom - kSlot);
  // The hand is only the hotbar with the melee tool up and magic off (the
  // number row belongs to the brush or the glyphs otherwise), so the strip
  // says so by dimming its selection rather than lying about what is held.
  const bool live = s.tool == UIState::kToolMelee && !s.magicMode;
  const int sel = s.itemSelected;
  // THE SPELL BAR (Z): the bound keys in the items' place, same geometry, so
  // the number row and the wheel move along it the way they move along the
  // items. It shows the bank the selection is in, or bank B while Shift is
  // held (Shift+number reaches it).
  const bool spellBar = s.magicMode && !s.glyphSlots.empty();

  dl->AddRectFilled(ImVec2(x0 - 6, y0 - 6), ImVec2(x0 + w + 6, y0 + kSlot + 6),
                    spellBar ? IM_COL32(20, 8, 40, 120) : IM_COL32(0, 0, 0, 90));
  ImGui::PushFont(ui::FontSmall());
  if (spellBar) {
    const int bank = s.glyphBankB ? 1 : (s.glyphSelected >= 10 ? 1 : 0);
    for (int c = 0; c < n && c < 10; c++) {
      const int i = bank * 10 + c;
      if (i >= (int)s.glyphSlots.size()) break;
      const ImVec2 at(x0 + c * (kSlot + kGap), y0);
      const bool filled = !s.glyphSlots[i].empty();
      const bool isSel = i == s.glyphSelected;
      const ui::SlotLook look = isSel    ? ui::SlotLook::Hover
                                : filled ? ui::SlotLook::Filled
                                         : ui::SlotLook::Empty;
      ui::SlotSurface(dl, at, kSlot, look, isSel);
      SlotRim(dl, at, look);
      if (filled) {
        const bool page = i < (int)s.glyphSlotKinds.size() && s.glyphSlotKinds[i] == 2;
        const uint32_t col = i < (int)s.glyphSlotColors.size() ? s.glyphSlotColors[i] : 0;
        const int type = i < (int)s.glyphSlotTypes.size() ? s.glyphSlotTypes[i] : -1;
        SpellContents(s, dl, at, s.glyphSlots[i], page, type, col, isSel ? 1.0f : 0.6f);
      }
      // A key already in a hand says which: R / L in the bottom corner.
      for (int hk = 0; hk < 2; hk++)
        if (!s.handSpell[hk].empty() && s.handSpell[hk] == s.glyphSlots[i])
          ui::ShadowText(dl, ImVec2(at.x + (hk == 1 ? 4 : kSlot - 10), at.y + kSlot - 16),
                         ui::ColGoldHi(), hk == 1 ? "L" : "R");
      char key[6];
      std::snprintf(key, sizeof key, bank ? "S%d" : "%d", (c + 1) % 10);
      ui::ShadowText(dl, ImVec2(at.x + 4, at.y + 2),
                     isSel ? ui::ColGoldHi() : Fade(ui::ColParch(), 0.7f), key);
    }
    const int sc = s.glyphSelected - bank * 10;
    if (sc >= 0 && sc < 10 && sc < n) {
      const ImVec2 at(x0 + sc * (kSlot + kGap), y0);
      dl->AddRect(ImVec2(at.x - 3, at.y - 3), ImVec2(at.x + kSlot + 3, at.y + kSlot + 3),
                  ui::ColGoldHi(), 0.0f, 0, 2.0f);
    }
    // Above the bar: the selected spell's name and what the keys do.
    const std::string& nm = s.glyphSelected >= 0 && s.glyphSelected < (int)s.glyphSlots.size()
                                ? s.glyphSlots[s.glyphSelected]
                                : std::string();
    const std::string line = (nm.empty() ? std::string("spells") : nm) +
                             "   -   Q left hand  .  E right hand  .  Z close";
    const ImVec2 ts = ImGui::CalcTextSize(line.c_str());
    ui::ShadowText(dl, ImVec2(std::floor((disp.x - ts.x) * 0.5f), y0 - 10 - ts.y),
                   Fade(ui::ColParch(), 0.9f), line.c_str());
  }
  for (int i = 0; i < n && !spellBar; i++) {
    const UIState::KitSlotUI& item = s.hotbarSlots[i];
    const ImVec2 at(x0 + i * (kSlot + kGap), y0);
    const bool filled = !item.name.empty();
    const bool isSel = live && i == sel;
    const ui::SlotLook look = isSel    ? ui::SlotLook::Hover
                              : filled ? ui::SlotLook::Filled
                                       : ui::SlotLook::Empty;
    ui::SlotSurface(dl, at, kSlot, look, isSel);
    SlotRim(dl, at, look);
    const ImVec2 mid(at.x + kSlot * 0.5f, at.y + kSlot * 0.5f);
    if (filled) {
      DrawVesselGlow(dl, mid, item.fill, item.fillSwatch, item.fillGlow);
      ui::DrawSpriteCentered(dl, ItemIcon(item.kind), ImVec2(mid.x + 1, mid.y + 1),
                             Fade(ui::ColInk(), 0.7f));
      ui::DrawSpriteCentered(dl, ItemIcon(item.kind), mid,
                             item.dyeSwatch ? item.dyeSwatch : IM_COL32_WHITE);
      DrawVesselContents(dl, mid, item.fill, item.fillSwatch, item.fillGlow,
                         &item.fillBandColor, &item.fillBandFrac);
      if (item.dyeSwatch) {
        dl->AddRectFilled(ImVec2(at.x + kSlot - 9, at.y + 3),
                          ImVec2(at.x + kSlot - 3, at.y + 9), item.dyeSwatch);
      }
      if (item.count > 1) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d", item.count);
        ui::CountBadge(dl, ImVec2(at.x + kSlot - 2, at.y + kSlot - 2), buf);
      }
      if (item.fill >= 0.0f) {
        // The vessel's gauge: a 4 px bar along the slot's floor, whole pixels.
        const float gx0 = at.x + 6, gx1 = at.x + kSlot - 6, gy = at.y + kSlot - 8;
        dl->AddRectFilled(ImVec2(gx0, gy), ImVec2(gx1, gy + 4), IM_COL32(10, 10, 14, 220));
        const float fx = std::floor(gx0 + (gx1 - gx0) * item.fill);
        if (fx > gx0)
          dl->AddRectFilled(ImVec2(gx0, gy), ImVec2(fx, gy + 4),
                            item.fillSwatch ? item.fillSwatch : IM_COL32(90, 160, 235, 255));
      }
    }
    // The key: 1..9 then 0, the number row as it lies under the fingers.
    char key[4];
    std::snprintf(key, sizeof key, "%d", (i + 1) % 10);
    ui::ShadowText(dl, ImVec2(at.x + 4, at.y + 2),
                   isSel ? ui::ColGoldHi() : Fade(ui::ColParch(), 0.7f), key);
  }
  // ---- THE HANDS (dual wielding): one slot either side of the strip --------
  //
  // The LEFT hand's slot left of the hotbar, the RIGHT's right of it, as the
  // player's own hands lie. Each says the key that fills it (Q / E), the
  // button that uses it (RMB / LMB) and — holding a vessel — what that button
  // does right now (pour / scoop / apply; F cycles it). The hand last used is
  // framed: it is the one F talks to.
  static const char* const kModeName[3] = {"pour", "scoop", "apply"};
  for (int side = 0; side < 2; side++) {
    const int hk = side == 0 ? 1 : 0;   // screen-left is the LEFT hand
    const int idx = s.handEquipSlot[hk];
    if (idx < 0 || idx >= (int)s.equipSlots.size()) continue;
    const UIState::KitSlotUI& item = s.equipSlots[idx];
    const float hx = side == 0 ? x0 - 18 - kSlot : x0 + w + 18;
    const ImVec2 at(hx, y0);
    const bool spell = item.name.empty() && !s.handSpell[hk].empty();
    const bool filled = !item.name.empty() || spell;
    const ui::SlotLook look = filled ? ui::SlotLook::Filled : ui::SlotLook::Empty;
    dl->AddRectFilled(ImVec2(at.x - 6, at.y - 6), ImVec2(at.x + kSlot + 6, at.y + kSlot + 6),
                      IM_COL32(0, 0, 0, 90));
    ui::SlotSurface(dl, at, kSlot, look, false);
    SlotRim(dl, at, look);
    const ImVec2 mid(at.x + kSlot * 0.5f, at.y + kSlot * 0.5f);
    if (spell) {
      SpellContents(s, dl, at, s.handSpell[hk], s.handSpellPage[hk], s.handSpellType[hk],
                    s.handSpellColor[hk], 1.0f);
    } else if (filled) {
      ui::DrawSpriteCentered(dl, ItemIcon(item.kind), ImVec2(mid.x + 1, mid.y + 1),
                             Fade(ui::ColInk(), 0.7f));
      ui::DrawSpriteCentered(dl, ItemIcon(item.kind), mid,
                             item.dyeSwatch ? item.dyeSwatch : IM_COL32_WHITE);
      if (item.fill >= 0.0f) {
        const float gx0 = at.x + 6, gx1 = at.x + kSlot - 6, gy = at.y + kSlot - 8;
        dl->AddRectFilled(ImVec2(gx0, gy), ImVec2(gx1, gy + 4), IM_COL32(10, 10, 14, 220));
        const float fx = std::floor(gx0 + (gx1 - gx0) * item.fill);
        if (fx > gx0)
          dl->AddRectFilled(ImVec2(gx0, gy), ImVec2(fx, gy + 4), IM_COL32(90, 160, 235, 255));
      }
    } else {
      ui::DrawSpriteCentered(dl, "slot_hands", mid, Fade(ui::ColParch(), 0.35f));
    }
    const bool framed = live && s.lastHand == hk;
    if (framed)
      dl->AddRect(ImVec2(at.x - 3, at.y - 3), ImVec2(at.x + kSlot + 3, at.y + kSlot + 3),
                  ui::ColGoldHi(), 0.0f, 0, 2.0f);
    ui::ShadowText(dl, ImVec2(at.x + 4, at.y + 2),
                   framed ? ui::ColGoldHi() : Fade(ui::ColParch(), 0.7f),
                   hk == 1 ? "Q" : "E");
    // Under the slot: the button, and a vessel's mode.
    char cap[32];
    const int mode = s.vesselModeShown[hk];
    if (mode >= 0 && mode < 3)
      std::snprintf(cap, sizeof cap, "%s %s", hk == 1 ? "RMB" : "LMB", kModeName[mode]);
    else if (spell)
      std::snprintf(cap, sizeof cap, "%s cast", hk == 1 ? "RMB" : "LMB");
    else
      std::snprintf(cap, sizeof cap, "%s", hk == 1 ? "RMB" : "LMB");
    const ImVec2 cs = ImGui::CalcTextSize(cap);
    ui::ShadowText(dl, ImVec2(std::floor(at.x + (kSlot - cs.x) * 0.5f), at.y - cs.y - 4),
                   Fade(ui::ColParch(), live || spell ? 0.85f : 0.4f), cap);
  }
  if (live && sel >= 0 && sel < n) {
    // A gold frame round the selection, 2 px, outside the rim so it never
    // covers the icon — the stack Q and E would put in a hand.
    const ImVec2 at(x0 + sel * (kSlot + kGap), y0);
    dl->AddRect(ImVec2(at.x - 3, at.y - 3), ImVec2(at.x + kSlot + 3, at.y + kSlot + 3),
                ui::ColGoldHi(), 0.0f, 0, 2.0f);
    // The name, fading 2 s after the selection last changed.
    static int lastSel = -1;
    static double changedAt = 0.0;
    const double now = ImGui::GetTime();
    if (sel != lastSel) {
      lastSel = sel;
      changedAt = now;
    }
    const float a = std::clamp(2.5f - (float)(now - changedAt), 0.0f, 1.0f);
    const std::string& name = s.hotbarSlots[sel].name;
    if (a > 0.0f && !name.empty()) {
      const ImVec2 ts = ImGui::CalcTextSize(name.c_str());
      ui::ShadowText(dl, ImVec2(std::floor((disp.x - ts.x) * 0.5f), y0 - 10 - ts.y),
                     Fade(ui::ColParch(), a), name.c_str());
    }
  }
  ImGui::PopFont();
}
