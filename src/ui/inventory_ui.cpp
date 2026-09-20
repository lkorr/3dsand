#include "ui/inventory_ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include <imgui.h>

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
constexpr const char* kPayloadGlyph = "SVGLY";
// A grimoire page by NAME (plan §12b), and a word's index inside the page
// being composed (reordering within the row). A BOUND key travels as its slot
// index: what is on it is already in a mirror both ends can read, and a slot is
// the one thing a key-to-key move needs to name.
constexpr const char* kPayloadPage = "SVPGE";
constexpr const char* kPayloadWord = "SVWRD";
constexpr const char* kPayloadBound = "SVBND";

// Peeking at a payload BEFORE the button is released: what makes the row able
// to draw the drop it is about to perform instead of performing it silently.
constexpr ImGuiDragDropFlags kPeek = ImGuiDragDropFlags_AcceptBeforeDelivery |
                                     ImGuiDragDropFlags_AcceptNoDrawDefaultRect;

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
  return "item_unknown";
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

const char* GlyphIcon(int type) {
  // GlyphSort: 0 matter, 1 effect, 2 delivery, 3 mod, 4 operator.
  switch (type) {
    case 2: return "glyph_form";
    case 0: return "glyph_element";
    default: return "glyph_modifier";
  }
}

// The sort's colour (plan §12a): matter, effect, delivery, mod, operator. Used
// on the arsenal's column rules, the cells' edge tags and the live readout,
// so the sentence you are speaking and the panel you bound it from agree.
ImU32 SortColour(int sort) {
  switch (sort) {
    case 0: return IM_COL32(120, 190, 120, 255);   // matter: green
    case 1: return IM_COL32(232, 138, 46, 255);    // effect: ember
    case 2: return IM_COL32(76, 132, 200, 255);    // delivery: mana blue
    case 3: return IM_COL32(200, 184, 138, 255);   // mod: parchment
    default: return IM_COL32(217, 190, 110, 255);  // operator: gold
  }
}

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
void BeginTip() {
  ImGui::BeginTooltip();
  ImGui::PushFont(ui::FontSmall());
  ImGui::PushTextWrapPos(320.0f);
}
void EndTip() {
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
  ImGui::EndTooltip();
}
void Tip(const char* text) {
  BeginTip();
  ImGui::TextUnformatted(text);
  EndTip();
}

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
    ui::DrawSpriteCentered(dl, ItemIcon(item.kind), ImVec2(mid.x + 1, mid.y + 1),
                           Fade(ui::ColInk(), 0.7f));   // pixel drop shadow
    // THE ICON TAKES THE DYE. The atlas is keyed on kind, so three tunics in
    // three colours are one picture three times — tinting the sprite is what
    // makes them three items in the pack instead of a stack that will not
    // stack. Undyed items pass 0 and draw in the atlas's own ink, exactly as
    // before.
    ui::DrawSpriteCentered(dl, ItemIcon(item.kind), mid,
                           item.dyeSwatch ? item.dyeSwatch : IM_COL32_WHITE);
    if (item.dyeSwatch) {
      // ...and a hard 4 px chip in the corner, because a tinted pixel-art icon
      // at this size reads as a lighting change rather than as a colour. The
      // chip is the unambiguous one.
      dl->AddRectFilled(ImVec2(at.x + 3, at.y + 3), ImVec2(at.x + 9, at.y + 9),
                        item.dyeSwatch);
      dl->AddRect(ImVec2(at.x + 3, at.y + 3), ImVec2(at.x + 9, at.y + 9),
                  Fade(ui::ColInk(), 0.8f));
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
void GlyphContents(ImDrawList* dl, ImVec2 at, const UIState::GlyphUI& g) {
  const ImVec2 mid(at.x + kSlot * 0.5f, at.y + kSlot * 0.5f);
  if (g.color) {
    const ImU32 sw = IM_COL32((g.color) & 0xFF, (g.color >> 8) & 0xFF,
                              (g.color >> 16) & 0xFF, 255);
    // Three nested rects at rising alpha: a stepped radial glow.
    dl->AddRectFilled(ImVec2(at.x + 4, at.y + 4), ImVec2(at.x + kSlot - 4, at.y + kSlot - 4),
                      Fade(sw, 0.16f));
    dl->AddRectFilled(ImVec2(at.x + 8, at.y + 8), ImVec2(at.x + kSlot - 8, at.y + kSlot - 8),
                      Fade(sw, 0.22f));
    dl->AddRectFilled(ImVec2(at.x + 12, at.y + 12), ImVec2(at.x + kSlot - 12, at.y + kSlot - 12),
                      Fade(sw, 0.30f));
    // And a 2 px strip of the pure colour along the bottom: the tag.
    dl->AddRectFilled(ImVec2(at.x + 6, at.y + kSlot - 6), ImVec2(at.x + kSlot - 6, at.y + kSlot - 4),
                      Fade(sw, 0.9f));
  }
  ui::DrawSpriteCentered(dl, GlyphIcon(g.type), ImVec2(mid.x + 1, mid.y + 1),
                         Fade(ui::ColInk(), 0.7f));
  ui::DrawSpriteCentered(dl, GlyphIcon(g.type), mid);
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
void GlyphInfoBox(const UIState::GlyphUI& g, const char* sortName) {
  BeginTip();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::ColGoldHi()));
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

// ---- the live portrait ------------------------------------------------------
//
// The image is whatever main.cpp rendered into the offscreen target this
// frame: the real rig, with its real damage, its real pose and whatever is in
// its hand. Nothing here knows any of that — which is the point, and is why a
// severed arm shows up in the panel with no UI code for severed arms.
void Portrait(UIState& s, ImVec2 at, ImVec2 size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 br(at.x + size.x, at.y + size.y);
  dl->AddRectFilled(at, br, IM_COL32(16, 12, 22, 255));
  ui::InnerShadow(dl, at, br, 10.0f, 0.5f);

  ImGui::SetCursorScreenPos(at);
  ImGui::InvisibleButton("##portrait", size,
                         ImGuiButtonFlags_MouseButtonLeft |
                         ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered();
  const bool dragL = ImGui::IsItemActive() &&
                     ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f);
  const bool dragR = ImGui::IsItemActive() &&
                     ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f);

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

  // ORBIT (left drag).
  if (dragL) {
    const ImVec2 d = ImGui::GetIO().MouseDelta;
    s.portraitYaw -= d.x * 0.012f;
    s.portraitPitch = std::clamp(s.portraitPitch - d.y * 0.008f, -0.9f, 0.9f);
  }
  // PAN (right drag): instant — writes both current and target.
  if (dragR) {
    const ImVec2 d = ImGui::GetIO().MouseDelta;
    const float dx = -d.x * 0.006f, dy = d.y * 0.006f;
    s.portraitPanX += dx;  s.portraitPanXTarget += dx;
    s.portraitPanY += dy;  s.portraitPanYTarget += dy;
  }
  // ZOOM (scroll wheel): instant — writes both current and target.
  if (hovered) {
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
  if (hovered && s.bodyValid &&
      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    const ImVec2 m = ImGui::GetMousePos();
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
    if (best >= 0) {
      s.portraitFocusSlot = best;
      s.portraitPivotSlot = best;
    }
  }

  // SINGLE-CLICK to select a limb in inspect mode. Detected on deactivation
  // (release) so drags do not fire it. The distance gate separates a click
  // from an orbit that barely moved, and the click-count gate lets double-
  // click-to-frame through without also selecting.
  if (s.inspectMode && s.bodyValid &&
      ImGui::IsItemDeactivated() &&
      ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
      ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 9.0f &&
      ImGui::GetIO().MouseClickedCount[0] < 2) {
    const ImVec2 m = ImGui::GetMousePos();
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
    s.inspectSelected = best;
  }

  // HEAD LOOK: only while not dragging at all.
  s.portraitLookValid = hovered && !dragL && !dragR;
  if (s.portraitLookValid) {
    const ImVec2 m = ImGui::GetMousePos();
    s.portraitLook[0] = std::clamp((m.x - at.x) / size.x * 2.0f - 1.0f, -1.0f,
                                   1.0f);
    s.portraitLook[1] = std::clamp(1.0f - (m.y - at.y) / size.y * 2.0f, -1.0f,
                                   1.0f);
  }

  if (hovered) ui::Glow(dl, at, br, ui::ColGold(), 6.0f, 0.22f);
  ui::Draw9(dl, "panel_inner", at, br);

  // RESET button — shown when the target differs from the default (not the
  // current, so it stays visible while the animation is still closing).
  const bool dirty = s.portraitZoomTarget != 1.0f ||
                     std::abs(s.portraitPanXTarget) > 0.001f ||
                     std::abs(s.portraitPanYTarget) > 0.001f;
  if (dirty) {
    if (ui::Button("##portraitreset", ImVec2(br.x - 54, br.y - 28), "reset",
                   false, 48))
      s.portraitReset = true;
  } else if (hovered && !dragL && !dragR) {
    const char* hint = "drag to turn  .  scroll to zoom  .  right-drag to pan";
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

// CAST ON A PART. With a sentence on the stack, every present limb of the
// inspector becomes a target: click it and the spell resolves there with
// `self` (docs/PLAN_magic_grammar.md §7). The panel never touches the VM — it
// latches the slot, and main.cpp turns the slot into a position and casts.
void InspectCastPicks(UIState& s, ImVec2 at, ImVec2 size) {
  if (!s.bodyValid || s.spellText.empty()) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float flash = 0.5f + 0.5f * (float)std::sin(ImGui::GetTime() * 3.0);
  for (int i = 0; i < UIState::kSlotCount; i++) {
    const UIState::BodyPartUI& b = s.body[i];
    if (!b.present || b.severed || !b.projValid) continue;
    ImVec2 p0(at.x + b.projMin[0] * size.x, at.y + b.projMin[1] * size.y);
    ImVec2 p1(at.x + b.projMax[0] * size.x, at.y + b.projMax[1] * size.y);
    // CLAMPED, not merely clipped: this one has a hit box on it, and a target
    // you cannot see is a target you cannot have meant to click.
    if (!ClipToPortrait(at, size, p0, p1)) continue;
    if (p1.x - p0.x < 2.0f || p1.y - p0.y < 2.0f) continue;
    ImGui::SetCursorScreenPos(p0);
    ImGui::PushID(1000 + i);
    ImGui::InvisibleButton("##castpart", ImVec2(p1.x - p0.x, p1.y - p0.y));
    const bool hot = ImGui::IsItemHovered();
    if (hot) {
      // A pixel outline, not an anti-aliased stroke: the frame reads as "this
      // is where it lands".
      const ImU32 col = Fade(IM_COL32(150, 200, 255, 255), 0.5f + 0.5f * flash);
      dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y + 2), col);
      dl->AddRectFilled(ImVec2(p0.x, p1.y - 2), ImVec2(p1.x, p1.y), col);
      dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p0.x + 2, p1.y), col);
      dl->AddRectFilled(ImVec2(p1.x - 2, p0.y), ImVec2(p1.x, p1.y), col);
      BeginTip();
      ImGui::Text("cast %s here", s.spellText.c_str());
      EndTip();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      s.castAtPart.pending = true;
      s.castAtPart.slot = i;
    }
    ImGui::PopID();
  }
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
constexpr int kUndoDepth = 32;
void PushUndo(UIState& s) {
  s.grimoireUndo.push_back(s.grimoireEditWords);
  if ((int)s.grimoireUndo.size() > kUndoDepth) s.grimoireUndo.erase(s.grimoireUndo.begin());
  // A new edit is a new future: whatever ctrl+Z had set aside is gone.
  s.grimoireRedo.clear();
}
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
      if (g.type == kSortOrder[c]) n++;
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
        if (g.type == sort) n++;
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
        if (g.type != sort) continue;
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
        ImGui::TextDisabled("%s speaks it  .  right-click to unbind", key);
        ImGui::TextDisabled("drag it onto another key to move or swap it  .  drag it out to unbind");
      } else if (isPage) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::ColGoldHi()));
        ImGui::Text("[%s]  a page", id.c_str());
        ImGui::PopStyleColor();
        const std::string& ro = i < (int)s.glyphSlotReadouts.size() ? s.glyphSlotReadouts[i] : "";
        ImGui::TextDisabled("%s", ro.empty() ? "(a page that names nothing)" : ro.c_str());
        ImGui::TextDisabled("%s speaks it  .  right-click to unbind", key);
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
  // ---- the page list ----
  ImGui::SetCursorScreenPos(at);
  ImGui::BeginChild("##pages", ImVec2(kListW, size.y), ImGuiChildFlags_None,
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
                          Fade(ui::ColGold(), 0.14f));
      cd->AddText(ImVec2(base.x + 8, py + 4),
                  hov || sel ? ui::ColGoldHi() : Fade(ui::ColParchDim(), 0.9f), "+ new page");
      if (ImGui::IsItemClicked()) {
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
      if (sel)
        cd->AddRectFilled(ImVec2(base.x, py), ImVec2(base.x + rowW, py + kRow),
                          Fade(ui::ColGold(), 0.18f));
      // A 2 px gold tag for a starter (authored, read-only), a parchment one
      // for the player's own; the name after it, clipped to the list.
      cd->AddRectFilled(ImVec2(base.x, py + 4), ImVec2(base.x + 2, py + kRow - 4),
                        p.readOnly ? Fade(ui::ColGoldDim(), 0.9f) : Fade(ui::ColParchDim(), 0.9f));
      cd->PushClipRect(ImVec2(base.x, py), ImVec2(base.x + rowW - 2, py + kRow), true);
      cd->AddText(ImVec2(base.x + 8, py + 4),
                  hov || sel ? ui::ColGoldPale() : Fade(ui::ColParch(), 0.9f), p.name.c_str());
      cd->PopClipRect();
      if (p.dropped > 0)
        cd->AddText(ImVec2(base.x + rowW - 12, py + 4), ui::ColBloodHi(), "?");
      if (ImGui::IsItemClicked()) {
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
        ImGui::TextDisabled("click to open  .  right-click to nest it in the open page  .  drag onto a key to bind it");
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
  ImGui::BeginChild("##compose", ImVec2(compW, size.y), ImGuiChildFlags_None,
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
    // The name.
    {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%s", s.grimoireEditName.c_str());
      ImGui::SetCursorScreenPos(ImVec2(base.x, cy));
      ImGui::PushItemWidth(innerW);
      if (readOnly) ImGui::BeginDisabled();
      if (ImGui::InputTextWithHint("##pagename", "name this page", buf, sizeof buf)) {
        s.grimoireEditName = buf;
        s.grimoireEditDirty = true;
      }
      if (readOnly) ImGui::EndDisabled();
      ImGui::PopItemWidth();
      if (ImGui::IsItemHovered())
        Tip(readOnly ? "An authored page: copy it to edit."
                     : "The page's name: what a bound key speaks.");
      cy += ImGui::GetFrameHeight() + 8;
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
    const int nWords = (int)s.grimoireEditWords.size();
    const int cells = std::max(1, s.grimoireMaxWords);
    const int perRow = std::max(1, (int)((innerW + (kCell - kSlot)) / kCell));
    const float rowY = cy;
    auto cellAt = [&](int i) {
      return ImVec2(base.x + (i % perRow) * kCell, rowY + (i / perRow) * kCell);
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
      ImGui::InvisibleButton("##wd", ImVec2(kSlot, kSlot));
      const bool hov = ImGui::IsItemHovered();
      // The LIVE size, not `nWords`: a right-click below removes a word from
      // under the rest of this very loop, and the last cell would then index
      // one past the end of a vector that has already shrunk.
      const bool has = i < (int)s.grimoireEditWords.size();
      const ui::SlotLook look = hov ? ui::SlotLook::Hover
                                : has ? ui::SlotLook::Filled
                                      : ui::SlotLook::Empty;
      ui::SlotSurface(cd, p, kSlot, look, false);
      SlotRim(cd, p, look);
      if (has) {
        const std::string& w = s.grimoireEditWords[i];
        const UIState::GlyphUI* g = FindGlyph(s, w);
        const UIState::GrimoirePageUI* pg = g ? nullptr : FindPageUI(s, w);
        if (g) {
          GlyphContents(cd, p, *g);
          cd->AddRectFilled(ImVec2(p.x + 2, p.y + 4), ImVec2(p.x + 4, p.y + kSlot - 4),
                            Fade(SortColour(g->type), 0.9f));
        } else if (pg) {
          PageContents(cd, p);
        } else {
          cd->AddText(ImVec2(p.x + kSlot * 0.5f - 6, p.y + kSlot * 0.5f - 13), ui::ColBloodHi(), "?");
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
          cd->AddRectFilled(p, ImVec2(p.x + kSlot, p.y + kSlot), Fade(ui::ColInk(), 0.55f));
        if (hov) {
          BeginTip();
          ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::ColGoldHi()));
          if (g) ImGui::TextUnformatted(w.c_str());
          else if (pg) ImGui::Text("[%s]  a page", w.c_str());
          else ImGui::Text("%s  (a word that no longer exists)", w.c_str());
          ImGui::PopStyleColor();
          if (g) ImGui::TextDisabled("%s  .  %s", kSortLabel[g->type], g->desc.c_str());
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
    // THE MARKER LAYER, over the whole row so a marker at a cell's edge is
    // never buried by the next cell's recess.
    if (caretAt >= 0) {
      const ImVec2 cp = cellAt(caretAt);
      // In the first column the seam would fall outside the child's clip, so
      // it sits just inside the cell instead of just before it.
      const float x = std::floor(caretAt % perRow == 0 ? cp.x + 1 : cp.x - 3);
      cd->AddRectFilled(ImVec2(x, cp.y - 2), ImVec2(x + 4, cp.y + kSlot + 2), ui::ColGoldHi());
      cd->AddRectFilled(ImVec2(x - 3, cp.y - 5), ImVec2(x + 7, cp.y - 1), ui::ColGoldHi());
      cd->AddRectFilled(ImVec2(x - 3, cp.y + kSlot + 1), ImVec2(x + 7, cp.y + kSlot + 5),
                        ui::ColGoldHi());
    }
    if (swapA >= 0) {
      const int pair[2] = {swapA, swapB};
      for (int k : pair) {
        const ImVec2 cp = cellAt(k);
        cd->AddRect(ImVec2(cp.x - 2, cp.y - 2), ImVec2(cp.x + kSlot + 2, cp.y + kSlot + 2),
                    ui::ColGoldHi(), 0.0f, 0, 2.0f);
      }
      // Two arrowheads back to back in the gap, pointing at where each word is
      // going. Only when the pair is side by side — across a row break there
      // is no gap to put them in, and the two lit cells say it alone.
      if (swapA / perRow == swapB / perRow) {
        const ImVec2 cp = cellAt(std::min(swapA, swapB));
        const float mx = cp.x + kSlot + (kCell - kSlot) * 0.5f, my = cp.y + kSlot * 0.5f;
        cd->AddTriangleFilled(ImVec2(mx - 7, my), ImVec2(mx - 1, my - 5), ImVec2(mx - 1, my + 5),
                              ui::ColGoldHi());
        cd->AddTriangleFilled(ImVec2(mx + 7, my), ImVec2(mx + 1, my - 5), ImVec2(mx + 1, my + 5),
                              ui::ColGoldHi());
      }
    }
    if (refuseAt >= 0) {
      const ImVec2 cp = cellAt(refuseAt);
      cd->AddRectFilled(cp, ImVec2(cp.x + kSlot, cp.y + kSlot), Fade(ui::ColBlood(), 0.35f));
      SlotRim(cd, cp, ui::SlotLook::Refuse);
    }
    cy += ((cells + perRow - 1) / perRow) * kCell + 4;

    // The readout and the price, in small type on a dark page: this is what
    // the row MEANS, from main.cpp's DescribeSpell of it.
    {
      ImGui::PushFont(ui::FontSmall());
      const char* ro = nWords == 0 ? "an empty page - drop words into the row above"
                                   : s.grimoireEditReadout.empty() ? "(says nothing)"
                                                                   : s.grimoireEditReadout.c_str();
      const float wrapW = innerW - 20;
      const ImVec2 ts = ImGui::CalcTextSize(ro, nullptr, false, wrapW);
      const float boxH = std::max(2 * 13.0f, ts.y) + 12;
      const ImVec2 a(base.x, cy), b(base.x + innerW, cy + boxH);
      cd->AddRectFilled(a, b, Fade(ui::ColInk(), 0.42f));
      cd->AddRectFilled(a, ImVec2(a.x + 2, b.y), Fade(ui::ColGoldDim(), 0.7f));
      cd->AddText(ui::FontSmall(), 13.0f, ImVec2(a.x + 10, a.y + 6),
                  nWords == 0 ? Fade(ui::ColParchDim(), 0.8f) : ui::ColParch(), ro, nullptr,
                  wrapW);
      cy += boxH + 6;
      char price[96];
      std::snprintf(price, sizeof price, "price %d%s      %d / %d words", s.grimoireEditPrice,
                    s.grimoireEditPriceUnknown ? " + ?" : "", nWords, s.grimoireMaxWords);
      cd->AddText(ImVec2(base.x + 2, cy), Fade(ui::ColParchDim(), 0.9f), price);
      cy += 16;
      // The row's gestures, said once where the row is. Four of the five are
      // invisible otherwise: you find a swap by trying it.
      if (!readOnly && nWords > 0) {
        static const char* kGestures =
            "drag to reorder  .  onto a neighbour to swap  .  ctrl+drag to copy  .  "
            "right-click to remove  .  ctrl+z undo";
        cd->AddText(ui::FontSmall(), 13.0f, ImVec2(base.x + 2, cy), Fade(ui::ColParchDim(), 0.75f),
                    kGestures, nullptr, innerW - 4);
        cy += ImGui::CalcTextSize(kGestures, nullptr, false, innerW - 4).y;
      }
      ImGui::PopFont();
      cy += 6;
    }

    // Save / Duplicate / Delete, in the screen's own button. A disabled one is
    // drawn and then dimmed: the row keeps its shape whichever page is open.
    {
      float bx = base.x;
      auto button = [&](const char* id, const char* label, bool enabled) {
        if (!enabled) ImGui::BeginDisabled();
        const bool clicked = ui::Button(id, ImVec2(bx, cy), label, false, 80);
        if (!enabled) {
          ImGui::EndDisabled();
          cd->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                            Fade(ui::ColInk(), 0.45f));
        }
        bx = ImGui::GetItemRectMax().x + 8;
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
      cy += ImGui::GetTextLineHeight() + 8 + 6;
    }

    // The status line: what just happened, else what to do next.
    {
      ImGui::PushFont(ui::FontSmall());
      const char* msg = nullptr;
      ImU32 col = Fade(ui::ColParchDim(), 0.85f);
      // A live drag speaks FIRST: "this page is full" while you are still
      // holding the word is a refusal you can act on, and the same sentence
      // after the release is only a report.
      if (rowNote) {
        msg = rowNote;
        col = refuseAt >= 0 ? ui::ColBloodHi() : ui::ColGoldHi();
      } else if (!s.kitMessage.empty() && s.kitMessageAge < 4.0f) {
        msg = s.kitMessage.c_str();
        col = ui::ColEmber();
      } else if (readOnly) {
        msg = "an authored page: copy it to make one of your own";
      } else if (s.grimoireEditDirty) {
        msg = "unsaved";
        col = ui::ColGoldHi();
      } else if (!sel && nWords == 0) {
        msg = "drag words in from the arsenal, or press = in magic mode to capture the stack";
      }
      if (msg) {
        cd->AddText(ui::FontSmall(), 13.0f, ImVec2(base.x + 2, cy), col, msg, nullptr, innerW - 4);
        cy += ImGui::CalcTextSize(msg, nullptr, false, innerW - 4).y + 4;
      }
      ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(base);
    ImGui::Dummy(ImVec2(innerW, cy - base.y));
  }
  ImGui::EndChild();
}

// ---- THE LOOT PANEL (game/corpses.h) ----------------------------------------
//
// A corpse's gear, drawn where the grimoire (wide) or the arsenal (narrow)
// otherwise sits: above or beside the pack, so a piece drags into your bag
// along one short line. Take-only - every slot here is a drag SOURCE and a
// right-click TAKE, and a drag INTO one is refused by main.cpp with the
// reason (a thing put on a corpse would have no body in the world).
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
//   POOLS    health, mana, and how much of this body is still physically here
//   TRIAGE   everything currently wrong, worst first, every row clickable
//   THE LIMB the selected part in full — what it is made of, what happened to
//            it, what is on it, and what is worn over it
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

  // ======================= POOLS ==============================================
  y = ui::Subheading(dl, ImVec2(base.x, y), w, "VITALS") + 4;
  y += PoolBar(dl, ImVec2(base.x, y), w, s.health, s.healthMax, s.healthCap,
               ui::ColBloodHi(), "HEALTH", 22.0f, true) + 6;
  y += PoolBar(dl, ImVec2(base.x, y), w, s.mana, s.manaMax, -1,
               ui::ColMana(), "MANA", 22.0f, true) + 6;

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
      char up[sizeof s.stainLabel];
      size_t k = 0;
      for (; k + 1 < sizeof up && s.stainLabel[k]; k++)
        up[k] = (char)std::toupper((unsigned char)s.stainLabel[k]);
      up[k] = '\0';
      chip(ui::Mix(s.stainColor, IM_COL32_WHITE, 0.35f), false, "%s %.0f%%",
           k ? up : "COATED", s.stainFrac * 100.0f);
    }
    if (!s.locoState.empty())
      chip(ui::ColParchDim(), false, "%s", s.locoState.c_str());
    if (!anyChip) chip(ui::ColParchDim(), false, "steady");
    ImGui::PopFont();
    y = cy + lineH + 10;
  }

  // ======================= TRIAGE =============================================
  //
  // WHAT IS WRONG, WORST FIRST, AND NOTHING ELSE. A limb with nothing to
  // report does not appear — the portrait already says it is there. Each row
  // is a button: clicking it selects the limb, which lights its callout on the
  // portrait, lights the piece of armour covering it, and fills the section
  // below. That is the whole navigation model of this column.
  y = ui::Subheading(dl, ImVec2(base.x, y), w, "TRIAGE") + 4;
  {
    struct Alarm { int slot; int sev; ImU32 col; char text[40]; };
    Alarm alarms[UIState::kSlotCount * 3];
    int na = 0;
    auto push = [&](int slot, int sev, ImU32 col, const char* fmt, ...) {
      if (na >= (int)(sizeof alarms / sizeof alarms[0])) return;
      Alarm& a = alarms[na++];
      a.slot = slot;
      a.sev = sev;
      a.col = col;
      va_list ap;
      va_start(ap, fmt);
      std::vsnprintf(a.text, sizeof a.text, fmt, ap);
      va_end(ap);
    };
    for (int i = 0; i < UIState::kSlotCount; i++) {
      const UIState::BodyPartUI& b = s.body[i];
      if (!b.present) continue;
      if (b.severed) { push(i, 0, ui::ColBloodHi(), "SEVERED"); continue; }
      if (b.burningVoxels > 0)
        push(i, 1, ui::ColEmber(), "BURNING %u", b.burningVoxels);
      if (b.bleeding) push(i, 2, ui::ColBloodHi(), "BLEEDING");
      if (b.hpFrac < 0.35f)
        push(i, 3, ui::ColBloodHi(), "CRITICAL  %.0f%% hp", b.hpFrac * 100.0f);
      else if (b.hpFrac < 0.8f)
        push(i, 5, ui::ColBlood(), "hurt  %.0f%% hp", b.hpFrac * 100.0f);
      if (b.voxelFrac < 0.6f)
        push(i, 4, ui::ColSteel(), "HOLLOW  %.0f%% left", b.voxelFrac * 100.0f);
      if (b.charredFrac > 0.25f)
        push(i, 6, ui::ColEmber(), "charred  %.0f%%", b.charredFrac * 100.0f);
    }
    std::stable_sort(alarms, alarms + na,
                     [](const Alarm& a, const Alarm& b) { return a.sev < b.sev; });
    if (na == 0) {
      small(Fade(ui::ColParchDim(), 0.85f), ImVec2(base.x + 4, y),
            s.bodyValid ? "Not a scratch." : "No body to inspect.");
      ImGui::PushFont(ui::FontSmall());
      y += ImGui::GetTextLineHeight() + 8;
      ImGui::PopFont();
    }
    ImGui::PushFont(ui::FontSmall());
    const float rowH = ImGui::GetTextLineHeight() + 4;
    for (int i = 0; i < na; i++) {
      const Alarm& a = alarms[i];
      const bool sel = s.inspectSelected == a.slot;
      ImGui::SetCursorScreenPos(ImVec2(base.x, y));
      ImGui::PushID(6000 + i);
      ImGui::InvisibleButton("##alarm", ImVec2(w, rowH));
      const bool hot = ImGui::IsItemHovered();
      if (ImGui::IsItemClicked()) s.inspectSelected = a.slot;
      ImGui::PopID();
      if (sel || hot)
        dl->AddRectFilled(ImVec2(base.x, y), ImVec2(base.x + w, y + rowH),
                          Fade(ui::ColGold(), sel ? 0.16f : 0.08f));
      // A 3 px severity stripe down the left edge: the column can be read for
      // "how bad is this body" without reading a word of it.
      dl->AddRectFilled(ImVec2(base.x, y), ImVec2(base.x + 3, y + rowH),
                        a.sev <= 2 ? Fade(a.col, 0.55f + 0.45f * pulse)
                                   : Fade(a.col, 0.75f));
      ui::ShadowText(dl, ImVec2(base.x + 9, y + 2),
                     sel ? ui::ColGoldHi() : ui::ColParch(),
                     s.body[a.slot].label);
      const ImVec2 ts = ImGui::CalcTextSize(a.text);
      ui::ShadowText(dl, ImVec2(base.x + w - ts.x - 4, y + 2), a.col, a.text);
      y += rowH;
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
  // The pack is exactly as tall as its grid and its hotbar; whatever column
  // it shares gives it that and keeps the rest.
  const float lineH = ImGui::GetTextLineHeight();
  const float packH = kFrame + ui::kHeaderH + 14 + s.bagRows * (kSlot + kSlotGap) + 10 +
                      (lineH + 6) + 2 + kSlot + 12 + kFrame;
  // COLUMN ORDER, left to right: character | grimoire over pack | arsenal.
  //
  // The PACK is next to the body that wears what is in it, which is the drag
  // everyone makes and makes constantly — bag to armour slot, corpse to bag —
  // and it used to be the longest line on the screen, across a whole column of
  // glyph table. The ARSENAL goes to the far edge: it is a READ surface most
  // of the time (every word you know, in one table) and the drags that start
  // in it end one column over in the grimoire, which is still adjacent.
  //
  // The grimoire column is ten cells wide (the eight-column bag fits inside);
  // the third column exists only when the window has room for the arsenal
  // beside the other two and height for a composer above the pack.
  const float grimX = midX;
  const float grimW = kPad * 2 + (kKeyCols - 1) * kCell + kSlot;
  const float arsX = midX + grimW + kColGap;
  const float arsRoom = disp.x - 28.0f - arsX;
  const bool wide = arsRoom >= arsenalW && (bottom - top) >= packH + kColGap + 300.0f;
  const float grimH = bottom - top - packH - kColGap;

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
    {
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
    if (s.inspectMode) {
      InspectOverlay(s, ImVec2(portX, portY), ImVec2(kPortraitW, kPortraitH));
      InspectCastPicks(s, ImVec2(portX, portY), ImVec2(kPortraitW, kPortraitH));
    }
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
    y = portY + kPortraitH + 16;

    // Sheath + quick slots: what is on your person but not in your hand.
    y = ui::Subheading(dl, ImVec2(wp.x + kPad, y), leftBase - kPad * 2,
                       "ON YOUR PERSON");
    y += 2;
    for (int k = 0; k < 5; k++) {
      const int idx = 8 + k;  // Sheath, Quick0..3
      const UIState::EquipSlotUI& d =
          idx < (int)s.equipDefs.size() ? s.equipDefs[idx]
                                        : UIState::EquipSlotUI{};
      char id[32];
      std::snprintf(id, sizeof id, "eq%d", idx);
      ItemSlot(s, id,
               ImVec2(wp.x + kPad + k * (kSlot + kSlotGap), y),
               SlotOr(s.equipSlots, idx), KitRef{KitSpace::Equip, idx},
               d.icon.c_str(), d.acceptsAnything, d.why.c_str(), false,
               &d.accepts);
    }
    y += kSlot + 18;

    if (showDetail) {
      // The pools have moved into the health column (see below), so the foot
      // of this one answers the question the column cannot: how much of this
      // body is behind metal at all, and what state that metal is in.
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
      // THE HEALTH COLUMN OWNS THE POOLS while it is open — it leads with
      // them, and one screen saying "142 / 200" in two places invites the two
      // to disagree the day one of them is changed. The column runs the FULL
      // height of the panel, from the top of the portrait to the bottom rule.
      const float detailX = wp.x + leftBase + kColGap;
      VitalsColumn(s, ImVec2(detailX, portY),
                   ImVec2(wp.x + ws.x - kPad - detailX,
                          wp.y + ws.y - kPad - portY));
    } else {
      // Health + mana, the same two pools the HUD shows, so the screen and the
      // corner never disagree about how close you are to dead.
      const float barW = ws.x - kPad * 2;
      y += PoolBar(dl, ImVec2(wp.x + kPad, y), barW, s.health, s.healthMax,
                   s.healthCap, ui::ColBloodHi(), "HEALTH") + 8;
      y += PoolBar(dl, ImVec2(wp.x + kPad, y), barW, s.mana, s.manaMax, -1,
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
    }
  }
  ImGui::End();

  // ==========================================================================
  // THE RIGHT TWO COLUMNS: grimoire over pack | arsenal (plan §12a, §12b)
  // ==========================================================================
  // The ARSENAL is a full column of its own: the twenty bound keys, then every
  // word in the library in a table with one row-band per sort. That table is
  // the drag SOURCE for both of the other panels, and a source you cannot see
  // while you compose is no source — the first version put the grimoire
  // behind a mode toggle on the arsenal, and the grid it needed vanished the
  // moment the toggle was pressed. The GRIMOIRE sits above the PACK in the
  // middle column, so a page drags onto a key and a glyph drags into a page
  // along one short horizontal line each, and a garment drags onto the body
  // along another. When the window has no room for a third column the old
  // arrangement returns: the arsenal toggles between its table and the
  // grimoire, over the pack.
  const float arsenalH = wide ? (bottom - top) : (bottom - top - packH - kColGap);
  ui::PanelStyle stLoot;
  stLoot.darkMix = 0.40f;
  stLoot.sheenPeak = 0.36f;
  stLoot.bronzeAlpha = 0.14f;
  // While a corpse is open, its panel takes the place a drag would otherwise
  // have to cross: the grimoire's slot above the pack when the window is wide,
  // the arsenal's beside it when it is not. Looting is a moment, the arsenal is
  // not going anywhere, and a fourth column would not fit on most screens.
  const bool lootHere = s.lootOpen && !wide;
  // Wide: the arsenal is the OUTER column. Narrow: it is the only one on the
  // right, and it takes the middle slot with the pack under it.
  const float arsenalX = wide ? arsX : midX;
  if (lootHere) {
    LootPanel(s, ImVec2(arsenalX, top), ImVec2(arsenalW, arsenalH), stLoot,
              kPanelFlags);
  } else {
  ImGui::SetNextWindowPos(ImVec2(arsenalX, top));
  ImGui::SetNextWindowSize(ImVec2(arsenalW, arsenalH));
  ImGui::Begin("##arsenal", nullptr, kPanelFlags);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    const bool grimoireHere = !wide && s.grimoireMode;
    float y = PanelChrome(dl, wp, ws, grimoireHere ? "GRIMOIRE" : "ARSENAL",
                          wide ? "every word you know" : nullptr, stArsenal);
    if (!wide) {
      // The narrow fallback's toggle, on the header bar like the character
      // panel's gear/health.
      const char* label = grimoireHere ? "words" : "grimoire";
      const ImVec2 ts = ImGui::CalcTextSize(label);
      const float bw = std::max(96.0f, ts.x + 24);
      if (ui::Button("##arsmode",
                     ImVec2(wp.x + ws.x - kFrame - 10 - bw,
                            wp.y + kFrame + std::floor((ui::kHeaderH - ts.y - 8) * 0.5f)),
                     label, grimoireHere, bw))
        s.grimoireMode = !s.grimoireMode;
      if (ImGui::IsItemHovered())
        Tip(grimoireHere ? "Back to the words."
                         : "Your pages: saved word lists that speak as one key.");
    }
    // The bound rows FIRST: they are the thing that matters, and they are
    // literally the number row the game is listening to.
    y = ui::Subheading(dl, ImVec2(wp.x + kPad, y), ws.x - kPad * 2,
                       "BOUND   1-0 / Shift+1-0");
    y += 2;
    y += BoundKeys(s, dl, ImVec2(wp.x + kPad, y), ws.x - kPad * 2) + 14;
    if (grimoireHere) {
      GrimoireBody(s, ImVec2(wp.x + kPad, y),
                   ImVec2(ws.x - kPad * 2, wp.y + ws.y - kPad - y));
    } else {
      y = ui::Subheading(dl, ImVec2(wp.x + kPad, y), ws.x - kPad * 2, "EVERY WORD");
      y += 2;
      const float innerW = ws.x - kPad * 2;
      const int perRow = std::max(1, (int)((innerW - kGutter + (kCell - kSlot)) / kCell));
      const float need = GlyphTableHeight(s, perRow);
      const float room = wp.y + ws.y - kPad - y - (wide ? 2 * 13.0f + 12 : 0.0f);
      const float tableH = std::min(need, room);
      GlyphTable(s, ImVec2(wp.x + kPad, y), ImVec2(innerW, tableH), perRow);
      y += tableH + 6;
      if (wide) {
        // Two lines of small type under the table: the three gestures the
        // column is for, said once where the words are.
        ImGui::PushFont(ui::FontSmall());
        dl->AddText(ImVec2(wp.x + kPad, y), Fade(ui::ColParchDim(), 0.8f),
                    "drag a word onto a key to bind it  .  drag a key onto a key to swap them  .  "
                    "right-click a key to clear it");
        dl->AddText(ImVec2(wp.x + kPad, y + 14), Fade(ui::ColParchDim(), 0.8f),
                    "right-click a word to write it onto the open page  .  hover to read");
        ImGui::PopFont();
      }
    }
  }
  ImGui::End();
  }

  // ---- THE GRIMOIRE (its own panel, wide layout only) -----------------------
  if (wide && s.lootOpen) {
    LootPanel(s, ImVec2(grimX, top), ImVec2(grimW, grimH), stLoot, kPanelFlags);
  } else if (wide) {
    ImGui::SetNextWindowPos(ImVec2(grimX, top));
    ImGui::SetNextWindowSize(ImVec2(grimW, grimH));
    ImGui::Begin("##grimoire", nullptr, kPanelFlags);
    {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 wp = ImGui::GetWindowPos();
      const ImVec2 ws = ImGui::GetWindowSize();
      const float y = PanelChrome(dl, wp, ws, "GRIMOIRE", "saved sentences", stGrim);
      GrimoireBody(s, ImVec2(wp.x + kPad, y),
                   ImVec2(ws.x - kPad * 2, wp.y + ws.y - kPad - y));
    }
    ImGui::End();
  }

  // ==========================================================================
  // THE PACK: bag + hotbar, under the grimoire (wide) or the arsenal (narrow)
  // ==========================================================================
  const ImVec2 packPos = wide ? ImVec2(grimX, top + grimH + kColGap)
                              : ImVec2(midX, top + arsenalH + kColGap);
  const float packW = wide ? grimW : arsenalW;
  ImGui::SetNextWindowPos(packPos);
  ImGui::SetNextWindowSize(ImVec2(packW, std::max(200.0f, bottom - packPos.y)));
  ImGui::Begin("##bag", nullptr, kPanelFlags);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    float y = PanelChrome(dl, wp, ws, "PACK", "drag out to drop", stBag);

    // The grid is centred in the panel: the panel is as wide as ten cells
    // (the column above it needs that) and the bag is eight, and eight cells
    // hugging the left edge of a ten-cell panel read as a mistake.
    const float bagInner = s.bagCols * kSlot + (s.bagCols - 1) * kSlotGap;
    const float ox = std::max(0.0f, std::floor((ws.x - kPad * 2 - bagInner) * 0.25f) * 2.0f);
    for (int r = 0; r < s.bagRows; r++)
      for (int c = 0; c < s.bagCols; c++) {
        const int idx = r * s.bagCols + c;
        const float gx = wp.x + kPad + ox + c * (kSlot + kSlotGap);
        const float gy = y + r * (kSlot + kSlotGap);
        if (gx + kSlot > wp.x + ws.x - kPad) continue;
        char id[32];
        std::snprintf(id, sizeof id, "bag%d", idx);
        ItemSlot(s, id, ImVec2(gx, gy), SlotOr(s.bagSlots, idx),
                 KitRef{KitSpace::Bag, idx}, nullptr, true, nullptr, false);
      }
    y += s.bagRows * (kSlot + kSlotGap) + 10;

    y = ui::Subheading(dl, ImVec2(wp.x + kPad, y), ws.x - kPad * 2,
                       "IN HAND  1-0");
    y += 2;
    for (int i = 0; i < (int)s.hotbarSlots.size(); i++) {
      const float gx = wp.x + kPad + i * kCell;
      if (gx + kSlot > wp.x + ws.x - kPad) break;
      char id[32];
      std::snprintf(id, sizeof id, "hb%d", i);
      ItemSlot(s, id, ImVec2(gx, y), s.hotbarSlots[i],
               KitRef{KitSpace::Hotbar, i}, nullptr, true, nullptr,
               i == s.itemSelected);
      char k[4];
      std::snprintf(k, sizeof k, "%d", (i + 1) % 10);
      ui::KeyBadge(dl, ImVec2(gx + 2, y + 2), k);
    }
  }
  ImGui::End();

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
