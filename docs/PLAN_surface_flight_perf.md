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

### Where it actually stands (2026-08-23, quiet machine, after A1 + A5 + B5)

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
