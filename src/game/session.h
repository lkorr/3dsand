// session.h — THE AUTHORITY / PRESENTATION BOUNDARY
// (docs/PLAN_multiplayer_now.md package N5, audit §S4/A3).
//
// Before this file, ONE PLAYER WAS A SET OF LOCAL VARIABLES IN main(). The
// camera, the controller, the brush, the avatar, the spell caster, the melee
// state, the sheath, the kit, the grab, the grenades in flight and thirty
// other things were declared side by side in main() and read by a 2,400-line
// tick body that also lived in main(). Nothing was wrong with any of it as
// code; what was wrong is that "a second player" had no shape. You could not
// name the thing that would have to be duplicated, and you could not tell,
// looking at the tick body, which of the ninety names it touched were the
// player's, the world's, or the window's.
//
// This file names them. Four types and one function:
//
//   PlayerSession  — everything that is ONE PLAYER'S. One instance today; a
//                    second could be constructed right now without a compile
//                    error and without touching a global, which is the whole
//                    acceptance criterion for this package.
//   TickAuthorityCtx — everything a tick of authority needs that is NOT one
//                    player: the engine systems (by reference), the loaded
//                    content (by reference), the per-WORLD tick scratch (owned
//                    here, because it used to be more main() locals), and —
//                    named and separated — the PRESENTATION seam the tick body
//                    still writes to.
//   OpBatch        — the tick's op vectors, owned by the caller. A server loop
//                    forwards them; the frame loop submits them.
//   FrameIntent    — what the frame layer settled before the tick.
//   TickAuthority  — one tick. The frame loop calls it; a `--frames` run calls
//                    it; a future headless server loop calls it.
//
// ============================ THE RULE ====================================
//
// PER-PLAYER STATE LIVES IN PlayerSession. SIM-AFFECTING PROCESS GLOBALS MUST
// NOT BE KEYED ON THE RESIDENCY WINDOW ORIGIN.
//
// The first half is enforceable by reading: if a new thing is one player's,
// it goes in the struct below, not in main(). The second half is the one that
// bites, because it is invisible until there are two windows. The process
// globals the sim reads are CurrentTuning(), CurrentWorldMap(), WaterBodies(),
// WorldEditLayer(), Celestial(), WindPrims() and CurrentPrims(). All but one
// are keyed on the WORLD; WaterBodies() is keyed on the residency window
// origin, which means it answers a different question for a second player
// standing 600 voxels away. That is the grandfathered exception and it is
// tracked in docs/RESEARCH_multiplayer_readiness.md S4 — do not add a second
// one.
//
// ==========================================================================
//
// WHAT THIS PACKAGE DID NOT DO, ON PURPOSE. main() still holds an ALIAS
// reference per PlayerSession member (`Camera& cam = session.cam;` and so on).
// The presentation half of main() — the render block, the character screen,
// the dev panel, shutdown — makes thousands of references to those names, and
// rewriting them to `session.cam` would have been a 600-site rename inside the
// same commit that moves a 2,400-line tick body, with a byte-identical op
// record as the only oracle. The aliases are a migration seam and nothing
// more: the STATE is in the object, so a second session compiles, and the
// names are the next package's mechanical rename.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/avatar.h"
#include "game/brush.h"
#include "game/camera.h"
#include "game/caster.h"
#include "game/corpses.h"
#include "game/equipment.h"
#include "game/grab.h"
#include "game/item.h"
#include "game/melee.h"
#include "game/mob.h"
#include "game/persist.h"
#include "game/player.h"
#include "game/prefab.h"
#include "game/remoteplayer.h"
#include "game/spell.h"
#include "game/strike_pick.h"
#include "game/thirdperson.h"
#include "gpu/context.h"
#include "gpu/passtimer.h"
#include "math3d.h"
#include "measure/perfscope.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/farfield.h"
#include "sim/materials.h"
#include "sim/microbody.h"
#include "sim/simulation.h"
#include "sim/stream.h"
#include "sim/tickinput.h"
#include "sim/world.h"
#include "ui/overlay.h"

// ---- small per-player value types, moved out of main() --------------------

// Thrown bouncing bomb — the first CPU gameplay projectile (DESIGN.md §8).
// Float math is fine here: the grid only ever sees the ExplosionOp it emits,
// which travels through the deterministic MutationQueue path.
struct Grenade {
  Vec3 pos, vel;  // voxel units, voxels/s
  float fuse;     // seconds
};
// Integrate one 30 Hz tick against the voxel mirror. Returns true on detonate.
bool UpdateGrenade(Grenade& g, float dt, const Player::KindFn& kindAt);

