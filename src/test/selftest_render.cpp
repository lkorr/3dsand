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
#include "sim/faredits.h"
#include "sim/farfield.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- one packed material byte out of farVox -----------------------------
// The far grid's own addressing (kFarN masks, chunk-major), mirroring
// common.wgsl's farVoxByteIndex. SELFTEST ONLY — farVox carries CopySrc for
// exactly this and the frame path stays readback-free (CLAUDE.md rule 3).
// Shared by the three far gates so the index math has one definition; a second
// copy is the "two places that must agree" bug this repo has a checker for.
uint32_t FarVoxByte(GpuContext& ctx, World& world, uint32_t level, IVec3 cc) {
  auto wrapv = [](int v) { return (uint32_t)(v & (int)(kFarN - 1)); };
  const uint32_t x = wrapv(cc.x), y = wrapv(cc.y), z = wrapv(cc.z);
  const uint32_t ch = ((z >> 4) * kFarNChunk + (y >> 4)) * kFarNChunk + (x >> 4);
  const uint32_t lo = ((z & 15) * kChunk + (y & 15)) * kChunk + (x & 15);
  const uint64_t bi =
      (uint64_t)(level - 1) * kFarVox + (uint64_t)ch * kChunkVol + lo;
  rhi::Buffer staging =
      CreateBuffer(ctx.device, 4, rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                   "farVoxRead");
  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  enc.CopyBufferToBuffer(world.farVox, bi & ~3ull, staging, 0, 4);
  ctx.queue.Submit(enc.Finish());
  uint32_t word = 0;
  rhi::ReadBufferBlocking(ctx.device, staging, 0, &word, 4);
  return (word >> ((bi & 3ull) * 8)) & 0xFFu;
}

// Fill every cascade level around `playerChunk` from scratch and wait for it.
// This is the operation far-field edit persistence is about: it is what
// startup, a teleport and LoadWorld all do, and before the patch pass it threw
// away every edit the live downsample had put in the cascades.
void DrainFullRefill(GpuContext& ctx, World& world, Simulation& sim,
                     IVec3 playerChunk) {
  FarField far;
  far.Init(&world);
  far.FullRefill(playerChunk);
  uint32_t n;
  while ((n = far.PrepareTick(ctx.queue)) > 0) {
    TickParams tp{0, kDefaultSeed, 0, 0};
    tp.farCount = n;
    ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeFarFill(enc, n);
    ctx.queue.Submit(enc.Finish());
  }
  ctx.WaitIdle();
}

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
  SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t, kDefaultSeed),
             SelftestExps(t, kDefaultSeed), {}, false, {8, 3, 8}, false,
             SelftestParticlesActive(t));
ctx.WaitIdle();
double t0 = NowSeconds();
for (uint32_t t = 61; t <= 160; t++)
  SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t, kDefaultSeed),
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
  // cold start: level 1 still pending -> only the residency window is trusted.
  // Both bounds come from world.h's cascade helpers rather than restating the
  // box-size relation: this gate previously hardcoded "half-extent = 2^k
  // window edges", which silently became wrong the moment the shift base
  // moved, failing the gate for a change that was correct.
  const float wantCold = kWindowHalfExtentMeters;
  const float wantFull = kFarHalfExtentMeters(kFarLevels);
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
  // resident. The height is TERRAIN-RELATIVE: y200 was open sky when the
  // band was y32..y86 and is underground now, and the gate reported that
  // as "0/27 level-1 cells air before the edit". 190 clears the tallest
  // canopy (TREE_MAX_ABOVE ~175 voxels at metre-true tree scale).
  const IVec3 c{140, FixtureY(140, 140, kDefaultSeed, 190, 64), 140};
  // One level-1 cell spans 2^(1+kFarShiftBase) fine voxels, sampled at its
  // center. The brush radius below must cover the sample points of the full
  // 3x3x3 cell block around the paint: the farthest one sits
  // 1.5 * cellsize - (cellsize/2 - offset) away per axis — radius 12 covers
  // it at the current 4-voxel cells (corner sample distance^2 = 108 < 144).
  const int farShift1 = (int)(1 + kFarShiftBase);
  auto farByte = [&](IVec3 cc) { return FarVoxByte(ctx, world, 1, cc); };
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

