# PLAN: frame-rate targets, ranked (2026-09-05)

The frame-rate survey of 2026-09-05: where the frame goes, the levers ranked by
expected ms per effort, what has already been refuted, and what landed. Read
CLAUDE.md's "When to run what" section before measuring anything — the
harnesses and their traps are all listed there.

## 1. Where the frame goes

The game is GPU-bound in every measured situation and the raymarch fragment is
the bound. CPU work matters only through the hitch tail.

| Situation (ms) | wall | raymarch | worldgen | far refill | CA | openness |
|---|---|---|---|---|---|---|
| Live game, standing (2026-09-03, `telemetry_capture.py`) | 21 | 17.7 | ~0 | ~0 | <1 | ~0 |
| Live game, flying (2026-09-04) | 82 | 22.6 | 7.9 | 16.1 | 3.2 | ~1 |
| `--perf --scenario surface-sprint` (2026-09-04, `build/perf.json`) | p50 22 / p95 62 | 11.2 | 7.9 | 0.02 | 3.0 | 1.3 |

Render-budget overlook (`build/render_budget.json`, 1080p, RTX 3060 Ti): noon
17.0 ms, dusk 19.1 ms; `noshadow` saves 3.0 / 3.7 ms, `nofar` 3.2 / 3.6 ms;
GI gather (`nogi`, PLAN_gi.md) 1.2 ms; `halfres` 70% of the raymarch.

Structural facts: no reprojection, no history buffer, no checkerboarding — the
only temporal reuse is the shadow cache. Sim and render share one queue, so in
flight the tick work (~13 ms) serialises ahead of the raymarch.

### Foliage cameras (2026-09-05, branch perf-foliage-cameras)

`--render-budget` gained two procedurally sited cameras, `meadow` (eye height in
the densest column-plant patch the CPU snapshot can find) and `canopy` (under
the largest crown, looking up through the leaves), four ceiling arms
(`micro1`, `plantlod4`, `fine2m`, `lod8` on every camera) and three counters
(`rmMicroEnters`, `rmPlantEvals`, `rmChunkSkips`; `kRenderStatSlots` 16 -> 19)
so plant cells ENTERED, plant EVALUATIONS (memo misses) and chunk-skip jumps
are separable from `rmMicroSteps`. The first run was cut short by the GPU
lock queue; what it recorded before that (1080p, RTX 3060 Ti, exclusive):

| camera | baseline | noshadow | nofar | nogi | lod8 | halfres |
|---|---|---|---|---|---|---|
| noon overlook | 19.6 | 17.4 | 15.5 | 17.6 | **8.9** | 5.4 |
| dusk overlook | 22.5 | 19.9 | 17.9 | | | 5.8 |
| submerged | 48.5 | 47.1 | 45.7 | | | 12.4 |
| meadow (430,240,172) | **23.4** | 22.1 | | | | |

Reading so far: the meadow frame is 23.4 ms at eye height versus 19.6 for the
overlook, and shadows are not it (1.3 ms). On the overlook the fine 10 cm
march from 8 to 24 m is still 10.7 ms of 19.6 (`lod8`), which is the largest
single lever in the table.

### THE MEADOW ARMS, FINALLY RUN (2026-09-11) — and they say the opposite of what the plan assumed

`SANDVOX_RUN_EXCLUSIVE=1 ... --render-budget --budget-cams meadow,canopy`,
1080p, RTX 3060 Ti. `canopy` DECLINED: the harness world has **0 leaf cells**
(no trees placed here) and only **756 plant cells in the whole 512³ window**.

