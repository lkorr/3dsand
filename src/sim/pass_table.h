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
  // ---- MLS-MPM fluid prototype (docs/PLAN_mpm_fluids.md; sim_fluid.wgsl) ----
  // None of these is read by any CA kernel or covered by the world hash;
  // FluidDispatchArgs is indirect-only and never bound (dispatchArgs note).
  FluidParticles,
  FluidSpawnOps,
  FluidBlockMap,
  FluidBlockList,
  FluidGrid,
  FluidArgsStage,
  FluidDispatchArgs,
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
  // MLS-MPM fluid prototype. Inserted BEFORE FarDown deliberately: the
  // pipeline-copy loop in Simulation::RecordTable is bounded by
  // `(int)Pipe::FarDown + 1`, so FarDown must stay the last enumerator or a
  // new pipeline is silently never handed to the recorder (a skipped row, not
  // a crash).
  FluidSpawn, FluidMark, FluidAlloc, FluidClear, FluidP2G, FluidP2G2,
  FluidGridUp, FluidG2P,
  FarFill, FarDown,
};

// Bind-group set. GRP_SIM also carries the dynamic passUBO offset.
enum class Groups : uint8_t { None, Sim, SlimPart, SlimFar, SlimFluid };

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
  FluidSpawn, // fluidSpawnCount > 0 (MLS-MPM spawn row in PT_TICK)
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
                             Fluid };

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
  // ---- MLS-MPM fluid prototype ----
  FluidP,            // (fluidCount + 63)/64 — fluidCount = base + spawns
  FluidSpawnSel,     // (fluidSpawnCount + 63)/64
  IndFluidArgs,      // indirect: world.fluidDispatchArgs @ 0
};

// Max `uses` entries on any row. Asserted against the widest row at compile
// time in pass_table.cpp, so growing a row past this fails the build rather
// than silently truncating a hazard.
// Raised 10 -> 12 by the page table (PLAN_page_table.md §5.3): `ca` goes
// 9 -> 10 with R(PageTable) -> 11 with A(PageFaults), which exceeded the old
// ceiling and stopped the build, as the static_assert is meant to. Raised to
// 12 rather than 11 so the next row addition does not repeat it.
inline constexpr int kMaxUses = 12;

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
