#include "ui/theme.h"

#include <algorithm>
#include <cmath>
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

ImU32 Fade(ImU32 c, float a) {
  const ImU32 keep = c & ~IM_COL32_A_MASK;
  const int alpha = (int)std::lround(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
  return keep | ((ImU32)std::clamp(alpha, 0, 255) << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  auto ch = [&](int shift) {
    const float va = (float)((a >> shift) & 0xFF);
    const float vb = (float)((b >> shift) & 0xFF);
    return (ImU32)std::clamp((int)std::lround(va + (vb - va) * t), 0, 255)
           << shift;
  };
  return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) |
         ch(IM_COL32_A_SHIFT);
}

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

// The grain tile, authored at 1x like everything else and drawn at 2x.
constexpr int kGrainN = 64;
constexpr const char* kGrainKey = "_grain";

struct Loaded {
  bool ok = false;
  int w = 0, h = 0;
  std::vector<unsigned char> rgba;   // source pixels, kept for RE-blitting
  struct Entry {
    int x = 0, y = 0, w = 0, h = 0;
    float border[4] = {0, 0, 0, 0};
    // Pixels of this entry's own, for sprites that are GENERATED rather than
    // read from the sheet (the grain). Empty = read from `rgba` at x,y.
    std::vector<unsigned char> own;
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

// The grain: seeded, so every launch has the same speckle and a screenshot
// diff is a diff of the code. Two octaves — a fine white speckle and a softer
// 4-px mottle under it — so it reads as brushed metal rather than as static.
// Greyscale about mid-grey: drawn with alpha `a` over a dark panel it lifts
// the panel by roughly a*(128-panel), which PanelBody's base colour allows
// for.
void EnsureGrain() {
  if (g.sprites.count(kGrainKey)) return;
  Loaded::Entry e;
  e.w = e.h = kGrainN;
  e.own.assign((size_t)kGrainN * kGrainN * 4, 255);
  uint32_t s = 0x9E3779B9u;
  auto rnd = [&]() {   // xorshift32; quality is irrelevant, determinism is not
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (float)(s & 0xFFFF) / 65535.0f;
  };
  // Coarse octave on a 4-px lattice, so the grain is not pure white noise.
  constexpr int kC = kGrainN / 4;
  float coarse[kC * kC];
  for (float& v : coarse) v = rnd() - 0.5f;
  for (int y = 0; y < kGrainN; y++)
    for (int x = 0; x < kGrainN; x++) {
      const float fine = rnd() - 0.5f;
      const float mott = coarse[(y / 4) * kC + (x / 4)];
      const int v = std::clamp((int)std::lround(128 + fine * 96 + mott * 40),
                               0, 255);
      unsigned char* d = &e.own[((size_t)y * kGrainN + x) * 4];
      d[0] = d[1] = d[2] = (unsigned char)v;
      d[3] = 255;
    }
  g.sprites[kGrainKey] = std::move(e);
  g.packed = false;   // a new rect to pack
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
      const unsigned char* s =
          e.own.empty() ? &g.rgba[((size_t)(e.y + y) * g.w + e.x) * 4]
                        : &e.own[(size_t)y * e.w * 4];
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

// ---- pixel helpers ---------------------------------------------------------

float Snap(float v) { return std::floor(v); }

// A hollow ring of filled strips between expansions e0 and e1 OUTSIDE [a,b).
// Filled rects rather than AddRect strokes: a stroke is anti-aliased and
// centred on its coordinate, and neither of those is a pixel.
void RingOut(ImDrawList* dl, ImVec2 a, ImVec2 b, float e0, float e1,
             ImU32 col) {
  if ((col & IM_COL32_A_MASK) == 0) return;
  dl->AddRectFilled(ImVec2(a.x - e1, a.y - e1), ImVec2(b.x + e1, a.y - e0), col);
  dl->AddRectFilled(ImVec2(a.x - e1, b.y + e0), ImVec2(b.x + e1, b.y + e1), col);
  dl->AddRectFilled(ImVec2(a.x - e1, a.y - e0), ImVec2(a.x - e0, b.y + e0), col);
  dl->AddRectFilled(ImVec2(b.x + e0, a.y - e0), ImVec2(b.x + e1, b.y + e0), col);
}

// The same ring INSIDE [a,b), between insets e0 and e1.
void RingIn(ImDrawList* dl, ImVec2 a, ImVec2 b, float e0, float e1, ImU32 col) {
  if ((col & IM_COL32_A_MASK) == 0) return;
  if (b.x - a.x < 2 * e1 || b.y - a.y < 2 * e1) return;
  dl->AddRectFilled(ImVec2(a.x + e0, a.y + e0), ImVec2(b.x - e0, a.y + e1), col);
  dl->AddRectFilled(ImVec2(a.x + e0, b.y - e1), ImVec2(b.x - e0, b.y - e0), col);
  dl->AddRectFilled(ImVec2(a.x + e0, a.y + e1), ImVec2(a.x + e1, b.y - e1), col);
  dl->AddRectFilled(ImVec2(b.x - e1, a.y + e1), ImVec2(b.x - e0, b.y - e1), col);
}

// A crisp outline of width w just inside [a,b).
void Outline(ImDrawList* dl, ImVec2 a, ImVec2 b, float w, ImU32 col) {
  RingIn(dl, a, b, 0.0f, w, col);
}

// Alpha quantisation for the banded gradients: the bands are 4 px, but a
// 1000 px wash in 4 px bands is 250 bands that differ by a fraction of a
// unit — smooth, which is the wrong look. Posterising to a fixed number of
// LEVELS is what makes the steps readable as shading.
constexpr int kLevels = 10;

float Quant(float t, int levels) {
  const int i = std::clamp((int)(t * levels), 0, levels - 1);
  return ((float)i + 0.5f) / (float)levels;
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
  c[ImGuiCol_TextDisabled] = V(ColParchDim(), 0.72f);
  c[ImGuiCol_WindowBg] = V(ColDeep(), 0.97f);
  c[ImGuiCol_ChildBg] = V(ColInk(), 0.45f);
  c[ImGuiCol_PopupBg] = V(ColDeep(), 0.98f);
  c[ImGuiCol_Border] = V(ColBronze(), 0.95f);
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
  c[ImGuiCol_Separator] = V(ColBronze(), 0.7f);
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
    g.sprites[it.key()] = std::move(e);
  }
  g.ok = true;
  return true;
}

void RefreshChrome() {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  if (!atlas) return;
  EnsureGrain();   // independent of the chrome file: code-generated
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
  if (!key) return nullptr;
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
    Outline(dl, a, b, 2.0f, ColInk());
    RingIn(dl, a, b, 2.0f, 4.0f, ColBronze());
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
  // Snapped to whole pixels: a sprite drawn at a half-pixel offset drops or
  // doubles a row, which on a 16 px engraving is immediately visible as a
  // wobble.
  const ImVec2 sz(s->size.x * kChromeScale, s->size.y * kChromeScale);
  DrawSprite(dl, key,
             ImVec2((float)(int)(c.x - sz.x * 0.5f),
                    (float)(int)(c.y - sz.y * 0.5f)),
             tint);
}

// ---- surfaces --------------------------------------------------------------

void GradientV(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom) {
  const float h = b.y - a.y;
  if (h <= 0 || b.x <= a.x) return;
  const int n = std::max(1, (int)std::ceil(h / kBand));
  const int levels = std::min(kLevels, n);
  for (int i = 0; i < n; i++) {
    const float y0 = a.y + i * kBand;
    const float y1 = std::min(b.y, y0 + kBand);
    const ImU32 col = Mix(top, bottom, Quant(((float)i + 0.5f) / n, levels));
    if ((col & IM_COL32_A_MASK) == 0) continue;
    dl->AddRectFilled(ImVec2(a.x, y0), ImVec2(b.x, y1), col);
  }
}

void GradientH(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 left, ImU32 right) {
  const float w = b.x - a.x;
  if (w <= 0 || b.y <= a.y) return;
  const int n = std::max(1, (int)std::ceil(w / kBand));
  const int levels = std::min(kLevels, n);
  for (int i = 0; i < n; i++) {
    const float x0 = a.x + i * kBand;
    const float x1 = std::min(b.x, x0 + kBand);
    const ImU32 col = Mix(left, right, Quant(((float)i + 0.5f) / n, levels));
    if ((col & IM_COL32_A_MASK) == 0) continue;
    dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1, b.y), col);
  }
}

void Grain(ImDrawList* dl, ImVec2 a, ImVec2 b, float alpha) {
  const ChromeSprite* s = Chrome(kGrainKey);
  if (!s || alpha <= 0.0f || b.x <= a.x || b.y <= a.y) return;
  const float tile = s->size.x * kChromeScale;
  const ImU32 tint = Fade(IM_COL32_WHITE, alpha);
  // Tiles are anchored to the SCREEN grid, not the panel, so two panels that
  // touch share one continuous grain and a panel that moves does not carry
  // its speckle with it like a decal.
  const float x0 = std::floor(a.x / tile) * tile;
  const float y0 = std::floor(a.y / tile) * tile;
  dl->PushClipRect(a, b, true);
  for (float y = y0; y < b.y; y += tile)
    for (float x = x0; x < b.x; x += tile)
      dl->AddImage(ChromeTex(), ImVec2(x, y), ImVec2(x + tile, y + tile),
                   s->uv0, s->uv1, tint);
  dl->PopClipRect();
}

void DropShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float spread, float alpha) {
  if (spread <= 0 || alpha <= 0) return;
  // Offset down-right: light comes from the top-left everywhere in the chrome
  // (the bevels say so), and a shadow that disagrees looks pasted on.
  const ImVec2 sa(a.x + 2, a.y + 3), sb(b.x + 2, b.y + 3);
  const int n = std::max(1, (int)(spread / 2.0f));
  for (int i = 0; i < n; i++) {
    const float t = (float)i / (float)n;
    const float f = (1.0f - t) * (1.0f - t);   // quadratic falloff
    RingOut(dl, sa, sb, i * 2.0f, i * 2.0f + 2.0f, Fade(ColInk(), alpha * f));
  }
}

void InnerShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float spread, float alpha) {
  if (spread <= 0 || alpha <= 0) return;
  const int n = std::max(1, (int)(spread / 2.0f));
  for (int i = 0; i < n; i++) {
    const float t = (float)i / (float)n;
    const float f = (1.0f - t) * (1.0f - t);
    RingIn(dl, a, b, i * 2.0f, i * 2.0f + 2.0f, Fade(ColInk(), alpha * f));
  }
}

void Glow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float spread,
          float alpha) {
  if (spread <= 0 || alpha <= 0) return;
  const int n = std::max(1, (int)(spread / 2.0f));
  for (int i = 0; i < n; i++) {
    const float t = (float)i / (float)n;
    const float f = (1.0f - t) * (1.0f - t);
    RingOut(dl, a, b, i * 2.0f, i * 2.0f + 2.0f, Fade(col, alpha * f));
  }
}

void PanelBody(ImDrawList* dl, ImVec2 a, ImVec2 b, const PanelStyle& st) {
  if (st.shadow > 0) DropShadow(dl, a, b, st.shadow, 0.6f);
  // Base: between obsidian and the panel field. Slightly under the target,
  // because the grain lifts it back (see EnsureGrain).
  dl->AddRectFilled(a, b, Mix(ColInk(), ColDeep(), st.darkMix));
  // Light from above: the whole face falls off from lit at the top to shaded
  // at the bottom. This is the one gradient that reads at arm's length; the
  // wash and the sheen below are what you see up close.
  GradientV(dl, a, b, Fade(ColHi(), 0.22f), Fade(ColInk(), 0.40f));
  // Bronze warmth falling from the top edge. Capped in height: past ~200 px
  // the wash is invisible anyway and the bands would only cost draw calls.
  const float washH = std::min(b.y - a.y, 220.0f);
  GradientV(dl, a, ImVec2(b.x, a.y + washH), Fade(ColBronze(), st.bronzeAlpha),
            Fade(ColBronze(), 0.0f));
  // Lateral sheen: rises to sheenPeak then falls away.
  const float peakX = Snap(a.x + (b.x - a.x) * st.sheenPeak);
  GradientH(dl, a, ImVec2(peakX, b.y), Fade(IM_COL32_WHITE, 0.0f),
            Fade(IM_COL32_WHITE, st.sheenAlpha));
  GradientH(dl, ImVec2(peakX, a.y), b, Fade(IM_COL32_WHITE, st.sheenAlpha),
            Fade(IM_COL32_WHITE, 0.0f));
  // Top ridge: two 2 px rows of brightened bronze — the specular edge.
  dl->AddRectFilled(a, ImVec2(b.x, a.y + 2), Fade(ColBronze(), st.bronzeAlpha * 2.5f));
  dl->AddRectFilled(ImVec2(a.x, a.y + 2), ImVec2(b.x, a.y + 4),
                    Fade(ColBronze(), st.bronzeAlpha * 1.2f));
  // Bottom inset: a dark 2 px line.
  dl->AddRectFilled(ImVec2(a.x, b.y - 2), b, Fade(ColInk(), 0.5f));
  Grain(dl, a, b, st.grainAlpha);
}