| arm | p50 | saved | |
|---|---|---|---|
| baseline | 15.70 | — | |
| noshadow | 15.54 | 0.16 | |
| nogi | 14.40 | 1.30 | |
| nofar | 12.05 | 3.64 | |
| **nomicro** (`microMaxPerRay` 4 → 0) | **8.28** | **7.41** | **47.2%** |
| micro1 (`microMaxPerRay` 4 → 1) | 14.51 | 1.19 | 7.6% |
| plantlod4 (`plantLodDist` 16 → 4 m) | 14.81 | 0.89 | 5.6% |
| lod8 (`lodHandoffDist` → 8 m) | 12.19 | 3.51 | 22.3% |
| fine2m (`lodHandoffDist` → 2 m) | 7.64 | 8.06 | 51.3% |
| halfres | 4.18 | 11.52 | 73.4% |

**And the counters for the baseline frame say `rmMicroEnters = 0.0`,
`rmPlantEvals = 0.0`, `rmMicroSteps = 0.0`.** Not "few". Zero. No ray in that
frame enters a `MATF_MICRO` cell at all — `rsAdd(RS_MICRO_ENTER)` fires before
the LOD cut, so this is not plants-turned-to-cubes, it is no plants in shot.
(`rmPxSky` is 0.562 and `rmFarSteps` 95/px: the camera is 1.6 m up pitched
−8.6° over a nearly plant-free world, so it is mostly sky and cascade.)

**So removing code that never executes saves 7.41 ms — 47% of the frame.
The plant cost is REGISTER SPILL, not traversal.** `--shader-stats`, same
tree, `microMaxPerRay` moved in `tuning.json` (a compiled-in `TUNE_` const, so
both arms are launches and neither is a build):

| arm | Register Count | Binary Size | local memory |
|---|---|---|---|
| baseline | 168 | 1,632,384 | **+160 B/thread** |
| `microMaxPerRay = 0` | 168 | 1,490,944 | **+16 B/thread** |

`RENDER_STATS` is off in a plain `--shader-stats` run (`main.cpp` sets it from
`--telemetry`/`--perf` only), so this is `tracePlant`'s own pressure and not
the `gRs` counter array.

**This is the `sunShadowAt` shape, and it is the test the W2-A refutation
above told us to apply.** That note priced the lean variant at −0.03 ms and
explained why: `fluidMarch`'s values die inside its own region and never
coexist with `trace()`'s DDA, so deleting it moved the BINARY and not the
PEAK. `tracePlant` is called from *inside* the DDA loop, with `out : Hit` (26
fields), `tMax`, `tDelta`, `cell`, the media accumulators and the tile memo
all live across it — its live ranges interleave, and the number moves: 10x the
spill, paid as fill traffic by every pixel in the frame including sky.

**What this invalidates.** Ranked candidates 1–4 in memory
`project-raymarch-accel-research` (per-chunk plant-layer top, per-column plant
bake, mid-distance grass LOD, Chebyshev distance field) all attack the number
of plant EVALUATIONS. The measured cost of every evaluation in this frame is
zero, because there are none. Those levers are worth at most the `micro1` +
`plantlod4` band (~1–2 ms) and cannot touch the 7.41. **Do not build them
first.** The traversal cost of a real meadow is still UNMEASURED — this world
has no grass to measure — and needs a vegetated harness world before anyone
quotes a number for it.

### Partitioning the 144 B by plant kind (2026-09-11) — specializing by kind is DEAD

`raymarch.wgsl` gained four compile gates next to the `PK_*` constants —
`PLANT_GRASS_ON` / `PLANT_FLOWER_ON` / `PLANT_SHROOM_ON` / `PLANT_FERN_ON`.
All true is the shipping shader and is **byte-identical** to the pre-gate build
(168 regs, 1,632,384 B, +160 B/thread — re-measured, matches exactly), so the
instrument is free and permanent. Fold one to false and run `--shader-stats`.

| arm | spill | the branch it removes costs |
|---|---|---|
| all on | 160 B | — |
| fern off | 144 | fern **16 B** |
| fern + shroom off | 128 | mushroom **16 B** |
| + grass off (flower only) | 80 | grass **48 B** |
| no plant code at all (`microMaxPerRay = 0`) | 16 | flower **64 B** |

