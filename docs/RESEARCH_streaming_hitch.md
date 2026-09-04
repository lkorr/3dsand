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

> ## P4-G. BUILT, MEASURED AND REJECTED 2026-09-03 — the mean cannot move and the split costs 28%
>
> **Branch** `worktree-p4g-genspread`. The implementation is complete and
> passing at **`78feb6c`**; the branch tip **reverts it** and keeps only the
> instrument that killed it. Read this section before re-attempting R2 — the
> reason it fails is arithmetic, not an implementation defect, so a better
> implementation cannot rescue it.
>
> ### What was built (it works; it is just not worth having)
>
> Exactly the design below. A shift plane's 1,024 chunks were ordered
> SURFACE OUTWARD (key: the chunk's distance from `World::TerrainHeight` at its
> own column, memoized per column; tie-break the slot index — a pure function of
> the plane and the seed, never of frame time) and issued
> `kGenChunksPerTick = 256` per tick over four ticks. Three consequences were
> handled and all three worked:
>
> - **A placeholder for a slot between the shift tick and its own batch.** Air
>   above the column's ground, `stone` at or below it, chosen on the CPU from
>   `World::TerrainHeight` and installed as a page-table SENTINEL under paged
>   (no page, no memory, no fill) or written by a stub dispatch under dense. The
>   Y band matters: DESIGN.md's "unloaded space is solid and inert" is the right
>   rule at and below the ground, where the neighbouring plane's sand and water
>   are active at the window edge, and the wrong rule above it, where a stone
>   placeholder is a wall of rock 25 m in front of a sprinting player that also
>   blocks the shadow and openness walks. Surface-outward ordering then means
>   the only chunks that ever WEAR a placeholder are the ones it describes
>   correctly.
> - **The occupancy/`genAct` readback moved behind the LAST batch** (recorded in
>   the same command buffer, tracker-derived barrier), with `genList`/`genAct`
>   partitioned into `kGenPlaneRing` per-plane regions addressed by a new
>   `TickParams::genBatch` (low 24 bits the base, bit 31 the stub mode) because
>   a shift lands roughly every tick and four planes are mid-generation at once.
> - **Cancellation.** `InvalidatePendingSlots` already blanked a stale verdict;
>   an un-dispatched batch entry additionally becomes `GEN_SLOT_SKIP` so the
>   kernel generates nothing for a slot a later shift has repurposed. Batches
>   use the CURRENT origin, which is correct precisely because the only slots a
>   later shift re-homes are the ones it has just marked stale.
>
> Gates: `--gate streaming` PASS paged and dense, `--gate determinism`
> `b9e443c7` UNMOVED (that gate never shifts the window), **page faults 0** in
> both modes.
>
> ### The measurement that ended it
>
> `SANDVOX_RUN_EXCLUSIVE=1 --perf --scenario surface-sprint`, 600 frames,
> RTX 3060 Ti. The `worldgen` GPU node's PER-FRAME distribution, read straight
> out of `series.gpu.worldgen` (non-zero entries only — those are the frames
> that ran a dispatch):
>
> | arm | mean (all 600) | p50 | p90 | p99 | max | shift ms |
> |---|---|---|---|---|---|---|
> | baseline `85133c1` | **3.86** | 5.28 | 8.26 | 11.84 | 12.46 | 4.24 |
> | R2, 4 batches of 256 | **4.92 / 4.94** (two runs) | 6.13 | 9.73 | 12.88 | **13.72** | 8.73 |
> | **control: R2 machinery, `kGenChunksPerTick = 1024`** | **3.77** | 5.23 | 7.10 | 10.38 | 11.87 | — |
>
> **The control is the whole finding.** All of R2's machinery — the region ring,
> the entry word, the placeholder pass, the moved readback — with the batch size
> set to the whole plane reproduces the baseline cost to within run noise
> (3.77 vs 3.86; two runs of the 256 arm agree to 0.4%). So the +28% is not the
> placeholders and not the bookkeeping: **it is the four dispatches.** Splitting
> one 1,024-workgroup `worldgenList` into four of 256 costs ~1.4 ms per shift —
> about 350 us per extra dispatch, which is the extra submit, the
> head-of-command-buffer global barrier every command buffer pays
> (vulkan_barrier_graph §3.4) and the ramp/tail of a dispatch that no longer
> fills the machine.
>
> The windowed harness agrees, and it is where the >100 ms tail lives.
> `SANDVOX_RUN_EXCLUSIVE=1 --frames 600 --autofly-surface`:
>
> | | baseline | R2 (256) |
> |---|---|---|
> | frame p50 / p95 / p99 / max | 16.1 / 62.0 / 85.1 / 160.0 | 18.1 / 70.8 / 90.4 / 159.7 |
> | frames > 33 ms | 133 (24.6%) | 155 (28.7%) |
> | window shift | 4.24 ms (fill-gen 0.21, demote 2.26) | 8.73 ms (fill-gen 1.12, gen-batch 0.36, demote 2.14) |
> | **wake-wait** | 0.74 ms, **21 misses** / 340 shifts | 4.05 ms, **78 misses** / 360 shifts |
> | page pool high water | 20,718 (59.5%) | **22,942 (65.9%)** |
> | page faults | 0 | 0 |
>
> ### Why no implementation can rescue it — three independent reasons
>
> 1. **THE MEAN IS FIXED BY THE SHIFT RATE, AND THE SHIFT RATE IS ~1/TICK.**
>    `surface-sprint` measures 0.99 shifts per tick (P3-F §1 measured the same).
>    A plane spread over four ticks means FOUR planes are each contributing a
>    quarter every tick — the same 1,024 chunks per tick the single dispatch
>    produced in one lump. Spreading redistributes work between planes, not
>    between ticks, and at one plane per tick that redistribution is the
>    identity. **The brief's target of `worldgen` 3.6 -> 0.9 ms/frame was
>    arithmetically unreachable from the start.** This should have been checked
>    against P3-F's own 0.99 shifts/tick line before any code was written.
> 2. **THE MAXIMUM DOES NOT MOVE EITHER, AND MEASURED, IT GOT WORSE.** The
>    argument for R2 was the 11.3 ms tree-and-cave plane. But a frame under
>    spreading carries one batch from each of four different planes, so the
>    expensive plane's quarter is added to three other planes' quarters rather
>    than replacing them: the frame total is a plane's worth either way. The
>    measured max went 12.46 -> 13.72 (and the 1-batch control's is 11.87), so
>    the variance-reduction claim is refuted by data as well as by arithmetic.
> 3. **IT UNDOES R4 ON CATCH-UP FRAMES.** R4 caps the window at one SHIFT per
>    frame, which bounds worldgen per frame to one plane. R2's batches are
>    scheduled per TICK — they have to be, or the tick a chunk stops being a
>    placeholder becomes a function of frame pacing and the world hash with it —
>    so a 3-tick catch-up frame issues three ticks of batches from every
>    in-flight plane, up to three planes' worth. That is the exact frame R4
>    exists to protect, and it is why `>33 ms` went 24.6% -> 28.7%.
>
> Two further costs that are fixable but pointless to fix given the above:
> `fill-gen` 0.21 -> 1.12 ms/shift is the CPU placeholder pass (the sort, the
> per-column `TerrainHeight` memo, 768 `SetSentinel`s and their coalesced table
> writes); and moving the readback behind the last batch leaves the T+K poll
> `kWakeLatency - (kGenBatches - 1)` ticks of drain instead of K, which tripled
> the wake-wait misses at three ticks. Buying that back means raising K, which
> is what put pool high water up 2,224 pages.
>
> ### One thing R2 DID get right, and it is worth keeping in mind
>
> Allocating pages per BATCH rather than per plane is strictly better than the
> shift tick allocating all 1,024 at once, and installing a placeholder SENTINEL
> costs the pool nothing at all. That half of the idea is real; it is just
> attached to a dispatch split that costs more than it saves. (`SetSentinel` ->
> `Free` returns a page to `freePages_` DIRECTLY — only the hysteresis free
> probe parks pages in the retire queue — so demoting a plane's worth of slots
> is not a `kPageRetireCeiling` hazard, which is worth knowing for R3.)
>
> ### What to do instead, with the evidence
>
> **Do not generate the sky.** `genChunk` already has a per-column early-out —
> `if (col.biome != B_DESERT && base.y > colTop) { write air; continue; }` in
> `worldgen.wgsl` — and P3-F measured ~586 of a plane's 1,024 chunks as pure
> sky. Those workgroups still dispatch, still run `genColumn` for 256 columns
> and still write 16 KiB of zeros each. A CPU-side skip that installed `PT_EMPTY`
> and left them out of the list would remove **~57% of `worldgenList`** — a real
> ~2 ms/frame, against R2's zero. What it needs is a CPU mirror of `colTop` (the
> top of anything worldgen places in a column: canopy, ruin walls, flora), which
> does not exist today; `World::TerrainHeight` is the GROUND by contract and is
> not it. That mirror is the item, and it is a "two places must agree" pair that
> `check_invariants.py` would have to police the way it already polices the
> height contract.
>
> The other honest reading of P3-F §4 still stands: `worldgenList` is 4.7 ms of
> a 16.7 ms frame and FLAT between a median and a tail frame, so it is not what
> the >100 ms windowed frames are made of. The standing hypothesis there is FIFO
> quantisation amplified by 3-tick frames, and the way to settle it is the ten
> lines in `main.cpp` P3-F §4 names, not another pass at the worldgen kernel.
>
> ### The instrument that shipped, and a pre-existing bug it found
>
> The branch tip keeps one thing: **`--gate streaming` now prints a fold of its
> whole 300-tick hash SEQUENCE (`seq`) and the hash of its FIRST tick (`t1`).**
> `sdet` was a twice-run comparison inside ONE residency mode; nothing in this
> engine was comparing the streamed world across `--residency paged` and
> `--residency dense`, which is the only live oracle the page table has.
>
> It does not agree, and **it did not agree before this package either**:
>
> | tree | paged `seq` | dense `seq` | `t1` (both modes) |
> |---|---|---|---|
> | `85133c1` (baseline) | `f23ebbe9` | `397cc3e2` | — |
> | branch tip | `f23ebbe9` | `397cc3e2` | matches |
> | R2 `78feb6c` | `77e93536` | `c452ed25` | `dc2eb1c2` (matches) |
>
> **`t1` MATCHES across modes and `seq` does not**, which places it: tick 1 is
> one CA tick after a fresh `SubmitWorldgen` with the window at the origin and
> before `stream.Update` has moved the player a whole chunk, so no shift has
> happened yet. The divergence is in the SHIFT path, not in worldgen or in
> materialization. It is not R2's — both trees show it — and it is not a
> determinism failure (each mode is reproducible against itself; that is what
> `sdet` says). It is the paged/dense oracle being broken for streaming, and
> nobody could have known because nothing printed a comparable number. The next
> step is a per-tick sequence dump behind an env var and a diff, which will name
> the first divergent tick in one run per mode.


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

