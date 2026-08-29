# MASTER PLAN: cheap large-water — bodies, drains, currents, vortices

> **Status: design of record for the water-bodies work, written 2026-08-28 from
> an owner design session. Nothing implemented.** This document supersedes
> `docs/PLAN_water_bodies.md` as the thing to implement from: it keeps that
> document's architecture, corrects five things in it, and reorganises the work
> around what each piece actually buys. `PLAN_water_bodies.md` stays as the
> longer rationale — read it for the *why* behind any component here, but build
> from this file.
>
> Read first, in this order: `CLAUDE.md` (the three inviolable rules, and the
> verification-is-a-budget section), `docs/RESEARCH_water_architecture.md` (the
> decision record this sits inside), `docs/PLAN_fluid_overhaul.md` §8 (WP5 — the
> hybrid CA/MPM split this builds on top of, with the measured numbers that are
> your baseline).
>
> Every file:line below is against the 2026-08-28 tree. Re-verify before acting
> on one.

---

## 0. What this document corrects in `PLAN_water_bodies.md`

An implementing agent will read both. These are the deltas, and they are load-bearing:

1. **`PLAN_water_bodies.md` §0 does not exist.** That file's header says "§0 is a
   rebuttal of this document, written by the same author. Read it first — it
   corrects §1's framing, names a competing architecture, and lists what this
   design does not cover," and then there are two horizontal rules with nothing
   between them. The section was never written or was lost (the file is
   untracked, so there is no history to recover it from). **Do not treat that
   document as having passed its own author's review.** Where this file and that
   one disagree, this one is later and was written with the owner in the room.

2. **The container curve is an area-per-height table, and the algorithm is a
   height-ordered union-find sweep, not a per-level flood fill.** See component 2.
   That change also produces the spill elevation and the split tree as
   byproducts, which `PLAN_water_bodies.md` §5 treats as separate work.

3. **The area table is a schedule, not an authority.** The surface-shave pass
   counts what it actually removed and debits *that*. Mass exactness therefore
   does not depend on the table being right, which means an approximate table for
   player-dug basins is acceptable. `PLAN_water_bodies.md` §7.1 implies the
   opposite ("`area` must come from the curve derivative"), and building it that
   way makes every table inaccuracy a mass leak. See component 3.

4. **The current field is superposition only. No neighbour coupling.** The owner's
   original framing was a field where nearby vectors influence each other so a
   river entering a pool dissipates outward. That behaviour is what a *point
   source* does for free under superposition; implementing it as actual diffusion
   means stored state, a relaxation solver, per-tick cost, and it does not sleep.
   See component 8.

5. **Jurisdiction is local, not global.** `PLAN_water_bodies.md` §6 says this in
   §6 and then §6.1 reads as an all-or-nothing enter/exit. Punching a hole in a
   lake must excite a *region*, never release the whole body — releasing at the
   moment of drainage throws away the entire win.

---

## 1. The goal, and how success is measured

**Goal:** a lake, pond, or sea costs O(1) at rest and O(1) to drain, regardless of
volume; a drain is a real hole with real water coming out of it; water shows
visible current, flow direction and vortices; and none of it can lose or invent a
single eighth of water.

**The benchmark scenes already exist.** `docs/PLAN_fluid_overhaul.md` §8 built
exactly the two scenes this work needs, for the WP5 A/B:

```bash
bash scripts/run.sh ./build/Release/sandvox.exe --fluid-bench wp5    # pond68
bash scripts/run.sh ./build/Release/sandvox.exe --fluid-bench wp5b   # worldlake
```

- **`pond68`** — worldgen's smallest disc pond, r=68, 203,298 water voxels
  (1,626,384 eighths = 6.4× the whole particle pool). Built, allowed to go
  provably asleep (68 idle ticks, zero live particles *and* zero active blocks),
  then a 5×5 shaft opened into a sealed 40×40×20 chamber that counts what arrives.
- **`worldlake`** — the real worldgen at labMode 0, against `genColumn`'s authored
  lake at (420,420): 347,832 water voxels. Same puncture.

**These are your before/after harness. Do not build new ones.** The measured
baseline to beat, from the WP5 block (drain window = 400 ticks after the plug,
RTX 3060 Ti):

| scene | arm | frame p50 | p99 | drained (eighths) | mass |
|---|---|---|---|---|---|
| pond68 | CA alone | 10.28 | 11.25 | 6,320 | EXACT |
| pond68 | hybrid, ceiling 8,000 | 14.51 | 16.03 | 6,336 | EXACT |
| worldlake | CA alone | 15.00 | 17.12 | 70,743 | EXACT |
| worldlake | hybrid, ceiling 8,000 | 25.05 | 28.60 | 72,996 | EXACT |
| worldlake | hybrid, ceiling 262,144 | 69.34 | 73.74 | 102,402 | EXACT |

**Success = drain MORE than those rows while costing LESS.** WP5's finding was
that drain throughput is flat from 4k to 32k particles and ~97% of what arrives
arrives as settled voxels — the CA is doing the transport, the particles are a
splash at the hole. This work replaces that transport with arithmetic, so the
target is a p50 at or below the CA-only arm with throughput well above the
ceiling-262,144 arm.

WP5's third finding is the one that sizes the ambition: *"There is no maximum
drainable body size, only a drain TIME. 400 ticks moves 2.5–2.7% of a
347,832-voxel lake either way."* That percentage is what should move.

### 1.1 M1 — LANDED 2026-08-28

Components 1, 2's analytic half and 5's structure, plus `--gate waterbody` and
the off switch. `src/sim/waterbody.{h,cpp}`, `src/test/selftest_water.cpp`,
DESIGN.md §5b. CPU-only: no GPU pass, no buffer, no binding, no `pass_table.def`
row, no `TickParams` field.

**Hash: unmoved, and measured rather than argued.**

```
--sweep sim.waterBodyMode=0,1   ->  d32a013e / d32a013e     (identical)
--gate waterbody pass D          ->  af008434 / af008434     (40-tick mutation
                                     script, mode 0 vs mode 1, same world)
--gate determinism               ->  adfcbce8, matches baseline
```

Note for a future reader: `--sweep` prints *"ALL HASHES IDENTICAL — the
parameter does not reach the kernel at these values"*. At M1 that is the
DESIRED result, not a defect report. It stops being desirable at M2.

**The curve is exact on both container kinds.** This is the number M2's ledger
inherits, and it came out better than §2's "the closed form does not have to be
perfectly exact" allowed for:

| check | analytic | world | error |
|---|---|---|---|
| authored lake volume (`pond68`) | 2,782,656 eighths | 2,782,656 | **+0.00%** |
| authored lake surface area | 14,493 cells | 14,493 | **+0.00%** |
| tarn r54 bowl, level walk vs column walk | 128,214 cells | 128,214 | **+0.00%** |
| settled surface spread | — | 0 vox | — |
| awake chunks with the feature on | — | 0 (budget 32) | — |
| descriptor state flips, 200 ticks parked on the threshold | — | 0 | — |

Two things that made the exactness possible and are worth not undoing:

* **The disc bound is carried, not the radius.** `pondAt` rejects `d2 > r*r`
  while the authored pools test `d2 < r*r` — one cell apart, which at r=68 is a
  ring of 428 cells. `WaterBasin::discD2Max` holds the inclusive bound so the two
  conventions cannot quietly disagree.
* **The lattice count is exact, not `pi*r*r`.** At r=68 the closed form is
  14,527.4 and the true count is 14,545. That is 17 cells = 136 eighths, i.e.
  exactly the kind of unattributable leak §7 says a conservation gate must never
  be handed.