// Where a spell resolved, for the renderer: a short-lived burst of sprites
// (SpellEmission::impacts). Render-only, counted down per TICK so the flash
// lasts the same world-time at any frame rate.
struct SpellFlash {
  Vec3 at;
  uint32_t color;
  float radius;
  int ttl, ttl0;
};

// A combat cue raised inside the tick loop cannot be played there: audio
// drains once per frame and the tick loop may have run four times or none. So
// each cue is a one-shot request with PEAK-HOLD ON POWER — a frame in which
// the blade crossed a body on three consecutive ticks is ONE blow to the ear,
// and it should be the hardest of the three rather than three overlapping
// copies of nearly the same sample. `at` travels with it because a killing
// blow can despawn its victim before the frame drains.
struct CombatCueRequest {
  bool pending = false;
  float power = 0.0f;  // 0..1
  Vec3 at{};
};

// THE HIT-STOP DIP. `pendScale`/`pendMs` are written INSIDE the tick loop,
// peak-held so a sever landing in the same frame as a chip keeps the sever's
// dip, and drained by the FRAME loop once, at the top, before the accumulator
// fills — which is why it is presentation state and not the session's.
//
// PEAK-HOLD IS min ON THE SCALE AND max ON THE DURATION. They are peaks of the
// same quantity — "how much stop" — expressed in units that run opposite ways,
// and combining them with the same operator is how you get a sever that stops
// hard for 55 ms because a chip landed in the same frame.
struct HitStop {
  float pendScale = 1.0f;  // requested this frame; 1 = nothing requested
  float pendMs = 0.0f;
  float timeLeft = 0.0f;  // REAL seconds remaining in the live dip
  float scale = 1.0f;     // the live dip's multiplier
  void Request(float s, float ms) {
    if (ms <= 0.0f || s >= 1.0f) return;
    pendScale = std::min(pendScale, s);
    pendMs = std::max(pendMs, ms);
  }
};

// ---- A BODY REDUCED TO WHAT THE PANEL NEEDS ---------------------------------
//
// One oriented collider box per limb, tagged with the figure slot it belongs
// to, in world voxels. Framing the portrait, framing ONE limb, and outlining
// limbs on the image are three readings of this same list — and it is a plain
// list of boxes rather than three walks of the rig for one reason: THE DEAD
// HAVE NO RIG. After Mob::Die() every PartBody() is 0, so a walk answers
// nothing, while a list of boxes captured at the instant of death goes on
// answering all three questions for as long as the death screen holds it. The
// live rig and the death snapshot therefore reach the camera through one type,
// and the panel cannot behave differently on the two.
//
// Here rather than in main.cpp because DeathBody holds one, and DeathBody is
// the presentation seam the tick body still writes to.
struct LimbBoxes {
  struct Box {
    int slot = -1;  // BodySlotFor(); -1 = a part with no figure slot
    Vec3 c[8];
  };
  std::vector<Box> boxes;
  bool Empty() const { return boxes.empty(); }
};

// THE PHOTOGRAPH THE DYING OBSERVER TOOK. The body's POSE at the instant it
// died — every limb's world transform plus its collider box — so the death
// screen can be orbited, zoomed and read ten seconds later while the limbs
// themselves go on burning, sinking or rolling downhill as debris. The
// portrait pass overwrites the live GPU transforms with these before it draws
// and puts the real ones back after.
//
// PRESENTATION, not the session's: it is one window's readout of one death,
// and a headless server has no portrait to freeze.
struct DeathBody {
  // One frozen limb: the physics handle it will be carrying as debris
  // (AdoptBody keeps the handle, which is the whole reason this can be
  // matched up again) and the transform it had when it was still yours.
  struct Limb {
    uint64_t body = 0;
    BodyXformGpu xf{};
  };
  bool have = false;
  float yaw = 0.0f;         // the body's facing, so the orbit keeps a front
  std::vector<Limb> limbs;  // the GPU transform override
  LimbBoxes boxes;          // framing, limb focus and the outlines
};