float HeaderBar(ImDrawList* dl, ImVec2 a, float width, const char* title,
                const char* right) {
  const ImVec2 b(a.x + width, a.y + kHeaderH);
  // Vertical gloss: deeply shaded top through the panel field, with the last
  // bit lifting to raised metal so the bar reads as a bar and not a hole.
  const float knee = Snap(a.y + kHeaderH * 0.8f);
  GradientV(dl, a, ImVec2(b.x, knee), IM_COL32(3, 2, 6, 255), ColDeep());
  GradientV(dl, ImVec2(a.x, knee), b, ColDeep(), ColMid());
  // Lateral sheen in papyrus: peaks at 30%, most of it gone by half way.
  const float p1 = Snap(a.x + width * 0.30f), p2 = Snap(a.x + width * 0.55f);
  GradientH(dl, a, ImVec2(p1, b.y), Fade(ColParch(), 0.0f), Fade(ColParch(), 0.18f));
  GradientH(dl, ImVec2(p1, a.y), ImVec2(p2, b.y), Fade(ColParch(), 0.18f),
            Fade(ColParch(), 0.05f));
  GradientH(dl, ImVec2(p2, a.y), b, Fade(ColParch(), 0.05f), Fade(ColParch(), 0.0f));
  Grain(dl, a, b, 0.05f);
  // Dark top line, gold underline: the two edges that make it chrome.
  dl->AddRectFilled(a, ImVec2(b.x, a.y + 2), Fade(IM_COL32_BLACK, 0.4f));
  dl->AddRectFilled(ImVec2(a.x, b.y - 2), b, Fade(ColGold(), 0.8f));
  dl->AddRectFilled(ImVec2(a.x, b.y - 4), ImVec2(b.x, b.y - 2),
                    Fade(ColGoldDim(), 0.35f));
  // Accent tick.
  dl->AddRectFilled(ImVec2(a.x + 8, a.y + 8), ImVec2(a.x + 12, b.y - 8),
                    ColGoldHi());
  dl->AddRectFilled(ImVec2(a.x + 12, a.y + 8), ImVec2(a.x + 14, b.y - 8),
                    Fade(ColGoldDim(), 0.8f));
  // Title: pale gold with a soft glow under it and an ink shadow under that.
  const float th = ImGui::GetTextLineHeight();
  const ImVec2 tp(a.x + 24, Snap(a.y + (kHeaderH - th) * 0.5f));
  constexpr float kTrack = 2.0f;
  TrackedText(dl, ImVec2(tp.x + 2, tp.y + 2), Fade(ColInk(), 0.9f), title, kTrack);
  TrackedText(dl, ImVec2(tp.x, tp.y + 2), Fade(ColGoldPale(), 0.22f), title, kTrack);
  TrackedText(dl, ImVec2(tp.x + 1, tp.y), Fade(ColGoldPale(), 0.22f), title, kTrack);
  TrackedText(dl, tp, ColGoldPale(), title, kTrack);
  if (right && *right) {
    const ImVec2 ts = ImGui::CalcTextSize(right);
    ShadowText(dl, ImVec2(b.x - 12 - ts.x, tp.y), Fade(ColParchDim(), 0.75f),
               right);
  }
  return b.y;
}

