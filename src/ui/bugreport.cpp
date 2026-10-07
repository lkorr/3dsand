// bugreport.cpp — the F12 bug report panel and its writer. See bugreport.h.
//
// THE LOOK is the conversation panel's (ui/theme.h): lit panel body, 9-slice
// frame, header bar, the 26 px pixel face, everything on the 2 px lattice.
// The thumbnails are MOSAICS — the shot box-averaged down to a few thousand
// cells and drawn as filled rects — because the engine has no texture-sampling
// path for UI images (theme.h, "WHY CHROME LIVES IN THE FONT ATLAS"), and a
// 4 px cell is pixel art anyway.
//
// PNG via Windows' own codec (WIC): no image library is vendored, and a 1080p
// BMP is 6 MB a picture.

#include "ui/bugreport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "ui/theme.h"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace {

constexpr float kFrame = 16.0f;   // the 9-slice frame's thickness (inventory_ui)
constexpr float kPad = 18.0f;
constexpr float kMaxW = 1100.0f;
constexpr int kThumbCells = 60;   // thumbnail width in mosaic cells
constexpr float kCell = 4.0f;     // one cell, screen px
constexpr float kThumbGap = 14.0f;
constexpr float kTextH = 220.0f;

float Snap(float v) { return std::floor(v * 0.5f) * 2.0f; }

// std::string-backed InputText (the imgui_stdlib pattern; that file is not
// compiled into imgui_lib).
int ResizeCb(ImGuiInputTextCallbackData* d) {
  if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    std::string* s = (std::string*)d->UserData;
    s->resize((size_t)d->BufTextLen);
    d->Buf = s->data();
  }
  return 0;
}

#ifdef _WIN32
bool WritePng(const std::filesystem::path& path, const std::vector<uint8_t>& rgba, uint32_t w,
              uint32_t h, std::string& err) {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  IWICImagingFactory* fac = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  IPropertyBag2* props = nullptr;
  do {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac)))) { err = "WIC factory"; break; }
    if (FAILED(fac->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE))) {
      err = "open " + path.string();
      break;
    }
    if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, &props)) || FAILED(frame->Initialize(props)) ||
        FAILED(frame->SetSize(w, h))) {
      err = "PNG encoder";
      break;
    }
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat24bppBGR) {
      err = "PNG pixel format";
      break;
    }
    const uint32_t stride = w * 3;
    std::vector<uint8_t> bgr((size_t)stride * h);
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
      bgr[i * 3 + 0] = rgba[i * 4 + 2];
      bgr[i * 3 + 1] = rgba[i * 4 + 1];
      bgr[i * 3 + 2] = rgba[i * 4 + 0];
    }
    if (FAILED(frame->WritePixels(h, stride, (UINT)bgr.size(), bgr.data())) ||
        FAILED(frame->Commit()) || FAILED(enc->Commit())) {
      err = "PNG write";
      break;
    }
    ok = true;
  } while (false);
  if (props) props->Release();
  if (frame) frame->Release();
  if (enc) enc->Release();
  if (stream) stream->Release();
  if (fac) fac->Release();
  if (SUCCEEDED(init)) CoUninitialize();
  return ok;
}
#else
bool WritePng(const std::filesystem::path&, const std::vector<uint8_t>&, uint32_t, uint32_t,
              std::string& err) {
  err = "PNG writing is Windows-only (WIC)";
  return false;
}
#endif

void Thumb(ImDrawList* dl, ImVec2 a, const UIState::BugReportUI::Shot& sh) {
  const ImVec2 b(a.x + sh.tw * kCell, a.y + sh.th * kCell);
  ui::DropShadow(dl, a, b, 6.0f, 0.5f);
  for (uint32_t y = 0; y < sh.th; y++)
    for (uint32_t x = 0; x < sh.tw; x++) {
      const ImVec2 p(a.x + x * kCell, a.y + y * kCell);
      dl->AddRectFilled(p, ImVec2(p.x + kCell, p.y + kCell), sh.thumb[(size_t)y * sh.tw + x]);
    }
  ui::Draw9(dl, "panel_inner", ImVec2(a.x - 4, a.y - 4), ImVec2(b.x + 4, b.y + 4));
}

}  // namespace

