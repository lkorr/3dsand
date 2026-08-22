#pragma once
#include <string>

#include "gpu/rhi.h"

// Small helpers over the seam. No abstraction ambitions — just the repetitive
// parts. (The seam itself is gpu/rhi.h.)
rhi::Buffer CreateBuffer(const rhi::Device& device, uint64_t size,
                         rhi::BufferUsage usage, const char* label);

// Loads assets/shaders/common.wgsl + the named file, concatenated behind the
// generated world-constant + tuning preludes, and compiles the result. Returns
// an invalid module on read failure.
rhi::ShaderModule LoadShader(const rhi::Device& device, const std::string& shaderDir,
                             const std::string& name);

rhi::ComputePipeline MakeComputePipeline(const rhi::Device& device,
                                         const rhi::PipelineLayout& layout,
                                         const rhi::ShaderModule& module,
                                         const char* entry, const char* label);