**Two corrections this milestone makes to the document above.**

1. **§2's "where the sweep runs" understates what the analytic case covers, and
   §6's M2 row overstates what M1 leaves undone.** The container curve's analytic
   half is *finished*, for both kinds, with zero measured error. M2 does not need
   to touch it.

2. **§3.4 has a hole this plan does not name: the QUIESCENCE input.** The
   jurisdiction test's "quiescent for K ticks" term is the one input to the
   ladder that is not a function of the tick — it reads `World::Snap()`, the
   async readback, which arrives on a schedule set by fence retirement. At M1
   that is harmless because the verdict is advisory and no kernel reads it. **The
   moment adoption gates a shave it is a rule-1 violation through the back door,
   and no determinism gate that runs twice in one process with the same fence
   cadence will catch it.** M2 must either derive quiescence from a
   tick-deterministic signal or put the adoption decision on the tick input
   stream the way `fluidExciteEnable` and `windMode` already ride it. This is
   restated at the top of `waterbody.h` and in DESIGN.md §5b.4.

**A limitation to know before writing M2's gate.** `pondInfo`'s keep-out box is
the 768-voxel square at the origin, which is *exactly* the residency window the
harness runs in (world.cpp says so). So **no tarn is ever resident during a
gate** — the only body the suite can sweep from voxels is the authored lake, and
that is a flat-floored cylinder, the degenerate curve. The bowl arm is checked
against `World::TerrainHeight` per column instead, which the `terrain` gate
proves per-voxel immediately beforehand. Any M2 pass that needs a real bowl's
VOXELS has to move the window, which is what `voxregion` does and why that gate
runs last (rule 7).

### 1.2 M2 — LANDED 2026-08-29

Components 3 (the drain ledger) and 4 (the surface shave), plus the per-body GPU
reduce component 1 deferred, plus the fix for §1.1's quiescence hazard.
`assets/shaders/sim_waterbody.wgsl` (4 entry points), `world.waterBodyState`
(4 KiB, simBGL_ binding 24), 4 `PT_TICK` rows under a new `C_WATERBODY`
condition, `TickParams.waterBodies`/`waterChunks`, `sim.waterBodyTestDrain`.
DESIGN.md §5b.4.

**Hash: unmoved, and measured three ways in one invocation.** `--gate waterbody`
pass D now runs THREE arms — mode 0, mode 1, mode 0 again — because this pass
runs after a drain and a two-arm comparison cannot tell "mode 1 changed the
world" from "arm 1 inherited something arm 2 did not":

```
--gate waterbody pass D  ->  af008434 / af008434 / af008434
```

That third arm earned itself immediately. The first version of M2's gate
reported `401bbd76 / af008434` and read exactly like a broken off switch. It was
not: pass A had left ~243 lake chunks freshly dirtied and the CA mid-relevel, and
the FIRST arm absorbed it. The three-arm form printed `401bbd76 / af008434 /
af008434` and named the real fault in one run — **the pass that perturbs the
world is the pass that owes the cleanup**, which is CLAUDE.md rule 7's "gates
share one `World`" applied *inside* a gate. Pass A now settles the world before
pass D hashes it, and asserts it settled (0 awake, 60 ticks later).

#### The ledger is exact, and the drain is real

| check | value |
|---|---|
| GPU adoption reduce vs the CPU voxel sweep | 2,782,656 / 2,782,656 eighths, **+0.0000%** |
| drain: 60 ticks at 14,493 eighths/tick (one eighth-step) | voxels 2,782,656 → 1,913,076 (**−869,580**) |
| ledger `drained` | 869,580 (= 60 × 14,493, exactly) |
| outstanding `debit` at rest | 0 |
| `capped` (eighths the cells did not have) | 0 |
| free surface | y209 → y202 (7 voxels, 31% of the lake) |
| **conservation `voxels + drained − debit − start`** | **+0 eighths** |
| page faults over the drain | 0 |
| awake chunks 60 ticks after the drain stopped | 0 |
| descriptor state flips, 200 ticks on the threshold | 0 |

`--gate waterbody` alone establishes every row above; there is no second
invocation and nothing to read off stderr.

#### §9's ranked-first risk, measured rather than argued

Plan §9 item 1 is that the shave feeds the excite detector the way WP5's
draining CA did — 169,616 candidates over 400 ticks on `worldlake`, enough to
convert the whole 262,144-particle pool. Over the 60 draining ticks, at the
shipped `sim.fluidExciteMode` 1:

```
exciteDetect LOOKED AT   10,855,257 settled liquid cells  (~180,900/tick)
excite CANDIDATES                 0
```

Both halves are load-bearing, and the first version of this probe reported only
the second. `0 candidates` alone is unattributable — it means either "the shave
creates no air-below" or "the detector never ran", and those are opposite
conclusions. `seen` is what separates them, and 10.9 M cells inspected says the
detector ran hard and found nothing. **The mechanism is not there:** excite
trigger (a) is *air below*, the shave removes from the TOP, and the CA
re-levelling behind it on the woken chunks does not produce one either. The gate
carries the `seen == 0` case as an explicit note so a future run cannot quietly
report a meaningless zero.

The control window (60 quiet ticks, same world) reports `0 seen / 0 candidates`
— the seam is not recorded at all when the world is settled, which is rule 2
working, not a gap in the measurement.

`--fluid-bench wp5b` was NOT re-run for this number and should not be. That
bench drains by PUNCTURE, so what it measures is the CA's candidate production,
which is WP5's already-published baseline and is unchanged by this milestone —
`sim.waterBodyTestDrain` is 0 there, so no shave fires. The number above is from
a real shave on a real adopted body, which is the thing §9 is about.

#### Three design notes worth not undoing

1. **The ledger is GPU-owned, and that was forced rather than chosen.** §3.2 says
   debit what was GRANTED; the only honest source for that is an atomic the
   shave increments; reading it back would put fence retirement inside a voxel
   write's control path. So authority is SPLIT: the CPU decides only what is a
   pure function of (seed, window, tuning) and PROPOSES, and the GPU measures
   quiescence from `dirtyIn` + the MPM block map and owns the ladder, the level,
   the area and the ledger. That is also what closes §1.1's correction 2.

2. **`area` is MEASURED, not predicted.** The shave counts the surface cells it
   saw and the ledger uses that count next tick; the analytic curve seeds only
   the first one. So §3.2's "a schedule, not an authority" is literally true —
   the GPU holds no copy of the container curve at all — and the 0-eighth
   conservation result does not depend on the curve being right.

3. **The band is two Y values.** The shave considers `level` and `level-1` only,
   so a listed chunk that misses the band returns after three scalar loads. A
   body's whole footprint is listed once and the dispatch still only works where
   the water is. One thread owns one (x,z) COLUMN and writes at most one cell in
   it: reach 0 write, reach 1 read, lattice-safe with no mark/apply.

#### What M2 did NOT do

* No discharge law and no local excite — those are M3, and until then the only
  drain source is `sim.waterBodyTestDrain`, 0 in every shipped world.
* No re-audit of an adopted body. The reduce runs ONCE, on the single tick a
  body spends in `WB_MEASURING`. A body whose voxels change underneath it
  (someone digs) carries a stale `volume`; that is M3's problem and it is named
  here so it is not discovered as a surprise.
* Gate passes B (split scheduling) and F (determinism mid-drain) are still
  absent, for §7's reason: B needs component 10's union-find sweep and F needs a
  second in-process world. Both would be assertions against zero today.
