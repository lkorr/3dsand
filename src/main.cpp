// sandvox — 3D falling-sand voxel engine (v0). See DESIGN.md.
// Fixed 30 Hz GPU simulation, uncapped raymarched rendering, walkable player,
// JSON materials, deterministic kernels with per-tick world hash.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <GLFW/glfw3.h>

#include "audio/cues.h"
#include "game/avatar.h"
#include "game/bodyreg.h"
#include "game/brush.h"
#include "game/persist.h"
#include "game/camera.h"
#include "game/caster.h"
#include "game/item.h"
#include "game/melee.h"
#include "game/mob.h"
#include "game/spell.h"
#include "game/player.h"
#include "game/prefab.h"
#include "game/thirdperson.h"
#include "gpu/context.h"
#include "gpu/resources.h"
#include "gpu/vk_info.h"
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
#include "sim/wind.h"
#include "sim/windprim.h"
#include "sim/world.h"
#include "sim/worldio.h"
#include "telemetry.h"
#include "test/selftest.h"
#include "measure/measure.h"
#include "test/support.h"
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


// --frames N (phase 4b D3): windowed verification harness. 0 = play normally.
uint64_t g_harnessFrames = 0;
bool g_autofly = false;
bool g_autoflyHard = false;  // --autofly-hard: adversarial traversal for pool sizing
// --autofly-surface: the RENDERER's adversarial traversal, the complement of
// --autofly-hard. Where the hard descent drives the window into solid bulk to
// stress residency, this one flies OVER the terrain to stress ray length: the
// surface case is where the frame cost lives (docs/PLAN_surface_flight_perf.md
// Part A), and it is exactly the case the descent cannot reach.
bool g_autoflySurface = false;
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
constexpr uint32_t kParkFlyTicks = 300;    // fly this far out, then stop
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
  for (uint32_t i = 0; i < kNumChunks; i++) {
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
    for (uint32_t i = 0; i < kNumChunks; i++) {
      const bool act = s.dirtyFlags[i] != 0;
      if (act != wantActive) continue;
      // Control arm is non-empty chunks only - comparing against sky would
      // make every material look enriched.
      if (!act && s.occupancy[i] == 0) continue;
      pool++;
    }
    const uint32_t stride = pool > kArm ? pool / kArm : 1u;
    uint32_t seenN = 0;
    for (uint32_t i = 0; i < kNumChunks && arm.size() < kArm; i++) {
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
  if (tick == kParkFlyTicks - 120u) ParkSampleRequest(world, "fly");
  if (tick == kParkFlyTicks - 80u) ParkSampleReport(world, mats, "fly");
  const uint32_t fetchTick = kParkFlyTicks + ParkSettleTicks();
  if (tick >= kParkFlyTicks && (tick % 25u) == 0u)
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

void FillBodyUI(const PlayerAvatar& avatar, UIState& ui) {
  for (int i = 0; i < UIState::kSlotCount; i++) ui.body[i] = {};
  const MobDef* def = avatar.Def();
  ui.bodyValid = def != nullptr;
  if (!def) return;
  // Walk the DEF's limbs, not PartCount() — the latter includes the borrowed
  // held-item slot, which is not part of the body.
  const int limbCount = (int)def->limbs.size();
  for (int i = 0; i < limbCount; i++) {
    const int slot = BodySlotFor(avatar.PartName(i), avatar.PartTag(i));
    if (slot < 0) continue;
    UIState::BodyPartUI& b = ui.body[slot];
    b.present = true;
    if (!avatar.PartAlive(i)) {
      b.severed = true;
      b.hpFrac = 0.0f;
      continue;  // a lost limb neither bleeds nor reports damage
    }
    const float max = avatar.PartHpMax(i);
    const float hp = avatar.PartHp(i);
    b.hpFrac = max > 0.0f ? hp / max : 1.0f;
    if (b.hpFrac < 0.0f) b.hpFrac = 0.0f;
    if (b.hpFrac > 1.0f) b.hpFrac = 1.0f;
    b.bleeding = avatar.PartBleeding(i);
  }
}

// --shot: minimal look-iteration harness. Worldgen, drain the far-field fill
// queue, settle briefly, write the three standard screenshots, exit — so
// render/look changes can be judged in seconds instead of the full selftest.
// Cameras deliberately match the selftest's so the two stay comparable.
int RunShots(GpuContext& ctx, World& world, Simulation& sim) {
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
    if (got && WriteBmpFile(path, pixels, W, H)) std::printf("wrote %s\n", path);
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
  auto render = [&](Vec3 eye, float yaw, float pitch, const char* path) {
    Camera c;
    c.yaw = yaw;
    c.pitch = pitch;
    WriteRenderParams(ctx.queue, world, eye, c, (float)W / H, true, kShotTime,
                      kFarFogDensity, 1080.0f, shotTick);
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
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
    rp.End();
    ctx.queue.Submit(enc.Finish());
    ctx.WaitIdle();
    grab(path);
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
  // Combat arena POI, centered at (180,110): from outside the -x/-z corner
  // looking across the deck, high enough to see the far wall and both doorways.
  {
    int ah = World::TerrainHeight(180, 110, kDefaultSeed);
    render({120, (float)(ah + 40), 50}, 0.9f, -0.32f, "screenshot_arena.bmp");
    render({180, (float)(ah + 90), 40}, 1.5708f, -0.85f, "screenshot_arena_top.bmp");
  }
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
  render({372, 80, 372}, 0.785f, -0.30f, "screenshot_water.bmp");
  // Standing over the middle looking down: the low-Fresnel angle, where
  // refraction, per-channel depth absorption and the visible bed carry it.
  render({420, 88, 452}, -1.571f, -0.75f, "screenshot_water_down.bmp");
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

    world.SetWindowOrigin({0, 0, 0});
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
        if (roll < 55u) { mat = kMatGrassTuft; }
        else if (roll < 62u) { mat = kMatFlowerPoppy; }
        else if (roll < 68u) { mat = kMatFlowerDaisy; }
        else if (roll < 72u) { mat = kMatFoliageBush; }
        else { continue; }  // bare ground between the tufts
        // Same word rules as a brush paint on a solid: state 0, unstamped.
        flora.push_back({World::SlotCellIndex(c), PackVoxNew(mat, 0u)});
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
    render({(float)(gx - 16), (float)(mh + 7), (float)(gz - 16)}, 0.785f, -0.32f,
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

// --shot-mob <def>[:limb,limb,...] — the mob counterpart of --shot: worldgen,
// spawn the named def, sever the listed limbs, run real ticks until the
// locomotion state settles, then write close-up screenshots from three angles.
// Exists because mob poses (gait, crawl clips, dismemberment states) can
// otherwise only be judged in a live session — this makes "what does the
// legless crawl actually look like" a ten-second question.
int RunMobShot(GpuContext& ctx, World& world, Simulation& sim, Physics& phys,
               DebrisSystem& debris, MobSystem& mobs, const std::string& spec) {
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
    mobs.PreTick(t + 1, world, ops, spawns);
    debris.QueueSupportEvents(world.Snap());
    std::vector<CellOp> cellOps;
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

}  // namespace

int main(int argc, char** argv) {
  InstallCrashHandler();

  bool selftest = false;
  bool shot = false;
  bool measure = false;  // --measure: Vulkan-port sizing harness (headless)
  bool rebaseline = false;  // --rebaseline: write observed values into baseline.json
  bool suiteAcceptance = false;  // --suite acceptance: one-process full acceptance
  std::string sweepParam;   // --sweep sim.X=a,b,c
  std::string sweepGate;    // --sweep-gate (default: determinism)
  // PAGED IS THE DEFAULT (2026-08-23, user decision): 4,975 resident pages =
  // 77.7 MiB against 512 MiB dense, both residency suites green at the phase-7
  // close. `--residency dense` stays available as the identity map and the
  // only live differential oracle — if paged ever misbehaves, the first
  // diagnostic step is the same scenario under dense.
  bool residencyPaged = true;  // --residency paged|dense
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
  std::string shotMob;  // --shot-mob <def>[:limb,...] (mob pose look iteration)
  bool shotFluid = false;  // --shot-fluid (MPM water look iteration)
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
          "  --sweep sim.X=a,b,c   In-process parameter sweep (hash per value)\n"
          "  --sweep-gate <name>   Gate for --sweep (default: determinism)\n\n"
          "Shot / screenshot modes:\n"
          "  --shot                Screenshot-only look iteration\n"
          "  --shot-fluid          MPM fluid screenshot mode\n"
          "  --shot-fluid-pond     MPM fluid poured into a generated pond\n"
          "                        (the MPM/settled-water seam)\n"
          "  --shot-mob <def>      Mob pose look iteration (def[:limb,...])\n"
          "  --time <0..1>         Time of day for --shot (0=midnight, 0.5=noon)\n\n"
          "Fluid lab:\n"
          "  --lab [scene]         Windowed fluid lab (basin|hill|faucet|pool|slosh|pond|worldlake)\n"
          "  --fluid-bench [scene] Headless fluid timing harness (scene|pond<N>|pours|all)\n\n"
          "Harness / perf:\n"
          "  --frames <N>          Run windowed game for N frames then exit\n"
          "  --autofly             Enable autofly camera\n"
          "  --autofly-hard        Adversarial autofly (diagonal + descent)\n"
          "  --autofly-surface     Surface-following autofly\n"
          "  --autofly-park        Surface autofly that stops (sleep discriminator)\n"
          "  --measure             Vulkan sizing harness (occupancy + GPU timings)\n\n"
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
    else if (a == "--autofly") g_autofly = true;
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
    else if (a == "--shot-mob") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--shot-mob requires a mob def\n"); return 1; }
      shotMob = argv[++i];
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
      GpuContext stCtx;
      if (!stCtx.Init(nullptr, 1600, 900, lowPowerAdapter, false, backend,
                      vkValidation, sledgehammer))
        return 1;
      World stWorld;
      stWorld.residency = World::Residency::Paged;
      stWorld.Init(stCtx.device);
      Simulation stSim;
      if (!stSim.Init(stCtx.device, stWorld, m, rx, mic, ad + "/shaders"))
        return 1;
      Physics stPhys; stPhys.Init();
      DebrisSystem stDebris; stDebris.Init(&stPhys, &stWorld, m, rx);
      MobSystem stMobs; stMobs.Init(&stPhys, &stWorld, &stDebris, m);
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
        Stream stStream;
        stStream.Init(&stCtx, &stWorld, &stSim, kDefaultSeed);
        stStream.OnMaterialsReloaded(m);
        selftest::Options so;
        if (rebaseline) so.rebaseline = true;
        selftest::Ctx sc{stCtx, stWorld, stSim, m, stPhys, stDebris, stMobs, stStream, stItems};
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
    std::printf("loaded %zu glyphs (%zu conjoined, %zu wards)\n",
                glyphs.glyphs.size(), glyphs.conjoined.size(),
                glyphs.wards.size());
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

  GLFWwindow* window = nullptr;
  if (!selftest && !shot && !measure && !fluidBench && shotMob.empty()) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window = glfwCreateWindow(1600, 900, "sandvox", nullptr, nullptr);
    if (!window) return 1;
  }

  GpuContext ctx;
  // Timestamps: --measure and --fluid-bench are the only modes that request
  // the TimestampQuery device feature (per-pass GPU timings).
  if (!ctx.Init(window, 1600, 900, lowPowerAdapter,
                /*wantTimestamps=*/measure || fluidBench, backend, vkValidation,
                sledgehammer))
    return 1;

  Telemetry telemetry;
  if (telemetryEnabled) telemetry.Start(telemetryPort);

  World world;
  world.residency =
      residencyPaged ? World::Residency::Paged : World::Residency::Dense;
  world.Init(ctx.device);
  Simulation sim;
  if (!sim.Init(ctx.device, world, mats, reactions, micro, assetDir + "/shaders"))
    return 1;

  Physics phys;
  if (!phys.Init()) return 1;
  DebrisSystem debris;
  debris.Init(&phys, &world, mats, reactions);
  MobSystem mobs;
  mobs.Init(&phys, &world, &debris, mats);
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
    sim.UploadMicroBodies(ctx.queue, mbSet);
    mobs.SetDefs(std::move(mobDefs));
  }
  Stream stream;
  stream.Init(&ctx, &world, &sim, kDefaultSeed);
  stream.OnMaterialsReloaded(mats);
  FarField far;
  far.Init(&world);

  if (measure) return RunMeasure(ctx, world, sim, mats);
  if (shot) return RunShots(ctx, world, sim);
  if (shotFluid || shotFluidPond)
    return RunFluidShot(ctx, world, sim, mats, shotFluidPond);
  if (fluidBench)
    return RunFluidBench(ctx, world, sim, mats, fluidBenchScene,
                         stOpt.jsonPath);
  if (!shotMob.empty())
    return RunMobShot(ctx, world, sim, phys, debris, mobs, shotMob);
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
    if (!SetSimField(baseTuning, field, vals[0])) {
      std::fprintf(stderr, "--sweep: unknown sim field '%s'\n", field.c_str());
      return 1;
    }

    std::string gate = sweepGate.empty() ? "determinism" : sweepGate;
    constexpr int kSweepTicks = 100;
    std::printf("=== sweep sim.%s over %zu values, gate %s, %d ticks ===\n",
                field.c_str(), vals.size(), gate.c_str(), kSweepTicks);

    SetHarnessSnapshotDrain(true);
    std::vector<uint32_t> hashes;
    for (size_t vi = 0; vi < vals.size(); vi++) {
      Tuning t = baseTuning;
      SetSimField(t, field, vals[vi]);
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
      std::printf("  sim.%s = %.4g  →  hash %08x\n", field.c_str(), vals[vi], h);
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
    selftest::Ctx sc{ctx, world, sim, mats, phys, debris, mobs, stream, items};
    return selftest::Run(sc, stOpt);
  }

  Overlay overlay;
  if (!overlay.Init(window, ctx.device, ctx.surfaceFormat)) return 1;

  // Audio comes up HERE, after the three headless modes have returned: none of
  // --shot/--shot-mob/--selftest should ever open a sound device (there is no
  // audio hardware in CI, and a selftest that depends on one is not a test).
  // A failed init is not an error anywhere — the game runs silent.
  audio::Cues audioCues;
  if (!noAudio) audioCues.Init(assetDir + "/sounds", mats);

  UIState ui;
  // The wind overlay's authored initial state (F4 toggles from here). Seeded
  // rather than defaulted so a tuning.json that asks for the arrows gets them
  // without a keypress — which is what makes the overlay reachable from a
  // headless run.
  ui.showWindField = CurrentTuning().wind.dbgWindField;
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

  // Material COLLISION class LUT for the player's mirror queries. Not raw
  // klass: BuildCollisionClasses remaps passable vegetation to gas so the
  // capsule sweep moves through reeds and kelp (sim/materials.h).
  std::vector<uint32_t> classOf = BuildCollisionClasses(mats);

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);

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
  avatar.Init(&phys, &world, &debris, mats);
  avatar.SetDefs(&mobs.Defs(), avatarDefName);
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
  int spawnH = World::TerrainHeight(140, 140, kDefaultSeed);
  player.pos = Vec3{140, (float)(spawnH + 10), 140};
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
  // kinematic capsule proxy so debris collides with (and is shoved by) the
  // player; terrain collision stays in the AABB controller
  uint64_t playerBody = phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);

  bool captured = true;
  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  double mx0 = 0, my0 = 0;
  glfwGetCursorPos(window, &mx0, &my0);
  // Look sensitivity scale while a melee weapon is up, eased rather than
  // switched (camera.meleeSensHalflife). See the note at the ApplyMouse call.
  float lookSensNow = 1.0f;

  KeyEdge eP, eN, eV, eF1, eF3, eF4, eF5, eF9, eF10, eR, eEsc, eLBracket, eRBracket, eJump,
      eG, eX, eB, eT, eO, eM, eK, eTab, eC, eH, eZ, eBack, eU, eL;
  KeyEdge eGlyph[kGlyphSlots];
  bool prevMouseL = false;
  bool prevMouseR = false;
  // RMB cast, latched until a tick actually runs (see the cast site below).
  bool castQueued = false;
  std::vector<Grenade> grenades;

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
  Inventory hotbar;
  {
    // Placeholder acquisition, mirroring GrantAllAndBind: you start with one
    // of everything the library defines. A real pickup loop replaces this.
    for (int i = 0; i < (int)items.items.size(); i++) hotbar.Add(i, 1);
  }
  // The blade's position last tick, so the sweep has something to sweep FROM.
  // Invalid until the first tick with a weapon drawn — a swing that started
  // from an unknown pose would carve a segment the blade never travelled.
  Vec3 lastEdgeBase{}, lastEdgeTip{};
  bool lastEdgeValid = false;

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
  while (!glfwWindowShouldClose(window)) {
    if (g_harnessFrames > 0) {
      frameCounter++;
      if (frameCounter == g_harnessFrames / 2) {
        std::printf("--frames harness: triggering shader reload (F5 path)\n");
        ui.reloadShaders = true;
      }
      if (frameCounter >= g_harnessFrames) glfwSetWindowShouldClose(window, 1);
    }
    // The park probe is tick-scheduled, so it decides its own end: --frames
    // only has to be generous enough to reach it.
    if (g_parkDone) glfwSetWindowShouldClose(window, 1);
    glfwPollEvents();
    double now = NowSeconds();
    float dt = (float)(now - lastTime);
    lastTime = now;
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
    if (eEsc.Pressed(key(GLFW_KEY_ESCAPE))) {
      captured = !captured;
      glfwSetInputMode(window, GLFW_CURSOR,
                       captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
      glfwGetCursorPos(window, &mx0, &my0);
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
      const bool bladeUp = melee.Phase() != SwingPhase::Idle;
      const float want = bladeUp ? ct.meleeSensitivity : 1.0f;
      const float hl = ct.meleeSensHalflife;
      const float k = hl > 1e-4f ? 1.0f - std::pow(0.5f, dt / hl) : 1.0f;
      lookSensNow += (want - lookSensNow) * k;
    }
    if (captured)
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
    if (captured) melee.AddMouse((float)(mx - mx0), (float)(my - my0));
    mx0 = mx;
    my0 = my;

    if (eP.Pressed(key(GLFW_KEY_P))) ui.paused = !ui.paused;
    if (eN.Pressed(key(GLFW_KEY_N))) ui.stepOnce = true;
    if (eV.Pressed(key(GLFW_KEY_V))) ui.fly = !ui.fly;
    if (eF1.Pressed(key(GLFW_KEY_F1))) ui.visible = !ui.visible;
    if (eF3.Pressed(key(GLFW_KEY_F3)))
      ui.showCollisionBoxes = !ui.showCollisionBoxes;
    // F4: the wind slope-field arrows (docs/RESEARCH_wind.md §4.8). Beside F3
    // because the two are the same kind of thing — a debug view of something
    // the world is doing invisibly — and free when off either way.
    if (eF4.Pressed(key(GLFW_KEY_F4)))
      ui.showWindField = !ui.showWindField;
    if (eF5.Pressed(key(GLFW_KEY_F5))) ui.reloadShaders = true;
    if (eF9.Pressed(key(GLFW_KEY_F9))) ui.saveWorld = true;
    if (eF10.Pressed(key(GLFW_KEY_F10))) ui.loadWorld = true;
    if (eR.Pressed(key(GLFW_KEY_R))) ui.reloadMaterials = true;
    if (eLBracket.Pressed(key(GLFW_KEY_LEFT_BRACKET)))
      ui.brushRadius = std::max(1, ui.brushRadius - 1);
    if (eRBracket.Pressed(key(GLFW_KEY_RIGHT_BRACKET)))
      ui.brushRadius = std::min(7, ui.brushRadius + 1);
    // The number row is SHARED: it picks a brush material normally and SPEAKS
    // glyphs in magic mode (Z). Both wanted 1-8 and the brush binding predates
    // magic, so a mode toggle is what keeps the existing tool usable rather
    // than silently stealing its keys.
    if (!ui.magicMode) {
      for (int i = 0; i < 8; i++)
        if (key(GLFW_KEY_1 + i) && i + 1 < (int)mats.size()) {
          if (ui.tool == UIState::kToolFluid)
            ui.fluidSpecies = i & 3;
          else
            ui.brushMaterial = i + 1;
        }
    } else {
      // Pressing a number SPEAKS that glyph — it never casts. Edge-triggered:
      // a held key must not stutter the same word onto the stack.
      for (int i = 0; i < kGlyphSlots; i++) {
        // GLFW's number row is contiguous 1..9 then 0, and slot 10 is the 0
        // key, matching the strip the HUD prints.
        int k = (i == 9) ? GLFW_KEY_0 : (GLFW_KEY_1 + i);
        if (captured && eGlyph[i].Pressed(key(k)))
          caster.SpeakSlot(glyphs, i);
      }
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
    if (ui.tool == UIState::kToolPrefab && eT.Pressed(key(GLFW_KEY_T)))
      ui.prefabRot = (ui.prefabRot + 1) & 3;
    if (ui.tool == UIState::kToolPrefab && eO.Pressed(key(GLFW_KEY_O)) &&
        !prefabs.empty())
      ui.prefabSelected = (ui.prefabSelected + 1) % (int)prefabs.size();

    PlayerInput pin;
    pin.forward = (key(GLFW_KEY_W) ? 1.f : 0.f) - (key(GLFW_KEY_S) ? 1.f : 0.f);
    pin.strafe = (key(GLFW_KEY_D) ? 1.f : 0.f) - (key(GLFW_KEY_A) ? 1.f : 0.f);
    pin.up = key(GLFW_KEY_SPACE);
    pin.down = key(GLFW_KEY_LEFT_CONTROL);
    pin.sprint = key(GLFW_KEY_LEFT_SHIFT);
    pin.jumpPressed = eJump.Pressed(key(GLFW_KEY_SPACE));
    // --autofly: hold W+sprint in fly mode, no human at the keyboard. Exists to
    // reproduce the streaming-shift stutter, which only appears when the window
    // origin moves several chunks per second.
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
        if (g_autoflyPark && tick >= kParkFlyTicks) {
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
        // The wind overlay is reachable two ways and F5 is where they meet:
        // the tuning bool is the authored state and F4 toggles from there, so
        // reloading re-seeds the toggle rather than leaving the key and the
        // file quietly disagreeing about whether the arrows are on.
        ui.showWindField = CurrentTuning().wind.dbgWindField;
        // Gore variance is drawn per mob at spawn, so mobs already standing in
        // the world hold profiles from the OLD tuning. Re-draw them here or an
        // edit to the randomness controls appears to do nothing until the next
        // spawn. Same id -> same draw, so a mob keeps its identity unless the
        // variance settings themselves changed.
        mobs.RefreshGoreProfiles();
        // The weather switches decide which reaction rules COMPILE, and the
        // reaction table is built by LoadAssets — which F5 does not otherwise
        // run. Fall through into the materials reload so a freeze/melt
        // checkbox applies on the same keypress as everything else in
        // tuning.json. The materials block is the next statement, so this
        // lands in the right order: tuning is live before LoadAssets reads it.
        ui.reloadMaterials = true;
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
          if (LoadGlyphs(assetDir + "/spells/glyphs.json", mats, next, gerr)) {
            glyphs = std::move(next);
            spells.Clear();          // live projectiles hold stale glyph indices
            caster.inventory.GrantAllAndBind(glyphs);
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
        mobs.Reset();
        mobs.OnMaterialsReloaded(mats);
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
        // a freed model. The hotbar stores indices into `items`, so it is
        // re-validated below once the new library exists.
        {
          std::string ierr;
          LoadItems(assetDir + "/items", mats.size(), mbSet, items, ierr);
          if (!ierr.empty()) std::fprintf(stderr, "%s", ierr.c_str());
        }
        sim.UploadMicroBodies(ctx.queue, mbSet);
        mobs.SetDefs(std::move(mobDefs));
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
      stream.OnRegen();
      world.SetWindowOrigin({0, 0, 0});
      SubmitWorldgen(ctx, world, sim, kDefaultSeed);
      player.pos = Vec3{140, (float)(spawnH + 10), 140};
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
      EntityIO eio = MakeEntityIO(debris, mobs, &avatar);
      SaveWorld(ctx, world, stream, "world.svd", mats, &eio);
    }
    if (ui.loadWorld) {
      ui.loadWorld = false;
      ctx.WaitIdle();
      EntityIO eio = MakeEntityIO(debris, mobs, &avatar);
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
    player.Update(dt, pin, cam.FlatForward(), cam.Right(), cam.Forward(), kindAt);
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
      if (g_autoflyPark && tick >= kParkFlyTicks) {
        if (!g_parkPosSet) {
          g_parkPos = player.pos;
          g_parkPosSet = true;
          std::printf("park: stopped at (%.1f, %.1f, %.1f) on tick %u\n",
                      g_parkPos.x, g_parkPos.y, g_parkPos.z, tick);
        }
        player.pos = g_parkPos;
        player.vel = Vec3{0, 0, 0};
      }
    }
    ui.fly = player.fly;

    // ---- fixed-tick simulation ----
    accumulator += dt;
    // Cap the tick backlog: with no cap, any stretch where 30 Hz can't be met
    // (heavy fire, worldgen, a save) accrues unbounded debt and the loop runs
    // 4 ticks/frame long after the load has passed. Drop the excess instead.
    if (accumulator > 4 * kTickDt) accumulator = 4 * kTickDt;
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
    const ItemDef* heldItem = items.At(hotbar.HeldDef());
    const bool meleeArmed = ui.tool == UIState::kToolMelee && !ui.magicMode &&
                            heldItem && heldItem->kind == ItemKind::Melee;
    const bool meleeHeld = meleeArmed && mouseL;
    // Scroll and the number row pick a hotbar slot while the melee tool is up,
    // which is the one context where the number row is otherwise unclaimed
    // (the brush owns it normally, glyphs own it in magic mode).
    if (ui.tool == UIState::kToolMelee && !ui.magicMode) {
      for (int i = 0; i < kItemSlots; i++) {
        int k = (i == 9) ? GLFW_KEY_0 : (GLFW_KEY_1 + i);
        if (captured && eGlyph[i].Pressed(key(k))) hotbar.Select(i);
      }
    }
    while (accumulator >= kTickDt && ticksThisFrame < 4) {
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
      ctx.ProcessEvents();
      if (g_autoflyPark) ParkProbe(world, mats, tick);

      // recenter the residency window on the player (between ticks only; at
      // most one 1-chunk shift per axis)
      IVec3 playerChunkNow{ifloor(player.pos.x) >> 4, ifloor(player.pos.y) >> 4,
                           ifloor(player.pos.z) >> 4};
      stream.Update(playerChunkNow, tick);
      // far-field cascades track the player the same way (render-only)
      far.Update(playerChunkNow);
      uint32_t farCount = far.PrepareTick(ctx.queue);

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

      // mobs: kinematic walk drive, terrain anchors for ManageTerrain,
      // bleeding ops — must run before debris.PreTick consumes the anchors
      mobs.PreTick(tick, world, ops, spawns);

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
          tpRig.Snap();   // re-entering from fly: don't ease across the gap
        }
        if (!wantAvatar && avatar.Spawned()) avatar.Despawn();
        // ---- melee: drive the swing, then pose the arm (game/melee.h) -------
        // BEFORE PreTick, because PreTick is what flattens the pose and
        // submits the kinematic limb targets — a weapon pose pushed in after
        // it would be a frame late and the blade would trail the mouse.
        {
          melee.Update(kTickDt, meleeHeld, meleeArmed, cam.Right(), cam.Up(),
                       cam.Forward());
          if (avatar.Spawned()) {
            // Equip/unequip only on a CHANGE. EquipItem builds a body and a
            // joint, so calling it every tick with the same weapon would
            // rebuild the sword 60 times a second; comparing against what is
            // already in the hand keeps this a no-op in the common case.
            const ItemDef* want = meleeArmed ? heldItem : nullptr;
            const std::string wantName = want ? want->name : std::string();
            if (avatar.HeldItem() != wantName) avatar.EquipItem(want);
            // Weight rises while the weapon is up and falls to zero at rest,
            // so the arm hands back to the walk cycle instead of being pinned
            // by an IK solve that is no longer expressing anything.
            const float w = melee.Phase() == SwingPhase::Idle ? 0.0f : 1.0f;
            avatar.SetWeaponPose(melee.HandOffset(), melee.BladeDir(),
                                 melee.BladeUp(), w);
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
          avatar.SetLook(lookRel, cam.pitch);
        }
        if (avatar.Spawned())
          avatar.PreTick(tick, player, avatarHeading, kTickDt, world, ops,
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
      }

      // ---- magic (game/spell.h) ---------------------------------------------
      // Same slot in the tick order as mobs and the avatar, and for the same
      // reason: everything a spell does leaves as ops on the streams assembled
      // below. The VM never touches a voxel buffer (thesis 1 / rule 3).
      std::vector<ExplosionOp> spellExps;
      {
        caster.mana.Tick();
        SpellEmission emit;

        // Consume the latch on the FIRST tick of the frame that sees it, and
        // clear it even when there is nothing spoken — otherwise a click on an
        // empty stack stays queued and fires the next spell the moment one is
        // spoken. Clearing outside the inner test is what makes this a one-shot
        // rather than a pending intent.
        const bool castNow = castQueued;
        castQueued = false;
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
          CastResult res =
              spells.Cast(caster.compiled, caster.mana, playerHealth,
                          0x9134A5EEu /*casterId*/, originFx, dirFx, tick, emit);
          caster.lastOutcome = res.outcome;
          if (res.outcome != CastOutcome::Nothing) caster.Clear(glyphs);
        }

        spells.Tick(tick, world, classOf, emit);

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

      // support-loss flags from the sim (burnt stems, undermined slabs) feed
      // the same island-check pipeline as explosions and brush erases
      debris.QueueSupportEvents(world.Snap());
      // island detection results + body burn + terrain collision upkeep
      // (may add cell ops and particle spawns from shattered bodies)
      std::vector<CellOp> cellOps;
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
      if (avatar.Spawned() && meleeArmed) {
        Vec3 eb, et;
        float ehw = 0;
        if (avatar.WeaponEdge(eb, et, ehw)) {
          if (lastEdgeValid) {
            // Tip speed is what scales the damage: the base of a blade barely
            // moves in a swing that whips the point through, and a cut should
            // reflect that. Measured over the tick, in world voxels/sec.
            const float tipSpeed = (et - lastEdgeTip).len() / kTickDt;
            const MeleeTuning& mt = melee.tuning;
            if (melee.Cutting() && tipSpeed > mt.minSpeed) {
              float t = (tipSpeed - mt.minSpeed) /
                        std::max(mt.fullSpeed - mt.minSpeed, 1e-3f);
              const float power = std::clamp(t, 0.0f, 1.0f);
              const float radius = ehw + heldItem->carveBonus;
              // Sample along the blade AND across the sweep, so a fast cut
              // does not tunnel between ticks. Both counts are bounded and
              // scale with how far the blade actually moved (rule 2): a
              // stationary blade costs one probe, and no swing can cost more
              // than kMaxSteps * kMaxAlong however fast the mouse is flicked.
              const float sweep = (et - lastEdgeTip).len();
              const int kMaxSteps = 6, kMaxAlong = 5;
              const int steps = std::clamp(
                  (int)std::ceil(sweep / std::max(radius, 0.5f)), 1, kMaxSteps);
              const int along = std::clamp(
                  (int)std::ceil((et - eb).len() / std::max(radius, 0.5f)), 1,
                  kMaxAlong);
              // One hit per body per swing tick: without this the same limb is
              // carved once per probe and a single cut removes a whole arm.
              std::vector<uint64_t> hitBodies;
              for (int s = 1; s <= steps && (int)hitBodies.size() < 8; s++) {
                const float u = (float)s / (float)steps;
                const Vec3 a = lastEdgeBase + (eb - lastEdgeBase) * u;
                const Vec3 b = lastEdgeTip + (et - lastEdgeTip) * u;
                for (int k = 0; k <= along; k++) {
                  const float v = (float)k / (float)along;
                  const Vec3 p = a + (b - a) * v;
                  // A short ray along the blade's own length is what finds
                  // what the edge is passing through. Using the segment the
                  // blade occupies (rather than a point test) is what lets a
                  // thin fast blade hit at all.
                  const Vec3 seg = (b - a);
                  const float segLen = seg.len();
                  if (segLen < 1e-4f) continue;
                  float frac = 1.0f;
                  const float probe = std::max(radius * 2.0f, 0.6f);
                  uint64_t hb = phys.CastRayBody(p, seg.normalized(), probe, frac);
                  if (!hb) continue;
                  bool seen = false;
                  for (uint64_t h : hitBodies) seen |= (h == hb);
                  if (seen) continue;
                  // A weapon must not cut its wielder. The avatar's own parts
                  // are permanently inside the swing arc — the blade starts in
                  // its own hand — so without this every guard would saw
                  // through the arm holding it.
                  if (avatar.OwnsBody(hb)) continue;
                  hitBodies.push_back(hb);
                  const Vec3 at = p + seg.normalized() * (frac * probe);
                  // LIVE FLESH CARVES; DEBRIS MELTS. The same two populations
                  // the laser splits on, through the same two calls — a mob
                  // limb loses voxels exactly where the edge crossed it, which
                  // is what makes dismemberment geometric rather than a
                  // threshold (DESIGN.md §7 "Carving living bodies").
                  const float dmg = heldItem->damage * power;
                  // Everything severed inside this scope is a BLADE cut, and
                  // gets the wet dismember sound on top of the creature's own
                  // cry. Both calls below can sever several frames deep —
                  // Damage() at zero hp or over the impact threshold,
                  // CarveLimbRadial() when the limb collapses — so the cause
                  // is marked around them rather than passed down through a
                  // chain the laser and explosions also use.
                  MobSystem::BladeCutScope blade(mobs, power);
                  if (mobs.Damage(hb, dmg, at, tipSpeed)) {
                    mobs.CarveLimbRadial(hb, at, radius * (0.6f + 0.4f * power),
                                         true /*ragged*/, true /*eject*/, world,
                                         spawns);
                  } else {
                    debris.MeltBodyAt(hb, at, radius, world, spawns);
                  }
                }
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
          phys.ApplyRadialImpulse(
              Vec3{(float)e.x, (float)e.y, (float)e.z},
              (float)e.radius * CurrentTuning().physics.explosionImpulseRadiusScale,
              (float)e.power * CurrentTuning().physics.explosionImpulseScale);
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
      phys.MovePlayerBody(playerBody, player.pos, kTickDt);
      double tSubmit0 = NowSeconds();
      SubmitTick(ctx, world, sim, tick, kDefaultSeed, ops, exps, cellOps,
                 tick % 15 == 0 /*hash occasionally*/, pc, true, particlesActive,
                 spawns, farCount, fluidSpawns, fluidCount, fluidSpeciesMat);
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
      // debris that ended the step overlapping the player pushes the player
      // out (fly mode ignores collision entirely, matching the voxel rules)
      if (!player.fly)
        player.ApplyPush(phys.PlayerPushOut(playerBody, player.pos), kindAt);
      double tEnd = NowSeconds();
      tickMsSmooth += ((float)((tEnd - t0) * 1000.0) - tickMsSmooth) * 0.1f;
      if (telemetry.Active()) {
        double streamMs = (tSubmit0 - t0) * 1000.0;
        double submitMs = (tSubmit1 - tSubmit0) * 1000.0;
        double physMs = (tPhys1 - tSubmit1) * 1000.0;
        double postMs = (tEnd - tPhys1) * 1000.0;
        TelemetryStage stages[] = {
          {"stream", streamMs}, {"submit", submitMs},
          {"physics", physMs}, {"post", postMs},
        };
        telemetry.Broadcast(tick, stages, 4);
      }
    }
    if (ui.paused) accumulator = std::min(accumulator, (double)kTickDt);

    // ---- render ----
    double tRender0 = NowSeconds();
    rhi::TextureView target = ctx.AcquireFrame();
    if (target) {
      // ViewEyePos, not EyePos: the render camera rides the step-smoothing
      // offset so voxel steps glide instead of popping. Everything that can
      // feed the sim (brush/laser/grenade rays, physics) stays on EyePos.
      Vec3 eye = player.ViewEyePos();
      // ---- avatar camera ----
      // The rig only decides where the RENDER eye sits. Picking rays, the
      // brush, the laser and the grenade all keep using player.EyePos(), so
      // switching to third person cannot change anything the sim sees — the
      // same guarantee the view-smoothing offset already relies on.
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
        std::vector<uint8_t> hide;
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
      // The listener rides the RENDER eye, not the player's head: in third
      // person the camera is where the player's attention is, and putting the
      // ears anywhere else makes panning disagree with what is on screen.
      // After the camera block, so `eye` is final for the frame.
      //
      // Footfalls are drained here rather than inside the tick loop because
      // that loop runs up to 4 times per frame; firing from inside it would
      // put several steps at the same instant.
      if (audioCues.Enabled()) {
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
          audioCues.SetNightAmbience(eye, want, allowStart);
        }

        audioCues.Update(dt, eye, cam.yaw, cam.pitch, &world);
      }
      avatar.ClearFootfalls();
      // Cleared unconditionally, like the footfalls: a queue that only drains
      // when audio happens to be on is a slow leak on a silent machine.
      debris.ClearBreakEvents();
      debris.ClearImpactEvents();
      mobs.ClearSeverEvents();
      mobs.ClearVoiceEvents();
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
      WriteRenderParams(ctx.queue, world, eye, cam,
                        (float)ctx.width / (float)ctx.height, ui.shadows,
                        (float)now, fogSmooth, (float)ctx.height, tick,
                        fluidCount,
                        (float)(accumulator / kTickDt));

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
      ui.totalChunks = kNumChunks;
      ui.voxelTotal = world.Snap().voxelTotal;
      ui.worldHash = world.Snap().worldHash;
      ui.mirrorValid = world.Snap().valid;
      ui.particleCount = world.Snap().particleCount;
      ui.bodyCount = debris.BodyCount();
      ui.activeBodyCount = debris.ActiveBodyCount();
      ui.prefabPending = (uint32_t)placer.PendingCount();
      ui.mobCount = mobs.MobCount();
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
      ui.health = playerHealth.Get();
      ui.healthMax = avatar.HealthMax();
      ui.playerAlive = avatar.IsAlive();
      // Body-condition readout: one figure slot per limb, keyed by the limb's
      // authored TAG and side suffix rather than by part name, so any humanoid
      // rig fills the same figure. A limb the rig does not have stays absent
      // and simply is not drawn.
      FillBodyUI(avatar, ui);
      ui.spellCost = caster.compiled.manaCost;
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
      for (int i = 0; i < kGlyphSlots; i++) {
        int gi = caster.inventory.At(i);
        ui.glyphSlots.push_back(
            gi >= 0 && gi < (int)glyphs.glyphs.size() ? glyphs.glyphs[gi].id : "");
      }
      // hotbar + swing readout (game/item.h, game/melee.h)
      ui.itemNames.clear();
      for (int i = 0; i < kItemSlots; i++) {
        const ItemDef* d = items.At(hotbar.slots[i].Empty() ? -1
                                                            : hotbar.slots[i].def);
        ui.itemNames.push_back(d ? d->name : "");
      }
      ui.itemSelected = hotbar.selected;
      switch (melee.Phase()) {
        case SwingPhase::Idle:    ui.swingPhase = meleeArmed ? "ready" : ""; break;
        case SwingPhase::Guard:   ui.swingPhase = "guard"; break;
        case SwingPhase::Wind:    ui.swingPhase = "winding"; break;
        case SwingPhase::Slash:   ui.swingPhase = "SLASH"; break;
        case SwingPhase::Recover: ui.swingPhase = "recover"; break;
      }
      ui.swingSpeed = melee.MouseSpeed();
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

      overlay.BeginFrame();
      // HUD first, dev panel second: the panel is a real ImGui window and gets
      // to sit on top of the chrome, not the other way round.
      overlay.DrawHUD(ui);
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

      // spell projectiles render as emissive sprites tinted by their element,
      // so a firebolt reads as fire and an acid bolt as acid with no per-spell
      // render code. THIS is the one place spell state becomes float: the
      // authoritative position is fixed-point and is only lerped to float here,
      // at the drawing boundary (spell.h thesis 3).
      for (const SpellProjectile& p : spells.Live()) {
        Sprite s{};
        s.pos[0] = SpellFxToFloat(p.pos.x);
        s.pos[1] = SpellFxToFloat(p.pos.y);
        s.pos[2] = SpellFxToFloat(p.pos.z);
        s.halfSize = 0.6f;
        s.color = p.spell.element < mats.size()
                      ? mats[p.spell.element].gpu.color0
                      : 0xFFFFFFFFu;
        s.emission = 1.0f;
        sprv.push_back(s);
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

      uint32_t debugBoxCount = 0;
      if (ui.showCollisionBoxes) {
        static std::vector<DebugBox> dbg;
        dbg.clear();
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
        if (!dbg.empty()) {
          debugBoxCount = (uint32_t)dbg.size();
          ctx.queue.WriteBuffer(world.debugBoxes, 0, dbg.data(),
                                dbg.size() * sizeof(DebugBox));
        }
      }

      // rigid bodies: debris takes slots [0, D), mob limbs stack after —
      // instances rebuild when either side changes (slot bases shift),
      // transforms are cheap and refresh per frame
      // Damaged micro bodies edited their bricks (copy-on-write) this tick, so
      // the shared pool has to reach the GPU before the march reads it. One
      // upload per tick regardless of how many bodies were hit — the flag is
      // set by every edit and cleared here.
      if (debris.MicroDirty()) {
        sim.UploadMicroBodies(ctx.queue, mbSet);
        mbSet.dirty = false;
      }
      BodyRegistry bodyReg(debris, mobs, &avatar);
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

      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      rhi::RenderPass rp = sim.BeginRenderPass(enc, target, ctx.surfaceFormat,
                                                       ctx.width, ctx.height);
      sim.DrawWorld(rp);
      sim.DrawParticles(rp);
      // Cube-debug mode only: in surface mode (the default) the raymarcher
      // draws the fluid as a real water surface and the cubes would z-fight it.
      if (CurrentTuning().render.fluidSurface < 0.5f)
        sim.DrawFluid(rp, fluidCount);
      sim.DrawBodies(rp, bodyInstCount);
      sim.DrawMicroBodies(rp, microCount);
      sim.DrawSprites(rp, (uint32_t)sprv.size());
      sim.DrawDebugBoxes(rp, debugBoxCount);
      // Wind arrows LAST of the world draws so they composite over everything
      // they annotate. Nothing is uploaded for them — the count is the only
      // CPU work, and at zero the draw is skipped outright (rule 2's shape,
      // applied to a debug view: off is not "cheap", it is nothing).
      sim.DrawWindField(rp, ui.showWindField
                                ? WindDebugArrowCount(CurrentTuning())
                                : 0u);
      overlay.Render(rp);
      rp.End();
      ctx.queue.Submit(enc.Finish());
      ctx.Present();
    }
    if (telemetry.Active() && ticksThisFrame > 0) {
      double renderMs = (NowSeconds() - tRender0) * 1000.0;
      TelemetryStage rs = {"render", renderMs};
      telemetry.Broadcast(tick, &rs, 1);
    }
    if (g_harnessFrames > 0) g_harnessRenderMs += (NowSeconds() - tRender0) * 1000.0;
    ctx.ProcessEvents();  // pumps MapAsync callbacks (mirror updates)
    telemetry.Poll();
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