Caveat on the table: `PLANT_FLOWER_ON` is declared but **not wired** — the
flower branch is the fall-through of the shared column block, not its own `if`,
so the "all four off" arm is really "flower only". Flower's 64 B is obtained by
subtraction against the `microMaxPerRay = 0` floor. Wiring it needs the column
block restructured and was not worth a run.

**The conclusion: grass + flower are 112 of the 144 B, and they are the two
COLUMN plants — the common case.** There is no subset to specialize away. You
cannot drop grass and flowers and still have grass and flowers, so a
per-kind variant recovers at most 32 B (fern + mushroom, both tile plants) for
the cost of a second pipeline. Dead end; do not spend a run re-deriving it.

### LANDED 2026-09-11: the two-phase detail resolve — and the diagnosis above is WRONG about the mechanism

`trace()` no longer evaluates a detail model inside its DDA loop. A primary ray
that enters a `MATF_MICRO` cell within the LOD **records** the cell and its
entry `t` in four fixed scalar slots and keeps marching, treating the cell as
air; a block at the function's tail walks the records in `t` order, evaluates
`tracePlant` / `traceMicro`, and takes the first hit nearer than the opaque hit
the march resolved. (`DEFERRED DETAIL` in `raymarch.wgsl`, next to the `PK_*`
constants, is the full argument.) The three in-loop `return out` became
`break`, which is what gives the tail a single exit to run in.

**`--render-budget`, 1080p, RTX 3060 Ti, one settled world, HEAD's shader as
the before arm via `SANDVOX_ASSET_DIR`:**

| camera | baseline before | baseline after | "plant code exists" tax before | after |
|---|---|---|---|---|
| meadow | 19.73 ms | **9.35 / 8.69 ms** | 8.95 ms (45.4%) | **−0.33 / −0.07 ms** |
| noon | 16.54 ms | **9.54 / 9.49 ms** | 5.60 ms (33.9%) | **0.31 / 0.49 ms** |

(two after-runs on separate boots, to show the per-arm noise)

The tax is `baseline − nomicro`, i.e. what the frame pays for `tracePlant`
merely being compiled into the shader, on a frame whose counters still read
`rmMicroEnters = rmPlantEvals = 0.0`. It is now inside the per-arm noise. Frame
time roughly halved on both cameras — including `noon`, which is not a plant
camera at all, because the tax was always paid by every pixel.

**THE MECHANISM IS OCCUPANCY, NOT SPILL, AND THE TABLE ABOVE MISREAD IT.**
`--shader-stats` after the change:

| | Register Count | Binary Size | local memory |
|---|---|---|---|
| before | 168 | 1,632,384 | +160 B/thread |
| **after** | **128** | 1,621,888 | **+160 B/thread** |

The spill did not move by one byte. The register count fell 168 → 128, which is
16 warps per SM instead of 12 — and this shader's own `sgn3` note, thirty lines
into `trace()`, already measured that trade in the other direction: hoisting a
dynamic index there bought 128 → 168 regs and zero spill, and cost 3.5 ms
(+35%). Same trade, same sign, bigger. What moving the call out of the loop
bought was (a) the allocator's freedom to choose the low-register plan, and
(b) the spill/fill traffic that remains is now executed inside a branch only
the pixels that recorded a detail cell take, instead of once per DDA step per
ray. **"Local Memory Size" is not the objective function on this shader. Frame
time is.** The `≤ 80 B` acceptance target this work was briefed with was
unmeetable and was measuring the wrong thing; do not chase it.

