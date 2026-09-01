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
// VULKAN ONLY since 2026-08-22 (Dawn removed; docs/PLAN_vulkan_port.md phase 6
// decision log). The device and queue are exposed as rhi:: handles:
// everything outside src/gpu/ speaks the seam only (phase 2a), and the
// VkInstance/VkPhysicalDevice/VkSurfaceKHR stay private in context.cpp.
// src/ui/overlay.* reaches the native Vulkan objects through
// rhi::vkr::NativeBackend()/NativeCmd() as the one sanctioned exception,
// because imgui_impl_vulkan takes them directly.
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
  // `backend` must be Vulkan — it is the only one left. The parameter stays
  // rather than being deleted because the rhi:: seam is retained (it is what
  // made the port testable) and phase 7's paged-vs-dense residency variants
  // are the next thing that will want to select an implementation here.
  // `vkValidation` turns on VK_LAYER_KHRONOS_validation + sync validation;
  // `vkSledgehammer` selects the barrier A/B oracle (barrier_graph §6.2).
  bool Init(GLFWwindow* window, uint32_t width, uint32_t height,
            bool lowPowerAdapter = false, bool wantTimestamps = false,
            rhi::BackendKind backend = rhi::BackendKind::Vulkan,
            bool vkValidation = false, bool vkSledgehammer = false);
  void Resize(uint32_t width, uint32_t height);

  // Returns an invalid TextureView if the surface is temporarily unusable.
  rhi::TextureView AcquireFrame();
  void Present();
  void ProcessEvents();
  // Blocks on the oldest outstanding snapshot readback only — see the contract
  // on rhi::Device::WaitOldestPendingMap. False = nothing was in flight.
  bool WaitOldestPendingMap();
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

  // The Vulkan backend object, or null before Init. Diagnostics only
  // (--vk-smoke prints caps + validation messages); nothing above src/gpu
  // dereferences it.
  vk::Backend* VkBackend() const;
  // Print every validation message the Vulkan debug messenger collected (they
  // are gathered continuously, but only the F5-reload scope pops them during
  // play). Returns the count, so a harness can assert ZERO and report honestly.
  size_t ReportVkValidation(const char* tag) const;
  // The physical device's name ("NVIDIA GeForce RTX 3060 Ti"), or "" before
  // Init. Diagnostics: the performance page stamps it into every recorded run,
  // because a frame time with no GPU attached to it is not comparable to
  // anything — including the same number measured on this machine last month.
  std::string DeviceName() const;
  // True only when Init was asked for timestamps AND the adapter supports them.
  bool timestampsEnabled = false;
  // GPU timestamp period in nanoseconds per tick, from
  // VkPhysicalDeviceLimits::timestampPeriod. 1.0 on this device, but never
  // assumed — PassTimer::Collect multiplies by it.
  double timestampPeriodNs = 1.0;

 private:
  // Backend-private state, defined in context.cpp. The pointer is what keeps
  // the Vulkan headers out of this header, which most of the engine includes.
  struct Backend;
  std::shared_ptr<Backend> back_;
};
