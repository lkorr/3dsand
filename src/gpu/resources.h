#pragma once
#include <string>

#include "gpu/rhi.h"

// Small helpers over the seam. No abstraction ambitions — just the repetitive
// parts. (The seam itself is gpu/rhi.h.)
rhi::Buffer CreateBuffer(const rhi::Device& device, uint64_t size,
                         rhi::BufferUsage usage, const char* label);

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

// Loads assets/shaders/common.wgsl + the named file, concatenated behind the
// generated world-constant + tuning preludes, and compiles the result. Returns
// an invalid module on read failure.
rhi::ShaderModule LoadShader(const rhi::Device& device, const std::string& shaderDir,
                             const std::string& name);

rhi::ComputePipeline MakeComputePipeline(const rhi::Device& device,
                                         const rhi::PipelineLayout& layout,
                                         const rhi::ShaderModule& module,
                                         const char* entry, const char* label);
