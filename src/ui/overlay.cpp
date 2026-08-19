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
  info.DepthStencilFormat = WGPUTextureFormat_Undefined;
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

  ImGui::Text("%.0f fps  (%.2f ms frame, %.2f ms tick cpu)", s.fps, s.frameMs,
              s.tickCpuMs);
  ImGui::Text("tick %u   active chunks %u / 4096", s.tick, s.activeChunks);
  ImGui::Text("voxels %.2f M   hash %08x %s", s.voxelTotal / 1e6, s.worldHash,
              s.mirrorValid ? "" : "(mirror pending)");
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

  if (ImGui::Button("reload shaders (F5)")) s.reloadShaders = true;
  ImGui::SameLine();
  if (ImGui::Button("reload materials (R)")) s.reloadMaterials = true;
  ImGui::SameLine();
  if (ImGui::Button("regen world")) s.regenWorld = true;

  ImGui::TextDisabled("LMB paint  RMB erase  1-8 material  Esc cursor  F1 UI");
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
