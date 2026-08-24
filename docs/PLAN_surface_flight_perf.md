# PLAN: surface-flight / altitude framerate collapse

**Symptom (2026-08-23):** underground/interiors ~90 fps after the paged-streaming work
(d3dcb76, 183df28). Horizontal flight over trees/grass: ~5 fps. Climbing so more world is
visible: ~0.2 fps — cost scales with how much world the camera can *see*.

> **CORRECTION (2026-08-23, measured on `--autofly-surface`, Phase 0 of this plan).**
> The diagnosis below is **wrong about which term dominates**, and the harness this plan
> asked for is what showed it. Measured p50 **488 ms** with `avg render+present` **1.5 ms**
> — the render timer starts at `main.cpp:2857`, so ~486 ms of every frame was
> **pre-render**. `--residency dense` was **25x faster** (high-cruise 1317 ms paged vs
> 53.5 ms dense) and had almost no altitude curve where paged had 4.1x.
>
> The cost was **Part B**, and specifically a site this plan does not list: the
> free-confirmation probe in `PageTable::ConsumeOccupancy` was one
> `rhi::ReadbackBlocking` (a full device drain) **per candidate**, at 665-804 candidates
> per tick under surface flight = 198 ms in a single tick. Those candidates are
> manufactured by the B3 stale-snapshot loop, not by real emptiness: the refusal words are
> material ids (`0x001`/`0x022`/`0x04d`), i.e. resident stone that a stale occupancy
> snapshot reported empty for 8 consecutive ticks.
>
> Batching that probe: low-skim 324.5 -> 68.2 ms, high-cruise 1317.4 -> **61.5 ms
> (21.4x)**, and **the altitude curve disappeared** (high cruise now marginally faster
> than low skim, which is what geometry predicts). Part A is real but is the NEXT
> bottleneck, worth ~46 ms: after the fix, dense's 51 ms p50 is ~46 ms of
> `render+present`, where paged's 62 ms is ~1.4 ms of it.
>
> Lesson for the next reader: get the whole-frame-vs-render split before believing a
> renderer diagnosis. `--frames` prints both.

> **CORRECTION 2 (2026-08-23, Phase 1 / Part A implemented and measured).**
> Part A is real but **small**, and the "~46 ms of render+present" above was a
> measurement artifact. Both are worth stating plainly because the first correction
> above is what sent the next session at Part A expecting a 46 ms prize.
>
> **The measurement environment was contaminated.** An unrelated `sandvox.exe` was
> being respawned continuously by a tuner instance — not launched through
> `scripts/run.sh`, so the run mutex never serialised it. The *same* gate, same
> binary, same config, measured **101.75 / 14.60 / 127.87 ms** on three back-to-back
> runs. Every "render+present" figure in Correction 1, and the first round of A1/A3
> numbers, were taken under that load. On a quiet machine the offscreen 1080p sweep
> is **~10 ms/frame with shadows, ~5 ms without** — the renderer was never spending
> 46 ms.
>
> **A1 (in-window LOD handoff): LANDED, ~8-11%, saturates at 22-24 m.**
> Implemented as a `min()` clamp on `trace()`'s `tExit`, so `fs()`'s existing
> `traceFar(.., h.tExit, ..)` handoff picks up at the LOD distance and all the
> cascade machinery (level selection, one-sided seam dither, `tPrev` ordering) is
> reused untouched. Measured, shadows on: 10.36 off / 9.65 at 22 m / **9.02 at
> 24 m** / 9.22 at 18 m. Image A/B at 18 m: near field bit-identical, silhouettes
> and positions preserved, but mid-field per-blade grass visibly becomes 40 cm
> blocks — so the default is **24 m**, which keeps nearly all of a small win and
> costs almost no visible detail. It does NOT flatten the altitude curve; f8c1bc7
> had already done that.
>
> **A3 (shadow-ray LOD): IMPLEMENTED, MEASURED, DEFAULT-OFF — the premise is wrong.**
> Routing distant receivers' shadows through `farShadowed` was expected to roughly
> halve open-terrain cost. Measured: 10.35 ms control / 10.26 ms at 12 m (noise) /
> **497.46 ms at 0 m — 48x WORSE**. The reason A3 cannot work as specified: a fine
> shadow ray is cheap *because it terminates on the first blocker*, typically a few
> voxels away, with chunk-occupancy skipping the rest. A cascade shadow ray has no
> such early exit — it must cross the whole `TUNE_FAR_SHADOW_REACH` (60 m) before it
> may conclude "unshadowed", and for a NEAR receiver `farLevelForDist` picks level 1
> whose cells are only 4 voxels, so that reach costs the full 128-step clamp plus an
> occupancy lookup per step. The nearer the receiver, the worse the trade — exactly
> backwards from an LOD. The cascade shadow is not a cheaper shadow; it is the
> shadow that exists where there are no fine voxels to march. Code kept behind
> `shadowMaxDist = 999` so the experiment is re-runnable and the refutation is
> recorded where the idea lives (`sunShadowAt` in `raymarch.wgsl`).
>
> **What this means for the ranking below.** Item 5 (A3) is refuted. The remaining
> honest levers on shadow cost are making the fine ray *terminate sooner* (A2's
> sub-chunk occupancy bitmask) or casting *fewer* of them — not swapping which
> volume it marches. And since the whole quiet-machine frame is ~10 ms, Part A's
> remaining items should be judged against that, not against 46 ms.

