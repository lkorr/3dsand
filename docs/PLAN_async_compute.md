# Async compute (Vulkan port phase 8) — measured, designed, built, OFF

Status 2026-10-02: **implemented behind `render.asyncCompute` (default OFF)**.
It is correct (sync validation clean, world hash identical on and off) and it
does **not** win on the RTX 3060 Ti, for the reasons measured below. The switch
stays because the scheduling machinery (two queues, timeline semaphores, a
recorder that places the join itself) is the piece any future overlap needs,
and because the result is hardware-specific: an AMD part, whose async compute
queues are the textbook case, may answer differently, and now one tuning flip
asks.

## 1. Measured first: what could overlap at all

`--perf` per-frame GPU attribution, RTX 3060 Ti, 1920x1080, exclusive lock,
main at 6c55214 (`build/base/perf_*_base.json` in the agent worktree):

| scenario | wall p50 | sim GPU / frame | render GPU / frame | sum | of which render-only derived (openness + glow, recorded on the TICK) |
|---|---|---|---|---|---|
| forestfire | 19.49 ms | 6.16 ms | 13.85 ms | 20.46 ms | 1.82 ms |
| village-fire | 28.06 ms | 15.68 ms | 13.32 ms | 29.03 ms | 1.84 ms |

The GPU is busy for the whole frame and the two halves are fully serialised
(sum of pass times ≈ wall clock). The naive bound — overlap the sim with the
render — is min(sim, render): 6 ms of 20 (forestfire), 13 ms of 29 (village).

**That bound is not reachable without a second copy of the world.** The
raymarch binds eighteen sim-written buffers (voxels, occupancy, farVox,
farOcc, pageTable, fluid grid + block map, gasOuter, gasFarOuter, waterFlux,
solTable/Pool, farMap, openness, glow, irradiance, rainMap, ...), and the sim's
first rows of every tick (mutate, rainFall, the CA) write the voxel pool. Tick
N+1 cannot start while frame N reads, and frame N cannot start before tick N
ends. The options the brief listed, priced:

* **Snapshot the render's inputs** so tick N+1 runs while frame N reads a copy:
  the pool alone is 576 MiB, farVox 1 GiB, plus every smaller buffer above —
  about 1.7 GiB more VRAM on an 8 GiB card, a per-tick copy of every page any
  of ~15 writers touched (the page table is CPU-allocated, so "which pages"
  is itself a new bookkeeping system), and double-buffering of the render-only
  derived state that is updated incrementally (openness, irradiance are EMAs).
  Rejected on memory alone.
* **Overlap only passes that touch nothing the other side writes.** This is
  what was built. The candidates are the tick's render-only DERIVED passes —
  the openness grid and the glow field (1.8 ms/frame in both scenarios) —
  against the head of the next render frame (sky-top reduce, ray-start map,
  wind streaks, shadow prepare: ~1.5 ms), which reads only what the derived
  passes also only read. Upper bound ~1.5 ms (7%) on forestfire, less on
  village (5%).
* farDown / farFill were NOT moved: particles and gas collide with farVox
  outside the window (`sim_particle.wgsl` `farBlocked`), so it is sim input,
  and moving one writer of it without the other would reorder their writes.

## 2. The design as built

**Queues.** `vk::Backend::CreateLogicalDevice` creates an async queue only on
request (section 4, finding 4: an unused second queue is not free on NVIDIA):
a compute-only family first (NVIDIA family 2), else a second queue of the main
family (`SANDVOX_ASYNC_QUEUE=same` forces that, `=none` creates none). No async queue (lavapipe, WARP) or no timeline
semaphores = single queue, the schedule the engine always had. With a separate
family every BUFFER is created `VK_SHARING_MODE_CONCURRENT` across the two
families (no ownership transfers: the set the async work touches is decided per
tick); images never reach the async queue.

**Which passes.** `pass_table.def` tags the five openness/glow rows
`PT_DERIVED` (they were the last rows of `PT_TICK`). **Switch off:**
`EncodeTick` records them itself, right after the tick table — exactly where
they always were, so the off-mode command stream is the pre-async one plus the
256-byte `copy_renderUBOTick` row. **Switch on:** `SubmitTick` sets
`Simulation::SetDerivedDeferred(true)` before `EncodeTick`, which then leaves
them out, and `EncodeDerived` records them from the tick's saved record
context and bind-group page into an async-queue encoder submitted right behind
the tick. On the async queue they therefore see the voxels AFTER the MLS-MPM
seam's settle and the far fill (render-only either way; no hashed buffer reads
them, and the async-on hash is the off hash).

