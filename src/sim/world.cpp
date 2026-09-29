#include "sim/world.h"

#include <algorithm>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gpu/resources.h"
#include "sim/farfeat.h"
#include "sim/pagetable.h"
#include "sim/pass_table.h"  // pass::Buf ids for the tracked readback copies
#include "sim/rng.h"
#include "sim/tuning.h"
#include "sim/worldmap.h"

// SANDVOX_DIRTY_REASONS=<n>: print the DIRTY_R_* histogram every n snapshots
// (see the fold in the snapshot builder below). 0 = off, which is the default
// and costs one compare per snapshot.
static const uint32_t kDirtyReasonEvery = [] {
  const char* e = getenv("SANDVOX_DIRTY_REASONS");
  const int v = e ? atoi(e) : 0;
  return v > 0 ? (uint32_t)v : 0u;
}();

// Readback slot layout (offsets in bytes).
constexpr uint64_t kChunkBytes = kChunkVol * 4;                 // 16 KB
constexpr uint64_t kMirrorBytes = 27 * kChunkBytes;             // 432 KB
constexpr uint64_t kDirtyOff = kMirrorBytes;
constexpr uint64_t kDirtyBytes = kNumSlots * 4;
constexpr uint64_t kActVoxVizWords = (uint64_t)kPoolPages * kChunkVol / 32;
constexpr uint64_t kActVoxVizBytes = kActVoxVizWords * sizeof(uint32_t);
constexpr uint64_t kOccOff = kDirtyOff + kDirtyBytes;
// The COUNT half of the occupancy buffer — the only half the snapshot ring
// carries. The sub-chunk bitmask that follows it on the GPU is render-only and
// no CPU reader wants it, so the staging slot stays exactly the size it was.
constexpr uint64_t kOccBytes = kNumSlots * 4;
// Render-only tail of the same GPU buffer; never staged, never read back.
constexpr uint64_t kSubOccBytes = (uint64_t)kNumSlots * kSubOccStride * 4;
constexpr uint64_t kHashOff = kOccOff + kOccBytes;
constexpr uint64_t kPickOff = kHashOff + 256;
constexpr uint64_t kPCountOff = kPickOff + 256;
// The particle-count block reserves 256 B and uses 16. The gas system's two
// small readbacks live in the slack rather than reflowing every offset below:
// 16 B of per-page live counts and the 32-byte gasSpawn header, which is this
// tick's gas counters (cleared before the CA, so each word is per-tick).
constexpr uint64_t kGasCountOff = kPCountOff + 16;   // 16 B
constexpr uint64_t kGasStatOff = kPCountOff + 64;    // kGasSpHdrBytes
static_assert(kGasStatOff + kGasSpHdrBytes <= kPCountOff + 256,
              "gas readback overruns the particle-count block's slack");
// The solute layer's header (world.h kSolM*: the two fault latches and the
// mass ledger) rides the same slack, after the gas header: its first 16
// words, 64 B. (It rode the page-fault block until that record grew to 64
// words for the reaction effects.)
constexpr uint64_t kSolMetaSnapOff = kPCountOff + 128;
// Words 0..31: the ledger, the latches and the per-species scoop ledger.
constexpr uint64_t kSolMetaSnapBytes = 128;
static_assert(kSolMScoopBySpecies + kSolScoopSpecies <= kSolMetaSnapBytes / 4,
              "the scoop ledger must ride the snapshot");
static_assert(kGasStatOff + kGasSpHdrBytes <= kSolMetaSnapOff &&
                  kSolMetaSnapOff + kSolMetaSnapBytes <= kPCountOff + 256,
              "solute header overruns the particle-count block's slack");
constexpr uint64_t kSupportOff = kPCountOff + 256;
constexpr uint64_t kSupportBytes = kNumSlots * 4;
constexpr uint64_t kPageFaultOff = kSupportOff + kSupportBytes;
// MLS-MPM fluid seam: fluidArgsStage (16 u32, the FA_* map) + the block list
// (kFluidBlocks u32). Small enough to ride every snapshot; the block list
// feeds PageTable::UpdateFluidChunks and the FA words feed the CPU's
// conservative live count + the splash sound cue.
constexpr uint64_t kFluidArgsOff = kPageFaultOff + 256;
constexpr uint64_t kFluidBlocksOff = kFluidArgsOff + 256;
constexpr uint64_t kFluidBlocksBytes = kFluidBlocks * 4;
// The excited-fluid mirror fold: one byte per mirror cell, packed 4/word.
constexpr uint64_t kFluidMirrorOff = kFluidBlocksOff + kFluidBlocksBytes;
constexpr uint64_t kFluidMirrorBytes = 27ull * kChunkVol;
constexpr uint64_t kFetchOff = kFluidMirrorOff + kFluidMirrorBytes;
// The per-chunk digest table (M9.3-A): kNumSlots digests + the trailing tick
// word. 128 KiB, which is why it sits at the END of the slot and not next to
// the world hash where it logically belongs: the snapshot parse bounces the
// dirty..support run (kDirtyOff..kSupportOff + kSupportBytes) through cached
// RAM in ONE memcpy, and putting the digest inside that run would grow that
// copy by 128 KiB per snapshot for a consumer that wants it on hash ticks
// only. Out here it is one straight memcpy out of the mapped slot.
constexpr uint64_t kChunkHashOff =
    kFetchOff + (uint64_t)World::kFetchPerTick * kChunkBytes;
constexpr uint64_t kSlotBytes = kChunkHashOff + kChunkHashBytes;

// Every WorldSnapshot the pipeline hands around is pre-sized: the readback
// callback memcpys straight into these arrays. One definition, so the published
// snapshot and the pooled ones cannot disagree about a length.
static void SizeSnapshot(WorldSnapshot& s) {
  if (s.mirror.size() == 27 * (size_t)kChunkVol) return;  // already sized
  s.mirror.assign(27 * kChunkVol, 0);
  s.dirtyFlags.assign(kNumSlots, 0);
  s.supportFlags.assign(kNumSlots, 0);
  s.occupancy.assign(kNumSlots, 0);
  s.occStain.assign(kNumSlots, 0);
  // chunkHash is NOT sized here: it is a shared, immutable table the parse
  // either copies fresh (hash ticks) or inherits from the previous snapshot.
  s.fluidBlocks.assign(kFluidBlocks, 0);
  s.fluidMirror.assign(27ull * kChunkVol, 0);
}

void World::Init(const rhi::Device& device) {
  using U = rhi::BufferUsage;
  // THE PAGE POOL. Sized by the residency mode: dense reserves one page per
  // slot (kNumSlots) so the identity map is address-identical to the
  // pre-paging buffer; paged reserves kPoolPages. This is the ONLY place the
  // pool size is decided, and PoolPages() is the ONLY reader of the mode.
  voxels = CreateBuffer(device, (uint64_t)PoolPages() * kChunkVol * 4,
                        U::Storage | U::CopySrc | U::CopyDst, "voxels");
  pageTable = CreateBuffer(device, (uint64_t)kNumSlots * 4,
                           U::Storage | U::CopySrc | U::CopyDst, "pageTable");
  pageFaults = CreateBuffer(device, kPageFaultBytes,
                            U::Storage | U::CopySrc | U::CopyDst, "pageFaults");
  // The solute layer (world.h's kSol* block). Created zeroed = every slot
  // SOL_EMPTY; ResetSolutes installs the free stack before the first tick.
  solTable = CreateBuffer(device, (uint64_t)kNumSlots * 4,
                          U::Storage | U::CopySrc | U::CopyDst, "solTable");
  solPool = CreateBuffer(device, (uint64_t)kSolutePoolPages * kSolWordsPerPage * 4,
                         U::Storage | U::CopySrc | U::CopyDst, "solPool");
  solMeta = CreateBuffer(device, (uint64_t)kSolMetaWords * 4,
                         U::Storage | U::CopySrc | U::CopyDst, "solMeta");
  solArgs = CreateBuffer(device, 12, U::Indirect | U::CopyDst, "solArgs");
  solStage = CreateBuffer(device, (uint64_t)kSolStageWords * 4,
                          U::Storage | U::CopySrc | U::CopyDst, "solStage");

  // The allocator + conservative dirty mirror + materialization rule. It
  // installs the initial table: the IDENTITY MAP in both modes, because
  // worldgen writes every slot and its post-pass compaction is what demotes
  // the all-air chunks (§3.5c). Under the identity map voxWordAt(c) resolves
  // to exactly voxels[cellIndexW(c)] — the same physical address — so the
  // whole translation path executes while producing bit-identical addresses to
  // pre-paging code, which is why --residency dense is the phase's oracle.
  pages = new PageTable();
  pages->Init(device, *this);
  dirty[0] = CreateBuffer(device, kDirtyBytes, U::Storage | U::CopySrc | U::CopyDst, "dirtyA");
  dirty[1] = CreateBuffer(device, kDirtyBytes, U::Storage | U::CopySrc | U::CopyDst, "dirtyB");
  dirtyList = CreateBuffer(device, kNumSlots * 4, U::Storage, "dirtyList");
  argsStage = CreateBuffer(device, 12, U::Storage | U::CopySrc | U::CopyDst, "argsStage");
  dispatchArgs = CreateBuffer(device, 12, U::Indirect | U::CopyDst, "dispatchArgs");
  // Counts first, then the sub-chunk bitmask (world.h kSubOccShift). One
  // buffer so every existing Occupancy barrier and bind-group entry covers the
  // mask too. Uninitialised content is not a hazard the way it would be for a
  // standalone buffer: every producer of the counts is also a producer of the
  // mask, and the first thing that ever runs over all kNumSlots slots is
  // worldgen `main` (or lr_occupancyFull on a load) — the same pass the counts
  // already depend on for not being garbage.
  occupancy = CreateBuffer(device, kOccBytes + kSubOccBytes,
                           U::Storage | U::CopySrc | U::CopyDst, "occupancy");
  support = CreateBuffer(device, kSupportBytes, U::Storage | U::CopySrc | U::CopyDst,
                         "supportFlags");
  hash = CreateBuffer(device, 16, U::Storage | U::CopySrc | U::CopyDst, "worldHash");
  // The per-chunk digest table (world.h kChunkHashWords). CopyDst so the
  // worldgen fill can zero it: nothing reads it before the first full
  // occupancy pass writes every slot, but a zeroed table makes "slot 7 has
  // never been hashed" a value instead of whatever the allocator left.
  chunkHash = CreateBuffer(device, kChunkHashBytes,
                           U::Storage | U::CopySrc | U::CopyDst, "chunkHash");
  quietTicks_.assign(kNumSlots, 0);
  tickUBO = CreateBuffer(device, sizeof(TickParams), U::Uniform | U::CopyDst, "tickUBO");
  passUBO = CreateBuffer(device, 54 * 256, U::Uniform | U::CopyDst, "passUBO");
  opsBuf = CreateBuffer(device, kMaxOpsPerTick * sizeof(BrushOp),
                        U::Storage | U::CopyDst, "brushOps");
  renderUBO = CreateBuffer(device, sizeof(RenderParams), U::Uniform | U::CopyDst, "renderUBO");
  cloudUBO = CreateBuffer(device, sizeof(CloudParams), U::Uniform | U::CopyDst, "cloudUBO");
  dirtyViz = CreateBuffer(device, kDirtyBytes, U::Storage | U::CopyDst, "dirtyViz");
  actVoxViz = CreateBuffer(device, kActVoxVizBytes, U::Storage | U::CopyDst, "actVoxViz");
  // Shadow cache (world.h kShadowCacheBuckets). CopyDst so a zero-fill can
  // reset it; never CopySrc — nothing reads it back, and a readback here would
  // put a fence in the frame path.
  shadowCache = CreateBuffer(device, kShadowCacheBytes, U::Storage | U::CopySrc | U::CopyDst,
                             "shadowCache");
  // CopySrc for the stats words only: --render-budget reads back the request
  // count and the overflow after a frame. Diagnostics, never the frame path.
  shadowReq = CreateBuffer(device, kShadowReqBytes,
                           U::Storage | U::CopySrc | U::CopyDst, "shadowReq");
  // The penumbra window (world.h kShadowHistBytes). A zero word is exactly
  // "nothing sampled yet", which is the state a freshly claimed slot is reset
  // to anyway, so the zero-initialized allocation IS the correct cold state.
  shadowHist = CreateBuffer(device, kShadowHistBytes, U::Storage | U::CopyDst,
                            "shadowHist");
  // RENDER_STATS counters (world.h kRenderStat*). CopySrc for the telemetry
  // readback; CopyDst so a reset path can zero it, though nothing needs to —
  // the CPU differences consecutive reads.
  renderStats = CreateBuffer(device, kRenderStatBytes,
                             U::Storage | U::CopySrc | U::CopyDst, "renderStats");
  // Openness grid (world.h kOpenFaces block). Zero-initialized allocation is
  // load-bearing for `opennessGen` and only for it: a zero stamp is "this slot
  // was never computed", so every reader falls back to the plain hemisphere
  // lerp until the pass has walked the chunk. `openness` itself needs no
  // initial value — no reader looks at a byte whose stamp does not match. Same
  // reliance farVox already has ("zero-initialized = air"), and EncodeLoadReset
  // re-clears the stamps so a new world cannot inherit the old one's.
  openness = CreateBuffer(device, kOpennessBytes,
                          U::Storage | U::CopySrc | U::CopyDst, "openness");
  opennessGen = CreateBuffer(device, kOpennessGenBytes,
                             U::Storage | U::CopySrc | U::CopyDst, "opennessGen");
  // Irradiance grid (world.h kIrradianceBytes). Zero = "no light has been seen
  // leaving this face", which is what an unlit face should read as, so the
  // zeroed allocation is the correct cold start and no reset path is needed:
  // a slot whose stamp does not match is skipped by the gather, and the walk
  // rewrites every word of a chunk it visits.
  irradiance = CreateBuffer(device, kIrradianceBytes,
                            U::Storage | U::CopySrc | U::CopyDst, "irradiance");
  // Glow field (world.h kGlowBytes block). The zero start is load-bearing in the
  // SAME one place opennessGen's is: the per-slot stamp at [slot*4+1]. Zero is
  // "never computed", so every reader returns no glow until sim_glow has walked
  // the chunk, and EncodeLoadReset re-zeroes the source region so a loaded world
  // cannot inherit the previous one's emitters. The 8 MiB field region needs no
  // initial value — no reader reaches a field word whose slot stamp mismatches.
  glow = CreateBuffer(device, kGlowBytes,
                      U::Storage | U::CopySrc | U::CopyDst, "glow");
  // The repose occupancy snapshot (world.h kReposeSnap* block). The zeroed
  // allocation is the correct cold start and needs no reset path anywhere: a
  // slot's stamp is `tick + 1`, so 0 reads as "no snapshot for this slot" and
  // every probe into it refuses until the prepass has written it. That is also
  // what makes a window shift safe without a stamp invalidation pass.
  reposeSnap = CreateBuffer(device, kReposeSnapBytes,
                            U::Storage | U::CopySrc | U::CopyDst, "reposeSnap");
  shadowArgsStage = CreateBuffer(device, 16, U::Storage | U::CopySrc | U::CopyDst,
                                 "shadowArgsStage");
  // Indirect ONLY, and out of every bind group — same rule as dispatchArgs.
  shadowArgs = CreateBuffer(device, 16, U::Indirect | U::CopyDst, "shadowArgs");
  pick = CreateBuffer(device, 32, U::Storage | U::CopySrc | U::CopyDst, "pick");
  // The water-body ledger AND (from M5) the measured container curves and split
  // maps that sit past the end of it — see the kWaterCurveBase block in
  // world.h for why they share one buffer rather than taking a binding. 60 KiB
  // of GPU-owned state, zeroed here and by EncodeLoadReset: a descriptor is a
  // description of a world, and a stale one after a worldgen/load reads as a
  // fresh one.
  waterBodyState = CreateBuffer(
      device, (uint64_t)kWaterBodyStateTotalWords * 4,
      U::Storage | U::CopySrc | U::CopyDst, "waterBodyState");
  // W2: the surface-momentum store (world.h kWaterFluxWords). The zeroed
  // allocation IS the correct cold start and needs no reset path, for
  // reposeSnap's reason: validity is a per-column STAMP carrying the tick and
  // the window page, so 0 reads as "no record here" from every reader and a
  // column that arrives in a reused slot after a window shift reads as empty
  // rather than inheriting the departed column's momentum.
  waterFlux = CreateBuffer(device, kWaterFluxBytes,
                           U::Storage | U::CopySrc | U::CopyDst, "waterFlux");

  particles[0] = CreateBuffer(device, (uint64_t)kParticleCap * 32, U::Storage, "particlesA");
  particles[1] = CreateBuffer(device, (uint64_t)kParticleCap * 32, U::Storage, "particlesB");
  particleCounts = CreateBuffer(device, 16, U::Storage | U::CopySrc | U::CopyDst,
                                "particleCounts");
  claim = CreateBuffer(device, (uint64_t)kClaimWords * 4, U::Storage | U::CopyDst, "claim");
  pArgsStage = CreateBuffer(device, 32, U::Storage | U::CopySrc, "pArgsStage");
  pDispatchArgs = CreateBuffer(device, 12, U::Indirect | U::CopyDst, "pDispatchArgs");
  drawArgs = CreateBuffer(device, 16, U::Indirect | U::CopyDst, "drawArgs");
  expOps = CreateBuffer(device, kMaxExplosionsPerTick * sizeof(ExplosionOp),
                        U::Storage | U::CopyDst, "explosionOps");
  expMask = CreateBuffer(device, (uint64_t)kMaxExplosionsPerTick * 68928 * 4,
                         U::Storage | U::CopyDst, "explosionMask");
  cellOps = CreateBuffer(device, kMaxCellOpsPerTick * sizeof(CellOp),
                         U::Storage | U::CopyDst, "cellOps");
  spawnOps = CreateBuffer(device, kMaxParticleSpawnsPerTick * sizeof(ParticleSpawn),
                          U::Storage | U::CopyDst, "spawnOps");
  sprites = CreateBuffer(device, kMaxSprites * sizeof(Sprite), U::Storage | U::CopyDst,
                         "sprites");

  // ---- gas particles (docs/PLAN_gas_particles.md stage 1) ----
  // CopySrc on gasCounts/gasSpawn/gasOuter is for the snapshot ring and the
  // gas gates; nothing on the frame path reads any of it synchronously.
  gasParticles[0] = CreateBuffer(device, (uint64_t)kGasParticleCap * 32,
                                 U::Storage | U::CopySrc, "gasParticlesA");
  gasParticles[1] = CreateBuffer(device, (uint64_t)kGasParticleCap * 32,
                                 U::Storage | U::CopySrc, "gasParticlesB");
  gasCounts = CreateBuffer(device, 16, U::Storage | U::CopySrc | U::CopyDst,
                           "gasCounts");
  gasClaim = CreateBuffer(device, (uint64_t)kGasClaimSize * 4,
                          U::Storage | U::CopyDst, "gasClaim");
  gasSpawn = CreateBuffer(
      device, (uint64_t)(kGasSpHdr + kGasSpawnPerTick * kGasSpStride) * 4,
      U::Storage | U::CopySrc | U::CopyDst, "gasSpawn");
  gasSpawnOps = CreateBuffer(
      device, (uint64_t)(kGasSpHdr + kGasCpuSpawnPerTick * kGasSpStride) * 4,
      U::Storage | U::CopyDst, "gasSpawnOps");
  gasArgs = CreateBuffer(device, 32, U::Storage | U::CopySrc, "gasArgs");
  gasDispatchArgs = CreateBuffer(device, 12, U::Indirect | U::CopyDst,
                                 "gasDispatchArgs");
  gasOuter = CreateBuffer(device, (uint64_t)kGasOuterWords * 4,
                          U::Storage | U::CopySrc | U::CopyDst, "gasOuter");
  // The far fire-plume emitter list (world.h kGasFarEmitMax). CopyDst only:
  // the CPU writes it and one kernel reads it; nothing copies it back.
  gasFarEmit = CreateBuffer(device, (uint64_t)kGasFarEmitWords * 4,
                            U::Storage | U::CopyDst, "gasFarEmit");
  // The long-range density box (world.h kGasFarOuterN). CopySrc for the gate,
  // CopyDst for the per-tick clear, exactly like gasOuter.
  gasFarOuter = CreateBuffer(device, (uint64_t)kGasFarOuterWords * 4,
                             U::Storage | U::CopySrc | U::CopyDst, "gasFarOuter");

  // MLS-MPM fluid (world.h fluid block). CopySrc on the particle pair is for
  // the fluid gates' mass audits; the frame path reads back only the small
  // fluidArgsStage + block list through the snapshot ring.
  fluidParticles[0] = CreateBuffer(device,
                                   (uint64_t)kFluidCap * kFluidParticleWords * 4,
                                   U::Storage | U::CopySrc, "fluidParticlesA");
  fluidParticles[1] = CreateBuffer(device,
                                   (uint64_t)kFluidCap * kFluidParticleWords * 4,
                                   U::Storage | U::CopySrc, "fluidParticlesB");
  fluidSpawnOps = CreateBuffer(device, kMaxFluidSpawnsPerTick * sizeof(FluidSpawnOp),
                               U::Storage | U::CopyDst, "fluidSpawnOps");
  // TWO kNumSlots arrays: [slot] = blockIdx+1, [kNumSlots + slot] = the
  // chunk's 16-bit Y-OCCUPANCY mask (world.h). The whole-buffer Fill at the
  // head of PT_FLUIDMAP clears both halves, which is what the mask needs.
  fluidBlockMap = CreateBuffer(device, (uint64_t)kNumSlots * 2 * 4,
                               U::Storage | U::CopyDst, "fluidBlockMap");
  fluidBlockList = CreateBuffer(device, (uint64_t)kFluidBlocks * 4,
                                U::Storage | U::CopySrc, "fluidBlockList");
  // 8 i32 words per node (FLUID_GW in sim_fluid.wgsl): mass, momentum xyz,
  // per-species mass x3, foam — 32 MiB of per-substep scratch at 256 blocks.
  fluidGrid = CreateBuffer(device, (uint64_t)kFluidBlocks * kChunkVol * 32,
                           U::Storage, "fluidGrid");
  // The FA_* word map (kFluidArgsWords, world.h). CopyDst: the seam relies on a
  // zeroed live count after worldgen/reset (fill-cleared there).
  fluidArgsStage = CreateBuffer(device, kFluidArgsBytes,
                                U::Storage | U::CopySrc | U::CopyDst,
                                "fluidArgsStage");
  fluidDispatchArgs = CreateBuffer(device, 12, U::Indirect | U::CopyDst,
                                   "fluidDispatchArgs");
  fluidPDispatchArgs = CreateBuffer(device, 12, U::Indirect | U::CopyDst,
                                    "fluidPDispatchArgs");
  // Seam scratch (layouts documented at the members in world.h). The excite
  // scratch is 16 header words + counts + bases + the slot list; the settle
  // scratch is speed maxima + marks + the settle list + the bins.
  // CopySrc is not decoration: pass_table's `copy_exciteArgs` row stages the
  // emit dispatch's args OUT of this buffer's header (TR(FluidExciteScratch)
  // -> TW(FluidPDispatchArgs)), so it is a transfer SOURCE. Missing since the
  // seam landed and invisible until now, because --vk-smoke's two scenarios
  // never had live fluid and PT_FLUIDSEAM is only recorded when they do;
  // WP5's flip of sim.fluidExciteMode is what finally gave the smoke water to
  // excite, and validation answered immediately with 10x
  // VUID-vkCmdCopyBuffer-srcBuffer-00118.
  fluidExciteScratch = CreateBuffer(device,
                                    (uint64_t)(16 + 3 * kNumSlots) * 4,
                                    U::Storage | U::CopySrc | U::CopyDst,
                                    "fluidExciteScratch");
  fluidCalm = CreateBuffer(device, (uint64_t)kNumSlots * 4,
                           U::Storage | U::CopyDst, "fluidCalm");
  // ... + 2: SP_LIVEFLAG / SP_LIVEPREV, the settle half's sleep flags
  // (sim_fluid_seam.wgsl SP_SCRATCH_WORDS is the layout's truth).
  fluidSettleScratch = CreateBuffer(
      device,
      (uint64_t)(2 * kNumSlots + 16 + 2 + kFluidSettleMax * kChunkVol * 2 +
                 kFluidSettleMax * 8 + 2) * 4,
      U::Storage | U::CopyDst, "fluidSettleScratch");
  // THREE arrays of one word per 256-particle compaction span, not two:
  // [0..SPANS) survivors, [SPANS..2*SPANS) their exclusive bases, and
  // [2*SPANS..3*SPANS) the EXCITE-ORIGIN survivors among them — the population
  // sim.fluidExciteCeiling is charged against (sim_fluid_seam.wgsl compactCount
  // / the budget block in exciteScan).
  fluidCompactScratch = CreateBuffer(device, (uint64_t)(kFluidCap / 256) * 3 * 4,
                                     U::Storage, "fluidCompactScratch");
  fluidCellScratch = CreateBuffer(
      device, (uint64_t)kFluidBlocks * kChunkVol * 2 * 4,
      U::Storage | U::CopyDst, "fluidCellScratch");
  fluidMirror = CreateBuffer(device, 27ull * kChunkVol,
                             U::Storage | U::CopySrc, "fluidMirror");
  debugBoxes = CreateBuffer(device, (uint64_t)kMaxDebugBoxes * sizeof(DebugBox),
                            U::Storage | U::CopyDst, "debugBoxes");
  bodyInstances = CreateBuffer(device, 262144ull * 16, U::Storage | U::CopyDst,
                               "bodyInstances");
  bodyXforms = CreateBuffer(device, (uint64_t)kMaxBodySlots * 32,
                            U::Storage | U::CopyDst, "bodyXforms");
  genList = CreateBuffer(device, kNumSlots * 4, U::Storage | U::CopyDst, "genList");
  // The generation verdict (world.h kGenVerdict*: the deferred wake's act bit
  // plus the chunk's page-table class). CopySrc because Stream reads it back —
  // one small copy per window shift, never mapped in the frame path — and so
  // does the batched worldgen, 8 KiB per batch instead of 32 MiB of words.
  // kGenActEntries covers a shift plane and a worldgen batch.
  genAct = CreateBuffer(device, (uint64_t)kGenActEntries * 4,
                        U::Storage | U::CopyDst | U::CopySrc, "genAct");
  // JITTER page materialization gets its OWN list, deliberately NOT genList.
  // Two u32 per entry (slot, sentinel entry) against genList's one, and — the
  // reason it cannot be shared — Stream::FillSlots writes genList MID-FRAME
  // while a page fill drains at the head of the next command buffer, so the two
  // deferred writes interleave. Sharing produced a stale-list read that
  // diverged the world hash only after a window shift (loud smoke ticks 86/88).
  pageFillList = CreateBuffer(device, (uint64_t)kNumSlots * 8,
                              U::Storage | U::CopyDst, "pageFillList");

  // Far-field cascades (render-only LOD). Zero-initialized = air, so unfilled
  // regions render as sky, never garbage.
  // CopySrc is selftest-only (the phase-2 downsample gate reads back one word);
  // the frame path stays readback-free per CLAUDE.md.
  farVox = CreateBuffer(device, (uint64_t)kFarLevels * kFarVox,
                        U::Storage | U::CopySrc, "farVox");
  // shadow_resolve.wgsl SKY_TOP_BASE indexes past the counts; FAR_LEVELS (8)
  // of these words are used, the rest are headroom.
  constexpr uint32_t kFarSkyTopWords = 16;
  static_assert(kFarLevels <= kFarSkyTopWords, "sky-bound tail too small");
  // + kFarSkyTopWords: the per-level SKY BOUND tail (shadow_resolve.wgsl
  // skyTopReduce, read by raymarch.wgsl traceFar/farShadowDist). Zero = "this
  // level holds nothing", which is also what an unfilled level is.
  farOcc = CreateBuffer(device,
                        ((uint64_t)kFarLevels * kFarNumChunks + kFarSkyTopWords) * 4,
                        U::Storage, "farOcc");
  farList = CreateBuffer(device, kFarListCap * 4, U::Storage | U::CopyDst, "farList");
  farUBO = CreateBuffer(device, sizeof(FarParams), U::Uniform | U::CopyDst, "farUBO");
  // Edit patches for the cascade fill. The header half is rewritten by every
  // PrepareTick that dispatches fills (2 u32 per entry, <= 32 KiB), so the
  // kernel never reads an uninitialized count — this buffer deliberately does
  // NOT lean on zero-initialized allocation the way farVox does.
  farPatch = CreateBuffer(device, (uint64_t)kFarPatchWords * 4,
                          U::Storage | U::CopyDst, "farPatch");
  farSig = CreateBuffer(device, (uint64_t)kNumSlots * 4, U::Storage | U::CopyDst,
                        "farSig");
  // Zero-initialised = every entry INVALID (kFarMapValid clear), which is the
  // safe state: the renderer refines nothing it was not told about.
  // + the FEATURE plane after it (sim/farfeat.h, LOD-seam package F): the
  // levels 1..kFarFeatLevels thin-feature words, zero = no feature.
  farMap = CreateBuffer(device, (kFarMapWords + kFarFeatWords) * 4,
                        U::Storage | U::CopySrc, "farMap");

  for (auto& s : slots_) {
    s.buf = CreateBuffer(device, kSlotBytes, U::MapRead | U::CopyDst, "readback");
    s.inFlight = false;
  }
  SizeSnapshot(snap_);

  // SANDVOX_GPUMEM=1 prints the buffer budget. Here rather than at exit because
  // this is where every window- and cascade-sized allocation has just happened
  // (the page pool, the far-field levels, the fluid scratch), and those are the
  // ones a sizing decision turns on.
  if (getenv("SANDVOX_GPUMEM")) DumpGpuBufferBudget("after World::Init");
}

