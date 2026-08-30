#include "ui/theme.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>   // ImFontAtlasTextureBlockQueueUpload
#include <nlohmann/json.hpp>

namespace ui {

static ImFont* g_fontSmall = nullptr;
static ImFont* g_fontLarge = nullptr;

ImFont* FontSmall() { return g_fontSmall; }
ImFont* FontLarge() { return g_fontLarge; }

namespace {

using nlohmann::json;

// ---- style -----------------------------------------------------------------

ImVec4 V(ImU32 c) {
  return ImVec4(((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
                ((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                ((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f,
                ((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f);
}
ImVec4 V(ImU32 c, float a) {
  ImVec4 v = V(c);
  v.w = a;
  return v;
}

// ---- the chrome atlas ------------------------------------------------------

struct Loaded {
  bool ok = false;
  int w = 0, h = 0;
  std::vector<unsigned char> rgba;   // source pixels, kept for RE-blitting
  struct Entry {
    int x = 0, y = 0, w = 0, h = 0;
    float border[4] = {0, 0, 0, 0};
    ImFontAtlasRectId rect = ImFontAtlasRectId_Invalid;
    ChromeSprite sprite;
  };
  std::map<std::string, Entry> sprites;
  // Which atlas texture the rects were last blitted into, and where they were.
  // Both have to be checked: a repack can move a rect without creating a new
  // texture, and a resize creates a new texture that has our rects at the same
  // coordinates but with different UVs.
  int blittedTexId = -1;
  bool packed = false;
};
Loaded g;

// 32-bit BMP reader, matched to what scripts/gen_ui_chrome.py writes: a
// BITMAPV4HEADER, 32 bpp, BI_BITFIELDS with BGRA masks, bottom-up.
//
// Deliberately strict. A loader that guesses at a header it does not recognise
// will happily produce garbage pixels, and garbage pixels in a UI atlas look
// like a rendering bug rather than like the malformed file they are — so
// anything unexpected is a REFUSAL with a message naming what it found.
bool ReadBmp32(const std::string& path, Loaded& out, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    err = "cannot open " + path;
    return false;
  }
  std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
  if (buf.size() < 54 || buf[0] != 'B' || buf[1] != 'M') {
    err = path + ": not a BMP";
    return false;
  }
  auto u32 = [&](size_t o) {
    return (uint32_t)buf[o] | ((uint32_t)buf[o + 1] << 8) |
           ((uint32_t)buf[o + 2] << 16) | ((uint32_t)buf[o + 3] << 24);
  };
  auto i32 = [&](size_t o) { return (int32_t)u32(o); };
  const uint32_t dataOff = u32(10);
  const int32_t w = i32(18), h = i32(22);
  const uint16_t bpp = (uint16_t)(buf[28] | (buf[29] << 8));
  if (bpp != 32) {
    err = path + ": expected 32 bpp, got " + std::to_string(bpp) +
          " (re-run scripts/gen_ui_chrome.py)";
    return false;
  }
  if (w <= 0 || h == 0) {
    err = path + ": bad dimensions";
    return false;
  }
  const bool topDown = h < 0;
  const int H = topDown ? -h : h;
  const size_t need = (size_t)dataOff + (size_t)w * H * 4;
  if (buf.size() < need) {
    err = path + ": truncated (" + std::to_string(buf.size()) + " of " +
          std::to_string(need) + " bytes)";
    return false;
  }
  out.w = w;
  out.h = H;
  out.rgba.assign((size_t)w * H * 4, 0);
  for (int y = 0; y < H; y++) {
    const int srcRow = topDown ? y : (H - 1 - y);
    const unsigned char* s = &buf[dataOff + (size_t)srcRow * w * 4];
    unsigned char* d = &out.rgba[(size_t)y * w * 4];
    for (int x = 0; x < w; x++) {   // BGRA on disk -> RGBA in memory
      d[x * 4 + 0] = s[x * 4 + 2];
      d[x * 4 + 1] = s[x * 4 + 1];
      d[x * 4 + 2] = s[x * 4 + 0];
      d[x * 4 + 3] = s[x * 4 + 3];
    }
  }
  return true;
}

// Register every sprite as a custom rect. Idempotent-ish: called once, and
// again only if a rect id ever comes back invalid.
void PackRects() {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  if (!atlas) return;
  // Colour in the atlas: tells backends this texture is not white+alpha, which
  // matters for any that would otherwise prefer an 8-bit format.
  atlas->TexPixelsUseColors = true;
  for (auto& kv : g.sprites) {
    Loaded::Entry& e = kv.second;
    if (e.rect != ImFontAtlasRectId_Invalid) continue;
    e.rect = atlas->AddCustomRect(e.w, e.h);
    if (e.rect == ImFontAtlasRectId_Invalid) {
      std::fprintf(stderr, "ui/chrome: atlas refused a %dx%d rect for '%s'\n",
                   e.w, e.h, kv.first.c_str());
    }
  }
  g.packed = true;
  g.blittedTexId = -1;   // freshly packed rects hold nobody's pixels yet
}

// Copy the source pixels into wherever the atlas put each rect, and tell ImGui
// which region changed.
void BlitRects() {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  ImTextureData* tex = atlas ? atlas->TexData : nullptr;
  if (!tex || !tex->Pixels || tex->Format != ImTextureFormat_RGBA32) return;
  for (auto& kv : g.sprites) {
    Loaded::Entry& e = kv.second;
    ImFontAtlasRect r;
    if (e.rect == ImFontAtlasRectId_Invalid ||
        !atlas->GetCustomRect(e.rect, &r))
      continue;
    for (int y = 0; y < e.h; y++) {
      const unsigned char* s = &g.rgba[((size_t)(e.y + y) * g.w + e.x) * 4];
      unsigned char* d = (unsigned char*)tex->GetPixelsAt(r.x, r.y + y);
      std::memcpy(d, s, (size_t)e.w * 4);
    }
    ImFontAtlasTextureBlockQueueUpload(atlas, tex, r.x, r.y, e.w, e.h);
    e.sprite.valid = true;
    e.sprite.uv0 = r.uv0;
    e.sprite.uv1 = r.uv1;
    e.sprite.size = ImVec2((float)e.w, (float)e.h);
    std::memcpy(e.sprite.border, e.border, sizeof(e.border));
  }
  g.blittedTexId = tex->UniqueID;
}

}  // namespace

void ApplyFantasyTheme() {
  ImGuiStyle& s = ImGui::GetStyle();
  ImGui::StyleColorsDark();

  // ZERO ROUNDING, EVERYWHERE. Rounded corners are the single loudest signal
  // that a UI is drawn by a vector library, and the whole look here is pixels
  // on a whole-number grid — one rounded rect next to a nearest-sampled sprite
  // and the two read as different programs.
  s.WindowRounding = s.ChildRounding = s.FrameRounding = 0.0f;
  s.PopupRounding = s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0.0f;
  s.WindowBorderSize = s.ChildBorderSize = s.PopupBorderSize = 1.0f;
  s.FrameBorderSize = 1.0f;
  s.WindowPadding = ImVec2(10, 10);
  s.FramePadding = ImVec2(6, 4);
  s.ItemSpacing = ImVec2(8, 6);
  s.ItemInnerSpacing = ImVec2(6, 4);
  s.ScrollbarSize = 12.0f;
  s.GrabMinSize = 10.0f;
  s.Alpha = 1.0f;   // was 0.92: a translucent dev panel is fine, a translucent
                    // character sheet reads as a bug

  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = V(ColParch());
  c[ImGuiCol_TextDisabled] = V(ColParchDim(), 0.62f);
  c[ImGuiCol_WindowBg] = V(ColDeep(), 0.97f);
  c[ImGuiCol_ChildBg] = V(ColInk(), 0.45f);
  c[ImGuiCol_PopupBg] = V(ColDeep(), 0.98f);
  c[ImGuiCol_Border] = V(ColGoldDim(), 0.80f);
  c[ImGuiCol_BorderShadow] = V(ColInk(), 0.0f);
  c[ImGuiCol_FrameBg] = V(ColInk(), 0.72f);
  c[ImGuiCol_FrameBgHovered] = V(ColMid(), 0.90f);
  c[ImGuiCol_FrameBgActive] = V(ColHi(), 0.95f);
  c[ImGuiCol_TitleBg] = V(ColInk());
  c[ImGuiCol_TitleBgActive] = V(ColMid());
  c[ImGuiCol_TitleBgCollapsed] = V(ColInk(), 0.7f);
  c[ImGuiCol_MenuBarBg] = V(ColMid());
  c[ImGuiCol_ScrollbarBg] = V(ColInk(), 0.6f);
  c[ImGuiCol_ScrollbarGrab] = V(ColMid());
  c[ImGuiCol_ScrollbarGrabHovered] = V(ColHi());
  c[ImGuiCol_ScrollbarGrabActive] = V(ColGoldDim());
  c[ImGuiCol_CheckMark] = V(ColGoldHi());
  c[ImGuiCol_SliderGrab] = V(ColGoldDim());
  c[ImGuiCol_SliderGrabActive] = V(ColGold());
  c[ImGuiCol_Button] = V(ColMid(), 0.90f);
  c[ImGuiCol_ButtonHovered] = V(ColHi());
  c[ImGuiCol_ButtonActive] = V(ColGoldDim());
  c[ImGuiCol_Header] = V(ColMid(), 0.85f);
  c[ImGuiCol_HeaderHovered] = V(ColHi());
  c[ImGuiCol_HeaderActive] = V(ColGoldDim());
  c[ImGuiCol_Separator] = V(ColGoldDim(), 0.55f);
  c[ImGuiCol_SeparatorHovered] = V(ColGold());
  c[ImGuiCol_SeparatorActive] = V(ColGoldHi());
  c[ImGuiCol_ResizeGrip] = V(ColGoldDim(), 0.35f);
  c[ImGuiCol_ResizeGripHovered] = V(ColGold(), 0.7f);
  c[ImGuiCol_ResizeGripActive] = V(ColGoldHi(), 0.9f);
  c[ImGuiCol_Tab] = V(ColInk());
  c[ImGuiCol_TabHovered] = V(ColHi());
  c[ImGuiCol_TabSelected] = V(ColMid());
  c[ImGuiCol_TabDimmed] = V(ColInk());
  c[ImGuiCol_TabDimmedSelected] = V(ColMid(), 0.8f);
  c[ImGuiCol_DragDropTarget] = V(ColGoldHi());
  c[ImGuiCol_NavCursor] = V(ColGold());
  c[ImGuiCol_ModalWindowDimBg] = V(ColInk(), 0.55f);

  // TWO FONTS, both ProggyClean (the built-in pixel font, hand-hinted at 13 px).
  // The 13 px version is the dev panel's native size — readable and compact.
  // The 26 px version (2x) sits beside kChromeScale=2 chrome in the inventory
  // screen. Both live in the same atlas; the first font added is ImGui's default.
  ImFontConfig cfgSmall;
  cfgSmall.SizePixels = 13.0f;
  cfgSmall.OversampleH = cfgSmall.OversampleV = 1;
  cfgSmall.PixelSnapH = true;
  g_fontSmall = ImGui::GetIO().Fonts->AddFontDefault(&cfgSmall);

  ImFontConfig cfgLarge;
  cfgLarge.SizePixels = 26.0f;
  cfgLarge.OversampleH = cfgLarge.OversampleV = 1;
  cfgLarge.PixelSnapH = true;
  g_fontLarge = ImGui::GetIO().Fonts->AddFontDefault(&cfgLarge);
}

bool LoadChrome(const std::string& assetDir, std::string& err) {
  g = Loaded{};
  const std::string bmp = assetDir + "/ui/chrome.bmp";
  const std::string js = assetDir + "/ui/chrome.json";
  if (!ReadBmp32(bmp, g, err)) return false;

  std::ifstream jf(js);
  if (!jf) {
    err = "cannot open " + js;
    return false;
  }
  json root;
  try {
    jf >> root;
  } catch (const std::exception& e) {
    err = js + ": " + e.what();
    return false;
  }
  if (!root.contains("sprites") || !root["sprites"].is_object()) {
    err = js + ": no \"sprites\" object";
    return false;
  }
  // The sheet size is recorded in the JSON as well as implied by the BMP, and
  // they are checked against each other: the two files are regenerated
  // together, so a mismatch means one of them is stale — which would otherwise
  // show up as sprites sampled from the wrong place.
  if (root.value("width", g.w) != g.w || root.value("height", g.h) != g.h) {
    err = js + ": size disagrees with " + bmp +
          " (re-run scripts/gen_ui_chrome.py and commit BOTH)";
    return false;
  }
  for (auto it = root["sprites"].begin(); it != root["sprites"].end(); ++it) {
    const json& v = it.value();
    Loaded::Entry e;
    e.x = v.value("x", 0);
    e.y = v.value("y", 0);
    e.w = v.value("w", 0);
    e.h = v.value("h", 0);
    if (e.w <= 0 || e.h <= 0 || e.x < 0 || e.y < 0 || e.x + e.w > g.w ||
        e.y + e.h > g.h) {
      err = js + ": sprite '" + it.key() + "' is outside the sheet";
      return false;
    }
    if (v.contains("border") && v["border"].is_array() &&
        v["border"].size() == 4) {
      for (int i = 0; i < 4; i++) e.border[i] = (float)v["border"][i];
      // A 9-slice whose borders leave no middle would stretch nothing and draw
      // the corners on top of each other. Refuse loudly rather than at 3 a.m.
      if (e.border[0] + e.border[2] >= (float)e.w ||
          e.border[1] + e.border[3] >= (float)e.h) {
        err = js + ": sprite '" + it.key() + "' has no middle slice left";
        return false;
      }
    }
    g.sprites[it.key()] = e;
  }
  g.ok = true;
  return true;
}

void RefreshChrome() {
  if (!g.ok) return;
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  if (!atlas) return;
  if (!g.packed) PackRects();
  ImTextureData* tex = atlas->TexData;
  if (!tex) return;
  // Re-blit when the texture we last wrote into is not the one being drawn.
  // A repack that MOVES a rect within the same texture is caught too, because
  // BlitRects re-reads GetCustomRect every time and the UV cache is refreshed
  // in the same pass — the sprites' UVs are never carried across a frame
  // boundary without being re-derived, which is what the ImGui docs demand.
  if (tex->UniqueID != g.blittedTexId) {
    BlitRects();
    return;
  }
  // Same texture: refresh the UVs anyway. Cheap (a map walk over ~20 entries)
  // and it is the cache-invalidation bug that does not happen.
  for (auto& kv : g.sprites) {
    ImFontAtlasRect r;
    if (kv.second.rect == ImFontAtlasRectId_Invalid ||
        !atlas->GetCustomRect(kv.second.rect, &r))
      continue;
    kv.second.sprite.uv0 = r.uv0;
    kv.second.sprite.uv1 = r.uv1;
  }
}

const ChromeSprite* Chrome(const char* key) {
  if (!g.ok || !key) return nullptr;
  auto it = g.sprites.find(key);
  if (it == g.sprites.end() || !it->second.sprite.valid) return nullptr;
  return &it->second.sprite;
}

ImTextureRef ChromeTex() { return ImGui::GetIO().Fonts->TexRef; }

void Draw9(ImDrawList* dl, const char* key, ImVec2 a, ImVec2 b, ImU32 tint) {
  const ChromeSprite* s = Chrome(key);
  if (!s || !s->NineSlice()) {
    // Fallback: the same two-tone frame in flat rectangles. Identical geometry
    // so a checkout with no assets/ui still lays out correctly — the panel
    // just looks plain rather than looking broken.
    dl->AddRectFilled(a, b, ColDeep());
    dl->AddRect(a, b, ColGoldDim());
    dl->AddRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), ColInk());
    return;
  }
  const ImTextureRef tex = ChromeTex();
  const float k = kChromeScale;
  // Border widths in SOURCE pixels and in SCREEN pixels. If the destination is
  // too small to hold both borders the frame is clamped: better a squashed
  // frame than corners drawn past each other.
  const float bl = s->border[0], bt = s->border[1];
  const float br = s->border[2], bb = s->border[3];
  float sl = bl * k, st = bt * k, sr = br * k, sb = bb * k;
  const float dw = b.x - a.x, dh = b.y - a.y;
  if (sl + sr > dw) {
    const float f = dw / (sl + sr);
    sl *= f;
    sr *= f;
  }
  if (st + sb > dh) {
    const float f = dh / (st + sb);
    st *= f;
    sb *= f;
  }
  // Source UV cuts. uvAt maps a source-pixel offset into the sprite's own UV
  // span, so this works wherever the atlas happened to pack it.
  const float su = s->uv1.x - s->uv0.x, sv = s->uv1.y - s->uv0.y;
  auto ux = [&](float px) { return s->uv0.x + su * (px / s->size.x); };
  auto uy = [&](float py) { return s->uv0.y + sv * (py / s->size.y); };
  const float xs[4] = {a.x, a.x + sl, b.x - sr, b.x};
  const float ys[4] = {a.y, a.y + st, b.y - sb, b.y};
  const float us[4] = {ux(0), ux(bl), ux(s->size.x - br), ux(s->size.x)};
  const float vs[4] = {uy(0), uy(bt), uy(s->size.y - bb), uy(s->size.y)};
  for (int j = 0; j < 3; j++)
    for (int i = 0; i < 3; i++) {
      if (xs[i + 1] <= xs[i] || ys[j + 1] <= ys[j]) continue;
      dl->AddImage(tex, ImVec2(xs[i], ys[j]), ImVec2(xs[i + 1], ys[j + 1]),
                   ImVec2(us[i], vs[j]), ImVec2(us[i + 1], vs[j + 1]), tint);
    }
}

ImVec2 DrawSprite(ImDrawList* dl, const char* key, ImVec2 at, ImU32 tint) {
  const ChromeSprite* s = Chrome(key);
  if (!s) return ImVec2(0, 0);
  const ImVec2 sz(s->size.x * kChromeScale, s->size.y * kChromeScale);
  dl->AddImage(ChromeTex(), at, ImVec2(at.x + sz.x, at.y + sz.y), s->uv0,
               s->uv1, tint);
  return sz;
}

void DrawSpriteCentered(ImDrawList* dl, const char* key, ImVec2 c, ImU32 tint) {
  const ChromeSprite* s = Chrome(key);
  if (!s) return;
  // Snapped to whole pixels: a sprite drawn at a half-pixel offset through a
  // nearest sampler drops or doubles a row, which on a 16 px engraving is
  // immediately visible as a wobble.
  const ImVec2 sz(s->size.x * kChromeScale, s->size.y * kChromeScale);
  DrawSprite(dl, key,
             ImVec2((float)(int)(c.x - sz.x * 0.5f),
                    (float)(int)(c.y - sz.y * 0.5f)),
             tint);
}

}  // namespace ui
