#pragma once
#include <string>

#include <imgui.h>

// THE LOOK: one ImGui style pass, a pixel-art chrome atlas, and a toolkit of
// lit surfaces to put the chrome on.
//
// A UI-INTERNAL header. It names ImGui types freely — unlike ui/overlay.h,
// which is included by main.cpp and stays imgui-free on purpose. Nothing
// outside src/ui/ may include this.
//
// WHERE THE LOOK COMES FROM (2026-09-02). The chrome is still pixel art —
// 1x-authored 9-slice frames, slot bevels and engravings drawn at an integer
// 2x, a hand-hinted pixel font, zero rounding anywhere — but what sits UNDER
// it is borrowed from the xyzpan plugin editor
// (programming/xyzpan/ui/AlchemyLookAndFeel.cpp, plugin/PluginEditor.cpp
// paint()): panels that are LIT rather than flat. A bronze wash falling from
// the top edge, a lateral specular sheen, a bright ridge along the top, a dark
// inset along the bottom, a seeded-noise grain tiled at a few percent, chrome
// header bars closed by a gold underline, drop shadows to lift the panels off
// the world, and hover said with a glow instead of a colour swap.
//
// HOW THAT STAYS PIXEL ART. Every one of those layers is QUANTISED: gradients
// are drawn as 4 px bands (kBand) with posterised alpha, shadows and glows are
// 2 px stepped rings, the grain tile is authored at 1x and drawn at 2x, and
// nothing has an anti-aliased curve. A smooth gradient next to a nearest-
// sampled sprite reads as two different programs; a banded one reads as the
// same artist shading with a bigger brush.
//
// WHY CHROME LIVES IN THE FONT ATLAS. The engine has no sprite pipeline: the
// rhi:: seam carries no sampler, no texture bind group and no
// CopyBufferToTexture, because nothing in the engine samples a texture. ImGui
// already owns exactly one texture and a working upload path for it, and since
// 1.92 it will pack arbitrary rectangles into that texture on request
// (ImFontAtlas::AddCustomRect). So the frames, engravings and the grain tile
// ride into the GPU on the font atlas's back and the UI needs no texture
// plumbing at all.
//
// THE ONE HAZARD, handled by RefreshChrome(): ImGui may resize or repack that
// texture AT ANY TIME (a new glyph size, a DPI change), which moves every
// custom rect and throws away the pixels in it. So the source pixels stay
// resident in RAM, and every frame we check whether the atlas we blitted into
// is still the atlas being drawn — re-blitting when it is not. Caching UVs
// across frames is exactly what the ImGui docs forbid, and this is why.
namespace ui {

// ---- palette ---------------------------------------------------------------
// Obsidian, gold leaf and aged papyrus — xyzpan's "Alchemy Gold" family — with
// the dark end pulled toward indigo so the frame still belongs to the
// character in it (mina's robe is deep indigo with gold trim). The same values
// are in scripts/gen_ui_chrome.py; change both. Kept as functions rather than
// constants because IM_COL32 is a macro over an expression, and a header full
// of static ImU32 objects is a static-init order question nobody should have
// to answer.
inline ImU32 ColInk()      { return IM_COL32(9, 8, 14, 255); }      // obsidian
inline ImU32 ColDeep()     { return IM_COL32(26, 22, 36, 255); }    // panel field
inline ImU32 ColMid()      { return IM_COL32(44, 38, 60, 255); }    // raised metal
inline ImU32 ColHi()       { return IM_COL32(60, 52, 82, 255); }    // lit bevel
inline ImU32 ColBronze()   { return IM_COL32(85, 74, 55, 255); }    // borders, rules
inline ImU32 ColGold()     { return IM_COL32(201, 168, 76, 255); }  // gold leaf
inline ImU32 ColGoldHi()   { return IM_COL32(217, 190, 110, 255); }
inline ImU32 ColGoldPale() { return IM_COL32(232, 212, 154, 255); } // hero titles
inline ImU32 ColGoldDim()  { return IM_COL32(166, 139, 58, 255); }
inline ImU32 ColParch()    { return IM_COL32(200, 184, 138, 255); } // body text
inline ImU32 ColParchDim() { return IM_COL32(168, 154, 112, 255); }
inline ImU32 ColBlood()    { return IM_COL32(139, 58, 58, 255); }   // cinnabar
inline ImU32 ColBloodHi()  { return IM_COL32(196, 92, 84, 255); }
inline ImU32 ColEmber()    { return IM_COL32(232, 138, 46, 255); }
inline ImU32 ColChar()     { return IM_COL32(56, 48, 48, 255); }
inline ImU32 ColMana()     { return IM_COL32(76, 132, 200, 255); }
inline ImU32 ColSteel()    { return IM_COL32(120, 132, 160, 255); }

// ---- THE LEAF (2026-09-22) -------------------------------------------------
// The spell page is not chrome: it is a sheet of VELLUM laid in the book, and
// the diagram on it is drawn the way a 12th-century quadrivium manuscript
// draws one - iron-gall brown for the figure, a rubricator's vermilion for
// what matters, and the earth pigments an illuminator actually had for the
// rest. Everything else on the character screen stays obsidian and gold: the
// book is the binding, this is the page inside it, and the contrast between
// them is the point.
inline ImU32 ColVellum()    { return IM_COL32(197, 179, 145, 255); }  // the leaf
inline ImU32 ColVellumHi()  { return IM_COL32(216, 201, 170, 255); }  // lit
inline ImU32 ColVellumLo()  { return IM_COL32(163, 145, 113, 255); }  // rubbed
inline ImU32 ColIronGall()  { return IM_COL32(46, 35, 26, 255); }     // the ink
inline ImU32 ColIronSoft()  { return IM_COL32(96, 78, 56, 255); }     // its bleed
inline ImU32 ColRubric()    { return IM_COL32(174, 66, 32, 255); }    // vermilion
inline ImU32 ColRubricHi()  { return IM_COL32(208, 104, 48, 255); }   // minium
inline ImU32 ColVerdigris() { return IM_COL32(62, 100, 74, 255); }    // matter
inline ImU32 ColAzurite()   { return IM_COL32(44, 74, 126, 255); }    // delivery
inline ImU32 ColOrpiment()  { return IM_COL32(140, 104, 36, 255); }   // mod


// Colour arithmetic every panel needs. `Fade` scales the alpha; `Mix` lerps
// all four channels.
ImU32 Fade(ImU32 c, float a);
ImU32 Mix(ImU32 a, ImU32 b, float t);

// The integer scale every piece of chrome is drawn at. Authored at 1x and
// blown up by a whole number — that is the entire reason the result stays
// crisp rather than turning into mush.
constexpr float kChromeScale = 2.0f;
// The band every gradient is quantised to, in screen pixels. Two source
// pixels: coarse enough to read as steps, fine enough not to read as stripes.
constexpr float kBand = 4.0f;

// Apply the style + font. Call once, after ImGui::CreateContext().
void ApplyFantasyTheme();

// Two fonts: the 26 px chrome font for the inventory screen and HUD, and the
// native 13 px ProggyClean for the dev panel. Both are in the same atlas.
ImFont* FontSmall();   // 13 px — dev panel, fluid tuning window
ImFont* FontLarge();   // 26 px — inventory screen, HUD bars

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
// screen falls back to flat rectangles. Returns false and fills `err`.
bool LoadChrome(const std::string& assetDir, std::string& err);

// Re-pack / re-blit if ImGui moved or rebuilt its texture. Call once per frame
// from Overlay::BeginFrame, AFTER ImGui::NewFrame(). Also owns the grain tile,
// which is generated in code and needs no asset file — so it works on a
// checkout with no assets/ui at all.
void RefreshChrome();

// Null when the sprite is unknown or chrome failed to load — every caller must
// handle that, because a missing sprite is the ordinary state of a checkout
// whose assets/ui was not regenerated.
const ChromeSprite* Chrome(const char* key);

// Which texture the sprites live in. Only meaningful after RefreshChrome().
ImTextureRef ChromeTex();

// Draw a 9-slice frame filling [a,b). The frame sprites are HOLLOW (their
// middle slice is transparent), so this draws a border and nothing else;
// the body underneath is PanelBody's job. Falls back to a plain two-tone
// outline when the sprite is missing, so the layout is identical either way.
void Draw9(ImDrawList* dl, const char* key, ImVec2 a, ImVec2 b,
           ImU32 tint = IM_COL32_WHITE);

// Draw one sprite with its TOP-LEFT at `at`, at kChromeScale. Returns the
// size it occupied (zero when the sprite is missing).
ImVec2 DrawSprite(ImDrawList* dl, const char* key, ImVec2 at,
                  ImU32 tint = IM_COL32_WHITE);

// Same, centred on `c`.
void DrawSpriteCentered(ImDrawList* dl, const char* key, ImVec2 c,
                        ImU32 tint = IM_COL32_WHITE);

// Same, at a whole multiple of kChromeScale picked from `mul` (0.5 -> the
// sprite's authored 1x, 1.0 -> 2x, ...). What a surface drawn at a rung other
// than 1x needs so its engravings shrink with it.
void DrawSpriteCenteredAt(ImDrawList* dl, const char* key, ImVec2 c, float mul,
                          ImU32 tint = IM_COL32_WHITE);

// ---- surfaces --------------------------------------------------------------
//
// The drawing vocabulary the character screen is written in. Every function
// takes an explicit draw list so the same surface can go on a window, the
// background or the foreground list. All of them are quantised to kBand /
// 2 px steps — see the header comment.

// Banded axis-aligned gradients. Alpha and colour are lerped per band.
void GradientV(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom);
void GradientH(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 left, ImU32 right);

// The grain: a seeded noise tile at 2x, at `alpha`, clipped to [a,b). This is
// what keeps a big flat-coloured panel from looking like a rectangle.
void Grain(ImDrawList* dl, ImVec2 a, ImVec2 b, float alpha);

// Stepped shadow OUTSIDE [a,b): 2 px rings out to `spread`, peak `alpha` at
// the edge, offset a little down-right the way pixel-art shadows are.
void DropShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float spread, float alpha);