void BugReportAddShot(UIState& s, std::vector<uint8_t> rgba, uint32_t w, uint32_t h) {
  UIState::BugReportUI::Shot sh;
  sh.w = w;
  sh.h = h;
  sh.tw = kThumbCells;
  sh.th = std::max(1u, (uint32_t)std::lround((double)kThumbCells * h / std::max(1u, w)));
  sh.thumb.resize((size_t)sh.tw * sh.th);
  for (uint32_t ty = 0; ty < sh.th; ty++)
    for (uint32_t tx = 0; tx < sh.tw; tx++) {
      const uint32_t x0 = tx * w / sh.tw, x1 = std::max(x0 + 1, (tx + 1) * w / sh.tw);
      const uint32_t y0 = ty * h / sh.th, y1 = std::max(y0 + 1, (ty + 1) * h / sh.th);
      uint64_t r = 0, g = 0, bl = 0, n = 0;
      for (uint32_t y = y0; y < y1 && y < h; y++)
        for (uint32_t x = x0; x < x1 && x < w; x++) {
          const uint8_t* p = &rgba[((size_t)y * w + x) * 4];
          r += p[0];
          g += p[1];
          bl += p[2];
          n++;
        }
      n = std::max<uint64_t>(n, 1);
      sh.thumb[(size_t)ty * sh.tw + tx] =
          IM_COL32((int)(r / n), (int)(g / n), (int)(bl / n), 255);
    }
  sh.rgba = std::move(rgba);
  s.bug.shots.push_back(std::move(sh));
}

bool WriteBugReport(const std::string& root, const UIState::BugReportUI& r,
                    const std::string& context, std::string& outDir, std::string& err) {
  namespace fs = std::filesystem;
  const std::time_t now = std::time(nullptr);
  std::tm lt{};
#ifdef _WIN32
  localtime_s(&lt, &now);
#else
  localtime_r(&now, &lt);
#endif
  char stamp[32], human[32];
  std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &lt);
  std::strftime(human, sizeof human, "%Y-%m-%d %H:%M:%S", &lt);
  fs::path dir = fs::path(root).lexically_normal() / stamp;
  // Two submits inside one second get their own folders.
  for (int k = 2; fs::exists(dir); k++)
    dir = fs::path(root).lexically_normal() / (std::string(stamp) + "_" + std::to_string(k));
  outDir = dir.string();
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    err = "mkdir " + outDir + ": " + ec.message();
    return false;
  }
  for (size_t i = 0; i < r.shots.size(); i++) {
    const auto& sh = r.shots[i];
    if (!WritePng(dir / ("shot_" + std::to_string(i + 1) + ".png"), sh.rgba, sh.w, sh.h, err))
      return false;
  }
  std::string text = r.text;
  while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
  std::ofstream md(dir / "report.md", std::ios::binary);
  if (!md) {
    err = "open report.md";
    return false;
  }
  md << "# Bug report " << human << "\n\n";
  md << (text.empty() ? std::string("(no description)") : text) << "\n\n";
  md << "## Screenshots\n\n";
  for (size_t i = 0; i < r.shots.size(); i++)
    md << "### Shot " << i + 1 << " (" << r.shots[i].w << "x" << r.shots[i].h << ")\n\n"
       << "![shot " << i + 1 << "](shot_" << i + 1 << ".png)\n\n";
  md << "## Context (at the first shot)\n\n" << context;
  if (!md.good()) {
    err = "write report.md";
    return false;
  }
  return true;
}