*Audit 2026-10-02:* as first built, the off path ALSO moved the rows to the
end of the tick, behind the seam's settle — whose voxel writes then reached the
openness walk a tick before the occupancy it skips empty bricks by. Render-only
and small, but a behaviour change for a switch documented as scheduling only;
the off path now records them in place (`--shot` matches main 6c55214's exe to
within the shot's own run-to-run noise, and the village-fire perf hash is
identical).

One input had to move: the openness rows read the key light from RenderUBO,
which every frame UPLOADS at the head of its first command buffer — a write to
something the async work reads, which would make the frame wait at its very
first command. A copy row at the end of the tick (`copy_renderUBOTick`,
`RenderUBO -> RenderUBOTick`, simBGL_ binding 50, `RT` in sim_openness.wgsl)
gives them the same bytes in a buffer the frame never writes.

**Cross-queue ordering: two timeline semaphores.** Once the switch has been on,
every main-queue submit signals `mainTimeline_`; an async submit waits for the
latest value (it runs after everything the main queue was handed before it)
and signals `asyncTimeline_`. The main queue JOINS (waits that value) before
the first command that conflicts with the outstanding async work — a write to
anything it touches, or a read of anything it writes.

**How `vk_record.cpp` learns about two queues.** It does not grow a second
barrier generator. Every buffer access already passes through the tracker
(`TouchBuffer` / `TouchExtra`) before its barrier is derived, so that is where
both halves hook in (`Recorder::NoteAsync`):

* an async recording collects (buffer, written?) — the conflict set its submit
  hands the backend; it cannot drift from what the async work does, because it
  IS the uses that generated its barriers;
