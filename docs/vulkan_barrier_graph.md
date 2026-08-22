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
`world.cpp:105-235`, `support.cpp:104-173`, `stream.cpp:140-279`, and the WGSL
binding declarations). Edges this document adds beyond the pass map are marked
**[NEW EDGE]** and are listed together in §6.9.

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
| Submit → submit on one queue | implicit: submission order on a single queue with no semaphores still guarantees *submission* order for the implicit ordering guarantee, but **not** memory visibility — see §3.0 |
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
any buffer's last-access state. §6.4 covers why that is the only correct
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
| T11 | `mutateCells` | `mutateCells_` (`sim_mutate:cells`) | Compute `((cells+63)/64,1,1)` wg`(64)` | `materials`:SR, `tickUBO`:U, `cellOps`:SR | `voxels`:RW, `DirtyIn`:AtomicRMW, `DirtyOut`:AtomicRMW | cCells |
| T12 | `explodeMark` | `explodeMark_` (`sim_explode:mark`) | Compute `(11*exp,11,11)` wg`(4,4,4)` | `voxels`:SR, `materials`:SR, `tickUBO`:U, `expOps`:SR | `expMask`:SW, `DirtyIn`:AtomicRMW, `DirtyOut`:AtomicRMW | cExp |
| T13 | `explodeApply` | `explodeApply_` (`sim_explode:apply`) | Compute `(11*exp,11,11)` wg`(4,4,4)` | `materials`:SR, `tickUBO`:U, `expOps`:SR, `expMask`:SR | `voxels`:RW, `DirtyIn`:AtomicRMW, `DirtyOut`:AtomicRMW, `particleCounts`:AtomicRMW, `Particles[page]`:SW | cExp |
| T14 | `particleSpawn` | `pSpawn_` (`sim_particle:spawn`) | Compute `((spawn+63)/64,1,1)` wg`(64)` | `spawnOps`:SR, `tickUBO`:U | `particleCounts`:AtomicRMW, `Particles[page]`:SW | cSpawn |
| T15 | `compact` | `compact_` (`sim_compact:main`) | Compute `(64,1,1)` wg`(64)` | `DirtyIn`:SR | `dirtyList`:SW, `argsStage`:AtomicRMW | cAlways |

Note T12 reads `voxels` and T13 writes it: an intra-"pass" RAW that Dawn
inserted for free. This is the mark/apply split CLAUDE.md documents as the fix
for a kernel that both read and wrote a neighborhood — it only works if the
barrier between them exists.

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