> **CORRECTION (2026-09-03).** That hoist had ALREADY LANDED when this was
> written, in `3fdcf5c` — `TreeCands` / `treeCandsInto` / `treeFromCands` in
> `worldgen.wgsl`. Both this paragraph and the plan's item 3 were repeating a
> stale to-do; the plan now says so at length, with what was really left.
>
> The tail was attacked anyway, on branch
> `worktree-agent-a5f7f4663644acfa6` @ `81b3769`, by hoisting four other things
> the column or the dispatch already knew (the two authored-POI `baseHeight`
> anchors, which `genCellIn` was re-deriving for EVERY CELL IN THE WORLD; the
> tree scan above `treeMaxTop()`; the upper-stalk `flowerAt`; and a per-column
> sky early-out). Measured on the `worldgen` GPU node of
> `--perf --scenario flythrough`, two interleaved pairs: **mean per dispatch
> -17.6% / -18.8%, max 18.4 -> 11.6 ms and 11.7 -> 7.0 ms (-37% / -40%)**, world
> hash unmoved. Numbers and method in PLAN_surface_flight_perf item 3.
>
> **The instrument, since §0 above no longer has one.** R1 removed the fence, so
> the `pre`/`occ` split in `stream.cpp` is gone and there is no per-shift
> worldgen timing on `--autofly-surface` any more. Use
> `--perf --scenario flythrough`: `series.gpu.worldgen` in the perf JSON is one
> GPU-timer entry PER FRAME for the `worldgen`+`worldgenList` passes, so the
> non-zero entries are the per-dispatch distribution directly. Do not read the
> whole-frame p50 for this — worldgen is under 1 ms of a 17-19 ms frame and the
> scenario is real-time-paced, so the frame histogram moves with machine state
> by more than the whole of this kernel.

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