float Subheading(ImDrawList* dl, ImVec2 at, float width, const char* text) {
  constexpr float kTrack = 2.0f;
  const float th = ImGui::GetTextLineHeight();
  TrackedText(dl, ImVec2(at.x + 1, at.y + 1), Fade(ColInk(), 0.8f), text, kTrack);
  TrackedText(dl, at, ColParchDim(), text, kTrack);
  const float tw = TrackedTextWidth(text, kTrack);
  const float ry = Snap(at.y + th * 0.5f);
  if (at.x + tw + 12 < at.x + width) {
    dl->AddRectFilled(ImVec2(at.x + tw + 12, ry), ImVec2(at.x + width, ry + 2),
                      Fade(ColBronze(), 0.6f));
  }
  return at.y + th + 6;
}

void ShadowText(ImDrawList* dl, ImVec2 at, ImU32 col, const char* text) {
  dl->AddText(ImVec2(at.x + 1, at.y + 1), Fade(ColInk(), 0.9f), text);
  dl->AddText(at, col, text);
}

void TrackedText(ImDrawList* dl, ImVec2 at, ImU32 col, const char* text,
                 float tracking) {
  float x = at.x;
  for (const char* p = text; *p; p++) {
    dl->AddText(ImVec2(x, at.y), col, p, p + 1);
    x += ImGui::CalcTextSize(p, p + 1).x + tracking;
  }
}

