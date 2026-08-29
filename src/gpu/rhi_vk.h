// rhi_vk.h — the Vulkan backend behind the rhi.h seam (port phase 4a).
//
// PRIVATE TO src/gpu/, and since the Dawn removal (2026-08-22) the ONLY
// implementation behind the seam. This is what replaced vk_sim.h's parallel
// resource declarations: World::Init and Simulation::Init create their
// buffers, layouts, descriptor sets and pipelines THROUGH the seam, and these
// impls translate each call onto vk::Backend (rhi_vulkan.h). One World, one
// backend.
//
// Phase 4b: textures, render pipelines, render passes and the swapchain are
// live. The one remaining abort is the generic compute-pass encoder, which no
// Vulkan path may reach (sim recording goes through the RecordTable bridge).

#pragma once

#include <memory>

#include "gpu/rhi.h"
#include "gpu/vk_loader.h"  // VkCommandBuffer for the overlay accessor

namespace vk {
class Backend;
struct Image;
}

namespace rhi {
namespace vkr {

// Wrap a vk::Backend into a seam Device. `sledgehammer` selects the
// barrier-mode A/B oracle (barrier_graph §6.2) for every recorded command
// buffer. The Backend is shared: the device, its queues, encoders and buffers
// all keep it alive until the last handle drops.
Device WrapDevice(std::shared_ptr<vk::Backend> be, bool sledgehammer);

// The backend behind a seam device (caps, validation messages), or null if the
// device is not a Vulkan one. Callers are diagnostics only (--vk-smoke's
// report) plus overlay.cpp's ImGui init.
vk::Backend* NativeBackend(const Device& d);

// Recording statistics of the most recently finished command buffer on this
// device (rows/dispatches/copies/fills/barriers) — the smoke's report line.
// Returned as plain numbers so callers need no vk headers.
struct Stats {
  uint32_t rows = 0, dispatches = 0, copies = 0, fills = 0;
  uint32_t barrierCalls = 0, bufferBarriers = 0, globalBarriers = 0;
};
Stats LastStats(const Device& d);

// ---- windowed path (phase 4b D3) ------------------------------------------

// Wrap an acquired swapchain image (vk::Backend::AcquireSwapchainImage) into a
// seam TextureView for Simulation::BeginRenderPass. Non-owning: the swapchain
// owns the image; the view is valid for the frame.
TextureView WrapSwapchainImage(vk::Image* img);

// The live VkCommandBuffer behind an open seam render pass — the ONE consumer
// is src/ui/overlay.cpp, which hands it to ImGui_ImplVulkan_RenderDrawData —
// the one sanctioned place outside src/gpu/ that names a native GPU handle,
// because ImGui's render backend takes it directly.
VkCommandBuffer NativeCmd(const RenderPass& p);

// The VkImageView behind a seam TextureView, for the same ONE consumer and the
// same reason as NativeCmd: src/ui/overlay.cpp hands it to
// ImGui_ImplVulkan_AddTexture, which takes a native handle directly. Used by
// the character panel's live avatar portrait, which is rendered offscreen
// through the ordinary seam and then SAMPLED by ImGui.
//
// Deliberately NOT a general texture-binding path: rhi::BindGroupEntry still
// holds only buffers and there is still no CreateSampler on the seam, because
// nothing inside the engine samples a texture. Widening the seam for one UI
// image would be a large change to serve a consumer that already has a
// sanctioned exception.
VkImageView NativeImageView(const TextureView& v);

}  // namespace vkr
}  // namespace rhi
