// rhi_vk.h — the Vulkan backend behind the rhi.h seam (port phase 4a).
//
// PRIVATE TO src/gpu/, like rhi_dawn.h. This is what replaced vk_sim.h's
// parallel resource declarations: World::Init and Simulation::Init now create
// their buffers, layouts, descriptor sets and pipelines THROUGH the seam, and
// these impls translate each call onto vk::Backend (rhi_vulkan.h). There is one
// World again, working on either backend.
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

// The backend behind a Vulkan seam device (caps, validation messages), or null
// for a Dawn device. Callers are diagnostics only (--vk-smoke's report).
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
// is src/ui/overlay.cpp, which hands it to ImGui_ImplVulkan_RenderDrawData
// (the imgui_impl_wgpu Native(pass) counterpart, same sanctioned exception).
VkCommandBuffer NativeCmd(const RenderPass& p);

}  // namespace vkr
}  // namespace rhi
