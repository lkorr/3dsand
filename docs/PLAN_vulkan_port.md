# PLAN: WebGPU/Dawn → Vulkan port

Status: proposed 2026-08-22 (phase 0 complete). Companion docs:
`docs/vulkan_pass_map.md` (the measured pass/resource dependency map),
`docs/vulkan_barrier_graph.md` (to be authored in phase 1).

## Why (and why now)

The end goal is not parity — it is what parity cannot reach:

1. **Sparse residency** (`VK_KHR_sparse_binding` / core sparse resources) for
   the voxel buffer. Measured on the default seed: the 512³ window is **11.25%
   non-air**, **84.8% of chunks are fully empty**, and at real 64 KiB page
   granularity **6,801 of 8,192 pages are fully empty → 86.9 MiB resident vs
   512 MiB dense (83% saved)**. Empty chunks cluster (sky), so page granularity
   costs almost nothing over the chunk-granular ideal. A 4 GiB virtual buffer
   backed only where non-air lives makes a 1024³ simulated window cheaper in
   physical memory than 512³ dense is today.
2. Explicit memory/heap control, async compute queues, subgroup ops — the
   latter only in derived render-only passes (occupancy, compaction); rule 1
   bans scheduling-dependent ops in sim kernels, and that ban survives the port.

## Measured baseline (2026-08-22, RTX 3060 Ti, seed 1337)

Occupancy (settled, 300 ticks, hash `b3c643a2`): 15,093,971 / 134,217,728
non-air (11.25%); chunks: 27,794/32,768 empty, 2,338 fully full; per-Y: terrain
occupies chunk layers y0–y4, everything above y≈80 is pure sky (17,408 chunks);
zero layers are fully solid (caves perforate even y1). The sparse payoff is
almost entirely sky.

Per-pass GPU time (µs/tick, per-ComputePassEncoder granularity, 120-tick avg):

| pass | settling | active | settled |
|---|---|---|---|
| prep (mutate+explode+compact) | 6.0 | 6.6 | 7.1 |
| CA (54 color×substep, aggregated) | 473.0 | 363.7 | **184.6** |
| particle chain (3 passes) | — | 28.4 | — |
| compactNext | 4.9 | 2.7 | 3.8 |
| occupancyDirty+pick | 12.6 | 14.7 | 9.8 |
| farDown | 31.7 | 53.5 | 5.5 |
| occupancyFull+pick (1-in-15 ticks) | 93.9 | 95.0 | 95.6 |
| **total** | **622** | **565** | **306** |

Render for comparison: ~12–16 ms/frame at 1080p — the sim is not the frame
cost; the port's perf case is idle overhead + memory, not throughput.

**Finding (rule-2 violation, pre-existing):** a fully settled world (0 active
chunks) still burns 184.6 µs/tick issuing 54 empty `DispatchWorkgroupsIndirect`
calls (~3.4 µs driver overhead each), plus 95 µs full-world occupancy scans on
hash ticks — the "costs nothing when idle" state costs ~60%+31% of the settled
tick. Tracked as a port-adjacent fix (phase 3/8): cheaper dispatch path first,
then a skip-encode-when-provably-empty option.

## Constraints that outrank the schedule

- **Rule 1 (bit-determinism)** is non-negotiable and verified on ONE vendor
  (DESIGN.md §14 risk 3 open). Dawn auto-generates barriers; in Vulkan they are
  hand-written across a chain with ~60 read-after-write hops on the voxels
  buffer per tick. A weak barrier is a timing-dependent race that passes
  locally and desyncs elsewhere. **The barrier graph is the highest-risk
  artifact**: it is authored as an explicit document + declarative pass table
  (phase 1), reviewed adversarially, and barriers are *generated* from that
  table — never scattered by hand at call sites.
- **Unbound sparse pages are not guaranteed to read as zero everywhere.** Sim
  kernels legitimately read empty neighbors (a dirty chunk at a sky boundary),
  so unbound pages WILL be read and must deterministically return 0 (the zero
  word is inert air by construction). Policy: sparse mode requires
  `residencyNonResidentStrict`; without it, sparse is disabled and the dense
  path runs. The existing "unloaded space is solid and inert" window authority
  stays untouched as the outer guard. This extends the rule; it never replaces
  it.
- **`--selftest` is the acceptance gate and stays green at every checkpoint.**
  Strategy: Dawn keeps working throughout; Vulkan lands as a second backend
  behind the same seam, validated by cross-backend world-hash equality (same
  seed + same tick = same hash) before it renders a single pixel. *(This
  strategy ran its course: Dawn was removed 2026-08-22 once it had agreed with
  Vulkan on all 23 gates for a full phase. The acceptance gate itself is
  unchanged, and the cross-backend hashes it produced are now pinned constants
  in `src/gpu/vk_smoke.cpp`.)*

## Shader strategy

WGSL stays the authoring language. Tint is already in the dependency tree as
linkable libraries (including `tint_lang_spirv_writer`): the Vulkan backend
compiles the exact string `ShaderConstantPrelude() + TuningWgslBlock() +
common.wgsl + body` to SPIR-V at load. `LoadShader`, F5 hot-reload,
`check_shaders.sh`, and the tuner pipeline survive unchanged. No GLSL, no
shader rewrites, no second source of truth.

## Phases and checkpoints

Every checkpoint = `bash scripts/build.sh --selftest` green (baseline
known-fails only: pond-freeze, mob) with the actual output reported.

**Phase 0 — measure (DONE).** Pass map, occupancy histogram, per-pass timing.
Artifacts: `docs/vulkan_pass_map.md`, `--measure` harness
(`src/measure/measure.*`, `src/gpu/passtimer.*`).

**Phase 1 — barrier graph + RHI design (doc only).**
Author `docs/vulkan_barrier_graph.md` from the pass map: every hazard edge with
exact `VkPipelineStageFlags2`/`VkAccessFlags2` scopes; submit-boundary sync
design (upload path replacing `queue.WriteBuffer` semantics, readback-ring
fences, eviction ordering, page-flip safety); the zero-init list; and the shape
of the declarative pass table `EncodeTick` will iterate. Adversarial review by
a second agent before anything consumes it. Design the RHI header (~10
concepts, wgpu-shaped so the Dawn impl is a passthrough).
*Checkpoint: docs reviewed; no code change; gate trivially green.*

