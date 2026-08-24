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
3. **A5 first (small, safe, immediate):** sentinel-aware trace loop. Read the
   `pageTable[]` entry beside `chunkOcc`; UNIFORM/JITTER blocker → hit at entry face,
   non-blocking → reuse the EMPTY exit-face jump (`raymarch.wgsl:1163-1195`).
   Render-only; hash must not move.
4. **A1 (the big one):** in-window LOD handoff. Let the primary march switch to the far
   cascade when projected cell size < ~1 px (distance threshold is an acceptable v1),
   instead of only at window exit. The representation already exists
   (`farCellShift(1)`); this is ROADMAP_scale.md:264 made real. Expect this alone to
   flatten the altitude curve.
5. **A3:** shadow-ray LOD — cap shadow length by camera distance and/or route
   beyond-a-few-metres shadows through the cascade (`farShadowed` at `:1602` is the
   template).
6. **A2 (if still needed after 4-5):** sub-chunk occupancy bitmask (4³ bits = 2 u32 per
   chunk) so half-full canopy/meadow chunks skip internally. NB: the sim cell-mask
   experiment reverted in ROADMAP_scale.md:139-186 was measured in a dispatch-floor
   regime — that negative result does NOT transfer to the render path.
7. **A6 (surface skim polish):** hoist/caches the per-cell column probes in
   `traceMicro`/`traceStrands`.

### Phase 2 — streaming (owns part of the low-altitude 5 fps)
8. **B2:** async-ify the shift-demote occupancy prefilter (same shape as
   `HarvestDemotes`: one-shift-latent result is fine).
9. **B3:** replace the `WaitIdle` staleness drain with a bounded catch-up (skip/defer
   the snapshot consumer instead of draining the device).
10. **B5:** `std::move` into `ChunkStore::Put`; amortize `SpillOverBudget`.
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

### Explicitly out of scope for now
- New sentinel classes for the surface band (heightfield/partial-page): real but large;
  revisit only if B1's floor still dominates after Phases 1–2.
- Collapsing `simSlimBGL_`, occupancy opaque/media split — separate hash-gated work.
