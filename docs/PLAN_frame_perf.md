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
   **Re-opened and handed off 2026-10-15 (worktree agent-a034c8b7, branch
   `worktree-agent-a034c8b79d4282554`, on main e145446).** The wave-1014 work
   (fresh windows cast whole, far-patch openness fans, far gathers) took the
   ticket flight's resolve to **2.2 ms a frame**. READ THE TABLE RIGHT: the
   `--frames` per-pass CSV showed it as 5.5 ms on "tick frames" and 0 on the
   rest, because the sim PassTimer is kicked only on tick frames and the
   per-frame rows recorded on the frames between are summed into the next
   tick frame's row (tick-frame row / frames-since-last-tick = 2.07-2.33 ms
   for gaps of 1-3 frames). The same holds for `ray_start_trace`,
   `cloud_*`, `gi_gather`, `opennessRefresh`: divide by the gap, or read the
   all-frames mean (`framepass.py`'s "ms/frame"). Attribution: a counter
   build (resolve classes read back per frame, not committed) and stub arms
   (`nearone`, `full3`, `noopen`, `nomarch`, `nogather`, `nofg` ...), one exe,
   asset trees A/B, exclusive. Per frame: 19.5k near requests (5.4k fresh: 5.3k
   settled by the three probes, ~190 casting all sixteen), 23k far (5.8k
   fresh, 7.2k fans; marches 23k but only 0.11 ms of the pass). The sixteen-ray
   windows cost 0.61 ms and the fans 0.44 ms through warp divergence (a warp
   runs as long as its slowest lane; nearly every warp held one). Landed
   (DESIGN.md 9.s "The rays are shared"): a far slot between refreshes is held
   (`raymarch.wgsl shadowSlotHeld`; fresh far 5.8k -> 3.6k), one inlined copy of
   each march, and the resolve's rays as a per-warp task list. Result, n=2
   each interleaved: **2.19-2.20 -> 1.16-1.20 ms a frame**, wall mean 13.4-13.5
   -> 12.3, p50 11.6-11.7 -> 9.7-9.8. `--perf` (n=1, same exe and asset A/B,
   exclusive; the `shadowCache` node): forestfire 1.79 -> 1.23 ms, wall p50
   20.2 -> 19.5; flythrough 1.84 -> 0.94 ms, wall p50 13.3 -> 12.5. Pictures:
   the ticket camera still (SANDVOX_AT_LIST, clock frozen) differs from base
   by mean |dL| 0.34-0.36 against 0.45 between two base runs; in-flight
   frames differ run to run by more than the change can be seen through.
   REFUTED here: sorting a 256-lane
   workgroup's requests by kind (+0.4 ms; the group holds the SM until its
   slowest warp ends), seeding a fresh window from its pool parent (-0.05,
   dropped). LEFT (~1.2 ms): ~14k one-ray window refreshes, ~5k fresh windows'
   probes, ~23k far marches, ~4.5k fans -- now ray count, not divergence. The
   next levers: fewer fresh far patches (3.6k a frame are genuinely new
   ground at the screen edge, the handoff sphere and the level rings while
   strafing -- a world-keyed openness memo would only help re-visits), and the
   kernel's occupancy (80 registers, ~124 B a lane of driver shared memory,
   both from the far path; splitting near and far into two dispatches is a
   C++ pass-table change).
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

The media march (2026-10-15, §5 below): a one-cell load lead in the liquid run
(-0.1..-0.25 ms, nothing on top of the uniform-chunk jump); letting the liquid
run cross chunk faces itself (+2.6 ms on the deep-lake camera: lanes reach a
face on different steps and the per-lane face work serialises); a jump bounded
by a per-RAY region (the surface cell the ray entered: +2.8 ms, same cause);
one loop body serving both liquid and gas runs (+0.36 ms on the deep lake: the
gas tests ride every water step).

Stale docs to be aware of: (PLAN_surface_flight_perf.md's B2 entry now says
DONE, 2026-10-13, with a re-measure: 0 wake-wait misses in 1,394 + 282
shifts); raymarch.wgsl's header says `Hit` has 26 fields
(it has 30); `build/shader_stats_before.json` is byte-identical to the current
stats and is not a baseline.

## 5. Handoff 2026-10-15 -- watching a fire: the CA and the openness walk

Package of the 10-15 frame-rate pass (fire scenes). One exe per arm pair,
asset arms via SANDVOX_ASSET_DIR, SANDVOX_RUN_EXCLUSIVE=1, `--perf`; the
harness is quiet to +-0.03 ms per pass between boots, so n=1 attribution arms
are readable and the shipped numbers are n=2.

