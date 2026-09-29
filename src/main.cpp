// sandvox — 3D falling-sand voxel engine (v0). See DESIGN.md.
// Fixed 30 Hz GPU simulation, uncapped raymarched rendering, walkable player,
// JSON materials, deterministic kernels with per-tick world hash.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <GLFW/glfw3.h>

#include "audio/cues.h"
#include "game/avatar.h"
#include "sim/trample.h"
#include "sim/plants.h"
#include "game/bodyreg.h"
#include "game/brush.h"
#include "game/persist.h"
#include "game/camera.h"
#include "game/caster.h"
#include "game/alchemy_bench.h"
#include "game/container.h"
#include "game/equipment.h"
#include "game/worlditems.h"
#include "game/corpses.h"
#include "game/dialogue.h"
#include "world/refs_doors.h"
#include "game/dye.h"
#include "game/grab.h"
#include "game/item.h"
#include "game/melee.h"
#include "game/mob.h"
#include "game/spell.h"
#include "game/spellgraph.h"
#include "game/strike_pick.h"
#include "sim/rng.h"
#include "game/player.h"
#include "game/prefab.h"
// THE AUTHORITY / PRESENTATION BOUNDARY (PLAN_multiplayer_now N5): PlayerSession
// is one player, TickAuthority is one tick of gameplay. Read its header before
// adding a per-player local to main().
#include "game/session.h"
#include "game/thirdperson.h"
#include "gpu/context.h"
#include "gpu/resources.h"
#include "gpu/rhi_vk.h"  // rhi::vkr::SetCaptureStats (--shader-stats)
#include "gpu/vk_shader_stats.h"
#include "gpu/vk_smoke.h"
#include "lab/lab.h"
#include "math3d.h"
// M9.2-C: the game talks to ONE peer. net/link.h is the transport (package A),
// net/protocol.h the message set and the lockstep pacer; the peer's BODY is a
// RemotePlayer ghost (package B), reached through game/session.h which already
// includes game/remoteplayer.h.
#include "net/link.h"
#include "net/chunksync.h"
// M9.4-D: the per-tick ENTITY traffic — interest, handoffs, the one envelope
// the ten record kinds ride in. Pure decision layer; this file does the
// sending, the same split chunksync.h keeps.
#include "net/entitysync.h"
#include "net/opsync.h"
#include "net/protocol.h"
#include "net/storesync.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/farfield.h"
#include "sim/celestial.h"
#include "sim/materials.h"
#include "sim/solutes.h"
#include "sim/oprecord.h"  // the op record carries the tick command (N2/N3)
#include "sim/microbody.h"
#include "sim/microvox.h"
#include "sim/pagetable.h"  // PagesHighWater for the --frames pool-margin line
#include "sim/simulation.h"
#include "sim/tuning.h"
#include "sim/stream.h"
#include "sim/tuningstamp.h"
#include "sim/voxload.h"
#include "sim/waterbody.h"
#include "sim/wind.h"
#include "sim/weather.h"
#include "sim/windprim.h"
#include "sim/currentprim.h"
#include "sim/world.h"
#include "sim/worldedit.h"
#include "sim/worldio.h"
#include "telemetry.h"
#include "test/selftest.h"
#include "tools/voxregion.h"  // --voxdump / --voxserve, the tuner's voxel view
#include "measure/measure.h"
#include "measure/perfsuite.h"
#include "measure/perfnodes.h"
#include "measure/perfscope.h"
#include "gpu/passtimer.h"
#include "measure/renderstats.h"
#include "test/support.h"
#include "test/tickrig.h"   // support::TickRig: the --shot-* harnesses tick TickAuthority
#include "test/treefixture.h"
#include "ui/overlay.h"
#include "world/refs.h"
#include "world/refs_game.h"
#include "crash.h"

// The sim/render plumbing these once defined in place now lives in
// test/support.{h,cpp}, so the selftest can use it from its own translation
// units without a second copy drifting out of step.
using namespace sandvox;

namespace {
// kDevFanOwner moved to game/session.h (N5): the dev panel writes it and
// the tick body honours it.

// ---- F4's authored starting point -------------------------------------------
// The overlay cycle is one integer but the tuning file has two independent
// bools, one per field, because each is also read on its own by the HEADLESS
// draw path (which has no UIState and cannot press a key). This collapses the
// pair into the cycle's state, and the current field wins a file that asks for
// both — a frame holding two arrow lattices at once is the thing the cycle
// exists to avoid.
int FieldVizFromTuning(const Tuning& t) {
  if (t.render.dbgCurrentField) return UIState::kFieldVizCurrent;
  if (t.wind.dbgWindField) return UIState::kFieldVizWind;
  return UIState::kFieldVizOff;
}

// ---- the scroll wheel -------------------------------------------------------
// GLFW callbacks are C function pointers, so the accumulator is file-scope.
// ACCUMULATED rather than sampled, because scroll arrives as discrete events
// inside glfwPollEvents and a frame that polls two notches must see two: a
// "last event wins" read would quietly drop half a fast flick.
//
// This closes a stale comment in the frame loop that claimed the wheel picked
// a hotbar slot — Inventory::Scroll (game/item.h) has been written and
// unreachable since it was added, because nothing ever installed a callback.
//
// INSTALLED BEFORE Overlay::Init, AND THAT ORDER IS LOAD-BEARING.
// ImGui_ImplGlfw_InitForOther(window, /*install_callbacks=*/true) installs its
// OWN scroll callback and CHAINS to whatever was registered before it. Setting
// this one afterwards replaced ImGui's outright, so ImGui never saw a wheel
// event and every scrollable panel in the dev overlay was frozen — which read
// as "scrolling is disabled in the menu" and is in fact a one-line ordering
// bug. Registering first puts this at the tail of ImGui's chain: both get the
// event, and WantsMouse() below decides who acts on it.
double g_scrollY = 0.0;
void ScrollCallback(GLFWwindow*, double, double dy) { g_scrollY += dy; }

// ---- --shot-frames: a subset of RunShots' frames, and a record of them ----
//
// RunShots writes ~33 BMPs, most of them for one feature each (the lava block,
// the blood block, the wind block...). A package that touched one of them
// wants that one, and --verify wants to record which files a launch produced
// without parsing "wrote ..." off stdout. Names match with or without ".bmp".
std::vector<std::string> g_shotOnly;
struct ShotRecord { std::string file; bool ok; };
std::vector<ShotRecord> g_shotResults;

bool ShotWanted(const char* path) {
  if (g_shotOnly.empty()) return true;
  std::string p = path;
  if (p.size() > 4 && p.compare(p.size() - 4, 4, ".bmp") == 0) p.resize(p.size() - 4);
  for (const std::string& want : g_shotOnly)
    if (want == p) return true;
  return false;
}

// ---- --shot-inventory: the character screen as a reviewable image ----------
//
// The screen's whole job is to be LOOKED at, and the only thing that can judge
// it is a picture. Without this, every visual iteration costs a human opening
// the game, walking somewhere, cutting bits off themselves and pressing I.
//
// It runs the ORDINARY windowed loop — same overlay, same portrait pass, same
// everything — and on one scheduled frame renders the whole thing a SECOND
// time into an offscreen target and writes it out. A second render rather than
// a swapchain grab because a presented image is not copyable, and re-recording
// the frame is both cheap and exactly what the screen already contains.
//
// The damage is scripted at fixed ticks so the picture is the same every run:
// an arm off, a hand off, and a bore through the torso, which between them
// exercise the severed row, the burning/bleeding chips and the "% intact" bar
// that hp alone cannot produce.
bool g_shotInventory = false;
// TWO pictures, because the screen has two halves and one of them cannot be
// seen from the other: the equipment view and the injury inspector share a
// frame and a portrait but show completely different things.
// The HUD before the screen opens: the hotbar strip along the bottom with a
// water-filled flask selected, so the pour-point sphere is in the picture too.
constexpr uint64_t kShotInvHudSetup = 110;
constexpr uint64_t kShotInvHudFrame = 140;     // -> screenshot_hud.bmp
constexpr uint64_t kShotInvOpenFrame = 150;    // let the avatar spawn and settle
constexpr uint64_t kShotInvDamageFrame = 170;
constexpr uint64_t kShotInvGearFrame = 220;    // -> screenshot_inventory.bmp
constexpr uint64_t kShotInvCaptureFrame = 240;  // -> ..._health.bmp
constexpr uint64_t kShotInvGrimoireFrame = 260; // -> ..._grimoire.bmp
// The SPELL PAGE (docs/PLAN_spell_graph.md §7): the same composer with the
// canvas in it, on a COPY of the `duststorm` starter (starters are read-only,
// and a page you cannot edit cannot show an edit), with one lane added through
// the intent path — which puts a socket, the bus, a mod tag and the per-level
// prices in one picture. Three frames apart from the grimoire shot because the
// edit is applied through main.cpp's intent consumer and has to be seen by a
// tick before it is drawn.
constexpr uint64_t kShotInvGraphSetup = 262;
constexpr uint64_t kShotInvGraphEdit = 266;
constexpr uint64_t kShotInvGraphFrame = 274;    // -> ..._inventory_graph.bmp
// Fourth: a dressed human killed in front of the player and its corpse opened
// — the loot panel where the grimoire was (game/corpses.h).
constexpr uint64_t kShotInvLootFrame = 280;     // -> ..._loot.bmp
// Fifth and sixth: THE DEATH SCREEN, which nothing else can photograph. The
// player is decapitated — the case that motivated the whole death hold, and
// the one a bare hp readout cannot express — and the screen is opened by hand,
// because a death no longer opens it (main.cpp's death-hold block).
//
// TWO pictures and a TURN between them, because one picture cannot tell the
// difference between the two implementations. What is frozen at a death is the
// body's POSE, not the image: the corpse in the world goes on ragdolling,
// settling and rolling, so a second capture 60 frames later at a different
// orbit is the only thing that says "still the body that died, from a new
// angle" rather than "a heap, or a still that does not turn".
constexpr uint64_t kShotInvDeathFrame = 310;
constexpr uint64_t kShotInvDeathShot = 350;      // -> ..._death.bmp
constexpr uint64_t kShotInvDeathTurnAt = 360;    // orbit ~80 degrees
constexpr uint64_t kShotInvDeathTurnShot = 420;  // -> ..._death_turn.bmp; last

// ---- --shot-spellpage: the SPELL PAGE's own look-iteration harness ---------
//
// `--shot-inventory` photographs the spell page once, on one five-word page.
// One picture of one tree cannot say whether a stroke overlaps a cell, whether
// a fan of nine sockets collides with its neighbour, or what a page too tall
// for the band looks like when the fit gives up - and those are exactly the
// questions a look pass asks. So this is the GALLERY: the same composer, the
// same canvas, and one BMP per scene, each scene a word list chosen to put a
// different piece of the drawing under the lens.
//
// Cheap on purpose: no damage, no dressing, no corpse, no death. It opens the
// screen, writes the words straight into `grimoireEditWords` (the composer's
// own field - the harness types, it does not reach into the tree), waits a few
// frames for main.cpp's parse/lower/build to refill the mirror, and shoots.
bool g_shotSpellPage = false;
struct ShotSpellScene {
  const char* name;
  const char* words;  // space separated; "" is the empty page
  // The view the scene is shot at. Zero zoom is the fit, which is what every
  // ordinary scene wants; a scene that names a rung and a pan is photographing
  // THE VIEW rather than the tree - the page's ground is drawn in page
  // coordinates, and the only way to prove that is a picture of it moved.
  float zoom = 0.0f;
  float panX = 0.0f, panY = 0.0f;
};
// THE SCENES. Every one of them is a question about the picture:
//   plain     - two words: does a minimal tree sit centred and whole?
//   duststorm - the shipped example: a bus, three sockets, a mod, a fan
//   operator  - `transmute` with a hole in it: the slash, the blank in the row
//   halfword  - `_ trail` alone: the word that is nothing until something is
//               spoken before it, and the clasp that says the pair is one thing
//   nested    - a delivery inside a delivery: the deep tree
//   lanes     - lane/end scoping: sockets with payloads of their own
//   fan       - shotgun twice: nine instances, the widest row the page draws
//   tall      - eighteen words of everything: the page that does not fit
//   split     - the shape the whole thing is for: one shotgun, three branches,
//               each rising to its own copy of the delivery
//   deepfan   - a shotgun on a box three boxes down, and a shotgun on the HAND:
//               the two places a fan can open that are not the top of the tree
const ShotSpellScene kShotSpellScenes[] = {
    // ONE WORD. The page that showed a pip, a rule and another pip for a spell
    // that has no payload, no fan and nothing shared (2026-09-22): two cells is
    // the whole truth about it.
    {"bare", "projectile"},
    {"plain", "fire projectile"},
    {"duststorm", "sand gust gust shotgun projectile"},
    {"operator", "water transmute swift bolt"},
    // BOTH HALVES OF THE SAME QUESTION: `trail` with nothing before it (one
    // blank, centred under the cell it is waiting for) and `transmute` with
    // nothing at all (two blanks side by side, which is the widest a clasp
    // gets). Until 2026-09-22 both drew as a finished cell with a 21 px ring
    // hung off its edge.
    {"halfword", "trail projectile"},
    {"halfwords", "transmute projectile"},
    {"nested", "fire explosive bomb orb long projectile"},
    {"lanes", "stone lane acid wide end lane fire end shotgun lob"},
    {"fan", "sand shotgun shotgun gust projectile"},
    {"tall",
     "water fire transmute twin seek heavy lane acid wide end lane stone "
     "bounce end explosive long shotgun lob"},
    // A FAN THAT IS NOT AT THE TOP. `shotgun` on the innermost box opens
    // three sockets into layers the boxes above it have already spent, and
    // `shotgun` spoken last edits the implicit `hand` - the one box drawn
    // bar-downward, so its fan hangs at the FOOT of the page. Both drew wrong
    // until 2026-09-22 (the bead's trunk stroke was patched onto whichever bar
    // finished first, which on the hand is a delivery several layers up).
    {"split", "sand gust shotgun projectile"},
    // A SPLIT INSIDE A SPLIT: the box fans into three and one of those branches
    // splits again. The grammar does it (`--gate spells`, L13); what this scene
    // watches is whether the PAGE says so.
    {"subfan", "sand shotgun lane gust shotgun end projectile"},
    // THE CASCADE, reported 2026-09-22: "heres a shotgun into a twin, the twin
    // doesnt split into 2 more. it should just fractal cascade into more and
    // more." The first is that sentence exactly - on the HAND, which has no
    // cell per branch, so the sub-fan was drawn as a tally on nothing at all.
    // The second is the same shape one level down, and the third is a fan
    // inside a fan inside a fan by nesting boxes, which is where the depth
    // actually comes from.
    {"cascade", "shotgun lane twin end"},
    {"cascade2", "sand shotgun lane twin end projectile"},
    {"cascade3", "fire shotgun projectile twin projectile"},
    {"deepfan", "fire shotgun projectile projectile projectile"},
    {"handfan", "explosive projectile projectile shotgun"},
    // MAGNITUDES (PLAN_spell_magnitude §2.5): a numeral at the foot of every
    // cell whose magnitude is not 1, on a word, an operator and a mod bead.
    {"magnitude", "fire@2 explosive@0.5 float@0.5 bounce@3 projectile"},
    // TIMING and SIGNED COMPONENTS (M2/M3): a tag on each item that does not
    // fire on hit, and a signed numeral on a lift pressing down.
    {"timing", "explosive!bounce fire!every10+20 bounce@2 lift@-0.5 speed@3 projectile"},
    {"empty", ""},
    // The same page, dragged: every mark on the sheet - the ruling, the
    // pricked margins, the foxing, the great figure - must have moved with it.
    {"panned", "sand gust gust shotgun projectile", 0.5f, 90.0f, 34.0f},
    // ...and read close, where the engravings come back.
    {"zoomed", "sand gust gust shotgun projectile", 1.0f, 0.0f, -40.0f},
};
constexpr int kShotSpellSceneCount =
    (int)(sizeof(kShotSpellScenes) / sizeof(kShotSpellScenes[0]));
// Open late enough that the world is up and the avatar has spawned (the screen
// draws a portrait either way, and an empty one is a distraction in a picture
// of a page). Then one scene every `kShotSpellStride` frames: the words go in
// on the scene's first frame and the shutter falls eight frames later, which
// is several ticks of the graph rebuild.
constexpr uint64_t kShotSpellOpen = 120;
constexpr uint64_t kShotSpellFirst = 140;
constexpr uint64_t kShotSpellStride = 12;
constexpr uint64_t kShotSpellShutter = 8;
constexpr uint64_t kShotSpellLast =
    kShotSpellFirst + (uint64_t)(kShotSpellSceneCount - 1) * kShotSpellStride +
    kShotSpellShutter;
// Which scene a frame belongs to, or -1: `setup` asks about the frame the
// words land on, otherwise about the frame the picture is taken on.
inline int ShotSpellSceneAt(uint64_t frame, bool setup) {
  const uint64_t base = kShotSpellFirst + (setup ? 0 : kShotSpellShutter);
  if (frame < base) return -1;
  const uint64_t d = frame - base;
  if (d % kShotSpellStride != 0) return -1;
  const int idx = (int)(d / kShotSpellStride);
  return idx < kShotSpellSceneCount ? idx : -1;
}

// ---- --shot-jump: the AIRBORNE POSE's look-iteration harness ---------------
//
// The avatar's air pose is driven by `vel.y` (avatar.airPose, tuning.h), and
// the only honest test of "does it look good" is a picture of each phase. This
// runs the windowed game in third person, walks the body forward, jumps it, and
// writes one BMP per phase.
//
// CAPTURED ON THE POSE'S OWN PHASE, NOT ON A FRAME NUMBER. Ticks and frames are
// not 1:1 here (the fixed-step loop fires 0..4 ticks a frame), so a frame
// schedule would photograph a different part of the arc on every machine and on
// every frame rate — which is exactly what a look-iteration harness must not
// do. The trigger is `PlayerAvatar::AirPoseVy()` crossing the thresholds the
// pose itself blends on, so each picture is the shape it is named after.
bool g_shotJump = false;
// Frames of walking before the jump: enough for the world to stream, the gait
// to reach steady state and the third-person boom to settle behind the body.
constexpr uint64_t kShotJumpAtFrame = 220;
// Hard stop, in case the body lands somewhere it cannot jump from again.
constexpr uint64_t kShotJumpLastFrame = 420;
// Which pictures are still owed, in the order the arc reaches them.
enum class JumpShot { Rise, Apex, Fall, Land, Done };
JumpShot g_shotJumpWant = JumpShot::Rise;
const char* g_shotJumpPath = nullptr;  // set for exactly one frame, then taken
// `--shot-bench`: the alchemy bench's look-iteration harness. Fills a flask
// (SANDVOX_BENCH_A, a ContainerParseFillSpec, default water+oil+sand) and a
// second one (SANDVOX_BENCH_B, default lava+sand), opens the first on the
// bench, stirs it, brings the second in, carries it over the mouth and tilts
// it to pour, then closes -- a picture at each step through g_shotJumpPath.
// THE CHEMISTRY (package C), by env: SANDVOX_BENCH_BURNER=1 lights the flame
// under A, SANDVOX_BENCH_STOPPER=1 stoppers A, SANDVOX_BENCH_SHOCK=<frame>
// electrifies A every 20 frames from that frame, SANDVOX_BENCH_NOPOUR=1 skips
// the stir and the pour and just watches (extra pictures _chem1.._chem3).
// e.g. SANDVOX_BENCH_A=acid:0.3+sand:0.08 (fumes), =salt:0.1 with BURNER=1 and
// SHOCK=300 (electrolysis), =water:0.3+sodium:0.01 (it blows up in your hands).
// SANDVOX_BENCH_INVERT=1: B is lifted high, carried over A and turned fully
// UPSIDE DOWN with its mouth over A's, into the headroom above the table's
// box (pictures _invlift, _invturn, _inverted, _invpoured).
// SANDVOX_BENCH_HIGH=1: B is lifted as high as the bench allows and swept
// side to side, untilted (the raised-arm repro); with SANDVOX_BENCH_DEBUG=2
// the carried arm prints every tick (session.cpp PoseBenchHands).
bool g_shotBench = false;
constexpr uint64_t kShotBenchLast = 425;
// `--shot-devpanel`: the F1 sidebar's look-iteration harness. One picture per
// page (screenshot_devpanel_<page>.bmp), through g_shotJumpPath, plus the
// Spawn page with a flask picked so the vessel picker's columns are in shot.
bool g_shotDevPanel = false;
constexpr uint64_t kShotDevFirst = 90, kShotDevStep = 12;
constexpr uint64_t kShotDevLast = kShotDevFirst + kShotDevStep * 7;
float g_shotJumpVy = 0.0f;             // the vy that picture was taken at, m/s
// `--shot-dialogue`: the CONVERSATION PANEL's look-iteration harness
// (ui/dialogue_ui.h). A dummy human is spawned ahead, the sample dialogue is
// started with it (by id, through the dev hook's session queue), and the panel is shot
// at its entry node (screenshot_dialogue.bmp), then again after [continue]
// is answered through the tick command, on the choice rows
// (screenshot_dialogue_2.bmp).
bool g_shotDialogue = false;
constexpr uint64_t kShotDlgSpawn = 90, kShotDlgBegin = 130, kShotDlgShot1 = 160,
                   kShotDlgChoose = 170, kShotDlgShot2 = 200, kShotDlgLast = 206;

// --frames N (phase 4b D3): windowed verification harness. 0 = play normally.
uint64_t g_harnessFrames = 0;

// ---- STARTUP TIMELINE ------------------------------------------------------
// One stderr line per startup phase: wall seconds since main(), the wall and
// the PROCESS CPU seconds (every thread, user + kernel) since the previous
// mark. The CPU column is the one that NAMES a stall: a phase that costs 90 s
// of wall and 2 s of CPU is waiting (GPU, a fence, a lock); one that costs 90 s
// of wall and 1,000 s of CPU is a driver thread pool compiling something � the
// NVIDIA pipeline compiler is the only multi-threaded work this process does
// before the first tick (Jolt's pool has nothing to step yet). Always on: it is
// ~15 lines per launch, and the one time it mattered (a multi-minute white
// window on a fresh build, docs/PLAN_lin_followups.md �6 T1) nobody had timed
// a launch before and after the week's landings, and three plausible causes
// were argued from what had changed instead of from a clock.
void StartupMark(const char* phase) {
  static const double t0 = NowSeconds();
  static double tPrev = t0;
  static double cPrev = ProcessCpuSeconds();
  const double t = NowSeconds();
  const double c = ProcessCpuSeconds();
  std::fprintf(stderr, "[startup %7.2fs | +%7.2fs wall  +%8.2fs cpu] %s\n",
               t - t0, t - tPrev, c - cPrev, phase);
  std::fflush(stderr);
  tPrev = t;
  cPrev = c;
}
// ---- SANDVOX_TICKS_PER_FRAME=<n>: DETERMINISTIC HARNESS PACING, OFF BY
// ---- DEFAULT (docs/PLAN_multiplayer_now.md N5, step 5)
//
// A `--frames N` run's TICK SCHEDULE is a function of the wall clock: the
// accumulator fills at real dt, the GPU-lag throttle drops a tick when the
// device is behind, and streaming's R4 clamp is per FRAME. Every scripted
// input in this file is already a pure function of `tick` (the autofly phase
// comments say so in as many words), so what varies between two runs of the
// SAME binary is only HOW MANY ticks N frames contained — measured 143 and 145
// over two back-to-back `--frames 600 --autofly-hard` runs. Anything recorded
// off the frame loop is therefore not reproducible, which makes an op record
// useless as a before/after oracle for a refactor.
//
// Set, this switch makes a frame run EXACTLY n ticks (n clamped to the same
// backlog cap), bypasses the GPU-lag throttle, and DROPS THE MOUSE — the OS
// cursor is not input to a scripted harness, and a stray delta at tick 321 was
// the second reason two identical runs diverged. It changes only WHICH ticks
// run and what the camera is pointed at, never what a tick computes, so
// nothing hashed can move and no headless suite reaches it (they have no
// frame loop). Same family as SANDVOX_NO_GPU_THROTTLE / SANDVOX_FRAMES_NO_RELOAD.
int HarnessTicksPerFrame() {
  static const int n = [] {
    const char* s = std::getenv("SANDVOX_TICKS_PER_FRAME");
    if (!s) return 0;
    return std::max(0, std::min(std::atoi(s), World::kMaxTicksPerFrame));
  }();
  return n;
}
// ---- SANDVOX_NET_SMOKE_EXIT_ON_PEER_DONE=1: THE TWO-PROCESS SMOKE'S FULL STOP
//
// Same family as the switch above, and it exists for the same reason: a
// harness has to be able to END. The M9.2 smoke runs a host at --frames 900
// and a client at --frames 600 so the CLIENT is the one that finishes first;
// with lockstep pacing the host is then stalled forever on batches that will
// never arrive, and the run has to be killed rather than exiting 0. Set, a
// --frames host closes the window the moment its peer disconnects, so the
// smoke ends on its own and both processes reach their exit report.
//
// Deliberately NOT the default: a listen server whose player quits for a
// moment should go back to listening, not shut the world down. Only the
// harness wants a peer's departure to be terminal.
bool NetSmokeExitOnPeerDone() {
  static const bool on = std::getenv("SANDVOX_NET_SMOKE_EXIT_ON_PEER_DONE") != nullptr;
  return on;
}
// ---- SANDVOX_NET_SMOKE_PAINT=1: AN AUTHOR IN A HARNESS RUN (M9.3-B) -------
//
// Third of the same family, and it exists because the two-process smoke could
// otherwise prove nothing about the op exchange: --autofly-hard flies, and a
// flying camera authors NO world ops at all. The exchange would be exercised
// by zero ops in both directions and the run would be green for the wrong
// reason — a "fixture that cannot fail".
//
// Set, a --frames run pushes ONE small brush op into its own outgoing batch
// every 30 ticks, at the player's feet. It goes in through the delay queue
// like any other local op (net/opsync.h), so it carries the same label, it
// crosses the wire, and both machines submit it on the same tick: exactly the
// path a human's brush stroke takes. It is NOT a second mutation path -- the
// op reaches the world through SubmitTick and nothing else.
//
// Only under --frames, and only while connected: a windowed game has a player
// to do the painting.
bool NetSmokePaint() {
  static const bool on = std::getenv("SANDVOX_NET_SMOKE_PAINT") != nullptr;
  return on;
}
// ---- SANDVOX_NET_SMOKE_DRIFT=1: A DELIBERATE DIVERGENCE (M9.3-C) ---------
//
// Fourth of the family, and the one with the most uncomfortable job: the
// chunk resync repairs a divergence, so a smoke that cannot MANUFACTURE one
// tests the publish path and nothing else. Every natural divergence is either
// a defect (so it cannot be summoned on demand) or a window-edge case the
// comparable rule deliberately excludes.
//
// Set, a --frames run submits ONE local-only CellOp every 90 ticks into a
// quiet chunk four chunks below the player's feet — deep buried stone under a
// standing host, which is the QUIET case the resync schedule is built for. It
// is appended AFTER the op merge (session.cpp phase N, via
// TickAuthorityCtx::driftCells), so it reaches this machine's GPU and never
// the peer's. Ninety ticks is three seconds: long enough for the chunk to go
// quiet again, be published, be drilled into, be requested and be repaired
// before the next one lands, so the mismatch series shows a spike and a
// return to zero rather than a permanent offset.
//
// It writes a PALETTE VARIANT of stone, not a new material: the state nibble
// is hashed (world.h's word layout) so the digest moves, while no reaction can
// fire and no neighbour can be woken into moving — the divergence stays the
// one voxel it was asked to be.
bool NetSmokeDrift() {
  static const bool on = std::getenv("SANDVOX_NET_SMOKE_DRIFT") != nullptr;
  return on;
}
// ---- SANDVOX_NET_SMOKE_MOBS=1: SOMETHING TO OWN (M9.4-D) ----------------
//
// Fifth of the family, and the reason is the one NetSmokePaint gives about
// ops, applied to entities: --autofly-hard flies, and an empty world contains
// no creature and no loose body, so the entity exchange would be exercised by
// ZERO announces, ZERO poses and ZERO handoffs and the smoke would be green
// because nothing happened. A "fixture that cannot fail" (CLAUDE.md).
//
// Set, a --frames run spawns three of the `crowd` gate's humanoid def eight
// voxels in front of the player at tick 30 — through MobSystem::Spawn, the
// same single call the AI panel makes, so they are ordinary creatures with
// ordinary brains and nothing about them is a test object. Tick 30 rather
// than tick 0 because the window has to have shifted onto the player and the
// terrain under their feet has to exist.
//
// The HOST is the one that sets it in the smoke recipe: the host stands still
// and owns them, the client flies through and sees ghosts, which is exactly
// the asymmetry the `entities:` report line is read for.
bool NetSmokeMobs() {
  static const bool on = std::getenv("SANDVOX_NET_SMOKE_MOBS") != nullptr;
  return on;
}
bool g_autofly = false;
bool g_autoflyHard = false;  // --autofly-hard: adversarial traversal for pool sizing
// --autofly-surface: the RENDERER's adversarial traversal, the complement of
// --autofly-hard. Where the hard descent drives the window into solid bulk to
// stress residency, this one flies OVER the terrain to stress ray length: the
// surface case is where the frame cost lives (docs/PLAN_surface_flight_perf.md
// Part A), and it is exactly the case the descent cannot reach.
bool g_autoflySurface = false;
// --autowalk: the WALKING harness. Forward held on foot (no fly), a hop every
// 45 ticks to clear low obstacles, and a quarter turn every 240 ticks so a
// wall does not end the walk. Exists because the two CPU spikes reported
// from live play (terrain-collider rebuilds on a chunk-boundary crossing,
// the GPU-lag catch-up loop) never fire under fly mode: a flying player has
// no ground to collide with and crosses chunk boundaries too fast to walk
// into a mob's nav horizon. Pair with --duel-dummy for a hostile follower.
bool g_autoWalk = false;
// Which of the two --autofly-surface regimes the current frame is in, set from
// the tick phase in the input block and consumed by the altitude pin after
// player.Update. Two sites because the phase is known where every other autofly
// decision is made, but the pin has to land after the integrator.
bool g_autoflySurfaceHigh = false;
// Voxels of clearance for each regime. The low skim wants to be just over the
// canopy — trees run ~30 voxels above the ground they stand on — so it is
// canopy + 5. The high cruise is +150 voxels (15 m at kVoxelMeters 0.10), well
// clear of anything worldgen builds, which is what isolates the altitude term.
constexpr float kAutoflySurfaceLowVox = 35.0f;
constexpr float kAutoflySurfaceHighVox = 150.0f;
std::vector<double> g_frameMs;  // --frames: whole-frame wall clock, for percentiles
// --autofly-surface splits the SAME samples by regime. A pooled p50 over a run
// that alternates low skim and high cruise is the median of a bimodal
// distribution and answers neither question: the altitude term is the whole
// point of the harness, so the two arms are reported separately as well.
std::vector<double> g_frameMsLow, g_frameMsHigh;
// The SIM LOAD the frame times above were produced under. The CA cost model in
// docs/ROADMAP_scale.md §3.0 is
//   CA/tick = 54 x (4.65 us + 0.245 us x activeChunks)
// which splits into a fixed 251 us DISPATCH FLOOR and a per-chunk term, and the
// two are attacked by completely different optimizations — fewer dispatches vs
// less work per chunk. Which one dominates is decided entirely by this number,
// and the harness reported frame milliseconds without ever saying which regime
// produced them. The model was fitted on scripted scenes at 3-69 active chunks;
// sustained flight is a different regime (every window shift wakes the chunks
// genChunk just generated with matter), so it needs its own reading.
std::vector<double> g_activeChunks;
double g_harnessRenderMs = 0.0;
// --frames: the SAME CPU scope attribution the Performance tab shows, summed
// over the run, plus the per-frame series for each scope so a spike can be
// reported as a max and not just as a mean.
//
// It lives here because the tab needs a telemetry client attached and the
// windowed harness has none — which meant the one path that reproduces the
// user-visible spikes (fly around, watch a bar jump to 30 ms) was the one path
// with no way to record them. `--frames 600 --autofly-hard` now prints the
// whole table, so "which scope spiked" is a single non-interactive run.
double g_frameScopeSum[sandvox::kPerfScopeCount] = {};
double g_frameScopeMax[sandvox::kPerfScopeCount] = {};
std::vector<double> g_frameScopeSeries[sandvox::kPerfScopeCount];
uint64_t g_frameSnapStalls = 0;      // total paged-staleness WaitIdle stalls
uint64_t g_frameSnapStallFrames = 0; // frames that paid at least one
uint64_t g_frameRbDeclines = 0;      // readback requests the ring refused
// Ticks the GPU-lag throttle (the tick loop) pushed to a later frame because
// the GPU still owed two or more snapshot readbacks. Sim time dilates by that
// many ticks instead of the frame stalling on a fence.
uint64_t g_ticksThrottled = 0;
// ---- THE CASCADE REFILL, AS ENTRIES AND NOT AS A MEAN (CLAUDE.md rule 6) ---
// `farField` on the GPU table is one mean over the whole run and it cannot
// distinguish 'the refill finished cheaply' from 'the refill never finished'.
// Diagnosing the horizon's arrival needed exactly that distinction — a
// 256-entry slice measured 4.4x the TOTAL far GPU time of a 4096-entry one
// for what should have been the same 262,144 entries — and there was no way
// to read the entry count off a run. These three say how much sieve actually
// ran, in how many ticks, and how much was still queued at exit.
uint64_t g_farEntries = 0;    // sieve entries dispatched over the run
uint64_t g_farTicks = 0;      // ticks that dispatched at least one
uint64_t g_farBiggest = 0;    // the largest single tick's count
// --frames: the GPU side of the same picture. The live telemetry path already
// bills every timestamped pass to its Engine-map node per frame; the harness
// keeps the per-frame series so the summary can say WHICH GPU row spiked,
// which is the question every stall on the CPU side ends in.
std::vector<double> g_frameGpuSeries[sandvox::kPerfNodeCount];
// ...and per PASS, because a node is a sum: `farField` is farDown (the
// dirty-list downsample) + farFill (the sieve) + farPatchFill, and they
// scale with different things. Keyed by pass name; a frame in which a pass
// did not run is a zero for it, padded at print time.
std::map<std::string, std::vector<double>> g_frameGpuPassSeries;
size_t g_frameGpuFrames = 0;
// P2-D: WHICH cause and WHICH arm (support.h SnapshotStallStats). Accumulated
// over the harness frames only, like the three counters above.
sandvox::SnapshotStallStats g_frameStallStats{};

// ---- --autofly-park: the active-chunk DECAY probe ---------------------------
//
// `--autofly-surface` measures ~550 active chunks at p50 while `--autofly-hard`
// measures 0, and those two numbers admit two very different explanations:
//
//   (a) a SETTLING TRANSIENT — a window shift wakes every chunk genChunk just
//       generated with matter (worldgen.wgsl's `n > 0` wake), so at ~0.7
//       shifts/tick x ~500 woken chunks the steady state is just the pipeline
//       of chunks that have not settled YET, and nothing is wrong; or
//   (b) a rule that NEVER SLEEPS in the surface biomes (the failure mode
//       documented at doReactions' keepAwake note), which the `sleep` gate
//       cannot see because it tests the origin-area world.
//
// The two are separated by removing the shifts: fly out to representative
// surface terrain, then STOP DEAD and watch the count. Decays to ~0 => (a);
// plateaus => (b), and the plateau is the prize. Parking also freezes the
// altitude regime, because the low/high alternation is a 115-voxel hop that is
// itself a 7-chunk vertical window shift.
//
// ANSWER (2026-08-24, SANDVOX_PARK_SETTLE=3000): (a), and the mechanism is
// worth keeping the probe for. Parked, the count decays MONOTONICALLY 630 ->
// ~30 over ~2,700 ticks — a half-life near 700 ticks, not the ~1.5 ticks the
// arithmetic behind (a) assumed. 97% of the survivors at every point along
// that curve contain LAVA, and they sit in one band, world chunk Y -7..-3,
// which is `caveAt`'s deep-cavern lava (worldgen.wgsl, `cv == 2`). Worldgen
// lays that lava down as a 3-voxel FULL slab on a noisy cavern floor, so it
// spends ~90 seconds of sim time flowing out to rest — and every window shift
// regenerates it. Under sustained flight it is therefore permanently mid-
// settle, which is what a ~550 steady state with no rule at fault looks like.
// Nothing here never sleeps; the settle is just far longer than one tick.
bool g_autoflyPark = false;
// --duel-dummy: spawn one sword-armed human in front of the player and leave it
// there. The manual half of the melee gates — `swing` and `swing-plane` assert
// the trajectory and the wound, and this is where a person judges the FEEL.
bool g_duelDummy = false;
// --fell-tree: the tree-fell gate's fixture, planted 48 voxels ahead of the
// player once the world has settled, cut 120 ticks later, and the 300-tick
// fall profiled exactly as the gate profiles it. The gate is headless, so the
// render half of the handoff (BuildInstances, the drawBodies span, the
// whole-frame time) only exists as a number HERE, under --frames.
bool g_fellTree = false;
// THE BODY RENDER HALF, as numbers (rigidbody perf package, 2026-09-28): the
// CPU time spent building the cube-instance list, the bytes it sent, the micro
// brick-pool bytes, and how many body draws were issued. Whole run; printed by
// the --frames report as the "body-render" line. `frames` counts frames that
// had any body slot at all, which is the divisor every mean below uses.
struct BodyRenderStats {
  uint64_t frames = 0, builds = 0, instBytes = 0, microPoolBytes = 0;
  uint64_t cubeDraws = 0, cubeInstDrawn = 0, cubeInstLive = 0;
  double buildUs = 0, buildMaxUs = 0, listUs = 0, commitUs = 0;
};
BodyRenderStats g_bodyStats;
int g_fellTreeAt = 240;  // the plant tick; the cut is 120 ticks later
// An optional site ("x,z"): the game then STARTS 48 voxels west of it looking
// +X, so the tree stands there. Default is 48 voxels ahead of wherever the
// player spawned -- which at the map's spawn site is inside a forest, and a
// crown that touches a neighbour's is anchored through it (2026-09-12: the
// flood walked 54 voxels west and 40 down through another tree and correctly
// refused). The harness pad (map.json site `harness`, x/z -128..640) has no
// trees by construction; --fell-tree 240 200,200 plants there.
//
// The 2026-09-22 "3 fps after a tree falls or a big body splits" repro is
//   SANDVOX_FRAMES_NO_RELOAD=1 SANDVOX_FELL_SPECIES=birch SANDVOX_FELL_SPLIT=90
//     ./sandvox.exe --frames 2000 --fell-tree 240 200,200
// (a BAKED tree, not the fixture; two splits; CutBody timed on the log). The
// NO_RELOAD is not optional: the harness's mid-run F5 lands just after the
// cut and wipes the bodies, which read as "the tree vanished".
bool g_fellSiteSet = false;
int g_fellSiteX = 0, g_fellSiteZ = 0;
// --forest-fire [tick]: the "whole forest is burning around me" frame-rate
// repro, in the live loop so debris, mobs, audio and the render all pay what
// the game pays (--perf treeburn is ONE tree, headless, with no debris).
// On `tick` (default 240) fire is seeded, IfAir, in a disc of columns out to
// 100 voxels round the player at ground level and up through the canopy
// band; the first 600 ticks after that are the fire taking hold and are NOT
// measured (every harness series is cleared at ignition+600), the next 600
// are, with the camera turning one full circle so the number is the fire all
// round and not whichever side the spawn faced. Then the run ends itself and
// the normal --frames report is the forest fire's.
//   SANDVOX_FRAMES_NO_RELOAD is implied.
//   bash scripts/run.sh ./build/Release/sandvox.exe --frames 100000 --forest-fire
bool g_forestFire = false;
int g_forestFireAt = 240;
bool g_forestFireDone = false;
// SANDVOX_PARK_AT="x,y,z": park at a NAMED PLACE instead of wherever the
// procedural surface route happens to stop.
//
// The route is dt-integrated, so where it ends is not a choice anybody made —
// the 2026-08-24 answer below came back "97% of the survivors contain LAVA"
// because that is what the flight passed over, and a re-run on 2026-09-08
// landed in open desert and settled to 0 active chunks. Neither run says
// anything about a material it never flew over. "Which materials hold a chunk
// awake" is a question about a PLACE, and this engine has places at known
// addresses: the harness map's water sites (World::WaterSiteDisc) give the
// fixture lake's centre and waterline, `--voxdump`'s coordinates give any other.
//
//   SANDVOX_PARK_AT=420,230,420 ./sandvox.exe --autofly-park --frames 600
//
// With it set the fly phase is skipped (the point is the sample, not the
// route), so the settle window starts almost immediately.
bool ParkAtPos(Vec3& out) {
  static const char* e = getenv("SANDVOX_PARK_AT");
  if (!e) return false;
  float v[3] = {0, 0, 0};
  if (std::sscanf(e, "%f,%f,%f", &v[0], &v[1], &v[2]) != 3) return false;
  out = Vec3{v[0], v[1], v[2]};
  return true;
}
uint32_t ParkFlyTicks() {
  Vec3 unused;
  return ParkAtPos(unused) ? 5u : 300u;   // fly this far out, then stop
}
// Settle window, in ticks, before the sample is taken. Overridable because the
// whole question is "does this decay or plateau", and one duration cannot
// answer it: SANDVOX_PARK_SETTLE=3000 is what separates a very slow settle from
// a rule that never sleeps.
uint32_t ParkSettleTicks() {
  static const uint32_t v = [] {
    const char* e = getenv("SANDVOX_PARK_SETTLE");
    return e ? (uint32_t)std::max(50, atoi(e)) : 400u;
  }();
  return v;
}
bool g_parkPosSet = false;
bool g_parkDone = false;  // dump printed: the run has nothing left to measure
Vec3 g_parkPos{};
// World chunks whose voxels were requested for a sample, split by whether they
// were still ACTIVE at request time. The inactive arm is the control: a
// material that is in every active chunk is only a suspect if it is NOT in
// every chunk. Cleared between the two samples.
std::vector<IVec3> g_parkActiveIds, g_parkIdleIds;
// A third arm that is a PLACE rather than a population: every chunk in a box
// around the park point. The active/idle arms answer "what is awake"; they
// stride over the whole 512^3 window, so parked at a pond they mostly sample
// desert and reported "0 of 0 submerged liquid cells" twice. To ask a question
// ABOUT WATER you have to go and fetch the water.
std::vector<IVec3> g_parkBoxIds;

// Where the active chunks are and what they are made of, at one instant.
// Split out from the schedule below because the SAME question has to be asked
// twice: once MID-FLIGHT (the regime the 550-chunk p50 came from) and once
// after the park has settled.
void ParkSampleRequest(World& world, const char* label) {
  const WorldSnapshot& s = world.Snap();
  constexpr uint32_t kArm = 190;  // 2 x 190 < 2 x kFetchPerTick x ticks-to-dump
  g_parkActiveIds.clear();
  g_parkIdleIds.clear();
  std::map<int, uint32_t> yhist;
  uint32_t emptyActive = 0, totalActive = 0;
  for (uint32_t i = 0; i < kNumSlots; i++) {
    if (!s.dirtyFlags[i]) continue;
    totalActive++;
    if (s.occupancy[i] == 0) emptyActive++;
    yhist[world.SlotToWorldChunk(i).y]++;
  }
  std::printf("park[%s]: %u active, %u of them EMPTY (occupancy 0)\n", label,
              totalActive, emptyActive);
  std::printf("park[%s]: active-by-worldChunkY:", label);
  for (auto& kv : yhist) std::printf(" %d:%u", kv.first, kv.second);
  std::printf("\n");
  // Sample both arms with a STRIDE, not the first N: slot index runs x fastest
  // then y then z, so taking a prefix samples one z-slab of the window and
  // nothing else.
  auto sample = [&](bool wantActive, std::vector<IVec3>& arm) {
    uint32_t pool = 0;
    for (uint32_t i = 0; i < kNumSlots; i++) {
      const bool act = s.dirtyFlags[i] != 0;
      if (act != wantActive) continue;
      // Control arm is non-empty chunks only - comparing against sky would
      // make every material look enriched.
      if (!act && s.occupancy[i] == 0) continue;
      pool++;
    }
    const uint32_t stride = pool > kArm ? pool / kArm : 1u;
    uint32_t seenN = 0;
    for (uint32_t i = 0; i < kNumSlots && arm.size() < kArm; i++) {
      const bool act = s.dirtyFlags[i] != 0;
      if (act != wantActive) continue;
      if (!act && s.occupancy[i] == 0) continue;
      if ((seenN++ % stride) != 0) continue;
      const IVec3 wc = world.SlotToWorldChunk(i);
      world.RequestChunkFetch(wc);
      arm.push_back(wc);
    }
  };
  sample(true, g_parkActiveIds);
  sample(false, g_parkIdleIds);

  // The box arm. 9 x 5 x 9 = 405 chunks against a fetch ring of 64/tick and 40
  // ticks before the report, so it lands comfortably. Centred on the park point
  // and taller downward than upward, because a body of water is UNDER the
  // camera, not around it.
  g_parkBoxIds.clear();
  if (g_parkPosSet) {
    // Anchored to the GROUND under the park point, not to the camera. The
    // camera's altitude is whatever SANDVOX_PARK_AT was told; the water is at
    // terrain height, and a box hung off the camera missed a pond by ~90
    // voxels and reported "0 of 0 submerged liquid cells" twice.
    const int px = (int)std::floor(g_parkPos.x);
    const int pz = (int)std::floor(g_parkPos.z);
    const int gh = World::TerrainHeight(px, pz, kDefaultSeed);
    const IVec3 pc{px >> 4, gh >> 4, pz >> 4};
    std::printf("park[%s]: box centre world chunk (%d,%d,%d), ground y %d\n",
                label, pc.x, pc.y, pc.z, gh);
    for (int dz = -4; dz <= 4; dz++)
      for (int dy = -3; dy <= 2; dy++)
        for (int dx = -4; dx <= 4; dx++) {
          const IVec3 wc{pc.x + dx, pc.y + dy, pc.z + dz};
          world.RequestChunkFetch(wc);
          g_parkBoxIds.push_back(wc);
        }
  }
}

void ParkSampleReport(World& world, const std::vector<MaterialDef>& mats,
                      const char* label) {
  auto tally = [&](const std::vector<IVec3>& ids, std::vector<uint32_t>& out,
                   uint32_t& got) {
    out.assign(mats.size(), 0u);
    got = 0;
    for (const IVec3& wc : ids) {
      const CachedChunk* cc = world.Cached(wc);
      if (!cc || cc->voxels.size() != kChunkVol) continue;
      got++;
      std::vector<uint8_t> seen(mats.size() + 1, 0);
      for (uint32_t w : cc->voxels) {
        const uint32_t m = w & 0xFFFu;
        if (m != 0) seen[m < mats.size() ? m : mats.size()] = 1;
      }
      for (size_t m = 1; m < mats.size(); m++) out[m] += seen[m];
    }
  };
  std::vector<uint32_t> a, b;
  uint32_t na = 0, nb = 0;
  tally(g_parkActiveIds, a, na);
  tally(g_parkIdleIds, b, nb);
  // Does an ALL-INERT chunk (matCanAct false for every cell in it) even exist
  // in this world? That is the population worldgen's narrowed wake predicate
  // can decline to wake, and if it is empty the predicate cannot pay whatever
  // else is true. Same three tests as matCanAct in common.wgsl, on the CPU
  // copy of the same table.
  // ---- THE HYDROSTATIC INVARIANT, COUNTED ---------------------------------
  //
  // "An eighth is a SURFACE thing; anything with water on top of it is full."
  // That is a per-CELL statement, and the dirty-reason histogram cannot check
  // it: those bits are OR'd per CHUNK, so `equalize` and `SUBMERGED` appearing
  // together says only that both happened somewhere in the same 16^3 box, not
  // that one happened to the other. (That distinction cost a wrong reading of
  // the first stage histogram — worth stating, because every per-chunk mask in
  // this engine has the same trap in it.)
  //
  // So count the thing itself, out of the voxels the park probe has already
  // fetched: a liquid cell with the SAME liquid directly above it, holding
  // fewer than 8 eighths. Zero is the invariant holding. The top row of each
  // chunk is skipped because its neighbour lives in a chunk this sample may not
  // have pulled; that costs 1/16 of the population and no accuracy in an
  // is-it-zero question.
  auto underfull = [&](const std::vector<IVec3>& ids) {
    uint64_t submerged = 0, partial = 0;
    for (const IVec3& wc : ids) {
      const CachedChunk* cc = world.Cached(wc);
      if (!cc || cc->voxels.size() != kChunkVol) continue;
      for (uint32_t z = 0; z < kChunk; z++)
        for (uint32_t y = 0; y + 1 < kChunk; y++)
          for (uint32_t x = 0; x < kChunk; x++) {
            const uint32_t w = cc->voxels[(z * kChunk + y) * kChunk + x];
            const uint32_t m = w & 0xFFFu;
            if (m == 0 || m >= mats.size()) continue;
            if (mats[m].gpu.klass != CLASS_LIQUID) continue;
            const uint32_t a = cc->voxels[(z * kChunk + y + 1) * kChunk + x];
            if ((a & 0xFFFu) != m) continue;   // free surface: may be partial
            submerged++;
            if (((w >> 12) & 0xFu) != 7u) partial++;  // state nibble = f - 1
          }
    }
    return std::pair<uint64_t, uint64_t>(partial, submerged);
  };
  {
    const auto a = underfull(g_parkActiveIds), b = underfull(g_parkIdleIds);
    const auto x = underfull(g_parkBoxIds);
    std::printf("park[%s]: HYDROSTATIC submerged-but-PARTIAL: box %llu/%llu "
                "(%.2f%%) | active %llu/%llu | idle %llu/%llu\n", label,
                (unsigned long long)x.first, (unsigned long long)x.second,
                x.second ? 100.0 * (double)x.first / (double)x.second : 0.0,
                (unsigned long long)a.first, (unsigned long long)a.second,
                (unsigned long long)b.first, (unsigned long long)b.second);
    // The SHAPE of the answer, not just its size: where the eighths actually
    // are. A healthy body is "surface cells hold 1..8, submerged cells hold 8".
    uint64_t surfH[9] = {0}, subH[9] = {0};
    for (const IVec3& wc : g_parkBoxIds) {
      const CachedChunk* cc = world.Cached(wc);
      if (!cc || cc->voxels.size() != kChunkVol) continue;
      for (uint32_t z = 0; z < kChunk; z++)
        for (uint32_t y = 0; y + 1 < kChunk; y++)
          for (uint32_t xx = 0; xx < kChunk; xx++) {
            const uint32_t w = cc->voxels[(z * kChunk + y) * kChunk + xx];
            const uint32_t m = w & 0xFFFu;
            if (m == 0 || m >= mats.size()) continue;
            if (mats[m].gpu.klass != CLASS_LIQUID) continue;
            const uint32_t f = ((w >> 12) & 0xFu) + 1u;
            const uint32_t a2 = cc->voxels[(z * kChunk + y + 1) * kChunk + xx];
            if ((a2 & 0xFFFu) == m) subH[f]++; else surfH[f]++;
          }
    }
    std::printf("park[%s]: fullness histogram (eighths 1..8) surface:", label);
    for (int i = 1; i <= 8; i++)
      std::printf(" %llu", (unsigned long long)surfH[i]);
    std::printf("  submerged:");
    for (int i = 1; i <= 8; i++)
      std::printf(" %llu", (unsigned long long)subH[i]);
    std::printf("\n");
  }

  auto inertOnly = [&](const std::vector<IVec3>& ids) {
    uint32_t n = 0, tot = 0;
    for (const IVec3& wc : ids) {
      const CachedChunk* cc = world.Cached(wc);
      if (!cc || cc->voxels.size() != kChunkVol) continue;
      tot++;
      bool anyAct = false, anyMatter = false;
      for (uint32_t w : cc->voxels) {
        const uint32_t mi = w & 0xFFFu;
        if (mi == 0 || mi >= mats.size()) continue;
        anyMatter = true;
        const MaterialGpu& g = mats[mi].gpu;
        if (g.klass != CLASS_SOLID || g.reactCount > 0 ||
            (g.stainPack & 0x7u) != 0) { anyAct = true; break; }
      }
      if (anyMatter && !anyAct) n++;
    }
    return std::pair<uint32_t, uint32_t>(n, tot);
  };
  {
    auto ia = inertOnly(g_parkActiveIds), ib = inertOnly(g_parkIdleIds);
    std::printf("park[%s]: all-inert chunks (matCanAct false everywhere): "
                "active %u/%u  idle %u/%u\n", label, ia.first, ia.second,
                ib.first, ib.second);
  }
  std::printf("park[%s]: material presence over %u ACTIVE vs %u IDLE chunks "
              "(pct of chunks containing the material)\n", label, na, nb);
  struct Row { double act, idle; size_t id; };
  std::vector<Row> rows;
  for (size_t m = 1; m < mats.size(); m++) {
    if (a[m] == 0 && b[m] == 0) continue;
    rows.push_back({na ? 100.0 * a[m] / na : 0.0,
                    nb ? 100.0 * b[m] / nb : 0.0, m});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
    return (x.act - x.idle) > (y.act - y.idle);
  });
  for (const Row& r : rows) {
    if (r.act - r.idle < 1.0 && r.act < 20.0) continue;  // noise floor
    std::printf("park[%s]:   %-20s id %3zu  active %5.1f%%   idle %5.1f%%   "
                "delta %+6.1f\n", label, mats[r.id].name.c_str(), r.id, r.act,
                r.idle, r.act - r.idle);
  }
}

// One-shot per-tick body of the park probe. Prints the decay curve, then at a
// fixed tick pulls the still-active chunks' voxels back through the ordinary
// on-demand fetch ring (64/tick, no blocking readback in the frame path) and
// histograms which materials they contain against an equal-sized control.
void ParkProbe(World& world, const std::vector<MaterialDef>& mats, uint32_t tick) {
  const WorldSnapshot& s = world.Snap();
  if (!s.valid) return;
  // THE DETERMINISTIC ARM. Everything else here is measured under flight, and
  // flight is a bad differential: the route is dt-integrated, so two runs of
  // the same command fly over different terrain and the active-chunk mean
  // moves +-15% between them — larger than the effects being tested. The
  // FIRST FEW TICKS have no such problem. The initial worldgen covers all
  // 32,768 chunks at a fixed origin, so "how many chunks did the wake
  // predicate wake" is one exact number that does not vary at all between
  // runs. Any change to that predicate shows up here first, and cleanly.
  if (tick <= 6u)
    std::printf("park: postgen t%u active %u (snap t%u)\n", tick,
                s.activeChunks, s.tick);
  // MID-FLIGHT sample, taken while the window is still shifting: this is the
  // regime the 550-chunk p50 was measured in, and it is the one that decides
  // whether narrowing genChunk's wake predicate is worth anything.
  if (tick == ParkFlyTicks() - 120u) ParkSampleRequest(world, "fly");
  if (tick == ParkFlyTicks() - 80u) ParkSampleReport(world, mats, "fly");
  const uint32_t fetchTick = ParkFlyTicks() + ParkSettleTicks();
  if (tick >= ParkFlyTicks() && (tick % 25u) == 0u)
    std::printf("park: t%u  active %u  (snap t%u)\n", tick, s.activeChunks,
                s.tick);
  if (tick == fetchTick) ParkSampleRequest(world, "parked");
  if (tick == fetchTick + 40u) {
    ParkSampleReport(world, mats, "parked");
    g_parkDone = true;
  }
}

// ---- body-condition HUD mirror (ui/overlay.h UIState::body) -----------------
// BodySlotFor moved to game/session.h+.cpp (N5): the tick body and the
// character screen both ask, and those are two files now.

// Human-readable name per figure slot, for the character screen's injury list.
// Indexed by UIState::BodySlot, so it is one table beside the enum rather than
// a string built from the rig's part names — a rig's authored name ("armL.R")
// is a content identifier and has no business being shown to a player.
const char* BodySlotLabel(int slot) {
  static const char* k[UIState::kSlotCount] = {
      "Head",         "Torso",         "Hips",
      "Left upper arm", "Left forearm", "Left hand",
      "Right upper arm", "Right forearm", "Right hand",
      "Left thigh",   "Left shin",     "Left foot",
      "Right thigh",  "Right shin",    "Right foot"};
  return (slot >= 0 && slot < UIState::kSlotCount) ? k[slot] : "?";
}

// ---- BURN DAMAGE IS MATERIAL IDENTITY, NOT A FIELD -------------------------
//
// A limb that has been in a fire does not carry a "burned" number: its voxels
// have REACTED, skin -> flesh_cooked -> flesh_burning -> flesh_charred -> ash,
// and the same ladder exists for cloth. So "this arm is 40% charred" is a
// count over the limb's own voxels by material id, and needs no new engine
// state at all — which is why these ids are resolved ONCE (after every
// material load, never per frame: PartMaterialCount is a scan, and a material
// name lookup on top of it every frame for every limb would be gratuitous).
struct BurnMats {
  std::vector<uint32_t> cooked;   // "on the way": cooked + actively burning
  std::vector<uint32_t> charred;  // "gone": charred + ash
};

struct TissueMats {
  uint32_t skin = 0, flesh = 0, muscle = 0, bone = 0, brain = 0;
  // ROT IS A LIST, not an id. The four tissues above are the ones a human rig
  // is BUILT from, so each is one authored name; what a bite leaves behind is
  // whatever the attacking effect named as its `infect` material
  // (assets/mobs/effects/zombie.json: "rotflesh"), and a second undead with a
  // second rot material would simply be a second entry here. The tag is the
  // contract — `matInfectious_` in mob.cpp reads the same one to decide
  // whether a graft latches an infection, so the readout and the mechanic
  // cannot disagree about what counts as rot.
  std::vector<uint32_t> rot;
};

TissueMats ResolveTissueMats(const MobSystem& mobs,
                             const std::vector<MaterialDef>& mats) {
  TissueMats t;
  t.skin   = mobs.MaterialIdNamed("skin");
  t.flesh  = mobs.MaterialIdNamed("flesh");
  t.muscle = mobs.MaterialIdNamed("muscle");
  t.bone   = mobs.MaterialIdNamed("bone");
  t.brain  = mobs.MaterialIdNamed("brain");
  for (size_t i = 0; i < mats.size(); i++) {
    for (const std::string& tag : mats[i].tags) {
      if (tag == "infectious") { t.rot.push_back((uint32_t)i); break; }
    }
  }
  return t;
};

BurnMats ResolveBurnMats(const std::vector<MaterialDef>& mats) {
  BurnMats bm;
  // Never hardcoded by id (CLAUDE.md conventions): the stage is AUTHORED on
  // the material (materials.json "burnStage"), and it is the same field the
  // burn cap (sim/tuning.h Gore §G) counts with, so the HUD's per-limb readout
  // and the creature's own health cap cannot disagree about what "burnt" is.
  for (size_t i = 0; i < mats.size(); i++) {
    const uint8_t stage = mats[i].burnStage;
    if (stage == 1) bm.cooked.push_back((uint32_t)i);
    if (stage == 2) bm.charred.push_back((uint32_t)i);
  }
  return bm;
}

// ---- WHAT IS ON A BODY, AS THE HUD HAS TO DRAW IT ---------------------------
//
// The dominant coat material's colour: its authored `stain.color`, or its
// darkest palette entry when it declares none — which is the very fallback
// ParseStain itself applies, so the HUD and the world agree on what dried
// blood looks like without either of them naming a hue.
//
// NO SWIZZLE, deliberately. materials.cpp's ParseColor stores 0xAABBGGRR so
// the shader can unpack R out of the low byte, and that is byte-for-byte
// ImGui's default IM_COL32 packing. The only thing that has to be forced is
// ALPHA: a palette entry's is whatever the author wrote, and a stain drawn at
// the liquid's own alpha would vanish. 0 means "nothing to draw".
uint32_t CoatColorOf(const std::vector<MaterialDef>& mats, uint32_t mat) {
  if (mat == 0 || mat >= mats.size()) return 0;
  uint32_t c = mats[mat].gpu.stainColor;
  if ((c & 0x00FFFFFFu) == 0) c = mats[mat].gpu.color1;
  if ((c & 0x00FFFFFFu) == 0) return 0;
  return (c & 0x00FFFFFFu) | 0xFF000000u;
}

// The material's authored NAME, copied into the UI's own buffer. Copied and
// not pointed at because R replaces `mats` wholesale (see BodyPartUI).
void CopyCoatLabel(const std::vector<MaterialDef>& mats, uint32_t mat,
                   char* out, size_t n) {
  out[0] = '\0';
  if (mat == 0 || mat >= mats.size()) return;
  std::snprintf(out, n, "%s", mats[mat].name.c_str());
}

// The F1 panel's material mirror: names, swatch colours, class and tags, all
// index == material id. Rebuilt at load and on every R.
void FillUiMaterials(const std::vector<MaterialDef>& mats, UIState& ui) {
  ui.materialNames.clear();
  ui.materialColors.clear();
  ui.materialColors1.clear();
  ui.materialColors2.clear();
  ui.materialClass.clear();
  ui.materialTags.clear();
  for (const MaterialDef& m : mats) {
    ui.materialNames.push_back(m.name);
    ui.materialColors.push_back(m.gpu.color0);
    ui.materialColors1.push_back(m.gpu.color1);
    ui.materialColors2.push_back(m.gpu.color2);
    ui.materialClass.push_back((uint8_t)m.gpu.klass);
    std::string tags;
    for (const std::string& t : m.tags) tags += (tags.empty() ? "" : ",") + t;
    ui.materialTags.push_back(std::move(tags));
  }
}

// One figure slot's coat, folded from the limbs drawn as that segment.
//
// ACCUMULATED PER SLOT rather than assigned per limb, because BodySlotFor is
// many-to-one: every non-hip spine part lands on the torso. Reading the coat
// off whichever limb happened to be last would make a two-part torso report
// half the blood that is on it.
struct SlotCoat {
  uint32_t voxels = 0, sumAmt = 0;
  // Four candidate substances, folded from each contributing limb's own two.
  // One more level of the approximation LimbCoat already documents: a body is
  // realistically bloody, or wet, or bloody and wet, and a fifth distinct
  // substance on one segment contributes to the totals but is not named.
  uint32_t mat[4] = {}, amt[4] = {};
  void Add(uint32_t m, uint32_t a) {
    if (!m || !a) return;
    for (int k = 0; k < 4; k++) {
      if (mat[k] == m) { amt[k] += a; return; }
      if (mat[k] == 0) { mat[k] = m; amt[k] = a; return; }
    }
  }
  uint32_t Top() const {
    uint32_t best = 0, bestAmt = 0;
    for (int k = 0; k < 4; k++)
      if (mat[k] && amt[k] > bestAmt) { best = mat[k]; bestAmt = amt[k]; }
    return best;
  }
};

void FillBodyUI(const PlayerAvatar& avatar, const BurnMats& burnMats,
                const TissueMats& tissueMats,
                const MobSystem& mobs, const std::vector<MaterialDef>& mats,
                UIState& ui) {
  for (int i = 0; i < UIState::kSlotCount; i++) ui.body[i] = {};
  for (int i = 0; i < UIState::kSlotCount; i++)
    ui.body[i].label = BodySlotLabel(i);
  ui.stainFrac = 0.0f;
  ui.stainMat = 0;
  ui.stainColor = 0;
  ui.stainLabel[0] = '\0';
  for (auto& c : ui.coats) c = {};
  ui.coatCount = 0;
  ui.stainHudMin = CurrentTuning().coat.hudMinFrac;
  const MobDef* def = avatar.Def();
  ui.bodyValid = def != nullptr;
  if (!def) return;
  const uint64_t mobId = avatar.Id();
  SlotCoat slotCoat[UIState::kSlotCount];
  // ONE WALK PER LIMB. Every material this readout counts gets a bit in a
  // 4096-entry mask table, and PartMaterialTally bins the limb's voxels in a
  // single pass. It was one full walk of the skin PER MATERIAL — two burn
  // lists, five tissues and the rot list, ten-plus walks of every limb every
  // frame. A MASK, not a bin index, so a material in two lists (a cooked id
  // that is also a named tissue) still counts into both, exactly as the
  // separate PartMaterialCount calls did.
  enum : int { kBinCooked, kBinCharred, kBinSkin, kBinFlesh, kBinMuscle,
               kBinBone, kBinBrain, kBinRot, kBinCount };
  std::array<uint16_t, 4096> binMask{};
  auto mark = [&](uint32_t m, int bin) { binMask[m & 0xFFFu] |= (uint16_t)(1u << bin); };
  for (uint32_t m : burnMats.cooked) mark(m, kBinCooked);
  for (uint32_t m : burnMats.charred) mark(m, kBinCharred);
  if (tissueMats.skin)   mark(tissueMats.skin, kBinSkin);
  if (tissueMats.flesh)  mark(tissueMats.flesh, kBinFlesh);
  if (tissueMats.muscle) mark(tissueMats.muscle, kBinMuscle);
  if (tissueMats.bone)   mark(tissueMats.bone, kBinBone);
  if (tissueMats.brain)  mark(tissueMats.brain, kBinBrain);
  for (uint32_t m : tissueMats.rot) mark(m, kBinRot);
  // Walk the DEF's limbs, not PartCount() — the latter includes the borrowed
  // held-item slot, which is not part of the body.
  const int limbCount = (int)def->limbs.size();
  for (int i = 0; i < limbCount; i++) {
    const int slot = BodySlotFor(avatar.PartName(i), avatar.PartTag(i));
    if (slot < 0) continue;
    UIState::BodyPartUI& b = ui.body[slot];
    b.present = true;
    b.hpMax = avatar.PartHpMax(i);
    if (!avatar.PartAlive(i)) {
      b.severed = true;
      b.hpFrac = 0.0f;
      b.hp = 0.0f;
      b.voxelFrac = 0.0f;
      continue;  // a lost limb neither bleeds nor reports damage
    }
    const float max = avatar.PartHpMax(i);
    const float hp = avatar.PartHp(i);
    b.hp = hp;
    b.hpFrac = max > 0.0f ? hp / max : 1.0f;
    if (b.hpFrac < 0.0f) b.hpFrac = 0.0f;
    if (b.hpFrac > 1.0f) b.hpFrac = 1.0f;
    b.bleeding = avatar.PartBleeding(i);
    b.burningVoxels = avatar.PartBurningCount(i);

    const uint32_t spawn = avatar.PartVoxelsAtSpawn(i);
    const uint32_t now = avatar.PartVoxelCount(i);
    b.voxelFrac = spawn > 0 ? std::clamp((float)now / (float)spawn, 0.0f, 1.0f)
                            : 1.0f;
    uint32_t bins[kBinCount] = {};
    if (now > 0) avatar.PartMaterialTally(i, binMask.data(), bins);
    if (now > 0) {
      const uint32_t cooked = bins[kBinCooked], charred = bins[kBinCharred];
      // Charred counts double against "intact-looking": cooked flesh is still
      // flesh, charred flesh is structurally gone. Reported as one fraction
      // because the player's question is "how much of this limb is ruined",
      // not "which rung of the reaction ladder is it on".
      b.charredFrac =
          std::clamp((float)(cooked + charred * 2) / (float)(now * 2), 0.0f,
                     1.0f);
    }

    b.voxelTotal = now;
    // (A limb with no voxels tallied nothing, which is the 0 every one of
    // these per-material counts returned for it.)
    if (tissueMats.skin)   b.voxelSkin   = bins[kBinSkin];
    if (tissueMats.flesh)  b.voxelFlesh  = bins[kBinFlesh];
    if (tissueMats.muscle) b.voxelMuscle = bins[kBinMuscle];
    if (tissueMats.bone)   b.voxelBone   = bins[kBinBone];
    if (tissueMats.brain)  b.voxelBrain  = bins[kBinBrain];
    // Rot is tissue the limb is made of NOW, exactly like the four above — a
    // bitten arm's flesh bar shrinking with nothing taking its place was the
    // whole defect: the voxels are still in voxelTotal, they had just stopped
    // being anything the panel had a name for.
    b.voxelRot = bins[kBinRot];
    b.voxelBrainMax = avatar.PartBrainAtSpawn(i);

    // What is ON the limb. The ledger is recounted by the creature itself at
    // its own bounded cadence (Mob::RecountCoat), so this is a read of three
    // words per limb per frame and a clean body costs nothing.
    if (const LimbCoat* lc = mobs.LimbCoatOf(mobId, i)) {
      SlotCoat& sc = slotCoat[slot];
      sc.voxels += lc->voxels;
      sc.sumAmt += lc->sumAmt;
      for (const CoatEntry& e : lc->top) sc.Add(e.mat, e.sumAmt);
    }
  }

  for (int i = 0; i < UIState::kSlotCount; i++) {
    const SlotCoat& sc = slotCoat[i];
    if (sc.voxels == 0) continue;
    UIState::BodyPartUI& b = ui.body[i];
    // The same amount-weighted fraction LimbCoat::Frac computes, over the
    // union of the limbs this segment draws.
    b.stainFrac = std::clamp(
        (float)sc.sumAmt / (float)(kBodyStainAmtMax * sc.voxels), 0.0f, 1.0f);
    b.stainMat = sc.Top();
    b.stainColor = CoatColorOf(mats, b.stainMat);
    CopyCoatLabel(mats, b.stainMat, b.stainLabel, sizeof b.stainLabel);
  }

  // Body-level, from the creature's own ledger rather than by averaging the
  // slots: BodyCoat is over the BASE limbs, so a blood-soaked robe does not
  // report the wearer as covered, and re-deriving it here would be a second
  // answer to a question that already has one.
  const LimbCoat whole = mobs.BodyCoat(mobId);
  ui.stainFrac = std::clamp(whole.Frac(), 0.0f, 1.0f);
  ui.stainMat = whole.top[0].mat;
  ui.stainColor = CoatColorOf(mats, ui.stainMat);
  CopyCoatLabel(mats, ui.stainMat, ui.stainLabel, sizeof ui.stainLabel);
  static_assert(UIState::kCoatNames == kCoatTop,
                "UIState::coats mirrors LimbCoat::top");
  for (const CoatEntry& en : whole.top) {
    if (en.mat == 0 || en.sumAmt == 0 || whole.voxels == 0) continue;
    UIState::CoatName& c = ui.coats[ui.coatCount++];
    c.mat = en.mat;
    c.frac = std::clamp((float)en.sumAmt / (float)(kBodyStainAmtMax * whole.voxels),
                        0.0f, 1.0f);
    c.color = CoatColorOf(mats, en.mat);
    CopyCoatLabel(mats, en.mat, c.label, sizeof c.label);
  }
}

// ---- the character panel's live avatar portrait -----------------------------
//
// A SECOND CAMERA POINTED AT THE PLAYER'S OWN RIG, rendered offscreen once per
// frame while the screen is open and sampled by ImGui. It is the whole reason
// the panel needs no portrait ART: the avatar is one live copy-on-write
// micro-voxel body, so missing voxels, char/cook material transitions, severed
// limbs, dismemberment poses and whatever is in its hand all appear for free.
// Nothing in the panel knows about any of them.
//
// The recipe is --shot-mob's `shoot` lambda: place a camera, write the render
// params, open a pass on an offscreen view, draw the bodies, submit. The one
// real cost is the SECOND SUBMIT — world.renderUBO is a single buffer, so two
// cameras cannot share one command buffer, and each write has to be followed
// by its own submit for the deferred upload to land in front of the right pass.
// The camera is held as a real `Camera` (yaw/pitch) rather than as a hand-built
// basis, and that is load-bearing: WriteRenderParams derives camRight/camUp/
// camFwd from a Camera, so the basis the SHADER marches with and the basis the
// inspector projects with are the same three lines of code. A second
// hand-rolled basis here would only have to agree with Camera::Right()'s
// handedness — and getting that backwards produces a mirror image, which reads
// as "the outline is on the wrong arm" rather than as a maths bug.
struct PortraitCam {
  bool valid = false;
  Camera cam;
  Vec3 eye{}, target{};
  float tanHalf = 0.5f, aspect = 1.0f;
};

// The eight corners of a limb's oriented collider box, in world voxels. Used
// both to FRAME the portrait and to outline limbs on it — one helper so the
// two can never disagree about where a limb is.
bool LimbBoxCorners(const PlayerAvatar& av, Physics& phys, int part,
                    Vec3 out[8]) {
  const uint64_t body = av.PartBody(part);
  if (!body) return false;
  Vec3 pos;
  Quat rot;
  if (!av.PartWorldTransform(part, pos, rot)) return false;
  Vec3 lo, hi;
  // Body-origin-local, centre of mass already baked in — the same convention
  // rigrender::AppendDebugBox relies on, which is why the debug overlay and
  // this agree about where a limb is.
  if (!phys.GetLocalBounds(body, lo, hi)) return false;
  for (int i = 0; i < 8; i++) {
    const Vec3 c{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y,
                 (i & 4) ? hi.z : lo.z};
    out[i] = pos + QuatRotate(rot, c);
  }
  return true;
}

// LimbBoxes moved to game/session.h (N5): DeathBody holds one, and
// DeathBody is part of the presentation seam TickAuthority writes to.

// The LIVE rig's boxes. Limb parts only (`Def()->limbs`): a held weapon lives
// in an appended part slot, and framing the portrait on the sword would push
// the body itself out of shot every time it is drawn.
void CollectLimbBoxes(const PlayerAvatar& av, Physics& phys, LimbBoxes& out) {
  out.boxes.clear();
  if (!av.Spawned() || !av.Def()) return;
  const int limbCount = (int)av.Def()->limbs.size();
  for (int i = 0; i < limbCount; i++) {
    LimbBoxes::Box b;
    if (!LimbBoxCorners(av, phys, i, b.c)) continue;
    b.slot = BodySlotFor(av.PartName(i), av.PartTag(i));
    out.boxes.push_back(b);
  }
}

// Frame the LIVE body, not the def's box. A def-sized frame is wrong twice
// over: a rig that has lost both legs is half the height it was authored at,
// and the origin of a heavily dismembered body is nowhere near the part of it
// you can still see (the same lesson --shot-mob learned about corpses).
PortraitCam MakePortraitCam(const LimbBoxes& lb, float yaw,
                            float pitch, float aspect, float zoom,
                            float panX, float panY, int pivotSlot = -1) {
  PortraitCam pc;
  Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
  Vec3 pivotLo{1e9f,1e9f,1e9f}, pivotHi{-1e9f,-1e9f,-1e9f};
  bool anyPivot = false;
  bool any = false;
  for (const LimbBoxes::Box& box : lb.boxes) {
    any = true;
    const int s = box.slot;
    for (const Vec3& p : box.c) {
      lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
      hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
      if (s == pivotSlot) {
        pivotLo = Vec3{std::min(pivotLo.x,p.x),std::min(pivotLo.y,p.y),std::min(pivotLo.z,p.z)};
        pivotHi = Vec3{std::max(pivotHi.x,p.x),std::max(pivotHi.y,p.y),std::max(pivotHi.z,p.z)};
        anyPivot = true;
      }
    }
  }
  if (!any) return pc;

  pc.target = anyPivot ? (pivotLo + pivotHi) * 0.5f : (lo + hi) * 0.5f;
  pc.tanHalf = std::tan(CurrentTuning().camera.fovY * 0.5f);
  pc.aspect = aspect;
  const Vec3 half = (hi - lo) * 0.5f;
  const float halfH = std::max(0.5f, half.y);
  const float radiusXZ = std::max(0.5f, std::max(half.x, half.z));
  const float fitV = halfH / pc.tanHalf;
  const float fitH = radiusXZ / (pc.tanHalf * std::max(aspect, 1e-3f));
  // THE MARGIN IS PAYING FOR THE SKIN, not for taste. These are COLLIDER
  // boxes and the micro skin is drawn outside them — pauldrons, boots, a hat —
  // so a frame fitted to the boxes clips the body. 1.28 was exactly enough for
  // an intact human, where the head's box carries the error, and not nearly
  // enough for a decapitated one, where the shoulders are the top of the box
  // list and their armour stands above it: the death portrait came out with
  // the torso cut off at the frame edge.
  const float dist = std::max(fitV, fitH) * 1.45f / std::max(zoom, 0.1f);

  pc.cam.yaw = yaw;
  pc.cam.pitch = pitch;
  // Pan: shift the target along the camera's right and up axes, scaled by the
  // body's half-height so the pan amount is body-size-invariant.
  pc.target = pc.target + pc.cam.Right() * (panX * halfH) +
              pc.cam.Up() * (panY * halfH);
  pc.eye = pc.target - pc.cam.Forward() * dist;
  pc.valid = true;
  return pc;
}

// World point -> portrait-normalized (0,0 top-left .. 1,1 bottom-right).
//
// This is the INVERSE of raymarch.wgsl's primary ray, which builds
//   dir = normalize(camFwd + camRight * (ndc.x * tanHalfFov * aspect)
//                          + camUp    * (ndc.y * tanHalfFov))
// over a full-screen triangle whose `uv` IS clip space, under a
// negative-height viewport (vk_record.cpp) — so ndc.y = +1 is the TOP of the
// image. Getting that flip wrong shows up as an inspector that outlines the
// feet when you damage the head, which is why it is spelled out here.
bool ProjectToPortrait(const PortraitCam& pc, const Vec3& p, float out[2]) {
  const Vec3 fwd = pc.cam.Forward(), right = pc.cam.Right(), up = pc.cam.Up();
  const Vec3 d = p - pc.eye;
  const float z = d.dot(fwd);
  if (z <= 0.05f) return false;                  // behind, or on the plane
  const float ndcX = d.dot(right) / (z * pc.tanHalf * pc.aspect);
  const float ndcY = d.dot(up) / (z * pc.tanHalf);
  out[0] = 0.5f + 0.5f * ndcX;
  out[1] = 0.5f - 0.5f * ndcY;
  return true;
}

// Fill each figure slot's projected outline. Runs after FillBodyUI, and only
// while the inspector is showing — projecting 15 boxes is cheap but it is not
// free, and nothing reads the result otherwise.
void ProjectBodyUI(const LimbBoxes& lb, const PortraitCam& pc, UIState& ui) {
  for (int i = 0; i < UIState::kSlotCount; i++) ui.body[i].projValid = false;
  if (!pc.valid) return;
  for (const LimbBoxes::Box& box : lb.boxes) {
    const int slot = box.slot;
    if (slot < 0) continue;
    float mn[2] = {1e9f, 1e9f}, mx[2] = {-1e9f, -1e9f};
    int hits = 0;
    for (const Vec3& p : box.c) {
      float uv[2];
      if (!ProjectToPortrait(pc, p, uv)) continue;
      hits++;
      mn[0] = std::min(mn[0], uv[0]);
      mn[1] = std::min(mn[1], uv[1]);
      mx[0] = std::max(mx[0], uv[0]);
      mx[1] = std::max(mx[1], uv[1]);
    }
    // A partially-clipped box would report a bound built from the corners that
    // happened to survive, which is a smaller rectangle in the wrong place.
    // All eight or nothing.
    if (hits != 8) continue;
    UIState::BodyPartUI& b = ui.body[slot];
    // Several rig limbs can map to one figure slot; take the UNION so the
    // outline covers the whole thing the label names.
    if (b.projValid) {
      mn[0] = std::min(mn[0], b.projMin[0]);
      mn[1] = std::min(mn[1], b.projMin[1]);
      mx[0] = std::max(mx[0], b.projMax[0]);
      mx[1] = std::max(mx[1], b.projMax[1]);
    }
    b.projMin[0] = mn[0];
    b.projMin[1] = mn[1];
    b.projMax[0] = mx[0];
    b.projMax[1] = mx[1];
    b.projValid = true;
  }
}

// ---- the spawn site (docs/PLAN_environment_truth.md P-C) --------------------
// The game starts on the map's kind "spawn" site (worldmap.h kHSpawnX/Z), not
// at a literal: the harness pad (-128..640)^2 refuses every tree, crown, tarn
// and cover so the fixtures keep their ground, and a player who starts inside
// it sees 77 m of bare grass whatever the biome says. The fixtures do not
// move; the player does. `SpawnWindowOrigin` centres the residency window on
// that column BEFORE the boot worldgen, so the first frame is generated
// around the player rather than streamed to them one chunk-plane at a time
// (Stream::Update shifts one chunk per axis per frame).
IVec3 SpawnWindowOrigin() {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  const int half = (int)kNChunk / 2;
  int sx = m.spawnX, sz = m.spawnZ;
  // --fell-tree x,z starts the game at its site: without this the boot window
  // sat at the map's spawn and the stream WALKED it to the pad one plane per
  // frame (205-234 shifts, 10 ms each, plus a worldgen plane per shift) --
  // over before the cut, but it owned the whole-run stream and worldgen rows.
  if (g_fellSiteSet) { sx = g_fellSiteX - 48; sz = g_fellSiteZ; }
  return IVec3{(sx >> 4) - half, 0, (sz >> 4) - half};
}
Vec3 SpawnPos() {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  const int h = World::TerrainHeight(m.spawnX, m.spawnZ, kDefaultSeed);
  return Vec3{(float)m.spawnX, (float)(h + 10), (float)m.spawnZ};
}

// --shot: minimal look-iteration harness. Worldgen, drain the far-field fill
// queue, settle briefly, write the three standard screenshots, exit — so
// render/look changes can be judged in seconds instead of the full selftest.
// Cameras deliberately match the selftest's so the two stay comparable.
int RunShots(GpuContext& ctx, World& world, Simulation& sim) {
  // THE WHOLE WINDOW PER TICK for the openness/irradiance refresh, in this
  // harness only. The grid's rolling refresh covers 256 slots a tick and a
  // worldgen zeroes every stamp, so a section that re-runs worldgen and
  // settles 40 ticks (the lava pool) had a grid for slots 256..10,495 and
  // nothing else — its rim rock gathered from unstamped blocks, i.e. from
  // nothing, and the P3 frames came out bit-identical with GI on and off. In
  // play the same 128-tick latency is four seconds after a load, which is
  // fine; a frame that is the evidence for a lighting phase is not.
  {
    Tuning t = CurrentTuning();
    t.render.opennessChunksPerFrame = (int)kNumChunks;
    SetCurrentTuning(t);
  }
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  // Centred on the WINDOW's centre chunk, as play centres it on the player.
  // This was a literal {8, 3, 8} from the kNChunk = 16 era — a quarter of the
  // way into today's window and 13 chunks below its centre.
  sandvox::RefillFarAround(ctx, world, sim, sandvox::WindowCentreChunk(world));
  // THE SUN BEFORE THE SETTLE TICKS. The openness walk (sim_openness.wgsl)
  // reads RenderParams for the key light when it deposits its off-screen
  // irradiance sample (docs/PLAN_gi.md §3), and 120 ticks of refresh cover
  // 30,720 of the window's 32,768 slots — so written here, the settle loop
  // is also what lights the grid for every frame below. Written after the
  // tick loop, every shot would have shown P1 from the resolve pass alone,
  // i.e. only what each camera's own patches had deposited. Any camera does;
  // the sun is what the walk reads.
  {
    const Tuning& tn = CurrentTuning();
    const uint32_t tpd = TicksPerDay(tn);
    const uint32_t sunTick =
        (uint32_t)((double)g_shotTimeOfDay * (double)tpd) % tpd;
    Camera c0;
    WriteRenderParams(ctx.queue, world, Vec3{128, 240, 128}, c0, 16.0f / 9.0f,
                      true, 11.7f, kFarFogDensity, 1080.0f, sunTick);
  }
  for (uint32_t t = 1; t <= 120; t++)  // powders settle so shots match play
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, {}, false, {8, 3, 8},
               false, false);
  ctx.WaitIdle();

  const uint32_t W = 1920, H = 1080;
  rhi::Texture offscreen = ctx.device.CreateTexture({W, H, 1}, rhi::TextureFormat::RGBA8Unorm, rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc, "offscreen");
  rhi::TextureView view = offscreen.CreateView();
  auto grab = [&](const char* path) {
    rhi::Buffer shot = CreateBuffer(ctx.device, (uint64_t)W * H * 4,
                                     rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                     "screenshot");
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    rhi::TexelCopyTexture srcT{};
    srcT.texture = offscreen;
    rhi::TexelCopyBuffer dstB{};
    dstB.buffer = shot;
    dstB.bytesPerRow = W * 4;
    dstB.rowsPerImage = H;
    rhi::Extent3D ext{W, H, 1};
    enc.CopyTextureToBuffer(srcT, dstB, ext);
    ctx.queue.Submit(enc.Finish());
    std::vector<uint8_t> pixels(W * H * 4);
    bool got = false;
    got = rhi::ReadBufferBlocking(ctx.device, shot, 0, pixels.data(), (size_t)(pixels.size()));
    const bool wrote = got && WriteBmpFile(path, pixels, W, H);
    if (wrote) std::printf("wrote %s\n", path);
    g_shotResults.push_back({path, wrote});
  };
  // Fixed, nonzero shot time: wave animation and flicker are driven by R.time,
  // so a time of 0 would show every shot at the one phase where the ripples
  // happen to be flat. Constant, so shots stay reproducible frame to frame.
  const float kShotTime = 11.7f;
  // Time of day for the shots. `--time 0..1` (0 = midnight, 0.5 = noon) maps
  // to the tick that lands on that phase, so the sky/sun/moon can be inspected
  // at any point in the cycle without waiting for the cycle to get there.
  // Defaults to mid-morning, which shows terrain lighting at a readable sun
  // angle rather than the flat overhead of noon.
  const Tuning& shotTun = CurrentTuning();
  uint32_t shotTicksPerDay = TicksPerDay(shotTun);
  uint32_t shotTick =
      (uint32_t)((double)g_shotTimeOfDay * (double)shotTicksPerDay) % shotTicksPerDay;
  // `tickOverride` is for the ONE frame that has to pin its own time of day
  // (the night sky, below): every other frame honours `--time` and passes -1.
  // A negative sentinel rather than a second lambda, because the four-frame
  // warm-up and the readback below are the part nobody should have a second
  // copy of.
  auto renderAt = [&](Vec3 eye, float yaw, float pitch, const char* path,
                      int64_t tickOverride) {
    // --shot-frames: the scene setup around a frame (a pour, a spawn, a
    // window relocation) still runs — it is what the NEXT frames stand on —
    // but the render and the readback, which are the expensive part, do not.
    if (!ShotWanted(path)) return;
    const uint32_t frameTick =
        tickOverride < 0 ? shotTick : (uint32_t)tickOverride;
    Camera c;
    c.yaw = yaw;
    c.pitch = pitch;
    // FOUR FRAMES, GRAB THE LAST. The shadow cache resolves a patch one frame
    // after a pixel first asks for it, and since P1 the resolve pass is also
    // what deposits the irradiance grid (docs/PLAN_gi.md §3) — so a single
    // frame per camera showed every shot with cold shadows and no bounce from
    // anything the camera itself was the first to see. Three warm frames at
    // ~10 ms each cost nothing against the readback. Fresh RenderParams per
    // frame: the frame counter inside them is what the cache's stamps advance
    // on.
    // The shading-LOD filter (render.denoise) runs in the shots exactly as it
    // does in play, in place over `offscreen` after the world pass — a look
    // pass that the look harness did not show would be untunable. Its params
    // are uploaded before the pass opens, like everything else here.
    const bool shotDenoise = CurrentTuning().render.denoise != 0;
    for (int f = 0; f < 4; f++) {
      WriteRenderParams(ctx.queue, world, eye, c, (float)W / H, true, kShotTime,
                        kFarFogDensity, 1080.0f, frameTick);
      if (shotDenoise) {
        sim.EnsureDenoise(W, H);
        sim.WriteDenoiseParams(ctx.queue, W, H,
                               std::tan(CurrentTuning().camera.fovY * 0.5f),
                               /*bgraSource=*/false);
      }
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      sim.EncodeShadowResolve(enc);
      rhi::RenderPass rp =
          sim.BeginRenderPass(enc, view, rhi::TextureFormat::RGBA8Unorm, W, H);
      sim.DrawWorld(rp);
      // Wind slope-field arrows, off unless wind.dbgWindField asks for them.
      // Headless cannot press F4, and the overlay's whole job is to be LOOKED
      // at — so the tuning bool is how a screenshot run reaches it, and the
      // wind block below is what uses that.
      sim.DrawWindField(rp, CurrentTuning().wind.dbgWindField
                                ? WindDebugArrowCount(CurrentTuning())
                                : 0u);
      // The CURRENT field's arrows, reached the same way and for the same
      // reason (water plan component 8).
      sim.DrawCurrentField(rp, CurrentTuning().render.dbgCurrentField
                                   ? CurrentDebugArrowCount()
                                   : 0u);
      rp.End();
      // A no-op until the first draw has built the pipeline (frame 0), so the
      // grabbed fourth frame is always filtered.
      if (shotDenoise)
        sim.EncodeDenoise(enc, offscreen, view, rhi::TextureFormat::RGBA8Unorm,
                          W, H);
      ctx.queue.Submit(enc.Finish());
    }
    ctx.WaitIdle();
    grab(path);
  };
  // The ordinary frame: honours `--time` like everything else always has.
  auto render = [&](Vec3 eye, float yaw, float pitch, const char* path) {
    renderAt(eye, yaw, pitch, path, -1);
  };
  int h108 = World::TerrainHeight(108, 108, kDefaultSeed);
  // Sky shot: aimed along the sun's azimuth and tilted up, so the frame holds
  // the sun disc, the halo, the scattering gradient AND long raking shadows on
  // the terrain below. The other shots deliberately face away from the sun, so
  // without this one the whole sky/sun path goes unreviewed.
  {
    SkyState ss = SkyForTick(shotTun, shotTick);
    // Camera::Forward() is (cos yaw, sin pitch, sin yaw), so yaw runs from +X
    // toward +Z — atan2(z, x), NOT atan2(x, z).
    float sunYaw = std::atan2(ss.sunDir[2], ss.sunDir[0]);
    // Pitch straight AT the sun so the disc, its limb darkening and the halo
    // are actually in frame — a shot merely pointed down-sun misses the disc
    // entirely and the whole sun path goes unreviewed.
    float sunPitch = std::asin(std::clamp(ss.sunDir[1], -1.0f, 1.0f));
    render({108, (float)(h108 + 40), 108}, sunYaw, sunPitch, "screenshot_sky.bmp");
  }
  // NIGHT SKY shot: the only frame that reviews the dome's night half — the
  // galactic band, the nebulae, the aurora curtains, the moons and the
  // starfield's DEPTH ORDER against all of them (raymarch.wgsl SkyLayer.veil).
  // Every other capture here is a daylight frame, so before this one the whole
  // night path was unreviewed and the stars sat visibly on top of the aurora
  // for as long as the aurora has existed.
  //
  // It PINS ITS OWN TICK rather than honouring `--time`, and that is the point:
  // a night frame that goes blue whenever someone runs `--shot --time 0.5`
  // reviews nothing.
  //
  // PITCH +0.35, and the number is load-bearing. nightGlow fades the aurora in
  // above rd.y = 0.02 and back out from 0.35, so its density peaks in a band
  // just above the horizon and is already down by a third at the zenith. An
  // earlier +0.55 framing looked like more sky and reviewed a THINNER aurora:
  // the densest block in that frame averaged 16/255 of purple, which is too
  // faint for any occlusion change to read. This aims at the band.
  {
    // 0.02 of a cycle past midnight: fully dark, and OFF the exact midnight
    // phase so a bug that only shows at dayT == 0 has no special case to hide
    // in. Both moons are wherever their own orbits put them, which is the
    // honest test of the disc/star occlusion — pinning them full would only
    // test the easy case.
    uint32_t nightTick =
        (uint32_t)(0.02 * (double)shotTicksPerDay) % shotTicksPerDay;
    renderAt({108, (float)(h108 + 60), 108}, 0.785f, 0.35f,
             "screenshot_night_sky.bmp", (int64_t)nightTick);
  }
  render({108, (float)(h108 + 120), 108}, 0.785f, -0.35f, "screenshot.bmp");
  render({140, 220, 140}, 0.785f, -0.20f, "screenshot_far.bmp");
  render({108, (float)(h108 + 28), 108}, 0.785f, -0.02f, "screenshot_ground.bmp");
  // CASCADE shot: the only capture here that is actually MOSTLY far field.
  // The two shots above look down from moderate height, so nearly every pixel
  // lands inside the residency window and the cascades barely appear — neither
  // could catch a far-field regression. Measured on this exact frame: 50% of
  // its pixels come from traceFar (checked by stubbing the far march out), so
  // it is the one that would notice.
  //
  // Both numbers below are load-bearing. The camera must be well ABOVE the
  // terrain, because the residency window is only half a window edge in radius
  // (25.6 m at kWorldN=512, kVoxelMeters=0.10) and any eye-height view is
  // walled in by near terrain — an earlier version of this shot sat at
  // h108+12 and differed by ONE pixel in 2,073,600 between two very different
  // cascade configurations, i.e. it tested nothing. And the pitch must be
  // near-horizontal: aimed steeply down, the frame fills with the window again.
  render({108, (float)(h108 + 300), 108}, 0.785f, -0.06f,
         "screenshot_cascade.bmp");
  // (The combat arena and its two screenshots went with the world map's P2b;
  // authored sites return through the map's site table in P5.)
  // Water look shots: the authored lake is centered at (420,420), surface at
  // y=68 (worldgen poolY 44 + 24), floor at y=44, rim y=70.
  //   _water: from the near rim at a shallow grazing angle — where Fresnel
  //           reflection and sun glint dominate.
  //   _water_down: from above looking down — the low-Fresnel angle, where
  //           refraction, depth absorption and the visible bed have to carry it.
  // Just above the surface at the near rim, looking across the lake: the
  // grazing angle where Fresnel reflection and sun glint dominate.
  // Birch look shot: the branching-skeleton species is the one tree whose
  // silhouette can't be judged from the general shots — it needs a single
  // specimen against the sky. Birch at (75,506), ground y=53, trunk 113.
  render({75 - 115, 53 + 85, 506 - 115}, 0.785f, -0.18f, "screenshot_birch.bmp");
  // THE SURFACE HEIGHT IS ASKED FOR, NOT WRITTEN DOWN, and that is a fix
  // rather than a tidy-up. Both of these cameras carried literal y values (80
  // and 88) from before the terrain overhaul moved `spawnPlainY` to 200: the
  // lake's surface is at y=209 on this tree, so both shots were rendering from
  // ~120 voxels inside solid rock and had been a flat grey rectangle for as
  // long as that. A screenshot nobody can tell is broken is worse than no
  // screenshot, and the only durable fix is to derive the number from the same
  // worldgen the water comes out of.
  {
    const World::Column lakeCol = World::TerrainColumn(420, 420, kDefaultSeed);
    const float surf = (float)(lakeCol.water != INT32_MIN ? lakeCol.water
                                                          : lakeCol.h);
    render({386, surf + 2.5f, 386}, 0.785f, -0.04f, "screenshot_water.bmp");
    // Standing over the middle looking down: the low-Fresnel angle, where
    // refraction, per-channel depth absorption and the visible bed carry it.
    render({420, surf + 24.0f, 462}, -1.571f, -0.70f,
           "screenshot_water_down.bmp");

    // ---- the current field and the waves it drives (plan components 8+9) ---
    // The shipped world has no drain in it, so without this the two things M4
    // built would go unreviewed in every screenshot run: a still lake looks
    // exactly the same whether or not a current field exists. So put a
    // whirlpool in the lake and shoot it twice — once bare, so the FLOW is
    // judged on the water (advected wave phase, foam on the convergence line),
    // and once with the arrow overlay on, which is the only picture that can
    // show the field is the shape it claims to be.
    //
    // The primitives are spawned directly rather than seeded from a drain
    // because --shot never ticks the sim: there is no dig, no ledger and no
    // hole here, and a screenshot path that had to drain a lake first would be
    // a different program.
    {
      const Tuning shotBase = CurrentTuning();
      CurrentPrims().Clear();
      CurrentPrim v;
      v.kind = kCurrentPrimVortex;
      v.x = 420; v.y = (int)surf; v.z = 420;
      v.radius = 44;
      v.reach = 30;
      v.swirlQ = 1 << 16;
      v.decayTicks = kCurrentPrimForever;
      v.ownerId = 1;
      CurrentPrimAim(v, Vec3{0.0f, -1.0f, 0.0f},
                     CurrentGammaToCoreMs(shotBase.sim.currentVortexGamma, 44));
      CurrentPrims().Spawn(v);
      CurrentPrim k;
      k.kind = kCurrentPrimSink;
      k.x = 420; k.y = (int)surf - 8; k.z = 420;
      k.radius = 16;
      k.reach = 16;
      k.decayTicks = kCurrentPrimForever;
      k.ownerId = 2;
      CurrentPrimAim(k, Vec3{0.0f, -1.0f, 0.0f}, shotBase.sim.currentSinkSpeed);
      CurrentPrims().Spawn(k);
      // Past the attack ramp, so the shot shows the field at full strength
      // rather than at a sixth of it.
      CurrentPrims().Tick(64);
      render({420, surf + 24.0f, 462}, -1.571f, -0.70f,
             "screenshot_water_flow.bmp");
      Tuning arrowT = shotBase;
      arrowT.render.dbgCurrentField = true;
      SetCurrentTuning(arrowT);
      render({420, surf + 24.0f, 462}, -1.571f, -0.70f,
             "screenshot_water_current.bmp");
      SetCurrentTuning(shotBase);
      CurrentPrims().Clear();
    }
  }
  // ---- SUBMERGED shots: the camera is INSIDE the lake ----
  // These are the only views that exercise shadeSubmerged (god rays, silt,
  // Snell's window, and the caustic web painted on the bed), and none of the
  // above reach it: every one of them is a ray entering the water from dry
  // air, which is a different code path entirely. The lake spans y=44 (floor)
  // to y=68 (surface), so an eye at y=56 is comfortably mid-column with both
  // the bed and the surface in reach.
  //   _sub_up:    looking up at the underside of the surface — Snell's window,
  //               the bright compressed disc of sky ringed by total internal
  //               reflection. The single most recognisable underwater cue.
  //   _sub_bed:   looking down/across at the lit bed — the caustic web is the
  //               whole subject of this frame.
  //   _sub_shaft: level and aimed toward the sun's azimuth, where the
  //               forward-scattering phase makes the light shafts brightest.
  render({420, 56, 420}, 0.785f, 1.20f, "screenshot_sub_up.bmp");
  render({412, 58, 412}, 0.785f, -0.55f, "screenshot_sub_bed.bmp");
  {
    SkyState ss = SkyForTick(shotTun, shotTick);
    float sunYaw = std::atan2(ss.sunDir[2], ss.sunDir[0]);
    render({414, 54, 414}, sunYaw, 0.35f, "screenshot_sub_shaft.bmp");
  }
  // ---- a GENERATED pond, with its vegetation ----
  // The authored lake above is a bare stone tub; it exercises the water and
  // submerged shading but has no plant life, because pond flora is placed by
  // pondAt() and the authored pools are explicitly excluded from it. This is
  // the one shot that shows lilypads, reeds and kelp, and the one that would
  // catch worldgen placing them somewhere absurd (floating, or on dry land).
  // Oil pond (260,300) and lava pool (220,520): the non-water liquid paths.
  // Oil exercises the palette-derived absorption; lava is MATF_OPAQUE and must
  // still render as a surface hit, untouched by any of the water work.
  // SUBMERGED IN OIL. The generic per-liquid profile (submergedProfile) has to
  // be judged on a liquid that is NOT water, and oil is the far end of the
  // range: opacity 235 against water's 90. What this frame must show is a
  // near-blind brown-black press — visibility about a metre, no Snell window,
  // no god rays, no silt — where the same code on water gives an 11 m view
  // with light shafts in it. If this looks like brown water, the clarity curve
  // is not separating them. The pool spans y=50 (floor) to y=68 (surface).
  //
  // Needs the window moved onto it, exactly like the pond shots at the end of
  // this function and for the same reason: the pool centre (260,300) sits
  // outside the origin window, and outside the window a liquid shades through
  // the far-field cascade as flat colour with no submerged path at all. Shot
  // from the origin window this frame is a grey slab of far-field stone.
  {
    world.SetWindowOrigin({260 / (int)kChunk - 8, 0, 300 / (int)kChunk - 8});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    for (uint32_t t = 1; t <= 40; t++)
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, {}, false, {8, 3, 8},
                 false, false);
    ctx.WaitIdle();
    // The oil SURFACE, from just above it at a grazing angle. Moved inside this
    // window-relocated block along with the submerged shot, and for the same
    // reason: from the origin window the pool is FAR-FIELD, which shades a
    // liquid as flat colour with no Fresnel, no reflection and no specular at
    // all - the exact terms this frame exists to judge. Shot from out there it
    // was a blurred grey haze.
    //
    // Grazing on purpose: oil's look is carried by the reflection and by a
    // tight glint, both Fresnel-weighted, so a top-down view is the one angle
    // where neither shows up. The camera has to sit just above the surface
    // (y=68) and INSIDE the rim ring, which is raised to y=70 out at radius 42
    // - anything beyond that is looking at the outside of a stone wall. The
    // fluid disc is only 32 voxels across, so it sits close to the middle.
    // Pitch is a compromise. Too shallow (-0.10) and the ray clears the far
    // rim entirely and the frame is sky; too steep and the Fresnel terms
    // vanish. -0.42 from 5 voxels up puts the far side of the 32-voxel disc
    // across the middle of the frame while still hitting it at a glancing
    // enough angle for the reflection and glint to read.
    render({260 - 24, 73, 300 - 24}, 0.785f, -0.42f, "screenshot_oil.bmp");
    render({260, 60, 300}, 0.785f, 0.10f, "screenshot_oil_sub.bmp");
    // Restore the origin window: the lava/blood/micro shots below all assume
    // it, and a regen here would otherwise silently relocate every one of them.
    world.SetWindowOrigin({0, 0, 0});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    for (uint32_t t = 1; t <= 40; t++)
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, {}, false, {8, 3, 8},
                 false, false);
    ctx.WaitIdle();
  }

  // ---- A TALL-GRASS STAND: the stacked swaying meadow grass ----
  // Tall grass is placed by patch noise in flowerAt() (worldgen.wgsl), and no
  // other shot frames a stand: the meadow around spawn is flowers and single-
  // cell tufts. The nearest dense stand to the origin sits at (336,96) —
  // located by porting vnoise/biomeAt to Python against seed 1337 — which is
  // outside the origin window, hence the same relocate-and-restore dance as
  // the oil pool above. Two frames: the stand as a dome rising out of the
  // lawn (the patch-ramped height doing its job), and an eye-level view into
  // the blades where the head cells' tan tips and the per-column height
  // jitter either read or don't. Both are stills; judging the SWAY needs the
  // live app, but a frame mid-gust still shows the sheared blades leaning.
  {
    world.SetWindowOrigin({336 / (int)kChunk - 8, 0, 96 / (int)kChunk - 8});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    for (uint32_t t = 1; t <= 40; t++)
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, {}, false, {8, 3, 8},
                 false, false);
    ctx.WaitIdle();
    int gh = World::TerrainHeight(336, 96, kDefaultSeed);
    render({296, (float)(gh + 16), 56}, 0.785f, -0.18f, "screenshot_tallgrass.bmp");
    render({322, (float)(gh + 6), 82}, 0.785f, 0.0f, "screenshot_tallgrass_eye.bmp");
    // ---- WIND: the slope-field overlay, over this same stand ----
    //
    // Two frames, and the PAIR is the evidence — either alone proves nothing.
    // Same eye, same tick, same sun, same grass. The ONLY difference between
    // them is the wind direction:
    //
    //   _wind      arrows over the stand at the authored direction
    //   _wind_rot  the same frame with windDirDeg turned 90 degrees
    //
    // What has to be visible in the second is that the ARROWS and the GRASS
    // LEAN rotated TOGETHER. That is the whole phase-1 claim in one picture:
    // the sway and the overlay are reading ONE field (windAt in common.wgsl),
    // not two implementations that happen to agree today.
    //
    // It is shot HERE, inside the relocated window, rather than at the origin,
    // because the meadow around spawn is flowers and single-cell tufts — the
    // lean of a one-cell tuft is a couple of pixels. This is the only stand in
    // the world tall enough for the comparison to be legible.
    //
    // weatherAuto is pinned OFF for both. Evolving weather would make the two
    // frames incomparable: a difference between them could be the knob or
    // could be the clock, and a shot that cannot separate those is not
    // evidence of anything.
    //
    // BOTH directions are held well off the camera's own axis, and that is
    // deliberate. An arrow aligned with the view ray carries no direction on
    // screen and the shader fades it out (see the axial fade in
    // debug_wind.wgsl), so a frame shot straight up-wind is a picture of a
    // hole. The camera looks along yaw 45 degrees, so the pair is taken at 90
    // and 180: one crossing left-to-right, one crossing the other way, 90
    // degrees apart as the comparison requires and neither degenerate.
    {
      const Tuning saved = CurrentTuning();
      Tuning tw = saved;
      tw.wind.dbgWindField = true;
      tw.wind.weatherAuto = false;
      // The eye-level camera, backed off and lifted a little: low enough that
      // individual blades still resolve, high enough that the arrow lattice
      // fills the frame. Both things being compared have to be legible at once.
      const Vec3 eye{318, (float)(gh + 8), 78};
      tw.wind.windDirDeg = 90.0f;
      SetCurrentTuning(tw);
      render(eye, 0.785f, -0.08f, "screenshot_wind.bmp");
      tw.wind.windDirDeg = 180.0f;
      SetCurrentTuning(tw);
      render(eye, 0.785f, -0.08f, "screenshot_wind_rot.bmp");
      SetCurrentTuning(saved);
    }
    world.SetWindowOrigin({0, 0, 0});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    for (uint32_t t = 1; t <= 40; t++)
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, {}, false, {8, 3, 8},
                 false, false);
    ctx.WaitIdle();
  }

  // ---- AN OIL SLICK ON WATER: the case that SHOULD show the rainbow ----
  // The pool shot above is oil on stone and must show NO iridescence - a deep
  // pool has no second interface within reach of the light, so there is no film
  // to interfere. This is the other half of that test: oil spilled onto the
  // water lake, where floatingOnLiquid() finds water underneath and the sheen
  // switches on. Two frames that differ only in what is UNDER the oil.
  //
  // Oil is density 900 against water's 1000, so the sim floats it without any
  // help here; the ops just place it at the surface and let it spread.
  {
    // The harness map's fixture lake (--shot loads the harness map). Its
    // surface is ASKED FOR: the literal y=68 this once carried predates the
    // datum move to y200 and put the slick ~140 voxels inside the rock.
    const int kLx = 420, kLz = 420;
    const World::Column lakeCol = World::TerrainColumn(kLx, kLz, kDefaultSeed);
    const int kSurf = lakeCol.water != INT32_MIN ? lakeCol.water : lakeCol.h;
    world.SetWindowOrigin({kLx / (int)kChunk - 8, 0, kLz / (int)kChunk - 8});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> slick;
    // A disc of oil laid ON the water surface. Deterministic, no rand(), like
    // every other look shot.
    for (int dx = -26; dx <= 26; dx++)
      for (int dz = -26; dz <= 26; dz++) {
        if (dx * dx + dz * dz > 26 * 26) continue;
        // ABOVE the surface, not AT it. Writing into the surface cell itself
        // replaces scattered water voxels with oil and leaves the rest water,
        // which at a grazing angle reads as a hard 1:1 checkerboard of two
        // very differently shaded liquids rather than as a slick. Dropped from
        // one voxel up, the oil settles into a continuous layer ON the water -
        // which is also the only configuration floatingOnLiquid() should fire
        // on, so it is the honest test.
        IVec3 c{kLx + dx, kSurf + 1, kLz + dz};
        if (!world.CellInWindow(c)) continue;
        // Same word rules as a brush paint: liquid born full, unstamped.
        uint32_t word = PackVoxNew(kMatOil, 7u);
        slick.push_back({World::SlotCellIndex(c), word});
      }
    // Long settle: the layer has to spread and level before it reads as one.
    for (uint32_t t = 1; t <= 200; t++)
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {},
                 t == 1 ? slick : std::vector<CellOp>{}, false, {8, 3, 8},
                 false, false);
    ctx.WaitIdle();
    // Grazing, like the pool shot, since the sheen is Fresnel-weighted.
    // Inside the slick, not outside it: the disc is radius 26 and a camera 40
    // voxels out looks straight past it at open water.
    render({(float)(kLx - 14), (float)(kSurf + 4), (float)(kLz - 14)}, 0.785f,
           -0.16f, "screenshot_oil_slick.bmp");

    // NOT {0, 0, 0}: the lava cameras below sit at z 496..546, and with the
    // window at 512 cells (it was 1024 when this section was written) an
    // origin of 0 puts them in the FAR CASCADE. z origin 8 chunks (128..639)
    // keeps them AND the sections after (the room at z 160, the blood scene
    // at z 150) inside the window. KNOWN STALE FIXTURE, found 2026-09-02 while
    // judging P3: the pool worldgen once authored at (220, 520) is not there
    // at this seed any more — the ground is at y ~211 and the three cameras
    // at y 70..86 are buried in rock, so the frames are flat dark facets with
    // GI on or off. The live lava frame is screenshot_lava_spatter (stamped
    // through the mutation queue on the surface); whoever re-authors the pool
    // should move these cameras with it.
    world.SetWindowOrigin({0, 0, 8});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    for (uint32_t t = 1; t <= 40; t++)
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, {}, false, {8, 3, 8},
                 false, false);
    ctx.WaitIdle();
  }
  // Lava pool (220,520), surface y=64, rim y=66: close and low, the angle
  // where crust structure and the glow from the cracks have to carry the look.
  render({196, 74, 496}, 0.785f, -0.30f, "screenshot_lava.bmp");
  // Looking down INTO the lava pool: the crust plates and crack network fill
  // the frame, which is the only way to judge them.
  render({220, 86, 546}, -1.571f, -0.80f, "screenshot_lava_down.bmp");
  // Low and close across the pool: the angle where embers rising off the
  // surface read against the far rim, and where the crust is seen at a grazing
  // angle rather than plan view.
  render({242, 70, 542}, -2.36f, -0.10f, "screenshot_lava_close.bmp");

  // ---- scattered lava: the laser-spatter case ----
  // Single isolated lava voxels have no surface for a crust to form on, so
  // shadeMolten's pooling term should fade them back to the simple emissive
  // look. Paint some on open ground next to the pool and shoot them, so the
  // two treatments can be compared in one pass.
  {
    std::vector<CellOp> spatter;
    int gx = 300, gz = 470;
    int gh = World::TerrainHeight(gx, gz, kDefaultSeed);
    for (int i = 0; i < 40; i++) {
      // deterministic scatter — no rand(), so the shot is reproducible
      int ox = ((i * 37) % 19) - 9;
      int oz = ((i * 53) % 23) - 11;
      int oy = ((i * 29) % 3);
      IVec3 c{gx + ox * 2, gh + 1 + oy, gz + oz * 2};
      if (!world.CellInWindow(c)) continue;
      // same word rules as a brush paint: liquid born full, unstamped
      uint32_t word = PackVoxNew(kMatLava, 7u);
      spatter.push_back({World::SlotCellIndex(c), word});
    }
    for (uint32_t t = 121; t <= 124; t++)
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {},
                 t == 121 ? spatter : std::vector<CellOp>{}, false, {8, 3, 8},
                 false, false);
    ctx.WaitIdle();
    render({(float)(gx - 26), (float)(gh + 14), (float)(gz - 26)}, 0.785f,
           -0.32f, "screenshot_lava_spatter.bmp");
  }


  // ---- openness: a roofed interior lit only through its doorway ----------
  // docs/PLAN_gi.md §2. The P0 grid's whole claim is that enclosure darkens
  // and open sky does not, and no camera in this harness could see either:
  // every existing frame is outdoors under an unobstructed hemisphere, which
  // is exactly the case the grid leaves BIT-IDENTICAL. So the subject is built
  // here, the same way the lava-spatter and blood scenes above are built.
  //
  // A SHELTER, NOT A CAVE. There is a cave band under this window (worldgen
  // caveBands), but its floor is tens of metres down and its location is a
  // noise threshold — a camera aimed at it would be a coordinate that goes
  // stale the first time a cave knob moves, which is the trap the water shots
  // above document at length. A stamped room has a doorway in a known wall, a
  // roof at a known height and open ground right outside it, so one frame
  // holds the dark interior, the lit doorway wedge and the untouched meadow
  // and the three can be compared against each other rather than against
  // memory.
  {
    const int gx = 240, gz = 160;
    const int gh = World::TerrainHeight(gx, gz, kDefaultSeed);
    const int kR = 14;     // interior half-extent, voxels
    const int kH = 22;     // interior height to the underside of the roof
    std::vector<CellOp> room;
    auto put = [&](int x, int y, int z) {
      IVec3 c{x, y, z};
      if (!world.CellInWindow(c)) return;
      room.push_back({World::SlotCellIndex(c), PackVoxNew(kMatStone, 0u)});
    };
    for (int x = -kR; x <= kR; x++)
      for (int z = -kR; z <= kR; z++) {
        // roof, two voxels thick so a ray cannot slip between layers
        put(gx + x, gh + kH, gz + z);
        put(gx + x, gh + kH + 1, gz + z);
      }
    for (int y = 1; y < kH; y++)
      for (int t = -kR; t <= kR; t++) {
        // Four walls, with a doorway punched in the -Z wall: the wedge of light
        // it throws on the floor is the thing to look at, because a grid that
        // only knows "indoors" would light the whole floor equally.
        const bool door = (t >= -3 && t <= 3 && y <= 12);
        if (!door) put(gx + t, gh + y, gz - kR);
        put(gx + t, gh + y, gz + kR);
        put(gx - kR, gh + y, gz + t);
        put(gx + kR, gh + y, gz + t);
      }
    for (uint32_t t = 131; t <= 140; t++)
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {},
                 t == 131 ? room : std::vector<CellOp>{}, false, {8, 3, 8},
                 false, false);
    ctx.WaitIdle();
    // From outside, past the doorway: the lit meadow, the shaded outer wall and
    // the dark interior in one frame. This is the frame that would show open
    // ground WRONGLY darkening next to a wall, if it did.
    render({(float)(gx - 4), (float)(gh + 8), (float)(gz - 40)}, 1.5708f, -0.10f,
           "screenshot_openness_out.bmp");
    // From inside, looking at the doorway. The floor gradient from the doorway
    // to the back wall is the P0 term and nothing else — there is no bounce
    // light in the engine yet, so anything visible here is openness.
    render({(float)gx, (float)(gh + 9), (float)(gz + kR - 4)}, -1.5708f, -0.12f,
           "screenshot_openness_in.bmp");
    // Straight down at the doorway floor from inside, where the 40 cm block
    // quantisation would show as tiling if the bilinear filter were not on.
    render({(float)gx, (float)(gh + kH - 3), (float)(gz - kR + 6)}, -1.5708f,
           -0.85f, "screenshot_openness_floor.bmp");
  }

  // ---- blood: the spatter case AND the pooled case, in one frame ----
  // Blood's whole shading problem is that it is usually NOT a still pool: it
  // comes out of NPCs as droplets, runs and thin trails. shadeViscous blends
  // between a droplet look and a pool look, so the shot has to contain both or
  // half the model goes unreviewed — and the failure mode being guarded
  // against here (every voxel shading as its own little cube) shows up on the
  // scattered droplets long before it shows up on a pool.
  //
  // Laid out as: a filled basin, a run of blood down a step, and a field of
  // isolated droplets, all in one view. Deterministic placement, no rand(),
  // so the shot is reproducible frame to frame like every other look shot.
  {
    std::vector<CellOp> gore;
    int gx = 340, gz = 300;
    int gh = World::TerrainHeight(gx, gz, kDefaultSeed);
    auto put = [&](int x, int y, int z, uint32_t mat) {
      IVec3 c{x, y, z};
      if (!world.CellInWindow(c)) return;
      uint32_t state = (mat == kMatAir) ? 0u : 7u;  // liquids are born full
      gore.push_back({World::SlotCellIndex(c), PackVoxNew(mat, state)});
    };
    // A stone basin holding a pool: the "still pool" end of the blend, and the
    // surface that the surrounding stone gets stained by.
    for (int z = -7; z <= 7; z++)
      for (int x = -7; x <= 7; x++) {
        bool rim = (x < -6 || x > 6 || z < -6 || z > 6);
        put(gx + x, gh + 1, gz + z, kMatStone);
        put(gx + x, gh + 2, gz + z, rim ? kMatStone : kMatBlood);
      }
    // A run down a two-step ledge: the vertical-trail case, which is where a
    // height-field normal (water's model) would fail outright.
    for (int i = 0; i < 10; i++) {
      put(gx + 10, gh + 2 - i / 3, gz - 6 + i, kMatStone);
      put(gx + 10, gh + 3 - i / 3, gz - 6 + i, kMatBlood);
    }
    // Isolated droplets scattered over open ground: the "in flight / just
    // landed" end, and the case that reads as gelatin cubes when the surface
    // normal is per-voxel rather than from the smooth field.
    for (int i = 0; i < 48; i++) {
      int ox = ((i * 37) % 21) - 10;
      int oz = ((i * 53) % 25) - 12;
      int oy = ((i * 29) % 2);
      put(gx - 22 + ox, gh + 1 + oy, gz + oz, kMatBlood);
    }
    // Only a few ticks of settle. Blood carries a decay rule ("blood dries
    // away", reactions.json) at 8 per-mille, so a long settle leaves nothing
    // but the STAIN in frame — which is a fine shot of the stain layer and a
    // useless one for judging the liquid. 12 ticks is enough for the pool to
    // find its surface and the droplets to land, and ~91% of the blood is
    // still there.
    for (uint32_t t = 121; t <= 132; t++)
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {},
                 t == 121 ? gore : std::vector<CellOp>{}, false, {8, 3, 8},
                 false, false);
    ctx.WaitIdle();
    // Low and close across the basin: the grazing angle where the wet sheen
    // and the Fresnel rim have to carry it, with the droplet field in frame.
    render({(float)(gx - 30), (float)(gh + 9), (float)(gz - 24)}, 0.60f, -0.22f,
           "screenshot_blood.bmp");
    // Looking down into the pool: the low-Fresnel angle, where the body colour
    // and the stain on the surrounding stone carry the frame instead.
    render({(float)gx, (float)(gh + 16), (float)(gz + 14)}, -1.571f, -0.85f,
           "screenshot_blood_down.bmp");
  }

  // ---- static micro-detail: grass, foliage and flowers -------------------
  // Worldgen does not place any of these (deliberately — Wave 1a does not
  // touch worldgen), so the shot has to paint them itself, exactly the way the
  // lava-spatter and blood scenes above do. Without this the entire feature
  // would go unreviewed by --shot.
  //
  // The layout is chosen to exercise the three things that can go wrong:
  //   * a MEADOW of grass_tuft, which is where the "cell must not block the
  //     ray on a miss" rule shows up — get it wrong and this reads as a solid
  //     green slab rather than as blades against ground.
  //   * a MIXED patch of flowers among the grass, which is where the per-cell
  //     yaw/jitter has to stop the field looking stamped.
  //   * a low CLOSE camera and a HIGH one, so the LOD handoff at
  //     TUNE_MICRO_LOD_DIST is visible in the same pass.
  {
    std::vector<CellOp> flora;
    const int gx = 150, gz = 150;
    // Deterministic placement — no rand(), so the shot is reproducible frame to
    // frame like every other look shot in this function.
    for (int dz = -22; dz <= 22; dz++) {
      for (int dx = -22; dx <= 22; dx++) {
        int wx = gx + dx, wz = gz + dz;
        int gh = World::TerrainHeight(wx, wz, kDefaultSeed);
        IVec3 c{wx, gh + 1, wz};
        if (!world.CellInWindow(c)) continue;
        // A cheap integer hash of the column picks what grows here. Grass is
        // the common case; flowers are sparse, because a meadow where every
        // cell is a poppy reads as gravel.
        uint32_t r = (uint32_t)(wx * 73856093 ^ wz * 19349663);
        r ^= r >> 13; r *= 0x9E3779B9u; r ^= r >> 16;
        uint32_t roll = r % 100u;
        uint32_t mat;
        int height = 1;
        // The close camera stands at (-16, -16): nothing within three cells
        // of it, or the frame is the inside of whatever grew there.
        if (dx >= -19 && dx <= -13 && dz >= -19 && dz <= -13) continue;
        // The near quadrant (dz > 6) is a tall-grass stand so the close shot
        // has blades at eye level; the rest is lawn with flowers in it.
        if (dz > 6 && roll < 80u) {
          mat = kMatTallGrass;
          height = 4 + (int)((r >> 8u) % 5u);
        } else if (roll < 50u) { mat = kMatGrassTuft; height = 1 + (int)((r >> 9u) & 1u); }
        else if (roll < 55u) { mat = kMatFlowerPoppy; height = 3; }
        else if (roll < 60u) { mat = kMatFlowerDaisy; height = 2 + (int)((r >> 9u) & 1u); }
        else if (roll < 63u) { mat = kMatFlowerFoxglove; height = 5 + (int)((r >> 9u) % 3u); }
        else if (roll < 67u) { mat = kMatFlowerBluebell; height = 2 + (int)((r >> 9u) & 1u); }
        else if (roll < 71u) { mat = kMatFlowerButtercup; height = 2; }
        else if (roll < 73u) { mat = kMatFoliageBush; }
        else { continue; }  // bare ground between the tufts
        for (int k = 0; k < height; k++) {
          IVec3 ck{wx, gh + 1 + k, wz};
          if (!world.CellInWindow(ck)) continue;
          uint32_t m = mat;
          if (mat == kMatTallGrass && k == height - 1) m = kMatTallGrassHead;
          // Same word rules as a brush paint on a solid: state 0, unstamped.
          flora.push_back({World::SlotCellIndex(ck), PackVoxNew(m, 0u)});
        }
      }
    }
    // TILE PLANTS: a fern bank and two big toadstools on the far side, and a
    // ring of small mushrooms, placed exactly where the renderer will rebuild
    // them (sim/plants.h is the CPU twin of plantTileAt).
    {
      auto paintTile = [&](int tx, int tz, uint32_t salt, int tile, int foot, int minH,
                           int maxH, uint32_t mat) {
        PlantTileCpu pt = PlantTileAtCpu(tx * tile, tz * tile, kDefaultSeed, salt,
                                         tile, foot, minH, maxH, 100u);
        int hc = World::TerrainHeight(pt.cx, pt.cz, kDefaultSeed);
        int half = foot / 2;
        for (int dz = -half; dz <= half; dz++)
          for (int dx = -half; dx <= half; dx++)
            for (int k = 1; k <= pt.h; k++) {
              IVec3 c{pt.cx + dx, hc + k, pt.cz + dz};
              if (!world.CellInWindow(c)) continue;
              flora.push_back({World::SlotCellIndex(c), PackVoxNew(mat, 0u)});
            }
      };
      for (int tz = -4; tz <= -2; tz++)
        for (int tx = 0; tx <= 3; tx++)
          paintTile((gx / kPlantFernTile) + tx, (gz / kPlantFernTile) + tz,
                    kPlantFernSalt, kPlantFernTile, kPlantFernFoot, kPlantFernMinH,
                    kPlantFernMaxH, kMatFern);
      paintTile((gx / kPlantShroomTile) - 1, (gz / kPlantShroomTile) - 1,
                kPlantShroomSalt, kPlantShroomTile, kPlantShroomFoot,
                kPlantShroomMinH, kPlantShroomMaxH, kMatMushroomLarge);
      paintTile((gx / kPlantShroomTile) + 1, (gz / kPlantShroomTile) - 2,
                kPlantShroomSalt, kPlantShroomTile, kPlantShroomFoot,
                kPlantShroomMinH, kPlantShroomMaxH, kMatMushroomLarge);
      for (int i = 0; i < 12; i++) {
        int wx = gx - 12 + (i * 7) % 11, wz = gz - 14 + (i * 5) % 7;
        int gh = World::TerrainHeight(wx, wz, kDefaultSeed);
        IVec3 c{wx, gh + 1, wz};
        if (!world.CellInWindow(c)) continue;
        flora.push_back({World::SlotCellIndex(c),
                         PackVoxNew((i & 1) ? kMatMushroomCluster : kMatToadstoolPale, 0u)});
      }
    }
    for (uint32_t t = 133; t <= 136; t++)
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {},
                 t == 133 ? flora : std::vector<CellOp>{}, false, {8, 3, 8},
                 false, false);
    ctx.WaitIdle();
    int mh = World::TerrainHeight(gx, gz, kDefaultSeed);
    // Eye-level and close: individual blades and petals have to resolve here,
    // and a micro cell that wrongly blocked its ray shows up immediately as a
    // wall of green cubes.
    // Above the tips looking down the slope: close enough that individual
    // blades and petals resolve, but OUT of the grass — a camera at tuft height
    // sits inside a blade and the frame is one green wall.
    // On the CAMERA's own column: the datum moved with the terrain overhaul
    // and mh + 7 at (-16, -16) was a lens buried in the hillside.
    const int ch = World::TerrainHeight(gx - 16, gz - 16, kDefaultSeed);
    render({(float)(gx - 16), (float)(ch + 8), (float)(gz - 16)}, 0.785f, -0.28f,
           "screenshot_micro.bmp");
    // High and back: crosses TUNE_MICRO_LOD_DIST inside one frame, so the
    // near/far handoff is visible as a single image rather than two shots.
    render({(float)(gx - 60), (float)(mh + 30), (float)(gz - 60)}, 0.785f, -0.30f,
           "screenshot_micro_far.bmp");
  }

  // ---- a GENERATED pond, with its vegetation ----
  // LAST on purpose: this block MOVES THE RESIDENCY WINDOW and regenerates the
  // world, so anything shot after it would be looking at a different region.
  //
  // The authored lake shot earlier is a bare stone tub — it exercises the water
  // and submerged shading but has no plant life, because pond flora is placed
  // by pondAt() and the authored pools are explicitly excluded from it. This is
  // the only shot that shows lilypads, reeds and kelp, and the one that would
  // catch worldgen putting them somewhere absurd (floating, or on dry land).
  //
  // WHY THE WINDOW HAS TO MOVE: every pond site is deliberately outside the
  // spawn keep-out box (-44..264), so no generated pond can fall inside the
  // residency window while that window sits at the origin. Outside the window
  // a lake shades through the FAR-FIELD cascade, which paints liquids as flat
  // colour with no Fresnel, no refraction and no visible bed — from the origin
  // window a pond is a flat blue disc that tells you nothing. So re-centre on
  // it and regenerate. Chunk units, min corner, 16 chunks to a 256-voxel window.
  {
    // Pond at (258,-235) radius 109 for kDefaultSeed, from pondAt's tile hash.
    // A pond is now up to radius 127, so the widest ones no longer fit inside
    // the 256-voxel window with any margin — centring the window on the pond
    // puts its far shore right at the window edge. That is fine for a look
    // shot (the near half is what these frames are about) but it is why the
    // camera sits close to the middle rather than back on the bank.
    const int kPx = 258, kPz = -235;
    world.SetWindowOrigin({kPx / (int)kChunk - 8, 0, kPz / (int)kChunk - 8});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    for (uint32_t t = 1; t <= 60; t++)
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, {}, false, {8, 3, 8},
                 false, false);
    ctx.WaitIdle();

    // Anchor to terrain OUTSIDE the bowl (the rim), not the centre —
    // TerrainHeight in the middle of a pond reports the carved floor, 2.6 m
    // down, and a camera placed relative to that sits underground.
    // Offsets scale with the pond: these were sized for the old radius-52
    // bowl and a doubled pond puts the far shore out of frame at those
    // distances. kOff sits just outside the rim of this pond.
    const int kR = 109, kOff = kR + 22;
    int rim = World::TerrainHeight(kPx + kOff, kPz + kOff, kDefaultSeed);
    // From off the +x/+z side looking back at the centre: atan2(-1,-1) =
    // -135 degrees. The pad-strewn surface, the reed fringe and the shore.
    render({(float)(kPx + kOff), (float)(rim + 26), (float)(kPz + kOff)},
           -2.356f, -0.24f, "screenshot_pond.bmp");
    // Straight down over the centre: the framing-independent check that the
    // bowl, the lilypad scatter and the plant density are what worldgen
    // intended, without depending on getting an eye-level camera right.
    render({(float)kPx, (float)(rim + 118), (float)kPz}, 0.785f, -1.50f,
           "screenshot_pond_top.bmp");
    // INSIDE the pond, under the waterline: kelp silhouettes, the caustic web
    // on the bed and the light shafts all have to read at once. The surface
    // sits 2 under the lowest rim sample, the centre TUNE_POND_DEPTH below it.
    render({(float)(kPx - 30), (float)(rim - 10), (float)(kPz - 30)}, 0.785f,
           0.04f, "screenshot_pond_sub.bmp");
  }

  // ---- THE LOD SEAM AT EYE HEIGHT (2026-09-28, LOD-seam overhaul P0) ----
  //
  // Where the residency window's box face hands the fine march off to the far
  // cascade, seen from where a player stands: 1.7 m over the ground, window
  // AND far field centred on the eye exactly as Stream / FarField centre them
  // in play (sandvox::CentreWindowOnEye), so the face is 25.6 m ahead along an
  // axis and ~36 m toward a box corner — not wherever the harness origin put
  // it. Every other frame here either looks down from well above the ground
  // or stands in a window centred somewhere else, so none of them shows the
  // seam at its in-game distance. The poses are shared with the `seam` /
  // `seamveg` --render-budget cameras, so the frame judged is the frame timed.
  //   _x     level down +x, the flattest pad column (bare sand): face 25.6 m ahead
  //   _diag  level toward the (-x,+z) box corner: face vs corner asymmetry
  //   _low   +x pitched down 0.08: the ground band 10-60 m fills the frame
  //   _veg   level down +x at the nearest meadow to the spawn: plants across
  //          the seam (the harness pad refuses all cover)
  //   _x_mask, _diag_mask  _x / _diag with the far march off: beyond the
  //          face is fog, so the pair locates the handoff line exactly
  // LAST in RunShots and skipped whole unless one of them is wanted: each site
  // is a worldgen + a full far refill, and nothing after this needs restoring.
  if (ShotWanted("screenshot_seam_x") || ShotWanted("screenshot_seam_diag") ||
      ShotWanted("screenshot_seam_low") || ShotWanted("screenshot_seam_veg") ||
      ShotWanted("screenshot_seam_x_mask") ||
      ShotWanted("screenshot_seam_diag_mask")) {
    const sandvox::SeamPose plain = sandvox::SeamPlainPose();
    std::printf("seam site: (%d,%d) ground y=%d eye y=%.1f -- %s\n", plain.x,
                plain.z, plain.ground, plain.ey, plain.what);
    sandvox::CentreWindowOnEye(ctx, world, sim, plain.ex, plain.ey, plain.ez,
                               120);
    const Vec3 pe{plain.ex, plain.ey, plain.ez};
    render(pe, 0.0f, 0.0f, "screenshot_seam_x.bmp");
    render(pe, 2.356f, 0.0f, "screenshot_seam_diag.bmp");
    render(pe, 0.0f, -0.08f, "screenshot_seam_low.bmp");
    // Where the face IS, as arithmetic: the window spans chunks
    // [origin, origin + kNChunk), so the +x face is at (origin.x + kNChunk)*16.
    {
      const IVec3 o = world.WindowOrigin();
      const float fx = (float)((o.x + (int)kNChunk) * (int)kChunk) - plain.ex;
      const float fzp = (float)((o.z + (int)kNChunk) * (int)kChunk) - plain.ez;
      const float fxm = plain.ex - (float)(o.x * (int)kChunk);
      std::printf("seam geometry: window origin chunk (%d,%d,%d); +x face %.1f m "
                  "ahead; (-x,+z) corner %.1f m away (faces %.1f / %.1f m)\n",
                  o.x, o.y, o.z, fx * 0.1f,
                  std::sqrt(fxm * fxm + fzp * fzp) * 0.1f, fxm * 0.1f,
                  fzp * 0.1f);
    }
    // _x_mask / _diag_mask: the SAME two frames with the far march off
    // (render.farSteps 0, --render-budget's `nofar` arm), so every ray that
    // leaves the window is fog/sky. Diffed against _x / _diag it is a pixel-
    // exact map of where the handoff line falls — the seam, located without
    // having to spot it. Costs a shader reload each way, so only when asked.
    if (ShotWanted("screenshot_seam_x_mask") ||
        ShotWanted("screenshot_seam_diag_mask")) {
      const Tuning saved = CurrentTuning();
      Tuning tm = saved;
      tm.render.farSteps = 0;
      SetCurrentTuning(tm);
      sim.ReloadShaders(ctx.device);
      render(pe, 0.0f, 0.0f, "screenshot_seam_x_mask.bmp");
      render(pe, 2.356f, 0.0f, "screenshot_seam_diag_mask.bmp");
      SetCurrentTuning(saved);
      sim.ReloadShaders(ctx.device);
    }
    if (ShotWanted("screenshot_seam_veg")) {
      const sandvox::SeamPose veg = sandvox::SeamVegPose();
      std::printf("seam veg site: (%d,%d) ground y=%d eye y=%.1f -- %s\n",
                  veg.x, veg.z, veg.ground, veg.ey, veg.what);
      sandvox::CentreWindowOnEye(ctx, world, sim, veg.ex, veg.ey, veg.ez, 120);
      render({veg.ex, veg.ey, veg.ez}, 0.0f, 0.0f, "screenshot_seam_veg.bmp");
    }
  }
  // A hazard report with no message pop is a hazard report that goes nowhere:
  // the debug messenger collects continuously, but only the F5-reload scope
  // pops. Print (and count) whatever this run gathered.
  return ctx.ReportVkValidation("--shot") > 0 ? 1 : 0;
}

// --shot-waterfall: the fixture the render-only waterfall mist (13.3.1) is
// judged on.
//
// THERE IS NO WATERFALL IN THE SHIPPED WORLD, and that is why this exists.
// Every authored pool is a flat stone basin and every generated tarn is a bowl,
// so "liquid with air under it" - the one cue the mist detector fires on - is
// never true for more than a tick anywhere in --shot's forty frames. A look
// feature with no frame that contains its subject is a feature nobody can
// review, so this builds the subject: a stone terrace with a spout cut in its
// rim, a plunge basin at its foot, and a water source in the trough behind the
// lip, poured for long enough that a column is falling AND the pool below it
// has formed.
//
// EVERYTHING here enters the world through the MutationQueue as CellOps
// (CLAUDE.md rule 3) - the same op stream a brush or a spell uses. Nothing
// writes the voxel buffer directly and nothing here runs under --selftest, so
// the fixture cannot move a gate or the world hash.
int RunWaterfallShot(GpuContext& ctx, World& world, Simulation& sim) {
  // ---- where. Inside the origin residency window on purpose: a liquid
  // outside it shades through the far-field cascade as flat colour, with no
  // surface, no fullness gradient and therefore no falling column to detect
  // (the same trap the oil and pond frames in --shot document).
  const int cx = 120, cz = 200;   // the fall's XZ: lip at cx-1, water at cx
  const int kBack = 8;            // terrace depth in -X
  // CLEARED AIR IN +X, AND IT IS A FRAMING NUMBER, NOT A SCENERY ONE. The
  // first cut of this scene cleared 36 voxels, which put the only legal camera
  // 3 m from a 4 m cliff: the frame was one flat white wall, the fall was a
  // 20 cm ribbon against it, and the shot could not be read at all. The eye
  // has to get far enough back that the terrace has SKY behind its top.
  const int kFront = 60;
  // Half the box width in Z, and it has to clear the CAMERA, not just the
  // subject: at 14 the only legal eye sat two voxels inside the +Z edge and
  // a live trunk just outside the excavation stood in the middle of the
  // frame.
  const int kHalfZ = 18;
  const int kDrop = 40;           // voxels of fall (4.0 m at kVoxelMeters=0.10)
  const int kRim = 4;             // trough wall height on the terrace top
  const int kPit = 16;            // plunge basin depth below the plaza
  const int x0 = cx - kBack, x1 = cx + kFront - 1;
  const int z0 = cz - kHalfZ, z1 = cz + kHalfZ - 1;

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  FarField far;
  far.Init(&world);
  far.FullRefill(IVec3{8, 3, 8});
  uint32_t nfar;
  while ((nfar = far.PrepareTick(ctx.queue)) > 0) {
    TickParams tp{0, kDefaultSeed, 0, 0};
    tp.farCount = nfar;
    ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeFarFill(enc, nfar);
    ctx.queue.Submit(enc.Finish());
  }

  // The plaza sits one voxel above the HIGHEST terrain in the footprint, so
  // nothing native can poke through the floor of the frame. Derived, never
  // written down: spawnPlainY has moved once already and every literal height
  // in this file that predated the move was rendering from inside rock.
  int floorY = INT32_MIN;
  for (int z = z0; z <= z1; z++)
    for (int x = x0; x <= x1; x++)
      floorY = std::max(floorY, World::TerrainHeight(x, z, kDefaultSeed));
  floorY += 1;
  const int cliffTop = floorY + kDrop;
  const int yLo = floorY - kPit - 2, yHi = cliffTop + kRim + 4;
  // Plunge basin: open at the cliff face so the column drops straight into it.
  const int bx0 = cx, bx1 = cx + 22, bz0 = cz - 11, bz1 = cz + 11;
  const int pitFloor = floorY - kPit;
  // The trough on the terrace top, open only at the lip.
  const int tz0 = cz - 4, tz1 = cz + 3, tx0 = x0 + 2;

  // ONE op per cell, decided once. Emitting "air here, stone there" as two
  // passes would put two ops on the same cell in the same tick, and sim_mutate
  // applies a tick's ops in parallel - two writers to one cell is exactly the
  // scheduling-dependent outcome rule 1 forbids.
  auto desired = [&](int x, int y, int z) -> uint32_t {
    if (y <= floorY) {
      if (x >= bx0 && x <= bx1 && z >= bz0 && z <= bz1) {
        if (y > pitFloor) return kMatAir;    // the basin
        // A DRAIN, so the scene is BOUNDED (CLAUDE.md rule 2). A source with
        // no sink fills the basin, tops the plaza and then runs out over live
        // terrain, which is an unbounded wet region and hundreds of chunks
        // that never sleep. Void is the engine's authored liquid sink; a small
        // patch of it in the basin floor lets the pool find a level instead.
        if (y == pitFloor && x >= bx1 - 4 && x <= bx1 - 2 &&
            z >= cz - 1 && z <= cz + 1)
          return kMatVoid;
      }
      return kMatStone;                      // solid tub, whatever worldgen left
    }
    if (x < cx) {                            // the terrace / cliff column
      if (y <= cliffTop) return kMatStone;
      if (y <= cliffTop + kRim)
        return (z >= tz0 && z <= tz1 && x >= tx0) ? kMatAir : kMatStone;
    }
    return kMatAir;                          // open air in front and above
  };

  std::vector<CellOp> build;
  build.reserve((size_t)(x1 - x0 + 1) * (size_t)(z1 - z0 + 1) *
                (size_t)(yHi - yLo + 1));
  for (int z = z0; z <= z1; z++)
    for (int x = x0; x <= x1; x++)
      for (int y = yLo; y <= yHi; y++) {
        const uint32_t m = desired(x, y, z);
        build.push_back({World::SlotCellIndex({x, y, z}),
                         m == kMatAir ? 0u : PackVoxNew(m, 0u)});
      }

  // The source: full water cells rewritten at the back of the trough every
  // tick. A CellOp overwrite is a cheaper and more predictable source than a
  // source_water block (whose emit is a per-tick reaction chance), and it is
  // the same op stream, so the fixture stays inside rule 3 either way.
  // AT THE LIP, NOT AT THE BACK OF THE TROUGH, and that is the whole
  // difference between a waterfall and a damp stain. Poured at the back, the
  // stream that actually leaves the spout is whatever the CA's lateral
  // equalisation delivers over the lip - measured here, about one cell a tick,
  // a 20 cm ribbon that is invisible against lit stone. Writing the source
  // cells straddling the lip instead makes the SHEET the authored quantity: one
  // fresh row a tick, which is also the rate the column falls, so it stays
  // dense all the way down.
  std::vector<CellOp> pour;
  for (int z = cz - 3; z <= cz + 2; z++)
    for (int x = cx - 3; x <= cx + 1; x++)
      pour.push_back({World::SlotCellIndex({x, cliffTop + 1, z}),
                      (kMatWater & 0xFFFu) | (7u << 12)});   // 8/8 fullness

  // Build first (chunked under the per-tick op cap), then pour long enough for
  // BOTH halves of the subject to exist: a column in flight the whole height of
  // the cliff, and a plunge pool deep enough to shade as water rather than as a
  // wet floor.
  const size_t kOpsPerTick = 60000;
  uint32_t tick = 0;
  const IVec3 pchunk{cx / (int)kChunk, floorY / (int)kChunk, cz / (int)kChunk};
  for (size_t off = 0; off < build.size(); off += kOpsPerTick) {
    const size_t n = std::min(kOpsPerTick, build.size() - off);
    std::vector<CellOp> slice(build.begin() + (ptrdiff_t)off,
                              build.begin() + (ptrdiff_t)(off + n));
    SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, slice, false,
               pchunk, false, false);
  }
  for (int i = 0; i < 260; i++)
    SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, pour, false,
               pchunk, false, false);
  ctx.WaitIdle();

  // ---- CENSUS, not a guess ------------------------------------------------
  // A look fixture whose subject silently failed to build is a frame nobody can
  // interpret: is the water missing, or is the camera pointed at the back of a
  // wall? One readback separates those two forever, and it is the difference
  // between reading the next shot and re-running the harness to find out.
  {
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    uint32_t nWater = 0;
    int wMinY = INT32_MAX, wMaxY = INT32_MIN;
    for (int qz = z0 >> 4; qz <= (z1 >> 4); qz++)
      for (int qy = yLo >> 4; qy <= (yHi >> 4); qy++)
        for (int qx = x0 >> 4; qx <= (x1 >> 4); qx++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({qx, qy, qz}), 1,
                         cbuf.data(), "waterfallCensus");
          for (uint32_t k = 0; k < kChunkVol; k++) {
            if ((cbuf[k] & 0xFFFu) != kMatWater) continue;
            const int wy = (int)((k / kChunk) % kChunk) + qy * (int)kChunk;
            nWater++;
            wMinY = std::min(wMinY, wy);
            wMaxY = std::max(wMaxY, wy);
          }
        }
    std::printf("--shot-waterfall: %u water cells, y %d..%d "
                "(basin floor %d, plaza %d, lip %d)\n",
                nWater, wMinY, wMaxY, pitFloor, floorY, cliffTop);
  }

  const uint32_t W = 1920, H = 1080;
  rhi::Texture offscreen = ctx.device.CreateTexture(
      {W, H, 1}, rhi::TextureFormat::RGBA8Unorm,
      rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
      "offscreen");
  rhi::TextureView view = offscreen.CreateView();
  const Tuning& shotTun = CurrentTuning();
  const uint32_t ticksPerDay = TicksPerDay(shotTun);
  const uint32_t shotTick =
      (uint32_t)((double)g_shotTimeOfDay * (double)ticksPerDay) % ticksPerDay;
  auto render = [&](Vec3 eye, float yaw, float pitch, const char* path) {
    Camera c;
    c.yaw = yaw;
    c.pitch = pitch;
    WriteRenderParams(ctx.queue, world, eye, c, (float)W / H, true, 11.7f,
                      kFarFogDensity, 1080.0f, shotTick);
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeShadowResolve(enc);
    rhi::RenderPass rp =
        sim.BeginRenderPass(enc, view, rhi::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    rp.End();
    ctx.queue.Submit(enc.Finish());
    ctx.WaitIdle();
    rhi::Buffer shot = CreateBuffer(
        ctx.device, (uint64_t)W * H * 4,
        rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "screenshot");
    rhi::CommandEncoder enc2 = ctx.device.CreateCommandEncoder();
    rhi::TexelCopyTexture srcT{};
    srcT.texture = offscreen;
    rhi::TexelCopyBuffer dstB{};
    dstB.buffer = shot;
    dstB.bytesPerRow = W * 4;
    dstB.rowsPerImage = H;
    enc2.CopyTextureToBuffer(srcT, dstB, {W, H, 1});
    ctx.queue.Submit(enc2.Finish());
    std::vector<uint8_t> pixels((size_t)W * H * 4);
    if (rhi::ReadBufferBlocking(ctx.device, shot, 0, pixels.data(),
                                pixels.size()) &&
        WriteBmpFile(path, pixels, W, H))
      std::printf("wrote %s\n", path);
  };

  // SUN BEHIND THE CAMERA, asked for rather than written down. Both cameras
  // have to approach from +X (the cliff walls off -X), so the only freedom is
  // which side in Z, and the sun's own azimuth picks it: standing so the light
  // comes over your shoulder is what puts the mist between the sun and the eye
  // without turning the column into a silhouette.
  const SkyState ss = SkyForTick(shotTun, shotTick);
  const float zs = ss.sunDir[2] >= 0.0f ? 1.0f : -1.0f;
  auto aim = [&](Vec3 eye, Vec3 at) {
    const float dx = at.x - eye.x, dy = at.y - eye.y, dz = at.z - eye.z;
    const float hd = std::sqrt(dx * dx + dz * dz);
    // Camera::Forward() is (cos yaw, sin pitch, sin yaw) - atan2(z, x).
    return std::pair<float, float>{std::atan2(dz, dx), std::atan2(dy, hd)};
  };
  {
    // _waterfall_over: the whole fixture from outside and above. Not a look
    // frame - it is the one that says the TERRACE, the SPOUT and the BASIN are
    // where the code thinks they are, which is the question every unreadable
    // close-up leaves open.
    const Vec3 eye{(float)(cx + 52), (float)(floorY + 62),
                   (float)cz + zs * 46.0f};
    const Vec3 at{(float)(cx + 2), (float)(floorY + 14), (float)cz};
    const auto ya = aim(eye, at);
    render(eye, ya.first, ya.second, "screenshot_waterfall_over.bmp");
  }
  {
    // _waterfall: the whole column, from the side and slightly above, with the
    // cliff behind it and the plunge pool in the lower third of the frame.
    const Vec3 eye{(float)(cx + 52), (float)(floorY + 16),
                   (float)cz + zs * 9.0f};
    const Vec3 at{(float)(cx + 1), (float)(floorY + 22), (float)cz};
    const auto ya = aim(eye, at);
    render(eye, ya.first, ya.second, "screenshot_waterfall.bmp");
  }
  {
    // _waterfall_base: the impact point, close and low. This is the frame the
    // SPRAY term is judged on - the mist above is barely in it.
    const Vec3 eye{(float)(cx + 24), (float)(floorY + 6),
                   (float)cz + zs * 10.0f};
    const Vec3 at{(float)(cx + 3), (float)(floorY - 6), (float)cz + zs * 1.0f};
    const auto ya = aim(eye, at);
    render(eye, ya.first, ya.second, "screenshot_waterfall_base.bmp");
  }
  std::printf("--shot-waterfall: floorY=%d cliffTop=%d, %zu build ops\n",
              floorY, cliffTop, build.size());
  return ctx.ReportVkValidation("--shot-waterfall") > 0 ? 1 : 0;
}

// --shot-debris-pond: the owner's own report, photographed
// (docs/PLAN_debris_buoyancy.md). "I explode a tree and its voxels all fall on
// top of the water and form a structure on top of the water."
//
// The `debris-float` gate measures this numerically in a sealed basin, which is
// the right instrument for "did the iron reach the bed" and the wrong one for
// the thing that was actually reported: a LOOK. So this is the real path end to
// end — a generated pond, a wooden mass over it, and the blast's own ejecta
// raining down — with the frame taken twice: once while the chips are still in
// the air, and once after they have settled.
//
// The census printed beside the frames is what makes it more than a picture: it
// counts wood ABOVE the waterline (the reported bug: a raft in the air) against
// wood AT it, on the same voxels the camera is looking at.
int RunDebrisPondShot(GpuContext& ctx, World& world, Simulation& sim,
                      const std::vector<MaterialDef>& mats) {
  // THE LAKE IS ASKED FOR BY NAME, not written down. `--shot`'s pond block and
  // `--shot-fluid-pond` both carry the literal (258,-235) of a tile-hashed
  // pond, and that site has already moved once — the first cut of this fixture
  // probed there and found no water at all. The map knows where its water is:
  // `World::WaterSiteDisc` is the authored lake (map.json's `home_lake`, the
  // same disc the shader fills), and a map with no water site says so instead
  // of photographing dry ground.
  if (World::WaterSiteCount() <= 0) {
    std::printf("--shot-debris-pond: the loaded map authors no water site\n");
    return 1;
  }
  const World::PondDisc lake = World::WaterSiteDisc(0, kDefaultSeed);
  const int kPx = lake.cx, kPz = lake.cz;
  // WHY THE WINDOW HAS TO MOVE (the reason --shot's pond block gives): outside
  // the residency window a lake shades through the far-field cascade as flat
  // colour with no surface and no bed, so debris floating on it would be
  // invisible by construction. Floor division, not truncation — the authored
  // lake can sit at a negative coordinate, and -235/16 is -14 in C++ where the
  // chunk that holds it is -15.
  const IVec3 here{(int)std::floor(kPx / (float)kChunk), 3,
                   (int)std::floor(kPz / (float)kChunk)};
  world.SetWindowOrigin({here.x - 8, 0, here.z - 8});
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  FarField far;
  far.Init(&world);
  far.FullRefill(here);
  uint32_t nfar;
  while ((nfar = far.PrepareTick(ctx.queue)) > 0) {
    TickParams tp{0, kDefaultSeed, 0, 0};
    tp.farCount = nfar;
    ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeFarFill(enc, nfar);
    ctx.queue.Submit(enc.Finish());
  }

  uint32_t waterId = 0, woodId = 0;
  for (size_t i = 0; i < mats.size(); i++) {
    if (mats[i].name == "water") waterId = (uint32_t)i;
    else if (mats[i].name == "wood") woodId = (uint32_t)i;
  }

  uint32_t t = 0;
  auto tick = [&](const std::vector<ExplosionOp>& exps,
                  const std::vector<CellOp>& cells) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, exps, cells, false,
               here, false, true);
    ctx.WaitIdle();
    ctx.ProcessEvents();
  };
  for (int i = 0; i < 30; i++) tick({}, {});

  // WHERE THE WATER ACTUALLY IS, read off the grid rather than derived from
  // TerrainHeight — which reports the carved floor in the middle of a bowl, and
  // every literal height in this file that predated a worldgen change was
  // rendering from inside rock.
  int waterY = INT32_MIN;
  {
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    const IVec3 wo = world.WindowOrigin();
    for (int cy = wo.y; cy < wo.y + (int)kNChunk; cy++) {
      ReadVoxelsSync(ctx, world,
                     World::SlotChunkIndex({here.x, cy, here.z}), 1,
                     cbuf.data(), "debrisPondProbe");
      for (uint32_t j = 0; j < kChunkVol; j++)
        if ((cbuf[j] & 0xFFFu) == waterId)
          waterY = std::max(waterY, cy * (int)kChunk + (int)((j / kChunk) % kChunk));
    }
  }
  if (waterY == INT32_MIN) {
    std::printf("--shot-debris-pond: the map's water site is at (%d,%d) r%d "
                "surf %d, but its centre chunk column holds no water voxel\n",
                kPx, kPz, lake.r, lake.surf);
    return 1;
  }

  // The subject: a wooden mass hanging over open water, blown apart in place.
  // Not a real tree — a tree is rooted on land and the interesting half of the
  // report is what the CHIPS do — but the same material, the same blast and the
  // same ejecta path the owner was looking at.
  // LOW, and it is a FRAMING number. At +26 the trunk was above the top of
  // every frame a camera looking across the water could take, the blast was
  // out of shot, and the chips arrived from nowhere. A mass 12 voxels up
  // scatters into a tight patch the eye can hold in one view.
  const int trunkY = waterY + 12;
  std::vector<CellOp> build;
  for (int y = 0; y < 14; y++)
    for (int z = -3; z <= 3; z++)
      for (int x = -3; x <= 3; x++)
        build.push_back({World::SlotCellIndex({kPx + x, trunkY + y, kPz + z}),
                         PackVoxNew(woodId, 0u)});
  tick({}, build);
  for (int i = 0; i < 4; i++) tick({}, {});

  const uint32_t W = 1920, H = 1080;
  rhi::Texture offscreen = ctx.device.CreateTexture(
      {W, H, 1}, rhi::TextureFormat::RGBA8Unorm,
      rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
      "offscreen");
  rhi::TextureView view = offscreen.CreateView();
  const Tuning& shotTun = CurrentTuning();
  const uint32_t ticksPerDay = TicksPerDay(shotTun);
  const uint32_t shotTick =
      (uint32_t)((double)g_shotTimeOfDay * (double)ticksPerDay) % ticksPerDay;
  auto render = [&](Vec3 eye, Vec3 at, const char* path) {
    const float dx = at.x - eye.x, dy = at.y - eye.y, dz = at.z - eye.z;
    const float hd = std::sqrt(dx * dx + dz * dz);
    Camera c;
    c.yaw = std::atan2(dz, dx);
    c.pitch = std::atan2(dy, hd);
    WriteRenderParams(ctx.queue, world, eye, c, (float)W / H, true, 11.7f,
                      kFarFogDensity, 1080.0f, shotTick);
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeShadowResolve(enc);
    rhi::RenderPass rp =
        sim.BeginRenderPass(enc, view, rhi::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    rp.End();
    ctx.queue.Submit(enc.Finish());
    ctx.WaitIdle();
    rhi::Buffer shotBuf = CreateBuffer(
        ctx.device, (uint64_t)W * H * 4,
        rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "screenshot");
    rhi::CommandEncoder enc2 = ctx.device.CreateCommandEncoder();
    rhi::TexelCopyTexture srcT{};
    srcT.texture = offscreen;
    rhi::TexelCopyBuffer dstB{};
    dstB.buffer = shotBuf;
    dstB.bytesPerRow = W * 4;
    dstB.rowsPerImage = H;
    enc2.CopyTextureToBuffer(srcT, dstB, {W, H, 1});
    ctx.queue.Submit(enc2.Finish());
    std::vector<uint8_t> pixels((size_t)W * H * 4);
    if (rhi::ReadBufferBlocking(ctx.device, shotBuf, 0, pixels.data(),
                                pixels.size()) &&
        WriteBmpFile(path, pixels, W, H))
      std::printf("wrote %s\n", path);
  };
  // CLOSE. The authored lake is 100+ voxels across and the subject is a patch
  // of chips a few voxels wide; from the bank it is a speck among the lilypads
  // worldgen scatters over the same surface.
  const Vec3 eye{(float)(kPx + 20), (float)(waterY + 7), (float)(kPz + 20)};
  const Vec3 at{(float)kPx, (float)(waterY + 2), (float)kPz};

  // The blast, then the frame WHILE IT IS IN THE AIR — the one that shows the
  // chips on their way down rather than where they ended up.
  tick({{kPx, trunkY + 6, kPz, 9, 900, 0, 0, 0}}, {});
  for (int i = 0; i < 6; i++) tick({}, {});
  render(eye, at, "screenshot_debris_pond_blast.bmp");

  // ...and then after it has settled. 400 ticks is well past the point where a
  // floater's bob has damped out (the gate measures ~40) and past
  // sim.partFloatPatience, so anything still in flight here is a bug and shows
  // up in the live count below.
  for (int i = 0; i < 400; i++) tick({}, {});
  render(eye, at, "screenshot_debris_pond.bmp");
  // Straight down over the same patch: the framing-independent frame. A raft
  // spread flat over the water and a tower standing on it look identical from
  // the bank and nothing alike from above, which is the whole reported defect.
  render({(float)kPx, (float)(waterY + 34), (float)(kPz + 1)},
         {(float)kPx, (float)waterY, (float)kPz},
         "screenshot_debris_pond_top.bmp");

  // The census, over the column of chunks the blast could have reached: wood
  // sitting ABOVE the waterline is the reported bug, wood AT it is the fix, and
  // wood below it is a chip that sank (wood should not, so this is the third
  // number rather than a lumped "not above").
  int above = 0, atLine = 0, below = 0, lo = 1 << 30, hi = -(1 << 30);
  {
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    const IVec3 wo = world.WindowOrigin();
    for (int cy = wo.y; cy < wo.y + (int)kNChunk; cy++)
      for (int cz = here.z - 3; cz <= here.z + 3; cz++)
        for (int cx = here.x - 3; cx <= here.x + 3; cx++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx, cy, cz}), 1,
                         cbuf.data(), "debrisPondCensus");
          for (uint32_t j = 0; j < kChunkVol; j++) {
            if ((cbuf[j] & 0xFFFu) != woodId) continue;
            const int y = cy * (int)kChunk + (int)((j / kChunk) % kChunk);
            lo = std::min(lo, y);
            hi = std::max(hi, y);
            if (y > waterY + 1) above++;
            else if (y >= waterY) atLine++;
            else below++;
          }
        }
  }
  uint32_t counts[2] = {};
  ReadCountsSync(ctx, world, counts);
  std::printf("--shot-debris-pond: waterline y%d, trunk y%d | wood %d above / "
              "%d at the waterline / %d under, y%d..%d | %u particles still in "
              "flight\n",
              waterY + 1, trunkY, above, atLine, below, lo, hi,
              std::min(counts[sim.Page()], kParticleCap));
  return ctx.ReportVkValidation("--shot-debris-pond") > 0 ? 1 : 0;
}

// --shot-fluid: the MPM water counterpart of --shot. Worldgen, pour a pool of
// MLS-MPM fluid onto open terrain with the same spawn-op shape the game's mpm
// tool emits, let it slosh, then keep a narrow stream falling and write
// screenshots — the pool at a grazing angle (Fresnel/reflection/glint), from
// above (refraction/absorption/bed), and mid-splash (foam, crown, droplets).
// Exists because the water look can otherwise only be judged by hand-pouring
// in a live session.
// `pond` switches this to the SEAM scene (--shot-fluid-pond): the same pour,
// but aimed into a GENERATED pond so the MPM isosurface has to live on top of
// a deep body of SETTLED CA water. That interaction is the one --shot-fluid
// cannot show — it pours onto dry terrain, where every water pixel is MPM and
// the seam never has to agree with anything — and it is where the seam's
// render defects live (flat chunk-sized colour patches, thickness that stops
// at the fluid AABB, ownership flipping tick to tick).
int RunFluidShot(GpuContext& ctx, World& world, Simulation& sim,
                 const std::vector<MaterialDef>& mats, bool pond) {
  // Generated pond centre + radius for kDefaultSeed (pondAt's tile hash) — the
  // same site --shot's pond block uses, for the same reason: no generated pond
  // falls inside the origin window, so the window has to move onto it or the
  // lake shades through the far-field cascade as a flat blue disc.
  const int kPx = 258, kPz = -235, kR = 109;
  if (pond) {
    // THE FLUID AABB ONLY EXISTS IF THE SNAPSHOT DOES. FluidRenderBounds builds
    // R.fluidLo/fluidHi from the snapshot's active block list and hands back the
    // WHOLE WINDOW when no snapshot has landed (support.h: headless harnesses
    // submit and pump in lockstep, so they never land one). A whole-window box
    // clips nothing — which would hide the exact defect this scene exists to
    // photograph. Drain it so the shot sees the box the game sees.
    SetHarnessSnapshotDrain(true);
    world.SetWindowOrigin({kPx / (int)kChunk - 8, 0, kPz / (int)kChunk - 8});
  }
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // The poured liquid, resolved BY NAME at load like all content (CLAUDE.md
  // conventions): water, or SANDVOX_SHOT_FLUID_MAT=<name> (e.g. acid) for the
  // same scene in another liquid. The particle's material is its whole
  // identity — colour, splash droplets, density — so this one id is all the
  // scene needs. Missing name = water; missing water = nothing poured.
  uint32_t pourMat = 0;
  {
    const char* want = std::getenv("SANDVOX_SHOT_FLUID_MAT");
    const std::string name = (want && *want) ? want : "water";
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == name) { pourMat = (uint32_t)i; break; }
    if (pourMat == 0)
      for (size_t i = 0; i < mats.size(); i++)
        if (mats[i].name == "water") { pourMat = (uint32_t)i; break; }
    std::printf("--shot-fluid: pouring '%s' (id %u)\n",
                pourMat < mats.size() ? mats[pourMat].name.c_str() : "?",
                pourMat);
  }

  // Pour target. On dry terrain that is the ground under the pour; over the
  // pond it is the RIM height, because TerrainHeight in the middle of a bowl
  // reports the carved floor and everything derived from it (pour height,
  // camera) would end up underground.
  const int cx = pond ? kPx : 108, cz = pond ? kPz : 108;
  const int h = World::TerrainHeight(pond ? kPx + kR + 22 : cx,
                                     pond ? kPz + kR + 22 : cz, kDefaultSeed);
  uint32_t fluidCount = 0;

  // One tick of pour: a sphere of cells above `at`, 8 particles per cell on
  // the half-cell lattice with deterministic jitter — the mpm tool's shape.
  auto pour = [&](uint32_t tick, IVec3 at, int rr,
                  std::vector<FluidSpawnOp>& out) {
    for (int z = -rr; z <= rr; z++)
      for (int y = -rr; y <= rr; y++)
        for (int x = -rr; x <= rr; x++) {
          if (x * x + y * y + z * z > rr * rr) continue;
          if (fluidCount + out.size() + 8 > kFluidCap) return;
          if (out.size() + 8 > kMaxFluidSpawnsPerTick) return;
          for (int s = 0; s < 8; s++) {
            uint32_t hh = (tick * 9781u + (uint32_t)out.size() * 6271u) *
                              747796405u + 2891336453u;
            FluidSpawnOp op{};
            op.px = ((at.x + x) << 16) + ((s & 1) ? 49152 : 16384) +
                    (int32_t)(hh % 8192u) - 4096;
            op.py = ((at.y + y) << 16) + ((s & 2) ? 49152 : 16384) +
                    (int32_t)((hh >> 13) % 8192u) - 4096;
            op.pz = ((at.z + z) << 16) + ((s & 4) ? 49152 : 16384) +
                    (int32_t)((hh >> 19) % 8192u) - 4096;
            op.vx = 0; op.vy = -19661; op.vz = 0;
            op.mat = pourMat;  // the particle's one identity
            out.push_back(op);
          }
        }
  };

  uint32_t tick = 0;
  auto step = [&](int n, int pourR, int pourHeight) {
    for (int i = 0; i < n; i++) {
      tick++;
      std::vector<FluidSpawnOp> spawns;
      if (pourR > 0) pour(tick, {cx, h + pourHeight, cz}, pourR, spawns);
      SubmitTick(ctx, world, sim, tick, kDefaultSeed, {}, {}, {}, false,
                 {cx / (int)kChunk, h / (int)kChunk, cz / (int)kChunk}, false,
                 /*particlesActive=*/true, {}, 0, spawns, fluidCount);
      fluidCount = std::min(fluidCount + (uint32_t)spawns.size(), kFluidCap);
    }
  };

  const uint32_t W = 1920, H = 1080;
  rhi::Texture offscreen = ctx.device.CreateTexture(
      {W, H, 1}, rhi::TextureFormat::RGBA8Unorm,
      rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
      "offscreen");
  rhi::TextureView view = offscreen.CreateView();
  auto render = [&](Vec3 eye, float yaw, float pitch, const char* path) {
    Camera c;
    c.yaw = yaw;
    c.pitch = pitch;
    uint32_t shotTick = (uint32_t)(0.30 * (double)TicksPerDay(CurrentTuning()));
    WriteRenderParams(ctx.queue, world, eye, c, (float)W / H, true, 11.7f,
                      kFarFogDensity, 1080.0f, shotTick, fluidCount);
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeShadowResolve(enc);
    rhi::RenderPass rp =
        sim.BeginRenderPass(enc, view, rhi::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    sim.DrawParticles(rp);  // the splash droplets
    if (CurrentTuning().render.fluidSurface < 0.5f)
      sim.DrawFluid(rp, fluidCount);
    rp.End();
    ctx.queue.Submit(enc.Finish());
    ctx.WaitIdle();
    rhi::Buffer shot = CreateBuffer(
        ctx.device, (uint64_t)W * H * 4,
        rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "screenshot");
    rhi::CommandEncoder enc2 = ctx.device.CreateCommandEncoder();
    rhi::TexelCopyTexture srcT{};
    srcT.texture = offscreen;
    rhi::TexelCopyBuffer dstB{};
    dstB.buffer = shot;
    dstB.bytesPerRow = W * 4;
    dstB.rowsPerImage = H;
    enc2.CopyTextureToBuffer(srcT, dstB, {W, H, 1});
    ctx.queue.Submit(enc2.Finish());
    std::vector<uint8_t> pixels((size_t)W * H * 4);
    if (rhi::ReadBufferBlocking(ctx.device, shot, 0, pixels.data(),
                                pixels.size()) &&
        WriteBmpFile(path, pixels, W, H))
      std::printf("wrote %s\n", path);
  };

  // ---- the SEAM scene: MPM water poured into settled CA water --------------
  // Four frames of the SAME camera, so the only thing that changes between
  // them is how much MPM fluid is in the pond. That is what makes this a
  // diagnostic rather than a gallery: frame 1 is the CA-only reference, and
  // any part of frames 2-4 that does not match it outside the splash is a
  // seam defect, not a water look.
  if (pond) {
    // Let the generated pond's CA water find its surface first.
    step(60, 0, 0);
    // Off the -x/-z shore looking back across the middle, low enough that the
    // pour, the far shore and a long stretch of settled surface are all in
    // frame at a grazing angle — the angle at which a thickness or Fresnel
    // discontinuity is most visible.
    const float ex = (float)(kPx - 46), ez = (float)(kPz - 46);
    render({ex, (float)(h + 7), ez}, 0.785f, -0.12f,
           "screenshot_seam_before.bmp");
    // Straight down over the pour: the framing that shows CHUNK-shaped colour
    // patches for what they are, because chunk boundaries are axis-aligned in
    // this projection.
    render({(float)kPx, (float)(h + 46), (float)(kPz + 8)}, -1.571f, -1.15f,
           "screenshot_seam_before_top.bmp");
    step(34, 3, 22);        // pour into the middle of the pond
    render({ex, (float)(h + 7), ez}, 0.785f, -0.12f, "screenshot_seam_pour.bmp");
    render({(float)kPx, (float)(h + 46), (float)(kPz + 8)}, -1.571f, -1.15f,
           "screenshot_seam_pour_top.bmp");
    // Stop pouring and let the excited water hand itself back to the CA. The
    // reported "breaks until the fluid settles" state is this one.
    step(30, 0, 0);
    render({ex, (float)(h + 7), ez}, 0.785f, -0.12f,
           "screenshot_seam_settle.bmp");
    render({(float)kPx, (float)(h + 46), (float)(kPz + 8)}, -1.571f, -1.15f,
           "screenshot_seam_settle_top.bmp");
    std::printf("--shot-fluid-pond: %u particles, water level ~%d\n", fluidCount,
                h - 2);
    return ctx.ReportVkValidation("--shot-fluid-pond") > 0 ? 1 : 0;
  }

  // Fill a pool (~55k particles), let it slosh down but not to glass...
  step(70, 3, 12);
  step(40, 0, 0);
  std::printf("--shot-fluid: %u particles after pour+settle\n", fluidCount);
  render({(float)(cx - 16), (float)(h + 4), (float)(cz - 16)}, 0.785f, -0.10f,
         "screenshot_fluid.bmp");
  render({(float)cx, (float)(h + 26), (float)(cz + 10)}, -1.571f, -1.10f,
         "screenshot_fluid_top.bmp");
  // ...then a thin stream from higher up, shot mid-impact: crown, foam,
  // droplets in flight.
  step(18, 1, 22);
  render({(float)(cx - 12), (float)(h + 8), (float)(cz - 12)}, 0.785f, -0.28f,
         "screenshot_fluid_splash.bmp");
  render({(float)(cx - 7), (float)(h + 3), (float)cz}, 0.0f, 0.05f,
         "screenshot_fluid_low.bmp");
  return ctx.ReportVkValidation("--shot-fluid") > 0 ? 1 : 0;
}

// Count of chunks whose dirty flag is set (selftest only — blocking readback).

// --shot-mob <def>[:limb|+item,...][@x,z] — the mob counterpart of --shot:
// worldgen, spawn the named def, sever the listed limbs and PUT ON the listed
// items ("+robe"), run real ticks until the locomotion state settles, then
// write close-up screenshots from three angles.
// Exists because mob poses (gait, crawl clips, dismemberment states, and now
// what the wardrobe looks like on a body) can otherwise only be judged in a
// live session — this makes "what does the legless crawl actually look like"
// and "do the sleeves sit on the arms" ten-second questions.
int RunMobShot(GpuContext& ctx, World& world, Simulation& sim, Physics& phys,
               DebrisSystem& debris, MobSystem& mobs, const ItemLibrary& items,
               const std::string& spec, Stream& stream,
               const std::vector<MaterialDef>& mats) {
  std::string defName = spec, limbCsv;
  // optional trailing "@x,z" picks the spawn column (default 137,139) — the
  // default area is forested and a wandering mob ends its shot behind a trunk
  // often enough that re-aiming from the CLI beats rebuilding.
  int spawnX = 137, spawnZ = 139;
  if (size_t at = defName.find('@'); at != std::string::npos) {
    std::sscanf(defName.c_str() + at + 1, "%d,%d", &spawnX, &spawnZ);
    defName = defName.substr(0, at);
  }
  if (size_t c = defName.find(':'); c != std::string::npos) {
    limbCsv = defName.substr(c + 1);
    defName = defName.substr(0, c);
  }
  if (size_t at = limbCsv.find('@'); at != std::string::npos) {
    std::sscanf(limbCsv.c_str() + at + 1, "%d,%d", &spawnX, &spawnZ);
    limbCsv = limbCsv.substr(0, at);
  }
  // FindOrComposeDef, so a generated pool body (`pool/m02`) or a composed
  // def is shot exactly as the game would spawn it.
  const int defIndex = mobs.FindOrComposeDef(defName);
  if (defIndex < 0) {
    std::fprintf(stderr, "--shot-mob: no mob def named \"%s\"\n",
                 defName.c_str());
    return 1;
  }
  const MobDef& def = mobs.Defs()[defIndex];

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  int h = World::TerrainHeight(spawnX + 3, spawnZ + 1, kDefaultSeed);
  uint32_t t = 6000;
  for (int i = 0; i < 60; i++)  // powders settle, as in play
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
               {8, h / 16, 8}, false, false);
  ctx.WaitIdle();

  uint64_t id = mobs.Spawn(defIndex, {spawnX, h + 1, spawnZ});
  if (!id) {
    std::fprintf(stderr, "--shot-mob: spawn failed\n");
    return 1;
  }
  // THE GAME'S OWN TICK (W2-O, test/tickrig.h): a picture of a creature is a
  // picture of what the tick does to it, not of a hand-copied part of it.
  support::TickRig rig(
      support::TickEngine{ctx, world, sim, stream, phys, mobs, debris, mats, items},
      t, IVec3{8, h / 16, 8});
  auto mobTick = [&]() {
    support::RunTicks(rig, 1);
    t = rig.tick;
  };

  for (int i = 0; i < 20; i++) mobTick();  // healthy walk first: live gait pose
  std::vector<uint64_t> droppedBodies;       // "_item" drops, logged at the shot
  std::vector<std::string> pendingDrops;     // "_item=mat:frac", dropped later
  for (size_t start = 0; start < limbCsv.size();) {
    size_t end = limbCsv.find(',', start);
    if (end == std::string::npos) end = limbCsv.size();
    std::string nm = limbCsv.substr(start, end - start);
    start = end + 1;
    // "*name" puts an item IN THE HAND (Mob::EquipItem, held_right) and
    // "!clip" plays a clip on it, so a held pose -- the `pour` clip with a
    // flask -- is a screenshot rather than a live session.
    // "*flask=lava:0.4" also FILLS a held vessel (material, fraction of its
    // capacity), so what the contents look like in the hand is a screenshot
    // too (game/container.h ContainerHeldFillWord).
    // "_flask=lava:0.4" DROPS a filled vessel on the ground in front of the
    // creature, so what the contents look like on a body lying on its side is
    // a screenshot too (phys/fillview.h). Recorded here and dropped after the
    // creature has settled, beside where the camera will look.
    if (!nm.empty() && nm[0] == '_') {
      pendingDrops.push_back(nm.substr(1));
      continue;
    }
    if (!nm.empty() && nm[0] == '*') {
      std::string itemName = nm.substr(1), fillSpec;
      if (size_t eq = itemName.find('='); eq != std::string::npos) {
        fillSpec = itemName.substr(eq + 1);
        itemName.resize(eq);
      }
      const ItemDef* it = items.At(items.Find(itemName));
      Mob* hm = mobs.FindCreature(id);
      if (!it || !hm || !hm->EquipItem(it)) {
        std::fprintf(stderr, "--shot-mob: could not hold \"%s\"\n", itemName.c_str());
        continue;
      }
      std::printf("--shot-mob: holding %s\n", itemName.c_str());
      if (!fillSpec.empty() && it->IsContainer()) {
        ItemStack st = StackOf(items, items.Find(itemName));
        st.contents = ContainerParseFillSpec(fillSpec, it->container.capacity, mats);
        // Through the CONTENTS, as a rising or a peer's announce would:
        // MobSystem::RefreshHeldFills turns them into the view each tick.
        hm->SetHeldContents(st.contents, Hand::Right);
        std::printf("--shot-mob: %s holds %s %d/%d eighths\n",
                    itemName.c_str(), fillSpec.c_str(), (int)st.FillTotal(),
                    it->container.capacity);
      }
      continue;
    }
    if (!nm.empty() && nm[0] == '!') {
      if (!mobs.PlayClip(id, nm.substr(1)))
        std::fprintf(stderr, "--shot-mob: no clip \"%s\"\n", nm.c_str() + 1);
      continue;
    }
    // "+name" DRESSES rather than dismembers. Worn geometry is the one thing
    // about a rig that no headless mode could see: the shells only exist on a
    // body somebody put clothes on, and the only place that happened was the
    // live game's equipment panel. So "does the sleeve sit on the arm" could
    // be answered by a screenshot of a running session and by nothing else,
    // which is how it stayed wrong. The piece goes in the first slot whose
    // authored rule takes its kind — the same choice right-click makes.
    if (!nm.empty() && nm[0] == '+') {
      std::string itemName = nm.substr(1);
      // "+tunic#B4472A" DYES IT (game/dye.h). The commoner wardrobe is painted
      // in greyscale and gets its colour from a runtime word, so a screenshot
      // of an undyed tunic photographs the pattern and says nothing at all
      // about what the garment looks like in play — which is the one question
      // this harness exists to answer without a live session.
      uint32_t wearDye = 0;
      if (size_t hash = itemName.find('#'); hash != std::string::npos) {
        // Authored the way a human writes a colour (#RRGGBB) and packed the
        // way the GPU reads one (r in the low byte). The swap lives in
        // dye.h (DyeParseHex) now that a mob's loot table types colours too.
        wearDye = DyeParseHex(itemName.substr(hash));
        itemName = itemName.substr(0, hash);
      }
      const int ii = items.Find(itemName);
      const ItemDef* it = items.At(ii);
      if (!it) {
        std::fprintf(stderr, "--shot-mob: no item named \"%s\"\n",
                     itemName.c_str());
        return 1;
      }
      // Only WORN kinds. A sword would resolve to the sheath, which holds an
      // item without putting it on a body, and the shot would come back
      // identical with no hint why.
      const int slot =
          ItemKindIsWorn(it->kind) ? EquipSlotFor(it->kind, Equipment{}) : -1;
      if (slot < 0) {
        std::fprintf(stderr, "--shot-mob: nothing wears a \"%s\"\n",
                     ItemKindName(it->kind));
        return 1;
      }
      if (!mobs.WearItem(id, it, slot, wearDye))
        std::fprintf(stderr, "--shot-mob: \"%s\" would not go on\n",
                     itemName.c_str());
      else
        std::printf("--shot-mob: wearing %s in slot %d%s%s\n",
                    itemName.c_str(), slot,
                    wearDye ? ", dyed " : "",
                    wearDye ? DyeName(wearDye).c_str() : "");
      continue;
    }
    int li = -1;
    for (size_t i = 0; i < def.limbs.size(); i++)
      if (def.limbs[i].name == nm) li = (int)i;
    if (li < 0) {
      std::fprintf(stderr, "--shot-mob: def \"%s\" has no limb \"%s\"\n",
                   defName.c_str(), nm.c_str());
      return 1;
    }
    mobs.Sever(id, li);
  }
  // enough for the loco crossfade to finish and the sever spray to land, but
  // short enough that a crawler hasn't dragged itself in among the trees
  for (int i = 0; i < 90; i++) mobTick();
  std::printf("--shot-mob: %s locoState=%d clips=%d\n", spec.c_str(),
              mobs.LocoState(id), mobs.ActiveClips(id));
  {
    std::printf("--shot-mob: live clips:");
    for (const auto& cw : mobs.ClipWeights(id))
      std::printf(" %s=%.2f", cw.first.c_str(), cw.second);
    std::printf("\n");
  }
  // Objective pose numbers alongside the pixels: each live limb's local +Y
  // axis, as degrees above the horizon. Screenshots on sloped ground lie
  // about angles; the quaternion does not. (For the dummy's torso this IS
  // the crawl elevation the states ladder tunes.)
  {
    std::vector<BodyXformGpu> mt;
    mobs.AppendXforms(mt);
    std::printf("--shot-mob: limb +Y elevation above horizon (90 = upright, "
                "0 = flat on the ground):\n");
    size_t slot = 0;
    for (size_t i = 0; i < def.limbs.size() && slot < mt.size(); i++) {
      if (!mobs.LimbBody(id, (int)i)) continue;  // severed: no slot emitted
      const BodyXformGpu& m = mt[slot++];
      Quat q{m.quat[0], m.quat[1], m.quat[2], m.quat[3]};
      Vec3 up = QuatRotate(q, {0, 1, 0});
      Vec3 lup = mobs.LimbLocalUp(id, (int)i);
      Vec3 mup = mobs.LimbModelUp(id, (int)i);
      std::printf("    %-8s world %5.1f  model %5.1f  local %5.1f deg\n",
                  def.limbs[i].name.c_str(),
                  std::asin(std::clamp(up.y, -1.0f, 1.0f)) * 57.29578f,
                  std::asin(std::clamp(mup.y, -1.0f, 1.0f)) * 57.29578f,
                  std::asin(std::clamp(lup.y, -1.0f, 1.0f)) * 57.29578f);
    }
  }

  // FOOT CLEARANCE — the number that says whether the mob is standing on the
  // ground or hovering over it. The elevation table above is all angles, and a
  // rig floating ten voxels up poses exactly as correctly as one on the floor,
  // which is how a whole-body hover stayed invisible in this output.
  //
  // Measured as the lowest occupied voxel of any live limb minus the terrain
  // height under it: ~0 is standing, positive is hovering, negative is sunk.
  {
    float lowest = 0;
    bool any = false;
    for (size_t i = 0; i < def.limbs.size(); i++) {
      if (!mobs.LimbBody(id, (int)i)) continue;
      uint32_t n = mobs.LimbVoxelCount(id, (int)i);
      for (uint32_t v = 0; v < n; v++) {
        Vec3 p = mobs.LimbVoxelPos(id, (int)i, v);
        if (!any || p.y < lowest) { lowest = p.y; any = true; }
      }
    }
    if (any) {
      int gy = World::TerrainHeight(ifloor(mobs.MobOrigin(id).x + def.worldSize.x * 0.5f),
                                    ifloor(mobs.MobOrigin(id).z + def.worldSize.z * 0.5f),
                                    kDefaultSeed);
      std::printf("--shot-mob: foot clearance %.2f voxels (lowest limb voxel "
                  "y=%.2f, terrain y=%d; ~0 = standing)\n",
                  lowest - (float)(gy + 1), lowest, gy);
    }
    // ...and PER LIMB, against the terrain under THAT limb: one whole-body
    // minimum says nothing about a prone body, whose hands can be on the floor
    // while its chest hangs a forearm above it.
    std::printf("--shot-mob: per-limb clearance (lowest voxel - terrain under "
                "it; + = above ground):\n");
    for (size_t i = 0; i < def.limbs.size(); i++) {
      if (!mobs.LimbBody(id, (int)i)) continue;
      const uint32_t n = mobs.LimbVoxelCount(id, (int)i);
      float lo = 0;
      Vec3 at{};
      bool have = false;
      for (uint32_t v = 0; v < n; v++) {
        const Vec3 p = mobs.LimbVoxelPos(id, (int)i, v);
        if (!have || p.y < lo) { lo = p.y; at = p; have = true; }
      }
      if (!have) continue;
      const int tg = World::TerrainHeight(ifloor(at.x), ifloor(at.z), kDefaultSeed);
      std::printf("    %-8s %6.2f\n", def.limbs[i].name.c_str(), lo - (float)(tg + 1));
    }
  }

  // ---- "_item" DROPS: beside the creature as it stands NOW --------------
  // The live-limb centroid is where the camera will look (below); MobOrigin
  // is not, and the harness spawn column is on a slope a flask rolls off.
  if (!pendingDrops.empty()) {
    static WorldItems shotGround;   // the shot is one process; bodies outlive it
    std::vector<BodyXformGpu> mt;
    mobs.AppendXforms(mt);
    Vec3 c = mobs.MobOrigin(id);
    if (!mt.empty()) {
      Vec3 sum{};
      for (const BodyXformGpu& m : mt) sum += Vec3{m.pos[0], m.pos[1], m.pos[2]};
      c = sum * (1.0f / (float)mt.size());
    }
    const Vec3 fwdNow = mobs.MobFacing(id);
    const Vec3 rightNow{fwdNow.z, 0, -fwdNow.x};
    for (size_t k = 0; k < pendingDrops.size(); k++) {
      std::string itemName = pendingDrops[k], fillSpec;
      if (size_t eq = itemName.find('='); eq != std::string::npos) {
        fillSpec = itemName.substr(eq + 1);
        itemName.resize(eq);
      }
      const ItemDef* it = items.At(items.Find(itemName));
      if (!it) {
        std::fprintf(stderr, "--shot-mob: no item \"%s\" to drop\n", itemName.c_str());
        continue;
      }
      ItemStack st = StackOf(items, items.Find(itemName));
      if (!fillSpec.empty() && it->IsContainer()) {
        st.contents = ContainerParseFillSpec(fillSpec, it->container.capacity, mats);
      }
      // A little in front and to the side, just above the ground there.
      const Vec3 xz = c + fwdNow * 5.0f + rightNow * (3.0f + 3.0f * (float)k);
      const int gy = World::TerrainHeight(ifloor(xz.x), ifloor(xz.z), kDefaultSeed);
      const Vec3 at{xz.x, (float)gy + 2.0f, xz.z};
      const uint64_t body = DropItemToWorld(*it, st, at, Vec3{0, 0, 0}, phys,
                                            debris, debris.MicroSet(), shotGround);
      if (body) {
        debris.SetBodyFill(body, ContainerHeldFillOf(*it, st));
        droppedBodies.push_back(body);
      }
      std::printf("--shot-mob: dropped %s (%d/%d eighths)\n", itemName.c_str(),
                  (int)st.FillTotal(), it->container.capacity);
    }
    for (int i = 0; i < 45; i++) mobTick();   // let it fall and settle
  }

  // Body upload through the ONE slot walk (game/bodyreg.h). This harness has
  // no avatar, which the registry represents explicitly (nullptr) — all three
  // arrays still agree with each other by construction, which is the property
  // the hand-rolled version here had silently lost.
  // ---- THE BRICKS THEMSELVES, which this harness used to never send --------
  //
  // A limb's micro brick is uploaded once at startup (UploadMicroBodies, after
  // the mob defs load) and then again only from the FRAME LOOP's `if
  // (mbSet.dirty)`. RunMobShot is not the frame loop: it runs its own ticks and
  // renders its own frames, so every brick edit made after boot — a
  // copy-on-write clone from a carve, a burn's per-voxel poke, and now a
  // creature born bitten (MobRotDef) — stayed on the CPU and the GPU went on
  // marching the pristine model, or none at all.
  //
  // The symptom is that the affected limbs DO NOT DRAW. Measured with the
  // undead: eleven of fifteen limbs were carved at spawn and eleven of fifteen
  // were invisible, the four that rendered being exactly the four that happened
  // to roll zero bites — with the rig itself perfectly healthy (every limb had
  // its own micro record, its own body and its full voxel count).
  //
  // Which makes it a real hole in what this harness is FOR: it exists to answer
  // "what does a damaged body actually look like" without a live session, and
  // damage was the one thing it could not photograph.
  if (MicroBodySet* mbs = debris.MicroSet(); mbs != nullptr && mbs->dirty)
    sim.UploadMicroBodies(ctx.queue, *mbs);

  BodyRegistry bodyReg(debris, mobs, nullptr);
  std::vector<BodyXformGpu> xf;
  bodyReg.BuildXforms(xf);
  if (!xf.empty())
    ctx.queue.WriteBuffer(world.bodyXforms, 0, xf.data(),
                          xf.size() * sizeof(BodyXformGpu));
  std::vector<MicroBodyInstGpu> microInsts;
  bodyReg.BuildMicroInsts(microInsts);
  std::vector<BodyVoxInst> inst;
  bodyReg.BuildInstances(inst);
  if (!inst.empty())
    ctx.queue.WriteBuffer(world.bodyInstances, 0, inst.data(),
                          inst.size() * sizeof(BodyVoxInst));

  // Aim at the centroid of the LIVE limbs, not the spawn point — the mob has
  // been walking, and after heavy dismemberment its origin is nowhere near
  // the visible body.
  Vec3 target = mobs.MobOrigin(id) +
                Vec3{def.worldSize.x * 0.5f, def.worldSize.y * 0.3f,
                     def.worldSize.z * 0.5f};
  float corpseSpread = 0;  // 0 while the mob is alive; see below
  {
    std::vector<BodyXformGpu> mt;
    mobs.AppendXforms(mt);
    // A KILLED mob has no live limbs at all: Die() hands every body to
    // DebrisSystem and drops the husk, so AppendXforms is empty and MobOrigin
    // above is a dead id. `xf` is the registry's whole-world walk, and in this
    // harness the only bodies in the world are this mob's corpse — so it is
    // the corpse's centroid, which is what "sever a vital limb and look at the
    // ragdoll" needs the camera to find.
    const std::vector<BodyXformGpu>& src = mt.empty() ? xf : mt;
    if (!src.empty()) {
      Vec3 sum{};
      for (const BodyXformGpu& m : src)
        sum += Vec3{m.pos[0], m.pos[1], m.pos[2]};
      target = sum * (1.0f / (float)src.size()) + Vec3{0, 1, 0};
      // How far the pieces actually spread, for the framing below. A corpse is
      // lying down and half its def's standing height across, so framing it by
      // the def's box leaves it a speck in the middle of a landscape shot.
      for (const BodyXformGpu& m : src)
        corpseSpread = std::max(
            corpseSpread, (Vec3{m.pos[0], m.pos[1], m.pos[2]} - target).len());
    }
  }

  const uint32_t W = 1280, H = 720;
  // Honour `--time` here the same way RunShots does. Passing a literal 0 tick
  // pinned every mob shot to MIDNIGHT, which is the worst possible light for
  // judging a character's silhouette — the whole point of this mode.
  const Tuning& mobShotTun = CurrentTuning();
  uint32_t mobShotTicksPerDay = TicksPerDay(mobShotTun);
  uint32_t mobShotTick = (uint32_t)((double)g_shotTimeOfDay *
                                    (double)mobShotTicksPerDay) %
                         mobShotTicksPerDay;
  auto shoot = [&](Vec3 dir, float dist, const char* path) {
    Vec3 eye = target + dir.normalized() * dist;
    Vec3 look = (target - eye).normalized();
    Camera cam;
    cam.yaw = std::atan2(look.z, look.x);
    cam.pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
    WriteRenderParams(ctx.queue, world, eye, cam, (float)W / H, true, 0.0f,
                      kFarFogDensity, 1080.0f, mobShotTick);
    rhi::Texture tex = ctx.device.CreateTexture(
        {W, H, 1}, rhi::TextureFormat::RGBA8Unorm,
        rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
        "shotTarget");
    // Upload BEFORE the render pass opens (barrier graph §4.6).
    uint32_t microCount = sim.UploadMicroBodyInsts(ctx.queue, microInsts);
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeShadowResolve(enc);
    rhi::RenderPass rp = sim.BeginRenderPass(
        enc, tex.CreateView(), rhi::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    sim.DrawBodies(rp, (uint32_t)inst.size());
    sim.DrawMicroBodies(rp, microCount);
    rp.End();
    rhi::Buffer shotBuf = CreateBuffer(
        ctx.device, (uint64_t)W * H * 4,
        rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "mobShot");
    rhi::TexelCopyTexture srcT{};
    srcT.texture = tex;
    rhi::TexelCopyBuffer dstB{};
    dstB.buffer = shotBuf;
    dstB.bytesPerRow = W * 4;
    dstB.rowsPerImage = H;
    rhi::Extent3D ext{W, H, 1};
    enc.CopyTextureToBuffer(srcT, dstB, ext);
    ctx.queue.Submit(enc.Finish());
    std::vector<uint8_t> pixels((size_t)W * H * 4, 0);
    rhi::ReadBufferBlocking(ctx.device, shotBuf, 0, pixels.data(), (size_t)(pixels.size()));
    if (WriteBmpFile(path, pixels, W, H)) std::printf("wrote %s\n", path);
  };
  // Camera directions are relative to the mob's FACING (it turns while it
  // walks): the side view is the one that shows pitch, the front quarter
  // shows limb placement.
  Vec3 fwd = mobs.MobFacing(id);
  Vec3 right{fwd.z, 0, -fwd.x};
  // Frame by the corpse's measured spread once the mob is dead, and by the
  // def's own SIZE while it is alive: the dummy and critter are ~8 voxels tall
  // but a humanoid rig is ~28, and a constant distance either crops the tall
  // one or leaves the short one a speck.
  const float shotDist =
      corpseSpread > 0.5f
          ? std::max(14.0f, 3.2f * corpseSpread)
          : std::max(18.0f, 2.4f * std::max(def.worldSize.y,
                                            std::max(def.worldSize.x,
                                                     def.worldSize.z)));
  for (uint64_t b : droppedBodies) {
    BodyTransform bx{};
    const bool live = phys.GetTransform(b, bx);
    std::printf("--shot-mob: dropped body %llu %s at (%.1f, %.1f, %.1f), camera "
                "target (%.1f, %.1f, %.1f)\n",
                (unsigned long long)b, live ? "is" : "is GONE", bx.pos.x,
                bx.pos.y, bx.pos.z, target.x, target.y, target.z);
  }
  shoot(right + Vec3{0, 0.15f, 0}, shotDist, "screenshot_mob_side.bmp");
  shoot((fwd + right) * 0.7071f + Vec3{0, 0.3f, 0}, shotDist,
        "screenshot_mob_quarter.bmp");
  shoot(fwd + Vec3{0, 0.15f, 0}, shotDist, "screenshot_mob_front.bmp");
  // ...a close-up of what is in the fist: at the body's framing a sword is a
  // few dozen pixels, which cannot show whether its art reads (a bevel, a
  // wrap, a guard). Aimed at the held part's own body, so it follows the arm.
  if (Mob* hm = mobs.FindCreature(id); hm && hm->HeldSlot() >= 0) {
    target = mobs.LimbPosition(id, hm->HeldSlot());
    // `right` is the creature's LEFT in this frame (see the side shot), and
    // a weapon is held in the right hand, so both views come from -right:
    // level with the blade, and from above and a little ahead.
    shoot(Vec3{0, 0.3f, 0} - right, 7.0f, "screenshot_mob_held.bmp");
    shoot(fwd * 0.35f - right + Vec3{0, 1.1f, 0}, 7.0f,
          "screenshot_mob_held2.bmp");
  }
  // ...and a close-up of what was dropped: the creature keeps walking while
  // the drop settles, so the three above cannot be trusted to frame it.
  if (!droppedBodies.empty()) {
    BodyTransform bx{};
    if (phys.GetTransform(droppedBodies[0], bx)) {
      target = bx.pos;
      shoot(Vec3{0.6f, 0.8f, 1.0f}, 12.0f, "screenshot_mob_drop.bmp");
    }
  }
  return ctx.ReportVkValidation("--shot-mob") > 0 ? 1 : 0;
}

// ===========================================================================
// --shot-strike <attacker>[:limb,...][+item,...] <style> [<target>[...]]
//
// ONE BLOW, PHOTOGRAPHED AND COUNTED. --shot-mob answers "what does this body
// look like"; this answers "what does this blow DO", which since 2026-09-15 is
// a different question with three independent halves (game/impact.h): a strike
// is a CUT, a BLUNT part and a BITE part, and from outside all three arrive as
// the same creature being a bit smaller than it was.
//
// WHY IT PRINTS A TABLE AND NOT A NUMBER. "The mace does not seem to do much"
// has at least six causes — the style was refused, the stroke never reached a
// cut tick, the edge was too slow (melee.minSpeed), it passed through nothing,
// it hit the PLATE and stopped, or it hit and the wound was small — and a bare
// "hp fell by 4" separates none of them (CLAUDE.md rule 6). So the readout
// carries the stroke's own tally (sweeps, bodies hit, top tip speed, the limb
// the style DREW), the profile that was actually swung (after the gauntlet
// override and the creature's infection), and a per-slot diff of everything
// the wound model can move: flesh and shell voxels, hp, bleed budget, bruise
// cells, infected cells, rot stain.
//
// The diff is per SLOT and taken before/after rather than per hit, because a
// sweep meets several slots in one tick (a plate and the chest under it) and
// "one line per hit" would have to invent an attribution the resolver does not
// make. Every slot that moved gets a line; nothing that moved is left out.
//
// THE PICTURES ARE TAKEN ON THE STROKE'S OWN PHASES, not on frame numbers, for
// the reason --shot-jump states: the tick counts are tempo-jittered, so a
// frame schedule photographs a different part of the swing on every seed.
// ===========================================================================

// `name[:limb,limb][+item,item]` — the same spelling --shot-mob takes, plus a
// `+` list that may be repeated. Deliberately permissive about which separator
// repeats: `human+mace+iron_gauntlets` and `human+mace,iron_gauntlets` are the
// same request and guessing wrong costs a rebuild.
struct StrikeSide {
  std::string def;
  std::vector<std::string> severs, items;
  // `@<voxels>` on the ATTACKER: stand them this far apart instead of at the
  // style's own reach. Not a convenience — the default is the honest one (a
  // style states the distance it commits from, and photographing it there is
  // how you find out it cannot actually reach), but once that is known, LOOKING
  // at the wound needs a blow that lands. --shot-mob's `@x,z` is the same idea
  // for the same reason.
  float gap = 0;
};

static void SplitCsv(const std::string& s, std::vector<std::string>& out) {
  size_t p = 0;
  while (p <= s.size()) {
    const size_t c = s.find(',', p);
    const size_t end = (c == std::string::npos) ? s.size() : c;
    if (end > p) out.push_back(s.substr(p, end - p));
    if (c == std::string::npos) break;
    p = c + 1;
  }
}

static StrikeSide ParseStrikeSide(const std::string& spec) {
  StrikeSide out;
  size_t p = 0;
  std::string head;
  // Everything up to the first '+' is the def and its sever list; each '+'
  // chunk after that is one or more item names.
  const size_t firstPlus = spec.find('+');
  head = (firstPlus == std::string::npos) ? spec : spec.substr(0, firstPlus);
  if (firstPlus != std::string::npos) {
    p = firstPlus + 1;
    while (p <= spec.size()) {
      const size_t nx = spec.find('+', p);
      const size_t end = (nx == std::string::npos) ? spec.size() : nx;
      SplitCsv(spec.substr(p, end - p), out.items);
      if (nx == std::string::npos) break;
      p = nx + 1;
    }
  }
  if (const size_t at = head.find('@'); at != std::string::npos) {
    out.gap = (float)std::atof(head.c_str() + at + 1);
    head = head.substr(0, at);
  }
  const size_t colon = head.find(':');
  out.def = (colon == std::string::npos) ? head : head.substr(0, colon);
  if (colon != std::string::npos) SplitCsv(head.substr(colon + 1), out.severs);
  return out;
}

// Everything the wound model can move on one rig slot, in one struct, so a
// before/after pair is a subtraction rather than nine parallel arrays.
struct SlotState {
  uint32_t art = 0;      // art voxels still on the lattice
  float hp = 0;
  float bleed = 0;
  uint32_t bruise = 0;   // gore.bruiseMat cells
  uint32_t rot = 0;      // rotflesh cells
  uint32_t stain = 0;    // stained cells, any type
  bool alive = false;
};

static void SampleSlots(MobSystem& mobs, uint64_t id, int slots,
                        uint32_t bruiseMat, uint32_t rotMat,
                        std::vector<SlotState>& out) {
  out.assign((size_t)std::max(slots, 0), SlotState{});
  for (int i = 0; i < slots; i++) {
    SlotState& s = out[(size_t)i];
    s.alive = mobs.LimbBody(id, i) != 0;
    if (!s.alive) continue;
    s.art = mobs.LimbArtVoxelCount(id, i);
    s.hp = mobs.LimbHp(id, i);
    s.bleed = mobs.LimbBleedBudget(id, i);
    // A bruise is neither a material rewrite nor (since 2026-09-26) a coat:
    // it is the skin's own bruise byte (voxload.h PrefabVoxel::bruise).
    if (bruiseMat) s.bruise = mobs.LimbBruiseCount(id, i, 1);
    if (rotMat) s.rot = mobs.LimbMaterialCount(id, i, rotMat);
    s.stain = mobs.LimbStainCount(id, i, 1);
  }
}

int RunStrikeShot(GpuContext& ctx, World& world, Simulation& sim, Physics& phys,
                  DebrisSystem& debris, MobSystem& mobs,
                  const ItemLibrary& items, const std::string& attackerSpec,
                  const std::string& styleName,
                  const std::string& targetSpec, int tailTicksWanted,
                  Stream& stream, const std::vector<MaterialDef>& mats) {
  const StrikeSide A = ParseStrikeSide(attackerSpec);
  const StrikeSide B =
      ParseStrikeSide(targetSpec.empty() ? std::string("human") : targetSpec);
  auto findDef = [&](const std::string& n) {
    for (size_t i = 0; i < mobs.Defs().size(); i++)
      if (mobs.Defs()[i].name == n) return (int)i;
    return -1;
  };
  const int defA = findDef(A.def), defB = findDef(B.def);
  if (defA < 0 || defB < 0) {
    std::fprintf(stderr, "--shot-strike: no mob def named \"%s\"\n",
                 defA < 0 ? A.def.c_str() : B.def.c_str());
    return 1;
  }
  const int styleIdx = mobs.AttackStyles().Find(styleName);
  if (styleIdx < 0) {
    std::fprintf(stderr, "--shot-strike: no attack style named \"%s\"\n",
                 styleName.c_str());
    return 1;
  }
  const AttackStyle& sty = *mobs.AttackStyles().At(styleIdx);
  const MobDef& dA = mobs.Defs()[defA];
  const MobDef& dB = mobs.Defs()[defB];

  // ---- FLAT GROUND, because a strike is measured in voxels ---------------
  // The default --shot-mob column is forested and sloped; a lunge that lands
  // on a root reports a landing distance that is about the root. This walks a
  // small neighbourhood for the flattest 5x5 it can find, which is cheap
  // (TerrainHeight is the same function worldgen uses) and makes the two
  // creatures' feet comparable.
  int spawnX = 137, spawnZ = 139, bestSpread = 1 << 30;
  for (int ox = -48; ox <= 48; ox += 8) {
    for (int oz = -48; oz <= 48; oz += 8) {
      int lo = 1 << 30, hi = -(1 << 30);
      for (int dx = -2; dx <= 2; dx++)
        for (int dz = -2; dz <= 34; dz += 4) {
          const int h = World::TerrainHeight(137 + ox + dx, 139 + oz + dz,
                                             kDefaultSeed);
          lo = std::min(lo, h);
          hi = std::max(hi, h);
        }
      if (hi - lo < bestSpread) {
        bestSpread = hi - lo;
        spawnX = 137 + ox;
        spawnZ = 139 + oz;
      }
    }
  }

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  const int h = World::TerrainHeight(spawnX, spawnZ, kDefaultSeed);
  uint32_t t = 6000;
  for (int i = 0; i < 60; i++)
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
               {8, h / 16, 8}, false, false);
  ctx.WaitIdle();

  // THE GAME'S OWN TICK (W2-O, test/tickrig.h), as --shot-mob's.
  support::TickRig rig(
      support::TickEngine{ctx, world, sim, stream, phys, mobs, debris, mats, items},
      t, IVec3{8, h / 16, 8});
  auto mobTick = [&]() {
    rig.tick = t;
    support::RunTicks(rig, 1);
    t = rig.tick;
  };

  // ---- HOW FAR APART: THE DISTANCE THE AI WOULD ACTUALLY COMMIT FROM -----
  //
  // `MobSystem::StyleReachOn` -- the same number `BeginStroke` refuses a style
  // against and `AttackReachOf` hands the arbiter -- which for a style that
  // authors no `reach` is derived from the body: the effector's own reach plus
  // whatever the lunge closes plus a body's half-depth. Photographing a blow
  // at any other distance photographs a fight that never happens, and the
  // harness's old fallback (the profile's 9, or a flat 10) is exactly how a
  // punch came to be measured from nine voxels out on an arm that reaches
  // five.
  //
  // IT NEEDS A LIVE RIG, so the attacker is spawned and given a few ticks to
  // flatten a pose FIRST and the victim is placed afterwards. A `@gap`
  // override still wins outright -- that is what it is for.
  const uint64_t idA = mobs.Spawn(defA, {spawnX, h + 1, spawnZ});
  if (!idA) {
    std::fprintf(stderr, "--shot-strike: spawn failed\n");
    return 1;
  }
  // PINNED BEFORE IT IS TICKED, not after. `duelist` is mobile and hostile,
  // and four ticks of it is four ticks of a 31.5 vox/s walk -- measured, the
  // attacker had wandered 3.2 voxels out of its own stand-off before the
  // victim was even placed, and the readout's "shoulder-to-chest" came back
  // at 3.8 on a 7-voxel gap. A harness that photographs a distance has to own
  // that distance from the first tick.
  mobs.SetMobBehavior(idA, "training_dummy");
  // ---- ARMED BEFORE IT IS MEASURED --------------------------------------
  // `StyleReachOn` asks the LIVE weapon how far it reaches, so a mace has to
  // be in the fist before the stand-off is computed -- measured, computing it
  // first stood a mace-holder at the profile's 10 and 98 of its 100 probe rays
  // found air. Only the HELD item is needed here (armour changes no reach);
  // the full `dress` still runs below and is what reports what was worn.
  for (const std::string& nm0 : A.items) {
    const ItemDef* it0 = items.At(items.Find(nm0));
    if (it0 != nullptr && it0->kind == ItemKind::Melee) mobs.EquipItem(idA, it0);
  }
  for (int i = 0; i < 4; i++) mobTick();
  float gap = A.gap;
  if (gap <= 0.0f) {
    if (Mob* m0 = mobs.FindMobById(idA)) gap = mobs.StyleReachOn(*m0, sty);
    if (gap <= 0.0f) {
      const int prof = mobs.Behaviors().Find(dA.behavior.empty() ? "duelist"
                                                                 : dA.behavior);
      const ai::Profile* p = mobs.Behaviors().At(prof);
      gap = (p != nullptr && p->attack.reach > 0.0f) ? p->attack.reach : 9.0f;
    }
  }
  const int gapZ = std::max(2, (int)std::lround(gap));

  const int hB = World::TerrainHeight(spawnX, spawnZ + gapZ, kDefaultSeed);
  const uint64_t idB = mobs.Spawn(defB, {spawnX, hB + 1, spawnZ + gapZ});
  if (!idA || !idB) {
    std::fprintf(stderr, "--shot-strike: spawn failed\n");
    return 1;
  }
  // Heading 0 is +Z (the rig convention), so the attacker already faces the
  // target's column and the target faces back down it.
  mobs.SetHeading(idA, 0.0f);
  mobs.SetHeading(idB, 3.14159265f);
  // ---- BOTH SIDES ARE PINNED, and that is a deliberate deviation ---------
  //
  // Spawn already applied each def's own `behavior` (zombie.json names one),
  // and for anything the AI decides that is the right profile. It is the wrong
  // one for a MEASUREMENT, for two separate reasons that both showed up on the
  // first run:
  //
  //   * A duelist KEEPS ITS RANGE. Stood at the style's own reach and left for
  //     65 settle ticks, both creatures walked to the profile's preferred band
  //     — measured 11.0 voxels shoulder-to-chest whether they were placed 7
  //     apart or 9, which makes the stand-off this harness is FOR unauthorable.
  //   * A hostile profile SWINGS ON ITS OWN CLOCK, and `ForceAttack` refuses
  //     while a stroke is live (a queued swing is an unbounded backlog), so the
  //     blow being photographed would sometimes be a different one.
  //
  // `training_dummy` is blind, passive and immobile, so neither happens and
  // the heading stays where it was put. Nothing about the BLOW changes:
  // ForceAttack replays the named style through the same stroke program the
  // AI's request would have started, and the profile has no say in it.
  const std::string profA = dA.behavior.empty() ? "duelist" : dA.behavior;
  mobs.SetMobBehavior(idA, "training_dummy");
  mobs.SetMobBehavior(idB, "training_dummy");

  auto dress = [&](uint64_t id, const StrikeSide& side, const char* who) {
    for (const std::string& nm : side.items) {
      const ItemDef* it = items.At(items.Find(nm));
      if (it == nullptr) {
        std::fprintf(stderr, "--shot-strike: no item named \"%s\"\n", nm.c_str());
        continue;
      }
      if (ItemKindIsWorn(it->kind)) {
        const int slot = EquipSlotFor(it->kind, Equipment{});
        if (slot < 0 || !mobs.WearItem(id, it, slot))
          std::fprintf(stderr, "--shot-strike: %s could not wear %s\n", who,
                       nm.c_str());
        else
          std::printf("--shot-strike: %s wears %s (slot %d)\n", who, nm.c_str(),
                      slot);
      } else if (!mobs.EquipItem(id, it)) {
        std::fprintf(stderr, "--shot-strike: %s could not hold %s\n", who,
                     nm.c_str());
      } else {
        std::printf("--shot-strike: %s holds %s\n", who, nm.c_str());
      }
    }
  };
  auto maim = [&](uint64_t id, const MobDef& d, const StrikeSide& side,
                  const char* who) {
    for (const std::string& nm : side.severs) {
      int li = -1;
      for (size_t i = 0; i < d.limbs.size(); i++)
        if (d.limbs[i].name == nm) li = (int)i;
      if (li < 0) {
        std::fprintf(stderr, "--shot-strike: %s (\"%s\") has no limb \"%s\"\n",
                     who, d.name.c_str(), nm.c_str());
        continue;
      }
      mobs.Sever(id, li);
      std::printf("--shot-strike: %s loses %s\n", who, nm.c_str());
    }
  };
  dress(idA, A, "attacker");
  dress(idB, B, "target");
  for (int i = 0; i < 20; i++) mobTick();   // settle, and let the gait pose
  maim(idA, dA, A, "attacker");
  maim(idB, dB, B, "target");
  // Long enough for a severed rig to fall into its crawl state and latch it —
  // the whole point of the "zombie with both legU off" case is that the state
  // rule has taken effect before the leap is asked for.
  for (int i = 0; i < 45; i++) mobTick();

  auto chestOf = [&](uint64_t id, const MobDef& d) {
    return mobs.MobOrigin(id) + Vec3{d.worldSize.x * 0.5f, d.worldSize.y * 0.62f,
                                     d.worldSize.z * 0.5f};
  };

  const uint32_t bruiseMat = mobs.MaterialIdNamed(CurrentTuning().gore.bruiseMat);
  const uint32_t rotMat = mobs.MaterialIdNamed("rotflesh");
  Mob* mB = mobs.FindMobById(idB);
  Mob* mA = mobs.FindMobById(idA);
  if (mA == nullptr || mB == nullptr) {
    std::fprintf(stderr, "--shot-strike: a combatant did not survive setup\n");
    return 1;
  }
  const int slotsB = mB->LimbCount();
  std::vector<SlotState> before, after;
  SampleSlots(mobs, idB, slotsB, bruiseMat, rotMat, before);
  const float hpB0 = mobs.TotalHp(idB);

  // ---- SWING IT. A FIXED SEED, because the tempo jitter decides the tick
  // counts and a look-iteration harness whose swing changes length between
  // runs cannot be compared to itself (mob.h ForceAttack says the same).
  // RE-ASSERTED AFTER THE SETTLE, not only at spawn: a gait, a slide down a
  // dune or a single AI tick before the pin took can leave a creature a few
  // degrees off, and a stroke aimed from a body facing 10 degrees wide is a
  // different swing (the aim is taken in the wielder's own basis).
  mobs.SetHeading(idA, 0.0f);
  mobs.SetHeading(idB, 3.14159265f);
  const Vec3 aimAt = chestOf(idB, dB);
  const Vec3 fromA = mobs.MobOrigin(idA);
  if (!mobs.ForceAttack(idA, styleName, aimAt, t, 0x5EED51UL)) {
    std::fprintf(stderr,
                 "--shot-strike: \"%s\" was refused — the attacker has no "
                 "weapon for it, or a stroke was already live. Styles this "
                 "creature can use:", styleName.c_str());
    for (const AttackStyle& s2 : mobs.AttackStyles().styles)
      if (StyleUsable(*mA, s2)) std::fprintf(stderr, " %s", s2.name.c_str());
    std::fprintf(stderr, "\n");
    return 1;
  }

  // ---- the camera, and the five pictures --------------------------------
  const uint32_t W = 1280, H = 720;
  const Tuning& tun = CurrentTuning();
  const uint32_t ticksPerDay = TicksPerDay(tun);
  const uint32_t shotTick =
      (uint32_t)((double)g_shotTimeOfDay * (double)ticksPerDay) % ticksPerDay;
  std::vector<MicroBodyInstGpu> microInsts;
  std::vector<BodyVoxInst> inst;
  auto shoot = [&](const char* phaseName) {
    // PUBLISHED EVERY SHOT, not once at the end. This harness is not the frame
    // loop, so the brick upload and the instance arrays only happen where they
    // are called — and the whole point here is that the body CHANGES between
    // pictures (--shot-mob learned this the hard way: eleven carved limbs did
    // not draw at all).
    if (MicroBodySet* mbs = debris.MicroSet(); mbs != nullptr && mbs->dirty)
      sim.UploadMicroBodies(ctx.queue, *mbs);
    BodyRegistry bodyReg(debris, mobs, nullptr);
    std::vector<BodyXformGpu> xf;
    bodyReg.BuildXforms(xf);
    if (!xf.empty())
      ctx.queue.WriteBuffer(world.bodyXforms, 0, xf.data(),
                            xf.size() * sizeof(BodyXformGpu));
    microInsts.clear();
    bodyReg.BuildMicroInsts(microInsts);
    inst.clear();
    bodyReg.BuildInstances(inst);
    if (!inst.empty())
      ctx.queue.WriteBuffer(world.bodyInstances, 0, inst.data(),
                            inst.size() * sizeof(BodyVoxInst));

    // FRAMED ON THE PAIR, from three-quarters: the attacker's own forward
    // rotated 40 degrees off, raised a little. A side-on camera hides the
    // closing distance and a front-on one hides the arm.
    const Vec3 pa = mobs.MobOrigin(idA) +
                    Vec3{dA.worldSize.x * 0.5f, dA.worldSize.y * 0.55f,
                         dA.worldSize.z * 0.5f};
    const Vec3 pb = chestOf(idB, dB);
    const Vec3 mid = (pa + pb) * 0.5f;
    const float sep = (pb - pa).len();
    // FRAMED ON WHAT IS IN THE PICTURE, which is two creatures and the gap
    // between them — not on a bounding sphere. 1.9x the taller creature's
    // whole height left the pair at a third of the frame and the wound
    // unreadable, which for a look-iteration harness is the only failure that
    // matters. The +6 is elbow room so a lunge does not leave the frame.
    const float span = std::max(sep, std::max(dA.worldSize.y, dB.worldSize.y));
    const float dist = std::max(16.0f, 1.15f * (span + 6.0f));
    const Vec3 dir = Vec3{0.77f, 0.42f, -0.64f}.normalized();
    const Vec3 eye = mid + dir * dist;
    const Vec3 look = (mid - eye).normalized();
    Camera cam;
    cam.yaw = std::atan2(look.z, look.x);
    cam.pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
    WriteRenderParams(ctx.queue, world, eye, cam, (float)W / H, true, 0.0f,
                      kFarFogDensity, 1080.0f, shotTick);
    rhi::Texture tex = ctx.device.CreateTexture(
        {W, H, 1}, rhi::TextureFormat::RGBA8Unorm,
        rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
        "shotTarget");
    uint32_t microCount = sim.UploadMicroBodyInsts(ctx.queue, microInsts);
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeShadowResolve(enc);
    rhi::RenderPass rp = sim.BeginRenderPass(
        enc, tex.CreateView(), rhi::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    sim.DrawBodies(rp, (uint32_t)inst.size());
    sim.DrawMicroBodies(rp, microCount);
    rp.End();
    rhi::Buffer shotBuf = CreateBuffer(
        ctx.device, (uint64_t)W * H * 4,
        rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "strikeShot");
    rhi::TexelCopyTexture srcT{};
    srcT.texture = tex;
    rhi::TexelCopyBuffer dstB{};
    dstB.buffer = shotBuf;
    dstB.bytesPerRow = W * 4;
    dstB.rowsPerImage = H;
    rhi::Extent3D ext{W, H, 1};
    enc.CopyTextureToBuffer(srcT, dstB, ext);
    ctx.queue.Submit(enc.Finish());
    std::vector<uint8_t> pixels((size_t)W * H * 4, 0);
    rhi::ReadBufferBlocking(ctx.device, shotBuf, 0, pixels.data(),
                            (size_t)(pixels.size()));
    char path[256];
    std::snprintf(path, sizeof(path), "shot_strike_%s_%s.bmp",
                  styleName.c_str(), phaseName);
    if (WriteBmpFile(path, pixels, W, H)) std::printf("wrote %s\n", path);
  };

  // ---- run the stroke, photographing its own phases ----------------------
  bool shotWindup = false, shotCutStart = false, shotCutEnd = false;
  bool shotRecover = false;
  int airTicks = 0, cutTicks = 0, sweeps = 0, bodiesHit = 0;
  int targetLimb = -1, tailTicks = -1;
  bool arrested = false, launched = false;
  float topTipSpeed = 0, peakRise = 0;
  Vec3 landedAt = fromA;
  bool wasCutting = false;
  // ---- WHAT SWUNG, AND HOW CLOSE IT EVER GOT ----------------------------
  //
  // The effector is CLEARED when the stroke ends (Mob::ClearStrikeEffector — a
  // creature that finishes a punch goes back to carrying whatever is in its
  // fist), so asking afterwards which weapon swung answers "(nothing)". The
  // weapon is therefore latched on the first cut tick.
  //
  // THE GAP IS THE MINIMUM OVER THE WHOLE CUT, and sampling it once was the
  // instrument's own first bug: a cut OPENS at the chamber, so one sample on
  // the first cut tick measures how far the fist was BEFORE it travelled and
  // reported a punch from 6 voxels as 5.1 voxels short of a body it might well
  // have reached. "How close did the point ever get" is the question, and it
  // is the one that separates a STAND-OFF problem (the style's `reach` is
  // further than the arm can serve) from a wound-model one (it touched and did
  // nothing). A bare `bodiesHit 0` says neither — CLAUDE.md rule 6.
  StrikeProfile prof{};
  std::string weaponName = "(nothing)";
  float edgeGap = 1e9f, edgeLen = 0, chestGap = -1;
  bool sampled = false;
  // ---- WHY THE POINT DID NOT GET THERE, in the arm's own numbers ---------
  // A bare "0.7 voxels short" has four causes and from outside they are the
  // same miss: the driver never COMMANDED the extension, the IK could not
  // serve it, the pose clamp took it back, or the reported hand and the solved
  // hand disagree. Mob::WeaponArmDiag separates all four, and this harness is
  // where a human reads them (CLAUDE.md rule 6: attribution beats elimination).
  int probesCast = 0, probesAir = 0, probesSelf = 0, probesBody = 0;
  int diagTicks = 0, diagRan = 0;
  float cmdReachMax = 0, gotReachMax = 0;
  float worstIkMiss = 0, worstClampShift = 0, worstRoundTrip = 0;
  float worstShoulderClamp = 0, worstElbowClamp = 0;
  auto sampleWeapon = [&]() {
    Mob* m = mobs.FindMobById(idA);
    if (m == nullptr) return;
    if (!sampled) {
      sampled = true;
      if (const MobNaturalWeaponDef* nw = m->EffectorWeapon(); nw != nullptr) {
        prof = m->StrikeProfileFor(*nw);
        weaponName = nw->name + " (" + nw->part + ", " +
                     (m->StrikeEffectorKind() == StrikeEffectorMode::Aim
                          ? "aim" : "chain") + ")";
      } else if (!m->HeldItem().empty()) {
        if (const ItemDef* it = items.At(items.Find(m->HeldItem()));
            it != nullptr) {
          prof = it->strike;
          weaponName = m->HeldItem() + " (held)";
        }
      }
      const Vec3 shoulder =
          mobs.MobOrigin(idA) + Vec3{dA.worldSize.x * 0.5f,
                                     dA.worldSize.y * 0.62f,
                                     dA.worldSize.z * 0.5f};
      chestGap = (chestOf(idB, dB) - shoulder).len();
    }
    Vec3 base{}, tip{};
    float hw = 0;
    if (!m->WeaponEdge(base, tip, hw, nullptr)) return;
    edgeLen = (tip - base).len();
    // To the NEAREST LIVE VOXEL of the victim, which is the question the
    // sweep's probe rays actually ask — not to its centre, which a creature
    // with any width at all makes meaningless.
    float best = 1e9f;
    for (int li = 0; li < slotsB; li++) {
      if (!mobs.LimbBody(idB, li)) continue;
      const uint32_t n = mobs.LimbVoxelCount(idB, li);
      for (uint32_t v = 0; v < n; v += 17u)
        best = std::min(best, (mobs.LimbVoxelPos(idB, li, v) - tip).len());
    }
    if (best < 1e8f) edgeGap = std::min(edgeGap, best - hw);
    const Mob::WeaponArmDiag& d = m->WeaponArmDiagnostics();
    diagTicks++;
    if (d.ran) {
      diagRan++;
      cmdReachMax = std::max(cmdReachMax, d.cmdHand.len());
      gotReachMax = std::max(gotReachMax, d.gotHand.len());
      worstIkMiss = std::max(worstIkMiss, d.ikMiss);
      worstClampShift = std::max(worstClampShift, d.clampShift);
      worstRoundTrip = std::max(worstRoundTrip, d.roundTrip);
      worstShoulderClamp = std::max(worstShoulderClamp, d.shoulderClamp);
      worstElbowClamp = std::max(worstElbowClamp, d.elbowClamp);
    }
  };
  for (int i = 0; i < 260; i++) {
    const Mob* m = mobs.FindMobById(idA);
    const NpcStroke* s = m != nullptr ? mobs.MobStroke(idA) : nullptr;
    if (s != nullptr) {
      if (s->Cutting()) cutTicks++;
      sweeps = std::max(sweeps, s->sweeps);
      probesCast = std::max(probesCast, s->probesCast);
      probesAir = std::max(probesAir, s->probesAir);
      probesSelf = std::max(probesSelf, s->probesSelf);
      probesBody = std::max(probesBody, s->probesBody);
      bodiesHit = std::max(bodiesHit, s->bodiesHit);
      topTipSpeed = std::max(topTipSpeed, s->topTipSpeed);
      if (s->targetLimb >= 0) targetLimb = s->targetLimb;
      arrested = arrested || s->arrested;
    }
    if (m != nullptr) {
      if (m->Airborne()) {
        launched = true;
        airTicks++;
      } else if (launched) {
        landedAt = m->Origin();
      }
      peakRise = std::max(peakRise, m->Origin().y - fromA.y);
    }
    // HALFWAY THROUGH THE WINDUP, not at its start: the first tick of a
    // telegraph is the rest pose with one frame of lean on it, which is a
    // picture of nothing.
    if (s != nullptr && !shotWindup &&
        s->phase == NpcStroke::Phase::Windup &&
        s->phaseTick * 2 >= s->windupTicks) {
      shotWindup = true;
      shoot("windup");
    }
    if (s != nullptr && s->Cutting()) sampleWeapon();
    if (s != nullptr && !shotCutStart && s->Cutting()) {
      shotCutStart = true;
      shoot("cut_start");
    }
    if (s != nullptr && !shotCutEnd && s->Cutting() &&
        s->phaseTick + 1 >= s->cutTicks) {
      shotCutEnd = true;
      shoot("cut_end");
    }
    if (s != nullptr && !shotRecover &&
        s->phase == NpcStroke::Phase::Recover &&
        s->phaseTick * 2 >= s->recoverTicks) {
      shotRecover = true;
      shoot("recover");
    }
    const bool live = s != nullptr && s->Active();
    if (!live && wasCutting && tailTicks < 0) tailTicks = 0;
    wasCutting = wasCutting || (s != nullptr && s->Cutting());
    if (tailTicks >= 0) {
      if (tailTicks >= tailTicksWanted) break;
      tailTicks++;
    }
    mobTick();
  }
  // The last picture is the one the OWNER asked for by name: twenty ticks
  // after the swing is over, which is where the blood has finished running and
  // a rot wound has had time to look like one.
  //
  // ...AND A DISEASE IS SLOWER THAN A BLOW. `--shot-strike-tail <n>` moves that
  // tail, because the wound an INFECTION makes does not exist twenty ticks
  // after the bite -- the rot is still the size of the teeth. Photographing
  // what it eventually looks like (a limb worked through to bloodied bone,
  // Mob::InfectStep) needs hundreds, and the picture is named for the count so
  // two tails can sit side by side in a directory.
  if (!shotCutStart) shoot("cut_start");
  if (!shotRecover) shoot("recover");
  const std::string tailName = "after" + std::to_string(tailTicksWanted);
  shoot(tailName.c_str());

  // ---- WHAT IT DID ------------------------------------------------------
  SampleSlots(mobs, idB, slotsB, bruiseMat, rotMat, after);
  sampleWeapon();   // a stroke that never cut still has a weapon to name
  if (edgeGap > 1e8f) edgeGap = -1.0f;   // never measured: say so, not 1e9
  std::printf(
      "\n--shot-strike: %s  \"%s\"  ->  %s\n"
      "  weapon      %s\n"
      "  profile     cut %.1f  blunt %.1f  bluntCarve %.2f  armorBreak %.2f  "
      "bite %.1f  infect %u/%u\n"
      "  stand-off   %d voxels%s (style reach %.0f)\n"
      "  behaviour   %s (both sides pinned to training_dummy for the shot: an\n"
      "              AI that keeps its range makes the stand-off unauthorable)\n"
      "  stroke      %d sweep ticks, %d cut ticks, %d bodies hit, top tip "
      "%.1f vox/s%s\n"
      "  geometry    a %.1f-voxel edge; over the cut its point came within "
      "%.1f vox of the victim's nearest voxel (shoulder-to-chest %.1f)\n",
      attackerSpec.c_str(), styleName.c_str(),
      targetSpec.empty() ? "human" : targetSpec.c_str(), weaponName.c_str(),
      prof.cut, prof.blunt, prof.bluntCarve, prof.armorBreak, prof.bite,
      (unsigned)prof.infectMat, (unsigned)prof.infectStain, gapZ,
      A.gap > 0.0f ? " (@ override)" : "", sty.reach, profA.c_str(),
      sweeps, cutTicks, bodiesHit, topTipSpeed,
      arrested ? " (ARRESTED by a parry)" : "", edgeLen, edgeGap, chestGap);
  // ---- THE ARM, when there is one (Mob::WeaponArmDiag) ------------------
  // Printed for every chain effector, held or natural, because "the point did
  // not get there" is the same question for a sword and a fist and the four
  // causes are the same four. `commanded -> solved` is the pair that matters:
  // they agree when the arm served the ask and diverge by exactly the
  // shortfall when it could not.
  std::printf(
      "  probes      %d rays cast: %d found air, %d never left the wielder, "
      "%d found a body\n",
      probesCast, probesAir, probesSelf, probesBody);
  // ZERO IS AS INFORMATIVE AS ANY OTHER NUMBER HERE: an aim effector has no
  // arm and reports 0/N, which is how a reader tells "the jaws are not on a
  // chain" from "the chain never ran".
  if (diagTicks > 0)
    std::printf(
        "  arm[%d/%d]   hand reach commanded %.2f -> solved %.2f vox; ik miss "
        "%.2f, clamp shift %.2f vox (shoulder %.2f rad, elbow %.2f rad), "
        "read-back err %.2f\n",
        diagRan, diagTicks, cmdReachMax, gotReachMax, worstIkMiss,
        worstClampShift,
        worstShoulderClamp, worstElbowClamp, worstRoundTrip);
  if (targetLimb >= 0 && targetLimb < (int)dB.limbs.size())
    std::printf("  aimed at    %s (the style's `target` table drew it)\n",
                dB.limbs[(size_t)targetLimb].name.c_str());
  else
    std::printf("  aimed at    the chest (no `target` table, or nothing live)\n");
  if (sty.lunge.Any() || launched)
    std::printf(
        "  lunge       %s: %d air ticks, peak rise %.2f vox, travelled %.2f "
        "vox (authored %d ticks at %.1f m/s, rise %.1f)\n",
        launched ? "LAUNCHED" : "never left the ground", airTicks, peakRise,
        std::sqrt((landedAt.x - fromA.x) * (landedAt.x - fromA.x) +
                  (landedAt.z - fromA.z) * (landedAt.z - fromA.z)),
        sty.lunge.ticks, sty.lunge.speed, sty.lunge.rise);
  std::printf("  victim hp   %.1f -> %.1f\n", hpB0, mobs.TotalHp(idB));

  // Per-slot, and only the slots that MOVED. `< AppendedBase()` is the rig's
  // own flesh/shell split (game/impact.h StruckKind) and is the line the
  // resolver classifies on, so the report names it the same way.
  std::printf("  slot                 kind   voxels     hp        bleed   "
              "bruise  rot   stain\n");
  bool anyRow = false;
  for (int i = 0; i < slotsB && i < (int)after.size(); i++) {
    const SlotState& b = before[(size_t)i];
    const SlotState& a = after[(size_t)i];
    const int dArt = (int)a.art - (int)b.art;
    const bool moved = dArt != 0 || std::fabs(a.hp - b.hp) > 0.01f ||
                       std::fabs(a.bleed - b.bleed) > 0.01f ||
                       a.bruise != b.bruise || a.rot != b.rot ||
                       a.stain != b.stain || a.alive != b.alive;
    if (!moved) continue;
    anyRow = true;
    std::string name;
    const char* kind = "flesh";
    if (i < mB->AppendedBase()) {
      name = (i < (int)dB.limbs.size()) ? dB.limbs[(size_t)i].name
                                        : std::string("limb?");
    } else {
      kind = (i == mB->HeldSlot()) ? "held" : "shell";
      const int host = mB->WornHostOf(i);
      name = std::string(kind) + " over " +
             ((host >= 0 && host < (int)dB.limbs.size())
                  ? dB.limbs[(size_t)host].name
                  : std::string("?"));
    }
    std::printf("  %-20s %-6s %6d   %6.1f->%-6.1f %5.1f  %+6d %+5d %+6d%s\n",
                name.c_str(), kind, dArt, b.hp, a.hp, a.bleed,
                (int)a.bruise - (int)b.bruise, (int)a.rot - (int)b.rot,
                (int)a.stain - (int)b.stain,
                (b.alive && !a.alive) ? "   SEVERED" : "");
  }
  if (!anyRow)
    std::printf("  (nothing on the target changed — the blow missed, or it "
                "was too slow to do anything: melee.minSpeedMps)\n");
  return ctx.ReportVkValidation("--shot-strike") > 0 ? 1 : 0;
}


struct KeyEdge {
  bool prev = false;
  bool Pressed(bool now) {
    bool e = now && !prev;
    prev = now;
    return e;
  }
};

// Grenade / UpdateGrenade and LaserMuzzle moved to game/session.h+.cpp
// (N5): the tick body throws the grenades and fires the ray, the render
// block draws both, and those are two files now.

// ---- --heightmap: the tuner's terrain map --------------------------------
//
// A res x res grid of World::TerrainColumn over a `span`-voxel square centred
// on (cx, cz), written as a small binary the browser decodes with a DataView.
// Binary rather than JSON because a 384x384 map is 147k columns and the JSON
// for it is ~4 MB of text to parse on every slider drag; this is 1.2 MB of
// bytes with no parse at all.
//
// LAYOUT (all little-endian, which every platform this runs on is):
//   0  u32  magic 'SVHM'                16  i32 cx
//   4  u32  version (1)                 20  i32 cz
//   8  u32  res                         24  u32 seed
//   12 u32  span (voxels)               28  i32 voxelsPerMetre
//   32 i32  hMin   36 i32 hMax          40 i32 seaHint (the map's terrain.homeArea.y)
//   44 u32  reserved
//   48 .. res*res * 8 bytes: i32 h, i16 water (h - water, clamped, or -32768
//          for dry), u8 sed, u8 slope (Q8 clamped to 255)
//
// `water` is stored as a DEPTH relative to the ground rather than an absolute
// Y so it fits in 16 bits at any datum — the mistake the rest of this overhaul
// spent a day undoing, made once, deliberately, where it is bounded.
int WriteHeightmap(const std::string& spec, const std::string& outPath) {
  int cx = 0, cz = 0, span = 4096, res = 256;
  unsigned seed = kDefaultSeed;
  {
    std::vector<long> v;
    const char* p = spec.c_str();
    while (*p) {
      char* end = nullptr;
      long n = std::strtol(p, &end, 10);
      if (end == p) break;
      v.push_back(n);
      p = end;
      while (*p == ',' || *p == ' ') p++;
    }
    if (v.size() < 4) {
      std::fprintf(stderr, "--heightmap wants cx,cz,span,res[,seed], got '%s'\n",
                   spec.c_str());
      return 1;
    }
    cx = (int)v[0]; cz = (int)v[1]; span = (int)v[2]; res = (int)v[3];
    if (v.size() >= 5) seed = (unsigned)v[4];
  }
  // Bounds, because this is reachable from a browser: a res of 4096 is 16.7M
  // columns at ~25 hash3 each and would hang the tuner rather than fail it.
  if (res < 8) res = 8;
  if (res > 1024) res = 1024;
  if (span < res) span = res;                   // never finer than one voxel
  if (span > 1 << 22) span = 1 << 22;

  std::vector<uint8_t> buf((size_t)48 + (size_t)res * res * 8);
  auto put32 = [&](size_t off, uint32_t v) {
    buf[off] = (uint8_t)v; buf[off + 1] = (uint8_t)(v >> 8);
    buf[off + 2] = (uint8_t)(v >> 16); buf[off + 3] = (uint8_t)(v >> 24);
  };
  int hMin = INT32_MAX, hMax = INT32_MIN;
  const double step = (double)span / (double)res;
  for (int j = 0; j < res; j++) {
    const int wz = cz - span / 2 + (int)(((double)j + 0.5) * step);
    for (int i = 0; i < res; i++) {
      const int wx = cx - span / 2 + (int)(((double)i + 0.5) * step);
      const World::Column col = World::TerrainColumn(wx, wz, seed);
      hMin = std::min(hMin, col.h);
      hMax = std::max(hMax, col.h);
      const size_t o = 48 + ((size_t)j * res + i) * 8;
      put32(o, (uint32_t)col.h);
      int depth = col.water == INT32_MIN ? -32768
                                         : std::clamp(col.water - col.h, -32767, 32767);
      buf[o + 4] = (uint8_t)(depth & 0xFF);
      buf[o + 5] = (uint8_t)((depth >> 8) & 0xFF);
      buf[o + 6] = (uint8_t)std::clamp(col.sed, 0, 255);
      buf[o + 7] = (uint8_t)std::clamp(col.slope, 0, 255);
    }
  }
  put32(0, 0x4D485653u);   // 'SVHM' little-endian
  put32(4, 1);
  put32(8, (uint32_t)res);
  put32(12, (uint32_t)span);
  put32(16, (uint32_t)cx);
  put32(20, (uint32_t)cz);
  put32(24, seed);
  put32(28, (uint32_t)kVoxelsPerMetre);
  put32(32, (uint32_t)hMin);
  put32(36, (uint32_t)hMax);
  put32(40, (uint32_t)worldmap::CurrentTerrain().homeY);
  put32(44, 0);

  std::error_code ec;
  std::filesystem::create_directories(
      std::filesystem::path(outPath).parent_path(), ec);
  std::ofstream f(outPath, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "--heightmap: cannot write %s\n", outPath.c_str());
    return 1;
  }
  f.write((const char*)buf.data(), (std::streamsize)buf.size());
  f.close();
  std::printf("heightmap: %dx%d over %d voxels (%.1f m) at (%d,%d) seed %u, "
              "y%d..y%d -> %s\n",
              res, res, span, (double)span * kVoxelMeters, cx, cz, seed, hMin,
              hMax, outPath.c_str());
  return 0;
}

}  // namespace

// ---- --verify: the end-of-package check as ONE launch ----------------------
//
// Measured 2026-09-01/02: agents ran --gate X, --shot, --render-budget and
// --shader-stats as four separate boots — four device + SPIR-V + worldgen
// boots and four waits on the run lock, for one claim. Nothing in them needs
// its own process. This runs the gates (selftest::Run, which writes its own
// build/last_run.json), then the requested --shot frames, then the requested
// --render-budget arms, on the one GpuContext the normal path already built,
// and rewrites build/last_run.json with all three so the record of a
// verification is one file. Nonzero exit if any part failed.
int RunVerify(GpuContext& ctx, World& world, Simulation& sim,
              std::vector<MaterialDef>& mats,
              std::vector<ReactionGpu>& reactions, Physics& phys,
              DebrisSystem& debris, MobSystem& mobs, Stream& stream,
              ItemLibrary& items, const selftest::Options& stOpt,
              const sandvox::PerfOptions& perfOpt, bool doShots,
              bool doBudget) {
  const double t0 = NowSeconds();
  int failures = 0;
  std::printf("=== sandvox --verify ===\n");

  // The "gates" object selftest::Run wrote, kept verbatim: its writer owns
  // that format (status/seconds/detail per gate) and this file must stay
  // readable by whatever reads a plain --selftest run.
  std::string gatesJson;
  if (!stOpt.only.empty()) {
    std::printf("\n--- verify: %zu gate(s) ---\n", stOpt.only.size());
    selftest::Ctx sc{ctx,  world,  sim,  mats,   reactions,
                     phys, debris, mobs, stream, items};
    if (selftest::Run(sc, stOpt) != 0) failures++;
    std::ifstream f("build/last_run.json");
    std::string line;
    while (std::getline(f, line)) gatesJson += line + "\n";
  }

  if (doShots) {
    std::printf("\n--- verify: %zu shot frame(s) ---\n", g_shotOnly.size());
    g_shotResults.clear();
    if (RunShots(ctx, world, sim) != 0) failures++;
    for (const std::string& want : g_shotOnly) {
      bool seen = false;
      for (const ShotRecord& r : g_shotResults) {
        std::string p = r.file;
        if (p.size() > 4 && p.compare(p.size() - 4, 4, ".bmp") == 0) p.resize(p.size() - 4);
        if (p == want) seen = true;
      }
      // A frame name that matched nothing is the typo case: report it rather
      // than let "0 frames written" read as a pass.
      if (!seen) {
        std::fprintf(stderr, "--verify: no --shot frame named '%s'\n", want.c_str());
        g_shotResults.push_back({want + ".bmp", false});
        failures++;
      }
    }
  }

  std::vector<sandvox::RenderBudgetRow> rows;
  if (doBudget) {
    std::printf("\n--- verify: %zu render-budget arm(s) ---\n", perfOpt.arms.size());
    if (sandvox::RunRenderBudget(ctx, world, sim, mats, perfOpt, &rows) != 0)
      failures++;
  }

  // One file. Splice the shots and the arms into the gates document (or an
  // empty one) rather than emitting a second file nobody would look for.
  {
    std::string doc = gatesJson;
    // Cut at the gates document's own pipelineCompileMs section rather than at
    // its closing brace: that section is re-emitted below with the numbers as
    // of NOW, and the gates ran before the shots, which is when the deferred
    // far compile usually lands. Keeping both would be a duplicate key.
    size_t close = doc.find("\"pipelineCompileMs\"");
    if (close == std::string::npos) close = doc.rfind('}');
    if (close == std::string::npos) doc = "{\n  \"gates\": {}";
    else doc = doc.substr(0, close);
    while (!doc.empty() &&
           (doc.back() == '\n' || doc.back() == ' ' || doc.back() == ','))
      doc.pop_back();
    auto esc = [](const std::string& s) {
      std::string o;
      for (char ch : s) {
        if (ch == '"' || ch == '\\') o += '\\';
        if (ch == '\n') { o += "\\n"; continue; }
        o += ch;
      }
      return o;
    };
    doc += ",\n  \"mode\": \"verify\",\n  \"shots\": [";
    for (size_t i = 0; i < g_shotResults.size(); i++)
      doc += std::string(i ? ", " : "") + "{\"file\": \"" + esc(g_shotResults[i].file) +
             "\", \"ok\": " + (g_shotResults[i].ok ? "true" : "false") + "}";
    doc += "],\n  \"budget\": [";
    for (size_t i = 0; i < rows.size(); i++) {
      char num[96];
      std::snprintf(num, sizeof(num), "\"gpuP50Ms\": %.3f, \"gpuP95Ms\": %.3f",
                    rows[i].gpuP50Ms, rows[i].gpuP95Ms);
      doc += std::string(i ? ", " : "") + "{\"cam\": \"" + esc(rows[i].cam) +
             "\", \"arm\": \"" + esc(rows[i].arm) +
             "\", \"ok\": " + (rows[i].ok ? "true" : "false") + ", " + num +
             ", \"why\": \"" + esc(rows[i].why) + "\"}";
    }
    doc += "],\n  \"failures\": " + std::to_string(failures) + ",\n";
    doc += PipelineTimingJson("  ");
    doc += "\n}\n";
    std::ofstream out("build/last_run.json");
    if (out) { out << doc; std::printf("wrote build/last_run.json (gates + shots + budget)\n"); }
    else std::fprintf(stderr, "--verify: cannot write build/last_run.json\n");
  }

  std::printf("\n=== verify %s (%d failure(s), %.1fs) ===\n",
              failures == 0 ? "PASS" : "FAIL", failures, NowSeconds() - t0);
  return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
  InstallCrashHandler();
  StartupMark("main");
  // The last mark a run prints: after every local in main() is gone and
  // before static destructors. The gap from the previous mark is the cost of
  // tearing the engine down.
  std::atexit([] { StartupMark("atexit: all of main()'s locals destroyed"); });

  // --crash-test: fault on purpose, so the crash REPORTER is verifiable.
  // The handler is the one piece of code whose correctness cannot be observed
  // during normal operation — it only ever runs when something else has already
  // gone wrong, which is the worst moment to discover that its stack walk is
  // broken. Six real dumps on 2026-08-27 were unusable and nobody knew until
  // they were needed. One flag, no GPU, no window, ~0.2 s: run it after any
  // edit to crash.cpp and read crash.log.
  //
  // Deliberately a NULL READ, matching the historical 0xC0000005 signature, so
  // what the log prints here is directly comparable to a real dump.
  // `--crash-test[=null|abort|throw]` picks WHICH fatal path to take, because
  // they reach the reporter through four different mechanisms and only the
  // first is an SEH exception. abort() in particular is the page pool's
  // documented exhaustion path, and it used to write nothing at all.
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("--crash-test", 0) != 0) continue;
    const std::string kind =
        a.size() > 12 && a[12] == '=' ? a.substr(13) : "null";
    std::fprintf(stderr, "--crash-test=%s: failing on purpose\n", kind.c_str());
    if (kind == "abort") {
      std::fprintf(stderr, "FATAL: pretend page pool exhausted\n");
      std::abort();
    }
    if (kind == "throw") throw std::runtime_error("crash-test uncaught throw");
    volatile int* p = nullptr;
    return *p;
  }

  bool selftest = false;
  bool shot = false;
  bool measure = false;  // --measure: Vulkan-port sizing harness (headless)
  // --perf: the engine performance suite behind the tuner's Performance tab
  // (src/measure/perfsuite.cpp). Headless, like --measure, and for the same
  // reason: a windowed run measures the compositor as much as the engine.
  bool perf = false;
  bool renderBudget = false;
  // --shader-stats: what the DRIVER says about each compiled shader (register
  // count, spilled bytes, occupancy) via VK_KHR_pipeline_executable_properties.
  // Headless and one-shot: it builds every pipeline once, prints, and exits.
  // The renderer's cost model has been reasoning about register footprint from
  // WGSL shape alone; this reads the number instead.
  bool shaderStats = false;
  sandvox::PerfOptions perfOpt;
  bool rebaseline = false;  // --rebaseline: write observed values into baseline.json
  bool suiteAcceptance = false;  // --suite acceptance: one-process full acceptance
  // --verify <gates>: one boot for the end-of-package check — those gates,
  // then the --shot-frames, then the --budget-arms, all into
  // build/last_run.json. See RunVerify.
  bool verify = false;
  bool shotFrames = false;   // --shot-frames given (implies --shot on its own)
  bool budgetArms = false;   // --budget-arms given (implies --render-budget)
  std::string sweepParam;   // --sweep sim.X=a,b,c
  std::string sweepGate;    // --sweep-gate (default: determinism)
  // PAGED IS THE DEFAULT (2026-08-23, user decision): 4,975 resident pages =
  // 77.7 MiB against 512 MiB dense, both residency suites green at the phase-7
  // close. `--residency dense` stays available as the identity map and the
  // only live differential oracle — if paged ever misbehaves, the first
  // diagnostic step is the same scenario under dense.
  bool residencyPaged = true;  // --residency paged|dense
  // --present fifo|mailbox|immediate pins the swapchain present mode for the
  // run; -1 = follow render.presentMode in tuning.json (F5-live).
  int presentOverride = -1;
  bool vkSmoke = false;  // --vk-smoke: cross-backend world-hash comparison (headless)
  // --vk-smoke-loud: phase 3c's determinism acceptance evidence — the same
  // comparison over an ACTIVE world (ops, explosions, particles, readback ring,
  // streaming) rather than a quiet one.
  bool vkSmokeLoud = false;
  // The backend. VULKAN IS THE ONLY ONE since 2026-08-22 (user decision;
  // docs/PLAN_vulkan_port.md phase 6 decision log): Dawn was removed after it
  // ran all 23 gates with results identical to Vulkan's for a full phase. The
  // variable stays because the rhi:: seam stays.
  rhi::BackendKind backend = rhi::BackendKind::Vulkan;
  bool sledgehammer = false;  // --barriers=sledgehammer (barrier_graph §6.2 oracle)
  bool vkValidation = false;  // --vk-validation: VK_LAYER_KHRONOS_validation + sync
  bool lowPowerAdapter = false;
  bool noAudio = false;  // --noaudio: run silent (also implied by every headless mode)
  bool telemetryEnabled = false;
  uint16_t telemetryPort = 8080;
  std::string shotMob;  // --shot-mob <def>[:limb|+item[#RRGGBB],...][@x,z] (pose/wardrobe look)
  // --shot-strike <attacker>[:limb,...][+item,...] <style> [<target>[...]]:
  // the IMPACT look-iteration harness. Three words rather than one colon-
  // separated string because two of them are creature specs that already use
  // ':' and '+', and a fourth separator on top of those is unreadable.
  std::string shotStrikeA, shotStrikeStyle, shotStrikeB;
  // Ticks simulated after the stroke ends, before the last --shot-strike
  // picture. 20 is what a blow needs; an infection needs hundreds.
  int shotStrikeTail = 20;
  bool shotFluid = false;  // --shot-fluid (MPM water look iteration)
  // --shot-waterfall: the CA falling-column fixture. Its own flag rather
  // than a frame inside --shot because it BUILDS a scene (a terrace, a
  // spout and a plunge basin) and pours for 300 ticks, and paying that on
  // every look-iteration run of the other forty frames is the wrong trade.
  bool shotWaterfall = false;
  // --shot-debris-pond: the buoyancy fixture (docs/PLAN_debris_buoyancy.md).
  // Its own flag for the same reason the waterfall has one: it moves the
  // residency window onto a generated pond, regenerates the world and runs
  // 450 ticks, which is not a cost the other forty look frames should pay.
  bool shotDebrisPond = false;
  // --shot-fluid-pond: the same harness aimed into a generated pond, so the
  // MPM isosurface has to share the frame with deep SETTLED water. Separate
  // process rather than an extra block in --shot-fluid because the scene moves
  // the residency window and regenerates the world.
  bool shotFluidPond = false;
  // The fluid lab (docs/PLAN_fluid_overhaul.md §4): `--lab [scene]` runs the
  // windowed game on the flat-slab lab world with the named scripted scene
  // (default basin); `--fluid-bench [scene|hill0|all]` is the headless timing
  // harness. Both flip World::SetLabWorld — the worldgen mode tap.
  bool labFlag = false;
  std::string labSceneName = "basin";
  bool fluidBench = false;
  std::string fluidBenchScene = "all";
  // --heightmap cx,cz,span,res[,seed] — the tuner's terrain map. See the
  // dispatch below for why this is a GPU-free early exit.
  std::string heightmapArgs;
  std::string heightmapOut = "build/heightmap.bin";
  // --mapcheck <name> — load one map exactly as the game would and print its
  // load warnings / refusal as JSON (the World map page's warnings panel).
  std::string mapcheckName;
  // --voxdump ox,oy,oz,nx,ny,nz,lod[,seed] / --voxserve (tools/voxregion.h)
  std::string voxdumpArgs;
  std::string voxdumpOut = "build/voxregion.bin";
  bool voxserve = false;
  // --export-edits <saveDir>,<layerName>[,seed] (tools/voxregion.h, map-overhaul P7)
  std::string exportEdits;
  // ---- THE OP RECORD, ON THE COMMAND LINE (PLAN_multiplayer_now N3/N5) ----
  //
  // N3 built the recorder but could not wire these two flags: main.cpp was
  // claimed, so recording was reachable only through SANDVOX_RECORD_OPS and
  // only from `selftest::Run`. That left the one path the record exists FOR —
  // the game's own frame loop — unrecordable, and it is the path whose
  // per-tick input a network layer will have to carry.
  //
  // `--record-ops <file>` arms the recorder for whatever mode this invocation
  // runs (the windowed game, `--frames`, `--sweep`, `--selftest`); it is the
  // env var with an argv door, and it wins over the env var when both are set.
  // `--replay-ops <file>` is a MODE: no window, no player, no frame loop — it
  // loads the record, boots worldgen from the seed in the header and feeds
  // SubmitTick the recorded frames, which is the `ops-replay` gate's drive
  // loop pointed at a file the game wrote instead of a scene the gate scripted.
  std::string recordOpsPath;
  std::string replayOpsPath;
  // ---- M9.2-C: --host [port] / --join <ip[:port]> ------------------------
  // Two exes, one world. --host is a LISTEN SERVER: it opens the port at boot
  // and keeps playing single-player until somebody connects, so a host is a
  // normal game that happens to be joinable. --join BLOCKS at boot until the
  // handshake answers, because there is nothing sensible to do with a world
  // the host may be about to refuse (a different tuning.json, a different
  // seed) — see the boot block below "THE GAME TALKS TO ONE PEER".
  //
  // netPort 0 with hosting on means "the OS picks", which TcpLink::Listen
  // supports and ListenPort() reads back; the default is 7777.
  bool netHost = false;
  std::string netJoinIp;
  uint16_t netPort = 7777;
  // ---- M9.5-B: THE WORLD DIRECTORY IS NO LONGER A LITERAL ---------------
  //
  // Saving was UI-only and hardcoded to "world.svd" (F9/F10 and the dev
  // panel). The M9.5-B smoke needs a THIRD process to load what the host
  // saved, and a harness that has to press F10 is not a harness — so the
  // directory is a variable and `--load-world <dir>` is the UI load path's
  // twin: it sets `ui.loadWorld` before the first frame and the existing
  // handler does every other line of the work. Deliberately NOT a second
  // load implementation; the one at the F10 site is the only one.
  std::string worldDir = "world.svd";
  bool loadWorldAtBoot = false;
  // ---- M9.5-B: --net-smoke-persist --------------------------------------
  //
  // The host half of the two-process smoke's acceptance. Set, the host saves
  // the world to `build/smoke_world.svd` at the moment its peer disconnects
  // (which under SANDVOX_NET_SMOKE_EXIT_ON_PEER_DONE is also the moment it
  // decides to exit) and prints the store's chunk count and how many of them
  // carry a non-zero tick tag. Those two numbers are the claim: a tag is only
  // written by an authority's eviction or an accepted `ChunkPut`, so "N
  // tagged" is exactly the shared world state that survived, and the third
  // launch's `--load-world` must report the same N.
  //
  // A FLAG AND NOT AN ENV VAR, unlike the other five smoke switches, because
  // it takes an ACTION (a save, to a path) rather than modifying behaviour
  // the game already has — and because a stray save over somebody's world
  // directory is the kind of thing that should be visible in the command
  // line that caused it.
  bool netSmokePersist = false;
  uint32_t replayTicks = 0;  // --ticks N: stop the replay after N frames
  selftest::Options stOpt;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      std::printf(
          "sandvox — 3D falling-sand voxel engine\n\n"
          "Usage: sandvox [options]\n\n"
          "Selftest:\n"
          "  --selftest            Run the headless selftest suite\n"
          "  --gate <name>         Run one selftest gate (repeatable)\n"
          "  --list                List available selftest gates\n"
          "  --json <path>         Write selftest results as JSON\n"
          "  --baseline <path>     Selftest baseline file\n"
          "  --rebaseline          Write observed values into baseline.json\n"
          "  --suite acceptance    One-process selftest + both smokes + validation\n"
          "  --verify a,b[,..]     ONE boot: those gates (or 'none'), then --shot-frames,\n"
          "                        then --budget-arms; all recorded in build/last_run.json\n"
          "  --shot-frames x,y     Only these --shot frames (names, .bmp optional)\n"
          "  --budget-arms p,q     Only these --render-budget arms (e.g. baseline,noshadow)\n"
          "  --sweep sim.X=a,b,c   In-process parameter sweep (hash per value)\n"
          "  --sweep-gate <name>   Gate for --sweep (default: determinism)\n\n"
          "Op record (sim/oprecord.h — the MutationQueue as a replay log):\n"
          "  --record-ops <file>   Record every tick this run submits\n"
          "  --replay-ops <file>   Replay a record headlessly, hash every 15 ticks\n"
          "  --ticks <N>           Stop --replay-ops after N recorded ticks\n\n"
          "Shot / screenshot modes:\n"
          "  --shot                Screenshot-only look iteration\n"
          "  --shot-fluid          MPM fluid screenshot mode\n"
          "  --shot-waterfall      Waterfall mist/spray fixture (CA liquid)\n"
          "  --shot-debris-pond    Blow a wooden mass apart over a generated\n"
          "                        pond: does the debris sink, float or hang?\n"
          "  --shot-fluid-pond     MPM fluid poured into a generated pond\n"
          "                        (the MPM/settled-water seam)\n"
          "  --shot-mob <def>      Mob pose look iteration (def[:limb,...])\n"
          "  --shot-strike <attacker> <style> [<target>]\n"
          "                        One blow, photographed on its own phases and\n"
          "                        counted per rig slot. Each creature is\n"
          "                        <def>[@gap][:limb,...][+item,...], e.g.\n"
          "                        --shot-strike zombie bite_lunge human+iron_cuirass\n"
          "  --shot-devpanel       One BMP per F1 sidebar page\n"
          "  --shot-dialogue       The conversation panel: screenshot_dialogue.bmp\n"
          "                        (entry node) and _2.bmp (the choice rows)\n"
          "  --shot-inventory      Character screen (I) with a damaged avatar,\n"
          "                        one frame to screenshot_inventory.bmp\n"
          "  --shot-spellpage      The spell page, one BMP per word list:\n"
          "                        shot_spell_<scene>.bmp (the look gallery)\n"
          "  --shot-jump           Airborne pose look iteration: third person,\n"
          "                        one BMP per phase of a jump (rise/apex/\n"
          "                        fall/land), triggered on the pose's own vy\n"
          "  --time <0..1>         Time of day for --shot (0=midnight, 0.5=noon)\n\n"
          "Fluid lab:\n"
          "  --lab [scene]         Windowed fluid lab (basin|hill|faucet|pool|slosh|pond|worldlake)\n"
          "  --fluid-bench [scene] Headless fluid timing harness (scene|pond<N>|pours|all)\n\n"
          "Harness / perf:\n"
          "  --duel-dummy          Spawn a sword-armed human 3 m ahead (melee feel)\n"
          "  --frames <N>          Run windowed game for N frames then exit\n"
          "  --autofly             Enable autofly camera\n"
          "  --autofly-hard        Adversarial autofly (diagonal + descent)\n"
          "  --autofly-surface     Surface-following autofly\n"
          "  --autowalk            Walk on foot: forward held, hop /45 ticks, quarter turn /240\n"
          "  --autofly-park        Surface autofly that stops (sleep discriminator)\n"
          "  --measure             Vulkan sizing harness (occupancy + GPU timings)\n"
          "  --perf                Performance suite -> build/perf.json (tuner Performance tab)\n"
          "  --perf-list           List the --perf scenarios and exit\n"
          "  --scenario <id>       One --perf scenario (idle|treeburn|forestfire|flythrough|explosion|water)\n"
          "  --perf-out <path>     Where --perf writes its JSON\n"
          "  --perf-w/--perf-h <n> Offscreen render size for --perf/--render-budget\n"
          "  --render-budget       Where INSIDE the raymarch the GPU frame went\n"
          "  --budget-cams <list>  --render-budget cameras (noon,dusk,cascade,submerged,meadow,canopy,fire,seam,seamveg; default all)\n"
          "  --shader-stats        Per-shader registers/spills from the driver\n"
          "                        -> build/shader_stats.json (headless)\n\n"
          "Residency:\n"
          "  --residency paged|dense  Voxel buffer residency mode (default: paged)\n\n"
          "Vulkan / debug:\n"
          "  --backend vulkan      Explicitly name the Vulkan backend\n"
          "  --vk-smoke            Quiet 50-tick pinned hash comparison\n"
          "  --vk-smoke-loud       Active 120-tick hash comparison (19 probes)\n"
          "  --vk-validation       Enable VK_LAYER_KHRONOS_validation + sync\n"
          "  --barriers=sledgehammer  Full barrier oracle (§6.2)\n"
          "  --barriers=precise    Precise barriers (default)\n\n"
          "Multiplayer (M9.2 — two players, one world):\n"
          "  --host [port]         Listen for one peer (default 7777); play meanwhile\n"
          "  --load-world <dir>    Load a saved world directory at boot (F10's twin)\n"
          "  --net-smoke-persist   Host: save to build/smoke_world.svd when the peer quits\n"
          "  --join <ip[:port]>    Connect to a host and share its tick clock\n\n"
          "Misc:\n"
          "  --adapter low         Select low-power (iGPU) adapter\n"
          "  --noaudio             Disable audio\n"
          "  --telemetry           Enable telemetry\n"
          "  --telemetry-port <N>  Telemetry port (default 8080)\n"
          "  --help, -h            Show this help\n");
      return 0;
    }
    if (a == "--selftest") selftest = true;
    // `--gate <name>` (repeatable) runs ONE gate plus whatever it declares as a
    // dependency, in seconds rather than the ~70 s full run. `--list` names
    // them. This is what makes "is this failure mine?" a cheap question — see
    // the header comment in test/selftest.h.
    else if (a == "--gate") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--gate requires a gate name\n"); return 1; }
      selftest = true;
      stOpt.only.push_back(argv[++i]);
    }
    else if (a == "--list") {
      selftest = true;
      stOpt.list = true;
    }
    else if (a == "--json") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--json requires a path\n"); return 1; }
      stOpt.jsonPath = argv[++i];
    }
    else if (a == "--baseline") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--baseline requires a path\n"); return 1; }
      stOpt.baselinePath = argv[++i];
    }
    else if (a == "--shot") shot = true;
    else if (a == "--shot-fluid") shotFluid = true;
    else if (a == "--shot-waterfall") shotWaterfall = true;
    else if (a == "--shot-debris-pond") shotDebrisPond = true;
    else if (a == "--shot-fluid-pond") shotFluidPond = true;
    // Fluid lab modes. The scene argument is optional (it must not start
    // with '-' or it is the next flag).
    else if (a == "--lab") {
      labFlag = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') labSceneName = argv[++i];
    }
    else if (a == "--fluid-bench") {
      fluidBench = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') fluidBenchScene = argv[++i];
    }
    // `--frames N` runs the WINDOWED game for N frames, fires one F5 shader
    // reload midway, and exits cleanly — the phase-4b D3 verification harness.
    else if (a == "--frames") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--frames requires a count\n"); return 1; }
      g_harnessFrames = (uint64_t)std::atoll(argv[++i]);
    }
    // ---- MULTIPLAYER (docs/PLAN_multiplayer_m9.md M9.2) -------------------
    // `--host` takes an OPTIONAL port, so the `argv[i+1][0] != '-'` guard is
    // the same one --lab and --fluid-bench use: `--host --frames 900` has to
    // mean "default port", not "port --frames".
    else if (a == "--host") {
      netHost = true;
      if (i + 1 < argc && argv[i + 1][0] != '-')
        netPort = (uint16_t)std::atoi(argv[++i]);
    }
    // `--join <ip>` or `--join <ip>:<port>`. The port rides in the one
    // argument because that is how a person copies an address out of a chat
    // window; a separate --join-port would be a second thing to get wrong.
    else if (a == "--join") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--join requires <ip[:port]>\n"); return 1; }
      std::string addr = argv[++i];
      const size_t colon = addr.rfind(':');
      if (colon != std::string::npos) {
        netPort = (uint16_t)std::atoi(addr.c_str() + colon + 1);
        addr.resize(colon);
      }
      netJoinIp = addr;
    }
    // M9.5-B. `--load-world <dir>` is the F10 path with the directory named
    // on the command line; `--net-smoke-persist` is the host's save-on-
    // disconnect (see their declarations above).
    else if (a == "--load-world") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "--load-world requires a directory\n");
        return 1;
      }
      worldDir = argv[++i];
      loadWorldAtBoot = true;
    } else if (a == "--net-smoke-persist") {
      netSmokePersist = true;
    }
    // `--shot-inventory` is the character screen's look-iteration harness: run
    // the windowed game, spawn and damage the avatar on a fixed schedule, open
    // the screen, and write ONE frame out as a BMP. See the note at
    // g_shotInventory.
    else if (a == "--shot-bench") {
      g_shotBench = true;
      // SANDVOX_BENCH_INVERT runs a longer script (the lift, the turn, the pour).
      g_harnessFrames = kShotBenchLast + (std::getenv("SANDVOX_BENCH_INVERT") ? 160 : 0);
    }
    else if (a == "--shot-devpanel") {
      g_shotDevPanel = true;
      g_harnessFrames = kShotDevLast + 2;
    }
    else if (a == "--shot-dialogue") {
      g_shotDialogue = true;
      g_harnessFrames = kShotDlgLast;
    }
    else if (a == "--shot-inventory") {
      g_shotInventory = true;
      g_harnessFrames = kShotInvDeathTurnShot;
    }
    // `--shot-spellpage` is the SPELL PAGE's gallery: one BMP per word list,
    // all of them on the same canvas. See the note at g_shotSpellPage.
    else if (a == "--shot-spellpage") {
      g_shotSpellPage = true;
      g_harnessFrames = kShotSpellLast;
    }
    // `--shot-jump` is the AIRBORNE POSE's look-iteration harness: walk the
    // avatar in third person, jump it, and write one picture per phase of the
    // arc. See the note at g_shotJump for why the captures are triggered by
    // the pose's own vel.y rather than by a frame number.
    else if (a == "--shot-jump") {
      g_shotJump = true;
      g_harnessFrames = kShotJumpLastFrame;
    }
    // `--duel-dummy` is the melee FEEL harness: a sword-armed human standing
    // three metres in front of the spawn, with nothing driving it. Mobs have no
    // AI yet, so "stands still" is simply the default and this flag is only a
    // spawn — the point is to have something at a known distance to cut, so the
    // stroke can be judged against a body rather than against the sky. Phase B's
    // AI/spawn panel supersedes it; keep the footprint here at one bool.
    else if (a == "--duel-dummy") g_duelDummy = true;
    else if (a == "--forest-fire") {
      g_forestFire = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') g_forestFireAt = std::atoi(argv[++i]);
    }
    else if (a == "--fell-tree") {
      g_fellTree = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') g_fellTreeAt = std::atoi(argv[++i]);
      if (i + 1 < argc && argv[i + 1][0] != '-' &&
          std::sscanf(argv[i + 1], "%d,%d", &g_fellSiteX, &g_fellSiteZ) == 2) {
        g_fellSiteSet = true;
        i++;
      }
    }
    // `--short-range` is the headless handle on the dev panel's short-range
    // row: no offscreen path can press a radio button, and the whole point of
    // the mode is a LOOK and a frame time to compare, both of which are
    // captured by --shot / --render-budget. Equivalent to
    // SANDVOX_SHORT_RANGE=1. `--short-range-near` picks the tighter arm
    // (render.shortRangeNearDist, 50 m by default) and implies the mode, so
    // the near arm is one flag rather than two that must be given together.
    // In the windowed game both only set the row's starting position — the
    // panel stays the live authority from frame 1.
    else if (a == "--short-range") SetShortRange(true);
    else if (a == "--short-range-near") { SetShortRange(true); SetShortRangeNear(true); }
    else if (a == "--autofly") g_autofly = true;
    else if (a == "--autowalk") g_autoWalk = true;
    else if (a == "--autofly-hard") { g_autofly = true; g_autoflyHard = true; }
    else if (a == "--autofly-surface") { g_autofly = true; g_autoflySurface = true; }
    // `--autofly-park` is --autofly-surface that STOPS (see ParkProbe): the
    // sleep-vs-transient discriminator for the active-chunk count.
    else if (a == "--autofly-park") {
      g_autofly = true;
      g_autoflySurface = true;
      g_autoflyPark = true;
    }
    // `--measure` is the Vulkan-port sizing harness (src/measure/measure.cpp):
    // occupancy histogram of the residency window + per-compute-pass GPU
    // timings. Headless, off by default, and the ONLY thing that requests the
    // TimestampQuery device feature.
    else if (a == "--measure") measure = true;
    else if (a == "--perf") perf = true;
    else if (a == "--perf-list") { perf = true; perfOpt.list = true; }
    // `--render-budget` reuses --perf's scenario cameras and --perf-w/h, so it
    // shares perfOpt and is dispatched AHEAD of --perf below: `--render-budget
    // --scenario water` must budget the water camera, not run the water
    // scenario.
    else if (a == "--render-budget") renderBudget = true;
    // `--shader-stats` must arm capture BEFORE Simulation::Init builds any
    // pipeline (CAPTURE_STATISTICS is a create flag), which is why it is a
    // plain bool read down where the device is made, not a runner argument.
    else if (a == "--shader-stats") shaderStats = true;
    else if (a == "--perf-w") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--perf-w requires a width\n"); return 1; }
      perfOpt.width = (uint32_t)std::atoi(argv[++i]);
    }
    else if (a == "--perf-h") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--perf-h requires a height\n"); return 1; }
      perfOpt.height = (uint32_t)std::atoi(argv[++i]);
    }
    else if (a == "--scenario") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--scenario requires a scenario id\n"); return 1; }
      perfOpt.only = argv[++i];
      perf = true;
    }
    // `--budget-cams noon,dusk,cascade,submerged,meadow,canopy` picks which of
    // --render-budget's cameras run (default: all six). It does NOT imply --render-budget —
    // unlike --scenario, which has to imply --perf because that is the only
    // harness it means anything to. This one is a modifier on a mode you
    // already asked for, and silently turning a 3-camera budget on because
    // somebody named a camera would be a surprise.
    else if (a == "--budget-cams") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--budget-cams requires a comma-separated camera list\n"); return 1; }
      perfOpt.cams = argv[++i];
    }
    else if (a == "--perf-out") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--perf-out requires a path\n"); return 1; }
      perfOpt.out = argv[++i];
    }
    // `--residency paged|dense` selects the voxel buffer's residency
    // (docs/PLAN_page_table.md §6.2). Paged is the default; dense is the
    // identity-map oracle. ONE variable with a total order of values rather
    // than two flags, per the phase-6 lesson that a flag named for the
    // non-default cannot express a default flip.
    //
    // `dense` is the identity map: address-identical to pre-paging code while
    // still running the whole translation path. With Dawn gone it is the ONLY
    // live differential oracle the engine has, which makes it load-bearing
    // test infrastructure rather than a fallback — never selected
    // automatically, always available (§6.3).
    else if (a == "--residency") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--residency requires paged|dense\n"); return 1; }
      const std::string v = argv[++i];
      if (v == "paged") residencyPaged = true;
      else if (v == "dense") residencyPaged = false;
      else { std::fprintf(stderr, "--residency wants paged|dense, got '%s'\n",
                          v.c_str()); return 1; }
    }
    else if (a == "--present") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--present requires fifo|mailbox|immediate\n"); return 1; }
      const std::string v = argv[++i];
      if (v == "fifo") presentOverride = 0;
      else if (v == "mailbox") presentOverride = 1;
      else if (v == "immediate") presentOverride = 2;
      else { std::fprintf(stderr, "--present wants fifo|mailbox|immediate, got '%s'\n",
                          v.c_str()); return 1; }
    }
    else if (a == "--shot-mob") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--shot-mob requires a mob def\n"); return 1; }
      shotMob = argv[++i];
    }
    else if (a == "--shot-strike") {
      if (i + 2 >= argc) {
        std::fprintf(stderr,
                     "--shot-strike wants <attacker>[@gap][:limb,...][+item,...] "
                     "<style> [<target>[:limb,...][+item,...]]\n"
                     "  e.g. --shot-strike zombie bite_lunge human+iron_cuirass\n");
        return 1;
      }
      shotStrikeA = argv[++i];
      shotStrikeStyle = argv[++i];
      // The target is optional, so it is only taken when the next word is not
      // another flag — otherwise `--shot-strike human punch_r --vk-validation`
      // would eat the flag as a creature and report "no mob def named
      // --vk-validation", which is a confusing way to say "you left it out".
      if (i + 1 < argc && argv[i + 1][0] != '-') shotStrikeB = argv[++i];
    }
    else if (a == "--shot-strike-tail") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "--shot-strike-tail wants <ticks>\n");
        return 1;
      }
      shotStrikeTail = std::max(0, atoi(argv[++i]));
    }
    else if (a == "--noaudio") noAudio = true;
    else if (a == "--telemetry") telemetryEnabled = true;
    else if (a == "--telemetry-port") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--telemetry-port requires a port number\n"); return 1; }
      telemetryPort = (uint16_t)std::atoi(argv[++i]);
    }
    // `--time 0..1` sets the time of day for --shot: 0 = midnight, 0.25 =
    // sunrise, 0.5 = noon, 0.75 = sunset. Lets the sky be judged at any point
    // in the cycle without waiting for it.
    else if (a == "--time") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--time requires a value (0..1)\n"); return 1; }
      g_shotTimeOfDay = std::fmod(std::atof(argv[++i]), 1.0);
      if (g_shotTimeOfDay < 0.0f) g_shotTimeOfDay += 1.0f;
    }
    // `--adapter low` picks the LowPower adapter (iGPU) so the selftest hash
    // can be compared across GPU vendors (DESIGN.md §14 risk 3).
    else if (a == "--adapter") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--adapter requires a value\n"); return 1; }
      lowPowerAdapter = std::string(argv[++i]) == "low";
    }
    // `--vk-smoke` runs a quiet 50-tick world and compares its hashes against
    // the PINNED sequence (src/gpu/vk_smoke.cpp). It used to compare Dawn
    // against Vulkan; with Dawn gone the pinned values ARE the reference, so
    // the regression power the cross-backend diff provided is preserved.
    else if (a == "--vk-smoke") vkSmoke = true;
    // `--vk-smoke-loud` does the same over 120 ticks of an ACTIVE world,
    // reaching everything a quiet world leaves dark — the brush/cell mutation
    // kernels, the explosion mark/apply split, the whole particle chain, the
    // readback ring, and a streaming walk that forces eviction and procgen
    // refill. 19 pinned probes.
    else if (a == "--vk-smoke-loud") vkSmokeLoud = true;
    // `--backend vulkan` names the only backend explicitly, so existing
    // invocations and scripts keep working. `--backend dawn` is REFUSED with
    // an explanation rather than quietly served by Vulkan: a run reported as
    // Dawn that was Vulkan all along is worse than no run — the same principle
    // that made phase 3b refuse `--backend vulkan` before it could honour it.
    else if (a == "--backend") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--backend requires a value\n"); return 2; }
      std::string b = argv[++i];
      if (b == "vulkan") {
        backend = rhi::BackendKind::Vulkan;
      } else if (b == "dawn") {
        std::fprintf(stderr,
                     "--backend dawn: Dawn was REMOVED 2026-08-22 and the engine is\n"
                     "Vulkan-only (docs/PLAN_vulkan_port.md phase 6 decision log).\n"
                     "Drop the flag, or pass --backend vulkan.\n");
        return 2;
      } else {
        std::fprintf(stderr, "unknown --backend '%s' (expected vulkan)\n", b.c_str());
        return 2;
      }
    }
    // `--barriers=sledgehammer` is the A/B oracle of barrier_graph §6.2: every
    // command preceded by a full ALL_COMMANDS/MEMORY_READ|WRITE barrier, i.e.
    // maximally-ordered execution of the same total order. Read §6.2 before
    // trusting a green run — it is WEAK at detecting a missing barrier and
    // STRONG at exonerating the barrier graph.
    else if (a == "--barriers=sledgehammer") sledgehammer = true;
    else if (a == "--barriers=precise") sledgehammer = false;
    // Turns on VK_LAYER_KHRONOS_validation with SYNCHRONIZATION validation —
    // the primary detector for a missing barrier (§6.2's detection ladder).
    else if (a == "--vk-validation") vkValidation = true;
    else if (a == "--rebaseline") rebaseline = true;
    // `--verify a,b,c` — the gates; `--shot-frames x,y` and `--budget-arms p,q`
    // add the frames and the arms to the same boot. Each of the last two also
    // works alone, as a filter on --shot / --render-budget.
    else if (a == "--verify") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--verify requires a gate list (or 'none')\n"); return 1; }
      verify = true;
      std::string list = argv[++i];
      if (list != "none")
        for (size_t p = 0; p < list.size();) {
          size_t q = list.find(',', p);
          if (q == std::string::npos) q = list.size();
          if (q > p) stOpt.only.push_back(list.substr(p, q - p));
          p = q + 1;
        }
    }
    else if (a == "--shot-frames") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--shot-frames requires a frame list\n"); return 1; }
      shotFrames = true;
      std::string list = argv[++i];
      for (size_t p = 0; p < list.size();) {
        size_t q = list.find(',', p);
        if (q == std::string::npos) q = list.size();
        if (q > p) {
          std::string f = list.substr(p, q - p);
          if (f.size() > 4 && f.compare(f.size() - 4, 4, ".bmp") == 0) f.resize(f.size() - 4);
          g_shotOnly.push_back(f);
        }
        p = q + 1;
      }
    }
    else if (a == "--budget-arms") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--budget-arms requires an arm list\n"); return 1; }
      budgetArms = true;
      std::string list = argv[++i];
      for (size_t p = 0; p < list.size();) {
        size_t q = list.find(',', p);
        if (q == std::string::npos) q = list.size();
        if (q > p) perfOpt.arms.push_back(list.substr(p, q - p));
        p = q + 1;
      }
    }
    else if (a == "--suite") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--suite requires a name\n"); return 1; }
      std::string s = argv[++i];
      if (s == "acceptance") suiteAcceptance = true;
      else { std::fprintf(stderr, "--suite wants 'acceptance', got '%s'\n", s.c_str()); return 1; }
    }
    else if (a == "--sweep") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--sweep requires param=val1,val2,...\n"); return 1; }
      sweepParam = argv[++i];
      selftest = true;
    }
    else if (a == "--sweep-gate") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--sweep-gate requires a gate name\n"); return 1; }
      sweepGate = argv[++i];
    }
    else if (a == "--heightmap") {
      if (i + 1 >= argc) {
        std::fprintf(stderr,
                     "--heightmap wants cx,cz,span,res[,seed]\n");
        return 1;
      }
      heightmapArgs = argv[++i];
    }
    else if (a == "--heightmap-out") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--heightmap-out wants a path\n"); return 1; }
      heightmapOut = argv[++i];
    }
    else if (a == "--mapcheck") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--mapcheck wants a map name\n"); return 1; }
      mapcheckName = argv[++i];
    }
    // --voxdump / --voxserve: REAL VOXELS for the tuner's terrain viewer.
    // Unlike --heightmap these need a GPU (genCellIn is WGSL), so they answer
    // after device init — see tools/voxregion.h for why one is a server.
    else if (a == "--voxdump") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "--voxdump wants ox,oy,oz,nx,ny,nz,lod[,seed]\n");
        return 1;
      }
      voxdumpArgs = argv[++i];
    }
    else if (a == "--voxdump-out") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--voxdump-out wants a path\n"); return 1; }
      voxdumpOut = argv[++i];
    }
    else if (a == "--voxserve") voxserve = true;
    else if (a == "--export-edits") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "--export-edits wants <saveDir>,<layerName>[,seed]\n");
        return 1;
      }
      exportEdits = argv[++i];
    }
    else if (a == "--record-ops") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--record-ops requires a path\n"); return 1; }
      recordOpsPath = argv[++i];
    }
    else if (a == "--replay-ops") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--replay-ops requires a path\n"); return 1; }
      replayOpsPath = argv[++i];
    }
    else if (a == "--ticks") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--ticks requires a count\n"); return 1; }
      replayTicks = (uint32_t)std::max(0, std::atoi(argv[++i]));
    }
    else {
      std::fprintf(stderr, "unrecognized argument: '%s'\n"
                           "Run with --help for usage.\n", a.c_str());
      return 3;
    }
  }

  // Fluid-lab world mode, set ONCE before anything reads terrain: gates every
  // TickParams.labMode write and the CPU TerrainHeight mirror together.
  // --selftest and the smokes never pass through here with it set, so the
  // pinned hash 7cfa2420 stays a labMode=0 fact.
  const int labScene = labFlag ? LabSceneFromName(labSceneName) : -1;
  if (labFlag && labScene < 0) {
    std::fprintf(stderr, "--lab: unknown scene '%s' (want basin|hill|faucet|"
                 "pool|slosh|pond|worldlake)\n", labSceneName.c_str());
    return 1;
  }
  // `worldlake` is the one lab scene that runs on the REAL worldgen — it is
  // the main-world arm of the pond measurement and its water is worldgen's
  // authored lake, not a scripted box. --fluid-bench re-decides this per run
  // (it may mix lab and world scenes in one invocation); this is the windowed
  // path's answer.
  if (labFlag) World::SetLabWorld(LabSceneUsesLabWorld(labScene));
  else if (fluidBench) World::SetLabWorld(true);

  // THE HARNESS MAP (sim/worlddefaults.h). Every mode whose fixtures are
  // written against known ground -- the gates, the smokes, the fluid benches'
  // `worldlake`, the --shot scenes -- generates assets/worldmap/harness, not
  // the game's map, so repainting the shipped map can never move a gate.
  // The game, --perf / --render-budget / --measure (which measure the game)
  // and the tuner's tools keep world.mapLayer. SANDVOX_MAP still wins.
  if (selftest || verify || suiteAcceptance || vkSmoke || vkSmokeLoud ||
      fluidBench || labFlag || shot || shotFrames || shotFluid ||
      shotWaterfall || shotDebrisPond || shotFluidPond)
    worldmap::SetMapOverride(kHarnessMapName);

  // --list is pure metadata: answering it before any device or asset init
  // means an agent can ask "what gates exist" without a GPU or a built world.
  if (stOpt.list) return selftest::List();

  // --heightmap: render a grid of World::TerrainColumn to a file and exit.
  //
  // NO GPU, NO WINDOW — it answers before GpuContext exists, which is what
  // makes it cheap enough for the tuner to call on every redraw. It reads
  // tuning.json (for the map's name), the materials, the biome files and the
  // map FRESH -- since P-G every number that shapes the ground is the map's
  // and the biomes' (map.json `terrain`, the per-biome relief record), so the
  // picture it draws is the world those files describe right now.
  //
  // It is the same World::TerrainHeight the game collides against, on purpose:
  // a JS reimplementation in the tuner would be a third copy of the octave
  // ladder with nothing enforcing it against the other two (see the note over
  // World::Column). The cost is a process launch per map, ~150 ms.
  if (!heightmapArgs.empty()) {
    Tuning tune;
    std::string tuneErrs;
    LoadTuning(AssetDir() + "/materials/tuning.json", tune);
    SetCurrentTuning(tune);
    {
      const std::string ad = AssetDir();
      std::vector<MaterialDef> m;
      std::vector<ReactionGpu> rx;
      std::string errs;
      if (!LoadAssets(ad + "/materials/materials.json", ad + "/materials/reactions.json", m, rx, errs)) {
        std::fprintf(stderr, "--heightmap: asset load failed:\n%s\n", errs.c_str());
        return 1;
      }
      biomes::BiomeSet set;
      worldmap::WorldMapData map;
      std::string blog;
      if (!biomes::LoadBiomeSet(ad, m, set, blog) ||
          !worldmap::LoadWorldMap(ad, worldmap::ActiveMapName(CurrentTuning().world.mapLayer), set, m.size(), kDefaultSeed, map, blog)) {
        std::fprintf(stderr, "--heightmap: %s", blog.c_str());
        return 1;
      }
      worldmap::SetCurrentWorldMap(std::move(map));
    }
    return WriteHeightmap(heightmapArgs, heightmapOut);
  }
  // --mapcheck <name>: the same GPU-free load --heightmap does, of the NAMED
  // map (the page's, not world.mapLayer), answered as one `MAPCHECK {json}`
  // line: the load warnings (unknown site kinds, missing stamps / species,
  // overlapping footprints, ...) or the refusal. Exit 0 either way -- a
  // refused map is an answer, not a crash; the tuner shows it.
  if (!mapcheckName.empty()) {
    Tuning tune;
    LoadTuning(AssetDir() + "/materials/tuning.json", tune);
    SetCurrentTuning(tune);
    const std::string ad = AssetDir();
    std::vector<MaterialDef> m;
    std::vector<ReactionGpu> rx;
    std::string errs;
    biomes::BiomeSet set;
    worldmap::WorldMapData map;
    std::string blog;
    bool ok = mapcheckName.find_first_of("/\\:") == std::string::npos;   // a bare name, never a path
    if (!ok) blog = "not a bare map name";
    ok = ok && LoadAssets(ad + "/materials/materials.json", ad + "/materials/reactions.json", m, rx, errs);
    if (!ok && blog.empty()) blog = "asset load failed: " + errs;
    ok = ok && biomes::LoadBiomeSet(ad, m, set, blog);
    ok = ok && worldmap::LoadWorldMap(ad, mapcheckName, set, m.size(), kDefaultSeed, map, blog);
    std::printf("MAPCHECK %s\n", worldmap::MapCheckJson(mapcheckName, ok, map, ok ? "" : blog).c_str());
    return 0;
  }

  // --suite acceptance: one process, all measurements. The expensive part of a
  // run is Vulkan device creation + SPIR-V compilation + worldgen. This
  // amortizes that cost across selftest + both smokes in one invocation.
  if (suiteAcceptance) {
    double t0 = NowSeconds();
    std::printf("=== sandvox --suite acceptance ===\n");
    int failures = 0;

    // Smokes first (they build their own GpuContext).
    std::printf("\n--- vk-smoke (quiet) ---\n");
    int r1 = sandvox::RunVkSmoke(lowPowerAdapter, sledgehammer, vkValidation,
                                 true, rebaseline);
    if (r1 != 0) failures++;

    std::printf("\n--- vk-smoke-loud ---\n");
    int r2 = sandvox::RunVkSmokeLoud(lowPowerAdapter, sledgehammer, vkValidation,
                                     true, rebaseline);
    if (r2 != 0) failures++;

    // Selftest (paged, the default — builds its own GpuContext further down,
    // but --suite shortcuts past the asset load below). We need to do the same
    // setup the normal selftest path does.
    {
      std::printf("\n--- selftest (paged) ---\n");
      std::string ad = AssetDir();
      Tuning tune;
      LoadTuning(ad + "/materials/tuning.json", tune);
      SetCurrentTuning(tune);
      std::vector<MaterialDef> m;
      std::vector<ReactionGpu> rx;
      std::string errs;
      if (!LoadAssets(ad + "/materials/materials.json",
                      ad + "/materials/reactions.json", m, rx, errs)) {
        std::fprintf(stderr, "asset load failed:\n%s\n", errs.c_str());
        return 1;
      }
      MicroSet mic;
      { std::string ml; LoadMicroVox(ad + "/materials/materials.json", ad, m, mic, ml); }
      // The second world this process builds needs the same trees as the first
      // -- a treeless second world would hash differently for a reason that has
      // nothing to do with what is being tested.
      biomes::BiomeSet stBiomes;
      { std::string bl;
        if (!biomes::LoadBiomeSet(ad, m, stBiomes, bl)) {
          std::fprintf(stderr, "%s", bl.c_str());
          return 1;
        } }
      TreeAtlas stTrees;
      { std::string tl;
        if (!LoadTreeAtlas(ad + "/trees", m, stBiomes, stTrees, tl)) {
          std::fprintf(stderr, "%s", tl.c_str());
          return 1;
        } }
      GpuContext stCtx;
      if (!stCtx.Init(nullptr, 1600, 900, lowPowerAdapter, false, backend,
                      vkValidation, sledgehammer))
        return 1;
      World stWorld;
      stWorld.residency = World::Residency::Paged;
      stWorld.Init(stCtx.device);
      std::vector<uint32_t> stMapWords;
      { std::string wl;
        worldmap::WorldMapData stMap;
        if (!worldmap::LoadWorldMap(ad, worldmap::ActiveMapName(CurrentTuning().world.mapLayer), stBiomes, m.size(), kDefaultSeed, stMap, wl) ||
            !worldmap::PackWorldMap(stBiomes, stMap, stMapWords, wl)) {
          std::fprintf(stderr, "%s", wl.c_str());
          return 1;
        }
        worldmap::SetCurrentWorldMap(std::move(stMap)); }
      Simulation stSim;
      if (!stSim.Init(stCtx.device, stWorld, m, rx, mic, stTrees, stMapWords, ad + "/shaders"))
        return 1;
      Physics stPhys; stPhys.Init();
      DebrisSystem stDebris; stDebris.Init(&stPhys, &stWorld, m, rx);
      MobSystem stMobs; stMobs.Init(&stPhys, &stWorld, &stDebris, m, rx);
      MicroBodySet stMbSet;
      stDebris.SetMicroSet(&stMbSet);
      stMobs.SetMicroSet(&stMbSet);
      {
        std::vector<MobDef> defs;
        std::string ml;
        std::shared_ptr<MobDefFactory> stFac;
        LoadMobDefs(ad + "/mobs", m, defs, stMbSet, ml, &stFac);
        stMobs.SetDefFactory(std::move(stFac));
        ItemLibrary stItems;
        std::string ie;
        LoadItems(ad + "/items", m.size(), stMbSet, stItems, ie);
        stSim.UploadMicroBodies(stCtx.queue, stMbSet);
        stMobs.SetDefs(std::move(defs));
        {
          ai::Library beh;
          std::string blog;
          ai::LoadBehaviors(ad + "/mobs/behaviors.json", beh, blog);
          stMobs.SetBehaviors(std::move(beh));
          StyleLibrary sty;
          LoadAttackStyles(ad + "/mobs/attack_styles.json", sty, blog);
          stMobs.SetAttackStyles(std::move(sty));
          stMobs.SetItems(&stItems);
        }
        Stream stStream;
        stStream.Init(&stCtx, &stWorld, &stSim, kDefaultSeed);
        stStream.OnMaterialsReloaded(m);
        selftest::Options so;
        if (rebaseline) so.rebaseline = true;
        selftest::Ctx sc{stCtx,  stWorld, stSim,  m,       rx,
                         stPhys, stDebris, stMobs, stStream, stItems};
        int r3 = selftest::Run(sc, so);
        if (r3 != 0) failures++;
      }
    }

    double elapsed = NowSeconds() - t0;
    std::printf("\n=== suite acceptance %s (%.1fs) ===\n",
                failures == 0 ? "PASS" : "FAIL", elapsed);
    return failures == 0 ? 0 : 1;
  }

  // Every mode below — the 23 selftest gates, --shot/--shot-mob, --measure and
  // the windowed game (swapchain + imgui_impl_vulkan) — runs on Vulkan, the
  // only backend. The smokes build their own GpuContext, so they run here
  // before the game's asset load.
  if (vkSmoke)
    return sandvox::RunVkSmoke(lowPowerAdapter, sledgehammer, vkValidation,
                               residencyPaged, rebaseline);
  if (vkSmokeLoud)
    return sandvox::RunVkSmokeLoud(lowPowerAdapter, sledgehammer, vkValidation,
                                   residencyPaged, rebaseline);

  std::string assetDir = AssetDir();
  // Tuning first: LoadShader() bakes these into every shader's constant
  // prelude, so they have to be live before the first pipeline build.
  {
    Tuning tune;
    LoadTuning(assetDir + "/materials/tuning.json", tune);
    for (const std::string& w : tune.warnings)
      std::fprintf(stderr, "tuning: %s\n", w.c_str());
    // The windowed lab always exercises the full excite/settle loop:
    // fluidExciteMode is the one live CPU-read fluid knob (consumed per tick
    // in EncodeTick's input stream), so this is a runtime force, not a file
    // edit — tuning.json keeps the shipped default. --fluid-bench sets it
    // per scene itself.
    if (labScene >= 0) tune.sim.fluidExciteMode = 1;
    // --shot-jump photographs a POSE, and the default world drops the spawn in
    // a forest: a third-person boom inside a pine is a picture of bark. Trees
    // and cover off gives bare terrain to jump on and a clear line to the
    // body. Forced here rather than in tuning.json because it feeds the shader
    // constant prelude and therefore worldgen — it has to be true before the
    // first chunk is generated, not toggled later.
    if (g_shotJump) {
      tune.debug.vegetation = 0;
      tune.debug.groundCover = 0;
    }
    SetCurrentTuning(tune);
  }
  std::vector<MaterialDef> mats;
  std::vector<ReactionGpu> reactions;
  std::string errors;
  if (!LoadAssets(assetDir + "/materials/materials.json",
                  assetDir + "/materials/reactions.json", mats, reactions, errors)) {
    std::fprintf(stderr, "asset load failed:\n%s\n", errors.c_str());
    return 1;
  }
  std::printf("loaded %zu materials, %zu reactions\n", mats.size(), reactions.size());
  // THE SOLUTE TABLE (solutes.json) the alchemy bench dissolves by (package
  // C); reloaded with the materials (R). A bad table is reported and the
  // bench simply dissolves nothing.
  std::vector<SoluteDef> benchSolutes;
  {
    std::string serr;
    if (!LoadSolutes(assetDir + "/materials/solutes.json", mats, benchSolutes, serr))
      std::fprintf(stderr, "solutes: %s", serr.c_str());
  }

  // glyphs (assets/spells/glyphs.json — DESIGN.md §8 "The spell system").
  // Content like materials.json, so it loads here and hot-reloads on the same
  // key (R). A bad glyph file is a LOUD failure at startup rather than a spell
  // that silently conjures air.
  GlyphLibrary glyphs;
  {
    std::string gerr;
    if (!LoadGlyphs(assetDir + "/spells/glyphs.json", mats, glyphs, gerr)) {
      std::fprintf(stderr, "glyph load failed:\n%s", gerr.c_str());
      return 1;
    }
    std::printf("loaded %zu glyphs (%zu conjoined)\n", glyphs.glyphs.size(),
                glyphs.conjoined.size());
  }
  // Bumped whenever `glyphs` or `mats` is replaced (the R reload). The
  // character screen's glyph and spell-readout mirrors are cached against it
  // rather than rebuilt every frame — see "the mirrors" in the frame loop.
  uint32_t glyphEpoch = 1;
  uint32_t glyphsOwnedEpoch = 0;  // the epoch ui.glyphsOwned was built at
  // What a word list SAYS and COSTS (ExpandWords -> CompileSpell ->
  // DescribeSpell), keyed on the words. A pure function of (glyph library,
  // grimoire, words), so the cache is dropped whenever `glyphEpoch` moves or
  // the grimoire's content differs from the one it was filled against. It
  // was recomputed for every page, every bound hotbar page and the composer,
  // every frame, whether or not the character screen was even open.
  struct SpellDescCache {
    struct Entry {
      std::string text;     // DescribeSpell's bracket string alone
      std::string readout;  // + verdict + the dropped / truncated notes
      int32_t price = 0;
      bool unknown = false;
      int dropped = 0;
    };
    uint32_t epoch = 0;
    std::string grimoireKey;
    std::unordered_map<std::string, Entry> byWords;
  } spellDesc;

  // items (assets/items/items.json — game/item.h). Content, same as glyphs,
  // same hot-reload key. Not fatal if it fails: an item file that will not
  // load costs you the hotbar, not the game, and the rest of the session is
  // still worth having.
  // Declared here, LOADED BELOW once the micro-body pool exists: an item owns
  // its own .vox now, and its brick goes in the same pool the rigs use (a held
  // item is drawn by the borrowed slot's own render path). See the load beside
  // LoadMobDefs.
  ItemLibrary items;

  // voxel art prefabs (PLAN §A): drop .vox files in assets/prefabs/
  std::vector<Prefab> prefabs;
  {
    std::string plog;
    LoadPrefabDir(assetDir + "/prefabs", mats.size(), prefabs, plog);
    if (!plog.empty()) std::fprintf(stderr, "%s", plog.c_str());
    std::printf("loaded %zu prefabs\n", prefabs.size());
  }

  // static micro-detail bricks (docs/PLAN_voxel_editor.md §A). Runs AFTER
  // LoadAssets because it needs the compiled material list to resolve names,
  // and BEFORE Simulation::Init because it SETS MATF_MICRO on `mats` — the
  // material table upload has to carry that flag or the raymarcher never looks
  // at the brick table.
  MicroSet micro;
  {
    std::string mvlog;
    LoadMicroVox(assetDir + "/materials/materials.json", assetDir, mats, micro, mvlog);
    if (!mvlog.empty()) std::fprintf(stderr, "%s", mvlog.c_str());
    std::printf("loaded %u micro materials (%u frames, %zu pool words)\n",
                micro.materialCount, micro.frameCount, micro.pool.size());
  }

  // The baked tree atlas (src/sim/treeatlas.h). AFTER LoadAssets, because it
  // resolves the material NAMES its .svtree files carry against the compiled
  // table, and before Simulation::Init, which uploads it.
  // The biome set (assets/biomes/*.json) is loaded FIRST: it is the id space
  // the tree atlas's weight table and the worldMap buffer's record table are
  // both laid out in (docs/PLAN_world_map.md P1).
  biomes::BiomeSet biomeSet;
  std::vector<uint32_t> worldMapWords;
  {
    std::string blog;
    if (!biomes::LoadBiomeSet(assetDir, mats, biomeSet, blog)) {
      std::fprintf(stderr, "%s", blog.c_str());
      std::fprintf(stderr, "biome files failed to load -- refusing to start\n");
      return 1;
    }
    std::vector<std::string> problems;
    if (biomes::ValidateBiomeSet(biomeSet, problems)) {
      for (const std::string& p : problems) std::fprintf(stderr, "biomes: %s\n", p.c_str());
      std::fprintf(stderr, "biome files are invalid -- refusing to start with a world "
                           "whose biome ids cannot be laid out\n");
      return 1;
    }
    worldmap::WorldMapData map;
    if (!worldmap::LoadWorldMap(assetDir, worldmap::ActiveMapName(CurrentTuning().world.mapLayer), biomeSet, mats.size(), kDefaultSeed, map, blog) ||
        !worldmap::PackWorldMap(biomeSet, map, worldMapWords, blog)) {
      std::fprintf(stderr, "%s", blog.c_str());
      std::fprintf(stderr, "world map '%s' failed to load -- refusing to start (a world with no "
                           "map is not a world; see src/sim/worldmap.h)\n",
                   worldmap::ActiveMapName(CurrentTuning().world.mapLayer).c_str());
      return 1;
    }
    worldmap::SetCurrentWorldMap(std::move(map));
    // The authored edit layer the MAP names (map.json `editLayer`, P7). Read
    // here, before any world exists, so the very first SubmitWorldgen already
    // queues it — a layer loaded after worldgen would not appear until
    // something happened to regenerate the chunks it lives in. After the map,
    // because a v2 layer resolves against the map's ground.
    LoadWorldEditLayerForMap(assetDir);
  }
  TreeAtlas treeAtlas;
  {
    std::string tlog;
    if (!LoadTreeAtlas(assetDir + "/trees", mats, biomeSet, treeAtlas, tlog)) {
      std::fprintf(stderr, "%s", tlog.c_str());
      std::fprintf(stderr, "tree atlas failed to load -- refusing to start with a "
                           "half-read forest\n");
      return 1;
    }
    if (!tlog.empty()) std::fprintf(stderr, "%s", tlog.c_str());
  }
  // What the world is about to be generated FROM, as one line: the tuner
  // compares these against the files on disk to say whether the running game
  // is behind an Environment save (docs/PLAN_environment_truth.md P-A).
  // Re-stamped by every environment reload (F7 / regen world / Apply).
  biomes::EnvironmentStamp envStamp =
      biomes::StampEnvironment(assetDir, worldmap::ActiveMapName(CurrentTuning().world.mapLayer),
                                worldmap::CurrentWorldMap().editLayer);
  std::printf("%s\n", envStamp.Line().c_str());
  // ...and the inputs that shape the world AFTER worldgen: tuning.json,
  // materials.json, reactions.json (src/sim/tuningstamp.h). All three
  // hot-reload and none is in the save, so they are what two machines running
  // "the same build" are most likely to differ on. One line here is the whole
  // of the desync-cause elimination we can afford before a transport exists;
  // under DESIGN.md §10's model these three numbers go in the join handshake.
  const sandvox::TuningStamp tuneStamp = sandvox::StampTuning(assetDir);
  std::printf("%s\n", tuneStamp.Line().c_str());
  auto envStampMessage = [&envStamp]() {
    return std::string("{\"v\":3,\"type\":\"environment\",\"stamp\":") + envStamp.Json() + "}";
  };

  GLFWwindow* window = nullptr;
  if (!selftest && !shot && !shotWaterfall && !shotDebrisPond && !measure && !perf &&
      !fluidBench && !shaderStats &&
      shotMob.empty() && voxdumpArgs.empty() && !voxserve && exportEdits.empty()) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window = glfwCreateWindow(1600, 900, "sandvox", nullptr, nullptr);
    if (!window) return 1;
  }
  StartupMark("assets loaded (materials, micro, trees), window created");

  // RENDER_STATS (gpu/resources.h): the raymarch's per-call-site step
  // counters compile in only for the consumer that reads them back — the live
  // Performance tab. Decided BEFORE the device and the first LoadShader,
  // because it is a prelude const, not a flag. NOT for --perf: the harness
  // does not harvest them yet (perfsuite.cpp), and a counter nobody reads
  // would still perturb the raymarch number it reports.
  SetRenderStatsEnabled(telemetryEnabled);

  GpuContext ctx;
  // Timestamps: --measure and --fluid-bench are the only modes that request
  // the TimestampQuery device feature (per-pass GPU timings).
  if (!ctx.Init(window, 1600, 900, lowPowerAdapter,
                /*wantTimestamps=*/measure || perf || renderBudget || fluidBench ||
                    budgetArms || telemetryEnabled || g_harnessFrames > 0,
                backend, vkValidation,
                sledgehammer))
    return 1;
  StartupMark("device + swapchain");

  // ARM PIPELINE STATISTICS CAPTURE BEFORE THE FIRST PIPELINE EXISTS. This is
  // the whole reason --shader-stats is a bool up here instead of a runner
  // argument down at the dispatch: VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT
  // is a CREATE flag, and Simulation::Init below builds every compute pipeline
  // in the engine. Set it afterwards and the mode reports nothing, silently.
  if (shaderStats) rhi::vkr::SetCaptureStats(ctx.device, true);

  Telemetry telemetry;
  if (telemetryEnabled) telemetry.Start(telemetryPort);

  World world;
  world.residency =
      residencyPaged ? World::Residency::Paged : World::Residency::Dense;
  world.Init(ctx.device);
  StartupMark("world buffers (page pool, far cascades)");
  Simulation sim;
  // WHETHER THE FAR CASCADES COMPILE AT ALL (Simulation::FarBuild). Decided
  // before Init because BuildPipelines is what would start them. Eager for
  // anything that will render a horizon — the game and --frames, every --shot
  // family, --measure / --perf / --render-budget, --shader-stats (which must
  // see every pipeline), and the FULL selftest suite, which contains the far
  // gates. Lazy for the iteration tools: --voxdump / --voxserve, a filtered
  // `--gate` / `--verify` run, --sweep, the fluid bench. Lazy is not "never":
  // a far gate under `--gate far-fog` still gets its pipelines, it just pays
  // for them when it asks (Simulation::EnsureFarPipelines) instead of the
  // whole run paying at exit for a horizon nobody looked at.
  {
    const bool voxelTool = voxserve || !voxdumpArgs.empty() || !exportEdits.empty();
    const bool checkedOutput =
        selftest || verify || measure || perf || renderBudget || budgetArms ||
        shot || shotFrames || shotWaterfall || shotDebrisPond || shotFluid ||
        shotFluidPond ||
        fluidBench || shaderStats || !shotMob.empty() || !sweepParam.empty();
    const bool rendersHorizon =
        shot || shotFrames || shotWaterfall || shotDebrisPond || shotFluid ||
        shotFluidPond ||
        !shotMob.empty() || measure || perf || renderBudget || budgetArms ||
        shaderStats || suiteAcceptance ||
        (selftest && stOpt.only.empty() && !stOpt.list && sweepParam.empty());
    const bool eager = (!checkedOutput && !voxelTool) || rendersHorizon;
    sim.SetFarBuild(eager ? Simulation::FarBuild::Eager
                          : Simulation::FarBuild::Lazy);
    if (!eager)
      std::printf("far-cascade pipelines: lazy (compiled only if this run asks "
                  "for cascade content)\n");
  }
  if (!sim.Init(ctx.device, world, mats, reactions, micro, treeAtlas, worldMapWords,
                assetDir + "/shaders"))
    return 1;
  StartupMark("sim init: every compute shader through Tint + the driver");

  // --shader-stats answers HERE, before a player, a physics world or a single
  // tick exists: every pipeline in the engine has now been created, which is
  // the only state it needs. The render pipelines are the exception — they are
  // built lazily on the first draw — so force them into existence in the
  // format the offscreen harnesses use, or the `raymarch` fragment row (the
  // one the whole mode is for) would be missing from the table.
  if (shaderStats) {
    sim.ForceRenderPipelines(rhi::TextureFormat::RGBA8Unorm);
    return sandvox::RunShaderStats(ctx.device, "build/shader_stats.json");
  }

  // ---- LIVE PERFORMANCE TELEMETRY (--telemetry) ---------------------------
  //
  // The tuner's Performance tab in "watch me play" mode: the same PerfSample
  // the --perf harness records, produced once per frame and pushed down the
  // WebSocket. Everything below is gated on `telemetryEnabled`, which is off by
  // default, so the ordinary game encodes the same command buffers it always
  // did and pays nothing — the ONE cost that survives the flag being off is
  // this pair of default-constructed PassTimers, which allocate nothing until
  // Init() is called.
  //
  // ROW granularity, deferred collection. Blocking on timestamps at 60 fps
  // would make the profiler the slowest thing in the frame; KickDeferred puts
  // the map behind a fence and PollDeferred picks it up two or three frames
  // later, tagged with the frame it belongs to.
  PassTimer liveTimer, liveRenderTimer;
  bool liveTimed = false;
  // The raymarch's inside (measure/renderstats.h): only with telemetry, and
  // only if the RENDER_STATS const actually compiled in (fragment atomics).
  sandvox::RenderStatsRing liveStats;
  bool liveStatsOn = false;
  // The `--frames` harness takes the timers too (not the port): its summary
  // prints the per-node GPU rows beside the CPU scopes, from the same samples.
  if (telemetryEnabled || g_harnessFrames > 0) {
    if (liveTimer.Init(ctx, 192)) {
      liveTimer.SetRowGranularity(true);
      sim.SetPassTimer(&liveTimer);
      // Seven spans per render command buffer (kPerfRenderSpans), not one.
      liveTimed = liveRenderTimer.Init(ctx, 16);
    }
    liveStatsOn = RenderStatsEnabled() && FragmentStoresAvailable() &&
                  liveStats.Init(ctx);
    if (telemetryEnabled)
      std::printf("telemetry: live on port %u, GPU pass timings %s, raymarch "
                  "step counters %s\n",
                  telemetryPort, liveTimed ? "ON" : "unavailable (no timestamps)",
                  liveStatsOn ? "ON" : "off");
  }
  // The frame being accumulated, and the map from a frame number to the sample
  // still waiting for its GPU numbers. Three deep: a deferred timestamp map
  // lands two or three frames after the work, and a sample that has already
  // been sent cannot be corrected.
  sandvox::PerfSample liveSample;
  uint32_t liveFrameNo = 0;
  struct LivePending {
    uint32_t frame;
    sandvox::PerfSample s;
    std::vector<std::pair<const char*, double>> passes;  // harness only
  };
  std::vector<LivePending> livePending;

  Physics phys;
  if (!phys.Init()) return 1;
  // WHAT IS LYING ON THE GROUND. A dropped item is an ordinary debris body
  // (game/worlditems.h); this registry is the only thing debris cannot carry
  // — which body used to be which item. The release hook keeps the two in
  // step: Jolt reuses handles, so an entry that outlived its body would
  // eventually hand the player a sword they picked up off a rock.
  //
  // DECLARED BEFORE `debris`, and that is not tidiness. `debris` holds a
  // callback into this object, so it must be destroyed FIRST — with the
  // declarations the other way round, any future DebrisSystem destructor that
  // released its bodies would call into a WorldItems that no longer exists.
  // Ordering makes that unrepresentable; a teardown call would only make it
  // unlikely.
  WorldItems ground;
  // (The corpses you can loot are dead MOBS now and need no registry of
  // their own: game/corpses.h reads their gear live off the rig.)
  DebrisSystem debris;
  debris.Init(&phys, &world, mats, reactions);
  debris.SetOnBodyGone([&ground](uint64_t h) { ground.OnBodyGone(h); });
  MobSystem mobs;
  mobs.Init(&phys, &world, &debris, mats, reactions);
  // S5b parking (game/persist.h). Lives beside `mobs` rather than in the
  // tick block so a load can ResetWaits(): records LoadWorld had to keep
  // (cap full, pool refused) are retried at once, not on the next window move.
  MobParking mobParking;
  // Micro-body bricks (PLAN §C) are packed at mob-def load and uploaded
  // straight after: they are per-DEF art, shared by every instance. The set
  // persists past load because the sphere spawner packs 2x-detail ball models
  // into the same pool lazily (one per material, cached below) and re-uploads.
  // Damage also allocates here: a blasted or cut micro body clones its model
  // copy-on-write so its crater is its own (sim/microbody.h), which is why the
  // debris system needs a handle on the same set.
  MicroBodySet mbSet;
  debris.SetMicroSet(&mbSet);
  // Carving a LIVE limb clones its brick out of the same pool, so mobs need the
  // same handle: without it a wounded limb still loses real voxels, it just
  // cannot show them (game/mob.h CarveLimbRadial).
  mobs.SetMicroSet(&mbSet);
  // sphereModels moved into TickAuthorityCtx (N5, section D): per-WORLD tick
  // scratch, aliased back for main() below the ctx construction.
  {
    std::vector<MobDef> mobDefs;
    std::string mlog;
    std::shared_ptr<MobDefFactory> mobFac;
    LoadMobDefs(assetDir + "/mobs", mats, mobDefs, mbSet, mlog, &mobFac);
    // Beside the defs, and for the same lifetime: this is what lets a creature
    // become a variant of itself later (MobSystem::DefWithEffects).
    mobs.SetDefFactory(std::move(mobFac));
    if (!mlog.empty()) std::fprintf(stderr, "%s", mlog.c_str());
    std::printf("loaded %zu mob defs (%zu micro-body limb models, %zu pool words)\n",
                mobDefs.size(), mbSet.models.size(), mbSet.pool.size());
    // Items load into the SAME pool, before the upload, so a held item's brick
    // rides the one UploadMicroBodies the rigs already pay for. Not fatal if
    // it fails: a broken item file costs you the hotbar, not the session.
    std::string ierr;
    const bool itemsOk = LoadItems(assetDir + "/items", mats.size(), mbSet,
                                   items, ierr);
    // Warnings survive a successful load (a stray palette index, a missing
    // grip context), so print them either way rather than only on failure.
    if (!ierr.empty()) std::fprintf(stderr, "%s", ierr.c_str());
    if (itemsOk) std::printf("loaded %zu items\n", items.items.size());
    // WHAT THE SHARED POPULATION COSTS, said once, because it is half of a
    // ceiling the other half of which is spent at runtime on damaged bodies
    // (world.h kMaxMicroBodyModels). Every garment added here is four more
    // records nobody's gore gets to use, and until 2026-09-15 there was no
    // number anywhere that said how close to the wall a fresh world started.
    std::printf("micro bodies: %zu/%u shared model records, %zu/%u pool words "
                "after mobs + items\n",
                mbSet.models.size(), kMaxMicroBodyModels, mbSet.pool.size(),
                kMicroBodyPoolWordsWorld);
    sim.UploadMicroBodies(ctx.queue, mbSet);
    mobs.SetDefs(std::move(mobDefs));
    // NPC behaviour profiles (game/ai_behavior.h). Content, like materials and
    // glyphs: a missing file is not an error, it just means every mob keeps the
    // wander-and-avoid it has always had.
    ai::Library beh;
    std::string blog;
    if (ai::LoadBehaviors(assetDir + "/mobs/behaviors.json", beh, blog))
      std::printf("loaded %zu behaviour profiles\n", beh.profiles.size());
    // ...and the authored ATTACK STYLES they swing with (game/strokes.h). Same
    // contract: a missing file is content, not an error — the AI still issues
    // attack requests and the log says why nothing swings.
    StyleLibrary sty;
    if (LoadAttackStyles(assetDir + "/mobs/attack_styles.json", sty, blog))
      std::printf("loaded %zu attack styles\n", sty.styles.size());
    if (!blog.empty()) std::fprintf(stderr, "%s", blog.c_str());
    mobs.SetBehaviors(std::move(beh));
    mobs.SetAttackStyles(std::move(sty));
    // The item library, so an NPC's sweep can read the damage and HEFT of
    // whatever is in its fist. By pointer, because items reload on R.
    mobs.SetItems(&items);
  }
  // A PIECE OF GEAR HITTING THE FLOOR BY FORCE — a cuirass cut loose, a sword
  // knocked from a hand, anyone's — is the same kind of body an inventory drop
  // makes, and the registry is what makes `E` see it (DESIGN.md §8c). The
  // release hook above already forgets it when the body goes.
  mobs.SetOnItemShed(
      [&ground](uint64_t h, const ItemInstance& it) { ground.Add(h, it); });
  Stream stream;
  stream.Init(&ctx, &world, &sim, kDefaultSeed);
  stream.OnMaterialsReloaded(mats);
  FarField far;
  far.Init(&world);
  StartupMark("physics, debris, mobs, far-field init");

  // WHO MAY RUN WHILE `far`/`fardown` ARE STILL COMPILING
  // (docs/PLAN_shader_compile.md package A). Simulation::BuildPipelines
  // returned before those two exist — 746 s + 98 s of driver work — so the
  // game is playable in ~100 s after a worldgen edit instead of ~17 min. The
  // cascades are render-only derived data, so a world without them ticks,
  // hashes and saves identically; it just has no horizon.
  //
  // The DEFAULT is to block (Simulation::EnsureFarPipelines), so getting this
  // list wrong costs a mode an unnecessary wait, never a wrong answer. Two
  // things opt out:
  //   - the interactive game and --frames, which are the whole point;
  //   - --voxdump / --voxserve, which read terrain through worldgen and never
  //     touch a cascade at all (a tuner region request must not wait 12 min).
  {
    const bool checkedOutput =
        selftest || verify || measure || perf || renderBudget || budgetArms ||
        shot || shotFrames || shotWaterfall || shotDebrisPond || shotFluid ||
        shotFluidPond ||
        fluidBench || shaderStats || !shotMob.empty() || !sweepParam.empty();
    const bool voxelTool = voxserve || !voxdumpArgs.empty() || !exportEdits.empty();
    sim.AllowDeferredFar(!checkedOutput || voxelTool);
    // The specialized raymarch variant follows the same rule and for the same
    // reason (Simulation::AllowDeferredRaymarchVariant): the interactive game
    // must not stall on a second compile of the biggest fragment shader, and a
    // checked output must not depend on when that compile happened to land.
    sim.AllowDeferredRaymarchVariant(!checkedOutput || voxelTool);
    // SANDVOX_NO_RAYMARCH_SPEC=1 draws every frame with the universal
    // pipeline. Two uses, and the second is the reason it is an env var rather
    // than an argument: it is the A/B arm for "the two variants produce
    // identical pixels" (--shot the same frame with and without it and diff),
    // and it is the first thing to try if a driver ever miscompiles the
    // variant — the same escape hatch, for the same reason, as
    // SANDVOX_SPIRV_OPT=0.
    if (const char* v = std::getenv("SANDVOX_NO_RAYMARCH_SPEC")) {
      if (*v && std::strcmp(v, "0") != 0) {
        sim.SetForceUniversalRaymarch(true);
        std::printf("raymarch specialization DISABLED (SANDVOX_NO_RAYMARCH_SPEC)\n");
      }
    }
  }

  // --verify first: it is the union of three modes below on this one context.
  if (verify) {
    if (rebaseline) stOpt.rebaseline = true;
    return RunVerify(ctx, world, sim, mats, reactions, phys, debris, mobs,
                     stream, items, stOpt, perfOpt, shotFrames, budgetArms);
  }
  if (shotFrames) shot = true;          // --shot-frames alone = filtered --shot
  if (budgetArms) renderBudget = true;  // --budget-arms alone = filtered budget

  if (measure) return RunMeasure(ctx, world, sim, mats);
  if (renderBudget)
    return sandvox::RunRenderBudget(ctx, world, sim, mats, perfOpt);
  if (perf) return sandvox::RunPerf(ctx, world, sim, mats, perfOpt);
  // The voxel-region modes answer here: after the device, shaders and material
  // table exist (genCellIn is WGSL and the palette is the COMPILED table), and
  // before anything spawns a player, a mob or a physics world — none of which a
  // terrain dump has any use for.
  if (!voxdumpArgs.empty())
    return RunVoxDump(ctx, world, sim, mats, voxdumpArgs, voxdumpOut);
  if (voxserve) return RunVoxServe(ctx, world, sim, mats);
  if (!exportEdits.empty()) return RunExportEdits(ctx, world, sim, mats, exportEdits);
  if (shot) return RunShots(ctx, world, sim);
  if (shotWaterfall) return RunWaterfallShot(ctx, world, sim);
  if (shotDebrisPond) return RunDebrisPondShot(ctx, world, sim, mats);
  if (shotFluid || shotFluidPond)
    return RunFluidShot(ctx, world, sim, mats, shotFluidPond);
  if (fluidBench)
    return RunFluidBench(ctx, world, sim, mats, fluidBenchScene,
                         stOpt.jsonPath);
  if (!shotMob.empty())
    return RunMobShot(ctx, world, sim, phys, debris, mobs, items, shotMob,
                      stream, mats);
  if (!shotStrikeA.empty())
    return RunStrikeShot(ctx, world, sim, phys, debris, mobs, items,
                         shotStrikeA, shotStrikeStyle, shotStrikeB,
                         shotStrikeTail, stream, mats);
  if (rebaseline) stOpt.rebaseline = true;

  // ---- --replay-ops <file> [--ticks N]: the record, played back ------------
  //
  // The `ops-replay` gate's pass B, pointed at a file somebody else wrote. It
  // is a MODE and it returns: no window, no player, no frame loop, so what
  // reproduces the world is the record and nothing else. SetReplay also arms
  // the TickParams comparison inside SubmitTick, which is the interesting half
  // — it asserts that every per-tick knob (wind, day phase, the fluid gates,
  // the water bodies) is a pure function of the recorded input rather than of
  // some CPU state the replay does not have.
  //
  // Checked BEFORE --record-ops arms anything: replaying into a recorder would
  // append the replay's own frames to whatever file argv named.
  if (!replayOpsPath.empty()) {
    namespace ops = sandvox::opstream;
    ops::Log log;
    std::string err;
    if (!log.Load(replayOpsPath, mats, err)) {
      std::fprintf(stderr, "--replay-ops: %s\n", err.c_str());
      return 1;
    }
    std::printf("=== replay %s: %zu frames, seed %u, record version %u ===\n",
                replayOpsPath.c_str(), log.frames.size(), log.header.seed,
                log.header.version);
    // A harness tick has to block for its readback the way a game frame's
    // pacing does, or World::Snap() never becomes valid (test/support.h).
    SetHarnessSnapshotDrain(true);
    ops::ResetReplayStats();
    ops::SetReplay(&log);
    SubmitWorldgen(ctx, world, sim, log.header.seed);
    ctx.WaitIdle();
    constexpr uint32_t kProbeEvery = 15;
    uint32_t played = 0, lastHash = 0;
    // M9.3-C: how many recorded chunk resyncs this replay re-applied. Reported
    // beside the TickParams mismatch count, because a replay that reproduced
    // the hash means something different when the record contained syncs and
    // the replay silently applied none of them.
    uint32_t replayReplaces = 0;
    for (const ops::Frame& f : log.frames) {
      if (replayTicks && played >= replayTicks) break;
      // M9.3-C: the phase-B position of THIS drive loop. A chunk resync is a
      // per-tick input that is not an op (oprecord.h Frame::chunkReplaces);
      // applied here, before the submit, so the tick's CA runs on the
      // corrected chunk exactly as it did during the recording.
      replayReplaces += sandvox::opstream::ReplaceChunksIfReplaying(f.in.tick, stream);
      SubmitTick(ctx, world, sim, f.in.tick, f.in.seed, f.ops, f.exps, f.cells,
                 f.in.hashEnable != 0,
                 {f.in.playerChunk[0], f.in.playerChunk[1], f.in.playerChunk[2]},
                 f.in.wantReadback != 0, f.in.particlesActive != 0, f.spawns,
                 f.in.farCount, f.fluid, f.in.fluidLive,
                 f.in.vizActive != 0);
      played++;
      if (f.in.tick % kProbeEvery == 0 || played == log.frames.size()) {
        lastHash = ReadHashSync(ctx, world);
        std::printf("  tick %6u  hash %08x\n", f.in.tick, lastHash);
      }
    }
    ops::SetReplay(nullptr);
    const uint32_t miss = ops::ReplayParamMismatches();
    std::printf("replay: %u ticks, final hash %08x, TickParams words rebuilt "
                "differently: %u, chunk resyncs re-applied: %u (refused %u)\n",
                played, lastHash, miss, replayReplaces,
                sandvox::opstream::ReplayChunkReplaceRefusals());
    if (miss) {
      // Rule 6: name the WORD, not the count. The word index is a u32 offset
      // into TickParams, so `offsetof(TickParams, field) / 4` in world.h reads
      // it straight off.
      std::printf("  first at tick %u, word %u: record %u, rebuilt %u | "
                  "distinct words (u32 offsets into TickParams):",
                  ops::ReplayFirstMismatchTick(), ops::ReplayFirstMismatchWord(),
                  ops::ReplayFirstMismatchRecorded(),
                  ops::ReplayFirstMismatchRebuilt());
      const std::vector<uint32_t>& w = ops::ReplayMismatchWords();
      for (size_t k = 0; k < w.size() && k < 24; k++) std::printf(" %u", w[k]);
      if (w.size() > 24) std::printf(" ... (%zu total)", w.size());
      std::printf("\n  *** a per-tick knob is read from outside the input "
                  "stream ***\n");
      // Known and structural, not a regression: the residency window ORIGIN
      // (TickParams.origin / mirrorBase) is moved by Stream::Update from the
      // tick body, and no recorded input carries it — so a replay of a session
      // that FLEW rebuilds the window where it started. The `ops-replay` gate
      // cannot see this: its scene never leaves one window.
    }
    return miss ? 1 : 0;
  }

  // ---- --record-ops <file>: arm the recorder for whatever runs below ------
  // Before the sweep, the selftest and the frame loop, so all three record.
  // The env var stays as N3 left it; argv wins when both name a file.
  if (!recordOpsPath.empty()) {
    std::string err;
    if (!sandvox::opstream::StartRecording(recordOpsPath, kDefaultSeed, mats,
                                          err)) {
      std::fprintf(stderr, "--record-ops: %s\n", err.c_str());
      return 1;
    }
    std::printf("op record: writing %s\n", recordOpsPath.c_str());
  }

  // --sweep sim.X=a,b,c [--sweep-gate <gate>]: run the determinism check at
  // each value, in-process, without touching any files. Proves a tuning knob
  // reaches the kernel (different values → different hashes).
  if (!sweepParam.empty()) {
    // Parse "sim.windDragRef=6,40,120" → field "windDragRef", values [6,40,120]
    size_t dot = sweepParam.find('.');
    size_t eq = sweepParam.find('=');
    if (dot == std::string::npos || eq == std::string::npos || eq <= dot) {
      std::fprintf(stderr, "--sweep wants sim.field=val1,val2,...\n");
      return 1;
    }
    std::string group = sweepParam.substr(0, dot);
    std::string field = sweepParam.substr(dot + 1, eq - dot - 1);
    std::string valStr = sweepParam.substr(eq + 1);
    std::vector<float> vals;
    {
      size_t p = 0;
      while (p < valStr.size()) {
        size_t c = valStr.find(',', p);
        if (c == std::string::npos) c = valStr.size();
        vals.push_back(std::stof(valStr.substr(p, c - p)));
        p = c + 1;
      }
    }
    if (vals.empty()) { std::fprintf(stderr, "--sweep: no values\n"); return 1; }

    Tuning baseTuning = CurrentTuning();
    // ANY group, not just sim: worldgen knobs are exactly the ones CLAUDE.md
    // asks you to prove with --sweep, and they were the ones it could not
    // reach. SetTuningField is generated from tuning_params.def.
    if (!SetTuningField(baseTuning, group, field, vals[0])) {
      std::fprintf(stderr, "--sweep: unknown field '%s.%s'\n", group.c_str(),
                   field.c_str());
      return 1;
    }

    std::string gate = sweepGate.empty() ? "determinism" : sweepGate;
    constexpr int kSweepTicks = 100;
    std::printf("=== sweep %s.%s over %zu values, gate %s, %d ticks ===\n",
                group.c_str(), field.c_str(), vals.size(), gate.c_str(),
                kSweepTicks);

    SetHarnessSnapshotDrain(true);
    std::vector<uint32_t> hashes;
    for (size_t vi = 0; vi < vals.size(); vi++) {
      Tuning t = baseTuning;
      SetTuningField(t, group, field, vals[vi]);
      SetCurrentTuning(t);
      sim.ReloadShaders(ctx.device);
      SubmitWorldgen(ctx, world, sim, kDefaultSeed);
      ctx.WaitIdle();
      for (uint32_t tick = 1; tick <= kSweepTicks; tick++) {
        SubmitTick(ctx, world, sim, tick, kDefaultSeed,
                   SelftestOps(tick, kDefaultSeed), SelftestExps(tick, kDefaultSeed), {},
                   tick == kSweepTicks, {8, 3, 8}, false,
                   SelftestParticlesActive(tick));
      }
      uint32_t h = ReadHashSync(ctx, world);
      hashes.push_back(h);
      std::printf("  %s.%s = %.4g  →  hash %08x\n", group.c_str(),
                  field.c_str(), vals[vi], h);
    }
    SetCurrentTuning(baseTuning);

    bool allSame = true;
    for (size_t i = 1; i < hashes.size(); i++)
      if (hashes[i] != hashes[0]) allSame = false;
    if (allSame) {
      std::printf("\n*** ALL HASHES IDENTICAL — the parameter does not reach "
                  "the kernel at these values ***\n");
    } else {
      // Count distinct hashes
      std::unordered_set<uint32_t> unique(hashes.begin(), hashes.end());
      std::printf("\n  parameter REACHES the kernel (%zu distinct hash%s)\n",
                  unique.size(), unique.size() == 1 ? "" : "es");
    }
    return 0;
  }

  if (selftest) {
    if (stOpt.list) return selftest::List();
    selftest::Ctx sc{ctx,   world,  sim,    mats,  reactions,
                     phys,  debris, mobs,   stream, items};
    const int rc = selftest::Run(sc, stOpt);
    // The gate has printed its verdict; everything after this line is
    // destructors. Marked because a headless run was measured sitting 45 s
    // between its last line and process exit (2026-09-09), and without a
    // clock on it that time is invisible.
    StartupMark("selftest returned; teardown begins");
    return rc;
  }

  // BEFORE Overlay::Init — ImGui's own scroll callback chains to whatever was
  // installed first, and installing after it replaces ImGui's and freezes every
  // scrollable panel in the overlay. See the note on ScrollCallback.
  glfwSetScrollCallback(window, ScrollCallback);

  Overlay overlay;
  if (!overlay.Init(window, ctx.device, ctx.surfaceFormat, assetDir)) return 1;

  // Audio comes up HERE, after the three headless modes have returned: none of
  // --shot/--shot-mob/--selftest should ever open a sound device (there is no
  // audio hardware in CI, and a selftest that depends on one is not a test).
  // A failed init is not an error anywhere — the game runs silent.
  audio::Cues audioCues;
  if (!noAudio) audioCues.Init(assetDir + "/sounds", mats);
  StartupMark("imgui overlay + audio");

  UIState ui;
  // The field overlay's authored initial state (F4 cycles from here). Seeded
  // rather than defaulted so a tuning.json that asks for the arrows gets them
  // without a keypress — which is what makes the overlay reachable from a
  // headless run.
  ui.fieldViz = FieldVizFromTuning(CurrentTuning());
  // Same idea for short range: `--short-range` / SANDVOX_SHORT_RANGE set the
  // checkbox's starting position, and from here the checkbox is the authority
  // (the frame loop re-asserts it into SetShortRange every frame). Without
  // this seed the flag would be latched in support.cpp and the panel would
  // show the box unticked while the frame rendered at 100 m.
  ui.shortRange = ShortRangeMode();
  ui.shortRangeNear = ShortRangeNear();
  {
    const auto& fs = CurrentTuning().sim;
    ui.fGravity     = fs.fluidGravity;
    ui.fStiffness   = fs.fluidStiffness;
    ui.fRestDensity = fs.fluidRestDensity;
    ui.fEosPower    = fs.fluidEosPower;
    ui.fCohesion    = fs.fluidCohesion;
    ui.fAttractSame = fs.fluidAttractSame;
    ui.fAttractDiff = fs.fluidAttractDiff;
    ui.fViscosity   = fs.fluidViscosity;
    ui.fDamping     = fs.fluidDamping;
    ui.fSplashRate       = fs.fluidSplashRate;
    ui.fSplashSpeed      = fs.fluidSplashSpeed;
    ui.fSplashMaxDensity = fs.fluidSplashMaxDensity;
    ui.fSplashLife       = fs.fluidSplashLife;
    ui.fSplashScaleIdx   = fs.fluidSplashScaleIdx;
    ui.fFoamRate         = fs.fluidFoamRate;
    ui.fFoamCrestRate    = fs.fluidFoamCrestRate;
    ui.fTrappedMin       = fs.fluidTrappedMin;
    ui.fTrappedMax       = fs.fluidTrappedMax;
    ui.fCrestMin         = fs.fluidCrestMin;
    ui.fCrestMax         = fs.fluidCrestMax;
    ui.fFoamEnergyMin    = fs.fluidFoamEnergyMin;
    ui.fFoamEnergyMax    = fs.fluidFoamEnergyMax;
    ui.fFoamLife         = fs.fluidFoamLife;
    ui.fFoamLifeMin      = fs.fluidFoamLifeMin;
    ui.fBubbleBuoyancy   = fs.fluidBubbleBuoyancy;
    ui.fFoamDrag         = fs.fluidFoamDrag;
    ui.fBubbleDensity    = fs.fluidBubbleDensity;
    ui.fSprayDensity     = fs.fluidSprayDensity;
    ui.fFoamScaleIdx     = fs.fluidFoamScaleIdx;
    ui.fExciteMode       = fs.fluidExciteMode;
    ui.windGasScale      = fs.windGasScale;
    ui.windPartScale     = fs.windPartScale;
    ui.windDragRef       = fs.windDragRef;
    ui.fSettleEps        = fs.fluidSettleEps;
    ui.fWakeSpeed        = fs.fluidWakeSpeed;
    ui.fSettleTicks      = fs.fluidSettleTicks;
    const auto& fr = CurrentTuning().render;
    ui.fSurface      = fr.fluidSurface;
    ui.fIso          = fr.fluidIso;
    ui.fSmooth       = fr.fluidSmooth;
    ui.fIor          = fr.fluidIor;
    ui.fClarity      = fr.fluidClarity;
    ui.fReflect      = fr.fluidReflect;
    ui.fSpecular     = fr.fluidSpecular;
    std::memcpy(ui.fShallow, fr.fluidShallow, sizeof(ui.fShallow));
    std::memcpy(ui.fDeep,    fr.fluidDeep,    sizeof(ui.fDeep));
    ui.fDepth        = fr.fluidDepth;
    ui.fGradientStr  = fr.fluidGradient;
    ui.fRFoam        = fr.fluidFoam;
    ui.fRFoamField   = fr.fluidFoamField;
    ui.fRFoamTexture = fr.fluidFoamTexture;
    ui.fRFoamSpeed   = fr.fluidFoamSpeed;
    ui.fWobble       = fr.fluidWobble;
    ui.fParticleSize = fr.fluidParticleSize;
    ui.fStretch      = fr.fluidStretch;
    ui.fDensityShade = fr.fluidDensityShade;
  }
  FillUiMaterials(mats, ui);

  // ---- the character panel's portrait target -------------------------------
  // Created ONCE at a fixed size, never resized with the window: the image is
  // displayed 1:1 at whole-pixel coordinates through a nearest sampler, so a
  // target that tracked the framebuffer would resample it and throw away
  // exactly the crispness the sampler is there for. RenderAttachment because
  // the world pipelines draw into it, TextureBinding because ImGui samples it.
  //
  // NEVER READ BACK. rhi::ReadBufferBlocking is forbidden in the frame path
  // (rhi.h), and nothing here needs it: the pixels go straight from the pass
  // that wrote them to the ImGui draw that samples them, GPU-side, in the same
  // frame.
  //
  // THE FORMAT IS THE SWAPCHAIN'S, and that is not cosmetic:
  // Simulation::EnsureRenderPipelines caches on ONE target format and rebuilds
  // EVERY render pipeline when it changes. A portrait in RGBA8 beside a
  // BGRA8 swapchain would therefore rebuild the whole render pipeline set
  // TWICE PER FRAME for as long as the screen was open.
  constexpr uint32_t kPortraitW = 320, kPortraitH = 448;
  rhi::Texture portraitTexture = ctx.device.CreateTexture(
      {kPortraitW, kPortraitH, 1}, ctx.surfaceFormat,
      rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::TextureBinding,
      "avatarPortrait");
  rhi::TextureView portraitView = portraitTexture.CreateView();
  ui.portraitTex = overlay.RegisterTexture(portraitView);
  // ---- THE ALCHEMY BENCH'S PICTURE (game/alchemy_bench.h) -----------------
  // Drawn on the CPU by the bench's sim and copied in each frame it is open
  // (rhi CopyBufferToTexture, from a staging buffer the queue writes). RGBA8
  // like ImGui's own atlas; nearest-sampled through the portrait's sampler,
  // so the integer scale the panel draws it at stays pixel-crisp.
  alchemy::AlchemyBench bench;
  // The largest table; a table uses its top-left W x H.
  constexpr uint32_t kBenchW = (uint32_t)alchemy::AlchemyBench::kMaxW;
  constexpr uint32_t kBenchH = (uint32_t)alchemy::AlchemyBench::kMaxH;
  rhi::Texture benchTexture = ctx.device.CreateTexture(
      {kBenchW, kBenchH, 1}, rhi::TextureFormat::RGBA8Unorm,
      rhi::TextureUsage::CopyDst | rhi::TextureUsage::TextureBinding, "alchemyBench");
  rhi::TextureView benchView = benchTexture.CreateView();
  rhi::Buffer benchStaging = ctx.device.CreateBuffer(
      (uint64_t)kBenchW * kBenchH * 4, rhi::BufferUsage::CopySrc | rhi::BufferUsage::CopyDst,
      "alchemyBenchStaging");
  ui.alchemy.tex = overlay.RegisterTexture(benchView);
  ui.alchemy.texW = (int)kBenchW;
  ui.alchemy.texH = (int)kBenchH;
  ui.alchemy.liftH = alchemy::AlchemyBench::kLiftH;
  ui.alchemy.minW = alchemy::AlchemyBench::kMinW;
  // ---- internal render scale (render.renderScale) --------------------------
  // The world's offscreen colour target when the scale is below 1, cached on
  // its size like the depth targets; the blit up to the swapchain and the
  // native-size UI pass are in the render section. At scale 1 none of this
  // exists and the frame renders into the swapchain exactly as before.
  rhi::Texture scaledTex;
  rhi::TextureView scaledView;
  uint32_t scaledW = 0, scaledH = 0;
  // ---- TAA (render.taa; assets/shaders/taa.wgsl) ---------------------------
  // The offscreen target above becomes unconditional when TAA is on, because
  // the resolve reads the finished world frame out of it — at renderScale 1 it
  // is the same size as the swapchain, and the resolve does anti-aliasing
  // rather than upscaling, but the path is identical either way.
  //
  // `taaFrameNo` is the jitter sequence's index. It counts RESOLVED frames, not
  // rendered ones, so a frame the resolve skipped (feature off, portrait pass)
  // does not advance the sequence past a sample that was never taken.
  uint32_t taaFrameNo = 0;
  bool taaWasOn = false;
  float taaScaleWas = -1.0f;
  // Present mode last requested from the context; -1 forces the first frame
  // to apply whatever tuning / --present says.
  int presentApplied = -1;
  // fpsCap: the deadline the previous frame ended on.
  double fpsCapLast = 0.0;
  ui.portraitW = (int)kPortraitW;
  ui.portraitH = (int)kPortraitH;
  // A FIXED "studio" sun, independent of the world clock. --shot-mob's own
  // comment says why: midnight is the worst possible light for judging a
  // silhouette, and a character sheet that goes unreadable at night is one you
  // cannot use half the time. 0.30 of a day is mid-morning, the same phase the
  // fluid shots pick for the same reason.
  const uint32_t kPortraitLightTick =
      (uint32_t)(0.30 * (double)TicksPerDay(CurrentTuning()));

  // Material COLLISION class LUT for the player's mirror queries. Not raw
  // klass: BuildCollisionClasses remaps passable vegetation to gas so the
  // capsule sweep moves through reeds and kelp (sim/materials.h).
  std::vector<uint32_t> classOf = BuildCollisionClasses(mats);

  // The window around the spawn site (SpawnWindowOrigin), so the boot
  // worldgen fills the ground the player lands on. The lab scenes below set
  // their own origins; the smoke/selftest paths never reach here.
  if (labScene < 0) world.SetWindowOrigin(SpawnWindowOrigin());
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  StartupMark("worldgen submitted");

  // ---- ONE PLAYER, AS AN OBJECT (game/session.h, PLAN_multiplayer_now N5) --
  //
  // Everything below that used to be a per-player local in main() now lives
  // in this struct, and what main() keeps is an ALIAS reference per member so
  // the presentation half of the file keeps its names. A second
  // `PlayerSession session2;` compiles today and touches no global: that is
  // the package's acceptance criterion, and the aliases are a migration seam
  // the next package renames away.
  PlayerSession session;
  // THIS ONE OWNS THE WINDOW (M9.1 P1). Index 0 is the primary every world
  // phase of the tick consults for a window question — the dev panel's ray,
  // the far-plume eye, the chunk the submit centres on — and `localView` is
  // what lets the tick write the death screen, the hit-stop dip and the combat
  // cues to the real UIState instead of to a throw-away sink. Both are the
  // defaults; they are spelled out here because a second session added beside
  // this one must NOT take them.
  session.index = 0;
  session.localView = true;
  Camera& cam = session.cam;
  Player& player = session.player;
  Brush& brush = session.brush;
  PrefabPlacer& placer = session.placer;
  // The player avatar shares MobSystem's def list rather than loading its own:
  // one micro-body pool, and a hot reload (R) rebuilds both at once. It points
  // at whichever def is named by tuning.json player.model, so swapping
  // the player character is data, not code.  F5 re-reads it.
  std::string& avatarDefName = session.avatarDefName;
  avatarDefName = CurrentTuning().player.model;
  PlayerAvatar& avatar = session.avatar;
  avatar.Init(&phys, &world, &debris, mats, &mobs);
  avatar.SetDefs(&mobs.Defs(), avatarDefName);
  // THE PLAYER IS A TARGET. The avatar is a Mob but it is not in MobSystem's
  // list, so the handle-keyed lookups could not find it and an NPC's sweep
  // melted the player's limbs as debris instead of wounding them
  // (MobSystem::SetAvatar has the whole argument). Registered ONCE here, for
  // the whole session: it is a pointer to a member of this frame, and
  // Despawn/Revive rebuild the rig behind it without moving the object.
  mobs.SetAvatar(&avatar);
  ThirdPersonRig& tpRig = session.tpRig;
  CameraMode& camMode = session.camMode;
  float& avatarHeading = session.avatarHeading;   // body facing, radians +Y
  float& fovNow = session.fovNow;
  fovNow = CurrentTuning().camera.fovY;
  // First-person eye: its eased offset from the player's own eye to the
  // posed head's eyes (PlayerAvatar::HeadEyeWorld). Render-only.
  Vec3 fpHeadDelta{};
  bool fpHeadValid = false;
  float& respawnTimer = session.respawnTimer;
  // Night-ambience rarity roll. Deliberately a plain PRNG and NOT the sim's
  // counter-based hash: this decides whether a mood bed plays, never anything
  // that touches voxel state, so it is outside the determinism domain (rule 1
  // constrains the sim). Seeding it from the clock would be wrong for a
  // different reason — a replay should sound the same — so it is fixed.
  std::mt19937 nightRng{0x9147A1};
  float nightRollTimer = 0.0f;
  // Clear-then-fill, matching the hot-reload path below. These run once here,
  // but an append-only build of a list the UI indexes into is exactly how a
  // duplicate entry (and the ImGui ID collision that follows) gets introduced.
  ui.prefabNames.clear();
  ui.mobNames.clear();
  for (const Prefab& p : prefabs) ui.prefabNames.push_back(p.name);
  for (const MobDef& d : mobs.Defs()) ui.mobNames.push_back(d.name);
  for (const ai::Profile& p : mobs.Behaviors().profiles)
    ui.aiProfileNames.push_back(p.name);
  // WHAT THE AI PANEL CAN ARM A SPAWN WITH: every melee item in the library,
  // by KIND rather than by a list of names, so a blade added to items.json
  // shows up in the picker on the next R and nothing here has to be edited.
  //
  // The selection is re-found BY NAME afterwards for the reason
  // selftest_playerkit pins: a library index is items.json's order, so an
  // insert renumbers everything after it and a raw index would silently move
  // the pick onto the neighbouring weapon.
  auto rebuildAiWeapons = [&ui, &items]() {
    const std::string was =
        (ui.aiWeaponPick >= 0 && ui.aiWeaponPick < (int)ui.aiWeaponNames.size())
            ? ui.aiWeaponNames[ui.aiWeaponPick]
            : std::string();
    ui.aiWeaponNames.clear();
    // "fists" RATHER THAN "(unarmed)", AND THAT IS A BEHAVIOUR CHANGE, NOT A
    // RELABEL. Before the impact package an empty hand meant a creature that
    // could not attack at all, so the entry was the ABSENCE of a choice; it
    // now names a weapon — a rig's `natural` block gives it fists and jaws,
    // and behaviors.json lists punch/bite styles as fallbacks, so a creature
    // spawned this way fights. The sentinel still works by being a name no
    // item has: `items.Find("fists")` returns -1, At(-1) is nullptr, and the
    // mob spawns with nothing in its fist (see the spawn site below).
    ui.aiWeaponNames.push_back("fists");
    for (const ItemDef& it : items.items)
      if (it.kind == ItemKind::Melee) ui.aiWeaponNames.push_back(it.name);
    // First build has nothing to restore: default to the arming sword, which
    // is what these buttons armed a spawn with before the picker existed.
    const std::string want = was.empty() ? std::string("sword") : was;
    ui.aiWeaponPick = 0;
    for (int i = 0; i < (int)ui.aiWeaponNames.size(); i++)
      if (ui.aiWeaponNames[i] == want) ui.aiWeaponPick = i;
  };
  rebuildAiWeapons();
  // ---- THE F1 "GIVE ME A FILLED VESSEL" PICKERS (UIState::giveVesselNames) --
  // Every container item, and per vessel every material whose CLASS it holds,
  // sorted by name. By name and re-found by name on each rebuild, for the
  // weapon picker's reason; rebuilt on R because both the item library and
  // the material table can change under it.
  auto rebuildGiveVessels = [&ui, &items, &mats]() {
    const std::string wasV =
        (ui.giveVesselPick >= 0 && ui.giveVesselPick < (int)ui.giveVesselNames.size())
            ? ui.giveVesselNames[ui.giveVesselPick] : std::string();
    std::string wasM;
    if (ui.giveVesselPick >= 0 && ui.giveVesselPick < (int)ui.giveVesselMats.size()) {
      const auto& l = ui.giveVesselMats[ui.giveVesselPick];
      if (ui.giveMatPick >= 0 && ui.giveMatPick < (int)l.size()) wasM = l[ui.giveMatPick];
    }
    ui.giveVesselNames.clear();
    ui.giveVesselMats.clear();
    for (const ItemDef& it : items.items) {
      if (!it.IsContainer()) continue;
      std::vector<std::string> ms;
      for (size_t m = 1; m < mats.size(); m++) {
        const uint32_t k = mats[m].gpu.klass;
        if (k <= 31 && ((it.container.holds >> k) & 1u)) ms.push_back(mats[m].name);
      }
      std::sort(ms.begin(), ms.end());
      ui.giveVesselNames.push_back(it.name);
      ui.giveVesselMats.push_back(std::move(ms));
    }
    ui.giveVesselPick = 0;
    ui.giveMatPick = 0;
    for (int i = 0; i < (int)ui.giveVesselNames.size(); i++)
      if (ui.giveVesselNames[i] == wasV) ui.giveVesselPick = i;
    if (ui.giveVesselPick < (int)ui.giveVesselMats.size()) {
      const auto& l = ui.giveVesselMats[ui.giveVesselPick];
      for (int i = 0; i < (int)l.size(); i++)
        if (l[i] == (wasM.empty() ? std::string("water") : wasM)) ui.giveMatPick = i;
    }
  };
  rebuildGiveVessels();
  // ---- THE WARDROBE PICKERS (game/dye.h, the overlay's Wardrobe window) ----
  //
  // Every DYEABLE piece, split by the slot it goes in. Same contract as the
  // weapon picker above and for exactly the same reason: names, not indices,
  // rebuilt on every R, re-found by name afterwards.
  //
  // ELIGIBILITY IS `ItemDef::dyeable`, which is authored in the piece's own
  // sidecar beside the greyscale art that makes it true — NOT a list of item
  // names here. Adding a tenth pattern is then a generator run and an R, with
  // no C++ edit, which is the test of whether this is content or code.
  auto rebuildWardrobe = [&ui, &items]() {
    auto fill = [&items](std::vector<std::string>& out, int& pick,
                         ItemKind kind) {
      const std::string was =
          (pick > 0 && pick < (int)out.size()) ? out[pick] : std::string();
      out.clear();
      out.push_back("(none)");
      for (const ItemDef& it : items.items)
        if (it.kind == kind && it.dyeable) out.push_back(it.name);
      pick = out.size() > 1 ? 1 : 0;   // first real piece, not "(none)"
      if (!was.empty())
        for (int i = 0; i < (int)out.size(); i++)
          if (out[i] == was) pick = i;
    };
    fill(ui.wardrobeShirts, ui.wardrobeShirtPick, ItemKind::ArmorChest);
    fill(ui.wardrobeLegs, ui.wardrobeLegsPick, ItemKind::ArmorLegs);
    fill(ui.wardrobeFeet, ui.wardrobeFeetPick, ItemKind::ArmorBoots);
  };
  rebuildWardrobe();
  // ---- THE AI PANEL'S OUTFIT PICKERS (UIState::aiOutfit) -------------------
  //
  // One name list per WORN equip slot, every item the slot's authored rule
  // accepts (EquipSlotAccepts — the same test a drag onto the character screen
  // makes), dyeable or not. Same contract as the three wardrobe lists: names,
  // rebuilt on every R, the pick re-found by name afterwards. The slot LABEL
  // travels with it so the overlay can say "Head" without including the equip
  // table.
  auto rebuildAiWear = [&ui, &items]() {
    std::vector<std::string> was(kEquipSlotCount);
    for (int s = 0; s < (int)ui.aiWearNames.size() && s < kEquipSlotCount; s++)
      if (s < (int)ui.aiWearPick.size() && ui.aiWearPick[s] > 0 &&
          ui.aiWearPick[s] < (int)ui.aiWearNames[s].size())
        was[s] = ui.aiWearNames[s][ui.aiWearPick[s]];
    ui.aiWearSlotLabels.clear();
    ui.aiWearNames.clear();
    ui.aiWearPick.clear();
    for (int s = 0; s < kEquipSlotCount; s++) {
      if (!EquipSlotIsWorn(s)) break;   // Head..Trinket lead the table
      std::vector<std::string> names{"(none)"};
      for (const ItemDef& it : items.items)
        if (EquipSlotAccepts(s, it.kind)) names.push_back(it.name);
      int pick = 0;
      for (int i = 0; i < (int)names.size(); i++)
        if (!was[s].empty() && names[i] == was[s]) pick = i;
      ui.aiWearSlotLabels.push_back(EquipSlotAt(s).label);
      ui.aiWearNames.push_back(std::move(names));
      ui.aiWearPick.push_back(pick);
    }
  };
  rebuildAiWear();
  // WHICH CREATURE the panel spawns. Same contract as the weapon picker above,
  // for the same reason: the list is rebuilt off the LIVE defs on every R and
  // the selection is re-found BY NAME, because a def index is directory order
  // and adding a creature would silently move the pick onto its neighbour.
  //
  // Eligibility is "publishes a held_right socket", which is the test the spawn
  // ALREADY applied silently — the panel arms what it spawns, and a creature
  // with no fist cannot be armed.
  //
  // AND IT IS BODIES ONLY: a def carrying `effects` is some body in this list
  // with a modifier already poured on it, and it is reachable as that body plus
  // the box (see the effect list below). Filtering them out is what stops the
  // combo from growing a row per authored combination — and it is also what
  // keeps a def MobSystem::DefWithEffects COMPOSED this session (`human+zombie`,
  // appended to the live defs and never removed) from turning up in the picker
  // as if somebody had authored it.
  auto rebuildAiCreatures = [&ui, &mobs, &avatarDefName]() {
    const std::string was = (ui.aiCreaturePick >= 0 &&
                             ui.aiCreaturePick < (int)ui.aiCreatureNames.size())
                                ? ui.aiCreatureNames[ui.aiCreaturePick]
                                : std::string();
    ui.aiCreatureNames.clear();
    for (const MobDef& d : mobs.Defs())
      // ...and not a random-human pool body a spawn built this session
      // (MobSystem::PoolDef): those are the "random human" button's.
      if (d.FindSocket("held_right") >= 0 && d.effects.empty() &&
          !MobSystem::IsPoolName(d.name))
        ui.aiCreatureNames.push_back(d.name);
    // First build defaults to the avatar's own species, which is the def the
    // old code preferred when it picked for you — so the panel's behaviour is
    // unchanged until somebody touches the combo.
    const std::string want = was.empty() ? avatarDefName : was;
    ui.aiCreaturePick = 0;
    for (int i = 0; i < (int)ui.aiCreatureNames.size(); i++)
      if (ui.aiCreatureNames[i] == want) ui.aiCreaturePick = i;
  };
  rebuildAiCreatures();
  // ...AND WHICH MODIFIERS IT CAN POUR ON THAT BODY. The effects directory is
  // the list (MobEffectNames), re-read on every R for the same reason the two
  // lists above are rebuilt there: a file somebody just wrote should get its box
  // without a restart. Ticks are carried across BY NAME, so adding an effect
  // cannot silently move a tick onto its alphabetical neighbour.
  auto rebuildAiEffects = [&ui, &assetDir]() {
    std::vector<std::string> on;
    for (int i = 0; i < (int)ui.aiEffectNames.size(); i++)
      if (i < (int)ui.aiEffectOn.size() && ui.aiEffectOn[i] != 0)
        on.push_back(ui.aiEffectNames[i]);
    ui.aiEffectNames = MobEffectNames(assetDir + "/mobs");
    ui.aiEffectOn.assign(ui.aiEffectNames.size(), 0);
    for (int i = 0; i < (int)ui.aiEffectNames.size(); i++)
      if (std::find(on.begin(), on.end(), ui.aiEffectNames[i]) != on.end())
        ui.aiEffectOn[i] = 1;
  };
  rebuildAiEffects();
  // Creatures the AI panel put in the world, so its "kill all spawned" button
  // reaps exactly those and leaves content-placed mobs alone.
  // aiSpawnedMobs moved into TickAuthorityCtx (N5, section D).
  // The map's spawn site (SpawnPos above): the selftest's own player proxies
  // keep their literal (140, 140) -- they are fixtures on the harness pad.
  player.pos = SpawnPos();
  std::printf("spawn: (%d, %d) on the map's spawn site, ground y%d\n",
              worldmap::CurrentWorldMap().spawnX, worldmap::CurrentWorldMap().spawnZ,
              (int)player.pos.y - 10);
  if (g_fellSiteSet) {
    const int px = g_fellSiteX - 48;
    player.pos = Vec3{(float)px,
                      (float)(World::TerrainHeight(px, g_fellSiteZ, kDefaultSeed) + 10),
                      (float)g_fellSiteZ};
    cam.yaw = 0.0f;    // Forward() = +X: the tree is planted 48 voxels that way
    cam.pitch = 0.0f;
    // SANDVOX_FELL_YAW=<radians>: look away from the tree by that much — the
    // body-cull arm (the pieces half in view, or wholly behind the camera).
    if (const char* e = std::getenv("SANDVOX_FELL_YAW")) cam.yaw = (float)std::atof(e);
    std::printf("--fell-tree: spawn moved to (%d, %d) so the tree stands at (%d, %d)\n",
                px, g_fellSiteZ, g_fellSiteX, g_fellSiteZ);
  }
  // Lab: fixed per-scene pose, flying, aimed at the scene — the same pose the
  // bench renders from, so what is judged live and what is measured headless
  // are the same framing.
  if (labScene >= 0) {
    Vec3 labEye;
    float labYaw = 0, labPitch = 0;
    LabSceneCamera(labScene, labEye, labYaw, labPitch);
    player.pos = labEye;
    cam.yaw = labYaw;
    cam.pitch = labPitch;
    player.fly = true;
    ui.fly = true;
  }
  // PLAY OR DEV at boot (UIState::devControls). A lab scene is a bench,
  // SANDVOX_DEV=1 asks for one, and every SCRIPTED run (--frames, each --shot,
  // which all set g_harnessFrames) keeps the dev bindings and fly state it was
  // measured with. Otherwise the game starts as a game: walking, hands up.
  // F2 flips it.
  ui.devControls = labScene >= 0 || g_harnessFrames > 0 ||
                   std::getenv("SANDVOX_DEV") != nullptr;
  if (!ui.devControls) {
    ui.fly = false;
    player.fly = false;
    ui.tool = UIState::kToolMelee;
  }
  // The render camera interpolates prevPos -> pos across a tick (N2), and
  // every placement above is a TELEPORT: without this the first frames would
  // lerp the eye in from the constructor's default position.
  player.SnapRender();
  // seed the far-field cascades around spawn (coarsest first; the queue
  // drains at kFarListCap level-chunks per tick through SubmitTick)
  far.FullRefill(IVec3{ifloor(player.pos.x) >> 4, ifloor(player.pos.y) >> 4,
                       ifloor(player.pos.z) >> 4});
  StartupMark("far-field refill queued");
  // kinematic capsule proxy so debris collides with (and is shoved by) the
  // player; terrain collision stays in the AABB controller
  uint64_t& playerBody = session.playerBody;
  playerBody = phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);
  // The avatar's body is exempt from THIS capsule and nobody else's
  // (Physics::BodyRole, OWNED). Once, here: every respawn reads it.
  session.avatar.SetCollisionOwner(playerBody);

  bool& captured = session.captured;
  // What `captured` was before the character screen took the cursor, so
  // closing hands it back rather than assuming. A player who pressed Esc to
  // free the cursor, then opened the screen, then closed it, must not have the
  // cursor grabbed out from under them.
  bool captureBeforeUi = true;
  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  double mx0 = 0, my0 = 0;
  glfwGetCursorPos(window, &mx0, &my0);

  KeyEdge eP, eN, eV, eF1, eF2, eF3, eF4, eF5, eF6, eF7, eF9, eF10, eR, eEsc, eLBracket, eRBracket, eJump,
      eJ, eX, eB, eT, eO, eM, eK, eTab, eC, eH, eZ, eBack, eDel, eU, eL, eI;
  // THE HANDS (dual wielding, 2026-09-27): Q puts the selected hotbar stack
  // in the LEFT hand, E in the RIGHT (a swap: what was held goes back into
  // that hotbar slot; an empty selected slot takes the hand's item back).
  KeyEdge eQ, eE;
  // THE CONVERSATION KEYS (game/dialogue.h): 1-9 answer, Space/Enter
  // continue. Their own edges: the number row's other bindings are gated off
  // while the panel is up, and a held key must answer once.
  KeyEdge eTalk[9], eTalkGo;
  // What `captured` was before a conversation freed the cursor (the
  // captureBeforeUi rule, for the panel).
  bool talkWasOpen = false, captureBeforeTalk = true;
  // (G has no KeyEdge: it is a hold-aware binding — tap to take, hold to
  // drag or, with a throwable vessel in hand, to throw — see the block by
  // `takeE` — and an edge tracker would only be half of it.)
  KeyEdge eGlyph[kGlyphSlots];
  bool prevMouseL = false;
  bool prevMouseR = false;
  // ---- THE FRAME LAYER'S COMMAND ACCUMULATOR (PLAN_multiplayer_now N2) ----
  //
  // This replaces the ad-hoc sticky latches that used to live here —
  // `castQueued`, `strikeQueued`, `ui.placePrefab`, `ui.spawnMob`,
  // `dropStatusQueued` — each of which was a hand-rolled instance of the same
  // rule: the tick loop below runs ZERO times on most frames at 60+ fps
  // against a 30 Hz tick, so a frame-local one-shot is discarded unread eight
  // tries out of nine (that is the bug that made RMB casting fire one try in
  // nine, and the note it left behind is now this type's docstring).
  //
  // The frame layer WRITES held state and the axes wholesale every frame, ORs
  // in every pressed edge it sees, and adds the frame's raw mouse pixels. Each
  // tick CONSUMES one TickInput: held state is broadcast to every tick of a
  // multi-tick frame, an edge is delivered to exactly one tick. That contract
  // is what the `tick-input` gate pins, and it is the message a networked
  // client will send.
  TickInputFeeder& feeder = session.feeder;
  // A body part clicked in the inspector with a sentence on the stack, latched
  // the same way: the slot, or -1. NOT a TickInput field — it is a click in an
  // ImGui panel on a specific rig part, which is a UI transaction rather than
  // a player command, and it has no meaning on a remote peer.
  int& castAtPartQueued = session.castAtPartQueued;
  // ---- looking at things, and looting them (game/corpses.h) ----------------
  // What the reach ray found this frame (a debris body handle or 0), the
  // corpse the loot panel is open on, and whether E opened the character
  // screen to show it — so closing the loot closes the screen it opened and
  // leaves alone one the player opened themselves.
  uint64_t& lookBody = session.lookBody;
  uint64_t& lootCorpse = session.lootCorpse;
  bool& lootOpenedScreen = session.lootOpenedScreen;
  // ---- the physics grab (game/grab.h) -------------------------------------
  // E IS TWO BINDINGS ON ONE KEY. Tapped it is the pickup/loot it has always
  // been; held past player.grabHoldTime it lifts whatever the reach ray is on
  // and carries it. That works only because the tap now fires on RELEASE and
  // only when the hold never latched — `eHeld` is how long the key has been
  // down, `eGrabbed` remembers that this press was spent on a grab (or on
  // dropping one) so letting go does not also pick something up.
  GrabHold& grab = session.grab;
  float& eHeld = session.eHeld;
  // G held past the hold time with a throwable vessel in hand: the throw's
  // wind-up (fed to TB_THROW below). Frame-side like `eHeld`.
  bool gThrowHold = false;
  bool& eGrabbed = session.eGrabbed;
  bool& ePrevDown = session.ePrevDown;
  std::vector<uint64_t>& lookIgnore = session.lookIgnore;  // own limbs, per frame
  // ...and the same list for the STRIKE aim ray (the melee tick's, below).
  // Its own vector rather than a share of `lookIgnore`: that one is filled in
  // the frame block and read by the E prompt, and the melee ray runs inside
  // the tick loop, where overwriting it would silently change what E offers.
  std::vector<uint64_t>& strikeIgnore = session.strikeIgnore;
  std::vector<Grenade>& grenades = session.grenades;
  // Where a spell resolved, for the renderer: a short-lived burst of sprites
  // (SpellEmission::impacts). Render-only, counted down per TICK so the flash
  // lasts the same world-time at any frame rate.
  // SpellFlash itself is in game/session.h: the tick body makes them and the
  // render block draws them.
  std::vector<SpellFlash>& spellFlashes = session.spellFlashes;

  // ---- magic (game/spell.h, game/caster.h) ---------------------------------
  // The VM is not player-coupled: SpellSystem takes an origin, a direction and
  // a CasterState, so a mob can drive the identical call later. PlayerCaster is
  // only the player's inventory + spoken stack, kept out of Player (which stays
  // a clean movement controller).
  SpellSystem& spells = session.spells;
  spells.SetLibrary(&glyphs);
  PlayerCaster& caster = session.caster;
  caster.inventory.GrantAllAndBind(glyphs);   // placeholder acquisition
  caster.Recompile(glyphs);
  // Health lives on PlayerAvatar's per-part hp, read through this indirection
  // so the VM never includes the avatar (thesis 4 in spell.h).
  CasterHealth& playerHealth = session.playerHealth;
  playerHealth.ctx = &avatar;
  playerHealth.get = [](void* c) {
    return ((PlayerAvatar*)c)->TotalHealth();
  };
  playerHealth.spend = [](void* c, int32_t amount) {
    ((PlayerAvatar*)c)->SpendHealth(amount);
  };
  // ---- items and melee (game/item.h, game/melee.h) -------------------------
  // Same shape as the caster block above: a hotbar the player owns, and a
  // state machine that turns mouse motion into a swing. Neither is bolted onto
  // Player or PlayerAvatar — main.cpp holds them and pushes the resulting pose
  // into the avatar, exactly as it already does for heading.
  MeleeState& melee = session.melee;
  // The feel numbers come from tuning.json's `melee.*` group (sim/tuning.h
  // Tuning::Melee, applied by game/melee.cpp ApplyMeleeTuning). Seeded here and
  // re-applied in the F5 block, which is the whole point of the migration: the
  // stroke's feel is a JSON edit and a keypress rather than a rebuild.
  ApplyMeleeTuning(melee.tuning);
  // ---- DISCRETE STRIKES (the player's only melee control) -----------------
  // The player's authored-attack state: a bare StrokeCursor stepped by the
  // same StepStrokeProgram the NPCs use (game/strokes.h), driving the SAME
  // `melee` above — so the sweep, the whoosh, the Arrest and the Nudge blocks
  // below never learn which mode fed the driver. The picker owns the flick
  // read; the two ints are the press latches (see the sticky-flag note at the
  // click edges: the tick loop runs 0..4x a frame, so an unlatched click is
  // dropped ~8 frames of 9).
  StrikePicker& strikePicker = session.strikePicker;
  StrokeCursor& playerStrike = session.playerStrike;
  int& strikeQueued = session.strikeQueued;    // style latched at the press
  int& strikeBuffered = session.strikeBuffered;  // ONE strike banked mid-swing
  // THE KIT IS THE AVATAR'S (W2-M, Mob::kit_): hotbar, bag and equipment in
  // one Kit on the body that carries them. The hotbar is still WHAT IS IN
  // YOUR HAND; the melee path reads Inventory::Selected() exactly as before.
  // Two references so the rest of this function reads as it always has.
  //
  // What used to sit here — the avatar-kit callback a rising asked for the
  // bag and hotbar, and the `wearTried`/`wearDye` latches of the per-tick
  // wear loop — is gone: a rising reads the avatar's kit directly
  // (MobSystem::ServiceRising, `avatar.keepKitOnTurn`), and the rig dresses
  // itself from the kit (Mob::DressFromKit, session.cpp).
  Kit& kit = session.kit();
  Inventory& hotbar = kit.hotbar;
  // The last frame's RENDER eye, for the pour point (ContainerPourPoint): the
  // crosshair ray starts there -- ahead of the head in first person
  // (avatar.firstPersonForward), on the boom in third -- and the tick's pour
  // is settled before this frame's eye exists.
  Vec3 pourFrom{};
  bool pourFromValid = false;
  // ---- THE HANDS ARE KIT SLOTS (dual wielding, 2026-09-27) ----------------
  //
  // It used to be "the hotbar is the hand": the selected slot was drawn.
  // Now each hand is its own equipment slot (game/equipment.h HandR/HandL):
  // select a hotbar slot, press Q for the left hand or E for the right, and
  // it is in that fist until you put it away (the same key on an empty
  // hotbar slot). LMB swings or uses the right hand, RMB the left. The Sheath
  // and Quick slots are still places a blade rides on your person.
  {
    // ---- THE STARTING KIT, AND IT IS STILL A STUB ------------------------
    //
    // One of everything the library defines, put somewhere it can actually be
    // USED rather than all of it in the hotbar: armour goes to the pack (so it
    // can be dragged onto the figure), and anything else -- weapons included,
    // since the hotbar is the hand -- keeps the hotbar. Before
    // this, an armour item started in the hotbar and there was no way to get
    // it onto the body without first knowing to drag it out.
    //
    // MARKED AS A STUB deliberately: an economy replaces this, and the pickup
    // loop that P5 landed is the first half of one. What it must not become is
    // "the starting kit", quietly, because nobody removed it.
    for (int i = 0; i < (int)items.items.size(); i++) {
      const ItemDef& it = items.items[i];
      int home = -1;
      for (int s = 0; s < kEquipSlotCount && home < 0; s++)
        if (it.kind != ItemKind::Melee && !EquipSlotIsWorn(s) &&
            !EquipSlotIsHand(s) &&
            EquipSlotAccepts(s, it.kind) && kit.equip.At(s).Empty())
          home = s;
      // THE FIRST WEAPON STARTS IN THE RIGHT HAND, so the stub kit spawns
      // armed as it did when the selected hotbar slot was the hand.
      if (home < 0 && it.kind == ItemKind::Melee &&
          kit.equip.InHand(Hand::Right).Empty())
        home = EquipSlotOfHand(Hand::Right);
      if (home >= 0)
        kit.equip.slots[home] = StackOf(items, i);
      else if (ItemKindIsWorn(it.kind))
        kit.bag.Add(StackOf(items, i));        // armour, into the pack
      else
        hotbar.Add(StackOf(items, i));
    }
  }
  // The equipment slot table, mirrored into the UI once. It is authored data
  // (game/equipment.h EquipSlots), so the panel reads it rather than
  // restating it — the day ItemKind::ArmorHead exists, this needs no change.
  {
    ui.equipDefs.clear();
    for (int i = 0; i < kEquipSlotCount; i++) {
      const EquipSlotDef& d = EquipSlotAt(i);
      UIState::EquipSlotUI u;
      u.label = d.label;
      u.icon = d.icon;
      u.why = d.why;
      u.acceptsAnything = d.accepts[0] != ItemKind::None;
      // The accepted kinds, as the same words KitSlotUI::kind carries, so the
      // panel can light the slot a dragged piece belongs in. Copied from the
      // authored table rather than restated — a second list here is the one
      // that goes stale when a kind is added to a row.
      for (ItemKind k : d.accepts)
        if (k != ItemKind::None) u.accepts.push_back(ItemKindName(k));
      ui.equipDefs.push_back(std::move(u));
    }
    ui.bagCols = Bag::kCols;
    ui.bagRows = Bag::kRows;
    ui.handEquipSlot[0] = EquipSlotOfHand(Hand::Right);
    ui.handEquipSlot[1] = EquipSlotOfHand(Hand::Left);
  }
  // Burn-material ids for the inspector's charred readout, resolved ONCE here
  // and again after every materials reload — never per frame (see ResolveBurnMats).
  BurnMats burnMats = ResolveBurnMats(mats);
  TissueMats tissueMats = ResolveTissueMats(mobs, mats);
  // ---- THE DEATH SCREEN'S PHOTOGRAPH ---------------------------------------
  // Registered once, called from inside Die() while the rig is still whole
  // (PlayerAvatar::SetDyingObserver). It runs the SAME mirror the frame loop
  // runs, which is the only reason the death screen can show a real body: a
  // frame-loop mirror taken after the tick that killed you reads a husk, since
  // Die() hands every limb to DebrisSystem and zeroes `partAlive` — every
  // limb would report SEVERED with no voxels and no hp, whatever happened.
  //
  // `deathFrozen` below then stops the per-frame mirror from overwriting this,
  // so what is on the death screen stays the body that died rather than the
  // corpse as it burns, rots and gets eaten.
  //
  // THE POSE IS PART OF THE PHOTOGRAPH TOO — AND IT IS A POSE, NOT A PICTURE.
  //
  // The portrait is a live second camera pointed at the rig (MakePortraitCam),
  // so it dies with the readouts and for the same reason: after Die() every
  // PartBody() is 0, the camera is invalid, the pass is skipped, and the panel
  // goes on sampling whatever was last rendered into portraitTexture — a frame
  // of a LIVING body from the last time the screen happened to be open.
  //
  // Freezing the TEXTURE would fix what is on screen and nothing else: a death
  // you cannot turn round is a death you cannot read, and "which arm" or "how
  // deep" is usually not answerable from one angle. So what is frozen is the
  // body's POSE — every limb's world transform at the instant it died, plus its
  // collider box for framing and outlining — and the portrait goes on rendering
  // every frame, live, orbit and zoom and limb focus all working. The limbs are
  // DebrisSystem's by then and still drawn; the pass simply overwrites their
  // GPU transforms with these before it draws and puts the real ones back
  // after (see the portrait pass). What you orbit is the body as it fell,
  // however long it has since been burning, sinking or rolling downhill.
  // DeathBody itself is in game/session.h.
  DeathBody deathBody;
  // The living rig's boxes, hoisted so an open screen reuses the capacity
  // rather than allocating fifteen boxes a frame.
  LimbBoxes liveLimbBoxes;
  bool deathFrozen = false;
  avatar.SetDyingObserver([&] {
    ui.health = playerHealth.Get();
    ui.healthMax = avatar.HealthMax();
    ui.healthCap = avatar.HealthCap();
    ui.playerAlive = false;
    ui.locoState = avatar.Spawned() ? avatar.Locomotion().stateName : "";
    FillBodyUI(avatar, burnMats, tissueMats, mobs, mats, ui);
    ui.deathCause = avatar.DeathCause();
    // Front-on and un-zoomed, whatever the orbit was left at the last time the
    // screen was open: a death framed by a half-finished drag from a previous
    // inspection is not a readable one. The player can turn it from there.
    ui.portraitYaw = 0.0f;  ui.portraitPitch = -0.08f;
    ui.portraitZoom = 1.0f; ui.portraitZoomTarget = 1.0f;
    ui.portraitPanX = 0.0f; ui.portraitPanXTarget = 0.0f;
    ui.portraitPanY = 0.0f; ui.portraitPanYTarget = 0.0f;
    ui.portraitPivotSlot = -1;
    ui.portraitReset = false;
    ui.portraitFocusSlot = -1;
    deathBody.yaw = avatarHeading;
    CollectLimbBoxes(avatar, phys, deathBody.boxes);
    // EVERY PART, not just the def's limbs: the box list above is deliberately
    // limbs-only so the framing is not thrown off by a held sword, but the
    // transform override has the opposite requirement — a garment shell or a
    // weapon left at its live transform would be the one thing in the picture
    // still moving, sliding off a body that is standing still.
    deathBody.limbs.clear();
    const int parts = (int)avatar.PartCount();
    for (int i = 0; i < parts; i++) {
      DeathBody::Limb fl;
      fl.body = avatar.PartBody(i);
      if (!fl.body) continue;
      Vec3 pos; Quat rot;
      if (!avatar.PartWorldTransform(i, pos, rot)) continue;
      fl.xf.pos[0] = pos.x; fl.xf.pos[1] = pos.y; fl.xf.pos[2] = pos.z;
      fl.xf.quat[0] = rot.x; fl.xf.quat[1] = rot.y;
      fl.xf.quat[2] = rot.z; fl.xf.quat[3] = rot.w;
      deathBody.limbs.push_back(fl);
    }
    deathBody.have = !deathBody.boxes.Empty() && !deathBody.limbs.empty();
    deathFrozen = true;
  });
  // The blade's position last tick, so the sweep has something to sweep FROM.
  // Invalid until the first tick with a weapon drawn — a swing that started
  // from an unknown pose would carve a segment the blade never travelled.
  Vec3& lastEdgeBase = session.lastEdgeBase;
  Vec3& lastEdgeTip = session.lastEdgeTip;
  bool& lastEdgeValid = session.lastEdgeValid;
  // Rig slots the player's CURRENT swing has already delivered a blunt/bite
  // impulse to (melee.h EdgeSweep::struck). Owned here rather than on either
  // cursor because the player has two cut states -- the discrete program's and
  // the freeform driver's -- and one swing must mean one impulse in both.
  std::vector<uint64_t>& playerStruck = session.playerStruck;
  // ...and whether this swing has already BITTEN (melee.h EdgeSweep::bitten).
  // Once per STROKE rather than once per slot, and cleared on the same line
  // `playerStruck` is, for the same two-cut-states reason.
  bool& playerBitten = session.playerBitten;
  // duelDummySpawned moved into TickAuthorityCtx (N5, section D).

  // everExploded / lastExplosionTick moved into TickAuthorityCtx (N5).
  // MLS-MPM fluid (docs/PLAN_mpm_fluids.md): the CPU's CONSERVATIVE live
  // estimate — the GPU owns the real count now (settle kills particles,
  // excite births them; the seam's compaction maintains fluidArgs[FA_LIVE]).
  // Refreshed from the snapshot readback each frame, bumped by spawns
  // submitted since that snapshot's tick so a fresh pour never reads as
  // empty. Drives record/skip, draw counts and the HUD only — every kernel
  // re-bounds itself on the GPU count. Not persisted.
  // fluidCount moved into TickAuthorityCtx (N5, section D).
  // Spawns submitted after the newest snapshot's tick: (tick, count) pairs,
  // dropped once a snapshot at/after their tick arrives (the GPU count now
  // includes them).
  // fluidPendingSpawns moved into TickAuthorityCtx (N5, section D).
  // Splash sound cue: fired once per snapshot tick that reports a burst of
  // excitement, voiced through water's Impact slot (the Break precedent —
  // audio is presentation-only and reads the same readback).
  uint32_t lastFluidCueTick = 0;
  // fluidCueMat / fluidPourMat moved into TickAuthorityCtx (N5).
  // Last tick the MPM fluid was live: keeps the particle passes awake for the
  // splash droplets (see particlesActive below).
  // lastFluidTick moved into TickAuthorityCtx (N5, section D).
  // ---- fluid lab (lab/lab.h) ----
  // labTick is the SCENE clock: 1-based from worldgen (or the last L reset),
  // driving the build-op and pour schedules. Resetting it to 0 IS the scene
  // reset — the next sim tick re-submits the build ops (which cover the whole
  // scene volume, air included) and the pour replays identically, while the
  // world outside the scene box is untouched (no re-worldgen).
  // labTick moved into TickAuthorityCtx (N5, section D).
  // tuning.json watcher (~4 Hz, lab only): mtime of the file content this
  // process last LOADED (or wrote). A newer file on disk triggers the F5
  // path; the ImGui writeback refuses to clobber anything newer than this.
  const std::string labTuningPath = assetDir + "/materials/tuning.json";
  int64_t labTuningMtime =
      labScene >= 0 ? LabFileMtimeNs(labTuningPath) : -1;
  double labWatchPoll = 0.0;
  uint32_t tick = 0;
  uint32_t bodyInstCount = 0;
  // The cube-instance buffer as per-body spans (game/bodyreg.h
  // BodyInstanceArena): only what changed is uploaded, and each body is
  // frustum-culled on its own. `bodyDraws` is the main view's culled list.
  BodyInstanceArena bodyArena;
  // SANDVOX_BODY_INST_LEGACY=1: whole-list upload + one undivided draw, the
  // one-binary A/B arm for the arena and the cull (phys/debris.cpp reads the
  // same variable for its cull cache). The 18-vertex cube is NOT in the arm.
  const bool bodyInstLegacy = [] {
    const char* e = std::getenv("SANDVOX_BODY_INST_LEGACY");
    return e && e[0] != '0';
  }();
  std::vector<BodyVoxInst> bodyInstScratch;
  std::vector<uint64_t> bodyHandles;
  std::vector<std::pair<uint32_t, uint32_t>> bodyUploads, bodyDraws;
  // Per-frame render scratch, hoisted so the steady state reuses capacity.
  std::vector<BodyXformGpu> bodyXf;
  std::vector<MicroBodyInstGpu> microInsts;
  double lastTime = NowSeconds();
  double accumulator = 0;
  // ---- HIT-STOP (sim/tuning.h Tuning::CombatFx) ----------------------------
  //
  // WHAT IT DOES TO THE SIM: nothing. The dip scales the rate the tick
  // ACCUMULATOR fills at, so for a fifteenth of a second the world advances
  // fewer 30 Hz ticks per frame. That is a stream the sim already has to be
  // correct under — this loop runs it 0..4 times per frame depending on frame
  // time, and does so on every machine — so the world sees exactly what it
  // would have seen on a slower one. No tick computes anything different, no
  // hashed state is touched, and `--selftest` never executes this loop at all
  // (the gates drive SubmitTick directly through src/test/support.cpp).
  //
  // A STICKY LATCH WITH PEAK-HOLD, and that is not a style choice. The tick
  // loop below runs ZERO times on most frames at any frame rate over 30, so a
  // frame-local "was there a hit" bool is discarded unread most of the time —
  // the exact bug that made RMB casting fire one try in nine (see castQueued's
  // note). `pendScale`/`pendMs` are written INSIDE the tick loop, peak-held so
  // a sever landing in the same frame as a chip keeps the sever's dip, and
  // drained by the frame loop once, at the top, before the accumulator fills.
  //
  // PEAK-HOLD IS min ON THE SCALE AND max ON THE DURATION. They are peaks of
  // the same quantity — "how much stop" — expressed in units that run opposite
  // ways, and combining them with the same operator is how you get a sever
  // that stops hard for 55 ms because a chip landed in the same frame.
  // HitStop itself is in game/session.h: the tick body REQUESTS a dip and
  // the frame loop ages it, which is why it is presentation state.
  HitStop hitStop;
  // ---- COMBAT CUES: the same latch shape, for the same reason ---------------
  //
  // A cue raised inside the tick loop cannot be played there: audio drains once
  // per frame (down in the `audioCues.Enabled()` block), and the loop above it
  // may have run four times or none. So each combat cue is a one-shot request
  // with PEAK-HOLD ON POWER — a frame in which the blade crossed a body on
  // three consecutive ticks is ONE blow to the ear, and it should be the
  // hardest of the three rather than three overlapping copies of nearly the
  // same sample. `at` travels with it because a killing blow can despawn its
  // victim before the frame drains, exactly as MobSystem::VoiceEvent carries
  // its own defIndex for that reason.
  // CombatCueRequest itself is in game/session.h.
  CombatCueRequest& combatWhooshCue = session.combatWhooshCue;
  CombatCueRequest& combatFleshCue = session.combatFleshCue;
  CombatCueRequest& combatClangCue = session.combatClangCue;
  CombatCueRequest& combatStrikeCue = session.combatStrikeCue;
  CombatCueRequest& combatCutCue = session.combatCutCue;
  bool& combatStrikeEdged = session.combatStrikeEdged;
  // THE WHOOSH IS THE ONE CUE THAT IS NOT AN INSTANT, so its voice is kept and
  // moved. Handle from Cues::Combat; self-invalidating, so nothing here has to
  // know when the sample ended (audio/world.h PlayOneShotTracked).
  int combatWhooshVoice = -1;
  // ---- THE BLOCK HOOK (game/melee.h BlockEvent) ----------------------------
  //
  // `clang` is "a cut stopped by something that is not flesh". Two things
  // produce that, and BOTH exist now:
  //
  //   1. THE SWEEP hitting a non-flesh body — debris, a dropped item, a
  //      weapon held in someone else's fist. Wired above, at the sweep.
  //   2. A DELIBERATE BLOCK — a guard raised and the blade stopping on it.
  //      That is the BlockEvent queue, drained below the AI readout, which is
  //      this lambda's only caller.
  //
  // A lambda rather than inline code at the drain because the two producers
  // must agree on the latch, the peak-hold, the volume law and the slot, and
  // a second copy of any of those is a second thing to keep in step.
  // Deliberately takes the two things a block has and nothing else; it must
  // not need a Mob, because a parry between two NPCs has no player in it.
  auto CombatBlockCue = [&](const Vec3& at, float power) {
    const float pw = std::clamp(power, 0.0f, 1.0f);
    if (!combatStrikeCue.pending || pw > combatStrikeCue.power) {
      combatStrikeCue.power = pw;
      combatStrikeCue.at = at;
      combatStrikeEdged = true;
    }
    combatStrikeCue.pending = true;
    if (!combatClangCue.pending || pw > combatClangCue.power) {
      combatClangCue.power = pw;
      combatClangCue.at = at;
    }
    combatClangCue.pending = true;
    // A block is a hit that did not land, and it should still stop time —
    // being parried is information, and information the player does not feel
    // is information they miss. The chip tier: something happened, but nothing
    // came off.
    const Tuning::CombatFx& fx = CurrentTuning().combatfx;
    hitStop.Request(fx.hitStopChipScale, fx.hitStopChipMs);
    //
    // THE FLASH ON THE BLOCKING WEAPON IS NOT HERE, and that is not an
    // omission. MeleeSweepDamage already charges the parry to the blocking
    // item's own slot (`mobs.Damage(blockBody, ...)` in melee.cpp), and
    // Mob::Damage sets the hit flash for every cause with the two-tier split
    // an `item`-tagged limb resolves to `combatfx.flashChip` — the chip tier,
    // which is exactly what a parry is. Flashing again from here would be a
    // second writer of one fact, and it would double nothing but the risk of
    // the two drifting.
    //
    // The ATTACKER's blade does not flash: it takes no damage from being
    // stopped, and BlockEvent carries no handle for it. A feel item, not a
    // bug — see the merge report's checklist.
  };
  float fpsSmooth = 0, frameMsSmooth = 0, tickMsSmooth = 0, frameMsWorst = 0;
  float frameMsP95 = 0, frameMsP99 = 0;
  double fpsWinStart = lastTime, fpsWinWorst = 0;
  int fpsWinFrames = 0;
  // ---- LIVE TAIL PERCENTILES, over a much longer window than the 0.5 s the
  // average and the worst use, and the length is the whole point.
  //
  // `worst` is ONE sample, so it is whatever the unluckiest frame in the last
  // half second did — a background process waking up reads the same as a real
  // regression. p95/p99 are the numbers that say whether a stutter is
  // systematic, and they only mean anything with enough samples underneath
  // them: over a 0.5 s window (~15-50 frames) p99 IS the max, and reporting it
  // beside the max would be two names for one number.
  //
  // 512 frames is ~11 s at 45 fps and ~5 s at 100. p99 is then the ~6th worst
  // frame and p95 the ~26th, both genuinely distinct from the max. The cost is
  // a 512-float sort twice a second, which is microseconds and happens on the
  // same 0.5 s boundary the other stats already update on.
  //
  // Sampled unconditionally, not under the --frames harness guard, because
  // this readout is for playing the game and watching the panel.
  constexpr size_t kTailWindow = 512;
  std::vector<float> tailRing;
  tailRing.reserve(kTailWindow);
  size_t tailNext = 0;
  std::vector<float> tailSorted;
  // Adaptive fog (plan phase 3B): the queue is drained by whole planes, so the
  // trusted radius jumps in steps. Start at the cold-start ceiling (nothing is
  // filled yet at this point — FullRefill above just queued everything) and
  // ease outward as the bands land.
  float fogSmooth = kFarFogDensityMax;

  // --frames N harness (phase 4b D3 verification): run N frames windowed,
  // fire one F5 shader reload midway (the Tint recompile path), then close
  // cleanly through the normal shutdown — so "window opens, world renders,
  // reload works, clean exit" is checkable without a human at the keyboard.
  // ---- THE WORLD A TICK RUNS IN (game/session.h TickAuthorityCtx) ---------
  //
  // Five labelled sections: the engine, the content, the PRESENTATION seam the
  // tick body still writes to, the per-WORLD tick scratch it owns, and the
  // argv-owned harness hooks. Built once and reused every tick; only
  // `frameTime` is rewritten per frame, because `now` is a frame-loop local
  // and a reference to it would be the frame layer keeping state for the
  // authority.
  TickAuthorityCtx tickCtx{
      .ctx = ctx,
      .world = world,
      .sim = sim,
      .stream = stream,
      .far = far,
      .phys = phys,
      .mobs = mobs,
      .debris = debris,
      .mbSet = mbSet,
      .mats = mats,
      .glyphs = glyphs,
      .items = items,
      .prefabs = prefabs,
      .classOf = classOf,
      .ui = ui,
      .hitStop = hitStop,
      .deathBody = deathBody,
      .deathFrozen = deathFrozen,
      .liveTimed = liveTimed,
      .liveTimer = liveTimer,
      .liveSample = liveSample,
      .liveFrameNo = liveFrameNo,
      .tickMsSmooth = tickMsSmooth,
      .farEntries = g_farEntries,
      .farTicks = g_farTicks,
      .farBiggest = g_farBiggest,
      .labScene = labScene,
  };
  // ---- CONVERSATIONS (game/dialogue.h, PLAN_world_editor P3) -------------
  // One store per world: the loaded assets/dialogue/*.json, the flags and the
  // met set (saved as 'DLGF' in world.sve). R reloads the files and keeps the
  // flags; the Spawn page's Dialogue section lists what is wrong with them.
  dialogue::Store talkStore;
  talkStore.dir = assetDir + "/dialogue";
  talkStore.items = &items;
  auto reloadDialogue = [&]() {
    talkStore.Reload();
    ui.dialogueNames.clear();
    for (const dialogue::Dialogue& d : talkStore.lib.All())
      ui.dialogueNames.push_back(d.name);
    ui.dialogueProblems.clear();
    for (const dialogue::Problem& p : talkStore.problems)
      ui.dialogueProblems.push_back(p.Line());
    char buf[96];
    std::snprintf(buf, sizeof buf, "%zu conversation(s) loaded", talkStore.lib.All().size());
    ui.dialogueStatus = buf;
  };
  reloadDialogue();
  tickCtx.talk = &talkStore;
  // The per-WORLD scratch main() still reads, aliased back out of the ctx the
  // same way the player's members are aliased out of `session`.
  auto& sphereModels = tickCtx.sphereModels;
  bool& everExploded = tickCtx.everExploded;
  uint32_t& fluidCount = tickCtx.fluidCount;
  auto& fluidPendingSpawns = tickCtx.fluidPendingSpawns;
  uint32_t& fluidCueMat = tickCtx.fluidCueMat;
  uint32_t& labTick = tickCtx.labTick;
  // Water's id, resolved once, and species 0's splash material with it, so the
  // fluid tool pours water without an explicit key press.
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == "water") { fluidCueMat = (uint32_t)i; break; }
  // The mpm tool's keys 1-4 pour these liquids, resolved BY NAME (a missing
  // one falls back to water in the pour). The particle's material is its whole
  // identity -- colour, splash, density -- so this table is all "which liquid"
  // means; there is no species.
  {
    static const char* kPourNames[4] = {"water", "oil", "acid", "blood"};
    for (int k = 0; k < 4; k++)
      for (size_t i = 0; i < mats.size(); i++)
        if (mats[i].name == kPourNames[k]) {
          tickCtx.fluidPourMat[k] = (uint32_t)i;
          break;
        }
  }
  tickCtx.duelDummy = g_duelDummy;

  // HOW THIS SESSION READS THE WORLD, bound ONCE rather than rebuilt as a
  // lambda every frame. Player::KindFn was always a std::function precisely so
  // a per-player collision source could be plugged in; now it can be.
  //
  // Excited MPM water folds into the liquid answer BEFORE the voxel mirror:
  // swimming, buoyancy and the waterline frame work identically whichever
  // representation the water happens to be in (plan 6.5). The >= 2 eighths
  // floor keeps a lone stray droplet from reading as a pool.
  session.kindAt = [&world, &classOf](IVec3 c) {
    if (world.FluidEighthsAt(c) >= 2) return CellKind::Liquid;
    return world.KindAt(c, classOf);
  };
  const Player::KindFn& kindAt = session.kindAt;
  // The player stands ON grains, not on the cell they sit in: a partial
  // powder cell is only mass/8 tall (docs/PLAN_powder_mass.md P4).
  session.player.cellTop = [&world, &classOf](IVec3 c) {
    return world.CellTopAt(c, classOf);
  };

  // ---- THE HARNESS HOOKS (session.h section E) ----------------------------
  // All three are argv state the FRAME layer owns; passing them as callbacks
  // is what keeps game/session.cpp from linking against this file's globals.
  if (g_autoflySurface) {
    tickCtx.afterPlayerUpdate = [](Player& pl, uint32_t tk) {
      // --autofly-surface altitude pin. Held analytically against the worldgen
      // heightfield rather than flown, so the measured quantity (ray length
      // through unskipped chunks) depends only on the tick schedule. Fly mode
      // has no terrain collision, so assigning the position outright is legal
      // here; vel.y is zeroed so the integrator does not carry an accumulated
      // climb into the next frame and fight this every step.
      const int gh = World::TerrainHeight((int)std::floor(pl.pos.x),
                                          (int)std::floor(pl.pos.z),
                                          kDefaultSeed);
      pl.pos.y = (float)gh + (g_autoflySurfaceHigh ? kAutoflySurfaceHighVox
                                                   : kAutoflySurfaceLowVox);
      pl.vel.y = 0.0f;
      // Park: latch the pose once and re-assign it every frame. Zeroing the
      // input axis is not enough on its own -- fly mode integrates velocity,
      // so a coast of even a few voxels crosses a chunk boundary and shifts
      // the window, which is precisely the stimulus under test.
      if (g_autoflyPark && tk >= ParkFlyTicks()) {
        if (!g_parkPosSet) {
          // SANDVOX_PARK_AT wins over the route's endpoint, and it is read
          // AFTER the surface-follow above rewrote pos.y: the whole point of
          // naming a place is that the probe holds THAT altitude, which over a
          // lake is not TerrainHeight's.
          if (!ParkAtPos(g_parkPos)) g_parkPos = pl.pos;
          g_parkPosSet = true;
          std::printf("park: stopped at (%.1f, %.1f, %.1f) on tick %u\n",
                      g_parkPos.x, g_parkPos.y, g_parkPos.z, tk);
        }
        pl.pos = g_parkPos;
        pl.vel = Vec3{0, 0, 0};
        pl.SnapRender();   // a pin is a teleport: do not lerp into it
      }
    };
  }
  if (g_fellTree) {
    // ---- --fell-tree: the tree-fell gate's cut, in the live frame loop ----
    // Same fixture (selftest::BuildTree), same cut (a 9x9x3 slab of air ten
    // cells up plus the destruction event the brush would raise), same
    // 300-tick profile window. What differs is that this loop RENDERS. Here
    // rather than in the tick body because it reads g_frameMs, which is the
    // frame layer's own whole-frame ring.
    tickCtx.fellTree = [&world, &mats, &debris, &player, &cam, &phys, &treeAtlas](
                           uint32_t tick, std::vector<CellOp>& cellOps) {
      static int fellPhase = 0;  // 0 waiting, 1 planted, 2 cut, 3 reported
      static uint32_t fellPlantTick = 0, fellCutTick = 0;
      static int fellGroundY = 0;
      static size_t fellFrame0 = 0;
      static bool fellBodySeen = false;
      static bool fellTreeSeen = false;
      static uint32_t fellTreeTick = 0;
      static selftest::TreeFixture fellTree;
      auto matByName = [&](const char* n) -> uint32_t {
        for (size_t i = 0; i < mats.size(); i++)
          if (mats[i].name == n) return (uint32_t)i;
        return 0u;
      };
      // SANDVOX_FELL_SPLIT=<ticks>: that long after the tree becomes a body,
      // split the largest body across its longest axis, twice, 45 ticks
      // apart -- the "one big body becomes several" half of the report --
      // and profile the 300 ticks after the first split like the fall.
      static const int splitAfter = [] {
        const char* e = std::getenv("SANDVOX_FELL_SPLIT");
        return e ? std::max(1, std::atoi(e)) : 0;
      }();
      if ((tick % 60u) == 0u)
        std::printf("--fell-tree: tick %u player (%.1f,%.1f,%.1f) yaw %.2f fly %d\n",
                    tick, player.pos.x, player.pos.y, player.pos.z, cam.yaw,
                    player.fly ? 1 : 0);
      static bool fellCutWide = false;
      static int fellCutR = 4;
      if (fellPhase == 0 && tick >= (uint32_t)g_fellTreeAt &&
          std::getenv("SANDVOX_FELL_SPECIES") != nullptr) {
        // SANDVOX_FELL_SPECIES=<name>[:variant]: the BAKED tree instead of the
        // fixture -- the atlas's own voxels (TreeAtlasCellAt, the shader's
        // column/run path), stamped where the fixture would stand, over as
        // many ticks as the op cap needs. What a player actually cuts down.
        static std::vector<CellOp> stamp;
        static size_t stampAt = 0;
        static bool stampBuilt = false;
        if (!stampBuilt) {
          stampBuilt = true;
          std::string want = std::getenv("SANDVOX_FELL_SPECIES");
          int variant = 0;
          if (const size_t colon = want.find(':'); colon != std::string::npos) {
            variant = std::atoi(want.c_str() + colon + 1);
            want.resize(colon);
          }
          int sp = -1;
          for (int i = 0; i < (int)treeAtlas.species.size(); i++)
            if (treeAtlas.species[i].name == want) sp = i;
          if (sp < 0) {
            std::printf("--fell-tree: no species '%s' in the atlas\n", want.c_str());
            fellPhase = 3;
            return;
          }
          using namespace treeatlas;
          const uint32_t* W = treeAtlas.words.data();
          const uint32_t* sd = W + W[kHSpeciesDir] + sp * kSpeciesWords;
          variant = std::min(variant, (int)sd[kSVariantCount] - 1);
          const uint32_t* d = W + sd[kSVariantDir] + variant * kVariantWords;
          const int nx = (int)d[kVNx], ny = (int)d[kVNy], nz = (int)d[kVNz];
          const int ax = (int)d[kVAnchorX], az = (int)d[kVAnchorZ];
          const Vec3 fwd = cam.Forward();
          const int bx = ifloor(player.pos.x + fwd.x * 48.0f);
          const int bz = ifloor(player.pos.z + fwd.z * 48.0f);
          fellGroundY = World::TerrainHeight(bx, bz, kDefaultSeed);
          size_t wood = 0, other = 0;
          int trunkR = 0;
          for (int lz = 0; lz < nz; lz++)
            for (int ly = 0; ly < ny; ly++)
              for (int lx = 0; lx < nx; lx++) {
                const uint32_t m = TreeAtlasCellAt(treeAtlas, sp, variant, lx, ly, lz);
                if (m == 0) continue;
                const IVec3 cc{bx + lx - ax, fellGroundY + 1 + ly, bz + lz - az};
                if (!world.CellInWindow(cc)) continue;
                stamp.push_back({World::SlotCellIndex(cc),
                                 PackVoxNew(m, (uint32_t)(lx * 7 + ly * 3 + lz) % 3u)});
                const bool isWood = m < mats.size() &&
                    (mats[m].name.find("wood") != std::string::npos ||
                     mats[m].name.find("bark") != std::string::npos);
                (isWood ? wood : other)++;
                if (isWood && ly == 11)
                  trunkR = std::max(trunkR, std::max(std::abs(lx - ax), std::abs(lz - az)));
              }
          fellTree.base = IVec3{bx, fellGroundY + 1, bz};
          fellTree.lo = IVec3{bx - ax, fellGroundY + 1, bz - az};
          fellTree.hi = IVec3{bx - ax + nx - 1, fellGroundY + ny, bz - az + nz - 1};
          fellTree.woodCells = (uint32_t)wood;
          fellTree.leafCells = (uint32_t)other;
          fellCutWide = true;
          fellCutR = std::min(40, trunkR + 3);
          std::printf("--fell-tree: SPECIES %s variant %d: %dx%dx%d, %zu wood + %zu "
                      "other, trunk half-width %d at +11, stamped at (%d,%d,%d)\n",
                      want.c_str(), variant, nx, ny, nz, wood, other, trunkR, bx,
                      fellGroundY + 1, bz);
        }
        while (stampAt < stamp.size() && cellOps.size() < kMaxCellOpsPerTick)
          cellOps.push_back(stamp[stampAt++]);
        if (stampAt < stamp.size()) return;
        fellPlantTick = tick;
        fellPhase = 1;
        std::fflush(stdout);
      } else if (fellPhase == 0 && tick >= (uint32_t)g_fellTreeAt) {
        const Vec3 fwd = cam.Forward();
        const int bx = ifloor(player.pos.x + fwd.x * 48.0f);
        const int bz = ifloor(player.pos.z + fwd.z * 48.0f);
        fellGroundY = World::TerrainHeight(bx, bz, kDefaultSeed);
        fellTree = selftest::BuildTree(world, {bx, fellGroundY + 1, bz},
                                       matByName("wood"),
                                       matByName("leaves"), cellOps);
        fellPlantTick = tick;
        fellPhase = 1;
        // A crown that touches a hillside is anchored, and the cut then
        // frees nothing: say what the ground does under the box.
        int hillMax = fellGroundY;
        for (int z = fellTree.lo.z; z <= fellTree.hi.z; z++)
          for (int x = fellTree.lo.x; x <= fellTree.hi.x; x++)
            hillMax = std::max(hillMax, World::TerrainHeight(x, z, kDefaultSeed));
        std::printf("--fell-tree: ground under the box rises to y%d (trunk foot "
                    "y%d, crown from y%d)\n", hillMax, fellGroundY + 1,
                    fellGroundY + 1 + fellTree.height - 6 - fellTree.crownR / 2);
        std::printf("--fell-tree: planted at (%d,%d,%d) tick %u: %u wood + %u "
                    "leaves, box (%d,%d,%d)..(%d,%d,%d)\n",
                    bx, fellGroundY + 1, bz, tick, fellTree.woodCells,
                    fellTree.leafCells, fellTree.lo.x, fellTree.lo.y,
                    fellTree.lo.z, fellTree.hi.x, fellTree.hi.y, fellTree.hi.z);
        std::fflush(stdout);
      } else if (fellPhase == 1 && tick >= fellPlantTick + 120) {
        const int fx = fellTree.base.x, fz = fellTree.base.z;
        const int cutY = fellGroundY + 11;
        const int cr = fellCutWide ? fellCutR : 4;
        for (int y = cutY; y < cutY + 3; y++)
          for (int dz = -cr; dz <= cr; dz++)
            for (int dx = -cr; dx <= cr; dx++) {
              const IVec3 cc{fx + dx, y, fz + dz};
              if (!world.CellInWindow(cc)) continue;
              if (cellOps.size() < kMaxCellOpsPerTick)
                cellOps.push_back({World::SlotCellIndex(cc), 0u});
            }
        debris.AddDestructionEvent(tick, {fx - cr - 1, fellGroundY + 10, fz - cr - 1},
                                   {fx + cr + 1, fellGroundY + 15, fz + cr + 1});
        debris.SetProfiling(true);
        debris.ResetProfile();
        debris.ResetFloaterProbe();
        world.ResetFetchProbe();
        fellCutTick = tick;
        fellFrame0 = g_frameMs.size();
        fellPhase = 2;
        std::printf("--fell-tree: cut at tick %u (frame %zu)\n", tick,
                    fellFrame0);
        std::fflush(stdout);
      } else if (fellPhase == 2 && !fellTreeSeen && [&] {
                   for (uint32_t b = 0; b < debris.BodyCount(); b++)
                     if (debris.BodyVoxelCount(b) >= 1000u) return true;
                   return false;
                 }()) {
        fellTreeSeen = true;
        uint32_t big = 0;
        for (uint32_t b = 0; b < debris.BodyCount(); b++)
          big = std::max(big, debris.BodyVoxelCount(b));
        std::printf("--fell-tree: the TREE is a body at tick %u (+%u after the cut), "
                    "%u vox\n", tick, tick - fellCutTick, big);
        std::fflush(stdout);
      } else if (fellPhase == 2 && debris.BodyCount() > 0 && !fellBodySeen) {
        fellBodySeen = true;
        std::printf("--fell-tree: first body at tick %u (+%u after the cut), "
                    "%u vox\n", tick, tick - fellCutTick,
                    debris.BodyVoxelCount(0));
        std::fflush(stdout);
      } else if (fellPhase == 2 && tick >= fellCutTick + 300) {
        fellPhase = 3;
        const DebrisSystem::FloaterProbe& fp = debris.Floaters();
        std::printf("--fell-tree: probe: scans %u, oversize-bbox %u, "
                    "deferred-oversize %u, defer-gave-up %u (fetch %u), "
                    "deferred-unfetched %u, fetch-wait no-cache %u no-vox %u, "
                    "anchored boundary %u unknown %u oversize-flood %u, "
                    "stuck-dropped %u, queue-full-dropped %u | last give-up: chunk "
                    "(%d,%d,%d) inWin %u cached %u, seed (%d,%d,%d)..(%d,%d,%d)\n",
                    fp.scans, fp.oversizeBboxSkipped, fp.deferredOversize,
                    fp.deferGaveUp, fp.deferGaveUpFetch, fp.deferredUnfetched,
                    fp.fetchWaitNoCache, fp.fetchWaitNoVoxels,
                    fp.anchoredByRegionBoundary, fp.anchoredByUnknownChunk,
                    fp.anchoredByOversizeFlood, fp.stuckEventDropped,
                    fp.eventQueueFullDropped, fp.gaveUpChunk.x, fp.gaveUpChunk.y,
                    fp.gaveUpChunk.z, fp.gaveUpChunkInWindow, fp.gaveUpChunkCached,
                    fp.gaveUpSeedLo.x, fp.gaveUpSeedLo.y, fp.gaveUpSeedLo.z,
                    fp.gaveUpSeedHi.x, fp.gaveUpSeedHi.y, fp.gaveUpSeedHi.z);
        uint32_t bodies = debris.BodyCount(), vox = 0;
        for (uint32_t b = 0; b < bodies; b++) vox += debris.BodyVoxelCount(b);
        std::vector<double> win(g_frameMs.begin() + (ptrdiff_t)fellFrame0,
                                g_frameMs.end());
        std::sort(win.begin(), win.end());
        auto pct = [&](double p) {
          return win.empty() ? 0.0 : win[(size_t)(p * (win.size() - 1))];
        };
        size_t over33 = 0;
        for (double m : win) if (m > 33.0) over33++;
        std::printf("--fell-tree: FALL over 300 ticks / %zu frames: whole-frame "
                    "ms p50 %.1f p95 %.1f p99 %.1f max %.1f, >33ms %zu; bodies "
                    "%u holding %u vox; COST %s | %s\n",
                    win.size(), pct(0.5), pct(0.95), pct(0.99),
                    win.empty() ? 0.0 : win.back(), over33, bodies, vox,
                    debris.ProfileReport().c_str(),
                    world.FetchReport().c_str());
        std::fflush(stdout);
        if (!std::getenv("SANDVOX_DEBRIS_PROFILE")) debris.SetProfiling(false);
      }
      // THE WORST PHYSICS STEP, per 60 ticks while anything is a body: a
      // whole-frame max says a frame was slow, this says Jolt was the reason.
      if (fellPhase >= 2 && (tick % 60u) == 0u) {
        std::printf("--fell-tree: tick %u (+%u) bodies %u, worst Jolt Update "
                    "since last line %.1f ms\n",
                    tick, tick - fellCutTick, debris.BodyCount(),
                    phys.Runaway().worstStepMs);
        phys.ResetRunawayProbe();
      }
      if (fellTreeSeen && fellTreeTick == 0) fellTreeTick = tick;
      static int splits = 0;
      static uint32_t splitTick0 = 0, lastSplitTick = 0;
      static size_t splitFrame0 = 0;
      if (splitAfter > 0 && fellTreeTick != 0 && splits < 2 &&
          tick >= fellTreeTick + (uint32_t)splitAfter &&
          (splits == 0 || tick >= lastSplitTick + 45)) {
        uint32_t bi = UINT32_MAX, big = 0;
        for (uint32_t b = 0; b < debris.BodyCount(); b++)
          if (debris.BodyVoxelCount(b) > big) {
            big = debris.BodyVoxelCount(b);
            bi = b;
          }
        Vec3 lmn{}, lmx{};
        BodyTransform bxf{};
        const uint64_t h = bi != UINT32_MAX ? debris.BodyHandle(bi) : 0;
        if (h != 0 && phys.GetLocalBounds(h, lmn, lmx) && phys.GetTransform(h, bxf)) {
          // Across the body's own longest LOCAL axis, through its middle.
          const float ext[3] = {lmx.x - lmn.x, lmx.y - lmn.y, lmx.z - lmn.z};
          int ax = 0;
          for (int a = 1; a < 3; a++)
            if (ext[a] > ext[ax]) ax = a;
          auto rot = [&](Vec3 v) {
            const Vec3 u{bxf.quat[0], bxf.quat[1], bxf.quat[2]};
            const Vec3 t = u.cross(v) * 2.0f;
            return v + t * bxf.quat[3] + u.cross(t);
          };
          const Vec3 c = bxf.pos + rot(Vec3{0.5f * (lmn.x + lmx.x),
                                            0.5f * (lmn.y + lmx.y),
                                            0.5f * (lmn.z + lmx.z)});
          const Vec3 n = rot(Vec3{ax == 0 ? 1.0f : 0.0f, ax == 1 ? 1.0f : 0.0f,
                                  ax == 2 ? 1.0f : 0.0f});
          // THE BLADE'S COST ON THIS BODY FIRST: eight kerfs across the
          // split plane, each timed, the way a sword stroke's probes land on a
          // log (melee.cpp ResolveOnLooseMatter -> CutBody). Shallow slots,
          // so they carve without parting it and the split below still has
          // the whole body to work on.
          if (splits == 0) {
            std::vector<ParticleSpawn> sp;
            double worst = 0, tot = 0;
            for (int k = 0; k < 8; k++) {
              KerfCut kc;
              kc.at = c + Vec3{0.0f, 0.5f * (float)k, 0.0f};
              kc.edgeAxis = Vec3{n.z, 0.0f, -n.x}.len() > 0.1f
                                ? Vec3{n.z, 0.0f, -n.x}.normalized()
                                : Vec3{1.0f, 0.0f, 0.0f};
              kc.cutDir = n;
              kc.depth = 1.5f;
              kc.halfWidth = 0.3f;
              kc.length = 3.0f;
              kc.seed = (uint32_t)k;
              const auto tk = std::chrono::steady_clock::now();
              debris.CutBody(debris.BodyHandle(bi), kc, world, sp);
              const double ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - tk).count();
              worst = std::max(worst, ms);
              tot += ms;
            }
            std::printf("--fell-tree: 8 CutBody kerfs on the %u-vox body: %.2f ms "
                        "total, worst %.2f, bodies now %u\n", big, tot, worst,
                        debris.BodyCount());
          }
          const auto t0 = std::chrono::steady_clock::now();
          const bool ok = debris.SplitBody(debris.BodyHandle(bi), c, n);
          const double ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count();
          std::printf("--fell-tree: SPLIT %d of body %u (%u vox) across axis %d "
                      "at tick %u: %s in %.2f ms, bodies now %u\n",
                      splits, bi, big, ax, tick, ok ? "ok" : "REFUSED", ms,
                      debris.BodyCount());
          if (splits == 0) {
            splitTick0 = tick;
            splitFrame0 = g_frameMs.size();
          }
          lastSplitTick = tick;
          splits++;
          std::fflush(stdout);
        } else {
          splits = 2;  // nothing left to split
        }
      }
      if (splitTick0 != 0 && tick == splitTick0 + 300) {
        std::vector<double> win(g_frameMs.begin() + (ptrdiff_t)splitFrame0,
                                g_frameMs.end());
        std::sort(win.begin(), win.end());
        auto pct = [&](double p) {
          return win.empty() ? 0.0 : win[(size_t)(p * (win.size() - 1))];
        };
        size_t over33 = 0;
        for (double m : win) if (m > 33.0) over33++;
        std::printf("--fell-tree: SPLIT window 300 ticks / %zu frames: whole-frame "
                    "ms p50 %.1f p95 %.1f p99 %.1f max %.1f, >33ms %zu; bodies %u\n",
                    win.size(), pct(0.5), pct(0.95), pct(0.99),
                    win.empty() ? 0.0 : win.back(), over33, debris.BodyCount());
        std::fflush(stdout);
      }
      // SANDVOX_FELL_IGNITE=<ticks>: that long after the tree becomes a body,
      // scatter fire (IfAir) through every body's world AABB so the pieces
      // burn — the "any body's payload changes every tick" half of the
      // rigidbody render cost (instance rebuild + upload per burning tick).
      // Fixed lattice, so the seed set is the same every run.
      static const int igniteAfter = [] {
        const char* e = std::getenv("SANDVOX_FELL_IGNITE");
        return e ? std::atoi(e) : 0;
      }();
      static bool ignited = false;
      if (igniteAfter > 0 && !ignited && fellTreeTick != 0 &&
          tick >= fellTreeTick + (uint32_t)igniteAfter) {
        ignited = true;
        uint32_t fire = matByName("fire");
        size_t seeded = 0;
        // Every 24th collider voxel's own cell (air in the grid: the body is
        // not in it), so the fire sits against the wood it is meant to light.
        // SANDVOX_FELL_IGNITE_ONE=1: light only the SMALLEST piece, so the
        // other bodies stay unchanged while one flags every tick — the case
        // per-body instance caching exists for.
        uint32_t only = UINT32_MAX;
        if (std::getenv("SANDVOX_FELL_IGNITE_ONE")) {
          uint32_t best = UINT32_MAX;
          for (uint32_t b = 0; b < debris.BodyCount(); b++)
            if (debris.BodyVoxelCount(b) < best) {
              best = debris.BodyVoxelCount(b);
              only = b;
            }
        }
        for (uint32_t b = 0; b < debris.BodyCount(); b++) {
          if (only != UINT32_MAX && b != only) continue;
          Vec3 w{};
          for (uint32_t k = 0; only == UINT32_MAX && debris.BodyVoxelWorld(b, k, w); k += 24) {
            const IVec3 cc{ifloor(w.x), ifloor(w.y), ifloor(w.z)};
            if (!world.CellInWindow(cc)) continue;
            if (cellOps.size() >= kMaxCellOpsPerTick) break;
            cellOps.push_back({World::SlotCellIndex(cc), fire | kCellOpIfAir});
            seeded++;
          }
          // ...and embers IN the lattice (the corpse-crossheat fixture's way
          // of lighting a body), so it burns whether or not the world's fire
          // reaches it before going out.
          const uint64_t h = debris.BodyHandle(b);
          const uint32_t ember = matByName("ember");
          for (const char* fuel : {"leaves", "autumn_leaves", "birch_wood", "wood"})
            if (const uint32_t fm = matByName(fuel); fm && ember)
              seeded += debris.RewriteBodyMaterial(h, fm, ember, 64);
        }
        std::printf("--fell-tree: IGNITE at tick %u: %zu fire seeds over %u bodies\n",
                    tick, seeded, debris.BodyCount());
        std::fflush(stdout);
      }
    };
  }
  if (g_forestFire && !g_fellTree) {
    // ---- --forest-fire (see g_forestFire). Borrows the fell-tree slot: it is
    // the frame layer's one cell-op hook, and the two harnesses are exclusive.
    tickCtx.fellTree = [&world, &mats, &debris, &mobs, &player, &sim](
                           uint32_t tick, std::vector<CellOp>& cellOps) {
      static std::vector<CellOp> seed;
      static size_t seedAt = 0;
      static bool built = false;
      if (g_forestFireDone) return;
      // NOT BEFORE THE HORIZON EXISTS. The far pipelines compile on background
      // threads and any worldgen.wgsl edit makes that a minute or more; a
      // fire measured before `fardown` exists is measured without it (it
      // happened twice on 2026-09-22 and read as a 15 ms win). Ignition slides
      // to 60 ticks after they are ready, and every later phase with it.
      if (!built && (uint32_t)g_forestFireAt <= tick && !sim.FarPipelinesReady()) {
        g_forestFireAt = (int)tick + 60;
        return;
      }
      const uint32_t t0 = (uint32_t)g_forestFireAt;
      if (tick < t0) return;
      if (!built) {
        built = true;
        uint32_t fire = 0;
        for (size_t i = 0; i < mats.size(); i++)
          if (mats[i].name == "fire") { fire = (uint32_t)i; break; }
        const int px = ifloor(player.pos.x), pz = ifloor(player.pos.z);
        // A jittered 9-voxel grid of columns, 12..100 voxels out. Jitter is a
        // fixed hash of the column so the seed set is the same every run.
        for (int gz = -100; gz <= 100; gz += 9)
          for (int gx = -100; gx <= 100; gx += 9) {
            const uint32_t h = (uint32_t)(gx * 73856093) ^ (uint32_t)(gz * 19349663);
            const int x = px + gx + (int)(h % 5u) - 2;
            const int z = pz + gz + (int)((h >> 8) % 5u) - 2;
            const int d2 = (x - px) * (x - px) + (z - pz) * (z - pz);
            if (d2 < 12 * 12 || d2 > 100 * 100) continue;
            const int g = World::TerrainHeight(x, z, kDefaultSeed);
            for (int y : {g + 1, g + 2, g + 10, g + 18, g + 26, g + 34, g + 42}) {
              const IVec3 c{x, y, z};
              if (!world.CellInWindow(c)) continue;
              seed.push_back({World::SlotCellIndex(c), fire | kCellOpIfAir});
            }
          }
        // SANDVOX_FOREST_FIRE_CONTROL=1: the same run, camera and schedule
        // with no fire -- the control arm every per-pass number is read against.
        if (std::getenv("SANDVOX_FOREST_FIRE_CONTROL")) seed.clear();
        std::printf("--forest-fire: tick %u, %zu fire seeds round (%d,%d)\n",
                    tick, seed.size(), px, pz);
        std::fflush(stdout);
      }
      while (seedAt < seed.size() && cellOps.size() < kMaxCellOpsPerTick)
        cellOps.push_back(seed[seedAt++]);
      if (tick % 60u == 0u) {
        const WorldSnapshot& sn = world.Snap();
        std::printf("--forest-fire: tick %u (+%u) active %u particles %u bodies %u\n",
                    tick, tick - t0, sn.valid ? sn.activeChunks : 0u,
                    sn.valid ? sn.particleCount : 0u, debris.BodyCount());
        std::fflush(stdout);
      }
      // The fire has had 20 s. Every --frames series starts over here, so the
      // exit report is the burning forest and nothing before it.
      if (tick == t0 + 600u) {
        g_frameMs.clear();
        g_activeChunks.clear();
        for (int i = 0; i < sandvox::kPerfScopeCount; i++) {
          g_frameScopeSum[i] = 0;
          g_frameScopeMax[i] = 0;
          g_frameScopeSeries[i].clear();
        }
        for (int n = 0; n < sandvox::kPerfNodeCount; n++) g_frameGpuSeries[n].clear();
        g_frameGpuPassSeries.clear();
        g_frameGpuFrames = 0;
        // The debris/terrain phase profile over the same window: the CPU
        // table's terrainMesh / debris rows are the whole of it, and a row
        // is a sum -- this says which phase of it.
        debris.SetProfiling(true);
        debris.ResetProfile();
        mobs.ResetBurnStats();  // the body-reaction evaluator's cost split
        std::printf("--forest-fire: MEASURING from tick %u\n", tick);
        std::fflush(stdout);
      }
      // SANDVOX_FOREST_FIRE_PROBE=1: WHAT the awake chunks are (depth band and
      // material mix vs an idle control), via the park probe's sampler. Off by
      // default: it pulls ~380 chunks through the fetch ring mid-measurement.
      static const bool probe = std::getenv("SANDVOX_FOREST_FIRE_PROBE") != nullptr;
      if (probe && tick == t0 + 900u) ParkSampleRequest(world, "fire");
      if (probe && tick == t0 + 940u) ParkSampleReport(world, mats, "fire");
      if (tick >= t0 + 1200u) {
        std::printf("--forest-fire: debris profile over the measured window: %s\n",
                    debris.ProfileReport().c_str());
        // WHERE burnBodies' time goes, from the one evaluator's own counters
        // (every body population goes through MobSystem::BurnOneLimb).
        const MobSystem::BurnStats& bs = mobs.Burn();
        std::printf("--forest-fire: body-reaction evaluator over the window: "
                    "%llu visits (%llu loose debris, %llu of them deferred by "
                    "the walk pot), %llu asleep, %llu world cells walked, %llu "
                    "index builds over %llu cells, %u candidates evaluated\n",
                    (unsigned long long)bs.visits,
                    (unsigned long long)bs.looseVisits,
                    (unsigned long long)bs.walkDeferred,
                    (unsigned long long)bs.sleeps,
                    (unsigned long long)bs.walkCells,
                    (unsigned long long)bs.indexBuilds,
                    (unsigned long long)bs.indexCells, bs.candidates);
        std::fflush(stdout);
        g_forestFireDone = true;
      }
    };
  }
  // Respawn out of an open inventory hands the cursor back to the window:
  // `captureBeforeUi`, glfwSetInputMode and the cursor-position reset are all
  // the WINDOW's, and there is no window on the authority side.
  tickCtx.restoreCursorAfterUi = [&session, &captureBeforeUi, window, &mx0,
                                  &my0]() {
    session.captured = captureBeforeUi;
    glfwSetInputMode(window, GLFW_CURSOR,
                     session.captured ? GLFW_CURSOR_DISABLED
                                      : GLFW_CURSOR_NORMAL);
    glfwGetCursorPos(window, &mx0, &my0);
  };

  // THE TICK'S OPS, owned by the frame layer. A server loop would forward the
  // batch; this loop submits it inside TickAuthority. Hoisted out of the tick
  // so the vectors keep their capacity across ticks.
  OpBatch opBatch;

  // ================== THE GAME TALKS TO ONE PEER (M9.2-C) ===================
  //
  // docs/PLAN_multiplayer_m9.md M9.2, package C. Packages A and B built the
  // two halves this block joins: `net::TcpLink` + the message set + the
  // `LockstepPacer` (src/net/), and the `RemotePlayer` ghost the four guarded
  // seams of `TickAuthority` already know how to walk (game/remoteplayer.h).
  // Nothing here is new mechanism; it is the WIRING, and the wiring is where
  // the three rules of the model get honoured or broken:
  //
  //  1. NO `TickInput` CROSSES THE WIRE. What goes out is the OUTCOME of a
  //     tick — one `PlayerState` — because the peer is authoritative for its
  //     own controller and this machine has no collision source for a body
  //     twelve chunks away (M9.1 P3 proved exactly that).
  //  2. SEND BEFORE WAIT. At local tick T the batch labelled T+D goes out, and
  //     only then does T+1 wait for the peer's batch labelled T+1. Both sides
  //     doing the reverse is the deadlock of §4 finding 5. The pacer states
  //     this as three invariants and the ONLY send site below is the
  //     `while (pacer.ShouldSend())` loop, so the pre-send at connect and the
  //     steady state are literally the same code.
  //  3. A BATCH EVERY TICK, EMPTY OR NOT. "No ops" and "not arrived" have to
  //     be different observations. There are no ops until M9.3, so today every
  //     batch is a header plus 108 bytes of `PlayerState` — and it still goes
  //     out on every single tick.
  //
  // ZERO COST WHEN NOBODY IS NETWORKED. Every line below is behind
  // `netRoleBoot != NetRole::None`, `tickCtx.remotes` stays null, and the one
  // in-loop test on the tick path is a `bool`. That is the package's
  // acceptance criterion, not a micro-optimisation: the `--record-ops` stream
  // of a plain `--frames 600 --autofly-hard` run has to be BYTE-IDENTICAL to
  // the pre-package oracle, and a single extra op or a single reordered one
  // would show up as a differing byte.
  enum class NetRole { None, Host, Client };
  const NetRole netRoleBoot = netHost      ? NetRole::Host
                              : !netJoinIp.empty() ? NetRole::Client
                                                   : NetRole::None;
  // BY POINTER, and that is load-bearing. `LinkBase::Fail` LATCHES: once a
  // peer drops, `failed_` is set for the life of the object, `Poll()` returns
  // immediately and `Send` refuses — there is no Reset in package A's API. A
  // listen server that goes back to listening after its player quits
  // therefore needs a FRESH TcpLink, and re-seating a unique_ptr is how a
  // caller gets one without a one-line addition to net/link.h.
  std::unique_ptr<net::TcpLink> link;
  net::LockstepPacer pacer;
  // The peers' bodies. Borrowed by TickAuthorityCtx below; cleared on
  // disconnect and at shutdown, because a ghost owns real Jolt bodies.
  RemotePlayers remotes;
  // THE OP EXCHANGE (M9.3-B, net/opsync.h). Borrowed by TickAuthorityCtx
  // below. Connected() is false until the handshake completes, and while it
  // is false phase N submits exactly the vectors the tick built — which is
  // why a plain --frames run's op record is byte-identical to the oracle.
  net::OpSync opsync;
  // THE CONVERGENCE HALF (M9.3-C, net/chunksync.h). Borrowed by
  // TickAuthorityCtx below so phase B can install what arrives; everything
  // else it does is socket work and happens in `netPump` / `netChunkPump`.
  // Connected() is false until the handshake completes, and while it is false
  // nothing here sends a byte or touches a chunk.
  net::ChunkSync chunksync;
  // ---- THE ENTITY HALF (M9.4-D, net/entitysync.h) ----------------------
  //
  // NOT borrowed by TickAuthorityCtx, and that is deliberate: everything it
  // does happens at the FRAME layer, around the tick rather than inside it.
  // The handoff scan must run before `MobSystem::PreTick` (which is where
  // `RefreshOwnership` turns a creature into a ghost and after which
  // `TakeHandoff` would refuse it), the apply must run before the tick that
  // carries the label, and the build must run after that tick — three points
  // the frame loop already stands at, and none of which is inside a phase.
  //
  // What DOES reach the tick is the pair of ownership CLOSURES it hands to
  // MobSystem and DebrisSystem at connect. Those are null until then, which
  // is this package's whole hash argument: with no function bound both
  // systems' RefreshOwnership returns on its first line and the tick is
  // byte-for-byte the one the `--record-ops` oracle recorded.
  net::EntitySync entities;
  // ---- THE PERSISTENCE HALF (M9.5-B, net/storesync.h) -------------------
  //
  // NOT borrowed by TickAuthorityCtx either, but for a different reason than
  // EntitySync's: it is not called by the frame layer at all. It is a
  // `Stream::ChunkExchange`, so `Stream` itself calls it — from inside the
  // tick, at the eviction sites and at the refill miss — and it is reached by
  // main only to bind it, to feed it the two peer views, to hand it messages
  // and to pump it.
  //
  // THE HASH ARGUMENT IS THE BINDING AND NOTHING ELSE. `Stream`'s exchange
  // pointer is null in every single-player run, every gate and both smokes,
  // and with it null `Stream` makes exactly the calls it made before M9.5-A
  // (sim/stream.h says so). The `netStoreBind` lambda below is the ONLY place
  // it is ever set, and it is inside `netStartPacing`.
  net::StoreSync storesync;
  // Store telemetry for the exit report, harvested the same way the op, sync
  // and entity totals are — `Disconnect()` keeps its counters (they are
  // diagnostics), so `netDrop` does NOT fold them in and the exit report
  // reads them live. Stated here because the three neighbours above do the
  // opposite and the asymmetry would otherwise look like an oversight.
  //
  // Entity telemetry for the exit report, accumulated HERE and not read off
  // the object at exit — `Disconnect()` clears its counters, so a host that
  // outlives its peer would report zeroes for work it really did. Same
  // harvest-first discipline as the op and chunk-sync totals below.
  net::EntitySync::Counters netEnt{};
  uint32_t netGhostsMax = 0;
  // Chunk-sync telemetry for the exit report, accumulated HERE for the same
  // reason the op counters are: Disconnect() clears the state machine, so a
  // host that outlives its peer would report zeroes for work it really did.
  // Folded in by netDrop.
  struct NetSyncTotals {
    uint64_t blocksSent = 0, blocksRecv = 0, mismatches = 0, drills = 0;
    uint64_t requests = 0, applied = 0, busy = 0, notResident = 0;
    uint64_t chunkMismatches = 0, bytes = 0;
    std::vector<net::ChunkSync::MismatchPoint> series;
  } netSync;
  // Op telemetry for the exit report, accumulated HERE and not read off the
  // queue at exit: Disconnect() resets the queue, so a host that outlives its
  // peer would report zeroes for work it really did (measured -- the first
  // smoke's host printed mergedMax=0 after a session that merged plenty).
  // The last two are folded in from the queue inside netDrop.
  uint64_t netOpsSent = 0, netOpsRecv = 0;
  // M9.3-C: how many local-only divergences SANDVOX_NET_SMOKE_DRIFT injected.
  // The denominator of the smoke's claim: "4 syncs applied" means nothing
  // without "4 drifts injected" beside it.
  uint64_t netDriftOps = 0;
  uint64_t netCellsDropped = 0, netMergedMax = 0;
  // ---- THE SILENCE TIMER MEASURES POLLED TIME, NOT WALL TIME -----------
  //
  // M9.2-C measured `NowSeconds() - Stats().lastRecvSeconds > 3 s` and that
  // is wrong across a FOREGROUND STALL: a pipeline compile, a worldgen, a
  // save or an F5 reload can hold this thread for many seconds, during which
  // wall time passes, nothing polls the socket, and the peer's batches sit
  // unread in the kernel buffer. The link is perfectly healthy and the timer
  // declares it dead the instant the stall ends — the smoke's mid-run shader
  // reload is exactly such a stall.
  //
  // So silence is accumulated ACROSS FRAMES THAT POLLED, and a frame whose
  // own duration exceeded kNetPollStallSeconds contributes NOTHING: this
  // machine was not listening, so its silence says nothing about the peer.
  // The 3 s budget then means "three seconds of a running frame loop with
  // nothing arriving", which is the condition the rule was always about.
  double netSilence = 0.0;          // polled seconds since the last byte
  double netSilenceLastPoll = 0.0;  // NowSeconds() at the previous netPump
  uint64_t netPollStalls = 0;       // frames excluded as foreground stalls
  // ---- ...AND THE PEER'S STALL IS NOT VISIBLE AT ALL -------------------
  //
  // The polled-time rule above fixes the half of the bug this machine can
  // see. The other half it cannot: a peer that is inside a multi-second
  // foreground operation is not sending, and from here that is
  // indistinguishable from a peer that died. MEASURED, first two-process
  // smoke of M9.3-B: the client printed `render pipelines built in 48.21 s`
  // on its first frame (a cold pipeline set for a ground-level view; the host
  // had been rendering for a minute and had them all), during which it polled
  // nothing -- and the host dropped it four ticks after the handshake with
  // "3 s silence" while both processes were perfectly healthy.
  //
  // So the silence RULE does not run until the connection has had time to get
  // through both machines' first frame. 90 s is twice the measured worst
  // compile and far below the 120 s the host already waits for a peer to
  // arrive at all. A broken SOCKET still drops instantly inside the grace --
  // that is an answer from the OS, not an inference from silence.
  constexpr double kNetConnectGraceSeconds = 90.0;
  double netPacedAt = 0.0;          // NowSeconds() at the handshake
  // Handshake complete and the pacer owns the tick gate. Distinct from
  // `link->Connected()`: a TCP connection with no HelloAck behind it must not
  // stall the world, and a host is playing normally long before anyone joins.
  bool netPaced = false;
  // Has a peer EVER completed the handshake? Distinct from `netPaced`, which
  // goes back to false on a disconnect. Only the --frames budget reads it.
  bool netEverPaced = false;
  // The host is player 0 and the client player 1, assigned by the HelloAck.
  // `MakePlayerState` fills `playerId` from the SESSION INDEX, which is 0 on
  // both machines, so the wire id is stamped over it at send time — otherwise
  // both ghosts would key on 0 and the client would upsert a ghost for itself.
  uint32_t netMyId = 0, netPeerId = 1;

  // ---- WHAT ARRIVED, BY LABEL, NOT YET CONSUMED -------------------------
  //
  // A GHOST MUST NOT JUMP AHEAD. Batches run D ticks in front of the tick
  // being simulated (that is what D buys), so the `PlayerState` in the batch
  // labelled T+D describes a body D ticks in the peer's future relative to
  // the world this machine is about to step. Applying it on arrival would put
  // the ghost four ticks ahead of every local body it can collide with and
  // make it visibly rubber-band. So states are QUEUED by label and applied in
  // the tick whose label they carry, and the map is pruned as ticks run.
  //
  // std::map and not unordered_map: it never holds more than D+1 entries, and
  // the ordered erase-up-to-and-including-T below is one call on a map and a
  // scan on a hash table.
  struct NetPending {
    PlayerState st{};
    bool haveState = false;
    float timeScale = 1.0f;   // the SENDER's, applied on the client only
    uint32_t vizActive = 0;
  };
  std::map<uint32_t, NetPending> netQueue;

  // ---- MESSAGES THE BOOT JOIN SAW BUT IS NOT ALLOWED TO EAT (M9.5-B) ----
  //
  // The client's late join drains the socket before the frame loop exists,
  // waiting for the full `ChunkManifest`. TCP preserves order and the host
  // sends that manifest before its pre-send batches, so in practice nothing
  // else is in front of it — but "in practice" is not a guarantee, and the
  // consequence of getting it wrong is silent: a dropped pre-send `TickBatch`
  // is a hole in the pacer's contiguity that shows up as a stall and a
  // non-zero `late=` in the exit report, which is the number that is supposed
  // to mean "the label arithmetic is wrong".
  //
  // So anything the join loop pulls out that is not a store message is PARKED
  // here and `netPump` drains it first, in arrival order, through the same
  // switch it would have gone through. `Link::Recv` has no peek and no
  // put-back; this deque is that put-back.
  std::deque<net::Msg> netEarly;

  // ---- THE PEER'S LATEST APPLIED STATE, AS THE AUTHORITY SEES IT (M9.4-D)
  //
  // `net::PeerView` needs two facts about the peer that only its
  // `PlayerState` carries: its feet chunk and its window origin. This is that
  // state, copied at the moment it is APPLIED (not at the moment it arrives),
  // so the authority arithmetic runs on the same snapshot of the peer the
  // local ghost body is standing at. Applying it on arrival would compute
  // ownership from a peer four ticks in its own future, which on a walking
  // player is a different set of chunks and would re-home entities early.
  PlayerState netPeerLast{};
  bool netHavePeerLast = false;

  // ---- ...AND MY OWN, AT THE SAME TICK. THE PAIR MUST BE SYMMETRIC -----
  //
  // MEASURED, the first two-process smoke of this package: the host reported
  // `handoffs out=6` while the client reported `handoffs in=3` and
  // `misses=144` — 144 poses for creatures the client believed were already
  // its own. Both machines were simulating the same three humans at once,
  // which is the single-producer rule (§4 finding 4) broken in the open.
  //
  // THE CAUSE WAS NOT THE AUTHORITY ARITHMETIC. It was what the two machines
  // fed it. A peer's `PlayerState` is D+1 ticks stale by construction (that
  // is what D BUYS), so at local tick N the host was computing from
  //   (host NOW, client at N-D-1)
  // and the client, symmetrically, from
  //   (client NOW, host at N-D-1).
  // Those are different question, and "ownership is DERIVED, so both
  // machines get the same answer and there is no claim message anywhere in
  // the protocol" (authority.h) is simply FALSE under them. The 2-chunk
  // hysteresis absorbs it while both players walk; `--autofly-hard` crosses
  // 32 voxels in well under five ticks, so the smoke flew straight through
  // the margin and the disagreement became visible.
  //
  // THE FIX IS TO AGE MY OWN VIEW TO MATCH. This ring holds what this
  // machine's `PeerView` WAS at each recent tick; the lookup key is the
  // peer's `st.tick`, which is the tick the peer's state describes. Both
  // machines then compute from
  //   (host at N-D-1, client at N-D-1)
  // — the same two facts, in the same order, and the answers are identical
  // by arithmetic rather than by luck. A stale pair is not a problem here:
  // authority is about WHICH MACHINE STEPS A THING, and agreeing on a
  // five-tick-old answer is strictly better than disagreeing on a fresh one.
  //
  // std::map and not a deque: it is looked up by tick, it never holds more
  // than the prune below leaves in it, and the ordered erase-up-to is one
  // call. The depth is generous (four times D+1) because the peer's state
  // can be older than D+1 when a batch was late.
  std::map<uint32_t, net::PeerView> netMyRing;
  constexpr uint32_t kNetMyRingDepth = 4 * (net::kOpDelayTicks + 1);
  auto netNoteMyView = [&](uint32_t t) {
    net::PeerView v{};
    v.playerId = netMyId;
    v.chunk = {ifloor(session.player.pos.x) >> 4,
               ifloor(session.player.pos.y) >> 4,
               ifloor(session.player.pos.z) >> 4};
    v.windowOrigin = world.WindowOrigin();
    v.connected = true;
    netMyRing[t] = v;
    while (netMyRing.size() > kNetMyRingDepth)
      netMyRing.erase(netMyRing.begin());
  };

  // Rebuilt every tick from the ring + `netPeerLast` and handed to the
  // EntitySync, which is the only thing that reads it.
  auto netRefreshPeerViews = [&]() {
    net::PeerView mine{};
    mine.playerId = netMyId;
    mine.chunk = {ifloor(session.player.pos.x) >> 4,
                  ifloor(session.player.pos.y) >> 4,
                  ifloor(session.player.pos.z) >> 4};
    mine.windowOrigin = world.WindowOrigin();
    mine.connected = true;
    // The ring entry for the tick the peer's state describes. Absent only in
    // the first few ticks after connect (nothing has been recorded yet), when
    // the fresh view above is the only thing there is and the peer has not
    // spoken either — so nothing is decided from the mismatched pair.
    if (netHavePeerLast) {
      const auto ri = netMyRing.find(netPeerLast.tick);
      if (ri != netMyRing.end()) mine = ri->second;
    }
    if (!netHavePeerLast) {
      entities.SetPeers(mine, nullptr);
      // M9.5-B: the SAME pair, to the same arithmetic. StoreSync asks
      // `net::ChunkAuthority` "may I put this chunk" and "does the peer own
      // it", and if it were fed a different (fresher, or staler) view than
      // EntitySync then the two machines could disagree about who speaks for
      // a chunk — the exact failure the netMyRing comment above documents
      // measuring, transplanted from mobs to persistence. One call site, one
      // pair, both consumers.
      storesync.SetPeers(mine, nullptr);
      return;
    }
    net::PeerView theirs{};
    theirs.playerId = netPeerId;
    theirs.chunk = {ifloor(netPeerLast.pos.x) >> 4,
                    ifloor(netPeerLast.pos.y) >> 4,
                    ifloor(netPeerLast.pos.z) >> 4};
    theirs.windowOrigin = {netPeerLast.windowOrigin[0],
                           netPeerLast.windowOrigin[1],
                           netPeerLast.windowOrigin[2]};
    theirs.connected = true;
    entities.SetPeers(mine, &theirs);
    storesync.SetPeers(mine, &theirs);  // M9.5-B; see the null branch above
  };

  // Counters for the HUD and for the `--frames` exit report.
  uint64_t netBatchesSent = 0, netBatchesRecv = 0, netStalls = 0;
  uint64_t netLate = 0, netDisconnects = 0;
  int netMaxLag = 0;
  // Byte totals SURVIVE the link. A disconnect re-seats the unique_ptr with a
  // fresh TcpLink whose stats start at zero, so the exit report would
  // otherwise only ever describe the last connection.
  uint64_t netBytesInPrev = 0, netBytesOutPrev = 0;

  // THE HANDSHAKE'S IDENTITY: every fact that must be identical for two
  // machines to simulate the same world. Built from what this file already
  // printed at boot — the environment stamp, the tuning stamp — plus the seed
  // and the material table's hash, which is the SAME hash the op record's
  // header refuses a replay on. Everything else (kWorldN, kChunk,
  // kVoxelMeters, sizeof(TickParams), the protocol/record/tickinput versions)
  // is compiled in by `net::LocalHello`, which is the point: those are exactly
  // the fields a hand-filled Hello gets subtly wrong on one side.
  auto netLocalHello = [&](uint32_t myId) {
    return net::LocalHello(
        (uint32_t)kDefaultSeed, (uint32_t)mats.size(),
        sandvox::opstream::MaterialTableHash(mats), tuneStamp.tuning,
        tuneStamp.materials, tuneStamp.reactions, envStamp.map,
        envStamp.biomes, envStamp.trees, envStamp.mapName, myId);
  };

  // ---- M9.3-C: THE ONLY PLACE A SYNC MESSAGE REACHES THE SOCKET ---------
  //
  // `net::ChunkSync` produces payloads and never sends them, for the same
  // reason `LockstepPacer` never touches a clock: it keeps the whole
  // convergence protocol drivable by a gate with no link at all. This is the
  // three lines that close the gap.
  auto netSendSync = [&](const std::vector<net::ChunkSync::Out>& msgs) {
    if (!link) return;
    for (const net::ChunkSync::Out& o : msgs)
      link->Send((uint16_t)o.type, net::kProtocolVersion, o.payload.data(),
                 o.payload.size());
  };

  // ---- ONE OUTGOING BATCH -----------------------------------------------
  //
  // `label` is the tick the PEER will run when it consumes this; the
  // `PlayerState` inside is the FRESHEST outcome this machine has, i.e. the
  // tick it just finished. Those are deliberately different numbers. Sending
  // a state stamped with the label would be inventing a position four ticks
  // into our own future; sending the newest state under a future label is the
  // honest statement "by the time you run T+D, this is where I was at T", and
  // it is what makes D a JITTER BUDGET rather than a prediction.
  auto netSendBatch = [&](uint32_t label) {
    if (!link) return;
    net::TickBatchWire w;
    w.h.tick = label;
    w.h.playerId = netMyId;
    const IVec3 wo = world.WindowOrigin();
    w.h.windowOrigin[0] = wo.x;
    w.h.windowOrigin[1] = wo.y;
    w.h.windowOrigin[2] = wo.z;
    // The two HASHED presentation inputs (session.cpp phase L11): the
    // celestial time scale and the dirty-voxel viz. They ride the batch so the
    // host's clock is the world's clock — see the client-side apply below.
    w.h.timeScale = ui.timeScale;
    w.h.vizActive = ui.showDirtyVoxels ? 1u : 0u;
    PlayerState st = MakePlayerState(session, tick, wo);
    st.playerId = netMyId;
    w.playerState.resize(sizeof(PlayerState));
    std::memcpy(w.playerState.data(), &st, sizeof st);
    // ---- M9.3-B: AND THE OPS FOR THAT LABEL -------------------------
    //
    // The batch this machine authored at `label - kOpLabelAhead` and stored
    // under `label` (session.cpp phase N). Empty for the first few ticks
    // after connect, and empty on most ticks of a normal game — which is fine
    // and is the point of invariant 2: a batch goes out every tick either
    // way, so "no ops" and "not arrived" stay different observations.
    //
    // THE ORIGIN INSIDE THE BLOB IS NOT THE HEADER'S. The header carries the
    // sender's window origin NOW; the blob carries the origin the ops were
    // PRODUCED under, five ticks ago, because a CellOp's slot index is only
    // interpretable under that one (net/opsync.h point 3). A fly-through can
    // shift the window inside D ticks, so the two genuinely differ and using
    // the header's would paint the wrong chunk.
    {
      IVec3 opsOrigin{0, 0, 0};
      const OpBatch& ob = opsync.Q().Outgoing(label, &opsOrigin);
      const size_t n = ob.ops.size() + ob.exps.size() + ob.cells.size() +
                       ob.spawns.size() + ob.fluid.size();
      if (n == 0) opsOrigin = wo;   // nothing to interpret; say something sane
      netOpsSent += n;
      net::OpsWire::Encode(ob, opsOrigin, label, w.ops);
    }
    std::vector<uint8_t> buf;
    w.Encode(buf);
    link->Send((uint16_t)net::MsgType::TickBatch, net::kProtocolVersion,
               buf.data(), buf.size());
    netBatchesSent++;
    // ---- M9.4-D: AND THE ENTITIES FOR THE SAME LABEL -------------------
    //
    // A SEPARATE MESSAGE, SENT AFTER THE BATCH, carrying the SAME label. Two
    // reasons it is not a third blob inside `TickBatchWire`:
    //
    //  1. SIZE. A `TickBatch` is ~150 B and the pacer blocks on it; an
    //     entity batch carrying a handoff carries a whole per-mob save
    //     record (kilobytes) and two voxel lattices. Putting them in one
    //     frame would make the peer's tick gate wait on a payload that has
    //     nothing to do with whether the tick may run — §4 finding 8's
    //     head-of-line argument, applied to the one message that must never
    //     be delayed.
    //  2. ABSENCE IS FINE. Invariant 2 ("a batch every tick, empty or not")
    //     is about the PACER: "no ops" and "not arrived" must differ. No such
    //     rule binds entities — nothing waits on them — so an empty entity
    //     batch is simply not sent, which is most ticks of most games.
    //
    // Built here rather than in the tick loop because the pre-send at connect
    // calls this D+1 times before the first tick runs, and the send site is
    // the one place that knows the label each of those carries.
    {
      net::EntityBatch eb;
      entities.Build(mobs, debris, label, eb);
      if (!eb.Empty()) {
        std::vector<uint8_t> ebuf;
        eb.Encode(ebuf);
        link->Send((uint16_t)net::MsgType::EntityBatch, net::kProtocolVersion,
                   ebuf.data(), ebuf.size());
      }
    }
  };

  // ---- CONNECT: RESET THE PACER AND LET IT DO THE PRE-SEND ---------------
  //
  // There is NO separate pre-send path, by design (protocol.h invariant 3):
  // `Reset` puts `sent` behind by D+1 and the same `while (ShouldSend())` loop
  // the tick path uses emits T0..T0+D. A hand-written "send D batches" here
  // would be a second implementation of the send rule and would drift from it
  // the first time D moved.
  auto netStartPacing = [&](uint32_t startTick) {
    pacer.Reset(startTick);
    netQueue.clear();
    netMyRing.clear();   // M9.4-D: a tick label from a previous session
                         // names a different moment; see netMyRing.
    netHavePeerLast = false;
    // The op exchange starts with the pacer and on the same tick numbering: a
    // label from a previous session means nothing in this one.
    opsync.Connect(netMyId, netPeerId);
    // ...and the convergence half, on the same clock and for the same reason:
    // an origin-ring entry or a half-reassembled chunk from a previous
    // connection names a tick that no longer exists.
    chunksync.Connect(netMyId, netPeerId);
    // ---- M9.4-D: AND THE ENTITY HALF -----------------------------------
    //
    // THE ORDER IN THIS BLOCK IS LOAD-BEARING, top to bottom:
    //
    //  1. `SetIdBand` / `SetLocalPlayerId` FIRST, before anything can spawn.
    //     A mob id and a debris global id are the keys every record below is
    //     addressed to, and both machines mint them from a monotonic counter
    //     that starts at 1 — so without the band the client's first creature
    //     and the host's first creature are the same creature as far as the
    //     wire is concerned. The host is player 0 and keeps today's
    //     numbering, which is why a single-player save is unchanged.
    //
    //     HONEST LIMIT: bodies that existed BEFORE the join keep
    //     `ownerAtCreate = 0` (debris.h says SetLocalPlayerId does not
    //     re-mint them, and cannot — those ids may already be out on the
    //     wire from a previous connection). On a client that joins a world it
    //     has been playing alone in, those bodies sit in the host's band. It
    //     is a real collision hazard and the fix is M9.5's late-join reset,
    //     not a re-mint here; the smoke joins at boot, so nothing predates it.
    //
    //  2. The ownership closures, which are what make `RefreshOwnership` in
    //     both systems stop being a no-op. From this line on, a mob or a body
    //     can be somebody else's.
    //
    //  3. The item callbacks, which let a peer's `ItemTake` be answered out
    //     of THIS machine's ground registry (worlditems.h says why they are
    //     handed down rather than the layering being inverted).
    entities.Connect(netMyId, netPeerId);
    mobs.SetIdBand(netMyId);
    mobs.SetLocalPlayerId(netMyId);
    debris.SetLocalPlayerId(netMyId);
    mobs.SetOwnershipFn([&entities](uint64_t id, Vec3 feet) {
      return entities.MobOwner(id, feet);
    });
    debris.SetOwnershipFn(
        [&entities](Vec3 pos) { return entities.BodyOwner(pos); });
    debris.SetChunkOwnedFn(
        [&entities](IVec3 wc) { return entities.ChunkOwned(wc); });
    // ---- M9.4-E: THE ID-CARRYING CLOSURES, BOUND AT CONNECT -----------
    //
    // The two `SetOwnershipFn` calls above install the POSITION-ONLY forms,
    // and `DebrisSystem`'s falls back to a CHUNK-keyed incumbent — one
    // hysteresis slot shared by every body standing in the chunk, which is
    // the flapping case entitysync.h describes. `BindOwnership` replaces
    // both with the body-id-keyed versions. `ScanHandoffs` already calls it
    // idempotently every tick, so this is the EARLY binding and not a
    // behaviour change: it just means the first tick after the handshake
    // gets the right closures instead of the second.
    entities.BindOwnership(mobs, debris);
    debris.SetItemLookupFn(ground.LookupFn());
    debris.SetItemTakeFn([&debris](uint64_t h) {
      // The same call the local E key makes. The registry entry is dropped by
      // the release hook when the body goes, so this is one call, not two.
      return debris.DestroyBody(h);
    });
    // ---- M9.5-B: AND THE PERSISTENCE HALF ------------------------------
    //
    // `Connect` seats the link, the role and MY store, and re-offers every
    // put the previous connection never got an ack for (net/storesync.h: the
    // unacked list deliberately survives a disconnect). The delivery pair is
    // `Stream`'s own two doors.
    //
    // WHERE `deliver_` FIRES, and why that is a legal place to install a
    // chunk: it is called from inside `netPump`'s dispatch, which runs ONCE
    // per frame BEFORE the tick loop. `Stream::ReplaceChunk`'s contract asks
    // for "the phase-B position, before stream.Update and therefore before
    // the tick's submit", and the top of the frame is strictly earlier than
    // that — its deferred WriteBuffers are ordered before the next submit,
    // which is the same guarantee FillSlots relies on.
    storesync.Connect(link.get(), netRoleBoot == NetRole::Host,
                      &stream.Store(), netMyId, netPeerId);
    storesync.SetDelivery(
        [&stream](IVec3 wc, uint32_t t, const std::vector<uint32_t>& rle) {
          stream.DeliverRemote(wc, t, rle);
        },
        [&stream](IVec3 wc) { stream.DeliverMiss(wc); });
    if (netRoleBoot == NetRole::Host) {
      // THE HOST BINDS IMMEDIATELY: it needs no manifest (its own store IS
      // the manifest) and its `Wanted` is arithmetic, not a table.
      stream.SetChunkExchange(&storesync);
      // ...and sends the whole thing, in <= 64 KiB slices, BEFORE the first
      // TickBatch goes out below. Plan step 3: the client must know what the
      // host holds before its own window is re-pulled, or it will regenerate
      // pristine terrain over the other player's edits.
      const size_t rows = storesync.SendFullManifest();
      std::printf("net: sent chunk manifest: %zu rows\n", rows);
      std::fflush(stdout);
    }
    // The CLIENT binds later, in the join block, once the manifest has
    // actually arrived — see "THE LATE JOIN" there. Binding here would let a
    // `Wanted` be answered from an empty mirror, which is the one wrong
    // answer (it regenerates over an edit and cannot be undone).
    netSilence = 0.0;
    netSilenceLastPoll = 0.0;
    netPacedAt = net::NowSeconds();
    netPaced = true;
    netEverPaced = true;
    while (pacer.ShouldSend()) {
      const uint32_t t = pacer.NextToSend();
      netSendBatch(t);
      pacer.NoteSent(t);
    }
    link->Poll();   // push the pre-send out now, not a frame from now
  };

  // ---- LOSE THE PEER: FREE-RUN, AND KEEP NOTHING OF IT -------------------
  auto netDrop = [&](const char* why) {
    netDisconnects++;
    std::printf("net: peer lost at tick %u (%s) -- free-running\n", tick, why);
    std::fflush(stdout);
    // The avatar list MobSystem resolves an actor id through is INDEX-ALIGNED
    // with the ghost list, and `RemotePlayers::Clear` cannot fix it: phase H's
    // re-sync is guarded on a NON-EMPTY ghost list, so clearing to empty would
    // leave MobSystem holding a pointer to a PlayerAvatar that no longer
    // exists. Restore the single-avatar registration this file made at boot.
    remotes.Clear(phys);
    mobs.SetAvatar(&session.avatar);
    netQueue.clear();
    netMyRing.clear();
    netHavePeerLast = false;
    netPaced = false;
    // ---- M9.4-D: NOTHING MAY FREEZE WHEN THE PEER GOES ------------------
    //
    // Every ghost in this world is a creature or a body that ANOTHER machine
    // was stepping. With the peer gone nothing poses them, and a ghost mob is
    // a fourth PreTick branch that runs no AI and a ghost body is a KINEMATIC
    // Jolt body — so left alone they stand and hang exactly where they were,
    // forever, un-fightable and un-pushable. That is the worst possible
    // outcome of a disconnect: the world looks intact and half of it is
    // furniture.
    //
    // So every ghost is PROMOTED TO LOCAL at its last pose, which the plan
    // describes as "a handoff to self from the announce + last pose" and
    // which is spelled here as an ownership function that answers `me` for
    // everything. That is not a shortcut around the handoff path — it IS the
    // handoff path: `RefreshOwnership` is what both systems consult, it runs
    // at the top of the very next PreTick, and `MakeOwned` / the local branch
    // do exactly what a received handoff's tail does (flip to dynamic, drop
    // the pose latch, resume stepping from where the body stands).
    //
    // WHY NOT JUST CLEAR THE FUNCTIONS. A null `ownershipFn_` makes
    // `RefreshOwnership` return on its FIRST LINE (mob.cpp, debris.cpp), so
    // every existing ghost keeps `owner_ != local` forever and the freeze is
    // exactly the bug above. The always-me function costs one call per entity
    // per tick in a process that has already lost its peer, and it is the
    // only form of "everything is mine again" that the two systems can act
    // on. It stays bound for the life of the process rather than being
    // cleared a tick later: a one-tick state machine here would be a second
    // rule to get wrong, and the two functions are behaviourally identical to
    // null once every entity is local.
    {
      const uint32_t me = netMyId;
      mobs.SetOwnershipFn([me](uint64_t, Vec3) { return me; });
      debris.SetOwnershipFn([me](Vec3) { return me; });
      debris.SetChunkOwnedFn(nullptr);   // every chunk is mine to scan again
    }
    netEnt = [&] {
      // Harvest FIRST, then Disconnect: it clears the counters, and a listen
      // server that loses one player and gains another would otherwise report
      // only the second one's work.
      const net::EntitySync::Counters& s = entities.Stats();
      net::EntitySync::Counters t = netEnt;
      t.announcesOut += s.announcesOut;   t.announcesIn += s.announcesIn;
      t.posesOut += s.posesOut;           t.posesIn += s.posesIn;
      t.handoffsOut += s.handoffsOut;     t.handoffsIn += s.handoffsIn;
      t.statesOut += s.statesOut;         t.statesIn += s.statesIn;
      t.posesAsleep += s.posesAsleep;
      t.gonesOut += s.gonesOut;           t.gonesIn += s.gonesIn;
      t.takesOut += s.takesOut;           t.takesIn += s.takesIn;
      t.grantsOut += s.grantsOut;         t.grantsIn += s.grantsIn;
      t.batchesOut += s.batchesOut;       t.batchesIn += s.batchesIn;
      t.applyMisses += s.applyMisses;     t.expired += s.expired;
      t.ghostsMax = std::max(t.ghostsMax, s.ghostsMax);
      return t;
    }();
    netGhostsMax = std::max(netGhostsMax, netEnt.ghostsMax);
    // RESET AFTER HARVESTING, and this line is not optional bookkeeping.
    // `Disconnect()` deliberately does NOT clear the counters (they are
    // diagnostics, not connection state), so without this the exit report's
    // `netEnt + Stats()` adds the same work twice. MEASURED: the first smoke
    // of this package printed the host's every entity number at exactly 2x
    // the client's matching number — announces 6/3, poses 288/144, batches
    // 98/49 — and the perfect 2:1 across five unrelated fields was the only
    // clue that it was a reporting defect and not a protocol one.
    entities.ResetStats();
    entities.Disconnect();
    // The op exchange goes with the peer. Its counters are harvested FIRST:
    // Disconnect() clears the queue, and the delay queue's labels are this
    // session's and no other -- a rejoin restarts the host's clock, and a
    // stale label would merge a batch from the previous connection into a
    // tick of the new one.
    netCellsDropped += opsync.Q().droppedCells;
    netMergedMax = std::max(netMergedMax, opsync.Q().mergedMax);
    opsync.Disconnect();
    // M9.3-C: harvest FIRST, then clear. Same hazard the op counters hit —
    // Disconnect() wipes the state machine, and a listen server that loses one
    // player and gains another would otherwise report only the second one's
    // work. The mismatch SERIES is concatenated rather than summed: it is the
    // shape that says whether the repair converged, and a total cannot.
    netSync.blocksSent += chunksync.hashBlocksSent;
    netSync.blocksRecv += chunksync.hashBlocksRecv;
    netSync.mismatches += chunksync.blockMismatches;
    netSync.chunkMismatches += chunksync.chunkMismatches;
    netSync.drills += chunksync.drillsSent;
    netSync.requests += chunksync.requestsSent;
    netSync.applied += chunksync.syncsApplied;
    netSync.busy += chunksync.busyRecv;
    netSync.notResident += chunksync.syncsRefusedNotResident;
    netSync.bytes += chunksync.bytesShipped;
    netSync.series.insert(netSync.series.end(), chunksync.series.begin(),
                          chunksync.series.end());
    chunksync.Disconnect();
    chunksync.series.clear();
    // ---- M9.5-B: THE STORE EXCHANGE GOES WITH THE PEER -----------------
    //
    // TWO STEPS, AND THE ORDER MATTERS. `Disconnect()` first, because it is
    // what answers every outstanding `ChunkGet` with a local MISS — a held
    // slot waits INDEFINITELY (sim/stream.h) and a disconnect is not an
    // answer, so without this the chunks that were in flight when the peer
    // vanished would read as air for the rest of the process and nothing
    // would ever re-ask. Those misses go through `stream.DeliverMiss`, which
    // needs the exchange to still be the bound one for its bookkeeping to
    // line up, hence unbinding SECOND.
    //
    // Unlike the three counter blocks above, nothing is harvested here:
    // `StoreSync::Disconnect` deliberately keeps its counters AND its unacked
    // put list AND its manifest mirror, because a rejoin in the same process
    // re-offers the first and re-receives the second.
    storesync.Disconnect();
    stream.SetChunkExchange(nullptr);
    // ---- M9.5-B: --net-smoke-persist -----------------------------------
    //
    // THE SAVE HAPPENS HERE AND NOT AT EXIT, and that is the whole point of
    // hanging it on the disconnect: under SANDVOX_NET_SMOKE_EXIT_ON_PEER_DONE
    // this function also closes the window, so "the world as it was when the
    // two players were done with it" and "the world at exit" are the same
    // moment — but only this one is reachable before the frame loop starts
    // tearing things down. `SaveWorld` flushes the resident window into the
    // store and then flushes the store, so the client's `ChunkPut`s are
    // already in it: they were put there by `StoreSync::OnMessage`, not by
    // anything the save has to know about.
    if (netSmokePersist) {
      ctx.WaitIdle();
      const PlayerKitRefs kitRefs = PlayerKitOf(session, glyphs, items);
      WorldItemRefs groundRefs{&ground, &phys, &debris, &mbSet, &items};
      EntityIO eio = MakeEntityIO(debris, mobs, &avatar, &kitRefs, &groundRefs, nullptr, &talkStore);
      WorldStamp stamp{tick, (uint32_t)kDefaultSeed, true};
      if (SaveWorld(ctx, world, stream, "build/smoke_world.svd", mats, &eio,
                    stamp)) {
        std::vector<std::pair<IVec3, uint32_t>> rows;
        stream.Store().Manifest(rows);
        size_t tagged = 0;
        for (const auto& [wc, tg] : rows)
          if (tg != 0) tagged++;
        std::printf("net-smoke-persist: saved build/smoke_world.svd at tick %u "
                    "-- store %zu chunks, %zu tick-tagged\n",
                    tick, rows.size(), tagged);
        std::fflush(stdout);
      }
    }
    netSilence = 0.0;
    netSilenceLastPoll = 0.0;
    pacer = net::LockstepPacer{};
    if (netRoleBoot == NetRole::Host) {
      // BACK TO LISTENING ON THE SAME SOCKET (M9.3-B). M9.2-C had to throw
      // the whole TcpLink away here because `Fail` latched with no reset,
      // and re-binding is the one part of that which can genuinely fail --
      // there is no SO_REUSEADDR (net/link.cpp Listen says why), so a host
      // that lost a player could end up with no port at all. `Reset()`
      // un-latches the failure and drops the dead peer's half-received
      // bytes; the listen socket was never closed, so accepting resumes with
      // no re-bind and the port cannot be lost. The stats keep accumulating
      // across peers, which is why the exit report no longer carries a
      // per-connection carry-over on this path.
      link->Reset();
      std::printf("net: listening again on 127.0.0.1:%u\n",
                  (unsigned)link->ListenPort());
    } else {
      // A client has nothing to re-listen for. Its byte totals are folded
      // into the carry-over before the link goes, or the exit report would
      // lose them.
      netBytesInPrev += link->Stats().bytesIn;
      netBytesOutPrev += link->Stats().bytesOut;
      link.reset();
    }
    // The two-process smoke's full stop; see NetSmokeExitOnPeerDone().
    if (NetSmokeExitOnPeerDone() && g_harnessFrames > 0 && window)
      glfwSetWindowShouldClose(window, 1);
  };

  // ---- THE ONE SOCKET TOUCH OF THE FRAME --------------------------------
  //
  // Accept / read / flush, then drain every fully framed message. Called ONCE
  // per frame and BEFORE the tick loop, so a batch that arrived while we were
  // rendering unblocks this frame's ticks instead of next frame's — with D=4
  // at 30 Hz there is only ~133 ms of slack and a frame of added latency eats
  // a quarter of it.
  auto netPump = [&]() {
    if (!link) return;
    link->Poll();
    net::Msg m;
    // THE PARKED ONES FIRST, IN ARRIVAL ORDER (M9.5-B; see `netEarly`). The
    // loop below pops from the deque until it is empty and only then starts
    // reading the socket, so a pre-send batch the join loop had to pull out
    // of the way is applied before anything that arrived after it.
    for (;;) {
      if (!netEarly.empty()) {
        m = std::move(netEarly.front());
        netEarly.pop_front();
      } else if (!link->Recv(m)) {
        break;
      }
      switch ((net::MsgType)m.type) {
        // ---- HOST SIDE OF THE HANDSHAKE ---------------------------------
        case net::MsgType::Hello: {
          if (netRoleBoot != NetRole::Host || netPaced) break;
          net::Hello theirs;
          if (!theirs.Decode(m.payload.data(), m.payload.size())) break;
          const net::Hello mine = netLocalHello(0);
          if (const char* bad = net::Hello::FirstMismatch(mine, theirs)) {
            // NAME THE FIELD. A refusal carrying only "incompatible" is what
            // makes a user re-copy their whole assets directory to fix a
            // tuning.json. Flush it before the peer goes away.
            net::HelloRefuse r{bad};
            std::vector<uint8_t> b;
            r.Encode(b);
            link->Send((uint16_t)net::MsgType::HelloRefuse,
                       net::kProtocolVersion, b.data(), b.size());
            link->Poll();
            std::printf("net: refusing peer: %s\n", bad);
            std::fflush(stdout);
            break;
          }
          // THE HOST'S TICK COUNTER IS THE CLOCK. `startTick` is the next tick
          // this loop will run, so both machines' first paced tick has the
          // same number and every batch label means the same thing on both.
          netMyId = 0;
          netPeerId = 1;
          net::HelloAck ack;
          ack.startTick = tick + 1;
          ack.yourPlayerId = netPeerId;
          std::vector<uint8_t> b;
          ack.Encode(b);
          link->Send((uint16_t)net::MsgType::HelloAck, net::kProtocolVersion,
                     b.data(), b.size());
          netStartPacing(ack.startTick);
          std::printf("net: peer joined as player %u, start tick %u\n",
                      netPeerId, ack.startTick);
          std::fflush(stdout);
          break;
        }
        // ---- THE STEADY STATE -------------------------------------------
        case net::MsgType::TickBatch: {
          if (!netPaced) break;
          net::TickBatchWire w;
          if (!w.Decode(m.payload.data(), m.payload.size())) break;
          netBatchesRecv++;
          pacer.NotePeerBatch(w.h.tick);
          netMaxLag = std::max(netMaxLag, pacer.Lag());
          // A LABEL WE HAVE ALREADY RUN CANNOT BE APPLIED. `pacer.localTick`
          // is the tick about to run, so anything below it describes a world
          // state this machine has already stepped past; applying it would
          // teleport the ghost backwards. Counted rather than silently
          // dropped: with TCP and a correct pre-send this number must stay 0,
          // and a non-zero `late=` in the exit report is the pacing bug's
          // first symptom.
          if (w.h.tick < pacer.localTick) {
            netLate++;
            break;
          }
          // ---- M9.3-B: THE PEER'S OPS FOR THIS LABEL --------------------
          //
          // Straight into the delay queue, which is keyed by the SAME label
          // the PlayerState below is queued under -- but consumed one layer
          // deeper: the state is applied by this loop before the tick, the
          // ops by phase N inside it. `origin` comes out of the blob (the
          // origin they were PRODUCED under), never out of the header.
          //
          // A blob that fails to decode is dropped whole and counted as a
          // late batch: applying half a peer's tick would be a divergence
          // with no symptom until the next hash comparison.
          if (!w.ops.empty()) {
            OpBatch rb;
            IVec3 rorigin{0, 0, 0};
            uint32_t rlabel = 0;
            if (net::OpsWire::Decode(w.ops.data(), w.ops.size(), rb, rorigin,
                                     rlabel) &&
                rlabel == w.h.tick) {
              netOpsRecv += rb.ops.size() + rb.exps.size() + rb.cells.size() +
                            rb.spawns.size() + rb.fluid.size();
              opsync.Q().NoteRemote(w.h.tick, w.h.playerId, rorigin,
                                    std::move(rb));
            } else {
              netLate++;
            }
          }
          NetPending& q = netQueue[w.h.tick];
          q.timeScale = w.h.timeScale;
          q.vizActive = w.h.vizActive;
          if (w.playerState.size() == sizeof(PlayerState)) {
            std::memcpy(&q.st, w.playerState.data(), sizeof(PlayerState));
            // ---- M9.3-C: THE PEER'S ORIGIN RING ------------------------
            //
            // `st.tick` is the tick the peer had just FINISHED when it built
            // this state, and `st.windowOrigin` is where its window was AT
            // THAT TICK — the two are a matched pair by construction
            // (MakePlayerState), which is exactly what a comparison at hash
            // tick H needs. The batch LABEL is a different number (it is
            // st.tick + D + 1) and using it here would compare the digests of
            // tick H against a window origin from five ticks later, which on
            // a walking peer is a different set of comparable chunks.
            if (q.st.version == kPlayerStateVersion) {
              chunksync.NotePeerState(
                  q.st.tick,
                  {q.st.windowOrigin[0], q.st.windowOrigin[1],
                   q.st.windowOrigin[2]},
                  {ifloor(q.st.pos.x) >> 4, ifloor(q.st.pos.y) >> 4,
                   ifloor(q.st.pos.z) >> 4});
            }
            // A version the handshake did not refuse but this struct cannot
            // read is still not readable. Better one motionless ghost than a
            // body standing at fields read at the wrong offsets.
            q.haveState = (q.st.version == kPlayerStateVersion);
          }
          break;
        }
        // ---- M9.3-C: THE CONVERGENCE MESSAGES ---------------------------
        //
        // Five new types plus the ChunkSync slices, all handed straight to
        // the state machine (net/chunksync.h), which is where every rule
        // about them lives. This file's job is the socket and nothing else:
        // the switch does not know what a block sum means and must not start
        // to, or the pure half would stop being gate-drivable.
        //
        // They ride the SAME link as the tick batches and are sent AFTER them
        // within a frame, so a repair can never delay the batch the peer's
        // pacer is waiting on.
        // ---- M9.4-D: THE ENTITY ENVELOPE --------------------------------
        //
        // QUEUED BY LABEL, applied by the tick loop when that label runs —
        // the same rule the `PlayerState` above obeys and for the same
        // reason: a record applied on arrival would move a creature D ticks
        // ahead of every local body it can be hit by. There is no "late"
        // counter here because `ApplyForTick` deliberately applies a batch
        // whose label has already passed rather than dropping it (see the
        // note there): a stale pose is the freshest thing anybody has said
        // about that entity, and discarding it freezes a ghost.
        case net::MsgType::EntityBatch: {
          if (!netPaced) break;
          net::EntityBatch eb;
          if (!eb.Decode(m.payload.data(), m.payload.size())) {
            netLate++;   // a half-applied entity batch is worse than none
            break;
          }
          entities.NoteRemote(std::move(eb));
          break;
        }
        // ---- FIXED HERE (found by M9.5-B's two-process smoke) -----------
        //
        // THESE FIVE USED TO FALL THROUGH INTO `EntityBatch`. The M9.3-C
        // block listed them immediately above `case EntityBatch:` with no
        // body of their own, intending to reach `ChunkSync` below — and
        // M9.4-D then inserted the entity case BETWEEN them and their target.
        // Every `HashBlocks`, `HashDrill`, `HashChunks`, `ChunkRequest` and
        // `ChunkBusy` was therefore handed to `net::EntityBatch::Decode`,
        // which correctly refused it and charged it to `netLate`.
        //
        // MEASURED, the first M9.5-B smoke: both processes reported
        // `hashBlocks sent=38 recv=0` and `late=38`, and the mismatch series
        // printed "(none - no publish was ever compared)". The whole M9.3-C
        // convergence protocol was dead on the wire and the only symptom was
        // a counter that reads as a pacing bug. One `break` is the fix.
        case net::MsgType::HashBlocks:
        case net::MsgType::HashDrill:
        case net::MsgType::HashChunks:
        case net::MsgType::ChunkRequest:
        case net::MsgType::ChunkBusy:
        case net::MsgType::ChunkSync: {
          if (!netPaced) break;
          std::vector<net::ChunkSync::Out> reply;
          chunksync.OnMessage((net::MsgType)m.type, m.payload.data(),
                              m.payload.size(), world, tick, reply);
          netSendSync(reply);
          break;
        }
        // ---- M9.5-B: THE PERSISTENCE MESSAGES ---------------------------
        //
        // Seven types, all handed straight to the state machine, which is
        // where every rule about them lives — the same split as ChunkSync
        // above, and for the same reason: this file's job is the socket, and
        // a switch that started to know what a manifest row MEANS would
        // make the pure half stop being gate-drivable.
        //
        // NOT GUARDED ON `netPaced`, unlike every case above it, and that is
        // deliberate: the host's full `ChunkManifest` is sent inside
        // `netStartPacing` and therefore lands on the client while its own
        // `netPaced` is being set in the same call. It is also the one thing
        // that must arrive BEFORE the first tick runs (the late join binds
        // the exchange on it), so dropping it for being "too early" would
        // deadlock the join. `StoreSync` refuses on `!link_` instead, which
        // is the condition that actually matters.
        case net::MsgType::ChunkPut:
        case net::MsgType::ChunkPutAck:
        case net::MsgType::ChunkManifest:
        case net::MsgType::ManifestDelta:
        case net::MsgType::ChunkGet:
        case net::MsgType::ChunkData:
        case net::MsgType::ChunkMiss:
          storesync.OnMessage((net::MsgType)m.type, m.payload.data(),
                              m.payload.size());
          break;
        default:
          break;
      }
    }
    // ---- M9.3-C: PUBLISH, SERVICE, REQUEST ------------------------------
    //
    // Once per frame, after the inbox is drained so a request that arrived
    // this frame can be answered this frame. `Pump` publishes a HashBlocks
    // only when the DIGEST TABLE has advanced (the GPU's schedule, not the
    // frame's), services fetches that have landed, expires stale requests and
    // issues up to the in-flight cap.
    if (netPaced) {
      std::vector<net::ChunkSync::Out> outMsgs;
      chunksync.Pump(world, tick, outMsgs);
      netSendSync(outMsgs);
      // ---- M9.5-B: ISSUE THE QUEUED CHUNK GETS ------------------------
      //
      // `Request` only QUEUES: it is called from inside `Stream::Update`,
      // once per wanted slot, and a window shift refills a 1,024-chunk plane
      // in a single call — so the send site has to be somewhere the
      // in-flight cap can bound it. That is here, once a frame, after the
      // inbox so an answer that arrived this frame has already freed its
      // slot in the cap. It also re-asserts the host's tick tags, which
      // `Stream`'s own untagged Put erases (net/storesync.h `Pump`).
      storesync.Pump();
    }
    // ---- DISCONNECT (§4 finding 5: 3 s silence -> local authority) -------
    // Two doors: the socket itself failed (`Error()` is the latch `DropPeer`
    // set, and it covers a clean FIN as "peer closed" too), or the peer went
    // quiet. The silence test measures BYTES, not messages, so a peer part-way
    // through a large transfer is not declared dead; it only runs while paced,
    // because a listening host with no peer has been silent since boot.
    if (link) {
      // POLLED TIME, NOT WALL TIME (M9.3-B; see netSilence's declaration for
      // the bug). A frame longer than kNetPollStallSeconds was this machine
      // not listening, so it contributes nothing to the peer's silence.
      constexpr double kNetPollStallSeconds = 1.0;
      constexpr double kNetSilenceLimit = 3.0;
      const double nowSec = net::NowSeconds();
      const double gap =
          netSilenceLastPoll > 0.0 ? nowSec - netSilenceLastPoll : 0.0;
      netSilenceLastPoll = nowSec;
      if (link->Stats().lastRecvSeconds >= nowSec - gap) {
        netSilence = 0.0;   // something arrived during this frame
      } else if (gap > kNetPollStallSeconds) {
        netPollStalls++;    // a foreground stall: not the peer's fault
      } else {
        netSilence += gap;
      }
      const bool broken = !link->Error().empty();
      const bool silent = netPaced && netSilence > kNetSilenceLimit &&
                          nowSec - netPacedAt > kNetConnectGraceSeconds;
      if (broken || silent)
        netDrop(broken ? link->Error().c_str() : "3 s of polled silence");
    }
  };

  // ---- BOOT: OPEN THE PORT, OR JOIN AND BLOCK ---------------------------
  if (netRoleBoot == NetRole::Host) {
    // A LISTEN SERVER. The world is already generated and the frame loop is
    // about to start; hosting adds a socket and nothing else, so a host plays
    // a completely normal single-player game until somebody arrives.
    link = std::make_unique<net::TcpLink>();
    if (!link->Listen(netPort)) {
      std::fprintf(stderr, "net: cannot listen on %u: %s\n", (unsigned)netPort,
                   link->Error().c_str());
      return 2;
    }
    std::printf("net: hosting on 127.0.0.1:%u (player 0), waiting for a peer\n",
                (unsigned)link->ListenPort());
    std::fflush(stdout);
  } else if (netRoleBoot == NetRole::Client) {
    // AND A CLIENT BLOCKS. There is nothing sensible to do with a world the
    // host may be about to refuse — a different seed, a different
    // tuning.json — and the alternative (start playing, then jump the tick
    // counter when the ack lands) is a visible world discontinuity for
    // exactly the case where the join succeeds.
    const net::Hello mine = netLocalHello(1);
    bool sentHello = false, acked = false;
    const double tJoin0 = NowSeconds();
    // THE 10 s IS A RETRY WINDOW, NOT A SINGLE ATTEMPT. A host that is still
    // booting has no listen socket yet — device + SPIR-V + worldgen is ~20 s
    // on this machine — and a non-blocking connect to a closed port comes back
    // ECONNREFUSED in microseconds. Giving up on the first refusal makes the
    // whole window worthless: "join failed" one second into a ten-second grace
    // period is the answer nobody wants, and it is what made the two-process
    // smoke a race between two boot times.
    //
    // A FRESH TcpLink PER ATTEMPT, because `LinkBase::Fail` latches: a link
    // that has once reported "connect refused" refuses to Poll or Send for the
    // rest of its life (see the unique_ptr note at the top of this block).
    int netJoinAttempts = 0;
    auto netTryConnect = [&]() -> bool {
      link = std::make_unique<net::TcpLink>();
      sentHello = false;
      netJoinAttempts++;
      return link->Connect(netJoinIp, netPort);
    };
    if (!netTryConnect()) {
      std::fprintf(stderr, "net: cannot connect to %s:%u: %s\n",
                   netJoinIp.c_str(), (unsigned)netPort,
                   link->Error().c_str());
      return 2;
    }
    while (!acked) {
      link->Poll();
      if (!link->Error().empty()) {
        // A refusal DURING the window is "not up yet"; a refusal after it is
        // the answer. The distinction is the elapsed time and nothing else —
        // a refused connect and a host that never existed look identical on
        // the wire.
        if (NowSeconds() - tJoin0 <= 10.0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          if (!netTryConnect()) {
            std::fprintf(stderr, "net: cannot connect to %s:%u: %s\n",
                         netJoinIp.c_str(), (unsigned)netPort,
                         link->Error().c_str());
            return 2;
          }
          continue;
        }
        std::fprintf(stderr, "net: join failed after %d attempt(s): %s\n",
                     netJoinAttempts, link->Error().c_str());
        return 2;
      }
      if (link->Connected() && !sentHello) {
        std::vector<uint8_t> b;
        mine.Encode(b);
        link->Send((uint16_t)net::MsgType::Hello, net::kProtocolVersion,
                   b.data(), b.size());
        link->Poll();
        sentHello = true;
      }
      net::Msg m;
      while (link->Recv(m)) {
        if ((net::MsgType)m.type == net::MsgType::HelloRefuse) {
          net::HelloRefuse r;
          r.Decode(m.payload.data(), m.payload.size());
          // THE FIELD, NOT "incompatible". This is the line a user acts on.
          std::printf("net: refused: %s\n", r.field.c_str());
          std::fflush(stdout);
          return 2;
        }
        if ((net::MsgType)m.type == net::MsgType::HelloAck) {
          net::HelloAck ack;
          if (!ack.Decode(m.payload.data(), m.payload.size())) {
            std::fprintf(stderr, "net: malformed HelloAck\n");
            return 2;
          }
          netMyId = ack.yourPlayerId;
          netPeerId = 0;   // the host is always player 0
          // THE HOST'S COUNTER IS THE CLOCK. `tick` is set one BELOW the start
          // tick because the accumulator loop increments before it runs, so
          // the first paced tick on this machine is exactly `startTick`.
          tick = ack.startTick - 1;
          netStartPacing(ack.startTick);
          acked = true;
          break;
        }
      }
      if (acked) break;
      if (NowSeconds() - tJoin0 > 10.0) {
        std::fprintf(stderr, "net: join timed out after 10 s (no HelloAck from "
                             "%s:%u, %d connect attempt(s))\n",
                     netJoinIp.c_str(), (unsigned)netPort, netJoinAttempts);
        return 2;
      }
      // 2 ms, not a spin: the host may still be booting its device and this
      // loop has nothing else to do with the CPU.
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::printf("net: joined %s:%u as player %u, start tick %u\n",
                netJoinIp.c_str(), (unsigned)netPort, netMyId, tick + 1);
    std::fflush(stdout);

    // ---- THE LATE JOIN (M9.5-B, plan step 3) -----------------------------
    //
    // This machine already has a world: worldgen ran at boot, before the
    // socket existed, and every chunk in the window is PRISTINE procgen. The
    // host's may not be — anything a previous session edited and evicted is
    // in the host's store and in nothing else. So the join is not finished
    // until this window has been re-pulled with the exchange bound.
    //
    // THE ORDER IS THE WHOLE CORRECTNESS ARGUMENT, and it is three steps:
    //
    //   1. WAIT FOR THE MANIFEST. `Wanted` must answer from a table already
    //      in hand (sim/stream.h forbids a blocking answer), so the table
    //      has to be complete before anything can ask. It is at most a few
    //      64 KiB slices and the host sent them before its first TickBatch.
    //   2. BIND, THEN RELOAD. Binding after the reload would re-pull the
    //      whole window from procgen and then leave the peer's edits to
    //      trickle in over the next few window shifts, which is both slower
    //      and visibly wrong (an edit appearing minutes later).
    //   3. BEFORE THE FIRST TICK. Everything here runs before the frame loop
    //      is entered, so the first tick this machine simulates already sees
    //      the held slots — no tick ever runs over a chunk that is about to
    //      be replaced under it.
    //
    // THE WAIT IS BOUNDED AND A TIMEOUT IS NOT FATAL. A host that never
    // sends a manifest is a host running an older build; the honest
    // degradation is "play with procgen terrain", not "refuse to join". The
    // exchange is bound either way, so a later ManifestDelta still lands.
    {
      const double tMan0 = NowSeconds();
      while (!storesync.ManifestReady() && NowSeconds() - tMan0 < 10.0) {
        link->Poll();
        net::Msg mm;
        while (link->Recv(mm)) {
          // Only the manifest is interesting here. Anything else that beat
          // it — the D+1 pre-sent TickBatches, an EntityBatch — is PARKED
          // for netPump rather than consumed: dropping a pre-send would
          // punch a hole in the pacer's contiguity and report itself as
          // `late=`, which is the one number that is supposed to mean the
          // label arithmetic is broken (see `netEarly`).
          if (!storesync.OnMessage((net::MsgType)mm.type, mm.payload.data(),
                                   mm.payload.size()))
            netEarly.push_back(std::move(mm));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      if (!storesync.ManifestReady())
        std::printf("net: no chunk manifest after 10 s -- joining with "
                    "procgen terrain (the host may be an older build)\n");
      else
        std::printf("net: chunk manifest received: %zu rows\n",
                    storesync.ManifestSize());
      stream.SetChunkExchange(&storesync);
      // THE WHOLE-WINDOW RE-PULL. `ReloadWindow` is exactly what LoadWorld
      // uses (sim/stream.h): every slot refilled from the store, misses to
      // procgen — except that now a miss consults `Wanted` first, so a chunk
      // the host holds is HELD INERT and arrives over the wire instead of
      // being generated and then overwritten in front of the player.
      stream.ReloadWindow(world.WindowOrigin());
      std::fflush(stdout);
    }
  }
  // THE GHOST LIST REACHES THE TICK ONLY WHEN THERE IS A NETWORK. Null
  // otherwise, which is what keeps phases B / H / I / O making exactly the
  // calls they made before package B landed.
  // The ground-item registry: the tick throws vessels into it and breaks the
  // ones that land hard (session.h section H2).
  tickCtx.ground = &ground;
  // ---- THE MAP'S REFERENCES (world/refs.h, PLAN_world_editor.md P1) -------
  // assets/worldmap/<map>/refs/*.json: villagers, doors, markers, houses. The
  // tick activates them against the window and runs the use verb
  // (session.h section H3). NOT for the harness map: its refs are the refs-*
  // gates' fixtures, and a --shot or a smoke must not find a villager in it.
  refs::RegisterAllKinds();
  refs::RefStore refStore;
  uint32_t lookUseRef = 0;   // the use prompt's target (refs::RefHash), 0 = none
  auto refsMapName = [] { return worldmap::ActiveMapName(CurrentTuning().world.mapLayer); };
  if (refsMapName() != kHarnessMapName) {
    refStore.LoadMap(assetDir, refsMapName());
    refStore.BindChunkStore(&stream.Store());
    tickCtx.refs = &refStore;
    ui.refs = &refStore;
  }
  if (netRoleBoot != NetRole::None) tickCtx.remotes = &remotes;
  // ...and so does the op exchange. Null in every harness and in every
  // single-player frame; even here it does nothing until Connected().
  if (netRoleBoot != NetRole::None) tickCtx.opsync = &opsync;
  // ...and the convergence half, which phase B drains before stream.Update.
  if (netRoleBoot != NetRole::None) tickCtx.chunksync = &chunksync;
  // ---- M9.3-C: THE SMOKE'S DELIBERATE DIVERGENCE (NetSmokeDrift) --------
  //
  // Bound only when the switch is set AND this is a --frames harness run, so
  // it cannot be reached from a game. Phase N calls it after the merge, which
  // is what makes the op local-only; see session.h section I.
  if (netRoleBoot != NetRole::None && NetSmokeDrift() && g_harnessFrames > 0) {
    tickCtx.driftCells = [&](uint32_t t, std::vector<CellOp>& cells) {
      if (!netPaced || t % 90 != 0) return;
      // Four chunks (64 voxels, 6.4 m) below the feet: buried stone under a
      // standing host, and inside the window by a wide margin so the
      // comparable rule (>= 2 chunks in) never excludes it.
      const IVec3 pc{ifloor(session.player.pos.x) >> 4,
                     ifloor(session.player.pos.y) >> 4,
                     ifloor(session.player.pos.z) >> 4};
      const IVec3 wc{pc.x, pc.y - 4, pc.z};
      if (!world.ChunkInWindow(wc)) return;
      const IVec3 cell{wc.x * (int)kChunk + 8, wc.y * (int)kChunk + 8,
                       wc.z * (int)kChunk + 8};
      // A palette variant that CHANGES every time, so the second drift is not
      // silently the same word as the first and a repair that did nothing
      // would still look repaired.
      const uint32_t variant = 1u + ((t / 90u) & 7u);
      cells.push_back({World::SlotCellIndex(cell),
                       PackVoxNew(kMatStone, variant)});
      netDriftOps++;
    };
  }

  // ---- M9.5-B: --load-world IS THE F10 PATH, PRESSED FOR YOU ------------
  //
  // Not a second load implementation and deliberately so: the handler in the
  // frame loop resets the transient state the grid restore invalidates
  // (grenades, the MPM fluid count, the render interpolation), and a boot
  // path that called `LoadWorld` directly would be a second list of those to
  // keep in step. Setting the flag costs one frame of a pristine world nobody
  // is looking at and inherits every line of the real path, including the
  // SVM5 clock resume.
  if (loadWorldAtBoot) {
    std::printf("--load-world %s: loading on the first frame\n",
                worldDir.c_str());
    std::fflush(stdout);
    ui.loadWorld = true;
  }

  uint64_t frameCounter = 0;
  StartupMark("frame loop entered");
  uint64_t startupFrame = 0;
  // A HOST'S --frames BUDGET STARTS WHEN THE PEER ARRIVES (M9.2-C).
  //
  // `--host 7777 --frames 900` means "900 frames of two-player play", not
  // "900 frames, some of which may be spent alone". Without this the
  // two-process smoke is a race between two boot times that the machine
  // decides: device + SPIR-V + worldgen is ~24 s here and 900 frames at
  // vsync-uncapped is ~11 s, so the host was reliably finished before the
  // client's window even existed and the smoke measured nothing.
  //
  // The deadline is the safety catch: a `--host --frames N` run that nobody
  // ever joins must still terminate, or a harness invocation becomes a hang.
  // 120 s is far longer than any boot and far shorter than a CI timeout.
  const double netHostWaitDeadline = NowSeconds() + 120.0;
  auto netHostStillWaiting = [&]() {
    return netRoleBoot == NetRole::Host && !netEverPaced &&
           NowSeconds() < netHostWaitDeadline;
  };
  while (!glfwWindowShouldClose(window)) {
    // M9.2-C: THE FRAME'S ONE SOCKET TOUCH, and it is before the tick loop
    // (and before the harness's own exit check, so a peer that quit is
    // observed on the frame it quit). Accepts, reads, flushes, decodes every
    // arrived batch into the pacer and the label queue, and handles the
    // disconnect. No-op when nothing is networked: `link` is null.
    if (link) netPump();
    if (g_harnessFrames > 0 && !netHostStillWaiting()) {
      frameCounter++;
      // The mid-run reload verifies the F5 path, but it also recompiles every
      // pipeline in the foreground and the far set on three background
      // threads for the next ~40 s, which is half the run — so the frame-time
      // tail of a default `--frames` run is the compiler, not the game.
      // SANDVOX_FRAMES_NO_RELOAD=1 is the measurement arm.
      static const bool noReload =
          std::getenv("SANDVOX_FRAMES_NO_RELOAD") != nullptr || g_forestFire ||
          g_shotDialogue;  // the reload would wipe the listener it spawned
      if (frameCounter == g_harnessFrames / 2 && !noReload) {
        std::printf("--frames harness: triggering shader reload (F5 path)\n");
        ui.reloadShaders = true;
      }
      if (frameCounter >= g_harnessFrames) glfwSetWindowShouldClose(window, 1);
    }
    // The park probe is tick-scheduled, so it decides its own end: --frames
    // only has to be generous enough to reach it.
    if (g_parkDone || g_forestFireDone) glfwSetWindowShouldClose(window, 1);

    // --shot-jump: decide whether THIS frame is one of the four pictures.
    //
    // Run here, before the render, so the capture block downstream only has to
    // check a pointer. The thresholds are read off the same tuning rows the
    // pose blends on, so a picture called "rise" is the shape the tuck is
    // authored at rather than "whatever 6 frames after the jump happened to
    // be" — see the note at g_shotJump.
    g_shotJumpPath = nullptr;
    // A GROUNDED FRAME OF THE SAME BODY, FIRST. Judging an airborne pose in
    // isolation is judging it against a memory: half of "does this look right"
    // is whether the arms, the lean and the knee bend read as the SAME
    // character who was walking a moment ago. This is the reference the other
    // four are compared with, and it costs one frame.
    if (g_shotJump && frameCounter == kShotJumpAtFrame - 20)
      g_shotJumpPath = "screenshot_jump_walk.bmp";
    if (g_shotJump && frameCounter > kShotJumpAtFrame &&
        g_shotJumpWant != JumpShot::Done && avatar.Spawned()) {
      const auto& av = CurrentTuning().avatar;
      // OFF THE PLAYER, NOT OFF THE POSE. `AirPoseVy` is zero whenever
      // avatar.airPose is off, so triggering on it would make this harness
      // unable to photograph the thing it is supposed to be compared against —
      // and an A/B whose control arm writes no files is not an A/B. The
      // controller's own velocity is the same signal the pose reads anyway.
      const float vy = player.vel.y * kVoxelMeters;   // voxels/s -> m/s
      const bool air = !player.grounded;
      // A FEW FRAMES OF AIR BEFORE THE FIRST PICTURE. The launch velocity is
      // whole on the very first airborne frame while the pose is still blending
      // in over ikBlendHalflife, so a bare `vy > threshold` photographs the
      // standing pose with a jump's velocity attached — measured, 0.25 of the
      // way in. This is not a fudge for the blend: no animation system snaps,
      // and "what does the rig look like a tenth of a second into a jump" is
      // the honest question.
      static int airFrames = 0;
      airFrames = air ? airFrames + 1 : 0;
      switch (g_shotJumpWant) {
        case JumpShot::Rise:
          // Most of the way up the launch, so the tuck is nearly whole.
          if (air && airFrames >= 5 && vy > av.airPoseRiseSpeed * 0.45f) {
            g_shotJumpPath = "screenshot_jump_rise.bmp";
            g_shotJumpWant = JumpShot::Apex;
          }
          break;
        case JumpShot::Apex:
          // The float shape is the vy == 0 end of the blend, by construction.
          if (air && std::fabs(vy) < av.airPoseRiseSpeed * 0.15f) {
            g_shotJumpPath = "screenshot_jump_apex.bmp";
            g_shotJumpWant = JumpShot::Fall;
          }
          break;
        case JumpShot::Fall:
          // Committed descent. A jump off flat ground never reaches the full
          // airPoseFallSpeed (it only regains its launch speed), so this asks
          // for a fraction of the launch speed downward instead — the picture
          // is of the reach shape coming in, which is what a jump shows.
          if (air && vy < -av.airPoseRiseSpeed * 0.45f) {
            g_shotJumpPath = "screenshot_jump_fall.bmp";
            g_shotJumpWant = JumpShot::Land;
          }
          break;
        case JumpShot::Land: {
          // The landing PREPARE, taken off the ground probe's own weight
          // rather than off "the last airborne frame". Those are not the same
          // picture: the pose fades out over the IK half-life once the feet
          // are down, so by the time the rig reads grounded the prepare has
          // already begun unwinding and the photograph is of the `land`
          // squash instead — a different system's work.
          //
          // ...OR the last airborne frame, whichever comes first. The probe's
          // weight is squared against airPoseLandHeight, so a body that jumps
          // off a lip and lands lower than it left never reaches the high end
          // of it — and a harness that silently writes three files instead of
          // four is worse than one that writes a slightly early fourth.
          const bool deep = avatar.AirPoseLand() > 0.5f;
          if ((deep && air) || player.grounded) {
            g_shotJumpPath = "screenshot_jump_land.bmp";
            g_shotJumpWant = JumpShot::Done;
          }
          break;
        }
        default:
          break;
      }
      if (g_shotJumpPath) g_shotJumpVy = vy;
    }

    // --shot-spellpage's scripted schedule: open the screen on the grimoire,
    // then one word list per scene. The words are written into the COMPOSER's
    // field, which is the same thing typing them does; everything else (parse,
    // lower, build, the mirror the canvas draws) happens on the ordinary path.
    if (g_shotSpellPage) {
      if (frameCounter == 1) {
        ui.fly = false;
        player.fly = false;
      }
      if (frameCounter == kShotSpellOpen) {
        ui.inventoryOpen = true;
        ui.grimoireMode = true;
        // THE BOOK IS SHUT BY DEFAULT (2026-09-22). This harness photographs
        // the page, so it opens the book the way the player does.
        ui.spellbookOpen = true;
        ui.visible = false;  // the dev panel sits on top of the thing we shoot
        captured = false;
        captureBeforeUi = false;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        // PARK THE POINTER. It lands wherever the window happened to put it,
        // and one of the gallery's pictures came back with the page list's
        // tooltip open across the canvas - a picture of a tooltip, not of a
        // page. Bottom-centre is the one patch of this screen with nothing
        // hoverable on it.
        glfwSetCursorPos(window, 1200.0, 880.0);
      }
      const int sceneSetup = ShotSpellSceneAt(frameCounter, true);
      if (sceneSetup >= 0) {
        const ShotSpellScene& sc = kShotSpellScenes[sceneSetup];
        GrimoirePage pg;
        pg.name = std::string("shot-") + sc.name;
        std::string w;
        for (const char* c = sc.words;; c++) {
          if (*c == ' ' || *c == 0) {
            if (!w.empty()) pg.words.push_back(w);
            w.clear();
            if (*c == 0) break;
          } else {
            w.push_back(*c);
          }
        }
        if (caster.grimoire.Find(pg.name) < 0) caster.grimoire.pages.push_back(pg);
        ui.grimoireMode = true;
        ui.grimoireSelected = pg.name;
        ui.grimoireEditName = pg.name;
        ui.grimoireEditWords = pg.words;
        ui.grimoireEditDirty = false;
        // A view driven by an earlier scene is not this scene's picture.
        ui.spellGraphZoom = sc.zoom;
        ui.spellGraphPanX = sc.panX;
        ui.spellGraphPanY = sc.panY;
        std::printf("--shot-spellpage: [%s] %d words\n", sc.name,
                    (int)pg.words.size());
      }
    }
    // --shot-bench's script (see g_shotBench). Frame-counted like the rest.
    if (g_shotBench) {
      const uint64_t f = frameCounter;
      if (f == 1) {
        ui.fly = false;
        player.fly = false;
      }
      static int benchA = -1, benchB = -1;
      if (f == 140) {
        const int fi = items.Find("flask");
        const ItemDef* fd = fi >= 0 ? items.At(fi) : nullptr;
        if (fd) {
          const char* ea = std::getenv("SANDVOX_BENCH_A");
          const char* eb = std::getenv("SANDVOX_BENCH_B");
          ItemStack a = StackOf(items, fi);
          a.contents = ContainerParseFillSpec(ea ? ea : "water:0.3+oil:0.22+sand:0.08",
                                              fd->container.capacity, mats);
          // The source may be another kind of vessel (SANDVOX_BENCH_B_ITEM,
          // e.g. "pouch").
          const char* ebi = std::getenv("SANDVOX_BENCH_B_ITEM");
          const int bi = ebi ? items.Find(ebi) : fi;
          const ItemDef* bd = bi >= 0 ? items.At(bi) : nullptr;
          ItemStack b = StackOf(items, bd ? bi : fi);
          b.contents = ContainerParseFillSpec(eb ? eb : "lava:0.25+sand:0.1",
                                              (bd ? bd : fd)->container.capacity, mats);
          hotbar.slots[8] = a;
          benchA = 8;
          benchB = kit.bag.FirstFree();
          if (benchB >= 0) kit.bag.slots[benchB] = b;
        }
        ui.inventoryOpen = true;
        ui.visible = false;   // the dev panel, not the game UI
        captured = false;
      }
      const KitRef refA{KitSpace::Hotbar, benchA}, refB{KitSpace::Bag, benchB};
      if (f == 150 && benchA >= 0) {
        ui.alchemy.wantOpen = true;
        ui.alchemy.openRef = refA;
      }
      if (f == 160 && benchB >= 0) {
        ui.alchemy.wantToggle = true;
        ui.alchemy.toggleRef = refB;
      }
      if (f == 175) g_shotJumpPath = "screenshot_bench.bmp";
      static const bool chemBurner = std::getenv("SANDVOX_BENCH_BURNER") != nullptr;
      static const bool chemStopper = std::getenv("SANDVOX_BENCH_STOPPER") != nullptr;
      static const bool chemNoPour = std::getenv("SANDVOX_BENCH_NOPOUR") != nullptr;
      static const int chemShock = std::getenv("SANDVOX_BENCH_SHOCK") ? std::atoi(std::getenv("SANDVOX_BENCH_SHOCK")) : -1;
      if (f == 178 && bench.IsOpen()) {
        if (chemBurner) bench.SetBurner(refA, true);
        if (chemStopper) bench.SetStopper(refA, true);
      }
      if (chemShock > 0 && (int)f >= chemShock && ((int)f - chemShock) % 20 == 0 && bench.IsOpen())
        bench.Shock(refA);
      if (chemNoPour) {
        if (f == 240) g_shotJumpPath = "screenshot_bench_chem1.bmp";
        if (f == 320) g_shotJumpPath = "screenshot_bench_chem2.bmp";
        if (chemShock > 0 && (int)f == chemShock + 2) g_shotJumpPath = "screenshot_bench_arc.bmp";
        if (f == 398) g_shotJumpPath = "screenshot_bench_chem3.bmp";
      }
      // SANDVOX_BENCH_PORTRAIT_YAW turns the portrait (radians; 1.3 = from the
      // side): the front view foreshortens a flask held out toward you.
      if (const char* py = std::getenv("SANDVOX_BENCH_PORTRAIT_YAW"))
        ui.portraitYaw = (float)std::atof(py);
      alchemy::Xform pa, pb;
      float wa = 0, ha = 0, wb = 0, hb = 0;
      const bool haveA = bench.IsOpen() && bench.PoseOf(refA, pa, wa, ha);
      const bool haveB = bench.IsOpen() && bench.PoseOf(refB, pb, wb, hb);
      // Stir A: the stick sweeps side to side in its belly.
      if (f > 180 && f < 240 && haveA && !chemNoPour) {
        ui.alchemy.tool = 1;
        ui.alchemy.over = true;
        ui.alchemy.down = true;
        ui.alchemy.atX = pa.pos.x + wa * 0.3f * std::sin((float)f * 0.15f);
        ui.alchemy.atY = pa.pos.y + ha * 0.15f;
      }
      if (f == 238 && !chemNoPour) g_shotJumpPath = "screenshot_bench_stir.bmp";
      // Pour B into A with the hand: grab B by its belly, lift it clear,
      // then carry and tip it so its left lip corner hangs over A's mouth.
      static float want = 0.0f, reqd = 0.0f;
      static alchemy::V2 grabAt, start;
      static const bool invert = std::getenv("SANDVOX_BENCH_INVERT") != nullptr;
      // SANDVOX_BENCH_INVERT: lift B straight up, swing its lip over A's
      // mouth while tipping it to 75 degrees (before its contents reach the
      // lip), then turn it on to upside down, the pivot sliding from the
      // pouring lip corner to the mouth's centre, 30 px over A's mouth -- the
      // path of gate alchemy-lift. The pointer is the grab point of that
      // pose; the bench's HandGoal does the rest.
      static alchemy::Xform b0;
      if (invert && f >= 260 && f < 560 && haveA && haveB && !chemNoPour) {
        const alchemy::V2 grabL{0.0f, hb * 0.35f};
        if (f == 260) {
          b0 = pb;
          reqd = 0.0f;
        }
        auto ease = [](float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3 - 2 * t); };
        auto rot = [](alchemy::V2 v, float a) {
          return alchemy::V2{v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a)};
        };
        const float side = pa.pos.x < b0.pos.x ? 1.0f : -1.0f;   // + = CCW, mouth swings left
        const alchemy::V2 L{-side * 0.30f * wb * 0.5f, hb};        // the pouring lip corner
        const alchemy::V2 M{pa.pos.x, pa.pos.y + ha + 30.0f};    // ...goes here
        const float liftY = M.y + 20.0f;                           // B's base, lifted upright
        const float kSwing = 1.309f, kPi = 3.14159f;
        const float fl = (float)f;
        alchemy::Xform p{{b0.pos.x, liftY}, 0.0f};
        if (fl < 300) {
          p.pos.y = b0.pos.y + (liftY - b0.pos.y) * ease((fl - 260) / 40);
        } else {
          float ang;
          alchemy::V2 corner;
          if (fl < 370) {
            const float e = ease((fl - 300) / 70);
            ang = kSwing * e;
            const alchemy::V2 c0{b0.pos.x + L.x, liftY + L.y};
            corner = {c0.x + (M.x - c0.x) * e, c0.y + (M.y - c0.y) * e};
          } else {
            ang = kSwing + (kPi - kSwing) * ease((fl - 370) / 90);
            corner = M;
          }
          const float k = std::clamp((kPi - ang) / (kPi - kSwing), 0.0f, 1.0f);
          const alchemy::V2 r = rot({L.x * k, L.y}, side * ang);
          p = {{corner.x - r.x, corner.y - r.y}, side * ang};
        }
        const alchemy::V2 g = rot(grabL, p.angle);
        ui.alchemy.tool = 0;
        ui.alchemy.over = true;
        ui.alchemy.pressed = f == 260;
        ui.alchemy.down = true;
        ui.alchemy.atX = f == 260 ? b0.pos.x + grabL.x : p.pos.x + g.x;
        ui.alchemy.atY = f == 260 ? b0.pos.y + grabL.y : p.pos.y + g.y;
        ui.alchemy.tiltReq = p.angle - reqd;
        reqd = p.angle;
      }
      if (invert && !chemNoPour) {
        if (f == 299) g_shotJumpPath = "screenshot_bench_invlift.bmp";
        if (f == 385) g_shotJumpPath = "screenshot_bench_invturn.bmp";
        if (f == 440) g_shotJumpPath = "screenshot_bench_inverted.bmp";
        if (f == 555) g_shotJumpPath = "screenshot_bench_invpoured.bmp";
        if (f == 565) ui.alchemy.wantClose = true;
        if (f == 580) g_shotJumpPath = "screenshot_bench_closed.bmp";
      }
      if (!invert && f >= 260 && f < 400 && haveA && haveB && !chemNoPour) {
        const alchemy::V2 grabL{0.0f, hb * 0.35f};
        if (f == 260) {
          grabAt = {pb.pos.x + grabL.x, pb.pos.y + grabL.y};
          start = grabAt;
          want = reqd = 0.0f;
        }
        auto ease = [](float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3 - 2 * t); };
        const float fl = (float)f;
        const alchemy::V2 mouth{pa.pos.x, pa.pos.y + ha};
        want = fl < 290 ? 0.0f : fl < 330 ? 1.6f * ease((fl - 290) / 40) : 1.6f + 0.9f * ease((fl - 330) / 50);
        // Where the grab point must be for the lip to sit over the mouth.
        // Tip toward A: counter-clockwise off the left lip when A is to the
        // left, clockwise off the right lip when it is to the right.
        const float side = pa.pos.x < pb.pos.x ? 1.0f : -1.0f;
        // SANDVOX_BENCH_AWAY=1: lift it and tip it AWAY from A, never
        // carrying it over (the arm must only rise and the flask turn).
        static const bool away = std::getenv("SANDVOX_BENCH_AWAY") != nullptr;
        want *= away ? -side : side;
        const alchemy::V2 lipL{-side * 0.30f * wb * 0.5f, hb};
        const alchemy::V2 r{lipL.x - grabL.x, lipL.y - grabL.y};
        const float c = std::cos(want), sn = std::sin(want);
        const alchemy::V2 over{mouth.x - (r.x * c - r.y * sn), mouth.y + 14.0f - (r.x * sn + r.y * c)};
        const alchemy::V2 lifted{start.x, std::max(start.y, mouth.y + hb * 0.7f)};
        alchemy::V2 at = lifted;
        // SANDVOX_BENCH_HIGH=1: lift it as high as the bench lets it go and
        // sweep it side to side up there, no tilt (the raised-arm repro).
        static const bool high = std::getenv("SANDVOX_BENCH_HIGH") != nullptr;
        if (high) {
          want = 0.0f;
          const float t = ease((fl - 260) / 30);
          const float sweep = fl < 290 ? 0.0f : std::sin((fl - 290) * 0.05f) * wb * 1.5f;
          at = {start.x + sweep, start.y + (2000.0f - start.y) * t};
        } else if (fl < 290) {
          const float t = ease((fl - 260) / 30);
          at = {start.x + (lifted.x - start.x) * t, start.y + (lifted.y - start.y) * t};
        } else if (!away) {
          const float t = ease((fl - 290) / 40);
          at = {lifted.x + (over.x - lifted.x) * t, lifted.y + (over.y - lifted.y) * t};
        }
        ui.alchemy.tool = 0;
        ui.alchemy.over = true;
        ui.alchemy.pressed = f == 260;
        ui.alchemy.down = true;
        ui.alchemy.atX = f == 260 ? grabAt.x : at.x;
        ui.alchemy.atY = f == 260 ? grabAt.y : at.y;
        ui.alchemy.tiltReq = want - reqd;
        reqd = want;
      }
      if (!invert) {
        if (f == 289 && !chemNoPour) g_shotJumpPath = "screenshot_bench_lift.bmp";
        if (f == 300 && !chemNoPour) g_shotJumpPath = "screenshot_bench_pour.bmp";
        if (f == 398 && !chemNoPour) g_shotJumpPath = "screenshot_bench_poured.bmp";
        if (f == 405) ui.alchemy.wantClose = true;
        if (f == 420) g_shotJumpPath = "screenshot_bench_closed.bmp";
      }
    }
    // --shot-dialogue's schedule (see g_shotDialogue).
    if (g_shotDialogue) {
      const uint64_t f = frameCounter;
      // On foot, looking level: the listener spawns "a few metres ahead",
      // and a first-person camera pitched at the sky photographs stars.
      if (f == 1) {
        ui.fly = false;
        player.fly = false;
      }
      if (f < kShotDlgBegin) cam.pitch = -0.12f;
      if (f == kShotDlgSpawn) {
        ui.visible = false;       // the dev panel is not in the picture
        ui.aiSpawnDummy = true;   // blind, never moves: stands and listens
      }
      // The listener is the creature the panel just spawned, by id: "nearest
      // within 12 m" depends on where the spawn landed, and a picture of no
      // conversation proves nothing. Same queue the dev hook uses.
      if (f == kShotDlgBegin) {
        dialogue::Speaker sp;
        if (!tickCtx.aiSpawnedMobs.empty()) {
          sp.mobId = tickCtx.aiSpawnedMobs.back();
          if (const Mob* m = mobs.FindMobById(sp.mobId); m && m->Def())
            sp.name = m->Def()->name;
        }
        session.talkBegin = dialogue::BeginRequest{true, sp, "sample_stranger"};
        std::printf("--shot-dialogue: talking to mob %llu\n",
                    (unsigned long long)sp.mobId);
      }
      // Once the panel has freed the cursor, park it off the choice rows (a
      // hovered row is a different picture). Not before: moving a CAPTURED
      // cursor is a look delta, and the camera would spin.
      if (f == kShotDlgBegin + 10 && !captured) glfwSetCursorPos(window, 40.0, 40.0);
      if (f == kShotDlgShot1) g_shotJumpPath = "screenshot_dialogue.bmp";
      // [continue] past the greeting to the question hub: the second picture
      // is the numbered choice rows.
      if (f == kShotDlgChoose) feeder.Talk(kTalkContinue);
      if (f == kShotDlgShot2) g_shotJumpPath = "screenshot_dialogue_2.bmp";
    }
    // --shot-devpanel: page k is selected at kShotDevFirst + k*step and shot
    // step-2 frames later (a page's first frame lays out before it settles).
    if (g_shotDevPanel && frameCounter >= kShotDevFirst) {
      static const char* const kPages[] = {
          "screenshot_devpanel_paint.bmp", "screenshot_devpanel_spawn.bmp",
          "screenshot_devpanel_world.bmp", "screenshot_devpanel_view.bmp",
          "screenshot_devpanel_magic.bmp", "screenshot_devpanel_debug.bmp",
          "screenshot_devpanel_spawn_pouch.bmp"};
      const uint64_t k = (frameCounter - kShotDevFirst) / kShotDevStep;
      const uint64_t r = (frameCounter - kShotDevFirst) % kShotDevStep;
      if (k < 7) {
        ui.visible = true;
        ui.devTab = k == 6 ? 1 : (int)k;
        if (k == 6 && r == 0)
          for (int i = 0; i < (int)ui.giveVesselNames.size(); i++)
            if (ui.giveVesselNames[i] == "pouch") ui.giveVesselPick = i;
        if (r == kShotDevStep - 2) g_shotJumpPath = kPages[k];
      }
    }
    // --shot-inventory's scripted schedule. Frame-counted rather than
    // wall-clocked so the same picture comes out on any machine.
    if (g_shotInventory) {
      // FLY MODE HAS NO BODY (see the avatar block in the tick loop): the rig
      // is despawned while flying, so a harness that left the game's default
      // fly=true would photograph an empty portrait frame and prove nothing.
      // Walking is also what the screen is normally opened from.
      if (frameCounter == 1) {
        ui.fly = false;
        player.fly = false;
      }
      if (frameCounter == kShotInvHudSetup) {
        ui.visible = false;
        ui.tool = UIState::kToolMelee;
        ui.magicMode = false;
        uint16_t water = 0;
        for (size_t m = 0; m < mats.size(); m++)
          if (mats[m].name == "water") water = (uint16_t)m;
        for (int i = 0; i < kItemSlots; i++) {
          const ItemDef* d = items.Of(hotbar.slots[i]);
          if (d && d->IsContainer() && water) {
            hotbar.slots[i].ClearFill();
            hotbar.slots[i].contents.Add(water, (uint32_t)(d->container.capacity * 2 / 3));
            hotbar.Select(i);
            break;
          }
        }
      }
      if (frameCounter == kShotInvOpenFrame) {
        // DRESS THE FIGURE BEFORE PHOTOGRAPHING IT. The portrait is the only
        // view of worn armour anybody looks at while iterating on it, and an
        // undressed portrait proves that the panel draws — which was never in
        // doubt. Every worn item in the library goes into the first slot that
        // accepts its kind; the wear sync in the tick loop does the rest, so
        // this harness drives the SAME path the player does rather than
        // reaching into the rig.
        int dressed = 0;
        for (int i = 0; i < (int)items.items.size(); i++) {
          const ItemDef& it = items.items[i];
          if (!ItemKindIsWorn(it.kind)) continue;
          for (int s = 0; s < kEquipSlotCount; s++)
            if (EquipSlotAccepts(s, it.kind) && kit.equip.At(s).Empty()) {
              kit.equip.slots[s] = StackOf(items, i);
              dressed++;
              break;
            }
        }
        if (dressed)
          std::printf("--shot-inventory: dressed the avatar in %d worn "
                      "pieces\n",
                      dressed);
        ui.inventoryOpen = true;
        // The dev panel is F1-hideable and sits ON TOP of the character
        // screen by design (a menu must not make F5 unreachable) — which
        // means it also sits on top of the thing this harness exists to
        // photograph. Hidden for the shot only.
        ui.visible = false;
        captured = false;
        captureBeforeUi = false;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      }
      if (frameCounter == kShotInvDamageFrame && avatar.Spawned()) {
        // ONE sever and a shallow bore. Deliberately survivable: a DEAD avatar
        // is despawned and respawned, so an overzealous script photographs an
        // empty frame — which is exactly what the first version of this did.
        // Between them the two produce a SEVERED row, a bleeding stump, and a
        // limb at full hp that has nonetheless lost voxels (the "% intact"
        // bar, the readout hp cannot produce and therefore the one most worth
        // having a picture of).
        avatar.SeverByName("hand.R");
        std::vector<ParticleSpawn> shotSpawns;
        Vec3 chest;
        const int torso = avatar.Parts().torso;
        if (torso >= 0 && avatar.PartAnchorWorld(torso, chest))
          avatar.CarveRadial(chest, 1.8f, world, shotSpawns);
        std::printf("--shot-inventory: severed hand.R, bored the torso "
                    "(health %d/%d)\n",
                    avatar.TotalHealth(), avatar.HealthMax());
      }
      // Swap to the inspector between the two captures, so the second picture
      // is the half the first cannot show.
      // ...with the bored torso already open in the health column, since that
      // is the limb this harness went to the trouble of damaging. The panel
      // itself opens on the worst limb when you press the button; setting it
      // here is how a scripted capture gets the same picture.
      if (frameCounter == kShotInvGearFrame + 1) {
        ui.inspectMode = true;
        ui.inspectSelected = UIState::kSlotTorso;
      }
      // Third picture: the grimoire with a page selected and its word row
      // populated — the panel's job is to be looked at (plan §12c).
      if (frameCounter == kShotInvCaptureFrame + 1) {
        ui.inspectMode = false;
        ui.grimoireMode = true;
        ui.spellbookOpen = true;
        if (!glyphs.conjoined.empty()) {
          const ConjoinedGlyph& cg = glyphs.conjoined[0];
          ui.grimoireSelected = cg.id;
          ui.grimoireEditName = cg.id;
          ui.grimoireEditWords.clear();
          for (int gi : cg.glyphs)
            if (const GlyphDef* d = glyphs.At(gi)) ui.grimoireEditWords.push_back(d->id);
        }
      }
      // Fourth picture: THE SPELL PAGE. A COPY of the `duststorm` starter —
      // starters are read-only and a page you cannot edit cannot show an edit —
      // with `shotgun` on it, so the tree has a bus (the shared sand and gusts),
      // a mod tag (`count x3`), three sockets and a price under every bar.
      if (frameCounter == kShotInvGraphSetup) {
        ui.grimoireMode = true;
        ui.lootOpen = false;
        ui.spellbookOpen = true;   // the book is shut by default; open it
        GrimoirePage p;
        p.name = "duststorm-mine";
        p.words = {"sand", "gust", "gust", "shotgun", "projectile"};
        if (caster.grimoire.Find(p.name) < 0) caster.grimoire.pages.push_back(p);
        ui.grimoireSelected = p.name;
        ui.grimoireEditName = p.name;
        ui.grimoireEditWords = p.words;
        ui.grimoireEditDirty = false;
        std::printf("--shot-inventory: composed [%s] for the spell page\n",
                    p.name.c_str());
      }
      // ...and ONE edit, applied through the intent latch exactly as a drop
      // would: a lane holding `fire` on the projectile. The box is found in the
      // MIRROR the panel is about to draw, which is the same lookup the drop
      // target does — the harness drives the player's path, it does not reach
      // into the tree.
      if (frameCounter == kShotInvGraphEdit) {
        int box = -1;
        for (const UIState::SpellGraphUI::Node& n : ui.spellGraph.nodes)
          // `primary < 0`: a split delivery is drawn once per branch and only
          // the primary cell owns the socket list this harness reads.
          if (n.kind == 2 /* Join */ && n.treeNode >= 0 && n.primary < 0) {
            box = (int)(&n - ui.spellGraph.nodes.data());
            break;
          }
        if (box >= 0) {
          // THE SOCKET THAT USED TO REFUSE. `shotgun` gives this box three
          // sockets over zero lanes, and the lane a socket drop asks for is
          // `instance + 1` — so the LAST instance is the drop that was
          // impossible until lanes learned to open in a run (2026-09-21), and
          // it is the picture worth reviewing. Read off the mirror's socket
          // list exactly as the canvas's drop target does.
          const UIState::SpellGraphUI::Node& bn = ui.spellGraph.nodes[box];
          int32_t lane = bn.laneCount + 1;
          if (!bn.sockets.empty()) {
            const UIState::SpellGraphUI::Node& sk =
                ui.spellGraph.nodes[(size_t)bn.sockets.back()];
            lane = sk.lane > 0 ? sk.lane : sk.instance + 1;
          }
          ui.graphEdit = {};
          ui.graphEdit.pending = true;
          ui.graphEdit.op = UIState::GraphEditIntent::Insert;
          ui.graphEdit.treeNode = bn.treeNode;
          ui.graphEdit.lane = lane;
          ui.graphEdit.glyphId = "fire";
          std::printf("--shot-inventory: spell page edit -> lane %d of [%s] "
                      "takes `fire`\n",
                      (int)lane, bn.label.c_str());
        } else {
          std::fprintf(stderr, "--shot-inventory: no join in the spell graph\n");
        }
      }
      // Fifth picture: a corpse with its gear on, opened. A human is spawned
      // a few paces ahead, dressed in every worn piece the library has and
      // handed a sword, and killed — the same dead Mob a fight produces — and
      // the panel is opened on it as E would.
      if (frameCounter == kShotInvGraphFrame + 1) {
        ui.grimoireMode = false;
        // Shut it again: the pictures after this one are about the corpse and
        // the body, and an open book covers the pack they are drawn beside.
        ui.spellbookOpen = false;
        int humanDef = -1;
        for (size_t i = 0; i < mobs.Defs().size(); i++)
          if (mobs.Defs()[i].name == "human") humanDef = (int)i;
        if (humanDef >= 0) {
          const MobDef& d = mobs.Defs()[humanDef];
          const Vec3 fwd = cam.Forward();
          const float dist = MetresToCells(2.0f);
          const int sx = ifloor(player.pos.x + fwd.x * dist) - d.prefab.size.x / 2;
          const int sz = ifloor(player.pos.z + fwd.z * dist) - d.prefab.size.z / 2;
          const int sy = World::TerrainHeight(sx + d.prefab.size.x / 2,
                                              sz + d.prefab.size.z / 2,
                                              kDefaultSeed) + 1;
          const uint64_t id = mobs.Spawn(humanDef, {sx, sy, sz});
          int dressed = 0;
          if (id) {
            Equipment worn;
            for (const ItemDef& it : items.items) {
              if (!ItemKindIsWorn(it.kind)) continue;
              const int slot = EquipSlotFor(it.kind, worn);
              if (slot < 0 || !worn.At(slot).Empty()) continue;
              if (mobs.WearItem(id, &it, slot)) {
                worn.slots[slot] = ItemInstance{it.name};
                dressed++;
              }
            }
            if (const ItemDef* sword = items.At(items.Find("sword")))
              mobs.EquipItem(id, sword);
            if (Mob* m = mobs.FindMobById(id)) m->Die();
          }
          Mob* c = id ? mobs.FindMobById(id) : nullptr;
          if (c && !c->Lootable()) c = nullptr;
          std::vector<LootPiece> pieces;
          if (c) {
            c->LootPieces(pieces);
            lootCorpse = id;
            ui.lootOpen = true;
            ui.lootTitle = c->Def() ? c->Def()->name : std::string();
          }
          std::printf("--shot-inventory: corpse of a human in %d worn pieces, "
                      "%zu lootable%s\n",
                      dressed, pieces.size(),
                      c ? "" : " (NO LOOTABLE CORPSE)");
        } else {
          std::fprintf(stderr, "--shot-inventory: no \"human\" mob def\n");
        }
      }
      // Fifth: THE DEATH SCREEN. The head comes off, which kills by
      // "vital limb destroyed" — the one death a number cannot describe and
      // the reason the portrait freezes a POSE rather than a picture.
      if (frameCounter == kShotInvDeathFrame && avatar.Spawned()) {
        ui.lootClose = true;
        ui.inspectMode = true;
        ui.grimoireMode = false;
        // THE PORTRAIT DRAWS EVERY BODY IN THE WORLD, and the loot corpse the
        // previous picture needed is standing two metres in front of the
        // player — which is exactly where this camera is. It photographed as a
        // second, hazy body laid over the first. Clearing debris first also
        // takes the hand severed at frame 170, so the only thing left for the
        // player's own limbs to be is the corpse this picture is about. The
        // loot corpse is a dead MOB now, not debris, so it goes separately.
        debris.Reset();
        for (uint32_t i = mobs.MobCount(); i-- > 0;) {
          const uint64_t mid = mobs.MobIdAt(i);
          if (mid && !mobs.IsAlive(mid)) mobs.RemoveMob(mid);
        }
        avatar.SeverByName("head");
        std::printf("--shot-inventory: took the player's head off "
                    "(health %d/%d, alive=%d)\n",
                    avatar.TotalHealth(), avatar.HealthMax(),
                    avatar.IsAlive() ? 1 : 0);
      }
      // A death does not open the screen any more, so the harness presses I
      // the way a player would — one frame later, so the death has been seen
      // by the tick loop and `ui.deathScreen` is set (which is what makes the
      // screen land on the health column).
      if (frameCounter == kShotInvDeathFrame + 2) {
        ui.inventoryOpen = true;
        ui.deathScreenOpened = false;
      }
      // Sixth: the same corpse, turned. If the pose override ever stops
      // working this picture is the one that says so — by then the real
      // corpse has been ragdolling for a second and is nowhere near this.
      if (frameCounter == kShotInvDeathTurnAt) ui.portraitYaw = 1.4f;
    }
    glfwPollEvents();
    double now = NowSeconds();
    float dt = (float)(now - lastTime);
    lastTime = now;
    // Scope timing is a branch on a bool unless someone is watching, and the
    // flag is re-read every frame because a client can attach mid-session.
    // `--frames` also opts in: it is the only non-interactive way to reproduce
    // the spikes the Performance tab shows, so it must record the same table.
    sandvox::PerfScopesEnable(telemetry.HasClient() || g_harnessFrames > 0);
    // ---- INPUT: everything from here to the fixed-tick loop ---------------
    // Key and mouse edge detection, the camera, the character screen and
    // hotbar, tool selection, and player.Update — movement integration plus the
    // voxel collision sweep against the CPU mirror. Per FRAME, not per tick.
    //
    // This span is the reason the row exists. `input` used to be
    // `wallMs - sum(everything else)`, so it was not a measurement of anything:
    // it was the frame's UNATTRIBUTED remainder wearing the name of the one
    // system that is genuinely cheap. Flying spiked it to 30 ms because the
    // toroidal window shift is 900 lines further down and had no timer on it.
    // The residual still exists — it is `other` now, and it says so.
    sandvox::PerfSpan spanInput(sandvox::PerfScope::Input);
    // Presented rate = frames / wall-clock over a window. An EMA of the
    // instantaneous 1/dt over-weights the fast frames whenever the CPU races
    // ahead of a GPU-bound present queue (several ~5 ms loops, one long
    // block), and reads 100+ while the screen updates at <10.
    // Skip the first 60 frames: worldgen and first-use pipeline creation are
    // startup cost, not the steady-state stall being measured.
    if (g_harnessFrames > 0 && frameCounter > 60) {
      g_frameMs.push_back(dt * 1000.0);
      // Snapshot-latent by ~2 ticks, which is irrelevant at percentile scale.
      if (world.Snap().valid)
        g_activeChunks.push_back((double)world.Snap().activeChunks);
      // Bucketed by the regime this frame was FLOWN in, which is the flag the
      // altitude pin used, not a re-derivation of the tick phase here — the
      // two would disagree on the frames straddling a phase flip.
      if (g_autoflySurface) {
        (g_autoflySurfaceHigh ? g_frameMsHigh : g_frameMsLow)
            .push_back(dt * 1000.0);
      }
    }
    fpsWinFrames++;
    fpsWinWorst = std::max(fpsWinWorst, (double)dt);
    // Ring, so the tail window is the last kTailWindow frames regardless of
    // how the 0.5 s reporting boundary happens to fall.
    if (tailRing.size() < kTailWindow) {
      tailRing.push_back((float)(dt * 1000.0));
    } else {
      tailRing[tailNext] = (float)(dt * 1000.0);
      tailNext = (tailNext + 1) % kTailWindow;
    }
    if (now - fpsWinStart >= 0.5) {
      fpsSmooth = (float)(fpsWinFrames / (now - fpsWinStart));
      frameMsSmooth = (float)(1000.0 * (now - fpsWinStart) / fpsWinFrames);
      frameMsWorst = (float)(fpsWinWorst * 1000.0);
      // Sort a COPY: the ring is in arrival order and stays that way, or the
      // next frame would overwrite whatever slot sorting moved into tailNext.
      tailSorted = tailRing;
      std::sort(tailSorted.begin(), tailSorted.end());
      const size_t n = tailSorted.size();
      frameMsP95 = tailSorted[(size_t)(0.95 * (double)(n - 1))];
      frameMsP99 = tailSorted[(size_t)(0.99 * (double)(n - 1))];
      fpsWinFrames = 0;
      fpsWinWorst = 0;
      fpsWinStart = now;
    }

    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    if (fbw > 0 && fbh > 0 && ((uint32_t)fbw != ctx.width || (uint32_t)fbh != ctx.height))
      ctx.Resize(fbw, fbh);
    // Present mode, applied only when the setting CHANGES (a swapchain
    // recreate drains the queue). Same spot as the resize because it is the
    // same operation, and no image is acquired here.
    {
      const int want = presentOverride >= 0 ? presentOverride
                                            : CurrentTuning().render.presentMode;
      if (want != presentApplied) {
        ctx.SetPresentMode((rhi::PresentMode)want);
        presentApplied = want;
      }
    }

    // ---- lab tuning watcher (plan §4.3) ----
    // Poll tuning.json's mtime at ~4 Hz and run the existing F5 path on any
    // change, so a tuner.html save reaches the running lab within a second
    // (the sim.fluid* WGSL consts recompile through ReloadShaders exactly as
    // a manual F5 would). The mtime is recorded HERE, before the reload runs,
    // so a slow reload cannot re-trigger itself.
    if (labScene >= 0 && now - labWatchPoll > 0.25) {
      labWatchPoll = now;
      const int64_t m = LabFileMtimeNs(labTuningPath);
      if (m >= 0 && m != labTuningMtime) {
        labTuningMtime = m;
        std::printf("lab: tuning.json changed on disk — reloading (F5 path)\n");
        ui.reloadShaders = true;
      }
    }

    // ---- input ----
    auto key = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };

    // ---- WHO IS LISTENING TO THE KEYBOARD ----------------------------------
    //
    // Three tiers, and the middle one is a bug fix that predates this screen:
    //
    //   uiTyping  an ImGui widget has keyboard focus (a text field, a slider
    //             being typed into). NOTHING game-side may fire. Until now
    //             io.WantCaptureKeyboard was never consulted anywhere, so
    //             typing "5" into a dev-panel field also switched the brush
    //             material and typing "b" placed a prefab.
    //   gameKeys  the world is being played: no menu, nothing focused.
    //   devKeys   F-keys, pause, step, reload. These stay live WITH the
    //             character screen open on purpose — F1/F5/F9 must not become
    //             unreachable because a menu is up.
    const bool uiTyping = overlay.WantsKeyboard();
    const bool devKeys = !uiTyping;
    const bool gameKeys = !ui.inventoryOpen && !ui.talk.open && !uiTyping;

    // I opens and closes the character screen. Opening frees the cursor and
    // remembers what capture WAS, so closing restores it rather than assuming.
    if (devKeys && eI.Pressed(key(GLFW_KEY_I))) {
      ui.inventoryOpen = !ui.inventoryOpen;
      if (ui.inventoryOpen) {
        captureBeforeUi = captured;
        captured = false;
      } else {
        captured = captureBeforeUi;
        ui.portraitYaw = 0.0f;  ui.portraitPitch = -0.08f;
        ui.portraitZoom = 1.0f; ui.portraitZoomTarget = 1.0f;
        ui.portraitPanX = 0.0f; ui.portraitPanXTarget = 0.0f;
        ui.portraitPanY = 0.0f; ui.portraitPanYTarget = 0.0f;
        ui.portraitPivotSlot = -1;
        ui.inspectSelected = -1;
        // Re-opening while dead lands on the health column again (see
        // DrawInventoryScreen): the frozen readout is the reason the screen
        // is up at all, and it is the wrong thing to have to re-find.
        ui.deathScreenOpened = false;
      }
      glfwSetInputMode(window, GLFW_CURSOR,
                       captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
      glfwGetCursorPos(window, &mx0, &my0);
    }
    // Esc CLOSES the screen when it is open, and otherwise does what it always
    // did. Escape meaning "back out of the thing in front of me" before it
    // means "let go of the mouse" is the order every game uses, and it is the
    // one that does not strand a player with a menu they cannot dismiss.
    if (devKeys && eEsc.Pressed(key(GLFW_KEY_ESCAPE))) {
      // A conversation first: Esc is "leave", when the node allows it (a
      // command for the tick, like every other answer).
      if (ui.talk.open) {
        if (ui.talk.canLeave) feeder.Talk(kTalkLeave);
      } else
      // The bench first: Esc puts the vessels back and leaves the screen up.
      if (ui.alchemy.open) {
        ui.alchemy.wantClose = true;
      } else if (ui.inventoryOpen) {
        ui.inventoryOpen = false;
        captured = captureBeforeUi;
        ui.portraitYaw = 0.0f;  ui.portraitPitch = -0.08f;
        ui.portraitZoom = 1.0f; ui.portraitZoomTarget = 1.0f;
        ui.portraitPanX = 0.0f; ui.portraitPanXTarget = 0.0f;
        ui.portraitPanY = 0.0f; ui.portraitPanYTarget = 0.0f;
        ui.portraitPivotSlot = -1;
        ui.inspectSelected = -1;
        // Re-opening while dead lands on the health column again (see
        // DrawInventoryScreen): the frozen readout is the reason the screen
        // is up at all, and it is the wrong thing to have to re-find.
        ui.deathScreenOpened = false;
      } else {
        captured = !captured;
      }
      glfwSetInputMode(window, GLFW_CURSOR,
                       captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
      glfwGetCursorPos(window, &mx0, &my0);
    }
    // ---- THE CONVERSATION (game/dialogue.h) ------------------------------
    // Opening frees the cursor for the choice rows and closing hands back
    // what it was (the captureBeforeUi rule). The answer -- a key or a click
    // on the panel (UIState::talk.pick) -- goes into the tick COMMAND; the
    // tick applies it (dialogue::TickSession), never this frame.
    if (ui.talk.open != talkWasOpen) {
      talkWasOpen = ui.talk.open;
      if (ui.talk.open) {
        captureBeforeTalk = captured;
        captured = false;
      } else {
        captured = captureBeforeTalk;
      }
      glfwSetInputMode(window, GLFW_CURSOR,
                       captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
      glfwGetCursorPos(window, &mx0, &my0);
    }
    if (ui.talk.open && !uiTyping) {
      for (int i = 0; i < 9; i++)
        if (eTalk[i].Pressed(key(GLFW_KEY_1 + i)) && i < (int)ui.talk.choices.size())
          feeder.Talk(i + 1);
      if (eTalkGo.Pressed(key(GLFW_KEY_SPACE) || key(GLFW_KEY_ENTER) ||
                          key(GLFW_KEY_KP_ENTER)) &&
          ui.talk.canContinue)
        feeder.Talk(kTalkContinue);
    }
    if (ui.talk.pick != 0) {
      feeder.Talk(ui.talk.pick);
      ui.talk.pick = 0;
    }
    // The wheel, drained once per frame. Three claimants, in priority order:
    //
    //   1. THE UI. The character screen, or any dev-overlay window the cursor
    //      is over. ImGui has already consumed the same event through its own
    //      callback (see ScrollCallback's note on the install order), so this
    //      only has to decline.
    //   2. THE CAMERA, whenever the view is not first person. A boom camera
    //      with no zoom is the one control every third-person game has and
    //      this did not.
    //   3. THE HOTBAR, which is what it has always done and what remains
    //      correct in first person, where there is no boom to move.
    {
      const double dy = g_scrollY;
      g_scrollY = 0.0;
      if (dy != 0.0 && !ui.inventoryOpen && !overlay.WantsMouse() && captured) {
        if (camMode != CameraMode::First) {
          tpRig.Zoom((float)dy);
        } else if (ui.magicMode) {
          // The spell bar: along the bank on screen, wrapping, as the item
          // hotbar scrolls.
          const int step = dy > 0 ? -1 : 1;
          const int cur = caster.selected >= 0 ? caster.selected : 0;
          const int bank = cur / kGlyphBank;
          const int col = ((cur % kGlyphBank) + step + kGlyphBank) % kGlyphBank;
          caster.SelectSlot(glyphs, bank * kGlyphBank + col);
          caster.selected = bank * kGlyphBank + col;   // an empty key is still a place on the bar
        } else {
          hotbar.Scroll(dy > 0 ? -1 : 1);
        }
      }
    }
    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    // --fell-tree with a site OWNS the camera: the first frame's cursor delta
    // (wherever the mouse happened to be when the window opened) turned the
    // view 88 degrees and planted the oak in the wrong place with 0 cells,
    // and a hand on the mouse mid-run would move the very view the drawBodies
    // number is measured through.
    //
    // THE OS CURSOR IS NOT INPUT TO A SCRIPTED HARNESS either. Under
    // SANDVOX_TICKS_PER_FRAME the delta is dropped for the camera and for the
    // command alike, so `--autofly-hard`'s path is the tick phase and nothing
    // else. A stray cursor event at tick 321 was the second reason two runs of
    // one binary produced different op records.
    const bool harnessInput = HarnessTicksPerFrame() > 0;
    if (captured && !g_fellSiteSet && !harnessInput)
      cam.ApplyMouse((float)(mx - mx0), (float)(my - my0));
    // CRAWLING LOCKS THE VIEW TO THE BODY (avatar.crawlLookYaw). Looking no
    // longer turns a crawling body (ResolveAvatarHeading), so the view is
    // held within a cone either side of it instead of looking through it.
    // Camera yaw -> rig heading is h = pi/2 - yaw.
    if (camMode == CameraMode::First && avatar.Spawned() &&
        avatar.LocoGroundAlign() >= 0.5f) {
      const float lim =
          CurrentTuning().avatar.crawlLookYaw * (3.14159265f / 180.0f);
      float off = (1.5707963f - cam.yaw) - avatarHeading;
      while (off > 3.14159265f) off -= 6.2831853f;
      while (off < -3.14159265f) off += 6.2831853f;
      if (std::fabs(off) > lim)
        cam.yaw = 1.5707963f - (avatarHeading + std::copysign(lim, off));
    }
    // The look delta is ACCUMULATED into the tick command (N2) rather than
    // delivered to a consumer here: the mouse is sampled per frame, and the
    // strike picker integrates one whole tick's pixels at kTickDt inside the
    // tick loop. It is the only swing consumer — the driver is fed by the
    // stroke program, never by the mouse (the freeform mode that was is gone).
    if (captured && !harnessInput)
      feeder.Look((float)(mx - mx0), (float)(my - my0));
    mx0 = mx;
    my0 = my;

    // The DEV tier: still live with the character screen open, dead while an
    // ImGui field has focus.
    if (devKeys && eP.Pressed(key(GLFW_KEY_P))) ui.paused = !ui.paused;
    if (devKeys && ui.devControls && eN.Pressed(key(GLFW_KEY_N))) ui.stepOnce = true;
    if (devKeys && ui.devControls && eV.Pressed(key(GLFW_KEY_V))) ui.fly = !ui.fly;
    if (devKeys && eF1.Pressed(key(GLFW_KEY_F1))) ui.visible = !ui.visible;
    // F2: PLAY <-> DEV controls (UIState::devControls). The transition itself
    // is applied below, where the checkbox path lands too.
    if (devKeys && eF2.Pressed(key(GLFW_KEY_F2))) ui.devControls = !ui.devControls;
    if (devKeys && eF3.Pressed(key(GLFW_KEY_F3)))
      ui.showCollisionBoxes = !ui.showCollisionBoxes;
    // F4 CYCLES the vector-field arrows: off -> wind (RESEARCH_wind.md §4.8)
    // -> water current (DESIGN.md §9d.8) -> off. Beside F3 because all of them
    // are the same kind of thing — a debug view of something the world is doing
    // invisibly — and free when off, since each draw is skipped at zero arrows.
    //
    // A cycle rather than two keys: the two fields are read by comparing them
    // (does the raft answer the wind or the water?), and drawing both lattices
    // into one frame is unreadable, so only one is ever live.
    if (devKeys && eF4.Pressed(key(GLFW_KEY_F4)))
      ui.fieldViz = (ui.fieldViz + 1) % UIState::kFieldVizCount;
    if (devKeys && eF5.Pressed(key(GLFW_KEY_F5))) ui.reloadShaders = true;
    if (devKeys && eF6.Pressed(key(GLFW_KEY_F6)))
      ui.showDirtyChunks = !ui.showDirtyChunks;
    // F7: reload the environment (biomes, world map, tree atlas) from disk
    // and regenerate. Beside F5 (shaders + tuning) and R (materials) because
    // it is the third kind of hot reload, and the one an Environment-tab save
    // needs -- worldgen reads those tables, so a reload without a regen would
    // show nothing and a regen without a reload shows the OLD tables.
    // F7 ALSO takes the F5 path first: worldgen's knobs are WGSL constants
    // baked into the kernel through the tuning prelude, so a regen on the old
    // kernel shows the old world -- which is the same argument as the
    // environment reload above, one level down. The reload block runs earlier
    // in this same frame than the regen block, so the order is right.
    if (devKeys && eF7.Pressed(key(GLFW_KEY_F7))) {
      ui.reloadShaders = true;
      ui.regenWorld = true;
    }
    if (devKeys && eF9.Pressed(key(GLFW_KEY_F9))) ui.saveWorld = true;
    if (devKeys && eF10.Pressed(key(GLFW_KEY_F10))) ui.loadWorld = true;
    if (devKeys && eR.Pressed(key(GLFW_KEY_R))) ui.reloadMaterials = true;
    if (gameKeys && ui.devControls && eLBracket.Pressed(key(GLFW_KEY_LEFT_BRACKET)))
      ui.brushRadius = std::max(1, ui.brushRadius - 1);
    if (gameKeys && ui.devControls && eRBracket.Pressed(key(GLFW_KEY_RIGHT_BRACKET)))
      ui.brushRadius = std::min(7, ui.brushRadius + 1);
    // The number row is SHARED: it picks a brush material normally and SPEAKS
    // glyphs in magic mode (Z). Both wanted 1-8 and the brush binding predates
    // magic, so a mode toggle is what keeps the existing tool usable rather
    // than silently stealing its keys.
    if (!ui.magicMode) {
      for (int i = 0; i < 8; i++)
        if (gameKeys && ui.devControls && key(GLFW_KEY_1 + i) &&
            i + 1 < (int)mats.size()) {
          if (ui.tool == UIState::kToolFluid)
            ui.fluidPour = i & 3;   // which liquid (TickAuthorityCtx::fluidPourMat)
          else
            ui.brushMaterial = i + 1;
        }
    } else {
      // THE PAGE IS THE INTERFACE (PLAN_spell_graph §0b). Pressing a number
      // SELECTS the spell bound to that key on the spell bar — a grimoire
      // page, or a single glyph as a one-word spell — and Q / E put it into a
      // hand, whose button then casts it as often as it is clicked (spells in
      // hand, below). Building the sentence happens on the page's tree, which
      // is a surface with room for it; the number row used to be the only
      // authoring tool there was, and it was a bad one.
      //
      // Edge-triggered still: a held key must not re-select every frame and
      // wipe a half-drained mana readout. TWO BANKS (plan §12a): `1`-`0` is
      // bank A, `Shift+1`-`0` bank B. Sprint is on Shift outside magic mode and
      // magic mode captures the number row, so nothing collides.
      const bool bankB = key(GLFW_KEY_LEFT_SHIFT) || key(GLFW_KEY_RIGHT_SHIFT);
      for (int i = 0; i < kGlyphBank; i++) {
        // GLFW's number row is contiguous 1..9 then 0, and slot 10 is the 0
        // key, matching the strip the HUD prints.
        int k = (i == 9) ? GLFW_KEY_0 : (GLFW_KEY_1 + i);
        if (captured && eGlyph[i].Pressed(key(k))) {
          caster.SelectSlot(glyphs, i + (bankB ? kGlyphBank : 0));
          caster.selected = i + (bankB ? kGlyphBank : 0);   // empty keys are places on the bar too
        }
      }
    }
    // Z OPENS THE SPELL BAR (spells in hand, 2026-09-27): the HUD's strip
    // shows the bound spells in place of the items, the number row and the
    // wheel move along it, and Q / E put the selected spell into a hand. The
    // selection survives closing the bar; what is in your hands is unaffected
    // by either (caster.h PlayerCaster::hand).
    if (captured && eZ.Pressed(key(GLFW_KEY_Z))) ui.magicMode = !ui.magicMode;
    // Abandon a half-spoken spell. Backspace rather than a letter: it is the
    // universal "undo what I just typed" key and the left hand is on WASD.
    if (captured && eBack.Pressed(key(GLFW_KEY_BACKSPACE))) caster.Clear(glyphs);

    // The dev grenade moved off G to J when G became take/throw (dual
    // wielding, 2026-09-27).
    if (captured && ui.devControls && eJ.Pressed(key(GLFW_KEY_J))) {
      Grenade g;
      g.pos = player.EyePos() + cam.Forward() * 2.0f;
      g.vel = cam.Forward() * (CurrentTuning().grenade.throwSpeed / kVoxelMeters) +
              player.vel;
      g.fuse = CurrentTuning().grenade.fuse;
      grenades.push_back(g);
    }
    if (captured && ui.devControls && eX.Pressed(key(GLFW_KEY_X)))
      ui.pendingDetonate = true;
    // ---- E: PICK IT UP ------------------------------------------------------
    //
    // A short camera ray filtered through the ground registry, which is what
    // makes this cheap: the ray finds the nearest DYNAMIC body (the same cast
    // the laser uses), and the registry answers "is that a thing, and which
    // thing". A body the registry does not know is scenery — a rock, a corpse,
    // a chunk of somebody's wall — and is left alone.
    //
    // THE RAY RUNS EVERY FRAME, NOT ONLY ON THE PRESS, because the prompt is
    // the feature: "E  pick up robe" under the crosshair is what tells the
    // player the thing on the floor is a thing at all. One Jolt ray cast per
    // frame against the moving layer is nothing next to the laser's.
    //
    // Reach, in world voxels. A literal rather than a tuning knob because it
    // is a HUMAN dimension, not a feel dial: the avatar is 17 voxels tall
    // (gen_human's height contract) and the ray starts at the EYE, so a thing
    // lying at your feet is a full body height away before you have bent
    // down — 24 is the floor a pace ahead, and not the far side of the room.
    constexpr float kPickupReach = 24.0f;
    // How far you may wander from a corpse with its panel open before it
    // closes: a few paces, the same "still standing over it" a pickup means.
    constexpr float kLootRange = 40.0f;
    lookBody = 0;
    lookUseRef = 0;
    ui.lookPrompt.clear();
    // ---- "fly to" from F1 -> World -> References (ui/refs_ui.cpp) --------
    // Fly mode, a few metres off the ref and above it, looking at it.
    // The container editor's item list (References page): the library's names.
    if (ui.itemLibraryNames.size() != items.items.size()) {
      ui.itemLibraryNames.clear();
      for (const ItemDef& d : items.items) ui.itemLibraryNames.push_back(d.name);
    }
    if (ui.refFlyTo) {
      ui.refFlyTo = false;
      ui.fly = true;
      const Vec3 at{ui.refFlyPos[0] + 0.5f, ui.refFlyPos[1] + 8.0f, ui.refFlyPos[2] + 0.5f};
      player.pos = at + Vec3{-28.0f, 14.0f, -28.0f};
      player.vel = Vec3{0, 0, 0};
      const Vec3 d = at - player.EyePos();
      const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
      if (len > 1e-3f) {
        cam.yaw = std::atan2(d.z, d.x);
        cam.pitch = std::asin(std::clamp(d.y / len, -1.0f, 1.0f));
      }
      player.ResetViewSmooth();
      player.SnapRender();
    }
    if (captured && !ui.inventoryOpen) {
      // TWO THINGS THIS RAY MUST NOT DO, both measured 2026-09-12 with a
      // walking avatar (a fly-mode harness has no rig and showed neither):
      //
      //  1. HIT THE PLAYER'S OWN HEAD. The eye sits inside the avatar's head
      //     collider, and Jolt reports a convex shape the ray starts in as a
      //     hit at fraction 0 — so from the player's eye EVERY cast answered
      //     "your own head", the prompt never appeared and E did nothing.
      //     The rig's live limb bodies (shells and the held item included)
      //     are excluded from the cast.
      //  2. MISS WHAT THE CROSSHAIR IS ON IN THIRD PERSON. The camera is on a
      //     boom metres behind the body; a ray from the head along the
      //     camera's forward runs parallel to the crosshair line but metres
      //     off it (16 of 16 corpse bodies missed). So the ray starts at the
      //     RENDER eye — the boom in Third / OverShoulder, the head in First
      //     — and a hit only counts if the point it lands on is within reach
      //     of the HEAD, so arm's length stays arm's length from the body.
      //
      // This is the one ray that reads the camera: it is a UI query against
      // Jolt bodies, not a sim input, so the "picking rays use player.EyePos
      // so the camera cannot change what the sim sees" contract in the camera
      // block below is not what it is protecting. `tpRig.EyePos()` is last
      // frame's boom, which is where the picture the player is aiming with
      // was drawn from.
      lookIgnore.clear();
      avatar.AppendLiveLimbBodies(lookIgnore);
      const Vec3 hand = player.EyePos();
      const Vec3 from = camMode == CameraMode::First ? hand : tpRig.EyePos();
      const Vec3 fwd = cam.Forward();
      const float castLen = kPickupReach + (from - hand).len();
      float frac = 1.0f;
      const uint64_t hit =
          phys.CastRayBody(from, fwd, castLen, frac, lookIgnore);
      if (hit && (from + fwd * (frac * castLen) - hand).len() <= kPickupReach)
        lookBody = hit;
      // The ground registry FIRST: a shed robe still lying beside the body
      // it came off reads as the robe, not as the corpse (ShedCorpseLoot).
      // Then a DEAD MOB under the crosshair — any of its limbs, shells or the
      // sword in its hand answers as the corpse (MobSystem::FindOwner).
      if (const WorldItem* w = lookBody ? ground.Find(lookBody) : nullptr) {
        ui.lookPrompt = "G  pick up " + w->name;
      } else if (const Mob* c = lookBody ? mobs.FindOwner(lookBody) : nullptr;
                 c != nullptr && c->Lootable()) {
        std::vector<LootPiece> pieces;
        c->LootPieces(pieces);
        const std::string name = c->Def() ? c->Def()->name : std::string();
        ui.lookPrompt = pieces.empty() ? name + "  -  nothing left on it"
                                       : "G  loot " + name;
      }
      // ...and a USABLE REFERENCE when nothing lying about claims the key: a
      // door, a chest, a villager (world/refs_game.h). The prompt says what
      // G will do; the press carries THIS ref's hash, so the tick acts on
      // what was promised (TickInput::useRef).
      if (ui.lookPrompt.empty() && tickCtx.refs != nullptr) {
        refs::RefCtx rc{&refStore, &mobs, &world, &stream.Store(), nullptr, tick};
        rc.phys = &phys;
        rc.debris = &debris;
        rc.mats = &tickCtx.mats;
        rc.items = &items;
        std::string p;
        if (const refs::Ref* r = refs::PickUsable(refStore, rc, from, fwd, hand, &p)) {
          lookUseRef = r->hash;
          ui.lookPrompt = "G  " + p;
        }
      }
      // ...and the second half of the same prompt: anything the grab would
      // accept says so, whether or not it is also an item. The prompt is how
      // the player finds out the key does two things at all.
      if (CurrentTuning().player.grabHoldTime > 0.0f) {
        if (const uint64_t g = GrabHold::Grabbable(debris, &mobs, lookBody)) {
          const float kg = phys.BodyMass(g);
          const float cap = CurrentTuning().player.grabMaxMass;
          char buf[96];
          if (cap > 0.0f && kg > cap)
            std::snprintf(buf, sizeof buf, "too heavy to lift  (%.0f kg)", kg);
          else
            std::snprintf(buf, sizeof buf, "hold G  move  (%.0f kg)", kg);
          if (!ui.lookPrompt.empty()) ui.lookPrompt += "   -   ";
          ui.lookPrompt += buf;
        }
      }
    }
    // While something is held the prompt is what is IN YOUR HANDS, not what
    // is behind it: the reach ray is still running (it has to, for the frame
    // after you let go) but the weight you are carrying is the useful readout.
    // ...and when the hold ended by itself (not by E), say why, once.
    if (std::string n = grab.TakeDropNote(); !n.empty()) {
      ui.kitMessage = n;
      ui.kitMessageAge = 0.0f;
    }
    if (grab.Active()) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "release G  -  carrying %.0f kg",
                    grab.MassKg());
      ui.lookPrompt = buf;
    }
    // WHAT A TAP OF E DOES — unchanged from when E was a plain press binding,
    // but now a lambda, because the key grew a second meaning (the hold-to-
    // drag block below) and the tap has to fire on the key-UP of a press that
    // was not spent grabbing.
    // ---- M9.4-D: THE ANSWER TO AN E ON SOMEBODY ELSE'S ITEM ------------
    //
    // A ghost item belongs to the peer: only the owner's copy of a body is
    // real, so taking one here would be inventing a sword out of a render
    // proxy and both machines would end up holding it. The E below therefore
    // SENDS A REQUEST for a ghost, and this is where the reply lands.
    //
    // `granted == 0` is a real and necessary answer, not an error: the owner
    // may have picked the thing up half a second before you asked. Without a
    // refusal the requester could never tell "you have it" from "there was
    // nothing there", and a ghost item nobody could ever take again would sit
    // on the ground for the rest of the session (debris.h, net::ItemGrant).
    //
    // The bag/hotbar code below is the SAME code the local branch of `takeE`
    // runs — deliberately duplicated rather than factored out, because the
    // two differ in what they must NOT do: the local branch destroys the body
    // (it owns it), this one must not (the owner already did, and its own
    // `BodyGone` removes the ghost).
    //
    // Both branches put the WHOLE ItemInstance in the kit (W2-M) — its colour,
    // what a vessel holds, what a worn piece has been through — through this
    // one helper: bag first, hotbar as the overflow, -1 when there is no room
    // or the name is not an item this library has.
    auto pocket = [&](const ItemInstance& it) -> int {
      const ItemStack s = KitStackFrom(it.One(), items);
      if (s.Empty()) return -1;
      int where = kit.bag.Add(s);
      if (where < 0) where = hotbar.Add(s);
      return where;
    };
    if (netPaced) {
      net::ItemGrant g;
      while (entities.PopLocalGrant(g)) {
        if (!g.granted) {
          ui.kitMessage = "somebody else got there first";
          ui.kitMessageAge = 0.0f;
          continue;
        }
        const int where = pocket(g.item);
        ui.kitMessage = where >= 0 ? "picked up " + g.item.name
                                   : "you have no room for that";
        ui.kitMessageAge = 0.0f;
      }
    }
    auto takeE = [&]() {
      const uint64_t hit = lookBody;
      const WorldItem* w = hit ? ground.Find(hit) : nullptr;
      const Mob* corpse = (w || !hit) ? nullptr : mobs.FindOwner(hit);
      if (corpse && !corpse->Lootable()) corpse = nullptr;
      std::vector<LootPiece> corpsePieces;
      if (corpse) corpse->LootPieces(corpsePieces);
      if (corpse && !corpsePieces.empty()) {
        // OPEN THE CORPSE: the character screen with its loot panel up. The
        // cursor dance is the I key's, and `lootOpenedScreen` remembers that
        // it was E who opened the screen so closing the loot closes it again.
        lootCorpse = corpse->Id();
        ui.lootRef.clear();   // a corpse, not a chest (world/refs_doors.h)
        ui.lootOpen = true;
        ui.lootTitle = corpse->Def() ? corpse->Def()->name : std::string();
        if (!ui.inventoryOpen) {
          lootOpenedScreen = true;
          ui.inventoryOpen = true;
          captureBeforeUi = captured;
          captured = false;
          glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
          glfwGetCursorPos(window, &mx0, &my0);
        }
      } else if (w && netPaced && debris.IsGhost(hit)) {
        // M9.4-D: NOT MINE — ASK. The ghost is deliberately NOT removed here:
        // removing it optimistically would leave a refused request with a
        // sword that exists on one machine and not the other, and no message
        // that would ever put it back. The reply is drained above.
        const uint64_t gid = debris.GlobalIdOf(hit);
        if (gid != 0 && debris.RequestItemTake(gid)) {
          ui.kitMessage = "reaching for " + w->name;
          ui.kitMessageAge = 0.0f;
        }
      } else if (w) {
        // Bag first, hotbar as the overflow. A full pack REFUSES rather than
        // silently swallowing or silently dropping: the item stays exactly
        // where it was, which is the only behaviour under which a pickup
        // cannot lose anything.
        // ...and in the colour it was lying there in (game/dye.h). The
        // registry carries the word because the art cannot: a dyed garment is
        // painted in neutral greys, so a pickup that forgot the dye would hand
        // back a grey tunic with nothing anywhere to say it had ever been red.
        // ...holding what it held when it went down, with the holes it had:
        // the registry entry IS an ItemInstance and it goes in whole.
        const int where = pocket(*w);
        if (where >= 0) {
          ui.kitMessage = "picked up " + w->name;
          // Order matters: the registry entry is dropped by the release hook
          // when the body goes, so this is one call, not two.
          debris.DestroyBody(hit);
        } else {
          ui.kitMessage = "you have no room for that";
        }
        ui.kitMessageAge = 0.0f;
      }
    };
    // ---- E: TAP TO TAKE, HOLD TO DRAG (game/grab.h) ------------------------
    //
    // The ORDER here is the whole design. `eHeld` accumulates while the key is
    // down; the grab latches mid-hold; the TAP fires on release and only if
    // the hold never latched. A press spent on a grab (or on dropping one) is
    // marked `eGrabbed`, so letting go of a carried crate does not also pocket
    // whatever has drifted under the crosshair.
    //
    // A press while already carrying is a DROP, and it is taken on the PRESS
    // rather than the release: waiting for the key-up would hold the thing
    // through the whole press and read as a stuck grab.
    //
    // Losing the mouse (Esc, the character screen) drops what is carried
    // rather than freezing it in mid-air — the key-up that would have released
    // it is never going to arrive.
    //
    // G IS THE KEY NOW (dual wielding moved E to the right hand). And with a
    // THROWABLE VESSEL in either hand the hold means THROW, not drag: past
    // the same hold time the press latches as a throw, TB_THROW is held from
    // there until the key comes up, and the tick winds up and lets go
    // (session.cpp THE THROW). A tap is still a take, so a flask in hand
    // does not stop you picking a sword up.
    {
      const Tuning::Player& tp = CurrentTuning().player;
      const bool eDown = captured && key(GLFW_KEY_G);
      const bool throwable = [&] {
        if (ui.tool != UIState::kToolMelee) return false;
        for (int hk = 0; hk < kHands; hk++) {
          const ItemDef* d = items.Of(kit.equip.InHand(HandAt(hk)));
          if (d && d->IsContainer() && ContainerThrowable(*d)) return true;
        }
        return false;
      }();
      if (eDown && !ePrevDown) {
        eHeld = 0.0f;
        eGrabbed = false;
        gThrowHold = false;
      }
      if (eDown) eHeld += dt;
      if (!captured && grab.Active()) grab.Release(phys);
      if (eDown && !eGrabbed && throwable && !grab.Active() &&
          eHeld >= std::max(tp.grabHoldTime, 0.05f)) {
        gThrowHold = true;
        eGrabbed = true;   // the press is spent on the throw, not a take
      }
      if (!eDown || !throwable) gThrowHold = false;
      // `> 0` and not `>= 0`: at a hold time of zero the grab would latch on
      // the first frame the key is down and the TAP could never fire, so zero
      // is the off switch (tuning.h says so) rather than a hair trigger.
      if (eDown && !eGrabbed && tp.grabHoldTime > 0.0f &&
          eHeld >= tp.grabHoldTime) {
        if (grab.Active()) {
          grab.Release(phys);
          eGrabbed = true;
        } else if (const uint64_t g = GrabHold::Grabbable(debris, &mobs, lookBody)) {
          if (grab.Begin(phys, g, tp, player.EyePos(), playerBody)) {
            eGrabbed = true;
          } else if (grab.RefusedTooHeavy()) {
            ui.kitMessage = "too heavy to lift";
            ui.kitMessageAge = 0.0f;
            eGrabbed = true;  // the refusal IS the answer; do not also take it
          }
        }
      }
      // A tap USES the reference the prompt named when there is one (the use
      // verb, world/refs_game.h), else takes / loots as it always did.
      if (!eDown && ePrevDown && !eGrabbed && captured) {
        if (lookUseRef != 0) feeder.Use(lookUseRef);
        else takeE();
      }
      if (!eDown) eHeld = 0.0f;
      ePrevDown = eDown;
    }
    if (captured && ui.devControls && eTab.Pressed(key(GLFW_KEY_TAB))) {
      ui.tool = (ui.tool + 1) % UIState::kToolCount;
      if (ui.tool == UIState::kToolFluid && fluidCueMat != 0)
        ui.brushMaterial = (int)fluidCueMat;
    }
    if (captured && ui.devControls && eM.Pressed(key(GLFW_KEY_M))) feeder.Press(TB_SPAWN);
    if (captured && ui.devControls && eB.Pressed(key(GLFW_KEY_B))) feeder.Press(TB_PLACE);
    if (captured && ui.devControls && eK.Pressed(key(GLFW_KEY_K))) ui.spawnSphere = true;
    // U clears the experimental MLS-MPM fluid (sticky flag, consumed in the
    // tick loop like every other one-shot input — see the cast-key note).
    if (captured && ui.devControls && eU.Pressed(key(GLFW_KEY_U))) ui.clearFluid = true;
    // L (lab only) resets the scene: scene clock to zero + fluid cleared, so
    // the next tick re-submits the build CellOps and the pour replays from
    // its fixed schedule — an identical A/B run without regenerating the
    // world (plan §4.2's reset key).
    if (labScene >= 0 && captured && eL.Pressed(key(GLFW_KEY_L))) {
      labTick = 0;
      ui.clearFluid = true;
      std::printf("lab: scene reset (%s)\n", LabSceneName(labScene));
    }
    // C cycles first -> third -> over-shoulder. Snapping the rig on a change
    // stops the boom easing across the world when the mode flips.
    if (captured && eC.Pressed(key(GLFW_KEY_C))) {
      camMode = (CameraMode)(((int)camMode + 1) % (int)CameraMode::Count);
      tpRig.Snap();
    }
    // H severs the next intact part, worst-case first: a debug driver for the
    // dismemberment states that does not need a weapon pointed at yourself.
    // The order walks DOWN the state ladder (hand -> arm -> foot -> leg ->
    // head), so repeated presses march through limp, hop, crawl and squirm.
    if (captured && ui.devControls && eH.Pressed(key(GLFW_KEY_H)) &&
        avatar.Spawned()) {
      static const char* kSeverOrder[] = {
          "staff",  "hand.R", "hand.L", "armL.R", "armL.L",
          "foot.R", "foot.L", "legL.R", "legL.L", "armU.R",
          "armU.L", "legU.R", "legU.L", "head"};
      for (const char* nm : kSeverOrder)
        if (avatar.SeverByName(nm)) break;
    }
    if (gameKeys && ui.devControls && ui.tool == UIState::kToolPrefab &&
        eT.Pressed(key(GLFW_KEY_T)))
      ui.prefabRot = (ui.prefabRot + 1) & 3;
    if (gameKeys && ui.devControls && ui.tool == UIState::kToolPrefab &&
        eO.Pressed(key(GLFW_KEY_O)) && !prefabs.empty())
      ui.prefabSelected = (ui.prefabSelected + 1) % (int)prefabs.size();

    // MOVEMENT WAS NEVER GATED. It predates every other binding here and was
    // read straight off the keyboard, so WASD walked the player while a dev
    // panel field had focus and would walk them around behind the character
    // screen. `gameKeys` is the fix, and the axes are left at zero rather than
    // frozen so the controller decelerates properly instead of holding the
    // last input.
    //
    // THE ONE PLACE THE KEYBOARD IS READ FOR MOVEMENT. `pin` is this FRAME's
    // sample; it is pushed into the feeder at the bottom of this block and the
    // tick loop takes it from there. Nothing below the input span calls
    // glfwGetKey (N2: `grep glfwGet src/game/` is empty and the frame layer is
    // the only reader).
    TickInput pin;
    if (gameKeys) {
      pin.forward = (key(GLFW_KEY_W) ? 1.f : 0.f) - (key(GLFW_KEY_S) ? 1.f : 0.f);
      pin.strafe = (key(GLFW_KEY_D) ? 1.f : 0.f) - (key(GLFW_KEY_A) ? 1.f : 0.f);
      pin.SetHeld(TB_JUMP, key(GLFW_KEY_SPACE));
      pin.SetHeld(TB_CROUCH, key(GLFW_KEY_LEFT_CONTROL));
      pin.SetHeld(TB_SPRINT, key(GLFW_KEY_LEFT_SHIFT));
      pin.SetPressed(TB_JUMP, eJump.Pressed(key(GLFW_KEY_SPACE)));
    } else {
      // Keep the jump edge fed with `false` so a space held THROUGH a menu
      // does not read as a fresh press the instant it closes.
      eJump.Pressed(false);
    }
    // --shot-jump: walk forward and jump once, with nobody at the keyboard.
    //
    // A WALKING jump, not a standing one, because the travel lean is part of
    // the pose (avatar.airPoseLean) and a standing jump shows none of it — the
    // picture would be of half the feature. Sprint is off: a sprint jump is the
    // loudest version of the lean and therefore the least useful one to judge
    // the neutral shape from.
    if (g_shotJump) {
      if (frameCounter == 1) {
        // The dev panel covers the half of the frame the body walks through,
        // and it is the same reason --shot-inventory hides it: a harness whose
        // whole output is a picture must not photograph the debug overlay.
        ui.visible = false;
        // MID-MORNING, NOT MIDNIGHT. A new world starts just after midnight,
        // so the default picture is an unlit silhouette — which is useless for
        // judging a POSE, the one thing this harness exists to show. The
        // celestial clock is the game's own way to move the sun (the dev
        // panel's time slider drives it); engage it with a scale change and
        // then place it, rather than fast-forwarding thousands of ticks.
        CelestialClock& sky = Celestial();
        sky.SetScale(2.0f, tick);   // any value but 1.0 engages the clock
        sky.SetScale(1.0f, tick);   // ...then back to real time, sun placed
        const int64_t tpd = (int64_t)TicksPerDay(CurrentTuning());
        sky.ticks = tpd * 42 / 100;   // ~10 a.m.: raking light, clear shapes
        sky.prevTicks = sky.ticks;
        sky.rem = 0;
      }
      player.fly = false;
      ui.fly = false;
      // Third person, or there is nothing to photograph: first person hides
      // the body and keeps only the arms.
      camMode = CameraMode::Third;
      // RUN DIAGONALLY, so the camera gets a THREE-QUARTER view for free.
      //
      // In third person the body faces where it RUNS, not where the camera
      // looks (avatar body facing, thirdperson.h), and the boom stays behind
      // the CAMERA. So a forward+strafe input turns the body ~40 degrees out of
      // the view and the picture shows the fore/aft leg scissor and the arms'
      // depth — both of which a dead-astern shot flattens away entirely, and
      // the scissor is most of what tells the four shapes apart.
      // Nearly side-on (~70 degrees out of the view): a jump is read off its
      // PROFILE — knee tuck, hip angle, the arms' fore/aft — and every one of
      // those is a depth the camera flattens from behind.
      pin.forward = 0.4f;
      pin.strafe = 1.f;
      pin.SetHeld(TB_SPRINT, false);
      cam.yaw = 0.6f;
      cam.pitch = -0.12f;
      pin.SetPressed(TB_JUMP, frameCounter == kShotJumpAtFrame);
    }
    // --autofly: hold W+sprint in fly mode, no human at the keyboard. Exists to
    // reproduce the streaming-shift stutter, which only appears when the window
    // origin moves several chunks per second.
    if (g_autoWalk) {
      player.fly = false;
      ui.fly = false;
      pin.forward = 1.f;
      pin.strafe = 0.f;
      pin.SetHeld(TB_SPRINT, false);
      static uint32_t lastHopTick = ~0u;
      if (tick % 45u == 0u && lastHopTick != tick) {
        pin.SetPressed(TB_JUMP, true);
        lastHopTick = tick;
      }
      // A fixed tick schedule, like the autofly phases: reproducible run to run.
      cam.yaw = 2.35f + (float)((tick / 240u) % 4u) * 1.5707963f;
    }
    // --forest-fire: one full turn over the measured 600 ticks, level-ish, so
    // the frame is the fire on every side and not the side the spawn faced.
    if (g_forestFire && tick >= (uint32_t)g_forestFireAt + 600u) {
      const uint32_t t = tick - ((uint32_t)g_forestFireAt + 600u);
      cam.yaw = 6.2831853f * (float)(t % 600u) / 600.0f;
      cam.pitch = 0.08f;
    }
    if (g_autofly) {
      player.fly = true;
      ui.fly = true;
      pin.forward = 1.f;
      pin.strafe = 0.f;
      pin.SetHeld(TB_SPRINT, true);
      // --autofly-hard: the ADVERSARIAL traversal, which is what actually sizes
      // the pool (production streaming guidance is explicit that teleports,
      // 180-degree turns and fast diagonal traversal define a pool, not steady
      // state — and a window shift is structurally a teleport). Strafes and
      // descends at the same time, so all three axes shift together and the
      // window drives DOWN into solid underground bulk, where almost every
      // chunk needs a real page instead of an EMPTY sentinel.
      if (g_autoflyHard) {
        // Turn on a fixed tick schedule, never on wall-clock: this has to be
        // reproducible run to run.
        const uint32_t phase = (uint32_t)(tick / 90u) & 3u;
        pin.strafe = (phase == 1) ? 1.f : (phase == 3) ? -1.f : 0.f;
        // descend into solid rock: worst case for residency
        pin.SetHeld(TB_CROUCH, true);
      }
      // --autofly-surface: the RENDERER's worst case. Fly forward over the
      // terrain at two altitudes on the same fixed `tick/90` phase form the
      // hard descent uses, so the two harnesses are directly comparable and
      // both are reproducible run to run.
      //
      // WHY THE ALTITUDE IS HELD ANALYTICALLY, not by the space/ctrl bits: the
      // quantity under test is RAY LENGTH THROUGH UNSKIPPED CHUNKS, which is a
      // function of height above the terrain, and a fly-mode climb driven by an
      // input axis wanders with frame time. World::TerrainHeight is the exact
      // integer CPU mirror of worldgen's baseHeight (world.cpp), so the target
      // is a pure function of (x, z, seed) — no world reads, no residency
      // dependence, nothing that could vary between a paged and a dense run
      // being differenced. The pin itself is applied AFTER player.Update (see
      // the autofly-surface block down at the Update call), because fly mode
      // integrates velocity and would otherwise fight the assignment.
      //
      // Phase bit alternates the two regimes that bracket the collapse:
      //   low skim   — just over the canopy: micro/strand and half-full foliage
      //                chunks dominate, rays are short but never skippable.
      //   high cruise— well above everything: rays run the full window diagonal
      //                and the altitude term is the whole cost.
      if (g_autoflySurface) {
        const uint32_t phase = (uint32_t)(tick / 90u) & 1u;
        pin.SetHeld(TB_JUMP | TB_CROUCH, false);
        g_autoflySurfaceHigh = (phase == 1u);
        // Park: hold ONE regime and stop the forward axis. The regime hop is
        // 115 voxels, i.e. a 7-chunk vertical shift, so leaving it alternating
        // would keep the streaming wake this probe exists to remove.
        if (g_autoflyPark && tick >= ParkFlyTicks()) {
          pin.forward = 0.f;
          pin.SetHeld(TB_SPRINT, false);
          g_autoflySurfaceHigh = false;
        }
      }
    }
    // ---- THIS FRAME'S SAMPLE, INTO THE COMMAND -----------------------------
    // Held state and the axes REPLACE what is pending (the newest keyboard
    // sample wins, so a key released between two ticks reads as released);
    // the jump EDGE is OR-ed in and waits for a tick to take it. Every other
    // edge is pressed at its own binding further down this block.
    feeder.SetAxes(pin.forward, pin.strafe);
    feeder.SetHeldMask(pin.held);
    if (pin.Pressed(TB_JUMP)) feeder.Press(TB_JUMP);

    if (ui.reloadShaders) {
      ui.reloadShaders = false;
      // Tuning feeds the shader constant prelude, so re-read it first — this
      // is what makes F5 a one-key apply for everything in tuning.json, both
      // the WGSL constants and the CPU-side gameplay values.
      {
        Tuning tune;
        LoadTuning(assetDir + "/materials/tuning.json", tune);
        for (const std::string& w : tune.warnings)
          std::fprintf(stderr, "tuning: %s\n", w.c_str());
        // Lab: the excite/settle loop stays on across reloads (the same
        // runtime force the startup load applies — the file's shipped
        // default is not the lab's default). Track the mtime we just
        // consumed so the watcher and the ImGui writeback agree on what
        // "newer on disk" means.
        if (labScene >= 0) {
          tune.sim.fluidExciteMode = 1;
          labTuningMtime = LabFileMtimeNs(labTuningPath);
        }
        SetCurrentTuning(tune);
        {
          const auto& fs = tune.sim;
          ui.fGravity     = fs.fluidGravity;
          ui.fStiffness   = fs.fluidStiffness;
          ui.fRestDensity = fs.fluidRestDensity;
          ui.fEosPower    = fs.fluidEosPower;
          ui.fCohesion    = fs.fluidCohesion;
          ui.fAttractSame = fs.fluidAttractSame;
          ui.fAttractDiff = fs.fluidAttractDiff;
          ui.fViscosity   = fs.fluidViscosity;
          ui.fDamping     = fs.fluidDamping;
          ui.fSplashRate       = fs.fluidSplashRate;
          ui.fSplashSpeed      = fs.fluidSplashSpeed;
          ui.fSplashMaxDensity = fs.fluidSplashMaxDensity;
          ui.fSplashLife       = fs.fluidSplashLife;
          ui.fSplashScaleIdx   = fs.fluidSplashScaleIdx;
          ui.fFoamRate         = fs.fluidFoamRate;
          ui.fFoamCrestRate    = fs.fluidFoamCrestRate;
          ui.fTrappedMin       = fs.fluidTrappedMin;
          ui.fTrappedMax       = fs.fluidTrappedMax;
          ui.fCrestMin         = fs.fluidCrestMin;
          ui.fCrestMax         = fs.fluidCrestMax;
          ui.fFoamEnergyMin    = fs.fluidFoamEnergyMin;
          ui.fFoamEnergyMax    = fs.fluidFoamEnergyMax;
          ui.fFoamLife         = fs.fluidFoamLife;
          ui.fFoamLifeMin      = fs.fluidFoamLifeMin;
          ui.fBubbleBuoyancy   = fs.fluidBubbleBuoyancy;
          ui.fFoamDrag         = fs.fluidFoamDrag;
          ui.fBubbleDensity    = fs.fluidBubbleDensity;
          ui.fSprayDensity     = fs.fluidSprayDensity;
          ui.fFoamScaleIdx     = fs.fluidFoamScaleIdx;
          ui.fExciteMode       = fs.fluidExciteMode;
          ui.windGasScale      = fs.windGasScale;
          ui.windPartScale     = fs.windPartScale;
          ui.windDragRef       = fs.windDragRef;
          ui.fSettleEps        = fs.fluidSettleEps;
          ui.fWakeSpeed        = fs.fluidWakeSpeed;
          ui.fSettleTicks      = fs.fluidSettleTicks;
          const auto& fr = tune.render;
          ui.fSurface      = fr.fluidSurface;
          ui.fIso          = fr.fluidIso;
          ui.fSmooth       = fr.fluidSmooth;
          ui.fIor          = fr.fluidIor;
          ui.fClarity      = fr.fluidClarity;
          ui.fReflect      = fr.fluidReflect;
          ui.fSpecular     = fr.fluidSpecular;
          std::memcpy(ui.fShallow, fr.fluidShallow, sizeof(ui.fShallow));
          std::memcpy(ui.fDeep,    fr.fluidDeep,    sizeof(ui.fDeep));
          ui.fDepth        = fr.fluidDepth;
          ui.fGradientStr  = fr.fluidGradient;
          ui.fRFoam        = fr.fluidFoam;
          ui.fRFoamField   = fr.fluidFoamField;
          ui.fRFoamTexture = fr.fluidFoamTexture;
          ui.fRFoamSpeed   = fr.fluidFoamSpeed;
          ui.fWobble       = fr.fluidWobble;
          ui.fParticleSize = fr.fluidParticleSize;
          ui.fStretch      = fr.fluidStretch;
          ui.fDensityShade = fr.fluidDensityShade;
        }
        avatarDefName = CurrentTuning().player.model;
        // The field overlay is reachable two ways and F5 is where they meet:
        // the tuning bools are the authored state and F4 cycles from there, so
        // reloading re-seeds the cycle rather than leaving the key and the
        // file quietly disagreeing about which arrows are on.
        ui.fieldViz = FieldVizFromTuning(CurrentTuning());
        // Gore variance is drawn per mob at spawn, so mobs already standing in
        // the world hold profiles from the OLD tuning. Re-draw them here or an
        // edit to the randomness controls appears to do nothing until the next
        // spawn. Same id -> same draw, so a mob keeps its identity unless the
        // variance settings themselves changed.
        mobs.RefreshGoreProfiles();
        // THE STROKE'S FEEL, re-applied from the freshly-loaded `melee.*`
        // group. This is the whole reason MeleeTuning's values moved into
        // tuning.json: MeleeState holds a MeleeTuning by value, so without
        // this line an edit to a swing knob would need a rebuild — which is
        // the loop the migration exists to close (game/melee.h
        // ApplyMeleeTuning). `combatfx.*` needs no equivalent: nothing caches
        // it, every reader goes through CurrentTuning() at the point of use.
        ApplyMeleeTuning(melee.tuning);
        // The weather switches decide which reaction rules COMPILE, and the
        // reaction table is built by LoadAssets — which F5 does not otherwise
        // run. Fall through into the materials reload so a freeze/melt
        // checkbox applies on the same keypress as everything else in
        // tuning.json. The materials block is the next statement, so this
        // lands in the right order: tuning is live before LoadAssets reads it.
        ui.reloadMaterials = true;
        // The edit layer is NOT re-read here any more: it belongs to the map
        // (map.json `editLayer`, P7) and is re-read by ReloadEnvironment, i.e.
        // by F7, which also regenerates the window it applies to.
      }
      std::printf("reloading shaders... %s\n",
                  sim.ReloadShaders(ctx.device) ? "ok" : "FAILED (kept old)");
    }
    if (ui.reloadMaterials) {
      ui.reloadMaterials = false;
      // The weather presets reload with the other R-reloaded content: they are
      // authored data next to materials, and a tuner edit of a sky should show
      // on the same keypress.
      weather::Presets().Reload();
      // ...and the map's references: a hand edit of a group file shows on
      // the same key (world/refs.h RefStore::Reload re-applies what changed).
      if (tickCtx.refs != nullptr) {
        refs::RefCtx rc{&refStore, &mobs, &world, &stream.Store(), nullptr, tick};
        rc.phys = &phys;
        rc.debris = &debris;
        rc.mats = &tickCtx.mats;
        rc.items = &items;
        refStore.Reload(rc);
      }
      std::vector<MaterialDef> newMats;
      std::vector<ReactionGpu> newReactions;
      // THE ALCHEMY BENCH: its substances, rules and ledger are indexed by the
      // material ids of the table it opened with (alchemy_bench.h Open), and
      // it validates its tally against `mats` when it closes. A reload that
      // MOVES ids (a material inserted, removed or renamed) under an open
      // bench would validate it against the wrong rows -- so that reload
      // waits: the bench is closed (written back normally) this frame and the
      // reload runs on the next. A reload that keeps every id (a tuning F5, a
      // colour or rule edit) goes ahead under the open bench, which keeps
      // running on the rules it copied at Open.
      auto idsMoved = [&](const std::vector<MaterialDef>& nm) {
        if (nm.size() != mats.size()) return true;
        for (size_t i = 0; i < nm.size(); i++)
          if (nm[i].name != mats[i].name) return true;
        return false;
      };
      bool loaded = LoadAssets(assetDir + "/materials/materials.json",
                               assetDir + "/materials/reactions.json", newMats, newReactions,
                               errors);
      if (loaded && bench.IsOpen() && idsMoved(newMats)) {
        std::fprintf(stderr, "materials reload moves material ids: closing the alchemy "
                             "bench first, reloading next frame\n");
        ui.alchemy.wantClose = true;
        ui.reloadMaterials = true;
        loaded = false;
      }
      if (loaded) {
        mats = std::move(newMats);
        reactions = std::move(newReactions);
        {
          std::string serr;
          std::vector<SoluteDef> ns;
          if (LoadSolutes(assetDir + "/materials/solutes.json", mats, ns, serr)) benchSolutes = std::move(ns);
          else std::fprintf(stderr, "solutes: %s", serr.c_str());
        }
        // Micro bricks BEFORE the table upload: LoadMicroVox sets MATF_MICRO on
        // `mats`, and the flag has to be in the buffer the raymarcher reads or
        // an edited "micro" block would silently do nothing until a restart.
        // It also has to precede stream.OnMaterialsReloaded, which mirrors
        // isRayBlocker (and that now depends on the flag).
        {
          std::string mvlog;
          LoadMicroVox(assetDir + "/materials/materials.json", assetDir, mats, micro,
                       mvlog);
          if (!mvlog.empty()) std::fprintf(stderr, "%s", mvlog.c_str());
          sim.UploadMicro(ctx.queue, micro);
        }
        sim.UploadTables(ctx.queue, mats, reactions);
        debris.OnMaterialsReloaded(mats, reactions);
        stream.OnMaterialsReloaded(mats);
        // Glyphs reload with materials, and MUST: a glyph holds a resolved
        // 12-bit material id, so a materials edit that reorders the file would
        // otherwise leave every element glyph naming the wrong matter. A failed
        // reload keeps the old library rather than leaving the player with no
        // magic — the diagnostic is the fix path.
        {
          GlyphLibrary next;
          std::string gerr;
          // WHAT WAS BOUND, BY NAME, BEFORE THE INDICES DIE. A glyph slot
          // holds an index into GlyphLibrary::glyphs, which is file-order
          // dependent — so an edit that merely REORDERS glyphs.json silently
          // rebinds every key to a different spell. Until the character screen
          // existed the point was moot (GrantAllAndBind overwrote the
          // bindings anyway, which is its own bug: every reload threw away
          // whatever the player had arranged). Snapshot, reload, re-resolve.
          std::vector<std::string> boundNames(kGlyphSlots), boundPages(kGlyphSlots);
          for (int i = 0; i < kGlyphSlots; i++) {
            const int gi = caster.inventory.At(i);
            if (gi >= 0 && gi < (int)glyphs.glyphs.size())
              boundNames[i] = glyphs.glyphs[gi].id;
            boundPages[i] = caster.inventory.PageAt(i);
          }
          if (LoadGlyphs(assetDir + "/spells/glyphs.json", mats, next, gerr)) {
            glyphs = std::move(next);
            spells.Clear();          // live projectiles hold stale glyph indices
            caster.inventory.GrantAllAndBind(glyphs);   // acquisition placeholder
            // Re-bind by name over the identity mapping GrantAllAndBind just
            // laid down. A name that no longer exists leaves the slot EMPTY
            // rather than pointing at whatever now occupies that index — the
            // same rule the item hotbar's re-validation below uses.
            for (int i = 0; i < kGlyphSlots; i++) {
              if (!boundPages[i].empty()) {
                caster.inventory.BindPage(i, boundPages[i]);   // pages are names already
                continue;
              }
              if (boundNames[i].empty()) {
                caster.inventory.Bind(i, -1);
                continue;
              }
              caster.inventory.Bind(i, glyphs.Find(boundNames[i]));
            }
            caster.Clear(glyphs);
            caster.RefreshHands(glyphs);   // re-speak the hands' spells by the new indices
            std::printf("glyphs reloaded (%zu)\n", glyphs.glyphs.size());
          } else {
            std::fprintf(stderr, "glyph reload failed:\n%s", gerr.c_str());
          }
          // Either way: the materials changed under the glyph mirrors (their
          // swatches are material colours) even when the glyphs did not.
          glyphEpoch++;
        }
        // prefabs hot-reload with materials: palette indices may map now
        std::string plog;
        LoadPrefabDir(assetDir + "/prefabs", mats.size(), prefabs, plog);
        if (!plog.empty()) std::fprintf(stderr, "%s", plog.c_str());
        ui.prefabNames.clear();
        for (const Prefab& p : prefabs) ui.prefabNames.push_back(p.name);
        if (ui.prefabSelected >= (int)prefabs.size()) ui.prefabSelected = 0;
        // mob defs too (tuning dummy.json live is the test loop); live mobs
        // reference the old defs by index, so they respawn fresh
        // EVERY HOLDER OF A MODEL INDEX DIES BEFORE THE TABLE IT INDEXES.
        //
        // A micro model index is a POSITION in `mbSet.models`, and the rebuild
        // below throws that vector away and packs a new one. Live mobs had to
        // go anyway (they hold DEF indices, hence the old `mobs.Reset()` here);
        // debris bodies and the avatar hold BRICK indices and were being left
        // behind — so every corpse, gib, severed limb and dropped item on the
        // ground came out of the reload drawing whatever def happened to land
        // at its old position. That is the limb swap that SURVIVED the fix to
        // the sever path (92fb188): a leg redrawn as a torso, your arm as a
        // zombie's head, arriving on an R / F5 / a combat-slider edit rather
        // than on a kill. The stale holders also FREE against the new table
        // later, which is how one corpse hands a live creature's brick to a
        // third body — `build/microbody_audit.log` is full of exactly that,
        // every offending holder a debris body ("holds model 212 of 184").
        //
        // Reset, not remap: the shared records line up again only if the assets
        // are byte-identical, and every copy-on-write clone (a carve, a char, a
        // bloodied limb) is gone whatever we do. Through BodyRegistry because
        // that is the one type that enumerates ALL THREE holder populations —
        // a fourth system arriving must be dropped here too, and the gate that
        // pins this calls the same function.
        BodyRegistry(debris, mobs, &avatar, &mbSet).ReleaseMicroHolders();
        mobs.OnMaterialsReloaded(mats, reactions);
        std::vector<MobDef> mobDefs;
        std::string mlog;
        // rebuild the shared micro pool from scratch: model indices die here,
        // so the cached sphere models die with them (material ids can remap)
        mbSet = MicroBodySet{};
        sphereModels.clear();
        {
          std::shared_ptr<MobDefFactory> rFac;
          LoadMobDefs(assetDir + "/mobs", mats, mobDefs, mbSet, mlog, &rFac);
          // The factory reloads with the defs: it carries the material table
          // and the clip library, both of which this R may have changed.
          mobs.SetDefFactory(std::move(rFac));
        }
        if (!mlog.empty()) std::fprintf(stderr, "%s", mlog.c_str());
        // Items MUST reload here too: their bricks live in the pool that was
        // just thrown away, so a stale ItemDef would hold a model index into
        // a freed model.
        //
        // NO SLOT HOLDS AN ITEM INDEX (W2-M): every hotbar, pack and
        // equipment slot is an ItemInstance and names its item, so a reorder
        // of items.json changes nothing and the dye, a vessel's contents and
        // a piece's damage ride through untouched. All a reload can do to the
        // kit is REMOVE an item: Kit::DropUnknown empties those slots (and
        // re-clamps a vessel to an edited capacity). This block used to
        // snapshot every slot by name and re-resolve it, field by field, and
        // it was one forgotten field away from bleaching the wardrobe.
        {
          std::string ierr;
          LoadItems(assetDir + "/items", mats.size(), mbSet, items, ierr);
          if (!ierr.empty()) std::fprintf(stderr, "%s", ierr.c_str());
          if (const int gone = kit.DropUnknown(items))
            std::fprintf(stderr,
                         "items reload: %d slot(s) named an item that is gone; "
                         "emptied\n",
                         gone);
          // The AI panel's weapon picker is a mirror of the same library, and
          // by name for the same reason the slots above are: a new blade in
          // items.json appears in the combo on this R without disturbing what
          // is already selected.
          rebuildAiWeapons();
          rebuildGiveVessels();
          // ...and the wardrobe's three, for the same reason and by the same
          // rule: a pattern added to items.json appears on this R without
          // moving what is already picked.
          rebuildWardrobe();
          rebuildAiWear();
          // Conversations name items; re-read and re-validate them against
          // the library just loaded.
          reloadDialogue();
        }
        sim.UploadMicroBodies(ctx.queue, mbSet);
        mobs.SetDefs(std::move(mobDefs));
        // After SetDefs, not before: the creature list is the LIVE defs, so a
        // sidecar added or renamed on this R has to be in place first.
        rebuildAiCreatures();
        rebuildAiEffects();
        // Behaviour profiles reload with the rest of the content. SetBehaviors
        // re-resolves every LIVE mob's profile by name, so retuning a duelist
        // and hitting R is visible on the duelists already fighting you rather
        // than only on the next one spawned.
        {
          ai::Library beh;
          std::string blog;
          ai::LoadBehaviors(assetDir + "/mobs/behaviors.json", beh, blog);
          if (!blog.empty()) std::fprintf(stderr, "%s", blog.c_str());
          mobs.SetBehaviors(std::move(beh));
          // Attack styles reload with them, for the same reason: retuning a
          // windup and hitting R must be visible on the duelists already
          // fighting you.
          StyleLibrary sty;
          LoadAttackStyles(assetDir + "/mobs/attack_styles.json", sty, blog);
          if (!blog.empty()) std::fprintf(stderr, "%s", blog.c_str());
          mobs.SetAttackStyles(std::move(sty));
          ui.aiProfileNames.clear();
          for (const ai::Profile& p : mobs.Behaviors().profiles)
            ui.aiProfileNames.push_back(p.name);
        }
        // The avatar holds a MobDef* INTO that vector, so it must be
        // re-published after SetDefs replaces the contents or the pointer
        // dangles. SetDefs despawns first, which also drops limb bodies that
        // reference the now-freed micro models.
        avatar.OnMaterialsReloaded(mats);
        avatar.SetDefs(&mobs.Defs(), avatarDefName);
        // Re-resolve material -> footstep set and the acoustic table. Also
        // rescans assets/sounds, so dropping in a new step variant and hitting
        // R makes it audible without a rebuild.
        audioCues.RescanSounds(mats);
        tpRig.Snap();
        ui.mobNames.clear();
        for (const MobDef& d : mobs.Defs()) ui.mobNames.push_back(d.name);
        if (ui.mobSelected >= (int)mobs.Defs().size()) ui.mobSelected = 0;
        classOf = BuildCollisionClasses(mats);
        // The burn ladder is resolved by NAME, so a materials edit that adds,
        // removes or reorders flesh_charred/ash has to re-resolve here or the
        // inspector's charred readout counts the wrong material.
        burnMats = ResolveBurnMats(mats);
        tissueMats = ResolveTissueMats(mobs, mats);
        FillUiMaterials(mats, ui);
        std::printf("materials reloaded (%zu, %zu reactions)\n", mats.size(),
                    reactions.size());
      } else if (!ui.reloadMaterials) {   // (deferred for the bench: not a failure)
        std::fprintf(stderr, "asset reload failed:\n%s\n", errors.c_str());
      }
    }
    if (ui.regenWorld) {
      ui.regenWorld = false;
      // A regen ALWAYS re-reads the environment first (PLAN_environment_truth
      // P-A): the button exists to see what you authored, and what you
      // authored is on disk. A file that refuses keeps the old tables and
      // says so; the regen still happens, on those.
      {
        std::string elog;
        if (ReloadEnvironment(ctx, sim, mats, envStamp, elog)) {
          std::printf("%s (reloaded)\n", envStamp.Line().c_str());
        } else {
          std::fprintf(stderr, "%s", elog.c_str());
        }
        if (telemetryEnabled) {
          const std::string m = envStampMessage();
          telemetry.SendText(m.c_str(), (int)m.size());
        }
      }
      stream.OnRegen();
      // The spawn site and the ground under it are functions of the map just
      // reloaded: a moved spawn takes effect here, on F7.
      world.SetWindowOrigin(labScene >= 0 ? IVec3{0, 0, 0} : SpawnWindowOrigin());
      SubmitWorldgen(ctx, world, sim, kDefaultSeed);
      player.pos = SpawnPos();
      // Lab: a regen wipes the scene structure, so restart the scene clock
      // (build ops re-land on the next tick) and return to the scene pose.
      if (labScene >= 0) {
        labTick = 0;
        Vec3 labEye;
        float labYaw = 0, labPitch = 0;
        LabSceneCamera(labScene, labEye, labYaw, labPitch);
        player.pos = labEye;
        cam.yaw = labYaw;
        cam.pitch = labPitch;
      }
      player.ResetViewSmooth();   // teleport: never smooth across it
      player.SnapRender();        // ...and never interpolate across it either
      tick = 0;
      grenades.clear();
      everExploded = false;
      fluidCount = 0;  // MPM fluid does not survive a regen (the worldgen
      fluidPendingSpawns.clear();  // table zeroes the GPU count + calm state)
      debris.Reset();
      mobs.Reset();
      // A new world: the refs start as authored (no deltas, nothing active),
      // read again from the map F7 may just have switched to.
      if (refsMapName() != kHarnessMapName) {
        refStore.LoadMap(assetDir, refsMapName());
        tickCtx.refs = &refStore;
        ui.refs = &refStore;
      } else {
        tickCtx.refs = nullptr;
        ui.refs = nullptr;
      }
      // The avatar's severed parts live in DebrisSystem and its live limbs are
      // Jolt bodies in the world that just went away; despawn rather than
      // leave it holding handles into a system that has been reset. The
      // per-tick block respawns it on the next tick.
      avatar.Despawn();
      tpRig.Snap();
    }
    if (ui.saveWorld) {
      ui.saveWorld = false;
      ctx.WaitIdle();
      // Grid + entities: everything outside the voxel grid rides the
      // sections registered in game/persist.cpp -- world.sve, this player's
      // players/local.svp, and per-region r_*.sve buckets (S4).
      const PlayerKitRefs kitRefs = PlayerKitOf(session, glyphs, items);
      WorldItemRefs groundRefs{&ground, &phys, &debris, &mbSet, &items};
      EntityIO eio = MakeEntityIO(debris, mobs, &avatar, &kitRefs, &groundRefs, tickCtx.refs, &talkStore);
      // M9.5-B: the directory is `--load-world`'s, and the save now carries
      // the SIM TICK and the SEED (meta.svm's SVM5 pair). The tick is what a
      // reload has to resume above so that the per-chunk tick tags this
      // world's evictions wrote stay meaningful; the seed is what
      // regenerates every chunk the store does not hold.
      SaveWorld(ctx, world, stream, worldDir, mats, &eio,
                WorldStamp{tick, (uint32_t)kDefaultSeed, true});
    }
    if (ui.loadWorld) {
      ui.loadWorld = false;
      ctx.WaitIdle();
      const PlayerKitRefs kitRefs = PlayerKitOf(session, glyphs, items);
      WorldItemRefs groundRefs{&ground, &phys, &debris, &mbSet, &items};
      EntityIO eio = MakeEntityIO(debris, mobs, &avatar, &kitRefs, &groundRefs, tickCtx.refs, &talkStore);
      WorldStamp loaded{};
      if (LoadWorld(ctx, world, sim, stream, worldDir, mats, &eio, &loaded)) {
        mobParking.ResetWaits();
        // ---- RESUME THE CLOCK ABOVE THE SAVE (M9.5-B) -----------------
        //
        // The per-chunk tick tags in `manifest.svt` were written against the
        // tick this world was saved at. A process that loads it and keeps
        // counting from ITS own tick — which after a fresh boot is a few
        // hundred — would evict chunks tagged BELOW the tags already in the
        // store, and a peer's "newer wins" arbitration would then refuse the
        // live edits in favour of the saved ones. Resuming above the save is
        // what keeps the tag a monotonic statement about freshness.
        //
        // ONLY WHEN THE FILE SAID SO. An SVM4 save reports `known == false`
        // and the clock is left alone: "the file did not say" is not "the
        // file said 0", and jumping to 0 would be strictly worse than
        // staying where we are. Also only FORWARD — `tick` is the sim clock
        // and moving it backwards would make the CA's stamp nibble
        // (world.h's bits 16-18) cycle through values it has already used
        // this session.
        if (loaded.known && loaded.tick > tick) {
          std::printf("load: resuming the sim clock at tick %u (was %u)\n",
                      loaded.tick, tick);
          tick = loaded.tick;
        }
        // ...and the SKY follows the save even when the sim clock could not
        // (an older save reloaded mid-session): game/persist.h ResumeWorldClock.
        if (loaded.known && ResumeWorldClock(loaded.tick, tick))
          std::printf("load: sky clock engaged at the save's tick %u (sim tick "
                      "stays %u)\n",
                      loaded.tick, tick);
        // Debris/mobs were reset and reloaded by their sections; the avatar
        // was despawned by its reset and respawns on the next tick, applying
        // the saved damage state (avatar.h persistence note). Only main's own
        // transient state is cleared here.
        grenades.clear();
        everExploded = false;
        fluidCount = 0;  // MPM fluid is not in the save format: saves
        fluidPendingSpawns.clear();  // force-settle (loadReset zeroes the
                                     // GPU count + calm state)
        player.SnapRender();  // the load moved the body: do not lerp into it
        tpRig.Snap();
        // ---- A BLADE SAVED IN THE SHEATH COMES BACK TO THE HOTBAR -------
        //
        // Before 2026-09-23 the draw key pulled from the Sheath slot; now
        // the hand reads ONLY the hotbar ("THE HOTBAR IS THE HAND"), so a
        // save made under the old rule would leave its sword where no key
        // reaches it. Moved here, at the one game-side load, rather than in
        // LoadPlayerKit: the format round-trip stays exact (the playerkit
        // gate asserts the sheath comes back) and only the GAME re-homes it.
        // No free hotbar slot = it stays sheathed; the pack UI can still
        // drag it out.
        for (int e = 0; e < kEquipSlotCount; e++) {
          if (EquipSlotAt(e).id != EquipSlotId::Sheath) continue;
          ItemStack& sh = kit.equip.slots[e];
          if (sh.Empty()) continue;
          for (ItemStack& hs : hotbar.slots) {
            if (!hs.Empty()) continue;
            hs = sh;
            sh = ItemStack{};
            break;
          }
        }
      }
    }

    // ---- player: the FRAME half (PLAN_multiplayer_now N2) ----------------
    // The controller itself no longer runs here. What is left is the two
    // things that are genuinely per-frame: the fly-mode mirror of the dev
    // toggle, and the `kindAt` closure the tick body and the third-person
    // boom both call. Player::Update moved into the fixed-tick loop below.
    player.fly = ui.fly;
    // (`kindAt` is bound ONCE above the frame loop now, into session.kindAt:
    //  a per-player collision source is the point of Player::KindFn.)
    // ---- the trample ring: feet on the plants ------------------------------
    // Every grounded presser lays or refreshes a footprint stamp under itself
    // each frame (sim/trample.h); the plants read the ring at their base and
    // flatten under it, then spring back over render.trampleRecover seconds.
    // The avatar is the player and is NOT in mobs_, so it is not counted
    // twice. A mob's stamp is its collision footprint, not its art, so a bird
    // overhead presses nothing — trampleAt also rejects stamps whose ground
    // level is far from the plant's base.
    {
      const Tuning& trTun = CurrentTuning();
      TrampleRing& tr = Tramples();
      if (player.grounded) {
        tr.Press(player.pos.x, player.pos.z, player.pos.y - Player::kHalfY,
                 Player::kHalfXZ * trTun.render.trampleRadius, 1.0f, (float)now);
      }
      for (uint32_t i = 0; i < mobs.MobCount(); i++) {
        const Mob* m = mobs.MobAt(i);
        if (!m || !m->Alive() || !m->Def()) continue;
        // origin_.y is NOT the live height: a walking mob keeps its foot
        // height in bodyY_ (the ground probe writes it) and origin_.y is the
        // spawn value, so a stamp at Origin().y sat metres off the plants'
        // base and trampleAt's ground band rejected every one of them.
        const Vec3 o = m->Origin();
        const Vec3 s = m->Def()->worldSize;
        const float half = std::max(s.x, s.z) * 0.5f;
        if (half <= 0.0f) continue;
        tr.Press(o.x + s.x * 0.5f, o.z + s.z * 0.5f, m->BodyY(),
                 half * trTun.render.trampleRadius,
                 std::min(1.0f, 0.5f + half * 0.15f), (float)now);
      }
      tr.Expire((float)now, trTun.render.trampleRecover);
    }
    ui.fly = player.fly;

    // ---- HIT-STOP: drain the latch, then age the live dip -------------------
    //
    // BILLED TO `input`, decided at the combat/perf merge. This sits inside
    // `spanInput`, whose own comment declares its boundary as "everything from
    // here to the fixed-tick loop" — per-frame pre-tick work — and that is
    // exactly what this is. It is a latch drain, a subtraction and a compare;
    // it cannot move a percentile, and moving the span's edge around it would
    // make the boundary a list of exceptions instead of a line. The dip
    // CHANGES what the frame costs (fewer ticks run), but that shows up on
    // `sim`/`submit`, where the work actually did not happen — not here.
    //
    // ORDER MATTERS AND THIS IS THE ONLY PLACE IT IS RIGHT: the drain must
    // happen after the tick loop that WROTE the request (last frame) and before
    // the accumulator that READS the dip (three lines down). A request made on
    // frame N therefore lands on frame N+1 — one frame, ~16 ms, which is under
    // the shortest dip and well under the perceptual threshold for "did the
    // game react to my hit".
    //
    // BYPASSED WHOLESALE by the deterministic harnesses. `--autofly*` pins a
    // FIXED TICK SCHEDULE on purpose (CLAUDE.md: it is how residency is sized,
    // and both the page-pool numbers and the traversal measurements depend on
    // the tick count per frame being what the harness says it is). Nothing in
    // those runs swings a sword today, so this is a guard rather than a fix —
    // but a measurement harness silently dilated by a gameplay effect is
    // exactly the kind of thing that costs a session, and the check is free.
    // `--selftest` needs no guard at all: it never reaches this loop.
    {
      const Tuning::CombatFx& fx = CurrentTuning().combatfx;
      const bool allowed = fx.hitStop && !g_autoflySurface && !g_autoflyHard;
      if (hitStop.pendMs > 0.0f && allowed) {
        hitStop.timeLeft = std::max(hitStop.timeLeft, hitStop.pendMs * 0.001f);
        hitStop.scale = std::min(hitStop.scale, hitStop.pendScale);
      }
      hitStop.pendScale = 1.0f;
      hitStop.pendMs = 0.0f;
      if (hitStop.timeLeft > 0.0f) {
        // Aged in REAL time (`dt`), never in ticks — the dip is a length of
        // held breath the player perceives, and a dip measured in the ticks it
        // is itself suppressing would last however long it felt like.
        hitStop.timeLeft -= (float)dt;
        if (hitStop.timeLeft <= 0.0f) {
          hitStop.timeLeft = 0.0f;
          hitStop.scale = 1.0f;
        }
      }
      if (!allowed) {
        hitStop.timeLeft = 0.0f;
        hitStop.scale = 1.0f;
      }
    }
    ui.hitStopScale = hitStop.timeLeft > 0.0f ? hitStop.scale : 1.0f;

    // ---- fixed-tick simulation ----
    // THE DIP IS APPLIED HERE AND NOWHERE ELSE. Scaling the accumulator's FILL
    // RATE is the whole mechanism: fewer whole ticks clear the `>= kTickDt`
    // test below, so the world runs slower without any tick running
    // differently. Deliberately not `kTickDt * k` (which would change what a
    // tick MEANS to everything downstream that uses it as a dt) and not a
    // sleep (which would stall rendering too, so the hit would freeze rather
    // than slow).
    accumulator += dt * (double)(hitStop.timeLeft > 0.0f ? hitStop.scale : 1.0f);
    // Cap the tick backlog: with no cap, any stretch where 30 Hz can't be met
    // (heavy fire, worldgen, a save) accrues unbounded debt and the loop runs
    // 4 ticks/frame long after the load has passed. Drop the excess instead.
    // ONE DEFINITION of the cap: World::kReadbackSlots is derived from it (the
    // snapshot ring must cover every tick that can be in flight), so a literal
    // here would silently under-size the ring.
    constexpr int kMaxTicksPerFrame = World::kMaxTicksPerFrame;
    if (accumulator > kMaxTicksPerFrame * kTickDt)
      accumulator = kMaxTicksPerFrame * kTickDt;
    // SANDVOX_TICKS_PER_FRAME: exactly n ticks this frame, whatever the clock
    // says. See HarnessTicksPerFrame's comment for why the harness needs it.
    const int fixedTicksPerFrame = HarnessTicksPerFrame();
    if (fixedTicksPerFrame > 0)
      accumulator = (double)fixedTicksPerFrame * kTickDt;
    int ticksThisFrame = 0;
    bool mouseL = captured && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    bool mouseR = captured && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    // LMB routes to the active tool: continuous for brush/laser, click-edge
    // for one-shot tools (prefab stamp, mob spawn)
    bool mouseLClick = mouseL && !prevMouseL;
    bool mouseRClick = mouseR && !prevMouseR;
    prevMouseL = mouseL;
    prevMouseR = mouseR;
    // ---- EVERY ONE-SHOT, INTO THE COMMAND'S PRESSED MASK ------------------
    //
    // These five used to be five hand-rolled sticky booleans (`castQueued`,
    // `strikeQueued`, `ui.placePrefab`, `ui.spawnMob`, `dropStatusQueued`),
    // each latched here and consumed-and-cleared somewhere in the tick body,
    // because the tick loop runs ZERO times on most frames at 60+ fps against
    // a 30 Hz tick and a frame-local bool is discarded unread eight tries out
    // of nine — the bug that made RMB casting fire one try in nine. The feeder
    // does that for all of them at once, and guarantees the half nobody wrote
    // by hand: an edge reaches EXACTLY ONE tick, never zero and never two.
    //
    // THERE IS NO CAST KEY any more (spells in hand, 2026-09-27): a spell is
    // put into a hand from the spell bar and that hand's own button casts it
    // (session.cpp SPELLS IN HAND reads TB_ATTACK / TB_ALT). TB_CAST is unused.
    // PLAY (UIState::devControls off) has no prefab or mob tool: the panel's
    // radio buttons can still set one for the frame before the PLAY block
    // below forces the hands back, so the click is gated here too.
    if (ui.devControls && mouseLClick && ui.tool == UIState::kToolPrefab)
      feeder.Press(TB_PLACE);
    if (ui.devControls && mouseLClick && ui.tool == UIState::kToolMob)
      feeder.Press(TB_SPAWN);
    if (mouseLClick) feeder.Press(TB_ATTACK);
    // RMB IS THE LEFT HAND (dual wielding): its click edge starts a left-hand
    // strike or reports a vessel's refusal (session.cpp reads Pressed(TB_ALT)).
    // The feeder never derives an edge from a held bit, so without this the
    // left hand could hold but never begin anything.
    if (mouseRClick) feeder.Press(TB_ALT);
    if (captured && ui.magicMode && eDel.Pressed(key(GLFW_KEY_DELETE)))
      feeder.Press(TB_DROP);
    // The DEV PANEL asks for the same two things through UIState, so its
    // buttons funnel into the same edges rather than through a second door.
    // The flag is cleared as it is converted: it is a request, not state --
    // and in PLAY it is cleared WITHOUT converting, so a panel button pressed
    // in play mode neither fires now nor waits to fire on the switch to DEV.
    if (ui.placePrefab) {
      ui.placePrefab = false;
      if (ui.devControls) feeder.Press(TB_PLACE);
    }
    if (ui.spawnMob) {
      ui.spawnMob = false;
      if (ui.devControls) feeder.Press(TB_SPAWN);
    }
    // The held mouse bits, and the laser's OR of F-from-any-tool with
    // LMB-with-the-laser-tool. The laser is a DEV tool: in PLAY neither door
    // opens (the tool is forced to the hands below, and F is gated here). Re-sampled every frame, so a button released
    // between two ticks reads as released on the second one.
    feeder.Hold(TB_ATTACK, mouseL);
    feeder.Hold(TB_ALT, mouseR);
    // G held winds up a throw of a held vessel; letting go throws it
    // (game/container.h). Only a throwable vessel in hand reads it; the hold
    // is decided with the take/drag above (`gThrowHold`).
    feeder.Hold(TB_THROW, captured && gameKeys && gThrowHold);
    // ---- THE HANDS, AS THE FRAME SEES THEM (dual wielding) ----------------
    //
    // `handsUp`: the melee tool is up and magic is off, so LMB and RMB belong
    // to the hands. What each hand HOLDS is the kit's HandR/HandL — the tick
    // reads the same slots itself (session.h HandsNow); these copies are for
    // the frame's own questions: which button a vessel is on, where its pour
    // marker goes, what the HUD says.
    // The spell bar (magic mode) does not take the buttons: a spell is cast
    // from the hand it was put in, by that hand's button (spells in hand).
    const bool handsUp = ui.tool == UIState::kToolMelee;
    auto handDef = [&](Hand h) -> const ItemDef* {
      const ItemDef* d = items.Of(kit.equip.InHand(h));
      return d && EquipSlotAccepts(EquipSlotOfHand(h), d->kind) ? d : nullptr;
    };
    auto handVessel = [&](Hand h) {
      const ItemDef* d = handDef(h);
      return handsUp && d && d->IsContainer();
    };
    // THE VESSEL THE FRAME TALKS ABOUT: the one whose button is down, else
    // the last-used hand's, else the only one (the tick's own rule).
    const int vesselHand = [&] {
      const bool r = handVessel(Hand::Right), l = handVessel(Hand::Left);
      if (r && mouseL) return 0;
      if (l && mouseR) return 1;
      if (r && l) return HandIndex(session.lastHand);
      return r ? 0 : l ? 1 : -1;
    }();
    // F WITH A VESSEL IN HAND cycles THAT HAND'S MODE: pour -> scoop ->
    // apply (sim/tickinput.h TB_SCOOP/TB_APPLY and their _L twins). The
    // hand's own button then does it — LMB for a right-hand flask, RMB for a
    // left — so one button per vessel carries all three uses and the other
    // fist stays free to swing. Holding two vessels, F cycles the one in
    // the hand last used. While a vessel is in hand, F is this and not the
    // dev laser.
    static const char* const kVesselModeName[3] = {"pour", "scoop", "apply"};
    {
      static bool fPrev = false;
      const bool fDown = captured && gameKeys &&
                         glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;
      if (vesselHand >= 0 && fDown && !fPrev) {
        int& m = ui.vesselMode[vesselHand];
        m = (m + 1) % 3;
        const char* btn = vesselHand == 0 ? "LMB" : "RMB";
        static const char* const kWhat[3] = {"pours it out", "scoops loose matter in",
                                             "puts it on whoever you aim at"};
        ui.kitMessage = std::string(HandName(HandAt(vesselHand))) + " hand " +
                        kVesselModeName[m] + ": " + btn + " " + kWhat[m];
        ui.kitMessageAge = 0.0f;
      }
      fPrev = fDown;
    }
    for (int hk = 0; hk < kHands; hk++)
      ui.vesselModeShown[hk] = handVessel(HandAt(hk)) ? ui.vesselMode[hk] : -1;
    ui.lastHand = HandIndex(session.lastHand);
    ui.applyShown = vesselHand >= 0 && ui.vesselMode[vesselHand] == 2;
    feeder.Hold(TB_SCOOP, handVessel(Hand::Right) && ui.vesselMode[0] == 1);
    feeder.Hold(TB_APPLY, handVessel(Hand::Right) && ui.vesselMode[0] == 2);
    feeder.Hold(TB_SCOOP_L, handVessel(Hand::Left) && ui.vesselMode[1] == 1);
    feeder.Hold(TB_APPLY_L, handVessel(Hand::Left) && ui.vesselMode[1] == 2);
    feeder.Hold(TB_LASER,
                captured && ui.devControls &&
                    ((vesselHand < 0 && glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS) ||
                     (ui.tool == UIState::kToolLaser && mouseL)));
    // The RENDER layer draws the beam sprites and has no TickInput (it runs on
    // frames the tick loop did not). It reads the same bit off the pending
    // command rather than re-deriving the expression, so what is DRAWN and
    // what CUTS can never come from two different reads of the mouse.
    const bool laserHeldFrame = feeder.pend.Held(TB_LASER);
    // A click made while paused is DROPPED rather than held: the tick loop
    // breaks before the cast and strike sites while paused, so a latched click
    // would sit there and discharge the instant you unpause, at whatever you
    // happen to be aiming at then.
    if (ui.paused && !ui.stepOnce) {
      feeder.Cancel(TB_CAST | TB_ATTACK | TB_ALT);
      // ...and the strike style a tick already picked, for the same reason.
      // Cancel() only reaches edges that have not been consumed yet; a press
      // taken on the last tick before the pause has already become a style
      // index sitting in `strikeQueued`, and that one has to be dropped here
      // or it fires the instant the world resumes.
      strikeQueued = -1;
    }
    // ---- PLAY / DEV (UIState::devControls) ----------------------------------
    // Applied here, after every key and the dev panel have had their say and
    // before anything reads the tool, so the checkbox and F2 are one path.
    // ON A CHANGE TO PLAY: land (fly off) and put the hands up. EVERY FRAME IN
    // PLAY: the hands are the only tool -- the dev panel's radio buttons and a
    // Q that stows back to `toolBefore` cannot leave the brush in them.
    {
      static bool devPrev = ui.devControls;
      if (ui.devControls != devPrev) {
        devPrev = ui.devControls;
        if (!ui.devControls) {
          ui.fly = false;
          player.fly = false;
        }
        ui.kitMessage = ui.devControls ? "dev controls ON (F2)"
                                       : "dev controls OFF -- play mode (F2)";
        ui.kitMessageAge = 0.0f;
      }
      // ...and EVERY FRAME, not only on the change: the panel's fly checkbox
      // is still a live widget in PLAY. `player.fly = ui.fly` ran above this
      // frame, but the ticks run below, so clearing both here means no tick
      // ever flies in PLAY.
      if (!ui.devControls) {
        ui.tool = UIState::kToolMelee;
        ui.fly = false;
        player.fly = false;
      }
    }
    bool brushActive = ui.tool == UIState::kToolBrush && !ui.magicMode;
    // MELEE: hold LMB with the melee tool to arm the weapon, then flick.
    // Magic mode wins the mouse, so guarding and casting can never both be
    // live on the same button.
    // WHAT IS IN THE HAND is the selected hotbar slot (see "THE HOTBAR IS THE
    // HAND" above). A weapon dragged out of it, or a slot change, is noticed
    // here for free: there is no drawn flag to keep in step.
    // ---- GEAR THAT LEFT THE BODY BY FORCE (Mob::LostGear) --------------------
    //
    // The rig reports; the KIT is this frame's to fix, and it has to be fixed
    // BEFORE the hand and the wear loop below read it in the same tick, or
    // the re-equip seams would faithfully put a second sword in your hand
    // from the hotbar while the first lies at your feet, and put the cuirass back on
    // a body the plate has just fallen off. The piece on the ground is
    // registered as the item it is (SetOnItemShed hands the registry the whole
    // ItemInstance), so picking it back up is the ordinary `E` and wearing it
    // again restores exactly the holes it had. A worn piece has already left
    // the kit (Mob::ShedGearBeforeDetach empties its equipment slot); the HAND
    // is the one thing the body cannot fix, because it does not know which
    // hotbar slot filled it.
    for (const Mob::LostGear& lg : avatar.LostGearEvents()) {
      if (lg.held) {
        // THE KIT ALREADY KNOWS (dual wielding): the hand is a kit slot, and
        // Mob::ShedGearBeforeDetach emptied it at the instant the item left
        // the fist — the rig owns the kit it is dressed from. All that is
        // left for the frame is to say which hand.
        ui.kitMessage = "your " + lg.item + " was knocked from your " +
                        HandName(lg.hand) + " hand";
      } else {
        ui.kitMessage = "your " + lg.item + " was cut loose";
      }
      ui.kitMessageAge = 0.0f;
    }
    avatar.ClearLostGear();
    // THE WEAPON THE HUD TALKS ABOUT: the last-used hand's, else the other's.
    // The tick decides what actually swings (session.h HandsNow); this is the
    // frame's readout of the same kit slots.
    const ItemDef* heldItem = [&]() -> const ItemDef* {
      if (!handsUp) return nullptr;
      const Hand first = session.lastHand;
      for (Hand h : {first, OtherHand(first)}) {
        const ItemDef* d = handDef(h);
        if (d && d->kind == ItemKind::Melee) return d;
      }
      return nullptr;
    }();
    const bool meleeArmed = handsUp && heldItem != nullptr;
    // ---- ...AND WITH NOTHING IN YOUR HANDS (plan §6) -----------------------
    //
    // "The melee tool is up, nothing is drawn, and this body can punch." The
    // last clause is deliberately NOT `def->FindNatural("fist.R")`: a weapon
    // name spelled in C++ is exactly the closed-ended system design rule 4
    // forbids, and it would make the avatar the one creature whose anatomy the
    // engine knows by heart. Instead the CONTENT answers it — the
    // `playerUnarmed` compass resolves, and at least one style it points at is
    // one this body can actually swing (`StyleUsable`, the same question the
    // NPC draw asks). Lose both hands and the compass stops resolving on its
    // own; author a creature with claws instead of fists and nothing here
    // changes.
    // ---- ...OR A VESSEL (game/container.h) ---------------------------------
    //
    // The melee tool is the hands. With nothing drawn and a flask or pouch
    // selected in the hotbar, the hands hold THAT: RMB scoops, LMB pours, and
    // the unarmed compass below stays off -- a fist round a flask does not
    // punch.
    // (THE VESSEL is `vesselHand` above: a kit hand slot, not a hotbar one.)
    // WHERE A FILLED VESSEL POURS (game/container.h ContainerPourPoint): on
    // the crosshair ray from the RENDER eye. The tick pours at this one, cast
    // from LAST frame's eye (this frame's is set after the ticks); the marker
    // re-casts the same function from this frame's eye so the sphere sits on
    // the crosshair exactly. The two differ by one frame of motion.
    bool pourAimValid = false;
    Vec3 pourAim{};
    if (vesselHand >= 0) {
      const ItemStack& vs = kit.equip.InHand(HandAt(vesselHand));
      const ItemDef* vdef = items.Of(vs);
      if (vdef && vdef->IsContainer() && vs.Filled()) {
        static std::vector<uint64_t> own;
        own.clear();
        avatar.AppendLiveLimbBodies(own);
        const Vec3 head = player.EyePos();
        const Vec3 from = pourFromValid ? pourFrom : head;
        pourAim = ContainerPourPoint(
            *vdef, from, head, cam.Forward(),
            [&](IVec3 c) { return world.KindAt(c, classOf); }, phys, own);
        pourAimValid = true;
      }
    }
    const bool meleeUnarmed = [&] {
      if (!handsUp) return false;
      // A FIST needs an EMPTY hand: both holding something, no punch.
      if (handDef(Hand::Right) && handDef(Hand::Left)) return false;
      if (heldItem != nullptr || !avatar.Spawned()) return false;
      const StyleLibrary& lib = mobs.AttackStyles();
      if (!lib.playerUnarmed.Usable()) return false;
      for (const PlayerStrikeMap::Sector& s : lib.playerUnarmed.sectors)
        if (const AttackStyle* sty = lib.At(s.style))
          if (StyleUsable(avatar, *sty)) return true;
      for (int k = 0; k < 2; k++)
        if (const AttackStyle* sty = lib.At(lib.playerUnarmed.neutral[k]))
          if (StyleUsable(avatar, *sty)) return true;
      return false;
    }();
    // WHAT EVERY DOWNSTREAM "IS MELEE LIVE" TEST MEANS NOW. `meleeArmed` still
    // means "a weapon is drawn" and is what decides whether to equip one; this
    // is the one the driver, the program and the sweep gate on, because a fist
    // is as live as a sword.
    const bool meleeReady = meleeArmed || meleeUnarmed;
    // THE DISCRETE STRIKE'S PICK MOVED INTO THE TICK. It used to be read here,
    // at the click edge, off a frame-smoothed picker; the picker now
    // integrates one tick's pixels at kTickDt and the pick happens on the tick
    // that consumes TB_ATTACK. The same instant of intent, on one clock.
    //
    // Scroll and the number row pick a hotbar slot while the melee tool is up,
    // which is the one context where the number row is otherwise unclaimed
    // (the brush owns it normally, glyphs own it in magic mode). A UI
    // transaction, so it stays on the frame; the command merely CARRIES the
    // resulting selection, so a record or a remote peer knows what was in hand
    // when a tick's ops were authored.
    if (ui.tool == UIState::kToolMelee && !ui.magicMode) {
      for (int i = 0; i < kItemSlots; i++) {
        int k = (i == 9) ? GLFW_KEY_0 : (GLFW_KEY_1 + i);
        if (captured && eGlyph[i].Pressed(key(k))) hotbar.Select(i);
      }
    }
    // ---- Q / E: THE SELECTED HOTBAR STACK INTO THE LEFT / RIGHT HAND --------
    //
    // A kit move (Mob::KitMove), so it is a SWAP and nothing can be lost: the
    // hand's old item goes into the hotbar slot the new one came out of. On
    // an EMPTY hotbar slot the key puts the hand's item away into it. What a
    // hand refuses (armour: it is worn, not held) says why, as a drag does.
    // Frame-side like the number row and every other kit transaction: the
    // tick reads the result from the kit (session.h HandsNow).
    {
      const bool qDown = captured && gameKeys && key(GLFW_KEY_Q);
      const bool eDown = captured && gameKeys && key(GLFW_KEY_E);
      const bool qPress = eQ.Pressed(qDown), ePress = eE.Pressed(eDown);
      for (int pass = 0; pass < 2; pass++) {
        const bool press = pass == 0 ? ePress : qPress;
        if (!press) continue;
        const Hand h = pass == 0 ? Hand::Right : Hand::Left;
        const int hk = HandIndex(h);
        // ---- THE SPELL BAR IS OPEN: the selected spell into the hand -------
        //
        // A spell needs an EMPTY hand: whatever the hand holds is put away
        // into the first free hotbar slot, else the pack (a kit move, so
        // nothing is lost), and the key refuses if there is nowhere to put
        // it. On an empty key, or pressed again for the spell already there,
        // the key lets the hand's spell go.
        if (ui.magicMode) {
          const int slot = caster.selected;
          const bool slotHas =
              slot >= 0 && caster.inventory.KindAt(slot) != SlotKind::None;
          std::string msg;
          if (!slotHas || caster.hand[hk].slot == slot) {
            if (caster.hand[hk].Equipped()) {
              msg = "you let the " + caster.hand[hk].name + " go from your " +
                    HandName(h) + " hand";
              caster.ClearHand(hk);
            } else {
              msg = "no spell on that key";
            }
          } else {
            bool free = kit.equip.InHand(h).Empty();
            if (!free) {
              const std::string what = kit.equip.InHand(h).name;
              const KitRef hand{KitSpace::Equip, EquipSlotOfHand(h)};
              KitRef to{};
              for (int i = 0; i < kItemSlots && to.space == KitSpace::None; i++)
                if (hotbar.slots[i].Empty()) to = KitRef{KitSpace::Hotbar, i};
              for (int i = 0; i < Bag::kSlots && to.space == KitSpace::None; i++)
                if (kit.bag.slots[i].Empty()) to = KitRef{KitSpace::Bag, i};
              if (to.space != KitSpace::None &&
                  avatar.KitMove(hand, to, items) == MoveResult::Ok) {
                free = true;
                msg = "you put away the " + what + "; ";
              } else {
                msg = std::string("no room to put away the ") + what;
              }
            }
            if (free) {
              if (caster.EquipHand(glyphs, hk, slot)) {
                msg += caster.hand[hk].name + " in your " + HandName(h) + " hand (" +
                       (h == Hand::Right ? "LMB" : "RMB") + " casts)";
                session.lastHand = h;
              } else {
                msg += "that spell says nothing castable";
              }
            }
          }
          ui.kitMessage = msg;
          ui.kitMessageAge = 0.0f;
          continue;
        }
        const KitRef hot{KitSpace::Hotbar, hotbar.selected};
        const KitRef hand{KitSpace::Equip, EquipSlotOfHand(h)};
        const bool hotEmpty = hotbar.Selected().Empty();
        const bool handEmpty = kit.equip.InHand(h).Empty();
        std::string msg;
        if (hotEmpty && handEmpty && caster.hand[hk].Equipped()) {
          // An empty hotbar slot on a spell hand: lower the spell, as it
          // would put an item away.
          msg = "you let the " + caster.hand[hk].name + " go from your " +
                HandName(h) + " hand";
          caster.ClearHand(hk);
        } else if (hotEmpty && handEmpty) {
          msg = std::string("nothing to put in your ") + HandName(h) + " hand";
        } else {
          const std::string what =
              hotEmpty ? kit.equip.InHand(h).name : hotbar.Selected().name;
          const MoveResult r = hotEmpty ? avatar.KitMove(hand, hot, items)
                                        : avatar.KitMove(hot, hand, items);
          if (r == MoveResult::Ok)
            msg = hotEmpty ? "you put away the " + what
                           : what + " in your " + HandName(h) + " hand";
          else
            msg = MoveResultText(r, hand);
          if (r == MoveResult::Ok && !hotEmpty) session.lastHand = h;
        }
        ui.kitMessage = msg;
        ui.kitMessageAge = 0.0f;
      }
      // AN ITEM IN A HAND DISPLACES ITS SPELL, however it got there (Q / E
      // above, a drag on the character screen, a pickup): the hand holds one
      // thing, and the thing you just put in it is the one you meant.
      for (int hk = 0; hk < kHands; hk++)
        if (caster.hand[hk].slot >= 0 && !kit.equip.InHand(HandAt(hk)).Empty())
          caster.ClearHand(hk);
    }
    feeder.SetSelection(ui.tool, hotbar.selected);
    spanInput.Close();
    // R4 (docs/RESEARCH_streaming_hitch.md): one residency shift per FRAME.
    // Stream::Update is a per-TICK call and the clamp below runs up to four of
    // them, so a slow frame used to shift two or three times and get slower.
    stream.BeginFrame();
    // ...and one cascade-refill slice per frame, for the same reason
    // (farfield.h BeginFrame).
    far.BeginFrame();
    while (accumulator >= kTickDt && ticksThisFrame < kMaxTicksPerFrame) {
      // ---- THE GPU-LAG THROTTLE: a second tick only if the GPU can take it.
      //
      // The 4-tick catch-up above is Gaffer's clamp and it is right for a CPU
      // hitch: the CPU owes the world some ticks and pays them. It is WRONG
      // when the long frame was the GPU's: every tick submits a plane of
      // worldgen, a CA pass and a snapshot copy, so paying three of them into
      // a queue that is already a frame behind makes the next frame longer,
      // which owes more ticks, which is the loop that ends in the two blocking
      // waits on this path — SubmitTick's snapshot-staleness fence (Snapshot
      // Stall) and Stream's T+kWakeLatency wake fence (billed to World
      // Storage). Measured `--frames 900 --autofly-surface` before this:
      // stalls on 14.8% of frames, 215 fences for 7.95 s, whole-frame p99
      // 539 ms.
      //
      // The lag is READ, not guessed: each submitted tick kicks one snapshot
      // readback (KickReadback) and its map ticket stays pending until that
      // submit's fence signals, so PendingMapCount after a pump is exactly
      // how many ticks the GPU has not finished. The first tick of a frame
      // always runs; each further one runs only while the GPU owes fewer than
      // two. When it owes more, the surplus debt is DROPPED (sim time dilates
      // by those ticks) rather than banked, or the frame the GPU catches up
      // on would fire a four-tick burst and put it straight back behind.
      //
      // Pure pacing: which ticks run and what they compute is unchanged, so
      // no hashed state moves and the headless harnesses never see this
      // branch (they have no frame loop). Read the `ticksThisFrame` counter
      // on the Performance tab, or the harness's `gpu-lag throttle` line.
      // SANDVOX_NO_GPU_THROTTLE=1 is the A/B arm in one binary: the pre-throttle
      // loop, for measuring what the throttle buys on a given scene.
      static const bool noThrottle = std::getenv("SANDVOX_NO_GPU_THROTTLE") != nullptr;
      // SANDVOX_TICKS_PER_FRAME bypasses it too: the whole point of that
      // switch is a tick schedule that does not depend on how far behind the
      // GPU happens to be on this machine, this second.
      if (ticksThisFrame > 0 && !noThrottle && fixedTicksPerFrame == 0) {
        ctx.ProcessEvents();  // retire what the GPU finished during the tick above
        constexpr int kGpuLagThrottleTicks = 2;
        if (ctx.PendingMapCount() >= kGpuLagThrottleTicks) {
          g_ticksThrottled++;
          accumulator = std::min(accumulator, (double)kTickDt);
          break;
        }
      }
      // ---- M9.2-C: THE LOCKSTEP GATE --------------------------------
      //
      // The third pacing case, beside `fixedTicksPerFrame` and the GPU-lag
      // throttle above. `pacer.localTick` is the tick about to run and equals
      // `tick + 1` (the increment is three lines down), so this asks exactly
      // "has the peer's batch for the tick I am about to simulate arrived?".
      //
      // BREAK, NOT BLOCK, and the accumulator is NOT touched: the world owes
      // this tick and will run it as soon as the batch lands, while the frame
      // keeps rendering, keeps polling and keeps the window responsive. The
      // 4-tick cap at the top of the frame already bounds how much debt can
      // accrue, so a slow peer makes this machine LAG rather than fast-forward
      // through a burst when it catches up.
      //
      // It cannot deadlock: our own batch for T+D went out at T, before this
      // wait at T+1 (protocol.h invariant 1), and a batch goes out every tick
      // whether or not it carries anything.
      if (netPaced && !pacer.CanRun(tick + 1)) {
        netStalls++;
        break;
      }
      accumulator -= kTickDt;
      if (ui.paused && !ui.stepOnce) break;
      ui.stepOnce = false;
      tick++;
      ticksThisFrame++;

      // PUMP THE READBACK RING BETWEEN TICKS, NOT ONCE PER FRAME.
      //
      // Paged residency makes snapshot FRESHNESS load-bearing, and freshness is
      // per TICK, not per frame. §3.2's intersection is the only thing that
      // shrinks cpuDirty, and TightenFromSnapshot rolls a stale snapshot
      // forward by dilating it one N26 ring per tick of lag — the same ring
      // step (1) applies to cpuDirty itself. So at a 2-tick lag the two
      // operands have grown by the same factor and the intersection stops
      // removing anything: measured `tighten from snap 51 (2 rolls):
      // 6563 -> 6563`, a no-op, after which the mirror compounds ~2.3x/tick
      // and materializes straight through the pool (FATAL at ~23.4k of 24,576
      // pages while sprint-flying).
      //
      // The pump was at the BOTTOM of the frame loop, so a frame running the
      // 4-tick backlog cap got ONE snapshot for four ticks and three of them
      // tightened against roll-stale data. Over a 300-frame flight only 34 of
      // 206 tightenings ran at 0 rolls. This is NOT the readback ring running
      // out of slots — EncodeReadbacks declined just twice in that run.
      //
      // ProcessEvents is non-blocking (PollFences + fire-ready callbacks), so
      // a tick whose snapshot has not landed pays a fence status check and
      // moves on. It is called before the tick that will CONSUME the snapshot,
      // so a fence that signalled during the previous tick's GPU work is
      // observed on the very next tick instead of a frame later.
      {
        // The map-callback pump. Billed to `readback` because that is what it
        // pumps — the snapshot fences and the 3x3x3 CPU mirror rebuild — and
        // because it runs PER TICK here, so a 4-tick catch-up frame pays for
        // four of them. It is non-blocking by construction; if this row is ever
        // large, the mirror copy is the reason, not a stall.
        sandvox::PerfSpan spanRb(sandvox::PerfScope::Readback);
        ctx.ProcessEvents();
      }
      if (g_autoflyPark) ParkProbe(world, mats, tick);

      // ================= ONE TICK OF AUTHORITY (game/session.h) ===========
      //
      // Everything that used to sit between these braces — the player
      // controller, the brush, the spells, the melee sweep, mobs, debris,
      // physics and the submit — is TickAuthority() now. What the FRAME layer
      // kept is what is genuinely the frame's: the accumulator and the
      // throttle above, the readback pump, the park probe, and the HANDOVER
      // of one TickInput. That handover IS the authority boundary.
      //
      // `ti` is consumed here rather than inside, because consuming the
      // command is the frame layer's last act as the player's proxy: a server
      // would take the same value off the wire and call the same function.
      const TickInput ti =
          feeder.Consume(cam.FlatForward(), cam.Right(), cam.Forward());
      tickCtx.frameTime = now;

      // ---- M9.2-C: WHAT THE PEER SAID ABOUT *THIS* TICK ------------------
      //
      // Applied HERE, immediately before the tick that carries the label, and
      // never on arrival — a batch labelled T lands up to D ticks early and
      // applying it then would run the ghost four ticks ahead of every local
      // body it can stand on or be hit by.
      if (netPaced) {
        const auto it = netQueue.find(tick);
        if (it != netQueue.end()) {
          if (it->second.haveState) {
            // Upsert, not Find: the ghost is CREATED by its first state, so
            // the spawn happens on the tick that first has somewhere to put
            // it. `playerId` is re-stamped with the handshake's id because
            // MakePlayerState fills it from the sender's session index, which
            // is 0 on both machines.
            PlayerState st = it->second.st;
            st.playerId = netPeerId;
            remotes.Upsert(netPeerId).Apply(st);
            // M9.4-D: the authority's view of the peer, captured at the
            // moment the ghost body is placed from the same state. See the
            // note on netPeerLast for why it is not captured on arrival.
            netPeerLast = st;
            netHavePeerLast = true;
          }
          // THE HOST WINS ON THE TWO HASHED PRESENTATION INPUTS (session.cpp
          // phase L11: `timeScale` reaches Celestial and `vizActive` reaches
          // SubmitTick). They are sim inputs, so two machines disagreeing
          // about them is two different worlds; one of them has to be the
          // authority and the host's clock is already the tick clock.
          if (netRoleBoot == NetRole::Client) {
            ui.timeScale = it->second.timeScale;
            ui.showDirtyVoxels = it->second.vizActive != 0;
          }
        }
        // Prune everything at or before the tick about to run. The map never
        // holds more than D+1 entries, so this is the whole memory management.
        netQueue.erase(netQueue.begin(), netQueue.upper_bound(tick));

        // ---- M9.4-D: THE ENTITY TICK, AND ITS ORDER IS THE DESIGN -------
        //
        // Four steps, all of them BEFORE TickAuthority and all of them in
        // this order for a reason that a comment is the only place to record:
        //
        //  1. REBUILD THE PEER VIEWS. Both machines' authority arithmetic
        //     reads the same two facts (each player's feet chunk and window
        //     origin) and gets the same answer; that is what makes ownership
        //     derived rather than negotiated. Rebuilt every tick because a
        //     walking player moves the boundary.
        //  2. APPLY WHAT ARRIVED FOR THIS LABEL. Handoffs, announces, poses,
        //     gones, item traffic — in the envelope's field order. Before the
        //     handoff scan, so a creature the peer has just given me is not
        //     immediately considered for giving back.
        //  3. SCAN FOR HANDOFFS. Before the tick, because
        //     `MobSystem::PreTick` starts with `RefreshOwnership`, which
        //     turns a creature whose authority has flipped into a ghost — and
        //     `TakeHandoff` refuses a ghost. This is the ONLY window in which
        //     the losing side can still describe what it is losing.
        //  4. EXPIRE. The 3 s backstop for a ghost whose owner stopped
        //     talking about it because I left ITS window (see
        //     kGhostExpiryTicks) — the one case no message can cover.
        //
        // The batch that carries the results goes out after the tick, from
        // the same `netSendBatch` the `TickBatch` rides.
        netRefreshPeerViews();
        entities.ApplyForTick(tick, mobs, debris, &ground);
        entities.ScanHandoffs(mobs, debris, tick);
        entities.ExpireGhosts(tick, mobs, debris);
        netGhostsMax = std::max(netGhostsMax, entities.Stats().ghostsNow);
      }

      // ---- M9.4-D: SOMETHING FOR THE SMOKE TO OWN --------------------
      //
      // SANDVOX_NET_SMOKE_MOBS only (see the switch). Three creatures of the
      // `crowd` gate's humanoid def, eight voxels ahead of the player, once,
      // at tick 30. `MobSystem::Spawn` is the same call the AI panel makes —
      // nothing here is a test-only object, and nothing here is a second
      // spawn path.
      if (NetSmokeMobs() && g_harnessFrames > 0 && tick == 30) {
        int humanDef = -1;
        for (size_t i = 0; i < mobs.Defs().size(); i++) {
          if (mobs.Defs()[i].FindSocket("held_right") < 0) continue;
          if (humanDef < 0 || mobs.Defs()[i].name == "human")
            humanDef = (int)i;
        }
        if (humanDef >= 0) {
          const Vec3 fwd = cam.FlatForward();
          int spawned = 0;
          for (int k = 0; k < 3; k++) {
            // Spread ACROSS the look direction so all three are in front and
            // none is inside another: a crowd spawned on one cell would spend
            // its first ticks resolving overlap instead of standing there.
            const int sx = ifloor(session.player.pos.x + fwd.x * 8.0f) +
                           (k - 1) * 3;
            const int sz = ifloor(session.player.pos.z + fwd.z * 8.0f);
            const int sy = World::TerrainHeight(sx, sz, kDefaultSeed) + 1;
            if (mobs.Spawn(humanDef, {sx, sy, sz}) != 0) spawned++;
          }
          std::printf("net smoke: spawned %d mob(s) of '%s' at tick %u\n",
                      spawned, mobs.Defs()[humanDef].name.c_str(), tick);
          std::fflush(stdout);
        }
      }

      TickAuthority(tickCtx, session,
                    FrameIntent{brushActive, handsUp, pourAimValid, pourAim,
                                ui.pourRadius, pourFromValid, pourFrom},
                    ti, tick, opBatch);

      // ---- S5b: NPCs OUTLIVE THE WINDOW (game/persist.h MobParking) --------
      // A creature leaving the window is parked in its region bucket by
      // mobs.PreTick; here, after the tick, the ones the window has reached
      // again come back, at most MobParking::kUnparkPerCall per tick and only
      // onto ground the fetch cache has answered for. Parking is on for single
      // player and the host (whose store is the world's), off for a client.
      {
        mobParking.Bind(mobs, stream.Store(), netRoleBoot != NetRole::Client);
        mobParking.Unpark(mobs, stream.Store(), world, tick);
      }

      // ---- M9.3-B: THE SMOKE'S AUTHOR ------------------------------------
      //
      // SANDVOX_NET_SMOKE_PAINT only (see the switch's note). One small brush
      // op every 30 ticks at the player's feet, appended to the LOCAL batch
      // phase N has just stored under `tick + kOpLabelAhead` -- i.e. before
      // the send loop below reads that label, and into the same vector a
      // human's stroke would have gone into. It is not a second mutation path: the op is merged,
      // submitted and recorded exactly like any other.
      if (netPaced && NetSmokePaint() && tick % 30 == 0) {
        BrushOp b{};
        b.x = ifloor(session.player.pos.x);
        b.y = ifloor(session.player.pos.y) - 2;   // at the feet, not in them
        b.z = ifloor(session.player.pos.z);
        b.radius = 2;
        b.material = kMatStone;
        b.mode = 1u;   // overwrite: a paint-into-air op in solid ground is a
                       // no-op and would prove nothing
        opsync.Q().AppendLocalBrush(tick + opsync.Q().D(), b,
                                    sandvox::opstream::Producer::Lab);
      }

      // ---- M9.3-C: OUR OWN ORIGIN RING ----------------------------------
      //
      // AFTER the tick, so the origin recorded for tick T is the one phase B
      // left the window at — the same moment the peer's `MakePlayerState`
      // samples for its own ring. Two rings sampled at different points of
      // the tick would disagree by one shift on a walking player and the
      // comparable set would stop being symmetric.
      // M9.4-D: and MY OWN authority view for this tick, sampled at exactly
      // the point `MakePlayerState` samples the state the peer will pair it
      // with (after the tick, `world.WindowOrigin()` as phase B left it). A
      // ring sampled at a different point of the tick would be off by one
      // window shift on a walking player, which is the whole bug it exists
      // to fix — see netMyRing.
      if (netPaced) netNoteMyView(tick);
      if (netPaced) {
        chunksync.NoteOwnTick(tick, world.WindowOrigin(),
                              {ifloor(session.player.pos.x) >> 4,
                               ifloor(session.player.pos.y) >> 4,
                               ifloor(session.player.pos.z) >> 4});
      }

      // ---- M9.2-C: AND THE BATCH FOR T+D GOES OUT ------------------------
      //
      // After the tick, so the `PlayerState` inside is this tick's outcome
      // rather than the previous one's, and before the next tick's wait, which
      // is invariant 1. In the steady state `ShouldSend()` is true exactly
      // once here; at connect the same loop already emitted T0..T0+D.
      if (netPaced) {
        pacer.NoteRan();
        while (pacer.ShouldSend()) {
          const uint32_t label = pacer.NextToSend();
          netSendBatch(label);
          pacer.NoteSent(label);
        }
      }
    }
    if (ui.paused) accumulator = std::min(accumulator, (double)kTickDt);

    // ---- render ----
    //
    // ACQUIREFRAME IS A WAIT, NOT WORK, AND IT IS BILLED AS ONE.
    //
    // ctx.AcquireFrame() blocks twice before it returns anything: on the fence
    // of the submit that last used this acquire slot (rhi_vulkan.cpp,
    // AcquireSwapchainImage), and then inside vkAcquireNextImageKHR with an
    // infinite timeout — and the swapchain is VK_PRESENT_MODE_FIFO_KHR, so that
    // second block IS vsync. Neither is the CPU doing anything.
    //
    // Both used to land inside PerfScope::RenderCpu, because tRender0 was taken
    // on the line above the acquire. The Performance tab then reported ~18.9 ms
    // of "render-pass encode: draw calls, instance buffers, overlay" on a frame
    // whose actual encode is ~0.07 ms, and read as CPU-bound while the machine
    // was 100% GPU-bound. It is the same trap as the free-probe stall and the
    // "39.5 ms readback" that was two thirds genChunk: a fence tells you HOW
    // MUCH you waited, never WHAT you waited for, so it must be charged to the
    // thing that made you wait.
    //
    // So the wait goes to `present`, which is where perfnodes.h already says it
    // belongs ("AcquireFrame + Present. Under vsync this row IS the wait") and
    // where the offscreen --perf harness has always put its own frame-in-flight
    // throttle. Live telemetry and the harness now agree, which is the property
    // that makes a number measured in one comparable to the other.
    double tAcquire0 = NowSeconds();
    rhi::TextureView target = ctx.AcquireFrame();
    double tRender0 = NowSeconds();
    if (target) {
      // RenderEyePos, not EyePos, and the difference is now TWO corrections
      // rather than one:
      //   * the step-smoothing offset, so voxel steps glide instead of popping
      //     (Player::ViewEyePos's original job), and
      //   * the TICK INTERPOLATION (package N2). The body moves only on the
      //     30 Hz tick now, so at 144 fps four frames in five would draw the
      //     eye at the identical position and the fifth would jump. `alpha` is
      //     the leftover accumulator as a fraction of a tick — exactly what
      //     Celestial::RenderTickInterp already uses for the sky — and the
      //     camera rides prevPos -> pos by it.
      // Everything that can feed the sim (brush/laser/grenade rays, physics,
      // the pick, the NPCs' idea of where you are) stays on pos/EyePos().
      const float tickAlpha =
          std::min(1.0f, std::max(0.0f, (float)(accumulator / kTickDt)));
      Vec3 eye = player.RenderEyePos(tickAlpha);
      if (camMode == CameraMode::First) {
        // THE EYE RIDES THE HEAD. The player's eye point is a fixed height
        // over the collision box, which is right for a standing body and
        // wrong for everything the pose does to the head -- above all a
        // crawl, where the head is down on the floor out in front of the
        // box. The head's posed eyes are measured as an OFFSET from the
        // body's tick position, that offset is eased
        // (avatar.firstPersonHeadHalflife), and the eye is placed where the
        // DRAWN head is: the same RenderBodyOffset the art is drawn with
        // (tick interpolation + step banking), so eye and body stay one rigid
        // thing and only the head's own movement (posed at 30 Hz) is eased.
        // Render-only: every pick, ray and the sim still read EyePos().
        Vec3 headEye;
        if (avatar.Spawned() && avatar.HeadEyeWorld(headEye)) {
          const Vec3 want = headEye - player.pos;
          const float hl = CurrentTuning().avatar.firstPersonHeadHalflife;
          const float k =
              hl <= 1e-4f ? 1.0f : 1.0f - std::exp2(-dt / hl);
          fpHeadDelta = fpHeadValid ? fpHeadDelta + (want - fpHeadDelta) * k
                                    : want;
          fpHeadValid = true;
          eye = player.pos + player.RenderBodyOffset(tickAlpha) + fpHeadDelta;
        } else {
          fpHeadValid = false;
        }
        const float fpFwd =
            CurrentTuning().avatar.firstPersonForward / kVoxelMeters;
        eye = eye + cam.FlatForward() * fpFwd;
      } else {
        fpHeadValid = false;
      }
      // ...and the BODY gets the same two corrections, or the art and the
      // camera disagree. The avatar is posed once per 30 Hz tick around
      // `player.pos`, so before this the figure stair-stepped at the tick rate
      // under a camera that glides, and a step-up put it a whole voxel higher
      // in a single frame while the eye eased up over viewSmoothHalflife.
      // Render-only, applied in ONE place (Mob::AppendXforms) — the colliders,
      // the reach tests and every strike still read the posed transforms.
      avatar.SetRenderOffset(player.RenderBodyOffset(tickAlpha));
      // First-person part-hiding mask, hoisted so the portrait pass below can
      // restore it after drawing the whole body. See the note at its fill.
      std::vector<uint8_t> hide;
      // ---- avatar camera ----
      // The rig only decides where the RENDER eye sits. Picking rays, the
      // brush, the laser and the grenade all keep using player.EyePos(), so
      // switching to third person cannot change anything the sim sees — the
      // same guarantee the view-smoothing offset already relies on. (The E
      // look-at ray is the one deliberate exception: a UI query that has to
      // agree with the crosshair, reach-limited from the head; see its note.)
      {
        const AvatarLocomotion loco = avatar.Locomotion();
        // ORBIT THE PLAYER, NOT THE ART. The obvious-looking choice — the head
        // joint's world anchor — is wrong three times over: that transform is
        // read back from Jolt (one tick latent), it is driven by the gait's
        // bob/sway, and it swings with every animation. Orbiting it makes the
        // camera chase a lagging, bobbing point, which reads as constant jank
        // and, worse, makes walking feel like it does nothing: the boom is
        // still catching up to where the body was rather than following where
        // the player IS.
        //
        // The player's own eye is authoritative, frame-current, and already
        // step-smoothed (ViewEyePos), so it is the only stable thing to orbit.
        // The avatar merely rides along.
        Vec3 focus = eye;
        tpRig.Update(dt, camMode, cam, focus, loco, world, kindAt);
        if (camMode != CameraMode::First) eye = tpRig.EyePos();
        pourFrom = eye;
        pourFromValid = true;

        // Hide the body in first person so the player is not inside their own
        // hat, but keep the arms and the staff — seeing your own hands is most
        // of what sells a first-person body.
        //
        // HOISTED out of this block because the AVATAR PORTRAIT needs it back.
        // The hide mask feeds the shared bodyInstances/microInsts buffers, so
        // the portrait (which must show the whole body) and the main view
        // (which in first person must not) cannot both read one upload — the
        // portrait pass below re-uploads with an empty mask, draws, and then
        // restores this one for the main pass.
        hide.clear();
        if (avatar.Spawned()) {
          const AvatarParts& p = avatar.Parts();
          // Sized from the LIVE rig, not the def: a held item borrows an
          // appended slot, so the def's limb count is one short whenever the
          // player is armed and the weapon would never be addressable here.
          hide.assign(avatar.PartCount(), 0);
          const int heldPart = avatar.HeldSlot();
          if (camMode == CameraMode::First) {
            // Show the whole body except the head (its inside would fill the
            // view). Worn shells over the head are hidden too -- and so is
            // anything else hung off it, which since long hair became a BASE
            // limb (a generated character's `hair`, parent "head") includes
            // base slots: starting the sweep at AppendedBase() drew the hair
            // across the camera. Parents precede children in both ranges
            // (the base rig is ParentsFirst, an appended slot's host is an
            // earlier slot), so one forward pass reaches every descendant.
            if (p.head >= 0 && p.head < (int)hide.size()) hide[p.head] = 1;
            for (int i = 0; i < (int)hide.size(); i++) {
              const int par = avatar.PartParent(i);
              if (par >= 0 && par < (int)hide.size() && hide[par] == 1)
                hide[i] = 1;
            }
          }
          // NOTHING TO HIDE FOR UNHELD ITEMS ANY MORE. The rig used to carry
          // one part per possible weapon and hide the ones not in hand, which
          // is what made "equipping" a visibility trick. An item is now a
          // standalone asset that borrows a slot only while actually held, so
          // an unequipped sword has no part at all — there is nothing to hide,
          // and the rig cannot accumulate luggage it is not carrying.
          avatar.SetHiddenParts(hide);
        }

        // Speed-driven FOV: widens toward a sprint and eases back. Purely a
        // feel knob, and eased with the same half-life form as the rig so it
        // behaves identically at any frame rate.
        const auto& tp = CurrentTuning().thirdPerson;
        float sp = Vec3{player.vel.x, 0, player.vel.z}.len() * kVoxelMeters;
        float ref = std::max(CurrentTuning().player.sprintSpeed, 0.01f);
        float fovGoal = CurrentTuning().camera.fovY +
                        tp.speedFov * std::clamp(sp / ref, 0.0f, 1.0f);
        float k = tp.speedFovHalflife <= 1e-4f
                      ? 1.0f
                      : 1.0f - std::exp2(-dt / tp.speedFovHalflife);
        fovNow += (fovGoal - fovNow) * k;
        cam.fovY = fovNow;
      }
      // ---- WORLD -> SCREEN for the References page (a door's swing arc) ----
      // The inverse of raymarch.wgsl's primary ray (see ProjectToPortrait),
      // against THIS frame's render eye. The UI draws with it and owns no
      // camera convention of its own.
      {
        const Vec3 pfwd = cam.Forward(), pright = cam.Right(), pup = cam.Up();
        const float th = std::tan(cam.fovY * 0.5f);
        int winW = 1, winH = 1;
        glfwGetWindowSize(window, &winW, &winH);   // ImGui's DisplaySize
        const float dispX = (float)std::max(winW, 1), dispY = (float)std::max(winH, 1);
        const float asp = dispX / dispY;
        const Vec3 peye = eye;
        ui.projectWorld = [=](const float w[3], float out[2]) {
          const Vec3 d = Vec3{w[0], w[1], w[2]} - peye;
          const float z = d.dot(pfwd);
          if (z <= 0.05f) return false;
          out[0] = (0.5f + 0.5f * d.dot(pright) / (z * th * asp)) * dispX;
          out[1] = (0.5f - 0.5f * d.dot(pup) / (z * th)) * dispY;
          return true;
        };
      }

      // ---- audio ----
      // THE EARS ARE ON THE CHARACTER, NOT ON THE CAMERA. `eye` is the RENDER
      // eye and in third person that is a boom several metres behind the body,
      // so using it moved the whole soundscape backwards the moment you pressed
      // the camera key: distances, doppler and — worst — occlusion were all
      // solved from the boom, which routinely sits inside the wall behind you
      // and muffled everything. Third person now hears exactly what first
      // person hears.
      //
      // Position is `Player::ViewEyePos()` — the head, at ear height, in BOTH
      // modes, and the same value first person was already using. Deliberately
      // not the avatar's head joint: that transform is one tick latent out of
      // Jolt and rides the gait's bob and sway, which the listener would
      // convert into doppler wobble on every step (the same three reasons the
      // camera block above refuses to orbit it).
      //
      // Orientation stays `cam.yaw/pitch` — the LOOK direction, which is what
      // the ears face in first person and what the screen is showing in third.
      // The body's own heading is not it: in third person the model faces where
      // it RUNS (ResolveAvatarHeading), so strafing would swing the stereo
      // image away from the picture.
      //
      // Footfalls are drained here rather than inside the tick loop because
      // that loop runs up to 4 times per frame; firing from inside it would
      // put several steps at the same instant.
      sandvox::PerfSpan spanAudio(sandvox::PerfScope::Audio);
      // Interpolated for the same reason the camera above is: a listener
      // that teleports 30 times a second doppler-shifts every loop.
      const Vec3 earPos = player.RenderEyePos(tickAlpha);
      if (audioCues.Enabled()) {
        // THE LISTENER IS PUBLISHED FIRST, BEFORE ANY CUE IS FIRED (2026-09-19).
        // Every trigger below is placed relative to the pose AudioWorld is
        // holding (PlayOneShot ranks and pre-positions the voice against
        // `listener_`), so publishing at the END of the block — which is what
        // this did — spatialized this frame's impacts against LAST frame's head.
        // A mouse flick during a swing is tens of degrees per frame, and the
        // whole error lands on the one sound the player is listening hardest to.
        // The FULL Update still runs at the end of the block: it reaps bleed
        // loops, which must see the frame's MobBleed calls, and re-solves
        // occlusion once per voice.
        audioCues.PublishListener(earPos, cam.yaw, cam.pitch);
        for (const PlayerAvatar::Footfall& ff : avatar.Footfalls()) {
          if (ff.landing)
            audioCues.Land(ff.mat, ff.posVox, ff.fallSpeed);
          else
            audioCues.Footstep(ff.mat, ff.posVox, ff.speed, ff.foot);
        }
        // Terrain that came loose this frame. Drained here for the same reason
        // as the footfalls: PreTick runs once per tick and the tick loop runs
        // up to 4 times a frame, so voicing from inside it would stack several
        // snaps on one instant.
        for (const DebrisSystem::BreakEvent& be : debris.BreakEvents())
          audioCues.Break(be.material, be.posVoxel, be.sizeVoxels);

        // Debris that LANDED this frame, from the Jolt contact listener
        // (DESIGN.md §12b). Same drain-outside-the-tick-loop reason as the
        // breaks above; the material here is the surface that was STRUCK, and
        // the speed gate / per-body gap / per-step cap that keep a collapsing
        // wall from flooding the mixer all live in DebrisSystem.
        for (const DebrisSystem::ImpactEvent& ie : debris.ImpactEvents())
          audioCues.Impact(ie.material, ie.posVoxel, ie.energy);

        // A burst of MPM excitement (rock in a lake, floor carved under a
        // pool) reads as an impact through the water material's existing
        // slot — the Break precedent, driven from the same snapshot
        // readback. Once per snapshot tick, positioned at the last exciting
        // chunk's centre (coarse is fine for a splash).
        {
          const WorldSnapshot& fsn = world.Snap();
          if (fsn.valid && fsn.tick != lastFluidCueTick &&
              fsn.fluidExcitedEighths >= 64 && fluidCueMat != 0) {
            lastFluidCueTick = fsn.tick;
            uint32_t s = fsn.fluidLastSlot;
            IVec3 sc{(int)(s % kNChunk), (int)((s / kNChunk) % kNChunk),
                     (int)(s / (kNChunk * kNChunk))};
            IVec3 o = fsn.windowOrigin;
            int m = (int)kNChunk - 1;
            IVec3 wc{o.x + ((sc.x - o.x) & m), o.y + ((sc.y - o.y) & m),
                     o.z + ((sc.z - o.z) & m)};
            Vec3 pos{wc.x * 16.0f + 8.0f, wc.y * 16.0f + 8.0f,
                     wc.z * 16.0f + 8.0f};
            audioCues.Impact(fluidCueMat, pos,
                             std::min(fsn.fluidExcitedEighths / 64.0f, 8.0f));
          }
        }

        // ---- MELEE COMBAT (assets/sound_schema.js, the `combat` owner) -----
        //
        // Three sounds a fight makes that belong to no material and no
        // creature. Drained here rather than fired at the point of the event
        // for the reason the whole block exists: the events happen inside the
        // tick loop, which runs 0..4 times a frame, and audio is a per-frame
        // job.
        //
        // A SWORD BLOW CAN LEGITIMATELY MAKE SIX SOUNDS: `whoosh` when the
        // cut committed, `strike_edge` or `strike_blunt` for the weapon's own
        // ring/thud, `flesh` when it landed on a body, `cut` for the wet
        // slicing of an edged weapon in flesh, then the creature's own `hurt`
        // (or `sever` + `dismember` if a limb came off) from the blocks below.
        //
        // AND THE WHOOSH MOVES WHILE IT PLAYS. The other five are instants: a
        // blade touched a body at one place at one moment. A whoosh is the air a
        // blade is STILL shifting, so its voice tracks the weapon for as long as
        // the sample lasts, which is what makes a cut from left to right pan
        // from left to right instead of hanging where the cut committed.
        //
        // `whooshPan` scales the whole offset from the ear, because this is the
        // one sound in the game whose source is IN THE PLAYER'S OWN HANDS: at 0
        // it collapses onto the head and is effectively mono, which is the dial
        // to reach for if a swing sweeping across your own stereo image reads as
        // wrong rather than as physical.
        {
          const Tuning::CombatFx& fxa = CurrentTuning().combatfx;
          auto whooshAt = [&](const Vec3& p) {
            return earPos + (p - earPos) * fxa.whooshPan;
          };
          if (combatWhooshCue.pending) {
            combatWhooshVoice =
                audioCues.Combat(audio::Cues::CombatCue::Whoosh,
                                 whooshAt(combatWhooshCue.at),
                                 combatWhooshCue.power);
          }
          if (!audioCues.CombatActive(combatWhooshVoice))
            combatWhooshVoice = -1;   // sample ended: stop paying for the follow
          if (combatWhooshVoice >= 0) {
            Vec3 eb, et, ef;
            float ehw = 0;
            if (avatar.WeaponEdge(eb, et, ehw, &ef)) {
              audioCues.MoveCombat(
                  combatWhooshVoice,
                  whooshAt(eb + (et - eb) * fxa.whooshEdgeFrac));
            } else {
              // Weapon sheathed, dropped, or the arm holding it came off
              // mid-swing: leave the sound where it was made rather than
              // teleporting it to the player.
              combatWhooshVoice = -1;
            }
          }
        }
        if (combatStrikeCue.pending) {
          audioCues.Combat(combatStrikeEdged
                               ? audio::Cues::CombatCue::StrikeEdge
                               : audio::Cues::CombatCue::StrikeBlunt,
                           combatStrikeCue.at, combatStrikeCue.power,
                           combatStrikeCue.gainDb);
        }
        if (combatFleshCue.pending) {
          audioCues.Combat(audio::Cues::CombatCue::Flesh, combatFleshCue.at,
                           combatFleshCue.power, combatFleshCue.gainDb);
        }
        if (combatCutCue.pending) {
          audioCues.Combat(audio::Cues::CombatCue::Cut, combatCutCue.at,
                           combatCutCue.power, combatCutCue.gainDb);
        }
        if (combatClangCue.pending) {
          audioCues.Combat(audio::Cues::CombatCue::Clang, combatClangCue.at,
                           combatClangCue.power, combatClangCue.gainDb);
        }
        // ---- BLOWS AN NPC LANDED (game/melee.h StrikeEvent, 2026-09-28) ----
        //
        // The latches above are the PLAYER's own sweep. An NPC's sweep had no
        // cue path at all, so an enemy's sword in your ribs was silent unless
        // a limb came off. MeleeSweepDamage now reports each NPC stroke's
        // first contact with each creature, and it gets the same layers a
        // player blow does: the weapon's ring/thud, flesh or clang, and the
        // wet cut for an edge in flesh.
        //
        // A BLOW ON THE PLAYER IS NEVER DROPPED: no cap, and `priority` takes a
        // voice even from a saturated pool. Everyone else's are capped per
        // frame so a brawl across the yard cannot drown the mixer.
        {
          const uint64_t meId = avatar.Spawned() ? avatar.Id() : 0;
          int others = 0;
          for (const StrikeEvent& ev : mobs.StrikeEvents()) {
            const bool onMe = meId != 0 && ev.victimId == meId;
            if (!onMe && others >= 4) continue;
            if (!onMe) others++;
            const float pw = std::clamp(ev.power, 0.0f, 1.0f);
            audioCues.Combat(ev.edged ? audio::Cues::CombatCue::StrikeEdge
                                      : audio::Cues::CombatCue::StrikeBlunt,
                             ev.at, pw, ev.gainDb, onMe);
            audioCues.Combat(ev.flesh ? audio::Cues::CombatCue::Flesh
                                      : audio::Cues::CombatCue::Clang,
                             ev.at, pw, ev.gainDb, onMe);
            if (ev.flesh && ev.edged)
              audioCues.Combat(audio::Cues::CombatCue::Cut, ev.at, pw,
                               ev.gainDb, onMe);
          }
        }
        // Liquid that landed in a flask since the last frame: one call, at
        // the fill the LAST cell reached (Cues::FlaskFill rate-limits).
        if (!session.flaskFills.empty())
          audioCues.FlaskFill(session.flaskFillAt, session.flaskFills.back(),
                              (int)session.flaskFills.size());

        // Limbs that came off this frame. The creature's cry fires for every
        // sever; the wet CUT only for one made by a blade, because an
        // explosion that takes the same arm off did not saw through anything.
        for (const MobSystem::SeverEvent& se : mobs.SeverEvents()) {
          if (se.defIndex < 0 || se.defIndex >= (int)mobs.Defs().size()) continue;
          const MobDef& md = mobs.Defs()[(size_t)se.defIndex];
          audioCues.MobSound(md, audio::Cues::MobEvent::Sever, se.posVoxel,
                             se.severity, se.mobId);
          if (se.byBlade)
            audioCues.MobSound(md, audio::Cues::MobEvent::Dismember,
                               se.posVoxel, se.severity, se.mobId);
        }

        // ---- WHAT A CORPSE SAYS (2026-09-20) ------------------------------
        //
        // The same two takes a living creature's dismemberment makes, minus
        // the cry: a corpse does not scream, but flesh parting is flesh
        // parting and a body being hacked apart used to make the noise a
        // CRATE makes (main.cpp's tier logic infers the cue from the mob
        // queues, and dead flesh fills neither). The def rides on the event
        // because the mob that owned this flesh is long despawned, so a
        // corpse still makes its OWN species' wet sounds.
        //
        // `Sever` is the creature's take for a limb coming off and is voiced
        // here only when a piece actually came off; `Dismember` is the wet
        // tearing layer and, exactly as for the living, only a BLADE arms it
        // (a mace that caves a corpse in did not saw through anything).
        for (const DebrisSystem::GoreEvent& ge : debris.GoreEvents()) {
          if (ge.defIndex < 0 || ge.defIndex >= (int)mobs.Defs().size())
            continue;
          const MobDef& md = mobs.Defs()[(size_t)ge.defIndex];
          if (ge.severed)
            audioCues.MobSound(md, audio::Cues::MobEvent::Sever, ge.posVoxel,
                               ge.severity, 0);
          if (ge.severed && ge.byBlade)
            audioCues.MobSound(md, audio::Cues::MobEvent::Dismember,
                               ge.posVoxel, ge.severity, 0);
        }

        // Creatures hurt and killed this frame. Same shape as the severs
        // above; the def index rides on the event because a killing blow
        // despawns the mob before this drains. `mobId` is the rate-limiter
        // key, which is what makes a burst of per-tick laser damage one cry.
        for (const MobSystem::VoiceEvent& ve : mobs.VoiceEvents()) {
          if (ve.defIndex < 0 || ve.defIndex >= (int)mobs.Defs().size()) continue;
          const MobDef& md = mobs.Defs()[(size_t)ve.defIndex];
          // Every NPC death names its cause on the console (Mob::DeathCause):
          // four mechanisms end in the same ragdoll, and "he fell apart when I
          // hit him" is otherwise unattributable from the game.
          if (ve.kind == MobSystem::VoiceKind::Death)
            std::printf("mob %s (id %llu) died: %s\n", md.name.c_str(),
                        (unsigned long long)ve.mobId, mobs.DeathCause(ve.mobId));
          audioCues.MobSound(md,
                             ve.kind == MobSystem::VoiceKind::Death
                                 ? audio::Cues::MobEvent::Death
                                 : audio::Cues::MobEvent::Hurt,
                             ve.posVoxel, ve.intensity, ve.mobId);
        }

        // Wounds still pumping. Reported every frame while they bleed; the
        // audio layer starts, tracks and reaps the loop from that alone, so
        // nothing here has to remember a handle.
        for (const MobSystem::BleedSource& bs : mobs.BleedSources())
          audioCues.MobBleed(bs.key, bs.posVoxel, bs.intensity);

        // The night bed. `want` eases the bed in after dusk and out at dawn;
        // `allowStart` is rolled at most once every nightRetrySeconds and is
        // what makes it rare rather than a permanent night-time backing track.
        {
          const Tuning::Audio& ta = CurrentTuning().audio;
          // Derived from the tick, exactly as the sim and the sky do — not
          // from wall time — so the bed rises and falls with the same clock
          // the world is lit by, and a replay hears it at the same moment.
          const Tuning& tt = CurrentTuning();
          const uint32_t phase =
              DayPhaseForTick(tick, TicksPerDay(tt), tt.dayNight.freeze != 0,
                              (uint32_t)tt.dayNight.freezePhase);
          // DaylightStrengthCpu is 0 through the whole night and climbs after
          // sunrise, so this is 1 at night and 0 by day, with the twilight
          // wedge doing the crossfade for free.
          const float day = (float)DaylightStrengthCpu(phase) / 255.0f;
          const float want = std::clamp(1.0f - day * 4.0f, 0.0f, 1.0f);
          bool allowStart = false;
          if (want > 0.0f) {
            nightRollTimer += dt;
            if (nightRollTimer >= ta.nightRetrySeconds) {
              nightRollTimer = 0.0f;
              std::uniform_real_distribution<float> d(0.0f, 1.0f);
              allowStart = d(nightRng) < ta.nightChance;
            }
          } else {
            // Roll immediately on the first night after a day, rather than
            // making the player wait out a full retry period past dusk.
            nightRollTimer = ta.nightRetrySeconds;
          }
          audioCues.SetNightAmbience(earPos, want, allowStart);
        }

        audioCues.Update(dt, earPos, cam.yaw, cam.pitch, &world);
      }
      avatar.ClearFootfalls();
      // Cleared unconditionally, like the footfalls: a queue that only drains
      // when audio happens to be on is a slow leak on a silent machine.
      debris.ClearBreakEvents();
      debris.ClearImpactEvents();
      mobs.ClearSeverEvents();
      mobs.ClearVoiceEvents();
      mobs.ClearStrikeEvents();
      debris.ClearGoreEvents();
      // OUTSIDE the audio block, exactly like the queues above and for the
      // same reason stated there: a request that only clears when audio
      // happens to be on is a stuck flag on a silent machine, and the next
      // frame with audio would play a whoosh for a cut made minutes ago.
      combatWhooshCue.pending = false;
      combatFleshCue.pending = false;
      combatClangCue.pending = false;
      combatStrikeCue.pending = false;
      combatCutCue.pending = false;
      // The haft's softer level is per hit, never carried into the next frame's
      // latch (the block cue writes power/at without touching it).
      combatFleshCue.gainDb = combatClangCue.gainDb = 0.0f;
      combatStrikeCue.gainDb = combatCutCue.gainDb = 0.0f;
      session.flaskFills.clear();
      // (The hit flash is NOT decayed here. It ages on the tick, inside
      // MobSystem::PreTick, because a frame-driven decay is never called by
      // the selftest — see MobSystem::DecayHitFlash.)
      // SCOPE BOUNDARY, decided at the combat/perf merge: `audio` closes AFTER
      // the combat cue latches, on purpose and for the same reason it already
      // closed after ClearFootfalls/ClearBreakEvents above — these are audio
      // event queues, so draining them is audio work and belongs on the audio
      // row. Three bool stores; it cannot make the row lie either way, but the
      // row's name stays true to what it encloses.
      spanAudio.Close();
      // Adaptive fog: pin the fade to whatever cascade radius is actually
      // filled, so a backlogged refill (spawn, load, teleport, sprinting past
      // a level's hysteresis) fogs out the pending bands instead of showing
      // sky holes through them. Clamped to [kFarFogDensity, kFarFogDensityMax]
      // — never thinner than the full-horizon pin, never so thick that the
      // residency window itself disappears — then eased so the horizon opens
      // smoothly rather than stepping with each landed plane.
      //
      // The budget and the ease are render.fogOpticalDepths /
      // render.fogLerpPerFrame (read here since 2026-09-24; they loaded into
      // nothing before). world.h's constexpr pins are the same numbers for the
      // --shot harnesses, so the clamp bounds scale by the knob's ratio to
      // them: exactly 1 at the shipped default, which keeps this bit-identical.
      static_assert(TPD(render, fogOpticalDepths) == kFogOpticalDepths &&
                        TPD(render, fogLerpPerFrame) == kFogLerpPerFrame,
                    "tuning_params.def fog defaults must match world.h's pins");
      const Tuning::Render& fogTun = CurrentTuning().render;
      const float fogScale = fogTun.fogOpticalDepths / kFogOpticalDepths;
      float fogTarget = std::clamp(
          fogTun.fogOpticalDepths / far.SafeRadiusMeters(),
          kFarFogDensity * fogScale, kFarFogDensityMax * fogScale);
      fogSmooth += (fogTarget - fogSmooth) * fogTun.fogLerpPerFrame;
      // ---- short range: the panel checkbox is the live authority ----------
      // Pushed into support.cpp rather than OR'd into extraFlags below so that
      // ONE place decides the flag bit for every drawing path — the portrait
      // pass, the lab and --shot all write RenderParams through the same
      // function and would otherwise each need their own copy of this.
      SetShortRange(ui.shortRange);
      SetShortRangeNear(ui.shortRangeNear);
      // The panel's draw-distance readout, and the evidence that ticking the
      // box did something. Normally the cascade's FILLED radius (the same
      // number the adaptive fog is pinned to just above, so the two cannot
      // disagree about how far the world is trusted); the ceiling when the
      // mode is on. render.shortRangeDist is read live so the tuner's slider
      // moves this the moment F5 lands.
      ui.renderRangeM =
          ui.shortRange ? (ui.shortRangeNear
                               ? CurrentTuning().render.shortRangeNearDist
                               : CurrentTuning().render.shortRangeDist)
                        : far.SafeRadiusMeters();
      // HOISTED INTO A LAMBDA because it may have to run TWICE. world.renderUBO
      // is one buffer, so the avatar portrait's camera necessarily clobbers
      // the main camera; the portrait pass writes its own params, submits, and
      // then calls this again to put the world's camera back in front of the
      // main pass. One definition, so the two cannot drift.
      // The world's render size this frame. `scaled` is the whole switch: at
      // 1.0 (or on a surface that cannot be blitted into) the frame renders
      // straight into the swapchain and nothing below changes.
      const float renderScale = CurrentTuning().render.renderScale;
      const bool scaled = renderScale < 0.999f && ctx.SwapchainBlittable();
      const uint32_t renderW =
          scaled ? std::max(1u, (uint32_t)std::lround(ctx.width * renderScale))
                 : ctx.width;
      const uint32_t renderH =
          scaled ? std::max(1u, (uint32_t)std::lround(ctx.height * renderScale))
                 : ctx.height;
      // ---- TAA: the sub-pixel camera jitter ---------------------------------
      // `taaOn` also forces the OFFSCREEN path at scale 1: the resolve reads
      // the finished world frame out of a texture it can copy from, and the
      // swapchain image is not that.
      const bool taaOn = CurrentTuning().render.taa != 0 && sim.TaaAvailable() &&
                         ctx.SwapchainBlittable();
      // ---- the shading-LOD filter (denoise.wgsl) ----------------------------
      // Runs IN PLACE over the offscreen world frame between the world pass
      // and whatever consumes it (TAA or the blit), so like TAA it forces the
      // offscreen path at scale 1: the swapchain image cannot be copied out of.
      const bool denoiseOn = CurrentTuning().render.denoise != 0 &&
                             sim.DenoiseAvailable() && ctx.SwapchainBlittable();
      const bool offscreen = scaled || taaOn || denoiseOn;
      //
      // THE JITTER IS A YAW/PITCH NUDGE, not a shear of the basis, and that is
      // the whole reason it is safe. Every path that draws this frame — the
      // raymarch's ray construction AND projectView for bodies, particles,
      // sprites and the debug arrows — derives its basis from the SAME
      // RenderParams, so perturbing the Camera the params are written from
      // moves all of them by exactly the same amount. There is no second place
      // to keep in step and no way for the raster and the ray to disagree.
      //
      // Gameplay is untouched: `cam` itself is not modified, only the copy
      // handed to WriteRenderParams. Picking, the brush ray, the player's
      // movement basis and every mirror query still see the true camera, so
      // nothing that reaches the sim can see the jitter — rule 1 is not in
      // play here at all, but "render-only means render-only" is cheap to keep.
      Camera jcam = cam;
      Simulation::TaaCamera taaCam{};
      if (taaOn) {
        ApplyTaaJitter(cam, eye, (float)ctx.width / (float)ctx.height, renderW,
                       renderH, taaFrameNo, CurrentTuning().render.taaJitter,
                       jcam, taaCam);
      }
      // viewPx is the RENDER height: every footprint / cone-width term in the
      // raymarch is derived from it, so a scaled frame tells the shader its
      // real pixel size rather than the window's.
      //
      // …EXCEPT under TAA with render.taaSharpLod, where it is deliberately
      // told the NATIVE height instead. That is the voxel equivalent of the
      // negative mip bias every temporal upscaler ships with: `viewPx` drives
      // the plant LOD distance, the water ripple footprint and the
      // micro-detail cutoff, so a scaled frame without it is not merely
      // sampled more coarsely — it is DRAWN with coarser content, and detail
      // the renderer chose not to draw is detail no accumulator can recover.
      const float viewPxThisFrame =
          (taaOn && CurrentTuning().render.taaSharpLod != 0) ? (float)ctx.height
                                                             : (float)renderH;
      auto writeMainRenderParams = [&] {
        // renderW x renderH is the target, whatever viewPx says for LOD.
        WriteRenderParams(ctx.queue, world, eye, jcam,
                          (float)ctx.width / (float)ctx.height, ui.shadows,
                          (float)now, fogSmooth, viewPxThisFrame, tick,
                          fluidCount,
                          (float)(accumulator / kTickDt),
                          ui.showDirtyVoxels ? 2u : 0u, renderW, renderH);
      };
      writeMainRenderParams();
      // actVoxViz is filled GPU-side by sim_step.wgsl when vizActive is set;
      // the old CPU dirtyViz upload (stamp-comparison) is no longer needed.

      // Celestial readout for the panel. Recomputed rather than cached out of
      // WriteRenderParams because the solve is a handful of trig calls once a
      // frame — cheaper than the plumbing to carry it, and it cannot go stale.
      {
        const SkyState sky = ComputeSky(CurrentTuning(),
            Celestial().RenderTickInterp(tick, accumulator / kTickDt));
        ui.skyDayT = sky.dayT;
        ui.skyYearT = sky.yearT;
        ui.skyMoonPhase = sky.moonPhase;
        ui.skyMoon2Phase = sky.moon2Phase;
        ui.skySolarEclipse = sky.solarEclipse;
        ui.skySunElevDeg =
            std::asin(std::clamp(sky.sunDir[1], -1.0f, 1.0f)) * 57.2957795f;
      }

      ui.fps = fpsSmooth;
      ui.frameMs = frameMsSmooth;
      ui.frameMsWorst = frameMsWorst;
      ui.frameMsP95 = frameMsP95;
      ui.frameMsP99 = frameMsP99;
      ui.tickCpuMs = tickMsSmooth;
      ui.tick = tick;
      ui.activeChunks = world.Snap().activeChunks;
      ui.totalChunks = kNumSlots;
      ui.voxelTotal = world.Snap().voxelTotal;
      ui.worldHash = world.Snap().worldHash;
      ui.mirrorValid = world.Snap().valid;
      ui.particleCount = world.Snap().particleCount;
      ui.bodyCount = debris.BodyCount();
      ui.activeBodyCount = debris.ActiveBodyCount();
      ui.prefabPending = (uint32_t)placer.PendingCount();
      // The LIVING: a corpse is a Mob now (PLAN_corpse_is_a_mob.md) and the
      // overlay's "mobs" is the crowd, which is what the spawn cap counts.
      ui.mobCount = mobs.LiveMobCount();

      // What the wardrobe's picked colour is CALLED (game/dye.h). Mirrored
      // rather than computed in the overlay so the UI keeps its "reads
      // UIState, knows no headers" shape — and so the name the panel shows is
      // literally the one the item's tooltip will carry.
      ui.wardrobeColorName = DyeName(DyePack(
          ui.wardrobeColor[0], ui.wardrobeColor[1], ui.wardrobeColor[2]));

      // ---- NPC AI panel mirror (game/ai_behavior.h) ------------------------
      //
      // The overlay never reaches into MobSystem; everything it draws is
      // mirrored here, and everything it asks for comes back as a flag. One
      // line per creature: who it is, what character it is running, what it
      // DECIDED this tick and how far its target is. That last pair is the
      // whole debugging surface — "it walked at me" and "it scored Approach
      // 1.15 against HoldRange 0.6" are very different amounts of information.
      //
      // ONLY WHILE THE WINDOW IS OPEN: an snprintf and a std::string per
      // creature per frame for a list nothing draws otherwise. The one other
      // reader, the tick's "apply behaviour" latch (session.cpp), is set by a
      // button in this same window, so it is only ever raised against a list
      // built while the window was up.
      ui.aiMobIds.clear();
      ui.aiMobLabels.clear();
      for (uint32_t i = 0; ui.aiWindowOpen && i < mobs.MobCount(); i++) {
        const uint64_t mid = mobs.MobIdAt(i);
        if (mid == 0) continue;
        if (!mobs.IsAlive(mid)) continue;   // the dead have no AI to show
        const ai::Brain* br = mobs.MobBrain(mid);
        const ai::Profile* pf =
            br != nullptr ? mobs.Behaviors().At(br->profile) : nullptr;
        char line[256];
        // ...and the fight as it reads it: blows answered (guard / dodge),
        // feints thrown, ripostes landed, rules holding.
        std::snprintf(line, sizeof line,
                      "#%llu  %-16s %-10s %s d=%.1f%s  g%u d%u f%u r%u rules %x",
                      (unsigned long long)mid,
                      pf != nullptr ? pf->name.c_str() : "(no ai)",
                      br != nullptr ? ai::IntentName(br->intent) : "-",
                      br != nullptr && br->hasTarget
                          ? (br->visible ? "seen" : "lost")
                          : "----",
                      br != nullptr ? br->targetDist : 0.0f,
                      br != nullptr && br->path.valid ? "  [path]" : "",
                      br != nullptr ? br->guards : 0u,
                      br != nullptr ? br->dodges : 0u,
                      br != nullptr ? br->feints : 0u,
                      br != nullptr ? br->ripostes : 0u,
                      br != nullptr ? br->rulesHeld : 0u);
        ui.aiMobIds.push_back(mid);
        ui.aiMobLabels.push_back(line);
      }

      // ---- the attack seam, consumed ---------------------------------------
      // Phase C replaces this with a real stroke. Until then the requests are
      // DRAINED AND SHOWN, which is the correct stub: a seam nobody reads is a
      // seam nobody notices has stopped firing. Drained here, once per FRAME,
      // after the tick loop has run 0..4 times — the requests accumulate across
      // those sub-ticks exactly as sever and voice events do, precisely because
      // a per-frame read of a per-tick event otherwise drops three of four.
      for (const ai::AttackRequest& r : mobs.AttackRequests()) {
        char line[192];
        std::snprintf(line, sizeof line,
                      "mob #%llu -> #%llu  style \"%s\"  at (%.0f,%.0f,%.0f)  "
                      "d=%.1f  commit %u ticks  (tick %u)",
                      (unsigned long long)r.mobId,
                      (unsigned long long)r.targetId, r.style.c_str(),
                      r.targetPoint.x, r.targetPoint.y, r.targetPoint.z,
                      r.distance, r.commitTicks, r.tick);
        ui.aiLastAttack = line;
        ui.aiAttackCount++;
      }
      mobs.ClearAttackRequests();
      // ---- BLADE ON BLADE (game/melee.h BlockEvent) -----------------------
      //
      // Drained here for the same reason the attack requests above are: they
      // accumulate across the frame's 0..4 sub-ticks, and a per-frame read of a
      // per-tick event otherwise drops three of every four.
      //
      // THE CUE AND THE DIP, wired here (phase D's `CombatBlockCue`, defined up
      // with the hit-stop latch). `ev.at`/`ev.power` are on the event for
      // exactly this, and the lambda owns the latch, the peak-hold and the
      // `melee clang` slot, so this is one call rather than a policy.
      //
      // ONE FRAME LATE, KNOWINGLY. This drain sits below the audio drain, so a
      // clang raised here voices at the top of the next frame (~16 ms). That
      // is the same latency the hit-stop already has from EVERY producer — the
      // stop latch is drained above the tick loop, so the sweep's own requests
      // are next-frame too — so the two halves of a blocked blow stay together,
      // which is what matters. Hoisting the drain above the audio block would
      // make the sound early relative to its own dip, not less late.
      //
      // STILL MISSING: the spark burst. It wants `ev.at` and a particle spawn
      // list this block does not have.
      for (const BlockEvent& ev : mobs.BlockEvents()) {
        char line[160];
        std::snprintf(line, sizeof line,
                      "#%llu blocked by #%llu at (%.0f,%.0f,%.0f) power %.2f",
                      (unsigned long long)ev.attackerId,
                      (unsigned long long)ev.blockerId, ev.at.x, ev.at.y,
                      ev.at.z, ev.power);
        ui.aiLastBlock = line;
        ui.aiBlockCount++;
        // Every parry rings and every parry stops time a little, whoever threw
        // it and whoever caught it — a fight between two NPCs across the yard
        // is as audible as one in your face, at the volume the distance gives
        // it (audio::Cues::Combat spatializes on `ev.at`).
        CombatBlockCue(ev.at, ev.power);
        // THE PLAYER'S OWN GUARD IS BEATEN OPEN HERE and not in MobSystem,
        // because this MeleeState is main.cpp's: MobSystem::PushBlockEvent
        // nudges its own mobs and deliberately leaves the avatar to its owner.
        // Same counter-based draw, so both sides of an exchange shove the same
        // way (CLAUDE.md rule 1).
        if (avatar.Spawned() && ev.blockerId == avatar.Id()) {
          const uint32_t h = rng::Hash3((uint32_t)ev.blockerId ^ 0xB10Cu,
                                        (uint32_t)ev.attackerId, 0);
          melee.Nudge((h & 1u ? 1.0f : -1.0f) * melee.tuning.blockNudgeAz *
                          ev.power,
                      (h & 2u ? 1.0f : -1.0f) * melee.tuning.blockNudgeEl *
                          ev.power);
        }
      }
      mobs.ClearBlockEvents();
      // Ledge-grab readout: the probe result plus every latch gate, so "why
      // didn't it grab" is readable in the panel rather than inferred. The
      // gates mirror the latch condition in Player::Update exactly.
      {
        const float nonJump =
            CurrentTuning().player.nonJumpSpeed / kVoxelMeters;
        char lg[160];
        if (player.hanging) {
          std::snprintf(lg, sizeof lg,
                        "HANGING lip(%d,%d,%d) — hold W: pull up, A/D: "
                        "shimmy, re-tap space: jump, ctrl: drop",
                        player.hangLip.x, player.hangLip.y, player.hangLip.z);
        } else if (player.ledgeInReach) {
          std::snprintf(
              lg, sizeof lg,
              "lip(%d,%d,%d) IN REACH — air=%d space=%d velOk=%d%s",
              player.ledgeLip.x, player.ledgeLip.y, player.ledgeLip.z,
              player.grounded ? 0 : 1, pin.Held(TB_JUMP) ? 1 : 0,
              player.vel.y <= nonJump ? 1 : 0,
              player.grounded ? "  (jump at it holding space)" : "");
        } else {
          std::snprintf(lg, sizeof lg,
                        "no lip in reach (need a wall top between shoulders "
                        "and fingertips)");
        }
        ui.ledgeState = player.hanging ? 2 : (player.ledgeInReach ? 1 : 0);
        ui.ledgeText = lg;
      }
      // magic readout: cost must be visible BEFORE the cast, which is what
      // makes the mana/health crossover a decision rather than a surprise.
      ui.mana = caster.mana.mana;
      ui.manaMax = caster.mana.EffectiveMax();
      ui.manaPoolMax = caster.mana.manaMax;
      ui.manaReserved = caster.mana.reserved;
      ui.spellStatuses.clear();
      for (const SpellStatus& st : spells.Statuses()) {
        if (st.casterId != kPlayerCasterId) continue;
        const GlyphDef* ig = glyphs.At(st.effect.inner.empty() ? -1 : st.effect.inner[0].glyph);
        const GlyphDef* ag = glyphs.At(st.effect.glyph);
        std::string line =
            std::string(ig ? ig->id : "mod") + " " + (ag ? ag->id : "aura") +
               (st.target == kPlayerCasterId ? " on you" : (st.target ? " on them" : " on the place")) +
               "  " + std::to_string(st.perTick) + "/tick, " +
               std::to_string(st.ticksLeft / 30) + " s";
        ui.spellStatuses.push_back(line);
      }
      for (const SpellBeam& bm : spells.Beams())
        if (bm.casterId == kPlayerCasterId)
          ui.spellStatuses.push_back("beam  " + std::to_string(bm.perTick) + "/tick");
      // FROZEN ONCE DEAD. The dying observer above took this same mirror at
      // the instant of death; from here on the rig is a pile of debris
      // handles, so refilling it would replace the readout of the body that
      // died with a readout of nothing. Thawed by the respawn below.
      if (!deathFrozen) {
        ui.health = playerHealth.Get();
        ui.healthMax = avatar.HealthMax();
        ui.healthCap = avatar.HealthCap();
        ui.playerAlive = avatar.IsAlive();
        // Body-condition readout: one figure slot per limb, keyed by the
        // limb's authored TAG and side suffix rather than by part name, so any
        // humanoid rig fills the same figure. A limb the rig does not have
        // stays absent and simply is not drawn.
        FillBodyUI(avatar, burnMats, tissueMats, mobs, mats, ui);
        ui.locoState = avatar.Spawned() ? avatar.Locomotion().stateName : "";
      }
      ui.spellCost = caster.compiled.manaCost;
      ui.spellWord = caster.compiled.wordCost;
      ui.spellTariff = caster.compiled.tariff;
      ui.spellCarry = caster.compiled.carryCost;
      ui.spellPriceUnknown = caster.compiled.priceUnknown;
      ui.spellLastBillAge += dt;
      ui.spellText = caster.readout.text;
      ui.armedPage = caster.armedPage;
      ui.spellVerdict = caster.readout.verdict;
      ui.spellOutcome = (int)caster.lastOutcome;
      ui.liveProjectiles = spells.LiveCount();
      // Wind primitives: what is alive and what it costs (§4.3). `windPrims`
      // is the population; `windWakeChunks` is the rule-2 number — the chunks
      // those primitives are holding awake so they can move settled matter.
      ui.windPrims = (int)WindPrims().Count();
      ui.windWakeChunks = (int)WindPrims().LastWakeCount();
      // The spell readout cache (SpellDescCache, declared with glyphEpoch):
      // validate against the grimoire as it stands, then look words up. Called
      // again before the character-screen mirrors below, because the panel's
      // intents in between may have edited the grimoire.
      auto spellDescValidate = [&]() {
        std::string key;
        for (const GrimoirePage& pg : caster.grimoire.pages) {
          key += pg.name;
          key += pg.readOnly ? "\x1e" "1" : "\x1e" "0";
          for (const std::string& w : pg.words) {
            key += '\x1f';
            key += w;
          }
          key += '\x1d';
        }
        if (spellDesc.epoch != glyphEpoch || key != spellDesc.grimoireKey) {
          spellDesc.byWords.clear();
          spellDesc.epoch = glyphEpoch;
          spellDesc.grimoireKey = std::move(key);
        }
      };
      auto spellDescFor = [&](const std::vector<std::string>& words)
          -> const SpellDescCache::Entry& {
        std::string key;
        for (const std::string& w : words) {
          key += w;
          key += '\x1f';
        }
        auto it = spellDesc.byWords.find(key);
        if (it != spellDesc.byWords.end()) return it->second;
        // Bounded: every intermediate composer edit is a new key.
        if (spellDesc.byWords.size() >= 512) spellDesc.byWords.clear();
        SpellDescCache::Entry e;
        const GrimoireExpansion ex = ExpandWords(glyphs, caster.grimoire, words, kSpellStackMax);
        SpellStack st = StackOf(ex);
        const CastList l = CompileSpell(glyphs, st);
        const SpellReadout r = DescribeSpell(glyphs, l);
        e.text = r.text;
        e.readout = r.text;
        // THE VERDICT ON ITS OWN LINE. It is the sentence that says what the
        // page DOES ("a bolt that sprays sand and fire, three of them"),
        // which the bracket string above deliberately does not; the composer
        // wraps both rather than clipping either.
        if (!r.verdict.empty()) e.readout += "\n" + r.verdict;
        if (ex.dropped > 0) e.readout += "   (? = a word that no longer exists)";
        if (ex.truncated) e.readout += "   (cut at the stack bound)";
        e.price = l.manaCost;
        e.unknown = l.priceUnknown;
        e.dropped = ex.dropped;
        return spellDesc.byWords.emplace(std::move(key), std::move(e)).first->second;
      };
      // The readouts are read only by the character screen (inventory_ui.cpp);
      // the HUD's glyph bar draws the names and kinds, which stay per frame.
      if (ui.inventoryOpen) spellDescValidate();
      ui.glyphSlots.clear();
      ui.glyphSlotKinds.clear();
      ui.glyphSlotReadouts.clear();
      for (int i = 0; i < kGlyphSlots; i++) {
        const SlotKind k = caster.inventory.KindAt(i);
        ui.glyphSlotKinds.push_back((int)k);
        if (k == SlotKind::Page) {
          const std::string& name = caster.inventory.PageAt(i);
          ui.glyphSlots.push_back(name);
          ui.glyphSlotReadouts.push_back(
              ui.inventoryOpen ? spellDescFor({name}).text : std::string());
          continue;
        }
        int gi = caster.inventory.At(i);
        ui.glyphSlots.push_back(
            gi >= 0 && gi < (int)glyphs.glyphs.size() ? glyphs.glyphs[gi].id : "");
        ui.glyphSlotReadouts.push_back("");
      }
      ui.glyphSelected = caster.selected;
      // ---- THE SPELL BAR AND THE HANDS' SPELLS (spells in hand) -----------
      //
      // A spell's COLOUR is its flight's (the projectile block below): the
      // first matter it carries, else its delivery's look. One rule, so the
      // key on the bar, the light in the fist and the bolt that leaves it are
      // the same colour. A key's colour needs a compile, so it is cached per
      // key on what the key says and the glyph epoch.
      {
        auto spellColor = [&](const CastList& l) -> uint32_t {
          if (l.casts.empty()) return 0;
          const SpellCast& c = l.casts[0];
          const uint32_t tint = CastTintMaterial(c);
          const uint32_t base = tint != 0 && tint < mats.size()
                                    ? mats[tint].gpu.color0
                                    : glyphs.Delivery(c.delivery.glyph).look.color;
          return 0xFF000000u | (base & 0x00FFFFFFu);
        };
        auto glyphType = [&](const std::vector<int>& spoken) {
          const GlyphDef* g = spoken.size() == 1 ? glyphs.At(spoken[0]) : nullptr;
          return g ? (int)g->sort : -1;
        };
        static std::string keySaid[kGlyphSlots];
        static uint32_t keyColor[kGlyphSlots];
        static int keyType[kGlyphSlots];
        static uint32_t keyEpoch = ~0u;
        if (keyEpoch != glyphEpoch) {
          keyEpoch = glyphEpoch;
          for (std::string& k : keySaid) k = "\x01";   // never a real binding
        }
        ui.glyphSlotColors.assign(kGlyphSlots, 0);
        ui.glyphSlotTypes.assign(kGlyphSlots, -1);
        for (int i = 0; i < kGlyphSlots; i++) {
          const SlotKind k = caster.inventory.KindAt(i);
          std::string said = std::to_string((int)k) + ":" +
                             (k == SlotKind::Page ? caster.inventory.PageAt(i)
                                                  : std::to_string(caster.inventory.At(i)));
          // A page's words are what it says: editing the page recolours
          // its key (nested pages still wait for the next epoch).
          if (k == SlotKind::Page) {
            const int pi = caster.grimoire.Find(caster.inventory.PageAt(i));
            if (pi >= 0)
              for (const std::string& w : caster.grimoire.pages[pi].words) said += "\x1f" + w;
          }
          if (said != keySaid[i]) {
            keySaid[i] = said;
            keyColor[i] = 0;
            keyType[i] = -1;
            if (k != SlotKind::None) {
              PlayerCaster tmp;
              tmp.inventory = caster.inventory;
              tmp.grimoire = caster.grimoire;
              tmp.SpeakSlot(glyphs, i);
              keyColor[i] = spellColor(tmp.compiled);
              keyType[i] = k == SlotKind::Page ? -1 : glyphType(tmp.stack.spoken);
            }
          }
          ui.glyphSlotColors[i] = keyColor[i];
          ui.glyphSlotTypes[i] = keyType[i];
        }
        for (int hk = 0; hk < kHands; hk++) {
          const PlayerCaster::HandSpell& hs = caster.hand[hk];
          const bool on = hs.Equipped();
          ui.handSpell[hk] = on ? hs.name : std::string();
          ui.handSpellPage[hk] =
              on && caster.inventory.KindAt(hs.slot) == SlotKind::Page &&
              caster.inventory.PageAt(hs.slot) == hs.name;
          ui.handSpellColor[hk] = on ? spellColor(hs.compiled) : 0;
          ui.handSpellType[hk] = on ? glyphType(hs.stack.spoken) : -1;
          if (ui.handSpellType[hk] < 0 && on) ui.handSpellPage[hk] = true;
        }
      }
      ui.glyphBankB = captured && ui.magicMode &&
                      (key(GLFW_KEY_LEFT_SHIFT) || key(GLFW_KEY_RIGHT_SHIFT));
      caster.noteAge += dt;
      ui.spellNote = caster.note;
      ui.spellNoteAge = caster.noteAge;
      // hotbar + swing readout (game/item.h, game/melee.h)
      ui.itemNames.clear();
      for (int i = 0; i < kItemSlots; i++) {
        const ItemDef* d = items.Of(hotbar.slots[i]);
        // A vessel says what is in it, right on the strip: the only way to
        // know how much is left while pouring.
        if (d && d->IsContainer())
          ui.itemNames.push_back(d->name + " (" +
                                 ContainerFillText(*d, hotbar.slots[i], mats) + ")");
        else
          ui.itemNames.push_back(d ? d->name : "");
      }
      ui.itemSelected = hotbar.selected;
      // The portrait pour brush (inventory_ui.cpp Portrait): live only while
      // the flask chosen on the FLASKS row is FILLED, tinted with the
      // substance's own colour. A choice whose slot no longer holds a vessel
      // is dropped here, so the row never lights a slot the flask left.
      {
        const ItemStack* hp = kit.Resolve(ui.activeVessel);
        const ItemDef* hd = hp ? items.Of(*hp) : nullptr;
        if (!hd || !hd->IsContainer()) ui.activeVessel = KitRef{};
        ui.applyText.clear();
        ui.applyColor = 0;
        ui.applyStoppered = false;
        const uint16_t topMat = hp ? ContainerTopMat(*hp, &mats) : 0;
        if (hd && hd->IsContainer() && hp->Filled() && topMat < mats.size()) {
          const ItemStack& hs = *hp;
          ui.applyText = ContainerFillText(*hd, hs, mats);
          ui.applyStoppered = hs.stoppered;
          ui.pourDrainPerSec = PourBrushCellsPerSec(*hd, ui.pourRadius);
          // The brush paints with what comes out: the top layer.
          const uint32_t c = mats[topMat].gpu.color0;
          ui.applyColor = 0xFF000000u | (c & 0x00FFFFFFu);
        }
      }
      switch (melee.Phase()) {
        case SwingPhase::Idle:    ui.swingPhase = meleeReady ? "ready" : ""; break;
        case SwingPhase::Guard:   ui.swingPhase = "guard"; break;
        case SwingPhase::Wind:    ui.swingPhase = "winding"; break;
        case SwingPhase::Recover: ui.swingPhase = "recover"; break;
      }
      // THE CUT IS THE PROGRAM'S, not a driver phase (melee.h SwingPhase).
      if (session.playerStrike.Cutting()) ui.swingPhase = "CUT";
      ui.swingSpeed = melee.MouseSpeed();
      // Which authored strike is running (discrete mode): the label, so the
      // flick's read-back is on screen while the swing is. A banked follow-up
      // is shown too — it is the answer to "did my mid-swing click register".
      ui.swingStyle.clear();
      // ---- THE WEAPON READOUT SAYS "fists" (plan §6) ------------------------
      // Through `swingStyle` rather than a new UIState field, because
      // `ui/overlay.*` belongs to a concurrent session this package may not
      // edit (plan §1) — and this is the line that already sits under the
      // swing phase and already names what you are swinging.
      if (meleeUnarmed) ui.swingStyle = "fists";
      if (playerStrike.Active()) {
        if (const AttackStyle* sty = mobs.AttackStyles().At(playerStrike.style))
          ui.swingStyle =
              (meleeUnarmed ? "fists: " : "") + sty->label;
        if (strikeBuffered >= 0) {
          if (const AttackStyle* nxt = mobs.AttackStyles().At(strikeBuffered))
            ui.swingStyle += "  (next: " + nxt->label + ")";
        }
      }
      // ---- THE STRIKE COMPASS (overlay.h UIState::strikeCompass) -----------
      // Debug readout while a weapon is in hand: the same map the press reads
      // (strokes.h PlayerCompass), the picker's live velocity, and what the
      // last press resolved to. Render-only; nothing here feeds the tick.
      // Fists too: the unarmed compass charges exactly as a weapon's does.
      ui.strikeCompass = meleeReady && avatar.Spawned();
      if (ui.strikeCompass) {
        const StyleLibrary& lib = mobs.AttackStyles();
        const PlayerStrikeMap& map = PlayerCompass(lib, meleeArmed);
        auto baseName = [&](int si) -> std::string {
          const AttackStyle* st = lib.At(si);
          if (!st) return "?";
          // "horizontal_r@short:player" -> "horizontal_r [short]".
          std::string n = st->name;
          const size_t c = n.find(":player");
          if (c != std::string::npos) n.resize(c);
          const size_t at = n.find('@');
          if (at != std::string::npos) n = n.substr(0, at) + " [" + n.substr(at + 1) + "]";
          return n;
        };
        ui.strikeSectors.clear();
        for (const PlayerStrikeMap::Sector& sec : map.sectors) {
          UIState::StrikeSector u;
          const float len = std::sqrt(sec.x * sec.x + sec.y * sec.y);
          u.x = len > 1e-6f ? sec.x / len : 0.0f;
          u.y = len > 1e-6f ? sec.y / len : 0.0f;
          u.name = baseName(sec.style);
          u.neutral = sec.style == map.neutral[0] || sec.style == map.neutral[1];
          ui.strikeSectors.push_back(std::move(u));
        }
        // Which sector a style index lands on (the FIRST sector naming it,
        // which is also the one QuantizeStrike's max-dot picks among ties).
        auto sectorOf = [&](int si, float fx, float fy) {
          int best = -1;
          float bestDot = -1e9f;
          for (size_t k = 0; k < map.sectors.size(); k++) {
            if (map.sectors[k].style != si) continue;
            const float d = map.sectors[k].x * fx + map.sectors[k].y * fy;
            if (d > bestDot) { bestDot = d; best = (int)k; }
          }
          return best;
        };
        const StrikePicker& pk = session.strikePicker;
        ui.strikeFlickX = pk.vx;
        ui.strikeFlickY = pk.vy;
        ui.strikePickMin = std::max(1.0f, CurrentTuning().melee.pickMinSpeed);
        ui.strikeHover = -1;
        {
          float fx = 0, fy = 0;
          if (pk.Pick(ui.strikePickMin, fx, fy)) {
            const int si = QuantizeStrike(map, fx, fy);
            if (si >= 0) ui.strikeHover = sectorOf(si, fx, fy);
          }
        }
        const PlayerSession::StrikePickNote& note = session.lastStrikePick;
        if (note.serial != ui.strikeLastSerial) {
          ui.strikeLastSerial = note.serial;
          ui.strikeLastAge = 0.0f;
          ui.strikeLastFlicked = note.flicked;
          ui.strikeLastX = note.fx;
          ui.strikeLastY = note.fy;
          ui.strikeLastSector =
              note.flicked ? sectorOf(note.style, note.fx, note.fy) : -1;
          // Named as the weapon's FORM of it, which is what the press began.
          const int formed = lib.ResolveForm(
              note.style, FormForItem(heldItem));
          std::string t = note.style >= 0 ? baseName(formed) : "nothing";
          for (char& ch : t) if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
          if (note.flicked) {
            char b[48];
            // Screen +y is down: a flick UP is fy < 0.
            const float deg = std::atan2(-note.fy, note.fx) * 57.29578f;
            std::snprintf(b, sizeof b, "  flick %.0f deg", deg);
            t += b;
          } else {
            t += "  no flick (neutral alternate)";
          }
          ui.strikeLastText = std::move(t);
        } else {
          ui.strikeLastAge += dt;
        }
        // ---- THE CHARGED STRIKE (strokes.h StrokeCursor::holdWindup) -------
        // Which sector the HELD strike is (the base style the hold now
        // names, which a re-aim changes), the remembered flick it re-aims to,
        // and how far the re-aim slide has got.
        ui.strikeCharging = playerStrike.Holding();
        ui.strikeCharged = playerStrike.Active() && playerStrike.charged;
        ui.strikeChargeMul = CurrentTuning().melee.chargeDamage;
        ui.strikeMemValid = pk.Remembered(ui.strikeMemX, ui.strikeMemY);
        ui.strikeHeldSector =
            ui.strikeCharged
                ? sectorOf(session.strikeBase,
                           ui.strikeMemValid ? ui.strikeMemX : 1.0f,
                           ui.strikeMemValid ? ui.strikeMemY : 0.0f)
                : -1;
        ui.strikeBlend =
            playerStrike.chargeBlendTicks > 0
                ? std::clamp((float)playerStrike.chargeBlend /
                                 (float)playerStrike.chargeBlendTicks,
                             0.0f, 1.0f)
                : 1.0f;
        ui.strikeNowText.clear();
        if (playerStrike.Active()) {
          if (const AttackStyle* sty = lib.At(playerStrike.style)) {
            const char* ph =
                playerStrike.phase == StrokeCursor::Phase::Windup ? "windup"
              : playerStrike.phase == StrokeCursor::Phase::Cut    ? "CUT"
              : playerStrike.phase == StrokeCursor::Phase::Recover
                  ? (playerStrike.releasing ? "release" : "recover")
                                                                 : "guard";
            const int k = playerStrike.frame;
            const std::string fname =
                k >= 0 && k < (int)sty->frames.size() ? sty->frames[k].name : "";
            char b[160];
            std::snprintf(b, sizeof b, "%s  %s  frame %d/%d \"%s\"%s",
                          baseName(playerStrike.style).c_str(), ph,
                          std::min(k + 1, playerStrike.frames), playerStrike.frames,
                          fname.c_str(), sty->Keyed() ? "  keyed" : "  TIP-DRIVEN");
            ui.strikeNowText = b;
          }
        }
      }
      ui.playerPos[0] = player.pos.x;
      ui.playerPos[1] = player.pos.y;
      ui.playerPos[2] = player.pos.z;

      // crosshair material readout — same sim_pick snapshot the brush, laser
      // and prefab placer read, so the name shown is exactly the cell those
      // tools would act on (one tick latent, like every other pick consumer).
      {
        const auto& psnap = world.Snap();
        if (psnap.valid && psnap.pick[0] != 0) {
          ui.hoverMat = (int)psnap.pick[1];
          ui.hoverCell[0] = (int)psnap.pick[2];
          ui.hoverCell[1] = (int)psnap.pick[3];
          ui.hoverCell[2] = (int)psnap.pick[4];
          float dx = (float)ui.hoverCell[0] + 0.5f - eye.x;
          float dy = (float)ui.hoverCell[1] + 0.5f - eye.y;
          float dz = (float)ui.hoverCell[2] + 0.5f - eye.z;
          ui.hoverDist =
              std::sqrt(dx * dx + dy * dy + dz * dz) * kVoxelMeters;
        } else {
          ui.hoverMat = 0;
        }
      }

      // The portrait camera. Computed here, BEFORE the panel is drawn, because
      // the inspector's limb outlines are projections through this exact
      // camera and the panel draws them in the same frame the pass renders.
      // Cheap and skipped entirely when the screen is closed.
      PortraitCam portraitCam;
      if (ui.inventoryOpen) {
        // WHOSE BOXES. Alive, the live rig's, rebuilt every frame. Dead, the
        // ones the dying observer froze — same type, same code below, so orbit,
        // pan, zoom, limb focus and the outlines all work on a corpse exactly
        // as they do on a living body, and there is no second path to keep in
        // step with this one. The FACING is frozen with them: `avatarHeading`
        // belongs to a rig that is not being driven any more, and a portrait
        // that swung because the player turned the free camera would be turning
        // the picture out from under their own drag.
        LimbBoxes& lb = (deathFrozen && deathBody.have) ? deathBody.boxes
                                                        : liveLimbBoxes;
        if (!(deathFrozen && deathBody.have))
          CollectLimbBoxes(avatar, phys, lb);
        const float bodyYaw =
            (deathFrozen && deathBody.have) ? deathBody.yaw : avatarHeading;
        // THE ORBIT IS RELATIVE TO THE CHARACTER'S OWN FACING, so opening the
        // screen always shows their FRONT and turning the body does not spin
        // the portrait out from under the player's drag.
        //
        // The two conventions differ and the conversion is the whole reason
        // this is a comment: a rig's forward is (sin h, ., cos h) while a
        // Camera's is (cos yaw, ., sin yaw), so a camera LOOKING AT the face
        // needs forward == -rigForward, i.e. yaw = atan2(-cos h, -sin h).
        // Reset: set TARGETS to defaults — the lerp below animates there.
        if (ui.portraitReset) {
          ui.portraitZoomTarget = 1.0f;
          ui.portraitPanXTarget = 0.0f;
          ui.portraitPanYTarget = 0.0f;
          ui.portraitPivotSlot = -1;
          ui.portraitReset = false;
        }
        // Focus on a limb: compute its world-space bounding box and set
        // TARGETS so the portrait smoothly frames the limb.
        if (ui.portraitFocusSlot >= 0) {
          const int slot = ui.portraitFocusSlot;
          ui.portraitFocusSlot = -1;
          if (slot < UIState::kSlotCount && !lb.Empty()) {
            Vec3 bodyLo{1e9f,1e9f,1e9f}, bodyHi{-1e9f,-1e9f,-1e9f};
            Vec3 limbLo{1e9f,1e9f,1e9f}, limbHi{-1e9f,-1e9f,-1e9f};
            bool anyBody = false, anyLimb = false;
            for (const LimbBoxes::Box& box : lb.boxes) {
              const int s = box.slot;
              for (const Vec3& v : box.c) {
                bodyLo = Vec3{std::min(bodyLo.x,v.x),std::min(bodyLo.y,v.y),std::min(bodyLo.z,v.z)};
                bodyHi = Vec3{std::max(bodyHi.x,v.x),std::max(bodyHi.y,v.y),std::max(bodyHi.z,v.z)};
                anyBody = true;
                if (s == slot) {
                  limbLo = Vec3{std::min(limbLo.x,v.x),std::min(limbLo.y,v.y),std::min(limbLo.z,v.z)};
                  limbHi = Vec3{std::max(limbHi.x,v.x),std::max(limbHi.y,v.y),std::max(limbHi.z,v.z)};
                  anyLimb = true;
                }
              }
            }
            if (anyBody && anyLimb) {
              const Vec3 bodyHalf = (bodyHi - bodyLo) * 0.5f;
              const Vec3 limbHalf = (limbHi - limbLo) * 0.5f;
              const float halfH = std::max(0.5f, bodyHalf.y);
              // The orbit now pivots on the limb itself, so pan resets to zero.
              ui.portraitPanXTarget = 0.0f;
              ui.portraitPanYTarget = 0.0f;
              const float limbSpan = std::max({limbHalf.x, limbHalf.y, limbHalf.z, 0.5f});
              ui.portraitZoomTarget = std::clamp(halfH / limbSpan * 0.55f, 1.0f, 6.0f);
            }
          }
        }
        // Smooth lerp: exponential ease toward the target each frame.
        {
          const float lerpDt = dt;
          const float rate = 12.0f;
          const float t = 1.0f - std::exp(-rate * lerpDt);
          auto ease = [t](float& cur, float tgt) {
            if (std::abs(cur - tgt) < 0.001f) cur = tgt;
            else cur += (tgt - cur) * t;
          };
          ease(ui.portraitZoom, ui.portraitZoomTarget);
          ease(ui.portraitPanX, ui.portraitPanXTarget);
          ease(ui.portraitPanY, ui.portraitPanYTarget);
        }
        const float frontYaw =
            std::atan2(-std::cos(bodyYaw), -std::sin(bodyYaw));
        portraitCam = MakePortraitCam(lb, frontYaw + ui.portraitYaw,
                                      ui.portraitPitch,
                                      (float)kPortraitW / (float)kPortraitH,
                                      ui.portraitZoom, ui.portraitPanX,
                                      ui.portraitPanY, ui.portraitPivotSlot);
        ProjectBodyUI(lb, portraitCam, ui);
      }

      // ======================================================================
      // THE CHARACTER SCREEN: consume last frame's intents, then re-mirror.
      // ======================================================================
      //
      // ORDER MATTERS AND IS THE WHOLE CONTRACT. The screen sets a latch; this
      // block executes it against the REAL container and then rebuilds the
      // mirror the screen will read. So the panel's own view of an item is
      // never authoritative and never even one frame stale in a way that could
      // be acted on twice — the latch is cleared here, by its consumer, the
      // same shape every other one-shot in this loop uses.
      // ---- LOOT: the corpse the screen is open on ---------------------------
      //
      // Consumed here, beside the kit latches, because a loot slot is one more
      // address the same drag can name (KitSpace::Loot). The corpse is
      // re-found BY ID on every use — it is a Mob in a vector, and a pointer
      // does not survive anything that spawns or removes one.
      {
        auto say = [&](const std::string& m) {
          ui.kitMessage = m;
          ui.kitMessageAge = 0.0f;
        };
        auto closeLoot = [&]() {
          ui.lootOpen = false;
          ui.lootClose = false;
          lootCorpse = 0;
          ui.lootRef.clear();
          if (lootOpenedScreen && ui.inventoryOpen) {
            ui.inventoryOpen = false;
            captured = captureBeforeUi;
            ui.portraitYaw = 0.0f;  ui.portraitPitch = -0.08f;
            ui.portraitZoom = 1.0f; ui.portraitZoomTarget = 1.0f;
            ui.portraitPanX = 0.0f; ui.portraitPanXTarget = 0.0f;
            ui.portraitPanY = 0.0f; ui.portraitPanYTarget = 0.0f;
            ui.portraitPivotSlot = -1;
            ui.inspectSelected = -1;
            glfwSetInputMode(window, GLFW_CURSOR,
                             captured ? GLFW_CURSOR_DISABLED
                                      : GLFW_CURSOR_NORMAL);
            glfwGetCursorPos(window, &mx0, &my0);
          }
          lootOpenedScreen = false;
        };
        // A CHEST the tick's use verb opened (world/refs_doors.h container):
        // the same panel and the same cursor dance as E on a corpse.
        if (ui.lootRefOpenReq) {
          ui.lootRefOpenReq = false;
          const refs::Ref* cr = ui.lootRef.empty() ? nullptr : refStore.Find(ui.lootRef);
          if (cr != nullptr) {
            lootCorpse = 0;
            ui.lootOpen = true;
            ui.lootTitle = refs::ContainerTitle(*cr);
            if (!ui.inventoryOpen) {
              lootOpenedScreen = true;
              ui.inventoryOpen = true;
              captureBeforeUi = captured;
              captured = false;
              glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
              glfwGetCursorPos(window, &mx0, &my0);
            }
          } else {
            ui.lootRef.clear();
          }
        }
        // The screen closed under the panel (I / Esc): the loot goes with it.
        if (ui.lootOpen && !ui.inventoryOpen) {
          ui.lootOpen = false;
          lootCorpse = 0;
          ui.lootRef.clear();
          lootOpenedScreen = false;
        }
        if (ui.lootClose) closeLoot();
        // ---- THE CHEST BRANCH: take one / take all / put, then skip the
        // corpse code below (it would close a panel with no corpse behind it).
        const bool chestOpen = ui.lootOpen && !ui.lootRef.empty();
        if (chestOpen) {
          const refs::Ref* cr = refStore.Find(ui.lootRef);
          const Vec3 cp = cr ? Vec3{(float)cr->pos.x + 0.5f, (float)cr->pos.y + 0.5f,
                                    (float)cr->pos.z + 0.5f}
                             : Vec3{};
          if (cr == nullptr || (cp - player.pos).len() > kLootRange) {
            closeLoot();
          } else {
            const std::string cid = ui.lootRef;
            auto takeChest = [&](int index) -> bool {
              std::string m;
              const bool ok = refs::ContainerTake(refStore, cid, index, kit, items, &m);
              say(m);
              return ok;
            };
            if (ui.moveItem.pending && ui.moveItem.to.space == KitSpace::Loot &&
                ui.moveItem.from.space != KitSpace::Loot) {
              // PUT: the whole stack from the bag or the hotbar. Not from an
              // equip slot -- a worn piece leaves through Mob::KitMove, which
              // flushes its shells; take it off first.
              ui.moveItem.pending = false;
              if (ui.moveItem.from.space == KitSpace::Equip) {
                say("take it off first");
              } else if (ItemStack* src = kit.Resolve(ui.moveItem.from);
                         src != nullptr && !src->Empty()) {
                ItemStack moving = *src;
                std::string m;
                if (refs::ContainerPut(refStore, cid, moving, &m)) *src = ItemStack{};
                say(m);
              }
            }
            if (ui.moveItem.pending && ui.moveItem.from.space == KitSpace::Loot) {
              ui.moveItem.pending = false;
              takeChest(ui.moveItem.from.index);
            }
            if (ui.equipItem.pending && ui.equipItem.from.space == KitSpace::Loot) {
              ui.equipItem.pending = false;
              takeChest(ui.equipItem.from.index);
            }
            if (ui.takeLoot.pending) {
              ui.takeLoot.pending = false;
              if (ui.takeLoot.all) {
                int took = 0;
                while (took < Bag::kSlots && takeChest(0)) took++;
                if (took > 1) say("took everything that fit");
              } else {
                takeChest(ui.takeLoot.index);
              }
            }
            if (ui.dropItem.pending && ui.dropItem.from.space == KitSpace::Loot) {
              ui.dropItem.pending = false;
              say("take it out of the " + refs::ContainerTitle(*cr) + " first");
            }
          }
        }
        // Walked away, or the heap is gone / picked clean: close by itself.
        // The dead Mob the panel is open on, or null once it is not one any
        // more (released to debris by the dead cap, risen, removed).
        auto lootMob = [&]() -> Mob* {
          Mob* m = lootCorpse ? mobs.FindMobById(lootCorpse) : nullptr;
          return m != nullptr && m->Lootable() ? m : nullptr;
        };
        if (ui.lootOpen && !chestOpen) {
          const Mob* c = lootMob();
          if (!c || !c->AnyLimbWithin(player.pos, kLootRange)) closeLoot();
        }
        auto takeOne = [&](int index, KitRef dest) -> bool {
          Mob* c = lootMob();
          if (!c) return false;
          std::string name;
          const LootResult r =
              TakeCorpseLoot(*c, index, dest, avatar, items, &name);
          if (r == LootResult::Ok) {
            say("took " + name);
            return true;
          }
          say(LootResultText(r, dest));
          return false;
        };
        if (ui.moveItem.pending && ui.lootOpen && !chestOpen &&
            ui.moveItem.to.space == KitSpace::Loot) {
          ui.moveItem.pending = false;
          if (ui.moveItem.from.space != KitSpace::Loot)
            say("drag it out of the screen to put it on the ground");
        }
        if (ui.moveItem.pending && ui.moveItem.from.space == KitSpace::Loot) {
          ui.moveItem.pending = false;
          if (ui.lootOpen) takeOne(ui.moveItem.from.index, ui.moveItem.to);
        }
        if (ui.equipItem.pending && ui.equipItem.from.space == KitSpace::Loot) {
          ui.equipItem.pending = false;   // the panel routes these to takeLoot
          if (ui.lootOpen) takeOne(ui.equipItem.from.index, KitRef{});
        }
        if (ui.takeLoot.pending) {
          ui.takeLoot.pending = false;
          if (ui.lootOpen) {
            if (ui.takeLoot.all) {
              // Front to back until one refuses: a full pack stops the sweep
              // with everything else still on the corpse, which is the only
              // outcome under which nothing is lost.
              int took = 0;
              for (;;) {
                const Mob* c = lootMob();
                std::vector<LootPiece> left;
                if (c) c->LootPieces(left);
                if (left.empty()) break;
                if (!takeOne(0, KitRef{})) break;
                took++;
              }
              if (took > 0) say(took == 1 ? "took one thing" : "took everything that fit");
            } else {
              takeOne(ui.takeLoot.index, KitRef{});
            }
          }
        }
        if (ui.dropItem.pending && ui.dropItem.from.space == KitSpace::Loot) {
          ui.dropItem.pending = false;
          // Dragged OUT of the loot panel: the piece comes off the corpse and
          // stays on the floor as a thing you can pick up (ShedCorpseLoot).
          Mob* c = ui.lootOpen ? lootMob() : nullptr;
          std::string name;
          const int gi = ui.dropItem.from.index;
          std::vector<LootPiece> pieces;
          if (c) c->LootPieces(pieces);
          const bool carried = c && gi >= 0 && gi < (int)pieces.size() &&
                               pieces[(size_t)gi].kind == LootPiece::Kind::Carried;
          if (carried) {
            // A CARRIED STACK HAS NO BODY TO SHED, so this one is a DROP, not
            // a hand-over: ShedCorpseLoot cuts loose a body that is already
            // lying on the corpse, and a pack item never was one. Same spawn
            // the bag's own drop uses, from the corpse rather than from the eye
            // — it should land on the body it came off, not in front of you.
            const LootPiece& pc = pieces[(size_t)gi];
            const ItemDef* idef = items.Of(pc);
            Vec3 at = player.EyePos() + cam.Forward() * 2.0f;
            {
              const Vec3 rp = c->RootWorldPos();
              at = rp + Vec3{0, 2, 0};
            }
            // ONE of it, as the object it was in the pack (its dye, what a
            // vessel held).
            if (idef && DropItemToWorld(*idef, pc.One(), at, Vec3{}, phys,
                                        debris, &mbSet, ground)) {
              name = pc.name;
              // ONE of the stack, the rule the bag's drop states one block
              // down: dropping a count you did not mean to is the mis-click
              // swap-never-overwrite exists to prevent.
              c->TakeLootPiece(gi, nullptr, 1);
              say("left the " + name + " on the ground");
            } else {
              say("there is nowhere to put that");
            }
          } else if (c && ShedCorpseLoot(*c, gi, &name)) {
            say("left the " + name + " on the ground");
          }
        }
      }
      if (ui.moveItem.pending) {
        ui.moveItem.pending = false;
        // Through the WEARER (Mob::KitMove): a piece dragged off the body
        // takes its holes with it, and the rig re-dresses from the result.
        const MoveResult r =
            avatar.KitMove(ui.moveItem.from, ui.moveItem.to, items);
        const char* why = MoveResultText(r, ui.moveItem.to);
        if (why && *why) {
          ui.kitMessage = why;
          ui.kitMessageAge = 0.0f;
        }
        // A move into or out of the HOTBAR can change what is in hand, and the
        // melee path reads Inventory::Selected() straight out of it — so the
        // equip/unequip comparison in the tick loop does the rest by itself on
        // the next tick. Nothing to do here, which is the point of routing the
        // change through the real container rather than around it.
      }
      // ---- RIGHT-CLICK: PUT IT ON, OR TAKE IT OFF -----------------------
      //
      // Consumed here, beside the move latch, and EXECUTED AS A MOVE — so the
      // kind check, the swap-never-overwrite rule and the refusal sentence are
      // the same ones a drag gets, rather than a second path that does almost
      // the same thing. The only thing this adds is CHOOSING the destination,
      // which is the whole gesture.
      //
      // The choice, in order:
      //   * From an equip slot -> the first free bag slot. Taking something off
      //     has one obvious destination and it is not another equip slot.
      //   * Otherwise -> the first slot whose authored `accepts` takes this
      //     kind and is EMPTY; failing that, the first that takes it at all,
      //     which swaps. Preferring the empty one is what makes right-clicking
      //     two rings put them on two fingers instead of one finger twice.
      if (ui.equipItem.pending) {
        ui.equipItem.pending = false;
        const ItemStack* src = kit.Resolve(ui.equipItem.from);
        const ItemDef* def = src ? items.Of(*src) : nullptr;
        KitRef to{};
        bool have = false;
        if (!def) {
          ui.kitMessage = "nothing to equip";
          ui.kitMessageAge = 0.0f;
        } else if (ui.equipItem.from.space == KitSpace::Equip) {
          const int free = kit.bag.FirstFree();
          if (free >= 0) {
            to = KitRef{KitSpace::Bag, free};
            have = true;
          } else {
            ui.kitMessage = "your pack is full";
            ui.kitMessageAge = 0.0f;
          }
        } else {
          const int first = EquipSlotFor(def->kind, kit.equip);
          if (first >= 0) {
            to = KitRef{KitSpace::Equip, first};
            have = true;
          } else {
            ui.kitMessage = "there is nowhere on you that takes that";
            ui.kitMessageAge = 0.0f;
          }
        }
        if (have) {
          const MoveResult r = avatar.KitMove(ui.equipItem.from, to, items);
          const char* why = MoveResultText(r, to);
          if (why && *why) {
            ui.kitMessage = why;
            ui.kitMessageAge = 0.0f;
          }
        }
      }
      // ---- DROP: the drag that landed on nothing ------------------------
      //
      // Consumed at the SAME point in the frame as the move latch, and for the
      // same reason: the panel's view of a slot is never authoritative, so the
      // intent is executed against the real container here and the mirror is
      // rebuilt below.
      // ---- THE ALCHEMY BENCH (game/alchemy_bench.h) ----------------------
      //
      // Opened by a double-click on a vessel slot; closed by "done", Esc, or
      // the screen shutting. Closing is the ONE place the bench's tally
      // reaches the kit: ValidateBench proves every material conserved, each
      // vessel must still be exactly what came on to the bench (a stack that
      // moved, emptied or was swapped while the panel was up voids the
      // session rather than being written over), and what was spilled goes
      // into the world at your feet through the vessel-spill queue.
      // WHERE THE HANDS ARE (world voxels): between the lips of the vessels
      // the character holds for the bench, else a little in front of the
      // chest. The bench's explosions go off here, its broken glass spills
      // here.
      auto benchHands = [&]() -> Vec3 {
        const Vec3 fwd = cam.Forward();
        Vec3 at = player.EyePos() + fwd * MetresToCells(0.4f) - Vec3{0, MetresToCells(0.45f), 0};
        if (!avatar.Spawned()) return at;
        Vec3 sum{};
        int n = 0;
        for (int hk = 0; hk < kHands; hk++) {
          if (session.benchHold.hand[hk].item.empty()) continue;
          const float ang = session.benchHold.hand[hk].angle;
          const Quat yaw = QuatAxisAngle({0, 1, 0}, avatar.Heading());
          const Vec3 axisW = QuatRotate(yaw, Vec3{-std::sin(ang), std::cos(ang), 0.0f});
          Vec3 lip;
          if (avatar.HeldMouthWorld(axisW, lip, HandAt(hk))) { sum = sum + lip; n++; }
        }
        return n ? sum * (1.0f / (float)n) : at;
      };
      // Returns false when the session was VOIDED (nothing written back):
      // the caller must then not apply anything else the session produced
      // (an explosion at the hands), or the reaction would go off AND the
      // vessels come back full to do it again.
      auto finishBench = [&](bool abrupt = false, bool breakHeld = false) -> bool {
        alchemy::BenchResult r = bench.Finish(abrupt);
        ui.alchemy.open = false;
        ui.alchemy.texReady = false;
        ui.alchemy.rows.clear();
        // What already fell into the world while the bench was up (streamed
        // off the table, vented out of a mouth) is gone whatever happens
        // here: a voided session takes it out of the vessels that are still
        // what they were, so voiding is never a way to pour a flask out and
        // keep it full. Both void paths below owe it.
        auto takeStreamed = [&]() {
          for (size_t m = 0; m < r.streamed.size(); m++) {
            uint32_t owe = r.streamed[m];
            for (const alchemy::BenchEntry& o : r.vessels) {
              if (!owe) break;
              ItemStack* os = kit.Resolve(o.ref);
              if (os && os->name == o.item && os->count == 1 && os->contents.SameAs(o.before))
                owe -= os->contents.Take((uint16_t)m, owe);
            }
          }
        };
        std::string why;
        if (!alchemy::ValidateBench(r, mats, why)) {
          std::fprintf(stderr, "alchemy bench: %s -- nothing changed\n", why.c_str());
          takeStreamed();
          ui.kitMessage = "the bench lost track of something; nothing changed";
          ui.kitMessageAge = 0.0f;
          return false;
        }
        for (const alchemy::BenchEntry& e : r.vessels) {
          const ItemStack* st = kit.Resolve(e.ref);
          if (!st || st->name != e.item || st->count != 1 || !st->contents.SameAs(e.before)) {
            takeStreamed();
            ui.kitMessage = "something moved while you worked; nothing changed";
            ui.kitMessageAge = 0.0f;
            return false;
          }
        }
        // THE GLASS THAT BROKE: a vessel whose pressure burst it on the bench,
        // and -- when an event blew up in the character's hands -- the ones
        // they were holding. The item is gone; what was in it BURSTS out at
        // the hands (a break's spill, not a pour).
        for (const alchemy::BenchEntry& e : r.vessels) {
          ItemStack* st = kit.Resolve(e.ref);
          if (!st) continue;
          st->contents = e.after;
          st->stoppered = e.stoppered;
          const bool breaks = e.broken || (breakHeld && e.onTable);
          if (!breaks) continue;
          if (!st->contents.Empty()) {
            ContainerSpill sp;
            sp.at = benchHands();
            sp.vel = player.vel;
            sp.away = Vec3{0, 1, 0};
            sp.rest = st->contents;
            sp.seed = rng::Hash3(0xB0B5Eu, (uint32_t)frameCounter, st->contents.Total());
            tickCtx.vesselSpills.push_back(sp);
          }
          avatar.KitTake(e.ref, 1);
          if (ItemStack* left = kit.Resolve(e.ref); left && !left->Empty()) {
            left->ClearFill();
            left->stoppered = false;
          }
        }
        if (!r.spilled.Empty()) {
          ContainerSpill sp;
          const Vec3 fwd = cam.Forward();
          sp.at = player.EyePos() + fwd * MetresToCells(0.35f) -
                  Vec3{0, MetresToCells(0.6f), 0};
          sp.vel = player.vel;
          sp.away = Vec3{0, 1, 0};
          sp.rest = r.spilled;
          sp.seed = rng::Hash3(0xA1C4E3u, (uint32_t)frameCounter, r.spilled.Total());
          if (r.ejected) sp.at = benchHands();
          tickCtx.vesselSpills.push_back(sp);
          if (!r.ejected) {
            ui.kitMessage = "some of it spilled at your feet";
            ui.kitMessageAge = 0.0f;
          }
        }
        return true;
      };
      // Put a vessel on the bench (opening the bench if it is not up yet, sized
      // to the room the panel measured last) or take it off.
      auto benchPlace = [&](KitRef ref) {
        ItemStack* st = kit.Resolve(ref);
        const ItemDef* d = st ? items.Of(*st) : nullptr;
        if (!d || !d->IsContainer()) return;
        if (st->count != 1) {
          ui.alchemy.message = "take one out of the stack to put it on the bench";
          return;
        }
        if (!bench.IsOpen()) {
          const int sc = std::max(1, ui.alchemy.scale);
          const float aw = ui.alchemy.areaW > 0 ? ui.alchemy.areaW : 1000.0f;
          const float ah = ui.alchemy.areaH > 0 ? ui.alchemy.areaH : 780.0f;
          // THE GRID RUNS ABOVE THE BOX: up to the top of the screen, and
          // never less than the lift room (Open enforces kLiftH), so a flask
          // can be held upside down over the other; the panel draws the part
          // above its box over its header.
          const float rh = std::max(ah, ui.alchemy.roomH);
          bench.Open((int)(aw / sc), (int)(rh / sc), mats, reactions, benchSolutes);
          ui.alchemy.open = true;
          ui.alchemy.texReady = false;
          ui.alchemy.tableW = bench.W();
          ui.alchemy.gridH = bench.H();
          ui.alchemy.tableH = std::min(bench.H(), (int)(ah / sc));
          ui.alchemy.message.clear();
          ui.alchemy.tool = 0;
        }
        if (!bench.Place(ref, *d, *st))
          ui.alchemy.message = bench.Refusal().empty() ? "that will not go on the bench" : bench.Refusal();
        else
          ui.alchemy.message.clear();
      };
      if (ui.alchemy.wantOpen) {
        ui.alchemy.wantOpen = false;
        if (!bench.OnTable(ui.alchemy.openRef)) benchPlace(ui.alchemy.openRef);
      }
      if (ui.alchemy.wantToggle) {
        ui.alchemy.wantToggle = false;
        if (bench.OnTable(ui.alchemy.toggleRef)) bench.Remove(ui.alchemy.toggleRef);
        else benchPlace(ui.alchemy.toggleRef);
      }
      // ---- THE BENCH'S EVENTS (alchemy_bench.h: the registry by effect kind) --
      // Each goes through its handler into one BenchOutcome, applied here
      // generically: a message, an EJECT (the bench shuts mid-motion, the
      // vessels in hand break), real EXPLOSIONS at the hands through the
      // grenade slot (PlayerSession::pendingBlasts) and a PUFF of the
      // reaction's gases round them on the CPU gas stream. BEFORE the close
      // below, so an event raised in the frame the bench is closed is not
      // dropped with the session.
      if (bench.IsOpen()) {
        alchemy::BenchOutcome out;
        bool any = false;
        for (const alchemy::BenchEvent& ev : bench.TakeEvents()) {
          any |= alchemy::DispatchBenchEvent(ev, out);
          if (std::getenv("SANDVOX_BENCH_DEBUG"))
            std::printf("bench event: %s x%d (entry %d)\n", ev.kind.c_str(), ev.count, ev.entry);
        }
        if (any) {
          if (!out.message.empty()) {
            ui.alchemy.message = out.message;
            ui.kitMessage = out.message;
            ui.kitMessageAge = 0.0f;
          }
          const Vec3 hands = benchHands();
          std::vector<ExplosionOp> exps;
          std::vector<GasSpawnOp> gas;
          alchemy::BenchOutcomeWorldOps(out, hands, rng::Hash3(0xB1A57u, (uint32_t)frameCounter, 1u), mats,
                                        exps, gas);
          for (int be : out.burst) bench.BurstEntry(be);
          // An EJECT closes the session first, and its blast and puff go off
          // only if the session was written back (the held vessels broke). A
          // VOIDED session put the vessels back as they came -- sodium and
          // water still in them -- so blasting too would let the same
          // reaction explode again the next time they went on the bench.
          bool apply = true;
          if (out.eject) {
            apply = finishBench(true, out.breakHeld);
            ui.inventoryOpen = false;   // thrown out of the bench AND the screen
          }
          if (apply) {
            for (const ExplosionOp& e : exps) session.pendingBlasts.push_back(e);
            if (!gas.empty()) world.QueueGasSpawns(gas.data(), (uint32_t)gas.size());
          }
        }
      }
      if (ui.alchemy.wantClose || (bench.IsOpen() && !ui.inventoryOpen)) {
        ui.alchemy.wantClose = false;
        if (bench.IsOpen()) finishBench();
      }
      // ---- THE BENCH IN THE CHARACTER'S HANDS (session.h BenchHold) --------
      // One vessel on the table: held in one hand. Two or more: the one in
      // the bench's hand (or the one last touched) and its nearest
      // neighbour, one per hand -- the bench is the portrait's mirror image,
      // so the one on the bench's LEFT is in the character's RIGHT hand. The
      // hands are kept while the pair is the same pair, so carrying one flask
      // across the other does not swap them between the fists.
      {
        static KitRef benchInHand[kHands] = {};
        PlayerSession::BenchHold bh{};
        std::vector<alchemy::BenchVesselView> views;
        if (bench.IsOpen()) views = bench.Vessels();
        if (!views.empty()) {
          int a = -1, b = -1;
          for (size_t i = 0; i < views.size(); i++)
            if (views[i].held) a = (int)i;
          if (a < 0) {
            const KitRef fr = bench.Focus();
            for (size_t i = 0; i < views.size(); i++)
              if (views[i].ref == fr) a = (int)i;
          }
          if (a < 0) a = (int)views.size() - 1;
          for (size_t i = 0; i < views.size(); i++) {
            if ((int)i == a) continue;
            if (b < 0 || std::fabs(views[i].pose.pos.x - views[a].pose.pos.x) <
                             std::fabs(views[b].pose.pos.x - views[a].pose.pos.x))
              b = (int)i;
          }
          auto isIn = [&](int v) {
            return v >= 0 && (views[v].ref == benchInHand[0] || views[v].ref == benchInHand[1]);
          };
          const int want = b >= 0 ? 2 : 1;
          const int have = (benchInHand[0].Valid() ? 1 : 0) + (benchInHand[1].Valid() ? 1 : 0);
          if (have != want || !isIn(a) || (b >= 0 && !isIn(b))) {
            benchInHand[0] = benchInHand[1] = KitRef{};
            if (b < 0) {
              // The hand that can GRIP it (session.cpp's bench equip refuses
              // a hand below melee.injuredArmDrop).
              const int hk = avatar.HandCondition(Hand::Right) >=
                                     std::max(CurrentTuning().melee.injuredArmDrop, 1e-6f)
                                 ? 0
                                 : 1;
              benchInHand[hk] = views[a].ref;
            } else {
              const bool aLeft = views[a].pose.pos.x < views[b].pose.pos.x;
              benchInHand[HandIndex(Hand::Right)] = views[aLeft ? a : b].ref;
              benchInHand[HandIndex(Hand::Left)] = views[aLeft ? b : a].ref;
            }
          }
          for (int hk = 0; hk < kHands; hk++) {
            for (const alchemy::BenchVesselView& v : views) {
              if (!benchInHand[hk].Valid() || !(v.ref == benchInHand[hk])) continue;
              const ItemStack* st = kit.Resolve(v.ref);
              const ItemDef* d = st ? items.Of(*st) : nullptr;
              if (!d) continue;
              PlayerSession::BenchHold::HandView& hv = bh.hand[hk];
              hv.item = d->name;
              if (!bench.Contents(v.ref, hv.contents)) hv.contents = st->contents;
              hv.angle = v.pose.angle;
              const float ca = v.pose.angle, hh = v.height * 0.5f;
              hv.cx = v.pose.pos.x - std::sin(ca) * hh;
              hv.cy = v.pose.pos.y + std::cos(ca) * hh;
              hv.height = v.height;
              hv.carried = v.held;
              hv.stoppered = v.stoppered;
            }
          }
          bh.active = !bh.hand[0].item.empty() || !bh.hand[1].item.empty();
        } else {
          benchInHand[0] = benchInHand[1] = KitRef{};
        }
        session.benchHold = bh;

        // ---- WHAT FALLS OFF THE TABLE FALLS INTO THE WORLD, NOW -----------
        // Whole eighths as the bench drains them (AlchemyBench::TakeSpill),
        // poured -- not burst -- from the lip of the flask in the hand that
        // holds the vessel it LEFT (one spill per source vessel, so two
        // flasks spilling at once each pour from their own), or at your feet
        // for one not in a hand. Only matter that left no known vessel falls
        // back to the vessel nearest where it fell off the table.
        std::vector<alchemy::BenchSpill> fellBy;
        float exitX = -1.0f;
        if (bench.IsOpen() && bench.TakeSpill(fellBy, exitX)) {
          for (const alchemy::BenchSpill& bs : fellBy) {
            const alchemy::Composition& fell = bs.what;
            ContainerSpill sp;
            const Vec3 fwd = cam.Forward();
            sp.at = player.EyePos() + fwd * MetresToCells(0.35f) -
                    Vec3{0, MetresToCells(0.6f), 0};
            int nearest = -1;
            if (bs.ref.Valid()) {
              for (size_t i = 0; i < views.size(); i++)
                if (views[i].ref == bs.ref) nearest = (int)i;
            } else if (exitX >= 0) {
              for (size_t i = 0; i < views.size(); i++)
                if (nearest < 0 || std::fabs(views[i].pose.pos.x - exitX) <
                                    std::fabs(views[nearest].pose.pos.x - exitX))
                  nearest = (int)i;
            }
            if (nearest >= 0 && avatar.Spawned()) {
              for (int hk = 0; hk < kHands; hk++) {
                if (!(views[nearest].ref == benchInHand[hk])) continue;
                const Hand h = HandAt(hk);
                const float ang = views[nearest].pose.angle;
                const Quat yaw = QuatAxisAngle({0, 1, 0}, avatar.Heading());
                const Vec3 axisW = QuatRotate(yaw, Vec3{-std::sin(ang), std::cos(ang), 0.0f});
                Vec3 lip;
                if (avatar.HeldMouthWorld(axisW, lip, h)) sp.at = lip;
              }
            }
            sp.vel = player.vel;
            sp.pour = true;
            sp.splatted = true;
            sp.rest = fell;
            sp.seed = rng::Hash3(0xA1C4E5u, (uint32_t)frameCounter,
                                 fell.Total() + 131u * (uint32_t)(&bs - fellBy.data()));
            tickCtx.vesselSpills.push_back(sp);
            if (std::getenv("SANDVOX_BENCH_DEBUG"))
              std::printf("bench spill: frame %llu, %u eighths off the table at x %.0f, from %s (view %d)\n",
                          (unsigned long long)frameCounter, fell.Total(), exitX,
                          bs.ref.Valid() ? "its vessel" : "no known vessel", nearest);
          }
        }
      }

      if (ui.dropItem.pending) {
        ui.dropItem.pending = false;
        ItemStack* src = kit.Resolve(ui.dropItem.from);
        const ItemDef* def = src ? items.Of(*src) : nullptr;
        if (def) {
          // Thrown gently forward from the eye, so it lands in front of you
          // rather than inside your own capsule.
          const Vec3 at = player.EyePos() + cam.Forward() * 2.0f;
          const Vec3 vel = cam.Forward() * 4.0f + player.vel;
          // A piece dropped straight off the body goes down with its holes.
          if (ui.dropItem.from.space == KitSpace::Equip)
            avatar.KitFlushWorn(ui.dropItem.from.index);
          if (DropItemToWorld(*def, src->One(), at, vel, phys, debris, &mbSet,
                              ground)) {
            // ONE of the stack. Dropping a count you did not mean to is the
            // mis-click this system's swap-never-overwrite rule exists to
            // prevent, and it applies here too. A fill is ONE vessel's and it
            // went with the dropped one (a filled stack of more than one is
            // only ever an old save's): what stays is empty, not a copy.
            avatar.KitTake(ui.dropItem.from, 1);
            if (!src->Empty()) src->ClearFill();
            ui.kitMessage = "dropped";
          } else {
            ui.kitMessage = "there is nowhere to put that";
          }
          ui.kitMessageAge = 0.0f;
        }
      }
      if (ui.castAtPart.pending) {
        ui.castAtPart.pending = false;
        castAtPartQueued = ui.castAtPart.slot;
      }
      // THE POUR BRUSH: the cursor on the portrait becomes a world ray through
      // the SAME camera that rendered it -- the inverse of ProjectToPortrait,
      // i.e. raymarch.wgsl's primary ray -- so the cell the ring sits on is
      // the cell that was drawn under the cursor. The pick runs every frame
      // the cursor is over the body, for the ring; the tick re-picks with the
      // pose it pours on (MobSystem::PourOnBody).
      {
        PlayerSession::PourStroke& ps = session.pourStroke;
        ps.active = false;
        ui.pourCursorValid = false;
        if (ui.inventoryOpen && ui.pourBrush.hover && portraitCam.valid &&
            avatar.Spawned()) {
          const PortraitCam& pc = portraitCam;
          const float ndcX = ui.pourBrush.uv[0] * 2.0f - 1.0f;
          const float ndcY = 1.0f - ui.pourBrush.uv[1] * 2.0f;
          const Vec3 rd = (pc.cam.Forward() +
                           pc.cam.Right() * (ndcX * pc.tanHalf * pc.aspect) +
                           pc.cam.Up() * (ndcY * pc.tanHalf))
                              .normalized();
          const MobSystem::BodyRayHit hit =
              mobs.PickBody(avatar.Id(), pc.eye, rd, 4096.0f);
          float uv[2], edge[2];
          if (hit.hit && ProjectToPortrait(pc, hit.pos, uv) &&
              ProjectToPortrait(pc, hit.pos + pc.cam.Right() * ui.pourRadius, edge)) {
            ui.pourCursorValid = true;
            ui.pourCursorUV[0] = uv[0];
            ui.pourCursorUV[1] = uv[1];
            ui.pourCursorR = std::fabs(edge[0] - uv[0]);
          }
          ps.active = ui.pourBrush.active;
          ps.vessel = ui.activeVessel;
          ps.ro = pc.eye;
          ps.rd = rd;
          ps.radius = ui.pourRadius;
        }
        ui.pourBrush.hover = ui.pourBrush.active = false;
      }
      if (ui.bindGlyph.pending) {
        ui.bindGlyph.pending = false;
        // BY NAME. The panel never handles a glyph index, so a bind cannot
        // survive into a reload as a stale index (see the R-reload block).
        if (ui.bindGlyph.page) {
          GrimoirePage scratch;
          if (!FindPage(glyphs, caster.grimoire, ui.bindGlyph.glyphId, scratch) ||
              !caster.inventory.BindPage(ui.bindGlyph.slot, ui.bindGlyph.glyphId)) {
            ui.kitMessage = "no such page";
            ui.kitMessageAge = 0.0f;
          }
        } else {
          const int gi = ui.bindGlyph.glyphId.empty()
                             ? -1
                             : glyphs.Find(ui.bindGlyph.glyphId);
          if (!caster.inventory.Bind(ui.bindGlyph.slot, gi)) {
            ui.kitMessage = "you do not know that glyph";
            ui.kitMessageAge = 0.0f;
          }
        }
      }
      if (ui.moveBind.pending) {
        ui.moveBind.pending = false;
        // A key dragged onto a key: the two EXCHANGE what they hold (an empty
        // destination makes that a plain move). Read both ends out first —
        // PageAt returns a reference INTO the array the binds are about to
        // overwrite, so a copy is not optional here.
        const int a = ui.moveBind.from, b = ui.moveBind.to;
        if (a >= 0 && a < kGlyphSlots && b >= 0 && b < kGlyphSlots && a != b) {
          const int ga = caster.inventory.At(a), gb = caster.inventory.At(b);
          const std::string pa = caster.inventory.PageAt(a), pb = caster.inventory.PageAt(b);
          auto put = [&](int slot, int gi, const std::string& page) {
            if (!page.empty()) caster.inventory.BindPage(slot, page);
            else caster.inventory.Bind(slot, gi);
          };
          put(b, ga, pa);
          put(a, gb, pb);
        }
      }
      // READY A PAGE (UIState::armPage): the spellbook's click. An empty name
      // is the blank leaf, which puts the readied spell away.
      if (ui.armPage.pending) {
        ui.armPage.pending = false;
        if (ui.armPage.name.empty()) caster.Clear(glyphs);
        else caster.ArmPage(glyphs, ui.armPage.name);
      }
      // A save or delete can change what the readied page says: speak it again
      // from the grimoire afterwards (ArmPage clears it if the page is gone).
      const bool rearmAfterOp = ui.grimoireOp.pending && !caster.armedPage.empty();
      if (ui.grimoireOp.pending) {
        ui.grimoireOp.pending = false;
        const UIState::GrimoireIntent& op = ui.grimoireOp;
        auto say = [&](const std::string& m) {
          ui.kitMessage = m;
          ui.kitMessageAge = 0.0f;
        };
        if (op.op == UIState::GrimoireIntent::Delete) {
          const int pi = caster.grimoire.Find(op.name);
          if (pi >= 0) {
            caster.grimoire.pages.erase(caster.grimoire.pages.begin() + pi);
            // Slots holding the page keep its name: they speak nothing and the
            // strip shows ?, which is the by-name contract (DESIGN §8b).
            say("deleted " + op.name);
            if (ui.grimoireSelected == op.name) {
              ui.grimoireSelected.clear();
              ui.grimoireEditWords.clear();
              ui.grimoireEditName.clear();
              ui.grimoireEditDirty = false;
            }
          } else {
            say("that page is not yours to delete");
          }
        } else if (op.op == UIState::GrimoireIntent::Duplicate) {
          GrimoirePage scratch;
          const GrimoirePage* src = FindPage(glyphs, caster.grimoire, op.name, scratch);
          if (!src) {
            say("no such page");
          } else if ((int)caster.grimoire.pages.size() >= glyphs.budgets.maxGrimoirePages) {
            say("the grimoire is full");
          } else {
            GrimoirePage copy;
            copy.words = src->words;
            copy.name = op.name + "-copy";
            for (int k = 2; caster.grimoire.Find(copy.name) >= 0 && k < 100; k++)
              copy.name = op.name + "-copy" + std::to_string(k);
            caster.grimoire.pages.push_back(copy);
            ui.grimoireSelected = copy.name;
            ui.grimoireEditName = copy.name;
            ui.grimoireEditWords = copy.words;
            ui.grimoireEditDirty = false;
            say("copied to " + copy.name);
          }
        } else {
          // SAVE: a name, a bounded word list, no cycle. A starter's name
          // cannot be taken (it would shadow the read-only page).
          std::string name = op.name;
          while (!name.empty() && name.back() == ' ') name.pop_back();
          std::vector<std::string> words = op.words;
          if ((int)words.size() > glyphs.budgets.maxMacroWords)
            words.resize(glyphs.budgets.maxMacroWords);
          bool starter = false;
          for (const ConjoinedGlyph& cg : glyphs.conjoined) starter = starter || cg.id == name;
          std::string why;
          if (name.empty()) {
            say("a page needs a name");
          } else if (!GrimoireNameUsable(glyphs, name, why)) {
            say(why);
          } else if (starter) {
            say("that name belongs to the library");
          } else if (GrimoireWouldCycle(glyphs, caster.grimoire, name, words, why)) {
            say("refused: " + why);
          } else {
            int pi = caster.grimoire.Find(name);
            // Renaming the selected page: the old entry goes, slots bound to
            // the old name follow it.
            if (pi < 0 && !ui.grimoireSelected.empty() && ui.grimoireSelected != name) {
              const int old = caster.grimoire.Find(ui.grimoireSelected);
              if (old >= 0) {
                caster.grimoire.pages[old].name = name;
                for (int i = 0; i < kGlyphSlots; i++)
                  if (caster.inventory.PageAt(i) == ui.grimoireSelected)
                    caster.inventory.BindPage(i, name);
                pi = old;
              }
            }
            if (pi < 0 && (int)caster.grimoire.pages.size() >= glyphs.budgets.maxGrimoirePages) {
              say("the grimoire is full");
            } else {
              if (pi < 0) {
                caster.grimoire.pages.push_back(GrimoirePage{name, words, false});
              } else {
                caster.grimoire.pages[pi].words = words;
              }
              ui.grimoireSelected = name;
              ui.grimoireEditName = name;
              ui.grimoireEditDirty = false;
              say("saved " + name);
            }
          }
        }
      }
      // ---- A GESTURE ON THE CANVAS (docs/PLAN_spell_graph.md §5) -------------
      //
      // The page never edits words. It names a TREE OP; this applies it to the
      // tree of the composed words, and on success writes the LINEARIZED result
      // back into the row. Every op in game/spellgraph.h is total — it returns
      // either the new word list or a reason — so a refusal is a sentence on the
      // status line and never a malformed page.
      //
      // The tree is built from the EXPANSION, so a nested page is edited as its
      // words. That is the v1 compromise §5 names, and the composer says so.
      if (rearmAfterOp) {
        const std::string page = caster.armedPage;
        caster.ArmPage(glyphs, page);
      }
      if (ui.graphEdit.pending) {
        ui.graphEdit.pending = false;
        const UIState::GraphEditIntent op = ui.graphEdit;
        auto say = [&](const std::string& m) {
          ui.kitMessage = m;
          ui.kitMessageAge = 0.0f;
        };
        const GrimoireExpansion gex = ExpandWords(glyphs, caster.grimoire,
                                                  ui.grimoireEditWords, kSpellStackMax);
        SpellStack gst = StackOf(gex);
        SpellTree gtree = ParseSpell(glyphs, gst);
        if (gtree.Empty()) gtree = EmptyTree();
        EditResult r;
        // A PAGE DROPPED ON THE TREE IS ITS WORDS, one InsertItem each, the
        // tree re-parsed between: the tree has no node kind for "a page", and
        // refusing the gesture outright would make the page list useless up
        // here. The first refusal stops the run and keeps what landed.
        std::vector<int> insert;
        // A PAGE dropped on the tree lands as the SUBTREE its words parse to,
        // magnitudes and timing included (`InsertWords`), not one word at a
        // time - which lost both, and turned `fire projectile` into `fire`
        // beside an empty bolt.
        std::vector<std::string> pageWords;
        if ((op.op == UIState::GraphEditIntent::Insert ||
             op.op == UIState::GraphEditIntent::AttachMod) &&
            glyphs.FindWord(op.glyphId) < 0 && !op.glyphId.empty()) {
          const GrimoireExpansion pe =
              ExpandWords(glyphs, caster.grimoire, {op.glyphId}, kSpellStackMax);
          pageWords = ExpansionWords(glyphs, pe);
          if (pageWords.empty()) say("[" + op.glyphId + "] says nothing");
        } else if (!op.glyphId.empty()) {
          insert.push_back(glyphs.Find(op.glyphId));
        }
        const int gi = insert.empty() ? -1 : insert[0];
        switch (op.op) {
          case UIState::GraphEditIntent::Insert:
          case UIState::GraphEditIntent::AttachMod: {
            if (!pageWords.empty()) {
              r = InsertWords(glyphs, gtree, op.treeNode, op.lane, pageWords);
              break;
            }
            SpellTree cur = gtree;
            for (size_t k = 0; k < insert.size(); k++) {
              r = op.op == UIState::GraphEditIntent::AttachMod
                      ? AttachMod(glyphs, cur, op.treeNode, op.lane, insert[k])
                      : InsertItem(glyphs, cur, op.treeNode, op.lane, insert[k]);
              if (!r.ok) {
                if (k > 0) r.ok = true;   // some of it landed; keep that
                break;
              }
              // RE-PARSE BETWEEN WORDS, because the node indices the next
              // InsertItem needs are the new tree's, not the old one's. The box
              // is found again by its position in the same place: the root.
              if (k + 1 < insert.size()) {
                cur = ParseWords(glyphs, r.words);
                if (cur.Empty()) break;
              }
            }
            break;
          }
          case UIState::GraphEditIntent::FillSlot:
            r = FillSlot(glyphs, gtree, op.treeNode,
                         op.side ? SlotSide::Right : SlotSide::Left, gi);
            break;
          case UIState::GraphEditIntent::Wrap:
            r = WrapInBox(glyphs, gtree, op.treeNode, gi);
            break;
          case UIState::GraphEditIntent::Unbox:
            r = Unbox(glyphs, gtree, op.treeNode);
            break;
          case UIState::GraphEditIntent::Remove:
            r = Remove(glyphs, gtree, op.treeNode);
            break;
          case UIState::GraphEditIntent::Move:
            r = Move(glyphs, gtree, op.treeNode, op.boxTreeNode, op.lane, op.copy);
            break;
          case UIState::GraphEditIntent::CloseLane:
            r = CloseLane(glyphs, gtree, op.treeNode, op.lane);
            break;
          case UIState::GraphEditIntent::SetMagnitude:
            r = SetMagnitude(glyphs, gtree, op.treeNode, op.mag);
            break;
          case UIState::GraphEditIntent::SetTiming: {
            SpellTiming tm;
            tm.trigger = (SpellTrigger)std::clamp(op.trigger, 0, kSpellTriggerCount - 1);
            tm.every = op.every;
            tm.delay = op.delay;
            r = SetTiming(glyphs, gtree, op.treeNode, tm);
            break;
          }
        }
        if (r.ok) {
          if ((int)r.words.size() > glyphs.budgets.maxMacroWords) {
            say("that would not fit on the page");
            if (!ui.grimoireUndo.empty()) ui.grimoireUndo.pop_back();
          } else {
            ui.grimoireEditWords = r.words;
            ui.grimoireEditDirty = true;
          }
        } else {
          say(r.why.empty() ? std::string("that cannot be said") : r.why);
          // The panel pushed an undo before it latched; a refusal changed
          // nothing, so the stack must not grow an identical entry.
          if (!ui.grimoireUndo.empty()) ui.grimoireUndo.pop_back();
        }
      }
      ui.kitMessageAge += dt;

      // ---- the mirrors -------------------------------------------------------
      // Rebuilt from the real containers every frame the character screen is
      // OPEN (the hotbar strip every frame, the HUD draws it) — that is the
      // reason the panel can never show something the game does not have.
      // Shut, nothing reads them, and the spell readouts and glyph catalogue
      // they carry were most of a frame's string work (P6.2).
      {
        // CONDITION COMES FROM WHEREVER THE PIECE ACTUALLY IS. On the body the
        // shells are the truth and the stack's `damage` is stale (it is only
        // written when a piece comes OFF, Mob::KitFlushWorn); in the pack there
        // are no shells and the stack's own damage is all there is. Asking the
        // wrong one is not a rounding error — it is a robe that reads 100%
        // while it burns off your back. PER STACK now (W2-M): two robes, two
        // conditions.
        const float ruinedAt = CurrentTuning().gear.ruinedCondition;
        auto conditionOf = [&](const ItemStack& st, int equipSlot) {
          if (equipSlot >= 0 && EquipSlotIsWorn(equipSlot) && avatar.Spawned() &&
              avatar.WornItem(equipSlot) == st.name)
            return avatar.WornCondition(equipSlot);
          return st.damage.Condition();
        };
        auto mirror = [&](const ItemStack& st, int equipSlot = -1) {
          UIState::KitSlotUI u;
          const ItemDef* d = items.Of(st);
          if (!d) return u;
          u.name = d->name;
          u.count = st.count;
          u.wearable = ItemKindIsWorn(d->kind);
          if (u.wearable) {
            u.condition = conditionOf(st, equipSlot);
            u.ruined = GearRuined(u.condition, ruinedAt);
          }
          // THE KIND, AS THE ONE NAME THE FILE FORMAT ALREADY USES. It was
          // "melee" or the empty string, which meant every worn piece told the
          // panel nothing about itself — and the panel now needs it twice: to
          // pick the item's icon, and to decide which equip slot lights up
          // under a drag. ItemKindName is the same table items.json parses
          // through, so there is still exactly one spelling of "armor_legs".
          u.kind = d->kind == ItemKind::None ? "" : ItemKindName(d->kind);
          // THE DYE, converted to what the panel draws with. ImGui packs its
          // u32 colours 0xAABBGGRR and the dye word is 0x_1_BBGGRR (dye.h), so
          // the low 24 bits transfer verbatim and only the alpha is added —
          // which is the second reason the packing order is the one it is
          // (the first being that unpackColor reads it directly on the GPU).
          if (DyeSet(st.dye)) {
            u.dyeSwatch = 0xFF000000u | (st.dye & 0x00FFFFFFu);
            u.dyeName = DyeName(st.dye);
          }
          // Whatever the def actually carries — no invented stats. A melee
          // item has damage and reach; something with neither says nothing
          // rather than saying "0".
          char tip[192];
          if (d->kind == ItemKind::Melee) {
            std::snprintf(tip, sizeof tip,
                          "%.0f damage at full speed\n%.1f voxel reach%s",
                          d->strike.cut, d->reach,
                          d->hasEdge ? "\ncuts along its own edge" : "");
            u.tip = tip;
          } else if (d->IsContainer()) {
            // What is in it, and the two buttons -- the vessel has no other
            // stat, and nothing else on screen says how to use one.
            u.fill = d->container.capacity > 0
                         ? std::clamp((float)st.FillTotal() / (float)d->container.capacity,
                                      0.0f, 1.0f)
                         : 0.0f;
            u.fillSwatch = ContainerFillSwatch(st, mats);
            u.fillGlow = ContainerFillGlow(st, mats);
            // A mixture's layers, heaviest at the bottom (the order the bench
            // settles them in).
            if (st.contents.n > 1) {
              std::vector<alchemy::Portion> ps(st.contents.p, st.contents.p + st.contents.n);
              std::stable_sort(ps.begin(), ps.end(), [&](const alchemy::Portion& a, const alchemy::Portion& b) {
                const int32_t da = a.mat < mats.size() ? mats[a.mat].gpu.density : 0;
                const int32_t db = b.mat < mats.size() ? mats[b.mat].gpu.density : 0;
                return da > db;
              });
              const float tot = (float)std::max<uint32_t>(1, st.FillTotal());
              for (const alchemy::Portion& p : ps) {
                u.fillBandColor.push_back(0xFF000000u | (p.mat < mats.size() ? (mats[p.mat].gpu.color0 & 0x00FFFFFFu) : 0u));
                u.fillBandFrac.push_back((float)p.eighths / tot);
              }
            }
            u.tip = ContainerFillText(*d, st, mats) +
                    "\ndouble-click: open it on the alchemy bench" +
                    "\nQ / E puts it in your left / right hand;"
                    "\nthat hand's button (RMB / LMB) uses it,"
                    "\nF cycles pour / scoop / apply, hold G throws"
                    "\ncharacter screen: click it under FLASKS, then"
                    "\nhold LMB on the portrait to pour it on yourself";
          }
          return u;
        };
        // The HUD's hotbar strip (DrawHudHotbar) reads this one, so it is the
        // one mirror built with the screen shut.
        // WHILE THE BENCH IS UP a vessel it has had shows what it holds NOW
        // (AlchemyBench::Contents), not the kit's copy: the kit is only
        // written at Finish, and the character screen's FLASKS row, the
        // hands and the hotbar read as the flasks are poured.
        auto live = [&](KitSpace space, int idx, const ItemStack& st) {
          ItemStack shown = st;
          if (bench.IsOpen() && !st.Empty()) bench.Contents(KitRef{space, idx}, shown.contents);
          return shown;
        };
        ui.hotbarSlots.clear();
        for (int i = 0; i < kItemSlots; i++)
          ui.hotbarSlots.push_back(mirror(live(KitSpace::Hotbar, i, hotbar.slots[i])));
        // ...and THE TWO HANDS, which the HUD draws beside the strip (dual
        // wielding): the equipment mirror below is only rebuilt with the
        // screen open, so the hand entries are refreshed here every frame.
        if ((int)ui.equipSlots.size() != kEquipSlotCount)
          ui.equipSlots.assign(kEquipSlotCount, UIState::KitSlotUI{});
        for (int hk = 0; hk < kHands; hk++) {
          const int hs = EquipSlotOfHand(HandAt(hk));
          ui.equipSlots[hs] = mirror(live(KitSpace::Equip, hs, kit.equip.slots[hs]), hs);
        }
        // EVERYTHING BELOW IS THE CHARACTER SCREEN'S, and only it reads these
        // (inventory_ui.cpp, spellgraph_ui.cpp). Shut, they are left as they
        // were and rebuilt on the first frame it is open again — the open key
        // is handled above this block, so that frame is never stale.
        if (ui.inventoryOpen) {
        // ---- THE BENCH, one frame of it (game/alchemy_bench.h) ---------
        // The pointer and tool go to the bench's own thread (it steps at 60
        // Hz whatever this frame does); what comes back is its newest
        // picture and the live contents of whatever is on it.
        if (bench.IsOpen()) {
          alchemy::BenchInput in;
          in.over = ui.alchemy.over;
          in.at = {ui.alchemy.atX, ui.alchemy.atY};
          in.down = ui.alchemy.down;
          in.pressed = ui.alchemy.pressed;
          in.tilt = ui.alchemy.tiltReq;
          in.shock = ui.alchemy.shockReq;
          ui.alchemy.shockReq = false;
          bench.Frame(in, (alchemy::BenchTool)ui.alchemy.tool);
          // The table had no clear spot for a vessel (FindPlaceSpot): it is
          // back in the list, and this says why.
          if (std::string lr = bench.TakeLateRefusal(); !lr.empty()) ui.alchemy.message = lr;
          auto densityOf = [&](const std::string& name) {
            for (size_t k = 1; k < mats.size(); k++)
              if (mats[k].name == name) return mats[k].gpu.density;
            return 0;
          };
          auto partsOf = [&](const alchemy::Composition& c) {
            std::vector<UIState::AlchemyUI::Portion> out;
            for (int i = 0; i < c.n; i++) {
              UIState::AlchemyUI::Portion p;
              const uint16_t mid = alchemy::BaseMat(c.p[i].mat);
              const bool dis = alchemy::IsDissolved(c.p[i].mat);
              p.name = mid < mats.size() ? mats[mid].name + (dis ? " (dissolved)" : "") : std::string("?");
              p.color = mid < mats.size() ? (0xFF000000u | (mats[mid].gpu.color0 & 0x00FFFFFFu)) : 0u;
              p.eighths = (int)c.p[i].eighths;
              // `dissolved` is read as "takes no room" (the fill bar and the
              // n / cap count): dissolved matter rides the liquid, and a
              // stoppered flask's gas is headspace (container.h
              // ContainerVolume) -- counting it read a flask of fumes over-full.
              p.dissolved = dis || (mid < mats.size() && mats[mid].gpu.klass == CLASS_GAS);
              out.push_back(p);
            }
            // Heaviest first: the list reads bottom layer to top.
            std::stable_sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
              return densityOf(a.name) > densityOf(b.name);
            });
            return out;
          };
          // Every single vessel you carry, filled or not (an empty flask is
          // what you pour into), with the bench's live contents for one it
          // has had.
          ui.alchemy.rows.clear();
          auto row = [&](KitSpace space, int idx, const ItemStack& st, int equipSlot) {
            const KitRef r{space, idx};
            if (st.Empty() || st.count != 1) return;
            const ItemDef* d = items.Of(st);
            if (!d || !d->IsContainer()) return;
            ItemStack shown = st;
            bench.Contents(r, shown.contents);
            UIState::AlchemyUI::Row rw;
            rw.ref = r;
            rw.label = d->name + ": " + ContainerFillText(*d, shown, mats);
            rw.slot = mirror(shown, equipSlot);
            rw.onTable = bench.OnTable(r);
            ui.alchemy.rows.push_back(std::move(rw));
          };
          for (int hk = 0; hk < kHands; hk++) {
            const int hs = EquipSlotOfHand(HandAt(hk));
            row(KitSpace::Equip, hs, kit.equip.slots[hs], hs);
          }
          for (int i = 0; i < kItemSlots; i++) row(KitSpace::Hotbar, i, hotbar.slots[i], -1);
          for (int i = 0; i < Bag::kSlots; i++) row(KitSpace::Bag, i, kit.bag.slots[i], -1);
          // The readout: the vessel in hand, or under it, or last touched.
          const KitRef fr = bench.Focus();
          ui.alchemy.focusName.clear();
          ui.alchemy.focusParts.clear();
          ui.alchemy.focusCap = 0;
          // The devices too: a focus with none (off the table) must not show
          // the last vessel's stopper, flame and pressure.
          ui.alchemy.focusStoppered = false;
          ui.alchemy.focusBurner = false;
          ui.alchemy.focusHeat = 0.0f;
          ui.alchemy.focusPressure = 0.0f;
          if (fr.Valid()) {
            const ItemStack* st = kit.Resolve(fr);
            const ItemDef* d = st ? items.Of(*st) : nullptr;
            alchemy::Composition c;
            if (d && bench.Contents(fr, c)) {
              ui.alchemy.focusName = d->name;
              ui.alchemy.focusParts = partsOf(c);
              ui.alchemy.focusCap = d->container.capacity;
              bool stop = false, burn = false;
              float heat = 0, press = 0;
              if (bench.Devices(fr, stop, burn, heat, press)) {
                ui.alchemy.focusStoppered = stop;
                ui.alchemy.focusBurner = burn;
                ui.alchemy.focusHeat = heat;
                ui.alchemy.focusPressure = press;
              }
            }
          }
        }
        ui.bagSlots.clear();
        for (int i = 0; i < Bag::kSlots; i++)
          ui.bagSlots.push_back(mirror(live(KitSpace::Bag, i, kit.bag.slots[i])));
        ui.equipSlots.clear();
        for (int i = 0; i < kEquipSlotCount; i++)
          ui.equipSlots.push_back(mirror(live(KitSpace::Equip, i, kit.equip.slots[i]), i));
        // The corpse's gear, through the same mirror so a robe on a corpse is
        // drawn and tipped exactly as one in the pack — with its condition
        // read off the death-time capture, the only record there is for a
        // piece nobody is wearing.
        ui.lootSlots.clear();
        if (ui.lootOpen && !ui.lootRef.empty()) {
          // A CHEST: its bag, in slot order, the order ContainerTake counts.
          if (const refs::Ref* cr = refStore.Find(ui.lootRef)) {
            ui.lootTitle = refs::ContainerTitle(*cr);
            Bag chest;
            refs::ContainerContents(refStore, *cr, &items, chest);
            for (const ItemStack& st : chest.slots)
              if (!st.Empty()) ui.lootSlots.push_back(mirror(st));
          }
        } else if (ui.lootOpen) {
          const Mob* c = lootCorpse ? mobs.FindMobById(lootCorpse) : nullptr;
          if (c && c->Lootable()) {
            ui.lootTitle = c->Def() ? c->Def()->name : std::string();
            std::vector<LootPiece> pieces;
            c->LootPieces(pieces);
            for (const LootPiece& pc : pieces) {
              // The COUNT comes off the piece now that a corpse can hold a
              // carried stack as well as a worn garment (LootPiece::count).
              // Worn and held pieces are always 1 — a rig slot is one garment —
              // so this reads exactly as the hard-coded 1 did for them, and a
              // pack item gets its badge (ui::CountBadge, inventory_ui.cpp).
              // The piece IS an ItemInstance, so it mirrors as one: its
              // condition is its own captured damage, its fill a flask's.
              ItemStack st = static_cast<const ItemInstance&>(pc);
              if (st.count <= 0) st.count = 1;
              UIState::KitSlotUI u = mirror(st);
              if (u.name.empty()) u.name = pc.name;   // gone from the library
              ui.lootSlots.push_back(std::move(u));
            }
          }
        }

        // The glyph catalogue is a function of the library and the materials
        // alone — valence strings, tariffs, the O(glyphs^2) "delivers" list —
        // so it is REBUILT ONLY WHEN glyphEpoch MOVES. The one per-player
        // field, `owned`, is refreshed every frame the screen is open.
        if (glyphsOwnedEpoch != glyphEpoch ||
            ui.glyphsOwned.size() != glyphs.glyphs.size()) {
        glyphsOwnedEpoch = glyphEpoch;
        ui.glyphsOwned.clear();
        for (int gi = 0; gi < (int)glyphs.glyphs.size(); gi++) {
          const GlyphDef& g = glyphs.glyphs[gi];
          UIState::GlyphUI u;
          u.id = g.id;
          u.desc = g.desc;
          u.type = (int)g.sort;
          u.mana = g.word;
          u.owned = caster.inventory.Owns(gi);
          u.hidden = g.hidden;
          u.axis = g.axis;
          u.example = g.example;
          // Valence, in the sort names the grammar uses (plan §9).
          auto slotNames = [](uint8_t mask) {
            if (mask == kSortAny) return std::string("any");
            std::string s;
            const char* names[5] = {"matter", "effect", "delivery", "mod", "operator"};
            for (int b = 0; b < 5; b++)
              if (mask & (1u << b)) s += (s.empty() ? "" : "/") + std::string(names[b]);
            return s;
          };
          if (g.sort == GlyphSort::Operator) {
            u.valence = (g.hasLeft ? slotNames(g.leftMask) + " < " : std::string()) + g.id +
                        (g.hasRight ? " > " + slotNames(g.rightMask) : std::string()) + " -> " +
                        GlyphSortName(g.result);
            u.emptyNote = std::string(g.hasLeft ? "left empty: incomplete (charged)" : "") +
                          (g.hasLeft && g.hasRight ? "   " : "") +
                          (g.hasRight ? "right empty: incomplete (charged)" : "");
          } else if (g.sort == GlyphSort::Mod) {
            const char* opn = g.op == ModOp::Mul ? "x" : g.op == ModOp::Div ? "/" : "+";
            u.valence = std::string("edits ") + ModFieldName(g.field) + " " + opn +
                        std::to_string(g.amount) + ", again per repeat";
            // COUNT IS RECORD-WIDE wherever it is spoken (spell.cpp's LowerBox),
            // so the canvas must not promise a socket drop edits one instance.
            u.recordWide = false;   // nothing is record-wide wherever spoken now
            u.splits = g.field == ModField::Count;
          } else if (g.sort == GlyphSort::Delivery) {
            u.valence = std::string(g.mech == DeliveryMech::Flight ? "flight" :
                                    g.mech == DeliveryMech::Continuous ? "continuous" : "instant") +
                        ", carry x" + std::to_string(g.carryMille / 1000) + "." +
                        std::to_string((g.carryMille % 1000) / 100);
          } else if (g.sort == GlyphSort::Effect) {
            u.valence = std::string("effect: ") + SpellVerbName(g.verb);
          } else {
            u.valence = g.wildcard ? "matter: whatever is there"
                                   : "matter: " + g.materialName + ", value " +
                                         std::to_string(glyphs.Arcane(g.material));
          }
          switch (g.sort == GlyphSort::Operator || g.sort == GlyphSort::Effect ? g.verb
                                                                                : SpellVerb::None) {
            case SpellVerb::Convert: u.tariff = "volume x (convert + value gap)"; break;
            case SpellVerb::Explode: u.tariff = "power x r^3"; break;
            case SpellVerb::Wind: u.tariff = "footprint x ticks"; break;
            case SpellVerb::Mend: u.tariff = "per voxel: value(M) x graft + foreign penalty"; break;
            case SpellVerb::Trail: u.tariff = "budget voxels x E"; break;
            case SpellVerb::Sustain: u.tariff = "E per tick, sustained"; break;
            case SpellVerb::Filter: u.tariff = "radius^3 per tick"; break;
            case SpellVerb::Repeat: u.tariff = "repeats x E"; break;
            case SpellVerb::Place: u.tariff = "voxels x value(M)"; break;
            default:
              u.tariff = g.sort == GlyphSort::Matter ? "voxels x value(M) x place" :
                         g.sort == GlyphSort::Mod ? "on the expansion (count, volume)" :
                         g.sort == GlyphSort::Delivery ? "carry x the payload, + the word" : "";
          }
          if (g.sort != GlyphSort::Delivery) {
            std::string d;
            for (const GlyphDef& o : glyphs.glyphs)
              if (o.sort == GlyphSort::Delivery) d += (d.empty() ? "" : ", ") + o.id;
            u.delivers = "hand, " + d;
          }
          // The matter swatch is the material's own gpu colour, so a fire
          // glyph is the colour fire actually renders as rather than a colour
          // somebody picked for the UI.
          if (g.sort == GlyphSort::Matter && !g.wildcard && g.material > 0 &&
              g.material < mats.size())
            u.color = mats[g.material].gpu.color0;
          ui.glyphsOwned.push_back(std::move(u));
        }
        }  // glyphsOwnedEpoch
        for (int gi = 0; gi < (int)ui.glyphsOwned.size(); gi++)
          ui.glyphsOwned[gi].owned = caster.inventory.Owns(gi);

        // The grimoire: the authored starters (read-only) and the player's
        // pages, each with the readout and price of its expansion, through
        // the same DescribeSpell the live sentence uses — via the readout
        // cache (spellDescFor, above), re-validated here because the panel's
        // intents earlier this frame may have edited the grimoire.
        spellDescValidate();
        ui.grimoirePages.clear();
        ui.grimoireMaxPages = glyphs.budgets.maxGrimoirePages;
        ui.grimoireMaxWords = glyphs.budgets.maxMacroWords;
        auto describeWords = [&](const std::vector<std::string>& words, std::string& readout,
                                 int32_t& price, bool& unknown, int& dropped) {
          const SpellDescCache::Entry& e = spellDescFor(words);
          readout = e.readout;
          price = e.price;
          unknown = e.unknown;
          dropped = e.dropped;
        };
        for (const ConjoinedGlyph& cg : glyphs.conjoined) {
          UIState::GrimoirePageUI p;
          p.name = cg.id;
          p.readOnly = true;
          for (int gi : cg.glyphs)
            if (const GlyphDef* d = glyphs.At(gi)) p.words.push_back(d->id);
          describeWords(p.words, p.readout, p.price, p.priceUnknown, p.dropped);
          ui.grimoirePages.push_back(std::move(p));
        }
        for (const GrimoirePage& pg : caster.grimoire.pages) {
          UIState::GrimoirePageUI p;
          p.name = pg.name;
          p.words = pg.words;
          describeWords(p.words, p.readout, p.price, p.priceUnknown, p.dropped);
          ui.grimoirePages.push_back(std::move(p));
        }
        {
          int dropped = 0;
          describeWords(ui.grimoireEditWords, ui.grimoireEditReadout, ui.grimoireEditPrice,
                        ui.grimoireEditPriceUnknown, dropped);
        }
        }  // if (ui.inventoryOpen): the character screen's mirrors

        // ---- THE SPELL GRAPH MIRROR (docs/PLAN_spell_graph.md §4) ----------
        //
        // The composer's words -> expansion -> parse -> lower -> BuildGraph,
        // copied field by field into a plain-struct mirror. The same expansion
        // the readout above is produced from, so the drawing and the sentence
        // under it can never be of two different spells.
        //
        // Only while the character screen is open: it is a parse and a
        // lowering per frame, and nothing looks at it otherwise. NOT gated on
        // `grimoireMode` — that flag is the NARROW layout's arsenal/grimoire
        // toggle; the wide three-column layout draws the grimoire always with
        // the flag false, and gating on it left the canvas empty in the game
        // while `--shot-inventory` (which sets the flag by hand) showed a tree.
        // ...and only while the BOOK IS OPEN (2026-09-22). Shut, the panel is a
        // spine with the bound keys on it and nothing reads the mirror at all,
        // so a parse and a lowering every frame would be pure waste.
        if (ui.inventoryOpen && ui.spellbookOpen) {
          ui.spellGraph = UIState::SpellGraphUI{};
          const GrimoireExpansion ex =
              ExpandWords(glyphs, caster.grimoire, ui.grimoireEditWords, kSpellStackMax);
          // A NESTED PAGE IS EXPANDED TO ITS WORDS to draw, and an edit writes
          // the expansion back — the tree has no node for "a page". Said on the
          // status line rather than silently (v1; PLAN §5 notes it).
          for (const std::string& w : ui.grimoireEditWords)
            if (glyphs.FindWord(w) < 0 && !w.empty()) {
              ui.spellGraph.expandedNote =
                  "page " + w + " is drawn expanded; an edit writes out its words";
              break;
            }
          SpellStack gst = StackOf(ex);
          SpellTree tree = ParseSpell(glyphs, gst);
          // A BLANK PAGE STILL HAS A HAND. `ParseSpell` of silence has no
          // clause at all (silence is not a spell), and a canvas with no root
          // has nothing to drop the first word onto.
          if (tree.Empty()) tree = EmptyTree();
          const CastList gl = LowerSpell(glyphs, tree);
          const SpellGraph sg = BuildGraph(glyphs, gl);
          ui.spellGraph.root = sg.root;
          ui.spellGraph.width = sg.width;
          ui.spellGraph.height = sg.height;
          ui.spellGraph.layers = sg.layers;
          ui.spellGraph.wordCost = gl.wordCost;
          ui.spellGraph.tariff = gl.tariff;
          ui.spellGraph.carryCost = gl.carryCost;
          ui.spellGraph.manaCost = gl.manaCost;
          ui.spellGraph.priceUnknown = gl.priceUnknown;
          ui.spellGraph.nodes.reserve(sg.nodes.size());
          for (const SpellGraphNode& n : sg.nodes) {
            UIState::SpellGraphUI::Node u;
            u.kind = (int)n.kind;
            u.treeNode = n.treeNode;
            u.label = n.label;
            u.n = n.n;
            u.sort = (int)n.sort;
            u.lane = n.lane;
            u.x = n.x; u.y = n.y; u.w = n.w; u.h = n.h;
            u.layer = n.layer;
            u.baseLayer = n.baseLayer;
            u.subX = n.subX; u.subW = n.subW;
            u.hasLeft = n.hasLeft; u.hasRight = n.hasRight;
            u.leftFilled = n.leftFilled; u.rightFilled = n.rightFilled;
            u.complete = n.complete;
            u.instances = n.instances;
            u.laneCount = n.laneCount;
            u.sockets = n.sockets;
            u.bus = n.bus;
            u.hasPrice = n.hasPrice;
            u.wordCost = n.price.wordCost;
            u.tariff = n.price.tariff;
            u.carryCost = n.price.carryCost;
            u.priceInstances = n.price.instances;
            u.leaves = n.price.leaves;
            u.instancesClamped = n.price.instancesClamped;
            u.subtotal = n.subtotal;
            u.primary = n.primary;
            u.split = n.split;
            u.owner = n.owner;
            u.bolts = n.bolts;
            u.instance = n.instance;
            u.pipW = n.pipW;
            u.edit = n.edit;
            u.wasted = n.wasted;
            u.mag = n.mag;
            u.graded = n.graded;
            u.magMin = n.magMin;
            u.magMax = n.magMax;
            u.magStep = n.magStep;
            u.magDefault = n.magDefault;
            u.magLabel = n.magLabel;
            u.trigger = (int)n.timing.trigger;
            u.every = n.timing.every;
            u.delay = n.timing.delay;
            u.timingPhrase = n.timing.IsDefault() ? std::string() : TimingPhrase(n.timing);
            u.spanFirst = n.spanFirst;
            u.spanLast = n.spanLast;
            // BY NAME, not by index (DESIGN §8b): the panel holds this across
            // a frame and an R reload renumbers every glyph.
            if (const GlyphDef* gd = glyphs.At(n.glyph)) {
              u.glyphId = gd->id;
              u.wordCostOf = gd->word;
              if (gd->sort == GlyphSort::Matter && !gd->wildcard && gd->material > 0 &&
                  gd->material < mats.size())
                u.color = mats[gd->material].gpu.color0;
            }
            ui.spellGraph.nodes.push_back(std::move(u));
          }
          for (const SpellGraphEdge& e : sg.edges)
            ui.spellGraph.edges.push_back({e.from, e.to, (int)e.kind});
        }
      }

      // ---- THE CONVERSATION MIRROR + THE DEV HOOK (game/dialogue.h) -------
      {
        const dialogue::View v = dialogue::MakeView(talkStore, session.talk);
        ui.talk.open = v.open;
        ui.talk.dialogue = v.dialogue;
        ui.talk.speaker = v.speaker;
        ui.talk.text = v.text;
        ui.talk.choices = v.choices;
        ui.talk.canContinue = v.canContinue;
        ui.talk.canLeave = v.canLeave;
        ui.talk.steps = v.steps;
        if (ui.dialogueReload) {
          ui.dialogueReload = false;
          reloadDialogue();
        }
        if (ui.dialogueResetFlags) {
          ui.dialogueResetFlags = false;
          talkStore.ResetState();
          ui.dialogueStatus = "flags and met cleared";
        }
        // "talk to nearest creature" / "talk (no speaker)": queued on the
        // session and started by the next tick (dialogue::BeginRequest).
        if ((ui.dialogueTalkNearest || ui.dialogueTalkVoice) &&
            !ui.dialogueNames.empty()) {
          const std::string name = ui.dialogueNames[std::clamp(
              ui.dialoguePick, 0, (int)ui.dialogueNames.size() - 1)];
          dialogue::Speaker sp;
          bool ok = true;
          if (ui.dialogueTalkNearest) {
            const float reach = 12.0f / kVoxelMeters;
            float best = reach * reach;
            for (uint32_t i = 0; i < mobs.MobCount(); i++) {
              const Mob* m = mobs.MobAt(i);
              if (!m || !m->Alive() || !m->Def()) continue;
              const Vec3 d = m->Origin() - player.pos;
              const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
              if (d2 < best) {
                best = d2;
                sp.mobId = m->Id();
                sp.name = m->Def()->name;
              }
            }
            ok = sp.mobId != 0;
            ui.dialogueStatus = ok ? "talking to " + sp.name + " (" + name + ")"
                                   : "nobody within 12 m to talk to";
          } else {
            ui.dialogueStatus = "talking (" + name + ")";
          }
          std::printf("dialogue: %s\n", ui.dialogueStatus.c_str());
          if (ok) session.talkBegin = dialogue::BeginRequest{true, sp, name};
        }
        ui.dialogueTalkNearest = ui.dialogueTalkVoice = false;
        if (ui.visible) {
          ui.dialogueFlags.clear();
          for (const auto& [k, val] : talkStore.flags)
            ui.dialogueFlags.push_back(k + " = " + std::to_string(val));
          for (const std::string& m : talkStore.met) ui.dialogueFlags.push_back("met: " + m);
        }
      }
      overlay.BeginFrame();
      // HUD first, dev panel second: the panel is a real ImGui window and gets
      // to sit on top of the chrome, not the other way round.
      //
      // The HUD is SUPPRESSED while the character screen is open: the screen
      // already shows both pools and the body condition, larger and with the
      // numbers spelled out, so drawing the corner chrome underneath it is two
      // readouts of the same thing fighting for the same corner.
      // ...and while talking: the conversation panel sits where the hotbar is.
      if (!ui.inventoryOpen && !ui.talk.open) overlay.DrawHUD(ui);
      overlay.Draw(ui);

      // Wind force multipliers. Its OWN latch, and deliberately not folded
      // into fluidTuningDirty below: that path ends in sim.ReloadShaders(),
      // and these two knobs ride TickParams precisely so they do not need one.
      // A slider that recompiled every shader on each frame of a drag would be
      // unusable, which is the whole reason they are on the tick stream and
      // are NO_WGSL rows of tuning_params.def.
      if (ui.windTuningDirty) {
        ui.windTuningDirty = false;
        Tuning t = CurrentTuning();
        t.sim.windGasScale = ui.windGasScale;
        t.sim.windPartScale = ui.windPartScale;
        t.sim.windDragRef = ui.windDragRef;
        SetCurrentTuning(t);
      }

      // ---- the Combat panel ------------------------------------------------
      //
      // The panel already wrote the tuning itself (see UIState::
      // combatWindowOpen for why that one deviates from the mirror pattern),
      // so there is nothing to copy here. TWO THINGS still have to happen on
      // this side, and neither is a tuning value:
      //
      //   * MeleeState caches its MeleeTuning BY VALUE, so a `melee.*` edit is
      //     invisible to the live stroke until it is re-applied. This is the
      //     same class of thing as RefreshGoreProfiles in the F5 block: state
      //     drawn from the tuning at some earlier moment has to be told.
      //     `combatfx.*` and `gore.*` need no equivalent — every reader of
      //     those goes through CurrentTuning() at the point of use.
      //   * Save is a file write, which the overlay has no business doing.
      if (ui.combatTuningDirty) {
        ui.combatTuningDirty = false;
        ApplyMeleeTuning(melee.tuning);
        // infectSpreadRate drives grid-side reaction chances compiled by
        // LoadAssets, so recompile ONLY when it actually moved. Every other
        // combat slider is CPU-only and needs no materials reload.
        static float prevInfectSpread = CurrentTuning().gore.infectSpreadRate;
        const float curInfectSpread = CurrentTuning().gore.infectSpreadRate;
        if (curInfectSpread != prevInfectSpread) {
          ui.reloadMaterials = true;
          prevInfectSpread = curInfectSpread;
        }
      }
      if (ui.combatSave) {
        ui.combatSave = false;
        std::string serr;
        ui.combatSaveStatus =
            SaveCombatTuning(assetDir + "/materials/tuning.json",
                             CurrentTuning(), serr)
                ? "saved"
                : ("FAILED: " + serr);
      }

      // ---- NPC behaviour profile editing -----------------------------------
      //
      // Its own latch, like the wind knobs and for the same reason: nothing
      // here touches a shader, so an AI you have to press Apply to feel would
      // be an AI you cannot tune. Write-through is to the PROFILE, so every mob
      // running it changes at once — a per-mob override would be variance, and
      // this engine already has a place for that.
      {
        ai::Library& lib = mobs.BehaviorsMut();
        ai::Profile* pe = lib.At(ui.aiProfileEdit);
        if (pe != nullptr && ui.aiProfileReseat) {
          ui.aiProfileReseat = false;
          ui.aiSightRange = pe->perception.sightRange;
          ui.aiFovDegrees = pe->perception.fovDegrees;
          ui.aiRequireLos = pe->perception.requireLos;
          ui.aiAlertDecayTicks = (int)pe->perception.alertDecayTicks;
          ui.aiKeepRangeScale = pe->perception.keepRangeScale;
          ui.aiMobile = pe->movement.mobile;
          ui.aiRangeMin = pe->movement.rangeMin;
          ui.aiRangeMax = pe->movement.rangeMax;
          ui.aiBandSlack = pe->movement.bandSlack;
          ui.aiApproachSpeed = pe->movement.approachSpeed;
          ui.aiStrafeSpeed = pe->movement.strafeSpeed;
          ui.aiRetreatSpeed = pe->movement.retreatSpeed;
          ui.aiCircleTendency = pe->movement.circleTendency;
          ui.aiCircleHoldTicks = (int)pe->movement.circleHoldTicks;
          ui.aiRepathTicks = (int)pe->movement.repathTicks;
          ui.aiNavRadius = pe->movement.navRadius;
          ui.aiAttackReach = pe->attack.reach;
          ui.aiAimTolerance = pe->attack.aimTolerance;
          ui.aiCadenceTicks = (int)pe->attack.cadenceTicks;
          ui.aiJitterTicks = (int)pe->attack.jitterTicks;
          ui.aiCommitTicks = (int)pe->attack.commitTicks;
          ui.aiDisengageTicks = (int)pe->attack.disengageTicks;
          ui.aiHysteresis = pe->hysteresis;
          static_assert((int)ai::Intent::Count == UIState::kAiIntents,
                        "the dev panel's intent rows must match ai::Intent");
          for (int k = 0; k < (int)ai::Intent::Count; k++) {
            ui.aiIntentWeight[k] = pe->intents[k].weight;
            ui.aiIntentCooldown[k] = (int)pe->intents[k].cooldownTicks;
            ui.aiIntentDwell[k] = (int)pe->intents[k].minDwellTicks;
          }
        }
        if (pe != nullptr && ui.aiTuningDirty) {
          ui.aiTuningDirty = false;
          pe->perception.sightRange = ui.aiSightRange;
          pe->perception.fovDegrees = ui.aiFovDegrees;
          pe->perception.requireLos = ui.aiRequireLos;
          pe->perception.alertDecayTicks = (uint32_t)ui.aiAlertDecayTicks;
          pe->perception.keepRangeScale = ui.aiKeepRangeScale;
          pe->movement.mobile = ui.aiMobile;
          pe->movement.rangeMin = ui.aiRangeMin;
          pe->movement.rangeMax = ui.aiRangeMax;
          pe->movement.bandSlack = ui.aiBandSlack;
          pe->movement.approachSpeed = ui.aiApproachSpeed;
          pe->movement.strafeSpeed = ui.aiStrafeSpeed;
          pe->movement.retreatSpeed = ui.aiRetreatSpeed;
          pe->movement.circleTendency = ui.aiCircleTendency;
          pe->movement.circleHoldTicks = (uint32_t)ui.aiCircleHoldTicks;
          pe->movement.repathTicks = (uint32_t)ui.aiRepathTicks;
          pe->movement.navRadius = ui.aiNavRadius;
          pe->attack.reach = ui.aiAttackReach;
          pe->attack.aimTolerance = ui.aiAimTolerance;
          pe->attack.cadenceTicks = (uint32_t)ui.aiCadenceTicks;
          pe->attack.jitterTicks = (uint32_t)ui.aiJitterTicks;
          pe->attack.commitTicks = (uint32_t)ui.aiCommitTicks;
          pe->attack.disengageTicks = (uint32_t)ui.aiDisengageTicks;
          pe->hysteresis = ui.aiHysteresis;
          static_assert((int)ai::Intent::Count == UIState::kAiIntents,
                        "the dev panel's intent rows must match ai::Intent");
          for (int k = 0; k < (int)ai::Intent::Count; k++) {
            pe->intents[k].weight = ui.aiIntentWeight[k];
            pe->intents[k].cooldownTicks = (uint32_t)ui.aiIntentCooldown[k];
            pe->intents[k].minDwellTicks = (uint32_t)ui.aiIntentDwell[k];
          }
        }
        if (ui.aiSaveBehaviors) {
          ui.aiSaveBehaviors = false;
          std::string serr;
          ui.aiSaveStatus =
              ai::SaveBehaviors(assetDir + "/mobs/behaviors.json", lib, serr)
                  ? "saved"
                  : ("FAILED: " + serr);
        }
      }

      if (ui.fluidTuningDirty) {
        ui.fluidTuningDirty = false;
        Tuning t = CurrentTuning();
        t.sim.fluidGravity     = ui.fGravity;
        t.sim.fluidStiffness   = ui.fStiffness;
        t.sim.fluidRestDensity = ui.fRestDensity;
        t.sim.fluidEosPower    = ui.fEosPower;
        t.sim.fluidCohesion    = ui.fCohesion;
        t.sim.fluidAttractSame = ui.fAttractSame;
        t.sim.fluidAttractDiff = ui.fAttractDiff;
        t.sim.fluidViscosity   = ui.fViscosity;
        t.sim.fluidDamping     = ui.fDamping;
        t.sim.fluidSplashRate       = ui.fSplashRate;
        t.sim.fluidSplashSpeed      = ui.fSplashSpeed;
        t.sim.fluidSplashMaxDensity = ui.fSplashMaxDensity;
        t.sim.fluidSplashLife       = ui.fSplashLife;
        t.sim.fluidSplashScaleIdx   = ui.fSplashScaleIdx;
        t.sim.fluidFoamRate         = ui.fFoamRate;
        t.sim.fluidFoamCrestRate    = ui.fFoamCrestRate;
        t.sim.fluidTrappedMin       = ui.fTrappedMin;
        t.sim.fluidTrappedMax       = ui.fTrappedMax;
        t.sim.fluidCrestMin         = ui.fCrestMin;
        t.sim.fluidCrestMax         = ui.fCrestMax;
        t.sim.fluidFoamEnergyMin    = ui.fFoamEnergyMin;
        t.sim.fluidFoamEnergyMax    = ui.fFoamEnergyMax;
        t.sim.fluidFoamLife         = ui.fFoamLife;
        t.sim.fluidFoamLifeMin      = ui.fFoamLifeMin;
        t.sim.fluidBubbleBuoyancy   = ui.fBubbleBuoyancy;
        t.sim.fluidFoamDrag         = ui.fFoamDrag;
        t.sim.fluidBubbleDensity    = ui.fBubbleDensity;
        t.sim.fluidSprayDensity     = ui.fSprayDensity;
        t.sim.fluidFoamScaleIdx     = ui.fFoamScaleIdx;
        t.sim.fluidExciteMode       = ui.fExciteMode;
        t.sim.fluidSettleEps        = ui.fSettleEps;
        t.sim.fluidWakeSpeed        = ui.fWakeSpeed;
        t.sim.fluidSettleTicks      = ui.fSettleTicks;
        t.render.fluidSurface      = ui.fSurface;
        t.render.fluidIso          = ui.fIso;
        t.render.fluidSmooth       = ui.fSmooth;
        t.render.fluidIor          = ui.fIor;
        t.render.fluidClarity      = ui.fClarity;
        t.render.fluidReflect      = ui.fReflect;
        t.render.fluidSpecular     = ui.fSpecular;
        std::memcpy(t.render.fluidShallow, ui.fShallow, sizeof(ui.fShallow));
        std::memcpy(t.render.fluidDeep,    ui.fDeep,    sizeof(ui.fDeep));
        t.render.fluidDepth        = ui.fDepth;
        t.render.fluidGradient     = ui.fGradientStr;
        t.render.fluidFoam         = ui.fRFoam;
        t.render.fluidFoamField    = ui.fRFoamField;
        t.render.fluidFoamTexture  = ui.fRFoamTexture;
        t.render.fluidFoamSpeed    = ui.fRFoamSpeed;
        t.render.fluidWobble       = ui.fWobble;
        t.render.fluidParticleSize = ui.fParticleSize;
        t.render.fluidStretch      = ui.fStretch;
        t.render.fluidDensityShade = ui.fDensityShade;
        SetCurrentTuning(t);
        sim.ReloadShaders(ctx.device);
        if (labScene >= 0) LabPatchTuningJson(labTuningPath, t, &labTuningMtime);
      }

      // grenades render as emissive sprite cubes (flash as the fuse runs out)
      std::vector<Sprite> sprv;
      for (const Grenade& g : grenades) {
        float flash =
            (g.fuse < 0.7f && std::fmod(g.fuse, 0.22f) < 0.11f) ? 0.9f : 0.05f;
        Sprite s{};
        s.pos[0] = g.pos.x; s.pos[1] = g.pos.y; s.pos[2] = g.pos.z;
        s.halfSize = 1.3f;
        s.color = 0xFF202038;  // dark, slightly red (0xAABBGGRR)
        s.emission = flash;
        sprv.push_back(s);
      }
      // laser beam: emissive sprite dashes from the muzzle to the picked
      // surface + an impact glow (render-only; the cut is the mode-2 ops)
      if (laserHeldFrame && world.Snap().valid && world.Snap().pick[0] != 0) {
        const WorldSnapshot& snap = world.Snap();
        Vec3 hit{(float)(int)snap.pick[2] + 0.5f, (float)(int)snap.pick[3] + 0.5f,
                 (float)(int)snap.pick[4] + 0.5f};
        Vec3 from = LaserMuzzle(player, cam);
        Vec3 d = hit - from;
        int n = std::min(22, (int)(d.len() / 2.5f) + 1);
        for (int i = 1; i <= n && sprv.size() + 1 < kMaxSprites; i++) {
          float f = (float)i / (float)(n + 1);
          Sprite s{};
          Vec3 p = from + d * f;
          s.pos[0] = p.x; s.pos[1] = p.y; s.pos[2] = p.z;
          s.halfSize = 0.18f;
          s.color = 0xFF2030FF;  // red beam (0xAABBGGRR)
          s.emission = 0.9f;
          sprv.push_back(s);
        }
        Sprite s{};
        s.pos[0] = hit.x; s.pos[1] = hit.y; s.pos[2] = hit.z;
        s.halfSize = 0.7f;
        s.color = 0xFF60B0FF;
        s.emission = 1.0f;
        sprv.push_back(s);
      }

      // SPELL FLIGHTS. Each delivery glyph's `look` (glyphs.json, render-only
      // content) says how its carrier is drawn: a bolt is a bright core with a
      // streak back along the velocity, a ball a rounded mass of lobes with a
      // short tail, an orb a slow breathing core with motes orbiting the flight
      // axis, a spark a flicker. All emissive, tinted by the first matter the
      // cast carries (a firebolt reads as fire, an acid bolt as acid) or else
      // by the look's own colour, with no per-spell render code. THIS is the
      // one place spell state becomes float: the authoritative position is
      // fixed-point and is only lerped to float here, at the drawing boundary
      // (spell.h thesis 3). Over the sprite budget a flight keeps its core and
      // loses its dressing, in launch order.
      {
        const float tNow = (float)NowSeconds();
        auto scaleColor = [](uint32_t c, float k) -> uint32_t {
          auto ch = [&](int sh) {
            const float v = (float)((c >> sh) & 0xFFu) * k;
            return (uint32_t)std::clamp(v, 0.0f, 255.0f) << sh;
          };
          return (c & 0xFF000000u) | ch(0) | ch(8) | ch(16);
        };
        auto push = [&](Vec3 at, float half, uint32_t color, float emission) {
          if (sprv.size() + 1 >= kMaxSprites) return false;
          Sprite s{};
          s.pos[0] = at.x;
          s.pos[1] = at.y;
          s.pos[2] = at.z;
          s.halfSize = half;
          s.color = color;
          s.emission = emission;
          sprv.push_back(s);
          return true;
        };
        static const Vec3 kAxes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const SpellProjectile& p : spells.Live()) {
          const GlyphLook& lk = glyphs.Delivery(p.cast.delivery.glyph).look;
          const Vec3 at{SpellFxToFloat(p.pos.x), SpellFxToFloat(p.pos.y), SpellFxToFloat(p.pos.z)};
          const Vec3 v{SpellFxToFloat(p.vel.x), SpellFxToFloat(p.vel.y), SpellFxToFloat(p.vel.z)};
          const float sp = v.len();
          const Vec3 dir = sp > 1e-4f ? v * (1.0f / sp) : Vec3{0, 1, 0};
          const uint32_t tint = CastTintMaterial(p.cast);
          const uint32_t base = tint != 0 && tint < mats.size() ? mats[tint].gpu.color0 : lk.color;
          const float phase = (float)(p.seq % 64u) * 0.37f;
          const float flick = 0.85f + 0.15f * std::sin(tNow * 23.0f + phase);
          if (p.resting) {
            // A fused bolt sits where it landed: a spark that pulses faster as
            // the fuse runs down.
            const float pulse =
                0.5f + 0.5f * std::sin(tNow * (8.0f + 40.0f / (float)std::max(1, p.fuseLeft)));
            push(at, lk.size * 0.5f * (0.6f + 0.4f * pulse), base, lk.glow * (0.5f + pulse));
            continue;
          }
          switch (lk.shape) {
            case LookShape::Bolt: {
              push(at, lk.size * flick, base, lk.glow * 1.2f);
              for (int k = 1; k <= lk.tail; k++) {
                const float f = (float)k / (float)(lk.tail + 1);
                if (!push(at - dir * (lk.tailStep * (float)k), lk.size * (1.0f - 0.8f * f) * 0.8f,
                          scaleColor(base, 1.0f - 0.7f * f), lk.glow * (1.0f - f)))
                  break;
              }
              break;
            }
            case LookShape::Ball: {
              push(at, lk.size * flick, base, lk.glow);
              // Six lobes make one cube read as a rounded mass.
              const float lobe = lk.size * 0.62f, off = lk.size * 0.55f;
              for (int k = 0; k < 6; k++)
                push(at + kAxes[k] * off, lobe, scaleColor(base, 0.85f), lk.glow * 0.8f);
              for (int k = 1; k <= lk.tail; k++) {
                const float f = (float)k / (float)(lk.tail + 1);
                if (!push(at - dir * (lk.tailStep * (float)k), lk.size * (0.7f - 0.5f * f),
                          scaleColor(base, 0.8f - 0.6f * f), lk.glow * (0.7f - 0.6f * f)))
                  break;
              }
              break;
            }
            case LookShape::Orb: {
              const float breathe = 1.0f + 0.12f * std::sin(tNow * 3.0f + phase);
              push(at, lk.size * breathe, base, lk.glow);
              // A ring of motes about the flight axis.
              const Vec3 up = std::fabs(dir.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
              const Vec3 u = up.cross(dir).normalized();
              const Vec3 w = dir.cross(u);
              const int motes = std::max(3, lk.tail);
              const float rr = lk.size * 1.6f;
              for (int k = 0; k < motes; k++) {
                const float ang = tNow * 4.0f + phase + (float)k * 6.2831853f / (float)motes;
                if (!push(at + (u * std::cos(ang) + w * std::sin(ang)) * rr, lk.size * 0.28f,
                          scaleColor(base, 1.1f), lk.glow * 1.3f))
                  break;
              }
              break;
            }
            case LookShape::Spark: {
              push(at, lk.size * flick, base, lk.glow * 1.5f);
              for (int k = 1; k <= lk.tail; k++) {
                const float f = (float)k / (float)(lk.tail + 1);
                if (!push(at - dir * (lk.tailStep * (float)k), lk.size * 0.5f * (1.0f - f),
                          scaleColor(base, 1.0f - 0.8f * f), lk.glow * (1.0f - f)))
                  break;
              }
              break;
            }
          }
        }
        // ---- THE SPELL IN THE FIST (spells in hand) -------------------------
        //
        // A hand holding a spell holds its light: a breathing core in the
        // spell's colour (the colour its flight is drawn in, above) with motes
        // circling the hand, and a flare for a moment after each cast. Drawn
        // at SpellHandPoint, the point the tick casts from, so the light is
        // where the bolt comes out. Presentation only: frame time, no tick
        // state. First person too: the arms are kept in view.
        for (int hk = 0; hk < kHands; hk++) {
          const uint32_t col = ui.handSpellColor[hk];
          if (col == 0 || ui.handSpell[hk].empty()) continue;
          if (!kit.equip.InHand(HandAt(hk)).Empty()) continue;
          const Hand h = HandAt(hk);
          // Plus the body's render offset (tick interpolation + step smoothing,
          // Mob::SetRenderOffset): the arm is DRAWN at xf + offset, and without
          // it the light snaps at the tick rate while the hand glides.
          const Vec3 at = SpellHandPoint(avatar, player, cam, h) +
                          (avatar.Spawned() ? avatar.RenderOffset() : Vec3{0, 0, 0});
          // The flare: the core swells for ~0.2 s after this hand's press.
          static double flareAt[2] = {-9.0, -9.0};
          static bool btnPrev[2] = {false, false};
          const bool btn = captured && (hk == 0 ? mouseL : mouseR);
          if (btn && !btnPrev[hk]) flareAt[hk] = tNow;
          btnPrev[hk] = btn;
          const float flare = std::clamp(1.0f - (float)(tNow - flareAt[hk]) / 0.2f, 0.0f, 1.0f);
          const float phase = (float)hk * 2.1f;
          const float breathe = 1.0f + 0.15f * std::sin(tNow * 4.0f + phase);
          push(at, (0.32f * breathe) * (1.0f + 1.2f * flare), col, 1.6f + 2.5f * flare);
          // Three motes on tilted orbits round the hand, a slower wisp rising.
          static const Vec3 kOrbitU[3] = {{1, 0, 0}, {0, 0.7071f, 0.7071f}, {0.7071f, 0.7071f, 0}};
          static const Vec3 kOrbitW[3] = {{0, 1, 0}, {1, 0, 0}, {0, -0.7071f, 0.7071f}};
          for (int k = 0; k < 3; k++) {
            const float ang = tNow * (3.0f + 0.9f * (float)k) + phase + (float)k * 2.094f;
            const float rr = 0.75f + 0.1f * std::sin(tNow * 5.0f + (float)k);
            if (!push(at + (kOrbitU[k] * std::cos(ang) + kOrbitW[k] * std::sin(ang)) * rr,
                      0.1f, scaleColor(col, 1.25f), 2.2f))
              break;
          }
          const float rise = std::fmod(tNow * 0.9f + phase, 1.0f);
          push(at + Vec3{0.15f * std::sin(tNow * 7.0f + phase), 0.3f + rise * 1.1f, 0.0f},
               0.12f * (1.0f - rise), scaleColor(col, 0.9f), 1.4f * (1.0f - rise));
        }
        // Bombs are debris bodies and the debris path draws them; the fuse
        // sparks on top, faster as it runs down.
        for (const SpellBomb& bm : spells.Bombs()) {
          const Vec3 at{SpellFxToFloat(bm.lastPos.x), SpellFxToFloat(bm.lastPos.y) + 2.0f,
                        SpellFxToFloat(bm.lastPos.z)};
          const float pulse =
              0.5f + 0.5f * std::sin(tNow * (6.0f + 60.0f / (float)std::max(1, bm.fuseLeft)));
          push(at, 0.25f + 0.2f * pulse, 0xFF60D0FFu, 1.5f + pulse);
        }
        // Impact flashes: a core that shrinks as a shell of motes flies out.
        for (const SpellFlash& fl : spellFlashes) {
          const float age = (float)(fl.ttl0 - fl.ttl) / (float)fl.ttl0;   // 0..1
          const float r = fl.radius * (0.3f + 0.9f * age);
          push(fl.at, fl.radius * 0.5f * (1.0f - age) + 0.2f, fl.color, 2.0f * (1.0f - age));
          for (int k = 0; k < 8; k++) {
            const Vec3 corner{(k & 1) ? 0.577f : -0.577f, (k & 2) ? 0.577f : -0.577f,
                              (k & 4) ? 0.577f : -0.577f};
            if (!push(fl.at + corner * r, 0.25f * (1.0f - age) + 0.08f,
                      scaleColor(fl.color, 1.0f - 0.5f * age), 1.5f * (1.0f - age)))
              break;
          }
        }
      }

      // SCOOP MOTES (game/session.h ScoopMote): every cell a flask or pouch
      // took flies into its mouth -- a hop up off the ground, a swirl about
      // the line to the vessel, then an accelerating pull that shrinks it to
      // nothing at the mouth. The mouth is re-read every frame, so the stream
      // bends after the hand. Render-only; the matter was already paid for by
      // the scoop ledger.
      if (!session.scoopMotes.empty()) {
        Vec3 mouth = player.RenderEyePos(tickAlpha) + cam.Forward() * MetresToCells(0.35f) -
                     Vec3{0, MetresToCells(0.15f), 0};
        {
          Vec3 hp;
          Quat hq;
          const int hs = avatar.Spawned() ? avatar.HeldSlot() : -1;
          if (hs >= 0 && avatar.PartWorldTransform(hs, hp, hq)) {
            mouth = hp + Vec3{0, MetresToCells(0.08f), 0};
            // The vessel's own lip on the look side (Mob::HeldMouthWorld),
            // the point the tick streams from.
            Vec3 lip;
            if (avatar.HeldMouthWorld(cam.Forward(), lip)) mouth = lip;
          }
        }
        auto shade = [](uint32_t c, float k) -> uint32_t {
          auto ch = [&](int sh) {
            const float v = (float)((c >> sh) & 0xFFu) * k;
            return (uint32_t)std::clamp(v, 0.0f, 255.0f) << sh;
          };
          return 0xFF000000u | ch(0) | ch(8) | ch(16);
        };
        auto at = [&](const ScoopMote& m, float t, float phase) {
          // APPLY MODE (ScoopMote::out) flies the same path backwards: mouth
          // to skin, so a slow lift out of the neck and a fast landing.
          if (m.out) {
            const Vec3 d = m.to - mouth;
            const float len = std::max(d.len(), 1e-3f);
            const Vec3 axis = d * (1.0f / len);
            const Vec3 ref = std::fabs(axis.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
            const Vec3 u = ref.cross(axis).normalized();
            const Vec3 v = axis.cross(u);
            const float push = t * t;
            const float arc = std::sin(3.14159265f * t);
            const float ang = phase + t * 9.0f;
            const float r = std::min(0.8f, 0.08f * len) * arc;
            return mouth + d * push + Vec3{0, 0.6f, 0} * arc * (1.0f - push) +
                   (u * std::cos(ang) + v * std::sin(ang)) * r;
          }
          const Vec3 d = mouth - m.from;
          const float len = std::max(d.len(), 1e-3f);
          const Vec3 axis = d * (1.0f / len);
          const Vec3 ref = std::fabs(axis.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
          const Vec3 u = ref.cross(axis).normalized();
          const Vec3 v = axis.cross(u);
          const float pull = t * t;                         // slow lift, fast suck
          const float arc = std::sin(3.14159265f * t);      // 0 at both ends
          const float ang = phase + t * 9.0f;               // ~1.5 turns
          const float r = std::min(1.2f, 0.12f * len) * arc;
          return m.from + d * pull + Vec3{0, 1.1f, 0} * arc * (1.0f - pull) +
                 (u * std::cos(ang) + v * std::sin(ang)) * r;
        };
        for (const ScoopMote& m : session.scoopMotes) {
          if (m.delay > 0) continue;   // the cell is still in the grid
          if (sprv.size() + 2 >= kMaxSprites) break;
          const float t = std::clamp(((float)m.age + tickAlpha) / (float)m.life, 0.0f, 1.0f);
          const float phase = (float)(m.seed & 0xFFFFu) * (6.2831853f / 65536.0f);
          const float size = 0.40f + 0.06f * (float)((m.seed >> 16) & 3u) / 3.0f;
          const float bright = 0.9f + 0.2f * (float)((m.seed >> 20) & 7u) / 7.0f;
          const Vec3 p = at(m, t, phase);
          Sprite s{};
          s.pos[0] = p.x; s.pos[1] = p.y; s.pos[2] = p.z;
          // Inbound shrinks into the mouth; outbound grows out of it.
          s.halfSize = m.out ? size * (0.25f + 0.75f * t)
                             : size * (1.0f - 0.8f * t * t);
          s.color = shade(m.color, bright);
          s.emission = 0.0f;
          s.mat = m.mat;   // glows as its matter does (debris.wgsl vsSprite)
          sprv.push_back(s);
          // A crumb trailing behind, so the stream reads as matter pouring
          // up rather than cubes teleporting.
          if (t > 0.15f) {
            const Vec3 q = at(m, t - 0.15f, phase + 1.7f);
            Sprite c = s;
            c.pos[0] = q.x; c.pos[1] = q.y; c.pos[2] = q.z;
            c.halfSize = s.halfSize * 0.4f;
            c.color = shade(m.color, bright * 0.85f);
            sprv.push_back(c);
          }
        }
      }

      // prefab tool preview: marker box at the anchor cell, sized to the
      // rotated footprint (cheap stand-in for a full ghost render)
      if (ui.tool == UIState::kToolPrefab && !prefabs.empty() &&
          ui.prefabSelected < (int)prefabs.size() && world.Snap().valid &&
          world.Snap().pick[0] != 0) {
        const WorldSnapshot& snap = world.Snap();
        const Prefab& pf = prefabs[ui.prefabSelected];
        IVec3 rs = PrefabPlacer::RotatedSize(pf, ui.prefabRot);
        Sprite s{};
        s.pos[0] = (float)((int)snap.pick[5]) + 0.5f;
        s.pos[1] = (float)((int)snap.pick[6]) + (float)rs.y * 0.5f;
        s.pos[2] = (float)((int)snap.pick[7]) + 0.5f;
        s.halfSize = 0.5f * (float)std::max(rs.x, std::max(rs.y, rs.z));
        s.color = 0x2860E0FF;  // translucent warm marker (0xAABBGGRR)
        s.emission = 0.25f;
        sprv.push_back(s);
      }
      if (!sprv.empty()) {
        if (sprv.size() > kMaxSprites) sprv.resize(kMaxSprites);
        ctx.queue.WriteBuffer(world.sprites, 0, sprv.data(),
                              sprv.size() * sizeof(Sprite));
      }

      static std::vector<DebugBox> dbg;
      dbg.clear();
      if (ui.showCollisionBoxes) {
        avatar.AppendDebugBoxes(dbg, kMaxDebugBoxes, 0xC000FF40u);
        mobs.AppendDebugBoxes(dbg, kMaxDebugBoxes, 0xC0FFFF40u);
        {
          static std::vector<SubShapeBox> subs;
          for (uint32_t i = 0; i < debris.BodyCount(); i++) {
            if (dbg.size() >= kMaxDebugBoxes) break;
            const uint64_t h = debris.BodyHandle(i);
            if (!h) continue;
            BodyTransform xf{};
            if (!phys.GetTransform(h, xf)) continue;
            const Quat bodyQ{xf.quat[0], xf.quat[1], xf.quat[2], xf.quat[3]};
            subs.clear();
            if (phys.GetSubShapeBoxes(h, subs, kMaxDebugBoxes - dbg.size())) {
              for (const SubShapeBox& ss : subs) {
                DebugBox b{};
                const Vec3 c = xf.pos + QuatRotate(bodyQ, ss.center);
                b.pos[0] = c.x; b.pos[1] = c.y; b.pos[2] = c.z;
                b.half[0] = ss.halfExtents.x;
                b.half[1] = ss.halfExtents.y;
                b.half[2] = ss.halfExtents.z;
                const Quat q = QuatNormalize(QuatMul(bodyQ, Quat{ss.quat[0], ss.quat[1], ss.quat[2], ss.quat[3]}));
                b.quat[0] = q.x; b.quat[1] = q.y; b.quat[2] = q.z;
                b.quat[3] = q.w;
                b.color = 0xC040FFFFu;
                dbg.push_back(b);
              }
            } else {
              Vec3 lo, hi;
              if (!phys.GetLocalBounds(h, lo, hi)) continue;
              DebugBox b{};
              const Vec3 mid{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f,
                             (lo.z + hi.z) * 0.5f};
              const Vec3 c = xf.pos + QuatRotate(bodyQ, mid);
              b.pos[0] = c.x; b.pos[1] = c.y; b.pos[2] = c.z;
              b.half[0] = (hi.x - lo.x) * 0.5f;
              b.half[1] = (hi.y - lo.y) * 0.5f;
              b.half[2] = (hi.z - lo.z) * 0.5f;
              std::memcpy(b.quat, xf.quat, sizeof(b.quat));
              b.color = 0xC040FFFFu;
              dbg.push_back(b);
            }
          }
        }
      }
      if (ui.showDirtyChunks) {
        const WorldSnapshot& dsnap = world.Snap();
        if (dsnap.valid && dsnap.dirtyFlags.size() == kNumSlots) {
          constexpr float h = (float)kChunk * 0.5f;
          for (uint32_t i = 0; i < kNumSlots && dbg.size() < kMaxDebugBoxes; i++) {
            if (!dsnap.dirtyFlags[i]) continue;
            IVec3 wc = world.SlotToWorldChunk(i);
            DebugBox b{};
            b.pos[0] = (float)(wc.x * (int)kChunk) + h;
            b.pos[1] = (float)(wc.y * (int)kChunk) + h;
            b.pos[2] = (float)(wc.z * (int)kChunk) + h;
            b.half[0] = h; b.half[1] = h; b.half[2] = h;
            b.quat[3] = 1.0f;
            b.color = 0xC000FF00u;
            dbg.push_back(b);
          }
        }
      }
      // ---- NPC AI debug viz (game/ai_behavior.h) --------------------------
      //
      // There is no line primitive in this engine — debug_lines.wgsl draws
      // oriented BOXES and nothing else — so a segment is a very thin box and a
      // ring is a dashed circle of small ones. That is not a workaround to be
      // apologised for: the box path is already barrier-correct, depth-tested
      // off (so a path behind a hill still reads), and costs literally zero
      // when the count is zero.
      //
      // Colour carries the meaning: the profile's own colour for the path, a
      // brighter version of it for the line to the target, and a dimmer one for
      // the range band. Per-mob state labels are in the panel rather than in
      // the world, because this engine has no world-space text (overlay.cpp
      // draws screen chrome only) and inventing a projection for a dev readout
      // is not worth the surface area.
      if (ui.showAiDebug) {
        auto segment = [&](Vec3 a, Vec3 b, float halfW, uint32_t col) {
          if (dbg.size() >= kMaxDebugBoxes) return;
          const Vec3 d = b - a;
          const float len = d.len();
          if (len < 1e-3f) return;
          // Yaw+pitch to point local +Z along the segment; the box is then a
          // stick of half-length len/2.
          const float yaw = std::atan2(d.x, d.z);
          const float pitch =
              -std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z));
          const Quat q = QuatMul(QuatAxisAngle({0, 1, 0}, yaw),
                                 QuatAxisAngle({1, 0, 0}, pitch));
          DebugBox bx{};
          const Vec3 mid = (a + b) * 0.5f;
          bx.pos[0] = mid.x; bx.pos[1] = mid.y; bx.pos[2] = mid.z;
          bx.half[0] = halfW; bx.half[1] = halfW; bx.half[2] = len * 0.5f;
          bx.quat[0] = q.x; bx.quat[1] = q.y; bx.quat[2] = q.z; bx.quat[3] = q.w;
          bx.color = col;
          dbg.push_back(bx);
        };
        for (uint32_t i = 0; i < mobs.MobCount(); i++) {
          const uint64_t mid = mobs.MobIdAt(i);
          if (!mobs.IsAlive(mid)) continue;   // a corpse's last plan is not one
          const ai::Brain* br = mobs.MobBrain(mid);
          if (br == nullptr || br->profile < 0) continue;
          const ai::Profile* pf = mobs.Behaviors().At(br->profile);
          if (pf == nullptr) continue;
          const Vec3 o = mobs.MobOrigin(mid);
          const Vec3 foot{o.x, o.y, o.z};
          // path polyline, from the mob through every remaining waypoint
          Vec3 prev = foot;
          for (size_t w = br->path.cursor; w < br->path.pts.size(); w++) {
            segment(prev + Vec3{0, 1, 0}, br->path.pts[w] + Vec3{0, 1, 0}, 0.18f,
                    pf->color);
            prev = br->path.pts[w];
          }
          if (!br->hasTarget) continue;
          // line to the target: solid while it can SEE it, and that is the
          // distinction worth showing — a mob heading for a remembered position
          // looks identical to one that is tracking you.
          segment(foot + Vec3{0, 6, 0}, br->targetPos, br->visible ? 0.3f : 0.12f,
                  br->visible ? 0xE0FFFFFFu : 0x60FFFFFFu);
          if (!ui.showAiRing || pf->movement.rangeMax <= 0) continue;
          // the engagement band, as two dashed rings about the TARGET — the
          // band is a property of the distance between them, so drawing it
          // around the thing being circled is what makes "it is holding its
          // range" visible instead of inferred.
          for (int k = 0; k < 24 && dbg.size() < kMaxDebugBoxes; k++) {
            const float a0 = (float)k * (6.2831853f / 24.0f);
            for (int e = 0; e < 2; e++) {
              const float r = e == 0 ? pf->movement.rangeMin
                                     : pf->movement.rangeMax;
              if (r <= 0) continue;
              DebugBox bx{};
              bx.pos[0] = br->targetPos.x + std::sin(a0) * r;
              bx.pos[1] = br->targetPos.y - 6.0f;
              bx.pos[2] = br->targetPos.z + std::cos(a0) * r;
              bx.half[0] = bx.half[1] = bx.half[2] = 0.35f;
              bx.quat[3] = 1.0f;
              bx.color = e == 0 ? 0x80FF4040u : 0x8040FF40u;
              dbg.push_back(bx);
            }
          }
        }
      }
      const uint32_t debugBoxCount = (uint32_t)dbg.size();
      // ---- THE POUR POINT (game/container.h ContainerPourPoint) -----------
      // A filled vessel in the hands shows where its stream goes: a small,
      // mostly transparent sphere on the crosshair a metre in front of the
      // character, or on whatever is in the way -- `pourAim`, the same point
      // the tick pours at. It rides
      // the debugBoxes upload as ONE record after the overlay's boxes (so the
      // overlay's count excludes it) and has its own DEPTH-TESTED pipeline
      // (Simulation::DrawPourMarker): the ground in front of it hides it.
      uint32_t pourMarkerAt = UINT32_MAX;
      // `pourAimValid` was decided BEFORE the ticks, and a tick can empty the
      // slot since (a throw, a pour to the last drop), so re-read it here.
      const ItemStack* vsM =
          pourAimValid && vesselHand >= 0
              ? &kit.equip.InHand(HandAt(vesselHand))
              : nullptr;
      const ItemDef* vdefM = vsM ? items.Of(*vsM) : nullptr;
      // APPLY MODE (TB_APPLY): no stream, so no pour point. The marker sits
      // on the SKIN the brush would take -- the tick's own pick
      // (session.cpp: CastRayBody -> FindOwner -> PickBody) from this frame's
      // eye -- and the HUD names whose it is.
      ui.applyTarget.clear();
      bool applyMarked = false;
      if (ui.applyShown && vesselHand >= 0) {
        const ItemDef* adef = items.Of(kit.equip.InHand(HandAt(vesselHand)));
        if (adef && adef->IsContainer()) {
          static std::vector<uint64_t> ownA;
          ownA.clear();
          avatar.AppendLiveLimbBodies(ownA);
          const Vec3 rd = cam.Forward().normalized();
          const float maxT = std::max(0.0f, (player.EyePos() - eye).dot(rd)) +
                             adef->container.pourRange + MetresToCells(1.0f);
          float frac = 1.0f;
          const uint64_t body = phys.CastRayBody(eye, rd, maxT, frac, ownA);
          Mob* tm = body ? mobs.FindOwner(body) : nullptr;
          if (tm && avatar.Spawned() && tm->Id() == avatar.Id()) tm = nullptr;
          const MobSystem::BodyRayHit hit =
              tm ? mobs.PickBody(tm->Id(), eye, rd, maxT) : MobSystem::BodyRayHit{};
          if (hit.hit) {
            const MobDef* md = tm->Def();
            ui.applyTarget = std::string(tm->Alive() ? "" : "dead ") +
                             (md ? md->name : std::string("creature"));
            if (md && hit.limb >= 0 && hit.limb < (int)md->limbs.size())
              ui.applyTarget += " - " + md->limbs[hit.limb].name;
            if (dbg.size() < kMaxDebugBoxes) {
              DebugBox b{};
              b.pos[0] = hit.pos.x; b.pos[1] = hit.pos.y; b.pos[2] = hit.pos.z;
              b.half[0] = b.half[1] = b.half[2] =
                  std::max(MetresToCells(0.02f), ui.pourRadius);
              b.quat[3] = 1.0f;
              b.color = 0x6060FFA0u;  // pale green, mostly clear (0xAABBGGRR)
              pourMarkerAt = (uint32_t)dbg.size();
              dbg.push_back(b);
            }
          }
        }
        applyMarked = true;
      }
      if (!applyMarked && vdefM && vdefM->IsContainer() && vsM->Filled() &&
          dbg.size() < kMaxDebugBoxes) {
        static std::vector<uint64_t> ownM;
        ownM.clear();
        avatar.AppendLiveLimbBodies(ownM);
        const Vec3 at = ContainerPourPoint(
            *vdefM, eye, player.EyePos(), cam.Forward(),
            [&](IVec3 c) { return world.KindAt(c, classOf); }, phys, ownM);
        DebugBox b{};
        b.pos[0] = at.x; b.pos[1] = at.y; b.pos[2] = at.z;
        b.half[0] = b.half[1] = b.half[2] = MetresToCells(0.02f);  // radius
        b.quat[3] = 1.0f;
        b.color = 0x60FFF0E0u;  // pale, mostly clear (0xAABBGGRR)
        pourMarkerAt = (uint32_t)dbg.size();
        dbg.push_back(b);
      }
      if (!dbg.empty())
        ctx.queue.WriteBuffer(world.debugBoxes, 0, dbg.data(),
                              dbg.size() * sizeof(DebugBox));

      // rigid bodies: debris takes slots [0, D), mob limbs stack after —
      // instances rebuild when either side changes (slot bases shift),
      // transforms are cheap and refresh per frame
      // Damaged micro bodies edited their bricks (copy-on-write) this tick, so
      // the shared pool has to reach the GPU before the march reads it. One
      // upload per tick regardless of how many bodies were hit — the flag is
      // set by every edit and cleared by UploadMicroBodies itself, which also
      // sends only the WORD RANGES that changed (per-voxel burning dirties the
      // pool every tick, and the whole pool is 4 MiB).
      if (mbSet.dirty) sim.UploadMicroBodies(ctx.queue, mbSet);
      BodyRegistry bodyReg(debris, mobs, &avatar, &mbSet);
      const auto bodyT0 = std::chrono::steady_clock::now();
      // WHO ELSE IS HOLDING MY ARM. One index sweep while everything is
      // healthy, a named report the moment two entities point at one brick
      // record or a holder is left pointing at a freed one — the owner-visible
      // symptom of either is a limb wearing another creature's shape.
      // Rate-limited to one line per distinct fault and mirrored to
      // build/microbody_audit.log, because this fires while somebody is
      // playing and a windowed session's stderr goes nowhere.
      bodyReg.AuditMicroModels();
      // Build -> commit into the arena -> send only the changed ranges. Used
      // by the main view and, twice more, by the inventory portrait.
      auto commitBodyInstances = [&] {
        const auto c0 = std::chrono::steady_clock::now();
        bodyReg.BuildInstances(bodyInstScratch);
        bodyReg.BuildHandles(bodyHandles);
        const auto c1 = std::chrono::steady_clock::now();
        g_bodyStats.listUs += std::chrono::duration<double, std::micro>(c1 - c0).count();
        if (bodyInstLegacy) {
          // The A/B arm: the whole list, every time (see BodyInstLegacyArm).
          if (!bodyInstScratch.empty())
            ctx.queue.WriteBuffer(world.bodyInstances, 0, bodyInstScratch.data(),
                                  bodyInstScratch.size() * sizeof(BodyVoxInst));
          g_bodyStats.instBytes += bodyInstScratch.size() * sizeof(BodyVoxInst);
          bodyInstCount = (uint32_t)bodyInstScratch.size();
          g_bodyStats.builds++;
          return;
        }
        bodyArena.Commit(bodyInstScratch, bodyHandles);
        bodyArena.TakeUploads(bodyUploads);
        g_bodyStats.commitUs += std::chrono::duration<double, std::micro>(
                                    std::chrono::steady_clock::now() - c1).count();
        for (const auto& u : bodyUploads) {
          ctx.queue.WriteBuffer(world.bodyInstances,
                                (uint64_t)u.first * sizeof(BodyVoxInst),
                                bodyArena.Data() + u.first,
                                (size_t)(u.second - u.first) * sizeof(BodyVoxInst));
          g_bodyStats.instBytes += (uint64_t)(u.second - u.first) * sizeof(BodyVoxInst);
        }
        bodyInstCount = (uint32_t)bodyInstScratch.size();
        g_bodyStats.builds++;
      };
      if (bodyReg.AnyInstancesDirty()) commitBodyInstances();
      // Micro bodies (PLAN §C) share the slot space with the cube path: each
      // slot is claimed by exactly one of the two passes. Both scratch vectors
      // are hoisted out of the loop so a steady-state frame reuses their
      // capacity instead of allocating — clear() keeps the storage.
      microInsts.clear();
      if (bodyReg.TotalSlots() > 0) {
        bodyReg.BuildXforms(bodyXf);
        ctx.queue.WriteBuffer(world.bodyXforms, 0, bodyXf.data(),
                              bodyXf.size() * sizeof(BodyXformGpu));
        bodyReg.BuildMicroInsts(microInsts);
      }
      // Upload BEFORE the render pass opens (barrier graph §4.6): a buffer
      // write with the pass open is legal in WebGPU and illegal in Vulkan.
      uint32_t microCount = sim.UploadMicroBodyInsts(ctx.queue, microInsts);
      // Per-body frustum cull, against the camera the main pass is drawn with
      // (the jittered one: the +1 voxel margin in CullBodyRanges covers the
      // jitter either way).
      auto cullBodies = [&](const Vec3& e, const Camera& c, float asp,
                            std::vector<std::pair<uint32_t, uint32_t>>& out) {
        uint32_t n = 0;
        if (bodyInstLegacy) {  // one undivided draw of the whole list
          out.clear();
          if (bodyInstCount) out.push_back({0u, bodyInstCount});
          return bodyInstCount;
        }
        CullBodyRanges(bodyArena.Ranges(), bodyXf, e, c.Forward(), c.Right(),
                       c.Up(), std::tan(CurrentTuning().camera.fovY * 0.5f),
                       asp, out, &n);
        return n;
      };
      const uint32_t bodyInstDrawn =
          cullBodies(eye, jcam, (float)ctx.width / (float)ctx.height, bodyDraws);
      if (bodyReg.TotalSlots() > 0) {
        // Audit + instance build/upload + xforms + micro list: the whole
        // per-frame CPU half of the body render, main view only.
        const double us = std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - bodyT0).count();
        g_bodyStats.frames++;
        g_bodyStats.buildUs += us;
        g_bodyStats.buildMaxUs = std::max(g_bodyStats.buildMaxUs, us);
        g_bodyStats.cubeInstLive += bodyInstCount;
        g_bodyStats.cubeInstDrawn += bodyInstDrawn;
        g_bodyStats.cubeDraws += 2 * bodyDraws.size();
      }

      // ======================================================================
      // THE AVATAR PORTRAIT PASS — its own submit, before the frame's.
      // ======================================================================
      //
      // WHY A SEPARATE SUBMIT AND NOT A SECOND PASS IN THE SAME ENCODER:
      // world.renderUBO is ONE buffer. Two passes recorded into one command
      // buffer would both read whichever camera landed last, so the portrait
      // and the world would necessarily share a camera. Queue writes drain at
      // the head of the NEXT command buffer, so "write params, submit; write
      // params, submit" is exactly the pattern that gives each pass its own —
      // and it is the same shape the --shot harnesses already use.
      //
      // THE INSTANCE RE-UPLOAD is the other half of the same problem, one
      // level down: in first person the shared bodyInstances/micro lists have
      // the torso and legs HIDDEN, and a portrait of a floating pair of arms
      // is not a portrait. So the portrait re-uploads with an empty mask,
      // draws, and hands the real mask back for the main pass. Both uploads
      // are followed by their own submit, for the same reason as the camera.
      //
      // THE WORLD IS DRAWN BEHIND THE FIGURE (owner, 2026-09-27: to watch a
      // pour on yourself land in the real sand and water around you). It was
      // a flat clear colour; a 320x448 raymarch is a fraction of the main
      // view's pixels, and it runs only while the screen is open. The sky
      // behind it is cloudless (support.cpp: an aux view composites no deck).
      // The pour's droplets and the MPM water come with it: DrawParticles,
      // and the real fluidCount so the raymarcher draws the surface.
      //
      // THE DEATH POSE IS A THIRD RE-UPLOAD OF THE SAME SHAPE. A dead player's
      // limbs are a dead Mob's (MobSystem::AdoptDeadAvatar) and go on
      // ragdolling, settling, burning and rolling downhill; the death screen
      // must show the body as it FELL, and must still be turnable while it
      // does. So the portrait overwrites those bodies' transforms with the ones
      // the dying observer froze, draws, and hands the live ones back — the
      // same borrow-and-return the hide mask above does, one array over.
      // Matched by PHYSICS HANDLE, because the move into mobs_ (and a sever's
      // AdoptBody) carries each handle unchanged and a slot index does not
      // survive the next population change.
      // THE BENCH'S PICTURE: queue-written into the staging buffer BEFORE the
      // encoder that copies it exists (the write drains at the head of the
      // next command buffer), and submitted on its own ahead of the frame's
      // UI so Finish() has left the texture in its sampled layout by then.
      // `texReady` goes up only after the first copy, so the panel never
      // samples an image that has not been written.
      const uint32_t tbW = (uint32_t)bench.W(), tbH = (uint32_t)bench.H();
      if (bench.IsOpen() && tbW <= kBenchW && tbH <= kBenchH &&
          bench.Pixels().size() == (size_t)tbW * tbH && bench.TakeFresh()) {
        ctx.queue.WriteBuffer(benchStaging, 0, bench.Pixels().data(), (size_t)tbW * tbH * 4);
        rhi::CommandEncoder bEnc = ctx.device.CreateCommandEncoder();
        rhi::TexelCopyBuffer src;
        src.buffer = benchStaging;
        src.bytesPerRow = tbW * 4;
        src.rowsPerImage = tbH;
        rhi::TexelCopyTexture dst;
        dst.texture = benchTexture;
        bEnc.CopyBufferToTexture(src, dst, {tbW, tbH, 1});
        ctx.queue.Submit(bEnc.Finish());
        ui.alchemy.texReady = true;
      }
      if (ui.inventoryOpen && portraitCam.valid && portraitView) {
        // THE WHOLE BODY, ALWAYS — but only a mask that actually hides
        // something needs swapping (third person hides nothing). The arena
        // keeps a hidden part's span alive between toggles (kKeepCommits), so
        // the swap re-sends at most the parts whose content moved, and the
        // portrait draws only the bodies ITS camera sees (CullBodyRanges)
        // instead of every body in the world. (Was: two full rebuilds and two
        // whole-buffer uploads per frame while the screen was open.)
        const bool maskSwap = std::any_of(hide.begin(), hide.end(),
                                          [](uint8_t h) { return h != 0; });
        if (maskSwap) {
          avatar.SetHiddenParts({});
          commitBodyInstances();
        }
        // The TRANSFORMS need no re-upload FOR THE MASK: a hidden limb still
        // consumes its slot (game/mob.cpp's walk advances for every part with
        // a body, drawn or not), so the transform array is mask-independent by
        // construction. Only the instance lists change. The death pose is the
        // one thing that does re-upload them, and it puts them back below.
        bool posedDead = false;
        if (deathFrozen && deathBody.have && !bodyXf.empty()) {
          // EVERY slot, not the debris range: the corpse is a dead Mob in
          // mobs_ now (MobSystem::AdoptDeadAvatar) and draws through mob
          // slots, while a limb severed before death is still debris.
          std::vector<uint64_t> slotHandles;
          bodyReg.BuildHandles(slotHandles);
          const uint32_t nSlots = std::min<uint32_t>(
              (uint32_t)slotHandles.size(), (uint32_t)bodyXf.size());
          for (uint32_t s = 0; s < nSlots; s++) {
            const uint64_t h = slotHandles[s];
            if (!h) continue;
            for (const auto& fl : deathBody.limbs) {
              if (fl.body != h) continue;
              bodyXf[s] = fl.xf;
              posedDead = true;
              break;
            }
          }
          if (posedDead)
            ctx.queue.WriteBuffer(world.bodyXforms, 0, bodyXf.data(),
                                  bodyXf.size() * sizeof(BodyXformGpu));
        }
        microInsts.clear();
        bodyReg.BuildMicroInsts(microInsts);
        const uint32_t pMicro = sim.UploadMicroBodyInsts(ctx.queue, microInsts);
        // bodyXf carries the death pose here when there is one, so the cull
        // tests the pose the portrait draws.
        std::vector<std::pair<uint32_t, uint32_t>> pDraws;
        const uint32_t pDrawn = cullBodies(portraitCam.eye, portraitCam.cam,
                                           portraitCam.aspect, pDraws);

        // auxView: the portrait is drawn INSIDE the main view's frame, so it
        // must not touch the main view's cloud history or weather ease (see
        // WriteRenderParams in test/support.h).
        WriteRenderParams(ctx.queue, world, portraitCam.eye, portraitCam.cam,
                          portraitCam.aspect, /*shadows=*/true, (float)now,
                          /*fogDensity=*/0.0f, (float)kPortraitH,
                          kPortraitLightTick, fluidCount,
                          /*frameFrac=*/0.0f, /*extraFlags=*/0u, kPortraitW,
                          kPortraitH, /*auxView=*/true);
        rhi::CommandEncoder pEnc = ctx.device.CreateCommandEncoder();
        // Near-black indigo, the panel's own deepest tone, so the portrait
        // reads as an inset rather than as a hole punched in the sheet.
        const float kPortraitClear[4] = {0.055f, 0.048f, 0.086f, 1.0f};
        rhi::RenderPass pRp =
            sim.BeginAuxRenderPass(pEnc, portraitView, ctx.surfaceFormat,
                                   kPortraitW, kPortraitH, kPortraitClear);
        sim.DrawWorld(pRp);
        sim.DrawParticles(pRp);
        if (CurrentTuning().render.fluidSurface < 0.5f) sim.DrawFluid(pRp, fluidCount);
        sim.DrawBodyRanges(pRp, pDraws);
        sim.DrawMicroBodies(pRp, pMicro);
        sim.DrawSprites(pRp, (uint32_t)sprv.size());
        pRp.End();
        // One line, on the captured frames only: "the portrait drew N bodies
        // from HERE". It is what turns "the frame is empty" from a guess into
        // a reading — an empty portrait is either no instances, a camera not
        // pointed at the body, or a sprite drawn over the top, and this
        // separates the first two from the third in a single run. (It was the
        // third: a 9-slice frame whose middle slice was opaque.)
        if (g_shotInventory && (frameCounter == kShotInvGearFrame ||
                                frameCounter == kShotInvCaptureFrame ||
                                frameCounter == kShotInvGrimoireFrame ||
                                frameCounter == kShotInvGraphFrame ||
                                frameCounter == kShotInvLootFrame ||
                                frameCounter == kShotInvDeathShot ||
                                frameCounter == kShotInvDeathTurnShot))
          std::printf("--shot-inventory: portrait cube=%zu micro=%u "
                      "eye=(%.1f %.1f %.1f) target=(%.1f %.1f %.1f)%s\n",
                      (size_t)pDrawn, pMicro, portraitCam.eye.x, portraitCam.eye.y,
                      portraitCam.eye.z, portraitCam.target.x,
                      portraitCam.target.y, portraitCam.target.z,
                      posedDead ? "  [death pose]" : "");
        ctx.queue.Submit(pEnc.Finish());

        // Put the world back: the real hide mask, its instances, the corpse's
        // LIVE transforms, and the player's own camera. The mask change
        // re-dirties the avatar, so the rebuild below is a genuine requirement
        // rather than a precaution.
        if (posedDead) {
          bodyReg.BuildXforms(bodyXf);
          ctx.queue.WriteBuffer(world.bodyXforms, 0, bodyXf.data(),
                                bodyXf.size() * sizeof(BodyXformGpu));
        }
        if (maskSwap) {
          avatar.SetHiddenParts(hide);
          commitBodyInstances();
        }
        // Re-cull the main view: the swap may have moved spans (a repack).
        cullBodies(eye, jcam, (float)ctx.width / (float)ctx.height, bodyDraws);
        microInsts.clear();
        bodyReg.BuildMicroInsts(microInsts);
        microCount = sim.UploadMicroBodyInsts(ctx.queue, microInsts);
        writeMainRenderParams();
      }

      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      // --telemetry: a GPU timestamp pair around EACH DRAW of the render pass,
      // billed through kPerfRenderSpans (perfnodes.h) to the box that issued
      // it. This used to be ONE pair around the whole pass, which told the
      // Performance tab "the GPU frame is the render pass" and nothing else.
      //
      // Written INSIDE the dynamic-rendering scope, which is legal: the spec
      // allows vkCmdWriteTimestamp2 inside a render pass instance. What is
      // not allowed inside one is vkCmdResetQueryPool and
      // vkCmdCopyQueryPoolResults — the query set is reset at creation and by
      // EncodeResolve after each read, both outside — so the old comment's
      // "not legal inside a render pass" was about the resolve, not the write.
      //
      // Both ends of a span are ALL_COMMANDS (`bottom`), not TOP_OF_PIPE for
      // the start: draws in one pass overlap in the pipeline, and a
      // top-of-pipe start stamp can land before the PREVIOUS draw's fragments
      // have retired. Stamping when all prior commands complete makes span k
      // "from the end of draw k-1 to the end of draw k", which is the only
      // sequential attribution a shared pipeline can honestly give.
      //
      // The shadow-cache resolve runs BEFORE the pass and is timed by the pass
      // table (its `shadowCache` row); it is no longer inside the raymarch
      // number, so the two rows no longer double-count it.
      const bool liveRenderTimed =
          liveTimed && (telemetry.HasClient() || g_harnessFrames > 0);
      struct LiveSpan { uint32_t b = 0, e = 0; bool on = false; };
      auto spanBegin = [&](const char* name) {
        LiveSpan sp;
        if (liveRenderTimed &&
            liveRenderTimer.AllocPassPair(name, sp.b, sp.e)) {
          sp.on = true;
          enc.WriteTimestamp(liveRenderTimer.NativeQuerySet(), sp.b, true);
        }
        return sp;
      };
      auto spanEnd = [&](const LiveSpan& sp) {
        if (sp.on) enc.WriteTimestamp(liveRenderTimer.NativeQuerySet(), sp.e, true);
      };
      sim.EncodeShadowResolve(enc);
      if (offscreen && (!scaledView || scaledW != renderW || scaledH != renderH)) {
        scaledW = renderW;
        scaledH = renderH;
        scaledTex = ctx.device.CreateTexture(
            {renderW, renderH, 1}, ctx.surfaceFormat,
            rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
            "worldScaled");
        scaledView = scaledTex.CreateView();
        std::printf("render scale %.2f%s: world at %ux%u, window %ux%u\n",
                    renderScale, taaOn ? " (taa)" : "", renderW, renderH,
                    ctx.width, ctx.height);
      }
      if (taaOn) {
        // A toggle or a scale change invalidates the accumulator: the history
        // was built at a different resolution or with a jitter sequence that
        // was not running. EnsureTaa resets on a size change by itself; this
        // covers the two that keep the size and change the meaning.
        if (!taaWasOn || taaScaleWas != renderScale) sim.ResetTaa();
        sim.EnsureTaa(renderW, renderH, ctx.width, ctx.height);
        // Uploaded HERE, before any render pass opens, for the reason
        // UploadMicroBodyInsts is hoisted out of DrawMicroBodies: a buffer
        // write issued with a rendering scope open is legal in WebGPU and
        // ILLEGAL in Vulkan (barrier_graph §4.6). Everything it needs — the
        // jittered basis, the eye, the two knobs — is already known.
        sim.WriteTaaParams(ctx.queue, taaCam, CurrentTuning().render.taaMaxHist,
                           CurrentTuning().render.taaClamp, /*reset=*/false,
                           ctx.surfaceFormat == rhi::TextureFormat::BGRA8Unorm);
      }
      if (denoiseOn) {
        // Same rule as WriteTaaParams above: uploaded before any render pass
        // opens. The projection is the one WriteRenderParams uses.
        sim.EnsureDenoise(renderW, renderH);
        sim.WriteDenoiseParams(ctx.queue, renderW, renderH,
                               std::tan(CurrentTuning().camera.fovY * 0.5f),
                               ctx.surfaceFormat == rhi::TextureFormat::BGRA8Unorm);
      }
      taaWasOn = taaOn;
      taaScaleWas = renderScale;
      rhi::RenderPass rp =
          sim.BeginRenderPass(enc, offscreen ? scaledView : target,
                              ctx.surfaceFormat, renderW, renderH);
      {
        const LiveSpan sp = spanBegin("rm_world");
        sim.DrawWorld(rp);
        spanEnd(sp);
      }
      {
        const LiveSpan sp = spanBegin("rm_particles");
        sim.DrawParticles(rp);
        // Cube-debug mode only: in surface mode (the default) the raymarcher
        // draws the fluid as a real water surface and the cubes would z-fight
        // it.
        if (CurrentTuning().render.fluidSurface < 0.5f)
          sim.DrawFluid(rp, fluidCount);
        spanEnd(sp);
      }
      {
        const LiveSpan sp = spanBegin("rm_bodies");
        sim.DrawBodyRanges(rp, bodyDraws);
        spanEnd(sp);
      }
      {
        const LiveSpan sp = spanBegin("rm_micro");
        sim.DrawMicroBodies(rp, microCount);
        spanEnd(sp);
      }
      {
        const LiveSpan sp = spanBegin("rm_sprites");
        sim.DrawSprites(rp, (uint32_t)sprv.size());
        spanEnd(sp);
      }
      {
        const LiveSpan sp = spanBegin("rm_debug");
        sim.DrawDebugBoxes(rp, debugBoxCount);
        sim.DrawPourMarker(rp, pourMarkerAt);
        // Field arrows LAST of the world draws so they composite over
        // everything they annotate. Nothing is uploaded for them — the count
        // is the only CPU work, and at zero the draw is skipped outright
        // (rule 2's shape, applied to a debug view: off is not "cheap", it is
        // nothing). F4's cycle means at most ONE of these two is ever nonzero.
        sim.DrawWindField(rp, ui.fieldViz == UIState::kFieldVizWind
                                  ? WindDebugArrowCount(CurrentTuning())
                                  : 0u);
        sim.DrawCurrentField(rp, ui.fieldViz == UIState::kFieldVizCurrent
                                     ? CurrentDebugArrowCount()
                                     : 0u);
        spanEnd(sp);
      }
      if (offscreen) {
        // The world is done at internal resolution. Either resolve it through
        // TAA (accumulate + upscale) or blit it up (NEAREST — the voxels stay
        // square), then open a native-size pass for the UI so text and panels
        // are never scaled.
        rp.End();
        if (denoiseOn) {
          // The filter's copies and passes, all derived-barrier: see
          // Simulation::EncodeDenoise. The result lands back in scaledTex, so
          // TAA and the blit below read the filtered frame without knowing.
          const LiveSpan spd = spanBegin("rm_denoise");
          sim.EncodeDenoise(enc, scaledTex, scaledView, ctx.surfaceFormat,
                            renderW, renderH);
          spanEnd(spd);
        }
        if (taaOn) {
          // The copies MUST be outside a rendering scope — the recorder drops a
          // transfer recorded inside one, silently. `rp.End()` above is what
          // makes them legal, and BeginTaaRenderPass's own BeginRendering
          // flushes the transfer->fragment-read hazard on both buffers for us
          // (vk_record.cpp's FlushForRenderDomain walks the untracked extras
          // too), so there is no hand-written barrier here and there must not be.
          const LiveSpan spc = spanBegin("rm_taa_copy");
          sim.EncodeTaaCapture(enc, scaledTex);
          spanEnd(spc);
          rp = sim.BeginTaaRenderPass(enc, target, ctx.surfaceFormat, ctx.width,
                                      ctx.height);
          {
            const LiveSpan sp = spanBegin("rm_taa");
            sim.DrawTaa(rp);
            spanEnd(sp);
          }
          rp.End();
          taaFrameNo++;
          sim.FlipTaaPage();
        } else {
          enc.BlitTexture(scaledView, target);
        }
        rp = sim.BeginOverlayRenderPass(enc, target, ctx.surfaceFormat, ctx.width,
                                        ctx.height);
      }
      {
        const LiveSpan sp = spanBegin("rm_overlay");
        overlay.Render(rp);
        spanEnd(sp);
      }
      rp.End();
      if (liveRenderTimed && liveRenderTimer.PassesThisBuffer() > 0)
        liveRenderTimer.EncodeResolve(enc);
      // The raymarch's step counters, copied out behind the pass that wrote
      // them (measure/renderstats.h). Only with a page listening: the counters
      // accumulate regardless, and a frame nobody harvests is just a gap.
      const bool liveStatsEncoded =
          liveStatsOn && telemetry.HasClient() &&
          liveStats.Encode(enc, world, liveFrameNo);
      double tPresent0 = NowSeconds();
      ctx.queue.Submit(enc.Finish());

      // ---- --shot-inventory: the same frame again, into a file -------------
      // A presented swapchain image cannot be copied, so the frame is
      // RE-RECORDED into an offscreen target of the same size (same depth
      // cache, so nothing thrashes) and read back. The blocking readback is
      // legal here for the same reason it is in --shot: this is the last frame
      // of a harness run, not the frame path of a game.
      // --shot-spellpage names its file after the SCENE, not after the frame:
      // a gallery whose pictures are called shot_spell_148.bmp is a gallery
      // nobody can diff against the last pass.
      static char sSpellShotPath[64];
      const int spellShot =
          g_shotSpellPage ? ShotSpellSceneAt(frameCounter, false) : -1;
      if (spellShot >= 0)
        std::snprintf(sSpellShotPath, sizeof sSpellShotPath,
                      "shot_spell_%s.bmp", kShotSpellScenes[spellShot].name);
      if ((g_shotInventory && (frameCounter == kShotInvGearFrame ||
                               frameCounter == kShotInvHudFrame ||
                               frameCounter == kShotInvCaptureFrame ||
                               frameCounter == kShotInvGrimoireFrame ||
                               frameCounter == kShotInvGraphFrame ||
                               frameCounter == kShotInvLootFrame ||
                               frameCounter == kShotInvDeathShot ||
                               frameCounter == kShotInvDeathTurnShot)) ||
          spellShot >= 0 || g_shotJumpPath) {
        const char* shotPath =
            spellShot >= 0                       ? sSpellShotPath
            :
            g_shotJumpPath                       ? g_shotJumpPath
            : frameCounter == kShotInvGearFrame  ? "screenshot_inventory.bmp"
            : frameCounter == kShotInvHudFrame   ? "screenshot_hud.bmp"
            : frameCounter == kShotInvCaptureFrame
                ? "screenshot_inventory_health.bmp"
            : frameCounter == kShotInvGrimoireFrame
                ? "screenshot_inventory_grimoire.bmp"
            : frameCounter == kShotInvGraphFrame
                ? "screenshot_inventory_graph.bmp"
            : frameCounter == kShotInvLootFrame
                ? "screenshot_inventory_loot.bmp"
            : frameCounter == kShotInvDeathShot
                ? "screenshot_inventory_death.bmp"
                : "screenshot_inventory_death_turn.bmp";
        const uint32_t W = ctx.width, H = ctx.height;
        rhi::Texture shotTex = ctx.device.CreateTexture(
            {W, H, 1}, ctx.surfaceFormat,
            rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
            "inventoryShot");
        rhi::CommandEncoder senc = ctx.device.CreateCommandEncoder();
        sim.EncodeShadowResolve(senc);
        rhi::RenderPass srp = sim.BeginRenderPass(
            senc, shotTex.CreateView(), ctx.surfaceFormat, W, H);
        sim.DrawWorld(srp);
        sim.DrawBodyRanges(srp, bodyDraws);
        sim.DrawMicroBodies(srp, microCount);
        sim.DrawPourMarker(srp, pourMarkerAt);
        overlay.RenderRecorded(srp);
        srp.End();
        ctx.queue.Submit(senc.Finish());
        ctx.WaitIdle();
        // The copy goes in its OWN encoder, submitted after the render has
        // retired — the shape RunShots::grab uses. Folding it into the render
        // encoder is legal in the headless harnesses and produced a black
        // image here.
        rhi::Buffer shotBuf = CreateBuffer(
            ctx.device, (uint64_t)W * H * 4,
            rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
            "inventoryShotRead");
        rhi::CommandEncoder cenc = ctx.device.CreateCommandEncoder();
        rhi::TexelCopyTexture src{};
        src.texture = shotTex;
        rhi::TexelCopyBuffer dst{};
        dst.buffer = shotBuf;
        dst.bytesPerRow = W * 4;
        dst.rowsPerImage = H;
        cenc.CopyTextureToBuffer(src, dst, {W, H, 1});
        ctx.queue.Submit(cenc.Finish());
        ctx.WaitIdle();
        std::vector<uint8_t> px((size_t)W * H * 4, 0);
        if (rhi::ReadBufferBlocking(ctx.device, shotBuf, 0, px.data(),
                                    px.size())) {
          // The swapchain is BGRA and WriteBmpFile expects RGBA; swap in place
          // rather than teaching the writer about formats, which every other
          // caller would then have to care about.
          if (ctx.surfaceFormat == rhi::TextureFormat::BGRA8Unorm)
            for (size_t i = 0; i < px.size(); i += 4)
              std::swap(px[i], px[i + 2]);
          // A byte sum, because "the file was written" is not the claim that
          // matters — an all-zero image writes just as successfully as a real
          // one, and a harness whose failure mode is a black rectangle needs
          // to say so out loud rather than print "wrote".
          uint64_t sum = 0;
          for (uint8_t b : px) sum += b;
          if (WriteBmpFile(shotPath, px, W, H)) {
            std::printf("wrote %s (%ux%u, pixel sum %llu%s)", shotPath, W, H,
                        (unsigned long long)sum,
                        sum == 0 ? " *** ALL BLACK ***" : "");
            // THE PICTURE SAYS WHICH POSE IT IS. A file called "fall" proves
            // nothing on its own — the whole claim of the air pose is that the
            // shape is a function of vel.y, so the vy the shutter fired at
            // belongs next to the filename or the harness is only asserting
            // that four BMPs exist.
            if (g_shotJumpPath)
              std::printf(" | vy %+.2f m/s, air %.2f, land %.2f", g_shotJumpVy,
                          avatar.AirPoseWeight(), avatar.AirPoseLand());
            std::printf("\n");
          }
        }
      }

      ctx.Present();
      if (liveRenderTimed) liveRenderTimer.KickDeferred(ctx, liveFrameNo);
      if (liveStatsEncoded) liveStats.Kick(ctx);
      {
        using sandvox::PerfScope;
        // Through the same accumulator as every other scope, rather than
        // straight into liveSample: `--frames` collects the identical table
        // with no telemetry client attached, and two write paths into one
        // sample is how the two stopped agreeing in the first place.
        //
        // Encode only — the acquire wait above is not in this span.
        sandvox::PerfScopeAdd(PerfScope::RenderCpu, tRender0, tPresent0);
        // The swapchain wait (AcquireFrame) plus the present itself. Under FIFO
        // this is the whole vsync/GPU-completion stall and is normally the
        // largest CPU row on the page; that is the frame finishing early, not
        // the CPU being busy.
        sandvox::PerfScopeAdd(PerfScope::Present, tAcquire0, tRender0);
        sandvox::PerfScopeAdd(PerfScope::Present, tPresent0, NowSeconds());
      }
    }
    // tAcquire0, not tRender0: this harness number has always been "acquire +
    // encode + present", and the scope split above must not silently redefine
    // it into encode-only.
    if (g_harnessFrames > 0) g_harnessRenderMs += (NowSeconds() - tAcquire0) * 1000.0;
    // ---- fps cap (render.fpsCap) ----
    // A sleep to the next deadline, billed to `present`: it is a wait, not
    // work, exactly like the vsync block. Deadlines advance by the period, not
    // by "now + period", so the cap does not drift under jitter; a frame that
    // arrives more than a period late resets rather than trying to catch up.
    {
      const float cap = CurrentTuning().render.fpsCap;
      if (cap > 0.0f) {
        const double period = 1.0 / (double)cap;
        const double t0 = NowSeconds();
        double deadline = fpsCapLast + period;
        if (deadline < t0 - period) deadline = t0;
        if (deadline > t0) {
          const double coarse = deadline - t0 - 0.0015;
          if (coarse > 0.0)
            std::this_thread::sleep_for(std::chrono::duration<double>(coarse));
          while (NowSeconds() < deadline) std::this_thread::yield();
        }
        fpsCapLast = deadline;
        sandvox::PerfScopeAdd(sandvox::PerfScope::Present, t0, NowSeconds());
      } else {
        fpsCapLast = 0.0;
      }
    }
    {
      sandvox::PerfSpan spanRb(sandvox::PerfScope::Readback);
      ctx.ProcessEvents();  // pumps MapAsync callbacks (mirror updates)
    }
    telemetry.Poll();
    // The inbound half of the socket (PLAN_environment_truth P-A): a page
    // that just attached gets the environment stamp once; a page that asks
    // for a reload gets exactly what F7 does, on the next frame.
    if (telemetryEnabled) {
      if (telemetry.TakeNewClients() > 0) {
        const std::string m = envStampMessage();
        telemetry.SendText(m.c_str(), (int)m.size());
      }
      for (std::string cmd; telemetry.PopCommand(cmd);) {
        if (cmd.find("apply-environment") != std::string::npos) {
          // EXACTLY F7: the F5 half re-reads tuning.json (world.mapLayer)
          // and rebuilds the kernels before the regen half reloads the
          // environment (the map, and the edit layer map.json names). A
          // regen alone kept the map the game booted with.
          ui.reloadShaders = true;
          ui.regenWorld = true;
        } else if (cmd.find("env-stamp") != std::string::npos) {
          const std::string m = envStampMessage();
          telemetry.SendText(m.c_str(), (int)m.size());
        }
      }
    }

    // ---- CLOSE THE FRAME'S CPU ACCOUNTING -------------------------------
    //
    // ONE drain, ONE residual, and both consumers read the same array. The
    // Performance tab and the `--frames` harness used to be two write paths
    // into two different pictures of the same frame; they are one now, which
    // is the only reason a non-interactive run can answer "which bar spiked".
    double frameScope[sandvox::kPerfScopeCount] = {};
    uint32_t frameStalls = 0;
    uint32_t frameDeclines = 0;
    sandvox::SnapshotStallStats frameStallStats{};
    const double frameWallMs = (NowSeconds() - now) * 1000.0;
    // The tail of the startup timeline: the first frames are where the lazily
    // created graphics pipelines, the far-field drain and the first ticks land.
    startupFrame++;
    if (startupFrame <= 3 || startupFrame == 10 || startupFrame == 30 ||
        startupFrame == 60) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "frame %llu presented",
                    (unsigned long long)startupFrame);
      StartupMark(buf);
    }
    if (sandvox::PerfScopesOn()) {
      // Zeroes as it copies, so a span straddling this point cannot be counted
      // into two frames.
      sandvox::PerfScopesDrain(frameScope);
      double sum = 0;
      for (int i = 0; i < sandvox::kPerfScopeCount; i++) sum += frameScope[i];
      // THE RESIDUAL, and it is named as one now. Whatever wall clock no span
      // claimed goes to `other`. This used to be added to `input`, which turned
      // every unmeasured span in the engine into a report that the player was
      // polling the keyboard too hard. A large or spiky `other` is a TODO —
      // add a span where the time is going — not a system to go optimise.
      frameScope[(int)sandvox::PerfScope::Other] +=
          std::max(0.0, frameWallMs - sum);
      frameStalls = TakeSnapshotStalls();
      frameDeclines = TakeReadbackDeclines();
      frameStallStats = TakeSnapshotStallStats();
    }
    if (g_harnessFrames > 0 && frameCounter > 60) {
      for (int i = 0; i < sandvox::kPerfScopeCount; i++) {
        g_frameScopeSum[i] += frameScope[i];
        if (frameScope[i] > g_frameScopeMax[i]) g_frameScopeMax[i] = frameScope[i];
        g_frameScopeSeries[i].push_back(frameScope[i]);
      }
      g_frameSnapStalls += frameStalls;
      if (frameStalls) g_frameSnapStallFrames++;
      g_frameRbDeclines += frameDeclines;
      g_frameStallStats.stalls += frameStallStats.stalls;
      g_frameStallStats.refusedArm += frameStallStats.refusedArm;
      g_frameStallStats.issuedArm += frameStallStats.issuedArm;
      g_frameStallStats.mapArm += frameStallStats.mapArm;
      g_frameStallStats.idleArm += frameStallStats.idleArm;
      g_frameStallStats.idleFutile += frameStallStats.idleFutile;
      g_frameStallStats.proceedStale += frameStallStats.proceedStale;
      g_frameStallStats.mapWaits += frameStallStats.mapWaits;
      for (int i = 0; i < 10; i++)
        g_frameStallStats.gapHist[i] += frameStallStats.gapHist[i];
      g_frameStallStats.gapMax =
          std::max(g_frameStallStats.gapMax, frameStallStats.gapMax);
      g_frameStallStats.mapArmMs += frameStallStats.mapArmMs;
      g_frameStallStats.idleArmMs += frameStallStats.idleArmMs;
    }

    // ---- LIVE TELEMETRY: close the frame and send it --------------------
    //
    // GPU numbers arrive two or three frames late, so a frame is held in
    // `livePending` until either its timestamps land or it ages out. Sending a
    // frame early and "correcting" it later is not an option — the page has
    // already drawn it, and a bar that retroactively grows is worse than one
    // that is honestly marked as having no GPU data.
    if (telemetry.HasClient() || g_harnessFrames > 0) {
      using sandvox::PerfScope;
      liveSample.frame = liveFrameNo;
      liveSample.wallMs = frameWallMs;
      // The drain and the residual already happened above, for both consumers.
      for (int i = 0; i < sandvox::kPerfScopeCount; i++)
        liveSample.cpuMs[i] += frameScope[i];
      // Counters, from the snapshot the pump above may just have landed.
      const WorldSnapshot& lsn = world.Snap();
      auto ctr = [&](sandvox::PerfCounter c, double v) {
        liveSample.counters[(int)c] = v;
      };
      if (lsn.valid) {
        ctr(sandvox::PerfCounter::ActiveChunks, lsn.activeChunks);
        ctr(sandvox::PerfCounter::Particles, lsn.particleCount);
        ctr(sandvox::PerfCounter::FluidParticles, lsn.fluidLive);
        ctr(sandvox::PerfCounter::PageFaults, lsn.pageFaults);
        ctr(sandvox::PerfCounter::VoxelsNonAir, (double)lsn.voxelTotal);
      }
      if (world.pages) {
        ctr(sandvox::PerfCounter::PagesResident, world.pages->PagesInUse());
        // WHY those pages are resident, not just how many
        // (docs/RESEARCH_streaming_hitch.md §6). The four `held` rows partition
        // PagesResident, so a live capture over --telemetry answers the
        // question the count alone cannot — which is the whole reason the
        // census runs on the frame path instead of behind SANDVOX_PT_DEBUG.
        const PageCensus& pc = world.pages->Census();
        if (pc.valid) {
          ctr(sandvox::PerfCounter::PagesHeldDirty, pc.rDirty + pc.rRing);
          ctr(sandvox::PerfCounter::PagesHeldMatter, pc.rFull + pc.rMatter);
          ctr(sandvox::PerfCounter::PagesHeldEmpty,
              pc.rShell + pc.rStain + pc.rWaiting + pc.rCand);
          ctr(sandvox::PerfCounter::PagesHeldOrphan, pc.rOrphan);
          ctr(sandvox::PerfCounter::PagesRetired, pc.retired);
        }
      }
      // Read-and-cleared above, so a frame that ran four ticks reports all four
      // of their stalls and the next frame starts at zero.
      ctr(sandvox::PerfCounter::SnapshotStalls, (double)frameStalls);
      ctr(sandvox::PerfCounter::ReadbackDeclined, (double)frameDeclines);
      livePending.push_back({liveFrameNo, liveSample});

      // Harvest whatever has landed and post it to the frame it belongs to.
      auto post = [&](PassTimer& t, bool isRender) {
        const uint32_t tag = t.LastFrameTag();
        for (LivePending& lp : livePending) {
          if (lp.frame != tag) continue;
          for (const PassSample& ps : t.LastFrame()) {
            // Render spans and pass rows are two namespaces with two tables;
            // both live in perfnodes.h so the --perf harness bills the same.
            // ONE lookup over both (P3-F): the tick command buffer now carries
            // hand-written spans too — the page fills and the readback copies —
            // so `isRender` is no longer the same question as "which table".
            (void)isRender;
            const int node = sandvox::PerfNodeForTimedName(ps.name);
            if (node < 0) continue;
            lp.s.gpuMs[node] += (double)ps.ns / 1e6;
            lp.s.gpuValid = true;
            if (g_harnessFrames > 0) lp.passes.push_back({ps.name, (double)ps.ns / 1e6});
          }
          break;
        }
      };
      // The raymarch's step counters land on the same fence as the render
      // spans (same command buffer), so harvest them FIRST: the sample is sent
      // the moment gpuValid flips, and a counter arriving one poll later would
      // be posted to a frame already gone.
      if (liveStatsOn) {
        sandvox::RenderStatsRing::Frame rf;
        while (liveStats.Poll(rf)) {
          for (LivePending& lp : livePending) {
            if (lp.frame != rf.frame) continue;
            // Slot k is PerfCounter::RmPixels + k: slot 0 the pixel
            // denominator, slots 1.. the step / pixel-class rows in order
            // (world.h kRenderStat* and perfnodes.h say the same thing).
            for (uint32_t k = 0; k < kRenderStatSlots; k++)
              lp.s.counters[(int)sandvox::PerfCounter::RmPixels + k] =
                  rf.counters[k];
            break;
          }
        }
      }
      if (liveTimed && liveTimer.PollDeferred(ctx) > 0) post(liveTimer, false);
      if (liveTimed && liveRenderTimer.PollDeferred(ctx) > 0)
        post(liveRenderTimer, true);

      // Send anything that is complete or three frames old, oldest first.
      while (!livePending.empty() &&
             (livePending.front().s.gpuValid ||
              liveFrameNo - livePending.front().frame >= 3)) {
        if (telemetry.HasClient()) telemetry.BroadcastSample(livePending.front().s);
        // The harness keeps the GPU rows of every frame that got them; a
        // frame whose queries never resolved is left out rather than logged
        // as zero (same rule as the page's gpuValid).
        // Same warm-up exclusion as the CPU table (frame > 60): the first
        // frames hold the startup horizon refill (262k far entries at
        // kFarListCap per tick) and would own every GPU p99 otherwise.
        if (g_harnessFrames > 0 && livePending.front().s.gpuValid &&
            livePending.front().frame > 60) {
          for (int n = 0; n < sandvox::kPerfNodeCount; n++)
            g_frameGpuSeries[n].push_back(livePending.front().s.gpuMs[n]);
          // Per pass: sum this frame's spans by name, then append one
          // value per pass (padding to the frame count happens at print).
          std::map<std::string, double> perPass;
          for (const auto& pr : livePending.front().passes) perPass[pr.first] += pr.second;
          for (const auto& pr : perPass) {
            std::vector<double>& v = g_frameGpuPassSeries[pr.first];
            v.resize(g_frameGpuFrames, 0.0);
            v.push_back(pr.second);
          }
          g_frameGpuFrames++;
        }
        livePending.erase(livePending.begin());
      }
      liveSample = sandvox::PerfSample{};
    } else if (!livePending.empty()) {
      livePending.clear();   // browser went away; do not hoard frames
    }
    liveFrameNo++;
  }

  // ---- M9.2-C: THE NET REPORT -------------------------------------------
  //
  // ONE LINE, fixed field order, printed whenever this process was networked
  // at all (not only under --frames, so a windowed session that quits also
  // says what happened). It is the two-process smoke's whole acceptance:
  // batches in BOTH directions proves the link and the pacer ran, `stalls`
  // says how often the peer was late, `maxLag` should sit at D in the steady
  // state (that is what D buys), and `late` must be 0 — a batch arriving for
  // a tick already run means the pre-send or the label arithmetic is wrong.
  if (netRoleBoot != NetRole::None) {
    const uint64_t bIn =
        netBytesInPrev + (link ? link->Stats().bytesIn : 0);
    const uint64_t bOut =
        netBytesOutPrev + (link ? link->Stats().bytesOut : 0);
    // M9.3-B adds the op fields. `opsSent`/`opsRecv` > 0 in BOTH directions
    // is the two-process smoke's proof that the exchange ran at all;
    // `cellsDropped` is the residency filter doing its job (a peer editing a
    // chunk this machine does not hold), and `pollStalls` counts the frames
    // the silence timer refused to blame on the peer.
    std::printf("net: role=%s batchesSent=%llu batchesRecv=%llu stalls=%llu "
                "maxLag=%d late=%llu disconnects=%llu bytesIn=%llu "
                "bytesOut=%llu opsSent=%llu opsRecv=%llu cellsDropped=%llu "
                "mergedMax=%llu pollStalls=%llu\n",
                netRoleBoot == NetRole::Host ? "host" : "client",
                (unsigned long long)netBatchesSent,
                (unsigned long long)netBatchesRecv,
                (unsigned long long)netStalls, netMaxLag,
                (unsigned long long)netLate,
                (unsigned long long)netDisconnects,
                (unsigned long long)bIn, (unsigned long long)bOut,
                (unsigned long long)netOpsSent,
                (unsigned long long)netOpsRecv,
                (unsigned long long)(netCellsDropped +
                                     opsync.Q().droppedCells),
                (unsigned long long)std::max(netMergedMax,
                                             opsync.Q().mergedMax),
                (unsigned long long)netPollStalls);
    // ---- M9.3-C: THE CONVERGENCE LINE -----------------------------------
    //
    // A SECOND line rather than more fields on the first, because it answers
    // a different question: the first says "did the two machines talk", this
    // says "did they agree, and when they did not, did they converge".
    //
    // The acceptance is a SHAPE, not a total (CLAUDE.md rule 6): `applied` > 0
    // on the side that drifted, `busy` counted separately from `notResident`
    // (the first is normal back-off, the second is a window that moved), and
    // the per-hash-tick mismatch series below, which must spike and return to
    // zero. `drifts` is the denominator: without it, "0 syncs" and "nothing
    // ever diverged" are the same number.
    {
      const uint64_t bs = netSync.blocksSent + chunksync.hashBlocksSent;
      const uint64_t br = netSync.blocksRecv + chunksync.hashBlocksRecv;
      const uint64_t mm = netSync.mismatches + chunksync.blockMismatches;
      const uint64_t cm = netSync.chunkMismatches + chunksync.chunkMismatches;
      const uint64_t dr = netSync.drills + chunksync.drillsSent;
      const uint64_t rq = netSync.requests + chunksync.requestsSent;
      const uint64_t ap = netSync.applied + chunksync.syncsApplied;
      const uint64_t by = netSync.busy + chunksync.busyRecv;
      const uint64_t nr = netSync.notResident + chunksync.syncsRefusedNotResident;
      const uint64_t sb = netSync.bytes + chunksync.bytesShipped;
      std::printf("net: hashBlocks sent=%llu recv=%llu | mismatches blocks=%llu "
                  "chunks=%llu | drills=%llu | chunkRequests=%llu | chunkSyncs "
                  "applied=%llu refused(busy)=%llu refused(notResident)=%llu | "
                  "syncBytes=%llu | drifts=%llu\n",
                  (unsigned long long)bs, (unsigned long long)br,
                  (unsigned long long)mm, (unsigned long long)cm,
                  (unsigned long long)dr, (unsigned long long)rq,
                  (unsigned long long)ap, (unsigned long long)by,
                  (unsigned long long)nr, (unsigned long long)sb,
                  (unsigned long long)netDriftOps);
      // THE SERIES, one entry per HashBlocks this machine received: the hash
      // tick, how many blocks disagreed, and how many were compared at all.
      // "3 of 40, then 0 of 40" is a repair; "3, 3, 3" is a repair that never
      // landed, and no total can tell those apart.
      std::vector<net::ChunkSync::MismatchPoint> ser = netSync.series;
      ser.insert(ser.end(), chunksync.series.begin(), chunksync.series.end());
      std::printf("net: mismatch series (hashTick:bad/compared):");
      if (ser.empty()) std::printf(" (none — no publish was ever compared)");
      for (const net::ChunkSync::MismatchPoint& mp : ser)
        std::printf(" %u:%u/%u", mp.hashTick, mp.blocks, mp.compared);
      std::printf("\n");
    }
    // ---- M9.4-D: THE ENTITY LINE ----------------------------------------
    //
    // A THIRD line, because it answers the third question: "was anything in
    // this world simulated on exactly one machine and seen on both?"
    //
    // THE ACCEPTANCE IS ASYMMETRIC AND THAT IS THE POINT. The machine that
    // OWNS the creatures reports announces > 0 and poses > 0 (it is the only
    // one that can describe them); the machine that walks past them reports
    // ghosts > 0 (it is the only one that can hold one). A run where both
    // sides report the same numbers has not tested ownership at all — it has
    // tested two machines doing the same work, which is the exact failure
    // mode the single-producer rule exists to prevent.
    //
    // `handoffs` may legitimately be 0: a flyby that never lingers long
    // enough for the 2-chunk hysteresis to release a creature does not flip
    // authority, and forcing it to would mean tuning the hysteresis to a
    // smoke rather than to the game. The NUMBER is reported either way.
    //
    // `misses` is the diagnostic that distinguishes the two ways "poses > 0,
    // ghosts == 0" can happen: a pose for an id this machine does not hold is
    // an announce that never arrived (a real bug), while zero misses and zero
    // ghosts means nothing was ever in range (a fixture that measured
    // nothing). A bare ghost count cannot tell those apart.
    {
      const net::EntitySync::Counters& s = entities.Stats();
      std::printf(
          "net: entities: announces out=%llu in=%llu | poses out=%llu in=%llu "
          "asleep=%llu | states out=%llu in=%llu "
          "| handoffs out=%llu in=%llu | gones out=%llu in=%llu | ghosts "
          "now=%u max=%u expired=%llu | items takes=%llu/%llu grants=%llu/%llu "
          "| misses=%llu | entityBatches out=%llu in=%llu\n",
          (unsigned long long)(netEnt.announcesOut + s.announcesOut),
          (unsigned long long)(netEnt.announcesIn + s.announcesIn),
          (unsigned long long)(netEnt.posesOut + s.posesOut),
          (unsigned long long)(netEnt.posesIn + s.posesIn),
          (unsigned long long)(netEnt.posesAsleep + s.posesAsleep),
          (unsigned long long)(netEnt.statesOut + s.statesOut),
          (unsigned long long)(netEnt.statesIn + s.statesIn),
          (unsigned long long)(netEnt.handoffsOut + s.handoffsOut),
          (unsigned long long)(netEnt.handoffsIn + s.handoffsIn),
          (unsigned long long)(netEnt.gonesOut + s.gonesOut),
          (unsigned long long)(netEnt.gonesIn + s.gonesIn),
          (unsigned)s.ghostsNow,
          (unsigned)std::max(netGhostsMax, s.ghostsMax),
          (unsigned long long)(netEnt.expired + s.expired),
          (unsigned long long)(netEnt.takesOut + s.takesOut),
          (unsigned long long)(netEnt.takesIn + s.takesIn),
          (unsigned long long)(netEnt.grantsOut + s.grantsOut),
          (unsigned long long)(netEnt.grantsIn + s.grantsIn),
          (unsigned long long)(netEnt.applyMisses + s.applyMisses),
          (unsigned long long)(netEnt.batchesOut + s.batchesOut),
          (unsigned long long)(netEnt.batchesIn + s.batchesIn));
    }
    // ---- M9.5-B: THE STORE LINE -----------------------------------------
    //
    // A FOURTH line, answering the fourth question: "did an edit made by one
    // machine survive the other machine walking away from it?"
    //
    // READ LIVE OFF THE OBJECT, unlike the three lines above, and that is not
    // an inconsistency: `StoreSync::Disconnect` deliberately KEEPS its
    // counters (as well as its unacked list and its manifest mirror, because
    // a rejoin re-offers the first and re-receives the second), so there is
    // nothing to harvest before it and a carry-over would double every
    // number.
    //
    // THE ACCEPTANCE IS ASYMMETRIC, like the entity line's and for the same
    // reason. The machine that flies and paints reports `puts sent > 0` (it
    // is the authority for the chunks under its own feet and it is the one
    // that evicts them); the host reports the matching `puts recv`, because
    // its store is the truth. A run where both report the same numbers has
    // not tested ownership.
    //
    // `misses` is the diagnostic that separates the two ways `gets sent > 0,
    // data recv == 0` can happen: a miss is an honest "I do not have it"
    // that regenerates the slot, while silence is a held slot that will read
    // as air forever. A bare "0 chunks delivered" cannot tell those apart.
    // `unacked` must be 0 at a clean exit: a put still in the list is an
    // edit the host was never told about.
    {
      const net::StoreSync::Counters& sc = storesync.Stats();
      std::printf("net: store: puts sent=%llu (distinct=%llu) recv=%llu acked=%llu "
                  "refusedOld=%llu local=%llu | manifest entries=%llu "
                  "sent=%llu recv=%llu | gets sent=%llu recv=%llu | data "
                  "sent=%llu recv=%llu | misses sent=%llu recv=%llu | "
                  "wanted=%llu unacked=%llu reoffered=%llu abandoned=%llu\n",
                  (unsigned long long)sc.putsSent,
                  (unsigned long long)sc.putsDistinct,
                  (unsigned long long)sc.putsRecv,
                  (unsigned long long)sc.putsAcked,
                  (unsigned long long)sc.putsRefusedOld,
                  (unsigned long long)sc.putsLocal,
                  (unsigned long long)storesync.ManifestSize(),
                  (unsigned long long)sc.manifestSent,
                  (unsigned long long)sc.manifestRecv,
                  (unsigned long long)sc.getsSent,
                  (unsigned long long)sc.getsRecv,
                  (unsigned long long)sc.dataSent,
                  (unsigned long long)sc.dataRecv,
                  (unsigned long long)sc.missesSent,
                  (unsigned long long)sc.missesRecv,
                  (unsigned long long)sc.wanted,
                  (unsigned long long)storesync.UnackedCount(),
                  (unsigned long long)sc.reoffered,
                  (unsigned long long)sc.abandoned);
      // ...and the Stream side of the same story, which is the half this
      // object cannot see: how many refills took the inert hold, how many
      // were filled by a delivery, and how many holds the window threw away
      // before an answer came (M9.5-A's ExchangeStats).
      const Stream::ExchangeStats& es = stream.Exchange();
      std::printf("net: store: stream holds=%llu delivered=%llu missed=%llu "
                  "forgotten=%llu rejected=%llu\n",
                  (unsigned long long)es.held,
                  (unsigned long long)es.delivered,
                  (unsigned long long)es.missed,
                  (unsigned long long)es.forgotten,
                  (unsigned long long)es.rejected);
    }
    std::fflush(stdout);
  }
  // A ghost owns real Jolt limb bodies and a kinematic capsule; drop them
  // while `phys` is still alive rather than at the end of main's scope.
  // Guarded, like everything else in this package, so a single-player exit
  // executes not one line of it.
  if (netRoleBoot != NetRole::None) {
    remotes.Clear(phys);
    mobs.SetAvatar(&session.avatar);
    // M9.5-B: `storesync` is declared AFTER `stream` in this function, so it
    // is destroyed FIRST — and a Stream holding a pointer to a dead exchange
    // would call through it on the next eviction. Nothing evicts after this
    // line today; unbinding anyway is one statement and removes the whole
    // class of question (sim/stream.h: "clear it before the object it points
    // at dies").
    stream.SetChunkExchange(nullptr);
    if (link) link->Close();
  }

  if (g_harnessFrames > 0 && frameCounter > 0) {
    std::printf("--frames harness: %llu frames, avg render+present %.2f ms "
                "(FIFO/vsync-paced; offscreen render cost is the selftest "
                "'render 1080p' sweep)\n",
                (unsigned long long)frameCounter,
                g_harnessRenderMs / (double)frameCounter);
    // WHOLE-FRAME wall clock, which is what a stall shows up in. The average
    // above is render+present only and a streaming hitch is invisible in it;
    // the percentiles below are the number that matches "it drops to a crawl".
    std::sort(g_frameMs.begin(), g_frameMs.end());
    auto pct = [&](double p) {
      return g_frameMs.empty() ? 0.0
                               : g_frameMs[(size_t)(p * (g_frameMs.size() - 1))];
    };
    size_t over33 = 0, over100 = 0;
    for (double m : g_frameMs) {
      if (m > 33.0) over33++;
      if (m > 100.0) over100++;
    }
    std::printf("--frames harness: whole-frame ms  p50 %.1f  p95 %.1f  p99 %.1f "
                " max %.1f | >33ms %zu (%.1f%%)  >100ms %zu\n",
                pct(0.50), pct(0.95), pct(0.99),
                g_frameMs.empty() ? 0.0 : g_frameMs.back(), over33,
                100.0 * over33 / (double)g_frameMs.size(), over100);
    // ---- THE CPU SCOPE TABLE, same attribution as the Performance tab ------
    //
    // A mean alone cannot describe the thing the user reported ("usually 0.2 ms,
    // sometimes 30"), so every scope carries p99 and max beside it. That is the
    // whole point: a scope whose mean is 0.1 and whose max is 30 is a STALL,
    // and a scope whose mean is 3 and whose max is 4 is a COST. They need
    // opposite fixes and the mean cannot tell them apart.
    {
      using sandvox::kPerfScopeCount;
      using sandvox::kPerfScopeKeys;
      const size_t n = g_frameScopeSeries[0].size();
      if (n > 0) {
        std::printf("--frames harness: CPU scope attribution over %zu frames "
                    "(ms/frame)\n", n);
        std::printf("    %-14s %8s %8s %8s %8s %7s\n",
                    "scope", "mean", "p50", "p99", "max", "%busy");
        // Rank by mean, because the reader wants "what costs the most" first
        // and can then scan the max column for what SPIKES the most.
        int order[kPerfScopeCount];
        for (int i = 0; i < kPerfScopeCount; i++) order[i] = i;
        std::sort(order, order + kPerfScopeCount, [&](int a, int b) {
          return g_frameScopeSum[a] > g_frameScopeSum[b];
        });
        // `present` is the vsync wait, not work — excluded from the busy
        // denominator for the same reason the page excludes it.
        double busy = 0;
        for (int i = 0; i < kPerfScopeCount; i++)
          if (i != (int)sandvox::PerfScope::Present) busy += g_frameScopeSum[i];
        for (int oi = 0; oi < kPerfScopeCount; oi++) {
          const int i = order[oi];
          if (g_frameScopeSum[i] <= 0.0) continue;
          std::vector<double>& v = g_frameScopeSeries[i];
          std::sort(v.begin(), v.end());
          const bool wait = i == (int)sandvox::PerfScope::Present;
          std::printf("    %-14s %8.3f %8.3f %8.3f %8.3f %7s\n",
                      kPerfScopeKeys[i], g_frameScopeSum[i] / (double)n,
                      v[(size_t)(0.50 * (v.size() - 1))],
                      v[(size_t)(0.99 * (v.size() - 1))], g_frameScopeMax[i],
                      wait ? "(wait)"
                           : (busy > 0 ? [&] {
                               static char b[16];
                               std::snprintf(b, sizeof b, "%.1f%%",
                                             100.0 * g_frameScopeSum[i] / busy);
                               return b;
                             }()
                                       : "-"));
        }
        // The bug counter, printed whether or not it fired — "0 stalls" is a
        // result and a missing line is not.
        // ---- THE GPU ROWS, same shape, same reason -------------------------
        {
          size_t gn = 0;
          for (int n = 0; n < sandvox::kPerfNodeCount; n++)
            gn = std::max(gn, g_frameGpuSeries[n].size());
          if (gn > 0) {
            std::printf("--frames harness: GPU node attribution over %zu timed frames "
                        "(ms/frame; rows under 0.05 mean omitted)\n", gn);
            std::printf("    %-16s %8s %8s %8s %8s\n", "node", "mean", "p50", "p99", "max");
            int gorder[sandvox::kPerfNodeCount];
            double gsum[sandvox::kPerfNodeCount];
            for (int n = 0; n < sandvox::kPerfNodeCount; n++) {
              gorder[n] = n;
              gsum[n] = 0;
              for (double v : g_frameGpuSeries[n]) gsum[n] += v;
            }
            std::sort(gorder, gorder + sandvox::kPerfNodeCount,
                      [&](int a, int b) { return gsum[a] > gsum[b]; });
            for (int oi = 0; oi < sandvox::kPerfNodeCount; oi++) {
              const int n = gorder[oi];
              std::vector<double>& v = g_frameGpuSeries[n];
              if (v.empty() || gsum[n] / (double)v.size() < 0.05) continue;
              std::sort(v.begin(), v.end());
              std::printf("    %-16s %8.3f %8.3f %8.3f %8.3f\n",
                          sandvox::kPerfNodes[n].node, gsum[n] / (double)v.size(),
                          v[(size_t)(0.50 * (v.size() - 1))],
                          v[(size_t)(0.99 * (v.size() - 1))], v.back());
              // A node that only runs for PART of the run (drawBodies while a
              // felled tree is a body, before settle-back returns it to the
              // grid) has a whole-run mean and p50 that say how long it ran,
              // not what it cost: the same 28k-voxel oak read p50 0.000 with a
              // 300-tick body life and p50 1.1 with a standing one. The row
              // that answers "what does a frame WITH it cost" is the one over
              // the frames it was measurably in.
              size_t firstOn = 0;
              while (firstOn < v.size() && v[firstOn] < 0.05) firstOn++;
              const size_t on = v.size() - firstOn;
              if (on > 0 && on < v.size() * 9 / 10) {
                double onSum = 0;
                for (size_t i = firstOn; i < v.size(); i++) onSum += v[i];
                std::printf("    %-16s %8.3f %8.3f %8.3f %8.3f  (%zu of %zu frames)\n",
                            "  ^ frames >0.05", onSum / (double)on,
                            v[firstOn + (size_t)(0.50 * (on - 1))],
                            v[firstOn + (size_t)(0.99 * (on - 1))], v.back(), on,
                            v.size());
              }
            }
          }
        }
        if (g_frameGpuFrames > 0) {
          std::printf("--frames harness: GPU PASS attribution (ms/frame; rows under 0.05 mean omitted)\n");
          std::printf("    %-22s %8s %8s %8s %8s\n", "pass", "mean", "p50", "p99", "max");
          std::vector<std::pair<double, std::string>> order;
          for (auto& kv : g_frameGpuPassSeries) {
            kv.second.resize(g_frameGpuFrames, 0.0);
            double sum = 0;
            for (double v : kv.second) sum += v;
            order.push_back({sum / (double)g_frameGpuFrames, kv.first});
          }
          std::sort(order.begin(), order.end(),
                    [](const auto& a, const auto& b) { return a.first > b.first; });
          for (const auto& o : order) {
            if (o.first < 0.05) continue;
            std::vector<double>& v = g_frameGpuPassSeries[o.second];
            std::sort(v.begin(), v.end());
            std::printf("    %-22s %8.3f %8.3f %8.3f %8.3f\n", o.second.c_str(), o.first,
                        v[(size_t)(0.50 * (v.size() - 1))],
                        v[(size_t)(0.99 * (v.size() - 1))], v.back());
          }
        }
        {
          const DebrisSystem::SettleProbe& sp = debris.Settle();
          {
            const BodyRenderStats& bs = g_bodyStats;
            const double f = bs.frames ? (double)bs.frames : 1.0;
            std::printf("--frames harness: body-render over %llu body frames: "
                        "instance build %.1f us/frame (max %.1f), %llu rebuilds, "
                        "cube upload %.1f KiB/frame, micro pool upload %.1f "
                        "KiB/frame, cube draws %.2f/frame, cube instances "
                        "%.0f live / %.0f drawn per frame\n",
                        (unsigned long long)bs.frames, bs.buildUs / f, bs.buildMaxUs,
                        (unsigned long long)bs.builds, bs.instBytes / f / 1024.0,
                        (double)sim.MicroPoolBytesSent() / f / 1024.0,
                        bs.cubeDraws / f, bs.cubeInstLive / f, bs.cubeInstDrawn / f);
            std::printf("    body-render split: list build %.1f us/frame, arena "
                        "commit %.1f us/frame, %u arena repacks\n",
                        bs.listUs / f, bs.commitUs / f, bodyArena.Repacks());
          }
          std::printf("    terrain patches: %u rebuilt, %u deferred by the per-tick "
                      "budget, %u refreshed with an identical occupancy box\n",
                      sp.terrainBuilds, sp.terrainDeferred, sp.terrainSame);
          // The shared readback FIFO, whole run: who asked, how long they
          // waited, what was dropped. See World::FetchProbe.
          std::printf("    %s\n", world.FetchReport().c_str());
        }
        std::printf("    gpu-lag throttle: %llu ticks deferred to a later frame "
                    "(the GPU owed >= 2 snapshots when a second tick was due)\n",
                    (unsigned long long)g_ticksThrottled);
        // A full refill is kFarLevels * kFarNumChunks entries; anything less
        // than that here means the horizon was still arriving at exit.
        std::printf("    far-cascade sieve: %llu entries over %llu ticks "
                    "(biggest tick %llu, cap %d) | %zu still queued at exit "
                    "(a full refill is %u)\n",
                    (unsigned long long)g_farEntries,
                    (unsigned long long)g_farTicks,
                    (unsigned long long)g_farBiggest,
                    CurrentTuning().render.farRefillRate, far.PendingFills(),
                    kFarLevels * kFarNumChunks);
    // WHAT PUT IT THERE (farfield.h's counters). A queue depth on its own does
    // not say whether the fix is a bigger plane cap, a cheaper sieve entry, or
    // an origin that keeps falling a whole box behind — and those want
    // opposite changes.
    std::printf("      produced by: %llu wholesale refills, %llu resets "
                "(%llu an origin gap, %llu a coalesced backlog) "
                "x %u entries + %llu planes x %u entries | worst origin gap "
                "%u level chunks on level %u (reset at %u)\n",
                (unsigned long long)far.RefillsIssued(),
                (unsigned long long)far.ResetsIssued(),
                (unsigned long long)far.GapResetsIssued(),
                (unsigned long long)far.CoalescedResets(), kFarNumChunks,
                (unsigned long long)far.PlanesIssued(),
                kFarNChunk * kFarNChunk, far.WorstGap(),
                far.WorstGapLevel(), kFarNChunk);
        std::printf("    snapshot stalls (blocking WaitIdle on the frame path):"
                    " %llu over %llu frames (%.1f%% of frames) | readback "
                    "requests the ring refused: %llu\n",
                    (unsigned long long)g_frameSnapStalls,
                    (unsigned long long)n,
                    100.0 * (double)g_frameSnapStallFrames / (double)n,
                    (unsigned long long)g_frameRbDeclines);
        // ---- P2-D: which cause, which arm, how stale (CLAUDE.md rule 6) ----
        // The line above is the bare count. This one says whether the ring was
        // too shallow (refused) or the GPU too far behind (issued-not-landed),
        // whether the targeted one-fence wait covered it or the full device
        // drain ran, and how many of those drains were futile.
        {
          const sandvox::SnapshotStallStats& ss = g_frameStallStats;
          std::printf("      cause: refused %u (no copy encoded for the tick) "
                      "| not-landed %u (GPU queue depth)\n",
                      ss.refusedArm, ss.issuedArm);
          std::printf("      arm:   map-wait %u (%u fences, %.1f ms total) "
                      "| WaitIdle %u (%.1f ms total, %u futile) "
                      "| proceeded stale %u\n",
                      ss.mapArm, ss.mapWaits, ss.mapArmMs, ss.idleArm,
                      ss.idleArmMs, ss.idleFutile, ss.proceedStale);
          std::printf("      staleness at stall (ticks):");
          for (int i = 0; i <= 8; i++)
            if (ss.gapHist[i]) std::printf(" %d:%u", i, ss.gapHist[i]);
          if (ss.gapHist[9]) std::printf(" no-snap:%u", ss.gapHist[9]);
          std::printf("  max %u\n", ss.gapMax);
        }
        // ---- and one level down into `stream`, which is where it all is ----
        // The scope table says streaming dominates; this says which PART of a
        // window shift. Denominated PER SHIFT, because a shift is the unit the
        // cost comes in — a per-frame mean of an all-or-nothing 40 ms stall
        // describes nothing that happens.
        const Stream::Timing& st = stream.Timings();
        if (st.shifts > 0) {
          const double per = 1.0 / (double)st.shifts;
          // `wake-wait` is the R1 poll that was NOT ready at T+kWakeLatency
          // and had to block (docs/RESEARCH_streaming_hitch.md). Nonzero means
          // the fence the deferral was written to remove has come back; zero
          // is the design working. `demote` is now the T+K completion, not a
          // fence, so it should read under a millisecond.
          std::printf("    window shift breakdown: %u shifts, %.2f ms each "
                      "| evict %.2f  fill-store %.2f  fill-gen %.2f  demote "
                      "%.2f  wake-wait %.2f (%u) || per-tick: harvest %.3f  "
                      "dirty-fold %.3f\n",
                      st.shifts, st.totalMs * per, st.evictMs * per,
                      st.fillStoreMs * per, st.fillGenMs * per,
                      st.demoteMs * per, st.wakeWaitMs * per, st.wakeWaits,
                      st.harvestMs / (double)n,
                      st.dirtyFoldMs / (double)n);
        }
      }
    }
    // Which half of the CA cost model this run lived in (see g_activeChunks).
    if (!g_activeChunks.empty()) {
      std::sort(g_activeChunks.begin(), g_activeChunks.end());
      auto apct = [&](double p) {
        return g_activeChunks[(size_t)(p * (g_activeChunks.size() - 1))];
      };
      double sum = 0.0;
      for (double a : g_activeChunks) sum += a;
      const double mean = sum / (double)g_activeChunks.size();
      // The model of ROADMAP_scale.md §3.0, evaluated at the mean, so the
      // floor share is reported rather than left to be recomputed by hand.
      const double floorUs = 54.0 * 4.65;
      const double perChunkUs = 54.0 * 0.245 * mean;
      std::printf("--frames harness: active chunks  p50 %.0f  p95 %.0f  max %.0f"
                  "  mean %.0f | modelled CA %.0f us/tick = %.0f floor + %.0f "
                  "per-chunk (floor %.0f%%)\n",
                  apct(0.50), apct(0.95), g_activeChunks.back(), mean,
                  floorUs + perChunkUs, floorUs, perChunkUs,
                  100.0 * floorUs / (floorUs + perChunkUs));
    }
    // PAGE POOL HIGH WATER — and this harness is the only honest place to read
    // it. kPoolPages exhaustion is a fatal abort, not a degradation, so the
    // margin has to be a tracked number; but the selftest harness's window is
    // sky-heavy and under-reports by ~2x, and SANDVOX_PT_DEBUG's `inUse=` trace
    // is per-tick spam rather than a summary. `--frames N --autofly-hard` is
    // the adversarial traversal CLAUDE.md names for pool sizing, and until now
    // it printed everything about a run EXCEPT the number it exists to measure.
    // pagesHighWater_ is monotonic and never reset, so this covers the whole
    // run regardless of where the peak fell.
    if (world.residency == World::Residency::Paged && world.pages) {
      const uint32_t hw = world.pages->PagesHighWater();
      std::printf("--frames harness: page pool high water %u of %u (%.1f%%, "
                  "%.1f MiB) | in use at exit %u | %u window shifts\n",
                  hw, kPoolPages, 100.0 * (double)hw / (double)kPoolPages,
                  (double)hw * kChunkVol * 4.0 / (1024.0 * 1024.0),
                  world.pages->PagesInUse(), stream.ShiftCount());
      // And WHAT those pages are, at the peak and at the exit — the two ticks
      // a sizing run needs explained. Without this the harness prints the
      // number it exists to measure and nothing about its composition, which
      // is the same bare-count trap the high-water line itself was added to
      // fix one level up (CLAUDE.md rule 6).
      PrintPageCensus(world.pages->CensusAtHighWater(), "high water");
      PrintPageCensus(world.pages->Census(), "exit");
      // AND THE FAULT COUNT, because a residency number is only meaningful
      // beside it. The autofly arms are the only place the streaming and free
      // paths are exercised at length, and until now the one counter this
      // engine treats as "0 is the only acceptable value" was invisible on
      // exactly those runs - it is printed by --selftest and the smokes and by
      // nothing else, so "--frames 1200 --autofly-surface, faults 0" was a
      // claim nobody could actually read off the run. The snapshot already
      // carries it (world.cpp's PageFaults copy); this just prints it.
      {
        const WorldSnapshot& fsn = world.Snap();
        std::printf("--frames harness: page faults %u%s\n",
                    fsn.valid ? fsn.pageFaults : 0u,
                    (fsn.valid && fsn.pageFaults)
                        ? "  *** SENTINEL WRITES LOST VOXELS ***"
                        : " (0 is the only acceptable value)");
      }
    }
    // Per-regime arms (see g_frameMsLow/High). The HIGH number is the one the
    // altitude work is judged on; the LOW one is the canopy/meadow skim.
    if (g_autoflySurface) {
      auto arm = [](const char* name, std::vector<double>& v) {
        if (v.empty()) return;
        std::sort(v.begin(), v.end());
        auto q = [&](double p) { return v[(size_t)(p * (v.size() - 1))]; };
        std::printf("--autofly-surface %s: n %zu  p50 %.1f  p95 %.1f  max %.1f\n",
                    name, v.size(), q(0.50), q(0.95), v.back());
      };
      arm("low-skim  ", g_frameMsLow);
      arm("high-cruise", g_frameMsHigh);
    }
  }

  // Close the op record explicitly rather than leaving it to the CRT: a
  // `--frames` run that is killed by the harness would otherwise lose the
  // buffered tail, and the whole use of this file is a byte-for-byte compare.
  if (sandvox::opstream::Recording()) {
    std::printf("op record: %u frames, %llu bytes -> %s\n",
                sandvox::opstream::RecordedFrames(),
                (unsigned long long)sandvox::opstream::RecordedBytes(),
                recordOpsPath.c_str());
    sandvox::opstream::StopRecording();
  }
  telemetry.Shutdown();
  ctx.WaitIdle();
  // Windowed Vulkan: print (and count) everything the debug messenger
  // collected over the session — nothing pops a scope except F5 reloads.
  ctx.ReportVkValidation("session");
  // Audio down before anything it points at: Shutdown stops the device, which
  // is the only thread that can still be inside the mixer.
  if (audioCues.Enabled()) {
    const audio::Cues::Stats& as = audioCues.GetStats();
    std::printf("[audio] %u steps, %u landings, %u impacts, %u breaks, "
                "%u creature, %u bleeds, %u dropped\n",
                as.steps, as.lands, as.impacts, as.breaks, as.mobs, as.bleeds,
                as.dropped);
  }
  audioCues.Shutdown();
  overlay.Shutdown();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
