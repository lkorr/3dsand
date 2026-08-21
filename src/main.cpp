// sandvox — 3D falling-sand voxel engine (v0). See DESIGN.md.
// Fixed 30 Hz GPU simulation, uncapped raymarched rendering, walkable player,
// JSON materials, deterministic kernels with per-tick world hash.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <GLFW/glfw3.h>

#include "audio/cues.h"
#include "game/avatar.h"
#include "game/brush.h"
#include "game/camera.h"
#include "game/mob.h"
#include "game/player.h"
#include "game/prefab.h"
#include "game/thirdperson.h"
#include "gpu/context.h"
#include "gpu/resources.h"
#include "math3d.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/farfield.h"
#include "sim/materials.h"
#include "sim/microbody.h"
#include "sim/microvox.h"
#include "sim/simulation.h"
#include "sim/tuning.h"
#include "sim/stream.h"
#include "sim/voxload.h"
#include "sim/world.h"
#include "sim/worldio.h"
#include "ui/overlay.h"

namespace {

constexpr float kTickDt = 1.0f / 30.0f;
constexpr uint32_t kDefaultSeed = 1337;

std::string AssetDir() {
#ifdef SANDVOX_ASSET_DIR
  return SANDVOX_ASSET_DIR;
#else
  return "assets";
#endif
}

double NowSec() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// fogDensity defaults to the fully-filled-cascade pin so every call site that
// renders against complete far-field data (the selftest's benchmark,
// screenshot, and debris views) stays a one-liner. The live frame loop passes
// the smoothed adaptive value instead (plan phase 3B).
// Ticks per in-game day, from the tuning cycle length. Sim runs at 30 Hz.
uint32_t TicksPerDay(const Tuning& t) {
  int m = t.dayNight.cycleMinutes < 1 ? 1 : t.dayNight.cycleMinutes;
  return (uint32_t)m * 60u * 30u;
}

// The sky for a given sim tick. Both the renderer and the sim's reaction gate
// derive from this same tick-based phase, which is what keeps a sun-driven
// reaction deterministic (CLAUDE.md rule 1).
SkyState SkyForTick(const Tuning& t, uint32_t tick) {
  uint32_t tpd = TicksPerDay(t);
  uint32_t phase = DayPhaseForTick(tick, tpd, t.dayNight.freeze != 0,
                                   (uint32_t)t.dayNight.freezePhase);
  return ComputeSkyState(t, phase, tpd ? tick / tpd : 0u);
}

void WriteRenderParams(const wgpu::Queue& queue, const World& world,
                       const Vec3& eye, const Camera& cam, float aspect,
                       bool shadows, float time,
                       float fogDensity = kFarFogDensity,
                       float viewPx = 1080.0f,
                       uint32_t tick = 0) {
  RenderParams rp{};
  Vec3 f = cam.Forward(), r = cam.Right(), u = cam.Up();
  rp.camPos[0] = eye.x; rp.camPos[1] = eye.y; rp.camPos[2] = eye.z;
  rp.camRight[0] = r.x; rp.camRight[1] = r.y; rp.camRight[2] = r.z;
  rp.camUp[0] = u.x; rp.camUp[1] = u.y; rp.camUp[2] = u.z;
  rp.camFwd[0] = f.x; rp.camFwd[1] = f.y; rp.camFwd[2] = f.z;
  rp.tanHalfFov = std::tan(CurrentTuning().camera.fovY * 0.5f);
  rp.aspect = aspect;
  rp.time = time;
  rp.flags = shadows ? 1u : 0u;
  // ~41 deg elevation: low enough that terrain and canopy cast readable
  // shadows (near field AND the far-field cascade shadow march), high enough
  // that valleys aren't pits. The old 0.78 y put the sun ~52 deg up and
  // flattened the world — shadows were 1-2 cells long and the far field read
  // as unlit wallpaper.
  // Sun/moon now come from the tick-driven cycle rather than a fixed tuning
  // vector: render.sunDir survives only as the fallback when the cycle is
  // disabled (cycleMinutes clamped, freeze pinned) and as the tuner's manual
  // handle. See ComputeSkyState.
  const Tuning& tun = CurrentTuning();
  SkyState sky = SkyForTick(tun, tick);
  rp.sunDir[0] = sky.sunDir[0];
  rp.sunDir[1] = sky.sunDir[1];
  rp.sunDir[2] = sky.sunDir[2];
  rp.moonDir[0] = sky.moonDir[0];
  rp.moonDir[1] = sky.moonDir[1];
  rp.moonDir[2] = sky.moonDir[2];
  rp.dayT = sky.dayT;
  rp.sunUp = sky.sunUp;
  rp.moonPhase = sky.moonPhase;
  rp.starRot = sky.starRot;
  rp.fogDensity = fogDensity;  // horizon fades at the trusted far-field extent
  rp.viewPx = viewPx;          // water ripple LOD footprint (see world.h)
  // Micro-detail animation clock + per-cell variation key (see world.h). Both
  // are render-only inputs; the tick is passed rather than `time` so a flipbook
  // advances at the sim's rate on every machine and reproduces in a replay.
  rp.tick = tick;
  rp.seed = kDefaultSeed;
  IVec3 o = world.WindowOrigin();
  rp.origin[0] = o.x; rp.origin[1] = o.y; rp.origin[2] = o.z;
  queue.WriteBuffer(world.renderUBO, 0, &rp, sizeof(rp));
}

// Encode + submit one sim tick (uniform writes must precede the submit and
// happen once per tick, hence submit-per-tick). particlesActive must be
// derived only from tick-deterministic inputs (explosion history + a settled
// particle count), never from frame timing — see DESIGN.md §2/§4.
void SubmitTick(GpuContext& ctx, World& world, Simulation& sim, uint32_t tick,
                uint32_t seed, const std::vector<BrushOp>& ops,
                const std::vector<ExplosionOp>& exps,
                const std::vector<CellOp>& cells, bool hashEnable,
                IVec3 playerChunk, bool wantReadback, bool particlesActive,
                const std::vector<ParticleSpawn>& spawns = {},
                uint32_t farCount = 0) {
  particlesActive = particlesActive || !exps.empty() || !spawns.empty();
  uint32_t cellCount = std::min((uint32_t)cells.size(), kMaxCellOpsPerTick);
  uint32_t spawnCount = std::min((uint32_t)spawns.size(), kMaxParticleSpawnsPerTick);
  TickParams tp{tick, seed, (uint32_t)ops.size(), hashEnable ? 1u : 0u,
                (uint32_t)exps.size(), sim.Page(), cellCount, 0};
  tp.spawnCount = spawnCount;
  tp.farCount = farCount;  // far-field fills ride the tick submit (render-only)
  // Day phase for THIS tick. Derived from `tick` alone — the daylight-gated
  // reactions read it, so anything frame-timed here would break determinism.
  const Tuning& dtun = CurrentTuning();
  tp.dayPhase = DayPhaseForTick(tick, TicksPerDay(dtun),
                                dtun.dayNight.freeze != 0,
                                (uint32_t)dtun.dayNight.freezePhase);
  IVec3 wo = world.WindowOrigin();
  tp.origin[0] = wo.x; tp.origin[1] = wo.y; tp.origin[2] = wo.z;
  ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
  if (!ops.empty())
    ctx.queue.WriteBuffer(world.opsBuf, 0, ops.data(), ops.size() * sizeof(BrushOp));
  if (!exps.empty())
    ctx.queue.WriteBuffer(world.expOps, 0, exps.data(), exps.size() * sizeof(ExplosionOp));
  if (cellCount > 0)
    ctx.queue.WriteBuffer(world.cellOps, 0, cells.data(), cellCount * sizeof(CellOp));
  if (spawnCount > 0)
    ctx.queue.WriteBuffer(world.spawnOps, 0, spawns.data(),
                          spawnCount * sizeof(ParticleSpawn));
  if (particlesActive) {
    // the write page starts each tick empty; survivors + emissions repopulate
    uint32_t zero = 0;
    ctx.queue.WriteBuffer(world.particleCounts, (1 - sim.Page()) * 4, &zero, 4);
  }

  // Day/night sleep handshake. The daylight-gated reactions deliberately do
  // NOT hold a chunk awake while their condition is unmet, so a pond that went
  // to sleep at dusk would never notice sunrise. Wake the world on the ticks
  // where daylight actually switches on or off — a handful per in-game day.
  //
  // Derived from the tick, not from frame timing, so every machine wakes on
  // the same tick and the hash stays identical (CLAUDE.md rule 1).
  if (tick > 0) {
    uint32_t prevPhase = DayPhaseForTick(tick - 1, TicksPerDay(dtun),
                                         dtun.dayNight.freeze != 0,
                                         (uint32_t)dtun.dayNight.freezePhase);
    bool wasDay = DaylightStrengthCpu(prevPhase) > 0;
    bool isDay = DaylightStrengthCpu(tp.dayPhase) > 0;
    if (wasDay != isDay) sim.EncodeWakeAll(ctx.queue);
  }

  wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  sim.EncodeTick(enc, (uint32_t)ops.size(), hashEnable, (uint32_t)exps.size(),
                 particlesActive, cellCount, spawnCount);
  sim.EncodeFarFill(enc, farCount);
  bool doCopy = false;
  if (wantReadback) {
    doCopy = world.EncodeReadbacks(ctx.device, enc,
                                   {playerChunk.x - 1, playerChunk.y - 1, playerChunk.z - 1},
                                   1 - sim.Page(), tick);
    if (doCopy) world.EncodeDirtyCopy(enc, sim.DirtyNext());
  }
  wgpu::CommandBuffer cmd = enc.Finish();
  ctx.queue.Submit(1, &cmd);
  sim.FlipPage();
  if (doCopy) world.KickReadback();
}

void SubmitWorldgen(GpuContext& ctx, World& world, Simulation& sim, uint32_t seed) {
  TickParams tp{0, seed, 0, 0};
  IVec3 wo = world.WindowOrigin();
  tp.origin[0] = wo.x; tp.origin[1] = wo.y; tp.origin[2] = wo.z;
  ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
  wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  sim.EncodeWorldgen(enc);
  wgpu::CommandBuffer cmd = enc.Finish();
  ctx.queue.Submit(1, &cmd);
}

// Body render plumbing shared by the frame loop and the selftest. Debris takes
// slots [0, D), mob limbs stack after, and the player avatar's parts stack
// after those — so the three systems must always be walked in that order and
// with those bases, which is exactly the sort of agreement that rots when it
// is spelled out at two call sites.
//
// `avatar` may be null (selftest paths that never spawn one); the slot walk is
// then identical to what it was before the avatar existed.
void BuildBodyXforms(const DebrisSystem& debris, const MobSystem& mobs,
                     const PlayerAvatar* avatar,
                     std::vector<BodyXformGpu>& out) {
  debris.BuildXforms(out);
  mobs.AppendXforms(out);
  if (avatar) avatar->AppendXforms(out);
}
// Micro bodies (PLAN §C): already compacted, so `out.size()` IS the draw's
// instance count and an empty result means the pass is skipped entirely.
void BuildMicroInsts(const DebrisSystem& debris, const MobSystem& mobs,
                     const PlayerAvatar* avatar,
                     std::vector<MicroBodyInstGpu>& out) {
  out.clear();
  debris.AppendMicroInsts(out);
  mobs.AppendMicroInsts(out, debris.BodyCount());
  if (avatar)
    avatar->AppendMicroInsts(out, debris.BodyCount() + mobs.LimbBodyCount());
}

bool WriteBmp(const std::string& path, const std::vector<uint8_t>& rgba,
              uint32_t w, uint32_t h) {
  uint32_t rowBytes = w * 3;
  uint32_t imgBytes = rowBytes * h;
  uint32_t fileBytes = 54 + imgBytes;
  std::vector<uint8_t> f(fileBytes, 0);
  auto put32 = [&](size_t off, uint32_t v) { std::memcpy(&f[off], &v, 4); };
  f[0] = 'B'; f[1] = 'M';
  put32(2, fileBytes); put32(10, 54); put32(14, 40);
  put32(18, w); put32(22, h);
  f[26] = 1; f[28] = 24;
  put32(34, imgBytes);
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* src = &rgba[(size_t)(h - 1 - y) * w * 4];
    uint8_t* dst = &f[54 + (size_t)y * rowBytes];
    for (uint32_t x = 0; x < w; x++) {
      dst[x * 3 + 0] = src[x * 4 + 2];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 0];
    }
  }
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;
  std::fwrite(f.data(), 1, f.size(), fp);
  std::fclose(fp);
  return true;
}

// Whole-world hash of a quiescent world (no tick): save/load verification.
uint32_t HashWorldNow(GpuContext& ctx, World& world, Simulation& sim, uint32_t seed);

// Synchronously read the 4-byte world hash (selftest only).
uint32_t ReadHashSync(GpuContext& ctx, World& world) {
  wgpu::Buffer staging = CreateBuffer(ctx.device, 16,
                                      wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                      "hashRead");
  wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  enc.CopyBufferToBuffer(world.hash, 0, staging, 0, 16);
  wgpu::CommandBuffer cmd = enc.Finish();
  ctx.queue.Submit(1, &cmd);
  uint32_t result = 0;
  wgpu::Future f = staging.MapAsync(
      wgpu::MapMode::Read, 0, 16, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
        if (status == wgpu::MapAsyncStatus::Success) {
          std::memcpy(&result, staging.GetConstMappedRange(0, 16), 4);
          staging.Unmap();
        }
      });
  ctx.instance.WaitAny(f, UINT64_MAX);
  return result;
}

uint32_t HashWorldNow(GpuContext& ctx, World& world, Simulation& sim, uint32_t seed) {
  TickParams tp{0, seed, 0, 1, 0, 0, 0, 0};
  ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
  wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  sim.EncodeHashOnly(enc);
  wgpu::CommandBuffer cmd = enc.Finish();
  ctx.queue.Submit(1, &cmd);
  return ReadHashSync(ctx, world);
}

std::vector<BrushOp> SelftestOps(uint32_t tick) {
  std::vector<BrushOp> ops;
  if (tick >= 5 && tick < 150) {
    ops.push_back({100, 170, 100, 6, kMatSand, 0, 0, 0});
    ops.push_back({176, 150, 176, 5, kMatWater, 0, 0, 0});
  }
  if (tick >= 30 && tick < 90) {
    ops.push_back({64, 60, 72, 4, kMatSmoke, 0, 0, 0});
  }
  // reaction-system coverage: lava boiling the pool, fire on the wood
  // platform, seeds germinating — all feed the determinism hash check
  if (tick >= 40 && tick < 100) {
    ops.push_back({176, 120, 150, 4, kMatLava, 0, 0, 0});
  }
  if (tick >= 60 && tick < 120) {
    ops.push_back({110, 80, 110, 3, kMatFire, 0, 0, 0});
  }
  if (tick >= 10 && tick < 16) {
    ops.push_back({150, 90, 128, 2, kMatSeed, 0, 0, 0});
  }
  // melt-mode coverage (laser, PLAN §C1): catches the falling sand column in
  // a mode-2 brush — molten-glass conversion feeds the determinism hash
  if (tick >= 70 && tick < 100) {
    ops.push_back({100, 166, 100, 3, 0, 2u, 0, 0});
  }
  return ops;
}

// Explosion + particle coverage for the determinism hashes: a terrain blast
// (solids/powder ejecta) and a pool blast (liquid splash).
std::vector<ExplosionOp> SelftestExps(uint32_t tick, uint32_t seed) {
  std::vector<ExplosionOp> exps;
  if (tick == 60) {
    int h = World::TerrainHeight(100, 100, seed);
    exps.push_back({100, h, 100, 14, 400, 0, 0, 0});
  }
  if (tick == 90) {
    exps.push_back({176, 50, 176, 10, 300, 0, 0, 0});
  }
  return exps;
}
constexpr uint32_t kSelftestFirstExp = 60;
// particles are possible from the first scripted explosion until well after
// the last ejecta has reinserted; must be a pure function of tick (§2/§4)
bool SelftestParticlesActive(uint32_t tick) { return tick >= kSelftestFirstExp; }

// Synchronously read both particle page counts (selftest only).
void ReadCountsSync(GpuContext& ctx, World& world, uint32_t out[2]) {
  wgpu::Buffer staging = CreateBuffer(ctx.device, 16,
                                      wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                      "countsRead");
  wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  enc.CopyBufferToBuffer(world.particleCounts, 0, staging, 0, 16);
  wgpu::CommandBuffer cmd = enc.Finish();
  ctx.queue.Submit(1, &cmd);
  wgpu::Future f = staging.MapAsync(
      wgpu::MapMode::Read, 0, 16, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
        if (status == wgpu::MapAsyncStatus::Success) {
          std::memcpy(out, staging.GetConstMappedRange(0, 16), 8);
          staging.Unmap();
        }
      });
  ctx.instance.WaitAny(f, UINT64_MAX);
}

// Time of day used by --shot, as a 0..1 fraction of the cycle (0 = midnight,
// 0.5 = noon). Set by `--time`; see RunShots.
float g_shotTimeOfDay = 0.34f;

// Which mob def the player wears. Swapping the player character is meant to be
// a one-line data change, so this lives in ONE place: the game's avatar and
// the selftest's avatar block both read it. Two literals would let the test
// keep passing against a character nobody plays.
const char* kAvatarDefName = "mina";

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
    wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeFarFill(enc, n);
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx.queue.Submit(1, &cmd);
  }
  for (uint32_t t = 1; t <= 120; t++)  // powders settle so shots match play
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, {}, false, {8, 3, 8},
               false, false);
  ctx.WaitIdle();

  const uint32_t W = 1920, H = 1080;
  wgpu::TextureDescriptor td{};
  td.size = {W, H, 1};
  td.format = wgpu::TextureFormat::RGBA8Unorm;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  wgpu::Texture offscreen = ctx.device.CreateTexture(&td);
  wgpu::TextureView view = offscreen.CreateView();
  auto grab = [&](const char* path) {
    wgpu::Buffer shot = CreateBuffer(ctx.device, (uint64_t)W * H * 4,
                                     wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                     "screenshot");
    wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo srcT{};
    srcT.texture = offscreen;
    wgpu::TexelCopyBufferInfo dstB{};
    dstB.buffer = shot;
    dstB.layout.bytesPerRow = W * 4;
    dstB.layout.rowsPerImage = H;
    wgpu::Extent3D ext{W, H, 1};
    enc.CopyTextureToBuffer(&srcT, &dstB, &ext);
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx.queue.Submit(1, &cmd);
    std::vector<uint8_t> pixels(W * H * 4);
    bool got = false;
    wgpu::Future f = shot.MapAsync(
        wgpu::MapMode::Read, 0, pixels.size(), wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
          if (status == wgpu::MapAsyncStatus::Success) {
            std::memcpy(pixels.data(), shot.GetConstMappedRange(0, pixels.size()),
                        pixels.size());
            shot.Unmap();
            got = true;
          }
        });
    ctx.instance.WaitAny(f, UINT64_MAX);
    if (got && WriteBmp(path, pixels, W, H)) std::printf("wrote %s\n", path);
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
    wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    wgpu::RenderPassEncoder rp =
        sim.BeginRenderPass(enc, view, wgpu::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    rp.End();
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx.queue.Submit(1, &cmd);
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
  // Oil pond (260,300) and lava pool (220,520): the non-water liquid paths.
  // Oil exercises the palette-derived absorption; lava is MATF_OPAQUE and must
  // still render as a surface hit, untouched by any of the water work.
  render({236, 80, 276}, 0.785f, -0.45f, "screenshot_oil.bmp");
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
      // same word rules as a brush paint: liquid born full, stamp 0xFF
      uint32_t word = (kMatLava & 0xFFFu) | (7u << 12) | (0xFFu << 16);
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
      gore.push_back({World::SlotCellIndex(c),
                      (mat & 0xFFFu) | (state << 12) | (0xFFu << 16)});
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
        // Same word rules as a brush paint on a solid: state 0, stamp 0xFF.
        flora.push_back({World::SlotCellIndex(c), (mat & 0xFFFu) | (0xFFu << 16)});
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
  return 0;
}

