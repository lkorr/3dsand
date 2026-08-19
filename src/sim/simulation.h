#pragma once
#include <string>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include "sim/materials.h"
#include "sim/world.h"

// Owns the compute pipelines + bind groups and records the fixed-tick GPU
// work: mutate -> 27 color passes -> occupancy/hash -> pick. Also owns the
// raymarch render pipeline (it reads the same buffers).
class Simulation {
 public:
  bool Init(const wgpu::Device& device, World& world,
            const std::vector<MaterialDef>& mats,
            const std::vector<ReactionGpu>& reactions, const std::string& shaderDir);

  // Recompile all WGSL from disk; returns false (keeping old pipelines) on
  // compile error.
  bool ReloadShaders(const wgpu::Device& device, const wgpu::Instance& instance);
  // Re-upload the material + reaction tables (JSON hot reload).
  void UploadTables(const wgpu::Queue& queue, const std::vector<MaterialDef>& mats,
                    const std::vector<ReactionGpu>& reactions);

  void EncodeWorldgen(const wgpu::CommandEncoder& enc);

  // One 30 Hz tick. Caller writes tickUBO/opsBuf via queue.WriteBuffer first,
  // then submits the encoder produced here before encoding the next tick.
  void EncodeTick(const wgpu::CommandEncoder& enc, uint32_t opsCount, bool hashEnable);

  // Render pass (raymarch fullscreen + caller then draws UI into same pass).
  wgpu::RenderPassEncoder BeginRenderPass(const wgpu::CommandEncoder& enc,
                                          const wgpu::TextureView& target,
                                          wgpu::TextureFormat format);
  void DrawWorld(const wgpu::RenderPassEncoder& pass);

  // Which dirty buffer the tick just encoded writes as "active next tick".
  const wgpu::Buffer& DirtyNext() const { return world_->dirty[1 - page_]; }
  // The dirty buffer the NEXT tick will read (valid after FlipPage) — used by
  // the selftest to count active chunks in a settled world.
  const wgpu::Buffer& DirtyActive() const { return world_->dirty[page_]; }
  // Call once after each EncodeTick has been submitted.
  void FlipPage();

 private:
  bool BuildPipelines(const wgpu::Device& device, std::string* err);

  World* world_ = nullptr;
  wgpu::Device device_;
  std::string shaderDir_;
  wgpu::Buffer materialBuf_;
  wgpu::Buffer reactionBuf_;

  wgpu::BindGroupLayout simBGL_, renderBGL_;
  wgpu::PipelineLayout simPL_, renderPL_;
  wgpu::ComputePipeline worldgen_, mutate_, compact_, compactNext_, step_,
      occupancy_, occupancyDirty_, pick_;
  wgpu::RenderPipeline raymarch_;
  wgpu::ShaderModule raymarchModule_;
  wgpu::TextureFormat raymarchFormat_ = wgpu::TextureFormat::Undefined;

  // Two bind groups: page 0 reads dirty[0]/writes dirty[1], page 1 reversed.
  wgpu::BindGroup simBG_[2];
  wgpu::BindGroup renderBG_;
  int page_ = 0;
};