**"The CA doubled since 10-06" was a definition mismatch, not a regression of
that size.** On the same `--perf village-fire` harness the CA rows were ca
14.06 + ca1 8.26 = 22.3 ms on 10-01 (build/perf_vf_cur.json, post-fix) and
9.77 + 4.98 = 14.74 on 10-10 (a frozen e7f839f exe with its own assets,
same scene: 158,507 oil cells, 654k gas / 395k other cells a tick). The
"7.23 ms" of 10-06 (ROADMAP_scale.md) is `ca` alone. What DID grow was
10-10 -> e145446: ca + ca1 14.74 -> 16.41 (+1.7), opennessDirty 2.07 ->
2.56, opennessRefresh 0.91 -> 1.17.

Attribution (ms/frame, village-fire, same exe, asset arms, world hash
fc3db438 in every arm -- none of these changes is behaviour):

| arm | ca + ca1 | opennessDirty | opennessRefresh |
|---|---|---|---|
| control (e145446 assets) | 16.40 | 2.55 | 1.17 |
| sim_step.wgsl of 10-10 (e7f839f) | 14.55 | | |
| revert 8833133 (audit DET-1) only | 15.42 | | |
| flowMeasNote a no-op (4d42cce) | 15.55 | | |
| flow notes summed in workgroup memory | 16.54 (worse) | | |
| gasOuterSplat a no-op | 16.19 | | |
| every dirtyOut atomicOr issued twice | 16.35 (no cost) | | |
| openness: return after the blocker signature | | 0.35 | 1.05 |
| openness: no rays (origin search kept) | | 0.98 | 0.53 |
| openness: no sweep 1 of the column search (a94edc5) | | 2.20 | 0.91 |
| openness: no sun sample | | 2.21 | 1.09 |

The CA mechanism is REGISTERS (`--shader-stats`, pipeline `step`): 72
registers either way, local memory 64 B at 10-10 and 192 B at e145446.
DET-1's flagSupportLossNbr carried the actor cell into three inlined call
sites; that plus the flow note's per-move atomics pushed the kernel into
spill, which every cell of both substeps pays. The atomics themselves are
cheap (the doubling arm moved nothing; the shared-memory version cost more).

LANDED (branch worktree-agent-a83e8ec746f2fc2f2):
1. `flagSupportLossNbr(n, f, ...)` takes the face index (the far face is
   face f of n): same test, one u32. `step` 80 regs / 96 B local.
2. `main1` / Pipe::Step1: ca1's pipeline with the substep a compile-time
   constant (all `P.substep` reads go through `gCaSub`); `step1` is 64 regs,
   half the binary. ca1 5.07 -> 4.82.
3. The openness walk in three workgroup phases (DESIGN.md §9.x, "A walk is
   three workgroup phases"): blocker bitmap for dense chunks, origin search,
   then every surface face's five rays as one shared list. Exact (integer
   weights, half-voxel origins).

Shipped, n=2 each, interleaved with control:

| scene | pass | control | shipped |
|---|---|---|---|
| village-fire | ca + ca1 | 16.39 | 14.43 |
| | opennessDirty + Refresh | 3.71 | 2.65 |
| | sum of GPU passes | 37.85 | 34.80 (-3.0) |
| forestfire | ca + ca1 | 9.50 | 8.38 |
| | opennessDirty + Refresh | 4.82 | 3.44 |
| | sum of GPU passes | 28.33 | 25.82 (-2.5) |

World hashes unchanged (village-fire fc3db438, forestfire d7c2097f,
determinism gate 4993b9b0). `--verify` determinism, repose, oil-fire,
wind-gas, openness, gi-bounce, gi-nightfall, cave-window, support-flag,
support-flag-post, fire-down, tree-fell, solute-blood, blood-cloud,
flow-measure: PASS.

LEFT: reposeSnap 1.09 ms (skipping it for chunks without repose powder is
not exact, DESIGN.md), caMask 1.06 + 0.70, the openness rays (~1.2 ms of
the walk after compaction: the up-ray to the window top is the long one) and
the sun sample (~0.4 ms, still per-thread, could join the shared list).
## 6. Handoff 2026-10-15: the media march (water, smoke) — agent ad3b7e

Package: the primary raymarch's media march (water, smoke) and the owner's
two water tickets (2026-10-14_22-55-53 "30 fps staring out on the lake",
22-55-02 "water pouring through a break tanks to ~20"). Everything here is
`--render-budget`, 1920x1080, RTX 3060 Ti, `SANDVOX_RUN_EXCLUSIVE=1`, main's
e145446 exe with `SANDVOX_ASSET_DIR` trees; experiments were `debug.perfExp`
arms in ONE process (base2 drift <= 0.19 ms). Cameras: `lake` / `submerged` /
`smoke` on the harness map, `at` = ticket 22-55-53's
`--at 2959.05,585.40,-8807.40,-80.5,-24.1` on the default map (a DEEP lake:
no bed within the 10 m cap, 104 media cells/px).

