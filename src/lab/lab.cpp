// lab.cpp — the fluid lab: scenes, bench harness, tuning-file plumbing.
// See lab.h and docs/PLAN_fluid_overhaul.md §4 (WP1).

#include "lab/lab.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "gpu/context.h"
#include "gpu/passtimer.h"
#include "gpu/resources.h"
#include "sim/simulation.h"
#include "sim/tuning.h"
#include "test/support.h"

namespace sandvox {
namespace {

// ---- shared scene frame ----------------------------------------------------
// First air row above the slab, and the scene anchor. All scene geometry is
// absolute world voxels around the window-center column, window origin
// {0,0,0} — the lab never shifts the window.
constexpr int G = kLabSlabY + 1;   // 128
constexpr int CX = 256, CZ = 256;

// Per-tick pour ceiling in CELLS (8 particles each; kMaxFluidSpawnsPerTick /8).
constexpr uint32_t kPourCellsPerTick = kMaxFluidSpawnsPerTick / 8;  // 512

const char* kSceneNames[kLabSceneCount] = {"basin", "hill", "faucet", "pool",
                                           "slosh"};

// The hill ramp: solid up to `G + 19 - drop(x)` for ramp x-offsets 0..31.
// (dx*3)/5 steps 1 voxel down every 1-2 columns — a stepped ~31 deg slope,
// the plan's "~30° stepped stone ramp".
constexpr int kHillTop = 19;      // deck height above G
constexpr int kHillRampX0 = 216;  // first ramp column
constexpr int kHillRampX1 = 247;  // last ramp column (drop 18 -> surface G+1)
int HillDrop(int x) { return ((x - kHillRampX0) * 3) / 5; }

// Scene material at a world cell, or ~0u for "outside this scene's volume".
// One function per scene keeps the build list and the reset trivially the
// same thing: LabSceneBuildOps just walks the volume and asks this.
uint32_t SceneMatAt(int scene, int x, int y, int z) {
  switch (scene) {
    case kLabBasin: {
      // Walled 24x24 box, walls 2 thick, 16 high. Interior [244,267]^2.
      if (y > G + 15) return kMatAir;
      const bool wall = x < 244 || x > 267 || z < 244 || z > 267;
      return wall ? kMatStone : kMatAir;
    }
    case kLabHill: {
      // Channel z: interior [248,263], walls [246,247] and [264,265].
      const bool zWall = z < 248 || z > 263;
      // x zones: back wall | pour deck | stepped ramp | catch basin | far wall.
      if (x <= 203) return y <= G + 26 ? kMatStone : kMatAir;      // back wall
      int wallTop, floorTop;
      if (x <= 215) {              // pour deck
        floorTop = G + kHillTop;
        wallTop = G + 26;
      } else if (x <= kHillRampX1) {  // the ramp
        floorTop = G + kHillTop - HillDrop(x);
        floorTop = std::max(floorTop, G - 1);
        wallTop = floorTop + 6;
      } else if (x <= 269) {       // catch basin interior (floor = the slab)
        floorTop = G - 1;
        wallTop = G + 7;
      } else {                     // far wall
        return y <= G + 7 ? kMatStone : kMatAir;
      }
      if (zWall) return y <= wallTop ? kMatStone : kMatAir;
      return y <= floorTop ? kMatStone : kMatAir;
    }
    case kLabFaucet: {
      // Walled 20x20 basin under a point pour. Interior [246,265]^2, walls 10.
      if (y > G + 9) return kMatAir;
      const bool wall = x < 246 || x > 265 || z < 246 || z > 265;
      return wall ? kMatStone : kMatAir;
    }
    case kLabPool: {
      // Walled 16x16 pool. Interior [248,263]^2, walls 10 high.
      if (y > G + 9) return kMatAir;
      const bool wall = x < 248 || x > 263 || z < 248 || z > 263;
      return wall ? kMatStone : kMatAir;
    }
    case kLabSlosh: {
      // 48x10 channel, walls 2 thick, 12 high. Interior x [232,279], z
      // [251,260].
      if (y > G + 11) return kMatAir;
      const bool wall = x < 232 || x > 279 || z < 251 || z > 260;
      return wall ? kMatStone : kMatAir;
    }
  }
  return kMatAir;
}

// One cell's 8 spawn particles on the half-cell lattice with deterministic
// jitter — byte-for-byte the mpm dev tool's shape (main.cpp pour), so the lab
// exercises exactly the path the user pours through. `salt` is the scene
// tick; jitter is hash(salt, index) only (rule 1 discipline).
void EmitCell(int x, int y, int z, int32_t vx, int32_t vy, int32_t vz,
              uint32_t mat, uint32_t salt, std::vector<FluidSpawnOp>& out) {
  for (int s = 0; s < 8; s++) {
    uint32_t h = (salt * 9781u + (uint32_t)out.size() * 6271u) * 747796405u +
                 2891336453u;
    FluidSpawnOp op{};
    op.px = (x << 16) + ((s & 1) ? 49152 : 16384) + (int32_t)(h % 8192u) - 4096;
    op.py = (y << 16) + ((s & 2) ? 49152 : 16384) +
            (int32_t)((h >> 13) % 8192u) - 4096;
    op.pz = (z << 16) + ((s & 4) ? 49152 : 16384) +
            (int32_t)((h >> 19) % 8192u) - 4096;
    op.vx = vx;
    op.vy = vy;
    op.vz = vz;
    op.species = 0;
    op.mat = mat;
    out.push_back(op);
  }
}

// Budget test for one more cell (rule 2: charged BEFORE emission, cell
// refused whole).
bool CellFits(uint32_t liveEstimate, const std::vector<FluidSpawnOp>& out) {
  if (liveEstimate + out.size() + 8 > kFluidCap) return false;
  if (out.size() + 8 > kMaxFluidSpawnsPerTick) return false;
  return true;
}

// Instant-block scenes (basin dam, slosh wave): the block's cells in a fixed
// order, spawned kPourCellsPerTick cells per tick starting at `firstTick`.
struct BlockSpawn {
  IVec3 lo, hi;   // inclusive cell box
  int32_t vx, vy, vz;
  uint32_t firstTick;
};

BlockSpawn SceneBlock(int scene) {
  if (scene == kLabBasin)
    // Dam column against the -x wall: 8 x 24 x 10 cells = 15,360 eighths,
    // standing (v = 0) — the wall "removal" is just that nothing holds it.
    return {{244, G, 244}, {251, G + 9, 267}, 0, 0, 0, 10};
  // Slosh: a raised block at the -x end of the channel, launched at +0.3
  // cells/tick — collapses into a travelling wave.
  return {{232, G, 251}, {241, G + 7, 260}, 19661, 0, 0, 5};
}

void BlockPour(const BlockSpawn& b, uint32_t sceneTick, uint32_t liveEstimate,
               uint32_t waterMat, std::vector<FluidSpawnOp>& out) {
  const int nx = b.hi.x - b.lo.x + 1, ny = b.hi.y - b.lo.y + 1,
            nz = b.hi.z - b.lo.z + 1;
  const uint32_t total = (uint32_t)(nx * ny * nz);
  if (sceneTick < b.firstTick) return;
  const uint32_t ti = sceneTick - b.firstTick;
  const uint32_t k0 = ti * kPourCellsPerTick;
  if (k0 >= total) return;
  const uint32_t k1 = std::min(k0 + kPourCellsPerTick, total);
  for (uint32_t k = k0; k < k1; k++) {
    if (!CellFits(liveEstimate, out)) return;
    const int x = b.lo.x + (int)(k % (uint32_t)nx);
    const int y = b.lo.y + (int)((k / (uint32_t)nx) % (uint32_t)ny);
    const int z = b.lo.z + (int)(k / (uint32_t)(nx * ny));
    EmitCell(x, y, z, b.vx, b.vy, b.vz, waterMat, sceneTick, out);
  }
}

// Sustained-pour scenes: a radius-r sphere of cells above the scene, every
// tick in [first, last], falling gently (-0.3 cells/tick) like the dev tool.
void SpherePour(IVec3 at, int r, uint32_t sceneTick, uint32_t first,
                uint32_t last, uint32_t liveEstimate, uint32_t waterMat,
                std::vector<FluidSpawnOp>& out) {
  if (sceneTick < first || sceneTick > last) return;
  for (int z = -r; z <= r; z++)
    for (int y = -r; y <= r; y++)
      for (int x = -r; x <= r; x++) {
        if (x * x + y * y + z * z > r * r) continue;
        if (!CellFits(liveEstimate, out)) return;
        EmitCell(at.x + x, at.y + y, at.z + z, 0, -19661, 0, waterMat,
                 sceneTick, out);
      }
}

double Pct(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[(size_t)(p * (double)(v.size() - 1))];
}

}  // namespace

int LabSceneFromName(const std::string& name) {
  for (int i = 0; i < kLabSceneCount; i++)
    if (name == kSceneNames[i]) return i;
  return -1;
}

const char* LabSceneName(int scene) {
  return (scene >= 0 && scene < kLabSceneCount) ? kSceneNames[scene] : "?";
}

void LabSceneCamera(int scene, Vec3& eye, float& yaw, float& pitch) {
  // Eye + look target per scene; yaw/pitch derived (Camera convention:
  // yaw = atan2(dz, dx), the RunMobShot shape).
  // Eyes sit well above the wall tops so the interior — where the water is —
  // fills the frame rather than the outside of a stone box.
  Vec3 target{(float)CX, (float)G, (float)CZ};
  switch (scene) {
    case kLabBasin:  eye = {222, (float)(G + 30), 222}; break;
    case kLabHill:   eye = {200, (float)(G + 34), 218};
                     target = {236, (float)(G + 2), 256}; break;
    case kLabFaucet: eye = {224, (float)(G + 24), 224}; break;
    case kLabPool:   eye = {226, (float)(G + 22), 226}; break;
    case kLabSlosh:  eye = {256, (float)(G + 24), 224};
                     target = {256, (float)G, 256}; break;
    default:         eye = {222, (float)(G + 30), 222}; break;
  }
  Vec3 d = target - eye;
  const float flat = std::sqrt(d.x * d.x + d.z * d.z);
  yaw = std::atan2(d.z, d.x);
  pitch = std::atan2(d.y, flat);
}

void LabSceneBuildOps(int scene, uint32_t sceneTick, uint32_t /*waterMat*/,
                      std::vector<CellOp>& out) {
  // The whole build lands on scene tick 1: every scene volume is well under
  // kMaxCellOpsPerTick (largest is hill at 43,400 of 65,536).
  if (sceneTick != 1) return;
  IVec3 lo, hi;
  LabSceneBounds(scene, lo, hi);
  for (int y = lo.y; y <= hi.y; y++)
    for (int z = lo.z; z <= hi.z; z++)
      for (int x = lo.x; x <= hi.x; x++)
        out.push_back({World::SlotCellIndex({x, y, z}),
                       SceneMatAt(scene, x, y, z) & 0xFFFu});
}

void LabScenePour(int scene, uint32_t sceneTick, uint32_t liveEstimate,
                  uint32_t waterMat, std::vector<FluidSpawnOp>& out) {
  switch (scene) {
    case kLabBasin:
    case kLabSlosh:
      BlockPour(SceneBlock(scene), sceneTick, liveEstimate, waterMat, out);
      return;
    case kLabHill:
      // Pour onto the deck; the only exit is the ramp. 33 cells x 8 = 264
      // particles/tick for 150 ticks = 39,600 eighths.
      SpherePour({210, G + 23, CZ}, 2, sceneTick, 10, 159, liveEstimate,
                 waterMat, out);
      return;
    case kLabFaucet:
      // The one open-ended pour: runs until the budget refuses (windowed) or
      // the bench run ends. 7 cells x 8 = 56 particles/tick.
      SpherePour({CX, G + 22, CZ}, 1, sceneTick, 10, ~0u, liveEstimate,
                 waterMat, out);
      return;
    case kLabPool:
      SpherePour({CX, G + 20, CZ}, 2, sceneTick, 10, 109, liveEstimate,
                 waterMat, out);
      return;
  }
}

uint32_t LabScenePourEnd(int scene) {
  switch (scene) {
    case kLabBasin:  return 13;    // 1,920 cells / 512 per tick from tick 10
    case kLabHill:   return 159;
    case kLabFaucet: return ~0u;   // never stops
    case kLabPool:   return 109;
    case kLabSlosh:  return 6;     // 800 cells / 512 per tick from tick 5
  }
  return 0;
}

uint32_t LabSceneBenchTicks(int scene) {
  switch (scene) {
    case kLabBasin:  return 400;
    case kLabHill:   return 600;
    case kLabFaucet: return 600;
    case kLabPool:   return 500;
    case kLabSlosh:  return 500;
  }
  return 400;
}

void LabSceneBounds(int scene, IVec3& lo, IVec3& hi) {
  switch (scene) {
    case kLabBasin:  lo = {242, G, 242}; hi = {269, G + 23, 269}; return;
    case kLabHill:   lo = {202, G, 246}; hi = {271, G + 30, 265}; return;
    case kLabFaucet: lo = {244, G, 244}; hi = {267, G + 26, 267}; return;
    case kLabPool:   lo = {246, G, 246}; hi = {265, G + 24, 265}; return;
    case kLabSlosh:  lo = {230, G, 249}; hi = {281, G + 15, 262}; return;
  }
  lo = {0, 0, 0};
  hi = {0, 0, 0};
}

// ---- --fluid-bench ---------------------------------------------------------

int RunFluidBench(GpuContext& ctx, World& world, Simulation& sim,
                  const std::vector<MaterialDef>& mats,
                  const std::string& sceneArg, const std::string& jsonPath) {
  struct BenchRun {
    int scene;
    int excite;         // sim.fluidExciteMode for the run
    std::string tag;    // scene name, or "hill0" for the excite-0 A/B
  };
  std::vector<BenchRun> runs;
  if (sceneArg.empty() || sceneArg == "all") {
    for (int s = 0; s < kLabSceneCount; s++) {
      runs.push_back({s, 1, LabSceneName(s)});
      // The hill A/B at exciteMode 0 reproduces the reported mid-slope
      // trapdoor until WP3 closes it (plan §4.2).
      if (s == kLabHill) runs.push_back({s, 0, "hill0"});
    }
  } else if (sceneArg == "hill0") {
    runs.push_back({kLabHill, 0, "hill0"});
  } else {
    int s = LabSceneFromName(sceneArg);
    if (s < 0) {
      std::fprintf(stderr,
                   "--fluid-bench: unknown scene '%s' (want basin|hill|hill0|"
                   "faucet|pool|slosh|all)\n",
                   sceneArg.c_str());
      return 1;
    }
    runs.push_back({s, 1, sceneArg});
  }

  uint32_t waterId = 0;
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) {
    std::fprintf(stderr, "--fluid-bench: no 'water' material\n");
    return 1;
  }
  const uint32_t splashMats[4] = {waterId, 0, 0, 0};

