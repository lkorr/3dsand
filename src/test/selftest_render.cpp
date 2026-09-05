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
#include "sim/microvox.h"
#include "sim/plants.h"
#include "sim/trample.h"
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

// ---- the conservative blocker flag, bit 7 of the far cell byte -----------
// (13.2.2 / W2-D; common.wgsl FAR_BLOCKER_BIT)
//
// This rides on `far-fog` rather than on `far-downsample` for one reason: this
// gate does its own `FullRefill` and drains it, so it is the only far gate
// whose cascade data is real when run as `--gate far-fog` alone. `farVox` is
// zero-initialised, so a flag check against an UNFILLED cascade reads "no flag
// anywhere" and passes for the wrong reason forever.
//
// Two claims, and neither is the trivial one:
//
//   ORDER. In every column, the highest cell carrying the flag is at or above
//   the highest cell carrying MATERIAL. The flag is "the surface reaches into
//   this cell" and the material byte is "the cell's centre sample was solid",
//   and the centre is half a cell above the floor — so the flag can only ever
//   extend the column upward. A flag BELOW the material top would mean the two
//   writers disagree about where the ground is.
//
//   RECOVERY. At least one column has a cell with the flag and NO material.
//   That cell is the entire point of the feature: the half of every surface
//   cell whose ground landed in its lower half, which the centre sample calls
//   air. Without this line the flag could be a synonym for `mat != 0` and
//   nothing would say so.
//
// The knob (`render.farBlockerHitLevel`) ships at 0, so NOTHING ELSE in the
// suite or in any screenshot would notice if the writers stopped emitting the
// flag: the shadow half is a shading difference no assertion covers.
static bool CheckFarBlockerFlag(GpuContext& ctx, World& world) {
  constexpr uint32_t kBlockerBit = 0x80u;
  constexpr uint32_t kMatMask = 0x7Fu;
  const int shift1 = (int)(1 + kFarShiftBase);
  int columns = 0, recovered = 0, disordered = 0;
  // Eight columns spread across the level-1 box, well inside it (its
  // half-extent is kFarN/2 cells) and away from the two paint sites the other
  // far gates use.
  for (int i = 0; i < 8; i++) {
    const int wx = 200 + i * 24, wz = 200 + i * 17;
    const int h = World::TerrainHeight(wx, wz, kDefaultSeed);
    const int cx = wx >> shift1, cz = wz >> shift1;
    // twelve cells straddling the surface: eight below it, four above
    const int cy0 = ((h - 8 * (1 << shift1)) >> shift1);
    int topMat = INT32_MIN, topFlag = INT32_MIN;
    bool flagOnly = false;
    for (int k = 0; k < 12; k++) {
      const uint32_t b = FarVoxByte(ctx, world, 1, {cx, cy0 + k, cz});
      if ((b & kMatMask) != 0) topMat = cy0 + k;
      if ((b & kBlockerBit) != 0) {
        topFlag = cy0 + k;
        if ((b & kMatMask) == 0) flagOnly = true;
      }
    }
    if (topMat == INT32_MIN && topFlag == INT32_MIN) continue;  // all sky
    columns++;
    if (topFlag < topMat) disordered++;
    if (flagOnly) recovered++;
  }
  const bool ok = columns >= 4 && disordered == 0 && recovered >= 1;
  std::printf("far blocker flag: %s (%d columns sampled, %d with a flagged cell "
              "the centre sample called air, %d where the flag sits below the "
              "material top)\n",
              ok ? "PASS" : "FAIL", columns, recovered, disordered);
  return ok;
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

  // Verdict: the flag the moved body already computed, plus the blocker bit.
  return (fogOk && CheckFarBlockerFlag(ctx, world)) ? Status::Pass : Status::Fail;
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
// openness: the sky-visibility grid knows a roof from an open field.
//
// WHAT IS UNDER TEST. sim_openness.wgsl writes one byte per (chunk slot, 4^3
// block, face) — the unblocked fraction of that face's hemisphere within
// render.opennessReach — and `ambientAt` multiplies the hemisphere ambient by
// it. The claim the whole phase rests on is that the number MEANS something:
// a face under a roof reads low, a face under open sky reads high. This gate
// reads the buffer back and asserts exactly that, in ranges, because the refresh
// order is not deterministic across GPUs and does not need to be.
//
// TWO FACES OF ONE FIXTURE, not two places in the world. `a` is the +Y face of
// the ground block under the centre of an authored slab; `b` is the +Y face of
// the slab's own top block. Same stone, same tick, 1.4 m apart, and the ONLY
// difference between them is that one has a roof over it. A second site
// somewhere "open" in the terrain would have been a worse control: rolling
// ground puts a hillside inside 12 m of most surfaces, and the gate would then
// be measuring worldgen relief rather than the grid.
//
// WHY THE SLAB IS WIDE AND LOW. The five rays are the normal plus four at 45
// degrees, so a roof only reads as cover if its HALF-EXTENT exceeds its HEIGHT
// above the receiver — otherwise the four diagonals escape past its edge and
// the face reads 4/6 = 0.67, which is "mostly open" and correct. A 12x12 slab
// 3 m up (the first sketch of this fixture) measures the slab's edge, not its
// cover. 4.4 m of half-extent at 1.4 m of height clears it with room.
//
// SELF-SUFFICIENT: it generates its own world and stamps its own blocker, so
// `--gate openness` alone is a valid run (CLAUDE.md's "a new gate must be
// verifiable with --gate <name> alone").
//
// THE SECOND TICK IS LOAD-BEARING AND IT DOCUMENTS A REAL LIMIT. An edit
// dirties the chunks it WRITES, and the openness dirty walk follows that list.
// The slab is 14 voxels above the ground, which is a different chunk, so
// stamping the slab does NOT re-walk the floor it now shades — the floor's
// openness only changes when the rolling refresh reaches it, up to
// kNumChunks / render.opennessChunksPerFrame ticks later. Tick B writes one
// voxel into the floor's own chunk to put it on the dirty list. If that
// latency is ever a visible problem the fix is a bigger refresh budget, not a
// dilated dirty list: a 12 m reach dilates to a 15^3 chunk neighbourhood.
uint32_t OpennessByteAt(GpuContext& ctx, World& world, IVec3 c, uint32_t face,
                        bool* stampOk) {
  const IVec3 wc{c.x >> 4, c.y >> 4, c.z >> 4};
  const uint32_t slot = World::SlotChunkIndex(wc);
  const uint32_t lx = (uint32_t)(c.x & 15), ly = (uint32_t)(c.y & 15),
                 lz = (uint32_t)(c.z & 15);
  // subOccBitLocal, mirrored: (bz * DIM + by) * DIM + bx.
  const uint32_t bx = lx >> kSubOccShift, by = ly >> kSubOccShift,
                 bz = lz >> kSubOccShift;
  const uint32_t block = (bz * kSubOccDim + by) * kSubOccDim + bx;
  const uint64_t bi =
      ((uint64_t)slot * kOpenBlocksPerChunk + block) * kOpenFaces + face;

  rhi::Buffer stage =
      CreateBuffer(ctx.device, 8,
                   rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                   "opennessRead");
  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  enc.CopyBufferToBuffer(world.openness, (bi & ~3ull), stage, 0, 4);
  enc.CopyBufferToBuffer(world.opennessGen, (uint64_t)slot * 4, stage, 4, 4);
  ctx.queue.Submit(enc.Finish());
  uint32_t two[2] = {0, 0};
  if (!rhi::ReadBufferBlocking(ctx.device, stage, 0, two, 8)) {
    if (stampOk) *stampOk = false;
    return 0;
  }
  // THE STAMP IS COMPARED AGAINST THE C++ MIRROR, which is the whole reason
  // sandvox::OpennessStamp exists as a function rather than as a comment: it and
  // common.wgsl's opennessStamp are two places that must agree, and this is the
  // check. A mismatch here means either the slot was never walked or the two
  // hashes have drifted, and both are failures of this gate.
  if (stampOk) *stampOk = (two[1] == OpennessStamp(wc.x, wc.y, wc.z));
  return (two[0] >> ((bi & 3ull) * 8)) & 0xFFu;
}

Status GateOpenness(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  const uint32_t mStone = [&]() -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == "stone") return (uint32_t)i;
    return 0;
  }();
  if (!mStone) {
    detail = "stone missing from materials.json";
    return Status::Fail;
  }
  if (CurrentTuning().render.opennessStrength <= 0.0f) {
    detail = "render.opennessStrength is 0 — the pass is not recorded";
    return Status::Fail;
  }

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  const int gx = 300, gz = 300;
  const int ground = World::TerrainHeight(gx, gz, kDefaultSeed);
  const int kHalf = 22;            // slab half-extent, voxels (2.2 m)
  const int kLift = 14;            // slab underside above ground, voxels (1.4 m)
  const int slabY = ground + kLift;
  const IVec3 pchunk{gx / 16, slabY / 16, gz / 16};

  std::vector<CellOp> slab;
  for (int dx = -kHalf; dx <= kHalf; dx++)
    for (int dy = 0; dy < 2; dy++)
      for (int dz = -kHalf; dz <= kHalf; dz++) {
        const IVec3 cc{gx + dx, slabY + dy, gz + dz};
        if (!world.CellInWindow(cc)) continue;
        slab.push_back({World::SlotCellIndex(cc), PackVoxNew(mStone, 0u)});
      }
  if (slab.empty()) {
    detail = "slab site is outside the residency window";
    return Status::Fail;
  }

  uint32_t tick = 60000;
  SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, slab, false, pchunk,
             false, false);
  ctx.WaitIdle();

  // Tick B: one voxel in the FLOOR's chunk, so the floor is re-walked now that
  // the roof exists. Written as stone into a cell that is already solid ground,
  // so the geometry the measurement depends on does not move.
  const IVec3 floorCell{gx, ground, gz};
  if (!world.CellInWindow(floorCell)) {
    detail = "floor site is outside the residency window";
    return Status::Fail;
  }
  std::vector<CellOp> poke{
      {World::SlotCellIndex(floorCell), PackVoxNew(mStone, 0u)}};
  SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, poke, false, pchunk,
             false, false);
  ctx.WaitIdle();

  bool okA = false, okB = false;
  const uint32_t roofByte = OpennessByteAt(ctx, world, floorCell, 3u, &okA);
  const IVec3 topCell{gx, slabY + 1, gz};
  const uint32_t openByte = OpennessByteAt(ctx, world, topCell, 3u, &okB);

  const double a = roofByte / 255.0;
  const double b = openByte / 255.0;
  const double aMax = BaselineNumber("openness.roofedMax", 0.30);
  const double bMin = BaselineNumber("openness.openMin", 0.75);
  const bool ok = okA && okB && a < aMax && b > bMin;

  std::printf(
      "openness: %s (ground under a %dx%d slab %.1f m up: %.2f, must be < %.2f;"
      " the slab's own top face: %.2f, must be > %.2f; stamps %s/%s, %zu slab "
      "cells at (%d,%d) ground y=%d)\n",
      ok ? "PASS" : "FAIL", kHalf * 2 + 1, kHalf * 2 + 1,
      kLift * (double)kVoxelMeters, a, aMax, b, bMin, okA ? "ok" : "STALE",
      okB ? "ok" : "STALE", slab.size(), gx, gz, ground);
  detail = Format("roofed %.2f (< %.2f), open %.2f (> %.2f), stamps %s/%s", a,
                  aMax, b, bMin, okA ? "ok" : "stale", okB ? "ok" : "stale");
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// gi-bounce: sunlit green ground tints the white wall standing beside it.
//
// WHAT IS UNDER TEST (docs/PLAN_gi.md §3, W3 P1). Two halves and both are
// asserted: INJECTION — the shadow resolve pass deposits each lit patch's
// albedo × sun into its block-face's irradiance word, so the green floor's +Y
// face must read GREEN (G > R in the word itself); and the GATHER — the
// raymarch reads the faces around a hit and adds the receiver's bounce, so a
// white wall facing that floor must come out greener with giStrength = 1 than
// with 0, on the same frame, from the same camera. The second claim is the one
// the phase is judged by ("a window lights up the room it looks into"), and it
// is measured as a DELTA between the two arms so the wall's own direct light
// and ambient — which are identical in both — cancel exactly.
//
// THE FIXTURE floats: a 41x41 slab of `leaves` (plain solid, green, not micro
// — a micro material is not a ray blocker and has no block-face entry) with
// its top 0.9 m over the terrain at (300,300), and a `bone` wall (white) at
// its -X edge whose +X face looks across the whole slab. Floating, because
// rolling procgen ground under a wall would put its own light in the frame
// and the assertion would then depend on what worldgen happens to grow there.
//
// WHY SIX FRAMES. Frame 1 registers the patches; the resolve pass deposits at
// frame 2 (256 deposits per block-face at 1/16 each converge within it); the
// gather reads the result from frame 3. Six leaves room for the far cascade
// to settle so the two arms' skies are the same.
//
// THE REGION is the middle of the frame — the wall fills it at this distance
// and the slab and sky only enter at the edges — so the assertion is about
// the wall and not about the floor's own (smaller, whiter) bounce off it.
uint32_t IrradianceWordAt(GpuContext& ctx, World& world, IVec3 c, uint32_t face) {
  const IVec3 wc{c.x >> 4, c.y >> 4, c.z >> 4};
  const uint32_t slot = World::SlotChunkIndex(wc);
  const uint32_t lx = (uint32_t)(c.x & 15), ly = (uint32_t)(c.y & 15),
                 lz = (uint32_t)(c.z & 15);
  const uint32_t bx = lx >> kSubOccShift, by = ly >> kSubOccShift,
                 bz = lz >> kSubOccShift;
  const uint32_t block = (bz * kSubOccDim + by) * kSubOccDim + bx;
  const uint64_t idx =
      ((uint64_t)slot * kOpenBlocksPerChunk + block) * kOpenFaces + face;
  rhi::Buffer stage =
      CreateBuffer(ctx.device, 4,
                   rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                   "irradianceRead");
  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  enc.CopyBufferToBuffer(world.irradiance, idx * 4, stage, 0, 4);
  ctx.queue.Submit(enc.Finish());
  uint32_t w = 0;
  if (!rhi::ReadBufferBlocking(ctx.device, stage, 0, &w, 4)) return 0;
  return w;
}

