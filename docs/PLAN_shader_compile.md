# PLAN: shader pipeline compile time (2026-09-07)

**Problem.** A cold pipeline build is ~17 min, serial, on the boot thread. The cost is
`vkCreateComputePipelines` in the NVIDIA driver, one entry point at a time. Measured
(SANDVOX_SHADER_TIMING, post-unroll-fence): worldgen `main` 80 s, `list` 75 s, `far` 746 s,
`fardown` 98 s. Every worldgen shader edit invalidates the per-CWD caches
(`shader_cache/` keys on WGSL source, `sandvox_pipeline_cache.bin` on the driver blob),
so worldgen iteration pays the full bill every time.

**Prior art already in the tree (uncommitted on main as of this writing):** the
`unrollFence()` trick in worldgen.wgsl (made `far` compile at all) and two mid-build
`SavePipelineCache` calls in `Simulation::BuildPipelines` (a killed boot keeps the
worldgen ISA). Carried by agents via `build/main_wip_2026-09-07.patch`.

## Audit of the research recommendations

| # | Idea | Verdict |
|---|---|---|
| 1 | Thread-pool pipeline creation | **DO.** Serial today; only blocker is `Backend::pipelines_` (rhi_vulkan.cpp:1408) being unsynchronized. One shared `VkPipelineCache` is internally synchronized per spec. Wall clock becomes max(entry) ≈ far. |
| 2 | Defer far/fardown off the critical path | **DO.** worldgen.wgsl:5017 documents cascades as render-only derived data, no determinism attaches. Time-to-playable after a worldgen edit → ~80-100 s. |
| 3 | spirv-opt between Tint and the driver | **EXPERIMENT.** SPIRV-Tools is in the Dawn checkout. Must key the SPIR-V disk cache on the opt flag. Adopt as default only if far improves ≥25 %. |
| 4 | `SANDVOX_PIPELINE_CACHE` path override | **DO.** Path is hardcoded CWD-relative (rhi_vulkan.cpp:354). Kills the per-worktree cold compile. Atomic save (temp+rename); processes sharing it race benignly (last writer wins a superset after load+create). |
| 5-7 | far kernel split, flatten PondSet/LandCol by-value args, dedupe genColumn inlines | **REAL but DEFERRED** — agent-9ef45d holds a live claim on worldgen.wgsl (P-G/P-I). See "Package C" below; it is the follow-up for the next worldgen owner. Added: merge `main`+`list` entries (differ by one indirection; ~75 s of duplicate compile). |
| 8 | Compile-time gate | **MODIFIED.** Wall-clock ceilings only fire on cold caches. Instead: SPIR-V instruction-count ceiling for worldgen.wgsl in `check_shaders.sh` (deterministic, runs on every shader edit), plus per-pipeline compile ms always recorded in `build/last_run.json`. |
| 9 | DISABLE_OPTIMIZATION bit on far | **SKIP.** NVIDIA largely ignores it; costs runtime perf if honored. |
| 10 | File NVIDIA repro | Optional, not engineering work here. Forum thread: /t/very-slow-pipeline-creation-failure/252439. |

## Packages

### A — fast boot (C++, worktree `shader-compile-a`)
1. `SANDVOX_PIPELINE_CACHE` env override for `pipelineCachePath_`; atomic temp+rename save.
2. Thread-pool the `MakeComputePipeline` calls in `Simulation::BuildPipelines` (and
   `EnsureRenderPipelines` if trivially reachable): mutex on backend pipeline
   bookkeeping; keep `--shader-stats` (`captureStats_`) path serial.
3. Deferred far/fardown: futures; `EncodeFarFill`/FarDown early-out until ready; full
   cascade refill on completion; `Simulation::WaitForAllPipelines()` called by
   selftest/`--shot`/`--verify`/`--render-budget` paths; F5 rebuild joins first;
   `SavePipelineCache` when the deferred set completes.
4. Per-pipeline compile ms always into `build/last_run.json`.

Hash must NOT move (no sim-state change). Verification: one build, `--gate determinism`,
one `--verify` with a shot frame, ONE cold-dir SANDVOX_SHADER_TIMING run at the end
quoting "interactive after X s / far ready after Y s".