float TrackedTextWidth(const char* text, float tracking) {
  float w = 0;
  for (const char* p = text; *p; p++)
    w += ImGui::CalcTextSize(p, p + 1).x + tracking;
  return w > 0 ? w - tracking : 0;
}

void SlotSurface(ImDrawList* dl, ImVec2 a, float size, SlotLook look,
                 bool selected) {
  const ImVec2 b(a.x + size, a.y + size);
  // The state, said with light around the slot BEFORE the slot is drawn, so
  // the glow sits under the frame's hard edge rather than on top of it.
  if (selected) {
    Glow(dl, a, b, ColGoldHi(), 8.0f, 0.45f);
    RingOut(dl, a, b, 0.0f, 2.0f, ColGoldHi());
  } else if (look == SlotLook::Hover) {
    Glow(dl, a, b, ColGold(), 8.0f, 0.35f);
  } else if (look == SlotLook::Refuse) {
    Glow(dl, a, b, ColBloodHi(), 8.0f, 0.45f);
  } else if (look == SlotLook::Accept) {
    // PULSED, not static. This one has to be found by an eye that is following
    // a cursor across the screen, and a steady wash at this size loses to the
    // motion; a breathing one does not. One wall-clock phase for every slot, so
    // a piece that fits two of them reads as one invitation rather than two.
    const float p = 0.5f + 0.5f * (float)std::sin(ImGui::GetTime() * 6.0);
    Glow(dl, a, b, ColGoldHi(), 10.0f, 0.28f + 0.34f * p);
    RingOut(dl, a, b, 0.0f, 2.0f, ColGoldHi());
  }
  // The recess. Dark at the top where the rim shades it, a little lit at the
  // bottom; filled slots get a faint gold wash so "something is here" reads
  // before the icon does.
  const ImU32 base = look == SlotLook::Refuse ? IM_COL32(44, 18, 24, 255)
                                              : ColInk();
  dl->AddRectFilled(a, b, base);
  const ImVec2 fa(a.x + 2, a.y + 2), fb(b.x - 2, b.y - 2);
  GradientV(dl, fa, fb, Fade(ColInk(), 0.0f), Fade(ColHi(), 0.28f));
  if (look == SlotLook::Filled || look == SlotLook::Hover ||
      look == SlotLook::Accept)
    dl->AddRectFilled(fa, fb, Fade(ColGold(), look == SlotLook::Filled ? 0.06f
                                                                       : 0.10f));
  Grain(dl, fa, fb, 0.07f);
  InnerShadow(dl, fa, fb, 6.0f, 0.55f);
}

void ValueBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, ImU32 fill,
              bool hero) {
  frac = std::clamp(frac, 0.0f, 1.0f);
  if (hero) Glow(dl, a, b, fill, 6.0f, 0.16f);
  dl->AddRectFilled(a, b, ColInk());
  const ImVec2 ia(a.x + 2, a.y + 2), ib(b.x - 2, b.y - 2);
  if (frac > 0.0f) {
    const float fx = Snap(ia.x + (ib.x - ia.x) * frac);
    if (fx > ia.x) {
      GradientH(dl, ia, ImVec2(fx, ib.y), Mix(fill, ColInk(), 0.50f),
                Mix(fill, ColInk(), 0.15f));
      // Lit top edge and a bright cap at the leading end.
      dl->AddRectFilled(ia, ImVec2(fx, ia.y + 2), Fade(IM_COL32_WHITE, 0.12f));
      dl->AddRectFilled(ImVec2(std::max(ia.x, fx - 2), ia.y), ImVec2(fx, ib.y),
                        Mix(fill, IM_COL32_WHITE, 0.25f));
    }
  }
  Grain(dl, ia, ib, 0.06f);
  InnerShadow(dl, a, b, 4.0f, 0.5f);
  Outline(dl, a, b, 1.0f, Fade(ColBronze(), 0.9f));
}

