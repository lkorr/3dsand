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
>
> **[MEASURED 2026-08-24] §3.0's 251 µs floor is ~2x high — it is 122–128 µs,
> directly measured (§3.2a). Per-colour-phase dispatch skipping, the obvious
> attack on that floor, is REFUTED (§3.2).**
>
> **[LANDED 2026-08-24] The floor's real lever was §3.2d, and it is BUILT:
> `particlesActive` no longer holds §3.4's settled-tick skip off for 400 ticks
> after every explosion. ACTIVE scenario `CA skipped on 0/120 → 45/120`,
> CA −16.7%, total sim GPU −13.2%, hash `7cfa2420` unchanged. Gate `ca-skip`.
> Note the culprit was NOT `main.cpp` but `NoteTickInputs(dirtiedNow)` in
> `src/test/support.cpp` — the path the GAME shares with the harness.**

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
SLOPE, not an intercept.

**Only `SANDVOX_CA_FORCE` survived the merge to main** (`simulation.cpp`,
default-off, reachable as an env var or programmatically via `SetCaForced` —
which is what the `ca-skip` gate uses as its oracle). It is hash-neutral: it
only ADDS dispatches that do nothing. `SANDVOX_CA_REPEAT` was DELETED from
`vk_record.cpp`: truncating the CA loop breaks the colour lattice and therefore
the world hash, and a live switch for that in the hot record loop is not a
thing rule 1 tolerates sitting in main. The numbers below stand; to re-measure
the slope, recover the knob from branch `perf/ca-phase-skip` and delete it
again afterwards. RTX 3060 Ti, seed 1337,
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

#### 3.2d LANDED 2026-08-24 — §3.4 was being held off by a 400-tick timer

**[BUILT AND MEASURED, branch `perf/ca-settled-particles`. Hash `7cfa2420`
unchanged. New gate `ca-skip`.]** The phase-histogram instrumentation that
refuted §3.2 found a real lever on its way past.

**The bug.** In `--measure` scenario (b) ACTIVE, **66 of 120 ticks had a
completely EMPTY dirty set, and `CA skipped on 0/120`** — every one of those
ticks recorded all 54 indirect dispatches, the `compact` scan of 32,768 dirty
flags and the args staging copy, and dispatched zero workgroups. The blocker
was `particlesActive`, which `main.cpp` computes as `everExploded && (tick -
lastExplosionTick < 400 || Snap().particleCount > 0)` and which `EncodeTick`
folded into `inputsThisTick`, re-stamping `lastDirtyTick_` on every tick so
`settledProven_` could never latch. **For 13.3 seconds of wall clock after
every single explosion, the settled-tick skip was off.**

The term was legitimately *about* something — `sim_particle`'s `resolve`
reinserts a voxel into the grid and calls `markDirtyNext`
(`sim_particle.wgsl:251,:274`), so it is a dirty-writer whose target the CPU
never chose — but 400 ticks is a blunt stand-in for "a snapshot old enough to
be conclusive", which is exactly the invariant `NoteSnapshot` already
implements via `snapTick >= lastDirtyTick_`.

**The conservatism rule that landed.** `particlesActive` still gates the
particle PASSES, unchanged — dropping it mid-flight would strand live
particles. It simply no longer speaks for the CA. In its place:

> A snapshot licenses the skip only when it shows `activeChunks == 0` **and**
> `particleCount == 0` **and** `snapTick >= lastDirtyTick_`, where
> `lastDirtyTick_` is stamped by every tick that carries an op, a cell op, an
> explosion, a shatter spawn or any MPM fluid — i.e. by every tick on which a
> particle can be CREATED.

Three lines of code: the particle conjunct in `Simulation::NoteSnapshot`, the
removal of `particlesActive` from `EncodeTick`'s `inputsThisTick`, and the same
removal from `SubmitTick`'s `NoteTickInputs` call.

**Why the reinsertion window is CLOSED, not merely narrow.** The trap flagged
when this was a proposal is that a skip taken between a particle landing and
the snapshot that reports it does not corrupt anything — it processes a dirty
chunk one tick LATE, and the hash moves. It cannot happen, because both
conjuncts come off ONE snapshot word captured at ONE point in the tick, and
that point is downstream of every writer:

1. `World::EncodeReadbacks` is recorded after the whole `PT_TICK` table, whose
   order is `... ca, particleIntegrate, particleResolve, occupancy ...`. So
   snapshot *S*'s dirty flags already carry `resolve`'s `markDirtyNext` for
   tick *S*. A landing is never invisible to the snapshot of its own tick.
2. `particleCount` is `counts[1 - page]` (`SubmitTick` passes
   `1 - sim.Page()` as `particleLivePage`) — the count `integrate` built at *S*
   and `resolve` then consumed at *S*, i.e. the exact population resolve ran
   over. `particleCount(S) == 0` therefore means resolve at *S* touched
   nothing AND the read page for *S+1* is empty, so `integrate` at *S+1*
   dispatches nothing and appends nothing. **Zero is self-propagating**, which
   is what makes it an induction base rather than a sample.
3. A particle that lands at *S* is still COUNTED at *S* (`resolve` clears its
   flags but never decrements `counts`), so the landing tick reports
   `particleCount > 0` on top of `activeChunks > 0`. The conjunct errs one tick
   in the safe direction at exactly the moment that matters.
