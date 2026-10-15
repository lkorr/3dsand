#include "ui/overlay.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

// BACKEND EXCEPTION (docs/PLAN_vulkan_port.md phase 4b): this file is the ONE
// place outside src/gpu/ that may name native GPU handles, because ImGui's
// render backend takes them directly — ImGui_ImplVulkan_* wants the
// VkInstance/VkDevice/VkCommandBuffer. The overlay's own INTERFACE
// (overlay.h) speaks only rhi::, so nothing above it sees any of this.
// The ImGui_ImplWGPU_* half was deleted with Dawn (2026-08-22).
#include "gpu/rhi_vk.h"
#include "gpu/rhi_vulkan.h"
#include "sim/tuning.h"  // the Combat panel edits melee/combatfx/gore live
#include "sim/world.h"   // kWindPrimCap for the primitive panel
#include "sim/weather.h" // the weather row: preset pin + readout
#include "sim/windfield.h" // the wind & weather section: regimes + readout
#include "ui/contract_ui.h"
#include "ui/dialogue_ui.h"
#include "ui/map_ui.h"
#include "ui/inventory_ui.h"
#include "ui/refs_ui.h"
#include "ui/theme.h"

bool Overlay::Init(GLFWwindow* window, const rhi::Device& device,
                   rhi::TextureFormat format, const std::string& assetDir) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ui::ApplyFantasyTheme();
  if (!ImGui_ImplGlfw_InitForOther(window, true)) return false;
  {
    std::string cerr;
    if (!ui::LoadChrome(assetDir, cerr))
      std::fprintf(stderr, "ui/chrome: %s (panels will draw plain)\n",
                   cerr.c_str());
  }

  vk::Backend* be = rhi::vkr::NativeBackend(device);
  if (!be) return false;
  // The engine loads Vulkan dynamically (VK_NO_PROTOTYPES everywhere), so
  // ImGui gets its entry points from the same loader.
  if (!ImGui_ImplVulkan_LoadFunctions(
          VK_API_VERSION_1_3,
          [](const char* name, void* ud) {
            return ((vk::Backend*)ud)->InstanceProc(name);
          },
          be))
    return false;
  ImGui_ImplVulkan_InitInfo info{};
  info.ApiVersion = VK_API_VERSION_1_3;
  info.Instance = be->Instance();
  info.PhysicalDevice = be->PhysicalDevice();
  info.Device = be->Device();
  info.QueueFamily = be->QueueFamily();
  info.Queue = be->GpuQueue();
  // Let the backend create its own descriptor pool. It is a FIXED budget of
  // combined image samplers, one per RegisterTexture and per atlas page, and
  // exhausting it does not fail cleanly: the next texture update reads a null
  // set inside the driver. The minimum (8) held the atlas, the chrome, the
  // bench, the item stage and one portrait; the portrait's eight size tiers
  // (UIState::portraitTierTex, 2026-10-08) took it past that. 32 leaves room.
  info.DescriptorPoolSize = 32;
  info.MinImageCount = 2;
  info.ImageCount = be->SwapchainImageCount() >= 2 ? be->SwapchainImageCount() : 2;
  // Same dynamic-rendering scope as the world pass it draws into; the
  // formats must match Simulation's pipelines (color = swapchain format,
  // depth = kDepthFormat), or ImGui renders into an incompatible scope.
  static VkFormat colorFmt;  // ImGui keeps the pointer; static storage
  colorFmt = format == rhi::TextureFormat::BGRA8Unorm ? VK_FORMAT_B8G8R8A8_UNORM
                                                      : VK_FORMAT_R8G8B8A8_UNORM;
  info.UseDynamicRendering = true;
  info.PipelineInfoMain.PipelineRenderingCreateInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
  info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
  info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFmt;
  info.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat =
      VK_FORMAT_D32_SFLOAT;
  if (!ImGui_ImplVulkan_Init(&info)) return false;

  // ---- the one sampler (character-panel portrait) --------------------------
  // NEAREST + CLAMP, and both halves matter. The portrait is rendered at a
  // fixed offscreen size and displayed at an integer multiple of it, so linear
  // filtering would buy nothing but blur — and the whole look here is pixels
  // on a whole-number grid. Clamp because a portrait is not tiled and an edge
  // that wraps is a visible seam.
  //
  // Resolved through InstanceProc rather than a DeviceFns row: the engine
  // loads Vulkan dynamically (VK_NO_PROTOTYPES) and DeviceFns carries only the
  // entry points the SIM needs. One sampler created once, in the one file
  // allowed to name Vulkan handles at all, does not earn a row in that table.
  device_ = be;
  auto createSampler =
      (PFN_vkCreateSampler)be->InstanceProc("vkCreateSampler");
  if (createSampler) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 1.0f;
    VkSampler smp = VK_NULL_HANDLE;
    if (createSampler(be->Device(), &si, nullptr, &smp) == VK_SUCCESS)
      sampler_ = (uint64_t)smp;
  }
  if (!sampler_)
    std::fprintf(stderr,
                 "ui: no sampler — the character portrait will draw empty\n");
  return true;
}

uint64_t Overlay::RegisterTexture(const rhi::TextureView& view) {
  VkImageView iv = rhi::vkr::NativeImageView(view);
  if (!iv || !sampler_) return 0;
  // SHADER_READ_ONLY_OPTIMAL: the recorder leaves a colour attachment in that
  // layout after the pass that wrote it, which is the whole reason the
  // portrait can be sampled without an explicit transition here.
  VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(
      (VkSampler)sampler_, iv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  return (uint64_t)ds;
}

void Overlay::UnregisterTexture(uint64_t id) {
  if (id) ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)id);
}

// The F1 sidebar's width last frame (the popout windows open right of it),
// and the top of the HUD's bottom-left block last frame (the sidebar stops
// above it, so the bars and the body figure stay where the player expects
// them and never land on top of the panel).
static float sPanelW = 480.0f;
static float sHudTop = -1.0f;

// Double-click any slider to type a precise value (one at a time).
static ImGuiID sManualInputId    = 0;
static bool    sManualInputFocus = false;

static bool EditableSliderFloat(const char* label, float* v, float lo, float hi,
                                const char* fmt = "%.3f") {
  ImGuiID id = ImGui::GetID(label);
  if (sManualInputId == id) {
    if (sManualInputFocus) {
      ImGui::SetKeyboardFocusHere();
      sManualInputFocus = false;
    }
    ImGui::InputFloat(label, v, 0, 0, fmt);
    if (ImGui::IsItemDeactivated()) {
      *v = std::clamp(*v, lo, hi);
      sManualInputId = 0;
      return true;
    }
    return false;
  }
  bool changed = ImGui::SliderFloat(label, v, lo, hi, fmt);
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
    sManualInputId = id;
    sManualInputFocus = true;
  }
  return changed;
}

static bool EditableSliderInt(const char* label, int* v, int lo, int hi) {
  ImGuiID id = ImGui::GetID(label);
  if (sManualInputId == id) {
    if (sManualInputFocus) {
      ImGui::SetKeyboardFocusHere();
      sManualInputFocus = false;
    }
    ImGui::InputInt(label, v, 0, 0);
    if (ImGui::IsItemDeactivated()) {
      *v = std::clamp(*v, lo, hi);
      sManualInputId = 0;
      return true;
    }
    return false;
  }
  bool changed = ImGui::SliderInt(label, v, lo, hi);
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
    sManualInputId = id;
    sManualInputFocus = true;
  }
  return changed;
}

bool Overlay::WantsMouse() const { return ImGui::GetIO().WantCaptureMouse; }
bool Overlay::WantsKeyboard() const {
  return ImGui::GetIO().WantCaptureKeyboard;
}

void Overlay::QueueMouse(void* iov, float x, float y, int button, bool down,
                         float wheel, float dblClickSec) {
  ImGuiIO& io = *static_cast<ImGuiIO*>(iov);
  if (dblClickSec > 0.0f) io.MouseDoubleClickTime = dblClickSec;
  io.AddMousePosEvent(x, y);
  if (button >= 0) io.AddMouseButtonEvent(button, down);
  if (wheel != 0.0f) io.AddMouseWheelEvent(0.0f, wheel);
}

void Overlay::BeginFrame() {
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  if (injectInput) injectInput(&ImGui::GetIO());
  ImGui::NewFrame();
  // AFTER NewFrame: ImGui may have created or resized its texture during font
  // baking, and the chrome rects have to be re-blitted when it does (ui/theme.h
  // "THE ONE HAZARD").
  ui::RefreshChrome();
}

