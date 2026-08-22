// selftest_render.cpp — render selftest gates.
//
// Bodies moved verbatim out of the old monolithic RunSelftest; see
// scripts/split_selftest.py for the exact source ranges. Each gate returns a
// Status and fills `detail` with the parenthetical the old printf carried, so
// the console output is unchanged and --json can carry the same numbers.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game/brush.h"
#include "game/camera.h"
#include "gpu/resources.h"
#include "sim/farfield.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- far-fog -----------------------------------------------------------
Status GateFarFog(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
// sim perf: worst-case-ish activity (brushes + explosions + particles),
// synchronous timing
SubmitWorldgen(ctx, world, sim, kDefaultSeed);
ctx.WaitIdle();
for (uint32_t t = 1; t <= 60; t++)  // warm up with heavy activity
  SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t),
             SelftestExps(t, kDefaultSeed), {}, false, {8, 3, 8}, false,
             SelftestParticlesActive(t));
ctx.WaitIdle();
double t0 = NowSeconds();
for (uint32_t t = 61; t <= 160; t++)
  SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t),
             SelftestExps(t, kDefaultSeed), {}, false, {8, 3, 8}, false,
             SelftestParticlesActive(t));
ctx.WaitIdle();
c.simMs = (NowSeconds() - t0) * 1000.0 / 100.0;
double simMs = c.simMs;
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
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeFarFill(enc, n);
    ctx.queue.Submit(enc.Finish());
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

  // Verdict: the flag the moved body already computed.
  return fogOk ? Status::Pass : Status::Fail;
}

// ---- far-downsample ----------------------------------------------------
Status GateFarDownsample(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
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
    rhi::Buffer staging = CreateBuffer(
        ctx.device, 4, rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
        "farVoxRead");
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    enc.CopyBufferToBuffer(world.farVox, bi & ~3ull, staging, 0, 4);
    ctx.queue.Submit(enc.Finish());
    uint32_t word = 0;
    rhi::ReadBufferBlocking(ctx.device, staging, 0, &word, (size_t)(4));
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

  // Verdict: the flag the moved body already computed.
  return farDownOk ? Status::Pass : Status::Fail;
}

// ---- screenshots -------------------------------------------------------
Status GateScreenshots(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const uint32_t W = c.width;
  const uint32_t H = c.height;
  rhi::Texture& offscreen = c.offscreen;
  rhi::TextureView& view = c.view;
// render perf: offscreen 1080p. The target itself is created once by the
// harness and shared with every other gate that draws (Ctx::offscreen).

Camera cam;
cam.yaw = 0.785f;   // look out over the forest from above the canopy
cam.pitch = -0.35f;
// Anchored to the local ground, not a fixed y: terrain now reaches y140 and
// a hardcoded eye height buried the camera inside a hillside (the render
// benchmark then timed a screenful of dirt, and the screenshot showed one).
// 20 m up clears the canopy on any ridge.
Vec3 eye{108, (float)(World::TerrainHeight(108, 108, kDefaultSeed) + 120), 108};

double& bestFrameMs = c.bestFrameMs;
for (int pass = 0; pass < 2; pass++) {
  bool shadows = pass == 0;
  ctx.WaitIdle();
  double r0 = NowSeconds();
  for (int i = 0; i < 60; i++) {
    WriteRenderParams(ctx.queue, world, eye, cam, (float)W / H, shadows, 0);
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    rhi::RenderPass rp =
        sim.BeginRenderPass(enc, view, rhi::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    sim.DrawParticles(rp);
    rp.End();
    ctx.queue.Submit(enc.Finish());
  }
  ctx.WaitIdle();
  double ms = (NowSeconds() - r0) * 1000.0 / 60.0;
  std::printf("render 1080p %s: %.2f ms/frame (%.0f fps)\n",
              shadows ? "shadows on " : "shadows off", ms, 1000.0 / ms);
  bestFrameMs = std::min(bestFrameMs, ms);
}

// Read the offscreen target back and write it out. Shared by the standard
// screenshot and the far-field view below.
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
  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  rhi::RenderPass rp =
      sim.BeginRenderPass(enc, view, rhi::TextureFormat::RGBA8Unorm, W, H);
  sim.DrawWorld(rp);
  rp.End();
  ctx.queue.Submit(enc.Finish());
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
  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  rhi::RenderPass rp =
      sim.BeginRenderPass(enc, view, rhi::TextureFormat::RGBA8Unorm, W, H);
  sim.DrawWorld(rp);
  rp.End();
  ctx.queue.Submit(enc.Finish());
  ctx.WaitIdle();
  grab("screenshot_ground.bmp");
}

  return Status::Pass;
}

}  // namespace

const std::vector<Gate>& RenderGates() {
  static const std::vector<Gate> g = {
      {"far-fog", "render", {}, false, GateFarFog},
      {"far-downsample", "render", {}, false, GateFarDownsample},
      // The only gate in this file that actually DRAWS: far-fog and
      // far-downsample exercise the far-field cascades through compute and a
      // one-word readback, and never touch the offscreen target.
      {"screenshots", "render", {}, false, GateScreenshots, /*needsRender=*/true},
  };
  return g;
}

}  // namespace selftest
