// vk_shader_stats.h — the `--shader-stats` readout (PLAN_lin_followups W1-A).
//
// WHAT IT ANSWERS, in the plain words of RESEARCH_john_lin.md §14: "tells us,
// in one line per shader, whether the GPU is running out of fast registers and
// falling back to slow memory. Right now we guess."
//
// The guessing is not hypothetical. The renderer's cost model
// (docs/, "raymarch cost attribution") attributes ~49% of the frame to the
// REGISTER FOOTPRINT of trace() call sites, and the 10.8x by-value-uniform bug
// was a spill nobody could see — it was found by reasoning about WGSL, not by
// reading a number. This mode reads the number.
//
// HOW: VK_KHR_pipeline_executable_properties. Pipelines built with
// VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR can be interrogated for their
// per-executable statistics, which is where a driver reports register counts,
// spill/scratch bytes and occupancy. The extension is OPTIONAL and the counter
// set is VENDOR-DEFINED: nothing in this file may name a vendor's counter.
// Everything the driver reports is printed; only the row SORT looks at names,
// and only by substring ("local" / "spill" / "scratch"), so a GPU with
// different names still prints a complete table, just in create order.
//
// It is an instrument, not a gate: no baseline, no pass/fail, no hash. It
// creates a device, builds every pipeline once and exits.

#pragma once

#include <string>

namespace rhi {
class Device;
}

namespace sandvox {

// Walk every pipeline on `device`, print the table, and write `jsonPath`.
// Returns a process exit code: 0 = a table was printed, 2 = the driver does
// not support VK_KHR_pipeline_executable_properties (one line saying so, which
// is a legitimate answer rather than a failure of this code).
//
// The caller must have called rhi::vkr::SetCaptureStats(device, true) BEFORE
// creating any pipeline, and must have forced the lazily-created render
// pipelines into existence (Simulation::ForceRenderPipelines) — otherwise the
// interesting row, `raymarch`'s fragment executable, does not exist yet.
int RunShaderStats(const rhi::Device& device, const std::string& jsonPath);

}  // namespace sandvox