* `--fluid-bench wp5`/`wp5b` throughput was not re-measured. M2's drain is a
  test tap, not a hole, so there is no throughput claim to make yet — that
  comparison belongs to M3, against the §1 baseline table.


---

## 2. The substrate that already exists

Do not rebuild any of this. An agent that does not read this section will
reimplement three of these.

| What | Where | Relevance |
|---|---|---|
| **CA liquid, fullness in eighths** | state nibble, `common.wgsl:22`, `LIQ_FULL_STATE = 7u`, code 0..7 = 1..8 eighths | The unit of the entire ledger. Integer, no scaling, no rounding. |
| **The ratified hybrid** | `PLAN_fluid_overhaul.md` §8 | **CA owns supported bulk water, MPM owns water with momentum**, split by state not material. The deletion of CA liquid was REJECTED. This plan sits above both. |
| **Excite/settle seam** | `sim_fluid_seam.wgsl` | `spawnAppend` (:368) is how anything becomes particles. `FluidSpawnOp` (`common.wgsl:1851`, 32 B, must match `world.h`). Budget charged CPU-side, `slot >= FLUID_CAP` refusal. |
| **Excite bounds** | `sim.fluidExciteCeiling` = 8,000, `sim.fluidExciteRate` = 4,096 | Measured in WP5. Refusal is graceful — refused water is still settled water and the CA moves settled water. |
| **`FLUID_VMAX`** | `common.wgsl:1673`, 0.45 cell/substep | `spawnAppend` clamps to it (:378). A Torricelli velocity under high head *will* exceed it. See component 6. |
| **Analytic pond bowls** | `pondAt`, `worldgen.wgsl:816`; params `tuning.h:1923–1937` (`pondTile 448`, `pondRadiusMin 48`, `pondRadiusSpan 32`, `pondDepth 26`, `pondDepthRim 3`, `pondBerm 5`, `pondBermWidth 14`) | The bowl **replaces** the ground (terrain overhaul package C), so it is an exact analytic parabola. Component 2's table is closed-form for every natural pond. |
| **Landform slope** | `Land.slope`, `worldgen.wgsl:443` — `\|dh/dx\| + \|dh/dz\|` in Q8, `256 == 1 voxel/voxel == repose` | The stream arm of the current field. **It is `g2`, the HILL-octave gradient, deliberately (`worldgen.wgsl:550`).** Use it; never the fine gradient — the grain octave crosses a whole gate range in one column. |
| **Wind field** | `windAt` `common.wgsl:1085`, `windAtQ` :1482, `windPrims : array<vec4<i32>, 96>` in **both** `TickParams` (:410) and `RenderParams` (:638), `windPrimEvalF` :1021 / `windPrimEvalQ` :1341 transcribed from each other, AABB early-out via `windPrimLo/Hi` | The exact template for component 8. Clone the shape, including the transcription discipline and the AABB. |
| **The `ptr<uniform, T>` rule** | `common.wgsl:522–534`, stated in as many words | A function that dynamically indexes a primitive array on a **by-value** uniform spills the whole struct to scratch: 220 ms vs 20 ms frames. Any current-primitive accessor takes `ptr<uniform, T>` **and so does every caller**, from the first line written. |
| **Submersion is a fraction** | `player.h:49`, `player.cpp:479` `inLiquid = liquidCells > 0` | Ankle-deep vs fully-under already distinguished. Component 8's player forces and any funnel work get partial buoyancy free. |
| **Sparse aux layer keyed by chunk** | design guideline #2 | Where the basin label goes. Do not widen the 32-bit voxel. |
| **Page table** | `docs/PLAN_page_table.md` | Address voxels ONLY through `voxWordAt`/`voxWordIndex`/`voxStore` (WGSL) or `World::PageOffsetOfSlot` (C++). `voxStore` has a page-fault probe that names the dropped word and the refusing chunk — that is your first diagnostic, not your last. |

---

## 3. The four disciplines

These recur in nearly every component. They are the difference between this
working and this being a mass pump.

### 3.1 Derived data, one owner

> **Voxels are authoritative. Every descriptor, table, label and field in this
> plan is derived: reconstructible from the voxels, disposable, never saved,
> never hashed.**

The interior voxels of a pooled body continue to exist and continue to *be* the
mass. The descriptor is a cache of aggregates over them. This costs nothing (a
still lake's chunks are asleep either way) and the win survives intact, because
the win was never "don't store the water," it was "don't *transport* mass through
cells that have nothing to say."

Consequence: any gate may recompute a descriptor from scratch and assert equality
with the live one. That is the primary test hook.

### 3.2 Debit what was granted, never what was demanded

This is the master rule and it appears three times:

- The surface shave debits the ledger by **cells it actually shaved**, not by the
  area table's prediction (component 3/4).
- The drain debits by **eighths actually emitted**, not by the analytic discharge
  `Q` (component 6).
- Excite debits by **what the seam actually took** after the ceiling and rate
  bind, not by what it asked for (component 7).

Every one of these is a place where an analytic demand meets a granted supply.
Wire the accounting to the supply side and the whole design is mass-exact by
construction. Wire it to the demand side and you get a slow leak that no
conservation gate will attribute correctly.

### 3.3 Never read a tally in the pass that writes it

Accumulate this tick, act next tick. Standard mark/apply cadence, already the
engine's idiom. Integer `atomicAdd` into a per-body word is sanctioned (addition
is associative and commutative, so the final value is order-independent); the
`atomicCAS`/exchange ban is untouched.

### 3.4 Schedules are functions of the tick number

Any work spread over ticks — re-deriving a basin, adopting a body, running a
discovery sweep — must be scheduled as `id % N == tick % N`, never "when the CPU
got around to it." A re-derive "when convenient" is a scheduling-dependent
outcome and breaks rule 1 through the back door.

---

## 4. The components

Ten components. Six make it cheap and correct, three make it look like water, one
is for when the player digs. Each section states what it is, how to build it, what
verifies it, and what breaks if you get it wrong.

---

### Component 1 — Body identity

**What.** A small record per still body of water: which basin it is, its surface
level, its surface cell count, its total volume in eighths, its ledger fields, and
its holes.

**Why it is first.** Without a name for "this lake" there is nothing to do
arithmetic on, and components 2–7 are all impossible. It changes no behaviour at
all, which makes it the cheapest thing to land and the thing that proves the
off switch works.

**Where it lives.** CPU-side (`src/sim/waterbody.h` + `.cpp`, new files — no board
contention). Body counts are tiny; a cap of 64 is generous. Per tick, upload a
small array into `TickParams`:

```
waterBodies : array<vec4<i32>, 4 * WATERBODY_CAP>
```

packing per body: `level`, `surfaceArea`, `remainderEighths`, `shaveFlag`,
`debitEighths`, `holeCount`, `state`, plus hole descriptors. Same
`ptr<uniform, T>` rule as `windPrims` — this array is dynamically indexed.

The GPU-side mutable half (the atomically-accumulated debit and shaved counts)
goes in a small storage buffer, **not** the uniform: `waterBodyState :
array<atomic<i32>>`, a handful of words per body.

**Cell → body mapping.** A sparse aux layer keyed by chunk, per design guideline
#2: `chunkBasin : array<u32, kNumChunks>`, 32,768 × 4 B = 128 KiB. Derived data,
like the page table: not hashed, not saved, rebuilt on load.

**The straddle case, and the cheap correct answer.** A chunk can contain water
from two different basins at two different levels. The shave predicate keys off
the chunk's body id, so a straddling chunk would shave one of them at the wrong
level. **Detect straddling chunks during labelling and refuse adoption for both
bodies.** Falling back to the CA is a safe degradation, it is trivially cheap to
detect, and straddles are rare because basins are separated by terrain above the
water line. Do not attempt per-cell labelling to fix this.