// Stepped shadow INSIDE [a,b) along every edge: the recess of a slot or a
// track.
void InnerShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float spread, float alpha);

// Stepped glow ring OUTSIDE [a,b) in `col`: hover and selection — the state
// is said with light, not with a different fill colour.
void Glow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float spread,
          float alpha);

// The metallic panel body: drop shadow, base fill, bronze wash from the top,
// lateral sheen peaking at `sheenPeak` (0..1 across the width), the top
// ridge, the bottom inset, then grain. Draw it, then Draw9 the frame over it,
// then the contents.
struct PanelStyle {
  float darkMix = 0.55f;      // 0 = pure obsidian, 1 = pure panel field
  float sheenPeak = 0.45f;    // where the lateral highlight peaks
  float bronzeAlpha = 0.16f;  // warmth of the top-down wash
  float sheenAlpha = 0.055f;  // strength of the lateral highlight
  float grainAlpha = 0.06f;
  float shadow = 14.0f;       // drop shadow spread, px; 0 = none
};
void PanelBody(ImDrawList* dl, ImVec2 a, ImVec2 b, const PanelStyle& st = {});

// The chrome header bar across the top of a panel's content area: vertical
// gloss, lateral sheen, dark top line, gold underline, an accent tick, and the
// title in pale gold with a soft glow under it. `right` is an optional dim
// caption at the right end. Returns the bar's bottom y.
constexpr float kHeaderH = 36.0f;
float HeaderBar(ImDrawList* dl, ImVec2 a, float width, const char* title,
                const char* right = nullptr);

