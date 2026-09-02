# PLAN: Lin follow-ups — perf, secondary rays, worldgen look, indirect light

**Source:** `docs/RESEARCH_john_lin.md` §13.1–13.4 (and §14, the plain-words contract).
**Written:** 2026-09-01, after re-verifying every §13 item against the tree at `73bc9c6`.
**Status:** plan of record. One orchestrator, Opus agents per package, one worktree per
package. Each package below states its owner scope, its hash impact, and the ONE
verification that closes it.

This document exists because the research doc's §13 was written from a read of the code,
and a second read found nine places where the code disagrees with the plan. They are
listed first, because they change what gets built.

---

## 0. Corrections to §13 that change the work

| §13 says | The tree says | Consequence |
|---|---|---|
| 13.1.1 write a new `traceOpaque()` | `shadowMarch` in `shadow_resolve.wgsl:94` already IS a media-blind opaque DDA returning `{hit, t}`, sentinel-aware, chunk-skipping. It just lives in a compute shader and returns too little. | Promote `shadowMarch` into `common.wgsl` as the slim tracer, widen its return to `{hit,t,cell,axis,sgn,word}`, and make `shadow_resolve`, `traceReflection`, `traceRefraction`, `sunShadowAt` all call it. One DDA, not three. The `shadow-cache` gate ("two DDAs agree") becomes trivially true. |
| 13.1.1 secondary rays need "media, micro or water" dropped | Verified: `traceReflection`/`traceRefraction`/`shadeSecondaryHit` read **7 of 26** `Hit` fields (`hit,t,word,axis,sgn,cell`). `wantMedia=false` already zeroes the micro budget, so reflections show no grass today; a secondary ray already marches *through* liquid to the bed. | Nothing visible is lost by the slim tracer. |
| 13.1.1 the shadow half "is being fixed by shadow_resolve" | `fs` still contains `sunShadowAt` (`raymarch.wgsl:6707`) as the cache-off fallback, and `sunShadowAt` calls `trace()`. Whether Tint const-folds it away when `shadowCache=1` is **unknown** — if it does not, the 3.7 ms `shadow0` footprint is still being paid with the cache on. | 13.1.1 must audit this and end with **exactly one `trace()` call in `fs`'s call graph** (the primary ray). This is potentially the largest single win in the plan and it is currently unmeasured. |
| 13.1.4 build a per-column highest-blocker map | No column-height structure exists (`world.cpp:974`, `waterbody.h:69` record the last copy being deleted on purpose). A column map is also wrong for any sun that is not overhead, and `godRays` runs down to 5° elevation (`keyLightDir().y < 0.08` gate). | Do not build it. `godRays` casts `trace(p, kd, 20, false)` per step (`raymarch.wgsl:3556`) — the cost is the `trace()` footprint plus a 40-read `waterAbove` walk, not the marching. Replace with the slim tracer in **coarse-terminate mode from t=0** over the 4³ blocker mask (below). Correct at every sun angle, no new buffer. Measure first (13.1.5). |
| 13.2.1 "a set 4³ blocker bit" | The 4³ mask exists (`kSubOccShift=2`, two classes, tail of `occupancy`, `common.wgsl:2838-2889`), is maintained by `sim_occupancy` on every dirty walk, and is consumed by **nobody** (`SUBOCC_SKIP=false`). `shadowMarch` never reads it. | Free lever, exactly as the doc hoped. The blockers class (`[1]`) is what shadow and god rays test. |
| 13.2.2 "a bit beside the material byte" | A far cell is **one byte = a raw material id**, no parallel array, no spare bit declared. Material count is 115 (< 128) so bit 7 is *unused in practice* but unasserted; both writers use atomic byte RMW; readers `farMatAt`, `farShadowed`, `traceFar`. | Bit 7 can be claimed only with a load-time assert `materialCount <= 127` and a mask at every read. And "any blocker" over a 16 m cell makes every canopy-touching cell a solid cube on the horizon — a real regression risk for the HIT test. Scope: shadows at all levels, hit test at levels ≤ 2 behind a knob. Experiment with a kill criterion. |
| 13.3.2 "apply the pond ordering to `worldgen.wgsl:3099-3115`" | Ruins are stamped in the **cell half** (`genCellIn:3095-3155`) at `baseHeight(centre)` — raw `landAt().h`, ignoring pond carve and pool floors — with no pad, no slope gate. Ponds are decided in the **column half** (`landColumn:2248`) and rewrite `h` before any cell fills. | Move the ruin site decision into `landColumn`. **And into `world.cpp`**: `landColumn` is token-mirrored on the CPU (`MIRROR-BEGIN height/landheight`, checked by `check_invariants.py`, compared per voxel by `--gate terrain`). Every worldgen height change in this plan is a two-file change. |
| 13.3.3 "a polyline per biome in `tuning.json`" | The tuning pipeline has **no array transport** — `TP_F/I/U/V3` scalars only (`tuning.cpp:2805`). No curve widget exists in the tuner. Biome is a thresholded noise (`biomeAt:613`, 4 biomes) with no continuous weight. Height is 5 unrolled integer octaves with iq gradient attenuation (`landAt:497-591`); `f32` is forbidden in worldgen (CPU mirror). | Ship the curve as **8 uniformly-spaced knot rows per biome** (`worldgen.curve<Biome>0..7`, `TP_I`, Q14) — zero new mechanism, select-chain lookup by construction. Blend across biome edges with a soft weight derived from the biome band value. Scale the accumulated gradient by the segment slope so attenuation stays coherent. Interpolate with an integer monotone cubic so an authored plateau is flat and an authored ramp has no creases. The drag widget is a follow-up, not a prerequisite. |
| 13.3.4 "grass only under open sky, moss on shaded faces, crystals in caves" | Canopy cover already drives undergrowth (`undergrowthSite`, 5×5 tile scan with authored `shade`). Cave floor/ceiling is **closed-form** (`caveBands:2053`, `f1/c1/f2/c2`), no walk needed. There are no overhangs outside caves/trees/ruins. **No crystal material exists.** Worldgen has no sun direction. | Scope shrinks to what is actually missing: cave flora keyed on the cave bands (mushrooms on shallow-band floors, a new emissive crystal material on deep-band walls/ceilings), ruin footprint suppression of grass/trees (free once 13.3.2 puts the site in the column half), and moss on a fixed "north" (−Z) face of trunks and ruin walls as a convention. No marching. |
| 13.3.5 "set the palette jitter amplitude to zero" | The per-voxel jitter is `paletteJitter`'s `switch (j % 3)` over **three authored palette colours** — there is no amplitude and no knob. The knob that exists is `render.grainAmp`/`grainAmpFar` (value-noise albedo modulation, `tuning_params.def:167`). | Two look tests, not one: `grainAmp=grainAmpFar=0` (tuning only, no code) and a one-line WGSL toggle forcing `color0` (WGSL only, no rebuild). Four `--shot`s, one paragraph in `PLAN_gi.md`. |
| 13.4 P0 "written by the shadow resolve pass" | The resolve pass is **locality-bound** (22 ns/ray vs 1.7 ns inline; Morton sort is the owed fix). The slot word has **zero** spare bits. Sky visibility is sun-independent and screen-independent; the resolve pass is neither. | P0 gets its **own** small pass over the dirty list plus a rolling refresh, writing a dense per-(slot, 4³ block, face) byte buffer (12.6 MiB). Marches the 4³ blocker mask, not voxels. P1 (sun-dependent injection) is where the resolve pass becomes the right writer. |