const char* World::FetchSourceName(FetchSource s) {
  switch (s) {
    case FetchSource::IslandScan: return "island";
    case FetchSource::Terrain: return "terrain";
    case FetchSource::Mob: return "mob";
    case FetchSource::Settle: return "settle";
    default: return "other";
  }
}

void World::RequestChunkFetch(IVec3 worldChunk, FetchSource src) {
  const int si = (int)src < FetchProbe::kSources ? (int)src : 0;
  if (!ChunkInWindow(worldChunk)) {  // not resident: nothing to read
    fetchProbe_.refused[si]++;
    return;
  }
  uint64_t key = PackChunkKey(worldChunk);
  if (fetchQueued_.count(key)) {
    fetchProbe_.coalesced[si]++;
    return;
  }
  fetchQueued_[key] = 1;
  fetchQueue_.push_back(FetchReq{worldChunk, fetchTick_, (uint8_t)si});
  fetchProbe_.requests[si]++;
  if (fetchQueue_.size() > fetchProbe_.depthMax)
    fetchProbe_.depthMax = (uint32_t)fetchQueue_.size();
}

std::string World::FetchReport() const {
  const FetchProbe& p = fetchProbe_;
  uint64_t req = 0, coal = 0, ref = 0, car = 0, drop = 0, age = 0;
  uint32_t ageMax = 0;
  for (int i = 0; i < FetchProbe::kSources; i++) {
    req += p.requests[i];
    coal += p.coalesced[i];
    ref += p.refused[i];
    car += p.carried[i];
    drop += p.dropped[i];
    age += p.ageSum[i];
    ageMax = std::max(ageMax, p.ageMax[i]);
  }
  char buf[640];
  int n = std::snprintf(
      buf, sizeof buf,
      "fetch fifo since tick %u: %llu queued (%llu coalesced, %llu refused "
      "non-resident), %llu carried in %u slots, %llu dropped streamed-out; age "
      "ticks mean %.2f max %u; depth max %u, %u slots drained full, left "
      "waiting mean %.1f max %u; by source",
      p.sinceTick, (unsigned long long)req, (unsigned long long)coal,
      (unsigned long long)ref, (unsigned long long)car, p.drains,
      (unsigned long long)drop, car ? (double)age / (double)car : 0.0, ageMax,
      p.depthMax, p.drainsFull,
      p.drains ? (double)p.leftSum / (double)p.drains : 0.0, p.leftMax);
  std::string s(buf, n > 0 ? (size_t)std::min(n, (int)sizeof buf - 1) : 0);
  for (int i = 0; i < FetchProbe::kSources; i++) {
    if (p.requests[i] == 0 && p.coalesced[i] == 0 && p.refused[i] == 0)
      continue;
    n = std::snprintf(buf, sizeof buf,
                      " %s %llu (+%llu coalesced, %llu refused) age %.2f/%u",
                      FetchSourceName((FetchSource)i),
                      (unsigned long long)p.requests[i],
                      (unsigned long long)p.coalesced[i],
                      (unsigned long long)p.refused[i],
                      p.carried[i] ? (double)p.ageSum[i] / (double)p.carried[i]
                                   : 0.0,
                      p.ageMax[i]);
    if (n > 0) s.append(buf, (size_t)std::min(n, (int)sizeof buf - 1));
  }
  return s;
}

const CachedChunk* World::Cached(IVec3 worldChunk) const {
  auto it = cache_.find(PackChunkKey(worldChunk));
  return it == cache_.end() ? nullptr : &it->second;
}

// ---- THE RING DEPTH, AS AN A/B ARM IN ONE BINARY (P2-D) -------------------
//
// kReadbackSlots is derived from the pipeline depth (world.h) and that is the
// shipping value. This caps how many of those slots may be OCCUPIED, so the
// old 3-slot behaviour and the derived 16-slot behaviour are two arms of the
// SAME executable rather than two builds -- the same argument as the
// "an #if-guarded SIMD path tests only itself" note: a differential measured
// across two binaries on a shared machine measures the binaries too. The
// buffers for all kSlots are allocated either way; a capped run just leaves
// the tail of the ring unused, which costs address space and nothing else.
static int ActiveReadbackSlots() {
  static const int n = [] {
    if (const char* e = std::getenv("SANDVOX_READBACK_SLOTS")) {
      const long v = std::strtol(e, nullptr, 10);
      if (v >= 1 && v <= World::kReadbackSlots) {
        std::printf("[snap] SANDVOX_READBACK_SLOTS=%ld (of %d)\n", v,
                    World::kReadbackSlots);
        return (int)v;
      }
    }
    return World::kReadbackSlots;
  }();
  return n;
}

