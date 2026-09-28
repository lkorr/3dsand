// pass_table.h — types for the declarative pass table (src/sim/pass_table.def).
//
// Phase 2b of docs/PLAN_vulkan_port.md. The .def is the single source; this
// header gives it C++ types and expands it into one constexpr array, and
// scripts/check_pass_table.py scrapes the same .def. Read the .def's header
// comment first — it explains what a row means and why the table exists at all.
//
// Nothing here knows about Vulkan. The table declares WHAT is recorded; the
// last-access tracker in gpu/vk_record.cpp is what turns each row's `uses`
// into vkCmdPipelineBarrier2 calls (barrier_graph §3.3). Keeping those two
// apart is why a row can be checked against the WGSL by a python script.

#pragma once

#include <cstdint>

namespace pass {

// ---------------------------------------------------------------- buffers --
// Resolvable identities, NOT strings: a typo is a compile error, and the
// recorder maps an id to a live rhi::Buffer in exactly one switch
// (PassBuffer, pass_table.cpp).
//
// DirtyIn/DirtyOut are SYMBOLIC — `page_` selects which of dirty[0]/dirty[1]
// each resolves to, and the recorder resolves at record time. Dirty0/Dirty1 are
// the two concrete ids, used by worldgen which clears both regardless of page.
// barrier_graph §2.2, and §4.1's [NEW EDGE]: DirtyIn and DirtyOut must never
// resolve to the same id, or a tick's dirtyOut fill would silently clobber a
// day/night wake. check_pass_table.py asserts it for both page values.
enum class Buf : uint8_t {
  Voxels,
  DirtyIn,          // symbolic: dirty[page_]
  DirtyOut,         // symbolic: dirty[1 - page_]
  Dirty0,           // concrete dirty[0]
  Dirty1,           // concrete dirty[1]
  Materials,
  TickUBO,
  PassUBO,
  OpsBuf,
  Occupancy,
  Hash,
  Pick,
  RenderUBO,
  Reactions,
  DirtyList,
  ArgsStage,
  CellOps,
  Support,
  GenList,
  PageFillList,     // JITTER materialization: (slot, entry) pairs
  DispatchArgs,
  ParticlesRead,    // symbolic: particles[page_]
  ParticlesWrite,   // symbolic: particles[1 - page_]
  ParticleCounts,
  Claim,
  PArgsStage,
  PDispatchArgs,
  ExpOps,
  ExpMask,
  SpawnOps,
  DrawArgs,
  // ---- gas particles (docs/PLAN_gas_particles.md stage 1) ----
  // The pair is SYMBOLIC like ParticlesRead/Write: page_ resolves which
  // concrete buffer each names, same parity convention.
  //
  // GasSpawn carries a WRITE from sim_step (the CA appends every voxel that
  // left the window) and a READ from sim_gas's spawn pass in the same command
  // buffer — one of the few genuine CA -> non-CA hazards in the tick, and the
  // reason it is on the table rather than being "just an op list".
  // GasDispatchArgs is indirect-only and never bound, like DispatchArgs.
  GasParticlesRead,
  GasParticlesWrite,
  GasCounts,
  GasClaim,
  GasSpawn,
  GasSpawnOps,
  GasArgsStage,
  GasDispatchArgs,
  // The far fire-plume emitter list (world.h kGasFarEmitMax). CPU-written,
  // read by ONE kernel, and its only product is GasOuter — so it carries a
  // host-write -> shader-read hazard and nothing else. On the table because a
  // read the table does not know about is the failure mode this file exists to
  // make impossible.
  GasFarEmit,
  // The LONG-RANGE density box (world.h kGasFarOuterN). Exactly GasOuter's
  // standing -- render-only derived data written on the tick command buffer and
  // read by the raymarcher in the fragment stage -- at eight times the cell.
  GasFarOuter,
  // Render-only derived data, on the table for the shadow cache's reason: the
  // splat WRITES it on the tick command buffer and the raymarcher READS it in
  // the fragment stage, and a hazard the table does not know about generates
  // no barrier.
  GasOuter,
  FarVox,
  FarOcc,
  FarList,
  FarUBO,
  FarPatch,   // cascade edit patches, read by farFill (world.h kFarPatch*)
  FarSig,     // per-slot far-matter signature, fardown's skip (world.h farSig)
  FarMap,     // the far SURFACE MAP (world.h kFarMap*): farmap fills, farpatch/fardown invalidate
  // ---- the software page table (docs/PLAN_page_table.md §5.1) ----
  // PageTable is READ by every row whose entry point touches a voxel and
  // written by nothing on the tick path — it is dispatch-invariant
  // configuration, not sim state.
  //
  // PageFaults is permanent, always-bound and UNCONDITIONAL: no PAGE_ASSERT
  // prelude flag, no conditional binding, no #if in the .def. It gets an
  // A(PageFaults) use on every row that can call voxStore. The atomic
  // increment sits on a branch never taken in a correct build, so its
  // production cost is a branch that never fires — and in exchange there is
  // ONE bind-group layout, ONE .def, and no configuration under which the
  // pass table and the shaders disagree. A conditionally-declared binding
  // would be two layouts that must agree, which is the shape this repo has a
  // checker to prevent.
  PageTable,
  PageFaults,
  // ---- MLS-MPM fluid (docs/PLAN_mpm_fluids.md; sim_fluid.wgsl + seam) ----
  // The particle pair is SYMBOLIC like ParticlesRead/Write: page_ resolves
  // which concrete buffer each names. FluidParticlesWrite is the tick's
  // working buffer (compaction target, solver state, render source);
  // FluidParticlesRead is last tick's, read only by the compaction.
  // FluidDispatchArgs / FluidPDispatchArgs are indirect-only, never bound.
  FluidParticlesRead,
  FluidParticlesWrite,
  FluidSpawnOps,
  FluidBlockMap,
  FluidBlockList,
  FluidGrid,
  FluidArgsStage,
  FluidDispatchArgs,
  FluidPDispatchArgs,
  FluidExciteScratch,
  FluidCalm,
  FluidSettleScratch,
  FluidCompactScratch,
  FluidCellScratch,
  FluidMirror,
  ActVoxViz,
  // The water-body drain ledger (docs/PLAN_water_master.md M2). GPU-OWNED
  // state: the level, the debit, the adoption verdict. Written by every one of
  // the four sim_waterbody.wgsl entry points and read by nothing else on the
  // tick path, which is what lets it be one buffer with one barrier class.
  WaterBodyState,
  // The surface-momentum store (docs/PLAN_water_relevel.md §4.1, world.h
  // kWaterFluxWords). Per COLUMN rather than per body, which is why it is a
  // resource of its own and not more words on WaterBodyState. Written by
  // `waterFlux` and `waterSurface`, read by `waterRelevel` — three rows in one
  // command buffer, so the compute->compute barrier between them is exactly
  // what this table exists to derive.
  WaterFlux,
  // The baked tree atlas (src/sim/treeatlas.h). READ-ONLY, dispatch-invariant
  // asset data uploaded once at load, exactly like Materials -- worldgen samples
  // it instead of evaluating implicit tree shapes per cell. It never appears on
  // a write side, so it generates no barriers; it is in the table because a
  // read that the table does not know about is the failure mode this file
  // exists to make impossible.
  TreeAtlas,
  // The authored world map (src/sim/worldmap.h, docs/PLAN_world_map.md).
  // Identical standing to TreeAtlas above: read-only, dispatch-invariant asset
  // data uploaded once at load, sampled by worldgen per column. It never
  // appears on a write side, so it generates no barriers; it is on the table
  // because a read the table does not know about is exactly the failure mode
  // this file exists to make impossible.
  WorldMap,
  // ---- the repose occupancy snapshot (world.h kReposeSnap* block) ----
  // 1 bit per voxel, "a powder could drop into this cell", plus one tick stamp
  // per slot. WRITTEN by the `reposesnap` prepass and READ by the CA's own
  // rows, both on PT_TICK, which is exactly the compute->compute hop the table
  // exists to synchronize: a missing barrier there would let the CA read the
  // snapshot while the prepass is still writing it, which is the determinism
  // hole the snapshot was introduced to close.
  ReposeSnap,
  // ---- the clouds (cloud.wgsl, world.h kCloud*) ----
  // Render-only derived data on the per-FRAME ShadowCache table, there for
  // the shadow cache's reason: the cloud compute rows WRITE these and the
  // raymarcher and the raster bodies READ them in the fragment/vertex stages
  // of the same command buffer — the compute->fragment hop nothing else in
  // this engine synchronises. CloudUBO is the uniform (world.h CloudParams).
  CloudUBO,
  CloudNoise,
  CloudWeather,
  CloudMaps,
  CloudRaw,
  CloudHist,
  // ---- the voxel-keyed shadow cache (world.h kShadowCacheBuckets) ----
  // RENDER-side buffers, on the table for the same reason TreeAtlas is: a
  // hazard the table does not know about generates no barrier. The resolve
  // compute pass WRITES ShadowCache and the fragment shader READS it in the
  // same command buffer, which is the exact compute->fragment hop that has no
  // other source of synchronisation in this engine.
  //
  // ShadowArgs is indirect-only and never bound, like DispatchArgs.
  ShadowCache,
  ShadowReq,
  // The penumbra window (world.h kShadowHistBytes). Read AND written by the
  // resolve pass and by nothing else, so it generates no cross-stage hazard —
  // it is on the table for the reason WorldMap is: a buffer the table does not
  // know about is the failure mode this file exists to make impossible.
  ShadowHist,
  // The ray-start map (assets/shaders/ray_start.wgsl): per 2x2 pixel block,
  // a distance along the primary ray before which nothing can be hit.
  // WRITTEN by the two per-frame rows on the ShadowCache table and READ by
  // raymarch.wgsl's fragment stage in the same command buffer -- the
  // compute->fragment hop the table exists to synchronise. Render-private
  // like ShadowCache: never hashed, never saved, sized on the render TARGET
  // (Simulation::EnsureRayStart), not the world.
  RayStart,
  ShadowArgsStage,
  ShadowArgs,
  // ---- the openness (sky-visibility) grid (world.h kOpenFaces) ----
  // Render-only derived data, on the table for the shadow cache's reason: the
  // openness pass WRITES these on the TICK command buffer and the raymarcher
  // (plus microbody/debris) READS them in the fragment stage, and a hazard the
  // table does not know about generates no barrier.
  Openness,
  OpennessGen,
  // ---- the irradiance grid (world.h kIrradianceBytes, PLAN_gi.md §3) ----
  // Same standing; written by shadow_resolve (per frame) AND the openness walk
  // (per tick), so it carries a write in two tables and the recorder's
  // per-command-buffer tracker plus the global barrier every buffer opens with
  // are what order them.
  Irradiance,
  // ---- the deferred streaming wake's act verdict (world.h `genAct`) ----
  // Written by worldgen:list/genChunk when TickParams::genDeferWake is set and
  // read back by Stream. On the table because the readback copy is issued
  // through CopyTracked and the recorder needs the compute-write -> transfer-
  // read hazard (docs/RESEARCH_streaming_hitch.md R1).
  GenAct,
  // ---- the glow field (world.h kGlowBytes, assets/shaders/sim_glow.wgsl) ----
  // ONE Buf id for BOTH regions of one buffer (the per-chunk sources and the
  // per-block field): they live in one allocation so the field costs one
  // binding in each layout that carries it rather than two, and the recorder
  // wants exactly this granularity anyway — the `src` row writes the source
  // words and the `field` row reads them back, so the RW -> RW edge between the
  // two rows IS the barrier that makes the two-stage split correct.
  Glow,
  // ---- the per-chunk digest table (world.h kChunkHashWords, M9.3-A) ----
  // Written by sim_occupancy.wgsl's FULL entry point and by nothing else, and
  // READ BACK through CopyTracked every tick — which is the hazard the table
  // has to know about: a compute write followed by a transfer read in the same
  // command buffer, exactly Hash's situation one array wider.
  ChunkHash,
  // ---- the worldgen column cache (worldgen.wgsl, Simulation::WriteGenList) ----
  // genCols is CPU-written list input (WriteBuffer, drained at the head of the
  // command buffer); colCache is written by `cols` and read by genChunk in
  // the SAME command buffer, which is the hazard the table has to know about.
  GenCols,
  ColCache,
  // The solute layer (world.h kSol* block, sim_solute.wgsl). SolArgs is
  // indirect-only and never bound, like DispatchArgs.
  SolTable,
  SolPool,
  SolMeta,
  SolSpec,
  SolArgs,
  SolStage,   // eviction / restore staging (world.h kSolStage*), binding 44
  kCount,
};

// How a pass touches a buffer. The barrier mapping (phase 3) is in
// barrier_graph §3.2; StorageAtomicRMW is READ|WRITE, not something weaker.
enum class Acc : uint8_t {
  StorageRead,
  StorageWrite,      // written but never read by this entry point
  StorageRW,
  StorageAtomicRMW,
  Uniform,
  IndirectRead,
  TransferRead,
  TransferWrite,
};

struct Use {
  Buf buf;
  Acc acc;
};

enum class Kind : uint8_t { Compute, ComputeIndirect, Copy, Fill };

// Which Simulation pipeline member. PIPE_NONE for Fill/Copy rows.
enum class Pipe : uint8_t {
  None,
  Worldgen, WorldgenList,
  // The column-cache pre-pass both of the above read (worldgen.wgsl `cols`).
  WorldgenCols,
  Mutate, MutateCells,
  // The wind primitive footprint wake (sim_mutate.wgsl `windWake`): the only
  // kernel in the engine that dirty-marks a chunk without writing a voxel.
  // See docs/RESEARCH_wind.md §4.3 and the note at the entry point.
  WindWake,
  // Rain on the ground and the drying of wet top surfaces (sim_mutate.wgsl
  // `rainFall`): one thread per kRainTile^2 column tile, every tick.
  RainFall,
  Compact, CompactNext,
  Step,
  Occupancy, OccupancyDirty,
  Pick,
  ExplodeMark, ExplodeApply,
  PArgs1, PSpawn, PIntegrate, PArgs2, PResolve,
  // Gas particles (sim_gas.wgsl, docs/PLAN_gas_particles.md). Five entry
  // points shaped like the ballistic five, in the same order and for the same
  // reasons; GasArgs1 additionally zeroes the write page's cursor, which is
  // why the gas pool needs no per-tick fill.
  GasArgs1, GasSpawnP, GasIntegrate, GasArgs2, GasResolve,
  // Far fire plumes: the frozen-fire density splat (sim_gas.wgsl
  // `gasFarPlume`). A sixth gas entry point, in the gas group, writing only
  // GasOuter — it is not part of the parcel pipeline and is recorded under its
  // own condition, so a world with frozen fires and no parcels pays for this
  // row and none of the five above it.
  GasFarPlume,
  // ...and the same kernel one LOD out (sim_gas.wgsl `gasFarPlumeWide`), for
  // the fires past gasOuter's reach. A seventh gas entry point on the same
  // layout, writing only the long-range box.
  GasFarPlumeWide,
  // The angle-of-repose occupancy snapshot: a second entry point of
  // sim_step.wgsl, not a new module, so it costs no bind-group layout and
  // cannot drift from the kernel that reads it.
  ReposeSnap,
  // MLS-MPM fluid. Inserted BEFORE FarDown deliberately: the
  // pipeline-copy loop in Simulation::RecordTable is bounded by
  // `(int)Pipe::FarDown + 1`, so FarDown must stay the last enumerator or a
  // new pipeline is silently never handed to the recorder (a skipped row, not
  // a crash).
  FluidSpawn, FluidMark, FluidAlloc, FluidClear, FluidP2G, FluidP2G2,
  FluidGridUp, FluidG2P,
  // The excite/settle seam (sim_fluid_seam.wgsl).
  FluidCompactCount, FluidCompactScan, FluidCompactScatter,
  FluidExciteDetect, FluidExciteScan, FluidExciteEmit,
  FluidPTick, FluidSettleJudge, FluidSettleScan, FluidSettleBin,
  FluidSettleCheck, FluidSettleCommit, FluidSettleKill,
  FluidConsumeApply, FluidStainApply, FluidMirrorFold, FluidCellClear,
  // Materializes a JITTER page (world.h's JITTER block): the one fill a
  // vkCmdFillBuffer cannot do, because the words vary per cell.
  PageFill,
  // Water bodies (sim_waterbody.wgsl). Four entry points, recorded in this
  // order inside one PT_TICK pass group — the order is load-bearing and the
  // shader's header says why.
  WaterQuiet, WaterLedger, WaterReduce, WaterShave, WaterDrain, WaterHole,
  // M5: the scheduled container sweep and its split labelling.
  WaterSweep, WaterSplit,
  // W1: the relevel apply and the free-surface measure that feeds next tick's
  // ledger (docs/PLAN_water_relevel.md §3.1).
  WaterRelevel, WaterSurface, WaterFluxPipe,
  // The far-fill sieve and the edit-patch half it was split into
  // (docs/PLAN_shader_compile.md package C item 1: worldgen.wgsl `far` and
  // `farpatch`). Two pipelines, two rows on PT_FARFILL, recorded back to back
  // — the pass table's edge between them is the storageBarrier that used to
  // sit inside the merged entry.
  // FarMapFill is the far SURFACE MAP's fill (worldgen.wgsl `farmap`), a third
  // row between the two: after the sweep, before the patch that invalidates it.
  FarFill, FarMapFill, FarPatchFill, FarDown,
  // The openness grid (sim_openness.wgsl). Two entry points: the dirty walk
  // (indirect on the compacted dirty list, exactly like occupancyDirty) and the
  // rolling refresh. BEFORE ShadowPrepare so the pipeline-copy loop's bound in
  // Simulation::RecordTable — which is `(int)Pipe::ShadowResolve + 1` — still
  // covers them without moving.
  OpennessDirty, OpennessRefresh,
  // The glow field (sim_glow.wgsl). Three entry points: the two-stage dirty
  // walk (`src` then `field`, both indirect on the compacted dirty list) and
  // the rolling refresh. BEFORE ShadowPrepare for the reason stated above —
  // the pipeline-copy loop's bound in Simulation::RecordTable is
  // `(int)Pipe::ShadowResolve + 1` and a Pipe added past it is silently never
  // copied into the recorder's table.
  GlowSrc, GlowField, GlowRefresh,
  // The solute layer (sim_solute.wgsl). BEFORE ShadowResolve: RecordTable
  // hands the recorder pipelines up to that enumerator only.
  SolWant, SolArgsP, SolAlloc, SolDiffuse, SolCompact, SolScoop, SolPour, SolHash, SolEvict, SolRestore,
  // The clouds (cloud.wgsl): the one-shot noise bake, then the per-frame
  // weather map, shadow map, env map, march and temporal resolve. BEFORE
  // ShadowPrepare for the pipeline-copy bound's reason stated above.
  CloudNoise, CloudWeather, CloudShadow, CloudEnv, CloudMarch, CloudResolve,
  // Voxel-keyed shadow cache (shadow_resolve.wgsl). AFTER FarDown, which the
  // note at the fluid block says must stay last — so the copy loop's bound in
  // Simulation::RecordTable moves to ShadowResolve with these. Both are render
  // -path passes: they run once per FRAME from EncodeShadowResolve, not on the
  // tick table.
  // The far cascade's per-level sky bound (shadow_resolve.wgsl skyTop*),
  // per-FRAME rows on the ShadowCache table. Before ShadowResolve for the copy
  // loop's bound.
  SkyTopClear, SkyTopReduce,
  // The ray-start map (ray_start.wgsl), per-FRAME rows on the ShadowCache
  // table, after the sky bound they read and before the resolve.
  RayStartTrace, RayStartMin,
  ShadowPrepare, ShadowResolve,
  // Not a pipeline: the array bound the two recorder-side mirrors size
  // themselves by. It was a LITERAL 64 in vk_record.h and rhi_record.h, and
  // the enum reached 63 before gas added five — one more addition anywhere
  // would have written past the end of both, silently, with the row that
  // overran being whichever came last. A count that derives from the list it
  // counts cannot go stale.
  kPipeCount,
};

// Bind-group set. GRP_SIM also carries the dynamic passUBO offset.
enum class Groups : uint8_t { None, Sim, SlimPart, SlimFar, SlimFluid,
                              SlimFluidSeam,
                              // gasBGL_ — the gas pool, its claim/args/outbox,
                              // plus farVox + FarParams (blocking outside the
                              // window) and reactions (the decay bucket).
                              SlimGas,
                              // shadowBGL_ — voxels/occupancy/materials/
                              // renderUBO/pageTable plus the cache's own three.
                              Shadow };

// Dynamic passUBO offset selector.
//   None  no dynamic offset (the row's groups have none)
//   Zero  offset 0 — every non-CA GRP_SIM row
//   Ca    k * kPassStride for iteration k: the colour phase + gravity substep.
//         This is what makes each CA iteration a DIFFERENT colour, which is why
//         the iterations must not overlap (pass_table.def header, §3.6/§7.1).
enum class Dyn : uint8_t { None, Zero, Ca };

// The passUBO slice stride Dyn::Ca steps by. It lives HERE rather than as a
// file-static in simulation.cpp because it is a property of the TABLE's
// Dyn::Ca selector, not of a recorder — it was already consumed by two walkers
// before the Dawn removal left one, and a constant copied into a consumer is
// the "two places that must agree" bug this repo has a checker for. Keep it
// here even though there is currently a single reader.
//
// 256 is the value passUBO was built with (54 slices x 256 B) and is a legal
// dynamic offset on every desktop device: Vulkan caps
// minUniformBufferOffsetAlignment at 256, and the requirement is that the
// offset be a multiple of it (it is 64 on the development RTX 3060 Ti).
inline constexpr uint32_t kPassStride = 256;

// Conditions, all known on the CPU before recording begins. A row whose
// condition is false is SKIPPED ENTIRELY (barrier_graph §3.9/§7.5).
enum class Cond : uint8_t {
  Always,
  Ops,        // opsCount > 0
  Cells,      // cellCount > 0
  Exp,        // expCount > 0
  Spawn,      // spawnCount > 0
  Particles,  // particlesActive
  Hash,       // hashEnable  (tick % 15 == 0)
  DirtyTick,  // !hashEnable
  GenCount,   // EncodeGenList count > 0
  FarCount,   // EncodeFarFill count > 0
  // Whole-world worldgen: the D_CHUNKS dispatch that writes all 32,768 slots.
  // Suppressed under --residency paged, where worldgen runs BATCHED through
  // worldgenList instead (PLAN_page_table.md §3.5c): a kernel cannot allocate,
  // so every slot genChunk touches must have a page before the dispatch, and
  // all 32,768 at once would need a dense pool — i.e. no saving at the moment
  // of worldgen, and under §3.8's fatal policy an abort at startup.
  DenseWorldgen,
  FluidSpawn, // fluidSpawnCount > 0 (MLS-MPM spawn row in PT_TICK)
  // windWakeCount > 0: at least one wind primitive holds the entrainment
  // licence and its footprint has chunks with matter in them. Zero on every
  // tick of a world with no fans in it, which is what makes the feature free.
  WindWake,
  // The CA loop and its compaction/staging setup, suppressed when the CPU can
  // PROVE the dirty set is empty (ROADMAP_scale.md §3.4, Simulation::
  // NoteTickInputs). A settled world otherwise records 54 indirect dispatches
  // of (0,1,1) — 141.7 µs/tick measured for provably zero invocations, and the
  // cost is per-DISPATCH so it survives every other optimization and grows 8x
  // at a 2048³ window.
  //
  // Skipping a zero-workgroup dispatch removes no invocation, so the voxel
  // writes are bit-identical and the pinned hash is the gate on that claim.
  // The proof obligation is entirely on the CPU mirror being CONSERVATIVE:
  // wrong-active costs microseconds, wrong-idle loses world state.
  CaActive,
  VizActive,
  // waterChunkCount > 0: at least one water body is proposed AND
  // sim.waterBodyMode is 1. Zero on every tick of a shipped world, which is
  // what makes `sim.waterBodyMode = 0` an EXACT identity rather than merely a
  // cheap path — nothing is recorded, so the pinned hash cannot move.
  WaterBody,
  WaterDrain, // waterDrainBodies > 0 (the reserved spawn-op block, M3)
  // M5: waterSweepSlot < kWaterBodyCap. The CPU schedules AT MOST ONE
  // basin's re-derive per tick, and only for a basin someone has dug in —
  // so on every tick of every world where nobody has touched the water,
  // neither sweep row is recorded and the cost is exactly zero (rule 2).
  WaterSweep,
  // W2: sim.waveMode > 0 AND a body is listed. The surface-momentum row is the
  // only row this gates, and leaving it unrecorded at 0 is what makes
  // `--sweep sim.waveMode=0,1` an identity claim about the ROW as well as about
  // the kernel -- the pinned hash cannot see a pass that was never recorded.
  WaterWave,
  // opennessChunks > 0: the openness grid is on (render.opennessStrength > 0)
  // AND its per-tick chunk budget is nonzero. Both rows carry it, so
  // `opennessStrength = 0` records NOTHING — which is what makes the
  // `noopenness` arm in --render-budget measure the pass AND the reads rather
  // than the reads alone.
  Openness,
  // glowChunks > 0: the glow field is on (render.glowStrength > 0) AND its
  // per-tick refresh budget is nonzero. All three rows carry it, so
  // `glowStrength = 0` records NOTHING — which is what makes the `noglow`
  // --render-budget arm measure the passes AND the reads rather than the reads
  // alone, exactly as `noopenness` does one line up.
  Glow,
  // Gas particles exist OR the CA has work this tick. Both halves are needed
  // and neither alone is enough: a parcel already in flight must be stepped
  // even in a chunk-quiet world, and a gas voxel can only reach the window
  // edge from a chunk the CA is running. In a settled world with no plume both
  // are false and every gas row records NOTHING — which is what makes the
  // system free when it is not being used (rule 2).
  Gas,
  // ---- the two far fire-plume conditions (world.h kGasFarEmitMax) ----------
  // GasFarEmit: the CPU handed the GPU at least one frozen-fire emitter this
  // tick. It gates the splat row ALONE, so a world with no evicted fire in it
  // records nothing new whatever the parcels are doing.
  //
  // GasOuter: Gas OR GasFarEmit — "somebody is going to write the density box
  // this tick". It gates the box's per-tick CLEAR, and it has to be the union
  // rather than either half: gasOuter is a per-tick SPLAT, not an accumulator,
  // so a tick that splats without clearing leaves last tick's plume added to
  // this one's, and a tick that clears without splatting is a plume that
  // flickers out. The five PARCEL rows stay on Gas — arming them for a frozen
  // fire would pay gasSpawnStep's fixed 1,042 workgroups forever for a
  // population that does not exist (rule 2).
  GasFarEmit,
  GasOuter,
  // GasFarWide: the CPU handed the GPU at least one LONG-RANGE emitter this
  // tick. It gates the wide splat AND the long-range box's own clear, which is
  // the union discipline the two conditions above state, in its simplest form:
  // this box has exactly ONE writer, so "whoever writes it" and "the clear"
  // are the same predicate and there is nothing to union. render.farPlumeRange
  // 0 makes it false and nothing about the long-range box is recorded at all.
  GasFarWide,
  // Any LOADED material authors a non-default `repose` AND the CA has work.
  // The first half is a property of materials.json, latched once at
  // UploadTables rather than recomputed per tick; the second is CaActive,
  // folded in so the prepass never records on a tick the CA itself skips.
  //
  // It exists so the feature is provably free when nothing uses it: strip every
  // `repose` line and not one reposeSnap row is recorded, no 16 MiB buffer is
  // touched, and the pinned hash cannot move. The same shape as Cond::Gas and
  // Cond::WaterBody -- "off" means NO ROW, not a cheap row.
  ReposeActive,
  // ---- the per-frame table's three switches (RecordCtx::cloudFlags) ----
  // Clouds: the sky has cloud in it and weather.clouds is on. Off = not one
  // cloud row recorded, and the composite reads nothing (RWF_CLOUDS clear).
  Clouds,
  // CloudBake: the noise volume needs baking (first frame after a pipeline
  // build). One row, once.
  CloudBake,
  // ShadowCacheOn: the voxel shadow cache's three rows. They were implicitly
  // gated by EncodeShadowResolve returning early; now that the table also
  // carries the clouds, the gate has to be a row condition.
  ShadowCacheOn,
  // RayStart: the ray-start map's two rows (RecordCtx::rayStartGx > 0, i.e.
  // a world pass has sized its buffer). Off = no row, and the raymarch's
  // key check reads the stale key as "not this frame" and marches from
  // the camera.
  RayStart,
};

// Which command buffer a row belongs to — one per Encode* entry point.
// Fluid is the exception to "one per entry point" and deliberately so: it is
// ONE MLS-MPM SUBSTEP, recorded kFluidSubsteps times per tick from EncodeTick
// into the tick's own command buffer (precedent: FarFill is also recorded into
// the tick's buffer). The recorder's last-access tracker persists across
// RecordTable calls within a command buffer, so the inter-substep barriers
// (grid WAW against the next clear, particle RAW into the next mark) are
// generated exactly like intra-table ones.
enum class Table : uint8_t { Tick, Worldgen, GenList, LoadReset, HashOnly, FarFill,
                             FluidMap, Fluid, FluidSeam, FluidSettle, PageFill,
                             // Per-FRAME, not per-tick: recorded by
                             // EncodeShadowResolve immediately before the
                             // render pass that consumes it.
                             ShadowCache, SolEvict, SolRestore };

// Dispatch extents. Values >= kDynBase are selectors resolved at record time
// from the tick's counts; anything below is a literal extent. Indirect rows put
// the args-buffer selector in x.
enum class DispatchSel : uint32_t {
  // literal extents pass through unchanged (0 .. kDynBase-1)
  kDynBase = 0x10000000,
  Ops,        // 4 * opsCount        (y,z are literal 4,4)
  Cells,      // (cellCount + 63)/64
  Exp,        // kExplosionWg * expCount   (y,z literal kExplosionWg)
  Spawn,      // (spawnCount + 63)/64
  ExpWg,      // kExplosionWg — the y/z extent of the two explosion rows
  Chunks,     // kNumChunks
  Chunks64,   // kNumChunks / 64
  GenCount,   // EncodeGenList count
  GenColCount,  // chunk-columns of the gen list (Simulation::WriteGenList)
  FarCount,   // EncodeFarFill count
  IndDispatchArgs,   // indirect: world.dispatchArgs @ 0
  IndPDispatchArgs,  // indirect: world.pDispatchArgs @ 0
  // ---- MLS-MPM fluid ----
  WindWakeSel,       // (windWakeCount + 63)/64
  FluidSpawnSel,     // (fluidSpawnCount + 63)/64
  IndFluidArgs,      // indirect: world.fluidDispatchArgs @ 0
  IndFluidPArgs,     // indirect: world.fluidPDispatchArgs @ 0 — the seam's
                     // per-particle passes and its list-shaped dispatches
                     // (the seam re-copies the buffer between uses)
  // ---- water bodies ----
  // One WORKGROUP per chunk the openness refresh walks this tick, from
  // render.opennessChunksPerFrame. A knob, not a count of live work, so it is a
  // selector rather than a literal extent: changing it must not need a rebuild.
  OpennessChunks,
  // Same shape for the glow field's rolling refresh, from
  // render.glowChunksPerFrame.
  GlowChunks,
  WaterChunks,       // waterChunkCount — one WORKGROUP per listed chunk
  WaterChunks64,     // (waterChunkCount + 63) / 64 — one THREAD per chunk
  // One THREAD per RESERVED drain spawn-op slot. Every slot in the block has
  // to be written every tick it exists — a slot the pass skipped keeps a
  // stale particle that spawnAppend would hand back to the pool as live.
  WaterDrainSel,     // (waterDrainBodies * kWaterDrainOpsPerBody + 63) / 64
  // ---- shadow cache ----
  // Indirect: world.shadowArgs @ 0. The count is a GPU-side quantity (the
  // fragment shader appended it last frame), so the dispatch size cannot come
  // from a RecordCtx field the way farCount does — nothing on the CPU knows it,
  // and asking would mean a readback in the frame path.
  IndShadowArgs,
  // ---- gas particles ----
  // FIXED extent: one thread per slot of BOTH spawn lists, whose caps are
  // compile-time constants. Threads past either list's live count return on
  // one load, so the constant dispatch buys a kernel that needs no CPU-side
  // knowledge of how many voxels left the window this tick (nothing on the CPU
  // knows, and asking would mean a readback in the tick path).
  GasSpawnSel,
  IndGasDispatchArgs,  // indirect: world.gasDispatchArgs @ 0
  // ONE WORKGROUP PER EMITTER. The count is CPU-known (the CPU built the list
  // this tick), so unlike the parcel dispatches this needs no indirect args
  // buffer and no readback — which is the whole reason the emitter list is a
  // CPU-side index rather than a GPU one.
  GasFarEmitSel,
  // One workgroup per LONG-RANGE emitter, same argument as the line above.
  GasFarWideSel,
  // ---- the clouds: one 8x8 workgroup per tile of the low-res target ----
  CloudGx,
  CloudGy,
  // ---- the ray-start map: one 8x8 workgroup per tile of the half-size
  // sample grid of the LARGEST target sized so far (ray_start.wgsl bounds
  // each thread against this frame's own size) ----
  RayStartGx,
  RayStartGy,
  IndSolArgs,        // indirect: world.solArgs @ 0 (one group per want-list entry)
};

// Max `uses` entries on any row. Asserted against the widest row at compile
// time in pass_table.cpp, so growing a row past this fails the build rather
// than silently truncating a hazard.
// Raised 10 -> 12 by the page table (PLAN_page_table.md §5.3): `ca` goes
// 9 -> 10 with R(PageTable) -> 11 with A(PageFaults), which exceeded the old
// ceiling and stopped the build, as the static_assert is meant to. Raised to
// 12 rather than 11 so the next row addition does not repeat it.
// Raised 12 -> 16 by the MPM seam: `ca` gains the excited-fluid coupling
// (R(FluidBlockMap) R(FluidGrid) A(FluidCellScratch)) -> 14 uses.
// Raised 16 -> 20 by the gas package: `ca` gains A(GasSpawn), the window-edge
// outbox, -> 17 uses. Four of headroom rather than one, for the reason the
// page-table note above gives.
inline constexpr int kMaxUses = 24;

struct Row {
  const char* name;
  // Which ComputePassEncoder this row is recorded into. Consecutive compute
  // rows sharing a group string go into ONE pass, exactly as today; a Fill or
  // Copy row (group nullptr) ends the open pass, because ClearBuffer and
  // CopyBufferToBuffer are encoder-level commands that cannot be recorded
  // inside a compute pass.
  //
  // This is deliberately part of the table rather than inferred: the pass
  // splits are the thing phase 2b must NOT change, and "prep is one pass,
  // integrate+args2 is one pass" is a fact about the recording that a reader
  // should be able to see without reconstructing it from adjacency. Under
  // Vulkan a compute pass has no meaning at all (barrier_graph §1.2), so this
  // field becomes a pure PassTimer label there.
  const char* group;
  Table table;
  Pipe pipe;
  Kind kind;
  // Compute: workgroup extents (x may be a DispatchSel selector).
  // Indirect: x is the args-buffer selector.
  // Copy:     x = srcOffset, y = dstOffset, z = size.
  // Fill:     x = offset, y = size in bytes; y == 0 means to the end of the
  //           buffer (every row but fill_gasSpawn's header clear is 0,0).
  uint32_t x, y, z;
  Groups groups;
  Dyn dyn;
  Cond cond;
  uint32_t repeat;
  Use uses[kMaxUses];
  int useCount;
};

// The expanded table, in record order. Rows for one Table are contiguous.
extern const Row* const kRows;
extern const int kRowCount;

// ---------------------------------------------------------- record context --
// Everything a row's Cond and DispatchSel resolve against, gathered by
// Simulation's Encode* functions and read by the recorder (vk::Recorder::
// CondHolds / Extent). ONE struct, used on both sides of the rhi seam.
//
// It used to be THREE — Simulation's file-local RecordCtx, rhi::TableCtx, and
// vk::RecordCtx — hand-copied field by field twice per RecordTable call, and
// three fields went missing across those copies at different times
// (denseWorldgen and caActive, then vizActive; see the git history of
// rhi_vk.cpp's RecordTableVulkan). A field that did not cross was silently its
// DEFAULT on the recorder side. With one plain struct there is no copy to
// forget. The defaults below are the recorder's safe side: CaActive defaults
// TRUE (the CA records unless proven idle), every count 0 (the row is skipped).
//
// Plain data, no Vulkan types, so src/sim and src/gpu both include it.
struct RecordCtx {
  uint32_t opsCount = 0;
  uint32_t cellCount = 0;
  uint32_t expCount = 0;
  uint32_t spawnCount = 0;
  uint32_t genCount = 0;
  uint32_t genColCount = 0;     // chunk-columns of the list: the `cols` pre-pass extent
  uint32_t farCount = 0;
  uint32_t fluidCount = 0;       // MLS-MPM particles alive AFTER this tick's spawns
  uint32_t fluidSpawnCount = 0;  // MLS-MPM spawn ops this tick
  // Chunk slots this tick's wind primitives want dirty-marked
  // (docs/RESEARCH_wind.md §4.3). Zero on every tick of a world with no fan in
  // it, which is what skips the windWake row entirely (Cond::WindWake).
  uint32_t windWakeCount = 0;
  // Water-body chunk-list entries this tick (docs/PLAN_water_master.md M2).
  // Zero whenever sim.waterBodyMode is 0 — the off switch is "no row is
  // recorded" (Cond::WaterBody), which is what keeps it an exact identity.
  uint32_t waterChunkCount = 0;
  // Reserved drain spawn-op BLOCKS this tick (M3, component 6). Zero at
  // sim.waterBodyMode 0 and whenever no body is proposed, so the discharge
  // row is not recorded and the shipped world cannot see it.
  uint32_t waterDrainBodies = 0;
  // M5: which body's container curve re-derives this tick, or any value >=
  // kWaterBodyCap (world.h) for "none" — which is every tick of a basin nobody
  // has dug into, and what leaves both sweep rows unrecorded (Cond::WaterSweep
  // tests `< kWaterBodyCap`). The default is "none" in the widest form so this
  // header need not include world.h.
  uint32_t waterSweepSlot = 0xFFFFFFFFu;
  // W2: sim.waveMode ANDed with "a body is listed this tick"
  // (docs/PLAN_water_relevel.md §4.2). 0 leaves the surface-momentum row
  // unrecorded (Cond::WaterWave), which is the exact-identity arm.
  uint32_t waveMode = 0;
  // Chunks the openness refresh walks this tick (render.opennessChunksPerFrame,
  // clamped, and gated on render.opennessStrength). 0 = Cond::Openness false
  // and NOTHING recorded, which is what makes the `noopenness` --render-budget
  // arm measure the pass as well as the reads.
  uint32_t opennessChunks = 0;
  // Chunks the glow refresh walks this tick (render.glowChunksPerFrame,
  // clamped, and zeroed when render.glowStrength is 0). Suppresses all three
  // glow rows (Cond::Glow), which is what makes the off switch exact.
  uint32_t glowChunks = 0;
  bool hashEnable = false;
  bool particlesActive = false;
  // False under --residency paged: worldgen's whole-world dispatch is replaced
  // by batched worldgenList submits (PLAN_page_table.md §3.5c).
  bool denseWorldgen = true;
  // False ONLY when the CPU can prove the dirty set is empty (ROADMAP_scale.md
  // §3.4). Drops compact + the args staging copy + all 54 CA iterations, which
  // is the whole of a settled tick's CA cost. Defaults TRUE so a caller that
  // never sets it records the CA exactly as before — the safe direction.
  bool caActive = true;
  // True only while the per-voxel activity overlay is on. Gates the ActVoxViz
  // write so the debug buffer costs nothing when the dev toggle is off.
  bool vizActive = false;
  // Gas particles (docs/PLAN_gas_particles.md). True when parcels are already
  // in flight OR the CA has work this tick — a voxel can only reach the window
  // edge from a chunk the CA is running, and a parcel already out there has to
  // be stepped whether or not any chunk is awake. See the latch in EncodeTick.
  bool gasActive = false;
  // Far fire-plume emitters the CPU handed the GPU this tick (world.h
  // kGasFarEmitMax). 0 = no frozen fire is in range, and then the splat row is
  // not recorded at all; it also decides, together with gasActive, whether the
  // density box is cleared (Cond::GasOuter).
  uint32_t gasFarEmitCount = 0;
  // ...and the LONG-RANGE emitters (world.h kGasFarOuterN). 0 = no fire is in
  // the 51.2 m..409.6 m band, and then neither the wide splat nor the wide
  // box's clear is recorded.
  uint32_t gasFarWideCount = 0;
  // Any loaded material authors a `repose` AND the CA has work this tick
  // (world.h kReposeSnap*). False for a materials.json with no repose line,
  // and then the snapshot prepass is not recorded at all. See the latch in
  // EncodeTick.
  bool reposeActive = false;
  // ---- the clouds (cloud.wgsl, DESIGN.md 9.w) — per-FRAME, ShadowCache table
  // bit 0 kCloudRecOn: the weather has something in the sky and weather.clouds
  //   is on, so the weather/shadow/env/march/resolve rows record (Cond::Clouds).
  // bit 1 kCloudRecBake: the noise volume has not been baked since the last
  //   pipeline build; the bake row records once (Cond::CloudBake).
  // bit 2 kCloudRecShadowCache: the voxel shadow cache is on
  //   (Cond::ShadowCacheOn).
  uint32_t cloudFlags = 0;
  // Workgroups (8x8) over the low-res cloud target, from the frame's size.
  uint32_t cloudGx = 0, cloudGy = 0;
  // Workgroups (8x8) over the ray-start map's sample grid; 0 = no buffer
  // yet, and then neither ray-start row records (Cond::RayStart).
  uint32_t rayStartGx = 0, rayStartGy = 0;
};

}  // namespace pass