  SetHarnessSnapshotDrain(true);  // headless tick loop: see test/support.h

  PassTimer timer;
  const bool haveTimer = timer.Init(ctx, 128);
  if (!haveTimer)
    std::printf("--fluid-bench: TimestampQuery unavailable — per-pass GPU "
                "times will be zero (wall-clock rows still valid)\n");

  // Offscreen 1080p target, once for all runs. Render cost is measured as
  // WaitIdle-bracketed wall time (the render pass has no query hooks); with
  // the queue drained before each render that is GPU time + submit overhead.
  const uint32_t W = 1920, H = 1080;
  rhi::Texture offscreen = ctx.device.CreateTexture(
      {W, H, 1}, rhi::TextureFormat::RGBA8Unorm,
      rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
      "labBench");
  rhi::TextureView view = offscreen.CreateView();

  const Tuning savedTuning = CurrentTuning();
  const uint32_t skyTick =
      (uint32_t)(0.30 * (double)TicksPerDay(savedTuning));  // fixed daylight

  std::string outPath = jsonPath.empty() ? "fluid_bench.json" : jsonPath;
  std::ostringstream json;
  json << "[\n";

  for (size_t ri = 0; ri < runs.size(); ri++) {
    const BenchRun& run = runs[ri];
    const int scene = run.scene;
    const uint32_t N = LabSceneBenchTicks(scene);
    const uint32_t pourEnd = LabScenePourEnd(scene);

    // Per-run tuning: the lab exercises the full excite/settle loop
    // (exciteMode is the only live CPU-read fluid knob), and the day phase is
    // frozen with both water sinks off — the fluid-excite gate's pinning — so
    // the mass ledger cannot be drained by the evaporation reactions while
    // the bench watches it. Everything else stays at whatever tuning.json
    // says: measuring the live configuration is the point.
    {
      Tuning t = savedTuning;
      t.sim.fluidExciteMode = run.excite;
      t.dayNight.freeze = 1;
      t.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
      SetCurrentTuning(t);
    }

    sim.SetPassTimer(nullptr);  // a timed worldgen would dangle its queries
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    if (haveTimer) {
      timer.ResetStats();
      sim.SetPassTimer(&timer);
    }

    Vec3 eye;
    float yaw = 0, pitch = 0;
    LabSceneCamera(scene, eye, yaw, pitch);
    Camera cam;
    cam.yaw = yaw;
    cam.pitch = pitch;
    const IVec3 pc{CX / (int)kChunk, G / (int)kChunk, CZ / (int)kChunk};

    // Per-tick series.
    std::vector<double> frameMs, renderMs;
    std::map<std::string, uint64_t> prevNs;
    std::map<std::string, std::vector<double>> passMs;
    std::vector<uint32_t> liveCurve, blockCurve;
    uint64_t poured = 0, settledSum = 0, excitedSum = 0, deadSum = 0,
             binnedSum = 0, consumedSum = 0, emittedSum = 0, refusedSum = 0;
    int tickOfSettle = -1;
    uint32_t liveEst = 0;
    uint32_t tick = 0;

    for (uint32_t st = 1; st <= N; st++) {
      const double f0 = NowSeconds();
      std::vector<CellOp> cops;
      LabSceneBuildOps(scene, st, waterId, cops);
      std::vector<FluidSpawnOp> fs;
      LabScenePour(scene, st, liveEst, waterId, fs);
      SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, cops, false,
                 pc, false, liveEst + fs.size() > 0, {}, 0, fs, liveEst,
                 splashMats);
      poured += fs.size();
      ctx.WaitIdle();
      ctx.ProcessEvents();
      if (haveTimer) {
        timer.Collect(ctx);
        for (const PassTimer::Stat& s : timer.Stats()) {
          const uint64_t d = s.totalNs - prevNs[s.name];
          prevNs[s.name] = s.totalNs;
          passMs[s.name].push_back((double)d * 1e-6);
        }
      }
      uint32_t fa[16] = {};
      rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                            64, "benchArgs");
      liveEst = std::min(fa[7], kFluidCap);  // exact after the WaitIdle
      liveCurve.push_back(liveEst);
      blockCurve.push_back(fa[3]);
      settledSum += fa[10];
      excitedSum += fa[11];
      deadSum += fa[8];
      binnedSum += fa[15];
      consumedSum += fa[16];
      emittedSum += fa[9];
      refusedSum += fa[12];
      if (tickOfSettle < 0 && pourEnd != ~0u && st > pourEnd + 2 &&
          fa[7] == 0 && fa[3] == 0)
        tickOfSettle = (int)st;

