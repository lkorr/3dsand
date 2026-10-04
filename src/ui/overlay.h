#pragma once
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

#include "game/contract.h" // contract::Page: the contract editor's draft (demons D5)
#include "game/kitref.h"   // KitRef: the one slot address the screen drags in
#include "gpu/rhi.h"

struct GLFWwindow;
namespace refs {
class RefStore;  // world/refs.h: the References page reads and edits it
}

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
  // CHUNK TICKETS (src/sim/tickets.h): live / releasing / cap, the run's
  // activations / releases / refusals, and one line per live ticket. Filled
  // by main.cpp from Stream::TicketSet(); read-only here.
  uint32_t ticketsLive = 0, ticketsReleasing = 0, ticketsCap = 0;
  uint64_t ticketsActivated = 0, ticketsReleased = 0, ticketsRefused = 0;
  uint32_t ticketLandingsParked = 0;
  std::vector<std::string> ticketLines;
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
  // ---- SHORT-RANGE MODE ----------------------------------------------------
  // The comparison arm against the dense-100 m WebGPU voxel engines: clamp
  // EVERY ray to render.shortRangeDist and hide the wall behind a fog ramp
  // (raymarch.wgsl aerialFrac / traceFar's tCeil). A perf mode as much as a
  // look — the frame stops marching the ~6.5 km cascade rather than fogging it
  // after paying for it.
  //
  // IT LIVES HERE AND NOT IN TUNING ON PURPOSE. The running game writes
  // tuning.json (the tuner's Save, the F5 round trip), so a view toggle stored
  // as a tuning value could overwrite the user's saved defaults just by being
  // ticked. It reaches the shader as RenderParams flag bit 2, exactly the way
  // `shadows` (bit 0) and `showDirtyVoxels` (bit 1) already do. The SHAPE of
  // the mode — ceiling, fog start fraction, fog density — is tuning, because
  // that is authored data; whether it is on is session state.
  bool shortRange = false;
  // WHICH ceiling, once the mode is on: false = render.shortRangeDist (the
  // 100 m arm), true = render.shortRangeNearDist (the 50 m one). The panel
  // draws the two as one off / 50 m / 100 m radio row, but the state stays two
  // bools so the chosen ARM survives toggling the mode off and back on.
  bool shortRangeNear = false;
  // Effective draw distance in METRES, written by main each frame for the
  // panel readout: the cascade's filled radius normally, the short-range
  // ceiling while the mode is on. Read-only in the UI — it is evidence that
  // the checkbox did something, not a second place to set it.
  float renderRangeM = 0.0f;
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
  // Top-right corner readout: fps + the material under the crosshair. Drawn
  // whether or not the dev panel is open (that is the point of it).
  bool showCornerReadout = true;
  bool showDirtyChunks = false;
  bool showDirtyVoxels = false;
  // THE CHARGE VIEW (F11; package E5b): every surface the raymarch shades is
  // drawn in false colour by the charge field's P (log scale: blue a few units,
  // yellow a spark, red an arc, white lightning) and everything uncharged in
  // dim grey. RenderParams flag bit 6 (raymarch.wgsl RFLAG_ELECVIEW), a
  // SPEC_DEBUG_VIZ branch, so it costs nothing while off.
  bool showChargeView = false;
  // Vector-field overlay (F4), a THREE-state cycle: off -> wind -> current.
  // One key rather than two because the two fields answer the same question in
  // the same picture and you almost always want to compare them, not composite
  // them — two overlapping arrow lattices in one frame read as noise.
  //
  // Each state is an arrow per lattice point around the camera, oriented and
  // coloured by the SAME field function the world itself samples — windAt() for
  // the grass sway (docs/RESEARCH_wind.md §4.8), currentAt() for the wave
  // advection and floating debris (DESIGN.md §9d.8). That identity is what
  // makes either one evidence rather than decoration.
  //
  // Seeded from wind.dbgWindField / render.dbgCurrentField on startup and on
  // every tuning reload, so both are reachable from a saved tuning.json and
  // from headless screenshot runs, neither of which can press a key. Free when
  // off: the draw is skipped outright.
  enum FieldViz : int { kFieldVizOff = 0, kFieldVizWind = 1, kFieldVizCurrent = 2, kFieldVizCount = 3 };
  int fieldViz = kFieldVizOff;
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
  // ---- the wind readout (F1 -> World -> Wind & weather) ------------------
  // Filled by the frame loop from windfield::Probe — the C++ mirror of the
  // sim's windAtQ — at the player's head, so "what is the wind here, and
  // why" has numbers: the regime, the ramp's pieces, the terms. Readout only.
  // THE TEMPERATURE AT YOUR FEET (docs/PLAN_temperature.md), filled by the
  // frame loop: the ambient the CPU computes the way the shader does
  // (HeatAmbientCpu), and the local field's target and actual excess from the
  // snapshot's probe words (the block under the feet, heatRelax). Readout only.
  struct HeatReadout {
    bool valid = false;
    std::string biome;
    int base = 0, swing = 0, ambient = 0;
    bool day = true, snowline = false, paged = false, current = false;
    int x = 0, xTarget = 0, emit = 0, emitters = 0;
    uint32_t pages = 0, pagesPeak = 0, pool = 0, refused = 0;
    uint32_t melts = 0, ignites = 0, freezes = 0, relaxChunks = 0, recompChunks = 0;
  } heat;
  struct WindReadout {
    bool valid = false;
    int source = 0;
    float intensity = 0, gale = 0, convective = 0, stability = 0, coupling = 1;
    float refSpeed = 0, gustAmp = 0, gustFrac = 0, headingDeg = 0, localHeadingDeg = 0;
    float totalMs = 0, meanMs = 0, gustExcess = 0, extraMs = 0;
    float haglM = 0, groundY = 0, profile = 0, exposure = 0, expMul = 1, ramp = 1, leeMean = 1;
    float wanderDeg = 0, thermalMs = 0, slopeMs = 0, seaMs = 0;
    float stormPhase = -1, envelope = 1, jumpDeg = 0;
    uint32_t terrQueries = 0;
  } wind;
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
  // Powder brush grain size, eighths of a cell: 0 = mixed grains (default),
  // 8 = whole cells.
  int brushGrain = 0;
  int brushMaterial = 3;     // sand
  bool reloadShaders = false;
  bool reloadMaterials = false;
  bool regenWorld = false;
  bool pendingDetonate = false;  // X key / UI button: explode at crosshair
  bool ragdollMe = false;        // one-shot: the player goes limp for ragdoll.devSeconds
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
  // PLAY or DEV (F2, and the dev panel's checkbox). Off = the game as a
  // player meets it: the hands are the only tool (kToolMelee -- fists, a
  // drawn weapon, a flask), the number row and wheel pick the hotbar, and
  // every build/debug binding (Tab tools, brush keys, M/B/K/U/L/H/T/O, fly,
  // single-step, grenades) is inert. On = everything as it always was. The
  // F-keys, pause, save/load and the menus work in both. main.cpp enforces
  // it every frame and applies the transition (fly off, hands up) on a change.
  bool devControls = true;

  // MLS-MPM fluid prototype (docs/PLAN_mpm_fluids.md): the experimental
  // particle liquid, placeable side by side with CA water for comparison.
  // fluidCount is mirrored from the main loop's CPU-owned count for the HUD;
  // clearFluid is a one-shot request consumed inside the tick loop.
  uint32_t fluidCount = 0;
  bool clearFluid = false;
  int fluidPour = 0;   // mpm tool keys 1-4: water / oil / acid / blood
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
  // ---- look tab (render) ----
  float fSurface = 1.0f;
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

  // ---- THE STRIKE COMPASS (debug, HUD, right of the hp bar) ----------------
  // Drawn while a weapon is in hand: the active flick map's sectors, the live
  // flick (the picker's smoothed mouse velocity against the pick threshold),
  // the sector a press would pick RIGHT NOW, and a readout naming the strike
  // the last press actually resolved to and the frame the running one is on.
  // main.cpp fills it from PlayerSession; the overlay only draws it.
  struct StrikeSector {
    float x = 0, y = 0;      // screen space, +y DOWN (strokes.h PlayerStrikeMap)
    std::string name;        // base style id, ":player" stripped
    bool neutral = false;    // one of the two a no-flick click alternates
  };
  bool strikeCompass = false;
  std::vector<StrikeSector> strikeSectors;
  float strikeFlickX = 0, strikeFlickY = 0;   // picker velocity, px/s
  float strikePickMin = 1;                    // melee.pickMinSpeed, px/s
  int strikeHover = -1;      // sector a press would pick now; -1 = neutral
  int strikeLastSector = -1; // sector the last press resolved to
  bool strikeLastFlicked = false;
  float strikeLastX = 0, strikeLastY = 0;     // the last press's flick dir
  std::string strikeLastText;   // "OVERHEAD  flick up" / "... no flick"
  float strikeLastAge = 1e9f;   // seconds since that press
  uint32_t strikeLastSerial = 0;  // main.cpp's edge detector on the press
  std::string strikeNowText;    // running strike: name, phase, frame
  // ---- ...AND THE CHARGED STRIKE on the same compass ----------------------
  // `strikeCharging`: a strike is parked at the end of its windup
  // (StrokeCursor::Holding). `strikeCharged`: the running strike is a charged
  // one (held, or its release still cutting). While charging the compass
  // shows the HELD sector, the remembered flick it re-aims to
  // (strike_pick.h Remembered; `strikeMemValid` false = none since the
  // press), and the re-aim slide's progress 0..1 (1 = arrived).
  bool strikeCharging = false;
  bool strikeCharged = false;
  int strikeHeldSector = -1;
  bool strikeMemValid = false;
  float strikeMemX = 0, strikeMemY = 0;
  float strikeBlend = 1.0f;
  float strikeChargeMul = 1.5f;

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
  // One-shot: spawn with THIS behaviors.json profile (the fighting-style
  // buttons: swordsman, fencer, brawler, berserker, guardian). Empty = none.
  std::string aiSpawnProfile;
  // ---- ...AND THE CREATURE'S OWN (2026-09-16) -----------------------------
  //
  // The three above are BEHAVIOUR PRESETS and they silently overrode whatever
  // the sidecar asked for, so a creature whose whole character is its profile
  // could not be spawned from this panel at all. Picking `zombie` in the
  // creature list and pressing any of them produced a zombie BODY running the
  // `duelist` profile -- which lists five sword styles and three fallback
  // punches and NOT ONE BITE -- so with an empty hand every non-fallback style
  // was unusable and the thing could only ever throw a punch.
  //
  // That is the whole of "the zombie is still just punching", and nothing was
  // wrong with the zombie: it was never a zombie. This spawns the creature on
  // the profile its own JSON names (`MobDef::behavior`), which is the rule the
  // rest of the engine already follows -- `--shot-strike` reads
  // `dA.behavior.empty() ? "duelist" : dA.behavior`, and this panel was the one
  // producer that did not.
  bool aiSpawnOwn = false;
  // ---- A RANDOM HUMAN (2026-09-24) ------------------------------------------
  // One-shot: a body drawn from assets/mobs/pool/ (MobSystem::PoolDef; baked by
  // scripts/bake_human_pool.mjs), with its weapon, outfit, dye and behaviour
  // rolled too. The ticked effect boxes still apply. session.cpp consumes it
  // beside the four buttons above.
  bool aiSpawnRandom = false;
  bool aiKillSpawned = false;     // one-shot: despawn everything this panel made
  bool aiRagdollSpawned = false;  // one-shot: knock everything this panel made flat
  // WHAT THE SPAWN BUTTONS PUT IN ITS HAND. Names rather than library indices,
  // for the reason selftest_playerkit's "names survive a library reorder" case
  // exists: an item's library index is items.json's ORDER, so inserting a
  // weapon renumbers every entry after it (which is why an ItemStack holds a
  // NAME since W2-M, and why this picker does too). main.cpp rebuilds this list from the item
  // library at load and on every R hot-reload, and re-finds the selection by
  // name — so adding a blade to items.json and hitting R moves the picker
  // without moving what is already selected.
  //
  // Entry 0 is "(unarmed)": the empty hand is a real case the AI has to cope
  // with, and a picker that cannot express it would need a second button.
  std::vector<std::string> aiWeaponNames;
  int aiWeaponPick = 0;
  // ---- F1 Spawn tab: GIVE YOURSELF A FILLED VESSEL --------------------------
  // Every container item (flask, pouch, ...) and, per vessel, every material
  // its `container.holds` classes admit -- both BY NAME and rebuilt by main.cpp
  // off the live item library and material table on load and every R, for the
  // weapon picker's reason (indices renumber on insert). session.cpp consumes
  // the one-shot `giveVessel`, filling the vessel to capacity through the same
  // ContainerAccepts rule a scoop obeys, into the hotbar or else the bag.
  std::vector<std::string> giveVesselNames;
  std::vector<std::vector<std::string>> giveVesselMats;
  int giveVesselPick = 0, giveMatPick = 0;
  bool giveVessel = false;
  std::string giveVesselStatus;
  // WHICH CREATURE the Spawn tab's three buttons put in the world. Same
  // contract as the weapon picker above and for the same reasons: rebuilt off
  // the LIVE mob defs at load and on every R, re-found by name afterwards
  // (a def index is directory order, and adding a creature renumbers it).
  //
  // The list is every def that publishes a `held_right` socket — the same
  // eligibility test the spawn already applied, now surfaced instead of
  // silently picking for you.
  //
  // IT IS BODIES ONLY (2026-09-20). A def that carries `effects` is a VARIANT
  // of a body that is already in this list — `zombie` is human+zombie,
  // `jujunud_zombie` is jujunud+zombie — and listing those beside their own
  // bases made the panel a catalogue of every combination anybody had bothered
  // to author, which is the thing the inheritance work exists to delete. The
  // combinations live in `aiEffectNames` below instead: a body, plus boxes.
  std::vector<std::string> aiCreatureNames;
  int aiCreaturePick = 0;
  // Each creature's race (MobDef::race), parallel to aiCreatureNames, and the
  // list's race filter: 0 all, 1 human, 2 sylvan, 3 other.
  std::vector<std::string> aiCreatureRaces;
  int aiRaceFilter = 0;
  // ---- ...AND WHAT IS WRONG WITH IT (2026-09-20) --------------------------
  //
  // One checkbox per `assets/mobs/effects/*.json` (game/mob.h MobEffectNames),
  // rebuilt off the directory on every R like every other list here. Whatever
  // is ticked is handed to MobSystem::DefWithEffects as `fx` at spawn, so
  // `jujunud` + [zombie] spawns the zombie of jujunud — resolving to the
  // AUTHORED `jujunud_zombie.json` if there is one and composing it if there
  // is not, which is the preference order that call already implements.
  //
  // Nothing here knows the word "zombie", and that is the point: the panel
  // offers exactly the modifiers the content publishes, so `effects/burning.json`
  // gets a box the moment it exists.
  //
  // `int` rather than `bool` because std::vector<bool> has no addressable
  // element to hand ImGui::Checkbox.
  std::vector<std::string> aiEffectNames;
  std::vector<int> aiEffectOn;
  // ---- WHAT THE SPAWN WEARS (2026-09-19) ----------------------------------
  //
  // Until this the panel spawned every creature naked, and the only way to
  // watch armour take a blow was to dress the AVATAR through the wardrobe and
  // let something hit you. `aiOutfit` is the mode: 0 nothing (what it always
  // did), 1 random commoner clothes (a dyeable piece per slot, each in its own
  // random colour), 2 full plate (every slot's iron piece), 3 custom — one
  // picker per worn equip slot, so "only a helmet and a cuirass" is two combos
  // rather than a preset somebody has to author.
  //
  // The pickers are mirrors of the item library BY SLOT, rebuilt by main.cpp
  // on every R and re-found by name, exactly like the weapon picker above.
  // Indexed by worn equip slot in EquipSlotId order (Head, Chest, Legs, Boots,
  // Shoulders, Hands, Belt, Trinket); entry 0 of each name list is "(none)".
  // A slot no shipped item fits stays a one-entry list and the overlay skips
  // drawing it, so the panel only ever shows slots something can go in.
  int aiOutfit = 0;
  std::vector<std::string> aiWearSlotLabels;
  std::vector<std::vector<std::string>> aiWearNames;
  std::vector<int> aiWearPick;
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
  // One per ai::Intent, in enum order (main.cpp static_asserts the count).
  static constexpr int kAiIntents = 16;   // + `cast` (demons D4)
  float aiIntentWeight[kAiIntents] = {};
  int aiIntentCooldown[kAiIntents] = {};
  int aiIntentDwell[kAiIntents] = {};
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

  // ---- WARDROBE (game/dye.h, scripts/gen_peasant_clothes.py) --------------
  //
  // The colour half of the commoner clothes. The art is nine greyscale
  // patterns whose cells are multipliers; the colour arrives at runtime as one
  // packed word, so a shirt, a pair of trousers and a pair of shoes plus a
  // point on a colour wheel is a whole outfit nobody had to draw.
  //
  // SAME SHAPE AS THE AI PANEL ABOVE, deliberately, down to the one-shot
  // bools: the overlay owns no game state, the pickers are mirrors main.cpp
  // rebuilds off the LIVE item library on every R, and the selection is
  // re-found BY NAME afterwards — so adding a tenth pattern to
  // gen_peasant_clothes.py and hitting R puts it in the combo without
  // disturbing what is already picked.
  // (Drawn in the F1 sidebar's Spawn page, "Clothes"; no window of its own.)
  // The dye, as the picker's own 0..1 RGB. Packed by main.cpp when it spawns,
  // never here — UIState is a POD the overlay may only read and write, and
  // packing it here would put game/dye.h in the UI's dependency set for no
  // gain.
  float wardrobeColor[3] = {0.42f, 0.22f, 0.58f};
  // Dyeable pieces by slot, mirrored from the item library. Entry 0 of each is
  // "(none)", so an outfit of trousers and nothing else is expressible without
  // a second button.
  std::vector<std::string> wardrobeShirts, wardrobeLegs, wardrobeFeet;
  int wardrobeShirtPick = 0, wardrobeLegsPick = 0, wardrobeFeetPick = 0;
  bool wardrobeSpawnSet = false;   // one-shot: the whole outfit into the pack
  bool wardrobeWearSet = false;    // one-shot: ...and put it on
  bool wardrobeRandomColor = false;  // one-shot: reroll wardrobeColor
  bool wardrobeDyeWorn = false;    // one-shot: recolour what is already worn
  std::string wardrobeStatus;      // what the last button did
  // What the picked colour is CALLED (game/dye.h DyeName), mirrored per frame
  // so the overlay does not need the header.
  std::string wardrobeColorName;

  std::vector<std::string> materialNames;  // index == material id
  std::vector<uint32_t> materialColors;    // 0xAABBGGRR swatch (gpu color0)
  // The F1 material picker's columns and icons, parallel to materialNames and
  // rebuilt with it (main.cpp FillUiMaterials): the class (0 solid, 1 powder,
  // 2 liquid, 3 gas — materials.h CLASS_*), the two other palette variants so
  // the icon is a 2x2 of the real jitter colours, and the tags comma-joined
  // (the solids column sub-groups on them and the tooltip lists them).
  std::vector<uint8_t> materialClass;
  std::vector<uint32_t> materialColors1, materialColors2;
  std::vector<std::string> materialTags;
  bool visible = true;
  // Which page of the F1 sidebar is showing (Overlay::Draw's kDevTabs).
  int devTab = 0;

  // ---- magic (game/spell.h, game/caster.h) --------------------------------
  // The crossover readout — where the running cost stops coming out of mana
  // and starts coming out of health — IS the tension mechanic, so it gets a
  // real visual break rather than a number.
  bool magicMode = false;         // number row speaks glyphs instead of picking
                                  // a brush material
  int32_t mana = 0, manaMax = 0;  // manaMax is the EFFECTIVE max (pool - reservation)
  // The pool's authored max and what live statuses reserve out of it (plan
  // §7): the bar draws [manaMax, manaPoolMax] as reserved.
  int32_t manaPoolMax = 0, manaReserved = 0;
  // Dev overrides for the pool ("dev: mana pool" under the magic bar). The
  // pool belongs to the caster, so the panel only ASKS and main.cpp applies
  // these in the tick loop, beside the regen tick, so a sustained aura under
  // "infinite" never sees a dry tick between frames.
  int32_t devManaMaxEdit = 0;        // the input box; 0 = seed from manaPoolMax
  int32_t devManaMaxRequest = -1;    // one-shot: set the pool's max to this
  bool devManaFill = false;          // one-shot: mana = effective max
  bool devManaInfinite = false;      // sticky: refill to the max every tick
  // demons D2: "learn all demon names" (game/demon_lore.h). One-shot ask,
  // applied in the tick beside the mana overrides; the tick writes back what
  // it did for the line under the button.
  bool devLearnDemonNames = false;
  std::string devDemonNamesStatus;
  // What the caster is sustaining, newest last, for the HUD list and the
  // drop key. Each line is the status's readout and its per-tick price.
  std::vector<std::string> spellStatuses;
  int spellRefused = 0;           // ops a ward refused this tick
  int32_t health = 0;
  // Authored ceiling for the HUD bar's denominator. Health does NOT regenerate,
  // so this is only ever a high-water mark the player moves away from.
  int32_t healthMax = 0;
  // The most health the body can currently HOLD: healthMax x the burn cap
  // (PlayerAvatar::HealthCap). The bar draws [healthCap, healthMax] as charred
  // off, so a burnt body reads as a permanently short bar rather than as a
  // bar that quietly rescaled itself.
  int32_t healthCap = 0;
  bool playerAlive = true;

  // ---- THE DEATH HOLD -------------------------------------------------------
  // A dead player is no longer rebuilt on a timer. The body stays where it
  // fell, the character screen opens on the health column, and every readout
  // below is FROZEN at the moment of death — main.cpp stops refilling it, so
  // what is on screen is the state the body was in when it died rather than
  // the state the corpse has decayed into since. Nothing comes back until the
  // column's respawn button is pressed.
  //
  // This exists to make a death diagnosable: "which limb ran out, what was on
  // fire, what was still worn" is unanswerable if the answer is overwritten
  // three seconds later by a fresh body.
  bool deathScreen = false;      // dead and holding; the readout is frozen
  float deathHoldSec = 0.0f;     // how long the body has lain there, seconds
  float deathRespawnAfter = 0.0f;  // tune.avatar.respawnDelay: button waits it out
  bool respawnRequest = false;   // the button was pressed; main.cpp consumes it
  bool deathScreenOpened = false;  // this death has already opened the column
  // What the engine says killed you (Mob::DeathCause) — "blood loss", "vital
  // limb destroyed", "burnt past the death knot". Empty when nothing claimed
  // it, which is itself worth seeing: it means the body simply ran out.
  std::string deathCause;

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
    // Voxels whose material is tagged `hot` RIGHT NOW, and specifically NOT
    // the size of the limb's burn front: the front carries every voxel with a
    // self-reaction, which includes drying blood and crumbling char, and
    // reporting that as "burning" left a limb reading ON FIRE long after the
    // fire was out. See BodyBurnState::hotVox in game/mob.h.
    uint32_t burningVoxels = 0;
    // ---- WHAT IS ON THE OUTSIDE OF IT (docs/PLAN_body_coat.md) -----------
    // The coat ledger's verdict for this limb (game/mob.h LimbCoat): how
    // soaked it is, and by what. A different question again from hpFrac /
    // voxelFrac / charredFrac — those are all about damage to the limb's OWN
    // matter, and a limb can be drenched in somebody else's blood at full
    // health, which is exactly the case the figure had no way to show.
    //
    // stainFrac is AMOUNT-WEIGHTED (LimbCoat::Frac), so 1.0 means every voxel
    // is saturated rather than "every voxel has a speck on it".
    float stainFrac = 0.0f;
    uint32_t stainMat = 0;    // dominant material id; 0 = clean
    // Its authored stain colour, opaque. Stored in the GPU's 0xAABBGGRR,
    // which is byte-for-byte ImGui's default IM_COL32 packing — so this is an
    // ImU32 that needs no swizzle. Carried as uint32_t rather than ImU32
    // because this header deliberately does not include imgui.h.
    // 0 = nothing to draw.
    uint32_t stainColor = 0;
    // COPIED, not pointed at. Every other string in this struct is either a
    // std::string or a pointer into a static table; a material NAME lives in
    // main.cpp's `mats` vector, which R (reload materials) replaces wholesale
    // mid-frame, and one dangling frame is not worth the four bytes saved.
    char stainLabel[24] = {0};
    float hp = 0, hpMax = 0;     // absolute, for the numeric readout
    // Per-tissue voxel counts: what the limb is MADE OF right now.
    uint32_t voxelTotal = 0;     // surviving voxels (same as PartVoxelCount)
    uint32_t voxelSkin = 0;
    uint32_t voxelFlesh = 0;
    uint32_t voxelMuscle = 0;
    uint32_t voxelBone = 0;
    uint32_t voxelBrain = 0;
    uint32_t voxelBrainMax = 0;  // brain voxels at spawn (for "X missing")
    // Tissue a bite has already turned (materials tagged `infectious`, i.e.
    // rotflesh). It is counted in voxelTotal like any other tissue, so it does
    // NOT move voxelFrac — rot is a limb being the wrong thing, not a limb
    // being smaller, and only the rot that eventually deletes voxels hollows
    // it out. A separate number because it is the only tissue count that goes
    // UP when things get worse.
    uint32_t voxelRot = 0;
    // ...BY INFECTION (2026-10-01): which infection materials those are, the
    // heaviest first, named by their authored `infect.label` ("rot",
    // "venom") and drawn in the material's own colour -- a snake bite reads
    // as venom, not as rot. voxelRot is their sum.
    struct InfectRow {
      uint32_t count = 0;
      uint32_t color = 0;   // ImU32, the material's palette colour 0
      char label[16] = {};
    };
    static constexpr int kInfectRows = 3;
    InfectRow infect[kInfectRows];
    int infectCount = 0;
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
  // The same three over the WHOLE body (MobSystem::BodyCoat, which counts the
  // base limbs only — a robe soaked through is not the wearer being covered).
  // Drawn as one line above the health bar, so "I am covered in blood" is
  // legible without opening anything.
  float stainFrac = 0.0f;
  uint32_t stainMat = 0;
  uint32_t stainColor = 0;
  char stainLabel[24] = {0};
  // ...and EVERY substance the ledger ranked, heaviest first, each with its
  // own share of the body (same amount-weighted scale as stainFrac, so they
  // sum to at most stainFrac). The line above names the dominant one only,
  // which is how an oiled player standing in the rain read as "water" and
  // nothing else. Sized to game/mob.h kCoatTop (not included here; main.cpp
  // static_asserts the two agree).
  static constexpr int kCoatNames = 4;
  struct CoatName {
    float frac = 0.0f;
    uint32_t mat = 0;
    uint32_t color = 0;     // 0xAABBGGRR, as stainColor
    char label[24] = {0};
  };
  CoatName coats[kCoatNames];
  int coatCount = 0;
  // tune.coat.hudMinFrac, MIRRORED IN rather than read: ui/ includes no sim
  // header, and a threshold the overlay reached for itself would be a second
  // place the number lives. Below it the HUD line, the figure's chip and the
  // injury row all say nothing — a single splashed voxel is not "bloodied".
  float stainHudMin = 0.02f;
  int32_t spellCost = 0;          // running cost of the spoken sequence
  // The price SPLIT (plan §9): word costs, the tariff on what the cast does
  // to the world, and the delivery premium on that tariff. "Why is this 900
  // mana" is answered here before the cast.
  int32_t spellWord = 0, spellTariff = 0, spellCarry = 0;
  bool spellPriceUnknown = false; // `anything`: part of the price is billed on resolve
  int32_t spellLastBill = 0;      // what the last wildcard resolve billed
  float spellLastBillAge = 99.0f;
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
  // slot -> glyph id or page name, for the bound-key strip. Empty string =
  // unbound slot. Twenty slots: bank A (1-0) then bank B (Shift+1-0).
  std::vector<std::string> glyphSlots;
  std::vector<int> glyphSlotKinds;             // SlotKind: 0 none, 1 glyph, 2 page
  std::vector<std::string> glyphSlotReadouts;  // a page's bracket readout
  bool glyphBankB = false;                     // Shift held: the strip highlights bank B
  // Which bound key's spell is SELECTED (PLAN_spell_graph §0b: a number key
  // selects, right-click casts, the selection persists). -1 = nothing held.
  int glyphSelected = -1;
  // THE SPELL BAR (Z; spells in hand): per bound key, the colour its spell's
  // flight is drawn in (0xAABBGGRR, 0 = none) and the glyph's sort for its
  // engraving (-1 for a page), parallel to glyphSlots.
  std::vector<uint32_t> glyphSlotColors;
  std::vector<int> glyphSlotTypes;
  // The spell in each hand (0 right, 1 left): its name ("" = none), whether it
  // is a page, its colour, and the glyph sort for a one-glyph spell.
  std::string handSpell[2];
  bool handSpellPage[2] = {false, false};
  uint32_t handSpellColor[2] = {0, 0};
  int handSpellType[2] = {0, 0};
  std::string spellNote;                       // "saved as ...", "the stack is full"
  float spellNoteAge = 99.0f;

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
  bool inspectResume = false;   // the health column was shut for the alchemy bench; reopen after
  int inspectSelected = -1;     // BodySlot of the limb whose detail is open, -1 = none
  // WHICH TRIAGE GROUPS ARE EXPANDED, one bit per body group (head, torso,
  // left arm, right arm, left leg, right leg — the table lives in the panel,
  // which is the only thing that knows what a "group" is). Kept here rather
  // than as a static in the drawing code because it is per-PLAYER UI state
  // like the selection above it, and because a static would survive a body
  // it has nothing to do with.
  uint32_t triageOpen = 0;

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
  // Zoom and pan: the CURRENT values are what the camera reads this frame.
  // The TARGET values are where it is heading — set by limb focus or reset,
  // then the per-frame lerp in main.cpp closes the gap. Direct manipulation
  // (scroll wheel, right-drag) writes BOTH so the response is instant.
  float portraitZoom = 1.0f;
  float portraitPanX = 0.0f, portraitPanY = 0.0f;
  float portraitZoomTarget = 1.0f;
  float portraitPanXTarget = 0.0f, portraitPanYTarget = 0.0f;
  // One-shot: reset orbit, zoom and pan to defaults.
  bool portraitReset = false;
  // One-shot: zoom and pan to frame the given BodySlot. -1 = none.
  int portraitFocusSlot = -1;
  // The limb the orbit PIVOTS around. -1 = whole body center of mass (the
  // default). Set by double-click-to-frame; cleared by reset. While set,
  // left-drag rotates around that limb's world-space center rather than the
  // body's, so clicking a hand and orbiting keeps the hand centered.
  int portraitPivotSlot = -1;
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
    // Does a double-click open it on the ITEM STAGE (itemstage::StageTakes,
    // asked of the def by main.cpp): the gesture and its tooltip hint.
    bool stageable = false;
    // WHAT IT IS COATED WITH (DESIGN.md "A coat moves on contact"): the coat
    // on most of its voxels, as a swatch (0xAABBGGRR, 0 = clean) and a name,
    // so a venomed blade reads as one in the pack and in its tooltip.
    uint32_t coatSwatch = 0;
    std::string coatName;
    bool ruined = false;
    // ---- the DYE (game/dye.h) ------------------------------------------------
    // The item's colour as a 0xAABBGGRR swatch ImGui can draw directly, and 0
    // for undyed — which is every weapon, every plate piece and every garment
    // nobody has coloured. Pre-converted by main.cpp rather than handed over
    // packed, so this panel keeps its "reads UIState, knows no headers" shape.
    //
    // This is the whole reason a dyed item is legible in the pack at all. The
    // icon is drawn from a sprite atlas keyed on KIND, so three tunics in three
    // colours are three identical pictures; the swatch is what tells them
    // apart, and `dyeName` is what the tooltip says out loud.
    uint32_t dyeSwatch = 0;
    std::string dyeName;
    // A VESSEL's contents over its capacity, 0..1; -1 for anything that is not
    // a vessel. The HUD hotbar draws it as a gauge under the icon, the one
    // number you need while pouring.
    float fill = -1.0f;
    // ...and what it holds, as the contents' own colour (0xAABBGGRR, opaque),
    // 0 when empty: the icon pours it into the flask and the gauges draw in it
    // (game/container.h ContainerFillSwatch).
    uint32_t fillSwatch = 0;
    // ...and how brightly they glow, 0..1: the material's own `emission`
    // (game/container.h ContainerFillGlow), so the icon glows exactly when the
    // flask in the world does. 0 for anything that does not.
    float fillGlow = 0.0f;
    // A MIXTURE's layers, bottom to top (heaviest first, the order they
    // settle in): each band's colour and the fraction of the CONTENTS it
    // is. Empty for an unmixed vessel -- the icon then pours `fillSwatch`.
    std::vector<uint32_t> fillBandColor;
    std::vector<float> fillBandFrac;
  };
  std::vector<KitSlotUI> bagSlots;      // Bag::kSlots, row-major
  std::vector<KitSlotUI> hotbarSlots;   // kItemSlots
  std::vector<KitSlotUI> equipSlots;    // kEquipSlotCount
  // THE TWO HANDS (dual wielding): which equipSlots entries they are
  // (game/equipment.h EquipSlotOfHand; [0] right, [1] left), filled by
  // main.cpp from the table so this layer names no EquipSlotId. And the
  // hand last used, which the HUD frames: F cycles its vessel.
  int handEquipSlot[2] = {-1, -1};
  int lastHand = 0;
  // Per equipment slot, from the authored table in game/equipment.h. Mirrored
  // rather than re-declared here on purpose: the accepted-kinds table is where
  // future armour lands, and a second copy in the UI would be the thing that
  // goes stale the day it does.
  struct EquipSlotUI {
    std::string label;
    std::string icon;    // chrome-atlas sprite key for the empty engraving
    std::string why;     // refusal sentence when the slot accepts nothing
    bool acceptsAnything = false;
    // The slot's authored accepted kinds, as the same display words KitSlotUI
    // carries. Mirrored so the panel can light up the ONE slot a dragged piece
    // fits — before that it could only ask "does this slot take anything at
    // all", which turned every armour slot red under every drag. Still not a
    // second copy of the rule: this IS EquipSlotDef::accepts, named.
    std::vector<std::string> accepts;
  };
  std::vector<EquipSlotUI> equipDefs;
  int bagCols = 8, bagRows = 4;

  // ---- arsenal mirror (game/caster.h GlyphInventory) ----------------------
  struct GlyphUI {
    std::string id;
    std::string desc;
    int type = 0;         // GlyphSort: 0 matter, 1 effect, 2 delivery, 3 mod, 4 operator
    int mana = 0;         // the word cost
    bool hidden = false;  // replaced by a signed component (M2): loadable,
                          // not offered in the word column
    uint32_t color = 0;   // matter swatch (gpu color0), 0 = not matter
    // The §9 info box, every field read from the glyph's JSON entry so the
    // box is never wrong about the glyph and a modder's glyph gets one free.
    bool owned = true;    // unowned: drawn greyed, name hidden
    std::string valence;  // "matter < >< > matter -> effect"
    std::string tariff;   // the tariff formula, in words
    std::string axis;     // what repeating scales
    std::string example;
    std::string delivers; // which deliveries carry it
    std::string emptyNote;// "left empty: _" for operators
    // A Mod that edits the whole record WHEREVER it is spoken. `count` was the
    // only one and is not any more (2026-09-22): a count splits the scope it
    // was spoken in, so a mod dropped on a branch edits that branch. Kept as a
    // field because the drop preview promises which it is, and the day a
    // genuinely record-wide mod exists it has somewhere to say so.
    bool recordWide = false;
    // A Mod whose field is `count`: the word that MAKES the branches. The canvas
    // needs to know, because a split is the one mod a socket drop may not answer
    // by opening a lane - see the drop rule in spellgraph_ui.cpp.
    bool splits = false;
  };
  std::vector<GlyphUI> glyphsOwned;   // EVERY glyph, `owned` says which
  // ---- the grimoire (plan §12b) --------------------------------------------
  struct GrimoirePageUI {
    std::string name;
    std::vector<std::string> words;
    bool readOnly = false;      // an authored starter
    std::string readout;        // the bracket readout of its expansion
    int32_t price = 0;          // lowered as if cast alone
    bool priceUnknown = false;  // depends on `anything`
    int dropped = 0;            // words that no longer resolve
    // ITS SHAPE AS ONE GLYPH inside another spell (caster.h, PageShape):
    // the sort it stands as (GlyphSort; 2 = delivery for a CARRIER page that
    // boxes what is before it), -1 when it is no usable page; how many open
    // slots it takes as inputs (`leftInputs` of them from the words before
    // it); how many items it holds.
    int shapeSort = -1;
    bool carrier = false;
    int inputs = 0, leftInputs = 0, outputs = 0;
  };
  std::vector<GrimoirePageUI> grimoirePages;
  int grimoireMaxPages = 32, grimoireMaxWords = 16;
  // The page being composed: UI-owned until Save. main.cpp fills the readout
  // and price for the row every frame through the same DescribeSpell the live
  // sentence uses, so the panel can never disagree with the game.
  bool grimoireMode = false;                // vestigial: the old arsenal/grimoire toggle
  // THE BOOK IS SHUT UNTIL YOU OPEN IT (2026-09-22). The character screen is
  // two things at once: a place you take stock (body, gear, pack) and a place
  // you WRITE (the spell page). The second wants every pixel on the screen and
  // the first wants none of them, so the book lies closed on the desk - its
  // spine, and the ten keys you actually cast with - and opening it takes the
  // whole column, pack and all. Closed is the default because opening the
  // character screen is usually about the body.
  bool spellbookOpen = false;
  // THE SPELLBOOK'S LOWER HALF. The grimoire and the arsenal are one panel
  // (2026-09-21); this folds the EVERY WORD table away and leaves the bound
  // keys, which is the trade a short screen wants — the table is a reference
  // surface, the tree above it is the thing being built. Open by default.
  bool spellWordsOpen = true;
  std::string grimoireSelected;             // page name, "" = none
  std::string grimoireEditName;
  std::vector<std::string> grimoireEditWords;
  bool grimoireEditDirty = false;
  std::string grimoireEditReadout;
  int32_t grimoireEditPrice = 0;
  bool grimoireEditPriceUnknown = false;
  // COMPOSER UNDO, UI-owned like the row itself. Every mutation of the row
  // pushes the list it replaced onto `grimoireUndo`; ctrl+Z pops it onto
  // `grimoireRedo`. The stacks belong to ONE page: `grimoireUndoPage` records
  // which, and the panel drops both the moment the open page changes under
  // them (a page selected in the list, a save that renamed it, a delete) —
  // an undo that pasted one page's words into another would be worse than no
  // undo at all. The row is a drag-built list, and a drag that landed one cell
  // off is the mistake this panel makes most.
  std::vector<std::vector<std::string>> grimoireUndo, grimoireRedo;
  std::string grimoireUndoPage;
  // ---- THE SPELL GRAPH (docs/PLAN_spell_graph.md §4-§6) ---------------------
  //
  // A PLAIN-STRUCT MIRROR of game/spellgraph.h's `SpellGraph`, field for field,
  // in ints and strings. This header is included by main.cpp and stays both
  // imgui-free AND spell-free on purpose, so it may not name `SpellGraphNode`,
  // `GlyphSort` or `BoxPrice`; main.cpp copies across every frame the composer
  // is open. A glyph crosses BY NAME (DESIGN §8b) — indices die on R reload —
  // but a TREE NODE index is fine, because it is re-derived from the same word
  // list in the same frame the intent is consumed.
  struct SpellGraphUI {
    struct Node {
      int kind = 0;            // GraphKind: 0 word 1 operator 2 join 3 modtag
                               // 4 root 5 socket 6 bus 7 split
      int treeNode = -1;       // index into the SpellTree; -1 = synthesized
      std::string glyphId;     // the word's NAME; "" on a synthesized node
      std::string label;       // what the cell says ("fire", "PROJECTILE")
      int n = 1;               // multiplicity
      int sort = 0;            // GlyphSort: 0 matter 1 effect 2 delivery
                               // 3 mod 4 operator 5 separator
      uint32_t color = 0;      // the matter swatch, as GlyphUI carries it
      int lane = 0;
      int x = 0, y = 0, w = 0, h = 0;
      int layer = 0;
      // The LOWEST layer this node's subtree occupies, which is the band its
      // edge into its parent leaves from (SpellGraphNode::baseLayer). Equal to
      // `layer` for everything a single band tall; lower on a spoken delivery,
      // whose bar is the top of its own span.
      int baseLayer = 0;
      int subX = 0, subW = 0;
      // Operator
      bool hasLeft = false, hasRight = false;
      bool leftFilled = false, rightFilled = false;
      bool complete = true;
      int wordCostOf = 0;      // the glyph's word cost, for the struck-through
                               // price on an incomplete operator
      // Join / Root
      int instances = 1, laneCount = 0;
      std::vector<int> sockets;
      int bus = -1;
      bool hasPrice = false;
      int wordCost = 0, tariff = 0, carryCost = 0, priceInstances = 1, leaves = 1;
      bool instancesClamped = false;
      int subtotal = 0;
      // ONE RECORD, SEVERAL CELLS: a split delivery is drawn once per branch and
      // exactly one of those cells - the PRIMARY - owns `sockets`, `bus`,
      // `split`, the price and the box's whole span. `primary` is the primary's
      // index on a copy and -1 on the primary itself, so an unsplit box reads
      // exactly as it did before the field existed. See SpellGraphNode.
      int primary = -1;
      int split = -1;          // this box's Split junction, -1 when unsplit
      // On a Socket / Bus / Split: the box cell that owns it (SpellGraphNode::
      // owner). A sub-fan's pip is in no `sockets` list, so this is the only
      // answer to "which box is this mark on".
      int owner = -1;
      int bolts = 1;           // how many bolts THIS branch fires (a sub-split)
      // Socket, or WHICH BRANCH a Join cell caps
      int instance = -1;
      int pipW = 32;
      // ModTag
      std::string edit;        // "speed x2"
      bool wasted = false;
      // Word / Operator / ModTag: MAGNITUDE (docs/PLAN_spell_magnitude.md
      // §2.5). Per-mille; `graded` says the wheel over the cell may move it,
      // on the lattice magMin..magMax by magStep; `magLabel` is what it reads
      // as ("gravity -0.50 g").
      int mag = 1000;
      bool graded = false;
      int magMin = 1000, magMax = 1000, magStep = 1000;
      int magDefault = 1000;
      std::string magLabel;
      // WHEN it fires in its carrier (M3): SpellTrigger 0 hit 1 bounce 2 expire
      // 3 launch 4 every; `every` the period; `delay` ticks later. The phrase
      // is empty at the default (on hit, no delay).
      int trigger = 0, every = 0, delay = 0;
      std::string timingPhrase;
      int spanFirst = -1, spanLast = -1;
      // A PAGE used as one glyph (SpellGraphNode::page): its name on the cell
      // that stands for it (and on a Hole that is one of its inputs), with
      // how many inputs it takes and how many items it holds.
      std::string page;
      int inputs = 0, outputs = 0;
    };
    struct Edge {
      int from = -1, to = -1;
      int kind = 0;            // GraphEdge: 0 trunk 1 bus 2 socket 3 fan 4 slot
    };
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    int root = -1;
    int width = 0, height = 0, layers = 0;
    // The whole cast's total, the three parts the HUD shows, under the hand.
    int wordCost = 0, tariff = 0, carryCost = 0, manaCost = 0;
    bool priceUnknown = false;
    // A page nested in the composed words was EXPANDED to draw the tree, so an
    // edit rewrites it as its words. Said on the status line rather than
    // silently.
    std::string expandedNote;
  };
  SpellGraphUI spellGraph;
  // THE CANVAS'S VIEW (2026-09-22): drag to pan, wheel to zoom. UI-owned and
  // persistent across frames, because a view is a thing you SET and then work
  // in — it must survive the composer resizing under it, a word being typed,
  // and the page's shape changing.
  //
  // `spellGraphZoom` is 0 for AUTO: the fit-or-halve rule the canvas always
  // had, re-decided every frame, with the drawing centred and the pan ignored.
  // Any other value is an explicit scale off the ladder in spellgraph_ui.cpp,
  // set by the wheel, and it is the only state that turns the pan on. A
  // double-click on the canvas's background puts it back to 0.
  //
  // THE LADDER IS THE PIXEL-ART RULE. Only 0.5 and whole numbers are ever
  // stored here: a 2x sprite at 0.5x is its authored size and at 2x/3x/4x is
  // whole pixels, and anything between would put chrome on half a pixel.
  float spellGraphZoom = 0.0f;
  float spellGraphPanX = 0.0f, spellGraphPanY = 0.0f;
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
  // ---- LOOT (game/corpses.h) -----------------------------------------------
  // The corpse the screen is open ON, mirrored like the bag: one row per piece
  // still on it. main.cpp knows WHICH corpse; the panel only sees the list, and
  // a KitRef in KitSpace::Loot indexes it. Opened by E over a corpse (which
  // opens the screen too) and closed by the panel's button, by the screen
  // closing, by the corpse being emptied or destroyed, or by walking away.
  bool lootOpen = false;
  std::string lootTitle;               // what fell (the mob def's name)
  std::vector<KitSlotUI> lootSlots;
  // Right-click on a loot slot, or the panel's "take all". Take-only: putting a
  // thing ONTO a corpse would be data with no body in the world, so a drag
  // into the loot panel is refused (main.cpp answers with the sentence).
  struct TakeIntent {
    bool pending = false;
    int index = -1;
    bool all = false;
  } takeLoot;
  bool lootClose = false;              // the panel's close button
  // ---- A CHEST (world/refs_doors.h `container`) in the same panel ----------
  // Non-empty while the loot panel shows a container ref's contents instead
  // of a corpse's. Set by the tick's use verb (refs::TickRefs), with
  // `lootRefOpenReq` asking the frame to open the screen around it. A chest
  // TAKES PUTS: a drag from the bag/hotbar onto the panel stores the stack.
  std::string lootRef;
  bool lootRefOpenReq = false;
  // ---- the look prompt ------------------------------------------------------
  // What E would do to the thing under the crosshair, or empty. Written by
  // main.cpp's reach ray every frame, drawn by DrawHUD under the crosshair.
  std::string lookPrompt;
  // ---- THE CONVERSATION PANEL (ui/dialogue_ui.h, game/dialogue.h) ----------
  // A MIRROR of the primary session's conversation, filled by main.cpp every
  // frame from dialogue::MakeView. The panel draws it and writes `pick` (a
  // TalkCommand: 1..9 a choice, 10 continue, 11 leave); main.cpp hands that
  // to the tick command, and the TICK applies it. The panel never advances a
  // conversation itself.
  struct TalkUI {
    bool open = false;
    std::string dialogue;
    std::string speaker;
    std::string text;
    std::vector<std::string> choices;
    bool canContinue = false;
    bool canLeave = true;
    uint32_t steps = 0;
    int pick = 0;      // intent latch, 0 = none
    int hover = -1;    // which row the mouse is on (drawing only)
  } talk;
  // The Spawn page's Dialogue section (the dev hook until P7 wires NPCs).
  std::vector<std::string> dialogueNames;
  int dialoguePick = 0;
  bool dialogueTalkNearest = false;  // talk to the nearest creature (12 m)
  bool dialogueTalkVoice = false;    // talk with nobody in the world
  bool dialogueReload = false;
  bool dialogueResetFlags = false;
  std::string dialogueStatus;
  std::vector<std::string> dialogueProblems;  // file / node / field lines
  std::vector<std::string> dialogueFlags;     // "name = value", then "met: ..."

  // RIGHT-CLICK: "put this where it belongs, I do not want to aim." The panel
  // deliberately does NOT pick the destination slot — it has the accepted-kinds
  // mirror and could, but choosing where a piece goes is the equipment system's
  // decision (game/equipment.h owns the slot table, and it is the thing that
  // knows a slot is already full). So this says only WHICH item was clicked and
  // main.cpp answers with a real Move, exactly as a drag does.
  struct EquipIntent {
    bool pending = false;
    KitRef from;
  } equipItem;
  // A glyph dropped on a bound slot, BY NAME rather than by library index:
  // glyph indices are file-order dependent and die on every R reload, and a
  // latch that survives one frame can easily straddle one.
  struct BindIntent {
    bool pending = false;
    int slot = -1;             // 0..kGlyphSlots-1
    std::string glyphId;       // empty = unbind
    bool page = false;         // glyphId names a grimoire page, not a glyph
  } bindGlyph;
  // A bound key dragged onto another bound key: the binding MOVES there,
  // exchanging with whatever was already on that key. One gesture, two binds —
  // which the single-slot BindIntent above cannot say, and the reason the
  // twenty keys used to be rearrangeable only by re-dragging from the table.
  struct BindMoveIntent {
    bool pending = false;
    int from = -1, to = -1;    // 0..kGlyphSlots-1
  } moveBind;
  // A grimoire operation: save the composed page (cycle-checked by main.cpp,
  // refused with the reason in kitMessage), delete a page, or duplicate a
  // read-only starter into the player's own pages.
  struct GrimoireIntent {
    bool pending = false;
    enum Op { Save = 0, Delete, Duplicate } op = Save;
    std::string name;
    std::vector<std::string> words;
  } grimoireOp;
  // READY A PAGE: a click on a spellbook page makes it the spell the portrait
  // casts on the limb you click (PlayerCaster::ArmPage). An empty name clears
  // it (the blank leaf). `armedPage` is main.cpp's mirror of what is readied.
  struct ArmPageIntent {
    bool pending = false;
    std::string name;
  } armPage;
  std::string armedPage;
  // A GESTURE ON THE CANVAS (PLAN_spell_graph §5). The page never edits words:
  // it names a TREE OP and main.cpp applies it to the tree of the current edit
  // words, linearizes the result back and writes it into `grimoireEditWords`.
  // Two views, one truth — and every op is total, so a refusal is a sentence on
  // the status line (`kitMessage`) rather than a malformed page.
  struct GraphEditIntent {
    bool pending = false;
    enum Op {
      Insert = 0, FillSlot, Wrap, AttachMod, Remove, Unbox, Move, CloseLane,
      SetMagnitude,  // the wheel over a graded cell: `treeNode` to `mag`
      SetTiming      // the timing popup: `treeNode` fires on trigger/every/delay
    } op = Insert;
    int treeNode = -1;      // the box (Insert/AttachMod/CloseLane), the group
                            // (FillSlot), or the subject (Wrap/Remove/Unbox/Move)
    int boxTreeNode = -1;   // Move's destination box
    int lane = 0;
    int side = 0;           // SlotSide: 0 left, 1 right
    std::string glyphId;    // a word's NAME, or a page's (Insert/Fill/Wrap/Mod)
    bool copy = false;      // ctrl held: Move duplicates instead
    int mag = 1000;         // SetMagnitude: the new magnitude, per-mille
    int trigger = 0, every = 0, delay = 0;   // SetTiming (SpellTrigger order)
  } graphEdit;
  // A body part clicked in the health inspector with a sentence on the
  // stack: cast it with `self` resolving AT that part (docs/
  // PLAN_magic_grammar.md §7 — `fire self` on a bleeding stump). The panel
  // says only WHICH slot; main.cpp turns it into a position.
  struct CastAtPartIntent {
    bool pending = false;
    int slot = -1;             // UIState::BodySlot
  } castAtPart;
  // ...and with a filled VESSEL chosen on the FLASKS row instead (game/
  // container.h; `activeVessel` below): the portrait becomes a BRUSH. Left button held over the
  // body pours where the cursor is (MobSystem::PourOnBody); right-drag
  // orbits, middle-drag pans, the wheel (or [ ]) sizes the brush and
  // ctrl+wheel zooms. The panel
  // reports only WHERE on the picture, normalized like projMin/projMax;
  // main.cpp owns the portrait camera and turns it into a ray, and mirrors
  // the answer back as the cursor ring. `applyText` is what the flask holds
  // ("water 64/128"), empty when the chosen slot is no filled vessel -- and
  // then the portrait is not a brush.
  struct PourBrushIntent {
    bool hover = false;    // the cursor is over the portrait: pick for the ring
    bool active = false;   // ...and the left button is down: pour
    float uv[2] = {0, 0};  // portrait-normalized, (0,0) top-left
  } pourBrush;
  float pourRadius = 0.5f;  // world voxels; brush disc on the skin
  // What that size drains, cells a second (container.h PourBrushCellsPerSec),
  // mirrored by main.cpp so the readout is the number the tick spends by.
  float pourDrainPerSec = 0.0f;
  bool pourCursorValid = false;       // main.cpp: the ray hits the body
  float pourCursorUV[2] = {0, 0};     // where it hit, portrait-normalized
  float pourCursorR = 0.0f;           // brush radius, fraction of portrait width
  std::string applyText;
  uint32_t applyColor = 0;   // the substance's own colour, for the hover frame
  // WHICH FLASK IS IN USE on your own body: a pack or hotbar slot, picked by
  // clicking it on the character screen's FLASKS row and put back with the
  // row's button. The panel owns the choice; main.cpp clears it when the slot
  // it names stops holding a vessel (moved, dropped, swapped for a sword).
  KitRef activeVessel{};
  // The chosen flask is stoppered: the brush is up but pours nothing (the
  // pour refuses a stoppered vessel, session.cpp), so the portrait says why.
  bool applyStoppered = false;
  // ...and whether what pours out of it COATS anything (a body-coat stain: the
  // item stage pours only those; sand in a flask does not stick to a blade).
  bool applyCoats = false;
  // ---- THE ALCHEMY BENCH (game/alchemy_bench.h) ----------------------------
  // A 2D cross-section of one vessel with its contents simulated; another
  // vessel can be brought in and tilted to pour, and a stick stirs. It takes
  // the spellbook's column while it is open (the character stays on the
  // left). Mirror in, intent out, like the rest of the screen: main.cpp owns
  // the bench and draws its picture into `alchemy.tex`; the panel draws it,
  // reports the pointer over it in SIM pixels, and raises the latches.
  struct AlchemyUI {
    // ---- mirror ----
    bool open = false;
    uint64_t tex = 0;            // ImTextureID of the bench picture
    int texW = 0, texH = 0;      // the texture's full size
    int tableW = 0, tableH = 0;  // the VISIBLE table (the box), sim pixels
    // The sim grid's height (top-left gridH rows of tex): the table plus the
    // HEADROOM a lifted flask goes up into, drawn above the box. >= tableH.
    int gridH = 0;
    int liftH = 0;               // the least grid height (AlchemyBench::kLiftH), sim pixels
    int minW = 0;                // the least table width (AlchemyBench::kMinW), sim pixels
    bool texReady = false;       // copied at least once (safe to sample)
    struct Portion {
      std::string name;
      uint32_t color = 0;        // 0xAABBGGRR
      int eighths = 0;
      bool dissolved = false;    // in solution: rides the liquid, takes no room
    };
    // Every vessel you carry, and whether it is on the table. Click one to
    // put it on / take it off.
    struct Row {
      KitRef ref;
      std::string label;         // "flask: water 12, oil 3"
      KitSlotUI slot;            // for the icon (live contents while benched)
      bool onTable = false;
    };
    std::vector<Row> rows;
    // The vessel under the hand (or last picked up): its contents, heaviest
    // first, and its capacity.
    std::string focusName;
    std::vector<Portion> focusParts;
    int focusCap = 0;
    // Its devices (the chemistry tools): stopper in, burner lit, how hot the
    // glass is (0..1) and, stoppered, the gas pressure (FlaskSim::Pressure).
    bool focusStoppered = false, focusBurner = false;
    float focusHeat = 0.0f, focusPressure = 0.0f;
    std::string message;         // the last thing the bench said
    // ---- panel-owned ----
    int tool = 0;                // 0 hand, 1 stirring stick, 2 stopper, 3 burner
    bool shockReq = false;       // Electrify pressed (consumed by main.cpp)
    // What the panel measured last frame, for sizing the next table: the
    // picture's room in screen pixels and the integer scale it draws at.
    float areaW = 0.0f, areaH = 0.0f;
    float roomH = 0.0f;          // the picture's bottom edge, px from the screen top
    int scale = 3;
    // ---- input, written by the panel every frame ----
    bool over = false;
    float atX = 0.0f, atY = 0.0f;   // sim pixels, y up
    bool down = false, pressed = false;
    float tiltReq = 0.0f;           // radians requested this frame
    // ---- latches ----
    bool wantOpen = false;       // double-click on a vessel slot
    KitRef openRef{};
    bool wantClose = false;      // "done", Esc, or the screen closing
    bool wantToggle = false;     // a row clicked: on / off the table
    KitRef toggleRef{};
  } alchemy;
  // ---- THE ITEM STAGE (ui/item_stage.h, game/itemstage.h) -------------------
  // One weapon or armour piece, alone, turned in the hand: double-click it in
  // the pack, the hotbar or on the body. It takes the spellbook's column like
  // the bench. With a filled vessel chosen on the FLASKS row the left button
  // splashes it onto the voxels under the brush (the tick does it, from the
  // ray main.cpp builds; the panel only says where). Mirror in, intent out.
  struct ItemStageUI {
    // ---- mirror ----
    bool open = false;
    uint64_t tex = 0;            // ImTextureID of the picture
    int texW = 0, texH = 0;      // the texture's full size
    int imgW = 0, imgH = 0;      // the drawn picture (top-left of tex), stage pixels
    bool texReady = false;
    std::string name;            // "sword", "leather tunic"
    std::string kindText;        // "melee", "worn: chest"
    std::vector<std::string> shellNames;   // a worn piece's parts; empty for a held item
    struct Coat {
      std::string name;
      uint32_t color = 0;        // 0xAABBGGRR
      int voxels = 0;
      float frac = 0.0f;         // of the item's voxels
    };
    std::vector<Coat> coats;     // heaviest coverage first
    int voxels = 0;              // the shell's voxel count
    std::string where;           // "in your right hand", "in the pack"
    bool frozen = false;         // in the pack/hotbar: its coat does not dry there
    // The brush ring, in stage pixels, when the cursor is on the item.
    bool cursorValid = false;
    float cursorPx[2] = {0, 0};
    float cursorR = 0.0f;
    float cellPx = 1.0f;         // stage pixels per lattice cell
    // ---- panel-owned ----
    int shell = 0;
    float yaw = 0.45f, pitch = -0.32f, zoom = 1.0f;   // itemstage::View
    bool resetView = false;      // consumed by main.cpp (the defaults live there)
    float radius = 2.0f;         // brush radius, lattice cells
    // ---- input, written by the panel every frame ----
    bool over = false;
    float at[2] = {0, 0};        // stage pixels
    bool paint = false;          // left button held with a vessel chosen
    // ---- latches ----
    bool wantOpen = false;
    KitRef openRef{};
    bool wantClose = false;      // "done", Esc, or the screen closing
    // ---- where it is on screen, for a harness's mouse (uiRects) ----
    float imgX = 0, imgY = 0;    // the picture's top-left, screen pixels
    float imgScale = 1;          // screen pixels per stage pixel
    float areaW = 0, areaH = 0;  // the room the panel has for it, screen pixels
  } itemStage;
  // ---- WHERE THINGS ARE DRAWN, for a look-iteration harness's mouse --------
  // Off in the game. When `recordRects` is set (--shot-stage), every item slot
  // and the stage's buttons record their screen rectangle here by widget id
  // ("bag3", "eq1", "flask1_2", "##stagenext"), refreshed each frame, so the
  // harness clicks where a hand would and drives the panels' own input.
  struct UiRect {
    std::string id;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  };
  bool recordRects = false;
  std::vector<UiRect> uiRects;
  void RecordRect(const char* id, float x0, float y0, float x1, float y1) {
    if (!recordRects) return;
    for (UiRect& r : uiRects)
      if (r.id == id) { r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1; return; }
    uiRects.push_back(UiRect{id, x0, y0, x1, y1});
  }
  const UiRect* FindRect(const std::string& id) const {
    for (const UiRect& r : uiRects)
      if (r.id == id) return &r;
    return nullptr;
  }
  // THE THROW'S WIND-UP, 0..1 while Q is held with a throwable vessel in
  // hand, -1 otherwise (game/container.h ContainerThrowCharge). Written by the
  // tick; the HUD draws the meter under the crosshair, shaking at full.
  float throwCharge = -1.0f;
  // THE SHOCK (MobSystem::ApplyShocks; wave 2 package E). Written by the tick
  // (TickAuthority's stun pass) for the HUD's pixel cue under the crosshair:
  // the stun's ticks left (0 = not stunned) and how many ticks ago this body
  // last took current (-1 = never). The cue: a jagged pixel bolt, STUNNED and
  // a pip per remaining half-second, plus an edge flash for a few ticks after
  // a jolt.
  int stunTicksLeft = 0;
  int shockTicksAgo = -1;
  // THE SUMMONER'S DEMON (game/demon.h DemonTick; docs/PLAN_demons.md D1):
  // the most recent demon this player called up and whether a salt circle
  // holds it -- 0 none, 1 CONTAINED (the circle's radius, metres), 2 UNBOUND.
  // Written by the tick; the HUD draws a small pixel tab for it.
  int demonState = 0;
  std::string demonName;
  float demonRadiusM = 0.0f;
  // D3 (game/demon_seals.h FillHud): while CONTAINED with a band reading, the
  // circle's STRENGTH against the demon's POWER, which channels the seal piles
  // sever (bit per demon::Channel: move, cast_out, blink, touch), and the gaze
  // strain (0..1; `demonGazeHold` = the demon wants your eyes, else averted;
  // `demonGazeBroken` = you are breaking that rule this tick). demonState 3 =
  // RELEASED (the binding held when you let it out).
  bool demonHasBinding = false;
  int demonStrength = 0, demonPower = 0;
  uint8_t demonSevered = 0;
  float demonStrain = 0.0f;
  bool demonGazeHold = false, demonGazeBroken = false;
  // D5 (game/demon_talk.h ContractTick): what the bound demons of this player
  // are doing -- one row each for the HUD tab: name, contract, the duty in
  // hand ("" = standing), the term left in seconds (-1 = indefinite) and the
  // mana it reserves.
  struct BoundDemonUI {
    std::string name, contract, duty;
    int termLeftS = -1;
    int upkeep = 0;
    bool contained = false;   // bound but not yet released
  };
  std::vector<BoundDemonUI> boundDemons;
  // THE DEMON CONVERSATION'S STRIP (ui/contract_ui.cpp DrawDemonTalkStrip),
  // drawn over the dialogue panel while the speaker is a contained demon.
  // Written by the tick (ContractTick); the latches (`presentPick`,
  // `lookAwayToggle`) are the panel's and main.cpp turns them into the
  // command (TB_DEMON_PRESENT / TB_DEMON_LOOKAWAY).
  struct DemonTalkUI {
    bool active = false;
    uint64_t mobId = 0;
    std::string name;
    int strength = 0, power = 0;
    bool bound = false;           // bound under `contract` (weight `weight`)
    std::string contract;
    int weight = 0, upkeep = 0;
    bool lookingAway = false;     // the command's gaze this tick
    float strain = 0.0f;
    bool gazeHold = false, gazeBroken = false;
    bool pickerOpen = false;      // `present_contract` asked for the picker
    struct Offer {
      std::string name;
      uint32_t hash = 0;
      int weight = 0;
      bool stock = false;
      bool compiles = true;
    };
    std::vector<Offer> offers;    // what may be presented, with its weight
    std::string lastResult;       // the last presentation's answer
    uint32_t presentPick = 0;     // latch: a page's hash, 0 = none
    bool lookAwayToggle = false;  // the strip's button: look away until clicked again
    bool closePicker = false;     // latch: the picker's "back"
  } demonTalk;
  // THE CONTRACT EDITOR (ui/contract_ui.cpp DrawContractEditor), opened from
  // the spellbook's header. A mirror of the player's pages and the stock
  // ones (main.cpp copies them in while nothing is being edited), the draft
  // being written, and one operation latch main.cpp applies to the player's
  // PlayerCaster::contracts.
  struct ContractEdUI {
    bool open = false;
    std::vector<contract::Page> pages;   // the player's own
    std::vector<contract::Page> stock;   // read-only
    int selected = -1;                   // index into pages, or -2 - stock index
    contract::Page draft;
    bool dirty = false;
    bool advanced = false;               // selectors, triggers, counters shown
    enum Op : uint8_t { None = 0, Save, Delete } op = None;
    std::string opName;                  // Delete: the page; Save: the draft's old name
    std::string status;                  // what the last op said
    int upkeepPower = 20;                // the power the upkeep line is shown for
    std::string upkeepFor = "an imp";
  } contractEd;
  // A HELD VESSEL'S MODE, PER HAND (dual wielding; sim/tickinput.h
  // TB_SCOOP/TB_APPLY and their _L twins): 0 pour, 1 scoop, 2 apply. F
  // cycles the vessel in the hand last used; the hand's own button (LMB
  // right, RMB left) then does it. `vesselModeShown` is the mode of a vessel
  // actually in that hand with the hands up this frame, -1 for none (the HUD
  // hand slots). `applyShown` = the acting vessel is in apply mode (the HUD
  // tag); `applyTarget` names who it would brush right now, "" for nobody.
  int vesselMode[2] = {0, 0};
  int vesselModeShown[2] = {-1, -1};
  bool applyShown = false;
  std::string applyTarget;

  // What the last refused action said, and how long ago. Flashed under the
  // panel rather than swallowed: a slot that silently declines is the failure
  // mode that makes an inventory feel broken (game/equipment.h MoveResult).
  std::string kitMessage;
  float kitMessageAge = 1e9f;   // seconds; main.cpp ages it

  // The avatar's active dismemberment state ("normal", "crawling", "limping"),
  // from AvatarLocomotion::stateName. One line, and it says more about a pair
  // of lost legs than any number of bars.
  std::string locoState;

  // ---- F1 -> World -> References (ui/refs_ui.cpp, world/refs.h) ----------
  // The map's reference store, owned by main(); null = the page says there is
  // none (a harness run). The page EDITS it through refs::Place / Move /
  // SetProp / SetField / Delete, which write the group JSON file -- authored
  // data, like the tuner's JSON editors -- and the next tick re-applies.
  refs::RefStore* refs = nullptr;
  // Every item name the library knows (the container contents editor's
  // searchable list), mirrored by main.cpp.
  std::vector<std::string> itemLibraryNames;
  // World voxels -> screen pixels through the MAIN camera, or false when the
  // point is behind it. Installed by main.cpp (which owns the camera
  // convention); the References page draws a door's swing arc with it.
  std::function<bool(const float world[3], float screen[2])> projectWorld;
  // "fly to" on a ref: main puts the player in fly mode a few metres off it,
  // looking at it. One-shot.
  bool refFlyTo = false;
  float refFlyPos[3] = {};
  // R also reloads the map's refs (main.cpp's reloadMaterials path).
  // P7 "jump clock to ..." (the npc inspector): minutes after midnight, -1 =
  // none. main.cpp moves the celestial clock there (the sky, the day phase,
  // every schedule and the dialogue `time` condition follow) -- a dev tool,
  // like the time slider, and it moves the world hash the same way.
  int jumpClockMinute = -1;
  // What the schedule clock reads right now (-1 = unknown), mirrored by main
  // for the npc inspector's "now" line; and whether the day is frozen by
  // tuning (dayNight.freeze), in which case a jump does nothing.
  int clockMinute = -1;
  bool clockFrozen = false;
  // P4: "place structure here" -- while the page is choosing, the footprint
  // box it would occupy (world voxels, inclusive) is drawn in the world as a
  // wireframe (main.cpp's debug-box list), plus a small box on its front
  // door side. Written by the page every frame it is open; main clears it.
  bool structPreview = false;
  int structPreviewLo[3] = {}, structPreviewHi[3] = {};
  int structPreviewFront[3] = {};
  // "open in editor" (P5 hands the structure to the in-game editor). Until
  // P5 lands the page says so; the request is kept here for it to take.
  std::string structOpenRequest;
  // The last live re-apply's one-line result (main.cpp), for the page.
  std::string structReapplyStatus;
};