**A per-column plant memo is NOT safe, and one was written and reverted here.**
An in-march memo that keyed column plants (grass, flowers) the way the tile memo
keys ferns and toadstools looks free — the wind sample and the
`plantColumnExtent` walk really are per-column constants. It is still wrong:
`tracePlant`'s column branch intersects only the segment of the plant **inside
the cell it was called for** (`yTop = min(1, sH - rise0)`, Y half-planes in
cell-local coordinates), so the remembered hit can only lie in the cell it was
computed in and every later cell of the column resolves to a memo miss. The
blades above the first cell a ray enters are never drawn. It shows in the
`plants` gate's own picture as horizontal bands of missing blade at every cell
boundary of the stand: **37,097 px over 24 SAD, 1.8% of the frame.** Collapsing
a column needs `tracePlant` to intersect the whole column in one call first.

**Image parity** (the gates cannot see it): `plants_grown.bmp` /
`plants_pressed.bmp` from `--gate plants`, before vs after, **678 / 548 px over
24 SAD (0.03%)** — against a same-shader re-run control of **617 px**, so the
residual is entirely the harness's own frame-to-frame noise (the shadow and GI
caches are not warmed deterministically). The fixture covers grass (column),
fern and toadstool (tile).

**Measure it at the same SCOPE on both arms** (CLAUDE.md rule 7 applies to
pictures too, and cost one confused reading here). The same comparison run with
the plants gate inside `--verify plants,player-plants,screenshots,determinism`
instead of alone reports **162,511 px** — not a regression, just three other
gates' worth of shared `World` and warmed caches in front of it.

**Still true and still unfixed:** the harness world has 756 plant cells and no
canopy, so every number above is a PRESENCE cost, not a traversal cost, and the
traversal cost of a real meadow remains unmeasured.

**What is worth doing, in order.** (a) ~~Get `tracePlant`'s pressure out of the
DDA's live range~~ — DONE, above. The two candidate shapes were a `SPEC_PLANTS`
lean variant
(the mechanism in `Simulation::BuildRaymarchVariant` is already built, tested
by `check_shaders.sh` and dormant behind `kRaymarchVariantOn`) — which needs a
per-frame "can a plant be hit" oracle this engine does not have, and which in
an outdoor world would rarely select — or a two-phase trace, which is what
landed: not a re-march (the record carries the cell, so nothing is walked
twice) but the same idea, and it needed no oracle at all. (b) Give the harness
a world with grass in it, or every future plant measurement repeats this one.
Note that `lod8` and `fine2m` are measuring the same in-window fine march and
are the OTHER half of this frame; they are not additive with `nomicro`.

## 2. Landed

- **Internal render scale + present mode + fps cap** (commit `7f214ac`):
  `render.renderScale`, `render.presentMode` (mailbox default; FIFO quantised a
  22 ms frame to 33), `render.fpsCap`. Harness p50 7.6 -> 3.8 ms at 0.75 scale.
  `rhi::CommandEncoder::BlitTexture`, `Simulation::BeginOverlayRenderPass`.

