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

bool Overlay::Init(GLFWwindow* window, const rhi::Device& device,
                   rhi::TextureFormat format) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui::GetStyle().Alpha = 0.92f;
  if (!ImGui_ImplGlfw_InitForOther(window, true)) return false;

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
  return ImGui_ImplVulkan_Init(&info);
}

void Overlay::BeginFrame() {
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
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
  // crosshair
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImVec2 c = ImGui::GetIO().DisplaySize;
  c.x *= 0.5f;
  c.y *= 0.5f;
  dl->AddCircleFilled(c, 2.5f, IM_COL32(255, 255, 255, 200));

  if (!s.visible) return;

  ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
  ImGui::Begin("sandvox", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);

  ImGui::Text("%.0f fps  (%.1f ms avg, %.0f ms worst, %.2f ms tick cpu)",
              s.fps, s.frameMs, s.frameMsWorst, s.tickCpuMs);
  ImGui::Text("tick %u   active chunks %u / 4096", s.tick, s.activeChunks);
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
    ImGui::TextDisabled("hold LMB: pour experimental MLS-MPM liquid (compare "
                        "with CA water)");
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
}

void Overlay::Render(const rhi::RenderPass& pass) {
  ImGui::Render();
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), rhi::vkr::NativeCmd(pass));
}

void Overlay::Shutdown() {
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
}
