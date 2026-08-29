#include "ui/inventory_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include <imgui.h>

#include "ui/theme.h"

namespace {

// ---- geometry ---------------------------------------------------------------
// Every number here is a multiple of the 2x chrome scale, so nothing lands on
// a half pixel. A slot is the 22 px slot sprite doubled.
constexpr float kSlot = 44.0f;
constexpr float kSlotGap = 8.0f;
constexpr float kPad = 16.0f;
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

ImU32 Fade(ImU32 c, float a) {
  const ImU32 keep = c & ~IM_COL32_A_MASK;
  const int alpha = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
  return keep | ((ImU32)std::clamp(alpha, 0, 255) << IM_COL32_A_SHIFT);
}

// A heading in gold small caps, with a rule under it. Used for every region so
// the three panels read as one document.
void Heading(ImDrawList* dl, ImVec2 at, float width, const char* text) {
  dl->AddText(ImVec2(at.x + 1, at.y + 1), ui::ColInk(), text);
  dl->AddText(at, ui::ColGoldHi(), text);
  const float y = at.y + ImGui::GetTextLineHeight() + 3.0f;
  dl->AddLine(ImVec2(at.x, y), ImVec2(at.x + width, y),
              Fade(ui::ColGoldDim(), 0.8f), 1.0f);
}

// Which chrome sprite carries an item of this kind.
const char* ItemIcon(const std::string& kind) {
  if (kind == "melee") return "item_melee";
  return "item_unknown";
}

const char* GlyphIcon(int type) {
  switch (type) {
    case 1: return "glyph_form";
    case 2: return "glyph_modifier";
    default: return "glyph_element";
  }
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
// function needs no change at all.
void ItemSlot(UIState& s, const char* id, ImVec2 at,
              const UIState::KitSlotUI& item, KitRef ref,
              const char* emptyIcon, bool acceptsAnything, const char* whyNot,
              bool selected) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(at);
  ImGui::PushID(id);
  ImGui::InvisibleButton("##slot", ImVec2(kSlot, kSlot));
  const bool hovered = ImGui::IsItemHovered();
  const bool filled = !item.name.empty();

  // Is a drag in flight, and would this slot take it? Asked here rather than
  // inside the drop target so the frame can turn red BEFORE the player lets
  // go — a refusal you find out about after committing is not a refusal, it
  // is a punishment.
  bool refusing = false;
  if (const ImGuiPayload* p = ImGui::GetDragDropPayload()) {
    if (p->IsDataType(kPayloadItem) && !acceptsAnything &&
        ref.space == KitSpace::Equip)
      refusing = true;
  }

  const char* frame = refusing      ? "slot_refuse"
                      : hovered     ? "slot_hover"
                      : filled      ? "slot_filled"
                                    : "slot";
  if (!ui::Chrome(frame)) {
    dl->AddRectFilled(at, ImVec2(at.x + kSlot, at.y + kSlot),
                      refusing ? IM_COL32(70, 26, 30, 255) : ui::ColInk());
    dl->AddRect(at, ImVec2(at.x + kSlot, at.y + kSlot),
                refusing  ? ui::ColBlood()
                : hovered ? ui::ColGold()
                          : ui::ColGoldDim());
  } else {
    ui::DrawSprite(dl, frame, at);
  }
  const ImVec2 mid(at.x + kSlot * 0.5f, at.y + kSlot * 0.5f);

  if (filled) {
    ui::DrawSpriteCentered(dl, ItemIcon(item.kind), mid);
    if (item.count > 1) {
      char buf[16];
      std::snprintf(buf, sizeof buf, "%d", item.count);
      const ImVec2 ts = ImGui::CalcTextSize(buf);
      const ImVec2 tp(at.x + kSlot - ts.x - 3, at.y + kSlot - ts.y - 2);
      dl->AddText(ImVec2(tp.x + 1, tp.y + 1), ui::ColInk(), buf);
      dl->AddText(tp, ui::ColParch(), buf);
    }
  } else if (emptyIcon && *emptyIcon) {
    // The engraving sits BEHIND whatever lands here, dim enough to read as a
    // label rather than as contents.
    ui::DrawSpriteCentered(dl, emptyIcon, mid, Fade(IM_COL32_WHITE, 0.34f));
  }
  // The selection ring is the hotbar's "this is in my hand" marker, and it is
  // the same ring the in-game strip shows.
  if (selected) {
    dl->AddRect(ImVec2(at.x - 2, at.y - 2), ImVec2(at.x + kSlot + 2,
                                                   at.y + kSlot + 2),
                ui::ColGoldHi(), 0.0f, 0, 2.0f);
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

  if (hovered) {
    ImGui::BeginTooltip();
    if (filled) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
                                               ui::ColGoldHi()));
      ImGui::TextUnformatted(item.name.c_str());
      ImGui::PopStyleColor();
      if (!item.kind.empty()) ImGui::TextDisabled("%s", item.kind.c_str());
      if (!item.tip.empty()) ImGui::TextUnformatted(item.tip.c_str());
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
  dl->AddRectFilled(at, br, IM_COL32(26, 20, 34, 255));

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

  ui::Draw9(dl, "panel_inner", at, br);
  if (hovered && !dragging) {
    const char* hint = "drag to turn";
    const ImVec2 ts = ImGui::CalcTextSize(hint);
    dl->AddText(ImVec2(br.x - ts.x - 10, br.y - ts.y - 8),
                Fade(ui::ColParchDim(), 0.55f), hint);
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
      col = Fade(ui::ColBlood(), 0.35f + 0.45f * (1.0f - worst));
    const ImVec2 p0(at.x + b.projMin[0] * size.x, at.y + b.projMin[1] * size.y);
    const ImVec2 p1(at.x + b.projMax[0] * size.x, at.y + b.projMax[1] * size.y);
    // Corner ticks rather than a full box: a closed rectangle over a character
    // reads as a selection widget, and four brackets read as a callout.
    const float t = std::min(10.0f, std::min(p1.x - p0.x, p1.y - p0.y) * 0.4f);
    if (t <= 1.0f) continue;
    const ImVec2 cs[4] = {p0, ImVec2(p1.x, p0.y), ImVec2(p0.x, p1.y), p1};
    const float sx[4] = {1, -1, 1, -1}, sy[4] = {1, 1, -1, -1};
    for (int k = 0; k < 4; k++) {
      dl->AddLine(cs[k], ImVec2(cs[k].x + t * sx[k], cs[k].y), col, 2.0f);
      dl->AddLine(cs[k], ImVec2(cs[k].x, cs[k].y + t * sy[k]), col, 2.0f);
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

  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::ColorConvertU32ToFloat4(
                            b.severed ? ui::ColBlood() : ui::ColParch()));
  ImGui::TextUnformatted(b.label);
  ImGui::PopStyleColor();

  if (b.severed) {
    ImGui::SameLine(kBarX);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(ui::ColBlood()));
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
    const ImVec2 p0(a.x, a.y + (h - 8.0f) * 0.5f);
    const ImVec2 p1(p0.x + kBarW, p0.y + 8.0f);
    dl->AddRectFilled(p0, p1, IM_COL32(20, 18, 32, 255));
    dl->AddRectFilled(p0,
                      ImVec2(p0.x + kBarW * std::clamp(frac, 0.0f, 1.0f), p1.y),
                      fill);
    dl->AddRect(p0, p1, Fade(ui::ColGoldDim(), 0.7f));
    ImGui::Dummy(ImVec2(kBarW, h));
  };
  char cap[48];
  std::snprintf(cap, sizeof cap, "%.0f / %.0f hp", b.hp, b.hpMax);
  bar(b.hpFrac, ui::ColBlood(), cap);
  std::snprintf(cap, sizeof cap, "%.0f%% intact", b.voxelFrac * 100.0f);
  bar(b.voxelFrac, IM_COL32(120, 132, 160, 255), cap);

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
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
  };
  if (b.bleeding) chip(ui::ColBloodHi(), "BLEEDING");
  if (b.burningVoxels > 0) chip(ui::ColEmber(), "BURNING %u", b.burningVoxels);
  if (b.charredFrac > 0.02f)
    chip(ui::ColEmber(), "CHARRED %.0f%%", b.charredFrac * 100.0f);
  if (any) ImGui::Unindent(12.0f);
  ImGui::Dummy(ImVec2(0, 4));
}

}  // namespace

