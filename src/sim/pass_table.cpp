// pass_table.cpp — expands src/sim/pass_table.def into the constexpr row array.
//
// The macro names here are the vocabulary the .def is written in, and are also
// what scripts/check_pass_table.py's regexes look for. Renaming one means
// updating the checker in the same commit.
//
// The .def is expanded TWICE, with different macro definitions: once to build
// the rows, once to count each row's `uses` entries. A braced initializer
// cannot portably report its own length, and a padding entry is
// indistinguishable from a real `R(Voxels)` — so rather than guess, the count
// comes from the same single list under a counting expansion. One source, two
// expansions, no possibility of drift.

#include "sim/pass_table.h"
#include "sim/world.h"

namespace {
// sim_mutate.wgsl `rainFall`: one thread per RAIN_TILE x RAIN_TILE KEY tile of
// the tick's fall-line lattice (src/sim/rainexpo.h), 64 to a workgroup; the
// count follows the rain slope (D_RAINFALL, Simulation::SetTickRain). At a
// vertical fall it is the window's column tiles, kRainFallGroupsFlat.
constexpr uint32_t kRainTile = 8;
constexpr uint32_t kRainFallGroupsFlat = (kWorldN / kRainTile) * (kWorldN / kRainTile) / 64;
static_assert(kRainFallGroupsFlat == 64, "RecordCtx::rainFallGroups' default");
// The sky bound's reduce (shadow_resolve.wgsl skyTopReduce): one thread per far
// occupancy word, 64 per group, and a group never straddles two levels.
static_assert((kFarNumChunks % 64u) == 0u, "skyTopReduce groups straddle levels");
constexpr uint32_t kSkyTopGroups = kFarLevels * kFarNumChunks / 64;
}  // namespace