4. Every spawn path is inside the same word. Explosion ejecta
   (`sim_explode.wgsl:153,:175`) and CPU shatter spawns
   (`sim_particle.wgsl:97`) append to the READ page and fly the same tick; MPM
   splash droplets append to the WRITE page (`sim_fluid.wgsl:924,:1116`) from
   tables recorded after `PT_TICK`. There is no spawn whose product is
   invisible to the snapshot of its own tick.

A stale non-zero count can only COST a skip, never license one — the safe
direction — and it is bounded, because the counts are zeroed per tick while the
pipeline runs, so the population genuinely reaches 0 a couple of ticks after
the last particle dies. This is the same evidence and the same shape as
`PageTable::ApplyParticleShell`'s off condition (`pagetable.cpp:643`), which is
the precedent for trusting `particleCount` at all.

**Measured** (RTX 3060 Ti, seed 1337, 120 ticks/scenario). The control arm is
the SAME binary with `SANDVOX_CA_FORCE=1`, which records the CA on every tick —
bit-for-bit the recording the old code produced in ACTIVE, where the old latch
was pinned open. Same binary, back-to-back runs, both through `scripts/run.sh`:

| (b) ACTIVE, µs/tick | before (forced) | after | delta |
|---|---|---|---|
| `CA skipped on N/120` | **0** | **45** | +45 |
| `ca(54 colour x substep)` | 389.603 | 324.626 | **−64.98 (−16.7%)** |
| `prep(mutate+explode+compact)` | 5.047 | 3.471 | −1.58 |
| ticks on which `ca` was recorded | 120 | **75** | −45 |
| TOTAL GPU compute | 520.778 | 452.245 | **−68.53 (−13.2%)** |
| wall clock (ms, submit+WaitIdle) | 1.753 | 1.701 | −0.05 |

Cross-check on the cost model: the same pair of runs reads the content-free
cost directly off scenario (c) SETTLED, where the skip is the only difference —
164.825 µs/tick forced vs 27.702 skipped, i.e. **137 µs per empty recorded
tick** for `ca` + `prep` together, squarely on §3.2a's 122–128 µs for the 54
`ca` dispatches alone. 45 such ticks predicts ~51 µs/tick; 67 was measured.
The residual is inside this harness's run-to-run noise — scenario (a) SETTLING
is untouched by this change and still moved 591.9 → 624.1 µs (5.5%) between the
same two runs.

**Why 45 skips and not 66.** The remaining 21 are snapshot latency, not a
partial fix: ACTIVE detonates 6 times in 120 ticks and each blast costs ~3–4
ticks before a snapshot stamped at-or-after it can arrive and report zero.
`75 recorded − 54 genuinely-working = 21`. Buying those back means predicting
the settle, which is exactly the unsound direction §3.2c already refuted.

**Unchanged, as predicted:** (d) HEAVY 1252.8 → 1251.1 µs and (c) SETTLED
(already 120/120 skips) — both pass `particlesActive = false`, so they never
had the bug.

**What it cost.** `NoteSnapshot` gained a parameter; `Simulation::SetCaForced`
(and `SANDVOX_CA_FORCE=1`) was added as a test/measurement switch. No new
buffer, so no `pass_table.def` row. One new gate, `ca-skip`, ~30 s.

**The gate, and why a hash check alone would not have been enough.** The
failure mode is "a chunk was processed one tick late", which a single run
cannot see — it produces a *self-consistent* history that is simply the wrong
one, and `--gate determinism` compares two runs of the same code. So `ca-skip`
(`src/test/selftest_ca.cpp`) runs one scripted history TWICE — settle 300
ticks, detonate r=14 at the surface, let the ejecta fly and land, hold the
particle pipeline open for 160 ticks past the blast the way the game does, and
settle — hashing **every** tick, once with the latch live and once with
`SetCaForced(true)`. Forcing can only add dispatches whose indirect count is
zero, so the two hash sequences must be identical; a latch that clears one tick
early diverges immediately, and the gate prints the tick and both hashes.

It also asserts the optimization itself, which is the part that would rot
silently: **skips taken on ticks where `particlesActive` was true**. Under the
old code that count is 0 by construction, because `particlesActive` was in
`inputsThisTick`. Result:

```
ca-skip: PASS (hash 619a6de5 identical over 220 scripted ticks skip-on vs
  forced, 151 / 220 skipped (91 of them with the particle pipeline live, first
  69 ticks after the blast), forced run skipped 0, 100 skips over the settle,
  0 particles alive, 0 page faults)
```

The 91 is the load-bearing number and the 151 is not: the skips are counted
over the scripted window only because the settle phase's count depends on what
the previous gate left in the page table and swung 3x between a standalone and
a full-suite run. The hash sequence and the 91 did not move.

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
   was never the bottleneck it appeared to be. **Extended 2026-08-24 (§3.2d):**
   the latch was disabled for 400 ticks after every explosion by
   `particlesActive`; replacing that timer with a `particleCount == 0` conjunct
   on the licensing snapshot took the ACTIVE scenario from 0/120 skips to
   45/120 and its total sim GPU time down 13.2%, hash unchanged.
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