      const double r0 = NowSeconds();
      WriteRenderParams(ctx.queue, world, eye, cam, (float)W / H, true, 11.7f,
                        kFarFogDensity, (float)H, skyTick, liveEst);
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      rhi::RenderPass rp =
          sim.BeginRenderPass(enc, view, rhi::TextureFormat::RGBA8Unorm, W, H);
      sim.DrawWorld(rp);
      sim.DrawParticles(rp);
      if (CurrentTuning().render.fluidSurface < 0.5f)
        sim.DrawFluid(rp, liveEst);
      rp.End();
      ctx.queue.Submit(enc.Finish());
      ctx.WaitIdle();
      const double r1 = NowSeconds();
      renderMs.push_back((r1 - r0) * 1000.0);
      frameMs.push_back((r1 - f0) * 1000.0);
    }
    sim.SetPassTimer(nullptr);

    // ---- end state: the mass ledger (eighths in == out) --------------------
    // In: every spawned particle carries one eighth (8 per cell at rest
    // density — the fluid-settle gate's equivalence). Out: live particles'
    // carried fullness + standing water eighths inside the scene bounds.
    uint64_t carriedEighths = 0;
    if (liveEst > 0) {
      std::vector<uint32_t> pbuf((size_t)liveEst * kFluidParticleWords);
      rhi::ReadbackBlocking(ctx.device, ctx.queue,
                            world.fluidParticles[sim.Page()], 0, pbuf.data(),
                            pbuf.size() * 4, "benchEndP");
      for (uint32_t k = 0; k < liveEst; k++)
        carriedEighths += (pbuf[k * kFluidParticleWords + 18] >> 12) & 0x7u;
    }
    uint64_t standingEighths = 0;
    {
      IVec3 lo, hi;
      LabSceneBounds(scene, lo, hi);
      std::vector<uint32_t> cbuf((size_t)kChunkVol);
      for (int cy = lo.y / 16; cy <= hi.y / 16; cy++)
        for (int cz = lo.z / 16; cz <= hi.z / 16; cz++)
          for (int cx = lo.x / 16; cx <= hi.x / 16; cx++) {
            ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx, cy, cz}), 1,
                           cbuf.data(), "benchVox");
            for (uint32_t i = 0; i < kChunkVol; i++) {
              const int x = (int)(i % 16) + cx * 16,
                        y = (int)((i / 16) % 16) + cy * 16,
                        z = (int)(i / 256) + cz * 16;
              if (x < lo.x || x > hi.x || y < lo.y || y > hi.y || z < lo.z ||
                  z > hi.z)
                continue;
              if ((cbuf[i] & 0xFFFu) == waterId)
                standingEighths += ((cbuf[i] >> 12) & 0xFu) + 1u;
            }
          }
    }
    const bool massExact = standingEighths + carriedEighths == poured;

    // ---- screenshot (look acceptance is judged on these, plan §9) ----------
    {
      rhi::Buffer shot = CreateBuffer(
          ctx.device, (uint64_t)W * H * 4,
          rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "labShot");
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      rhi::TexelCopyTexture srcT{};
      srcT.texture = offscreen;
      rhi::TexelCopyBuffer dstB{};
      dstB.buffer = shot;
      dstB.bytesPerRow = W * 4;
      dstB.rowsPerImage = H;
      enc.CopyTextureToBuffer(srcT, dstB, {W, H, 1});
      ctx.queue.Submit(enc.Finish());
      std::vector<uint8_t> pixels((size_t)W * H * 4);
      const std::string bmp = "screenshot_lab_" + run.tag + ".bmp";
      if (rhi::ReadBufferBlocking(ctx.device, shot, 0, pixels.data(),
                                  pixels.size()) &&
          WriteBmpFile(bmp, pixels, W, H))
        std::printf("wrote %s\n", bmp.c_str());
    }

    // ---- report ------------------------------------------------------------
    auto avg = [](const std::vector<double>& v) {
      double s = 0;
      for (double d : v) s += d;
      return v.empty() ? 0.0 : s / (double)v.size();
    };
    std::printf(
        "fluid-bench %-7s excite %d: %u ticks | frame ms p50 %.2f p95 %.2f "
        "p99 %.2f | render ms avg %.2f | live max %u end %u | settle t%d | "
        "mass %s (%llu poured = %llu standing + %llu carried)\n",
        run.tag.c_str(), run.excite, N, Pct(frameMs, 0.50), Pct(frameMs, 0.95),
        Pct(frameMs, 0.99), avg(renderMs),
        *std::max_element(liveCurve.begin(), liveCurve.end()), liveEst,
        tickOfSettle, massExact ? "EXACT" : "LEAK",
        (unsigned long long)poured, (unsigned long long)standingEighths,
        (unsigned long long)carriedEighths);
    for (auto& [name, v] : passMs)
      if (avg(v) > 0.0005)
        std::printf("    %-24s avg %7.3f ms  p95 %7.3f ms\n", name.c_str(),
                    avg(v), Pct(v, 0.95));

    json << "  {\n    \"scene\": \"" << run.tag << "\",\n"
         << "    \"exciteMode\": " << run.excite << ",\n"
         << "    \"ticks\": " << N << ",\n"
         << "    \"tuning\": {\"stiffness\": " << savedTuning.sim.fluidStiffness
         << ", \"gravity\": " << savedTuning.sim.fluidGravity
         << ", \"eosPower\": " << savedTuning.sim.fluidEosPower
         << ", \"cohesion\": " << savedTuning.sim.fluidCohesion
         << ", \"attractSame\": " << savedTuning.sim.fluidAttractSame
         << ", \"attractDiff\": " << savedTuning.sim.fluidAttractDiff
         << ", \"viscosity\": " << savedTuning.sim.fluidViscosity
         << ", \"damping\": " << savedTuning.sim.fluidDamping << "},\n";
    json << "    \"frameMs\": {\"p50\": " << Pct(frameMs, 0.50)
         << ", \"p95\": " << Pct(frameMs, 0.95)
         << ", \"p99\": " << Pct(frameMs, 0.99) << "},\n";
    json << "    \"renderMsWall\": {\"avg\": " << avg(renderMs)
         << ", \"p95\": " << Pct(renderMs, 0.95) << "},\n";
    json << "    \"passesMs\": {";
    bool first = true;
    for (auto& [name, v] : passMs) {
      if (!first) json << ", ";
      first = false;
      json << "\"" << name << "\": {\"avg\": " << avg(v)
           << ", \"p95\": " << Pct(v, 0.95) << "}";
    }
    json << "},\n";
    json << "    \"tickOfSettle\": " << tickOfSettle << ",\n";
    json << "    \"massLedger\": {\"pouredEighths\": " << poured
         << ", \"standingEighths\": " << standingEighths
         << ", \"carriedEighths\": " << carriedEighths
         << ", \"exact\": " << (massExact ? "true" : "false")
         << ", \"settledFlow\": " << settledSum
         << ", \"excitedFlow\": " << excitedSum
         << ", \"deadParticles\": " << deadSum
         << ", \"binned\": " << binnedSum << ", \"consumed\": " << consumedSum
         << ", \"emittedDroplets\": " << emittedSum
         << ", \"exciteRefused\": " << refusedSum << "},\n";
    json << "    \"liveMax\": "
         << *std::max_element(liveCurve.begin(), liveCurve.end())
         << ", \"liveEnd\": " << liveEst << ",\n";
    json << "    \"liveCurve\": [";
    for (size_t i = 0; i < liveCurve.size(); i++)
      json << (i ? "," : "") << liveCurve[i];
    json << "],\n    \"blockCurve\": [";
    for (size_t i = 0; i < blockCurve.size(); i++)
      json << (i ? "," : "") << blockCurve[i];
    json << "]\n  }" << (ri + 1 < runs.size() ? "," : "") << "\n";
  }
  json << "]\n";
  SetCurrentTuning(savedTuning);

  std::ofstream out(outPath, std::ios::binary);
  out << json.str();
  out.close();
  std::printf("fluid-bench: wrote %s\n", outPath.c_str());
  return ctx.ReportVkValidation("--fluid-bench") > 0 ? 1 : 0;
}