// ---- the player-facing HUD --------------------------------------------------
//
// Bottom-left, two stacked bars: health above mana. Deliberately NOT an ImGui
// window — it is chrome, not a panel, so it neither moves, focuses, nor eats
// the mouse, and it is drawn on the FOREGROUND list so nothing in the dev panel
// can land on top of it.
//
// The dev panel's magic bar (Draw() below) stays as-is and keeps showing the
// mana/health CROSSOVER on one shared axis, which is a debugging readout. This
// HUD answers a different question — "how much of each do I have" — so the two
// pools get one bar each and the cost is shown as drain off the right end of
// whichever pool will pay it.
void Overlay::DrawHUD(const UIState& s) {
  ImGui::PushFont(ui::FontSmall());
  ImDrawList* d = ImGui::GetForegroundDrawList();
  const ImVec2 disp = ImGui::GetIO().DisplaySize;

  const float w = 240.0f, h = 16.0f, pad = 18.0f, gap = 6.0f;
  const float x = pad;
  // Anchored to the BOTTOM edge: y is derived from display height so the HUD
  // stays put when the window is resized.
  //
  // ...UNLESS THE HOTBAR IS IN THE WAY. The strip is centred (inventory_ui.cpp
  // DrawHudHotbar: n slots at a 48 px pitch, and a hand slot 18 px off each
  // end with its 6 px backing), so on a window narrower than ~1300 px (an
  // almost square monitor) its left hand reaches under these bars. Then the
  // bars, and everything stacked on them, stand on the strip's shoulder
  // instead: above its hand captions, still at the left edge.
  const float stripHalf =
      0.5f * ((float)s.hotbarSlots.size() * 48.0f - 4.0f) + 18.0f + 44.0f + 6.0f;
  // The bow tab (below) sits right of the bars too, beside or atop the compass.
  const float bowPx = 2.0f;                 // the bow's pixel
  const float bowGlyphW = 13 * bowPx;       // string pulled 6 + stave bow 4 + head 3
  char bowCount[16];
  std::snprintf(bowCount, sizeof bowCount, "x%d", std::max(0, s.arrowsLeft));
  const ImVec2 bowCs = ImGui::CalcTextSize(bowCount);
  const float bowInner = bowGlyphW + 8.0f + bowCs.x;
  const float barsRight =
      x + w + std::max(s.strikeCompass ? 14.0f + 116.0f : 0.0f,
                       s.bowDraw >= 0.0f ? 14.0f + 6.0f + bowInner + 6.0f : 0.0f);
  const bool overStrip =
      !s.hotbarSlots.empty() && barsRight + 12.0f > disp.x * 0.5f - stripHalf;
  // 14 + 44 = the strip's bottom margin and slot; 30 = its backing and the
  // RMB/LMB caption over a hand slot.
  const float yMana = overStrip ? disp.y - (14.0f + 44.0f + 30.0f) - h : disp.y - pad - h;
  const float yHealth = yMana - gap - h;

  // One bar: backdrop, fill, and an optional brighter "this is about to be
  // spent" segment eating right-to-left off the end of the fill.
  // `cap` < max draws the span past it as CHARRED OFF — the burn cap
  // (UIState::healthCap). -1 = no cap for this pool.
  auto bar = [&](float y, int32_t cur, int32_t max, int32_t pending,
                 ImU32 fill, ImU32 spend, const char* label, int32_t cap) {
    const ImVec2 p(x, y), q(x + w, y + h);
    d->AddRectFilled(ImVec2(p.x - 2, p.y - 2), ImVec2(q.x + 2, q.y + 2),
                     IM_COL32(0, 0, 0, 110), 3.0f);          // outer scrim
    d->AddRectFilled(p, q, IM_COL32(18, 20, 28, 220), 2.0f);  // empty track
    if (max > 0) {
      if (cur < 0) cur = 0;
      if (cur > max) cur = max;
      const float frac = (float)cur / (float)max;
      const float fx = p.x + w * frac;
      if (frac > 0) d->AddRectFilled(p, ImVec2(fx, q.y), fill, 2.0f);
      if (cap >= 0 && cap < max) {
        const float cx = p.x + w * ((float)cap / (float)max);
        d->AddRectFilled(ImVec2(cx, p.y), q, IM_COL32(40, 30, 26, 235), 2.0f);
        d->AddLine(ImVec2(cx, p.y), ImVec2(cx, q.y), IM_COL32(120, 60, 40, 255));
      }
      // Pending cost: the part of the fill this pool is about to lose.
      if (pending > 0) {
        int32_t take = pending < cur ? pending : cur;
        if (take > 0) {
          const float sx = p.x + w * ((float)(cur - take) / (float)max);
          d->AddRectFilled(ImVec2(sx, p.y), ImVec2(fx, q.y), spend, 2.0f);
        }
      }
    }
    d->AddRect(p, q, IM_COL32(255, 255, 255, 45), 2.0f);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %d/%d", label, cur < 0 ? 0 : cur, max);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    const ImVec2 tp(p.x + 6, p.y + (h - ts.y) * 0.5f);
    d->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 190), buf);
    d->AddText(tp, IM_COL32(235, 238, 245, 255), buf);
  };

  // A spoken spell drains mana first and only bites into health past it — the
  // same split ResolveCast() applies, so the HUD cannot promise a cost the VM
  // will not charge.
  const int32_t fromMana = s.spellCost < s.mana ? s.spellCost : s.mana;
  const int32_t fromHealth = s.spellCost - fromMana;

  bar(yHealth, s.health, s.healthMax, fromHealth, IM_COL32(190, 55, 55, 235),
      IM_COL32(255, 140, 60, 245), "hp", s.healthCap);
  // The reservation is drawn the way the burn cap is: the span past the
  // effective max is what the auras are holding, not mana that was spent.
  bar(yMana, s.mana, s.manaPoolMax > 0 ? s.manaPoolMax : s.manaMax, fromMana,
      IM_COL32(70, 120, 230, 235), IM_COL32(150, 200, 255, 245), "mp",
      s.manaReserved > 0 ? s.manaMax : -1);

  // ---- the strike compass, right of the bars (UIState::strikeCompass) -------
  // The flick map's spokes, the live flick against the pick threshold (the
  // inner ring), the spoke a press would take NOW (gold), the spoke the last
  // press took (ember, fading), and two lines naming the last strike and
  // where the running one is.
  //
  // ...AND THE CHARGED STRIKE. Holding the button past the windup turns the
  // box ember: the HELD spoke is drawn ember and grows to full length as the
  // arm slides onto it (a re-aim restarts it short), the remembered flick it
  // re-aims to is a hollow square on the rim, the centre fills, and the first
  // readout line says what a release does. The charged cut keeps the ember
  // spoke until the stroke ends.
  if (s.strikeCompass) {
    const float S = 116.0f;
    const float bx = std::floor(x + w + 14.0f);
    const float by = std::floor(yMana + h - S);
    const ImVec2 c(std::floor(bx + S * 0.5f), std::floor(by + S * 0.5f));
    const float R = S * 0.5f - 6.0f;
    const float r0 = R * 0.30f;   // the pick threshold's radius
    d->AddRectFilled(ImVec2(bx - 2, by - 2), ImVec2(bx + S + 2, by + S + 2),
                     IM_COL32(0, 0, 0, 110));
    d->AddRectFilled(ImVec2(bx, by), ImVec2(bx + S, by + S), IM_COL32(18, 20, 28, 200));
    d->AddRect(ImVec2(bx, by), ImVec2(bx + S, by + S),
               s.strikeCharging ? ui::ColEmber() : ui::ColBronze(), 0.0f, 0,
               s.strikeCharging ? 3.0f : 2.0f);
    d->AddCircle(c, r0, IM_COL32(255, 255, 255, 50), 20, 1.0f);
    const float lastA = std::clamp(1.0f - s.strikeLastAge / 3.0f, 0.0f, 1.0f);
    // "horizontal_r" -> "hor_r": every word cut to three.
    auto shortName = [](const std::string& n) {
      std::string out;
      size_t a = 0;
      while (a <= n.size()) {
        size_t b = n.find('_', a);
        if (b == std::string::npos) b = n.size();
        if (!out.empty()) out += '_';
        out += n.substr(a, std::min<size_t>(3, b - a));
        a = b + 1;
      }
      return out;
    };
    for (int k = 0; k < (int)s.strikeSectors.size(); k++) {
      const UIState::StrikeSector& sec = s.strikeSectors[k];
      const ImVec2 tip(std::floor(c.x + sec.x * R * 0.55f),
                       std::floor(c.y + sec.y * R * 0.55f));
      ImU32 col = sec.neutral ? ui::ColParchDim() : ui::ColSteel();
      float thick = 2.0f;
      if (k == s.strikeLastSector && lastA > 0.0f) {
        col = ui::Mix(col, ui::ColEmber(), lastA);
        thick = 4.0f;
      }
      if (k == s.strikeHover && !s.strikeCharged) {
        col = ui::ColGoldHi();
        thick = 4.0f;
      }
      if (k == s.strikeHeldSector) {
        // The held (or charged-and-cutting) strike: a dim full-length guide
        // with the ember spoke over it, as long as the slide is far along.
        d->AddLine(c, tip, ui::Fade(ui::ColEmber(), 0.35f), 5.0f);
        const float grow = 0.30f + 0.70f * s.strikeBlend;
        const ImVec2 gt(std::floor(c.x + sec.x * R * 0.55f * grow),
                        std::floor(c.y + sec.y * R * 0.55f * grow));
        d->AddLine(c, gt, ui::ColEmber(), 5.0f);
        d->AddRectFilled(ImVec2(gt.x - 3, gt.y - 3), ImVec2(gt.x + 3, gt.y + 3),
                         ui::ColEmber());
        col = ui::ColEmber();   // the label below takes the colour
      } else {
        d->AddLine(c, tip, col, thick);
      }
      const std::string lab = shortName(sec.name);
      const ImVec2 ts = ImGui::CalcTextSize(lab.c_str());
      const ImVec2 lc(c.x + sec.x * R * 0.80f, c.y + sec.y * R * 0.80f);
      ui::ShadowText(d, ImVec2(std::floor(lc.x - ts.x * 0.5f), std::floor(lc.y - ts.y * 0.5f)),
                     col, lab.c_str());
    }
    // The live flick: its length is speed / threshold x r0, so it crosses the
    // ring exactly when a press would read as a flick.
    const float sp = std::sqrt(s.strikeFlickX * s.strikeFlickX +
                               s.strikeFlickY * s.strikeFlickY);
    if (sp > 1e-3f) {
      const float len = std::min(R, sp / s.strikePickMin * r0);
      const ImVec2 fp(std::floor(c.x + s.strikeFlickX / sp * len),
                      std::floor(c.y + s.strikeFlickY / sp * len));
      const ImU32 fc = sp >= s.strikePickMin ? ui::ColGoldPale() : IM_COL32(150, 150, 160, 200);
      d->AddLine(c, fp, fc, 2.0f);
      d->AddRectFilled(ImVec2(fp.x - 2, fp.y - 2), ImVec2(fp.x + 2, fp.y + 2), fc);
    }
    // The last press's own flick direction, as a dot on the rim.
    if (s.strikeLastFlicked && lastA > 0.0f) {
      const ImVec2 lp(std::floor(c.x + s.strikeLastX * R), std::floor(c.y + s.strikeLastY * R));
      d->AddRectFilled(ImVec2(lp.x - 3, lp.y - 3), ImVec2(lp.x + 3, lp.y + 3),
                       ui::Fade(ui::ColEmber(), lastA));
    }
    // The remembered flick a charged hold re-aims to: a hollow square on the
    // rim (only while holding — a normal click never reads it).
    if (s.strikeCharging && s.strikeMemValid) {
      const ImVec2 mp(std::floor(c.x + s.strikeMemX * R), std::floor(c.y + s.strikeMemY * R));
      d->AddRect(ImVec2(mp.x - 4, mp.y - 4), ImVec2(mp.x + 4, mp.y + 4), ui::ColEmber(), 0.0f, 0, 2.0f);
    }
    if (s.strikeCharged)
      d->AddRectFilled(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), ui::ColEmber());
    else
      d->AddRectFilled(ImVec2(c.x - 2, c.y - 2), ImVec2(c.x + 2, c.y + 2), ui::ColParch());

    // The readout, bottom-aligned beside the box.
    const float tx = bx + S + 10.0f;
    const float lh = ImGui::GetTextLineHeight() + 2.0f;
    float ty = std::floor(by + S - lh * 3.0f);
    char buf[96];
    if (s.strikeCharging) {
      std::snprintf(buf, sizeof buf, "CHARGED x%.1f  release to strike, flick to re-aim",
                    s.strikeChargeMul);
      ui::ShadowText(d, ImVec2(tx, ty), ui::ColEmber(), buf);
    } else if (s.strikeCharged) {
      std::snprintf(buf, sizeof buf, "CHARGED STRIKE x%.1f", s.strikeChargeMul);
      ui::ShadowText(d, ImVec2(tx, ty), ui::ColEmber(), buf);
    } else {
      std::snprintf(buf, sizeof buf, "flick %.0f px/s (pick at %.0f)  hold to charge",
                    sp, s.strikePickMin);
      ui::ShadowText(d, ImVec2(tx, ty),
                     sp >= s.strikePickMin ? ui::ColGoldPale() : ui::ColParchDim(), buf);
    }
    ty += lh;
    if (!s.strikeLastText.empty()) {
      const std::string t = "last: " + s.strikeLastText;
      ui::ShadowText(d, ImVec2(tx, ty),
                     ui::Mix(ui::ColParchDim(), ui::ColEmber(), lastA), t.c_str());
    }
    ty += lh;
    if (!s.strikeNowText.empty())
      ui::ShadowText(d, ImVec2(tx, ty), ui::ColGoldHi(), s.strikeNowText.c_str());
  }

  // What is ON you used to be a text line here ("stained 40% . oil 22%");
  // it is the coat chip in the paper doll's right column now, still silent
  // below s.stainHudMin, with the per-substance breakdown on the character
  // screen where there is room to read it.
  const float yStack = yHealth - gap - 2.0f;

  // ---- body condition: the paper doll, directly above the hp bar -----------
  const float figureH = DrawBodyFigure(s, x, yStack, w);
  float yTop = yStack - figureH;

  // ---- swim stamina: a thin bar on top of the doll -------------------------
  // Only while afloat or refilling (UIState::swimStaminaShow). Water blue,
  // going to ember in the last fifth, where the body is about to sink.
  if (s.playerAlive && s.swimStaminaShow) {
    const float sh = 8.0f;
    const float sy = std::floor(yTop - gap - sh);
    const float frac = std::clamp(s.swimStamina, 0.0f, 1.0f);
    const ImVec2 p(x, sy), q(x + w, sy + sh);
    d->AddRectFilled(ImVec2(p.x - 2, p.y - 2), ImVec2(q.x + 2, q.y + 2),
                     IM_COL32(0, 0, 0, 110), 3.0f);
    d->AddRectFilled(p, q, IM_COL32(18, 20, 28, 220), 2.0f);
    const ImU32 fill = frac > 0.2f ? IM_COL32(70, 150, 215, 235)
                                   : ui::Mix(IM_COL32(200, 70, 50, 235),
                                             IM_COL32(70, 150, 215, 235), frac / 0.2f);
    if (frac > 0.0f)
      d->AddRectFilled(p, ImVec2(std::floor(p.x + w * frac), q.y), fill, 2.0f);
    d->AddRect(p, q, IM_COL32(255, 255, 255, 45), 2.0f);
    yTop = sy - 2.0f;
  }
  sHudTop = yTop - (s.playerAlive ? 8.0f : 30.0f);

  if (!s.playerAlive) {
    // Nothing respawns you on a timer any more (UIState::deathScreen), so the
    // HUD has to say where the way back is — a player staring at a corpse with
    // no prompt has no reason to believe the game is still waiting for them.
    const char* dead = s.deathScreen ? "DEAD  -  I  to respawn" : "DEAD";
    const ImVec2 ts = ImGui::CalcTextSize(dead);
    const ImVec2 tp(x, yTop - ts.y - 6);
    d->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 190), dead);
    d->AddText(tp, IM_COL32(255, 70, 60, 255), dead);
  }

  // ---- the look prompt, and what the last E did ----------------------------
  // Under the crosshair, on a dark tab, the way the character screen's footer
  // does it: "E  pick up robe". The kit message rides beneath it while fresh
  // so "picked up" / "you have no room for that" is SEEN in play rather than
  // only on the character screen, which is where it used to be drawn alone.
  auto tab = [&](const char* text, float y, ImU32 rim, ImU32 ink, float alpha) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 tp(std::floor((disp.x - ts.x) * 0.5f), y);
    d->AddRectFilled(ImVec2(tp.x - 10, tp.y - 3), ImVec2(tp.x + ts.x + 10, tp.y + ts.y + 3),
                     IM_COL32(0, 0, 0, (int)(150 * alpha)));
    d->AddRectFilled(ImVec2(tp.x - 10, tp.y - 3), ImVec2(tp.x + ts.x + 10, tp.y - 1),
                     ui::Fade(rim, 0.55f * alpha));
    d->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, (int)(190 * alpha)), text);
    d->AddText(tp, ui::Fade(ink, alpha), text);
    return ts.y + 8;
  };
  float py = std::floor(disp.y * 0.5f) + 28.0f;
  // ---- STEALTH: the eye above the crosshair (game/stealth.h) ---------------
  //
  // Skyrim's eye, in pixel art on a 3 px grid: a 15x7-block sprite that
  // opens with the highest detection meter any creature holds for the
  // player -- shut with its lashes down hidden, a slit at the notice level,
  // half open past it, wide when something has DETECTED you -- coloured by
  // the worst state among the
  // watchers (parchment calm, gold suspicious, ember searching, cinnabar
  // fighting). Round it, MGSV's per-watcher pips: one block per creature that
  // is watching, on a ring, at the screen bearing it is watching from, as
  // bright as it is sure. Under it, Chaos Theory's two meters on a dark tab:
  // the LIGHT on you (gold) and your NOISE (parchment) with a steel tick at
  // the level under which not even someone beside you hears it.
  if (s.playerAlive && s.stealthShow) {
    const float b = 2.0f;
    const ImU32 lidCol = s.stealthAlert >= 3   ? ui::ColBloodHi()
                         : s.stealthAlert == 2 ? ui::ColEmber()
                         : s.stealthAlert == 1 ? ui::ColGold()
                                               : ui::ColParchDim();
    // THE EYE: four hand-drawn 15x7 sprites, picked by how far the meter has
    // got -- shut (nobody has noticed you), a slit (something half-saw or
    // heard), half open (it is coming to look / searching), wide (DETECTED).
    static const char* kShut[7] = {"...............", "...............",
                                   "...............", ".##.........##.",
                                   "...#########...", "....#.#.#.#....",
                                   "..............."};
    static const char* kSlit[7] = {"...............", "...............",
                                   "....#######....", ".###..###..###.",
                                   "....#######....", "...............",
                                   "..............."};
    static const char* kHalf[7] = {"...............", "....#######....",
                                   "..##..###..##..", ".#...#####...#.",
                                   "..##..###..##..", "....#######....",
                                   "..............."};
    static const char* kOpen[7] = {"....#######....", "..##.......##..",
                                   ".#....###....#.", "#....#####....#",
                                   ".#....###....#.", "..##.......##..",
                                   "....#######...."};
    const float lv = std::clamp(s.stealthEye, 0.0f, 1.0f);
    const char* const* eye = lv >= 0.999f ? kOpen
                             : lv >= 0.45f ? kHalf
                             : lv >= 0.05f ? kSlit
                                           : kShut;
    // The tab: the eye on top, the two meters under it.
    constexpr int kPips = 10;
    const float pw = 3.0f, ph = 4.0f, pg = 1.0f;
    const float mw = kPips * pw + (kPips - 1) * pg;
    const float glyphW = 10.0f;
    const float eb = 3.0f;   // the eye's own block: 45 x 21 px
    const float tabW = std::max(15 * eb, glyphW + mw) + 16.0f;
    const float tabH = 7 * eb + 2 * ph + 4.0f + 18.0f;
    const float cx = std::floor(disp.x * 0.5f);
    const float tx0 = std::floor(cx - tabW * 0.5f);
    const float ty0 = std::floor(disp.y * 0.5f) - 34.0f - tabH;
    d->AddRectFilled(ImVec2(tx0, ty0), ImVec2(tx0 + tabW, ty0 + tabH), IM_COL32(0, 0, 0, 140));
    d->AddRect(ImVec2(tx0, ty0), ImVec2(tx0 + tabW, ty0 + tabH), ui::Fade(lidCol, 0.8f), 0.0f, 0,
               1.0f);
    const float ex0 = std::floor(cx - 7.5f * eb), ey0 = ty0 + 6.0f;
    for (int r = 0; r < 7; r++)
      for (int c = 0; c < 15; c++)
        if (eye[r][c] == '#')
          d->AddRectFilled(ImVec2(ex0 + c * eb, ey0 + r * eb),
                           ImVec2(ex0 + (c + 1) * eb, ey0 + (r + 1) * eb), lidCol);
    // THE WATCHERS' PIPS on a ring round the tab, at the screen bearing each
    // watches from (0 = ahead, clockwise), as bright as it is sure.
    const float rcx = tx0 + tabW * 0.5f, rcy = ty0 + tabH * 0.5f;
    const float ring = 0.5f * std::sqrt(tabW * tabW + tabH * tabH) + 6.0f;
    for (int i = 0; i < s.stealthPipN && i < 4; i++) {
      const float a = s.stealthPipAngle[i];
      const float px = std::floor(rcx + std::sin(a) * ring - b);
      const float pyy = std::floor(rcy - std::cos(a) * ring - b);
      const ImU32 pc = s.stealthPipAlert[i] >= 3   ? ui::ColBloodHi()
                       : s.stealthPipAlert[i] == 2 ? ui::ColEmber()
                                                   : ui::ColGold();
      d->AddRectFilled(ImVec2(px - 1, pyy - 1), ImVec2(px + 2 * b + 1, pyy + 2 * b + 1),
                       IM_COL32(0, 0, 0, 150));
      d->AddRectFilled(ImVec2(px, pyy), ImVec2(px + 2 * b, pyy + 2 * b),
                       ui::Fade(pc, 0.35f + 0.65f * std::clamp(s.stealthPipLevel[i], 0.0f, 1.0f)));
    }
    // THE TWO METERS: ten pips each, the empty ones still drawn so the
    // meter's length reads, and the noise meter's steel tick at the level
    // under which not even someone at arm's length hears you.
    const float mx = std::floor(tx0 + (tabW - (glyphW + mw)) * 0.5f + glyphW);
    auto meter = [&](float y, float frac, ImU32 col, float tick, const char* glyph) {
      d->AddText(ImVec2(mx - glyphW, y - 5.0f), ui::Fade(col, 0.85f), glyph);
      const int lit = (int)std::ceil(std::clamp(frac, 0.0f, 1.0f) * kPips - 1e-3f);
      for (int i = 0; i < kPips; i++) {
        const float x0 = mx + i * (pw + pg);
        d->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + pw, y + ph), i < lit ? col : ui::ColMid());
      }
      if (tick >= 0.0f) {
        const float tx = std::floor(mx + std::clamp(tick, 0.0f, 1.0f) * mw);
        d->AddRectFilled(ImVec2(tx, y - 2.0f), ImVec2(tx + 1.0f, y + ph + 2.0f), ui::ColSteel());
      }
    };
    const float my = ey0 + 7 * eb + 6.0f;
    meter(my, s.stealthLight, ui::ColGold(), -1.0f, "*");
    meter(my + ph + 5.0f, s.stealthNoise, ui::ColParch(), s.stealthQuiet, "~");
  }
  // ---- THE SNEAK-ATTACK TOAST, under the crosshair -------------------------
  if (s.playerAlive && s.sneakToastAge < 1.6f && s.sneakToastMul > 1.0f) {
    char t[48];
    std::snprintf(t, sizeof t, "%s  x%.1f", s.sneakToastRanged ? "SNEAK SHOT" : "SNEAK ATTACK",
                  s.sneakToastMul);
    const float alpha = std::clamp((1.6f - s.sneakToastAge) / 0.5f, 0.0f, 1.0f);
    py += tab(t, py, ui::ColEmber(), ui::ColEmber(), alpha);
  }
  // ---- SHOCKED: an edge flash, then STUNNED under the crosshair ------------
  //
  // Pixel art on the 2 px grid like the throw meter below. The FLASH: for six
  // ticks after a jolt, a frame of pale-blue blocks round the screen's edge,
  // stepping out (not fading smoothly) -- the arc's own colour. The CUE: a
  // dark tab holding a 5x8-block lightning glyph, the word STUNNED and one pip
  // per remaining half-second (15 ticks), the whole tab twitching a whole
  // pixel on a fast step while the body does. Nothing is drawn when neither.
  if (s.playerAlive && s.shockTicksAgo >= 0 && s.shockTicksAgo < 6) {
    const ImU32 arc = IM_COL32(170, 214, 255, 255);
    const float a = 0.55f - 0.09f * (float)s.shockTicksAgo;
    const float t = 6.0f + 2.0f * (float)(s.shockTicksAgo & ~1);
    // Broken border: 18 px blocks with 6 px gaps, offset per tick so it
    // crackles rather than sits.
    const float off = (float)((s.shockTicksAgo * 10) % 24);
    for (float x0 = -off; x0 < disp.x; x0 += 24.0f) {
      d->AddRectFilled(ImVec2(std::max(0.0f, x0), 0), ImVec2(x0 + 18.0f, t), ui::Fade(arc, a));
      d->AddRectFilled(ImVec2(std::max(0.0f, x0), disp.y - t), ImVec2(x0 + 18.0f, disp.y),
                       ui::Fade(arc, a));
    }
    for (float y0 = -off; y0 < disp.y; y0 += 24.0f) {
      d->AddRectFilled(ImVec2(0, std::max(0.0f, y0)), ImVec2(t, y0 + 18.0f), ui::Fade(arc, a));
      d->AddRectFilled(ImVec2(disp.x - t, std::max(0.0f, y0)), ImVec2(disp.x, y0 + 18.0f),
                       ui::Fade(arc, a));
    }
  }
  if (s.playerAlive && s.stunTicksLeft > 0) {
    const ImU32 arc = IM_COL32(170, 214, 255, 255);
    const ImU32 core = IM_COL32(236, 246, 255, 255);
    const char* word = "STUNNED";
    const ImVec2 ts = ImGui::CalcTextSize(word);
    // The glyph: a jagged bolt in 2 px blocks, 5 wide x 8 tall.
    static const char* kBolt[8] = {"..###", ".###.", ".##..", "#####",
                                   "..##.", ".##..", ".#...", "#...."};
    const float b = 2.0f, gw = 5 * b, gh = 8 * b;
    constexpr int kMaxPips = 8;
    const int pips = std::clamp((s.stunTicksLeft + 14) / 15, 1, kMaxPips);
    const float pw = 4.0f, ph = 6.0f, pg = 2.0f;
    const float pipsW = kMaxPips * pw + (kMaxPips - 1) * pg;
    const float inner = gw + 8.0f + std::max(ts.x, pipsW);
    const int step = (int)(ImGui::GetTime() * 30.0);
    const uint32_t h = (uint32_t)step * 2654435761u;
    const float jx = (float)((int)(h >> 29) % 3 - 1) * 2.0f;
    const float x0 = std::floor((disp.x - inner) * 0.5f) + jx;
    const float y0 = py;
    const float tabH = std::max(gh, ts.y + 4.0f + ph);
    d->AddRectFilled(ImVec2(x0 - 8, y0 - 4), ImVec2(x0 + inner + 8, y0 + tabH + 4),
                     IM_COL32(0, 0, 0, 170));
    d->AddRect(ImVec2(x0 - 8, y0 - 4), ImVec2(x0 + inner + 8, y0 + tabH + 4), arc, 0.0f, 0,
               2.0f);
    const float gy = y0 + std::floor((tabH - gh) * 0.5f);
    for (int r = 0; r < 8; r++)
      for (int c = 0; c < 5; c++)
        if (kBolt[r][c] == '#')
          d->AddRectFilled(ImVec2(x0 + c * b, gy + r * b), ImVec2(x0 + (c + 1) * b, gy + (r + 1) * b),
                           (step & 2) ? core : arc);
    const float tx = x0 + gw + 8.0f;
    d->AddText(ImVec2(tx + 1, y0 + 1), IM_COL32(0, 0, 0, 190), word);
    d->AddText(ImVec2(tx, y0), core, word);
    const float pyy = y0 + ts.y + 4.0f;
    for (int i = 0; i < kMaxPips; i++) {
      const ImVec2 a(tx + i * (pw + pg), pyy), c(tx + i * (pw + pg) + pw, pyy + ph);
      d->AddRectFilled(a, c, i < pips ? arc : ui::ColDeep());
    }
    py += tabH + 14.0f;
  }
  // ---- the summoner's demon (game/demon.h, docs/PLAN_demons.md D1 + D3) ----
  //
  // A small tab: a pixel salt ring (2 px blocks, 7x7) and up to three lines.
  //   1. the name and the state: CONTAINED (with the circle's radius) draws
  //      the ring whole in salt-white; UNBOUND draws it broken in ember;
  //      RELEASED draws it whole in gold.
  //   2. (contained, D3) the circle's STRENGTH against the demon's POWER --
  //      salt-white while it would hold, ember while it would not; the power
  //      reads `20-6` when iron in the band weakens the demon (D6) -- then the
  //      three channels as letters (M move, C cast out, B blink):
  //      salt-white = a seal severs it, ember = a loophole; then the key.
  //   3. (contained, while the gaze strains) AVERT or MEET EYES and ten pixel
  //      pips of strain, ember while you are breaking the rule.
  // The tab is sized from the widest line, so the words always fit their box.
  if (s.playerAlive && s.demonState != 0 && !s.demonName.empty()) {
    const bool held = s.demonState == 1, released = s.demonState == 3;
    const ImU32 salt = IM_COL32(236, 232, 220, 255);
    const ImU32 ring = held ? salt : released ? ui::ColGold() : ui::ColEmber();
    char line[96];
    std::string up = s.demonName;
    for (char& c : up) c = (char)std::toupper((unsigned char)c);
    if (held)
      std::snprintf(line, sizeof line, "%s  CONTAINED  %.1f m", up.c_str(), s.demonRadiusM);
    else if (released)
      std::snprintf(line, sizeof line, "%s  RELEASED", up.c_str());
    else
      std::snprintf(line, sizeof line, "%s  UNBOUND", up.c_str());
    const bool showBind = held && s.demonHasBinding;
    const bool holds = s.demonStrength >= s.demonPower - s.demonIronCut;
    char str[48];
    if (s.demonIronCut > 0)
      std::snprintf(str, sizeof str, "CIRCLE %d / %d-%d  ", s.demonStrength, s.demonPower,
                    s.demonIronCut);
    else
      std::snprintf(str, sizeof str, "CIRCLE %d / %d  ", s.demonStrength, s.demonPower);
    constexpr int kChanN = 3;
    static const char kChan[kChanN] = {'M', 'C', 'B'};
    const char* keyHint = "  T TALK  Y RELEASE";
    const bool showGaze = showBind && (s.demonStrain > 0.0f || s.demonGazeBroken);
    const char* gazeWord = s.demonGazeHold ? "MEET EYES " : "AVERT ";
    constexpr int kPips = 10;
    const float pw = 4.0f, ph = 6.0f, pg = 2.0f;
    const ImVec2 ts = ImGui::CalcTextSize(line);
    const float chW = ImGui::CalcTextSize("M ").x;
    const float l2w = showBind ? ImGui::CalcTextSize(str).x + kChanN * chW + ImGui::CalcTextSize(keyHint).x
                               : 0.0f;
    const float l3w =
        showGaze ? ImGui::CalcTextSize(gazeWord).x + kPips * (pw + pg) : 0.0f;
    const float textW = std::max(ts.x, std::max(l2w, l3w));
    const float lh = ts.y + 2.0f;
    const int lines = 1 + (showBind ? 1 : 0) + (showGaze ? 1 : 0);
    static const char* kRing[7] = {"..###..", ".#...#.", "#.....#", "#.....#",
                                   "#.....#", ".#...#.", "..###.."};
    const float b = 2.0f, gw = 7 * b, gh = 7 * b;
    const float inner = gw + 8.0f + textW;
    const float x0 = std::floor((disp.x - inner) * 0.5f);
    const float y0 = py;
    const float tabH = std::max(gh, lh * lines - 2.0f);
    d->AddRectFilled(ImVec2(x0 - 8, y0 - 4), ImVec2(x0 + inner + 8, y0 + tabH + 4),
                     IM_COL32(0, 0, 0, 170));
    d->AddRect(ImVec2(x0 - 8, y0 - 4), ImVec2(x0 + inner + 8, y0 + tabH + 4), ring, 0.0f, 0,
               2.0f);
    const float gy = y0 + std::floor((tabH - gh) * 0.5f);
    for (int r = 0; r < 7; r++)
      for (int c = 0; c < 7; c++) {
        if (kRing[r][c] != '#') continue;
        // Broken: the ring's right-hand cells are missing.
        if (!held && !released && c >= 5 && r >= 2 && r <= 4) continue;
        d->AddRectFilled(ImVec2(x0 + c * b, gy + r * b), ImVec2(x0 + (c + 1) * b, gy + (r + 1) * b),
                         ring);
      }
    const float tx = x0 + gw + 8.0f;
    auto text = [&](float x, float y, ImU32 col, const char* t) {
      d->AddText(ImVec2(x + 1, y + 1), IM_COL32(0, 0, 0, 190), t);
      d->AddText(ImVec2(x, y), col, t);
    };
    text(tx, y0, ring, line);
    float ly = y0 + lh;
    if (showBind) {
      text(tx, ly, holds ? salt : ui::ColEmber(), str);
      float cx = tx + ImGui::CalcTextSize(str).x;
      for (int i = 0; i < kChanN; i++) {
        const char t[2] = {kChan[i], 0};
        text(cx, ly, ((s.demonSevered >> i) & 1u) ? salt : ui::ColEmber(), t);
        cx += chW;
      }
      text(cx, ly, ui::ColParchDim(), keyHint);
      ly += lh;
    }
    if (showGaze) {
      const ImU32 gc = s.demonGazeBroken ? ui::ColEmber() : ui::ColParch();
      text(tx, ly, gc, gazeWord);
      const float px = tx + ImGui::CalcTextSize(gazeWord).x;
      const float pyy = ly + std::floor((ts.y - ph) * 0.5f);
      const int lit = (int)std::ceil(s.demonStrain * kPips - 1e-4f);
      for (int i = 0; i < kPips; i++)
        d->AddRectFilled(ImVec2(px + i * (pw + pg), pyy), ImVec2(px + i * (pw + pg) + pw, pyy + ph),
                         i < lit ? gc : ui::ColDeep());
    }
    py += tabH + 14.0f;
  }
  // ---- the bound demons (demons D5, game/demon_talk.h): one tab each, ------
  // the contract, the duty in hand, the term left and the mana it holds.
  for (size_t i = 0; s.playerAlive && i < s.boundDemons.size() && i < 3; i++) {
    const UIState::BoundDemonUI& b = s.boundDemons[i];
    char term[32];
    if (b.termLeftS < 0) std::snprintf(term, sizeof term, "no end");
    else std::snprintf(term, sizeof term, "%d:%02d left", b.termLeftS / 60, b.termLeftS % 60);
    char t[200];
    std::snprintf(t, sizeof t, "%s  %s  %s  %s  -%d mana", b.name.c_str(), b.contract.c_str(),
                  b.contained ? "in the circle" : b.duty.empty() ? "standing" : b.duty.c_str(),
                  term, b.upkeep);
    py += tab(t, py, ui::ColGoldDim(), ui::ColGoldPale(), 0.95f);
  }
  // ---- the throw's wind-up: a row of pixel pips under the crosshair --------
  //
  // Ten 6x8 cells on the 2 px grid, lit left to right in gold as Q is held,
  // turning ember at full. FULL is the one state the eye has to catch without
  // counting pips, so at full the whole meter SHAKES -- a whole-pixel jitter
  // on a fast wall-clock step, never a sub-pixel slide -- and the rim glows.
  if (s.throwCharge >= 0.0f) {
    constexpr int kPips = 10;
    const float pw = 6.0f, ph = 8.0f, pg = 2.0f;
    const float mw = kPips * pw + (kPips - 1) * pg;
    const bool full = s.throwCharge >= 1.0f;
    float jx = 0.0f, jy = 0.0f;
    if (full) {
      const int step = (int)(ImGui::GetTime() * 40.0);
      const uint32_t h = (uint32_t)step * 2654435761u;
      jx = (float)((int)(h >> 29) % 3 - 1) * 2.0f;
      jy = (float)((int)(h >> 27) % 3 - 1) * 2.0f;
    }
    const float mx = std::floor((disp.x - mw) * 0.5f) + jx;
    const float my = py + jy;
    const ImU32 rim = full ? ui::ColEmber() : ui::ColBronze();
    if (full)
      d->AddRectFilled(ImVec2(mx - 6, my - 6), ImVec2(mx + mw + 6, my + ph + 6),
                       ui::Fade(ui::ColEmber(), 0.25f));
    d->AddRectFilled(ImVec2(mx - 4, my - 4), ImVec2(mx + mw + 4, my + ph + 4),
                     IM_COL32(0, 0, 0, 170));
    d->AddRect(ImVec2(mx - 4, my - 4), ImVec2(mx + mw + 4, my + ph + 4), rim,
               0.0f, 0, 2.0f);
    const int lit = (int)std::floor(s.throwCharge * kPips + 1e-4f);
    for (int i = 0; i < kPips; i++) {
      const float x0 = mx + i * (pw + pg);
      const ImVec2 a(x0, my), b(x0 + pw, my + ph);
      if (i < lit) {
        const ImU32 c = full ? ui::ColEmber() : ui::ColGold();
        d->AddRectFilled(a, b, c);
        d->AddRectFilled(a, ImVec2(b.x, a.y + 2), full ? ui::ColGoldPale()
                                                       : ui::ColGoldHi());
      } else {
        d->AddRectFilled(a, b, ui::ColDeep());
      }
    }
    py += ph + 14.0f;
  }
  // ---- the bow: a pixel bow right of the bars whose string comes back ------
  //
  // With a bow in either hand (UIState::bowDraw >= 0; game/arrows.h): a
  // braced bow drawn on the 2 px grid, 15 blocks tall, the stave bowing to
  // the right of a string whose middle is pulled back to the left by the
  // draw, with the nocked arrow across it. Under it, the draw as a bar (a
  // notch where a release starts to loose at all) and the arrows left. At a
  // FULL draw it holds still and the arrow's head goes ember -- the one state
  // to catch without reading anything. No arrows: the count is cinnabar and
  // there is no arrow on the string.
  //
  // Bottom-left, NOT under the crosshair: the centre of the screen is where
  // you aim. The tab stands beside the bars, bottom-aligned with the mp bar,
  // or on top of the strike compass when a melee hand puts that there.
  if (s.bowDraw >= 0.0f) {
    const float b = bowPx;
    const int rows = 15, mid = 7;
    const float draw = std::clamp(s.bowDraw, 0.0f, 1.0f);
    const bool full = draw >= 0.999f;
    const bool nocked = s.arrowsLeft > 0;
    const int pull = (int)std::lround(draw * 6.0f);   // blocks the string comes back
    const char* count = bowCount;
    const ImVec2 cs = bowCs;
    const float glyphW = bowGlyphW;
    const float barW = 13 * b, barH = 4.0f;
    const float inner = bowInner;
    const float tabH = rows * b + 4.0f + barH;
    const float x0 = std::floor(x + w + 14.0f + 6.0f);
    const float bottom = s.strikeCompass ? yMana + h - 116.0f - 2.0f - 8.0f : yMana + h;
    const float y0 = std::floor(bottom - 4.0f - tabH);
    d->AddRectFilled(ImVec2(x0 - 6, y0 - 4), ImVec2(x0 + inner + 6, y0 + tabH + 4),
                     IM_COL32(0, 0, 0, 170));
    d->AddRect(ImVec2(x0 - 6, y0 - 4), ImVec2(x0 + inner + 6, y0 + tabH + 4),
               full ? ui::ColEmber() : ui::ColBronze(), 0.0f, 0, 2.0f);
    // Block (c, r): column 0 is the string at full draw, the stave's tips sit
    // at column 6, its belly bows out to column 10.
    auto px = [&](int c, int r, ImU32 col) {
      d->AddRectFilled(ImVec2(x0 + c * b, y0 + r * b),
                       ImVec2(x0 + (c + 1) * b, y0 + (r + 1) * b), col);
    };
    for (int r = 0; r < rows; r++) {
      const float u = (float)(r - mid) / (float)mid;
      const int stave = 6 + (int)std::lround(4.0f * (1.0f - u * u));
      px(stave, r, (r == 0 || r == rows - 1) ? ui::ColGoldDim() : ui::ColGold());
      if (std::abs(r - mid) <= 1) px(stave + 1, r, ui::ColBronze());  // the grip
      // The string: a V from the tips to the nock, `pull` blocks back.
      const int str = 6 - (int)std::lround((float)pull * (1.0f - std::fabs(u)));
      px(str, r, ui::ColParch());
    }
    if (nocked) {
      // The arrow on the string, its head past the stave.
      const int nock = 6 - pull;
      for (int c = nock; c <= 12; c++) px(c, mid, ui::ColGoldPale());
      px(13 - 1, mid - 1, full ? ui::ColEmber() : ui::ColSteel());
      px(13 - 1, mid + 1, full ? ui::ColEmber() : ui::ColSteel());
      px(12, mid, full ? ui::ColEmber() : ui::ColSteel());
      px(nock - 1 < 0 ? 0 : nock - 1, mid - 1, ui::ColBloodHi());  // fletching
      px(nock - 1 < 0 ? 0 : nock - 1, mid + 1, ui::ColBloodHi());
    }
    // The draw as a bar, with the least-draw notch.
    const float by = y0 + rows * b + 4.0f;
    d->AddRectFilled(ImVec2(x0, by), ImVec2(x0 + barW, by + barH), ui::ColDeep());
    d->AddRectFilled(ImVec2(x0, by), ImVec2(x0 + std::floor(barW * draw / b) * b, by + barH),
                     full ? ui::ColEmber() : ui::ColGold());
    const float nx = x0 + std::floor(barW * std::clamp(s.bowMinDraw, 0.0f, 1.0f) / b) * b;
    d->AddRectFilled(ImVec2(nx, by - 2), ImVec2(nx + b, by + barH + 2), ui::ColParchDim());
    const float tx = x0 + glyphW + 8.0f;
    const float ty = y0 + std::floor((rows * b - cs.y) * 0.5f);
    d->AddText(ImVec2(tx + 1, ty + 1), IM_COL32(0, 0, 0, 190), count);
    d->AddText(ImVec2(tx, ty), nocked ? ui::ColGoldPale() : ui::ColBloodHi(), count);
  }
  // APPLY MODE (TB_APPLY): say it is on, and whose skin LMB would brush.
  if (s.applyShown) {
    const std::string t = s.applyTarget.empty() ? std::string("apply - aim at a body")
                                                : "apply to " + s.applyTarget;
    py += tab(t.c_str(), py, ui::ColBronze(), ui::ColGoldPale(), 0.95f);
  }
  if (!s.lookPrompt.empty())
    py += tab(s.lookPrompt.c_str(), py, ui::ColGoldDim(), ui::ColParch(), 0.95f);
  if (!s.kitMessage.empty() && s.kitMessageAge < 2.5f) {
    const float a = std::clamp(1.6f - s.kitMessageAge * 0.7f, 0.0f, 1.0f);
    tab(s.kitMessage.c_str(), py, ui::ColEmber(), ui::ColEmber(), a);
  }
  ImGui::PopFont();
  // The hotbar along the bottom centre (ui/inventory_ui.h): what is in your
  // hand and what the number row reaches.
  DrawHudHotbar(s, d);
}

// ---- the body-condition PAPER DOLL ------------------------------------------
//
// A panel as wide as the health bar it sits on. Left: the body as a pixel
// grid (UIState::kDollRect), every limb filled from its OWN voxels, so the
// figure is not a diagram of the body but a small picture of it — a forearm
// bored through has a hole where the bolt went, a charred thigh is black where
// it is charred, a burning hand is on fire. Right: only what is urgent, and
// only while it is. One word for the worst thing happening, a pixel icon per
// condition with a pip per limb it is happening to, the coat as a strip of
// its colours, and a heart trace whose rate and colour follow the hp pool.
// Numbers live in the character screen's health view; this is the glance.
//
// Per cell, in the order they paint (later layers win, because they are
// later in the story of the limb and the louder alarm):
//   base     hp tint of the slot (parchment -> ochre -> cinnabar), shaded as
//            if lit from the upper left, darkened as the column hollows out
//   char     toward soot, by how cooked/charred the column is
//   rot      toward the infection's own colour
//   bruise   toward a contusion purple; split skin toward blood
//   coat     toward the substance on it, per cell, so a spray reads as a spray
//   fire     a stepped flicker of flame colours, plus rising embers
//   flash    white for a moment when the slot just lost hp
// A column with less than ~15% of its matter left is a HOLE and draws as a
// dim checker of what was there; a severed limb draws its whole silhouette
// that way in dried blood, so a lost arm is visibly an absence and not an
// arm the HUD forgot. Bleeding limbs get a pulsing red rim and falling drops;
// burning limbs a flickering ember rim with a glow ring outside it.
//
// Everything that moves runs on the wall clock (ImGui::GetTime) and is
// stepped at a sprite-like rate, never smooth, so it stays pixel art.
namespace {

uint32_t DollHash(uint32_t a, uint32_t b = 0, uint32_t c = 0) {
  uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return h;
}
float DollRand(uint32_t a, uint32_t b = 0, uint32_t c = 0) {
  return (float)(DollHash(a, b, c) & 0xFFFFu) / 65535.0f;
}

// A 2x pixel icon from a little bitmap: '.' clear, anything else looked up
// in `pal` by character. Drawn at `px` screen pixels per art pixel.
struct IconInk { char ch; ImU32 col; };
void DollIcon(ImDrawList* d, ImVec2 at, const char* const* rows, int nRows,
              const IconInk* pal, int nPal, float px) {
  for (int y = 0; y < nRows; y++)
    for (int x = 0; rows[y][x]; x++) {
      const char ch = rows[y][x];
      if (ch == '.') continue;
      for (int k = 0; k < nPal; k++)
        if (pal[k].ch == ch) {
          const ImVec2 a(at.x + x * px, at.y + y * px);
          d->AddRectFilled(a, ImVec2(a.x + px, a.y + px), pal[k].col);
          break;
        }
    }
}

const char* const kIconFlameA[8] = {"...#...", "..##...", "..###..", ".##o##.",
                                    ".#ooo#.", "##oYo##", "#ooYYo#", ".#ooo#."};
const char* const kIconFlameB[8] = {"....#..", "...##..", "..###..", ".##o##.",
                                    ".#oo##.", "##oYo##", "#oYYoo#", ".#ooo#."};
const char* const kIconDrop[8] = {"...#...", "...#...", "..###..", ".##o##.",
                                  "##oo###", "##o####", ".#####.", "..###.."};
const char* const kIconSkull[8] = {".#####.", "#######", "#kk#kk#", "#kk#kk#",
                                   "###k###", ".#####.", ".#k#k#.", "......."};
const char* const kIconSplat[8] = {"#..#...", "...#..#", ".####..", "######.",
                                   ".#####.", "..###.#", ".#..#..", "#......"};

}  // namespace