// A small-caps section label with a thin bronze rule running out to `width`.
// Returns the y just under it.
float Subheading(ImDrawList* dl, ImVec2 at, float width, const char* text);

// Text with a one-pixel obsidian shadow under it, so it stays legible on any
// of the surfaces above.
void ShadowText(ImDrawList* dl, ImVec2 at, ImU32 col, const char* text);

// Text with letter-spacing (ASCII only). Titles and small-caps labels.
void TrackedText(ImDrawList* dl, ImVec2 at, ImU32 col, const char* text,
                 float tracking);
float TrackedTextWidth(const char* text, float tracking);

// The recess UNDER a slot sprite: banded gradient, grain, inner shadow, and
// the state said in light around it. The sprite frame goes on top.
// `Accept` is "a drag is in flight and THIS is where it goes" — the positive
// half of `Refuse`, which existed alone and made every equip slot go red the
// moment anything was picked up, including the one slot that would have taken
// it. A refusal with nothing to contrast against says only "not here".
enum class SlotLook { Empty, Filled, Hover, Refuse, Accept };
void SlotSurface(ImDrawList* dl, ImVec2 a, float size, SlotLook look,
                 bool selected);

// A value bar: recessed track with inner shadow, banded fill from `fill` to a
// brighter `fill`. `hero` bars (health, mana) get the outer glow xyzpan gives
// its hero sliders.
void ValueBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, ImU32 fill,
              bool hero);

