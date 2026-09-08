#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/rhi.h"

// Small helpers over the seam. No abstraction ambitions — just the repetitive
// parts. (The seam itself is gpu/rhi.h.)
rhi::Buffer CreateBuffer(const rhi::Device& device, uint64_t size,
                         rhi::BufferUsage usage, const char* label);

// ---- GPU buffer budget (diagnostics) ---------------------------------------
// CreateBuffer above is the single choke point for every buffer the engine
// owns, so it records one of these per allocation. This exists so that a
// question like "what does kFarN=512 or a 1024^3 window actually cost?" is
// answered by an allocation record instead of by re-deriving constants on
// paper. Never read by the sim, never hashed.
struct GpuBufferRecord {
  std::string label;
  uint64_t bytes;
};
uint64_t GpuBufferBytesTotal();
const std::vector<GpuBufferRecord>& GpuBufferRecords();
// Prints every buffer >= 1 MiB largest-first, plus a rolled-up tail and total.
void DumpGpuBufferBudget(const char* whenLabel);

// The world constants, emitted as WGSL `const` declarations generated from the
// C++ definitions in sim/world.h, so the two can never disagree. Prepended
// ahead of common.wgsl by LoadShader below.
//
// Declared here (it used to be a bare definition in resources.cpp) because the
// Vulkan backend's `--vk-info` compiles the same shaders through Tint and has
// to assemble byte-for-byte the same source string. A compiler that succeeds on
// a source the engine never feeds it has proven nothing — so the concatenation
// has to be shared, not re-derived. NOTE that scripts/check_shaders.sh is a
// THIRD reproduction of this same prelude, scraped from world.h in bash; adding
// a constant means adding it there too.
std::string ShaderConstantPrelude();

// Whether this device enabled fragmentStoresAndAtomics, which the shadow cache
// needs because raymarch.wgsl WRITES its bucket table (world.h
// kShadowCacheBuckets). Set once from GpuContext::Init and emitted by the
// prelude above as `SHADOW_CACHE_AVAILABLE`.
//
// A CAPABILITY, NOT A PREFERENCE, and the two are deliberately separate
// constants: this one says the hardware can, `TUNE_SHADOW_CACHE` says we want
// to. raymarch.wgsl ANDs them, so either can turn the cache off and only the
// tuning one is hot-reloadable. Defaults true so --vk-info, check_shaders.sh
// and any other consumer that assembles the prelude without a device still
// compiles the shipping variant of the shader.
void SetFragmentStoresAvailable(bool available);
bool FragmentStoresAvailable();

// RENDER_STATS — raymarch.wgsl's per-call-site step counters (World::
// renderStats, perfnodes.h's `rm*` counters). A prelude CONST, not a uniform
// flag, so the shipping shader compiles the counters out entirely: trace() is
// register-footprint bound and a live "if (statsOn)" would keep the
// accumulators resident whether or not they were read. Set from main.cpp when
// --telemetry or --perf is on, BEFORE the first LoadShader; default false so
// every other path compiles the shipping variant. Flipping it after pipelines
// exist needs the F5 reload path, exactly like a TUNE_* constant.
void SetRenderStatsEnabled(bool on);
bool RenderStatsEnabled();

// Loads assets/shaders/common.wgsl + the named file, concatenated behind the
// generated world-constant + tuning preludes, and compiles the result. Returns
// an invalid module on read failure.
rhi::ShaderModule LoadShader(const rhi::Device& device, const std::string& shaderDir,
                             const std::string& name);

rhi::ComputePipeline MakeComputePipeline(const rhi::Device& device,
                                         const rhi::PipelineLayout& layout,
                                         const rhi::ShaderModule& module,
                                         const char* entry, const char* label);

// ---- pipeline compile times (docs/PLAN_shader_compile.md) ------------------
//
// What the driver charged for each entry point, ALWAYS recorded — not only
// under SANDVOX_SHADER_TIMING, which is the flag that prints them. A cold
// worldgen compile is ~17 min and the whole reason the plan exists, so "which
// entry point cost what on this run" belongs in build/last_run.json next to
// the gate verdicts rather than in a terminal somebody has to have thought to
// enable (CLAUDE.md verification rule 6: attribution in the reporter, not one
// A/B run per hypothesis).
//
// THREAD-SAFE: MakeComputePipeline runs on the build pool.
struct PipelineCompileRecord {
  std::string label;
  std::string entry;
  double ms = 0;
};
void RecordPipelineCompile(const char* label, const char* entry, double ms);
std::vector<PipelineCompileRecord> PipelineCompileRecords();

// Wall-clock milliseconds from process start (the first call to
// MarkInteractiveReady/MarkFarReady's clock, which is a static set on first
// use in resources.cpp) to the two milestones the plan is about:
//   interactiveReadyMs — BuildPipelines returned, i.e. the game can run.
//   farReadyMs         — the deferred `far`/`fardown` compiles landed, i.e.
//                        the horizon exists. -1 while still pending.
// Set by Simulation::BuildPipelines and its deferred completion.
void MarkInteractiveReady();
void MarkFarReady();
double InteractiveReadyMs();
double FarReadyMs();

// The three of them as a JSON fragment: `"pipelineCompileMs": {...},
// "interactiveReadyMs": N, "farReadyMs": N`, with no leading or trailing
// comma. Every writer of build/last_run.json splices this in (selftest,
// --verify, --vk-smoke) so the record does not depend on which mode ran.
std::string PipelineTimingJson(const char* indent);