> ## ANSWERED 2026-09-03 — a third of the pool is pages the free path can never revisit
>
> Instrument: `PageCensus` in `src/sim/pagetable.h`/`.cpp` — every RESIDENT page
> billed to exactly one mutually exclusive reason (the buckets sum to
> `resident`, and the printer reports the residual so a future contributor
> cannot hide in one), plus a height band and a per-tick free-path event
> tally. Three exposures: `SANDVOX_PT_DEBUG=1` prints it every
> `SANDVOX_PT_CENSUS_EVERY` ticks (default 120), `--frames` prints it at its
> high-water tick and at exit, and five `PerfSample` counters
> (`pagesHeldDirty` / `pagesHeldMatter` / `pagesHeldEmpty` / `pagesHeldOrphan`
> / `pagesRetired`) carry it over `--telemetry` so the LIVE session can be read
> the same way. The first four partition `pagesResident`.
>
> **The paragraph above was wrong about two things before it was wrong about
> the cause.** (a) "not another autofly run" — `--frames 1200
> --autofly-surface` reaches **29,901 of 34,816 (85.9%)**, i.e. ABOVE the live
> 83%; the doc's 65% / 74% figures were 600-frame runs and the number had not
> stopped climbing. (b) The §2 LANDED note's "+3,179 pages, 65% → 74%" is
> therefore not the ceiling of what K=4 costs, it is what it cost in 600
> frames.
>
> ### The measurement
>
> | run (`SANDVOX_PT_DEBUG=1`) | pool high water | at exit |
> |---|---|---|
> | `--frames 1200 --autofly-surface` | 29,901 (85.9%) @ tick 387 | 25,025 (71.9%) |
> | `--frames 1200 --autofly-hard` | 26,144 (75.1%) @ tick 100 (the worldgen settle) | 4,928 (14.2%) |
> | `--frames 2400 --autofly-park` (`SANDVOX_PARK_SETTLE=1500`) | 23,413 (67.2%) @ tick 338 | 23,104 (66.4%) |
>
> Census at `--autofly-surface`'s peak (tick 387, 29,901 resident): dirty
> 12,875 (43.1%) | ring 4,484 (15.0%) | full 7,907 (26.4%) | matter 1,192
> (4.0%) | waiting 324 | **orphan 3,119 (10.4%)**. So 58% of the PEAK is the
> conservative mirror and its N26 ring, not matter — but that is a FLIGHT
> number and it decays to nothing when the player stops.
>
> **`--autofly-park` is the one that answers the live session, because it is
> the only arm that is stationary.** It flies 300 ticks, parks, and then:
> residency **plateaus at 23,104 (66.4%) and does not move again for 1,000+
> ticks**. At the plateau `dirty 0 | ring 0 | waiting 0 | cand 0` — the mirror
> is empty, the free path has nothing queued, nothing is in flight — and the
> 23,104 pages are:
>
> ```
>   full   9,198 (39.8%)  +  matter 5,746 (24.9%)  = 14,944 real matter
>   ORPHAN 8,160 (35.3%)                            = all air, never reclaimable
>   bands  sky 7,883 (all-air 7,883) | surface 2,038 | buried 13,183
> ```
>
> **A third of a stationary window's resident pages are all-air pages over open
> sky that the free path will never look at again.** Residency does NOT come
> down after parking. It cannot.
>
> ### The mechanism, exactly
>
> `PageTable::ConsumeOccupancy`'s free trigger is an EQUALITY:
>
> ```cpp
> if (zeroStreak_[s] != kPageFreeTicks) continue;   // 8, exactly
> ```
>
> and the only thing that keeps a candidate eligible when it is not freed this
> tick is `zeroStreak_[candidates[i]] = kPageFreeTicks - 1`, which lives INSIDE
> `if (probeSubmit_ && !candidates.empty() && !probePending_) { ... }`. The
> deferred word probe submits on tick N and harvests on N+1; whenever that map
> has not landed, `probePending_` is still true, the whole block is skipped,
> and **this tick's entire candidate set keeps a streak of exactly 8, steps to
> 9 next tick, and can never satisfy `== kPageFreeTicks` again for the life of
> the process.** Measured over the census samples of the surface run: 2,341
> candidates, 384 submitted, 91 freed, 439 correctly deferred by the cap, and
> **1,518 (65%) stranded by `probe-busy`**. Two smaller paths strand the same
> way (a vanished page and the slot-identity mismatch at harvest also `continue`
> without re-arming); they were 0 and 3.
>
> ### What is NOT the lever, now that each has a number
>
> - **The probe cap (`kPageFreeProbesPerTick` = 128) is not it.** It re-arms
>   correctly; `capped` slots come back the next tick. It is a rate limit, and
>   the doc's suspicion that it is "a plausible mechanism for residency
>   creeping up and never coming back down" is refuted: what never comes back
>   down is the stranded set, which the cap does not produce.
> - **The stain bit is not it.** `stain 0` in every census of all three runs.
> - **The particle flight shell is not it.** `shell 0` throughout.
> - **cpuDirty dilation is not it, for a STATIONARY session.** It is the single
>   largest bucket while flying (dirty+ring = 58% of the peak) and it is worth
>   attacking on its own account, but it decays to literally zero within ~100
>   ticks of parking, so it cannot be what the user's stationary 29k is made of.
> - **Retire quarantine is not it.** `retired` is 0-800, and 0 at the plateau.
>
> ### So what are the live session's ~29,000 pages
>
> Real matter (the harness parks at ~14,900; a lived-in surface window with
> lakes, trees and structures will be higher) plus an ORPHAN set that only ever
> grows. The strand bucket is monotonic — nothing removes a member — so its
> size is a function of how much flying and activity the session has done. The
> harness accumulated 8,160 of them in 300 ticks of flight. Two hours is
> ~216,000 ticks. 29,063 is arithmetically unremarkable, and the reason it
> "creeps up and never comes back" is that it literally cannot come back.
>
> ### The fix: TRIED, MEASURED, AND REJECTED — it loses voxels
>
> The three-line fix is obvious and it does not work, and WHY it does not work
> is the more useful half of this section. Do not re-attempt it as written.
>
> **What was tried.** `ConsumeOccupancy`'s trigger widened from
> `zeroStreak_[s] == kPageFreeTicks` to `>=`, the now-redundant re-arm loop
> deleted, and the candidate list capped at collection so the eligible set does
> not become a 32k-entry vector per tick. It does exactly what it says: frees
> over `--gate streaming` went from a handful to **24,179**, i.e. the backlog
> drains.
>
> **What it costs.** `--gate streaming` reports **21,696,512 page faults —
> `*** SENTINEL WRITES LOST VOXELS ***`, lost lava.** That is 5,296 chunks'
> worth of dropped stores (21,696,512 = 5,296 x 4,096), against 0 on the tree
> that ships here. A change that trips the one counter this engine treats as
> "0 is the only acceptable value" does not land, however good its residency
> number is.
>
> **Two further guards were tried and neither touched it**, which is the
> finding:
>
> 1. Re-arming `zeroStreak_` in `EnsurePageForOverwrite` (residency BEGINS
>    there for every streaming refill / genList slot / worldgen batch, and
>    `Materialize` already re-arms at its own sentinel->page transition, so this
>    is a real omission worth fixing on its own terms — it just is not this bug).
> 2. Adding the snapshot's own `dirtyFlags[slot] == 0` as a second conjunct at
>    selection — the same belt-and-braces the deferred-wake shift adopted for
>    its sky demote.
>
> All three binaries reported **the identical 21,696,512** and the identical
> 24,179 frees. The census's new `snap-dirty` counter says why guard 2 was
> inert: **0 refusals on every sample** — the snapshot's dirty flags are clear
> for every slot the free path selects. The chunks are genuinely clean when the
> snapshot is stamped and are woken afterwards, by something the CPU mirror
> never hears about.
>
> **So the strand was load-bearing.** Two things were true at once and only the
> first was known:
>
> - `hasMatter` in `Materialize` is "a resident page OR a non-air sentinel" —
>   **a resident ALL-AIR page counts**. So every stranded page was widening the
>   materialization set by its own 26-ring, every tick, for free. Freeing them
>   shrinks that set.
> - The free path's only real guard is `!cpuDirty`, and `cpuDirty` is a superset
>   of the writes the **CPU caused**, not of `dirtyIn`. The GPU wakes chunks the
>   mirror never hears about, and **freeing a page does not clear the GPU's
>   dirty bit**. The equality trigger made eligibility a one-tick window a slot
>   got once in its life, so that hole was almost never reached.
>
> The leak and the mirror hole are therefore the same bug seen from two ends,
> and the leak is the one that is currently holding the world together. **The
> `>=` change must land WITH a repair to `cpuDirty ⊇ dirtyIn`, not before it** —
> and that repair is in `stream.cpp` / the deferred wake, not in `pagetable.cpp`.
> Sizing K against §6 should be redone after both.
>
> **One reporter defect found on the way, worth fixing whoever gets there
> first:** `selftest.cpp`'s page-fault report decodes `pageFaults[1]`/`[3]` (the
> highest and lowest refusing SLOT) through `SlotToWorldChunk` **at report
> time**, i.e. with whatever window origin the run ended on. After any window
> shift those printed chunk coordinates are fiction. They cost an hour here:
> they appeared to name chunks nowhere near anything the free path had touched,
> which is what the freed-slot attribution (`[pt] tick N FREED slot ... chunk
> ...`, added and then reverted with the rest of the attempt) eventually
> contradicted. The fault path should record the world chunk key, not the slot.
>
> ### What the harness still cannot reproduce
>
> Brush/spell/explosion ops, mobs, fire, day/night `EncodeWakeAll` (which
> unions all 32,768 slots into the mirror and materializes every hasMatter
> chunk at once, a few times per in-game day), store-hit refills on revisited
> terrain, and TIME — 1,428 ticks is 48 seconds against two hours. Every one of
> those adds to the strand bucket and none of them removes from it, so the
> harness numbers are a FLOOR for the live session, not an estimate of it.

