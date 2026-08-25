# ROADMAP: scale — smaller voxels, bigger sim, farther horizon

Status: research summary, 2026-08-22; updated 2026-08-24. The Vulkan port is
COMPLETE (phase 7 closed at `a5359a4`; Dawn removed 2026-08-22) and the
software page table has LANDED (`src/sim/pagetable.cpp`, `d3dcb76`). Items
below marked DONE are verified against the codebase. Where this contradicts
DESIGN.md, DESIGN.md wins until explicitly amended.

## 0. Ground truth this was computed from (re-verify before acting)

- Window: `kWorldN = 512` (512³ voxels, 32³ chunks), `kVoxelMeters = 0.10`
  → 51.2 m edge. Voxel buffer 512 MiB dense; ~710 MiB total persistent GPU.
  (Some `world.h` comments still say 16/4096 chunks — stale from the 256 era.)
- Measured occupancy (phase-0 `--measure`, seed 1337, 300 settle ticks):
  **11.25% non-air; 27,794/32,768 chunks empty; 2,338 fully full; 2,636 mixed**.
  Sparse payoff is almost entirely sky; terrain surface ≈ 2.6× flat footprint.
- Per-pass GPU time: settled tick 306 µs, of which **184.6 µs is 54 empty
  indirect dispatches** and 95 µs full-world scans on hash ticks (rule-2
  violation, tracked in the port plan phase 3/8).
- Far field: 8 levels × 16 MiB = 128 MiB, horizon ≈ 6.5 km, derived/disposable,
  regenerates from **seed + persisted edits** (`src/sim/faredits.h`, 2026-08-24:
  the sieve's refills no longer erase what the live downsample put there, and
  `LoadWorld` rebuilds the edit index from the region files).
- Saves refuse to load on any `kWorldN` or `kVoxelMeters` change (worldio.cpp
  checks the exact float bits). Changing either invalidates every save.

## 1. The one architectural decision: software page table, not hardware sparse

**DONE.** The software page table landed as `src/sim/pagetable.cpp` (commit
`d3dcb76`), with the JITTER sentinel at `f65aa2a` compressing buried bulk.
`docs/PLAN_page_table.md` is the plan of record. Adversarial descent residency
settled at 14,697 / 32,768 slots (-55% vs dense). Evidence that informed the
decision:

- NVIDIA `vkQueueBindSparse` measured ~41 µs/page at 730 pages degrading
  superlinearly to ~390 µs/page at 9,200; known regression since driver 555.x.
  Binds are CPU queue submits — can never be GPU-driven.
- `maxStorageBufferRange` is spec-capped at 4 GiB and reports 2 GiB on some
  AMD drivers; the planned "4 GiB virtual buffer" sits at/over the descriptor
  ceiling on day one. BDA (`VK_KHR_buffer_device_address`, core in 1.3) or
  split ≤2 GiB pools is the standard escape.
- Hardware sparse zeroes unbound pages (air only, and only with the optional
  `residencyNonResidentStrict` cap). A software table gets sentinels:
  **`EMPTY` and `UNIFORM(material)`** — the latter is what makes downward
  window growth (solid stone bulk) nearly free, which hardware sparse cannot do.
- Industry precedent: virtual texturing converged on software tables (id Tech,
  PLAYERUNKNOWN Productions, WebGPU rejected sparse resources); Gustafsson's
  post-Teardown engine uses sparse 8³ chunks tracked by a bitmap.

Shape: flat array of `kNumChunks` u32 entries (chunk slot → page index in a
pooled physical buffer, or sentinel). One extra dependent load per chunk
entered; table is small and cache-hot. Determinism is by construction — no
driver-dependent read behavior, so the sparse-vs-dense hash-equality
checkpoint gets easier, not harder. All phase-7 checkpoints survive verbatim.
**This is NOT an octree** — O(1) access, in-place mutation, CA unaware of it.
John Lin's SVO critique (see `docs/refs/perfect_voxel_engine.md`) does not
apply to a one-level page table; it does apply to any tree under the sim.

## 2. The scaling law (napkin math, user-reviewed)

