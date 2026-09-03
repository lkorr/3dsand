# RESEARCH: flight hitches — the residency shift is a per-tick GPU fence

**Date:** 2026-09-03. **Status:** research + measurement, no code changed.
**Question:** why does framerate collapse and stutter while flying, and what
would make window shifts smooth (no stall) or at least spread across ticks?

Companion docs: `docs/PLAN_surface_flight_perf.md` (history, Corrections 1-6),
`docs/PLAN_page_table.md` §3.2 / §3.6 (why the CPU mirror exists),
`src/sim/stream.cpp:860-940` (the fence's own comment).

---

## 0. The measurement (do not trust the prose without re-running this)

Binary `build/Release/sandvox.exe` of 2026-09-02 20:04 (tree 2e30611), quiet
machine, run through `scripts/run.sh`:

```
bash scripts/run.sh ./build/Release/sandvox.exe --frames 600 --autofly-surface
SANDVOX_PT_DEBUG=1 bash scripts/run.sh ./build/Release/sandvox.exe --frames 600 --autofly-surface
```

Clean run, whole frame:

| | p50 | p95 | p99 | max | >33 ms | >100 ms |
|---|---|---|---|---|---|---|
| all frames | 32.8 | 95.8 | 129.4 | 233.9 | 270 / 540 (50%) | 19 |
| low skim | 38.0 | 98.3 | | 233.9 | | |
| high cruise | 29.3 | 87.8 | | 142.6 | | |

CPU attribution, per frame: `stream` mean 33.1 ms, p50 27.4, p99 119, **92% of
CPU busy time**. Everything else is under 1 ms mean (`pageTableCpu` 0.89,
`encode` 0.53, `readback` 0.23, `readbackStall` 0.001). Snapshot stalls: 1 in
540 frames. Readback ring refusals: 11.

The shift breakdown line: **618 shifts in 540 ticking frames, 34.80 ms each**
= `evict 0.11 | fill-store 0.04 | fill-gen 0.16 | demote 33.06`. Per-tick
harvest 1.59 ms.

Debug run (the `pre` drain splits the fence), 475 shifts:

| per shift | mean | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| `pre` = GPU work already queued before the shift | 26.8 | 19.0 | 48.0 | | 270 |
| `occ` = genChunk dispatch + 128 KiB copy alone | 4.8 | 2.6 | 9.8 | | 20.7 |
| demote candidates per plane | 330 | | | | |
| sky chunks per plane (no copy) | ~586 | | | | |

Other per-tick numbers from the debug run: materialize set p50 3,792 / p90
11,924 / max 19,865 slots (CPU 0.17 ms); pool in use p50 12,709 / max 21,594;
high water 22,652 of 34,816 (65%); evict harvest 0.69 ms/event with 0.00 wait;
demote harvest memcpy 0.49 + classify 0.99 ms/event.

**This is a regression against the 2026-08-24 numbers in
PLAN_surface_flight_perf.md** (p50 25.3, p99 62, 0 frames over 100 ms). The
per-shift wait was 18.2 ms on 2026-08-31 (`--autofly-hard`) and is 33 ms today
on `--autofly-surface`. Whether that is scenario (surface vs descent), the GI
passes landed since (openness, shadow resolve, irradiance — all GPU work the
fence now waits behind), or something else, is not established here. It does
not change the diagnosis below.

### The user's live session (telemetry capture, 955 frames, stationary)

Captured over the `--telemetry` WebSocket from the running game before it
exited (raw client at the end of this doc). Stationary, no shifts: frame p50
21.1 ms, `raymarch` GPU 17.7 ms, `present` (the FIFO/GPU wait) 17.8 ms. The
live frame is **GPU-bound at ~21 ms before any streaming happens**, so a 35 ms
CPU stall lands on top of a frame that already misses 60 Hz. And
`pagesResident` was **29,063 -> 29,195 of 34,816 (83%)** — far above the
harness's 65% high water and above the "standing player 15.5k" figure in
PLAN_page_table §9.3. The abort band opens at 32,768 resident + retired. That
is a separate finding (§6) and it matters for any design that holds pages
longer.

---

## 1. Mechanism: what a shift does today, and where the 35 ms is

One axis per `Stream::Update` (called per tick, between ticks), when the
player is `kHysteresis = 2` chunks past the window centre
(`stream.cpp:227-252`). `ShiftAxis` then does, synchronously, inside the
tick:

1. `EvictSlots` — leaving plane, 1,024 slots. Async copies in 256-chunk
   batches to pooled staging; sentinel slots stored on the CPU. **No wait.**
2. `FillSlots` store-hit branch — RLE decode + `Classify` + up to five deferred
   `WriteBuffer`s per revisited slot. **No wait** (but see R6: the class-A upload path).
3. `FillSlots` gen branch — `EnsurePageForOverwrite` × gen slots (a page for
   every generated chunk, because a kernel cannot allocate), `genList` +
   `tickUBO` upload, one command buffer with `worldgenList` (1,024 workgroups,
   16 MiB of voxels), `Submit`. **No wait.**
4. **The occupancy prefilter** (`stream.cpp:941-963`): copy the 128 KiB
   `occupancy` buffer to staging, `Submit`, `MapReadDeferred`, **`omap.Wait()`**.
   `MapReadDeferred` borrows the fence of the LAST submit, and a fence on a
   single queue waits for everything queued before it: the previous frame's
   render (raymarch + GI passes), the previous ticks' CA submits, this
   shift's eviction copies, then genChunk. That is the 26.8 ms `pre` against
   the 4.8 ms `occ`.
5. From the read-back occupancy, on the CPU: (a) the **act set** — which
   generated chunks can act, declared to the page table via `RefilledSlot` so
   `Materialize` (next in `SubmitTick`) gives their 26-neighbourhoods pages
   before the CA runs; (b) sky chunks → `SetSentinel(EMPTY)` with no copy;
   (c) full chunks → `IssueDemoteCopies` (async, verified later by
   `HarvestDemotes`).

Only (5a) needs the data THIS tick. The stream.cpp comment (quoted in full at
`stream.cpp:910-940`) is right that filtering the act set on the PREVIOUS
shift's occupancy is unsafe: a chunk wrongly judged "pure sky" is never
declared, its neighbours are never materialized, and the CA's first write into
a sentinel neighbour is a silent lost voxel (the 217-page-fault bug). The
comment concludes the act set must reach the CPU "by a route that is not a CPU
fence" and names "compute the act set on the GPU beside genChunk". Half of
that already exists:

**`genChunk` already computes the act predicate on the GPU.**
`worldgen.wgsl:4029+` accumulates `wgAct` (any cell with `matCanAct`) per
workgroup and writes `dirtyIn[slot]` / `dirtyOut[slot]` itself (grep
`atomicStore(&dirtyIn[slot]` near line 4150). The GPU wake is not the problem.
The fence exists so the CPU MIRROR learns the same set in the same tick,
because `PageTable::Materialize` must run before the CA that acts on it.

So the constraint is not "the CPU needs occupancy". It is **"the CA must not
act on a freshly generated chunk before the CPU has materialized that chunk's
neighbourhood"**, and there are two ways to satisfy it: block the CPU until
it knows (today), or **delay the wake by a fixed number of ticks** so the
knowledge can arrive asynchronously.

### Why the frame hits 100+ ms: the shift stacks with the tick cap

`main.cpp:5282`: up to 4 ticks per frame, backlog capped at 4 ticks. At sprint
(32 m/s ≈ 0.67 chunk/tick) a shift fires every ~1.5 ticks. A 35 ms shift makes
the frame long, the long frame owes 2-4 ticks, 2-3 of those ticks shift, the
frame is now 70-105 ms, and the loop is self-sustaining. That is the p95 of
96 ms and the 19 frames over 100 ms. Gaffer's tick clamp is correctly in
place; it cannot help because the cost is INSIDE the tick.

---

## 2. Recommendation, ranked

> ## LANDED 2026-09-03 — R1 and R4, branch `worktree-agent-a9033ba7ce34f01bc`
>
> Both shipped essentially as written below. Numbers, `--frames 600
> --autofly-surface`, same machine, before = tree 2e30611 (§0 above):
>
> | | before | after |
> |---|---|---|
> | window shift | 34.80 ms (evict .11 / fill-store .04 / fill-gen .16 / **demote 33.06**) | **2.74 ms** (.09 / .09 / .13 / **demote 0.23** / wake-wait 0.65) |
> | shifts | 618 in 540 frames | 338 in 600 frames |
> | frame p50 / p95 / p99 / max | 32.8 / 95.8 / 129.4 / 233.9 | **16.0 / 56.4 / 87.1 / 119.8** |
> | frames > 33 ms | 270 (50%) | **96 (17.8%)** |
> | frames > 100 ms | 19 | **2** |
> | page pool high water | 22,652 (65%) | 25,831 (74%) |
> | page faults | 0 | **0** |
>
> Control arm `--frames 600 --autofly-hard` on the shipped tree: p50 9.1, p95
> 40.0, p99 64.0, max 83.7, 0 frames over 100 ms, shift 5.49 ms. (No same-tree
> "before" for this arm — the main checkout's binary was rebuilt by another
> session mid-package, so the only honest comparison is the surface one above.)
>
> **Two corrections to §2 R1, both from measurement:**
>
> 1. **K = 2 IS NOT ENOUGH, and the reason the doc gives for capping it is not
>    the binding one.** At K=2 the CPU work fell exactly as designed (`demote`
>    33.06 → 0.31 ms) but the T+K poll was still not ready on **296 of 390
>    shifts** and blocked **16.86 ms each**: the engine is GPU-bound at ~21 ms
>    and runs under one tick per frame at that rate, so two ticks is not even
>    one frame of drain. The measured curve:
>
>    | K | shift ms | wake-wait ms (missed / shifts) | frame p50 / p95 / p99 | >100 ms | pool HW |
>    |---|---|---|---|---|---|
>    | 2 | 18.22 | 16.86 (296 / 390) | 22.2 / 75.9 / 97.5 | 6 | 20,567 |
>    | 3 | 8.87 | 7.42 (160 / 366) | 19.2 / 82.0 / 109.6 | 12 | 22,003 |
>    | 4 | 2.74 | 0.65 (17 / 338) | 16.0 / 56.4 / 87.1 | 2 | 25,831 |
>
>    K=4 is shipped. The doc's bound (`K × 0.67 chunk/tick < kHysteresis`)
>    would have forbidden it, but that bound is about a REVERSAL scrolling the
>    plane back out before its wake, and the implementation handles reversal
>    explicitly — `Stream::InvalidatePendingSlots` blanks any pending verdict
>    for a slot a later shift regenerates, and the later shift's own entry
>    covers them. What actually bounds K is the plane being inert for K ticks
>    (133 ms, 6+ chunks away, invisible) and the residency below.
>
> 2. **"Pool cost: ~1,200 pages at K=2" understates it.** The whole plane holds
>    its pages K ticks longer, not just the sky share, and the measured cost at
>    K=4 is **+3,179 pages of high water (65% → 74%)**. That is fine in the
>    harness. It is NOT obviously fine against §6's live-session 29,195 (83%),
>    and §6 is still undiagnosed — if the live number is real, this change puts
>    it near the abort band and K should be reconsidered (it is one `constexpr`
>    in `stream.h` and the curve above is the whole trade-off).
>
> **One thing NOT closed.** The T+K poll still blocks when it misses (17 of 338
> shifts, 0.65 ms amortised). Deferring the completion another tick instead
> would make the tick a plane first acts on a function of fence timing, which
> is the determinism rule, so blocking is the only legal answer; the only lever
> is K.
>
> Also landed with it: `genAct` (binding 30, `simBGL_` only) and
> `TickParams::genDeferWake` — see DESIGN.md's Streaming section.


### R1. Deferred wake: generate at tick T, wake at tick T+K, never fence (the fix)

Change the shift from "gen, wait, wake" to a two-phase pipeline with a fixed
tick latency `K` (2 is enough at any sane speed; see below):

**Tick T (`ShiftAxis`):** evict, bump origin, allocate pages, submit
`worldgenList` exactly as today — but the kernel does **not** write
`dirtyIn`/`dirtyOut` for generated slots. It writes `occupancy[slot]` as now
(the renderer, occupancy-skip and shadow rays need it immediately) and writes
its `wgAct` verdict into a small side buffer indexed by genList position. Then
submit the 128 KiB occupancy copy (or, better, just the ~4 KiB side buffer:
`nonAir`, `blocker`, `act` per generated slot) and `MapReadDeferred` — **no
`Wait()`**. Record `{tick T, plane slots, ticket}` in a pending-shift queue.

**Ticks T+1 .. T+K-1:** the plane is resident, generated, rendered, and
inert. Nothing dispatches it (not in `dirtyIn`). Neighbouring active chunks
may write one cell into it — those writes land on real pages (every gen slot
has one), and `markDirty` puts the written chunk into the GPU dirty set and
the snapshot, exactly as for any other chunk.

**Tick T+K (`Stream::Update`, before `SubmitTick`):** the ticket is polled
with `Ready()`. It has had K ticks of wall time; the fence covers work that
was submitted K ticks ago. If it is not ready, `Wait()` — a stall, but a
rare and reproducible one, the same shape as the existing snapshot-staleness
fallback. Then run today's CPU logic unchanged: act set → `RefilledSlot`
(`Materialize` in this tick's `SubmitTick` gives the neighbourhoods pages),
sky → `SetSentinel(EMPTY)`, full → `IssueDemoteCopies`. Finally **wake** the
act set on the GPU for this tick: the act slots into `dirtyIn` via the
existing wake path (`EncodeWakeAll`'s list form, or an `AddOp`-style wake
list the mutate kernel already knows how to apply — pick whichever is already
slot-list-shaped).

**Determinism.** The world hash depends on WHEN a chunk first acts. Today that
is tick T (gen tick). Under R1 it is tick T+K with K a constant, so it is a
pure function of inputs and tick, not of readback timing. The only
timing-dependent behaviour is the rare `Wait()`, which changes WHEN the CPU
proceeds, never WHAT it does. `--selftest` twice-run and `--gate streaming`
(which flies) are the gates; `--residency dense` must stay bit-identical to
paged; the hash WILL move once (generated terrain acts K ticks later, RNG is
keyed on tick) — one `--rebaseline`, not an investigation.

**The two races the deferral opens, and the closing argument for each:**

- *A neighbour writes into a plane chunk between T and T+K, and the sky
  demote at T+K turns that chunk into `EMPTY`, deleting the grain.* The
  writer chunk was in `dirtyIn` at the write tick, so it was in the snapshot's
  dirty flags and in `cpuDirty_`, and `Materialize` dilates `cpuDirty_` by
  N26 — the written chunk is therefore in `cpuDirty_` at T+K. Make the sky
  demote conditional on `!CpuDirty(slot)` (the same conjunct §3.6 already
  requires for freeing a page: "a chunk cannot be simultaneously in the
  materialization set and eligible to free"). Belt and braces: also require
  the snapshot's `dirtyFlags[slot] == 0`. Both are lookups the code already
  does elsewhere.
- *The plane scrolls out again before T+K (reversal at a boundary).* The
  pending-shift entry names its slots and origin; on eviction of any of them
  the entry is dropped (or completed early with a `Wait()`). K=2 at 0.67
  chunk/tick is 1.3 chunks of travel against a 2-chunk hysteresis, so a
  reversal cannot reach the plane in K ticks; a teleport goes through
  `ReloadWindow`, which should drain the queue.

**Pool cost.** ~586 sky pages per plane are held K ticks longer: about
1,200 pages at K=2, against a harness high water of 22,652 and the live
session's 29,195. Fine in the harness; §6 says the live number needs its own
look first.

**What it buys.** The 33 ms `demote` term becomes ~0 on the shift tick and
`Ready()`-polled bookkeeping at T+K. Frame CPU falls to render + CA + ~1.5 ms
of harvest. The GPU still does genChunk (2.6-20 ms) — see R2.

**Why not the other two exits.** "Filter on last shift's occupancy" is unsafe
for the reason stream.cpp gives. "Wake the whole plane" was measured twice as
fatal pool exhaustion (`stream.cpp:966-975`). "Act set on the GPU, CA acts the
same tick" fails for the same reason as the whole-plane wake: the CPU cannot
materialize neighbourhoods it has not been told about. R1 is the version of
"act set on the GPU" that respects the mirror: the GPU decides, the CPU
learns K ticks later, and the CA is simply told to wait K ticks.

### R2. Spread the plane's worldgen over ticks (once R1 removes the fence)

With no fence, `worldgenList` no longer needs to finish in the shift tick — it
needs to finish by T+K. Split the 1,024-chunk dispatch into K submits of
1,024/K (or budget it: the `occ` distribution says 2.6 ms p50 but 9.8 ms p90
and 20.7 ms max, so some planes are 4x the median — trees and caves, not sky).
Ordering rule for determinism: generation order within a plane must be a
fixed function of the plane, never of frame time (Godot Voxel budgets in ms
and documents the resulting non-repeatability; here budget in CHUNKS PER TICK,
carry the remainder to the next tick).

Also worth taking from the plan's open list: **`treeAt` per-column hoist**
(PLAN_surface_flight_perf item 3) — the tile set is column-invariant and
`genChunk` is already column-major; this attacks the 9.8-20 ms tail directly.

### R3. Speculative prefetch into free pages (the "no latency at all" version)

The page pool breaks the constraint that makes toroidal clipmaps unable to
generate ahead (in a pure toroidal texture the incoming slab IS the outgoing
slab's memory; here they are different pages behind the same slot). Predict
the next plane from velocity, generate it into free pages over the preceding
ticks — genChunk writing through an explicit `(slot, page)` list the way
`pagefill` already does, occupancy/sub-occ/act into side buffers — read back
async at leisure, and at the crossing: evict the old plane, flip 1,024
`pageTable` entries, copy the side occupancy in, wake. The crossing becomes
O(1,024 table writes) and there is no worldgen and no readback on the
critical tick at all.

Costs: one plane of pool headroom per predicted axis (+1,024-3,072 pages),
wasted work on a wrong prediction (the player turns), a second set of
slot-indexed side buffers, and a bigger change to `worldgen.wgsl` and the
pass table than R1. R3 is the right END STATE; R1 gets ~90% of the frame-time
win with a fraction of the surface area and is a strict prerequisite (R3 needs
the same deferred-wake machinery for the flip tick).

### R4. Cap shifts per frame at one, and make the accumulator shift-aware

Independent of R1. `Stream::Update` is called per tick; a 4-tick frame can
shift 3 times. One axis per FRAME is safe by the same argument the code
already uses for one axis per call (`kHysteresis = 2` chunks of slack; the
player is 13+ chunks from the window edge). This turns a 105 ms frame into a
~40 ms frame TODAY, before R1, and after R1 it bounds the GPU worldgen per
frame. It is the cheapest change in this document — a frame counter and one
compare. It does not change any tick's world state: the shift still happens
at the first tick of the next frame, which is a different tick than today, so
the hash moves and the `streaming` gate re-baselines.

### R5. Vulkan queues (secondary; real but smaller than R1)

- **Async compute queue for `worldgenList`.** Removes the render/CA backlog
  from anything that waits on worldgen (the 26.8 ms `pre` term) even without
  R1, and lets genChunk overlap the raymarch. NVIDIA's guidance: both are
  bandwidth-heavy compute, so expect modest concurrency; the win is the
  fence scope, not throughput. Needs a semaphore from the gen queue to the
  tick that first reads the plane (with R1 that is T+K, comfortably late).
- **Dedicated transfer queue** for eviction copies, demote copies, snapshot
  readbacks and deferred uploads (GPUOpen reports ~10% frame time in one
  title from double-buffered copy-queue uploads). Buffers can stay
  `SHARING_MODE_CONCURRENT`; ownership transfer is an image concern.
- **Timeline semaphores** replace the per-submit fence borrowing that makes
  every `MapReadDeferred` wait on "whatever was submitted last". Not needed
  for R1, needed for R3 or any multi-queue design.
- Verify any overlap in Nsight GPU Trace; a second `VkQueue` is not proof of
  a second hardware queue (Intel and pre-Ampere NVIDIA may serialise).

### R6. Small things found on the way

- `harvest` 1.59 ms/tick mean is `CompleteOldest` + `HarvestDemotes`: 4 MiB
  memcpy out of HOST_COHERENT (uncached) mapped memory plus RLE/Classify. The
  readback staging is `HOST_COHERENT` only (`rhi_vulkan.cpp:691`); asking
  for `HOST_CACHED` where available makes CPU reads of staging much cheaper
  (VMA's `HOST_ACCESS_RANDOM` pattern). Cheap to try; measure the harvest
  line.
- A **revisited** plane uploads 1,024 × 16 KiB through `WriteBuffer` class A
  (`kClassAMaxBytes = 65536`), i.e. 16 MiB recorded inline as
  `vkCmdUpdateBuffer` in the next command buffer. Forward flight never hits
  this (fill-store 0.04 ms), a return trip over visited terrain will. Route
  ≥16 KiB writes through the staging ring copy path (class B). Not measured
  here; flagged because the mechanism is certain and the scenario is common.
- Readback ring depth 3 refused 11 requests; snapshot stalls 1. Not the
  problem today; R1 adds one more in-flight map per shift, so watch it.
- Store `Get`/`Put` can do synchronous disk I/O on the tick path (region
  load, LRU spill). Not seen in these runs (pure RAM store); flagged.

---

## 3. What the outside world does (sources in §7)

- **Budget in units, carry over, prioritise by distance** (Godot Voxel
  Tools' `time_budget_ms`, Sodium's effort-estimated upload limit, the
  PocketMine FIFO-generation bug). Here: budget in chunks per TICK, not ms,
  or determinism goes.
- **Coarse-to-fine with a cutoff** (Losasso & Hoppe geometry clipmaps: update
  coarse levels first, stop at budget). Here: sentinel classification is the
  "coarse" step and is already free; the analogue is R2's chunk budget.
- **Latency-tolerant residency is the standard design** (id's virtual
  texturing: "a frame old data and a frame of latency" is fine; GigaVoxels'
  request buffer + LRU with a per-frame fill cap; sparse virtual shadow maps'
  atomic page free-list). A CA has no "blurry fallback", so the tolerant form
  here is "inert for K ticks" (R1), never "act on stale data".
- **Toroidal clipmaps never generate ahead** because the incoming region's
  memory is live (SGI clipmaps, VXGI, Enlisted, Ogre-Next revoxelise one
  row). The page pool is what makes R3 possible here and nowhere in that
  literature.
- **Async compute / copy queues are fence-scope tools first, throughput
  tools second** (NVIDIA Do's and Don'ts; Khronos async_compute sample:
  22.9 → 21.8 ms). Matches R5's framing.
- **Tick clamps are right and insufficient** (Gaffer, Unity
  `maximumDeltaTime`): they stop the spiral from diverging; they cannot fix a
  stall inside a tick. Matches §1.

---

## 4. Verification plan (a budget, per CLAUDE.md)

- R4 alone: one `--frames 600 --autofly-surface` (frames >100 ms should drop
  to ~0; p50 unchanged), `--gate streaming`, `--gate determinism`, one
  `--rebaseline`.
- R1: `--gate determinism` and `--gate streaming` in paged AND `--residency
  dense` (must match), `SANDVOX_PT_DEBUG=1 --frames 600 --autofly-surface`
  for page faults = 0 and the `demote` term, then the clean run for the frame
  distribution. The `demote` column of the shift breakdown is the number to
  quote: 33.06 ms → expected < 1.
- Both: `--autofly-hard` as the control arm (must not regress), pool high
  water noted alongside (R1 holds pages K ticks longer).

---

## 5. Explicitly not the cause (so nobody re-measures it)

- The renderer's altitude curve (fixed 2026-08-23/24, Corrections 1-6).
- `ReadbackBlocking` — no frame-path caller remains (tests/measure/lab only).
- The `WaitIdle` staleness fallback — 1 stall in 540 frames.
- `Classify`, the RLE, the CPU page-table mirror — all sub-millisecond per
  tick in these runs.
- The eviction path — 0.11 ms per shift, 0.00 ms of waiting.

## 6. Open: why is the live game at 83% pool residency?

29,063-29,195 pages resident, stationary, in the user's session vs 12,381 in
use at the harness exit and 22,652 high water under 20 s of sprint flight.
The B4 positive feedback (fewer demotable slots → bigger materialize set)
degrades frame time before the abort. Not diagnosed here. First step is
`SANDVOX_PT_DEBUG=1` in a real play session and reading the `inUse=` curve
plus the `--measure` histogram against that window, not another autofly run.
Any of R1/R3 should be sized against this number, not the harness's.

---

## 7. Sources

- Godot Voxel Tools performance / `time_budget_ms`:
  https://voxel-tools.readthedocs.io/en/latest/performance/
- Sodium effort-estimated chunk upload limit:
  https://github.com/CaffeineMC/sodium/issues/691
- PocketMine FIFO generation bug: https://github.com/pmmp/PocketMine-MP/issues/4187
- Losasso & Hoppe, Geometry Clipmaps: https://hhoppe.com/geomclipmap.pdf
- Godot proposal 3024 (amortised cascade transitions):
  https://github.com/godotengine/godot-proposals/issues/3024
- van Waveren, Software Virtual Textures:
  https://www.researchgate.net/publication/259000438_Software_Virtual_Textures
- Crassin, GigaVoxels cache management:
  https://maverick.inria.fr/Publications/2009/CNLE09/CNLE09.pdf
- Sparse virtual shadow maps (GPU page free-list):
  https://ktstephano.github.io/rendering/stratusgfx/svsm
- NVIDIA async compute and overlap:
  https://developer.nvidia.com/blog/advanced-api-performance-async-compute-and-overlap
- NVIDIA async copy: https://developer.nvidia.com/blog/advanced-api-performance-async-copy
- GPUOpen concurrent execution / async queues:
  https://gpuopen.com/learn/concurrent-execution-asynchronous-queues/
- Khronos async_compute sample:
  https://docs.vulkan.org/samples/latest/samples/performance/async_compute/README.html
- Khronos timeline semaphore sample:
  https://github.com/KhronosGroup/Vulkan-Samples/tree/main/samples/extensions/timeline_semaphore
- nvpro vk_async_resources (never write in-flight targets; double-buffer):
  https://github.com/nvpro-samples/vk_async_resources
- Vulkan memory types on PC (HOST_CACHED for readback):
  https://asawicki.info/news_1740_vulkan_memory_types_on_pc_and_how_to_use_them
- Fix Your Timestep: https://gafferongames.com/post/fix_your_timestep/
- Enlisted 3D clipmap GI (toroidal, no copies on move):
  https://enlisted.net/en/news/show/25-gdc-talk-scalable-real-time-ray-traced-global-illumination-for-large-scenes-en/
- Ogre-Next VCT partial revoxelisation:
  https://www.ogre3d.org/2021/10/25/upcoming-global-illumination-improvements-in-ogre-next

## Appendix: capturing the live game's frames

The `--telemetry` WebSocket on port 8080 sends one JSON `PerfSample` per frame
(`telemetry.cpp:312+`) to every connected client (`kMaxClients`).
`scripts/telemetry_capture.py <seconds> <out.jsonl>` (added with this doc) is a
40-line stdlib Python client (RFC 6455 handshake + frame parse, no
dependencies); it captured 955 frames in 20 s without disturbing the tuner's
own connection.
Fields used above: `wallMs`, `cpu.*` per `PerfScope`, `gpu.raymarch`,
`counters.pagesResident`. This is the cheapest way to get the USER's numbers
rather than a harness's, and it is passive.