// Count of chunks whose dirty flag is set (selftest only — blocking readback).
uint32_t ReadActiveChunksSync(GpuContext& ctx, World& world, Simulation& sim) {
  wgpu::Buffer staging = CreateBuffer(ctx.device, kNumChunks * 4,
                                      wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                      "activeRead");
  wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  enc.CopyBufferToBuffer(sim.DirtyActive(), 0, staging, 0, kNumChunks * 4);
  wgpu::CommandBuffer cmd = enc.Finish();
  ctx.queue.Submit(1, &cmd);
  uint32_t n = 0;
  wgpu::Future f = staging.MapAsync(
      wgpu::MapMode::Read, 0, kNumChunks * 4, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
        if (status == wgpu::MapAsyncStatus::Success) {
          const uint32_t* d =
              (const uint32_t*)staging.GetConstMappedRange(0, kNumChunks * 4);
          for (uint32_t i = 0; i < kNumChunks; i++)
            if (d[i] != 0) n++;
          staging.Unmap();
        }
      });
  ctx.instance.WaitAny(f, UINT64_MAX);
  return n;
}

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

  // body upload, same slot agreement as the frame loop: debris first, mobs after
  std::vector<BodyXformGpu> xf;
  BuildBodyXforms(debris, mobs, nullptr, xf);
  if (!xf.empty())
    ctx.queue.WriteBuffer(world.bodyXforms, 0, xf.data(),
                          xf.size() * sizeof(BodyXformGpu));
  std::vector<MicroBodyInstGpu> microInsts;
  BuildMicroInsts(debris, mobs, nullptr, microInsts);
  std::vector<BodyVoxInst> inst;
  debris.BuildInstances(inst);
  mobs.AppendInstances(inst, debris.BodyCount());
  if (!inst.empty())
    ctx.queue.WriteBuffer(world.bodyInstances, 0, inst.data(),
                          inst.size() * sizeof(BodyVoxInst));

  // Aim at the centroid of the LIVE limbs, not the spawn point — the mob has
  // been walking, and after heavy dismemberment its origin is nowhere near
  // the visible body.
  Vec3 target = mobs.MobOrigin(id) +
                Vec3{def.worldSize.x * 0.5f, def.worldSize.y * 0.3f,
                     def.worldSize.z * 0.5f};
  {
    std::vector<BodyXformGpu> mt;
    mobs.AppendXforms(mt);
    if (!mt.empty()) {
      Vec3 sum{};
      for (const BodyXformGpu& m : mt)
        sum += Vec3{m.pos[0], m.pos[1], m.pos[2]};
      target = sum * (1.0f / (float)mt.size()) + Vec3{0, 1, 0};
    }
  }

  const uint32_t W = 1280, H = 720;
  wgpu::TextureDescriptor td{};
  td.size = {W, H, 1};
  td.format = wgpu::TextureFormat::RGBA8Unorm;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
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
    wgpu::Texture tex = ctx.device.CreateTexture(&td);
    wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    wgpu::RenderPassEncoder rp = sim.BeginRenderPass(
        enc, tex.CreateView(), wgpu::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    sim.DrawBodies(rp, (uint32_t)inst.size());
    if (!microInsts.empty()) sim.DrawMicroBodies(rp, ctx.queue, microInsts);
    rp.End();
    wgpu::Buffer shotBuf = CreateBuffer(
        ctx.device, (uint64_t)W * H * 4,
        wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst, "mobShot");
    wgpu::TexelCopyTextureInfo srcT{};
    srcT.texture = tex;
    wgpu::TexelCopyBufferInfo dstB{};
    dstB.buffer = shotBuf;
    dstB.layout.bytesPerRow = W * 4;
    dstB.layout.rowsPerImage = H;
    wgpu::Extent3D ext{W, H, 1};
    enc.CopyTextureToBuffer(&srcT, &dstB, &ext);
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx.queue.Submit(1, &cmd);
    std::vector<uint8_t> pixels((size_t)W * H * 4, 0);
    wgpu::Future f = shotBuf.MapAsync(
        wgpu::MapMode::Read, 0, pixels.size(), wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
          if (status == wgpu::MapAsyncStatus::Success) {
            std::memcpy(pixels.data(),
                        shotBuf.GetConstMappedRange(0, pixels.size()),
                        pixels.size());
            shotBuf.Unmap();
          }
        });
    ctx.instance.WaitAny(f, UINT64_MAX);
    if (WriteBmp(path, pixels, W, H)) std::printf("wrote %s\n", path);
  };
  // Camera directions are relative to the mob's FACING (it turns while it
  // walks): the side view is the one that shows pitch, the front quarter
  // shows limb placement.
  Vec3 fwd = mobs.MobFacing(id);
  Vec3 right{fwd.z, 0, -fwd.x};
  // Frame by the def's own SIZE rather than a fixed 18 voxels: the dummy and
  // critter are ~8 voxels tall but a humanoid rig is ~28, and a constant
  // distance either crops the tall one or leaves the short one a speck.
  const float shotDist =
      std::max(18.0f, 2.4f * std::max(def.worldSize.y,
                                      std::max(def.worldSize.x,
                                               def.worldSize.z)));
  shoot(right + Vec3{0, 0.15f, 0}, shotDist, "screenshot_mob_side.bmp");
  shoot((fwd + right) * 0.7071f + Vec3{0, 0.3f, 0}, shotDist,
        "screenshot_mob_quarter.bmp");
  shoot(fwd + Vec3{0, 0.15f, 0}, shotDist, "screenshot_mob_front.bmp");
  return 0;
}

