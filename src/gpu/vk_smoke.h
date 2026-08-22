// vk_smoke.h — `--vk-smoke`, the quiet-world PINNED hash-sequence gate.
//
// Originally the phase-3b cross-backend proof (Dawn vs Vulkan, same seed, tick
// for tick). Dawn was removed 2026-08-22, so the reference is no longer a
// second backend but the PINNED sequence in vk_smoke.cpp — the values the
// cross-backend diff agreed on, which every phase since has reproduced
// byte-for-byte. Read that file's header for why this keeps (and in one
// respect widens) the regression coverage the diff provided.
//
// Two stages, because they fail differently and the difference localises the
// bug (barrier_graph §6.2's interpretation table):
//
//   (a) WORLDGEN. EncodeWorldgen + EncodeHashOnly. A mismatch here is not a
//       barrier bug at all: worldgen is one dispatch after seven fills, with
//       no loop and no indirect args. It means the SPIR-V differs from the
//       WGSL that produced the pin, or a buffer was not zero-initialised, or
//       a binding is wrong.
//
//   (b) TICKS. N ticks on the quiet world, hashes checked at ticks 1, 15, 30
//       and 50. The divergence PATTERN is the diagnosis:
//         - tick 1 differs        -> recording or the CA loop
//         - first hash tick (15)  -> the occupancy path
//         - drift appearing later -> a barrier race; rerun with
//                                    --barriers=sledgehammer to bisect, while
//                                    remembering §6.2 rates that as weak
//                                    evidence on a single GPU.
//
// Run with `--vk-validation`: synchronization validation is §6.2's PRIMARY
// missing-barrier detector and does not need a divergence to occur, which is
// what carries the barrier evidence now that nothing can disagree. A single
// message FAILS the run.
//
// A quiet world sounds like it exercises little. It exercises: five fills, the
// compaction dispatch, the argsStage->dispatchArgs copy, 54 indirect CA
// dispatches with a different dynamic passUBO offset each, compactNext, a
// second args copy, occupancyDirty, pick, farDown, and — every 15th tick — the
// whole-world occupancy+hash branch instead. That is every structural feature
// of the tick table except the particle and explosion chains.

#pragma once

namespace sandvox {

// Run the scenario, check it against the pinned sequence, and return a process
// exit code (0 = PASS).
// `lowPower` mirrors `--adapter low` (it selects among Vulkan physical devices).
// `sledgehammer` selects the §6.2 A/B oracle barrier mode.
// `validation` turns on VK_LAYER_KHRONOS_validation + synchronization validation.
int RunVkSmoke(bool lowPower, bool sledgehammer, bool validation,
               bool paged = false);

// `--vk-smoke-loud` — the ACTIVE-world pinned sequence (19 probes).
//
// WHY A SECOND SMOKE, AND WHAT IT ADDS
// ------------------------------------
// The quiet smoke above proves the STRUCTURE of the tick table: fills, compact,
// the indirect staging hops, 54 CA iterations with per-iteration dynamic
// offsets, both hash-tick branches, farDown. What it cannot reach is everything
// gated behind a condition that a quiet world never satisfies — and those are
// exactly the chains phase 3c added machinery for:
//
//   * `C_OPS` / `C_CELLS`   brush and exact-cell mutation (T10, T11)
//   * `C_EXP`               the explosion mark/apply split and its expMask
//                           (T12, T13) — the two-phase kernel whose RAW the
//                           tracker must derive rather than be told
//   * `C_PARTICLES`         the whole particle chain: args1, the pArgs staging
//                           copy, integrate (indirect), args2, the SECOND copy
//                           that overwrites args an in-flight indirect read
//                           already fetched (§7.2), drawArgs, resolve
//   * `C_SPAWN`             CPU particle spawns appending to the read page
//   * the readback ring     3 slots on borrowed fences, polled not blocked
//   * streaming             eviction (eager submit, fence wait) and procgen
//                           fill, through a REAL Stream + ChunkStore. The walk
//                           stays one-directional — a return leg's content
//                           depends on store policy that rides snapshot
//                           timing, not on barriers (see the note in
//                           vk_smoke.cpp); the store-hit round trip is proven
//                           by the save-load/region-store selftest gates.
//
// A hash sequence that holds its pinned values through all of that is a
// materially stronger statement than the quiet one, because every one of those
// paths is a place where a missing barrier has somewhere to hide. Hashes are
// checked at INTERVALS rather than only at the end: an end-only check cannot
// distinguish "never diverged" from "diverged and reconverged", and the tick
// at which a divergence first appears is the diagnosis (§6.2).
int RunVkSmokeLoud(bool lowPower, bool sledgehammer, bool validation,
                   bool paged = false);

}  // namespace sandvox
