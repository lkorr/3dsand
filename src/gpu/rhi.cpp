// rhi.cpp — the public seam classes, forwarding to the abstract impl layer.
//
// Every method here is one virtual hop into rhi_impl.h; which backend answers
// is decided by which subclass the Device was created from (rhi_dawn.cpp /
// rhi_vk.cpp). Nothing in this file makes a decision, so a reader auditing the
// seam only has to read the two backends.

#include "gpu/rhi_impl.h"

namespace rhi {

// -------------------------------------------------------------- Buffer ----

uint64_t Buffer::Size() const { return p_ ? p_->size : 0; }

// ------------------------------------------------------------- Texture ----

TextureView Texture::CreateView() const {
  if (!*this) return {};
  return Get()->CreateView();
}

// --------------------------------------------------------- ComputePass ----

void ComputePass::SetPipeline(const ComputePipeline& pipe) const { p_->SetPipeline(pipe); }

void ComputePass::SetBindGroup(uint32_t index, const BindGroup& bg) const {
  p_->SetBindGroup(index, bg, 0, nullptr);
}

void ComputePass::SetBindGroup(uint32_t index, const BindGroup& bg,
                               uint32_t dynamicOffsetCount,
                               const uint32_t* dynamicOffsets) const {
  p_->SetBindGroup(index, bg, dynamicOffsetCount, dynamicOffsets);
}

void ComputePass::DispatchWorkgroups(uint32_t x, uint32_t y, uint32_t z) const {
  p_->Dispatch(x, y, z);
}

void ComputePass::DispatchWorkgroupsIndirect(const Buffer& args, uint64_t offset) const {
  p_->DispatchIndirect(args, offset);
}

void ComputePass::End() const { p_->End(); }

// ---------------------------------------------------------- RenderPass ----

void RenderPass::SetPipeline(const RenderPipeline& pipe) const { p_->SetPipeline(pipe); }

void RenderPass::SetBindGroup(uint32_t index, const BindGroup& bg) const {
  p_->SetBindGroup(index, bg);
}

void RenderPass::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                      uint32_t firstInstance) const {
  p_->Draw(vertexCount, instanceCount, firstVertex, firstInstance);
}

void RenderPass::DrawIndirect(const Buffer& args, uint64_t offset) const {
  p_->DrawIndirect(args, offset);
}

void RenderPass::End() const { p_->End(); }

// ------------------------------------------------------ CommandEncoder ----

void CommandEncoder::ClearBuffer(const Buffer& b, uint64_t offset, uint64_t size) const {
  p_->ClearBuffer(b, offset, size);
}

void CommandEncoder::CopyBufferToBuffer(const Buffer& src, uint64_t srcOffset,
                                        const Buffer& dst, uint64_t dstOffset,
                                        uint64_t size) const {
  p_->CopyBufferToBuffer(src, srcOffset, dst, dstOffset, size);
}

void CommandEncoder::CopyTracked(pass::Buf srcId, const Buffer& src, uint64_t srcOffset,
                                 const Buffer& dst, uint64_t dstOffset,
                                 uint64_t size) const {
  p_->CopyTracked(srcId, src, srcOffset, dst, dstOffset, size);
}
void CommandEncoder::CopyRenderWritten(const Buffer& src, uint64_t srcOffset,
                                       const Buffer& dst, uint64_t dstOffset,
                                       uint64_t size) const {
  p_->CopyRenderWritten(src, srcOffset, dst, dstOffset, size);
}

void CommandEncoder::FillTracked(pass::Buf id, const Buffer& b) const {
  p_->FillTracked(id, b);
}

void CommandEncoder::FillTracked(pass::Buf id, const Buffer& b, uint64_t offset,
                                 uint64_t size, uint32_t pattern) const {
  p_->FillTrackedRange(id, b, offset, size, pattern);
}

void CommandEncoder::CopyTextureToBuffer(const TexelCopyTexture& src,
                                         const TexelCopyBuffer& dst,
                                         const Extent3D& extent) const {
  p_->CopyTextureToBuffer(src, dst, extent);
}

void CommandEncoder::ResolveQuerySet(const QuerySet& qs, uint32_t firstQuery,
                                     uint32_t queryCount, const Buffer& dst,
                                     uint64_t dstOffset) const {
  p_->ResolveQuerySet(qs, firstQuery, queryCount, dst, dstOffset);
}

void CommandEncoder::WriteTimestamp(const QuerySet& qs, uint32_t index,
                                    bool bottom) const {
  p_->WriteTimestamp(qs, index, bottom);
}

ComputePass CommandEncoder::BeginComputePass(const char* label) const {
  return p_->BeginComputePass(label, nullptr);
}

ComputePass CommandEncoder::BeginComputePass(const char* label,
                                             const PassTimestampWrites& ts) const {
  return p_->BeginComputePass(label, &ts);
}

RenderPass CommandEncoder::BeginRenderPass(const RenderPassDesc& d) const {
  return p_->BeginRenderPass(d);
}

