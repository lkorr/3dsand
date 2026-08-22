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

}  // namespace sandvox