**Attribution first** (perfExp arms, ms saved off the camera's baseline):

| arm | lake 13.0 | submerged 8.6 | at 15.7 | smoke 9.0 |
|---|---|---|---|---|
| liquid path capped at 2 m (everything that scales with water path past 2 m) | 2.76 | 2.69 | **4.29** | 0 |
| ...of it, the BED's shade under > 2 m of water (bed hits dropped) | 1.79 | 0.70 | 0.02 | |
| `waterAbove` + bed caustics off | 0.58 | 0.50 | ~0 | |
| `solPathLook` off (clean water still walks the solute table per chunk) | 0.23 | 0.20 | | 0 |
| reflection rays off (`noreflect`) | 0.76 | ~0 | | |

So on the owner's lake the cost is the WATER MARCH ITSELF (2 -> 10 m of
path, ~80 cells a pixel), not shading; on the harness lake (2.4 m deep, bed in
view) it is mostly shading the bed under the water, which is general opaque
shading and not this package's to cut.

**Landed (exact up to float rounding; render-only; no hashed state):**

1. **Uniform-chunk jump.** Occupancy word bit 30 = every cell of the chunk is
   one material+state (genChunk + both sim_occupancy entry points; 0 from
   every other writer = conservative). trace()'s liquid run crosses a uniform
   chunk of its own liquid in one box exit. `at` 15.77 -> 14.50 ms (in-process
   arm). The bit costs common.wgsl's `occBlockers` a 13-bit mask; the CPU never
   sees it (world.cpp keeps the 16-bit count).
2. **Gas run telescoped** (the liquid run's form: in-chunk word index, sums
   once, saturation / half-opacity as distances). `smoke` 9.03 -> 8.50 ms; the
   general per-cell gas loop is gone.
3. **Column walk** (`waterColumnUp` behind `waterAbove` / `waterTopAbove`):
   one page resolve per chunk, one material read per distinct word.
   `submerged` 8.63 -> 8.36 ms.
4. **Deferred MPM thickness**: fs() walks the MPM thickness only after the
   lake-ownership test, so a surface handed to the lake's shade (`mfLake`)
   no longer pays 14 field samples for a value it discards. `--fluid-bench
   worldlake` paired arms (in-process variant): fluid net 2.560 -> 2.467 ms
   (the bench camera sees few lake-owned MPM pixels; the gain grows with how
   much settled lake the live blocks cover on screen).

**Before / after** (control = e145446's assets, final = this tree, same
exe, two interleaved passes ctl / fin / ctl / fin, `baseline` arm only):

