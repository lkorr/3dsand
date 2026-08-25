# ROADMAP: scale — smaller voxels, bigger sim, farther horizon

Status: research summary, 2026-08-22. Written for the agent that picks this up
**after the Vulkan port completes** (see `docs/PLAN_vulkan_port.md`). Nothing
here is committed work; it is the distilled findings of a research session
(codebase sweep + external research) plus napkin math the user has reviewed.
Where this contradicts DESIGN.md, DESIGN.md wins until explicitly amended.

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
  regenerates from seed on load (edits beyond the window don't persist there).
- Saves refuse to load on any `kWorldN` or `kVoxelMeters` change (worldio.cpp
  checks the exact float bits). Changing either invalidates every save.

## 1. The one architectural decision: software page table, not hardware sparse

`PLAN_vulkan_port.md` phase 7 as written uses `VK_KHR_sparse_binding`.
**Recommendation (user-reviewed, plan doc not yet amended): rewrite phase 7
around a software page table instead.** Evidence:

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
>
> **[MEASURED 2026-08-24] §3.0's 251 µs floor is ~2x high — it is 122–128 µs,
> directly measured (§3.2a). Per-colour-phase dispatch skipping, the obvious
> attack on that floor, is REFUTED (§3.2). The floor's remaining real lever is
> §3.2d.**

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

### 3.2 was MEASURED AND REFUTED — per-colour-phase dispatch skipping

**[MEASURED 2026-08-24, branch `perf/ca-phase-skip`. Nothing was built. Do not
re-derive this.]**

§3.0 ends by saying the fixed floor "is attacked only by ISSUING FEWER
DISPATCHES" and proposes no way to do it. The obvious way is to generalise
§3.4: a colour phase only has work if some dirty chunk holds a cell of that
phase's colour, so skip iteration k when phase k's set is empty. **It does not
work, and the reason is content, not engineering.**

#### 3.2a The floor has MOVED — 122 µs/tick, not 251

§3.0's floor was the *intercept* of a three-point least-squares fit. It is now
measured directly: `SANDVOX_CA_FORCE=1` defeats §3.4's settled skip so a
SETTLED world still records all 54 CA iterations with an indirect count of
**zero**, making the `ca(...)` row the pure content-free cost;
`SANDVOX_CA_REPEAT=n` truncates the loop to n so the per-dispatch cost is a
SLOPE, not an intercept. Both knobs live on the branch (`simulation.cpp`,
`vk_record.cpp`), are env-gated and default-off. RTX 3060 Ti, seed 1337,
`--measure` scenario (c), 120 ticks:

| CA iterations recorded | `ca(...)` µs/tick |
|---|---|
| 0 | 0.099 |
| 6 | 17.819 |
| 27 | 67.137 |
| 54 | 122.502 / 128.099 (two runs) |

Least squares: **2.25 µs per recorded empty dispatch, +3.0 µs fixed** → the
full 54-iteration floor is **122–128 µs/tick, not 251**. §3.0's figure was
~2× high because the intercept absorbed the per-chunk term's curvature. Every
"floor share" percentage in §3.0 should be roughly halved. What did NOT change
is the shape: the cost is paid per *recorded* dispatch, so it is a CPU
record-time cost, which is why §3.4 had to be a CPU latch and why any successor
must be one too.

#### 3.2b The empty-phase fraction is ZERO in every real scenario

`--measure` gained MEASUREMENT 3 (`SANDVOX_PHASE_HIST=1`): each tick it reads
the dirty flags and every dirty chunk's voxels and asks, per colour c in 0..26,
whether any cell of colour c passes `matCanAct` (common.wgsl:118 — the
predicate `sim_step` itself early-outs on). This is measured on FRESH,
UNDILATED state, so it is a strict upper bound on any implementable mechanism.

| scenario | active chunks/tick | colours per dirty CHUNK | **empty phases / 27** |
|---|---|---|---|
| (b) ACTIVE | 3.0 | 19.61 | **0.000 (0.00%)** |
| (a) SETTLING | 25.0 | 25.72 | **0.000 (0.00%)** |
| (d) HEAVY | 44.0 | 20.45 | **0.021 (0.08%)** |
| (e) MINIMAL — ONE grain | 0.7 | 7.52 | 18.44 (68.31%) |

**The prior in §3.1 bug 2 was right.** A chunk is in the dirty list because
`markDirty` wakes every chunk a written cell BORDERS, and a woken chunk is a
16³ box of terrain: ONE dirty chunk already holds actionable cells of ~20 of
the 27 colours, and the union over two or more saturates. Over 190 active ticks
across the three real scenarios the union was empty at a colour **once**, on a
single HEAVY tick.

Scenario (e) MINIMAL was added specifically to give the idea its best possible
case: one sand voxel released into empty sky 40 cells above the terrain, every
40 ticks — a single acting cell in a dirty set of one or two nearly-all-air
chunks. Nothing the engine can do is smaller. Even there, the 68% collapses as
soon as the grain nears the ground (the distribution is `26 empty:30 ticks,
14–15 empty:24 ticks, 0 empty:7 ticks` — 26 while falling through sky, ~14 once
a terrain chunk is in the set).

#### 3.2c Conservatism eats even the MINIMAL case

The 68% is unreachable, because the decision is made on the CPU at record time
from a readback that is at least a tick old. Two dilations are mandatory:

1. **Movement.** Between the readback and the tick, cells move. The exact
   write-target set of `sim_step.wgsl` (self + `tryMove` + `transferLiquid` +
   `doReactions`' `faceDir` products) is 15 of the 27 offsets: self, the 6
   faces, and the 8 vertical diagonals. Dilating one occupied colour by that
   set gives 15 colours before anything else is considered — and there are TWO
   gravity substeps per tick, so composing it with itself reaches all 27.
2. **Dirty-set growth.** This is the killer, and it is §3.1's bug 2 restated at
   colour granularity: a chunk *entering* the dirty list brings its ENTIRE
   contents' colour set with it — terrain the previous tick's colour set never
   mentioned. Any tick on which the dirty set grows must fall back to all-27.
   Measured frequency of growth on dispatching ticks: **42/54 ACTIVE, 18/38
   SETTLING, 45/97 HEAVY, 11/60 MINIMAL**.

Plus §3.4's existing op/particle fallback, since ops, explosions and particle
reinsertion place matter at arbitrary colours.

Measured over only the ticks that actually dispatch work (so nothing here is
double-counting §3.4's win):

| scenario | movement dilation only | **+ dirty-set growth (sound)** |
|---|---|---|
| (b) ACTIVE | 0.00% | **0.00%** |
| (a) SETTLING | 0.00% | **0.00%** |
| (d) HEAVY | 0.00% | **0.00%** |
| (e) MINIMAL | 22.22% (3 model VIOLATIONS) | **17.78%** |

The ceiling on the whole idea is therefore `0.00% × 122 µs = 0 µs/tick` in
every scenario except a single grain falling through empty sky, where it is
`17.78% × 122 µs ≈ 22 µs` on 60 of 120 ticks — about 7% of that scenario's CA
time, for a per-phase readback, a new buffer, a `pass_table.def` row, and a
new class of hash bug.

**And the model was still not sound.** The harness cross-checks its own
prediction against the truth and counted **3 violations in 60 MINIMAL ticks**
even with both fallbacks in place — ticks where real work appeared at a colour
the model had proved empty. In a real implementation each one is a wrong world
hash. A correct version needs strictly more conservatism than was modelled
here, and the modelled version already wins nothing.

**Do not revisit at 5 cm.** Halving the voxel size makes events cover MORE
chunks, and a chunk's colour coverage is already saturated at 10 cm. This gets
monotonically worse, not better — unlike §3.1, which is "measured, not viable
yet".

#### 3.2d The lever that IS there: §3.4 is being held off by a 400-tick timer

The same instrumentation found a real one. In `--measure` scenario (b) ACTIVE,
**66 of 120 ticks have a completely EMPTY dirty set, yet `CA skipped on 0/120`**
— every one of those ticks records all 54 dispatches, the `compact` scan of
32,768 dirty flags and the args staging copy, and dispatches zero workgroups.
At the measured floor that is `66 × ~125 µs ≈ 8.3 ms` in a 120-tick window:
**~17% of all CA GPU time in the ACTIVE scenario, spent on nothing, on ticks
§3.4's mechanism was built to skip.**

The blocker is `particlesActive`, which `main.cpp` computes as
`everExploded && (tick - lastExplosionTick < 400 || Snap().particleCount > 0)`
and `EncodeTick` folds into `inputsThisTick`, resetting `lastDirtyTick_` every
tick and so preventing `settledProven_` from ever latching. **13.3 seconds of
wall clock after every single explosion, the settled-tick skip is off.** The
term is legitimately there — a particle rejoining the grid writes voxels and
marks `dirtyOut`, and the CPU learns of it only via a snapshot — but 400 ticks
is a blunt stand-in for "a snapshot old enough to be conclusive", which is
exactly the invariant `NoteSnapshot(snapTick, activeChunks)` already implements
via `snapTick >= lastDirtyTick_`.

The shape of the fix (NOT built, NOT measured): keep `particlesActive` as-is
for the particle passes — it must stay true or live particles stop simulating —
but stop feeding the 400-tick timer into the CA-skip decision. Instead bump
`lastDirtyTick_` on particle SPAWNS only, and require the clearing snapshot to
show `particleCount == 0` as well as `activeChunks == 0`. Then "no spawns since
snapTick, and that snapshot saw no particles and no dirty chunks" proves
nothing can write voxels, on exactly the machinery §3.4 already has. It is
hash-neutral by construction (a tick where nothing would be dispatched), so
acceptance is bit-identical `7cfa2420`, and the trap to watch is the one-tick
window between a resolve and the snapshot that reports it — a skip taken there
does not corrupt the world, it processes the chunk a tick LATE, which moves the
hash. HEAVY (23 empty ticks, 15 skips) and MINIMAL (59 empty, 56 skips) show
the latch working when nothing holds it off, so the residual there is small;
ACTIVE is where all the loss is.

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
  was a WebGPU limit that Vulkan deletes). Two gaps before pushing far:
  cascade **edit persistence** (currently regenerates from seed — player edits
  vanish from the horizon) and shift/fill bandwidth. `kFarShiftBase` retune
  needed when the window grows (already noted in port plan).
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

1. **Uniform-chunk histogram in `--measure`** (~20 lines): how many "fully
   full" chunks are single-word uniform vs mixed-material/stained. The last
   unknown in the 2048³ memory budget — sizes the page pool.
2. Occupancy re-measure at the grown window before committing (the plan
   already stages this).
3. Cell-mask win: `--measure` before/after on a scripted active scene.
4. Verify the 0.2 µs/chunk compute anchor with a controlled active-chunk-count
   scene (current figure is a one-measurement extrapolation).

## 6. Sequencing and dependencies

```
Vulkan port phases 3–6 (in flight)
  └─ phase 7 REWRITTEN: software page table + sentinels + pool (§1)
       └─ grow window to 2048³ (fresh measurements, §5.1-2 first)
            └─ kVoxelMeters 0.10 → 0.05 (the one-time retune, §2)
independent of the port (can run in worktrees NOW):
  - bulk promotion policy (§3.2) — game/phys files only
  - --measure histogram (§5.1)
  - near-field surface detail / subdiv-16 (§4) — WGSL + tuning only
after port phase 3b lands (simulation.cpp / pass_table free up):
  - cell-level active masks (§3.1)  ← do this one first; biggest single win
  - then temporal rate LOD (§3.5)
with phase 8:
  - async compute overlap (§3.6), settled-tick fixes (§3.4)
far-field extension (§4): independent, any time after render path exists
```

Constraints that outrank everything here, restated so no optimization erodes
them: rule 1 (bit-determinism: integer sim, stateless RNG, ≤1-cell writes, no
scheduling-dependent outcomes), rule 2 (cost scales with activity — every new
system needs a costs-nothing-idle state), rule 3 (all mutations through the
MutationQueue). Every hash-affecting change re-runs `--selftest`; hash-neutral
changes (masks, skip-encode, page table) must prove bit-identical hashes.