bool World::EncodeReadbacks(const rhi::Device&, const rhi::CommandEncoder& enc,
                            IVec3 playerChunkBase, uint32_t particleLivePage,
                            uint32_t tick, bool hashTick, bool fluidRecorded) {
  // A TICK REWIND FLUSHES THE PIPELINE. The harness runs many scenes through
  // one World and each restarts its own tick base (kOrder, test/selftest.cpp),
  // so `tick` is monotonic WITHIN a scene and not across them. Unsigned
  // arithmetic would read a leftover snapshot stamped 5040 against a new base
  // of 3 as "four billion ticks in the future" and never publish again -- and
  // that snapshot describes a world the new scene has usually re-generated
  // anyway. Bumping the epoch drops it, which is exactly what a reset does.
  if (haveEncodeTick_ && tick < lastEncodeTick_) InvalidateSnapshot();
  lastEncodeTick_ = tick;
  haveEncodeTick_ = true;
  // Counted BEFORE the decline below (see TicksEncoded): a tick that gets no
  // copy must leave a hole in submitSeq, or the save path could not tell it
  // happened.
  const uint32_t seq = ++ticksEncoded_;
  int slot = -1;
  const int active = ActiveReadbackSlots();
  for (int i = 0; i < active; i++) {
    if (!slots_[i].inFlight) { slot = i; break; }
  }
  // A DECLINE BREAKS THE CONSTANT. The tick that finds no free slot gets no
  // copy, so the publish at T + kSnapshotLatency has nothing to hand over and
  // the latency stops being a property of the build. SubmitTick waits on
  // ReadbackSlotFree() before it calls this, so reaching here means the ring
  // is genuinely exhausted with nothing outstanding to wait for -- counted,
  // and asserted zero by the snapshot-latency gate.
  if (slot < 0) {
    snapPipe_.declines++;
    return false;
  }
  Slot& s = slots_[slot];
  s.particleLivePage = particleLivePage;
  s.tick = tick;
  s.seq = seq;
  s.origin = origin_;

  // drain queued chunk fetches into this slot (bounded per tick); anything
  // that streamed out since it was queued just drops
  s.fetchIds.clear();
  while (!fetchQueue_.empty() && s.fetchIds.size() < kFetchPerTick) {
    const FetchReq r = fetchQueue_.front();
    fetchQueue_.pop_front();
    fetchQueued_.erase(PackChunkKey(r.wc));
    const int si = r.src < FetchProbe::kSources ? r.src : 0;
    if (!ChunkInWindow(r.wc)) {
      fetchProbe_.dropped[si]++;
      continue;
    }
    const uint32_t age = tick >= r.tick ? tick - r.tick : 0u;
    fetchProbe_.carried[si]++;
    fetchProbe_.ageSum[si] += age;
    if (age > fetchProbe_.ageMax[si]) fetchProbe_.ageMax[si] = age;
    s.fetchIds.push_back(r.wc);
  }
  // The stamp every request made from here until the next slot carries; a
  // request that rides the very next slot therefore ages exactly 1.
  fetchTick_ = tick;
  fetchProbe_.drains++;
  if (s.fetchIds.size() >= kFetchPerTick && !fetchQueue_.empty())
    fetchProbe_.drainsFull++;
  fetchProbe_.leftSum += fetchQueue_.size();
  if (fetchQueue_.size() > fetchProbe_.leftMax)
    fetchProbe_.leftMax = (uint32_t)fetchQueue_.size();
  // Every copy below is TRACKED (rhi::CommandEncoder::CopyTracked): the sources
  // are pass-table buffers written by the tick rows earlier in this SAME
  // command buffer, so each copy must declare its read to the generated-barrier
  // tracker (vk_record.h §3.3) — an untracked CopyBufferToBuffer here reads
  // whatever the GPU happens to have written, with no barrier ordering it.
  //
  // THE CPU SEAM (§2.1a): the source offset resolves through PageOffsetOfSlot,
  // never through slot * kChunkBytes. A sentinel slot is SKIPPED entirely and
  // its 4,096 words are synthesized CPU-side when the snapshot is consumed —
  // strictly cheaper than today, since a sentinel chunk costs a 4-byte table
  // read instead of a 16 KiB GPU->CPU copy, which also reduces the readback
  // traffic kFetchPerTick exists to bound.
  s.fetchSentinel.assign(s.fetchIds.size(), 0u);
  for (size_t i = 0; i < s.fetchIds.size(); i++) {
    const uint32_t slotIdx = SlotChunkIndex(s.fetchIds[i]);
    const uint64_t off = PageOffsetOfSlot(slotIdx);
    if (off == kNoPage) {
      s.fetchSentinel[i] = PageEntryOfSlot(slotIdx);
      continue;
    }
    enc.CopyTracked(pass::Buf::Voxels, voxels, off, s.buf,
                    kFetchOff + i * kChunkBytes, kChunkBytes);
  }

  // clamp the 3x3x3 mirror to the residency window (world chunk coords).
  // MirrorBaseFor is the ONE clamp — the seam's mirrorFold kernel gets the
  // same value through TickParams.mirrorBase, or the fluid-occupancy fold
  // and the voxel mirror would describe two different cubes.
  s.base = MirrorBaseFor(playerChunkBase);

  // THE CPU SEAM again, and this is the worst of the five sites (§2.1a): the
  // mirror is CPU-only collision data, so a corrupted mirror is the player
  // falling through the floor with a CORRECT world hash. Nothing in the
  // determinism gate can catch it. Sentinel slots are skipped and synthesized
  // on consumption, exactly like the fetch above.
  s.mirrorSentinel.fill(0u);
  for (int dz = 0; dz < 3; dz++)
    for (int dy = 0; dy < 3; dy++)
      for (int dx = 0; dx < 3; dx++) {
        uint32_t ci = SlotChunkIndex({s.base.x + dx, s.base.y + dy, s.base.z + dz});
        size_t m = (size_t)((dz * 3 + dy) * 3 + dx);
        uint64_t dst = (uint64_t)m * kChunkBytes;
        uint64_t off = PageOffsetOfSlot(ci);
        if (off == kNoPage) {
          s.mirrorSentinel[m] = PageEntryOfSlot(ci);
          continue;
        }
        enc.CopyTracked(pass::Buf::Voxels, voxels, off, s.buf, dst, kChunkBytes);
      }
  // dirty buffer note: caller copies the *next-tick* dirty buffer; we take a
  // buffer reference at encode time via these explicit copies instead.
  enc.CopyTracked(pass::Buf::Occupancy, occupancy, 0, s.buf, kOccOff, kOccBytes);
  enc.CopyTracked(pass::Buf::Hash, hash, 0, s.buf, kHashOff, 16);
  // The per-chunk digest table: 128 KiB, copied ONLY when this tick can have
  // rewritten it. It used to ride every snapshot on the argument that the
  // trailing tick word tells the reader which pass it is looking at — true,
  // and it cost 128 KiB of DMA plus a 128 KiB memcpy per tick to re-deliver a
  // table that changes on one tick in fifteen (the game's hash cadence). The
  // in-tick writer is the full occupancy pass (C_HASH, i.e. `hashTick`); the
  // out-of-tick writers — worldgen's fill and the load reset, both behind
  // InvalidateSnapshot, and the hash-only pass behind NoteChunkHashWritten —
  // raise chunkHashForce_. A slot that skips the copy inherits the previous
  // snapshot's table at parse time, which is byte-for-byte what the buffer
  // still holds, so ChunkHashOfSlot/ChunkHashTick read exactly what they did.
  s.hashCopied = hashTick || chunkHashForce_;
  if (s.hashCopied) {
    enc.CopyTracked(pass::Buf::ChunkHash, chunkHash, 0, s.buf, kChunkHashOff,
                    kChunkHashBytes);
    chunkHashForce_ = false;
  }
  enc.CopyTracked(pass::Buf::Pick, pick, 0, s.buf, kPickOff, 32);
  enc.CopyTracked(pass::Buf::ParticleCounts, particleCounts, 0, s.buf, kPCountOff, 16);
  // Gas: the live per-page counts and the 8-word counter header. Async and one
  // tick latent like every other row here — no gas path anywhere reads back
  // synchronously on the frame path.
  enc.CopyTracked(pass::Buf::GasCounts, gasCounts, 0, s.buf, kGasCountOff, 16);
  enc.CopyTracked(pass::Buf::GasSpawn, gasSpawn, 0, s.buf, kGasStatOff,
                  kGasSpHdrBytes);
  // support-loss flags are one-shot: consume into this slot, then clear so the
  // next window of ticks accumulates fresh flags (no readback = they persist).
  // The copy-then-clear pair is a genuine transfer WAR (barrier_graph §7.4);
  // routing the fill through the tracker is what makes it fall out on Vulkan.
  enc.CopyTracked(pass::Buf::Support, support, 0, s.buf, kSupportOff, kSupportBytes);
  enc.FillTracked(pass::Buf::Support, support);
  // The page-fault counter rides the ring (risk 1's residual mitigation). No
  // clear-after-copy, unlike `support`: the counter is MONOTONIC and a non-zero
  // value is a permanent "this build has a bug" latch, which is the semantics
  // wanted. That is what makes the detector work in ordinary play rather than
  // only under test.
  enc.CopyTracked(pass::Buf::PageFaults, pageFaults, 0, s.buf, kPageFaultOff,
                  kPageFaultBytes);
  // The solute layer's header (world.h kSolM*): the two fault latches and
  // the mass ledger, 64 B in the page-fault block's slack.
  enc.CopyTracked(pass::Buf::SolMeta, solMeta, 0, s.buf, kSolMetaSnapOff,
                  kSolMetaSnapBytes);
  // MLS-MPM fluid seam: the live count + event counters and the active block
  // list. 1.3 KB per snapshot; the block list is what keeps every chunk the
  // seam may write materialized (PageTable::UpdateFluidChunks).
  enc.CopyTracked(pass::Buf::FluidArgsStage, fluidArgsStage, 0, s.buf,
                  kFluidArgsOff, kFluidArgsBytes);
  enc.CopyTracked(pass::Buf::FluidBlockList, fluidBlockList, 0, s.buf,
                  kFluidBlocksOff, kFluidBlocksBytes);
  // The swimming fold (108 KiB) only when the seam was RECORDED this tick —
  // the fold pass is its last row, so a tick without the seam leaves the
  // buffer as it was and there is no live fluid for it to describe (the
  // seam's recording predicate covers every live particle; see
  // Simulation::EncodeTick). The parse treats a skipped copy as "no water",
  // the zero-fill it already applied whenever fluidLive read 0.
  s.fluidCopied = fluidRecorded;
  if (fluidRecorded)
    enc.CopyTracked(pass::Buf::FluidMirror, fluidMirror, 0, s.buf,
                    kFluidMirrorOff, kFluidMirrorBytes);
  lastSlot_ = slot;
  return true;
}

void World::EncodeDirtyCopy(const rhi::CommandEncoder& enc, const rhi::Buffer& dirtyNext) {
  if (lastSlot_ < 0) return;
  // dirtyNext is Simulation::DirtyNext() — the buffer the tick just encoded
  // writes as "active next tick", i.e. the table's symbolic DirtyOut (the page
  // has not flipped yet at this point in SubmitTick).
  enc.CopyTracked(pass::Buf::DirtyOut, dirtyNext, 0, slots_[lastSlot_].buf, kDirtyOff,
                  kDirtyBytes);
}

void World::KickReadback() {
  if (lastSlot_ < 0) return;
  Slot& s = slots_[lastSlot_];
  s.inFlight = true;
  s.epoch = snapEpoch_;
  int slot = lastSlot_;
  const uint32_t epoch = snapEpoch_;
  lastSlot_ = -1;
  rhi::MapReadAsync(
      s.buf, 0, kSlotBytes, [this, slot, epoch](const void* mapped) {
        Slot& sl = slots_[slot];
        // The slot is free the moment its bytes are in hand, and it is released
        // BEFORE any early return below: the fixed-latency pipeline may never
        // decline a readback (World::ReadbackSlotFree), so a slot that a failed
        // map or a dead epoch left marked in-flight would eventually stall a
        // tick that is contractually owed a snapshot.
        sl.inFlight = false;
        // A world reset -- or a harness scene restarting its tick counter --
        // since this copy was encoded makes these bytes a description of a DEAD
        // WORLD, whose stamp can read as newer than the new world's early
        // ticks. The epoch is what says so; see World::InvalidateSnapshot.
        if (epoch != snapEpoch_) {
          snapPipe_.dropped++;
          return;
        }
        const uint8_t* p = (const uint8_t*)mapped;
        if (!p) return;
        // ---- PARSE INTO A HOLDING SNAPSHOT, NEVER STRAIGHT INTO snap_ ------
        //
        // This callback fires whenever a fence retires, and ProcessEvents fires
        // every ready map at once, so on a fast GPU several ticks' snapshots
        // land in one pump. Writing them into snap_ as they arrive is exactly
        // what made the CPU's world view a function of GPU timing (finding L1).
        // They queue here in tick order instead and SubmitTick publishes the
        // one tick T is owed -- T - kSnapshotLatency, exactly.
        WorldSnapshot out;
        if (!snapPool_.empty()) {
          out = std::move(snapPool_.back());
          snapPool_.pop_back();
        }
        SizeSnapshot(out);
        // Set before the parse: the SANDVOX_DIRTY_REASONS diagnostic below
        // stamps its line with it.
        out.tick = sl.tick;
        out.submitSeq = sl.seq;
        std::memcpy(out.mirror.data(), p, kMirrorBytes);
        // Sentinel chunks were never copied (§2.1a); synthesize their words
        // now, through the SAME rule the shader uses. SynthWord (world.h)
        // and synthWord (common.wgsl) are the two halves of one contract —
        // the page-roundtrip gate asserts they agree.
        //
        // A JITTER sentinel is POSITIONAL, so its cells cannot be one
        // repeated word: each takes the palette variant for its own world
        // coordinate. The mirror knows the world chunk of every one of its
        // 27 slots from sl.base, which is what makes that reconstructible
        // here. Getting this wrong would be invisible to the world hash and
        // would show up only as the player colliding with the wrong thing —
        // the mirror is CPU-only collision data.
        for (size_t m = 0; m < sl.mirrorSentinel.size(); m++) {
          const uint32_t e = sl.mirrorSentinel[m];
          if (e == 0u) continue;  // a real copy landed for this cell
          const int mx = (int)(m % 3), my = (int)((m / 3) % 3),
                    mz = (int)(m / 9);
          const IVec3 wc{sl.base.x + mx, sl.base.y + my, sl.base.z + mz};
          // Air (whatever the JITTER bit says) / UNIFORM / JITTER in row
          // order: world.h SynthChunkWords, the one whole-chunk form of
          // SynthWordAt, shared with the fetch cache below.
          SynthChunkWords(e, wc, mirrorSeed_,
                          out.mirror.data() + m * kChunkVol);
        }
        out.mirrorBase = sl.base;
        out.windowOrigin = sl.origin;
        // ---- ONE sequential copy of the per-chunk arrays -------------
        // `p` is the persistently mapped readback slot. The mirror above
        // is already memcpy'd out before it is touched; dirty/occ/support
        // never were, and were scanned a word at a time (3 x 32,768 words
        // = 384 KiB) straight out of mapped host memory, with occW[i]
        // re-loaded three times per iteration. That is the exact consumer
        // Stream::HarvestEvict warns about ("One sequential copy out of
        // write-combined map memory; Classify then reads cached RAM").
        //
        // dirty, occ, the hash/pick/pcount block and support are
        // contiguous in the slot layout at the top of this file, so ONE
        // copy covers all of them. Rebasing by kDirtyOff lets every reader
        // below keep its original `+ kXxxOff` form.
        constexpr uint64_t kBounceBytes =
            (kSupportOff + kSupportBytes) - kDirtyOff;
        if (snapBounce_.size() < kBounceBytes) snapBounce_.resize(kBounceBytes);
        std::memcpy(snapBounce_.data(), p + kDirtyOff, kBounceBytes);
        const uint8_t* b = snapBounce_.data() - kDirtyOff;

        const uint32_t* dirtyW = (const uint32_t*)(b + kDirtyOff);
        const uint32_t* occW = (const uint32_t*)(b + kOccOff);
        const uint32_t* supW = (const uint32_t*)(b + kSupportOff);
        uint32_t active = 0;
        uint32_t reasonOr = 0;
        uint64_t total = 0;
        for (uint32_t i = 0; i < kNumSlots; i++) {
          // See WorldSnapshot::dirtyFlags: MUTATE-only is its own value.
          out.dirtyFlags[i] = dirtyW[i] == 0 ? 0
                              : dirtyW[i] == kDirtyReasonMutate ? kDirtyMutateOnly
                                                                : 1;
          active += dirtyW[i] != 0 ? 1u : 0u;
          reasonOr |= dirtyW[i];
          // GPU word packs [31] anyStain | [30..16] blockers | [15..0]
          // nonAir (packOccStain, common.wgsl). Existing CPU consumers
          // (streaming evict, voxelTotal) want the non-air COUNT, so that
          // stays the stored value — but the STAIN FLAG is carried across
          // in its own array rather than masked away, because the page
          // table's free path needs it and reading it back per candidate
          // was costing a blocking WaitIdle + 16 KiB per chunk.
          const uint32_t o = occW[i];          // ONE load, was three
          const uint32_t nonAir = o & 0xFFFFu;
          out.occupancy[i] = nonAir;
          out.occStain[i] = (o >> 31) & 1u;
          total += nonAir;
          // Folded in from its own second pass over the same 32,768
          // chunks — safe now that all three streams are cached RAM.
          out.supportFlags[i] = supW[i] != 0 ? 1 : 0;
        }
        out.activeChunks = active;
        out.dirtyReasonOr = reasonOr;
        out.watchReason = dirtyWatch_ < kNumSlots ? dirtyW[dirtyWatch_] : 0u;
        out.voxelTotal = total;
        // ---- SANDVOX_DIRTY_REASONS=<n>: WHY are these chunks awake? ----
        //
        // The dirty word is a DIRTY_R_* bitmask now (common.wgsl), not the
        // literal 1, and this is the one place the whole per-chunk array is
        // already in cached RAM. CLAUDE.md rule 6: "58 page faults" and
        // "108 chunks awake" are the same shape of non-measurement, and the
        // answer is attribution at the reporter, not a fortnight of A/B
        // arms. `active 219, water in 98% of them` says nothing about which
        // RULE asked; `flow 214 | react 5` says all of it.
        //
        // Printed every <n> snapshots and gated on an env var because it is
        // a diagnostic, not telemetry: the fold above stays one pass and
        // costs nothing when the var is unset.
        if (kDirtyReasonEvery != 0 &&
            (out.tick % kDirtyReasonEvery) == 0 && active != 0) {
          uint32_t hist[kDirtyReasonBits] = {0};
          uint32_t multi = 0;
          for (uint32_t i = 0; i < kNumSlots; i++) {
            const uint32_t d = dirtyW[i];
            if (d == 0) continue;
            if ((d & (d - 1)) != 0) multi++;
            for (int bit = 0; bit < kDirtyReasonBits; bit++)
              if (d & (1u << bit)) hist[bit]++;
          }
          std::printf("dirty-reasons t%u: active %u (%u multi-cause)",
                      out.tick, active, multi);
          for (int bit = 0; bit < kDirtyReasonBits; bit++)
            if (hist[bit]) std::printf(" | %s %u", kDirtyReasonName[bit], hist[bit]);
          std::printf("\n");
          std::fflush(stdout);
        }
        std::memcpy(&out.worldHash, b + kHashOff, 4);
        // The digest table rides the tail of the slot, outside the bounce
        // above, so it is read from the mapped pointer directly — but only
        // when this slot carried it (EncodeReadbacks' hashTick). Otherwise
        // the snapshot SHARES the previous one's table: the GPU buffer was
        // not rewritten in between, so it is the same bytes, and sharing
        // costs a reference count instead of a 128 KiB memcpy. The trailing
        // word is the tick the FULL occupancy pass stamped it with; on a
        // dirty tick that is an earlier tick, and saying so is the whole
        // point of carrying it.
        if (sl.hashCopied || !lastChunkHash_) {
          auto t = std::make_shared<std::vector<uint32_t>>(kChunkHashWords, 0u);
          if (sl.hashCopied)
            std::memcpy(t->data(), p + kChunkHashOff, kChunkHashBytes);
          lastChunkHash_ = std::move(t);
        }
        out.chunkHash = lastChunkHash_;
        out.chunkHashTick = (*out.chunkHash)[kChunkHashTickWord];
        std::memcpy(&out.pageFaults, p + kPageFaultOff, 4);
        std::memcpy(&out.scoopEighths, p + kPageFaultOff + kPageFaultScoopEighths * 4, 4);
        std::memcpy(&out.scoopApplied, p + kPageFaultOff + kPageFaultScoopApplied * 4, 4);
        std::memcpy(&out.scoopRefused, p + kPageFaultOff + kPageFaultScoopRefused * 4, 4);
        {
          uint32_t sm[kSolMetaHdrWords] = {};
          std::memcpy(sm, p + kSolMetaSnapOff, kSolMetaSnapBytes);
          out.solFree = sm[kSolMFree];
          out.solFaults = sm[kSolMFaults];
          out.solExhausted = sm[kSolMExhausted];
          out.solHighWater = sm[kSolMHighWater];
          out.solDissolved = sm[kSolMDissolved];
          out.solPrecip = sm[kSolMPrecip];
          out.solDiscarded = sm[kSolMDiscarded];
          out.solConverted = sm[kSolMConverted];
          out.solPoured = sm[kSolMPoured];
          out.solScooped = sm[kSolMScooped];
          out.solFaultSlot = sm[kSolMFaultSlot];
          out.solFaultTick = sm[kSolMFaultTick];
          for (uint32_t k = 0; k < kSolScoopSpecies; k++)
            out.solScoopedBy[k] = sm[kSolMScoopBySpecies + k];
          // THE REACTION-EFFECT RECORD (world.h kPageFaultReactFx*): this
          // tick's firings of rules with effects, one winner per slot. The
          // record's tick word guards against a copy that raced nothing but
          // is still worth checking -- a stale record would be a blast twice.
          uint32_t rec[kPageFaultWords - kPageFaultReactFxBase];
          std::memcpy(rec, p + kPageFaultOff + kPageFaultReactFxBase * 4, sizeof(rec));
          auto at = [&](uint32_t w) { return rec[w - kPageFaultReactFxBase]; };
          out.reactFx.clear();
          out.reactFxFires = at(kPageFaultReactFxFires);
          const uint32_t recTick = at(kPageFaultReactFxTick);
          if (out.reactFxFires != 0 && recTick != 0) {
            const IVec3 org{(int32_t)at(kPageFaultReactFxOrigin),
                            (int32_t)at(kPageFaultReactFxOrigin + 1),
                            (int32_t)at(kPageFaultReactFxOrigin + 2)};
            for (uint32_t s = 0; s < kPageFaultReactFxSlots; s++) {
              const uint32_t key = at(kPageFaultReactFxSlot0 + s);
              if (key == 0) continue;
              ReactFxEvent e;
              e.fxId = key >> kReactFxCellBits;
              e.cell = ReactFxDecodeCell(key & kReactFxCellMask, org);
              e.tick = recTick - 1u;
              out.reactFx.push_back(e);
            }
          }
        }
        std::memcpy(out.pick, b + kPickOff, 32);
        uint32_t pcounts[2];
        std::memcpy(pcounts, b + kPCountOff, 8);
        out.particleCount =
            std::min(pcounts[sl.particleLivePage & 1], kParticleCap);
        {
          uint32_t gcounts[4];
          std::memcpy(gcounts, b + kGasCountOff, 16);
          // Same parity as the ballistic count: the page the tick just
          // wrote is the one `resolve` ran over.
          out.gasCount =
              std::min(gcounts[sl.particleLivePage & 1], kGasParticleCap);
          uint32_t g[kGasSpHdr];
          std::memcpy(g, b + kGasStatOff, kGasSpHdrBytes);
          out.gasLeaveAccepted = std::min(g[kGasSpCount], kGasSpawnPerTick);
          out.gasLeaveRefused = g[kGasSpRefused];
          out.gasEdgeHits = g[kGasSpEdge];
          out.gasPoolRefused = g[kGasSpPoolFull];
          out.gasReentered = g[kGasSpReenter];
          out.gasDied = g[kGasSpDied];
          out.gasAboveWindow = g[kGasSpAbove];
        }
        // MLS-MPM fluid seam: live count, event counters, block list.
        {
          uint32_t fa[kFluidArgsWords];
          std::memcpy(fa, p + kFluidArgsOff, kFluidArgsBytes);
          out.fluidLive = std::min(fa[7], kFluidCap);
          out.fluidSettledEighths = fa[10];
          out.fluidExcitedEighths = fa[11];
          out.fluidExciteRefused = fa[12];
          out.fluidLastSlot = fa[14];
          out.fluidExciteSeen = fa[27];        // FA_EXSEEN
          out.fluidExciteCandidates = fa[28];  // FA_EXCANDID
          out.fluidBlockCount = std::min(fa[3], kFluidBlocks);
          std::memcpy(out.fluidBlocks.data(), p + kFluidBlocksOff,
                      kFluidBlocksBytes);
          // The occupancy fold is only meaningful while fluid is live —
          // the seam stops recording (and refreshing the buffer) at
          // zero, so a stale fold must read as no water.
          if (out.fluidLive > 0 && sl.fluidCopied) {
            std::memcpy(out.fluidMirror.data(), p + kFluidMirrorOff,
                        kFluidMirrorBytes);
          } else {
            std::fill(out.fluidMirror.begin(), out.fluidMirror.end(),
                      (uint8_t)0);
          }
        }
        out.tick = sl.tick;
        out.valid = true;
        // BOUNDED, and it is one line rather than an argument. SubmitTick
        // publishes on every tick from kSnapshotLatency onward, so the queue
        // holds at most K + 1 entries in every path this engine has -- but a
        // caller that submitted only ticks BELOW K would never publish, and
        // "the queue is bounded because nobody does that" is not a bound. The
        // oldest goes back to the pool; it is older than anything a publish
        // could still be owed.
        if (ready_.size() >= (size_t)kReadbackSlots) {
          snapPipe_.dropped++;
          snapPool_.push_back(std::move(ready_.front()));
          ready_.pop_front();
        }
        ready_.push_back(std::move(out));

        // fetched chunks land in the CPU cache keyed by WORLD chunk,
        // stamped with their tick
        for (size_t i = 0; i < sl.fetchIds.size(); i++) {
          CachedChunk& cc = cache_[PackChunkKey(sl.fetchIds[i])];
          if (cc.version <= sl.tick) {
            cc.version = sl.tick;
            const uint32_t e =
                i < sl.fetchSentinel.size() ? sl.fetchSentinel[i] : 0u;
            if (e != 0u) {
              // resize, not assign: every branch below writes all 4,096
              // words, so assign's zero-fill was 16 KiB of memset thrown
              // away immediately. (§2.1a)
              cc.voxels.resize(kChunkVol);
              // The mirror's synthesis, from the same helper (world.h).
              SynthChunkWords(e, sl.fetchIds[i], mirrorSeed_,
                              cc.voxels.data());
            } else {
              cc.voxels.assign(
                  (const uint32_t*)(p + kFetchOff + i * kChunkBytes),
                  (const uint32_t*)(p + kFetchOff + (i + 1) * kChunkBytes));
            }
          }
        }
        // bound the cache (drop chunks far in the past)
        if (cache_.size() > 1024) {
          for (auto it = cache_.begin(); it != cache_.end();) {
            if (it->second.version + 600 < sl.tick) it = cache_.erase(it);
            else ++it;
          }
        }
      });
}

