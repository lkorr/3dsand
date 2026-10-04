// contract_ui.cpp — the contract editor and the demon conversation's strip.
// See contract_ui.h.
//
// THE LOOK is the character screen's (ui/theme.h): the lit panel body, the
// 9-slice frame, the header bar, the 26 px pixel face for anything read and
// the 13 px face for captions. Laid on the 2 px lattice (Snap), no rounding.
// EVERY STRING IS FITTED to the box it sits in (Fit): a name too long for its
// row is cut with "..", never run under its neighbour.

#include "ui/contract_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "game/contract.h"
#include "ui/theme.h"

namespace {

constexpr float kFrame = 16.0f;
constexpr float kPad = 18.0f;

float Snap(float v) { return std::floor(v * 0.5f) * 2.0f; }

// `text` cut to fit `w` px in the current font, with ".." when cut.
std::string Fit(const std::string& text, float w) {
  if (w <= 0.0f) return std::string();
  if (ImGui::CalcTextSize(text.c_str()).x <= w) return text;
  const float dots = ImGui::CalcTextSize("..").x;
  std::string s = text;
  while (!s.empty() && ImGui::CalcTextSize(s.c_str()).x + dots > w) s.pop_back();
  return s + "..";
}

void Label(ImDrawList* dl, ImVec2 at, ImU32 col, const std::string& text, float w) {
  const std::string f = Fit(text, w);
  ui::ShadowText(dl, ImVec2(Snap(at.x), Snap(at.y)), col, f.c_str());
}

// A pixel token: ui::Button with the label fitted to `w` (the button is never
// wider than `w`). Returns clicked.
bool Token(const char* id, ImVec2 at, const std::string& label, float w, bool toggled = false) {
  const std::string f = Fit(label, std::max(8.0f, w - 24.0f));
  return ui::Button(id, ImVec2(Snap(at.x), Snap(at.y)), f.c_str(), toggled, w);
}
float TokenH() { return ImGui::GetTextLineHeight() + 8.0f; }

struct Preset {
  const char* label;
  const char* sel;
};
// The SIMPLE mode's "who" choices: a click on the token steps through these.
const Preset kWho[] = {
    {"me", "me"},
    {"who attacks me", "attacking(me)"},
    {"whom I attack", "attacked_by(me)"},
    {"hostile to me", "hostile_to(me)"},
    {"attack me, hostile", "attacking(me) & hostile_to(me)"},
    {"I attack, hostile", "attacked_by(me) & hostile_to(me)"},
    {"villagers", "faction(harrowby)"},
    {"everyone", "all"},
};
const char* WhoLabel(const std::string& sel) {
  for (const Preset& p : kWho)
    if (sel == p.sel) return p.label;
  return nullptr;
}
int WhoIndex(const std::string& sel) {
  for (int i = 0; i < (int)(sizeof kWho / sizeof kWho[0]); i++)
    if (sel == kWho[i].sel) return i;
  return -1;
}
const char* const kFetchMats[] = {"water", "blood", "sand", "salt", "oil", "sulfur"};
const char* const kLeaveM[] = {"4", "8", "16", "32"};
// A forbid-cast's footprint tag (D4's kit tags; contract.h): "" = any spell.
const char* const kCastTag[] = {"", "direct", "creates:fire", "creates:lava",
                                "alters:ground_under_target", "affects_body"};
const char* CastTagLabel(const std::string& t) {
  if (t.empty()) return "any spell";
  if (t == "direct") return "aimed at";
  if (t == "creates:fire") return "fire";
  if (t == "creates:lava") return "lava";
  if (t == "alters:ground_under_target") return "the ground under";
  if (t == "affects_body") return "on the body";
  return nullptr;
}

const char* KindWord(contract::Kind k) {
  return k == contract::Kind::Duty ? "MUST" : k == contract::Kind::Forbid ? "NEVER" : "IF";
}
ImU32 KindCol(contract::Kind k) {
  return k == contract::Kind::Duty     ? ui::ColGold()
         : k == contract::Kind::Forbid ? ui::ColEmber()
                                       : ui::ColBloodHi();
}

contract::Clause NewClause(contract::Kind k) {
  contract::Clause c;
  c.kind = k;
  c.verb = contract::VerbsFor(k).front();
  c.who = "me";
  if (k == contract::Kind::Penalty) {
    c.verb = contract::Verb::Attack;
    c.who = "faction(harrowby)";
    c.then = contract::Then::Pain;
  }
  return c;
}

// One InputText bound to a std::string (64 chars is every field here).
bool TextField(const char* id, const char* hint, std::string& v, float w, bool ro) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "%s", v.c_str());
  ImGui::PushItemWidth(w);
  if (ro) ImGui::BeginDisabled();
  const bool ch = ImGui::InputTextWithHint(id, hint, buf, sizeof buf);
  if (ro) ImGui::EndDisabled();
  ImGui::PopItemWidth();
  if (ch) v = buf;
  return ch;
}

