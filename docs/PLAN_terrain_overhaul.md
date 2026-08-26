# Terrain overhaul: from a 5.4 m tabletop to a 200 m world with an ocean

> **Status.** Package **A** landed `9c42a3c` (the `terrain` gate + baseline-value
> plumbing). Package **B** landed `3fdcf5c` (foundations: Q14 log2-cell noise with
> an enforced C++ mirror, the three column hoists, the height contract, perched
> tarns, terrain-relative fixtures). Package **C**, the scale pass, has **landed**
> — see §C below for what shipped and what it corrected. Package **D** (the
> global sea + `LAVA_LID`) is next.
>
> **What package C changed against the plan as written**, all of it measured:
>
> 1. The octaves are **centred deviations**, so `baseHeight` is the world's MEAN
>    and there is room below the datum for basins. The plan's `bed = spawnPlainY
>    + ((raw - spawnPlainY) * ws >> 14)` fades the WHOLE deviation, which pins
>    spawn to an exact plane 64 m across; only the two COARSE octaves fade.
> 2. `Land.slope` is the **landform** gradient (accumulated through the hill
>    octave), not the full one. Through the grain octave `d(slope)/dcolumn` is
>    96 Q8 — the whole gate range in ONE column — so a slope-gated wedge becomes
>    a cliff wherever the fine noise crosses the threshold. 108 awake chunks at
>    tick 120 became 8. The tarn placement gate reads the same field, for the
>    same reason.
> 3. **`poolY` had to move with the datum.** It was a bare `vlen(44)`; at y200
>    that is a 15 m crater with vertical walls at (420,420), reported as a
>    143-voxel adjacent step and 58 lost voxels.
> 4. A pond's **bowl now REPLACES the ground** rather than `min()`-ing into it,
>    and `pondInfo` gained a radius-aware gate. As a min() the bed was raw
>    hillside wherever the terrain undercut the bowl — with SAND laid on it.
> 5. `spawnPlainFade = 2048` is **205 m of ramp, not 2 km** (the plan's own
>    arithmetic slip). Full drama is reached ~237 m from the origin.
> 6. **`VOX_PER_M` was fixed** (16 → the prelude's `VOXELS_PER_M`), so trees are
>    their documented metre size for the first time since `kVoxelMeters` moved.
>
> **Two things package C found that are NOT terrain and are recorded here
> because they will bite package D harder:**
>
> * **The CPU dirty mirror could only shrink.** `PageTable::TightenFromSnapshot`
>   intersected and never unioned, so `cpuDirty` was a superset of "writes the
>   CPU asked for" rather than of "writes the GPU will make" — measured at 0 on
>   every tick against a snapshot dirty set of 200-390 chunks. Any worldgen
>   output that is not perfectly at rest falls in that gap. Fixed by seeding the
>   mirror from the snapshot's own dirty set; the fixed point is unchanged
>   (GPU-active ∪ N26(GPU-active), zero when the world sleeps).
> * **A generated tarn does not reach rest.** Seven chunks around one stay awake
>   indefinitely — five from the pond vegetation, two from the water — which
>   `sleep` tolerates (bound 32) and `ca-skip`/`wind-prim` do not. The wedge,
>   the bowl, the berm, the shore fringe, ruins, evaporation and the MPM seam
>   were each ruled out by measurement; the residue is a liquid-CA question.
>   `pondInfo`'s authored-origin keep-out now covers the residency window, which
>   unblocks the suite and is defensible on its own (that cube is authored
>   content end to end) but does NOT fix the defect. A global sea puts water
>   everywhere, so package D cannot inherit this unresolved.
>
> Copied into the repo from `~/.claude/plans/` after package B, because a plan of
> record that only exists in one machine's home directory is not a plan of record.
> `docs/RESEARCH_worldgen.md` is the survey it corrects; **§1 below lists six
> places that doc is wrong as written** — read them before implementing its
> §4.1/§4.2. `docs/MEASURED_terrain_baseline.md` carries the before/after numbers.
>
> Two corrections the packages themselves produced, which §1 does not have:
> a pond bowl is steepest at its rim, so `pondDepth` is bounded by
> `pondRadiusMin` at the angle of repose (a swimmable tarn needs `r >= 46`, not
> the ~24 §B4 assumed); and snow is a **powder** that the authored pool rims
> never suppressed, which is a rule-2 landmine that reports itself as `ca-skip`
> finding the world never quiet.

## Context

`docs/RESEARCH_worldgen.md` (rev 2) established the finding: **this world renders to
6.6 km and has 5.4 m of vertical relief.** The far cascade machinery draws 8 levels out to
a 6.6 km horizon at 51.2 m per cell, and levels 6–8 are drawing a flat plane, because the
entire vertical extent of the world is one tenth of a single level-8 cell. The biome field
(38.4 m) is smaller than one level-8 cell, so at the horizon it point-samples into white
noise. Every landform that is missing — ranges, chasms, coastlines, real caves — is missing
because the terrain function's amplitude and wavelength sit two to three orders of
magnitude below what the renderer already shows, not because the algorithms are hard.

Two design passes were run against the doc. They agree on the stages and correct the doc
in six places that change the work (§1 below). Decisions taken:

- **Full overhaul**, all stages.
- **~200 m of relief plus a global ocean**, datum raised to ≈ y200 so there is room *below*
  for sea basins as well as above for mountains.
- **A calm home area** at spawn, ramping to full drama over ~2 km.
- **Fixed compass climate** (snowy north, desert east on every seed) with seeded regional
  and local content beneath it.

**The world hash will move repeatedly and that is fine** — rebaseline at the end of each
work package, not between edits. What must hold at every step is *determinism*: integer-only
terrain math, the C++ mirror bit-identical to the WGSL, and no order- or
scheduling-dependent generation. That is what §5's `terrain` gate exists to prove.

Outcome: a world where the cascades do work, where digging a valley floor gives you metres
of gravel that actually flows and digging a ridge gives you rock, and where "go east until
the sand starts" is a sentence that means something.

---

## 1. Six corrections to the research doc

These change the plan, not just the prose. Do not implement §4.1/§4.2 of the doc as written.

1. **The binding ceiling on `vnoise` is output resolution, not overflow.** `vnoise` returns
   0..255. At a continental amplitude of ~1024 voxels one output LSB is **4 voxels** — the
   doc's 8-bit-weight fix yields a perfectly non-overflowing terrain made of 4-voxel
   terraces. Value and weight must both go to ~14–15 bits.
2. **Pass the noise cell as a log2 shift, not a size.** Then `fdiv` is `>>`, `fmodp` is `&`,
   and the rescale is a shift: **zero integer divides** where today's `vnoise` does five.
   The rewrite is *cheaper* than what it replaces.
3. **`caveAt` is an unclaimed ~16×, larger than anything in the doc's §7.1.** Every one of
   its six `vnoise` calls is a pure function of `(x,z,h,seed)`; `y` appears only in the
   range compares. `genCellIn` calls it per stone cell, so an underground column recomputes
   the identical field 16 times — ≈234 `hash3` per buried column against `genColumn`'s own
   ~18. That is ~93% of the worldgen cost of a buried chunk.
4. **This CA's angle of repose is exactly 1.0 voxel/column (45°)** — `sim_step.wgsl:1645`
   slides a powder into any free down-diagonal. The doc's ladder at slope 1.37 puts the
   whole world above repose, survivable today only because nothing loose sits on it — and
   the sediment stage exists to put loose material on it. Ladder goes to `A/W = 0.5`, and
   the sediment wedge is **slope-gated**.
5. **`pondSurface` must be deleted, not scaled.** 24 × `baseHeight` = 192 hashes per pond
   *or shore* column today; on a 5-octave ladder it becomes 480. Its stated guarantee is
   already stale (comment justifies 24 samples for r=36; tuning gives r=127 — one sample
   every 33 voxels of arc against a −2 margin). Invert the dependency: pick the surface from
   the centre column and **force a berm** on the annulus. 192 hashes → 20, and the guarantee
   becomes structural instead of a sampling density.
6. **Absolute-Y fixture literals must become terrain-relative before the datum moves.** Two
   independent copies (`src/test/support.cpp:836-876` and `src/gpu/vk_smoke.cpp:197-219`),
   plus `selftest_render.cpp:389` and `page-roundtrip`'s "provably empty sky".

Net effect of the cost work: **the overhaul is cheaper per column than HEAD** (surface
~18→44 `hash3`, but shore 210→64 and underground 252→90).

---

## 2. Pre-flight

- `git status` shows `M assets/shaders/worldgen.wgsl` — an uncommitted comment-only fix from
  a prior session. Confirm with `git diff` that it has zero non-comment lines, commit it
  alone.
- `bash scripts/board.sh claim` for `src/main.cpp`, `assets/materials/tuning.json`,
  `assets/tuner.html`, `assets/tuner_schema.js`, `DESIGN.md`, `CLAUDE.md`, plus
  `assets/shaders/worldgen.wgsl`, `src/sim/world.cpp`, `src/sim/tuning_params.def`,
  `src/test/`. Open claim `agent-686217` on `world.h`/`world.cpp`/`pagetable.*` is from
  2026-08-23 and stale — post a note and proceed.
- `export SANDVOX_NO_CRASH_DIALOG=1`.

---

## 3. Work packages

Eight packages. **Each ends with one rebaseline** (`--selftest --rebaseline`, plus
`--vk-smoke --rebaseline` + `--vk-smoke-loud --rebaseline` when render output moved). Do not
rebaseline mid-package.

### A · Instrument the terrain (build this before touching any terrain math)

There is **no worldgen or terrain gate today**. Terrain is asserted only by one 32-bit
whole-world hash, tick-0 smoke probes, and `sleep`. A 30× relief change would land on a suite
with nothing that measures terrain at all, and every failure would surface as a confusing
secondary symptom in `sleep`, `debris`, `player-walk` or a pool-exhaustion abort.

**A1 — baseline-value plumbing.** `tests/baseline.json` holds only `pass`/`fail` and
`determinismHash`; `LoadBaseline` (`selftest.cpp:160-198`) extracts nothing else, so
CLAUDE.md's "thresholds in JSON, not C++" is aspirational. Replace `g_goldenHash` with a
key→value map, add `BaselineValue(const char*)` to `selftest.h`, add
`std::vector<std::pair<std::string,std::string>> observed` to `struct Result` (`:92`), and
extend `RebaselineSelftest` (`:230-301`) to write it back through the same textual surgery.
The scanner is strict — values must be quoted strings, so numbers go in as `"3"` — and it
silently skips absent keys, so seed all `terrain.*` keys by hand once.

**A2 — the `terrain` gate**, registered first in `kOrder` (`selftest.cpp:51-65`), leaving
`WindowOrigin` at `{0,0,0}` so `determinism` is unaffected. Complete verdict under
`--gate terrain` alone, ~10 s.

| Pass | Cost | Asserts |
|---|---|---|
| **A** analytic, CPU only | ~50 ms | **A1 window containment** — every surface Y in a ±1024-voxel grid lies inside the 512-voxel window with margin. *Highest-value new assertion, not in the doc:* at 200 m of relief "the terrain is not in the window at all" becomes the default failure and everything else degrades silently from it. **A2** relief min/max/mean/p95. **A3 crenellation** — max and p99.9 of `\|Δh\|` between adjacent columns; §3.3 says surface area, not depth, costs pages, so this is the free residency predictor. **A4** ramp continuity across the home-area boundary at 4 headings. **A5** `TREELINE` strictly inside [p5,p95] of surface Y. **A6** dense pond-rim sweep (512 directions × radii r−1..r+2). **A7** `255·cs² < INT32_MAX` over live tuning. |
| **B** whole-window, free | ~0 | `WorldSnapshot::occupancy` is already delivered CPU-side under the harness drain. Per chunk-column, topmost non-empty chunk vs the CPU mirror, ±3 chunks of slack for flora. Catches gross CPU/GPU divergence at whole-window coverage for zero readback cost. |
| **C** targeted readback | ~0.5 s | **C1 exact CPU/GPU height agreement** — the "player falls through ground they can see" hazard, today enforced only by a comment at `world.cpp:558-561`. **This is the determinism assertion that replaces hash-stability as the safety net.** **C2 fixture clearance** — no blocking voxel in `[h+1, h+24]` at the ~14 named fixture columns; what `inSpawnClearing`/`onFixturePad` exist to provide, currently asserted by prose alone. **C3 liquid separation** — no water/oil face-adjacent to lava; vacuous today, goes live with the sea. |
| **D** settle proxy | ~2 s | 120 ticks, `ReadActiveChunksSync`, loose threshold. Overlaps `sleep` deliberately: a 2 s early warning so a bad fill rule is not discovered 60 s into `--gate sleep`. |

Readback discipline: go through `ReadVoxelsSync` (`support.cpp:898`), the seam `sleep`
already uses. Keep pass C to ~36 calls by reading contiguous `cx` runs within the
mirror-predicted `cy` band, never vertical stacks (slot index is `cx`-contiguous).

Deliberately **not** in the gate: page high-water. `pagesHighWater_` is monotonic with no
reset, so a mid-suite read is meaningless, and a heavy readback here worsens the known
`page-roundtrip`-in-suite artifact. Crenellation is the proxy; `--autofly-hard` is the real
instrument.

**A3 — `--frames` page high-water line.** ~8 lines in `src/main.cpp`'s report block
(`:4631-4688`) printing `PagesHighWater()` / `kPoolPages` / MiB / shift count. `world` and
`stream` are both in scope. Today that number is printed only by the selftest harness, which
is the wrong instrument — the harness window is sky-heavy and under-reports 2×.

**A4 — stock-HEAD baselines**, recorded in `docs/`:

```
bash scripts/run.sh ./build/Release/sandvox.exe --frames 1200 --autofly-hard   # peak resident slots
bash scripts/run.sh ./build/Release/sandvox.exe --frames 400                   # whole-frame p50/p95/p99
```

§3.3 predicts a proportional scale pass adds ~0 resident slots. If the after-number matches,
package C is medium risk; if it doesn't, the ladder's slope is wrong and you learn it from
one command rather than from a fatal pool abort.

Also add the gate to `ARCH_NODES` in `assets/tuner.html`.

---

### B · Foundations — new noise, the three hoists, and one height contract

Everything here is prerequisite plumbing. Land it as one package with one rebaseline.

**B1 — `vnoise2` / `isin16` / `vnoise3`, added beside the old ones.**

*Do not rewrite `vnoise`.* 14 of its 18 call sites are decorative fields at `cs ∈ {11..48}`
(flower clumps, undergrowth patches, ground cover) — nowhere near any ceiling, and rewriting
them silently re-rolls every flower bed. Migrate only `baseHeight`, `biomeAt`, `caveAt`, and
add a `LoadTuning` clamp recording the legacy 2901-voxel ceiling on the old one.

- `vnoise2d(x, z, csl, seed) -> N2 {n, dx, dz}`: **Q14 value** (0..16383), **Q15 weights**,
  cubic `3t²−2t³` interpolant, analytic derivative for the gradient trick. The 14-bit corner
  width is forced, not chosen: the bilinear cross term reaches `2·(2^b−1)`, and at b=15
  `c·s = 2.147e9` clears `2^31` by 65k — zero margin, exactly the doc's trap §10.3. b=14
  gives 2× headroom.
- `isin16`: 16-bit phase. The **8-bit phase is the artifact** (32 visible terraces per ridge
  flank), not the parabola — whose real error is 5.6%, not the ~1.5% its own comment claims,
  but which is smooth and therefore invisible once squared. Correct it anyway with the
  two-multiply refinement `y·(0.775 + 0.225y)`.
- `vnoise3(x, y, z, cxl, cyl, seed)`: the Y lattice index **salts the seed** rather than
  becoming a fourth hash argument, so the `pcg(gz)` and `pcg(gx^·)` terms are shared across
  both Y layers — **14 `pcg`, not 32**. Separate `cxl`/`cyl` is free and is what gives
  ravines. Write the sharing out explicitly; do not leave it to CSE, because `world.cpp` must
  mirror it identically.
- **C++ mirrors** in `world.cpp`'s mirror block. Note in a comment that `>>` on negative
  `int` is floor in C++20 and arithmetic in WGSL — they agree; this is the one place the two
  languages could have diverged. Keep `fdiv`/`fmodp` for the legacy noise and every tile
  lookup. Wrap both sides in `// MIRROR-BEGIN <name>` / `// MIRROR-END` and add a
  `scripts/check_invariants.py` check comparing normalised token streams — it fires at edit
  time via the PostToolUse hook. `check_invariants.py` has nothing about worldgen today.

