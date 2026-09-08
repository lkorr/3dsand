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
4. Reduce genColumn/genCellIn inline copies (a helper does NOT dedupe — every call site
   inlines; only a rolled loop over a worklist dedupes). `far` currently reaches 3-4 copies.

## Expected end state
- A lands: worldgen edit → playable in ~80-100 s, far terrain pops in ~12 min later;
  full cold build bounded by `far` alone (~750 s), not the 17-min sum.
- B lands (if the experiment wins): the 750 s bound itself shrinks.
- C lands (later): far ≈ fardown ≈ 100 s → whole cold boot ~2 min, and with A's
  parallelism, iteration on worldgen becomes ~100 s wall.
