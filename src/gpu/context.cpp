#include "gpu/context.h"

#include <cstdio>
#include <string>

#include <webgpu/webgpu_glfw.h>

#include "gpu/rhi_dawn.h"
#include "gpu/rhi_vk.h"
#include "gpu/rhi_vulkan.h"

// After the Vulkan headers (via rhi_vulkan.h) so glfw3.h sees VK_VERSION_1_0
// and declares glfwCreateWindowSurface / glfwGetRequiredInstanceExtensions.
#include <GLFW/glfw3.h>

// Backend-private state: the Dawn members when backendKind is Dawn, the
// vk::Backend when it is Vulkan (phase 4a runtime selection).
struct GpuContext::Backend {
  wgpu::Instance instance;
  wgpu::Adapter adapter;
  wgpu::Surface surface;
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

static void PrintDeviceError(const wgpu::Device&, wgpu::ErrorType type,
                             wgpu::StringView message) {
  std::fprintf(stderr, "[webgpu error %d] %.*s\n", (int)type, (int)message.length,
               message.data);
}

bool GpuContext::Init(GLFWwindow* window, uint32_t w, uint32_t h,
                      bool lowPowerAdapter, bool wantTimestamps,
                      rhi::BackendKind backend, bool vkValidation,
                      bool vkSledgehammer) {
  width = w;
  height = h;
  backendKind = backend;
  back_ = std::make_shared<Backend>();

  if (backend == rhi::BackendKind::Vulkan) {
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

  wgpu::InstanceDescriptor idesc{};
  static const auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
  idesc.requiredFeatureCount = 1;
  idesc.requiredFeatures = &kTimedWaitAny;
  back_->instance = wgpu::CreateInstance(&idesc);
  if (!back_->instance) {
    std::fprintf(stderr, "failed to create WebGPU instance\n");
    return false;
  }

  if (window) {
    back_->surface = wgpu::glfw::CreateSurfaceForWindow(back_->instance, window);
    if (!back_->surface) {
      std::fprintf(stderr, "failed to create surface\n");
      return false;
    }
  }

  wgpu::RequestAdapterOptions opts{};
  opts.compatibleSurface = back_->surface;  // null is fine for headless selftest
  opts.powerPreference = lowPowerAdapter ? wgpu::PowerPreference::LowPower
                                         : wgpu::PowerPreference::HighPerformance;
  wgpu::Future af = back_->instance.RequestAdapter(
      &opts, wgpu::CallbackMode::WaitAnyOnly,
      [this](wgpu::RequestAdapterStatus status, wgpu::Adapter a,
             wgpu::StringView message) {
        if (status == wgpu::RequestAdapterStatus::Success) {
          back_->adapter = std::move(a);
        } else {
          std::fprintf(stderr, "RequestAdapter failed: %.*s\n",
                       (int)message.length, message.data);
        }
      });
  back_->instance.WaitAny(af, UINT64_MAX);
  if (!back_->adapter) return false;

  wgpu::AdapterInfo info{};
  back_->adapter.GetInfo(&info);
  std::printf("adapter: %.*s (backend %d)\n", (int)info.device.length,
              info.device.data, (int)info.backendType);

  // Ask for generous storage limits where the hardware allows.
  wgpu::Limits supported{};
  back_->adapter.GetLimits(&supported);
  wgpu::Limits required{};
  auto clampTo = [](uint64_t want, uint64_t have) { return want < have ? want : have; };
  required.maxStorageBufferBindingSize =
      clampTo(512ull * 1024 * 1024, supported.maxStorageBufferBindingSize);
  required.maxBufferSize = clampTo(512ull * 1024 * 1024, supported.maxBufferSize);
  required.maxStorageBuffersPerShaderStage =
      clampTo(16, supported.maxStorageBuffersPerShaderStage);
  required.maxComputeInvocationsPerWorkgroup =
      clampTo(256, supported.maxComputeInvocationsPerWorkgroup);

  // Optional TimestampQuery, for `--measure` only. Guarded on adapter support:
  // an unsupported feature in requiredFeatures makes RequestDevice FAIL, so a
  // blind request would break the game on any adapter without it.
  wgpu::FeatureName tsFeature = wgpu::FeatureName::TimestampQuery;
  if (wantTimestamps && back_->adapter.HasFeature(wgpu::FeatureName::TimestampQuery))
    timestampsEnabled = true;

  wgpu::DeviceDescriptor ddesc{};
  if (timestampsEnabled) {
    ddesc.requiredFeatureCount = 1;
    ddesc.requiredFeatures = &tsFeature;
  }
  ddesc.requiredLimits = &required;
  ddesc.SetUncapturedErrorCallback(PrintDeviceError);
  ddesc.SetDeviceLostCallback(
      wgpu::CallbackMode::AllowSpontaneous,
      [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
        if (reason != wgpu::DeviceLostReason::Destroyed) {
          std::fprintf(stderr, "device lost (%d): %.*s\n", (int)reason,
                       (int)message.length, message.data);
        }
      });

  wgpu::Device nativeDevice;
  wgpu::Future df = back_->adapter.RequestDevice(
      &ddesc, wgpu::CallbackMode::WaitAnyOnly,
      [&nativeDevice](wgpu::RequestDeviceStatus status, wgpu::Device d,
                      wgpu::StringView message) {
        if (status == wgpu::RequestDeviceStatus::Success) {
          nativeDevice = std::move(d);
        } else {
          std::fprintf(stderr, "RequestDevice failed: %.*s\n",
                       (int)message.length, message.data);
        }
      });
  back_->instance.WaitAny(df, UINT64_MAX);
  if (!nativeDevice) return false;

  device = rhi::dawn::WrapDevice(nativeDevice, back_->instance);
  queue = device.GetQueue();

  if (back_->surface) {
    wgpu::SurfaceCapabilities caps{};
    back_->surface.GetCapabilities(back_->adapter, &caps);
    wgpu::TextureFormat fmt =
        caps.formatCount > 0 ? caps.formats[0] : wgpu::TextureFormat::BGRA8Unorm;
    surfaceFormat = rhi::dawn::FromWgpu(fmt);
    Resize(width, height);
  } else {
    surfaceFormat = rhi::TextureFormat::RGBA8Unorm;  // headless offscreen
  }
  return true;
}

void GpuContext::Resize(uint32_t w, uint32_t h) {
  if (back_ && back_->vk) {
    if (w == 0 || h == 0) return;
    width = w;
    height = h;
    std::string err;
    if (!back_->vk->ConfigureSwapchain(VK_NULL_HANDLE, w, h, err))
      std::fprintf(stderr, "swapchain resize failed: %s\n", err.c_str());
    return;
  }
  if (!back_ || !back_->surface || w == 0 || h == 0) return;
  width = w;
  height = h;
  wgpu::SurfaceConfiguration cfg{};
  cfg.device = rhi::dawn::Native(device);
  cfg.format = rhi::dawn::ToWgpu(surfaceFormat);
  cfg.usage = wgpu::TextureUsage::RenderAttachment;
  cfg.width = width;
  cfg.height = height;
  cfg.presentMode = wgpu::PresentMode::Fifo;
  back_->surface.Configure(&cfg);
}

rhi::TextureView GpuContext::AcquireFrame() {
  if (back_ && back_->vk) {
    vk::Image* im = back_->vk->AcquireSwapchainImage();
    return im ? rhi::vkr::WrapSwapchainImage(im) : rhi::TextureView{};
  }
  if (!back_ || !back_->surface) return {};
  wgpu::SurfaceTexture st{};
  back_->surface.GetCurrentTexture(&st);
  if (st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
      st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
    return {};
  }
  return rhi::dawn::WrapTextureView(st.texture.CreateView());
}

void GpuContext::Present() {
  if (back_ && back_->vk) {
    back_->vk->PresentAcquired();
    return;
  }
  if (back_ && back_->surface) back_->surface.Present();
}

void GpuContext::ProcessEvents() { device.ProcessEvents(); }

void GpuContext::WaitIdle() { device.WaitIdle(); }
