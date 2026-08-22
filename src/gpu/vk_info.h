// vk_info.h — the `--vk-info` headless smoke mode (Vulkan port phase 3a).
//
// This is the phase-3a EXIT PROOF. Phase 3a builds the Vulkan foundations —
// device, memory, WGSL->SPIR-V, pipelines — but deliberately executes no sim
// work (that is 3b: command recording and barrier generation). Foundations that
// are never exercised are foundations nobody has tested, so `--vk-info` runs
// every one of them once and says PASS or FAIL:
//
//   * create an instance and device, and PRINT THE CAPABILITY RECORD phase 7's
//     sparse-residency payoff depends on (sparseBinding, sparseResidencyBuffer,
//     residencyNonResidentStrict, maxStorageBufferRange, timestamps). Those
//     numbers are a deliverable in themselves — they decide whether the sparse
//     plan is viable on this hardware.
//   * compile EVERY WGSL shader the engine loads, through Tint, to SPIR-V.
//   * create every compute pipeline and descriptor set layout.
//   * allocate buffers through VMA, run ZeroInitAll(), and submit ONE empty
//     fenced command buffer and wait on it.
//
// It touches no Dawn object and mutates no world state.

#pragma once

namespace sandvox {

// Run the smoke test and return a process exit code (0 = PASS).
// `lowPower` prefers an integrated GPU, mirroring `--adapter low`.
int RunVkInfo(bool lowPower);

}  // namespace sandvox