**Descriptor fields (minimum):**

```
basinId        u32     stable id from the basin registry
level          i32     world Y of the free surface
surfaceArea    u32     surface cells at `level` (from component 2)
volumeEighths  u64     total, for conservation gates
debitEighths   i32     accumulated removed-but-not-yet-shaved  (GPU atomic)
shavedEighths  i32     what last tick's shave actually removed (GPU atomic)
remainder      u32     leftover below one full step             (see 3.2)
state          enum    Candidate | Adopted | Releasing
quietTicks     u32     for the jurisdiction test
holes          [..]    position, area, elevation
```

**Verification.** Gate pass: recompute every descriptor from the voxels, assert
equality with the live one. That single assertion catches label corruption, bad
merges, bad splits and ledger drift at once, and needs no reference world.

**What breaks if wrong.** Nothing yet — this component has no behaviour. That is
the point of landing it alone.

---

### Component 2 — The container curve

**What.** For each basin, a table of **cell count at each height Y**. Volume is
its prefix sum; level is a binary search of that prefix sum.

```
area(y)        = number of BASIN CELLS at height y
volume(level)  = Σ area(y) for y ≤ level
level(volume)  = binary search over the prefix sum
```

**Count cells, not columns.** This is the whole reason this design beats a
heightfield. Counting cells handles caves, overhangs, a flooded tunnel under the
lake, water under a rock ledge — with no special cases. Counting columns silently
reimplements the single-span-per-column assumption that got heightfields rejected
(`RESEARCH_water_architecture.md` §4.1.1).

**Why you need it.** A bowl narrows as it empties. At the default pond
(`pondDepth 26`, `pondDepthRim 3`) the area at the bottom is a small fraction of
the area at the rim. A fixed-area assumption drifts badly and in the direction of
draining too slowly at the end — the drain visibly stalls.

**Size.** One entry per Y from floor to rim. A default pond is ~26 entries. A big
lake is still one entry per Y. This is nothing.

**Building it, case 1 — natural ponds: closed form, no algorithm.** `pondAt`
(`worldgen.wgsl:816`) is an exact parabola with known parameters, and the terrain
overhaul made the bowl *replace* the ground rather than `min()` into it. So
`area(y)` is a closed-form function of Y: invert the parabola for the radius at
that height, area is the disc. Evaluate ~26 entries on CPU at adoption time. No
measurement pass, no flood fill, no storage beyond the pond's tile coordinates.

The same closed form gives the spill elevation (the berm level, `surf + pondBerm`)
directly.

**Building it, case 2 — player-dug basins: one height-ordered union-find sweep.**
Do **not** run a flood fill per Y level; that costs O(levels × cells) and gives
you only the area table.

Process the basin's cells in **increasing height order** with union-find. One
pass, four outputs:

1. **`area(y)`** — the size of each component as the sweep rises.
2. **The spill elevation** — the height at which a component first connects to the
   outside world. Upper bound on the table's validity; above it, water overflows.
3. **The split elevations** — every height where two components *merge* going up
   is a height where one basin *splits* going down. This is the merge tree of the
   terrain.
4. **Which children each split produces**, so a split is exact integer division of
   the parent's water rather than a search.

That fourth output is the answer to the hardest scheduled event in this design.
**A draining lake splits itself** — any bowl with an uneven floor becomes two
puddles as the level falls past the high point between them, and that is the last
phase of *every* drain, not an exotic case. Getting it from the same sweep that
produces the area table is the main argument for building the curve this way.

This is standard terrain-hydrology work (priority-flood / merge tree — Barnes,
Meijster). **Read the literature before deriving it.**

**Where the sweep runs.** It needs voxel granularity over the basin's AABB, which
the 3×3×3 CPU mirror cannot see. Options, in preference order:

1. A GPU compute pass over the basin's chunk AABB writing the table to a small
   buffer. No readback, fits the engine's model.
2. An **async** readback of the basin's region via the existing `voxregion`
   machinery (`src/tools/voxregion.h`). Acceptable **only** because it is rare,
   bounded, and scheduled — never in the frame path (rule 3).

Scheduled by `basinId % N == tick % N` (discipline 3.4).

**The de-risking property: this table is a schedule, not an authority.** The
surface shave counts what it actually removed and debits *that* (component 3). So
an inaccurate table costs you a slightly off-pace surface descent and **never a
mass error**. Consequences:

- The analytic closed form does not have to be perfectly exact.
- The dug-basin sweep may be approximate, and may be a few ticks stale after a dig
  without consequence.
- You may ship component 2 with **only** the analytic case and no sweep at all,
  and dug basins simply are not adopted. That is a legitimate v1.

**Verification.** Gate pass: build a known bowl, assert `volume(spill)` matches a
hand-computed number; assert the split elevations match the bowl's known interior
high point.

**What breaks if wrong.** The surface descends at the wrong rate — visible as a
drain that stalls or races near the end. Not a mass bug, by construction.

---

### Component 3 — The drain ledger

**What.** One integer per body counting eighths removed from the body but not yet
taken off its surface. **This is the mechanism that makes draining O(1) instead of
O(volume).** Everything else exists to serve it or keep it honest.

**The arithmetic.** Fullness is eighths, so:

> Lowering a body's surface by one eighth costs exactly **`area` eighths**, where
> `area` is the surface cell count. Each of `area` cells loses 1/8 voxel;
> `area × 1/8` voxels = `area` eighths.

Integer compared against integer. No scaling, no rounding, anywhere.

**The loop, per tick, per body** — one thread per body in a tiny compute pass:

```
1. debit  += (what drains/excite actually took, atomically, last tick)
2. debit  -= (what last tick's shave actually removed)     <-- discipline 3.2
3. steps   = debit / area                                  <-- whole eighth-steps ready
   frac    = debit % area                                  <-- the dither fraction
4. publish (level, steps, frac) for this tick's shave pass
5. if steps > 0: level -= steps eighths' worth; recompute area from component 2
```

Never read the tally in the pass that adds to it (discipline 3.3). Accumulate this
tick, act next tick.

**The legitimate divergence.** Between shaves, the ledger and the voxel sum
genuinely disagree by the accumulated remainder, in `[0, area)` eighths. This is
not a bug. It must be:

- **stored as its own field**, never implied;
- **included in every conservation gate**, or the first such gate reports a leak
  that does not exist;
- **written back into voxels on release** (component 5).

**A worked number, to calibrate.** Default pond, radius ~48: surface area ≈ 7,240
cells, so one eighth-step costs ≈ 7,240 eighths. Total volume at mean depth ~12
vox ≈ 87,000 voxels ≈ 700,000 eighths. A 1-voxel-square hole under 2 m of head
drains ≈ 10 eighths/tick, so **~724 ticks (~24 s) per eighth-step of surface
drop**, and a ~39-minute full drain. Two conclusions: the feature is about *slow
bulk* — the drama is at the jet, not the lake — and that is ~70,000 ticks of
pressure propagation through ~87,000 cells the CA now never runs.

A violent drain (1 m² hole) is ~3,800 voxels/s: full pond in ~23 s, surface descent
~4.2 eighths/s, **~14% of surface cells stepping per tick**. Under the dither
below that reads as a smooth, slightly shimmering, continuously descending
surface. The level model survives this fine; it is the throat that needs
component 7.

**Verification.** Gate pass A (conservation): `Σ(voxel eighths) + Σ(ledger
remainders) + Σ(in-flight MPM mass)` invariant across a full drain. This fails on
every mistake in components 3, 4, 6 and 7.