> **CORRECTION 3 (2026-08-24). B2 was two thirds `genChunk`, and the surface frame is
> now GPU-bound.** Commits `0818548`, `74a2563`, `5eff6c4`, `b277641`.
>
> Correction 2 left B2 — the shift-demote `omap.Wait()` — as "the single highest-value
> item left", measured at 39.5 ms of a 40 ms paged frame. That measurement is right and
> the conclusion drawn from it was wrong, for a reason worth stating: **the fence is on
> the whole queue**, so its cost says only "this much GPU work was outstanding", never
> *what*. Draining the backlog immediately before `genChunk`'s submit splits it. The probe
> is in `stream.cpp` under `SANDVOX_PT_DEBUG` (`pre` vs `occ`) and is worth keeping:
>
> ```
> pre  mean  9.77 ms    backlog that already existed
> occ  mean 20.76 ms    genChunk + its 128 KiB copy, alone
> ```
>
> Two thirds of the "readback" was the shift's **own worldgen dispatch**. And the reason
> `genChunk` cost 21 ms for 1,024 chunks was not worldgen being big, it was two loop
> shapes:
>
> - **`treeAt` ran a 25-tile scan for every AIR CELL above ground**, and each tile's
>   `treeInfo` costs a `biomeAt` + a `baseHeight` + a `pondAt` (~20 hashes) — all paid
>   *before* the `y > vtop` test that throws the tile away. A cell 300 voxels up in open
>   sky spent ~500 hashes concluding "no tree here", and open sky is most of a streamed-in
>   vertical slab. Rejects are now ordered by cost (global `TREE_MAX_TOP` compare →
>   hash-only trunk site → `baseHeight` alone), with the exact per-species tests unchanged
>   underneath.
> - **`genCell`'s first ninety lines contain no `y`.** `baseHeight`, the pool discs,
>   `biomeAt`, `pondAt`, `shoreAt` are a pure function of `(x, z)`, and a chunk evaluated
>   them 4,096 times for 256 distinct answers. Split into `genColumn`/`genCellIn`;
>   `genChunk` walks column-major and evaluates the column once.
>
> `genChunk` 20.76 → **5.37 ms**. Whole-frame `--autofly-surface` p50 **57.1 → 25.3 ms**,
> p99 151.2 → 62.0, frames over 100 ms **47 → 0**. Determinism `7cfa2420` throughout —
> which is the real gate on both, since neither changes any arithmetic.
>
> **Two lessons for the next reader, both about measurement:**
>
> 1. **A fence tells you how much, never what.** Before optimising anything a stall is
>    waiting on, drain the queue in front of it and measure the two halves separately.
>    Three sessions treated this fence as a readback problem.
> 2. **This harness is real-time-paced, so p50 has a feedback loop in it.** Faster frames
>    accumulate fewer ticks per frame, and a frame with no tick is render-only and cheap —
>    so p50 moves faster than the work removed. Debug runs (with the extra drain) and
>    clean runs are *not* comparable to each other for this reason; compare clean to
>    clean at the same `--frames`. The numbers above are all clean 600-frame runs.
>
> **Where the frame goes now:** render + CA. The paging path is `genChunk` (5.4 ms/shift)
> plus fence serialisation, and CPU-side paging work has stopped mattering — the eviction
> and demote wins below moved p99 and `max` but left p50 flat, because they were already
> overlapping the GPU. Part A items are now the honest targets again, against a ~9-10 ms
> offscreen renderer.