**Phase 7 — readbacks** (`World::EncodeReadbacks`, `world.cpp:105-163`; all
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
| T78 | `copy.dirtyNext` | Copy 16 KiB | `DirtyOut`:TransferRead | `slot.buf`:TransferWrite |
| T79 | `barrier.hostRead` | Barrier only | `slot.buf`:HostRead | — |

T70–T78 are all transfer→transfer against each other and need **no barriers
between themselves** (they write disjoint ranges of `slot.buf` and read
different sources) — with the single exception of T76→T77, which is a genuine
transfer-read→transfer-write WAW/WAR on `support` and is the case most likely to
be dropped as "just two copies" (§6.2). T79 is a synthetic row: it exists so the
recorder emits the host-visibility barrier described in §3.2.

**Phase 8 — measurement**

| # | Name | Kind | Notes |
|---|---|---|---|
| T80 | `timerResolve` | Copy | `vkCmdCopyQueryPoolResults`; `--measure` only, `passTimer_ != null`. Timestamps are written by `vkCmdWriteTimestamp2` around rows carrying a timer label, which does not change any dispatch — the determinism claim in `passtimer.h` survives the port unchanged. |

### 2.5 The non-tick tables

Same schema, separate arrays, each recorded into its own command buffer.

**`worldgen`** (`simulation.cpp:550-566`): fills `dirty[0]`, `dirty[1]`, `hash`,
`support`, `particleCounts`, `claim`, `drawArgs` (7 Fill rows, no hazards among
them), then `worldgen_` direct `(4096,1,1)` reading `materials`/`tickUBO`,
writing `voxels`:SW, `occupancy`:SW, `DirtyIn`:AtomicRMW, `DirtyOut`:AtomicRMW.
Note the fills of `dirty[0/1]` DO hazard against the dispatch — `genChunk`
`atomicStore`s both (worldgen.wgsl:2605-2609) — so a TRANSFER→COMPUTE barrier is
required there and the "no hazards among them" applies only to the fills
pairwise.

**`genList`** (`simulation.cpp:568-576`): `worldgenList_` direct `(count,1,1)`,
reads `genList`:SR, `materials`:SR, `tickUBO`:U; writes `voxels`:SW,
`occupancy`:SW, `DirtyIn`:AtomicRMW, `DirtyOut`:AtomicRMW. Recorded in its own
submit from `Stream::FillSlots`; §3.7.

**`loadReset`** (`:588-601`): 5 fills + `occupancy_` full.
**`hashOnly`** (`:603-611`): fill `hash` + `occupancy_` full.
**`wakeAll`** (`:613-620`): an upload only — no command buffer today; §3.1
gives it one.

### 2.6 The render table

Recorded per frame, one submit. Its only interesting property for this document
is what it reads that the tick wrote.

| # | Name | Kind | Reads (buffers) |
|---|---|---|---|
| R0 | `hoisted uploads` | Copy | writes `mbInstBuf_`, `bodyInstances`, `bodyXforms`, `sprites`, `debugBoxes`, `renderUBO`, `mbModelBuf_`, `mbPoolBuf_` — **before** `vkCmdBeginRendering` (§3.6) |
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
§6.9's finding on `farFill`'s `atomicStore`.)

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

Iteration k+1 reads `voxels` that iteration k wrote; both write `DirtyOut` and
`support` atomically. The algorithm produces, between every pair, three buffer
barriers:

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

1. **Correctness is strictly stronger.** A global memory barrier's scope covers
   every buffer, including any binding a future edit adds to `sim_step.wgsl`
   without updating the table. In the loop with 53 repetitions and the entire
   determinism guarantee riding on it, a barrier that cannot be made too narrow
   by a table mistake is worth more than a barrier that is minimal.
2. **In practice every real driver implements a compute→compute buffer barrier
   on a 512 MiB device-local buffer as a full cache flush + invalidate anyway.**
   Buffer barriers on VkBuffers do not carry layout information; the range is
   advisory to nearly all implementations. Form (A) buys measurable time only on
   implementations that do per-range tracking, which for storage buffers is
   uncommon.
3. **It is one API call with one struct instead of three, in a loop that runs 54
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
intermittently. §6.3.

### 3.8 Special case: atomic-only WAW on `farVox` / `farOcc`

T5A (`farDown`) and T60 (`farFill`) both touch `farVox`/`farOcc` with atomics
only, in that order, in the same command buffer. The algorithm emits a
COMPUTE→COMPUTE `READ|WRITE`→`READ|WRITE` barrier between them. That is correct
and necessary — see §6.9 for why the "they're both atomic, atomics are coherent"
intuition is wrong here specifically.

The far-field buffers are render-only derived data (DESIGN.md §9), excluded from
the world hash and from persistence. A missing barrier there therefore cannot
desync the sim — it can only produce a visibly wrong cascade. That makes them
**lower risk, not zero risk**, and the table does not special-case them.

### 3.9 Special case: skipped conditional rows

Covered as a risk in §6.4 because the failure mode is subtle. The mechanism is
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
submit*, in queue order, with no barrier needed by the caller. Vulkan has no
such operation. Two mechanisms, chosen by size class:

**Class A — `vkCmdUpdateBuffer`, for ≤ 65536 B.** Recorded directly into the
command buffer at the head. It is a transfer operation, so the recorder's
`Fill`/`Copy` machinery already tracks it correctly as `TransferWrite` — no
staging allocation, no host-visible memory, no extra lifetime. `vkCmdUpdateBuffer`'s
limit is 65536 bytes and the data must be 4-byte-aligned in size and offset;
both hold for every buffer in this class.

Class A covers: `tickUBO` (64 B), `renderUBO`, `farUBO` (128 B), `opsBuf`
(≤2 KiB), `expOps` (≤256 B), `particleCounts` (4 B partial), `sprites`
(≤2 KiB), `bodyXforms` (≤16 KiB), `farList` (≤16 KiB), `dirty[page]` on the
day/night wake (16 KiB), `genList` (≤16 KiB), `mbInstBuf_` (≤8 KiB),
`mbModelBuf_` (4 KiB), `debugBoxes`.

`debugBoxes` sits **exactly on the boundary**: `kMaxDebugBoxes = 1024`
(`world.h:156`) × `sizeof(DebugBox) = 64` (`world.h:155` static_assert) = 65536
bytes, and `vkCmdUpdateBuffer`'s limit is "≤ 65536", so a full-capacity write is
legal by one byte. Same for `microTableBuf_` at `kMaterialSlots = 4096` ×
`sizeof(MicroBrickGpu) = 16` = 65536. **The recorder must `static_assert` both
sizes against the 65536 limit rather than trusting this**, because raising
either constant by one entry silently turns a legal `vkCmdUpdateBuffer` into a
validation error — and `microTableBuf_` is therefore assigned to Class B below
despite fitting, so that a hot-reload path never sits on the boundary.

**Class B — persistent-mapped staging ring + `vkCmdCopyBuffer`, for > 65536 B.**
A ring of `HOST_VISIBLE | HOST_COHERENT` buffers, persistently mapped, sized to
comfortably hold one frame's worth of large uploads (16 MiB is ample: the worst
tick is `cellOps` 512 KiB + `spawnOps` 128 KiB + `bodyInstances` 4 MiB +
`microPoolBuf_`/`mbPoolBuf_` 4 MiB each on a hot reload). The CPU memcpies into
the ring at the same call site that calls `WriteBuffer` today, records a
`vkCmdCopyBuffer` at the submit head, and advances the ring cursor. Ring regions
are reclaimed by the same fence that retires the submit that consumed them.

Class B covers: `cellOps` (≤512 KiB), `spawnOps` (≤128 KiB),
`bodyInstances` (≤4 MiB), `materialBuf_` (4096 × `sizeof(MaterialGpu)`),
`reactionBuf_`, `microTableBuf_` (64 KiB exactly — Class B by the boundary rule
above), `microPoolBuf_` (4 MiB), `mbPoolBuf_` (4 MiB), `passUBO` (13.5 KiB,
once at init — Class A would also work), and streaming's per-slot 16 KiB
`voxels` writes.

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

**Ordering.** All uploads for a submit are recorded at the **head** of that
submit's command buffer, in the order the CPU issued them, before any pass row.
This reproduces WebGPU's semantics exactly, including the semantics that make
`SetArtPalette`'s partial write into a live `materialBuf_` (assumption 9) safe:
a partial `vkCmdUpdateBuffer`/`vkCmdCopyBuffer` into a range no in-flight submit
is reading, ordered ahead of every reader in this submit, is the same operation
WebGPU performs. The recorder tracks `materialBuf_` as `TransferWrite` and the
first shader read of it in the tick gets a TRANSFER→COMPUTE barrier for free.

**`EncodeWakeAll` gets a command buffer.** Today it is a bare
`queue.WriteBuffer` with no encoder (`simulation.cpp:613-620`), issued from
`SubmitTick` before the tick's encoder is created. Under the upload path it
becomes a Class A `vkCmdUpdateBuffer` recorded at the head of the *tick's* own
command buffer, ahead of T00. This is a behavior-preserving simplification: the
write must land before `compact` reads `DirtyIn`, and nothing between the two
touches `dirty[page]`. **[NEW EDGE]**: note that T00 fills `dirty[1-page]` and
the wake writes `dirty[page]` — different buffers, no hazard — but if the page
ever aliased, the wake would be clobbered. The recorder's symbolic
`DirtyIn`/`DirtyOut` resolution makes this checkable.

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

