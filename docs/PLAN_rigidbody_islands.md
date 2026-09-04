# Large islands: felling a tree, and everything shaped like one

Status: **§1–§3 measured and landed (2026-09-04). §4 onward is designed, not
built.** The acceptance test already exists and is already red on purpose:
`--selftest --gate tree-fell`, assertion `cut-trunk-fells-the-tree`.

Owner decision 2026-09-04: **articulated general islands**, not a tree special
case. A cut trunk, a blown bridge span, a severed cliff overhang and a toppled
tower are the same problem, and the tree is only the one that shows up first.

---

## 1. What actually happens today, measured

`tree-fell` plants an oak-sized tree (92 tall, 53 across) on a levelled site,
then either engulfs it or cuts three voxels out of the trunk. On the tree that
comes out:

| | before this work | after §3 |
|---|---|---|
| burn: floating single voxels | 38 | **0** |
| burn: floating 2..7 clumps | 25 | **0** |
| burn: floating >=8 clumps | 4 | **0** |
| burn: bodies made | 22 | **50** |
| cut: severed tree still in the grid | 28573 vox | **28573 vox** |
| cut: largest body made | 25 vox | **25 vox** |

The burn half is largely fixed. **The cut half is untouched, and no amount of
tuning will touch it**, because it is not a tuning problem.

## 2. The three caps, and which one binds

A tree, from the shipped atlas headers (`assets/trees/*.svtree`):

| species | height | reachXZ |
|---|---|---|
| bush | 15 | 18 |
| oak | 88 | 54 |
| birch | 94 | 60 |
| spruce | 108 | 112 |
| pine | 121 | 47 |
| willow | 124 | 95 |
| great_oak | 159 | 115 |
| eucalyptus | 183 | 96 |
| redwood | 217 | 42 |

Against three independent limits in `src/phys/debris.cpp` and
`src/phys/physics.h`:

| limit | value | what it stops |
|---|---|---|
| `kMaxRegionCells` | 80 cells/axis | the scan box cannot **contain** a tree |
| `kMaxIslandVoxels` | 32,000 voxels | the flood aborts and declares "anchored" |
| `DebrisVoxel` `int8_t x,y,z` | ±120/axis | no single body can **hold** a tree |

For the gate's 92-tall fixture, only the **first** binds: 28573 voxels is under
32000 and every axis is under 120, but the bbox is 90 tall against a region
capped at 80, so the component touches the scan-box ceiling on every scan and is
anchored at the boundary every time (`anchoredByRegionBoundary` 4646 in the cut
pass). A `great_oak` would fail all three at once.

**Read that as the ordering for the work below.** Widening the region alone
would fell the oak and nothing bigger; the other two have to follow.

## 3. What landed (2026-09-04)

Three changes, in the order the instrumentation pointed at them.

### 3.1 The event queue was head-of-line blocked
`PreTick` examined `events_.front()` and nothing else. A head whose chunks had
not arrived did nothing for 120 ticks while every event behind it aged toward
its own timeout — one region that streamed out took four seconds of island
detection down with it. Measured: **312 events dropped over one burning tree**,
each a region whose floaters nothing would look at again.

