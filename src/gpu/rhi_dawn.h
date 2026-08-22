// rhi_dawn.h — the Dawn/WebGPU backend behind the rhi.h seam.
//
// PRIVATE TO src/gpu/. Nothing outside this directory may include it: the whole
// point of the seam is that `wgpu::` names do not escape src/gpu/, and this
// header is where they live. (The one sanctioned exception is src/ui/overlay.*,
// which holds ImGui_ImplWGPU_* until PHASE 4b swaps it for imgui_impl_vulkan;
// it reaches the native device through rhi::dawn::Native() below.)
//
// Since phase 4a the seam is polymorphic (rhi_impl.h): every impl here is a
// subclass of the abstract base, holding the corresponding wgpu:: handle and
// nothing else, so the Dawn backend remains a passthrough with no state of its
// own beyond what WebGPU already owns. Recording is byte-identical to the
// pre-polymorphism seam — same wgpu calls, one virtual hop — which is what the
// pinned determinism hash verifies. rhi_vk.cpp is the second backend, selected
// at runtime by GpuContext::Init's backend argument.

#pragma once

#include <webgpu/webgpu_cpp.h>

#include "gpu/rhi_impl.h"

namespace rhi {
namespace dawn {

// Wrap a native handle into a seam handle. Used by GpuContext (which still
// creates the device through Dawn's own API) and by the resource helpers.
Device WrapDevice(const wgpu::Device& d, const wgpu::Instance& inst);
Buffer WrapBuffer(const wgpu::Buffer& b, uint64_t size);
Texture WrapTexture(const wgpu::Texture& t);
TextureView WrapTextureView(const wgpu::TextureView& v);
Queue WrapQueue(const wgpu::Queue& q);

// Unwrap back to native. Legitimate uses are inside src/gpu/ only, plus the
// PHASE 4b overlay exception (ImGui_ImplWGPU needs the raw device, format and
// render pass encoder). These downcast to the Dawn impl and are valid ONLY for
// handles created by this backend — every caller is on a Dawn-only path.
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