void DrawBugReport(UIState& s) {
  UIState::BugReportUI& r = s.bug;
  if (r.hideForShot) return;
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  r.toastLeft = std::max(0.0f, r.toastLeft - ImGui::GetIO().DeltaTime);
  const ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
      ImGuiWindowFlags_NoSavedSettings;

  // ---- folded: a strip at the top saying a report is waiting --------------
  if (!r.open) {
    const bool toast = !r.toast.empty() && r.toastLeft > 0.0f;
    if (!r.collecting && !toast) return;
    ImGui::PushFont(ui::FontSmall());
    char line[256];
    if (r.collecting)
      std::snprintf(line, sizeof line,
                    "BUG REPORT  %zu shot%s   F12 capture next   shift+F12 back to report",
                    r.shots.size(), r.shots.size() == 1 ? "" : "s");
    else
      std::snprintf(line, sizeof line, "%s", r.toast.c_str());
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(line);
    const ImVec2 a(Snap((disp.x - ts.x) * 0.5f - 14), 12), b(Snap(a.x + ts.x + 28), Snap(12 + ts.y + 14));
    ui::PanelBody(dl, a, b);
    dl->AddRectFilled(a, ImVec2(a.x + 2, b.y), r.collecting ? ui::ColBloodHi() : ui::ColGold());
    ui::ShadowText(dl, ImVec2(a.x + 14, Snap(a.y + 7)), ui::ColGoldPale(), line);
    ImGui::PopFont();
    return;
  }

  // ---- the panel ------------------------------------------------------------
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  ui::ScreenDim(bg, disp, 0.35f, 0.3f);

  ImGui::PushFont(ui::FontLarge());
  const float lineH = ImGui::GetTextLineHeight();
  const float panelW = Snap(std::min(kMaxW, disp.x - 80.0f));
  const float inner = panelW - 2 * (kFrame + kPad);
  const float thumbW = kThumbCells * kCell;
  const int perRow = std::max(1, (int)((inner + kThumbGap) / (thumbW + kThumbGap)));
  float thumbH = 0.0f;
  for (const auto& sh : r.shots) thumbH = std::max(thumbH, sh.th * kCell);
  const int rows = std::max(1, ((int)r.shots.size() + perRow - 1) / perRow);
  const float capH = 22.0f;  // the "#1  1920x1080" caption under each
  const float shotsH = rows * (thumbH + capH + kThumbGap);
  const float panelH = Snap(std::min(
      disp.y - 40.0f, kFrame + ui::kHeaderH + 14 + shotsH + 40 + kTextH + 18 + lineH + 34 + kPad + kFrame));
  const ImVec2 wp(Snap((disp.x - panelW) * 0.5f), Snap((disp.y - panelH) * 0.5f));
  const ImVec2 wb(wp.x + panelW, wp.y + panelH);

  ImGui::SetNextWindowPos(wp);
  ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
  ImGui::Begin("##bugreport", nullptr, kFlags);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ui::PanelStyle st;
  st.darkMix = 0.34f;
  st.shadow = 18.0f;
  ui::PanelBody(dl, wp, wb, st);
  ui::Draw9(dl, "panel", wp, wb);
  const float hb = ui::HeaderBar(dl, ImVec2(wp.x + kFrame, wp.y + kFrame), panelW - 2 * kFrame,
                                 "BUG REPORT", nullptr);
  {
    ImGui::PushFont(ui::FontSmall());
    const char* hint = "ctrl+enter  submit    esc  leave the text box";
    const ImVec2 hs = ImGui::CalcTextSize(hint);
    ui::ShadowText(dl, ImVec2(Snap(wb.x - kFrame - 14 - hs.x),
                              Snap(wp.y + kFrame + (ui::kHeaderH - hs.y) * 0.5f)),
                   ui::Fade(ui::ColParchDim(), 0.85f), hint);
    ImGui::PopFont();
  }

  const float cx = wp.x + kFrame + kPad;
  float y = hb + 14;

  // ---- the pictures -----------------------------------------------------------
  ImGui::PushFont(ui::FontSmall());
  for (size_t i = 0; i < r.shots.size(); i++) {
    const auto& sh = r.shots[i];
    const int col = (int)i % perRow, row = (int)i / perRow;
    const ImVec2 a(Snap(cx + col * (thumbW + kThumbGap) + 4),
                   Snap(y + row * (thumbH + capH + kThumbGap) + 4));
    Thumb(dl, a, sh);
    char cap[48];
    std::snprintf(cap, sizeof cap, "#%zu  %ux%u", i + 1, sh.w, sh.h);
    ui::ShadowText(dl, ImVec2(a.x, a.y + sh.th * kCell + 6), ui::ColParch(), cap);
    ImGui::PushID((int)i);
    if (ui::Button("##rm", ImVec2(a.x + thumbW - 26, a.y + sh.th * kCell + 4), "x"))
      r.wantRemove = (int)i;
    if (ImGui::IsItemHovered()) ui::Tip("remove this picture");
    ImGui::PopID();
  }
  ImGui::PopFont();
  y = Snap(y + shotsH + 4);

  // ---- the text ---------------------------------------------------------------
  y = ui::Subheading(dl, ImVec2(cx, y), inner, "WHAT WENT WRONG") + 6;
  ImGui::SetCursorScreenPos(ImVec2(cx, y));
  if (r.focusText) {
    ImGui::SetKeyboardFocusHere();
    r.focusText = false;
  }
  // Esc inside an ImGui text box REVERTS everything typed since it was
  // focused. That is a whole bug description gone to the key a player reaches
  // for to "get out of the box" — so Esc keeps the text and only lets go.
  const std::string before = r.text;
  const bool entered = ImGui::InputTextMultiline(
      "##bugtext", r.text.data(), r.text.capacity() + 1, ImVec2(inner, kTextH),
      ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue, ResizeCb,
      &r.text);
  if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    r.text = before;
  if (r.text.empty() && !ImGui::IsItemActive()) {
    ImGui::PushFont(ui::FontSmall());
    ui::ShadowText(dl, ImVec2(cx + 10, y + 8), ui::Fade(ui::ColParchDim(), 0.7f),
                   "what you did, what you saw, what you expected. 'add another' to pair more pictures.");
    ImGui::PopFont();
  }
  // Ctrl+Enter validates a multiline box (plain Enter is a newline).
  if (entered) r.wantSubmit = true;
  y = Snap(y + kTextH + 18);

  // ---- the buttons ------------------------------------------------------------
  float bx = cx;
  if (ui::Button("##add", ImVec2(bx, y), "add another")) r.wantAddAnother = true;
  if (ImGui::IsItemHovered())
    ui::Tip("give the game back; F12 takes the next picture for this same report");
  bx += ImGui::GetItemRectSize().x + 12;
  if (ui::Button("##submit", ImVec2(bx, y), "submit", true)) r.wantSubmit = true;
  bx += ImGui::GetItemRectSize().x + 12;
  if (ui::Button("##discard", ImVec2(bx, y), r.discardArmed ? "discard? click again" : "discard")) {
    if (r.discardArmed) r.wantDiscard = true;
    r.discardArmed = !r.discardArmed;
  }
  {
    char n[64];
    std::snprintf(n, sizeof n, "%zu picture%s", r.shots.size(), r.shots.size() == 1 ? "" : "s");
    const ImVec2 ns = ImGui::CalcTextSize(n);
    ui::ShadowText(dl, ImVec2(Snap(cx + inner - ns.x), Snap(y + 4)), ui::ColParchDim(), n);
  }
  // A failed submit keeps the panel up; say why under the buttons.
  if (!r.toast.empty() && r.toastLeft > 0.0f) {
    ImGui::PushFont(ui::FontSmall());
    ui::ShadowText(dl, ImVec2(cx, Snap(y + lineH + 14)), ui::ColBloodHi(), r.toast.c_str());
    ImGui::PopFont();
  }
  ImGui::End();
  ImGui::PopFont();
}
