// sandvox — 3D falling-sand voxel engine (v0). See DESIGN.md.
// Fixed 30 Hz GPU simulation, uncapped raymarched rendering, walkable player,
// JSON materials, deterministic kernels with per-tick world hash.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "game/brush.h"
#include "game/camera.h"
#include "game/player.h"
#include "gpu/context.h"
#include "gpu/resources.h"
#include "math3d.h"
#include "sim/materials.h"
#include "sim/simulation.h"
#include "sim/world.h"
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
  queue.WriteBuffer(world.renderUBO, 0, &rp, sizeof(rp));
}

// Encode + submit one sim tick (uniform writes must precede the submit and
// happen once per tick, hence submit-per-tick).
void SubmitTick(GpuContext& ctx, World& world, Simulation& sim, uint32_t tick,
                uint32_t seed, const std::vector<BrushOp>& ops, bool hashEnable,
                IVec3 playerChunk, bool wantReadback) {
  TickParams tp{tick, seed, (uint32_t)ops.size(), hashEnable ? 1u : 0u};
  ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
  if (!ops.empty())
    ctx.queue.WriteBuffer(world.opsBuf, 0, ops.data(), ops.size() * sizeof(BrushOp));

  wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  sim.EncodeTick(enc, (uint32_t)ops.size(), hashEnable);
  bool doCopy = false;
  if (wantReadback) {
    doCopy = world.EncodeReadbacks(ctx.device, enc,
                                   {playerChunk.x - 1, playerChunk.y - 1, playerChunk.z - 1});
    if (doCopy) world.EncodeDirtyCopy(enc, sim.DirtyNext());
  }
  wgpu::CommandBuffer cmd = enc.Finish();
  ctx.queue.Submit(1, &cmd);
  sim.FlipPage();
  if (doCopy) world.KickReadback();
}

void SubmitWorldgen(GpuContext& ctx, World& world, Simulation& sim, uint32_t seed) {
  TickParams tp{0, seed, 0, 0};
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

std::vector<BrushOp> SelftestOps(uint32_t tick) {
  std::vector<BrushOp> ops;
  if (tick >= 5 && tick < 150) {
    ops.push_back({100, 170, 100, 6, kMatSand, 0, 0, 0});
    ops.push_back({176, 150, 176, 5, kMatWater, 0, 0, 0});
  }
  if (tick >= 30 && tick < 90) {
    ops.push_back({64, 60, 72, 4, kMatSmoke, 0, 0, 0});
  }
  return ops;
}

int RunSelftest(GpuContext& ctx, World& world, Simulation& sim,
                const std::vector<MaterialDef>& mats) {
  std::printf("=== selftest ===\n");
  constexpr int kTicks = 200;

  // determinism: two identical runs must produce identical hash sequences
  std::vector<uint32_t> hashes[2];
  for (int run = 0; run < 2; run++) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    for (uint32_t t = 1; t <= kTicks; t++) {
      SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t), true,
                 {8, 3, 8}, false);
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

  // sim perf: worst-case-ish activity, synchronous timing
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  for (uint32_t t = 1; t <= 60; t++)  // warm up with heavy activity
    SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t), false, {8, 3, 8}, false);
  ctx.WaitIdle();
  double t0 = NowSec();
  for (uint32_t t = 61; t <= 160; t++)
    SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t), false, {8, 3, 8}, false);
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
          sim.BeginRenderPass(enc, view, wgpu::TextureFormat::RGBA8Unorm);
      sim.DrawWorld(rp);
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
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, false, pc, true);
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

  bool perfOk = simMs < 8.0 && bestFrameMs < 16.0;
  std::printf("perf: %s\n", perfOk ? "PASS" : "MARGINAL (see numbers above)");
  bool pass = deterministic && walkOk;
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

}  // namespace

