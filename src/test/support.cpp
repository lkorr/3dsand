// support.cpp — shared sim/render plumbing. Moved verbatim out of main.cpp's
// anonymous namespace so the selftest could leave main.cpp without cloning it.
// See support.h for why one definition matters here.

#include "test/support.h"

#include "sim/pagetable.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gpu/resources.h"
#include "sim/farfield.h"

namespace sandvox {

const char* kAvatarDefName = "mina";

// Time of day used by --shot, as a 0..1 fraction of the cycle (0 = midnight,
// 0.5 = noon). Set by `--time`; see RunShots.
float g_shotTimeOfDay = 0.34f;

std::string AssetDir() {
#ifdef SANDVOX_ASSET_DIR
  return SANDVOX_ASSET_DIR;
#else
  return "assets";
#endif
}

double NowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

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

void WriteRenderParams(const rhi::Queue& queue, const World& world,
                       const Vec3& eye, const Camera& cam, float aspect,
                       bool shadows, float time,
                       float fogDensity, float viewPx, uint32_t tick) {
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
                const std::vector<ParticleSpawn>& spawns,
                uint32_t farCount) {
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

  // ---- the page table: MATERIALIZE BEFORE THE ENCODER (§3) ----------------
  //
  // A GPU kernel cannot allocate, so every page a kernel might write must
  // exist before the command buffer is submitted. This is the whole reason
  // the phase has a CPU-side conservative dirty mirror: the question
  // "at encode time, what is the set of chunks this tick could write?" is
  // CPU-derivable, and the GPU has no monopoly on it.
  //
  // The order below is the §3.2 normative definitions, in their stated order:
  //   BeginTick        -> clears this tick's C(N) contributors
  //   AddOp*           -> contributor (a), opTargets(N)
  //   UpdateParticles  -> contributor (b), particleChunks(N)
  //   TightenFromSnapshot -> step (2), INTERSECTION only, never assignment
  //   WakeAll/Refilled -> step (3), strictly AFTER the tightening
  //   Materialize      -> step (1) propagate, then step (4) allocate + fill
  //
  // EncodeWakeAll above has already unioned all-ones into the mirror (§3.2a
  // fix 1: the wake IS a dirty-set mutation and the two must be ONE operation,
  // not two that must agree).
  PageTable& pt = *world.pages;
  pt.BeginTick(tick);
  for (const BrushOp& o : ops)
    pt.AddOpSphere({o.x, o.y, o.z}, o.radius, world);
  for (const ExplosionOp& e : exps)
    pt.AddOpBox({e.x, e.y, e.z}, kMaxExplosionRadius, world);  // EXP_BOX
  for (uint32_t i = 0; i < cellCount; i++)
    pt.AddOpTarget(cells[i].cellIdx / kChunkVol);  // already a slot chunk index
  {
    std::vector<IVec3> spawnCells, expCenters;
    spawnCells.reserve(spawnCount);
    for (uint32_t i = 0; i < spawnCount; i++)
      spawnCells.push_back({spawns[i].px >> 8, spawns[i].py >> 8,
                            spawns[i].pz >> 8});
    for (const ExplosionOp& e : exps) expCenters.push_back({e.x, e.y, e.z});
    const WorldSnapshot& sn = world.Snap();
    pt.UpdateParticles(particlesActive, sn.valid ? sn.particleCount : 0,
                       spawnCells, expCenters, world);
  }
  {
    const WorldSnapshot& sn = world.Snap();
    if (sn.valid) pt.TightenFromSnapshot(sn.dirtyFlags, sn.tick, tick);
  }
  pt.Materialize(ctx.queue);

  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  // The fills go in at the HEAD of the command buffer, before any row (§5.4):
  // FillTracked declares TransferWrite on Voxels, and the first row with
  // RW(Voxels) then gets a derived TRANSFER->COMPUTE barrier. A fill recorded
  // after a dispatch that reads the page is exactly the hazard this ordering
  // exists to prevent.
  pt.DrainFills(enc);
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
  ctx.queue.Submit(enc.Finish());
  sim.FlipPage();
  if (doCopy) {
    world.KickReadback();
    // Harness only, off by default (see SetHarnessSnapshotDrain in support.h).
    // Wait for the submit's fence, then pump — which is what a game frame does
    // for free by having real time elapse between the two. The game's frame
    // loop shares SubmitTick and must never take this path.
    if (HarnessSnapshotDrain()) {
      ctx.WaitIdle();
      ctx.ProcessEvents();
    }
  }
}

namespace {
// Off by default: main.cpp's frame loop shares SubmitTick and must never pay
// this sync point. Only --selftest / --vk-smoke / --measure opt in.
bool g_harnessSnapshotDrain = false;
}  // namespace

void SetHarnessSnapshotDrain(bool on) { g_harnessSnapshotDrain = on; }
bool HarnessSnapshotDrain() { return g_harnessSnapshotDrain; }

void SubmitWorldgen(GpuContext& ctx, World& world, Simulation& sim, uint32_t seed) {
  TickParams tp{0, seed, 0, 0};
  IVec3 wo = world.WindowOrigin();
  tp.origin[0] = wo.x; tp.origin[1] = wo.y; tp.origin[2] = wo.z;
  ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
  if (world.residency != World::Residency::Paged) {
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeWorldgen(enc);
    ctx.queue.Submit(enc.Finish());
    return;
  }

  // ---- BATCHED worldgen (PLAN_page_table.md §3.5c, §9 open question 1) ----
  //
  // genChunk writes all 4,096 cells of every target slot and does NOT know in
  // advance which it will fill with air, so every slot it touches must have a
  // page before the dispatch — a kernel cannot allocate. Doing all 32,768 at
  // once would need a dense pool (512 MiB), i.e. no saving at all at the
  // moment of worldgen, and under §3.8's fatal policy it would simply ABORT at
  // startup with kPoolPages = 8192.
  //
  // So: materialize a batch, dispatch it through `worldgenList` (which already
  // takes a slot list — the primitive was there), read its occupancy, demote
  // the empties, and let those pages come back for the next batch. The
  // transient is bounded by the batch size and it costs 32768/2048 = 16
  // submits at startup, which is nothing off the frame path. It also
  // generalizes to a grown window, where a dense transient would be 4 GiB and
  // simply impossible.
  {
    const uint32_t kGenBatch = 2048;
    // The first submit still has to clear the transient buffers (hash,
    // support, particle counts, both dirty pages) exactly as EncodeWorldgen
    // does, so run it over an EMPTY slot list: the fills land, the dispatch
    // covers zero slots.
    world.pages->ResetAllEmpty(ctx.queue);
    {
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      sim.EncodeWorldgen(enc, /*denseGen=*/false);
      ctx.queue.Submit(enc.Finish());
    }
    std::vector<uint32_t> batch;
    std::vector<uint32_t> vox((size_t)kGenBatch * kChunkVol);
    batch.reserve(kGenBatch);
    for (uint32_t base = 0; base < kNumChunks; base += kGenBatch) {
      const uint32_t n = std::min(kGenBatch, kNumChunks - base);
      batch.clear();
      for (uint32_t k = 0; k < n; k++) {
        batch.push_back(base + k);
        world.pages->EnsurePageForOverwrite(base + k);
      }
      world.pages->FlushTableWrites(ctx.queue);
      ctx.queue.WriteBuffer(world.genList, 0, batch.data(), batch.size() * 4);
      TickParams gp{};
      gp.seed = seed;
      gp.genCount = (uint32_t)batch.size();
      gp.origin[0] = wo.x; gp.origin[1] = wo.y; gp.origin[2] = wo.z;
      ctx.queue.WriteBuffer(world.tickUBO, 0, &gp, sizeof(gp));
      rhi::CommandEncoder ge = ctx.device.CreateCommandEncoder();
      sim.EncodeGenList(ge, (uint32_t)batch.size());
      ctx.queue.Submit(ge.Finish());
      // Classify and demote, which returns the all-air pages to the free list
      // for the next batch. This is the compaction §3.5c calls for, run
      // eagerly once per batch rather than on the hysteresis cadence.
      ReadVoxelsSync(ctx, world, base, n, vox.data(), "wgClassify");
      for (uint32_t k = 0; k < n; k++) {
        const uint32_t e = PageTable::Classify(vox.data() + (size_t)k * kChunkVol);
        if (e != PageTable::kNeedsPage) world.pages->SetSentinel(base + k, e);
      }
      world.pages->FlushTableWrites(ctx.queue);
    }
    std::printf("worldgen (paged, %u-slot batches): %u pages in use "
                "(%.1f MiB of %.1f MiB pool), high water %u\n",
                kGenBatch, world.pages->PagesInUse(),
                (double)world.pages->PagesInUse() * kChunkVol * 4.0 / 1048576.0,
                (double)world.pages->PoolPages() * kChunkVol * 4.0 / 1048576.0,
                world.pages->PagesHighWater());
    return;
  }
}

// (Body render plumbing lives in game/bodyreg.h — see the note in support.h.)

bool WriteBmpFile(const std::string& path, const std::vector<uint8_t>& rgba,
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
  uint32_t result = 0;
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.hash, 0, &result, 4, "hashRead");
  return result;
}

