# PLAN: a body of water finds its level — relevel first, then slosh

> **Status (2026-10-02 audit): ALL FOUR PACKAGES LANDED on main, 2026-09-14** —
> W1 relevel `ba4de39` (checkpoint `c9d22b4`), W-D discovery `fcc639b`, W2
> slosh `d0ce910`, W3 impulses + render read of `waterFlux` (`raymarch.wgsl`
> binding 22) `fef39cb`, which also flipped `sim.waveMode` to 1. Gate:
> `--gate waterbody` (passes R/S/N). §6's non-goals stand.
>
> Original status: PLAN, amended after audit (2026-09-14, same day). `sim.waterBodyMode`
> was flipped to 1 the same day, so the M1–M5 body system (DESIGN.md §5b) now runs
> in the shipped game. This document is what to build ON it. The audit fixed five
> spec defects in place (histogram base address §3.2, ledger word aliasing §3.2,
> the credit clamp §3.3, W2's outflow-clamp write hazard §4.2, the excite-trigger
> statement §3.8) and — by owner decision — pulled CREATED bodies INTO scope as
> §8. Four packages, each moving the world hash exactly once at its own
> rebaseline, landed in this order:
>
> * **W1 — relevel.** A disturbed body flattens in tens of ticks instead of ten
>   minutes. The mirror image of the M2 shave. WGSL + ledger words + one gate
>   pass. The whole owner complaint, closed.
> * **W-D — discovery.** A body the player CREATES (a dug basin filled by hand,
>   a pool a drain leaves behind) is adopted and gets W1/W2 behaviour; puddles
>   stay CA for free. §8. Runs second so relevel reaches created bodies early.
> * **W2 — slosh.** A per-column surface momentum layer (virtual pipes) so the
>   surface overshoots, rings and dies out. Same column pass, plus one buffer.
> * **W3 — looks alive.** Impulses into that layer (blasts, drains, swimmers),
>   and the render reading it for sub-voxel normals and foam.
>
> Read first: `CLAUDE.md` (the three rules; verification is a budget),
> DESIGN.md §5b (the body system this extends, especially §5b.4's authority
> split), `docs/PLAN_water_master.md` §3.2–§3.3 (debit what was granted; never
> read a tally in the pass that writes it), and the memory note
> `gotcha-ca-liquid-levelling-limit` — its proof is §1 here.
>
> Every file:line below is against the 2026-09-14 tree. Re-verify before acting.

---

## 1. The problem, and why no CA rule can fix it

Blow a hole in the floor of a pond. What happens today, in order:

1. `sim_explode.wgsl` ejects some of the water as debris and opens a crater.
2. Water with air under it satisfies seam trigger (a) and excites into the MPM,
   capped at `sim.fluidExciteCeiling` (8,000 cells) and `sim.fluidExciteRate`.
   It falls into the crater and settles back to voxels after
   `sim.fluidSettleTicks` (24) quiet ticks. This part is fast and fine.
3. What remains is a surface DEPRESSION above the crater — a cone of settled
   voxels with water, not air, under every one of them. No excite trigger fires
   again. The only lateral mover left is the reach-1 CA in `sim_step.wgsl`.

The CA cannot finish the job, and this is proven rather than tuned:

* The equalize branch fires only at a 2-eighth difference between neighbours
  (`TUNE_LIQUID_EQUALIZE`, sim_step.wgsl:1544), and `bridgeLevel` (:1680)
  extends its reach to a distance-2 pair. **A ramp of 1 eighth per 2 cells is a
  stable fixed point.** Over an 80-cell radius that is 40 eighths: a 5-voxel
  cone is permanent. That is the "large ramp" that never goes away.
* Above that slope it flattens by diffusion: a feature of radius r costs on the
  order of r² ticks per eighth of drop. At r ≈ 80 and 30 ticks/s that is
  minutes per eighth. That is the "10+ minutes".

Lowering the equalize threshold to 1 was refused with proof (memory note; the
(k, k+1) pair is the integer diffusion equilibrium and every reach-1 tie-break
ratchets or oscillates). Flatter than reach 1 allows needs a global operation.
The engine already has one: the body system makes resting voxels move under a
ledger, at O(surface) per tick, and it already knows which columns are the
lake's. It only ever moves them DOWN. W1 teaches it to move them up.

## 2. The shape: a body owns its free SURFACE as a column field

