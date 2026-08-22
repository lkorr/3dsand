#include "gpu/context.h"

#include <cstdio>
#include <string>

#include "gpu/rhi_vk.h"
#include "gpu/rhi_vulkan.h"

// After the Vulkan headers (via rhi_vulkan.h) so glfw3.h sees VK_VERSION_1_0
// and declares glfwCreateWindowSurface / glfwGetRequiredInstanceExtensions.
#include <GLFW/glfw3.h>

// Backend-private state. Vulkan-only since the Dawn removal (2026-08-22): the
// pointer indirection stays because it is what keeps the Vulkan headers out of
// context.h, which is included all over the engine.
struct GpuContext::Backend {
  std::shared_ptr<vk::Backend> vk;
};

vk::Backend* GpuContext::VkBackend() const { return back_ ? back_->vk.get() : nullptr; }

size_t GpuContext::ReportVkValidation(const char* tag) const {
  vk::Backend* be = VkBackend();
  if (!be) return 0;
  const std::vector<std::string>& msgs = be->ValidationMessages();
  for (const std::string& m : msgs)
    std::fprintf(stderr, "[vk-validation] %s\n", m.c_str());
  if (be->GetCaps().validationEnabled)
    std::printf("%s: vulkan validation messages: %zu%s\n", tag ? tag : "run",
                msgs.size(), msgs.empty() ? " (clean)" : "  <-- REPORT VERBATIM");
  return msgs.size();
}

bool GpuContext::Init(GLFWwindow* window, uint32_t w, uint32_t h,
                      bool lowPowerAdapter, bool wantTimestamps,
                      rhi::BackendKind backend, bool vkValidation,
                      bool vkSledgehammer) {
  width = w;
  height = h;
  backendKind = backend;
  back_ = std::make_shared<Backend>();

  // Vulkan is the only backend. The argument survives so callers keep a single
  // shape and so a second backend (or a paged/dense variant, phase 7) has a
  // place to plug in; anything other than Vulkan is a caller bug, and main.cpp
  // rejects `--backend dawn` with an explanation before reaching here.
  if (backend != rhi::BackendKind::Vulkan) {
    std::fprintf(stderr, "GpuContext::Init: unsupported backend (Vulkan only)\n");
    return false;
  }

  back_->vk = std::make_shared<vk::Backend>();
  std::string err;
  // Windowed: GLFW supplies the surface instance extensions
  // (VK_KHR_surface + VK_KHR_win32_surface here) and creates the surface.
  const char** glfwExts = nullptr;
  uint32_t glfwExtCount = 0;
  if (window) {
    glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    if (!glfwExts || glfwExtCount == 0) {
      std::fprintf(stderr, "GLFW reports no Vulkan surface support\n");
      return false;
    }
  }
  // Sync validation follows validation: it is the barrier document's primary
  // detector for a missing barrier (§6.2).
  if (!back_->vk->Init(lowPowerAdapter, vkValidation, vkValidation, err, glfwExts,
                       glfwExtCount, /*wantSwapchain=*/window != nullptr)) {
    std::fprintf(stderr, "Vulkan backend init failed: %s\n", err.c_str());
    return false;
  }
  const vk::Caps& caps = back_->vk->GetCaps();
  std::printf("adapter: %s (backend vulkan)\n", caps.deviceName.c_str());
  if (caps.validationEnabled)
    std::printf("  validation layer: ENABLED   sync validation: %s\n",
                caps.syncValidationEnabled ? "ENABLED" : "off");
  device = rhi::vkr::WrapDevice(back_->vk, vkSledgehammer);
  queue = device.GetQueue();
  if (window) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult sr = glfwCreateWindowSurface(back_->vk->Instance(), window, nullptr,
                                          &surface);
    if (sr != VK_SUCCESS || surface == VK_NULL_HANDLE) {
      std::fprintf(stderr, "glfwCreateWindowSurface failed (%d)\n", (int)sr);
      return false;
    }
    if (!back_->vk->ConfigureSwapchain(surface, width, height, err)) {
      std::fprintf(stderr, "swapchain creation failed: %s\n", err.c_str());
      return false;
    }
    surfaceFormat = back_->vk->SwapchainFormat();
  } else {
    surfaceFormat = rhi::TextureFormat::RGBA8Unorm;  // headless offscreen
  }
  timestampsEnabled = wantTimestamps && caps.timestampQuery;
  // Vulkan reports raw ticks; the period converts them to nanoseconds in
  // PassTimer::Collect (1.0 on this device, but never assumed).
  timestampPeriodNs = caps.timestampPeriodNs;
  return true;
}

void GpuContext::Resize(uint32_t w, uint32_t h) {
  if (!back_ || !back_->vk || w == 0 || h == 0) return;
  width = w;
  height = h;
  std::string err;
  if (!back_->vk->ConfigureSwapchain(VK_NULL_HANDLE, w, h, err))
    std::fprintf(stderr, "swapchain resize failed: %s\n", err.c_str());
}

rhi::TextureView GpuContext::AcquireFrame() {
  if (!back_ || !back_->vk) return {};
  vk::Image* im = back_->vk->AcquireSwapchainImage();
  return im ? rhi::vkr::WrapSwapchainImage(im) : rhi::TextureView{};
}

void GpuContext::Present() {
  if (back_ && back_->vk) back_->vk->PresentAcquired();
}

void GpuContext::ProcessEvents() { device.ProcessEvents(); }

void GpuContext::WaitIdle() { device.WaitIdle(); }
