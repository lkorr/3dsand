// sandvox — 3D falling-sand voxel engine (v0). See DESIGN.md.
// Fixed 30 Hz GPU simulation, uncapped raymarched rendering, walkable player,
// JSON materials, deterministic kernels with per-tick world hash.

#include <algorithm>
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
#include "game/equipment.h"
#include "game/worlditems.h"
#include "game/corpses.h"
#include "game/dye.h"
#include "game/item.h"
#include "game/melee.h"
#include "game/mob.h"
#include "game/spell.h"
#include "game/strike_pick.h"
#include "sim/rng.h"
#include "game/player.h"
#include "game/prefab.h"
#include "game/thirdperson.h"
#include "gpu/context.h"
#include "gpu/resources.h"
#include "gpu/rhi_vk.h"  // rhi::vkr::SetCaptureStats (--shader-stats)
#include "gpu/vk_info.h"
#include "gpu/vk_shader_stats.h"
#include "gpu/vk_smoke.h"
#include "lab/lab.h"
#include "math3d.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/farfield.h"
#include "sim/celestial.h"
#include "sim/materials.h"
#include "sim/microbody.h"
#include "sim/microvox.h"
#include "sim/pagetable.h"  // PagesHighWater for the --frames pool-margin line
#include "sim/simulation.h"
#include "sim/tuning.h"
#include "sim/stream.h"
#include "sim/voxload.h"
#include "sim/waterbody.h"
#include "sim/wind.h"
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
#include "test/treefixture.h"
#include "ui/overlay.h"
#include "crash.h"

// The sim/render plumbing these once defined in place now lives in
// test/support.{h,cpp}, so the selftest can use it from its own translation
// units without a second copy drifting out of step.
using namespace sandvox;

namespace {
// Owner handle for wind primitives placed from the DEV PANEL, so "clear all"
// retires those and nothing else. A spell's gust owns itself (the casterId) and
// expires on its own TTL; the panel has no business reaching into gameplay.
constexpr uint64_t kDevFanOwner = 0xDEFA11Au;

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
constexpr uint64_t kShotInvOpenFrame = 150;    // let the avatar spawn and settle
constexpr uint64_t kShotInvDamageFrame = 170;
constexpr uint64_t kShotInvGearFrame = 220;    // -> screenshot_inventory.bmp
constexpr uint64_t kShotInvCaptureFrame = 240;  // -> ..._health.bmp
constexpr uint64_t kShotInvGrimoireFrame = 260; // -> ..._grimoire.bmp
// Fourth: a dressed human killed in front of the player and its corpse opened
// — the loot panel where the grimoire was (game/corpses.h). Last frame.
constexpr uint64_t kShotInvLootFrame = 280;     // -> ..._loot.bmp; last frame

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
float g_shotJumpVy = 0.0f;             // the vy that picture was taken at, m/s

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
int g_fellTreeAt = 240;  // the plant tick; the cut is 120 ticks later
// An optional site ("x,z"): the game then STARTS 48 voxels west of it looking
// +X, so the tree stands there. Default is 48 voxels ahead of wherever the
// player spawned -- which at the map's spawn site is inside a forest, and a
// crown that touches a neighbour's is anchored through it (2026-09-12: the
// flood walked 54 voxels west and 40 down through another tree and correctly
// refused). The harness pad (map.json site `harness`, x/z -128..640) has no
// trees by construction; --fell-tree 240 200,200 plants there.
bool g_fellSiteSet = false;
int g_fellSiteX = 0, g_fellSiteZ = 0;
// SANDVOX_PARK_AT="x,y,z": park at a NAMED PLACE instead of wherever the
// procedural surface route happens to stop.
//
// The route is dt-integrated, so where it ends is not a choice anybody made —
// the 2026-08-24 answer below came back "97% of the survivors contain LAVA"
// because that is what the flight passed over, and a re-run on 2026-09-08
// landed in open desert and settled to 0 active chunks. Neither run says
// anything about a material it never flew over. "Which materials hold a chunk
// awake" is a question about a PLACE, and this engine has places at known
// addresses: `World::AuthoredPoolList` gives the authored lake's centre and
// waterline, `--voxdump`'s coordinates give any other.
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
//
// Maps the avatar's limbs onto the fixed stick-figure slots. The mapping is by
// authored TAG ("head"/"spine"/"arm"/"hand"/"leg"/"foot") plus the ".L"/".R"
// side suffix and an upper/lower discriminator, NOT by mina's exact part names
// — a different humanoid rig with the same tags fills the same figure, and a
// rig missing a part leaves that slot absent rather than mis-drawn.
//
// Upper vs lower within a limb is read from the name ("armU"/"armL"), which is
// the convention the rigs already use; when a rig has only one segment per limb
// it lands in the upper slot and the figure simply draws a shorter limb.
int BodySlotFor(const char* name, const char* tag) {
  const std::string n = name, t = tag;
  const bool left = n.size() >= 2 && n.compare(n.size() - 2, 2, ".L") == 0;
  const bool right = n.size() >= 2 && n.compare(n.size() - 2, 2, ".R") == 0;
  // The segment letter is the character just before the side suffix:
  // "armU.L" -> 'U' (upper), "armL.L" -> 'L' (lower). Anything without a side
  // suffix, or with any other letter there, is treated as the upper segment.
  const bool lower =
      (left || right) && n.size() >= 3 && n[n.size() - 3] == 'L';

  if (t == "head") return UIState::kSlotHead;
  if (t == "spine") {
    // The rig's root is the hips; every other spine part is the torso.
    return n.find("hip") != std::string::npos ? UIState::kSlotHips
                                              : UIState::kSlotTorso;
  }
  if (t == "hand") return left ? UIState::kSlotHandL : UIState::kSlotHandR;
  if (t == "foot") return left ? UIState::kSlotFootL : UIState::kSlotFootR;
  if (t == "arm") {
    if (left) return lower ? UIState::kSlotArmLL : UIState::kSlotArmUL;
    if (right) return lower ? UIState::kSlotArmLR : UIState::kSlotArmUR;
  }
  if (t == "leg") {
    if (left) return lower ? UIState::kSlotLegLL : UIState::kSlotLegUL;
    if (right) return lower ? UIState::kSlotLegLR : UIState::kSlotLegUR;
  }
  return -1;  // props, held items, anything the figure has no place for
}

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
};

TissueMats ResolveTissueMats(const MobSystem& mobs) {
  TissueMats t;
  t.skin   = mobs.MaterialIdNamed("skin");
  t.flesh  = mobs.MaterialIdNamed("flesh");
  t.muscle = mobs.MaterialIdNamed("muscle");
  t.bone   = mobs.MaterialIdNamed("bone");
  t.brain  = mobs.MaterialIdNamed("brain");
  return t;
};

