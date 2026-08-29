#pragma once
#include <string>

#include <imgui.h>

// THE LOOK: one ImGui style pass plus a pixel-art chrome atlas.
//
// A UI-INTERNAL header. It names ImGui types freely — unlike ui/overlay.h,
// which is included by main.cpp and stays imgui-free on purpose. Nothing
// outside src/ui/ may include this.
//
// WHY CHROME LIVES IN THE FONT ATLAS. The engine has no sprite pipeline: the
// rhi:: seam carries no sampler, no texture bind group and no
// CopyBufferToTexture, because nothing in the engine samples a texture. ImGui
// already owns exactly one texture and a working upload path for it, and since
// 1.92 it will pack arbitrary rectangles into that texture on request
// (ImFontAtlas::AddCustomRect). So the panels, slot frames and engravings ride
// into the GPU on the font atlas's back and the UI needs no texture plumbing
// at all.
//
// THE ONE HAZARD, handled by RefreshChrome(): ImGui may resize or repack that
// texture AT ANY TIME (a new glyph size, a DPI change), which moves every
// custom rect and throws away the pixels in it. So the source pixels stay
// resident in RAM, and every frame we check whether the atlas we blitted into
// is still the atlas being drawn — re-blitting when it is not. Caching UVs
// across frames is exactly what the ImGui docs forbid, and this is why.
namespace ui {

// ---- palette ---------------------------------------------------------------
// Derived from mina's own robe materials so the frame and the character in it
// are made of the same colours. Kept as functions rather than constants
// because IM_COL32 is a macro over an expression, and a header full of static
// ImU32 objects is a static-init order question nobody should have to answer.
inline ImU32 ColInk()      { return IM_COL32(18, 16, 30, 255); }
inline ImU32 ColDeep()     { return IM_COL32(34, 30, 56, 255); }
inline ImU32 ColMid()      { return IM_COL32(48, 42, 78, 255); }
inline ImU32 ColHi()       { return IM_COL32(68, 60, 104, 255); }
inline ImU32 ColGold()     { return IM_COL32(201, 164, 74, 255); }
inline ImU32 ColGoldHi()   { return IM_COL32(240, 214, 140, 255); }
inline ImU32 ColGoldDim()  { return IM_COL32(120, 96, 42, 255); }
inline ImU32 ColParch()    { return IM_COL32(216, 205, 180, 255); }
inline ImU32 ColParchDim() { return IM_COL32(150, 140, 118, 255); }
inline ImU32 ColBlood()    { return IM_COL32(168, 46, 46, 255); }
inline ImU32 ColBloodHi()  { return IM_COL32(232, 74, 62, 255); }
inline ImU32 ColEmber()    { return IM_COL32(232, 138, 46, 255); }
inline ImU32 ColChar()     { return IM_COL32(56, 48, 48, 255); }

// The integer scale every piece of chrome is drawn at. Authored at 1x and
// blown up by a whole number with a NEAREST sampler — that pair is the entire
// reason the result stays crisp rather than turning into mush.
constexpr float kChromeScale = 2.0f;

// Apply the style + font. Call once, after ImGui::CreateContext().
void ApplyFantasyTheme();

// ---- the chrome atlas ------------------------------------------------------

struct ChromeSprite {
  bool valid = false;
  ImVec2 uv0{}, uv1{};
  ImVec2 size{};        // source size, PIXELS (multiply by kChromeScale to draw)
  // 9-slice borders in source pixels, L/T/R/B. All zero = draw whole, no slice.
  float border[4] = {0, 0, 0, 0};
  bool NineSlice() const { return border[0] > 0 || border[1] > 0; }
};

// Reads assets/ui/chrome.{bmp,json}. Failure is NOT fatal and never should be:
// a missing chrome file costs the panel its decoration, not the session — the
// screen falls back to flat ImGui rectangles. Returns false and fills `err`.
bool LoadChrome(const std::string& assetDir, std::string& err);

// Re-pack / re-blit if ImGui moved or rebuilt its texture. Call once per frame
// from Overlay::BeginFrame, AFTER ImGui::NewFrame().
void RefreshChrome();

// Null when the sprite is unknown or chrome failed to load — every caller must
// handle that, because a missing sprite is the ordinary state of a checkout
// whose assets/ui was not regenerated.
const ChromeSprite* Chrome(const char* key);

// Which texture the sprites live in. Only meaningful after RefreshChrome().
ImTextureRef ChromeTex();

// Draw a 9-slice frame filling [a,b). Falls back to a plain two-tone rectangle
// when the sprite is missing, so the layout is identical either way.
void Draw9(ImDrawList* dl, const char* key, ImVec2 a, ImVec2 b,
           ImU32 tint = IM_COL32_WHITE);

// Draw one sprite with its TOP-LEFT at `at`, at kChromeScale. Returns the
// size it occupied (zero when the sprite is missing).
ImVec2 DrawSprite(ImDrawList* dl, const char* key, ImVec2 at,
                  ImU32 tint = IM_COL32_WHITE);

// Same, centred on `c`.
void DrawSpriteCentered(ImDrawList* dl, const char* key, ImVec2 c,
                        ImU32 tint = IM_COL32_WHITE);

}  // namespace ui
