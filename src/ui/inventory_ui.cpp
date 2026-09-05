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
    ImGui::BeginTooltip();
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
    ImGui::EndTooltip();
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
  const float rightX = leftX + leftW + 24.0f;
  // The right column is CAPPED, not stretched to the window. A 4x8 grid in a
  // 1000-pixel panel is a grid floating in a sea of frame; the panel should be
  // the size of what is in it. The cap is the widest of the two things that
  // live there: eight bag columns, or ten bound-glyph keys.
  const float cols = (float)std::max(1, s.bagCols);
  const float bagW = kPad * 2 + cols * kSlot + (cols - 1) * kSlotGap;
  const float keyW = kPad * 2 + 10 * kSlot + 9 * 6.0f;
  const float rightWant = std::max(bagW, keyW);
  const float rightW =
      std::max(320.0f, std::min(rightWant, disp.x - rightX - 28.0f));

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
        ImGui::SetTooltip(
            s.inspectMode
                ? "Back to equipment."
                : "What is actually wrong with this body: per-limb hp, how\n"
                  "much of each limb is still THERE, what is on fire, and\n"
                  "what came off. The two are different measurements - a\n"
                  "laser can bore a limb hollow without hurting it much.");
    }

    // The portrait, with a column of slots on each side.
    const float colL = wp.x + kPad;
    const float portX = colL + kSlot + kSlotGap;
    const float colR = portX + kPortraitW + kSlotGap;
    const float portY = y;

    ImGui::SetCursorScreenPos(ImVec2(portX, portY));
    Portrait(s, ImVec2(portX, portY), ImVec2(kPortraitW, kPortraitH));
    if (s.inspectMode)
      InspectOverlay(s, ImVec2(portX, portY), ImVec2(kPortraitW, kPortraitH));

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
  // TOP RIGHT: the arsenal
  // ==========================================================================
  const float arsenalH = std::min(380.0f, (bottom - top) * 0.46f);
  ImGui::SetNextWindowPos(ImVec2(rightX, top));
  ImGui::SetNextWindowSize(ImVec2(rightW, arsenalH));
  ImGui::Begin("##arsenal", nullptr, kPanelFlags);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    float y = PanelChrome(dl, wp, ws, "ARSENAL", "drag a glyph onto a key",
                          stArsenal);

    // The bound row FIRST: it is the thing that matters, and it is literally
    // the number row the game is listening to.
    y = ui::Subheading(dl, ImVec2(wp.x + kPad, y), ws.x - kPad * 2, "BOUND");
    y += 2;
    const float glyphSlot = kSlot;
    for (int i = 0; i < (int)s.glyphSlots.size(); i++) {
      const float gx = wp.x + kPad + i * (glyphSlot + 6);
      if (gx + glyphSlot > wp.x + ws.x - kPad) break;
      const std::string& id = s.glyphSlots[i];
      ImGui::SetCursorScreenPos(ImVec2(gx, y));
      ImGui::PushID(1000 + i);
      ImGui::InvisibleButton("##gs", ImVec2(glyphSlot, glyphSlot));
      const bool hov = ImGui::IsItemHovered();
      // The glyph in this slot, looked up in the owned list so the icon and
      // swatch come from one place.
      const UIState::GlyphUI* g = nullptr;
      for (const UIState::GlyphUI& c : s.glyphsOwned)
        if (c.id == id) g = &c;
      const ui::SlotLook look = hov ? ui::SlotLook::Hover
                                : g ? ui::SlotLook::Filled
                                    : ui::SlotLook::Empty;
      ui::SlotSurface(dl, ImVec2(gx, y), glyphSlot, look, false);
      SlotRim(dl, ImVec2(gx, y), look);
      if (g) GlyphContents(dl, ImVec2(gx, y), *g);
      // The key that speaks it. 1..9 then 0, matching the HUD strip and the
      // GLFW binding in main.cpp.
      {
        char k[4];
        std::snprintf(k, sizeof k, "%d", (i + 1) % 10);
        ui::KeyBadge(dl, ImVec2(gx + 2, y + 2), k);
      }
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p =
                ImGui::AcceptDragDropPayload(kPayloadGlyph)) {
          s.bindGlyph.pending = true;
          s.bindGlyph.slot = i;
          s.bindGlyph.glyphId = (const char*)p->Data;
        }
        ImGui::EndDragDropTarget();
      }
      // Right-click unbinds. A bound slot has to be clearable without needing
      // somewhere to drag it TO.
      if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !id.empty()) {
        s.bindGlyph.pending = true;
        s.bindGlyph.slot = i;
        s.bindGlyph.glyphId.clear();
      }
      if (hov) {
        ImGui::BeginTooltip();
        if (g) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
                                                   ui::ColGoldHi()));
          ImGui::TextUnformatted(g->id.c_str());
          ImGui::PopStyleColor();
          if (!g->desc.empty()) ImGui::TextDisabled("%s", g->desc.c_str());
          ImGui::TextDisabled("right-click to unbind");
        } else {
          ImGui::TextDisabled("unbound - drag a glyph here");
        }
        ImGui::EndTooltip();
      }
      ImGui::PopID();
    }
    y += glyphSlot + 18;

    y = ui::Subheading(dl, ImVec2(wp.x + kPad, y), ws.x - kPad * 2, "KNOWN");
    y += 2;

    ImGui::SetCursorScreenPos(ImVec2(wp.x + kPad, y));
    ImGui::BeginChild("##known",
                      ImVec2(ws.x - kPad * 2, wp.y + ws.y - kPad - y - 4),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    {
      ImDrawList* cd = ImGui::GetWindowDrawList();
      const float availW = ImGui::GetContentRegionAvail().x;
      const int perRow = std::max(1, (int)((availW + 6) / (glyphSlot + 6)));
      // The grid's ORIGIN, taken ONCE. Reading the cursor inside the loop
      // reads where the PREVIOUS slot left it, which lays the grid out
      // diagonally — every slot offset from the last one instead of from the
      // top-left.
      const ImVec2 base = ImGui::GetCursorScreenPos();
      for (int i = 0; i < (int)s.glyphsOwned.size(); i++) {
        const UIState::GlyphUI& g = s.glyphsOwned[i];
        const float gx = base.x + (i % perRow) * (glyphSlot + 6);
        const float gy = base.y + (i / perRow) * (glyphSlot + 6);
        ImGui::SetCursorScreenPos(ImVec2(gx, gy));
        ImGui::PushID(2000 + i);
        ImGui::InvisibleButton("##kg", ImVec2(glyphSlot, glyphSlot));
        const bool hov = ImGui::IsItemHovered();
        const ui::SlotLook look = hov ? ui::SlotLook::Hover : ui::SlotLook::Filled;
        ui::SlotSurface(cd, ImVec2(gx, gy), glyphSlot, look, false);
        SlotRim(cd, ImVec2(gx, gy), look);
        GlyphContents(cd, ImVec2(gx, gy), g);
        if (ImGui::BeginDragDropSource()) {
          char buf[64] = {};
          std::snprintf(buf, sizeof buf, "%s", g.id.c_str());
          ImGui::SetDragDropPayload(kPayloadGlyph, buf, sizeof(buf));
          ImGui::TextUnformatted(g.id.c_str());
          ImGui::EndDragDropSource();
        }
        if (hov) {
          ImGui::BeginTooltip();
          ImGui::PushStyleColor(
              ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::ColGoldHi()));
          ImGui::TextUnformatted(g.id.c_str());
          ImGui::PopStyleColor();
          ImGui::TextDisabled("%s  -  %d mana",
                              g.type == 1   ? "form"
                              : g.type == 2 ? "modifier"
                                            : "element",
                              g.mana);
          if (!g.desc.empty()) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(360.0f);
            ImGui::TextUnformatted(g.desc.c_str());
            ImGui::PopTextWrapPos();
          }
          ImGui::EndTooltip();
        }
        ImGui::PopID();
      }
      if (s.glyphsOwned.empty()) {
        ImGui::SetCursorScreenPos(base);
        ImGui::TextDisabled("You know no glyphs.");
      }
      // Reserve the rows the manual placement above drew into, so the child
      // scrolls when the library outgrows the panel.
      const int rows =
          ((int)s.glyphsOwned.size() + perRow - 1) / std::max(1, perRow);
      ImGui::SetCursorScreenPos(base);
      ImGui::Dummy(ImVec2(availW, rows * (glyphSlot + 6)));
    }
    ImGui::EndChild();
  }
  ImGui::End();

  // ==========================================================================
  // BOTTOM RIGHT: bag + hotbar
  // ==========================================================================
  const float bagY = top + arsenalH + 20;
  ImGui::SetNextWindowPos(ImVec2(rightX, bagY));
  ImGui::SetNextWindowSize(ImVec2(rightW, std::max(200.0f, bottom - bagY)));
  ImGui::Begin("##bag", nullptr, kPanelFlags);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    float y = PanelChrome(dl, wp, ws, "PACK", "drag out to drop", stBag);

    for (int r = 0; r < s.bagRows; r++)
      for (int c = 0; c < s.bagCols; c++) {
        const int idx = r * s.bagCols + c;
        const float gx = wp.x + kPad + c * (kSlot + kSlotGap);
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
      const float gx = wp.x + kPad + i * (kSlot + 6);
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
    y += kSlot + 12;

    // The refusal flash. Fades over ~2.5 s rather than sticking: it is an
    // answer to something you just did, and an answer still on screen a minute
    // later reads as a persistent error state.
    if (!s.kitMessage.empty() && s.kitMessageAge < 2.5f) {
      const float a = std::clamp(1.6f - s.kitMessageAge * 0.7f, 0.0f, 1.0f);
      ui::ShadowText(dl, ImVec2(wp.x + kPad, y), Fade(ui::ColEmber(), a),
                     s.kitMessage.c_str());
    }
  }
  ImGui::End();

  // ---- DRAGGED OUT OF EVERY PANEL: PUT IT ON THE FLOOR --------------------
  //
  // Every slot is a drop TARGET, so a drag that ends anywhere else has been
  // refused by all of them and imgui simply forgets it. That silence is the
  // problem: "drag it out of the window" is the gesture every inventory in the
  // genre uses for discard, and having it do nothing reads as broken.
  //
  // Detected the only way imgui allows — the payload was still live last frame
  // and the mouse has now been released with nobody accepting it. Deliberately
  // AFTER every panel has had its chance, so a legal move is never mistaken
  // for a drop.
  if (const ImGuiPayload* p = ImGui::GetDragDropPayload()) {
    if (p->IsDataType(kPayloadItem) &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        !ImGui::IsAnyItemHovered()) {
      KitRef from{};
      std::memcpy(&from, p->Data, sizeof(from));
      s.dropItem.pending = true;
      s.dropItem.from = from;
    }
  }

  // The one line of instruction, centred under everything, on a dark tab so
  // it reads over whatever the world is doing down there.
  {
    const char* hint = "I or Esc to close   |   drag out to drop";
    const ImVec2 ts = ImGui::CalcTextSize(hint);
    const ImVec2 tp(std::floor((disp.x - ts.x) * 0.5f), disp.y - ts.y - 8);
    bg->AddRectFilled(ImVec2(tp.x - 12, tp.y - 2), ImVec2(tp.x + ts.x + 12, disp.y),
                      Fade(ui::ColInk(), 0.6f));
    bg->AddRectFilled(ImVec2(tp.x - 12, tp.y - 2), ImVec2(tp.x + ts.x + 12, tp.y),
                      Fade(ui::ColGoldDim(), 0.5f));
    ui::ShadowText(bg, tp, Fade(ui::ColParchDim(), 0.85f), hint);
  }
}