* a main recording made while async work is outstanding asks the backend
  whether each access conflicts, and at the first one that does SPLITS the
  command buffer: everything so far is the head (submitted without a wait, so
  it overlaps), the rest is the tail (waits). Table rows decide this before
  their pipeline is bound (binds do not cross command buffers);
  `BeginRendering` always joins (a draw's descriptor reads are not table uses).
  `Queue::Submit` sends head + tail as one batch.
* an upload that flushes into a buffer head and hits the conflict set, or an
  async submit made after the encoder began recording, joins at the head.

Pipeline barriers carry across the split unchanged: their scopes are defined by
submission order on the queue, not by command-buffer boundaries.

**Determinism is structurally untouched.** The sim's dispatches, their order and
their inputs are byte-for-byte the switch-off recording; the async rows write
only Openness / OpennessGen / Irradiance / Glow, which nothing in the sim
reads; everything they read is written by nothing until the join.

## 3. Verification

* `--gate determinism`, `SANDVOX_ASYNC_COMPUTE=1 --vk-validation`: final hash
  **5dabc010 = the pinned baseline**, gas digest reproduced, **0 validation
  messages** (sync validation on). Off: the same 5dabc010.
* `--verify gi-bounce,shadow-cache`, async on, sync validation: both PASS, 0
  messages, with 2 split command buffers and 2 head joins exercised.
* `--perf --scenario forestfire`, async on, sync validation: 1500 async
  submits, 1500 joins (600 at a buffer head, 900 split command buffers), **no
  synchronization hazard reported**. The run does report `vkCmdWriteTimestamp2
  ... query not reset` for the --perf pass timer's query pool — and reports
  MORE of them with async OFF (10 vs 2: the derived rows are untimed on the
  async queue), so it is pre-existing and not this change's.
* Final tree: `--verify determinism,gi-bounce,shadow-cache`, async on, sync
  validation: 5dabc010, both gates PASS, 0 messages (404 async submits, 2
  splits, 402 head joins in the device record of `build/last_run.json`).
* The perf arms below hold the scenario hash at 1d62bf26 in every arm.

* **Audit (2026-10-02), after the off-path fix:** async ON,
  `--vk-validation --verify determinism,gi-bounce,shadow-cache,openness,glow
  --shot-frames screenshot` on the compute-only family (device picked by
  name): 5dabc010, all PASS, 0 validation messages (415 async submits, 5
  splits, 410 head joins). `--vk-validation --perf --scenario village-fire`,
  async ON: 1350 async submits, 750 splits, hash d7633cb6 = off; the only 2
  messages are the pass timer's `query not reset`, which main 6c55214's exe
  reports identically (2) with no async code at all. Async OFF vs main's exe,
  village-fire, exclusive: p50 30.70 vs 30.66 ms, CPU encode 0.333 / submit
  0.055 ms both, every GPU row within noise, same hash. A window RESIZE with
  async on was not exercised (not scriptable headless); the present path goes
  through the same `SubmitMainImpl` and swapchain recreation waits both
  queues idle.

## 4. Before / after (in-process arms, exclusive lock)

`SANDVOX_PERF_ASYNC_ARMS=off,on,...` records the same scenario once per arm in
one process (boot-to-boot noise is ±1 ms, bigger than the lever). forestfire,
p50 frame ms:

| arms (in order) | results |
|---|---|
| compute-family queue: off, on, off, on | 19.41, **23.51**, 23.52, 23.46 |
| compute-family queue: off, off, on, on, off | 19.50, 19.78, **23.53**, 23.67, 23.55 |
| timelines only, rows kept on the main queue (`SANDVOX_ASYNC_DRYRUN=1`): off, on | 20.18, 19.58 |
| second queue of the MAIN family (`SANDVOX_ASYNC_QUEUE=same`): off, on, off | 19.47, **20.18**, 19.94 |
| same family, a fresh process, on only | 22.29 |

village-fire (the sim-heavy scenario), p50 frame ms:

| configuration | results |
|---|---|
| main's exe (6c55214), two boots | 28.29, 28.30 |
| this build, NO async queue created (`SANDVOX_ASYNC_QUEUE=none`, now the default) | 28.34 |
| second queue of the main family created: off, on, off | **31.70, 32.24, 31.87** |
| compute-only family created: off, on | **31.77, 43.98** |

Findings:

1. **Using NVIDIA's compute-only queue family costs ~4 ms a frame for the rest of
   the process**, and switching back off does not give it back (the off arms
   after the first on arm stay at 23.5). Inside those frames the CA went from
   5.2 to 9.2 ms and the raymarch from 11.5 to 13.6 ms with identical work and
   active-chunk counts — the GPU runs everything slower once that queue has
   carried work, consistent with the driver leaving a time-sliced
   graphics/compute scheduling mode on. The semaphore traffic alone costs
   nothing (dry-run arm).
2. **A second queue of the main family gains nothing**: 20.18 vs 19.47/19.94
   in-process, and 22.29 in a fresh process. Concurrency between two
   memory-bound passes (the openness march vs the ray-start march) on one SM
   pool does not shorten either, and each frame now pays two cross-queue
   waits.
3. The 900 splits per 300 frames show the overlap path is actually taken (the
   frame's head runs beside the derived work); it just is not faster.
4. **Merely CREATING a second queue costs village-fire ~3.4 ms a frame** (28.3
   -> 31.7 with nothing ever submitted to it; every pass ~12% slower — CA 12.6
   -> 14.2, raymarch 10.9 -> 12.6), while forestfire did not move. So the
   async queue is now created ONLY when asked for at device creation
   (`render.asyncCompute` on at boot, `SANDVOX_ASYNC_COMPUTE=1`,
   `SANDVOX_ASYNC_QUEUE=family|same`, or an `on` arm); switching the knob on
   later in a boot without a queue says so once and stays single-queue. With
   the knob off the device is created exactly as before (28.34 vs main's 28.3).

**Default: OFF, and no queue created.** No configuration measured faster than
single-queue; the cheapest one (same family) costs 0.5-4 ms depending on the
scenario.

## 5. What would change the answer

* A GPU with real async compute (AMD GCN/RDNA ACEs) — flip `render.asyncCompute`
  and re-run the arms; nothing else needs touching.
* More overlappable work. The only big lever is the render reading tick N while
  tick N+1 runs, which needs the render inputs double-buffered (section 1).
  If that is ever done for another reason (a network snapshot, a replay
  scrubber), the queue machinery here is what it would ride on.
