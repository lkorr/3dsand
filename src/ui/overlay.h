#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/rhi.h"

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
  // Celestial time multiplier: 1 = normal, 0 = frozen, negative = reverse,
  // 100 = fast-forward. Drives the CelestialClock (sim/world.h), which feeds
  // BOTH the rendered sky and the sim's integer day phase — so cranking it
  // makes the world actually react (water freezes, snow melts) rather than
  // just racing the sun across a world that ignores it.
  //
  // That means a value other than 1 CHANGES THE WORLD HASH, deliberately: it
  // is a dev tool. The clock is disengaged until this first leaves 1.0, so no
  // headless path can observe it and the pinned hash is safe.
  float timeScale = 1.0f;
  // Read back from the celestial solve for the panel readout (render-only).
  float skyDayT = 0.5f;
  float skyYearT = 0.0f;
  float skyMoonPhase = 0.5f, skyMoon2Phase = 0.5f;
  float skySolarEclipse = 0.0f;
  float skySunElevDeg = 0.0f;
  // Collision-box debug overlay (F3). Draws one green oriented wireframe per
  // physics body — avatar and mob limbs, held items, rigidbody debris — using
  // the body's ACTUAL Jolt collider bounds rather than its art, so the two
  // disagreeing is visible rather than inferred. Off by default and free when
  // off (the draw is skipped at zero boxes).
  bool showCollisionBoxes = false;
  int brushRadius = 4;
  int brushMaterial = 3;     // sand
  bool reloadShaders = false;
  bool reloadMaterials = false;
  bool regenWorld = false;
  bool pendingDetonate = false;  // X key / UI button: explode at crosshair
  bool saveWorld = false;        // F9
  bool loadWorld = false;        // F10

  // active tool: LMB drives it; Tab cycles. F/M/B stay as shortcuts.
  // kToolMelee swings whatever the hotbar has equipped (game/melee.h): it is a
  // tool rather than a mode because LMB already routes per-tool, so the sword
  // gets the attack button without taking it from the brush.
  enum Tool {
    kToolBrush = 0, kToolLaser, kToolPrefab, kToolMob, kToolMelee, kToolFluid,
    kToolCount
  };
  int tool = kToolBrush;

  // MLS-MPM fluid prototype (docs/PLAN_mpm_fluids.md): the experimental
  // particle liquid, placeable side by side with CA water for comparison.
  // fluidCount is mirrored from the main loop's CPU-owned count for the HUD;
  // clearFluid is a one-shot request consumed inside the tick loop.
  uint32_t fluidCount = 0;
  bool clearFluid = false;
  // Live tuning copies — main.cpp seeds these from CurrentTuning().sim on
  // startup.  The overlay draws sliders; main.cpp detects changes (via
  // fluidTuningDirty) and writes them back + reloads shaders.
  float fGravity = 98.1f;
  float fStiffness = 5400.0f;
  int   fEosPower = 4;
  float fCohesion = 90.0f;
  float fAttractSame = 45.0f;
  float fAttractDiff = -90.0f;
  float fViscosity = 1.5f;
  float fDamping = 0.0f;
  int   fExciteMode = 0;
  bool  fluidTuningDirty = false;

  // hotbar (game/item.h): mirrored out of Inventory each frame for the HUD.
  // The overlay never owns inventory state — it only draws it.
  std::vector<std::string> itemNames;   // per slot, "" = empty
  int itemSelected = 0;
  const char* swingPhase = "";          // melee state, for the HUD readout
  float swingSpeed = 0;                 // mouse speed driving the swing

  // Ledge-grab readout (dev panel). main.cpp composes the text from the
  // player's per-frame probe so a refused grab says WHICH latch gate refused;
  // state drives the colour: 0 = no lip in reach, 1 = in reach, 2 = hanging.
  int ledgeState = 0;
  std::string ledgeText;

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
  // Authored ceiling for the HUD bar's denominator. Health does NOT regenerate,
  // so this is only ever a high-water mark the player moves away from.
  int32_t healthMax = 0;
  bool playerAlive = true;

  // ---- body condition, drawn as a stick figure above the health bar -------
  // The hp bar answers "how much life is left"; this answers "what part of me
  // is broken", which is a different question the moment a limb can be severed
  // independently of the total. One entry per BodySlot below, mirrored out of
  // PlayerAvatar each frame — the overlay never reaches into the avatar itself.
  //
  // Laid out by limb TAG + side rather than by mina's part names, so any
  // humanoid rig fills the same figure and a rig missing a part simply leaves
  // that segment absent.
  enum BodySlot {
    kSlotHead = 0, kSlotTorso, kSlotHips,
    kSlotArmUL, kSlotArmLL, kSlotHandL,
    kSlotArmUR, kSlotArmLR, kSlotHandR,
    kSlotLegUL, kSlotLegLL, kSlotFootL,
    kSlotLegUR, kSlotLegLR, kSlotFootR,
    kSlotCount
  };
  struct BodyPartUI {
    bool present = false;   // rig HAS this part (false = never draw it)
    bool severed = false;   // lost — drawn as a stump, not as a damaged limb
    bool bleeding = false;  // actively losing blood — flashes
    float hpFrac = 1.0f;    // 1 = untouched, 0 = destroyed; drives the tint
  };
  BodyPartUI body[kSlotCount];
  bool bodyValid = false;   // false until the avatar has spawned
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
  bool Init(GLFWwindow* window, const rhi::Device& device,
            rhi::TextureFormat format);
  void BeginFrame();
  // The player-facing HUD: health + mana in the bottom-left corner. Separate
  // from Draw() and drawn unconditionally, because the dev panel is F1-hideable
  // and the HUD must not be.
  void DrawHUD(const UIState& s);
  void Draw(UIState& s);

 private:
  // The per-limb body readout that sits above the hp bar. Draws upward from
  // `yBottom` and returns the height it reserved, so DrawHUD stacks the "DEAD"
  // banner above it without duplicating the figure's proportions.
  float DrawBodyFigure(const UIState& s, float x, float yBottom);

 public:
  void Render(const rhi::RenderPass& pass);
  void Shutdown();
};