> ### P3-E. FIXED 2026-09-03 - and the strand was hiding an upload bug, not covering for one
>
> **Branch** `worktree-agent-ad3bffc9a478973bc` (commits 45e6be8 attribution,
> 5761a2e the hole, ccf8357 the leak, 44dc1e7 census, 05a3e8f harness).
>
> The section above is right about the mechanism and right that the three-line
> fix loses 21.7M voxels. It is wrong about one inference, and that inference is
> what made the fix look impossible: **"the CA writes into chunks the mirror
> never hears about" was never demonstrated.** It was inferred from a fault
> count with no writer attached. The mirror is fine. The CA never faulted once.
>
> #### The attribution, which took one run
>
> `pageFaults` grew a per-kernel tally, a world chunk resolved INSIDE `voxStore`
> at fault time, the tick, and the page-table entry that refused. One
> `--gate streaming`:
>
> ```
> by kernel: worldgen:list 13656064, worldgen:pagefill 8077312
> FIRST worldgen:pagefill chunk (432,160,352) word 0x00000001 entry 0xc0000001
> ```
>
> 3,334 and 1,972 WHOLE chunks of 4,096 cells; `0xc0000001` is JITTER(stone),
> the sentinel each slot held *before* the CPU allocated its page. Two CPU-side
> preconditions added in the same commit - every genList slot and every
> jitter-fill slot must hold a page when its list is built - fired **zero**
> times. The pages existed; the GPU never heard about them.
>
> #### The cause: creating a command encoder is what drains the upload queue
>
> `Backend::FlushUploads` runs from `BeginCommands`, i.e. at command-buffer
> CREATION, not at submit. So `CreateCommandEncoder()` consumes the pending
> upload queue, and a caller that creates an encoder and then decides it has
> nothing to record DELETES every write issued before it.
>
> That caller is §3.6's free-confirmation probe: it created its encoder, found
> that none of its 128 candidates still had a page, and returned without
> submitting - taking `PageTable::Materialize`'s page-table flush for the whole
> tick with it. **Under the `==` trigger the probe almost never ran, so the
> window was almost never open.** That is the real sense in which the strand was
> "load-bearing": not that the orphan pages widened the materialization set
> usefully, but that the leak kept the free probe idle and so kept this latent
> bug unreachable.
>
> Fixed at the class, not the instance: an encoder dropped without `Finish()`,
> or a finished `CommandBuffer` dropped without `Submit()`, hands the swallowed
> uploads BACK to the front of the queue (`Backend::AbandonCommands`). The
> handle owns the debt, so its destructor settles it - exact, where a "was the
> previous one submitted yet?" test at the next `BeginCommands` cannot tell a
> dead encoder from a live second one.
>
> #### Then the leak, unchanged in shape from the section above
>
> `>=` instead of `==`; the re-arm DELETED rather than hoisted (under `>=` there
> is nothing to re-arm); the eligible set bounded at collection to
> `kPageFreeProbesPerTick` with the scan start rotating by that same amount per
> tick so the cap cannot starve the tail; `EnsurePageForOverwrite` re-arms
> `zeroStreak_` (residency begins there). `kPageRetireCeiling` is untouched -
> freeing is still capped at `kPageFreeProbesPerTick`/tick, which is what
> `kPoolPages` is sized against.
>
> #### Measured, after
>
> | run | before | after |
> |---|---|---|
> | `--frames 2400 --autofly-park` PLATEAU | 23,104 (66.4%), **orphan 8,160** | **16,308 (46.8%), orphan 0** |
> | ... of which all-air sky pages | 7,883 | **186** |
> | `--frames 1200 --autofly-surface` high water | 29,901 (85.9%) | 22,804 (65.5%) |
> | `--frames 1200 --autofly-hard` high water | 26,144 (75.1%) | 24,437 (70.2%) |
> | `--gate streaming` page faults | 0 (leaky) / 21,733,376 (`>=` alone) | **0** |
>
> **The plateau is the number that matters and it is the one that moved.** A
> parked window is now `full 12,017 + matter 4,105`, with `dirty 0 | ring 0 |
> ORPHAN 0` - i.e. real matter and nothing else. The 8,160 all-air pages the
> section above measured are gone, and residency comes down after parking, which
> it previously could not.
>
> **The flight PEAKS barely move, and that is the honest reading**: 58% of the
> peak was `dirty + ring` (the conservative mirror and its N26 dilation), which
> the leak fix does not touch. The binding constraint during flight is now the
> drain RATE, visible as `cands 4923 -> submitted 128, capped 4795` in the
> census: eligibility is no longer the problem, the 128/tick probe budget is.
> That is a separate, well-posed item with a number attached, and it did not
> exist as one before.
>
> **Caveat on the peaks specifically.** The autofly arms are FRAME-budgeted, so
> how many ticks a `--frames N` run reaches varies with machine load (this
> surface run reached tick 797; the "before" note's reached 387). Residency is a
> function of ticks travelled, so cross-run PEAK comparisons are soft. The
> plateau is not - it is a fixed point, held for 1,000+ ticks, not a transient.
>
> **One reporter defect from the section above is fixed**: selftest.cpp no
> longer decodes the refusing SLOT through `SlotToWorldChunk` at report time.
> The record carries the world chunk resolved at fault time instead.

---

## P2-D. The snapshot-staleness stall under a deep GPU queue — LANDED 2026-09-03

**Branch:** `worktree-agent-a88855b327636dc1f` off `streaming-smooth` @ 82bf233.
**Item:** `PLAN_surface_flight_perf.md` B3, "replace the `WaitIdle` staleness
drain with a bounded catch-up". **Files:** `src/sim/world.h` (`kReadbackSlots`),
`src/sim/world.cpp` (`EncodeReadbacks`), `src/test/support.{h,cpp}`
(`SubmitTick` attribution + fallback), `src/main.cpp` (summary line, tick cap),
`scripts/check_invariants.py` (new `ringdepth` check).

### 0. The attribution, before anything was changed

Rule 6: the existing report was a bare count — "snapshot stalls: 136, ring
refusals: 119". Splitting it took one build and one run, and the answer was not
the one the item was written for.

`--frames 600 --autofly-surface`, 540 measured frames, integration tree plus
counters only:

```
snapshot stalls: 157 over 540 frames (26.1%) | ring refused: 128
  cause: refused 83 (no copy encoded for the tick) | not-landed 74 (queue depth)
  arm:   map-wait 157 (157 fences, 3564.9 ms) | WaitIdle 0 (0.0 ms, 0 futile)
  staleness at stall (ticks): 5:157  max 5
```

Three findings, in order of how much they change the plan:

1. **The `WaitIdle` arm never fires.** B3's premise — a full device drain on the
   frame path — was already fixed by the deferred-wake commit's targeted
   `WaitOldestPendingMap` loop, and the counter that reads "blocking WaitIdle on
   the frame path" was mis-labelled: it counts every stall, not the drain. What
   remained was **one fence wait per stall**, 157 of them, 22.7 ms each.
2. **The staleness histogram is degenerate: always exactly 5.** The check runs
   every tick and the gap can only grow by one per tick, so it fires the instant
   it crosses `kPagedSnapshotMaxGap = 4` and never gets further. "How stale" was
   never the question; "how far behind is the GPU" is.
3. **53% of stalls were on a tick for which no copy had been encoded at all.**
   That is the ring being too short, not the GPU being slow — a distinct cause
   with a distinct fix, and invisible in the aggregate count.

`WaitIdle` being unreachable is structural, not lucky: the loop is bounded by
the ring depth, the ring is the only producer of `MapReadAsync` maps, so it
cannot exit on the bound with maps still outstanding. It can only exit stale
when every map has been delivered — and then `WaitIdle` waits for *work*, which
is not what is missing. It is now gated on "maps remain" and the leftover case
is counted as `proceeded stale` (0 in every run since).

### 1. The fix: size the ring from the pipeline, not from a literal

`kReadbackSlots` was `3`. A slot is held from the tick that encodes the copy
until the submit that produced it retires, so the ring must cover every tick
that can be in flight: `kMaxTicksPerFrame * (kFramesInFlight + 1)` = 4 x 4 =
**16**. `main.cpp`'s tick cap now *is* `World::kMaxTicksPerFrame` (one
definition), and `check_invariants.py`'s new `ringdepth` check compares
`kFramesInFlight` against `rhi_vulkan.h`'s `kAcquireSlots` — world.h cannot
include a backend header to read it, and an unchecked mirror is what this
script exists for.