- **Which fence signals what.** The fence passed to `vkQueueSubmit` for the
  **tick command buffer that contains rows T70–T79**. One fence per submit; the
  slot borrows it. If a tick records no readback (`cReadback` false), it submits
  with `VK_NULL_HANDLE` and no slot is claimed. This is a 1:1 mapping onto
  today's `lastSlot_`.
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
  writes are visible to the host. The T79 row emits
  `srcStage=COPY, srcAccess=TRANSFER_WRITE → dstStage=HOST, dstAccess=HOST_READ`
  as the last barrier in the command buffer. **JUDGMENT:** with coherent memory
  and a fence wait this barrier is arguably redundant (the fence + coherent
  memory is the documented sufficient condition), but it is one barrier per tick
  and it makes the readback's visibility requirement explicit in the same table
  everything else lives in. Keep it.
- **Blocking readbacks** (`ReadHashSync`, selftest voxel dumps, screenshots)
  become: record copies → submit with a fence → `vkWaitForFences(UINT64_MAX)` →
  read the map. The one sanctioned synchronous path stays exactly as sanctioned.

### 4.3 (c) Eviction staging pool cross-submit ordering (assumption 7)

`Stream::EvictSlots` (`stream.cpp:157-181`) records `voxels → evictStaging`
copies in **its own encoder**, submits it, and only then does `FillSlots`
overwrite the same `voxels` slots with `WriteBuffer`. The comment at
`stream.cpp:172-174` states the guarantee explicitly: queue order makes the copy
read the leaving plane's data even though the map completes ticks later.

Vulkan preserves this, with one added requirement:

1. **Same queue, submit order preserved.** v1 has one queue (§5), so the
   eviction submit is ordered before the `FillSlots` uploads by
   submission-order semantics. But submission order alone gives execution
   ordering; the eviction copy's *read* of `voxels` and the fill's *write* of
   `voxels` is a WAR across submits.
2. **The §3.4 global barrier at the head of every command buffer resolves it.**
   The `FillSlots`/`genList` command buffer opens with
   `ALL_COMMANDS/MEMORY_WRITE → ALL_COMMANDS/MEMORY_READ|WRITE`, which orders
   its transfer writes after every prior submit's reads. This is precisely the
   class of cross-submit hazard §3.4 exists to make un-reasonable-about.
3. **`CompleteOldest` becomes a fence wait.** Each `PendingEvict` carries the
   `VkFence` of its own submit. `CompleteOldest` does
   `vkWaitForFences(fence, UINT64_MAX)` — a genuine block, exactly as
   `instance.WaitAny(p.future, UINT64_MAX)` blocks today (`stream.cpp:201`) —
   then reads the persistently-mapped staging buffer, RLE-encodes, and returns
   the buffer to `stagingPool_`. The `kMaxPendingEvicts = 4` ring and the
   "ring full: recycle the oldest" path in `AcquireStaging`
   (`stream.cpp:184-195`) are unchanged in shape.
4. **`FillSlots`'s `while (pendingChunks_.count(...)) CompleteOldest()` loop**
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

Under §4.1 this is preserved exactly: each submit's `tickUBO` upload is recorded
at the head of *its own* command buffer, so the streaming submit reads the
streaming params and the tick submit reads the tick params. The cross-submit
WAW on `tickUBO` (streaming's write vs. the tick's write) is ordered by
submission order + the head global barrier.