int RunSelftest(GpuContext& ctx, World& world, Simulation& sim,
                const std::vector<MaterialDef>& mats, Physics& phys,
                DebrisSystem& debris, MobSystem& mobs, Stream& stream) {
  std::printf("=== selftest ===\n");
  constexpr int kTicks = 200;

  // determinism: two identical runs must produce identical hash sequences
  std::vector<uint32_t> hashes[2];
  for (int run = 0; run < 2; run++) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    for (uint32_t t = 1; t <= kTicks; t++) {
      SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t),
                 SelftestExps(t, kDefaultSeed), {}, true, {8, 3, 8}, false,
                 SelftestParticlesActive(t));
      hashes[run].push_back(ReadHashSync(ctx, world));
    }
  }
  bool deterministic = hashes[0] == hashes[1];
  std::printf("determinism: %s (final hash %08x over %d ticks)\n",
              deterministic ? "PASS" : "FAIL", hashes[0].back(), kTicks);
  if (!deterministic) {
    for (int i = 0; i < kTicks; i++) {
      if (hashes[0][i] != hashes[1][i]) {
        std::printf("  first divergence at tick %d: %08x vs %08x\n", i + 1,
                    hashes[0][i], hashes[1][i]);
        break;
      }
    }
  }

  // sleep: a settled world must go (nearly) fully idle — the M2 exit
  // criterion, and the guard against reaction rules that never stop matching.
  // Includes an explosion: every ejected particle must reinsert and die.
  uint32_t sleepActive = 0;
  uint32_t particlesLeft = 0;
  int settled = 0;  // tick at which the world went quiet (or the cap)
  {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    uint32_t t = 0;
    // Settle budget is ADAPTIVE: 500 fixed ticks was tuned for the 256^3
    // window; the 512^3 window holds 8x the content (every pond and lava
    // pocket in 32 m of world), and freshly generated liquid legitimately
    // takes longer to equalize. Tick until the world is quiet, hard-capped —
    // the cap is what still catches never-sleeping content (rule 2).
    uint32_t quiet = kNumChunks;
    for (int i = 0; i < 3000; i++) {
      std::vector<ExplosionOp> exps;
      if (i == 30) exps.push_back({110, 76, 110, 12, 350, 0, 0, 0});  // wood slab
      bool pactive = i >= 30 && i < 460;
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, exps, {}, false, {8, 3, 8},
                 false, pactive);
      if (i >= 500 && i % 100 == 0) {
        ctx.WaitIdle();
        quiet = ReadActiveChunksSync(ctx, world, sim);
        settled = i;
        if (quiet < 32) break;
      }
    }
    ctx.WaitIdle();
    uint32_t counts[2] = {};
    ReadCountsSync(ctx, world, counts);
    particlesLeft = std::min(counts[sim.Page()], kParticleCap);
    double s0 = NowSec();  // settled-world cost: the whole point of dirty dispatch
    for (int i = 0; i < 100; i++)
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false, {8, 3, 8},
                 false, false);
    ctx.WaitIdle();
    std::printf("sim settled: %.3f ms/tick\n", (NowSec() - s0) * 1000.0 / 100.0);

    wgpu::Buffer staging = CreateBuffer(ctx.device, kNumChunks * 4,
                                        wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                        "dirtyRead");
    wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    enc.CopyBufferToBuffer(sim.DirtyActive(), 0, staging, 0, kNumChunks * 4);
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx.queue.Submit(1, &cmd);
    std::vector<uint32_t> awake;
    wgpu::Future f = staging.MapAsync(
        wgpu::MapMode::Read, 0, kNumChunks * 4, wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
          if (status == wgpu::MapAsyncStatus::Success) {
            const uint32_t* d =
                (const uint32_t*)staging.GetConstMappedRange(0, kNumChunks * 4);
            for (uint32_t i = 0; i < kNumChunks; i++) {
              if (d[i] != 0) {
                sleepActive++;
                awake.push_back(i);
              }
            }
            staging.Unmap();
          }
        });
    ctx.instance.WaitAny(f, UINT64_MAX);

    // diagnosis on failure: where are the awake chunks, and what's in them?
    if (sleepActive >= 32 && !awake.empty()) {
      for (size_t i = 0; i < awake.size() && i < 12; i++) {
        uint32_t ci = awake[i];
        std::printf("  awake chunk (%u,%u,%u)", ci % kNChunk, (ci / kNChunk) % kNChunk,
                    ci / (kNChunk * kNChunk));
        if (i % 4 == 3) std::printf("\n");
      }
      std::printf("\n");
      // material histogram of the first awake chunks
      wgpu::Buffer vstage = CreateBuffer(ctx.device, kChunkVol * 4 * 4,
                                         wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                         "voxRead");
      wgpu::CommandEncoder e2 = ctx.device.CreateCommandEncoder();
      for (int k = 0; k < 4 && k < (int)awake.size(); k++)
        e2.CopyBufferToBuffer(world.voxels, (uint64_t)awake[k] * kChunkVol * 4, vstage,
                              (uint64_t)k * kChunkVol * 4, kChunkVol * 4);
      wgpu::CommandBuffer c2 = e2.Finish();
      ctx.queue.Submit(1, &c2);
      uint32_t hist[64] = {};
      wgpu::Future f2 = vstage.MapAsync(
          wgpu::MapMode::Read, 0, kChunkVol * 4 * 4, wgpu::CallbackMode::WaitAnyOnly,
          [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
            if (status == wgpu::MapAsyncStatus::Success) {
              const uint32_t* v =
                  (const uint32_t*)vstage.GetConstMappedRange(0, kChunkVol * 4 * 4);
              for (uint32_t i = 0; i < kChunkVol * 4; i++) hist[std::min(v[i] & 0xFFFu, 63u)]++;
              vstage.Unmap();
            }
          });
      ctx.instance.WaitAny(f2, UINT64_MAX);
      std::printf("  first-4-chunk contents:");
      for (uint32_t m = 1; m < 64; m++)
        if (hist[m]) std::printf(" %s=%u", m < mats.size() ? mats[m].name.c_str() : "?", hist[m]);
      std::printf("\n");
    }
  }
  bool sleepOk = sleepActive < 32 && particlesLeft == 0;
  std::printf("sleep: %s (%u / %u chunks active, %u particles alive, quiet "
              "after ~%d settle ticks)\n",
              sleepOk ? "PASS" : "FAIL", sleepActive, kNumChunks, particlesLeft,
              settled);

  // ---- pond freezes shore-first, not uniformly -------------------------------
  // The water->ice rule is scaled by its count of non-water neighbours
  // (reactions.json), so ice appears at the rim first and works inward. That
  // SHAPE is the whole point of the rule and it is invisible to the hash test,
  // which only proves the sim is reproducible, not that it is right.
  //
  // What this measures is a RATE gradient, not an absolute gate, and the
  // distinction is worth stating because it is easy to write a test that
  // asserts the wrong thing (this one did, first time round):
  //
  //   An open pond's whole surface has AIR above it, so every surface cell
  //   counts >= 1 non-water neighbour and CAN freeze. Only water enclosed by
  //   water on all six sides is truly gated to zero, which in a real pond
  //   means the sub-surface body, not the top face.
  //
  // So the rim's advantage is that it counts 2-3 (bank + air) against the
  // middle's 1, i.e. it freezes 1.6-2.2x faster and the ice front then feeds
  // itself inward. The check therefore samples EARLY, while that ratio is
  // still visible, and separately asserts the hard gate on a cell that really
  // is enclosed: the pond's bottom layer under unfrozen water.
  bool pondOk = false;
  {
    auto matId = [&](const char* n) {
      for (size_t i = 0; i < mats.size(); i++)
        if (mats[i].name == n) return (int)i;
      return -1;
    };
    const int wi = matId("water"), si = matId("stone"), ii = matId("ice");
    // Pin the cycle at midnight so the night-gated rule fires every tick.
    Tuning night = CurrentTuning();
    night.dayNight.freeze = 1;
    night.dayNight.freezePhase = 0;  // 0 = midnight
    Tuning saved = CurrentTuning();
    SetCurrentTuning(night);

    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();

    // A 24x24 pond, 3 deep, walled and floored in stone, with open air above so
    // needsSky passes. Placed inside the window, well clear of terrain. Three
    // deep is what gives a middle layer (y+1) that is enclosed by water above
    // and below — the cells the count-0 gate must forbid outright.
    const int px = 96, py = 120, pz = 96, R = 12, kDepth = 3;
    std::vector<CellOp> pond;
    auto put = [&](int x, int y, int z, int m) {
      uint32_t state = (m == wi) ? 7u : 0u;  // liquids are born full (LIQ_FULL_STATE)
      pond.push_back({World::SlotCellIndex({x, y, z}),
                      (uint32_t)((m & 0xFFF) | (state << 12))});
    };
    for (int z = -R - 1; z <= R + 1; z++)
      for (int x = -R - 1; x <= R + 1; x++) {
        put(px + x, py - 1, pz + z, si);  // floor
        bool rim = (x < -R || x > R || z < -R || z > R);
        for (int y = 0; y < kDepth; y++) put(px + x, py + y, pz + z, rim ? si : wi);
        for (int y = kDepth; y < kDepth + 3; y++) put(px + x, py + y, pz + z, 0);
      }
    uint32_t pt = 1;
    SubmitTick(ctx, world, sim, pt, kDefaultSeed, {}, {}, pond, false,
               {6, 7, 6}, false, false);
    ctx.WaitIdle();

    // Sampled while the rim/middle rate ratio is still visible. Run much
    // longer and the whole surface saturates at 100% ice, which says nothing
    // about the ordering.
    for (uint32_t t = 2; t <= 250; t++)
      SubmitTick(ctx, world, sim, ++pt, kDefaultSeed, {}, {}, {}, false,
                 {6, 7, 6}, false, false);
    ctx.WaitIdle();

    // Pull the whole voxel buffer down once — ~1200 sample points, and a
    // blocking map each would be far slower than one copy.
    std::vector<uint32_t> vox(kNumChunks * (size_t)kChunkVol);
    {
      const uint64_t bytes = (uint64_t)vox.size() * 4;
      wgpu::Buffer st = CreateBuffer(ctx.device, bytes,
                                     wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                     "pondRead");
      wgpu::CommandEncoder e = ctx.device.CreateCommandEncoder();
      e.CopyBufferToBuffer(world.voxels, 0, st, 0, bytes);
      wgpu::CommandBuffer c = e.Finish();
      ctx.queue.Submit(1, &c);
      wgpu::Future f = st.MapAsync(
          wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::WaitAnyOnly,
          [&](wgpu::MapAsyncStatus s2, wgpu::StringView) {
            if (s2 == wgpu::MapAsyncStatus::Success) {
              std::memcpy(vox.data(), st.GetConstMappedRange(0, bytes), bytes);
              st.Unmap();
            }
          });
      ctx.instance.WaitAny(f, UINT64_MAX);
    }
    auto readCell = [&](int x, int y, int z) {
      return vox[World::SlotCellIndex({x, y, z})] & 0xFFFu;
    };
    // Surface layer (y+kDepth-1): rim ring touches the bank and counts 2-3;
    // the middle 5x5 only has air above and counts 1. Rim must be visibly
    // ahead.
    const int surf = py + kDepth - 1;
    uint32_t edgeIce = 0, edgeN = 0, midIce = 0, midN = 0;
    for (int z = -R; z <= R; z++)
      for (int x = -R; x <= R; x++) {
        bool edge = (x == -R || x == R || z == -R || z == R);
        bool mid = (std::abs(x) <= 2 && std::abs(z) <= 2);
        if (!edge && !mid) continue;
        uint32_t m = readCell(px + x, surf, pz + z);
        if (edge) { edgeN++; if (m == (uint32_t)ii) edgeIce++; }
        else      { midN++;  if (m == (uint32_t)ii) midIce++; }
      }
    // The hard gate, asserted directly on the final state rather than by
    // sampling a rate: NO ice voxel anywhere in the pond may be one that had
    // zero non-water neighbours. Equivalently — since ice only ever replaces
    // water in place — every ice voxel must still touch something that is not
    // water. An ice voxel fully surrounded by water is a voxel that froze at
    // count 0, which the rule forbids outright.
    //
    // This is what makes the check decisive. A rate comparison cannot separate
    // "the gate works and the front crept down from the surface" from "the
    // gate is ignored" — both land near the same percentage. The invariant
    // can: it is violated by exactly one of those two.
    uint32_t violations = 0, iceTotal = 0;
    for (int y = 0; y < kDepth; y++)
      for (int z = -R; z <= R; z++)
        for (int x = -R; x <= R; x++) {
          if (readCell(px + x, py + y, pz + z) != (uint32_t)ii) continue;
          iceTotal++;
          const int d[6][3] = {{0,-1,0},{0,1,0},{1,0,0},{-1,0,0},{0,0,1},{0,0,-1}};
          bool touchesNonWater = false;
          for (auto& o : d)
            if (readCell(px + x + o[0], py + y + o[1], pz + z + o[2]) != (uint32_t)wi)
              touchesNonWater = true;
          if (!touchesNonWater) violations++;
        }
    // Rim ahead of the middle by a clear margin, and no voxel froze in the
    // interior of open water. Both halves matter: the gradient alone would
    // pass a rule that froze everything, the invariant alone would pass one
    // that never fired.
    bool gradient = edgeN > 0 && midN > 0 && edgeIce * midN > 2 * midIce * edgeN;
    pondOk = gradient && violations == 0 && edgeIce > 0;
    std::printf("pond freeze: %s (rim %u/%u vs middle %u/%u ice at 250 night "
                "ticks; %u ice voxels, %u frozen with 0 non-water neighbours)\n",
                pondOk ? "PASS" : "FAIL", edgeIce, edgeN, midIce, midN,
                iceTotal, violations);
    SetCurrentTuning(saved);
  }

  // ---- the sun dries spread water, not bodies of water -----------------------
  // The mirror of the pond-freeze test above, and it asserts the thing that
  // actually went wrong: evaporation used to be a flat chance on any sunlit
  // surface cell, so a pond boiled off from its whole top face in seconds.
  // The fix is scaleByNeighbors with minCount 4 — a cell must have >= 4
  // non-water face neighbours before the sun can take it.
  //
  // A rate comparison would be weak here for the same reason it was for
  // freezing, so this asserts the two ENDS of the rule as hard invariants on
  // the final state:
  //
  //   1. A pond does NOT shrink. Its surface counts 1 non-water neighbour
  //      (the air above), which is below minCount, so after thousands of
  //      sunlit ticks every last surface cell must still be water. One
  //      missing cell means the gate is not being applied.
  //   2. Isolated droplets DO go. A single water voxel sitting on stone
  //      counts 5-6 non-water neighbours and must evaporate.
  //
  // Together they pin the rule from both sides: (1) alone passes a rule that
  // never fires, (2) alone passes the old always-fires rule.
  bool evapOk = false;
  {
    auto matId = [&](const char* n) {
      for (size_t i = 0; i < mats.size(); i++)
        if (mats[i].name == n) return (int)i;
      return -1;
    };
    const int wi = matId("water"), si = matId("stone");
    // Pin the cycle at noon so the day-gated rule is at full strength every
    // tick (minLight 120 needs a high sun).
    Tuning noon = CurrentTuning();
    noon.dayNight.freeze = 1;
    noon.dayNight.freezePhase = 32768;  // 32768 = noon
    Tuning saved = CurrentTuning();
    SetCurrentTuning(noon);

    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();

    // Left: a walled pond, 3 deep, open to the sky. Right: a row of single
    // water voxels on a stone shelf, spaced 3 apart so none touches another
    // (spacing matters — two adjacent droplets would each count a water
    // neighbour and drop toward the gate).
    const int px = 80, py = 120, pz = 96, R = 8, kDepth = 3;
    const int dx = 120, kDrops = 12;
    std::vector<CellOp> scene;
    auto put = [&](int x, int y, int z, int m) {
      uint32_t state = (m == wi) ? 7u : 0u;  // liquids are born full
      scene.push_back({World::SlotCellIndex({x, y, z}),
                       (uint32_t)((m & 0xFFF) | (state << 12))});
    };
    for (int z = -R - 1; z <= R + 1; z++)
      for (int x = -R - 1; x <= R + 1; x++) {
        put(px + x, py - 1, pz + z, si);  // floor
        bool rim = (x < -R || x > R || z < -R || z > R);
        for (int y = 0; y < kDepth; y++) put(px + x, py + y, pz + z, rim ? si : wi);
        for (int y = kDepth; y < kDepth + 3; y++) put(px + x, py + y, pz + z, 0);
      }
    for (int i = 0; i < kDrops; i++) {
      const int x = dx + i * 3;
      put(x, py - 1, pz, si);            // shelf under the droplet
      put(x, py, pz, wi);                // the droplet itself
      for (int y = 1; y < 4; y++) put(x, py + y, pz, 0);  // open sky above
    }
    uint32_t et = 1;
    SubmitTick(ctx, world, sim, et, kDefaultSeed, {}, {}, scene, false,
               {6, 7, 6}, false, false);
    ctx.WaitIdle();

    // Long enough that a droplet at ~2-3 per-mille is overwhelmingly likely to
    // have gone (P(survive) < 1e-3 at 2500 ticks), and long enough that the
    // old flat 2 per-mille rule would have stripped the pond surface many
    // times over.
    for (uint32_t t = 2; t <= 2500; t++)
      SubmitTick(ctx, world, sim, ++et, kDefaultSeed, {}, {}, {}, false,
                 {6, 7, 6}, false, false);
    ctx.WaitIdle();

    std::vector<uint32_t> vox(kNumChunks * (size_t)kChunkVol);
    {
      const uint64_t bytes = (uint64_t)vox.size() * 4;
      wgpu::Buffer st = CreateBuffer(ctx.device, bytes,
                                     wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                     "evapRead");
      wgpu::CommandEncoder e = ctx.device.CreateCommandEncoder();
      e.CopyBufferToBuffer(world.voxels, 0, st, 0, bytes);
      wgpu::CommandBuffer c = e.Finish();
      ctx.queue.Submit(1, &c);
      wgpu::Future f = st.MapAsync(
          wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::WaitAnyOnly,
          [&](wgpu::MapAsyncStatus s2, wgpu::StringView) {
            if (s2 == wgpu::MapAsyncStatus::Success) {
              std::memcpy(vox.data(), st.GetConstMappedRange(0, bytes), bytes);
              st.Unmap();
            }
          });
      ctx.instance.WaitAny(f, UINT64_MAX);
    }
    auto readCell = [&](int x, int y, int z) {
      return vox[World::SlotCellIndex({x, y, z})] & 0xFFFu;
    };

    // (1) The pond's surface layer must be intact — every cell still water.
    const int surf = py + kDepth - 1;
    uint32_t surfN = 0, surfWater = 0;
    for (int z = -R; z <= R; z++)
      for (int x = -R; x <= R; x++) {
        surfN++;
        if (readCell(px + x, surf, pz + z) == (uint32_t)wi) surfWater++;
      }
    // (2) The droplets must be gone.
    uint32_t dropsLeft = 0;
    for (int i = 0; i < kDrops; i++)
      if (readCell(dx + i * 3, py, pz) == (uint32_t)wi) dropsLeft++;

    evapOk = surfWater == surfN && dropsLeft == 0;
    std::printf("evaporation: %s (pond surface %u/%u water after 2500 noon "
                "ticks, %u/%d isolated droplets left)\n",
                evapOk ? "PASS" : "FAIL", surfWater, surfN, dropsLeft, kDrops);
    SetCurrentTuning(saved);
  }

  // ---- blood stains what it touches, and then goes to sleep ------------------
  // Staining writes the voxel word's spare bits (kStain*, world.h) from the
  // liquid movement path in sim_step.wgsl. Three separate things have to hold,
  // and none of them is visible to the hash test:
  //
  //   1. It HAPPENS — stone under a pool of blood ends up carrying a stain of
  //      the right type. (A rule that silently never fires still hashes fine.)
  //   2. It TERMINATES — the chunk goes back to sleep. This is the rule-2 risk
  //      and the one that would not show up until a level is full of gore: a
  //      pool of blood sits on stone forever, so a naive "keep me awake while
  //      I'm touching something stainable" would pin those chunks awake for
  //      the rest of the session. doStaining only holds the chunk while there
  //      is UNSTAINED surface left in reach, and this is what proves it.
  //   3. It stays BOUNDED — stain amounts saturate at kStainAmtMax rather than
  //      overflowing into the neighbouring bits of the word (which would
  //      corrupt the tick-stamp and, one bit further, the material id).
  bool stainOk = false;
  {
    auto matId = [&](const char* n) {
      for (size_t i = 0; i < mats.size(); i++)
        if (mats[i].name == n) return (int)i;
      return -1;
    };
    const int bi = matId("blood"), si = matId("stone");
    const uint32_t stainType = mats[bi].gpu.stainPack & kStainPackTypeMask;

    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();

    // A stone basin with blood poured into it, in open air well clear of the
    // terrain. The floor and walls are what should end up stained.
    const int px = 96, py = 120, pz = 96, R = 6;
    std::vector<CellOp> pool;
    auto put = [&](int x, int y, int z, int m) {
      uint32_t state = (m == bi) ? 7u : 0u;  // liquids are born full
      pool.push_back({World::SlotCellIndex({x, y, z}),
                      (uint32_t)((m & 0xFFF) | (state << 12))});
    };
    for (int z = -R - 1; z <= R + 1; z++)
      for (int x = -R - 1; x <= R + 1; x++) {
        put(px + x, py - 1, pz + z, si);  // floor
        bool rim = (x < -R || x > R || z < -R || z > R);
        for (int y = 0; y < 2; y++) put(px + x, py + y, pz + z, rim ? si : bi);
        for (int y = 2; y < 5; y++) put(px + x, py + y, pz + z, 0);
      }
    uint32_t st = 1;
    SubmitTick(ctx, world, sim, st, kDefaultSeed, {}, {}, pool, false,
               {6, 7, 6}, false, false);
    ctx.WaitIdle();
    // Run until the blood has DRIED AWAY, not merely until it has finished
    // staining. Blood carries an unconditional decay rule ("blood dries away",
    // reactions.json) at 8 per-mille, which correctly holds its chunks awake
    // for as long as any blood is left — so a shorter run would measure a pool
    // that is still mid-evaporation and say nothing about sleep.
    //
    // Waiting for the dry-out is the stronger test anyway: it asserts that the
    // STAIN OUTLIVES THE LIQUID. That is the whole point of putting stain in
    // the voxel word rather than deriving it from what is standing there — the
    // mark on the floor has to survive the blood evaporating off it, and it
    // has to do so without keeping the chunk awake.
    // 8 per-mille gives a half-life of ~87 ticks; 4000 is ~46 half-lives.
    const uint32_t kDryTicks = 4000;
    for (uint32_t t = 2; t <= kDryTicks; t++)
      SubmitTick(ctx, world, sim, ++st, kDefaultSeed, {}, {}, {}, false,
                 {6, 7, 6}, false, false);
    ctx.WaitIdle();

    std::vector<uint32_t> vox(kNumChunks * (size_t)kChunkVol);
    {
      const uint64_t bytes = (uint64_t)vox.size() * 4;
      wgpu::Buffer sb = CreateBuffer(ctx.device, bytes,
                                     wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                     "stainRead");
      wgpu::CommandEncoder e = ctx.device.CreateCommandEncoder();
      e.CopyBufferToBuffer(world.voxels, 0, sb, 0, bytes);
      wgpu::CommandBuffer c = e.Finish();
      ctx.queue.Submit(1, &c);
      wgpu::Future f = sb.MapAsync(
          wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::WaitAnyOnly,
          [&](wgpu::MapAsyncStatus s2, wgpu::StringView) {
            if (s2 == wgpu::MapAsyncStatus::Success) {
              std::memcpy(vox.data(), sb.GetConstMappedRange(0, bytes), bytes);
              sb.Unmap();
            }
          });
      ctx.instance.WaitAny(f, UINT64_MAX);
    }

    // Count stained floor voxels, and check every stain in the world is
    // well-formed: right type, amount within the field, and never on air.
    uint32_t stainedFloor = 0, floorN = 0, badStain = 0, consumed = 0;
    for (int z = -R; z <= R; z++)
      for (int x = -R; x <= R; x++) {
        floorN++;
        uint32_t w = vox[World::SlotCellIndex({px + x, py - 1, pz + z})];
        if ((w & 0xFFFu) == 0u) { consumed++; continue; }  // eaten by the stain
        if (VoxStainType(w) == stainType && VoxStainAmt(w) > 0) stainedFloor++;
      }
    for (size_t i = 0; i < vox.size(); i++) {
      uint32_t w = vox[i];
      uint32_t type = VoxStainType(w), amt = VoxStainAmt(w);
      if (type == 0 && amt == 0) continue;
      // A stain must have both halves, name a registered type, fit the field,
      // and sit on actual matter.
      if (type != stainType || amt == 0 || amt > kStainAmtMax ||
          (w & 0xFFFu) == 0u) {
        badStain++;
      }
    }

    // How much blood is left? The sleep assertion only means anything once the
    // pool has actually dried, so report it rather than assuming.
    uint32_t bloodLeft = 0;
    for (size_t i = 0; i < vox.size(); i++)
      if ((vox[i] & 0xFFFu) == (uint32_t)bi) bloodLeft++;

    uint32_t stainActive = ReadActiveChunksSync(ctx, world, sim);
    // Four assertions, each covering a different way this could be broken:
    //   covered   — the stain HAPPENED (a rule that never fires still hashes)
    //   badStain  — every stain is well-formed and none landed on air, so the
    //               packing never overflowed into the stamp or material bits
    //   bloodLeft — the pool really did dry, so the sleep check below is
    //               measuring a settled world and not a mid-reaction one
    //   active    — and the stained floor SLEEPS. This is the rule-2 half: a
    //               stain is permanent, so if it held its chunk awake the way
    //               a naive "am I touching something stainable" rule would,
    //               every gore-soaked chunk would stay awake for the session.
    //
    // bloodLeft is checked against a small threshold, not zero. A handful of
    // isolated voxels can settle in a chunk that then goes to sleep with no
    // neighbour left to wake it, and a sleeping chunk does not run reactions —
    // so their decay is simply paused until something disturbs them. That is
    // the sleep rule working as designed (cost scales with activity), not a
    // stuck reaction, and it is exactly why `active` is the assertion that
    // matters here rather than a demand that every last voxel evaporate.
    bool covered = stainedFloor + consumed > floorN / 2 && stainedFloor > 0;
    const uint32_t kBloodDregs = 16;  // isolated voxels in sleeping chunks
    stainOk = covered && badStain == 0 && bloodLeft <= kBloodDregs &&
              stainActive < 32;
    std::printf("blood stain: %s (%u/%u floor stained, %u consumed, %u malformed, "
                "%u blood left, %u chunks active after %u ticks)\n",
                stainOk ? "PASS" : "FAIL", stainedFloor, floorN, consumed,
                badStain, bloodLeft, stainActive, kDryTicks);
  }

  bool fullOk = false;
  // ---- flung liquid lands FULL (the anti-gelatin invariant) --------------
  // A liquid that rejoins the grid from flight must be born at full fullness,
  // exactly like one a brush paints (sim_mutate.wgsl: "liquids are born
  // full"). This is a LOOK invariant with no visible symptom in any other
  // check: the state nibble is fullness, the renderer builds blood's smooth
  // surface from that as a density field, and a spawn that leaves the nibble
  // at 0 lands at 1/8 density. The hash still matches, the world still
  // settles, nothing fails — the blood just renders as separately shaded
  // translucent cubes (the "gelatin" look shadeViscous exists to avoid).
  //
  // So this asserts the nibble directly rather than trusting a screenshot.
  {
    auto matId = [&](const char* n) {
      for (size_t i = 0; i < mats.size(); i++)
        if (mats[i].name == n) return (int)i;
      return -1;
    };
    const int bloodMat = matId("blood");

    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();

    // A stone slab in open air, and blood particles dropped onto it — the
    // same coordinates and window the stain test uses, which are known to sit
    // inside the residency window with nothing else going on around them.
    const int px = 96, py = 120, pz = 96;
    std::vector<CellOp> slab;
    for (int z = -3; z <= 3; z++)
      for (int x = -3; x <= 3; x++)
        slab.push_back({World::SlotCellIndex({px + x, py, pz + z}),
                        (uint32_t)(matId("stone") & 0xFFF)});

    std::vector<ParticleSpawn> drops;
    for (int i = 0; i < 25 && bloodMat > 0; i++) {
      ParticleSpawn s{};
      s.px = (px - 2 + (i % 5)) * 256 + 128;
      s.py = (py + 6) * 256 + 128;
      s.pz = (pz - 2 + (i / 5)) * 256 + 128;
      s.vx = 0; s.vy = -128; s.vz = 0;
      s.payload = (uint32_t)bloodMat;  // state nibble deliberately left 0
      s.flags = kPFlagAlive;
      drops.push_back(s);
    }
    uint32_t ft = 20000;
    for (int i = 0; i < 40; i++) {
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {},
                 i == 0 ? slab : std::vector<CellOp>{}, false, {6, 7, 6},
                 /*wantReadback=*/false, /*particlesActive=*/true,
                 i == 1 ? drops : std::vector<ParticleSpawn>{});
      ctx.WaitIdle();
      ctx.ProcessEvents();
    }

    std::vector<uint32_t> fv(kNumChunks * (size_t)kChunkVol);
    {
      const uint64_t bytes = (uint64_t)fv.size() * 4;
      wgpu::Buffer sb = CreateBuffer(
          ctx.device, bytes,
          wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst, "fullRead");
      wgpu::CommandEncoder e = ctx.device.CreateCommandEncoder();
      e.CopyBufferToBuffer(world.voxels, 0, sb, 0, bytes);
      wgpu::CommandBuffer c = e.Finish();
      ctx.queue.Submit(1, &c);
      wgpu::Future f = sb.MapAsync(
          wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::WaitAnyOnly,
          [&](wgpu::MapAsyncStatus s2, wgpu::StringView) {
            if (s2 == wgpu::MapAsyncStatus::Success) {
              std::memcpy(fv.data(), sb.GetConstMappedRange(0, bytes), bytes);
              sb.Unmap();
            }
          });
      ctx.instance.WaitAny(f, UINT64_MAX);
    }
    // Count landed blood and how much of it is at less than full fullness.
    // Blood FLOWS once it lands, and flowing splits a cell's fullness across
    // its neighbours, so partial cells are expected at the spreading edge —
    // the bug being caught is "everything lands at the 1/8 floor", so the
    // assertion is that the maximum reached full, not that every cell did.
    uint32_t landed = 0, maxState = 0;
    for (size_t i = 0; i < fv.size(); i++) {
      if ((fv[i] & 0xFFFu) != (uint32_t)bloodMat) continue;
      landed++;
      maxState = std::max(maxState, (fv[i] >> 12) & 0xFu);
    }
    fullOk = bloodMat > 0 && landed > 0 && maxState == 7;
    std::printf("flung liquid fullness: %s (%u blood voxels landed, max "
                "fullness %u/7 - 0 would render as gelatin cubes)\n",
                fullOk ? "PASS" : "FAIL", landed, maxState);
  }

  // sim perf: worst-case-ish activity (brushes + explosions + particles),
  // synchronous timing
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  for (uint32_t t = 1; t <= 60; t++)  // warm up with heavy activity
    SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t),
               SelftestExps(t, kDefaultSeed), {}, false, {8, 3, 8}, false,
               SelftestParticlesActive(t));
  ctx.WaitIdle();
  double t0 = NowSec();
  for (uint32_t t = 61; t <= 160; t++)
    SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t),
               SelftestExps(t, kDefaultSeed), {}, false, {8, 3, 8}, false,
               SelftestParticlesActive(t));
  ctx.WaitIdle();
  double simMs = (NowSec() - t0) * 1000.0 / 100.0;
  std::printf("sim: %.2f ms/tick (active scene, includes submit overhead)\n", simMs);

  // far-field cascades: fill fully (render-only, ~6 dispatches) so the render
  // benchmark + screenshot cover the far march with real data.
  //
  // This drain is also the gate for the phase-3 adaptive fog radius: the queue
  // starts full (nothing filled) and ends empty, so SafeRadiusMeters must
  // start at the cold-start floor, never go BACKWARDS while draining (fog that
  // re-closes as data arrives would pop the wrong way), and finish at the full
  // outermost half-extent.
  bool fogOk = false;
  {
    FarField far;
    far.Init(&world);
    far.FullRefill({108 >> 4, 122 >> 4, 108 >> 4});
    const float coldR = far.SafeRadiusMeters();
    float prevR = coldR;
    bool monotone = true;
    uint32_t n;
    while ((n = far.PrepareTick(ctx.queue)) > 0) {
      TickParams tp{0, kDefaultSeed, 0, 0};
      tp.farCount = n;
      ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
      wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      sim.EncodeFarFill(enc, n);
      wgpu::CommandBuffer cmd = enc.Finish();
      ctx.queue.Submit(1, &cmd);
      float r = far.SafeRadiusMeters();
      if (r < prevR) monotone = false;
      prevR = r;
    }
    ctx.WaitIdle();
    // cold start: level 1 still pending -> only the residency window is trusted
    const float wantCold = (float)(kWorldN >> 1) * kVoxelMeters;
    const float wantFull =
        (float)(kWorldN >> 1) * (float)(1u << kFarLevels) * kVoxelMeters;
    fogOk = std::abs(coldR - wantCold) < 1e-3f && monotone &&
            std::abs(prevR - wantFull) < 1e-3f;
    std::printf("far fog radius: %s (cold %.1f m -> filled %.1f m, monotone=%d)\n",
                fogOk ? "PASS" : "FAIL", coldR, prevR, monotone ? 1 : 0);
  }

  // far-field phase 2 (edits at distance): paint a distinctive block into a
  // resident region well above terrain, run a few ticks, and verify the
  // cascade cells covering it now read that material. Proves the dirty-driven
  // downsample (worldgen.wgsl `fardown`) actually reaches farVox — without it
  // the cascades still hold pristine procgen (air up here) and the edit
  // vanishes the moment the player walks away.
  bool farDownOk = false;
  {
    // The window origin is {0,0,0} in this section, so the paint site is
    // resident; (140, 200, 140) is open air well above the hills and canopy.
    const IVec3 c{140, 200, 140};
    // One level-1 cell spans 2^(1+kFarShiftBase) fine voxels, sampled at its
    // center. The brush radius below must cover the sample points of the full
    // 3x3x3 cell block around the paint: the farthest one sits
    // 1.5 * cellsize - (cellsize/2 - offset) away per axis — radius 12 covers
    // it at the current 4-voxel cells (corner sample distance^2 = 108 < 144).
    const int farShift1 = (int)(1 + kFarShiftBase);
    auto farByte = [&](IVec3 cc) {
      // farVoxByteIndex(1, cc) on the FAR grid (kFarN masks, chunk-major)
      auto wrapv = [](int v) { return (uint32_t)(v & (int)(kFarN - 1)); };
      uint32_t x = wrapv(cc.x), y = wrapv(cc.y), z = wrapv(cc.z);
      uint32_t ch = ((z >> 4) * kFarNChunk + (y >> 4)) * kFarNChunk + (x >> 4);
      uint32_t lo = ((z & 15) * kChunk + (y & 15)) * kChunk + (x & 15);
      uint64_t bi = (uint64_t)ch * kChunkVol + lo;
      wgpu::Buffer staging = CreateBuffer(
          ctx.device, 4, wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
          "farVoxRead");
      wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      enc.CopyBufferToBuffer(world.farVox, bi & ~3ull, staging, 0, 4);
      wgpu::CommandBuffer cmd = enc.Finish();
      ctx.queue.Submit(1, &cmd);
      uint32_t word = 0;
      wgpu::Future f = staging.MapAsync(
          wgpu::MapMode::Read, 0, 4, wgpu::CallbackMode::WaitAnyOnly,
          [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
            if (status == wgpu::MapAsyncStatus::Success) {
              std::memcpy(&word, staging.GetConstMappedRange(0, 4), 4);
              staging.Unmap();
            }
          });
      ctx.instance.WaitAny(f, UINT64_MAX);
      return (word >> ((bi & 3ull) * 8)) & 0xFFu;
    };
    auto scan = [&](uint32_t want) {
      uint32_t n = 0;
      for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++)
            if (farByte({(c.x >> farShift1) + dx, (c.y >> farShift1) + dy,
                         (c.z >> farShift1) + dz}) == want)
              n++;
      return n;
    };
    // before: the sieve filled these cells from pristine procgen — open sky,
    // so all 27 must read air. Guards the gate against passing vacuously.
    uint32_t airBefore = scan(kMatAir);
    for (uint32_t t = 1; t <= 4; t++) {
      std::vector<BrushOp> ops;
      if (t == 1) ops.push_back({c.x, c.y, c.z, 12, kMatGlass, 1u /*overwrite*/, 0, 0});
      SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, {}, false, {8, 12, 8},
                 false, false);
    }
    ctx.WaitIdle();
    uint32_t glassAfter = scan(kMatGlass);
    farDownOk = airBefore == 27 && glassAfter == 27;
    std::printf("far downsample: %s (%u/27 level-1 cells air before the edit, "
                "%u/27 read glass after)\n",
                farDownOk ? "PASS" : "FAIL", airBefore, glassAfter);
  }

  // render perf: offscreen 1080p
  const uint32_t W = 1920, H = 1080;
  wgpu::TextureDescriptor td{};
  td.size = {W, H, 1};
  td.format = wgpu::TextureFormat::RGBA8Unorm;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  wgpu::Texture offscreen = ctx.device.CreateTexture(&td);
  wgpu::TextureView view = offscreen.CreateView();

  Camera cam;
  cam.yaw = 0.785f;   // look out over the forest from above the canopy
  cam.pitch = -0.35f;
  // Anchored to the local ground, not a fixed y: terrain now reaches y140 and
  // a hardcoded eye height buried the camera inside a hillside (the render
  // benchmark then timed a screenful of dirt, and the screenshot showed one).
  // 20 m up clears the canopy on any ridge.
  Vec3 eye{108, (float)(World::TerrainHeight(108, 108, kDefaultSeed) + 120), 108};

  double bestFrameMs = 1e9;
  for (int pass = 0; pass < 2; pass++) {
    bool shadows = pass == 0;
    ctx.WaitIdle();
    double r0 = NowSec();
    for (int i = 0; i < 60; i++) {
      WriteRenderParams(ctx.queue, world, eye, cam, (float)W / H, shadows, 0);
      wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      wgpu::RenderPassEncoder rp =
          sim.BeginRenderPass(enc, view, wgpu::TextureFormat::RGBA8Unorm, W, H);
      sim.DrawWorld(rp);
      sim.DrawParticles(rp);
      rp.End();
      wgpu::CommandBuffer cmd = enc.Finish();
      ctx.queue.Submit(1, &cmd);
    }
    ctx.WaitIdle();
    double ms = (NowSec() - r0) * 1000.0 / 60.0;
    std::printf("render 1080p %s: %.2f ms/frame (%.0f fps)\n",
                shadows ? "shadows on " : "shadows off", ms, 1000.0 / ms);
    bestFrameMs = std::min(bestFrameMs, ms);
  }

  // Read the offscreen target back and write it out. Shared by the standard
  // screenshot and the far-field view below.
  auto grab = [&](const char* path) {
    wgpu::Buffer shot = CreateBuffer(ctx.device, (uint64_t)W * H * 4,
                                     wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                     "screenshot");
    wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo srcT{};
    srcT.texture = offscreen;
    wgpu::TexelCopyBufferInfo dstB{};
    dstB.buffer = shot;
    dstB.layout.bytesPerRow = W * 4;
    dstB.layout.rowsPerImage = H;
    wgpu::Extent3D ext{W, H, 1};
    enc.CopyTextureToBuffer(&srcT, &dstB, &ext);
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx.queue.Submit(1, &cmd);
    std::vector<uint8_t> pixels(W * H * 4);
    bool got = false;
    wgpu::Future f = shot.MapAsync(
        wgpu::MapMode::Read, 0, pixels.size(), wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
          if (status == wgpu::MapAsyncStatus::Success) {
            std::memcpy(pixels.data(), shot.GetConstMappedRange(0, pixels.size()),
                        pixels.size());
            shot.Unmap();
            got = true;
          }
        });
    ctx.instance.WaitAny(f, UINT64_MAX);
    if (got && WriteBmp(path, pixels, W, H)) std::printf("wrote %s\n", path);
  };

  // screenshot
  grab("screenshot.bmp");

  // far-field view (plan phase 3): the standard screenshot looks DOWN at the
  // near forest, where cascade data barely appears. This one puts the camera
  // high with a near-level horizon so the cascade bands fill most of the
  // frame — the only way to actually see the level seams the phase-3 dither
  // targets, and the swiss-cheese check for coarse-level cave sampling.
  {
    Camera farCam;
    farCam.yaw = 0.785f;      // look diagonally out over open terrain
    farCam.pitch = -0.20f;    // shallow: horizon high in the frame
    // Well above the canopy on purpose. At tree height a single trunk in front
    // of the lens fills the frame and the shot shows no far field at all —
    // which is exactly what happened when worldgen grew taller trees.
    Vec3 farEye{140, 220, 140};
    WriteRenderParams(ctx.queue, world, farEye, farCam, (float)W / H, true, 0);
    wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    wgpu::RenderPassEncoder rp =
        sim.BeginRenderPass(enc, view, wgpu::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    rp.End();
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx.queue.Submit(1, &cmd);
    ctx.WaitIdle();
    grab("screenshot_far.bmp");
  }

  // ground-level view (phase 4): eye height on the terrain, horizon in frame.
  // This is the player's actual experience of the distance work — the elevated
  // shots can look fine while the first-person seam/fog/shading is still
  // wrong, so judge distance changes against THIS one.
  {
    Camera gCam;
    gCam.yaw = 0.785f;
    gCam.pitch = -0.02f;
    Vec3 gEye{108, (float)(World::TerrainHeight(108, 108, kDefaultSeed) + 28), 108};
    WriteRenderParams(ctx.queue, world, gEye, gCam, (float)W / H, true, 0);
    wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    wgpu::RenderPassEncoder rp =
        sim.BeginRenderPass(enc, view, wgpu::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    rp.End();
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx.queue.Submit(1, &cmd);
    ctx.WaitIdle();
    grab("screenshot_ground.bmp");
  }

  // player walk test: drop onto terrain through the real async-mirror path
  bool walkOk = false;
  {
    std::vector<uint32_t> classOf;
    for (auto& m : mats) classOf.push_back(m.gpu.klass);
    Player player;
    player.fly = false;
    int h = World::TerrainHeight(140, 140, kDefaultSeed);
    player.pos = Vec3{140.5f, (float)(h + 30), 140.5f};
    auto kindAt = [&](IVec3 c) { return world.KindAt(c, classOf); };
    uint32_t t = 200;
    for (int i = 0; i < 240; i++) {
      IVec3 pc{ifloor(player.pos.x) / (int)kChunk, ifloor(player.pos.y) / (int)kChunk,
               ifloor(player.pos.z) / (int)kChunk};
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false, pc, true, false);
      ctx.WaitIdle();
      ctx.ProcessEvents();  // deliver the mirror
      player.Update(1.0f / 30.0f, PlayerInput{}, Vec3{1, 0, 0}, Vec3{0, 0, 1},
                    Vec3{1, 0, 0}, kindAt);
      if (player.grounded) break;
    }
    float feet = player.pos.y - Player::kHalfY;
    walkOk = player.grounded && std::abs(feet - (float)(h + 1)) < 4.0f;
    std::printf("player walk: %s (grounded=%d feet y=%.1f, terrain h=%d)\n",
                walkOk ? "PASS" : "FAIL", player.grounded ? 1 : 0, feet, h);
  }

  // M6 debris: build a stone arm held up by one pillar, blast the pillar,
  // and require the arm to (a) get detected as an island, (b) fall as a Jolt
  // body onto marching-cubes terrain, (c) go to sleep.
  bool debrisOk = false;
  {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    int h = World::TerrainHeight(60, 60, kDefaultSeed);
    uint32_t t = 2000;
    uint32_t bodiesSeen = 0;

    for (int i = 0; i < 420; i++) {
      std::vector<BrushOp> ops;
      if (i < 8) {
        // pillar: stacked stone spheres; arm: a bar of spheres at the top
        ops.push_back({60, h + 2 + i * 3, 60, 2, kMatStone, 1, 0, 0});
        if (i < 3) ops.push_back({60 + 6 * (i + 1), h + 22, 60, 3, kMatStone, 1, 0, 0});
        ops.push_back({60, h + 22, 60, 3, kMatStone, 1, 0, 0});
      }
      std::vector<ExplosionOp> exps;
      if (i == 40) {
        exps.push_back({60, h + 10, 60, 7, 500, 0, 0, 0});
        debris.AddDestructionEvent(t + 1, {50, h, 50}, {84, h + 26, 70});
      }

      debris.QueueSupportEvents(world.Snap());
      std::vector<CellOp> cellOps;
      std::vector<ParticleSpawn> spawns;
      debris.PreTick(t + 1, world, cellOps, spawns);
      ++t;
      SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, exps, cellOps, false,
                 {3, (h + 10) / 16, 3}, true, i >= 40 && i < 380, spawns);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      phys.Step(kTickDt);
      debris.PostStep();
      bodiesSeen = std::max(bodiesSeen, debris.BodyCount());
    }
    uint32_t awake = debris.ActiveBodyCount();
    debrisOk = bodiesSeen >= 1 && awake == 0;
    std::printf("debris: %s (%u bodies spawned, %u awake after settling, "
                "%u events pending)\n",
                debrisOk ? "PASS" : "FAIL", bodiesSeen, awake,
                debris.PendingEvents());

    // visual proof: render the settled debris field to screenshot_debris.bmp
    if (debris.BodyCount() > 0) {
      std::vector<BodyVoxInst> inst;
      debris.BuildInstances(inst);
      ctx.queue.WriteBuffer(world.bodyInstances, 0, inst.data(),
                            inst.size() * sizeof(BodyVoxInst));
      std::vector<BodyXformGpu> xf;
      debris.BuildXforms(xf);
      ctx.queue.WriteBuffer(world.bodyXforms, 0, xf.data(),
                            xf.size() * sizeof(BodyXformGpu));

      const uint32_t W = 1280, H = 720;
      wgpu::TextureDescriptor td{};
      td.size = {W, H, 1};
      td.format = wgpu::TextureFormat::RGBA8Unorm;
      td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
      wgpu::Texture tex = ctx.device.CreateTexture(&td);
      Camera cam2;
      cam2.yaw = -2.356f;
      cam2.pitch = -0.32f;
      Vec3 eye{60.0f + 34, (float)h + 26, 60.0f + 34};
      WriteRenderParams(ctx.queue, world, eye, cam2, (float)W / H, true, 0);
      wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      wgpu::RenderPassEncoder rp = sim.BeginRenderPass(
          enc, tex.CreateView(), wgpu::TextureFormat::RGBA8Unorm, W, H);
      sim.DrawWorld(rp);
      sim.DrawParticles(rp);
      sim.DrawBodies(rp, debris.InstanceCount());
      rp.End();
      wgpu::Buffer shot = CreateBuffer(ctx.device, (uint64_t)W * H * 4,
                                       wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                       "debrisShot");
      wgpu::TexelCopyTextureInfo srcT{};
      srcT.texture = tex;
      wgpu::TexelCopyBufferInfo dstB{};
      dstB.buffer = shot;
      dstB.layout.bytesPerRow = W * 4;
      dstB.layout.rowsPerImage = H;
      wgpu::Extent3D ext{W, H, 1};
      enc.CopyTextureToBuffer(&srcT, &dstB, &ext);
      wgpu::CommandBuffer cmd = enc.Finish();
      ctx.queue.Submit(1, &cmd);
      std::vector<uint8_t> pixels(W * H * 4);
      bool got = false;
      wgpu::Future f = shot.MapAsync(
          wgpu::MapMode::Read, 0, pixels.size(), wgpu::CallbackMode::WaitAnyOnly,
          [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
            if (status == wgpu::MapAsyncStatus::Success) {
              std::memcpy(pixels.data(), shot.GetConstMappedRange(0, pixels.size()),
                          pixels.size());
              shot.Unmap();
              got = true;
            }
          });
      ctx.instance.WaitAny(f, UINT64_MAX);
      if (got && WriteBmp("screenshot_debris.bmp", pixels, W, H))
        std::printf("wrote screenshot_debris.bmp\n");
    }
  }

  // Milestone A prefab placement: a 48^3 stone cube (110,592 voxels) must
  // spread across ticks under the 16384/tick placer budget and land with
  // ZERO dropped voxels (exact count via chunk fetches).
  bool prefabOk = false;
  {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    Prefab pf;
    pf.name = "testcube";
    PrefabModel pm;
    pm.name = "cube";
    pm.size = {48, 48, 48};
    for (int z = 0; z < 48; z++)
      for (int y = 0; y < 48; y++)
        for (int x = 0; x < 48; x++)
          pm.voxels.push_back({(int16_t)x, (int16_t)y, (int16_t)z,
                               (uint16_t)kMatStone});
    pf.size = pm.size;
    pf.models.push_back(std::move(pm));

    PrefabPlacer placer;
    IVec3 lo{100, 150, 100}, hi;
    placer.Place(pf, lo, 0, true, mats, lo, hi);
    uint32_t t = 4000;
    int drainTicks = 0;
    while (placer.PendingCount() > 0 && drainTicks < 32) {
      std::vector<CellOp> cellOps;
      placer.PreTick(world, cellOps);
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, cellOps, false,
                 {8, 10, 8}, false, false);
      drainTicks++;
    }
    ctx.WaitIdle();

    // count the stamped voxels back out through the async fetch path
    uint32_t placedTick = t;
    uint64_t stone = 0;
    bool allFresh = false;
    for (int tries = 0; tries < 90 && !allFresh; tries++) {
      allFresh = true;
      stone = 0;
      for (int cz = 100 >> 4; cz <= 147 >> 4; cz++)
        for (int cy = 150 >> 4; cy <= 197 >> 4; cy++)
          for (int cx = 100 >> 4; cx <= 147 >> 4; cx++) {
            const CachedChunk* cc = world.Cached({cx, cy, cz});
            if (!cc || cc->version < placedTick) {
              world.RequestChunkFetch({cx, cy, cz});
              allFresh = false;
              continue;
            }
            for (uint32_t w : cc->voxels)
              if ((w & 0xFFFu) == kMatStone) stone++;
          }
      if (!allFresh) {
        SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
                   {8, 10, 8}, true, false);
        ctx.WaitIdle();
        ctx.ProcessEvents();
      }
    }
    prefabOk = allFresh && stone == 48ull * 48 * 48 && drainTicks >= 6;
    std::printf("prefab: %s (%llu / %llu voxels landed over %d ticks)\n",
                prefabOk ? "PASS" : "FAIL", (unsigned long long)stone,
                48ull * 48 * 48, drainTicks);
  }

  // Milestone B mobs: spawn the generated dummy on terrain — it must stand
  // and walk (kinematic limbs over cached-chunk ground), lose an arm to
  // Sever (joint destroyed, limb adopted as debris), die to a vital hit
  // (whole-body ragdoll), and every piece must go to sleep.
  bool mobOk = false;
  {
    debris.Reset();
    mobs.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    if (mobs.Defs().empty()) {
      std::printf("mob: FAIL (no mob defs — run scripts/gen_test_mob.py)\n");
    } else {
      // Select the dummy BY NAME and resolve limb indices by name too: mob
      // defs load in filename order, so adding assets/mobs/critter.* would
      // otherwise silently re-point this fixture at a different rig. The
      // loader also topologically sorts limbs, so positional indices are not
      // stable across sidecar edits either.
      int dummyDef = 0;
      for (size_t i = 0; i < mobs.Defs().size(); i++)
        if (mobs.Defs()[i].name == "dummy") dummyDef = (int)i;
      const MobDef& dd = mobs.Defs()[dummyDef];
      auto limbIndex = [&](const char* name) {
        for (size_t i = 0; i < dd.limbs.size(); i++)
          if (dd.limbs[i].name == name) return (int)i;
        return -1;
      };
      const int nLimbs = (int)dd.limbs.size();
      int h = World::TerrainHeight(140, 140, kDefaultSeed);
      uint32_t t = 6000;
      auto mobTick = [&](std::vector<BrushOp> ops) {
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

      uint64_t id = mobs.Spawn(dummyDef, {137, h + 1, 139});
      Vec3 spawnPos = mobs.MobOrigin(id);
      // Same forward-locomotion invariant the critter is held to (below): the
      // legacy scale-1 rig must keep walking along its facing, so a fix aimed
      // at one model can never silently reverse the other.
      Vec3 dPrev = spawnPos;
      float dAlong = 0.0f, dPath = 0.0f;
      for (int i = 0; i < 120; i++) {
        Vec3 face = mobs.MobFacing(id);
        mobTick({});
        Vec3 now = mobs.MobOrigin(id);
        Vec3 step{now.x - dPrev.x, 0, now.z - dPrev.z};
        dPrev = now;
        dAlong += step.x * face.x + step.z * face.z;
        dPath += std::sqrt(step.x * step.x + step.z * step.z);
      }
      bool dummyForward = dPath > 1.0f && dAlong > 0.5f * dPath;
      Vec3 walked = mobs.MobOrigin(id);
      float dist = (walked - spawnPos).len();
      // The mob wanders ~20 voxels over these 120 ticks, and terrain averages
      // ~0.34 voxels of fall per voxel travelled, so a healthy mob legitimately
      // ends up several voxels below where it started. The bound only has to
      // catch "fell through the world" / "stuck in the air", not honest walking
      // downhill — it was 6.0 when hills spanned 45 voxels and the mob grazed
      // it at exactly -6.0 once hills spanned 90.
      bool standing = mobs.IsAlive(id) && mobs.LimbBodyCount() == (uint32_t)nLimbs &&
                      std::abs(walked.y - (float)(h + 1)) < 16.0f;

      // sever arm.L
      uint32_t debrisBefore = debris.BodyCount();
      mobs.Sever(id, limbIndex("arm.L"));
      bool severed = mobs.LimbBodyCount() == (uint32_t)(nLimbs - 1) &&
                     debris.BodyCount() == debrisBefore + 1 && mobs.IsAlive(id);
      for (int i = 0; i < 60; i++) mobTick({});

      // vital hit: decapitation kills — remaining 5 limbs ragdoll into debris.
      // settle window covers the blood drying out (its chunks stay dirty
      // while wet, and terrain refreshes wake nearby bodies by design)
      mobs.Sever(id, limbIndex("head"));
      bool died = !mobs.IsAlive(id) || mobs.MobCount() == 0;
      // 2500, not 500: dismemberment now throws real blood voxels (gore.
      // severVoxels) on top of the wound drip, and wet blood keeps its chunks
      // dirty while it flows and soaks, which by design keeps waking the
      // bodies resting in it. Measured on the RTX 3060 Ti: awake=5 at 500
      // ticks, awake=0 by 2500. The assertion still tests that the scene
      // reaches full rest — this is a slower settle, not a leak, and shrinking
      // the blood to fit the old window would be testing the tuning instead of
      // the invariant.
      for (int i = 0; i < 2500; i++) mobTick({});
      uint32_t awake = debris.ActiveBodyCount();
      bool settled = awake == 0 && mobs.MobCount() == 0;

      mobOk = standing && severed && died && settled && dummyForward;
      std::printf(
          "mob: %s (stood=%d walked %.1f vox, forward %.1f/%.1f, sever=%d, "
          "death=%d, %u debris pieces, %u awake after settle)\n",
          mobOk ? "PASS" : "FAIL", standing ? 1 : 0, dist, dAlong, dPath,
          severed ? 1 : 0, died ? 1 : 0, debris.BodyCount(), awake);
      debris.Reset();
      mobs.Reset();

      // ---- Wave 2a: procedural gait + IK + clips on the critter rig ----
      // Per-tick INVARIANTS, not rate comparisons: (a) at most one gait group
      // swings at a time, which is the whole gait state machine; (b) some foot
      // is always planted, or the mob is airborne; (c) losing a leg silently
      // drops it from the schedule; (d) a non-fatal hit starts the flinch clip
      // and that clip eventually blends out to nothing.
      int critterDef = -1;
      for (size_t i = 0; i < mobs.Defs().size(); i++)
        if (mobs.Defs()[i].name == "critter") critterDef = (int)i;
      if (critterDef < 0) {
        std::printf("mob gait: SKIP (no critter def — run "
                    "scripts/gen_critter_mob.py)\n");
      } else {
        const MobDef& cd = mobs.Defs()[critterDef];
        auto critterLimb = [&](const char* nm) {
          for (size_t i = 0; i < cd.limbs.size(); i++)
            if (cd.limbs[i].name == nm) return (int)i;
          return -1;
        };
        uint64_t cid = mobs.Spawn(critterDef, {137, h + 1, 139});
        int maxSwing = 0, everSwung = 0, neverPlanted = 0;
        // FORWARD-LOCOMOTION CHECK. A mob must travel along the direction it
        // faces. Sample facing every tick and accumulate the dot product of
        // each tick's displacement with that tick's facing, so a mid-walk turn
        // (the critter turns 90 deg when blocked) can never make a
        // forward-walking mob look backward. A model authored nose-backwards
        // walks in reverse and this sum goes negative — that was a real bug.
        Vec3 prevPos = mobs.MobOrigin(cid);
        float alongFacing = 0.0f, pathLen = 0.0f;
        for (int i = 0; i < 150; i++) {
          Vec3 face = mobs.MobFacing(cid);
          mobTick({});
          int sw = mobs.SwingingFeet(cid);
          int pl = mobs.PlantedFeet(cid);
          if (sw > maxSwing) maxSwing = sw;
          if (sw > 0) everSwung++;
          if (pl == 0) neverPlanted++;
          Vec3 now = mobs.MobOrigin(cid);
          Vec3 step{now.x - prevPos.x, 0, now.z - prevPos.z};
          prevPos = now;
          alongFacing += step.x * face.x + step.z * face.z;
          pathLen += std::sqrt(step.x * step.x + step.z * step.z);
        }
        // Require the motion to be not merely forward-ish but essentially ALL
        // forward: a backwards model scores about -1 here, a correct one +1.
        bool walksForward = pathLen > 1.0f && alongFacing > 0.5f * pathLen;
        // groups are diagonal PAIRS, so up to 2 feet may swing together, but
        // never a third (that would mean two groups swinging at once)
        bool oneGroup = maxSwing <= 2;
        bool stepped = everSwung > 0;
        bool grounded = neverPlanted < 30;   // brief all-swing frames are ok

        // limb loss: sever a front-left leg; its chain must drop out entirely
        int beforeFeet = mobs.SwingingFeet(cid) + mobs.PlantedFeet(cid);
        mobs.Sever(cid, critterLimb("legU.FL"));
        for (int i = 0; i < 40; i++) mobTick({});
        int afterFeet = mobs.SwingingFeet(cid) + mobs.PlantedFeet(cid);
        bool legLost = mobs.IsAlive(cid) && afterFeet < beforeFeet;

        // flinch clip: a non-fatal hit on the torso starts "attack"
        uint64_t torso = mobs.LimbBody(cid, critterLimb("torso"));
        mobs.Damage(torso, 1.0f, mobs.MobOrigin(cid), 0.0f);
        int clipsNow = mobs.ActiveClips(cid);
        for (int i = 0; i < 40; i++) mobTick({});
        int clipsLater = mobs.ActiveClips(cid);
        // the clip must both START and eventually retire (blend-out works)
        bool clipOk = clipsNow >= 1 && clipsLater == 0;

        bool gaitOk = oneGroup && stepped && grounded && legLost && clipOk &&
                      walksForward;
        std::printf(
            "mob gait: %s (max %d feet swinging, stepped on %d/150 ticks, "
            "%d all-swing ticks, leg loss %d->%d feet, clip %d->%d, "
            "forward %.1f of %.1f vox travelled)\n",
            gaitOk ? "PASS" : "FAIL", maxSwing, everSwung, neverPlanted,
            beforeFeet, afterFeet, clipsNow, clipsLater, alongFacing, pathLen);
        if (!walksForward)
          std::printf("  critter walks BACKWARDS (displacement . facing = "
                      "%.2f over %.1f vox of path)\n",
                      alongFacing, pathLen);
        mobOk = mobOk && gaitOk;

        // ---- Wave 3: the microvoxel render pass actually draws ----
        // The critter is a "scale": 2 def, so its limbs emit NO cube instances
        // and must come entirely from microbody.wgsl. Render the same frame
        // twice — once with the micro pass, once without — and count differing
        // pixels. That proves three things at once with no depth readback: the
        // pass produced fragments, they survived the reversed-Z depth test
        // against the world raymarch (a pass writing depth behind the terrain
        // would change nothing), and the limbs are not ALSO being drawn by the
        // cube path (which would make both images identical).
        {
          const uint32_t W = 640, H = 360;
          wgpu::TextureDescriptor td{};
          td.size = {W, H, 1};
          td.format = wgpu::TextureFormat::RGBA8Unorm;
          td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;

          // Slot lists exactly as the frame loop builds them.
          std::vector<BodyXformGpu> xf;
          BuildBodyXforms(debris, mobs, nullptr, xf);
          if (!xf.empty())
            ctx.queue.WriteBuffer(world.bodyXforms, 0, xf.data(),
                                  xf.size() * sizeof(BodyXformGpu));
          std::vector<MicroBodyInstGpu> microInsts;
          BuildMicroInsts(debris, mobs, nullptr, microInsts);
          std::vector<BodyVoxInst> inst;
          debris.BuildInstances(inst);
          mobs.AppendInstances(inst, debris.BodyCount());
          if (!inst.empty())
            ctx.queue.WriteBuffer(world.bodyInstances, 0, inst.data(),
                                  inst.size() * sizeof(BodyVoxInst));

          // Close in and AIMED. The critter is ~3 world voxels across and has
          // been walking for 190 ticks, so a fixed yaw/pitch pair aimed at the
          // spawn point misses it entirely and the pixel threshold below stops
          // meaning anything. Take the torso's live transform and derive the
          // camera angles from the look vector.
          Vec3 target{};
          {
            std::vector<BodyXformGpu> t;
            mobs.AppendXforms(t);
            if (!t.empty()) target = Vec3{t[0].pos[0], t[0].pos[1], t[0].pos[2]};
            else target = mobs.MobOrigin(cid);
          }

          auto shoot = [&](bool withMicro, std::vector<uint8_t>& out) {
            wgpu::Texture tex = ctx.device.CreateTexture(&td);
            wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
            wgpu::RenderPassEncoder rp = sim.BeginRenderPass(
                enc, tex.CreateView(), wgpu::TextureFormat::RGBA8Unorm, W, H);
            sim.DrawWorld(rp);
            sim.DrawBodies(rp, (uint32_t)inst.size());
            if (withMicro) sim.DrawMicroBodies(rp, ctx.queue, microInsts);
            rp.End();
            wgpu::Buffer shot = CreateBuffer(
                ctx.device, (uint64_t)W * H * 4,
                wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst, "microShot");
            wgpu::TexelCopyTextureInfo srcT{};
            srcT.texture = tex;
            wgpu::TexelCopyBufferInfo dstB{};
            dstB.buffer = shot;
            dstB.layout.bytesPerRow = W * 4;
            dstB.layout.rowsPerImage = H;
            wgpu::Extent3D ext{W, H, 1};
            enc.CopyTextureToBuffer(&srcT, &dstB, &ext);
            wgpu::CommandBuffer cmd = enc.Finish();
            ctx.queue.Submit(1, &cmd);
            out.assign((size_t)W * H * 4, 0);
            wgpu::Future f = shot.MapAsync(
                wgpu::MapMode::Read, 0, out.size(), wgpu::CallbackMode::WaitAnyOnly,
                [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                  if (status == wgpu::MapAsyncStatus::Success) {
                    std::memcpy(out.data(), shot.GetConstMappedRange(0, out.size()),
                                out.size());
                    shot.Unmap();
                  }
                });
            ctx.instance.WaitAny(f, UINT64_MAX);
          };

          // VIEW SWEEP. One camera angle cannot test an OBB: the box has six
          // faces and a winding bug in even one of them only hides the body
          // from the directions that face it. Orbit the critter through the 8
          // diagonal octants AND the 6 axis directions, and require the pass to
          // change pixels from EVERY one. This is the regression for the
          // mixed-winding bug (backface culling ate the faces whose triangles
          // wound the other way, so the critter vanished from half the compass).
          const Vec3 kDirs[] = {
              // 8 diagonal octants
              {1, 1, 1},   {1, 1, -1},  {1, -1, 1},  {1, -1, -1},
              {-1, 1, 1},  {-1, 1, -1}, {-1, -1, 1}, {-1, -1, -1},
              // 6 axis directions (grazing/axis-aligned rays are their own case:
              // the DDA's near-zero-component guard only matters here)
              {1, 0, 0},   {-1, 0, 0},  {0, 0, 1},   {0, 0, -1},
              {0, 1, 0},   {0, -1, 0},
          };
          const int kNumDirs = (int)(sizeof(kDirs) / sizeof(kDirs[0]));
          uint32_t minDiff = 0xFFFFFFFFu, maxDiff = 0;
          int badDirs = 0, firstBad = -1;
          std::vector<uint8_t> withPix, withoutPix, keepPix;
          for (int d = 0; d < kNumDirs; d++) {
            // Normalize then push out to a fixed radius so every direction
            // frames the critter at the same distance — otherwise a diagonal
            // eye sits 1.7x further out than an axial one and the pixel counts
            // are not comparable.
            Vec3 dir = kDirs[d].normalized();
            Vec3 eye = target + dir * 8.0f;
            Vec3 look = (target - eye).normalized();
            Camera cam2;
            cam2.yaw = std::atan2(look.z, look.x);
            cam2.pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
            WriteRenderParams(ctx.queue, world, eye, cam2, (float)W / H, true, 0);

            shoot(true, withPix);
            shoot(false, withoutPix);
            uint32_t diff = 0;
            for (size_t p = 0; p + 3 < withPix.size(); p += 4)
              if (withPix[p] != withoutPix[p] ||
                  withPix[p + 1] != withoutPix[p + 1] ||
                  withPix[p + 2] != withoutPix[p + 2])
                diff++;
            if (diff < minDiff) minDiff = diff;
            if (diff > maxDiff) maxDiff = diff;
            // A scale-2 critter framed from 8 voxels away covers several
            // thousand pixels at 640x360. 500 is a floor only a direction that
            // drew nothing — or drew entirely behind the terrain, i.e. got the
            // depth convention wrong — can fall under. Deliberately far below
            // the observed count so gait wander can never flake the test.
            if (diff < 500) {
              badDirs++;
              if (firstBad < 0) firstBad = d;
            }
            // keep the first diagonal's image as the visual artifact
            if (d == 0) keepPix = withPix;
          }
          // SINGLE-BODY PROBE. The sweep above draws all 9 limbs at once, so a
          // box that vanishes is masked by its neighbours — the critter as a
          // whole stays visible even when individual limbs drop out. Culling is
          // decided per triangle in the body's OWN object space, so isolate ONE
          // body with an IDENTITY rotation: object space then equals world
          // space, and the camera's octant maps 1:1 onto the box's own octant.
          // A winding bug in the 36-vertex cube shows up here as a whole octant
          // rendering nothing, which is exactly the reported symptom.
          int soloBad = 0, soloFirst = -1;
          uint32_t soloMin = 0xFFFFFFFFu;
          {
            std::vector<MicroBodyInstGpu> solo(1, microInsts[0]);
            // Park the single body in open air well above the terrain, with an
            // identity quaternion, so nothing occludes it and no gait pose
            // rotates the octants out from under the assertion.
            uint32_t slot = solo[0].slot;
            Vec3 soloPos{target.x, target.y + 24.0f, target.z};
            std::vector<BodyXformGpu> sxf = xf;
            if (slot < sxf.size()) {
              sxf[slot].pos[0] = soloPos.x;
              sxf[slot].pos[1] = soloPos.y;
              sxf[slot].pos[2] = soloPos.z;
              sxf[slot].quat[0] = 0.0f;
              sxf[slot].quat[1] = 0.0f;
              sxf[slot].quat[2] = 0.0f;
              sxf[slot].quat[3] = 1.0f;
              ctx.queue.WriteBuffer(world.bodyXforms, 0, sxf.data(),
                                    sxf.size() * sizeof(BodyXformGpu));
            }
            std::vector<uint8_t> sWith, sWithout;
            for (int d = 0; d < kNumDirs; d++) {
              Vec3 dir = kDirs[d].normalized();
              Vec3 eye = soloPos + dir * 6.0f;
              Vec3 look = (soloPos - eye).normalized();
              Camera cam3;
              cam3.yaw = std::atan2(look.z, look.x);
              cam3.pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
              WriteRenderParams(ctx.queue, world, eye, cam3, (float)W / H, true, 0);
              // Draw ONLY the micro pass against the world, twice, so the diff
              // isolates this one body.
              auto shootSolo = [&](bool withMicro, std::vector<uint8_t>& out) {
                wgpu::Texture tex = ctx.device.CreateTexture(&td);
                wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
                wgpu::RenderPassEncoder rp = sim.BeginRenderPass(
                    enc, tex.CreateView(), wgpu::TextureFormat::RGBA8Unorm, W, H);
                sim.DrawWorld(rp);
                if (withMicro) sim.DrawMicroBodies(rp, ctx.queue, solo);
                rp.End();
                wgpu::Buffer shot = CreateBuffer(
                    ctx.device, (uint64_t)W * H * 4,
                    wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                    "microSolo");
                wgpu::TexelCopyTextureInfo srcT{};
                srcT.texture = tex;
                wgpu::TexelCopyBufferInfo dstB{};
                dstB.buffer = shot;
                dstB.layout.bytesPerRow = W * 4;
                dstB.layout.rowsPerImage = H;
                wgpu::Extent3D ext{W, H, 1};
                enc.CopyTextureToBuffer(&srcT, &dstB, &ext);
                wgpu::CommandBuffer cmd = enc.Finish();
                ctx.queue.Submit(1, &cmd);
                out.assign((size_t)W * H * 4, 0);
                wgpu::Future fu = shot.MapAsync(
                    wgpu::MapMode::Read, 0, out.size(),
                    wgpu::CallbackMode::WaitAnyOnly,
                    [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                      if (status == wgpu::MapAsyncStatus::Success) {
                        std::memcpy(out.data(),
                                    shot.GetConstMappedRange(0, out.size()),
                                    out.size());
                        shot.Unmap();
                      }
                    });
                ctx.instance.WaitAny(fu, UINT64_MAX);
              };
              shootSolo(true, sWith);
              shootSolo(false, sWithout);
              uint32_t sd = 0;
              for (size_t p = 0; p + 3 < sWith.size(); p += 4)
                if (sWith[p] != sWithout[p] || sWith[p + 1] != sWithout[p + 1] ||
                    sWith[p + 2] != sWithout[p + 2])
                  sd++;
              if (sd < soloMin) soloMin = sd;
              // A single limb 6 voxels away fills hundreds of pixels; 50 is a
              // floor only "drew nothing at all" can fall under.
              if (sd < 50) {
                soloBad++;
                if (soloFirst < 0) soloFirst = d;
              }
            }
            // restore the real transforms for anything downstream
            if (!xf.empty())
              ctx.queue.WriteBuffer(world.bodyXforms, 0, xf.data(),
                                    xf.size() * sizeof(BodyXformGpu));
          }

          bool microOk = !microInsts.empty() && badDirs == 0 && soloBad == 0;
          std::printf("micro body render: %s (%zu micro slots, %d/%d views drew, "
                      "%u..%u px changed of %u, solo body %d/%d views (min %u px), "
                      "%zu cube instances from micro limbs)\n",
                      microOk ? "PASS" : "FAIL", microInsts.size(),
                      kNumDirs - badDirs, kNumDirs, minDiff, maxDiff, W * H,
                      kNumDirs - soloBad, kNumDirs, soloMin, inst.size());
          if (badDirs > 0)
            std::printf("  critter INVISIBLE from %d view(s); first is dir "
                        "(%.0f,%.0f,%.0f)\n",
                        badDirs, kDirs[firstBad].x, kDirs[firstBad].y,
                        kDirs[firstBad].z);
          if (soloBad > 0)
            std::printf("  SOLO micro body INVISIBLE from %d/%d view(s); first "
                        "is dir (%.0f,%.0f,%.0f) — mixed cube winding?\n",
                        soloBad, kNumDirs, kDirs[soloFirst].x, kDirs[soloFirst].y,
                        kDirs[soloFirst].z);
          // Visual proof alongside the numeric one — the pixel count says
          // "something drew", the image says "it drew a critter".
          if (!keepPix.empty() && WriteBmp("screenshot_microbody.bmp", keepPix, W, H))
            std::printf("wrote screenshot_microbody.bmp\n");
          mobOk = mobOk && microOk;
        }

        // ---- per-voxel carving of a LIVE limb ----
        // The assertions are per-limb invariants, not rates (the frontier-rule
        // lesson):
        //   1. a carve removes REAL voxels from a limb that is still attached,
        //   2. the limb keeps its identity while it does — same body, still
        //      alive, still driving locomotion — so wounds are cosmetic until
        //      they are not, and
        //   3. carving the SAME spot until the limb cannot hold together
        //      severs it, with no hp threshold having decided that.
        // (3) is the load-bearing one: it is what makes dismemberment a
        // geometric consequence of what the player actually cut away.
        //
        // Run on the CRITTER (scale 2), not the dummy: the dummy's arm is five
        // world voxels, which is below the fragment floor before a single cut
        // lands, so it could only ever prove the collapse branch. Carving is
        // for micro rigs — that is where a limb has enough voxels for a wound
        // to be a wound rather than an amputation.
        if (critterDef >= 0) {
          debris.Reset();
          mobs.Reset();
          const MobDef& ccd = mobs.Defs()[critterDef];
          auto critterLimb = [&](const char* name) {
            for (size_t i = 0; i < ccd.limbs.size(); i++)
              if (ccd.limbs[i].name == name) return (int)i;
            return -1;
          };
          uint64_t cid = mobs.Spawn(critterDef, {143, h + 1, 143});
          for (int i = 0; i < 6; i++) mobTick({});
          // An upper leg: severable (unlike the torso, which is the root) and
          // big enough at scale 2 to survive several bites.
          const int carveLimb = critterLimb("legU.FL");
          uint32_t v0 = mobs.LimbVoxelCount(cid, carveLimb);
          uint32_t spawnVox = mobs.LimbVoxelsAtSpawn(cid, carveLimb);

          // One nick, off the joint: the limb must lose matter and keep living.
          uint32_t v1 = v0;
          bool stillAttached = false, nickHit = false;
          {
            uint64_t lb = mobs.LimbBody(cid, carveLimb);
            std::vector<ParticleSpawn> cs;
            // Aim at a voxel in the middle of the limb's own list — far from
            // the hip anchor, so this cannot be the joint-crossing sever path
            // in disguise. Not xf.pos: that is the min corner, not flesh.
            // Radius is in WORLD voxels; at scale 2 this is ~1 world voxel.
            nickHit = mobs.CarveLimbRadial(
                lb, mobs.LimbVoxelPos(cid, carveLimb, v0 / 2), 0.5f, true, true,
                world, cs);
            for (int i = 0; i < 3; i++) mobTick({});
            v1 = mobs.LimbVoxelCount(cid, carveLimb);
            stillAttached = mobs.LimbBody(cid, carveLimb) != 0;
          }

          // Now keep cutting the same limb until it comes off. The body handle
          // is re-read every pass: a carve rebuilds the collider and hands the
          // limb a NEW handle, so a cached one goes stale after the first cut
          // (the same trap the debris kerf test documents).
          int passes = 0;
          for (; passes < 40 && mobs.LimbBody(cid, carveLimb) != 0; passes++) {
            uint64_t lb = mobs.LimbBody(cid, carveLimb);
            std::vector<ParticleSpawn> cs;
            // Bite at flesh that is STILL THERE each pass. A fixed aim point
            // bores one hole and then sits in the cavity it made — what
            // survives is exactly what was out of its reach — so eating a limb
            // through means following the remaining meat.
            mobs.CarveLimbRadial(lb, mobs.LimbVoxelPos(cid, carveLimb, 0), 0.8f,
                                 true, true, world, cs);
            mobTick({});
          }
          bool severedByCarving = mobs.LimbBody(cid, carveLimb) == 0;
          // The mob must survive losing an arm — otherwise "it died" would
          // explain the detachment just as well as carving did.
          bool aliveAfter = mobs.IsAlive(cid);

          bool carveOk = nickHit && v1 < v0 && v1 > 0 && stillAttached &&
                         severedByCarving && aliveAfter;
          std::printf("mob carve: %s (legU.FL %u/%u -> %u voxels attached=%d, "
                      "severed after %d more carves, mob alive=%d)\n",
                      carveOk ? "PASS" : "FAIL", v0, spawnVox, v1,
                      stillAttached ? 1 : 0, passes, aliveAfter ? 1 : 0);
          mobOk = mobOk && carveOk;
          debris.Reset();
          mobs.Reset();
        } else {
          std::printf("mob carve: SKIP (no critter def)\n");
        }

        // ---- dismemberment locomotion states: the maimed keep moving ----
        // The rules live in the sidecars ("states"). Assertions are
        // structural — which rule is active, the loco clip is running, the
        // gait is silenced while a crawl owns the pose — plus "it still makes
        // way along its facing", never rate comparisons (the frontier-rule
        // lesson: rates prove nothing).
        {
          debris.Reset();
          mobs.Reset();

          // dummy ladder, most-maimed-first in the sidecar so the indices
          // run BACKWARDS as limbs come off: intact -1, one leg lost -> 3
          // (crawl.oneLeg), both legs -> 2 (crawl.legless), plus an arm -> 1
          // (crawl.oneArm), no arms -> 0 (prone, speedScale 0).
          uint64_t did = mobs.Spawn(dummyDef, {137, h + 1, 139});
          for (int i = 0; i < 10; i++) mobTick({});
          int s0 = mobs.LocoState(did);
          mobs.Sever(did, limbIndex("leg.L"));
          for (int i = 0; i < 10; i++) mobTick({});
          int s1 = mobs.LocoState(did);
          mobs.Sever(did, limbIndex("leg.R"));
          for (int i = 0; i < 10; i++) mobTick({});
          int s2 = mobs.LocoState(did);
          bool dummyClip = mobs.ActiveClips(did) >= 1;
          // legless, it must still make way along its facing
          Vec3 dPrev2 = mobs.MobOrigin(did);
          float dAlong2 = 0.0f, dPath2 = 0.0f;
          for (int i = 0; i < 180; i++) {
            Vec3 face = mobs.MobFacing(did);
            mobTick({});
            Vec3 now = mobs.MobOrigin(did);
            Vec3 step{now.x - dPrev2.x, 0, now.z - dPrev2.z};
            dPrev2 = now;
            dAlong2 += step.x * face.x + step.z * face.z;
            dPath2 += std::sqrt(step.x * step.x + step.z * step.z);
          }
          bool dummyCrawls = dPath2 > 1.0f && dAlong2 > 0.5f * dPath2;
          // one arm gone: still a (slower) crawl; both arms gone: prone and
          // IMMOBILE — "it stops moving" is the invariant, so measure the
          // path, not the rate.
          mobs.Sever(did, limbIndex("arm.L"));
          for (int i = 0; i < 10; i++) mobTick({});
          int s3 = mobs.LocoState(did);
          mobs.Sever(did, limbIndex("arm.R"));
          for (int i = 0; i < 10; i++) mobTick({});
          int s4 = mobs.LocoState(did);
          bool proneClip = mobs.ActiveClips(did) >= 1;
          Vec3 pPrev = mobs.MobOrigin(did);
          float pPath = 0.0f;
          for (int i = 0; i < 100; i++) {
            mobTick({});
            Vec3 now = mobs.MobOrigin(did);
            Vec3 step{now.x - pPrev.x, 0, now.z - pPrev.z};
            pPrev = now;
            pPath += std::sqrt(step.x * step.x + step.z * step.z);
          }
          bool proneStill = pPath < 0.25f && mobs.IsAlive(did);
          bool dummyStates = s0 == -1 && s1 == 3 && s2 == 2 && s3 == 1 &&
                             s4 == 0 && dummyClip && dummyCrawls &&
                             proneClip && proneStill;

          // critter: one lost chain is the gait's own graceful degradation
          // (no rule fires); the second flips it to the crawl state, which
          // must silence the gait scheduler completely.
          uint64_t cid2 = mobs.Spawn(critterDef, {150, h + 1, 150});
          mobs.Sever(cid2, critterLimb("legU.FL"));
          for (int i = 0; i < 10; i++) mobTick({});
          int c1 = mobs.LocoState(cid2);
          mobs.Sever(cid2, critterLimb("legU.BR"));
          for (int i = 0; i < 10; i++) mobTick({});
          int c2 = mobs.LocoState(cid2);
          bool critClip = mobs.ActiveClips(cid2) >= 1;
          int swingTicks = 0;
          Vec3 cPrev2 = mobs.MobOrigin(cid2);
          float cAlong2 = 0.0f, cPath2 = 0.0f;
          for (int i = 0; i < 150; i++) {
            Vec3 face = mobs.MobFacing(cid2);
            mobTick({});
            if (mobs.SwingingFeet(cid2) > 0) swingTicks++;
            Vec3 now = mobs.MobOrigin(cid2);
            Vec3 step{now.x - cPrev2.x, 0, now.z - cPrev2.z};
            cPrev2 = now;
            cAlong2 += step.x * face.x + step.z * face.z;
            cPath2 += std::sqrt(step.x * step.x + step.z * step.z);
          }
          bool critCrawls = cPath2 > 1.0f && cAlong2 > 0.5f * cPath2;
          bool critStates = c1 == -1 && c2 == 0 && critClip &&
                            swingTicks == 0 && critCrawls;

          bool stateOk = dummyStates && critStates;
          std::printf(
              "mob dismember states: %s (dummy %d->%d->%d->%d->%d clip=%d "
              "crawled %.1f/%.1f vox, prone drift %.2f; critter %d->%d "
              "clip=%d swingTicks=%d crawled %.1f/%.1f vox)\n",
              stateOk ? "PASS" : "FAIL", s0, s1, s2, s3, s4, dummyClip ? 1 : 0,
              dAlong2, dPath2, pPath, c1, c2, critClip ? 1 : 0, swingTicks,
              cAlong2, cPath2);
          mobOk = mobOk && stateOk;
        }

        debris.Reset();
        mobs.Reset();
      }

      // ---- player avatar ----
      // The avatar reuses the mob rig but is driven by the PLAYER, so none of
      // the tests above touch it. What is worth asserting is exactly the part
      // that is avatar-specific and easy to break silently:
      //   1. the avatar def loads with every part, chain and state resolved
      //   2. spawning creates one Jolt body per part
      //   3. the body FOLLOWS the player rather than wandering off
      //   4. dismemberment walks DOWN the authored state ladder and each step
      //      actually slows the player (the movement coupling), ending with a
      //      state that cannot jump
      //   5. severed parts become debris and the avatar tears down cleanly
      {
        // Test whatever def the GAME uses as the avatar (kAvatarDefName), not
        // a hardcoded name — a selftest pinned to the old name would keep
        // passing against a character nobody plays.
        const std::string avDefName = kAvatarDefName;
        int wizDef = -1;
        for (size_t i = 0; i < mobs.Defs().size(); i++)
          if (mobs.Defs()[i].name == avDefName) wizDef = (int)i;
        if (wizDef < 0) {
          std::printf("avatar: SKIP (no %s def — run scripts/gen_%s.py)\n",
                      avDefName.c_str(), avDefName.c_str());
        } else {
          debris.Reset();
          const MobDef& wd = mobs.Defs()[wizDef];
          PlayerAvatar avatar;
          avatar.Init(&phys, &world, &debris, mats);
          avatar.SetDefs(&mobs.Defs(), avDefName);

          int h2 = World::TerrainHeight(140, 140, kDefaultSeed);
          Player pl;
          pl.fly = false;
          // The avatar gates its locomotion clips on `grounded` (you do not
          // swing your arms mid-jump), and a default-constructed Player is not
          // grounded — so without this the walk/run clips never start and the
          // arms hang dead through the whole test. That is exactly the "arms
          // outstretched like a zombie" case, so leaving it unset would have
          // the test assert on a pose the game never shows.
          pl.grounded = true;
          pl.pos = Vec3{140.5f, (float)(h2 + 2) + Player::kHalfY, 140.5f};
          bool spawned = avatar.Spawn(pl, 0.0f);
          const int nParts = (int)wd.limbs.size();
          bool allBodies = (int)avatar.LimbBodyCount() == nParts;

          auto avTick = [&]() {
            std::vector<BrushOp> ops;
            std::vector<ParticleSpawn> spawns;
            avatar.PreTick(t + 1, pl, 0.0f, kTickDt, world, ops, spawns);
            debris.QueueSupportEvents(world.Snap());
            std::vector<CellOp> cellOps;
            debris.PreTick(t + 1, world, cellOps, spawns);
            ++t;
            SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps,
                       false, {140 / 16, h2 / 16, 140 / 16}, true, false,
                       spawns);
            ctx.WaitIdle();
            ctx.ProcessEvents();
            phys.Step(kTickDt);
            debris.PostStep();
            avatar.PostStep();
          };
          for (int i = 0; i < 20; i++) avTick();

          // The body must TRACK the player: walk the player 12 voxels and the
          // avatar origin has to come along. A rig that ignored its driver
          // (the mob wander drive, say) would sit still and still look fine in
          // a screenshot.
          Vec3 originBefore = avatar.Origin();
          // Footfall accounting over the walk. These are what drive footstep
          // AUDIO, but the assertion is deliberately about the EVENTS, not the
          // sound: the selftest is headless and opens no audio device, so this
          // checks the half that can actually break silently — that the gait
          // emits one event per plant, on a real material, at the foot.
          int footfalls = 0;
          int footfallsBadMat = 0;
          int footfallsFarFromFoot = 0;
          // +Z is FORWARD at heading 0 — walk the way the body faces. Driving
          // +X here walked the avatar sideways, which is not a gait the game
          // can ever show and is not what the pose assertions below describe.
          for (int i = 0; i < 60; i++) {
            pl.pos.z += 0.2f;
            avTick();
            for (const PlayerAvatar::Footfall& ff : avatar.Footfalls()) {
              footfalls++;
              // A step must name a real, non-air material, or it is silent.
              if (ff.mat == 0 || ff.mat >= mats.size()) footfallsBadMat++;
              // ...and must land at the body, not at the world origin: a step
              // heard 100 m away from the player is the failure mode a
              // coordinate-conversion bug produces.
              if (Vec3{ff.posVox.x - pl.pos.x, 0, ff.posVox.z - pl.pos.z}.len() > 8.0f)
                footfallsFarFromFoot++;
            }
            avatar.ClearFootfalls();
          }
          float followed = avatar.Origin().z - originBefore.z;
          bool follows = followed > 10.0f;
          // 60 ticks of walking is 2 s; any sane gait plants several times.
          bool stepsOk = footfalls >= 2 && footfallsBadMat == 0 &&
                         footfallsFarFromFoot == 0;

          // The body must stay AT the player, not drift vertically away from
          // it. The gait used to re-derive its own standing height from the
          // foot plane, and because the foot goal falls back to that same
          // height when a ground probe misses, it fed itself and the avatar
          // climbed ~9.5 voxels a tick — "the wizard is 100 feet above the
          // player". A drift assertion is the cheap guard: any feedback path
          // that returns shows up here as an unbounded number, whatever its
          // cause. Tolerance is one body height, which covers the legitimate
          // gap between the AABB sole and an animated pose.
          float soleY = pl.pos.y - Player::kHalfY;
          float drift = std::abs(avatar.BodyY() - soleY);
          bool tracksY = drift < wd.worldSize.y;

          // YOUR OWN BODY MUST NOT PUSH YOU. The avatar's limbs are drawn
          // around the player capsule, so on the normal dynamic layer they sit
          // permanently interpenetrated with the proxy and PlayerPushOut reads
          // a large ejection vector whose direction swings with the gait —
          // walking forward drifted backwards and diagonally. The limbs live
          // on Layers::AVATAR now, which PlayerPushOut does not see. Sampled
          // over several ticks because the failure was ANIMATED, not static:
          // one sample could land on a frame where the swing happened to
          // cancel.
          uint64_t avProxy = phys.CreatePlayerBody(Player::kHalfXZ,
                                                   Player::kHalfY);
          float selfPush = 0.0f;
          for (int i = 0; i < 30; i++) {
            pl.pos.x += 0.2f;
            phys.MovePlayerBody(avProxy, pl.pos, kTickDt);
            avTick();
            selfPush = std::max(selfPush,
                                phys.PlayerPushOut(avProxy, pl.pos).len());
          }
          phys.RemoveBody(avProxy);
          bool noSelfPush = selfPush < 0.001f;

          // ---- gait quality AT THE PLAYER'S REAL SPEED ----
          // Everything above walks the player at 0.2 vox/tick = 6 vox/sec,
          // which is a sixth of walkSpeed and a tenth of sprintSpeed. That is
          // why "the legs flail behind like a naruto run" sailed through a
          // green selftest: the failure only exists at speeds the test never
          // reached. Drive the real numbers and assert on the POSE.
          //
          // The invariant is limb ELEVATION: a leg that is walking stays near
          // vertical (its two bones fold and swing about the hip), while a leg
          // whose IK target is out of reach straightens and rotates toward
          // horizontal to point at it. So "min elevation over a stride" is a
          // direct measure of the trailing-leg failure, with no reference pose
          // to keep in sync.
          // Elevation of a limb's own axis above horizontal, 90 = hanging
          // straight down/up, 0 = sticking straight out. Limbs point along
          // their local +Y, and a limb rotates AWAY from vertical as it swings,
          // so this is the natural "is it swinging or is it pointing" measure.
          auto elevationOf = [&](int part) {
            Vec3 p;
            Quat q;
            if (!avatar.PartWorldTransform(part, p, q)) return 90.0f;
            Vec3 axis = QuatRotate(q, Vec3{0, 1, 0});
            float a = std::asin(std::clamp(axis.y, -1.0f, 1.0f));
            return std::fabs(a) * 57.29578f;
          };
          // SIGNED fore/aft swing of a limb, in degrees: how far its axis has
          // rotated out of vertical along the travel direction. The unsigned
          // elevation above folds a forward swing onto a backward one, so an
          // arm swinging +-14 degrees and an arm frozen at 0 both read ~90 and
          // the test could not tell "swinging" from "held out".
          // MEASURE IN THE PLANE THE LIMB ACTUALLY SWINGS IN.
          //
          // This test walks the avatar at heading 0, and heading 0 faces +Z
          // (fwd = {sin(h), 0, cos(h)}). Limb swing is authored as a rotation
          // about local X, which tilts a downward-hanging limb into Z — so the
          // fore/aft component of the swing is Z, and the X component stays
          // ~0 no matter how hard the limb swings.
          //
          // The previous version of this test measured atan2(axis.x, ...) while
          // ALSO driving the player along +X, i.e. it walked the character
          // SIDEWAYS (moving +X while facing +Z) and then read the one axis the
          // swing never reaches. Both halves were wrong, and together they made
          // every pose number it printed meaningless: a full-amplitude 14-degree
          // arm swing reported as ~2 degrees, so "arms swing" passed on an
          // 8-degree threshold that the authored 28-degree motion should have
          // cleared by 3x. Walk the way the body faces and measure the plane the
          // limb moves in, or this test cannot see the thing it exists to catch.
          auto swingOf = [&](int part) {
            Vec3 p;
            Quat q;
            if (!avatar.PartWorldTransform(part, p, q)) return 0.0f;
            Vec3 axis = QuatRotate(q, Vec3{0, 1, 0});
            // Fold onto the DOWNWARD hemisphere first: a limb model may point
            // either way along its local +Y, and atan2 against the wrong one
            // wraps to +-180 and makes a 14-degree swing look like 360.
            if (axis.y > 0) axis = axis * -1.0f;
            return std::atan2(axis.z, -axis.y) * 57.29578f;
          };
          const int legParts[4] = {avatar.PartIndex("legU.L"),
                                   avatar.PartIndex("legU.R"),
                                   avatar.PartIndex("legL.L"),
                                   avatar.PartIndex("legL.R")};
          const int armParts[2] = {avatar.PartIndex("armU.L"),
                                   avatar.PartIndex("armU.R")};
          float minLegElev = 90.0f, minArmElev = 999.0f, maxArmElev = -999.0f;
          // Signed fore/aft extremes of the LEGS, tracked per leg. A leg that
          // only ever reads negative is a leg that is always behind the body —
          // the "legs are just behind the whole time" failure — and no unsigned
          // measure can tell that apart from a healthy stride.
          float minLegSwing[2] = {999.0f, 999.0f};
          float maxLegSwing[2] = {-999.0f, -999.0f};
          const float walkStep =
              (CurrentTuning().player.walkSpeed / kVoxelMeters) * kTickDt;
          for (int i = 0; i < 90; i++) {
            pl.pos.z += walkStep;   // forward at heading 0, see swingOf above
            avTick();
            // Let the gait reach STEADY STATE before sampling. From a standing
            // start both feet are planted under the body and the first strides
            // are catching up, so the legs legitimately pass through low
            // elevations for a few ticks. 20 ticks was not enough once the
            // stride budget raised the cadence; 30 clears the transient with
            // margin and still leaves 60 ticks (2 s, several full cycles) of
            // real walking to assert on.
            if (i < 30) continue;
            for (int lp : legParts)
              if (lp >= 0) minLegElev = std::min(minLegElev, elevationOf(lp));
            for (int s = 0; s < 2; s++)
              if (legParts[s] >= 0) {
                float e = swingOf(legParts[s]);
                minLegSwing[s] = std::min(minLegSwing[s], e);
                maxLegSwing[s] = std::max(maxLegSwing[s], e);
              }
            for (int ap : armParts)
              if (ap >= 0) {
                float e = swingOf(ap);
                minArmElev = std::min(minArmElev, e);
                maxArmElev = std::max(maxArmElev, e);
              }
            // Per-tick gait trace. The summary line only reports extremes, and
            // a gait fails in ways an extreme cannot show — both legs stuck in
            // phase, an arm swinging at a quarter amplitude, a leg that never
            // comes in FRONT of the body. Those are all obvious in a tick-by-
            // tick column and invisible in a min/max. Off by default; the
            // assertions below are what gate the build.
            //   SANDVOX_GAITDBG=1 ./sandvox.exe --selftest
            // `swing` is signed fore/aft: negative = behind the body, positive
            // = in front, so a healthy walk shows BOTH signs on every limb.
            if (getenv("SANDVOX_GAITDBG")) {
              std::printf(
                  "  t%02d spd=%4.1f arm %6.1f/%6.1f  legU %6.1f/%6.1f  "
                  "legElev %5.1f/%5.1f\n",
                  i, avatar.SpeedNow(), swingOf(armParts[0]),
                  swingOf(armParts[1]), swingOf(legParts[0]),
                  swingOf(legParts[1]), elevationOf(legParts[0]),
                  elevationOf(legParts[1]));
            }
          }
          // A walking leg should never lie down. A healthy stride bottoms out
          // around 23 degrees at the extremes of the swing and spends most of
          // the cycle well above it; the trailing-leg failure drove it into
          // single digits, so the gap is wide.
          bool legsUpright = minLegElev > 18.0f;

          // THE LEGS MUST ALTERNATE FORE AND AFT, not just move.
          //
          // This is the assertion the old test was missing entirely, and it is
          // the one that matches the actual complaint: "when running the legs
          // are always behind the character instead of alternating in front of
          // and then behind". A leg driven by a negative stride budget still
          // SWINGS — it cycles between "far behind" and "slightly less far
          // behind" — so every range- or amplitude-based check passes while the
          // character rakes its legs out behind it. Only the SIGN catches it.
          // Require each leg to spend part of the cycle genuinely in front of
          // the hip, with a few degrees of margin so it cannot pass on noise.
          bool legsAlternate = true;
          for (int s = 0; s < 2; s++)
            legsAlternate = legsAlternate && maxLegSwing[s] > 5.0f &&
                            minLegSwing[s] < -5.0f;

          // Arms must SWING and must stay roughly under the shoulder. In the
          // signed measure, 0 is hanging straight down and +-90 is held
          // straight out. A zombie arm pins near one extreme and never moves;
          // a walking arm oscillates about 0.
          //
          // The threshold is 20 degrees against an authored +-14 (28 total).
          // The old 8 was below HALF the authored motion, which is how a walk
          // rendering at 8 degrees — visually a dead arm — passed this test for
          // as long as it did. A threshold that a correct implementation clears
          // by only a hair is not a test. Set it close under the authored value
          // so any real suppression of the swing fails immediately.
          bool armsSwing = (maxArmElev - minArmElev) > 20.0f;
          bool armsHang = std::fabs(maxArmElev) < 60.0f &&
                          std::fabs(minArmElev) < 60.0f;

          // ---- FALLING: the legs must not invert into the body ----
          //
          // Completely uncovered before, and the failure was spectacular: the
          // gait ran while airborne, its ground probe (which only ever scans
          // DOWNWARD) missed every tick, and `planted` — a WORLD-space point —
          // stayed where the floor used to be while the body fell away from it.
          // Within a few ticks the IK target sat ABOVE the hip and the solver
          // dutifully aimed the legs up at it, folding them through the pelvis
          // and inside the torso and head.
          //
          // Assert on the MODEL-space pose, which is what "the legs inverted"
          // actually means: the rig's own idea of where the limbs are. Reading
          // the Jolt transforms back instead would measure body CENTRES that
          // the solver is still chasing through a teleporting fall, and they
          // collapse toward each other for reasons that have nothing to do with
          // the pose.
          //
          // The invariant is that the leg keeps HANGING: the hip-to-foot vector
          // must stay pointed downward-ish in the body frame. An inverted leg
          // flips that vector to point up. Measuring the vector rather than an
          // angle means a tucked knee (jump) and a straight leg (fall) are both
          // fine, and only a genuine fold-through fails.
          float worstLegUp = -1e9f;
          {
            pl.grounded = false;
            const int hipParts[2] = {avatar.PartIndex("legU.L"),
                                     avatar.PartIndex("legU.R")};
            const int feet[2] = {avatar.PartIndex("foot.L"),
                                 avatar.PartIndex("foot.R")};
            for (int i = 0; i < 45; i++) {
              // Fall: the player leaves the ground and keeps dropping, which is
              // what starves the downward-only ground probe and, before the
              // fix, left the IK chasing a foot plant the body had left behind.
              pl.pos.y -= 0.6f;
              pl.vel.y = -18.0f;
              avTick();
              for (int s = 0; s < 2; s++) {
                Vec3 hp, fp;
                Quat hq, fq;
                if (hipParts[s] < 0 || feet[s] < 0) continue;
                if (!avatar.PartModelTransform(hipParts[s], hp, hq)) continue;
                if (!avatar.PartModelTransform(feet[s], fp, fq)) continue;
                // Model space is Y-up, so a hanging leg has foot.y < hip.y.
                // Positive = the foot has risen above its own hip: inverted.
                worstLegUp = std::max(worstLegUp, fp.y - hp.y);
              }
              if (getenv("SANDVOX_GAITDBG")) {
                Vec3 hp, fp;
                Quat hq, fq;
                avatar.PartModelTransform(hipParts[0], hp, hq);
                avatar.PartModelTransform(feet[0], fp, fq);
                std::printf("  fall t%02d clips=%d hipY=%.2f footY=%.2f %+.2f\n",
                            i, avatar.ActiveClips(), hp.y, fp.y, fp.y - hp.y);
              }
            }
            pl.grounded = true;
          }
          // A hanging leg is about -4.5 here (hip to foot down the leg). Allow
          // plenty of slack for a jump tuck; only a real fold-through goes
          // positive.
          bool legsNotInverted = worstLegUp < -0.5f;

          // Walk DOWN the state ladder and check the movement coupling at each
          // rung. Speed must be non-increasing and must actually drop by the
          // end — the whole point of the states is that damage costs you.
          const char* ladder[] = {"foot.R", "legL.R", "legU.L"};
          float prevSpeed = avatar.Locomotion().speedScale;
          bool monotone = prevSpeed == 1.0f;
          int statesSeen = 0;
          for (const char* nm : ladder) {
            if (!avatar.SeverByName(nm)) continue;
            for (int i = 0; i < 12; i++) avTick();
            float s = avatar.Locomotion().speedScale;
            monotone = monotone && s <= prevSpeed + 1e-4f;
            prevSpeed = s;
            if (avatar.LocoState() >= 0) statesSeen++;
          }
          bool slowed = prevSpeed < 1.0f;
          // both legs unusable -> no jump. This is derived from leg liveness
          // rather than authored, so it must hold whatever the rules say.
          // Captured HERE, while the avatar is still standing: reading it back
          // after Despawn would report the pristine defaults and quietly turn
          // this assertion into a tautology.
          const bool canJumpNow = avatar.Locomotion().canJump;
          const int stateNow = avatar.LocoState();
          bool noJump = !canJumpNow;

          uint32_t partsLeft = avatar.LimbBodyCount();
          bool partsGone = (int)partsLeft < nParts;
          size_t debrisNow = debris.BodyCount();
          bool becameDebris = debrisNow > 0;

          avatar.Despawn();
          bool tornDown = avatar.LimbBodyCount() == 0;

          bool avOk = spawned && allBodies && follows && tracksY &&
                      noSelfPush && monotone && slowed && noJump &&
                      statesSeen > 0 && partsGone && becameDebris && tornDown &&
                      legsUpright && legsAlternate && legsNotInverted &&
                      armsHang && armsSwing;
          std::printf(
              "avatar: %s (%d parts, spawned=%d bodies=%d, followed %.1f vox, "
              "y-drift %.2f vox, self-push %.3f vox, states seen=%d (last %d) "
              "speed 1.00->%.2f monotone=%d canJump=%d, %u parts left, "
              "%zu debris, torn down=%d; walking legElev>=%.0f arm %.0f..%.0f "
              "legL %.0f..%.0f legR %.0f..%.0f "
              "upright=%d alternate=%d hang=%d swing=%d; "
              "falling hipToFootY %.2f notInverted=%d)\n",
              avOk ? "PASS" : "FAIL", nParts, spawned ? 1 : 0,
              allBodies ? 1 : 0, followed, drift, selfPush, statesSeen,
              stateNow, prevSpeed, monotone ? 1 : 0, canJumpNow ? 1 : 0,
              partsLeft, debrisNow, tornDown ? 1 : 0, minLegElev, minArmElev,
              maxArmElev, minLegSwing[0], maxLegSwing[0], minLegSwing[1],
              maxLegSwing[1], legsUpright ? 1 : 0, legsAlternate ? 1 : 0,
              armsHang ? 1 : 0, armsSwing ? 1 : 0, worstLegUp,
              legsNotInverted ? 1 : 0);
          mobOk = mobOk && avOk;

          // Reported separately from `avatar` so a gait-look regression and a
          // footstep-plumbing regression never hide behind one another.
          std::printf(
              "avatar footfalls: %s (%d plants over 60 walk ticks, %d bad "
              "material, %d away from the body)\n",
              stepsOk ? "PASS" : "FAIL", footfalls, footfallsBadMat,
              footfallsFarFromFoot);
          mobOk = mobOk && stepsOk;
          debris.Reset();
        }
      }
    }
  }

  // B6 settle-back: a dropped stone block must sleep, snap to the lattice,
  // convert back into grid voxels through the op stream, and free its body —
  // closing the grid -> body -> grid loop.
  bool settleOk = false;
  {
    debris.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    int h = World::TerrainHeight(80, 80, kDefaultSeed);
    std::vector<float> dens;
    for (const auto& m : mats) dens.push_back((float)m.gpu.density);
    std::vector<DebrisVoxel> vox;
    for (int z = 0; z < 3; z++)
      for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
          vox.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
    uint64_t bh = phys.CreateDebrisBody(vox, {80, h + 4, 80}, dens);
    BodyTransform bxf{};
    bxf.pos = Vec3{80, (float)(h + 4), 80};
    bxf.quat[3] = 1;
    debris.AdoptBody(bh, vox, bxf);

    uint32_t t = 8000;
    for (int i = 0; i < 360 && debris.BodyCount() > 0; i++) {
      std::vector<CellOp> cellOps;
      std::vector<ParticleSpawn> spawns;
      debris.PreTick(t + 1, world, cellOps, spawns);
      ++t;
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cellOps, false,
                 {5, h / 16, 5}, true, false, spawns);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      phys.Step(kTickDt);
      debris.PostStep();
    }
    settleOk = debris.BodyCount() == 0 && debris.SettledBack() >= 1;
    std::printf("settle-back: %s (%u bodies converted to grid, %u still "
                "bodies)\n",
                settleOk ? "PASS" : "FAIL", debris.SettledBack(),
                debris.BodyCount());

    // C2 body split: a 3x3x9 bar cut through the middle must become two
    // independent bodies (no stepping needed — pure partition + respawn)
    std::vector<DebrisVoxel> bar;
    for (int z = 0; z < 9; z++)
      for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
          bar.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
    uint64_t barBody = phys.CreateDebrisBody(bar, {500, 500, 500}, dens);
    BodyTransform barXf{};
    barXf.pos = Vec3{500, 500, 500};
    barXf.quat[3] = 1;
    debris.AdoptBody(barBody, bar, barXf);
    bool splitOk = debris.SplitBody(barBody, Vec3{501.5f, 501.5f, 504.5f},
                                    Vec3{0, 0, 1}) &&
                   debris.BodyCount() == 2;
    std::printf("body split: %s (%u bodies after cut)\n",
                splitOk ? "PASS" : "FAIL", debris.BodyCount());
    settleOk = settleOk && splitOk;
    debris.Reset();

    // body blast: an explosion must take VOXELS OFF a body, not just shove it.
    // A blast centred on the waist of a dumbbell has to cut it in two, so the
    // gate is both "lost voxels" and "ended up as more than one body" — the
    // second is what separates real damage from a cosmetic crater.
    {
      SubmitWorldgen(ctx, world, sim, kDefaultSeed);
      ctx.WaitIdle();
      int bh3 = World::TerrainHeight(120, 120, kDefaultSeed);
      // two 5x5x2 plates joined by a 1x1x3 neck: blasting the neck separates
      // them, and each plate is far above the 8-voxel body floor
      std::vector<DebrisVoxel> bar;
      for (int z = 0; z < 2; z++)
        for (int y = 0; y < 5; y++)
          for (int x = 0; x < 5; x++) {
            bar.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
            bar.push_back({(int8_t)x, (int8_t)y, (int8_t)(z + 5), 0, kMatStone});
          }
      for (int z = 2; z < 5; z++)
        bar.push_back({2, 2, (int8_t)z, 0, kMatStone});
      uint32_t barVox = (uint32_t)bar.size();
      Vec3 barAt{120, (float)(bh3 + 8), 120};
      uint64_t bb = phys.CreateDebrisBody(bar, {120, bh3 + 8, 120}, dens);
      BodyTransform bxf{};
      bxf.pos = barAt;
      bxf.quat[3] = 1;
      debris.AdoptBody(bb, bar, bxf);

      std::vector<ParticleSpawn> bspawns;
      // centred on the neck (local 2,2,3 -> world), radius covers it only
      debris.DamageBodiesRadial(barAt + Vec3{2.5f, 2.5f, 3.5f}, 2.5f, world,
                                bspawns);
      uint32_t after = 0;
      {
        std::vector<BodyVoxInst> bi2;
        debris.BuildInstances(bi2);
        after = (uint32_t)bi2.size();
      }
      bool blastOk = after < barVox && debris.BodyCount() >= 2;
      std::printf("body blast: %s (%u -> %u voxels, %u bodies, %zu ejecta)\n",
                  blastOk ? "PASS" : "FAIL", barVox, after, debris.BodyCount(),
                  bspawns.size());
      settleOk = settleOk && blastOk;
      debris.Reset();
    }

    // laser kerf on a body: repeated melts at one spot must bore through and
    // eventually sever it. Unlike the old plane split this removes matter, so
    // the gate is "voxels went away AND the body came apart" — a cut that only
    // separated (without eating a channel) would be the old behaviour back.
    {
      SubmitWorldgen(ctx, world, sim, kDefaultSeed);
      ctx.WaitIdle();
      int bh4 = World::TerrainHeight(150, 150, kDefaultSeed);
      std::vector<DebrisVoxel> rod;  // 3x3x11 rod: cut across the middle
      for (int z = 0; z < 11; z++)
        for (int y = 0; y < 3; y++)
          for (int x = 0; x < 3; x++)
            rod.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
      uint32_t rodVox = (uint32_t)rod.size();
      Vec3 rodAt{150, (float)(bh4 + 8), 150};
      uint64_t rb = phys.CreateDebrisBody(rod, {150, bh4 + 8, 150}, dens);
      BodyTransform rxf{};
      rxf.pos = rodAt;
      rxf.quat[3] = 1;
      debris.AdoptBody(rb, rod, rxf);

      // Hold the beam on the middle of the rod for a few ticks. The handle is
      // re-read every iteration because each melt rebuilds the collider and
      // hands the body a new one — `rb` is stale after the first pass.
      std::vector<ParticleSpawn> lspawns;
      for (int i = 0; i < 12 && debris.BodyCount() < 2; i++)
        debris.MeltBodyAt(debris.BodyHandle(0), rodAt + Vec3{1.5f, 1.5f, 5.5f},
                          2.0f, world, lspawns);
      uint32_t lafter = 0;
      {
        std::vector<BodyVoxInst> bi3;
        debris.BuildInstances(bi3);
        lafter = (uint32_t)bi3.size();
      }
      bool kerfOk = lafter < rodVox && debris.BodyCount() >= 2;
      std::printf("laser kerf: %s (%u -> %u voxels, %u bodies)\n",
                  kerfOk ? "PASS" : "FAIL", rodVox, lafter, debris.BodyCount());
      settleOk = settleOk && kerfOk;
      debris.Reset();
    }

    // body burn: a rigidbody carrying embers must KEEP burning — embers decay
    // away (voxels leave the body) and emit real fire into the grid so nearby
    // flammables can catch. This is the fix for detached islands freezing
    // mid-flame forever.
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    int bh2 = World::TerrainHeight(90, 90, kDefaultSeed);
    std::vector<DebrisVoxel> plank;
    for (int z = 0; z < 5; z++)
      for (int y = 0; y < 5; y++)
        for (int x = 0; x < 5; x++)
          plank.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0,
                           (uint16_t)(y == 4 ? kMatEmber : kMatWood)});
    uint32_t plankVoxels = (uint32_t)plank.size();
    uint64_t pb = phys.CreateDebrisBody(plank, {90, bh2 + 4, 90}, dens);
    BodyTransform pxf{};
    pxf.pos = Vec3{90, (float)(bh2 + 4), 90};
    pxf.quat[3] = 1;
    debris.AdoptBody(pb, plank, pxf);
    uint32_t fireOps = 0;
    t = 9000;
    for (int i = 0; i < 90 && debris.BodyCount() > 0; i++) {
      std::vector<CellOp> cellOps;
      std::vector<ParticleSpawn> spawns;
      debris.PreTick(t + 1, world, cellOps, spawns);
      for (const CellOp& op : cellOps)
        if ((op.word & 0xFFFu) == kMatFire) fireOps++;
      ++t;
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cellOps, false,
                 {5, bh2 / 16, 5}, true, false, spawns);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      phys.Step(kTickDt);
      debris.PostStep();
    }
    std::vector<BodyVoxInst> burnInst;
    debris.BuildInstances(burnInst);
    bool burnOk = fireOps > 5 && (uint32_t)burnInst.size() < plankVoxels;
    std::printf("body burn: %s (%u fire ops emitted, %u -> %zu voxels)\n",
                burnOk ? "PASS" : "FAIL", fireOps, plankVoxels,
                burnInst.size());
    settleOk = settleOk && burnOk;
    debris.Reset();

    // body shatter: burn through a dumbbell's ember bridge and the small
    // clump must disconnect and re-enter the world as ballistic particles
    // (the big plate keeps the body). Spin the body so it never settles.
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<DebrisVoxel> bell;
    // The plate is 5x5, not 3x3, and that margin is the point. The ember does
    // not merely burn the bridge: it ignites the wood it touches, so the plate
    // is losing voxels the whole time the bridge is burning through. A 3x3
    // plate (9 voxels) erodes past the 8-voxel dissolve floor BEFORE the
    // connectivity check ever separates the clump, so the whole dumbbell went
    // to particles and the test measured erosion instead of shattering.
    // 25 voxels outlast the bridge with room to spare.
    for (int y = 0; y < 5; y++)  // 5x5x1 plate at z=0
      for (int x = 0; x < 5; x++)
        bell.push_back({(int8_t)x, (int8_t)y, 0, 0, kMatWood});
    bell.push_back({2, 2, 1, 0, kMatEmber});  // bridge, centred on the plate
    bell.push_back({2, 2, 2, 0, kMatWood});   // 4-voxel clump beyond it
    bell.push_back({1, 2, 2, 0, kMatWood});
    bell.push_back({3, 2, 2, 0, kMatWood});
    bell.push_back({2, 1, 2, 0, kMatWood});
    uint64_t db = phys.CreateDebrisBody(bell, {90, bh2 + 6, 90}, dens);
    BodyTransform dxf{};
    dxf.pos = Vec3{90, (float)(bh2 + 6), 90};
    dxf.quat[3] = 1;
    debris.AdoptBody(db, bell, dxf);
    phys.SetBodyVelocities(db, Vec3{0, 0, 0}, Vec3{0.4f, 1.2f, 0.3f});
    uint32_t spawnsSeen = 0;
    t = 10000;
    for (int i = 0; i < 400 && debris.BodyCount() > 0 && spawnsSeen < 3; i++) {
      std::vector<CellOp> cellOps;
      std::vector<ParticleSpawn> spawns;
      debris.PreTick(t + 1, world, cellOps, spawns);
      spawnsSeen += (uint32_t)spawns.size();
      ++t;
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cellOps, false,
                 {5, bh2 / 16, 5}, true, true, spawns);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      phys.Step(kTickDt);
      debris.PostStep();
    }
    bool shatterOk = spawnsSeen >= 3 && debris.BodyCount() == 1;
    std::printf("body shatter: %s (%u fragment voxels -> particles, %u bodies)\n",
                shatterOk ? "PASS" : "FAIL", spawnsSeen, debris.BodyCount());
    settleOk = settleOk && shatterOk;
    debris.Reset();
  }

  // player↔body (deferred from M6): the kinematic player proxy must register
  // debris overlap as a depenetration push, and read clear when separated.
  // Pure narrow-phase — no stepping between spawn and query, so deterministic.
  bool pushOk = false;
  {
    std::vector<float> dens;
    for (const auto& m : mats) dens.push_back((float)m.gpu.density);
    auto stoneBlock = [&](IVec3 origin) {
      std::vector<DebrisVoxel> vox;
      for (int z = 0; z < 3; z++)
        for (int y = 0; y < 3; y++)
          for (int x = 0; x < 3; x++)
            vox.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
      return phys.CreateDebrisBody(vox, origin, dens);
    };
    uint64_t pb = phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);
    Vec3 at{500.0f, 500.0f, 500.0f};  // far from the debris-test terrain
    phys.MovePlayerBody(pb, at, kTickDt);
    phys.Step(kTickDt);  // proxy reaches its target
    uint64_t nearBody = stoneBlock({499, 499, 499});  // straddles the capsule
    float pushNear = phys.PlayerPushOut(pb, at).len();
    phys.RemoveBody(nearBody);
    uint64_t farBody = stoneBlock({520, 500, 500});
    float pushFar = phys.PlayerPushOut(pb, at).len();
    phys.RemoveBody(farBody);

    // mass-relative shove: the proxy is dynamic with a real mass, so walking
    // into a light sphere must move it far more than the same walk into a
    // heavy one (both fall freely — only horizontal displacement counts).
    auto walkInto = [&](float density) {
      at = Vec3{500.0f, 500.0f, 500.0f};
      phys.MovePlayerBody(pb, at, kTickDt);
      phys.Step(kTickDt);
      const float startX = 509.5f;  // just clear of capsule(4.8) + sphere(4)
      uint64_t s = phys.CreateSphereBody({startX, 500.0f, 500.0f}, 4.0f, density);
      for (int i = 0; i < 12; i++) {
        at.x += 2.2f;  // ~4.2 m/s walk
        phys.MovePlayerBody(pb, at, kTickDt);
        phys.Step(kTickDt);
      }
      BodyTransform xf{};
      phys.GetTransform(s, xf);
      phys.RemoveBody(s);
      return xf.pos.x - startX;
    };
    float lightMoved = walkInto(150.0f);    // ~10 kg beach ball
    float heavyMoved = walkInto(12000.0f);  // ~780 kg lead sphere
    phys.RemoveBody(pb);
    bool shoveOk = lightMoved > 2.0f && lightMoved > 3.0f * heavyMoved;
    pushOk = pushNear > 0.01f && pushFar < 1e-3f && shoveOk;
    std::printf(
        "player body: %s (overlap push %.2f vox, clear push %.3f vox, "
        "shove light %.1f vox vs heavy %.1f vox)\n",
        pushOk ? "PASS" : "FAIL", pushNear, pushFar, lightMoved, heavyMoved);
  }

  // M2 save/load: snapshot at tick 100, diverge 50 ticks, load — the world
  // hash must return exactly to the snapshot value (stamp bytes excluded).
  bool saveOk = false;
  {
    const char* kPath = "selftest_world.svd";
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    uint32_t t = 3000;
    for (int i = 0; i < 100; i++)
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, SelftestOps(i), {}, {}, false,
                 {8, 3, 8}, false, false);
    ctx.WaitIdle();
    uint32_t h1 = HashWorldNow(ctx, world, sim, kDefaultSeed);
    bool saved = SaveWorld(ctx, world, stream, kPath);
    for (int i = 100; i < 150; i++)
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, SelftestOps(i), {}, {}, false,
                 {8, 3, 8}, false, false);
    ctx.WaitIdle();
    uint32_t hDiverged = HashWorldNow(ctx, world, sim, kDefaultSeed);
    bool loaded = LoadWorld(ctx, world, sim, stream, kPath);
    uint32_t h2 = HashWorldNow(ctx, world, sim, kDefaultSeed);
    saveOk = saved && loaded && h1 == h2 && h1 != hDiverged;
    std::printf("save/load: %s (hash %08x -> diverged %08x -> restored %08x)\n",
                saveOk ? "PASS" : "FAIL", h1, hDiverged, h2);
    stream.Store().Unbind();  // detach before deleting the directory
    std::filesystem::remove_all(kPath);
  }

  // region store: RAM must stay bounded past kMaxRamRegions (LRU spill to
  // region files) and spilled chunks must read back from disk intact.
  bool storeOk = false;
  {
    const char* kDir = "selftest_store.svd";
    ChunkStore cs;
    storeOk = cs.BindSave(kDir);
    const size_t kRegions = ChunkStore::kMaxRamRegions + 16;
    for (size_t i = 0; i < kRegions; i++) {
      // one chunk per region: a full-chunk run of a per-region material
      std::vector<uint16_t> rle = {(uint16_t)kChunkVol,
                                   (uint16_t)(kMatStone + (i % 3))};
      cs.Put({(int)i * 16, 0, 0}, std::move(rle));
    }
    size_t ramAfterPuts = cs.Count();
    for (size_t i = 0; i < kRegions && storeOk; i++) {
      const std::vector<uint16_t>* rle = cs.Get({(int)i * 16, 0, 0});
      storeOk = rle && rle->size() == 2 && (*rle)[0] == (uint16_t)kChunkVol &&
                (*rle)[1] == (uint16_t)(kMatStone + (i % 3));
    }
    storeOk = storeOk && ramAfterPuts <= ChunkStore::kMaxRamRegions;
    std::printf("region store: %s (%zu regions written, %zu chunks in RAM "
                "after puts)\n",
                storeOk ? "PASS" : "FAIL", kRegions, ramAfterPuts);
    cs.Unbind();
    std::filesystem::remove_all(kDir);
  }

  // M2/M7 streaming: (a) slide the residency window +X across many shifts,
  // twice — the per-tick hash sequences must match exactly (streaming +
  // procgen are deterministic); (b) edit a far chunk, walk past its eviction,
  // walk back, and verify the edit survived the store roundtrip.
  bool streamOk = false;
  {
    std::vector<uint32_t> shash[2];
    for (int run = 0; run < 2; run++) {
      stream.OnRegen();
      world.SetWindowOrigin({0, 0, 0});
      SubmitWorldgen(ctx, world, sim, kDefaultSeed);
      ctx.WaitIdle();
      uint32_t t = 5000;
      for (int i = 0; i < 300; i++) {
        IVec3 pc{8 + i / 10, 8, 8};  // one chunk every 10 ticks -> 30 shifts
        stream.Update(pc);
        SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, true, pc,
                   false, false);
        shash[run].push_back(ReadHashSync(ctx, world));
      }
    }
    bool sdet = shash[0] == shash[1];

    // persistence roundtrip (live readbacks so eviction filters see reality)
    stream.OnRegen();
    world.SetWindowOrigin({0, 0, 0});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    uint32_t t = 7000;
    auto tickAt = [&](IVec3 pc, std::vector<BrushOp> ops) {
      stream.Update(pc);
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, ops, {}, {}, false, pc,
                 true, false);
      ctx.WaitIdle();
      ctx.ProcessEvents();
    };
    const int ballX = 25 * 16 + 8, ballZ = 128;
    int ballH = World::TerrainHeight(ballX, ballZ, kDefaultSeed);
    IVec3 ballCell{ballX, ballH + 1, ballZ};
    IVec3 ballChunk{ballCell.x >> 4, ballCell.y >> 4, ballCell.z >> 4};
    auto walkTo = [&](int fromCx, int toCx) {
      int step = toCx > fromCx ? 1 : -1;
      for (int cx = fromCx; cx != toCx; cx += step)
        for (int k = 0; k < 10; k++) tickAt({cx, 8, 8}, {});
    };
    walkTo(8, 25);
    // glass ball half-buried at the surface (anchored: resting on the ground)
    tickAt({25, 8, 8}, {{ballCell.x, ballCell.y, ballCell.z, 3, kMatGlass, 1, 0, 0}});
    stream.MarkModifiedBox({ballCell.x - 3, ballCell.y - 3, ballCell.z - 3},
                           {ballCell.x + 3, ballCell.y + 3, ballCell.z + 3});
    for (int k = 0; k < 10; k++) tickAt({25, 8, 8}, {});
    walkTo(25, 60);  // ball chunk streams out (origin.x reaches 52 > 25)
    bool evicted = !world.ChunkInWindow(ballChunk);
    walkTo(60, 25);  // and back in
    world.RequestChunkFetch(ballChunk);
    uint32_t glass = 0;
    for (int k = 0; k < 90; k++) {
      tickAt({25, 8, 8}, {});
      const CachedChunk* cc = world.Cached(ballChunk);
      if (cc && cc->version > t - 30 && cc->voxels.size() == kChunkVol) {
        for (uint32_t w : cc->voxels)
          if ((w & 0xFFFu) == kMatGlass) glass++;
        break;
      }
      world.RequestChunkFetch(ballChunk);
    }
    // a REAL player (collision through the async mirror) flies +X far beyond
    // the original 256-box — the literal M2 exit criterion. Catches any
    // leftover fixed-world assumption in the player/collision path (a v0
    // position clamp produced exactly this bug: an invisible wall at x=254).
    bool crossed = false;
    {
      std::vector<uint32_t> classOf;
      for (auto& m : mats) classOf.push_back(m.gpu.klass);
      Player p2;
      p2.fly = true;
      p2.pos = Vec3{140.5f, 110.0f, 140.5f};  // above the tallest hills (~90)
      auto kindAt = [&](IVec3 c) { return world.KindAt(c, classOf); };
      PlayerInput in{};
      in.forward = 1.0f;
      in.sprint = true;
      for (int i = 0; i < 1200 && !crossed; i++) {
        IVec3 pc{ifloor(p2.pos.x) >> 4, ifloor(p2.pos.y) >> 4,
                 ifloor(p2.pos.z) >> 4};
        stream.Update(pc);
        SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false, pc,
                   true, false);
        ctx.WaitIdle();
        ctx.ProcessEvents();
        p2.Update(1.0f / 30.0f, in, Vec3{1, 0, 0}, Vec3{0, 0, 1}, Vec3{1, 0, 0},
                  kindAt);
        crossed = p2.pos.x > 600.0f;
      }
      std::printf("  player flight: %s (reached x=%.0f, window origin.x=%d)\n",
                  crossed ? "crossed" : "BLOCKED", (double)p2.pos.x,
                  world.WindowOrigin().x);
    }

    streamOk = sdet && evicted && glass > 0 && crossed;
    std::printf("streaming: %s (hash sequences %s over %u shifts, ball chunk "
                "evicted=%d, %u glass voxels after re-entry, player crossed=%d, "
                "store %zu chunks)\n",
                streamOk ? "PASS" : "FAIL", sdet ? "match" : "DIVERGE",
                stream.ShiftCount(), evicted ? 1 : 0, glass, crossed ? 1 : 0,
                stream.Store().Count());
  }

  bool perfOk = simMs < 8.0 && bestFrameMs < 16.0;
  std::printf("perf: %s\n", perfOk ? "PASS" : "MARGINAL (see numbers above)");
  bool pass = deterministic && walkOk && sleepOk && pondOk && evapOk && stainOk && fullOk && debrisOk &&
              prefabOk && mobOk && settleOk && pushOk && saveOk && storeOk &&
              streamOk && farDownOk && fogOk;
  std::printf("=== selftest %s ===\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
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

}  // namespace