// ---- live-tuning file plumbing ---------------------------------------------

int64_t LabFileMtimeNs(const std::string& path) {
  std::error_code ec;
  const auto t = std::filesystem::last_write_time(path, ec);
  if (ec) return -1;
  return (int64_t)t.time_since_epoch().count();
}

bool LabPatchTuningJson(const std::string& path, const Tuning& t,
                        int64_t* lastLoadedMtimeNs) {
  // Never overwrite a file newer than the one this process last loaded: that
  // is the tuner's edit, and the watcher is about to apply it. Last-writer-
  // wins, decided by mtime (lab.h block comment).
  const int64_t diskMtime = LabFileMtimeNs(path);
  if (diskMtime < 0) return false;
  if (lastLoadedMtimeNs && diskMtime > *lastLoadedMtimeNs) {
    std::printf("lab: tuning.json is newer on disk than in memory — ImGui "
                "edit NOT written (the watcher will load the file's values)\n");
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  in.close();

  // Patch the value text of one "key": <number> pair in place. Text surgery
  // rather than a re-serialize, so the tuner's formatting and every key this
  // panel does not own survive byte-for-byte.
  auto patch = [&text](const char* key, double v, bool isInt) -> bool {
    const std::string needle = std::string("\"") + key + "\"";
    size_t k = text.find(needle);
    if (k == std::string::npos) return false;
    size_t colon = text.find(':', k + needle.size());
    if (colon == std::string::npos) return false;
    size_t vs = colon + 1;
    while (vs < text.size() && (text[vs] == ' ' || text[vs] == '\t')) vs++;
    size_t ve = vs;
    while (ve < text.size() &&
           (std::isdigit((unsigned char)text[ve]) || text[ve] == '-' ||
            text[ve] == '+' || text[ve] == '.' || text[ve] == 'e' ||
            text[ve] == 'E'))
      ve++;
    if (ve == vs) return false;
    char buf[48];
    if (isInt)
      std::snprintf(buf, sizeof buf, "%lld", (long long)llround(v));
    else
      std::snprintf(buf, sizeof buf, "%g", v);
    text.replace(vs, ve - vs, buf);
    return true;
  };
  bool ok = true;
  ok &= patch("fluidGravity", t.sim.fluidGravity, false);
  ok &= patch("fluidStiffness", t.sim.fluidStiffness, false);
  ok &= patch("fluidEosPower", (double)t.sim.fluidEosPower, true);
  ok &= patch("fluidCohesion", t.sim.fluidCohesion, false);
  ok &= patch("fluidAttractSame", t.sim.fluidAttractSame, false);
  ok &= patch("fluidAttractDiff", t.sim.fluidAttractDiff, false);
  ok &= patch("fluidViscosity", t.sim.fluidViscosity, false);
  ok &= patch("fluidDamping", t.sim.fluidDamping, false);
  if (!ok) {
    std::fprintf(stderr, "lab: tuning.json fluid keys not found — file NOT "
                         "written\n");
    return false;
  }
  std::ofstream outF(path, std::ios::binary | std::ios::trunc);
  if (!outF) return false;
  outF << text;
  outF.close();
  if (lastLoadedMtimeNs) *lastLoadedMtimeNs = LabFileMtimeNs(path);
  return true;
}

}  // namespace sandvox
