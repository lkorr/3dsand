# PLAN: the 2026-09-08 frame-rate regression — measured, attributed, ranked

**Date:** 2026-09-08. **Tree:** main `3098128`. **Status:** measurement +
diagnosis, no code changed.
**Audit 2026-10-02 (current state of §7, checked against code):** P1's intent
(sky chunks stop paying the per-column worldgen) is met by a different shape —
the `cols` column-cache pre-pass and its block header (`CCH_TOP`, read once per
chunk in `worldgen.wgsl` genChunk; map overhaul P4 `0e9b59e`, 2026-09-26). P4 is
still open: `sim_glow.wgsl`'s backstop still walks `TUNE_GLOW_CHUNKS` slots every
tick unconditionally. P6's fine-march work went on as the raymarch perf rounds
(ray-start map `b0ba179`, 2026-09-28). P0a/P2/P3/P5 not re-audited here.
**Question the owner asked:** "performance is pretty bad and fps is very
stuttery, and low compared to a few days ago."

Everything below came from **two** binary launches on a renamed copy of
`build/Release/sandvox.exe` (15:23), both under `SANDVOX_RUN_EXCLUSIVE=1`,
RTX 3060 Ti, 1920x1080. No build was run. Raw records:
`build/perf_diag.json`, `build/perf_diag.log`, `build/rb_diag.log`.

Companion docs whose numbers this is compared against:
`docs/PLAN_frame_perf.md` (2026-09-05 survey),
`docs/RESEARCH_streaming_hitch.md` (2026-09-03/04 streaming numbers, and the
place where three of the fixes below were already scoped).

---

## 1. The measurement

`--perf` (all scenarios, 240–600 frames each):

| scenario | p50 | p95 | p99 | max | frames > 1.5x mean |
|---|---|---|---|---|---|
| idle (settled) | 18.84 | 20.33 | 20.80 | 21.98 | 0 |
| flythrough | 20.56 | 32.21 | 35.40 | 40.05 | **60 of 600, at a strict 15-frame period** |
| explosion | 20.36 | 22.38 | 23.77 | 25.58 | 0 |
| water (pond drain) | 26.94 | 31.06 | 32.48 | 37.01 | 0 |
| surface-sprint | 22.67 | 40.18 | 55.84 | 58.01 | 48 of 600 |

`--render-budget`, noon overlook (the one camera with history behind it):

| arm | ms | delta = cost of |
|---|---|---|
| baseline | **13.52** | (19.6 on 2026-09-05 — the renderer got 6 ms FASTER) |
| lod8 (`lodHandoffDist` 24 -> 8 m) | 5.46 | **8.06** — the fine 10 cm march |
| halfres (960x540) | 4.09 | 9.43 — 30% of baseline, i.e. pure pixel cost |
| nofar (`farSteps` 384 -> 0) | 11.38 | 2.14 |
| noshadow | 11.71 | 1.81 |
| nogi | 12.33 | 1.19 |
| noglow (landed today) | 12.68 | 0.84 |

---

## 2. Finding A — the stutter is `worldgenList`, and it has ~2.2x'd

**`worldgenList` is streaming chunk generation** (`PT_GENLIST`, submitted
mid-frame from `Stream::FillSlots`). `Stream::ShiftAxis` builds a plane of
`kNChunk^2` = **1,024 slots**, evicts it, moves the origin, and hands the whole
plane to `FillSlots` as **one dispatch**. There is no per-frame budget on it.

Per-occurrence cost, from `build/perf_diag.json`:

| scenario | fires | per occurrence | share of frame |
|---|---|---|---|
| flythrough | every ~14 frames | **11.6 ms** | doubles a 20 ms frame |
| surface-sprint | every frame (`windowShifts` mean 0.9) | **9.3 ms** | **41% of 22.7 ms** |

The flythrough spike frames are `2,3 / 17,18 / 32,33 / 47,48 / 62,63 / 77,78 /
92,93 / 107,108` — a period of exactly 15, two frames wide, ~2x the mean. That
regular doubling is what "very stuttery" is.