Everything below is one idea. For an adopted body, the free surface is a
heightfield `s(x, z)` in eighths (`8·y + fullness`) over the columns the body
owns (`wbOwns`: inside the disc, in this component after a split). The column
is the unit of work in every pass — one thread, one column, writes only in its
own column — so every pass here is lattice-safe with no mark/apply, exactly as
`wbShave` is today (sim_waterbody.wgsl:824).

* **W1 moves mass between columns through the ledger** (a global potential:
  every column relaxes toward the body's mean surface). No new buffer.
* **W2 moves mass between NEIGHBOURING columns through face fluxes** (a local
  momentum: virtual pipes). One new buffer holding the fluxes.
* The two coexist: W2's pipes carry the wave, W1's relaxation carries what pipes
  cannot reach quickly (across a wide basin, around an island) and pays down
  the integer residue. Both sleep when the surface is flat.

What this deliberately does NOT do: it does not touch water below the surface
band. The interior of a lake stays a name and five integers. Sub-surface
topology (a crater, a cave mouth under the bank) is filled by the MPM and the
CA's descent rules, which are fast; the surface layer only ever has to answer
"which columns are high, which are low, and how much moves between them".

## 3. W1 — relevel: the shave's mirror image

### 3.1 Three passes, in the existing row block

| pass | shape | what it does |
|---|---|---|
| `wbSurface` | one thread per column of a listed chunk | finds the column's free-surface cell in the band, adds its height to the body's histogram |
| `wbLedger` (extended) | one thread per body | turns LAST tick's histogram into THIS tick's give/take cutoffs, keeps the credit |
| `wbRelevel` | one thread per column of a listed chunk | gives or takes eighths in its own column per the cutoffs, REPORTS what it moved |

Order in the block: `wbQuiet → wbLedger → wbDrain → wbReduce → wbShave →
wbRelevel → wbSurface → wbHole`. The surface measure runs AFTER every writer in
the block so next tick's ledger sees this tick's final surface; the ledger
consumes it one tick later (plan §3.3 at pass granularity, same as the shave
report). `wbRelevel` runs after `wbShave` so a draining body's shave and its
relevel never touch the same cell in one tick: the shave owns `level` and
`level-1`, the relevel skips a column whose surface is in that band while
`WBS_STEPS | WBS_FRAC` is nonzero.

### 3.2 The measure (`wbSurface`)

Per column, walk down from `min(level + 1, chunkTop)` to `max(level -
TUNE_WATER_RELEVEL_DEPTH, chunkBottom)` looking for the shave's own predicate:
a cell of the body's material with NOT the body's material directly above. Take
the first found. Its height in eighths is `s = 8·y + fullness` where
`fullness = state nibble + 1`. The cell above must be AIR for the column to
count — a roofed pocket (stone above) or a foreign material is not a free
surface and the column is skipped, which is what keeps this out of caves that
open under a bank.

Report, per body, with order-free atomics only:

* `WBS_RVCOUNT += 1`, `WBS_RVSUM += s` (Σs over 20k columns at s ≈ 2,500 is
  50 M, safe in i32). **`WBS_RV*`, not `WBS_R*` as first drafted: `WBS_RSUM`
  is TAKEN** — word 12 is the adoption/re-audit reduce's running sum
  (common.wgsl:765, filled at sim_waterbody.wgsl:792), and a re-audit can run
  while a body is hot and relevelling, so the two sets must not alias. Every
  new relevel word is `WBS_RV*`, appended at 27+ (`kWaterBodyStateWords`
  27 → ~38, mirrored in common.wgsl and taught to `check_invariants.py`).
* histogram bucket `h[s - sBase] += 1`, where `sBase = 8·(level -
  TUNE_WATER_RELEVEL_DEPTH)` and the bucket range is `8·(DEPTH + 2)` words.
  At the shipped DEPTH of 32 that is 272 words per body, 64 bodies, 68 KiB —
  appended to `waterBodyState` past the END of the existing layout
  (`kWaterBodyStateTotalWords`, world.h — i.e. AFTER the shared sweep scratch.
  The first draft said `kWaterCurveBase + kWaterBodyCap·kWaterCurveWords`,
  but that address IS `kWaterSweepScratchBase` and would have overlaid the
  split map's label-propagation scratch). Still NO new binding. The ledger
  ZEROES a body's bucket block as it consumes it (same ≤272-word loop, one
  thread), so `wbSurface` always accumulates into a clean histogram — nobody
  else may clear it or the report double-counts. Anything
  outside the band clamps into the end buckets and is treated as "deep" or
  "high" — bounded, and a column 32 voxels below the level is a hole the MPM
  owns anyway.

Cost: the chunk-level early-out is the shave's (three scalar loads if the chunk
misses the band). A column costs up to DEPTH reads only in a chunk that
straddles a deep cone; a flat lake costs one or two reads per column, and the
pass does not run at all outside the hot window (§3.6).

