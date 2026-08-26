# Smoke probe history

The `smokeQuiet` and `smokeLoud` arrays in `tests/baseline.json` are the pinned
hash sequences for `--vk-smoke` and `--vk-smoke-loud`. This file preserves the
provenance and interpretation that the C++ comments used to carry.

## Origin

- Quiet probes: established in commit 10156bb under the cross-backend diff
  (Dawn vs Vulkan agreed on these values).
- Loud probes: established in commit c0cc28f under the same diff.

Dawn was removed 2026-08-22; the pinned values ARE the reference now.

## Rebaseline history

Each sanctioned content change re-records the probe tables. The worldgen row
(tick 0, hash `f97ba745`) has been byte-identical across every rebaseline — it
is the standing check that a change is CONTENT and not codegen/zero-init/binding.

1. **WP5 flip** (fluidExciteMode 0→1): world hash 7b01cfd8 → 58b27f33.
2. **Levelling pass** (dc666ada → 882a30f3): sim_step.wgsl filmPressed +
   bridgeLevel, sim.liquidMinFilm back to 1, seam surface-step excite trigger.
3. **Wind flip** (882a30f3 → 47dd1520): sim.windMode 0→1. Quiet moved from
   tick 1 (worldgen's powders landing differently under drift bias). Settled
   matter is untouched (the `wind` gate asserts this).
4. **Drag ramp** (sim.windDragRef): loud moves from tick 15. Worldgen
   matches bitwise. Content, not binding — the ramp lives in two sim kernels.
5. **Gas vertical model**: loud moves from tick 52 (first fire puts smoke in
   the air). Quiet unmoved (no gas in a quiet world). Ticks 15-47 are powder
   settling — untouched.

## What the two scenarios separate

- **Quiet** (50 ticks, 5 probes): fills, compaction, argsStage copy, 54
  indirect CA dispatches, compactNext, occupancyDirty, pick, farDown, and the
  hash branch. Everything STRUCTURAL in the tick table.
- **Loud** (120 ticks, 19 probes): brush/cell mutations, explosion mark/apply,
  the particle chain, the readback ring, and an 8-shift streaming walk. The
  ticks where these overlap are where a missing barrier would show.

## Interpretation of a mismatch

- **Worldgen** differs → SPIR-V, zero-init, or binding change.
- **Tick 1** differs → recording or the CA loop.
- **First hash tick (15)** differs → the occupancy path.
- **Drift appearing later** → possible barrier race. Rerun with
  `--barriers=sledgehammer` (§6.2 rates this as weak single-GPU evidence).

Always run with `--vk-validation` — synchronization validation is the PRIMARY
missing-barrier detector.
