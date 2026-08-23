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

namespace pass {
namespace {

// ---- the .def's vocabulary, as macros ------------------------------------
#define PT_TICK       Table::Tick
#define PT_WORLDGEN   Table::Worldgen
#define PT_GENLIST    Table::GenList
#define PT_LOADRESET  Table::LoadReset
#define PT_HASHONLY   Table::HashOnly
#define PT_FARFILL    Table::FarFill
#define PT_FLUID      Table::Fluid

#define PIPE_NONE            Pipe::None
#define PIPE_WORLDGEN        Pipe::Worldgen
#define PIPE_WORLDGEN_LIST   Pipe::WorldgenList
#define PIPE_MUTATE          Pipe::Mutate
#define PIPE_MUTATE_CELLS    Pipe::MutateCells
#define PIPE_COMPACT         Pipe::Compact
#define PIPE_COMPACT_NEXT    Pipe::CompactNext
#define PIPE_STEP            Pipe::Step
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
#define PIPE_FAR_FILL        Pipe::FarFill
#define PIPE_FAR_DOWN        Pipe::FarDown
#define PIPE_FLUID_SPAWN     Pipe::FluidSpawn
#define PIPE_FLUID_MARK      Pipe::FluidMark
#define PIPE_FLUID_ALLOC     Pipe::FluidAlloc
#define PIPE_FLUID_CLEAR     Pipe::FluidClear
#define PIPE_FLUID_P2G       Pipe::FluidP2G
#define PIPE_FLUID_GRIDUP    Pipe::FluidGridUp
#define PIPE_FLUID_G2P       Pipe::FluidG2P

#define K_COMPUTE  Kind::Compute
#define K_INDIRECT Kind::ComputeIndirect
#define K_COPY     Kind::Copy
#define K_FILL     Kind::Fill

#define GRP_NONE       Groups::None
#define GRP_SIM        Groups::Sim
#define GRP_SLIM_PART  Groups::SlimPart
#define GRP_SLIM_FAR   Groups::SlimFar
#define GRP_SLIM_FLUID Groups::SlimFluid

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
#define C_CAACTIVE  Cond::CaActive

#define D_OPS       (uint32_t)DispatchSel::Ops
#define D_CELLS     (uint32_t)DispatchSel::Cells
#define D_EXP       (uint32_t)DispatchSel::Exp
#define D_EXPWG     (uint32_t)DispatchSel::ExpWg
#define D_SPAWN     (uint32_t)DispatchSel::Spawn
#define D_CHUNKS    (uint32_t)DispatchSel::Chunks
#define D_CHUNKS64  (uint32_t)DispatchSel::Chunks64
#define D_GENCOUNT  (uint32_t)DispatchSel::GenCount
#define D_FARCOUNT  (uint32_t)DispatchSel::FarCount
#define IND_DISPATCHARGS  (uint32_t)DispatchSel::IndDispatchArgs
#define IND_PDISPATCHARGS (uint32_t)DispatchSel::IndPDispatchArgs
#define D_FLUIDP          (uint32_t)DispatchSel::FluidP
#define D_FLUIDSPAWN      (uint32_t)DispatchSel::FluidSpawnSel
#define IND_FLUIDARGS     (uint32_t)DispatchSel::IndFluidArgs

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