CommandBuffer CommandEncoder::Finish() const { return p_->Finish(); }

// --------------------------------------------------------------- Queue ----

void Queue::WriteBuffer(const Buffer& b, uint64_t offset, const void* data,
                        size_t size) const {
  p_->WriteBuffer(b, offset, data, size);
}

void Queue::Submit(const CommandBuffer& cmd) const { p_->Submit(1, &cmd); }

void Queue::Submit(uint32_t count, const CommandBuffer* cmds) const {
  p_->Submit(count, cmds);
}

// -------------------------------------------------------------- Device ----

BackendKind Device::Kind() const { return p_->Kind(); }

Queue Device::GetQueue() const { return p_->GetQueue(); }

Buffer Device::CreateBuffer(uint64_t size, BufferUsage usage, const char* label) const {
  return p_->CreateBuffer(size, usage, label);
}

Texture Device::CreateTexture(const Extent3D& size, TextureFormat format,
                              TextureUsage usage, const char* label) const {
  return p_->CreateTexture(size, format, usage, label);
}

QuerySet Device::CreateTimestampQuerySet(uint32_t count, const char* label) const {
  return p_->CreateTimestampQuerySet(count, label);
}

BindGroupLayout Device::CreateBindGroupLayout(const BindGroupLayoutEntry* entries,
                                              size_t count) const {
  return p_->CreateBindGroupLayout(entries, count);
}

PipelineLayout Device::CreatePipelineLayout(const BindGroupLayout* groups,
                                            size_t count) const {
  return p_->CreatePipelineLayout(groups, count);
}

BindGroup Device::CreateBindGroup(const BindGroupLayout& layout,
                                  const BindGroupEntry* entries, size_t count,
                                  const char* label) const {
  return p_->CreateBindGroup(layout, entries, count, label);
}

ShaderModule Device::CreateShaderModule(const std::string& wgsl, const char* label) const {
  return p_->CreateShaderModule(wgsl, label);
}

ComputePipeline Device::CreateComputePipeline(const PipelineLayout& layout,
                                              const ShaderModule& module,
                                              const char* entry, const char* label) const {
  return p_->CreateComputePipeline(layout, module, entry, label);
}

RenderPipeline Device::CreateRenderPipeline(const RenderPipelineDesc& d) const {
  return p_->CreateRenderPipeline(d);
}

CommandEncoder Device::CreateCommandEncoder(const char* label) const {
  return p_->CreateCommandEncoder(label);
}

void Device::PushValidationScope() const { p_->PushValidationScope(); }

bool Device::PopValidationScopeBlocking() const { return p_->PopValidationScopeBlocking(); }

void Device::ProcessEvents() const { p_->ProcessEvents(); }

bool Device::WaitOldestPendingMap() const { return p_->WaitOldestPendingMap(); }

void Device::WaitIdle() const { p_->WaitIdle(); }

// -------------------------------------------------------- read helpers ----

bool ReadBufferBlocking(const Device& dev, const Buffer& src, uint64_t offset,
                        void* out, size_t size) {
  if (!dev || !src || size == 0) return false;
  return dev.Get()->ReadBufferBlocking(src, offset, out, size);
}

// Backend-generic by construction: staging + copy + submit + blocking read all
// go through the seam, so this is defined once rather than per backend.
bool ReadbackBlocking(const Device& dev, const Queue& queue, const Buffer& src,
                      uint64_t srcOffset, void* out, size_t size, const char* label) {
  if (!dev || !queue || !src || size == 0) return false;
  Buffer staging =
      dev.CreateBuffer(size, BufferUsage::MapRead | BufferUsage::CopyDst, label);
  if (!staging) return false;
  CommandEncoder enc = dev.CreateCommandEncoder();
  enc.CopyBufferToBuffer(src, srcOffset, staging, 0, size);
  queue.Submit(enc.Finish());
  return ReadBufferBlocking(dev, staging, 0, out, size);
}

void MapReadAsync(const Buffer& b, uint64_t offset, uint64_t size,
                  std::function<void(const void*)> done) {
  if (!b) {
    done(nullptr);
    return;
  }
  b.Get()->MapReadAsync(offset, size, std::move(done));
}

// -------------------------------------------------------------- tickets ----

bool MapTicket::Ready() const { return p_ && p_->Ready(); }
void MapTicket::Wait() const {
  if (p_) p_->Wait();
}
bool MapTicket::Succeeded() const { return p_ && p_->Succeeded(); }
const void* MapTicket::Data() const { return p_ ? p_->Data() : nullptr; }
void MapTicket::Unmap() const {
  if (p_) p_->Unmap();
}

MapTicket MapReadDeferred(const Device& dev, const Buffer& b, uint64_t offset,
                          uint64_t size) {
  if (!dev || !b) return {};
  return dev.Get()->MapReadDeferred(b, offset, size);
}

}  // namespace rhi