// Owner handle for wind primitives placed from the DEV PANEL, so "clear all"
// retires those and nothing else. A spell's gust owns itself (the casterId) and
// expires on its own TTL; the panel has no business reaching into gameplay.
// Shared because the panel writes it and the tick body honours it.
constexpr uint64_t kDevFanOwner = 0xDEFA11Au;

// Where the beam LEAVES the caster, in world voxels.
//
// It must clear the avatar's own head. The eye sits inside the skull part, so
// a damage ray cast from there hits the caster on its first tick and the
// dismemberment path takes their head off before the beam reaches anything —
// which is exactly what "the laser kills me instantly" was. Forward of the
// face and down-right of the eyeline, so it reads as fired from the hand.
//
// ONE definition on purpose: the damage ray (the tick) and the beam sprites
// (the render block) both call this. They used to carry separate offsets, so
// the visible beam came from the hand while the lethal one came from the
// middle of the player's face.
Vec3 LaserMuzzle(const Player& player, const Camera& cam);

// Which figure slot a rig part belongs to, for the character screen and for a
// spell aimed at a body part. A pure function of the authored part name and
// tag; lives here rather than in main.cpp because both the tick body and the
// UI ask, and a second copy is a second thing to keep in step.
int BodySlotFor(const char* name, const char* tag);

// ---- the tick's ops -------------------------------------------------------

// The op vectors ONE TICK produces, owned by the caller rather than declared
// inside the tick body. That is the shape a server loop needs: the batch is
// what would go on the wire, and TickAuthority filling a caller's struct is
// the difference between "the tick submitted something" and "the tick produced
// something the caller decides what to do with".
//
// FIVE VECTORS, NOT SIX. The plan says six; the sixth (gas spawns) is queued
// inside World by the producers themselves and joins at SubmitTick, so there
// is no vector here for the frame layer to own. Recorded rather than faked
// with an always-empty member.
struct OpBatch {
  std::vector<BrushOp> ops;
  std::vector<ExplosionOp> exps;
  std::vector<CellOp> cells;
  std::vector<ParticleSpawn> spawns;
  std::vector<FluidSpawnOp> fluid;
  // Cleared, not reconstructed: the capacity is worth keeping across ticks and
  // the values are identical either way.
  void Clear() {
    ops.clear();
    exps.clear();
    cells.clear();
    spawns.clear();
    fluid.clear();
  }
};

// WHERE A SESSION THAT OWNS NO WINDOW PUTS ITS PRESENTATION (M9.1 P1).
//
// The tick body writes a death screen, a hit-stop dip, a frozen death pose, a
// refused-spell counter and five combat cue requests. Every one of them is ONE
// WINDOW'S readout, and every one of them used to be written unconditionally
// because there was only ever one player and it was the window's.
//
// A second session on this machine — a peer's ghost (M9.2), a scripted second
// player in a gate — must not reach any of them: its death would black out
// YOUR screen and its sword would dip YOUR frame clock. Rather than guard
// forty call sites, the tick BINDS THE NAMES somewhere harmless for such a
// session (session.cpp, SV_SEAM_REFS_PLAYER). Nothing reads what lands here;
// it exists so the writes have a legal address.
//
// It has to PERSIST, which is why it hangs off the session rather than off the
// tick's scratch: the death/respawn block reads back the `deathScreen` flag it
// set on an earlier tick to decide whether the respawn hold has started.
struct PresentationSink {
  UIState ui;
  HitStop hitStop;
  DeathBody deathBody;
  bool deathFrozen = false;
};

// ---- one player -----------------------------------------------------------

// The body of PlayerSession::PrefetchAround, as a free function over a
// POSITION and a CURSOR rather than over a session.
//
// Split out because the `two-players` gate drives two bare `Player`s and two
// `PlayerAvatar`s with explicit ids (a PlayerSession's avatar is constructed
// with the default id and the gate needs 0x5A11ED and 0x5A11ED+1), so it has
// no PlayerSession to call the member on. A gate that re-implemented the
// prefetch would be asserting on its own copy of it, which is the "a fixture
// that measures itself" trap; one definition, two callers.
inline void PrefetchChunksAround(World& world, Vec3 posVox,
                                 uint32_t& cursor) {
  const IVec3 c{ifloor(posVox.x) >> 4, ifloor(posVox.y) >> 4,
                ifloor(posVox.z) >> 4};
  const uint32_t refresh = cursor % 27u;
  cursor++;
  uint32_t i = 0;
  for (int dz = -1; dz <= 1; dz++)
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++, i++) {
        const IVec3 wc{c.x + dx, c.y + dy, c.z + dz};
        // Already in the cache and not this tick's refresh slot: nothing to
        // ask for. This is the line that keeps the steady-state cost at ONE
        // fetch per tick instead of 27 (inviolable rule 2).
        if (i != refresh && world.Cached(wc) != nullptr) continue;
        world.RequestChunkFetch(wc, World::FetchSource::Mob);
      }
}

