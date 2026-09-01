#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/kitref.h"   // KitRef: the one slot address the screen drags in
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
  // The authored strike a discrete click resolved to (its label), "" outside
  // discrete mode or between swings. Same falsifiability job as swingPhase:
  // the player must be able to tell "the game misread my flick" from "I
  // misjudged the cut" — main.cpp fills it, the overlay only draws it.
  std::string swingStyle;

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
  // ---- the Combat panel (melee.* / combatfx.* / gore.* in tuning.json) -----
  //
  // FIVE FIELDS, NOT FIFTY, AND THE DEVIATION IS DELIBERATE. Every other panel
  // here mirrors each knob into a UIState float and lets main.cpp write it
  // through, because that keeps the overlay from owning game state. The Combat
  // panel instead edits the tuning SINGLETON in place (overlay.cpp reads
  // CurrentTuning(), runs the sliders on a copy, and SetCurrentTuning()s it
  // back), for three reasons that all point the same way:
  //
  //   * it is ~60 knobs across three groups. Sixty mirrors is sixty chances
  //     for a name to drift from the tuning field it shadows, with nothing
  //     checking the correspondence — the failure mode being a slider that
  //     silently edits nothing.
  //   * a mirror needs a RESEAT path (the AI panel's aiProfileReseat) so F5
  //     and the browser tuner do not leave the sliders showing stale numbers.
  //     Reading the live tuning every frame has no stale state to reseat.
  //   * `CurrentTuning`/`SetCurrentTuning` are a process-global that tuning.h
  //     exports on purpose and that eight other systems already read straight
  //     from. It is not main.cpp's private state in the way MobSystem is.
  //
  // What still crosses through main.cpp is everything that is NOT a tuning
  // value: the dirty latch (MeleeState caches its MeleeTuning by value, so it
  // has to be told), the save request, and the readout below.
  bool combatWindowOpen = false;
  bool combatTuningDirty = false;  // a slider moved: re-apply to MeleeState
  bool combatSave = false;         // one-shot: write melee.*/combatfx.* to JSON
  std::string combatSaveStatus;    // last save result, shown by the button
  // Live hit-stop multiplier, for the panel's readout. 1 = running normally.
  // A READOUT and not a control: the panel exists partly so "is hit-stop
  // firing at all" is answerable without a frame counter, since a 60 ms dip is
  // exactly the sort of thing you cannot tell you are seeing.
  float hitStopScale = 1.0f;

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
  // Last attack request drained from the seam, and the last PARRY. Both are
  // readouts rather than mechanisms: seeing the requests is what proves the AI
  // seam still fires at the right moments, and seeing the blocks is what
  // proves emergent blocking is happening at all — a defender's blade in the
  // way makes no sound yet (phase D), so without this line a parry and a miss
  // look identical.
  std::string aiLastAttack;
  int aiAttackCount = 0;
  std::string aiLastBlock;
  int aiBlockCount = 0;

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
    // ---- the INSPECTOR's extra columns (ui/inventory_ui.cpp) --------------
    // The stick figure needs only hpFrac; the character screen's health view
    // reports what actually happened to the limb, which hp alone cannot say.
    //
    // voxelFrac is the load-bearing one: a laser can bore an arm hollow
    // without driving its hp to zero, and a blast can shave hp off a limb that
    // has lost no geometry at all, so "how hurt" and "how much is left" are
    // genuinely two measurements. charredFrac and burningVoxels come from
    // MATERIAL IDENTITY on the limb's own voxels (skin -> cooked -> burning ->
    // charred -> ash), which is why burn damage needs no new engine state at
    // all — the answer was already in the voxels.
    float voxelFrac = 1.0f;      // live voxels / voxels at spawn
    float charredFrac = 0.0f;    // share of the limb cooked/charred through
    uint32_t burningVoxels = 0;  // voxels alight RIGHT NOW
    float hp = 0, hpMax = 0;     // absolute, for the numeric readout
    // WHERE THE LIMB IS ON THE PORTRAIT, so the inspector can outline it.
    // Normalized to the portrait frame: (0,0) top-left, (1,1) bottom-right,
    // as the screen-space bounds of the limb's projected oriented box.
    //
    // ALREADY PROJECTED, deliberately. main.cpp owns the portrait camera, so
    // it is the only thing that can turn a world-space box into a place on
    // that image; handing the UI a view-projection matrix and eight corners
    // would put a second copy of the camera convention in the overlay, which
    // is precisely how the two would drift. The UI draws a rectangle.
    // A SEVERED limb gets no marker here, deliberately: it has no body, so
    // there is nothing at any position to point at, and the arm being visibly
    // ABSENT from the portrait is already the clearest possible statement that
    // it is gone. The injury list carries the word. (An X drawn at a guessed
    // anchor would be a marker for something that is not there, which is worse
    // than the hole.)
    float projMin[2] = {0, 0}, projMax[2] = {0, 0};
    bool projValid = false;
    const char* label = "";  // "Left forearm" etc, for the damage list
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

  // ==========================================================================
  // THE CHARACTER SCREEN (I) — ui/inventory_ui.cpp
  // ==========================================================================
  //
  // Everything below is MIRROR IN, INTENT OUT, and the split is the whole
  // contract this file has always claimed ("the overlay never owns inventory
  // state — it only draws it"). The screen reads the mirrors, and when the
  // player drags something it sets a one-shot latch; main.cpp consumes the
  // latch, calls the real method on the real container, and the change shows
  // up in next frame's mirror. Nothing in ui/ ever writes a game container.
  //
  // The latches follow placeWindFan's pattern above — sticky flags cleared by
  // the consumer, never frame-local bools — for the reason recorded there: the
  // fixed-tick loop runs zero times on most frames, and a frame-local bool is
  // discarded unread most of the time.

  bool inventoryOpen = false;   // I toggles; main.cpp owns the cursor/capture
  bool inspectMode = false;     // left panel: CHARACTER (gear) vs HEALTH

  // ---- the live avatar portrait (main.cpp's second render pass) ------------
  // `portraitTex` is an ImTextureID (a VkDescriptorSet behind the scenes) that
  // Overlay::RegisterTexture handed back; 0 means "not registered yet, draw
  // the empty frame". Held as uint64_t rather than ImTextureID so this header
  // stays free of imgui.h — main.cpp includes it and must not need ImGui.
  uint64_t portraitTex = 0;
  int portraitW = 0, portraitH = 0;
  // Orbit, radians. Written by the panel's drag and READ by main.cpp when it
  // places the portrait camera — the same shape as brushRadius, i.e. a view
  // parameter the UI is allowed to edit because nothing in the world depends
  // on it.
  // Yaw is an OFFSET from the character's own facing (main.cpp adds it), so 0
  // is always a front view however the body happens to be turned. Pitch is
  // absolute and slightly negative: looking a little DOWN at a standing figure
  // is the angle that reads as a portrait rather than as a worm's-eye shot.
  float portraitYaw = 0.0f, portraitPitch = -0.08f;
  // Cursor position over the portrait frame, normalized to [-1,1] with +y up,
  // valid only while the pointer is inside it and not dragging. main.cpp turns
  // this into a SetLook() so the character glances at the mouse — GAME state
  // (it moves a real rig), which is exactly why the UI reports the cursor and
  // does not pose anything itself.
  float portraitLook[2] = {0, 0};
  bool portraitLookValid = false;

  // ---- item mirrors -------------------------------------------------------
  // One row per slot in each container, in slot order. `name` empty = the slot
  // is empty. `kind` is a display word ("melee"), not an enum, because the
  // screen shows it and never branches on it.
  struct KitSlotUI {
    std::string name;
    std::string kind;
    std::string tip;    // the tooltip body: damage, reach, whatever the def has
    int count = 0;
    // ---- worn condition (game/equipment.h) ----------------------------------
    // Voxels of the piece still present, over what it started with. 1 for
    // anything that is not a worn piece, so a slot that never had a condition
    // cannot be drawn as a full bar by accident — it simply has nothing to say.
    // `ruined` is the tuning threshold already applied, so the panel never
    // holds a second copy of the rule (game/equipment.h GearRuined).
    float condition = 1.0f;
    bool wearable = false;
    bool ruined = false;
  };
  std::vector<KitSlotUI> bagSlots;      // Bag::kSlots, row-major
  std::vector<KitSlotUI> hotbarSlots;   // kItemSlots
  std::vector<KitSlotUI> equipSlots;    // kEquipSlotCount
  // Per equipment slot, from the authored table in game/equipment.h. Mirrored
  // rather than re-declared here on purpose: the accepted-kinds table is where
  // future armour lands, and a second copy in the UI would be the thing that
  // goes stale the day it does.
  struct EquipSlotUI {
    std::string label;
    std::string icon;    // chrome-atlas sprite key for the empty engraving
    std::string why;     // refusal sentence when the slot accepts nothing
    bool acceptsAnything = false;
  };
  std::vector<EquipSlotUI> equipDefs;
  int bagCols = 8, bagRows = 4;

  // ---- arsenal mirror (game/caster.h GlyphInventory) ----------------------
  struct GlyphUI {
    std::string id;
    std::string desc;
    int type = 0;         // GlyphType: 0 element, 1 form, 2 modifier
    int mana = 0;
    uint32_t color = 0;   // element swatch (gpu color0), 0 = not an element
  };
  std::vector<GlyphUI> glyphsOwned;
  // `glyphSlots` above is already the bound strip (slot -> glyph id) and IS
  // the arsenal's bottom row — the panel and the live hotkeys read one mirror,
  // which is what makes binding in the panel provably the same thing as the
  // number row.

  // ---- intents (one-shot latches, consumed by main.cpp) -------------------
  // A drag that landed. `from`/`to` are KitRefs (game/kitref.h), so one latch
  // covers bag<->hotbar<->equipment without a case per pair.
  struct MoveIntent {
    bool pending = false;
    KitRef from, to;
  } moveItem;
  // A drag that landed on NOTHING — dropped outside every panel. The one
  // gesture the move latch cannot express, because it has no destination
  // KitRef: this says "put it on the floor" and main.cpp turns it into a real
  // debris body (game/worlditems.h).
  struct DropIntent {
    bool pending = false;
    KitRef from;
  } dropItem;
  // A glyph dropped on a bound slot, BY NAME rather than by library index:
  // glyph indices are file-order dependent and die on every R reload, and a
  // latch that survives one frame can easily straddle one.
  struct BindIntent {
    bool pending = false;
    int slot = -1;             // 0..kGlyphSlots-1
    std::string glyphId;       // empty = unbind
  } bindGlyph;

  // What the last refused action said, and how long ago. Flashed under the
  // panel rather than swallowed: a slot that silently declines is the failure
  // mode that makes an inventory feel broken (game/equipment.h MoveResult).
  std::string kitMessage;
  float kitMessageAge = 1e9f;   // seconds; main.cpp ages it

  // The avatar's active dismemberment state ("normal", "crawling", "limping"),
  // from AvatarLocomotion::stateName. One line, and it says more about a pair
  // of lost legs than any number of bars.
  std::string locoState;
};