**Memory:** one slot is 1,997,056 B = 1.904 MiB (27 x 16 KiB mirror + 128 KiB
dirty + 128 KiB occupancy + 128 KiB support + 108 KiB fluid mirror + ~2 KiB of
small tables + **1 MiB of `kFetchPerTick` chunk fetches**, the biggest single
term). 16 slots = **30.5 MiB**, up from 5.7 MiB, against a 360 MiB page pool.

Two knobs stay, permanently, because both questions here are cost questions and
a cost question deserves a run rather than a rebuild:

- `SANDVOX_READBACK_SLOTS=n` caps how many slots may be occupied, so the 3-slot
  and 16-slot behaviours are two arms of **one binary** (the "an `#if`-guarded
  SIMD path tests only itself" rule).
- `SANDVOX_SNAP_MAXGAP=n` overrides `kPagedSnapshotMaxGap`. The long "raising
  this is a trap" note in `support.cpp` ends with "the honest way to raise it is
  to measure first"; this is that measurement, at one run per candidate.

### 2. The measurement

Four arms of one binary, `--frames 600 --autofly-surface`,
`SANDVOX_RUN_EXCLUSIVE=1`, interleaved A-B-C-D twice (the route is
dt-integrated; single runs are +-15%, and the first "before" run of the session
— taken while the machine was loaded — read p50 22.8 / 157 stalls against the
same configuration's 18.6 / 46.5 when quiet, which is exactly why).

| arm | frame p50 / p95 / p99 / max | >33 / >100 | readbackStall mean / p99 | stalls | refusals | pool HW |
|---|---|---|---|---|---|---|
| **A** ring 3, gap 4 (before) | 18.6 / 70.0 / 87.3 / 162.9 | 157 / 2.5 | **1.84 / 42.4** | **46.5** | **70** | 21,126 (60.6%) |
| **B** ring 16, gap 4 (shipped) | 18.6 / 69.7 / 86.9 / 246.9 | 150 / 1.5 | **0.43 / 12.9** | 30.0 | **0** | 20,732 (59.5%) |
| C ring 16, gap 6 | 18.7 / 67.8 / 82.0 / 113.7 | 148 / 1.0 | 0.00 / 0.0 | **0** | 0 | 21,545 (61.9%) |
| D ring 16, gap 8 | 18.3 / 70.2 / 88.2 / 142.0 | 148 / 3.0 | 0.00 / 0.0 | 0 | 0 | 22,463 (64.5%) |

Control, `--frames 600 --autofly-hard` (the residency worst case), A vs C
interleaved twice — and note `maxGap` is **inert** in this arm, because neither
side stalls at all, so the whole difference is ring depth:

| arm | frame p50 / p95 / p99 | stalls | pool HW |
|---|---|---|---|
| A ring 3 | 8.7 / 26.5 / 48.2 and 8.8 / 25.2 / 48.4 | 0, 0 | 27,201 (78.1%) and 26,339 (75.7%) |
| C ring 16 | 8.9 / 28.1 / 46.1 and 8.7 / 28.3 / 46.3 | 0, 0 | **24,500 (70.4%) and 24,244 (69.6%)** |

Page faults 0 in every run (`--gate streaming` under both residencies, and a
`SANDVOX_PT_DEBUG=1 --autofly-hard` pass).

### 3. What the numbers say, including the part that argues against the item

- **The ring depth is the whole of the defensible win.** It takes refusals from
  70 to **zero**, `readbackStall` p99 from 42.4 ms to 12.9 (3.3x), stalls from
  46.5 to 30 — and, unexpectedly, takes the adversarial arm's page-pool high
  water **down 5-8 points** (78.1/75.7% -> 70.4/69.6%). The mechanism is §3.2
  read backwards: a deeper ring lands a snapshot on more ticks, so
  `TightenFromSnapshot` runs at a smaller gap, so `cpuDirty` stays tighter and
  fewer chunks get materialized. A short ring was costing pages, not saving
  them.
- **Raising `kPagedSnapshotMaxGap` was NOT taken.** Gap 6 does remove the last
  30 stalls, and it does not blow up the pool (61.9% vs 60.6%, inside this
  route's run-to-run spread) — so the trap note's arithmetic is not wrong, it is
  just not binding at 6. It buys 0.43 ms/frame of CPU that this route does not
  spend on the critical path, in exchange for loosening a documented
  freshness bound on two samples. Not worth it. **The knob and this table are
  the record, so the next person pays one run, not one rebuild.**
- **The honest headline: removing the stall did not move frame time.** p50/p95/
  p99 are statistically identical across all four arms (18.3-18.7 / 67.8-70.2 /
  82.0-88.2). The engine is GPU-bound at ~21 ms of `present`, and the stall was
  CPU time overlapping the wait it would have paid at the swapchain anyway. The
  stall was genuinely 52% of CPU *busy* and genuinely not on the critical path;
  those are compatible statements and only the second one is about framerate.
  What the fix removes is a **CPU-side blocking wait that gets worse exactly
  when the machine is loaded** — the loaded-machine run measured 157 stalls
  against a quiet 46.5 — plus the pool-residency cost above, which is the one
  that has an abort at the end of it.

### 4. Determinism

Hash-neutral by construction: the ring depth and the staleness ceiling change
only *when the CPU blocks* and *which chunks are in the materialize set*, both
of which are derived, unhashed, unsaved data. The harness path
(`HarnessSnapshotDrain`) is untouched and still drains per tick at gap 0.

- `--gate streaming` **paged and `--residency dense` are identical**: "hash
  sequences match over 227 shifts, ball chunk evicted=1, 76 glass voxels after
  re-entry, player crossed=1, store 2527 chunks" on both, page faults 0 on both.
- `--gate determinism` on this branch is `b9e443c7`, matching the unmodified
  `streaming-smooth` @ 82bf233. **That is not the pinned `5c5e3236`** — the
  integration branch already carries a hash move (suspected upload-ordering in
  the merged rhi staging-ring change, being diagnosed elsewhere and NOT from
  this package). The claim made here is only that this package moves nothing
  relative to the tree it branched from.

### 5. Found on the way — a build-correctness hazard, not a perf finding

**`build/.ninja_deps` in a Ninja+sccache worktree is nearly empty** (472 KB, 7
`world.h` entries for a 100-TU target): sccache swallows MSVC's `/showIncludes`,
so ninja tracks almost no header dependencies. A `world.h` edit here rebuilt
only the 30 TUs that include `support.h`; `world.cpp.obj` stayed stale, and the
link produced a binary with `kSlots = 3` in one translation unit and `16` in
another — the World object layout differing across TUs, silently. Nothing in the
build output says so.

**After any header edit in such a worktree, `rm -rf
build/CMakeFiles/sandvox_core.dir` before `build.sh`.** sccache makes the
rebuild ~30 s (62 of 101 TUs came straight from cache). The tell is the build
log's `Building CXX` count: 30 after a `world.h` change is wrong, ~100 is right.
This is the "header edit during background build -> mixed struct layouts" trap
with a new cause, and it applies to every agent using this generator.

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


---

## P3-F. The sprint tail, attributed: it is not the streaming path — LANDED 2026-09-03

**Branch:** `worktree-agent-ae8b16bb4a02aa534` off `streaming-smooth` @ 840e63f.
**Files:** `src/measure/perfsuite.cpp` (the `surface-sprint` scenario + real-time
pacing), `src/measure/perfnodes.h` (7 counters, 3 non-pass spans, one
attribution entry point), `src/test/support.{h,cpp}` (the spans themselves),
`src/gpu/passtimer.{h,cpp}` (ring depth + a collector form of `PollDeferred`),
`scripts/check_invariants.py` (`autofly` check), `scripts/p3f_analyse.py`.
**Hash-neutral:** `--gate determinism` reads `b9e443c7`, unchanged; `--gate
streaming` passes paged; `--perf`'s own timer-neutrality gate reports
`cd34055d untimed vs cd34055d timed — IDENTICAL` with the new spans attached.

### 0. What was actually missing: the streaming path had NO GPU BILL AT ALL

Not "unattributed" — **unrecorded**. `PageTable::DrainFills` issues one
`vkCmdFillBuffer` per page materialized from an `EMPTY`/`UNIFORM` sentinel, the
free-confirmation probe issues its own copies on its own submit, and
`EncodeReadbacks` copies ~1.9 MiB out per tick. None of those are
`pass_table.def` rows, so the barrier generator never sees them, so
`PerfNodeForPass` cannot name them — and an untimed command produces no
`PassSample`, so it does not even land in the `unattributed` bucket the page
prints a warning for. Time that is invisible to the one report designed to
catch invisible time is the worst case there is, and it is exactly where
hypothesis (a) lived.

Three timestamp spans now bracket them, on the same `PassTimer` the pass table
uses and resolved by the resolve `EncodeTick` already encodes:
`pageFillCmd` -> `pageTable`, `freeProbeCopy` -> `pageTable`, `readbackCopy`
-> `readback` (names in `kPerfRenderSpans`, which is no longer only render
spans). `PerfNodeForTimedName` is now the ONE lookup for both populations, so
the next hand-written span cannot be silently dropped by a caller that picked
the wrong table.

**One hazard found writing them, worth stating because it is a hang and not a
wrong number.** `EncodeResolve` resolves `[0, used_)` with
`VK_QUERY_RESULT_WAIT_BIT`, so every query it covers must have been WRITTEN by a
command buffer that is actually submitted. The free probe's first version
allocated its pair and then took the `copied == 0` early return without
submitting; the tick's resolve would then wait forever on a query nothing wrote.
The plan is built before the span is opened for that reason alone.

### 1. The scenario: `--perf --scenario surface-sprint`

`--frames 600 --autofly-surface` as a recorded, attributed run.
`flythrough` cannot see this tail and it is not a subtle difference: it descends
at 1.5 vox/tick against sprint's 10.8, and it runs exactly one tick per frame.

Faithful to `src/main.cpp`: fly mode, forward + sprint, `player.flySprint /
kVoxelMeters` along `Camera::Forward()` at the game's own default yaw 2.35 /
pitch -0.2, altitude pinned analytically to `World::TerrainHeight` + 35 / + 150
voxels alternating on `tick / 90 & 1`. The two clearances are restated in
`perfsuite.cpp` and compared against `main.cpp` by
`check_invariants.py --autofly`, because a copy that drifts measures a
different flight while looking identical on the page. It advances per TICK
rather than per frame, which is what keeps the world a pure function of the
tick counter under a pacing scheme whose frame count is not.

**Real-time paced, and the pacing took two corrections that are the useful part
of this section.**

1. **No pacing model at all -> the scenario did not exist.** First run: 133 sim
   ticks over 600 frames (0.22/frame, against the game's ~0.6), 4 active chunks
   against `--autofly-surface`'s ~550. `dt` IS the frame time and the offscreen
   harness's frame is ~4.8 ms, so the accumulator barely filled. `FramePacer`
   already reproduces the swapchain's frames-in-flight bound explicitly because
   there is no swapchain; the present interval needed the same treatment.
2. **A FLOOR is not FIFO. FIFO QUANTISES, and the difference is the whole of
   hypothesis (b).** With `dt = max(work, 1/60)` the run reached 322 ticks over
   600 frames (0.54/frame — the right cadence) and `ticksThisFrame` never once
   exceeded 1: floored, a 17 ms frame contributes 17 ms and the feedback loop
   has no gain. Under FIFO that frame is displayed on the *second* refresh
   boundary and contributes 33.3 ms, which is what buys the next frame an extra
   tick, which costs more GPU, which crosses another boundary. Shipped:
   `dt = ceil(work / period) * period`, bookkeeping only — nothing sleeps, so
   `wallMs` stays the frame's WORK. `SANDVOX_PERF_VSYNC_HZ` is the knob (0
   disables), so the three arms above are one binary.

The scenario reaches the game's cadence: **474 ticks over 600 frames
(0.70/frame), 468 window shifts = 0.99 shifts/tick** (the windowed harness
measures 0.68), `activeChunks` p50 124 / p95 ~420 / max 498 against
`--autofly-surface`'s ~550 p50. It does NOT reach the game's frame magnitude —
see §4, which is a finding rather than an excuse.

### 2. The attribution table

`SANDVOX_RUN_EXCLUSIVE=1 --perf --scenario surface-sprint`, 600 frames, RTX
3060 Ti, 1920x1080, paged. Split on the 418 frames that ran a tick (the 182 that
ran none are a real part of a 60 Hz / 30 Hz cadence but comparing them to a
ticking frame answers nothing). `unattributed: 0 ns, no names.`

| | median frame | worst 5% | delta |
|---|---|---|---|
| **wall** | 16.73 | 24.67 (max 27.37) | +7.9 |
| gpu raymarch | 8.11 | 11.69 | **+3.58** |
| gpu worldgen (`worldgenList`) | 4.59 | 5.29 | +0.70 |
| gpu caLoop | 1.66 | 2.19 | +0.53 |
| gpu openness | 1.02 | 1.30 | +0.27 |
| gpu farField | 0.02 | 0.15 | +0.13 |
| gpu shadowCache | 0.61 | 0.64 | +0.03 |
| gpu occupancy / fluidSys / compact | 0.46 | 0.48 | +0.02 |
| **gpu readback** (`readbackCopy`) | **0.043** | **0.047** | +0.004 |
| **gpu pageTable** (`pageFillCmd` + `pageFill` + `freeProbeCopy`) | **0.087** | **0.051** | **-0.036** |
| **GPU TOTAL** | **16.60** | **21.83** | **+5.23** |
| cpu present (the pacer's frames-in-flight wait) | 13.92 | 21.68 | +7.76 |
| cpu stream | 1.62 | 1.72 | +0.10 |
| cpu pageTableCpu | 0.59 | 0.62 | +0.03 |
| cpu everything else | 0.56 | 0.61 | +0.05 |
| ticks this frame | 1.00 | 1.00 | **0.00** |
| window shifts this frame | 1.00 | 1.00 | **0.00** |
| shift wake-wait ms | 0.00 | 0.00 | **0.00** (0 misses in the whole run) |
| snapshot stalls | 0 | 0 | 0 |
| page fills (EMPTY/UNIFORM, = the fill COMMAND count) | 180 | 192 | +12 |
| page fills (JITTER, dispatch slots) | 44 | 28 | -16 |
| page fill bytes | 3.68 MB | 3.60 MB | -0.07 MB |
| cpuDirty chunks | 4,354 | 5,932 | +1,578 |
| active chunks | 124 | 201 | +77 |
| pages resident | 20,338 | 20,486 | +148 |

Per-pass, whole run (us per frame, 600 frames):

| pass / span | us/frame | events | node |
|---|---|---|---|
| `worldgenList` | **3,625.9** | 468 | worldgen |
| `ca` | 1,238.2 | 471 | caLoop |
| `opennessDirty` | 536.3 | 474 | openness |
| `shadow_resolve` | 437.0 | 658 | shadowCache |
| `opennessRefresh` | 269.6 | 474 | openness |
| `readbackCopy` *(new)* | **33.0** | 473 | readback |
| `freeProbeCopy` *(new)* | **27.4** | 193 | pageTable |
| `pageFillCmd` *(new)* | **17.4** | 474 | pageTable |
| `pageFill` (JITTER dispatch) | 12.0 | 409 | pageTable |

### 3. The hypotheses, with numbers

- **(a) thousands of 16 KiB `vkCmdFillBuffer`s + the JITTER dispatch — KILLED,
  by two orders of magnitude.** The count was never thousands per tick: 180
  fills on a median ticking frame, 192 on a tail frame, 987 at the run's
  maximum. And `pageFillCmd` costs **17.4 us per frame** — 0.1% of a 16.7 ms
  frame — with the JITTER dispatch a further 12.0 us. The whole `pageTable` GPU
  node is 0.087 ms median and goes DOWN in the tail. This was the leading
  suspect and it is not a contributor at all.
- **(b) four ticks stacking inside one frame — REAL IN THE GAME, STRUCTURALLY
  UNREACHABLE HERE, and the arithmetic is the useful part.** `kTickDt` is
  1/30 s and the refresh period 1/60 s, so the accumulator is spent the moment
  it crosses 33.3 ms: **a frame must exceed FOUR refresh periods (>50 ms of
  work) before it can owe two ticks.** This harness's worst frame is 27.4 ms —
  two periods — so `ticksThisFrame` is 1.000 on the median frame and 1.000 on
  the tail frame. The windowed harness's 11-13 frames over 100 ms are 6+
  periods and DO owe 3 ticks; the loop is real, it is just gated behind a frame
  cost this scenario does not reach (§4).
- **(c) the deferred wake's T+K miss — DID NOT FIRE ONCE.** 0 wake-wait misses
  and 0.00 ms of wake-wait over 468 shifts, against the brief's 68 misses / 2.3
  ms mean over 367 shifts in the windowed harness. Two readings are possible and
  they are not distinguished here: the harness pumps `ProcessEvents` every frame
  with nothing else competing, so K=4 ticks is always enough; or the windowed
  run's misses are a consequence of the long frames rather than a cause of them.
  The second is the more likely given (b) — a 100 ms frame is 3 ticks of shift
  in one burst — and it is testable by correlating `shiftWakeWaitMs` against
  `ticksThisFrame` in a run that reaches the band.
- **(d) eviction / demote / free-probe / snapshot copy bandwidth — KILLED.**
  `freeProbeCopy` 27.4 us/frame, `readbackCopy` 33.0 us/frame. `stream` CPU is
  1.62 ms median / 1.72 ms tail and its shift breakdown (`shiftCpuMs`) is 0.505
  median and 0.504 tail — flat to three decimals. The eviction and demote copies
  ride Stream's own command buffers and are still untimed on the GPU (see §5),
  but their CPU issue cost is 0.5 ms/shift and their bandwidth is bounded by the
  same 1,024-slot plane the 4.65 ms `worldgenList` writes 16 MiB into, so they
  cannot be an order of magnitude larger than the thing that generates them.
- **(e) far-field cascade fills on a level crossing — NOT THE TAIL, but the
  spikiest small row.** `farField` is 0.021 ms on the median frame and 0.153 on
  a tail frame with a max of 2.70 ms. That is a 128x ratio on a row that is
  otherwise nothing, so it IS a real periodic spike; it is 3% of the tail delta.
- **(f) something unbilled — NO. `unattributed: 0 ns`,** on a run where every
  streaming and paging command is now inside a span.

**What the tail in this scenario IS made of:** +5.23 ms of GPU, of which
**raymarch is +3.58 (68%)**, `worldgenList` +0.70 (13%), `caLoop` +0.53 (10%),
`openness` +0.27, `farField` +0.13. The whole page/stream copy path contributes
**-0.03 ms**. The +7.76 ms on `present` is the pacer draining a queue those GPU
milliseconds built; it is a consequence, not a cause.

### 4. What this scenario does NOT reproduce, and why that is still an answer

Windowed: p50 18.7, p95 82.2, p99 113.5, max 175, 11-13 frames over 100 ms.
Here: p50 14.6, p95 23.3, p99 25.1, **max 27.4, zero frames over 33 ms**.
The cadence, the shift rate and the active-chunk count all match; the frame
COST does not, and the reason is that the offscreen harness renders three draws
(world, particles, fluid) where the game renders those plus bodies, micro
bodies, sprites, debug and the ImGui overlay, and runs Jolt, mobs, the avatar
and audio around them. Its `raymarch` is 8.1 ms against the owner's live 17.7.

That gap is exactly what keeps it below the boundary in (b), and it makes the
positive claim sharper rather than weaker. **The streaming and paging path was
measured HARDER here than the game runs it — 0.99 shifts per tick against 0.68
— and its entire GPU bill is 4.7 ms per frame, of which 98% is one dispatch
(`worldgenList`) and 2% is everything else combined, and it is FLAT between a
median frame and a tail frame.** No arrangement of a flat 4.7 ms produces an
82 ms frame. Whatever the windowed harness's tail is, it is not the fills, not
the copies, not the readbacks and not the page table.

The standing hypothesis it leaves, for whoever takes the next package: the
windowed tail is **FIFO quantisation of a GPU frame that is already at the
period, amplified by (b)**. The suspicious arithmetic is that 82.2 ms is 4.93
refresh periods and 113.5 is 6.81 — the p95 and p99 of a distribution that can
only take values near multiples of 16.67 ms. The way to settle it is not another
`--frames` run: it is to record `ticksThisFrame` and the presented-frame index
in the WINDOWED harness (both are already counters) and check whether the
>100 ms frames are the 3-tick frames. That is a `main.cpp` change of about ten
lines and one run.

### 5. Still unbilled, for the next package

`Stream`'s eviction copies (`stream.cpp:469`), demote copies (`:1223`) and the
occupancy/genAct copy (`:855`) ride command buffers `Stream` creates and submits
itself, with no query resolve. Timing them means writing timestamps in
`stream.cpp`, which package P3-E holds. It is four lines beside each
`CreateCommandEncoder` using the same `TickGpuSpan` shape, and §3(d) argues they
are small — but "argues" is the operative word and they are the last GPU
commands in this engine that no span covers.

### 6. Tooling notes

- `scripts/p3f_analyse.py <perf.json> [id]` prints the median-vs-worst-5% split
  used above, per GPU node, CPU scope and counter, sorted by delta. It reads
  `build/last_run.json`-style output; no re-run needed to ask a new question of
  a recorded run.
- `PassTimer::PollDeferred(ctx, &out)` is a new collector form. `Absorb` CLEARS
  `last_` per command buffer, so `LastFrame()` is the newest one only — correct
  when a frame submits one timed buffer, and a silent 75% under-count when a
  paced frame submits four. `kRing` went 6 -> 16 for the same reason
  (`kMaxTicksPerFrame * (kFramesInFlight + 1)`, the `World::kReadbackSlots`
  argument): at 6, `KickDeferred` returns early on a slot still mapped and the
  next `EncodeResolve` overwrites numbers nobody read.
- **A paced scenario's `worldHash` is machine-dependent by construction** and
  the three runs above recorded three different ones (`760667c7`,
  `d7fe12c3`, `53f5e174`). That is not a determinism failure: the WORLD is a
  pure function of the tick, and the run simply executes a different NUMBER of
  ticks on a faster or slower machine. Do not compare it across runs and do not
  rebaseline anything from it — `--perf`'s timer-neutrality gate, which runs a
  fixed 60 ticks, is the hash that means something in this harness.