struct PlayerSession {
  // ---- who this player is, to the tick ----
  // Index in the span TickAuthority is given. 0 is the PRIMARY: every world
  // phase that needs a player at all (the dev panel's ray, the far-plume eye,
  // the chunk the submit centres on, the tick record's one command) takes
  // session 0, because those are the WINDOW's questions and the window follows
  // the primary.
  int index = 0;
  // Does this player own the window this process is drawing? True for the one
  // session the game constructs. False for a peer's ghost and for a second
  // scripted player in a gate — see PresentationSink above for what that
  // changes, plus one behaviour: a session with no view cannot be asked to
  // respawn, so it revives itself once `avatar.respawnDelay` has elapsed.
  bool localView = true;
  std::unique_ptr<PresentationSink> sink;  // allocated iff !localView
  // The splash edge detector. It was `static bool wasInLiquid` inside the tick
  // body until M9.1 — the one function static in session.cpp, and the exact
  // shape of defect the rule at the top of this file names: two players in two
  // lakes shared one edge and the second one never splashed.
  bool wasInLiquid = false;

  // ---- body and view ----
  Camera cam;
  Player player;
  PlayerAvatar avatar;
  std::string avatarDefName;  // tuning.json player.model, re-read on F5
  ThirdPersonRig tpRig;
  CameraMode camMode = CameraMode::First;
  float avatarHeading = 0.0f;  // body facing, radians about +Y
  float fovNow = 0.0f;
  float respawnTimer = 0.0f;
  // Kinematic capsule proxy so debris collides with (and is shoved by) the
  // player; terrain collision stays in the AABB controller.
  uint64_t playerBody = 0;
  // How this player's sweeps read the world. A std::function because a second
  // session would want its own mirror source (player.h's KindFn note). Bound
  // ONCE, before the frame loop, rather than rebuilt as a lambda every frame.
  Player::KindFn kindAt;

  // ---- tools ----
  Brush brush;
  PrefabPlacer placer;
  Inventory hotbar;
  PlayerKit kit;
  SheathState sheath;
  // What we last ASKED the body to wear, per equip slot, and in what dye. Not
  // a second copy of the equipment — it is the record that keeps a REFUSED
  // piece from being retried thirty times a second.
  std::string wearTried[kEquipSlotCount];
  uint32_t wearDye[kEquipSlotCount] = {};

  // ---- looking at things, and looting them (game/corpses.h) ----
  // What the reach ray found this frame (a debris body handle or 0), the
  // corpse the loot panel is open on, and whether E opened the character
  // screen to show it.
  uint64_t lookBody = 0;
  uint64_t lootCorpse = 0;
  bool lootOpenedScreen = false;
  // ---- the physics grab (game/grab.h) ----
  // E IS TWO BINDINGS ON ONE KEY: tapped it is the pickup/loot, held past
  // player.grabHoldTime it lifts whatever the reach ray is on.
  GrabHold grab;
  float eHeld = 0.0f;
  bool eGrabbed = false;
  bool ePrevDown = false;
  std::vector<uint64_t> lookIgnore;  // the avatar's own limbs, per frame

  // ---- magic ----
  SpellSystem spells;
  PlayerCaster caster;
  CasterHealth playerHealth;
  std::vector<SpellFlash> spellFlashes;
  // A body part clicked in the inspector with a sentence on the stack: the
  // slot, or -1. NOT a TickInput field — it is a click in an ImGui panel on a
  // specific rig part, a UI transaction rather than a player command, and it
  // has no meaning on a remote peer.
  int castAtPartQueued = -1;