Everything else in §13.1–13.4 verified as written.

---

## 1. Ground rules for every package

- **One worktree per package**, branched from `main`. Build with `bash scripts/build.sh`
  in the worktree; run **every** binary through `bash scripts/run.sh` (absolute path to the
  main checkout's script if the worktree predates it). Claim on the board first.
- **Measurement hygiene.** Two `sandvox_tuner.exe` instances are running on this machine
  and a tuner respawns `sandvox.exe` **outside the run mutex** — `PLAN_surface_flight_perf.md`
  Correction 2 lost a session to it. Before any `--render-budget`: `tasklist | grep -i sandvox`
  and refuse to measure while a stray `sandvox.exe` exists. Measure twice; if p50 moves >5%
  between the two, the environment is dirty, not the code.
- **Budget rule.** One `--render-budget` per hypothesis. `--gate X` while iterating, the
  full suite once at the end. A hash you moved on purpose costs one `--rebaseline`.
- **Hash discipline.** 13.1.x, 13.2.x, 13.3.1, 13.4 are render-only and must leave
  `determinismHash` **unmoved** — that is their cheapest correctness proof, check it.
  13.3.2–13.3.4 move it once each; rebaseline at the commit, never before.
- **Land order matters for `raymarch.wgsl`.** It is 7,088 lines and every renderer package
  touches it. Packages are sequenced below so no two agents hold it at once. The main
  checkout currently has **uncommitted** shadow-cache-under-motion work in `common.wgsl`,
  `raymarch.wgsl`, `shadow_resolve.wgsl`, `selftest_render.cpp`, `vk_smoke.cpp` (board
  claim by agent-a0ca74, 2026-09-01 22:34Z). That work was committed as `13adeb1`/`046bf77` at 23:10Z, so Wave 2 is unblocked
  and branches from `046bf77`.
- **Done means §14's line is true.** A package is not done because a number moved; it is
  done when the plain-words line for it holds and the verification below says so.

---

## 2. Wave 1 — instruments and worldgen (independent files, run in parallel)

### W1-A · `--shader-stats` (13.1.2) — C++ only, no hash

**Why first:** 13.1.3 is gated on it, 13.1.1 and 13.1.4 are judged by it, and it is the
instrument that would have named the 10.8x by-value-uniform bug in one run.

**Verified facts to build on:**
- Device creation: `Backend::CreateLogicalDevice` (`src/gpu/rhi_vulkan.cpp:488`), one device
  extension today (swapchain, `:546-563` is the enumerate→check→push template), pNext =
  `VkPhysicalDeviceVulkan13Features` only. Caps in `vk::Caps` filled by `QueryCaps` (`:440`).
- Pipelines: `CreateComputePipeline` (`:1127`), `CreateGraphicsPipeline` (`:1228`); both
  **discard their `label`** (`const char* /*label*/`) and push bare handles into `pipelines_`.
  Callers pass real names (`simulation.cpp:770-854`, `EnsureRenderPipelines:1522+`).
- Graphics pipelines are created **lazily on first draw**; `BuildPipelines` (`simulation.cpp:737`)
  is the single compute hook. Pipeline cache `sandvox_pipeline_cache.bin` is passed to both creates.
- Sanctioned back-door to the backend: `rhi::vkr::NativeBackend` (`rhi_vk.h:37`), precedent
  `rhi::vkr::LastStats` (`:47`). Device entry points via `VKL_D` (`vk_loader.cpp:100-160`).
- Flags: `main.cpp:2215-2335` parse chain; headless suppression list `:2668-2670` (add the flag);
  timestamp enable `:2681`.

**Build:**
1. Query and, if present, enable `VK_KHR_pipeline_executable_properties` +
   `VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR` in the pNext chain. Optional:
   record `caps_.pipelineExecutableProps`, print it in `--vk-info`.
2. `Backend` keeps `{VkPipeline, std::string label, bool compute}`; stop discarding labels.
3. A backend-wide `captureStats` bool. When set: `VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR`
   on both creates and **bypass the on-disk pipeline cache** for that run.
4. `--shader-stats`: headless, constructs ctx/world/sim like `--render-budget`, forces
   `EnsureRenderPipelines` (render one offscreen frame if that is the only way), then for
   every pipeline calls `vkGetPipelineExecutablePropertiesKHR` /
   `vkGetPipelineExecutableStatisticsKHR` and prints **all** statistics generically
   (name/value as the driver reports them — do not hardcode NVIDIA's names). One line per
   executable, sorted by any statistic whose name contains "local"/"spill"/"scratch" descending.
   Also write `build/shader_stats.json` (`{pipeline, stage, stats{}}`).
5. Add the command to CLAUDE.md's verify list.

**Verify:** run it once on the unmodified tree; the `raymarch` line exists and reports a
register count. Then read it: if `fs` shows non-zero local memory, that number is the
baseline W2 works against. No gate — it is an instrument.

### W1-B · Dusk and submerged cameras in `--render-budget` (13.1.5) + zero-jitter look (13.3.5) — C++ + shots, no hash

**Verified facts:** one camera (`perfsuite.cpp:1786-1795`, overlook at (256, h+120, 256)),
`FindNoonTick` (`:1393`) scans sun elevation; 16 arms in `kRenderArms[]` (`:1415-1512`), all
but `noshadow`/`halfres` are tuning mutations → shader recompile per arm. Underwater is
**derived in the shader** (`raymarch.wgsl:6917`, `h.liqT < 0.05`) — placing the eye inside
the liquid is the whole change. `SetupWater` (`:777-897`) and `World::AuthoredPoolList`
already find the authored lake (floor y=44, surface y=68 near (420,·,420) per `main.cpp:934`);
the pool must be inside the residency window or it shades through the cascade. The harness
writes **no JSON** today.

**Build:**
1. `FindTickAtElevation(tun, sinElev)` next to `FindNoonTick`. Cameras:
   `noon` (unchanged, default, so historical numbers stay comparable), `dusk` (same eye,
   sun at ~8° elevation), `submerged` (authored lake via `AuthoredPoolList`, eye midway
   between floor and surface, looking horizontally toward the shore, noon sun so `godRays` runs).
2. Extra cameras run a reduced arm set: `baseline, noshadow, nofar, noreflect, halfres`, and
   for `submerged` two new arms `nogodray` (`render.godraySteps=0`) and `godshadow0`
   (`render.godrayShadowSteps=0`). Those two arms are the measurement 13.1.4 is judged by.
3. `--budget-cams noon,dusk,submerged` (default all three). One BMP per camera
   (`build/render_budget_<cam>.bmp`), and **`build/render_budget.json`** with every
   camera × arm p50/p95 plus the shadow-cache attribution, so nobody re-runs to read a number.
4. Look test (13.3.5): four `--shot`s of the same view — stock; `grainAmp=grainAmpFar=0`
   (tuning override, no code); palette forced to `color0` (one-line WGSL toggle in
   `paletteJitter`, reverted after); both. Save to `build/look_*.bmp`. Write one paragraph
   of findings into `docs/PLAN_gi.md` §0. This is a look, not a measurement.

**Verify:** the harness prints three camera tables; `submerged` `nogodray` gap is a number
(first time this path has ever been measured). Determinism hash unmoved.

### W1-C · Worldgen package: ruin pads → openness placement → biome curves (13.3.2, 13.3.4, 13.3.3) — moves the hash three times, serial, one agent

Serial on one agent because all three touch `landColumn`/`genCellIn` and the `world.cpp`
mirror; parallel agents here would spend their time in merge conflicts.

**Verified facts:** see §0 rows for 13.3.2–13.3.4. Plus: worldgen is integer-only because
`world.cpp` re-implements `landAt`/`landColumn` and `--gate terrain` compares CPU vs GPU per
voxel over 9,409 columns; `genCell` is pure; `--heightmap` renders the CPU mirror, so a
shader-only height change shows nothing there and fails `check_invariants.py`. The
`worldgen.wgsl` board claim by agent-8db0b7 (2026-08-29) is stale: the file has been
committed three times since and its owner posted no `done`.

**Step 1 — ruin pads (13.3.2).** Move the ruin tile decision into `landColumn` (both files):
site found → compute a pad height (median of the four corner column heights of the footprint,
or the centre's `landColumn.h` after ponds — not `baseHeight`), refuse the site if the corner
spread exceeds `worldgen.ruinMaxSlope`, else set `h = padH` inside the footprint and blend
linearly to terrain over `worldgen.ruinPadMargin` cells outside it. Sediment inside the pad
→ 0. The cell half keeps stamping the shell at the (now consistent) pad height. Knobs:
`ruinPadMargin`, `ruinMaxSlope`, `ruinPadBlend` (all `TP_I`, all through the 4-place
tuning pipeline). **Verify:** `python scripts/check_invariants.py`, `--gate terrain` green,
`check_worldview.sh` with a hillside ruin in view, then `--selftest --rebaseline`, commit.

**Step 2 — openness placement (13.3.4).** (a) Ruin footprint (now known in the column half)
suppresses trees and tall grass, allows moss/litter. (b) Cave flora from `caveBands`:
mushrooms on shallow-band floors (`y == f1+1`) with a density knob; a new `crystal` material
(emissive, `materials.json`, palette authored) in clusters on deep-band walls and ceilings,
seeded by the existing site-hash pattern, never in lava reach. (c) Moss on the −Z face of
trunks and ruin walls by convention (`worldgen.mossFace` knob for the face). No column
marching anywhere; every test is a closed form the column already has. **Verify:** a
`--voxdump` of a cave box counts `> 0` crystals and mushrooms and `0` above ground;
`--gate terrain`; rebaseline; commit.

**Step 3 — biome height curves (13.3.3).** Inside `landAt` after `o0+o1` (the continental
and range rungs) and before hill/detail/grain: `sum' = curve_b(sum)`, where `curve_b` is 8
knots per biome on a uniform input grid spanning `[-contAmp-rangeAmp, +contAmp+rangeAmp]`,
values Q14 in the same range, interpolated by an integer monotone cubic (PCHIP; Catmull-Rom
is acceptable if PCHIP in fixed point proves fiddly, provided the identity curve reproduces
the input **bit-exactly** so the default knots move nothing but the rebaseline). The
accumulated gradient `g` is multiplied by the segment slope (Q8) so iq attenuation of the
finer rungs stays coherent. Biome weight: compute the biome band value inside `landAt`
(pure in x,z) and blend the two nearest biomes' curve outputs over a ±`worldgen.biomeBlend`
band around each threshold, so no cliff appears at a biome edge. Defaults = identity for
all four biomes (so the hash move at this commit is zero — prove it: hash unmoved). Rows:
`worldgen.curveForest0..7`, `curvePine0..7`, `curveMeadow0..7`, `curveDesert0..7` in
`tuning_params.def`, `tuning.h`, `LoadTuning`, `tuning.json`, `tuner_schema.js` (one group
per biome). Mirror in `world.cpp`. Then author one non-identity default (meadow flatter,
pine steeper) and rebaseline. **Verify:** `--sweep worldgen.curvePine4=8000,12000` proves
reach (two hashes); `--heightmap` shows the change; `--gate terrain`; rebaseline; commit.
The Worldgen-tab drag widget is **W4**, not here.

Update `ARCH_NODES` and the Development Status panel in `assets/tuner.html` for all three.

### W1-D · `docs/PLAN_gi.md` — docs only

Written by the orchestrator from the verified facts (see §5 below), before W3 starts.

---

## 3. Wave 2 — the renderer, in sequence (unblocked 2026-09-01 23:10Z; W2-A launched)

### W2-A · One slim tracer for every secondary ray (13.1.1) — WGSL only, no hash

1. Move `shadowMarch` from `shadow_resolve.wgsl` to `common.wgsl` as
   `traceOpaque(ro, rd, maxSteps, coarseFromT) -> OpaqueHit{hit, t, cell, axis, sgn, word}`
   (`coarseFromT` is accepted now, ignored until W2-B; pass a huge value). Keep its chunk-skip
   on `occBlockers`, its sentinel fast paths, its `isRayBlocker` hit test.
2. `shadow_resolve.wgsl` calls it. `traceReflection`, `traceRefraction`, `shadeSecondaryHit`
   take `OpaqueHit`. `sunShadowAt` uses it.
3. **Audit `fs`'s call graph until exactly one `trace()` call remains** (the primary). Confirm
   with `--shader-stats` (register count / local memory before vs after) — this also answers
   whether the cache-off fallback was still being compiled in.
4. Reflections that exit the window keep returning `reflectionSky`; reflections still march
   through liquid to the bed; no far cascade — all unchanged behaviour, state it in the commit.

**Verify:** `--render-budget` (noon): the `noreflect − reflgate` gap closes to < 0.5 ms; the
`shadow0 − noshadow` gap likewise; `--gate shadow-cache` green; `determinismHash` unmoved;
`--shot` of the lake view before/after for a visual sanity diff. One run each.

### W2-B · Coarse-terminate secondary rays, god rays, axis sites (13.2.1, 13.1.4, 13.1.3) — WGSL only, no hash

Depends on W2-A (the tracer) and W1-A/W1-B (the numbers).

1. **Coarse terminate (13.2.1).** In `traceOpaque`, once `t > coarseFromT` and the current
   chunk has blockers, test the 4³ **blockers-class** bit for the current cell's block
   (`common.wgsl:2838-2889` accessors); set → hit at the block entry. Knob
   `render.shadowCoarseDist` (metres, default from the experiment; 0 disables). The resolve
   pass passes it; the near-receiver region stays bit-identical so the gate holds.
2. **God rays (13.1.4).** `godRays` per-step shadow becomes `traceOpaque(p, kd, steps, 0.0)`
   — coarse from the first step; a volumetric sample wants the pre-integrated answer anyway,
   which is also why it will alias less. Reduce `TUNE_GODRAY_SHADOW_STEPS` in *blocks* accordingly.
   Leave `waterAbove` alone unless the submerged arm says it is the remaining cost.
3. **Axis sites (13.1.3).** Only where W1-A shows local memory in the shader that owns them:
   rewrite the runtime-index sites (`rd[axis]`, `n[axis]`, `d1[a1]=s1` in `voxelAO`,
   `f[t.x]`/`c[t.x]` ×4 in `shadowCached`, `hp[a1]`, `nn[a1]`, `nLocal[axis]` in
   `microbody.wgsl`) as `select` chains / mask dots. Skip the `tMax[a]` sites inside unrolled
   constant-trip loops — their indices are literals after unrolling.

**Verify:** `--render-budget` noon+dusk shadow arms and `submerged` `nogodray` gap (must
shrink by the factor the arm predicted — state the number); `--gate shadow-cache`;
`--shader-stats` local memory for the touched pipelines goes to zero or the site is reverted;
`determinismHash` unmoved; a `--shot` under a tree canopy for the "heavier, calmer" claim.

### W2-C · Waterfall mist (13.3.1) — WGSL + a shot fixture, no hash

Runs in parallel with W2-B (different functions: `shadeWater`/`shadeSubmerged` region vs
tracer/AO). **Verified:** "liquid with air below" is not in the word — one `voxWordAt` of
the cell below on liquid hits only; `siltMotes` (`raymarch.wgsl:3583`) is the pattern: fixed
slabs, `valueNoise`, time drift, hard threshold, falloff, `TUNE_*_DENSITY <= 0` early-out
that Tint folds away. **There is no waterfall in the shipped world** (authored pools are
flat basins) — the fixture is a `--shot waterfall` variant that pours water off a ledge
through the MutationQueue before capturing.

Build: on a liquid hit whose cell below is air (and, cheaper still, whose cell below that is
also air or liquid), overlay mist slabs along the view ray around the column; on an opaque
hit whose cell above is liquid-over-air, spray. ≤3 extra voxel reads, only on those hits.
Knobs `render.mistDensity/mistBrightness/mistDrift`. **Verify:** the shot; `determinismHash`
unmoved; `--render-budget` noon baseline unchanged within noise (the dry frame pays nothing).

### W2-D · Conservative far "any blocker" bit (13.2.2) — experiment, WGSL only, no hash

After W1-C lands (shares `worldgen.wgsl`). Claim bit 7 of the far byte; `LoadMaterials`
asserts `count <= 127`; mask `& 0x7F` in `farMatAt` and both writers' RMW. Pristine fill:
level 1 from the column's `[bed,h]` intersection plus closed-form tree/ruin bounds at the
cell's four corners; levels ≥2 as the OR of the eight children (fill low levels first, or
recompute the same predicate with wider bounds — either way `far` and `fardown` must agree
byte-for-byte at the seam, which the existing far gates check). Readers: `farShadowed` uses
the bit at all levels; `traceFar` uses it only at `level <= render.farBlockerHitLevel`
(default 2). **Kill criterion:** a `--shot` pair of the horizon; if the tree line or ridge
fattens visibly, keep the shadow half and set the hit-level knob to 0. **Verify:** `far-fog`
gate, the shot pair, `determinismHash` unmoved.

---

## 4. Wave 3 — indirect light P0, then P1 (13.4)

Governed by `docs/PLAN_gi.md`. Summary of what W3 builds, so this plan is self-contained:

**P0 — openness (sky visibility) grid.** New buffer `openness`: one byte per
(chunk slot, 4³ block, face) = `kNumChunks × 64 × 6` = 12.6 MiB, dense, window-slot keyed,
with a per-slot generation stamp so a chunk that streams into a slot invalidates the old
entries. Written by a new compute pass `sim_openness.wgsl` (pass_table rows, compacted
dirty list via indirect dispatch + a rolling refresh of `render.opennessChunksPerFrame`
non-sentinel chunks, oldest first). One thread per (block, face) whose blockers bit is set
and whose face-neighbour block is not full: march ~5 fixed directions in the face's
hemisphere through the **4³ blocker mask** (block steps, not voxel steps) to
`render.opennessReach` metres; openness = unblocked fraction, quantised to a byte. Readers:
`ambientAt` becomes `mix(ground, sky, openness)`-shaped at near-field hits (grid read at the
hit's block-face, nearest first, bilinear only if it tiles); far hits keep the `n.y` lerp;
**`ambientAtP` in the raster shaders (mobs, microbodies, debris) must sample the same grid**
or bodies glow in caves. Render-only, hash unmoved. Cost bounded by the per-frame chunk
budget; sentinel chunks need no entries.

**Verify P0:** a new `--gate openness` arm inside `selftest_render.cpp`: after settling,
read back the grid for a known cave block and a known open-field block and assert
`cave < 0.2 < 0.8 < field` (ranges, not equality — the rolling refresh order is not
deterministic); `--render-budget` noon baseline delta printed; `--shot` cave mouth + ruin
interior judged by eye. Hash unmoved.

**P1 — direct injection + one-bounce gather.** Only after P0 is judged good by eye. Third
buffer `irradiance` (RGB9E5 per block-face, 50 MiB dense, or sparse over blocks with a set
blockers bit). The **resolve pass** deposits `albedo × sun × lit` into the patch's block-face
(it already knows cell, face, and lit); `fs` gathers 3–4 blocks along the normal hemisphere
through the block grid. Sun-aware rolling decay. P2 (write-back) and P3 (emissives from the
`sim_occupancy` dirty walk; delete `heatSpill`) are specified in `PLAN_gi.md` and are not
scheduled here.

---

## 5. Not in this plan, and the trigger that would put it in

| Item | Why not now | Trigger |
|---|---|---|
| 13.2.3 half-res primary + depth-aware upsample | Needs the G-buffer split `DESIGN.md:3358` describes and `fs` (`raymarch.wgsl:6399`, ~690 inline lines) does not have; plus a history buffer. A week, an architecture change. | Either P1 goes stochastic (then the history buffer is owed anyway) or the noon baseline is still > 12 ms after W2. |
| 13.2.4 shared-memory tiled CA | Weeks plus a determinism proof. | A `--perf` scenario where the CA row, not render, is the frame. None exists today. |
| 13.2.5 multi-region uploads | Inline threshold covers every per-tick write. | `kFetchPerTick` or page size crosses 64 KiB. |
| 13.6.3 DESIGN §9 pipeline bullet | One sentence is wrong. | W2-A's commit fixes the sentence (needs the `DESIGN.md` claim); do not build the split to make the doc true. |
| Morton sort of the shadow request list | Owed by the shadow-cache work, not by this plan. | Before P1 makes the resolve pass do more per request. |
| Worldgen-tab curve drag widget | Knot rows are editable as numbers in the tuner today. | After W1-C step 3 lands and someone has moved a knot by hand three times. |

---

## 6. Schedule and hand-offs

```
W1-A shader-stats ─┐
W1-B cameras+look ─┼─ parallel, independent files ─► merge to main ─► W2-A ─► W2-B ─┐
W1-C worldgen ×3  ─┘                                              └► W2-C ─┤
W1-D PLAN_gi.md (orchestrator)                                     └► W2-D ─┴─► W3 P0 ─► (judge) ─► P1
```

Each package: claim → worktree → build → implement → its ONE verification → commit on
branch → report (numbers, hash, gate lines, files) → orchestrator reviews the diff and
merges. Full `--suite acceptance` **once per wave**, on the merged tree, not per package.