| camera | control | final | change |
|---|---|---|---|
| `at` (ticket 22-55-53's deep lake) | 15.82 / 15.79 | 14.45 / 14.44 | **-1.36 ms** |
| `smoke` (inside ether vapour) | 9.10 / 9.10 | 8.48 / 8.50 | **-0.61 ms** |
| `submerged` | 8.59 / 8.59 | 8.43 / 8.47 | -0.14 ms |
| `lake` (harness, 2.4 m deep, bed in view) | 13.46 / 12.98 | 13.07 / 13.19 | ~0 (boot noise is +-0.4 on this camera) |

Pictures (`scripts/perf/bmpdiff.py`, the budget's own baseline shots): every
control-vs-final diff is inside the control-vs-control one -- `at` max 1/255,
`submerged` max 2, `smoke` 7 px over 2 (control vs control 0, final vs final
30; max 3), `lake` 696 / 3,096 px over 2, max 4 / 5, against 1,833 px max 4
between two control boots (the shadow/GI caches are not warmed
deterministically). `--shader-stats`: raymarch fs 128 registers and 112 B
local memory before and after (binary 4,505,472 -> 4,520,064 B);
`occupancyDirty` 22 -> 24 registers; worldgen's dense `main` 96 -> 128 (the
min/max pair; only `--residency dense` dispatches it -- the paged `list` path
is 128 either way).

**The dam-breach ticket (22-55-02) — what was and was not established.**
Suspect (1), "while MPM is live the WHOLE frame switches to the all-features
raymarch variant", is refuted by the code, not by a run: `kRaymarchVariantOn`
has been false since W2-A (simulation.cpp), so every frame always draws the
universal pipeline and nothing switches. What MPM liveness does add is
`fluidMarch` on every pixel whose ray crosses the live block box, and inside
it the thickness walk on lake-owned hits (item 4 above). Suspect (2) dissolved
blood: `solPathLook` costs 0.2 ms over clean water and walks up to 100 slabs
only through chunks with solute, still unmeasured on a bloody lake. The 47 ms
frame is still NOT reproduced: the bench's camera stands 1.5 r back from the
lake, the ticket's 3 m over the breach looking down at -48 deg. Next step
there: a `--render-budget` camera that pulls the worldlake plug and shoots
from the ticket's pose (C++), with arms for fluid on/off and solute on/off.

**What is left on the deep lake, and the lever.** After the jump, the `at`
camera's water still costs ~3 ms past 2 m: grazing rays spend their 10 m
inside the chunk LAYER that holds the surface, which is never uniform (air
above it, often a partial top layer). The next lever is a per-chunk uniform
Y SPAN (the cells from the chunk floor up to the first non-uniform layer, and
from the bed up), so the run jumps the slab under the surface. It needs ~9
bits and the occupancy word has 1 left (bit 29) without repacking the
blocker COUNT that stream.cpp writes from the CPU (every reader only tests it
against zero, so a 1-bit blocker flag would free 13 bits) — i.e. a C++ change
plus both WGSL producers. Measure it the way the jump was: the region must be
per-CHUNK (coherent across a warp), never per-ray. (DONE: §8.)

## 7. Handoff 2026-10-15: water pouring through a breach (ticket 22-55-02)

**The scene, permanent.** `SANDVOX_MAP=harness --render-budget --budget-cams
pour,pourblood` and `SANDVOX_MAP=harness --perf --scenario pour`
(perfsuite.cpp, commit c743bdf9). The harness lake (r 68, 24 deep) gets a pit
cut beside it (24 x 25 cells, floor 20 under the bed) and, 5 ticks later, a
16-deep notch from the lake into it. The lake falls through as MPM + CA water
(9-12k MPM particles live). The eye is the ticket's: 3 m from the torrent at
-48.5 deg, over the pit, looking back at the notch. `pourblood` paints twelve
blood spheres into the lake on ticks 5 and 105, so the pour carries dissolved
blood. The budget camera freezes at tick 180. Two budget extras:
`SANDVOX_BUDGET_REPEAT=n` interleaves the arm list n times, and every arm row
prints its p10 beside the p50.

**What it costs (1920x1080, RTX 3060 Ti, in-process perfExp arms, quiet machine,
pourblood baseline 16.0 ms, base2 drift <= 0.16):**

| part of the raymarch | ms |
|---|---|
| everything MPM (march + every MPM shade; perfExp arm skips the fluid block) | **7.3** |
| . march loop skeleton (iterations, no field samples) | 2.1 |
| . field samples + bisection + lake-ownership sample | 2.55 |
| . calm-raft `shadeWater` (the second, lake shade on MPM pixels) | 1.41 |
| . shadeMpmFluid itself: thickness 0.6, normal 0.28, refraction 0.2, reflection 0.1, glint vis 0.1, foam 0.07 | ~1.24 |
| `solPathLook` everywhere (dissolved blood; CA + MPM water) | 1.1 |
| everything else (primary march through the lake/pit water, terrain) | 8.7 |

Pour vs pourblood: 12.97 vs 13.63 ms in one boot, so a bloody pour adds ~0.7 ms.
Per pixel (the step heatmap): 14.2 march iterations and 5.7 field samples.
Iterations: 4.75 class-0 chunk strides, 1.88 empty-Y-level skips, and 4.59 steps
through seam-ring chunks. 58% of samples return exactly 0. An oracle 4^3-brick
source mask per block would prove 44% of those zero (1.5 samples/px). The rest
sit in tap boxes that straddle a chunk face, in ring chunks, or beside water.
MPM shading covers 9.2% of pixels. 8.2% are calm rafts that ALSO run
shadeWater, and on half of those the raft weight is >= 0.998.

Sim, `--perf pour` (one tick a frame, 300 frames over the pour): ca 3.08 +
ca1 1.68, MLS-MPM 1.67 (g2p 0.55, p2g1 0.44, p2g2 0.27, gridUp 0.27, clear 0.21),
openness 0.55, waterBodies 0.19, solute 0.19. That is ~7.5 ms of GPU a tick.
The perf scenario's raymarch row is 8.5 ms p50 as the pour thins
(9.6k -> 1k particles).

**Does it reproduce the 47 ms? Not as one number, and the owner's inference is
wrong in a way that matters.** "Looking at the floor goes back to 60, so it is
rendering" does not follow. Ticks run at 30 Hz on the same queue as the render,
so a frame costs render + (frame/33 ms) x tick, and the second tick per frame is
throttled (main.cpp CPU budget / GPU-lag throttle). The same session's lake
ticket (22-55-53) gives two equations: 34.7 ms staring at the lake, with a lake
render the other agent measured at 17.5 ms at 1080p (~12.6 at the owner's 0.88
scale), and 60+ fps on grass. Together they put the owner's tick at ~20 ms of
GPU: the breached lake, a solute layer full of blood, swimmers. With that tick,
the pour's 47 ms needs only ~19 ms of render at the owner's scale (~26 at
1080p). The floor view then pays ~3 ms of render, the tick fits in its period,
and the frame reads 60. Here the tick is 7.5 ms and the pour render 16 ms at
1080p (~11.5 at 0.88), so the harness pour sits near the 60 fps edge rather
than at 20. Getting to the owner's frame takes a bigger flood (more excited
lake, more awake CA water, more solute), not a different render path.