  // ---- melee ----
  MeleeState melee;
  SwingPhase meleePhasePrev = SwingPhase::Idle;
  StrikePicker strikePicker;
  StrokeCursor playerStrike;
  int strikeQueued = -1;    // style index latched at the press, -1 = none
  int strikeBuffered = -1;  // ONE strike banked mid-swing, fired at recover
  Vec3 lastEdgeBase{}, lastEdgeTip{};
  bool lastEdgeValid = false;
  // The STRIKE aim ray's ignore list. Its own vector rather than a share of
  // `lookIgnore`: that one is filled in the frame block and read by the E
  // prompt, and the melee ray runs inside the tick loop, where overwriting it
  // would silently change what E offers.
  std::vector<uint64_t> strikeIgnore;
  // Who the player's stroke struck this tick, and whether a bite landed on
  // them. Per-player because they are one stroke's readout.
  std::vector<uint64_t> playerStruck;
  bool playerBitten = false;
  CombatCueRequest combatWhooshCue, combatFleshCue, combatClangCue,
      combatStrikeCue, combatCutCue;
  bool combatStrikeEdged = true;

  // ---- projectiles ----
  std::vector<Grenade> grenades;

  // ---- the frame layer's command accumulator (package N2) ----
  // The frame layer WRITES held state and the axes wholesale every frame, ORs
  // in every pressed edge it sees, and adds the frame's raw mouse pixels. Each
  // tick CONSUMES one TickInput.
  TickInputFeeder feeder;
  bool captured = true;  // is the cursor grabbed for this session's window

  // ---- M9.1 P3: KEEPING A NON-PRIMARY PLAYER'S GROUND IN THE CACHE --------
  //
  // Only session 0 gets the snapshot's 3x3x3 mirror (the window follows the
  // primary — see `index` above). Every other session reads the world through
  // `World::KindAtCached`, which answers from the on-demand chunk cache and
  // falls back to the analytic terrain column on a miss. The fallback is
  // correct-ish and cheap-ish, but it is only correct where the world still
  // looks like worldgen made it, so the cache has to be kept warm AHEAD of the
  // body rather than filled by the misses the body is already suffering.
  //
  // WHY NOT JUST REQUEST ALL 27 EVERY TICK. `World::RequestChunkFetch`
  // coalesces a chunk that is ALREADY QUEUED, but a chunk that has already
  // landed is dropped from the queue, so a blind 27-per-tick prefetch costs 27
  // of the 64-chunk-per-tick fetch budget forever — it would scale with world
  // residency rather than with activity, which is inviolable rule 2. So:
  //
  //   * every chunk of the 3x3x3 cube that is NOT cached is requested (the
  //     body needs it NOW and the miss path is already paying for it);
  //   * exactly ONE cached chunk is re-requested per tick, round-robin, so the
  //     whole cube refreshes every 27 ticks (0.9 s at 30 Hz) and a cell another
  //     player dug stops being a ghost floor within a second.
  //
  // Steady state on flat ground is therefore ONE fetch per tick per non-primary
  // session, and a session that is standing still costs the same one. The
  // cursor is per-session state, not a process global (DESIGN.md §10).
  uint32_t prefetchCursor = 0;
  void PrefetchAround(World& world) {
    PrefetchChunksAround(world, player.pos, prefetchCursor);
  }
};

// The 'PLYR' save section, built from ONE PLAYER plus the two content
// libraries. See the note over PlayerKitRefs (game/persist.h): the section used
// to be assembled out of whichever main() locals were in scope, and "these five
// belong to the same player" was a fact nobody could check.
inline PlayerKitRefs PlayerKitOf(PlayerSession& s, const GlyphLibrary& glyphs,
                                 const ItemLibrary& items) {
  return PlayerKitRefs{&s.caster, &glyphs, &s.hotbar, &s.kit, &items};
}

// What the FRAME layer decided this frame that the tick has to act under.
// These three are derived from the tool selector, magic mode and the sheath —
// UI state, settled before the tick and passed in rather than re-derived,
// because the sheath is Reconcile'd on the frame and re-deriving here is how
// two reads of one fact stop agreeing.
struct FrameIntent {
  bool brushActive = false;
  // `meleeArmed` means "a weapon is drawn" and is what decides whether to
  // equip one; `meleeReady` is the one the driver, the program and the sweep
  // gate on, because a fist is as live as a sword (it folds in the unarmed
  // compass, which is a CONTENT question the frame layer already resolved).
  bool meleeArmed = false;
  bool meleeReady = false;
  const ItemDef* heldItem = nullptr;
};

// ---- the world a tick runs in ---------------------------------------------

