// sandvox — 3D falling-sand voxel engine (v0). See DESIGN.md.
// Fixed 30 Hz GPU simulation, uncapped raymarched rendering, walkable player,
// JSON materials, deterministic kernels with per-tick world hash.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "game/brush.h"
#include "game/camera.h"
#include "game/mob.h"
#include "game/player.h"
#include "game/prefab.h"
#include "gpu/context.h"
#include "gpu/resources.h"
#include "math3d.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/materials.h"
#include "sim/simulation.h"
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

void WriteRenderParams(const wgpu::Queue& queue, const World& world,
                       const Vec3& eye, const Camera& cam, float aspect,
                       bool shadows, float time) {
  RenderParams rp{};
  Vec3 f = cam.Forward(), r = cam.Right(), u = cam.Up();
  rp.camPos[0] = eye.x; rp.camPos[1] = eye.y; rp.camPos[2] = eye.z;
  rp.camRight[0] = r.x; rp.camRight[1] = r.y; rp.camRight[2] = r.z;
  rp.camUp[0] = u.x; rp.camUp[1] = u.y; rp.camUp[2] = u.z;
  rp.camFwd[0] = f.x; rp.camFwd[1] = f.y; rp.camFwd[2] = f.z;
  rp.tanHalfFov = std::tan(cam.fovY * 0.5f);
  rp.aspect = aspect;
  rp.time = time;
  rp.flags = shadows ? 1u : 0u;
  Vec3 sun = Vec3{0.45f, 0.78f, 0.32f}.normalized();
  rp.sunDir[0] = sun.x; rp.sunDir[1] = sun.y; rp.sunDir[2] = sun.z;
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
                IVec3 playerChunk, bool wantReadback, bool particlesActive) {
  particlesActive = particlesActive || !exps.empty();
  uint32_t cellCount = std::min((uint32_t)cells.size(), kMaxCellOpsPerTick);
  TickParams tp{tick, seed, (uint32_t)ops.size(), hashEnable ? 1u : 0u,
                (uint32_t)exps.size(), sim.Page(), cellCount, 0};
  IVec3 wo = world.WindowOrigin();
  tp.origin[0] = wo.x; tp.origin[1] = wo.y; tp.origin[2] = wo.z;
  ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
  if (!ops.empty())
    ctx.queue.WriteBuffer(world.opsBuf, 0, ops.data(), ops.size() * sizeof(BrushOp));
  if (!exps.empty())
    ctx.queue.WriteBuffer(world.expOps, 0, exps.data(), exps.size() * sizeof(ExplosionOp));
  if (cellCount > 0)
    ctx.queue.WriteBuffer(world.cellOps, 0, cells.data(), cellCount * sizeof(CellOp));
  if (particlesActive) {
    // the write page starts each tick empty; survivors + emissions repopulate
    uint32_t zero = 0;
    ctx.queue.WriteBuffer(world.particleCounts, (1 - sim.Page()) * 4, &zero, 4);
  }

  wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  sim.EncodeTick(enc, (uint32_t)ops.size(), hashEnable, (uint32_t)exps.size(),
                 particlesActive, cellCount);
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
  {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    uint32_t t = 0;
    for (int i = 0; i < 500; i++) {  // seeds sprout+harden, pools settle
      std::vector<ExplosionOp> exps;
      if (i == 30) exps.push_back({110, 76, 110, 12, 350, 0, 0, 0});  // wood slab
      bool pactive = i >= 30 && i < 460;
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, exps, {}, false, {8, 3, 8},
                 false, pactive);
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
  std::printf("sleep: %s (%u / 4096 chunks active, %u particles alive after "
              "600 settle ticks)\n",
              sleepOk ? "PASS" : "FAIL", sleepActive, particlesLeft);

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

  // render perf: offscreen 1080p
  const uint32_t W = 1920, H = 1080;
  wgpu::TextureDescriptor td{};
  td.size = {W, H, 1};
  td.format = wgpu::TextureFormat::RGBA8Unorm;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  wgpu::Texture offscreen = ctx.device.CreateTexture(&td);
  wgpu::TextureView view = offscreen.CreateView();

  Camera cam;
  cam.yaw = 0.785f;   // overlook the water pool at (176,176)
  cam.pitch = -0.5f;
  Vec3 eye{108, 122, 108};

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

  // screenshot
  {
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
    if (got && WriteBmp("screenshot.bmp", pixels, W, H))
      std::printf("wrote screenshot.bmp\n");
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
      debris.PreTick(t + 1, world, cellOps);
      ++t;
      SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, exps, cellOps, false,
                 {3, (h + 10) / 16, 3}, true, i >= 40 && i < 380);
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
      int h = World::TerrainHeight(140, 140, kDefaultSeed);
      uint32_t t = 6000;
      auto mobTick = [&](std::vector<BrushOp> ops) {
        mobs.PreTick(t + 1, world, ops);
        debris.QueueSupportEvents(world.Snap());
        std::vector<CellOp> cellOps;
        debris.PreTick(t + 1, world, cellOps);
        ++t;
        SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps, false,
                   {8, h / 16, 8}, true, false);
        ctx.WaitIdle();
        ctx.ProcessEvents();
        phys.Step(kTickDt);
        debris.PostStep();
        mobs.PostStep();
      };

      uint64_t id = mobs.Spawn(0, {137, h + 1, 139});
      Vec3 spawnPos = mobs.MobOrigin(id);
      for (int i = 0; i < 120; i++) mobTick({});
      Vec3 walked = mobs.MobOrigin(id);
      float dist = (walked - spawnPos).len();
      bool standing = mobs.IsAlive(id) && mobs.LimbBodyCount() == 6 &&
                      std::abs(walked.y - (float)(h + 1)) < 6.0f;

      // sever arm.L (limb index 2 in the generated sidecar)
      uint32_t debrisBefore = debris.BodyCount();
      mobs.Sever(id, 2);
      bool severed = mobs.LimbBodyCount() == 5 &&
                     debris.BodyCount() == debrisBefore + 1 && mobs.IsAlive(id);
      for (int i = 0; i < 60; i++) mobTick({});

      // vital hit: decapitation kills — remaining 5 limbs ragdoll into debris.
      // settle window covers the blood drying out (its chunks stay dirty
      // while wet, and terrain refreshes wake nearby bodies by design)
      mobs.Sever(id, 1);
      bool died = !mobs.IsAlive(id) || mobs.MobCount() == 0;
      for (int i = 0; i < 500; i++) mobTick({});
      uint32_t awake = debris.ActiveBodyCount();
      bool settled = awake == 0 && mobs.MobCount() == 0;

      mobOk = standing && severed && died && settled;
      std::printf(
          "mob: %s (stood=%d walked %.1f vox, sever=%d, death=%d, %u debris "
          "pieces, %u awake after settle)\n",
          mobOk ? "PASS" : "FAIL", standing ? 1 : 0, dist, severed ? 1 : 0,
          died ? 1 : 0, debris.BodyCount(), awake);
      debris.Reset();
      mobs.Reset();
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
      debris.PreTick(t + 1, world, cellOps);
      ++t;
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cellOps, false,
                 {5, h / 16, 5}, true, false);
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
    phys.RemoveBody(pb);
    pushOk = pushNear > 0.01f && pushFar < 1e-3f;
    std::printf("player body: %s (overlap push %.2f vox, clear push %.3f vox)\n",
                pushOk ? "PASS" : "FAIL", pushNear, pushFar);
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
  bool pass = deterministic && walkOk && sleepOk && debrisOk && prefabOk &&
              mobOk && settleOk && pushOk && saveOk && storeOk && streamOk;
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
  bool lowPowerAdapter = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--selftest") selftest = true;
    // `--adapter low` picks the LowPower adapter (iGPU) so the selftest hash
    // can be compared across GPU vendors (DESIGN.md §14 risk 3).
    if (a == "--adapter" && i + 1 < argc) lowPowerAdapter = std::string(argv[++i]) == "low";
  }

  std::string assetDir = AssetDir();
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

  GLFWwindow* window = nullptr;
  if (!selftest) {
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
  if (!sim.Init(ctx.device, world, mats, reactions, assetDir + "/shaders")) return 1;

  Physics phys;
  if (!phys.Init()) return 1;
  DebrisSystem debris;
  debris.Init(&phys, &world, mats);
  MobSystem mobs;
  mobs.Init(&phys, &world, &debris, mats);
  {
    std::vector<MobDef> mobDefs;
    std::string mlog;
    LoadMobDefs(assetDir + "/mobs", mats, mobDefs, mlog);
    if (!mlog.empty()) std::fprintf(stderr, "%s", mlog.c_str());
    std::printf("loaded %zu mob defs\n", mobDefs.size());
    mobs.SetDefs(std::move(mobDefs));
  }
  Stream stream;
  stream.Init(&ctx, &world, &sim, kDefaultSeed);

  if (selftest)
    return RunSelftest(ctx, world, sim, mats, phys, debris, mobs, stream);

  Overlay overlay;
  if (!overlay.Init(window, ctx.device, ctx.surfaceFormat)) return 1;

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
  for (const Prefab& p : prefabs) ui.prefabNames.push_back(p.name);
  for (const MobDef& d : mobs.Defs()) ui.mobNames.push_back(d.name);
  int spawnH = World::TerrainHeight(140, 140, kDefaultSeed);
  player.pos = Vec3{140, (float)(spawnH + 10), 140};
  // kinematic capsule proxy so debris collides with (and is shoved by) the
  // player; terrain collision stays in the AABB controller
  uint64_t playerBody = phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);

  bool captured = true;
  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  double mx0 = 0, my0 = 0;
  glfwGetCursorPos(window, &mx0, &my0);

  KeyEdge eP, eN, eV, eF1, eF5, eF9, eF10, eR, eEsc, eLBracket, eRBracket, eJump,
      eG, eX, eB, eT, eO, eM, eTab;
  bool prevMouseL = false;
  std::vector<Grenade> grenades;
  // particle-pass gating: tick-deterministic inputs only (see SubmitTick note)
  bool everExploded = false;
  uint32_t lastExplosionTick = 0;
  uint32_t tick = 0;
  uint32_t bodyInstCount = 0;
  double lastTime = NowSec();
  double accumulator = 0;
  float fpsSmooth = 60, frameMsSmooth = 16, tickMsSmooth = 0;

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    double now = NowSec();
    float dt = (float)(now - lastTime);
    lastTime = now;
    frameMsSmooth += (dt * 1000.0f - frameMsSmooth) * 0.05f;
    fpsSmooth += (1.0f / std::max(dt, 1e-4f) - fpsSmooth) * 0.05f;

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
      g.vel = cam.Forward() * (20.0f / kVoxelMeters) + player.vel;
      g.fuse = 2.2f;
      grenades.push_back(g);
    }
    if (captured && eX.Pressed(key(GLFW_KEY_X))) ui.pendingDetonate = true;
    if (captured && eTab.Pressed(key(GLFW_KEY_TAB)))
      ui.tool = (ui.tool + 1) % UIState::kToolCount;
    if (captured && eM.Pressed(key(GLFW_KEY_M))) ui.spawnMob = true;
    if (captured && eB.Pressed(key(GLFW_KEY_B))) ui.placePrefab = true;
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
        sim.UploadTables(ctx.queue, mats, reactions);
        debris.OnMaterialsReloaded(mats);
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
        LoadMobDefs(assetDir + "/mobs", mats, mobDefs, mlog);
        if (!mlog.empty()) std::fprintf(stderr, "%s", mlog.c_str());
        mobs.SetDefs(std::move(mobDefs));
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
      tick = 0;
      grenades.clear();
      everExploded = false;
      debris.Reset();
      mobs.Reset();
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
      }
    }

    // ---- player (per frame, against the latest one-tick-latent mirror) ----
    player.fly = ui.fly;
    auto kindAt = [&](IVec3 c) { return world.KindAt(c, classOf); };
    player.Update(dt, pin, cam.FlatForward(), cam.Right(), cam.Forward(), kindAt);
    ui.fly = player.fly;

    // ---- fixed-tick simulation ----
    accumulator += dt;
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
      stream.Update({ifloor(player.pos.x) >> 4, ifloor(player.pos.y) >> 4,
                     ifloor(player.pos.z) >> 4});

      std::vector<BrushOp> ops;
      brush.radius = ui.brushRadius;
      brush.material = (uint32_t)ui.brushMaterial;

      // laser (PLAN §C1/C2): laser tool + LMB, or hold F from any tool.
      // Bodies are tested first — a mob limb or debris chunk in the beam
      // takes the hit instead of the wall behind it.
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
        const float kLaserRange = 200.0f;
        uint64_t hitBody = phys.CastRayBody(eye, fwd, kLaserRange, frac);
        float bodyDist = frac * kLaserRange;

        if (hitBody != 0 && bodyDist < gridDist) {
          // body cut (PLAN §C2): mob limbs take damage (instant sever when
          // the beam crosses a joint); plain debris splits along a vertical
          // plane containing the beam
          Vec3 hitPos = eye + fwd * bodyDist;
          if (!mobs.Damage(hitBody, 1.5f, hitPos) && tick % 6 == 0) {
            Vec3 right = cam.Right();
            Vec3 n = right - fwd * right.dot(fwd);
            if (n.len() < 0.1f) n = cam.Up();
            debris.SplitBody(hitBody, hitPos, n.normalized());
          }
        } else if (gridDist < 1e8f) {
          const int r = 2;
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

      // mobs: kinematic walk drive, terrain anchors for ManageTerrain,
      // bleeding ops — must run before debris.PreTick consumes the anchors
      mobs.PreTick(tick, world, ops);

      // support-loss flags from the sim (burnt stems, undermined slabs) feed
      // the same island-check pipeline as explosions and brush erases
      debris.QueueSupportEvents(world.Snap());
      // island detection results + terrain collision upkeep (may add cell ops)
      std::vector<CellOp> cellOps;
      debris.PreTick(tick, world, cellOps);
      // prefab stamps drain after island ops (they win same-cell conflicts)
      placer.PreTick(world, cellOps);

      // explosions: X-detonate at the crosshair + grenade fuses
      std::vector<ExplosionOp> exps;
      if (ui.pendingDetonate) {
        ui.pendingDetonate = false;
        const WorldSnapshot& snap = world.Snap();
        if (snap.valid && snap.pick[0] != 0) {
          exps.push_back({(int)snap.pick[2], (int)snap.pick[3], (int)snap.pick[4],
                          12, 340, 0, 0, 0});
        }
      }
      for (size_t i = 0; i < grenades.size();) {
        if (UpdateGrenade(grenades[i], kTickDt, kindAt)) {
          if (exps.size() < kMaxExplosionsPerTick) {
            exps.push_back({ifloor(grenades[i].pos.x), ifloor(grenades[i].pos.y),
                            ifloor(grenades[i].pos.z), 13, 380, 0, 0, 0});
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
          phys.ApplyRadialImpulse(Vec3{(float)e.x, (float)e.y, (float)e.z},
                                  (float)e.radius * 3.0f, (float)e.power * 0.15f);
          stream.MarkModifiedBox({e.x - e.radius, e.y - e.radius, e.z - e.radius},
                                 {e.x + e.radius, e.y + e.radius, e.z + e.radius});
        }
      }
      // CPU-known writes mark chunks modified now — eviction can't wait for
      // the latent dirty-flag snapshot
      for (const BrushOp& b : ops)
        stream.MarkModifiedBox({b.x - b.radius, b.y - b.radius, b.z - b.radius},
                               {b.x + b.radius, b.y + b.radius, b.z + b.radius});
      bool particlesActive =
          everExploded &&
          (tick - lastExplosionTick < 400 || world.Snap().particleCount > 0);

      IVec3 pc{ifloor(player.pos.x) / (int)kChunk, ifloor(player.pos.y) / (int)kChunk,
               ifloor(player.pos.z) / (int)kChunk};
      double t0 = NowSec();
      phys.MovePlayerBody(playerBody, player.pos, kTickDt);
      SubmitTick(ctx, world, sim, tick, kDefaultSeed, ops, exps, cellOps,
                 tick % 15 == 0 /*hash occasionally*/, pc, true, particlesActive);
      phys.Step(kTickDt);   // CPU physics overlaps the GPU tick
      debris.PostStep();
      mobs.PostStep();
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
      Vec3 eye = player.EyePos();
      WriteRenderParams(ctx.queue, world, eye, cam,
                        (float)ctx.width / (float)ctx.height, ui.shadows,
                        (float)now);

      ui.fps = fpsSmooth;
      ui.frameMs = frameMsSmooth;
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
      if (debris.InstancesDirty() || mobs.InstancesDirty()) {
        std::vector<BodyVoxInst> inst;
        debris.BuildInstances(inst);
        mobs.AppendInstances(inst, debris.BodyCount());
        bodyInstCount = (uint32_t)inst.size();
        if (!inst.empty())
          ctx.queue.WriteBuffer(world.bodyInstances, 0, inst.data(),
                                inst.size() * sizeof(BodyVoxInst));
      }
      if (debris.BodyCount() + mobs.LimbBodyCount() > 0) {
        std::vector<BodyXformGpu> xf;
        debris.BuildXforms(xf);
        mobs.AppendXforms(xf);
        ctx.queue.WriteBuffer(world.bodyXforms, 0, xf.data(),
                              xf.size() * sizeof(BodyXformGpu));
      }

      wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      wgpu::RenderPassEncoder rp = sim.BeginRenderPass(enc, target, ctx.surfaceFormat,
                                                       ctx.width, ctx.height);
      sim.DrawWorld(rp);
      sim.DrawParticles(rp);
      sim.DrawBodies(rp, bodyInstCount);
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
  overlay.Shutdown();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