BurnMats ResolveBurnMats(const std::vector<MaterialDef>& mats) {
  BurnMats bm;
  // Named, never hardcoded by id (CLAUDE.md conventions), and the NAME LIST
  // lives in one place: Mob::BurnStageOfMaterialName is what the burn cap
  // (sim/tuning.h Gore §G) counts with, so the HUD's per-limb readout and the
  // creature's own health cap cannot disagree about what "burnt" is. A name
  // not in this content simply contributes nothing — the readout degrades to
  // "not charred" rather than reporting a wrong material's count.
  for (size_t i = 0; i < mats.size(); i++) {
    const uint8_t stage = Mob::BurnStageOfMaterialName(mats[i].name);
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
  ui.stainHudMin = CurrentTuning().coat.hudMinFrac;
  const MobDef* def = avatar.Def();
  ui.bodyValid = def != nullptr;
  if (!def) return;
  const uint64_t mobId = avatar.Id();
  SlotCoat slotCoat[UIState::kSlotCount];
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
    if (now > 0) {
      uint32_t cooked = 0, charred = 0;
      for (uint32_t m : burnMats.cooked) cooked += avatar.PartMaterialCount(i, m);
      for (uint32_t m : burnMats.charred)
        charred += avatar.PartMaterialCount(i, m);
      // Charred counts double against "intact-looking": cooked flesh is still
      // flesh, charred flesh is structurally gone. Reported as one fraction
      // because the player's question is "how much of this limb is ruined",
      // not "which rung of the reaction ladder is it on".
      b.charredFrac =
          std::clamp((float)(cooked + charred * 2) / (float)(now * 2), 0.0f,
                     1.0f);
    }

    b.voxelTotal = now;
    if (tissueMats.skin)   b.voxelSkin   = avatar.PartMaterialCount(i, tissueMats.skin);
    if (tissueMats.flesh)  b.voxelFlesh  = avatar.PartMaterialCount(i, tissueMats.flesh);
    if (tissueMats.muscle) b.voxelMuscle = avatar.PartMaterialCount(i, tissueMats.muscle);
    if (tissueMats.bone)   b.voxelBone   = avatar.PartMaterialCount(i, tissueMats.bone);
    if (tissueMats.brain)  b.voxelBrain  = avatar.PartMaterialCount(i, tissueMats.brain);
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

// Frame the LIVE body, not the def's box. A def-sized frame is wrong twice
// over: a rig that has lost both legs is half the height it was authored at,
// and the origin of a heavily dismembered body is nowhere near the part of it
// you can still see (the same lesson --shot-mob learned about corpses).
PortraitCam MakePortraitCam(const PlayerAvatar& av, Physics& phys, float yaw,
                            float pitch, float aspect, float zoom,
                            float panX, float panY, int pivotSlot = -1) {
  PortraitCam pc;
  if (!av.Spawned() || !av.Def()) return pc;
  Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
  Vec3 pivotLo{1e9f,1e9f,1e9f}, pivotHi{-1e9f,-1e9f,-1e9f};
  bool anyPivot = false;
  const int limbCount = (int)av.Def()->limbs.size();
  bool any = false;
  for (int i = 0; i < limbCount; i++) {
    Vec3 c[8];
    if (!LimbBoxCorners(av, phys, i, c)) continue;
    any = true;
    const int s = BodySlotFor(av.PartName(i), av.PartTag(i));
    for (const Vec3& p : c) {
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
  const float dist = std::max(fitV, fitH) * 1.28f / std::max(zoom, 0.1f);

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
void ProjectBodyUI(const PlayerAvatar& av, Physics& phys, const PortraitCam& pc,
                   UIState& ui) {
  for (int i = 0; i < UIState::kSlotCount; i++) ui.body[i].projValid = false;
  if (!pc.valid || !av.Def()) return;
  const int limbCount = (int)av.Def()->limbs.size();
  for (int i = 0; i < limbCount; i++) {
    const int slot = BodySlotFor(av.PartName(i), av.PartTag(i));
    if (slot < 0) continue;
    Vec3 c[8];
    if (!LimbBoxCorners(av, phys, i, c)) continue;
    float mn[2] = {1e9f, 1e9f}, mx[2] = {-1e9f, -1e9f};
    int hits = 0;
    for (const Vec3& p : c) {
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
  FarField far;
  far.Init(&world);
  far.FullRefill({8, 3, 8});
  uint32_t n;
  while ((n = far.PrepareTick(ctx.queue)) > 0) {
    TickParams tp{0, kDefaultSeed, 0, 0};
    tp.farCount = n;
    ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeFarFill(enc, n);
    ctx.queue.Submit(enc.Finish());
  }
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
    const int kLx = 420, kLz = 420, kSurf = 68;   // authored lake, surface y=68
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
  far.FullRefill({8, 3, 8});
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

  // Splash droplets carry the water material, resolved BY NAME at load like
  // all content (CLAUDE.md conventions). Missing name = no droplets, not a
  // crash — the surface still renders.
  uint32_t splashMats[4] = {0, 0, 0, 0};
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == "water") { splashMats[0] = (uint32_t)i; break; }

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
            op.species = 0;
            op.mat = splashMats[0];  // water: the particle's settled identity
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
                 /*particlesActive=*/true, {}, 0, spawns, fluidCount,
                 splashMats);
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
               const std::string& spec) {
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
  int defIndex = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == defName) defIndex = (int)i;
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
  auto mobTick = [&]() {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(t + 1, world, ops, cellOps, spawns);
    debris.QueueSupportEvents(world.Snap());
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps, false,
               {8, h / 16, 8}, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
    mobs.PostStep();
  };

  for (int i = 0; i < 20; i++) mobTick();  // healthy walk first: live gait pose
  for (size_t start = 0; start < limbCsv.size();) {
    size_t end = limbCsv.find(',', start);
    if (end == std::string::npos) end = limbCsv.size();
    std::string nm = limbCsv.substr(start, end - start);
    start = end + 1;
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
        const unsigned long v =
            std::strtoul(itemName.c_str() + hash + 1, nullptr, 16);
        // Authored the way a human writes a colour (#RRGGBB) and packed the
        // way the GPU reads one (r in the low byte) — the swap is here rather
        // than in dye.h because this is the only place a dye is ever typed.
        wearDye = DyePack((float)((v >> 16) & 0xFFu) / 255.0f,
                          (float)((v >> 8) & 0xFFu) / 255.0f,
                          (float)(v & 0xFFu) / 255.0f);
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
  shoot(right + Vec3{0, 0.15f, 0}, shotDist, "screenshot_mob_side.bmp");
  shoot((fwd + right) * 0.7071f + Vec3{0, 0.3f, 0}, shotDist,
        "screenshot_mob_quarter.bmp");
  shoot(fwd + Vec3{0, 0.15f, 0}, shotDist, "screenshot_mob_front.bmp");
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
    // A bruise is a COAT, not a material rewrite (DESIGN.md, "A bruise is an
    // alpha that deepens"), so counting voxels whose MATERIAL is `bruiseMat`
    // reports zero on a thoroughly beaten limb. Count the ones wearing it.
    if (bruiseMat) s.bruise = mobs.LimbCoatMatCount(id, i, bruiseMat, 1);
    if (rotMat) s.rot = mobs.LimbMaterialCount(id, i, rotMat);
    s.stain = mobs.LimbStainCount(id, i, 1);
  }
}

int RunStrikeShot(GpuContext& ctx, World& world, Simulation& sim, Physics& phys,
                  DebrisSystem& debris, MobSystem& mobs,
                  const ItemLibrary& items, const std::string& attackerSpec,
                  const std::string& styleName,
                  const std::string& targetSpec, int tailTicksWanted) {
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

  auto mobTick = [&]() {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(t + 1, world, ops, cellOps, spawns);
    debris.QueueSupportEvents(world.Snap());
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps, false,
               {8, h / 16, 8}, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
    mobs.PostStep();
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

// Thrown bouncing bomb — the first CPU gameplay projectile (DESIGN.md §8).
// Float math is fine here: the grid only ever sees the ExplosionOp it emits,
// which travels through the deterministic MutationQueue path.
struct Grenade {
  Vec3 pos, vel;  // voxel units, voxels/s
  float fuse;     // seconds
};

// Integrate one 30 Hz tick against the voxel mirror. Returns true on detonate.
bool UpdateGrenade(Grenade& g, float dt, const Player::KindFn& kindAt) {
  g.fuse -= dt;
  if (g.fuse <= 0.0f) return true;
  g.vel.y -= (9.81f / kVoxelMeters) * dt;
  IVec3 at{ifloor(g.pos.x), ifloor(g.pos.y), ifloor(g.pos.z)};
  if (kindAt(at) == CellKind::Liquid) g.vel = g.vel * 0.90f;  // water drag

  for (int axis = 0; axis < 3; axis++) {
    float& v = axis == 0 ? g.vel.x : axis == 1 ? g.vel.y : g.vel.z;
    float d = v * dt;
    if (d == 0) continue;
    float* c = axis == 0 ? &g.pos.x : axis == 1 ? &g.pos.y : &g.pos.z;
    float step = d > 0 ? 0.4f : -0.4f;
    int n = (int)std::ceil(std::abs(d) / 0.4f);
    for (int i = 0; i < n; i++) {
      float prev = *c;
      float next = (i == n - 1) ? *c + (d - step * i) : *c + step;
      *c = next;
      IVec3 cell{ifloor(g.pos.x), ifloor(g.pos.y), ifloor(g.pos.z)};
      if (kindAt(cell) == CellKind::Solid) {
        *c = prev;
        v = -v * 0.45f;  // bounce with restitution
        if (axis != 0) g.vel.x *= 0.8f;
        if (axis != 1) g.vel.y *= 0.8f;
        if (axis != 2) g.vel.z *= 0.8f;
        break;
      }
    }
  }
  return false;
}

// Where the beam LEAVES the caster, in world voxels.
//
// It must clear the avatar's own head. The eye sits inside the skull part, so
// a damage ray cast from there hits the caster on its first tick and the
// dismemberment path takes their head off before the beam reaches anything —
// which is exactly what "the laser kills me instantly" was. Forward of the
// face and down-right of the eyeline, so it reads as fired from the hand.
//
// ONE definition on purpose: the damage ray and the beam sprites both call
// this. They used to carry separate offsets, so the visible beam came from the
// hand while the lethal one came from the middle of the player's face.
Vec3 LaserMuzzle(const Player& player, const Camera& cam) {
  return player.EyePos() + cam.Forward() * 1.2f + cam.Right() * 0.7f -
         cam.Up() * 0.5f;
}

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
  bool vkInfo = false;   // --vk-info: Vulkan backend smoke test (headless)
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
  // --voxdump ox,oy,oz,nx,ny,nz,lod[,seed] / --voxserve (tools/voxregion.h)
  std::string voxdumpArgs;
  std::string voxdumpOut = "build/voxregion.bin";
  bool voxserve = false;
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
          "  --shot-inventory      Character screen (I) with a damaged avatar,\n"
          "                        one frame to screenshot_inventory.bmp\n"
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
          "  --scenario <id>       One --perf scenario (idle|treeburn|flythrough|explosion|water)\n"
          "  --perf-out <path>     Where --perf writes its JSON\n"
          "  --perf-w/--perf-h <n> Offscreen render size for --perf/--render-budget\n"
          "  --render-budget       Where INSIDE the raymarch the GPU frame went\n"
          "  --budget-cams <list>  --render-budget cameras (noon,dusk,cascade,submerged,meadow,canopy; default all)\n"
          "  --shader-stats        Per-shader registers/spills from the driver\n"
          "                        -> build/shader_stats.json (headless)\n\n"
          "Residency:\n"
          "  --residency paged|dense  Voxel buffer residency mode (default: paged)\n\n"
          "Vulkan / debug:\n"
          "  --backend vulkan      Explicitly name the Vulkan backend\n"
          "  --vk-info             Vulkan device + shader compile check (headless)\n"
          "  --vk-smoke            Quiet 50-tick pinned hash comparison\n"
          "  --vk-smoke-loud       Active 120-tick hash comparison (19 probes)\n"
          "  --vk-validation       Enable VK_LAYER_KHRONOS_validation + sync\n"
          "  --barriers=sledgehammer  Full barrier oracle (§6.2)\n"
          "  --barriers=precise    Precise barriers (default)\n\n"
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
    // `--shot-inventory` is the character screen's look-iteration harness: run
    // the windowed game, spawn and damage the avatar on a fixed schedule, open
    // the screen, and write ONE frame out as a BMP. See the note at
    // g_shotInventory.
    else if (a == "--shot-inventory") {
      g_shotInventory = true;
      g_harnessFrames = kShotInvLootFrame;
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
    // `--vk-info` is the Vulkan port's phase-3a exit proof (src/gpu/vk_info.cpp):
    // create a VkDevice, print the capability record phase 7 needs, compile
    // every WGSL shader to SPIR-V through Tint, build every compute pipeline,
    // zero-init and submit one fenced command buffer. Headless, and it runs no
    // sim work — the only commands submitted are the zero-init fills.
    else if (a == "--vk-info") vkInfo = true;
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
    // --voxdump / --voxserve: REAL VOXELS for the tuner's terrain viewer.
    // Unlike --heightmap these need a GPU (genCell is WGSL), so they answer
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
          !worldmap::LoadWorldMap(ad, CurrentTuning().world.mapLayer, set, m.size(), kDefaultSeed, map, blog)) {
        std::fprintf(stderr, "--heightmap: %s", blog.c_str());
        return 1;
      }
      worldmap::SetCurrentWorldMap(std::move(map));
    }
    return WriteHeightmap(heightmapArgs, heightmapOut);
  }

  // --vk-info answers before any GpuContext exists: it builds its own device
  // to print the capability record, so it must not race the engine's for the
  // adapter.
  if (vkInfo) return sandvox::RunVkInfo(lowPowerAdapter);

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
        if (!worldmap::LoadWorldMap(ad, CurrentTuning().world.mapLayer, stBiomes, m.size(), kDefaultSeed, stMap, wl) ||
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
        LoadMobDefs(ad + "/mobs", m, defs, stMbSet, ml);
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
  // The authored edit layer named by worldgen.editLayer. Read here, before any
  // world exists, so the very first SubmitWorldgen already queues it — a layer
  // loaded after worldgen would not appear until something happened to
  // regenerate the chunks it lives in.
  LoadWorldEditLayerFromTuning(assetDir);
  std::vector<MaterialDef> mats;
  std::vector<ReactionGpu> reactions;
  std::string errors;
  if (!LoadAssets(assetDir + "/materials/materials.json",
                  assetDir + "/materials/reactions.json", mats, reactions, errors)) {
    std::fprintf(stderr, "asset load failed:\n%s\n", errors.c_str());
    return 1;
  }
  std::printf("loaded %zu materials, %zu reactions\n", mats.size(), reactions.size());

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
    if (!worldmap::LoadWorldMap(assetDir, CurrentTuning().world.mapLayer, biomeSet, mats.size(), kDefaultSeed, map, blog) ||
        !worldmap::PackWorldMap(biomeSet, map, worldMapWords, blog)) {
      std::fprintf(stderr, "%s", blog.c_str());
      std::fprintf(stderr, "world map '%s' failed to load -- refusing to start (a world with no "
                           "map is not a world; see src/sim/worldmap.h)\n",
                   CurrentTuning().world.mapLayer.c_str());
      return 1;
    }
    worldmap::SetCurrentWorldMap(std::move(map));
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
      biomes::StampEnvironment(assetDir, CurrentTuning().world.mapLayer);
  std::printf("%s\n", envStamp.Line().c_str());
  auto envStampMessage = [&envStamp]() {
    return std::string("{\"v\":3,\"type\":\"environment\",\"stamp\":") + envStamp.Json() + "}";
  };

  GLFWwindow* window = nullptr;
  if (!selftest && !shot && !shotWaterfall && !shotDebrisPond && !measure && !perf &&
      !fluidBench && !shaderStats &&
      shotMob.empty() && voxdumpArgs.empty() && !voxserve) {
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
    const bool voxelTool = voxserve || !voxdumpArgs.empty();
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
  // ...and the corpses you can loot (game/corpses.h): the same registry shape
  // over the same bodies, declared before `debris` for the same reason.
  Corpses corpses;
  DebrisSystem debris;
  debris.Init(&phys, &world, mats, reactions);
  debris.SetOnBodyGone([&ground, &corpses](uint64_t h) {
    ground.OnBodyGone(h);
    corpses.OnBodyGone(h);
  });
  MobSystem mobs;
  mobs.Init(&phys, &world, &debris, mats, reactions);
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
  std::unordered_map<uint32_t, MicroBodyRef> sphereModels;  // material -> model
  {
    std::vector<MobDef> mobDefs;
    std::string mlog;
    LoadMobDefs(assetDir + "/mobs", mats, mobDefs, mbSet, mlog);
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
      [&ground](uint64_t h, const std::string& name, uint32_t dye) {
        ground.Add(h, name, dye);
      });
  // A CREATURE THAT FELL WITH THINGS ON IT. Die() reports the bodies it became
  // and the gear still on them the instant before the rig forgets; from here
  // on the heap is a corpse the crosshair can name and the character screen
  // can open (Mob::CorpseReport, game/corpses.h).
  mobs.SetOnCorpse([&corpses](const CorpseReport& r) { corpses.Add(r); });
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
    const bool voxelTool = voxserve || !voxdumpArgs.empty();
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
  // table exist (genCell is WGSL and the palette is the COMPILED table), and
  // before anything spawns a player, a mob or a physics world — none of which a
  // terrain dump has any use for.
  if (!voxdumpArgs.empty())
    return RunVoxDump(ctx, world, sim, mats, voxdumpArgs, voxdumpOut);
  if (voxserve) return RunVoxServe(ctx, world, sim, mats);
  if (shot) return RunShots(ctx, world, sim);
  if (shotWaterfall) return RunWaterfallShot(ctx, world, sim);
  if (shotDebrisPond) return RunDebrisPondShot(ctx, world, sim, mats);
  if (shotFluid || shotFluidPond)
    return RunFluidShot(ctx, world, sim, mats, shotFluidPond);
  if (fluidBench)
    return RunFluidBench(ctx, world, sim, mats, fluidBenchScene,
                         stOpt.jsonPath);
  if (!shotMob.empty())
    return RunMobShot(ctx, world, sim, phys, debris, mobs, items, shotMob);
  if (!shotStrikeA.empty())
    return RunStrikeShot(ctx, world, sim, phys, debris, mobs, items,
                         shotStrikeA, shotStrikeStyle, shotStrikeB,
                         shotStrikeTail);
  if (rebaseline) stOpt.rebaseline = true;

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
    ui.fStainRate        = fs.fluidStainRate;
    const auto& fr = CurrentTuning().render;
    ui.fSurface      = fr.fluidSurface;
    std::memcpy(ui.fColor,  fr.fluidColor,  sizeof(ui.fColor));
    std::memcpy(ui.fColor1, fr.fluidColor1, sizeof(ui.fColor1));
    std::memcpy(ui.fColor2, fr.fluidColor2, sizeof(ui.fColor2));
    std::memcpy(ui.fColor3, fr.fluidColor3, sizeof(ui.fColor3));
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
  for (auto& m : mats) {
    ui.materialNames.push_back(m.name);
    ui.materialColors.push_back(m.gpu.color0);
  }

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

  Camera cam;
  Player player;
  Brush brush;
  PrefabPlacer placer;
  // The player avatar shares MobSystem's def list rather than loading its own:
  // one micro-body pool, and a hot reload (R) rebuilds both at once. It points
  // at whichever def is named by tuning.json player.model, so swapping
  // the player character is data, not code.  F5 re-reads it.
  std::string avatarDefName = CurrentTuning().player.model;
  PlayerAvatar avatar;
  avatar.Init(&phys, &world, &debris, mats, &mobs);
  avatar.SetDefs(&mobs.Defs(), avatarDefName);
  // THE PLAYER IS A TARGET. The avatar is a Mob but it is not in MobSystem's
  // list, so the handle-keyed lookups could not find it and an NPC's sweep
  // melted the player's limbs as debris instead of wounding them
  // (MobSystem::SetAvatar has the whole argument). Registered ONCE here, for
  // the whole session: it is a pointer to a member of this frame, and
  // Despawn/Revive rebuild the rig behind it without moving the object.
  mobs.SetAvatar(&avatar);
  ThirdPersonRig tpRig;
  CameraMode camMode = CameraMode::First;
  float avatarHeading = 0.0f;   // body facing, radians about +Y
  float fovNow = CurrentTuning().camera.fovY;
  float respawnTimer = 0.0f;
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
  // with no fist cannot be armed. Surfacing it as a list rather than resolving
  // it to one def is the whole change; a variant sidecar (zombie.json extends
  // human, so it inherits the socket) therefore appears with no UI edit.
  auto rebuildAiCreatures = [&ui, &mobs, &avatarDefName]() {
    const std::string was = (ui.aiCreaturePick >= 0 &&
                             ui.aiCreaturePick < (int)ui.aiCreatureNames.size())
                                ? ui.aiCreatureNames[ui.aiCreaturePick]
                                : std::string();
    ui.aiCreatureNames.clear();
    for (const MobDef& d : mobs.Defs())
      if (d.FindSocket("held_right") >= 0) ui.aiCreatureNames.push_back(d.name);
    // First build defaults to the avatar's own species, which is the def the
    // old code preferred when it picked for you — so the panel's behaviour is
    // unchanged until somebody touches the combo.
    const std::string want = was.empty() ? avatarDefName : was;
    ui.aiCreaturePick = 0;
    for (int i = 0; i < (int)ui.aiCreatureNames.size(); i++)
      if (ui.aiCreatureNames[i] == want) ui.aiCreaturePick = i;
  };
  rebuildAiCreatures();
  // Creatures the AI panel put in the world, so its "kill all spawned" button
  // reaps exactly those and leaves content-placed mobs alone.
  std::vector<uint64_t> aiSpawnedMobs;
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
  // seed the far-field cascades around spawn (coarsest first; the queue
  // drains at kFarListCap level-chunks per tick through SubmitTick)
  far.FullRefill({ifloor(player.pos.x) >> 4, ifloor(player.pos.y) >> 4,
                  ifloor(player.pos.z) >> 4});
  StartupMark("far-field refill queued");
  // kinematic capsule proxy so debris collides with (and is shoved by) the
  // player; terrain collision stays in the AABB controller
  uint64_t playerBody = phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);

  bool captured = true;
  // What `captured` was before the character screen took the cursor, so
  // closing hands it back rather than assuming. A player who pressed Esc to
  // free the cursor, then opened the screen, then closed it, must not have the
  // cursor grabbed out from under them.
  bool captureBeforeUi = true;
  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  double mx0 = 0, my0 = 0;
  glfwGetCursorPos(window, &mx0, &my0);
  // Look sensitivity scale while a melee weapon is up, eased rather than
  // switched (camera.meleeSensHalflife). See the note at the ApplyMouse call.
  float lookSensNow = 1.0f;

  KeyEdge eP, eN, eV, eF1, eF3, eF4, eF5, eF6, eF7, eF9, eF10, eR, eEsc, eLBracket, eRBracket, eJump,
      eG, eX, eB, eT, eO, eM, eK, eTab, eC, eH, eZ, eBack, eDel, eEq, eU, eL, eI, eQ,
      eE;
  KeyEdge eGlyph[kGlyphSlots];
  bool prevMouseL = false;
  bool prevMouseR = false;
  // RMB cast, latched until a tick actually runs (see the cast site below).
  bool castQueued = false;
  // A body part clicked in the inspector with a sentence on the stack, latched
  // the same way: the slot, or -1.
  int castAtPartQueued = -1;
  // ---- looking at things, and looting them (game/corpses.h) ----------------
  // What the reach ray found this frame (a debris body handle or 0), the
  // corpse the loot panel is open on, and whether E opened the character
  // screen to show it — so closing the loot closes the screen it opened and
  // leaves alone one the player opened themselves.
  uint64_t lookBody = 0;
  uint64_t lootCorpse = 0;
  bool lootOpenedScreen = false;
  std::vector<uint64_t> lookIgnore;   // the avatar's own limbs, per frame
  // ...and the same list for the STRIKE aim ray (the melee tick's, below).
  // Its own vector rather than a share of `lookIgnore`: that one is filled in
  // the frame block and read by the E prompt, and the melee ray runs inside
  // the tick loop, where overwriting it would silently change what E offers.
  std::vector<uint64_t> strikeIgnore;
  // RMB held (a beam stays lit while it is), and Delete pressed in magic mode
  // (drop the newest status), both read on the frame and consumed by the tick.
  bool beamHeld = false;
  bool dropStatusQueued = false;
  std::vector<Grenade> grenades;
  // Where a spell resolved, for the renderer: a short-lived burst of sprites
  // (SpellEmission::impacts). Render-only, counted down per TICK so the flash
  // lasts the same world-time at any frame rate.
  struct SpellFlash {
    Vec3 at;
    uint32_t color;
    float radius;
    int ttl, ttl0;
  };
  std::vector<SpellFlash> spellFlashes;

  // ---- magic (game/spell.h, game/caster.h) ---------------------------------
  // The VM is not player-coupled: SpellSystem takes an origin, a direction and
  // a CasterState, so a mob can drive the identical call later. PlayerCaster is
  // only the player's inventory + spoken stack, kept out of Player (which stays
  // a clean movement controller).
  SpellSystem spells;
  spells.SetLibrary(&glyphs);
  PlayerCaster caster;
  caster.inventory.GrantAllAndBind(glyphs);   // placeholder acquisition
  caster.Recompile(glyphs);
  // Health lives on PlayerAvatar's per-part hp, read through this indirection
  // so the VM never includes the avatar (thesis 4 in spell.h).
  CasterHealth playerHealth;
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
  MeleeState melee;
  // The feel numbers come from tuning.json's `melee.*` group (sim/tuning.h
  // Tuning::Melee, applied by game/melee.cpp ApplyMeleeTuning). Seeded here and
  // re-applied in the F5 block, which is the whole point of the migration: the
  // stroke's feel is a JSON edit and a keypress rather than a rebuild.
  ApplyMeleeTuning(melee.tuning);
  // ---- DISCRETE STRIKES (melee.controlMode 0, the default) -----------------
  // The player's authored-attack state: a bare StrokeCursor stepped by the
  // same StepStrokeProgram the NPCs use (game/strokes.h), driving the SAME
  // `melee` above — so the sweep, the whoosh, the Arrest and the Nudge blocks
  // below never learn which mode fed the driver. The picker owns the flick
  // read; the two ints are the press latches (see the sticky-flag note at the
  // click edges: the tick loop runs 0..4x a frame, so an unlatched click is
  // dropped ~8 frames of 9).
  StrikePicker strikePicker;
  StrokeCursor playerStrike;
  int strikeQueued = -1;    // style index latched at the press, -1 = none
  int strikeBuffered = -1;  // ONE strike banked mid-swing, fired at recover
  Inventory hotbar;
  // The rest of the kit: worn/sheathed/quick slots and the pack
  // (game/equipment.h). Held beside the hotbar rather than inside it because
  // the hotbar is WHAT IS IN YOUR HAND and predates all of this; the melee
  // path reads Inventory::Selected() and must keep doing exactly that.
  PlayerKit kit;
  // What we last ASKED the body to wear, per equip slot. Not a second copy of
  // the equipment — it is the record that keeps a REFUSED piece (a helm on a
  // creature with no head) from being retried thirty times a second. Cleared
  // whenever the avatar is rebuilt, because a fresh rig wears nothing and
  // every slot has to be offered to it again.
  std::string wearTried[kEquipSlotCount];
  // ...and IN WHAT COLOUR. The sync below compares by item NAME, which is what
  // makes it survive an R reload — and a name alone cannot tell a red tunic
  // from a blue one, so dyeing a garment you are already wearing would change
  // nothing until you took it off. The dye is part of the comparison for the
  // same reason the name is the rest of it. 0 = undyed (game/dye.h).
  uint32_t wearDye[kEquipSlotCount] = {};
  // ---- THE SHEATH IS THE WEAPON SLOT -------------------------------------
  //
  // A blade is either DRAWN (a real rig part in the fist, swinging) or STOWED
  // (an entry in the Sheath equip slot and nothing else). Q toggles. The
  // hotbar stays exactly what it was — WHAT IS IN YOUR HAND for consumables
  // and tools, still selected by the number row — it simply stops being where
  // a weapon comes from, because "the sword you are carrying" and "the potion
  // you have selected" were never the same question.
  //
  // `toolBeforeDraw` is what the tool selector goes back to on stow. Drawing
  // forces the melee tool (otherwise you draw a sword and the left mouse
  // button still paints stone), and silently keeping it afterwards would
  // strand the player in a mode they never chose.
  SheathState sheath;
  sheath.toolBefore = UIState::kToolBrush;
  const int kSheathSlot = (int)EquipSlotId::Sheath;
  // What the Sheath slot holds, as a kind. Written as a lambda because both
  // the key press and the per-frame reconcile ask, and re-deriving it at each
  // site is how the two stop agreeing.
  auto sheathKind = [&] {
    const ItemStack& sh = kit.equip.At(kSheathSlot);
    const ItemDef* d = items.At(sh.Empty() ? -1 : sh.def);
    return d ? d->kind : ItemKind::None;
  };
  {
    // ---- THE STARTING KIT, AND IT IS STILL A STUB ------------------------
    //
    // One of everything the library defines, put somewhere it can actually be
    // USED rather than all of it in the hotbar: a weapon goes to the sheath
    // (which is where the draw key looks), armour goes to the pack (so it can
    // be dragged onto the figure), and anything else keeps the hotbar. Before
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
        if (!EquipSlotIsWorn(s) && EquipSlotAccepts(s, it.kind) &&
            kit.equip.At(s).Empty())
          home = s;
      if (home >= 0)
        kit.equip.slots[home] = {i, 1};       // the sword, into the sheath
      else if (ItemKindIsWorn(it.kind))
        kit.bag.Add(i, 1);                     // armour, into the pack
      else
        hotbar.Add(i, 1);
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
  }
  // Burn-material ids for the inspector's charred readout, resolved ONCE here
  // and again after every materials reload — never per frame (see ResolveBurnMats).
  BurnMats burnMats = ResolveBurnMats(mats);
  TissueMats tissueMats = ResolveTissueMats(mobs);
  // The blade's position last tick, so the sweep has something to sweep FROM.
  // Invalid until the first tick with a weapon drawn — a swing that started
  // from an unknown pose would carve a segment the blade never travelled.
  Vec3 lastEdgeBase{}, lastEdgeTip{};
  bool lastEdgeValid = false;
  // Rig slots the player's CURRENT swing has already delivered a blunt/bite
  // impulse to (melee.h EdgeSweep::struck). Owned here rather than on either
  // cursor because the player has two cut states -- the discrete program's and
  // the freeform driver's -- and one swing must mean one impulse in both.
  std::vector<uint64_t> playerStruck;
  // ...and whether this swing has already BITTEN (melee.h EdgeSweep::bitten).
  // Once per STROKE rather than once per slot, and cleared on the same line
  // `playerStruck` is, for the same two-cut-states reason.
  bool playerBitten = false;
  // --duel-dummy fires once, from inside the tick loop (see the note there).
  bool duelDummySpawned = false;

  // particle-pass gating: tick-deterministic inputs only (see SubmitTick note)
  bool everExploded = false;
  uint32_t lastExplosionTick = 0;
  // MLS-MPM fluid (docs/PLAN_mpm_fluids.md): the CPU's CONSERVATIVE live
  // estimate — the GPU owns the real count now (settle kills particles,
  // excite births them; the seam's compaction maintains fluidArgs[FA_LIVE]).
  // Refreshed from the snapshot readback each frame, bumped by spawns
  // submitted since that snapshot's tick so a fresh pour never reads as
  // empty. Drives record/skip, draw counts and the HUD only — every kernel
  // re-bounds itself on the GPU count. Not persisted.
  uint32_t fluidCount = 0;
  // Spawns submitted after the newest snapshot's tick: (tick, count) pairs,
  // dropped once a snapshot at/after their tick arrives (the GPU count now
  // includes them).
  std::vector<std::pair<uint32_t, uint32_t>> fluidPendingSpawns;
  // Splash sound cue: fired once per snapshot tick that reports a burst of
  // excitement, voiced through water's Impact slot (the Break precedent —
  // audio is presentation-only and reads the same readback).
  uint32_t lastFluidCueTick = 0;
  uint32_t fluidCueMat = 0;
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == "water") { fluidCueMat = (uint32_t)i; break; }
  // Material id each MPM species splashes micro droplets as, recorded from the
  // pour's brush material (TickParams.fluidSplashMat). Species 0 defaults to
  // water so the fluid tool pours water without an explicit key press.
  uint32_t fluidSpeciesMat[4] = {fluidCueMat, 0, 0, 0};
  // Last tick the MPM fluid was live: keeps the particle passes awake for the
  // splash droplets (see particlesActive below).
  uint32_t lastFluidTick = 0;
  // ---- fluid lab (lab/lab.h) ----
  // labTick is the SCENE clock: 1-based from worldgen (or the last L reset),
  // driving the build-op and pour schedules. Resetting it to 0 IS the scene
  // reset — the next sim tick re-submits the build ops (which cover the whole
  // scene volume, air included) and the pour replays identically, while the
  // world outside the scene box is untouched (no re-worldgen).
  uint32_t labTick = 0;
  // tuning.json watcher (~4 Hz, lab only): mtime of the file content this
  // process last LOADED (or wrote). A newer file on disk triggers the F5
  // path; the ImGui writeback refuses to clobber anything newer than this.
  const std::string labTuningPath = assetDir + "/materials/tuning.json";
  int64_t labTuningMtime =
      labScene >= 0 ? LabFileMtimeNs(labTuningPath) : -1;
  double labWatchPoll = 0.0;
  uint32_t tick = 0;
  uint32_t bodyInstCount = 0;
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
  struct HitStop {
    float pendScale = 1.0f;   // requested this frame; 1 = nothing requested
    float pendMs = 0.0f;
    float timeLeft = 0.0f;    // REAL seconds remaining in the live dip
    float scale = 1.0f;       // the live dip's multiplier
    void Request(float s, float ms) {
      if (ms <= 0.0f || s >= 1.0f) return;
      pendScale = std::min(pendScale, s);
      pendMs = std::max(pendMs, ms);
    }
  } hitStop;
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
  struct CombatCueRequest {
    bool pending = false;
    float power = 0.0f;   // 0..1
    Vec3 at{};
  };
  CombatCueRequest combatWhooshCue, combatFleshCue, combatClangCue,
                   combatStrikeCue, combatCutCue;
  bool combatStrikeEdged = true;
  // THE WHOOSH IS THE ONE CUE THAT IS NOT AN INSTANT, so its voice is kept and
  // moved. Handle from Cues::Combat; self-invalidating, so nothing here has to
  // know when the sample ended (audio/world.h PlayOneShotTracked).
  int combatWhooshVoice = -1;
  // The swing whoosh fires on the EDGE into Slash, not while Slash is held:
  // committing is a moment, and a per-tick test would play the sample five
  // times over one cut. Remembered across frames, so the edge survives a frame
  // that ran no ticks at all.
  SwingPhase meleePhasePrev = SwingPhase::Idle;
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
  uint64_t frameCounter = 0;
  StartupMark("frame loop entered");
  uint64_t startupFrame = 0;
  while (!glfwWindowShouldClose(window)) {
    if (g_harnessFrames > 0) {
      frameCounter++;
      // The mid-run reload verifies the F5 path, but it also recompiles every
      // pipeline in the foreground and the far set on three background
      // threads for the next ~40 s, which is half the run — so the frame-time
      // tail of a default `--frames` run is the compiler, not the game.
      // SANDVOX_FRAMES_NO_RELOAD=1 is the measurement arm.
      static const bool noReload =
          std::getenv("SANDVOX_FRAMES_NO_RELOAD") != nullptr;
      if (frameCounter == g_harnessFrames / 2 && !noReload) {
        std::printf("--frames harness: triggering shader reload (F5 path)\n");
        ui.reloadShaders = true;
      }
      if (frameCounter >= g_harnessFrames) glfwSetWindowShouldClose(window, 1);
    }
    // The park probe is tick-scheduled, so it decides its own end: --frames
    // only has to be generous enough to reach it.
    if (g_parkDone) glfwSetWindowShouldClose(window, 1);

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
              kit.equip.slots[s] = {i, 1};
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
      if (frameCounter == kShotInvGearFrame + 1) ui.inspectMode = true;
      // Third picture: the grimoire with a page selected and its word row
      // populated — the panel's job is to be looked at (plan §12c).
      if (frameCounter == kShotInvCaptureFrame + 1) {
        ui.inspectMode = false;
        ui.grimoireMode = true;
        if (!glyphs.conjoined.empty()) {
          const ConjoinedGlyph& cg = glyphs.conjoined[0];
          ui.grimoireSelected = cg.id;
          ui.grimoireEditName = cg.id;
          ui.grimoireEditWords.clear();
          for (int gi : cg.glyphs)
            if (const GlyphDef* d = glyphs.At(gi)) ui.grimoireEditWords.push_back(d->id);
        }
      }
      // Fourth picture: a corpse with its gear on, opened. A human is spawned
      // a few paces ahead, dressed in every worn piece the library has and
      // handed a sword, and killed — the same CorpseReport path a fight
      // produces — and the panel is opened on it as E would.
      if (frameCounter == kShotInvGrimoireFrame + 1) {
        ui.grimoireMode = false;
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
                worn.slots[slot] = {items.Find(it.name), 1};
                dressed++;
              }
            }
            if (const ItemDef* sword = items.At(items.Find("sword")))
              mobs.EquipItem(id, sword);
            if (Mob* m = mobs.FindMobById(id)) m->Die();
          }
          const CorpseReport* c = corpses.Find(id);
          if (c) {
            lootCorpse = id;
            ui.lootOpen = true;
            ui.lootTitle = c->def;
          }
          std::printf("--shot-inventory: corpse of a human in %d worn pieces, "
                      "%zu lootable%s\n",
                      dressed, c ? c->gear.size() : (size_t)0,
                      c ? "" : " (NO CORPSE REPORTED)");
        } else {
          std::fprintf(stderr, "--shot-inventory: no \"human\" mob def\n");
        }
      }
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
    const bool gameKeys = !ui.inventoryOpen && !uiTyping;

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
      if (ui.inventoryOpen) {
        ui.inventoryOpen = false;
        captured = captureBeforeUi;
        ui.portraitYaw = 0.0f;  ui.portraitPitch = -0.08f;
        ui.portraitZoom = 1.0f; ui.portraitZoomTarget = 1.0f;
        ui.portraitPanX = 0.0f; ui.portraitPanXTarget = 0.0f;
        ui.portraitPanY = 0.0f; ui.portraitPanYTarget = 0.0f;
        ui.portraitPivotSlot = -1;
        ui.inspectSelected = -1;
      } else {
        captured = !captured;
      }
      glfwSetInputMode(window, GLFW_CURSOR,
                       captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
      glfwGetCursorPos(window, &mx0, &my0);
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
        if (camMode != CameraMode::First)
          tpRig.Zoom((float)dy);
        else
          hotbar.Scroll(dy > 0 ? -1 : 1);
      }
    }
    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    // THE VIEW SLOWS WHILE THE BLADE IS UP; THE BLADE DOES NOT.
    //
    // The same delta drives both the camera and the swing, which is what makes
    // the weapon feel attached to the hand — but at equal gain a cut you want
    // to WATCH also whips the view off the target, so the swing you just made
    // leaves the screen before you see it land. Scaling only the look leaves
    // the mouse stroke buying mostly arm instead of mostly yaw, which is the
    // whole point: you are steering a blade, not aiming a gun.
    //
    // Eased on a half-life rather than switched, because clicking mid-stroke
    // would otherwise step the view. Keyed off the swing PHASE rather than the
    // button so the slowdown covers the recover tail too and hands the view
    // back as the weapon settles. One frame latent (the phase is advanced in
    // the tick loop below) and imperceptibly so.
    {
      const auto& ct = CurrentTuning().camera;
      // FREEFORM ONLY (controlMode 1): the damping exists because the mouse
      // steers the blade and the view at once. A discrete strike never reads
      // the mouse mid-stroke, so damping the look there would just make the
      // camera feel sticky for the length of every swing.
      const bool bladeUp = CurrentTuning().melee.controlMode == 1 &&
                           melee.Phase() != SwingPhase::Idle;
      const float want = bladeUp ? ct.meleeSensitivity : 1.0f;
      const float hl = ct.meleeSensHalflife;
      const float k = hl > 1e-4f ? 1.0f - std::pow(0.5f, dt / hl) : 1.0f;
      lookSensNow += (want - lookSensNow) * k;
    }
    // --fell-tree with a site OWNS the camera: the first frame's cursor delta
    // (wherever the mouse happened to be when the window opened) turned the
    // view 88 degrees and planted the oak in the wrong place with 0 cells,
    // and a hand on the mouse mid-run would move the very view the drawBodies
    // number is measured through.
    if (captured && !g_fellSiteSet)
      cam.ApplyMouse((float)(mx - mx0) * lookSensNow,
                     (float)(my - my0) * lookSensNow);
    // The swing gets the RAW delta — deliberately not scaled with the view
    // above. MeleeTuning::commitSpeed is calibrated in true mouse pixels per
    // second, so damping the input here would move the commit threshold every
    // time somebody retunes the camera, and it would also shrink the cut the
    // player physically made. Fed per FRAME, because that is the rate the
    // mouse is sampled at; the tick loop below runs 0..4 times per frame and
    // integrating it there would multiply-count a fast flick into a much
    // faster one.
    //
    // UNDER HIT-STOP THE STROKE INTEGRATES IN SIM TIME, and that is the
    // decision rather than an oversight. During a dip the tick loop runs less
    // often, so the same frame's pixels sit in MeleeState's accumulator across
    // more frames and are delivered to fewer Update() calls. Two consequences,
    // and they pull opposite ways:
    //
    //   * THE TIP DISPLACEMENT IS EXACT. az/el are integrated from the raw
    //     delta at a fixed radians-per-pixel gain, so total pixels -> total
    //     arc is preserved bit for bit however the ticks are spaced. This is
    //     the property the `swing` gate states ("a displacement, not a rate")
    //     and the one the player actually feels; nothing here may perturb it,
    //     which rules out scaling the fed pixels.
    //   * THE DERIVED SPEED READS HIGH, by 1/scale, because Update divides by
    //     kTickDt and a tick now covers more real time than that. It is
    //     bounded and it is nearly unreachable: `commitSpeed` is only
    //     consulted in Guard, and a dip can only have been caused by a hit,
    //     which can only happen in Slash. By the time the longest dip (140 ms)
    //     has run out, a 170 ms slash is into its follow-through.
    //
    // Fixing the second would mean giving Update a second dt (real vs sim),
    // widening a signature three callers and the NPC driver share, to correct
    // a number that is not read in the window where it is wrong.
    //
    // MODE SPLIT (D10 of the discrete-strikes plan): in discrete mode the
    // driver is fed by the stroke program in the tick loop, so raw pixels go
    // to the PICKER instead — feeding both would double-integrate the same
    // motion into the accumulator and bend every authored cut.
    if (captured) {
      if (CurrentTuning().melee.controlMode == 1)
        melee.Feed((float)(mx - mx0), (float)(my - my0));
      else
        strikePicker.Feed((float)(mx - mx0), (float)(my - my0), dt);
    }
    mx0 = mx;
    my0 = my;

    // The DEV tier: still live with the character screen open, dead while an
    // ImGui field has focus.
    if (devKeys && eP.Pressed(key(GLFW_KEY_P))) ui.paused = !ui.paused;
    if (devKeys && eN.Pressed(key(GLFW_KEY_N))) ui.stepOnce = true;
    if (devKeys && eV.Pressed(key(GLFW_KEY_V))) ui.fly = !ui.fly;
    if (devKeys && eF1.Pressed(key(GLFW_KEY_F1))) ui.visible = !ui.visible;
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
    if (gameKeys && eLBracket.Pressed(key(GLFW_KEY_LEFT_BRACKET)))
      ui.brushRadius = std::max(1, ui.brushRadius - 1);
    if (gameKeys && eRBracket.Pressed(key(GLFW_KEY_RIGHT_BRACKET)))
      ui.brushRadius = std::min(7, ui.brushRadius + 1);
    // The number row is SHARED: it picks a brush material normally and SPEAKS
    // glyphs in magic mode (Z). Both wanted 1-8 and the brush binding predates
    // magic, so a mode toggle is what keeps the existing tool usable rather
    // than silently stealing its keys.
    if (!ui.magicMode) {
      for (int i = 0; i < 8; i++)
        if (gameKeys && key(GLFW_KEY_1 + i) && i + 1 < (int)mats.size()) {
          if (ui.tool == UIState::kToolFluid)
            ui.fluidSpecies = i & 3;
          else
            ui.brushMaterial = i + 1;
        }
    } else {
      // Pressing a number SPEAKS that glyph — it never casts. Edge-triggered:
      // a held key must not stutter the same word onto the stack. TWO BANKS
      // (plan §12a): `1`-`0` speak bank A, `Shift+1`-`0` bank B. Sprint is
      // on Shift outside magic mode and magic mode captures the number row,
      // so nothing collides.
      const bool bankB = key(GLFW_KEY_LEFT_SHIFT) || key(GLFW_KEY_RIGHT_SHIFT);
      for (int i = 0; i < kGlyphBank; i++) {
        // GLFW's number row is contiguous 1..9 then 0, and slot 10 is the 0
        // key, matching the strip the HUD prints.
        int k = (i == 9) ? GLFW_KEY_0 : (GLFW_KEY_1 + i);
        if (captured && eGlyph[i].Pressed(key(k)))
          caster.SpeakSlot(glyphs, i + (bankB ? kGlyphBank : 0));
      }
      // `=` captures the sentence on the stack into the grimoire (§12b): the
      // fast path for "that worked, make it one key".
      if (captured && eEq.Pressed(key(GLFW_KEY_EQUAL))) caster.CaptureStack(glyphs);
    }
    if (captured && eZ.Pressed(key(GLFW_KEY_Z))) {
      ui.magicMode = !ui.magicMode;
      if (!ui.magicMode) caster.Clear(glyphs);   // leaving mode abandons the spell
    }
    // Abandon a half-spoken spell. Backspace rather than a letter: it is the
    // universal "undo what I just typed" key and the left hand is on WASD.
    if (captured && eBack.Pressed(key(GLFW_KEY_BACKSPACE))) caster.Clear(glyphs);

    if (captured && eG.Pressed(key(GLFW_KEY_G))) {
      Grenade g;
      g.pos = player.EyePos() + cam.Forward() * 2.0f;
      g.vel = cam.Forward() * (CurrentTuning().grenade.throwSpeed / kVoxelMeters) +
              player.vel;
      g.fuse = CurrentTuning().grenade.fuse;
      grenades.push_back(g);
    }
    if (captured && eX.Pressed(key(GLFW_KEY_X))) ui.pendingDetonate = true;
    // DRAW / STOW. Q rather than the X the plan proposed: X already
    // detonates, and a key that does two things is a key that does the wrong
    // one under pressure.
    if (captured && eQ.Pressed(key(GLFW_KEY_Q)) &&
        !sheath.Toggle(sheathKind(), UIState::kToolMelee, ui.tool)) {
      // Reaching for a sword that is not there says so, rather than silently
      // arming an empty hand — the same rule the equipment panel's refusals
      // follow.
      ui.kitMessage = "your sheath is empty";
      ui.kitMessageAge = 0.0f;
    }
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
    ui.lookPrompt.clear();
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
      // The ground registry FIRST: a shed robe still lying in the heap it
      // came off reads as the robe, not as the corpse (ShedCorpseLoot).
      if (const WorldItem* w = lookBody ? ground.Find(lookBody) : nullptr) {
        ui.lookPrompt = "E  pick up " + w->item;
      } else if (const CorpseReport* c = corpses.FindByBody(lookBody)) {
        ui.lookPrompt = c->gear.empty() ? c->def + "  -  nothing left on it"
                                        : "E  loot " + c->def;
      }
    }
    if (captured && eE.Pressed(key(GLFW_KEY_E))) {
      const uint64_t hit = lookBody;
      const WorldItem* w = hit ? ground.Find(hit) : nullptr;
      const CorpseReport* corpse = w ? nullptr : corpses.FindByBody(hit);
      if (corpse && !corpse->gear.empty()) {
        // OPEN THE CORPSE: the character screen with its loot panel up. The
        // cursor dance is the I key's, and `lootOpenedScreen` remembers that
        // it was E who opened the screen so closing the loot closes it again.
        lootCorpse = corpse->mobId;
        ui.lootOpen = true;
        ui.lootTitle = corpse->def;
        if (!ui.inventoryOpen) {
          lootOpenedScreen = true;
          ui.inventoryOpen = true;
          captureBeforeUi = captured;
          captured = false;
          glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
          glfwGetCursorPos(window, &mx0, &my0);
        }
      } else if (w) {
        const int di = items.Find(w->item);
        // Bag first, hotbar as the overflow. A full pack REFUSES rather than
        // silently swallowing or silently dropping: the item stays exactly
        // where it was, which is the only behaviour under which a pickup
        // cannot lose anything.
        // ...and in the colour it was lying there in (game/dye.h). The
        // registry carries the word because the art cannot: a dyed garment is
        // painted in neutral greys, so a pickup that forgot the dye would hand
        // back a grey tunic with nothing anywhere to say it had ever been red.
        int where = di >= 0 ? kit.bag.Add(di, 1, w->dye) : -1;
        if (where < 0 && di >= 0) where = hotbar.Add(di, 1, w->dye);
        if (where >= 0) {
          ui.kitMessage = "picked up " + w->item;
          // Order matters: the registry entry is dropped by the release hook
          // when the body goes, so this is one call, not two.
          debris.DestroyBody(hit);
        } else {
          ui.kitMessage = "you have no room for that";
        }
        ui.kitMessageAge = 0.0f;
      }
    }
    if (captured && eTab.Pressed(key(GLFW_KEY_TAB))) {
      ui.tool = (ui.tool + 1) % UIState::kToolCount;
      if (ui.tool == UIState::kToolFluid && fluidCueMat != 0)
        ui.brushMaterial = (int)fluidCueMat;
    }
    if (captured && eM.Pressed(key(GLFW_KEY_M))) ui.spawnMob = true;
    if (captured && eB.Pressed(key(GLFW_KEY_B))) ui.placePrefab = true;
    if (captured && eK.Pressed(key(GLFW_KEY_K))) ui.spawnSphere = true;
    // U clears the experimental MLS-MPM fluid (sticky flag, consumed in the
    // tick loop like every other one-shot input — see the cast-key note).
    if (captured && eU.Pressed(key(GLFW_KEY_U))) ui.clearFluid = true;
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
    if (captured && eH.Pressed(key(GLFW_KEY_H)) && avatar.Spawned()) {
      static const char* kSeverOrder[] = {
          "staff",  "hand.R", "hand.L", "armL.R", "armL.L",
          "foot.R", "foot.L", "legL.R", "legL.L", "armU.R",
          "armU.L", "legU.R", "legU.L", "head"};
      for (const char* nm : kSeverOrder)
        if (avatar.SeverByName(nm)) break;
    }
    if (gameKeys && ui.tool == UIState::kToolPrefab &&
        eT.Pressed(key(GLFW_KEY_T)))
      ui.prefabRot = (ui.prefabRot + 1) & 3;
    if (gameKeys && ui.tool == UIState::kToolPrefab &&
        eO.Pressed(key(GLFW_KEY_O)) && !prefabs.empty())
      ui.prefabSelected = (ui.prefabSelected + 1) % (int)prefabs.size();

    // MOVEMENT WAS NEVER GATED. It predates every other binding here and was
    // read straight off the keyboard, so WASD walked the player while a dev
    // panel field had focus and would walk them around behind the character
    // screen. `gameKeys` is the fix, and the axes are left at zero rather than
    // frozen so the controller decelerates properly instead of holding the
    // last input.
    PlayerInput pin;
    if (gameKeys) {
      pin.forward = (key(GLFW_KEY_W) ? 1.f : 0.f) - (key(GLFW_KEY_S) ? 1.f : 0.f);
      pin.strafe = (key(GLFW_KEY_D) ? 1.f : 0.f) - (key(GLFW_KEY_A) ? 1.f : 0.f);
      pin.up = key(GLFW_KEY_SPACE);
      pin.down = key(GLFW_KEY_LEFT_CONTROL);
      pin.sprint = key(GLFW_KEY_LEFT_SHIFT);
      pin.jumpPressed = eJump.Pressed(key(GLFW_KEY_SPACE));
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
      pin.sprint = false;
      cam.yaw = 0.6f;
      cam.pitch = -0.12f;
      pin.jumpPressed = (frameCounter == kShotJumpAtFrame);
    }
    // --autofly: hold W+sprint in fly mode, no human at the keyboard. Exists to
    // reproduce the streaming-shift stutter, which only appears when the window
    // origin moves several chunks per second.
    if (g_autoWalk) {
      player.fly = false;
      ui.fly = false;
      pin.forward = 1.f;
      pin.strafe = 0.f;
      pin.sprint = false;
      static uint32_t lastHopTick = ~0u;
      if (tick % 45u == 0u && lastHopTick != tick) {
        pin.jumpPressed = true;
        lastHopTick = tick;
      }
      // A fixed tick schedule, like the autofly phases: reproducible run to run.
      cam.yaw = 2.35f + (float)((tick / 240u) % 4u) * 1.5707963f;
    }
    if (g_autofly) {
      player.fly = true;
      ui.fly = true;
      pin.forward = 1.f;
      pin.strafe = 0.f;
      pin.sprint = true;
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
        pin.down = true;   // descend into solid rock: worst case for residency
      }
      // --autofly-surface: the RENDERER's worst case. Fly forward over the
      // terrain at two altitudes on the same fixed `tick/90` phase form the
      // hard descent uses, so the two harnesses are directly comparable and
      // both are reproducible run to run.
      //
      // WHY THE ALTITUDE IS HELD ANALYTICALLY, not by pin.up/pin.down: the
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
        pin.up = false;
        pin.down = false;
        g_autoflySurfaceHigh = (phase == 1u);
        // Park: hold ONE regime and stop the forward axis. The regime hop is
        // 115 voxels, i.e. a 7-chunk vertical shift, so leaving it alternating
        // would keep the streaming wake this probe exists to remove.
        if (g_autoflyPark && tick >= ParkFlyTicks()) {
          pin.forward = 0.f;
          pin.sprint = false;
          g_autoflySurfaceHigh = false;
        }
      }
    }

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
          ui.fStainRate        = fs.fluidStainRate;
          const auto& fr = tune.render;
          ui.fSurface      = fr.fluidSurface;
          std::memcpy(ui.fColor,  fr.fluidColor,  sizeof(ui.fColor));
          std::memcpy(ui.fColor1, fr.fluidColor1, sizeof(ui.fColor1));
          std::memcpy(ui.fColor2, fr.fluidColor2, sizeof(ui.fColor2));
          std::memcpy(ui.fColor3, fr.fluidColor3, sizeof(ui.fColor3));
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
        // F5 re-reads tuning, so it re-reads which layer is named and what is
        // in it. Re-queued against the CURRENT window, so an edit saved from
        // the tuner appears on the next keypress instead of the next restart.
        LoadWorldEditLayerFromTuning(assetDir);
        WorldEditLayer().QueueWindow(world);
      }
      std::printf("reloading shaders... %s\n",
                  sim.ReloadShaders(ctx.device) ? "ok" : "FAILED (kept old)");
    }
    if (ui.reloadMaterials) {
      ui.reloadMaterials = false;
      std::vector<MaterialDef> newMats;
      std::vector<ReactionGpu> newReactions;
      if (LoadAssets(assetDir + "/materials/materials.json",
                     assetDir + "/materials/reactions.json", newMats, newReactions,
                     errors)) {
        mats = std::move(newMats);
        reactions = std::move(newReactions);
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
            std::printf("glyphs reloaded (%zu)\n", glyphs.glyphs.size());
          } else {
            std::fprintf(stderr, "glyph reload failed:\n%s", gerr.c_str());
          }
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
        LoadMobDefs(assetDir + "/mobs", mats, mobDefs, mbSet, mlog);
        if (!mlog.empty()) std::fprintf(stderr, "%s", mlog.c_str());
        // Items MUST reload here too: their bricks live in the pool that was
        // just thrown away, so a stale ItemDef would hold a model index into
        // a freed model.
        //
        // AND EVERY SLOT THAT HOLDS AN ITEM INDEX MUST BE RE-RESOLVED. The
        // hotbar, the pack and the equipment all store indices into
        // ItemLibrary::items, which is file-order dependent; an items.json
        // that merely reorders entries would otherwise turn a sheathed sword
        // into whatever now sits at that index. This block used to carry a
        // comment promising the hotbar was "re-validated below" — it was not,
        // and the character screen makes the consequence permanent rather than
        // transient, so the promise is kept here for all three containers.
        {
          // Name, count AND DYE. The dye is the other half of what a stack is
          // (game/dye.h): dropping it here would bleach every coloured garment
          // the player owns on every R, which reads as a rendering bug rather
          // than as the data loss it is.
          struct KitSnap {
            std::string name;
            int count = 0;
            uint32_t dye = 0;
          };
          auto snapshot = [&](ItemStack* v, int n,
                              std::vector<KitSnap>& out) {
            out.clear();
            for (int i = 0; i < n; i++)
              out.push_back({KitItemName(v[i], items), v[i].count, v[i].dye});
          };
          auto restore = [&](ItemStack* v, int n,
                             const std::vector<KitSnap>& in) {
            for (int i = 0; i < n && i < (int)in.size(); i++) {
              const ItemStack s = KitItemFromName(in[i].name, in[i].count,
                                                  items, in[i].dye);
              if (!in[i].name.empty() && s.Empty())
                std::fprintf(stderr,
                             "items reload: \"%s\" is gone; slot emptied\n",
                             in[i].name.c_str());
              v[i] = s;
            }
          };
          std::vector<KitSnap> hb, bg, eq;
          snapshot(hotbar.slots, kItemSlots, hb);
          snapshot(kit.bag.slots, Bag::kSlots, bg);
          snapshot(kit.equip.slots, kEquipSlotCount, eq);

          std::string ierr;
          LoadItems(assetDir + "/items", mats.size(), mbSet, items, ierr);
          if (!ierr.empty()) std::fprintf(stderr, "%s", ierr.c_str());

          restore(hotbar.slots, kItemSlots, hb);
          restore(kit.bag.slots, Bag::kSlots, bg);
          restore(kit.equip.slots, kEquipSlotCount, eq);
          // The AI panel's weapon picker is a mirror of the same library, and
          // by name for the same reason the slots above are: a new blade in
          // items.json appears in the combo on this R without disturbing what
          // is already selected.
          rebuildAiWeapons();
          // ...and the wardrobe's three, for the same reason and by the same
          // rule: a pattern added to items.json appears on this R without
          // moving what is already picked.
          rebuildWardrobe();
          rebuildAiWear();
        }
        sim.UploadMicroBodies(ctx.queue, mbSet);
        mobs.SetDefs(std::move(mobDefs));
        // After SetDefs, not before: the creature list is the LIVE defs, so a
        // sidecar added or renamed on this R has to be in place first.
        rebuildAiCreatures();
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
        tissueMats = ResolveTissueMats(mobs);
        ui.materialNames.clear();
        ui.materialColors.clear();
        for (auto& m : mats) {
          ui.materialNames.push_back(m.name);
          ui.materialColors.push_back(m.gpu.color0);
        }
        std::printf("materials reloaded (%zu, %zu reactions)\n", mats.size(),
                    reactions.size());
      } else {
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
      player.viewYOffset = 0.0f;  // teleport: never smooth across it
      tick = 0;
      grenades.clear();
      everExploded = false;
      fluidCount = 0;  // MPM fluid does not survive a regen (the worldgen
      fluidPendingSpawns.clear();  // table zeroes the GPU count + calm state)
      debris.Reset();
      mobs.Reset();
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
      // entities.sve sections registered in game/persist.cpp.
      PlayerKitRefs kitRefs{&caster, &glyphs, &hotbar, &kit, &items};
      WorldItemRefs groundRefs{&ground, &phys, &debris, &mbSet, &items};
      EntityIO eio = MakeEntityIO(debris, mobs, &avatar, &kitRefs, &groundRefs);
      SaveWorld(ctx, world, stream, "world.svd", mats, &eio);
    }
    if (ui.loadWorld) {
      ui.loadWorld = false;
      ctx.WaitIdle();
      PlayerKitRefs kitRefs{&caster, &glyphs, &hotbar, &kit, &items};
      WorldItemRefs groundRefs{&ground, &phys, &debris, &mbSet, &items};
      EntityIO eio = MakeEntityIO(debris, mobs, &avatar, &kitRefs, &groundRefs);
      if (LoadWorld(ctx, world, sim, stream, "world.svd", mats, &eio)) {
        // Debris/mobs were reset and reloaded by their sections; the avatar
        // was despawned by its reset and respawns on the next tick, applying
        // the saved damage state (avatar.h persistence note). Only main's own
        // transient state is cleared here.
        grenades.clear();
        everExploded = false;
        fluidCount = 0;  // MPM fluid is not in the save format: saves
        fluidPendingSpawns.clear();  // force-settle (loadReset zeroes the
                                     // GPU count + calm state)
        tpRig.Snap();
      }
    }

    // ---- player (per frame, against the latest one-tick-latent mirror) ----
    player.fly = ui.fly;
    // Excited MPM water folds into the liquid answer BEFORE the voxel
    // mirror: swimming, buoyancy and the waterline frame work identically
    // whichever representation the water happens to be in (plan §6.5). The
    // >= 2 eighths floor keeps a lone stray droplet from reading as a pool.
    auto kindAt = [&](IVec3 c) {
      if (world.FluidEighthsAt(c) >= 2) return CellKind::Liquid;
      return world.KindAt(c, classOf);
    };
    // Dismemberment drives movement: the active AnimStateRule's speedScale and
    // the leg-liveness-derived jump scale come straight from the avatar, so
    // losing a leg slows the player down and losing both stops them jumping.
    // Fly mode deliberately ignores all of it — a debug camera should not be
    // crippled by the character's injuries.
    {
      const AvatarLocomotion loco = avatar.Locomotion();
      const bool couple = avatar.Spawned() && !player.fly;
      player.speedScale = couple ? loco.speedScale : 1.0f;
      player.jumpScale = couple ? loco.jumpScale : 1.0f;
      player.canJump = couple ? loco.canJump : true;
    }
    // ---- component 9: impact ripples -------------------------------------
    // The event source, and it is a RISING EDGE rather than a per-frame test:
    // an impact happens once. Sampled around player.Update because the entry
    // speed is what sizes the splash and it is gone a frame later.
    //
    // Render-only, bounded by the ring, and it is deliberately NOT an audio
    // cue's twin — a cue fires on the same event but through a different
    // system, and coupling them would make one of them the other's trigger.
    {
      static bool wasInLiquid = false;
      const float enterSpeed = -player.vel.y;
      // A limp or rising body owns the player, not the controller: no
      // input, no gravity, no sweeps. The capsule is moved onto the body
      // after each physics step (PlayerAvatar::RagdollFollow, below).
      if (avatar.Spawned() && avatar.Ragdolled()) {
        player.vel = {};
      } else {
        player.Update(dt, pin, cam.FlatForward(), cam.Right(), cam.Forward(),
                      kindAt);
      }
      if (player.inLiquid && !wasInLiquid && enterSpeed > 2.0f) {
        // Crest height in metres, from the entry speed, capped: a splash from
        // a great fall is bigger, but not without limit — an unbounded
        // amplitude here would tilt the surface normal past the shore and the
        // whole lake would go black (the ripple steepness note in
        // raymarch.wgsl is the same trap).
        const float amp = std::min(0.02f + enterSpeed * 0.004f, 0.12f);
        WaveImpacts().Add(player.pos.x, player.pos.z, (float)now, amp);
      }
      wasInLiquid = player.inLiquid;
    }
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
    // --autofly-surface altitude pin. Held analytically against the worldgen
    // heightfield rather than flown, so the measured quantity (ray length
    // through unskipped chunks) depends only on the tick schedule — see the
    // block beside g_autoflyHard for why. Fly mode has no terrain collision, so
    // assigning the position outright is legal here; vel.y is zeroed so the
    // integrator does not carry an accumulated climb into the next frame and
    // fight this every step.
    if (g_autoflySurface) {
      const int gh = World::TerrainHeight((int)std::floor(player.pos.x),
                                          (int)std::floor(player.pos.z),
                                          kDefaultSeed);
      player.pos.y = (float)gh + (g_autoflySurfaceHigh ? kAutoflySurfaceHighVox
                                                       : kAutoflySurfaceLowVox);
      player.vel.y = 0.0f;
      // Park: latch the pose once and re-assign it every frame. Zeroing the
      // input axis is not enough on its own — fly mode integrates velocity, so
      // a coast of even a few voxels crosses a chunk boundary and shifts the
      // window, which is precisely the stimulus under test.
      if (g_autoflyPark && tick >= ParkFlyTicks()) {
        if (!g_parkPosSet) {
          // SANDVOX_PARK_AT wins over the route's endpoint, and it is read
          // AFTER the surface-follow above rewrote pos.y: the whole point of
          // naming a place is that the probe holds THAT altitude, which over a
          // lake is not TerrainHeight's.
          if (!ParkAtPos(g_parkPos)) g_parkPos = player.pos;
          g_parkPosSet = true;
          std::printf("park: stopped at (%.1f, %.1f, %.1f) on tick %u\n",
                      g_parkPos.x, g_parkPos.y, g_parkPos.z, tick);
        }
        player.pos = g_parkPos;
        player.vel = Vec3{0, 0, 0};
      }
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
    int ticksThisFrame = 0;
    bool mouseL = captured && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    bool mouseR = captured && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    // LMB routes to the active tool: continuous for brush/laser, click-edge
    // for one-shot tools (prefab stamp, mob spawn)
    bool mouseLClick = mouseL && !prevMouseL;
    bool mouseRClick = mouseR && !prevMouseR;
    prevMouseL = mouseL;
    prevMouseR = mouseR;
    if (mouseLClick && ui.tool == UIState::kToolPrefab) ui.placePrefab = true;
    if (mouseLClick && ui.tool == UIState::kToolMob) ui.spawnMob = true;
    // THE CAST KEY is RMB, and only while magic mode is on.
    //
    // Chosen over Enter, which the brief suggested against for the right
    // reason: the left hand lives on WASD and the right hand is already on the
    // mouse for aiming, so Enter would mean leaving the number row to reach
    // across the keyboard mid-fight. A spell is AIMED, so the cast belongs on
    // the aiming hand. Magic mode is what keeps this from stealing brush-erase.
    // LATCHED, not frame-local. The cast is consumed inside the fixed-tick
    // loop below, which runs ZERO times on any frame where the accumulator has
    // not reached a whole tick — at 50 fps against a 30 Hz tick that is most
    // frames. A frame-local bool is therefore discarded unread most of the
    // time, which reads as "RMB does nothing 8 tries out of 9".
    //
    // Every other one-shot input here (prefab stamp, mob spawn, detonate) is
    // already a sticky flag consumed-and-cleared inside the loop for exactly
    // this reason; casting was the one that was not.
    if (captured && ui.magicMode && mouseRClick) castQueued = true;
    beamHeld = captured && mouseR;
    if (captured && ui.magicMode && eDel.Pressed(key(GLFW_KEY_DELETE))) dropStatusQueued = true;
    beamHeld = captured && mouseR;
    if (captured && ui.magicMode && eDel.Pressed(key(GLFW_KEY_DELETE))) dropStatusQueued = true;
    // A click made while paused is DROPPED rather than held: the tick loop
    // breaks before the cast site while paused, so a latched click would sit
    // there and discharge the instant you unpause, at whatever you happen to
    // be aiming at then.
    if (ui.paused && !ui.stepOnce) castQueued = false;
    bool laserHeld =
        captured && (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS ||
                     (ui.tool == UIState::kToolLaser && mouseL));
    bool brushActive = ui.tool == UIState::kToolBrush && !ui.magicMode;
    // MELEE: hold LMB with the melee tool to arm the weapon, then flick.
    // Magic mode wins the mouse, so guarding and casting can never both be
    // live on the same button.
    // WHAT IS IN THE HAND comes from the SHEATH, not the hotbar (see the
    // draw/stow block above). A weapon that left the sheath while it was drawn
    // — dragged into the pack from the character screen, say — is no longer
    // drawn, and this is where that is noticed: the state lives on one side of
    // the question, so there is nothing to keep in step.
    // ---- GEAR THAT LEFT THE BODY BY FORCE (Mob::LostGear) --------------------
    //
    // The rig reports; the KIT is this frame's to fix, and it has to be fixed
    // BEFORE the sheath and the wear loop below read it in the same tick, or
    // the re-equip seams would faithfully pull a second sword out of the
    // sheath while the first lies at your feet, and put the cuirass back on
    // a body the plate has just fallen off. The piece on the ground is
    // registered under its name (SetOnItemShed), so picking it back up is the
    // ordinary `E` and wearing it again restores exactly the holes it had:
    // the damage travels through `kit.wornDamage` by name, the same road a
    // piece dragged into the pack takes.
    for (const Mob::LostGear& lg : avatar.LostGearEvents()) {
      if (lg.held) {
        ItemStack& sh = kit.equip.slots[kSheathSlot];
        if (KitItemName(sh, items) == lg.item) sh = ItemStack{};
        ui.kitMessage = "your " + lg.item + " was knocked from your hand";
      } else {
        kit.SetDamage(lg.item, lg.damage);
        if (lg.equipSlot >= 0 && lg.equipSlot < kEquipSlotCount &&
            KitItemName(kit.equip.At(lg.equipSlot), items) == lg.item) {
          kit.equip.slots[lg.equipSlot] = ItemStack{};
          wearTried[lg.equipSlot].clear();
          wearDye[lg.equipSlot] = 0;
        }
        ui.kitMessage = "your " + lg.item + " was cut loose";
      }
      ui.kitMessageAge = 0.0f;
    }
    avatar.ClearLostGear();
    sheath.Reconcile(sheathKind(), UIState::kToolMelee, ui.tool);
    const ItemStack& sheathed = kit.equip.At(kSheathSlot);
    const ItemDef* heldItem =
        sheath.drawn ? items.At(sheathed.Empty() ? -1 : sheathed.def)
                     : nullptr;
    const bool meleeArmed = ui.tool == UIState::kToolMelee && !ui.magicMode &&
                            heldItem && heldItem->kind == ItemKind::Melee;
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
    const bool meleeUnarmed = [&] {
      if (ui.tool != UIState::kToolMelee || ui.magicMode) return false;
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
    const bool meleeHeld = meleeReady && mouseL;
    // DISCRETE STRIKES (melee.controlMode 0): the click is the whole input,
    // LATCHED like castQueued above and for the same reason. The direction is
    // read HERE, at the press edge, because the flick is freshest at the
    // instant of intent — a strike fired from the buffer later still cuts the
    // direction that was flicked, aimed wherever the camera is THEN (the
    // aim is resolved at the windup's end, like every stroke).
    if (mouseLClick && meleeReady && CurrentTuning().melee.controlMode == 0) {
      const StyleLibrary& styleLib = mobs.AttackStyles();
      // WHICH COMPASS: the sword's, or the fists'. Two maps rather than one
      // filtered set, because the two are different SHAPES (strokes.h
      // StyleLibrary::playerUnarmed) — a punch compass has a jab and a cross
      // where a sword's has an overhead and a thrust.
      const PlayerStrikeMap& map =
          meleeArmed ? styleLib.player : styleLib.playerUnarmed;
      float fx = 0, fy = 0;
      int si = -1;
      if (strikePicker.Pick(CurrentTuning().melee.pickMinSpeed, fx, fy))
        si = QuantizeStrike(map, fx, fy);
      if (si < 0) {
        // No flick: alternate the two horizontals so plain clicking is a
        // usable L/R rhythm rather than the same cut stamped.
        si = NeutralStrike(map, strikePicker.altRight);
        strikePicker.altRight = !strikePicker.altRight;
      }
      if (si >= 0) strikeQueued = si;
    }
    // Same pause rule as the cast: a click made while paused is dropped, not
    // banked to fire at whatever is under the crosshair on unpause.
    if (ui.paused && !ui.stepOnce) strikeQueued = -1;
    // Scroll and the number row pick a hotbar slot while the melee tool is up,
    // which is the one context where the number row is otherwise unclaimed
    // (the brush owns it normally, glyphs own it in magic mode).
    if (ui.tool == UIState::kToolMelee && !ui.magicMode) {
      for (int i = 0; i < kItemSlots; i++) {
        int k = (i == 9) ? GLFW_KEY_0 : (GLFW_KEY_1 + i);
        if (captured && eGlyph[i].Pressed(key(k))) hotbar.Select(i);
      }
    }
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
      if (ticksThisFrame > 0 && !noThrottle) {
        ctx.ProcessEvents();  // retire what the GPU finished during the tick above
        constexpr int kGpuLagThrottleTicks = 2;
        if (ctx.PendingMapCount() >= kGpuLagThrottleTicks) {
          g_ticksThrottled++;
          accumulator = std::min(accumulator, (double)kTickDt);
          break;
        }
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

      // recenter the residency window on the player (between ticks only; at
      // most one 1-chunk shift per axis)
      IVec3 playerChunkNow{ifloor(player.pos.x) >> 4, ifloor(player.pos.y) >> 4,
                           ifloor(player.pos.z) >> 4};
      // ...and the CONTINUOUS position, for the far-plume crossfade weights.
      // The chunk coord above is what the window recentres on and is therefore
      // the wrong input for a weight: it moves 16 voxels at a time. See
      // world.h kGasFarEyeSlackVox. This is the only caller — a headless
      // harness never sets it and keeps the window-centre weights it had.
      if (world.farPlumes)
        world.farPlumes->SetEye({ifloor(player.pos.x), ifloor(player.pos.y),
                                 ifloor(player.pos.z)});
      uint32_t farCount = 0;
      {
        // ---- STREAM: the row that flying lights up --------------------
        // The toroidal window shift, chunk fetch/evict and the far-field
        // cascade recentre. THIS is the cost the Performance tab was reporting
        // as "input" — it had no timer, so it fell into the residual, and the
        // residual was billed to the input row.
        sandvox::PerfSpan spanStream(sandvox::PerfScope::Stream);
        stream.Update(playerChunkNow, tick);
        // THE HORIZON ARRIVING. `far`/`fardown` compile on a background thread
        // (docs/PLAN_shader_compile.md package A) and this is the one place
        // that notices they landed. Nothing was recorded for the cascades
        // before that, so a wholesale refill is what puts terrain back past
        // the residency window. Fires exactly once.
        if (sim.PollFarPipelines()) far.FullRefill(playerChunkNow);
        // ...and until it does, FarField MUST NOT DRAIN (2026-09-10). It used
        // to: PrepareTick popped, EncodeFarFill found no pipeline and dropped
        // the entries, and pending_ emptied — so for the whole compile (2.5 s
        // warm, 20.6 s on a cold shader cache, measured) the fog reported the
        // full 6.5 km horizon and the valid box reported every level marchable
        // over cascade buffers that were still zeros. Rays escaped through the
        // ground into the sky and the clouds were drawn UNDER the terrain,
        // then the refill landed and slammed the fog onto the window. Holding
        // the queue keeps both readings honest for those seconds — closed fog,
        // empty valid box — and costs nothing else, since the entries were
        // being thrown away anyway. Update() is held with it: recentring while
        // blind would pile incoming planes onto a queue the FullRefill above
        // is about to clear.
        const bool farBlind = sim.FarFillsDeferred();
        // The refill's per-tick slice, live from tuning so F5 moves it (see
        // Tuning::Render::farRefillRate — this is the knob that turned the
        // horizon's arrival from a 16 s 2 fps stall into a fog that opens).
        far.SetBulkCap((uint32_t)CurrentTuning().render.farRefillRate);
        // And ORDINARY TRAVEL's slice, the one a moving player actually feels
        // (Tuning::Render::farPlaneFillRate). Live from tuning for the same
        // reason: what it trades against is the sieve's per-entry GPU cost,
        // which a shader edit can move by more than 2x without a rebuild.
        far.SetPlaneCap((uint32_t)CurrentTuning().render.farPlaneFillRate);
        // far-field cascades track the player the same way (render-only)
        if (!farBlind) far.Update(playerChunkNow);
        farCount = far.PrepareTick(ctx.queue, !farBlind);
        if (farCount) {
          g_farEntries += farCount;
          g_farTicks++;
          g_farBiggest = std::max<uint64_t>(g_farBiggest, farCount);
        }
      }
      // ---- GAME LOGIC: the rest of the tick body up to the submit ---------
      // Brush, laser, melee, spells, mob/avatar/debris PreTick, explosions.
      // Closed at `t0` below, where the submit sequence starts.
      sandvox::PerfSpan spanGame(sandvox::PerfScope::GameLogic);

      std::vector<BrushOp> ops;
      brush.radius = ui.brushRadius;
      brush.material = (uint32_t)ui.brushMaterial;

      // laser (PLAN §C1/C2): laser tool + LMB, or hold F from any tool.
      // Bodies are tested first — a mob limb or debris chunk in the beam
      // takes the hit instead of the wall behind it.
      struct LaserCut {
        uint64_t body = 0;
        Vec3 at{};
        float radius = 0;
        bool limb = false;  // a live mob limb carves; plain debris melts
      } laserCut;
      if (laserHeld) {
        const WorldSnapshot& lsnap = world.Snap();
        Vec3 fwd = cam.Forward();
        // MUZZLE, NOT EYE. The avatar's own head occupies the eye position, so
        // a ray cast from there hits the caster's skull on frame 1 and
        // avatar.Damage() below decapitates them instantly. Emit from in FRONT
        // of the head instead — the same reason the spell block muzzles its
        // bolt, and the same offset the beam sprites below already draw from,
        // so what cuts and what is visible are finally the same ray.
        const Vec3 muzzle = LaserMuzzle(player, cam);
        float gridDist = 1e9f;
        IVec3 hit{};
        if (lsnap.valid && lsnap.pick[0] != 0) {
          hit = {(int)lsnap.pick[2], (int)lsnap.pick[3], (int)lsnap.pick[4]};
          // PROJECTED onto fwd, not the euclidean distance from the muzzle.
          // The two distances compared below have to be the same measurement,
          // and they come from two DIFFERENT rays: bodyDist is along fwd from
          // the muzzle, while this pick cell was found by sim_pick.wgsl casting
          // from R.camPos (the EYE) — the muzzle is offset right and down of
          // that line, so a euclidean distance mixes in the ~0.86 vox lateral
          // displacement. That inflates nothing but shortens plenty: the eye
          // ray clips ground or a wall edge the muzzle ray misses, gridDist
          // comes back shorter than the mob actually is, and the body branch
          // never runs — which is "the laser stopped hurting mobs".
          gridDist =
              (Vec3{hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f} - muzzle).dot(fwd);
        }
        float frac = 1.0f;
        const float kLaserRange = CurrentTuning().tools.laserRange;
        uint64_t hitBody = phys.CastRayBody(muzzle, fwd, kLaserRange, frac);
        float bodyDist = frac * kLaserRange;
        // A weapon must not cut its wielder (main.cpp melee sweep, same rule).
        // The muzzle above clears the head, but an arm swings through the beam
        // line constantly and a severed-then-still-owned part lingers there —
        // so the reject is by OWNERSHIP, not by distance. Dropping the hit
        // entirely (rather than falling through to the grid branch) is
        // deliberate: the beam is occluded by the limb it refuses to cut.
        if (hitBody != 0 && avatar.OwnsBody(hitBody)) hitBody = 0;

        if (hitBody != 0 && bodyDist < gridDist) {
          // body cut (PLAN §C2): mob limbs take damage (instant sever when
          // the beam crosses a joint); plain debris is MELTED where the beam
          // lands. The beam bores a channel tick by tick and the body splits
          // when that channel actually severs it (DebrisSystem::MeltBodyAt) —
          // no cutting plane is chosen, so what falls apart is decided by the
          // geometry the player carved, not by camera orientation.
          Vec3 hitPos = muzzle + fwd * bodyDist;
          // The avatar is checked alongside mobs, but the OwnsBody reject
          // above means this can no longer be one of the caster's LIVE parts.
          // It still has to run: a part that was severed and whose hold has
          // expired is back on MOVING and no longer owned, so it takes the
          // beam like any other debris — which is the intent. What it must
          // never again do is take the beam off the head it was fired from.
          if (avatar.Damage(hitBody, CurrentTuning().tools.laserDamage,
                            hitPos)) {
            // handled by the avatar
          } else if (mobs.Damage(hitBody, CurrentTuning().tools.laserDamage,
                                 hitPos)) {
            // A limb hit is now BOTH: the hp/sever logic above (joint
            // crossings, flinch, loco states) AND a real channel bored through
            // the flesh. Deferred like the melt below, for the same reason.
            //
            // Damage() may have severed the limb outright, in which case this
            // handle is no longer a live limb — the carve then simply misses
            // (CarveLimbRadial returns false) rather than touching stale state.
            laserCut = {hitBody, hitPos,
                        (float)CurrentTuning().tools.laserCarveRadius, true};
          } else {
            // Deferred: the melt needs the `spawns` list that debris.PreTick
            // fills further down, and the ray must be cast HERE where the
            // camera and physics state for this tick are current. Carrying the
            // hit forward is cheaper than reordering the tick.
            float br = (float)CurrentTuning().tools.laserMeltRadius;
            laserCut = {hitBody, hitPos + fwd * (br * 0.5f), br, false};
          }
        } else if (gridDist < 1e8f) {
          const int r = CurrentTuning().tools.laserMeltRadius;
          ops.push_back({hit.x, hit.y, hit.z, r, 0, 2u /*melt*/, 0, 0});
          // cutting through a support must drop the far side: rate-limited
          // island checks over the cut (support-loss flags catch the rest)
          if (tick % 8 == 0)
            debris.AddDestructionEvent(tick, {hit.x - r, hit.y - r, hit.z - r},
                                       {hit.x + r, hit.y + r, hit.z + r});
        }
      }

      // mob spawn (mob tool LMB, or M): drop the selected def at the picked
      // surface, feet on the last empty cell
      // The lab is mob-free by design (plan §4.1): a wandering mob is exactly
      // the confounding load the lab exists to exclude. Consume the request
      // so it cannot latch across a mode where it would fire.
      if (labScene >= 0) ui.spawnMob = false;
      // ---- --duel-dummy: one target, once, three metres ahead ---------------
      // Deferred to the tick loop rather than done at load because the player's
      // position and the terrain height under it are only settled here, and a
      // dummy spawned inside a hill is not a dummy. A mob with no AI stands
      // where it is put, so nothing else is needed to keep it still.
      if (g_duelDummy && !duelDummySpawned && avatar.Spawned()) {
        duelDummySpawned = true;
        int humanDef = -1;
        for (size_t i = 0; i < mobs.Defs().size(); i++)
          if (mobs.Defs()[i].name == "human") humanDef = (int)i;
        if (humanDef >= 0) {
          const MobDef& d = mobs.Defs()[humanDef];
          const Vec3 fwd = cam.Forward();
          const float dist = MetresToCells(3.0f);
          const int sx = ifloor(player.pos.x + fwd.x * dist) - d.prefab.size.x / 2;
          const int sz = ifloor(player.pos.z + fwd.z * dist) - d.prefab.size.z / 2;
          const int sy = World::TerrainHeight(sx + d.prefab.size.x / 2,
                                              sz + d.prefab.size.z / 2,
                                              kDefaultSeed) + 1;
          const uint64_t id = mobs.Spawn(humanDef, {sx, sy, sz});
          // ARMED, so the dummy is the same body a real opponent will be:
          // a sword in its hand is a limb the player's edge can bind against
          // and, in Phase C, an arm that will swing back.
          const ItemDef* sword = items.At(items.Find("sword"));
          if (id && sword) mobs.EquipItem(id, sword);
          std::printf("--duel-dummy: human at (%d,%d,%d)%s\n", sx, sy, sz,
                      id ? "" : " FAILED");
        } else {
          std::fprintf(stderr, "--duel-dummy: no \"human\" mob def\n");
        }
      }
      if (ui.spawnMob) {
        ui.spawnMob = false;
        const WorldSnapshot& msnap = world.Snap();
        if (msnap.valid && msnap.pick[0] != 0 && !mobs.Defs().empty()) {
          if (ui.mobSelected >= (int)mobs.Defs().size()) ui.mobSelected = 0;
          const MobDef& d = mobs.Defs()[ui.mobSelected];
          mobs.Spawn(ui.mobSelected,
                     {(int)msnap.pick[5] - d.prefab.size.x / 2,
                      (int)msnap.pick[6],
                      (int)msnap.pick[7] - d.prefab.size.z / 2});
        }
      }

      // ---- NPC AI panel: spawn / despawn / retarget (game/ai_behavior.h) ---
      //
      // Producers on the SAME path gameplay uses: MobSystem::Spawn, then
      // EquipItem, then SetMobBehavior. There is no dev-only entry into the AI,
      // which is why the panel's duelist and a duelist placed by content are
      // the same creature.
      if (labScene >= 0) {
        ui.aiSpawnDummy = ui.aiSpawnStatic = ui.aiSpawnDuelist = false;
        ui.aiSpawnOwn = false;
        ui.aiKillSpawned = false;
      }
      if (ui.aiSpawnDummy || ui.aiSpawnStatic || ui.aiSpawnDuelist ||
          ui.aiSpawnOwn) {
        // EMPTY MEANS "THE CREATURE'S OWN", resolved once the def is known --
        // see UIState::aiSpawnOwn for what the override was costing.
        const char* profile = ui.aiSpawnOwn         ? ""
                              : ui.aiSpawnDummy     ? "dummy"
                              : ui.aiSpawnStatic    ? "swordsman_static"
                                                    : "duelist";
        ui.aiSpawnDummy = ui.aiSpawnStatic = ui.aiSpawnDuelist = false;
        ui.aiSpawnOwn = false;
        // A humanoid that can actually HOLD the sword: picked by capability
        // (a `held_right` socket) rather than by name, so renaming an asset
        // cannot silently spawn an unarmed creature.
        // WHAT THE PANEL PICKED, resolved BY NAME at spawn time for the reason
        // the weapon pick below is: a def index is directory order, and the
        // list can be rebuilt by an R reload between the click and here.
        const std::string& creature =
            ui.aiCreatureNames.empty()
                ? avatarDefName
                : ui.aiCreatureNames[ui.aiCreaturePick <
                                             (int)ui.aiCreatureNames.size()
                                         ? ui.aiCreaturePick
                                         : 0];
        int aiDef = -1;
        for (size_t i = 0; i < mobs.Defs().size(); i++) {
          if (mobs.Defs()[i].FindSocket("held_right") < 0) continue;
          // Fall back to the old rule — first eligible def, preferring the
          // avatar's own species — if the pick names a def that has gone away
          // under an R reload. Never spawn nothing because a name went stale.
          if (aiDef < 0 || mobs.Defs()[i].name == avatarDefName) aiDef = (int)i;
          if (mobs.Defs()[i].name == creature) { aiDef = (int)i; break; }
        }
        if (aiDef >= 0) {
          // Crosshair hit when there is one, otherwise a few metres ahead on
          // the ground: spawning behind you is useless for watching a duelist.
          const WorldSnapshot& asnap = world.Snap();
          IVec3 at{};
          const MobDef& d = mobs.Defs()[aiDef];
          if (asnap.valid && asnap.pick[0] != 0) {
            at = IVec3{(int)asnap.pick[5] - d.prefab.size.x / 2,
                       (int)asnap.pick[6],
                       (int)asnap.pick[7] - d.prefab.size.z / 2};
          } else {
            const Vec3 fwd = cam.Forward();
            const int sx = ifloor(player.pos.x + fwd.x * 40.0f);
            const int sz = ifloor(player.pos.z + fwd.z * 40.0f);
            at = IVec3{sx, World::TerrainHeight(sx, sz, kDefaultSeed) + 1, sz};
          }
          const uint64_t nid = mobs.Spawn(aiDef, at);
          if (nid != 0) {
            // WHAT THE PANEL PICKED, resolved by name at spawn time. Entry 0
            // is "fists" and Find() returns -1 for it, so the empty hand needs
            // no special case — At(-1) is nullptr and the mob spawns with
            // nothing in its fist, which is no longer a creature that cannot
            // fight: its `natural` weapons and its profile's fallback punches
            // are what it swings (docs/PLAN_impact_unarmed.md §3/§5).
            const std::string& pick =
                ui.aiWeaponNames[ui.aiWeaponPick < (int)ui.aiWeaponNames.size()
                                     ? ui.aiWeaponPick
                                     : 0];
            const ItemDef* weapon = items.At(items.Find(pick));
            if (weapon != nullptr) mobs.EquipItem(nid, weapon);
            // WHAT IT WEARS (UIState::aiOutfit). Through MobSystem::WearItem,
            // the path the player's own equip slots take, so a spawn dressed
            // here is a creature in armour and not a dev-only object: its
            // plate dents, its cloth burns, and its corpse can be looted.
            //
            // Slot by slot in EquipSlotId order, one piece per slot. The
            // random outfit and the dye are hashed off the tick and the mob id
            // rather than rand(), so a replay dresses the same crowd.
            {
              int dressed = 0;
              for (int s = 0; s < (int)ui.aiWearNames.size(); s++) {
                if (!EquipSlotIsWorn(s)) break;
                const ItemDef* piece = nullptr;
                const uint32_t roll = rng::Hash3(tick, (uint32_t)nid, (uint32_t)s);
                if (ui.aiOutfit == 1) {
                  // RANDOM CLOTHES: a dyeable piece of this slot's kind. The
                  // commoner set is chest / legs / boots, so a helmet slot has
                  // no candidates and stays bare, which is the point.
                  std::vector<const ItemDef*> cands;
                  for (const ItemDef& it : items.items)
                    if (it.dyeable && EquipSlotAccepts(s, it.kind))
                      cands.push_back(&it);
                  if (!cands.empty()) piece = cands[roll % cands.size()];
                } else if (ui.aiOutfit == 2) {
                  // FULL PLATE: the slot's `iron_*` piece. By name prefix
                  // rather than by material because the stock set is authored
                  // that way (scripts/gen_stock_armor.py) and nothing on an
                  // ItemDef says "this is armour rather than a shirt".
                  for (const ItemDef& it : items.items)
                    if (EquipSlotAccepts(s, it.kind) &&
                        it.name.rfind("iron_", 0) == 0) {
                      piece = &it;
                      break;
                    }
                } else if (ui.aiOutfit == 3) {
                  const int wp = s < (int)ui.aiWearPick.size() ? ui.aiWearPick[s]
                                                                : 0;
                  if (wp > 0 && wp < (int)ui.aiWearNames[s].size())
                    piece = items.At(items.Find(ui.aiWearNames[s][wp]));
                }
                if (piece == nullptr) continue;
                // A dyeable piece gets a colour, the same "full saturation at a
                // middling value" the wardrobe's random button uses: a random
                // point in the RGB cube is mostly mud.
                uint32_t dye = 0;
                if (piece->dyeable) {
                  float rgb[3];
                  DyeFromHsv((float)(roll % 3600u) / 3600.0f,
                             0.45f + (float)((roll >> 12) % 100u) / 100.0f * 0.5f,
                             0.35f + (float)((roll >> 20) % 100u) / 100.0f * 0.5f,
                             rgb);
                  dye = DyePack(rgb[0], rgb[1], rgb[2]);
                }
                if (mobs.WearItem(nid, piece, s, dye)) dressed++;
                else
                  std::printf("AI panel: \"%s\" would not go on %s\n",
                              piece->name.c_str(), d.name.c_str());
              }
              if (ui.aiOutfit != 0)
                std::printf("AI panel: %s spawned wearing %d piece(s)\n",
                            d.name.c_str(), dressed);
            }
            // THE SIDECAR'S OWN PROFILE when the button asked for it, and the
            // same `empty() ? "duelist" : behavior` fallback --shot-strike
            // uses, so a def that names none still gets something that fights.
            const std::string prof =
                *profile != '\0' ? std::string(profile)
                : d.behavior.empty() ? std::string("duelist")
                                     : d.behavior;
            if (!mobs.SetMobBehavior(nid, prof)) {
              // A NAMED PROFILE THAT IS NOT LOADED leaves the creature on the
              // legacy wander, where it walks off and never fights — and it
              // did it silently. The panel is a dev tool; say so.
              std::printf(
                  "AI panel: \"%s\" asked for behaviour \"%s\", which is not in"
                  " behaviors.json — it will not fight\n",
                  d.name.c_str(), prof.c_str());
            }
            aiSpawnedMobs.push_back(nid);
          }
        }
      }
      if (ui.aiKillSpawned) {
        ui.aiKillSpawned = false;
        // Kill rather than delete: a corpse ragdolls, its limbs become debris,
        // and the whole teardown path gets exercised. Silently dropping the
        // rig would be a second despawn implementation.
        for (uint64_t mid : aiSpawnedMobs)
          if (Mob* m = mobs.FindMobById(mid)) m->Die();
        aiSpawnedMobs.clear();
      }
      if (ui.aiRagdollSpawned) {
        ui.aiRagdollSpawned = false;
        for (uint64_t mid : aiSpawnedMobs)
          mobs.RagdollMob(mid, CurrentTuning().ragdoll.devSeconds);
      }
      if (ui.ragdollMe) {
        ui.ragdollMe = false;
        if (avatar.Spawned() && avatar.IsAlive()) {
          avatar.StartRagdoll(CurrentTuning().ragdoll.devSeconds, "dev button");
          // A nudge backwards and up so the body keels over instead of
          // folding straight down onto its own feet.
          const Vec3 back{-std::sin(avatarHeading), 0.35f,
                          -std::cos(avatarHeading)};
          avatar.SetLimbVelocities(back.normalized() * MetresToCells(1.5f));
        }
      }
      // ---- WARDROBE panel: make a set of clothes in a colour --------------
      //
      // Producers on the SAME paths the game uses: PlayerKit's own containers
      // and the ordinary equip slots, so a tunic this button made is a tunic,
      // not a dev-only object. The only thing the panel adds to an item that
      // picking one off the ground would not is the dye word.
      if (ui.wardrobeRandomColor) {
        ui.wardrobeRandomColor = false;
        // Hashed off the tick so it is reproducible from a replay and does not
        // reach for rand(). Full saturation at a middling value is where
        // clothes live: a random point in the RGB cube is mostly mud.
        const uint32_t h = rng::Hash3(tick, 0xD1E5u, 0x9E37u);
        const float hue = (float)(h % 3600u) / 3600.0f;
        const float sat = 0.45f + (float)((h >> 12) % 100u) / 100.0f * 0.5f;
        const float val = 0.35f + (float)((h >> 20) % 100u) / 100.0f * 0.5f;
        DyeFromHsv(hue, sat, val, ui.wardrobeColor);
      }
      if (ui.wardrobeSpawnSet || ui.wardrobeWearSet || ui.wardrobeDyeWorn) {
        const bool wear = ui.wardrobeWearSet;
        const bool redye = ui.wardrobeDyeWorn;
        ui.wardrobeSpawnSet = ui.wardrobeWearSet = ui.wardrobeDyeWorn = false;
        const uint32_t dye = DyePack(ui.wardrobeColor[0], ui.wardrobeColor[1],
                                     ui.wardrobeColor[2]);
        const std::string colour = DyeName(dye);
        if (redye) {
          // RE-DYE WHAT IS ON. Only the dyeable pieces: the wizard's robe is
          // painted in real colours and multiplying them by another colour is
          // not a feature (ItemDef::dyeable). The wear sync a few hundred lines
          // down notices the changed dye and rebuilds those shells, which is
          // why nothing here touches the rig.
          int n = 0;
          for (int s = 0; s < kEquipSlotCount; s++) {
            ItemStack& st = kit.equip.slots[s];
            const ItemDef* d = items.At(st.Empty() ? -1 : st.def);
            if (d == nullptr || !d->dyeable) continue;
            st.dye = dye;
            n++;
          }
          ui.wardrobeStatus = n ? ("re-dyed " + std::to_string(n) +
                                   " worn piece(s) " + colour)
                                : "nothing you are wearing takes a dye";
        } else {
          // THE PICKERS NAME THE PIECES; entry 0 of each is "(none)" and
          // Find() returns -1 for it, so an outfit of trousers and nothing
          // else needs no special case.
          auto picked = [&](const std::vector<std::string>& names, int pick) {
            return (pick > 0 && pick < (int)names.size()) ? names[pick]
                                                          : std::string();
          };
          const std::string want[3] = {
              picked(ui.wardrobeShirts, ui.wardrobeShirtPick),
              picked(ui.wardrobeLegs, ui.wardrobeLegsPick),
              picked(ui.wardrobeFeet, ui.wardrobeFeetPick)};
          int made = 0, refused = 0;
          std::string names;
          for (const std::string& w : want) {
            if (w.empty()) continue;
            const int di = items.Find(w);
            const ItemDef* d = items.At(di);
            if (d == nullptr) continue;
            // WEAR puts it in the equip slot its kind belongs to (swapping
            // out whatever was there, which lands back in the pack); the
            // plain spawn goes to the hotbar, falling back to the bag exactly
            // as a corpse's gear does (game/corpses.h).
            bool ok = false;
            if (wear) {
              const int slot = EquipSlotFor(d->kind, kit.equip);
              if (slot >= 0) {
                const ItemStack was = kit.equip.At(slot);
                if (!was.Empty()) kit.bag.Add(was.def, was.count, was.dye);
                kit.equip.slots[slot] = ItemStack{di, 1, dye};
                ok = true;
              }
            } else {
              ok = hotbar.Add(di, 1, dye) >= 0 ||
                   kit.bag.Add(di, 1, dye) >= 0;
            }
            if (ok) {
              if (!names.empty()) names += ", ";
              names += w;
              made++;
            } else {
              refused++;
            }
          }
          if (made == 0) {
            ui.wardrobeStatus = refused ? "no room for any of it"
                                        : "pick something first";
          } else {
            ui.wardrobeStatus = colour + " " + names +
                                (wear ? " — worn" : " — in your pack");
            if (refused)
              ui.wardrobeStatus += "  (" + std::to_string(refused) +
                                   " refused: no room)";
          }
        }
        ui.kitMessage = ui.wardrobeStatus;
        ui.kitMessageAge = 0.0f;
      }

      if (ui.aiApplyBehavior) {
        ui.aiApplyBehavior = false;
        if (ui.aiMobSelected >= 0 &&
            ui.aiMobSelected < (int)ui.aiMobIds.size() &&
            ui.aiBehaviorPick >= 0 &&
            ui.aiBehaviorPick < (int)ui.aiProfileNames.size())
          mobs.SetMobBehavior(ui.aiMobIds[ui.aiMobSelected],
                              ui.aiProfileNames[ui.aiBehaviorPick]);
      }

      // rolling sphere (K): a rigidbody ball, half the player's height in
      // diameter, made of the current brush material. The collider is a true
      // Jolt sphere (CreateSphereBody) so it rolls smoothly; rendering is a
      // scale-2 MICROVOXEL ball (PLAN §C) — twice the voxels across the same
      // radius, so the silhouette carries real curvature. Models are packed
      // lazily into the shared micro pool, one per material (micro voxels
      // bake material ids), cached, and the pool re-uploaded on first use.
      // Mass comes from the material's density — which is also what decides
      // how far the player can shove it.
      if (ui.spawnSphere) {
        ui.spawnSphere = false;
        const WorldSnapshot& ssnap = world.Snap();
        uint32_t sphereMat = (uint32_t)ui.brushMaterial;
        if (ssnap.valid && ssnap.pick[0] != 0 && sphereMat < mats.size()) {
          const float r = Player::kHalfY * 0.5f;  // vox: diameter = height/2
          const uint32_t kSphereScale = 2;        // micro voxels per world voxel
          const float rm = r * (float)kSphereScale;  // radius, micro voxels
          const int dims = (int)std::ceil(2.0f * rm);
          // brick coords run [0..dims); the ball centre sits mid-brick
          auto inBall = [&](int x, int y, int z) {
            float dx = x + 0.5f - dims * 0.5f, dy = y + 0.5f - dims * 0.5f,
                  dz = z + 0.5f - dims * 0.5f;
            return dx * dx + dy * dy + dz * dz <= rm * rm;
          };
          auto mit = sphereModels.find(sphereMat);
          if (mit == sphereModels.end() && sphereMat <= 255) {
            std::vector<PrefabVoxel> mv;
            for (int z = 0; z < dims; z++)
              for (int y = 0; y < dims; y++)
                for (int x = 0; x < dims; x++)
                  if (inBall(x, y, z))
                    mv.push_back({(int16_t)x, (int16_t)y, (int16_t)z,
                                  (uint16_t)sphereMat});
            std::string slog;
            int mi = MicroBodyPack(mbSet, mv, {dims, dims, dims}, kSphereScale,
                                   "sphere:" + mats[sphereMat].name, slog);
            if (!slog.empty()) std::fprintf(stderr, "%s", slog.c_str());
            MicroBodyRef packed{};
            if (mi >= 0) {
              packed.model = (uint32_t)mi;
              packed.skinScale = kSphereScale;
              sim.UploadMicroBodies(ctx.queue, mbSet);
            }
            // a failed pack caches as invalid: fall back to the cube path
            // below rather than re-attempting (and re-logging) every K press
            mit = sphereModels.emplace(sphereMat, packed).first;
          }
          MicroBodyRef ref =
              mit != sphereModels.end() ? mit->second : MicroBodyRef{};

          // sphere centre drops just above the picked surface cell
          Vec3 center{(float)ssnap.pick[5] + 0.5f,
                      (float)ssnap.pick[6] + r + 1.5f,
                      (float)ssnap.pick[7] + 0.5f};
          std::vector<DebrisVoxel> ball;
          BodyTransform sxf{};
          sxf.quat[3] = 1;
          uint64_t sh = 0;
          if (ref.Valid()) {
            // micro body: min-corner origin shared by the brick march and the
            // collider (sphere shape offset to the brick centre). Body voxels
            // are in MICRO units, which is what settle-back's downsample and
            // AdoptBody's radius calculation expect.
            for (int z = 0; z < dims; z++)
              for (int y = 0; y < dims; y++)
                for (int x = 0; x < dims; x++)
                  if (inBall(x, y, z))
                    ball.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0,
                                    (uint16_t)sphereMat});
            sxf.pos = center - Vec3{r, r, r};
            sh = phys.CreateSphereBody(center, r,
                                       (float)mats[sphereMat].gpu.density,
                                       Vec3{r, r, r});
          } else {
            // cube-path fallback (material id > 255 or pack failure):
            // world-unit ball centered on the body origin, as before
            int ext = (int)std::ceil(r);
            for (int z = -ext; z < ext; z++)
              for (int y = -ext; y < ext; y++)
                for (int x = -ext; x < ext; x++) {
                  float dx = x + 0.5f, dy = y + 0.5f, dz = z + 0.5f;
                  if (dx * dx + dy * dy + dz * dz <= r * r)
                    ball.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0,
                                    (uint16_t)sphereMat});
                }
            sxf.pos = center;
            sh = phys.CreateSphereBody(center, r,
                                       (float)mats[sphereMat].gpu.density);
          }
          if (sh) debris.AdoptBody(sh, std::move(ball), sxf, ref);
        }
      }

      // ---- MLS-MPM fluid pour (docs/PLAN_mpm_fluids.md prototype) ----------
      // Hold LMB with the mpm tool: a small sphere of cells above the brush
      // target gains 8 particles each (the rest density), per tick, budget
      // permitting. Spawn data is part of the tick's input stream — positions
      // are jittered by a hash of (tick, index), never by frame state, so a
      // replayed op stream reproduces the pour exactly.
      std::vector<FluidSpawnOp> fluidSpawns;
      if (ui.clearFluid) {
        ui.clearFluid = false;
        // The count is GPU-owned now: zero the live word directly. Deferred
        // queue writes drain at the head of the NEXT command buffer — this
        // tick's — so the seam's compaction reads 0 and every particle is
        // gone before the substeps run (the deferred-WriteBuffer ordering
        // gotcha, used in the right direction for once).
        uint32_t zero = 0;
        ctx.queue.WriteBuffer(world.fluidArgsStage, 7 * 4, &zero, 4);
        fluidCount = 0;
        fluidPendingSpawns.clear();
      }
      if (ui.tool == UIState::kToolFluid && !ui.magicMode && mouseL) {
        const WorldSnapshot& fsnap = world.Snap();
        IVec3 at;
        if (fsnap.valid && fsnap.pick[0] != 0) {
          at = {(int)fsnap.pick[5], (int)fsnap.pick[6] + 2, (int)fsnap.pick[7]};
        } else {
          Vec3 p = player.EyePos() + cam.Forward() * 24.0f;
          at = {ifloor(p.x), ifloor(p.y), ifloor(p.z)};
        }
        const int rr = std::min(std::max(ui.brushRadius / 2, 1), 3);
        const uint32_t fluidSpecies = (uint32_t)ui.fluidSpecies & 3u;
        fluidSpeciesMat[fluidSpecies] = fluidCueMat;
        for (int z = -rr; z <= rr && fluidSpawns.size() < kMaxFluidSpawnsPerTick; z++)
          for (int y = -rr; y <= rr; y++)
            for (int x = -rr; x <= rr; x++) {
              if (x * x + y * y + z * z > rr * rr) continue;
              // Budget charged BEFORE emitting (rule 2): a cell that does not
              // fit its 8 particles is refused whole.
              if (fluidCount + fluidSpawns.size() + 8 > kFluidCap) break;
              if (fluidSpawns.size() + 8 > kMaxFluidSpawnsPerTick) break;
              for (int s = 0; s < 8; s++) {
                // 8 per cell on the half-cell lattice (rest density), with a
                // deterministic sub-lattice jitter so columns don't stack into
                // visible strings.
                uint32_t h = (tick * 9781u + (uint32_t)fluidSpawns.size() * 6271u) *
                                 747796405u + 2891336453u;
                FluidSpawnOp op{};
                op.px = ((at.x + x) << 16) + ((s & 1) ? 49152 : 16384) +
                        (int32_t)(h % 8192u) - 4096;
                op.py = ((at.y + y) << 16) + ((s & 2) ? 49152 : 16384) +
                        (int32_t)((h >> 13) % 8192u) - 4096;
                op.pz = ((at.z + z) << 16) + ((s & 4) ? 49152 : 16384) +
                        (int32_t)((h >> 19) % 8192u) - 4096;
                op.vx = 0; op.vy = -19661; op.vz = 0;  // gentle -0.3 cells/tick
                op.species = fluidSpecies;
                op.mat = fluidCueMat;
                fluidSpawns.push_back(op);
              }
            }
      }

      BrushOp op;
      if (brushActive && mouseL &&
          brush.BuildOp(world.Snap(), player.EyePos(), cam.Forward(), false, op))
        ops.push_back(op);
      if (brushActive && mouseR &&
          brush.BuildOp(world.Snap(), player.EyePos(), cam.Forward(), true, op)) {
        ops.push_back(op);
        // erasing can cut supports: queue an island check around the hole
        debris.AddDestructionEvent(tick, {op.x - op.radius, op.y - op.radius, op.z - op.radius},
                                   {op.x + op.radius, op.y + op.radius, op.z + op.radius});
      }

      // prefab placement: stamp at the last-empty pick cell, anchored at the
      // rotated footprint's bottom center
      if (ui.placePrefab) {
        ui.placePrefab = false;
        const WorldSnapshot& snap = world.Snap();
        if (snap.valid && snap.pick[0] != 0 && !prefabs.empty() &&
            ui.prefabSelected < (int)prefabs.size()) {
          const Prefab& pf = prefabs[ui.prefabSelected];
          IVec3 rs = PrefabPlacer::RotatedSize(pf, ui.prefabRot);
          IVec3 at{(int)snap.pick[5] - rs.x / 2, (int)snap.pick[6],
                   (int)snap.pick[7] - rs.z / 2};
          IVec3 blo, bhi;
          placer.Place(pf, at, ui.prefabRot, ui.prefabOverwrite, mats, blo, bhi);
          stream.MarkModifiedBox(blo, bhi);
        }
      }

      // Declared before mobs.PreTick so bleed spray and dismemberment gore
      // share the one per-tick spawn stream with debris shatter — the ring and
      // its 4096-op budget are global, so a single list is what keeps the two
      // systems honest about the shared limit.
      std::vector<ParticleSpawn> spawns;
      // Declared here rather than beside debris.PreTick because per-voxel limb
      // burning emits REAL fire voxels into the grid, and mobs run first.
      std::vector<CellOp> cellOps;

      // The day phase both body-burn passes gate their reactions on, taken
      // from the ONE function that also puts it on TickParams — see
      // sim/reactcpu.h for why the CPU has to agree with the GPU here.
      mobs.SetDayPhase(DayPhaseNow(tick));
      debris.SetDayPhase(DayPhaseNow(tick));

      // WHO THE NPCs ARE FIGHTING, pushed once per TICK rather than per frame.
      // The tick loop runs 0..4 times per frame, and a target position sampled
      // on the frame clock would make an NPC's decisions a function of frame
      // rate — the same reason the burn pass and the gait live in here.
      // Deliberately the capsule, not the avatar rig: the capsule is what the
      // player actually occupies, and it exists even before the avatar spawns.
      mobs.SetPlayerActor(player.pos, Player::kHalfXZ, Player::kHalfY * 2.0f,
                          ui.playerAlive);

      // mobs: kinematic walk drive, terrain anchors for ManageTerrain,
      // bleeding ops, per-voxel burning — must run before debris.PreTick
      // consumes the anchors
      mobs.PreTick(tick, world, ops, cellOps, spawns);

      // ---- player avatar ----
      // Same slot in the tick order as mobs, and for the same reason: it
      // drives kinematic bodies and appends bleeding ops that debris.PreTick
      // must see. The body's FACING is decided here rather than inside the
      // avatar because it is a game-design policy, not a rig property: in
      // first person the body always faces the camera (you are looking down
      // its own axis), while in third person it turns toward its MOTION and
      // only snaps to the camera when standing still — which is what stops
      // the character from moon-walking sideways across the screen.
      {
        const auto& av = CurrentTuning().avatar;
        // Camera yaw and rig heading use different conventions: Camera's
        // forward is (cos yaw, ., sin yaw) while a mob's is (sin h, ., cos h),
        // so h = pi/2 - yaw. Getting this wrong makes the avatar face 90
        // degrees off its travel direction.
        const float camHeading = 1.5707963f - cam.yaw;
        // WHERE THE BODY FACES is a policy, not a rig property, and it now
        // lives in ResolveAvatarHeading (game/thirdperson.h) rather than
        // inline here. It was moved because both bugs it has had — an inverted
        // glance, and a dead zone with no restoring term that froze the arms
        // off-view — were invisible to every gate, for the simple reason that
        // the policy only ever ran inside this render loop. It is pure, so the
        // selftest can drive it directly.
        avatarHeading = ResolveAvatarHeading(
            camMode, camHeading, avatarHeading,
            Vec3{player.vel.x, 0, player.vel.z}, kTickDt);

        // FLY MODE HAS NO BODY. Two things go wrong otherwise, and the second
        // one is what makes flying feel possessed:
        //   1. The rig walks/IKs against ground it is nowhere near, so the
        //      avatar flails or stretches toward the terrain below.
        //   2. Far worse — the 16 limb bodies are real Jolt bodies, and
        //      PlayerPushOut resolves the player against everything it
        //      overlaps. Flying leaves the player sitting inside their OWN
        //      limbs, so the body shoves its own player around the sky. Fly
        //      mode already ignores voxel collision for exactly this reason;
        //      the avatar has to follow the same rule.
        const bool wantAvatar = av.enabled && !player.fly;
        if (wantAvatar && avatar.HasDef() && !avatar.Spawned()) {
          avatar.Spawn(player, avatarHeading);
          // A new rig wears nothing (Mob::BuildRig clears its shells), so the
          // armour sync below has to be offered every slot again.
          for (std::string& w : wearTried) w.clear();
          for (uint32_t& d : wearDye) d = 0;
          tpRig.Snap();   // re-entering from fly: don't ease across the gap
        }
        if (!wantAvatar && avatar.Spawned()) avatar.Despawn();
        // ---- melee: drive the swing, then pose the arm (game/melee.h) -------
        // BEFORE PreTick, because PreTick is what flattens the pose and
        // submits the kinematic limb targets — a weapon pose pushed in after
        // it would be a frame late and the blade would trail the mouse.
        {
          // THE BASIS THE STROKE IS EXPRESSED IN is the camera's, yawed back
          // toward the body by the neck's own law (melee.aimYaw /
          // aimReleaseYaw; game/thirdperson.h ResolveSwingBasis). Without
          // this, a third-person camera orbited behind the character made
          // every strike cut backwards at the lens. Resolved once per tick
          // off the heading the policy above just produced, so the swing and
          // the head agree about where "behind" is. No body (fly mode): the
          // raw camera, as before.
          Vec3 swRight = cam.Right(), swUp = cam.Up(), swFwd = cam.Forward();
          if (avatar.Spawned())
            ResolveSwingBasis(camHeading, avatarHeading, cam.Right(), cam.Up(),
                              cam.Forward(), swRight, swUp, swFwd);
          if (avatar.Spawned()) {
            // Equip/unequip only on a CHANGE. EquipItem builds a body and a
            // joint, so calling it every tick with the same weapon would
            // rebuild the sword 60 times a second; comparing against what is
            // already in the hand keeps this a no-op in the common case.
            //
            // BEFORE melee.Update, not after: the arm read below needs the item
            // in the hand to know WHICH arm is the weapon arm, and the tick the
            // player selects the blade and clicks can be the same tick.
            const ItemDef* want = meleeArmed ? heldItem : nullptr;
            const std::string wantName = want ? want->name : std::string();
            if (avatar.HeldItem() != wantName) avatar.EquipItem(want);
            // WHERE THE BLADE IS, so taking control of it is not a teleport:
            // the stroke seeds itself from the live point AND the live hand,
            // takes its blade length from the pair, and bounds itself by the
            // rig's own reach (game/melee.h SetStroke).
            Vec3 handNow, tipNow, flatNow;
            float reachNow = 0;
            if (avatar.WeaponStrokePose(handNow, tipNow, flatNow, reachNow))
              melee.SetStroke(handNow, tipNow, flatNow, reachNow);
            else
              melee.ClearArm();
            // ...and where the avatar's own head is, so neither an authored
            // windup nor a freeform drag sweeps the blade through it
            // (melee.h SetKeepOut; the clamp lives in RebuildFrame).
            {
              Vec3 kc;
              float kr = 0;
              if (avatar.HeadKeepOut(kc, kr))
                melee.SetKeepOut(kc, kr);
              else
                melee.ClearKeepOut();
            }
            // The NPC driver always did this and the player's never had —
            // without it the asymmetric azimuth window (azOut on the weapon
            // side) assumes a right-handed rig whatever the avatar holds.
            melee.SetHandSign(avatar.HandSign());
          } else {
            melee.ClearArm();
          }
          // ---- WHICH MODE FEEDS THE DRIVER (melee.controlMode; tuning.h) ---
          // Read HERE and nowhere else. Downstream, the pose push, Arrest and
          // Nudge are mode-blind; the sweep and the whoosh key on
          // `melee.Cutting() || playerStrike.Cutting()`, which needs no mode
          // read because a freeform tick keeps the cursor Idle.
          const int meleeMode = CurrentTuning().melee.controlMode;
          // The program entered its cut THIS tick (the discrete whoosh edge —
          // see the whoosh block below for why Slash cannot be the trigger).
          bool strikeCutEdge = false;
          if (meleeMode == 1) {
            // FREEFORM: the original law, untouched. A mode flip mid-swing
            // (F5) drops any live program rather than leaving it half-run.
            playerStrike.Reset();
            strikeQueued = strikeBuffered = -1;
            melee.Update(kTickDt, meleeHeld, meleeReady, swRight, swUp, swFwd);
          } else {
            // DISCRETE: consume the press latch, then step the program. Begin
            // and first step land on the SAME tick, exactly as the NPC's
            // BeginStroke/StepStroke pair does.
            if (!meleeReady || !avatar.Spawned()) {
              // Weapon stowed (or body gone) mid-swing: drop the claim the
              // same way MobSystem's teardown guard does.
              playerStrike.Reset();
              strikeBuffered = -1;
              if (avatar.Spawned()) avatar.ClearStrikeEffector();
            }
            if (strikeQueued >= 0 && meleeReady && avatar.Spawned()) {
              if (!playerStrike.Active()) {
                if (const AttackStyle* sty =
                        mobs.AttackStyles().At(strikeQueued)) {
                  // ---- POINT THE DRIVER AT THE PART THIS STYLE SWINGS -----
                  // Exactly what MobSystem::BeginStroke does, and through the
                  // same call: `player_punch_r` names `fist.R`, so the arm
                  // chain is claimed on the fist and `avatar.WeaponEdge`
                  // reports the knuckles. A style whose weapon this body has
                  // not got refuses HERE, before the program begins, so a
                  // strike never runs with no edge on the end of it.
                  if (avatar.ArmForStyle(*sty)) {
                    // Fresh sliders per swing — MobSystem::BeginStroke says
                    // why (MeleeTuning is a copy; F5 must reach the next cut).
                    ApplyMeleeTuning(melee.tuning);
                    // The seed is (who, when), like the NPC's; the player's
                    // styles author jitter 0, so it only matters if an author
                    // turns jitter back on — and then it still replays.
                    BeginStrokeProgram(playerStrike, *sty, strikeQueued,
                                       rng::Hash3(0x504Cu, tick, 0x5747u));
                    // ...and the style's body animation, exactly as the NPC's
                    // BeginStroke does (strokes.h AttackStyle::clip).
                    if (!sty->clip.empty()) avatar.PlayClip(sty->clip);
                  }
                }
              } else if (playerStrike.phase == StrokeCursor::Phase::Cut ||
                         playerStrike.phase == StrokeCursor::Phase::Recover) {
                // ONE strike banks mid-swing (last click wins); a click during
                // the windup is dropped — the windup IS the commitment.
                strikeBuffered = strikeQueued;
              }
              strikeQueued = -1;
            }
            bool stepped = false;
            if (playerStrike.Active()) {
              const AttackStyle* sty = mobs.AttackStyles().At(playerStrike.style);
              if (sty == nullptr) {
                // The style library reloaded out from under a live swing.
                playerStrike.Reset();
              } else {
                const bool wasCutting = playerStrike.Cutting();
                // ---- THE AIM: WHERE THE CROSSHAIR IS, NOT WHERE IT POINTS --
                //
                // This used to pass (0, 0, 0) — "the camera IS the aim" — and
                // that is true of a DIRECTION and false of a BLOW. The stroke
                // is a bearing about the arm's own pivot (strokes.h
                // StrokeAimAt), and the shoulder sits a couple of voxels under
                // the eye and a couple to the side of it; copying the camera's
                // bearing therefore lands the fist a whole shoulder offset low
                // and wide of whatever is under the crosshair. At sword range
                // that is a few degrees. AT PUNCHING RANGE IT IS THE
                // DIFFERENCE BETWEEN A HEAD AND A COLLARBONE, which is the
                // "my punches don't go where I'm pointing" report.
                //
                // So resolve a POINT and take the bearing to it. Nearest of:
                //   * the first dynamic BODY down the crosshair line (a mob's
                //     head — the case the whole thing is for), the rig's own
                //     limbs excluded for the reason the E ray excludes them
                //     (the eye sits inside your own head collider and Jolt
                //     reports a shape the ray starts in as a hit at t=0);
                //   * the first solid VOXEL, marched on the CPU mirror. Not
                //     `snap.pick`, which the brush reads: that ray starts at
                //     the RENDER camera, and a punch may not move because the
                //     player pushed the third-person boom out.
                //   * failing both, a point far down the line, which reproduces
                //     the old camera-parallel aim to within a few degrees —
                //     so a strike at open air is unchanged and the two cases
                //     meet continuously instead of snapping.
                //
                // From `player.EyePos()` along `cam.Forward()`, the same pair
                // the brush, the laser and the grenade use, so the camera
                // cannot change where a strike lands.
                float aimAz = 0, aimEl = 0, aimDist = 0;
                {
                  Vec3 pivot;
                  if (avatar.StrokePivotWorld(pivot)) {
                    // Far enough that the fallback is effectively the camera
                    // line, short enough to stay inside the CPU mirror's
                    // 3x3x3 window for the voxel half (world.h KindAt).
                    constexpr float kAimRange = 40.0f;
                    const Vec3 eye = player.EyePos();
                    const Vec3 look = cam.Forward();
                    float hitDist = kAimRange;
                    strikeIgnore.clear();
                    avatar.AppendLiveLimbBodies(strikeIgnore);
                    float frac = 1.0f;
                    if (phys.CastRayBody(eye, look, kAimRange, frac,
                                         strikeIgnore))
                      hitDist = std::min(hitDist, frac * kAimRange);
                    // The voxel half, quarter-voxel steps from a half-voxel
                    // out (step 0 is the cell the eye is already in). Unknown
                    // is NOT a hit — the projectile convention, and the mirror
                    // answers Unknown past ~48 voxels.
                    for (float d = 0.5f; d < hitDist; d += 0.25f) {
                      const Vec3 p = eye + look * d;
                      if (kindAt(IVec3{ifloor(p.x), ifloor(p.y),
                                       ifloor(p.z)}) == CellKind::Solid) {
                        hitDist = d;
                        break;
                      }
                    }
                    StrokeAimAt(pivot, eye + look * hitDist, swRight, swUp,
                                swFwd, aimAz, aimEl, aimDist);
                  }
                }
                const StrokeStepResult r = StepStrokeProgram(
                    playerStrike, sty, melee, aimAz, aimEl, aimDist, kTickDt,
                    swRight, swUp, swFwd);
                stepped = r != StrokeStepResult::Idle;
                strikeCutEdge = playerStrike.Cutting() && !wasCutting;
                if (r == StrokeStepResult::Finished) {
                  playerStrike.Reset();
                  // ...and hand the part back, like MobSystem::StepStroke's
                  // Finished branch. With a sword drawn this falls straight
                  // through to the held item, so an armed player's arm claim
                  // between strikes is exactly what it always was.
                  avatar.ClearStrikeEffector();
                  // Chain the banked strike: promoted to the latch, so it
                  // begins on the next tick through the same door as a fresh
                  // click (one tick of gap, invisible at 30 Hz).
                  if (strikeBuffered >= 0) {
                    strikeQueued = strikeBuffered;
                    strikeBuffered = -1;
                  }
                }
              }
            }
            if (!stepped) {
              // No program stepped the driver this tick: it idles/unwinds
              // exactly as a released button always did, so the arm hands
              // back over the usual ramp. ONE advance per tick either way.
              melee.Update(kTickDt, false, meleeReady, swRight, swUp, swFwd);
            }
          }
          // ---- THE SWING WHOOSH, on the EDGE into Slash --------------------
          // A commit is a moment, so this fires once per cut rather than on
          // every tick the slash is live. Latched (not played here) because
          // this is inside the tick loop and audio drains once per frame.
          //
          // THE VOLUME AND PITCH COME OFF THE SPEED THE GAME ACTUALLY READ,
          // not off the tuning's threshold, so a whoosh is the audible half of
          // "speed is the damage" (melee.h note 2): the player hears the same
          // number the damage curve used, and a lazy wave under
          // combatfx.whooshMinSpeed makes no sound at all rather than a quiet
          // one — which is the honest report that it was not a cut.
          //
          // TWO TRIGGERS, ONE PER MODE, never both: freeform commits Slash by
          // mouse speed, but an authored THRUST drives mostly the radial
          // channel, which commitSpeed never sees — it can finish its whole
          // cut without ever entering Slash (the player-styles gate found
          // exactly this), so in discrete mode the cue keys on the PROGRAM's
          // own cut edge instead. OR-ing the two would whoosh a committed
          // discrete cut twice, one tick apart.
          const bool slashEdge = melee.Phase() == SwingPhase::Slash &&
                                 meleePhasePrev != SwingPhase::Slash;
          if (meleeMode == 0 ? strikeCutEdge : slashEdge) {
            const Tuning::CombatFx& fx = CurrentTuning().combatfx;
            const float lo = fx.whooshMinSpeed;
            const float hi = std::max(melee.tuning.commitSpeed, lo + 1.0f);
            const float sp = melee.MouseSpeed();
            if (sp > lo) {
              combatWhooshCue.pending = true;
              combatWhooshCue.power = std::clamp((sp - lo) / (hi - lo), 0.0f, 1.0f);
              // A POINT ALONG THE BLADE, not either end of it, and the SAME
              // point the voice then follows for the length of the sample (the
              // audio block moves it every frame). The hand alone was where this
              // used to sit: safe, but it barely travels, so a cut across the
              // body made no pan at all; the tip alone swings a metre wide of the
              // player holding it. `combatfx.whooshEdgeFrac` is the dial.
              Vec3 eb, et, ef;
              float ehw = 0;
              combatWhooshCue.at =
                  avatar.WeaponEdge(eb, et, ehw, &ef)
                      ? eb + (et - eb) * fx.whooshEdgeFrac
                      : player.pos;
            }
          }
          meleePhasePrev = melee.Phase();
          if (avatar.Spawned()) {
            // Weight rises while the weapon is up and FADES over the releasing
            // recover, so the arm is handed back to the walk cycle across a
            // couple of hundred ms instead of being dropped in one tick from
            // wherever the player left it (melee.h PoseWeight). The whole pose
            // travels as ONE value — hand, blade axis, blade roll and the
            // elbow's bend pole — because the rig needs all four to put the
            // sword where the stroke says it is.
            avatar.SetWeaponPose(melee.Pose());
          }
        }
        // ---- ARMOUR: what the equipment says vs what the body wears --------
        //
        // The SAME change-detection seam the weapon uses one block up, and in
        // the same place in the frame for the same reason: WearItem builds
        // bodies and joints, so it must run once per CHANGE rather than once
        // per tick, and it must run before PreTick flattens the pose and
        // submits the kinematic targets — a shell appended after that would
        // sit at its spawn pose for a tick and visibly snap into place.
        //
        // Compared BY NAME, which is also what makes this survive an R
        // hot-reload: library indices renumber, the name does not. A respawn
        // clears the rig's shells (BuildRig) and this loop simply puts them
        // back on the next tick, with no despawn/respawn bookkeeping anywhere.
        //
        // `wearTried` is what stops a REFUSED piece from being retried every
        // tick. Comparing against what the body is actually wearing is not
        // enough on its own: a piece that finds no limb to hang on (a helm on
        // a headless mob) leaves the slot un-worn, so the two would disagree
        // forever and WearItem would rebuild nothing, loudly, 30 times a
        // second. This records the last ATTEMPT, which the slot changing is
        // what clears.
        if (avatar.Spawned()) {
          for (int s = 0; s < kEquipSlotCount; s++) {
            if (!EquipSlotIsWorn(s)) continue;
            const ItemStack& st = kit.equip.At(s);
            const ItemDef* want = items.At(st.Empty() ? -1 : st.def);
            const std::string wantName = want ? want->name : std::string();
            const uint32_t wantDye = st.Empty() ? 0u : st.dye;
            if (avatar.WornItem(s) == wantName && wearTried[s] == wantName &&
                wearDye[s] == wantDye)
              continue;
            if (wearTried[s] == wantName && wearDye[s] == wantDye &&
                avatar.WornItem(s).empty() && !wantName.empty())
              continue;   // already refused this one; nothing has changed
            // TAKING IT OFF KEEPS ITS WOUNDS. The shells are the only place
            // the damage lives while the piece is on, and they are destroyed
            // with the slots — so it is read out here, one call before the
            // rig forgets it, and handed back on the next wear. Without this
            // pair, changing boots mends the pair you took off.
            const std::string had = avatar.WornItem(s);
            if (!had.empty()) {
              WornDamage d;
              if (avatar.CaptureWorn(s, d)) kit.SetDamage(had, std::move(d));
            }
            wearTried[s] = wantName;
            wearDye[s] = wantDye;
            if (wantName.empty()) {
              avatar.UnwearItem(s);
            } else if (!avatar.WearItem(want, s, kit.Damage(wantName),
                                        wantDye)) {
              ui.kitMessage = "that does not fit you";
              ui.kitMessageAge = 0.0f;
            }
          }
        }
        // Head look, the other half of the turn policy above: whatever yaw the
        // body did NOT take is what the head is asked for. Computed from the
        // post-turn heading so the two never disagree by a tick — and passed
        // unclamped, because SetLook clamps against the same headLookYaw this
        // block just used and a rig that quietly over-rotates when the two
        // drift is worse than a head that stops at its stop.
        //
        // Third person gets it too, and it is arguably more valuable there:
        // the body faces its travel direction, so strafing or running past a
        // target is exactly when you want the character visibly looking where
        // the player is looking.
        if (avatar.Spawned()) {
          float lookRel = camHeading - avatarHeading;
          while (lookRel > 3.14159265f) lookRel -= 6.2831853f;
          while (lookRel < -3.14159265f) lookRel += 6.2831853f;
          float lookPitch = cam.pitch;
          // WHILE THE CHARACTER SCREEN IS OPEN, the head follows the CURSOR
          // over the portrait instead of the camera — the camera is not
          // moving, so the ordinary rule would leave the character staring
          // fixedly past you while you look them over.
          //
          // The panel reports where the pointer is (portraitLook, normalized
          // to the frame) and this turns it into a look; posing the rig stays
          // game-side, which is why the UI reports a cursor rather than a
          // pose. SetLook clamps against the rig's own neck limits, so the
          // generous gains here cannot over-rotate anything.
          if (ui.inventoryOpen && ui.portraitLookValid) {
            lookRel = ui.portraitLook[0] * 0.9f;
            lookPitch = ui.portraitLook[1] * 0.5f;
          }
          avatar.SetLook(lookRel, lookPitch);
        }
        if (avatar.Spawned())
          avatar.PreTick(tick, player, avatarHeading, kTickDt, world, ops,
                         cellOps,
                         spawns);
        // Dead avatar: hold the corpse for respawnDelay, then rebuild it.
        // The parts are already DebrisSystem's by then, so the corpse stays
        // in the world and settles like any other debris.
        if (avatar.Spawned() && !avatar.IsAlive()) {
          respawnTimer += kTickDt;
          if (respawnTimer >= av.respawnDelay) {
            respawnTimer = 0;
            avatar.Revive(player, avatarHeading);
            tpRig.Snap();
          }
        } else {
          respawnTimer = 0;
        }
        // Drain the impact latch on the FIRST tick of the frame batch that
        // sees it — the same consume-and-clear every other one-shot here uses
        // (cast, prefab stamp, mob spawn). Player::Update writes it per FRAME
        // and peak-holds because this loop runs zero times on most frames at
        // 60+ fps against a 30 Hz tick; without the hold a landing is
        // overwritten unread and fall damage never fires.
        //
        // Cleared even when no avatar consumed it (fly mode, avatar disabled,
        // dead and awaiting respawn). A peak-hold that is never drained only
        // ratchets upward, and the next avatar to spawn would inherit the
        // hardest hit the session ever recorded and die on its first tick.
        player.impactDeltaV = Vec3{0, 0, 0};
        // Same drain, same reason (see Player::jumped): the avatar's `jump`
        // clip is edge-triggered off this latch, and Player::Update sets it
        // per FRAME while this loop runs 0..4 times per frame.
        player.jumped = false;
      }

      // ---- magic (game/spell.h) ---------------------------------------------
      // Same slot in the tick order as mobs and the avatar, and for the same
      // reason: everything a spell does leaves as ops on the streams assembled
      // below. The VM never touches a voxel buffer (thesis 1 / rule 3).
      std::vector<ExplosionOp> spellExps;
      {
        caster.mana.Tick();
        // Dev-panel overrides of the pool (ui/overlay.h devMana*). Applied
        // HERE, after the regen tick and before the cast/bill sites below, so
        // "infinite" is a full pool at every point a tariff is resolved
        // against it. The player's caster only; mobs keep their own pools.
        if (ui.devManaMaxRequest >= 0) {
          caster.mana.manaMax = std::max(1, ui.devManaMaxRequest);
          ui.devManaMaxRequest = -1;
          caster.mana.mana = std::min(caster.mana.mana, caster.mana.EffectiveMax());
        }
        if (ui.devManaFill || ui.devManaInfinite) {
          caster.mana.mana = caster.mana.EffectiveMax();
          caster.mana.regenAccum = 0;
          ui.devManaFill = false;
        }
        SpellEmission emit;

        // What the VM may ask about bodies: where an adopted bomb is, and the
        // nearest mob for a seeking bolt (never the caster's own body).
        struct SpellBodyCtx {
          Physics* phys;
          MobSystem* mobs;
          const Player* player;
          uint64_t playerId;
          const PlayerAvatar* avatar;
        } bodyCtx{&phys, &mobs, &player, 0x9134A5EEu, &avatar};
        SpellBodyProbe bodyProbe;
        bodyProbe.ctx = &bodyCtx;
        // WHAT A FLIGHT RUNS INTO: the first MOVING-layer body on the tick's
        // segment (a mob limb, debris, a bomb), through the same ray the laser
        // and the melee sweep use. The caster's own parts are rejected by
        // OWNERSHIP, not distance (the laser's rule): an arm swings through
        // the muzzle line constantly, and a bolt that went off in the hand
        // would read as the overcast backfire it is not.
        bodyProbe.bodyHit = [](void* c, Vec3 from, Vec3 to, uint64_t casterId, Vec3& out) {
          SpellBodyCtx& bc = *(SpellBodyCtx*)c;
          const Vec3 seg = to - from;
          const float len = seg.len();
          if (len < 1e-4f) return false;
          const Vec3 dir = seg * (1.0f / len);
          float frac = 1.0f;
          const uint64_t h = bc.phys->CastRayBody(from, dir, len, frac);
          if (h == 0) return false;
          if (casterId == bc.playerId && bc.avatar->OwnsBody(h)) return false;
          out = from + dir * (frac * len);
          return true;
        };
        // The body a status attaches to: the nearest mob origin within the
        // radius, else the player. Ids are the mob's own and the player's
        // caster id — opaque to the VM either way.
        bodyProbe.bodyIdAt = [](void* c, Vec3 from, float radius, uint64_t& id, Vec3& out) {
          SpellBodyCtx& bc = *(SpellBodyCtx*)c;
          float best = radius * radius;
          bool found = false;
          for (uint32_t i = 0; i < bc.mobs->MobCount(); i++) {
            const uint64_t mid = bc.mobs->MobIdAt(i);
            const Vec3 p = bc.mobs->MobOrigin(mid);
            const Vec3 d = p - from;
            const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
            if (d2 <= best) {
              best = d2;
              id = mid;
              out = p;
              found = true;
            }
          }
          const Vec3 dp = bc.player->pos - from;
          // The figure box is ~1.7 m: a point anywhere on the body counts.
          const float pr = radius + 9.0f;
          if (!found && dp.x * dp.x + dp.y * dp.y + dp.z * dp.z <= pr * pr) {
            id = bc.playerId;
            out = bc.player->pos;
            found = true;
          }
          return found;
        };
        bodyProbe.bodyPos = [](void* c, uint64_t id, Vec3& out) {
          SpellBodyCtx& bc = *(SpellBodyCtx*)c;
          if (id == bc.playerId) {
            out = bc.player->pos;
            return true;
          }
          for (uint32_t i = 0; i < bc.mobs->MobCount(); i++)
            if (bc.mobs->MobIdAt(i) == id) {
              out = bc.mobs->MobOrigin(id);
              return true;
            }
          return false;
        };
        bodyProbe.bodyAt = [](void* c, uint64_t h, Vec3& out) {
          BodyTransform xf;
          if (!((SpellBodyCtx*)c)->phys->GetTransform(h, xf)) return false;
          out = xf.pos;
          return true;
        };
        bodyProbe.nearestTarget = [](void* c, Vec3 from, uint64_t, Vec3& out) {
          MobSystem& m = *((SpellBodyCtx*)c)->mobs;
          float best = 96.0f * 96.0f;   // seek range, voxels squared
          bool found = false;
          for (uint32_t i = 0; i < m.MobCount(); i++) {
            const Vec3 p = m.MobOrigin(m.MobIdAt(i));
            const Vec3 d = p - from;
            const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
            if (d2 < best) {
              best = d2;
              out = p;
              found = true;
            }
          }
          return found;
        };
        // Consume the latch on the FIRST tick of the frame that sees it, and
        // clear it even when there is nothing spoken — otherwise a click on an
        // empty stack stays queued and fires the next spell the moment one is
        // spoken. Clearing outside the inner test is what makes this a one-shot
        // rather than a pending intent.
        const bool castNow = castQueued;
        castQueued = false;
        // The inspector's "cast it on this part": `self` resolves at the
        // clicked limb's centre, with the effect radii clamped to the part.
        // Same Cast(), one extra argument (plan §7); the VM never learns what
        // a part is.
        const int castPart = castAtPartQueued;
        castAtPartQueued = -1;
        if (castPart >= 0 && !caster.stack.Empty() && avatar.Spawned()) {
          int part = -1;
          if (const MobDef* def = avatar.Def()) {
            for (int i = 0; i < (int)def->limbs.size(); i++)
              if (BodySlotFor(avatar.PartName(i), avatar.PartTag(i)) == castPart &&
                  avatar.PartAlive(i))
                part = i;
          }
          Vec3 at;
          Quat rot;
          if (part >= 0 && avatar.PartWorldTransform(part, at, rot)) {
            const SpellFxVec selfAt{SpellFxFromFloat(at.x), SpellFxFromFloat(at.y),
                                    SpellFxFromFloat(at.z)};
            const Vec3 body = player.pos;
            const SpellFxVec originFx{SpellFxFromFloat(body.x), SpellFxFromFloat(body.y),
                                      SpellFxFromFloat(body.z)};
            const SpellFxVec dirFx{0, kSpellFxOne, 0};
            const SpellProbe probe = WorldSpellProbe(world);
            CastResult res = spells.Cast(caster.compiled, caster.mana, playerHealth,
                                         0x9134A5EEu, originFx, dirFx, tick, emit, &probe,
                                         &selfAt, &bodyProbe);
            caster.lastOutcome = res.outcome;
            if (res.outcome != CastOutcome::Nothing) caster.Clear(glyphs);
          }
        }
        if (castNow && !caster.stack.Empty()) {
          // Origin at the muzzle — in front of the eye so the bolt does not
          // spawn inside the caster's own head. Direction is the aim ray.
          const Vec3 eye = player.EyePos();
          const Vec3 fwd = cam.Forward();
          const Vec3 muzzle = eye + fwd * 1.5f;
          SpellFxVec originFx{SpellFxFromFloat(muzzle.x),
                              SpellFxFromFloat(muzzle.y),
                              SpellFxFromFloat(muzzle.z)};
          SpellFxVec dirFx{SpellFxFromFloat(fwd.x), SpellFxFromFloat(fwd.y),
                           SpellFxFromFloat(fwd.z)};
          // A FATAL cast runs its effect at the CASTER instead — and does so
          // through the same ApplySpellEffect call, with the caster's position
          // as the argument (thesis 2). Nothing here branches on the spell.
          const bool fatal =
              ResolveCast(caster.mana, playerHealth.Get(),
                          caster.compiled.manaCost).outcome == CastOutcome::Fatal;
          if (fatal) {
            const Vec3 body = player.pos;
            originFx = {SpellFxFromFloat(body.x), SpellFxFromFloat(body.y),
                        SpellFxFromFloat(body.z)};
          }
          const SpellProbe probe = WorldSpellProbe(world);
          CastResult res =
              spells.Cast(caster.compiled, caster.mana, playerHealth,
                          0x9134A5EEu /*casterId*/, originFx, dirFx, tick, emit,
                          &probe, nullptr, &bodyProbe);
          caster.lastOutcome = res.outcome;
          if (res.outcome != CastOutcome::Nothing) caster.Clear(glyphs);
        }

        // A held beam follows the aim while RMB stays down.
        {
          const Vec3 eye = player.EyePos();
          const Vec3 fwd = cam.Forward();
          const Vec3 muzzle = eye + fwd * 1.5f;
          spells.HoldBeam(0x9134A5EEu,
                          {SpellFxFromFloat(muzzle.x), SpellFxFromFloat(muzzle.y),
                           SpellFxFromFloat(muzzle.z)},
                          {SpellFxFromFloat(fwd.x), SpellFxFromFloat(fwd.y),
                           SpellFxFromFloat(fwd.z)},
                          beamHeld);
        }
        if (dropStatusQueued) {
          dropStatusQueued = false;
          spells.DropNewestStatus(0x9134A5EEu);
        }
        spells.Tick(tick, world, classOf, emit, &bodyProbe);
        // Impact flashes: born from this tick's resolves, aged per tick.
        for (SpellFlash& fl : spellFlashes) fl.ttl--;
        spellFlashes.erase(std::remove_if(spellFlashes.begin(), spellFlashes.end(),
                                          [](const SpellFlash& f) { return f.ttl <= 0; }),
                           spellFlashes.end());
        for (const SpellImpactFx& fx : emit.impacts) {
          if (spellFlashes.size() >= 24) break;
          SpellFlash fl;
          fl.at = Vec3{SpellFxToFloat(fx.at.x), SpellFxToFloat(fx.at.y), SpellFxToFloat(fx.at.z)};
          fl.color = fx.tint != 0 && fx.tint < mats.size()
                         ? mats[fx.tint].gpu.color0
                         : glyphs.Delivery(fx.deliveryGlyph).look.color;
          fl.radius = (float)std::clamp(fx.radius, 1, 12);
          fl.ttl = fl.ttl0 = 9;
          spellFlashes.push_back(fl);
        }

        // THE PER-TICK BILL. Statuses and a held beam pay the same tariff as
        // they emit (plan §4): mana first, then the body, and when neither
        // can pay the caster has run dry and everything they sustain drops.
        {
          int32_t bill = 0;
          for (const SpellBill& sb : emit.bills)
            if (sb.casterId == 0x9134A5EEu) bill += sb.amount;
          if (bill > 0) {
            const int32_t fromMana = std::min(bill, caster.mana.mana);
            caster.mana.mana -= fromMana;
            bill -= fromMana;
            if (bill > 0) {
              const int32_t hp = playerHealth.Get();
              if (hp > bill) {
                playerHealth.Spend(bill);
              } else {
                spells.DropAll(0x9134A5EEu);   // dry: it all goes out
              }
            }
          }
          caster.mana.reserved = spells.ReservationFor(0x9134A5EEu);
        }
        // A sustained gravity mod on the player's own body.
        for (const SpellBodyImpulse& bi : emit.bodyImpulses)
          if (bi.target == 0x9134A5EEu) player.vel.y += bi.vps.y;
        // GRAFTS: the world half already left as ops; the body half fills the
        // caster's missing anatomy cells with that matter, root-first. The VM
        // cannot reach a body (thesis 4); the owner does it.
        for (const SpellRestore& rs : emit.restores) {
          if (rs.casterId == 0x9134A5EEu) {
            if (avatar.Spawned()) avatar.RestoreBody(rs.material, rs.count);
          } else {
            mobs.RestoreMob(rs.casterId, rs.material, rs.count);
          }
        }
        // WARDS filter the spell's OWN emission too (a fire aura inside an
        // anti-fire ward is refused like anyone else's).
        ui.spellRefused = spells.FilterStreams(emit.ops, emit.explosions, emit.spawns, emit.winds);

        // BOMBS ARE DEBRIS. The VM asked for a rigid body; this is the owner
        // making one through the same path a dropped item takes (a Jolt
        // sphere so it rolls, a voxel ball to draw it, adopted by the debris
        // system so it falls, settles, burns and can be blown apart) and
        // handing the handle back. When the fuse runs out the VM resolves the
        // payload where the body is and asks for it to be taken away.
        for (const SpellBodyRequest& rq : emit.bodyRequests) {
          const float r = rq.radius;
          std::vector<DebrisVoxel> ball;
          const int ext = (int)std::ceil(r);
          for (int z = -ext; z < ext; z++)
            for (int y = -ext; y < ext; y++)
              for (int x = -ext; x < ext; x++) {
                const float dx = x + 0.5f, dy = y + 0.5f, dz = z + 0.5f;
                if (dx * dx + dy * dy + dz * dz <= r * r)
                  ball.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, (uint16_t)rq.material});
              }
          BodyTransform xf{};
          xf.pos = rq.pos;
          xf.quat[3] = 1;
          const float density =
              rq.material < mats.size() ? (float)mats[rq.material].gpu.density : 2000.0f;
          const uint64_t h = phys.CreateSphereBody(rq.pos, r, density);
          if (!h) continue;
          phys.SetBodyVelocity(h, rq.vel);
          phys.ReleaseToWorldWhenClear(h);
          debris.AdoptBody(h, std::move(ball), xf);
          spells.AdoptBody(rq.token, h);
        }
        for (uint64_t h : emit.bodyDone) debris.DestroyBody(h);
        // An anchored gravity Mod acting on the caster's own body (a hop).
        if (emit.casterImpulseVps.y != 0.0f) player.vel.y += emit.casterImpulseVps.y;

        // THE WILDCARD'S BILL. `anything` is priced by what it turned out to
        // be, when it resolves (plan §4): mana first, then the body, exactly
        // the crossover a spoken cost pays -- except that this one lands after
        // the fact, which is the danger the word is for.
        if (emit.billOnResolve > 0) {
          int32_t bill = emit.billOnResolve;
          const int32_t fromMana = std::min(bill, caster.mana.mana);
          caster.mana.mana -= fromMana;
          bill -= fromMana;
          if (bill > 0) playerHealth.Spend(bill);
          ui.spellLastBill = emit.billOnResolve;
          ui.spellLastBillAge = 0.0f;
        }

        // The caster's own body pays for a fatal overcast: severed parts, then
        // death, all through the existing dismemberment/gore pipeline. The
        // avatar half is separate from the world half above only because the
        // VM may not reach into PlayerAvatar (thesis 4).
        if (emit.carveCaster && avatar.Spawned())
          avatar.SelfDestruct(emit.carveAt, emit.carveRadius, world, spawns);

        // OP BUDGET FAIRNESS (§F). Magic gets an explicit reservation rather
        // than silently sharing the 64-op tick budget with mob bleeding (6)
        // and avatar bleeding (6). Anything past it is COUNTED, not dropped
        // silently — a spell that sometimes doesn't fire is miserable to
        // diagnose, so the overflow is visible in the HUD.
        int spellOps = 0;
        for (const BrushOp& b : emit.ops) {
          if (spellOps >= SpellSystem::kSpellOpsPerTick ||
              ops.size() >= kMaxOpsPerTick) {
            ui.spellOpsDropped++;
            continue;
          }
          ops.push_back(b);
          spellOps++;
        }
        // Explosions are carried to the explosion block below, where `exps`
        // exists — a spell blast must go through the SAME path as a grenade
        // (island checks, body damage, mob carving, impulse), not a second one.
        spellExps = std::move(emit.explosions);
        for (const ParticleSpawn& p : emit.spawns) spawns.push_back(p);
        // WIND PRIMITIVES (docs/RESEARCH_wind.md §4.3). The VM reported the
        // intent; this is the owner splicing it on, exactly as it does for
        // brush ops and particle spawns above. `spawnTick` is stamped HERE and
        // not in the VM, because a primitive's whole motion and lifetime are
        // f(t - spawnTick) and the tick a spell resolves on is the caller's
        // fact, not the VM's.
        //
        // Spawn() refuses when the world list is full (32) rather than evicting
        // someone's fan, and the refusal is COUNTED for the same reason the op
        // overflow above is: "my gust sometimes does nothing" is miserable to
        // diagnose from silence.
        for (WindPrim w : emit.winds) {
          w.spawnTick = tick;
          w.ownerId = 0x9134A5EEu;   // the same casterId the projectile carries
          if (!WindPrims().Spawn(w)) ui.windPrimsDropped++;
        }

        // ---- the dev-panel producer (docs/RESEARCH_wind.md §4.3) ------------
        // A placed fan, from the same panel the wind multipliers live on. It
        // exists because the gameplay producers are CONTENT — a gust is a glyph
        // in glyphs.json, a fan will be a prefab tag — and neither is a good way
        // to answer "what does a 30 m/s cone actually do to that dune?".
        //
        // It goes through WindPrims().Spawn() like everything else. There is no
        // dev-only path into the wind system, which is what makes what you see
        // here the same thing a spell would produce.
        //
        // ANCHORED IN FRONT OF THE CAMERA, aimed along the view ray, with an
        // INFINITE TTL: that is what makes it a fan rather than a gust, and it
        // is why "clear all" exists next to it. A fan holding its footprint
        // awake forever is a rule-2 leak if you cannot retire it.
        if (ui.placeWindFan) {
          ui.placeWindFan = false;
          const Vec3 fwd = cam.Forward();
          const Vec3 at = player.EyePos() + fwd * 2.0f;
          WindPrim w{};
          w.x = ifloor(at.x);
          w.y = ifloor(at.y);
          w.z = ifloor(at.z);
          w.kind = ui.windFanKind == 1   ? kWindPrimBurst
                   : ui.windFanKind == 2 ? kWindPrimVortex
                                         : kWindPrimCone;
          w.radius = ui.windFanRadius;
          w.reach = ui.windFanReach;
          w.ttl = kWindPrimForever;
          w.spawnTick = tick;
          w.ownerId = kDevFanOwner;
          w.flags = kWindPrimAir |
                    (ui.windFanEntrain ? kWindPrimEntrain : 0u);
          // A vortex with no swirl is a cone with extra steps, so the two
          // shares are given here rather than left to the panel: 1.0 of the
          // core speed tangentially and 0.5 axially is a tornado that both
          // spins and lifts. They are the primitive's parameters, not knobs
          // anyone has asked for yet.
          if (w.kind == kWindPrimVortex) {
            w.swirlQ = 65536;
            w.riseQ = 32768;
          }
          WindPrimAim(w, fwd, ui.windFanSpeed);
          if (!WindPrims().Spawn(w)) ui.windPrimsDropped++;
        }
        if (ui.clearWindFans) {
          ui.clearWindFans = false;
          // Only the dev-placed ones: a spell's gust owns itself and expires on
          // its own TTL, and clearing it from here would be the panel reaching
          // into gameplay.
          WindPrims().RetireOwner(kDevFanOwner);
          ui.windPrimsDropped = 0;
        }
      }

      // ---- --fell-tree: the tree-fell gate's cut, in the live frame loop ----
      // Same fixture (selftest::BuildTree), same cut (a 9x9x3 slab of air ten
      // cells up plus the destruction event the brush would raise), same
      // 300-tick profile window. What differs is that this loop RENDERS.
      if (g_fellTree) {
        static int fellPhase = 0;  // 0 waiting, 1 planted, 2 cut, 3 reported
        static uint32_t fellPlantTick = 0, fellCutTick = 0;
        static int fellGroundY = 0;
        static size_t fellFrame0 = 0;
        static bool fellBodySeen = false;
        static bool fellTreeSeen = false;
        static selftest::TreeFixture fellTree;
        auto matByName = [&](const char* n) -> uint32_t {
          for (size_t i = 0; i < mats.size(); i++)
            if (mats[i].name == n) return (uint32_t)i;
          return 0u;
        };
        if ((tick % 60u) == 0u)
          std::printf("--fell-tree: tick %u player (%.1f,%.1f,%.1f) yaw %.2f fly %d\n",
                      tick, player.pos.x, player.pos.y, player.pos.z, cam.yaw,
                      player.fly ? 1 : 0);
        if (fellPhase == 0 && tick >= (uint32_t)g_fellTreeAt) {
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
          for (int y = cutY; y < cutY + 3; y++)
            for (int dz = -4; dz <= 4; dz++)
              for (int dx = -4; dx <= 4; dx++) {
                const IVec3 cc{fx + dx, y, fz + dz};
                if (!world.CellInWindow(cc)) continue;
                if (cellOps.size() < kMaxCellOpsPerTick)
                  cellOps.push_back({World::SlotCellIndex(cc), 0u});
              }
          debris.AddDestructionEvent(tick, {fx - 5, fellGroundY + 10, fz - 5},
                                     {fx + 5, fellGroundY + 15, fz + 5});
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
      }

      // support-loss flags from the sim (burnt stems, undermined slabs) feed
      // the same island-check pipeline as explosions and brush erases
      debris.QueueSupportEvents(world.Snap());
      // island detection results + body burn + terrain collision upkeep
      // (may add cell ops and particle spawns from shattered bodies)
      debris.PreTick(tick, world, cellOps, spawns);
      // ---- fluid lab scene driver (lab/lab.h) ----
      // Advances the scene clock once per SIM tick (never per frame): the
      // build CellOps land on scene tick 1 and the pour follows its fixed
      // schedule, so a run — and every L-reset replay — is deterministic.
      // Pour budget is charged against the live estimate before emission,
      // the same rule as the mpm tool above.
      if (labScene >= 0) {
        labTick++;
        LabSceneBuildOps(labScene, labTick, fluidCueMat, cellOps);
        // The pond scenes' whole experiment: a still body, then a plug pulled
        // from under it. Same op stream as the build, so L replays it.
        if (LabScenePlugTick(labScene) == labTick)
          LabScenePlugOps(labScene, cellOps);
        LabScenePour(labScene, labTick, fluidCount, fluidCueMat, fluidSpawns);
      }
      // laser kerf into a body, deferred from the input block above so it can
      // reach `spawns` (a cut that severs the body sheds the loose bits)
      if (laserCut.body) {
        // A live limb is carved (eject=false: the beam vaporizes, and a held
        // laser spraying gobbets every tick would drain the particle ring);
        // plain debris melts. The two paths are the same operation on the two
        // populations — see game/mob.h.
        if (laserCut.limb)
          mobs.CarveLimbRadial(laserCut.body, laserCut.at, laserCut.radius,
                               false /*ragged*/, false /*eject*/, world, spawns);
        else
          debris.MeltBodyAt(laserCut.body, laserCut.at, laserCut.radius, world,
                            spawns);
      }
      // ---- the sword bites (game/melee.h) ---------------------------------
      // THE POSE IS THE HITBOX. The blade's authored `edge` segment is read
      // through its LIVE transform and swept from where it was last tick to
      // where it is now; anything that quad passes through is cut. Nothing
      // here consults the camera, so what you hit is exactly what the visible
      // blade travelled through — which is what makes the wound land where the
      // player aimed rather than where a cone in front of the crosshair says.
      //
      // Deferred to this point for the same reason the laser kerf is: a carve
      // needs the `spawns` list debris.PreTick fills just above.
      avatar.SetSwinging(melee.Cutting() || playerStrike.Cutting());
      if (avatar.Spawned() && meleeReady) {
        Vec3 eb, et, ef;
        float ehw = 0;
        if (avatar.WeaponEdge(eb, et, ehw, &ef)) {
          // EITHER cut counts, and no mode read is needed: a freeform tick
          // keeps the cursor Idle, and a discrete one may cut without ever
          // committing Slash — an authored THRUST drives mostly the radial
          // channel, which commitSpeed never sees, so gating on the driver
          // alone made thrusts free actions (the player-styles gate caught
          // it). This is the NPC's own contract: MobSystem::StepStroke sweeps
          // on the CURSOR's cut, and tip speed still scales the damage.
          // ONE STROKE, ONE IMPULSE PER SLOT (melee.h EdgeSweep::struck). The
          // set is cleared on the first tick neither cut state is live, which
          // is the same "is a cut happening" test the sweep gates on -- so a
          // freeform wave and a discrete program both get exactly one blunt
          // hit per body per swing with no mode read of their own.
          if (!(melee.Cutting() || playerStrike.Cutting())) {
            playerStruck.clear();
            playerBitten = false;
          }
          if (lastEdgeValid && (melee.Cutting() || playerStrike.Cutting())) {
            EdgeSweep sw;
            sw.aPrev = lastEdgeBase;
            sw.bPrev = lastEdgeTip;
            sw.aNow = eb;
            sw.bNow = et;
            sw.flatNow = ef;
            sw.dt = kTickDt;
            sw.halfWidth = ehw;
            // ---- WHOSE BLOW IS THIS: THE SWORD'S, OR THE FIST'S? ----------
            // `WeaponEdge` above already reported whichever the effector
            // names, so this is the matching half: the numbers come from the
            // same weapon the segment did. A fist's profile passes through
            // `StrikeProfileFor`, so a worn gauntlet upgrades the player's
            // punch exactly as it upgrades an NPC's (plan section 3). No
            // blade behind a fist, so the neutral heft (EdgeSweep::heft's
            // documented default) rather than an item's volume ratio.
            if (const MobNaturalWeaponDef* fist = avatar.EffectorWeapon()) {
              sw.strike = avatar.StrikeProfileFor(*fist);
            } else if (heldItem != nullptr) {
              // The whole profile, not a number (game/impact.h): a sword
              // fills mostly `cut` and behaves exactly as it did, a mace
              // fills `blunt`.
              sw.strike = heldItem->strike;
              sw.carveBonus = heldItem->carveBonus;
              // HEFT: the weapon's own volume against the reference, so a
              // greatsword cuts deeper than a knife because it IS bigger
              // (item.h ItemDef::heftVolume). Derived from the art, resolved
              // here because only the caller knows which item is in the fist.
              const auto& goreT = CurrentTuning().gore;
              sw.heft = heldItem->HeftFactor(goreT.woundHeftRef,
                                             goreT.woundHeftMax);
            }
            sw.tick = tick;
            // BLUNT AND BITE ARE IMPULSES (melee.h EdgeSweep::struck). The
            // player has TWO cut states -- the discrete program's cursor and
            // the freeform driver's own Slash -- so the set is owned here and
            // cleared on the tick neither is cutting, which is mode-blind and
            // is the same line the sweep gate below already reads.
            sw.struck = &playerStruck;
            sw.bitten = &playerBitten;
            // A FIST IS PART OF THE ARM THAT THROWS IT (melee.h selfMounted).
            sw.selfMounted = avatar.EffectorWeapon() != nullptr;
            sw.valid = true;
            // ---- WHAT THE BLOW WAS, MEASURED FROM WHAT IT LEFT BEHIND ------
            //
            // The sweep reports how many bodies it hit and how hard, but not
            // WHAT it hit. Rather than widen EdgeSweepResult — which would put
            // the answer inside MeleeSweepDamage, where two people are working
            // — the tier is read off the two event queues the sweep already
            // fills as a side effect, by differencing them across the call:
            //
            //   severs grew  -> a limb came off. The biggest of everything.
            //   voices grew  -> a LIVE creature was hurt: flesh.
            //   neither, but bodiesHit -> debris, a dropped item, a held
            //                             weapon. A chip.
            //
            // Both queues are frame-drained (down in the audio block), so the
            // sizes only ever grow within a frame and a difference is exactly
            // "what this call added". No new plumbing, no shared struct.
            //
            // KNOWN AND ACCEPTED: Hurt voices de-duplicate per mob per drain
            // window (MobSystem::PushVoice), so a SECOND cut into the same
            // creature in the same frame reads as a chip rather than as flesh.
            // The blow still flashes and still hurts — only the tier of the
            // dip and the choice of cue are affected, and only for a repeat
            // inside 16 ms. The alternative is a per-call "did I hurt live
            // flesh" flag threaded out of the sweep, which is the shared
            // surface this is avoiding.
            //
            // A PARRY IS NOT ONE OF THESE TIERS, and deliberately so: an
            // arrested sweep returns before any probe runs, so `bodiesHit` is
            // 0 and none of this fires. The clang and the dip for a blocked
            // blow come off the BlockEvent queue instead (the drain below the
            // AI readout), which is the only place that knows a block from a
            // chip. One blow, one cue, whichever way it ended.
            const size_t sev0 = mobs.SeverEvents().size();
            const size_t voi0 = mobs.VoiceEvents().size();
            const EdgeSweepResult res = MeleeSweepDamage(
                sw, melee.tuning, avatar, phys, mobs, debris, world, spawns);
            // PARRIED BY AN NPC'S BLADE. The sweep reports; ending the stroke
            // is the caller's job, because MeleeSweepDamage has no business
            // reaching into whichever driver happens to own this swing
            // (melee.h EdgeSweepResult). The player's cut stops here: no
            // follow-through, no second bite past the blade that stopped it.
            if (res.arrested) {
              melee.Arrest();
              // The program mirrors the driver, exactly as the NPC's stroke
              // does on a parry (MobSystem::StepStroke): the remaining cut
              // ticks are abandoned and the recover starts now.
              if (playerStrike.Cutting()) {
                playerStrike.phase = StrokeCursor::Phase::Recover;
                playerStrike.phaseTick = 0;
              }
            }
            if (res.bodiesHit > 0) {
              const bool severed = mobs.SeverEvents().size() > sev0;
              const bool flesh = mobs.VoiceEvents().size() > voi0;
              const Tuning::CombatFx& fx = CurrentTuning().combatfx;
              // LATCHED, not acted on. This is inside the tick loop, which
              // runs 0..4 times a frame; the frame loop drains it at the top
              // of the next frame (see HitStop's note).
              if (severed)
                hitStop.Request(fx.hitStopSeverScale, fx.hitStopSeverMs);
              else if (flesh)
                hitStop.Request(fx.hitStopFleshScale, fx.hitStopFleshMs);
              else
                hitStop.Request(fx.hitStopChipScale, fx.hitStopChipMs);
              // The impact cue, latched for the same reason and peak-held on
              // power so one frame carrying four tick-hits plays the hardest
              // of them once rather than four overlapping copies of nearly the
              // same sound.
              CombatCueRequest& q = flesh ? combatFleshCue : combatClangCue;
              const float pw = res.power * res.edgeAlign;
              // AT THE CONTACT POINT, not at the middle of the blade. The
              // segment midpoint is where the WEAPON is; on a sword that is up
              // to half a metre from the wound, and a listener standing right in
              // front of what they just hit could hear the blow off to one side
              // (reported 2026-09-19). `hitAt` is the probe ray's own hit
              // position — the same place the kerf is bored.
              const Vec3 hitAt = res.hasHitAt ? res.hitAt
                                              : (sw.aNow + sw.bNow) * 0.5f;
              if (!q.pending || pw > q.power) {
                q.power = pw;
                q.at = hitAt;
              }
              q.pending = true;
              // Weapon impact layer: the sword's ring or the mace's thud,
              // on ANY body contact regardless of flesh/armor.
              const bool edged = sw.strike.cut > sw.strike.blunt;
              if (!combatStrikeCue.pending || pw > combatStrikeCue.power) {
                combatStrikeCue.power = pw;
                combatStrikeCue.at = hitAt;
                combatStrikeEdged = edged;
              }
              combatStrikeCue.pending = true;
              // Wet cutting layer: edged weapons contacting flesh.
              if (flesh && edged) {
                if (!combatCutCue.pending || pw > combatCutCue.power) {
                  combatCutCue.power = pw;
                  combatCutCue.at = hitAt;
                }
                combatCutCue.pending = true;
              }
            }
          }
          lastEdgeBase = eb;
          lastEdgeTip = et;
          lastEdgeValid = true;
        } else {
          lastEdgeValid = false;   // sheathed or severed: no segment to sweep
        }
      } else {
        lastEdgeValid = false;
      }

      // prefab stamps drain after island ops (they win same-cell conflicts)
      placer.PreTick(world, cellOps);

      // explosions: X-detonate at the crosshair + grenade fuses + spell blasts
      std::vector<ExplosionOp> exps;
      // Spell blasts join here rather than getting their own path, so a
      // firebolt's detonation gets the island checks, body damage, mob carving
      // and impulse a grenade already gets — for free, and consistently.
      for (const ExplosionOp& e : spellExps)
        if (exps.size() < kMaxExplosionsPerTick) exps.push_back(e);
      if (ui.pendingDetonate) {
        ui.pendingDetonate = false;
        const WorldSnapshot& snap = world.Snap();
        if (snap.valid && snap.pick[0] != 0) {
          exps.push_back({(int)snap.pick[2], (int)snap.pick[3], (int)snap.pick[4],
                          CurrentTuning().tools.detonateRadius,
                          CurrentTuning().tools.detonatePower, 0, 0, 0});
        }
      }
      for (size_t i = 0; i < grenades.size();) {
        if (UpdateGrenade(grenades[i], kTickDt, kindAt)) {
          if (exps.size() < kMaxExplosionsPerTick) {
            exps.push_back({ifloor(grenades[i].pos.x), ifloor(grenades[i].pos.y),
                            ifloor(grenades[i].pos.z),
                            CurrentTuning().grenade.blastRadius,
                            CurrentTuning().grenade.blastPower, 0, 0, 0});
          }
          grenades[i] = grenades.back();
          grenades.pop_back();
        } else {
          i++;
        }
      }
      if (!exps.empty()) {
        everExploded = true;
        lastExplosionTick = tick;
        for (const ExplosionOp& e : exps) {
          debris.AddDestructionEvent(tick, {e.x - e.radius, e.y - e.radius, e.z - e.radius},
                                     {e.x + e.radius, e.y + e.radius, e.z + e.radius});
          // Blow voxels OFF the bodies in range before shoving what survives:
          // an explosion next to a rigidbody now craters it, and splits it into
          // separate bodies when the crater severs it. Runs first so the
          // impulse below acts on the post-damage bodies (including the new
          // fragments, which is what makes a blown-apart object scatter).
          const Vec3 ec{(float)e.x + 0.5f, (float)e.y + 0.5f, (float)e.z + 0.5f};
          const float edr =
              (float)e.radius * CurrentTuning().physics.explosionBodyDamageScale;
          debris.DamageBodiesRadial(ec, edr, world, spawns);
          // Living flesh craters too: a blast next to a mob tears voxels off
          // its limbs, and takes a limb clean off when it removes enough of it.
          // Same call shape as the debris line above — that parallel is the
          // point (game/mob.h).
          mobs.CarveMobsRadial(ec, edr, world, spawns);
          avatar.CarveRadial(ec, edr, world, spawns);
          // The per-body impulse is for DEBRIS. A living creature's limbs are
          // skipped whether kinematic (standing) or dynamic (already limp
          // from an earlier blast): impulse / limb mass on a 0.3 kg hand is
          // 170 m/s and the joints drag the rest of the rig after it —
          // "bodies zoom across the map". The rig takes ONE launch below.
          std::vector<uint64_t> rigBodies;
          mobs.AppendLiveLimbBodies(rigBodies);
          if (avatar.Spawned()) avatar.AppendLiveLimbBodies(rigBodies);
          std::sort(rigBodies.begin(), rigBodies.end());
          phys.ApplyRadialImpulse(
              Vec3{(float)e.x, (float)e.y, (float)e.z},
              (float)e.radius * CurrentTuning().physics.explosionImpulseRadiusScale,
              (float)e.power * CurrentTuning().physics.explosionImpulseScale,
              &rigBodies);
          // ...and the LIVING are knocked flying. A standing creature's limbs
          // are kinematic, so the impulse above never touched them; this is
          // the blast's other half (Mob::BlastRadial): go limp, take a launch
          // velocity of impulse / body mass toward away-from-the-blast,
          // capped at ragdoll.maxLaunchSpeed, and get back up once landed.
          {
            const auto& rg = CurrentTuning().ragdoll;
            const float reach = (float)e.radius * rg.blastRadiusScale;
            const float impulse = (float)e.power * rg.blastImpulseScale;
            mobs.BlastMobsRadial(ec, reach, impulse);
            if (avatar.Spawned()) avatar.BlastRadial(ec, reach, impulse);
          }
          stream.MarkModifiedBox({e.x - e.radius, e.y - e.radius, e.z - e.radius},
                                 {e.x + e.radius, e.y + e.radius, e.z + e.radius});
        }
      }
      // CPU-known writes mark chunks modified now — eviction can't wait for
      // the latent dirty-flag snapshot
      for (const BrushOp& b : ops)
        stream.MarkModifiedBox({b.x - b.radius, b.y - b.radius, b.z - b.radius},
                               {b.x + b.radius, b.y + b.radius, b.z + b.radius});
      // body-shatter spawns keep the particle passes alive exactly like
      // explosions do (a fragment must fly and land on later ticks too)
      if (!spawns.empty()) {
        everExploded = true;
        lastExplosionTick = tick;
      }
      bool particlesActive =
          everExploded &&
          (tick - lastExplosionTick < 400 || world.Snap().particleCount > 0);
      // MPM fluid sheds micro droplets into the particle system (sim_fluid
      // g2p), so live fluid must keep the particle passes awake too — and for
      // a droplet-lifetime tail after the fluid clears, so spray in flight
      // finishes its arc instead of freezing mid-air. Derived from the
      // CPU-owned count + tick only (rule 1).
      if (fluidCount > 0) lastFluidTick = tick;
      particlesActive = particlesActive ||
          (lastFluidTick != 0 && tick - lastFluidTick < 300);

      IVec3 pc{ifloor(player.pos.x) / (int)kChunk, ifloor(player.pos.y) / (int)kChunk,
               ifloor(player.pos.z) / (int)kChunk};
      double t0 = NowSeconds();
      spanGame.Close();
      // ---- the celestial clock (sim/world.h) -----------------------------
      // Advanced exactly ONCE per sim tick, here, immediately before the
      // submit that reads it. The dev overlay's time-speed slider scales it,
      // and it feeds BOTH the rendered sky and TickParams.dayPhase — so
      // cranking time makes the world react (freezing, melting, evaporation)
      // instead of just racing the sun over a world that ignores it.
      //
      // The clock stays DISENGAGED until the slider first leaves 1.0x, at
      // which point it adopts the current tick so the sky does not jump. While
      // disengaged the celestial tick is the sim tick byte for byte, which is
      // what makes every headless path (and the pinned hash) unaffected.
      Celestial().SetScale(ui.timeScale, tick);
      Celestial().Advance();
      // ---- the authored edit layer (sim/worldedit.h) ----------------------
      // Whatever worldgen produced this tick — startup, a window shift, a
      // regen — the layer's chunks were queued at the point of generation and
      // are paid out here, on the ordinary CellOp stream, bounded by the same
      // per-tick cap everything else on that stream respects. Rule 3: this is
      // the ONLY place the layer touches the world, and it touches it through
      // the queue.
      if (WorldEditLayer().HasPending() && cellOps.size() < kMaxCellOpsPerTick)
        WorldEditLayer().Drain(world, cellOps,
                               kMaxCellOpsPerTick - (uint32_t)cellOps.size());
      phys.MovePlayerBody(playerBody, player.pos, kTickDt);
      // WARDS, AT THE SPLICE (DESIGN.md §8): a live filter refuses ops of its
      // word's kind within its radius, whoever produced them — the brush, a
      // mob, a spell. The op stream, never the CA: acid already flowing still
      // flows, and that is the counterplay.
      {
        std::vector<WindPrim> noWinds;
        ui.spellRefused += spells.FilterStreams(ops, exps, spawns, noWinds);
      }
      // ---- W3: THE SWIMMER'S WAKE (docs/PLAN_water_relevel.md §5) --------
      //
      // HERE, on the TICK, and not beside player.Update above — which runs per
      // FRAME. The impulse queue is consumed once per sim tick, so a frame-rate
      // emitter would queue three or four records for one swimmer at 120 Hz and
      // shove the lake harder on a fast machine than on a slow one. One tick,
      // one record.
      //
      // THE SAME DOOR THE BLAST USES (SubmitTick's WaterBodyNoteBlast call):
      // one queue, one record type, one kernel term. The conversion — velocity
      // to Q8, submersion threshold, strength — lives in
      // WaterBodyNoteSwimmer so the gate exercises the arithmetic the game
      // runs rather than a copy of it.
      //
      // `player.vel` is VOXELS PER SECOND (player.cpp divides its m/s knobs by
      // kVoxelMeters); the door wants voxels per TICK.
      //
      // MOBS ARE NOT WIRED and that is a gap, not an omission of effort: a Mob
      // carries no submersion or swim state at all today (game/mob.h has no
      // such field), so there is nothing to read. The door is generic — a mob
      // that gains one is this same call with different arguments.
      //
      // At sim.waveSwimWake 0 nothing is queued at all.
      if (player.inLiquid) {
        WaterBodyNoteSwimmer(WaterBodies(), ifloor(player.pos.x),
                             ifloor(player.pos.z), player.vel.x * kTickDt,
                             player.vel.z * kTickDt, player.submersion,
                             CurrentTuning().sim.waveSwimWake);
      }
      double tSubmit0 = NowSeconds();
      SubmitTick(ctx, world, sim, tick, kDefaultSeed, ops, exps, cellOps,
                 tick % 15 == 0 /*hash occasionally*/, pc, true, particlesActive,
                 spawns, farCount, fluidSpawns, fluidCount, fluidSpeciesMat,
                 ui.showDirtyVoxels);
      // Conservative estimate refresh: the newest snapshot's GPU-owned count
      // plus every spawn batch it has not seen yet. Settles decay it (the
      // snapshot count shrinks); excites grow it one snapshot late, which the
      // seam's recording predicate covers (Simulation::EncodeTick).
      if (!fluidSpawns.empty())
        fluidPendingSpawns.push_back({tick, (uint32_t)fluidSpawns.size()});
      {
        const WorldSnapshot& fsn = world.Snap();
        uint32_t pend = 0;
        if (fsn.valid) {
          std::erase_if(fluidPendingSpawns,
                        [&](const std::pair<uint32_t, uint32_t>& p) {
                          return p.first <= fsn.tick;
                        });
          for (const auto& p : fluidPendingSpawns) pend += p.second;
          fluidCount = std::min(fsn.fluidLive + pend, kFluidCap);
        } else {
          fluidCount = std::min(fluidCount + (uint32_t)fluidSpawns.size(),
                                kFluidCap);
        }
      }
      ui.fluidCount = fluidCount;
      double tSubmit1 = NowSeconds();
      phys.Step(kTickDt);   // CPU physics overlaps the GPU tick
      double tPhys1 = NowSeconds();
      debris.PostStep();
      mobs.PostStep();
      avatar.PostStep();
      // ---- the player follows a ragdolled body ----
      // Limp: the capsule rides the pelvis wherever Jolt threw it, so the
      // camera goes with the body. Getting up: it sits on the standing spot
      // the get-up chose, so the controller resumes exactly there. The body
      // facing is copied back into the heading policy so the first driven
      // tick after the get-up does not snap the rig round to the camera.
      bool avatarRagdolled = false;
      {
        Vec3 follow;
        if (avatar.RagdollFollow(follow)) {
          player.pos = follow;
          player.vel = {};
          avatarHeading = avatar.Heading();
          avatarRagdolled = true;
        }
      }
      // debris that ended the step overlapping the player pushes the player
      // out (fly mode ignores collision entirely, matching the voxel rules;
      // a ragdolled player is being placed by the body, not the solver)
      if (!player.fly && !avatarRagdolled)
        player.ApplyPush(phys.PlayerPushOut(playerBody, player.pos), kindAt);
      double tEnd = NowSeconds();
      tickMsSmooth += ((float)((tEnd - t0) * 1000.0) - tickMsSmooth) * 0.1f;
      // Accumulate this tick into the frame's sample rather than sending it.
      // A frame may run 0..4 ticks, and a chart whose x axis is FRAMES has to
      // show what the frame cost — sending per tick made a 4-tick frame look
      // like four cheap frames and a 0-tick frame look like a gap.
      {
        using sandvox::PerfScope;
        // t0..tSubmit0 is the celestial clock, the authored edit-layer drain
        // and MovePlayerBody — game logic, not streaming. It was billed to
        // `stream` and it is one of the reasons that row never matched what the
        // window shift actually cost.
        //
        // tSubmit0..tSubmit1 is NOT billed here any more: SubmitTick bills
        // ITSELF, in five spans (upload / waterBody / pageTableCpu / encode /
        // submit), because "submit spiked to 30 ms" was never a diagnosis. The
        // spans go into the process-global accumulator and are drained into
        // this sample at the bottom of the frame.
        sandvox::PerfScopeAdd(PerfScope::GameLogic, t0, tSubmit0);
        sandvox::PerfScopeAdd(PerfScope::Physics, tSubmit1, tPhys1);
        sandvox::PerfScopeAdd(PerfScope::PostStep, tPhys1, tEnd);
        liveSample.tick = tick;
      }
      if (liveTimed) liveTimer.KickDeferred(ctx, liveFrameNo);
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
      // ViewEyePos, not EyePos: the render camera rides the step-smoothing
      // offset so voxel steps glide instead of popping. Everything that can
      // feed the sim (brush/laser/grenade rays, physics) stays on EyePos.
      Vec3 eye = player.ViewEyePos();
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
            for (size_t i = 0; i < hide.size(); i++) hide[i] = 1;
            if (CurrentTuning().avatar.firstPersonArms) {
              // THE WHOLE ARM, not just its ends. The forearms have to be in
              // here explicitly: the rig is armU -> armL -> hand, so keeping
              // only the upper arm and the hand left a floating fist with a
              // gap where the forearm should be — the arm you see in first
              // person is mostly forearm, so it is the one part that cannot be
              // omitted.
              const int keep[9] = {p.armUL, p.armUR, p.armLL, p.armLR,
                                   p.handL, p.handR, p.staff, heldPart, -1};
              for (int k : keep)
                if (k >= 0 && k < (int)hide.size()) hide[k] = 0;
              // ...AND WHATEVER IS WORN OVER THEM. The keep list names BODY
              // parts, so with a robe on, the arms you see in first person
              // would be bare while the sleeves stayed behind with the hidden
              // torso. A shell is kept exactly when the part it covers is —
              // read off the rig's parent link rather than from a second list
              // of sleeve names that would have to be maintained per item.
              for (int i = avatar.AppendedBase(); i < (int)hide.size(); i++) {
                const int par = avatar.PartParent(i);
                if (par >= 0 && par < (int)hide.size() && hide[par] == 0)
                  hide[i] = 0;
              }
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
      const Vec3 earPos = player.ViewEyePos();
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
                           combatStrikeCue.at, combatStrikeCue.power);
        }
        if (combatFleshCue.pending) {
          audioCues.Combat(audio::Cues::CombatCue::Flesh, combatFleshCue.at,
                           combatFleshCue.power);
        }
        if (combatCutCue.pending) {
          audioCues.Combat(audio::Cues::CombatCue::Cut, combatCutCue.at,
                           combatCutCue.power);
        }
        if (combatClangCue.pending) {
          audioCues.Combat(audio::Cues::CombatCue::Clang, combatClangCue.at,
                           combatClangCue.power);
        }

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
      // OUTSIDE the audio block, exactly like the queues above and for the
      // same reason stated there: a request that only clears when audio
      // happens to be on is a stuck flag on a silent machine, and the next
      // frame with audio would play a whoosh for a cut made minutes ago.
      combatWhooshCue.pending = false;
      combatFleshCue.pending = false;
      combatClangCue.pending = false;
      combatStrikeCue.pending = false;
      combatCutCue.pending = false;
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
      float fogTarget = std::clamp(kFogOpticalDepths / far.SafeRadiusMeters(),
                                   kFarFogDensity, kFarFogDensityMax);
      fogSmooth += (fogTarget - fogSmooth) * kFogLerpPerFrame;
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
        WriteRenderParams(ctx.queue, world, eye, jcam,
                          (float)ctx.width / (float)ctx.height, ui.shadows,
                          (float)now, fogSmooth, viewPxThisFrame, tick,
                          fluidCount,
                          (float)(accumulator / kTickDt),
                          ui.showDirtyVoxels ? 2u : 0u);
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
      ui.mobCount = mobs.MobCount();

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
      ui.aiMobIds.clear();
      ui.aiMobLabels.clear();
      for (uint32_t i = 0; i < mobs.MobCount(); i++) {
        const uint64_t mid = mobs.MobIdAt(i);
        if (mid == 0) continue;
        const ai::Brain* br = mobs.MobBrain(mid);
        const ai::Profile* pf =
            br != nullptr ? mobs.Behaviors().At(br->profile) : nullptr;
        char line[192];
        std::snprintf(line, sizeof line, "#%llu  %-16s %-10s %s d=%.1f%s",
                      (unsigned long long)mid,
                      pf != nullptr ? pf->name.c_str() : "(no ai)",
                      br != nullptr ? ai::IntentName(br->intent) : "-",
                      br != nullptr && br->hasTarget
                          ? (br->visible ? "seen" : "lost")
                          : "----",
                      br != nullptr ? br->targetDist : 0.0f,
                      br != nullptr && br->path.valid ? "  [path]" : "");
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
              player.grounded ? 0 : 1, pin.up ? 1 : 0,
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
        if (st.casterId != 0x9134A5EEu) continue;
        std::string line = DescribeCast(glyphs, SpellCast{});
        line.clear();
        const GlyphDef* ig = glyphs.At(st.effect.inner.empty() ? -1 : st.effect.inner[0].glyph);
        const GlyphDef* ag = glyphs.At(st.effect.glyph);
        line = std::string(ig ? ig->id : "mod") + " " + (ag ? ag->id : "aura") +
               (st.target == 0x9134A5EEu ? " on you" : (st.target ? " on them" : " on the place")) +
               "  " + std::to_string(st.perTick) + "/tick, " +
               std::to_string(st.ticksLeft / 30) + " s";
        ui.spellStatuses.push_back(line);
      }
      for (const SpellBeam& bm : spells.Beams())
        if (bm.casterId == 0x9134A5EEu)
          ui.spellStatuses.push_back("beam  " + std::to_string(bm.perTick) + "/tick");
      ui.health = playerHealth.Get();
      ui.healthMax = avatar.HealthMax();
      ui.healthCap = avatar.HealthCap();
      ui.playerAlive = avatar.IsAlive();
      // Body-condition readout: one figure slot per limb, keyed by the limb's
      // authored TAG and side suffix rather than by part name, so any humanoid
      // rig fills the same figure. A limb the rig does not have stays absent
      // and simply is not drawn.
      FillBodyUI(avatar, burnMats, tissueMats, mobs, mats, ui);
      ui.locoState = avatar.Spawned() ? avatar.Locomotion().stateName : "";
      ui.spellCost = caster.compiled.manaCost;
      ui.spellWord = caster.compiled.wordCost;
      ui.spellTariff = caster.compiled.tariff;
      ui.spellCarry = caster.compiled.carryCost;
      ui.spellPriceUnknown = caster.compiled.priceUnknown;
      ui.spellLastBillAge += dt;
      ui.spellText = caster.readout.text;
      ui.spellVerdict = caster.readout.verdict;
      ui.spellOutcome = (int)caster.lastOutcome;
      ui.liveProjectiles = spells.LiveCount();
      // Wind primitives: what is alive and what it costs (§4.3). `windPrims`
      // is the population; `windWakeChunks` is the rule-2 number — the chunks
      // those primitives are holding awake so they can move settled matter.
      ui.windPrims = (int)WindPrims().Count();
      ui.windWakeChunks = (int)WindPrims().LastWakeCount();
      ui.glyphSlots.clear();
      ui.glyphSlotKinds.clear();
      ui.glyphSlotReadouts.clear();
      for (int i = 0; i < kGlyphSlots; i++) {
        const SlotKind k = caster.inventory.KindAt(i);
        ui.glyphSlotKinds.push_back((int)k);
        if (k == SlotKind::Page) {
          const std::string& name = caster.inventory.PageAt(i);
          ui.glyphSlots.push_back(name);
          const GrimoireExpansion ex = ExpandWords(glyphs, caster.grimoire, {name}, kSpellStackMax);
          SpellStack st;
          st.spoken = ex.spoken;
          ui.glyphSlotReadouts.push_back(DescribeSpell(glyphs, CompileSpell(glyphs, st)).text);
          continue;
        }
        int gi = caster.inventory.At(i);
        ui.glyphSlots.push_back(
            gi >= 0 && gi < (int)glyphs.glyphs.size() ? glyphs.glyphs[gi].id : "");
        ui.glyphSlotReadouts.push_back("");
      }
      ui.glyphBankB = captured && ui.magicMode &&
                      (key(GLFW_KEY_LEFT_SHIFT) || key(GLFW_KEY_RIGHT_SHIFT));
      caster.noteAge += dt;
      ui.spellNote = caster.note;
      ui.spellNoteAge = caster.noteAge;
      // hotbar + swing readout (game/item.h, game/melee.h)
      ui.itemNames.clear();
      for (int i = 0; i < kItemSlots; i++) {
        const ItemDef* d = items.At(hotbar.slots[i].Empty() ? -1
                                                            : hotbar.slots[i].def);
        ui.itemNames.push_back(d ? d->name : "");
      }
      ui.itemSelected = hotbar.selected;
      switch (melee.Phase()) {
        case SwingPhase::Idle:    ui.swingPhase = meleeReady ? "ready" : ""; break;
        case SwingPhase::Guard:   ui.swingPhase = "guard"; break;
        case SwingPhase::Wind:    ui.swingPhase = "winding"; break;
        case SwingPhase::Slash:   ui.swingPhase = "SLASH"; break;
        case SwingPhase::Recover: ui.swingPhase = "recover"; break;
      }
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
          if (slot < UIState::kSlotCount && avatar.Def()) {
            // Need a temporary camera at the current orbit to get right/up.
            Camera tmpCam;
            const float fy = std::atan2(-std::cos(avatarHeading),
                                        -std::sin(avatarHeading));
            tmpCam.yaw = fy + ui.portraitYaw;
            tmpCam.pitch = ui.portraitPitch;
            Vec3 bodyLo{1e9f,1e9f,1e9f}, bodyHi{-1e9f,-1e9f,-1e9f};
            Vec3 limbLo{1e9f,1e9f,1e9f}, limbHi{-1e9f,-1e9f,-1e9f};
            bool anyBody = false, anyLimb = false;
            const int lc = (int)avatar.Def()->limbs.size();
            for (int p = 0; p < lc; p++) {
              Vec3 c[8];
              if (!LimbBoxCorners(avatar, phys, p, c)) continue;
              const int s = BodySlotFor(avatar.PartName(p), avatar.PartTag(p));
              for (const Vec3& v : c) {
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
            std::atan2(-std::cos(avatarHeading), -std::sin(avatarHeading));
        portraitCam = MakePortraitCam(avatar, phys, frontYaw + ui.portraitYaw,
                                      ui.portraitPitch,
                                      (float)kPortraitW / (float)kPortraitH,
                                      ui.portraitZoom, ui.portraitPanX,
                                      ui.portraitPanY, ui.portraitPivotSlot);
        ProjectBodyUI(avatar, phys, portraitCam, ui);
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
      // re-found BY ID on every use — a registry pointer does not survive
      // TakeCorpseLoot destroying the piece's body (game/corpses.h).
      {
        auto say = [&](const std::string& m) {
          ui.kitMessage = m;
          ui.kitMessageAge = 0.0f;
        };
        auto closeLoot = [&]() {
          ui.lootOpen = false;
          ui.lootClose = false;
          lootCorpse = 0;
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
        // The screen closed under the panel (I / Esc): the loot goes with it.
        if (ui.lootOpen && !ui.inventoryOpen) {
          ui.lootOpen = false;
          lootCorpse = 0;
          lootOpenedScreen = false;
        }
        if (ui.lootClose) closeLoot();
        // Walked away, or the heap is gone / picked clean: close by itself.
        if (ui.lootOpen) {
          const CorpseReport* c = corpses.Find(lootCorpse);
          if (!c || !CorpseWithin(*c, phys, player.pos, kLootRange))
            closeLoot();
        }
        auto takeOne = [&](int index, KitRef dest) -> bool {
          CorpseReport* c = corpses.Find(lootCorpse);
          if (!c) return false;
          std::string name;
          const LootResult r = TakeCorpseLoot(*c, index, dest, kit, hotbar,
                                              items, debris, &name);
          if (r == LootResult::Ok) {
            say("took " + name);
            return true;
          }
          say(LootResultText(r, dest));
          return false;
        };
        if (ui.moveItem.pending && ui.lootOpen &&
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
                const CorpseReport* c = corpses.Find(lootCorpse);
                if (!c || c->gear.empty()) break;
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
          CorpseReport* c = ui.lootOpen ? corpses.Find(lootCorpse) : nullptr;
          std::string name;
          if (c && ShedCorpseLoot(*c, ui.dropItem.from.index, debris, ground,
                                  &name))
            say("left the " + name + " on the ground");
        }
      }
      if (ui.moveItem.pending) {
        ui.moveItem.pending = false;
        const MoveResult r =
            kit.Move(ui.moveItem.from, ui.moveItem.to, hotbar, items);
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
        const ItemStack* src = kit.Resolve(ui.equipItem.from, hotbar);
        const ItemDef* def = src && !src->Empty() ? items.At(src->def) : nullptr;
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
          const MoveResult r = kit.Move(ui.equipItem.from, to, hotbar, items);
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
      if (ui.dropItem.pending) {
        ui.dropItem.pending = false;
        ItemStack* src = kit.Resolve(ui.dropItem.from, hotbar);
        const ItemDef* def = src && !src->Empty() ? items.At(src->def) : nullptr;
        if (def) {
          // Thrown gently forward from the eye, so it lands in front of you
          // rather than inside your own capsule.
          const Vec3 at = player.EyePos() + cam.Forward() * 2.0f;
          const Vec3 vel = cam.Forward() * 4.0f + player.vel;
          if (DropItemToWorld(*def, at, vel, phys, debris, &mbSet, ground,
                              nullptr, src->dye)) {
            // ONE of the stack. Dropping a count you did not mean to is the
            // mis-click this system's swap-never-overwrite rule exists to
            // prevent, and it applies here too.
            if (--src->count <= 0) *src = ItemStack{};
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
          } else if (starter || glyphs.Find(name) >= 0) {
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
      ui.kitMessageAge += dt;

      // ---- the mirrors -------------------------------------------------------
      // Rebuilt every frame from the real containers. Cheap (a few dozen
      // string copies) and it is the reason the panel can never show something
      // the game does not have.
      {
        // CONDITION COMES FROM WHEREVER THE PIECE ACTUALLY IS. On the body the
        // shells are the truth and the blob in `kit.wornDamage` is stale (it is
        // only written when a piece comes OFF); in the pack there are no shells
        // and the blob is all there is. Asking the wrong one is not a rounding
        // error — it is a robe that reads 100% while it burns off your back.
        const float ruinedAt = CurrentTuning().gear.ruinedCondition;
        auto conditionOf = [&](const ItemDef* d) {
          if (!d || !ItemKindIsWorn(d->kind)) return 1.0f;
          for (int s = 0; s < kEquipSlotCount; s++)
            if (avatar.Spawned() && avatar.WornItem(s) == d->name)
              return avatar.WornCondition(s);
          const WornDamage* w = kit.Damage(d->name);
          return w ? w->Condition() : 1.0f;
        };
        auto mirror = [&](const ItemStack& st) {
          UIState::KitSlotUI u;
          const ItemDef* d = items.At(st.Empty() ? -1 : st.def);
          if (!d) return u;
          u.name = d->name;
          u.count = st.count;
          u.wearable = ItemKindIsWorn(d->kind);
          if (u.wearable) {
            u.condition = conditionOf(d);
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
          }
          return u;
        };
        ui.hotbarSlots.clear();
        for (int i = 0; i < kItemSlots; i++)
          ui.hotbarSlots.push_back(mirror(hotbar.slots[i]));
        ui.bagSlots.clear();
        for (int i = 0; i < Bag::kSlots; i++)
          ui.bagSlots.push_back(mirror(kit.bag.slots[i]));
        ui.equipSlots.clear();
        for (int i = 0; i < kEquipSlotCount; i++)
          ui.equipSlots.push_back(mirror(kit.equip.slots[i]));
        // The corpse's gear, through the same mirror so a robe on a corpse is
        // drawn and tipped exactly as one in the pack — with its condition
        // read off the death-time capture, the only record there is for a
        // piece nobody is wearing.
        ui.lootSlots.clear();
        if (ui.lootOpen) {
          if (const CorpseReport* c = corpses.Find(lootCorpse)) {
            ui.lootTitle = c->def;
            for (const CorpseReport::Piece& pc : c->gear) {
              const int di = items.Find(pc.item);
              UIState::KitSlotUI u =
                  mirror(ItemStack{di, di >= 0 ? 1 : 0, pc.dye});
              if (u.name.empty()) u.name = pc.item;   // gone from the library
              if (u.wearable) {
                u.condition = pc.damage.Condition();
                u.ruined = GearRuined(u.condition, ruinedAt);
              }
              ui.lootSlots.push_back(std::move(u));
            }
          }
        }

        ui.glyphsOwned.clear();
        for (int gi = 0; gi < (int)glyphs.glyphs.size(); gi++) {
          const GlyphDef& g = glyphs.glyphs[gi];
          UIState::GlyphUI u;
          u.id = g.id;
          u.desc = g.desc;
          u.type = (int)g.sort;
          u.mana = g.word;
          u.owned = caster.inventory.Owns(gi);
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

        // The grimoire: the authored starters (read-only) and the player's
        // pages, each with the readout and price of its expansion, through
        // the same DescribeSpell the live sentence uses.
        ui.grimoirePages.clear();
        ui.grimoireMaxPages = glyphs.budgets.maxGrimoirePages;
        ui.grimoireMaxWords = glyphs.budgets.maxMacroWords;
        auto describeWords = [&](const std::vector<std::string>& words, std::string& readout,
                                 int32_t& price, bool& unknown, int& dropped) {
          const GrimoireExpansion ex = ExpandWords(glyphs, caster.grimoire, words, kSpellStackMax);
          SpellStack st;
          st.spoken = ex.spoken;
          const CastList l = CompileSpell(glyphs, st);
          readout = DescribeSpell(glyphs, l).text;
          if (ex.dropped > 0) readout += "   (? = a word that no longer exists)";
          if (ex.truncated) readout += "   (cut at the stack bound)";
          price = l.manaCost;
          unknown = l.priceUnknown;
          dropped = ex.dropped;
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
      }

      overlay.BeginFrame();
      // HUD first, dev panel second: the panel is a real ImGui window and gets
      // to sit on top of the chrome, not the other way round.
      //
      // The HUD is SUPPRESSED while the character screen is open: the screen
      // already shows both pools and the body condition, larger and with the
      // numbers spelled out, so drawing the corner chrome underneath it is two
      // readouts of the same thing fighting for the same corner.
      if (!ui.inventoryOpen) overlay.DrawHUD(ui);
      overlay.Draw(ui);

      // Wind force multipliers. Its OWN latch, and deliberately not folded
      // into fluidTuningDirty below: that path ends in sim.ReloadShaders(),
      // and these two knobs ride TickParams precisely so they do not need one.
      // A slider that recompiled every shader on each frame of a drag would be
      // unusable, which is the whole reason they are on the tick stream and
      // not in tuning_params.def.
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
          for (int k = 0; k < (int)ai::Intent::Count && k < 6; k++) {
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
          for (int k = 0; k < (int)ai::Intent::Count && k < 6; k++) {
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
        t.sim.fluidStainRate        = ui.fStainRate;
        std::memcpy(t.render.fluidColor,  ui.fColor,  sizeof(ui.fColor));
        std::memcpy(t.render.fluidColor1, ui.fColor1, sizeof(ui.fColor1));
        std::memcpy(t.render.fluidColor2, ui.fColor2, sizeof(ui.fColor2));
        std::memcpy(t.render.fluidColor3, ui.fColor3, sizeof(ui.fColor3));
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
      if (laserHeld && world.Snap().valid && world.Snap().pick[0] != 0) {
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
      uint32_t debugBoxCount = 0;
      if (!dbg.empty()) {
        debugBoxCount = (uint32_t)dbg.size();
        ctx.queue.WriteBuffer(world.debugBoxes, 0, dbg.data(),
                              dbg.size() * sizeof(DebugBox));
      }

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
      // WHO ELSE IS HOLDING MY ARM. One index sweep while everything is
      // healthy, a named report the moment two entities point at one brick
      // record or a holder is left pointing at a freed one — the owner-visible
      // symptom of either is a limb wearing another creature's shape.
      // Rate-limited to one line per distinct fault and mirrored to
      // build/microbody_audit.log, because this fires while somebody is
      // playing and a windowed session's stderr goes nowhere.
      bodyReg.AuditMicroModels();
      if (bodyReg.AnyInstancesDirty()) {
        std::vector<BodyVoxInst> inst;
        bodyReg.BuildInstances(inst);
        bodyInstCount = (uint32_t)inst.size();
        if (!inst.empty())
          ctx.queue.WriteBuffer(world.bodyInstances, 0, inst.data(),
                                inst.size() * sizeof(BodyVoxInst));
      }
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
      // No DrawWorld: the clear colour IS the backdrop, and a raymarch of the
      // whole residency window to fill 320x448 pixels behind a character is
      // the most expensive possible way to draw a background.
      if (ui.inventoryOpen && portraitCam.valid && portraitView) {
        avatar.SetHiddenParts({});               // the WHOLE body, always
        std::vector<BodyVoxInst> pInst;
        bodyReg.BuildInstances(pInst);
        if (!pInst.empty())
          ctx.queue.WriteBuffer(world.bodyInstances, 0, pInst.data(),
                                pInst.size() * sizeof(BodyVoxInst));
        // The TRANSFORMS need no re-upload: a hidden limb still consumes its
        // slot (game/mob.cpp's walk advances for every part with a body,
        // drawn or not), so the transform array is mask-independent by
        // construction. Only the instance lists change.
        microInsts.clear();
        bodyReg.BuildMicroInsts(microInsts);
        const uint32_t pMicro = sim.UploadMicroBodyInsts(ctx.queue, microInsts);

        WriteRenderParams(ctx.queue, world, portraitCam.eye, portraitCam.cam,
                          portraitCam.aspect, /*shadows=*/true, (float)now,
                          /*fogDensity=*/0.0f, (float)kPortraitH,
                          kPortraitLightTick);
        rhi::CommandEncoder pEnc = ctx.device.CreateCommandEncoder();
        // Near-black indigo, the panel's own deepest tone, so the portrait
        // reads as an inset rather than as a hole punched in the sheet.
        const float kPortraitClear[4] = {0.055f, 0.048f, 0.086f, 1.0f};
        rhi::RenderPass pRp =
            sim.BeginAuxRenderPass(pEnc, portraitView, ctx.surfaceFormat,
                                   kPortraitW, kPortraitH, kPortraitClear);
        sim.DrawBodies(pRp, (uint32_t)pInst.size());
        sim.DrawMicroBodies(pRp, pMicro);
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
                                frameCounter == kShotInvLootFrame))
          std::printf("--shot-inventory: portrait cube=%zu micro=%u "
                      "eye=(%.1f %.1f %.1f) target=(%.1f %.1f %.1f)\n",
                      pInst.size(), pMicro, portraitCam.eye.x, portraitCam.eye.y,
                      portraitCam.eye.z, portraitCam.target.x,
                      portraitCam.target.y, portraitCam.target.z);
        ctx.queue.Submit(pEnc.Finish());

        // Put the world back: the real hide mask, its instances, and the
        // player's own camera. The mask change re-dirties the avatar, so the
        // rebuild below is a genuine requirement rather than a precaution.
        avatar.SetHiddenParts(hide);
        std::vector<BodyVoxInst> mInst;
        bodyReg.BuildInstances(mInst);
        bodyInstCount = (uint32_t)mInst.size();
        if (!mInst.empty())
          ctx.queue.WriteBuffer(world.bodyInstances, 0, mInst.data(),
                                mInst.size() * sizeof(BodyVoxInst));
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
        sim.DrawBodies(rp, bodyInstCount);
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
      if ((g_shotInventory && (frameCounter == kShotInvGearFrame ||
                               frameCounter == kShotInvCaptureFrame ||
                               frameCounter == kShotInvGrimoireFrame ||
                               frameCounter == kShotInvLootFrame)) ||
          g_shotJumpPath) {
        const char* shotPath =
            g_shotJumpPath                       ? g_shotJumpPath
            : frameCounter == kShotInvGearFrame  ? "screenshot_inventory.bmp"
            : frameCounter == kShotInvCaptureFrame
                ? "screenshot_inventory_health.bmp"
            : frameCounter == kShotInvGrimoireFrame
                ? "screenshot_inventory_grimoire.bmp"
                : "screenshot_inventory_loot.bmp";
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
        sim.DrawBodies(srp, bodyInstCount);
        sim.DrawMicroBodies(srp, microCount);
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