// RGB9E5 decode, mirroring common.wgsl unpackRgb9e5 (bias 15, 9-bit mantissa).
static void UnpackRgb9e5(uint32_t w, double out[3]) {
  const int e = (int)(w >> 27);
  const double scale = std::ldexp(1.0, e - 15 - 9);
  out[0] = (double)(w & 511u) * scale;
  out[1] = (double)((w >> 9) & 511u) * scale;
  out[2] = (double)((w >> 18) & 511u) * scale;
}

Status GateGiBounce(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const uint32_t W = c.width, H = c.height;

  auto matNamed = [&](const char* name) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == name) return (uint32_t)i;
    return 0;
  };
  const uint32_t mGreen = matNamed("leaves");
  const uint32_t mWhite = matNamed("bone");
  if (!mGreen || !mWhite) {
    detail = "leaves/bone missing from materials.json";
    return Status::Fail;
  }
  const Tuning base = CurrentTuning();
  if (base.render.giStrength <= 0.0f) {
    detail = "render.giStrength is 0 — the gather is compiled out";
    return Status::Fail;
  }

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  const int gx = 300, gz = 300;
  const int ground = World::TerrainHeight(gx, gz, kDefaultSeed);
  const int kHalf = 20;              // slab half-extent (2.0 m)
  const int slabY = ground + 8;      // slab bottom; its top is slabY + 1
  const int wallX = gx - kHalf;      // the wall stands on the slab's -X edge
  const int kWallH = 14;             // wall height above the slab top
  const IVec3 pchunk{gx / 16, slabY / 16, gz / 16};
  std::vector<CellOp> fixture;
  auto put = [&](int x, int y, int z, uint32_t m) {
    const IVec3 cc{x, y, z};
    if (!world.CellInWindow(cc)) return;
    fixture.push_back({World::SlotCellIndex(cc), PackVoxNew(m, 0u)});
  };
  for (int dx = -kHalf; dx <= kHalf; dx++)
    for (int dz = -kHalf; dz <= kHalf; dz++) {
      put(gx + dx, slabY, gz + dz, mGreen);
      put(gx + dx, slabY + 1, gz + dz, mGreen);
    }
  for (int dz = -kHalf; dz <= kHalf; dz++)
    for (int y = 2; y < 2 + kWallH; y++) {
      put(wallX, slabY + y, gz + dz, mWhite);
      put(wallX - 1, slabY + y, gz + dz, mWhite);
    }
  if (fixture.empty()) {
    detail = "fixture site is outside the residency window";
    return Status::Fail;
  }
  uint32_t tick = 70000;
  SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, fixture, false,
             pchunk, false, false);
  ctx.WaitIdle();

  // Noon, scanned rather than hardcoded (the shadow-cache gate says why).
  uint32_t noonTick = 0;
  {
    float bestUp = -2.0f;
    for (uint32_t t = 0; t < 200000u; t += 64u) {
      const float up = ComputeSky(base, (double)t).sunDir[1];
      if (up > bestUp) { bestUp = up; noonTick = t; }
    }
  }

  // Facing the wall's +X face from over the middle of the slab, pitched down
  // so the slab is IN THE FRAME: the resolve pass deposits only for patches
  // somebody requested, and a floor nobody can see lights nothing on screen.
  const Vec3 eye{(float)wallX + 14.0f, (float)(slabY + 2 + kWallH / 2),
                 (float)gz};
  Camera cam;
  cam.yaw = 3.14159265f;   // -X
  cam.pitch = -0.25f;

  // The off-screen injection path, exercised on purpose: with the sun written
  // (WriteRenderParams below is what the walk reads it from), two ticks that
  // poke one slab cell put the slab's chunk on the dirty list, and the
  // openness walk deposits its coarse sun sample into every face it marches
  // there — the same faces the wall's rays will land on. Without this the
  // gate would only ever measure what the camera can see, which is the
  // narrower of the two halves.
  WriteRenderParams(ctx.queue, world, eye, cam, (float)W / H, true, 0.0f,
                    kFarFogDensity, (float)H, noonTick);
  for (int i = 0; i < 2; i++) {
    std::vector<CellOp> poke{
        {World::SlotCellIndex(IVec3{gx, slabY, gz}), PackVoxNew(mGreen, 0u)}};
    SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, poke, false,
               pchunk, false, false);
  }
  ctx.WaitIdle();

  auto render = [&](uint32_t frames, std::vector<uint8_t>& out) -> bool {
    rhi::Buffer shot =
        CreateBuffer(ctx.device, (uint64_t)W * H * 4,
                     rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                     "giBounceShot");
    for (uint32_t f = 0; f < frames; f++) {
      WriteRenderParams(ctx.queue, world, eye, cam, (float)W / H, true, 0.0f,
                        kFarFogDensity, (float)H, noonTick);
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      sim.EncodeShadowResolve(enc);
      rhi::RenderPass rp = sim.BeginRenderPass(
          enc, c.view, rhi::TextureFormat::RGBA8Unorm, W, H);
      sim.DrawWorld(rp);
      rp.End();
      if (f + 1 == frames) {
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
    }
    out.assign((size_t)W * H * 4, 0);
    return rhi::ReadBufferBlocking(ctx.device, shot, 0, out.data(), out.size());
  };
  auto arm = [&](float strength, std::vector<uint8_t>& out) -> bool {
    Tuning t = base;
    t.render.giStrength = strength;
    SetCurrentTuning(t);
    // The F5 path: TUNE_GI_STRENGTH is const-folded in three shaders.
    if (!sim.ReloadShaders(ctx.device)) return false;
    return render(6, out);
  };

  std::vector<uint8_t> onPx, offPx;
  const bool got = arm(base.render.giStrength, onPx) && arm(0.0f, offPx);
  // Injection, read straight from the word the resolve pass wrote for the
  // slab top's +Y face at the foot of the wall — a cell that is IN THE FRAME,
  // because the resolve pass only ever sees patches somebody requested, and
  // the first version of this gate probed a cell below the bottom edge of the
  // picture and read a clean zero. Read while the GI arm's deposits are still
  // in the buffer (giStrength 0 folds the deposit away but never clears it).
  double floorRgb[3] = {0, 0, 0};
  UnpackRgb9e5(IrradianceWordAt(ctx, world, IVec3{wallX + 3, slabY + 1, gz}, 3u),
               floorRgb);
  SetCurrentTuning(base);
  const bool restored = sim.ReloadShaders(ctx.device);
  if (!got || !restored) {
    detail = got ? "shader restore failed" : "render/readback failed";
    return Status::Fail;
  }

  // Mean per-channel delta (on - off) over the middle of the frame.
  double dR = 0, dG = 0, dB = 0;
  size_t n = 0;
  for (uint32_t y = H * 3 / 10; y < H * 6 / 10; y++)
    for (uint32_t x = W * 3 / 10; x < W * 7 / 10; x++) {
      const size_t i = ((size_t)y * W + x) * 4;
      dR += (double)onPx[i] - offPx[i];
      dG += (double)onPx[i + 1] - offPx[i + 1];
      dB += (double)onPx[i + 2] - offPx[i + 2];
      n++;
    }
  if (n) { dR /= (double)n; dG /= (double)n; dB /= (double)n; }

  const double minGreen = BaselineNumber("giBounce.minGreenDelta", 2.0);
  const double minOverRed = BaselineNumber("giBounce.minGreenOverRed", 1.0);
  const bool injected = floorRgb[1] > floorRgb[0] && floorRgb[1] > 0.0;
  const bool gathered = dG >= minGreen && (dG - dR) >= minOverRed;

  // ---- P2: the write-back converges (docs/PLAN_gi.md §4) ----
  // With giFeedback on, every frame blends each pixel's outgoing radiance into
  // its own block-face and the next frame's gather reads it back: a fixed-point
  // iteration whose gain is albedo × the gather's form factor × giStrength.
  // The claim is that it CONVERGES — the wall's +X word after 600 frames is
  // within giBounce.convergePct of its value after 500 — and never runs away.
  // Sampled from the word itself rather than a pixel, so the tonemap cannot
  // hide a slow climb.
  bool converged = true;
  double wall500[3] = {0, 0, 0}, wall600[3] = {0, 0, 0};
  const double convergePct = BaselineNumber("giBounce.convergePct", 5.0);
  if (base.render.giFeedback > 0.0f) {
    std::vector<uint8_t> scratch;
    Tuning t = base;
    SetCurrentTuning(t);
    bool okc = sim.ReloadShaders(ctx.device) && render(500, scratch);
    const IVec3 wallCell{wallX, slabY + 2 + kWallH / 2, gz};
    UnpackRgb9e5(IrradianceWordAt(ctx, world, wallCell, 1u), wall500);
    okc = okc && render(100, scratch);
    UnpackRgb9e5(IrradianceWordAt(ctx, world, wallCell, 1u), wall600);
    SetCurrentTuning(base);
    const double l500 = 0.299 * wall500[0] + 0.587 * wall500[1] + 0.114 * wall500[2];
    const double l600 = 0.299 * wall600[0] + 0.587 * wall600[1] + 0.114 * wall600[2];
    converged = okc && l500 > 0.0 && std::isfinite(l600) &&
                std::fabs(l600 - l500) <= l500 * convergePct / 100.0;
  }
  const bool ok = injected && gathered && converged;
  std::printf(
      "gi-bounce: %s (white wall beside a sunlit green slab: bounce delta "
      "R %+.2f G %+.2f B %+.2f /255, G must be >= %.2f and exceed R by >= %.2f; "
      "the slab's own +Y irradiance word reads (%.3f, %.3f, %.3f), G must lead; "
      "write-back at giFeedback %.2f: wall word luminance %.4f after 500 frames, "
      "%.4f after 600, must agree within %.0f%%; %zu fixture cells at (%d,%d) "
      "ground y=%d)\n",
      ok ? "PASS" : "FAIL", dR, dG, dB, minGreen, minOverRed, floorRgb[0],
      floorRgb[1], floorRgb[2], base.render.giFeedback,
      0.299 * wall500[0] + 0.587 * wall500[1] + 0.114 * wall500[2],
      0.299 * wall600[0] + 0.587 * wall600[1] + 0.114 * wall600[2], convergePct,
      fixture.size(), gx, gz, ground);
  if (!ok) {
    WriteBmpFile("build/gi_bounce_on.bmp", onPx, W, H);
    WriteBmpFile("build/gi_bounce_off.bmp", offPx, W, H);
  }
  detail = Format("delta R %+.2f G %+.2f B %+.2f (G >= %.2f, G-R >= %.2f); floor "
                  "word (%.3f, %.3f, %.3f); write-back %s",
                  dR, dG, dB, minGreen, minOverRed, floorRgb[0], floorRgb[1],
                  floorRgb[2], converged ? "converged" : "DIVERGED");
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// gi-nightfall: no block-face keeps yesterday's sun.
//
// THE BUG THIS PINS (2026-09-02, reported as "random glowing green blocks at
// night; the first night is dark, then after a full day some voxels never lose
// brightness"). Two writers charge an irradiance word and only one of them can
// ever discharge it. The shadow resolve pass deposits full sunlight for any
// patch that is ON SCREEN and stops the instant the camera looks away, so it
// is a charger with no expiry. The openness walk is the discharger: it visits
// every face every kNumChunks / opennessChunksPerFrame ticks whether anyone is
// looking or not, and that is the ONLY thing standing between a face and a
// value from noon.
//
// It skipped a face whose CENTRE COLUMN held no blocker. A block is 4 voxels
// wide; wherever the ground rises or falls by a block inside a 4x4 footprint --
// a slope, a bank, a trunk, a cliff -- the block holds matter, the face
// marches (openValueAt finds its ray origin with a 2x2 quincunx, not the
// centre), and the centre column is empty air. openSunSample looked only at
// the centre, found nothing, and returned "no sample"; the caller read that as
// "leave the word alone". Those faces were charged once by daylight and then
// lit their neighbours with it forever, scattered over exactly the steep
// ground where the geometry does that, in the green of the grass that charged
// them.
//
// THE FIXTURE is two floating 4x4 plates, each filling one block's footprint,
// charged at noon and then CARVED DOWN TO ONE COLUMN so that the block still
// holds matter (its sub-occupancy bit stays set, so the walk still visits it
// and does not simply zero the word) while its centre column is empty:
//   A keeps a column the quincunx probes  -> the walk has a real sample again
//                                            and blends the night's zero in
//   B keeps a block CORNER, which no probe reaches -> nothing can measure it,
//                                            and the walk must FADE it
// Both are the bug; they are separated because the two halves of the fix are
// different lines, and a gate that only had A would pass with the fade removed.
//
// NO RENDER PASS AND NO CAMERA, on purpose: the resolve pass must not be able
// to contribute, or the gate would be measuring the charger. The only writer
// under test is the walk, and the sun it reads comes from the RenderParams the
// gate writes -- noon for the charge, midnight for the fade. Each tick pokes
// one cell of each plate so the chunk lands on the dirty list and the walk runs
// there EVERY tick instead of once per full sweep; that is what keeps this gate
// at ~35 ticks instead of ~4,000.
Status GateGiNightfall(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  const uint32_t mSolid = [&]() -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == "stone") return (uint32_t)i;
    return 0;
  }();
  if (!mSolid) {
    detail = "stone missing from materials.json";
    return Status::Fail;
  }
  const Tuning base = CurrentTuning();
  if (base.render.giStrength <= 0.0f) {
    detail = "render.giStrength is 0 - the deposit is compiled out";
    return Status::Fail;
  }
  if (base.render.giDecay <= 0.0f) {
    detail = "render.giDecay is 0 - an unmeasurable face can never fade";
    return Status::Fail;
  }

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // Block-aligned so "the centre column" and "a corner" mean what they say.
  const int kBlk = 1 << (int)kSubOccShift;
  const int gx = 300, gz = 300;
  const int ground = World::TerrainHeight(gx, gz, kDefaultSeed);
  const int by = (ground + 12) & ~(kBlk - 1);
  const int bxA = gx & ~(kBlk - 1);
  const int bzA = gz & ~(kBlk - 1);
  const int bxB = bxA + 2 * kBlk;   // a different block, same row
  const int bzB = bzA;
  const IVec3 pchunk{bxA / (int)kChunk, by / (int)kChunk, bzA / (int)kChunk};
  // The survivors: A on a quincunx column (centre +-1 in both tangents), B on
  // the block corner, which openSunSample's five columns never reach.
  const IVec3 keepA{bxA + 1, by, bzA + 1};
  const IVec3 keepB{bxB + 0, by, bzB + 0};

  std::vector<CellOp> plates, carve;
  bool sited = true;
  for (int b = 0; b < 2; b++) {
    const int ox = b ? bxB : bxA, oz = b ? bzB : bzA;
    const IVec3 keep = b ? keepB : keepA;
    for (int dx = 0; dx < kBlk; dx++)
      for (int dz = 0; dz < kBlk; dz++) {
        const IVec3 cc{ox + dx, by, oz + dz};
        if (!world.CellInWindow(cc)) { sited = false; continue; }
        plates.push_back({World::SlotCellIndex(cc), PackVoxNew(mSolid, 0u)});
        if (cc.x != keep.x || cc.z != keep.z)
          carve.push_back({World::SlotCellIndex(cc), PackVoxNew(0u, 0u)});
      }
  }
  if (!sited || plates.empty()) {
    detail = "fixture site is outside the residency window";
    return Status::Fail;
  }

  // Noon and midnight, scanned rather than hardcoded (the shadow-cache gate
  // says why): the walk's sun is whatever RenderParams carries.
  uint32_t noonTick = 0, midnightTick = 0;
  {
    float bestUp = -2.0f, worstUp = 2.0f;
    for (uint32_t t = 0; t < 200000u; t += 64u) {
      const float up = ComputeSky(base, (double)t).sunDir[1];
      if (up > bestUp) { bestUp = up; noonTick = t; }
      if (up < worstUp) { worstUp = up; midnightTick = t; }
    }
  }
  const Vec3 eye{(float)bxA, (float)(by + 8), (float)bzA};
  Camera cam;
  auto sky = [&](uint32_t skyTick) {
    WriteRenderParams(ctx.queue, world, eye, cam, 16.0f / 9.0f, true, 0.0f,
                      kFarFogDensity, 1080.0f, skyTick);
  };

  uint32_t tick = 80000;
  auto step = [&](const std::vector<CellOp>& cells) {
    SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, cells, false,
               pchunk, false, false);
  };
  // One cell of each plate, rewritten to what it already is: the chunk goes on
  // the dirty list and the walk runs there this tick.
  const std::vector<CellOp> poke{
      {World::SlotCellIndex(keepA), PackVoxNew(mSolid, 0u)},
      {World::SlotCellIndex(keepB), PackVoxNew(mSolid, 0u)}};

  // ---- charge: full plates under a noon sun ----
  sky(noonTick);
  step(plates);
  for (int i = 0; i < 6; i++) step(poke);
  ctx.WaitIdle();
  double dayA[3] = {0, 0, 0}, dayB[3] = {0, 0, 0};
  UnpackRgb9e5(IrradianceWordAt(ctx, world, keepA, 3u), dayA);
  UnpackRgb9e5(IrradianceWordAt(ctx, world, keepB, 3u), dayB);

  // ---- carve to one column, then night ----
  step(carve);
  sky(midnightTick);
  for (int i = 0; i < 24; i++) step(poke);
  ctx.WaitIdle();
  double nightA[3] = {0, 0, 0}, nightB[3] = {0, 0, 0};
  UnpackRgb9e5(IrradianceWordAt(ctx, world, keepA, 3u), nightA);
  UnpackRgb9e5(IrradianceWordAt(ctx, world, keepB, 3u), nightB);

  auto lum = [](const double v[3]) {
    return 0.299 * v[0] + 0.587 * v[1] + 0.114 * v[2];
  };
  const double lDayA = lum(dayA), lDayB = lum(dayB);
  const double lNightA = lum(nightA), lNightB = lum(nightB);
  const double maxPct = BaselineNumber("giNightfall.maxRemainPct", 5.0);
  // The charge has to be real or "it faded" is vacuous - the same three-arm
  // rule the shadow-cache gate follows.
  const double minDay = BaselineNumber("giNightfall.minDayLum", 0.02);
  const bool charged = lDayA >= minDay && lDayB >= minDay;
  const double pctA = lDayA > 0.0 ? 100.0 * lNightA / lDayA : 0.0;
  const double pctB = lDayB > 0.0 ? 100.0 * lNightB / lDayB : 0.0;
  const bool ok = charged && pctA <= maxPct && pctB <= maxPct;

  std::printf(
      "gi-nightfall: %s (two floating blocks charged at noon then carved to one "
      "column and left in the dark for 24 walked ticks; A keeps a probed column "
      "and B a block corner. Day luminance A %.4f B %.4f, both must be >= %.4f; "
      "night A %.4f (%.1f%%) B %.4f (%.1f%%), both must be <= %.1f%% of day. "
      "blocks at (%d,%d,%d)/(%d,%d,%d), giDecay %.2f)\n",
      ok ? "PASS" : "FAIL", lDayA, lDayB, minDay, lNightA, pctA, lNightB, pctB,
      maxPct, bxA, by, bzA, bxB, by, bzB, base.render.giDecay);
  detail = Format("A %.1f%% of day, B %.1f%% (<= %.1f%%); day lum %.4f/%.4f%s",
                  pctA, pctB, maxPct, lDayA, lDayB,
                  charged ? "" : " - NEVER CHARGED");
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

  // THE BOUNCE IS NOT UNDER TEST HERE, and it cannot be in the picture: the
  // reference arm has no resolve pass, and since P1 (docs/PLAN_gi.md §3) the
  // resolve pass is also what injects the irradiance grid, so with GI on the
  // cache arm is lit by bounce the reference arm never receives — measured
  // 1.87 mean |dL| of pure indirect light, and 45k pixels moving between warm
  // frames 3 and 4 as the grid's blend converged. Every arm below runs with
  // giStrength 0 so the only difference left is the shadow term; `orig` is
  // what gets restored.
  const Tuning orig = CurrentTuning();
  Tuning base = orig;
  base.render.giStrength = 0.0f;

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
  SetCurrentTuning(orig);
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