**The regression.** `docs/RESEARCH_streaming_hitch.md` recorded, on the SAME
`surface-sprint` scenario and the same GPU on 2026-09-04:
`worldgenList` **4.59 ms** median per plane, "98% of the remaining streaming GPU
bill". Today it is **9.30**. Two times.

**Why, and it is not a mystery.** The environment-truth wave landed between
those two measurements — P-F (`8a92a6a`, 09-05: water presets drive pond
geometry, authored water sites, pond lattice) and P-G/P-I (`48e78e9`, 09-07: the
terrain is authored on the map — per-biome relief curve blended over map cells,
landform sites overlaid, the 68 `worldgen.*` knobs deleted). `worldgen.wgsl`
moved ~1,350 lines over that span, and all of the new work is **per column**:

- a `BiomeMix` bilinear over the landform plane,
- an 8-knot Catmull-Rom relief curve with tangents (`curveOne`), blended
  per-biome,
- `landformOctave` sampling the painted plane,
- the pond lattice (`col.pond`, `col.fluidTop`),
- `wmSiteTopAt`.

262,144 column evaluations per shift plane (1,024 chunks x 256 columns).

**And the sky pays all of it.** `genChunk`'s early-out at `worldgen.wgsl:4512`
is `if (!wmFlag(col.biome, WM_BF_CACTI) && base.y > colTop) { write air;
continue; }` — but `colTop` is
`max(col.h + skyMargin, col.fluidTop, col.pond + 1, trees.top, wmSiteTopAt(...))`,
so reaching the early-out requires the **entire** column preamble first,
including `trees.top`, which is a 25-candidate tree-lattice scan (`reach 115,
above 217` per the startup line). P3-F measured **~586 of a plane's 1,024
chunks as pure sky**. Those 586 workgroups today run the full new per-column
preamble 256 times each and then write 16 KiB of zeros.

---

## 3. Finding B — the MPM solver never sleeps near the authored lake

`--perf --scenario idle`, a **settled** world:

- `activeChunks` p50 = **0** — the CA is fully asleep. `ab6ce9c`'s
  free-surface fix works, and this confirms it on a second harness.
- `fluidParticles` (= `world.Snap().fluidLive`, a real GPU readback) = **7,680**,
  flat for all 240 frames, against `sim.fluidExciteCeiling` 8000.
- The 9-substep solver table therefore runs every tick:

| pass | samples / 240 frames | us/frame |
|---|---|---|
| fluidG2p | 2700 (9/tick) | 936.6 |
| fluidP2g2 | 2700 | 753.5 |
| fluidP2g1 | 2700 | 542.1 |
| fluidClear | 2700 | 526.2 |
| fluidGridUp | 2700 | 498.9 |
| | | **3.26 ms/frame** |

That is **18% of the 18.84 ms idle frame**, and idle's whole GPU compute bill is
only 5.74 ms — so **MPM is 57% of the sim tick in a world where nothing is
happening**.

`Simulation::EncodeTick`'s gate is correct:
`seamActive = fluidCount > 0 || fluidSpawnCount > 0 || (exciteOn && cx.caActive)`.
`exciteDetect` only walks the dirty list, which is empty. So nothing is
*re-*exciting: **7,680 particles were excited once and never settle back**,
despite `sim.fluidSettleTicks` = 24 and 240 ticks of stillness. The most likely
mechanism is `settleCheck`'s stability veto being permanently true in a lake
interior — the same predicate `ab6ce9c` changed the CA half of.

`src/measure/perfnodes.h` states the contract this breaks in as many words:
*"MLS-MPM Fluid ... Sleeps to 0.0 ms when no water is excited."*

**The date fits the owner's report.** `home_lake` is an **authored** water site
(`map.json`, `preset: spawn_lake`) that landed 2026-09-05 in `8a92a6a`. Before
that, the spawn area had no standing lake, so this cost did not exist where the
owner plays.

The `sleep` gate prints `0 particles alive` at the same tarn — so either the
gate settles longer than 240 ticks, or its fixture differs from the perf
harness's. Reconciling those two is the first task in P2 below.

---

## 4. Finding C — the renderer is NOT the regression