uint32_t HashWorldNow(GpuContext& ctx, World& world, Simulation& sim, uint32_t seed) {
  TickParams tp{0, seed, 0, 1, 0, 0, 0, 0};
  ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  sim.EncodeHashOnly(enc);
  ctx.queue.Submit(enc.Finish());
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
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.particleCounts, 0, out, 8,
                        "countsRead");
}

uint32_t ReadActiveChunksSync(GpuContext& ctx, World& world, Simulation& sim) {
  std::vector<uint32_t> flags(kNumChunks, 0);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0, flags.data(),
                        kNumChunks * 4, "activeRead");
  uint32_t n = 0;
  for (uint32_t i = 0; i < kNumChunks; i++)
    if (flags[i] != 0) n++;
  return n;
}

void ReadVoxelsSync(GpuContext& ctx, World& world, uint32_t firstSlot,
                    uint32_t count, uint32_t* out, const char* label) {
  // One copy per RUN of consecutive resident slots whose pages are also
  // consecutive, so the dense case (the identity map) is still exactly one
  // copy of the whole range — which is what it was before paging.
  uint32_t i = 0;
  while (i < count) {
    const uint64_t off = world.PageOffsetOfSlot(firstSlot + i);
    if (off == World::kNoPage) {
      // Sentinel: synthesize, through the same rule the shader uses.
      const uint32_t w = SynthWord(world.PageEntryOfSlot(firstSlot + i));
      uint32_t* dst = out + (size_t)i * kChunkVol;
      for (uint32_t k = 0; k < kChunkVol; k++) dst[k] = w;
      i++;
      continue;
    }
    uint32_t run = 1;
    while (i + run < count) {
      const uint64_t nxt = world.PageOffsetOfSlot(firstSlot + i + run);
      if (nxt != off + (uint64_t)run * kChunkVol * 4) break;
      run++;
    }
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.voxels, off,
                          out + (size_t)i * kChunkVol,
                          (size_t)run * kChunkVol * 4, label);
    i += run;
  }
}

}  // namespace sandvox