// EVERY REFERENCE MEMBER COMES FIRST, then everything with a default. Not a
// taste call: the caller builds this with designated initializers, C++20 wants
// them in declaration order, and a reference has no default to skip. So the
// groups below are ordered by what the language needs and labelled by what
// they mean.
struct TickAuthorityCtx {
  // ---- A. the engine, by reference. One per process today; one per WORLD in
  // the shape this is heading for.
  GpuContext& ctx;
  World& world;
  Simulation& sim;
  Stream& stream;
  FarField& far;
  Physics& phys;
  MobSystem& mobs;
  DebrisSystem& debris;
  MicroBodySet& mbSet;

  // ---- B. the loaded content, by reference. Hot-reloaded on R, so these are
  // references to main()'s owners rather than copies.
  const std::vector<MaterialDef>& mats;
  const GlyphLibrary& glyphs;
  const ItemLibrary& items;
  const std::vector<Prefab>& prefabs;
  // Material COLLISION class LUT for the mirror queries. Not raw klass:
  // BuildCollisionClasses remaps passable vegetation to gas.
  const std::vector<uint32_t>& classOf;

  // ---- C. THE PRESENTATION SEAM. Everything here is the WINDOW's, not the
  // world's, and the tick body still touches it. Named and grouped so the next
  // package knows exactly what to cut: a headless server would pass a dummy
  // UIState and dead timers, and nothing in section D would notice.
  UIState& ui;
  HitStop& hitStop;  // a hit REQUESTS a dip; the frame loop ages it
  DeathBody& deathBody;
  bool& deathFrozen;
  bool& liveTimed;  // telemetry: are per-pass GPU timings armed
  PassTimer& liveTimer;
  sandvox::PerfSample& liveSample;
  uint32_t& liveFrameNo;
  float& tickMsSmooth;
  // --frames far-field tally: entries dispatched over the run, ticks that
  // dispatched at least one, and the largest single tick's count. Harness
  // telemetry the frame layer prints at exit.
  uint64_t& farEntries;
  uint64_t& farTicks;
  uint64_t& farBiggest;

  // ---- D. per-WORLD tick scratch, OWNED HERE. Every one of these was a
  // main() local; none of them is one player's, and a second session must not
  // need a second copy.
  int labScene = -1;  // --lab scene index, or -1
  std::unordered_map<uint32_t, MicroBodyRef> sphereModels;  // material -> model
  std::vector<uint64_t> aiSpawnedMobs;  // what the AI panel put in the world
  bool everExploded = false;
  uint32_t lastExplosionTick = 0;
  // The CONSERVATIVE MLS-MPM live estimate the CPU owns (world.h fluid block).
  uint32_t fluidCount = 0;
  std::vector<std::pair<uint32_t, uint32_t>> fluidPendingSpawns;
  uint32_t fluidCueMat = 0;
  uint32_t lastFluidTick = 0;
  // Material id each MPM species splashes micro droplets as
  // (TickParams.fluidSplashMat).
  uint32_t fluidSpeciesMat[4] = {0, 0, 0, 0};
  uint32_t labTick = 0;
  bool duelDummySpawned = false;
  // Wall clock, for the render-only ripple timestamps. A VALUE the frame layer
  // rewrites each frame, not a reference: `now` is a frame-loop local, and a
  // reference to it would be the frame layer keeping state for the authority.
  double frameTime = 0;

  // ---- E. THE SCRIPTED-FLIGHT / MEASUREMENT HARNESSES, AS HOOKS. Every one
  // of these is argv state the FRAME layer owns, supplied as a callback so
  // game/ does not link against main.cpp's globals. All null in the game and
  // on a server.
  //
  // --autofly-surface pins the altitude analytically and --autofly-park stops
  // the flight; both have to apply BETWEEN player.Update and everything that
  // reads the position.
  std::function<void(Player&, uint32_t tick)> afterPlayerUpdate;
  // --fell-tree: the tree-fell gate's cut driven from the live frame loop. It
  // reads g_frameMs — the frame layer's own whole-frame ring — which is
  // exactly why it is a callback and not code in this file.
  std::function<void(uint32_t tick, std::vector<CellOp>& cellOps)> fellTree;
  // Respawn out of an open inventory has to hand the cursor back to the
  // window: `captureBeforeUi`, glfwSetInputMode and the cursor-position reset
  // are all the WINDOW's, and there is no window here.
  std::function<void()> restoreCursorAfterUi;
  // --duel-dummy: spawn one armed target three metres ahead, once.
  bool duelDummy = false;

