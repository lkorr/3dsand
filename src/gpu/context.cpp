#include "gpu/context.h"

#include <cstdio>
#include <string>

#include <webgpu/webgpu_glfw.h>

#include "gpu/rhi_dawn.h"

// Dawn-private state. Everything in this struct disappears when phase 3 adds a
// Vulkan backend behind the same GpuContext API.
struct GpuContext::Backend {
  wgpu::Instance instance;
  wgpu::Adapter adapter;
  wgpu::Surface surface;
};

static void PrintDeviceError(const wgpu::Device&, wgpu::ErrorType type,
                             wgpu::StringView message) {
  std::fprintf(stderr, "[webgpu error %d] %.*s\n", (int)type, (int)message.length,
               message.data);
}

bool GpuContext::Init(GLFWwindow* window, uint32_t w, uint32_t h,
                      bool lowPowerAdapter, bool wantTimestamps) {
  width = w;
  height = h;
  back_ = std::make_shared<Backend>();

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
  wgpu::SurfaceTexture st{};
  back_->surface.GetCurrentTexture(&st);
  if (st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
      st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
    return {};
  }
  return rhi::dawn::WrapTextureView(st.texture.CreateView());
}

void GpuContext::Present() { back_->surface.Present(); }

void GpuContext::ProcessEvents() { device.ProcessEvents(); }

void GpuContext::WaitIdle() { device.WaitIdle(); }