// ---- THE SOLUTE LAYER'S RESET (world.h kSol* block) ------------------------
// Every slot SOL_EMPTY, every page on the free stack (stack[i] = i, depth =
// kSolutePoolPages), every flag, stall counter, aggregate and ledger counter 0.
// The pool itself is not cleared: solAlloc fills a page from the sentinel it
// replaces before anything reads it. Deferred writes, like every other reset
// here: they land at the head of the next submit, before any tick reads them.
void World::ResetSolutes(const rhi::Queue& queue) {
  static const std::vector<uint32_t> kZeroSlots(kNumSlots, 0u);
  queue.WriteBuffer(solTable, 0, kZeroSlots.data(), kZeroSlots.size() * 4);
  std::vector<uint32_t> meta(kSolMetaWords, 0u);
  meta[kSolMFree] = kSolutePoolPages;
  for (uint32_t i = 0; i < kSolutePoolPages; i++) meta[kSolMStackBase + i] = i;
  queue.WriteBuffer(solMeta, 0, meta.data(), meta.size() * 4);
}

// ---- THE FIXED-LATENCY PIPELINE (docs/PLAN_multiplayer_now.md N1) ---------

void World::InvalidateSnapshot() {
  snap_.valid = false;
  // The digest table the next snapshot would inherit describes the dead
  // world, and the resets that land here (worldgen's fill, the load reset)
  // rewrite the GPU table: the next readback must COPY it, not share.
  lastChunkHash_.reset();
  chunkHashForce_ = true;
  // The quiet streaks described the DEAD world's chunks (M9.3-A). A fresh
  // worldgen makes every slot's history meaningless, and a stale streak here
  // would read as "settled, safe to compare" for a chunk that has not been
  // simulated once.
  std::fill(quietTicks_.begin(), quietTicks_.end(), (uint16_t)0);
  // Reaction effects the dead world published and nobody consumed yet.
  reactFxPending_.clear();
  // A regenerated window makes every cached chunk stale too: the fetch path's
  // version guard (`cc.version <= sl.tick`) would otherwise keep dead-world
  // contents for any later reader whose tick numbers are LOWER than the gate
  // that filled the entry (2026-09-13, see the header).
  cache_.clear();
  // Everything parsed but unpublished describes the dead world too. Recycle
  // the storage rather than freeing it: the pool is what keeps the steady
  // state at kSnapshotLatency + 1 WorldSnapshots instead of a fresh 780 KiB
  // allocation per tick.
  while (!ready_.empty()) {
    snapPipe_.dropped++;
    snapPool_.push_back(std::move(ready_.front()));
    ready_.pop_front();
  }
  // Readbacks still in flight were encoded against the dead world. They cannot
  // be cancelled, so they are DISOWNED: the callback compares the epoch it
  // captured against this one and drops the bytes.
  snapEpoch_++;
  haveEncodeTick_ = false;
}

bool World::ReadbackSlotFree() const {
  const int active = ActiveReadbackSlots();
  for (int i = 0; i < active; i++)
    if (!slots_[i].inFlight) return true;
  return false;
}

bool World::ReadbackPendingAtOrBefore(uint32_t tick) const {
  const int active = ActiveReadbackSlots();
  for (int i = 0; i < active; i++) {
    const Slot& s = slots_[i];
    // Only this epoch's slots are owed to anybody; a disowned one will free
    // itself without ever producing a snapshot, so waiting on it is waiting
    // for something that is not coming.
    if (!s.inFlight || s.epoch != snapEpoch_) continue;
    if (s.tick <= tick) return true;
  }
  return false;
}

bool World::PublishSnapshotsUpTo(uint32_t target) {
  snapPipe_.ticks++;
  // ready_ is in tick order (the backend fires pending maps in submission
  // order), so this walks forward and stops at the first snapshot that belongs
  // to a LATER tick. Publishing means a swap, not a copy: `snap_` keeps its
  // 780 KiB of arrays and the outgoing one goes back to the pool with its
  // allocations intact.
  bool got = false;
  while (!ready_.empty() && ready_.front().tick <= target) {
    std::swap(snap_, ready_.front());
    snapPool_.push_back(std::move(ready_.front()));
    ready_.pop_front();
    got = true;
    // Reaction effects ride the publish, one snapshot at a time, so a publish
    // that walks two snapshots delivers both ticks' firings (World::TakeReactFx).
    if (snap_.valid && !snap_.reactFx.empty()) {
      reactFxPending_.insert(reactFxPending_.end(), snap_.reactFx.begin(),
                             snap_.reactFx.end());
      if (reactFxPending_.size() > kReactFxPendingMax)
        reactFxPending_.erase(reactFxPending_.begin(),
                              reactFxPending_.end() - kReactFxPendingMax);
    }
    // ---- the quiet streak (M9.3-A) -------------------------------------
    // Here and not in the readback callback, because "quiet for N ticks" has
    // to count the ticks the CPU world view actually advanced through. The
    // callback fires on the GPU's schedule and can deliver two snapshots in
    // one pump; the publish loop walks them in tick order, one per tick, which
    // is exactly the sequence the counter is supposed to describe.
    //
    // dirtyFlags is the NEXT-TICK dirty flag the tick wrote, i.e. "something
    // in this chunk is scheduled to act", which is the conservative direction:
    // a chunk that woke and did nothing reads as busy for a tick, and a chunk
    // that changed never reads as quiet.
    const WorldSnapshot& sn = snap_;
    if (sn.valid && sn.dirtyFlags.size() == quietTicks_.size()) {
      for (size_t i = 0; i < quietTicks_.size(); i++) {
        if (sn.dirtyFlags[i]) quietTicks_[i] = 0;
        else if (quietTicks_[i] != 0xFFFFu) quietTicks_[i]++;
      }
    }
  }
  if (got && snap_.valid && snap_.tick == target) {
    snapPipe_.published++;
    return true;
  }
  // No snapshot AT the target. This is not a stall: it is the first
  // kSnapshotLatency ticks after a world reset, when the readback for
  // `target` was never encoded because the world did not exist yet. Snap()
  // stays as it was (invalid after a reset), which is a constant too.
  snapPipe_.missing++;
  return false;
}

CellKind World::KindAt(IVec3 cell, const std::vector<uint32_t>& classOf) const {
  if (!snap_.valid) return CellKind::Unknown;
  // outside the residency window = solid and inert (matches sim rule)
  IVec3 lo{snap_.windowOrigin.x * (int)kChunk, snap_.windowOrigin.y * (int)kChunk,
           snap_.windowOrigin.z * (int)kChunk};
  if (cell.x < lo.x || cell.y < lo.y || cell.z < lo.z ||
      cell.x >= lo.x + (int)kWorldN || cell.y >= lo.y + (int)kWorldN ||
      cell.z >= lo.z + (int)kWorldN)
    return CellKind::Solid;
  int cx = (cell.x >> 4) - snap_.mirrorBase.x;
  int cy = (cell.y >> 4) - snap_.mirrorBase.y;
  int cz = (cell.z >> 4) - snap_.mirrorBase.z;
  if (cx < 0 || cy < 0 || cz < 0 || cx >= 3 || cy >= 3 || cz >= 3)
    return CellKind::Unknown;
  int lx = cell.x & 15, ly = cell.y & 15, lz = cell.z & 15;
  uint32_t w = snap_.mirror[(size_t)((cz * 3 + cy) * 3 + cx) * kChunkVol +
                            (lz * (int)kChunk + ly) * (int)kChunk + lx];
  // The word -> class arms are World::KindOfWord (world.h). KindAtCached reads
  // a different store and has to answer identically for the same word; two
  // copies of this switch is how the two would stop agreeing.
  return KindOfWord(w, classOf);
}

float World::CellTopAt(IVec3 cell, const std::vector<uint32_t>& classOf) const {
  if (!snap_.valid) return 1.0f;
  int cx = (cell.x >> 4) - snap_.mirrorBase.x;
  int cy = (cell.y >> 4) - snap_.mirrorBase.y;
  int cz = (cell.z >> 4) - snap_.mirrorBase.z;
  if (cx < 0 || cy < 0 || cz < 0 || cx >= 3 || cy >= 3 || cz >= 3) return 1.0f;
  int lx = cell.x & 15, ly = cell.y & 15, lz = cell.z & 15;
  const uint32_t w = snap_.mirror[(size_t)((cz * 3 + cy) * 3 + cx) * kChunkVol +
                                  (lz * (int)kChunk + ly) * (int)kChunk + lx];
  const uint32_t m = w & 0xFFFu;
  if (m == 0 || m >= classOf.size() || classOf[m] != 1u) return 1.0f;
  return (float)PowderMassOfState((w >> 12) & 0xFu) / (float)kPowderFull;
}

// ---- M9.1 P3: the same question, for a body the mirror is not centred on ---
// world.h carries the whole argument (why the analytic fallback, why fluid is
// not answered, why this is not const). This is only the three cases.
CellKind World::KindAtCached(IVec3 cell,
                             const std::vector<uint32_t>& classOf) {
  const IVec3 wc{cell.x >> 4, cell.y >> 4, cell.z >> 4};
  // Outside the residency window: solid and inert. Bit-identical to KindAt's
  // first test, and the same rule the sim enforces at the window edge.
  if (!ChunkInWindow(wc)) return CellKind::Solid;
  const CachedChunk* cc = Cached(wc);
  // A chunk whose fetch is queued but not landed has an entry with no voxels
  // in some paths; MobSystem's probe checks the size for the same reason.
  if (cc != nullptr && cc->voxels.size() == kChunkVol) {
    const size_t li = (size_t)(((cell.z & 15) * (int)kChunk + (cell.y & 15)) *
                                   (int)kChunk +
                               (cell.x & 15));
    return KindOfWord(cc->voxels[li], classOf);
  }
  // Miss. Ask for the chunk (coalesced against anything already queued, and
  // refused outright if it is not resident -- which ChunkInWindow above has
  // already ruled out) and answer from worldgen's own height contract.
  RequestChunkFetch(wc, FetchSource::Mob);
  // `<=` because TerrainHeight names the TOPMOST GROUND VOXEL, not the first
  // air cell above it: a body standing on pristine terrain has its sole at
  // h + 1 (the `player-walk` gate asserts exactly that).
  return cell.y <= TerrainHeight(cell.x, cell.z, WorldSeed()) ? CellKind::Solid
                                                              : CellKind::Air;
}