class Overlay {
 public:
  // `assetDir` is where assets/ui/chrome.{bmp,json} live. A missing or broken
  // chrome file is NOT a failure: the screen falls back to flat rectangles
  // with identical layout, and the reason is printed once.
  bool Init(GLFWwindow* window, const rhi::Device& device,
            rhi::TextureFormat format, const std::string& assetDir);
  void BeginFrame();

  // ---- handing a rendered texture to ImGui --------------------------------
  // Registers an offscreen colour view as an ImGui-drawable image and returns
  // an ImTextureID (as uint64_t, so this header stays imgui-free). The ONE
  // caller is main.cpp's avatar portrait; the descriptor is owned by the ImGui
  // Vulkan backend and released by Unregister or by Shutdown.
  //
  // `view` must outlive the registration — the descriptor points straight at
  // the VkImageView. Registering the same view twice returns two descriptors,
  // so callers register once and keep the id.
  uint64_t RegisterTexture(const rhi::TextureView& view);
  void UnregisterTexture(uint64_t id);

  // True when ImGui wants the mouse / keyboard this frame. main.cpp gates the
  // game's own bindings on these so typing in a dev-panel field stops firing
  // key actions — a bug that predates the character screen and is fixed by the
  // same gate the screen needs.
  bool WantsMouse() const;
  bool WantsKeyboard() const;
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
  // One VkSampler for every registered texture (nearest + clamp: the portrait
  // is displayed at an integer multiple and must stay pixel-crisp). Held as
  // uint64_t so the header names no Vulkan type — the handle itself is only
  // ever touched inside overlay.cpp, the sanctioned backend-exception file.
  uint64_t sampler_ = 0;
  const void* device_ = nullptr;   // vk::Backend*, for sampler destruction

 public:
  void Render(const rhi::RenderPass& pass);
  // Re-record the draw data this frame ALREADY built into a second pass,
  // WITHOUT calling ImGui::Render() again. The one caller is
  // --shot-inventory, which renders the whole frame a second time into an
  // offscreen target so the character screen can be reviewed as an image.
  //
  // Split out rather than making Render() re-entrant because the difference is
  // real: ImGui::Render() finalizes the frame's draw lists and must happen
  // exactly once, while replaying those lists into another pass is free and
  // may happen any number of times.
  void RenderRecorded(const rhi::RenderPass& pass);
  void Shutdown();
};
