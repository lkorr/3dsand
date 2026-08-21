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
  bool spawnSphere = false;      // K key / UI button: rolling sphere of the
                                 // current brush material at the crosshair
  float playerPos[3] = {};
  bool mirrorValid = false;

  // what the crosshair is over, from the sim_pick readback (one tick latent).
  // hoverMat == 0 means the ray left the residency window without hitting
  // anything, so there is nothing to name.
  int hoverMat = 0;
  int hoverCell[3] = {};
  float hoverDist = 0;  // metres from the eye to the hit cell centre

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

  // ---- magic (game/spell.h, game/caster.h) --------------------------------
  // The crossover readout — where the running cost stops coming out of mana
  // and starts coming out of health — IS the tension mechanic, so it gets a
  // real visual break rather than a number.
  bool magicMode = false;         // number row speaks glyphs instead of picking
                                  // a brush material
  int32_t mana = 0, manaMax = 0;  // manaMax is the ward-adjusted EFFECTIVE max
  int32_t health = 0;
  int32_t spellCost = 0;          // running cost of the spoken sequence
  std::string spellText;          // "lava + trail + projectile"
  std::string spellVerdict;       // what the VM thinks it is
  int spellOutcome = -1;          // last CastOutcome, -1 = none yet
  int liveProjectiles = 0;
  int spellOpsDropped = 0;        // ops the reservation could not fit (§F)
  // slot -> glyph id, for the bound-key strip. Empty string = unbound slot.
  std::vector<std::string> glyphSlots;
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