**JUDGMENT / recommendation, not required for the port:** give streaming its own
`streamTickUBO` rather than borrowing the shared one. It removes a cross-submit
WAW on a uniform buffer entirely, costs 64 bytes, and makes the streaming path's
independence from the tick path structural instead of temporal. This is a
behavior-preserving change worth doing in phase 2 (it must be hash-neutral —
`worldgenList` and the tick read disjoint fields today only by convention), but
it is *not* a correctness requirement for v1 and should not be bundled into the
Vulkan landing.

### 4.8 (h) Zero-init policy (assumption 14)

**Policy: `vkCmdFillBuffer(buf, 0, VK_WHOLE_SIZE, 0)` on every buffer at
creation, without exception, in one command buffer submitted before anything
else runs.** WebGPU guarantees zero-initialized buffers; Vulkan guarantees
nothing. Matching the guarantee wholesale is one ~50-line loop over the buffer
inventory, executed once at startup, and it eliminates an entire class of
"works on my driver" bug.

Do not attempt to enumerate which buffers need it. The load-bearing cases known
today are:

- **`farVox` (128 MiB) and `farOcc` (128 KiB)** — `world.cpp:71-74` states it
  outright: zero = air, so unfilled cascade regions render as sky rather than
  garbage. `farOcc` has *no* `CopyDst` usage at all today
  (`world.cpp:77-78`), so it is written only by shaders and is *entirely*
  dependent on zero-init for every level-chunk the fill has not reached. Under
  Vulkan it needs `VK_BUFFER_USAGE_TRANSFER_DST_BIT` added purely so it can be
  filled. This is the single most likely buffer to be forgotten, because
  nothing in the current code writes it from the host.
- **`dirtyList` (no `CopyDst` today)** — same usage-flag consequence.
- **`particles[0/1]`** (8 MiB each, no `CopyDst`) — read only up to
  `particleCounts`, so garbage beyond the live count is unread, but "unread"
  arguments are exactly what a zero-fill policy exists to stop having to make.
- `hash`, `claim`, `support`, `particleCounts`, `drawArgs`, `occupancy`,
  `dirty[0/1]` — all explicitly cleared by `EncodeWorldgen`/`EncodeLoadReset`
  today, but only on those paths.

Every buffer therefore gains `VK_BUFFER_USAGE_TRANSFER_DST_BIT`. That flag is
free on device-local memory and is the price of the policy.

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

The call sites that matter:

- **Shader/pipeline hot-reload** (`R`, `F5`): must drain before destroying
  pipelines and descriptor sets. This is now a hard requirement rather than a
  convenience — Dawn refcounted objects into flight, Vulkan does not.
- **Save/load** (`worldio.cpp`): `LoadWorld` replaces the whole world; drain
  first, then `EncodeLoadReset`, then drain again before the readback.
- **Shutdown** (`main.cpp:2843`): drain before destroying anything. Extend to
  the staging rings, command pools, fences, and the readback slots.
- **`--shot` / `--shot-mob`**: drain, then blocking readback of the offscreen
  target.

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

2. **The WGSL.** For each row's `(wgsl file, entry point)`:
   - All `@group(G) @binding(B) var<storage, ACCESS> NAME` and
     `var<uniform> NAME` declarations, plus the module-scope `var<workgroup>`
     (ignored).
   - Which of those names appear **inside the entry point's reachable call
     graph**. This needs a light call-graph walk: `sim_step.wgsl:main` calls
     `markDirty` which touches `dirtyOut`, and a naive "does the name appear in
     the fn body" check would miss it. The walk is: collect all `fn NAME(...)`
     bodies, build a callee map by identifier match, BFS from the entry point,
     union the referenced globals. Approximate in the safe direction — a false
     *inclusion* costs a spurious barrier, a false *exclusion* would be a real
     bug, so the walk resolves ambiguity by including.
   - Read vs. write per name: `NAME[...] =`, `atomicStore/Add/Max/Min/And/Or/
     Xor/Exchange/CompareExchangeWeak(&NAME[...])` ⇒ write; any other appearance
     ⇒ read. `atomicLoad` ⇒ read only.

