#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <webgpu/webgpu_cpp.h>

struct GLFWwindow;

// Everything the debug overlay shows/edits. main.cpp owns the values.
struct UIState {
  // stats (read-only in UI)
  float fps = 0;
  float frameMs = 0;
  float tickCpuMs = 0;       // CPU encode+submit per tick
  uint32_t tick = 0;
  uint32_t activeChunks = 0;
  uint64_t voxelTotal = 0;
  uint32_t worldHash = 0;
  float playerPos[3] = {};
  bool mirrorValid = false;

  // controls (edited by UI, applied by main)
  bool paused = false;
  bool stepOnce = false;
  bool shadows = true;
  bool fly = true;
  int brushRadius = 4;
  int brushMaterial = 3;     // sand
  bool reloadShaders = false;
  bool reloadMaterials = false;
  bool regenWorld = false;

  std::vector<std::string> materialNames;  // index == material id
  bool visible = true;
};

class Overlay {
 public:
  bool Init(GLFWwindow* window, const wgpu::Device& device,
            wgpu::TextureFormat format);
  void BeginFrame();
  void Draw(UIState& s);
  void Render(const wgpu::RenderPassEncoder& pass);
  void Shutdown();
};