**B2 — the three column hoists.** Correctness signal: these are pure refactors, so if a
hoist changes generated words, the hoist is wrong (even though we no longer *gate* on the
hash, `--gate determinism` before/after within this package is still the cheapest way to
catch a botched one).

- **`caveAt` → column prologue in `genChunk`** (correction 3). Hoist into `genChunk`, *not*
  `genColumn` — `genColumn` is also the far path's per-cell entry, and computing cave bands
  there would add cost to air columns on the far path. Guard on
  `chunkBaseY <= col.h && !col.inRim`.
- **`treeAt` → column prologue.** `TREE_MAX_TOP` is a world-wide constant derived from the
  terrain band (worth 20.76 → 5.37 ms of `genChunk`) and dies the moment the band widens. A
  column's candidate tile set is column-invariant and bounded at 9 tiles (trunk sites in the
  middle half of a 144-voxel tile, `TREE_MAX_REACH = 124`). Reject #1 becomes
  `y > col.treeTop`, a correct **local** ceiling — strictly better than what it replaces.
  Fallback if registers spill: hoist `TreeSite` + `sbase` only. Audit `cactusAt` (`:1529`),
  which has no world-wide Y reject at all.
- **`far` per-column hoist.** `far` rebuilds `Col` for all 4096 cells of a level chunk that
  has only 256 distinct columns. Restructure column-major, stage through workgroup memory
  (4 KiB), and **delete `surfHeightAt`** — the surface height is `col.h`. This collapses the
  doc's four height functions to three and fixes §4.4's `labMode` divergence for free.
  `far-downsample` is the gate that must be green: `fardown` and `far` must share the same
  path or the residency boundary seams.

