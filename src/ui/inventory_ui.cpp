#include "ui/inventory_ui.h"

#include <algorithm>
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
// being composed (reordering within the row).
constexpr const char* kPayloadPage = "SVPGE";
constexpr const char* kPayloadWord = "SVWRD";

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
    ui::DrawSpriteCentered(dl, ItemIcon(item.kind), mid);
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
    s.equipItem.pending = true;
    s.equipItem.from = ref;
  }

  if (hovered) {
    BeginTip();
    if (filled) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
                                               ui::ColGoldHi()));
      ImGui::TextUnformatted(item.name.c_str());
      ImGui::PopStyleColor();
      if (!item.kind.empty()) ImGui::TextDisabled("%s", item.kind.c_str());
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
        ImGui::TextDisabled(ref.space == KitSpace::Equip
                                ? "right-click to take off"
                                : "right-click to wear");
      }
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
  // The frame's own backdrop, drawn UNDER the image. Deliberately a different
  // colour from the portrait pass's clear (main.cpp kPortraitClear): when the
  // two matched, "the texture is not being sampled" and "the pass drew nothing
  // but its clear" produced pixel-identical results and cost a diagnosis.
  dl->AddRectFilled(at, br, IM_COL32(16, 12, 22, 255));
  ui::InnerShadow(dl, at, br, 10.0f, 0.5f);

  ImGui::SetCursorScreenPos(at);
  ImGui::InvisibleButton("##portrait", size);
  const bool hovered = ImGui::IsItemHovered();
  const bool dragging = ImGui::IsItemActive() &&
                        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f);

  if (s.portraitTex) {
    dl->AddImage((ImTextureID)s.portraitTex, at, br);
  } else {
    const char* msg = "no body";
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(at.x + (size.x - ts.x) * 0.5f,
                       at.y + (size.y - ts.y) * 0.5f),
                Fade(ui::ColParchDim(), 0.6f), msg);
  }
  // A recess vignette OVER the picture: the portrait is a window into a
  // lit box, and the edges of a lit box are darker than its middle.
  ui::InnerShadow(dl, at, br, 16.0f, 0.55f);
  ui::Grain(dl, at, br, 0.035f);

  // ORBIT. The drag turns the portrait camera; pitch is clamped well short of
  // the poles because a camera that can pass over the head gimbals and the
  // avatar flips upside down mid-drag.
  if (dragging) {
    const ImVec2 d = ImGui::GetIO().MouseDelta;
    // Drag left turns the body to the left, and drag DOWN tilts the camera to
    // look down at it — the sign every model viewer uses, and the one that
    // matches Camera::pitch (positive = looking up).
    s.portraitYaw -= d.x * 0.012f;
    s.portraitPitch = std::clamp(s.portraitPitch - d.y * 0.008f, -0.9f, 0.9f);
  }
  // HEAD LOOK. While NOT dragging, the cursor over the frame is reported to
  // main.cpp, which turns it into a real SetLook — the character glances at
  // the mouse. Reported rather than applied because posing a rig is game
  // state; this function only knows where the pointer is.
  s.portraitLookValid = hovered && !dragging;
  if (s.portraitLookValid) {
    const ImVec2 m = ImGui::GetMousePos();
    s.portraitLook[0] = std::clamp((m.x - at.x) / size.x * 2.0f - 1.0f, -1.0f,
                                   1.0f);
    s.portraitLook[1] = std::clamp(1.0f - (m.y - at.y) / size.y * 2.0f, -1.0f,
                                   1.0f);
  }

  if (hovered) ui::Glow(dl, at, br, ui::ColGold(), 6.0f, 0.22f);
  ui::Draw9(dl, "panel_inner", at, br);
  if (hovered && !dragging) {
    const char* hint = "drag to turn";
    const ImVec2 ts = ImGui::CalcTextSize(hint);
    ui::ShadowText(dl, ImVec2(br.x - ts.x - 12, br.y - ts.y - 10),
                   Fade(ui::ColParchDim(), 0.7f), hint);
  }
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
  // One wall-clock phase for the whole body, so wounds pulse together and read
  // as one alarm — the same choice DrawBodyFigure makes, for the same reason.
  const float flash = 0.5f + 0.5f * (float)std::sin(ImGui::GetTime() * 4.5);

  for (int i = 0; i < UIState::kSlotCount; i++) {
    const UIState::BodyPartUI& b = s.body[i];
    // A severed limb is not drawn over: it is simply NOT IN THE PICTURE, which
    // says it better than any marker could (see BodyPartUI's note).
    if (b.severed || !b.projValid) continue;
    // Outline only what is actually wrong. Ringing every limb would make the
    // portrait unreadable and would say nothing.
    const float worst = std::min(b.hpFrac, b.voxelFrac);
    if (worst > 0.98f && b.burningVoxels == 0 && !b.bleeding) continue;
    ImU32 col = ui::ColEmber();
    if (b.burningVoxels > 0)
      col = Fade(ui::ColEmber(), 0.5f + 0.5f * flash);
    else if (b.bleeding)
      col = Fade(ui::ColBloodHi(), 0.45f + 0.55f * flash);
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
    const ImVec2 p0(at.x + b.projMin[0] * size.x, at.y + b.projMin[1] * size.y);
    const ImVec2 p1(at.x + b.projMax[0] * size.x, at.y + b.projMax[1] * size.y);
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

// One row of the injury list. Ordered worst-first by the caller.
//
// LAID OUT WITH THE CURSOR, not with hand-computed y offsets. The first
// version drew both bars and their labels at p.y + a constant, which put two
// captions and the limb's own name on top of each other the moment any of them
// was wider than guessed — a whole column of "9% i34/60ct". ImGui already
// knows how tall a line is; asking it is both shorter and correct at any font.
void InjuryRow(const UIState::BodyPartUI& b) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Where the bars start. Fixed, so every row's bars line up into a column
  // that can be read down rather than per-row.
  const float kBarX = 190.0f;
  const float kBarW = 130.0f;

  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ui::ShadowText(dl, p, b.severed ? ui::ColBloodHi() : ui::ColParch(),
                   b.label);
    ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
  }

  if (b.severed) {
    ImGui::SameLine(kBarX);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(ui::ColBloodHi()));
    ImGui::TextUnformatted("SEVERED");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
    return;
  }

  // TWO BARS, because they are two different measurements and reporting one
  // would be a lie: hp says how HURT the limb is, intactness says how much of
  // it is still THERE. A laser can bore a limb hollow at almost full hp, and a
  // blast can take hp off a limb that has lost no geometry at all.
  auto bar = [&](float frac, ImU32 fill, const char* caption) {
    ImGui::Indent(12.0f);
    ImGui::TextDisabled("%s", caption);
    ImGui::Unindent(12.0f);
    ImGui::SameLine(kBarX);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p0(a.x, std::floor(a.y + (h - 10.0f) * 0.5f));
    const ImVec2 p1(p0.x + kBarW, p0.y + 10.0f);
    ui::ValueBar(dl, p0, p1, frac, fill, false);
    ImGui::Dummy(ImVec2(kBarW, h));
  };
  char cap[48];
  std::snprintf(cap, sizeof cap, "%.0f / %.0f hp", b.hp, b.hpMax);
  bar(b.hpFrac, ui::ColBloodHi(), cap);
  std::snprintf(cap, sizeof cap, "%.0f%% intact", b.voxelFrac * 100.0f);
  bar(b.voxelFrac, ui::ColSteel(), cap);

  // State chips, in the order they matter to somebody deciding what to do next.
  bool any = false;
  auto chip = [&](ImU32 col, const char* fmt, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (any) ImGui::SameLine();
    else ImGui::Indent(12.0f);
    any = true;
    // A chip: the word in its colour on a dark tab with a coloured underline.
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddRectFilled(ImVec2(p.x - 4, p.y), ImVec2(p.x + ts.x + 4, p.y + ts.y),
                      Fade(ui::ColInk(), 0.7f));
    dl->AddRectFilled(ImVec2(p.x - 4, p.y + ts.y - 2),
                      ImVec2(p.x + ts.x + 4, p.y + ts.y), Fade(col, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(4, 0));
  };
  if (b.bleeding) chip(ui::ColBloodHi(), "BLEEDING");
  if (b.burningVoxels > 0) chip(ui::ColEmber(), "BURNING %u", b.burningVoxels);
  if (b.charredFrac > 0.02f)
    chip(ui::ColEmber(), "CHARRED %.0f%%", b.charredFrac * 100.0f);
  if (any) {
    ImGui::NewLine();
    ImGui::Unindent(12.0f);
  }
  ImGui::Dummy(ImVec2(0, 6));
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

// One word of the table: the cell, its sort tag, its valence mark, the drag
// out of it, and the §9 info box on hover.
// A word straight into the page being composed, at the end (the grimoire's
// right-click). Refused silently when the open page is authored or full; the
// composer's own text says why in both cases.
bool WriteWordIntoPage(UIState& s, const char* name) {
  const UIState::GrimoirePageUI* sel = FindPageUI(s, s.grimoireSelected);
  if (sel && sel->readOnly) return false;
  if ((int)s.grimoireEditWords.size() >= s.grimoireMaxWords) return false;
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
      } else if (isPage) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::ColGoldHi()));
        ImGui::Text("[%s]  a page", id.c_str());
        ImGui::PopStyleColor();
        const std::string& ro = i < (int)s.glyphSlotReadouts.size() ? s.glyphSlotReadouts[i] : "";
        ImGui::TextDisabled("%s", ro.empty() ? "(a page that names nothing)" : ro.c_str());
        ImGui::TextDisabled("%s speaks it  .  right-click to unbind", key);
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
    const int nWords = (int)s.grimoireEditWords.size();
    const int cells = std::max(1, s.grimoireMaxWords);
    const int perRow = std::max(1, (int)((innerW + (kCell - kSlot)) / kCell));
    auto insertWord = [&](int at, const char* name) {
      if (readOnly) return;
      if ((int)s.grimoireEditWords.size() >= s.grimoireMaxWords) return;
      at = std::max(0, std::min(at, (int)s.grimoireEditWords.size()));
      s.grimoireEditWords.insert(s.grimoireEditWords.begin() + at, name);
      s.grimoireEditDirty = true;
    };
    for (int i = 0; i < cells; i++) {
      const ImVec2 p(base.x + (i % perRow) * kCell, cy + (i / perRow) * kCell);
      ImGui::SetCursorScreenPos(p);
      ImGui::PushID(4000 + i);
      ImGui::InvisibleButton("##wd", ImVec2(kSlot, kSlot));
      const bool hov = ImGui::IsItemHovered();
      const bool has = i < nWords;
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
        if (hov) {
          BeginTip();
          ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::ColGoldHi()));
          if (g) ImGui::TextUnformatted(w.c_str());
          else if (pg) ImGui::Text("[%s]  a page", w.c_str());
          else ImGui::Text("%s  (a word that no longer exists)", w.c_str());
          ImGui::PopStyleColor();
          if (g) ImGui::TextDisabled("%s  .  %s", kSortLabel[g->type], g->desc.c_str());
          else if (pg) ImGui::TextDisabled("%s", pg->readout.c_str());
          if (!readOnly) ImGui::TextDisabled("drag to reorder  .  right-click or drag out to remove");
          EndTip();
        }
        if (!readOnly && hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          s.grimoireEditWords.erase(s.grimoireEditWords.begin() + i);
          s.grimoireEditDirty = true;
        }
      } else if (hov) {
        Tip(readOnly ? "An authored page: copy it to edit."
                     : "Drop a word from the arsenal, or a page from the list, here.");
      }
      if (!readOnly && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadGlyph))
          insertWord(i, (const char*)p->Data);
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadPage))
          insertWord(i, (const char*)p->Data);
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadWord)) {
          const int from = *(const int*)p->Data;
          if (from >= 0 && from < (int)s.grimoireEditWords.size() && from != i) {
            const std::string w = s.grimoireEditWords[from];
            s.grimoireEditWords.erase(s.grimoireEditWords.begin() + from);
            const int to = std::min(i > from ? i - 1 : i, (int)s.grimoireEditWords.size());
            s.grimoireEditWords.insert(s.grimoireEditWords.begin() + to, w);
            s.grimoireEditDirty = true;
          }
        }
        ImGui::EndDragDropTarget();
      }
      ImGui::PopID();
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
      ImGui::PopFont();
      cy += 20;
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
      if (!s.kitMessage.empty() && s.kitMessageAge < 4.0f) {
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

  // ---- layout ---------------------------------------------------------------
  const float kPortraitW =
      s.portraitW > 0 ? (float)s.portraitW : kPortraitWFallback;
  const float kPortraitH =
      s.portraitH > 0 ? (float)s.portraitH : kPortraitHFallback;
  const float leftW = kPad * 2 + kSlot * 2 + kSlotGap * 2 + kPortraitW + 8;
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
  // The third column holds the grimoire over the pack, ten cells wide (the
  // eight-column bag fits inside), and exists only when the window has room
  // for it beside the other two and height for a composer above the pack.
  const float grimX = midX + arsenalW + kColGap;
  const float grimWant = kPad * 2 + (kKeyCols - 1) * kCell + kSlot;
  const float grimRoom = disp.x - 28.0f - grimX;
  const bool wide = grimRoom >= grimWant && (bottom - top) >= packH + kColGap + 300.0f;
  const float grimW = std::min(grimRoom, grimWant + 40.0f);
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
    float y = PanelChrome(dl, wp, ws, s.inspectMode ? "CONDITION" : "CHARACTER",
                          nullptr, stChar);
    // The toggle sits on the header bar, right-aligned.
    {
      const char* label = s.inspectMode ? "gear" : "health";
      const ImVec2 ts = ImGui::CalcTextSize(label);
      const float bw = std::max(96.0f, ts.x + 24);
      if (ui::Button("##mode",
                     ImVec2(wp.x + ws.x - kFrame - 10 - bw,
                            wp.y + kFrame + std::floor((ui::kHeaderH - ts.y - 8) * 0.5f)),
                     label, s.inspectMode, bw))
        s.inspectMode = !s.inspectMode;
      if (ImGui::IsItemHovered())
        Tip(s.inspectMode
                ? "Back to equipment."
                : "What is actually wrong with this body: per-limb hp, how much of "
                  "each limb is still THERE, what is on fire, and what came off. The "
                  "two are different measurements - a laser can bore a limb hollow "
                  "without hurting it much.");
    }

    // The portrait, with a column of slots on each side.
    const float colL = wp.x + kPad;
    const float portX = colL + kSlot + kSlotGap;
    const float colR = portX + kPortraitW + kSlotGap;
    const float portY = y;

    ImGui::SetCursorScreenPos(ImVec2(portX, portY));
    Portrait(s, ImVec2(portX, portY), ImVec2(kPortraitW, kPortraitH));
    if (s.inspectMode) {
      InspectOverlay(s, ImVec2(portX, portY), ImVec2(kPortraitW, kPortraitH));
      InspectCastPicks(s, ImVec2(portX, portY), ImVec2(kPortraitW, kPortraitH));
    }

    if (!s.inspectMode) {
      // Armour columns. Indices are the EquipSlotId order from
      // game/equipment.h; the labels and refusal reasons come from that same
      // table through equipDefs, never restated here.
      const int leftCol[4] = {0, 1, 2, 3};   // head, chest, legs, boots
      const int rightCol[4] = {4, 5, 6, 7};  // shoulders, hands, belt, trinket
      for (int r = 0; r < 4; r++) {
        const float sy = portY + r * (kSlot + kSlotGap);
        for (int side = 0; side < 2; side++) {
          const int idx = side ? rightCol[r] : leftCol[r];
          const UIState::EquipSlotUI& d =
              idx < (int)s.equipDefs.size() ? s.equipDefs[idx]
                                            : UIState::EquipSlotUI{};
          char id[32];
          std::snprintf(id, sizeof id, "eq%d", idx);
          ItemSlot(s, id, ImVec2(side ? colR : colL, sy),
                   SlotOr(s.equipSlots, idx),
                   KitRef{KitSpace::Equip, idx}, d.icon.c_str(),
                   d.acceptsAnything, d.why.c_str(), false, &d.accepts);
        }
      }
    }
    y = portY + kPortraitH + 16;

    if (s.inspectMode) {
      // The injury list, worst first. `order` is rebuilt every frame — it is
      // 15 entries and the sort key changes as the body takes damage, so
      // caching it would only buy a stale list.
      y = ui::Subheading(dl, ImVec2(wp.x + kPad, y), ws.x - kPad * 2, "INJURIES");
      y += 4;
      ImGui::SetCursorScreenPos(ImVec2(wp.x + kPad, y));
      ImGui::PushClipRect(ImVec2(wp.x + kPad, y),
                          ImVec2(wp.x + ws.x - kPad, wp.y + ws.y - kPad), true);
      ImGui::BeginChild("##injuries",
                        ImVec2(ws.x - kPad * 2, wp.y + ws.y - kPad - y - 6),
                        ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
      int order[UIState::kSlotCount];
      int n = 0;
      for (int i = 0; i < UIState::kSlotCount; i++)
        if (s.body[i].present) order[n++] = i;
      std::stable_sort(order, order + n, [&](int a, int b) {
        auto score = [&](int i) {
          const UIState::BodyPartUI& p = s.body[i];
          if (p.severed) return -1.0f;             // gone: always first
          return std::min(p.hpFrac, p.voxelFrac);  // then worst-off
        };
        return score(a) < score(b);
      });
      bool anyHurt = false;
      for (int i = 0; i < n; i++) {
        const UIState::BodyPartUI& b = s.body[order[i]];
        const bool hurt = b.severed || b.bleeding || b.burningVoxels > 0 ||
                          b.hpFrac < 0.999f || b.voxelFrac < 0.999f;
        if (!hurt) continue;
        anyHurt = true;
        InjuryRow(b);
      }
      if (!anyHurt) {
        ImGui::TextDisabled(s.bodyValid ? "Not a scratch."
                                        : "No body to inspect.");
      }
      ImGui::EndChild();
      ImGui::PopClipRect();
    } else {
      // Sheath + quick slots: what is on your person but not in your hand.
      y = ui::Subheading(dl, ImVec2(wp.x + kPad, y), ws.x - kPad * 2,
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

      // Health + mana, the same two pools the HUD shows, so the screen and the
      // corner never disagree about how close you are to dead.
      const float barW = ws.x - kPad * 2;
      // `cap` < max is the burn cap (UIState::healthCap): the span past it is
      // drawn charred off. -1 = no cap for this pool.
      auto pool = [&](int32_t cur, int32_t max, ImU32 fill, const char* name,
                      int32_t cap) {
        const ImVec2 a(wp.x + kPad, y), b(a.x + barW, y + 22);
        const float f =
            max > 0 ? std::clamp((float)cur / (float)max, 0.0f, 1.0f) : 0.0f;
        ui::ValueBar(dl, a, b, f, fill, true);
        if (max > 0 && cap >= 0 && cap < max) {
          const float cf = std::clamp((float)cap / (float)max, 0.0f, 1.0f);
          const float cx = std::floor(a.x + (b.x - a.x) * cf);
          dl->AddRectFilled(ImVec2(cx, a.y), b, IM_COL32(40, 30, 26, 235));
          dl->AddLine(ImVec2(cx, a.y), ImVec2(cx, b.y),
                      IM_COL32(120, 60, 40, 255));
        }
        char buf[64];
        std::snprintf(buf, sizeof buf, "%d / %d", cur < 0 ? 0 : cur, max);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const float ty = std::floor(a.y + (22 - ts.y) * 0.5f);
        ui::TrackedText(dl, ImVec2(a.x + 9, ty + 1), Fade(ui::ColInk(), 0.9f),
                        name, 2.0f);
        ui::TrackedText(dl, ImVec2(a.x + 8, ty), ui::ColGoldPale(), name, 2.0f);
        ui::ShadowText(dl, ImVec2(b.x - ts.x - 8, ty), ui::ColParch(), buf);
        y += 30;
      };
      pool(s.health, s.healthMax, ui::ColBloodHi(), "HEALTH", s.healthCap);
      pool(s.mana, s.manaMax, ui::ColMana(), "MANA", -1);
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
  // THE RIGHT TWO COLUMNS: arsenal | grimoire over pack (plan §12a, §12b)
  // ==========================================================================
  // The ARSENAL is a full column of its own: the twenty bound keys, then every
  // word in the library in a table with one row-band per sort. That table is
  // the drag SOURCE for both of the other panels, and a source you cannot see
  // while you compose is no source — the first version put the grimoire
  // behind a mode toggle on the arsenal, and the grid it needed vanished the
  // moment the toggle was pressed. The GRIMOIRE sits above the PACK in a third
  // column, so a page drags onto a key and a glyph drags into a page along one
  // short horizontal line each. When the window has no room for a third
  // column the old arrangement returns: the arsenal toggles between its table
  // and the grimoire, over the pack.
  const float arsenalH = wide ? (bottom - top) : (bottom - top - packH - kColGap);
  ImGui::SetNextWindowPos(ImVec2(midX, top));
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
                    "drag a word onto a key to bind it  .  right-click a key to clear it");
        dl->AddText(ImVec2(wp.x + kPad, y + 14), Fade(ui::ColParchDim(), 0.8f),
                    "right-click a word to write it onto the open page  .  hover to read");
        ImGui::PopFont();
      }
    }
  }
  ImGui::End();

  // ---- THE GRIMOIRE (its own panel, wide layout only) -----------------------
  if (wide) {
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
          s.grimoireEditWords.erase(s.grimoireEditWords.begin() + idx);
          s.grimoireEditDirty = true;
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
    const char* text = fresh ? s.kitMessage.c_str() : "I or Esc to close   |   drag out to drop";
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