noon overlook baseline is **13.52 ms today vs 19.6 ms on 2026-09-05**. The
lever ranking is unchanged from `PLAN_frame_perf.md` §1: the fine 10 cm march is
8.06 of 13.52 ms and is still the largest single render lever in the engine.

The glow field, which landed today, is the only new render cost: **0.84 ms**
of raymarch plus 0.045 ms of compute (`glowSrc` 4.6 + `glowField` 3.9 +
`glowRefresh` 36.7 us/frame at idle). It is not a regression at that size, but
`glowRefresh` is 64 chunks fully scanned **every tick unconditionally**, which
is a standing cost in a world where nothing changes — see P4.

---

## 5. Finding D — presentation pacing amplifies all of it

`render.presentMode` = 1 (mailbox), `render.fpsCap` = 0, and measured frame
times are 18.8–22.7 ms. On a 60 Hz display that is a 1-or-2-vblank alternation
(16.7 / 33.3 ms) with nothing damping it: GPU time can be *steady* while the
picture is *uneven*. `PLAN_frame_perf.md` already records that FIFO quantised a
22 ms frame to 33, which is the same fact from the other side.

This costs nothing to test and no code, so it goes first (P0b).

---

## 6. Finding E — a `--render-budget` arm pays a 496 s cold compile

The `halfres` arm printed `far-cascade pipelines ready 496.1 s after the first
pipeline create`. `b2e1408`'s cache-key filtering (a shader's key names only the
tuning consts it can see) does not cover a render-SIZE change, so changing the
offscreen dimensions re-keys `worldgen.wgsl`'s `far` entry and pays the full
cold-compile cliff. Any session running `--render-budget` with `halfres` eats
eight minutes for it. Cheap fix, listed as P5.

---

## 7. The plan, ranked by ms per unit of effort

### P0a. Attribute `worldgenList`'s new cost inside the kernel — do NOT A/B features off

CLAUDE.md rule 6, and this file's own history is the proof: 58 page faults cost
14 elimination runs and arrived nowhere; a reporter cost 4 and printed the
answer. **A bare "9.3 ms" is not a measurement.**

Add to the `worldgenList` path, in one change:

1. A per-plane counter split of chunks by verdict: **sky-skipped / generated /
   store-hit**, so "586 of 1,024 are sky" is a live number and not a
   2026-09-03 memory.
2. A per-column timing or instruction-count proxy for the preamble terms
   (`BiomeMix` + `curveOne`, `landformOctave`, pond lattice, `trees.top`,
   `wmSiteTopAt`) — cheapest form: five `--sweep`-able consts that fold each
   term to its identity, so `--sweep` prices them without a rebuild.

**Cost:** one C++/WGSL change, one `--gate streaming` run.
**Buys:** the split of 9.3 ms across the five suspects, all at once.

### P0b. Ask the owner one question before optimising anything

What is the monitor's refresh rate, and does `render.fpsCap = 30` +
`render.presentMode = 0` (FIFO) *feel* smoother than today's uncapped mailbox?
That is a `tuning.json` edit and an F5 — no build, no run. If a capped 30 feels
better than an uneven 45, that reframes every target below.

### P1. Do not generate the sky — the 57% lever, already scoped

`RESEARCH_streaming_hitch.md` names this and objects that it "needs a CPU mirror
of `colTop`, which does not exist; `World::TerrainHeight` is the GROUND by
contract and is not it." **That objection is softer than it reads**, because the
test only has to be conservative — over-estimating the ceiling only declines a
skip, which the shader's own comment says. Two forms, do the cheap one first:

- **P1a (in-kernel, no CPU mirror, no invariant pair).** Add a cheap
  conservative PRE-test in front of the exact one, as a two-tier ladder, so no
  skip can be lost: compute `col.h` and `skyMargin` (which the exact test needs
  anyway), then test
  `base.y > col.h + skyMargin + kMaxTreeAbove + kMaxSiteAbove` and take the
  early-out *before* `trees.top`, the pond lattice and `wmSiteTopAt` are
  evaluated. The tree atlas already publishes `above 217` at load. A chunk that
  FAILS the cheap test falls through to the existing exact `colTop` and is
  decided as it is today, so the ladder is strictly additive: a chunk hundreds
  of voxels above ground exits on the cheap branch, and only the one or two
  chunk layers straddling the ceiling pay both tests.

  **What it can and cannot save.** It removes the tree-lattice scan, the pond
  lattice and `wmSiteTopAt` from sky chunks. It CANNOT remove the `BiomeMix` +
  `curveOne` + `landformOctave` preamble, because `col.h` is the input to the
  cheap bound itself. That is the ceiling on this item, and P0a's split is what
  says whether the half it can reach is the expensive half. WGSL-only, so
  **no rebuild**.
- **P1b (CPU-side, removes the workgroup entirely).** `TerrainHeight` +
  a constant margin -> install `PT_EMPTY` and leave the slot out of `genList`.
  This is the original P4-G. Bigger, and it is a "two places must agree" pair
  that `check_invariants.py` has to police. Do it only if P1a's number says the
  remaining sky cost is still worth it.

**Expected:** P1a alone should recover a large part of the 4.59 -> 9.30 move,
because the new per-column work is exactly what it stops paying on sky.

### P2. Make the MPM population settle

1. **Reconcile the two harnesses first** (one run, no code): the `sleep` gate
   reports `0 particles alive` at the tarn; `--perf --scenario idle` reports
   7,680 in the same window. One of them is not measuring what it says.
2. Then instrument `settleCheck`'s **refusal reason** the way `voxStore` got its
   dropped-word probe — count refusals by predicate branch into spare
   `fluidArgs` words. Do not turn triggers off one at a time.
3. The fix is whatever that names. A plausible candidate given `ab6ce9c` is that
   the settle veto still evaluates the pre-change lateral predicate, so a lake
   interior is permanently "unstable to settle into" while the CA half now
   refuses to move there at all.

**Buys:** 3.26 ms/frame (18%) back at idle wherever there is standing water —
which, since P-F, is the spawn area.

### P3. Spread the shift plane — but ONLY in the bursty regime

R2 (generic worldgen spreading) was **built and rejected with numbers** in
`a2ce7ad`: "at ~1 shift/tick spreading is the identity and adds 28%". That
verdict is correct for `surface-sprint`, which shifts ~0.9 times per frame and
has nothing to spread into. It says nothing about `flythrough`, which shifts
every ~14 frames and is where the 15-frame stutter lives.

So: split the plane into sub-batches **only while the shift rate is below one
per N frames**, and fall back to a single dispatch when saturated. Do this after
P1 — if P1 takes 11.6 ms to ~5, the burst may stop mattering and this becomes
unnecessary work.

### P4. `glowRefresh`'s unconditional 64-chunk walk

36.7 us/frame at idle is small, but it is a full cell scan of 64 chunks every
tick in a world where nothing changed, and it is 12 hours old. Give it the
`opennessRefresh` treatment (skip a still world) before it becomes load-bearing.

### P5. Stop `--render-budget`'s size arms re-keying `worldgen.wgsl`

Extend `b2e1408`'s cache-key filtering so the offscreen render dimensions are
not part of `worldgen.wgsl`'s key. Eight minutes per budget run, for every
session that measures.

### P6 (separate track). The fine 10 cm march — 8.06 ms of 13.52

Unchanged in rank since 2026-09-05 and unrelated to this regression. Candidates
are already ranked in memory `project-raymarch-accel-research` and
`PLAN_frame_perf.md` §3. Do not start here: it is the biggest number but the
regression the owner reported is P1 + P2, and those are recoveries rather than
new engineering.

---

## 8. What this plan deliberately does NOT do

- **It does not blame a moved hash.** `determinismHash` is `ae16bd40` and
  `--gate determinism` passes; nothing here is a determinism claim.
- **It does not re-run a suite.** Every number above came from two launches, and
  the 2026-09-04 comparison numbers were read out of a doc, not re-measured.
- **It does not A/B features off to find the worldgen cost.** See P0a.
- **It does not touch the renderer**, which is 6 ms faster than it was on
  2026-09-05 and is not what regressed.
