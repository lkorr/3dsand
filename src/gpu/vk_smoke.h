// vk_smoke.h — `--vk-smoke`, the cross-backend world-hash comparison.
//
// THE PHASE 3b EXIT PROOF, and the port's first real determinism evidence.
//
// Dawn's auto-generated barriers ARE the reference implementation of
// docs/vulkan_barrier_graph.md (§6.3). So the strongest single test of the
// generated-barrier recorder is not "does it run" — it is whether a world
// simulated through it hashes identically to the same world simulated through
// Dawn, from the same seed, tick for tick.
//
// Two stages, because they fail differently and the difference localises the
// bug (barrier_graph §6.2's interpretation table):
//
//   (a) WORLDGEN. EncodeWorldgen + EncodeHashOnly on both backends. A mismatch
//       here is not a barrier bug at all: worldgen is one dispatch after seven
//       fills, with no loop and no indirect args. It means the SPIR-V differs
//       from the WGSL Dawn ran, or a buffer was not zero-initialised, or a
//       binding is wrong.
//
//   (b) TICKS. N ticks on the quiet world, hashes compared at ticks 1, 15, 30
//       and 50. The divergence PATTERN is the diagnosis:
//         - tick 1 differs        -> recording or the CA loop
//         - first hash tick (15)  -> the occupancy path
//         - drift appearing later -> a barrier race; rerun with
//                                    --barriers=sledgehammer to bisect, while
//                                    remembering §6.2 rates that as weak
//                                    evidence on a single GPU.
//
// A quiet world sounds like it exercises little. It exercises: five fills, the
// compaction dispatch, the argsStage->dispatchArgs copy, 54 indirect CA
// dispatches with a different dynamic passUBO offset each, compactNext, a
// second args copy, occupancyDirty, pick, farDown, and — every 15th tick — the
// whole-world occupancy+hash branch instead. That is every structural feature
// of the tick table except the particle and explosion chains.

#pragma once

namespace sandvox {

// Run the comparison and return a process exit code (0 = PASS).
// `lowPower` mirrors `--adapter low` on BOTH backends.
// `sledgehammer` selects the §6.2 A/B oracle barrier mode on the Vulkan side.
// `validation` turns on VK_LAYER_KHRONOS_validation + synchronization validation.
int RunVkSmoke(bool lowPower, bool sledgehammer, bool validation);

// `--vk-smoke-loud` — PHASE 3c's determinism acceptance evidence.
//
// WHY A SECOND SMOKE, AND WHAT IT ADDS
// ------------------------------------
// The quiet smoke above proves the STRUCTURE of the tick table: fills, compact,
// the indirect staging hops, 54 CA iterations with per-iteration dynamic
// offsets, both hash-tick branches, farDown. What it cannot reach is everything
// gated behind a condition that a quiet world never satisfies — and those are
// exactly the chains phase 3c adds machinery for:
//
//   * `C_OPS` / `C_CELLS`   brush and exact-cell mutation (T10, T11)
//   * `C_EXP`               the explosion mark/apply split and its expMask
//                           (T12, T13) — the two-phase kernel whose RAW Dawn
//                           inserted for free
//   * `C_PARTICLES`         the whole particle chain: args1, the pArgs staging
//                           copy, integrate (indirect), args2, the SECOND copy
//                           that overwrites args an in-flight indirect read
//                           already fetched (§7.2), drawArgs, resolve
//   * `C_SPAWN`             CPU particle spawns appending to the read page
//   * the readback ring     3 slots on borrowed fences, polled not blocked
//   * streaming             eviction (eager submit, fence wait) and procgen
//                           fill, through a REAL Stream + ChunkStore on both
//                           backends (phase 4a: one World, one driver). The
//                           walk stays one-directional — a return leg's
//                           content depends on store policy that rides
//                           snapshot timing, not on barriers (see the note in
//                           vk_smoke.cpp); the store-hit round trip is proven
//                           by the save-load/region-store gates on
//                           `--selftest --backend vulkan`.
//
// A hash that matches across backends through all of that is a materially
// stronger statement than the quiet one, because every one of those paths is a
// place where a missing barrier has somewhere to hide. Hashes are compared at
// INTERVALS throughout rather than only at the end: an end-only comparison
// cannot distinguish "never diverged" from "diverged and reconverged", and the
// tick at which a divergence first appears is the diagnosis (§6.2).
int RunVkSmokeLoud(bool lowPower, bool sledgehammer, bool validation);

}  // namespace sandvox