**What it asserts.**

| Check | Severity |
|---|---|
| Every buffer the shader reads is in the row's read set (or write set, for RW) | **FAIL** — a missing read means a missing RAW barrier |
| Every buffer the shader writes is in the row's write set | **FAIL** — a missing write means every later reader is unsynchronized |
| Every buffer in the row's sets is actually referenced by the entry point | WARN — spurious barriers only |
| A row declaring `StorageRead` on a binding the WGSL declares `read_write`, where the entry point never writes it | WARN, with the row required to carry a `// read-only in practice: <reason>` comment; missing comment ⇒ FAIL |
| Every entry point in every `sim_*.wgsl` / `worldgen.wgsl` appears in at least one table row | **FAIL** — an unreferenced kernel is either dead or an untabled pass |
| Every buffer id in the table exists as a `World`/`Simulation` member | **FAIL** |
| The row's declared bind-group set contains every binding the entry point uses | **FAIL** — catches a kernel added to a pass whose layout cannot bind it |
| Exactly the rows marked `useGlobalBarrier` are the ones §3.6 sanctions | **FAIL** |

**What it cannot check, stated so nobody assumes it does:** it validates the R/W
*sets*, not the *order*. A table whose rows are in the wrong order passes this
check and produces a wrong world. That is what §6.2 is for.

**Where it runs:** added to `scripts/post_edit_check.sh` alongside
`check_invariants.py`, triggered on `assets/shaders/*.wgsl` and
`src/sim/pass_table.def`. Under phase 2 (RHI seam under Dawn) this checker is
already meaningful — the table exists and drives `EncodeTick` before Vulkan does
— which is the point of doing the restructure under Dawn first.

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

Interpretation:

| Result | Meaning |
|---|---|
| Sequences identical | No barrier in the precise recording is too weak *on this GPU, this run*. Necessary, not sufficient — a race can be latent. |
| Sequences differ | **A barrier is definitively wrong.** The first differing tick localizes it; bisect by making rows sledgehammer one phase at a time (`--barriers=precise:except=ca`, etc.). |
| Sledgehammer differs from **Dawn's** hash | Something other than a barrier is wrong (zero-init, upload ordering, a shader translation difference). Sledgehammer removes barriers from the suspect list, which is most of its value. |

Two additions that make this stronger than a single A/B:

- **Run both settings N times** (N=5) and compare all runs pairwise. A race that
  is 1-in-20 shows up as precise-vs-precise disagreement without needing the
  sledgehammer at all, and that is a cleaner signal.
- **`--barriers=sledgehammer` should also be run with a deliberately hostile
  scenario**: explosions at the residency-window edge, heavy particle traffic,
  and streaming shifts in the same ticks — the combination that puts `voxels`,
  `expMask`, `particles`, `claim` and the streaming submits all in flight.
  A settled world exercises almost none of the graph.

**Validation layers are assumed on for every debug run.** `VK_LAYER_KHRONOS_validation`
with **synchronization validation** (`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`)
detects most missing barriers directly and by construction, without needing a
divergence to happen. It is strictly the first line of defense and the
sledgehammer oracle is the second — sync validation has known gaps around
indirect args and cross-submit hazards, which is exactly where §4.5 and §4.3
live. Run both.

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

### 7.1 — `pDispatchArgs` WAR: the second copy vs. the in-flight indirect fetch

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

### 7.2 — `argsStage` double generation and the WAW at T55

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

### 7.3 — `ClearBuffer(support)` immediately after its copy-out (T76→T77)

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

### 7.4 — Skipped conditional rows leaving a stale "previous writer"

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

### 7.5 — `farOcc` has no `CopyDst` usage and depends entirely on zero-init

**The edge.** `world.cpp:77-78` creates `farOcc` with `Storage` only. Nothing on
the host ever writes it. Its correct initial state (all zero = nothing occupied)
comes purely from WebGPU's zero-init guarantee.

