// dialogue_ui.cpp — the conversation panel. See dialogue_ui.h.
//
// THE LOOK is the character screen's (ui/theme.h), on purpose: the same lit
// panel body, the same 9-slice frame, the same header bar and the same 26 px
// pixel face, so a conversation reads as part of the game's one UI rather
// than as a subtitle track. Everything is laid on the 2 px lattice (Snap)
// and every gradient is banded; there is no rounding anywhere.

#include "ui/dialogue_ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

#include "ui/theme.h"

namespace {

constexpr float kFrame = 16.0f;   // the 9-slice frame's thickness (inventory_ui)
constexpr float kPad = 18.0f;     // inside the frame
constexpr float kPortrait = 116.0f;
constexpr float kMaxW = 1180.0f;
constexpr float kBottomGap = 40.0f;
constexpr float kRowGap = 4.0f;

float Snap(float v) { return std::floor(v * 0.5f) * 2.0f; }

// Height of `text` wrapped at `wrap` in the current font.
float WrappedH(const char* text, float wrap) {
  ImFont* f = ImGui::GetFont();
  const float sz = ImGui::GetFontSize();
  return f->CalcTextSizeA(sz, 1e9f, wrap, text).y;
}

// Wrapped text with the one-pixel ink shadow ShadowText gives a single line.
void WrappedText(ImDrawList* dl, ImVec2 at, ImU32 col, const char* text, float wrap) {
  ImFont* f = ImGui::GetFont();
  const float sz = ImGui::GetFontSize();
  dl->AddText(f, sz, ImVec2(at.x + 1, at.y + 1), ui::Fade(ui::ColInk(), 0.9f), text,
              nullptr, wrap);
  dl->AddText(f, sz, at, col, text, nullptr, wrap);
}

// The portrait slot: an inset recess with the speaker's initial struck in
// gold, the way the character panel's slots carry an engraving. A real
// portrait (a render of the speaker) can go here later; the slot is the
// layout's, not the monogram's.
void Monogram(ImDrawList* dl, ImVec2 a, float size, const std::string& name) {
  const ImVec2 b(a.x + size, a.y + size);
  ui::GradientV(dl, a, b, ui::Mix(ui::ColInk(), ui::ColDeep(), 0.5f), ui::ColDeep());
  ui::Grain(dl, a, b, 0.07f);
  ui::InnerShadow(dl, a, b, 10.0f, 0.55f);
  ui::Draw9(dl, "panel_inner", a, b);
  char letter[2] = {'?', 0};
  for (char ch : name)
    if (std::isalpha((unsigned char)ch)) {
      letter[0] = (char)std::toupper((unsigned char)ch);
      break;
    }
  // The initial at twice the chrome face: still the pixel font, scaled by a
  // whole number, so its pixels stay square.
  ImFont* f = ImGui::GetFont();
  const float sz = Snap(ImGui::GetFontSize() * 2.0f);
  const ImVec2 ts = f->CalcTextSizeA(sz, 1e9f, 0.0f, letter);
  const ImVec2 tp(Snap(a.x + (size - ts.x) * 0.5f), Snap(a.y + (size - ts.y) * 0.5f));
  dl->AddText(f, sz, ImVec2(tp.x + 2, tp.y + 2), ui::Fade(ui::ColInk(), 0.9f), letter);
  dl->AddText(f, sz, ImVec2(tp.x, tp.y + 2), ui::Fade(ui::ColGoldPale(), 0.2f), letter);
  dl->AddText(f, sz, tp, ui::ColGold(), letter);
  // Two gold rules under it, the rubricator's underline.
  const float ry = Snap(b.y - 22);
  dl->AddRectFilled(ImVec2(a.x + 30, ry), ImVec2(b.x - 30, ry + 2),
                    ui::Fade(ui::ColGoldDim(), 0.7f));
}

std::string Upper(const std::string& s) {
  std::string o = s;
  for (char& c : o) c = (char)std::toupper((unsigned char)c);
  return o;
}

}  // namespace

