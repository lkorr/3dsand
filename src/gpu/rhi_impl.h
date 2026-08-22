// rhi_impl.h — the abstract impl layer behind the rhi.h handles (port phase 4a).
//
// PRIVATE TO src/gpu/. rhi.h forward-declares these types; this header defines
// them as abstract bases. There is exactly ONE subclass set today:
//
//   rhi_vk.cpp     Vkr*   subclasses — the Vulkan backend, reached through the
//                  seam rather than through phase 3b's vk_sim.h parallel
//                  resource declarations (which phase 4a deleted).
//
// rhi_dawn.cpp held the second set until the DAWN REMOVAL (2026-08-22).
//
// WHY THE ABSTRACTION SURVIVES ITS SECOND IMPLEMENTATION — this is the obvious
// thing to "clean up", so the reason is recorded here rather than in a commit
// message. Virtual dispatch was introduced in phase 4a because the selftest
// gates drive World/Simulation/Stream, whose resources are rhi:: handles, and
// pointing a gate at a backend therefore required one handle type with two
// implementations. That capability is what made the port verifiable phase by
// phase. It costs one indirect call on ~60 dispatches and a handful of copies
// per tick — microseconds of CPU, unmeasurable against the tick — and it is
// the slot phase 7's paged-residency BufferImpl plugs into: a sparse-backed
// voxels buffer is another BufferImpl subclass, and the paged-vs-dense hash
// equality checkpoint is the same two-configurations-one-driver shape the
// cross-backend diff had. Devirtualising now would have to be undone then.
//
// The ONE thing the polymorphism does NOT change: barrier generation. The
// Vulkan encoder does not derive barriers from these wgpu-shaped calls — sim
// recording routes through vk::Recorder walking pass::kRows (see the bridge in
// rhi_record.h), exactly the phase-3b shape. The encoder virtuals below exist
// for the off-table paths (readback copies, staging reads, fills), every one of
// which still expresses its hazard through the same tracker.

#pragma once

#include <functional>

#include "gpu/rhi.h"

namespace rhi {

struct BufferImpl {
  virtual ~BufferImpl() = default;
  // Non-blocking map for the World readback ring; the callback fires from
  // Device::ProcessEvents(). Dawn: wgpu MapAsync. Vulkan: the buffer is
  // persistently mapped, so this borrows the fence of the most recent submit
  // (barrier_graph §4.2) and fires when it signals.
  virtual void MapReadAsync(uint64_t offset, uint64_t size,
                            std::function<void(const void*)> done) = 0;
  uint64_t size = 0;
};

struct TextureImpl {
  virtual ~TextureImpl() = default;
  virtual TextureView CreateView() = 0;
};
struct TextureViewImpl { virtual ~TextureViewImpl() = default; };
struct ShaderModuleImpl { virtual ~ShaderModuleImpl() = default; };
struct BindGroupLayoutImpl { virtual ~BindGroupLayoutImpl() = default; };
struct BindGroupImpl { virtual ~BindGroupImpl() = default; };
struct PipelineLayoutImpl { virtual ~PipelineLayoutImpl() = default; };
struct ComputePipelineImpl { virtual ~ComputePipelineImpl() = default; };
struct RenderPipelineImpl { virtual ~RenderPipelineImpl() = default; };
struct CommandBufferImpl { virtual ~CommandBufferImpl() = default; };
struct QuerySetImpl { virtual ~QuerySetImpl() = default; };

struct ComputePassImpl {
  virtual ~ComputePassImpl() = default;
  virtual void SetPipeline(const ComputePipeline& p) = 0;
  virtual void SetBindGroup(uint32_t index, const BindGroup& bg, uint32_t dynCount,
                            const uint32_t* dynOffsets) = 0;
  virtual void Dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;
  virtual void DispatchIndirect(const Buffer& args, uint64_t offset) = 0;
  virtual void End() = 0;
};

struct RenderPassImpl {
  virtual ~RenderPassImpl() = default;
  virtual void SetPipeline(const RenderPipeline& p) = 0;
  virtual void SetBindGroup(uint32_t index, const BindGroup& bg) = 0;
  virtual void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                    uint32_t firstInstance) = 0;
  virtual void DrawIndirect(const Buffer& args, uint64_t offset) = 0;
  virtual void End() = 0;
};

