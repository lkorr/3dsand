#include "ui/overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
#include "ui/inventory_ui.h"
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
  // Let the backend create its own descriptor pool (font atlas + a few).
  info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE;
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

void Overlay::BeginFrame() {
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
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
  const float yMana = disp.y - pad - h;
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

  // ---- what is ON you, one line, only when there is enough of it -----------
  //
  // The bars say how you ARE; the figure's tint says which parts are coated;
  // this NAMES the substance, which a colour cannot. Below s.stainHudMin
  // (tune.coat.hudMinFrac, mirrored into UIState so this file needs no sim
  // header) the line is not drawn AT ALL — no reserved gap, no faded caption
  // — so a clean player's HUD is exactly the HUD that was here before.
  float yStack = yHealth - gap;
  if (s.bodyValid && s.stainFrac >= s.stainHudMin && s.stainColor != 0) {
    char buf[80];
    snprintf(buf, sizeof buf, "stained %.0f%% \xc2\xb7 %s",
             s.stainFrac * 100.0f,
             s.stainLabel[0] ? s.stainLabel : "something");
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    const ImVec2 tp(x, std::floor(yStack - ts.y - 2.0f));
    // Lightened toward white: an authored stain colour is picked to read as
    // DRIED matter on a lit surface, and the same value set as 13 px of text
    // over the figure's dark scrim is barely a shape.
    ui::ShadowText(d, tp, ui::Mix(s.stainColor, IM_COL32_WHITE, 0.45f), buf);
    yStack = tp.y - 2.0f;
  }

  // ---- body condition, sitting directly above the hp bar -------------------
  const float figureH = DrawBodyFigure(s, x, yStack);
  const float yTop = yStack - figureH;

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
  if (!s.lookPrompt.empty())
    py += tab(s.lookPrompt.c_str(), py, ui::ColGoldDim(), ui::ColParch(), 0.95f);
  if (!s.kitMessage.empty() && s.kitMessageAge < 2.5f) {
    const float a = std::clamp(1.6f - s.kitMessageAge * 0.7f, 0.0f, 1.0f);
    tab(s.kitMessage.c_str(), py, ui::ColEmber(), ui::ColEmber(), a);
  }
  ImGui::PopFont();
}

// ---- the body-condition stick figure ----------------------------------------
//
// A limb is one thick line segment. Three states, in priority order, because
// they are not independent — a severed limb cannot bleed and its damage is no
// longer news:
//
//   severed   -> the segment is NOT drawn at all; a dim stump tick is left at
//                the joint so the gap reads as "lost" rather than "the HUD
//                forgot to draw an arm"
//   bleeding  -> flashes between its damage colour and hot red, on a wall-clock
//                sine (ImGui::GetTime) so the pulse is frame-rate independent
//   damaged   -> lerp from bone-white at full hp to deep red at zero
//
// Drawn bottom-up from `yBottom` and returns its own height so the caller can
// stack whatever comes next without restating the layout.
float Overlay::DrawBodyFigure(const UIState& s, float x, float yBottom) {
  // Proportions in figure-local units, then scaled. Origin is the pelvis.
  const float unit = 5.0f;                 // px per figure unit
  const float legLen = 3.0f, armLen = 2.6f, spineLen = 3.2f;
  const float shoulder = 2.2f, hipW = 1.3f, headR = 1.5f * unit;
  const float thick = 3.2f;
  const float height = (spineLen + legLen * 2 + 3.4f) * unit;
  if (!s.bodyValid) return height;

  ImDrawList* d = ImGui::GetForegroundDrawList();

  // Pelvis sits one leg-span up from the bottom edge; the head crown lands at
  // the top of the reserved height.
  const float px = x + 3.0f * unit;   // a little inset so arms have room
  const float py = yBottom - legLen * 2.0f * unit - 2.0f;
  auto P = [&](float fx, float fy) {  // figure units -> screen, +fy is UP
    return ImVec2(px + fx * unit, py - fy * unit);
  };

  // Wall-clock flash for bleeding parts. One phase for the whole body so the
  // wounds pulse together and read as one alarm rather than as noise.
  const float flash =
      0.5f + 0.5f * (float)sin(ImGui::GetTime() * 7.0f);

  // Damage tint: bone -> deep red as hp drops. Bleeding parts blend toward a
  // hot red on the flash phase.
  auto tint = [&](const UIState::BodyPartUI& b) {
    float f = b.hpFrac < 0 ? 0.0f : (b.hpFrac > 1 ? 1.0f : b.hpFrac);
    int r = (int)(215 + (200 - 215) * (1.0f - f));
    int g = (int)(220 * f * f + 30 * f);
    int bl = (int)(225 * f * f + 30 * f);
    // WHAT IS ON THE LIMB, over what has happened TO it — mixed in AFTER the
    // damage lerp and BEFORE the bleeding flash, and the order is the whole
    // point: a coat is a layer on the outside, so it sits over the damage
    // colour; an active haemorrhage is an ALARM and has to stay the loudest
    // thing on the figure even on a limb already drenched in something.
    //
    // 1.5x with a 0.85 ceiling: a light splash is visible without repainting
    // the limb, a soaked one reads as the substance, and the ceiling keeps a
    // sliver of the damage tint so a drenched limb that is ALSO half dead is
    // still distinguishable from a drenched healthy one.
    if (b.stainFrac > 0.0f && b.stainColor != 0) {
      const float k = std::min(0.85f, b.stainFrac * 1.5f);
      const int sr = (int)((b.stainColor >> IM_COL32_R_SHIFT) & 0xFFu);
      const int sg = (int)((b.stainColor >> IM_COL32_G_SHIFT) & 0xFFu);
      const int sb = (int)((b.stainColor >> IM_COL32_B_SHIFT) & 0xFFu);
      r = (int)(r + (sr - r) * k);
      g = (int)(g + (sg - g) * k);
      bl = (int)(bl + (sb - bl) * k);
    }
    if (b.bleeding) {
      r = (int)(r + (255 - r) * flash);
      g = (int)(g * (1.0f - 0.85f * flash));
      bl = (int)(bl * (1.0f - 0.85f * flash));
    }
    return IM_COL32(r, g, bl, 245);
  };

  // One limb segment. Severed parts leave a stump tick at `a` instead.
  auto seg = [&](int slot, ImVec2 a, ImVec2 b) {
    const UIState::BodyPartUI& p = s.body[slot];
    if (!p.present) return;
    if (p.severed) {
      // A short perpendicular tick at the joint: the wound, not the limb.
      const float dx = b.x - a.x, dy = b.y - a.y;
      const float len = sqrtf(dx * dx + dy * dy);
      if (len > 0.001f) {
        const float nx = -dy / len * 2.5f, ny = dx / len * 2.5f;
        d->AddLine(ImVec2(a.x - nx, a.y - ny), ImVec2(a.x + nx, a.y + ny),
                   IM_COL32(120, 40, 40, 200), 2.0f);
      }
      return;
    }
    d->AddLine(a, b, tint(p), thick);
  };

  // scrim, so the figure stays readable over a bright world
  d->AddRectFilled(ImVec2(x - 4, yBottom - height), ImVec2(x + 13.5f * unit,
                   yBottom + 2), IM_COL32(0, 0, 0, 70), 4.0f);

  // ---- skeleton points (figure units, pelvis at origin) ----
  const ImVec2 pelvis = P(0, 0);
  const ImVec2 neck = P(0, spineLen);
  const ImVec2 shL = P(-shoulder, spineLen * 0.92f);
  const ImVec2 shR = P(shoulder, spineLen * 0.92f);
  const ImVec2 elbL = P(-shoulder - armLen * 0.35f, spineLen * 0.92f - armLen);
  const ImVec2 elbR = P(shoulder + armLen * 0.35f, spineLen * 0.92f - armLen);
  const ImVec2 hndL = P(-shoulder - armLen * 0.6f,
                        spineLen * 0.92f - armLen * 2.0f);
  const ImVec2 hndR = P(shoulder + armLen * 0.6f,
                        spineLen * 0.92f - armLen * 2.0f);
  const ImVec2 hipL = P(-hipW, 0), hipR = P(hipW, 0);
  const ImVec2 kneeL = P(-hipW * 1.1f, -legLen), kneeR = P(hipW * 1.1f, -legLen);
  const ImVec2 ankL = P(-hipW * 1.2f, -legLen * 2.0f);
  const ImVec2 ankR = P(hipW * 1.2f, -legLen * 2.0f);

  // torso + hips are the spine; drawn first so limbs overlap them at the joints
  seg(UIState::kSlotHips, pelvis, P(0, spineLen * 0.4f));
  seg(UIState::kSlotTorso, P(0, spineLen * 0.4f), neck);
  seg(UIState::kSlotArmUL, shL, elbL);
  seg(UIState::kSlotArmLL, elbL, hndL);
  seg(UIState::kSlotArmUR, shR, elbR);
  seg(UIState::kSlotArmLR, elbR, hndR);
  seg(UIState::kSlotLegUL, hipL, kneeL);
  seg(UIState::kSlotLegLL, kneeL, ankL);
  seg(UIState::kSlotLegUR, hipR, kneeR);
  seg(UIState::kSlotLegLR, kneeR, ankR);

  // hands and feet are dots rather than segments — too short to read as lines
  auto dot = [&](int slot, ImVec2 at, float r) {
    const UIState::BodyPartUI& p = s.body[slot];
    if (!p.present || p.severed) return;
    d->AddCircleFilled(at, r, tint(p), 8);
  };
  dot(UIState::kSlotHandL, hndL, 2.4f);
  dot(UIState::kSlotHandR, hndR, 2.4f);
  dot(UIState::kSlotFootL, ankL, 2.4f);
  dot(UIState::kSlotFootR, ankR, 2.4f);

  // head: a circle on the neck, or an empty socket outline when decapitated
  const UIState::BodyPartUI& head = s.body[UIState::kSlotHead];
  if (head.present) {
    const ImVec2 hc(neck.x, neck.y - headR - 1.0f);
    if (head.severed) {
      d->AddCircle(hc, headR * 0.5f, IM_COL32(120, 40, 40, 170), 10, 1.5f);
    } else {
      d->AddCircleFilled(hc, headR, tint(head), 14);
    }
  }
  return height;
}

