#pragma once
#include <cstdint>

#include <webgpu/webgpu_cpp.h>

struct GLFWwindow;

// Owns the WebGPU instance/adapter/device/queue and the window surface.
// Native path runs through Dawn (Vulkan backend); the same API surface is what
// the Emscripten browser build targets later.
class GpuContext {
 public:
  bool Init(GLFWwindow* window, uint32_t width, uint32_t height);
  void Resize(uint32_t width, uint32_t height);

  // Returns an invalid TextureView if the surface is temporarily unusable.
  wgpu::TextureView AcquireFrame();
  void Present();
  void ProcessEvents();
  // Blocks until all submitted GPU work completes (selftest timing / shutdown).
  void WaitIdle();

  wgpu::Instance instance;
  wgpu::Adapter adapter;
  wgpu::Device device;
  wgpu::Queue queue;
  wgpu::Surface surface;
  wgpu::TextureFormat surfaceFormat = wgpu::TextureFormat::Undefined;
  uint32_t width = 0, height = 0;
};
