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
               bool paged = false, bool rebaseline = false);

// `--vk-smoke-loud` — the ACTIVE-world pinned sequence (19 probes).
// Probe tables now live in tests/baseline.json; see tests/SMOKE_PROBES.md.
int RunVkSmokeLoud(bool lowPower, bool sledgehammer, bool validation,
                   bool paged = false, bool rebaseline = false);

}  // namespace sandvox
