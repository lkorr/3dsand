# Vulkan barrier graph and submit-boundary sync

Phase 1 of `docs/PLAN_vulkan_port.md`. Input: `docs/vulkan_pass_map.md` (the
measured pass/resource map). Output: the design the Vulkan backend is
implemented from — the declarative pass table, the algorithm that turns it into
`vkCmdPipelineBarrier2` calls, the submit-boundary mechanisms that replace
WebGPU's implicit queue semantics, and the checks that keep the table honest.

This document is load-bearing in the sense CLAUDE.md rule 1 means: Dawn
currently generates every hazard barrier in the tick automatically, and the tick
contains ~60 serial read-after-write hops on one 512 MiB buffer. A barrier that
is too weak is not a crash — it is a race whose outcome depends on how a
particular GPU schedules workgroups, i.e. a world hash that matches on the
RTX 3060 Ti and diverges on the next vendor. Every claim below is stated so it
can be argued with; anything marked **JUDGMENT** is a call made here that a
reviewer should re-derive rather than accept.

Verified against the live tree 2026-08-22 (`simulation.cpp:622-776`,
`world.cpp:105-235`, `support.cpp:104-173`, `stream.cpp:140-279`,
`main.cpp:132-152`, and the WGSL binding declarations). Edges this document adds
beyond the pass map are marked **[NEW EDGE]** and are listed together in §7.10.

**Completeness is the whole of the guarantee.** Until every command the engine
records appears in the table, "barriers are generated from the table" means
"barriers are missing wherever the table is". A path that records GPU work
outside the table gets no barriers at all — not weak ones, none — and that
failure is silent in exactly the way this document exists to prevent. The
enumeration below is therefore normative, not illustrative: adding a code path
that submits a command buffer means adding its table here in the same commit.

Paths covered by this document:

| Path | Table | Recorded by |
|---|---|---|
| the sim tick | §2.4 | `Simulation::EncodeTick` + `EncodeFarFill` + `World::EncodeReadbacks` + `World::EncodeDirtyCopy` |
| `--shot` far-fill drain loop | §2.5.6 `farFillOnly` | `RunShots`, `main.cpp:139-147` |
| worldgen | §2.5.1 | `Simulation::EncodeWorldgen` |
| streamed-chunk generation | §2.5.2 | `Simulation::EncodeGenList`, submitted by `Stream::FillSlots` |
| load reset | §2.5.3 | `Simulation::EncodeLoadReset` |
| hash-only rehash | §2.5.4 | `Simulation::EncodeHashOnly` |
| day/night wake-all | §2.5.5 | `Simulation::EncodeWakeAll` (upload only) |
| streaming slot uploads | §4.1 pending-upload queue | `Stream::FillSlots`, `stream.cpp:245-260` — **may submit nothing at all** |
| eviction copies | §4.3 | `Stream::EvictSlots`, own encoder + submit |
| readbacks | §2.4 phase 7a/7b | `World::EncodeReadbacks` / `EncodeDirtyCopy` — two separate functions |
| the frame | §2.6 | `main.cpp:2822-2834` |
| blocking selftest/screenshot reads | §4.2 | `ReadHashSync`, voxel dumps, `grab()` in `RunShots` |

---

## 1. Synchronization model overview

### 1.1 The tick is a DAG, recorded as a total order

One tick is a directed acyclic graph of passes over buffers. Every edge is a
hazard: RAW (a pass reads what an earlier pass wrote), WAW (two passes write the
same range), WAR (a pass writes what an earlier pass read). Dawn discovers these
edges from bind-group usage and inserts barriers; Vulkan requires us to state
them.

We do not implement a general DAG scheduler. The tick is recorded as a **total
order** — exactly the order `EncodeTick` uses today — and hazards are resolved
against the *most recent prior writer/reader in that order*. This is strictly
more conservative than the true DAG (it will occasionally insert a barrier
between two passes that are actually independent) and it is the only shape whose
correctness is checkable by eye. Reordering for overlap is a phase-8 concern
gated by hash equality, not a v1 concern.

**Design thesis: barriers are GENERATED from the pass table, never hand-written
at a call site.** `EncodeTick` becomes a loop over table rows. The recorder owns
a per-buffer "last access" state and emits barriers by comparing each row's
declared reads/writes against that state. There is no place in the backend where
an implementer types `VK_ACCESS_2_SHADER_STORAGE_READ_BIT` next to a dispatch —
if the barrier is wrong, the table row is wrong, and the checker in §5 is what
catches a table row that disagrees with the shader.

The corollary is a standing obligation, already stated in
`PLAN_vulkan_port.md`: a change to a sim kernel's bindings changes the table in
the same commit, or the checker fails.

### 1.2 What maps to what

| Ordering need | Vulkan primitive |
|---|---|
| Pass → pass inside one tick | `vkCmdPipelineBarrier2` with `VkBufferMemoryBarrier2` per hazarded buffer, batched into one call per boundary |
| Dispatch → dispatch inside what was one `ComputePassEncoder` | identical — Vulkan has no "pass" concept for compute, so the pass-map's "inside a pass" and "between passes" cases collapse into one mechanism |
| Copy → dispatch / dispatch → copy | same barrier call, with `TRANSFER` stage on one side |
| Copy → indirect dispatch/draw | same barrier call, `TRANSFER_WRITE` → `INDIRECT_COMMAND_READ` at `DRAW_INDIRECT` stage |
| Host write → device read | staging upload recorded at submit head (§3.1); no `HOST_WRITE` barrier needed in the common path |
| Submit → submit on one queue | implicit: submission order on a single queue with no semaphores still guarantees *submission* order for the implicit ordering guarantee, but **not** memory visibility — see §3.4 |
| Device work → host read | `VkFence` per readback ring slot, plus a `HOST_READ` barrier before end-of-command-buffer (§3.2) |
| Full drain (`WaitIdle`) | `vkQueueWaitIdle` / `vkDeviceWaitIdle` (§3.9) |
| Render pass attachments | `VkRenderingInfo` (dynamic rendering) with `loadOp = CLEAR`; image layout transitions are the swapchain's, unrelated to the buffer graph |

### 1.3 What is deliberately NOT used in v1

- **Events (`vkCmdSetEvent2`/`WaitEvents2`).** Split barriers buy overlap; they
  do not buy correctness, and they make the generated code harder to audit.
- **Semaphores between tick and frame.** v1 is one queue; §4.
- **Timeline semaphores for the readback ring.** A binary `VkFence` per slot is
  exactly the semantics `Slot::inFlight` has today. Timeline semaphores are a
  fine phase-8 refinement; they are not required to reproduce current behavior.