// body-shade: A RIGIDBODY IS LIT BY THE SAME SUN THE GROUND UNDER IT IS.
//
// WHAT IS UNDER TEST. Rigidbodies do not go through the raymarcher — they are
// rasterized cubes (debris.wgsl vsBody/fsBody, shaded by litColorS in
// common.wgsl). Until 2026-09-04 that path had NO SUN-SHADOW TERM AT ALL: a
// body received 100% of the key light wherever it stood, so a corpse, a rubble
// pile or a thrown grenade in shade was ~3.2x too bright in an ordinary cast
// shadow and ~10.4x indoors (owner report: "fine in full sun, washed out
// everywhere else"). Nothing else in the suite draws a body, so without this
// gate that regression is invisible to every check we have.
//
// THE MEASUREMENT IS A DIFFERENTIAL ON THE SHADOW TOGGLE, not an absolute
// brightness. WriteRenderParams(..., shadows, ...) sets R.flags bit 0, and that
// is the ONLY input that differs between the lit and shaded arms — same
// geometry, same camera, same tick, same openness and irradiance grids. So
// whatever moves between them moved because of the shadow term, and nothing
// else can be blamed for it.
//
// WHY THE BODY IS SYNTHESIZED RATHER THAN BROKEN OFF A WALL. The first version
// of this gate built a floating cube and let AddDestructionEvent's island scan
// turn it into real debris. That works, but it tests the DESTRUCTION path (an
// async island scan, Jolt, 60 ticks of settling) to get at the SHADING path,
// and the `debris` gate already owns the former. Writing the instance buffer
// directly — exactly as `fire-depth` above does — puts the same cubes through
// the same vsBody/fsBody pipeline in a handful of ticks instead of sixty, and
// makes the body position a constant of the fixture rather than an outcome of
// physics. What is under test is unchanged: these are the real body draws.
//
// TWO EARLIER VERSIONS OF THIS GATE PASSED WHILE MEASURING NOTHING, and both
// failure modes are worth stating because either would silently return green:
//   1. It called sim.DrawWorld() alone. DrawWorld does NOT draw bodies —
//      DrawBodies() is a separate call, after BuildInstances and an upload
//      (main.cpp:2250, selftest_phys.cpp:176). So no body was ever on screen.
//   2. Its body mask was "pixels that changed between a frame taken before the
//      body existed and one taken after". Sixty ticks separated those frames,
//      so the openness grid had caught up in between and the mask was full of
//      TERRAIN whose lighting had drifted — which is why it reported the same
//      number with the shadow term forced off as with it on.
// The mask below cannot fail that way: the reference arm is the same frame at
// the same tick with the body draw count set to 0, so a pixel differs if and
// only if a body cube covered it.
Status GateBodyShade(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const uint32_t W = c.width, H = c.height;

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // Same site as `openness` and `gi-bounce`, for the same reason: it is known
  // to sit inside the residency window wherever streaming has left it.
  const int gx = 300, gz = 300;
  const int ground = World::TerrainHeight(gx, gz, kDefaultSeed);
  const int kHalf = 22;             // 45x45 plates
  const int floorY = ground + 8;    // the deck the body rests on
  const int roofY = floorY + 13;    // the roof that shadows it
  const IVec3 pchunk{gx / 16, floorY / 16, gz / 16};

  std::vector<CellOp> fixture;
  auto put = [&](int x, int y, int z, uint32_t m) {
    const IVec3 cc{x, y, z};
    if (!world.CellInWindow(cc)) return;
    fixture.push_back({World::SlotCellIndex(cc), PackVoxNew(m, 0u)});
  };
  // A DECK AND A ROOF, both two voxels thick. Two thick because a one-voxel
  // plate is a one-voxel shadow caster and the ray TUNE_SHADOW_BIAS start
  // offset can step straight over it — the gate would then be measuring a bias
  // tuning rather than a shadow.
  for (int dx = -kHalf; dx <= kHalf; dx++)
    for (int dz = -kHalf; dz <= kHalf; dz++) {
      put(gx + dx, floorY, gz + dz, kMatStone);
      put(gx + dx, floorY + 1, gz + dz, kMatStone);
      put(gx + dx, roofY, gz + dz, kMatStone);
      put(gx + dx, roofY + 1, gz + dz, kMatStone);
    }
  if (fixture.empty()) {
    detail = "fixture site is outside the residency window";
    return Status::Fail;
  }
  uint32_t tick = 90000;
  SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, fixture, false,
             pchunk, false, false);
  ctx.WaitIdle();
  // The openness grid refreshes on a rolling walk, so the deck and roof do not
  // know about each other on the tick they were written. A few ticks let the
  // walk reach them; without this the deck reports open sky and the AMBIENT
  // term rather than the shadow carries the difference.
  for (int i = 0; i < 6; i++)
    SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, {}, false, pchunk,
               false, false);
  ctx.WaitIdle();

  // Noon, scanned rather than hardcoded (the shadow-cache gate says why). A
  // near-vertical sun is what makes the roof directly overhead the blocker.
  const Tuning base = CurrentTuning();
  uint32_t noonTick = 0;
  {
    float bestUp = -2.0f;
    for (uint32_t t = 0; t < 200000u; t += 64u) {
      const float up = ComputeSky(base, (double)t).sunDir[1];
      if (up > bestUp) { bestUp = up; noonTick = t; }
    }
  }

  // ---- the body: one 5^3 stone cube resting on the deck at the centre ----
  // Packed by hand rather than through rigrender, for the reason fire-depth
  // gives above. Slot 0, no art colour — see BodyVoxInst in phys/debris.h.
  const int kB = 5;
  const float bodyBaseY = (float)(floorY + 2);
  std::vector<BodyVoxInst> inst;
  for (int x = 0; x < kB; x++)
    for (int y = 0; y < kB; y++)
      for (int z = 0; z < kB; z++)
        inst.push_back({(float)x, (float)y, (float)z, kMatStone});
  std::vector<BodyXformGpu> xf;
  {
    BodyXformGpu m{};
    m.pos[0] = (float)gx - (float)kB * 0.5f;
    m.pos[1] = bodyBaseY;
    m.pos[2] = (float)gz - (float)kB * 0.5f;
    m.quat[3] = 1.0f;   // identity: (x,y,z,w)
    xf.push_back(m);
  }
  ctx.queue.WriteBuffer(world.bodyInstances, 0, inst.data(),
                        inst.size() * sizeof(BodyVoxInst));
  ctx.queue.WriteBuffer(world.bodyXforms, 0, xf.data(),
                        xf.size() * sizeof(BodyXformGpu));

  // Inside the roofed volume, looking across the deck at the cube from a
  // little above it, so the frame holds the body AND the deck under it.
  const Vec3 eye{(float)gx - 15.0f, bodyBaseY + 4.0f, (float)gz - 15.0f};
  Camera cam;
  cam.yaw = 0.785f;     // toward +X/+Z, i.e. at the cube
  cam.pitch = -0.18f;

  auto render = [&](bool shadows, uint32_t bodyInstances,
                    std::vector<uint8_t>& out) -> bool {
    rhi::Buffer shot =
        CreateBuffer(ctx.device, (uint64_t)W * H * 4,
                     rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                     "bodyShadeShot");
    // FOUR FRAMES, GRAB THE LAST, exactly as --shot does: the shadow cache
    // resolves a patch one frame after a pixel asks for it, so a single frame
    // per arm would compare a warm cache against a cold one and report the
    // cache latency as the body shadow.
    for (uint32_t f = 0; f < 4; f++) {
      WriteRenderParams(ctx.queue, world, eye, cam, (float)W / H, shadows, 0.0f,
                        kFarFogDensity, (float)H, noonTick);
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      sim.EncodeShadowResolve(enc);
      rhi::RenderPass rp = sim.BeginRenderPass(
          enc, c.view, rhi::TextureFormat::RGBA8Unorm, W, H);
      sim.DrawWorld(rp);
      // THE CALL THE FIRST VERSION OF THIS GATE OMITTED. DrawWorld draws the
      // raymarched world only; a body reaches the screen through here.
      sim.DrawBodies(rp, bodyInstances);
      rp.End();
      if (f == 3) {
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
    }
    out.assign((size_t)W * H * 4, 0);
    return rhi::ReadBufferBlocking(ctx.device, shot, 0, out.data(), out.size());
  };

  // Three arms of ONE world state. `noBody` differs from `lit` only in the
  // body draw count, so the pixels that differ ARE the body — an
  // independently-derived mask rather than a projected rectangle this gate
  // would then be asserting its own arithmetic against.
  std::vector<uint8_t> noBody, lit, shaded;
  const uint32_t n = (uint32_t)inst.size();
  if (!render(false, 0, noBody) || !render(false, n, lit) ||
      !render(true, n, shaded)) {
    detail = "render/readback failed";
    return Status::Fail;
  }

  auto lum = [](const std::vector<uint8_t>& px, size_t i) {
    return 0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2];
  };
  // Pass 1: the mask and its bounding box.
  std::vector<uint8_t> isBody((size_t)W * H, 0);
  uint32_t bx0 = W, bx1 = 0, by0 = H, by1 = 0;
  size_t nBody = 0;
  for (uint32_t y = 0; y < H; y++)
    for (uint32_t x = 0; x < W; x++) {
      const size_t i = ((size_t)y * W + x) * 4;
      if (std::fabs(lum(lit, i) - lum(noBody, i)) <= 8.0) continue;
      isBody[(size_t)y * W + x] = 1;
      bx0 = std::min(bx0, x); bx1 = std::max(bx1, x);
      by0 = std::min(by0, y); by1 = std::max(by1, y);
      nBody++;
    }
  if (nBody < 500) {
    detail = Format("only %zu body pixels — the cube is not in frame (or "
                    "DrawBodies drew nothing)", nBody);
    return Status::Fail;
  }

  // Pass 2: the terrain reference is THE DECK IMMEDIATELY UNDER AND AROUND THE
  // BODY — the band below the body bounding box, widened by half its width.
  // Not "every non-body pixel": the frame also holds sky past the deck edge and
  // roof overhead, neither of which responds to a cast shadow the way a floor
  // does, and averaging them in would dilute the denominator of the ratio with
  // surfaces the body is not standing on.
  const uint32_t bw = bx1 - bx0 + 1;
  const uint32_t tx0 = (uint32_t)std::max(0, (int)bx0 - (int)bw / 2);
  const uint32_t tx1 = std::min(W - 1, bx1 + bw / 2);
  const uint32_t ty0 = std::min(H - 1, by1 + 2);
  const uint32_t ty1 = std::min(H - 1, by1 + 2 + (by1 - by0) + 40);
  double bodyLit = 0, bodyShaded = 0, terrLit = 0, terrShaded = 0;
  size_t nTerr = 0;
  for (uint32_t y = 0; y < H; y++)
    for (uint32_t x = 0; x < W; x++) {
      const size_t p = (size_t)y * W + x, i = p * 4;
      if (isBody[p]) { bodyLit += lum(lit, i); bodyShaded += lum(shaded, i); }
      else if (y >= ty0 && y <= ty1 && x >= tx0 && x <= tx1) {
        terrLit += lum(lit, i); terrShaded += lum(shaded, i); nTerr++;
      }
    }
  if (nTerr < 500) {
    detail = Format("only %zu deck pixels under the body (%zu body px) — the "
                    "camera is not looking at the floor", nTerr, nBody);
    return Status::Fail;
  }
  bodyLit /= (double)nBody;   bodyShaded /= (double)nBody;
  terrLit /= (double)nTerr;   terrShaded /= (double)nTerr;

  // The response to the shadow toggle, as a FRACTION of the surface own lit
  // brightness — normalising by brightness is what makes a dark body and a
  // pale deck comparable at all.
  const double bodyResp = (bodyLit - bodyShaded) / std::max(bodyLit, 1.0);
  const double terrResp = (terrLit - terrShaded) / std::max(terrLit, 1.0);
  const double ratio = terrResp > 1e-6 ? bodyResp / terrResp : 0.0;

  // Thresholds in baseline.json, not here (CLAUDE.md: a threshold in source
  // costs a rebuild to tune). The band is deliberately wide: the claim is "the
  // body is shadowed roughly as much as the ground it lies on", and the two
  // paths legitimately differ — a body gets no voxel AO and no GI bounce, and
  // its faces are not axis-aligned with the grid. Measured with the shadow
  // term forced off, bodyResp is 0.00 and the ratio 0.00, which is the
  // regression this exists to catch.
  const double minTerr = BaselineNumber("bodyShade.minTerrainResponse", 0.05);
  const double minRatio = BaselineNumber("bodyShade.minRatio", 0.40);
  const double maxRatio = BaselineNumber("bodyShade.maxRatio", 2.50);
  // Checked FIRST so a broken fixture reports as a broken fixture rather than
  // as a body-shading regression: if the deck itself is not in shadow, the
  // ratio denominator is noise and the whole number is meaningless.
  const bool fixtureOk = terrResp >= minTerr;
  const bool ok = fixtureOk && ratio >= minRatio && ratio <= maxRatio;

  detail = Format(
      "body dims %.3f of its lit level when shadows go on, the deck under it "
      "%.3f — ratio %.2f, must be %.2f..%.2f (with the body path shadow forced "
      "off: 0.000 and 0.00). Deck response must exceed %.2f or the fixture "
      "casts nothing. %zu body px (%.1f lit, %.1f shaded), %zu deck px "
      "(%.1f lit, %.1f shaded); %d^3 cube on a deck at y=%d under a roof at "
      "y=%d",
      bodyResp, terrResp, ratio, minRatio, maxRatio, minTerr, nBody, bodyLit,
      bodyShaded, nTerr, terrLit, terrShaded, kB, floorY + 1, roofY);
  if (!fixtureOk)
    detail += " — FIXTURE, not the body path: the deck is not in shadow";
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace


// ---- plants: the analytic plants load, draw, and flatten under a foot ------
//
// Three claims, each cheap enough to iterate on with `--gate plants` alone:
//   1. LOADER: every shipped analytic species parses as a `plant` block of the
//      right kind (a typo in a param name is logged and the material silently
//      falls back to the cube path — this is where that would show).
//   2. RING: the trample ring keeps ONE stamp for a presser standing still,
//      lays another when it moves, and drops both once recovered.
//   3. PICTURE: a stand of tall grass, a fern tile and a big toadstool painted
//      on bare ground change the frame against the bare ground (the plants
//      draw at all — a miss that blocked, or a plant that vanished into its
//      clip, both fail here), and one foot stamp under the stand changes a
//      SUBSET of those pixels (the trample reaches the shader and does not
//      repaint the world). The three frames are written out as BMPs, because
//      "it drew something" is the most a pixel count can say about a fern.
Status GatePlants(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const uint32_t W = c.width, H = c.height;

  // ---- 1. the loader ----
  std::vector<MaterialDef> mats = c.mats;   // a copy: LoadMicroVox sets flags
  MicroSet ms;
  std::string log;
  const bool loaded = LoadMicroVox(AssetDir() + "/materials/materials.json",
                                   AssetDir(), mats, ms, log);
  int plantMats = 0;
  bool kinds[5] = {false, false, false, false, false};
  for (size_t i = 0; i < ms.table.size(); i++) {
    const MicroBrickGpu& b = ms.table[i];
    if (b.base == kMicroNoBrick || !(b.flags & kMicroPlant)) continue;
    plantMats++;
    const uint32_t kind = ms.pool[b.base] & 0xFFu;
    if (kind < 5) kinds[kind] = true;
  }
  auto isPlant = [&](uint32_t id) {
    return id < ms.table.size() && ms.table[id].base != kMicroNoBrick &&
           (ms.table[id].flags & kMicroPlant) != 0;
  };
  const bool loaderOk = loaded && kinds[kPlantGrass] && kinds[kPlantFlower] &&
                        kinds[kPlantMushroom] && kinds[kPlantFern] &&
                        isPlant(kMatTallGrass) && isPlant(kMatFern) &&
                        isPlant(kMatMushroomLarge) && isPlant(kMatGrassTuft) &&
                        plantMats >= 13;
  if (!log.empty()) std::printf("plants: micro loader said:\n%s", log.c_str());

  // ---- 2. the ring ----
  TrampleRing ring;
  ring.Press(10.0f, 10.0f, 5.0f, 3.0f, 1.0f, 0.0f);
  ring.Press(10.4f, 10.1f, 5.0f, 3.0f, 1.0f, 0.05f);   // still standing in it
  const bool oneStamp = ring.Count() == 1;
  ring.Press(14.0f, 10.0f, 5.0f, 3.0f, 1.0f, 0.10f);   // stepped out of it
  const bool twoStamps = ring.Count() == 2;
  ring.Expire(0.10f + kTrampleHoldSec + 1.4f + 0.01f, 1.4f);
  const bool expired = ring.Count() == 0;
  const bool ringOk = oneStamp && twoStamps && expired;

  // ---- 3. the picture ----
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  // A flat-ish site inside the window, well away from the fixture pads.
  const int gx = 300, gz = 220;
  const int gh = World::TerrainHeight(gx, gz, kDefaultSeed);
  const IVec3 pchunk{gx / 16, gh / 16, gz / 16};

  std::vector<CellOp> plants, clear;
  auto put = [&](IVec3 cc, uint32_t m) {
    if (!world.CellInWindow(cc)) return;
    plants.push_back({World::SlotCellIndex(cc), PackVoxNew(m, 0u)});
    clear.push_back({World::SlotCellIndex(cc), PackVoxNew(0u, 0u)});
  };
  // A 9x9 stand of tall grass, 4-8 cells, on ONE ground level so the frame is
  // about the plants and not the hillside under them.
  for (int dz = -4; dz <= 4; dz++)
    for (int dx = -4; dx <= 4; dx++) {
      uint32_t r = rng::Hash3(0x91A57u, (uint32_t)(gx + dx), (uint32_t)(gz + dz));
      const int height = 4 + (int)(r % 5u);
      for (int k = 1; k <= height; k++)
        put({gx - 9 + dx, gh + k, gz + 6 + dz},
            k == height ? kMatTallGrassHead : kMatTallGrass);
    }
  // A fern tile and a toadstool tile beside it, exactly where the renderer
  // rebuilds them (sim/plants.h is plantTileAt's CPU twin).
  auto paintTile = [&](int tx, int tz, uint32_t salt, int tile, int foot, int minH,
                       int maxH, uint32_t mat) {
    PlantTileCpu pt = PlantTileAtCpu(tx * tile, tz * tile, kDefaultSeed, salt,
                                     tile, foot, minH, maxH, 100u);
    const int half = foot / 2;
    const int hc = World::TerrainHeight(pt.cx, pt.cz, kDefaultSeed);
    for (int dz = -half; dz <= half; dz++)
      for (int dx = -half; dx <= half; dx++)
        for (int k = 1; k <= pt.h; k++) put({pt.cx + dx, hc + k, pt.cz + dz}, mat);
  };
  paintTile((gx + 2) / kPlantFernTile, (gz + 2) / kPlantFernTile, kPlantFernSalt,
            kPlantFernTile, kPlantFernFoot, kPlantFernMinH, kPlantFernMaxH, kMatFern);
  paintTile((gx + 10) / kPlantShroomTile, (gz + 3) / kPlantShroomTile, kPlantShroomSalt,
            kPlantShroomTile, kPlantShroomFoot, kPlantShroomMinH, kPlantShroomMaxH,
            kMatMushroomLarge);

  Camera cam;
  cam.yaw = 1.5708f;   // +Z: the fern dead ahead, the stand and toadstool beside it
  cam.pitch = -0.6f;
  const Vec3 eye{(float)gx + 0.5f, (float)gh + 6.0f, (float)gz - 4.0f};
  const uint32_t noon = TicksPerDayFromTuning(CurrentTuning()) / 2;
  auto frame = [&](std::vector<uint8_t>& out, const char* bmp) {
    WriteRenderParams(ctx.queue, world, eye, cam, (float)W / H, true, 0.0f,
                      kFarFogDensity, 1080.0f, noon);
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    rhi::RenderPass rp =
        sim.BeginRenderPass(enc, c.view, rhi::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    rp.End();
    ctx.queue.Submit(enc.Finish());
    ctx.WaitIdle();
    rhi::Buffer shot = CreateBuffer(ctx.device, (uint64_t)W * H * 4,
                                    rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                    "plantsShot");
    rhi::CommandEncoder enc2 = ctx.device.CreateCommandEncoder();
    rhi::TexelCopyTexture srcT{};
    srcT.texture = c.offscreen;
    rhi::TexelCopyBuffer dstB{};
    dstB.buffer = shot;
    dstB.bytesPerRow = W * 4;
    dstB.rowsPerImage = H;
    rhi::Extent3D ext{W, H, 1};
    enc2.CopyTextureToBuffer(srcT, dstB, ext);
    ctx.queue.Submit(enc2.Finish());
    out.assign((size_t)W * H * 4, 0);
    rhi::ReadBufferBlocking(ctx.device, shot, 0, out.data(), out.size());
    if (bmp) WriteBmpFile(bmp, out, W, H);
  };
  auto differing = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t n = 0;
    for (size_t i = 0; i + 3 < a.size(); i += 4) {
      const int d = std::abs((int)a[i] - (int)b[i]) + std::abs((int)a[i + 1] - (int)b[i + 1]) +
                    std::abs((int)a[i + 2] - (int)b[i + 2]);
      if (d > 24) n++;
    }
    return n;
  };

  uint32_t t = 41000;
  std::vector<uint8_t> bare, grown, pressed;
  Tramples().Clear();
  frame(bare, nullptr);
  SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, plants, false, pchunk,
             false, false);
  ctx.WaitIdle();
  frame(grown, "plants_grown.bmp");
  // One foot in the middle of the stand, pressed 1 s ago and still pressed at
  // the frame's R.time of 0 (WriteRenderParams passes time 0 in the harness).
  Tramples().Press((float)gx - 8.5f, (float)gz + 6.5f, (float)(gh + 1), 3.0f, 1.0f, -1.0f);
  Tramples().Press((float)gx - 8.5f, (float)gz + 6.5f, (float)(gh + 1), 3.0f, 1.0f, -0.05f);
  frame(pressed, "plants_pressed.bmp");
  Tramples().Clear();
  // Leave the world as this gate found it.
  SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, clear, false, pchunk,
             false, false);
  ctx.WaitIdle();

  const size_t grewPx = differing(bare, grown);
  const size_t pressPx = differing(grown, pressed);
  const bool drewOk = grewPx > 20000;
  const bool pressOk = pressPx > 300 && pressPx < grewPx;

  detail = Format("loader %s (%d plant materials) | ring %s | drew %zu px, "
                  "foot changed %zu px",
                  loaderOk ? "ok" : "FAIL", plantMats, ringOk ? "ok" : "FAIL",
                  grewPx, pressPx);
  return (loaderOk && ringOk && drewOk && pressOk) ? Status::Pass : Status::Fail;
}

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
      // No render pass: it reads a compute-written buffer back, like the far
      // gates above and unlike the three that draw.
      {"openness", "render", {}, false, GateOpenness},
      // Draws (two arms of a fixture frame) AND reads a word back.
      {"gi-bounce", "render", {}, false, GateGiBounce, /*needsRender=*/true},
      // No render pass on purpose: the resolve pass must not be able to
      // contribute, or the gate would be measuring the charger.
      {"gi-nightfall", "render", {}, false, GateGiNightfall},
      // The only gate in the suite that DRAWS A RIGIDBODY. Three arms of one
      // fixture frame, and it spawns a body through the real destruction path.
      {"body-shade", "render", {}, false, GateBodyShade, /*needsRender=*/true},
      // Draws three frames of a painted stand and reads them back.
      {"plants", "render", {}, false, GatePlants, /*needsRender=*/true},
  };
  return g;
}

}  // namespace selftest
