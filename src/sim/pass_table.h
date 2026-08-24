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
  FarVox,
  FarOcc,
  FarList,
  FarUBO,
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
  Mutate, MutateCells,
  Compact, CompactNext,
  Step,
  Occupancy, OccupancyDirty,
  Pick,
  ExplodeMark, ExplodeApply,
  PArgs1, PSpawn, PIntegrate, PArgs2, PResolve,
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
  FluidConsumeApply, FluidStainApply, FluidMirrorFold,
  // Materializes a JITTER page (world.h's JITTER block): the one fill a
  // vkCmdFillBuffer cannot do, because the words vary per cell.
  PageFill,
  FarFill, FarDown,
};

// Bind-group set. GRP_SIM also carries the dynamic passUBO offset.
enum class Groups : uint8_t { None, Sim, SlimPart, SlimFar, SlimFluid,
                              SlimFluidSeam };

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
// dynamic offset on this device: minUniformBufferOffsetAlignment is 64
// (--vk-info), and the requirement is that the offset be a multiple of it.
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
                             Fluid, FluidSeam, FluidSettle, PageFill };

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
  FarCount,   // EncodeFarFill count
  IndDispatchArgs,   // indirect: world.dispatchArgs @ 0
  IndPDispatchArgs,  // indirect: world.pDispatchArgs @ 0
  // ---- MLS-MPM fluid ----
  FluidSpawnSel,     // (fluidSpawnCount + 63)/64
  IndFluidArgs,      // indirect: world.fluidDispatchArgs @ 0
  IndFluidPArgs,     // indirect: world.fluidPDispatchArgs @ 0 — the seam's
                     // per-particle passes and its list-shaped dispatches
                     // (the seam re-copies the buffer between uses)
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
inline constexpr int kMaxUses = 16;

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
  // Fill:     unused (whole buffer).
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

}  // namespace pass