  // ---- F. THE PEERS' BODIES (M9.2 package B, game/remoteplayer.h).
  //
  // Per-WORLD, not per-player: a ghost is somebody else's body standing in
  // THIS world, and both local sessions of a split screen would see the same
  // one. Owned by main() (the frame loop is what polls the socket), borrowed
  // here, and NULL IN EVERY HARNESS — which is the point. Zero ghosts means
  // phases B, H, I and O make no call they did not make before this package,
  // and the one-session `--record-ops` stream is byte-identical to the
  // pre-package oracle. That identity is the acceptance criterion; the
  // guards at each of the four call sites are what buys it.
  RemotePlayers* remotes = nullptr;
};

// ---- one tick of authority, for N players ---------------------------------
//
// Everything between the accumulator loop's braces, minus the pacing, the
// readback pump and the park probe the frame layer owns. Takes ONE command PER
// PLAYER, produces ONE OpBatch for the whole tick, and submits it. No
// behaviour change from the version that lived in main(): the acceptance for
// that claim is a `--frames 600 --autofly-hard` op record that is
// byte-identical before and after (N5 step 5, re-run for M9.1 P1).
//
// ---- WHAT A PHASE IS, AND WHY THE ORDER IS WHAT IT IS ---------------------
//
// The body is cut into sixteen PHASES (session.cpp, PhaseA..PhaseP). A phase
// is a maximal run of the old single-player body with ONE scope: a PLAYER
// phase touches one PlayerSession and runs once per session in index order; a
// WORLD phase touches the world, the engine systems and the per-tick world
// scratch and runs once. TickAuthority is nothing but their alternation,
//
//     A* B C* D E* F G* H I* J K* L M* N O* P      (* = once per session)
//
// and that alternation IS the old body's order, segment for segment. That is
// the whole design constraint: the tick was never "all the player's work then
// all the world's". The controller has to run before the residency window
// recentres on it; the window has to shift before the laser ray reads the
// grid; the dev panel spawns before the brush writes; mobs.PreTick has to see
// the avatar's bleed ops; the submit has to come after every producer. Merging
// the four player runs into one, or the four world runs into one, reorders the
// op stream — and the op stream's order is the tick's identity (CLAUDE.md rule
// 3: the lowest op index owns the cell).
//
// WHERE A REMOTE GHOST PLUGS IN (M9.2 package B — LANDED): phase B (the
// interest set learns its chunk), phase H (SetPlayerActors lists it and the
// avatar list is re-registered beside it), phase I (its avatar's PreTick) and
// phase O (PostStep). Four guarded call sites on `TickAuthorityCtx::remotes`,
// all no-ops when it is null or empty — see game/remoteplayer.h. The package
// text said phase B for the actor list; the actor list has been phase H's
// since P1 and the code is the truth. NOT as a
// SessionTick: no TickInput crosses the wire in the model of record
// (docs/PLAN_multiplayer_m9.md) — the peer is authoritative for its own
// controller and sends the OUTCOME, a PlayerState, from which a ghost's
// `Player` fields are filled and its PlayerAvatar driven. A ghost is a
// RemotePlayer entry on TickAuthorityCtx that those two phases walk beside
// the span, with `localView = false` semantics (nothing it does reaches the
// window's presentation seam).

// ONE PLAYER'S INPUT TO ONE TICK. The session, what the frame layer settled
// for it, and the command it consumes. By pointer rather than reference so the
// span is assignable and a caller can build it in a vector.
struct SessionTick {
  PlayerSession* s;
  FrameIntent intent;
  TickInput ti;
};

void TickAuthority(TickAuthorityCtx& w, std::span<SessionTick> players,
                   uint32_t tick, OpBatch& out);

// ONE PLAYER IS A SPAN OF ONE. Kept because the frame loop and every harness
// fixture read better this way, not because the authority is singular — the
// same reason MobSystem::SetPlayerActor is kept beside SetPlayerActors.
inline void TickAuthority(TickAuthorityCtx& w, PlayerSession& s,
                          const FrameIntent& intent, const TickInput& ti,
                          uint32_t tick, OpBatch& out) {
  SessionTick one{&s, intent, ti};
  TickAuthority(w, std::span<SessionTick>(&one, 1), tick, out);
}