int main(int argc, char** argv) {
  bool selftest = false;
  for (int i = 1; i < argc; i++)
    if (std::string(argv[i]) == "--selftest") selftest = true;

  std::string assetDir = AssetDir();
  std::vector<MaterialDef> mats;
  std::string errors;
  if (!LoadMaterials(assetDir + "/materials/materials.json", mats, errors)) {
    std::fprintf(stderr, "material load failed:\n%s\n", errors.c_str());
    return 1;
  }
  std::printf("loaded %zu materials\n", mats.size());

  GLFWwindow* window = nullptr;
  if (!selftest) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window = glfwCreateWindow(1600, 900, "sandvox", nullptr, nullptr);
    if (!window) return 1;
  }

  GpuContext ctx;
  if (!ctx.Init(window, 1600, 900)) return 1;

  World world;
  world.Init(ctx.device);
  Simulation sim;
  if (!sim.Init(ctx.device, world, mats, assetDir + "/shaders")) return 1;

  if (selftest) return RunSelftest(ctx, world, sim, mats);

  Overlay overlay;
  if (!overlay.Init(window, ctx.device, ctx.surfaceFormat)) return 1;

  UIState ui;
  for (auto& m : mats) ui.materialNames.push_back(m.name);

  // material class LUT for the player's mirror queries
  std::vector<uint32_t> classOf;
  for (auto& m : mats) classOf.push_back(m.gpu.klass);

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);

  Camera cam;
  Player player;
  Brush brush;
  int spawnH = World::TerrainHeight(140, 140, kDefaultSeed);
  player.pos = Vec3{140, (float)(spawnH + 10), 140};

  bool captured = true;
  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  double mx0 = 0, my0 = 0;
  glfwGetCursorPos(window, &mx0, &my0);

  KeyEdge eP, eN, eV, eF1, eF5, eR, eEsc, eLBracket, eRBracket, eJump;
  uint32_t tick = 0;
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
    if (eR.Pressed(key(GLFW_KEY_R))) ui.reloadMaterials = true;
    if (eLBracket.Pressed(key(GLFW_KEY_LEFT_BRACKET)))
      ui.brushRadius = std::max(1, ui.brushRadius - 1);
    if (eRBracket.Pressed(key(GLFW_KEY_RIGHT_BRACKET)))
      ui.brushRadius = std::min(7, ui.brushRadius + 1);
    for (int i = 0; i < 8; i++)
      if (key(GLFW_KEY_1 + i) && i + 1 < (int)mats.size()) ui.brushMaterial = i + 1;

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
      if (LoadMaterials(assetDir + "/materials/materials.json", newMats, errors)) {
        mats = std::move(newMats);
        sim.UploadMaterials(ctx.queue, mats);
        classOf.clear();
        for (auto& m : mats) classOf.push_back(m.gpu.klass);
        ui.materialNames.clear();
        for (auto& m : mats) ui.materialNames.push_back(m.name);
        std::printf("materials reloaded (%zu)\n", mats.size());
      } else {
        std::fprintf(stderr, "material reload failed:\n%s\n", errors.c_str());
      }
    }
    if (ui.regenWorld) {
      ui.regenWorld = false;
      SubmitWorldgen(ctx, world, sim, kDefaultSeed);
      player.pos = Vec3{140, (float)(spawnH + 10), 140};
      tick = 0;
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
    while (accumulator >= kTickDt && ticksThisFrame < 4) {
      accumulator -= kTickDt;
      if (ui.paused && !ui.stepOnce) break;
      ui.stepOnce = false;
      tick++;
      ticksThisFrame++;

      std::vector<BrushOp> ops;
      brush.radius = ui.brushRadius;
      brush.material = (uint32_t)ui.brushMaterial;
      BrushOp op;
      if (mouseL && brush.BuildOp(world.Snap(), player.EyePos(), cam.Forward(), false, op))
        ops.push_back(op);
      if (mouseR && brush.BuildOp(world.Snap(), player.EyePos(), cam.Forward(), true, op))
        ops.push_back(op);

      IVec3 pc{ifloor(player.pos.x) / (int)kChunk, ifloor(player.pos.y) / (int)kChunk,
               ifloor(player.pos.z) / (int)kChunk};
      double t0 = NowSec();
      SubmitTick(ctx, world, sim, tick, kDefaultSeed, ops,
                 tick % 15 == 0 /*hash occasionally*/, pc, true);
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
      ui.playerPos[0] = player.pos.x;
      ui.playerPos[1] = player.pos.y;
      ui.playerPos[2] = player.pos.z;

      overlay.BeginFrame();
      overlay.Draw(ui);

      wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      wgpu::RenderPassEncoder rp = sim.BeginRenderPass(enc, target, ctx.surfaceFormat);
      sim.DrawWorld(rp);
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
