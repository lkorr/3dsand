// rhi_dawn.h — the Dawn/WebGPU backend behind rhi.h.
//
// PRIVATE TO src/gpu/. Nothing outside this directory may include it: the whole
// point of the seam is that `wgpu::` names do not escape src/gpu/, and this
// header is where they live. (The one sanctioned exception is src/ui/overlay.*,
// which holds ImGui_ImplWGPU_* until PHASE 4 swaps it for imgui_impl_vulkan;
// it reaches the native device through rhi::dawn::Native() below.)
//
// Every impl struct is a thin holder around the corresponding wgpu:: handle, so
// the Dawn backend is a passthrough with no state of its own beyond what WebGPU
// already owns. Phase 3 adds rhi_vulkan.{h,cpp} alongside this, selected by the
// --backend flag, and both compile against the same rhi.h.

#pragma once

#include <webgpu/webgpu_cpp.h>

#include "gpu/rhi.h"

namespace rhi {

struct BufferImpl { wgpu::Buffer h; uint64_t size = 0; };
struct TextureImpl { wgpu::Texture h; };
struct TextureViewImpl { wgpu::TextureView h; };
struct ShaderModuleImpl { wgpu::ShaderModule h; };
struct BindGroupLayoutImpl { wgpu::BindGroupLayout h; };
struct BindGroupImpl { wgpu::BindGroup h; };
struct PipelineLayoutImpl { wgpu::PipelineLayout h; };
struct ComputePipelineImpl { wgpu::ComputePipeline h; };
struct RenderPipelineImpl { wgpu::RenderPipeline h; };
struct CommandEncoderImpl { wgpu::CommandEncoder h; };
struct CommandBufferImpl { wgpu::CommandBuffer h; };
struct ComputePassImpl { wgpu::ComputePassEncoder h; };
struct RenderPassImpl { wgpu::RenderPassEncoder h; };
struct QuerySetImpl { wgpu::QuerySet h; };
struct QueueImpl { wgpu::Queue h; };

// The device impl additionally keeps the instance, because WebGPU's blocking
// primitives (WaitAny) hang off the instance rather than the device. Vulkan has
// no equivalent split — phase 3's DeviceImpl simply will not have this field.
struct DeviceImpl {
  wgpu::Device h;
  wgpu::Instance instance;
};

namespace dawn {

// Wrap a native handle into a seam handle. Used by GpuContext (which still
// creates the device through Dawn's own API) and by the resource helpers.
Device WrapDevice(const wgpu::Device& d, const wgpu::Instance& inst);
Buffer WrapBuffer(const wgpu::Buffer& b, uint64_t size);
Texture WrapTexture(const wgpu::Texture& t);
TextureView WrapTextureView(const wgpu::TextureView& v);
Queue WrapQueue(const wgpu::Queue& q);

// Unwrap back to native. Legitimate uses are inside src/gpu/ only, plus the
// PHASE 4 overlay exception (ImGui_ImplWGPU needs the raw device, format and
// render pass encoder).
const wgpu::Device& Native(const Device& d);
const wgpu::Instance& NativeInstance(const Device& d);
const wgpu::Buffer& Native(const Buffer& b);
const wgpu::Texture& Native(const Texture& t);
const wgpu::TextureView& Native(const TextureView& v);
const wgpu::Queue& Native(const Queue& q);
const wgpu::RenderPassEncoder& Native(const RenderPass& p);
const wgpu::CommandEncoder& Native(const CommandEncoder& e);

wgpu::TextureFormat ToWgpu(TextureFormat f);
TextureFormat FromWgpu(wgpu::TextureFormat f);

}  // namespace dawn
}  // namespace rhi