void Overlay::Draw(UIState& s) {
  // The character screen owns the frame while it is open: no crosshair (the
  // cursor is free), and it is drawn BEFORE the dev panel so the dev panel
  // stays reachable on top of it — F1 is not supposed to become unavailable
  // just because a menu is up.
  if (s.inventoryOpen) {
    ImGui::PushFont(ui::FontLarge());
    DrawInventoryScreen(s);
    ImGui::PopFont();
  } else {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 c = ImGui::GetIO().DisplaySize;
    c.x *= 0.5f;
    c.y *= 0.5f;
    dl->AddCircleFilled(c, 2.5f, IM_COL32(255, 255, 255, 200));
  }

  if (!s.visible) return;

  ImGui::PushFont(ui::FontSmall());

  ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
  ImGui::Begin("sandvox", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);

  ImGui::Text("%.0f fps  (%.1f ms avg, %.2f ms tick cpu)",
              s.fps, s.frameMs, s.tickCpuMs);
  // The tail gets its own line: four numbers do not fit beside the fps at this
  // panel width, and when you are chasing a stutter this line is the one you
  // watch. `worst` is a SINGLE frame out of the last half second, so it reacts
  // to any one-off hiccup; p95/p99 are over ~512 frames and are what say
  // whether the stutter is systematic. A high fps beside a high p99 is the
  // reading that matters — it means the average is being carried by cheap
  // frames while one in a hundred is visibly long.
  ImGui::Text("frame ms   p95 %.0f   p99 %.0f   worst %.0f",
              s.frameMsP95, s.frameMsP99, s.frameMsWorst);
  ImGui::Text("tick %u   active chunks %u / %u", s.tick, s.activeChunks,
              s.totalChunks);
  ImGui::Text("voxels %.2f M   particles %u   hash %08x %s", s.voxelTotal / 1e6,
              s.particleCount, s.worldHash, s.mirrorValid ? "" : "(mirror pending)");
  ImGui::Text("debris bodies %u (%u awake)   mobs %u", s.bodyCount,
              s.activeBodyCount, s.mobCount);
  ImGui::Text("pos %.0f %.0f %.0f  (%s)", s.playerPos[0], s.playerPos[1],
              s.playerPos[2], s.fly ? "fly" : "walk");
  // Ledge-grab state: green while hanging, yellow when a lip is in reach,
  // dim otherwise — with the latch gate flags spelled out (see UIState).
  {
    const ImVec4 c = s.ledgeState == 2   ? ImVec4(0.35f, 1.0f, 0.45f, 1.0f)
                     : s.ledgeState == 1 ? ImVec4(1.0f, 0.85f, 0.30f, 1.0f)
                                         : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    ImGui::TextColored(c, "ledge: %s", s.ledgeText.c_str());
  }

  // crosshair readout: what material the centre ray landed on. The swatch is
  // the same gpu color0 the material combo uses, so eyeballing "is that ice or
  // glass?" doesn't need the name to be read.
  if (s.hoverMat > 0 && s.hoverMat < (int)s.materialNames.size()) {
    if (s.hoverMat < (int)s.materialColors.size()) {
      uint32_t c = s.materialColors[s.hoverMat];  // 0xAABBGGRR
      ImVec4 col(((c) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                 ((c >> 16) & 0xFF) / 255.0f, 1.0f);
      ImGui::ColorButton("##hoversw", col,
                         ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                         ImVec2(14, 14));
      ImGui::SameLine();
    }
    ImGui::Text("looking at %s  [%d %d %d]  %.1fm",
                s.materialNames[s.hoverMat].c_str(), s.hoverCell[0],
                s.hoverCell[1], s.hoverCell[2], s.hoverDist);
  } else {
    ImGui::TextDisabled("looking at ---");
  }
  ImGui::Separator();

  // ---- magic (game/spell.h) -------------------------------------------------
  // The whole point of this readout is the CROSSOVER: the exact point where
  // the running cost stops coming out of mana and starts coming out of health.
  // It is drawn as one continuous bar with a hard break at that point, because
  // a pair of numbers does not communicate "this next glyph will cost you an
  // arm" the way a bar segment eating into red does.
  ImGui::Text("magic %s   (Z toggles; a number SELECTS a bound spell, RMB casts it)",
              s.magicMode ? "ON" : "off");
  {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 14.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const int32_t poolMax = s.manaMax > 0 ? s.manaMax : 1;
    // The bar spans mana + health so both costs are measured on ONE axis;
    // otherwise the crossover has no visual meaning.
    const int32_t span = poolMax + (s.health > 0 ? s.health : 0);
    auto frac = [&](int32_t v) {
      return span > 0 ? (float)v / (float)span : 0.0f;
    };
    // backdrop: mana region then health region
    d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(28, 32, 46, 255));
    const float manaEdge = p.x + w * frac(poolMax);
    d->AddRectFilled(ImVec2(manaEdge, p.y), ImVec2(p.x + w, p.y + h),
                     IM_COL32(52, 22, 24, 255));
    // filled mana
    d->AddRectFilled(p, ImVec2(p.x + w * frac(s.mana), p.y + h),
                     IM_COL32(70, 130, 235, 255));
    // filled health, starting at the mana edge
    d->AddRectFilled(ImVec2(manaEdge, p.y),
                     ImVec2(manaEdge + w * frac(s.health), p.y + h),
                     IM_COL32(190, 60, 60, 255));
    // THE CROSSOVER. The spoken cost is drawn as a bright overlay eating
    // right-to-left out of mana; the part of it past the mana edge is drawn in
    // warning colour because that part is coming out of the body.
    if (s.spellCost > 0) {
      const int32_t fromMana = s.spellCost < s.mana ? s.spellCost : s.mana;
      const int32_t fromHealth = s.spellCost - fromMana;
      const float x0 = p.x + w * frac(s.mana - fromMana);
      d->AddRectFilled(ImVec2(x0, p.y), ImVec2(p.x + w * frac(s.mana), p.y + h),
                       IM_COL32(150, 200, 255, 255));
      if (fromHealth > 0) {
        const float hx = manaEdge + w * frac(fromHealth < s.health ? fromHealth
                                                                   : s.health);
        d->AddRectFilled(ImVec2(manaEdge, p.y), ImVec2(hx, p.y + h),
                         IM_COL32(255, 140, 60, 255));
      }
      // the hard break itself
      d->AddLine(ImVec2(manaEdge, p.y - 2), ImVec2(manaEdge, p.y + h + 2),
                 IM_COL32(255, 255, 255, 220), 2.0f);
    }
    ImGui::Dummy(ImVec2(w, h + 4));
  }
  ImGui::Text("mana %d/%d   health %d   cost %d%s", s.mana, s.manaMax, s.health,
              s.spellCost, s.spellPriceUnknown ? " + ?" : "");
  if (s.spellCost > 0 || s.spellPriceUnknown)
    ImGui::TextDisabled("  word %d + tariff %d + carry %d%s", s.spellWord, s.spellTariff,
                        s.spellCarry,
                        s.spellPriceUnknown ? "   (anything: priced when it lands)" : "");
  if (s.spellLastBillAge < 2.5f && s.spellLastBill > 0)
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "billed %d on resolve",
                       s.spellLastBill);
  // Dev control of the pool: set the max to anything up to 2^30 (ResolveCast
  // adds mana + health in an int32, so the ceiling stays below overflow),
  // fill it once, or pin it full every tick. Requests only; main.cpp applies.
  if (ImGui::TreeNode("dev: mana pool")) {
    constexpr int32_t kDevManaCeiling = 1 << 30;
    if (s.devManaMaxEdit <= 0) s.devManaMaxEdit = s.manaPoolMax > 0 ? s.manaPoolMax : 100;
    ImGui::SetNextItemWidth(140.0f);
    int edit = s.devManaMaxEdit;
    if (ImGui::InputInt("max", &edit, 100, 10000)) s.devManaMaxEdit = edit;
    if (s.devManaMaxEdit < 1) s.devManaMaxEdit = 1;
    if (s.devManaMaxEdit > kDevManaCeiling) s.devManaMaxEdit = kDevManaCeiling;
    ImGui::SameLine();
    if (ImGui::Button("apply max")) s.devManaMaxRequest = s.devManaMaxEdit;
    ImGui::SameLine();
    if (ImGui::Button("fill")) s.devManaFill = true;
    // Presets apply immediately AND fill, which is what "ridiculous" means in
    // practice — nobody sets 1e9 and then wants to wait for regen.
    struct Preset {
      const char* label;
      int32_t value;
    };
    const Preset presets[] = {{"100", 100},      {"1k", 1000},        {"100k", 100000},
                              {"10M", 10000000}, {"1G", 1000000000}, {"2^30", kDevManaCeiling}};
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
      if (i > 0) ImGui::SameLine();
      if (ImGui::SmallButton(presets[i].label)) {
        s.devManaMaxEdit = presets[i].value;
        s.devManaMaxRequest = presets[i].value;
        s.devManaFill = true;
      }
    }
    ImGui::Checkbox("infinite (refill to max every tick)", &s.devManaInfinite);
    ImGui::TreePop();
  }
  if (s.spellCost > s.mana + s.health) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                       "FATAL - this will kill you");
  } else if (s.spellCost > s.mana) {
    ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f),
                       "unstable - %d from your body", s.spellCost - s.mana);
  }
  if (!s.spellText.empty()) {
    // THE BRACKET TEXT STAYS. It is what the oracle compares and what the dev
    // panel reads a tree off; the player-facing surface is the grimoire page's
    // canvas. "held" rather than "speaking" because the stack is now the
    // SELECTED spell, not a half-spoken one.
    ImGui::Text("held: %s", s.spellText.c_str());
    ImGui::TextDisabled("%s", s.spellVerdict.c_str());
  } else {
    ImGui::TextDisabled("held: (nothing)   a number selects a bound spell, "
                        "RMB casts it, Backspace clears");
  }
  if (!s.spellStatuses.empty()) {
    ImGui::Text("sustaining (%d reserved, Delete drops the newest):", s.manaReserved);
    for (const std::string& st : s.spellStatuses) ImGui::TextDisabled("  %s", st.c_str());
  }
  if (s.spellRefused > 0)
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "ward refused %d", s.spellRefused);
  if (!s.glyphSlots.empty()) {
    // Two rows: bank A on the number row, bank B on Shift. The bank Shift is
    // holding is drawn bright; a page shows as its name with a page mark.
    for (int bank = 0; bank < 2; bank++) {
      std::string strip = bank == 0 ? "1-0:   " : "S+1-0: ";
      bool any = false;
      for (size_t i = (size_t)bank * 10; i < s.glyphSlots.size() && i < (size_t)(bank + 1) * 10; i++) {
        if (s.glyphSlots[i].empty()) continue;
        any = true;
        const bool page = i < s.glyphSlotKinds.size() && s.glyphSlotKinds[i] == 2;
        // The SELECTED key is starred: the stack is its spell, and right-click
        // will fire that one until another key takes its place.
        const bool sel = (int)i == s.glyphSelected;
        strip += (sel ? "*" : "") + std::to_string((i + 1) % 10) + ":" +
                 (page ? "[" : "") + s.glyphSlots[i] + (page ? "]" : "") + "  ";
      }
      if (!any) continue;
      if (s.glyphBankB == (bank == 1)) ImGui::Text("%s", strip.c_str());
      else ImGui::TextDisabled("%s", strip.c_str());
    }
  }
  if (s.spellNoteAge < 3.0f && !s.spellNote.empty())
    ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.5f, 1.0f), "%s", s.spellNote.c_str());
  ImGui::Text("projectiles %d", s.liveProjectiles);
  if (s.spellOpsDropped > 0) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "  %d ops dropped",
                       s.spellOpsDropped);
  }
  // Wind primitives. Shown next to the projectile count because they are the
  // same kind of thing: a bounded population of live effects the player made,
  // each one costing until it expires. `wake` is the rule-2 number — the chunks
  // those primitives are holding awake so they can move settled matter, against
  // the sim.windWakeChunks budget.
  if (s.windPrims > 0 || s.windPrimsDropped > 0) {
    ImGui::Text("wind primitives %d (wake %d chunks)", s.windPrims,
                s.windWakeChunks);
    if (s.windPrimsDropped > 0) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "  %d refused (cap)",
                         s.windPrimsDropped);
    }
  }
  ImGui::Separator();

  // ---- hotbar + melee (game/item.h, game/melee.h) ---------------------------
  // The swing readout exists to make the input FALSIFIABLE. A cut is directed
  // by mouse motion, so when it goes wrong the player needs to tell "the game
  // misread my flick" from "I misjudged the distance" — showing the phase and
  // the speed the state machine actually measured is what makes that
  // answerable instead of a matter of opinion.
  if (!s.itemNames.empty()) {
    std::string strip;
    for (size_t i = 0; i < s.itemNames.size(); i++) {
      if (s.itemNames[i].empty()) continue;
      const bool sel = (int)i == s.itemSelected;
      strip += (sel ? "[" : " ") + std::to_string((i + 1) % 10) + ":" +
               s.itemNames[i] + (sel ? "] " : "  ");
    }
    if (!strip.empty()) ImGui::TextDisabled("%s", strip.c_str());
  }
  if (s.swingPhase && s.swingPhase[0]) {
    ImGui::Text("swing %s", s.swingPhase);
    ImGui::SameLine();
    ImGui::TextDisabled("  mouse %.0f px/s", s.swingSpeed);
    if (!s.swingStyle.empty()) ImGui::TextDisabled("%s", s.swingStyle.c_str());
  }
  ImGui::Separator();

  if (ImGui::Button(s.paused ? "resume (P)" : "pause (P)")) s.paused = !s.paused;
  ImGui::SameLine();
  if (ImGui::Button("step (N)")) s.stepOnce = true;
  ImGui::SameLine();
  ImGui::Checkbox("shadows", &s.shadows);

  // ---- short range: the two ceiling + fog comparison arms -------------------
  // Next to `shadows` because it is the same kind of switch: session state
  // that reaches the shader as a RenderParams flag, not a tuning value (see
  // State::shortRange for why that distinction is load-bearing here).
  //
  // THREE radios rather than two checkboxes: the arms are mutually exclusive
  // and "short range + near" as two independent ticks would let the user set a
  // near arm that does nothing. The metres come from tuning and not from
  // literals, so the labels track the sliders the moment F5 lands — a button
  // that says "50 m" while the shader ceilings at 80 is worse than no label.
  {
    const auto& rt = CurrentTuning().render;
    char nearLbl[24], farLbl[24];
    std::snprintf(nearLbl, sizeof nearLbl, "%.0f m", rt.shortRangeNearDist);
    std::snprintf(farLbl, sizeof farLbl, "%.0f m", rt.shortRangeDist);
    ImGui::TextUnformatted("short range");
    ImGui::SameLine();
    if (ImGui::RadioButton("off", !s.shortRange)) s.shortRange = false;
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
  }
  ImGui::SameLine();
  // The metres readout is the whole reason the row carries one more widget:
  // "short range" is a claim, and the effective draw distance dropping from
  // four digits to the ceiling the instant it is picked is the evidence for
  // it. It also shows the cascade REFILLING after a teleport, since the normal
  // value is the filled radius rather than the theoretical horizon.
  ImGui::TextDisabled("draw %.0f m", s.renderRangeM);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "Effective draw distance, in metres.\n\n"
        "Normally this is the far cascade's FILLED radius, so it dips while\n"
        "the cascade refills after a teleport or a fast sprint and climbs\n"
        "back to the full horizon when every level has landed.\n\n"
        "On either short-range arm it is that arm's ceiling, and that is a\n"
        "hard ceiling on every ray: the fine march and all eight cascade\n"
        "levels stop there. This is a PERF mode - the frame stops paying for\n"
        "the horizon - not a fog filter over a full-range image.\n\n"
        "Shape it under Rendering: shortRangeDist (the far arm),\n"
        "shortRangeNearDist (the near one), shortRangeFogStart,\n"
        "shortRangeFogDensity. Both arms share the fog ramp's shape, so only\n"
        "the wall moves between them. The choice itself is session state and\n"
        "is not saved to tuning.json.");

  // ---- celestial time -------------------------------------------------
  // Scales the clock the SKY and the daylight-gated reactions both run on
  // (sim/world.h CelestialClock). The sim tick rate is untouched — sand still
  // falls at 30 Hz — but the sun, both moons, the seasons and every
  // sun-driven reaction run at this multiple. Anything but 1x changes the
  // world hash, which the tooltip says out loud.
  {
    float t = std::cbrt(s.timeScale / 100.0f);
    if (ImGui::SliderFloat("time speed", &t, -1.0f, 1.0f, "")) {
      s.timeScale = t * t * t * 100.0f;
      if (std::abs(s.timeScale) < 0.05f) s.timeScale = 0.0f;
    }
    ImGui::SameLine();
    ImGui::Text("%.2fx", s.timeScale);
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "Speed of the CELESTIAL clock: the sun, both moons, the seasons, and\n"
        "the daylight-gated reactions (water freezing at night, snow melting\n"
        "in the sun) all run at this multiple. The simulation itself still\n"
        "ticks at 30 Hz - sand does not fall faster.\n\n"
        "0 freezes the sky, negative runs it backwards.\n\n"
        "Anything but 1x changes the world hash on purpose. --selftest never\n"
        "engages this clock, so the pinned hash is unaffected.");
  if (ImGui::Button("1x")) s.timeScale = 1.0f;
  ImGui::SameLine();
  if (ImGui::Button("10x")) s.timeScale = 10.0f;
  ImGui::SameLine();
  if (ImGui::Button("100x")) s.timeScale = 100.0f;
  ImGui::SameLine();
  if (ImGui::Button("freeze")) s.timeScale = 0.0f;
  ImGui::SameLine();
  if (ImGui::Button("rev")) s.timeScale = -1.0f;
  // Readout of what the orbital solve actually produced. Phases are shown as
  // named quarters because "0.73" tells you nothing about what is in the sky.
  {
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
    ImGui::TextDisabled("sky %02d:%02d  sun %+.0f\xc2\xb0  year %.0f%%",
                        hh, mm, s.skySunElevDeg, s.skyYearT * 100.0f);
    ImGui::TextDisabled("moon A %s (%.2f)   moon B %s (%.2f)",
                        phaseName(s.skyMoonPhase), s.skyMoonPhase,
                        phaseName(s.skyMoon2Phase), s.skyMoon2Phase);
    if (s.skySolarEclipse > 0.995f) {
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                         "*** TOTAL SOLAR ECLIPSE ***");
    } else if (s.skySolarEclipse > 0.0f) {
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                         "solar eclipse: %.0f%% covered",
                         s.skySolarEclipse * 100.0f);
    }
  }

  ImGui::Checkbox("fly (V)", &s.fly);
  ImGui::Checkbox("collision boxes (F3)", &s.showCollisionBoxes);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "Wireframes around every physics collider, read from the actual Jolt\n"
        "shape rather than the art: green = avatar parts (a held item too),\n"
        "cyan = mob limbs, yellow = loose debris.\n"
        "Drawn THROUGH walls on purpose - the reason to look at a collider is\n"
        "usually that something is on top of it.");
  ImGui::Checkbox("active voxels", &s.showDirtyVoxels);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "Red wireframe on every voxel the CA wrote this tick. Filled\n"
        "GPU-side so there is no snapshot lag or stamp aliasing.\n"
        "Combine with F6 (dirty chunks) to see cause and effect.");
  // One radio row rather than two checkboxes, because the underlying state is
  // one integer: F4 cycles off -> wind -> current and the panel is the same
  // control by another route, so a pair of boxes would have to forbid the
  // both-on combination that its own shape advertises.
  //
  // The tooltip describes all three states, so it hangs off EVERY widget in the
  // row rather than off the last one — the reason to hover "off" is usually to
  // find out what the other two would show.
  static constexpr const char* kFieldVizTip =
      "An arrow per lattice point around you, coloured by speed, cool to\n"
      "hot. F4 cycles off -> wind -> current; only one is ever drawn,\n"
      "because two overlapping lattices in one frame read as noise.\n"
      "\n"
      "WIND samples the SAME windAt() the grass sway does, so the arrows\n"
      "show what the foliage is standing in - turn the Wind tab's direction\n"
      "knob and both must swing together. Full scale is 24 m/s.\n"
      "CURRENT samples the SAME currentAt() the waves advect with and\n"
      "floating debris is dragged by, so the arrows show what is pushing a\n"
      "raft. Full scale is 4 m/s - at a wind scale every current in the\n"
      "world is one shade of blue.\n"
      "\n"
      "The current arrows do NOT need sim.currentMode: the render arm of the\n"
      "field is always live (a renderer cannot write a voxel), so they show\n"
      "the resolved primitives whether or not the sim is being pushed by\n"
      "them. What they cannot show is a field with nothing in it - streams\n"
      "and drains are seeded every tick, so a river has arrows and still\n"
      "water is honestly empty.\n"
      "\n"
      "Arrows pointing at or away from you fade out: one aimed down the view\n"
      "ray cannot show its direction anyway, so the hole is honest.\n"
      "Spacing and radius are per field - Wind tab and render.dbgCurrent*\n"
      "(F5 to apply).\n"
      "NOTE: F5 re-seeds this from wind.dbgWindField / render.dbgCurrentField,\n"
      "so a reload turns it back off unless the tuning file asks for it.";
  const auto fieldVizTip = [&] {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kFieldVizTip);
  };
  ImGui::TextUnformatted("vector field (F4)");
  fieldVizTip();
  ImGui::SameLine();
  ImGui::RadioButton("off##fieldviz", &s.fieldViz, UIState::kFieldVizOff);
  fieldVizTip();
  ImGui::SameLine();
  ImGui::RadioButton("wind##fieldviz", &s.fieldViz, UIState::kFieldVizWind);
  fieldVizTip();
  ImGui::SameLine();
  ImGui::RadioButton("current##fieldviz", &s.fieldViz, UIState::kFieldVizCurrent);
  fieldVizTip();

  // ---- the sky's weather (src/sim/weather.h) -------------------------------
  // Its own section of the F1 panel, open by default: the preset pin (a
  // render-only override that eases the sky over weather.transitionSeconds
  // without touching tuning.json — the running game writes that file, and a
  // look switch must not clobber saved defaults), and live edits of the
  // CPU-only weather knobs. Every one of them is render-only: the world hash
  // never sees the weather, so nothing here needs a shader reload or moves a
  // pinned number.
  ImGui::Separator();
  if (ImGui::CollapsingHeader("Weather", ImGuiTreeNodeFlags_DefaultOpen)) {
    const std::vector<weather::Preset>& ps = weather::Presets().Presets();
    const std::string cur = weather::Override();
    const weather::State& w = weather::Last();
    const char* shown = cur.empty() ? "(automatic / tuning)" : cur.c_str();
    if (ImGui::BeginCombo("sky##wxcombo", shown)) {
      if (ImGui::Selectable("(automatic / tuning)", cur.empty())) weather::SetOverride("");
      for (const weather::Preset& p : ps) {
        std::string lab = p.label + "##wx" + p.name;
        if (ImGui::Selectable(lab.c_str(), cur == p.name)) weather::SetOverride(p.name);
      }
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "Pin the sky to one assets/weather preset; it eases in over\n"
          "weather.transitionSeconds. (automatic / tuning) hands it back to\n"
          "the automatic cycle, or to weather.preset when that is off.\n"
          "Render-only: the world hash never sees it. R reloads the files.");
    // One-click row: the presets as small buttons, so switching the sky
    // while looking at it is a single click rather than a dropdown.
    {
      int n = 0;
      for (const weather::Preset& p : ps) {
        if (n++ % 4 != 0) ImGui::SameLine();
        const bool on = cur == p.name;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        std::string lab = p.label + "##wxb" + p.name;
        if (ImGui::SmallButton(lab.c_str())) weather::SetOverride(on ? "" : p.name);
        if (on) ImGui::PopStyleColor();
      }
    }
    if (w.fromName == w.toName)
      ImGui::Text("now: %s", w.fromName.c_str());
    else
      ImGui::Text("now: %s -> %s (%.0f%%)", w.fromName.c_str(), w.toName.c_str(),
                  w.blend * 100.0f);
    ImGui::Text("cover %.2f  rain %.2f  wet %.2f  overcast %.2f%s", w.mix.coverage,
                w.mix.precip, w.wetness, w.overcast, w.flash > 0.05f ? "  *flash*" : "");

    Tuning t = CurrentTuning();
    Tuning::Weather& wt = t.weather;
    bool changed = false;
    changed |= ImGui::Checkbox("clouds##wx", &wt.clouds);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Master switch. Off records no cloud pass at all.");
    ImGui::SameLine();
    changed |= ImGui::Checkbox("automatic cycle##wx", &wt.autoCycle);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "On: the sky walks the preset ladder by moisture on its own\n"
          "(clear -> fair -> scattered -> overcast -> rain -> storm and back).\n"
          "Off: it holds weather.preset. A pin above overrides both.");
    changed |= EditableSliderFloat("cycle speed##wx", &wt.cycleSpeed, 0.0f, 50.0f, "%.1fx");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "How fast the automatic weather runs. 0 freezes it; 20x shows a\n"
          "whole afternoon of weather in a few minutes.");
    changed |= EditableSliderFloat("coverage bias##wx", &wt.coverageBias, -1.0f, 1.0f, "%+.2f");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Added to every preset's coverage: cloudier / clearer.");
    changed |= EditableSliderFloat("raininess x##wx", &wt.precipScale, 0.0f, 4.0f, "%.2fx");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Multiplies every preset's raininess: wetter / drier.");
    changed |= EditableSliderFloat("ease (s)##wx", &wt.transitionSeconds, 0.0f, 60.0f, "%.0f s");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("How long a pinned change takes to blend in.");
    if (ImGui::SmallButton("reroll weather##wx")) {
      wt.seedOffset = (wt.seedOffset + 1) % 1000;
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("A different automatic weather sequence (weather.seedOffset).");
    if (changed) SetCurrentTuning(t);
  }
  ImGui::Separator();

  // ---- wind force multipliers, one per tier -------------------------------
  // Live: these ride TickParams, so a drag lands on the next tick with no
  // shader reload. Split by TIER because that is how the engine is split
  // (research doc §4.6) — the CA steers what is already moving, the particle
  // system carries the violence — and because they are the two things you want
  // to A/B against each other. Pinning one to 0 while pushing the other is the
  // fastest way to see which tier a given effect is actually coming from.
  if (EditableSliderFloat("wind x voxels", &s.windGasScale, 0.0f, 16.0f, "%.2fx"))
    s.windTuningDirty = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "Multiplies how hard the wind pushes CA VOXELS - smoke, steam, fire,\n"
        "and falling powder. It scales the drift-bias PROBABILITY, not the\n"
        "wind speed, and it is allowed past the sim.windDriftMax cap: at the\n"
        "top of the range every moving gas voxel goes downwind first and smoke\n"
        "stops looking like smoke and starts looking like a conveyor belt.\n"
        "Scaling the speed instead would go dead at about 2x, because the bias\n"
        "ramp already saturates near the default weather.\n"
        "SETTLED voxels are untouched at any value - that is entrainment,\n"
        "which is sim.windMode 2 and off. 0 pins the CA tier still.\n"
        "Changes the world hash. Deterministic, just a different world.");
  if (EditableSliderFloat("wind x particles", &s.windPartScale, 0.0f, 16.0f, "%.2fx"))
    s.windTuningDirty = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "Multiplies how hard the wind pushes the PARTICLE tier - explosion\n"
        "debris, blood and water spray, and MPM fluid surface nodes.\n"
        "It scales the wind VELOCITY they are dragged toward, which is what\n"
        "actually throws them further: the drag law means a particle can never\n"
        "outrun the air, so a faster air is the only way past that ceiling.\n"
        "Per-material response still applies, so heavy debris moves less than\n"
        "spray at the same multiplier. 0 pins the particle tier still.\n"
        "Changes the world hash. Deterministic, just a different world.");
  if (EditableSliderFloat("wind fall onset", &s.windDragRef, 1.0f, 120.0f,
                          "%.0f m/s"))
    s.windTuningDirty = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "How hard a wind it takes before falling debris feels the air.\n"
        "Drag on a particle pulls its velocity toward the local wind on EVERY\n"
        "axis, so a horizontal field drags the VERTICAL toward zero - that is\n"
        "air resistance, and at a fixed rate it applies just as hard on a calm\n"
        "day as in a gale. This is the wind speed at which sim.windDrag counts\n"
        "in full; below it the RATE ramps down with the wind, so calm air is\n"
        "ballistic and gravity is left alone.\n"
        "Terminal fall against the 6 vox/tick ballistic cap, at the default\n"
        "6 m/s weather: 120 -> 6.0 (no change), 40 -> 5.7, 20 -> 2.9,\n"
        "6 -> 0.86 (debris drifts down like ash).\n"
        "LOW makes ordinary weather floaty; HIGH means only a storm is felt.\n"
        "Changes the world hash. Deterministic, just a different world.");

  // ---- place a wind primitive (docs/RESEARCH_wind.md §4.3) ---------------
  // The button that turns wind from weather into a tool you can point at
  // something. It emits the SAME parametric object a `gust` spell emits, on
  // the same list, through the same budget — there is no dev-only wind path.
  if (ImGui::TreeNode("wind primitives (fans / gusts / vortices)")) {
    ImGui::TextDisabled("%d live, waking %d chunks", s.windPrims,
                        s.windWakeChunks);
    const char* kinds[] = {"cone (fan / jet)", "burst (blast or vacuum)",
                           "vortex (tornado)"};
    ImGui::Combo("kind", &s.windFanKind, kinds, 3);
    EditableSliderFloat("speed", &s.windFanSpeed, -40.0f, 40.0f, "%.0f m/s");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "Core speed at the mouth. NEGATIVE is legal and useful: it turns a\n"
          "burst into a vacuum and a cone into a draw.");
    EditableSliderInt("radius", &s.windFanRadius, 1, 64);
    EditableSliderInt("reach", &s.windFanReach, 1, 128);
    ImGui::Checkbox("may move SETTLED powder", &s.windFanEntrain);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "The entrainment licence. OFF, a fan only steers what is already\n"
          "moving - smoke, spray, falling sand - and costs nothing when the\n"
          "world around it is asleep.\n"
          "ON, it may pull RESTING powder loose inside its footprint, which is\n"
          "what blows a dune flat. That costs: the primitive dirty-marks its\n"
          "own footprint every tick so those chunks are simulated at all, and\n"
          "the chunks are charged against sim.windWakeChunks. It is per\n"
          "primitive rather than global because the global version is not\n"
          "page-table safe - see sim.windMode 2.");
    if (ImGui::Button("place where I'm looking")) s.placeWindFan = true;
    ImGui::SameLine();
    if (ImGui::Button("clear all")) s.clearWindFans = true;
    if (s.windPrimsDropped > 0)
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                         "%d refused (world cap is %d)", s.windPrimsDropped,
                         (int)kWindPrimCap);
    ImGui::TreePop();
  }

  ImGui::SliderInt("brush radius [ ]", &s.brushRadius, 1, 7);

  auto swatch = [&](int i) {
    if (i >= (int)s.materialColors.size()) return;
    uint32_t c = s.materialColors[i];  // 0xAABBGGRR
    ImVec4 col(((c) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
               ((c >> 16) & 0xFF) / 255.0f, 1.0f);
    ImGui::ColorButton(("##sw" + std::to_string(i)).c_str(), col,
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                       ImVec2(14, 14));
    ImGui::SameLine();
  };
  if (ImGui::BeginCombo("material",
                        s.brushMaterial < (int)s.materialNames.size()
                            ? s.materialNames[s.brushMaterial].c_str()
                            : "?")) {
    for (int i = 1; i < (int)s.materialNames.size(); i++) {
      swatch(i);
      if (ImGui::Selectable(s.materialNames[i].c_str(), i == s.brushMaterial))
        s.brushMaterial = i;
    }
    ImGui::EndCombo();
  }

  ImGui::Separator();
  ImGui::Checkbox("dev controls (F2)", &s.devControls);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("off = play mode: hands only, number row picks the hotbar,\n"
                      "no brush / spawn / fly / tool keys");
  ImGui::Text("tool (Tab):");
  ImGui::SameLine();
  ImGui::RadioButton("brush", &s.tool, UIState::kToolBrush);
  ImGui::SameLine();
  ImGui::RadioButton("laser", &s.tool, UIState::kToolLaser);
  ImGui::SameLine();
  ImGui::RadioButton("prefab", &s.tool, UIState::kToolPrefab);
  ImGui::SameLine();
  ImGui::RadioButton("mob", &s.tool, UIState::kToolMob);
  ImGui::SameLine();
  ImGui::RadioButton("sword", &s.tool, UIState::kToolMelee);
  ImGui::SameLine();
  ImGui::RadioButton("mpm", &s.tool, UIState::kToolFluid);

  if (ImGui::Button("Combat")) s.combatWindowOpen = !s.combatWindowOpen;
  ImGui::SameLine();
  if (ImGui::Button("NPC AI")) s.aiWindowOpen = !s.aiWindowOpen;
  ImGui::SameLine();
  if (ImGui::Button("Fluid")) s.fluidWindowOpen = !s.fluidWindowOpen;
  ImGui::SameLine();
  if (ImGui::Button("Wardrobe")) s.wardrobeWindowOpen = !s.wardrobeWindowOpen;

  if (s.tool == UIState::kToolMelee) {
    ImGui::TextDisabled("hold LMB to guard, then FLICK the mouse to cut");
  }
  if (s.tool == UIState::kToolFluid) {
    ImGui::TextDisabled("hold LMB: pour  1-4 species  U clear");
    ImGui::Text("mpm particles: %u / 262144", s.fluidCount);
    ImGui::SameLine();
    if (ImGui::Button("clear (U)")) s.clearFluid = true;
  }
  if (s.tool == UIState::kToolBrush) {
    ImGui::TextDisabled("LMB paint  RMB erase  1-8 / combo below");
  } else if (s.tool == UIState::kToolLaser) {
    ImGui::TextDisabled("hold LMB (or F): melts what it hits, cuts bodies");
  } else if (s.tool == UIState::kToolPrefab) {
    if (!s.prefabNames.empty()) {
      if (s.prefabSelected >= (int)s.prefabNames.size()) s.prefabSelected = 0;
      ImGui::TextUnformatted("prefab");
      ImGui::SameLine();
      // "##prefab" not "prefab": the label text would otherwise hash to the
      // same ID as the RadioButton("prefab") above it — same window, same ID
      // stack level — and ImGui resolves both to one widget, so the dropdown
      // stops responding. The visible caption is drawn separately.
      if (ImGui::BeginCombo("##prefab", s.prefabNames[s.prefabSelected].c_str())) {
        // PushID(i) makes each row's ID its INDEX, not its label. Two assets
        // that happen to share a display name (or an empty one) would otherwise
        // hash to the same ImGui ID: they draw as "2 items with conflicting
        // id!" and — worse — every click resolves to whichever row won the ID,
        // so the selection cannot be changed. Keying on the index is correct
        // for ANY future name collision rather than only the ones we have.
        for (int i = 0; i < (int)s.prefabNames.size(); i++) {
          ImGui::PushID(i);
          if (ImGui::Selectable(s.prefabNames[i].c_str(), i == s.prefabSelected))
            s.prefabSelected = i;
          ImGui::PopID();
        }
        ImGui::EndCombo();
      }
      ImGui::Text("rotation %d°", s.prefabRot * 90);
      ImGui::SameLine();
      if (ImGui::Button("rotate (T)")) s.prefabRot = (s.prefabRot + 1) & 3;
      ImGui::SameLine();
      ImGui::Checkbox("overwrite", &s.prefabOverwrite);
      if (s.prefabPending > 0)
        ImGui::Text("placing... %u voxels pending", s.prefabPending);
      ImGui::TextDisabled("LMB place  T rotate  O cycle");
    } else {
      ImGui::TextDisabled("no prefabs (assets/prefabs/*.vox)");
    }
  } else if (s.tool == UIState::kToolMob) {
    if (!s.mobNames.empty()) {
      if (s.mobSelected >= (int)s.mobNames.size()) s.mobSelected = 0;
      ImGui::TextUnformatted("mob");
      ImGui::SameLine();
      // "##mob" —collides with RadioButton("mob") otherwise; see the prefab
      // combo above.
      if (ImGui::BeginCombo("##mob", s.mobNames[s.mobSelected].c_str())) {
        // Index-keyed IDs — see the prefab combo above for why. A mob def's
        // name comes from the .vox filename stem, so two mob files in different
        // states of a rename, or a def whose sidecar failed to load, can put
        // the same string in this list twice.
        for (int i = 0; i < (int)s.mobNames.size(); i++) {
          ImGui::PushID(i);
          if (ImGui::Selectable(s.mobNames[i].c_str(), i == s.mobSelected))
            s.mobSelected = i;
          ImGui::PopID();
        }
        ImGui::EndCombo();
      }
      ImGui::TextDisabled("LMB (or M) spawn at crosshair");
    } else {
      ImGui::TextDisabled("no mobs (assets/mobs/*.vox + .json)");
    }
  }
  ImGui::Separator();

  if (ImGui::Button("reload shaders (F5)")) s.reloadShaders = true;
  ImGui::SameLine();
  if (ImGui::Button("reload materials (R)")) s.reloadMaterials = true;
  ImGui::SameLine();
  if (ImGui::Button("reload environment + regen world (F7)")) s.regenWorld = true;

  if (ImGui::Button("save world (F9)")) s.saveWorld = true;
  ImGui::SameLine();
  if (ImGui::Button("load world (F10)")) s.loadWorld = true;
  ImGui::SameLine();

  if (ImGui::Button("detonate at crosshair (X)")) s.pendingDetonate = true;
  ImGui::SameLine();
  // The player's body goes limp for ragdoll.devSeconds, then gets back up:
  // the whole live-ragdoll path (Mob::StartRagdoll -> BeginGetUp) on demand.
  if (ImGui::Button("ragdoll me")) s.ragdollMe = true;

  // rolling sphere: rigidbody ball of the current brush material, so its
  // mass — and how far the player can shove it — comes from the material
  if (ImGui::Button("spawn sphere (K)")) s.spawnSphere = true;

  ImGui::TextDisabled("Tab switch tool  1-8 material  Esc cursor  F1 UI");
  ImGui::TextDisabled("G grenade  X detonate  F laser  M spawn mob  B place");
  ImGui::End();

  // ---- separate MPM fluid tuning window ----
  if (s.fluidWindowOpen) {
    ImGui::SetNextWindowPos(ImVec2(370, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("MPM Fluid Tuning", &s.fluidWindowOpen)) {
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
            fslider("stain rate",       &s.fStainRate,   0.0f, 30.0f);
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
            fcolor("species 1##l", s.fColor);
            fcolor("species 2##l", s.fColor1);
            fcolor("species 3##l", s.fColor2);
            fcolor("species 4##l", s.fColor3);
            ImGui::Separator();
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

  // ---- WARDROBE window (game/dye.h) ---------------------------------------
  //
  // THREE PATTERNS AND A COLOUR WHEEL. The clothes in assets/items are painted
  // in greyscale — every cell a multiplier rather than a pigment
  // (scripts/gen_peasant_clothes.py) — so what comes out of this window is a
  // pattern index and one packed RGB word, and the same nine .vox files dress
  // an entire village in nine hundred different outfits.
  //
  // Same shape as the AI window below: the overlay owns no game state, the
  // combos are mirrors main.cpp rebuilds off the live item library, and every
  // button is a one-shot bool main.cpp consumes. That is also what makes the
  // picker survive an R hot-reload — see UIState's wardrobe block.
  if (s.wardrobeWindowOpen) {
    ImGui::SetNextWindowPos(ImVec2(300, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 560), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Wardrobe", &s.wardrobeWindowOpen)) {
      ImGui::TextDisabled("three patterns per slot, any colour:");
      ImGui::TextDisabled("the art is a greyscale weave and the");
      ImGui::TextDisabled("colour is applied at shade time, so the");
      ImGui::TextDisabled("seams, hems and mends survive the dye");
      ImGui::Separator();

      auto combo = [&](const char* label, const std::vector<std::string>& names,
                       int& pick) {
        if (names.empty()) {
          ImGui::TextDisabled("%s: no dyeable pieces in items.json", label);
          return;
        }
        if (pick >= (int)names.size()) pick = 0;
        ImGui::SetNextItemWidth(150);
        // "##" so three combos of the same shape cannot hash together.
        const std::string id = std::string(label) + "##wardrobe";
        if (ImGui::BeginCombo(id.c_str(), names[pick].c_str())) {
          for (int i = 0; i < (int)names.size(); i++) {
            ImGui::PushID(i);
            if (ImGui::Selectable(names[i].c_str(), i == pick)) pick = i;
            ImGui::PopID();
          }
          ImGui::EndCombo();
        }
      };
      combo("shirt", s.wardrobeShirts, s.wardrobeShirtPick);
      combo("legs", s.wardrobeLegs, s.wardrobeLegsPick);
      combo("feet", s.wardrobeFeet, s.wardrobeFeetPick);

      ImGui::Separator();
      // THE COLOUR WHEEL. PickerHueWheel rather than the bar-and-square: a
      // wheel is the one picker where "somewhere over there in the greens" is a
      // single gesture, which is what choosing a villager's shirt actually is.
      // No alpha, no input boxes — a dye has neither.
      ImGui::ColorPicker3("##wardrobedye", s.wardrobeColor,
                          ImGuiColorEditFlags_PickerHueWheel |
                              ImGuiColorEditFlags_NoSidePreview |
                              ImGuiColorEditFlags_NoInputs |
                              ImGuiColorEditFlags_NoLabel);
      // The name main.cpp mirrored for this colour, beside a swatch of it. The
      // name is what ends up in the item's tooltip, so showing it here is how
      // you find out that the thing you picked is going to be called "rust".
      ImGui::ColorButton("##wardrobeswatch",
                         ImVec4(s.wardrobeColor[0], s.wardrobeColor[1],
                                s.wardrobeColor[2], 1.0f),
                         0, ImVec2(28, 28));
      ImGui::SameLine();
      ImGui::TextUnformatted(s.wardrobeColorName.empty()
                                 ? "(undyed)"
                                 : s.wardrobeColorName.c_str());
      ImGui::SameLine();
      if (ImGui::Button("random##wardrobe")) s.wardrobeRandomColor = true;

      ImGui::Separator();
      if (ImGui::Button("into the pack##wardrobe")) s.wardrobeSpawnSet = true;
      ImGui::SameLine();
      ImGui::TextDisabled("hotbar, else the bag");
      if (ImGui::Button("...and put it on##wardrobe")) s.wardrobeWearSet = true;
      ImGui::SameLine();
      ImGui::TextDisabled("straight into the equip slots");
      if (ImGui::Button("re-dye what I'm wearing##wardrobe"))
        s.wardrobeDyeWorn = true;
      ImGui::SameLine();
      ImGui::TextDisabled("every dyeable piece on the body");
      if (!s.wardrobeStatus.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", s.wardrobeStatus.c_str());
      }
      ImGui::Separator();
      ImGui::TextDisabled("a dye is PAINT, not a material: dyed linen");
      ImGui::TextDisabled("burns, tears and soaks blood exactly as");
      ImGui::TextDisabled("undyed linen does, and a sleeve cut off");
      ImGui::TextDisabled("keeps its colour on the ground");
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
    ImGui::SetNextWindowPos(ImVec2(720, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 720), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("NPC AI", &s.aiWindowOpen)) {
      if (ImGui::BeginTabBar("##aitabs")) {
        // ---- Spawn -------------------------------------------------------
        if (ImGui::BeginTabItem("Spawn")) {
          ImGui::TextDisabled("spawns a creature, a few metres ahead of you");
          ImGui::TextDisabled("(or at the crosshair hit):");
          // WHICH BODY. Every mob def that can hold a weapon and is not itself
          // a variant of one, mirrored by main.cpp off the live defs — so a new
          // creature appears here on the next R with no list to keep in step by
          // hand. What is WRONG with the body is the checkbox row below.
          if (!s.aiCreatureNames.empty()) {
            if (s.aiCreaturePick >= (int)s.aiCreatureNames.size())
              s.aiCreaturePick = 0;
            ImGui::SetNextItemWidth(160);
            if (ImGui::BeginCombo("creature##ai",
                                  s.aiCreatureNames[s.aiCreaturePick].c_str())) {
              for (int i = 0; i < (int)s.aiCreatureNames.size(); i++) {
                ImGui::PushID(i);
                if (ImGui::Selectable(s.aiCreatureNames[i].c_str(),
                                      i == s.aiCreaturePick))
                  s.aiCreaturePick = i;
                ImGui::PopID();
              }
              ImGui::EndCombo();
            }
          }
          // WHAT IS WRONG WITH IT. One box per published effect
          // (UIState::aiEffectNames): the panel no longer has a creature called
          // "zombie" in it, it has a body and a tick. Ticking one hands it to
          // MobSystem::DefWithEffects, which prefers an authored combination
          // (jujunud_zombie.json) and composes one when there is none — so
          // `jujunud` + zombie and `human` + zombie are both one click and
          // neither needs a def of its own.
          if (!s.aiEffectNames.empty()) {
            ImGui::TextDisabled("...and what is wrong with it:");
            for (int i = 0; i < (int)s.aiEffectNames.size(); i++) {
              if (i >= (int)s.aiEffectOn.size()) break;
              // Two per row: the list is short and a column of lone checkboxes
              // wastes the panel's height on the tab that has the most in it.
              if (i > 0 && (i & 1) != 0) ImGui::SameLine(190);
              ImGui::PushID(i);
              bool on = s.aiEffectOn[i] != 0;
              if (ImGui::Checkbox(s.aiEffectNames[i].c_str(), &on))
                s.aiEffectOn[i] = on ? 1 : 0;
              ImGui::PopID();
            }
            ImGui::TextDisabled("zombie: walks slower, comes apart when cut,");
            ImGui::TextDisabled("spawns already bitten, bites back");
          }
          ImGui::TextDisabled("armed with:");
          // WHICH WEAPON. Every melee item in the library plus "(unarmed)",
          // mirrored by main.cpp — so a blade added to items.json appears here
          // on the next R, with no list to keep in step by hand.
          if (!s.aiWeaponNames.empty()) {
            if (s.aiWeaponPick >= (int)s.aiWeaponNames.size())
              s.aiWeaponPick = 0;
            ImGui::SetNextItemWidth(160);
            // "##" so this cannot hash to the same id as the behaviour combo
            // on the Mobs tab.
            if (ImGui::BeginCombo("weapon##ai",
                                  s.aiWeaponNames[s.aiWeaponPick].c_str())) {
              for (int i = 0; i < (int)s.aiWeaponNames.size(); i++) {
                ImGui::PushID(i);
                if (ImGui::Selectable(s.aiWeaponNames[i].c_str(),
                                      i == s.aiWeaponPick))
                  s.aiWeaponPick = i;
                ImGui::PopID();
              }
              ImGui::EndCombo();
            }
            ImGui::TextDisabled("the blade decides the wound: reach, cut");
            ImGui::TextDisabled("depth and heft all come off its own art");
            ImGui::TextDisabled("\"fists\" is a weapon now, not the absence of");
            ImGui::TextDisabled("one: a bare hand bruises and never severs,");
            ImGui::TextDisabled("and a mace beats plate in where a sword");
            ImGui::TextDisabled("skates off it");
          }
          // WHAT IT WEARS. A mode combo, and under "custom" one picker per
          // worn equip slot — mirrors main.cpp rebuilds off the item library
          // (UIState::aiOutfit), so a new piece in assets/items appears here on
          // the next R with no list to keep in step by hand.
          ImGui::TextDisabled("wearing:");
          {
            static const char* kOutfits[] = {"nothing", "random clothes",
                                             "full plate", "custom pieces"};
            ImGui::SetNextItemWidth(160);
            ImGui::Combo("outfit##ai", &s.aiOutfit, kOutfits, 4);
            if (s.aiOutfit == 1) {
              ImGui::TextDisabled("a shirt, legs and shoes drawn from the");
              ImGui::TextDisabled("dyeable commoner set, each its own colour");
            } else if (s.aiOutfit == 2) {
              ImGui::TextDisabled("every slot's iron_* piece: helm, cuirass,");
              ImGui::TextDisabled("greaves, sabatons, gauntlets");
            } else if (s.aiOutfit == 3) {
              ImGui::TextDisabled("(none) leaves that slot bare");
              const int n = (int)std::min(s.aiWearNames.size(),
                                          s.aiWearSlotLabels.size());
              if ((int)s.aiWearPick.size() < n) s.aiWearPick.resize(n, 0);
              for (int i = 0; i < n; i++) {
                const std::vector<std::string>& names = s.aiWearNames[i];
                if (names.size() <= 1) continue;   // nothing fits this slot
                int& pick = s.aiWearPick[i];
                if (pick < 0 || pick >= (int)names.size()) pick = 0;
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(160);
                // The label is the slot's own name from the equip table, so
                // this reads "Head", "Chest", ... exactly as the character
                // screen does.
                if (ImGui::BeginCombo((s.aiWearSlotLabels[i] + "##aiwear").c_str(),
                                      names[pick].c_str())) {
                  for (int k = 0; k < (int)names.size(); k++) {
                    ImGui::PushID(k);
                    if (ImGui::Selectable(names[k].c_str(), k == pick)) pick = k;
                    ImGui::PopID();
                  }
                  ImGui::EndCombo();
                }
                ImGui::PopID();
              }
            }
            ImGui::TextDisabled("a blade no longer cuts armour OFF: it wears");
            ImGui::TextDisabled("through in holes, and leaves with the limb");
          }
          // FIRST, because it is the one that spawns the creature you PICKED
          // rather than a behaviour preset wearing its body. The three below
          // override the sidecar's own `behavior`, which is right when you
          // want a duelist and wrong every other time -- a zombie on the
          // `duelist` profile has no bite in its style list and can only
          // punch.
          if (ImGui::Button("as authored##ai")) s.aiSpawnOwn = true;
          ImGui::SameLine();
          ImGui::TextDisabled("its own JSON behaviour (zombie -> bites)");
          ImGui::TextDisabled("...or override that with a preset:");
          if (ImGui::Button("dummy##ai")) s.aiSpawnDummy = true;
          ImGui::SameLine();
          ImGui::TextDisabled("blind, never moves, never turns");
          if (ImGui::Button("static swordsman##ai")) s.aiSpawnStatic = true;
          ImGui::SameLine();
          ImGui::TextDisabled("turns to face, swings in reach");
          if (ImGui::Button("duelist##ai")) s.aiSpawnDuelist = true;
          ImGui::SameLine();
          ImGui::TextDisabled("paths in, holds range, circles");
          ImGui::Separator();
          if (ImGui::Button("kill all spawned##ai")) s.aiKillSpawned = true;
          ImGui::SameLine();
          if (ImGui::Button("ragdoll all spawned##ai")) s.aiRagdollSpawned = true;
          ImGui::Separator();
          ImGui::Checkbox("debug viz (path / target / band)", &s.showAiDebug);
          ImGui::Checkbox("...include the range-band ring", &s.showAiRing);
          ImGui::Separator();
          ImGui::Text("attack requests: %d", s.aiAttackCount);
          ImGui::TextWrapped("last: %s", s.aiLastAttack.empty()
                                             ? "(none yet)"
                                             : s.aiLastAttack.c_str());
          ImGui::TextDisabled("the AI decides WHEN and WHERE; the stroke");
          ImGui::TextDisabled("program (game/strokes.h) swings them");
          ImGui::Separator();
          ImGui::Text("parries: %d", s.aiBlockCount);
          ImGui::TextWrapped("last: %s", s.aiLastBlock.empty()
                                             ? "(none yet)"
                                             : s.aiLastBlock.c_str());
          ImGui::TextDisabled("blocking is EMERGENT: a blade in the path");
          ImGui::TextDisabled("stops the blow. There is no block button.");
          ImGui::EndTabItem();
        }

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
              static const char* kIntent[6] = {"idle",      "face",
                                               "approach",  "holdRange",
                                               "circle",    "attack"};
              for (int k = 0; k < 6; k++) {
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
    ImGui::SetNextWindowPos(ImVec2(1130, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 720), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Combat", &s.combatWindowOpen)) {
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
            // Two modes exist, so this is a choice and not a slider —
            // handLead's convention below.
            bool freeform = m.controlMode == 1;
            if (ImGui::Checkbox("freeform mouse melee (the A/B)", &freeform)) {
              m.controlMode = freeform ? 1 : 0;
              moved = true;
            }
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "OFF = discrete (default): a click fires an authored strike\n"
                  "from attack_styles.json, direction read from the flick at\n"
                  "the press. ON = the original hold-and-steer mouse melee.\n"
                  "Everything below the input layer is shared.");
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
            f("commit speed (px/s)", &m.commitSpeed, 100.0f, 3000.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Mouse px/s that commits a guard to a cut. Below this\n"
                  "the blade is held; above it the stroke fires.");
            f("direction smoothing (s)", &m.dirSmoothing, 0.005f, 0.4f);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Seconds of mouse history averaged to pick the stroke\n"
                  "direction. Higher = less twitchy, slower to respond.");
            f("reach gain (m/unit)", &m.reachGainM, 0.0f, 0.02f, "%.4f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Metres of tip reach per dReach unit.");
          }
          if (ImGui::CollapsingHeader("Arc", ImGuiTreeNodeFlags_DefaultOpen)) {
            f("swing arc (rad)", &m.swingArc, 0.0f, 3.1f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Radians the committed cut carries the point.");
            f("anticipation", &m.swingAnticipate, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of the arc pulled back before the cut. Higher\n"
                  "= bigger wind-up; 0 = the cut starts where you are.");
            f("mid-stroke bow", &m.swingExtend, 0.0f, 0.6f, "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Fraction of reach the arc bows outward by at mid-\n"
                  "stroke, widening the sweep. 0 = straight-line cut.");
            f("slash time (s)", &m.slashTime, 0.03f, 0.6f);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Seconds the committed slash takes.");
            f("recover time (s)", &m.recoverTime, 0.03f, 0.8f);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Seconds of follow-through after the cut.");
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
            f("aim yaw (deg)", &m.aimYaw, 0.0f, 180.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Degrees the camera may lead the body's facing before\n"
                  "the swing pins at the cone edge. 180 = camera IS the\n"
                  "basis wherever it looks (old behaviour).");
            f("aim release yaw (deg)", &m.aimReleaseYaw, 0.0f, 180.0f, "%.0f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Degrees before straight-behind over which the swing\n"
                  "fades to the body's own forward — so a camera orbited\n"
                  "to the character's face gets a swing that goes the\n"
                  "way the character faces, not at the lens.");
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
            f("infection spread (vox/min)", &g.infectSpreadRate, 0.0f, 20.0f,
              "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How fast the rot GROWS, in world voxels a minute, per\n"
                  "infected limb. It converts healthy tissue next to\n"
                  "itself — soft tissue only, bone stays bone — so it\n"
                  "creeps out from the wound as a front. A limb with no\n"
                  "tissue left crosses a JOINT into the next limb, which\n"
                  "is how a bitten hand eventually reaches the torso.\n"
                  "0 = a bite is a static mark again.");
            f("rot / disintegration (vox/min)", &g.infectRotRate, 0.0f, 20.0f,
              "%.2f");
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "How fast the rot EATS you, in world voxels a minute,\n"
                  "per infected limb. Infected voxels evaporate for good.\n"
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
            ImGui::TextDisabled("assets are PLACEHOLDERS —");
            ImGui::TextDisabled("scripts/gen_combat_sounds.py");
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