void DrawDialoguePanel(UIState& s) {
  UIState::TalkUI& t = s.talk;
  if (!t.open) return;
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 disp = io.DisplaySize;

  // The world stays in view: only the lower half falls away, banded, so the
  // panel is the lit thing without the scene going dark behind the speaker.
  {
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const float y0 = Snap(disp.y * 0.42f);
    ui::GradientV(bg, ImVec2(0, y0), ImVec2(disp.x, disp.y), ui::Fade(ui::ColInk(), 0.0f),
                  ui::Fade(ui::ColInk(), 0.62f));
  }

  // ---- layout, measured before anything is drawn -----------------------------
  const float panelW = Snap(std::min(kMaxW, disp.x - 120.0f));
  const float inner = panelW - 2 * (kFrame + kPad);
  const float textX0 = kPortrait + 24.0f;  // relative to the content's left edge
  const float textW = std::max(160.0f, inner - textX0);
  const float lineH = ImGui::GetTextLineHeight();
  const float textH = std::max(WrappedH(t.text.c_str(), textW), lineH);
  const float badgeW = 40.0f;  // the key cap column
  const float choiceW = textW - badgeW;
  std::vector<float> rowH;
  float choicesH = 0.0f;
  if (t.choices.empty()) {
    rowH.push_back(lineH + 8);
  } else {
    for (const std::string& c : t.choices)
      rowH.push_back(std::max(lineH, WrappedH(c.c_str(), choiceW)) + 8);
  }
  for (float h : rowH) choicesH += h + kRowGap;
  const float bodyH = std::max(kPortrait, textH + 26.0f + choicesH);
  const float panelH = Snap(kFrame + ui::kHeaderH + 14 + bodyH + kPad + kFrame);
  const ImVec2 wp(Snap((disp.x - panelW) * 0.5f), Snap(disp.y - kBottomGap - panelH));

  ImGui::SetNextWindowPos(wp);
  ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
  const ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
      ImGuiWindowFlags_NoSavedSettings;
  ImGui::Begin("##conversation", nullptr, kFlags);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wb(wp.x + panelW, wp.y + panelH);

  ui::PanelStyle st;
  st.darkMix = 0.34f;
  st.sheenPeak = 0.36f;
  st.shadow = 18.0f;
  ui::PanelBody(dl, wp, wb, st);
  ui::Draw9(dl, "panel", wp, wb);
  const std::string title = Upper(t.speaker.empty() ? std::string("...") : t.speaker);
  const char* hint = t.canLeave ? (t.choices.empty() ? "space  continue    esc  leave"
                                                     : "1-9  choose    esc  leave")
                                : (t.choices.empty() ? "space  continue" : "1-9  choose");
  // The key hint is drawn here rather than as HeaderBar's `right` caption so
  // it can be set in the SMALL face: a caption, not a second title.
  const float hb = ui::HeaderBar(dl, ImVec2(wp.x + kFrame, wp.y + kFrame),
                                 panelW - 2 * kFrame, title.c_str(), nullptr);
  {
    ImGui::PushFont(ui::FontSmall());
    const ImVec2 hs = ImGui::CalcTextSize(hint);
    const float hx = Snap(wb.x - kFrame - 14 - hs.x);
    const float hy = Snap(wp.y + kFrame + (ui::kHeaderH - hs.y) * 0.5f);
    ui::ShadowText(dl, ImVec2(hx, hy), ui::Fade(ui::ColParchDim(), 0.85f), hint);
    ImGui::PopFont();
  }

  const float cx = wp.x + kFrame + kPad;
  const float cy = hb + 14;
  Monogram(dl, ImVec2(cx, cy), kPortrait, t.speaker);

  // ---- the line ---------------------------------------------------------------
  const float tx = Snap(cx + textX0);
  WrappedText(dl, ImVec2(tx, cy + 2), ui::ColParch(), t.text.c_str(), textW);
  float y = Snap(cy + 2 + textH + 10);
  // The rule between what was said and what you can say: bronze, with a gold
  // lozenge at its head, the Subheading vocabulary without a label.
  dl->AddRectFilled(ImVec2(tx + 14, y + 4), ImVec2(tx + textW, y + 6),
                    ui::Fade(ui::ColBronze(), 0.7f));
  ui::PixelLozenge(dl, ImVec2(tx + 5, y + 5), 5.0f, ui::ColGoldDim(), true, 2.0f);
  y += 16;

  // ---- the choices ------------------------------------------------------------
  t.hover = -1;
  auto row = [&](int i, const char* key, const char* text, float h, bool dim) {
    const ImVec2 a(tx - 6, y), b(tx + textW, y + h);
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(i);
    const bool clicked = ImGui::InvisibleButton("##row", ImVec2(b.x - a.x, h));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hov) {
      t.hover = i;
      ui::GradientH(dl, a, b, ui::Fade(ui::ColMid(), 0.85f), ui::Fade(ui::ColMid(), 0.0f));
      dl->AddRectFilled(a, ImVec2(a.x + 2, b.y), ui::ColGold());
    }
    // THE KEY CAP, in the chrome face and not the 13 px badge the slots use:
    // beside a 26 px line a 13 px digit reads as a footnote, and the number
    // is the thing the player's hand is looking for. A recess, a lit top
    // band, a 2 px bronze rim that turns gold under the mouse.
    const ImVec2 ks = ImGui::CalcTextSize(key);
    const float capW = Snap(std::max(26.0f, ks.x + 12)), capH = Snap(lineH + 2);
    const ImVec2 ca(tx, Snap(y + 2)), cb(tx + capW, Snap(y + 2) + capH);
    dl->AddRectFilled(ca, cb, ui::Fade(ui::ColInk(), 0.9f));
    dl->AddRectFilled(ImVec2(ca.x + 2, ca.y + 2), ImVec2(cb.x - 2, ca.y + 6),
                      ui::Fade(ui::ColHi(), 0.45f));
    const ImU32 rim = hov ? ui::ColGold() : ui::ColBronze();
    dl->AddRectFilled(ca, ImVec2(cb.x, ca.y + 2), rim);
    dl->AddRectFilled(ImVec2(ca.x, cb.y - 2), cb, rim);
    dl->AddRectFilled(ca, ImVec2(ca.x + 2, cb.y), rim);
    dl->AddRectFilled(ImVec2(cb.x - 2, ca.y), cb, rim);
    ui::ShadowText(dl, ImVec2(Snap(ca.x + (capW - ks.x) * 0.5f), Snap(ca.y + (capH - ks.y) * 0.5f)),
                   hov ? ui::ColGoldPale() : ui::ColGold(), key);
    // Choice text starts on one column however wide the cap ("space" is the
    // one wide cap, and its row has nothing to line up with).
    const float textAt = Snap(tx + std::max(badgeW, capW + 12));
    const ImU32 col = hov ? ui::ColGoldHi() : dim ? ui::ColParchDim() : ui::ColParch();
    WrappedText(dl, ImVec2(textAt, y + 4), col, text, choiceW - (textAt - tx - badgeW));
    y += h + kRowGap;
    return clicked;
  };
  if (t.choices.empty()) {
    if (row(0, "space", "continue", rowH[0], true)) t.pick = 10;
  } else {
    for (int i = 0; i < (int)t.choices.size(); i++) {
      char key[4];
      std::snprintf(key, sizeof key, "%d", i + 1);
      if (row(i, i < 9 ? key : " ", t.choices[i].c_str(), rowH[i], false) && i < 9)
        t.pick = i + 1;
    }
  }
  ImGui::End();
}
