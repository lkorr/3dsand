#include "gpu/resources.h"

#include <cstdio>
#include <fstream>
#include <sstream>

wgpu::Buffer CreateBuffer(const wgpu::Device& device, uint64_t size,
                          wgpu::BufferUsage usage, const char* label) {
  wgpu::BufferDescriptor d{};
  d.size = size;
  d.usage = usage;
  d.label = label;
  return device.CreateBuffer(&d);
}

static bool ReadFileText(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

wgpu::ShaderModule LoadShader(const wgpu::Device& device, const std::string& shaderDir,
                              const std::string& name) {
  std::string common, body;
  if (!ReadFileText(shaderDir + "/common.wgsl", common)) {
    std::fprintf(stderr, "cannot read %s/common.wgsl\n", shaderDir.c_str());
    return {};
  }
  if (!ReadFileText(shaderDir + "/" + name, body)) {
    std::fprintf(stderr, "cannot read %s/%s\n", shaderDir.c_str(), name.c_str());
    return {};
  }
  std::string src = common + "\n" + body;

  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = src.c_str();
  wgpu::ShaderModuleDescriptor d{};
  d.nextInChain = &wgsl;
  d.label = name.c_str();
  return device.CreateShaderModule(&d);
}

wgpu::ComputePipeline MakeComputePipeline(const wgpu::Device& device,
                                          const wgpu::PipelineLayout& layout,
                                          const wgpu::ShaderModule& module,
                                          const char* entry, const char* label) {
  wgpu::ComputePipelineDescriptor d{};
  d.layout = layout;
  d.compute.module = module;
  d.compute.entryPoint = entry;
  d.label = label;
  return device.CreateComputePipeline(&d);
}
