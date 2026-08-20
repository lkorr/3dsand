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
  float frameMs = 0;         // window average
  float frameMsWorst = 0;    // worst frame in the window (hitch visibility)
  float tickCpuMs = 0;       // CPU encode+submit per tick
  uint32_t tick = 0;
  uint32_t activeChunks = 0;
  uint64_t voxelTotal = 0;
  uint32_t worldHash = 0;
  uint32_t particleCount = 0;
  uint32_t bodyCount = 0;
  uint32_t activeBodyCount = 0;
  uint32_t mobCount = 0;
  bool spawnMob = false;         // M key: spawn mob def 0 at crosshair
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
  bool pendingDetonate = false;  // X key / UI button: explode at crosshair
  bool saveWorld = false;        // F9
  bool loadWorld = false;        // F10

  // active tool: LMB drives it; Tab cycles. F/M/B stay as shortcuts.
  enum Tool { kToolBrush = 0, kToolLaser, kToolPrefab, kToolMob, kToolCount };
  int tool = kToolBrush;

  // prefab placement tool (PLAN §A3)
  int prefabSelected = 0;        // index into prefabNames (O cycles)
  int prefabRot = 0;             // 90° Y steps (T rotates)
  bool prefabOverwrite = false;  // false = fill air only
  bool placePrefab = false;      // B key / LMB click / UI button
  uint32_t prefabPending = 0;    // voxels still draining (stat)
  std::vector<std::string> prefabNames;

  // mob spawner tool
  int mobSelected = 0;           // index into mobNames
  std::vector<std::string> mobNames;

  std::vector<std::string> materialNames;  // index == material id
  std::vector<uint32_t> materialColors;    // 0xAABBGGRR swatch (gpu color0)
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