**What breaks if wrong.** Everything. This is the component where a bug is a mass
leak rather than a cosmetic error.

---

### Component 4 — The surface shave

**What.** A per-cell pass that drops surface cells of a body by one eighth.

**The predicate**, per cell:

```
is liquid
AND my Y == body.level
AND the cell directly above is not this liquid   (i.e. I am the free surface)
AND ( steps > 0  OR  hash3(seed, tick, cellIndex) % area < frac )
```

Reach 0 for the write, reach 1 for the read directly above — both legal. No
mark/apply needed; it is lattice-safe by construction.

**The dither is not cosmetic polish, it is the accounting.** The `frac` term makes
each surface cell independently drop an extra eighth with probability `frac/area`,
so the expected number of extra drops is exactly `frac` — and the actual number is
counted and debited (discipline 3.2), so it is exact regardless. Without it,
dropping every surface cell at once reads as the whole lake snapping down a step.
With it, the level descends as a dissolving noise pattern. Free, deterministic,
and it is the difference between "the water is going down" and "someone edited the
water."

**The pass must report what it removed.** `atomicAdd(shavedEighths, 1)` per cell
shaved. Component 3 subtracts that next tick. This is the single line that makes
the area table a schedule rather than an authority.

**Waking chunks.** A pooled body's chunks are asleep. When a shave fires, the
body's surface chunks must be marked dirty for one tick — roughly `area / 256`
chunks, ≈28 for a default pond. That is fine: **no shave fires when nothing
drains, so idle cost is exactly zero** and the ≤32-active-chunks-at-rest assertion
is untouched. Verify this explicitly (gate pass E) rather than assuming it.

**The CA interaction to verify, not assume.** Waking those chunks also lets the CA
run on them. Two things to check with instrumentation, not reasoning:

1. The CA's levelling rules will try to re-level the dithered surface. That is
   *fine* — mass is conserved either way and the re-levelling is part of what
   makes the drop read cleanly. Confirm it does not fight the dither into a
   thrash.
2. **The shave must not feed the excite detector.** WP5 measured that a draining
   CA leaves transient gaps under cells all over the body, and excite trigger (a)
   is "air below" — that produced **169,616 candidates over 400 ticks** on
   `worldlake` from a hole 25 cells wide, converting the entire 262,144-particle
   pool. The shave removes from the *top*, so it should not create air-below; the
   CA re-levelling might. **This is the single most likely way this feature
   regresses into the exact burst WP5 had to bound.** Instrument the candidate
   count on `worldlake` before and after and quote the number.

**Verification.** Gate pass A (conservation, above). Plus a direct pass: known
body, known debit, assert exactly the predicted cells stepped.

---

### Component 5 — The jurisdiction test

**What.** The classifier that decides which water this system governs. **This is
where the whole design lives or dies.**

**A body is pooled — and therefore governed — only if all of:**

1. It sits **below its basin's spill elevation** (not overflowing means not flowing).
2. **Surface height spread** under a threshold. This is precisely the error term
   of the whole model: a stream down a hillside is one connected component with a
   200-voxel head difference between its ends. A connected component is a
   topological fact; an equipotential surface is a hydrostatic one; they coincide
   only at equilibrium.
3. **Volume above a size threshold.** Small ponds are cheap to simulate honestly
   and the model's error is relatively largest there.
4. **Quiescent for K ticks** — no excite events, no MPM particles in its chunks.
5. **No straddling chunks** (component 1).

Anything else is a stream and belongs entirely to the CA/MPM, untouched.

**Hysteresis is non-negotiable.** Distinct enter and exit thresholds with a real
gap. A body sitting exactly at a boundary flips representation every tick, and
**every flip is a seam crossing where mass can be lost.**