const ImGuiWindowFlags kFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
    ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
    ImGuiWindowFlags_NoSavedSettings;

}  // namespace

// ---- the editor -------------------------------------------------------------------------

void DrawContractEditor(UIState& s) {
  UIState::ContractEdUI& E = s.contractEd;
  if (!E.open) return;
  const contract::Content& content = contract::GetContent();
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  const float W = Snap(std::min(1320.0f, disp.x - 60.0f));
  const float H = Snap(std::min(900.0f, disp.y - 60.0f));
  const ImVec2 wp(Snap((disp.x - W) * 0.5f), Snap((disp.y - H) * 0.5f));
  ImGui::SetNextWindowPos(wp);
  ImGui::SetNextWindowSize(ImVec2(W, H));
  ImGui::Begin("##contracts", nullptr, kFlags);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wb(wp.x + W, wp.y + H);
  ui::PanelStyle st;
  st.darkMix = 0.30f;
  st.sheenPeak = 0.40f;
  st.shadow = 18.0f;
  ui::PanelBody(dl, wp, wb, st);
  ui::Draw9(dl, "panel", wp, wb);
  const float hb = ui::HeaderBar(dl, ImVec2(wp.x + kFrame, wp.y + kFrame), W - 2 * kFrame,
                                 "CONTRACTS", nullptr);
  const float th = TokenH();
  // ---- the header's buttons: close, simple / full -------------------------------------
  {
    const float by = Snap(wp.y + kFrame + (ui::kHeaderH - th) * 0.5f);
    float bx = wb.x - kFrame - 10 - 110;
    if (Token("##ctclose", ImVec2(bx, by), "close", 110)) E.open = false;
    bx -= 150 + 8;
    if (Token("##ctmode", ImVec2(bx, by), E.advanced ? "full" : "simple", 150, E.advanced))
      E.advanced = !E.advanced;
    if (ImGui::IsItemHovered())
      ui::Tip(E.advanced ? "Full: write who, when and counters as text (contract.h's language)."
                         : "Simple: every token is a click that steps through its choices.");
  }
  const float x0 = wp.x + kFrame + kPad;
  const float y0 = hb + 14;
  const float innerW = W - 2 * (kFrame + kPad);
  const float bottom = wb.y - kFrame - kPad;
  const float colL = Snap(std::min(300.0f, innerW * 0.24f));
  const float gap = 24.0f;
  const float rx = x0 + colL + gap;
  const float rw = innerW - colL - gap;
  const float weightW = Snap(std::min(320.0f, rw * 0.30f));
  const float cardsW = rw - weightW - gap;
  const float wx = rx + cardsW + gap;
  const float lh = ImGui::GetTextLineHeight();

  // ---- left: the pages ------------------------------------------------------------------
  float y = ui::Subheading(dl, ImVec2(x0, y0), colL, "YOUR PAGES");
  auto pageRow = [&](const char* id, const contract::Page& p, bool selected) {
    const ImVec2 a(x0, y), b(x0 + colL, y + lh + 8);
    ImGui::SetCursorScreenPos(a);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(colL, lh + 8));
    const bool hov = ImGui::IsItemHovered();
    if (selected || hov)
      ui::GradientH(dl, a, b, ui::Fade(ui::ColMid(), selected ? 0.95f : 0.6f),
                    ui::Fade(ui::ColMid(), 0.0f));
    if (selected) dl->AddRectFilled(a, ImVec2(a.x + 2, b.y), ui::ColGold());
    contract::Page c = p;
    std::vector<std::string> errs;
    const bool ok = contract::Compile(c, errs);
    char wbuf[16];
    std::snprintf(wbuf, sizeof wbuf, "%d", contract::Weigh(c, content.tariff).total);
    const float ww = ImGui::CalcTextSize(wbuf).x;
    Label(dl, ImVec2(a.x + 10, a.y + 4), ok ? (selected ? ui::ColGoldHi() : ui::ColParch())
                                            : ui::ColEmber(),
          p.name, colL - ww - 28);
    ui::ShadowText(dl, ImVec2(Snap(b.x - ww - 8), Snap(a.y + 4)), ui::ColParchDim(), wbuf);
    y += lh + 10;
    return clicked;
  };
  for (int i = 0; i < (int)E.pages.size(); i++) {
    char id[24];
    std::snprintf(id, sizeof id, "##ctpage%d", i);
    if (pageRow(id, E.pages[(size_t)i], E.selected == i)) {
      E.selected = i;
      E.draft = E.pages[(size_t)i];
      E.dirty = false;
      E.opName = E.draft.name;
    }
  }
  if (E.pages.empty()) {
    ImGui::PushFont(ui::FontSmall());
    Label(dl, ImVec2(x0 + 10, y + 2), ui::ColParchDim(), "none yet: start from a stock page",
          colL - 20);
    ImGui::PopFont();
    y += lh;
  }
  if (Token("##ctnew", ImVec2(x0, y + 4), "new page", colL)) {
    E.draft = contract::Page{};
    E.draft.name = "my contract";
    E.draft.clauses.push_back(NewClause(contract::Kind::Duty));
    contract::Clause f = NewClause(contract::Kind::Forbid);
    E.draft.clauses.push_back(f);
    E.selected = -1;
    E.dirty = true;
    E.opName.clear();
  }
  y += th + 18;
  y = ui::Subheading(dl, ImVec2(x0, y), colL, "STOCK");
  for (int i = 0; i < (int)E.stock.size(); i++) {
    char id[24];
    std::snprintf(id, sizeof id, "##ctstock%d", i);
    if (pageRow(id, E.stock[(size_t)i], E.selected == -2 - i)) {
      E.selected = -2 - i;
      E.draft = E.stock[(size_t)i];
      E.dirty = false;
      E.opName.clear();
    }
  }
  {
    ImGui::PushFont(ui::FontSmall());
    const char* note = "Stock pages are safe against an imp: they forbid it to strike or curse "
                       "you and run one day. Copy one to change it.";
    const ImVec2 ns = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), 1e9f, colL, note);
    if (y + ns.y + 8 < bottom)
      dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x0, y + 6), ui::ColParchDim(),
                  note, nullptr, colL);
    ImGui::PopFont();
  }

  // ---- right: the page being written ------------------------------------------------------
  contract::Page& D = E.draft;
  const bool ro = E.selected <= -2;
  const bool any = E.selected != -1 || E.dirty || !D.name.empty();
  if (!any) {
    Label(dl, ImVec2(rx, y0 + 4), ui::ColParchDim(), "Pick a page, or start a new one.", rw);
    ImGui::End();
    return;
  }
  float ry = y0;
  // name + term
  {
    ImGui::SetCursorScreenPos(ImVec2(rx, ry));
    if (TextField("##ctname", "name this contract", D.name, cardsW, ro)) E.dirty = true;
    ry += ImGui::GetFrameHeight() + 8;
    struct T {
      const char* label;
      int hours;
    } const kTerms[] = {{"1 day", 24}, {"3 days", 72}, {"7 days", 168}, {"forever", 0}};
    float tx = rx;
    const float tw = Snap((cardsW - 3 * 6) / 4);
    for (const T& t : kTerms) {
      char id[24];
      std::snprintf(id, sizeof id, "##ctterm%d", t.hours);
      if (ro) ImGui::BeginDisabled();
      if (Token(id, ImVec2(tx, ry), t.label, tw, D.hours == t.hours) && !ro) {
        D.hours = t.hours;
        E.dirty = true;
      }
      if (ro) ImGui::EndDisabled();
      tx += tw + 6;
    }
    ry += th + 10;
  }
  // the stock templates (simple mode) / copy (a stock page)
  if (ro) {
    if (Token("##ctcopy", ImVec2(rx, ry), "copy to my pages", 280)) {
      D.name = "my " + D.name;
      D.stock = false;
      E.selected = -1;
      E.dirty = true;
      E.opName.clear();
    }
    ry += th + 12;
  } else if (!E.advanced) {
    ImGui::PushFont(ui::FontSmall());
    Label(dl, ImVec2(rx, ry + 8), ui::ColParchDim(), "start from", 90);
    ImGui::PopFont();
    float tx = rx + 96;
    const float tw = Snap(std::min(220.0f, (cardsW - 96 - 2 * 6) / 3));
    for (int i = 0; i < (int)E.stock.size() && i < 3; i++) {
      char id[24];
      std::snprintf(id, sizeof id, "##cttmpl%d", i);
      std::string short_ = E.stock[(size_t)i].name;
      const size_t comma = short_.find(',');
      if (comma != std::string::npos) short_ = short_.substr(0, comma);
      if (Token(id, ImVec2(tx, ry), short_, tw)) {
        const std::string keep = D.name;
        D = E.stock[(size_t)i];
        D.name = keep;
        D.stock = false;
        E.dirty = true;
      }
      if (ImGui::IsItemHovered()) ui::Tip("Replace the clauses with this stock page's.");
      tx += tw + 6;
    }
    ry += th + 12;
  }
  ry = ui::Subheading(dl, ImVec2(rx, ry), cardsW, "CLAUSES");
  // ---- the cards, in a scrolling child ------------------------------------------------------
  contract::Page compiled = D;
  std::vector<std::string> errs;
  const bool compiles = contract::Compile(compiled, errs);
  const contract::Weight weight = contract::Weigh(compiled, content.tariff);
  const float addH = th + 14;
  ImGui::PushFont(ui::FontSmall());
  const float smallFrame = ImGui::GetFrameHeight();
  ImGui::PopFont();
  const float countersH = E.advanced ? smallFrame + 12 : 0.0f;
  const float cardsBottom = bottom - th - 16 - addH - countersH;
  ImGui::SetCursorScreenPos(ImVec2(rx, ry));
  ImGui::BeginChild("##ctcards", ImVec2(cardsW, std::max(60.0f, cardsBottom - ry)), false,
                    ImGuiWindowFlags_NoBackground);
  {
    ImDrawList* cd = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    float cy = c0.y;
    int remove = -1;
    const float cardH = th + 12 + (E.advanced ? smallFrame * 2 + 14 : 0.0f);
    for (int i = 0; i < (int)D.clauses.size(); i++) {
      contract::Clause& c = D.clauses[(size_t)i];
      const ImVec2 a(c0.x, cy), b(c0.x + cardsW - 14, cy + cardH);
      ui::GradientV(cd, a, b, ui::Mix(ui::ColInk(), ui::ColDeep(), 0.4f), ui::ColDeep());
      cd->AddRectFilled(a, ImVec2(a.x + 4, b.y), KindCol(c.kind));
      ui::Draw9(cd, "panel_inner", a, b);
      ImGui::PushID(i);
      float tx = a.x + 14;
      const float ty = a.y + 6;
      const float kw = ImGui::CalcTextSize("NEVER").x;
      ui::ShadowText(cd, ImVec2(Snap(tx), Snap(ty + 4)), KindCol(c.kind), KindWord(c.kind));
      tx += kw + 12;
      // weight of this clause, right; remove, right of that
      char wbuf[16];
      const int line = 1 + i;   // Weigh's lines: the term first, then one per clause
      std::snprintf(wbuf, sizeof wbuf, "+%d",
                    line < (int)weight.lines.size() ? weight.lines[(size_t)line].weight : 0);
      const float xw = 40.0f;
      const float wvw = ImGui::CalcTextSize(wbuf).x;
      const float rightX = b.x - 10 - xw - 10 - wvw;
      if (!ro && Token("##x", ImVec2(b.x - 10 - xw, ty), "x", xw)) remove = i;
      ui::ShadowText(cd, ImVec2(Snap(rightX), Snap(ty + 4)), ui::ColParchDim(), wbuf);
      const float avail = rightX - 12 - tx;
      // verb
      // The verb token is as wide as the widest verb, never cut.
      const float vw = Snap(std::min(avail * 0.4f, ImGui::CalcTextSize("follow").x + 30.0f));
      if (ro) ImGui::BeginDisabled();
      if (Token("##verb", ImVec2(tx, ty), contract::VerbName(c.verb), vw) && !ro) {
        const auto& vs = contract::VerbsFor(c.kind);
        size_t k = 0;
        while (k < vs.size() && vs[k] != c.verb) k++;
        c.verb = vs[(k + 1) % vs.size()];
        E.dirty = true;
      }
      tx += vw + 6;
      const float rest = rightX - 12 - tx;
      // object / argument tokens by verb
      if (c.verb == contract::Verb::Fetch) {
        const float w1 = Snap(rest * 0.5f - 3);
        if (Token("##arg", ImVec2(tx, ty), c.arg.empty() ? "what?" : c.arg, w1) && !ro) {
          int k = -1;
          for (int m = 0; m < 6; m++)
            if (c.arg == kFetchMats[m]) k = m;
          c.arg = kFetchMats[(k + 1) % 6];
          E.dirty = true;
        }
        if (Token("##into", ImVec2(tx + w1 + 6, ty),
                  c.into.empty() ? std::string("into any vessel") : "into " + c.into, w1) &&
            !ro) {
          c.into = c.into.empty() ? "flask" : c.into == "flask" ? "pouch" : "";
          E.dirty = true;
        }
      } else if (c.verb == contract::Verb::Spin || c.verb == contract::Verb::Goto) {
        Label(cd, ImVec2(tx, ty + 4), ui::ColParchDim(),
              c.verb == contract::Verb::Spin ? "on the spot"
                                             : (c.arg.empty() || c.arg == "here"
                                                    ? std::string("where I stand now")
                                                    : c.arg),
              rest);
      } else {
        const bool leave = c.verb == contract::Verb::Leave ||
                           (c.verb == contract::Verb::Cast && c.kind == contract::Kind::Forbid);
        const bool castTag = c.verb == contract::Verb::Cast;
        const bool pen = c.kind == contract::Kind::Penalty;
        const float wWho = Snap(rest * (leave || pen ? 0.56f : 1.0f) - 3);
        const char* wl = WhoLabel(c.who);
        if (Token("##who", ImVec2(tx, ty), wl ? std::string(wl) : c.who, wWho) && !ro) {
          const int k = WhoIndex(c.who);
          c.who = kWho[(k + 1) % (int)(sizeof kWho / sizeof kWho[0])].sel;
          E.dirty = true;
        }
        if (ImGui::IsItemHovered()) ui::Tip(c.who.c_str());
        float tx2 = tx + wWho + 6;
        const float w2 = rest - wWho - 6;
        if (castTag && !pen) {
          const char* tl = CastTagLabel(c.arg);
          if (Token("##tag", ImVec2(tx2, ty), tl ? std::string(tl) : c.arg, w2) && !ro) {
            int k = -1;
            for (int m = 0; m < 6; m++)
              if (c.arg == kCastTag[m]) k = m;
            c.arg = kCastTag[(k + 1) % 6];
            E.dirty = true;
          }
          if (ImGui::IsItemHovered())
            ui::Tip("Which spells: any, or only those whose footprint matches (the kit's tags).");
        } else if (leave && !pen) {
          if (Token("##m", ImVec2(tx2, ty), "> " + (c.arg.empty() ? std::string("?") : c.arg) + " m",
                    w2) &&
              !ro) {
            int k = -1;
            for (int m = 0; m < 4; m++)
              if (c.arg == kLeaveM[m]) k = m;
            c.arg = kLeaveM[(k + 1) % 4];
            E.dirty = true;
          }
        } else if (pen) {
          const float wm = leave ? Snap(w2 * 0.4f) : 0.0f;
          if (leave &&
              Token("##m", ImVec2(tx2, ty), (c.arg.empty() ? std::string("?") : c.arg) + " m", wm) &&
              !ro) {
            int k = -1;
            for (int m = 0; m < 4; m++)
              if (c.arg == kLeaveM[m]) k = m;
            c.arg = kLeaveM[(k + 1) % 4];
            E.dirty = true;
          }
          if (leave) tx2 += wm + 6;
          const std::string then = std::string("-> ") +
                                   (c.then == contract::Then::None ? "?" : contract::ThenName(c.then));
          if (Token("##then", ImVec2(tx2, ty), then, rest - (tx2 - tx)) && !ro) {
            c.then = (contract::Then)(((int)c.then % 3) + 1);
            E.dirty = true;
          }
        }
      }
      if (ro) ImGui::EndDisabled();
      // FULL: the who / when / on text
      if (E.advanced) {
        ImGui::PushFont(ui::FontSmall());
        const float fy = ty + th + 6;
        const float fw = Snap((b.x - a.x - 28 - 12) / 2);
        ImGui::SetCursorScreenPos(ImVec2(a.x + 14, fy));
        if (TextField("##whot", "who (a selector)", c.who, fw, ro)) E.dirty = true;
        ImGui::SetCursorScreenPos(ImVec2(a.x + 14 + fw + 12, fy));
        if (TextField("##whent", "when (dist < 6 & ...)", c.when, fw, ro)) E.dirty = true;
        const float fy2 = fy + smallFrame + 6;
        ImGui::SetCursorScreenPos(ImVec2(a.x + 14, fy2));
        if (TextField("##ont", "on (counter+1)", c.on, fw, ro)) E.dirty = true;
        if (c.verb == contract::Verb::Fetch || c.verb == contract::Verb::Goto ||
            c.verb == contract::Verb::Leave) {
          ImGui::SetCursorScreenPos(ImVec2(a.x + 14 + fw + 12, fy2));
          if (TextField("##argt",
                        c.verb == contract::Verb::Goto ? "x y z, or here" : c.verb == contract::Verb::Fetch ? "material" : c.verb == contract::Verb::Cast ? "tag (creates:fire)" : "metres",
                        c.arg, fw, ro))
            E.dirty = true;
        }
        ImGui::PopFont();
      } else if (!c.when.empty()) {
        ImGui::PushFont(ui::FontSmall());
        Label(cd, ImVec2(a.x + 14, b.y - 16), ui::ColParchDim(), "when " + c.when, b.x - a.x - 28);
        ImGui::PopFont();
      }
      ImGui::PopID();
      cy += cardH + 8;
    }
    if (remove >= 0) {
      D.clauses.erase(D.clauses.begin() + remove);
      E.dirty = true;
    }
    ImGui::SetCursorScreenPos(ImVec2(c0.x, cy));
    ImGui::Dummy(ImVec2(1, 1));
  }
  ImGui::EndChild();
  // ---- add a clause -------------------------------------------------------------------------
  float ay = cardsBottom + 8;
  if (!ro) {
    const float bw = Snap((cardsW - 2 * 8) / 3);
    const bool full = (int)D.clauses.size() >= contract::kMaxClauses;
    if (full) ImGui::BeginDisabled();
    if (Token("##addduty", ImVec2(rx, ay), "+ must", bw) && !full) {
      D.clauses.push_back(NewClause(contract::Kind::Duty));
      E.dirty = true;
    }
    if (ImGui::IsItemHovered()) ui::Tip("A DUTY: follow, guard, goto, fetch or spin.");
    if (Token("##addforbid", ImVec2(rx + bw + 8, ay), "+ never", bw) && !full) {
      D.clauses.push_back(NewClause(contract::Kind::Forbid));
      E.dirty = true;
    }
    if (ImGui::IsItemHovered())
      ui::Tip("A PROHIBITION: a hard filter the binding enforces. Dear.");
    if (Token("##addpen", ImVec2(rx + 2 * (bw + 8), ay), "+ if", bw) && !full) {
      D.clauses.push_back(NewClause(contract::Kind::Penalty));
      E.dirty = true;
    }
    if (ImGui::IsItemHovered())
      ui::Tip("A PENALTY: if it does this, that happens to it. Cheap, but only a deterrent.");
    if (full) ImGui::EndDisabled();
  }
  ay += addH;
  if (E.advanced) {
    std::string counters;
    for (size_t i = 0; i < D.counters.size(); i++) counters += (i ? ", " : "") + D.counters[i];
    ImGui::SetCursorScreenPos(ImVec2(rx, ay));
    ImGui::PushFont(ui::FontSmall());
    const bool chCounters =
        TextField("##ctcounters", "counters, by name: fetched, phase", counters, cardsW, ro);
    ImGui::PopFont();
    if (chCounters) {
      D.counters.clear();
      std::string cur;
      for (char ch : counters + ",") {
        if (ch == ',') {
          while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
          while (!cur.empty() && cur.back() == ' ') cur.pop_back();
          if (!cur.empty()) D.counters.push_back(cur);
          cur.clear();
        } else {
          cur += ch;
        }
      }
      E.dirty = true;
    }
    ay += countersH;
  }
  // ---- save / delete ----------------------------------------------------------------------
  {
    const float by = bottom - th;
    if (!ro) {
      const bool canSave = compiles && E.dirty;
      if (!canSave) ImGui::BeginDisabled();
      if (Token("##ctsave", ImVec2(rx, by), "save", 140) && canSave) {
        E.op = UIState::ContractEdUI::Save;
        E.dirty = false;
      }
      if (!canSave) ImGui::EndDisabled();
      if (E.selected >= 0 && Token("##ctdel", ImVec2(rx + 148, by), "delete", 140)) {
        E.op = UIState::ContractEdUI::Delete;
        E.opName = D.name;
      }
    }
    if (!E.status.empty()) {
      ImGui::PushFont(ui::FontSmall());
      Label(dl, ImVec2(rx + 300, by + 8), ui::ColParchDim(), E.status, cardsW - 300);
      ImGui::PopFont();
    }
  }
  // ---- the weight, live -----------------------------------------------------------------------
  {
    float wy = ui::Subheading(dl, ImVec2(wx, y0), weightW, "WEIGHT");
    const ImVec2 a(wx, wy), b(wx + weightW, bottom);
    ui::GradientV(dl, a, b, ui::Mix(ui::ColInk(), ui::ColDeep(), 0.5f), ui::ColDeep());
    ui::Grain(dl, a, b, 0.06f);
    ui::InnerShadow(dl, a, b, 10.0f, 0.5f);
    ui::Draw9(dl, "panel_inner", a, b);
    const float px = a.x + 14, pw = weightW - 28;
    float ly = a.y + 12;
    ImGui::PushFont(ui::FontSmall());
    const float sl = ImGui::GetTextLineHeight() + 4;
    for (const contract::WeightLine& l : weight.lines) {
      if (ly + sl > b.y - 150) {
        Label(dl, ImVec2(px, ly), ui::ColParchDim(), "...", pw);
        ly += sl;
        break;
      }
      char v[16];
      std::snprintf(v, sizeof v, "%d", l.weight);
      const float vw = ImGui::CalcTextSize(v).x;
      Label(dl, ImVec2(px, ly), ui::ColParch(), l.label, pw - vw - 12);
      ui::ShadowText(dl, ImVec2(Snap(px + pw - vw), Snap(ly)), ui::ColGoldHi(), v);
      ly += sl;
    }
    ImGui::PopFont();
    ly += 6;
    dl->AddRectFilled(ImVec2(px, ly), ImVec2(px + pw, ly + 2), ui::Fade(ui::ColBronze(), 0.8f));
    ly += 8;
    char tot[32];
    std::snprintf(tot, sizeof tot, "%d", weight.total);
    ui::ShadowText(dl, ImVec2(Snap(px), Snap(ly)), ui::ColGoldPale(), "TOTAL");
    ui::ShadowText(dl, ImVec2(Snap(px + pw - ImGui::CalcTextSize(tot).x), Snap(ly)),
                   ui::ColGoldPale(), tot);
    ly += lh + 10;
    ImGui::PushFont(ui::FontSmall());
    char up[96];
    // Two short lines rather than one long one: the column is narrow.
    std::snprintf(up, sizeof up, "upkeep while bound:");
    Label(dl, ImVec2(px, ly), ui::ColMana(), up, pw);
    ly += sl;
    std::snprintf(up, sizeof up, "%s (power %d): %d mana", E.upkeepFor.c_str(), E.upkeepPower,
                  contract::UpkeepFor(E.upkeepPower, content.tariff));
    Label(dl, ImVec2(px, ly), ui::ColMana(), up, pw);
    ly += sl;
    char need[96];
    std::snprintf(need, sizeof need, "binds iff circle >= %d", E.upkeepPower + weight.total);
    Label(dl, ImVec2(px, ly), ui::ColParch(), need, pw);
    ly += sl + 4;
    for (size_t i = 0; i < errs.size() && i < 4 && ly + sl < b.y - 6; i++) {
      Label(dl, ImVec2(px, ly), ui::ColEmber(), errs[i], pw);
      ly += sl;
    }
    if (compiles && ly + sl < b.y - 6)
      Label(dl, ImVec2(px, ly), ui::ColParchDim(), "reads clean", pw);
    ImGui::PopFont();
  }
  ImGui::End();
}