// P5: the F1 material picker and the "current material" line, for the
// in-game editor (ui/editor_ui.cpp). `ids` = the candidates (a structure can
// only hold ids 1..255).
bool OverlayMaterialPicker(const UIState& s, const char* id, const std::vector<int>& ids,
                           int& selected, char* search, size_t searchN, float maxH);
void OverlayCurrentMaterial(const UIState& s, int id, const char* what);

class Overlay {
 public:
  // `assetDir` is where assets/ui/chrome.{bmp,json} live. A missing or broken
  // chrome file is NOT a failure: the screen falls back to flat rectangles
  // with identical layout, and the reason is printed once.
  bool Init(GLFWwindow* window, const rhi::Device& device,
            rhi::TextureFormat format, const std::string& assetDir);
  void BeginFrame();
  // A LOOK-ITERATION HARNESS'S MOUSE (--shot-stage). Called inside BeginFrame
  // after the platform backend has queued the real input and before ImGui
  // reads it, so whatever it queues is the last word this frame: the panels
  // see a click exactly as a hand's click arrives -- hover, double-click
  // timing and drag included -- and no panel has a test-only path. The
  // argument is ImGuiIO*, untyped so this header stays imgui-free. Null in
  // the game.
  std::function<void(void* io)> injectInput;
  // What an injector queues, on the ImGuiIO it was handed: the pointer at
  // (x, y), then optionally one button edge (button 0 left / 1 right, -1 none)
  // and a wheel step. `dblClickSec` > 0 widens the double-click window (a
  // harness frame can be slower than a hand).
  static void QueueMouse(void* io, float x, float y, int button, bool down,
                         float wheel, float dblClickSec);

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
  // The F1 sidebar's pages (Draw() owns the frame, header and page strip).
  void DrawDevPaint(UIState& s);
  void DrawDevSpawn(UIState& s);
  void DrawDevWorld(UIState& s);
  void DrawDevView(UIState& s);
  void DrawDevMagic(UIState& s);
  void DrawDevDebug(UIState& s);
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