**Both directions must be mass-exact.** Adoption reads the voxel sum into the
ledger (a GPU reduce over the body's chunks, one word per body). Release writes
any remainder back into voxels *before* dropping the descriptor. Neither may
round.

**Jurisdiction is LOCAL.** This is the correction from §0 and it matters more than
the thresholds:

> A violent drain in an otherwise still pond keeps the pooled descriptor for its
> bulk and hands only a region around the throat to MPM. Full release is the exit
> for a body that stops being pooled *everywhere*; a local excite is the exit for
> a body that stops being pooled *in one place*.

Getting this wrong in the conservative direction — releasing the whole lake
because someone poked a hole in it — throws away the entire win at exactly the
moment it matters.

**The off switch.** `sim.waterBodyMode = 0` must reproduce today's behaviour
bit-for-bit: no descriptors, no adoption, no ledger. This is what makes the
feature cheap to evaluate and cheap to abandon, and what makes
`--sweep sim.waterBodyMode=0,1` a one-invocation differential.

**Verification.** Gate pass C (hysteresis): park a body at the size threshold, run
200 ticks, assert the descriptor does not flap and mass is flat. Gate pass D (off
switch): `sim.waterBodyMode=0` reproduces the pinned hash exactly.

---

### Component 6 — The discharge law

**What.** Depth of water above a hole gives exit speed and flow rate. Standard
orifice discharge:

```
h = body.level − hole.elevation          (already an integer, in voxels)
v = sqrt(2 g h)
Q = C_d · A · sqrt(2 g h)
```

Integer-deterministic via integer sqrt or a small lookup table.

**The single-evaluation rule.**

> **One evaluation of `h` must produce both the emitted particle momentum and the
> ledger debit.** If the jet is emitted by one rule and the lake decrements by
> another, they will disagree under every edge case and you have built a mass
> pump.

**Emission goes through the existing seam.** `spawnAppend` + `FluidSpawnOp`
(`sim_fluid_seam.wgsl:368`, `common.wgsl:1851`) is the entry point: CPU-charged
budget, `slot >= FLUID_CAP` belt-and-braces refusal, per-particle position,
velocity, species, material. Nothing new is needed to make a drain spit MPM.

**Two traps in that path, both of which break the rule above:**

1. **`spawnAppend` clamps velocity to ±`FLUID_VMAX`** (`:378`). A Torricelli
   velocity under high head *will* exceed it — at `pondDepth 26` = 2.6 m, exit
   speed is 7.1 m/s. The clamp is correct and must stay. But then the momentum you
   asked for is not the momentum you got. **For v1: cap `h` before computing `Q`,
   so the two stay consistent.** The alternative (accept the clamp, spread the
   flow over more particles at legal speed) is more physical and more work — do
   not silently let them diverge either way.
2. **Emission must be bounded per hole per tick** (rule 2: bound every emergent
   process). **When that cap binds, debit the ledger by what was actually emitted,
   never by the analytic `Q`** (discipline 3.2).

**Hole detection.** A cell that is inside the body's footprint, at or below body
level, and has air below or laterally outside the basin. Detect on the
chunk-dirty path when terrain is edited — holes only appear when someone digs or
explodes.

**The pleasant consequence.** Because the jet is real MPM, everything downstream
is already built: it splashes, it carves, it pushes the player, it interacts with
the CA at the existing seam. The drain is not a new fluid system, it is a *source
term* with a physically meaningful magnitude.

**Verification.** Gate pass A (conservation, across a full drain with the cap
binding). Gate pass F (determinism): same seed, two runs, compare mid-drain.

---

### Component 7 — Local excite at the hole

**What.** Wake a region around the hole into the existing particle solver, while
the other ~95% of the body stays a number.

**Why.** Water at a violent throat is genuinely moving at ~7 m/s and no painted
surface will sell that as still water. WP5's conclusion, restated: *"The level
model survives this fine. The throat is what breaks."*

**Mechanism.** Reuse `seam_excite_detect` with its existing bounds —
`sim.fluidExciteCeiling` (8,000) and `sim.fluidExciteRate` (4,096/tick), both
measured in WP5. Refusal is graceful for free, because refused water is still
settled water and the CA moves settled water.

**Radius.** For v1, a tuning knob. The principled version (once component 8's
vortex exists) is to **use the analytic model to decide where the analytic model
stops being good enough** — excite out to where the vortex surface dip exceeds
about one voxel:

```
r_excite = Γ / sqrt(8π² · g · 0.1)
```

**Honest cost flag, and it is the one number in this plan that could force a
redesign.** For a `Γ ≈ 22 m²/s` drain that is ~2.5 m ≈ 25 voxels, and a
25-voxel-radius hemisphere is **~33,000 particles** — against a largest-measured
lab scene of ~40,000 (`RESEARCH_water_architecture.md` §4.2). A violent drain sits
at the top of the measured envelope.

**The stated mitigation is unmeasured: excite a SHELL — the surface annulus plus
the throat column — rather than a solid ball.** Roughly 1% of the full-excite cost,
hundreds of particles rather than tens of thousands. The interior of a funnel is
water nobody can look at, and this is the same principle as the skin/collider
resolution split: full resolution where it is observed, cheap where it is not.
**Measure the shell before committing to the ball.**

This generalises past the drain and is probably the most reusable idea in the
whole design — analytic bulk plus MPM interface is also the right shape for
waterfalls, shore breakers and spillways. If it is built, build it as a shared
primitive, not as drain code.

**Verification.** A particle-count ceiling in `tests/baseline.json`. Gate pass A
(the excited region's mass must be in the conservation sum).

---

### Component 8 — The current field

**What.** A pure function giving flow velocity at any point. **This is what shows
flow, currents and vortices**, and what pushes the player, debris and particles.

**Structure: clone the wind field exactly.**

```
currentPrims    : array<vec4<i32>, 3 * CURRENT_PRIM_CAP>   in BOTH TickParams and RenderParams
currentPrimLo/Hi: vec3<i32>                                 union AABB early-out
currentPrimEvalF / currentPrimEvalQ                         transcribed from each other
currentAt(p, R) / currentAtQ(p, T)
```

Mirror `windPrims` (`common.wgsl:410` and `:638`), `windPrimEvalF` (:1021) /
`windPrimEvalQ` (:1341), `windPrimAt` (:1063) / `windPrimAtQ` (:1407). The
integer and float evaluators must be transcriptions of each other, with the same
comment discipline the wind pair has.

**`ptr<uniform, T>` from the first line written, and convert every caller.**
`common.wgsl:522–534` records what happens otherwise: 220 ms vs 20 ms frames.

**No neighbour coupling. Superposition only.** The behaviour the owner described —
a river running into a pool bed dissipates and spreads outward — is what a **point
source** does under superposition. A source's radial outflow *is* the jet
dissipating into the basin. Implementing it as actual vector diffusion means
stored state, a relaxation solver, per-tick cost, determinism exposure, and it
does not sleep. Superposition of sources, sinks and vortices is a **real solution
of Laplace's equation**, not a hack — incompressible irrotational flow away from
boundaries is approximately what pond water does.

**Primitive types:**

| Primitive | Field | Character |
|---|---|---|
| **Sink** (drain inflow) | `1/r²` radial | Violent but **only a couple of voxels wide** at any realistic `Q`. Must be clamped near `r → 0`. |
| **Vortex** | `Γ/2πr` tangential | **Reaches far.** The visible, navigable, player-affecting part. |
| **Source / jet** (river mouth, waterfall base) | `1/r²` radial outward | The dissipating-spread behaviour, for free. |
| **Uniform stream** | constant, from bed gradient | Rivers. |
| **Wind stress** (optional) | surface layer downwind, return flow beneath | One extra term with a depth-dependent sign, driven by the existing `windAt()`. What makes a lake read as a body of water rather than a scrolling texture. |

**That sink/vortex asymmetry is not a compromise, it is why real whirlpools look
enormous while the actual suction is a small throat.** Design the visible danger
around the tangential term and the lethality around the throat.

**Vortex circulation.** Set `Γ` from `hash3` of the hole position plus a
contribution from the body's ambient circulation. This is physically legitimate,
not a fudge: a real bathtub vortex is not created by the drain, it is residual
ambient circulation being concentrated as fluid moves inward — `Γ` is conserved,
so `v_θ = Γ/2πr` blows up as `r` shrinks. The swirl is an *initial condition*.
Chirality from the same hash, so not every drain in the world spins the same way.

**`Γ` must decay when flow stops.** Otherwise any funnel effect stands open in
still water, which is instantly and obviously wrong.

**The stream arm — genuinely independent of everything else in this plan.**
Manning/Chézy gives `v ∝ sqrt(slope · depth)`, direction from the bed gradient,
and `Land.slope` already exists as a Q8 field (`worldgen.wgsl:443`). No descriptor
needed; this arm can be built before component 1.

> **Trap, and it is a repeat of one this repo already paid for.** Use the
> **landform** gradient, not the full one. `worldgen.wgsl:550` states that
> `Land.slope` is `g2` accumulated through the hill octave *specifically because*
> `d(slope)/dcolumn` through the grain octave is 96 Q8 — the whole gate range in
> ONE column. A current built on the fine gradient is per-voxel noise. See the
> "slope gates must read the landform" failure.

**Seeding, split from evaluation.** The evaluation half (the array, the
evaluators, the AABB, the arrow viz, the consumers) is stable no matter what
components 1–7 decide and can be built at any time. The seeding half — which
primitives exist and where — comes from component 6's holes and component 1's
descriptors. Build the evaluator first with the stream arm and a debug/authored
primitive list; add seeders as components land.

**Accuracy envelope, and why the errors are harmless.** No no-flow boundary
conditions at terrain, no separation, no eddies behind obstacles, no turbulence.
**This is tolerable precisely because the field owns no mass.** A wrong current
pushes a leaf the wrong way; it cannot lose water. That is the payoff for keeping
it a pure function, and it is why this tier can be tuned by eye.

**Consumers.** Render surface advection (component 9), player force, debris and
particle drag, foam on convergence lines (the field is analytic, so its divergence
is closed-form).

**Verification.** Visual, plus an arrow-viz debug overlay. The existing arrow-viz
lesson applies: fade by the sine of the view angle and cull the near plane after
the length is known, or it streaks uselessly along the view axis. Determinism: the
integer evaluator is in the sim path, so `--gate determinism` covers it.

---

### Component 9 — Surface waves

**What.** Render-only surface displacement, evaluated where a ray hits the water.
Sum 4–8 Gerstner waves, analytic in `(x, z, t)`. Zero storage, zero sim cost, cost
is O(water pixels) not O(volume).

**Set each component's speed from its wavelength and the local depth.** This is
the single highest-leverage accuracy decision in the render tier and it costs
nothing — it is purely how you choose the constants:

```
ω² = g·k·tanh(k·h)          k = 2π/λ,  h = depth
  deep    (h ≫ λ):  tanh → 1      ⇒  c = sqrt(gλ/2π)   — longer waves are FASTER
  shallow (h ≪ λ):  tanh(kh)≈kh   ⇒  c = sqrt(g·h)     — λ cancels, all speeds equal
```

If every octave scrolls at one speed the surface reads as a moving texture. In the
default pond (`pondDepth 26` → 2.6 m at `kVoxelMeters 0.10`) the spread across the
octaves worth rendering is **4×**:

| λ | `k·h` at centre | `tanh(kh)` | `c` |
|---|---|---|---|
| 0.5 m | 32.7 | ≈1.00 | 0.88 m/s |
| 2 m | 8.2 | ≈1.00 | 1.77 m/s |
| 8 m | 2.04 | 0.967 | 3.48 m/s |

Then the same `tanh(kh)` pays a second time, because **`h` is already known** from
the body level and the terrain. Approaching a bank at `h = 0.3 m` the 8 m wave
slows from 3.48 to **1.70 m/s** while the 0.5 m wave barely changes. That
differential slowdown *is* shoaling: waves steepen in the shallows and **refract to
run parallel to the shore**. It is the effect that sells a lake, and it falls out
of a term already being computed.

**Coupling and detail:**

- **Flow reads as flow** by evaluating wave phase at `position − current·t`. The
  Doppler stretch downstream is what makes a surface look like it is *going
  somewhere*. Component 8 → component 9, ~free.
- **Fade amplitude to zero near shores.** Sum-of-sinusoids does not reflect off
  banks, and shallow water damps chop anyway — the cheap fix is also the
  physically right one.
- **Foam** on convergence lines of the current field, thinning on divergence.
- **Impact ripples need state**, because a ripple is the memory of an event. Stay
  in the `windAt()` idiom: a small **bounded** ring buffer of recent impacts, each
  drawn as an analytic expanding ring with amplitude decay — a pure function of
  `(eventList, t)`, bounded by construction. The upgrade path (a per-body 2D
  wave-equation texture) is stored state plus a solver. **Do not start there.**

**The one expensive mistake.** Evaluate at the water-surface hit, **not** through
the volume. The perf audit identified the raymarch media march as what collapsed
FPS during fires; do not add per-sample work to it.

**The render/sim boundary.** A render-only wave cannot push anything. The moment
displacement is fed back so boats bob, a render field has become authoritative for
sim (guideline #3). If that is ever wanted: expose surface height as a **read-only
query**, keep the body's *level* (sim) strictly separate from its *displacement*
(render), and never let the CA see the displacement.

**Independence.** This component needs only a level and a depth, both of which the
descriptor provides, and it is the cheapest large visual win in the plan. It moves
no hash. It can be built in parallel with everything.

---

### Component 10 — Basin discovery for dug terrain

**What.** The height-ordered union-find sweep of component 2, applied to basins
the player carved.

**Why it is last and small.** Natural ponds are analytic, so their tables are free.
Only player-modified basins need discovery, and only the ones they modified.

**Scoping.** Detect the *possibility* cheaply — a mutation touched a rim chunk of a
labelled basin — then schedule the re-derive over N ticks as `basinId % N == tick %
N` (discipline 3.4). Water is slow; a few ticks of a slightly stale level is
invisible, and the table is a schedule not an authority so a stale table cannot
leak mass.

**If a coarse sweep is wanted:** a lake 500 voxels across is ~31 chunks across at
`kChunk = 16`. Deterministic label propagation on the **chunk** graph converges in
O(diameter) — ~31 passes versus ~500 on the voxel graph — and the chunk graph *is*
the dirty-chunk list, already compacted, already indirect. Per-voxel connectivity
inside a chunk is irrelevant to an aggregate. Use the chunk graph for *labelling*;
use voxel granularity only for the area counts, and only if the approximation
proves visible.

**Verification.** Gate pass B (split scheduling): a dug pond with a known interior
high point drains past a partition elevation; assert two descriptors appear at
exactly the predicted level, with volumes summing to the parent's. Gate pass F
(determinism): catches a re-derive that was scheduled on CPU convenience rather
than tick number.

---

## 5. Named and explicitly out of scope: the conveyor (sleeping rivers)

The owner's second goal was *"make running rivers sleep without simulating every
water voxel's movement."* **This is a real component and it is not in this plan.**
It is named here so nobody accidentally half-builds it.

What it would be: a river bed's fullness held constant, mass deleted at the outlet
and materialised at the inlet at matched rates, with the visual motion supplied by
component 8. That is a **conveyor**, and its correctness argument is the same
ledger and single-evaluation discipline as components 3 and 6 — a source and a
sink that must debit and credit from one evaluation, or the source backs up or the
world gains water.

Component 8 is *necessary* for it (the river must look like it is moving) and
*not sufficient* (the field moves no mass). Build it after this plan lands, as its
own document, reusing components 1, 3, 5 and 8.

---

## 6. Sequencing

Dependencies are strict left to right within a milestone chain. The look chain is
genuinely parallel — disjoint files, no shared state, hash-neutral.

```
SIM CHAIN     1 ──► 2 ──► 3 ──► 4 ──► 6 ──► 7
              │           ▲
              └──► 5 ─────┘                      10 (after 2)

LOOK CHAIN    8 (stream arm + evaluator) ──► 9 ──► 8 (drain seeders, after 6)
```

| Milestone | Components | What it gains | Hash |
|---|---|---|---|
| **M1 — skeleton** ✅ **LANDED 2026-08-28** | 1, **2's analytic half**, 5's structure, the gate, the off switch | Nothing behavioural. Descriptor recomputes to +0.00% on both container kinds; off switch bit-identical. See §1.1. | **Identical**, measured: `--sweep sim.waterBodyMode=0,1` → one hash |
| **M2 — the drain works** ✅ **LANDED 2026-08-29** | 3, 4, the GPU reduce, and the quiescence fix of §1.1 correction 2 | A lake drains by arithmetic, driven by the `sim.waterBodyTestDrain` tap. Conservation exact at **+0 eighths** over 869,580 drained; the shave produces **0 excite candidates** against 10.9 M cells inspected. See §1.2. | **Identical**, measured: mode 0 / mode 1 / mode 0 all `af008434` |
| **M3 — it is a feature** | 6, 7 | Real holes, real jets, real throat. Quote `--fluid-bench wp5`/`wp5b` against the §1 baseline. | Moves — its own commit, `--rebaseline` |
| **M4 — it looks alive** | 8 (full), 9 | Current, vortices, flowing surface, foam. | 9 must not move it; 8's sim arm will |
| **M5 — completeness** | 10, 2's sweep | Player-dug basins participate. | Moves |

**M1 and M2 are the whole architectural risk and neither changes the world hash.**
M3 is where it becomes a feature. M4 is the fun. M5 is completeness.

**Parallelism.** The look chain touches `common.wgsl` and `raymarch.wgsl`; the sim
chain touches `src/sim/`, `sim_step.wgsl` and `sim_fluid_seam.wgsl`. Those are
disjoint enough for two worktrees. Board-claim per chain. Note the stale claim on
`raymarch.wgsl`/`common.wgsl` from 2026-08-25 (agent-037576, render-only liquid
fullness surface) — days old, treat as abandoned per CLAUDE.md, but check file
mtimes first.

---

## 7. The gate

One gate, `--gate waterbody`, verifiable **alone** — no separate smoke pass, no
manual terminal reading, no second run with different flags. Thresholds live in
`tests/baseline.json`, not in C++.

| Pass | Asserts | Catches |
|---|---|---|
| **A — conservation** | `Σ(voxel eighths) + Σ(ledger remainders) + Σ(in-flight MPM mass)` invariant across a full drain | Every mistake in components 3, 4, 6, 7. The primary pass. |
| **B — split scheduling** | A pond with a known interior high point drains past a partition elevation; exactly two descriptors appear at the predicted level, volumes summing to the parent's | Component 2's merge tree, component 10 |
| **C — hysteresis** | A body parked at the size threshold, 200 ticks: descriptor does not flap, mass is flat | Component 5's thresholds |
| **D — off switch** | `sim.waterBodyMode=0` reproduces the pinned hash exactly | Landed at M1, kept forever |
| **E — idle cost** | A large pooled lake keeps active chunks at rest under the existing ≤32 assertion | Component 4's chunk waking. **If a labelled body cannot sleep, the feature is a regression regardless of what it enables.** |
| **F — determinism** | Same seed, two runs, drain in progress at the compare tick | A re-derive scheduled on CPU convenience (discipline 3.4) |
| **G — recompute equality** | Recompute every descriptor from voxels, assert equality with the live one | Label corruption, bad merges, bad splits, ledger drift — all at once |

**Instrument at the point of failure, not the point of refusal.** Per CLAUDE.md
rule 6: when conservation fails, the reporter must say **which body, which term,
and by how much** — not "mass changed by 37." The `voxStore` page-fault probe is
the precedent (it records the dropped word *and* the refusing chunk span), and it
is the reason component 3 specifies the remainder as a stored field rather than an
implicit one. Budget four extra words per body for attribution before you need
them.

**Design for `--rebaseline`.** M3 and M5 move the hash. The rebaseline path must
handle it end to end — no hand-edited files, no hashes read off stderr.

---

## 8. Tuning knobs

Each follows the 5-place `TUNE_*` pipeline: row in `tuning_params.def` → decl in
`tuning.h` → `Read*` in `LoadTuning` → default in `tuning.json` → row in
`tuner_schema.js`. **No trailing comments on `.def` rows** —
`gen_tuning_prelude.py`'s parser chokes.

**Integer lane** (`sim.*` is integer-only per rule 1):

| Knob | Default | What |
|---|---|---|
| `sim.waterBodyMode` | **0** | Off switch. 0 must be bit-identical to today. |
| `sim.waterBodyMinVolume` | tbd | Enter threshold, eighths |
| `sim.waterBodyExitVolume` | tbd | Exit threshold — must be meaningfully below enter |
| `sim.waterBodySpreadEnter` | tbd | Max surface height spread to adopt, voxels |
| `sim.waterBodySpreadExit` | tbd | Release threshold — meaningfully above enter |
| `sim.waterBodyQuietTicks` | tbd | Quiescence window |
| `sim.waterBodyMaxCount` | 64 | Cap (rule 2). At the cap, refuse adoption of the smallest — unadopted just means "simulated the old way," a safe degradation. |
| `sim.drainMaxEighthsPerTick` | tbd | Per-hole emission bound (rule 2) |
| `sim.drainExciteRadius` | tbd | Component 7, v1 |
| `sim.currentMode` | 0 | Current field off switch |

**Human-unit float lane.** `sim.fluid*` and `sim.wind*` are the sanctioned
exception: human-unit floats (vox/s², m/s, seconds) converted to fixed-point-per-
tick integers by WGSL const-eval at the top of the kernel that reads them
(IEEE-exact, so the kernel stays integer and deterministic). **The physical
quantities here belong in that lane**, for the same reason:

| Knob | Units | What |
|---|---|---|
| `sim.drainCd` | dimensionless | Orifice discharge coefficient, ~0.6 |
| `sim.drainGravity` | vox/s² | Should match `sim.fluidGravity` |
| `sim.currentVortexGamma` | m²/s | Base circulation scale |
| `sim.currentVortexDecay` | s | `Γ` decay time when flow stops |
| `sim.currentStreamScale` | — | Manning/Chézy coefficient for the stream arm |

Wave knobs are render-side, not `sim.*`.

**Prove reachability with `--sweep`, not file edits.** Every new knob:
`--sweep sim.yourKnob=0,100` in one invocation, no file touched, no restore.

---

## 9. Risks, ranked

1. ~~**Component 4 feeding the excite detector.**~~ **MEASURED AT M2 AND IT IS NOT THERE: 0 candidates against 10,855,257 settled liquid cells inspected over 60 draining ticks (§1.2).** The reasoning below is why, and it held — but the instrumentation is what settled it, and the `seen` half of that pair is what makes the zero mean anything. Left in place because M3's real jets reopen the question at the throat, where the mechanism genuinely does exist. WP5 measured 169,616 excite
   candidates over 400 ticks on `worldlake` from a *draining CA* leaving transient
   gaps under cells — enough to convert the entire particle pool and hold ~70 ms
   frames. If the shave or the CA re-levelling after it reproduces that, this
   feature regresses into the exact burst WP5 had to bound. **Instrument the
   candidate count on `worldlake` at M2 and quote it.** This is the most likely
   way the work fails.
2. **Component 7's particle budget.** ~33,000 particles for a violent drain
   against a ~40,000 measured envelope. The shell mitigation is unmeasured.
   Measure the shell before committing to the ball.
3. **Component 5's boundary.** A threshold that flaps is a mass-loss machine.
   Hysteresis with a real gap, and local rather than global release.
4. **Component 2's dug-basin sweep.** The one place needing a readback. Async and
   scheduled, never in the frame path. Legitimate v1: ship analytic-only and do
   not adopt dug basins.
5. **`ptr<uniform, T>` on the current primitives.** A 10× frame-time regression
   with a known cause and a known fix. Get it right on the first line.
6. **The sea.** Terrain overhaul package D is not landed. It is the extreme case —
   one body, effectively infinite volume, level fixed by fiat. Probably a
   *degenerate* body with an infinite curve rather than a special case, but **this
   plan must not constrain package D**, and package D must not inherit the
   never-settling tarn.

---

## 10. Handoff rules

- **Board-claim per chain** (`bash scripts/board.sh claim`), one worktree per chain
  if parallel. The sim chain will touch `sim_step.wgsl`, `sim_fluid_seam.wgsl`,
  `simulation.cpp`, `world.h`; the look chain `common.wgsl`, `raymarch.wgsl`.
  Re-check the board before every build/commit/selftest.
- **Build only via `scripts/build.sh`; every run via `scripts/run.sh`.**
  `SANDVOX_NO_CRASH_DIALOG=1`. Verify exe mtime before trusting any result. An
  unmutexed concurrent `sandvox.exe` poisons every perf number — check `tasklist`,
  measure twice.
- **The verification budget in CLAUDE.md applies and is not optional.**
  `--gate waterbody` + `--fluid-bench wp5b` while iterating; full `--suite
  acceptance` once, at the end, on the tree you intend to ship. Before every run,
  answer "what claim does this establish that is not already established?"
- **WGSL-only edits need no C++ rebuild.** `LoadShader` reads from disk at runtime
  and the SPIR-V cache keys on content hash. Run `check_shaders.sh` and the
  existing binary.
- **A bare count is not a measurement.** When conservation fails, add attribution
  to the reporter (§7) rather than A/B-eliminating hypotheses one run at a time.
  The precedent cost 14 runs versus 4.
- **Update `DESIGN.md` in the same commit where behaviour changes.** §5 (MLS-MPM
  liquid, :577) and §4 (the CA, :261) are the sections this touches; a new §
  alongside §9b (Wind, :2985) is the right home for the current field.
- **Update `ARCH_NODES` in `assets/tuner.html`** when work lands or completes — add
  or remove `wip` fields, update `desc`/`details`, add nodes for new systems, and
  keep the Development Status panel current. ASCII `'` only for JS string
  delimiters.
- **Append measured numbers to §1 of this document** as each milestone lands, in
  the shape of the baseline table. Every claim in a handoff should be a number
  somebody can reproduce with one command.
```
