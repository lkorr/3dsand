// selftest_render.cpp — render selftest gates.
//
// Bodies moved verbatim out of the old monolithic RunSelftest; see
// scripts/split_selftest.py for the exact source ranges. Each gate returns a
// Status and fills `detail` with the parenthetical the old printf carried, so
// the console output is unchanged and --json can carry the same numbers.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "game/brush.h"
#include "game/camera.h"
#include "gpu/resources.h"
#include "sim/celestial.h"
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

// ---- the WATER arm of the render benchmark (PLAN_water_master.md M4) -------
// A 1080p frame that is MOSTLY WATER, which neither arm above is: the elevated
// pass looks down at forest and the grazing pass puts the lake a few dozen
// pixels wide on the horizon. Component 9's whole cost model is "O(water
// pixels), not O(volume)", and component 8 adds a per-water-pixel field
// evaluation plus four more for the foam convergence — so a frame with almost
// no water in it cannot judge either.
//
// It is REPORTED, not asserted, exactly like the grazing arm and for the same
// reason: bestFrameMs is a MIN feeding the perf gate's < 16 ms assertion, and
// folding a deliberately expensive view into it would make that assertion read
// as if it covered this one.
//
// The camera is derived from worldgen rather than written down. The two --shot
// water cameras carried literal y values from before the terrain overhaul moved
// spawnPlainY to 200 and had been rendering from inside solid rock ever since;
// a benchmark that silently starts measuring the inside of a rock reports a
// wonderful number.
{
  const World::Column lakeCol = World::TerrainColumn(420, 420, kDefaultSeed);
  const float surf =
      (float)(lakeCol.water != INT32_MIN ? lakeCol.water : lakeCol.h);
  Camera wCam;
  wCam.yaw = 0.785f;
  wCam.pitch = -0.04f;
  Vec3 wEye{386, surf + 2.5f, 386};
  for (int pass = 0; pass < 2; pass++) {
    bool wShadows = pass == 0;
    ctx.WaitIdle();
    double w0 = NowSeconds();
    for (int i = 0; i < 60; i++) {
      WriteRenderParams(ctx.queue, world, wEye, wCam, (float)W / H, wShadows, 0);
      rhi::CommandEncoder wenc = ctx.device.CreateCommandEncoder();
      rhi::RenderPass wrp =
          sim.BeginRenderPass(wenc, view, rhi::TextureFormat::RGBA8Unorm, W, H);
      sim.DrawWorld(wrp);
      wrp.End();
      ctx.queue.Submit(wenc.Finish());
    }
    ctx.WaitIdle();
    double wms = (NowSeconds() - w0) * 1000.0 / 60.0;
    std::printf("render 1080p water %s: %.2f ms/frame (%.0f fps)\n",
                wShadows ? "shadows on " : "shadows off", wms, 1000.0 / wms);
  }
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

// ---- fire-depth: does a flame cover what is behind it? ------------------
//
// THE BUG. The raymarcher draws the world and then the raster passes draw
// rigidbodies on top, sharing one reversed-Z depth buffer. Gas is not a
// surface — trace() accumulates it and keeps marching — so the depth the
// raymarcher wrote for a pixel full of fire was the depth of the SOLID BEHIND
// the fire, or the far plane for a ray that crossed the plume and reached sky.
// Every mob, every debris chunk and every dropped item therefore passed the
// depth test against a flame it was genuinely behind and drew straight over
// it. Fire never composited in front of a rigidbody at any distance, at any
// density: a burning creature had its own flames painted behind it.
//
// THE FIX is a depth for the volume — the point where the gas becomes half
// opaque (Hit.gasHalfT). BOTH HALVES ARE THE CLAIM, which is why this gate
// renders two arms rather than one: putting depth at the plume's FIRST cell
// would also pass "the fire hides what is behind it" while wrecking the case
// that already worked, so an arm that only checks the fix is not a test.
//
// Deliberately no Jolt and no DebrisSystem: the four body buffers are just
// buffers, so the fixture writes one slot's worth of cubes directly. The
// question is entirely about depth ordering between two draws, and a physics
// body that settles, sleeps or falls out of frame is a second thing that can
// break the picture.
Status GateFireDepth(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const uint32_t W = c.width, H = c.height;

  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mFire = matId("fire"), mStone = matId("stone");
  if (!mFire || !mStone) {
    detail = "fire or stone missing from materials.json";
    return Status::Fail;
  }

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // Open air well above the canopy, so the background is SKY. Against terrain
  // both arms would also be measuring the terrain shading behind the slab.
  const int gx = 300, gz = 300;
  const int y0 = World::TerrainHeight(gx, gz, kDefaultSeed) + 60;
  const IVec3 pchunk{gx / 16, y0 / 16, gz / 16};

  // A SLAB, not a puff: the ray has to cross enough fire to reach half
  // opacity (~1.8 cells at fire's authored opacity 150/255), and one sim tick
  // of a gas rearranges it. Six deep leaves margin for both.
  std::vector<CellOp> fireOps;
  for (int dx = 0; dx < 6; dx++)
    for (int dy = -6; dy <= 6; dy++)
      for (int dz = -6; dz <= 6; dz++) {
        const IVec3 cc{gx + dx, y0 + dy, gz + dz};
        if (!world.CellInWindow(cc)) continue;
        fireOps.push_back({World::SlotCellIndex(cc), PackVoxNew(mFire, 7u)});
      }
  uint32_t t = 40000;
  SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, fireOps, false, pchunk,
             false, false);
  ctx.WaitIdle();

  // Camera on the -X side looking along +X straight through the slab.
  Camera cam;
  cam.yaw = 0.0f;    // atan2(look.z, look.x) == 0 is +X
  cam.pitch = 0.0f;
  const Vec3 eye{(float)gx - 34.0f, (float)y0 + 0.5f, (float)gz + 0.5f};

  // One slot of cube instances: a 7^3 block of stone at a given min corner.
  auto renderWithBlock = [&](const Vec3* corner, std::vector<uint8_t>& out) {
    std::vector<BodyVoxInst> inst;
    std::vector<BodyXformGpu> xf;
    if (corner) {
      // Packed by hand rather than through rigrender: that header uses math3d
      // names unqualified at namespace scope, so it only compiles after a
      // `using namespace sandvox`, and one include in the wrong order here is
      // a wall of errors inside a file this gate has no business touching.
      // Slot 0, no art colour — see BodyVoxInst in phys/debris.h.
      for (int x = 0; x < 7; x++)
        for (int y = 0; y < 7; y++)
          for (int z = 0; z < 7; z++)
            inst.push_back({(float)x, (float)y, (float)z, mStone});
      BodyXformGpu m{};
      m.pos[0] = corner->x; m.pos[1] = corner->y; m.pos[2] = corner->z;
      m.quat[3] = 1.0f;
      xf.push_back(m);
      ctx.queue.WriteBuffer(world.bodyInstances, 0, inst.data(),
                            inst.size() * sizeof(BodyVoxInst));
      ctx.queue.WriteBuffer(world.bodyXforms, 0, xf.data(),
                            xf.size() * sizeof(BodyXformGpu));
    }
    WriteRenderParams(ctx.queue, world, eye, cam, (float)W / H, true, 0.0f,
                      kFarFogDensity, 1080.0f, t);
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    rhi::RenderPass rp = sim.BeginRenderPass(
        enc, c.view, rhi::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    sim.DrawBodies(rp, (uint32_t)inst.size());
    rp.End();
    rhi::Buffer shot =
        CreateBuffer(ctx.device, (uint64_t)W * H * 4,
                     rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                     "fireDepthShot");
    rhi::TexelCopyTexture srcT{};
    srcT.texture = c.offscreen;
    rhi::TexelCopyBuffer dstB{};
    dstB.buffer = shot;
    dstB.bytesPerRow = W * 4;
    dstB.rowsPerImage = H;
    rhi::Extent3D ext{W, H, 1};
    enc.CopyTextureToBuffer(srcT, dstB, ext);
    ctx.queue.Submit(enc.Finish());
    out.assign((size_t)W * H * 4, 0);
    return rhi::ReadBufferBlocking(ctx.device, shot, 0, out.data(), out.size());
  };

  std::vector<uint8_t> flameOnly, behindPx, frontPx;
  const Vec3 behind{(float)gx + 14.0f, (float)y0 - 3.0f, (float)gz - 3.0f};
  const Vec3 front{(float)gx - 16.0f, (float)y0 - 3.0f, (float)gz - 3.0f};
  const bool got = renderWithBlock(nullptr, flameOnly) &&
                   renderWithBlock(&behind, behindPx) &&
                   renderWithBlock(&front, frontPx);
  if (!got) {
    detail = "readback failed";
    return Status::Fail;
  }

  auto changed = [&](const std::vector<uint8_t>& a) {
    uint32_t n = 0;
    for (size_t i = 0; i + 3 < a.size(); i += 4) {
      const int d = std::max({std::abs((int)a[i] - (int)flameOnly[i]),
                              std::abs((int)a[i + 1] - (int)flameOnly[i + 1]),
                              std::abs((int)a[i + 2] - (int)flameOnly[i + 2])});
      if (d > 8) n++;
    }
    return n;
  };
  const uint32_t behindShown = changed(behindPx);
  const uint32_t frontShown = changed(frontPx);

  // The block subtends the same solid angle in both arms (same size, mirrored
  // offset), so the two counts are directly comparable and no pixel budget has
  // to be hardcoded. In FRONT it must be plainly visible; BEHIND, the flame
  // must swallow nearly all of it. A tenth is loose on purpose: fire is a
  // volume with soft edges and its silhouette is not the block's.
  const bool frontOk = frontShown > 2000;
  const bool behindOk = behindShown * 10 < frontShown;
  const bool ok = frontOk && behindOk;
  std::printf("fire depth: %s (block behind the flame paints %u px, same block "
              "in front paints %u px; behind must be under a tenth of front)\n",
              ok ? "PASS" : "FAIL", behindShown, frontShown);
  detail = Format("behind %u px, front %u px", behindShown, frontShown);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// shadow-cache: the voxel-keyed shadow cache agrees with the ray it replaced.
//
// WHY THIS GATE EXISTS. The cache does not reuse trace(). It cannot: trace()
// lives in raymarch.wgsl and the resolve pass is a compute shader, so
// shadow_resolve.wgsl carries its own media-blind DDA (shadowMarch). That is
// two implementations of one question, which is the shape this repo has a
// checker for everywhere else and had none for here. A comment claiming they
// agree is worth nothing; this runs both.
//
// THREE ARMS, NOT TWO, and the third is the point. Comparing "cache on" with
// "cache off" and finding them equal proves nothing on its own — a scene with
// no shadows in it at all would pass that test perfectly, and so would a cache
// that returns 1.0 for everything IF the reference had also stopped casting.
// The `noshadow` arm establishes that this frame HAS shadows worth agreeing
// about, so the small A-B difference is a real agreement rather than two blank
// pages matching. (See the "a hash-identity test needs three arms" note.)
// The request list's header after a frame has drawn and before the next
// prepare() has moved it: [0] is the raw count of patches the fragment shader
// asked for in the frame just rendered. Read here so the gate names the number
// itself — a flicker with no count attached invites an elimination run per
// hypothesis (CLAUDE.md rule 6), and the cap is the first one.
bool ReadShadowReqHeader(GpuContext& ctx, World& world, uint32_t out[4]) {
  rhi::Buffer stage =
      CreateBuffer(ctx.device, 16,
                   rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                   "shadowReqHeaderRead");
  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  enc.CopyBufferToBuffer(world.shadowReq, 0, stage, 0, 16);
  ctx.queue.Submit(enc.Finish());
  return rhi::ReadBufferBlocking(ctx.device, stage, 0, out, 16);
}

Status GateShadowCache(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const uint32_t W = c.width, H = c.height;

  // SELF-SUFFICIENT ON PURPOSE, and this is the half that matters most.
  // Gates share one World and most of them inherit terrain from whatever ran
  // before (CLAUDE.md rule 7), but a gate that only works inside the full suite
  // cannot be iterated on with `--gate shadow-cache` — which is exactly how
  // this one will be used. So it generates its own world and paints its own
  // blocker. Measured: without the SubmitWorldgen below, all three arms
  // rendered empty sky and every difference was 0.00.
  const uint32_t mStone = [&]() -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == "stone") return (uint32_t)i;
    return 0;
  }();
  if (!mStone) {
    detail = "stone missing from materials.json";
    return Status::Fail;
  }
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // A slab floating over open ground. Terrain alone would probably cast usable
  // shadows, but "probably" is how a gate becomes a coin flip on the next
  // worldgen tweak: an authored blocker at a known height guarantees the
  // reference frame has a large, unambiguous shadow in it, which is what the
  // third arm below has to be able to see.
  const int gx = 300, gz = 300;
  const int ground = World::TerrainHeight(gx, gz, kDefaultSeed);
  const int slabY = ground + 10;
  const IVec3 pchunk{gx / 16, slabY / 16, gz / 16};
  std::vector<CellOp> slab;
  for (int dx = -18; dx <= 18; dx++)
    for (int dy = 0; dy < 2; dy++)
      for (int dz = -18; dz <= 18; dz++) {
        const IVec3 cc{gx + dx, slabY + dy, gz + dz};
        if (!world.CellInWindow(cc)) continue;
        slab.push_back({World::SlotCellIndex(cc), PackVoxNew(mStone, 0u)});
      }
  uint32_t tick = 40000;
  SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, slab, false, pchunk,
             false, false);
  ctx.WaitIdle();

  // Looking down at the ground the slab shades, from off to one side so the
  // slab does not fill the frame.
  // Low and close, pitched well down: the quantity under test is the SHADOWED
  // GROUND, so it has to fill the frame. A high wide shot is mostly sky and
  // distant terrain, and the shadow gets averaged into irrelevance — measured,
  // the first framing here put the shadows-on/off difference at 1.57 against an
  // agreement of 0.33, a margin too thin to call either way.
  const Vec3 eye{(float)gx - 22.0f, (float)ground + 7.0f, (float)gz - 22.0f};
  Camera cam;
  cam.yaw = 0.785f;    // toward +X+Z, i.e. at the slab
  cam.pitch = -0.30f;

  const Tuning base = CurrentTuning();

  // BUDGET UNDER THE LIGHTING THE GAME IS PLAYED IN. WriteRenderParams derives
  // the sun from the celestial cycle, not from its `time` argument, and at an
  // arbitrary tick the sun can be below the horizon — where sunShadowAt is
  // never reached at all because `lambert > 0.0` fails. Measured: at tick 0
  // this gate saw a shadows-on/shadows-off difference of 0.41, i.e. a frame
  // with essentially no sunlight to block. Scanned rather than hardcoded for
  // the same reason --render-budget scans: cycleMinutes is a tuning value, so
  // any constant here becomes the wrong time of day the first time it moves.
  uint32_t noonTick = 0;
  {
    float bestUp = -2.0f;
    for (uint32_t t = 0; t < 200000u; t += 64u) {
      const float up = ComputeSky(base, (double)t).sunDir[1];
      if (up > bestUp) { bestUp = up; noonTick = t; }
    }
  }

  // Render `frames` frames and return the last one. The count matters for the
  // cache arm and only for it: the cache is empty on the first frame, every
  // patch misses and shades unshadowed, and it takes ONE further frame for the
  // resolve pass to fill in what that frame registered. Anything less than 2 is
  // measuring the cold-start miss, not the cache.
  //
  // `prev`, when given, receives the frame BEFORE the last one. Two consecutive
  // warmed frames of a static camera must be the same picture: the cache's
  // first version passed the single-frame agreement below while ~28k patches
  // per frame alternated lit/shadowed through bucket contention, because a
  // flicker averaged over one frame is just a little disagreement. Comparing
  // frame N-1 against N is what sees it.
  auto render = [&](bool shadows, uint32_t frames,
                    std::vector<uint8_t>& out,
                    std::vector<uint8_t>* prev) -> bool {
    rhi::Buffer shot =
        CreateBuffer(ctx.device, (uint64_t)W * H * 4,
                     rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                     "shadowCacheShot");
    rhi::Buffer shotPrev =
        CreateBuffer(ctx.device, (uint64_t)W * H * 4,
                     rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                     "shadowCacheShotPrev");
    for (uint32_t f = 0; f < frames; f++) {
      // Fresh render params per frame: WriteRenderParams is what advances the
      // cache's frame counter, so reusing one upload would leave every entry
      // looking stale and every lookup missing.
      WriteRenderParams(ctx.queue, world, eye, cam, (float)W / H, shadows, 0.0f,
                        kFarFogDensity, (float)H, noonTick);
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      sim.EncodeShadowResolve(enc);
      rhi::RenderPass rp = sim.BeginRenderPass(
          enc, c.view, rhi::TextureFormat::RGBA8Unorm, W, H);
      sim.DrawWorld(rp);
      rp.End();
      const bool last = (f + 1 == frames);
      const bool beforeLast = prev && frames >= 2 && (f + 2 == frames);
      if (last || beforeLast) {
        rhi::TexelCopyTexture srcT{};
        srcT.texture = c.offscreen;
        rhi::TexelCopyBuffer dstB{};
        dstB.buffer = last ? shot : shotPrev;
        dstB.bytesPerRow = W * 4;
        dstB.rowsPerImage = H;
        rhi::Extent3D ext{W, H, 1};
        enc.CopyTextureToBuffer(srcT, dstB, ext);
      }
      ctx.queue.Submit(enc.Finish());
    }
    out.assign((size_t)W * H * 4, 0);
    bool ok = rhi::ReadBufferBlocking(ctx.device, shot, 0, out.data(), out.size());
    if (ok && prev) {
      prev->assign((size_t)W * H * 4, 0);
      ok = rhi::ReadBufferBlocking(ctx.device, shotPrev, 0, prev->data(),
                                   prev->size());
    }
    return ok;
  };

  // Mean absolute luminance difference over the frame, in 0..255 units.
  auto meanDiff = [&](const std::vector<uint8_t>& a,
                      const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 1e9;
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = 0; i + 3 < a.size(); i += 4) {
      const double la = 0.299 * a[i] + 0.587 * a[i + 1] + 0.114 * a[i + 2];
      const double lb = 0.299 * b[i] + 0.587 * b[i + 1] + 0.114 * b[i + 2];
      acc += std::fabs(la - lb);
      n++;
    }
    return n ? acc / (double)n : 1e9;
  };

  auto arm = [&](int cacheOn, bool shadows, uint32_t frames,
                 std::vector<uint8_t>& out,
                 std::vector<uint8_t>* prev = nullptr) -> bool {
    Tuning t = base;
    t.render.shadowCache = cacheOn;
    SetCurrentTuning(t);
    // The F5 path: SHADOW_CACHE is const-folded, so the arm does not exist
    // until the shader is recompiled with it.
    if (!sim.ReloadShaders(ctx.device)) return false;
    return render(shadows, frames, out, prev);
  };

  // The reference arm renders TWO frames so its own frame-to-frame difference
  // is on the record: anything that legitimately changes between frames of a
  // pinned scene (a far cascade still converging, say) shows up there first,
  // and the cache's flicker is only meaningful against that floor.
  std::vector<uint8_t> refPx, refPrevPx, cachePx, cachePrevPx, noShadowPx;
  uint32_t reqStats[4] = {0, 0, 0, 0};
  // SANDVOX_SHADOW_GATE_FRAMES overrides the cache arm's warm-up length (4):
  // a flicker that vanishes at 12 frames was convergence, one that persists is
  // steady-state contention, and that distinction is one run, not a debate.
  uint32_t cacheFrames = 4;
  if (const char* e = std::getenv("SANDVOX_SHADOW_GATE_FRAMES")) {
    const int v = std::atoi(e);
    if (v >= 2) cacheFrames = (uint32_t)v;
  }
  bool got = arm(0, true, 2, refPx, &refPrevPx) &&  // per-pixel rays (reference)
             arm(1, true, cacheFrames, cachePx, &cachePrevPx) &&  // the cache, warmed
             ReadShadowReqHeader(ctx, world, reqStats) &&
             arm(0, false, 1, noShadowPx);    // no shadows at all

  // ---- THE WALK: the same comparison under camera MOTION -------------------
  // Everything the arms above can see is one picture: a warmed cache behind a
  // pinned camera. The defects that motivated this arm — shadows that pulse
  // while walking, that pop into existence a frame after their ground scrolls
  // into view, and stray dark squares on open ground — are MOTION defects,
  // and a pinned camera cannot register any of them: a patch that is on
  // screen every frame is registered every frame, so its one-frame resolve
  // latency, a window shift that renames every key, and a stale slot's
  // leftover value all hide behind the frame before. So: kWalk poses along a
  // walk-and-turn (0.6 voxel and ~1.4 deg of yaw per frame, a brisk walk
  // turning at ~85 deg/s at 60 fps), each rendered by the per-pixel reference
  // and by the cache running CONTINUOUSLY through them, with the residency
  // window shifted one chunk halfway along — which is what walking 1.6 m does
  // in play. Per frame: the agreement, and separately the pixels the cache
  // shades LIT where the reference has shadow (a HOLE: "shadows pop in") and
  // the pixels it shades DARK where the reference is lit (a PHANTOM: "tiny
  // unconnected shadows"). A mean hides a one-frame event, so the worst frame
  // and the shift frame are reported on their own, with images.
  const uint32_t kWalk = 24;
  const uint32_t kShiftAt = 12;
  const double kEdgeL = 12.0;   // luminance step that counts as a hole/phantom
  auto walkPose = [&](uint32_t f, Vec3& e, Camera& cm) {
    cm = cam;
    cm.yaw = cam.yaw - 0.30f + 0.025f * (float)f;
    Vec3 fwd = cam.Forward();
    fwd.y = 0.0f;
    fwd = fwd.normalized();
    e = eye + fwd * (0.6f * (float)f);
  };
  // One chunk of +x shift evicts the plane [ox*16, ox*16+16): only do it when
  // that plane is nowhere near the slab, or the fixture walks off with it.
  const IVec3 origin0 = world.WindowOrigin();
  const int halfC = (int)kNChunk / 2;
  const bool canShift =
      origin0.x * (int)kChunk + (int)kChunk < gx - 20 &&
      origin0.x * (int)kChunk + (int)kWorldN > gx + 20;
  bool shifted = false;
  auto shiftX = [&](int dir) -> bool {
    const IVec3 o = world.WindowOrigin();
    // kHysteresis is 2 chunks: a player 2 chunks off centre moves the window.
    c.stream.Update({o.x + halfC + 2 * dir, o.y + halfC, o.z + halfC}, tick);
    ctx.WaitIdle();
    const bool moved = world.WindowOrigin().x == o.x + dir;
    if (moved) shifted = !shifted;
    return moved;
  };
  auto renderOne = [&](bool shadows, const Vec3& e, const Camera& cm,
                       std::vector<uint8_t>* out) -> bool {
    WriteRenderParams(ctx.queue, world, e, cm, (float)W / H, shadows, 0.0f,
                      kFarFogDensity, (float)H, noonTick);
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeShadowResolve(enc);
    rhi::RenderPass rp = sim.BeginRenderPass(
        enc, c.view, rhi::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    rp.End();
    rhi::Buffer shot;
    if (out) {
      shot = CreateBuffer(ctx.device, (uint64_t)W * H * 4,
                          rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                          "shadowWalkShot");
      rhi::TexelCopyTexture srcT{};
      srcT.texture = c.offscreen;
      rhi::TexelCopyBuffer dstB{};
      dstB.buffer = shot;
      dstB.bytesPerRow = W * 4;
      dstB.rowsPerImage = H;
      rhi::Extent3D ext{W, H, 1};
      enc.CopyTextureToBuffer(srcT, dstB, ext);
    }
    ctx.queue.Submit(enc.Finish());
    if (!out) return true;
    out->assign((size_t)W * H * 4, 0);
    return rhi::ReadBufferBlocking(ctx.device, shot, 0, out->data(), out->size());
  };
  auto setCache = [&](int cacheOn) -> bool {
    Tuning t = base;
    t.render.shadowCache = cacheOn;
    SetCurrentTuning(t);
    return sim.ReloadShaders(ctx.device);
  };
  auto lumAt = [](const std::vector<uint8_t>& px, size_t i) {
    return 0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2];
  };

  // The reference walk, kept as one luminance byte per pixel per frame.
  std::vector<std::vector<uint8_t>> refLum(kWalk);
  std::vector<uint8_t> tmpPx;
  bool walkOk = got && setCache(0);
  for (uint32_t f = 0; walkOk && f < kWalk; f++) {
    Vec3 e; Camera cm;
    walkPose(f, e, cm);
    if (f == kShiftAt && canShift && !shiftX(+1)) walkOk = false;
    if (!renderOne(true, e, cm, &tmpPx)) { walkOk = false; break; }
    refLum[f].resize((size_t)W * H);
    for (size_t i = 0, j = 0; i + 3 < tmpPx.size(); i += 4, j++)
      refLum[f][j] = (uint8_t)std::min(255.0, lumAt(tmpPx, i) + 0.5);
  }
  if (shifted) shiftX(-1);

  // The cache walk: warmed at pose 0, then one frame per pose, continuously.
  // Each pose is rendered TWICE by the cache: once arriving from the pose
  // before (moving), then again standing still (settled). The settled frame
  // is the cache's error at that pose with no motion in it — the patch
  // quantisation the static arm already measures — so moving minus settled
  // is the MOTION error on its own, which is the number the one-frame resolve
  // latency shows up in and the number a pre-registration pass has to move.
  struct WalkFrame {
    double agree, agreeSettled;
    size_t holes, phantoms, holesSettled, phantomsSettled;
    uint32_t req, res;
  };
  std::vector<WalkFrame> walk(kWalk, {0.0, 0.0, 0, 0, 0, 0, 0, 0});
  uint32_t worstF = 0;
  std::vector<uint8_t> worstCachePx, worstRefLum;
  walkOk = walkOk && setCache(1);
  if (walkOk) {
    Vec3 e; Camera cm;
    walkPose(0, e, cm);
    for (uint32_t w = 0; w < 3; w++) renderOne(true, e, cm, nullptr);
  }
  for (uint32_t f = 0; walkOk && f < kWalk; f++) {
    Vec3 e; Camera cm;
    walkPose(f, e, cm);
    if (f == kShiftAt && canShift && !shiftX(+1)) walkOk = false;
    if (!renderOne(true, e, cm, &tmpPx)) { walkOk = false; break; }
    uint32_t hdr[4] = {0, 0, 0, 0};
    ReadShadowReqHeader(ctx, world, hdr);
    WalkFrame& wf = walk[f];
    wf.req = hdr[0];
    wf.res = hdr[1];
    // Record at the point of failure (CLAUDE.md rule 6): around the shift
    // frame, the cache's own state — how many slots hold a key, how many of
    // those were resolved, how many are dark, and the histogram of their
    // `requested` stamps — so a bad frame says WHICH half failed: the slots
    // were not found (stamps stop at the frame before) or were found and
    // held the wrong answer (stamps current, values lit).
    if (canShift && f + 1 >= kShiftAt && f <= kShiftAt + 1) {
      rhi::Buffer stage = CreateBuffer(
          ctx.device, kShadowCacheBytes,
          rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "shadowCacheRead");
      rhi::CommandEncoder cenc = ctx.device.CreateCommandEncoder();
      cenc.CopyBufferToBuffer(world.shadowCache, 0, stage, 0, kShadowCacheBytes);
      ctx.queue.Submit(cenc.Finish());
      std::vector<uint32_t> cw(kShadowCacheBuckets * kShadowCacheWords);
      if (rhi::ReadBufferBlocking(ctx.device, stage, 0, cw.data(),
                                  cw.size() * 4)) {
        size_t keyed = 0, valid = 0, dark = 0, stamps[16] = {};
        for (uint32_t b = 0; b < kShadowCacheBuckets; b++) {
          const uint32_t k = cw[b * 2], st = cw[b * 2 + 1];
          if (!k) continue;
          keyed++;
          if (st & 0x10000u) { valid++; if ((st & 0xFFu) < 128u) dark++; }
          stamps[(st >> 12) & 15u]++;
        }
        std::printf("              shift probe frame %u: %zu keyed slots, %zu "
                    "valid, %zu dark; requested-stamp histogram:", f, keyed,
                    valid, dark);
        for (int i = 0; i < 16; i++) std::printf(" %zu", stamps[i]);
        std::printf("\n");
      }
    }
    double acc = 0.0;
    for (size_t i = 0, j = 0; i + 3 < tmpPx.size(); i += 4, j++) {
      const double d = lumAt(tmpPx, i) - (double)refLum[f][j];
      acc += std::fabs(d);
      if (d > kEdgeL) wf.holes++;
      else if (d < -kEdgeL) wf.phantoms++;
    }
    wf.agree = acc / (double)((size_t)W * H);
    if (f == 0 || wf.agree > walk[worstF].agree) {
      worstF = f;
      worstCachePx = tmpPx;
      worstRefLum = refLum[f];
    }
    // The settled frame: same pose, one frame later.
    if (!renderOne(true, e, cm, &tmpPx)) { walkOk = false; break; }
    acc = 0.0;
    for (size_t i = 0, j = 0; i + 3 < tmpPx.size(); i += 4, j++) {
      const double d = lumAt(tmpPx, i) - (double)refLum[f][j];
      acc += std::fabs(d);
      if (d > kEdgeL) wf.holesSettled++;
      else if (d < -kEdgeL) wf.phantomsSettled++;
    }
    wf.agreeSettled = acc / (double)((size_t)W * H);
  }
  if (shifted) shiftX(-1);
  double walkMean = 0.0, settledMean = 0.0;
  size_t holesMax = 0, phantomsMax = 0;
  uint32_t holesF = 0, phantomsF = 0, reqMin = 0xFFFFFFFFu, reqMax = 0;
  for (uint32_t f = 0; f < kWalk; f++) {
    walkMean += walk[f].agree / (double)kWalk;
    settledMean += walk[f].agreeSettled / (double)kWalk;
    if (walk[f].holes > holesMax) { holesMax = walk[f].holes; holesF = f; }
    if (walk[f].phantoms > phantomsMax) { phantomsMax = walk[f].phantoms; phantomsF = f; }
    reqMin = std::min(reqMin, walk[f].req);
    reqMax = std::max(reqMax, walk[f].req);
  }
  if (walkOk && !worstCachePx.empty()) {
    std::vector<uint8_t> refImg((size_t)W * H * 4, 255), diff((size_t)W * H * 4, 255);
    for (size_t i = 0, j = 0; i + 3 < refImg.size(); i += 4, j++) {
      refImg[i] = refImg[i + 1] = refImg[i + 2] = worstRefLum[j];
      const double d = lumAt(worstCachePx, i) - (double)worstRefLum[j];
      // red = hole (cache lit, reference shadowed); blue = phantom (the reverse)
      diff[i] = d > kEdgeL ? 255 : 0;
      diff[i + 1] = 0;
      diff[i + 2] = d < -kEdgeL ? 255 : 0;
    }
    WriteBmpFile("build/shadow_walk_ref.bmp", refImg, W, H);
    WriteBmpFile("build/shadow_walk_cache.bmp", worstCachePx, W, H);
    WriteBmpFile("build/shadow_walk_diff.bmp", diff, W, H);
  }
  got = got && walkOk;
  SetCurrentTuning(base);
  const bool restored = sim.ReloadShaders(ctx.device);
  if (!got || !restored) {
    detail = got ? "shader restore failed" : "render/readback failed";
    return Status::Fail;
  }

  // LEAVE THE WORLD AS THIS GATE FOUND IT. Gates share one World and 42 of
  // them run after this one (selftest.cpp kOrder), so the 37x2x37 stone slab
  // painted above is not this gate's private fixture — it is a permanent edit
  // every later gate would inherit, in a suite whose whole ordering discipline
  // is about not doing that. Regenerating costs a second and removes the entire
  // class of question.
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  const double agree = meanDiff(refPx, cachePx);
  const double signal = meanDiff(refPx, noShadowPx);

  // AGREEMENT IS NOT EQUALITY, and must not be asserted as such: the cache
  // answers per PATCH, so a shadow edge lands on a patch boundary rather than a
  // pixel boundary and the two frames legitimately differ along every edge.
  //
  // THE TEST IS A RATIO, NOT AN ABSOLUTE, and that is deliberate. An absolute
  // millilumen threshold silently encodes this camera, this slab and this
  // worldgen; move any of them and it becomes either unfailable or a coin flip.
  // The claim worth asserting is scale-free — the cache is MUCH closer to the
  // per-pixel reference than dropping shadows entirely is — so that is what is
  // written down. kSignalMin keeps the ratio honest by refusing a frame with no
  // shadows in it, where both differences are ~0 and the ratio is meaningless.
  //
  // FLICKER IS THE THIRD CLAIM and the one the first version of this gate could
  // not make: a static camera's warmed frames 3 and 4 must be the same picture.
  // Nothing in the scene moves (time is pinned, the sun is pinned), so any
  // difference is the cache changing its mind — contention, a steal, a miss
  // default painted over a value. Held to 2% of the shadow signal rather than
  // zero so a handful of genuinely racing duplicate claims cannot fail it, but
  // the number expected with the set-associative cache is 0.00.
  const double flicker = meanDiff(cachePrevPx, cachePx);
  const double refFlicker = meanDiff(refPrevPx, refPx);
  // Record at the point of failure: the two warmed frames and a diff image
  // (white where luminance moved by more than 2/255), plus a count of moved
  // pixels, so a flicker number comes with WHERE and HOW MANY attached.
  size_t movedPx = 0;
  {
    std::vector<uint8_t> diff((size_t)W * H * 4, 255);
    for (size_t i = 0; i + 3 < cachePx.size() && i + 3 < cachePrevPx.size(); i += 4) {
      const double la = 0.299 * cachePx[i] + 0.587 * cachePx[i + 1] + 0.114 * cachePx[i + 2];
      const double lb = 0.299 * cachePrevPx[i] + 0.587 * cachePrevPx[i + 1] + 0.114 * cachePrevPx[i + 2];
      const bool moved = std::fabs(la - lb) > 2.0;
      movedPx += moved ? 1 : 0;
      const uint8_t v = moved ? 255 : 0;
      diff[i] = v; diff[i + 1] = v; diff[i + 2] = v;
    }
    WriteBmpFile("build/shadow_cache_prev.bmp", cachePrevPx, W, H);
    WriteBmpFile("build/shadow_cache_last.bmp", cachePx, W, H);
    WriteBmpFile("build/shadow_cache_diff.bmp", diff, W, H);
  }
  const double kSignalMin = 1.0;
  const double kAgreeFrac = 0.25;
  const double kFlickerFrac = 0.02;
  // THE WALK IS HELD TO THE SAME AGREEMENT AS THE STANDING FRAME, on its mean
  // and on its worst frame. The mean is the "shadows pulse while walking"
  // claim; the worst frame is the "they pop in" one, and it is the one a mean
  // over 24 frames would forgive. Both are ratios of the shadow signal for
  // the reason the static agreement is.
  const double kWalkWorstFrac = 0.40;
  const bool walkPass = walkMean < signal * kAgreeFrac &&
                        walk[worstF].agree < signal * kWalkWorstFrac;
  const bool ok = signal > kSignalMin && agree < signal * kAgreeFrac &&
                  flicker < signal * kFlickerFrac && walkPass;
  std::printf("shadow cache: %s (cache vs per-pixel rays: mean |dL| %.2f; "
              "per-pixel rays vs no shadows: %.2f, must exceed %.1f; agreement "
              "must be under %.0f%% of that, i.e. %.2f; frame-to-frame flicker "
              "%.3f, must be under %.0f%% of the signal, i.e. %.2f; reference "
              "arm's own frame-to-frame difference %.3f)\n"
              "              last cache frame (%u warmed): %u patches requested, "
              "cap %u, %u refused; %zu px moved > 2/255 between the last two "
              "frames (build/shadow_cache_{prev,last,diff}.bmp)\n",
              ok ? "PASS" : "FAIL", agree, signal, kSignalMin,
              kAgreeFrac * 100.0, signal * kAgreeFrac, flicker,
              kFlickerFrac * 100.0, signal * kFlickerFrac, refFlicker,
              cacheFrames, reqStats[0], kShadowReqCap,
              reqStats[0] > kShadowReqCap ? reqStats[0] - kShadowReqCap : 0u,
              movedPx);
  std::printf("              walk (%u frames, 0.6 vox + 1.4 deg each, window "
              "shift at frame %u%s): mean |dL| %.2f moving, %.2f settled at "
              "the same poses (must be under %.2f), worst moving frame %u at "
              "%.2f (must be under %.2f); holes (cache lit, "
              "reference shadowed, > %.0f/255) peak %zu px at frame %u; "
              "phantoms (the reverse) peak %zu px at frame %u; shift frame "
              "|dL| %.2f, %zu holes, %zu phantoms; requests %u..%u per frame "
              "(cap %u) (build/shadow_walk_{ref,cache,diff}.bmp = frame %u)\n",
              kWalk, kShiftAt, canShift ? "" : " SKIPPED (slab in the plane)",
              walkMean, settledMean, signal * kAgreeFrac, worstF,
              walk[worstF].agree, signal * kWalkWorstFrac, kEdgeL, holesMax,
              holesF, phantomsMax,
              phantomsF, walk[kShiftAt].agree, walk[kShiftAt].holes,
              walk[kShiftAt].phantoms, reqMin, reqMax, kShadowReqCap, worstF);
  std::printf("              per frame, moving|settled: |dL| / holes / "
              "phantoms (and rays resolved):");
  for (uint32_t f = 0; f < kWalk; f++)
    std::printf("%s %u:%.2f|%.2f/%zu|%zu/%zu|%zu(%u)",
                f % 4 == 0 ? "\n               " : "", f, walk[f].agree,
                walk[f].agreeSettled, walk[f].holes, walk[f].holesSettled,
                walk[f].phantoms, walk[f].phantomsSettled, walk[f].res);
  std::printf("\n");
  detail = Format("agree %.2f, signal %.2f, budget %.2f, flicker %.3f (ref "
                  "%.3f), %u requested; walk mean %.2f (settled %.2f) worst "
                  "%.2f@%u",
                  agree, signal, signal * kAgreeFrac, flicker, refFlicker,
                  reqStats[0], walkMean, settledMean, walk[worstF].agree,
                  worstF);
  return ok ? Status::Pass : Status::Fail;
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
      {"fire-depth", "render", {}, false, GateFireDepth, /*needsRender=*/true},
      {"shadow-cache", "render", {}, false, GateShadowCache, /*needsRender=*/true},
  };
  return g;
}

}  // namespace selftest