float Overlay::DrawBodyFigure(const UIState& s, float x, float yBottom, float width) {
  using DC = UIState::DollCell;
  constexpr int W = UIState::kDollW, H = UIState::kDollH;
  const float C = 2.0f;                       // screen px per doll cell
  const float pad = 6.0f;
  const float height = H * C + pad * 2.0f;
  const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
  const double now = ImGui::GetTime();
  // No body (fly mode despawns the rig): no panel, and no empty gap either.
  if (!s.bodyValid) {
    dollPrevValid_ = false;
    return 0.0f;
  }

  ImDrawList* d = ImGui::GetForegroundDrawList();
  const float top = std::floor(yBottom - height);
  x = std::floor(x);

  // ---- presentation memory: which slot just got hurt ----------------------
  for (int i = 0; i < UIState::kSlotCount; i++) {
    const UIState::BodyPartUI& b = s.body[i];
    dollFlash_[i] = std::max(0.0f, dollFlash_[i] - dt);
    if (dollPrevValid_ && b.present) {
      if (b.severed && !dollPrevSevered_[i]) dollFlash_[i] = 0.6f;
      else if (!b.severed && b.hp < dollPrevHp_[i] - 0.5f) dollFlash_[i] = 0.3f;
    }
    dollPrevHp_[i] = b.hp;
    dollPrevSevered_[i] = b.severed;
  }
  dollPrevValid_ = true;

  // ---- the panel ------------------------------------------------------------
  // Flat obsidian with a bronze rim and a 2 px ridge: the HUD bars' language,
  // zero rounding.
  d->AddRectFilled(ImVec2(x - 2, top - 2), ImVec2(x + width + 2, yBottom + 2),
                   IM_COL32(0, 0, 0, 110));
  d->AddRectFilled(ImVec2(x, top), ImVec2(x + width, yBottom), IM_COL32(18, 20, 28, 200));
  d->AddRectFilled(ImVec2(x, top), ImVec2(x + width, top + 2), ui::Fade(ui::ColHi(), 0.8f));
  d->AddRect(ImVec2(x, top), ImVec2(x + width, yBottom), ui::Fade(ui::ColBronze(), 0.9f),
             0.0f, 0, 2.0f);

  const float ox = x + 8.0f, oy = top + pad;   // doll origin
  // A faint floor line under the feet, so the figure stands on something.
  d->AddRectFilled(ImVec2(ox + 4, oy + H * C), ImVec2(ox + W * C - 4, oy + H * C + 2),
                   ui::Fade(ui::ColBronze(), 0.5f));

  // ---- composite the doll into a cell buffer --------------------------------
  // col = 0 means empty. ghost marks a hole/severed cell (drawn as a checker
  // and NOT counted as filled by the rim pass). owner = which slot painted it.
  ImU32 col[W * H] = {};
  ImU32 ghost[W * H] = {};
  int8_t owner[W * H];
  std::memset(owner, -1, sizeof owner);
  const float pulse = 0.5f + 0.5f * (float)std::sin(now * 7.0);
  const uint32_t flick = (uint32_t)(now * 12.0);   // sprite-rate flame step

  auto hpTint = [](float f) {
    f = std::clamp(f, 0.0f, 1.0f);
    const ImU32 hi = IM_COL32(214, 200, 160, 255);   // parchment, well
    const ImU32 mid = IM_COL32(206, 138, 70, 255);   // ochre, hurt
    const ImU32 lo = IM_COL32(150, 38, 36, 255);     // cinnabar, failing
    return f > 0.5f ? ui::Mix(mid, hi, (f - 0.5f) * 2.0f) : ui::Mix(lo, mid, f * 2.0f);
  };
  auto scale = [](ImU32 c, float k) {
    const int r = std::min(255, (int)(((c >> IM_COL32_R_SHIFT) & 0xFF) * k));
    const int g = std::min(255, (int)(((c >> IM_COL32_G_SHIFT) & 0xFF) * k));
    const int b = std::min(255, (int)(((c >> IM_COL32_B_SHIFT) & 0xFF) * k));
    return IM_COL32(r, g, b, 255);
  };
  const ImU32 kFlame[4] = {IM_COL32(255, 226, 110, 255), IM_COL32(250, 160, 50, 255),
                           IM_COL32(226, 90, 34, 255), IM_COL32(255, 196, 70, 255)};

  bool slotBurning[UIState::kSlotCount] = {};
  int nBurning = 0, nBleeding = 0, nSevered = 0;
  uint32_t rotVox = 0, bodyVox = 0, rotBest = 0;
  ImU32 rotCol = IM_COL32(90, 160, 110, 255);
  const char* rotLabel = "infected";

  for (int i = 0; i < UIState::kSlotCount; i++) {
    const UIState::BodyPartUI& b = s.body[i];
    if (!b.present) continue;
    const UIState::DollRect& R = UIState::kDollRect[i];
    slotBurning[i] = !b.severed && b.burningVoxels > 0;
    nBurning += slotBurning[i];
    nBleeding += (!b.severed && b.bleeding);
    nSevered += b.severed;
    bodyVox += b.voxelTotal;
    rotVox += b.voxelRot;
    if (b.infectCount > 0 && b.infect[0].count > rotBest) {
      rotBest = b.infect[0].count;
      rotCol = b.infect[0].color | 0xFF000000u;
      if (b.infect[0].label[0]) rotLabel = b.infect[0].label;
    }
    const ImU32 infectCol = b.infectCount > 0 ? (b.infect[0].color | 0xFF000000u) : rotCol;
    const ImU32 base = hpTint(b.hpFrac);
    const float flash = std::min(1.0f, dollFlash_[i] / 0.3f);
    for (int cy = 0; cy < R.h; cy++)
      for (int cx = 0; cx < R.w; cx++) {
        DC c;
        if (b.dollValid) {
          c = b.doll[cy * R.w + cx];
        } else {
          c.had = 1;
          c.intact = b.severed ? 0 : 255;
        }
        if (!c.had) continue;
        const int gx = R.x + cx, gy = R.y + cy;
        const int gi = gy * W + gx;
        if (b.severed || c.intact < 40) {
          // An absence: the severed limb's outline in dried blood, a hole in
          // a living limb as a dimmer ghost of the flesh it was.
          if (((gx + gy) & 1) == 0)
            ghost[gi] = b.severed ? ui::Mix(ui::Fade(ui::ColBlood(), 0.55f),
                                            ui::Fade(IM_COL32_WHITE, 0.7f), flash * 0.6f)
                                  : ui::Fade(ui::ColHi(), 0.55f);
          continue;
        }
        // Lit from the upper left, darker as the column hollows.
        const float t = R.w > 1 ? (float)cx / (float)(R.w - 1) : 0.5f;
        float k = 1.08f - 0.24f * t - (cy == 0 ? -0.06f : 0.0f);
        k *= 0.55f + 0.45f * (c.intact / 255.0f);
        ImU32 p = scale(base, k);
        if (c.charred) p = ui::Mix(p, IM_COL32(40, 33, 32, 255), std::min(1.0f, c.charred / 255.0f * 1.15f));
        if (c.rot) p = ui::Mix(p, infectCol, std::min(0.9f, c.rot / 255.0f * 1.2f));
        if (c.bruise) {
          const float bz = c.bruise / 255.0f;
          p = ui::Mix(p, bz > 0.75f ? ui::ColBlood() : IM_COL32(104, 54, 120, 255),
                      std::min(0.75f, bz * 1.1f));
        }
        if (c.stain && b.stainColor != 0)
          p = ui::Mix(p, b.stainColor | 0xFF000000u, std::min(0.85f, c.stain / 255.0f * 1.3f));
        if (c.hot) {
          const ImU32 fl = kFlame[DollHash((uint32_t)gi, flick) & 3u];
          p = ui::Mix(p, fl, std::min(1.0f, c.hot / 255.0f * 3.0f));
        }
        if (b.bleeding && !b.severed && c.bruise > 190)
          p = ui::Mix(p, ui::ColBloodHi(), pulse * 0.6f);
        if (flash > 0.0f) p = ui::Mix(p, IM_COL32(255, 250, 240, 255), flash * 0.85f);
        col[gi] = p;
        owner[gi] = (int8_t)i;
      }
  }

  // ---- the rim: an outline cell around every filled cell ---------------------
  // Ink by default; ember (flickering) for a burning limb, pulsing blood for a
  // bleeding one. A burning limb also gets a faint glow ring one cell further.
  ImU32 rim[W * H] = {};
  auto filled = [&](int gx, int gy) {
    return gx >= 0 && gy >= 0 && gx < W && gy < H && col[gy * W + gx] != 0;
  };
  for (int gy = 0; gy < H; gy++)
    for (int gx = 0; gx < W; gx++) {
      const int gi = gy * W + gx;
      if (col[gi]) continue;
      int nb = -1;
      const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
      for (int k = 0; k < 4 && nb < 0; k++)
        if (filled(gx + dx[k], gy + dy[k])) nb = owner[(gy + dy[k]) * W + gx + dx[k]];
      if (nb < 0) continue;
      const UIState::BodyPartUI& b = s.body[nb];
      if (slotBurning[nb])
        rim[gi] = ui::Mix(ui::ColEmber(), IM_COL32(255, 210, 90, 255),
                          (DollHash((uint32_t)gi, flick) & 1u) ? 0.6f : 0.0f);
      else if (b.bleeding)
        rim[gi] = ui::Mix(ui::ColBlood(), IM_COL32(255, 70, 60, 255), pulse);
      else
        rim[gi] = ui::ColInk();
    }
  for (int gy = 0; gy < H; gy++)
    for (int gx = 0; gx < W; gx++) {
      const int gi = gy * W + gx;
      if (col[gi] || rim[gi]) continue;
      for (int ddy = -2; ddy <= 2 && !rim[gi]; ddy++)
        for (int ddx = -2; ddx <= 2; ddx++) {
          if (!filled(gx + ddx, gy + ddy)) continue;
          const int nb = owner[(gy + ddy) * W + gx + ddx];
          if (nb >= 0 && slotBurning[nb] && ((gx + gy + (int)flick) % 3) != 0) {
            rim[gi] = ui::Fade(ui::ColEmber(), 0.35f);
            break;
          }
        }
    }

  // ---- draw, merging runs of one colour per row -----------------------------
  auto drawRow = [&](const ImU32* buf, int gy) {
    for (int gx = 0; gx < W;) {
      const ImU32 c = buf[gy * W + gx];
      int e = gx + 1;
      while (e < W && buf[gy * W + e] == c) e++;
      if (c)
        d->AddRectFilled(ImVec2(ox + gx * C, oy + gy * C), ImVec2(ox + e * C, oy + (gy + 1) * C), c);
      gx = e;
    }
  };
  for (int gy = 0; gy < H; gy++) {
    drawRow(ghost, gy);
    drawRow(rim, gy);
    drawRow(col, gy);
  }

  // ---- embers rising off burning cells, drops falling off bleeding limbs ----
  for (int i = 0; i < UIState::kSlotCount; i++) {
    const UIState::BodyPartUI& b = s.body[i];
    if (!b.present || b.severed) continue;
    const UIState::DollRect& R = UIState::kDollRect[i];
    if (slotBurning[i]) {
      // The cells actually burning, so the embers leave from the fire.
      int hotCells[UIState::kDollCellsMax];
      int nHot = 0;
      for (int c = 0; c < R.w * R.h; c++)
        if (b.dollValid ? b.doll[c].hot > 30 : c < R.w) hotCells[nHot++] = c;
      if (nHot == 0)
        for (int c = 0; c < R.w; c++) hotCells[nHot++] = c;
      const int n = std::min(7, 2 + nHot / 5);
      for (int j = 0; j < n; j++) {
        const float period = 0.55f + 0.45f * DollRand((uint32_t)i, (uint32_t)j, 1);
        const double u = now / period + DollRand((uint32_t)i, (uint32_t)j, 2);
        const uint32_t cycle = (uint32_t)u;
        const float ph = (float)(u - (double)cycle);
        const int cell = hotCells[DollHash((uint32_t)i, (uint32_t)j, cycle) % (uint32_t)nHot];
        const float sx = ox + (R.x + cell % R.w) * C;
        const float sy = oy + (R.y + cell / R.w) * C;
        const float wob = std::floor(std::sin((float)u * 9.0f + j) * 1.5f) * C;
        const float px = std::floor(sx + wob);
        const float pyq = std::floor((sy - ph * 26.0f) / C) * C;
        ImU32 ec = ph < 0.25f ? kFlame[0] : ph < 0.55f ? kFlame[1] : ph < 0.8f ? kFlame[2]
                                                                                : IM_COL32(110, 104, 100, 255);
        ec = ui::Fade(ec, ph < 0.8f ? 1.0f : (1.0f - ph) * 4.0f);
        const float sz = ph < 0.3f ? C * 2.0f : C;
        d->AddRectFilled(ImVec2(px, pyq), ImVec2(px + sz, pyq + sz), ec);
      }
    }
    if (b.bleeding) {
      // Drops leave the lowest row the limb still has matter in.
      int lowRow = R.h - 1;
      if (b.dollValid)
        for (; lowRow > 0; lowRow--) {
          bool any = false;
          for (int cx = 0; cx < R.w; cx++) {
            const DC& c = b.doll[lowRow * R.w + cx];
            any |= c.had && c.intact >= 40;
          }
          if (any) break;
        }
      for (int j = 0; j < 2; j++) {
        const float period = 0.8f + 0.3f * DollRand((uint32_t)i, (uint32_t)j, 7);
        const double u = now / period + 0.5 * j;
        const uint32_t cycle = (uint32_t)u;
        const float ph = (float)(u - (double)cycle);
        const int cx = (int)(DollHash((uint32_t)i, (uint32_t)j, cycle) % (uint32_t)R.w);
        const float px = ox + (R.x + cx) * C;
        const float py = std::floor((oy + (R.y + lowRow + 1) * C + ph * ph * 30.0f) / C) * C;
        if (py > oy + H * C) continue;
        const ImU32 bc = ui::Fade(IM_COL32(200, 40, 36, 255), ph < 0.7f ? 1.0f : (1.0f - ph) / 0.3f);
        d->AddRectFilled(ImVec2(px, py), ImVec2(px + C, py + C * (ph < 0.5f ? 2.0f : 1.0f)), bc);
      }
    }
  }

  // ---- the right column: only what is urgent --------------------------------
  const float sx = ox + W * C + 12.0f;
  const float sw = x + width - 8.0f - sx;
  float sy = oy;
  const float hpF = s.healthMax > 0 ? std::clamp((float)s.health / (float)s.healthMax, 0.0f, 1.0f)
                                    : 1.0f;

  // The worst thing happening, as one word. Priority is "what kills you
  // fastest and can you do something about it": fire, then blood, then the
  // infection, then a lost limb, then simply being low.
  {
    char word[24] = "";
    ImU32 wc = ui::ColGoldPale();
    bool throb = false;
    if (!s.playerAlive) {
      // The DEAD banner above the panel says it; the trace flatlines.
    } else if (nBurning > 0) {
      std::snprintf(word, sizeof word, "ON FIRE");
      wc = (flick & 1u) ? IM_COL32(255, 200, 80, 255) : ui::ColEmber();
    } else if (nBleeding > 0) {
      std::snprintf(word, sizeof word, "BLEEDING");
      wc = ui::Mix(ui::ColBlood(), IM_COL32(255, 80, 70, 255), pulse);
      throb = true;
    } else if (rotVox > 0) {
      int k = 0;
      for (const char* c = rotLabel; *c && k < (int)sizeof word - 1; c++)
        word[k++] = (char)std::toupper((unsigned char)*c);
      word[k] = 0;
      wc = ui::Mix(rotCol, IM_COL32_WHITE, 0.35f);
    } else if (nSevered > 0) {
      std::snprintf(word, sizeof word, "MAIMED");
      wc = ui::ColBloodHi();
    } else if (hpF < 0.3f) {
      std::snprintf(word, sizeof word, "CRITICAL");
      wc = ui::Mix(ui::ColBloodHi(), IM_COL32(255, 120, 100, 255), pulse);
      throb = true;
    }
    if (word[0]) {
      // The 26 px chrome font, with a whole-pixel 2 px shadow to match it.
      ImGui::PushFont(ui::FontLarge());
      const float jx = throb && pulse > 0.85f ? 2.0f : 0.0f;
      const ImVec2 at(std::floor(sx + jx), std::floor(sy - 4.0f));
      d->AddText(ImVec2(at.x + 2, at.y + 2), ui::Fade(ui::ColInk(), 0.9f), word);
      d->AddText(at, wc, word);
      ImGui::PopFont();
    }
    sy += 30.0f;
  }

  // Condition chips: an icon, then a pip per limb it is happening to.
  const float ipx = 2.0f;   // icon art pixel
  auto pips = [&](float px, float py, int n, ImU32 c) {
    for (int k = 0; k < std::min(n, 8); k++) {
      const float a = px + k * 7.0f;
      d->AddRectFilled(ImVec2(a, py + 3), ImVec2(a + 5, py + 13), ui::ColInk());
      d->AddRectFilled(ImVec2(a + 1, py + 4), ImVec2(a + 4, py + 12), c);
      d->AddRectFilled(ImVec2(a + 1, py + 4), ImVec2(a + 4, py + 6), ui::Mix(c, IM_COL32_WHITE, 0.4f));
    }
  };
  if (nBurning > 0) {
    const IconInk pal[3] = {{'#', IM_COL32(214, 64, 30, 255)}, {'o', ui::ColEmber()},
                            {'Y', IM_COL32(255, 228, 120, 255)}};
    DollIcon(d, ImVec2(sx, sy), (flick & 2u) ? kIconFlameB : kIconFlameA, 8, pal, 3, ipx);
    pips(sx + 20.0f, sy, nBurning, (flick & 1u) ? IM_COL32(255, 200, 80, 255) : ui::ColEmber());
    sy += 20.0f;
  }
  if (nBleeding > 0) {
    const IconInk pal[2] = {{'#', IM_COL32(176, 30, 30, 255)}, {'o', IM_COL32(240, 120, 110, 255)}};
    const float drip = pulse > 0.5f ? ipx : 0.0f;
    DollIcon(d, ImVec2(sx, sy + drip), kIconDrop, 8, pal, 2, ipx);
    pips(sx + 20.0f, sy, nBleeding, ui::Mix(ui::ColBlood(), IM_COL32(255, 80, 70, 255), pulse));
    sy += 20.0f;
  }
  if (rotVox > 0 && bodyVox > 0) {
    const IconInk pal[2] = {{'#', ui::Mix(rotCol, IM_COL32_WHITE, 0.2f)}, {'k', ui::ColInk()}};
    DollIcon(d, ImVec2(sx, sy), kIconSkull, 8, pal, 2, ipx);
    // How much of the body it has turned, on a short gauge.
    const float gw = std::min(84.0f, sw - 22.0f);
    const float f = std::clamp((float)rotVox / (float)bodyVox * 2.0f, 0.04f, 1.0f);
    d->AddRectFilled(ImVec2(sx + 20, sy + 4), ImVec2(sx + 20 + gw, sy + 12), ui::ColInk());
    d->AddRectFilled(ImVec2(sx + 21, sy + 5), ImVec2(sx + 21 + std::floor((gw - 2) * f / 2) * 2, sy + 11),
                     rotCol);
    sy += 20.0f;
  }
  // The coat: what is ON you, as a strip of its colours sized by share, and
  // the heaviest one named. Silent below tune.coat.hudMinFrac, so a clean
  // player's panel says nothing about it at all.
  if (s.stainFrac >= s.stainHudMin && s.stainColor != 0) {
    const ImU32 top = s.stainColor | 0xFF000000u;
    const IconInk pal[1] = {{'#', ui::Mix(top, IM_COL32_WHITE, 0.2f)}};
    DollIcon(d, ImVec2(sx, sy), kIconSplat, 8, pal, 1, ipx);
    const float gw = 48.0f;
    d->AddRectFilled(ImVec2(sx + 20, sy + 4), ImVec2(sx + 20 + gw, sy + 12), ui::ColInk());
    // Length is HOW MUCH (square-rooted, so a light splash is still a visible
    // bar rather than two pixels), split by WHAT in proportion.
    const float inner = gw - 2.0f;
    const float len = std::max(6.0f, std::floor(inner * std::sqrt(std::min(1.0f, s.stainFrac)) / 2) * 2);
    float cx = sx + 21;
    const float end = cx + len;
    float sum = 0.0f;
    for (int k = 0; k < s.coatCount; k++)
      if (s.coats[k].color != 0) sum += s.coats[k].frac;
    for (int k = 0; k < s.coatCount && sum > 0.0f; k++) {
      const UIState::CoatName& c = s.coats[k];
      if (c.color == 0 || c.frac < 0.005f) continue;
      const float e = std::min(end, cx + std::max(2.0f, std::round(len * c.frac / sum / 2) * 2));
      if (e > cx) d->AddRectFilled(ImVec2(cx, sy + 5), ImVec2(e, sy + 11), c.color | 0xFF000000u);
      cx = e;
    }
    if (cx < end) d->AddRectFilled(ImVec2(cx, sy + 5), ImVec2(end, sy + 11), top);
    const char* lbl = s.stainLabel[0] ? s.stainLabel : "";
    if (lbl[0] && sx + 20 + gw + 6 < x + width - 8)
      ui::ShadowText(d, ImVec2(sx + 20 + gw + 6, sy + 1), ui::Mix(top, IM_COL32_WHITE, 0.45f), lbl);
    sy += 20.0f;
  }

  // ---- the heart trace ------------------------------------------------------
  // A monitor sweep, not a scroll: a write head crosses the strip leaving the
  // trace behind it, older columns dimmer, a blank gap ahead of the head. The
  // rate climbs as the hp pool empties and with bleeding or fire, the colour
  // walks green -> amber -> red, and a dead body flatlines.
  {
    const float bpm = !s.playerAlive ? 0.0f
                      : std::min(180.0f, 62.0f + (1.0f - hpF) * 84.0f + (nBleeding ? 22.0f : 0.0f) +
                                             (nBurning ? 18.0f : 0.0f));
    auto wave = [](float p) {
      auto g = [](float p, float c, float w, float a) {
        const float z = (p - c) / w;
        return a * std::exp(-z * z);
      };
      return g(p, 0.10f, 0.025f, 0.12f) + g(p, 0.185f, 0.008f, -0.18f) + g(p, 0.21f, 0.010f, 1.0f) +
             g(p, 0.235f, 0.009f, -0.32f) + g(p, 0.42f, 0.040f, 0.24f);
    };
    const float eh = 30.0f;
    const float ey = oy + H * C - eh;
    const float ew = std::clamp(std::floor(sw / 2) * 2, 16.0f, (float)kEcgCols * 2.0f);
    const int cols = (int)(ew / 2.0f);   // the ring is this long; <= kEcgCols
    ecgHead_ %= cols;
    const float period = 1.0f / 40.0f;
    ecgAcc_ += dt;
    while (ecgAcc_ >= period) {
      ecgAcc_ -= period;
      float v = 0.0f;
      for (int k = 0; k < 6; k++) {
        const float w = wave(ecgPhase_ + bpm / 60.0f * period * (k / 6.0f));
        if (std::fabs(w) > std::fabs(v)) v = w;
      }
      ecgPhase_ += bpm / 60.0f * period;
      ecgPhase_ -= std::floor(ecgPhase_);
      ecg_[ecgHead_] = v;
      ecgHead_ = (ecgHead_ + 1) % cols;
    }
    d->AddRectFilled(ImVec2(sx - 2, ey - 2), ImVec2(sx + ew + 2, ey + eh + 2), ui::ColInk());
    d->AddRectFilled(ImVec2(sx, ey), ImVec2(sx + ew, ey + eh), IM_COL32(12, 22, 18, 230));
    for (float gx = sx + 8; gx < sx + ew; gx += 16)   // the graticule, dotted
      for (float gy = ey + 4; gy < ey + eh; gy += 8)
        d->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + 2, gy + 2), IM_COL32(40, 70, 52, 160));
    const ImU32 hiC = IM_COL32(110, 226, 140, 255), midC = IM_COL32(232, 180, 64, 255),
                loC = IM_COL32(236, 66, 56, 255);
    const ImU32 trace = !s.playerAlive ? IM_COL32(120, 120, 120, 255)
                        : hpF > 0.5f   ? ui::Mix(midC, hiC, (hpF - 0.5f) * 2.0f)
                                       : ui::Mix(loC, midC, hpF * 2.0f);
    const float mid = ey + std::floor(eh * 0.62f / 2) * 2;
    auto yAt = [&](float v) { return std::floor((mid - v * (eh * 0.55f)) / 2) * 2; };
    // A sweep: screen column k IS ring slot k, and its age is how far behind
    // the write head it sits.
    for (int k = 0; k < cols; k++) {
      const int idx = k;
      const int age = ((ecgHead_ - 1 - idx) % cols + cols) % cols;
      if (age >= cols - 3) continue;   // the blank gap ahead of the head
      const int prev = (idx + cols - 1) % cols;
      float y0 = yAt(ecg_[idx]);
      float y1 = k > 0 && age + 1 < cols - 3 ? yAt(ecg_[prev]) : y0;
      if (y1 < y0) std::swap(y0, y1);
      const float a = std::clamp(1.0f - (float)age / (float)cols, 0.2f, 1.0f);
      const float px = sx + k * 2.0f;
      d->AddRectFilled(ImVec2(px, y0), ImVec2(px + 2, y1 + 2), ui::Fade(trace, a));
      if (age == 0)
        d->AddRectFilled(ImVec2(px - 1, y0 - 1), ImVec2(px + 3, y1 + 3), ui::Fade(IM_COL32_WHITE, 0.8f));
    }
  }
  return height;
}

// ============================================================================
// THE F1 DEV PANEL: a full-height sidebar on the left edge
// ============================================================================
//
// It used to be one floating window with everything in it, top to bottom, in
// the order the features were written. It is now a map-editor-style sidebar:
// a fixed header (the numbers you glance at, pause/step, play/dev), a strip of
// PAGES, and one scrolling body per page, each cut into titled sections.
//
//   Paint  - the tool, the brush, and WHAT it paints (the material picker)
//   Spawn  - everything that puts a thing in the world or in your hands:
//            filled vessels, creatures, clothes, objects, and the cleanup
//   World  - time, sky, weather, wind, and the world file
//   View   - render and debug-draw switches, the player's movement
//   Magic  - the mana/health crossover and the spell being held
//   Debug  - the full stat block, the NPC log, the tuning windows, the keys
//
// THE OVERLAY STILL OWNS NO GAME STATE. Every control here writes a UIState
// field or raises a one-shot bool that main.cpp / session.cpp consume, exactly
// as before; only the arrangement moved.

namespace {

ImVec4 V4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// 0xAABBGGRR (a material's gpu colour) is ImU32's own byte order, so the only
// conversion is forcing it opaque: a gas's authored alpha is for the renderer.
ImU32 Opaque(uint32_t c) { return (c & 0x00FFFFFFu) | 0xFF000000u; }

// A titled, collapsible section. Bronze band, gold title, so the sections read
// as sections at a glance rather than as one more line of text.
bool Section(const char* title, bool defaultOpen = true) {
  ImGui::PushStyleColor(ImGuiCol_Header, V4(ui::ColMid()));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, V4(ui::ColHi()));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, V4(ui::ColHi()));
  ImGui::PushStyleColor(ImGuiCol_Text, V4(ui::ColGoldPale()));
  const bool open = ImGui::CollapsingHeader(
      title, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
  ImGui::PopStyleColor(4);
  if (open) ImGui::Spacing();
  return open;
}

// A small gold caption inside a section.
void Caption(const char* text) {
  ImGui::TextColored(V4(ui::ColGoldDim()), "%s", text);
}

// A toggle-look button: gold when `on`. Width 0 = fit the label.
bool ToggleButton(const char* label, bool on, float w = 0.0f, float h = 0.0f) {
  if (on) {
    ImGui::PushStyleColor(ImGuiCol_Button, V4(ui::ColGold()));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V4(ui::ColGoldHi()));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, V4(ui::ColGoldPale()));
    ImGui::PushStyleColor(ImGuiCol_Text, V4(ui::ColInk()));
  }
  const bool hit = ImGui::Button(label, ImVec2(w, h));
  if (on) ImGui::PopStyleColor(4);
  return hit;
}

// Width of one cell when `n` buttons share the current row.
float CellWidth(int n) {
  const float sp = ImGui::GetStyle().ItemSpacing.x;
  return std::floor((ImGui::GetContentRegionAvail().x - sp * (n - 1)) / n);
}

// ---- material icons and the picker -----------------------------------------

// The icon: a 2x2 of the material's three palette variants — the same jitter
// the world draws it with — in a dark rim, on whole pixels.
void MatIcon(ImDrawList* d, ImVec2 p, float size, const UIState& s, int id) {
  p = ImVec2(std::floor(p.x), std::floor(p.y));
  const float h = std::floor(size * 0.5f);
  auto col = [&](const std::vector<uint32_t>& v) {
    return id >= 0 && id < (int)v.size() ? Opaque(v[id]) : ui::ColMid();
  };
  const ImU32 c0 = col(s.materialColors);
  const ImU32 c1 = s.materialColors1.empty() ? c0 : col(s.materialColors1);
  const ImU32 c2 = s.materialColors2.empty() ? c0 : col(s.materialColors2);
  d->AddRectFilled(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + 2 * h + 1, p.y + 2 * h + 1),
                   ui::ColInk());
  d->AddRectFilled(p, ImVec2(p.x + h, p.y + h), c0);
  d->AddRectFilled(ImVec2(p.x + h, p.y), ImVec2(p.x + 2 * h, p.y + h), c1);
  d->AddRectFilled(ImVec2(p.x, p.y + h), ImVec2(p.x + h, p.y + 2 * h), c2);
  d->AddRectFilled(ImVec2(p.x + h, p.y + h), ImVec2(p.x + 2 * h, p.y + 2 * h), c0);
}

int MatClass(const UIState& s, int id) {
  return id >= 0 && id < (int)s.materialClass.size() ? s.materialClass[id] : 0;
}

bool HasTag(const std::string& tags, const char* t) {
  const size_t n = std::strlen(t);
  for (size_t a = 0; a <= tags.size();) {
    size_t b = tags.find(',', a);
    if (b == std::string::npos) b = tags.size();
    if (b - a == n && tags.compare(a, n, t) == 0) return true;
    a = b + 1;
  }
  return false;
}

const std::string& MatTags(const UIState& s, int id) {
  static const std::string kNone;
  return id >= 0 && id < (int)s.materialTags.size() ? s.materialTags[id] : kNone;
}

// "pine_needles" -> "pine needles": the picker shows words, not identifiers.
std::string MatLabel(const UIState& s, int id) {
  if (id < 0 || id >= (int)s.materialNames.size()) return "?";
  std::string n = s.materialNames[id];
  std::replace(n.begin(), n.end(), '_', ' ');
  return n;
}

const char* const kClassName[4] = {"Solids", "Powders", "Liquids", "Gases"};
const char* const kClassWord[4] = {"solid", "powder", "liquid", "gas"};
ImU32 ClassColor(int c) {
  switch (c) {
    case 1: return ui::ColGold();
    case 2: return IM_COL32(110, 160, 230, 255);
    case 3: return IM_COL32(170, 170, 190, 255);
    default: return ui::ColSteel();
  }
}

// The solids column is 120 long, so it is cut by tag into groups. Order is
// the order drawn; the ground-building materials first.
const char* const kSolidGroup[5] = {"stone & metal", "plants", "organic", "hot",
                                    "other"};
int SolidGroup(const std::string& tags) {
  if (HasTag(tags, "hot")) return 3;
  if (HasTag(tags, "mineral") || HasTag(tags, "metal")) return 0;
  if (HasTag(tags, "foliage")) return 1;
  if (HasTag(tags, "organic")) return 2;
  return 4;
}

void MatTooltip(const UIState& s, int id) {
  if (!ImGui::BeginTooltip()) return;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  MatIcon(ImGui::GetWindowDrawList(), p, 32.0f, s, id);
  ImGui::Dummy(ImVec2(34, 34));
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextColored(V4(ui::ColGoldPale()), "%s", MatLabel(s, id).c_str());
  ImGui::TextColored(V4(ClassColor(MatClass(s, id))), "%s", kClassWord[MatClass(s, id) & 3]);
  ImGui::SameLine();
  ImGui::TextDisabled("id %d", id);
  const std::string& tags = MatTags(s, id);
  if (!tags.empty()) ImGui::TextDisabled("%s", tags.c_str());
  ImGui::EndGroup();
  ImGui::EndTooltip();
}