- **GI gather cache + openness refresh skip** (2026-09-05, branch
  `perf-gi-openness`; §3 items 1 and 4). Same binary, assets A/B against a
  detached `a0deae5` worktree; every boot checked for `timestamps: yes`.
  - `giGather`'s nine rays now run once per block-face (from its centre) into
    a second plane of `irradiance` (`GI_CACHE_BASE`), per chunk slot every
    `render.giCachePeriod` (8) frames or when the word reads 0, read bilinear
    over the face plane (`giBounceAt`). `--shader-stats`: raymarch fragment
    168 -> 168 registers, no local memory, binary +2.4%. `--render-budget`
    noon 1080p, two boots: `nogicache` (the old per-pixel gather) +1.01 /
    +1.26 ms over a 17.9 / 17.7 ms baseline; `nogi` now saves only 0.31 /
    0.45 ms, i.e. the whole feature costs ~0.4 ms where it cost 1.2.
    `cachesub2` saves 0.59 / 0.19 ms — inside the ±0.5 ms per-arm noise, so
    subdiv 2 is not a lever at this camera. `--perf` raymarch mean: idle
    18.6 -> 17.7 ms, flythrough 21.0 -> 20.1, sprint 13.4 -> 13.0 (in flight
    the walk zeroes the cache for every touched chunk, so most of it is
    live). `gi-bounce` with the cache: G +31.8 vs +32.8 per-pixel; a
    `screenshot_ground` triplet reads cached vs GI-off G +2.60/255 against
    live vs GI-off +2.72. The first `nogicache` number this package printed
    (12.83 ms) was a shader that FAILED to compile — `% 0u` const-folds to a
    Tint error — and a boot contaminated by another agent's exe; both are
    why the modulo is guarded and why the two later boots are the record.
  - The refresh skips the five-ray march for a slot whose stamp matches and
    whose column nothing within reach touched since its last full walk
    (`opennessGen` planes `OPEN_WALKED_BASE` / `OPEN_TOUCH_BASE`; dirty walks
    and stale-stamp arrivals stamp the 17×17 columns around them), keeping
    only the sun re-sample and decay; solid sentinel chunks settle interior
    faces without the origin search. Measured (`--perf --scenario`, openness
    node): **idle settles to 0.053 ms/frame from 0.287 (-82%)** once every
    slot has had one full visit after its column's last touch — about two
    refresh cycles, ~8 s at 30 Hz — and a diagnostic with skip visits as a
    no-op reads 0.007, so what remains is the sun re-sample kept on purpose.
    Sprint 1.30 -> 1.21 ms and flythrough 0.48 -> 0.50: in flight a whole
    chunk plane streams in per tick and the node is the DIRTY walk of those
    planes, which no refresh policy can touch; the sentinel fast path is the
    7% there. The plan's "1.3 ms in a world where nothing changed" was the
    sprint number; in an idle world the node was 0.27 and is now 0.05.
  - The `openness` gate passes again (it was known-failing since the harness
    pads went): its floor is BUILT now, floating over the real surface found
    by scanning the voxel column, because the old probe cell sat in a
    one-voxel pit of a slope and the walk's origin search could not march it
    (`openness attribution:` line in the gate output says so).

## 3. Open targets, in order