int main(int argc, char** argv) {
  bool selftest = false;
  bool shot = false;
  bool lowPowerAdapter = false;
  bool noAudio = false;  // --noaudio: run silent (also implied by every headless mode)
  std::string shotMob;  // --shot-mob <def>[:limb,...] (mob pose look iteration)
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--selftest") selftest = true;
    if (a == "--shot") shot = true;  // screenshots only (look iteration)
    if (a == "--shot-mob" && i + 1 < argc) shotMob = argv[++i];
    if (a == "--noaudio") noAudio = true;
    // `--time 0..1` sets the time of day for --shot: 0 = midnight, 0.25 =
    // sunrise, 0.5 = noon, 0.75 = sunset. Lets the sky be judged at any point
    // in the cycle without waiting for it.
    if (a == "--time" && i + 1 < argc) {
      g_shotTimeOfDay = std::fmod(std::atof(argv[++i]), 1.0);
      if (g_shotTimeOfDay < 0.0f) g_shotTimeOfDay += 1.0f;
    }
    // `--adapter low` picks the LowPower adapter (iGPU) so the selftest hash
    // can be compared across GPU vendors (DESIGN.md §14 risk 3).
    if (a == "--adapter" && i + 1 < argc) lowPowerAdapter = std::string(argv[++i]) == "low";
  }

  std::string assetDir = AssetDir();
  // Tuning first: LoadShader() bakes these into every shader's constant
  // prelude, so they have to be live before the first pipeline build.
  {
    Tuning tune;
    LoadTuning(assetDir + "/materials/tuning.json", tune);
    for (const std::string& w : tune.warnings)
      std::fprintf(stderr, "tuning: %s\n", w.c_str());
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
  if (!selftest && !shot && shotMob.empty()) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window = glfwCreateWindow(1600, 900, "sandvox", nullptr, nullptr);
    if (!window) return 1;
  }

  GpuContext ctx;
  if (!ctx.Init(window, 1600, 900, lowPowerAdapter)) return 1;

  World world;
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
    sim.UploadMicroBodies(ctx.queue, mbSet);
    mobs.SetDefs(std::move(mobDefs));
  }
  Stream stream;
  stream.Init(&ctx, &world, &sim, kDefaultSeed);
  stream.OnMaterialsReloaded(mats);
  FarField far;
  far.Init(&world);

  if (shot) return RunShots(ctx, world, sim);
  if (!shotMob.empty())
    return RunMobShot(ctx, world, sim, phys, debris, mobs, shotMob);
  if (selftest)
    return RunSelftest(ctx, world, sim, mats, phys, debris, mobs, stream);

  Overlay overlay;
  if (!overlay.Init(window, ctx.device, ctx.surfaceFormat)) return 1;

  // Audio comes up HERE, after the three headless modes have returned: none of
  // --shot/--shot-mob/--selftest should ever open a sound device (there is no
  // audio hardware in CI, and a selftest that depends on one is not a test).
  // A failed init is not an error anywhere — the game runs silent.
  audio::Cues audioCues;
  if (!noAudio) audioCues.Init(assetDir + "/sounds", mats);

  UIState ui;
  for (auto& m : mats) {
    ui.materialNames.push_back(m.name);
    ui.materialColors.push_back(m.gpu.color0);
  }

  // material class LUT for the player's mirror queries
  std::vector<uint32_t> classOf;
  for (auto& m : mats) classOf.push_back(m.gpu.klass);

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);

  Camera cam;
  Player player;
  Brush brush;
  PrefabPlacer placer;
  // The player avatar shares MobSystem's def list rather than loading its own:
  // one micro-body pool, and a hot reload (R) rebuilds both at once. It points
  // at whichever def is named `avatarDefName`, so swapping the player
  // character is data, not code.
  const std::string avatarDefName = kAvatarDefName;
  PlayerAvatar avatar;
  avatar.Init(&phys, &world, &debris, mats);
  avatar.SetDefs(&mobs.Defs(), avatarDefName);
  ThirdPersonRig tpRig;
  CameraMode camMode = CameraMode::First;
  float avatarHeading = 0.0f;   // body facing, radians about +Y
  float fovNow = CurrentTuning().camera.fovY;
  float respawnTimer = 0.0f;
  // Clear-then-fill, matching the hot-reload path below. These run once here,
  // but an append-only build of a list the UI indexes into is exactly how a
  // duplicate entry (and the ImGui ID collision that follows) gets introduced.
  ui.prefabNames.clear();
  ui.mobNames.clear();
  for (const Prefab& p : prefabs) ui.prefabNames.push_back(p.name);
  for (const MobDef& d : mobs.Defs()) ui.mobNames.push_back(d.name);
  int spawnH = World::TerrainHeight(140, 140, kDefaultSeed);
  player.pos = Vec3{140, (float)(spawnH + 10), 140};
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

  KeyEdge eP, eN, eV, eF1, eF5, eF9, eF10, eR, eEsc, eLBracket, eRBracket, eJump,
      eG, eX, eB, eT, eO, eM, eK, eTab, eC, eH;
  bool prevMouseL = false;
  std::vector<Grenade> grenades;
  // particle-pass gating: tick-deterministic inputs only (see SubmitTick note)
  bool everExploded = false;
  uint32_t lastExplosionTick = 0;
  uint32_t tick = 0;
  uint32_t bodyInstCount = 0;
  // Per-frame render scratch, hoisted so the steady state reuses capacity.
  std::vector<BodyXformGpu> bodyXf;
  std::vector<MicroBodyInstGpu> microInsts;
  double lastTime = NowSec();
  double accumulator = 0;
  float fpsSmooth = 0, frameMsSmooth = 0, tickMsSmooth = 0, frameMsWorst = 0;
  double fpsWinStart = lastTime, fpsWinWorst = 0;
  int fpsWinFrames = 0;
  // Adaptive fog (plan phase 3B): the queue is drained by whole planes, so the
  // trusted radius jumps in steps. Start at the cold-start ceiling (nothing is
  // filled yet at this point — FullRefill above just queued everything) and
  // ease outward as the bands land.
  float fogSmooth = kFarFogDensityMax;

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    double now = NowSec();
    float dt = (float)(now - lastTime);
    lastTime = now;
    // Presented rate = frames / wall-clock over a window. An EMA of the
    // instantaneous 1/dt over-weights the fast frames whenever the CPU races
    // ahead of a GPU-bound present queue (several ~5 ms loops, one long
    // block), and reads 100+ while the screen updates at <10.
    fpsWinFrames++;
    fpsWinWorst = std::max(fpsWinWorst, (double)dt);
    if (now - fpsWinStart >= 0.5) {
      fpsSmooth = (float)(fpsWinFrames / (now - fpsWinStart));
      frameMsSmooth = (float)(1000.0 * (now - fpsWinStart) / fpsWinFrames);
      frameMsWorst = (float)(fpsWinWorst * 1000.0);
      fpsWinFrames = 0;
      fpsWinWorst = 0;
      fpsWinStart = now;
    }

    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    if (fbw > 0 && fbh > 0 && ((uint32_t)fbw != ctx.width || (uint32_t)fbh != ctx.height))
      ctx.Resize(fbw, fbh);

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
    if (captured) cam.ApplyMouse((float)(mx - mx0), (float)(my - my0));
    mx0 = mx;
    my0 = my;

    if (eP.Pressed(key(GLFW_KEY_P))) ui.paused = !ui.paused;
    if (eN.Pressed(key(GLFW_KEY_N))) ui.stepOnce = true;
    if (eV.Pressed(key(GLFW_KEY_V))) ui.fly = !ui.fly;
    if (eF1.Pressed(key(GLFW_KEY_F1))) ui.visible = !ui.visible;
    if (eF5.Pressed(key(GLFW_KEY_F5))) ui.reloadShaders = true;
    if (eF9.Pressed(key(GLFW_KEY_F9))) ui.saveWorld = true;
    if (eF10.Pressed(key(GLFW_KEY_F10))) ui.loadWorld = true;
    if (eR.Pressed(key(GLFW_KEY_R))) ui.reloadMaterials = true;
    if (eLBracket.Pressed(key(GLFW_KEY_LEFT_BRACKET)))
      ui.brushRadius = std::max(1, ui.brushRadius - 1);
    if (eRBracket.Pressed(key(GLFW_KEY_RIGHT_BRACKET)))
      ui.brushRadius = std::min(7, ui.brushRadius + 1);
    for (int i = 0; i < 8; i++)
      if (key(GLFW_KEY_1 + i) && i + 1 < (int)mats.size()) ui.brushMaterial = i + 1;

    if (captured && eG.Pressed(key(GLFW_KEY_G))) {
      Grenade g;
      g.pos = player.EyePos() + cam.Forward() * 2.0f;
      g.vel = cam.Forward() * (CurrentTuning().grenade.throwSpeed / kVoxelMeters) +
              player.vel;
      g.fuse = CurrentTuning().grenade.fuse;
      grenades.push_back(g);
    }
    if (captured && eX.Pressed(key(GLFW_KEY_X))) ui.pendingDetonate = true;
    if (captured && eTab.Pressed(key(GLFW_KEY_TAB)))
      ui.tool = (ui.tool + 1) % UIState::kToolCount;
    if (captured && eM.Pressed(key(GLFW_KEY_M))) ui.spawnMob = true;
    if (captured && eB.Pressed(key(GLFW_KEY_B))) ui.placePrefab = true;
    if (captured && eK.Pressed(key(GLFW_KEY_K))) ui.spawnSphere = true;
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
        SetCurrentTuning(tune);
        // Gore variance is drawn per mob at spawn, so mobs already standing in
        // the world hold profiles from the OLD tuning. Re-draw them here or an
        // edit to the randomness controls appears to do nothing until the next
        // spawn. Same id -> same draw, so a mob keeps its identity unless the
        // variance settings themselves changed.
        mobs.RefreshGoreProfiles();
      }
      std::printf("reloading shaders... %s\n",
                  sim.ReloadShaders(ctx.device, ctx.instance) ? "ok" : "FAILED (kept old)");
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
        classOf.clear();
        for (auto& m : mats) classOf.push_back(m.gpu.klass);
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
      player.viewYOffset = 0.0f;  // teleport: never smooth across it
      tick = 0;
      grenades.clear();
      everExploded = false;
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
      SaveWorld(ctx, world, stream, "world.svd");
    }
    if (ui.loadWorld) {
      ui.loadWorld = false;
      ctx.WaitIdle();
      if (LoadWorld(ctx, world, sim, stream, "world.svd")) {
        grenades.clear();
        everExploded = false;
        debris.Reset();
        mobs.Reset();
        avatar.Despawn();
        tpRig.Snap();
      }
    }

    // ---- player (per frame, against the latest one-tick-latent mirror) ----
    player.fly = ui.fly;
    auto kindAt = [&](IVec3 c) { return world.KindAt(c, classOf); };
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
    prevMouseL = mouseL;
    if (mouseLClick && ui.tool == UIState::kToolPrefab) ui.placePrefab = true;
    if (mouseLClick && ui.tool == UIState::kToolMob) ui.spawnMob = true;
    bool laserHeld =
        captured && (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS ||
                     (ui.tool == UIState::kToolLaser && mouseL));
    bool brushActive = ui.tool == UIState::kToolBrush;
    while (accumulator >= kTickDt && ticksThisFrame < 4) {
      accumulator -= kTickDt;
      if (ui.paused && !ui.stepOnce) break;
      ui.stepOnce = false;
      tick++;
      ticksThisFrame++;

      // recenter the residency window on the player (between ticks only; at
      // most one 1-chunk shift per axis)
      IVec3 playerChunkNow{ifloor(player.pos.x) >> 4, ifloor(player.pos.y) >> 4,
                           ifloor(player.pos.z) >> 4};
      stream.Update(playerChunkNow);
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
        Vec3 eye = player.EyePos(), fwd = cam.Forward();
        float gridDist = 1e9f;
        IVec3 hit{};
        if (lsnap.valid && lsnap.pick[0] != 0) {
          hit = {(int)lsnap.pick[2], (int)lsnap.pick[3], (int)lsnap.pick[4]};
          gridDist = (Vec3{hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f} - eye).len();
        }
        float frac = 1.0f;
        const float kLaserRange = CurrentTuning().tools.laserRange;
        uint64_t hitBody = phys.CastRayBody(eye, fwd, kLaserRange, frac);
        float bodyDist = frac * kLaserRange;

        if (hitBody != 0 && bodyDist < gridDist) {
          // body cut (PLAN §C2): mob limbs take damage (instant sever when
          // the beam crosses a joint); plain debris is MELTED where the beam
          // lands. The beam bores a channel tick by tick and the body splits
          // when that channel actually severs it (DebrisSystem::MeltBodyAt) —
          // no cutting plane is chosen, so what falls apart is decided by the
          // geometry the player carved, not by camera orientation.
          Vec3 hitPos = eye + fwd * bodyDist;
          // The avatar is checked alongside mobs — a beam that crosses the
          // player's own joint takes that part off exactly as it would a
          // mob's, with no avatar-specific damage path.
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
              packed.scale = kSphereScale;
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
        float wantHeading = camHeading;
        if (camMode != CameraMode::First) {
          Vec3 v{player.vel.x, 0, player.vel.z};
          float sp = v.len() * kVoxelMeters;   // voxels/s -> m/s
          if (sp > av.turnMinSpeed) wantHeading = std::atan2(v.x, v.z);
          else wantHeading = avatarHeading;    // too slow: hold, don't spin
        }
        // Shortest-arc turn toward the goal, rate-limited.
        float d = wantHeading - avatarHeading;
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        // FIRST PERSON DOES NOT EASE. The rate limit exists so a third-person
        // body pivots on its feet instead of snapping to face its travel
        // direction. In first person there is no body to watch turn — the
        // camera IS the head, so any lag means the torso is yawed away from
        // the view while the arms stay welded to the torso. Both of them then
        // hang off to one side of the screen until the ease catches up, which
        // at turnRate 12 rad/s is most of a fast mouse turn. Snapping is
        // correct here: the thing the lag was protecting is off-screen.
        if (camMode == CameraMode::First) {
          avatarHeading = wantHeading;
        } else {
          float maxStep = av.turnRate * kTickDt;
          avatarHeading += std::clamp(d, -maxStep, maxStep);
        }

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
      }

      // support-loss flags from the sim (burnt stems, undermined slabs) feed
      // the same island-check pipeline as explosions and brush erases
      debris.QueueSupportEvents(world.Snap());
      // island detection results + body burn + terrain collision upkeep
      // (may add cell ops and particle spawns from shattered bodies)
      std::vector<CellOp> cellOps;
      debris.PreTick(tick, world, cellOps, spawns);
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
      // prefab stamps drain after island ops (they win same-cell conflicts)
      placer.PreTick(world, cellOps);

      // explosions: X-detonate at the crosshair + grenade fuses
      std::vector<ExplosionOp> exps;
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

      IVec3 pc{ifloor(player.pos.x) / (int)kChunk, ifloor(player.pos.y) / (int)kChunk,
               ifloor(player.pos.z) / (int)kChunk};
      double t0 = NowSec();
      phys.MovePlayerBody(playerBody, player.pos, kTickDt);
      SubmitTick(ctx, world, sim, tick, kDefaultSeed, ops, exps, cellOps,
                 tick % 15 == 0 /*hash occasionally*/, pc, true, particlesActive,
                 spawns, farCount);
      phys.Step(kTickDt);   // CPU physics overlaps the GPU tick
      debris.PostStep();
      mobs.PostStep();
      avatar.PostStep();
      // debris that ended the step overlapping the player pushes the player
      // out (fly mode ignores collision entirely, matching the voxel rules)
      if (!player.fly)
        player.ApplyPush(phys.PlayerPushOut(playerBody, player.pos), kindAt);
      tickMsSmooth += ((float)((NowSec() - t0) * 1000.0) - tickMsSmooth) * 0.1f;
    }
    if (ui.paused) accumulator = std::min(accumulator, (double)kTickDt);

    // ---- render ----
    wgpu::TextureView target = ctx.AcquireFrame();
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
          hide.assign(avatar.Def() ? avatar.Def()->limbs.size() : 0, 0);
          if (camMode == CameraMode::First) {
            for (size_t i = 0; i < hide.size(); i++) hide[i] = 1;
            if (CurrentTuning().avatar.firstPersonArms) {
              const int keep[7] = {p.armUL, p.armUR, p.handL, p.handR,
                                   p.staff, -1, -1};
              for (int k : keep)
                if (k >= 0 && k < (int)hide.size()) hide[k] = 0;
            }
          }
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
        audioCues.Update(dt, eye, cam.yaw, cam.pitch, &world);
      }
      avatar.ClearFootfalls();
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
                        (float)now, fogSmooth, (float)ctx.height, tick);

      ui.fps = fpsSmooth;
      ui.frameMs = frameMsSmooth;
      ui.frameMsWorst = frameMsWorst;
      ui.tickCpuMs = tickMsSmooth;
      ui.tick = tick;
      ui.activeChunks = world.Snap().activeChunks;
      ui.voxelTotal = world.Snap().voxelTotal;
      ui.worldHash = world.Snap().worldHash;
      ui.mirrorValid = world.Snap().valid;
      ui.particleCount = world.Snap().particleCount;
      ui.bodyCount = debris.BodyCount();
      ui.activeBodyCount = debris.ActiveBodyCount();
      ui.prefabPending = (uint32_t)placer.PendingCount();
      ui.mobCount = mobs.MobCount();
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
      overlay.Draw(ui);

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
        Vec3 from = player.EyePos() + cam.Forward() * 1.2f +
                    cam.Right() * 0.7f - cam.Up() * 0.5f;
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
      if (debris.InstancesDirty() || mobs.InstancesDirty() ||
          avatar.InstancesDirty()) {
        std::vector<BodyVoxInst> inst;
        debris.BuildInstances(inst);
        mobs.AppendInstances(inst, debris.BodyCount());
        avatar.AppendInstances(inst,
                               debris.BodyCount() + mobs.LimbBodyCount());
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
      if (debris.BodyCount() + mobs.LimbBodyCount() +
              avatar.LimbBodyCount() >
          0) {
        BuildBodyXforms(debris, mobs, &avatar, bodyXf);
        ctx.queue.WriteBuffer(world.bodyXforms, 0, bodyXf.data(),
                              bodyXf.size() * sizeof(BodyXformGpu));
        BuildMicroInsts(debris, mobs, &avatar, microInsts);
      }

      wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      wgpu::RenderPassEncoder rp = sim.BeginRenderPass(enc, target, ctx.surfaceFormat,
                                                       ctx.width, ctx.height);
      sim.DrawWorld(rp);
      sim.DrawParticles(rp);
      sim.DrawBodies(rp, bodyInstCount);
      sim.DrawMicroBodies(rp, ctx.queue, microInsts);
      sim.DrawSprites(rp, (uint32_t)sprv.size());
      overlay.Render(rp);
      rp.End();
      wgpu::CommandBuffer cmd = enc.Finish();
      ctx.queue.Submit(1, &cmd);
      ctx.Present();
    }
    ctx.ProcessEvents();  // pumps MapAsync callbacks (mirror updates)
  }

  ctx.WaitIdle();
  // Audio down before anything it points at: Shutdown stops the device, which
  // is the only thread that can still be inside the mixer.
  if (audioCues.Enabled()) {
    const audio::Cues::Stats& as = audioCues.GetStats();
    std::printf("[audio] %u steps, %u landings, %u impacts, %u dropped\n",
                as.steps, as.lands, as.impacts, as.dropped);
  }
  audioCues.Shutdown();
  overlay.Shutdown();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