// One row of a picker column: [icon] name. Returns true on click.
bool MatRow(const UIState& s, int id, bool selected, float rowH) {
  ImGui::PushID(id);
  const bool hit = ImGui::Selectable("##m", selected, 0, ImVec2(0, rowH));
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* d = ImGui::GetWindowDrawList();
  const float icon = 12.0f;
  MatIcon(d, ImVec2(a.x + 3, a.y + std::floor((rowH - icon) * 0.5f)), icon, s, id);
  const std::string lab = MatLabel(s, id);
  const float ty = a.y + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f);
  d->PushClipRect(ImVec2(a.x + icon + 7, a.y), b, true);
  d->AddText(ImVec2(a.x + icon + 8, ty),
             selected ? ui::ColGoldPale() : ui::ColParch(), lab.c_str());
  d->PopClipRect();
  if (selected) d->AddRect(a, b, ui::ColGold());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) MatTooltip(s, id);
  ImGui::PopID();
  return hit;
}

// THE MATERIAL PICKER. `ids` is the candidate list (every material for the
// brush; only what the vessel holds for a flask or a pouch). One column per
// class present — solids / powders / liquids / gases — each scrolling on its
// own, with a search box over all of them. Returns true when `selected`
// changed.
bool MaterialPicker(const UIState& s, const char* id, const std::vector<int>& ids,
                    int& selected, char* search, size_t searchN,
                    float maxH = 300.0f) {
  bool changed = false;
  ImGui::PushID(id);
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputTextWithHint("##search", "search...", search, searchN);
  std::string q = search;
  for (char& c : q) c = (char)std::tolower((unsigned char)c);
  std::replace(q.begin(), q.end(), '_', ' ');

  std::vector<int> cols[4];
  for (int m : ids) {
    if (m <= 0 || m >= (int)s.materialNames.size()) continue;
    if (!q.empty()) {
      std::string l = MatLabel(s, m);
      for (char& c : l) c = (char)std::tolower((unsigned char)c);
      if (l.find(q) == std::string::npos) continue;
    }
    cols[MatClass(s, m) & 3].push_back(m);
  }
  // Solids by group, then id (id order is roughly "most basic first").
  std::stable_sort(cols[0].begin(), cols[0].end(), [&](int a, int b) {
    return SolidGroup(MatTags(s, a)) < SolidGroup(MatTags(s, b));
  });

  int nCols = 0, maxRows = 0;
  for (int c = 0; c < 4; c++) {
    if (cols[c].empty()) continue;
    nCols++;
    int rows = (int)cols[c].size();
    if (c == 0 && q.empty()) {
      int last = -1;
      for (int m : cols[0]) {
        const int g = SolidGroup(MatTags(s, m));
        if (g != last) rows++;
        last = g;
      }
    }
    maxRows = std::max(maxRows, rows);
  }
  if (nCols == 0) {
    ImGui::TextDisabled(ids.empty() ? "nothing to pick" : "nothing matches");
    ImGui::PopID();
    return false;
  }
  const float rowH = ImGui::GetTextLineHeight() + 4.0f;
  const float rowStep = rowH + ImGui::GetStyle().ItemSpacing.y;
  const float h = std::min(maxH, maxRows * rowStep + 10.0f);

  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
  if (ImGui::BeginTable("##cols", nCols,
                        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
    for (int c = 0; c < 4; c++) {
      if (cols[c].empty()) continue;
      ImGui::TableNextColumn();
      ImGui::TextColored(V4(ClassColor(c)), "%s", kClassName[c]);
      ImGui::SameLine();
      ImGui::TextDisabled("%d", (int)cols[c].size());
      const ImVec2 a = ImGui::GetCursorScreenPos();
      ImGui::GetWindowDrawList()->AddLine(
          a, ImVec2(a.x + ImGui::GetContentRegionAvail().x, a.y), ClassColor(c), 2.0f);
      ImGui::Dummy(ImVec2(0, 2));
    }
    ImGui::TableNextRow();
    for (int c = 0; c < 4; c++) {
      if (cols[c].empty()) continue;
      ImGui::TableNextColumn();
      ImGui::PushID(c);
      ImGui::BeginChild("##col", ImVec2(0, h));
      int last = -1;
      for (int m : cols[c]) {
        if (c == 0 && q.empty()) {
          const int g = SolidGroup(MatTags(s, m));
          if (g != last) {
            if (last != -1) ImGui::Dummy(ImVec2(0, 3));
            ImGui::TextDisabled("%s", kSolidGroup[g]);
            last = g;
          }
        }
        if (MatRow(s, m, m == selected, rowH) && m != selected) {
          selected = m;
          changed = true;
        }
      }
      ImGui::EndChild();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  ImGui::PopID();
  return changed;
}

// The "this is what you have" line over a picker: a big icon and the name.
void CurrentMaterial(const UIState& s, int id, const char* what) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  MatIcon(ImGui::GetWindowDrawList(), ImVec2(p.x + 1, p.y + 1), 26.0f, s, id);
  ImGui::Dummy(ImVec2(28, 28));
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextDisabled("%s", what);
  ImGui::TextColored(V4(ui::ColGoldPale()), "%s", MatLabel(s, id).c_str());
  ImGui::SameLine();
  ImGui::TextColored(V4(ClassColor(MatClass(s, id))), "%s", kClassWord[MatClass(s, id) & 3]);
  ImGui::EndGroup();
}

int MatIdByName(const UIState& s, const std::string& name) {
  for (int i = 0; i < (int)s.materialNames.size(); i++)
    if (s.materialNames[i] == name) return i;
  return -1;
}

// A list of names as a short scrolling list of selectables (a combo hides
// what the choices are until clicked; a list shows them).
bool PickList(const char* id, const std::vector<std::string>& names, int& pick,
              int visibleRows = 6) {
  bool changed = false;
  const float rowH = ImGui::GetTextLineHeightWithSpacing();
  const int rows = std::min((int)names.size(), visibleRows);
  ImGui::BeginChild(id, ImVec2(0, rows * rowH + 8), ImGuiChildFlags_Borders);
  for (int i = 0; i < (int)names.size(); i++) {
    ImGui::PushID(i);
    if (ImGui::Selectable(names[i].c_str(), i == pick)) {
      changed = i != pick;
      pick = i;
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  return changed;
}

bool NameCombo(const char* label, const std::vector<std::string>& names, int& pick) {
  if (names.empty()) return false;
  if (pick < 0 || pick >= (int)names.size()) pick = 0;
  bool changed = false;
  if (ImGui::BeginCombo(label, names[pick].c_str(), ImGuiComboFlags_HeightLarge)) {
    for (int i = 0; i < (int)names.size(); i++) {
      ImGui::PushID(i);
      if (ImGui::Selectable(names[i].c_str(), i == pick)) {
        changed = i != pick;
        pick = i;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  return changed;
}

// Right-aligned grey key hint on the current line.
void KeyHint(const char* text) {
  const float w = ImGui::CalcTextSize(text).x;
  ImGui::SameLine();
  const float avail = ImGui::GetContentRegionAvail().x;
  if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
  ImGui::TextDisabled("%s", text);
}

}  // namespace

// ---- page: Paint -----------------------------------------------------------
void Overlay::DrawDevPaint(UIState& s) {
  if (!s.devControls) {
    ImGui::TextColored(V4(ui::ColEmber()), "Play mode: tools are inactive.");
    if (ImGui::Button("switch to dev controls (F2)")) s.devControls = true;
    ImGui::Spacing();
  }
  if (Section("Tool")) {
    struct T { const char* label; int tool; const char* tip; };
    static const T kTools[] = {
        {"Brush", UIState::kToolBrush, "LMB paints the material below, RMB erases"},
        {"Laser", UIState::kToolLaser, "hold LMB (or F): melts what it hits, cuts bodies"},
        {"Prefab", UIState::kToolPrefab, "LMB places the prefab below; T rotates, O cycles"},
        {"MPM fluid", UIState::kToolFluid, "hold LMB: pour particle fluid; U clears"},
        {"Mob", UIState::kToolMob, "LMB (or M) places the mob def picked on the Spawn page"},
        {"Hands", UIState::kToolMelee, "your hands / the held item: guard, then flick to cut"},
    };
    const float w = CellWidth(3);
    for (int i = 0; i < 6; i++) {
      if (i % 3) ImGui::SameLine();
      // "##tool": "Brush" / "Prefab" / "MPM fluid" are also section titles.
      if (ToggleButton((std::string(kTools[i].label) + "##tool").c_str(),
                       s.tool == kTools[i].tool, w, 26))
        s.tool = kTools[i].tool;
      ImGui::SetItemTooltip("%s", kTools[i].tip);
    }
    ImGui::TextDisabled("Tab cycles tools");
  }

  if (s.tool == UIState::kToolBrush || s.tool == UIState::kToolLaser ||
      s.tool == UIState::kToolMelee || s.tool == UIState::kToolMob) {
    if (Section("Brush")) {
      ImGui::SetNextItemWidth(-90);
      ImGui::SliderInt("radius", &s.brushRadius, 1, 7);
      KeyHint("[ ]");
      ImGui::SetNextItemWidth(-90);
      ImGui::SliderInt("grain", &s.brushGrain, 0, 8, s.brushGrain == 0 ? "mixed" : "%d/8");
      ImGui::SetItemTooltip("Powder grain size in eighths of a cell. 0 = mixed grains.");
    }
    if (Section("Material")) {
      CurrentMaterial(s, s.brushMaterial, "painting with");
      // Keys 1-8 and the last few picks, as one-click icons.
      static int recent[8] = {};
      auto remember = [&](int id) {
        int k = 0;
        while (k < 7 && recent[k] != id) k++;
        for (; k > 0; k--) recent[k] = recent[k - 1];
        recent[0] = id;
      };
      auto iconRow = [&](const char* label, const int* ids, int n, bool numbered) {
        ImGui::TextDisabled("%s", label);
        for (int i = 0; i < n; i++) {
          const int m = ids[i];
          if (m <= 0 || m >= (int)s.materialNames.size()) continue;
          ImGui::SameLine();
          ImGui::PushID(i);
          ImGui::PushID(label);
          const ImVec2 p = ImGui::GetCursorScreenPos();
          if (ImGui::InvisibleButton("##q", ImVec2(22, 22))) {
            s.brushMaterial = m;
            remember(m);
          }
          ImDrawList* d = ImGui::GetWindowDrawList();
          MatIcon(d, ImVec2(p.x + 2, p.y + 2), 18.0f, s, m);
          if (m == s.brushMaterial)
            d->AddRect(p, ImVec2(p.x + 22, p.y + 22), ui::ColGold(), 0, 0, 2.0f);
          if (numbered) {
            char k[4];
            std::snprintf(k, sizeof k, "%d", i + 1);
            ui::ShadowText(d, ImVec2(p.x + 13, p.y + 9), ui::ColParch(), k);
          }
          if (ImGui::IsItemHovered()) MatTooltip(s, m);
          ImGui::PopID();
          ImGui::PopID();
        }
      };
      static const int kKeys[8] = {1, 2, 3, 4, 5, 6, 7, 8};
      iconRow("keys ", kKeys, 8, true);
      if (recent[0] > 0) iconRow("recent", recent, 8, false);
      ImGui::Spacing();
      std::vector<int> all;
      for (int i = 1; i < (int)s.materialNames.size(); i++) all.push_back(i);
      static char search[64] = "";
      if (MaterialPicker(s, "brushmat", all, s.brushMaterial, search, sizeof search, 340.0f))
        remember(s.brushMaterial);
    }
  }

  if (s.tool == UIState::kToolPrefab && Section("Prefab")) {
    if (s.prefabNames.empty()) {
      ImGui::TextDisabled("no prefabs (assets/prefabs/*.vox)");
    } else {
      if (s.prefabSelected >= (int)s.prefabNames.size()) s.prefabSelected = 0;
      PickList("##prefabs", s.prefabNames, s.prefabSelected, 10);
      ImGui::Text("rotation %d\xc2\xb0", s.prefabRot * 90);
      ImGui::SameLine();
      if (ImGui::Button("rotate (T)")) s.prefabRot = (s.prefabRot + 1) & 3;
      ImGui::SameLine();
      ImGui::Checkbox("overwrite", &s.prefabOverwrite);
      ImGui::SetItemTooltip("off = fill air only");
      if (s.prefabPending > 0)
        ImGui::Text("placing... %u voxels pending", s.prefabPending);
      ImGui::TextDisabled("LMB place  T rotate  O cycle");
    }
  }

  if (s.tool == UIState::kToolFluid && Section("MPM fluid")) {
    static const char* kPour[4] = {"water", "oil", "acid", "blood"};
    const float w = CellWidth(4);
    for (int k = 0; k < 4; k++) {
      if (k) ImGui::SameLine();
      ImGui::PushID(k);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (ToggleButton("##pour", s.fluidPour == k, w, 26)) s.fluidPour = k;
      const int m = MatIdByName(s, kPour[k]);
      ImDrawList* d = ImGui::GetWindowDrawList();
      if (m > 0) MatIcon(d, ImVec2(p.x + 6, p.y + 6), 14.0f, s, m);
      d->AddText(ImVec2(p.x + 26, p.y + 6),
                 s.fluidPour == k ? ui::ColInk() : ui::ColParch(), kPour[k]);
      ImGui::SetItemTooltip("key %d", k + 1);
      ImGui::PopID();
    }
    ImGui::Text("particles %u / 262144", s.fluidCount);
    ImGui::SameLine();
    if (ImGui::Button("clear (U)")) s.clearFluid = true;
    if (ImGui::Button("fluid tuning window...")) s.fluidWindowOpen = !s.fluidWindowOpen;
  }
  if (s.tool == UIState::kToolMelee) {
    ImGui::TextDisabled("hold LMB to guard, then FLICK the mouse to cut");
  }
}

// ---- page: Spawn -----------------------------------------------------------
void Overlay::DrawDevSpawn(UIState& s) {
  // ---- a filled vessel, into your own inventory ----
  // Pickers mirrored by main.cpp (UIState::giveVesselNames); the material
  // columns are only what THIS vessel can hold, so a pouch never offers water
  // and a flask never offers sand.
  if (!s.giveVesselNames.empty() && Section("Filled vessel -> your pack")) {
    if (s.giveVesselPick < 0 || s.giveVesselPick >= (int)s.giveVesselNames.size())
      s.giveVesselPick = 0;
    const int n = (int)s.giveVesselNames.size();
    const int perRow = std::min(n, 4);
    const float w = CellWidth(perRow);
    for (int i = 0; i < n; i++) {
      if (i % perRow) ImGui::SameLine();
      ImGui::PushID(i);
      if (ToggleButton(s.giveVesselNames[i].c_str(), i == s.giveVesselPick, w, 24) &&
          i != s.giveVesselPick) {
        s.giveVesselPick = i;
        s.giveMatPick = 0;
      }
      ImGui::PopID();
    }
    const std::vector<std::string>* names =
        s.giveVesselPick < (int)s.giveVesselMats.size()
            ? &s.giveVesselMats[s.giveVesselPick] : nullptr;
    if (names && !names->empty()) {
      if (s.giveMatPick < 0 || s.giveMatPick >= (int)names->size()) s.giveMatPick = 0;
      std::vector<int> ids;
      ids.reserve(names->size());
      for (const std::string& nm : *names) ids.push_back(MatIdByName(s, nm));
      int sel = ids[s.giveMatPick];
      CurrentMaterial(s, sel, "filled with");
      ImGui::SameLine();
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                           std::max(0.0f, ImGui::GetContentRegionAvail().x - 110));
      if (ToggleButton("GIVE (full)", true, 110, 28)) s.giveVessel = true;
      static char search[64] = "";
      if (MaterialPicker(s, "vesselmat", ids, sel, search, sizeof search, 240.0f))
        for (int i = 0; i < (int)ids.size(); i++)
          if (ids[i] == sel) s.giveMatPick = i;
      if (!s.giveVesselStatus.empty()) ImGui::TextDisabled("%s", s.giveVesselStatus.c_str());
    } else {
      ImGui::TextDisabled("this vessel holds no loaded material");
    }
  }

  // ---- creatures (the old NPC AI window's Spawn tab) ----
  if (Section("Creatures")) {
    ImGui::TextDisabled("spawn a few metres ahead (or at the crosshair)");
    if (!s.aiCreatureNames.empty()) {
      if (s.aiCreaturePick >= (int)s.aiCreatureNames.size()) s.aiCreaturePick = 0;
      // THE RACE FILTER: which kind of person the list below shows. The pick
      // stays an index into the FULL list (the spawn reads it that way); the
      // filtered view maps its rows back.
      // The machine races (automaton, android: assets/editor/automaton.js /
      // android.js), the demons, the nagas and the goblins (demon.js,
      // naga.js, goblin.js) each have their own button; "other" is
      // everything else.
      static const char* kRaces[] = {"all",   "human",  "sylvan",
                                     "automaton", "android", "demon",
                                     "naga",  "goblin", "other"};
      constexpr int kNamed = 9;
      Caption("race");
      {
        const float w = CellWidth(3);
        for (int r = 0; r < kNamed; r++) {
          // Rows of three: a fourth on one line would not fit the panel.
          if (r % 3) ImGui::SameLine();
          if (ToggleButton((std::string(kRaces[r]) + "##airace").c_str(),
                           s.aiRaceFilter == r, w, 22))
            s.aiRaceFilter = r;
        }
      }
      auto raceOf = [&](int i) -> std::string {
        return i < (int)s.aiCreatureRaces.size() ? s.aiCreatureRaces[i]
                                                 : std::string();
      };
      std::vector<std::string> shown;
      std::vector<int> index;
      for (int i = 0; i < (int)s.aiCreatureNames.size(); i++) {
        const std::string r = raceOf(i);
        bool named = false;
        for (int k = 1; k < kNamed - 1; k++) named = named || r == kRaces[k];
        const bool keep =
            s.aiRaceFilter == 0 ||
            (s.aiRaceFilter < kNamed - 1 && r == kRaces[s.aiRaceFilter]) ||
            (s.aiRaceFilter == kNamed - 1 && !named);
        if (!keep) continue;
        shown.push_back(s.aiCreatureNames[i]);
        index.push_back(i);
      }
      Caption("body");
      if (shown.empty()) {
        ImGui::TextDisabled("no %s creatures loaded", kRaces[s.aiRaceFilter]);
      } else {
        int at = 0;
        for (int k = 0; k < (int)index.size(); k++)
          if (index[k] == s.aiCreaturePick) at = k;
        // A filter that hides the current pick moves it onto the first
        // visible row, so the spawn buttons never spawn something unseen.
        s.aiCreaturePick = index[at];
        if (PickList("##creatures", shown, at, 5)) s.aiCreaturePick = index[at];
      }
    }
    // One box per published effect: MobSystem::DefWithEffects prefers an
    // authored combination and composes one when there is none.
    if (!s.aiEffectNames.empty()) {
      Caption("afflicted with");
      const float colW = CellWidth(2) + ImGui::GetStyle().ItemSpacing.x;
      for (int i = 0; i < (int)s.aiEffectNames.size() && i < (int)s.aiEffectOn.size(); i++) {
        if (i & 1) ImGui::SameLine(ImGui::GetCursorStartPos().x + colW);
        ImGui::PushID(i);
        bool on = s.aiEffectOn[i] != 0;
        if (ImGui::Checkbox(s.aiEffectNames[i].c_str(), &on)) s.aiEffectOn[i] = on ? 1 : 0;
        if (s.aiEffectNames[i] == "zombie")
          ImGui::SetItemTooltip("walks slower, comes apart when cut,\nspawns already bitten, bites back");
        ImGui::PopID();
      }
    }
    ImGui::SetNextItemWidth(-90);
    NameCombo("weapon##ai", s.aiWeaponNames, s.aiWeaponPick);
    ImGui::SetItemTooltip(
        "The blade decides the wound: reach, cut depth and heft all come off\n"
        "its own art. \"fists\" is a weapon: a bare hand bruises and never\n"
        "severs, and a mace beats plate in where a sword skates off it.");
    if (!s.aiOffhandNames.empty()) {
      ImGui::SetNextItemWidth(-90);
      NameCombo("off hand##ai", s.aiOffhandNames, s.aiOffhandPick);
      ImGui::SetItemTooltip(
          "A shield in the left fist. Its guard verb raises it instead of the\n"
          "blade (a guardian keeps it up), it stops what its own voxels are in\n"
          "the way of, and it chips, burns and breaks. A raised shield bars the\n"
          "other arm's swing and slows the walk and the turn.");
    }
    {
      static const char* kOutfits[] = {"nothing", "random clothes", "full plate",
                                       "custom pieces"};
      ImGui::SetNextItemWidth(-90);
      ImGui::Combo("outfit##ai", &s.aiOutfit, kOutfits, 4);
      ImGui::SetItemTooltip(
          "random clothes: shirt, legs and shoes from the dyeable set\n"
          "full plate: every slot's iron_* piece\n"
          "A blade wears through armour in holes; it leaves with the limb.");
      if (s.aiOutfit == 3) {
        const int n = (int)std::min(s.aiWearNames.size(), s.aiWearSlotLabels.size());
        if ((int)s.aiWearPick.size() < n) s.aiWearPick.resize(n, 0);
        for (int i = 0; i < n; i++) {
          if (s.aiWearNames[i].size() <= 1) continue;   // nothing fits this slot
          ImGui::PushID(i);
          ImGui::SetNextItemWidth(-90);
          NameCombo((s.aiWearSlotLabels[i] + "##aiwear").c_str(), s.aiWearNames[i],
                    s.aiWearPick[i]);
          ImGui::PopID();
        }
      }
    }
    ImGui::Spacing();
    Caption("spawn");
    {
      const float w = CellWidth(2);
      if (ToggleButton("random human##ai", true, w, 26)) s.aiSpawnRandom = true;
      ImGui::SetItemTooltip("any sex, build, face, hair, colouring, weapon,\n"
                            "outfit and fighting style (only the affliction\n"
                            "boxes above are used)");
      ImGui::SameLine();
      if (ToggleButton("as authored##ai", true, w, 26)) s.aiSpawnOwn = true;
      ImGui::SetItemTooltip("the picked body with its own JSON behaviour\n(zombie -> bites)");
    }
    ImGui::TextDisabled("...or override the behaviour with a preset:");
    {
      struct P { const char* label; const char* profile; const char* tip; bool* flag; };
      const P kPresets[] = {
          {"dummy", nullptr, "blind, never moves, never turns", &s.aiSpawnDummy},
          {"static", nullptr, "static swordsman: turns to face, swings in reach",
           &s.aiSpawnStatic},
          {"duelist", nullptr, "paths in, holds range, circles", &s.aiSpawnDuelist},
          {"swordsman", "swordsman", "all-rounder: guards, ripostes, adapts", nullptr},
          {"fencer", "fencer", "kites at the edge of your reach, dodges", nullptr},
          {"brawler", "brawler", "rushes inside, weaves, punishes whiffs", nullptr},
          {"berserker", "berserker", "relentless; worse when hurt", nullptr},
          {"guardian", "guardian", "turtles, parries, counters", nullptr},
          {"archer", "archer",
           "keeps its distance and shoots: a bow, a quiver, and the\n"
           "weapon picked above as its sidearm for when the arrows run out",
           nullptr},
      };
      const float w = CellWidth(4);
      int k = 0;
      for (const P& p : kPresets) {
        if (k++ % 4) ImGui::SameLine();
        if (ImGui::Button((std::string(p.label) + "##aipreset").c_str(), ImVec2(w, 0))) {
          if (p.flag) *p.flag = true;
          else s.aiSpawnProfile = p.profile;
        }
        ImGui::SetItemTooltip("%s", p.tip);
      }
    }
    ImGui::Spacing();
    if (ImGui::TreeNode("place any mob def with the Mob tool")) {
      if (s.mobNames.empty()) {
        ImGui::TextDisabled("no mobs (assets/mobs/*.vox + .json)");
      } else {
        if (s.mobSelected >= (int)s.mobNames.size()) s.mobSelected = 0;
        if (PickList("##mobdefs", s.mobNames, s.mobSelected, 6)) s.tool = UIState::kToolMob;
        if (ToggleButton("use Mob tool", s.tool == UIState::kToolMob))
          s.tool = UIState::kToolMob;
        ImGui::SameLine();
        ImGui::TextDisabled("LMB (or M) at crosshair");
      }
      ImGui::TreePop();
    }
  }

  // ---- conversations (game/dialogue.h) ----
  // THE DEV HOOK until NPCs carry their own dialogue (PLAN_world_editor P7):
  // pick a conversation file and talk to the nearest creature with it, or to
  // nobody at all. What is wrong with the files is listed here, by file, node
  // and field, so a typo shows in the game and not only on stderr.
  if (Section("Dialogue", false)) {
    if (s.dialogueNames.empty()) {
      ImGui::TextDisabled("no conversations in assets/dialogue/");
    } else {
      if (s.dialoguePick < 0 || s.dialoguePick >= (int)s.dialogueNames.size())
        s.dialoguePick = 0;
      PickList("##dialogues", s.dialogueNames, s.dialoguePick, 5);
      const float w = CellWidth(2);
      if (ImGui::Button("talk to nearest creature", ImVec2(w, 0)))
        s.dialogueTalkNearest = true;
      ImGui::SetItemTooltip("the closest living creature within 12 m speaks\n"
                            "these lines (it does not have to be a person)");
      ImGui::SameLine();
      if (ImGui::Button("talk (no speaker)", ImVec2(w, 0))) s.dialogueTalkVoice = true;
      ImGui::SetItemTooltip("the conversation with nobody standing there:\n"
                            "for reading a file through");
    }
    {
      const float w = CellWidth(2);
      if (ImGui::Button("reload files (R)", ImVec2(w, 0))) s.dialogueReload = true;
      ImGui::SameLine();
      if (ImGui::Button("forget flags + met", ImVec2(w, 0))) s.dialogueResetFlags = true;
      ImGui::SetItemTooltip("clears every dialogue flag and who you have met,\n"
                            "so a conversation can be tried from the start");
    }
    if (!s.dialogueStatus.empty()) ImGui::TextWrapped("%s", s.dialogueStatus.c_str());
    if (!s.dialogueProblems.empty()) {
      Caption("problems");
      for (const std::string& p : s.dialogueProblems) {
        const bool err = p.rfind("error", 0) == 0;
        ImGui::PushStyleColor(ImGuiCol_Text, err ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                                 : ImVec4(1.0f, 0.75f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", p.c_str());
        ImGui::PopStyleColor();
      }
    }
    if (!s.dialogueFlags.empty() && ImGui::TreeNode("flags + met")) {
      for (const std::string& f : s.dialogueFlags) ImGui::TextUnformatted(f.c_str());
      ImGui::TreePop();
    }
  }

  // ---- clothes (game/dye.h) ----
  // The art is a greyscale weave; the colour is applied at shade time, so a
  // dye is PAINT, not a material — dyed linen burns and tears like undyed.
  if (Section("Clothes (dyed)", false)) {
    auto combo = [&](const char* label, const std::vector<std::string>& names, int& pick) {
      if (names.empty()) {
        ImGui::TextDisabled("%s: no dyeable pieces in items.json", label);
        return;
      }
      ImGui::SetNextItemWidth(-90);
      NameCombo((std::string(label) + "##wardrobe").c_str(), names, pick);
    };
    combo("shirt", s.wardrobeShirts, s.wardrobeShirtPick);
    combo("legs", s.wardrobeLegs, s.wardrobeLegsPick);
    combo("feet", s.wardrobeFeet, s.wardrobeFeetPick);
    ImGui::SetNextItemWidth(std::min(200.0f, ImGui::GetContentRegionAvail().x));
    ImGui::ColorPicker3("##wardrobedye", s.wardrobeColor,
                        ImGuiColorEditFlags_PickerHueWheel |
                            ImGuiColorEditFlags_NoSidePreview |
                            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    ImGui::ColorButton("##wardrobeswatch",
                       ImVec4(s.wardrobeColor[0], s.wardrobeColor[1], s.wardrobeColor[2], 1.0f),
                       0, ImVec2(24, 24));
    ImGui::SameLine();
    ImGui::TextUnformatted(s.wardrobeColorName.empty() ? "(undyed)"
                                                       : s.wardrobeColorName.c_str());
    ImGui::SameLine();
    if (ImGui::Button("random##wardrobe")) s.wardrobeRandomColor = true;
    const float w = CellWidth(3);
    if (ImGui::Button("into pack##wardrobe", ImVec2(w, 0))) s.wardrobeSpawnSet = true;
    ImGui::SetItemTooltip("hotbar, else the bag");
    ImGui::SameLine();
    if (ImGui::Button("put it on##wardrobe", ImVec2(w, 0))) s.wardrobeWearSet = true;
    ImGui::SetItemTooltip("straight into the equip slots");
    ImGui::SameLine();
    if (ImGui::Button("re-dye worn##wardrobe", ImVec2(w, 0))) s.wardrobeDyeWorn = true;
    ImGui::SetItemTooltip("every dyeable piece on the body");
    if (!s.wardrobeStatus.empty()) ImGui::TextWrapped("%s", s.wardrobeStatus.c_str());
  }

  // ---- objects and effects ----
  if (Section("Objects & effects")) {
    const float w = CellWidth(2);
    if (ImGui::Button("rolling sphere (K)", ImVec2(w, 0))) s.spawnSphere = true;
    ImGui::SetItemTooltip("a rigidbody ball of the Paint page's material: its mass\n"
                          "(and how far you can shove it) comes from that material");
    ImGui::SameLine();
    if (ImGui::Button("detonate at crosshair (X)", ImVec2(w, 0))) s.pendingDetonate = true;
    ImGui::TextDisabled("G throws a grenade");

    // The same parametric object a `gust` spell emits, on the same list,
    // through the same budget — there is no dev-only wind path.
    ImGui::Spacing();
    Caption("wind primitive");
    ImGui::SameLine();
    ImGui::TextDisabled("%d live, waking %d chunks", s.windPrims, s.windWakeChunks);
    static const char* kinds[] = {"cone (fan / jet)", "burst (blast or vacuum)",
                                  "vortex (tornado)"};
    ImGui::SetNextItemWidth(-90);
    ImGui::Combo("kind", &s.windFanKind, kinds, 3);
    ImGui::SetNextItemWidth(-90);
    EditableSliderFloat("speed", &s.windFanSpeed, -40.0f, 40.0f, "%.0f m/s");
    ImGui::SetItemTooltip("Core speed at the mouth. NEGATIVE turns a burst into a\n"
                          "vacuum and a cone into a draw.");
    ImGui::SetNextItemWidth(-90);
    EditableSliderInt("radius", &s.windFanRadius, 1, 64);
    ImGui::SetNextItemWidth(-90);
    EditableSliderInt("reach", &s.windFanReach, 1, 128);
    ImGui::Checkbox("may move SETTLED powder", &s.windFanEntrain);
    ImGui::SetItemTooltip(
        "OFF: a fan only steers what is already moving and costs nothing\n"
        "when the world around it is asleep. ON: it may pull RESTING powder\n"
        "loose in its footprint (blows a dune flat); its chunks are charged\n"
        "against sim.windWakeChunks.");
    if (ImGui::Button("place where I'm looking", ImVec2(w, 0))) s.placeWindFan = true;
    ImGui::SameLine();
    if (ImGui::Button("clear all##fans", ImVec2(w, 0))) s.clearWindFans = true;
    if (s.windPrimsDropped > 0)
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%d refused (world cap is %d)",
                         s.windPrimsDropped, (int)kWindPrimCap);
  }

  if (Section("Cleanup")) {
    const float w = CellWidth(2);
    if (ImGui::Button("kill all spawned##ai", ImVec2(w, 0))) s.aiKillSpawned = true;
    ImGui::SameLine();
    if (ImGui::Button("ragdoll all spawned##ai", ImVec2(w, 0))) s.aiRagdollSpawned = true;
    if (ImGui::Button("clear MPM fluid (U)", ImVec2(w, 0))) s.clearFluid = true;
    ImGui::SameLine();
    if (ImGui::Button("clear wind fans", ImVec2(w, 0))) s.clearWindFans = true;
  }
}

// ---- page: World -----------------------------------------------------------
void Overlay::DrawDevWorld(UIState& s) {
  if (Section("Time")) {
    // Scales the CELESTIAL clock (sim/world.h CelestialClock); the sim still
    // ticks at 30 Hz. Anything but 1x changes the world hash.
    float t = std::cbrt(s.timeScale / 100.0f);
    ImGui::SetNextItemWidth(-90);
    if (ImGui::SliderFloat("sky speed", &t, -1.0f, 1.0f, "")) {
      s.timeScale = t * t * t * 100.0f;
      if (std::abs(s.timeScale) < 0.05f) s.timeScale = 0.0f;
    }
    ImGui::SetItemTooltip(
        "Speed of the CELESTIAL clock: the sun, both moons, the seasons, and\n"
        "the daylight-gated reactions all run at this multiple. The simulation\n"
        "itself still ticks at 30 Hz - sand does not fall faster.\n"
        "0 freezes the sky, negative runs it backwards.\n"
        "Anything but 1x changes the world hash on purpose.");
    ImGui::SameLine();
    ImGui::Text("%.2fx", s.timeScale);
    struct B { const char* l; float v; };
    static const B kB[] = {{"1x", 1.0f}, {"10x", 10.0f}, {"100x", 100.0f},
                           {"freeze", 0.0f}, {"reverse", -1.0f}};
    const float w = CellWidth(5);
    for (int i = 0; i < 5; i++) {
      if (i) ImGui::SameLine();
      if (ToggleButton(kB[i].l, s.timeScale == kB[i].v, w)) s.timeScale = kB[i].v;
    }
    auto phaseName = [](float p) {
      if (p < 0.06f || p > 0.94f) return "new";
      if (p < 0.19f) return "cresc";
      if (p < 0.31f) return "quarter";
      if (p < 0.44f) return "gibbous";
      if (p < 0.56f) return "FULL";
      if (p < 0.69f) return "gibbous";
      if (p < 0.81f) return "quarter";
      return "cresc";
    };
    const int hh = (int)(s.skyDayT * 24.0f) % 24;
    const int mm = (int)(s.skyDayT * 1440.0f) % 60;
    ImGui::TextDisabled("%02d:%02d  sun %+.0f\xc2\xb0  year %.0f%%", hh, mm,
                        s.skySunElevDeg, s.skyYearT * 100.0f);
    ImGui::TextDisabled("moon A %s (%.2f)   moon B %s (%.2f)", phaseName(s.skyMoonPhase),
                        s.skyMoonPhase, phaseName(s.skyMoon2Phase), s.skyMoon2Phase);
    if (s.skySolarEclipse > 0.995f)
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "*** TOTAL SOLAR ECLIPSE ***");
    else if (s.skySolarEclipse > 0.0f)
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "solar eclipse: %.0f%% covered",
                         s.skySolarEclipse * 100.0f);
  }

  // ---- the sky's weather (src/sim/weather.h) ----
  // Render-only: the world hash never sees the weather (except "rain touches
  // world"), so nothing here needs a shader reload.
  if (Section("Weather")) {
    const std::vector<weather::Preset>& ps = weather::Presets().Presets();
    const std::string cur = weather::Override();
    const weather::State& w = weather::Last();
    {
      const float bw = CellWidth(4);
      if (ToggleButton("auto##wx", cur.empty(), bw)) weather::SetOverride("");
      ImGui::SetItemTooltip("hand the sky back to the automatic cycle\n"
                            "(or weather.preset when that is off)");
      int n = 1;
      for (const weather::Preset& p : ps) {
        if (n++ % 4 != 0) ImGui::SameLine();
        const bool on = cur == p.name;
        if (ToggleButton((p.label + "##wxb" + p.name).c_str(), on, bw))
          weather::SetOverride(on ? "" : p.name);
      }
    }
    if (w.fromName == w.toName)
      ImGui::Text("now: %s", w.fromName.c_str());
    else
      ImGui::Text("now: %s -> %s (%.0f%%)", w.fromName.c_str(), w.toName.c_str(),
                  w.blend * 100.0f);
    ImGui::TextDisabled("cover %.2f  rain %.2f  wet %.2f  overcast %.2f%s", w.mix.coverage,
                        w.mix.precip, w.wetness, w.overcast, w.flash > 0.05f ? "  *flash*" : "");

    Tuning t = CurrentTuning();
    Tuning::Weather& wt = t.weather;
    bool changed = false;
    changed |= ImGui::Checkbox("clouds##wx", &wt.clouds);
    ImGui::SetItemTooltip("Master switch. Off records no cloud pass at all.");
    ImGui::SameLine();
    changed |= ImGui::Checkbox("automatic cycle##wx", &wt.autoCycle);
    ImGui::SetItemTooltip("On: the sky walks the preset ladder by moisture on its own.\n"
                          "Off: it holds weather.preset. A pin above overrides both.");
    if (ImGui::TreeNode("weather knobs")) {
      changed |= EditableSliderFloat("cycle speed##wx", &wt.cycleSpeed, 0.0f, 50.0f, "%.1fx");
      changed |= EditableSliderFloat("coverage bias##wx", &wt.coverageBias, -1.0f, 1.0f, "%+.2f");
      ImGui::SetItemTooltip("Added to every preset's coverage: cloudier / clearer.");
      changed |= EditableSliderFloat("raininess x##wx", &wt.precipScale, 0.0f, 4.0f, "%.2fx");
      changed |= ImGui::Checkbox("rain touches world##wx", &wt.rainTouchesWorld);
      ImGui::SetItemTooltip("Rain douses exposed fire and damps ignition (moves the world hash).");
      changed |= EditableSliderFloat("rain ignition damp##wx", &wt.rainIgniteDamp, 0.0f, 1.0f, "%.2f");
      {
        const uint32_t rw = weather::LastSimRainWord();
        ImGui::TextDisabled("sim: rain %u  wet %u /255", rw & 0xFFu, (rw >> 16) & 0xFFu);
      }
      changed |= EditableSliderFloat("ease (s)##wx", &wt.transitionSeconds, 0.0f, 60.0f, "%.0f s");
      if (ImGui::SmallButton("reroll weather##wx")) {
        wt.seedOffset = (wt.seedOffset + 1) % 1000;
        changed = true;
      }
      ImGui::TreePop();
    }
    if (changed) SetCurrentTuning(t);
  }

  // ---- WIND & WEATHER (docs/RESEARCH_wind.md §13) ----
  // One section, grouped as the model is: the regime that sets the wind's
  // character, the profile/terrain ramp, the gusts, local winds, storms, the
  // visuals. Every knob here is CPU-side (it rides the wf* block or
  // RenderParams) and applies on the next tick with no F5, EXCEPT the ones
  // marked (F5), which are compiled into the shaders.
  // ---- the temperature layer (docs/PLAN_temperature.md) ----
  // Ambient, target and actual, separately: T = ambient + X, and X walks
  // toward its target X* at the block's inertia.
  if (Section("Temperature")) {
    const UIState::HeatReadout& h = s.heat;
    if (h.valid) {
      ImGui::Text("%s  %s  ambient %+d  (base %+d, %s %+d)", h.biome.c_str(),
                  h.day ? "day" : "night", h.ambient, h.base, h.day ? "day" : "night",
                  h.day ? h.swing : -h.swing);
      ImGui::SetItemTooltip("The climate of the column you stand in (assets/biomes/<name>.json\n"
                            "climate.ambient), or the snowline's above the treeline. Heat units:\n"
                            "0 is the freezing point of water. Switches on the daylight tick.");
      if (!h.current) {
        ImGui::TextDisabled("local heat: no reading for this block yet (the world here is asleep)");
      } else if (h.paged) {
        ImGui::Text("ambient %+d  target T* %+d (+%d)  actual T %+d (+%d)", h.ambient,
                    h.ambient + h.xTarget, h.xTarget, h.ambient + h.x, h.x);
        ImGui::Text("sources in your block: emit %d x %d cells", h.emit, h.emitters);
      } else {
        ImGui::TextDisabled("local heat: none (no page here)  ->  T %+d", h.ambient);
      }
      ImGui::SetItemTooltip("The 2x2x2 block at your feet. The actual temperature T is the ambient\n"
                            "plus the local excess X; the target T* is the ambient plus X*, the\n"
                            "coverage-weighted mix of every heat source within sim.heatRadius\n"
                            "blocks. T walks to T* over a few ticks (inertia) and never passes the\n"
                            "hottest source in reach.");
      ImGui::TextDisabled("pages %u / %u (peak %u, refused %u)  relax %u  recompute %u",
                          h.pages, h.pool, h.pagesPeak, h.refused, h.relaxChunks, h.recompChunks);
      ImGui::TextDisabled("melts %u  ignitions %u  freezes %u (since the world loaded)", h.melts,
                          h.ignites, h.freezes);
    } else {
      ImGui::TextDisabled("no readout yet");
    }

    // ---- the knobs (src/sim/tuning_params.def, sim.heat*) ----
    // Ranges are the def rows' min/max, the load clamps. All live: a change
    // re-targets every heat page and wakes the world on the next tick
    // (Simulation::PrepareHeat), so settled heat follows the slider too.
    // Moves the world hash like any sim.* knob.
    Tuning t = CurrentTuning();
    Tuning::Sim& hs = t.sim;
    bool changed = false;
    bool on = hs.heatMode != 0;
    if (ImGui::Checkbox("heat on##heat", &on)) {
      hs.heatMode = on ? 1 : 0;
      changed = true;
    }
    ImGui::SetItemTooltip("Off: nothing warms anything -- no melting, no catching fire from\n"
                          "nearby heat, no freezing -- and the layer frees its memory.");
    ImGui::SameLine();
    if (ImGui::SmallButton("reset to defaults##heat")) {
      static const Tuning d{};   // the def rows' defaults (TPD)
      hs.heatMode = d.sim.heatMode;
      hs.heatRadius = d.sim.heatRadius;
      hs.heatGain = d.sim.heatGain;
      hs.heatUpGain = d.sim.heatUpGain;
      hs.heatSideGain = d.sim.heatSideGain;
      hs.heatDownGain = d.sim.heatDownGain;
      hs.heatSnowlineBase = d.sim.heatSnowlineBase;
      hs.heatSnowlineSwing = d.sim.heatSnowlineSwing;
      hs.heatLapsePerKm = d.sim.heatLapsePerKm;
      changed = true;
    }
    ImGui::SetItemTooltip("Every heat knob back to its shipped value.");
    changed |= EditableSliderFloat("rise (up)##heat", &hs.heatUpGain, 0.0f, 32.0f, "%.2fx");
    ImGui::SetItemTooltip("How far heat carries UPWARD: campfires scorch what hangs over them.\n"
                          "20 lights foliage 3-4 cells above a campfire.");
    changed |= EditableSliderFloat("spread (side)##heat", &hs.heatSideGain, 0.0f, 1.0f, "%.2fx");
    ImGui::SetItemTooltip("How much heat reaches things BESIDE a source. Low: a bush next to\n"
                          "a campfire stays green. 1 = as much as straight across.");
    changed |= EditableSliderFloat("sink (down)##heat", &hs.heatDownGain, 0.0f, 4.0f, "%.2fx");
    ImGui::SetItemTooltip("How much heat reaches things BELOW a source: the ground under a\n"
                          "fire, ice under a lava flow.");
    changed |= EditableSliderInt("reach##heat", &hs.heatRadius, 2, 8);
    ImGui::SetItemTooltip("How far any source reaches, in 2-voxel blocks (6 = 12 voxels).\n"
                          "Past it, a source adds nothing at all.");
    changed |= EditableSliderInt("intensity##heat", &hs.heatGain, 1, 64);
    ImGui::SetItemTooltip("How little of its surroundings a source must fill to heat a spot to\n"
                          "its own temperature (1/this). Higher: small fires heat harder,\n"
                          "lava lights wood farther off. Never hotter than the source.");
    changed |= EditableSliderInt("snowline base##heat", &hs.heatSnowlineBase, -64, 63);
    ImGui::SetItemTooltip("The climate above the treeline (0 = water freezes). Kept below 0 by\n"
                          "day (base + swing < 0) so the peaks keep their snow.");
    changed |= EditableSliderInt("snowline swing##heat", &hs.heatSnowlineSwing, 0, 63);
    ImGui::SetItemTooltip("Day / night swing of the snowline climate (day = base + swing).");
    changed |= EditableSliderInt("lapse rate##heat", &hs.heatLapsePerKm, 0, 64);
    ImGui::SetItemTooltip("Heat units colder per km above the sea, below the snowline\n"
                          "(map v2): a high valley's ponds skin over on cold nights.");

    // ---- heat-driven wind (wind phase 5; common.wgsl HEAT UPDRAFTS) ----
    // Def-row ranges. Live: they ride TickParams, and a change re-solves the
    // draft volume's stack field (Simulation::NoteDraftHeatKnobs).
    ImGui::SeparatorText("updrafts (heat -> wind)");
    changed |= EditableSliderFloat("updraft strength##heat", &hs.windUpdraftGain, 0.0f, 40.0f,
                                   "%.1f m/s per 100");
    ImGui::SetItemTooltip("Rising air over heat: m/s of lift per 100 heat units above ambient\n"
                          "(the air over a fire ~100, over lava ~200). Smoke over a fire stands\n"
                          "up in a crosswind, heavy gases are carried up, light particles loft.\n"
                          "Debris, corpses and trees do NOT feel it (GPU only). 0 = off.");
    changed |= EditableSliderFloat("updraft cap##heat", &hs.windUpdraftCap, 0.0f, 40.0f, "%.1f m/s");
    ImGui::SetItemTooltip("The most lift any heat makes. ~16 m/s holds a falling ember up.");
    changed |= EditableSliderFloat("inflow strength##heat", &hs.windUpdraftInflow, 0.0f, 4.0f, "%.2fx");
    ImGui::SetItemTooltip("Air drawn in toward a hot column at its base, per unit of lift\n"
                          "gradient: what pulls nearby smoke into a plume.");
    changed |= EditableSliderFloat("stack effect##heat", &hs.windStackGain, 0.0f, 4.0f, "%.2fx");
    ImGui::SetItemTooltip("Inside the draft volume round you, the lift is made to respect the\n"
                          "walls: a hot room with a low and a high opening draws air in at one\n"
                          "and vents at the other (a burning house breathes). 0 = local lift only.");
    if (ImGui::SmallButton("reset updrafts##heat")) {
      static const Tuning d{};
      hs.windUpdraftGain = d.sim.windUpdraftGain;
      hs.windUpdraftCap = d.sim.windUpdraftCap;
      hs.windUpdraftInflow = d.sim.windUpdraftInflow;
      hs.windStackGain = d.sim.windStackGain;
      changed = true;
    }
    if (changed) {
      // LoadTuning's hand-written rule (tuning_params.def above the row): the
      // snowline must stay frozen by day.
      if (hs.heatSnowlineBase + hs.heatSnowlineSwing >= 0)
        hs.heatSnowlineBase = -1 - hs.heatSnowlineSwing;
      SetCurrentTuning(t);
    }
  }

  if (Section("Wind & weather")) {
    Tuning t = CurrentTuning();
    Tuning::Wind& w = t.wind;
    bool changed = false;
    const UIState::WindReadout& r = s.wind;

    // ---- the readout: the ambient field where you stand ----
    if (r.valid) {
      static const char* kSrc[] = {"manual", "sky", "epochs", "pinned"};
      ImGui::Text("%s  I %.2f  gale %.2f  storm %.2f", r.source >= 0 && r.source < 4 ? kSrc[r.source] : "?",
                  r.intensity, r.gale, r.convective);
      ImGui::SetItemTooltip("Who set the regime (manual knobs / the sky's preset ladder / the wind's own\n"
                            "epochs / a pinned regime), and the three regime inputs: intensity (~Beaufort/12),\n"
                            "gale weight, convective-storm weight.");
      ImGui::Text("ref %.1f m/s  gust %.1f  heading %.0f deg", r.refSpeed, r.gustAmp, r.headingDeg);
      ImGui::Text("here %.1f m/s (mean %.1f, gusts %+.1f, local %.1f)", r.totalMs, r.meanMs, r.gustExcess,
                  r.extraMs);
      ImGui::SetItemTooltip("The C++ mirror of the SIM's windAtQ (windfield.h Probe) at your head height:\n"
                            "total horizontal speed, the mean after the ramp, the gust bands along the\n"
                            "mean right now, and the thermal + slope + sea terms. Primitives excluded.");
      ImGui::Text("%.1f m above ground (y %.0f)  profile %.2f", r.haglM, r.groundY, r.profile);
      ImGui::Text("exposure %+.2f -> x%.2f  ramp x%.2f  lee x%.2f up %.2f", r.exposure, r.expMul, r.ramp, r.leeMean, r.leeRise);
      ImGui::TextDisabled("stability %+.2f  coupling %.2f  meander %.0f deg  heading here %.0f", r.stability,
                          r.coupling, r.wanderDeg, r.localHeadingDeg);
      ImGui::TextDisabled("thermal %.1f  slope %+.1f  sea %+.1f m/s  gust frac %.2f", r.thermalMs, r.slopeMs,
                          r.seaMs, r.gustFrac);
      if (r.stormPhase >= 0.0f)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "storm cycle %.0f%%  envelope x%.2f  jump %+.0f deg",
                           r.stormPhase * 100.0f, r.envelope, r.jumpDeg);
      ImGui::TextDisabled("terrain table: %u column queries on its last rebuild", r.terrQueries);
    }

    // ---- Weather: the preset picker and the manual intensity ----
    if (ImGui::TreeNodeEx("Weather##wnd", ImGuiTreeNodeFlags_DefaultOpen)) {
      const auto& regs = windfield::Regimes();
      const float bw = CellWidth(4);
      const bool autoOn = w.regime.empty() || w.regime == "auto";
      if (ToggleButton("auto (sky)##wreg", autoOn, bw)) { w.regime = "auto"; changed = true; }
      ImGui::SetItemTooltip("The sky drives the wind: each sky preset names a regime and the sky's\n"
                            "ladder blends them (a storm sky blows a storm, fog is calm).");
      int n = 1;
      for (const windfield::Regime& g : regs) {
        if (n++ % 4 != 0) ImGui::SameLine();
        const bool on = w.regime == g.name;
        if (ToggleButton((g.label + "##wreg" + g.name).c_str(), on, bw)) {
          w.regime = on ? "auto" : g.name;
          changed = true;
        }
        if (!g.about.empty()) ImGui::SetItemTooltip("%s", g.about.c_str());
      }
      bool ovr = w.intensity >= 0.0f;
      if (ImGui::Checkbox("manual intensity##wnd", &ovr)) {
        w.intensity = ovr ? std::max(r.intensity, 0.0f) : -1.0f;
        changed = true;
      }
      ImGui::SetItemTooltip("Overrides the intensity whatever the sky or a pinned regime says (live).\n"
                            "0.05 calm, 0.3 breezy, 0.5 windy, 0.8 gale.");
      if (ovr) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= EditableSliderFloat("##wint", &w.intensity, 0.0f, 1.0f, "%.2f");
      }
      changed |= ImGui::Checkbox("evolving weather##wnd", &w.weatherAuto);
      ImGui::SetItemTooltip("On: heading and mood wander by epoch and the sky drives the regime.\n"
                            "Off: heading = direction below, intensity 0.3 (so the mean at the\n"
                            "reference height IS the wind speed below) unless pinned.");
      changed |= EditableSliderFloat("wind speed##wnd", &w.windSpeed, 0.0f, 40.0f, "%.1f m/s");
      ImGui::SetItemTooltip("The mean at the reference height at intensity 0.3 (breezy). The\n"
                            "regime's intensity curve multiplies it.");
      changed |= EditableSliderFloat("direction##wnd", &w.windDirDeg, 0.0f, 360.0f, "%.0f deg");
      changed |= EditableSliderFloat("mood spread##wnd", &w.moodSpread, 0.0f, 1.0f, "%.2f");
      changed |= EditableSliderFloat("speed at calm##wnd", &w.speedCalmMul, 0.0f, 1.0f, "%.2fx");
      changed |= EditableSliderFloat("speed at gale##wnd", &w.speedGaleMul, 1.0f, 10.0f, "%.2fx");
      changed |= EditableSliderFloat("speed at max##wnd", &w.speedMaxMul, 1.0f, 12.0f, "%.2fx");
      changed |= EditableSliderFloat("mixing intensity##wnd", &w.mixIntensity, 0.05f, 1.0f, "%.2f");
      ImGui::SetItemTooltip("Wind mixes the day/night stability away, to neutral by this intensity.");
      changed |= EditableSliderFloat("stable decoupling##wnd", &w.stableDecouple, 0.0f, 0.95f, "%.2f");
      changed |= EditableSliderFloat("decoupling depth##wnd", &w.decoupleHeight, 1.0f, 200.0f, "%.0f m");
      ImGui::TreePop();
    }

    if (ImGui::TreeNode("Profile & terrain##wnd")) {
      changed |= EditableSliderFloat("roughness z0##wnd", &w.roughness, 0.001f, 2.0f, "%.3f m");
      changed |= EditableSliderFloat("reference height##wnd", &w.profileRef, 0.1f, 50.0f, "%.1f m");
      ImGui::SetItemTooltip("Height above ground where the mean equals the authored speed.");
      changed |= EditableSliderFloat("profile floor##wnd", &w.profileFloor, 0.0f, 1.0f, "%.2fx");
      changed |= EditableSliderFloat("profile cap##wnd", &w.profileCap, 1.0f, 4.0f, "%.2fx");
      changed |= EditableSliderFloat("outside table##wnd", &w.profileNeutral, 0.1f, 4.0f, "%.2fx");
      changed |= EditableSliderFloat("altitude gain##wnd", &w.absGain, -0.5f, 1.0f, "%.2f /100m");
      changed |= EditableSliderFloat("neighbourhood##wnd", &w.tpiRadius, 13.0f, 400.0f, "%.0f m");
      ImGui::SetItemTooltip("TPI radius: the mean ground is taken over this. Rebuilds the table.");
      changed |= EditableSliderFloat("exposure scale##wnd", &w.tpiScale, 1.0f, 400.0f, "%.0f m");
      changed |= EditableSliderFloat("ridge gain light##wnd", &w.ridgeLight, 0.0f, 3.0f, "%.2f");
      changed |= EditableSliderFloat("ridge gain strong##wnd", &w.ridgeStrong, 0.0f, 3.0f, "%.2f");
      changed |= EditableSliderFloat("hollow shelter light##wnd", &w.valleyLight, 0.0f, 3.0f, "%.2f");
      changed |= EditableSliderFloat("hollow shelter strong##wnd", &w.valleyStrong, 0.0f, 3.0f, "%.2f");
      changed |= EditableSliderFloat("exposure depth##wnd", &w.exposureDepth, 1.0f, 400.0f, "%.0f m");
      changed |= EditableSliderFloat("water radius##wnd", &w.seaRadius, 13.0f, 400.0f, "%.0f m");
      ImGui::TreePop();
    }

    if (ImGui::TreeNode("Gusts##wnd")) {
      changed |= EditableSliderFloat("gustiness x##wnd", &w.gustStrength, 0.0f, 2.0f, "%.2fx");
      changed |= EditableSliderFloat("gust frac light##wnd", &w.gustLight, 0.0f, 3.0f, "%.2f");
      changed |= EditableSliderFloat("gust frac strong##wnd", &w.gustStrong, 0.0f, 3.0f, "%.2f");
      changed |= EditableSliderFloat("convective extra##wnd", &w.gustConvective, 0.0f, 3.0f, "%.2f");
      changed |= EditableSliderFloat("gale gust frac##wnd", &w.galeGust, 0.0f, 3.0f, "%.2f");
      changed |= EditableSliderFloat("front speed##wnd", &w.gustAdvect, 0.0f, 4.0f, "%.2fx mean");
      ImGui::SetItemTooltip("Gust fronts travel downwind at this fraction of the mean. Re-sums the\n"
                            "front clock from tick 0, so the pattern jumps once.");
      changed |= EditableSliderFloat("meander light##wnd", &w.wanderLight, 0.0f, 90.0f, "%.0f deg");
      changed |= EditableSliderFloat("meander strong##wnd", &w.wanderStrong, 0.0f, 90.0f, "%.0f deg");
      changed |= EditableSliderFloat("meander period##wnd", &w.wanderPeriod, 5.0f, 600.0f, "%.0f s");
      changed |= EditableSliderFloat("meander scale##wnd", &w.wanderWavelength, 5.0f, 1000.0f, "%.0f m");
      changed |= EditableSliderFloat("thermals##wnd", &w.thermalGust, 0.0f, 10.0f, "%.1f m/s");
      changed |= EditableSliderFloat("thermal cells##wnd", &w.thermalWavelength, 2.0f, 400.0f, "%.0f m");
      changed |= EditableSliderFloat("thermal period##wnd", &w.thermalPeriod, 1.0f, 300.0f, "%.0f s");
      ImGui::TextDisabled("(F5) wavelength %.1f m, evolution %.2f rad/s", w.gustWavelength, w.gustSpeed);
      ImGui::SetItemTooltip("gustWavelength and gustSpeed are compiled into the shaders: edit them in\n"
                            "tuning.json / the tuner's Wind tab and press F5.");
      ImGui::TreePop();
    }

    if (ImGui::TreeNode("Local winds##wnd")) {
      changed |= EditableSliderFloat("slope wind##wnd", &w.slopeWind, 0.0f, 10.0f, "%.1f m/s");
      ImGui::SetItemTooltip("Up the slopes by day, down them at night, on light-wind days.");
      changed |= EditableSliderFloat("night drainage##wnd", &w.slopeNight, 0.0f, 2.0f, "%.2fx");
      changed |= EditableSliderFloat("slope depth##wnd", &w.slopeDepth, 1.0f, 200.0f, "%.0f m");
      changed |= EditableSliderFloat("sea breeze##wnd", &w.seaBreeze, 0.0f, 15.0f, "%.1f m/s");
      changed |= EditableSliderFloat("land breeze##wnd", &w.seaNight, 0.0f, 2.0f, "%.2fx");
      changed |= EditableSliderFloat("breeze depth##wnd", &w.seaDepth, 1.0f, 400.0f, "%.0f m");
      changed |= EditableSliderFloat("fade by##wnd", &w.localFade, 0.5f, 40.0f, "%.1f m/s");
      ImGui::TreePop();
    }

    if (ImGui::TreeNode("Storms##wnd")) {
      changed |= EditableSliderFloat("cycle##wnd", &w.stormCycle, 30.0f, 3600.0f, "%.0f s");
      changed |= EditableSliderFloat("lull##wnd", &w.stormLull, 0.0f, 1.0f, "%.2fx");
      changed |= EditableSliderFloat("gust front##wnd", &w.stormFront, 1.0f, 5.0f, "%.2fx");
      changed |= EditableSliderFloat("post-front##wnd", &w.stormDecay, 1.0f, 4.0f, "%.2fx");
      changed |= EditableSliderFloat("storm gustiness##wnd", &w.stormGust, 0.0f, 3.0f, "%.2f");
      changed |= EditableSliderFloat("direction jump##wnd", &w.stormJump, 0.0f, 180.0f, "%.0f deg");
      changed |= ImGui::SliderInt("downbursts##wnd", &w.stormBursts, 0, 6);
      changed |= EditableSliderFloat("burst radius##wnd", &w.stormBurstRadius, 3.0f, 51.0f, "%.0f m");
      changed |= ImGui::Checkbox("front jet##wnd", &w.stormFrontJet);
      changed |= EditableSliderFloat("gale holds heading##wnd", &w.galeHold, 0.0f, 1.0f, "%.2f");
      ImGui::SeparatorText("lee turbulence");
      changed |= EditableSliderFloat("onset##wndlee", &w.leeOnset, 0.0f, 1.0f, "I %.2f");
      changed |= EditableSliderFloat("strength##wndlee", &w.leeStrength, 0.0f, 1.0f, "%.2f");
      changed |= EditableSliderFloat("slope##wndlee", &w.leeSlope, 0.05f, 3.0f, "%.2f");
      changed |= EditableSliderFloat("depth##wndlee", &w.leeDepth, 1.0f, 100.0f, "%.0f m");
      changed |= EditableSliderFloat("reverse flow##wndlee", &w.leeReverse, 0.0f, 1.0f, "%.2f");
      changed |= EditableSliderFloat("gust boost##wndlee", &w.leeGust, 0.0f, 4.0f, "%.2f");
      ImGui::TreePop();
    }

    if (ImGui::TreeNode("Visuals##wnd")) {
      changed |= EditableSliderFloat("streaks##wnd", &w.streakAlpha, 0.0f, 1.0f, "%.2f");
      ImGui::SetItemTooltip("Gust streaks: master visibility. 0 skips the pass entirely (live).");
      changed |= ImGui::SliderInt("streak count##wnd", &w.streakCount, 0, (int)kWindStreakCap);
      ImGui::SetItemTooltip("Slots at a 20 m radius: a larger radius scales them by its area\n"
                            "(the same density over the bigger disc), up to the pool.");
      changed |= EditableSliderFloat("spawn threshold##wnd", &w.streakThreshold, 0.0f, 20.0f, "%.1f m/s");
      ImGui::SetItemTooltip("A streak is born only where the gust excess (bands along the mean,\n"
                            "plus primitives) passes this. Calm days show nothing.");
      changed |= EditableSliderFloat("spawn span##wnd", &w.streakSpan, 0.1f, 30.0f, "%.1f m/s");
      ImGui::SetItemTooltip("Excess above the threshold at which every spawn attempt succeeds;\n"
                            "also how bright a streak is.");
      changed |= ImGui::SliderInt("trail points##wnd", &w.streakTrail, 2, (int)kWindStreakTrail);
      changed |= EditableSliderFloat("trail spacing##wnd", &w.streakSpacing, 0.01f, 0.2f, "%.3f s");
      changed |= EditableSliderFloat("lifetime##wnd", &w.streakLife, 0.2f, 6.0f, "%.1f s");
      changed |= EditableSliderFloat("radius##wnd", &w.streakRadius, 2.0f, 60.0f, "%.0f m");
      ImGui::SetItemTooltip("Live slots now: %u streaks + %u current tracers (pool %u).",
                            WindStreakSlots(CurrentTuning()), CurrentStreakSlots(CurrentTuning()),
                            kWindStreakCap);
      changed |= EditableSliderFloat("width##wnd", &w.streakWidth, 0.005f, 0.2f, "%.3f m");
      ImGui::TextDisabled("arrows: F4 or Debug draw; spacing/radius (F5) %.0f/%.0f vox", w.dbgWindSpacing,
                          w.dbgWindRadius);
      ImGui::TreePop();
    }
    if (changed) SetCurrentTuning(t);

    // ---- the per-tier force multipliers (unchanged: they ride TickParams) ----
    ImGui::SeparatorText("force multipliers");
    ImGui::SetNextItemWidth(-110);
    if (EditableSliderFloat("x voxels", &s.windGasScale, 0.0f, 16.0f, "%.2fx"))
      s.windTuningDirty = true;
    ImGui::SetItemTooltip(
        "How hard the wind pushes CA VOXELS - smoke, steam, fire, falling\n"
        "powder. Scales the drift-bias PROBABILITY. SETTLED voxels are\n"
        "untouched. 0 pins the CA tier still. Changes the world hash.");
    ImGui::SetNextItemWidth(-110);
    if (EditableSliderFloat("x particles", &s.windPartScale, 0.0f, 16.0f, "%.2fx"))
      s.windTuningDirty = true;
    ImGui::SetItemTooltip(
        "How hard the wind pushes the PARTICLE tier - debris, spray, MPM\n"
        "surface nodes. Scales the wind VELOCITY they are dragged toward.\n"
        "0 pins the particle tier still. Changes the world hash.");
    ImGui::SetNextItemWidth(-110);
    if (EditableSliderFloat("fall onset", &s.windDragRef, 1.0f, 120.0f, "%.0f m/s"))
      s.windTuningDirty = true;
    ImGui::SetItemTooltip(
        "The wind speed at which sim.windDrag counts in full; below it the\n"
        "drag ramps down, so calm air is ballistic. LOW makes ordinary\n"
        "weather floaty; HIGH means only a storm is felt.\n"
        "Terminal fall at the default 6 m/s weather: 120 -> 6.0,\n"
        "40 -> 5.7, 20 -> 2.9, 6 -> 0.86 vox/tick.");
    ImGui::TextDisabled("wind primitives (fans, vortices): Spawn page");
  }

  // ---- the map's references (ui/refs_ui.cpp) ----
  if (Section("References", false)) DrawRefsPage(s);

  if (Section("World file & reload")) {
    const float w = CellWidth(2);
    if (ImGui::Button("save world (F9)", ImVec2(w, 0))) s.saveWorld = true;
    ImGui::SameLine();
    if (ImGui::Button("load world (F10)", ImVec2(w, 0))) s.loadWorld = true;
    if (ImGui::Button("reload shaders (F5)", ImVec2(w, 0))) s.reloadShaders = true;
    ImGui::SameLine();
    if (ImGui::Button("reload materials (R)", ImVec2(w, 0))) s.reloadMaterials = true;
    if (ImGui::Button("reload environment + regen world (F7)", ImVec2(-FLT_MIN, 0)))
      s.regenWorld = true;
  }
}

// ---- page: View ------------------------------------------------------------
void Overlay::DrawDevView(UIState& s) {
  if (Section("Rendering")) {
    ImGui::Checkbox("shadows", &s.shadows);
    // Three radios: the arms are mutually exclusive. The metres come from
    // tuning so the labels track the sliders the moment F5 lands.
    const auto& rt = CurrentTuning().render;
    char nearLbl[24], farLbl[24];
    std::snprintf(nearLbl, sizeof nearLbl, "%.0f m", rt.shortRangeNearDist);
    std::snprintf(farLbl, sizeof farLbl, "%.0f m", rt.shortRangeDist);
    ImGui::TextUnformatted("short range");
    ImGui::SameLine();
    if (ImGui::RadioButton("off##sr", !s.shortRange)) s.shortRange = false;
    ImGui::SameLine();
    if (ImGui::RadioButton(nearLbl, s.shortRange && s.shortRangeNear)) {
      s.shortRange = true;
      s.shortRangeNear = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(farLbl, s.shortRange && !s.shortRangeNear)) {
      s.shortRange = true;
      s.shortRangeNear = false;
    }
    ImGui::TextDisabled("draw distance %.0f m", s.renderRangeM);
    ImGui::SetItemTooltip(
        "Normally the far cascade's FILLED radius, so it dips while the\n"
        "cascade refills after a teleport and climbs back when every level\n"
        "has landed. On a short-range arm it is that arm's hard ceiling on\n"
        "every ray - a PERF mode, not a fog filter. Shape it under Rendering:\n"
        "shortRangeDist, shortRangeNearDist, shortRangeFogStart/Density.");
  }
  if (Section("Debug draw")) {
    ImGui::Checkbox("corner readout (fps + looking at)", &s.showCornerReadout);
    ImGui::SetItemTooltip("Top-right, stays up with this panel closed.");
    ImGui::Checkbox("collision boxes (F3)", &s.showCollisionBoxes);
    ImGui::SetItemTooltip(
        "Wireframes around every physics collider, from the actual Jolt shape:\n"
        "green = avatar parts (a held item too), cyan = mob limbs,\n"
        "yellow = loose debris. Drawn THROUGH walls on purpose.");
    ImGui::Checkbox("active voxels", &s.showDirtyVoxels);
    ImGui::SetItemTooltip("Red wireframe on every voxel the CA wrote this tick.\n"
                          "Combine with F6 (dirty chunks) to see cause and effect.");
    ImGui::Checkbox("charge view (F11)", &s.showChargeView);
    ImGui::SetItemTooltip("The charge field's P in false colour, log scale:\n"
                          "blue = a few units, yellow = a spark (200),\n"
                          "red = an arc (2,000), white = lightning (30,000).\n"
                          "Uncharged surfaces are dim grey. Render-only.");
    static constexpr const char* kFieldVizTip =
        "An arrow per lattice point around you, coloured by speed.\n"
        "WIND samples the same windAt() the grass sway does (full scale 24 m/s).\n"
        "CURRENT samples the same currentAt() waves and floating debris use\n"
        "(full scale 4 m/s); still water is honestly empty.\n"
        "Spacing and radius: Wind tab and render.dbgCurrent* (F5).\n"
        "F5 re-seeds this from wind.dbgWindField / render.dbgCurrentField.";
    ImGui::TextUnformatted("vector field (F4)");
    ImGui::SetItemTooltip("%s", kFieldVizTip);
    ImGui::SameLine();
    ImGui::RadioButton("off##fieldviz", &s.fieldViz, UIState::kFieldVizOff);
    ImGui::SameLine();
    ImGui::RadioButton("wind##fieldviz", &s.fieldViz, UIState::kFieldVizWind);
    ImGui::SameLine();
    ImGui::RadioButton("current##fieldviz", &s.fieldViz, UIState::kFieldVizCurrent);
    ImGui::Checkbox("NPC AI viz (path / target / band)", &s.showAiDebug);
    ImGui::Indent();
    ImGui::Checkbox("...include the range-band ring", &s.showAiRing);
    ImGui::Unindent();
  }
  if (Section("Player")) {
    ImGui::Checkbox("fly (V)", &s.fly);
    ImGui::SameLine();
    if (ImGui::Button("ragdoll me")) s.ragdollMe = true;
    ImGui::SetItemTooltip("go limp for ragdoll.devSeconds, then get back up");
    ImGui::Text("pos %.0f %.0f %.0f  (%s)", s.playerPos[0], s.playerPos[1], s.playerPos[2],
                s.fly ? "fly" : "walk");
    const ImVec4 c = s.ledgeState == 2   ? ImVec4(0.35f, 1.0f, 0.45f, 1.0f)
                     : s.ledgeState == 1 ? ImVec4(1.0f, 0.85f, 0.30f, 1.0f)
                                         : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    ImGui::TextColored(c, "ledge: %s", s.ledgeText.c_str());
  }
}

// ---- page: Magic -----------------------------------------------------------
void Overlay::DrawDevMagic(UIState& s) {
  if (Section("Mana & health")) {
    // The CROSSOVER: where the running cost stops coming out of mana and
    // starts coming out of health, on one shared axis.
    ImGui::Text("magic %s", s.magicMode ? "ON" : "off");
    KeyHint("Z toggles");
    {
      const float w = ImGui::GetContentRegionAvail().x;
      const float h = 14.0f;
      ImVec2 p = ImGui::GetCursorScreenPos();
      ImDrawList* d = ImGui::GetWindowDrawList();
      const int32_t poolMax = s.manaMax > 0 ? s.manaMax : 1;
      const int32_t span = poolMax + (s.health > 0 ? s.health : 0);
      auto frac = [&](int32_t v) { return span > 0 ? (float)v / (float)span : 0.0f; };
      d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(28, 32, 46, 255));
      const float manaEdge = p.x + w * frac(poolMax);
      d->AddRectFilled(ImVec2(manaEdge, p.y), ImVec2(p.x + w, p.y + h), IM_COL32(52, 22, 24, 255));
      d->AddRectFilled(p, ImVec2(p.x + w * frac(s.mana), p.y + h), IM_COL32(70, 130, 235, 255));
      d->AddRectFilled(ImVec2(manaEdge, p.y), ImVec2(manaEdge + w * frac(s.health), p.y + h),
                       IM_COL32(190, 60, 60, 255));
      if (s.spellCost > 0) {
        const int32_t fromMana = s.spellCost < s.mana ? s.spellCost : s.mana;
        const int32_t fromHealth = s.spellCost - fromMana;
        const float x0 = p.x + w * frac(s.mana - fromMana);
        d->AddRectFilled(ImVec2(x0, p.y), ImVec2(p.x + w * frac(s.mana), p.y + h),
                         IM_COL32(150, 200, 255, 255));
        if (fromHealth > 0) {
          const float hx = manaEdge + w * frac(fromHealth < s.health ? fromHealth : s.health);
          d->AddRectFilled(ImVec2(manaEdge, p.y), ImVec2(hx, p.y + h), IM_COL32(255, 140, 60, 255));
        }
        d->AddLine(ImVec2(manaEdge, p.y - 2), ImVec2(manaEdge, p.y + h + 2),
                   IM_COL32(255, 255, 255, 220), 2.0f);
      }
      ImGui::Dummy(ImVec2(w, h + 4));
    }
    ImGui::Text("mana %d/%d   health %d   cost %d%s", s.mana, s.manaMax, s.health, s.spellCost,
                s.spellPriceUnknown ? " + ?" : "");
    if (s.spellCost > 0 || s.spellPriceUnknown)
      ImGui::TextDisabled("  word %d + tariff %d + carry %d%s", s.spellWord, s.spellTariff,
                          s.spellCarry,
                          s.spellPriceUnknown ? "   (anything: priced when it lands)" : "");
    if (s.spellLastBillAge < 2.5f && s.spellLastBill > 0)
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "billed %d on resolve", s.spellLastBill);
    if (s.spellCost > s.mana + s.health)
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "FATAL - this will kill you");
    else if (s.spellCost > s.mana)
      ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f), "unstable - %d from your body",
                         s.spellCost - s.mana);
  }
  // Requests only; main.cpp applies. ResolveCast adds mana + health in an
  // int32, so the ceiling stays below overflow.
  if (Section("Dev: mana pool", false)) {
    constexpr int32_t kDevManaCeiling = 1 << 30;
    if (s.devManaMaxEdit <= 0) s.devManaMaxEdit = s.manaPoolMax > 0 ? s.manaPoolMax : 100;
    ImGui::SetNextItemWidth(140.0f);
    int edit = s.devManaMaxEdit;
    if (ImGui::InputInt("max", &edit, 100, 10000)) s.devManaMaxEdit = edit;
    s.devManaMaxEdit = std::clamp(s.devManaMaxEdit, 1, kDevManaCeiling);
    ImGui::SameLine();
    if (ImGui::Button("apply max")) s.devManaMaxRequest = s.devManaMaxEdit;
    ImGui::SameLine();
    if (ImGui::Button("fill")) s.devManaFill = true;
    // Presets apply AND fill: nobody sets 1e9 and then waits for regen.
    struct Preset { const char* label; int32_t value; };
    const Preset presets[] = {{"100", 100},      {"1k", 1000},        {"100k", 100000},
                              {"10M", 10000000}, {"1G", 1000000000}, {"2^30", kDevManaCeiling}};
    const float w = CellWidth(6);
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
      if (i > 0) ImGui::SameLine();
      if (ImGui::Button(presets[i].label, ImVec2(w, 0))) {
        s.devManaMaxEdit = presets[i].value;
        s.devManaMaxRequest = presets[i].value;
        s.devManaFill = true;
      }
    }
    ImGui::Checkbox("infinite (refill to max every tick)", &s.devManaInfinite);
  }
  // demons D2 (docs/PLAN_demons.md): a demon's NAME is its summon glyph, and
  // names are learned (the Harrowby cellar book), not granted at start. This
  // is the shortcut. Requests only; the tick applies it (session.cpp).
  if (Section("Dev: demon names", false)) {
    if (ImGui::Button("learn all demon names", ImVec2(-FLT_MIN, 0))) s.devLearnDemonNames = true;
    ImGui::SetItemTooltip("grants every summon_<demon> glyph in glyphs.json\n"
                          "(saved with your kit, like a name read in a book)");
    if (!s.devDemonNamesStatus.empty()) ImGui::TextWrapped("%s", s.devDemonNamesStatus.c_str());
  }
  if (Section("Spell")) {
    // THE BRACKET TEXT STAYS: it is what the oracle compares and what a tree
    // is read off; the player-facing surface is the grimoire page's canvas.
    if (!s.spellText.empty()) {
      ImGui::TextWrapped("held: %s", s.spellText.c_str());
      ImGui::TextDisabled("%s", s.spellVerdict.c_str());
    } else {
      ImGui::TextDisabled("held: (nothing)");
      ImGui::TextDisabled("Z opens the spell bar, a number selects,");
      ImGui::TextDisabled("Q / E put it in a hand; RMB / LMB cast it");
    }
    if (!s.glyphSlots.empty()) {
      for (int bank = 0; bank < 2; bank++) {
        std::string strip = bank == 0 ? "1-0:   " : "S+1-0: ";
        bool any = false;
        for (size_t i = (size_t)bank * 10; i < s.glyphSlots.size() && i < (size_t)(bank + 1) * 10; i++) {
          if (s.glyphSlots[i].empty()) continue;
          any = true;
          const bool page = i < s.glyphSlotKinds.size() && s.glyphSlotKinds[i] == 2;
          const bool sel = (int)i == s.glyphSelected;
          strip += (sel ? "*" : "") + std::to_string((i + 1) % 10) + ":" +
                   (page ? "[" : "") + s.glyphSlots[i] + (page ? "]" : "") + "  ";
        }
        if (!any) continue;
        if (s.glyphBankB == (bank == 1)) ImGui::TextWrapped("%s", strip.c_str());
        else {
          ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
          ImGui::TextWrapped("%s", strip.c_str());
          ImGui::PopStyleColor();
        }
      }
    }
    if (s.spellNoteAge < 3.0f && !s.spellNote.empty())
      ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.5f, 1.0f), "%s", s.spellNote.c_str());
  }
  if (Section("Live effects")) {
    if (!s.spellStatuses.empty()) {
      ImGui::Text("sustaining (%d reserved, Delete drops the newest):", s.manaReserved);
      for (const std::string& st : s.spellStatuses) ImGui::TextDisabled("  %s", st.c_str());
    }
    if (s.spellRefused > 0)
      ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "ward refused %d", s.spellRefused);
    ImGui::Text("projectiles %d", s.liveProjectiles);
    if (s.spellOpsDropped > 0) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "  %d ops dropped", s.spellOpsDropped);
    }
    // `wake` is the rule-2 number: chunks held awake against sim.windWakeChunks.
    ImGui::Text("wind primitives %d (wake %d chunks)", s.windPrims, s.windWakeChunks);
    if (s.windPrimsDropped > 0) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "  %d refused (cap)", s.windPrimsDropped);
    }
  }
}