> **CORRECTION 4 (2026-08-24). The 550 active chunks are a settling transient that lasts
> ~2,700 ticks, and it is LAVA. Narrowing the streaming wake by material is correct, is
> bit-identical, and buys nothing.**
>
> Two questions were asked in order, and the first one's answer made the second one's
> answer a foregone conclusion — which is the useful part of the record.
>
> **Q1: does the surface hold 550 active chunks because something never sleeps?** No.
> `--autofly-park` (main.cpp) is `--autofly-surface` that flies 300 ticks out to
> representative terrain and then STOPS DEAD — no forward motion, one frozen altitude
> regime, so the window stops shifting entirely and the only thing left in the count is
> what has not settled. Parked, with `SANDVOX_PARK_SETTLE=3000`:
>
> ```
> t375  630     t975  363     t1575 193     t2175 114     t2775  46
> t675  486     t1275 275     t1875 164     t2475  76     t3300  34
> ```
>
> Monotonic decay to ~30, which is the `sleep` gate's ≤32 budget. So it settles — but the
> half-life is ~700 ticks (23 s), not the ~1.5 ticks the "each shift wakes ~500, they
> settle immediately" arithmetic assumed. The probe fetches the still-active chunks'
> voxels through the ordinary on-demand fetch ring and histograms them against an
> equal-sized idle control:
>
> ```
> lava    active 97.1%   idle  4.2%      active-by-worldChunkY: -7:2 -6:17 -5:15
> stone   active 100%    idle 90.5%
> ```
>
> One band, world chunk Y −7..−3, which is `caveAt`'s deep-cavern lava (`worldgen.wgsl`,
> `cv == 2`, `y <= f2 + 2 && m2 > 190`). Worldgen writes it as a **3-voxel FULL-fullness
> slab on a noisy cavern floor**, so it spends ~90 s of sim time flowing out to rest, and
> **every window shift regenerates it unsettled**. A ~550 steady state under sustained
> flight is exactly what that looks like. Mid-flight the split is about half this lava
> band and half surface ponds (water 53%, sand 40%, lilypad/kelp/reed).
>
> **The lever this exposes, which nobody has pulled:** worldgen could lay that lava down
> already at rest. Placing it level and at a fullness the CA will not immediately move
> would let those chunks sleep after one tick like the stone around them. Bounding how
> long generated matter takes to settle is a worldgen-authoring question, not a CA one.
>
> **Q2: is `n > 0` too wide a streaming wake?** Yes as a predicate, no as an
> optimisation. `genChunk` woke every generated chunk holding any matter; it now wakes
> only chunks in which some cell satisfies `matCanAct` (`common.wgsl` — non-solid class,
> or a reaction bucket, or staining). The same function is what `sim_step`'s `main()`
> returns on, so a chunk that fails it is one where every thread of every colour pass
> returns before writing: dispatching it and not dispatching it are bit-identical, and
> `7cfa2420` held.
>
> The population is large and the win is zero:
>
> ```
> all-inert chunks (matCanAct false everywhere)   active 3/177    idle 134/183
> post-worldgen active chunks, all 32,768 slots, IDENTICAL BOTH WAYS:
>   old (n > 0)   t2 254  t3 169  t4 159  t5 148
>   new (canAct)  t2 254  t3 169  t4 159  t5 148
> ```
>
> **73% of non-empty chunks are all-inert and 1.7% of ACTIVE ones are** — which is the
> whole answer. The chunks the predicate declines to wake were already sleeping after
> ONE tick; the steady state is made of chunks that genuinely can act, and no
> conservative predicate can refuse those. What the transient costs is set by RESIDENCE
> TIME, not by how many chunks are woken: ~320 inert chunks x 1 tick against ~500 lava
> and pond chunks x hundreds.
>
> **Two measurement traps this walked into, both worth inheriting.**
>
> 1. **`activeChunks` cannot see a one-tick wake, so it is the wrong instrument for
>    this question.** `World::EncodeDirtyCopy` copies `DirtyNext` — `dirtyOut`, "active
>    NEXT tick" — so a chunk woken by `genChunk`, dispatched once, and found inert has
>    already cleared by the time it is sampled. The two predicates therefore produce
>    **bit-identical active-chunk counts by construction**, which is exactly what the
>    deterministic post-worldgen arm above shows. The work the change removes is in
>    `dirtyIn` (one tick of 54 dispatches per generated inert chunk, ~320 per shift at
>    ~1 shift/12 ticks = ~6 us/tick amortised) and no metric in the harness reports it.
> 2. **`--autofly-surface` at `--frames 600` is not a repeatable route.** Forward motion
>    is `dt`-integrated, so a faster arm flies FURTHER and over different terrain; the
>    altitude is pinned analytically for precisely this reason and the forward axis
>    never was. Five interleaved pairs gave active-chunk means of 506/489/368/365/492
>    (old) against 415/364/293/352/500 (new) — a spread wider than any effect being
>    looked for, and an early 2-run pair out of that spread read as a confident −20%.
>    Trust the deterministic arm; treat a single 600-frame p50 as +-15%.
>
> **The bound on this whole line of work, measured:** with `genChunk`'s wake disabled
> entirely (`wgAct > 99999`, an experiment, not a shippable state) `--autofly-surface`
> reads **active p50/p95/max 0 and frame p50 16.6 ms against 25.6 for the same-session
> control**. So the streaming wake is worth roughly 9 ms of a ~25 ms surface frame —
> and that is the CEILING on everything in this section — but every one of those chunks
> CAN act, so the whole 9 ms is reachable only by making generated matter settle faster
> (Q1's lever), never by refusing to wake it. (That run also freezes streamed-in matter,
> so it is a bound, not a candidate.)
>
> The narrowed predicate is kept anyway: it is one compare per non-air cell in a loop
> that already reads `materials[m]`, it makes the sleep rule one function instead of two
> readings of it, and its population grows with the plane size the 5 cm / 2048³ target
> implies. It is recorded here as measuring **zero** so it is not re-measured hopefully.