namespace pass {
namespace {

// ---- the .def's vocabulary, as macros ------------------------------------
#define PT_TICK       Table::Tick
#define PT_WORLDGEN   Table::Worldgen
#define PT_GENLIST    Table::GenList
#define PT_LOADRESET  Table::LoadReset
#define PT_HASHONLY   Table::HashOnly
#define PT_FARFILL    Table::FarFill
#define PT_FLUIDMAP   Table::FluidMap
#define PT_FLUID      Table::Fluid
#define PT_FLUIDSEAM  Table::FluidSeam
#define PT_FLUIDSETTLE Table::FluidSettle
#define PT_PAGEFILL   Table::PageFill
#define PT_SHADOWCACHE Table::ShadowCache
#define PT_SOLEVICT   Table::SolEvict
#define PT_SOLRESTORE Table::SolRestore

#define PIPE_NONE            Pipe::None
#define PIPE_WORLDGEN        Pipe::Worldgen
#define PIPE_WORLDGEN_LIST   Pipe::WorldgenList
#define PIPE_WORLDGEN_COLS   Pipe::WorldgenCols
#define PIPE_MUTATE          Pipe::Mutate
#define PIPE_MUTATE_CELLS    Pipe::MutateCells
#define PIPE_WIND_WAKE       Pipe::WindWake
#define PIPE_RAIN_FALL       Pipe::RainFall
#define PIPE_RAIN_EXPO       Pipe::RainExpo
#define PIPE_COMPACT         Pipe::Compact
#define PIPE_COMPACT_NEXT    Pipe::CompactNext
#define PIPE_STEP            Pipe::Step
#define PIPE_REPOSE_SNAP     Pipe::ReposeSnap
#define PIPE_OCCUPANCY       Pipe::Occupancy
#define PIPE_OCCUPANCY_DIRTY Pipe::OccupancyDirty
#define PIPE_PICK            Pipe::Pick
#define PIPE_EXPLODE_MARK    Pipe::ExplodeMark
#define PIPE_EXPLODE_APPLY   Pipe::ExplodeApply
#define PIPE_P_ARGS1         Pipe::PArgs1
#define PIPE_P_SPAWN         Pipe::PSpawn
#define PIPE_P_INTEGRATE     Pipe::PIntegrate
#define PIPE_P_ARGS2         Pipe::PArgs2
#define PIPE_P_RESOLVE       Pipe::PResolve
#define PIPE_PAGEFILL        Pipe::PageFill
#define PIPE_FAR_FILL        Pipe::FarFill
#define PIPE_FAR_PATCH_FILL  Pipe::FarPatchFill
#define PIPE_FAR_MAP_FILL    Pipe::FarMapFill
#define PIPE_FAR_DOWN        Pipe::FarDown
#define PIPE_OPENNESS_DIRTY   Pipe::OpennessDirty
#define PIPE_OPENNESS_REFRESH Pipe::OpennessRefresh
#define PIPE_GLOW_SRC         Pipe::GlowSrc
#define PIPE_GLOW_FIELD       Pipe::GlowField
#define PIPE_GLOW_REFRESH     Pipe::GlowRefresh
#define PIPE_CLOUD_NOISE     Pipe::CloudNoise
#define PIPE_CLOUD_WEATHER   Pipe::CloudWeather
#define PIPE_CLOUD_SHADOW    Pipe::CloudShadow
#define PIPE_CLOUD_ENV       Pipe::CloudEnv
#define PIPE_CLOUD_MARCH     Pipe::CloudMarch
#define PIPE_CLOUD_RESOLVE   Pipe::CloudResolve
#define PIPE_SHADOW_PREPARE  Pipe::ShadowPrepare
#define PIPE_SKY_TOP_CLEAR   Pipe::SkyTopClear
#define PIPE_SKY_TOP_REDUCE  Pipe::SkyTopReduce
#define PIPE_RAY_START_TRACE Pipe::RayStartTrace
#define PIPE_RAY_START_MIN   Pipe::RayStartMin
#define PIPE_RAIN_MAP_PREP   Pipe::RainMapPrep
#define PIPE_RAIN_MAP_BUILD  Pipe::RainMapBuild
#define PIPE_WIND_STREAK     Pipe::WindStreak
#define PIPE_SHADOW_RESOLVE  Pipe::ShadowResolve
#define PIPE_FLUID_SPAWN     Pipe::FluidSpawn
#define PIPE_FLUID_MARK      Pipe::FluidMark
#define PIPE_FLUID_ALLOC     Pipe::FluidAlloc
#define PIPE_FLUID_CLEAR     Pipe::FluidClear
#define PIPE_FLUID_P2G       Pipe::FluidP2G
#define PIPE_FLUID_P2G2      Pipe::FluidP2G2
#define PIPE_FLUID_GRIDUP    Pipe::FluidGridUp
#define PIPE_FLUID_G2P       Pipe::FluidG2P
#define PIPE_FLUID_COMPACT_COUNT   Pipe::FluidCompactCount
#define PIPE_FLUID_COMPACT_SCAN    Pipe::FluidCompactScan
#define PIPE_FLUID_COMPACT_SCATTER Pipe::FluidCompactScatter
#define PIPE_FLUID_EXCITE_DETECT   Pipe::FluidExciteDetect
#define PIPE_FLUID_EXCITE_SCAN     Pipe::FluidExciteScan
#define PIPE_FLUID_EXCITE_EMIT     Pipe::FluidExciteEmit
#define PIPE_FLUID_PTICK           Pipe::FluidPTick
#define PIPE_FLUID_SETTLE_JUDGE    Pipe::FluidSettleJudge
#define PIPE_FLUID_SETTLE_SCAN     Pipe::FluidSettleScan
#define PIPE_FLUID_SETTLE_BIN      Pipe::FluidSettleBin
#define PIPE_FLUID_SETTLE_CHECK    Pipe::FluidSettleCheck
#define PIPE_FLUID_SETTLE_COMMIT   Pipe::FluidSettleCommit
#define PIPE_FLUID_SETTLE_KILL     Pipe::FluidSettleKill
#define PIPE_FLUID_CONSUME_APPLY   Pipe::FluidConsumeApply
#define PIPE_FLUID_STAIN_APPLY     Pipe::FluidStainApply
#define PIPE_FLUID_MIRROR_FOLD     Pipe::FluidMirrorFold
#define PIPE_FLUID_CELL_CLEAR      Pipe::FluidCellClear
#define PIPE_WATER_QUIET     Pipe::WaterQuiet
#define PIPE_WATER_LEDGER    Pipe::WaterLedger
#define PIPE_WATER_REDUCE    Pipe::WaterReduce
#define PIPE_WATER_SHAVE     Pipe::WaterShave
#define PIPE_WATER_DRAIN     Pipe::WaterDrain
#define PIPE_WATER_HOLE      Pipe::WaterHole
#define PIPE_WATER_SWEEP     Pipe::WaterSweep
#define PIPE_WATER_SPLIT     Pipe::WaterSplit
#define PIPE_WATER_RELEVEL   Pipe::WaterRelevel
#define PIPE_WATER_SURFACE   Pipe::WaterSurface
#define PIPE_WATER_FLUX      Pipe::WaterFluxPipe
#define PIPE_GAS_ARGS1       Pipe::GasArgs1
#define PIPE_GAS_SPAWN       Pipe::GasSpawnP
#define PIPE_GAS_INTEGRATE   Pipe::GasIntegrate
#define PIPE_GAS_ARGS2       Pipe::GasArgs2
#define PIPE_GAS_RESOLVE     Pipe::GasResolve
#define PIPE_GAS_FARPLUME    Pipe::GasFarPlume
#define PIPE_GAS_FARPLUMEW   Pipe::GasFarPlumeWide
#define PIPE_SOL_WANT        Pipe::SolWant
#define PIPE_SOL_ARGS        Pipe::SolArgsP
#define PIPE_SOL_ALLOC       Pipe::SolAlloc
#define PIPE_SOL_DIFFUSE     Pipe::SolDiffuse
#define PIPE_SOL_COMPACT     Pipe::SolCompact
#define PIPE_SOL_SCOOP       Pipe::SolScoop
#define PIPE_SOL_POUR        Pipe::SolPour
#define PIPE_SOL_HASH        Pipe::SolHash
#define PIPE_SOL_EVICT       Pipe::SolEvict
#define PIPE_SOL_RESTORE     Pipe::SolRestore
#define PIPE_DRAFT_MASK_ALL   Pipe::DraftMaskAll
#define PIPE_DRAFT_MASK_DIRTY Pipe::DraftMaskDirty
#define PIPE_DRAFT_ARGS       Pipe::DraftArgsP
#define PIPE_DRAFT_CBUILD     Pipe::DraftCoarseBuild
#define PIPE_DRAFT_CFACES     Pipe::DraftCoarseFaces
#define PIPE_DRAFT_CSOLVE     Pipe::DraftCoarseSolve
#define PIPE_DRAFT_FINE1      Pipe::DraftFineFirst
#define PIPE_DRAFT_FINE2      Pipe::DraftFineMid
#define PIPE_DRAFT_FINE2B     Pipe::DraftFineMid2
#define PIPE_DRAFT_FINE3      Pipe::DraftFineLast

#define K_COMPUTE  Kind::Compute
#define K_INDIRECT Kind::ComputeIndirect
#define K_COPY     Kind::Copy
#define K_FILL     Kind::Fill

#define GRP_NONE       Groups::None
#define GRP_SIM        Groups::Sim
#define GRP_SLIM_PART  Groups::SlimPart
#define GRP_SLIM_FAR   Groups::SlimFar
#define GRP_SLIM_FLUID Groups::SlimFluid
#define GRP_SLIM_FLUIDSEAM Groups::SlimFluidSeam
#define GRP_SHADOW     Groups::Shadow
#define GRP_SLIM_GAS   Groups::SlimGas

#define DYN_NONE Dyn::None
#define DYN_ZERO Dyn::Zero
#define DYN_CA   Dyn::Ca

#define C_ALWAYS    Cond::Always
#define C_OPS       Cond::Ops
#define C_CELLS     Cond::Cells
#define C_EXP       Cond::Exp
#define C_SPAWN     Cond::Spawn
#define C_PARTICLES Cond::Particles
#define C_HASH      Cond::Hash
#define C_DIRTYTICK Cond::DirtyTick
#define C_GENCOUNT  Cond::GenCount
#define C_DENSEWG   Cond::DenseWorldgen
#define C_FARCOUNT  Cond::FarCount
#define C_FLUIDSPAWN Cond::FluidSpawn
#define C_WINDWAKE  Cond::WindWake
#define C_CAACTIVE  Cond::CaActive
#define C_VIZACTIVE Cond::VizActive
#define C_WATERBODY Cond::WaterBody
#define C_WATERDRAIN Cond::WaterDrain
#define C_WATERSWEEP Cond::WaterSweep
#define C_WATERWAVE  Cond::WaterWave
#define C_OPENNESS  Cond::Openness
#define C_GLOW      Cond::Glow
#define C_GAS       Cond::Gas
#define C_GASFAR    Cond::GasFarEmit
#define C_GASOUT    Cond::GasOuter
#define C_GASWIDE   Cond::GasFarWide
#define C_REPOSE    Cond::ReposeActive
#define C_CLOUDS    Cond::Clouds
#define C_CLOUDBAKE Cond::CloudBake
#define C_SHADOWON  Cond::ShadowCacheOn
#define C_RAYSTART  Cond::RayStart
#define C_RAINMAP   Cond::RainMap
#define C_RAINEXPO  Cond::RainExpo
#define C_STREAKS   Cond::WindStreaks
#define C_DRAFTALL   Cond::DraftAll
#define C_DRAFTDIRTY Cond::DraftDirty
#define C_DRAFT      Cond::Draft

#define D_WINDWAKE  (uint32_t)DispatchSel::WindWakeSel
#define D_RAINFALL  (uint32_t)DispatchSel::RainFallSel
#define D_RAINEXPO  (uint32_t)DispatchSel::RainExpoSel
#define D_OPS       (uint32_t)DispatchSel::Ops
#define D_CELLS     (uint32_t)DispatchSel::Cells
#define D_EXP       (uint32_t)DispatchSel::Exp
#define D_EXPWG     (uint32_t)DispatchSel::ExpWg
#define D_SPAWN     (uint32_t)DispatchSel::Spawn
#define D_CHUNKS    (uint32_t)DispatchSel::Chunks
#define D_CHUNKS64  (uint32_t)DispatchSel::Chunks64
#define D_GENCOUNT  (uint32_t)DispatchSel::GenCount
#define D_GENCOLS   (uint32_t)DispatchSel::GenColCount
#define D_FARCOUNT  (uint32_t)DispatchSel::FarCount
#define IND_DISPATCHARGS  (uint32_t)DispatchSel::IndDispatchArgs
#define IND_PDISPATCHARGS (uint32_t)DispatchSel::IndPDispatchArgs
#define D_FLUIDSPAWN      (uint32_t)DispatchSel::FluidSpawnSel
#define IND_FLUIDARGS     (uint32_t)DispatchSel::IndFluidArgs
#define IND_FLUIDPARGS    (uint32_t)DispatchSel::IndFluidPArgs
#define D_OPENCHUNKS      (uint32_t)DispatchSel::OpennessChunks
#define D_GLOWCHUNKS      (uint32_t)DispatchSel::GlowChunks
#define D_WATERCHUNKS     (uint32_t)DispatchSel::WaterChunks
#define D_WATERCHUNKS64   (uint32_t)DispatchSel::WaterChunks64
#define D_WATERDRAIN      (uint32_t)DispatchSel::WaterDrainSel
#define IND_SHADOWARGS    (uint32_t)DispatchSel::IndShadowArgs
#define D_GASSPAWN        (uint32_t)DispatchSel::GasSpawnSel
#define IND_GASARGS       (uint32_t)DispatchSel::IndGasDispatchArgs
#define D_GASFAREMIT      (uint32_t)DispatchSel::GasFarEmitSel
#define D_GASFARWIDE      (uint32_t)DispatchSel::GasFarWideSel
#define D_CLOUDGX         (uint32_t)DispatchSel::CloudGx
#define D_CLOUDGY         (uint32_t)DispatchSel::CloudGy
#define D_RSGX            (uint32_t)DispatchSel::RayStartGx
#define D_RSGY            (uint32_t)DispatchSel::RayStartGy
#define D_STREAKGX        (uint32_t)DispatchSel::StreakGx
#define IND_SOLARGS       (uint32_t)DispatchSel::IndSolArgs
#define IND_DRAFTARGS     (uint32_t)DispatchSel::IndDraftArgs

// ---- expansion 1: the rows -----------------------------------------------
#define R(b)  Use{Buf::b, Acc::StorageRead},
#define W(b)  Use{Buf::b, Acc::StorageWrite},
#define RW(b) Use{Buf::b, Acc::StorageRW},
#define A(b)  Use{Buf::b, Acc::StorageAtomicRMW},
#define U(b)  Use{Buf::b, Acc::Uniform},
#define I(b)  Use{Buf::b, Acc::IndirectRead},
#define TR(b) Use{Buf::b, Acc::TransferRead},
#define TW(b) Use{Buf::b, Acc::TransferWrite},
#define USES(...) {__VA_ARGS__}

#define PASS(nm, grpLabel, tbl, pipe, knd, dx, dy, dz, grp, dyn, cnd, rep, uses) \
  Row{#nm, grpLabel, tbl, pipe, knd, (uint32_t)(dx), (uint32_t)(dy),             \
      (uint32_t)(dz), grp, dyn, cnd, (uint32_t)(rep), uses, 0},

constexpr Row kRowsInit[] = {
#include "sim/pass_table.def"
};

#undef PASS
#undef USES
#undef R
#undef W
#undef RW
#undef A
#undef U
#undef I
#undef TR
#undef TW

// ---- expansion 2: the per-row use counts ---------------------------------
#define R(b)  1 +
#define W(b)  1 +
#define RW(b) 1 +
#define A(b)  1 +
#define U(b)  1 +
#define I(b)  1 +
#define TR(b) 1 +
#define TW(b) 1 +
#define USES(...) (__VA_ARGS__ 0)

#define PASS(nm, grpLabel, tbl, pipe, knd, dx, dy, dz, grp, dyn, cnd, rep, uses) uses,

constexpr int kUseCounts[] = {
#include "sim/pass_table.def"
};

#undef PASS
#undef USES
#undef R
#undef W
#undef RW
#undef A
#undef U
#undef I
#undef TR
#undef TW

constexpr int kN = (int)(sizeof(kRowsInit) / sizeof(kRowsInit[0]));
static_assert(kN == (int)(sizeof(kUseCounts) / sizeof(kUseCounts[0])),
              "the two expansions of pass_table.def disagree on row count");

// Growing a row past kMaxUses would silently truncate a hazard, which is
// exactly the failure this table exists to prevent.
constexpr bool AllUsesFit() {
  for (int i = 0; i < kN; i++)
    if (kUseCounts[i] > kMaxUses) return false;
  return true;
}
static_assert(AllUsesFit(),
              "a pass_table.def row declares more uses than kMaxUses — raise "
              "kMaxUses in pass_table.h rather than dropping a use");

struct Storage {
  Row rows[kN];
};

constexpr Storage Build() {
  Storage s{};
  for (int i = 0; i < kN; i++) {
    s.rows[i] = kRowsInit[i];
    s.rows[i].useCount = kUseCounts[i];
  }
  return s;
}

constexpr Storage kStorage = Build();

}  // namespace

const Row* const kRows = kStorage.rows;
const int kRowCount = kN;

}  // namespace pass