### 3.3 The decision (`wbLedger`)

One thread per body, from last tick's histogram, all integer:

```
m        = RVSUM / RVCOUNT                     floor; the mean surface, eighths
k(s)     = clamp(|s - m| / RELEVEL_GAIN, 1, RELEVEL_MAX)   eighths this tick
supplyE  = Σ_{s >= m+1}  min(k(s), s - m) · h[s]           never give below m
demandE  = Σ_{s <= m-1}  min(k(s), m - s) · h[s]           never take above m
credit   = WBS_RVCREDIT                       eighths banked, may be < 0 (§3.5)

if credit >= 0:
  takeE  = min(supplyE + credit, demandE)
  giveE  = max(takeE - credit, 0)              <= supplyE by construction
else:                                          owing: repay before any take
  takeE  = 0
  giveE  = min(-credit, supplyE)
```

(The first draft's `grantE` arithmetic contradicted §3.5's "clamps takeE to 0
while credit < 0"; the branch above IS §3.5, written once. The audit fix.)

Then walk the histogram from the LOWEST bucket upward until `takeE` is spent,
and from the HIGHEST bucket downward until `giveE` is spent. The last bucket on
each side is partial; publish it as a cutoff plus a dither numerator exactly as
the shave publishes `WBS_STEPS`/`WBS_FRAC`:

* `WBS_RVTAKECUT`, `WBS_RVTAKEFRAC` — every column with `s < cut` takes `k(s)`;
  a column with `s == cut` takes iff `hash3(seed, tick, columnKey) % h[cut]
  < frac`. `columnKey` is a WORLD-stable column key (the shave's `cellIndexW`
  of the column base, or `(x mod N, z mod N)` packed) — never a chunk-list
  index, which reorders (an index used as identity is the world one).
  Deepest columns first, which is what pressure would do.
* `WBS_RVGIVECUT`, `WBS_RVGIVEFRAC` — the same from the top.
* `WBS_RVMEAN = m` so the apply pass can compute `k(s)` and the "never past m"
  clamp itself.

Two loops over ≤ 272 buckets on one thread. Negligible.

Termination: when every column sits in `{m, m+1}` there is no bucket at `m-1`
or below and none at `m+2` or above, so `demandE == 0`, the take cutoff is
published as "nothing", and — **while `credit >= 0`** — so is the give cutoff;
the apply pass returns after three loads. While `credit < 0` the give cutoff
stays LIVE against the m+1 columns until the borrowed eighths are repaid: that
strictly shrinks |credit| whenever `supplyE > 0`. A surface that goes perfectly
flat at m with credit still negative strands those few eighths in the credit
word forever — bounded by one tick's takes, RECORDED (the identity closes with
the term), and paid down by the first disturbance that lifts any column to
m+1. The remaining (m, m+1) ripple is one eighth — a single palette step — and
it is the same residue the CA's own equalize rule leaves.

### 3.4 The apply (`wbRelevel`)