Both budgets fix the window size **in voxels**, not meters:

- **Memory ≈ 2 GiB of resident pages** (8 GiB card minus far field, pools,
  render targets, headroom) buys ≈ a **2048³-voxel window** with the page
  table + sentinels. Resident cost is dominated by mixed (surface/cave)
  chunks: `pages ≈ 2.6 · L²/(16v)² × 16 KiB` (L = edge meters, v = voxel m).
- **Compute ≈ 30–50k active chunks/tick** at 30 Hz (extrapolated from
  0.2 µs/chunk; ±2–3× uncertainty). Halving voxel size costs a same-physical-
  size event ×8 chunks and naively ×2 substeps = **×16 per halving** (but see
  §3.3 — the substep tax is avoidable, making it ×8).

| Voxel size | 2048³ window edge | Same-size fire costs |
|---|---|---|
| 10 cm (today) | 204.8 m (±102 m) | 1× |
| **5 cm (recommended)** | **102.4 m (±51 m)** | 16× (8× with §3.3) |
| 2.5 cm | 51.2 m (±26 m) | 256× — **rejected**, kills large reactions |

**Recommended landing spot:** 5 cm sim voxels, 2048³ window (102.4 m edge =
2× today's physical distance at half the voxel size), ~1–1.5 GiB resident.
Apparent voxel size 6.25 mm via subdiv-8 micro detail (3 mm at subdiv 16).
Render distance via cascades: +16 MiB per horizon-doubling; 10 levels ≈ 52 km
at 160 MiB — the screen runs out (5 cm is sub-pixel past ~60 m at 1080p)
before memory does.

Event budget at 5 cm (with optimizations from §3): campfire trivial, burning
house fine, dam break borderline, forest fire needs the §3 wins to fit.

The 5 cm change is a project, not a constant edit: avatar height (17 voxels →
derived from Player constants, asserted in gen_mina), CPU-mirror physical
reach halves (fast-projectile Unknown-passable issue worsens), explosion
radii, reaction rates, particle tuning, streaming budget — and every save.
Pay it once; do not do 10→5 and later 5→2.5.

## 3. Simulation optimizations, ranked by payoff

> **[MEASURED 2026-08-22] The ranking below was written against a compute
> anchor that is wrong by ~35x, and item 1 has since been built, measured, and
> REVERTED as a net loss. Read §3.0 before acting on anything in this list.**

### 3.0 The measured cost model (supersedes the §2 anchor)

`--measure` now samples `Snap().activeChunks` per tick and carries a fourth
scenario (d) HEAVY specifically to put a third, distant point on the cost
curve. Least-squares over the three (RTX 3060 Ti, seed 1337, CA-running ticks):

```
CA per tick = 54 x (4.65 us  +  0.245 us x activeChunks)
              ^^^^^^^^^^^^^     ^^^^^^^^^^^^^^^^^^^^^^^
              251 us/tick       13.3 us per chunk per tick
              FIXED FLOOR       (not the 0.2 us §2 assumed)
```

Residuals are ±15% at the two low points, so chunk CONTENTS matter too and the
per-chunk figure is good to about a factor of two. Measured points:

| active chunks | CA µs/tick | floor share |
|---|---|---|
| 3.0 | 343 | 86% |
| 25.0 | 504 | 43% |
| 68.7 | 1188 | 22% |

**Where §2's 0.2 µs/chunk came from, so it is not re-derived:** §0 records
"473 µs CA while ~2,600 chunks were settling", and 473/2600 ≈ 0.2. But 2,636 is
the count of MIXED (non-empty) chunks in the whole 32,768-chunk window — a
static occupancy statistic — not the ACTIVE dirty chunks in that tick. The
settling scenario really runs ~25 active chunks. The denominator was ~100x too
large.

**Consequence:** at 8 ms/tick the CA affords roughly **600 active chunks**, not
30–50k. Every event-size row in §2's table is optimistic by a large factor.

**And it splits the problem in two, which the old single-number budget hid:**

- **the FLOOR** (54 dispatches x 4.65 µs, content-free) dominates at today's
  event sizes and is attacked only by ISSUING FEWER DISPATCHES.
- **the PER-CHUNK term** dominates above ~200 active chunks — i.e. exactly the
  regime the 5 cm / 2048³ plan targets — and is attacked by doing less work per
  chunk.

An optimization aimed at one does nothing for the other. Item 1 aimed at the
per-chunk term and was measured in the floor-dominated regime.

### 3.1 was BUILT AND REVERTED — cell masks are a net loss

Implemented in full: `sim_mask.wgsl` deriving a 4³-block bitmask (2 u32/chunk)
over the dirty list, consumed by `sim_step` before its voxel read. Proven
correct — **bit-identical hash `7cfa2420`** — after two real bugs, both worth
recording:

1. **A comment killed the shader.** `LoadShader` decides whether to keep
   `common.wgsl`'s page-table WRITE block by substring-searching the body for
   `read_write> voxels` (`resources.cpp: BodyWritesVoxels`). A COMMENT in
   sim_mask.wgsl containing that exact phrase matched, the block was kept, it
   referenced an undeclared `pageFaults`, and the shader failed to compile.
   The failure was silent: `vk_record` does `if (pipe == VK_NULL_HANDLE)
   continue`, so the row was skipped, the mask buffer held garbage, and the
   build was green. Symptom was a wrong world hash with no error printed.
2. **Intra-chunk dilation is not enough.** An all-air chunk is in the dirty
   list because `markDirty` marks every chunk a written cell BORDERS — and a
   grain falling in from the chunk above must act on a LATER colour phase of
   the same tick. Masking it out freezes that grain. Bisected: identity mask
   PASS `7cfa2420`; only-air-inert + whole-chunk dilation FAIL `2f3896ac`. The
   fix is a one-cell READ MARGIN per block (crossing chunk boundaries through
   `voxWordAt`), which is exact because the CA's write reach is ≤1 cell.

Measured with the correct version (µs/tick):

| scenario | CA before | CA after | mask pass | net |
|---|---|---|---|---|
| active | 348.3 | 348.2 | 34.9 | **+34.9** |
| settling | 506.3 | 497.1 | 35.0 | **+25.8** |
| heavy | 1059.2 | 1067.3 | 65.7 | **+73.8** |

A net loss in every scenario. A first version that staged the mask in
workgroup memory was far worse (+39% to +82% on the CA alone) because the
`workgroupBarrier()` it needs is paid by all 216 threads of all 54 dispatches;
removing the barrier for a direct global read restored the CA to baseline but
left the mask pass itself as pure added cost.

**Why it cannot be rescued by tuning the granularity:** the mask attacks the
per-chunk term only, and recovers none of the 251 µs floor. In the regime it
was measured in, the floor is 43–86% of the cost. A coarser whole-chunk mask
is worse than useless — it would re-derive what chunk sleeping already does,
since a chunk is only in the dirty list because something in it or beside it
moved.

**When to revisit:** only above ~200 active chunks/tick, where the per-chunk
term dominates. That is a real regime for the 5 cm target, so this is
"measured, not viable yet" rather than "wrong idea". The shader is recoverable
from git history if so.

---

The sim is the bottleneck (memory closes comfortably; compute caps event
size). GPU note: "batch 8 adjacent voxels per evaluation" does not help —
cells already run 216-wide per workgroup; ALU is free, memory touches /
dispatches / substeps are the cost. The wins are: don't visit dead cells,
move bulk matter as objects, and don't multiply substeps.

1. **~~Cell-level active bitmasks inside dirty chunks~~ — BUILT AND REVERTED,
   see §3.1 above. The estimate below was never realised.** (~5–15× on CA traffic).
   Per-chunk 16³ bitmask (512 B/chunk) of cells that can act this tick; passes
   skip clear bits. 3D analog of Noita's dirty rects. Hash-neutral by
   construction → acceptance test = **bit-identical world hash with masks
   on/off**, plus `--measure` before/after. Files: `world.h` (buffer),
   `sim_step.wgsl` + occupancy/compaction kernels, `simulation.cpp`, and a
   `uses` row in `src/sim/pass_table.def` **in the same commit** (a missing
   row = phase-3 generates no barrier = cross-vendor desync later).
2. **Bulk promotion policy — move clumps as objects, not cells.** When a
   connected falling region exceeds a threshold, carve it into a debris body
   (Jolt) / particles and stamp back on rest — machinery exists, this is
   policy + thresholds in `game/`/`phys/`. Turns collapses from
   O(volume × fall-ticks) into O(surface + 1 body). Changes sim outcomes
   (hash moves) — it's a design change with tuning judgment, not hash-neutral.
3. **Don't double substeps at 5 cm.** Keep 2 substeps; anything needing >1
   cell/substep is ballistic and belongs in particles (velocity-integrated,
   deterministically reinserted — already the pattern). Halves the 5 cm tax:
   ×16 → ×8.
4. **Settled-tick fixes** — **LANDED 2026-08-22 (commit 2dd5073)**, and the
   biggest realised win so far. `Cond::CaActive` drops `compact`, the args
   staging copy and all 54 CA iterations when a conservative CPU latch proves
   the dirty set empty. Measured: settled CA **141.7 → 1.0 µs/tick (141×)**,
   settled tick wall clock **2.080 → 0.445 ms**, skip firing on 119/120 settled
   ticks, hash `7cfa2420` unchanged, `perf` gate MARGINAL → PASS. The
   sentinel-aware scan half of this item was ALREADY DONE by the page table
   (`sim_occupancy.wgsl:main` has an analytic sentinel branch); the full scan
   also runs only on hash ticks (1-in-15), so it amortizes to ~7 µs/tick and
   was never the bottleneck it appeared to be.
5. **Temporal rate LOD for slow reactions.** Chunks whose only activity is
   slow (smolder, growth) dispatch every Nth tick, N a pure function of
   (tick, chunk coord, rule rate) — deterministic. Needs conservative wake at
   boundaries (fast neighbor forces full rate). ~3–4× on big fires. Sequence
   AFTER item 1 (builds on the mask infrastructure).
6. **Async compute** (port phase 8): sim overlaps the 12–16 ms render on a
   second queue; effectively free budget until sim exceeds render time.
7. **Long shot, not recommended now: Margolus block partitioning** (2×2×2
   blocks, alternating offsets — the formal "clump of 8" CA). 1 dispatch per
   substep instead of 27 color passes, race-free, deterministic — but every
   rule reauthored, movement semantics change, color-lattice architecture and
   hash history invalidated. A rewrite, not an optimization. Only revisit if
   items 1–6 are exhausted and the CA is still the wall.

**Rejected (do not resurrect):**
- Multi-resolution / coarse-far CA: no shipped precedent; LBM literature says
  the resolution seam needs conservative-transfer machinery hostile to rule 1.
  Binary sim LOD (full-res active, frozen beyond) is the shipped answer.
- Aggregate bulk solvers (hydrostatic per-chunk ponds): same seam problem.
- Event queues / first-come-first-served claiming: scheduling-dependent,
  rule 1 kills it.
- Uniform-clump single-evaluation: saves ALU the GPU has to spare.
- 0.5 cm (or any sub-cm) SIM voxels: ~8,000× — detail must be derived
  (§4), never stored.

Stacked estimate for items 1–6: **~10–30× effective headroom on large
events** — puts the forest fire at 5 cm back inside budget.

## 4. Rendering findings

- **Apparent voxel size comes from the micro layer, not the sim cell.**
  Subdiv-8 already = 1.25 cm apparent at zero per-cell storage. At 5 cm sim:
  6.25 mm. Subdiv 16 = 3 mm where the ray cost is worth it (micro DDA step
  cap and `TUNE_MICRO_MAX_PER_RAY` were tuned at subdiv 8 — measure).
- **Near-field surface detail pass**: extend micro substitution beyond
  `MATF_MICRO` plants to all surfaces within N meters, procedural displacement
  keyed on `hash3(seed, 0, cellIndexW)` — deterministic, derived, disposable,
  zero world state. The "cheapest attribute is a derived one" rule applied to
  the whole ground. COW micro bodies are the destructibility precedent.
- **Screen-space LOD rule** (from an SDF-splatting practitioner rendering
  3 cm voxels at 100 km): nothing on screen smaller than ~1–2 px; the voxel,
  not the pixel, is the blend unit. Replace `TUNE_MICRO_LOD_DIST` (distance
  const) with a projected-size test; same rule principled-ly picks the far
  cascade level per distance (~1 px per cell).
- **Far field**: +16 MiB per level per horizon-doubling (the 128 MiB ceiling
  was a WebGPU limit that Vulkan deletes). ~~Two gaps~~ one gap before pushing
  far: cascade **edit persistence** landed 2026-08-24 (`src/sim/faredits.h`,
  selftest `far-persist`; edits below a cascade cell are still invisible at
  that level, which is LOD behaviour, not a gap). Remaining: shift/fill
  bandwidth. `kFarShiftBase` retune needed when the window grows (already noted
  in port plan).
- **Optional, only if `--measure` says the far march is expensive**: box-splat
  far surface cells (instanced boxes + per-pixel slab/DDA — the micro-body
  OBB path already does exactly this shape) instead of marching 8 cascade
  volumes per ray. Costs an extraction step + update-on-mutation; forfeits
  free volumetric fog integration. Also: per-voxel deferred shading dedup
  (shade once per distant voxel ID, not per pixel) if far shading grows.
- **SVO/SVDAG verdict** (John Lin is right and the engine already agrees —
  CLAUDE.md's architecture section is distilled from his post): never under
  the sim; acceptable ONLY as a static, per-region, lazily-rebuilt far tier
  beyond the cascades — derived, disposable, never edited (regenerate, don't
  mutate; SVDAG editing is an open research problem). If ever built, prefer
  a 64-tree (flat, 64-bit child masks, ~0.19 B/voxel) over a classic octree —
  measured ESVO traversal is up to 60% SLOWER than simple two-level grids.
  Likely unnecessary: added cascade levels are cheaper engineering.

## 5. Open measurements (cheap, do these first)

1. **~~Uniform-chunk histogram in `--measure`~~** — **DONE**
   (`src/measure/measure.cpp:292`, "MEASUREMENT 1b: the UNIFORMITY histogram").
   The finding (2,115 single-material chunks vs 41 whole-word uniform) directly
   motivated the JITTER sentinel.
2. Occupancy re-measure at the grown window before committing (the plan
   already stages this).
3. Cell-mask win: `--measure` before/after on a scripted active scene.
4. Verify the 0.2 µs/chunk compute anchor with a controlled active-chunk-count
   scene (current figure is a one-measurement extrapolation).

## 6. Sequencing and dependencies

The Vulkan port is COMPLETE (`a5359a4`, 2026-08-22) and the software page
table has LANDED (`d3dcb76` + JITTER `f65aa2a`). Settled-tick fixes also
landed (§3.4, `2dd5073`). The remaining work:

```
DONE: Vulkan port (all phases), software page table (§1), --measure histogram (§5.1),
      settled-tick fixes (§3.4), cell-level active masks BUILT AND REVERTED (§3.1)

next:
  grow window to 2048³ (fresh measurements, §5.2 first)
       └─ kVoxelMeters 0.10 → 0.05 (the one-time retune, §2)
independent (can run now):
  - bulk promotion policy (§3.2) — game/phys files only
  - near-field surface detail / subdiv-16 (§4) — WGSL + tuning only
  - temporal rate LOD (§3.5)
  - async compute overlap (§3.6)
far-field extension (§4): independent, any time
```

Constraints that outrank everything here, restated so no optimization erodes
them: rule 1 (bit-determinism: integer sim, stateless RNG, ≤1-cell writes, no
scheduling-dependent outcomes), rule 2 (cost scales with activity — every new
system needs a costs-nothing-idle state), rule 3 (all mutations through the
MutationQueue). Every hash-affecting change re-runs `--selftest`; hash-neutral
changes (masks, skip-encode, page table) must prove bit-identical hashes.