**LANDED: (A) resolve a chunk once** (`fluidChunkHeld` /
`fluidSeamShellHeld`). On entry the march takes the class, the Y mask and the
ring chunk's six water faces in one go, so a ring step no longer re-reads up to
three neighbours' block-map and Y-mask words. It is exact: same loads, same
answers, no t changes. Registers stay at 128 / 112 B local. Measured with
`SANDVOX_BUDGET_REPEAT=3`, interleaved against perfExp-0 control on
`pourblood`: p50 16.15 / 16.13 / 16.40 -> 15.86 / 15.81 / 15.94, and p10
15.93 / 15.95 / 16.04 -> 15.41 / 15.56 / 15.57. That is **-0.3 ms p50,
-0.45 ms p10**.

**The other four, measured in the same run.** Alone, each was inside the noise:

- (E) a calm raft whose lake weight is >= 1 - 1/256 skips shadeMpmFluid after
  caFrac (the raft shade replaces it whole): -0.5 / +0.66 / -0.6.
- (D) skip the thickness walk when the CA column is >= 48 voxels. The walk
  cannot return more than 48, so max(thick, caPath) is caPath. Exact.
- (F) a multi-level Y jump that lands on the bit-identical plane. Each skipped
  level's step was absolute, and a level is >= 1 t-unit, so the 0.25 floor
  never skips a plane. This is NOT the rejected 2026-10-07 jump, which moved
  sample phase.
- (S) a settled-share-only sample for the mfLake test: same taps, same sums,
  no foam/velocity/composition reads. Alone it measured +1.1 / +1.1 / 0.

All five together (A+D+E+F+S) gave **15.10 / 14.91 / 15.02 against the same
16.15-16.40 control: -1.2 ms p50, -1.3 p10**, plus 14.73 in a separate boot.
They were NOT landed: 258 pixels moved by more than 15 (max 65) against a
control-vs-control floor of 0 above 15. (E) is the suspect. Its "< 1/256 of
the pixel" ignores HDR: the MPM sun glint (pow 380, x3 Fresnel) is far brighter
than 1.0, so 1/256 of it is still visible. The way back in: keep the glint in
E's early exit, or gate E on the glint being dark. Then re-measure the four
together with REPEAT. That code was not committed. Each change is ~10-35
lines, and the description above is the whole recipe.

REFUTED by register count: reusing the lake test's FluidSample in the shade,
and moving the thickness walk into the shade block. Each took the fs from 128
to 168 registers (`--shader-stats`), and the combined arm was +3 ms.

**Left, ranked.** (1) The tick, not the render, is the multiplier on the owner's
frame: the CA moving a flood (ca + ca1) and the solute layer under a bloody lake.
Price both on a bigger breach (widen the notch, or run the default map's lake
from a save). (2) A per-block FIELD CACHE: one 8 B word per block cell (fill,
fill*vy, blocker bit), built per frame in the pre table from the exact
fluidCellFromRef. Every field tap in a block chunk drops from grid + voxel +
material reads to one load, which covers march samples, bisection, thickness and
normal (~3.5 ms here). It needs a pass, a buffer (8 MB at kFluidBlocks 256) and
a binding, which could go in a fluidGrid tail. (3) A 4^3 brick source mask beside
it: <= 1.5 samples/px. (4) Calm rafts pay two water shades. Deciding the raft
weight before the MPM shade (E) is the place, once it can be measured.

## 8. Handoff 2026-10-15: the water-surface chunk layer -- agent a4fc38