struct CommandEncoderImpl {
  virtual ~CommandEncoderImpl() = default;
  virtual void ClearBuffer(const Buffer& b, uint64_t offset, uint64_t size) = 0;
  virtual void CopyBufferToBuffer(const Buffer& src, uint64_t srcOffset, const Buffer& dst,
                                  uint64_t dstOffset, uint64_t size) = 0;
  virtual void CopyTracked(pass::Buf srcId, const Buffer& src, uint64_t srcOffset,
                           const Buffer& dst, uint64_t dstOffset, uint64_t size) = 0;
  virtual void FillTracked(pass::Buf id, const Buffer& b) = 0;
  virtual void FillTrackedRange(pass::Buf id, const Buffer& b, uint64_t offset,
                                uint64_t size, uint32_t pattern) = 0;
  virtual void CopyTextureToBuffer(const TexelCopyTexture& src, const TexelCopyBuffer& dst,
                                   const Extent3D& extent) = 0;
  virtual void ResolveQuerySet(const QuerySet& qs, uint32_t firstQuery, uint32_t queryCount,
                               const Buffer& dst, uint64_t dstOffset) = 0;
  // ts == nullptr means an untimed pass; the public API's two overloads
  // collapse here so a backend implements exactly one entry point.
  virtual ComputePass BeginComputePass(const char* label, const PassTimestampWrites* ts) = 0;
  virtual RenderPass BeginRenderPass(const RenderPassDesc& d) = 0;
  virtual CommandBuffer Finish() = 0;
};

struct QueueImpl {
  virtual ~QueueImpl() = default;
  virtual void WriteBuffer(const Buffer& b, uint64_t offset, const void* data,
                           size_t size) = 0;
  virtual void Submit(uint32_t count, const CommandBuffer* cmds) = 0;
};

struct MapTicketImpl {
  virtual ~MapTicketImpl() = default;
  virtual bool Ready() = 0;
  virtual void Wait() = 0;
  virtual bool Succeeded() = 0;
  virtual const void* Data() = 0;
  virtual void Unmap() = 0;
};

struct DeviceImpl {
  virtual ~DeviceImpl() = default;
  virtual BackendKind Kind() const = 0;
  virtual Queue GetQueue() = 0;
  virtual Buffer CreateBuffer(uint64_t size, BufferUsage usage, const char* label) = 0;
  virtual Texture CreateTexture(const Extent3D& size, TextureFormat format,
                                TextureUsage usage, const char* label) = 0;
  virtual QuerySet CreateTimestampQuerySet(uint32_t count, const char* label) = 0;
  virtual BindGroupLayout CreateBindGroupLayout(const BindGroupLayoutEntry* entries,
                                                size_t count) = 0;
  virtual PipelineLayout CreatePipelineLayout(const BindGroupLayout* groups,
                                              size_t count) = 0;
  virtual BindGroup CreateBindGroup(const BindGroupLayout& layout,
                                    const BindGroupEntry* entries, size_t count,
                                    const char* label) = 0;
  virtual ShaderModule CreateShaderModule(const std::string& wgsl, const char* label) = 0;
  virtual ComputePipeline CreateComputePipeline(const PipelineLayout& layout,
                                                const ShaderModule& module,
                                                const char* entry, const char* label) = 0;
  virtual RenderPipeline CreateRenderPipeline(const RenderPipelineDesc& d) = 0;
  virtual CommandEncoder CreateCommandEncoder(const char* label) = 0;
  virtual void PushValidationScope() = 0;
  virtual bool PopValidationScopeBlocking() = 0;
  virtual void ProcessEvents() = 0;
  virtual void WaitIdle() = 0;
  // The blocking read of a MapRead buffer whose producing work is submitted.
  virtual bool ReadBufferBlocking(const Buffer& src, uint64_t offset, void* out,
                                  size_t size) = 0;
  virtual MapTicket MapReadDeferred(const Buffer& b, uint64_t offset, uint64_t size) = 0;
};

}  // namespace rhi