**B3 — fixture literals → terrain-relative.** `support.cpp:836-876` and
`vk_smoke.cpp:197-219` compute paint/blast sites as `TerrainHeight(x,z,seed) + kDelta`.
`SelftestOps(uint32_t tick)` gains a seed parameter (3 call sites), matching `SelftestExps`.
Also `selftest_render.cpp:389` (`farEye{140,220,140}`) and the stale comment at
`selftest_worldio.cpp:466`.

`page-roundtrip` (`selftest_sim.cpp:2004-2011`) cannot be fixed by a better constant — it
anchors *after* the `streaming` gate has flown the player out to x>600, so its "provably
empty sky" column sits in open procedural terrain. Replace the guess with a **search**: walk
the CPU page table downward from `origin.y + kNChunk − 3` for the first `EMPTY` sentinel.
Fail only if the whole window has no empty chunk — itself a meaningful new assertion.

**B4 — perched tarns, `pondSurface` deleted** (correction 5). Pick the surface from the
**centre** column (`centreBed`, one `landAt`, shared by `pondAt` and `shoreAt`), carve the
bowl below it, and **force the annulus up** to `surf + berm`, ramped so it blends back to
natural ground — exactly what the authored pool at (420,420) already does with
`h = max(h, poolY + 26)`. Every column in `[r, r+bermW]` is then provably above the
waterline at every seed and radius, with zero rim samples. On flat ground the berm is
invisible; on a slope only the downhill side rises, which reads as a dammed tarn. Gate tarns
on `slope < sedSlope` and on being above sea level. Shrink radii to ~24/24 — a 12.7 m
perched tarn on a 200 m mountainside is what the ocean should be doing. New rows:
`pondBerm`, `pondBermWidth`; re-derive the `pondDepth` clamp against `caveShell`.