1. **Shadow rays and the GI gather.** LANDED 2026-09-05 (§2): the gather is
   cached per block-face (`giBounceAt`, ~1.0–1.3 ms at the noon overlook);
   `cachesub2` measured inside the noise and stays at 4. What is left of the
   shadow ray is the per-frame resolve of every visible patch
   (`shadowCache` node 0.6–1.5 ms), not its granularity.
   **Handoff 2026-10-13 (agent a1384c, item 6) — closed by measurement.**
   The same noon overlook,
   `--render-budget --budget-cams noon --budget-arms baseline,nocache`, main
   9a573ad, SANDVOX_RUN_EXCLUSIVE=1, three boots: frame 12.04 / 12.19 / 12.11
   ms, of which `shadow_resolve` 0.22 / 0.22 / 0.22 ms (the per-frame table:
   ray_start_trace 1.46-1.88, gi_gather 0.26-0.27, cloud_march 0.19,
   cloud_resolve 0.12); the `nocache` arm (the old inline per-pixel ray) 13.70
   / 13.85 / 13.87 ms, so the cache still saves ~1.7 ms. The 0.6-1.5 ms was
   the `--perf` node in motion before STAGGERED REFRESH and the nearest-patch
   read (shadow_resolve.wgsl's 2026-10-03 note); in flight today it is 0.17
   (peaks-route) to 0.26 ms (ticket flight) mean, p99 1.34. Nothing left to
   take here; the largest row of the per-frame table is now `ray_start_trace`.
2. **Async compute queue for the sim** (ROADMAP_scale.md §3.6, never started — still true 2026-10-02: `src/gpu/` has one queue).
   Hides most of the ~13 ms tick side under the raymarch in flight; zero win
   standing still. Large: the barrier generator in `vk_record.cpp` is
   single-queue, and the render needs a stable read of sim buffers (timeline
   semaphores or a double buffer). RESEARCH_streaming_hitch.md R5 scopes it.
3. **LANDED 2026-09-26 (audit 2026-10-02): the `cols` column-cache entry in
   `worldgen.wgsl` (map overhaul P4, `0e9b59e`) evaluates each column once and
   `main`/`list` read it back.** Original item: **Worldgen column redundancy across the vertical stack.** `genChunk`
   (worldgen.wgsl ~4389) computes `genColumn` once per (x,z) per CHUNK, and an
   X/Z shift plane is 32 chunks tall, so every column is rebuilt 32 times. The
   column half is the documented largest term (the fern-footprint second
   `landColumn` + the 25-tile `undergrowthSite` scan, which is not hoisted into
   the column prologue). A per-plane column pre-pass into a buffer that genChunk
   reads. NOT the rejected R2 spreading experiment (that redistributed the same
   work and paid ~350 us per extra dispatch); this removes work and adds one
   dispatch. Measure the column share with a stub arm before building.
4. **Openness refresh runs every tick regardless of activity** — LANDED
   2026-09-05 (§2): idle 0.29 -> 0.05 ms. The sprint 1.3 ms is the dirty walk
   of the chunk plane that streams in every tick; the only lever left there is
   the per-face cost of a NEW surface chunk's walk (the up-rays to the window
   top are the long ones), or walking a streamed plane over more than one tick.
   **Handoff 2026-10-13 (agent a1384c, item 5) — closed by measurement;
   the 1.2-1.3 ms sprint figure is
   stale.** On main 5805949 / 17059f3, SANDVOX_RUN_EXCLUSIVE=1: `--perf
   --scenario surface-sprint` (n=3) opennessRefresh 0.32 ms a frame (0.53 a
   tick at 0.68 ticks a frame) and opennessDirty 0.035; `peaks-route` (n=6,
   one tick a frame) opennessRefresh 0.35-0.37 ms, opennessDirty 0.02; the
   12-10-09 ticket flight (595 frames at 13.75 m/s) the openness node 0.33
   mean / 1.56 p99 ms. The refresh is a fixed 256-slot round robin whose
   per-slot work is real (no idle launches, unlike the far passes), and the
   dirty walk of streamed planes is now ~0.03 ms. Nothing left here worth a
   package; re-open only if a harness shows the node above ~1 ms mean.
