// rhi_vk.h — the Vulkan backend behind the rhi.h seam (port phase 4a).
//
// PRIVATE TO src/gpu/, like rhi_dawn.h. This is what replaced vk_sim.h's
// parallel resource declarations: World::Init and Simulation::Init now create
// their buffers, layouts, descriptor sets and pipelines THROUGH the seam, and
// these impls translate each call onto vk::Backend (rhi_vulkan.h). There is one
// World again, working on either backend.
//
// WHAT IS DELIBERATELY MISSING until phase 4b: textures, render pipelines,
// render passes, compute-pass encoders. The gates that need them are declared
// (Gate::needsRender) and skipped on this backend; anything else reaching those
// entry points is an audit failure and aborts LOUDLY rather than limping.

#pragma once

#include <memory>

#include "gpu/rhi.h"

namespace vk {
class Backend;
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

}  // namespace vkr
}  // namespace rhi