Package: §6's "what is left on the deep lake", meaning grazing rays inside the
chunk layer that holds a lake's or the sea's surface. The uniform-chunk jump
never covered that layer. `--render-budget`, 1920x1080, RTX 3060 Ti,
`SANDVOX_RUN_EXCLUSIVE=1`. Every arm uses one exe (this branch's C++). The
control is perf-1015's assets via `SANDVOX_ASSET_DIR`.

**Attribution first.** One stub arm, `debug.perfExp 1` on the control: the
uniform jump taken in EVERY chunk a liquid run starts in. That is an upper
bound on any per-chunk jump. The picture is wrong; the arm is for timing only.
One process per map:

| camera | baseline | stub | bound |
|---|---|---|---|
| `at` (ticket 22-55-53's deep lake, default map) | 14.44 | 12.49 | 1.95 ms |
| `lake` (harness) | 13.17 | 11.17 | 2.00 |
| `submerged` (harness) | 9.37 | 8.06 | 1.31 |
| `peaksea` (peaks map) | 6.78 | 6.57 | 0.21 |

So §6's "~3 ms" is ~2 ms on `at`, and the same lever is worth as much on the
harness lake and under water. The sea camera is not march-bound.

**Design (landed).** The occupancy word's blocker COUNT is now a flag (bit 16):
every reader only tested it against 0. That frees bits 17..25 for a LIQUID
SPAN: layers [lo, lo+len) of the chunk hold one full-liquid key in every cell
(the longest such run). It replaces bit 30's UNIFORM (= span [0,16)). There are
three producers: genChunk, both sim_occupancy entry points and the CPU
store-hit upload. Each computes the span with a per-thread (key, bad-layer
mask) pair, reduced by min/max/or. That is exact for any cell-to-thread
mapping and needs no extra pass and no new workgroup memory beyond one atomic.

trace()'s liquid run jumps the box [chunk x/z] x [span y] when the cell it
started from AND the next cell are both in the span. The start cell's y is
rebuilt from `cell` and the step axis, so nothing new lives across the DDA
loop. The ray lands on the exit face's neighbour, with the other two axes
clamped into the box. The region is per-CHUNK, so a warp jumps together (the
per-ray region in §6 was +2.8 ms).

**Before / after.** Interleaved boots ctl / fin / ctl / fin, baseline arm.
These "fin" runs are from BEFORE the landing-cell clamp was added. The clamp
was then measured in ONE process against its absence and is neutral: `at`
14.61 with it vs 14.74 without, `lake` 14.29 vs 14.46, `submerged` 9.75 vs
9.67.

| camera | control | final | change |
|---|---|---|---|
| `at` | 14.44 / 14.42 | 12.74 / 12.65 | **-1.73 ms** (89% of the bound) |
| `submerged` | 9.37 / 8.50 | 7.26 / 7.23 | **-1.2..-2.1** |
| `lake` | 13.17 / 13.09 | 12.18 / 11.30 | **-0.9..-1.8** (noisy camera) |
| `peaksea` | 6.78 / 6.52 | 6.47 / 6.44 | ~0 |

In-process confirmation, same exe. `debug.perfExp 2` limited the span jump to
whole-chunk spans, which is the old uniform jump. Span jump vs uniform-only:
`at` 14.61 vs 18.97, `lake` 14.29 vs 16.30, `submerged` 9.75 vs 11.62,
`peaksea` 8.48 vs 7.94. The peaksea baseline was the first arm after a map
load, and its interleaved boots above show no regression. CAUTION: these runs,
and one more interleaved pair, came after the machine dropped into a slower
state, about +2 ms on every camera including the control (`lake` control
15.13, `at` final 15.49). Quote the first table.

**Pictures** (`scripts/perf/bmpdiff.py` on the budget's own shots):
- `at`: control vs final, 1,147..2,765 px differ, 0 over 2, max 1. Control vs
  control: 2,935 px, 0 over 2, max 1.
- `lake`: 268 / 561 px over 2, max 16. Two control boots: 502 over 2, max 5
  (GI noise on the bank cliff).
- `submerged`: 931 px over 2, max 39. Two control boots: 0 over 2. The
  difference is a 1-px line along chunk sides where the bed meets them. A ray
  grazing the shared edge of two adjacent bed cells hits one or the other,
  depending on rounding (the jump's analytic exit vs the DDA's accumulated
  tMax). There are also two isolated pixels over 12: one is an outlier in the
  control, one in the final. Neither difference is visible in the picture.
- Without the clamp, the final had dark one-pixel speckles on the harness lake
  bed (max 78). The clamp removed them.

`--shader-stats`: raymarch fs 128 registers, 112 B local memory (unchanged;
binary 4,525,440 B).

**Determinism.** No sim kernel reads the occupancy word, and the word is not
hashed. The `--verify` result is in the commit message.

**What is left.**
- On `at`, ~0.2 ms of the bound: rays whose run starts in a partial surface
  cell, or on a layer that a floating object breaks.
- `peaksea`'s 10.6 ms from the brief did not reproduce (6.5 ms here). Its sea
  is not where the cost is.
- On the harness lake, shading the bed under > 2 m of water (§6: 1.79 ms) is
  untouched. That is general opaque shading, not the march.

## 9. Handoff 2026-10-17: water down a hill, and the silverrun fall (tickets 11-14-41, 23-58-24)

Two owner tickets on the default map:

- 2026-10-16_11-14-41: "pouring a ton of water and having it run down this
  hill tanks fps from 60 to 25". Night, 41.6 ms at 0.88 scale. The owner's
  inference was "staring at the ground gives 70-90 fps, so it is rendering".
- 2026-10-16_23-58-24: river_silverrun's waterfall at 20 fps (49.0 ms), from
  a frame taken BEFORE the held-river overhaul.

**The scenes, permanent** (perfsuite.cpp; each loads the `default` map and
leaves it loaded, so list them after every harness camera):

- `--render-budget --budget-cams slope` (night: the ticket's tick 31558),
  `slopenoon`, and `--perf --scenario slope`. This is the ticket's --at pose.
  Water is painted every 2 ticks into the air over 15 brush sources (r 4), 11,
  17 and 23 m up the slope along the view. The sources stand on the ground
  READ from the voxels (World::TerrainHeight is only the noise). The budget
  camera feeds for 420 ticks and freezes while the sheet is still running. The
  setup's note prints a water census: total, partial, and how many cells are on
  the poured slope ahead.
- `--budget-cams silverrun` and `--perf --scenario silverrun`. This is the
  ticket's --at pose at its tick 7838 (sun at 20 deg). Nothing is poured: the
  river as the map makes it.
- `SANDVOX_BUDGET_TICK=<tick>` re-times all three cameras without a rebuild.
- Put `silverrun` BEFORE `slope` in `--budget-cams`. The slope camera's feed
  reads `fluidLive` back, so the MPM water it leaves behind differs from boot
  to boot. A camera listed after it inherits that water. Measured: silverrun
  run after slope drew different water in two boots (354k px over 2, max 197);
  run first, it was identical.

### What each frame costs (RTX 3060 Ti, 1080p)

**slope** (night): raymarch 15.9 ms. `nomicro` saves 9.3 ms: the heather is
the frame. The water arms are small:

| part | ms |
|---|---|
| water as air on the primary ray (all water work on water pixels) | 1.0 |
| shadeWater skipped | 0.95 |
| water side kind + falling sheet | 0.5 |
| bed caustics (waterAbove walk) | 0.3 |
| everything else water (each) | <= 0.15 |
| noshadow | 1.6 |
| nofar | 0.2 |

The sheet in this scene is thinner than the owner's: rmPxWater 1.2%, with
7-12k cells on the slope ahead. The rest runs off downhill (about 417k cells
lie in the eye's band).

`--perf slope` (one tick per frame, fed): GPU 26.2 ms per frame. The render
side is about 16.4 ms (raymarch 15.4, plus shadow cache, clouds, openness).
**The sim tick is 9.6 ms**: caLoop 6.2, farField 2.2 (farDown + farDownFeat
re-downsample the chunks the water keeps changing), MLS-MPM 0.7.

So the owner's "it is rendering" does not follow, as §7 already found for the
pour. Ticks run at 30 Hz on the same queue as the frames, so GPU time per
second is 30 x tick + fps x render. The owner's two readings (24 fps on the
hill, 80 fps on the ground) give 30 T + 24 R_hill = 30 T + 80 R_ground =
1000 ms. With R_hill = 12 ms (this camera at 0.88 scale), that solves to
**T = 24 ms per tick** and R_ground = 3.6 ms. The flood's tick takes about 70%
of the GPU while the owner stares at the hill. Water shading is about 1 ms of
the hill's render, and the heather is 9 ms.

The lever is the tick: the CA over a running sheet, plus the far downsample it
keeps dirty. That is the sim side, and the conduit agent is on it.

**silverrun** (sun at 20 deg): raymarch 20.5 ms. `--perf silverrun`: caLoop
0.01 ms. **The held river is asleep, so this scene is all render.** The 49 ms
in the ticket was taken before the overhaul. It does not reproduce on this
tree: raymarch is 26 ms in the perf scenario's dawn sky, and 20.5 at the
ticket's tick.

Attribution (`debug.perfExp` arms, one process, ms off a 20.5 baseline; the
base2 drift was 0.55):

| part | ms |
|---|---|
| shadeWater whole (skipped) | 9.0 |
| . waterNormal whole (flat normal) | 4.2 |
| . . the flowing two-copy wave field (flowing -> still branch) | 3.6 |
| . . . of it, the streaks (24 flowNoised a pixel) | 2.8 |
| . . . of it, the boils | 0.3 |
| . . the macro slope (4 liquidColumns + slopeFlowFrom) | 1.7 |
| . . openness taps / depth probe | 0.5 / 0.3 |
| . foam whole | 2.6 |
| . . flowFoamLines (3 currentAt + 4 noises) | 1.6 |
| . . convergence (4 currentAt, waterConvergeAt) | 1.3 |
| . traced reflection (`noreflect`) | 1.3 |
| . caustic curvature / solPathLook / glint | 0.9 / 0.65 / 0.56 |
| bed caustics on the bed under water (waterAbove walk) | 1.2 |
| waterfall mist on liquid pixels | 1.1 |
| far cascade (`nofar`: the lake above the fall, 38% far pixels) | 4.9 |
| noshadow | 1.75 |

### LANDED (raymarch.wgsl, near-field water only; exact up to float rounding)

1. `waveSlope`'s flowing branch evaluates both advected copies in ONE pass:
   `waveBands2` (the per-band tanh, dispersion sqrt, shoaling gain and
   footprint fade computed once for both points) and `flowStreakSlope2` (the
   flow frame and the octave fades computed once). The streaks are skipped
   when `st == 0`, where they are exactly zero (a pond's drift, a slow margin).
2. `flowFoamLines`:
   - the near current is handed over from `waterWaveNormal` (`gWaveFlow`,
     window hits only), where it used to sample currentAt again at the same
     point;
   - the distance fade is asked first;
   - one flow frame serves all four noises (`flowNoiseAlongF`);
   - the fleck noises run only where the line noise is over its threshold. The
     lines are sparse, and off a line the result was 0 anyway.
3. `shadeWater`: `waterReflBlur`'s 12 openness taps run only at night, since
   the blur is 0 by day whatever their answer.

**What it bought, and why it is more than the work removed.** `--shader-stats`
on the merge base: raymarch fs **168 registers / 80 B local**. This tree:
**128 registers / 112 B local**. Main's fragment shader had crossed the 128
cliff that §4 and the trace() header document (168 is 12 warps per SM against
16). These edits bring it back under. The work they remove is worth about 0.3
ms (an in-process perfExp A/B of old vs new paths on silverrun: 17.45 -> 17.16).
The rest is occupancy, paid by every pixel, which is why the slope camera
gains with 1% water pixels.

Exclusive, interleaved boots in both orders, merge-base assets vs this tree,
same exe, `SANDVOX_BUDGET_REPEAT=3`:

| camera | control | final | change |
|---|---|---|---|
| silverrun (pair 1: ctl, then fin) | 20.43-20.58 | 17.48-17.62 | -2.95 |
| silverrun (pair 2: fin, then ctl; silverrun listed first) | 19.73-19.92 | 16.96-17.11 | **-2.8 (-14%)** |
| slope (pair 1) | 15.72-15.87 | 14.85-14.98 | -0.9 |
| slope (pair 2) | 15.53-15.61 | 14.60-14.72 | **-0.9 (-6%)** |

Pair 1's silverrun world differed (it ran after slope; see above), so pair 2
is the number to quote.

Pictures (bmpdiff, pair 2): silverrun has 1,468 px over 2, max 18. They lie
on the cliff face and the grass bank (the GI / shadow cache warm noise §6
measured), none on the water. slope has 2 px over 2, max 3.

**FRAGILE.** The occupancy half of this is a register-allocation outcome, not
a property of the water code. The next edit anywhere in fs() can put it back
over 128. Check `--shader-stats` (raymarch fragment Register Count) on every
raymarch change. It went to 168 between §8 (128) and af1c7311 without anybody
noticing.

### Left, ranked

1. **The tick under a running sheet** (slope): CA 6.2 + farDown 2.2 ms on
   this thin pour, about 24 ms on the owner's. That is the sim lane.
2. **silverrun's far cascade**, 4.9 ms. That is the near/far water LOD lane.
3. The flow marks, 3.6 + 1.6 ms. They are look-defining. What is left in them
   is noise taps: 24 per pixel for the streaks, 4 for the foam lines. Cutting
   them needs an owner call, for example dropping the 6 mm streak octave or
   evaluating the marks at half rate.
4. The convergence, 1.3 ms: 4 currentAt, each with two bilinear grids. Inside
   one grid cell the central difference IS the bilinear patch's derivative, so
   about 39% of pixels (taps inside one node cell on both axes, measured grid
   dry) could take one 4-node fetch. That needs a transcription of flowGridAt
   in raymarch (drift risk) and is not done.
5. The macro slope's 1.7 ms is mostly the flow it switches on (gSlopeFlow),
   not its reads.