**Phase 2 — RHI seam under Dawn (mechanical, hash-neutral).**
Confine `wgpu::` to `src/gpu/` behind the seam; migrate the 24 files.
Restructure `EncodeTick` to iterate the declared pass table (still Dawn
underneath — Dawn's auto-barriers make the table's correctness testable before
Vulkan exists: a checker script can diff the table's R/W sets against WGSL
bindings, the repo's "two places that must agree" pattern). Hoist the
in-render-pass `WriteBuffer` (`mbInstBuf_`) — a latent bug worth fixing under
Dawn anyway.
*Checkpoint: full gate green, world hash byte-identical to pre-refactor, zero
behavior change.*

**Phase 3 — Vulkan compute backend, headless.**

> **[AS BUILT] Phase 3a deliverable 0 — the golden-hash gate.** Phase 2b
> discovered the full selftest PASSing on a build where the mutate and explode
> passes dispatched **zero workgroups**: the determinism gate compares two runs
> of the same build, so a sim that quietly does less stays self-consistent and
> green. `tests/baseline.json` now carries `"determinismHash": "7cfa2420"`; the
> gate compares the 200-tick final hash against it and reports a REGRESSION
> (exit 1) on mismatch, with an absent key meaning "not pinned" so the old
> behaviour survives. An intentional content change flips the value in the same
> commit, exactly like flipping a known failure — `tests/BASELINE.md` says when
> that is legitimate and when it is someone silencing a real regression. This
> protects every remaining phase of the port: cross-backend hash equality is
> only meaningful if the reference value is itself pinned.
>
> **[AS BUILT] Phase 3a deliverable 1 — build plumbing.** Three decisions:
>
> * **Vulkan headers, no loader.** `Vulkan::Headers` comes from Dawn's own
>   dependency tree; the loader is checked out there but never built, so there
>   is no import library to link and requiring a system SDK was rejected.
>   `src/gpu/vk_loader.*` does a volk-style `LoadLibraryA("vulkan-1.dll")` +
>   `vkGetInstanceProcAddr` walk across three tiers (global/instance/device).
>   The whole target compiles with `VK_NO_PROTOTYPES`, which makes a direct
>   `vk*` call a compile error rather than an accidental static bind — the
>   property that keeps "we do not ship a loader" true by construction.
>   Measured: instance version **1.4.341** on this machine.
> * **Tint links as a library, in process.** `tint_lang_wgsl_reader` +
>   `tint_lang_spirv_writer`, both `EXCLUDE_FROM_ALL` upstream so they build
>   only because we name them, and both carrying their own PUBLIC include
>   directories (nothing hardcodes a path into the dep cache). The pipeline is
>   `wgsl::reader::Parse` → `ProgramToLoweredIR` → `spirv::writer::Generate`,
>   the same sequence `tint.exe` runs. The offline fallback (precompiling .spv
>   at build time) was NOT needed, which matters: it could not have survived
>   phase 3c, because F5 rebuilds the constant prelude from live tuning values
>   and a precompiled blob freezes them.
> * **VMA is VENDORED at `src/gpu/vma/`, not fetched** — and the reason is a
>   trap worth knowing. `FETCHCONTENT_FULLY_DISCONNECTED` is ON in a configured
>   build tree (it is what stops CMake re-checking the network for Dawn every
>   configure). Under that flag a *newly declared* FetchContent dependency is
>   never downloaded **and CMake still exits 0**: it warns, and leaves
>   `${vulkanmemoryallocator_SOURCE_DIR}` pointing at a nonexistent directory.
>   The resulting empty include path broke the build in an unrelated place (an
>   ImGui/Dawn header failure) and took a while to trace back. Any new
>   FetchContent dependency in this repo is correct only for someone who also
>   wipes their build directory. See `src/gpu/vma/VENDORED_FROM.md`.
>
> Proven, not assumed: `--vk-info` compiled a WGSL compute shader to 230 SPIR-V
> words with the correct `07230203` magic, and loaded the Vulkan library.
>
> **[AS BUILT] Phase 3a deliverables 2+3 — backend foundations and `--vk-info`.**
> `src/gpu/rhi_vulkan.{h,cpp}` implements the §4 submit-boundary designs:
> registry-based zero-init, the pending-upload queue with both classes, a fence
> on every submit, VMA allocation, Tint-backed shader modules, descriptor/
> pipeline layouts and compute pipelines. `--vk-info` exercises every one and
> **PASSES on the RTX 3060 Ti**. Nothing executes sim work; the only commands
> submitted are the zero-init fills, one empty command buffer, and the upload
> flush.
>
> **The capability record decides phase 7, and the answer is good:**
>
> | cap | value | meaning |
> |---|---|---|
> | `sparseBinding` | YES | |
> | `sparseResidencyBuffer` | YES | |
> | `residencyNonResidentStrict` | **YES** | unbound pages read as ZERO — **sparse is viable**, the dense-fallback branch is not needed on this GPU |
> | `maxStorageBufferRange` | 4294967295 (4 GiB **minus one byte**) | see below |
> | `maxComputeWorkGroupInvocations` | 1024 | |
> | `maxPerStageDescriptorStorageBuffers` | 1048576 | Dawn's 16-per-stage limit is a *Dawn* limit, not hardware |
> | `minUniformBufferOffsetAlignment` | 64 | passUBO's 256 B stride is legal |
> | `timestampQuery` | YES, period 1.0 ns | |
>
> **Two findings that change later phases:**
>
> 1. **`maxStorageBufferRange` is 0xFFFFFFFF — one byte short of the 4 GiB the
>    phase-7 plan asks for.** A single 4 GiB storage binding does NOT fit. The
>    virtual voxels buffer must be capped just under 4 GiB, or split across two
>    bindings. Worth deciding before phase 7 sizes the window, not during.
> 2. **`VK_LAYER_KHRONOS_validation` is NOT INSTALLED** (no Vulkan SDK here by
>    design; the registry holds only three *disabled* implicit overlay layers).
>    So a malformed pipeline or descriptor description does not return an error
>    — it **faults inside the NVIDIA ICD**. That is how the one real bug in this
>    deliverable was found: `worldgen.wgsl`'s `far`/`fardown` entry points bind
>    `@group(1)`, and giving them a one-set pipeline layout produced an access
>    violation inside `vkCreateComputePipelines` rather than a clean failure.
>    **This is a blocker for 3b**, which generates barriers and which the barrier
>    document expects synchronization validation to police. Installing the
>    validation layer is a prerequisite for trusting a green 3b.
>
> Also noted from the record: `maxPerStageDescriptorStorageBuffers` = 1048576
> confirms `simSlimBGL_` exists purely for Dawn's layout limit and could collapse
> on Vulkan — a separate, hash-gated change, explicitly out of scope for v1
> (barrier_graph §4.10).

> **[AS BUILT] Phase 3b deliverable 1 — recording with GENERATED barriers.**
> `src/gpu/vk_record.{h,cpp}` is the last-access tracker of barrier_graph §3.3,
> implemented verbatim: per-buffer `{lastWriteStage, lastWriteAccess,
> readStagesSince, readAccessSince}`, WAR falling out of the write branch
> folding the accumulated reads into its own source scope, read-after-read
> emitting nothing, §3.4's head-of-command-buffer global barrier, §3.6's global
> form on the CA repeat span, §3.7's indirect handling falling out of the
> tracker rather than being special-cased, and §2.4 phase 7b's host-read barrier
> emitted at `Finish()` instead of at a table index. `--barriers=sledgehammer`
> is §6.2's A/B oracle. **No barrier is written at a call site**; the one
> off-table recording path (`CopyToHost`, the blocking hash read) expresses its
> source hazard as a `pass::Use` against the same tracker.
>
> **How the PassRow R/W sets reach the tracker — the seam this phase adds.**
> The rejected option was widening `rhi::`'s wgpu-shaped encoder with a
> `DeclareUses()` that only one backend honours: an API where forgetting a call
> silently removes barriers is the wrong shape for the one thing rule 1 depends
> on. Instead the Vulkan backend walks the SAME `pass::kRows` itself
> (`Recorder::RecordTable`), so the row is not a parameter that can be omitted —
> it is the loop variable, and every command the recorder can issue is reachable
> only from a row. Two walkers (Dawn's in `simulation.cpp`, Vulkan's in
> `vk_record.cpp`) read one table; `check_pass_table.py` plus cross-backend hash
> equality is what proves they agree.
>
> **Three bugs found en route, two of them the kind that only faults:**
>
> 1. **`vkCmdPipelineBarrier2` needs `synchronization2` ENABLED, not just a 1.3
>    device.** Promotion to core makes the entry point *resolve*; it does not
>    make the call legal. `CreateLogicalDevice` now queries
>    `VkPhysicalDeviceVulkan13Features`, enables it explicitly, and REFUSES to
>    initialise without it — a down-converted 1.0-barrier fallback would mean a
>    silently weaker barrier, which is the exact failure rule 1 cannot absorb.
> 2. **`CreateDescriptorSet` hardcoded `STORAGE_BUFFER` for every write.**
>    Phase 3a never noticed because `--vk-info` created no descriptor *sets*.
>    Every uniform binding — including `passUBO`, the dynamic one — was being
>    written with the wrong descriptor type, which is undefined behaviour that
>    corrupts the set and faults later in a dispatch. The type now comes from the
>    layout, matched **by binding number** rather than array position, because
>    the two arrays are written independently at each call site.
> 3. **`passUBO` must be bound with a 16-byte range, not the whole buffer.**
>    With a dynamic offset, `offset + range` must stay in bounds, so binding all
>    13.5 KiB puts every k > 0 past the end. Dawn's binding already used the
>    16-byte window; reproducing it was necessary, not cosmetic.
>
> **[AS BUILT] Phase 3b deliverable 2 — `--backend vulkan`, headless.**
> `src/gpu/vk_sim.{h,cpp}` builds the sim's buffers, layouts, descriptor sets
> and 19 compute pipelines against `vk::Backend` from the same descriptions
> `World::Init`/`Simulation::Init` use, and drives the recorded paths. It is a
> second set of resource DECLARATIONS rather than a second backend behind
> `rhi::`, because `rhi::` handles hold `shared_ptr<XImpl>` with every impl
> defined as a `wgpu::` holder — a second implementation needs virtual dispatch
> through the backend that is currently the port's only hash oracle, for no
> phase-3b benefit. What is duplicated is the resource description; what decides
> the world hash — the table — is shared. `--backend vulkan` without a headless
> mode is **refused with a message**, never quietly served by Dawn: a run
> reported as Vulkan that was Dawn all along is worse than no run.
>
> **[AS BUILT] Phase 3b deliverable 3 — `--vk-smoke`, and the port's first
> determinism evidence. Checkpoint 2 (cross-backend hash equality) is MET.**
>
> ```
> === sandvox --vk-smoke (Vulkan port phase 3b) ===
> mode: barriers=precise validation=ON adapter=default seed=1337 ticks=50
> loaded 97 materials, 74 reactions
> adapter: NVIDIA GeForce RTX 3060 Ti (backend 6)
> Vulkan device: NVIDIA GeForce RTX 3060 Ti
>   validation layer: ENABLED   sync validation: ENABLED
>   tick recording: 11 rows, 59 dispatches, 2 copies, 3 fills, 63 barrier calls (15 buffer + 55 global)
>
> === validation ===
>   ZERO messages (no synchronization hazards reported)
>
> === hashes ===
>   stage              Dawn         Vulkan
>   worldgen           f97ba745     f97ba745     MATCH
>   tick 1             d5c8944c     d5c8944c     MATCH
>   tick 15            f153ce74     f153ce74     MATCH
>   tick 30            434268e6     434268e6     MATCH
>   tick 50            b3c643a2     b3c643a2     MATCH
>
> === --vk-smoke PASS ===
> ```
>
> `b3c643a2` at tick 50 is the same settled hash this document's measured
> baseline recorded independently, which is a small extra corroboration.
> The 55 global barriers are 1 head + 54 CA iterations; the 53 inter-iteration
> ones ARE the colour lattice (§7.1), not a cache-flush detail.
>
> **Validation is now live and it changes the evidence quality.** The LunarG SDK
> was installed mid-phase, so `VK_LAYER_KHRONOS_validation` enumerates and
> synchronization validation runs. The smoke passes with **zero** messages,
> which is §6.2's *primary* detector reporting clean — materially stronger than
> the hash match alone. Layer selection was hardened at the same time: the SDK
> registered eight explicit layers, and instance creation requests
> `khronos_validation` by exact name only, never whatever enumerates.
>
> **The sledgehammer A/B agrees and that is WEAK evidence, as designed.**
> `--barriers=sledgehammer` (65 barrier calls, all global) produces byte-identical
> hashes. Per §6.2 that is *exoneration* — it says the barrier graph is not the
> cause of anything — and specifically NOT a verification that the precise
> barriers are right, because this hardware serialises back-to-back identical
> dispatches regardless. Reported as such rather than as a second PASS.
>
> **Still owed before phase 3 can claim completeness** (barrier_graph §8): the
> readback ring and streaming on Vulkan (3c), and tables for the readback copies
> (§2.4 phase 7a/7b) and eviction copies (§4.3). The quiet-world smoke exercises
> every structural feature of the tick table except the particle and explosion
> chains, which need ops to reach.

> **[AS BUILT] Phase 3c deliverables 1+2 — the rest of the per-tick machinery,
> and streaming.** `vk::SimBackend` grew the readback ring (3 slots, the
> `World::Init` layout verbatim), `SubmitTickFull` (the full input set — ops,
> explosions, cells, spawns, far-fill, readback — i.e. `test/support.cpp`'s
> `SubmitTick` against Vulkan resources), `PollReadbacks` (`ProcessEvents`'
> replacement, at the same frame point), `WakeAll`, `EvictSlots`/`CompleteEvict`,
> `FillSlotFromStore`, `FillSlotsByGen`, `SubmitLoadReset` and
> `ReadBufferBlocking`.
>
> **The readback and eviction copies are Uses, NOT table rows — and that is a
> decision, not a shortcut.** A `pass::Row` encodes a Copy's offsets as the
> literal constants `x/y/z`, which is exact for every copy in the tick table (the
> indirect-args hops are always 12 B at offset 0 or 16). It cannot express
> `EncodeReadbacks`: up to 64 chunk fetches at slot indices chosen at runtime
> from a queue, 27 mirror copies whose offsets come from the live window origin,
> into a slot picked from a 3-deep ring. Their count and offsets are *tick data*.
> Making the schema carry runtime-parameterised offsets would make a row a
> closure and dissolve the property that lets `check_pass_table.py` read the
> `.def` as static text. So they express their hazards as `pass::Use` against the
> same tracker, which is the mechanism `CopyToHost` established in 3b and which
> §8 already sanctions ("expressed as a table row's `uses`" — a Use, not
> necessarily a Row). Two new recorder entry points carry it: `CopyTracked`
> (source is a `pass::Buf`, so the RAW against its last writer is derived) and
> `FillTracked` (the `ClearBuffer(support)` WAW of §7.4, which falls out because
> the fill declares a TransferWrite on a buffer the tracker just saw read).
> `pass_table.def` is therefore UNCHANGED by 3c, and the checker stays silent.
>
> **One real bug, and it was in the fence pool.** §4.2 says a readback slot
> "borrows a reference to that submit's fence". `Backend::PollFences` recycled a
> signalled fence into the free list immediately, and `BeginCommands` calls
> `PollFences` on *every* command buffer — so a slot that submitted at tick N and
> had not yet been polled held a handle `AcquireFence` had already reset and
> handed to the tick-N+1 submit. `vkGetFenceStatus` on it then reports a
> different submit's status, and the slot reads its mapped memory while the GPU
> is still writing it: silent CPU-mirror corruption, no crash. Fixed with
> `RetainFence`/`ReleaseFence` refcounts — a retained fence is parked rather than
> pooled when its submit retires, and returns to the pool on the last release.
> The borrowed-fence model in §4.2 is correct; what it needed was a retain.
>
> **`EncodeWakeAll` needed no special case,** exactly as §4.1 predicted: it is a
> `QueueWrite` that drains at the head of the tick's command buffer ahead of the
> first row. The `stream.cpp:172` eviction-ordering comment was corrected in the
> same commit per §4.3 step 4 — the guarantee is not "both are submits and
> submits are ordered" but "EvictSlots submits eagerly while FillSlots only
> enqueues", and `kPersistMask` moved from a stream.cpp file-static to `stream.h`
> because the smoke now has to reproduce the store round-trip exactly.
>
> **[AS BUILT] Phase 3c deliverable 4 — `--vk-smoke-loud`, the port's
> determinism acceptance evidence for phase 3.** 120 ticks of an ACTIVE world on
> both backends: brush + melt ops, three explosions (the mark/apply split, the
> expMask, the whole spawn/integrate/resolve particle chain), exact-cell ops, the
> readback ring live every tick, and an 8-shift streaming walk with eviction and
> procgen refill. Hashes compared at 19 points throughout, validation ON.
>
> ```
> === validation ===
>   ZERO messages (no synchronization hazards reported)
>
> === streaming ===
>   Dawn:   8 window shifts, 34059 chunks in store
>   Vulkan: 8 window shifts, 8192 chunks evicted, 64 store-hit refills, 8192 procgen refills
>   store-hit refill self-check: PASS (100.00% of the plane restored through
>       deferred, submit-less writes flushed by the next tick)
>
>   worldgen f97ba745   f97ba745   MATCH      t60      f4fd73c6   f4fd73c6   MATCH
>   t15      958d2cd1   958d2cd1   MATCH      t75      3c954bbf   3c954bbf   MATCH
>   t30      9d6c5841   9d6c5841   MATCH      t76      20fd330a   20fd330a   MATCH
>   t45      896e2082   896e2082   MATCH      t84      95a876da   95a876da   MATCH
>   t46      5436693c   5436693c   MATCH      t85      4850717a   4850717a   MATCH
>   t47      22ec46d9   22ec46d9   MATCH      t86      38802cbb   38802cbb   MATCH
>   t52      c50f2236   c50f2236   MATCH      t87      250cd625   250cd625   MATCH
>   t53      663bc868   663bc868   MATCH      t88      1a9022a2   1a9022a2   MATCH
>   t90      2fe6536b   2fe6536b   MATCH      t105     16c239c7   16c239c7   MATCH
>   t120     cb036bd1   cb036bd1   MATCH
>
> === --vk-smoke-loud PASS ===
> ```
>
> An active tick records **20 rows, 64 dispatches, 38 copies, 5 fills, 104
> barrier calls (67 buffer + 55 global)** against the quiet tick's 11/59/2/3/63 —
> the difference is the conditional chains plus the readback ring's copies.
> `--barriers=sledgehammer` produces identical hashes (exoneration per §6.2, and
> still weak evidence on one GPU).
>
> **What the streaming leg deliberately does NOT compare, and why that is not a
> gap being papered over.** The walk is one-directional. A return leg re-enters
> evicted planes, which is the only route to the store-hit refill — but the
> store-hit path's *content* comes from a store, and the two backends cannot
> share one: Dawn drives the real `Stream` (sticky `modified_` set, `dropIfAir =
> modified_[s] == 0`, RLE through the region-file `ChunkStore`, force-completion
> of in-flight evictions), while the Vulkan side has no `Stream` at all — the
> same `rhi::`-ownership reason `vk_sim.h` exists. Any store the smoke emulates
> makes different refill decisions, so the worlds diverge in CONTENT.
>
> This was confirmed rather than assumed, and the confirmation is the useful
> part: with a return leg, **both backends were bit-stable run to run** (Dawn
> `c4c5178f`, Vulkan `2879f83e`, reproducing exactly) and diverged only after the
> reversal. A barrier race varies between runs; a policy difference does not.
> Three different emulated store policies each matched through the entire
> outbound leg and each diverged on the return. Two of those attempts were
> themselves instructive: storing raw evicted words (rather than applying
> `kPersistMask` + re-stamping `kStampNever`) and dropping all-air chunks
> unconditionally (rather than Dawn's `modified_`-gated `dropIfAir`) each produced
> a divergence with the exact signature of a barrier race and neither was one.
> So the store-hit path is proven by a direct Vulkan-side round-trip assertion
> instead — evict a plane, clobber those slots with procgen from a bogus origin,
> refill from the captured words through the deferred submit-less path, run one
> ordinary tick, and re-evict. It restores 100%, and step 4 is the load-bearing
> one: a model where uploads ride their own submit passes the first three steps
> and fails there.

> **[AS BUILT] Phase 3c deliverable 3 — `--selftest --backend vulkan` is
> REFUSED, and this is the deliverable's honest outcome rather than a gap.**
>
> The brief asked for the gates that do not render to run on Vulkan, with a
> per-gate flag or a curated skip list as the mechanism. Neither mechanism is the
> blocker, and discovering that is the result: **the gates never touch a
> backend.** They drive `World`, `Simulation`, `Stream`, `Physics` and
> `MobSystem`, and every GPU resource those own is an `rhi::` handle. `rhi::` has
> exactly one implementation — `rhi_dawn.cpp`, where every impl struct is a
> `wgpu::` holder. A second implementation of the same handle types cannot
> coexist in one binary without making all ~50 of those methods virtual, which is
> a restructure of the backend that is currently the port's only hash oracle.
> Phase 2a declined that deliberately and phase 3 did not revisit it:
> `vk::SimBackend` is a parallel set of resource DECLARATIONS driven by the
> shared pass table, explicitly *not* an `rhi::` backend (`vk_sim.h` says so).
>
> There is therefore no way to hand a gate a Vulkan `World`, and the three
> available responses were: run on Dawn and print "backend: vulkan" (a lie, and
> exactly what 3b's refusal exists to prevent); skip every gate and report a
> green run of nothing; or refuse and say what is missing. **The flag refuses,
> with the reason and the alternative in the message.**
>
> What phase 3c ships in its place is `--vk-smoke-loud`, which drives the same
> tick chain the gates drive and compares hashes against Dawn at 19 points. That
> is the determinism property `--selftest --backend vulkan` was wanted for; it is
> simply not spelled `--selftest`.
>
> **What DID land from this deliverable: `Gate::needsRender`, declared per gate
> and printed by `--list`.** Three of the 23 gates need the render path —
> `screenshots` (the only one in the render group that actually draws; `far-fog`
> and `far-downsample` exercise the cascades through compute and a one-word
> readback), `mob` (the 14-angle micro-body view sweep), and `perf` (no draw of
> its own, but its verdict reads `bestFrameMs`, which only `screenshots` sets).
> The other 20 are compute + readback only. That is the list phase 4/5 needs the
> moment `rhi::` can carry two backends, and it is now a property of each gate
> rather than a curated list in a commit message that goes stale the first time a
> gate learns to draw. Nothing consumes it to skip a gate yet, and the header
> comment says so.

Device init (require timestamp queries; report sparse + strict-residency caps),
VMA allocation, WGSL→SPIR-V via Tint, descriptor sets, command recording with
barriers generated from the pass table, indirect dispatch (keep the staging
copies verbatim in v1), upload path with correct queue ordering, readback ring
on real fences, `vkCmdFillBuffer` zero-init for all buffers at creation,
`--backend vulkan` flag (headless only).
*Checkpoints, in order, each green before the next:*
1. `--selftest --gate determinism --backend vulkan` passes.
2. **Cross-backend hash equality**: same seed/ticks, Dawn vs Vulkan, identical
   world-hash sequence (scripted via `--selftest --json`).
3. All compute-only gates green on Vulkan; default Dawn build still fully green.

**[AS BUILT] Checkpoints 1 and 3 were written on a false premise and are
superseded.** Both assume `--selftest` can be pointed at a backend. It cannot:
the gates hold `rhi::` handles and `rhi::` has one implementation, so there is
no Vulkan `World` to give them (phase 3c deliverable 3 below). What actually
discharges the intent behind them is checkpoint 2, widened: `--vk-smoke` (quiet,
3b) and `--vk-smoke-loud` (active, 3c) run the same tick chain on both backends
and compare world hashes — 5 probes over a settled world, 19 over an active one
with ops, explosions, particles, the readback ring and streaming. The Dawn
selftest stays fully green throughout, which is the other half of checkpoint 3
and is unaffected. Running the gate BODIES on Vulkan requires making `rhi::`
polymorphic and is folded into phase 4/5.

**Phase 4 — Vulkan render path.**
Swapchain, the 6 raster pipelines, reversed-Z depth, `imgui_impl_vulkan`,
screenshot/offscreen paths.
*Checkpoint: full 20-gate selftest green on `--backend vulkan`; screenshots
visually equivalent; render ms reported vs Dawn baseline.*

> **[AS BUILT] Phase 4b D1 — offscreen render on Vulkan (2026-08-22).**
> `CreateTexture`/`CreateRenderPipeline`/`BeginRenderPass`/`CopyTextureToBuffer`
> are real on the Vulkan seam; `--shot --backend vulkan` and
> `--shot-mob --backend vulkan` run end to end.
>
> * **Dynamic rendering, not render-pass objects.** `vkCmdBeginRendering`
>   matches WebGPU's model 1:1 (no framebuffer/render-pass caching keyed by
>   attachment combos), the barrier doc §1.2 already assumed it, and both
>   `dynamicRendering` and `synchronization2` are MANDATORY core-1.3 features —
>   the backend already refuses devices without the latter, so requiring the
>   former adds no reachable hardware constraint. Both are probed and enabled
>   explicitly (a core feature still needs enabling; the 3b lesson).
> * **WebGPU coordinates via negative-height viewport + CCW front face** — the
>   same pairing Dawn's own Vulkan backend uses. Framebuffer-space geometry and
>   winding match, so screenshots are byte-comparable against Dawn.
> * **Render barriers are generated, never hand-placed** (§2.6/§3.2 as-built
>   notes): `Recorder::BeginRendering` flushes the tracker into the
>   render-read domain before the scope opens (barriers are illegal inside);
>   image layout transitions are derived from `vk::Image::layout` + a
>   per-recording access state; the screenshot copy transitions
>   attachment→TRANSFER_SRC the same way. Sledgehammer mode substitutes its
>   full barrier at the same points, keeping the A/B honest.
> * **Checkpoint evidence:** all 28 `--shot` screenshots wrote on Vulkan with
>   sync validation ON; pixel-diff vs the same-build Dawn set: **23/28
>   byte-identical, the rest differ on <0.001% of pixels with max channel
>   delta 1/255** (raymarch is float; §3.2's "bytes may differ" allowance was
>   barely needed). Validation messages are now REPORTED (and fail the run) at
>   the end of --shot/--shot-mob — the messenger collects continuously but
>   nothing popped a scope in these harnesses before, so a hazard could have
>   gone unprinted.
>
> **[AS BUILT] Phase 4b D3 — windowed Vulkan (2026-08-22). Phase 4b complete.**
> Swapchain (FIFO, matching Dawn's present mode), resize, AcquireFrame/Present
> through the seam, and imgui_impl_vulkan behind the same Overlay interface.
>
> * **Semaphores stay exactly where §5.1 allows them**: per-image render-done
>   semaphores plus a fence-paced ring of 3 acquire semaphores (a semaphore
>   handed to vkAcquireNextImageKHR must be provably idle; the fence of the
>   submit that consumed it is the proof — the same Retain/Release discipline
>   as the readback ring). The presenting submit is detected by the encoder
>   (its render pass targeted a `presentable` image), not declared by callers.
> * **PRESENT_SRC transitions are derived like every other image barrier**:
>   `Recorder::Finish()` transitions any presentable image touched in the
>   recording, src = its tracked attachment write.
> * **ImGui**: `imgui_impl_vulkan` (1.92.4) with dynamic rendering, entry
>   points fed from the engine's own loader (`IMGUI_IMPL_VULKAN_NO_PROTOTYPES`
>   — one source of Vulkan entry points, by construction). overlay.cpp is now
>   the sanctioned dual-native exception; `rhi::dawn::Native` and
>   `rhi::vkr::NativeCmd` are its two doors.
> * **`--frames N` harness**: runs the windowed game N frames, fires one F5
>   shader reload midway (the Tint recompile path), exits cleanly, and prints
>   collected validation messages. Verified: window opens, the world renders
>   (a window capture shows the raymarched world, grass strands, ImGui panel +
>   HUD at 98 fps), `reloading shaders... ok`, clean exit,
>   `session: vulkan validation messages: 0 (clean)` over 1500 frames.
>   Windowed avg render+present 10.6–12.4 ms under FIFO on both backends; the
>   honest render-cost comparison is the selftest 1080p sweep — 17.4 ms
>   shadows-on / 12.4 off (Vulkan) vs 18.8 / 12.8 (Dawn).
>
> **[AS BUILT] Phase 4b D2 — all 23 gates run on both backends (2026-08-22).**
> The `needsRender` skip is gone (the flag survives as documentation in
> `--list`), the shared offscreen target is created on both backends, and the
> debris gate's diagnostic screenshot is un-gated. `--selftest --backend
> vulkan --vk-validation`: **23 gates, exit 0, determinism gate reports the
> pinned `7cfa2420`, `vulkan validation messages: 0 (clean)`**, and the
> pass/fail set equals Dawn's exactly — pond-freeze and mob known-failing,
> with the mob gate's two failing sub-checks (`micro body render: FAIL (10
> micro slots, 7/14 views drew, ...)` and `avatar: FAIL (... upright=0 ...)`)
> printing CHARACTER-FOR-CHARACTER identical measured values on both
> backends. The selftest harness now prints (and fails the run on) any
> collected Vulkan validation message — a sync-validation hazard is a barrier
> bug even when every gate is green.
>
> **One real barrier bug found and fixed by this deliverable** (the reason the
> harness reports validation at all): a WRITE_AFTER_WRITE across submits
> between a queued creation zero-fill and a later Class B data copy — the
> upload flush ran ahead of the §3.4 head barrier and nothing ordered it
> against previous submits. §4.1's phase-4b as-built note has the verbatim
> message and the mechanical fix (one memory barrier at the head of any
> non-empty flush). The micro-body pool was observably losing limbs to its own
> creation fill on this GPU.
>
> `--vk-smoke-loud` re-run after the render work: 19/19 MATCH, ZERO messages,
> hashes byte-identical to the phase-3c record (worldgen f97ba745 … t120
> cb036bd1). Render perf while here: selftest 1080p sweep is 17.4 ms/frame
> shadows-on / 12.4 shadows-off on Vulkan vs 18.8 / 12.8 on Dawn.
>
> **[AS BUILT] Phase 4a — runtime-selectable backends; the real gates run on
> Vulkan (2026-08-22).** Split off from phase 4: everything except the render
> path itself.
>
> **The seam design: abstract impl bases, chosen over handle+vtable and
> variant.** Every impl struct behind the rhi.h handles is now an abstract base
> (`rhi_impl.h`) with two subclasses — `rhi_dawn.cpp` (the same wgpu calls, one
> virtual hop; recording byte-identical, proven by the pinned hash) and
> `rhi_vk.cpp` (translating onto `vk::Backend`). Virtual dispatch was picked
> for clarity and phase-7 headroom: a sparse-resident voxels buffer is just
> another `BufferImpl` subclass, and phase 6's Dawn removal deletes subclasses
> without touching a caller. Dispatch cost is irrelevant at ~60 dispatches +
> a handful of copies per tick.
>
> **Barrier generation kept the phase-3b shape exactly.** The Vulkan encoder
> does NOT derive barriers from the wgpu-shaped calls: `Simulation::RecordTable`
> branches on `device.Kind()` and hands the page-resolved ids/pipelines/sets
> across a bridge (`rhi_record.h`) to the encoder's `vk::Recorder`, which walks
> `pass::kRows` itself. What was deleted is `vk_sim.{h,cpp}` — the phase-3b/3c
> parallel copy of every resource declaration — so there is ONE World again.
> Off-table copies go through new `CommandEncoder::CopyTracked/FillTracked`
> (a `pass::Buf` id + the concrete buffer; under Dawn byte-identical to the
> plain calls), and buffers with no id (staging, the timer resolve) are tracked
> BY POINTER in a recorder side table — still derived, never hand-placed.
> `pass_table.def` unchanged; `check_pass_table.py` silent.
>
> **The readback ring, chunk-fetch queue and eviction tickets are World's/
> Stream's own code on both backends** (closing 3c's "fetchIds has no Vulkan
> consumer" gap by deletion): `rhi::MapReadAsync` on Vulkan borrows the
> producing submit's fence (RetainFence) over the persistently mapped slot and
> fires from `ProcessEvents`; `MapTicket` is the same borrow consumed by
> poll/wait. Buffer handles are refcounted like wgpu's — released staging is
> freed through a serial-stamped graveyard once in-flight submits retire
> (a whole-world gate read is 512 MiB; leaking those to Shutdown was not an
> option).
>
> **Two real bugs, both found by sync validation + hash divergence, both in
> the new queued zero-init** (barrier_graph §4.1/§4.8 phase-4a notes):
> intra-flush WAW (a creation fill racing the first data upload — the material
> table lost and the world froze inert), and the Class B staging ring's own
> zero-fill wiping freshly host-staged payloads (MapWrite staging is now never
> GPU-zeroed). Neither existed in 3c because zero-init was one early submit.
>
> **`--selftest --backend vulkan` runs 20 of 23 gate bodies for real.** 19
> PASS; pond-freeze FAILs with the character-for-character identical assertion
> and measured values as Dawn ("rim 0/96 vs middle 0/25 ice at 250 night
> ticks; 0 ice voxels, 0 frozen with 0 non-water neighbours") — a known-fail
> failing the SAME WAY cross-backend. The determinism gate reports the pinned
> golden hash ON VULKAN: `final hash 7cfa2420 over 200 ticks, matches
> baseline`. save/load round-trips a real `.svd` (247105c6 → cd6022f6 →
> restored 247105c6, values identical to Dawn), streaming matches Dawn's hash
> sequence over 232 shifts (store 36013 chunks, identical). The three
> `needsRender` gates (screenshots, mob, perf) print
> `SKIP (needs the render path; not available on --backend vulkan until phase
> 4b)` — never silently pass; the Vulkan encoder ABORTS on any render entry
> point, which is how the debris gate's Dawn-only diagnostic screenshot (a
> draw appended after its compute-only verdict) was found and gated.
>
> **The smokes now drive both backends through the identical driver** (real
> `Stream` + `ChunkStore` on the Vulkan side, previously impossible):
> `--vk-smoke` 5/5 MATCH, `--vk-smoke-loud` 19/19 MATCH with validation ON and
> ZERO messages, hashes byte-identical to the phase-3c record (worldgen
> f97ba745 … t120 cb036bd1) — the folded path reproduces the 3c world
> bit-for-bit, and even the 34059-chunk stores match.
>
> **D4 — `--measure --backend vulkan` works, and the idle-overhead claim has
> its first cross-API data point** (same machine, same scenarios as the phase-0
> baseline; Vulkan timestamps via `VkQueryPool` + `vkCmdWriteTimestamp2` around
> the recorder's group transitions, the same spans Dawn's per-pass writes
> cover):
>
> | pass (settled world) | Dawn phase 0 | Vulkan 4a |
> |---|---|---|
> | prep (mutate+explode+compact) | 7.1 µs | 2.6 µs |
> | CA (54 empty indirect dispatches) | **184.6 µs** | **119.9 µs** |
> | compactNext | 3.8 µs | 2.7 µs |
> | occupancyDirty+pick | 9.8 µs | 4.1 µs |
> | farDown | 5.5 µs | 2.2 µs |
> | occupancyFull+pick (1-in-15) | 95.6 µs | 97.8 µs |
> | **settled tick total** | **306 µs** | **229 µs** |
>
> The settled tick is ~25% cheaper on Vulkan with identical work: the empty CA
> dispatches drop from ~3.4 to ~2.2 µs each (driver overhead, exactly the
> rule-2 violation phase 0 flagged), while the genuinely GPU-bound full-world
> occupancy scan is unchanged (95.6 vs 97.8 µs) — the port's perf case is
> idle overhead, and the remaining ~120 µs of empty dispatches is still the
> phase-8 skip-encode target. Active world: 501 µs (Vulkan) vs 565 µs (Dawn).

**Phase 5 — second-adapter validation.**
Run the Vulkan selftest on every enumerable device (`--adapter low` equivalent;
this machine may expose only the 3060 Ti — if so, evaluate lavapipe/SwiftShader
as a deterministic-CPU cross-check for the hash sequence). Progress on
DESIGN.md risk #3, not closure, unless a second vendor is actually present.

> **[AS BUILT] Phase 5 — one vendor is all this machine has, and the honest
> outcome is a documented option list rather than a closed risk (2026-08-22).**
>
> **What was enumerated, three independent ways.** `--vk-info` (default) and
> `--vk-info --adapter low` both select `NVIDIA GeForce RTX 3060 Ti`; the Dawn
> path prints the same adapter for `--backend dawn` and `--backend dawn
> --adapter low`; and the SDK's own `vulkaninfo --summary` — which enumerates
> without any of our selection logic — reports exactly `GPU0`, no GPU1. The
> `--adapter low` scoring in `Backend::PickPhysicalDevice` is working; there is
> simply nothing for it to prefer. **The CPU is an i7-11700F** — the `F` suffix
> is Intel's "no integrated graphics" part — so there is no iGPU to enable in
> BIOS on this machine, and that option is not merely unused, it is unavailable.
>
> **What WAS validated: full 23-gate parity on the one vendor, both backends.**
>
> ```
> $ sandvox --selftest --backend dawn --json p5_dawn.json
> adapter: NVIDIA GeForce RTX 3060 Ti (backend 6)
> === selftest === (23 gates, backend dawn)
> determinism: PASS (final hash 7cfa2420 over 200 ticks, matches baseline)
> known-failing at baseline (not yours): pond-freeze, mob
> === selftest PASS === (2 known failures carried)          exit 0
>
> $ sandvox --selftest --backend vulkan --vk-validation --json p5_vk.json
> adapter: NVIDIA GeForce RTX 3060 Ti (backend vulkan)
> === selftest === (23 gates, backend vulkan)
> determinism: PASS (final hash 7cfa2420 over 200 ticks, matches baseline)
> known-failing at baseline (not yours): pond-freeze, mob
> selftest: vulkan validation messages: 0 (clean)
> === selftest PASS === (2 known failures carried)          exit 0
> ```
>
> Diffing the two `--json` files gate-by-gate: 23 gates each, identical name
> set, **zero status differences**, and **zero differences in the reported
> detail strings** except `perf`, which reports wall-clock (`sim 2.11 ms/tick,
> best frame 13.21 ms` on Dawn vs `2.06 / 12.57` on Vulkan). Every gate that
> reports a measured value — hashes, voxel counts, pose angles, the two known
> failures' assertion text — reports it character-for-character identically
> across the two backends.
>
> **What that is worth, stated precisely.** It is a strong test of *this port*
> and a weak test of *rule 1*. Two different API host layers, two different
> barrier regimes (Dawn's auto-generated vs. this port's table-generated), two
> different SPIR-V producers reaching the driver — agreeing bit-for-bit on 23
> gates. What it does not vary is the thing risk #3 is actually about: **one
> vendor, one driver, one shader compiler back end**. An FMA-contraction or
> transcendental divergence lives below both of our host layers and would agree
> with itself here just as happily.
>
> **Options for closing risk #3, for the user to choose.** None were installed —
> the brief forbids installing software, and each has a real cost worth deciding
> deliberately:
>
> * **(a) A CPU Vulkan implementation — lavapipe (Mesa) or SwiftShader.** The
>   strongest available cross-check short of second hardware, and legitimately
>   so: these are *different compilers* (LLVM/Subzero) emitting for a *different
>   ISA* (x86 SIMD), so an integer sim kernel that divergently lowers on NVIDIA's
>   compiler has a genuine chance of lowering differently there. Wiring is
>   binary-only and needs no code change: drop the ICD and set
>   `VK_ICD_FILENAMES=<path>\lvp_icd.x86_64.json` (or SwiftShader's
>   `vk_swiftshader_icd.json`) — our loader is a plain `vulkan-1.dll` walk, so it
>   picks the override up. Cost: slow. The 23-gate suite is ~50 s on a 3060 Ti;
>   on lavapipe expect a large multiple, so the realistic scope is the
>   determinism gate plus `--vk-smoke`/`--vk-smoke-loud`, which is exactly the
>   hash *sequence* risk #3 wants. Caveat to record if we do it: a CPU ICD is
>   evidence about the *shader compiler*, not about GPU scheduling — it cannot
>   surface a race that only a real warp scheduler exposes, which is §7.1's
>   failure mode.
> * **(b) Run the suite on a second physical machine.** The only option that
>   varies the vendor for real (AMD or Intel silicon, ideally). Already
>   scriptable with no new code: `--selftest --json out.json` produces the
>   machine-readable per-gate record this note just diffed, and a `.svd` save
>   round-trips a world for byte-comparison. This is the option that actually
>   closes risk #3; the others are progress toward it.
> * **(c) Enable an iGPU in BIOS.** **Ruled out on this machine** — the
>   i7-11700F has no integrated graphics die. Listed only so the next reader does
>   not spend an evening in the BIOS looking for it.
>
> Risk #3 therefore stays **open**, with its DESIGN.md §14 text updated in the
> same commit to say what is now true: the hash is pinned and reproduced across
> two independent host layers on one vendor, and a second vendor's hash sequence
> is what remains.

**Phase 6 — switch default; update docs.**
Default backend → Vulkan. CLAUDE.md build/verify sections updated in the same
commit (DESIGN.md §12 was already updated when the plan was adopted). ~~Dawn is
retained through phase 7 purely as the cross-backend hash oracle~~ —
**SUPERSEDED 2026-08-22: the user relaxed cross-backend equality and Dawn was
removed immediately, before phase 7 rather than after it. See the DAWN REMOVAL
as-built note below.** The seam itself stays, as this paragraph always said: it
is what made the port testable and it costs nothing to keep.

> **[AS BUILT] Phase 6 — Vulkan is the default (2026-08-22). Phase 6 complete.**
>
> **The flip was small, and that is phase 4a's dividend rather than luck.** The
> backend is chosen in exactly one place. What needed changing was the *shape* of
> the variable holding it: `main.cpp` carried `bool backendVulkan = false`, a
> flag named for the non-default, which cannot express "default Vulkan" without
> reading backwards. It is now `rhi::BackendKind backend =
> rhi::BackendKind::Vulkan`, and the ternary at the `GpuContext::Init` call site
> is gone. Both `--backend dawn` and `--backend vulkan` stay accepted, so no
> existing invocation or script changes meaning.
>
> **Nothing else genuinely needed plumbing, including the two places that looked
> like they would.** There is no separate "selftest default": the harness prints
> and runs against `ctx.backendKind`, so it followed the flip for free. The two
> `BackendKind::Dawn` defaults in `context.h` (the `Init` default argument and
> the field initialiser) were flipped for consistency, but both call sites pass
> the argument explicitly — they were latent traps, not live behaviour. Recording,
> the pass table, barrier generation and buffer creation are untouched, as the
> brief required.
>
> **Checkpoint evidence, all on an mtime-verified exe:**
>
> | check | result |
> |---|---|
> | `--selftest` (no flag) | 23 gates, **exit 0**, `backend vulkan`, `determinism: PASS (final hash 7cfa2420 over 200 ticks, matches baseline)`, pond-freeze + mob known-failing |
> | `--selftest --backend dawn` | 23 gates, **exit 0**, `backend dawn`, same `7cfa2420`, same two known failures |
> | gate-by-gate `--json` diff | identical name set, **zero status differences, zero detail-string differences** except `perf` (wall clock: 2.10 vs 2.16 ms/tick) |
> | `--vk-smoke-loud --vk-validation` | **19/19 MATCH**, ZERO validation messages, hashes byte-identical to the phase-3c record (worldgen `f97ba745` … t120 `cb036bd1`) |
> | `check_pass_table.py` | `pass table OK` |
> | `check_invariants.py` | `invariants OK` |
> | `--frames 400 --vk-validation` | windowed default is Vulkan, F5 reload `ok`, clean exit, `session: vulkan validation messages: 0 (clean)` |
> | `--frames 120 --backend dawn` | windowed Dawn unaffected |
>
> **`tests/baseline.json` is unchanged, and that is the point.** The golden hash
> is backend-independent — it was already `7cfa2420` on both — so a default flip
> has nothing to flip. If a gate ever behaves differently under a backend change,
> the baseline is the wrong place to absorb it.
>
> **A correction to how the render numbers in the 4a/4b notes should be read.**
> The sim comparison is solid — `--measure` GPU timestamps put a settled tick at
> 229–236 µs on Vulkan vs 306 on Dawn, reproduced here. The *render* sweep is
> not: the selftest's 1080p figure moved between 8.0 and 18.8 ms/frame across
> runs of the same binary in this phase, depending on machine load and on what
> world state earlier gates left behind (the perf gate in isolation reports
> ~8 ms; inside a full run, ~17–19), and across those runs the two backends
> traded places. The 4b note's "17.4/12.4 Vulkan vs 18.8/12.8 Dawn" is one
> sample from inside that spread, not a measured advantage. **Do not quote a
> render delta from a single run**; if the render cost ever matters, it needs a
> harness that pins the scene and repeats, which `--measure` does for compute
> and nothing yet does for the frame.
>
> **D3 — the docs.** CLAUDE.md gained a *"Two backends: Vulkan runs, Dawn
> judges"* section in build/verify: default is Vulkan, `--backend dawn` is the
> ORACLE rather than a fallback, `--vk-validation`/`--frames` exist, the
> sync-validation-fails-the-run policy and *why* it must not be weakened,
> `SANDVOX_NO_CRASH_DIALOG=1` + `crash.log`, and verify-the-exe-mtime. Plus the
> header stack line, the `src/gpu/` layout row, the ImGui gotcha, and a new
> gotcha for phase 3b's trap (a core feature still has to be *enabled*).
> DESIGN.md §12 gained a second update paragraph: the port landed, Vulkan is
> default, parity is measured, and Dawn is retained because *its job is to
> disagree*.

> **[AS BUILT] DAWN REMOVAL — executed EARLY, out of phase order (2026-08-22,
> user decision).** This plan said removal waits until phase 7's page table is
> validated. It did not: **the user relaxed the cross-backend equality
> requirement outright** — bit-equality across backends no longer matters, and
> what matters is that determinism holds ON VULKAN going forward, anchored by
> the pinned `7cfa2420`. Dawn was retired having already agreed with Vulkan on
> all 23 gates, character for character, for a full phase (4b→6), so this is
> early retirement of a proven oracle rather than an abandoned proof.
>
> **What was removed:** `rhi_dawn.{h,cpp}`, the Dawn path in `context.cpp`
> (instance/adapter/surface/RequestDevice and the wgpu Resize/AcquireFrame/
> Present), `rhi::BackendKind::Dawn` *the enumerator*, the second table walk in
> `simulation.cpp` (plus `Simulation::BeginPass` and that file's local
> `CondHolds`/`Extent`, dead with it — `vk_record.cpp` has always carried its
> own pair), `ImGui_ImplWGPU_*` and `IMGUI_IMPL_WEBGPU_BACKEND_DAWN`, and
> `dawn::webgpu_dawn`/`webgpu_cpp`/`webgpu_glfw` from every link line
> including the two CPU-only test harnesses that only ever wanted the include
> path. `--backend dawn` now REFUSES with an explanation and exit 2 — a run
> reported as Dawn that was Vulkan all along is worse than no run, the same
> principle behind phase 3b's refusal.
>
> **What was kept, and why the seam did not go with it:** the `rhi::` seam
> including the polymorphic impl layer. An abstract base with one subclass is
> one virtual hop on ~60 dispatches a tick, it is what made this port testable
> phase by phase, and it is the slot phase 7's paged-residency `BufferImpl`
> plugs into. Removing it is the change that would have to be undone.
>
> **Tint stays, and so does the Dawn CHECKOUT — this is the constraint the
> removal turned on.** `tint_lang_wgsl_reader` + `tint_lang_spirv_writer` are
> linked into sandvox (WGSL→SPIR-V at load and at every F5; a precompiled
> `.spv` cannot work because F5 rebuilds the constant prelude from live tuning
> values) and `tint.exe` is `check_shaders.sh`'s validator. The mechanism that
> excludes the *engine* without touching Tint: Dawn's own CMakeLists gates
> `add_subdirectory(src/dawn)` — which is what DEFINES dawn_native,
> webgpu_dawn, webgpu_cpp and webgpu_glfw — on "at least one
> `DAWN_ENABLE_<backend>` is ON". With all of them OFF those targets are never
> declared, so nothing can link them by accident, while
> `add_subdirectory(src/tint)` is unconditional. `C:/sv-deps` cache semantics
> and the FetchContent declaration are untouched.
>
> **Three traps, all now commented in CMakeLists.txt:** (1)
> `TINT_BUILD_SPV_WRITER`/`SPV_READER` default to `${DAWN_ENABLE_VULKAN}`, so
> turning the Dawn Vulkan backend off silently turns off the SPIR-V writer the
> engine is built on — both forced ON independently. (2) `DAWN_USE_GLFW` must
> stay ON: that flag is what makes Dawn's `third_party` add the GLFW *library*
> our `glfw` target comes from (Dawn's own `dawn_glfw` wgpu-surface helper is a
> different thing, under `src/dawn`, excluded). (3) `Vulkan::Headers` is
> declared by Dawn only for its own Vulkan backend, so it vanished too — the
> sources are fetched under `dawn_standalone` regardless, so CMakeLists does
> that `add_subdirectory` itself behind a `NOT TARGET` guard.
>
> **The smokes were repurposed, not deleted (D2).** `--vk-smoke` and
> `--vk-smoke-loud` now check 5 and 19 probes against **pinned** hash
> sequences — verbatim the values 10156bb and c0cc28f established under the
> cross-backend diff. That is more coverage than the diff in one direction (it
> catches a change that moves *everything* identically, which the diff could
> not) and less in the other (no disagreement to localise a barrier bug by),
> and the gap is covered by sync validation, which was always §6.2's PRIMARY
> detector and needs no divergence to fire. A bug found doing this: the "tick
> recording" stats line read `LastStats` after the loop and so described a
> trailing rehash — it is now snapshotted at a representative tick and reprints
> the phase-3b/4a records exactly (quiet 11/59/2/3/63, loud 20/64/38/5/104).
>
> **Measured:** `sandvox.exe` 13,326,336 → 8,099,840 bytes (−39%); 156
> dawn_native TUs and a 178 MB `webgpu_dawn.lib` no longer built.
>
> **Checkpoint evidence (mtime-verified exe):** `--selftest` 23 gates exit 0,
> `determinism: PASS (final hash 7cfa2420 over 200 ticks, matches baseline)`,
> pond-freeze + mob known-failing; `--vk-smoke --vk-validation` 5/5 MATCH ZERO
> messages; `--vk-smoke-loud --vk-validation` 19/19 MATCH ZERO messages, 8
> shifts / 34059 chunks in store (identical to the phase-3c record);
> `--frames 300 --vk-validation` clean exit, 0 validation messages;
> `check_shaders.sh` 12 shaders OK; `check_invariants.py` and
> `check_pass_table.py` silent.
>
> **Deliberately NOT taken, to keep this purely subtractive:** the
> `simSlimBGL_` collapse (Dawn's 16-entry layout limit is gone; `--vk-info`
> measures 1048576 per stage) and the indirect staging-copy elimination. Both
> change descriptor sets or barrier scopes and are their own hash-gated
> changes.

**Phase 7 — sparse residency (the payoff) — REWRITTEN 2026-08-22 per
`docs/ROADMAP_scale.md` §1 (user-reviewed): SOFTWARE PAGE TABLE, not
`VK_KHR_sparse_binding`.** Hardware sparse was validated as *available* on
this GPU (phase 3a: all three caps YES) and rejected on evidence: measured
`vkQueueBindSparse` cost degrading superlinearly with page count on NVIDIA
(bind is a CPU queue submit, never GPU-driven), `maxStorageBufferRange`
spec-capped at 4 GiB and 2 GiB on some AMD drivers (3a measured 4 GiB−1 here),
and no way to express a `UNIFORM(material)` page — the sentinel that makes
downward window growth (solid bulk) nearly free. Shape: flat u32 table, chunk
slot → page index into a pooled physical buffer, or `EMPTY`/`UNIFORM(mat)`
sentinel; one dependent load per chunk entered; the CA is unaware of it; NOT
an octree (O(1), in-place mutation). Determinism by construction — no
driver-dependent unbound-read behavior, so `residencyNonResidentStrict`
becomes moot.
*Checkpoints (unchanged in substance):*
1. New gate: page alloc/free roundtrip under streaming + mutation.
2. **Paged-vs-dense hash equality**: same seed, page table on/off, identical
   hash sequence over a scripted scenario including sky-boundary explosions.
   **This is now THE equality matrix** — with Dawn removed, `vulkan-paged` vs
   `vulkan-dense` replaces `dawn` vs `vulkan` as the two configurations a
   sequence is diffed across. `--residency` should reuse the `--vk-smoke`
   driver rather than build a second one: `RunScenario` in `src/gpu/vk_smoke.cpp`
   still returns a full probe sequence (not a verdict) and `CompareAndReport`
   still takes two sequences, kept in that shape deliberately for this.
   Note what the diff is and is not worth: paged-vs-dense varies the
   *residency mapping*, not the host layer or the shader compiler, so it is a
   strong test of the page table and says nothing new about §14 risk #3.
3. Settled-world resident memory reported; full gate green.
Window growth (2048³ @ 5 cm per ROADMAP §2) remains its own later milestone
with fresh measurements.
*Checkpoints:*
1. New gate: unbind/rebind roundtrip under streaming + mutation.
2. **Sparse-vs-dense hash equality**: same seed, sparse on/off, identical hash
   sequence over a scripted scenario including explosions at the sky boundary.
3. Settled-world resident memory reported (~87 MiB expected); full gate green.
Then, as its own milestone with fresh measurements: grow the window (1024³
implies 8× occupancy/dirty metadata, compaction-scan and full-scan cost growth,
far-field shift-base retune — sized separately before committing).

### Phase 7 [AS BUILT] — 2026-08-22, commits dbf47d5 … 5d02e7b

Implemented against `docs/PLAN_page_table.md` §8, commits 0–5. **The memory
claim landed exactly**; one design formula did not survive contact and is
recorded below as an open item rather than papered over.

**Measured, `--measure --residency paged`, default seed, settled:**

| number | value | against |
|---|---|---|
| resident pages | **4,975 = 77.7 MiB** | 77.7 MiB measured / 86.9 MiB estimated |
| dense equivalent | 32,768 = 512 MiB | — |
| reduction | **6.6×** | — |
| pool reservation (`kPoolPages`) | 8,192 = 128 MiB | reserved VRAM, not resident |
| high water | 8,192 | during batched worldgen only |
| pages freed | 27,793 | the all-air chunks, demoted to `EMPTY` |
| settled tick | **240.975 µs** | 229–236 µs record → **+2.1%**, inside the 5% threshold |

Resident content and pool reservation are reported separately on purpose:
conflating them is how a phase claims a win it did not get (§3.7).

**Equality, `--vk-smoke --residency paged`:**

```
worldgen  f97ba745  MATCH   tick 1   d5c8944c  MATCH
tick 15   f153ce74  MATCH   tick 30  434268e6  MATCH
tick 50   b3c643a2  MATCH   5/5 MATCH   validation: ZERO messages
```

A world on 4,975 physical pages with 27,793 `EMPTY` sentinels hashes
bit-identically to the same world on 32,768 dense pages, and both reproduce
the pinned constants. That exercises the whole chain: translation arithmetic,
sentinel encoding, the analytic hash branch, the two-base split, `synthWord`'s
CPU/GPU agreement, materialization and the fills.

**Suite:** 25 gates, `determinism` reporting the pinned `7cfa2420`,
`page faults over the suite: 0`, `pond-freeze` and `mob` failing as at
baseline. `check_pass_table.py`, `check_invariants.py`, `check_shaders.sh` all
silent.

**Three design deviations, each resolved inside the doc's own rules:**

1. **§4.1a added — the two-base rule is not only about the hash.** Five more
   sites key an *identity* on a voxel index: the per-cell RNG in
   `sim_step:main` and `doStaining`, the ejecta and grit rolls in
   `sim_explode:apply`, the palette-variant roll in `sim_mutate:main`, and the
   particle claim lattice in `sim_particle:resolve`. Generalized rule: **an
   index used as an identity must be the SLOT index; only a memory address may
   be the PAGE index.** Equal under the identity map, which is why commit 1
   could introduce the split and prove it neutral.
2. **The accessors cannot live unconditionally in `common.wgsl`** — WGSL
   resolves module-scope references in unreachable functions, so a block naming
   `voxels`/`pageTable`/`pageFaults` fails to compile in the five shaders that
   declare none of them, and `raymarch` declares `voxels` as `read` with no
   `pageFaults`. Two delimited blocks (READ / WRITE), stripped per shader by
   `LoadShader` and by `check_shaders.sh`. `common.wgsl` stays the one place
   the translation is written, and `sim_compact` acquires no spurious
   `R(PageTable)`.
3. **Bindings are 17/18 in BOTH `simBGL_` and `simSlimBGL_`**, not 17/18 and
   5/6 as §5.2 says: one WGSL identifier cannot carry two binding numbers
   across modules sharing `common.wgsl`, and 5/6 are already `PassParams` and
   brush ops. The slim group becomes 0..4 + 17..18 — legal under Vulkan, and
   it preserves the property §5.2a wanted (anything on `simSlimBGL_` inherits
   translation, which is how `worldgen:fardown` gets it).

**One pre-existing gap surfaced and fixed in the harness layer:**
`World::Snap().valid` was false on every tick of every headless harness — the
async map's fence had not retired when `ProcessEvents` ran in the same loop
iteration. Pre-existing (it is why `KindAt` returned Unknown under harnesses)
but paging is the first system to depend on the snapshot, since §3.2's
tightening is the only thing that shrinks the mirror.
`SetHarnessSnapshotDrain()` (`test/support.h`), **off by default**, makes a
harness tick behave like a game frame. The game's frame loop shares
`SubmitTick` and never takes the path.

---

#### RESOLVED — §3.4's particle set collapsed to a per-tick spawn ring

The one formula that did not survive implementation. §3.4 originally carried
and dilated a set of chunks a particle might BE in; that is a correct superset
but unbounded, and measured on the loud scenario one explosion's debris drove
it **1, 27, 125, 343, 729, 1331, 2197, 3375** chunks over eight ticks
(3375 = 15³), past an 8,192-page pool on its own.

**Adjudicated by review as a COLLAPSE, not a bound.** Every particle write
lands within one cell of matter that already exists, so the bracketed half of
`materialize(N)` already covers all of them after the first tick of flight:
a stain write targets a non-air cell directly (guards at
`sim_particle.wgsl:229`, `:234`, and the buried branch at `:130-139`); a
reinsertion targets `lastAir`, ≤ 1 cell from a blocking sample because the
sweep subdivides to ≤ ½ voxel (`:153-154`). What remains is
`particleSpawnChunks(N)` — this tick's spawn sites plus one ring, recomputed
from scratch, bounded by the op caps and **independent of flight duration**.

*The old formula tracked where a particle might BE, which grows with flight
time; this one tracks where a particle might WRITE, which is pinned to matter
that already exists.*

The predicate was renamed `nonSentinel` → `hasMatter` and widened: only
`PT_EMPTY` is excluded, since a `UNIFORM(mat)` sentinel holds matter and
`blocksParticle` reads through `voxWordAt`, so a particle can legitimately land
against uniform water. A strict widening, so §3.2a's wake-all argument is
untouched.

**Result: the pool is stable.** The full 120-tick loud scenario now completes
at **~5,890 pages of 8,192**, with the materialization set flat at ~1,200 —
against exhaustion at tick 52 before. `pageFaults == 0` across the whole run,
which is the *evidence* for the adjacency argument rather than a formality.

A second leak was found and fixed alongside it: the streaming `genList` path
allocated a page for every slot on an incoming shift plane and never demoted
the ~85% that generate as pure sky, leaking ~880 pages per shift. It now
classifies and demotes exactly as batched worldgen and the store-hit path do.

#### CLOSED — the loud scenario's paged hash — [AS BUILT, phase 7 close]

`--vk-smoke-loud --residency paged` was **1/19** against the pinned constants,
diverging from tick 15 onward, while quiet-paged was 5/5 and loud-dense 19/19.
It is now **19/19 == dense == pinned**, and it was **two independent bugs plus
a broken evidence chain that hid the first of them**.

**(1) The induction base case — `materialize` did not dilate op targets.**
`materialize(N)`'s bracketed half is evaluated against `hasMatter` *at encode
time*, so an op that paints matter into `PT_EMPTY` sky is invisible to it, and
the CA moves that new matter one cell in the SAME tick — into a chunk that is
neither an op target nor ring-reachable from any chunk that had matter. The
loud scenario's WATER brush at `(176,150,176)` r5 spans the `y=9` chunks; water
fell into a `y=8` chunk at tick 8 and the write was lost (slot 11531,
`pageTable` entry `0x80000000`). Sand never exposed it because sand is painted
next to existing matter. Fix: `materialize` unions `N26(opTargets(N))`. One
ring is sufficient because op-marked acting cells write at reach ≤ 1; bounded
because op counts are capped and the set is recomputed each tick. Normative
formula and soundness argument: `PLAN_page_table.md` §3.2 step (4) and §3.4.

**(2) An RNG keyed on the physical page.** `sim_step:doReactions` called
`hash3(rnd, ri, idx)` with `idx` the page-resolved word index, making every
reaction roll a function of **allocation history**. Invisible under the
identity map (dense), so it could only ever appear once a page was assigned
non-identically. Reproduced as a deterministic lava/stone swap at slot 9450,
words 893/1149. Fix: it takes `slotIdx` alongside `idx`, exactly as `main`
does — whose comment predicted this failure verbatim. This is the sixth entry
in `PLAN_page_table.md` §4.1a's table; the five before it were found by
auditing `hash3` *call sites*, and this one hid one function-call deep. All
`hash3` sites in the sim shaders were re-audited; it was the only offender.

**(3) Why (1) went unseen for the whole phase: `pageFaults == 0` was vacuous.**
The bullet above claiming "`pageFaults == 0` on every run, so no write is being
lost" was **false in two ways at once**. `vk_smoke` printed a hardcoded `0` —
`RunResult::pageFaults` was declared and never assigned — and the counter
buffer was never zeroed, so it began at whatever the driver left behind
(measured 134,217,728 == 2²⁷ on a 3060 Ti). The GPU had recorded the tick-8
fault correctly all along; nothing ever read the register. Fixed: a real
blocking readback after `WaitIdle`, and zeroing in `ResetAllEmpty` /
`ResetIdentity` plus once after paged worldgen (worldgen legitimately stores
through sentinels for the batches it does not hold, which is what lets an
8,192-page pool generate 32,768 slots; the invariant the gates assert is about
the tick loop). The standing rule in §3.4 — *if `pageFaults` is ever non-zero,
find the path, do not widen the ring* — is only enforceable now that the
number is real. **The lesson worth carrying forward is procedural: a green
counter is evidence only if something has proven the counter can go red.**

#### CLOSED — the flight shell, the demotion leaks, and the measured pool — [AS BUILT, phase-7 FULLY closed, 2026-08-22]

**Both residency modes are green end-to-end.** `--selftest --residency paged`
exits 0 for the first time: hash `7cfa2420`, `pond-freeze` + `mob` the only
baseline failures, `pageFaults 0`, high-water 14,934 of 16,384. Dense is
byte-identical to before (hash unchanged). `--vk-smoke-loud --residency paged
--vk-validation` 19/19 vs pinned with ZERO validation messages; `--vk-smoke`
5/5 both modes.

**(1) The last formula gap: GPU-originated wakes during particle flight.**
Gate `flung-liquid` paged lost blood (21 voxels at fullness 1 vs dense's
76 at 7, 62 real page faults). Mechanism: airborne particles dirty nothing, a
fresh snapshot correctly tightens `cpuDirty` to 0, the chunks under the
flight path demote — and when the particles land, `resolve`'s `markDirtyNext`
has NO CPU-side contributor to re-enter the mirror (the intersection can only
remove; 0 stays 0 — measured for 37 straight ticks while faults climbed).
Fix: §3.1a contributor **(e), the particle flight shell** — while particles
may be in flight, `occMatter(S)` (chunks with non-zero snapshot occupancy) is
unioned into `cpuDirty` post-tighten, post-propagate, lingering one
application past the off condition; the bracketed half materializes its ring.
Seeded from OCCUPANCY because a residency-seeded shell feeds back one ring
per tick (measured to 3 rings, past an 8,192 pool on one gate); the ring
guards demotion instead of entering the mirror. Full soundness argument:
`PLAN_page_table.md` §3.4, "The GPU-originated-wake hole". Result:
`flung-liquid` paged **76/7, bit-identical to dense, 0 faults**.

**(2) Two permanent demotion leaks, found by the first suite that could run.**
`Classify` refused stamped air (`0x00030000` — the CA stamps vacated cells)
and jittered air (`0x00002000` — `sim_mutate` gives every painted voxel a
palette-jitter state, air included), so any chunk ever touched leaked its
page: ~14,400 resident after streaming+spells. The free predicate is now
"every cell is stainless air" (bits 12..23 are audited passenger bits on air;
stain stays load-bearing). Also: `zeroStreak_` re-arms on materialization
(the saturated-counter leak), and free probes are capped at 64/tick with
held-at-7 retry (uncapped, a whole-window load minted thousands of blocking
WaitIdle+readback probes on one tick — a minutes-long stall that presented as
a hang). `PLAN_page_table.md` §3.6, "Demotion in practice".

**(3) The pool, measured honestly this time.** The first attempt read 32,768
by construction — `ResetIdentity` latched `pagesHighWater_ = pagesInUse_` at
seeding, so raising the pool to measure raised the answer. The latch is gone
(high-water's sole writer is `Alloc()`), and the suite prints its high-water
on every paged run. Measured: **14,934 pages stable across four builds**,
driven by the streaming gate's flight-speed churn, not by the shell
(`flung-liquid` peaks at 8,406). **`kPoolPages = 16384` = 256 MiB reserved**,
2× under dense, 1.10× over the measured worst case — the "×1.25 → power of
two" rule would land on 32,768 = dense = no win, and is deliberately not
honoured; rationale in `PLAN_page_table.md` §3.7 and `world.h`.

**Gate fix in the same commit:** `page-roundtrip`'s free assertion was a
global page count, which reads a working roundtrip as a failure on a live
world (spell fires materialize faster than one page frees) — it now asserts
the painted SLOT returns to sentinel, waits bounded for the capped drain, and
seeds a world when run standalone against the identity map.

#### DEFERRED follow-ups (severable, recorded per the pacing directive)

- **Ring-starvation gate.** Hold all three readback slots in flight for ~10
  consecutive ticks while a fire spreads, and assert the hash still matches
  dense. It is the only test that would exercise §3.2's conservative path
  rather than its exact one — with a healthy ring the tightening is the
  identity and the fallback is dead code that has never run.
- **Low-`kPoolPages` abort gate.** Run with a deliberately tiny pool and assert
  the abort fires cleanly with the right message — testing that the failure
  mode *works*, since under §3.8 it is now the only one.
- **Sanitize air's state nibble at the sources** (`sim_mutate.wgsl:80`,
  `productState`): writing `state = 0` for `MAT_AIR` would stop minting the
  passenger bits the free path now masks. Audited inert, so it is a cleanup,
  not a correctness item — but it touches sim kernels, so it is its own
  hash-gated change.
- **Streaming churn residency** (the 12.4k–14.5k plateau): contributor (d)
  puts whole refilled planes into `cpuDirty` and their rings materialize
  behind the hysteresis; if the pool margin ever matters, an occupancy-
  filtered bracketed half (with op-ring retention for snapshot staleness) is
  the formula-level lever — sketched during the close, not taken.

**Phase 8 — capability exploitation (each its own measured change).**
Async compute/transfer queues (readbacks, far-field fill off the main queue);
subgroup ops in occupancy/compaction only (their output order provably cannot
leak into sim state — but treat as sim-adjacent and hash-gate anyway); the
settled-tick fixes (cheap empty dispatches, possibly skip-encode);
explicit heap placement. Re-run `--measure` after each.

## Working rules binding every implementation agent

- `bash scripts/board.sh claim` before editing; check mtimes on hub files;
  `done` with what actually landed. Build only via `bash scripts/build.sh`.
- **Commit at each green checkpoint** with the actual selftest result in the
  message. Stage only files inside your board claim — `git add -A` in this
  tree sweeps other sessions' work.
- Report actual selftest output; never claim a sim change works without it.
- No `f32`, no scheduling-dependent ops, no subgroup ops in sim kernels; write
  reach ≤ 1 cell; stateless RNG only (rule 1 survives the port verbatim).
- DESIGN.md/CLAUDE.md updated in the same commit as any contradicting change.
- The pass table and `docs/vulkan_barrier_graph.md` are load-bearing: a tick
  path edit that changes R/W sets updates both in the same commit, and the
  checker must agree.

## Decision log

- 2026-08-22: WGSL retained via Tint→SPIR-V (zero shader rewrites; hot-reload
  preserved). Staging copies for indirect args retained in v1 (collapsing them
  is legal in Vulkan but changes barrier scopes — separate hash-gated change).
- 2026-08-22: cross-backend hash comparison chosen over cross-API buffer
  sharing for validation (no interop complexity; the selftest already computes
  the hashes).
- 2026-08-22 (user): **browser build dropped.** DESIGN.md §1/§12 updated in the
  adoption commit. Dawn's remaining role is reference backend + hash oracle
  during the port; removed after phase 7 validates (see phase 6).
- 2026-08-22 (user): **DAWN REMOVED EARLY — cross-backend bit-equality is no
  longer a requirement.** Supersedes the line above: removal was scheduled
  after phase 7 and executed before it. The user's framing, recorded because it
  is what licenses the change: what matters is that determinism holds *on
  Vulkan* going forward, with the pinned `7cfa2420` as the anchor — not that
  two backends agree. Dawn had already agreed on all 23 gates for a full phase.
  Consequences folded into the plan: `--vk-smoke`/`--vk-smoke-loud` become
  pinned-sequence gates (D2), phase 7's equality matrix becomes vulkan-paged vs
  vulkan-dense, §14 risk #3 narrows to "one vendor, one driver, one compiler,
  one host layer" and still needs a second vendor to close. The Dawn CHECKOUT
  and Tint stay — WGSL→SPIR-V at load and F5 depend on them; only the Dawn
  ENGINE targets are excluded. The `rhi::` seam stays.
- 2026-08-22: **the now-unlocked simplifications are deliberately deferred**,
  so the removal commit stayed purely subtractive and its green gate means
  what it says: `simSlimBGL_` collapse (Dawn's layout-entry limit is gone) and
  indirect staging-copy elimination. Each changes descriptor sets or barrier
  scopes, so each is its own hash-gated change.
- 2026-08-22 (user): **commit at every completed unit of work.** Implementation
  agents commit at green checkpoints with the selftest result in the message;
  never sweep files outside their board claim into a commit.