### B — smaller SPIR-V — **LANDED 2026-09-07, ADOPTED (branch `shader-compile-b`)**
1. `SPIRV-Tools-opt` linked (already in the Dawn checkout, no new fetch);
   `vk_spirv.cpp` runs `RegisterPerformancePasses(preserve_interface=true)`
   between Tint and `vkCreateShaderModule`, behind `SANDVOX_SPIRV_OPT`.
   Optimizer output is validated; any failure falls through to the raw Tint blob
   with a stderr note, so it cannot take a boot down. The recipe id is mixed into
   the `shader_cache/` key so optimized and unoptimized blobs cannot collide.
2. **The measurement.** Two cold arms, fresh CWD each (cold Tint cache AND cold
   driver pipeline cache — both are per-CWD), `--frames 60`,
   `SANDVOX_SHADER_TIMING=1`, one build, RTX 3060 Ti / NVIDIA:

   | pipeline | opt OFF | opt ON | of which spirv-opt | delta |
   |---|---:|---:|---:|---:|
   | worldgen `main` | 84.3 s | 37.4 s | 22.6 s | -55.6 % |
   | worldgen `list` | 69.2 s | 36.2 s | 21.8 s | -47.7 % |
   | worldgen `pagefill` | 0.09 s | 0.12 s | 0.01 s | — |
   | worldgen **`far`** | **622.1 s** | **170.7 s** | 98.6 s | **-72.6 %** |
   | worldgen `fardown` | 85.6 s | 54.7 s | 38.2 s | -36.1 % |
   | **worldgen total** | **861.2 s** | **299.0 s** | 181.2 s | **-65.3 %** |
   | all 58 compute pipelines | 864.2 s | 318.4 s | ~188 s | -63.2 % |
   | render pipelines | 32.9 s | 29.5 s | 12.9 s | -10.3 % |

   >=25 % on `far`, so **the default is ON**; `SANDVOX_SPIRV_OPT=0` opts out.
   The `determinism` gate returns the same hash (`ef83fd47`) with the optimizer
   off and on, measured on the same tree — the optimizer is not a hashed-state
   change. (Both arms also report the same pin move off `44fa72cb` and the same
   24 page faults; those come from the carried main WIP, not from this package,
   which is exactly what the opt-OFF control on the same tree establishes.)

   The result is counter-intuitive and is the finding worth keeping: the
   optimizer makes the module far BIGGER (`far`: 66,416 -> 1,607,872 words,
   inline-exhaustive expanding genColumn's several call sites) and the driver
   still compiles it 3.6x faster. NVIDIA's front end is paying for inlining and
   for promoting Tint's function-scope `var` load/store soup, not for
   instruction count. Corollary for package C: item 4 (reduce genColumn inline
   copies) is aimed at a cost the driver was not actually charging — items 1 and
   3 are unaffected by this.

   Open follow-up: **181 s of the remaining 299 s is now spirv-opt itself**, and
   it runs on the boot thread, serially, once per entry point. Package A's thread
   pool parallelises it for free. A leaner recipe than full `-O` (the `legal`
   arm, already wired behind `SANDVOX_SPIRV_OPT=legal`) is the other half and is
   unmeasured.
3. Guardrail: worldgen.wgsl SPIR-V instruction-count ceiling in
   `check_shaders.sh` (`far` and `fardown`, 25,000 each = ~1.5x the 16.8 k
   measured 2026-09-07; `main` 16.5 k and `list` 16.6 k track them). Note it is a
   SIZE proxy only — `far` and `fardown` are the same size and cost 622 s and
   86 s. What it catches is worldgen quietly doubling, which is the P-F failure
   that shipped.

### C — worldgen kernel restructure (DEFERRED, needs worldgen.wgsl claim)
Blocked behind env-pg-pi (agent-9ef45d). For whoever owns worldgen.wgsl next, in
expected-payoff order; each item moves no behaviour, only compile time, but the WGSL↔C++
mirror token-compare (`check_invariants.py`) and hash-neutrality must be re-verified per
step:
1. Split `far` into two entry points: the sweep and the edit-patch loop (the patch loop
   duplicates a full genColumn+farSurfaceMat inline copy; driver cost is superlinear in
   entry size, and with Package A the halves compile in parallel). Est. far → ~100-200 s.
2. Merge `main` and `list` into one entry (uniform-driven indirection). Saves ~75 s.
3. Pass `PondSet`/`LandCol` via `ptr<function>` everywhere; stop embedding PondSet in
   LandCol. This is the documented NVIDIA pathology (large by-value aggregates).
4. ~~Reduce genColumn/genCellIn inline copies~~ — DEMOTED by package B's measurement
   (see above): spirv-opt's inline-exhaustive makes the module 24x LARGER and the driver
   compiles it 3.6x faster, so inline-copy count is not what the driver charges for.
   Only worth revisiting if it shrinks the 181 s spirv-opt pass itself.

## Measured end state (A+B merged on `shader-compile-fast`, 2026-09-07)
Cold CWD (both caches wiped), renamed exe, RTX 3060 Ti:
- **Cold boot**: frame loop entered 64.7 s, frame 1 at 113.6 s (render pipelines
  45.7 s, parallel but CPU-starved by the deferred far/fardown optimizer threads),
  far cascades ready 413.9 s. Was ~17 min before any frame.
- **Worldgen-edit iteration loop** (everything else warm): **frame 1 at 42.5 s**,
  far cascades refreshed at 292.9 s. This is the number the plan existed for.
- F5 with unchanged shaders: worldgen pipelines from cache in ~1-3 ms.
- Determinism: twice-run PASS on the merged tree; the pin move (44fa72cb →
  ef83fd47) and 24 selftest page faults belong to the carried main WIP's
  map.svmap edit (established by package A's worldmap-revert differential and
  package B's opt-off control), so the pin is the worldgen owner's to move.

Open follow-ups:
- spirv-opt on the deferred pair runs ~3x slower than standalone (far 302-349 s
  vs 98.6 s) from CPU contention with boot — the unmeasured `legal` recipe, or
  a lower thread count for the deferred set, would shrink far-ready.
- Package C (far split + main/list merge + by-value flattening) still applies
  and now attacks both the driver time AND the spirv-opt time.
- `scripts/run.sh`'s stale-lock handling killed this session's wrapper shells
  three times while a long compile held sv-gpu-lock (the exes survived,
  orphaned). Long-run holders need a keepalive the reaper respects.

## Package C — WRITTEN 2026-09-07, NOT YET MEASURED (branch `worktree-agent-a665bf2ee69ebad8e`)

Items 1 and 2 are implemented and compile-clean; item 3 is deliberately **not
done** and the reason is a measurement, below. **Nothing here has been run yet**
— the worktree's C++ compiled (981/981, sccache 94.4 % hit) but the link sat
queued on `sv-gpu-lock` for ~90 minutes behind a `sandvox.exe` running out of the
main checkout, so there is no determinism line, no far screenshot and no
`pipelineCompileMs` table. **That is the whole of what is left.** See "How to
finish it" at the bottom.

### Item 1 — `far` split into `far` (sweep) + `farpatch` (edit patch). DONE.

The patch loop used to sit at the bottom of `far` behind an in-kernel
`storageBarrier()`. It is a SECOND full `genColumn` inline copy, and through
`farBlockerBitAt` it carries four more `farColTop`/`landColumn` copies that the
sweep's hoisted `tops`/`btops` form does not have. Two reasons to separate them:
NVIDIA's front end charges superlinearly in entry-point size, and package A's
`PipelineBuildPool` compiles separate entry points in PARALLEL — so the wall
clock becomes max(sweep, patch) rather than one bigger whole.

- `assets/shaders/worldgen.wgsl`: new `@compute fn farpatch`, same
  `workgroup_size(64)`, same `D_FARCOUNT` extent, so `wg.x` still indexes
  `farPatch`'s header pairs. No early return on `pCnt` (it comes from a storage
  buffer; a barrier under it would fail WGSL's uniformity analysis) — with
  `pCnt == 0` the closing `farOcc` update is the exact identity on the stored
  word.
- **`farOcc` is now published in two halves.** `far` stores
  `farOccPack(min(count, CHUNK_VOL), top)`; `farpatch` read-modify-writes
  `farOccPack(min(cur.count + pnz, CHUNK_VOL), max(cur.top, patchTop))`. Only
  one workgroup ever touches a given slot, in either dispatch, so this is the
  same arithmetic the merged entry did in registers, and it errs in the same
  conservative-high direction.
- **The in-kernel `storageBarrier()` is now a pass-table edge.** Two rows on
  `PT_FARFILL` (`farFill`, then `farPatchFill`), both declaring `A(FarVox)
  A(FarOcc)`, so `vk_record` generates it. `R(FarPatch)` moved off the sweep row
  onto the patch row — the sweep no longer reads that buffer at all.
- C++: `Pipe::FarPatchFill` + `PIPE_FAR_PATCH_FILL`, `Simulation::farPatchFill_`,
  `FarPipelines::patch`. The deferred far build gets a **third thread** (one per
  entry point) — the split only pays if the halves compile concurrently.
  `PublishFarPipelines` checks all three; `EncodeFarFill` records both rows and
  refuses to record either until both exist ("the sweep alone would write
  pristine procgen over cells the player has edited, which is a WRONG horizon
  rather than a missing one").
- `check_shaders.sh` guards `farpatch` with its own 25,000-instruction ceiling.

SPIR-V instruction counts (Tint, pre-spirv-opt), before → after:

| entry | before | after |
|---|---:|---:|
| `far` (sweep + patch in one) | 16,778 | **15,578** |
| `farpatch` | — | **15,502** |
| `fardown` | 16,767 | 15,915 |

The counts barely move because Tint emits `genColumn` and friends as real
functions and shares them across entry points; the split is aimed at the
DRIVER's inliner, which is where package B measured the cost to be. That is
exactly why this number is not the result and the `pipelineCompileMs` table is.

### Item 2 — by-value aggregates flattened to `ptr<function>`. PARTLY DONE.

- **`Col` by pointer** through `genCellIn` / `genCellCol` / `farSurfaceMat`.
  `genCellIn` destructures into scalars in its first twelve lines either way, so
  nothing downstream changes shape. It is inlined into every worldgen entry
  point, and in the far sweep's 16×4 cell loop it was three aggregate copies per
  cell (`cols[b]` → `genCellCol` → `farSurfaceMat`); it is now one.
- **`LandCol` as an OUT-PARAMETER** of `landColumnBare` / `landColumn` instead
  of a return value. LandCol is ~65 words (fourteen column fields plus an
  embedded `PondSet`) and `landColumnBare` is inlined five to nine times inside
  the far entries alone (`farColTop` → `landColumn` → `landColumnBare`, and
  again through `sitePadAt`). The caller's `var L : LandCol` is zero-initialised
  exactly as the old local was, so the lab branch's early return leaves the same
  values behind it. Safe for `check_invariants.py`: `landColumnBare` /
  `sitePadAt` / `landColumn` live in the `landheight` mirror block, which is
  compared by INTEGER LITERAL only.
- **NOT done: `PondSet` by pointer, and un-embedding `PondSet` from `LandCol`.**
  Two separate reasons, both worth writing down:
  - `pondScan` / `pondCover` / `pondNear` / `pondCovers` / `setPush` / `inDisc`
    / `shoreD2` all live inside the `height` MIRROR block, which
    `check_invariants.py` compares as a FULL TOKEN STREAM against `world.cpp`.
    `ptr<function, PondSet>` and `(*s).d0` add tokens (`ptr`, `<`, `function`,
    `>`, `*`) that the C++ side has no counterpart for, so the shader half
    cannot move without moving `world.cpp` AND teaching the normaliser about
    pointer syntax. That is its own change with its own risk, not a rider.
  - Un-embedding `ponds` from `LandCol` looks free and is not: the only external
    consumer is `plantSiteAt`'s `undergrowthSite(cx, cz, seed, &L.ponds)`, and
    removing the field makes that site call `pondScan` itself — which is a
    THIRD `pondScan` inline copy (`plantColumnAt` calls `plantSiteAt` twice), in
    the file whose header comment records that inlining `pondRoll` too many
    times "took the driver's compile of this kernel from minutes to never". The
    out-parameter above already removes the aggregate COPY without adding a
    scan.

### Item 3 — merge `main` + `list`. **SKIPPED, and the plan's estimate is stale.**

The ~75 s in the audit table is the SERIAL, pre-spirv-opt number. Package B
measured `main` at 37.4 s and `list` at 36.2 s, and package A compiles them
**concurrently in batch A's pool** — so batch A's wall clock is
max(main, list, pagefill) ≈ 37 s whether they are one entry or two. Merging buys
~36 s of one worker thread, not 36 s of anybody's wall clock; the second-order
win (less CPU contention with the deferred set's spirv-opt threads) is real but
unquantified.

Against that: the two entries differ by `gPtKernel = PT_K_GENLIST`, the
`wg.x >= T.genCount` guard and the `genList[wg.x]` indirection, and the
discriminator has to be a uniform. `T.genCount` cannot serve — `EncodePageFill`'s
comment records that the tick UBO's `genCount` holds the PREVIOUS tick's value on
paths that do not write it, which cost a debugging cycle and 2,114 chunks of
stone once already. So it needs a NEW `TickParams` field, mirrored in `world.h`
and `common.wgsl`, plus deleting `Pipe::WorldgenList` and adding a spurious
`R(GenList)` to the whole-world row. A new uniform field is the one change in
this package that can move the world hash if the two struct layouts disagree.

**Verdict: not worth it as written.** Revisit only if the deferred set's
spirv-opt CPU contention (the open follow-up above) turns out to be what bounds
far-ready.

### Found on the way: `check_pass_table.py` had been checking NOTHING

It scrapes the pipeline → (shader, entry) mapping out of `BuildPipelines` so a
kernel repointed at a different entry cannot keep the old row's R/W set. Package
A replaced `mStep = mod("sim_step.wgsl")` with the threaded out-parameter form
`mod(&mStep, "sim_step.wgsl")`, which the scraper's regex does not match — so it
matched nothing, reported "could not scrape any pipeline -> (shader, entry)
mapping", and walked zero kernels. It now accepts both shapes, resolves the
deferred far build's lambda-local `layout`/`module` aliases, and maps
`r.fill`/`r.patch`/`r.down` onto the members `PublishFarPipelines` moves them to.

With the scraper working it immediately caught four missing declarations on the
MPM `fluidG2p` row: `R(Voxels)`, `R(Materials)`, `R(PageTable)` (g2p reads the
live grid for its solid collision) and `A(FluidArgsStage)` where the row said
`R` (g2p atomicAdds the splash counter). Declared, in the safe direction. **This
is a missing-barrier class of bug and it is not part of package C** — it is
listed here because this is where it was found.

### How to finish it (the ONE remaining run)

Nothing is verified. In the worktree:

```bash
bash scripts/build.sh                                   # link only; the objects are cached
cp build/Release/sandvox.exe build/Release/sandvox_pkgc.exe
SANDVOX_SHADER_TIMING=1 bash scripts/run.sh ./build/Release/sandvox_pkgc.exe \
    --verify determinism --shot-frames screenshot_far > build/pkgc_verify.log 2>&1
```

Run it from the WORKTREE's cwd: the pipeline and SPIR-V caches are per-CWD, so
that first worldgen compile is cold, which is the number this package exists to
produce. What to read out of `build/last_run.json` and the log:

1. `determinism` twice-run PASS, and the hash equal to **`ef83fd47`** (what main
   measures today; the pin in `baseline.json` may still read `44fa72cb` — ignore
   the pin, compare to `ef83fd47`). This package changes no hashed state, so a
   moved hash here is a BUG, not a notification.
2. `pipelineCompileMs` for `farFill` and `farPatchFill` separately, against
   package B's `far` = **170.7 s** cold with spirv-opt. The claim to test is
   `max(farFill, farPatchFill) < 170.7 s`; their SUM being larger is fine and
   expected (two entries, two spirv-opt passes).
3. `farReadyMs` against **413.9 s** and `interactiveReadyMs` against 64.7 s.
4. **The far screenshot, with eyes on it.** The far cascade is NOT in the world
   hash, so a hash-neutral kernel split can still leave the horizon blank or
   mis-shaded and every gate stays green. This is the only check that covers the
   `farOcc` two-half publication and the pass-table barrier that replaced the
   in-kernel `storageBarrier`.

Not verified, therefore not merged. Pre-existing and NOT from this package:
`check_invariants.py` fails on a stale `tests/env_predictions.json`
(`biomesHash c1481fcd` vs `73dc273a`), inherited from main's env-pg-pi merge.