// ---- page: Debug -----------------------------------------------------------
void Overlay::DrawDevDebug(UIState& s) {
  if (Section("Stats")) {
    ImGui::Text("%.0f fps  (%.1f ms avg, %.2f ms tick cpu)", s.fps, s.frameMs, s.tickCpuMs);
    // `worst` is ONE frame of the last half second; p95/p99 are over ~512
    // frames and say whether a stutter is systematic.
    ImGui::Text("frame ms   p95 %.0f   p99 %.0f   worst %.0f", s.frameMsP95, s.frameMsP99,
                s.frameMsWorst);
    ImGui::Text("tick %u   active chunks %u / %u", s.tick, s.activeChunks, s.totalChunks);
    ImGui::Text("voxels %.2f M   particles %u", s.voxelTotal / 1e6, s.particleCount);
    ImGui::Text("hash %08x %s", s.worldHash, s.mirrorValid ? "" : "(mirror pending)");
    ImGui::Text("debris bodies %u (%u awake)   mobs %u / %u", s.bodyCount,
                s.activeBodyCount, s.mobCount, kMaxLiveMobs);
    // CHUNK TICKETS (docs/PLAN_chunk_tickets.md): the boxes outside the
    // window the CA is still running. At rest this reads 0 live — the rule-2
    // claim the `sleep` gate asserts.
    ImGui::Text("tickets %u / %u live (%u releasing)   run: %llu on, %llu off, %llu refused",
                s.ticketsLive, s.ticketsCap, s.ticketsReleasing,
                (unsigned long long)s.ticketsActivated,
                (unsigned long long)s.ticketsReleased,
                (unsigned long long)s.ticketsRefused);
    if (s.ticketLandingsParked)
      ImGui::Text("far landings parked %u (wait for residency)", s.ticketLandingsParked);
    for (const std::string& line : s.ticketLines) ImGui::TextDisabled("  %s", line.c_str());
  }
  // The swing readout makes the input FALSIFIABLE: "the game misread my flick"
  // vs "I misjudged the distance" is answered by the phase and the speed the
  // state machine actually measured.
  if (Section("Hotbar & swing")) {
    std::string strip;
    for (size_t i = 0; i < s.itemNames.size(); i++) {
      if (s.itemNames[i].empty()) continue;
      const bool sel = (int)i == s.itemSelected;
      strip += (sel ? "[" : " ") + std::to_string((i + 1) % 10) + ":" + s.itemNames[i] +
               (sel ? "] " : "  ");
    }
    if (!strip.empty()) ImGui::TextWrapped("%s", strip.c_str());
    if (s.swingPhase && s.swingPhase[0]) {
      ImGui::Text("swing %s", s.swingPhase);
      ImGui::SameLine();
      ImGui::TextDisabled("  mouse %.0f px/s", s.swingSpeed);
      if (!s.swingStyle.empty()) ImGui::TextDisabled("%s", s.swingStyle.c_str());
    } else {
      ImGui::TextDisabled("no swing");
    }
  }
  if (Section("NPC log", false)) {
    ImGui::Text("attack requests: %d", s.aiAttackCount);
    ImGui::TextWrapped("last: %s", s.aiLastAttack.empty() ? "(none yet)" : s.aiLastAttack.c_str());
    ImGui::Text("parries: %d", s.aiBlockCount);
    ImGui::TextWrapped("last: %s", s.aiLastBlock.empty() ? "(none yet)" : s.aiLastBlock.c_str());
    ImGui::TextDisabled("blocking is EMERGENT: a blade in the path stops the blow");
  }
  if (Section("Tuning windows")) {
    const float w = CellWidth(3);
    if (ToggleButton("Combat", s.combatWindowOpen, w)) s.combatWindowOpen = !s.combatWindowOpen;
    ImGui::SetItemTooltip("stroke, damage and feel tuning (live)");
    ImGui::SameLine();
    if (ToggleButton("NPC AI", s.aiWindowOpen, w)) s.aiWindowOpen = !s.aiWindowOpen;
    ImGui::SetItemTooltip("live mobs (apply a behaviour) + profile editor");
    ImGui::SameLine();
    if (ToggleButton("MPM fluid", s.fluidWindowOpen, w)) s.fluidWindowOpen = !s.fluidWindowOpen;
  }
  if (Section("Keys")) {
    static const char* const kKeys[][2] = {
        {"F1", "this panel"},        {"F2", "play / dev controls"},
        {"Tab", "cycle tool"},       {"1-8", "brush material"},
        {"[ ]", "brush radius"},     {"Esc", "free cursor"},
        {"P / N", "pause / step"},   {"V", "fly"},
        {"G", "grenade"},            {"X", "detonate at crosshair"},
        {"F", "laser"},              {"M", "world map (Shift+M: spawn mob)"},
        {"K", "rolling sphere"},     {"B", "place"},
        {"Z", "magic mode"},         {"F3", "collision boxes"},
        {"F4", "vector field"},      {"F5", "reload shaders + tuning"},
        {"R", "reload materials"},   {"F7", "regen world"},
        {"F9 / F10", "save / load"}, {"F12", "bug report (screenshot + text)"},
    };
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit)) {
      for (const auto& k : kKeys) {
        ImGui::TableNextColumn();
        ImGui::TextColored(V4(ui::ColGold()), "%s", k[0]);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", k[1]);
      }
      ImGui::EndTable();
    }
  }
}