// ---- exact CPU mirror of worldgen.wgsl (integer-only, keep in sync) ----
// pcg/hash3 come from sim/rng.h; the lowercase wrappers keep this block
// reading like the WGSL it mirrors line-for-line.
static inline uint32_t pcg(uint32_t v) { return rng::Pcg(v); }
static inline uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
  return rng::Hash3(a, b, c);
}
// floor division / positive modulo, matching worldgen.wgsl fdiv/fmodp (the
// noise lattice must be seamless across negative world coordinates)
static int fdiv(int a, int b) {
  int q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) q -= 1;
  return q;
}
static int fmodp(int a, int b) {
  int m = a % b;
  return m < 0 ? m + b : m;
}
// The legacy 0..255 vnoise is NOT mirrored any more. Its only CPU caller was
// TerrainHeight, and the terrain octaves moved to vnoise2d below; the fourteen
// call sites it still has in the shader are all decorative fields (flower
// clumps, undergrowth masks) that no CPU path asks about.
//
// WGSL's `select(a, b, cond)` spelled the same way, so the mirrored bodies
// below can be read side by side with the shader's.
static int select(int a, int b, bool c) { return c ? b : a; }
// worldgen.wgsl's vlen(): the shader's hardcoded LENGTHS scaled from the
// reference voxel size (map.json terrain.refVoxelsPerMetre, the prelude's
// REF_VOXELS_PER_METRE) to the live one. The map's terrain numbers were
// already rescaled by LoadWorldMap, so this covers only what is written
// literally in the shader — which on this side of the mirror is the authored
// set-piece geometry.
static int vlen(int v) { return (v * kVoxelsPerMetre) / worldmap::CurrentTerrain().refVoxelsPerMetre; }

// MIRROR-BEGIN noise
// The Q14 value noise of worldgen.wgsl, mirrored line for line. Read that
// file's block for why 14 bits and why the cell is a log2 shift; the one thing
// worth repeating HERE is the reason this can be written twice at all:
//
//   `>>` ON A NEGATIVE SIGNED INTEGER IS ARITHMETIC IN BOTH LANGUAGES.
//   WGSL sign-extends by definition; C++20 (P0907) fixed signed integers as
//   two's complement and `>>` as floor-division by a power of two. So
//   `x >> csl` is the same value on both sides at every negative coordinate,
//   which is what lets it replace fdiv(), and `x & mask` replace fmodp().
//   That is the single place where these two languages could have disagreed,
//   and they do not.
//
// scripts/check_invariants.py compares the normalised token streams of the two
// blocks tagged `noise` below, so an edit to one that is not made to the other
// fails at edit time rather than as a player falling through visible ground.
struct N2 {
  int n;
  int dx;
  int dz;
};
static int vsmooth(int t) {
  int t2 = (t * t) >> 15;
  int t3 = (t2 * t) >> 15;
  return 3 * t2 - 2 * t3;
}
static int vsmoothd(int t) {
  return (6 * t * (32768 - t)) >> 15;
}
static int q15frac(int f, uint32_t csl) {
  if (csl <= 15u) { return f << (15u - csl); }
  return f >> (csl - 15u);
}
static int vbilerp(int c00, int c10, int c01, int c11, int sx, int sz) {
  int a = c00 + (((c10 - c00) * sx) >> 15);
  int b = c01 + (((c11 - c01) * sx) >> 15);
  return a + (((b - a) * sz) >> 15);
}
static N2 vnoise2d(int x, int z, uint32_t csl, uint32_t seed) {
  int gx = x >> csl;
  int gz = z >> csl;
  int mask = (int)((1u << csl) - 1u);
  int tx = q15frac(x & mask, csl);
  int tz = q15frac(z & mask, csl);
  int c00 = (int)(hash3(seed, (uint32_t)(gx), (uint32_t)(gz)) & 0x3FFFu);
  int c10 = (int)(hash3(seed, (uint32_t)(gx + 1), (uint32_t)(gz)) & 0x3FFFu);
  int c01 = (int)(hash3(seed, (uint32_t)(gx), (uint32_t)(gz + 1)) & 0x3FFFu);
  int c11 = (int)(hash3(seed, (uint32_t)(gx + 1), (uint32_t)(gz + 1)) & 0x3FFFu);
  int sx = vsmooth(tx);
  int sz = vsmooth(tz);
  N2 o;
  o.n = vbilerp(c00, c10, c01, c11, sx, sz);
  int ga = c10 - c00;
  int gb = c11 - c01;
  o.dx = ((ga + (((gb - ga) * sz) >> 15)) * vsmoothd(tx)) >> 15;
  int ha = c01 - c00;
  int hb = c11 - c10;
  o.dz = ((ha + (((hb - ha) * sx) >> 15)) * vsmoothd(tz)) >> 15;
  return o;
}
// MIRROR-END noise

// ---- the world map's biome, on the CPU (worldgen.wgsl mapBiomeAt) ---------
// NOT inside a MIRROR block: mapBiomeAt is outside every mirror in the
// shader too (biomeAt has never had a height role at default tuning). Spelled
// the same anyway -- same salts, same shift, same warp -- and the `worldmap`
// gate compares the two at cell centres.
uint32_t World::MapBiomeAt(int x, int z, uint32_t seed) {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (!m.Loaded()) return 0u;
  const int amp = m.warpAmpVox;
  const int wx = x + (((vnoise2d(x, z, 9u, seed ^ 0x3A9Fu).n - 8192) * amp) >> 14);
  const int wz = z + (((vnoise2d(x, z, 9u, seed ^ 0x3AA0u).n - 8192) * amp) >> 14);
  int cx, cz;
  m.CellOf(wx, wz, &cx, &cz);
  if (!m.Inside(cx, cz)) return static_cast<uint32_t>(m.oceanBiome);
  return m.BiomeCell(cx, cz);
}

// The map's calm pad box (worldgen.wgsl inPadBox; map.json kind "pad", which
// only the selftest map `harness` authors). Outside the mirror on both sides;
// siteKeepOut below calls it by the same name.
static bool inPadBox(int x, int z) {
  return worldmap::CurrentWorldMap().InPadBox(x, z);
}
bool World::InPadBox(int x, int z) { return inPadBox(x, z); }

// ---- the site table, on the CPU (worldgen.wgsl wmSiteAt / wmSiteI) --------
// Same names as the shader so the mirrored sitePadAt below reads the same.
static constexpr uint32_t WM_S_KIND = worldmap::kS_Kind;
static constexpr uint32_t WM_S_X = worldmap::kS_X;
static constexpr uint32_t WM_S_Z = worldmap::kS_Z;
static constexpr uint32_t WM_S_RADIUS = worldmap::kS_Radius;
static constexpr uint32_t WM_S_PAD_MARGIN = worldmap::kS_PadMargin;
static constexpr uint32_t WM_S_PRESET = worldmap::kS_Preset;
static constexpr uint32_t WM_S_PAD_Y = worldmap::kS_PadY;
static constexpr uint32_t WM_SITE_STAMP = worldmap::kSiteStamp;
static constexpr uint32_t WM_SITE_WATER = worldmap::kSiteWater;
static constexpr uint32_t WM_SITE_TREE = worldmap::kSiteTree;
// The cell's site list (worldmap.h "the site table"): nullptr = no site.
static const uint32_t* wmSiteList(int x, int z) {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (!m.Loaded() || m.sites.empty()) return nullptr;
  int cx, cz;
  m.CellOf(x, z, &cx, &cz);
  if (!m.Inside(cx, cz)) return nullptr;
  return m.SiteList(cx, cz);
}
static uint32_t wmSiteN(const uint32_t* lst) {
  return lst == nullptr ? 0u : std::min<uint32_t>(lst[0], worldmap::kSiteCellMax);
}
static uint32_t wmSiteK(const uint32_t* lst, uint32_t k) { return lst[1u + k]; }
static int wmSiteI(uint32_t sid, uint32_t w) {
  const worldmap::WorldMapData::StampSite& s = worldmap::CurrentWorldMap().sites[sid - 1];
  switch (w) {
    case worldmap::kS_Kind: return s.kind;
    case worldmap::kS_X: return s.x;
    case worldmap::kS_Z: return s.z;
    case worldmap::kS_Radius: return s.radius;
    case worldmap::kS_PadMargin: return s.padMargin;
    case worldmap::kS_Preset: return s.preset;
    case worldmap::kS_PadY: return s.padY;
    default: return 0;
  }
}
// Is (x, z) inside site `sid`'s own footprint? A lake (P-F): its DISC plus
// its shore/berm band. A tree (P6): its trunk keep-out square. A stamp: its
// pad (Chebyshev radius + margin). worldgen.wgsl siteFootprintHas, same test.
// A stamp's footprint rect / apron and a soften site's scales, read from the
// site itself (worldgen.wgsl reads the same numbers as record words
// kS_BoxX0.. / kS_Apron / kS_Soft*).
static const worldmap::WorldMapData::StampSite& wmSite(uint32_t sid) {
  return worldmap::CurrentWorldMap().sites[sid - 1];
}
// worldgen.wgsl stampRectDist: the larger axis gap to the rect plus half the
// smaller (an octagon), 0 inside.
static int stampRectDist(uint32_t sid, int x, int z) {
  const worldmap::WorldMapData::StampSite& s = wmSite(sid);
  const int gx = std::max(std::max(s.bx0 - x, x - s.x1), 0);
  const int gz = std::max(std::max(s.bz0 - z, z - s.z1), 0);
  return std::max(gx, gz) + (std::min(gx, gz) >> 1u);
}
// worldgen.wgsl sitePadWeight: Q8 share of the pad height, 256 over rect +
// apron, a smoothstep to 0 across the margin.
static int sitePadWeight(uint32_t sid, int x, int z) {
  const worldmap::WorldMapData::StampSite& s = wmSite(sid);
  if ((uint32_t)s.kind != WM_SITE_STAMP) return 0;
  const int e = stampRectDist(sid, x, z) - s.apron;
  if (e <= 0) return 256;
  const int margin = std::max(s.padMargin, 1);
  if (e >= margin) return 0;
  const int t = ((margin - e) * 256) / margin;
  return (t * t * (768 - 2 * t)) >> 16u;
}
// worldgen.wgsl sitePadNear: is any stamp's whole pad within r?
static bool sitePadNear(int x, int z, int r);
// worldgen.wgsl softenAt: the Q8 scales of the three local octaves.
struct Soften {
  int hill;
  int bump;
  int grain;
};
static Soften softenAt(int x, int z);
// Is (x, z) inside site `sid`'s own footprint? A lake (P-F): its DISC plus
// its shore/berm band. A tree (P6): its trunk keep-out square. A stamp: its
// FLAT core, rect + apron (2026-09-29; the ramp is ordinary ground).
// worldgen.wgsl siteFootprintHas, same test.
static bool siteFootprintHas(uint32_t sid, int x, int z) {
  const uint32_t kind = (uint32_t)(wmSiteI(sid, WM_S_KIND));
  // A clearing keeps crowns off and nothing else: no footprint. Nor does
  // softened ground or a route.
  if (kind == worldmap::kSiteClearing || kind == worldmap::kSiteSoften || kind == worldmap::kSitePath) return false;
  const int dx = x - wmSiteI(sid, WM_S_X);
  const int dz = z - wmSiteI(sid, WM_S_Z);
  if (kind == WM_SITE_WATER) {
    const int reach = wmSiteI(sid, WM_S_RADIUS) + wmSiteI(sid, WM_S_PAD_MARGIN);
    return dx * dx + dz * dz <= reach * reach;
  }
  if (kind == WM_SITE_STAMP) return stampRectDist(sid, x, z) <= wmSite(sid).apron;
  return std::max(std::abs(dx), std::abs(dz)) <= (int)worldmap::kSiteTreeKeepOut;
}
// The keep-out every feature avoids: the pad box, and every listed site's
// footprint (never its cells, since P6). Same test as the shader's.
static bool siteKeepOut(int x, int z) {
  if (inPadBox(x, z)) { return true; }
  const uint32_t* lst = wmSiteList(x, z);
  const uint32_t n = wmSiteN(lst);
  for (uint32_t k = 0u; k < n; k++) {
    if (siteFootprintHas(wmSiteK(lst, k), x, z)) { return true; }
  }
  return false;
}
bool worldmap::SiteKeepOut(int x, int z) { return siteKeepOut(x, z); }
// The authored lake a column's pond set carries (worldgen.wgsl
// wmWaterSiteAt): the first listed lake whose reach box holds the column,
// else the first listed lake -- which, for a cell only one lake reaches, is
// exactly the one lake the pre-P6 index gave every column of the cell.
static uint32_t wmWaterSiteAt(int x, int z) {
  const uint32_t* lst = wmSiteList(x, z);
  const uint32_t n = wmSiteN(lst);
  uint32_t first = 0u;
  for (uint32_t k = 0u; k < n; k++) {
    const uint32_t sid = wmSiteK(lst, k);
    if ((uint32_t)(wmSiteI(sid, WM_S_KIND)) != WM_SITE_WATER) { continue; }
    const int reach = wmSiteI(sid, WM_S_RADIUS) + wmSiteI(sid, WM_S_PAD_MARGIN) + 1;
    if (std::max(std::abs(x - wmSiteI(sid, WM_S_X)), std::abs(z - wmSiteI(sid, WM_S_Z))) <= reach) { return sid; }
    if (first == 0u) { first = sid; }
  }
  return first;
}
static bool sitePadNear(int x, int z, int r) {
  const uint32_t* lst = wmSiteList(x, z);
  const uint32_t n = wmSiteN(lst);
  for (uint32_t k = 0u; k < n; k++) {
    const uint32_t sid = wmSiteK(lst, k);
    if ((uint32_t)wmSite(sid).kind != WM_SITE_STAMP) continue;
    if (stampRectDist(sid, x, z) < wmSite(sid).apron + wmSite(sid).padMargin + r) return true;
  }
  return false;
}
// worldgen.wgsl routeDist2: squared distance to a route's segment, -1 when
// outside its box grown by `reach`.
static int routeDist2(const worldmap::WorldMapData::StampSite& st, int x, int z, int reach) {
  const int ax = st.x, az = st.z, bx = st.x1, bz = st.z1;
  if (x < std::min(ax, bx) - reach || x > std::max(ax, bx) + reach ||
      z < std::min(az, bz) - reach || z > std::max(az, bz) + reach) return -1;
  const int vx = bx - ax, vz = bz - az;
  const int l2 = vx * vx + vz * vz;
  const int t = (x - ax) * vx + (z - az) * vz;
  int cx = ax, cz = az;
  if (l2 > 0 && t >= l2) { cx = bx; cz = bz; }
  else if (l2 > 0 && t > 0) { cx = ax + (vx * t) / l2; cz = az + (vz * t) / l2; }
  const int dx = x - cx, dz = z - cz;
  return dx * dx + dz * dz;
}
static Soften softenAt(int x, int z) {
  Soften s{256, 256, 256};
  const uint32_t* lst = wmSiteList(x, z);
  const uint32_t n = wmSiteN(lst);
  int wbox = 0, pathQ = 256;
  for (uint32_t k = 0u; k < n; k++) {
    const worldmap::WorldMapData::StampSite& st = wmSite(wmSiteK(lst, k));
    if ((uint32_t)st.kind != worldmap::kSiteSoften) continue;
    const int d = std::max(std::max(st.x - x, x - st.x1), std::max(st.z - z, z - st.z1));
    int w = 256;
    if (d > 0) {
      const int f = st.padMargin;
      if (d >= f) continue;
      const int t = ((f - d) * 256) / f;
      w = (t * t * (768 - 2 * t)) >> 16u;
    }
    s.hill = std::min(s.hill, 256 - (((256 - st.softHill) * w) >> 8u));
    s.bump = std::min(s.bump, 256 - (((256 - st.softBump) * w) >> 8u));
    s.grain = std::min(s.grain, 256 - (((256 - st.softGrain) * w) >> 8u));
    if (w > wbox) { wbox = w; pathQ = st.softPath; }
  }
  if (wbox > 0) {
    int wr = 0;
    for (uint32_t k = 0u; k < n; k++) {
      const worldmap::WorldMapData::StampSite& st = wmSite(wmSiteK(lst, k));
      if ((uint32_t)st.kind != worldmap::kSitePath) continue;
      const int d2 = routeDist2(st, x, z, (int)worldmap::kRouteSoftOuter);
      if (d2 < 0 || d2 >= (int)worldmap::kRouteSoftOuter * (int)worldmap::kRouteSoftOuter) continue;
      int t = 256;
      const int i2 = (int)worldmap::kRouteSoftInner * (int)worldmap::kRouteSoftInner;
      if (d2 > i2) {
        const int o2 = (int)worldmap::kRouteSoftOuter * (int)worldmap::kRouteSoftOuter;
        t = ((o2 - d2) * 256) / (o2 - i2);
      }
      wr = std::max(wr, (t * t * (768 - 2 * t)) >> 16u);
    }
    const int wp = (wr * wbox) >> 8u;
    s.hill = std::min(s.hill, 256 - (((256 - pathQ) * wp) >> 8u));
  }
  return s;
}
// The painted cell's biome, no warp and no seed (worldgen.wgsl biomeCellAt):
// whose water rows roll at a pond tile.
static uint32_t biomeCellAt(int x, int z) {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (!m.Loaded()) return 0u;
  int cx, cz;
  m.CellOf(x, z, &cx, &cz);
  if (!m.Inside(cx, cz)) return static_cast<uint32_t>(m.oceanBiome);
  return m.BiomeCell(cx, cz);
}

