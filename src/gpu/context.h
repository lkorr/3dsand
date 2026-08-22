#pragma once
#include <cstdint>
#include <memory>

#include "gpu/rhi.h"

struct GLFWwindow;

namespace vk {
class Backend;
}

// Owns the GPU instance/adapter/device/queue and the window surface.
//
// The device and queue are exposed as rhi:: handles: everything outside
// src/gpu/ speaks the seam only (docs/PLAN_vulkan_port.md phase 2a). The
// backend-specific objects (Dawn instance/adapter/surface) stay private in
// context.cpp. src/ui/overlay.* reaches the native Dawn device through
// rhi::dawn::Native() as the one sanctioned exception, until PHASE 4 swaps it
// for imgui_impl_vulkan.
class GpuContext {
 public:
  // lowPowerAdapter selects the LowPower adapter (typically the iGPU) — used
  // by `--adapter low` to verify cross-vendor bit-determinism (DESIGN.md §14
  // risk 3: same seed + same ops must produce the same world hash everywhere).
  // wantTimestamps requests the timestamp-query feature, used ONLY by the
  // `--measure` harness (src/measure/measure.cpp) to time individual compute
  // passes. Off by default and gracefully degrading: if the adapter does not
  // advertise the feature we simply do not request it, `timestampsEnabled`
  // stays false, and the caller falls back to wall-clock timing. Nothing in the
  // frame path looks at this.
  // `backend` selects Dawn or Vulkan at runtime (phase 4a). Both are fully
  // capable — headless and windowed — since phase 4b, and **Vulkan is the
  // default** since phase 6; Dawn is kept as the cross-backend hash oracle.
  // `vkValidation` turns on VK_LAYER_KHRONOS_validation + sync validation;
  // `vkSledgehammer` selects the barrier A/B oracle (barrier_graph §6.2). Both
  // are ignored on Dawn.
  bool Init(GLFWwindow* window, uint32_t width, uint32_t height,
            bool lowPowerAdapter = false, bool wantTimestamps = false,
            rhi::BackendKind backend = rhi::BackendKind::Vulkan,
            bool vkValidation = false, bool vkSledgehammer = false);
  void Resize(uint32_t width, uint32_t height);

  // Returns an invalid TextureView if the surface is temporarily unusable.
  rhi::TextureView AcquireFrame();
  void Present();
  void ProcessEvents();
  // Blocks until all submitted GPU work completes (selftest timing / shutdown).
  void WaitIdle();

  rhi::Device device;
  rhi::Queue queue;
  // Which backend `device` is. Mirrors device.Kind(); kept as a field so a
  // caller with only the context does not need a live device to ask. Set by
  // Init from its argument — this initialiser only matters before Init runs.
  rhi::BackendKind backendKind = rhi::BackendKind::Vulkan;
  rhi::TextureFormat surfaceFormat = rhi::TextureFormat::Undefined;
  uint32_t width = 0, height = 0;

  // The Vulkan backend object, or null on Dawn. Diagnostics only (--vk-smoke
  // prints caps + validation messages); nothing above src/gpu dereferences it.
  vk::Backend* VkBackend() const;
  // Print every validation message the Vulkan debug messenger collected (they
  // are gathered continuously, but only the F5-reload scope pops them during
  // play). Returns the count, so a harness can assert ZERO and report honestly.
  // No-op returning 0 on Dawn.
  size_t ReportVkValidation(const char* tag) const;
  // True only when Init was asked for timestamps AND the adapter supports them.
  bool timestampsEnabled = false;
  // GPU timestamp period in nanoseconds per tick. Dawn already normalises
  // timestamp query results to nanoseconds, so this is 1.0; kept as a named
  // constant so the measure harness does not bake the assumption in silently.
  double timestampPeriodNs = 1.0;

 private:
  // Backend-private state, defined in context.cpp. The pointer is what keeps
  // wgpu:: out of this header.
  struct Backend;
  std::shared_ptr<Backend> back_;
};
