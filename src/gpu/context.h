#pragma once
#include <cstdint>
#include <memory>

#include "gpu/rhi.h"

struct GLFWwindow;

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
  bool Init(GLFWwindow* window, uint32_t width, uint32_t height,
            bool lowPowerAdapter = false, bool wantTimestamps = false);
  void Resize(uint32_t width, uint32_t height);

  // Returns an invalid TextureView if the surface is temporarily unusable.
  rhi::TextureView AcquireFrame();
  void Present();
  void ProcessEvents();
  // Blocks until all submitted GPU work completes (selftest timing / shutdown).
  void WaitIdle();

  rhi::Device device;
  rhi::Queue queue;
  rhi::TextureFormat surfaceFormat = rhi::TextureFormat::Undefined;
  uint32_t width = 0, height = 0;
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