- **Subpass/attachment dependencies for compute.** N/A.
- **Any relaxation from "unknown pass shape ⇒ conservative".** When the table
  cannot express something (ImGui's internal recording), the boundary around it
  is a full barrier.

---

## 2. The pass table

### 2.1 Row schema

```c++
enum class Acc : uint32_t {          // how a pass touches a buffer
  StorageRead,      // var<storage, read>            — SHADER_STORAGE_READ
  StorageRW,        // var<storage, read_write>      — READ | WRITE
  StorageWrite,     // written but never read by this pass (declared rw in WGSL)
  StorageAtomicRMW, // atomics only; still READ|WRITE for barrier purposes
  Uniform,          // var<uniform>                  — UNIFORM_READ
  IndirectRead,     // consumed by vkCmdDispatchIndirect / vkCmdDrawIndirect
  TransferRead,     // copy source / fill source
  TransferWrite,    // copy dest / vkCmdFillBuffer dest
  HostRead,         // mapped and read by the CPU after a fence
};

struct Use { BufId buf; Acc acc; };

enum class Kind { Compute, ComputeIndirect, Copy, Fill, DrawIndexedIndirect, Draw };

struct PassRow {
  const char*   name;        // stable; also the PassTimer label
  PipelineId    pipe;        // kNone for Copy/Fill
  Kind          kind;
  Dispatch      dispatch;    // direct extents, or {indirectBuf, offset}
  BindGroupSet  groups;      // which layout: sim / simSlim+particle / simSlim+far
  DynOffsetFn   dynOffset;   // passUBO slice selector, or null
  Use           uses[N];     // reads AND writes, one entry per buffer
  CondId        cond;        // predicate evaluated per tick
  uint32_t      repeat;      // >1 = the CA loop; dynOffset takes the iteration
};
```

Two rules about `uses` that the checker (§5) enforces:

1. **`uses` is the union of the shader's declared bindings that the entry point
   actually touches**, not the bind-group layout. `simBGL_` binds 17 buffers;
   `sim_step.wgsl` declares 8 of them. Declaring the whole layout would insert
   barriers on `occupancy`, `pick`, `hash` and `cellOps` in the CA loop that are
   pure cost with no hazard behind them.
2. **A binding declared `read_write` in WGSL but only read by the entry point is
   `StorageRead` in the table**, and the checker requires a comment on the row
   saying so. Two live cases: `dirtyList` in `sim_step.wgsl` (declared
   `read_write` at binding 12, used only as `dirtyList[wg.x]` at line 755) and
   `dirtyList` in `sim_occupancy.wgsl:mainDirty`. Getting this wrong is
   conservative-safe (it upgrades a RAW to a WAW-ish barrier), so the checker
   *warns* on read-only-in-practice rather than failing — but the CA loop is 53
   repetitions of this barrier, so it is worth being right.

### 2.2 Buffer identities

The recorder tracks state per **buffer**, not per range. `dirty[0]` and
`dirty[1]` are distinct ids. `page_` selects which id `dirtyIn`/`dirtyOut`
resolve to; the table names them symbolically (`DirtyIn`, `DirtyOut`) and the
recorder resolves at record time from the current page. Same for
`Particles[page]` / `Particles[1-page]`.

Sub-range tracking (`pArgsStage[0..16)` vs `[16..28)`, `particleCounts[page*4]`)
is **not** modelled in v1: the whole buffer is one tracked unit. This costs two
barriers per tick that a range-aware tracker would elide, on 32 B and 16 B
buffers. **JUDGMENT:** whole-buffer granularity, because sub-range tracking is
where a hand-written hazard graph gets subtly wrong, and the cost here is
provably nil. Revisit only with a measurement showing otherwise.

### 2.3 Conditions

```
cAlways        true
cOps           opsCount > 0
cCells         cellCount > 0
cExp           expCount > 0
cSpawn         spawnCount > 0
cParticles     particlesActive
cHash          hashEnable            (tick % 15 == 0)
cDirtyTick     !hashEnable
cFar           farCount > 0
cReadback      wantReadback && a free readback slot exists
```

All ten are known on the CPU before recording begins. A row whose condition is
false is **skipped entirely** — it contributes no barrier and does not update
any buffer's last-access state. §7.5 covers why that is the only correct
handling and what it implies.

### 2.4 The tick table

Order is the record order. `→` in the Writes column means the buffer is also
read by the same pass (`StorageRW`).

**Phase 0 — pre-pass fills** (`simulation.cpp:625-629`)

| # | Name | Kind | Reads | Writes | Cond |
|---|---|---|---|---|---|
| T00 | `fill.dirtyOut` | Fill | — | `DirtyOut`:TransferWrite | cAlways |
| T01 | `fill.argsStage` | Fill | — | `argsStage`:TransferWrite | cAlways |
| T02 | `fill.claim` | Fill | — | `claim`:TransferWrite | cParticles |
| T03 | `fill.expMask` | Fill | — | `expMask`:TransferWrite | cExp |
| T04 | `fill.hash` | Fill | — | `hash`:TransferWrite | cHash |

**Phase 1 — prep** (`simulation.cpp:634-668`; was one `ComputePassEncoder`)

| # | Name | Pipeline | Kind | Reads | Writes | Cond |
|---|---|---|---|---|---|---|
| T10 | `mutate` | `mutate_` (`sim_mutate:main`) | Compute `(4*ops,4,4)` wg`(4,4,4)` | `materials`:SR, `tickUBO`:U, `opsBuf`:SR | `voxels`:RW, `DirtyIn`:AtomicRMW, `DirtyOut`:AtomicRMW | cOps |
| T11 | `mutateCells` | `mutateCells_` (`sim_mutate:cells`) | Compute `((cells+63)/64,1,1)` wg`(64)` | `tickUBO`:U, `cellOps`:SR | `voxels`:RW, `DirtyIn`:AtomicRMW, `DirtyOut`:AtomicRMW | cCells |
| T12 | `explodeMark` | `explodeMark_` (`sim_explode:mark`) | Compute `(11*exp,11,11)` wg`(4,4,4)` | `voxels`:SR, `materials`:SR, `tickUBO`:U, `expOps`:SR | `expMask`:SW, `DirtyIn`:AtomicRMW, `DirtyOut`:AtomicRMW | cExp |
| T13 | `explodeApply` | `explodeApply_` (`sim_explode:apply`) | Compute `(11*exp,11,11)` wg`(4,4,4)` | `materials`:SR, `tickUBO`:U, `expOps`:SR, `expMask`:SR | `voxels`:RW, `DirtyIn`:AtomicRMW, `DirtyOut`:AtomicRMW, `particleCounts`:AtomicRMW, `Particles[page]`:SW | cExp |
| T14 | `particleSpawn` | `pSpawn_` (`sim_particle:spawn`) | Compute `((spawn+63)/64,1,1)` wg`(64)` | `spawnOps`:SR, `tickUBO`:U | `particleCounts`:AtomicRMW, `Particles[page]`:SW | cSpawn |
| T15 | `compact` | `compact_` (`sim_compact:main`) | Compute `(64,1,1)` wg`(64)` | `DirtyIn`:SR | `dirtyList`:SW, `argsStage`:AtomicRMW | cAlways |

Note T12 reads `voxels` and T13 writes it: an intra-"pass" RAW that Dawn
inserted for free. This is the mark/apply split CLAUDE.md documents as the fix
for a kernel that both read and wrote a neighborhood — it only works if the
barrier between them exists.

**T15 writes all three dispatch-args words, not just the count.**
`sim_compact.wgsl:22-25` has thread 0 `atomicStore(&args[1], 1u)` and
`atomicStore(&args[2], 1u)` before the `atomicAdd(&args[0], 1u)` append. The `y`
and `z` extents of `dispatchArgs` therefore come **from the shader**, not from
the T01 fill — the fill only zeroes the count. Consequence for the barrier: the
T01→T15 (and T55→T56, `sim_compact.wgsl:40-43`, identical) TransferWrite→
StorageAtomicRMW WAW barrier is load-bearing for **dispatch validity**, not
merely for count accuracy. If the fill lands after the shader's stores, the args
read `{count, 0, 0}` — a dispatch of zero workgroups, i.e. a world that silently
stops simulating rather than one that simulates slightly wrong. §7.3.

**Phase 2 — indirect staging** (`simulation.cpp:672`)

| # | Name | Kind | Reads | Writes | Cond |
|---|---|---|---|---|---|
| T20 | `copy.args→dispatchArgs` | Copy 12 B | `argsStage`:TransferRead | `dispatchArgs`:TransferWrite | cAlways |

**Phase 3 — the CA loop** (`simulation.cpp:674-687`)

| # | Name | Pipeline | Kind | Reads | Writes | Cond | Repeat |
|---|---|---|---|---|---|---|---|
| T30 | `ca[k]` | `step_` (`sim_step:main`) | ComputeIndirect `dispatchArgs@0` wg`(6,6,6)` | `materials`:SR, `tickUBO`:U, `passUBO`:U (dyn `k*256`), `reactions`:SR, `dirtyList`:SR¹, `dispatchArgs`:IndirectRead | `voxels`:RW, `DirtyOut`:AtomicRMW, `support`:AtomicRMW | cAlways | **54** |

¹ declared `read_write` at binding 12, read-only in the entry point (line 755).

**Phase 4 — particles** (`simulation.cpp:692-722`, all `cParticles`)

| # | Name | Pipeline | Kind | Reads | Writes |
|---|---|---|---|---|---|
| T40 | `pArgs1` | `pArgs1_` (`sim_particle:args1`) | Compute `(1,1,1)` wg`(1)` | `particleCounts`:AtomicRMW(read), `tickUBO`:U | `pArgsStage`:SW |
| T41 | `copy.pArgs→pDispatch` | Copy `pArgsStage@16 → pDispatchArgs@0`, 12 B | `pArgsStage`:TransferRead | `pDispatchArgs`:TransferWrite |
| T42 | `pIntegrate` | `pIntegrate_` (`sim_particle:integrate`) | ComputeIndirect `pDispatchArgs@0` wg`(64)` | `voxels`:SR, `materials`:SR, `tickUBO`:U, `Particles[page]`:RW, `pDispatchArgs`:IndirectRead | `Particles[1-page]`:SW, `particleCounts`:AtomicRMW, `claim`:AtomicRMW |

**T11 and T42 were both corrected by the checker when phase 2b built it**, and
the corrections are recorded here rather than only in `pass_table.def` because
this table is what a reviewer reads. `sim_mutate:cells` does **not** read
`materials` — it writes the authored word straight into the grid, unlike
`sim_mutate:main` which does consult the table; the `materials`:SR above was
wrong. `sim_particle:integrate` does **not** touch `dirtyOut` — `markDirtyNext`
is the module's only writer of it and is called only from `resolve`
(`sim_particle.wgsl:242, :265`); the earlier draft listed `dirtyOut`:AtomicRMW
on T42 and it has been dropped. Both were over-declarations, i.e. spurious
barriers rather than missing ones, so neither was a correctness bug — but both
are exactly the drift §6.1's rooted walk exists to find, and finding them on the
day the checker was written is the argument for having it.
| T43 | `pArgs2` | `pArgs2_` (`sim_particle:args2`) | Compute `(1,1,1)` wg`(1)` | `particleCounts`:AtomicRMW(read), `tickUBO`:U | `pArgsStage`:SW |
| T44 | `copy.pArgs→pDispatch` | Copy `pArgsStage@16 → pDispatchArgs@0`, 12 B | `pArgsStage`:TransferRead | `pDispatchArgs`:TransferWrite |
| T45 | `copy.pArgs→drawArgs` | Copy `pArgsStage@0 → drawArgs@0`, 16 B | `pArgsStage`:TransferRead | `drawArgs`:TransferWrite |
| T46 | `pResolve` | `pResolve_` (`sim_particle:resolve`) | ComputeIndirect `pDispatchArgs@0` wg`(64)` | `materials`:SR, `tickUBO`:U, `claim`:AtomicRMW(read), `particleCounts`:AtomicRMW(read), `pDispatchArgs`:IndirectRead | `voxels`:RW, `Particles[1-page]`:RW, `DirtyOut`:AtomicRMW |

T42→T43 is a RAW on `particleCounts` that Dawn inserted inside a single pass.
T43→T44 is a RAW on `pArgsStage` crossing a compute→transfer boundary. Both are
easy to lose sight of because they were invisible.

**Phase 5a — hash tick** (`simulation.cpp:724-732`, `cHash`)

| # | Name | Pipeline | Kind | Reads | Writes |
|---|---|---|---|---|---|
| T50 | `occupancyFull` | `occupancy_` (`sim_occupancy:main`) | Compute `(4096,1,1)` wg`(64)` | `voxels`:SR, `materials`:SR, `tickUBO`:U | `occupancy`:SW, `hash`:AtomicRMW |
| T51 | `pick` | `pick_` (`sim_pick:main`) | Compute `(1,1,1)` wg`(1)` | `voxels`:SR, `materials`:SR, `renderUBO`:U | `pick`:SW |

**Phase 5b — dirty tick** (`simulation.cpp:733-772`, `cDirtyTick`)

| # | Name | Pipeline | Kind | Reads | Writes |
|---|---|---|---|---|---|
| T55 | `fill.argsStage` | Fill | — | `argsStage`:TransferWrite |
| T56 | `compactNext` | `compactNext_` (`sim_compact:mainNext`) | Compute `(64,1,1)` wg`(64)` | `DirtyOut`:SR | `dirtyList`:SW, `argsStage`:AtomicRMW |
| T57 | `copy.args→dispatchArgs` | Copy 12 B | `argsStage`:TransferRead | `dispatchArgs`:TransferWrite |
| T58 | `occupancyDirty` | `occupancyDirty_` (`sim_occupancy:mainDirty`) | ComputeIndirect `dispatchArgs@0` wg`(64)` | `voxels`:SR, `materials`:SR, `dirtyList`:SR¹, `dispatchArgs`:IndirectRead | `occupancy`:SW |
| T59 | `pick` | `pick_` | Compute `(1,1,1)` wg`(1)` | `voxels`:SR, `materials`:SR, `renderUBO`:U | `pick`:SW |
| T5A | `farDown` | `farDown_` (`worldgen:fardown`) | ComputeIndirect `dispatchArgs@0` wg`(64)` | `voxels`:SR, `materials`:SR, `tickUBO`:U, `farUBO`:U, `dirtyList`:SR (as `farDirty`, group1 b4), `dispatchArgs`:IndirectRead | `farVox`:AtomicRMW, `farOcc`:AtomicRMW |

**Phase 6 — far fill** (`EncodeFarFill`, `simulation.cpp:578-586`)

| # | Name | Pipeline | Kind | Reads | Writes | Cond |
|---|---|---|---|---|---|---|
| T60 | `farFill` | `farFill_` (`worldgen:far`) | Compute `(farCount,1,1)` wg`(64)` | `materials`:SR, `tickUBO`:U, `farList`:SR, `farUBO`:U | `farVox`:AtomicRMW, `farOcc`:AtomicRMW | cFar |

**Phase 7a — readbacks** (`World::EncodeReadbacks`, `world.cpp:105-163`;
`cReadback`)

| # | Name | Kind | Reads | Writes |
|---|---|---|---|---|
| T70 | `copy.fetch[i]` ×≤64 | Copy 16 KiB each | `voxels`:TransferRead | `slot.buf`:TransferWrite |
| T71 | `copy.mirror[i]` ×27 | Copy 16 KiB each | `voxels`:TransferRead | `slot.buf`:TransferWrite |
| T72 | `copy.occupancy` | Copy 16 KiB | `occupancy`:TransferRead | `slot.buf`:TransferWrite |
| T73 | `copy.hash` | Copy 16 B | `hash`:TransferRead | `slot.buf`:TransferWrite |
| T74 | `copy.pick` | Copy 32 B | `pick`:TransferRead | `slot.buf`:TransferWrite |
| T75 | `copy.particleCounts` | Copy 16 B | `particleCounts`:TransferRead | `slot.buf`:TransferWrite |
| T76 | `copy.support` | Copy 16 KiB | `support`:TransferRead | `slot.buf`:TransferWrite |
| T77 | `fill.support` | Fill | — | `support`:TransferWrite |

**Phase 7b — the dirty copy** (`World::EncodeDirtyCopy`, `world.cpp:165-168`)

This is a **separate function**, called from `support.cpp:167` *after*
`EncodeReadbacks` has already returned — including after T77's
`ClearBuffer(support)` at `world.cpp:160`. It is guarded by
`if (lastSlot_ < 0) return;` and by `SubmitTick`'s `if (doCopy)`, so its
condition is "7a ran and claimed a slot", not `cReadback` independently.

| # | Name | Kind | Reads | Writes | Cond |
|---|---|---|---|---|---|
| T78 | `copy.dirtyNext` | Copy 16 KiB | `DirtyOut`:TransferRead | `slot.buf`:TransferWrite @`kDirtyOff` | 7a claimed a slot |

T70–T78 are all transfer→transfer against each other and need **no barriers
between themselves** (they write disjoint ranges of `slot.buf` and read
different sources) — with the single exception of T76→T77, which is a genuine
transfer-read→transfer-write WAR on `support` and is the case most likely to be
dropped as "just two copies" (§7.4).

**The host-read barrier is not a table row.** An earlier draft of this document
placed a `T79 barrier.hostRead` row at the end of phase 7, which was wrong: T78
writes `slot.buf` and is recorded *after* phase 7a, so a barrier at a fixed
index inside 7a would make the host read `dirtyFlags` (consumed at
`world.cpp:186-192`) with no visibility guarantee at all. The correct rule:

> **The recorder emits the host-visibility barrier at `Finish()` time — as the
> last command in the command buffer, after every writer of every
> host-visible buffer touched during the recording — never at a fixed table
> index.**

Mechanically: the recorder keeps the set of host-visible buffers written during
this recording (readback slots, staging ring regions being read back) and, in
`Finish()`, emits one `vkCmdPipelineBarrier2` with a `VkBufferMemoryBarrier2`
per such buffer, `srcStage=COPY srcAccess=TRANSFER_WRITE →
dstStage=HOST dstAccess=HOST_READ`. This is index-independent by construction,
so a future path that appends another copy into `slot.buf` cannot get behind it.

**Phase 8 — measurement**

| # | Name | Kind | Notes |
|---|---|---|---|
| T80 | `timerResolve` | Copy | `vkCmdCopyQueryPoolResults`; `--measure` only, `passTimer_ != null`. Timestamps are written by `vkCmdWriteTimestamp2` around rows carrying a timer label, which does not change any dispatch — the determinism claim in `passtimer.h` survives the port unchanged. |

### 2.5 The non-tick tables

Same schema, separate arrays, each recorded into its own command buffer. There
are **six**, and the sixth is the one an implementer working only from
`EncodeTick` will miss.

**2.5.1 `worldgen`** (`simulation.cpp:550-566`): fills `dirty[0]`, `dirty[1]`,
`hash`, `support`, `particleCounts`, `claim`, `drawArgs` (7 Fill rows, no
hazards among them *pairwise*), then `worldgen_` direct `(4096,1,1)` reading
`materials`/`tickUBO`, writing `voxels`:SW, `occupancy`:SW, `DirtyIn`:AtomicRMW,
`DirtyOut`:AtomicRMW. The fills of `dirty[0/1]` **do** hazard against the
dispatch — `genChunk` `atomicStore`s both (`worldgen.wgsl:2604-2610`) — so a
TRANSFER→COMPUTE barrier is required there, and "no hazards among them" applies
only to the fills against each other.

**2.5.2 `genList`** (`simulation.cpp:568-576`): `worldgenList_` direct
`(count,1,1)`, reads `genList`:SR, `materials`:SR, `tickUBO`:U; writes
`voxels`:SW, `occupancy`:SW, `DirtyIn`:AtomicRMW, `DirtyOut`:AtomicRMW.
Recorded in its own encoder and submitted **mid-frame, between ticks**, from
`Stream::FillSlots` (`stream.cpp:274-277`).

Two things about this row that are easy to get wrong:

- **It binds `simBG_` (all 17 bindings) but `worldgen:list`/`genChunk` touch
  seven of them.** `dirtyList`, `argsStage`, `opsBuf`, `cellOps`, `support`,
  `pick`, `hash`, `passUBO`, `reactions`, `renderUBO` are bound and never
  referenced. The `uses` set is the seven, per §2.1 rule 1 — and this is
  precisely the trap §6.1's finding-2 fix exists to keep the checker from
  flagging, now on a mid-frame submit where a spurious barrier would be
  charged against streaming latency.
- **`genChunk` `atomicStore`s both `dirtyIn` and `dirtyOut`**
  (`worldgen.wgsl:2604-2610`), while the concurrently-in-flight tick's
  `markDirty`/`markBoth` are atomically writing the same two buffers. That is a
  **cross-submit WAW against a shader atomic**, structurally identical to the
  `particleCounts` case in §4.4/§7.7 and covered by the same mechanism: §3.4's
  head-of-command-buffer global barrier orders the genList submit's writes after
  every prior submit's. Naming it here so it is not rediscovered as a bug.
  **[NEW EDGE]**

**2.5.3 `loadReset`** (`:588-601`): 5 fills + `occupancy_` full.
**2.5.4 `hashOnly`** (`:603-611`): fill `hash` + `occupancy_` full.
**2.5.5 `wakeAll`** (`:613-620`): an upload only — no command buffer today;
§4.1 gives it a home.

**2.5.6 `farFillOnly`** — the `--shot` far-field drain loop
(`RunShots`, `main.cpp:139-147`).

This path records GPU work in a command buffer that contains **only** T60, in a
loop, with no tick anywhere in it:

```c++
while ((n = far.PrepareTick(ctx.queue)) > 0) {   // writes farUBO (when dirty)
  TickParams tp{0, kDefaultSeed, 0, 0};          //         + farList, every iter
  tp.farCount = n;
  ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
  enc = device.CreateCommandEncoder();
  sim.EncodeFarFill(enc, n);                     // == T60, nothing else
  queue.Submit(1, &enc.Finish());
}
```

| # | Name | Kind | Reads | Writes | Cond |
|---|---|---|---|---|---|
| S00 | `upload.tickUBO` | Copy/Update 64 B | staging | `tickUBO`:TransferWrite | every iteration |
| S01 | `upload.farUBO` | Copy/Update 128 B | staging | `farUBO`:TransferWrite | `uboDirty_` (`farfield.cpp:103-113`) |
| S02 | `upload.farList` | Copy/Update ≤16 KiB | staging | `farList`:TransferWrite | every iteration |
| S03 | `farFill` (= T60) | Compute `(n,1,1)` wg`(64)` | `materials`:SR, `tickUBO`:U, `farList`:SR, `farUBO`:U | `farVox`:AtomicRMW, `farOcc`:AtomicRMW | every iteration |

**The upload→dispatch RAW on `tickUBO` is load-bearing per iteration, not
once.** `farCount` is carried *in `tickUBO`* (`main.cpp:141`, read by
`worldgen.wgsl:far` as `T.farCount` at its early-out) — it is not a push
constant and not a dispatch dimension. So every iteration writes a different
`farCount` into the same 64-byte uniform buffer and immediately dispatches
against it. Miss the TRANSFER_WRITE→UNIFORM_READ barrier and iteration k
dispatches `n_k` workgroups whose early-out tests `n_{k-1}`, silently dropping
or duplicating cascade fills. Because each iteration is its own submit, §3.4's
head barrier covers the cross-submit half; the intra-command-buffer half
(S00/S02 → S03) is an ordinary table-derived barrier, which is exactly why this
path must be *in* a table.

`farVox`/`farOcc` carry an iteration-to-iteration WAW across submits, covered by
§3.4. Nothing here is hashed (far-field is render-only, DESIGN.md §9), so this
path cannot desync — but a wrong `farCount` produces the sky-hole artifacts
`--shot` exists to inspect, which makes it self-defeating rather than harmless.

### 2.6 The render table

Recorded per frame, one submit. Its only interesting property for this document
is what it reads that the tick wrote.

| # | Name | Kind | Reads (buffers) |
|---|---|---|---|
| R0 | `hoisted uploads` | Copy | writes `mbInstBuf_`, `bodyInstances`, `bodyXforms`, `sprites`, `debugBoxes`, `renderUBO`, `mbModelBuf_`, `mbPoolBuf_` — **before** `vkCmdBeginRendering` (§4.6) |
| R1 | `raymarch` | Draw(3) | `voxels`, `occupancy`, `materials`, `renderUBO`, `farVox`, `farOcc`, `farUBO`, `microTable`, `microPool` |
| R2 | `particles` | DrawIndirect `drawArgs@0` | `materials`, `renderUBO`, `Particles[page]`, `drawArgs`:IndirectRead |
| R3 | `bodies` | Draw(36,n) | `materials`, `renderUBO`, `bodyInstances`, `bodyXforms` |
| R4 | `microBodies` | Draw(36,n) | `materials`, `renderUBO`, `bodyXforms`, `mbModelBuf_`, `mbPoolBuf_`, `mbInstBuf_` |
| R5 | `sprites` | Draw(36,n) | `materials`, `renderUBO`, `sprites` |
| R6 | `debugBoxes` | Draw(72,n) | `materials`, `renderUBO`, `debugBoxes` |
| R7 | `overlay` | ImGui | opaque; recorded last, inside the same rendering scope |

R1–R7 are read-only over everything the tick wrote, so **no barriers are needed
between them**. The barriers that matter are R0→R1 (transfer→vertex/fragment
shader read) and the cross-submit tick→frame edge (§3.5).

---

## 3. Barrier derivation

### 3.1 State tracked per buffer

```c++
struct BufState {
  VkPipelineStageFlags2 lastWriteStage;   // 0 = never written since record start
  VkAccessFlags2        lastWriteAccess;
  VkPipelineStageFlags2 readStagesSince;  // OR of all reads since lastWrite
  VkAccessFlags2        readAccessSince;
};
```

`readStagesSince`/`readAccessSince` accumulate; they are cleared when a write is
issued. This is the standard minimal tracker and it is enough for a total order.

### 3.2 Access → stage/access mask mapping

| `Acc` | `VkPipelineStageFlags2` | `VkAccessFlags2` | Is write? |
|---|---|---|---|
| StorageRead | `COMPUTE_SHADER` | `SHADER_STORAGE_READ` | no |
| StorageWrite | `COMPUTE_SHADER` | `SHADER_STORAGE_WRITE` | yes |
| StorageRW | `COMPUTE_SHADER` | `SHADER_STORAGE_READ \| SHADER_STORAGE_WRITE` | yes (and read) |
| StorageAtomicRMW | `COMPUTE_SHADER` | `SHADER_STORAGE_READ \| SHADER_STORAGE_WRITE` | yes (and read) |
| Uniform | `COMPUTE_SHADER` | `UNIFORM_READ` | no |
| IndirectRead | `DRAW_INDIRECT` | `INDIRECT_COMMAND_READ` | no |
| TransferRead | `COPY` (`ALL_TRANSFER` acceptable) | `TRANSFER_READ` | no |
| TransferWrite | `COPY` / `CLEAR` for fills | `TRANSFER_WRITE` | yes |
| HostRead | `HOST` | `HOST_READ` | no |

For the render table, `StorageRead`/`Uniform` map to
`VERTEX_SHADER \| FRAGMENT_SHADER` instead of `COMPUTE_SHADER`; `IndirectRead`
stays `DRAW_INDIRECT`. The stage is a property of the *table* (each table
declares its shader-stage domain), not of the `Acc` enum, so one enum serves
both.

**`StorageAtomicRMW` is `READ|WRITE`, not something weaker.** Vulkan has no
atomic-specific access flag. Two consecutive atomic-only passes on the same
buffer (`farDown` then `farFill` on `farVox`) still need a WAW barrier: atomics
guarantee per-operation atomicity, not visibility of one dispatch's results to
the next. §3.6 covers whether that barrier can be relaxed. (It cannot; see
§7.10's finding on `farFill`'s `atomicStore`.)

### 3.3 The algorithm

For each row in table order, with condition true:

```
srcStage = 0; srcAccess = 0; dstStage = 0; dstAccess = 0
barriers = []

for each Use u in row.uses:
    s = BufState[u.buf]
    (uStage, uAccess) = map(u.acc, table.shaderStageDomain)

    if isWrite(u.acc):
        # WAW against the last writer, and WAR against every reader since.
        # Both are expressed by one barrier whose src covers write+reads.
        if s.lastWriteStage != 0 or s.readStagesSince != 0:
            emit buffer barrier {
              buf:        u.buf,
              srcStage:   s.lastWriteStage | s.readStagesSince,
              srcAccess:  s.lastWriteAccess | s.readAccessSince,
              dstStage:   uStage,
              dstAccess:  uAccess,
            }
        s.lastWriteStage  = uStage
        s.lastWriteAccess = uAccess & WRITE_MASK
        s.readStagesSince = 0
        s.readAccessSince = 0
        # StorageRW / AtomicRMW also read: fold the read into the same barrier's
        # dstAccess (already done — uAccess carries READ|WRITE), and record the
        # read so a later writer sees it.
        if alsoReads(u.acc):
            s.readStagesSince = uStage
            s.readAccessSince = uAccess & READ_MASK
    else:
        # RAW against the last writer only. Read-after-read needs nothing.
        if s.lastWriteStage != 0:
            emit buffer barrier {
              buf:       u.buf,
              srcStage:  s.lastWriteStage,
              srcAccess: s.lastWriteAccess,
              dstStage:  uStage,
              dstAccess: uAccess,
            }
        s.readStagesSince |= uStage
        s.readAccessSince |= uAccess

if barriers not empty:
    vkCmdPipelineBarrier2(cb, {bufferMemoryBarrierCount: n, ...})
record the dispatch/copy/fill
```

Three properties worth stating explicitly, because a reviewer should check them:

- **WAR is covered without a separate case.** A write folds
  `readStagesSince`/`readAccessSince` into its own barrier's src scope. A WAR
  hazard needs only execution dependency (no cache flush), and passing a read
  access in `srcAccessMask` is legal and harmless — the implementation may skip
  the (unnecessary) availability operation. Splitting WAR into a
  memory-barrier-free execution barrier is a valid optimization that v1 does not
  take.
- **Read-after-read emits nothing.** Correct: `s.lastWriteStage` is unchanged
  and both reads are already visible.
- **The first touch of a buffer in a command buffer emits nothing** if it has
  never been written in this recording. That is *only* safe because of the
  cross-submit rule in §3.4 — do not read this as "buffers start clean".

### 3.4 Cross-submit and cross-command-buffer state

`BufState` is reset at the start of each command-buffer recording. Nothing in
Vulkan makes memory written by submit N automatically visible to submit N+1 —
submission order gives execution ordering guarantees between *submits on the
same queue* for the implicit-ordering guarantee, but memory availability and
visibility still require a barrier or an equivalent operation.

**v1 rule: every command buffer opens with a single global memory barrier.**

```
vkCmdPipelineBarrier2(cb, {
  memoryBarrierCount: 1,
  pMemoryBarriers: &{
    srcStageMask:  ALL_COMMANDS, srcAccessMask:  MEMORY_WRITE,
    dstStageMask:  ALL_COMMANDS, dstAccessMask:  MEMORY_READ | MEMORY_WRITE,
  }})
```

One barrier per submit, on a submit that is already hundreds of microseconds of
work. This makes "state resets per command buffer" sound, removes the entire
class of cross-submit reasoning errors (§3.5's `drawArgs`, §3.7's streaming,
§3.8's page flip), and costs one pipeline flush per submit — which the queue
was going to do anyway at the submit boundary in practice.

**JUDGMENT.** The alternative (persist `BufState` across command buffers in the
backend and emit precise cross-submit barriers) is more precise and strictly
harder to get right, because the CPU-side reordering — `FlipPage` after submit,
readbacks kicked after submit, streaming submits interleaved mid-frame — means
the "previous command buffer" is not always the one you think. Correctness
first. If a measurement ever shows this barrier costs real time, revisit it with
a hash gate.

### 3.5 Batching

All barriers for one row are emitted in **one** `vkCmdPipelineBarrier2` with N
`VkBufferMemoryBarrier2` entries. Merging further (deferring barriers across
rows) is not done: the recorder emits immediately before the row that needs
them, which is both the tightest scope and the easiest to read in a capture.

Rows T70–T78 (the readback copies) are the one place where the natural output is
dozens of barriers that are all no-ops. The algorithm already produces zero
there: `voxels` was last written by `pResolve`/`ca[53]`, so T70's first read of
`voxels` emits one COMPUTE→TRANSFER barrier, and T71's 27 further reads of
`voxels` emit nothing (read-after-read). Good.

### 3.6 Special case: the CA loop

**Read this before touching anything in this section: the 54 iterations must
never overlap, and that is a requirement of the color lattice, not of memory
visibility.**

`sim_step.wgsl:1-9` states the invariant the whole determinism argument rests
on: within one dispatch, every acting cell shares one `colorPhase`, so any two
acting cells are ≥3 apart on every axis while writes reach ≤1 cell, and
destination writes are *provably disjoint*. The disjointness holds **within a
color phase and nowhere else.** `colorPhase` changes every iteration — it
arrives through the dynamic `passUBO` offset `k * kPassStride`
(`simulation.cpp:682-683`), a different 256-byte slice per k. Two iterations
running concurrently are two *different* colors running concurrently, and cells
of different colors are adjacent by construction: their writes overlap, and the
outcome depends on which workgroup got there first.

So the inter-iteration barrier is not an optimization knob and not a
cache-flush detail — **it is the mechanism that makes the color lattice mean
anything.** Remove it and rule 1 is gone, not degraded: the sim becomes
first-come-first-served at every color boundary, which CLAUDE.md rule 1 bans by
name.

The practical consequence: **batching CA iterations is the single easiest way
to destroy determinism while believing you are optimizing barriers.** Any
future change that merges iterations, drops "redundant" barriers between them,
or moves them onto separate queues is a determinism change requiring a
cross-vendor hash gate, regardless of how it is framed. This is restated as
§7.1, the top risk.

With that established — iteration k+1 also reads `voxels` that iteration k
wrote, and both write `DirtyOut` and `support` atomically. The algorithm
produces, between every pair, three buffer barriers:

| buffer | src | dst |
|---|---|---|
| `voxels` | COMPUTE / STORAGE_READ\|WRITE | COMPUTE / STORAGE_READ\|WRITE |
| `DirtyOut` | COMPUTE / STORAGE_READ\|WRITE | COMPUTE / STORAGE_READ\|WRITE |
| `support` | COMPUTE / STORAGE_READ\|WRITE | COMPUTE / STORAGE_READ\|WRITE |

— i.e. 53 identical `vkCmdPipelineBarrier2` calls carrying 3 identical
`VkBufferMemoryBarrier2` each, plus `dirtyList`/`dispatchArgs`/`materials`/
`reactions`/`passUBO` reads that correctly emit nothing after the first
iteration.

**Should this be one global `VkMemoryBarrier2` instead?**

The two forms are:

- (A) 3 buffer barriers, scopes as above.
- (B) 1 `VkMemoryBarrier2` with
  `src/dstStage = COMPUTE_SHADER`, `src/dstAccess = SHADER_STORAGE_READ | SHADER_STORAGE_WRITE`.

**Decision: (B), one global memory barrier per CA iteration.** Reasons, in the
order that matters:

1. **(B) is stronger than (A) *within the compute-storage access domain*, which
   is where every hazard inside the loop lives.** It is **not** a superset of
   (A) in general, and an earlier draft of this document claimed it was. A
   `VkMemoryBarrier2` with `srcStage/dstStage = COMPUTE_SHADER` and
   `srcAccess/dstAccess = SHADER_STORAGE_READ | SHADER_STORAGE_WRITE` covers
   *every buffer* — including a binding a future edit adds to `sim_step.wgsl`
   without updating the table — but only for *those stages and those access
   types*. It does **not** cover `INDIRECT_COMMAND_READ` at `DRAW_INDIRECT`, and
   `sim_step` consumes `dispatchArgs` as an indirect source on every iteration.

   Why the loop is nonetheless sound: **nothing writes `dispatchArgs` inside the
   loop.** T20 copies into it once, before iteration 0 (`simulation.cpp:672`);
   the loop body is `SetBindGroup` + `DispatchWorkgroupsIndirect` and nothing
   else (`simulation.cpp:681-685`). The tracker therefore emits the
   `TRANSFER_WRITE → INDIRECT_COMMAND_READ` barrier once, ahead of the loop, and
   correctly emits nothing for iterations 1–53. **That soundness comes from the
   tracker, not from the global barrier**, and the distinction matters: if a
   future change ever wrote `dispatchArgs` between iterations, form (B) would
   silently fail to order it while form (A) — driven by the same tracker — would
   not, because the tracker would emit a per-buffer barrier for it.

   The design keeps (B) and makes the assumption checkable rather than implicit.
   **Checker assertion (§6.1): no row carrying `useGlobalBarrier` may declare an
   `IndirectRead` of a buffer that is written anywhere inside its own repeat
   span.** T30 satisfies it. Widening the CA barrier to include
   `DRAW_INDIRECT`/`INDIRECT_COMMAND_READ` in both scopes is the alternative and
   is also correct; it is rejected only because it makes the barrier look like
   it is doing something it is not, and the assertion is the honest form of the
   same guarantee.

   **[AS BUILT, phase 3b] The implementation takes a third option, and it is
   strictly safer than either.** `Recorder::FlushPending` still runs the full
   per-buffer tracker for a global row — it computes the pending
   `VkBufferMemoryBarrier2` list exactly as form (A) would — and only *then*
   collapses it. The collapse is not a discard: any pending barrier carrying an
   access outside `SHADER_STORAGE_READ|WRITE` has its stages and accesses ORed
   into the global barrier before the list is dropped. So on today's table it
   emits precisely form (B) (nothing in the CA loop's pending list is outside
   the domain after iteration 0), and if a future edit ever did write
   `dispatchArgs` between iterations, the global barrier would automatically
   widen to `DRAW_INDIRECT`/`INDIRECT_COMMAND_READ` rather than silently failing
   to order it.
   This makes the §6.1 checker assertion a defence-in-depth check rather than
   the sole guarantee, which is the right ordering for something rule 1 rests
   on: the code is correct without the checker, and the checker still catches
   the table drift that motivated it. The cost is computing a barrier list that
   is usually thrown away — a few dozen struct writes per CA iteration, on the
   CPU, in a loop that is already issuing a dispatch.

2. **Atomic visibility is covered; indirect visibility is not — and that is the
   same distinction.** §3.2 establishes that `StorageAtomicRMW` maps to
   `SHADER_STORAGE_READ | SHADER_STORAGE_WRITE` because Vulkan has no
   atomic-specific access flag, and that consecutive atomic-only passes still
   need a real barrier. Form (B)'s access mask contains exactly those two bits,
   so **the loop's atomic writes to `DirtyOut` and `support` are fully covered
   by (B)**. The indirect read is the one access in the loop whose flag is
   outside (B)'s mask, which is why it needs the argument in point 1 rather than
   falling out of it. Answering the question directly: atomics — yes; indirect —
   no.

3. **In practice every real driver implements a compute→compute buffer barrier
   on a 512 MiB device-local buffer as a full cache flush + invalidate anyway.**
   Buffer barriers on VkBuffers do not carry layout information; the range is
   advisory to nearly all implementations. Form (A) buys measurable time only on
   implementations that do per-range tracking, which for storage buffers is
   uncommon.

4. **It is one API call with one struct instead of three, in a loop that runs 54
   times per tick at 30 Hz** — a small but free CPU-side recording win.

The cost of (B) is that it also orders `materials`, `reactions` and the UBOs
that nothing writes — no hazard, no work, no flush beyond the one already
happening.

**This decision applies to the CA loop only.** Elsewhere in the tick, per-buffer
barriers stay, because elsewhere the table's precision is what documents the
graph: a capture showing `barrier(support)` between T30 and T76 is
self-explaining in a way `barrier(everything)` is not. The recorder therefore
carries a per-row flag `useGlobalBarrier`, set on T30 only, and the checker
asserts it is set on exactly the rows the table marks.

**JUDGMENT**, and the one most worth a reviewer's disagreement: if the reviewer
prefers uniformity, the safe direction is to make *everything* a global barrier,
never to make the CA loop per-buffer.

### 3.7 Special case: indirect args

Five sites (`vulkan_pass_map.md` §4). Each is a Copy row followed later by a
row with an `IndirectRead` use of the destination. The algorithm handles this
with no special casing at all — the copy leaves
`lastWrite = {COPY, TRANSFER_WRITE}`, the indirect read maps to
`{DRAW_INDIRECT, INDIRECT_COMMAND_READ}`, and the RAW branch emits exactly:

```
srcStage = VK_PIPELINE_STAGE_2_COPY_BIT       srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT
dstStage = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
dstAccess = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT
```

`DRAW_INDIRECT` is the correct stage for `vkCmdDispatchIndirect`'s argument
fetch as well as `vkCmdDrawIndirect`'s — the name is historical.

The subtlety the algorithm gets right and a hand-written version would not: for
the CA loop, the `dispatchArgs` barrier is emitted once, before iteration 0, and
**not** repeated for iterations 1–53, because no row between them writes
`dispatchArgs`. The pass map's instruction ("`dispatchArgs` needs
`INDIRECT_COMMAND_READ` visibility once before the loop, not per iteration")
falls out of the tracker rather than needing to be remembered — which is the
whole argument for generating barriers from a table.

The pDispatchArgs chain is the one that bites: T41 copy → T42 indirect read →
T43 writes `pArgsStage` → T44 copies it over `pDispatchArgs` **while T42's
indirect fetch may still be in flight**. That is a WAR on `pDispatchArgs`
(indirect read → transfer write) and the algorithm emits it from
`readStagesSince = DRAW_INDIRECT`, `readAccessSince = INDIRECT_COMMAND_READ`.
Losing this one is a classic: it produces a dispatch sized by the *wrong* args,
intermittently. §7.2.

### 3.8 Special case: atomic-only WAW on `farVox` / `farOcc`

T5A (`farDown`) and T60 (`farFill`) both touch `farVox`/`farOcc` with atomics
only, in that order, in the same command buffer. The algorithm emits a
COMPUTE→COMPUTE `READ|WRITE`→`READ|WRITE` barrier between them. That is correct
and necessary — see §7.8 for why the "they're both atomic, atomics are coherent"
intuition is wrong here specifically.

The far-field buffers are render-only derived data (DESIGN.md §9), excluded from
the world hash and from persistence. A missing barrier there therefore cannot
desync the sim — it can only produce a visibly wrong cascade. That makes them
**lower risk, not zero risk**, and the table does not special-case them.

### 3.9 Special case: skipped conditional rows

Covered as a risk in §7.5 because the failure mode is subtle. The mechanism is
one line of the algorithm: a row whose condition is false is never visited, so
it never touches `BufState`, so the next row that *is* visited computes its
barrier against the last actual accessor. There is no "skipped pass" concept in
the recorder at all — which is precisely why this is safe, and why any
implementation that pre-computes barriers per adjacent table-index pair (rather
than at record time against live state) is wrong.

---

## 4. Submit-boundary synchronization

The 15 implicit ordering assumptions in `vulkan_pass_map.md` §6, each with its
replacement. Assumptions 1–4 are the intra-command-buffer cases resolved
entirely by §3; the rest need mechanism.

### 4.0 Assumptions 1–4: intra-command-buffer

| # | Assumption | Replacement |
|---|---|---|
| 1 | auto barriers between dispatches in one ComputePassEncoder | §3.3, rows T10–T15 / T30 loop / T42–T43 / T50–T51 |
| 2 | auto barriers between passes in one encoder | §3.3 — identical mechanism; "pass" ceases to exist |
| 3 | `ClearBuffer` ordering, incl. `support` copy-then-clear | §3.3, `Fill` rows are TransferWrite; T76→T77 is a tracked WAR |
| 4 | copy → indirect visibility, 5 sites | §3.7 |

### 4.1 (a) The upload path — replacing `queue.WriteBuffer` (assumption 5, 9)

WebGPU's `queue.WriteBuffer` is defined to happen *at the start of the next
submit*, in queue order, with no barrier needed by the caller. Two things about
that definition have to be reproduced, and only one of them is about barriers:

- **"at the start of the next submit"** — the write is deferred, and it attaches
  to whichever submit happens next, from whatever code path issues it.
- **"in queue order"** — writes to the same range land in issue order, so the
  last writer before a submit wins.

The second is where a naive port breaks. `Stream::FillSlots`
(`stream.cpp:245-260`) writes `voxels` (16 KiB), `occupancy` (4 B),
`dirty[0]` (4 B) and `dirty[1]` (4 B) per store-hit slot — and **if every slot
hits the store, it submits nothing at all**: the submit at `stream.cpp:274-277`
is guarded by `if (!genSlots.empty())`. A model phrased as "uploads are recorded
at the head of *that submit's* command buffer" has no home for those writes.
They belong to the *next* command buffer submitted from *any* path, which is
usually the next tick.

#### The pending-upload queue

```c++
struct PendingUpload {
  BufId    dst;
  uint64_t dstOffset;
  uint64_t size;
  uint64_t stagingOffset;   // Class B: into the staging ring
  const void* inlineData;   // Class A: captured at record time (see below)
};
std::vector<PendingUpload> pendingUploads_;   // issue order, never reordered
```

Every call site that calls `queue.WriteBuffer` today calls
`Rhi::QueueWrite(dst, offset, data, size)` instead, which appends to
`pendingUploads_` (copying the payload into the staging ring for Class B, or
into a small CPU-side arena for Class A). **The recorder flushes the entire
queue at the head of the next command buffer it records, from whichever path
records it, in issue order, then clears it.** Nothing else drains it.

This reproduces both halves of the WebGPU semantics:

- *Deferred, attaches to the next submit*: by construction — the queue survives
  across `FillSlots` returning without submitting, and drains into the next
  tick's command buffer.
- *In queue order, last-write-wins per range*: by preserving issue order and
  never coalescing. The live case is `tickUBO`: `FillSlots` writes it at
  `stream.cpp:268-273` for the genList submit; if that submit happens, the queue
  drains there. If it does *not* happen (no `genSlots`), the streaming
  `TickParams` sits in the queue and is then followed by `SubmitTick`'s own
  `tickUBO` write (`support.cpp:126`), and the tick's write — issued later —
  lands later and wins. Both orderings are correct, and both are the same
  ordering WebGPU produces today.

A consequence worth stating: **`pendingUploads_` must be flushed before
`vkQueueWaitIdle`-style drains and at shutdown**, or a `FillSlots` that submitted
nothing right before a `WaitIdle` leaves its writes unapplied. The recorder
exposes `FlushUploads()` for exactly the `--shot`/save/load/shutdown call sites
in §4.9.

#### Class A — `vkCmdUpdateBuffer`

**Rule: Class A iff `size ≤ 65536` AND the payload is available at record time
AND the size is a multiple of 4 (`vkCmdUpdateBuffer` requires 4-byte-aligned
offset and size).** Nothing else. This is the one consistent rule; an earlier
draft applied it inconsistently (`farList` at 16 KiB in Class A while `passUBO`
at 13.5 KiB was in Class B) and that inconsistency is resolved here in favor of
the size rule.

`vkCmdUpdateBuffer` **captures the data into the command buffer at record
time**, which is why no staging allocation and no lifetime tracking are needed
— and it is also why the `--shot` far-fill loop (§2.5.6) is safe in Class A:
each iteration records its own `tickUBO` payload into its own command buffer, so
the loop's rapid overwrites of one 64-byte buffer cannot alias.

Class A covers: `tickUBO` (64 B), `renderUBO`, `farUBO` (128 B), `opsBuf`
(≤2 KiB), `expOps` (≤256 B), `particleCounts` (4 B partial write at
`(1-page)*4`), `sprites` (≤2 KiB), `bodyXforms` (≤16 KiB), `farList` (≤16 KiB),
`dirty[page]` on the day/night wake (16 KiB), `genList` (≤16 KiB), `mbInstBuf_`
(≤8 KiB), `mbModelBuf_` (4 KiB), `passUBO` (13.5 KiB, once at init),
`debugBoxes`, and the three streaming partial writes:

- **`occupancy` @ `slot*4`, 4 B** (`stream.cpp:257`)
- **`dirty[0]` @ `slot*4`, 4 B** (`stream.cpp:259`)
- **`dirty[1]` @ `slot*4`, 4 B** (`stream.cpp:260`)

These three are per-slot, so a streaming shift issues up to a few hundred of
them. They are partial writes into live buffers — the same shape as
`SetArtPalette` (assumption 9) — and each is one `vkCmdUpdateBuffer` of 4 bytes.
The recorder tracks the whole buffer as `TransferWrite` (per §2.2's
whole-buffer granularity), so the *first* one in a flush emits any needed
barrier and the rest emit nothing.

`debugBoxes` sits **exactly on the boundary**: `kMaxDebugBoxes = 1024`
(`world.h:156`) × `sizeof(DebugBox) = 64` (`world.h:155` static_assert) = 65536
bytes, and the limit is "≤ 65536", so a full-capacity write is legal by one
byte. `microTableBuf_` is identical: `kMaterialSlots = 4096` ×
`sizeof(MicroBrickGpu) = 16` (`microvox.h:67` static_assert) = 65536.

Both are Class A by the size rule, and both are one constant-bump away from
being illegal. **The recorder `static_assert`s every Class A buffer's capacity
against 65536** rather than relying on a reader noticing; raising
`kMaxDebugBoxes` or `kMaterialSlots` then fails the build instead of producing
a validation error at runtime. Do not instead move them to Class B "for safety"
— a size-derived rule with two hand-made exceptions is the kind of rule that
gets applied wrong by the next person.

**Class B — persistent-mapped staging ring + `vkCmdCopyBuffer`, for > 65536 B**
(or for any payload not available at record time).
A ring of `HOST_VISIBLE | HOST_COHERENT` buffers, persistently mapped, sized to
comfortably hold one frame's worth of large uploads (16 MiB is ample: the worst
tick is `cellOps` 512 KiB + `spawnOps` 128 KiB + `bodyInstances` 4 MiB +
`microPoolBuf_`/`mbPoolBuf_` 4 MiB each on a hot reload). The CPU memcpies into
the ring inside `QueueWrite`, and the recorder emits a `vkCmdCopyBuffer` when
the pending queue flushes. Ring regions are reclaimed by the fence of the submit
that consumed them — **which is why §4.2 gives every submit a fence, not just
the ones that carry a readback.**

Class B covers: `cellOps` (≤512 KiB), `spawnOps` (≤128 KiB),
`bodyInstances` (≤4 MiB), `materialBuf_` (4096 × `sizeof(MaterialGpu)`),
`reactionBuf_`, `microPoolBuf_` (4 MiB), `mbPoolBuf_` (4 MiB), and streaming's
per-slot 16 KiB `voxels` writes (`stream.cpp:247-248`).

**Why HOST_COHERENT and no `HOST_WRITE` barrier in the common path.** With
coherent memory, host writes are automatically available to the device at the
next queue submit — Vulkan guarantees that a queue submit performs an implicit
domain operation making prior host writes visible, provided the memory is
coherent. Non-coherent memory would need `vkFlushMappedMemoryRanges`. The
`HOST_WRITE → TRANSFER_READ` barrier is therefore **not** required for staging
written before the submit that reads it, and adding one is harmless noise. It
*is* required if a host write ever happens to memory already in use by a
submitted-but-unretired command buffer, which the ring's fence discipline
prevents by construction.

**Ordering and barriers.** The flush emits its uploads at the **head** of the
command buffer, in issue order, before any pass row — and each one goes through
the ordinary `TransferWrite` path in §3.3, so hazards against the rows that
follow are derived, not assumed. This makes `SetArtPalette`'s partial write into
a live `materialBuf_` (assumption 9) safe by the same mechanism as everything
else: the recorder marks `materialBuf_` written, and the first shader read of it
in the tick gets a TRANSFER→COMPUTE barrier automatically.

**`EncodeWakeAll` needs no command buffer of its own.** Today it is a bare
`queue.WriteBuffer` with no encoder (`simulation.cpp:613-620`), issued from
`SubmitTick` (`support.cpp:155`) before the tick's encoder is created. Under the
pending-upload queue it is simply a `QueueWrite` that drains at the head of the
tick's command buffer, ahead of T00 — no special case at all, which is the
point of the queue model. The write must land before `compact` (T15) reads
`DirtyIn`, and the flush ordering guarantees it.

**[NEW EDGE]** T00 fills `dirty[1-page]` while the wake writes `dirty[page]` —
different buffers, no hazard *as long as the symbolic resolution is right*. If
`DirtyIn`/`DirtyOut` ever resolved to the same id, T00's fill would silently
clobber the wake and the world would fail to wake at a day/night boundary. The
recorder's symbolic resolution (§2.2) makes this a checkable property rather
than a coincidence; the checker asserts `DirtyIn != DirtyOut` after resolution.

### 4.2 (b) The readback ring on real fences (assumption 10)

Today: `Slot::inFlight` is a bool, set before `MapAsync`, cleared in the map
callback, and the callback only runs inside `ctx.ProcessEvents()`
(`main.cpp:2833`). `EncodeReadbacks` returns false when all 3 slots are in
flight, and the tick simply skips its copies (`world.cpp:109-113`).

Vulkan replacement, one fence per slot:

```c++
struct Slot {
  VkBuffer      buf;        // HOST_VISIBLE|HOST_COHERENT, persistently mapped
  VkFence       fence;      // signalled by the submit that wrote this slot
  bool          inFlight;   // fence submitted, not yet observed signalled
  /* + the existing base/origin/tick/fetchIds/particleLivePage payload */
};
```

- **Every submit gets a fence. No exceptions.** An earlier draft said a tick
  recording no readback "submits with `VK_NULL_HANDLE`", which is wrong for a
  reason that has nothing to do with readbacks: **Class B staging-ring regions
  are reclaimed by the fence of the submit that consumed them** (§4.1), so a
  fenceless submit leaks its ring region permanently. The `--shot` far-fill loop
  (§2.5.6) is the case that proves it — it submits in a tight loop, carries no
  readback slot, and uploads `farList` every iteration; with fences only on
  readback submits, that loop exhausts the ring and deadlocks.

  So: the backend owns a small pool of fences, one per in-flight submit, and
  `vkQueueSubmit` always takes one. Retiring a fence does two things —
  reclaims any staging-ring regions charged to it, and releases the command
  buffer for reuse (§4.4).
- **Which fence signals a readback slot.** When a tick records rows T70–T78, the
  claimed slot *borrows a reference to that submit's fence*. The slot does not
  own a fence of its own; `Slot::fence` above is that borrowed handle. This
  keeps the 1:1 mapping onto today's `lastSlot_` while decoupling fence
  lifetime from readback lifetime.

  **[AS BUILT, phase 3c] A borrowed fence needs a RETAIN, and the first
  implementation without one was silently wrong.** The fence pool recycles a
  signalled fence into its free list as soon as `PollFences` observes it, and
  `BeginCommands` calls `PollFences` on *every* command buffer. So a slot that
  submitted at tick N and had not yet been polled by tick N+1 held a handle
  `AcquireFence` had already reset and handed to the tick-N+1 submit.
  `vkGetFenceStatus` on it then reports **a different submit's** status: the
  slot reads its mapped memory when some unrelated command buffer finishes,
  which for a 3-deep ring means reading a slot the GPU is still writing. That is
  silent corruption of the CPU mirror — no crash, no validation message, and the
  consumers (`KindAt`, the streaming evict filter, the sleep assertion) simply
  get wrong answers. `Backend::RetainFence`/`ReleaseFence` refcount the borrow;
  a retained fence whose submit retires is *parked* rather than pooled and
  returns to the pool on the last release. The borrowed-fence model here is
  right; what it was missing was the retain that makes "borrowed" true.
- **Where the CPU polls.** `ctx.ProcessEvents()`'s replacement, called at the
  same point in the frame (`main.cpp:2833`), walks the 3 slots and calls
  `vkGetFenceStatus`. On `VK_SUCCESS`: read the mapped pointer, run the exact
  body of today's `MapAsync` callback (`world.cpp:180-232`), `vkResetFences`,
  clear `inFlight`. Never blocks — `vkGetFenceStatus` is a poll, matching
  `AllowProcessEvents` semantics.
- **"Skip when all in flight."** Unchanged, and now it is a real statement about
  the GPU: `EncodeReadbacks` scans for `!inFlight` and returns false if none.
  Today's version is already correct for the same reason; the fence just makes
  the flag mean what it claims.
- **Visibility.** With `HOST_COHERENT` slot memory, a fence-signalled submit's
  writes are visible to the host. On top of that, the recorder emits
  `srcStage=COPY, srcAccess=TRANSFER_WRITE → dstStage=HOST, dstAccess=HOST_READ`
  for every host-visible buffer written during the recording — **at `Finish()`
  time, as the last command in the buffer**, per the rule in §2.4 phase 7b.
  Emitting it at a fixed table index is the bug that rule exists to prevent:
  `EncodeDirtyCopy` (T78) is a separate function called *after* `EncodeReadbacks`
  returns, so an index-anchored barrier would leave `slot.buf`'s `kDirtyOff`
  range — the `dirtyFlags` the host reads at `world.cpp:186-192` — behind the
  barrier meant to make it visible. **JUDGMENT:** with coherent memory and a
  fence this barrier is arguably redundant, but it is one barrier per submit and
  it makes the visibility requirement explicit. Keep it.
- **Blocking readbacks** (`ReadHashSync`, selftest voxel dumps, screenshots)
  become: record copies → submit with a fence → `vkWaitForFences(UINT64_MAX)` →
  read the map. The one sanctioned synchronous path stays exactly as sanctioned.

### 4.3 (c) Eviction staging pool cross-submit ordering (assumption 7)

`Stream::EvictSlots` (`stream.cpp:157-181`) records `voxels → evictStaging`
copies in **its own encoder**, submits it, and only then does `FillSlots`
overwrite the same `voxels` slots with `WriteBuffer`. The comment at
`stream.cpp:172-174` states the guarantee explicitly: queue order makes the copy
read the leaving plane's data even though the map completes ticks later.

Vulkan preserves this, but **the operative mechanism changes**, and it is worth
being precise because the code comment's reasoning ("queue order") is no longer
the whole story:

1. **The guarantee is no longer "the fill's submit comes after the eviction's
   submit".** Under §4.1's pending-upload queue, `FillSlots`' `voxels` writes
   are *deferred* — they do not belong to a submit of their own, and when every
   slot hits the store there is **no `FillSlots` submit at all**
   (`stream.cpp:261-278` is guarded by `if (!genSlots.empty())`). The writes
   drain into whatever command buffer is recorded next, usually the next tick's.
2. **What actually preserves the ordering is that the eviction submit is issued
   during `EvictSlots`, before `FillSlots` runs at all, and the pending-upload
   queue never reorders.** `Stream::Update` calls `EvictSlots` (which submits
   immediately, `stream.cpp:174`) and *then* `FillSlots` (which only enqueues).
   So the copy-out is already on the queue before the overwrite is even
   enqueued, let alone recorded. Submission order still does the work — it just
   orders "the eviction submit" against "some later submit that happens to carry
   the fill", rather than against a fill submit of its own.
3. **§3.4's head-of-command-buffer global barrier supplies the memory half of
   the dependency.** Submission order gives execution ordering between submits; it
   does not make the eviction copy's `voxels` read complete-before the later
   transfer write is visible. The command buffer that drains the fill opens with
   `ALL_COMMANDS/MEMORY_WRITE → ALL_COMMANDS/MEMORY_READ|WRITE`, which orders
   its transfer writes after every prior submit's accesses. This is exactly the
   class of cross-submit hazard §3.4 exists to make un-reasonable-about.
4. **Therefore the `stream.cpp:172-174` comment should be updated when this
   lands** — its claim ("submit BEFORE FillSlots writes: queue order makes the
   copy read the leaving plane's data") stays true, but the reason becomes
   "EvictSlots submits eagerly while FillSlots only enqueues", not "both are
   submits and submits are ordered". Same commit, per the CLAUDE.md rule about
   docs that contradict code. **[AS BUILT, phase 3c] Done** — the comment now
   states the mechanism, and notes that the memory half comes from §3.4's head
   barrier on Vulkan and is automatic under Dawn. `kPersistMask` moved from a
   stream.cpp file-static to `stream.h` in the same commit: the cross-backend
   smoke has to reproduce the store round-trip exactly, and a second copy of the
   literal is a "two places that must agree" bug — it had already produced one
   false divergence that read like a barrier race.
5. **`CompleteOldest` becomes a fence wait.** Each `PendingEvict` carries the
   `VkFence` of its own submit. `CompleteOldest` does
   `vkWaitForFences(fence, UINT64_MAX)` — a genuine block, exactly as
   `instance.WaitAny(p.future, UINT64_MAX)` blocks today (`stream.cpp:201`) —
   then reads the persistently-mapped staging buffer, RLE-encodes, and returns
   the buffer to `stagingPool_`. The `kMaxPendingEvicts = 4` ring and the
   "ring full: recycle the oldest" path in `AcquireStaging`
   (`stream.cpp:184-195`) are unchanged in shape.
6. **`FillSlots`'s `while (pendingChunks_.count(...)) CompleteOldest()` loop**
   (`stream.cpp:243-244`) is the same blocking wait and needs no change beyond
   (3). Its purpose — a chunk whose eviction is still in flight must be
   completed before the store is queried — is a CPU-side data dependency, not a
   GPU one.

### 4.4 (d) The `page_` flip safety argument (assumption 12)

`sim.FlipPage()` runs on the CPU immediately after `queue.Submit`
(`support.cpp:170-171`) with no GPU synchronization. Tick N's command buffer was
recorded with `page = P`; the CPU then sets `page = 1-P` and records tick N+1
against the flipped bind groups, possibly while tick N is still executing.

**Why this is safe, precisely:** the flip changes only which *pre-created*
descriptor set the next recording references. It does not mutate any descriptor
set (`simBG_[0]` and `simBG_[1]` are both created at init and neither is
rewritten), and it does not mutate any buffer. Tick N's command buffer holds a
reference to `simBG_[P]`, which remains valid and unmodified. The only shared
mutable state between ticks N and N+1 is the buffer contents themselves — which
are exactly what §3.4's head-of-command-buffer global barrier orders.

Three requirements this places on the implementation, all of which are things a
Vulkan backend must do anyway and none of which Dawn made visible:

- **Descriptor sets must not be updated while a command buffer referencing them
  is in flight.** They are not: `simBG_[0/1]`, `simSlimBG_[0/1]`,
  `particleBG_[0/1]`, `farBG_`, `renderBG_` are all built once at
  `Simulation::Init` / pipeline rebuild. A hot-reload (`R`, `F5`) rebuilds
  pipelines and descriptor sets — that path must `vkDeviceWaitIdle` first
  (§4.9), which it already does in spirit via `WaitIdle` in the reload path.
- **Command buffers must not be reset while in flight.** With one command pool
  per frame-in-flight slot and a fence gate, this is standard. v1 uses **N=3
  command-buffer slots** matching the readback ring depth, gated by their
  fences. That is not a determinism requirement; it is a lifetime requirement.
- **`particleCounts[1-page]` is zeroed by a CPU upload** in `SubmitTick`
  (`support.cpp:136-140`), recorded at tick N+1's submit head. That upload
  targets the page tick N was *writing* — safe only because tick N's command
  buffer is fully ordered ahead of tick N+1's, which is submission order plus
  the head barrier. **[NEW EDGE]** — the pass map lists this write under §5a but
  does not call out that its target page is the one the previous tick wrote,
  which makes it the sharpest cross-submit WAW in the CPU-side traffic.

### 4.5 (e) `drawArgs` across submits (assumption 15)

`drawArgs` is written by T45 in a tick submit and read by R2's
`vkCmdDrawIndirect` in a *later* frame submit. Two facts sharpen this beyond
what the pass map says:

- The frame loop runs **up to 4 ticks per frame** (`main.cpp:1723`,
  `ticksThisFrame < 4`), so up to 4 tick submits write `drawArgs` before one
  frame submit reads it. The renderer sees the last one. That is the intended
  behavior (draw the newest particle set) and it is deterministic in the sense
  that matters — `drawArgs` is render-only.
- On ticks where `particlesActive` is false, T45 does not run and `drawArgs`
  retains whatever the last active tick wrote. That is why `EncodeWorldgen` and
  `EncodeLoadReset` explicitly clear it ("no ghost particles",
  `simulation.cpp:558`, `:594`).

Mechanism: nothing beyond §3.4. The frame command buffer's head global barrier
orders every prior submit's writes ahead of its `INDIRECT_COMMAND_READ`. Within
the frame command buffer, R0's uploads do not touch `drawArgs`, so no per-buffer
barrier is emitted for it — which is correct, and is the case a hand-written
implementation is most likely to miss precisely *because* nothing in the frame
recording mentions `drawArgs` as a write.

An `indirectCount` of garbage is not a wrong-pixels bug; `vkCmdDrawIndirect`
with an out-of-range instance count is undefined behavior and can hang a device.
This edge is safety-relevant, not just visually relevant.

### 4.6 (f) The `mbInstBuf_` hoist (assumption 6)

`Simulation::DrawMicroBodies` calls `queue.WriteBuffer(mbInstBuf_, ...)` at
`simulation.cpp:987`, with the render pass open. In Vulkan, `vkCmdUpdateBuffer`
and `vkCmdCopyBuffer` are **forbidden inside a render pass instance / dynamic
rendering scope**. This must be hoisted regardless of backend.

The fix, which `PLAN_vulkan_port.md` phase 2 already schedules under Dawn:

1. `BodyRegistry::BuildMicroInsts(microInsts)` already runs before
   `BeginRenderPass` (`main.cpp:2815` region) — the data is available.
2. Move the upload to the R0 row: a Class A `vkCmdUpdateBuffer` at the frame
   command buffer's head, alongside `bodyXforms`/`bodyInstances`/`sprites`.
3. `DrawMicroBodies` loses its `queue` parameter and becomes a pure draw. Change
   its signature so the old call shape does not compile — a silent behavioral
   move here would be invisible under Dawn.

The R0→R4 hazard (`TRANSFER_WRITE → VERTEX_SHADER|FRAGMENT_SHADER
SHADER_STORAGE_READ`) is then an ordinary table-derived barrier.

### 4.7 (g) The streaming submit that overwrites `tickUBO` (assumption 8)

`Stream::FillSlots` writes its own `TickParams` into the shared `tickUBO`
(`stream.cpp:268-273`) and submits `worldgenList` — mid-frame, between ticks.
The next `SubmitTick` overwrites `tickUBO` with the real tick's params before
its own submit. Correct today purely by queue order plus WebGPU's
write-at-submit-head semantics.

Under §4.1's pending-upload queue this is preserved, in both of its two cases:

- **`genSlots` non-empty (a genList submit happens).** The streaming `tickUBO`
  write was enqueued at `stream.cpp:268-273`, immediately before the submit at
  `:274-277`, so it drains at the head of *that* command buffer and
  `worldgen:list` reads the streaming params. The next `SubmitTick` enqueues its
  own `tickUBO` write, which drains into the tick's command buffer. Each
  dispatch reads the params written for it.
- **`genSlots` empty (no streaming submit at all).** No `tickUBO` write was ever
  enqueued — `stream.cpp:268-273` is inside the `if (!genSlots.empty())` block —
  so there is nothing to order. This is the case that a "record uploads at the
  head of this path's submit" model gets wrong by having nowhere to put the
  writes; the queue model has no such case because the enqueue and the submit
  are independent.

The cross-submit WAW on `tickUBO` (streaming's write vs. the tick's) is ordered
by issue order within the queue plus §3.4's head global barrier across submits.

**JUDGMENT / recommendation, not required for the port:** give streaming its own
`streamTickUBO` rather than borrowing the shared one. It removes a cross-submit
WAW on a uniform buffer entirely, costs 64 bytes, and makes the streaming path's
independence from the tick path structural instead of temporal. This is a
behavior-preserving change worth doing in phase 2 (it must be hash-neutral —
`worldgenList` and the tick read disjoint fields today only by convention), but
it is *not* a correctness requirement for v1 and should not be bundled into the
Vulkan landing.

### 4.8 (h) Zero-init policy (assumption 14)

WebGPU guarantees zero-initialized buffers; Vulkan guarantees nothing.

**Policy, stated as a mechanism rather than a list:** buffer creation goes
through one function, and that function does three things unconditionally —
adds `VK_BUFFER_USAGE_TRANSFER_DST_BIT` to the requested usage, records the
buffer in a registry, and (at the end of `World::Init` / `Simulation::Init`)
issues `vkCmdFillBuffer(buf, 0, VK_WHOLE_SIZE, 0)` for **every buffer in the
registry**, in one command buffer submitted before anything else runs.

```c++
// gpu/resources.cpp — the ONLY buffer constructor.
Buffer CreateBuffer(Device&, uint64_t size, Usage usage, const char* label) {
  usage |= Usage::TransferDst;               // unconditional: zero-init needs it
  Buffer b = /* vkCreateBuffer + VMA alloc */;
  g_allBuffers.push_back(b);                 // registry, for ZeroInitAll()
  return b;
}
void ZeroInitAll();   // vkCmdFillBuffer over g_allBuffers, one submit, once
static_assert(/* every Buffer member is constructed via CreateBuffer */);
```

**The enumeration below is evidence that the mechanism is necessary, not a
worklist.** An earlier draft of this document said "do not attempt to enumerate"
and then enumerated four buffers — and missed two, one of them the worst case.
That is the self-refuting shape this mechanism exists to avoid: any
hand-maintained list of "buffers that need zeroing" will be wrong, including one
written by someone who just finished explaining why it would be.

The buffers created today **without** `CopyDst`, i.e. the ones whose usage flags
must change and which no host code can currently write
(`world.cpp:27-80`, all five verified by grep):

| Buffer | Line | Why zero matters |
|---|---|---|
| `pArgsStage` | `world.cpp:50` (`Storage\|CopySrc`) | **The worst miss.** `pArgs[0..3]` are the indirect *draw* args, copied to `drawArgs` by T45. On the first tick before `args2` has ever run, garbage here becomes a `vkCmdDrawIndirect` instance count — the device-hang hazard §4.5 flags. |
| `farVox` | `world.cpp:75-76` (`Storage\|CopySrc`) | `world.cpp:71-74` states it: zero = air, so unfilled cascade regions render as sky rather than garbage terrain. |
| `farOcc` | `world.cpp:77-78` (`Storage` only) | Same, and it is the buffer *nothing in the codebase ever writes from the host*, so it is the one a manual audit skips. |
| `dirtyList` | `world.cpp:31` (`Storage` only) | Indices consumed by 54 CA dispatches; garbage indices address arbitrary chunks. Bounded by `argsStage`'s count in practice — an "unread garbage" argument, i.e. the kind not worth making. |
| `particles[0/1]` | `world.cpp:45-46` (`Storage` only) | 8 MiB each; read only up to `particleCounts`. Same argument, same reason not to rely on it. |

Buffers that *are* cleared today, but only on the `EncodeWorldgen` /
`EncodeLoadReset` paths — so a path that reaches the tick without one of those
(`--shot-mob`, a future headless harness) inherits garbage: `hash`, `claim`,
`support`, `particleCounts`, `drawArgs`, `occupancy`, `dirty[0/1]`.

`TRANSFER_DST` is free on device-local memory and is the price of the policy.

**A determinism note that matters more than the fill itself:** the world hash
covers `voxels` bits 0..15 and 24..30 only, so garbage in the tick-stamp bits
(16..23) of never-written voxels would not show up in the hash — but *would*
change which voxels act on which substep, which changes the hash next tick. A
partial zero-init policy could therefore produce a divergence that the
determinism gate catches one tick late and attributes to the wrong change. Fill
everything.

### 4.9 (i) `WaitIdle` / `OnSubmittedWorkDone` (assumption 11)

`GpuContext::WaitIdle` is used by `--shot`, save/load, hot-reload and the
selftests. Replacement: `vkDeviceWaitIdle` (or `vkQueueWaitIdle` on the single
queue — equivalent in v1; prefer `vkQueueWaitIdle` so a later async queue does
not silently widen the drain).

**Every drain must `FlushUploads()` first** (§4.1). A path that enqueues uploads
and then drains without recording a command buffer would otherwise wait for work
that does not include its own writes — and then destroy or read the buffers
those writes were meant to fill.

The call sites that matter:

- **Shader/pipeline hot-reload** (`R`, `F5`): must drain before destroying
  pipelines and descriptor sets. This is now a hard requirement rather than a
  convenience — Dawn refcounted objects into flight, Vulkan does not.
- **Save/load** (`worldio.cpp`): `LoadWorld` replaces the whole world; drain
  first, then `EncodeLoadReset`, then drain again before the readback.
- **Shutdown** (`main.cpp:2843`): drain before destroying anything. Extend to
  the staging rings, command pools, fences, and the readback slots.
- **`--shot`, three distinct drains** (`RunShots`, `main.cpp:132-152`):
  1. `ctx.WaitIdle()` after `SubmitWorldgen` (`main.cpp:134`) — before the
     far-fill loop reads the world.
  2. **The far-fill loop itself** (`main.cpp:139-147`) — this is not a drain but
     it is the §2.5.6 path, and it is the reason every submit needs a fence
     (§4.2): it submits in a tight loop with no readback, and its Class B
     staging (if `farList` ever exceeds Class A) would leak without one. It also
     needs `FlushUploads` semantics per iteration, which the queue model gives
     it for free since each iteration records a command buffer.
  3. `ctx.WaitIdle()` after the 120 settling ticks (`main.cpp:151`) — before the
     offscreen render and blocking screenshot readback in `grab()`
     (`main.cpp:160-189`).
- **`--shot-mob`**: same shape as (3).

### 4.10 Assumption 13: WebGPU-only constraints

`sim_step.wgsl` omits binding 13 so `argsStage` stays outside the CA pass's
usage scope, and `simSlimBGL_` exists because of Dawn's 16-storage-buffers-per-
stage layout limit. Vulkan lifts both. **v1 changes neither.** Removing the
staging copies (making `dispatchArgs` a `STORAGE|INDIRECT` buffer written
directly by `compact`) is legal in Vulkan and would eliminate two copies per
tick, but it changes the barrier chain shape: `compact`'s storage write would
need `COMPUTE/SHADER_STORAGE_WRITE → DRAW_INDIRECT/INDIRECT_COMMAND_READ`
instead of the transfer hop, and the compaction pass and the CA loop would share
a buffer with different access types. That is a separate, hash-gated change per
the decision log in `PLAN_vulkan_port.md`. Same for collapsing `simSlimBGL_`.

---

## 5. Queue architecture

### 5.1 v1: one queue, hard rule

**v1 uses exactly one `VkQueue`, from a queue family with
`VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT`.
Everything — tick submits, streaming submits, eviction copies, frame submits,
uploads, readbacks — is submitted to it, in exactly the order the code submits
today. This is a hard rule for the port, not a default.**

The reason is rule 1. Today's ordering guarantees come from "one queue, submits
serialize". Every argument in §4 — eviction before fill, page flip after submit,
`drawArgs` from the last tick submit, `tickUBO` per-submit — is an argument
about *submission order on one queue*. Introducing a second queue makes all of
them arguments about semaphores instead, simultaneously with introducing
hand-written barriers, on a codebase whose determinism is verified on one
vendor. Two variables at once is how a race gets shipped.

Concretely this means:
- No `VkSemaphore` between tick and frame. Only the swapchain's acquire/present
  semaphores, which are unrelated to the buffer graph.
- No queue-family ownership transfers anywhere, so `VkBufferMemoryBarrier2`
  always has `srcQueueFamilyIndex = dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED`.
  (This is the correct value for a same-queue barrier and the reason §3's
  algorithm never mentions ownership.)
- Present is `vkQueuePresentKHR` on the same queue after the frame submit.

If the machine exposes no combined graphics+compute family (it will; every
desktop vendor does), the port stops and the situation is designed for
separately.

### 5.2 Forward look: where async queues would slot in — OUT OF SCOPE

Recorded so a later phase does not have to re-derive it. **None of this is part
of the port.**

| Candidate | Why it is a candidate | What it would need |
|---|---|---|
| Readback copies (T70–T78) | ~1.5 MiB of pure transfer per tick, no dependency on anything after them | A transfer queue; a semaphore from the tick submit; **queue-family ownership transfer** on `voxels`, `occupancy`, `hash`, `pick`, `particleCounts`, `support` (release barrier on the compute queue, acquire barrier on the transfer queue, matching src/dst family indices). `support`'s clear (T77) would have to stay on the compute queue or move with it. |
| Eviction copies (`stream.cpp`) | Already its own submit, already latency-tolerant | Same ownership dance on `voxels`, plus the WAR against `FillSlots`' uploads becomes a semaphore rather than submission order. |
| Far-field fill (T60) + `farDown` (T5A) | Render-only derived data, excluded from the hash — the *only* GPU work in the tick whose result cannot affect determinism | An async compute queue, ownership transfer on `farVox`/`farOcc`, and a semaphore into the frame submit (the raymarch reads them). The one place where a wrong barrier cannot desync the sim, which makes it the right first experiment. |
| Uploads (Class B staging copies) | Transfer-only | A transfer queue and ownership transfers on every uploaded buffer — high ceremony, low payoff, since the copies are small. Least attractive. |

The general shape: an async queue needs a **release** `VkBufferMemoryBarrier2`
on the source queue (`srcQueueFamilyIndex = src, dstQueueFamilyIndex = dst`,
`dstStageMask/dstAccessMask` ignored) and a **matching acquire** barrier on the
destination queue, with a semaphore between them. Getting the pair mismatched is
undefined behavior that typically manifests as corrupted data on one vendor
only — the exact failure class this port is trying to avoid. Phase 8, one queue
at a time, each hash-gated.

---

## 6. Mechanical validation

Two mechanisms, both implementable now. The first keeps the table honest against
the shaders; the second keeps the barriers honest against physics.

### 6.1 `scripts/check_pass_table.py` — the table vs. the WGSL

**Status: IMPLEMENTED (phase 2b), and it passes clean on the tree.** What
follows is the design; where the implementation diverged, it is marked
**[AS BUILT]** inline. Run it with `--selfcheck` to prove the walk itself still
behaves — that mode asserts the seven regression cases below are silent
independently of whatever the table currently says, so a walk that regresses to
module-scope is caught even if the table happens to agree with it.

Shaped after `scripts/check_invariants.py` (same output conventions, same
exit-code contract: 0 = agree, 1 = real mismatch; accepts an optional edited-file
argument so the PostToolUse hook can run only the relevant half).

**What it parses.**

1. **The table.** `src/sim/pass_table.cpp` (or `.def`, matching the
   `tuning_params.def` precedent) — one row per line in a macro form the C++
   expands and the script regexes:

   ```
   PASS(ca,          step_,     ComputeIndirect, dispatchArgs, 54, cAlways,
        R(materials) R(tickUBO) R(passUBO) R(reactions) R(dirtyList) I(dispatchArgs),
        RW(voxels) A(dirtyOut) A(support))
   ```

   The `.def` is the single source; the recorder expands it, the script scrapes
   it. Same play as `tuning_params.def` → `TuningWgslBlock()` →
   `tuning_prelude.py`, and for the same reason: two lists that must agree are a
   silent bug, one list plus a generator is not.

   **[AS BUILT]** The row form gained three things this sketch does not have,
   each because the recorder needs it to reproduce today's command buffer:

   - a **`passGroup`** string (2nd argument) naming the `ComputePassEncoder` the
     row is recorded into. Consecutive compute rows sharing it go into one pass;
     `nullptr` on Fill/Copy rows, which are encoder-level and close any open
     pass. The splits are stated rather than inferred from adjacency because
     "which rows share a pass" is precisely the thing phase 2b must not change.
   - a **`table`** selector (`PT_TICK`, `PT_WORLDGEN`, …) rather than six
     separate arrays, so §2.5's six non-tick tables and the tick table are one
     list that the checker walks once.
   - **dispatch-extent selectors** (`D_OPS`, `D_EXP`, `D_CHUNKS`, …) resolved
     from the tick's counts at record time, since a row's extents are a function
     of `opsCount`/`expCount`/etc. and cannot be literals.

   Reads and writes are one `USES(...)` list rather than two columns, tagged per
   entry (`R W RW A U I TR TW`); splitting them was redundant with the tag.
   `pass_table.cpp` expands the `.def` **twice** — once for the rows, once to
   count each row's uses — because a braced initializer cannot report its own
   length and a padding entry is indistinguishable from a real `R(voxels)`.

2. **The WGSL — a call-graph walk ROOTED AT THE ENTRY POINT.**

   This is the part that must be specified precisely, because the obvious
   implementation is wrong in a way that makes the checker useless. A WGSL file
   declares its storage bindings at **module scope**, shared by every entry
   point in the file. The set of bindings *declared in the file* is therefore
   much larger than the set *any one entry point touches*, and a checker that
   compares the table against module-scope declarations will disagree with a
   correct table on most rows in this codebase.

   **Algorithm.** For each row's `(wgsl file, entry point)`:

   a. Parse all module-scope `@group(G) @binding(B) var<storage, ACCESS> NAME`
      and `var<uniform> NAME` declarations into a *candidate* set. Ignore
      `var<workgroup>`. **This set is not the answer** — it is only the
      vocabulary of names to look for.
   b. Collect every `fn NAME(...) { ... }` body in the file (plus
      `common.wgsl`, which `LoadShader` prepends).
   c. Build a callee map: function F calls G if identifier `G` appears in F's
      body followed by `(`.
   d. **BFS from the entry point only.** Union the candidate names referenced in
      the bodies of the entry point and every function transitively reachable
      from it.
   e. A candidate name **not** reached in (d) is **excluded** — it is declared
      at module scope for a *different* entry point in the same file, and it is
      correct for the table to omit it.

   Step (e) is the fix. An earlier draft said the walk should "resolve ambiguity
   by including" on the grounds that a false inclusion only costs a spurious
   barrier. That is true of genuine *ambiguity* (an indirect reference the walk
   cannot resolve), and the rule is kept for that case — but it must not be
   applied to names that are simply unreachable, because then the walk always
   returns the whole module and every check below degenerates.

   **Regression cases the checker must pass.** These are real, in-tree, and each
   one breaks a module-scope implementation:

   | File | Entry | Declared at module scope | Actually touched |
   |---|---|---|---|
   | `worldgen.wgsl` | `far` | `voxels`, `dirtyIn`, `dirtyOut`, `occupancy`, `genList` (`:31-37`) + the group-1 far bindings (`:2649-2653`) | **none of the group-0 storage buffers** — only `materials`, `T`, `farList`, `farUBO`, `farVox`, `farOcc` |
   | `sim_particle.wgsl` | `args1`, `args2` | `voxels`, `dirtyOut`, `pRead`, `pWrite`, `claim`, `spawnOps` (`:12-22`) | only `counts`, `pArgs`, `T` |
   | `sim_compact.wgsl` | `main` | `dirtyIn` **and** `dirtyOut` (`:13-14`) | only `dirtyIn` (`:27`) |
   | `sim_compact.wgsl` | `mainNext` | same | only `dirtyOut` (`:44`) |
   | `sim_step.wgsl` | `main` | — | `dirtyOut` **only via `markDirty`** (`:65`), which (b)–(d) must find |
   | `worldgen.wgsl` | `list` | — | `dirtyIn`/`dirtyOut`/`occupancy`/`voxels` **only via `genChunk`** (`:2574-2612`) |

   The last two are the reason the walk must be transitive rather than
   body-local; the first four are the reason it must be rooted rather than
   module-scope. A checker that fails any of these produces WARN spam on
   correct rows, which trains implementers to ignore it — at which point the
   checker is worse than not having one, because it launders a false sense of
   coverage.

   **[AS BUILT]** These are encoded as `REGRESSIONS` in the script and asserted
   by `--selfcheck`, which checks them against the *walk* rather than against
   the table — so a walk that regresses to module-scope fails even if the table
   has been edited to agree with the broken walk. The list is **seven** rows,
   not six: the table above counts `sim_particle`'s `args1`/`args2` as one row
   because they have identical binding sets, but they are separate entry points
   and are asserted separately. Each case states both what the walk must
   EXCLUDE (module-scope bindings belonging to another entry point) and what it
   must INCLUDE (bindings reached only transitively), so neither half can
   regress silently.

   f. Read vs. write per name: `NAME[...] =`, `atomicStore/Add/Max/Min/And/Or/
      Xor/Exchange/CompareExchangeWeak(&NAME[...])` ⇒ write; any other
      appearance ⇒ read. `atomicLoad` ⇒ read only.

**What it asserts.**

All "the entry point" references below mean the **rooted-walk** result from
step (d), never the module-scope candidate set.

| Check | Severity |
|---|---|
| Every buffer the rooted walk says the entry point **reads** is in the row's read set (or write set, for RW) | **FAIL** — a missing read means a missing RAW barrier |
| Every buffer the rooted walk says the entry point **writes** is in the row's write set | **FAIL** — a missing write means every later reader is unsynchronized |
| Every buffer in the row's sets is reached by the rooted walk | WARN — spurious barriers only. **Must be silent on the six regression cases above**; if it is not, the walk is module-scope and the checker is broken |
| A row declaring `StorageRead` on a binding the WGSL declares `read_write`, where the rooted walk finds no write **and another entry point in the same module does write it** | FAIL without a `// read-only in practice: <reason>` comment on the row. **[AS BUILT]** the emphasised clause is a narrowing the spec did not have, and it is load-bearing: WGSL has no per-entry-point access modes, so `voxels` is declared `read_write` in *every* sim shader including the ones that only read it. Without the clause the rule fires on 11 rows, 9 of them unremarkable — the WARN spam this section elsewhere says makes a checker worse than none. With it, it fires on exactly the shape worth explaining: a row whose `R()` looks like an oversight against its own file. Five live cases, not two: `dirtyList` in `sim_step` and `sim_occupancy:mainDirty` as the spec predicted, plus `voxels` in `sim_explode:mark` (the pure-read half of the mark/apply split), `expMask` in `sim_explode:apply`, `voxels` in `sim_particle:integrate`, and `voxels` in `worldgen:fardown` |
| Every entry point in every `sim_*.wgsl` / `worldgen.wgsl` appears in at least one table row | **FAIL** — an unreferenced kernel is either dead or an untabled pass |
| Every buffer id in the table exists as a `World`/`Simulation` member | **FAIL** |
| The bind-group layout the row names contains every binding the entry point uses | **FAIL** — catches a kernel added to a pass whose layout cannot bind it. **Note the direction: layout ⊇ used, never layout = used.** `genList` binds `simBGL_`'s 17 bindings and uses 7 (§2.5.2); requiring equality would fail every correct row |
| No `useGlobalBarrier` row declares an `IndirectRead` of a buffer written anywhere inside its own repeat span | **FAIL** — this is the §3.6 point-1 assumption made checkable: form (B)'s access mask does not cover `INDIRECT_COMMAND_READ`, and T30's soundness depends on nothing writing `dispatchArgs` between iterations. **[AS BUILT]** implemented against `repeat > 1` rather than against a `useGlobalBarrier` flag, because that flag is a phase-3 construct and does not exist yet. The two coincide: `repeat > 1` is true of T30 alone, and §3.6 sanctions the global barrier for exactly the repeat span. When phase 3 adds the flag, tighten this to the flag |
| Exactly the rows marked `useGlobalBarrier` are the ones §3.6 sanctions | **FAIL**. **[AS BUILT]** deferred to phase 3 with the flag itself — there is nothing to check while the only marked row is implied by `repeat > 1` |
| `DirtyIn` and `DirtyOut` resolve to different buffer ids for every page value | **FAIL** — §4.1's wake-vs-T00 edge. **[AS BUILT]** checked structurally against `Simulation::PassBuffer`: the two cases must resolve to different expressions and both must mention `page_`. That is stronger than testing two page values, since it rejects a resolution that stops being symbolic at all |
| Every Class A buffer's declared capacity is ≤ 65536 | **FAIL** (also a C++ `static_assert`, §4.1). **[AS BUILT]** not implemented — Class A/B is the phase-3 upload path (§4.1) and no code declares a class yet. Lands with that path, where the `static_assert` is the primary guard anyway |
| The pipeline **layout** the row's kernel is built against can bind every binding the rooted walk finds | **FAIL** — **[AS BUILT]** the layout is scraped from the `MakeComputePipeline` call in `BuildPipelines` rather than declared on the row, so the row cannot lie about it. Direction is layout ⊇ used, as specified |

**What it cannot check, stated so nobody assumes it does:** it validates the R/W
*sets*, not the *order*. A table whose rows are in the wrong order passes this
check and produces a wrong world. That is what §6.2 is for.

**Where it runs:** added to `scripts/post_edit_check.sh` alongside
`check_invariants.py`, triggered on `assets/shaders/*.wgsl` and
`src/sim/pass_table.def`. Under phase 2 (RHI seam under Dawn) this checker is
already meaningful — the table exists and drives `EncodeTick` before Vulkan does
— which is the point of doing the restructure under Dawn first.
**[AS BUILT]** it also triggers on `src/sim/simulation.cpp`, because the
pipeline → `(shader, entry)` mapping and the `DirtyIn`/`DirtyOut` symbolic
resolution are both scraped from there; an edit to either can break the pair
without touching the `.def`. CLAUDE.md's invariant list carries the standing
obligation.

**It earned its keep on day one.** Building it against the live tree turned up
two over-declared rows in this document's own §2.4 (T11's `materials`, T42's
`dirtyOut` — both now corrected there), which is a useful calibration of how
much a hand-authored table drifts: the tick table was written carefully, reviewed
adversarially, and still had two wrong cells out of ~50 rows.

### 6.2 The sledgehammer oracle — barriers vs. physics

A debug-build option, `--barriers=sledgehammer` (default `precise`), that
changes exactly one thing in the recorder:

```c++
// In debug builds only. Replaces every generated barrier, and inserts one
// unconditionally between every pair of consecutive rows regardless of hazard.
static const VkMemoryBarrier2 kSledgehammer = {
  .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
  .srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
  .dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
};
```

Recorded before **every** row — dispatch, copy and fill alike — and before the
first row of every command buffer. This is the maximally-ordered execution of
the same total order: every hazard, real or imagined, is covered.

**The oracle:** run the determinism gate under both settings and compare the
world-hash *sequence*, not just the final hash.

```bash
./build/Release/sandvox.exe --selftest --gate determinism --backend vulkan \
    --barriers=precise     --json precise.json
./build/Release/sandvox.exe --selftest --gate determinism --backend vulkan \
    --barriers=sledgehammer --json sledge.json
python scripts/diff_hashes.py precise.json sledge.json
```

**What the oracle is actually good for — stated before the interpretation
table, because an earlier draft of this section oversold it and an oversold test
is worse than no test.**

The oracle is **weak at detecting a missing barrier and strong at exonerating
the barrier graph**. Both halves of that follow from the same fact: on this
hardware, a compute→compute barrier on a 512 MiB device-local buffer is already
a full cache flush, and back-to-back indirect dispatches of identical shape
rarely overlap in the first place. So a *missing* barrier is expected to produce
**identical hashes in both modes** — the hardware happens to serialize anyway.
The oracle is nearly blind to precisely the failure this port fears.

It also cannot distinguish two different mistakes: an absent barrier and a
barrier whose access masks are too narrow. On desktop drivers an execution
dependency alone usually suffices to make the data visible, so both mistakes
produce the same (passing) result.

Interpretation:

| Result | Meaning |
|---|---|
| Sequences identical | **Weak evidence.** It does not mean the barriers are right; it means this GPU serialized regardless. Do not report this as "barriers verified". |
| Sequences differ | **A barrier is definitively wrong.** Rare, but unambiguous. The first differing tick localizes it; bisect by making rows sledgehammer one phase at a time (`--barriers=precise:except=ca`, etc.). |
| Sledgehammer differs from **Dawn's** hash sequence | **The most valuable outcome.** Sledgehammer is maximally ordered, so barriers cannot be the cause — the bug is zero-init, upload ordering, table row order, or a shader translation difference. This *exonerates* the barrier graph and cuts the suspect list down, which is the oracle's real job. |

**Sync validation, not the oracle, is what detects a missing barrier.**
`VK_LAYER_KHRONOS_validation` with
`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` reports a hazard
from the *recorded commands*, without needing a divergence to occur — it does
not care whether the hardware happened to serialize. It is the primary detector
and it is assumed on for every debug run.

**[AS BUILT, phase 3b] It is now actually available.** The layer did not
enumerate on this machine when phases 1–3a were written (no Vulkan SDK, by
design), which made this paragraph aspirational and made a malformed descriptor
fault inside the ICD instead of erroring. The LunarG SDK was installed during
phase 3b, and `--vk-smoke --vk-validation` runs the full worldgen + 50-tick
comparison with synchronization validation on and reports **zero** messages.
One caution the SDK introduced: it registers eight explicit layers (api_dump,
gfxreconstruct, synchronization2, monitor, screenshot, profiles, shader_object,
validation), so instance creation must request `khronos_validation` **by exact
name** and never enable whatever enumerates — an API dumper or a capture layer
silently in the path of a determinism run is its own hazard. Its known gaps are around indirect
args and cross-submit hazards, i.e. exactly §4.5 and §4.3, so it is not
sufficient alone either.

The layered position, in order of detection power for a missing barrier:

1. **Sync validation** — detects the hazard directly. Primary.
2. **Cross-backend hash equality vs. Dawn** (§6.3) — Dawn's auto-barriers are
   the reference implementation of this graph. Strong, but only over the edges
   the scenario exercises.
3. **The sledgehammer A/B** — exoneration first, detection a distant second.

**N=5 reruns do not help.** An earlier draft suggested running each mode five
times to catch a 1-in-20 race. Scheduling for identical dispatch shapes on the
same hardware is near-deterministic, so repeated runs sample the same
interleaving rather than exploring the space. Spend the time on a hostile
scenario instead.

**Do run the hostile scenario.** A settled world exercises almost none of the
graph. The scenario that matters puts `voxels`, `expMask`, `particles`, `claim`
and the streaming submits in flight together: explosions at the
residency-window edge, heavy particle traffic, and streaming shifts on the same
ticks, with hash ticks (`tick % 15 == 0`) landing inside the burst so both
phase-5 branches are taken.

### 6.3 The cross-backend gate that already exists

`PLAN_vulkan_port.md` phase 3 checkpoint 2 (Dawn vs. Vulkan hash-sequence
equality) is the strongest single test of this document, because Dawn's
auto-generated barriers are the reference implementation of the graph. Any edge
this document gets wrong shows up there — provided the scenario exercises it,
which is what §6.2's hostile-scenario note is about.

---

## 7. Risk register

Ranked by (probability of being gotten wrong) × (cost of being wrong). "Test"
names the specific thing that would catch it, not "the selftest".

### 7.1 — The 54 CA iterations must never overlap (the color lattice)

**The edge.** The inter-iteration barrier in the CA loop is a **correctness
requirement of the 3×3×3 color lattice**, not a memory-visibility optimization.
`sim_step.wgsl:1-9`: within one dispatch every acting cell shares one
`colorPhase`, so acting cells are ≥3 apart on every axis while writes reach ≤1
cell, and destination writes are *provably disjoint*. That disjointness is a
property of a single color phase. `colorPhase` changes every iteration, via the
dynamic `passUBO` offset `k * kPassStride` (`simulation.cpp:682-683`). Two
iterations executing concurrently are two different colors executing
concurrently — and cells of adjacent colors are adjacent by construction, so
their writes collide and the winner is whichever workgroup arrived first.

**Why this outranks everything else here.** Every other risk in this register is
a specific barrier that might be omitted. This one is a *category* of change
that looks like optimization and is actually the removal of rule 1: batching
iterations, merging "redundant" barriers between them, splitting them across
queues, or hoisting them into a single dispatch with a loop inside. Each of
those is a natural thing to propose after reading "53 identical barriers in a
loop" and thinking about overhead — the settled-tick finding in
`PLAN_vulkan_port.md` (184.6 µs/tick of empty dispatches) actively invites it.
And the failure mode is the worst available: not a crash, not a visual artifact,
but first-come-first-served conflict resolution — which CLAUDE.md rule 1 bans by
name — producing a world that is correct-looking, correct-on-this-GPU, and
divergent on the next vendor.

**Neutralized by.** §3.6 states the requirement at the top of the section rather
than deriving it from memory visibility, so a reader optimizing the loop meets
the lattice argument before the performance argument. The barrier itself is
emitted by the tracker from T30's `repeat: 54` — the recorder has no "skip the
barrier between repeats" path, and adding one would require deliberately
special-casing the repeat span.

**Test.** Cross-vendor hash equality is the only real proof, and it is exactly
what DESIGN.md risk #3 says is still open. Short of that: the determinism gate
detects it *if* the GPU happens to overlap the dispatches — which it may not
(§6.2's honesty note applies here too, and is the reason this is a design-time
invariant rather than a test-time one). **Sync validation does not catch this
at all**: with the barrier removed, there is no unsynchronized-access hazard to
report — every iteration legitimately reads and writes `voxels`, and the layer
has no model of the color lattice. This risk is defended by the document and the
code comment, not by tooling.

### 7.2 — `pDispatchArgs` WAR: the second copy vs. the in-flight indirect fetch

**The edge.** T44 (`copy pArgsStage@16 → pDispatchArgs`) overwrites the args
that T42's `vkCmdDispatchIndirect` fetched. Without a barrier,
`TRANSFER_WRITE` can land while `DRAW_INDIRECT` is still reading, and T42
dispatches a workgroup count from `args2` instead of `args1`.

**Why dangerous.** It is a WAR, which is the case implementers skip ("nothing
reads it after the write, so who cares"). It is intermittent by nature. Its
effect — integrate running over the *write* page's count instead of the read
page's — is a wrong number of particles integrated: a real sim-state divergence,
because `resolve` writes `voxels`. And it only fires when `particlesActive`,
i.e. after an explosion, i.e. not in a settled-world test.

**Neutralized by.** §3.3's write branch folding `readStagesSince` into the
barrier's src scope. The recorder cannot skip it, because `IndirectRead`
populates `readStagesSince = DRAW_INDIRECT` and the next write to that buffer
unconditionally consults it.

**Test.** Sync validation flags `WRITE_AFTER_READ` on `pDispatchArgs` directly.
Failing that: a gate that fires an explosion and asserts
`particleCount` matches a Dawn-recorded reference sequence over 60 ticks.

### 7.3 — `argsStage` double generation and the WAW at T55

**The edge.** `argsStage` is: filled (T01) → atomically written by `compact`
(T15) → copied out (T20) → **filled again (T55)** → atomically written by
`compactNext` (T56) → copied out (T57). The T55 fill is a WAW against T15's
write *and* a WAR against T20's transfer read.

**Why dangerous.** Both hazards are on a 12-byte buffer, which is exactly the
size an implementer decides cannot possibly need a barrier. If T55's fill lands
before T20's read, `dispatchArgs` gets zero and the entire CA loop dispatches
nothing — a world that silently stops simulating, on some GPUs, sometimes.
If T55's fill races T56's `atomicAdd`, the dirty count is wrong and the
occupancy update misses chunks, corrupting `occupancy` (which is hashed
indirectly via what the renderer and streaming see, and directly affects
eviction decisions).

**Neutralized by.** `Fill` is `TransferWrite` in the table like any other write;
T55 consults `lastWrite = {COMPUTE, STORAGE_WRITE}` from T15 and
`readStagesSince = {COPY, TRANSFER_READ}` from T20, and emits one barrier
covering both. The reason this works is that the tracker is per-buffer live
state, not per-row-pair.

**Test.** A gate that runs 60 dirty ticks with continuous mutation and asserts
`activeChunks` is non-zero and the hash advances. A CA loop that dispatches
nothing produces a frozen world, which the walk test and any settling assertion
catch loudly — but only if the test *has* activity.

### 7.4 — `ClearBuffer(support)` immediately after its copy-out (T76→T77)

**The edge.** `world.cpp:159-160`: copy `support` → slot, then
`ClearBuffer(support)`. Transfer-read then transfer-write on the same buffer,
adjacent, in the same command buffer.

**Why dangerous.** Two transfer ops look independent. They are not: without a
barrier the fill can complete before or during the copy, and the CPU reads zeros
for the support-loss flags. Support flags drive structural collapse; losing them
means voxels that should have fallen do not, on the CPU side. It degrades
silently rather than crashing, and it does not affect the world hash (support is
CPU-consumed), so **the determinism gate cannot catch it**.

**Neutralized by.** §3.3 treats transfer↔transfer exactly like any other pair —
T77's `TransferWrite` sees `readStagesSince = {COPY, TRANSFER_READ}` and emits
`COPY/TRANSFER_READ → COPY/TRANSFER_WRITE`. The one thing an implementer must
not do is "batch all the readback copies and the clear into one barrier-free
block because they're all transfers".

**Test.** A dedicated gate: carve out the support beneath a structure, run one
tick, assert `Snap().supportFlags` has a non-zero entry in the affected chunk.
Currently the only coverage is indirect. Worth adding as part of phase 3.

### 7.5 — Skipped conditional rows leaving a stale "previous writer"

**The edge.** `dirtyList` is written by T15 (`compact`) and read by T30 (`ca`).
On a hash tick, T56 (`compactNext`) never runs — but T58/T5A never run either,
so nothing reads the second generation. Conversely, on a dirty tick with
`particlesActive` false, T46 (`pResolve`) does not write `voxels`, so T58's read
of `voxels` must sync against **`ca[53]`**, not against the skipped T46 and not
against T13 (`explodeApply`).

**Why dangerous.** Any implementation that computes barriers as a function of
*adjacent table indices* — a natural-looking optimization, and the shape a
precomputed static barrier list takes — produces the right answer only for the
condition pattern it was computed under. There are 2^10 condition combinations
per the §2.3 list (fewer in practice, but the point stands), and only the common
ones get tested.

**Neutralized by.** Barriers are computed **at record time against live
`BufState`**, never precomputed per table-index pair. A skipped row is simply not
visited and does not touch state, so the next visited row syncs against the last
*actual* accessor by construction. §3.9. This is stated as a hard implementation
constraint: **no static barrier list, ever.**

**Test.** A gate that walks the condition space: for each of the 10 conditions,
run 20 ticks with it forced on and forced off, comparing hash sequences against
Dawn. Cheap (the conditions are CPU-side flags) and it is the only test that
covers the combinatorics.

### 7.6 — The five buffers with no `CopyDst`, and why enumeration fails

**The edge.** Five buffers are created without `CopyDst`
(`world.cpp:31, 45-46, 50, 75-78`) and therefore have **no host writer at all**
today. Their correct initial state comes purely from WebGPU's zero-init
guarantee, which Vulkan does not provide: `dirtyList`, `particles[0]`,
`particles[1]`, `pArgsStage`, `farVox`, `farOcc`.

**Why dangerous — ranked, because they are not equally bad.**

- **`pArgsStage` (`world.cpp:50`) is the severe one.** Words `[0..3]` are the
  indirect *draw* args, copied verbatim into `drawArgs` by T45
  (`simulation.cpp:713`). Before `args2` has ever run — the first tick with
  `particlesActive`, or any frame after a `drawArgs` clear that precedes the
  first `args2` — uninitialized memory reaches `vkCmdDrawIndirect` as an
  instance count. §4.5 already flags an out-of-range indirect draw count as
  undefined behavior that can hang a device. This is that hazard, sourced from
  uninitialized memory rather than from a stale write.
- **`farOcc` (`world.cpp:77-78`) is the one an audit misses.** It gates whether
  the raymarcher descends into a far-field chunk. Garbage means marching
  cascade regions that contain nothing, or `farVox` bytes read as material ids —
  rendered terrain out of uninitialized memory. Not hashed, so **no determinism
  test catches it**: a visual bug at draw distance, on early frames, before the
  fill covers the region. It is the most likely buffer to be skipped precisely
  *because* nothing in the codebase writes it.
- `farVox`, `dirtyList`, `particles[0/1]` — real but bounded: each is read only
  up to a count that a shader wrote (`argsStage`, `particleCounts`) or rendered
  only where `farOcc` says something exists. The argument that they are safe is
  an "unread garbage" argument, which is the kind §4.8 exists to stop having to
  make.

**Why dangerous as a class.** All five need `VK_BUFFER_USAGE_TRANSFER_DST_BIT`
added *purely so they can be filled* — a usage-flag change with no other
motivation, which is exactly the kind of prerequisite that gets dropped.

**Neutralized by.** §4.8's mechanism: `CreateBuffer` adds `TRANSFER_DST`
unconditionally and registers the buffer, and `ZeroInitAll()` iterates the
registry. Not a list — an earlier draft of this very document wrote "do not
attempt to enumerate", enumerated four, and missed `pArgsStage` and `farVox`.
That is the evidence for the mechanism.

**Test.** A gate asserting that immediately after `Init`, before any worldgen,
reading back each of the five yields all zeros. Requires adding `CopySrc` to
`farOcc`, `dirtyList`, `particles[0/1]` for the test — acceptable, `farVox` and
`pArgsStage` already have it (`world.cpp:50, 75-76`), and `farVox`'s is already
documented as "selftest-only".

### 7.7 — `particleCounts[1-page]` zeroed by an upload that races the previous tick

**The edge (see §4.4, **[NEW EDGE]**).** `SubmitTick` writes zero to
`particleCounts[(1-page)*4]` (`support.cpp:136-140`). At tick N+1, `1-page` is
tick N's write page — the one `pIntegrate`/`pResolve` were `atomicAdd`ing into.
The upload is a cross-submit WAW against a *shader atomic*.

**Why dangerous.** If the zero lands early, particles integrated by tick N are
lost — an actual sim-state change, since `pResolve` writes `voxels`. It is
correct today only because submits serialize *and* because WebGPU's
write-at-submit-head puts the write after tick N's command buffer completes in
queue order. In Vulkan, "in queue order" gives execution ordering but the
availability/visibility of the shader's atomic writes to the subsequent transfer
write needs a barrier.

**Neutralized by.** §3.4's head-of-command-buffer global memory barrier, which
orders every prior submit's writes ahead of anything in this command buffer,
including the head uploads. This is the single strongest argument for §3.4 as
written — a per-buffer cross-submit tracker would have to know that
`particleCounts`' last writer was two submits ago in a *different* page role.

**Test.** Explosion gate over 60+ ticks asserting the particle count sequence
matches Dawn's tick-for-tick. A dropped generation shows as a count discontinuity.

### 7.8 — `farFill` (T60) overwrites what `farDown` (T5A) wrote, same submit

**The edge (see §7.10, **[NEW EDGE]**).** Both write `farVox`/`farOcc` with
atomics, but they are not the same *kind* of atomic write:
`fardown` does `atomicAnd` + `atomicOr` (partial-byte RMW) and
`atomicMax(&farOcc[...], 1)`; `far` does `atomicStore` of a full word and
`atomicStore` of the level-chunk count into `farOcc`
(`worldgen.wgsl:2698-2699`, `:2785-2789`). `far`'s stores **overwrite**
`fardown`'s results wholesale for any level-chunk both touch.

**Why dangerous.** The pass map records this as "WAW, both atomic", which reads
as benign. It is not: without a barrier, the interleaving of a partial-byte
RMW against a full-word store on the same word is undefined in *result*, not
just in order — `fardown` could read a word, `far` could store over it, and
`fardown`'s `atomicOr` could then resurrect a byte from the pre-store value.
Even with a barrier, the semantic layering (fill wins over downsample for
overlapping cells) is order-dependent behavior that is currently correct only
because `EncodeFarFill` is encoded after `EncodeTick`.

**Neutralized by.** §3.8's ordinary COMPUTE→COMPUTE `READ|WRITE` barrier, and by
the table preserving the T5A-before-T60 order verbatim. The design deliberately
does **not** relax the barrier on the grounds that both accesses are atomic.

**Consequence bound.** Far-field is render-only, excluded from the hash and from
persistence, so this cannot desync. It is a correctness-of-appearance bug with a
bounded blast radius — which is why it is ranked here and not higher.

**Test.** The existing phase-2 downsample gate (which reads back one `farVox`
word) extended to run a fill and a downsample targeting the same level-chunk in
one tick, asserting the fill's value wins.

### 7.9 — Two `dirtyList` generations, one buffer

**The edge.** T15 writes `dirtyList` (compacted `DirtyIn`); T30 ×54 reads it;
T56 overwrites it (compacted `DirtyOut`); T58 and T5A read the new contents.
One buffer, two meanings, within one tick.

**Why dangerous.** T56's write is a WAR against 54 iterations of reads. If it
lands early, the CA loop's tail iterations index chunks from the wrong list —
processing chunks that are dirty-out but not dirty-in. Because
`dirtyList` entries are chunk indices and both lists contain valid indices, this
does not crash: it silently simulates the wrong set of chunks, and the world
hash diverges. It is also the barrier most likely to be omitted by someone
thinking "the CA loop only reads it, so nothing to do".

**Neutralized by.** §3.3: T56's `StorageWrite` on `dirtyList` sees
`readStagesSince = COMPUTE/SHADER_STORAGE_READ` accumulated across all 54 CA
iterations and emits the WAR barrier. Note that this is *also* covered by the
global barrier the CA loop emits (§3.6) only for iterations within the loop —
the T30→T56 boundary needs its own, which the tracker provides.

**Test.** The determinism gate proper, and cross-backend hash equality. This one
does show up in the hash, immediately.

### 7.10 — Edges this document adds beyond `vulkan_pass_map.md`

Collected for the reviewer.

1. **`farFill`'s `atomicStore` overwrites `fardown`'s `atomicAnd`/`atomicOr`.**
   §7.8. The map's "WAW, both atomic" understates it; the two entry points use
   incompatible atomic idioms on the same words, and the ordering is semantic
   (fill wins), not incidental.
2. **`particleCounts[1-page]` is zeroed from the host at tick N+1 against a page
   the GPU was atomically writing at tick N.** §4.4 / §7.7. The map lists the
   write in §5a but not the cross-submit WAW it constitutes.
3. **Up to 4 tick submits occur per frame** (`main.cpp:1723`,
   `ticksThisFrame < 4`), so the `drawArgs` cross-submit hazard (map assumption
   15) is 4:1, not 1:1, and `EncodeWakeAll`'s bare `WriteBuffer` can be issued
   up to 4 times per frame with no encoder of its own.
4. **`sim_step.wgsl` and `sim_occupancy.wgsl:mainDirty` declare `dirtyList` as
   `var<storage, read_write>` but only read it.** The map's R/W table correctly
   lists it as a read; the *declaration* says otherwise, and a checker that
   trusts the qualifier would upgrade 54 barriers to WAW. §2.1 rule 2.
5. **`worldgen:genChunk` `atomicStore`s both `dirtyIn` and `dirtyOut`**
   (`worldgen.wgsl:2604-2610`), so `EncodeWorldgen`'s seven leading
   `ClearBuffer`s include two that genuinely hazard against the following
   dispatch. The map lists the fills and the dispatch but not that edge.
6. **Five buffers have no `CopyDst` usage today** — `dirtyList`,
   `particles[0/1]`, `pArgsStage`, `farVox`, `farOcc` (`world.cpp:31, 45-46,
   50, 75-78`) — so the zero-init policy requires adding `TRANSFER_DST` to their
   usage flags, an easily-missed prerequisite of §4.8. `pArgsStage` is the
   severe case (uninitialized indirect *draw* args) and `farOcc` the
   easily-skipped one (nothing in the codebase writes it from the host). §7.6.
7. **`pick` is stale in TWO independent ways, and both must be preserved.**
   §7.10a below.
8. **The `--shot` far-fill loop is a submit path with no tick in it**
   (`main.cpp:139-147`), carrying a per-iteration upload→dispatch RAW on
   `tickUBO` because `farCount` travels *inside* `tickUBO` rather than as a
   dispatch dimension. The map's §1.6 documents `EncodeFarFill` as part of the
   tick encoder and never mentions this standalone driver. §2.5.6.
9. **`Stream::FillSlots` can write four buffers and submit nothing at all** —
   the submit at `stream.cpp:274-277` is guarded by `if (!genSlots.empty())`,
   while the `voxels`/`occupancy`/`dirty[0]`/`dirty[1]` writes at `:247-260`
   happen for every store-hit slot. Any upload model phrased as "recorded at the
   head of this path's submit" has no home for them. §4.1.
10. **`worldgen:list`'s `genChunk` `atomicStore`s `dirtyIn`/`dirtyOut` from a
    mid-frame submit**, concurrent with the in-flight tick's `markDirty`/
    `markBoth` on the same two buffers — a cross-submit WAW against a shader
    atomic, structurally identical to edge 2. §2.5.2.
11. **`sim_compact`'s both entry points `atomicStore` `args[1]` and `args[2]`**
    (`sim_compact.wgsl:22-25, 40-43`), so `dispatchArgs.y/.z` come from the
    shader, not the fill. The fill→dispatch WAW is therefore load-bearing for
    dispatch *validity* — a lost barrier yields `{count, 0, 0}`, i.e. zero
    workgroups and a silently frozen world, not merely a wrong count. §7.3.

#### 7.10a — `pick`'s two staleness relationships

Both are pre-existing, both are harmless, and **both must be preserved rather
than "fixed" during the port**. They are listed together because fixing either
one in isolation is the tempting mistake.

- **Stale camera.** `pick_` runs on the *tick* path (T51/T59), but
  `WriteRenderParams` is called at `main.cpp:2569` — *after* the tick loop
  (`main.cpp:1723`) and before the render encoder (`main.cpp:2822`). So every
  pick in frame F reads the camera written at the end of frame F−1, and all
  up-to-4 picks in a frame read the same one. Moving the upload earlier changes
  what the pick returns.
- **Stale window origin.** `sim_pick.wgsl:13` bounds-tests with
  `inWindow(c, R.origin)` — the **render** params' window origin — while every
  sim kernel uses `T.origin` (`sim_step.wgsl:34`, `worldgen.wgsl` `genChunk`,
  `fardown`). After a mid-frame streaming shift, `R.origin` is the origin as of
  the last `WriteRenderParams` and `T.origin` is the current one, so the pick
  ray's residency test and the sim's disagree for the rest of the frame.

The second is the dangerous one to "fix", because it looks like an obvious bug:
a reader who notices `R.origin` in a sim-side shader will reach for `T.origin`.
Doing so while leaving the camera in `R` produces a *mixed* frame — current
origin, one-frame-old camera — which is a state that has never been tested and
is not obviously better. Both fields come from the same struct written at the
same instant; that coherence is the property worth keeping.

`pick` output is CPU-consumed UI state (a paint cursor, `world.cpp:201`), never
sim state, and `sim_pick.wgsl:1-4` already documents the readback as one tick
latent. One frame of lag on a crosshair raycast is invisible. Structurally this
is the same cross-submit-chain shape as `drawArgs` and gets the same §3.4
coverage; the map lists `renderUBO` under both `pick` and the render passes
without noting that they are different submits, that the tick's read is one
frame behind, or that the origin fields diverge.

---

## 8. What this document obliges

- **A tick-path edit that changes any kernel's bindings updates
  `src/sim/pass_table.def` in the same commit**, or `check_pass_table.py` fails.
  This is the same standing obligation `tuning_params.def` and
  `sound_schema.js` carry. *(Live since phase 2b: the table exists, drives
  recording, and the checker runs from the PostToolUse hook. The obligation also
  covers `simulation.cpp`, which the checker scrapes for the pipeline → entry
  point mapping and the symbolic page resolution.)*
- **A code path that submits a command buffer gets a table**, listed in the
  header enumeration. Until every recorded command is in a table, "barriers are
  generated from the table" means "barriers are missing wherever the table is".
  *(Phase 2b tabled the six `Simulation::Encode*` paths — §2.4 and §2.5.1–2.5.4,
  2.5.6. §2.5.5 `wakeAll` records no commands and needs none — phase 3c
  confirmed this: on Vulkan it is a `QueueWrite` draining at the head of the
  tick's command buffer, with no special case at all.)*

  **RESOLVED for the readback copies (§2.4 phase 7a/7b) and the eviction copies
  (§4.3), phase 3c — and the resolution is "a Use, not a Row".** A `pass::Row`
  encodes a Copy's offsets as the literal constants `x/y/z`. That is exact for
  every copy in the tick table, and it CANNOT express these: `EncodeReadbacks`
  issues up to 64 chunk fetches at slot indices chosen at runtime from a queue,
  27 mirror copies whose source offsets come from the live window origin, into a
  destination slot picked from a 3-deep ring; `EvictSlots` copies a
  runtime-sized batch out of runtime-chosen slots. Their count and their offsets
  are **tick data, not table data**. Widening the schema to carry
  runtime-parameterised offsets would make a row a closure and dissolve the
  property that lets `check_pass_table.py` read the `.def` as static text.

  So they go through `Recorder::CopyTracked` / `Recorder::FillTracked`, which
  take the *tracked* endpoint as a `pass::Buf` id and let §3.3 derive the
  barrier exactly as a row does. The bullet below already permitted this — "if a
  hazard needs expressing, it is expressed as a table row's `uses`" is satisfied
  by a `Use`, and `CopyToHost` established the pattern in 3b. Nothing is
  hand-written: `CopyTracked` declares a `TransferRead` on its source, and
  `FillTracked` declares a `TransferWrite`, which is what makes the §7.4
  `support` copy-then-clear WAR fall out instead of being remembered.
  `src/sim/pass_table.def` is UNCHANGED by phase 3c.

  **Still untabled: the render chain §2.6, which phase 4 covers.**
- **No barrier is ever written at a call site.** If a hazard needs expressing,
  it is expressed as a table row's `uses`. *(Live since phase 3b: the tracker is
  `gpu/vk_record.cpp`, and the Vulkan recorder reaches a command only by walking
  a `pass::Row`, so a use cannot be omitted at a call site — there are no call
  sites. The single off-table path, `CopyToHost` for the blocking hash read,
  expresses its source hazard as a `pass::Use` against the same tracker.)*
- **No static/precomputed barrier list.** §7.5.
- **The CA loop's 53 inter-iteration barriers are not negotiable.** They are the
  color lattice, not a cache flush. §7.1. *(Phase 3b's `--vk-smoke` prints the
  count: 55 global barriers per tick = 1 head + 54 CA iterations.)*
- **`synchronization2` must be ENABLED at device creation, not merely present in
  a 1.3 device.** Every scope in §3.2 is written in `VkPipelineStageFlags2` /
  `VkAccessFlags2`, so `vkCmdPipelineBarrier2` is the only barrier command the
  recorder emits. Core promotion makes the entry point resolve; it does not make
  the call legal. The backend queries the feature, enables it explicitly, and
  refuses to initialise without it — a fallback that down-converted these scopes
  to the 1.0 barrier would be a silently weaker barrier, i.e. the one failure
  mode this document exists to prevent.
- **v1 is one queue and keeps the indirect staging copies.** Both relaxations
  are legal in Vulkan and both are separate, hash-gated changes.
- **Zero-init is a mechanism, not a list**: `CreateBuffer` adds `TRANSFER_DST`
  and registers; `ZeroInitAll()` iterates the registry. §4.8.
- **Every submit takes a fence**, for staging-ring reclamation and command
  buffer reuse — not only the submits that carry a readback. §4.2.
- **The host-read barrier is emitted at `Finish()`**, after every writer, never
  at a fixed table index. §2.4 phase 7b.
- **Synchronization validation is the primary detector of a missing barrier**;
  the sledgehammer A/B is an exoneration tool and is weak evidence when it
  passes. §6.2.