void DrawInventoryScreen(UIState& s) {
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 disp = io.DisplaySize;
  ImDrawList* bg = ImGui::GetBackgroundDrawList();

  // The world stays visible and stays RUNNING behind this — you can watch the
  // fire you set spread while you rummage. Dimmed only enough that the panels
  // are the thing being read.
  bg->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(6, 5, 12, 140));

  // ---- layout ---------------------------------------------------------------
  const float kPortraitW =
      s.portraitW > 0 ? (float)s.portraitW : kPortraitWFallback;
  const float kPortraitH =
      s.portraitH > 0 ? (float)s.portraitH : kPortraitHFallback;
  const float leftW = kPad * 2 + kSlot * 2 + kSlotGap * 2 + kPortraitW + 8;
  const float top = 28.0f;
  const float bottom = std::max(top + 200.0f, disp.y - 28.0f);
  const float leftX = 28.0f;
  const float rightX = leftX + leftW + 20.0f;
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
    ui::Draw9(dl, "panel", wp, ImVec2(wp.x + ws.x, wp.y + ws.y));

    float y = wp.y + kPad + 6;
    Heading(dl, ImVec2(wp.x + kPad, y), ws.x - kPad * 2,
            s.inspectMode ? "CONDITION" : "CHARACTER");
    // The toggle sits on the heading's own line, right-aligned.
    {
      const char* label = s.inspectMode ? "gear" : "health";
      const ImVec2 ts = ImGui::CalcTextSize(label);
      ImGui::SetCursorScreenPos(
          ImVec2(wp.x + ws.x - kPad - ts.x - 16, y - 3));
      if (ImGui::Button(label)) s.inspectMode = !s.inspectMode;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            s.inspectMode
                ? "Back to equipment."
                : "What is actually wrong with this body: per-limb hp, how\n"
                  "much of each limb is still THERE, what is on fire, and\n"
                  "what came off. The two are different measurements - a\n"
                  "laser can bore a limb hollow without hurting it much.");
    }
    y += ImGui::GetTextLineHeight() + 16;

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
                   d.acceptsAnything, d.why.c_str(), false);
        }
      }
    }
    y = portY + kPortraitH + 14;

    if (s.inspectMode) {
      // The injury list, worst first. `order` is rebuilt every frame — it is
      // 15 entries and the sort key changes as the body takes damage, so
      // caching it would only buy a stale list.
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
      ImDrawList* d2 = ImGui::GetWindowDrawList();
      d2->AddText(ImVec2(wp.x + kPad, y), Fade(ui::ColParchDim(), 0.9f),
                  "ON YOUR PERSON");
      y += ImGui::GetTextLineHeight() + 6;
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
                 d.icon.c_str(), d.acceptsAnything, d.why.c_str(), false);
      }
      y += kSlot + 14;

      // Health + mana, the same two pools the HUD shows, so the screen and the
      // corner never disagree about how close you are to dead.
      const float barW = ws.x - kPad * 2;
      auto pool = [&](int32_t cur, int32_t max, ImU32 fill, const char* name) {
        const ImVec2 a(wp.x + kPad, y), b(a.x + barW, y + 16);
        d2->AddRectFilled(a, b, IM_COL32(20, 18, 32, 255));
        if (max > 0) {
          const float f = std::clamp((float)cur / (float)max, 0.0f, 1.0f);
          d2->AddRectFilled(a, ImVec2(a.x + barW * f, b.y), fill);
        }
        d2->AddRect(a, b, Fade(ui::ColGoldDim(), 0.85f));
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s  %d / %d", name, cur < 0 ? 0 : cur,
                      max);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const ImVec2 tp(a.x + 8, a.y + (16 - ts.y) * 0.5f);
        d2->AddText(ImVec2(tp.x + 1, tp.y + 1), ui::ColInk(), buf);
        d2->AddText(tp, ui::ColParch(), buf);
        y += 20;
      };
      pool(s.health, s.healthMax, ui::ColBlood(), "HEALTH");
      pool(s.mana, s.manaMax, IM_COL32(70, 120, 230, 255), "MANA");
      // The locomotion state is the one-line answer to "what is this damage
      // actually costing me", which no bar can give: "crawling" says more
      // about a pair of lost legs than two empty hp bars do.
      if (!s.playerAlive) {
        d2->AddText(ImVec2(wp.x + kPad, y + 2), ui::ColBloodHi(), "DEAD");
      } else if (!s.locoState.empty()) {
        d2->AddText(ImVec2(wp.x + kPad, y + 2), Fade(ui::ColParchDim(), 0.9f),
                    s.locoState.c_str());
      }
    }
  }
  ImGui::End();

  // ==========================================================================
  // TOP RIGHT: the arsenal
  // ==========================================================================
  const float arsenalH = std::min(360.0f, (bottom - top) * 0.46f);
  ImGui::SetNextWindowPos(ImVec2(rightX, top));
  ImGui::SetNextWindowSize(ImVec2(rightW, arsenalH));
  ImGui::Begin("##arsenal", nullptr, kPanelFlags);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    ui::Draw9(dl, "panel", wp, ImVec2(wp.x + ws.x, wp.y + ws.y));

    float y = wp.y + kPad + 6;
    Heading(dl, ImVec2(wp.x + kPad, y), ws.x - kPad * 2, "ARSENAL");
    {
      const char* hint = "drag a glyph onto a key";
      const ImVec2 ts = ImGui::CalcTextSize(hint);
      dl->AddText(ImVec2(wp.x + ws.x - kPad - ts.x, y + 2),
                  Fade(ui::ColParchDim(), 0.7f), hint);
    }
    y += ImGui::GetTextLineHeight() + 14;

    // The bound row FIRST: it is the thing that matters, and it is literally
    // the number row the game is listening to.
    const float glyphSlot = kSlot;
    for (int i = 0; i < (int)s.glyphSlots.size(); i++) {
      const float gx = wp.x + kPad + i * (glyphSlot + 6);
      if (gx + glyphSlot > wp.x + ws.x - kPad) break;
      const std::string& id = s.glyphSlots[i];
      ImGui::SetCursorScreenPos(ImVec2(gx, y));
      ImGui::PushID(1000 + i);
      ImGui::InvisibleButton("##gs", ImVec2(glyphSlot, glyphSlot));
      const bool hov = ImGui::IsItemHovered();
      ui::DrawSprite(dl, hov ? "slot_hover" : (id.empty() ? "slot"
                                                          : "slot_filled"),
                     ImVec2(gx, y));
      const ImVec2 mid(gx + glyphSlot * 0.5f, y + glyphSlot * 0.5f);
      // The glyph in this slot, looked up in the owned list so the icon and
      // swatch come from one place.
      const UIState::GlyphUI* g = nullptr;
      for (const UIState::GlyphUI& c : s.glyphsOwned)
        if (c.id == id) g = &c;
      if (g) {
        if (g->color) {
          const ImU32 sw = IM_COL32((g->color) & 0xFF, (g->color >> 8) & 0xFF,
                                    (g->color >> 16) & 0xFF, 255);
          dl->AddRectFilled(ImVec2(gx + 6, y + 6),
                            ImVec2(gx + glyphSlot - 6, y + glyphSlot - 6), sw);
        }
        ui::DrawSpriteCentered(dl, GlyphIcon(g->type), mid);
      }
      // The key that speaks it. 1..9 then 0, matching the HUD strip and the
      // GLFW binding in main.cpp.
      {
        char k[4];
        std::snprintf(k, sizeof k, "%d", (i + 1) % 10);
        dl->AddText(ImVec2(gx + 3, y + 1), ui::ColInk(), k);
        dl->AddText(ImVec2(gx + 2, y), ui::ColGoldHi(), k);
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
          ImGui::TextUnformatted(g->id.c_str());
          if (!g->desc.empty()) ImGui::TextDisabled("%s", g->desc.c_str());
          ImGui::TextDisabled("right-click to unbind");
        } else {
          ImGui::TextDisabled("unbound - drag a glyph here");
        }
        ImGui::EndTooltip();
      }
      ImGui::PopID();
    }
    y += glyphSlot + 14;

    dl->AddText(ImVec2(wp.x + kPad, y), Fade(ui::ColParchDim(), 0.9f), "KNOWN");
    y += ImGui::GetTextLineHeight() + 6;

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
        ui::DrawSprite(cd, hov ? "slot_hover" : "slot_filled", ImVec2(gx, gy));
        if (g.color) {
          const ImU32 sw = IM_COL32((g.color) & 0xFF, (g.color >> 8) & 0xFF,
                                    (g.color >> 16) & 0xFF, 255);
          cd->AddRectFilled(ImVec2(gx + 6, gy + 6),
                            ImVec2(gx + glyphSlot - 6, gy + glyphSlot - 6), sw);
        }
        ui::DrawSpriteCentered(cd, GlyphIcon(g.type),
                               ImVec2(gx + glyphSlot * 0.5f,
                                      gy + glyphSlot * 0.5f));
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
  const float bagY = top + arsenalH + 16;
  ImGui::SetNextWindowPos(ImVec2(rightX, bagY));
  ImGui::SetNextWindowSize(ImVec2(rightW, std::max(200.0f, bottom - bagY)));
  ImGui::Begin("##bag", nullptr, kPanelFlags);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    ui::Draw9(dl, "panel", wp, ImVec2(wp.x + ws.x, wp.y + ws.y));

    float y = wp.y + kPad + 6;
    Heading(dl, ImVec2(wp.x + kPad, y), ws.x - kPad * 2, "PACK");
    y += ImGui::GetTextLineHeight() + 14;

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
    y += s.bagRows * (kSlot + kSlotGap) + 8;

    dl->AddText(ImVec2(wp.x + kPad, y), Fade(ui::ColParchDim(), 0.9f),
                "IN HAND  (1-0)");
    y += ImGui::GetTextLineHeight() + 6;
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
      dl->AddText(ImVec2(gx + 3, y + 1), ui::ColInk(), k);
      dl->AddText(ImVec2(gx + 2, y), ui::ColGoldHi(), k);
    }
    y += kSlot + 10;

    // The refusal flash. Fades over ~2.5 s rather than sticking: it is an
    // answer to something you just did, and an answer still on screen a minute
    // later reads as a persistent error state.
    if (!s.kitMessage.empty() && s.kitMessageAge < 2.5f) {
      const float a = std::clamp(1.6f - s.kitMessageAge * 0.7f, 0.0f, 1.0f);
      dl->AddText(ImVec2(wp.x + kPad, y), Fade(ui::ColEmber(), a),
                  s.kitMessage.c_str());
    }
  }
  ImGui::End();

  // The one line of instruction, centred under everything.
  {
    const char* hint = "I or Esc to close";
    const ImVec2 ts = ImGui::CalcTextSize(hint);
    bg->AddText(ImVec2((disp.x - ts.x) * 0.5f, disp.y - ts.y - 8),
                Fade(ui::ColParchDim(), 0.75f), hint);
  }
}
