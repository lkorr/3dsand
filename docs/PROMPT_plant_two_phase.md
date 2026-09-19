# Agent brief: resolve plants in a SECOND PASS, not inside the DDA

You are implementing a renderer optimisation in the sandvox engine. Read
`CLAUDE.md` first and obey it — especially the board (`scripts/board.sh`), the
run mutex (`scripts/run.sh`, never launch the exe directly), and "verification
is a BUDGET". **Do not start until the tree is clean of other sessions'
uncommitted C++** (check `git status`); this package needs a build and linking
over someone else's half-finished `tuning.h` will produce breakage neither of
you can attribute.

## The problem, already measured — do not re-derive it

`tracePlant` (`assets/shaders/raymarch.wgsl`) is called from **inside
`trace()`'s DDA loop**. Its register live ranges interleave with the march's
(`out : Hit` has 26 fields, plus `tMax`, `tDelta`, `cell`, `tCur`, `stepv`, the
media accumulators and the `pKey`/`pHit`/`pT` plant memo), so the allocator
spills — and **every pixel in the frame pays the fill traffic, including sky
pixels that never touch a plant.**

Evidence (2026-09-11, RTX 3060 Ti, 1080p; full detail in
`docs/PLAN_frame_perf.md` and `DESIGN.md` §9 analytic plants):

- `--render-budget --budget-cams meadow`: `microMaxPerRay` 4 → 0 saves
  **7.41 ms of a 15.70 ms frame (47%)** on a frame whose own counters read
  `rmMicroEnters = rmPlantEvals = rmMicroSteps = 0.0`. **Nothing is evaluated.**
- `--shader-stats`: register count is **168 either way**, but local memory is
  **+160 B/thread with plants and +16 without**. It is spill, not occupancy and
  not traversal.
- Partitioned with the `PLANT_GRASS_ON` / `PLANT_FLOWER_ON` / `PLANT_SHROOM_ON`
  / `PLANT_FERN_ON` compile gates (already in the file next to the `PK_*`
  constants; all-true is byte-identical to the pre-gate build): flower 64 B,
  grass 48 B, mushroom 16 B, fern 16 B. **Grass + flower = 112 of 144, and they
  are the COLUMN plants — the common case — so per-kind specialization is dead.**
  Those four sum to exactly the 144 B delta, which also means `traceMicro` (the
  non-plant `MATF_MICRO` brick path in the same branch) costs ~0. Leave it alone.

**Approaches already refuted — do not spend runs on them:**
- A per-kind variant (recovers at most 32 B: fern + mushroom only).
- A `SPEC_PLANTS` lean pipeline: the mechanism exists and is dormant
  (`Simulation::BuildRaymarchVariant`, `kRaymarchVariantOn`), but it only helps
  frames with no plants nearby and needs a "can a plant be hit" oracle the
  engine does not have. Explicitly rejected by the owner as not robust.
- Anything that makes plant EVALUATIONS cheaper or rarer (per-chunk plant-layer
  tops, per-column extent bakes, mid-distance grass LOD, a Chebyshev distance
  field — candidates 1–4 of `project-raymarch-accel-research`). The measured
  cost of every evaluation on that frame is zero because there are none.

## STEP 0, NON-NEGOTIABLE: give the harness a world with plants in it

**The current test world holds 756 plant cells in the whole 512³ window and
ZERO leaf cells.** `--render-budget`'s `canopy` camera DECLINES for want of a
tree. This means:

- You cannot currently detect a grass-rendering regression. The `plants` gate
  passes on almost nothing.
- You cannot measure whether your change helps a real meadow, which is the
  entire point of the work.

Before touching `trace()`, produce a vegetated fixture — a seed/map/biome
selection or an authored `.svedit` layer that puts dense column plants and a
canopy in the harness window — and confirm `--render-budget --budget-cams
meadow,canopy` reports non-zero `rmMicroEnters` / `rmPlantEvals` and that
`canopy` runs. Record the new baseline arms. **If you skip this you are shipping
unverifiable work.**

## What to build

Split plant resolution out of the march:

- **Phase 1 (inside the DDA).** When a primary ray enters a plant cell with
  budget remaining, do NOT evaluate it. Record `(tEnter, cell, mat)` and keep
  marching, treating the cell as air. Charge `microBudget` exactly as today.
- **Phase 2 (after the march resolves).** Walk the recorded cells in `t` order
  and evaluate `tracePlant`. The nearest plant hit closer than the opaque hit's
  `t` wins; otherwise the opaque hit stands.