// ---- the conversation's strip ---------------------------------------------------------------

void DrawDemonTalkStrip(UIState& s) {
  UIState::DemonTalkUI& T = s.demonTalk;
  if (!T.active) return;
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  const float W = Snap(std::min(900.0f, disp.x - 120.0f));
  const float lh = ImGui::GetTextLineHeight();
  const float th = TokenH();
  ImGui::PushFont(ui::FontSmall());
  const float sl = ImGui::GetTextLineHeight() + 4;
  ImGui::PopFont();
  const bool picker = T.pickerOpen && !T.offers.empty();
  const int rows = picker ? (int)std::min<size_t>(T.offers.size(), 8) : 0;
  const float H = Snap(kFrame * 2 + ui::kHeaderH + 14 + (lh + 8) + (th + 8) +
                       (T.bound ? lh + 6 : 0) + (T.lastResult.empty() ? 0 : sl + 4) +
                       (picker ? 30 + rows * (th + 6) + th + 8 : 0) + kPad);
  const ImVec2 wp(Snap((disp.x - W) * 0.5f), 36.0f);
  ImGui::SetNextWindowPos(wp);
  ImGui::SetNextWindowSize(ImVec2(W, H));
  ImGui::Begin("##demonstrip", nullptr, kFlags);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wb(wp.x + W, wp.y + H);
  ui::PanelStyle st;
  st.darkMix = 0.26f;
  st.sheenPeak = 0.5f;
  st.shadow = 14.0f;
  ui::PanelBody(dl, wp, wb, st);
  ui::Draw9(dl, "panel", wp, wb);
  std::string title = T.name;
  for (char& c : title) c = (char)std::toupper((unsigned char)c);
  const float hb = ui::HeaderBar(dl, ImVec2(wp.x + kFrame, wp.y + kFrame), W - 2 * kFrame,
                                 title.c_str(), T.bound ? "BOUND, IN THE CIRCLE" : "CONTAINED");
  const float x = wp.x + kFrame + kPad;
  const float iw = W - 2 * (kFrame + kPad);
  float y = hb + 12;
  // the circle against it
  {
    const int margin = T.strength - T.power - (T.bound ? T.weight : 0);
    char a[96];
    if (T.bound)
      std::snprintf(a, sizeof a, "CIRCLE %d   POWER %d + CONTRACT %d", T.strength, T.power,
                    T.weight);
    else
      std::snprintf(a, sizeof a, "CIRCLE %d   POWER %d", T.strength, T.power);
    char m[32];
    std::snprintf(m, sizeof m, "MARGIN %+d", margin);
    const float mw = ImGui::CalcTextSize(m).x;
    Label(dl, ImVec2(x, y), ui::ColParch(), a, iw - mw - 16);
    ui::ShadowText(dl, ImVec2(Snap(x + iw - mw), Snap(y)),
                   margin >= 0 ? ui::ColGoldHi() : ui::ColEmber(), m);
    y += lh + 8;
  }
  // the gaze
  {
    const char* rule = T.gazeHold ? "MEET HIS EYES" : "DO NOT MEET HIS EYES";
    const char* now = T.lookingAway ? "you look away" : "you look at him";
    const float bw = 230.0f;
    ImGui::PushFont(ui::FontSmall());
    Label(dl, ImVec2(x, y + 2), T.gazeBroken ? ui::ColEmber() : ui::ColParch(), rule,
          iw - bw - 200);
    Label(dl, ImVec2(x, y + 2 + sl), ui::ColParchDim(), now, iw - bw - 200);
    ImGui::PopFont();
    // strain pips
    constexpr int kPips = 10;
    const float pw = 6.0f, ph = 10.0f, pg = 2.0f;
    const float px = Snap(x + iw - bw - 16 - kPips * (pw + pg));
    const int lit = (int)std::ceil(T.strain * kPips - 1e-4f);
    for (int i = 0; i < kPips; i++)
      dl->AddRectFilled(ImVec2(px + i * (pw + pg), y + 8),
                        ImVec2(px + i * (pw + pg) + pw, y + 8 + ph),
                        i < lit ? (T.gazeBroken ? ui::ColEmber() : ui::ColParch()) : ui::ColDeep());
    if (Token("##lookaway", ImVec2(x + iw - bw, y), "look away (L)", bw, T.lookAwayToggle))
      T.lookAwayToggle = !T.lookAwayToggle;
    if (ImGui::IsItemHovered())
      ui::Tip("Hold L, or click to keep looking away. Breaking the demon's gaze rule strains "
              "the circle.");
    y += th + 8;
  }
  if (T.bound) {
    char b[160];
    std::snprintf(b, sizeof b, "Bound by \"%s\", holding %d mana", T.contract.c_str(), T.upkeep);
    Label(dl, ImVec2(x, y), ui::ColGold(), b, iw);
    y += lh + 6;
  }
  if (!T.lastResult.empty()) {
    ImGui::PushFont(ui::FontSmall());
    Label(dl, ImVec2(x, y), ui::ColParchDim(), "last offer: " + T.lastResult, iw);
    ImGui::PopFont();
    y += sl + 4;
  }
  if (picker) {
    y = ui::Subheading(dl, ImVec2(x, y), iw, "PRESENT A CONTRACT");
    const float bw = 150.0f;
    for (int i = 0; i < rows; i++) {
      const UIState::DemonTalkUI::Offer& o = T.offers[(size_t)i];
      const int margin = T.strength - T.power - o.weight;
      char r[48];
      std::snprintf(r, sizeof r, "weight %d  margin %+d", o.weight, margin);
      const float rw = ImGui::CalcTextSize(r).x;
      Label(dl, ImVec2(x, y + 4), o.stock ? ui::ColParchDim() : ui::ColParch(),
            o.name, iw - bw - rw - 32);
      ui::ShadowText(dl, ImVec2(Snap(x + iw - bw - 16 - rw), Snap(y + 4)),
                     margin >= 0 ? ui::ColGoldHi() : ui::ColEmber(), r);
      char id[24];
      std::snprintf(id, sizeof id, "##offer%d", i);
      if (!o.compiles) ImGui::BeginDisabled();
      if (Token(id, ImVec2(x + iw - bw, y), "present", bw) && o.compiles) T.presentPick = o.hash;
      if (!o.compiles) ImGui::EndDisabled();
      y += th + 6;
    }
    if (Token("##pickback", ImVec2(x, y + 2), "back", 120)) T.pickerOpen = false;
  }
  ImGui::End();
}
