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
                       float fogDensity, float viewPx, uint32_t tick,
                       uint32_t fluidCount) {
  RenderParams rp{};
  rp.fluidCount = fluidCount;  // 0 skips the MPM fluid surface march entirely
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
                uint32_t farCount,
                const std::vector<FluidSpawnOp>& fluidSpawns,
                uint32_t fluidBase,
                const uint32_t* fluidSplashMat) {
  particlesActive = particlesActive || !exps.empty() || !spawns.empty();
  uint32_t cellCount = std::min((uint32_t)cells.size(), kMaxCellOpsPerTick);
  uint32_t spawnCount = std::min((uint32_t)spawns.size(), kMaxParticleSpawnsPerTick);
  // MLS-MPM fluid prototype: budget is charged by the CALLER before emitting
  // (rule 2 — emit-then-check overruns); this clamp is the belt to that brace.
  uint32_t fluidSpawnCount = std::min((uint32_t)fluidSpawns.size(),
                                      kMaxFluidSpawnsPerTick);
  fluidBase = std::min(fluidBase, kFluidCap);
  if (fluidSpawnCount > kFluidCap - fluidBase)
    fluidSpawnCount = kFluidCap - fluidBase;
  TickParams tp{tick, seed, (uint32_t)ops.size(), hashEnable ? 1u : 0u,
                (uint32_t)exps.size(), sim.Page(), cellCount, 0};
  tp.spawnCount = spawnCount;
  tp.farCount = farCount;  // far-field fills ride the tick submit (render-only)
  tp.fluidBase = fluidBase;
  tp.fluidSpawnCount = fluidSpawnCount;
  if (fluidSplashMat) {
    for (int i = 0; i < 4; i++) tp.fluidSplashMat[i] = fluidSplashMat[i];
  }
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
  if (fluidSpawnCount > 0)
    ctx.queue.WriteBuffer(world.fluidSpawnOps, 0, fluidSpawns.data(),
                          fluidSpawnCount * sizeof(FluidSpawnOp));
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
  //   WakeAll/Refilled/ParticleShell -> step (3), strictly AFTER the tightening
  //   Materialize      -> step (1) propagate, then step (4) allocate + fill
  //
  // EncodeWakeAll above has already unioned all-ones into the mirror (§3.2a
  // fix 1: the wake IS a dirty-set mutation and the two must be ONE operation,
  // not two that must agree).
  PageTable& pt = *world.pages;
  // Install the free-confirmation probe once (see SetChunkProbe): a page is
  // only released when the chunk's WORDS say empty, because occupancy does not
  // see the stain layer and the hash does.
  static bool probeInstalled = false;
  if (!probeInstalled) {
    probeInstalled = true;
    GpuContext* pctx = &ctx;
    World* pw = &world;
    pt.SetChunkProbe([pctx, pw](uint32_t slot, uint32_t* out) {
      const uint64_t off = pw->PageOffsetOfSlot(slot);
      if (off == World::kNoPage) return false;
      pctx->WaitIdle();
      return rhi::ReadbackBlocking(pctx->device, pctx->queue, pw->voxels, off,
                                   out, (size_t)kChunkVol * 4, "freeProbe");
    });
  }
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
    // particleSpawnChunks(N): THIS tick's spawn sites, one ring, recomputed
    // from scratch. Not carried — see the adjacency argument in
    // PageTable::UpdateSpawnRing.
    pt.UpdateSpawnRing(spawnCells, expCenters, world);
  }
  {
    const WorldSnapshot& sn = world.Snap();
    if (sn.valid) pt.TightenFromSnapshot(sn.dirtyFlags, sn.tick, tick);
    // Contributor (e), the particle flight shell — strictly AFTER the
    // tightening, like (c)/(d): a union applied after an intersection cannot
    // be undone by it. Covers the GPU-decided landing writes an airborne
    // particle will make (a mid-flight snapshot legitimately tightens the
    // mirror to empty — a flying particle dirties nothing — and the
    // intersection can never ADD the landing back). See §3.4.
    pt.ApplyParticleShell(sn, particlesActive);
  }
  pt.Materialize(ctx.queue);
  // ---- deallocation (§3.6), AFTER materialization -------------------------
  // Order matters: cpuDirty is the materialization set, and the free
  // condition's second conjunct tests against it. Running the free decision
  // before Materialize would test a mirror that had not yet absorbed this
  // tick's ops, and could free a chunk this very tick is about to write.
  //
  // Both steps read data the CPU already has: the occupancy the snapshot
  // already carries, and a tick counter. No new readback, no new scan.
  if (world.Snap().valid) pt.ConsumeOccupancy(world.Snap().occupancy, tick);
  pt.RetirePages(tick);

  // ---- the §3.4 settled-skip latch ----------------------------------------
  // Fed here because this is the one function BOTH the game loop and every
  // harness tick go through, so the latch cannot see a different world than
  // the encoder does. Declared BEFORE EncodeTick, which reads it.
  //
  // `dirtiedNow` deliberately over-declares: farCount is render-only derived
  // data that cannot dirty a sim chunk, but it costs nothing to be wrong in
  // the safe direction and the list stays a plain "did anything arrive".
  sim.NoteTickInputs(tick, !ops.empty() || !exps.empty() || cellCount > 0 ||
                               spawnCount > 0 || particlesActive ||
                               fluidBase + fluidSpawnCount > 0);
  {
    // A snapshot can only license a skip if it is BOTH valid and fresh enough
    // (Simulation::NoteSnapshot enforces the freshness against lastDirtyTick_).
    const WorldSnapshot& sn = world.Snap();
    if (sn.valid) sim.NoteSnapshot(sn.tick, sn.activeChunks);
  }

  // THE genList UPLOAD MUST HAPPEN BEFORE THE ENCODER EXISTS, and this is a
  // trap worth stating: rhi::Queue::WriteBuffer is a DEFERRED host write
  // (Backend::QueueWrite) that drains "at the head of the NEXT command buffer".
  // Called after CreateCommandEncoder, the write drains into the command buffer
  // AFTER this one, so the pageFill dispatch below read the PREVIOUS tick's
  // genList — every JITTER page materialized as zeros and 2,114 chunks of stone
  // silently became air.
  const uint32_t jitterFills = pt.UploadJitterFills(ctx.queue);

  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  // The fills go in at the HEAD of the command buffer, before any row (§5.4):
  // FillTracked declares TransferWrite on Voxels, and the first row with
  // RW(Voxels) then gets a derived TRANSFER->COMPUTE barrier. A fill recorded
  // after a dispatch that reads the page is exactly the hazard this ordering
  // exists to prevent.
  pt.DrainFills(enc);
  // The JITTER half of materialization, same position and same reason: a page
  // whose words vary per cell cannot be a fill pattern, so it is a dispatch.
  // Recorded BEFORE EncodeTick so the tick's first voxel read sees the filled
  // page (the recorder derives the COMPUTE->COMPUTE barrier from the W(Voxels)
  // in the pageFill row against the tick's first RW(Voxels)).
  sim.EncodePageFill(enc, jitterFills);
  sim.EncodeTick(enc, (uint32_t)ops.size(), hashEnable, (uint32_t)exps.size(),
                 particlesActive, cellCount, spawnCount,
                 fluidBase + fluidSpawnCount, fluidSpawnCount);
  sim.EncodeFarFill(enc, farCount);
  // PAGED RESIDENCY MAKES THE SNAPSHOT LOAD-BEARING, so the harness must ask
  // for one even when the caller did not. §3.2 step (2)'s intersection is the
  // ONLY thing that tightens cpuDirty; without a snapshot the mirror is only
  // ever the step-(1) recurrence, which dilates by a ring every tick and walks
  // the materialization set through the whole window in ~10 ticks — the pool
  // then exhausts and §3.8 aborts. Most selftest gates pass wantReadback=false
  // (they read the world through blocking hash/occupancy reads, not through
  // Snap()), so under --residency paged every gate hit that abort.
  //
  // PRE-EXISTING and independent of phase 7's two fixes: proven by stashing
  // them and rebuilding. The condition is narrow on purpose:
  //   - HarnessSnapshotDrain() — main.cpp's frame loop shares SubmitTick and
  //     already requests readbacks on its own schedule; it must not be touched.
  //   - Residency::Paged — under dense the page table is the identity map,
  //     nothing is ever materialized or freed, and cpuDirty is inert. Forcing
  //     a readback there would change dense timing for no reason, so the dense
  //     path stays byte-identical (verified: world hash 7cfa2420 unchanged).
  // A readback is a pure copy out plus a map — it mutates no world state — so
  // the only thing a gate can observe from this is Snap() becoming valid in
  // paged mode, which is exactly what it needs to be.
  // PAGED SELF-DEFENSE AGAINST SNAPSHOT STARVATION. §3.2's tightening is the
  // ONLY thing that shrinks cpuDirty; without a snapshot the mirror dilates a
  // ring per tick and the materialization set walks toward the whole window —
  // measured at game startup: the worldgen-sized dirty set went 8,379 →
  // 20,601 chunks over the four ticks the first frame runs before any
  // snapshot fence can retire, and hysteresis stacked the materialized pages
  // straight through the 16,384-page pool (§3.8 abort on the FIRST windowed
  // paged run). The same starvation kills every headless path that ticks
  // without readbacks (--shot settles worldgen over hundreds of such ticks).
  //
  // So paged mode enforces its own snapshot cadence: when no snapshot exists,
  // or the one in hand is older than the tick can tolerate, request the
  // readback and DRAIN it exactly like the harness does. Two thresholds:
  //   - the SETTLE WINDOW (PageTable::InSettleWindow — the first ticks after
  //     ANY world reset, anchored by the page table itself, because --shot's
  //     scenes re-worldgen at arbitrary tick values and an absolute-tick
  //     predicate missed every scene after the first) drains per-tick
  //     (gap 0): the dirty set is at its lifetime maximum, and even a 2-4
  //     tick lag — each stale snapshot dilated one ring per tick of lag —
  //     measured 16,347 pages in use by tick 8, vs 9,396 peak for the same
  //     settle under the harness's strict per-tick drain;
  //   - after it, kPagedSnapshotMaxGap: the settled world's dirty set is
  //     small, a few rings of it are cheap, and in steady play the game's own
  //     readbacks keep the snapshot 1-3 ticks fresh so this never triggers —
  //     it exists for hitches and for headless tick loops.
  // Shot paths WaitIdle every tick anyway, so the added pump costs nothing
  // there. Dense mode takes none of this — the identity map has no mirror to
  // starve.
  constexpr uint32_t kPagedSnapshotMaxGap = 4;
  const bool paged = world.residency == World::Residency::Paged;
  const uint32_t maxGap =
      world.pages->InSettleWindow(tick) ? 0u : kPagedSnapshotMaxGap;
  const bool snapshotStale =
      paged &&
      (!world.Snap().valid || (tick > world.Snap().tick + maxGap));
  const bool needSnapshotForPaging =
      paged && (HarnessSnapshotDrain() || snapshotStale);
  bool doCopy = false;
  if (wantReadback || needSnapshotForPaging) {
    doCopy = world.EncodeReadbacks(ctx.device, enc,
                                   {playerChunk.x - 1, playerChunk.y - 1, playerChunk.z - 1},
                                   1 - sim.Page(), tick);
    if (doCopy) world.EncodeDirtyCopy(enc, sim.DirtyNext());
  }
  ctx.queue.Submit(enc.Finish());
  sim.FlipPage();
  if (doCopy) world.KickReadback();
  // Wait for the submit's fence, then pump — which is what a game frame does
  // for free by having real time elapse between the two. Taken by the harness
  // drain (SetHarnessSnapshotDrain, off by default) and by paged mode's
  // staleness fallback above; the game's frame loop only pays it in the
  // startup/hitch cases the fallback exists for.
  //
  // The pump runs EVEN WHEN NO COPY WAS ENCODED, and that is load-bearing:
  // EncodeReadbacks DECLINES while all three ring slots are in flight, and in
  // a headless tick loop only this pump ever retires them. Gating the pump on
  // doCopy deadlocks the whole cadence — request declined, so no pump, so the
  // slots never retire, so every later request is declined too — measured on
  // --shot's oil-slick scene as a snapshot-starved mirror dilating a ring per
  // tick straight through the pool (materialize allocations 35 → 3,447/tick
  // over 13 ticks, no tighten ever running).
  if (HarnessSnapshotDrain() || snapshotStale) {
    ctx.WaitIdle();
    ctx.ProcessEvents();
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
  // The held snapshot describes the OLD world; scenes and gates also restart
  // their tick counters, which can make its stamp read as newer than the new
  // world's early ticks. See World::InvalidateSnapshot. Measured on --shot's
  // oil-slick scene: the stale stamp suppressed every tightening AND the
  // staleness fallback at once, and the unsnapshotted mirror dilated a ring
  // per tick through the pool.
  world.InvalidateSnapshot();
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
        const uint32_t e =
            world.pages->Classify(base + k, vox.data() + (size_t)k * kChunkVol);
        if (e != PageTable::kNeedsPage) world.pages->SetSentinel(base + k, e);
      }
      world.pages->FlushTableWrites(ctx.queue);
    }
    // ZERO THE FAULT COUNTER AFTER WORLDGEN, and only here.
    //
    // Worldgen is the one writer that legitimately stores through sentinels:
    // genChunk writes all 4,096 cells of every slot in its batch, and the
    // batches it does NOT currently hold are PT_EMPTY by construction (that is
    // what makes a 8,192-page pool able to generate 32,768 slots at all). Those
    // stores no-op and count, so the counter reaches exactly
    // kNumChunks * kChunkVol = 134,217,728 before tick 1 — in a run that is
    // otherwise perfectly correct. Measured identically on quiet-paged, which
    // is 5/5 MATCH, so it is a startup artifact and not a lost voxel.
    //
    // The invariant the gates actually assert is about the TICK LOOP: no sim
    // kernel may write through a sentinel. Zeroing here is what makes the
    // counter mean that, and it is why the dense run (identity map, nothing to
    // fault on) reads 0 both before and after this line.
    const uint32_t faultZero[4] = {0u, 0u, 0u, 0u};
    ctx.queue.WriteBuffer(world.pageFaults, 0, faultZero, sizeof(faultZero));
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
  // THE ORIGIN IS LOAD-BEARING HERE, and it was not before the JITTER sentinel
  // existed. This standalone rehash builds a fresh TickParams, and `origin`
  // defaults to {0,0,0} — harmless while nothing in the hash path used it.
  // sim_occupancy's analytic sentinel branch now resolves a JITTER chunk's
  // WORLD position from (slot, origin) to synthesize its palette variants, so a
  // zero origin after a window shift hashes every jittered chunk at the wrong
  // coordinates. Symptom: --vk-smoke-loud diverged at ticks 86/88 — the first
  // two shifts — with the chunk CONTENTS provably identical (a per-chunk digest
  // diff showed zero differing slots), because only the hash was wrong.
  const IVec3 wo = world.WindowOrigin();
  tp.origin[0] = wo.x; tp.origin[1] = wo.y; tp.origin[2] = wo.z;
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
      // POSITIONAL: SynthWordAt collapses to SynthWord for EMPTY/UNIFORM, so
      // one loop serves every sentinel form. A JITTER chunk read back through
      // here (the worldgen compaction classifier does exactly that) must see
      // the same words the GPU would, or classification would refuse chunks it
      // had itself just promoted.
      const uint32_t e = world.PageEntryOfSlot(firstSlot + i);
      const IVec3 wc = world.SlotToWorldChunk(firstSlot + i);
      const int bx = wc.x * (int)kChunk, by = wc.y * (int)kChunk,
                bz = wc.z * (int)kChunk;
      uint32_t* dst = out + (size_t)i * kChunkVol;
      for (uint32_t k = 0; k < kChunkVol; k++)
        dst[k] = SynthWordAt(e, bx + (int)(k % kChunk),
                             by + (int)((k / kChunk) % kChunk),
                             bz + (int)(k / (kChunk * kChunk)),
                             world.pages->WorldSeed());
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