An equally acceptable shape, if it measures better: record only the first plant
cell and have `fs` issue a second, plant-aware `trace()` for the pixels that
recorded one. That is the honest "two-pass" reading and may spill less; decide
with `--shader-stats`, not with taste.

### Hazards, all of which have already bitten someone here

1. **`trace()` returns from many points inside the loop.** Phase 2 needs a
   single tail. Converting those to `break` with a flag is invasive in the
   hottest function in the engine — consider instead doing phase 2 in a thin
   wrapper that calls `trace()`, with the records in `var<private>` globals (the
   `gRs` pattern).
2. **Do not use a dynamically indexed `var<private>` array for the records.**
   WGSL puts those in local memory, which is the exact thing you are trying to
   remove. Use a fixed, statically indexed set of scalars (`microMaxPerRay`
   ships at 4).
3. **Preserve the LOD and budget semantics exactly.** Past
   `min(TUNE_MICRO_LOD_DIST, TUNE_PLANT_LOD_DIST)` a column plant is a solid
   proxy cube; a tile plant past `TUNE_MICRO_LOD_DIST` keeps only its centre
   column. Once `microBudget` is exhausted a plant cell becomes a SOLID cube,
   not air — a ray that treats exhausted plant cells as air will see through
   meadows.
4. **`tracePlant`'s last argument is the clip bound AND the footprint cull
   bound.** It derives `segLo`/`segHi` (the per-blade XZ reject) from
   `entry.xz + rd.xz * tHiIn`. Passing `1e9` blows that box up to the whole
   world and silently disables the cull that makes the blade loop cheap. For a
   column plant the correct unclipped bound is the ray's exit from the column's
   XZ square (`min(tMax.x, tMax.z)`); for a tile plant it is `1e9` because
   `plantTileAt` bounds the footprint. This is already correct in the file —
   keep it correct.
5. **The plant memo (`pKey`/`pHit`/`pT`, `plantTileKey`/`plantColumnKey`)** gives
   one evaluation per plant per ray. In a deferred design it may become
   redundant or may need re-keying. Do not silently drop it — if you remove it,
   say so and show the cost.
6. **Media, stain and the liquid/gas side effects** of a cell must not be lost
   by treating a plant cell as air in phase 1.

## How to iterate — the oracle is fast

**`--shader-stats` is the measurement loop, not frame timings.** One run
(~2 min) prints `Local Memory Size` for the `raymarch` fragment; subtract
`68719476736` for bytes/thread of spill. Baseline is **160**; the floor with no
plant code is **16**. Iterate against that number. Only once spill has moved
should you spend a `--render-budget` run.

WGSL-only edits need **no rebuild** (`LoadShader` reads `assets/shaders/*.wgsl`
at launch and the SPIR-V cache keys on source). Validate with
`bash scripts/check_shaders.sh`. Keep constants out of `common.wgsl` — an edit
there is a cache miss for all 23 shaders and was measured at 536 s.

## Acceptance

- **Spill materially below 160 B/thread.** Target ≤ 80. Report the number
  whatever it is; a negative result honestly measured is a valid outcome and
  should be written into `docs/PLAN_frame_perf.md` so nobody retries it.
- **`--render-budget` on the NEW vegetated meadow/canopy cameras** shows the
  improvement, with the before/after both measured on the same world.
- **Gates green:** `plants`, `player-plants`, `screenshots`, `body-shade`,
  `determinism`. All of this is render-only — **`determinismHash` must NOT
  move.** If it does, you have touched sim state and that is a bug, not a
  rebaseline.
- **Image parity:** plants must look the same. Use `--shot` frames on the
  vegetated world before and after and diff them; the gates cannot see this.
- Update `DESIGN.md` §9 (analytic plants), `docs/PLAN_frame_perf.md`, and
  `ARCH_NODES` in `assets/tuner.html` in the same commit.

## Known-failing gates on this tree (NOT yours)

`env-reload`, `waterbody`, `ca-level-pond`, `fluid-excite`, `fluid-onwater`,
`body-shade` fail on clean `HEAD` and are unrecorded in `tests/baseline.json`
(verified 2026-09-11 by a full selftest on a reverted tree — identical 13
failures, identical `body-shade` ratio 0.15, identical determinism hash
`eb284643`). They will make the suite say "REGRESSIONS" and will refuse a
`--rebaseline`. Do not chase them and do not claim them.