void Overlay::Draw(UIState& s) {
  // The character screen owns the frame while it is open: no crosshair (the
  // cursor is free), and it is drawn BEFORE the dev panel so the dev panel
  // stays reachable on top of it — F1 is not supposed to become unavailable
  // just because a menu is up.
  if (s.inventoryOpen) {
    ImGui::PushFont(ui::FontLarge());
    DrawInventoryScreen(s);
    // demons D5: the contract editor, opened from the spellbook's header.
    DrawContractEditor(s);
    ImGui::PopFont();
  } else if (s.worldMap.open) {
    // THE WORLD MAP (M; ui/map_ui.h) owns the frame the same way.
    ImGui::PushFont(ui::FontLarge());
    DrawWorldMapScreen(s);
    ImGui::PopFont();
  } else if (s.talk.open) {
    // A CONVERSATION owns the frame the same way: the cursor is free for the
    // choice rows, so there is no crosshair (ui/dialogue_ui.h).
    ImGui::PushFont(ui::FontLarge());
    DrawDialoguePanel(s);
    // demons D5: talking to a contained demon -- the circle, the gaze, the
    // contract picker (ui/contract_ui.h).
    DrawDemonTalkStrip(s);
    ImGui::PopFont();
  } else {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 c = ImGui::GetIO().DisplaySize;
    c.x *= 0.5f;
    c.y *= 0.5f;
    dl->AddCircleFilled(c, 2.5f, IM_COL32(255, 255, 255, 200));
  }

  // ---- corner readout: fps + what the crosshair is on, panel open or not ----
  if (s.showCornerReadout) {
    ImGui::PushFont(ui::FontSmall());
    ImDrawList* d = ImGui::GetForegroundDrawList();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float pad = 10.0f, icon = 12.0f, gap = 4.0f;
    char fps[48];
    std::snprintf(fps, sizeof fps, "%.0f fps  %.1f ms", s.fps, s.frameMs);
    const bool hasMat = s.hoverMat > 0 && s.hoverMat < (int)s.materialNames.size();
    const std::string mat = hasMat ? MatLabel(s, s.hoverMat) : std::string("---");
    const ImVec2 fs = ImGui::CalcTextSize(fps);
    const ImVec2 ms = ImGui::CalcTextSize(mat.c_str());
    const float lineH = std::max(fs.y, icon);
    const float matW = ms.x + (hasMat ? icon + gap : 0.0f);
    const float boxW = std::max(fs.x, matW);
    const float x1 = disp.x - pad, y0 = pad;
    d->AddRectFilled(ImVec2(x1 - boxW - 6, y0 - 4), ImVec2(x1 + 4, y0 + lineH * 2 + gap + 4),
                     IM_COL32(0, 0, 0, 140), 3.0f);
    d->AddText(ImVec2(x1 - fs.x, y0), IM_COL32(235, 235, 235, 255), fps);
    const float y1 = y0 + lineH + gap;
    if (hasMat) MatIcon(d, ImVec2(x1 - matW, y1 + (lineH - icon) * 0.5f), icon, s, s.hoverMat);
    d->AddText(ImVec2(x1 - ms.x, y1),
               hasMat ? IM_COL32(235, 225, 190, 255) : IM_COL32(140, 140, 140, 255),
               mat.c_str());
    ImGui::PopFont();
  }

  if (!s.visible) return;

  ImGui::PushFont(ui::FontSmall());

  // Pinned to the left edge, top to just above the HUD; only the width is the
  // user's (drag the right edge).
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  const float panelH = std::floor(
      sHudTop > disp.y * 0.5f && sHudTop < disp.y ? sHudTop : disp.y);
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(480, panelH), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(380, panelH),
                                      ImVec2(std::max(380.0f, disp.x * 0.6f), panelH));
  ImGui::Begin("##devpanel", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  sPanelW = ImGui::GetWindowWidth();

  // ---- the fixed header: what you glance at, and the two switches ----------
  {
    ImGui::TextColored(V4(ui::ColGoldPale()), "SANDVOX");
    ImGui::SameLine();
    ImGui::TextDisabled("dev");
    char fps[64];
    std::snprintf(fps, sizeof fps, "%.0f fps  %.1f ms  p99 %.0f", s.fps, s.frameMs,
                  s.frameMsP99);
    KeyHint(fps);
    ImGui::TextDisabled("tick %u   chunks %u   mobs %u", s.tick, s.activeChunks, s.mobCount);
    // Crosshair readout with the material's own icon.
    if (s.hoverMat > 0 && s.hoverMat < (int)s.materialNames.size()) {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      MatIcon(ImGui::GetWindowDrawList(), ImVec2(p.x + 1, p.y + 2), 12.0f, s, s.hoverMat);
      ImGui::Dummy(ImVec2(14, 14));
      if (ImGui::IsItemHovered()) MatTooltip(s, s.hoverMat);
      ImGui::SameLine();
      ImGui::Text("%s", MatLabel(s, s.hoverMat).c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("[%d %d %d]  %.1fm", s.hoverCell[0], s.hoverCell[1],
                          s.hoverCell[2], s.hoverDist);
    } else {
      ImGui::TextDisabled("looking at ---");
    }
    const float w = CellWidth(3);
    if (ToggleButton(s.paused ? "resume (P)" : "pause (P)", s.paused, w)) s.paused = !s.paused;
    ImGui::SameLine();
    if (ImGui::Button("step (N)", ImVec2(w, 0))) s.stepOnce = true;
    ImGui::SameLine();
    if (ToggleButton(s.devControls ? "dev mode (F2)" : "play mode (F2)", s.devControls, w))
      s.devControls = !s.devControls;
    ImGui::SetItemTooltip("play = hands only, number row picks the hotbar,\n"
                          "no brush / spawn / fly / tool keys");
  }

  // ---- the page strip --------------------------------------------------------
  static const char* const kDevTabs[] = {"Paint", "Spawn", "World", "View", "Magic", "Debug"};
  constexpr int kTabs = 6;
  ImGui::Spacing();
  {
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(a.x, a.y - 3), ImVec2(a.x + ImGui::GetContentRegionAvail().x, a.y - 3),
        ui::ColBronze(), 2.0f);
    const float w = CellWidth(kTabs);
    if (s.devTab < 0 || s.devTab >= kTabs) s.devTab = 0;
    for (int i = 0; i < kTabs; i++) {
      if (i) ImGui::SameLine();
      if (ToggleButton(kDevTabs[i], s.devTab == i, w, 26)) s.devTab = i;
    }
  }

  // ---- the page body: the only part that scrolls ----------------------------
  ImGui::BeginChild("##devbody", ImVec2(0, 0), ImGuiChildFlags_Borders);
  switch (s.devTab) {
    case 0: DrawDevPaint(s); break;
    case 1: DrawDevSpawn(s); break;
    case 2: DrawDevWorld(s); break;
    case 3: DrawDevView(s); break;
    case 4: DrawDevMagic(s); break;
    default: DrawDevDebug(s); break;
  }
  ImGui::EndChild();
  ImGui::End();

  // ---- separate MPM fluid tuning window ----
  if (s.fluidWindowOpen) {
    ImGui::SetNextWindowPos(ImVec2(sPanelW + 12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("MPM Fluid Tuning", &s.fluidWindowOpen)) {
      ui::KeepWindowOnScreen(4.0f);  // a saved position may be off this monitor
      if (ImGui::Button("Apply")) s.fluidTuningDirty = true;
      ImGui::SameLine();
      ImGui::TextDisabled("recompiles shaders");
      ImGui::Separator();

      auto fslider = [](const char* label, float* v, float lo, float hi) {
        EditableSliderFloat(label, v, lo, hi, "%.2f");
      };
      auto islider = [](const char* label, int* v, int lo, int hi) {
        EditableSliderInt(label, v, lo, hi);
      };
      auto fcheck = [](const char* label, int* v) {
        bool on = *v != 0;
        if (ImGui::Checkbox(label, &on)) *v = on ? 1 : 0;
      };
      auto fcolor = [](const char* label, float col[3]) {
        ImGui::ColorEdit3(label, col, ImGuiColorEditFlags_Float);
      };

      if (ImGui::BeginTabBar("##fluidtabs")) {
        if (ImGui::BeginTabItem("Sim")) {
          ImGui::Text("particles: %u / 262144", s.fluidCount);
          ImGui::Separator();
          if (ImGui::CollapsingHeader("Core", ImGuiTreeNodeFlags_DefaultOpen)) {
            fslider("gravity",      &s.fGravity,      0.0f, 1800.0f);
            fslider("stiffness",    &s.fStiffness,    0.0f, 43200.0f);
            fslider("rest density", &s.fRestDensity,   1.0f, 32.0f);
            islider("EOS power",    &s.fEosPower,      1, 7);
            fslider("cohesion",     &s.fCohesion,      0.0f, 14400.0f);
            fslider("attract same", &s.fAttractSame,  -7200.0f, 7200.0f);
            fslider("attract diff", &s.fAttractDiff,  -7200.0f, 7200.0f);
            fslider("viscosity",    &s.fViscosity,     0.0f, 240.0f);
            fslider("damping",      &s.fDamping,       0.0f, 20.0f);
          }
          if (ImGui::CollapsingHeader("Splash")) {
            fslider("splash rate",    &s.fSplashRate,       0.0f, 60.0f);
            fslider("splash speed",   &s.fSplashSpeed,      0.0f, 90.0f);
            fslider("splash surface", &s.fSplashMaxDensity, 0.0f, 2.0f);
            fslider("splash life",    &s.fSplashLife,       0.05f, 8.5f);
            islider("splash size",    &s.fSplashScaleIdx,   0, 3);
          }
          if (ImGui::CollapsingHeader("Foam / Diffuse")) {
            fslider("trapped-air rate",  &s.fFoamRate,      0.0f, 180.0f);
            fslider("wave-crest rate",   &s.fFoamCrestRate, 0.0f, 180.0f);
            fslider("trapped min",       &s.fTrappedMin,    0.0f, 400.0f);
            fslider("trapped max",       &s.fTrappedMax,    0.0f, 400.0f);
            fslider("crest min",         &s.fCrestMin,      0.0f, 64.0f);
            fslider("crest max",         &s.fCrestMax,      0.0f, 64.0f);
            fslider("energy min",        &s.fFoamEnergyMin, 0.0f, 8100.0f);
            fslider("energy max",        &s.fFoamEnergyMax, 0.0f, 8100.0f);
            fslider("foam life",         &s.fFoamLife,      0.05f, 8.5f);
            fslider("foam life (weak)",  &s.fFoamLifeMin,   0.05f, 8.5f);
            fslider("bubble buoyancy",   &s.fBubbleBuoyancy,-4.0f, 8.0f);
            fslider("foam drag",         &s.fFoamDrag,      0.0f, 1.0f);
            fslider("bubble threshold",  &s.fBubbleDensity, 0.0f, 4.0f);
            fslider("spray threshold",   &s.fSprayDensity,  0.0f, 4.0f);
            islider("foam particle size",&s.fFoamScaleIdx,   0, 3);
          }
          if (ImGui::CollapsingHeader("Settle / Excite")) {
            fcheck("excite mode",       &s.fExciteMode);
            fslider("settle below",     &s.fSettleEps,   0.05f, 20.0f);
            fslider("wake above",       &s.fWakeSpeed,   0.1f, 50.0f);
            islider("settle ticks",     &s.fSettleTicks, 8, 600);
          }
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Look")) {
          if (ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Draw mode is a set of named stops, not a level, so it gets a
            // combo rather than the 0-1 slider it used to be: a float slider
            // parked at 1.4 is not any of the four modes. The underlying
            // tuning value stays a float (tuning.json compatibility) — see
            // Tuning::Render::fluidSurface for what each stop means.
            {
              const char* kDrawModes[] = {"cubes (per particle)",
                                          "surface (smooth)",
                                          "voxels - 1/2 cell",
                                          "voxels - 1 cell"};
              int mode = (int)(s.fSurface + 0.5f);
              mode = mode < 0 ? 0 : (mode > 3 ? 3 : mode);
              if (ImGui::Combo("draw mode##l", &mode, kDrawModes,
                               IM_ARRAYSIZE(kDrawModes)))
                s.fSurface = (float)mode;
            }
            fslider("surface threshold##l",&s.fIso,     0.08f, 1.0f);
            fslider("smoothing##l",       &s.fSmooth,   0.4f, 3.0f);
            fslider("refraction index##l",&s.fIor,      1.01f, 2.0f);
            fslider("clarity##l",         &s.fClarity,  0.05f, 20.0f);
            fslider("reflection##l",      &s.fReflect,  0.0f, 2.0f);
            fslider("sun glint##l",       &s.fSpecular, 0.0f, 4.0f);
            fslider("shimmer##l",         &s.fWobble,   0.0f, 2.0f);
          }
          if (ImGui::CollapsingHeader("Colour")) {
            fcolor("shallow tint##l", s.fShallow);
            fcolor("deep tint##l",    s.fDeep);
            fslider("gradient depth##l",   &s.fDepth,       0.05f, 20.0f);
            fslider("gradient strength##l",&s.fGradientStr,  0.0f, 1.0f);
          }
          if (ImGui::CollapsingHeader("Foam (render)")) {
            fslider("foam amount##l",  &s.fRFoam,        0.0f, 2.0f);
            fslider("foam field##l",   &s.fRFoamField,   0.0f, 3.0f);
            fslider("foam break-up##l",&s.fRFoamTexture,  0.0f, 1.0f);
            fslider("foam speed##l",   &s.fRFoamSpeed,   1.0f, 90.0f);
          }
          if (ImGui::CollapsingHeader("Debug cubes")) {
            fslider("cube size##l",          &s.fParticleSize, 0.2f, 1.2f);
            fslider("cube stretch##l",       &s.fStretch,      0.0f, 1.5f);
            fslider("cube pressure shade##l",&s.fDensityShade, 0.0f, 1.0f);
          }
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
    }
    ImGui::End();
  }

  // ---- NPC AI window (game/ai_behavior.h) ---------------------------------
  //
  // Same shape as the fluid window above, for the same reason: the overlay owns
  // no game state. Buttons set one-shot bools, sliders write UIState mirrors,
  // and main.cpp is the only thing that touches MobSystem or the behaviour
  // library. Scrolling is ImGui's own — see the note in overlay.h about why
  // installing a GLFW scroll callback here would freeze the wheel everywhere.
  if (s.aiWindowOpen) {
    ImGui::SetNextWindowPos(ImVec2(sPanelW + 12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 720), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("NPC AI", &s.aiWindowOpen)) {
      ui::KeepWindowOnScreen(4.0f);  // a saved position may be off this monitor
      if (ImGui::BeginTabBar("##aitabs")) {
        // ---- Mobs --------------------------------------------------------
        if (ImGui::BeginTabItem("Mobs")) {
          if (s.aiMobIds.empty()) {
            ImGui::TextDisabled("no live mobs");
          } else {
            if (s.aiMobSelected >= (int)s.aiMobIds.size()) s.aiMobSelected = 0;
            // A child region so a crowd scrolls instead of pushing the
            // behaviour controls off the bottom of the window.
            ImGui::BeginChild("##ailist", ImVec2(0, 190), true);
            for (int i = 0; i < (int)s.aiMobLabels.size(); i++) {
              // PushID per row: two creatures on the same profile produce the
              // same label text, and ImGui hashes the label — without this the
              // selection sticks on the first of them.
              ImGui::PushID(i);
              if (ImGui::Selectable(s.aiMobLabels[i].c_str(),
                                    i == s.aiMobSelected))
                s.aiMobSelected = i;
              ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::Separator();
            if (!s.aiProfileNames.empty()) {
              if (s.aiBehaviorPick >= (int)s.aiProfileNames.size())
                s.aiBehaviorPick = 0;
              ImGui::TextUnformatted("behaviour");
              ImGui::SameLine();
              // "##" so this combo cannot hash to the same id as the profile
              // combo on the next tab.
              if (ImGui::BeginCombo("##aibeh",
                                    s.aiProfileNames[s.aiBehaviorPick].c_str())) {
                for (int i = 0; i < (int)s.aiProfileNames.size(); i++) {
                  ImGui::PushID(i);
                  if (ImGui::Selectable(s.aiProfileNames[i].c_str(),
                                        i == s.aiBehaviorPick))
                    s.aiBehaviorPick = i;
                  ImGui::PopID();
                }
                ImGui::EndCombo();
              }
              ImGui::SameLine();
              if (ImGui::Button("apply to selected")) s.aiApplyBehavior = true;
            }
          }
          ImGui::EndTabItem();
        }

        // ---- Profile -----------------------------------------------------
        if (ImGui::BeginTabItem("Profile")) {
          if (s.aiProfileNames.empty()) {
            ImGui::TextDisabled("assets/mobs/behaviors.json has no profiles");
          } else {
            if (s.aiProfileEdit >= (int)s.aiProfileNames.size())
              s.aiProfileEdit = 0;
            ImGui::TextUnformatted("editing");
            ImGui::SameLine();
            if (ImGui::BeginCombo("##aiprof",
                                  s.aiProfileNames[s.aiProfileEdit].c_str())) {
              for (int i = 0; i < (int)s.aiProfileNames.size(); i++) {
                ImGui::PushID(i);
                if (ImGui::Selectable(s.aiProfileNames[i].c_str(),
                                      i == s.aiProfileEdit)) {
                  s.aiProfileEdit = i;
                  s.aiProfileReseat = true;   // reload mirrors from the library
                }
                ImGui::PopID();
              }
              ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Save")) s.aiSaveBehaviors = true;
            if (!s.aiSaveStatus.empty()) {
              ImGui::SameLine();
              ImGui::TextDisabled("%s", s.aiSaveStatus.c_str());
            }
            ImGui::TextDisabled("sliders are LIVE: every mob on this profile");
            ImGui::TextDisabled("updates as you drag. Save writes the JSON.");
            ImGui::Separator();

            // Every slider latches aiTuningDirty on its own return value —
            // the wind panel's shape, not the fluid panel's Apply button. These
            // knobs cost nothing to apply (no shader touches them), and an AI
            // you have to press Apply to feel is an AI you cannot tune.
            auto f = [&s](const char* label, float* v, float lo, float hi) {
              if (EditableSliderFloat(label, v, lo, hi, "%.2f"))
                s.aiTuningDirty = true;
            };
            auto i32 = [&s](const char* label, int* v, int lo, int hi) {
              if (EditableSliderInt(label, v, lo, hi)) s.aiTuningDirty = true;
            };
            auto b = [&s](const char* label, bool* v) {
              if (ImGui::Checkbox(label, v)) s.aiTuningDirty = true;
            };

            if (ImGui::CollapsingHeader("Perception",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
              f("sight range (vox)", &s.aiSightRange, 0.0f, 120.0f);
              f("FOV (deg, 360 = all round)", &s.aiFovDegrees, 20.0f, 360.0f);
              b("needs line of sight", &s.aiRequireLos);
              i32("alert decay (ticks)", &s.aiAlertDecayTicks, 0, 600);
              f("keep-range hysteresis", &s.aiKeepRangeScale, 1.0f, 3.0f);
            }
            if (ImGui::CollapsingHeader("Movement",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
              b("can move its feet", &s.aiMobile);
              f("range min (vox)", &s.aiRangeMin, 0.0f, 60.0f);
              f("range max (vox)", &s.aiRangeMax, 0.0f, 60.0f);
              f("band deadband (vox)", &s.aiBandSlack, 0.0f, 8.0f);
              f("approach speed x", &s.aiApproachSpeed, 0.0f, 2.0f);
              f("strafe speed x", &s.aiStrafeSpeed, 0.0f, 2.0f);
              f("retreat speed x", &s.aiRetreatSpeed, 0.0f, 2.0f);
              f("circle tendency", &s.aiCircleTendency, 0.0f, 1.0f);
              i32("circle hold (ticks)", &s.aiCircleHoldTicks, 4, 180);
              i32("repath (ticks)", &s.aiRepathTicks, 2, 120);
              f("nav radius (vox)", &s.aiNavRadius, 4.0f, 40.0f);
            }
            if (ImGui::CollapsingHeader("Attack")) {
              f("reach (vox)", &s.aiAttackReach, 0.0f, 40.0f);
              f("aim tolerance (rad)", &s.aiAimTolerance, 0.05f, 1.6f);
              i32("cadence (ticks)", &s.aiCadenceTicks, 1, 240);
              i32("jitter (ticks)", &s.aiJitterTicks, 0, 120);
              i32("commit (ticks)", &s.aiCommitTicks, 0, 90);
              i32("disengage (ticks)", &s.aiDisengageTicks, 0, 180);
            }
            if (ImGui::CollapsingHeader("Arbiter",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
              f("hysteresis (incumbent bonus)", &s.aiHysteresis, 0.0f, 1.5f);
              ImGui::TextDisabled("weight 0 = the intent is DISABLED");
              ImGui::TextDisabled("a holding rule (behaviors.json) overrides these");
              static const char* kIntent[] = {"idle",     "face",
                                              "approach", "holdRange",
                                              "circle",   "attack",
                                              "flee",     "guard",
                                              "dodge",    "sleep",
                                              "work",     "wander",
                                              "socialize", "eat",
                                              "goto",      "cast",
                                              "orbit",     "charge",
                                              "shoot",     "investigate",
                                              "search",    "stalk"};
              static_assert(sizeof(kIntent) / sizeof(kIntent[0]) ==
                            (size_t)UIState::kAiIntents);
              for (int k = 0; k < UIState::kAiIntents; k++) {
                ImGui::PushID(k);
                ImGui::TextUnformatted(kIntent[k]);
                if (EditableSliderFloat("weight", &s.aiIntentWeight[k], 0.0f,
                                        4.0f, "%.2f"))
                  s.aiTuningDirty = true;
                if (EditableSliderInt("cooldown", &s.aiIntentCooldown[k], 0, 180))
                  s.aiTuningDirty = true;
                if (EditableSliderInt("min dwell", &s.aiIntentDwell[k], 0, 180))
                  s.aiTuningDirty = true;
                ImGui::PopID();
                ImGui::Separator();
              }
            }
          }
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
    }
    ImGui::End();
  }

  // ---- Combat window (game/melee.h, sim/tuning.h melee/combatfx/gore) ------
  //
  // THE FEEL LOOP, in one place: how the stroke is steered, what the edge does
  // when it lands, and what the player is told about it. The three tabs are the
  // three tuning groups behind a fight, and they are together here because they
  // are only ever judged together — a swing that reads badly is as often the
  // wound as the arc, and as often the hit-stop as either.
  //
  // LIVE, WITH NO MIRRORS. Unlike every other panel in this file, the sliders
  // run on a copy of the tuning singleton and write it straight back, so there
  // is nothing to reseat after an F5 or a browser-tuner save and no shadow copy
  // of sixty knobs to drift. See the long note on UIState::combatWindowOpen for
  // why this one deviates. `combatTuningDirty` still crosses to main.cpp,
  // because MeleeState caches its MeleeTuning by value and has to be told.
  //
  // Scrolling is ImGui's own (child regions inside the tabs) — see overlay.h on
  // why installing a GLFW scroll callback here would freeze the wheel
  // everywhere.
  if (s.combatWindowOpen) {
    ImGui::SetNextWindowPos(ImVec2(sPanelW + 12, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 720), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Combat", &s.combatWindowOpen)) {
      ui::KeepWindowOnScreen(4.0f);  // a saved position may be off this monitor
      Tuning t = CurrentTuning();
      bool moved = false;
      // The AI panel's idiom exactly: the slider's own return value is the
      // latch. No Apply button, because nothing here recompiles a shader — the
      // melee and combatfx groups are CPU-only and gore is read per event, so
      // every one of these lands on the next tick.
      auto f = [&moved](const char* label, float* v, float lo, float hi,
                        const char* fmt = "%.3f") {
        if (EditableSliderFloat(label, v, lo, hi, fmt)) moved = true;
      };
      auto i32 = [&moved](const char* label, int* v, int lo, int hi) {
        if (EditableSliderInt(label, v, lo, hi)) moved = true;
      };
      auto b = [&moved](const char* label, bool* v) {
        if (ImGui::Checkbox(label, v)) moved = true;
      };

      if (ImGui::Button("Save")) s.combatSave = true;
      ImGui::SameLine();
      ImGui::TextDisabled("patches melee/combatfx/gore/gear in tuning.json");
      if (!s.combatSaveStatus.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", s.combatSaveStatus.c_str());
      }
      ImGui::TextDisabled("sliders are LIVE. F5 reloads the file over them.");
      ImGui::Separator();

      if (ImGui::BeginTabBar("##combattabs")) {
        // ---- Stroke: how the blade is steered ---------------------------
        if (ImGui::BeginTabItem("Stroke")) {
          ImGui::BeginChild("##strokescroll", ImVec2(0, 0), false);
          Tuning::Melee& m = t.melee;
          if (ImGui::CollapsingHeader("Discrete strikes",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            f("flick threshold (px/s)", &m.pickMinSpeed, 1.0f, 1500.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Below this, a click has no direction and the strike\n"
                  "alternates horizontal left/right instead.");
            f("torso twist share", &m.torsoShare, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of the stroke's azimuth the torso carries,\n"
                  "like the head-look's spine share. 0 = arm only.");
            f("torso pitch share", &m.torsoPitch, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of the stroke's elevation the torso carries.\n"
                  "0 = arm only, 1 = whole body pitches into the blow.");
            f("head keep-out (m)", &m.headClearM, 0.0f, 0.5f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Clearance beyond the head's own radius the hand-to-tip\n"
                  "segment is held out to, so no stroke sweeps the blade\n"
                  "through the wielder's own face. 0 = clamp off.");
          }
          if (ImGui::CollapsingHeader("Aim", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("radians of tip travel per mouse pixel");
            f("aim gain x", &m.aimGainX, 0.0005f, 0.02f, "%.4f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Radians of tip azimuth per mouse pixel.");
            f("aim gain y", &m.aimGainY, 0.0005f, 0.02f, "%.4f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Radians of tip elevation per mouse pixel.");
            f("reference speed (px/s)", &m.commitSpeed, 100.0f, 3000.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Reference drive speed: 35%% of it reads as Wind (what a\n"
                  "parry can arrest); the whoosh volume scales against it.\n"
                  "Nothing commits on it any more.");
            f("speed smoothing (s)", &m.dirSmoothing, 0.005f, 0.4f);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Seconds of drive history the speed is averaged over.");
            f("reach gain (m/unit)", &m.reachGainM, 0.0f, 0.02f, "%.4f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Metres of tip reach per dReach unit.");
          }
          if (ImGui::CollapsingHeader("Recover", ImGuiTreeNodeFlags_DefaultOpen)) {
            f("recover time (s)", &m.recoverTime, 0.03f, 0.8f);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Seconds over which the arm is handed back.");
          }
          if (ImGui::CollapsingHeader("Where the point may go",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            f("azimuth out (rad)", &m.azOut, 0.2f, 3.1f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How far round to the weapon side the point may go,\n"
                  "measured from straight ahead.\n\n"
                  "2.36 (135 deg) is BEHIND the character: it drove the\n"
                  "commanded point past the frontal plane and parked the\n"
                  "shoulder on its authored 50-deg-past-the-back stop, so\n"
                  "the arm ended up behind the body and stuck there.\n"
                  "1.83 is 105 deg: the whole front plus a little past\n"
                  "side-on, which is still a real wind-up.");
            f("azimuth across (rad)", &m.azAcross, 0.2f, 3.1f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How far across the body the point may go, measured\n"
                  "from straight ahead toward the off-hand side.");
            f("elevation min (rad)", &m.elMin, -1.55f, -0.1f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Lowest the point may go, in radians. -1.55 is the arm\n"
                  "hanging at the side.");
            f("elevation max (rad)", &m.elMax, 0.1f, 1.55f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Highest the point may go, in radians. 1.48 is\n"
                  "directly overhead.");
          }
          if (ImGui::CollapsingHeader("Arm and wrist",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            // The one number with a story. See MeleeTuning::wristMaxAngle.
            f("wrist limit (rad)", &m.wristMaxAngle, 0.0f, 3.14f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "CEILING on how far the wrist may take the blade from the\n"
                  "orientation the solved forearm gives it for free.\n\n"
                  "A GROSS budget, not an anatomical angle: the blade asks\n"
                  "for direction AND roll, and the neutral grip stands about\n"
                  "pi of roll away from a committed cut, so the STEERING\n"
                  "gets this minus that tax. 3.10 leaves ~1.1 rad free.\n\n"
                  "Measured, the ask is 2.6..3.1 rad whichever grip is\n"
                  "authored — which is why the along-the-arm grip the\n"
                  "overhaul tried bought nothing and cost the idle pose.\n"
                  "The grip is back to [0,-90,0]: blade out of the fist.");
            // ---- THE THROTTLE, and why it is next to the ceiling ---------
            // Same joint, two different questions, and separating them is
            // the fix: "how far CAN a wrist go" and "how much of that does
            // this stroke want". See MeleeTuning::steerSpeedLo.
            f("wrist steer at rest", &m.steerFloor, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How much of that alignment is applied when the blade is\n"
                  "NOT moving.\n\n"
                  "The stroke always commands the blade along the shoulder-\n"
                  "to-point radius. Right for a cut, wrong for a hold: at\n"
                  "1.0 a motionless guard is still wrenched round to lay\n"
                  "the sword along the radius, which is what \"it points\n"
                  "straight down when I hold it out in front\" was.\n\n"
                  "At 0.15 a still blade keeps its own grip pose (out of\n"
                  "the fist, UP with the arm forward) and slides into full\n"
                  "alignment as it moves. 1.0 is the old behaviour.");
            f("steer ramp: still below (m/s)", &m.steerSpeedLoMps, 0.0f, 6.0f,
              "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Blade tip speed in m/s below which the wrist stays\n"
                  "at the steer floor. Below this the blade keeps its\n"
                  "grip pose rather than aligning to the stroke.");
            f("steer ramp: committed above (m/s)", &m.steerSpeedHiMps, 0.05f,
              12.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Blade TIP speed at the two ends of the ramp. Between\n"
                  "them the applied alignment interpolates, smoothed on the\n"
                  "wrist halflife so a commit never snaps the fist.\n\n"
                  "ONLY a committed slash bypasses the ramp: a cut is a cut\n"
                  "even at the instant it reverses through zero. Wind and\n"
                  "recover used to bypass it too, which is why raising the\n"
                  "sword for an overhead wrenched the wrist at the cursor.");
            f("arm smoothing (s)", &m.armSmoothing, 0.0f, 0.4f);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Halflife easing the WHOLE stroke (azimuth, elevation,\n"
                  "reach, follow-through included) before the arm is built\n"
                  "from it — hand, elbow plane and blade lag together as\n"
                  "one rigid assembly. 0 is off (tick-exact tracking).");
            f("wrist smoothing (s)", &m.wristSmoothing, 0.0f, 0.5f);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Halflife on the WRIST alone: the commitment envelope\n"
                  "(attack at half this, release at 4x it) and the chase of\n"
                  "the commanded blade orientation, which runs 3x faster\n"
                  "through a slash so the edge still lays in crisply.\n\n"
                  "Separate knobs because the joints tolerate lag\n"
                  "differently: a lagging arm reads as weight, a lagging\n"
                  "wrist mid-cut costs edge alignment and damage.");
            f("hand behind limit", &m.handBackFrac, 0.0f, 0.5f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How far behind the shoulder's frontal plane the HAND may\n"
                  "sit, as a fraction of arm reach. The azimuth window\n"
                  "bounds the commanded POINT, but the hand is that point\n"
                  "minus a whole blade — unbounded it sat voxels behind the\n"
                  "plane at the stops, which is \"the arm goes behind him\".\n"
                  "0 pins the hand to the plane exactly.");
            f("elbow bend plane (rad)", &m.elbowPoleCone, 0.05f, 3.14f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How far the elbow may be steered off straight-back.\n\n"
                  "The stroke picks the plane the arm bends in — that is\n"
                  "what makes a horizontal cut read as shoulder rotation\n"
                  "plus elbow extension — but the plane is built from the\n"
                  "hand's travel and was free to point ANYWHERE, forward\n"
                  "past the fist included.\n\n"
                  "1.75 rad is 100 deg: down, up or out to either side,\n"
                  "never forward. Pi is unbounded (the old behaviour).");
            f("elbow hinge cone (rad)", &m.elbowAxisCone, 0.0f, 3.14f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "CAP on how far the steered elbow hinge axis may sit from\n"
                  "the forearm's AUTHORED one. DEFAULTS TO pi = OFF.\n\n"
                  "That default is measured, not an oversight: the pose\n"
                  "clamp keeps only the component about the axis it is\n"
                  "given and discards the rest, so penning the axis makes\n"
                  "it LOSSY — and a horizontal cut legitimately wants a\n"
                  "bend plane 90 deg off the resting axis. At 75 deg it\n"
                  "cost the swing-plane gate 1.76 rad of elbow clamp and\n"
                  "4.42 voxels of hand.\n\n"
                  "The anatomy is enforced on the BEND PLANE above, where\n"
                  "it is free. This is left as the A/B.");
            f("hand extension", &m.handExtend, 0.15f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How far the hand reaches out, as a fraction of arm\n"
                  "reach. Lower = the arm bends more; higher = straighter.");
            f("extension smoothing (s)", &m.extendSmoothing, 0.005f, 1.0f);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Halflife on extension changes, in seconds.");
            f("reach fraction", &m.reachFraction, 0.2f, 0.99f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of arm reach the hand may use. Caps how far\n"
                  "the tip can extend from the shoulder.");
            f("lean turn rate (rad/s)", &m.leanTurnRate, 0.5f, 60.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Radians/sec the lean plane may turn. Limits how fast\n"
                  "the body tilts to serve the stroke.");
            f("blade smoothing (s)", &m.bladeSmoothing, 0.005f, 0.4f);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Halflife on the blade frame, in seconds. The blade's\n"
                  "own orientation lags the wrist by this.");
            {
              bool handLeads = m.handLead >= 0.0f;
              if (ImGui::Checkbox("hand leads the point (sabre cut)",
                                  &handLeads)) {
                m.handLead = handLeads ? 1.0f : -1.0f;
                moved = true;
              }
              if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "ON = the hand leads the blade tip (sabre draw-cut).\n"
                    "OFF = the point leads (a thrust or a chop where the\n"
                    "tip arrives first).");
            }
          }
          if (ImGui::CollapsingHeader("Guard position")) {
            f("fallback reach (m)", &m.fallbackReachM, 0.1f, 1.5f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Reach in metres used when the rig has no arm data.\n"
                  "The seed of last resort for unarmed/fallback poses.");
            f("guard forward (m)", &m.guardForwardM, 0.0f, 0.6f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Metres forward of the shoulder the guard rests at\n"
                  "when nothing else drives the hand.");
            f("guard up (m)", &m.guardUpM, 0.0f, 0.6f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Metres above the shoulder the guard rests at.");
            f("guard side (m)", &m.guardSideM, 0.0f, 0.6f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Metres to the weapon side the guard rests at.");
          }
          if (ImGui::CollapsingHeader("Aim body binding")) {
            f("aim yaw (deg)", &m.aimYaw, 0.0f, 90.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Degrees the camera may lead the body's facing before\n"
                  "the swing pins at the cone edge. A look behind the\n"
                  "body is reflected to the front first.");
          }
          ImGui::EndChild();
          ImGui::EndTabItem();
        }

        // ---- Damage: what the edge does when it lands -------------------
        if (ImGui::BeginTabItem("Damage")) {
          ImGui::BeginChild("##dmgscroll", ImVec2(0, 0), false);
          Tuning::Gore& g = t.gore;
          if (ImGui::CollapsingHeader("Speed is the damage",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("tip speed, in m/s, at the two ends of the ramp");
            f("full damage at (m/s)", &t.melee.fullSpeedMps, 0.1f, 20.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Blade tip speed in m/s for full damage. A swing\n"
                  "faster than this does no extra — it is the ceiling.");
            f("nothing below (m/s)", &t.melee.minSpeedMps, 0.0f, 20.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Tip speed below which a hit does nothing at all.\n"
                  "Resting the blade on someone is not a cut.");
            f("flat-on floor", &t.melee.edgeFloor, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Damage multiplier for a cut travelling in the blade's own\n"
                  "FLAT — a slap with the side. 1.0 disables edge alignment\n"
                  "entirely; 0 makes a flat hit free. A real flat still\n"
                  "bruises and still breaks bone, so this is a floor rather\n"
                  "than a gate, and it is what makes rolling the blade into\n"
                  "the cut worth doing.");
          }
          // ---- BLADE ON BLADE ------------------------------------------
          // On the DAMAGE tab and not on Combat feel, because a parry is
          // mechanics: it ends the stroke, it costs the blocking blade hp,
          // and it shoves the defender's guard. Turning combat feel off
          // must not change who wins a fight; turning these off does.
          if (ImGui::CollapsingHeader("Blade on blade",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("a cut stopped by another creature's WEAPON");
            ImGui::TextDisabled("(worn armour is not a parry — it takes the cut)");
            f("parry reach (m)", &t.melee.blockGapM, 0.0f, 0.5f, "%.3f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How close the blades must come for the block to fire, on\n"
                  "top of the attacking blade's own half-width.\n\n"
                  "Not zero: a sword is about a quarter of a voxel thick and\n"
                  "two swept segments never intersect exactly. Past ~0.3 m\n"
                  "the defender parries blows that passed a foot away, which\n"
                  "reads as an invincible AI rather than as a bad number.");
            f("wear on the blade", &t.melee.blockItemDamage, 0.0f, 1.0f,
              "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of the blow the blocking blade takes as hp\n"
                  "damage. Parrying is not free — it wears your weapon.");
            f("guard beaten · az (rad)", &t.melee.blockNudgeAz, 0.0f, 1.0f,
              "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Radians the defender's guard is shoved in azimuth\n"
                  "at full power. Opens the guard for the next blow.");
            f("guard beaten · el (rad)", &t.melee.blockNudgeEl, 0.0f, 1.0f,
              "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How far a blocked blow shoves the DEFENDER'S stroke.\n"
                  "Blocking must not be free — a heavy blow caught on the\n"
                  "blade opens the guard and the next one has somewhere to\n"
                  "go. Only the SIGN is drawn, counter-based on the two ids,\n"
                  "so both sides of an exchange shove the same way in every\n"
                  "replay of the same fight.");
          }
          if (ImGui::CollapsingHeader("The kerf",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("a cut is a slot; dismemberment is what is");
            ImGui::TextDisabled("left of the lattice afterwards");
            f("bite, standing still (vox)", &g.cutDepth, 0.0f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How deep the kerf goes at zero swing speed, in world\n"
                  "voxels. The base depth before speed adds more.");
            f("bite from speed (vox)", &g.cutDepthPower, 0.0f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Extra depth added at full swing speed. Total depth is\n"
                  "(base + this * power) * heft. A greatsword at full\n"
                  "swing opens a gash three times as deep as a lazy wave.");
            f("cut length (vox)", &g.cutLength, 0.1f, 8.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Half-length of the slot along the edge at full power.\n"
                  "Makes a cut read as a slice, not a puncture. Scaled\n"
                  "by 0.4 + 0.6 * power so a graze is short.");
            f("cut width x blade", &g.cutWidth, 0.05f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Kerf half-thickness as a multiple of the blade's own\n"
                  "authored edgeHalfWidth. Below 1 because the edge is\n"
                  "thinner than the widest part of the blade.");
            i32("spall rounds", &g.cutSpallRounds, 0, 4);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Rounds of spall after the cut. Each round widens the\n"
                  "gash into its own rim, so sustained hits dismember.\n"
                  "0 = every cut is a clean bore.");
            f("spall strength", &g.cutSpallStrength, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How aggressively each spall round removes voxels at\n"
                  "the rim of the cut. Higher = wider gashes.");
          }
          if (ImGui::CollapsingHeader("When a limb comes off")) {
            f("sever fraction", &g.woundSeverFraction, 0.05f, 0.95f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "A cut that disconnects at least this fraction of the\n"
                  "limb's remaining voxels fires a sever. The edge came\n"
                  "out the other side.");
            f("neck radius (vox)", &g.woundNeckRadius, 0.0f, 8.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Radius in world voxels of the sphere checked around\n"
                  "the joint anchor for the hanging-by-a-thread test.");
            f("neck fraction", &g.woundNeckFraction, 0.0f, 0.95f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Below this fraction of the joint's original voxels,\n"
                  "the limb is not attached to anything worth the name\n"
                  "and comes off.");
            f("impact-sever scale", &g.woundImpactSeverScale, 0.0f, 16.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Multiplier on each mob's authored severImpactSpeed.\n"
                  "Those numbers were written when any fast hit severed\n"
                  "outright, so this scales them up to keep the old\n"
                  "thresholds as an extreme-speed exception only.");
          }
          if (ImGui::CollapsingHeader("Heft (how much weapon)")) {
            f("reference volume", &g.woundHeftRef, 0.01f, 40.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "World-voxel volume of the weapon that reads as heft\n"
                  "1.0. Derived from art, not authored: the stock sword\n"
                  "is 5.3 world voxels. Retune this and every weapon\n"
                  "rescales together.");
            f("heft ceiling", &g.woundHeftMax, 1.0f, 32.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Max heft multiplier. Caps how much a comically large\n"
                  "weapon can amplify a single swing's kerf depth.");
          }
          if (ImGui::CollapsingHeader("Blood")) {
            f("stain radius (vox)", &g.woundStainRadius, 0.0f, 8.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Radius in world voxels around a cut that gets soaked\n"
                  "in blood. The wound's visual footprint on the flesh.");
            f("stain density", &g.woundStainDensity, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of buried voxels in the stain radius that\n"
                  "take the blood material. Below 1 so the soak is\n"
                  "mottled, not a uniform red repaint.");
            f("bleed gain (per mob)", &g.bleedGain, 0.0f, 8.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Per-mob multiplier on all blood quantities (spray,\n"
                  "sever, voxels). The global 'how wet is this game'\n"
                  "dial. Centre is 1.0.");
            ImGui::TextDisabled("every drop is hp (Mob::DrainBlood)");
            f("hp per blood voxel", &g.bleedHpPerVoxel, 0.0f, 5.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "HP cost per whole blood voxel that drips into the\n"
                  "world. The drip IS the damage — a creature that\n"
                  "stands in its own blood is dying.");
            ImGui::Checkbox("stumps never close", &g.stumpBleedsOpen);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "ON = severed stumps bleed forever. OFF = they\n"
                  "eventually clot and stop dripping.");
          }
          if (ImGui::CollapsingHeader("Wound stain shape")) {
            f("surface spread", &g.woundStainSurface, 0.0f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Chance an EXPOSED voxel (hole walls, skin at the\n"
                  "mouth) takes the blood material. The wound's visible\n"
                  "face — what you see when you look at the cut.");
            f("blob size", &g.woundStainBlob, 0.0f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Feature size of the correlated noise in world voxels.\n"
                  "Controls how large the blotches of blood are. Without\n"
                  "correlation the soak is a fine speckle, not a smear.");
            f("coherence", &g.woundStainCoherence, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "0 = old independent per-voxel draws (fine speckle).\n"
                  "1 = fully correlated (blotchy smear). Blends between\n"
                  "the two noise fields.");
            f("crater rim stain", &g.craterStainRim, 0.0f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How far past removed cells the blood reaches on a\n"
                  "blast crater, in skin-lattice cells. Larger than cut\n"
                  "stain because a blast is messier than a clean slot.");
          }
          if (ImGui::CollapsingHeader("Wound healing")) {
            b("wounds heal", &g.woundHeals);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "ON = wound-material voxels revert to flesh instead of\n"
                  "decaying to air. The red fades off over seconds.\n"
                  "OFF = the undead setting: cuts rot outward and shed\n"
                  "parts, exactly the old behaviour.");
            f("heal slow factor", &g.woundHealSlow, 0.5f, 20.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Blood's own decay rate is divided by this inside a\n"
                  "limb, so wounds settle slower than a puddle on the\n"
                  "ground. 2 = a cut fades over ~6 s instead of ~3 s.");
            f("corpse bleed/voxel", &g.corpseBleedPerVoxel, 0.0f, 8.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Blood voxels a corpse owes per world voxel carved off\n"
                  "it. A dismembered corpse bleeds from where it is cut,\n"
                  "through the same cap as a live wound.");
          }
          if (ImGui::CollapsingHeader("Blunt and bruise",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("a punch, a mace, a pommel — the non-edge half");
            f("bruise radius (vox)", &g.bruiseRadius, 0.0f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How far a punch discolours the skin, in world voxels\n"
                  "at full power. Scaled by (0.5 + 0.5 * power), so\n"
                  "even a glancing hit marks.");
            f("bruise step (per hit)", &g.bruiseStep, 0.5f, 15.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Amount (0..15 scale) added per hit. At 6 (40%%) a\n"
                  "contact bruises on the first blow, saturates on the\n"
                  "second, and the third is blood. Below ~4 the mottle\n"
                  "noise makes the bruise nearly invisible.");
            f("bruise ceiling", &g.bruiseMax, 1.0f, 15.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Max bruise amount. 12 of 15 is 80%%: deep purple but\n"
                  "short of opaque, so the anatomy underneath still\n"
                  "shows. Past here further hits lay blood instead.");
            f("bruise->bleed chance", &g.bruiseBleedChance, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Per-voxel chance a blow lays BLOOD over a voxel already\n"
                  "at the bruise ceiling. 0 = bruises stay dry forever.");
            f("...from what depth", &g.bruiseBleedFrom, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of the ceiling a voxel must reach before the\n"
                  "roll above is made. 1.0 is EXACTLY saturated, which the\n"
                  "0.85..1.0 step jitter makes a blow harder to reach than\n"
                  "it looks; 0.85 is 'hit a bruise again and it bleeds'.");
            f("full-step hp", &g.bruiseHpRef, 0.0f, 64.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "THE FIST/MACE DIFFERENCE. Blunt hp that earns the whole\n"
                  "bruise step; a weaker blow gets a SQUARE-ROOTED share.\n"
                  "Root not linear: a linear quarter-step would land under\n"
                  "the mottle threshold and a punch would mark nothing.\n"
                  "0 = every blow marks like a full one.");
            f("...floor under it", &g.bruiseHpFloor, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Smallest share of the step a weak blow can be cut to,\n"
                  "so an incidental tap still leaves a visible mark.");
            f("blunt bleed scale", &g.bluntBleedScale, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of a cut's drip budget that blunt trauma uses.\n"
                  "0 = maces are dry; 1 = bleeds like a sword.");
            f("blunt carve radius (vox)", &g.bluntCarveRadius, 0.0f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How deep a blunt hit dents flesh THAT IS ALREADY PULPED,\n"
                  "in world voxels. Scaled by the weapon's bluntCarve\n"
                  "fraction (fist 0.3, gauntlet ~0.35, mace ~0.6) AND by\n"
                  "the pulp share below -- nothing comes off clean skin.\n"
                  "The crater is soaked in the victim's woundMat.");
            ImGui::TextDisabled("pulping: bruise -> blood -> voxels come away");
            f("pulped at (coat depth)", &g.pulpAmt, 1.0f, 15.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How deep a coat of the victim's own BLOOD a voxel must\n"
                  "wear to count as pulp. A depth, not merely 'wet': one\n"
                  "spray from a cut elsewhere must not make a limb crumble\n"
                  "under a punch.");
            f("dents past pulp share", &g.pulpCarveFrom, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Share of the contact CORE (inner half of the bruise\n"
                  "radius) that must already be pulp before ANY voxel is\n"
                  "removed; past it the dent ramps to full. 0 restores the\n"
                  "old 'first blow dents' behaviour. This is what makes\n"
                  "sustained hits on one spot cave a skull in while a mace\n"
                  "on an intact arm only bruises.");
            ImGui::TextDisabled("dissolution: pulped voxels dissolve over time");
            f("pulp rot (vox/min)", &g.pulpRotRate, 0.0f, 20.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How fast PULPED tissue dissolves, in world voxels a\n"
                  "minute, per flagged limb. When a blow earns a dent the\n"
                  "limb is flagged, and this rate eats blood-coated voxels\n"
                  "one at a time in a noisy pattern — the same Bernoulli\n"
                  "draw the infection uses. Replaces the old instant carve.\n"
                  "0 = pulped tissue stays put (bruise only, no dissolution).");
          }
          if (ImGui::CollapsingHeader("Unarmed overrides")) {
            ImGui::TextDisabled("negative = use the base value above");
            f("bruise radius (vox)##unarmed", &g.unarmedBruiseRadius,
              -1.0f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Bruise radius for natural weapons (fists, jaws).\n"
                  "Negative = same as the base row above.");
            f("bruise step##unarmed", &g.unarmedBruiseStep,
              -1.0f, 15.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Bruise step per hit for natural weapons.\n"
                  "Negative = same as the base row.");
            f("bleed chance##unarmed", &g.unarmedBleedChance,
              -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Bruise-to-blood chance for natural weapons.\n"
                  "Negative = same as the base row.");
            f("bleed scale##unarmed", &g.unarmedBleedScale,
              -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of a cut's drip budget for unarmed blunt.\n"
                  "Negative = same as the base row.");
            f("dent radius##unarmed", &g.unarmedCarveRadius,
              -1.0f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Blunt carve radius for natural weapons.\n"
                  "Negative = same as the base row.");
            f("dents past pulp share##unarmed", &g.unarmedPulpCarveFrom,
              -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Pulp share threshold for natural weapons.\n"
                  "Negative = same as the base row.");
          }
          if (ImGui::CollapsingHeader("Bite")) {
            f("bite radius (vox)", &g.biteRadius, 0.0f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Radius of the tear in world voxels at full power.\n"
                  "0.45 is a hole about 9 cm across — a bite. Enough\n"
                  "of them still take a hand off via collapse sever.");
            f("bite blob size", &g.biteBlob, 0.5f, 8.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Feature size of the correlated noise in skin voxels.\n"
                  "The size of one piece that comes away — same shape\n"
                  "as a zombie rot hole.");
            f("bite stain scale", &g.biteStainScale, 0.0f, 4.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Multiple of craterStainRim for the bite's blood soak.\n"
                  "Above 1 because a tear is a ragged hole: the mess\n"
                  "goes further than the damage.");
            f("infection heal slow", &g.infectHealSlow, 0.5f, 20.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Divisor on the decay of the material a bite rewrites\n"
                  "flesh to (the biter's infection). Larger = rot stays\n"
                  "longer. On undead whose wounds don't heal, rot never\n"
                  "goes away regardless.");
            ImGui::TextDisabled("the infection is alive");
            f("infection spread (/voxel/s)", &g.infectSpreadRate, 0.0f, 0.05f,
              "%.5f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How fast the rot GROWS: per rotflesh voxel, the chance\n"
                  "a second it converts a neighbour. It converts tissue next to\n"
                  "itself — soft tissue only, bone stays bone — so it\n"
                  "creeps out from the wound as a front. A limb with no\n"
                  "tissue left crosses a JOINT into the next limb, which\n"
                  "is how a bitten hand eventually reaches the torso.\n"
                  "0 = a bite is a static mark again.");
            f("rot / disintegration (/voxel/s)", &g.infectRotRate, 0.0f, 0.05f,
              "%.5f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How fast the rot EATS you: per rotflesh voxel, the\n"
                  "chance a second it is eaten. Eaten voxels are gone.\n"
                  "Charged through the ordinary burn flush: hp falls with\n"
                  "the fraction of the limb that is gone, a limb eaten\n"
                  "past collapse comes off, a vital limb eaten through\n"
                  "kills. Below the spread rate the infection grows while\n"
                  "you shrink — an untreated bite is fatal. 0 = spreads\n"
                  "but never consumes.");
            if (EditableSliderFloat("mob multiplier", &g.infectMobMult,
                                    0.0f, 50.0f, "%.1f"))
              SetCurrentTuning(t);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Scales both spread and rot rates on mob limbs only.\n"
                  "The grid-side reactions are unaffected. Crank this up\n"
                  "to watch an infection advance in real time without\n"
                  "touching the base rates or reloading materials.");
          }
          if (ImGui::CollapsingHeader("Armour vs blunt/bite")) {
            Tuning::Gear& gr = t.gear;
            ImGui::TextDisabled("shell hardness interaction");
            f("ruined condition", &gr.ruinedCondition, 0.05f, 0.95f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Below this fraction of its authored voxels, a worn\n"
                  "piece is RUINED. Condition is measured in voxels still\n"
                  "there, because a shell protects by being in the way.");
            f("cut hardness ref", &gr.cutHardnessRef, 1.0f, 255.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Material hardness that takes a blade's kerf unscaled.\n"
                  "Skin is 8 (cut like flesh), iron is 160 (chipped).\n"
                  "The kerf is scaled by this / shell hardness.");
            f("cut hardness floor", &gr.cutHardnessMin, 0.0f, 1.0f, "%.3f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Floor on the hardness scaling. Even the hardest shell\n"
                  "takes at least this fraction of the kerf.");
            ImGui::TextDisabled("blunt: mace vs plate");
            f("dent radius (vox)", &gr.bluntDentRadius, 0.0f, 8.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How far a full-power hit at armorBreak 1 breaks into\n"
                  "a worn shell, in world voxels. The plate is genuinely\n"
                  "gone, exposing the flesh to the next blow, fire, acid.");
            f("blunt hardness ref", &gr.bluntHardnessRef, 1.0f, 255.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Material hardness reference for the dent, like the\n"
                  "kerf's. 120 rather than 8 because plate is much LESS\n"
                  "proof against trauma than against an edge: iron keeps\n"
                  "~38%% of the dent vs 5%% of a kerf.");
            f("blunt hardness floor", &gr.bluntHardnessMin, 0.0f, 1.0f, "%.3f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Floor on the blunt hardness scaling. Even the hardest\n"
                  "shell takes at least this fraction of the dent.");
            f("blunt pass-through", &gr.bluntThrough, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of a blunt blow's hp that reaches the limb\n"
                  "under the shell, as trauma. This is the number that\n"
                  "says plate stops swords but maces go through.");
            f("blunt shell hp cost", &gr.bluntShellHp, 0.0f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of the blow the shell itself takes as hp.\n"
                  "Under 1 so plate that absorbed the whole blow is not\n"
                  "destroyed in the same number of hits as the wearer.");
            ImGui::TextDisabled("bite vs armour");
            f("bite on shell (frac)", &gr.biteOnShell, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of a bite's damage that lands as blunt\n"
                  "trauma when teeth meet a hard shell. No shell-\n"
                  "breaking: teeth do not dent plate.");
            f("bite through: soft <=", &gr.biteThroughSoft, 0.0f, 60.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Material hardness at or below which teeth pass\n"
                  "entirely through a garment. Linen is 4, cloth 5,\n"
                  "leather 14 — so every woven garment lets a bite in.");
            f("bite through: hard >=", &gr.biteThroughHard, 1.0f, 255.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Material hardness ramp: teeth pass entirely at or below\n"
                  "'soft' (linen=4, cloth=5, leather=14), nothing passes\n"
                  "at or above 'hard' (iron=160, steel=200).");
          }
          if (ImGui::CollapsingHeader("Burns cap health")) {
            ImGui::TextDisabled("burnt fraction -> max hp, three knots");
            f("burnt at mid knot", &g.burnCapMidFraction, 0.05f, 0.95f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Burn fraction at the middle knot of the health curve.\n"
                  "Three knots: 0%% burn = full hp, this = mid health,\n"
                  "death fraction = dead.");
            f("health at mid knot", &g.burnCapMidHealth, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Max hp fraction at the middle knot. Shapes how fast\n"
                  "health drops as burns accumulate.");
            f("burnt = death", &g.burnDeathFraction, 0.1f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Burnt fraction at which the creature dies outright.\n"
                  "1.0 = must be fully charred; lower = dies sooner.");
          }
          ImGui::EndChild();
          ImGui::EndTabItem();
        }

        // ---- Feel: what the player is TOLD a hit was --------------------
        if (ImGui::BeginTabItem("Feel")) {
          ImGui::BeginChild("##feelscroll", ImVec2(0, 0), false);
          Tuning::CombatFx& fx = t.combatfx;
          ImGui::Text("hit-stop now: %.2fx", s.hitStopScale);
          ImGui::TextDisabled("1.00x = running normally");
          ImGui::Separator();
          if (ImGui::CollapsingHeader("Hit-stop",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            b("hit-stop on", &fx.hitStop);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Dips the rate the fixed-tick accumulator fills at for a\n"
                  "few tens of milliseconds after a hit. It changes how many\n"
                  "ticks run per frame — which already varies 0..4 — and\n"
                  "never what a tick computes, so the sim is untouched.");
            ImGui::TextDisabled("chip: debris, a dropped item, a held weapon");
            f("chip speed", &fx.hitStopChipScale, 0.02f, 1.0f, "%.2fx");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Tick-rate multiplier during a chip hit-stop.\n"
                  "0.15 = the world runs at 15%% speed.");
            f("chip length (ms)", &fx.hitStopChipMs, 0.0f, 400.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Duration of the chip dip in real milliseconds.");
            ImGui::TextDisabled("flesh: a live creature was hurt");
            f("flesh speed", &fx.hitStopFleshScale, 0.02f, 1.0f, "%.2fx");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Tick-rate multiplier during a flesh hit-stop.");
            f("flesh length (ms)", &fx.hitStopFleshMs, 0.0f, 400.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Duration of the flesh dip in real milliseconds.");
            ImGui::TextDisabled("sever: a limb came off");
            f("sever speed", &fx.hitStopSeverScale, 0.02f, 1.0f, "%.2fx");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Tick-rate multiplier during a sever hit-stop.");
            f("sever length (ms)", &fx.hitStopSeverMs, 0.0f, 400.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Duration of the sever dip in real milliseconds.\n"
                  "Tiers are strictly ordered: a sever in the same\n"
                  "frame as a chip must not be shortened by it.");
          }
          if (ImGui::CollapsingHeader("Hit flash",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("additive, linear HDR, before the tonemap");
            f("chip flash", &fx.flashChip, 0.0f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Peak additive flash intensity for a chip hit.");
            f("flesh flash", &fx.flashFlesh, 0.0f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Peak additive flash intensity for a flesh hit.");
            f("sever flash", &fx.flashSever, 0.0f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Peak additive flash intensity for a sever.");
            f("halflife (s)", &fx.flashHalflife, 0.01f, 0.6f);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Seconds to halve the flash. Aged on the tick so it\n"
                  "slows under hit-stop. Keep short — a flash still\n"
                  "visible at the next blow reads as a shader bug.");
          }
          if (ImGui::CollapsingHeader("Hit reaction",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            b("hit-react on", &fx.hitReact);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "A struck creature rocks AWAY from the blade's travel.\n"
                  "Pose-space lean only — nothing here moves the origin.");
            f("reference damage (hp)", &fx.hitReactRefDamage, 1.0f, 60.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "HP at full swing speed that reads as the baseline\n"
                  "blow. A strike's total profile over this is the\n"
                  "multiplier on every peak below — a mace shoves\n"
                  "harder than a fist because it IS harder. 14 is a\n"
                  "sword's cut.");
            f("max scale", &fx.hitReactMaxScale, 0.5f, 8.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Ceiling on the damage multiplier, so a freak number\n"
                  "in items.json cannot fold somebody in half.");
            f("lean (deg)", &fx.hitReactLeanDeg, 0.0f, 45.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Peak lean away from the blow at the reference damage,\n"
                  "in degrees. The spring is clamped, so a multi-tick\n"
                  "cut leans exactly this far and holds there while the\n"
                  "blade is in the wound.");
            f("spine share", &fx.hitReactSpineShare, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of the lean the spine carries vs the root\n"
                  "limb. All on root = tips like a signpost. All on\n"
                  "spine = hips unnaturally still.");
            f("push (frac of height)", &fx.hitReactPushFrac, 0.0f, 0.3f, "%.3f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Peak root shove as a fraction of the creature's own\n"
                  "height. Same lurch on a rat and a troll. Horizontal\n"
                  "from the blade's travel; vertical is what makes an\n"
                  "overhead blow drive a body into its knees.");
            f("struck limb flick (deg)", &fx.hitReactLimbDeg, 0.0f, 90.0f, "%.1f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Peak flick of the struck limb about its own joint.\n"
                  "This is the part that says WHICH arm was hit.\n"
                  "Clamped to the joint's authored range.");
            f("halflife (s)", &fx.hitReactHalflife, 0.01f, 0.5f);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Seconds to halve. The whole reaction is over in ~4x\n"
                  "this. Past ~0.2 s it stops reading as a flinch and\n"
                  "starts reading as a wobble.");
          }
          if (ImGui::CollapsingHeader("Sound",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            f("whoosh volume", &fx.whooshVolume, 0.0f, 3.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Volume trim for the blade whoosh cue.");
            f("whoosh silent below (px/s)", &fx.whooshMinSpeed, 0.0f, 2000.0f,
              "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Mouse px/s below which a committed stroke gets no\n"
                  "whoosh. A cut that barely moved should not sound\n"
                  "like one.");
            f("whoosh pitch, slow", &fx.whooshRateSlow, 0.4f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Pitch multiplier at the slow end of the swing.\n"
                  "Lower = deeper whoosh for a lazy stroke.");
            f("whoosh pitch, fast", &fx.whooshRateFast, 0.4f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Pitch multiplier at the fast end. A faster cut is a\n"
                  "higher, tighter whoosh.");
            f("flesh impact volume", &fx.fleshVolume, 0.0f, 3.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Volume trim for the flesh impact cue.");
            f("blade clang volume", &fx.clangVolume, 0.0f, 3.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Volume trim for the blade-on-blade clang cue.");
            f("audible radius (m)", &fx.cueRadius, 1.0f, 120.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Radius in metres within which combat sounds are\n"
                  "audible. Same unit as audio cue groups.");
            ImGui::TextDisabled("add takes through");
            ImGui::TextDisabled("scripts/import_sounds.py");
          }
          ImGui::EndChild();
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }

      if (moved) {
        SetCurrentTuning(t);
        s.combatTuningDirty = true;
      }
    }
    ImGui::End();
  }

  // Closes the FontSmall push at the top of BuildUI: every window above draws
  // in it, so the pop must stay LAST no matter how many panels get appended.
  ImGui::PopFont();
}

void Overlay::Render(const rhi::RenderPass& pass) {
  ImGui::Render();
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), rhi::vkr::NativeCmd(pass));
}

void Overlay::RenderRecorded(const rhi::RenderPass& pass) {
  // GetDrawData() stays valid until the next NewFrame, so replaying it costs
  // nothing but the draw calls.
  if (ImDrawData* d = ImGui::GetDrawData())
    ImGui_ImplVulkan_RenderDrawData(d, rhi::vkr::NativeCmd(pass));
}

void Overlay::Shutdown() {
  if (sampler_ && device_) {
    vk::Backend* be = (vk::Backend*)device_;
    auto destroySampler =
        (PFN_vkDestroySampler)be->InstanceProc("vkDestroySampler");
    // Every ImGui descriptor pointing at this sampler dies with the backend's
    // pool in the Shutdown below, and the device is idle by the time main.cpp
    // reaches here (ctx.WaitIdle precedes it), so no in-flight command buffer
    // can still reference it.
    if (destroySampler) destroySampler(be->Device(), (VkSampler)sampler_,
                                       nullptr);
    sampler_ = 0;
    device_ = nullptr;
  }
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
}

// ---- P5: the material picker, for the in-game editor (ui/editor_ui.cpp) ----------
bool OverlayMaterialPicker(const UIState& s, const char* id, const std::vector<int>& ids,
                           int& selected, char* search, size_t searchN, float maxH) {
  return MaterialPicker(s, id, ids, selected, search, searchN, maxH);
}
void OverlayCurrentMaterial(const UIState& s, int id, const char* what) {
  CurrentMaterial(s, id, what);
}