// Both badges are set in the 13 px pixel font — the same face as the rest of
// the screen at 1x. A 26 px digit in the corner of a 44 px slot sits on top of
// the engraving; a 13 px one sits beside it.
void KeyBadge(ImDrawList* dl, ImVec2 at, const char* key) {
  ImGui::PushFont(FontSmall());
  const ImVec2 ts = ImGui::CalcTextSize(key);
  const ImVec2 b(at.x + ts.x + 5, at.y + ts.y + 1);
  dl->AddRectFilled(at, b, Fade(ColInk(), 0.88f));
  dl->AddRectFilled(ImVec2(at.x, b.y - 1), b, Fade(ColGoldDim(), 0.8f));
  ShadowText(dl, ImVec2(at.x + 3, at.y), ColGoldHi(), key);
  ImGui::PopFont();
}

void CountBadge(ImDrawList* dl, ImVec2 slotBr, const char* text) {
  ImGui::PushFont(FontSmall());
  const ImVec2 ts = ImGui::CalcTextSize(text);
  const ImVec2 a(slotBr.x - ts.x - 5, slotBr.y - ts.y - 1);
  dl->AddRectFilled(a, slotBr, Fade(ColInk(), 0.88f));
  ShadowText(dl, ImVec2(a.x + 3, a.y), ColParch(), text);
  ImGui::PopFont();
}

void ScreenDim(ImDrawList* dl, ImVec2 disp, float dim, float vignette) {
  dl->AddRectFilled(ImVec2(0, 0), disp, Fade(ColInk(), dim));
  if (vignette <= 0) return;
  // Four edge gradients. Where two overlap (the corners) they stack, which is
  // what a vignette does anyway.
  const float ex = Snap(disp.x * 0.30f), ey = Snap(disp.y * 0.30f);
  const ImU32 c0 = Fade(ColInk(), vignette), c1 = Fade(ColInk(), 0.0f);
  GradientV(dl, ImVec2(0, 0), ImVec2(disp.x, ey), c0, c1);
  GradientV(dl, ImVec2(0, disp.y - ey), disp, c1, c0);
  GradientH(dl, ImVec2(0, 0), ImVec2(ex, disp.y), c0, c1);
  GradientH(dl, ImVec2(disp.x - ex, 0), disp, c1, c0);
}

bool Button(const char* id, ImVec2 at, const char* label, bool toggled,
            float minWidth) {
  const ImVec2 ts = ImGui::CalcTextSize(label);
  const ImVec2 size(std::max(minWidth, ts.x + 24), ts.y + 8);
  ImGui::SetCursorScreenPos(at);
  const bool clicked = ImGui::InvisibleButton(id, size);
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 b(at.x + size.x, at.y + size.y);
  if (hovered) Glow(dl, at, b, toggled ? ColGoldHi() : ColGold(), 6.0f, 0.3f);
  DropShadow(dl, at, b, 4.0f, 0.5f);
  dl->AddRectFilled(at, b, Mix(ColDeep(), ColMid(), held ? 1.0f : hovered ? 0.6f : 0.25f));
  GradientV(dl, ImVec2(at.x + 1, at.y + 1), ImVec2(b.x - 1, Snap(at.y + size.y * 0.5f)),
            Fade(ColHi(), held ? 0.15f : 0.35f), Fade(ColHi(), 0.0f));
  Grain(dl, at, b, 0.06f);
  dl->AddRectFilled(ImVec2(at.x + 1, b.y - 2), ImVec2(b.x - 1, b.y - 1),
                    Fade(ColInk(), 0.6f));
  Outline(dl, at, b, 1.0f, toggled ? ColGold() : ColBronze());
  const ImVec2 tp(Snap(at.x + (size.x - ts.x) * 0.5f), Snap(at.y + (size.y - ts.y) * 0.5f));
  ShadowText(dl, tp, toggled || hovered ? ColGoldHi() : ColParch(), label);
  return clicked;
}

}  // namespace ui