Per column: re-derive `s` this tick (the histogram is a tick stale; drift is
absorbed, §3.5), skip if the chunk holds an MPM block (`fluidBlockMapS[slot]
!= 0` — that region is the solver's and its surface is not a surface), skip if
not `wbOwns`, then:

* **give** (`s` above the give cutoff): `n = min(k(s), s - m, topFullness)`.
  Take `n` eighths off the top cell; an emptied cell becomes a clean zero word
  (air), as the shave writes it. `WBS_RVGIVEN += n`. One cell written.
* **take** (`s` below the take cutoff): `n = min(k(s), m - s)`. Room in the top
  cell is `8 - topFullness`; the overflow goes into the cell ABOVE as a fresh
  fullness-`r` cell — only if that cell is air, else `n` is clamped to the
  room. `WBS_RVTAKEN += n`. Two cells written, both in this thread's column.
* `wbMarkDirty` on every cell written, so the CA re-examines the seam.

The new cell above takes the body's material, a clean stain, `STAMP_NEVER`, and
its state nibble `r - 1`. Nothing else in the word.

### 3.5 The invariant, and the credit

```
voxelEighths(t) + drained(t) - debit(t) + rcredit(t)  ==  voxelEighths(0)
```

`rcredit += RVGIVEN - RVTAKEN` each tick, in the ledger, from the counters the
apply pass filled — debit what was granted, never what was demanded. The
histogram the cutoffs came from is one tick old and the CA may have moved a few
surface cells under it, so `RVTAKEN` can exceed `RVGIVEN + credit` by a bounded
amount on one tick. That is water briefly borrowed from the body against next
tick's gives, not water invented: the ledger clamps `takeE` to 0 while `credit
< 0` and the gives pay it back first. It cannot stall — `credit < 0` means the
voxels hold more than the record, so the mean is above what it was and some
column is above it. This is the `WB_RELEASING` argument (a released body keeps
shaving until the ledger is square) applied in the other direction, and `--gate
waterbody` pass A's identity gains the term.

`RVCREDIT` is a STORED field for the reason `WBS_DEBIT` is: a conservation gate
that has to infer it cannot attribute a failure.

### 3.6 Idle cost, and the page table

Nothing here runs on a still lake. The CPU already latches every world edit in
a labelled chunk into a hot window (`kWaterDrainHotTicks` = 900, waterbody.cpp
`Tick`) during which the body declares its whole footprint to the page table
and `writesThisTick` is true. The three relevel passes record only when
`writesThisTick` — the same gate the shave uses — so an untouched lake costs
what it cost at M5: zero.

**One decoupling while wiring this (audit find):** the hot latch today fires
only when `mode != 0 && drainMax > 0 && worldEdited` (waterbody.cpp:686) —
relevel must not ride the DRAIN knob, or `sim.drainMaxEighthsPerTick = 0`
silently disables levelling too. Latch when
`mode != 0 && worldEdited && (drainMax > 0 || TUNE_WATER_RELEVEL_MAX > 0)`,
and apply the same OR to the `writesThisTick` arms (waterbody.cpp:626).

A cost to STATE rather than discover: because gives are dithered across every
above-mean column, any disturbance that opens real demand scatters single-
eighth writes across the body's whole surface and wakes most of its surface
chunks for the active ticks. Bounded and brief (the §3.7 table), correct for a
crater; for a one-column splash it is the price of the global potential. If a
measured case makes it matter, the knob is a demand floor in the ledger, not a
change to the apply — note only, do not build it in W1. Inside the window the measure pass runs every tick and
the apply pass writes only while the cutoffs admit anything; at RELEVEL_MAX 4
a 10-voxel cone is flat in ~20 ticks, the window is 900.

What the window does NOT cover: a body disturbed by something the CPU never
sees as an edit (a mob wading, a settle from the MPM long after the blast). Add
a GPU-side latch — `wbQuiet` already scans `dirtyIn` for candidate bodies; let
it also raise `WBS_RVHOT` for an adopted body whose listed chunk went dirty, and
have the ledger keep the relevel armed `TUNE_WATER_RELEVEL_TAIL` ticks past the
last dirty sighting. The CPU still gates the footprint declaration, so this
only matters while the footprint is declared anyway; note it as a W2 item
rather than solving it in W1.

The footprint declaration must include the Y layers down to `level - DEPTH`.
Today the hot window declares the whole footprint (DESIGN §5b.5, "the known M3
cost"), so it already does.

### 3.7 What it costs, and what it buys

| cone depth | RELEVEL_MAX | ticks to flat | at 30 Hz |
|---|---|---|---|
| 10 voxels | 1 | 80 | 2.7 s |
| 10 voxels | 4 | 20 | 0.7 s |
| 10 voxels | 8 | 10 | 0.3 s |

Per tick while active: two column passes over the body's surface chunks. For a
200k-voxel pond the surface is ~20k columns; that is 40k threads doing a
handful of loads, well under the shave's own cost on a drain. The chunks written
wake and the CA runs over the surface for the duration, then sleep. Awake-at-
rest stays under the selftest's 32.

### 3.8 Interactions to state, not discover

* **Shave.** Bands disjoint per tick (§3.1). The shave's `level` model assumes a
  flat surface; the relevel makes that assumption true sooner, and the shave's
  `atLevel == 0 → level - 1` logic is untouched.
* **Split.** `wbOwns` keys off the component map, so a split body relevels each
  pool to ITS OWN mean. Two pools at two levels is the correct answer.
* **MPM.** Columns in a chunk with an MPM block are skipped; when the solver
  settles particles into voxels next to a relevelled column, next tick's
  histogram simply sees them. The excite shell (trigger (e)) only fires while
  `WBS_EMIT > 0`, so a relevel cannot open a drain.
* **Excite triggers (a)/(b)/(d) — not just (e) (audit find).** Relevel writes
  mark chunks dirty, and `exciteDetect` scans dirty chunks. Trigger (a) (air
  below) and (b) (diagonal void at a column BASE) cannot be created by a
  relevel write: a give leaves the column's own top, a take stacks onto water.
  Trigger (d) (sim_fluid_seam.wgsl:555, a ≥2-CELL step between two water
  surfaces) is the live one — but a column moves ≤ RELEVEL_MAX eighths (half a
  cell) per tick and all columns move toward the same m, so relevel cannot
  MANUFACTURE a two-cell step between columns that were within one cell of
  each other. Where a ≥2-cell step already exists (the crater wall), (d)
  firing is CORRECT — that water should excite, and did before W1. The §3.10
  excite-candidate bound is the assertion that keeps this paragraph honest.
* **Mass thrown over the rim.** Water the blast lands outside the disc is not
  owned and stays CA. Correct: it is not in the lake.
* **Two bodies straddling a chunk.** Refused at adoption today; the relevel
  inherits the refusal.
* **A pond dug from nothing.** Not a basin (§5b.7), so no relevel. Stated in §6.

### 3.9 Knobs (the five-place `TUNE_*` pipeline, all integers)

| row | default | meaning |
|---|---|---|
| `sim.waterRelevelMax` | 4 | eighths a column may move per tick; 0 is an exact identity |
| `sim.waterRelevelGain` | 8 | eighths of deficit per extra eighth of rate: `k = |s-m| / gain` |
| `sim.waterRelevelDepth` | 32 | voxels below `level` the measure looks; sizes the histogram |

`--sweep sim.waterRelevelMax=0,4` proves reachability in one boot. 0 records
the passes but moves nothing, so the hash at 0 equals the pre-W1 hash — the
three-arm identity every water milestone shipped with.

### 3.10 The gate: `--gate waterbody` pass R

Same process, same lake, after pass H's machinery. Bore a 9×9×6 crater under
the harness lake with `CellOp`s (pass H already bores a 7×7 shaft), run
`waterbodyRelevelTicks` (baseline, 90) ticks, then assert on the voxels alone:

* surface spread `max(s) - min(s)` over owned columns ≤ `waterbodyRelevelSpread`
  (baseline, 2 eighths);
* the identity of §3.5 closes at +0 with the credit term;
* `RVCAPPED`-style attribution: eighths the cutoffs asked for that no cell could
  hold, recorded not asserted;
* awake chunks 20 ticks later ≤ 32 (pass E's bound);
* the excite candidate count over the window ≤ the existing 3,000/tick bound.

Print `spread`, `ticks-to-spread≤2`, `credit`, and `given/taken` on one line so
a failure names its term (CLAUDE.md rule 6). The lab pond scene (`--lab pond`,
`kLabPond` + `LabScenePlugOps`) gets a `crater` variant for the by-eye check
and `--fluid-bench` p95.

## 4. W2 — slosh: a surface momentum layer

W1 relaxes; it never overshoots. Slosh is overshoot: the columns above the
crater drop, their neighbours accelerate toward the drop, arrive with momentum,
and ring. That needs one extra integer per column face, and nothing else new.

### 4.1 The store

A dense XZ grid over the residency window: `kWorldN²` columns × **4
NON-NEGATIVE OUTFLOW pipes** (+x, −x, +z, −z, all owned by the column — the
Mei/O'Brien virtual-pipes layout) × u32 = 4 MiB at 512², indexed by world
`(x, z) mod kWorldN` exactly as every other window-relative buffer. Signed
shared faces (2 per column) were the first draft and are REJECTED by audit:
the outflow clamp must scale a column's outflows, and a column's outflow can
run through a face a NEIGHBOUR owns, so "write the scaled flux back" meant two
writers per face word — the exact mark/apply hazard everything else here
avoids. With owned outflows, every transfer amount lives in exactly one word,
written by its owner, gathered by its receiver. Owned by the body system (`world.waterFlux`),
one binding in the water group. Derived data in guideline #3's sense — not
saved, not in the world hash, zeroed for columns that leave the window on a
shift (momentum at the window edge is far from the player and dropping it is
the same call the MPM makes for its particles on save). It IS state that decides
voxel writes, so it is integer, tick-ordered and gather-only, and the twice-run
comparison covers it by construction.

Sparse-by-chunk was considered and rejected for W2: the body's chunk list is
rebuilt each tick and keying a flux by list index makes momentum move when the
list reorders. A stable key is the column itself, and 2 MiB is cheap. Revisit
only if `kWorldN` doubles.

### 4.2 The physics: virtual pipes, integer

Per outflow pipe d of column i toward neighbour j (column i owns all four of
its outflows; a pipe is ≥ 0 always — the reverse direction is j's pipe):

```
q_i[d] += G · (s_i - s_j)                Q8 eighths/tick; G from a human knob
q_i[d]  = max(q_i[d], 0)                 opposing head drains the pipe first,
                                         so momentum reverses through zero
q_i[d] *= (1 - DAMP)                     Q8 multiply, then shift
moved   = q_i[d] >> 8                    whole eighths; remainder stays in q
```

with the outflow clamp of the pipes literature: if a column's total outflow
this tick exceeds what it may give (`min(RELEVEL_MAX, s - floorS,
topFullness)`), scale all four of its OWN pipes by `avail / total` — all four
are its own words, so the clamp has ONE writer. (This is why §4.1 rejects the
first draft's shared signed faces: "write the scaled flux back" on a face the
neighbour owns is two writers per word.) Two column passes:

| pass | reads | writes |
|---|---|---|
| `wbFlux` | `s` of self and 4 neighbours (from `wbSurface`'s per-column height, §4.3), own 4 pipes | own 4 pipes (post-clamp) |
| `wbApply` | own 4 pipes (outflow), the 4 neighbours' pipes aimed at me (inflow) | own top cell(s), the `RVGIVEN`/`RVTAKEN`-style counters |

Reach 1 in XZ, no atomics except reporting, no CAS, no scheduling dependence.
Conservation is exact because each transfer amount lives in ONE word — the
giver's post-clamp pipe — and both ends read that word.

`G` is the sanctioned human-unit lane (`sim.waveGravity`, m/s², const-eval'd to
cells/tick² at 30 Hz by `/900` as `DRAIN_TWO_G` is). Wave speed is
`sqrt(G·depth)`: at real gravity and a 10-cell depth that is ~1 cell/tick,
which is the CFL limit, so the depth term is capped by `sim.waveDepthCap`
(10 voxels) — a deep lake's waves travel at the capped speed, which reads fine
and never blows up. `sim.waveDamping` (per second) sets how many seconds the
ring lasts; `sim.waveSleepEps` is the flux magnitude below which a body counts
as calm.

### 4.3 What W1 already built that W2 reuses

* The column height. `wbSurface` writes `s` per column into a per-column word
  beside the flux (a fifth i32, 5 MiB total with §4.1's four pipes), and W2's
  flux pass reads that instead
  of re-walking the voxels. W1 should write it from the start even though only
  the histogram consumes it — it is the same store, and it makes the W2 flux
  pass a pure gather.
* The apply. `wbApply` is `wbRelevel` with a different source of `n`: the sum of
  the four face transfers instead of the cutoff rule. Fold them into ONE pass
  that adds the two contributions, so a column is written once per tick.
* The credit and the identity. Pipe transfers are exact between columns; only
  the clamp against a top cell that could not take its inflow leaves a residue,
  and it goes to `RCREDIT` like everything else.
* The sleep. A per-body `atomicMax(|q|)` in `wbFlux`, read by the ledger next
  tick; below `waveSleepEps` for `fluidSettleTicks` ticks the body publishes
  "flux asleep" and both passes return after three loads. The CPU's hot window
  stays the outer gate.

### 4.4 Where the slosh comes from, with no impulse authored

The crater alone produces it: columns above the hole lose height as their water
excites and falls, `(s_i - s_j)` at the rim grows, flux builds, the rim columns
drain toward the hole and overshoot when the hole is full. A ring travels out
at ~1 cell/tick, reflects off the bank and dies in a few seconds of damping.
That is the "punch a hole and the pond sloshes and adapts" the owner asked for,
before any W3 work.

### 4.5 The gate: pass S

Lab pond, crater variant, `sim.waveMode=1`: assert mass exact with the credit
term, assert Σ|q| over the body rises then decreases monotonically after its
peak (energy is dissipated, never created — the seam's "settle+wake must be
strictly dissipative" lesson applied to a heightfield), assert the body reports
asleep within `waterbodyWaveSleepTicks`, and assert spread ≤ 2 eighths at the
end. `--sweep sim.waveMode=0,1` and `sim.waveGravity` for reachability.

## 5. W3 — it looks alive

Small items, each independent, each after W2:

* **Blast impulse.** `sim_explode.wgsl` cannot write the flux buffer (rule 3
  keeps its writes in the mutation path); instead the CPU's explosion event
  hands the body system a `(x, z, radius, strength)` and `wbFlux` adds a radial
  outward flux for one tick. Deterministic: on the tick stream.
* **Drain coupling.** A live discharge (`WBS_EMIT > 0`) adds a sink at the hole
  in the flux layer, so the surface dips toward the throat for real and §9d's
  render vortex sits over a sim dip.
* **Swimmer wake.** A mob's or the player's submerged velocity as a small
  directional impulse in its column. Same door as the blast.
* **Render.** `waveSlope` (raymarch.wgsl) keeps §9d.5's rule — the SIM height is
  authoritative — but reads the per-column height and flux for sub-voxel
  normals between eighth steps and for foam where `|q|` is high. Gerstner stays
  as the far/idle texture. This is the one place W2's buffer is bound for
  fragment read.

## 6. Not in this plan

* ~~Ponds dug from flat ground~~ — **IN SCOPE as of the 2026-09-14 owner
  decision.** See §8 (W-D). The old exclusion text is preserved there as the
  problem statement.
* **Deleting CA liquid rules.** Nothing here changes `sim_step.wgsl`. The CA
  still owns unowned water and the seam between owned columns and the CA.
* **The far-field store** (RESEARCH_water_architecture.md option E). Orthogonal.
* **Persisting momentum.** Flux is dropped on save and on window shift.

## 7. Order of work, and the verification budget

1. **W1**, one package. New: `wbSurface`, `wbRelevel`, ledger words `WBS_RV*`
   (`kWaterBodyStateWords` 27 → ~38, mirrored in common.wgsl), histogram block
   in `waterBodyState` (past `kWaterBodyStateTotalWords`, §3.2), three `TUNE_*`
   rows, two `pass_table.def` rows, the hot-latch decoupling (§3.6), gate
   pass R, lab crater variant. A C++ rebuild for the words and knobs; then WGSL
   iteration with no rebuild. Verification: `--gate waterbody` while iterating,
   `--sweep sim.waterRelevelMax=0,4` once, one `--verify` at the end, one
   rebaseline. `check_invariants.py` should learn the `WBS_RV*` mirror.
2. **W-D**, one package (§8). CPU registry + evidence accounting + save
   serialization, GPU size-gate + sticky refusal, two `TUNE_*` rows, gate
   pass N. Rebaseline once.
3. **W2**, one package. New buffer + binding, `wbFlux`, `wbApply` folded into
   `wbRelevel`, four `sim.wave*` rows, pass S. Rebaseline once.
4. **W3**, three small packages, each behind its own knob so each is an identity
   at 0.

DESIGN.md §5b gains a §5b.8 (relevel), §5b.9 (surface momentum) and a §5b.10
(discovery) in the landing commit of each; `ARCH_NODES` in `assets/tuner.html`
gets the new passes.

## 8. W-D — discovery: a CREATED body becomes a body

> Owner decision 2026-09-14: §6's old first exclusion ("ponds dug from flat
> ground… its own document") is lifted into this plan. Requirement: a
> SUBSTANTIALLY LARGE created body — a basin the player digs and fills, a pool
> a drain leaves behind — gets W1/W2 behaviour. Puddles stay CA, at no cost.

### 8.1 What already works, stated so nobody rebuilds it

* A MODIFIED authored body: M5 re-derives the container (`curveDirtyUntil_`).
* A body that SPLITS: the sweep's component map names ≤3 children inside the
  parent's disc, each adopted, each relevelling to ITS OWN mean (§3.8). So
  "drain a lake into two pools and disturb one later" is ALREADY the M5+W1
  path — no discovery involved.

The one true gap: water pooling where no basin was ever registered.

### 8.2 The determinism constraint that shapes everything

The CPU may not read voxels (the mirror is 3×3×3; a readback on the frame path
puts fence retirement in a voxel write's control path — §5b.4's whole
argument). So discovery takes the M2 authority split:

* **The CPU proposes and RETAINS candidates from the tick stream only.** Every
  water-placing brush/spell op and every carve rides the MutationQueue, so the
  CPU can account "eighths of liquid placed, where" EXACTLY, as a pure
  function of the tick stream. Replays reproduce it; the twice-run gate
  compares it by construction.
* **The GPU decides.** The existing WB_MEASURING reduce + M5 sweep measure the
  REAL body (volume, level, curve, components) and refuse the rest. The CPU
  never learns the verdict and never needs to: a refused candidate's ledger is
  zeroed and its steady-state cost is three loads.

### 8.3 CPU: evidence accounting and the probe registry

`WaterBodySystem` gains `discovered_`: `{center x,z; radius; floorY; mat;
evidenceEighths; lastEvidenceTick; basinId}` with basinId from a reserved
range. This is AUTHORED-EQUIVALENT TRUTH — not derivable from seed — so it is
**SAVED with the world** (a small serialized block; replay also rebuilds it
since evidence is pure tick stream). On load the entries are re-proposed and
the GPU re-adopts by re-measuring the restored voxels. Nothing else about a
body persists (ledger/curve stay derived, as today). Cap `kWaterDiscoveredCap`
(16) inside `kWaterBodyCap`.

Evidence: accumulate placed-liquid eighths per coarse XZ cell (32-voxel grid,
per material). Carves below an adopted body's waterline already have the hole
path; carves elsewhere accrue no evidence (an empty pit is not a body until
water arrives — and the water's arrival is the evidence). When a region's
evidence crosses `sim.waterDiscoverMinEighths` AND the site is not inside any
existing body's footprint, append a discovered entry: disc = bounding circle
of the contributing evidence cells padded by 8 voxels, floorY = lowest
evidence Y − 4. Later evidence in the disc GROWS it (bounding circle again)
and re-dirties the entry — the same latch shape as `curveDirtyUntil_`. The
disc is only a BOUND; the sweep's component map trims ownership to the actual
connected water, exactly as it does for authored basins.

Best-effort secondary source: while a drain is live, the hole hint seeds a
probe disc under the hole (the discharge pools somewhere below). Water that
runs far laterally out of every probe is missed in v1 — a stated limit; it
stays CA, which is today's behaviour, not a regression.

Eviction (cap pressure only): lowest `evidenceEighths`, ties by oldest
`lastEvidenceTick` — deterministic. An evicted entry is proposed with
`WBF_RELEASE` for `kWaterDiscoverReleaseTicks` (600) before being dropped, so
the mass-exact exit in `wbLedger` runs. NEVER drop a descriptor cold: slot
reuse against a stale ledger is the carried-descriptor bug class the drain
pass documents.

### 8.4 GPU: adopt or refuse — sticky, re-probed only on new evidence

A discovered entry is proposed exactly like an authored basin (same descriptor
words; `wbOwns` = disc ∧ component). The adoption reduce runs once
(WB_MEASURING); the ledger then applies the size gate: adopt iff measured
surface area ≥ `sim.waterAdoptMinArea` columns. Refusal is a new STICKY ledger
state (`WB_REFUSED`): three loads a tick, no footprint work, no chunk wakes.
It re-enters WB_MEASURING only when the CPU marks the entry dirty with NEW
evidence — which is tick-stream deterministic, so the re-measure tick is too.
Sixteen refused puddle-probes therefore cost nothing, which is the owner's
"puddles can be ignored easily" implemented literally. (The CPU-side
`waterDiscoverMinEighths` filter is the first line; this is the backstop for
evidence that evaporated, soaked away, or ran off before adoption.)

### 8.5 The gate: `--gate waterbody` pass N (new body)

Same process, after pass R: (1) CellOp-dig a pit OUTSIDE the harness lake's
disc, CellOp-fill it past `waterDiscoverMinEighths`, run the discovery
cadence, assert a body appears (count +1, WB_ADOPTED, measured volume within
tolerance of what was poured); (2) bore pass R's crater in the DISCOVERED body
and assert pass R's spread bound — W1 exercised on a created body, the owner's
actual ask; (3) the negative arm: a second pit filled to HALF the threshold,
assert the body count is unchanged and no ledger slot went live; (4) a
save/reload round-trip of the registry block, assert re-adoption. Print
`bodies`, `adopted-tick`, `refused` on one line. `--sweep
sim.waterDiscoverMinEighths=0,4096` for reachability.

### 8.6 Knobs

| row | default | meaning |
|---|---|---|
| `sim.waterDiscoverMinEighths` | 4096 | placed-liquid evidence that raises a probe (~500 voxels of water — a real pond; a puddle never gets close); 0 = discovery OFF, the identity arm |
| `sim.waterAdoptMinArea` | 64 | measured surface columns below which the GPU refuses adoption |
