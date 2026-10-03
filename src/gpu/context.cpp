#include "gpu/context.h"

#include <cstdio>
#include <string>

#include "gpu/rhi_vk.h"
#include "gpu/rhi_vulkan.h"
#include "gpu/resources.h"
#include "sim/tuning.h"  // render.asyncCompute decides whether the async queue exists

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

std::string GpuContext::DeviceName() const {
  vk::Backend* be = VkBackend();
  return be ? be->GetCaps().deviceName : std::string();
}

const char* SandvoxBuildCommit();  // generated: cmake/build_info.cmake

namespace {
// The most recent Init's backend, for writers that hold no context (the
// selftest's last_run.json, the fingerprint). One device per process in every
// mode that writes one; a process that boots two (the two-player smoke)
// records the last. WEAK and read at WRITE time, not at Init: one field
// (oversizeBindings) is only known once the bind groups exist.
std::weak_ptr<vk::Backend> g_lastBackend;
std::string g_lastDeviceLine;

std::string JsonEsc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

std::string VkVersionString(uint32_t v) {
  char b[32];
  std::snprintf(b, sizeof b, "%u.%u.%u", VK_API_VERSION_MAJOR(v),
                VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
  return b;
}

std::string LineOf(const vk::Caps& c) {
  char b[512];
  std::snprintf(b, sizeof b, "%s | %s %s | vk %s | %04x:%04x %s [%u/%u] | build %s",
                c.deviceName.c_str(), c.driverName.c_str(), c.driverInfo.c_str(),
                VkVersionString(c.apiVersion).c_str(), c.vendorId, c.deviceId,
                c.deviceType.c_str(), c.deviceIndex, c.deviceCount,
                SandvoxBuildCommit());
  return b;
}

std::string JsonOf(const vk::Backend& be) {
  const vk::Caps& c = be.GetCaps();
  const vk::Backend::AsyncStats& as = be.GetAsyncStats();
  char b[1792];
  std::snprintf(b, sizeof b,
                "{\"name\": \"%s\", \"driverName\": \"%s\", \"driverInfo\": \"%s\", "
                "\"driverId\": %u, \"driverVersion\": %u, \"apiVersion\": \"%s\", "
                "\"vendorId\": %u, \"deviceId\": %u, \"type\": \"%s\", "
                "\"index\": %u, \"count\": %u, \"pipelineCacheUuid\": \"%s\", "
                "\"queueFamilies\": \"%s\", \"asyncCompute\": %s, "
                "\"maxStorageBufferRange\": %llu, \"khr13Fallback\": %s, "
                "\"outOfSpecBindings\": %s, \"outOfSpecMaxRange\": %llu, \"robustBufferAccess\": %s, "
                "\"asyncComputeEnabled\": %s, \"asyncSubmits\": %llu, "
                "\"asyncJoins\": %llu, \"asyncHeadJoins\": %llu, \"asyncSplits\": %llu, "
                "\"descSets\": %u, \"descSetsMax\": %u, \"descStorage\": %u, "
                "\"descStorageMax\": %u, "
                "\"buildCommit\": \"%s\"}",
                JsonEsc(c.deviceName).c_str(), JsonEsc(c.driverName).c_str(),
                JsonEsc(c.driverInfo).c_str(), c.driverId, c.driverVersion,
                VkVersionString(c.apiVersion).c_str(), c.vendorId, c.deviceId,
                c.deviceType.c_str(), c.deviceIndex, c.deviceCount,
                c.pipelineCacheUuid.c_str(), JsonEsc(c.queueFamilies).c_str(),
                c.asyncComputeAvailable ? "true" : "false",
                (unsigned long long)c.maxStorageBufferRange,
                c.khr13Fallback ? "true" : "false",
                c.oversizeBindings ? "true" : "false",
                (unsigned long long)c.oversizeMaxRange,
                c.robustBufferAccessEnabled ? "true" : "false",
                be.AsyncEnabled() ? "true" : "false",
                (unsigned long long)as.submits, (unsigned long long)as.joins,
                (unsigned long long)as.headJoins, (unsigned long long)as.splits,
                vk::Backend::DescSetsUsed(), vk::kDescPoolMaxSets,
                vk::Backend::DescStorageUsed(), vk::kDescPoolStorageBuffers,
                JsonEsc(SandvoxBuildCommit()).c_str());
  return b;
}
}  // namespace

std::string LastDeviceJson() {
  std::shared_ptr<vk::Backend> be = g_lastBackend.lock();
  return be ? JsonOf(*be) : std::string("null");
}
const std::string& LastDeviceLine() { return g_lastDeviceLine; }
std::string BuildCommit() { return SandvoxBuildCommit(); }

std::string GpuContext::DeviceLine() const {
  vk::Backend* be = VkBackend();
  return be ? LineOf(be->GetCaps()) : std::string();
}

std::string GpuContext::DeviceJson() const {
  vk::Backend* be = VkBackend();
  return be ? JsonOf(*be) : std::string("null");
}

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
  // The async compute queue exists only if this boot's tuning asks for it
  // (render.asyncCompute; env overrides live in CreateLogicalDevice). An
  // unused second queue is not free on NVIDIA — docs/PLAN_async_compute.md.
  vk::Backend::SetAsyncQueueWanted(CurrentTuning().render.asyncCompute);
  // Sync validation follows validation: it is the barrier document's primary
  // detector for a missing barrier (§6.2).
  if (!back_->vk->Init(lowPowerAdapter, vkValidation, vkValidation, err, glfwExts,
                       glfwExtCount, /*wantSwapchain=*/window != nullptr)) {
    std::fprintf(stderr, "Vulkan backend init failed: %s\n", err.c_str());
    return false;
  }
  const vk::Caps& caps = back_->vk->GetCaps();
  std::printf("adapter: %s (backend vulkan)\n", caps.deviceName.c_str());
  // The facts a hash needs beside it (cross-vendor determinism): which driver
  // and shader compiler, which API version, which of the loader's devices, and
  // which source. One line, always, so any log of any run says what ran it.
  g_lastDeviceLine = DeviceLine();
  g_lastBackend = back_->vk;
  std::printf("device: %s\n", g_lastDeviceLine.c_str());
  std::printf("  queues: %s | async compute: %s | maxStorageBufferRange %llu%s\n",
              caps.queueFamilies.c_str(),
              caps.asyncComputeAvailable
                  ? ("family " + std::to_string(caps.asyncComputeFamily)).c_str()
                  : "none (single queue)",
              (unsigned long long)caps.maxStorageBufferRange,
              caps.khr13Fallback ? " | 1.2 + KHR sync2/dynamic_rendering" : "");
  // The shadow cache is the one fragment-stage storage write in the engine, and
  // it is compiled in or out rather than branched on (world.h). Tell the shader
  // prelude before anything loads a shader.
  SetFragmentStoresAvailable(caps.fragmentStoresAndAtomics);
  if (!caps.fragmentStoresAndAtomics)
    std::printf("  fragmentStoresAndAtomics: ABSENT — shadow cache disabled, "
                "raymarch falls back to the inline shadow ray\n");
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
    presentMode = back_->vk->ActivePresentMode();
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
  presentMode = back_->vk->ActivePresentMode();
}

