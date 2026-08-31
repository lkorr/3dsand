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
  // Tail percentiles over a LONGER window than the average/worst (512 frames,
  // ~5-11 s). `worst` is a single sample and moves on any one-off hiccup;
  // these say whether a stutter is systematic. See the sampling comment in
  // main.cpp for why the window cannot be the same 0.5 s the others use.
  float frameMsP95 = 0;
  float frameMsP99 = 0;
  float tickCpuMs = 0;       // CPU encode+submit per tick
  uint32_t tick = 0;
  uint32_t activeChunks = 0;
  // The denominator was the literal 4096 in the format string, and the window
  // has been 32^3 = 32,768 chunks for a while. Carried as a field because this
  // file sees only its own header and ImGui — a fresh literal would just be the
  // same bug again, one window resize later.
  uint32_t totalChunks = 0;
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
  bool showDirtyChunks = false;
  bool showDirtyVoxels = false;
  // Wind slope-field overlay (F4). An arrow per lattice point around the
  // camera, oriented and coloured by the SAME windAt() the grass sway samples
  // (docs/RESEARCH_wind.md §4.8) — which is what makes it evidence rather than
  // decoration. Seeded from wind.dbgWindField on startup and on every tuning
  // reload, so it is reachable from a saved tuning.json and from headless
  // screenshot runs, neither of which can press a key. Free when off: the draw
  // is skipped outright.
  bool showWindField = false;
  // ---- dev wind force multipliers, one per TIER ----
  // Mirrors sim.windGasScale / sim.windPartScale. They ride TickParams as Q8
  // integers rather than being const-folded into the shaders, which is what
  // makes them draggable: moving one takes effect on the NEXT TICK, with no
  // shader reload and no rebuild.
  //
  // They scale different quantities on purpose (see the long note in tuning.h):
  // gas scales the CA drift-bias PROBABILITY past its cap to certainty,
  // because scaling the velocity there dies at ~2x; particle scales the wind
  // VELOCITY that debris, spray and MPM nodes chase, because that is the only
  // way past the drag law's own ceiling.
  //
  // Both are determinism-critical: at exactly 1.0 the sim is bit-identical to
  // the pinned hash, and off 1.0 it is a different but equally deterministic
  // world. `windTuningDirty` is a SEPARATE latch from fluidTuningDirty
  // precisely so this path does not drag a shader reload along with it.
  float windGasScale = 1.0f;
  float windPartScale = 1.0f;
  // Mirrors sim.windDragRef, on the same latch and the same stream. Not a
  // multiplier: the wind speed (m/s) at which the drag rate reaches its
  // authored strength, i.e. how hard a wind it takes before falling debris
  // notices the air at all.
  float windDragRef = 40.0f;
  bool windTuningDirty = false;
  // ---- placing a wind PRIMITIVE by hand (docs/RESEARCH_wind.md §4.3) ------
  // The dev-panel producer, and the reason it exists is that the gameplay
  // producers are content: a fan is a spell glyph or a prefab tag, and neither
  // is a good way to answer "what does a 30 m/s cone actually do to that
  // dune?". These three are the request; main.cpp turns them into a WindPrim
  // and puts it on the same stream a spell uses, so nothing here is a side
  // channel into the wind system.
  //
  // The fan is anchored where the camera is LOOKING and aimed along the view
  // ray, which is the only placement that needs no extra UI and is also what a
  // player-facing "place object" would do.
  bool placeWindFan = false;      // one-shot: consumed by the frame loop
  bool clearWindFans = false;     // one-shot: retire every dev-placed fan
  float windFanSpeed = 25.0f;     // m/s at the core
  int windFanRadius = 8;          // world cells across
  int windFanReach = 48;          // world cells along the axis
  int windFanKind = 0;            // 0 cone, 1 burst, 2 vortex
  bool windFanEntrain = true;     // may it pull SETTLED powder loose
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
  int fluidSpecies = 0;
  // Live tuning copies — main.cpp seeds these from CurrentTuning() on
  // startup.  The overlay draws sliders; main.cpp detects changes (via
  // fluidTuningDirty) and writes them back + reloads shaders.
  bool fluidWindowOpen = false;
  // ---- sim tab ----
  float fGravity = 98.1f;
  float fStiffness = 5400.0f;
  float fRestDensity = 8.0f;
  int   fEosPower = 4;
  float fCohesion = 90.0f;
  float fAttractSame = 45.0f;
  float fAttractDiff = -90.0f;
  float fViscosity = 1.5f;
  float fDamping = 0.0f;
  float fSplashRate = 4.0f;
  float fSplashSpeed = 18.0f;
  float fSplashMaxDensity = 0.7f;
  float fSplashLife = 1.1f;
  int   fSplashScaleIdx = 2;
  float fFoamRate = 90.0f;
  float fFoamCrestRate = 120.0f;
  float fTrappedMin = 1.5f;
  float fTrappedMax = 11.0f;
  float fCrestMin = 0.25f;
  float fCrestMax = 2.0f;
  float fFoamEnergyMin = 8.0f;
  float fFoamEnergyMax = 260.0f;
  float fFoamLife = 2.2f;
  float fFoamLifeMin = 0.5f;
  float fBubbleBuoyancy = 1.6f;
  float fFoamDrag = 0.72f;
  float fBubbleDensity = 1.05f;
  float fSprayDensity = 0.42f;
  int   fFoamScaleIdx = 3;
  int   fExciteMode = 0;
  float fSettleEps = 0.9f;
  float fWakeSpeed = 3.6f;
  int   fSettleTicks = 45;
  float fStainRate = 8.0f;
  // ---- look tab (render) ----
  float fSurface = 1.0f;
  float fColor[3] = {0.20f, 0.42f, 0.85f};
  float fColor1[3] = {0.92f, 0.34f, 0.10f};
  float fColor2[3] = {0.22f, 0.78f, 0.28f};
  float fColor3[3] = {0.88f, 0.72f, 0.25f};
  float fIso = 0.30f;
  float fSmooth = 1.3f;
  float fIor = 1.33f;
  float fClarity = 1.3f;
  float fReflect = 1.0f;
  float fSpecular = 1.0f;
  float fShallow[3] = {0.42f, 0.86f, 0.82f};
  float fDeep[3] = {0.02f, 0.15f, 0.42f};
  float fDepth = 2.6f;
  float fGradientStr = 1.0f;
  float fRFoam = 0.55f;
  float fRFoamField = 1.0f;
  float fRFoamTexture = 0.65f;
  float fRFoamSpeed = 22.0f;
  float fWobble = 0.5f;
  float fParticleSize = 0.58f;
  float fStretch = 0.4f;
  float fDensityShade = 0.45f;
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

  // ---- NPC AI panel (game/ai_behavior.h) ----------------------------------
  //
  // Shaped exactly like the MPM fluid panel next door: mirror fields the
  // overlay draws, one-shot bools the frame loop consumes, and a dirty latch
  // for the sliders. The overlay never reaches into MobSystem — main.cpp
  // mirrors the live creature list in and applies the requests out, which is
  // what keeps this a producer on the same path gameplay uses rather than a
  // dev-only side channel into the AI.
  //
  // NO CUSTOM SCROLL CALLBACK. ImGui installs its own via
  // ImGui_ImplGlfw_InitForOther(window, true) inside Overlay::Init, and its
  // handler chains BACKWARD to whatever was installed before it — so a
  // callback registered AFTER Init silently replaces ImGui's and freezes the
  // wheel in every scrollable panel. Nothing in this engine installs one, this
  // panel does not need one (an ImGui child region scrolls by itself), and
  // that is the correct amount.
  bool aiWindowOpen = false;
  bool aiSpawnDummy = false;      // one-shot: spawn ahead of the crosshair
  bool aiSpawnStatic = false;
  bool aiSpawnDuelist = false;
  bool aiKillSpawned = false;     // one-shot: despawn everything this panel made
  bool aiSaveBehaviors = false;   // one-shot: write assets/mobs/behaviors.json
  bool aiApplyBehavior = false;   // one-shot: aiBehaviorPick -> the selected mob
  bool showAiDebug = false;       // in-world path / target / band viz
  bool showAiRing = true;         // ...the range-band ring specifically
  std::string aiSaveStatus;       // last save result, shown next to the button
  // Live creatures, mirrored per frame. Parallel arrays rather than a struct
  // because UIState is a POD the overlay may only read — the same shape
  // mobNames/materialNames already have.
  std::vector<uint64_t> aiMobIds;
  std::vector<std::string> aiMobLabels;   // "#3 mina [duelist] approach d=14.2"
  int aiMobSelected = 0;
  int aiBehaviorPick = 0;                 // index into aiProfileNames
  std::vector<std::string> aiProfileNames;
  // The profile whose sliders are on screen, and the values themselves. Written
  // THROUGH to the in-memory profile by main.cpp when aiTuningDirty latches, so
  // every mob on that profile updates at once (there is deliberately no
  // per-mob override — see MobSystem::BehaviorsMut).
  int aiProfileEdit = 0;
  bool aiTuningDirty = false;
  bool aiProfileReseat = true;    // reload the mirrors below from the library
  float aiSightRange = 0, aiFovDegrees = 360, aiKeepRangeScale = 1.4f;
  int aiAlertDecayTicks = 90;
  bool aiRequireLos = true;
  bool aiMobile = false;
  float aiRangeMin = 0, aiRangeMax = 0, aiBandSlack = 1.5f;
  float aiApproachSpeed = 1, aiStrafeSpeed = 0.55f, aiRetreatSpeed = 0.8f;
  float aiCircleTendency = 0;
  int aiCircleHoldTicks = 24, aiRepathTicks = 12;
  float aiNavRadius = 22;
  float aiAttackReach = 8, aiAimTolerance = 0.45f;
  int aiCadenceTicks = 40, aiJitterTicks = 18, aiCommitTicks = 10,
      aiDisengageTicks = 22;
  float aiHysteresis = 0.22f;
  float aiIntentWeight[6] = {};        // one per ai::Intent, in enum order
  int aiIntentCooldown[6] = {};
  int aiIntentDwell[6] = {};
  // Last attack request drained from the seam, for the readout. Phase C
  // replaces this consumer with a real stroke; until then, SEEING the requests
  // is what proves the seam fires at the right moments.
  std::string aiLastAttack;
  int aiAttackCount = 0;

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
  // ---- wind primitives (docs/RESEARCH_wind.md §4.3) ----
  // Fans, spell gusts and tornadoes currently alive, and the ones the world
  // list (32) had no room for. The refusal is SHOWN rather than swallowed for
  // the spellOpsDropped reason: "my gust sometimes does nothing" is miserable
  // to diagnose from silence, and the cap is a rule-2 budget rather than a bug.
  int windPrims = 0;
  int windPrimsDropped = 0;
  int windWakeChunks = 0;   // chunks the primitives woke last tick
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
