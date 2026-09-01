#include "ui/overlay.h"

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
  auto bar = [&](float y, int32_t cur, int32_t max, int32_t pending,
                 ImU32 fill, ImU32 spend, const char* label) {
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
      IM_COL32(255, 140, 60, 245), "hp");
  bar(yMana, s.mana, s.manaMax, fromMana, IM_COL32(70, 120, 230, 235),
      IM_COL32(150, 200, 255, 245), "mp");

  // ---- body condition, sitting directly above the hp bar -------------------
  const float figureH = DrawBodyFigure(s, x, yHealth - gap);
  const float yTop = yHealth - gap - figureH;

  if (!s.playerAlive) {
    const char* dead = "DEAD";
    const ImVec2 ts = ImGui::CalcTextSize(dead);
    const ImVec2 tp(x, yTop - ts.y - 6);
    d->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 190), dead);
    d->AddText(tp, IM_COL32(255, 70, 60, 255), dead);
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
  ImGui::Text("magic %s   (M toggles)", s.magicMode ? "ON" : "off");
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
  ImGui::Text("mana %d/%d   health %d   cost %d", s.mana, s.manaMax, s.health,
              s.spellCost);
  if (s.spellCost > s.mana + s.health) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                       "FATAL - this will kill you");
  } else if (s.spellCost > s.mana) {
    ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f),
                       "unstable - %d from your body", s.spellCost - s.mana);
  }
  if (!s.spellText.empty()) {
    ImGui::Text("speaking: %s", s.spellText.c_str());
    ImGui::TextDisabled("%s", s.spellVerdict.c_str());
  } else {
    ImGui::TextDisabled("speaking: (nothing)   RMB casts, C clears");
  }
  if (!s.glyphSlots.empty()) {
    std::string strip;
    for (size_t i = 0; i < s.glyphSlots.size(); i++) {
      if (s.glyphSlots[i].empty()) continue;
      strip += std::to_string((i + 1) % 10) + ":" + s.glyphSlots[i] + "  ";
    }
    ImGui::TextDisabled("%s", strip.c_str());
  }
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
  }
  ImGui::Separator();

  if (ImGui::Button(s.paused ? "resume (P)" : "pause (P)")) s.paused = !s.paused;
  ImGui::SameLine();
  if (ImGui::Button("step (N)")) s.stepOnce = true;
  ImGui::SameLine();
  ImGui::Checkbox("shadows", &s.shadows);

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
  ImGui::Checkbox("wind field (F4)", &s.showWindField);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "The ambient wind, drawn as an arrow per lattice point around you.\n"
        "Colour is speed, cool to hot. It samples the SAME windAt() the grass\n"
        "sway does, so what the arrows show is what the foliage is standing\n"
        "in - turn the Wind tab's direction knob and both must swing together.\n"
        "Arrows pointing at or away from you fade out: one aimed down the view\n"
        "ray cannot show its direction anyway, so the hole is honest.\n"
        "Spacing and radius are on the Wind tab (F5 to apply).\n"
        "NOTE: F5 re-seeds this from wind.dbgWindField, so a reload turns it\n"
        "back off unless the tuning file asks for it.");

  // ---- wind force multipliers, one per tier -------------------------------
  // Live: these ride TickParams, so a drag lands on the next tick with no
  // shader reload. Split by TIER because that is how the engine is split
  // (research doc §4.6) — the CA steers what is already moving, the particle
  // system carries the violence — and because they are the two things you want
  // to A/B against each other. Pinning one to 0 while pushing the other is the
  // fastest way to see which tier a given effect is actually coming from.
  if (ImGui::SliderFloat("wind x voxels", &s.windGasScale, 0.0f, 16.0f, "%.2fx"))
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
  if (ImGui::SliderFloat("wind x particles", &s.windPartScale, 0.0f, 16.0f, "%.2fx"))
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
  if (ImGui::SliderFloat("wind fall onset", &s.windDragRef, 1.0f, 120.0f,
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
    ImGui::SliderFloat("speed", &s.windFanSpeed, -40.0f, 40.0f, "%.0f m/s");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "Core speed at the mouth. NEGATIVE is legal and useful: it turns a\n"
          "burst into a vacuum and a cone into a draw.");
    ImGui::SliderInt("radius", &s.windFanRadius, 1, 64);
    ImGui::SliderInt("reach", &s.windFanReach, 1, 128);
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

  if (s.tool == UIState::kToolMelee) {
    ImGui::TextDisabled("hold LMB to guard, then FLICK the mouse to cut");
  }
  if (s.tool == UIState::kToolFluid) {
    ImGui::TextDisabled("hold LMB: pour  1-4 species  U clear");
    ImGui::Text("mpm particles: %u / 262144", s.fluidCount);
    ImGui::SameLine();
    if (ImGui::Button("clear (U)")) s.clearFluid = true;
    if (ImGui::Button("fluid tuning..."))
      s.fluidWindowOpen = !s.fluidWindowOpen;
  }
  if (s.tool == UIState::kToolMob) {
    if (ImGui::Button("NPC AI...")) s.aiWindowOpen = !s.aiWindowOpen;
    ImGui::SameLine();
    ImGui::TextDisabled("%d live", (int)s.aiMobIds.size());
  }
  // Off the MELEE tool, which is the one context where every knob in the panel
  // is about what you are currently doing. Deliberately not off the mob tool
  // beside the AI button: the two panels are used together, and a fight is
  // tuned from the weapon's end.
  if (s.tool == UIState::kToolMelee) {
    if (ImGui::Button("Combat...")) s.combatWindowOpen = !s.combatWindowOpen;
    ImGui::SameLine();
    if (s.hitStopScale < 0.999f)
      ImGui::TextDisabled("HIT-STOP %.2fx", s.hitStopScale);
    else
      ImGui::TextDisabled("stroke / gore / feel");
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
  if (ImGui::Button("regen world")) s.regenWorld = true;

  if (ImGui::Button("save world (F9)")) s.saveWorld = true;
  ImGui::SameLine();
  if (ImGui::Button("load world (F10)")) s.loadWorld = true;
  ImGui::SameLine();

  if (ImGui::Button("detonate at crosshair (X)")) s.pendingDetonate = true;

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
        ImGui::SliderFloat(label, v, lo, hi, "%.2f");
      };
      auto islider = [](const char* label, int* v, int lo, int hi) {
        ImGui::SliderInt(label, v, lo, hi);
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
          ImGui::TextDisabled("spawns a humanoid with a sword, a few metres");
          ImGui::TextDisabled("ahead of you (or at the crosshair hit)");
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
              if (ImGui::SliderFloat(label, v, lo, hi, "%.2f"))
                s.aiTuningDirty = true;
            };
            auto i32 = [&s](const char* label, int* v, int lo, int hi) {
              if (ImGui::SliderInt(label, v, lo, hi)) s.aiTuningDirty = true;
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
                if (ImGui::SliderFloat("weight", &s.aiIntentWeight[k], 0.0f,
                                       4.0f, "%.2f"))
                  s.aiTuningDirty = true;
                if (ImGui::SliderInt("cooldown", &s.aiIntentCooldown[k], 0, 180))
                  s.aiTuningDirty = true;
                if (ImGui::SliderInt("min dwell", &s.aiIntentDwell[k], 0, 180))
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
        if (ImGui::SliderFloat(label, v, lo, hi, fmt)) moved = true;
      };
      auto i32 = [&moved](const char* label, int* v, int lo, int hi) {
        if (ImGui::SliderInt(label, v, lo, hi)) moved = true;
      };
      auto b = [&moved](const char* label, bool* v) {
        if (ImGui::Checkbox(label, v)) moved = true;
      };

      if (ImGui::Button("Save")) s.combatSave = true;
      ImGui::SameLine();
      ImGui::TextDisabled("patches melee/combatfx/gore in tuning.json");
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
          if (ImGui::CollapsingHeader("Aim", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("radians of tip travel per mouse pixel");
            f("aim gain x", &m.aimGainX, 0.0005f, 0.02f, "%.4f");
            f("aim gain y", &m.aimGainY, 0.0005f, 0.02f, "%.4f");
            f("commit speed (px/s)", &m.commitSpeed, 100.0f, 3000.0f, "%.0f");
            f("direction smoothing (s)", &m.dirSmoothing, 0.005f, 0.4f);
            f("reach gain (m/unit)", &m.reachGainM, 0.0f, 0.02f, "%.4f");
          }
          if (ImGui::CollapsingHeader("Arc", ImGuiTreeNodeFlags_DefaultOpen)) {
            f("swing arc (rad)", &m.swingArc, 0.0f, 3.1f, "%.2f");
            f("anticipation", &m.swingAnticipate, 0.0f, 1.0f, "%.2f");
            f("mid-stroke bow", &m.swingExtend, 0.0f, 0.6f, "%.2f");
            f("slash time (s)", &m.slashTime, 0.03f, 0.6f);
            f("recover time (s)", &m.recoverTime, 0.03f, 0.8f);
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
            f("elevation min (rad)", &m.elMin, -1.55f, -0.1f, "%.2f");
            f("elevation max (rad)", &m.elMax, 0.1f, 1.55f, "%.2f");
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
            f("extension smoothing (s)", &m.extendSmoothing, 0.005f, 1.0f);
            f("reach fraction", &m.reachFraction, 0.2f, 0.99f, "%.2f");
            f("lean turn rate (rad/s)", &m.leanTurnRate, 0.5f, 60.0f, "%.1f");
            f("blade smoothing (s)", &m.bladeSmoothing, 0.005f, 0.4f);
            {
              // Only the SIGN is read (melee.h), so this is a choice and not a
              // slider. A slider here would let two knobs disagree about where
              // the hand is.
              bool handLeads = m.handLead >= 0.0f;
              if (ImGui::Checkbox("hand leads the point (sabre cut)",
                                  &handLeads)) {
                m.handLead = handLeads ? 1.0f : -1.0f;
                moved = true;
              }
            }
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
            f("nothing below (m/s)", &t.melee.minSpeedMps, 0.0f, 20.0f, "%.2f");
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
            f("guard beaten · az (rad)", &t.melee.blockNudgeAz, 0.0f, 1.0f,
              "%.2f");
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
            f("bite from speed (vox)", &g.cutDepthPower, 0.0f, 4.0f, "%.2f");
            f("cut length (vox)", &g.cutLength, 0.1f, 8.0f, "%.2f");
            f("cut width x blade", &g.cutWidth, 0.05f, 2.0f, "%.2f");
            i32("spall rounds", &g.cutSpallRounds, 0, 4);
            f("spall strength", &g.cutSpallStrength, 0.0f, 1.0f, "%.2f");
          }
          if (ImGui::CollapsingHeader("When a limb comes off")) {
            f("sever fraction", &g.woundSeverFraction, 0.05f, 0.95f, "%.2f");
            f("neck radius (vox)", &g.woundNeckRadius, 0.0f, 8.0f, "%.2f");
            f("neck fraction", &g.woundNeckFraction, 0.0f, 0.95f, "%.2f");
            f("impact-sever scale", &g.woundImpactSeverScale, 0.0f, 16.0f, "%.1f");
          }
          if (ImGui::CollapsingHeader("Heft (how much weapon)")) {
            f("reference volume", &g.woundHeftRef, 0.01f, 40.0f, "%.2f");
            f("heft ceiling", &g.woundHeftMax, 1.0f, 32.0f, "%.1f");
          }
          if (ImGui::CollapsingHeader("Blood")) {
            f("stain radius (vox)", &g.woundStainRadius, 0.0f, 8.0f, "%.2f");
            f("stain density", &g.woundStainDensity, 0.0f, 1.0f, "%.2f");
            f("bleed gain (per mob)", &g.bleedGain, 0.0f, 8.0f, "%.2f");
            ImGui::TextDisabled("the rest of gore.* is in the browser tuner");
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
            f("chip length (ms)", &fx.hitStopChipMs, 0.0f, 400.0f, "%.0f");
            ImGui::TextDisabled("flesh: a live creature was hurt");
            f("flesh speed", &fx.hitStopFleshScale, 0.02f, 1.0f, "%.2fx");
            f("flesh length (ms)", &fx.hitStopFleshMs, 0.0f, 400.0f, "%.0f");
            ImGui::TextDisabled("sever: a limb came off");
            f("sever speed", &fx.hitStopSeverScale, 0.02f, 1.0f, "%.2fx");
            f("sever length (ms)", &fx.hitStopSeverMs, 0.0f, 400.0f, "%.0f");
          }
          if (ImGui::CollapsingHeader("Hit flash",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("additive, linear HDR, before the tonemap");
            f("chip flash", &fx.flashChip, 0.0f, 4.0f, "%.2f");
            f("flesh flash", &fx.flashFlesh, 0.0f, 4.0f, "%.2f");
            f("sever flash", &fx.flashSever, 0.0f, 4.0f, "%.2f");
            f("halflife (s)", &fx.flashHalflife, 0.01f, 0.6f);
          }
          if (ImGui::CollapsingHeader("Sound",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            f("whoosh volume", &fx.whooshVolume, 0.0f, 3.0f, "%.2f");
            f("whoosh silent below (px/s)", &fx.whooshMinSpeed, 0.0f, 2000.0f,
              "%.0f");
            f("whoosh pitch, slow", &fx.whooshRateSlow, 0.4f, 2.0f, "%.2f");
            f("whoosh pitch, fast", &fx.whooshRateFast, 0.4f, 2.0f, "%.2f");
            f("flesh impact volume", &fx.fleshVolume, 0.0f, 3.0f, "%.2f");
            f("blade clang volume", &fx.clangVolume, 0.0f, 3.0f, "%.2f");
            f("audible radius (m)", &fx.cueRadius, 1.0f, 120.0f, "%.0f");
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
