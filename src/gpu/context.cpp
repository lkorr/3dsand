#include "gpu/context.h"

#include <cstdio>
#include <string>

#include <webgpu/webgpu_glfw.h>

static void PrintDeviceError(const wgpu::Device&, wgpu::ErrorType type,
                             wgpu::StringView message) {
  std::fprintf(stderr, "[webgpu error %d] %.*s\n", (int)type, (int)message.length,
               message.data);
}

bool GpuContext::Init(GLFWwindow* window, uint32_t w, uint32_t h,
                      bool lowPowerAdapter, bool wantTimestamps) {
  width = w;
  height = h;

  wgpu::InstanceDescriptor idesc{};
  static const auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
  idesc.requiredFeatureCount = 1;
  idesc.requiredFeatures = &kTimedWaitAny;
  instance = wgpu::CreateInstance(&idesc);
  if (!instance) {
    std::fprintf(stderr, "failed to create WebGPU instance\n");
    return false;
  }

  if (window) {
    surface = wgpu::glfw::CreateSurfaceForWindow(instance, window);
    if (!surface) {
      std::fprintf(stderr, "failed to create surface\n");
      return false;
    }
  }

  wgpu::RequestAdapterOptions opts{};
  opts.compatibleSurface = surface;  // null is fine for headless selftest
  opts.powerPreference = lowPowerAdapter ? wgpu::PowerPreference::LowPower
                                         : wgpu::PowerPreference::HighPerformance;
  wgpu::Future af = instance.RequestAdapter(
      &opts, wgpu::CallbackMode::WaitAnyOnly,
      [this](wgpu::RequestAdapterStatus status, wgpu::Adapter a,
             wgpu::StringView message) {
        if (status == wgpu::RequestAdapterStatus::Success) {
          adapter = std::move(a);
        } else {
          std::fprintf(stderr, "RequestAdapter failed: %.*s\n",
                       (int)message.length, message.data);
        }
      });
  instance.WaitAny(af, UINT64_MAX);
  if (!adapter) return false;

  wgpu::AdapterInfo info{};
  adapter.GetInfo(&info);
  std::printf("adapter: %.*s (backend %d)\n", (int)info.device.length,
              info.device.data, (int)info.backendType);

  // Ask for generous storage limits where the hardware allows (native GPUs
  // do; browser builds will fall back to a smaller world).
  wgpu::Limits supported{};
  adapter.GetLimits(&supported);
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
  if (wantTimestamps && adapter.HasFeature(wgpu::FeatureName::TimestampQuery))
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

  wgpu::Future df = adapter.RequestDevice(
      &ddesc, wgpu::CallbackMode::WaitAnyOnly,
      [this](wgpu::RequestDeviceStatus status, wgpu::Device d,
             wgpu::StringView message) {
        if (status == wgpu::RequestDeviceStatus::Success) {
          device = std::move(d);
        } else {
          std::fprintf(stderr, "RequestDevice failed: %.*s\n",
                       (int)message.length, message.data);
        }
      });
  instance.WaitAny(df, UINT64_MAX);
  if (!device) return false;
  queue = device.GetQueue();

  if (surface) {
    wgpu::SurfaceCapabilities caps{};
    surface.GetCapabilities(adapter, &caps);
    surfaceFormat = caps.formatCount > 0 ? caps.formats[0] : wgpu::TextureFormat::BGRA8Unorm;
    Resize(width, height);
  } else {
    surfaceFormat = wgpu::TextureFormat::RGBA8Unorm;  // headless offscreen
  }
  return true;
}

void GpuContext::Resize(uint32_t w, uint32_t h) {
  if (!surface || w == 0 || h == 0) return;
  width = w;
  height = h;
  wgpu::SurfaceConfiguration cfg{};
  cfg.device = device;
  cfg.format = surfaceFormat;
  cfg.usage = wgpu::TextureUsage::RenderAttachment;
  cfg.width = width;
  cfg.height = height;
  cfg.presentMode = wgpu::PresentMode::Fifo;
  surface.Configure(&cfg);
}

wgpu::TextureView GpuContext::AcquireFrame() {
  wgpu::SurfaceTexture st{};
  surface.GetCurrentTexture(&st);
  if (st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
      st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
    return {};
  }
  return st.texture.CreateView();
}

void GpuContext::Present() { surface.Present(); }

void GpuContext::ProcessEvents() { instance.ProcessEvents(); }

void GpuContext::WaitIdle() {
  bool done = false;
  wgpu::Future f = queue.OnSubmittedWorkDone(
      wgpu::CallbackMode::WaitAnyOnly,
      [&done](wgpu::QueueWorkDoneStatus, wgpu::StringView) { done = true; });
  instance.WaitAny(f, UINT64_MAX);
  (void)done;
}