// ---- the water table, on the CPU (worldgen.wgsl wmWater / wmWaterRow /
// pondTile / pondBand / waterKnot; P-F) ------------------------------------
// The mirrored pondInfo / bowlDepth / pondNear read the preset geometry
// through these names on both sides. Here they read WorldMapData::water and
// ::biomeWater, which LoadWorldMap filled with the SAME WaterGeomOf /
// PackWaterRows integers the packer wrote into the buffer.
static constexpr uint32_t WM_W_FILL = worldmap::kW_Fill;
static constexpr uint32_t WM_W_RADIUS_MIN = worldmap::kW_RadiusMin;
static constexpr uint32_t WM_W_RADIUS_SPAN = worldmap::kW_RadiusSpan;
static constexpr uint32_t WM_W_DEPTH = worldmap::kW_Depth;
static constexpr uint32_t WM_W_RIM_DEPTH = worldmap::kW_RimDepth;
static constexpr uint32_t WM_W_BERM_H = worldmap::kW_BermH;
static constexpr uint32_t WM_W_BERM_W = worldmap::kW_BermW;
static constexpr uint32_t WM_W_SHORE_BAND = worldmap::kW_ShoreBand;
static constexpr uint32_t WM_W_SHORE_LIFT = worldmap::kW_ShoreLift;
static constexpr uint32_t WM_W_BAND = worldmap::kW_Band;
static constexpr uint32_t WM_R_PRESET = worldmap::kR_Preset;
static constexpr uint32_t WM_R_CHANCE_Q16 = worldmap::kR_ChanceQ16;
static constexpr uint32_t WM_R_MIN_Y = worldmap::kR_MinY;
static constexpr uint32_t WM_R_MAX_Y = worldmap::kR_MaxY;
static constexpr uint32_t WM_R_MAX_SLOPE = worldmap::kR_MaxSlope;
static const worldmap::WaterGeom* waterGeomOf(uint32_t p) {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (p == 0u || p > m.water.size()) return nullptr;
  return &m.water[p - 1];
}
static uint32_t wmWater(uint32_t p, uint32_t w) {
  const worldmap::WaterGeom* g = waterGeomOf(p);
  if (!g) return 0u;
  switch (w) {
    case worldmap::kW_Fill: return g->fill;
    case worldmap::kW_RadiusMin: return (uint32_t)g->radiusMin;
    case worldmap::kW_RadiusSpan: return (uint32_t)g->radiusSpan;
    case worldmap::kW_Depth: return (uint32_t)g->depth;
    case worldmap::kW_RimDepth: return (uint32_t)g->rimDepth;
    case worldmap::kW_BermH: return (uint32_t)g->bermH;
    case worldmap::kW_BermW: return (uint32_t)g->bermW;
    case worldmap::kW_ShoreBand: return (uint32_t)g->shoreBand;
    case worldmap::kW_ShoreLift: return (uint32_t)g->shoreLift;
    case worldmap::kW_MudWidth: return (uint32_t)g->mudWidth;
    case worldmap::kW_MudMat: return g->mudMat;
    case worldmap::kW_BedShallow: return g->bedShallow;
    case worldmap::kW_BedDeep: return g->bedDeep;
    case worldmap::kW_BedShallowDepth: return (uint32_t)g->bedShallowDepth;
    case worldmap::kW_BedThickness: return (uint32_t)g->bedThickness;
    case worldmap::kW_BedSubstrate: return g->bedSubstrate;
    case worldmap::kW_Band: return (uint32_t)g->band;
    default: return 0u;
  }
}
static int wmWaterI(uint32_t p, uint32_t w) { return (int)wmWater(p, w); }
static uint32_t wmWaterRowCount(uint32_t b) {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (b >= m.biomeWater.size()) return 0u;
  return (uint32_t)m.biomeWater[b].size();
}
static int wmWaterRow(uint32_t b, uint32_t i, uint32_t w) {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (b >= m.biomeWater.size() || i >= m.biomeWater[b].size() || w >= worldmap::kWaterRowWords) return 0;
  return (int)m.biomeWater[b][i].w[w];
}
static int pondTile() { return worldmap::CurrentWorldMap().pondTile; }
static int pondBand() { return worldmap::CurrentWorldMap().pondBand; }
static int waterKnot(uint32_t p, uint32_t k) {
  const worldmap::WaterGeom* g = waterGeomOf(p);
  if (!g || k > 16u) return 0;
  return g->knots[k];
}

// The shader's vec2<i32>, so pondAt can be mirrored with the same shape.
// Outside the mirrored region: WGSL gets this type from the language.
struct IV2 {
  int x;
  int y;
};
static IV2 iv2(int a, int b) { IV2 v; v.x = a; v.y = b; return v; }

// The spawn site and the pad box's edge distance (worldgen.wgsl
// spawnCentre / padOutside): the two centres of the calm home area in
// the mirrored landAt below. Outside the mirror on both sides; the mirrored
// code calls them by name, and the `terrain` gate's C1 is the per-voxel
// proof they read the same numbers. An empty box (x1 < x0) is "far", which
// switches the pad's fade off.
static IV2 spawnCentre() {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  return iv2(m.spawnX, m.spawnZ);
}
static int padOutside(int x, int z) {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (m.padX1 < m.padX0) return 1073741824;
  return std::max(std::max(std::max(m.padX0 - x, x - m.padX1),
                           std::max(m.padZ0 - z, z - m.padZ1)), 0);
}

// ---- the map's terrain, on the CPU (worldgen.wgsl wmTerrain / wmTerrainU; P-G) ----
// The header words map.json `terrain` became (worldmap.h kHTerrain*), read
// by NAME inside the height mirror on both sides. Here they come from
// WorldMapData::terrainWords, which LoadWorldMap filled with the SAME
// TerrainWords() the packer wrote into the buffer; an unloaded map carries the
// defaults (WorldMapData's constructor), never zeros.
static constexpr uint32_t WM_H_TERRAIN_BASE_HEIGHT = worldmap::kHTerrainBaseHeight;
static constexpr uint32_t WM_H_TERRAIN_LANDFORM_RANGE = worldmap::kHTerrainLandformRange;
static constexpr uint32_t WM_H_TERRAIN_RANGE_AMPLITUDE = worldmap::kHTerrainRangeAmplitude;
static constexpr uint32_t WM_H_TERRAIN_RANGE_LOG2 = worldmap::kHTerrainRangeLog2;
static constexpr uint32_t WM_H_TERRAIN_HILL_AMPLITUDE = worldmap::kHTerrainHillAmplitude;
static constexpr uint32_t WM_H_TERRAIN_HILL_LOG2 = worldmap::kHTerrainHillLog2;
static constexpr uint32_t WM_H_TERRAIN_DETAIL_AMPLITUDE = worldmap::kHTerrainDetailAmplitude;
static constexpr uint32_t WM_H_TERRAIN_DETAIL_LOG2 = worldmap::kHTerrainDetailLog2;
static constexpr uint32_t WM_H_TERRAIN_GRAIN_AMPLITUDE = worldmap::kHTerrainGrainAmplitude;
static constexpr uint32_t WM_H_TERRAIN_GRAIN_LOG2 = worldmap::kHTerrainGrainLog2;
static constexpr uint32_t WM_H_TERRAIN_FBM_ATTEN = worldmap::kHTerrainFbmAtten;
static constexpr uint32_t WM_H_TERRAIN_HOME_Y = worldmap::kHTerrainHomeY;
static constexpr uint32_t WM_H_TERRAIN_HOME_R = worldmap::kHTerrainHomeR;
static constexpr uint32_t WM_H_TERRAIN_HOME_FADE = worldmap::kHTerrainHomeFade;
static constexpr uint32_t WM_H_TERRAIN_SED_CEIL = worldmap::kHTerrainSedCeil;
static constexpr uint32_t WM_H_TERRAIN_SED_FRACTION = worldmap::kHTerrainSedFraction;
static constexpr uint32_t WM_H_TERRAIN_SED_STRIP = worldmap::kHTerrainSedStrip;
static constexpr uint32_t WM_H_TERRAIN_SED_SLOPE = worldmap::kHTerrainSedSlope;
static constexpr uint32_t WM_H_TERRAIN_SED_MAX = worldmap::kHTerrainSedMax;
static constexpr uint32_t WM_H_TERRAIN_SED_TOPSOIL = worldmap::kHTerrainSedTopsoil;
static int wmTerrain(uint32_t w) {
  return (int)worldmap::CurrentWorldMap().terrainWords[w - worldmap::kHTerrainBaseHeight];
}
static uint32_t wmTerrainU(uint32_t w) {
  return worldmap::CurrentWorldMap().terrainWords[w - worldmap::kHTerrainBaseHeight];
}

// ---- per-biome relief, on the CPU (worldgen.wgsl biomeMixAt / biomeCurve /
// biomeReliefAt; P-G) --------------------------------------------------------
//
// OUTSIDE the tagged region, like the plane readers: the mirrored landAt calls
// biomeMixAt / biomeCurve / biomeReliefAt by name, and the `terrain` gate's
// pass C1 compares the two implementations per voxel over 9,409 columns,
// which is the proof this block is right. Spelled as the shader spells it
// anyway -- same cell lattice as mapLandformQ8, same bilinear shifts, same
// Hermite -- over the same integers (WorldMapData::biomeTerrain, the same
// PackBiomeTerrain the packer wrote into the records).
//
// See the long note over the same functions in worldgen.wgsl for why there are
// nine knots and not eight, why the domain is HALF what the plan said, and the
// four separate things that make the identity curve bit-exact.
static constexpr uint32_t WM_B_CURVE_KNOT0 = worldmap::kB_CurveKnot0;
static constexpr uint32_t WM_B_HILL_MUL = worldmap::kB_HillMul;
static constexpr uint32_t WM_B_DETAIL_MUL = worldmap::kB_DetailMul;
static constexpr uint32_t WM_B_GRAIN_MUL = worldmap::kB_GrainMul;
constexpr int kCurveKnots = 9;
constexpr int kCurveSegs = 8;

struct BiomeMix {
  uint32_t b00, b10, b01, b11;
  int fx, fz;
  uint32_t l;
};
static uint32_t wmBiomeCellId(int cx, int cz) {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  const int x = std::clamp(cx, 0, m.width - 1), z = std::clamp(cz, 0, m.height - 1);
  return m.biome[(size_t)z * m.width + x];
}
static BiomeMix biomeMixAt(int x, int z) {
  BiomeMix m{};
  const worldmap::WorldMapData& d = worldmap::CurrentWorldMap();
  if (!d.Loaded()) { m.l = 1u; return m; }
  const uint32_t l = (uint32_t)d.cellLog2;
  const int half = 1 << (l - 1u);
  const int mx = x - half + (d.originCellX << l);
  const int mz = z - half + (d.originCellZ << l);
  const int cx = mx >> l;
  const int cz = mz >> l;
  const int mask = (1 << l) - 1;
  m.fx = mx & mask;
  m.fz = mz & mask;
  m.l = l;
  m.b00 = wmBiomeCellId(cx, cz);
  m.b10 = wmBiomeCellId(cx + 1, cz);
  m.b01 = wmBiomeCellId(cx, cz + 1);
  m.b11 = wmBiomeCellId(cx + 1, cz + 1);
  return m;
}
static int mixI(BiomeMix m, int v00, int v10, int v01, int v11) {
  const int a = v00 + (((v10 - v00) * m.fx) >> m.l);
  const int b = v01 + (((v11 - v01) * m.fx) >> m.l);
  return a + (((b - a) * m.fz) >> m.l);
}
// A biome record word read as a signed value: the terrain record's words,
// out of the loader's copy of the packed table.
static int wmBiomeI(uint32_t b, uint32_t w) {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (b >= m.biomeTerrain.size() || w < worldmap::kB_CurveKnot0 || w > worldmap::kB_GrainMul) return 0;
  return (int)m.biomeTerrain[b].w[w - worldmap::kB_CurveKnot0];
}
static int curveKnotAt(BiomeMix m, int i) {
  const uint32_t w = WM_B_CURVE_KNOT0 + (uint32_t)std::clamp(i, 0, kCurveKnots - 1);
  return mixI(m, wmBiomeI(m.b00, w), wmBiomeI(m.b10, w), wmBiomeI(m.b01, w), wmBiomeI(m.b11, w));
}
static int curveHi() {
  return std::max((wmTerrain(WM_H_TERRAIN_LANDFORM_RANGE) + wmTerrain(WM_H_TERRAIN_RANGE_AMPLITUDE)) / 2, 1);
}
static int curveTangent(int dPrev, int dNext) {
  if (dPrev * dNext <= 0) return 0;
  return (2 * dPrev * dNext) / (dPrev + dNext);
}
static IV2 curveOne(BiomeMix m, int u) {
  const int hi = curveHi();
  const int uc = std::clamp(u, -hi, hi);
  const int p = std::clamp(((uc + hi) * (kCurveSegs << 12)) / (2 * hi), 0, kCurveSegs << 12);
  const int seg = std::min(p >> 12, kCurveSegs - 1);
  const int t = p - (seg << 12);
  const int km = curveKnotAt(m, seg - 1);
  const int k0 = curveKnotAt(m, seg);
  const int k1 = curveKnotAt(m, seg + 1);
  const int k2 = curveKnotAt(m, seg + 2);
  const int d0 = k1 - k0;
  const int m0 = seg == 0 ? d0 : curveTangent(k0 - km, d0);
  const int m1 = seg == kCurveSegs - 1 ? d0 : curveTangent(d0, k2 - k1);
  const int t2 = (t * t) >> 12;
  const int t3 = (t2 * t) >> 12;
  const int h00 = 2 * t3 - 3 * t2 + 4096;
  const int h10 = t3 - 2 * t2 + t;
  const int h01 = 3 * t2 - 2 * t3;
  const int h11 = t3 - t2;
  const int v = (k0 * h00 + m0 * h10 + k1 * h01 + m1 * h11) >> 12;
  const int g00 = 6 * t2 - 6 * t;
  const int g10 = 3 * t2 - 4 * t + 4096;
  const int g01 = 6 * t - 6 * t2;
  const int g11 = 3 * t2 - 2 * t;
  const int dv = (k0 * g00 + m0 * g10 + k1 * g01 + m1 * g11) >> 12;
  return iv2(uc + (((v - (p - 16384)) * hi) >> 14), std::clamp(dv >> 4, 0, 4096));
}
static IV2 biomeCurve(BiomeMix m, int u) {
  if (!worldmap::CurrentWorldMap().Loaded()) return iv2(u, 256);
  return curveOne(m, u);
}
struct BiomeRelief {
  int hill;
  int detail;
  int grain;
};
static BiomeRelief biomeReliefAt(BiomeMix m) {
  BiomeRelief r;
  r.hill = 256;
  r.detail = 256;
  r.grain = 256;
  if (!worldmap::CurrentWorldMap().Loaded()) return r;
  r.hill = mixI(m, wmBiomeI(m.b00, WM_B_HILL_MUL), wmBiomeI(m.b10, WM_B_HILL_MUL),
                wmBiomeI(m.b01, WM_B_HILL_MUL), wmBiomeI(m.b11, WM_B_HILL_MUL));
  r.detail = mixI(m, wmBiomeI(m.b00, WM_B_DETAIL_MUL), wmBiomeI(m.b10, WM_B_DETAIL_MUL),
                  wmBiomeI(m.b01, WM_B_DETAIL_MUL), wmBiomeI(m.b11, WM_B_DETAIL_MUL));
  r.grain = mixI(m, wmBiomeI(m.b00, WM_B_GRAIN_MUL), wmBiomeI(m.b10, WM_B_GRAIN_MUL),
                 wmBiomeI(m.b01, WM_B_GRAIN_MUL), wmBiomeI(m.b11, WM_B_GRAIN_MUL));
  return r;
}

// ---- the landform plane and the sea level, on the CPU (P4) ----------------
// The twins of worldgen.wgsl's seaLevelY / mapLandformQ8 / mapLandformGx,Gz:
// same arithmetic over the same bytes (worldmap::CurrentWorldMap()). Outside
// the height mirror on both sides; the mirrored landAt calls them by name and
// the `terrain` gate's C1 pass is the per-voxel proof they agree.
static int seaLevelY() { return worldmap::CurrentWorldMap().seaLevelY; }
static int wmLandformCellQ8(int cx, int cz) {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  const int x = std::clamp(cx, 0, m.width - 1), z = std::clamp(cz, 0, m.height - 1);
  return (int)m.landform[(size_t)z * m.width + x] << 8;
}
static int mapLandformQ8(int x, int z) {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (!m.Loaded()) return 32768;
  const uint32_t l = (uint32_t)m.cellLog2;
  const int half = 1 << (l - 1u);
  const int mx = x - half + (m.originCellX << l);
  const int mz = z - half + (m.originCellZ << l);
  const int cx = mx >> l;
  const int cz = mz >> l;
  const int mask = (1 << l) - 1;
  const int fx = mx & mask;
  const int fz = mz & mask;
  const int c00 = wmLandformCellQ8(cx, cz);
  const int c10 = wmLandformCellQ8(cx + 1, cz);
  const int c01 = wmLandformCellQ8(cx, cz + 1);
  const int c11 = wmLandformCellQ8(cx + 1, cz + 1);
  const int a = c00 + (((c10 - c00) * fx) >> l);
  const int b = c01 + (((c11 - c01) * fx) >> l);
  const int v = a + (((b - a) * fz) >> l);
  const int dOut = std::max(std::max(-cx, cx + 1 - m.width), std::max(-cz, cz + 1 - m.height));
  const int fade = std::max(m.oceanFadeCells, 1);
  if (dOut <= 0) return v;
  return (v * std::max(fade - dOut, 0)) / fade;
}
static int mapLandformGx(int x, int z) {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (!m.Loaded()) return 0;
  const uint32_t l = (uint32_t)m.cellLog2;
  const int half = 1 << (l - 1u);
  const int cx = (x - half + (m.originCellX << l)) >> l;
  const int cz = (z - half + (m.originCellZ << l)) >> l;
  const int d = wmLandformCellQ8(cx + 1, cz) - wmLandformCellQ8(cx, cz);
  return (d * wmTerrain(WM_H_TERRAIN_LANDFORM_RANGE)) >> (8u + l);
}
static int mapLandformGz(int x, int z) {
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (!m.Loaded()) return 0;
  const uint32_t l = (uint32_t)m.cellLog2;
  const int half = 1 << (l - 1u);
  const int cx = (x - half + (m.originCellX << l)) >> l;
  const int cz = (z - half + (m.originCellZ << l)) >> l;
  const int d = wmLandformCellQ8(cx, cz + 1) - wmLandformCellQ8(cx, cz);
  return (d * wmTerrain(WM_H_TERRAIN_LANDFORM_RANGE)) >> (8u + l);
}

// ---- the sculpt layer, on the CPU (worldgen.wgsl sculptSample / sculptOctave; P5) ----
// Outside the height mirror on both sides, like the plane readers: the
// mirrored landAt calls sculptOctave by name. Declared here, defined after the
// mirror (it returns the mirror's Oct); the `sculpt` gate compares the two
// implementations per column over a synthetic tile.
struct Oct;
static Oct sculptOctave(int x, int z);