> **CORRECTION 5 (2026-08-24). Worldgen now lays the deep lava down AT REST. The
> settling transient is GONE — and it buys far less frame time than Correction 4's
> ceiling implied, which is the more useful half of this entry.**
>
> Correction 4 named the lever ("worldgen could lay that lava down already at rest") and
> put a 9 ms ceiling on it. The lever has been pulled. `caveAt` (`worldgen.wgsl`) now
> routes **every** carve through `caveFill(y)`: a carved cell at or below
> `LAVA_LEVEL = -80` is lava, above it is air.
>
> **Why that form, given `genCell` is a pure per-cell function.** "At rest" for a liquid
> normally means "flat, at the level its basin sets", and a per-cell function with no
> flood fill cannot find a basin. The way out is that a flat cut does not need to: the
> cave's own complement is already the container. Fill on DEPTH ALONE and at every
> y ≤ `LAVA_LEVEL` a cell is lava exactly when it is carved and stone otherwise — so the
> lateral boundary is stone at every level, everything under a lava cell is lava or
> stone, and the only lava/air interface in the world is the single plane y = -80.
> `stepLiquid` falls through all three of its rules to the "settled: no markDirty" tail.
> **The flatness comes from the constant and the containment comes from geometry that was
> already there.** The cut must live in `caveFill` and not in band 2, because band 1's
> floor reaches h-100 and a band-1 AIR cell beside a band-2 LAVA cell at the same y is a
> hole in the container; and the level must be a CONSTANT, because any per-column level
> reintroduces a step, and a step in a liquid is a flow.
>
> **The settle curve, which is the route-free measurement** (`--autofly-park`,
> `SANDVOX_PARK_SETTLE=3000`, same session, quiet machine, one run each):
>
> ```
>            t300  t375  t675  t975  t1275  t1575  t2000  t2475  t2775  t3250
>  before     521   570   450   342    241    200    393    197    152     75
>  after        8     0     0     0      0      0      0      0      0      0
> ```
>
> Not "faster to settle" — **settled**, from t325 to the end of the run. The park
> histogram loses the whole lava band (`active-by-worldChunkY` `-4:103 -3:24 1:67 2:93`
> becomes `1:67 2:98 4:10`) and lava disappears from the active set entirely.
>
> **Interleaved `--autofly-surface --frames 600`, 6 pairs** (one pair discarded: two
> `sandvox.exe` were live, one of them not through `run.sh`, and it put a p95 of 1257 ms
> into that arm — Correction 2's trap, still biting):
>
> ```
>                      before        after
> active chunks p50      470            23      -95%
> active chunks mean     502            78      -84%
> modelled CA         ~6900 us/tick  ~1250      -82%
> whole-frame p50       23.4 ms       22.3      paired deltas -3.7 -3.0 -1.8 0.0 +0.2
> whole-frame p95       51.2          51.7      flat
> whole-frame p99       62.6          62.5      flat
> ```
>
> **Read that carefully: the active-chunk count collapsed by 95% and the frame moved
> ~1.5-2 ms at p50, with p95 and p99 flat.** Correction 4's 9 ms ceiling does NOT
> decompose into "half lava, half ponds"; that experiment (`wgAct > 99999`) also froze
> all streamed-in matter, and it is now clear most of its 9 ms was that freeze rather
> than the CA the wake causes. Two things are worth inheriting:
>
> 1. **The ROADMAP §3.0 CA cost model overstates badly when extrapolated.** It was fitted
>    at 3-69 active chunks and is being read here at ~500 — a 7x extrapolation. It claims
>    5.5 ms/tick was removed; the frame says under 2 ms. Most of those 500 chunks are a
>    lava pool where nearly every thread returns immediately, not an average chunk. Treat
>    the model as an ordering, not a budget.
> 2. **The remaining surface-flight frame is render, not CA.** Correction 3 already said
>    so; this confirms it from the other direction, by removing nearly all the CA and
>    watching p95/p99 not move.
>
> **The route-free per-event numbers, from the `streaming` gate (fixed route, so these
> are directly comparable):**
>
> ```
> chunks stored per streaming gate   5,177 -> 2,958   (-43%)
> page pool high water              14,909 -> 13,338  (-11%)
> ```
>
> `--autofly-hard` (the descent, the control arm) does not regress — p50 2.2 ms both
> arms — and it stops being an active scenario at all: **active chunks p95 501 -> 0,
> max 595 -> 19, mean 42 -> 0**, modelled CA 808 -> 253 us/tick, which is the bare
> 54-dispatch floor. A descent through the cave band used to be a descent through
> flowing lava.
>
> Both are consequences of the same thing: matter that never moves is never modified, so
> it is never stored, and a fully-flooded cavern chunk is one material and demotes to a
> `UNIFORM` sentinel instead of costing a 16 KiB page. The pool got *less* pressured, not
> more, despite 3.7x the lava volume.
>
> **The content change, sized before it was made** (exact Python replica of `caveAt`'s
> integer arithmetic, so these are not estimates): lava goes from 0.34 to 1.26 voxels per
> column and from 11.5% to 14.0% of columns, i.e. roughly the same chance of meeting lava
> in a deep cavern, in pools ~4x deeper instead of a 3-voxel skin, still in world chunk Y
> -7..-5. It floods ~14% of band 2's void, so caverns stay walkable. `LAVA_LEVEL` is the
> one knob: -70 is 9x the volume, -95 is a quarter of it.
>
> **THE PINNED HASH DID NOT MOVE, and that is a finding about the gate, not about this
> change.** `7cfa2420` holds in both residency modes. The reason is that the `determinism`
> gate's world sits at the DEFAULT window origin `(0,0,0)` (`world.h`, `origin_{0,0,0}`)
> and never shifts, so its window is **y 0..511** — it contains no band-2 cavern at all
> (band 2 lives entirely below y = -2) and only the y ≥ 0 slice of band 1. **A worldgen
> change to the entire cave and lava band of the world moves no gate hash whatsoever.**
> Verified deliberately rather than assumed: setting `LAVA_LEVEL = 10` (inside the
> harness window) moves the hash to `f3236b6f`, and -80 does not. Anyone changing
> worldgen below y = 0 should know the golden hash is blind to it and lean on
> `--gate streaming` instead, which does fly.
>
> That same experiment is also the cleanest proof of the rest property, and a better one
> than the settle curve: at `LAVA_LEVEL = 10`, with lava flooding the bottom of every
> band-1 cave across the whole window, the `sleep` gate reports **0 / 32768 chunks
> active**. Zero, in-window, observed.

**Diagnosis in one line:** the frame cost is dominated by the raymarcher, whose per-ray
cost scales linearly with ray length through non-empty chunks and which has **no LOD
anywhere inside the 512³ window**; streaming adds a second, smaller cost that is specific
to horizontal surface flight. The recent page-table wins were scenario-shaped (descent
through uniform bulk) and structurally cannot help the surface case — the plan doc says
so itself (`docs/PLAN_page_table.md` §9.7: −55% on the descent, **−5% for a standing
player on terrain**).

Correction carried through this doc: the window is **512³ voxels = 32³ chunks of 16³ =
51.2 m per side** (`src/sim/world.h:22,31-33`). CLAUDE.md's "256³" is stale.

---

## Part A — why altitude kills framerate (renderer; owns the 5→0.2 fps curve)

The primary march (`trace()` at `assets/shaders/raymarch.wgsl:1076`, loop `:1153-1409`)
is a per-voxel Amanatides–Woo DDA with **exactly one** skip level: a per-chunk occupancy
counter (`:1160-1196`). No occupancy mips, no octree, no distance field.

### A1. No in-window LOD — the dominant cause
The far-field cascade (`traceFar`, `:1478-1591` — 8 levels, per-level occupancy skip,
well built) is reached **only after a ray exits the whole window** (`:4422`, handoff at
`h.tExit`). Everything within 51.2 m marches at full 10 cm resolution: a hillside 45 m
out costs the same per pixel as a wall 2 m out. Ray budget `TUNE_PRIMARY_STEPS = 4096`
(`tuning_params.def:337`) ≈ 4.6× the window diagonal — effectively uncapped. Underground,
rays terminate in 1–3 m (90 fps); over a meadow they run tens of metres (5 fps); from
altitude they run up to ~887 voxel steps each (0.2 fps). The fps ratios track ray length
in unskipped chunks almost exactly.

`docs/ROADMAP_scale.md:264-268` already names the fix ("replace TUNE_MICRO_LOD_DIST with
a projected-size test; same rule picks the far cascade level per distance") — known gap,
never built.

### A2. Chunk skip is all-or-nothing at 1.6 m, keyed on a count
`:1160-1162`: skip only if `occN == 0`. One grass voxel in 4096 defeats it and the ray
steps 16–48 voxels through the chunk. Two content classes make this systematic:
- **Grass/micro** (`MATF_MICRO`) is excluded from `occBlockers` by design
  (`src/sim/stream.cpp:176-186` — must not stop shadow rays), but counts in `occTotal`,
  which is what **primary** rays test. A meadow is a slab of never-skippable chunks.
- **Tree foliage** (`leaves` 580, `pine_needles` 600, `autumn_leaves` 620) has **no
  `micro` block** — plain solid voxels. A canopy is a wall of half-full chunks: never
  skippable, marched per-voxel, and it also blocks shadow rays (each of which then
  marches toward the sun through the same canopy).

### A3. Shadow rays double everything
`sunShadow` (`:1920`, called `:4645`): a fresh 384-step trace on **every lit pixel**, no
distance falloff, no LOD. Grazing sun over open terrain ≈ worst case for the chunk skip.
`farShadowed` (`:1602`, 128 steps, one cascade level) is an existing working model for a
cheaper shadow.

### A4. Page-table indirection made every step more expensive (constant factor)
`voxWordAt` (`common.wgsl:1066-1072`) is now a **dependent two-level load**: `voxels[]`
address can't issue until `pageTable[]` returns; JITTER adds `hash3` synth per call.
Multiplies against A1–A3. Quantify with the same camera pose under `--residency dense`
vs paged (the sanctioned differential oracle) before/after any fix.

### A5. Sentinel chunks are invisible to the renderer
The trace loop never reads `pageTable[]`. EMPTY chunks skip only because occupancy
happens to agree; **UNIFORM/JITTER chunks report occupancy full**
(`sim_occupancy.wgsl:173`) and are marched per-voxel (JITTER: + `hash3` per cell) despite
being analytically describable. Free performance: the EMPTY-jump code at
`raymarch.wgsl:1163-1195` is already correct — add a sentinel test beside the occupancy
test: blocker material → hit at chunk entry face; non-blocking → jump to exit face.

### A6. Micro/strand cost inside 40 m (surface-skim specific, not altitude)
Bounded by `TUNE_MICRO_MAX_PER_RAY = 8`, but each brick ≈ 32 nested DDA steps + **16
`voxWordAt` column probes** (`raymarch.wgsl:735-750`, `949-963`) + 4 `sin()`. The column
probes (~128 indirected reads/ray) are recomputed per cell of the same plant column.

Not implicated: god rays / silt (underwater-gated), `heatSpill` (occupancy-gated),
`waterAbove` (liquid-gated), occupancy/far-field rebuild passes (dirty-list-indirect).

---

## Part B — streaming cost specific to horizontal surface flight (the low-altitude 5 fps share)

A horizontal shift plane is a **vertical slab: 32×32 = 1,024 chunks from bedrock to sky**
(`stream.cpp:240-277`, plane built `:245-254`), landing ~every 2 ticks at sprint speed
(32.5 m/s → ~20 chunk-crossings/s vs 30 Hz tick). The descent scenario's plane is
horizontal — all-sky or all-rock, uniformly cheap. This is structural, not tuning.

### B1. The mixed surface band cannot be sentinelized
`PageTable::Classify` (`pagetable.cpp:245-360`): EMPTY / whole-word UNIFORM /
single-material JITTER. Grass+dirt+tree chunks fail all three → real 16 KiB page each:
decode (`stream.cpp:496`) + 4,096-word occ scan (`:537-550`) + 16 KiB `WriteBuffer`
(`:519`) + 3 per-slot 4-byte `WriteBuffer`s (`:554-557`); no demote-back possible. Accept
this as the floor; bound it (amortize plane fill) rather than chase a new sentinel now.

### B2. Synchronous `omap.Wait()` in the shift path — `stream.cpp:689-697`
The shift-demote occupancy prefilter maps a staging buffer and **blocks the CPU behind
the 1,024-chunk `genChunk` dispatch** (`:592`), once per shift. The buffer was pooled;
the wait was never made async (unlike `HarvestDemotes` beside it, `:882-959`). Top
remaining sync readback in the shift path.

### B3. `ctx.WaitIdle()` on snapshot staleness — `src/test/support.cpp:391-394`
When frames are already slow, ticks fall behind the readback ring, `snapshotStale`
latches, and a **full device drain** lands on the frame path. Slow-frame amplifier; this
is the "readback-cadence dilation" mechanism from the board.

### B4. Act-set wakes the whole surface band
`stream.cpp:735-755`: sky skips, buried bulk skips, but **mixed → wake unconditionally**
(`:740`) → `refilled_` → `cpuDirty_` → N26 dilation (`pagetable.cpp:798-806`). The
narrowing that saved the descent case (`RefilledSlot` comment, `pagetable.cpp:563-571`)
does not bite on mixed terrain. Watch the `SANDVOX_PT_DEBUG` `inUse=` curve
(`pagetable.cpp:842-845`): near-exhaustion is a positive feedback loop (fewer
demotable slots → bigger materialize set) that degrades frame time long before the
`std::abort()` — matches "5 fps but no crash".

### B5. `ChunkStore::Put` copies the RLE by value + O(regions) LRU scan per Put
`chunkstore.h:30`, lvalue call sites `stream.cpp:329,449`, scan `chunkstore.cpp:115-127`.
Worst exactly when RLE doesn't compress — mixed surface chunks. Cheapest fix:
`std::move` both call sites; make `SpillOverBudget` amortized.

---

## Plan of attack (ranked, for the implementing agent)

### Phase 0 — measurement harness (do first; everything else needs it)
1. Add `--autofly-surface` beside `--autofly-hard` (`src/main.cpp:1730-1750`): forward +
   sprint on the same fixed-tick `tick/90` phase form; hold altitude analytically via
   `World::TerrainHeight` (`world.cpp:442-452`, exact CPU mirror of worldgen — no world
   reads, no determinism risk). Phase bit: low skim (~canopy+5 vox) vs high cruise
   (~+150 vox) to isolate the altitude term. Self-contained edit to one `if` block.
2. Capture baselines on that schedule: frame p50/p99, GPU-vs-CPU split, and
   `SANDVOX_PT_DEBUG=1` `inUse=` curve. Also one paged-vs-`--residency dense` pair at a
   fixed high pose to price the `voxWordAt` indirection (A4). **These runs are the
   budget; cache the control arms.**

### Phase 1 — renderer (owns the altitude curve; biggest wins)
3. ~~**A5 first (small, safe, immediate):** sentinel-aware trace loop.~~ **DONE**
   (f8c1bc7). Reads the `pageTable[]` entry beside `chunkOcc`; UNIFORM/JITTER blocker
   → hit at entry face, non-blocking → reuses the EMPTY exit-face jump.
4. ~~**A1 (the big one):** in-window LOD handoff.~~ **DONE, and it is not the big
   one** — ~8-11%, saturating at 22-24 m, default 24. See Correction 2. Implemented
   as a `min()` on `trace()`'s `tExit` so the existing `traceFar` handoff is reused
   verbatim; `TUNE_LOD_HANDOFF_DIST`, hot-reloadable, >= 25.6 disables.
5. ~~**A3:** shadow-ray LOD — route beyond-a-few-metres shadows through the cascade.~~
   **REFUTED BY MEASUREMENT** — 48x worse at full effect (497 ms vs 10.35 ms). The
   cascade shadow has no early-out; the fine one does. Code kept default-off
   (`shadowMaxDist = 999`) as a re-runnable experiment. See Correction 2 and the
   comment on `sunShadowAt`.
6. **A2 (if still needed after 4-5):** sub-chunk occupancy bitmask (4³ bits = 2 u32 per
   chunk) so half-full canopy/meadow chunks skip internally. NB: the sim cell-mask
   experiment reverted in ROADMAP_scale.md:139-186 was measured in a dispatch-floor
   regime — that negative result does NOT transfer to the render path.
7. **A6 (surface skim polish):** hoist/caches the per-cell column probes in
   `traceMicro`/`traceStrands`.

### Phase 2 — streaming (owns part of the low-altitude 5 fps)
8. **B2:** ~~async-ify the shift-demote occupancy prefilter (one-shift-latent is fine).~~
   **ATTRIBUTED AND DOCUMENTED, NOT DONE — "one-shift-latent is fine" is false.**
   This IS the remaining surface-band cost, and it is not close. Measured under
   `--autofly-surface` with `SANDVOX_PT_DEBUG=1`:

   ```
   [pt-time] shift demote: gen=1024 cands=815 issued total 39.78 ms (occ 39.55)
   [pt-time] tick 941 materialize: 0.38 ms   freeprobe: 1.12 ms
   [pt-time] evict harvest: 256 items total 6.56 ms
   ```

   **39.55 of 39.78 ms is the `omap.Wait()`**, against sub-millisecond materialize
   and single-digit freeprobe/harvest. It explains the whole ~40 ms paged p50.

   It cannot simply be deferred, because the buffer has **two consumers with
   opposite staleness requirements** (full argument at the site in `stream.cpp`):
   the demote prefilter is stale-tolerant (`HarvestDemotes` re-verifies every
   candidate from the copied words — identity, freshness, `CpuDirty`, `Classify`),
   but the **act-set wake is stale-fatal and has no downstream verification** —
   `nonAir == 0u → continue` skips `RefilledSlot`, so a stale "pure sky" verdict on
   a chunk holding matter is silent voxel loss, i.e. the 217-page-fault bug.
   The conservative directions are opposites: zeros mean "test everything" for
   demotion and "wake nothing" for the act set. Waking the whole plane instead is
   closed too — measured twice as fatal pool exhaustion.

   **The real fix is to compute the act set on the GPU beside `genChunk` and read
   back only the (stale-tolerant) demote filter.** That is a design change, not an
   async-ification, and it is the single highest-value item left in this plan.
9. **B3:** replace the `WaitIdle` staleness drain with a bounded catch-up (skip/defer
   the snapshot consumer instead of draining the device).
10. **B5:** ~~`std::move` into `ChunkStore::Put`; amortize `SpillOverBudget`.~~ **DONE
    (the move); the scan half was a misreading.** Both call sites now move — `Put`
    takes by value and already moves internally, so an lvalue copied the whole RLE
    for nothing, worst exactly on the mixed surface chunks that do not compress.
    Safe from the reused scratch buffers because both encoders `clear()` their `out`
    first. **`SpillOverBudget` was NOT the per-Put linear scan described here**: the
    `while (regions_.size() > kMaxRamRegions)` guard runs first, so an under-budget
    Put costs one `size()` compare, and the O(64) scan happens only on an actual
    eviction, one region per pass. Left alone, with a comment saying so.
11. **B4 (only if the `inUse=` curve from Phase 0 shows dilation):** narrow the mixed-
    chunk wake — e.g. wake only mixed chunks with a non-settled neighbourhood, or cap
    per-shift refill admissions and carry the remainder.

### Verification budget (per CLAUDE.md — do not spray runs)
- Renderer changes are render-only: `--gate determinism` once per landed change; pinned
  hash `7cfa2420` must not move. Full acceptance ONCE at the end.
- Streaming changes: `--gate determinism` + the streaming gate; `--autofly-hard` must
  not regress (its numbers are the control arm — cache them).
- Success criteria: high-cruise `--autofly-surface` within ~2× of underground frame
  time; low-skim p50 comparable to the landed descent p50 (~3 ms); no new faults; pool
  `inUse=` stable under sustained surface flight.

### Where it actually stands (2026-08-24, quiet machine, after Correction 3)

```
--autofly-surface  paged   p50 25.3   p95 54.9   p99 62.0   max 78.0   >100ms 0
                           low-skim p50 29.5     high-cruise p50 22.8
--autofly-hard     paged   p50 3.0    p95 32.1   (no regression)
genChunk per shift         5.37 ms    (was 20.76)
demote candidates/shift    270        (was 850; 592 demote with no copy)
chunks stored per shift    28         (was 397)
store after streaming gate 2,958      (5,177 before Correction 5; 35,471 before Correction 3)
page pool high water       13,338     (14,909 before Correction 5)
active chunks under flight p50 23     (was 470 — Correction 5; the frame moved ~1.5 ms)
```

Success criteria from Phase 0 are met: high-cruise is now the *cheaper* arm, and the
altitude curve is gone. What is left in the surface frame is **render + CA**, not paging.

Ranked, for whoever picks this up:

1. **Part A (renderer).** A2's sub-chunk occupancy bitmask and A6's column-probe hoist,
   against a ~9-10 ms offscreen budget. This is now the largest term.
2. **The fence's remaining half** — CPU/GPU serialisation, not the readback. Removing it
   recovers only the CPU work that cannot currently overlap on a shift frame, which the
   measurements above suggest is single-digit ms. The act-set staleness argument in
   `stream.cpp` is still the blocker and is still correct.
3. **`treeAt` per-column hoist.** The tile *set* is column-invariant, so with `genChunk`
   now column-major the surviving tiles could be resolved once per column instead of per
   cell. Bounded by register pressure — the horizontal reject admits at most 3 tiles per
   axis typically but 5 in the worst case, so a fixed cache needs an overflow path.

**Housekeeping owed:** `check_invariants.py` should assert that `TREE_MAX_TRUNK_DM` /
`TREE_MAX_RAD_DM` / `TREE_BIRCH_RAD_DM` / `TREE_MAX_ABOVE` in `worldgen.wgsl` still
dominate the size table in `treeInfo` beside them — a bound that goes stale *downward*
shears canopies and moves the world hash. Not added here only because another session had
`check_invariants.py` checked out with uncommitted changes.

### How it stood (2026-08-23, quiet machine, after A1 + A5 + B5)

**Success criteria NOT met, and the reason is B2, not the renderer.**

```
--autofly-surface  paged   low-skim p50 39.6   high-cruise p50 50.8
--autofly-surface  dense   low-skim p50 71.0   high-cruise p50 69.5
--autofly-hard     paged   p50 4.6   p95 35.6   >100ms 0        (no regression)
offscreen 1080p            9.02 ms shadows on / ~4.6 ms off     (the renderer)
```

Three things to carry forward:

1. **The renderer is ~9 ms and is no longer the problem.** The remaining ~40 ms of
   the paged surface frame is the B2 `omap.Wait()`, attributed above by direct
   measurement. Do not spend more effort on Part A items expecting frame-time wins;
   A2/A6 are worth doing for their own sake, against a 9 ms budget.
2. **Paged is now FASTER than dense on this scenario** (44.1 vs 70.0 ms whole-frame
   p50), reversing the earlier reading. The old "dense is 25x faster" measurement was
   taken under GPU contention and should not be quoted.
3. **Every number in this document that predates Correction 2 is suspect.** The
   contention was invisible — a second process, not launched through `scripts/run.sh`,
   so the run mutex never serialised it. Check `tasklist | grep sandvox` before
   trusting a measurement, and take any timing twice.

### Explicitly out of scope for now
- New sentinel classes for the surface band (heightfield/partial-page): real but large;
  revisit only if B1's floor still dominates after Phases 1–2.
- Collapsing `simSlimBGL_`, occupancy opaque/media split — separate hash-gated work.
