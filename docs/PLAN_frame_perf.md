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
single lever in the table. The meadow ceiling arms (`micro1`, `plantlod4`,
`fine2m`) and the canopy camera have NOT been measured yet: run
`SANDVOX_RUN_EXCLUSIVE=1 bash scripts/run.sh ./build/Release/sandvox.exe --render-budget --budget-cams meadow,canopy`
and fill this table in before building anything from the candidate list in
memory `project-raymarch-accel-research`. `--shader-stats` before/after the
counters was also not taken; the raymarch fragment was 168 registers before.

## 2. Landed

- **Internal render scale + present mode + fps cap** (commit `7f214ac`):
  `render.renderScale`, `render.presentMode` (mailbox default; FIFO quantised a
  22 ms frame to 33), `render.fpsCap`. Harness p50 7.6 -> 3.8 ms at 0.75 scale.
  `rhi::CommandEncoder::BlitTexture`, `Simulation::BeginOverlayRenderPass`.

## 3. Open targets, in order

1. **Shadow rays and the GI gather.** `shadowCacheSubdiv` is 4 (16 patches per
   face, 34 rays / 100 px). Price 2 as a `--render-budget` arm — tuning only.
   `giGather` (raymarch.wgsl ~8187 -> ~3348) casts 9 `traceOpaque` rays on every
   lit near pixel every frame, reading an irradiance buffer that is already
   temporally filtered; cache the gather per block-face patch the way the shadow
   cache works (`shadowCached`, ~3856), or gather every other pixel.
2. **Async compute queue for the sim** (ROADMAP_scale.md §3.6, never started).
   Hides most of the ~13 ms tick side under the raymarch in flight; zero win
   standing still. Large: the barrier generator in `vk_record.cpp` is
   single-queue, and the render needs a stable read of sim buffers (timeline
   semaphores or a double buffer). RESEARCH_streaming_hitch.md R5 scopes it.
3. **Worldgen column redundancy across the vertical stack.** `genChunk`
   (worldgen.wgsl ~4389) computes `genColumn` once per (x,z) per CHUNK, and an
   X/Z shift plane is 32 chunks tall, so every column is rebuilt 32 times. The
   column half is the documented largest term (the fern-footprint second
   `landColumn` + the 25-tile `undergrowthSite` scan, which is not hoisted into
   the column prologue). A per-plane column pre-pass into a buffer that genChunk
   reads. NOT the rejected R2 spreading experiment (that redistributed the same
   work and paid ~350 us per extra dispatch); this removes work and adds one
   dispatch. Measure the column share with a stub arm before building.
4. **Openness refresh runs every tick regardless of activity** — 256 chunks per
   tick + the dirty pass, 1.3 ms GPU mean in sprint. Halve
   `opennessChunksPerFrame` (tuning); structurally skip sentinel chunks and
   chunks whose generation already matches with no changed neighbour.
5. **Streaming hitch tail** (stream CPU p95 35 ms / max 171, wake-wait max
   118, snapshot stall max 81). Deferred wake + one shift per frame landed;
   the next lever is the mirror's N26 dilation (dirty 43% + ring 15% of the
   pool at flight peak), then the 16 KiB upload per revisited slot.
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

Stale docs to be aware of: PLAN_surface_flight_perf.md lists B2 as not done
(the deferred wake shipped it); raymarch.wgsl's header says `Hit` has 26 fields
(it has 30); `build/shader_stats_before.json` is byte-identical to the current
stats and is not a baseline.