Now a bounded prefix is probed (`kEventProbePerTick` 16), scans run until a
cell budget is spent (`kIslandScanCellsPerTick`, two wide 64³ scans' worth),
and only the first `kEventFetchProbes` 4 may push chunk fetches (so a deep probe cannot flood a fetch queue the events in
front of it are waiting on). A stuck event whose region is still resident spills
to `pendingSupport_` instead of being dropped; only a region that has genuinely
left the window is dropped, and that is not a leak.

`stuckEventDropped` 312 → **0**.

### 3.2 `EventReady` never fetched the ring it reads
`solidOutside` decides whether a component leaving the scan box is attached to
structure outside, by reading cells **one past each face**. Those chunks were
never requested, so a component touching a face whose outward chunk happened to
be unfetched was anchored on a guess. The ring is now requested — but *not*
waited for, since a stale ring is a fine answer to "is there rock out there" and
gating on it would add a chunk layer to every scan's critical path.

### 3.3 A solid with nothing touching it falls, in the CA
This is the one that mattered, and it is the smallest.

Island detection is a CPU machine: a GPU support-loss flag, a per-chunk
cooldown, an async readback, a queue, a bounded region scan, a connectivity
flood, an anchor test. All of that is the right tool for deciding whether a
**ledge** is still attached to a cliff. All of it is absurd for a single voxel,
and measurably it does not arrive — the sub-8 rubble handoff was reached 401
times against 11105 small components anchored at a scan-box boundary, with
`eventQueueFullSpilled` at 1113. A firehose into a straw.

But *"is this a one-voxel island"* needs no connectivity analysis. A solid with
no solid or powder on any of its six faces **is** a component of one, by
definition, from information the cell already holds. So `sim_step.wgsl` now
drops it like a powder: no flag, no queue, no scan, nothing to congest.

Three things make this legal rather than merely appealing:

- **The lattice's read bound.** The 3×3×3 colour lattice keeps acting cells ≥3
  apart and bounds *writes* to ≤1 cell; it promises nothing about reads, and a
  `seesSky` that walked 48 cells up once broke determinism at tick 1 for exactly
  that reason (`gotcha-lattice-bounds-writes-not-reads`). This reads six cells
  at distance 1. An acting cell may write at distance ≤1 from itself, so a cell
  one step from me can only be written by an acting cell within two steps of me
  — and I am the only one there. No race, whatever the schedule.
- **Rule 2 survives.** The probe orders its six faces downward-first, so the
  overwhelmingly common case (a terrain cell with ground beneath it) exits after
  one extra load. Failure never calls `markDirty`, so nothing can hold a chunk
  awake. `sleep` still reports **0 / 32768 chunks active**.
- **Things that float on liquid still float.** `canDisplace` already refuses a
  move into a denser target, and every water-surface plant is far lighter than
  water (lilypad 260, reed 280, cattail 270, water 1000). No new rule needed.

`terrain`, `sleep`, `ca-slope`, `debris`, `settle-back` and `floaters` all pass.
`determinismHash` moved, as an intentional change to hashed state always does.

### 3.4 The second report: clumps hang "for a minute or two"
After §3.3 the owner reported 2..8-voxel clumps that *do* fall, a minute or
two late. Two causes, separated by the gate once it learned to **quench** the
fire (turn every hot cell to ash/air at once, so time-to-clean measures the
handoff alone rather than a crown that keeps smouldering for thousands of
ticks):

- **Queue throughput.** §3.1 already removed the drops; the scan budget became
  a *cell* budget, the drain went to 8 chunks/tick, and the flood now seeds only
  from the changed box and stops at the first anchor — **40.2 M → 0.59 M cells
  visited per tree (68×)** with identical verdicts (`SANDVOX_ISLAND_FULL_FLOOD=1`
  is the A/B). A narrow first tier was tried and reverted: 627 of 714 narrow
  scans near a tree clipped something and escalated, doubling the work.
- **Result:** 0/0/0 floating components at +400 after the quench; clean within
  100 ticks (the sample stride). The burn half of `tree-fell` passes at zero.

### 3.5 The third report: "still plenty of 2–6 voxel clumps"
The quenched number above was honest and incomplete. Reproduced without the
quench — sample the natural burn-out every 100 ticks for floating components
AND cells still hot — the residue sat **flat at ~30 while the hot count fell
3×**: stranded, not smouldering. Attribution (per-single face classes; forced
rescan clears them; `freed 1198` for every small component a scan *saw*) said
they were never being scanned at all. Two holes, both physics errors and both
specific to a crown burning to ash:

- `flagSupportLoss` treated a solid that became **powder in place** (leaf →
  ash) as "still supports" and returned; the ash then flowed away flagging only
  the cell *above*. A clump held sideways or from above by that leaf never got
  a flag. Powder carries the cell above it and nothing else; the flag now says
  so.
- `soloSolid` counted powder on *any* face as attachment, so a lone leaf with
  ash beside it refused to fall — and, by the hole above, nothing ever came for
  it. Powder counts only below, as the scan's own anchor rule already said.

Plus two settle-back defects the same series exposed: `SettleFootprintSupported`
abstained-as-**yes** over an unfetched chunk (the 12-voxel leaf body at the edge
of the fetched region was the biggest floater left), and a stamp vacates
nothing so raised no flag — every settle now queues a support scan over its
box. And the scan budget now charges cells actually *visited*, so the 68× flood
saving finally buys scans. The sweep also learned that floating on a denser
liquid is support (six "floating" leaves were sitting on water).

**Natural burn-out series after:** `15, 16, 5, 3, 4` floating at
+300..+1500 with 890→215 cells still hot — a clump lives less than one sample
before the scan takes it. Quenched: 0/0/0, clean at +100, forced rescan finds
nothing.

### 3.6 One thing ruled out, cheaply
The per-chunk support cooldown was *dropping* suppressed flags rather than
delaying them, which would mean the final state of a burnt region — the one the
player is standing in front of — is the one state no scan ever runs on. That was
a good hypothesis and it is now fixed (`RearmLateSupport`), but the attribution
says it was **not** the cause: `cooldown-held 12 / rearmed 9` over an entire
tree burn. Recorded so nobody re-derives it. The fix stays because the old
behaviour was wrong on its own terms.

## 4. §4 — the scan must be able to contain what it judges

**The problem.** `RunIslandDetection` allocates a dense `words`/`solid`/`label`
mask over the whole region: 5 bytes plus a 4-byte label per cell. At 80³ that is
4.7 MB and fine. At the 256³ a `redwood` needs it is **150 MB per scan**, which
is not a routine per-tick allocation under any reading of rule 2.

**The shape of the fix: sparse, chunk-tiled, two-tier.**

1. Keep the dense mask, but make it the mask of a **chunk tile** (16³ = 4096
   cells, 20 KB) rather than of the whole region.
2. Flood at **cell granularity inside a tile** and at **chunk granularity
   between tiles** — the "chunk-face acceleration" DESIGN.md §7 has always
   described and never built. A chunk carries a 6-face occupancy summary (does
   any solid cell touch this face?), which the flood uses to decide whether to
   pull in the neighbouring tile at all.
3. The component is stored as a `vector<IVec3>` of world cells plus a
   `unordered_set` of visited cells, not as a label array over a bounding box.
   Memory is then proportional to the component, which is the thing that is
   actually bounded.
4. Raise `kMaxRegionCells` to 256 and `kMaxIslandVoxels` to ~250,000 (a
   `great_oak` is ~90k wood + leaves). Both stay hard aborts — an unbounded
   flood could collapse a dungeon level, which is the design disaster the
   original 32,000 was chosen to prevent.

**Cost control.** A 256³ region is only affordable because the flood is sparse:
a tree is ~0.5% of its bounding box. Budget the flood in *cells visited*, not in
region volume, and defer the event when the budget runs out — the deferral
machinery already exists and already works (`deferredCellOpBudget`,
`kMaxEventRetries`).

**Where the CPU mirror bites.** A 256³ region is 4096 chunks, and the mirror
only holds what has been fetched (`kFetchPerTick` = 64). `EventReady` would
block for a minute. The two-tier flood is what makes this tractable: only tiles
the flood actually *reaches* need fetching, which for a tree is ~60 chunks, not
4096. `EventReady` must therefore become incremental — fetch on demand as the
flood advances, and defer when it hits an unfetched tile — rather than a
gate evaluated up front over the whole box.

## 5. §5 — a body that no longer has to fit in an `int8`

`DebrisVoxel` stores body-local coordinates as `int8_t`, capping any body at
±120 voxels per axis. A `redwood` is 217 tall.

**Rejected: widen to `int16`.** It is the obvious move and it is the wrong one.
It costs a byte per voxel on a 262,144-instance buffer, it does not help the
`kMaxBodyVoxInstances` ceiling at all, and — decisively — a 217-voxel rigid log
is a *telephone pole*: perfectly stiff, one contact manifold, no flex, and a
~90k-voxel Jolt compound shape to build in one frame. It solves the storage
problem and creates a physics one.

**Chosen: shard into jointed sub-bodies.** A component larger than the sub-body
bound is diced on a lattice of ≤96-voxel cells (leaving headroom under 120), and
adjacent shards are welded with stiff cone-limited joints. This is not new
machinery — `Physics::JointDesc` already provides cone limits and
weight-normalized friction, it is what ragdolls use, and the `ragdoll-joints`
gate already covers it.

What that buys, beyond fitting in the type:

- A felled trunk **flexes** as it goes over and can **snap** on landing, which
  is what a falling tree looks like.
- The existing `ShatterBody` connectivity split works per shard, so damage
  behaves.
- Mass and inertia stay per-shard and sane, instead of one enormous compound.

What it costs, and these are the real risks:

- **Joint count.** A `great_oak` at 96-voxel shards is 3×2×3 = up to 18 shards
  and ~45 joints from ONE island. `kMaxBodies` is 200. The shard lattice must be
  chosen from the component's extent, not fixed, and a component that would need
  more than ~12 shards should shard coarsely and accept a stiffer fall.
- **Solver stability.** A chain of stiff joints under gravity is the classic
  Jolt instability. Shards must be built as a *tree* rooted at the heaviest
  shard (the trunk base), never as a loop — a grid of shards welded on all six
  faces is a loop lattice and will jitter.
- **Settle-back.** `SettleBodies` stamps one body back into the grid. An
  articulated assembly must settle **as a unit or not at all**, or a tree half
  settles and half stays a body. Simplest correct rule: an assembly settles only
  when every shard is asleep and axis-aligned; otherwise none of it does.

## 6. §6 — sequencing, and what to verify at each step

Each step is verifiable with `--gate tree-fell` alone, which is the property
that makes this affordable to iterate on.

| step | change | the gate says |
|---|---|---|
| A | sparse chunk-tiled flood, region still 80 | no behaviour change; `anchoredBy*` unchanged. A pure refactor with a differential. |
| B | incremental `EventReady`, region → 256 | `cut-trunk-fells-the-tree` still fails, but on `oversize-flood` / `oversizeBboxSkipped` instead of `anchoredByRegionBoundary`. **The failure mode moving is the result.** |
| C | raise `kMaxIslandVoxels` | fails on `oversizeBboxSkipped` alone — the component is now found whole and only the body type stops it. |
| D | shard + weld | `cut-trunk-fells-the-tree` **passes**; `treeFell.felledBodyVoxels` ≥ 30% of standing wood. |
| E | assembly-aware settle-back | `cut-leaves-nothing-standing` passes and stays passing after 600 further ticks. |

Steps B and C each move the world hash. Step D moves it heavily. Rebaseline once
per step and move on — `feedback-hash-moves-are-not-regressions`.

## 7. What is NOT on this plan, and why

- **A global "connected to the floor" sweep.** DESIGN.md §7 names it as the only
  thing that would make the no-floaters guarantee actually true, and it may
  still be right one day. It is not this work: §3.3 removed the single-voxel
  case locally and for free, and §4–§5 remove the large-structure case, which
  between them are the two the owner reports. Decide it from the `tree-fell`
  numbers after §6, not before.
- **Tree-aware felling via `treeCandsInto`.** Considered and rejected by the
  owner 2026-09-04: it would reach the exact visual faster, but it needs a CPU
  transcription of worldgen's tree placement (a two-places-must-agree hazard
  this repo has been bitten by) and it helps nothing but trees.
- **Queue congestion during a fire.** `eventQueueFullSpilled` still reads
  ~600 on one tree while it burns: the 64-deep event queue is a hard cap and a
  burning crown re-flags its chunks every 45 ticks. It clears within 100 ticks
  of the fire going out, so it is now latency during the fire, not after it;
  a forest fire will still be the case to measure.
- **The forced-rescan anomaly.** `tree-fell` pass A2 tiles the region with
  explicit destruction events, and after §3.3 that makes the residue *worse*
  (5/2/1 → 14/22/4) rather than better. It is reported and not asserted on.
  Hammering a region with 24 overlapping events every 60 ticks is not a
  situation the game produces, but something in that path is creating floaters
  rather than clearing them, and that is worth knowing before §4 changes the
  same code.