5. **Streaming hitch tail** (stream CPU p95 35 ms / max 171, wake-wait max
   118, snapshot stall max 81). Deferred wake + one shift per frame landed;
   the next lever is the mirror's N26 dilation (dirty 43% + ring 15% of the
   pool at flight peak), then the 16 KiB upload per revisited slot.
   **Re-attributed 2026-10-13 — those numbers are stale and the tail is not
   streaming CPU any more.** `--perf --scenario surface-sprint` n=3 (main
   5805949, exclusive): frame p50 13.0-13.3 / p95 19.6-20.8 ms, 0 frames
   over 33 ms, stream CPU p95 1.8-2.4 / max 5.7-6.4 ms, 0 wake-wait misses
   in 1,394 shifts. The flight that still hitched was the 12-10-09 ticket's
   (far field driven): its slow frames were far-sieve ticks, and the far
   passes' biggest cost was launching workgroups that returned at once --
   fixed (DESIGN.md "The far passes launch only the workgroups that work"):
   frames over 33 ms 16.5-17.1% -> 9.9-11.3%, n=3. Left on that flight:
   the 32 map columns of a plane's first slice (`farMapFill` p99 12-13 ms)
   and window shifts at ~5 ms CPU each, ~3.1-3.3 of it eviction, of which
   the solute layer's batches are only 0.21 ms (the `--frames` shift line
   splits them out since 2026-10-13): the rest is `EvictSlots` itself on a
   plane whose chunks the CA has touched (modified_, so stored), not yet
   attributed further (`SANDVOX_PT_DEBUG=1` prints its `evict issue` counts).
   **Handoff 2026-10-14 (ticket 2026-10-11_12-10-09, all three levers).**
   (0) The harness: `--frames` fires an F5 at frame N/2 = 600, the start of
   the flight, and its far FullRefill was the source of the remaining
   box-face handoffs; measure with `SANDVOX_FRAMES_NO_RELOAD=1`
   (`build/arms.sh`-style loops: `scripts/perf/fly_ticket.sh` does not set
   it). (b) "Eviction 3.1-3.3 ms per shift" was a per-shift MEAN: 8 startup
   shifts (snapshot not yet valid, so every JITTER slot stored, ~870 x 37 us)
   at 31-50 ms, 86 flight shifts at ~0.01. Fixed by the pristine-sentinel
   skip (DESIGN.md, "A pristine sentinel is not stored either"): slowest
   shift eviction 0.34 ms; flight shifts cost 1.5 ms CPU each, of which
   eviction 0.23 (solutes). (a)+(c) map columns spread over a plane's slices
   and plane slices staggered off shift ticks: frames > 33 ms 3.9-5.4% ->
   1.8-3.2%, p99 39-41 -> 34-38 ms (DESIGN.md "Map columns ride the
   slices"). LEFT: p99 is still ~35 ms, not 33. What the slow frames are
   now: a tick frame costs ~19 ms of GPU with no far work at all (the sim
   tick, ~13 ms over the 6 ms render) and 21-27 ms with a slice, so any
   tick frame with a far slice is one vblank from 33 ms; the next lever is
   the sim tick itself (item 2, async compute) or a smaller far slice floor
   (`render.farPlaneBatch` 256 -> 128 is untested on this flight), not the
   far passes' shapes. `farMapFill` is latency-bound (~4.5 ms for 8 columns,
   one sub-column per thread); overlapping it with `far` was considered and
   rejected -- any barrier between them (FarVox WAW vs `farDown`) waits on
   all prior compute in Vulkan's stage scope, so it needs pass grouping in
   the recorder first.
   **Follow-up 2026-10-14, on wave-1014 (38fa573: + desert farRefineLevel 8,
   far burial, far-ground openness, shadow-cache fallback probes,
   molten/cloud).** `scripts/perf/fly_ticket.sh` now sets
   `SANDVOX_FRAMES_NO_RELOAD=1` by default (`=0` restores the F5). Ticket
   flight, one exe, arms interleaved, exclusive, n=3 each: shipped 2.7 /
   3.2 / 2.7% frames > 33 ms, p95 29.7-29.8, p99 36.7-37.7 ms; control
   (`SANDVOX_FARMAP_FIRST=1 SANDVOX_FAR_NO_STAGGER=1
   SANDVOX_EVICT_NO_PRISTINE=1`) 5.2-5.7%, p95 33.1-33.7, p99 40.3-41.7;
   0 box-face handoffs in every arm. `render.farPlaneBatch` 128 (asset-dir
   copy, same exe): 2.5 / 2.7 / 2.9%, p99 36.3-36.8 -- NOT kept: the floor
   never binds on this flight (far frames 192-198 vs 193-201, far GPU
   1.94-1.98 vs 1.93-2.02 s), so the difference is run noise. The merged
   tree's frames got heavier: p50 11.3 -> 13.1 ms, and a tick frame with no
   far work now averages 25.8 ms of GPU (19 before the merge) against 23.8
   with a far slice -- the remaining > 33 ms frames are tick frames, and the
   far field is no longer what separates them. Next lever: the sim tick
   (item 2) and whatever the merged render work added per frame (attribute
   with `SANDVOX_FRAMES_PASSES` + `SANDVOX_GPU_TIMER_SERIAL=1`). Daylight
   capture (`SANDVOX_AT_MINUTE=660`, `SANDVOX_AT_SHOT_EVERY=15`): no seam
   lines in any of the 40 flight frames.
   **Attribution of the wave-1014 render growth (2026-10-14, b9f3c2d).** One
   exe (b9f3c2d), asset trees only (SANDVOX_ASSET_DIR): `base` = main
   ab6b5c6's raymarch/shadow_resolve/cloud.wgsl + tuning.json; `full` =
   wave-1014's; each suspect arm = `full` with that commit's shader diff
   reverse-applied (`refine2` = `full` with render.farRefineLevel 2).
   `SANDVOX_GPU_TIMER_SERIAL=1`, `SANDVOX_FRAMES_PASSES`, `SANDVOX_AT_MINUTE=720`,
   no F5, exclusive, arms interleaved, n=1 each (differences under ~0.15
   ms are noise). Scenarios: the ticket flight (595 frames) and the ticket
   camera held still at noon (600 frames). **The pass that grew is
   `rm_world` (the primary raymarch) and nothing else**: base -> full
   +1.83 / +1.56 ms (flight, tick / non-tick frames) and +1.33 / +2.11
   (static). Every other pass is within noise; tick-frame GPU totals are
   equal in base and full (26.3 / 26.2 ms flight, 18.2 / 18.0 static), so
   the earlier "tick frame 19 -> 25.8 ms" was a cross-session comparison
   (different exe, different C++), not a shader cost -- the C++ side of the
   merge was not varied here. `rm_world` ms per frame SAVED by reverting
   each commit (full minus arm):

   | commit (owner) | flight tick | flight non-tick | static tick | static non-tick |
   |---|---|---|---|---|
   | base -> full, total | +1.83 | +1.56 | +1.33 | +2.11 |
   | 9058556 lod-seam far-ground openness (CURRENT cost; being reworked into the shadow-patch cache) | 1.38 | 1.17 | 1.12 | 1.77 |
   | 3f768dc shadow-cache fallback probes | 0.34 | 0.39 | ~0 | ~0 |
   | 22063f5+30e559d+baded23 molten/cloud + terrace/spill | 0.13 | 0.29 | 0.07 | 0.16 |
   | 93fe630 render.farRefineLevel 2 -> 8 (tuning) | 0.12 | 0.25 | 0.21 | 0.18 |
   | 93fe630 desert burial (shader) | ~0 | ~0 | ~0 | ~0 |
   | a15a37e lye liquid surface | ~0 | 0.10 | ~0 | ~0 |
   | c8120c7 seam bed shadow | not split (its diff no longer reverse-applies; inside the openness rework's lines) | | | |

   The openness change is 60-85% of the growth everywhere; shadow-cache
   probes matter only in flight (cache misses when the view moves);
   the rest are each under 0.3 ms. Arms: `build/mkarms.sh`, runs
   `build/attr.sh`, table `build/attrsum.py` in the worktree that wrote
   this (not committed; the recipe above is enough to redo it).
6. **Unbilled CPU frame-path work** — player collision runs a
   `std::function` per cell (~600 cells x dozens of calls per frame, fluid
   mirror evaluated before the kind lookup); avatar `FindClip` is a linear
   string scan 4x per frame. The perf harness runs no mobs or avatar, so none
   of it is measured. Add attribution before touching.

## 4. Refuted — do not retry

Sub-chunk occupancy skipping on the primary ray (`SUBOCC_SKIP`), the page-table
hoist, cascade shadows for near receivers, spreading worldgen over ticks
(P4-G/R2), cell-level active masks and workgroup-memory CA staging, any per-ray
state that lives across the DDA loop (168 -> 231 register cliff), the micro
column-probe hoist. Numbers are in PLAN_surface_flight_perf.md,
ROADMAP_scale.md §3 and RESEARCH_streaming_hitch.md.

Stale docs to be aware of: (PLAN_surface_flight_perf.md's B2 entry now says
DONE, 2026-10-13, with a re-measure: 0 wake-wait misses in 1,394 + 282
shifts); raymarch.wgsl's header says `Hit` has 26 fields
(it has 30); `build/shader_stats_before.json` is byte-identical to the current
stats and is not a baseline.
