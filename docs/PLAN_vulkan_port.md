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
  seed + same tick = same hash) before it renders a single pixel.

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

**Phase 4 — Vulkan render path.**
Swapchain, the 6 raster pipelines, reversed-Z depth, `imgui_impl_vulkan`,
screenshot/offscreen paths.
*Checkpoint: full 20-gate selftest green on `--backend vulkan`; screenshots
visually equivalent; render ms reported vs Dawn baseline.*

**Phase 5 — second-adapter validation.**
Run the Vulkan selftest on every enumerable device (`--adapter low` equivalent;
this machine may expose only the 3060 Ti — if so, evaluate lavapipe/SwiftShader
as a deterministic-CPU cross-check for the hash sequence). Progress on
DESIGN.md risk #3, not closure, unless a second vendor is actually present.

**Phase 6 — switch default; update docs.**
Default backend → Vulkan. CLAUDE.md build/verify sections updated in the same
commit (DESIGN.md §12 was already updated when the plan was adopted). Dawn is
retained through phase 7 purely as the cross-backend hash oracle — the browser
requirement was dropped 2026-08-22, so once sparse residency is green and a
second adapter has validated the hash sequence, Dawn (and its ~15-min
first-configure fetch) is removed in a cleanup commit that also simplifies the
RHI seam to a single live backend. The seam itself stays: it is what made the
port testable and it costs nothing to keep.

**Phase 7 — sparse residency (the payoff).**
4 GiB virtual voxels buffer (verify `maxStorageBufferRange`), 64 KiB pages = 4
consecutive chunk slots; bind on write-need (worldgen/stream-in/mutation into
an unbound page), unbind when all 4 chunks report `occTotal == 0` with
hysteresis; sparse-queue binds fenced before the tick submit. Gated by
`residencyNonResidentStrict` (else dense fallback).
*Checkpoints:*
1. New gate: unbind/rebind roundtrip under streaming + mutation.
2. **Sparse-vs-dense hash equality**: same seed, sparse on/off, identical hash
   sequence over a scripted scenario including explosions at the sky boundary.
3. Settled-world resident memory reported (~87 MiB expected); full gate green.
Then, as its own milestone with fresh measurements: grow the window (1024³
implies 8× occupancy/dirty metadata, compaction-scan and full-scan cost growth,
far-field shift-base retune — sized separately before committing).

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
- 2026-08-22 (user): **commit at every completed unit of work.** Implementation
  agents commit at green checkpoints with the selftest result in the message;
  never sweep files outside their board claim into a commit.