**Why dangerous.** In Vulkan it is uninitialized device memory. `farOcc` gates
whether the raymarcher descends into a far-field chunk; garbage means the
renderer marches through cascade regions that contain nothing, or — worse —
`farVox` garbage bytes get interpreted as material ids, producing rendered
terrain out of uninitialized memory. It is not hashed, so **no determinism test
catches it**; it is a visual bug that appears only at draw distance, only on
first frames, only before the fill has covered the region.

**Neutralized by.** §4.8's blanket fill-everything policy plus adding
`TRANSFER_DST` to every buffer's usage flags. The specific mitigation is that
the policy is blanket: an implementer enumerating "which buffers need zeroing"
would skip `farOcc` precisely because nothing writes it.

**Test.** A gate asserting that immediately after `Init`, before any worldgen,
reading back `farVox[0..N]` yields all zeros. Requires adding `CopySrc` to
`farOcc` for the test — acceptable, `farVox` already has it "selftest-only".

### 7.6 — `particleCounts[1-page]` zeroed by an upload that races the previous tick

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

### 7.7 — `farFill` (T60) overwrites what `farDown` (T5A) wrote, same submit

**The edge (see §6.9, **[NEW EDGE]**).** Both write `farVox`/`farOcc` with
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

### 7.8 — Two `dirtyList` generations, one buffer

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

### 7.9 — Edges this document adds beyond `vulkan_pass_map.md`

Collected for the reviewer.

1. **`farFill`'s `atomicStore` overwrites `fardown`'s `atomicAnd`/`atomicOr`.**
   §7.7. The map's "WAW, both atomic" understates it; the two entry points use
   incompatible atomic idioms on the same words, and the ordering is semantic
   (fill wins), not incidental.
2. **`particleCounts[1-page]` is zeroed from the host at tick N+1 against a page
   the GPU was atomically writing at tick N.** §4.4 / §7.6. The map lists the
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
6. **`farOcc` and `dirtyList` and `particles[0/1]` have no `CopyDst` usage
   today**, so the zero-init policy requires adding `TRANSFER_DST` to their
   usage flags — an easily-missed prerequisite of §4.8, and `farOcc` is the case
   where nothing in the code ever writes it from the host.
7. **`pick` reads a one-frame-stale `renderUBO`.** `pick_` runs on the *tick*
   path (T51/T59), but `WriteRenderParams` is called at `main.cpp:2569` —
   *after* the tick loop (`main.cpp:1723`) and before the render encoder
   (`main.cpp:2822`). So every pick in frame F reads the camera written at the
   end of frame F−1, and all up-to-4 picks in a frame read the same one. This
   is pre-existing behavior, harmless (`pick` output is CPU-consumed UI state,
   never sim state, and one frame of camera lag on a crosshair raycast is
   invisible), and **must be preserved rather than "fixed" during the port** —
   moving the upload earlier would change what the pick returns. Structurally
   it is the same cross-submit-chain shape as `drawArgs` and gets the same §3.4
   coverage. The map lists `renderUBO` under both `pick` and the render passes
   without noting that they are different submits or that the tick's read is
   one frame behind.

---

## 8. What this document obliges

- **A tick-path edit that changes any kernel's bindings updates
  `src/sim/pass_table.def` in the same commit**, or `check_pass_table.py` fails.
  This is the same standing obligation `tuning_params.def` and
  `sound_schema.js` carry.
- **No barrier is ever written at a call site.** If a hazard needs expressing,
  it is expressed as a table row's `uses`.
- **No static/precomputed barrier list.** §7.4.
- **v1 is one queue and keeps the indirect staging copies.** Both relaxations
  are legal in Vulkan and both are separate, hash-gated changes.
- **Zero-fill every buffer at creation.** No enumeration, no exceptions.
- **Every debug run has synchronization validation on**, and the sledgehammer
  A/B is run before any checkpoint that claims the barrier graph is correct.
