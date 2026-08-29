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
#include <cstdlib>

#include "gpu/resources.h"
#include "sim/farfield.h"
#include "sim/wind.h"
#include "sim/worldedit.h"
#include "sim/waterbody.h"
#include "sim/windprim.h"

namespace sandvox {

// Must track tuning.json player.model — the avatar gates exist to test the
// character the GAME plays, and one pinned to the old name goes on passing
// against a character nobody plays (see the note at the avatar gate itself).
const char* kAvatarDefName = "human";

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

// Ticks per in-game SOLAR day, from the tuning cycle length. Sim runs at 30 Hz.
// The definition itself lives in tuning.h so sim/celestial.cpp can use it
// without linking the render plumbing; this is the name the frame loop and the
// gates already call.
uint32_t TicksPerDay(const Tuning& t) { return TicksPerDayFromTuning(t); }

uint32_t DayPhaseNow(uint32_t tick) {
  const Tuning& t = CurrentTuning();
  return DayPhaseForTick(Celestial().SimTick(tick), TicksPerDay(t),
                         t.dayNight.freeze != 0,
                         (uint32_t)t.dayNight.freezePhase);
}

// The sky for a given sim tick — now a full Keplerian solve (sim/celestial.*)
// rather than a phase ramp. `tick` is routed through the celestial clock, which
// is DISENGAGED unless the dev overlay's time-speed slider has been moved: on
// every headless path this is the identity map and the celestial tick IS the
// sim tick, so the pinned world hash cannot move.
SkyState SkyForTick(const Tuning& t, uint32_t tick) {
  return ComputeSky(t, Celestial().RenderTick(tick));
}

void WriteRenderParams(const rhi::Queue& queue, const World& world,
                       const Vec3& eye, const Camera& cam, float aspect,
                       bool shadows, float time,
                       float fogDensity, float viewPx, uint32_t tick,
                       uint32_t fluidCount, float frameFrac,
                       uint32_t extraFlags) {
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
  rp.flags = (shadows ? 1u : 0u) | extraFlags;
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
  SkyState sky = ComputeSky(tun, Celestial().RenderTickInterp(tick, (double)frameFrac));
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
  // Moon B + eclipse geometry. All of it falls out of the orbital solve — the
  // renderer is told where the bodies ARE and how big they look, and draws
  // them; it decides nothing about the sky's state.
  rp.moon2Dir[0] = sky.moon2Dir[0];
  rp.moon2Dir[1] = sky.moon2Dir[1];
  rp.moon2Dir[2] = sky.moon2Dir[2];
  rp.moon2Phase = sky.moon2Phase;
  rp.moonAngRadius = sky.moonAngRadius;
  rp.moon2AngRadius = sky.moon2AngRadius;
  rp.moonPhaseSign = sky.moonPhaseSign;
  rp.moon2PhaseSign = sky.moon2PhaseSign;
  rp.solarEclipse = sky.solarEclipse;
  rp.lunarEclipse = sky.lunarEclipse;
  rp.eclipseBody = sky.eclipseBody;
  // The axis the starfield wheels about — derived from latitude, so the stars
  // turn about the same pole the sun arcs around. raymarch.wgsl reads it in
  // BOTH starField() and nightGlow(); leaving it unwritten does not fail, it
  // renders the night sky as flat black (see the poleDir note in world.h).
  rp.poleDir[0] = sky.poleDir[0];
  rp.poleDir[1] = sky.poleDir[1];
  rp.poleDir[2] = sky.poleDir[2];
  // MPM fluid render bounds (plan §7 item 5). The AABB is what actually makes
  // the fluid march sleep: `fluidCount` is a MONOTONE estimate that never
  // decays, so without this a world that once held water pays a screen-wide
  // block-map march forever. An empty box (lo > hi) is the "no fluid" signal
  // the shader tests, and it costs one slab test per ray.
  {
    IVec3 flo{0, 0, 0}, fhi{-1, -1, -1};
    world.FluidRenderBounds(tick, flo, fhi);
    rp.fluidLo[0] = flo.x; rp.fluidLo[1] = flo.y; rp.fluidLo[2] = flo.z;
    rp.fluidHi[0] = fhi.x; rp.fluidHi[1] = fhi.y; rp.fluidHi[2] = fhi.z;
  }
  rp.fogDensity = fogDensity;  // horizon fades at the trusted far-field extent
  rp.viewPx = viewPx;          // water ripple LOD footprint (see world.h)
  // Micro-detail animation clock + per-cell variation key (see world.h). Both
  // are render-only inputs; the tick is passed rather than `time` so a flipbook
  // advances at the sim's rate on every machine and reproduces in a replay.
  rp.tick = tick;
  rp.seed = kDefaultSeed;
  // Wind. The evolving half of the field (docs/RESEARCH_wind.md §4.2) — the
  // rest of it is TUNE_* constants folded into the shader. Derived from the
  // TICK, not from `time`, for the same reason the flipbook clock is: weather
  // that advanced with wall time would run at a different rate per machine and
  // would not reproduce in a replay. WindWeather is the only author of these,
  // here and (phase 4) in TickParams, so the renderer and the CA cannot end up
  // in different weather.
  {
    const WindState wind = WindWeather(tun, rp.seed, tick);
    rp.windDir[0] = wind.dirX;
    rp.windDir[1] = wind.dirZ;
    rp.windSpeed = wind.speed;
    rp.windGust = wind.gust;
  }
  // WIND PRIMITIVES (§4.3). The SAME resolved list SubmitTick shipped to the
  // sim this tick — WindPrims() is advanced there and read here, which is what
  // makes the grass lean in a fan's blast and the debug arrows agree with the
  // smoke. Copied rather than shared because the sim and render bind groups
  // deliberately have no buffer in common.
  {
    const WindPrimSystem& wp = WindPrims();
    const uint32_t n = std::min(wp.Count(), kWindPrimCap);
    rp.windPrimCount = n;
    const IVec3 lo = wp.BoundsLo(), hi = wp.BoundsHi();
    rp.windPrimLo[0] = lo.x; rp.windPrimLo[1] = lo.y; rp.windPrimLo[2] = lo.z;
    rp.windPrimHi[0] = hi.x; rp.windPrimHi[1] = hi.y; rp.windPrimHi[2] = hi.z;
    for (uint32_t i = 0; i < n; i++)
      std::memcpy(&rp.windPrims[i * kWindPrimWords], wp.Resolved()[i].w,
                  kWindPrimWords * sizeof(int32_t));
  }
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
                uint32_t fluidLive,
                const uint32_t* fluidSplashMat,
                bool vizActive) {
  particlesActive = particlesActive || !exps.empty() || !spawns.empty();
  uint32_t cellCount = std::min((uint32_t)cells.size(), kMaxCellOpsPerTick);
  uint32_t spawnCount = std::min((uint32_t)spawns.size(), kMaxParticleSpawnsPerTick);
  // MLS-MPM fluid: `fluidLive` is the caller's CONSERVATIVE live estimate
  // (snapshot count + spawns since — the GPU owns the real number). The spawn
  // budget is charged by the CALLER before emitting (rule 2); this clamp is
  // the belt to that brace, and the GPU excite scan enforces the cap exactly.
  uint32_t fluidSpawnCount = std::min((uint32_t)fluidSpawns.size(),
                                      kMaxFluidSpawnsPerTick);
  fluidLive = std::min(fluidLive, kFluidCap);
  if (fluidSpawnCount > kFluidCap - fluidLive)
    fluidSpawnCount = kFluidCap - fluidLive;
  TickParams tp{tick, seed, (uint32_t)ops.size(), hashEnable ? 1u : 0u,
                (uint32_t)exps.size(), sim.Page(), cellCount, 0};
  tp.spawnCount = spawnCount;
  tp.farCount = farCount;  // far-field fills ride the tick submit (render-only)
  // The disturbance-excite switch rides the tick input stream (the dayPhase
  // precedent): tuning is read CPU-side, HERE, so replays and the twice-run
  // determinism gates capture it and a per-gate SetCurrentTuning overrides it
  // with no pipeline rebuild.
  tp.fluidExciteEnable =
      CurrentTuning().sim.fluidExciteMode != 0 ? 1u : 0u;
  tp.fluidSpawnCount = fluidSpawnCount;
  // Wind, the sim's copy (docs/RESEARCH_wind.md §4.2). Same tick, same seed and
  // the same WindWeather call the renderer makes in WriteRenderParams above —
  // one author, so the CA and the grass are in one weather. Quantised to
  // Q16.16 here because everything downstream is integer (rule 1); the gate
  // rides along beside it, read CPU-side per tick the way fluidExciteMode is,
  // so a per-gate SetCurrentTuning moves it without a pipeline rebuild.
  {
    const Tuning& wtun = CurrentTuning();
    const WindStateQ wq = WindQuantize(WindWeather(wtun, seed, tick));
    tp.windDirQ[0] = wq.dirX;
    tp.windDirQ[1] = wq.dirZ;
    tp.windSpeedQ = wq.speed;
    tp.windGustQ = wq.gust;
    tp.windMode = (uint32_t)wtun.sim.windMode;
    // The two dev force multipliers, Q8. Rounded half-away-from-zero by hand
    // for the WindQuantize reason — the rounding mode is part of what the sim
    // sees, so it is written here rather than left to a compiler flag. At the
    // 1.0x default this is exactly kWindScaleOne and every shader consumer
    // takes its identity path, which is what keeps the pinned hash pinned.
    auto scaleQ = [](float v) {
      float c = v < 0.0f ? 0.0f : v;
      int32_t q = (int32_t)(c * (float)kWindScaleOne + 0.5f);
      return q > kWindScaleMax ? kWindScaleMax : q;
    };
    tp.windGasScaleQ = scaleQ(wtun.sim.windGasScale);
    tp.windPartScaleQ = scaleQ(wtun.sim.windPartScale);
    // The drag ramp reference, m/s -> Q16.16 world cells/s. Converted HERE and
    // not in the shader for the WindQuantize reason above: metres are a knob
    // unit, the sim only ever sees cells, and one boundary between them is one
    // that cannot disagree with itself. LoadTuning floors the knob at 1 m/s,
    // and the max() is the second belt — the kernel divides by this.
    {
      double cells = (double)wtun.sim.windDragRef / (double)kVoxelMeters;
      double r = cells * 65536.0 + 0.5;
      if (r > 2147483000.0) r = 2147483000.0;
      tp.windDragRefQ = r < 65536.0 ? 65536 : (int32_t)r;
    }
  }
  // ---- WIND PRIMITIVES (docs/RESEARCH_wind.md §4.3) ------------------------
  //
  // Advanced HERE, in the one function the game loop, --shot and every gate go
  // through, for the reason this file exists at all: a second call site that
  // resolved the list at a different tick would ship the sim a fan that is
  // somewhere the renderer does not draw it, and that reads as a shader bug.
  // Everything downstream — the render copy in WriteRenderParams, the wake
  // list below, the page-table footprint declaration after BeginTick — reads
  // what this call produced.
  //
  // An EMPTY list writes count 0 and the empty AABB, and every shader consumer
  // takes an exact-identity early-out on that, so a world with no fans in it is
  // bit-identical to one built before wind primitives existed.
  std::vector<uint32_t> windWake;
  {
    WindPrimSystem& wp = WindPrims();
    wp.Tick(tick);
    const uint32_t n = std::min(wp.Count(), kWindPrimCap);
    tp.windPrimCount = n;
    const IVec3 lo = wp.BoundsLo(), hi = wp.BoundsHi();
    tp.windPrimLo[0] = lo.x; tp.windPrimLo[1] = lo.y; tp.windPrimLo[2] = lo.z;
    tp.windPrimHi[0] = hi.x; tp.windPrimHi[1] = hi.y; tp.windPrimHi[2] = hi.z;
    for (uint32_t i = 0; i < n; i++)
      std::memcpy(&tp.windPrims[i * kWindPrimWords], wp.Resolved()[i].w,
                  kWindPrimWords * sizeof(int32_t));

    // THE FOOTPRINT WAKE (§10). Only primitives holding the entrainment
    // licence produce one, and the snapshot's occupancy filters out the sky —
    // so a decorative gust costs nothing and a fan aimed at a dune wakes the
    // dune. The budget is charged here, before emission, and the refusals are
    // counted rather than hidden.
    const WorldSnapshot& sn = world.Snap();
    static const std::vector<uint32_t> kNoOcc;
    wp.BuildWake(world, sn.valid ? sn.occupancy : kNoOcc,
                 (uint32_t)std::max(0, CurrentTuning().sim.windWakeChunks),
                 windWake);
    tp.windWakeCount = (uint32_t)windWake.size();
    for (size_t i = 0; i < windWake.size(); i++) tp.windWake[i] = windWake[i];
  }
  // WATER BODIES (docs/PLAN_water_master.md M1; sim/waterbody.h). Advanced
  // HERE, from the one place the game and every harness go through, for exactly
  // the reason WindPrims() is: a path that forgot to advance it would describe a
  // world with no lakes in it while every other path had them.
  //
  // From M2 it WRITES INTO `tp`: geometry, thresholds and a chunk list, all of
  // them pure functions of (seed, window, tuning). That is the whole shape of
  // the fix M1 flagged — the quiescence term used to read the async snapshot,
  // and a shave gated on "when the CPU got around to noticing" is rule 1 broken
  // through the back door. Quiescence and adoption are GPU-side now
  // (sim_waterbody.wgsl); what rides this stream cannot see a fence.
  //
  // `sim.waterBodyMode` is 0 by default and mode 0 is an immediate early-out
  // that leaves both counts at zero, so every pass row's condition is false,
  // nothing is recorded, and the pinned world hash cannot see any of it.
  const WaterBodyGpu* waterGpu = nullptr;
  uint32_t drainBodies = 0;
  {
    const Tuning& wt = CurrentTuning();
    WaterBodySystem& wb = WaterBodies();
    // `worldEdited` is the mutation latch component 6 needs (waterbody.h's
    // drainHotUntil_): holes appear when someone digs, and every dig arrives
    // through the mutation queue, so this is the CPU-visible, tick-stream
    // signal that a hole MIGHT now exist.
    //
    // NARROWED TO A LABELLED CHUNK, and the narrowing is worth 1.9 ms. Arming on
    // ANY mutation anywhere means a lab scene that builds itself out of cell ops
    // arms every lake in the window for 30 s, which keeps the discharge's
    // spawn-op block reserved, which keeps the whole fluid pipeline recorded on
    // ticks nothing is happening: measured 5.10 -> 7.02 ms p50 on `pond68` and
    // "0 idle ticks first" where the scene otherwise reports 68. So a mutation
    // only arms a body whose OWN chunks it touched.
    //
    // `ChunkBody()` here is LAST tick's labelling — deliberately, and it is
    // still tick-deterministic: the labelling is a pure function of (seed,
    // window) and only changes when one of those moves.
    bool worldEdited = false;
    {
      const std::vector<uint32_t>& lbl = wb.ChunkBody();
      auto touch = [&](IVec3 wc) {
        if (worldEdited || !world.ChunkInWindow(wc)) return;
        if (lbl.size() == kNumChunks && lbl[World::SlotChunkIndex(wc)] != 0)
          worldEdited = true;
      };
      for (uint32_t i = 0; i < cellCount && !worldEdited; i++) {
        const uint32_t slot = cells[i].cellIdx / kChunkVol;
        if (slot < kNumChunks && lbl.size() == kNumChunks && lbl[slot] != 0)
          worldEdited = true;
      }
      for (const BrushOp& o : ops) touch({o.x >> 4, o.y >> 4, o.z >> 4});
      for (const ExplosionOp& e : exps) touch({e.x >> 4, e.y >> 4, e.z >> 4});
    }
    wb.Tick(world, seed, tick, wt.sim.waterBodyMode, wt.sim.waterBodyTestDrain,
            wt.sim.drainMaxEighthsPerTick, worldEdited);
    const WaterBodyGpu& g = wb.Gpu();
    waterGpu = &g;
    tp.waterBodyMode = (uint32_t)wt.sim.waterBodyMode;
    tp.waterBodyCount = g.bodyCount;
    tp.waterChunkCount = (uint32_t)g.chunks.size();
    tp.waterTestDrain = wt.sim.waterBodyTestDrain;
    tp.waterQuietTicks = wt.sim.waterBodyQuietTicks;
    tp.waterMinVolume = wt.sim.waterBodyMinVolume;
    // ---- M3: RESERVE THE DISCHARGE'S OP BLOCK (component 6) --------------
    //
    // `spawnAppend` reads a CPU-sized op stream, and the discharge cannot size
    // itself: the head `h` comes from a level the GPU owns. So the CPU does the
    // one thing it can do deterministically — CHARGE THE BUDGET BEFORE EMISSION
    // (rule 2) — by reserving a fixed block per proposed body immediately after
    // this tick's real pours, and sim_waterbody.wgsl's wbDrain fills every slot
    // in it. The ledger refuses the discharge outright to any body that did not
    // get a block (`b < T.waterDrainBodies`), so a granted eighth always has a
    // particle behind it and plan §3.2 holds by construction.
    //
    // Zero at sim.waterBodyMode 0, zero at sim.drainMaxEighthsPerTick 0, and
    // zero whenever nothing is proposed — so the shipped world reserves nothing,
    // the discharge row is not recorded, and the pinned hash cannot see it.
    const uint32_t drainRoom =
        fluidSpawnCount < kMaxFluidSpawnsPerTick
            ? (kMaxFluidSpawnsPerTick - fluidSpawnCount) / kWaterDrainOpsPerBody
            : 0u;
    // GATED ON THE MUTATION LATCH, not merely on the knob. The block is filled
    // every tick it exists, so a standing reservation keeps fluidSpawnCount
    // non-zero forever, keeps the whole fluid seam recorded, and costs a lake
    // nobody has touched real milliseconds — see WaterBodyGpu::drainArmed.
    if (g.drainArmed) drainBodies = std::min(g.bodyCount, drainRoom);
    tp.waterDrainSpawnBase = fluidSpawnCount;
    tp.waterDrainBodies = drainBodies;
    tp.waterDrainMax = wt.sim.drainMaxEighthsPerTick;
    tp.waterExciteRadius = wt.sim.drainExciteRadius;
    // The total the seam dispatches over: real pours, then the reserved block.
    // Written AFTER tp.fluidSpawnCount's own assignment above on purpose — the
    // WriteBuffer below still uploads only the CPU half, because the GPU owns
    // the rest of the range.
    tp.fluidSpawnCount = fluidSpawnCount + drainBodies * kWaterDrainOpsPerBody;
    for (size_t i = 0; i < g.bodies.size() && i < kWaterBodyScalars; i++)
      tp.waterBodies[i] = g.bodies[i];
    for (size_t i = 0; i < g.chunks.size() && i < kWaterChunkCap; i++)
      tp.waterChunks[i] = g.chunks[i];
  }

  // Fluid-lab flat-slab worldgen (world.h kLabSlabY): 0 everywhere except
  // --lab/--fluid-bench. Set on EVERY TickParams write so streamed genList
  // refills and far-cascade fills that ride this tick see the same world.
  tp.labMode = World::LabWorld() ? 1u : 0u;
  if (fluidSplashMat) {
    for (int i = 0; i < 4; i++) tp.fluidSplashMat[i] = fluidSplashMat[i];
  }
  // Day phase for THIS tick. Derived from an INTEGER tick counter — the
  // daylight-gated reactions read it, so anything frame-timed here would break
  // determinism (CLAUDE.md rule 1).
  //
  // That counter is the celestial clock's, not the raw sim tick, and the
  // difference is deliberate: the dev time-speed slider is meant to make the
  // WORLD respond to accelerated time (water freezing, snow melting), not just
  // to race the sun across a world that ignores it. The clock is an exact
  // rational counter, so this stays integer end to end, and it is DISENGAGED
  // unless the slider has been moved — on every headless path it returns
  // `tick` unchanged and the pinned hash cannot move.
  tp.dayPhase = DayPhaseNow(tick);
  IVec3 wo = world.WindowOrigin();
  tp.origin[0] = wo.x; tp.origin[1] = wo.y; tp.origin[2] = wo.z;
  // The mirror corner for the seam's fluid-occupancy fold: the SAME clamp
  // EncodeReadbacks applies to the same input below, so the fold and the
  // voxel mirror describe one cube.
  IVec3 mb = world.MirrorBaseFor(
      {playerChunk.x - 1, playerChunk.y - 1, playerChunk.z - 1});
  tp.mirrorBase[0] = mb.x; tp.mirrorBase[1] = mb.y; tp.mirrorBase[2] = mb.z;
  tp.vizActive = vizActive ? 1u : 0u;
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
  if (fluidSpawnCount > 0) {
    ctx.queue.WriteBuffer(world.fluidSpawnOps, 0, fluidSpawns.data(),
                          fluidSpawnCount * sizeof(FluidSpawnOp));
    // Render-only: a fresh pour must be visible on the frame it lands, and the
    // block list that normally bounds the fluid march is a few ticks behind.
    world.NoteFluidSpawnBounds(fluidSpawns.data(), fluidSpawnCount, tick);
  }
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
  //
  // Compared on the PREVIOUS CELESTIAL tick, not the previous sim tick. Under
  // the time-speed slider the clock can advance many day-phase ticks per sim
  // tick — comparing against `tick - 1` would test a phase the world never
  // saw, and at 100x the world would sail through several dawns without ever
  // waking. `prevCel` is the clock's own previous value, so the switch is
  // detected however far the clock jumped (a jump that skips a whole day still
  // wakes exactly once, which is right: one wake re-dirties everything).
  if (tick > 0) {
    const Tuning& dtun = CurrentTuning();
    const uint32_t prevCel = Celestial().PrevSimTick(tick);
    uint32_t prevPhase = DayPhaseForTick(prevCel, TicksPerDay(dtun),
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
    struct ProbeState {
      rhi::Buffer staging;
      rhi::MapTicket map;
      size_t stagingSlots = 0;
      size_t lastBatchSize = 0;
    };
    auto state = std::make_shared<ProbeState>();

    pt.SetChunkProbe(
        // SUBMIT: encode copies, kick deferred map, return per-slot validity.
        [pctx, pw, state](const std::vector<uint32_t>& slots)
            -> std::vector<bool> {
          std::vector<bool> ok(slots.size(), false);
          if (slots.empty()) return ok;
          const size_t stride = (size_t)kChunkVol * 4;
          if (state->stagingSlots < slots.size()) {
            state->stagingSlots = slots.size();
            state->staging = CreateBuffer(
                pctx->device, (uint64_t)state->stagingSlots * stride,
                rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                "freeProbe");
          }
          rhi::CommandEncoder enc = pctx->device.CreateCommandEncoder();
          size_t copied = 0;
          for (size_t i = 0; i < slots.size(); i++) {
            const uint64_t off = pw->PageOffsetOfSlot(slots[i]);
            if (off == World::kNoPage) continue;
            enc.CopyTracked(pass::Buf::Voxels, pw->voxels, off,
                            state->staging, i * stride, stride);
            ok[i] = true;
            copied++;
          }
          if (copied == 0) return ok;
          pctx->queue.Submit(enc.Finish());
          state->map = rhi::MapReadDeferred(pctx->device, state->staging, 0,
                                            (uint64_t)slots.size() * stride);
          state->lastBatchSize = slots.size();
          return ok;
        },
        // HARVEST: non-blocking Ready() check; copy data out if complete.
        [state](uint32_t* out) -> bool {
          if (!state->map.Ready()) return false;
          state->map.Wait();
          if (!state->map.Succeeded() || !state->map.Data()) {
            state->map.Unmap();
            return false;
          }
          const size_t stride = (size_t)kChunkVol * 4;
          std::memcpy(out, state->map.Data(),
                      state->lastBatchSize * stride);
          state->map.Unmap();
          return true;
        });
  }
  pt.BeginTick(tick);
  for (const BrushOp& o : ops)
    pt.AddOpSphere({o.x, o.y, o.z}, o.radius, world);
  for (const ExplosionOp& e : exps)
    pt.AddOpBox({e.x, e.y, e.z}, kMaxExplosionRadius, world);  // EXP_BOX
  for (uint32_t i = 0; i < cellCount; i++)
    pt.AddOpTarget(cells[i].cellIdx / kChunkVol);  // already a slot chunk index
  // THE WHOLE POINT OF THE FOOTPRINT WAKE (docs/RESEARCH_wind.md §10). The
  // chunks a wind primitive is about to dirty-mark are declared as OP TARGETS,
  // in the same breath and from the same list, so they are materialized with
  // their 26-ring before the command buffer exists.
  //
  // This is what repairs the page table's soundness argument. `cpuDirty` is
  // tightened against a lagging snapshot, and that tightening is only sound
  // because settled matter writes nothing — entrainment is the first rule that
  // makes resting voxels move, and switching it on without this lost 62 voxels
  // to page faults across two 160-tick runs. A grain that hops into a
  // neighbouring chunk now hops into one the CPU already said could be written.
  for (uint32_t slot : windWake) pt.AddOpTarget(slot);
  // THE SAME REPAIR, FOR THE SURFACE SHAVE (docs/PLAN_water_master.md M2). The
  // shave is the second rule in this engine to make RESTING voxels move, and
  // the paragraph above is exactly why that matters: cpuDirty's tightening
  // against a lagging snapshot is only sound because settled matter writes
  // nothing. Entrainment broke that and lost 62 voxels; a shave into a JITTER
  // sentinel would lose a lake one eighth at a time and report it as page
  // faults rather than as a leak.
  //
  // Declared ONLY when a shave can actually fire this tick (`writesThisTick`),
  // so a still lake materializes no pages at all — the residency cost of the
  // feature at rest is zero, not small.
  if (waterGpu && waterGpu->writesThisTick) {
    for (uint32_t e : waterGpu->chunks) pt.AddOpTarget(e & 0xFFFFu);
  }
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
    // fluidChunks(N): every chunk the MLS-MPM seam may write a voxel into —
    // the active block slots from the one-tick-latent snapshot readback plus
    // this tick's CPU-known fluid spawn cells, dilated one ring inside
    // UpdateFluidChunks. The settle converter's >= 8 calm-tick floor is what
    // makes the readback latency safe (world.h fluid block).
    const WorldSnapshot& sn = world.Snap();
    std::vector<uint32_t> blockSlots;
    if (sn.valid && sn.fluidBlockCount > 0) {
      blockSlots.assign(sn.fluidBlocks.begin(),
                        sn.fluidBlocks.begin() + sn.fluidBlockCount);
    }
    std::vector<IVec3> fluidCells;
    fluidCells.reserve(fluidSpawnCount);
    for (uint32_t i = 0; i < fluidSpawnCount; i++)
      fluidCells.push_back({fluidSpawns[i].px >> 16, fluidSpawns[i].py >> 16,
                            fluidSpawns[i].pz >> 16});
    pt.UpdateFluidChunks(blockSlots, fluidCells, world);
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
  if (world.Snap().valid)
    pt.ConsumeOccupancy(world.Snap().occupancy, world.Snap().occStain, tick);
  pt.RetirePages(tick);

  // ---- the §3.4 settled-skip latch ----------------------------------------
  // Fed here because this is the one function BOTH the game loop and every
  // harness tick go through, so the latch cannot see a different world than
  // the encoder does. Declared BEFORE EncodeTick, which reads it.
  //
  // `dirtiedNow` deliberately over-declares: farCount is render-only derived
  // data that cannot dirty a sim chunk, but it costs nothing to be wrong in
  // the safe direction and the list stays a plain "did anything arrive".
  //
  // particlesActive is NOT in the list (§3.2d). It says the particle PASSES are
  // recorded, not that anything was written this tick; the ticks on which a
  // particle can be created are exps/spawns/fluid, all of which are here, and
  // the population already in flight is proven empty (or not) by the snapshot
  // conjunct below. It used to be here, and it held the latch off for the whole
  // 400-tick post-explosion window main.cpp keeps the pipeline alive for.
  //
  // windWake is in the list because it IS a chunk-dirtying input: a settled
  // world with a fan pointed at a dune would otherwise prove itself idle and
  // skip the CA rows the wake had just made necessary, and the fan would mark
  // chunks nothing then simulated.
  //
  // The water shave is in the list for the windWake reason and only when it can
  // fire: it dirty-marks chunks, so a settled world with a draining lake would
  // otherwise prove itself idle and skip the CA rows the shave had just made
  // necessary. A merely LABELLED lake is not an input — it writes nothing —
  // which is what keeps the settled-tick skip alive at sim.waterBodyMode 1.
  sim.NoteTickInputs(tick, !ops.empty() || !exps.empty() || cellCount > 0 ||
                               spawnCount > 0 || !windWake.empty() ||
                               (waterGpu && waterGpu->writesThisTick) ||
                               drainBodies > 0 ||
                               fluidLive + fluidSpawnCount > 0);
  {
    // A snapshot can only license a skip if it is BOTH valid and fresh enough
    // (Simulation::NoteSnapshot enforces the freshness against lastDirtyTick_),
    // and shows nothing in flight — `resolve` is a dirty-writer whose target
    // the CPU never chose, so activeChunks alone does not mean settled.
    const WorldSnapshot& sn = world.Snap();
    if (sn.valid) sim.NoteSnapshot(sn.tick, sn.activeChunks, sn.particleCount);
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
  // The seam's spawn dispatch covers the CPU pours AND the reserved drain
  // block, so both counts carry the total; `waterDrainBodies` is what sizes the
  // discharge row itself and what the ledger's rule-2 refusal compares against.
  const uint32_t fluidSpawnTotal =
      fluidSpawnCount + drainBodies * kWaterDrainOpsPerBody;
  sim.EncodeTick(enc, (uint32_t)ops.size(), hashEnable, (uint32_t)exps.size(),
                 particlesActive, cellCount, spawnCount,
                 fluidLive + fluidSpawnTotal, fluidSpawnTotal,
                 (uint32_t)windWake.size(), vizActive,
                 waterGpu ? (uint32_t)waterGpu->chunks.size() : 0u,
                 drainBodies);
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
  // The authored edit layer patches whatever worldgen produces, so a fresh
  // world re-queues every edited chunk the window contains. Queue only — the
  // ops go out through the MutationQueue on the ticks that follow (rule 3), not
  // by writing voxels from here.
  WorldEditLayer().QueueWindow(world);
  // THE WATER-BODY LEDGER DESCRIBES A WORLD THAT NO LONGER EXISTS. Same
  // argument as InvalidateSnapshot below and the same failure shape: the ledger
  // is GPU-carried state (level, volume, debit, hole), a fresh worldgen refills
  // the lake to its authored height, and a descriptor that survived would go on
  // shaving at the old level against a hole that was filled in. Derived data is
  // reconstructible and DISPOSABLE (plan section 3.1) — so dispose of it, on
  // both sides: the CPU registry and the GPU record.
  WaterBodies().Reset();
  {
    static const std::vector<int32_t> kZero(
        (size_t)kWaterBodyCap * kWaterBodyStateWords, 0);
    ctx.queue.WriteBuffer(world.waterBodyState, 0, kZero.data(),
                          kZero.size() * sizeof(int32_t));
  }
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
  tp.labMode = World::LabWorld() ? 1u : 0u;  // fluid-lab slab (world.h)
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
      gp.labMode = World::LabWorld() ? 1u : 0u;  // fluid-lab slab (world.h)
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

// See support.h for why no gate may write an absolute Y.
int FixtureY(int x, int z, uint32_t seed, int above, int pad) {
  return FixtureYOver(x, z, x, z, seed, above, pad);
}

int FixtureYOver(int x0, int z0, int x1, int z1, uint32_t seed, int above,
                 int pad) {
  // A coarse 5x5 sample of the footprint. TerrainHeight is ~25 hash3 and this
  // runs once per fixture, so the cost is nothing; sampling the CORNERS ONLY
  // would miss a ridge crossing the middle of a wide slab.
  int h = INT32_MIN;
  const int nx = std::max(x1 - x0, 0), nz = std::max(z1 - z0, 0);
  for (int j = 0; j <= 4; j++)
    for (int i = 0; i <= 4; i++)
      h = std::max(h, World::TerrainHeight(x0 + nx * i / 4, z0 + nz * j / 4,
                                           seed));
  const int y = h + above;
  // Clamped against the window, not against a constant: a fixture built
  // outside residency reads as air to every kernel and the gate fails as
  // "nothing landed" rather than as "your fixture is off the map".
  return std::min(y, (int)kWorldN - pad);
}

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

// EVERY Y HERE IS A HEIGHT ABOVE THE GROUND, never an absolute one, and that is
// load-bearing rather than tidy. These ops are what the determinism hash and the
// smoke probes are computed over: a sand column falling onto terrain, a pour
// landing on the platform, fire reaching a surface, a seed on soil. Written as
// absolute Y they were silently anchored to a 5.4 m terrain band — the moment
// the datum moves (the terrain overhaul raises it by ~200 voxels) every one of
// them is either buried in rock or dropped from the sky into a chunk that is not
// resident, and the failure surfaces as a hash change with no cause.
//
// World::TerrainHeight is genColumn's `h` exactly (the height contract in
// DESIGN.md), so `+ kDelta` means what it reads as: that many voxels of clear
// air above the ground the brush is aimed at.
std::vector<BrushOp> SelftestOps(uint32_t tick, uint32_t seed) {
  std::vector<BrushOp> ops;
  auto ground = [&](int x, int z) { return World::TerrainHeight(x, z, seed); };
  if (tick >= 5 && tick < 150) {
    ops.push_back({100, ground(100, 100) + 110, 100, 6, kMatSand, 0, 0, 0});
    ops.push_back({176, ground(176, 176) + 90, 176, 5, kMatWater, 0, 0, 0});
  }
  if (tick >= 30 && tick < 90) {
    ops.push_back({64, ground(64, 72) + 6, 72, 4, kMatSmoke, 0, 0, 0});
  }
  // reaction-system coverage: lava boiling the pool, fire on the wood
  // platform, seeds germinating — all feed the determinism hash check
  if (tick >= 40 && tick < 100) {
    ops.push_back({176, ground(176, 150) + 60, 150, 4, kMatLava, 0, 0, 0});
  }
  if (tick >= 60 && tick < 120) {
    ops.push_back({110, ground(110, 110) + 20, 110, 3, kMatFire, 0, 0, 0});
  }
  if (tick >= 10 && tick < 16) {
    ops.push_back({150, ground(128, 128) + 30, 128, 2, kMatSeed, 0, 0, 0});
  }
  // melt-mode coverage (laser, PLAN §C1): catches the falling sand column in
  // a mode-2 brush — molten-glass conversion feeds the determinism hash. Four
  // under the sand source, so it cuts the column rather than its origin.
  if (tick >= 70 && tick < 100) {
    ops.push_back({100, ground(100, 100) + 106, 100, 3, 0, 2u, 0, 0});
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

void ReadWaterLedgerSync(GpuContext& ctx, World& world, int32_t* out) {
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.waterBodyState, 0, out,
                        (size_t)kWaterBodyCap * kWaterBodyStateWords * 4,
                        "waterLedgerRead");
}

void ReadFluidArgsSync(GpuContext& ctx, World& world, uint32_t* out32) {
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, out32,
                        32 * 4, "fluidArgsRead");
}

void ReadPageFaultsSync(GpuContext& ctx, World& world, uint32_t out[4]) {
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.pageFaults, 0, out, 16,
                        "pageFaultRead");
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
