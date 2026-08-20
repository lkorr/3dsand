#include "ui/overlay.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_wgpu.h>

bool Overlay::Init(GLFWwindow* window, const wgpu::Device& device,
                   wgpu::TextureFormat format) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui::GetStyle().Alpha = 0.92f;
  if (!ImGui_ImplGlfw_InitForOther(window, true)) return false;
  ImGui_ImplWGPU_InitInfo info{};
  info.Device = device.Get();
  info.NumFramesInFlight = 3;
  info.RenderTargetFormat = (WGPUTextureFormat)format;
  // must match Simulation::kDepthFormat — the overlay draws into the same
  // render pass as the raymarch + debris pipelines
  info.DepthStencilFormat = WGPUTextureFormat_Depth32Float;
  return ImGui_ImplWGPU_Init(&info);
}

void Overlay::BeginFrame() {
  ImGui_ImplWGPU_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
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
  ImGui::Separator();

  if (ImGui::Button(s.paused ? "resume (P)" : "pause (P)")) s.paused = !s.paused;
  ImGui::SameLine();
  if (ImGui::Button("step (N)")) s.stepOnce = true;
  ImGui::SameLine();
  ImGui::Checkbox("shadows", &s.shadows);

  ImGui::Checkbox("fly (V)", &s.fly);
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

  if (s.tool == UIState::kToolBrush) {
    ImGui::TextDisabled("LMB paint  RMB erase  1-8 / combo below");
  } else if (s.tool == UIState::kToolLaser) {
    ImGui::TextDisabled("hold LMB (or F): melts what it hits, cuts bodies");
  } else if (s.tool == UIState::kToolPrefab) {
    if (!s.prefabNames.empty()) {
      if (s.prefabSelected >= (int)s.prefabNames.size()) s.prefabSelected = 0;
      if (ImGui::BeginCombo("prefab", s.prefabNames[s.prefabSelected].c_str())) {
        for (int i = 0; i < (int)s.prefabNames.size(); i++)
          if (ImGui::Selectable(s.prefabNames[i].c_str(), i == s.prefabSelected))
            s.prefabSelected = i;
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
      if (ImGui::BeginCombo("mob", s.mobNames[s.mobSelected].c_str())) {
        for (int i = 0; i < (int)s.mobNames.size(); i++)
          if (ImGui::Selectable(s.mobNames[i].c_str(), i == s.mobSelected))
            s.mobSelected = i;
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

void Overlay::Render(const wgpu::RenderPassEncoder& pass) {
  ImGui::Render();
  ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.Get());
}

void Overlay::Shutdown() {
  ImGui_ImplWGPU_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
}