// ---- far-persist -------------------------------------------------------
// Far-field EDIT PERSISTENCE (src/sim/faredits.h). `far-downsample` above
// proves an edit reaches the cascades while its chunk is resident and dirty;
// this proves it SURVIVES a cascade refill, which is what actually happens
// when the player walks past a level's box edge and back, teleports, or
// reloads the world.
//
// The gate is a two-armed differential inside one run, so it cannot pass
// vacuously and cannot pass without the patch pass:
//
//   arm A (control): paint, ghost it into the cascades via `fardown`, then
//     FullRefill with the edit index EMPTY. The sieve regenerates pristine
//     procgen — open sky up here — so all 27 cells must read AIR. That arm is
//     literally the bug: it is the behaviour every build had before this
//     change, and it is asserted here so a future change that quietly stops
//     refilling cannot make arm B pass for the wrong reason.
//   arm B (the fix): hand the index the same chunks eviction would have handed
//     it, FullRefill again, and the 27 cells must read the paint back.
Status GateFarPersist(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  // Open air well above the hills and canopy, resident under the {0,0,0}
  // window origin this section runs at, and clear of far-downsample's own
  // paint site so the two gates cannot read each other's cells.
  const IVec3 site{300, FixtureY(300, 300, kDefaultSeed, 190, 64), 300};
  const int shift1 = (int)(1 + kFarShiftBase);
  const IVec3 cell0{site.x >> shift1, site.y >> shift1, site.z >> shift1};
  // Radius 12 covers the sample points of the whole 3x3x3 level-1 cell block
  // at the current 4-fine-voxel cells (corner sample distance^2 = 108 < 144) —
  // the same sizing argument far-downsample makes.
  const int kRadius = 12;
  const IVec3 playerChunk{site.x >> 4, site.y >> 4, site.z >> 4};

  auto scan = [&](uint32_t want) {
    uint32_t n = 0;
    for (int dz = -1; dz <= 1; dz++)
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
          if (FarVoxByte(ctx, world, 1,
                         {cell0.x + dx, cell0.y + dy, cell0.z + dz}) == want)
            n++;
    return n;
  };

  // ---- paint, and let the live downsample carry it into the cascades ----
  for (uint32_t t = 1; t <= 4; t++) {
    std::vector<BrushOp> ops;
    if (t == 1)
      ops.push_back({site.x, site.y, site.z, kRadius, kMatGlass,
                     1u /*overwrite*/, 0, 0});
    SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, {}, false, playerChunk,
               false, false);
  }
  ctx.WaitIdle();
  const uint32_t ghosted = scan(kMatGlass);

  // ---- arm A: refill with an empty index -> the horizon heals itself ----
  FarEdits& edits = c.stream.Edits();
  // The harness's Stream may have evicted chunks in an earlier gate, and a
  // stale index entry for THIS level chunk would patch the control arm. The
  // index is derived and disposable by construction, so dropping it is legal
  // (a real session rebuilds it from the store on load).
  edits.Clear();
  DrainFullRefill(ctx, world, sim, playerChunk);
  const uint32_t healed = scan(kMatAir);

  // ---- arm B: feed the index the way eviction does, then refill ---------
  // Stream::CompleteOldest hands FarEdits the harvested words of every chunk
  // it persists; here the words come straight off the GPU instead, because
  // forcing a real window shift mid-suite would move the origin out from under
  // every gate that follows. The chunks are exactly the ones that own the 27
  // cells' sample voxels — a level-1 cell samples the fine voxel at
  // (cell << shift) + half.
  int lo[3] = {INT32_MAX, INT32_MAX, INT32_MAX};
  int hi[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
  for (int dz = -1; dz <= 1; dz++)
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        const int f[3] = {((cell0.x + dx) << shift1) + (1 << (shift1 - 1)),
                          ((cell0.y + dy) << shift1) + (1 << (shift1 - 1)),
                          ((cell0.z + dz) << shift1) + (1 << (shift1 - 1))};
        for (int a = 0; a < 3; a++) {
          lo[a] = std::min(lo[a], f[a] >> 4);
          hi[a] = std::max(hi[a], f[a] >> 4);
        }
      }
  std::vector<uint32_t> words(kChunkVol);
  uint32_t noted = 0;
  for (int cz = lo[2]; cz <= hi[2]; cz++)
    for (int cy = lo[1]; cy <= hi[1]; cy++)
      for (int cx = lo[0]; cx <= hi[0]; cx++) {
        const IVec3 wc{cx, cy, cz};
        if (!world.ChunkInWindow(wc)) continue;
        ReadVoxelsSync(ctx, world, World::SlotChunkIndex(wc), 1, words.data(),
                       "farPersist");
        edits.NoteChunk(wc, words.data());
        noted++;
      }
  DrainFullRefill(ctx, world, sim, playerChunk);
  const uint32_t kept = scan(kMatGlass);

  const bool ok = ghosted == 27 && healed == 27 && kept == 27;
  std::printf("far persist: %s (%u/27 cells downsampled, %u/27 back to air on a "
              "refill with no index, %u/27 preserved from %u indexed chunks)\n",
              ok ? "PASS" : "FAIL", ghosted, healed, kept, noted);
  detail = Format("%u/27 ghosted, %u/27 healed, %u/27 preserved", ghosted,
                  healed, kept);
  return ok ? Status::Pass : Status::Fail;
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
  // ABOVE THE GROUND, not at an absolute Y: the datum moves with the terrain
  // overhaul and a literal 220 puts this camera underground the moment it does.
  // ~160 voxels clears TREE_MAX_ABOVE's crown reach at this column.
  Vec3 farEye{140, (float)(World::TerrainHeight(140, 140, kDefaultSeed) + 160),
              140};
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
  // ---- the GRAZING arm of the render benchmark ----------------------------
  // The elevated pass above looks DOWN at the forest from 12 m, so nearly every
  // ray terminates within a few metres and the whole 1080p frame is ~5 ms with
  // shadows costing ~0.5 ms of it. That is not the shape of the frame the
  // surface-flight work is about: a near-level eye ray runs tens of metres
  // through meadow and canopy, and its shadow ray then runs back through the
  // same canopy toward the sun. It is the case chunk-skipping is worst at, so
  // it is the case any empty-space-skipping change has to be judged on.
  //
  // Deliberately NOT folded into bestFrameMs: that is a MIN across passes and
  // feeds the `perf` gate's < 16 ms assertion, so adding a slower arm to it
  // would change nothing except to make the assertion read as if it covered
  // this view. It does not; this arm is reported and not asserted.
  for (int pass = 0; pass < 2; pass++) {
    bool gShadows = pass == 0;
    ctx.WaitIdle();
    double g0 = NowSeconds();
    for (int i = 0; i < 60; i++) {
      WriteRenderParams(ctx.queue, world, gEye, gCam, (float)W / H, gShadows, 0);
      rhi::CommandEncoder genc = ctx.device.CreateCommandEncoder();
      rhi::RenderPass grp =
          sim.BeginRenderPass(genc, view, rhi::TextureFormat::RGBA8Unorm, W, H);
      sim.DrawWorld(grp);
      grp.End();
      ctx.queue.Submit(genc.Finish());
    }
    ctx.WaitIdle();
    double gms = (NowSeconds() - g0) * 1000.0 / 60.0;
    std::printf("render 1080p ground %s: %.2f ms/frame (%.0f fps)\n",
                gShadows ? "shadows on " : "shadows off", gms, 1000.0 / gms);
  }
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
      {"far-persist", "render", {}, false, GateFarPersist},
      // The only gate in this file that actually DRAWS: far-fog and
      // far-downsample exercise the far-field cascades through compute and a
      // one-word readback, and never touch the offscreen target.
      {"screenshots", "render", {}, false, GateScreenshots, /*needsRender=*/true},
  };
  return g;
}

}  // namespace selftest