static const char* PresentModeName(rhi::PresentMode m) {
  switch (m) {
    case rhi::PresentMode::Mailbox: return "mailbox";
    case rhi::PresentMode::Immediate: return "immediate";
    default: return "fifo";
  }
}

void GpuContext::SetPresentMode(rhi::PresentMode mode) {
  if (!back_ || !back_->vk) return;
  if (back_->vk->RequestedPresentMode() == mode) return;
  back_->vk->SetPresentMode(mode);
  // Headless (no surface yet) or before Init: the request is remembered and
  // the first ConfigureSwapchain honours it.
  if (back_->vk->SwapchainImageCount() == 0 || width == 0 || height == 0) return;
  std::string err;
  if (!back_->vk->ConfigureSwapchain(VK_NULL_HANDLE, width, height, err)) {
    std::fprintf(stderr, "swapchain present-mode change failed: %s\n", err.c_str());
    return;
  }
  presentMode = back_->vk->ActivePresentMode();
  std::printf("present mode: %s%s\n", PresentModeName(presentMode),
              presentMode != mode ? " (requested mode not offered by the surface)" : "");
}

bool GpuContext::SwapchainBlittable() const {
  return back_ && back_->vk && back_->vk->SwapchainBlittable();
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

bool GpuContext::WaitOldestPendingMap() { return device.WaitOldestPendingMap(); }
int GpuContext::PendingMapCount() const { return device.PendingMapCount(); }

void GpuContext::WaitIdle() { device.WaitIdle(); }