// MIRROR-BEGIN height
// The height chain, mirrored. Everything here is a pure function of (x, z,
// seed) and of the worldgen tuning; nothing reads a material id, which is what
// keeps it comparable token-for-token against the shader. The declarations are
// in the SHADER'S order, because check_invariants.py concatenates the tagged
// blocks in file order and compares the streams.
//
// Every number the terrain used to take from tuning is a MAP word now
// (wmTerrain / wmTerrainU, P-G) and is spelled identically on both sides.
struct Land {
  int h;
  int slope;
  int sed;
};
struct Oct {
  int dev;
  int gx;
  int gz;
};
static Oct octave(int x, int z, uint32_t csl, int amp,
                  int gx, int gz, uint32_t seed) {
  N2 n = vnoise2d(x, z, csl, seed);
  int g = std::abs(gx) + std::abs(gz);
  int att = 65536 / (256 + ((wmTerrain(WM_H_TERRAIN_FBM_ATTEN) * ((g * g) >> 8)) >> 8));
  Oct o;
  o.dev = ((((n.n - 8192) * amp) >> 14) * att) >> 8;
  o.gx = (((n.dx * amp) >> (6u + csl)) * att) >> 8;
  o.gz = (((n.dz * amp) >> (6u + csl)) * att) >> 8;
  return o;
}

static Oct landformOctave(int x, int z) {
  Oct o;
  o.dev = ((mapLandformQ8(x, z) - 32768) * wmTerrain(WM_H_TERRAIN_LANDFORM_RANGE)) >> 16;
  o.gx = mapLandformGx(x, z);
  o.gz = mapLandformGz(x, z);
  return o;
}
static Land landAt(int x, int z, uint32_t seed) {
  BiomeMix bm = biomeMixAt(x, z);
  BiomeRelief rl = biomeReliefAt(bm);
  Oct o0 = landformOctave(x, z);
  Oct o1 = octave(x, z, wmTerrainU(WM_H_TERRAIN_RANGE_LOG2), wmTerrain(WM_H_TERRAIN_RANGE_AMPLITUDE),
                  o0.gx, o0.gz, seed ^ 2u);
  // The per-biome height curve, on the two COARSE rungs only. cv.y is the
  // curve's own slope in Q8 and it scales the accumulated gradient rather than
  // the deviation, so iq's attenuation of hill/detail/grain keeps describing
  // the ground it is actually attenuating against. See worldgen.wgsl.
  const IV2 cv = biomeCurve(bm, o0.dev + o1.dev);
  int g1x = ((o0.gx + o1.gx) * cv.y) >> 8;
  int g1z = ((o0.gz + o1.gz) * cv.y) >> 8;
  Oct o2 = octave(x, z, wmTerrainU(WM_H_TERRAIN_HILL_LOG2), (wmTerrain(WM_H_TERRAIN_HILL_AMPLITUDE) * rl.hill) >> 8,
                  g1x, g1z, seed ^ 3u);
  int g2x = g1x + o2.gx;
  int g2z = g1z + o2.gz;
  Oct o3 = octave(x, z, wmTerrainU(WM_H_TERRAIN_DETAIL_LOG2), (wmTerrain(WM_H_TERRAIN_DETAIL_AMPLITUDE) * rl.detail) >> 8,
                  g2x, g2z, seed ^ 4u);
  int g3x = g2x + o3.gx;
  int g3z = g2z + o3.gz;
  Oct o4 = octave(x, z, wmTerrainU(WM_H_TERRAIN_GRAIN_LOG2), (wmTerrain(WM_H_TERRAIN_GRAIN_AMPLITUDE) * rl.grain) >> 8,
                  g3x, g3z, seed ^ 5u);

  IV2 sc = spawnCentre();
  int fade = wmTerrain(WM_H_TERRAIN_HOME_FADE);
  int d = std::max(std::abs(x - sc.x), std::abs(z - sc.y)) - wmTerrain(WM_H_TERRAIN_HOME_R);
  int w = 16384;
  if (d < fade) {
    w = (std::max(d, 0) * 16384) / fade;
  }
  int dh = padOutside(x, z);
  int wh = 16384;
  if (dh < fade) {
    wh = (dh * 16384) / fade;
  }
  int ws = vsmooth(std::min(w, wh) << 1) >> 1;
  int homeY = wmTerrain(WM_H_TERRAIN_HOME_Y);
  int coarse = wmTerrain(WM_H_TERRAIN_BASE_HEIGHT) + cv.x - homeY;
  Oct sp = sculptOctave(x, z);
  Soften sf = softenAt(x, z);
  int bed = homeY + ((o2.dev * sf.hill) >> 8) + ((o3.dev * sf.bump) >> 8) + ((o4.dev * sf.grain) >> 8)
          + ((coarse * ws) >> 14) + sp.dev;

  int slope = std::abs(g1x + ((o2.gx * sf.hill) >> 8) + sp.gx) + std::abs(g1z + ((o2.gz * sf.hill) >> 8) + sp.gz);
  int room = std::max(0, wmTerrain(WM_H_TERRAIN_SED_CEIL) - bed);
  int sed = ((room * wmTerrain(WM_H_TERRAIN_SED_FRACTION)) >> 8) - wmTerrain(WM_H_TERRAIN_SED_STRIP);
  int sedSlope = wmTerrain(WM_H_TERRAIN_SED_SLOPE);
  sed = (std::max(sed, 0) * std::max(sedSlope - slope, 0)) /
        std::max(sedSlope, 1);
  sed = std::clamp(sed, 0, wmTerrain(WM_H_TERRAIN_SED_MAX));
  if (bed < seaLevelY()) { sed = 0; }

  Land l;
  l.h = bed + sed;
  l.slope = slope;
  l.sed = sed;
  return l;
}
static int baseHeight(int x, int z, uint32_t seed) {
  return landAt(x, z, seed).h;
}
struct Pond {
  bool present;
  bool authored;
  int cx;
  int cz;
  int r;
  int surf;
  uint32_t wp;
  int minY;
  int maxY;
  int maxSlope;
};

static Pond pondNone() {
  Pond p;
  p.present = false; p.authored = false; p.cx = 0; p.cz = 0; p.r = 0; p.surf = -1; p.wp = 0u;
  p.minY = -1; p.maxY = -1; p.maxSlope = 1024;
  return p;
}

struct PondSet {
  int n;
  Pond d0;
  Pond d1;
  Pond d2;
  Pond d3;
  Pond d4;
};

static PondSet pondSetNone() {
  PondSet s;
  s.n = 0;
  s.d0 = pondNone(); s.d1 = pondNone(); s.d2 = pondNone(); s.d3 = pondNone(); s.d4 = pondNone();
  return s;
}

static PondSet setPush(PondSet s, Pond p) {
  PondSet q = s;
  if (q.n == 0) { q.d0 = p; } else if (q.n == 1) { q.d1 = p; } else if (q.n == 2) { q.d2 = p; }
  else if (q.n == 3) { q.d3 = p; } else { q.d4 = p; }
  q.n = q.n + 1;
  return q;
}

static int bowlDepth(uint32_t wp, int r, int d2) {
  const int r2 = std::max(r * r, 1);
  const int s = std::min(d2, r2) * 16;
  const int u = std::min(s / r2, 15);
  const int frac = s - u * r2;
  const int k0 = waterKnot(wp, (uint32_t)(u));
  const int k1 = waterKnot(wp, (uint32_t)(u) + 1u);
  const int f = k0 - ((k0 - k1) * frac) / r2;
  const int rd = wmWaterI(wp, WM_W_RIM_DEPTH);
  return rd + ((wmWaterI(wp, WM_W_DEPTH) - rd) * f) / 256;
}

static int isqrtLe(int v, int hi0) {
  int lo = 0;
  int hi = hi0;
  for (int i = 0; i < 12; i++) {
    if (lo >= hi) { break; }
    const int mid = (lo + hi + 1) / 2;
    if (mid * mid <= v) { lo = mid; } else { hi = mid - 1; }
  }
  return lo;
}

static bool bowlSteep(Pond p, int x, int z) {
  const int dx = x - p.cx;
  const int dz = z - p.cz;
  const int d2 = dx * dx + dz * dz;
  const int d = isqrtLe(d2, p.r) + 1;
  const int here = bowlDepth(p.wp, p.r, d2);
  const int out = bowlDepth(p.wp, p.r, std::min(d * d, p.r * p.r));
  return here - out > 1;
}

static Pond waterSiteNear(int x, int z) {
  Pond p = pondNone();
  const uint32_t sid = wmWaterSiteAt(x, z);
  if (sid == 0u) { return p; }
  p.cx = wmSiteI(sid, WM_S_X);
  p.cz = wmSiteI(sid, WM_S_Z);
  p.r = wmSiteI(sid, WM_S_RADIUS);
  p.wp = (uint32_t)(wmSiteI(sid, WM_S_PRESET));
  p.present = true;
  p.authored = true;
  return p;
}

static bool waterRowHit(uint32_t biome, uint32_t i, int pt, int pz, uint32_t seed) {
  const uint32_t hRow = hash3(seed ^ (0xB0A7u + i * 0x9E37u), (uint32_t)(pt), (uint32_t)(pz));
  return (int)(hRow & 0xFFFFu) < wmWaterRow(biome, i, WM_R_CHANCE_Q16);
}

static Pond pondRoll(int pt, int pz, uint32_t seed) {
  Pond p = pondNone();
  const int tile = pondTile();
  if (tile <= 0) { return p; }
  const uint32_t rh = hash3(seed ^ 0xB0A7u, (uint32_t)(pt), (uint32_t)(pz));
  const uint32_t span = (uint32_t)(std::max(tile / 2, 1));
  int cx = pt * tile + tile / 4 + (int)((rh >> 9u) % span);
  int cz = pz * tile + tile / 4 + (int)((rh >> 17u) % span);
  const uint32_t biome = biomeCellAt(cx, cz);
  const uint32_t n = wmWaterRowCount(biome);
  if (n == 0u) { return p; }
  uint32_t row = 4u;
  if (n > 3u && waterRowHit(biome, 3u, pt, pz, seed)) { row = 3u; }
  if (n > 2u && waterRowHit(biome, 2u, pt, pz, seed)) { row = 2u; }
  if (n > 1u && waterRowHit(biome, 1u, pt, pz, seed)) { row = 1u; }
  if (n > 0u && waterRowHit(biome, 0u, pt, pz, seed)) { row = 0u; }
  if (row == 4u) { return p; }
  const uint32_t wp = (uint32_t)(wmWaterRow(biome, row, WM_R_PRESET));
  if (wp == 0u) { return p; }
  const int r = wmWaterI(wp, WM_W_RADIUS_MIN) + (int)((((rh >> 4u) & 0xFFFFu) * (uint32_t)(wmWaterI(wp, WM_W_RADIUS_SPAN))) >> 16u);
  const int inset = r + 4;
  if (tile - 2 * inset < 1) { return p; }
  cx = std::clamp(cx, pt * tile + inset, pt * tile + tile - 1 - inset);
  cz = std::clamp(cz, pz * tile + inset, pz * tile + tile - 1 - inset);
  if (siteKeepOut(cx, cz) || sitePadNear(cx, cz, r)) { return p; }
  p.present = true; p.cx = cx; p.cz = cz; p.r = r; p.wp = wp;
  p.minY = wmWaterRow(biome, row, WM_R_MIN_Y);
  p.maxY = wmWaterRow(biome, row, WM_R_MAX_Y);
  p.maxSlope = wmWaterRow(biome, row, WM_R_MAX_SLOPE);
  return p;
}

static PondSet pondScan(int x, int z, uint32_t seed) {
  PondSet s = pondSetNone();
  const Pond a = waterSiteNear(x, z);
  if (a.present) { s = setPush(s, a); }
  const int tile = pondTile();
  if (tile <= 0) { return s; }
  const int band = pondBand();
  const int pt = fdiv(x, tile);
  const int pz = fdiv(z, tile);
  const int lx = fmodp(x, tile);
  const int lz = fmodp(z, tile);
  const int sx = select(select(0, 1, lx >= tile - band), -1, lx < band);
  const int sz = select(select(0, 1, lz >= tile - band), -1, lz < band);
  const Pond p0 = pondRoll(pt, pz, seed);
  if (p0.present) { s = setPush(s, p0); }
  if (sx != 0) {
    const Pond p1 = pondRoll(pt + sx, pz, seed);
    if (p1.present) { s = setPush(s, p1); }
  }
  if (sz != 0) {
    const Pond p2 = pondRoll(pt, pz + sz, seed);
    if (p2.present) { s = setPush(s, p2); }
  }
  if (sx != 0 && sz != 0) {
    const Pond p3 = pondRoll(pt + sx, pz + sz, seed);
    if (p3.present) { s = setPush(s, p3); }
  }
  return s;
}

static Pond pondGate(Pond q, uint32_t seed) {
  Pond p = q;
  if (!p.present) { return p; }
  const Land c = landAt(p.cx, p.cz, seed);
  p.surf = c.h;
  if (p.authored) { return p; }
  if (p.minY >= 0 && c.h < p.minY) { return pondNone(); }
  if (p.maxY >= 0 && c.h > p.maxY) { return pondNone(); }
  if (c.slope > p.maxSlope) { return pondNone(); }
  if (c.slope * p.r > (wmWaterI(p.wp, WM_W_DEPTH) - wmWaterI(p.wp, WM_W_RIM_DEPTH)) * 256) { return pondNone(); }
  return p;
}

static int bermLift(uint32_t wp, int h, int surf, int past) {
  const int bw = wmWaterI(wp, WM_W_BERM_W);
  const int bh = wmWaterI(wp, WM_W_BERM_H);
  const int core = std::max(bw / 4, 2);
  if (past < core) { return std::max(h, surf + bh); }
  const int span = std::max(bw - core, 1);
  const int t = span - (past - core);
  if (t <= 0) { return h; }
  return std::max(h, h + ((surf + bh - h) * t) / span);
}

static bool inDisc(Pond p, int x, int z) {
  const int dx = x - p.cx;
  const int dz = z - p.cz;
  return p.present && dx * dx + dz * dz <= p.r * p.r;
}

static bool pondCovers(PondSet s, int x, int z) {
  return inDisc(s.d0, x, z) || inDisc(s.d1, x, z) || inDisc(s.d2, x, z) ||
         inDisc(s.d3, x, z) || inDisc(s.d4, x, z);
}

static Pond pondCover(PondSet s, int x, int z, uint32_t seed) {
  Pond cand = pondNone();
  if (inDisc(s.d4, x, z)) { cand = s.d4; }
  if (inDisc(s.d3, x, z)) { cand = s.d3; }
  if (inDisc(s.d2, x, z)) { cand = s.d2; }
  if (inDisc(s.d1, x, z)) { cand = s.d1; }
  if (inDisc(s.d0, x, z)) { cand = s.d0; }
  return pondGate(cand, seed);
}

static IV2 bowlAt(Pond p, int x, int z) {
  if (!p.present) { return iv2(-1, -1); }
  const int dx = x - p.cx;
  const int dz = z - p.cz;
  const int depth = bowlDepth(p.wp, p.r, dx * dx + dz * dz);
  return iv2(p.surf - depth, p.surf);
}

static int candD2(Pond p, int x, int z, int band) {
  if (!p.present) { return 0x7FFFFFFF; }
  const int dx = x - p.cx;
  const int dz = z - p.cz;
  const int d2 = dx * dx + dz * dz;
  const int outer = p.r + band;
  if (d2 > outer * outer) { return 0x7FFFFFFF; }
  return d2;
}
static int shoreD2(Pond p, int x, int z) {
  return candD2(p, x, z, wmWaterI(p.wp, WM_W_BAND));
}
struct Shore {
  bool onShore;
  int past;
  int surf;
  uint32_t wp;
};

static Shore pondNear(PondSet s, int x, int z, uint32_t seed) {
  Shore sh;
  sh.onShore = false; sh.past = 0; sh.surf = -1; sh.wp = 0u;
  if (pondCovers(s, x, z)) { return sh; }
  int best = 0x7FFFFFFF;
  Pond bestP = pondNone();
  const int e0 = shoreD2(s.d0, x, z);
  if (e0 < best) { best = e0; bestP = s.d0; }
  const int e1 = shoreD2(s.d1, x, z);
  if (e1 < best) { best = e1; bestP = s.d1; }
  const int e2 = shoreD2(s.d2, x, z);
  if (e2 < best) { best = e2; bestP = s.d2; }
  const int e3 = shoreD2(s.d3, x, z);
  if (e3 < best) { best = e3; bestP = s.d3; }
  const int e4 = shoreD2(s.d4, x, z);
  if (e4 < best) { best = e4; bestP = s.d4; }
  const Pond g = pondGate(bestP, seed);
  if (!g.present) { return sh; }

  int lo = 0;
  int hi = wmWaterI(g.wp, WM_W_BAND);
  for (int i = 0; i < 8; i++) {
    if (lo >= hi) { break; }
    const int mid = (lo + hi) / 2;
    const int rr = g.r + mid;
    if (best <= rr * rr) { hi = mid; } else { lo = mid + 1; }
  }
  sh.onShore = true;
  sh.past = std::max(lo - 1, 0);
  sh.surf = g.surf;
  sh.wp = g.wp;
  return sh;
}
// MIRROR-END height