**B5 — `World::TerrainHeight` ≡ `genColumn.h`.** Not "topmost solid" — literal topmost solid
includes canopy, ruin walls and grass tufts, and cannot be mirrored cheaply (it needs
`treeAt`'s tile scan in a tick path). Every one of the ~30 call sites wants the **ground**:

> `World::TerrainHeight(x,z,seed)` ≡ `genColumn(x,z,seed).h`, exactly, for all inputs.

One new WGSL `colHeightAt` that `genColumn` calls; `World::TerrainHeight` mirrors it. After
B4 the pond mirror is one `landAt`, not 24 samples, so the worst case is ~400 hashes at
O(1)/frame — acceptable; add a comment forbidding per-voxel loops over it. The arena stays
**out** (it is a material override in `genCellIn`; folding it into `Col.h` would double-apply
it and move cave depth and tree bases under its footprint). Drop only the `(408,128)`
streaming-ball keep-out from `pondInfo`; the `−44..264` box and the three authored-pool discs
stay. Write the contract into `DESIGN.md`.

---

### C · The scale pass — the stage that changes the game

**`landAt`**, five octaves, lacunarity 4, persistence 0.25, uniform `A/W = 0.5`, with iq's
derivative attenuation (`att = 1/(1+|g|²)`, `TUNE_FBM_ATTEN` in Q8, clamp ≤ 256 or
`d2·atten` overflows). Wavelengths as log2 exponents:

| Octave | log2 | cell | amp |
|---|---:|---:|---:|
| continental | 11 | 2048 vox / 204.8 m | 1024 / 102.4 m |
| range | 9 | 512 / 51.2 m | 256 / 25.6 m |
| hill | 7 | 128 / 12.8 m | 64 / 6.4 m |
| detail | 5 | 32 / 3.2 m | 16 / 1.6 m |
| grain | 3 | 8 / 0.8 m | 4 / 0.4 m |

Sum 1364 vox = **136 m** of fBm; ranges (package E) add up to 64 m → the ~200 m band.
Attenuation is what makes this self-limiting rather than a sum of five 0.75s: at `|g| = 1`
every subsequent octave is halved, so the field saturates near slope 1.2 on ridges and goes
genuinely flat in valleys. **That is the rule-2 mechanism, not a look knob.**

**The calm home area.** Chebyshev distance (no `isqrt`), smootherstep fade:

```
d   = max(|x|,|z|) - spawnPlainR
w   = clamp((d << 14) / spawnPlainFade, 0, 16384)
ws  = vsmooth(w << 1) >> 1
bed = spawnPlainY + ((raw - spawnPlainY) * ws >> 14)
```

`spawnPlainR ≈ 320`, `spawnPlainFade ≈ 2048` (≈2 km of ramp-up). **The fade width is
load-bearing**: a ramp of magnitude `A` over width `W` adds slope up to `1.875·A/W`, so a
300-voxel fade against ~900 voxels of relief builds a 5.6-slope cliff at exactly x=220 — an
avalanche generator and a page-residency wall constructed by the test fixture. Gate A4
measures this; read the number, do not guess it. Because the ramp lives inside the height
function, `World::TerrainHeight` mirrors it for free and every fixture-anchored gate keeps
working. It is also §8.3's "finite authored home region" implementation point, so a coarse
erosion map can slot in there later.

Datum moves to `spawnPlainY ≈ 200` (B3 is the prerequisite). `TREELINE` becomes
`seaLevel + treelineAbove`. `TREE_MAX_BASE` re-derived. `biomeAt` migrates to `vnoise2`.

**Sediment wedge**, in the same package — it is what makes the relief mean something to the
sim:

```
room = max(0, sedCeil - bed)
sed  = ((room * sedFraction) >> 8) - sedStrip
sed  = sed * max(0, sedSlope - land.slope) / sedSlope      // THE SLOPE GATE
sed  = clamp(sed, 0, sedMax)
h    = bed + sed
```

`Col` gains `sed` and `slope`. Stack: solid skin at `y == h` (grass; sand in desert, below
`seaLevel + beachBand`, or on a fixture pad; snow above the snowline), `M_DIRT` for
`sedTopsoil` voxels, `M_GRAVEL` below, `M_STONE` under the wedge. Underwater columns get sand
over gravel.

Why buried powder is safe here when the old constant-depth dirt shell was not
(`worldgen.wgsl:2074`): that bug was powder under a solid skin on ~50° ground, where the
lateral neighbour is 2+ voxels lower so the grain has an exposed side face. Here `sed > 0`
only where surface slope < repose, `sed` varies smoothly with `bed`, and the gate ramps to
zero continuously. Ship `sedSlope` conservative (200 ≈ 38°) and raise it under `--gate
sleep`; at 0 the feature is off, so `--sweep worldgen.sedSlope=0,200,256` is the
one-invocation proof.

**Measure residency with `--autofly-hard` and frame cost with `--frames 400` against A4's
baselines.** No headless gate can see either.

---

### D · Global sea level + `LAVA_LID` — never ship one without the other

```wgsl
fn caveFill(y : i32) -> i32 {
  if (y <= LAVA_LEVEL)            { return 2; }   // lava:  flat, still
  if (y <= LAVA_LEVEL + LAVA_LID) { return 0; }   // stone plug
  if (y <= SEA_LEVEL)             { return 3; }   // flooded cave: flat, still
  return 1;                                       // open cave
}
```

Without the lid, every cave crossing `y == LAVA_LEVEL` puts water directly on lava
**everywhere in the world on tick 1** — `reactions.json` has water→steam on `tag:hot`
(chance 180) and lava→stone on water (chance 300) both live. That is a world-sized reaction
front whose steam rises and wakes everything above it. Keep the lid at 4+ so one explosion
cannot open a global front either.

Open-air branch: authored pools and perched tarns win first, then `y <= SEA_LEVEL → water`.
A tarn above sea level keeps its surface; below it is subsumed (same material, flat).

**The most dangerous line in the overhaul:** if `SEA_LEVEL > poolY` the ocean floods the
authored *lava* pool — the same catastrophe outside `caveFill`. Move `poolY` to
`spawnPlainY − 8` and make `LoadTuning` **refuse** (not clamp) on `seaLevel ≥ poolY − 4`.

Rest proof, written into the shader as the sea's counterpart to the magma-table note:
flatness from the constant; containment from the terrain (at `y ≤ SEA_LEVEL` a cell is water
iff not solid, so no lateral fullness difference exists); vertical closure; `LIQ_FULL_STATE`;
separation from the lid. Plus a **reaction audit** the lava table never needed: suppress
`M_SEED` scatter below sea level (seed→sprout on a water neighbour fires otherwise);
evaporation needs 4 non-water neighbours and an ocean surface cell has 1, so it never fires;
and put a comment on `waterFreezes` in `tuning.json` — turning it on with a world-spanning
sea surface is a rule-2 landmine (every sky-exposed surface cell rolling 1/1000 per night
tick).

A constant-level sea is a half-space test, so it renders correctly at every cascade level
with no extra work — the only water formulation that does.

---

### E · Mountain ranges — sine-of-contours, domain-warped

`ridgeAt` = highland mask (one `vnoise2`, early-out for ~55% of columns) → domain-warped
contour field → `isin16((c<<2) * ridgeCount)` squared → attenuated by distance from the
reference isoline → scaled by the highland mask. Plus a correlated additive plateau term,
which is what gives "high ridges also mean high valleys" instead of uniform corrugation.

Added **after** the attenuated fBm, never inside the octave loop — feeding a 640-amplitude
term into the accumulated gradient would attenuate the fine octaves to nothing and produce
bald mountains. Warp the ridge field only; warping the fBm ladder destroys the analytic
gradient (chain rule through the warp Jacobian is 4 more terms and a matrix product in fixed
point). Cost ≈ 8 `hash3` average.

**This is the one term that survives a level-8 point sample** — a 51.2 m sample still lands
on a level set of an 819 m field — and therefore the one that makes levels 6–8 do work.

---

### F · 3D caves — `a² + b² < ε`

Two `vnoise3` fields, carve where both are near zero, early-out on the first (~97% of cells
pay one evaluation). Three layers through one fill point: tunnels (`cxl=cyl=7`), ravines
(`cyl=9`, 4× Y stretch), chambers (low-frequency hard threshold).

**The province mask is page-pool protection, not decoration, and the doc does not name this
risk.** `kPoolPages = 32768` and exhaustion is a *fatal abort*. A mountain interior is free
only because it is `JITTER(stone)` — and **a single carved cell anywhere in a 16³ chunk
destroys that sentinel and costs a real page.** Unbanded 3D caves under 200 m of terrain
would make most underground chunks non-uniform and §3.3's "deep terrain is nearly free"
argument evaporates with it. A 2D low-frequency province mask (~35% of columns), plus a shell
(never breach the sediment from below) and a floor, is what keeps buried chunks uniform.
**`--autofly-hard` peak resident slots is the acceptance number, not the look.**

Replacing `caveAt`'s no-floaters guarantee: there is no per-cell substitute, and the honest
answer is a chain of bounds. (i) **A generated floater is inert and costs zero** —
`flagSupportLoss` fires only when a supporting cell *leaves*, nothing in a fresh cave moves,
so island detection is never queued and `sleep` passes unchanged. (ii) The detector is
event-scoped to a 64³ box around a blast, and a rock detaching from a roof you just blew up
is correct. (iii) **Never generate powder inside a cave** — `caveFill` returns
air/water/stone/lava and nothing else; write that into the function's comment, it is the rule
someone will break later. (iv) Keep each layer's carved fraction ≈1% so the complement stays
connected almost surely. (v) Later: KdotJPG's derivative dead-end closing, which is also the
exploration payoff.

---

### G · Tier A/B/C climate

**Tier A** — `climateAt(x,z)` takes **no seed**; `hash3` is called with a literal
`CLIMATE_SEED` constant. Saturating compass ramps (cold with +Z, aridity with +X, extreme
reached at `climateSpan` and held — a snowy north you can walk to and *stay* in, not a band
you pass through) plus one fixed-seed raggedness noise at ~1.6 km cells. `CLIMATE_SEED` is
fixed forever; changing it re-lays every region in every saved world.

**Tier B/C** — seeded. Crucially, **biomes do not own height functions.** There is one height
function whose octave amplitudes are modulated by a smooth seeded uplift field. A smooth
modulation of a continuous function is continuous, so the doc's §6.6 "spikes at biome
boundaries" trap is *structurally unreachable* — there are no per-biome heightmaps to
interpolate between. The **biome label** reads the broken-up copy of the same field; nothing
geometric reads the label. This is what MC 1.18 converged on and it is cheaper than N height
functions plus a normalised sparse convolution.

`biomeAt` becomes a pure function of `(cold, arid, pick, h, slope)` with no noise of its own,
with a **latitude-dependent snowline**. `TUNE_BIOME_SCALE = 384` retires — it is below the
51.2 m level-8 cell and aliases to white noise at the horizon; `climateLog2 = 14` (1.6 km)
and `upliftLog2 = 12` (410 m) both clear it. Scattered-Biome-Blender is deliberately **not**
adopted: amplitude modulation already gives smooth height blending for free, and its
per-column reciprocal divide is the one place `f32` would sneak in.

The far sampler shares the code path by construction once B2 deleted `surfHeightAt`.

---

### H · Land it

`DESIGN.md` (the height contract, the sea's rest argument, the new octave ladder),
`ARCH_NODES` + Development Status in `assets/tuner.html`, a revision-3 note on
`docs/RESEARCH_worldgen.md` recording the six corrections, final `--suite acceptance`, and
the after-side of A4's two measurements.

---

## 4. Tuning plumbing

~45 new `worldgen` rows, each needing **5 edits**: `src/sim/tuning_params.def` (no trailing
comments — the parser chokes) → decl in `src/sim/tuning.h` → clamped `Read*` in `LoadTuning`
(`src/sim/tuning.cpp:1614-1785`) → default in `assets/materials/tuning.json` → row in
`assets/tuner_schema.js` (Worldgen tab, `:869-943`). Then
`python scripts/gen_tuning_prelude.py`.

All integer — the `worldgen` group is 27 `TP_I` + 33 `TP_U` with zero floats, and the
`sim.fluid*`/`sim.wind*` float escape hatch is a different group and is not available here.

Retire `hillAmplitude` / `hillWavelength` / `detailAmplitude` / `detailWavelength` /
`biomeScale` **by deleting the rows**, not by leaving them dead, or `check_invariants.py`'s
`TUNE_*` check goes quiet on the wrong side.

Clamps that matter: `octNLog2 ∈ [3,23]`, `fbmAtten ≤ 256`,
`seaLevel > lavaLevel + lavaLid + 1`, and the **refusal** on `seaLevel ≥ poolY − 4`.

---

## 5. Verification

Hash movement is expected and unremarkable. What is being verified is **determinism** and
**rule 2** (nothing generated is in motion).

**While iterating** — `bash scripts/check_shaders.sh` (free, no rebuild: `LoadShader` reads
WGSL from disk and the SPIR-V cache keys on content hash), `python
scripts/check_invariants.py` (fires from the PostToolUse hook), and
`--selftest --gate terrain` (~10 s). That is the whole loop. Gate pass C1 is the
CPU/GPU-agreement proof; pass D is the early rule-2 warning.

**Per work package** — `--gate terrain`, then `--gate sleep` once (~60 s, only after pass D
is green — that is the proxy's entire purpose), then `--selftest --rebaseline` and, when
render output moved, `--vk-smoke --rebaseline` + `--vk-smoke-loud --rebaseline`.
`--rebaseline` refuses on validation messages or page faults, so it cannot paper over a
regression.

**New knobs** — `--sweep worldgen.<knob>=a,b,c`. Different values must give different hashes;
identical hashes mean the knob does not reach the kernel. No files touched.

**After packages C and F specifically** — `--frames 1200 --autofly-hard` and `--frames 400`
against A4's recorded baselines. Those two packages are where the residency argument is
spent, and no headless gate can see a fill-path or render-occupancy cliff.

**Once, at the end, on the tree that ships** — `--suite acceptance`. Known: `page-roundtrip`
may fail in-suite from ordering; re-confirm standalone. Every run auto-dumps
`build/last_run.json` — never re-run a binary just to read data.

**One optional instrument change:** `--autofly-hard` holds `down` permanently, which was the
right worst case at 54 voxels of relief. With 200 m above the datum the new adversarial case
is *climbing* a face, where surface area and therefore real pages are maximal. Add an
`--autofly-climb` sibling rather than silently redefining `--autofly-hard` — its numbers are
quoted in `CLAUDE.md` and `docs/PLAN_page_table.md`.

---

## 6. Critical files

- `assets/shaders/worldgen.wgsl` — `vnoise` :207, `baseHeight` :257, `biomeAt` :279,
  `pondSurface` :404, `shoreAt` :479, `isin` :742, `treeAt` :1245, `caveFill`/`caveAt`
  :1840/:1845, `Col`/`genColumn` :1882/:1894, `genCellIn` :2031, `surfHeightAt` :2749,
  `farSurfaceMat` :2823, `genChunk` :2867, `far` :3093
- `src/sim/world.cpp` — the CPU mirror block :515-566
- `src/sim/tuning_params.def` :508-645, `src/sim/tuning.cpp` :1614-1785, `src/sim/tuning.h`,
  `assets/materials/tuning.json`, `assets/tuner_schema.js` :869-943
- `src/test/selftest.cpp` (`kOrder` :51, `LoadBaseline` :160, `RebaselineSelftest` :230),
  `src/test/selftest_sim.cpp` (:2208 registration, :2004 `page-roundtrip`),
  `src/test/support.cpp` :836-876, `src/gpu/vk_smoke.cpp` :197-219
- `src/main.cpp` — `--frames` report :4631-4688, `--autofly-hard` :2654-2667
- `scripts/check_invariants.py` — has nothing about worldgen today