// The screen's tooltip, always in the 13 px small font and wrapped at 320 px.
// One owner, because a tooltip set in the 26 px chrome face is the panel's
// worst clipped text and every surface here needs the same one.
void BeginTip();
void EndTip();
void Tip(const char* text);

// ---- the quill stroke (PLAN_spell_graph) ------------------------------------
//
// A cubic from `from` to `to` with the control points straight DOWN out of the
// child and straight UP into the parent, flattened and drawn as a RIBBON - a
// strip of quads whose half-width follows a nib profile, thin at the tip, full
// at the belly, thin into the join.
//
// IT USED TO BE A POLYLINE OF THICK SEGMENTS, and that is exactly what made it
// look janky: `AddLine` with a 6 px width draws each segment as its own
// unjoined rectangle, so every bend in the curve opened a notch on the outside
// and doubled the ink on the inside, and quantising each segment's width to a
// 2 px step turned the taper into a visible staircase of loose blocks. A
// ribbon has no joins to open: consecutive quads share their two end vertices,
// so the edge is continuous however hard the curve turns.
//
// AND IT WOBBLES. A quill held by a hand does not draw a mathematical cubic;
// the stroke is displaced along its own normal by a couple of pixels of
// deterministic tremor (seeded from the endpoints, so the same edge wobbles
// the same way every frame), and laid down twice - a wider, paler pass that
// reads as ink bleeding into the fibre, then the core over it. That, and not
// the curve, is what makes it read as drawn.
void InkStroke(ImDrawList* dl, ImVec2 from, ImVec2 to, ImU32 col, float weight);

// ---- the sheet -------------------------------------------------------------
//
// A leaf of vellum filling [a,b): the warm base, the light falling from the
// top-left, the rubbed edges, the fibre. Everything else drawn on a page -
// the ruling, the marginalia, the figure - is the caller's, because it
// belongs in the page's own coordinates and this does not.
void VellumSheet(ImDrawList* dl, ImVec2 a, ImVec2 b);
// The book's window onto it: the shadow of the binding round the opening and
// a gold edge down the gutter. Drawn last, over everything on the sheet.
void VellumFrame(ImDrawList* dl, ImVec2 a, ImVec2 b);
// A rubricated VERSAL: the big coloured initial a scribe opens a passage with,
// in a ruled box with corner serifs. Returns the box's width.
float Versal(ImDrawList* dl, ImVec2 at, char letter, float size);

// ---- compass-and-rule figures (the manuscript's own vocabulary) -------------
//
// A diagram in a book like this is drawn with a compass and a straight edge,
// and every one of these is one of those two instruments. They are PIXEL
// figures, not vector ones: every sample is snapped to the same 2 px lattice
// the rest of the chrome lives on, so a circle here is a stepped circle drawn
// by the same hand that drew the 9-slice frames - not an anti-aliased curve
// sitting beside them saying "different program".

// An arc of radius `r` about `c`, from `a0` to `a1` radians (y down, 0 = +x),
// `thick` px of ink. Sampled at roughly one step per pixel of arc length and
// de-duplicated, so the cost is the arc's length and not the caller's guess.
void PixelArc(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 col,
              float thick);
// The whole circle.
void PixelRing(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float thick);
// Filled, row by row: the wash inside a roundel.
void PixelDisc(ImDrawList* dl, ImVec2 c, float r, ImU32 col);
// A straight edge in dots rather than in line: the ruling a scribe pricks out
// before writing, and the marginal dots down a manuscript's gutter.
void DottedRule(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float step,
                float dot);
// A lozenge (a diamond on its point) inscribed in [c-r, c+r]: a bead on a
// trunk, a junction on a bus.
void PixelLozenge(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool filled,
                  float thick);


// The little key cap in a slot corner ("1".."0").
void KeyBadge(ImDrawList* dl, ImVec2 at, const char* key);

// A count in a slot's bottom-right corner ("12").
void CountBadge(ImDrawList* dl, ImVec2 slotBr, const char* text);

// The screen dim behind the panels: a flat dim plus a banded vignette so the
// world falls away toward the edges and the panels read as the lit thing.
void ScreenDim(ImDrawList* dl, ImVec2 disp, float dim, float vignette);

// A button drawn in this vocabulary. Lays out an ImGui item at `at`, so it
// hovers, clicks and tooltips like any other. Returns true when clicked.
bool Button(const char* id, ImVec2 at, const char* label, bool toggled = false,
            float minWidth = 0.0f);

}  // namespace ui