// The same arithmetic as the shader over WorldMapData::sculpt, the block
// PackWorldMap appends (offsets relative to its first word, so base 0 here).
static int sculptSample(const std::vector<uint32_t>& blk, uint32_t t, int k) {
  const uint32_t w = blk[t + (uint32_t)(k >> 1)];
  const uint32_t sh = (uint32_t)(k & 1) * 16u;
  return (int32_t)(w << (16u - sh)) >> 16;
}
static Oct sculptOctave(int x, int z) {
  using namespace worldmap;
  Oct o;
  o.dev = 0;
  o.gx = 0;
  o.gz = 0;
  const std::vector<uint32_t>& blk = CurrentWorldMap().sculpt;
  if (blk.empty()) return o;
  const int sx = x >> kSculptSpacingLog2;
  const int sz = z >> kSculptSpacingLog2;
  const int tx = sx >> kSculptTileLog2;
  const int tz = sz >> kSculptTileLog2;
  const int rx = (tx >> kSculptRegionLog2) - (int32_t)blk[kSc_RegionX0];
  const int rz = (tz >> kSculptRegionLog2) - (int32_t)blk[kSc_RegionZ0];
  const int rw = (int)blk[kSc_RegionW];
  if (rx < 0 || rz < 0 || rx >= rw || rz >= (int)blk[kSc_RegionH]) return o;
  const uint32_t dir = blk[kSculptHdrWords + (uint32_t)(rz * rw + rx)];
  if (dir == 0u) return o;
  const int rmask = (1 << kSculptRegionLog2) - 1;
  const uint32_t body = blk[dir + (uint32_t)(((tz & rmask) << kSculptRegionLog2) + (tx & rmask))];
  if (body == 0u) return o;
  const int smask = (1 << kSculptTileLog2) - 1;
  const int side = (int)kSculptTileSide;
  const int k = (sz & smask) * side + (sx & smask);
  const int s00 = sculptSample(blk, body, k);
  const int s10 = sculptSample(blk, body, k + 1);
  const int s01 = sculptSample(blk, body, k + side);
  const int s11 = sculptSample(blk, body, k + side + 1);
  const int one = 1 << kSculptSpacingLog2;
  const int fx = x & (one - 1);
  const int fz = z & (one - 1);
  const int a = s00 + (((s10 - s00) * fx) >> kSculptSpacingLog2);
  const int b = s01 + (((s11 - s01) * fx) >> kSculptSpacingLog2);
  o.dev = a + (((b - a) * fz) >> kSculptSpacingLog2);
  o.gx = ((s10 - s00) * (one - fz) + (s11 - s01) * fz) << (8u - 2u * kSculptSpacingLog2);
  o.gz = ((s01 - s00) * (one - fx) + (s11 - s10) * fx) << (8u - 2u * kSculptSpacingLog2);
  return o;
}

// Fluid-lab flat-slab mode (world.h kLabSlabY). Process-wide, set once at
// startup by --lab / --fluid-bench, mirrored to the GPU as TickParams.labMode.
static bool sLabWorld = false;
void World::SetLabWorld(bool on) { sLabWorld = on; }
bool World::LabWorld() { return sLabWorld; }

// The bare ground under a pad / stamp site's centre (worldgen.wgsl sitePadY):
// LoadWorldMap's bake for the load seed. The lab slab is the one world where
// the centre column is not that terrain -- the shader's landColumnBare returns
// the slab there -- so the lab answers the slab, as the per-call form did.
static int sitePadY(uint32_t sid) {
  if (sLabWorld) { return kLabSlabY; }
  return wmSiteI(sid, WM_S_PAD_Y);
}


// MIRROR-BEGIN landheight
// THE HEIGHT CONTRACT (DESIGN.md; landColumn in worldgen.wgsl):
//
//     World::TerrainHeight(x, z, seed)  ==  genColumn(x, z, seed).h,  exactly,
//     for all inputs.
//
// Not "the topmost solid voxel" — that includes canopy, ruin walls, grass tufts
// and the arena deck, and it cannot be mirrored cheaply (a tree tile scan in a
// tick path). Every one of this function's ~30 callers is asking where the
// GROUND is so it can stand something on it, and this is that.
//
// This is the ONE function on the CPU side that the shader's landColumn is not
// token-compared against — it branches on a process-wide bool where the shader
// branches on a uniform, and it discards the fields the CPU has no use for.
// check_invariants.py instead compares the two blocks' INTEGER LITERALS as a
// multiset, which is the drift that actually happens here: the deleted
// surfHeightAt was a copy of exactly this arithmetic and it had already gone
// stale. The `terrain` gate's pass C1 is the per-voxel proof.
//
// COST: ~25 hash3 (two octaves, one pond tile, one pond centre, up to four
// neighbour tiles, one more centre) for the bare column; a site pad adds no
// second column (its centre height is baked at map load, sitePadY). That is
// fine at O(1) per frame — spawn
// placement, fixture anchoring, a mob ground probe. NEVER call it in a
// per-voxel loop; the GPU has genColumn for that and it is hoisted per column.
//
// The ground BEFORE the ruin pad, which is what a pad's corner samples want.
// Mirrors landColumnBare in worldgen.wgsl; `wet` is "under a bowl or the
// sea", the facts ruinPad refuses a site on.
struct BareCol {
  int h;
  bool wet;
};
static BareCol landColumnBare(int x, int z, uint32_t seed) {
  const Land land = landAt(x, z, seed);
  const int bed = land.h - land.sed;
  // The disc tests come first because the SEDIMENT decision needs them and
  // the wedge lives inside `h`.
  const PondSet s = pondScan(x, z, seed);
  const Pond pc = pondCover(s, x, z, seed);
  IV2 pw = bowlAt(pc, x, z);
  Shore near;
  near.onShore = false; near.past = 0; near.surf = -1; near.wp = 0u;
  if (pw.y < 0) { near = pondNear(s, x, z, seed); }

  // The wedge, ramped out across a tarn's bank rather than switched off at its
  // edge — a hard switch is a cliff of loose gravel over a bowl of sand. The
  // band is the near pond's preset's (P-F).
  int sed = land.sed;
  if (pw.y >= 0) {
    sed = 0;
  } else if (near.onShore) {
    const int band = std::max(wmWaterI(near.wp, WM_W_BAND), 1);
    sed = (sed * std::min(near.past, band)) / band;
  }
  int h = bed + sed;
  // Disc ponds: the bowl REPLACES the ground inside (see the block over the
  // same line in landColumn — as a min() the bed was raw hillside wherever the
  // terrain undercut the bowl, and genCellIn lays sand on it), berm outside.
  if (pw.y >= 0) {
    h = pw.x;
  } else if (near.onShore && near.past < wmWaterI(near.wp, WM_W_BERM_W)) {
    h = bermLift(near.wp, h, near.surf, near.past);
  }
  BareCol b;
  b.h = h;
  b.wet = (pw.y >= 0 || h < seaLevelY());
  return b;
}

static int sitePadAt(int x, int z, int h) {
  const uint32_t* lst = wmSiteList(x, z);
  const uint32_t n = wmSiteN(lst);
  int hh = h;
  for (uint32_t k = 0u; k < n; k++) {
    const uint32_t sid = wmSiteK(lst, k);
    const int w = sitePadWeight(sid, x, z);
    if (w <= 0) { continue; }
    hh = hh + (((sitePadY(sid) - hh) * w + 128) >> 8u);
  }
  return hh;
}

int World::TerrainHeight(int x, int z, uint32_t seed) {
  // Lab slab: the same guard landColumn takes in worldgen.wgsl. Before the
  // tuning reads on purpose — the lab surface must not move when worldgen
  // knobs are tuned, or every scene's fixture heights drift.
  if (sLabWorld) return kLabSlabY;
  const int h = landColumnBare(x, z, seed).h;
  return sitePadAt(x, z, h);
}
// MIRROR-END landheight

// worldmap.h: LoadWorldMap's site-pad bake, against whatever map is current.
int worldmap::BareGroundHeight(int x, int z, uint32_t seed) {
  return landColumnBare(x, z, seed).h;
}


// The map probe (world.h Column). Composed from the SAME functions the height
// contract is built out of rather than re-deriving anything — `landAt` for the
// wedge and the landform gradient, `TerrainHeight` for the ground, and the
// covering pond (rolled or an authored water site) for standing water. A separate implementation here
// would be the fourth copy of the terrain, and the deleted `surfHeightAt` is
// the file's own evidence for how that ends.
World::Column World::TerrainColumn(int x, int z, uint32_t seed) {
  Column c{};
  c.h = TerrainHeight(x, z, seed);
  c.water = INT32_MIN;
  if (sLabWorld) return c;
  const Land l = landAt(x, z, seed);
  c.slope = l.slope;
  // The wedge as it SURVIVED the overrides, not as landAt proposed it: a
  // bermed or bowl-carved column reports bare ground, which is what genCellIn
  // will actually lay there.
  const IV2 pw = bowlAt(pondCover(pondScan(x, z, seed), x, z, seed), x, z);
  c.sed = (pw.y >= 0) ? 0 : l.sed;
  if (pw.y >= 0) c.water = pw.y;                          // a tarn or lake
  else if (c.h < seaLevelY()) c.water = seaLevelY();      // the sea (P4)
  return c;
}

// The same branch landColumn takes, reported instead of applied. Kept adjacent
// to TerrainHeight on purpose: if one grows a case the other has to, and the
// `terrain` gate's berm assertion is only meaningful while they agree.
World::PondQuery World::PondNearColumn(int x, int z, uint32_t seed) {
  PondQuery q{};
  q.surf = -1;
  if (sLabWorld) return q;
  const PondSet s = pondScan(x, z, seed);
  const Pond pc = pondCover(s, x, z, seed);
  const IV2 pw = bowlAt(pc, x, z);
  if (pw.y >= 0) {
    q.inDisc = true;
    q.surf = pw.y;
    q.preset = pc.wp;
    q.bermH = wmWaterI(pc.wp, WM_W_BERM_H);
    q.bermW = wmWaterI(pc.wp, WM_W_BERM_W);
    q.band = wmWaterI(pc.wp, WM_W_BAND);
    return q;
  }
  const Shore near = pondNear(s, x, z, seed);
  q.near = near.onShore;
  q.past = near.past;
  q.surf = near.surf;
  q.preset = near.wp;
  q.bermH = wmWaterI(near.wp, WM_W_BERM_H);
  q.bermW = wmWaterI(near.wp, WM_W_BERM_W);
  q.band = wmWaterI(near.wp, WM_W_BAND);
  return q;
}

// A PondDisc from a mirrored Pond: the disc plus the geometry its preset
// gives it, for the basin registry and the gates. One place, so nothing
// downstream re-reads the table by hand.
static World::PondDisc DiscOf(const Pond& p) {
  World::PondDisc d;
  if (!p.present) return d;
  d.present = true;
  d.cx = p.cx;
  d.cz = p.cz;
  d.r = p.r;
  d.surf = p.surf;
  d.preset = p.wp;
  d.depth = wmWaterI(p.wp, WM_W_DEPTH);
  d.rimDepth = wmWaterI(p.wp, WM_W_RIM_DEPTH);
  d.bermH = wmWaterI(p.wp, WM_W_BERM_H);
  d.bermW = wmWaterI(p.wp, WM_W_BERM_W);
  d.band = wmWaterI(p.wp, WM_W_BAND);
  d.fillId = wmWater(p.wp, WM_W_FILL);
  return d;
}

// ---- the basin registry's source (world.h PondDisc) --------------------------
//
// Thin publishers, on purpose. Every literal and every gate below already
// exists above — `pondInfo` inside the token-compared MIRROR block, the pool
// discs inside TerrainHeight — and these hand them out rather than restating
// them. A basin registry that re-derived the tile hash would be the fourth copy
// of the terrain (see the accessor comment in world.h).

World::PondDisc World::PondTile(int tileX, int tileZ, uint32_t seed) {
  // The lab slab has no ponds at all: genColumn's labMode branch returns
  // pond = -1 for every column, so a registry that reported one would describe
  // water the world does not contain.
  if (sLabWorld) return PondDisc{};
  return DiscOf(pondGate(pondRoll(tileX, tileZ, seed), seed));
}

int World::PondTileSize() { return pondTile(); }

// The authored lakes (P-F): every kSiteWater record of the loaded map, with
// its waterline from the same mirrored pondSurf the shader runs.
int World::WaterSiteCount() {
  int n = 0;
  for (const worldmap::WorldMapData::StampSite& s : worldmap::CurrentWorldMap().sites)
    if (s.kind == worldmap::kSiteWater) n++;
  return n;
}
int World::WaterSiteIndex(const std::string& id) {
  int n = 0;
  for (const worldmap::WorldMapData::StampSite& s : worldmap::CurrentWorldMap().sites) {
    if (s.kind != worldmap::kSiteWater) continue;
    if (s.id == id) return n;
    n++;
  }
  return -1;
}
World::PondDisc World::WaterSiteDisc(int index, uint32_t seed) {
  if (sLabWorld) return PondDisc{};
  int n = 0;
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  for (size_t i = 0; i < m.sites.size(); i++) {
    const worldmap::WorldMapData::StampSite& s = m.sites[i];
    if (s.kind != worldmap::kSiteWater) continue;
    if (n++ != index) continue;
    Pond p = pondNone();
    p.present = true;
    p.cx = s.x; p.cz = s.z; p.r = s.radius; p.wp = (uint32_t)s.preset;
    p.surf = landAt(p.cx, p.cz, seed).h;   // the same waterline waterSiteAt gives it
    return DiscOf(p);
  }
  return PondDisc{};
}

int World::BowlDepth(uint32_t preset, int r, int d2) { return bowlDepth(preset, r, d2); }

int World::PondReachMax() {
  int reach = 0;
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  for (const worldmap::WaterGeom& g : m.water)
    reach = std::max(reach, g.radiusMin + g.radiusSpan - 1 + g.band);
  for (const worldmap::WorldMapData::StampSite& s : m.sites)
    if (s.kind == worldmap::kSiteWater) reach = std::max(reach, s.radius + s.padMargin);
  return reach + 4;
}

// ---- MPM fluid render bounds (RenderParams::fluidLo/fluidHi) ---------------
// See the RenderParams block in world.h. Render-only DERIVED data: no sim
// kernel and no sim decision reads either of these, so the readback latency
// they ride is a picture question, never a determinism one.

uint32_t World::QueueGasSpawns(const GasSpawnOp* ops, uint32_t n) {
  const size_t room = kGasCpuSpawnPerTick - pendingGasSpawns_.size();
  const uint32_t take = (uint32_t)std::min<size_t>(n, room);
  pendingGasSpawns_.insert(pendingGasSpawns_.end(), ops, ops + take);
  return take;
}

void World::TakeGasSpawns(std::vector<GasSpawnOp>& out) {
  out.swap(pendingGasSpawns_);
  pendingGasSpawns_.clear();
}

void World::NoteFluidSpawnBounds(const FluidSpawnOp* ops, uint32_t n,
                                 uint32_t tick) {
  if (n == 0) return;
  IVec3 lo{INT32_MAX, INT32_MAX, INT32_MAX};
  IVec3 hi{INT32_MIN, INT32_MIN, INT32_MIN};
  for (uint32_t i = 0; i < n; i++) {
    const IVec3 c{ops[i].px >> 16, ops[i].py >> 16, ops[i].pz >> 16};  // Q16.16
    lo.x = std::min(lo.x, c.x); lo.y = std::min(lo.y, c.y); lo.z = std::min(lo.z, c.z);
    hi.x = std::max(hi.x, c.x); hi.y = std::max(hi.y, c.y); hi.z = std::max(hi.z, c.z);
  }
  // Union with the box already held UNLESS it has expired: a pour that walks
  // (the dev tool's brush) must not drag a stale tail across the world.
  const bool live = spawnBoxHi_.x >= spawnBoxLo_.x &&
                    tick - spawnBoxTick_ < kFluidSpawnBoundsTicks;
  if (live) {
    lo.x = std::min(lo.x, spawnBoxLo_.x); lo.y = std::min(lo.y, spawnBoxLo_.y);
    lo.z = std::min(lo.z, spawnBoxLo_.z);
    hi.x = std::max(hi.x, spawnBoxHi_.x); hi.y = std::max(hi.y, spawnBoxHi_.y);
    hi.z = std::max(hi.z, spawnBoxHi_.z);
  }
  spawnBoxLo_ = lo;
  spawnBoxHi_ = hi;
  spawnBoxTick_ = tick;
}

bool World::FluidRenderBounds(uint32_t tick, IVec3& lo, IVec3& hi) const {
  // No snapshot yet: hand back the whole window. The one thing this must never
  // do is clip water the CPU cannot see.
  if (!snap_.valid) {
    lo = {origin_.x * (int)kChunk, origin_.y * (int)kChunk,
          origin_.z * (int)kChunk};
    hi = {lo.x + (int)kWorldN - 1, lo.y + (int)kWorldN - 1,
          lo.z + (int)kWorldN - 1};
    return true;
  }
  lo = {INT32_MAX, INT32_MAX, INT32_MAX};
  hi = {INT32_MIN, INT32_MIN, INT32_MIN};
  // The active block list: every chunk the solver allocated node storage for
  // as of that snapshot — i.e. every chunk whose grid can hold fluid mass.
  const uint32_t nb = std::min<uint32_t>(
      snap_.fluidBlockCount, (uint32_t)snap_.fluidBlocks.size());
  for (uint32_t i = 0; i < nb; i++) {
    const IVec3 wc = SlotToWorldChunk(snap_.fluidBlocks[i]);
    const IVec3 c0{wc.x * (int)kChunk, wc.y * (int)kChunk, wc.z * (int)kChunk};
    lo.x = std::min(lo.x, c0.x); lo.y = std::min(lo.y, c0.y); lo.z = std::min(lo.z, c0.z);
    hi.x = std::max(hi.x, c0.x + (int)kChunk - 1);
    hi.y = std::max(hi.y, c0.y + (int)kChunk - 1);
    hi.z = std::max(hi.z, c0.z + (int)kChunk - 1);
  }
  if (spawnBoxHi_.x >= spawnBoxLo_.x &&
      tick - spawnBoxTick_ < kFluidSpawnBoundsTicks) {
    lo.x = std::min(lo.x, spawnBoxLo_.x); lo.y = std::min(lo.y, spawnBoxLo_.y);
    lo.z = std::min(lo.z, spawnBoxLo_.z);
    hi.x = std::max(hi.x, spawnBoxHi_.x); hi.y = std::max(hi.y, spawnBoxHi_.y);
    hi.z = std::max(hi.z, spawnBoxHi_.z);
  }
  if (hi.x < lo.x) {               // no blocks, no recent pour: nothing to draw
    lo = {0, 0, 0};
    hi = {-1, -1, -1};             // the canonical empty box the shader tests
    return false;
  }
  lo.x -= kFluidRenderPadVox; lo.y -= kFluidRenderPadVox; lo.z -= kFluidRenderPadVox;
  hi.x += kFluidRenderPadVox; hi.y += kFluidRenderPadVox; hi.z += kFluidRenderPadVox;
  return true;
}
