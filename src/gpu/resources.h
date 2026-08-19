#pragma once
#include <string>

#include <webgpu/webgpu_cpp.h>

// Small helpers over the raw API. No abstraction ambitions — just the
// repetitive parts.
wgpu::Buffer CreateBuffer(const wgpu::Device& device, uint64_t size,
                          wgpu::BufferUsage usage, const char* label);

// Loads assets/shaders/common.wgsl + the named file, concatenated, and
// compiles the result. Returns an invalid module on read failure.
wgpu::ShaderModule LoadShader(const wgpu::Device& device, const std::string& shaderDir,
                              const std::string& name);

wgpu::ComputePipeline MakeComputePipeline(const wgpu::Device& device,
                                          const wgpu::PipelineLayout& layout,
                                          const wgpu::ShaderModule& module,
                                          const char* entry, const char* label);
