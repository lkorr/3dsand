// rhi_dawn.cpp — Dawn passthrough implementation of the rhi.h seam.
//
// Every function here is a translation of seam POD into a wgpu descriptor and a
// forwarded call. No decisions, no caching, no reordering: phase 2a's whole
// claim is that the command stream is byte-identical to what the call sites
// built by hand before, and that is only auditable if this file is boring.

#include "gpu/rhi_dawn.h"

#include <cstdio>
#include <vector>

namespace rhi {
namespace dawn {

// --------------------------------------------------------- enum mapping ----

wgpu::TextureFormat ToWgpu(TextureFormat f) {
  switch (f) {
    case TextureFormat::RGBA8Unorm: return wgpu::TextureFormat::RGBA8Unorm;
    case TextureFormat::BGRA8Unorm: return wgpu::TextureFormat::BGRA8Unorm;
    case TextureFormat::Depth32Float: return wgpu::TextureFormat::Depth32Float;
    default: return wgpu::TextureFormat::Undefined;
  }
}

TextureFormat FromWgpu(wgpu::TextureFormat f) {
  switch (f) {
    case wgpu::TextureFormat::RGBA8Unorm: return TextureFormat::RGBA8Unorm;
    case wgpu::TextureFormat::BGRA8Unorm: return TextureFormat::BGRA8Unorm;
    case wgpu::TextureFormat::Depth32Float: return TextureFormat::Depth32Float;
    default: return TextureFormat::Undefined;
  }
}

static wgpu::BufferUsage ToWgpu(BufferUsage u) {
  wgpu::BufferUsage r = wgpu::BufferUsage::None;
  auto has = [&](BufferUsage b) { return Any(u, b); };
  if (has(BufferUsage::MapRead)) r |= wgpu::BufferUsage::MapRead;
  if (has(BufferUsage::MapWrite)) r |= wgpu::BufferUsage::MapWrite;
  if (has(BufferUsage::CopySrc)) r |= wgpu::BufferUsage::CopySrc;
  if (has(BufferUsage::CopyDst)) r |= wgpu::BufferUsage::CopyDst;
  if (has(BufferUsage::Index)) r |= wgpu::BufferUsage::Index;
  if (has(BufferUsage::Vertex)) r |= wgpu::BufferUsage::Vertex;
  if (has(BufferUsage::Uniform)) r |= wgpu::BufferUsage::Uniform;
  if (has(BufferUsage::Storage)) r |= wgpu::BufferUsage::Storage;
  if (has(BufferUsage::Indirect)) r |= wgpu::BufferUsage::Indirect;
  if (has(BufferUsage::QueryResolve)) r |= wgpu::BufferUsage::QueryResolve;
  return r;
}

static wgpu::TextureUsage ToWgpu(TextureUsage u) {
  wgpu::TextureUsage r = wgpu::TextureUsage::None;
  auto has = [&](TextureUsage b) { return ((uint32_t)u & (uint32_t)b) != 0; };
  if (has(TextureUsage::CopySrc)) r |= wgpu::TextureUsage::CopySrc;
  if (has(TextureUsage::CopyDst)) r |= wgpu::TextureUsage::CopyDst;
  if (has(TextureUsage::TextureBinding)) r |= wgpu::TextureUsage::TextureBinding;
  if (has(TextureUsage::StorageBinding)) r |= wgpu::TextureUsage::StorageBinding;
  if (has(TextureUsage::RenderAttachment)) r |= wgpu::TextureUsage::RenderAttachment;
  return r;
}

static wgpu::ShaderStage ToWgpu(ShaderStage s) {
  wgpu::ShaderStage r = wgpu::ShaderStage::None;
  if ((uint32_t)s & (uint32_t)ShaderStage::Vertex) r |= wgpu::ShaderStage::Vertex;
  if ((uint32_t)s & (uint32_t)ShaderStage::Fragment) r |= wgpu::ShaderStage::Fragment;
  if ((uint32_t)s & (uint32_t)ShaderStage::Compute) r |= wgpu::ShaderStage::Compute;
  return r;
}

static wgpu::BufferBindingType ToWgpu(BufferBindingType t) {
  switch (t) {
    case BufferBindingType::Uniform: return wgpu::BufferBindingType::Uniform;
    case BufferBindingType::ReadOnlyStorage: return wgpu::BufferBindingType::ReadOnlyStorage;
    default: return wgpu::BufferBindingType::Storage;
  }
}

static wgpu::CompareFunction ToWgpu(CompareFunction c) {
  switch (c) {
    case CompareFunction::Never: return wgpu::CompareFunction::Never;
    case CompareFunction::Less: return wgpu::CompareFunction::Less;
    case CompareFunction::LessEqual: return wgpu::CompareFunction::LessEqual;
    case CompareFunction::Greater: return wgpu::CompareFunction::Greater;
    case CompareFunction::GreaterEqual: return wgpu::CompareFunction::GreaterEqual;
    case CompareFunction::Equal: return wgpu::CompareFunction::Equal;
    case CompareFunction::NotEqual: return wgpu::CompareFunction::NotEqual;
    default: return wgpu::CompareFunction::Always;
  }
}

static wgpu::CullMode ToWgpu(CullMode c) {
  switch (c) {
    case CullMode::Front: return wgpu::CullMode::Front;
    case CullMode::Back: return wgpu::CullMode::Back;
    default: return wgpu::CullMode::None;
  }
}

static wgpu::PrimitiveTopology ToWgpu(PrimitiveTopology t) {
  switch (t) {
    case PrimitiveTopology::PointList: return wgpu::PrimitiveTopology::PointList;
    case PrimitiveTopology::LineList: return wgpu::PrimitiveTopology::LineList;
    case PrimitiveTopology::LineStrip: return wgpu::PrimitiveTopology::LineStrip;
    case PrimitiveTopology::TriangleStrip: return wgpu::PrimitiveTopology::TriangleStrip;
    default: return wgpu::PrimitiveTopology::TriangleList;
  }
}

static wgpu::LoadOp ToWgpu(LoadOp o) {
  return o == LoadOp::Load ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
}

static wgpu::StoreOp ToWgpu(StoreOp o) {
  return o == StoreOp::Discard ? wgpu::StoreOp::Discard : wgpu::StoreOp::Store;
}

static wgpu::BlendFactor ToWgpu(BlendFactor f) {
  switch (f) {
    case BlendFactor::Zero: return wgpu::BlendFactor::Zero;
    case BlendFactor::SrcAlpha: return wgpu::BlendFactor::SrcAlpha;
    case BlendFactor::OneMinusSrcAlpha: return wgpu::BlendFactor::OneMinusSrcAlpha;
    case BlendFactor::Src: return wgpu::BlendFactor::Src;
    case BlendFactor::OneMinusSrc: return wgpu::BlendFactor::OneMinusSrc;
    case BlendFactor::Dst: return wgpu::BlendFactor::Dst;
    case BlendFactor::OneMinusDst: return wgpu::BlendFactor::OneMinusDst;
    default: return wgpu::BlendFactor::One;
  }
}

static wgpu::BlendOperation ToWgpu(BlendOperation o) {
  switch (o) {
    case BlendOperation::Subtract: return wgpu::BlendOperation::Subtract;
    case BlendOperation::ReverseSubtract: return wgpu::BlendOperation::ReverseSubtract;
    case BlendOperation::Min: return wgpu::BlendOperation::Min;
    case BlendOperation::Max: return wgpu::BlendOperation::Max;
    default: return wgpu::BlendOperation::Add;
  }
}

// ------------------------------------------------------- wrap / unwrap ----

Device WrapDevice(const wgpu::Device& d, const wgpu::Instance& inst) {
  if (!d) return {};
  auto p = std::make_shared<DeviceImpl>();
  p->h = d;
  p->instance = inst;
  return Device(std::move(p));
}

Buffer WrapBuffer(const wgpu::Buffer& b, uint64_t size) {
  if (!b) return {};
  auto p = std::make_shared<BufferImpl>();
  p->h = b;
  p->size = size;
  return Buffer(std::move(p));
}

Texture WrapTexture(const wgpu::Texture& t) {
  if (!t) return {};
  auto p = std::make_shared<TextureImpl>();
  p->h = t;
  return Texture(Texture_(std::move(p)));
}

TextureView WrapTextureView(const wgpu::TextureView& v) {
  if (!v) return {};
  auto p = std::make_shared<TextureViewImpl>();
  p->h = v;
  return TextureView(std::move(p));
}

Queue WrapQueue(const wgpu::Queue& q) {
  if (!q) return {};
  auto p = std::make_shared<QueueImpl>();
  p->h = q;
  return Queue(std::move(p));
}

static const wgpu::Device kNullDevice{};
static const wgpu::Instance kNullInstance{};
static const wgpu::Buffer kNullBuffer{};
static const wgpu::Texture kNullTexture{};
static const wgpu::TextureView kNullView{};
static const wgpu::Queue kNullQueue{};
static const wgpu::RenderPassEncoder kNullRenderPass{};
static const wgpu::CommandEncoder kNullEncoder{};

const wgpu::Device& Native(const Device& d) {
  return d ? d.Get()->h : kNullDevice;
}
const wgpu::Instance& NativeInstance(const Device& d) {
  return d ? d.Get()->instance : kNullInstance;
}
const wgpu::Buffer& Native(const Buffer& b) { return b ? b.Get()->h : kNullBuffer; }
const wgpu::Texture& Native(const Texture& t) { return t ? t.Get()->h : kNullTexture; }
const wgpu::TextureView& Native(const TextureView& v) {
  return v ? v.Get()->h : kNullView;
}
const wgpu::Queue& Native(const Queue& q) { return q ? q.Get()->h : kNullQueue; }
const wgpu::RenderPassEncoder& Native(const RenderPass& p) {
  return p ? p.Get()->h : kNullRenderPass;
}
const wgpu::CommandEncoder& Native(const CommandEncoder& e) {
  return e ? e.Get()->h : kNullEncoder;
}

}  // namespace dawn

// -------------------------------------------------------------- Buffer ----

uint64_t Buffer::Size() const { return p_ ? p_->size : 0; }

// ------------------------------------------------------------- Texture ----

TextureView Texture::CreateView() const {
  if (!*this) return {};
  return dawn::WrapTextureView(Get()->h.CreateView());
}

// --------------------------------------------------------- ComputePass ----

void ComputePass::SetPipeline(const ComputePipeline& pipe) const {
  p_->h.SetPipeline(pipe.Get()->h);
}

void ComputePass::SetBindGroup(uint32_t index, const BindGroup& bg) const {
  p_->h.SetBindGroup(index, bg.Get()->h);
}

void ComputePass::SetBindGroup(uint32_t index, const BindGroup& bg,
                               uint32_t dynamicOffsetCount,
                               const uint32_t* dynamicOffsets) const {
  p_->h.SetBindGroup(index, bg.Get()->h, dynamicOffsetCount, dynamicOffsets);
}

void ComputePass::DispatchWorkgroups(uint32_t x, uint32_t y, uint32_t z) const {
  p_->h.DispatchWorkgroups(x, y, z);
}

void ComputePass::DispatchWorkgroupsIndirect(const Buffer& args, uint64_t offset) const {
  p_->h.DispatchWorkgroupsIndirect(args.Get()->h, offset);
}

void ComputePass::End() const { p_->h.End(); }

// ---------------------------------------------------------- RenderPass ----

void RenderPass::SetPipeline(const RenderPipeline& pipe) const {
  p_->h.SetPipeline(pipe.Get()->h);
}

void RenderPass::SetBindGroup(uint32_t index, const BindGroup& bg) const {
  p_->h.SetBindGroup(index, bg.Get()->h);
}

void RenderPass::Draw(uint32_t vertexCount, uint32_t instanceCount,
                      uint32_t firstVertex, uint32_t firstInstance) const {
  p_->h.Draw(vertexCount, instanceCount, firstVertex, firstInstance);
}

void RenderPass::DrawIndirect(const Buffer& args, uint64_t offset) const {
  p_->h.DrawIndirect(args.Get()->h, offset);
}

void RenderPass::End() const { p_->h.End(); }

// ------------------------------------------------------ CommandEncoder ----

void CommandEncoder::ClearBuffer(const Buffer& b, uint64_t offset, uint64_t size) const {
  p_->h.ClearBuffer(b.Get()->h, offset,
                    size == kWholeSize ? wgpu::kWholeSize : size);
}

void CommandEncoder::CopyBufferToBuffer(const Buffer& src, uint64_t srcOffset,
                                        const Buffer& dst, uint64_t dstOffset,
                                        uint64_t size) const {
  p_->h.CopyBufferToBuffer(src.Get()->h, srcOffset, dst.Get()->h, dstOffset, size);
}

void CommandEncoder::CopyTextureToBuffer(const TexelCopyTexture& src,
                                         const TexelCopyBuffer& dst,
                                         const Extent3D& extent) const {
  wgpu::TexelCopyTextureInfo s{};
  s.texture = src.texture.Get()->h;
  s.mipLevel = src.mipLevel;
  s.origin = {src.originX, src.originY, src.originZ};
  wgpu::TexelCopyBufferInfo d{};
  d.buffer = dst.buffer.Get()->h;
  d.layout.offset = dst.offset;
  d.layout.bytesPerRow = dst.bytesPerRow;
  d.layout.rowsPerImage = dst.rowsPerImage;
  wgpu::Extent3D e{extent.width, extent.height, extent.depthOrArrayLayers};
  p_->h.CopyTextureToBuffer(&s, &d, &e);
}

void CommandEncoder::ResolveQuerySet(const QuerySet& qs, uint32_t firstQuery,
                                     uint32_t queryCount, const Buffer& dst,
                                     uint64_t dstOffset) const {
  p_->h.ResolveQuerySet(qs.Get()->h, firstQuery, queryCount, dst.Get()->h, dstOffset);
}

ComputePass CommandEncoder::BeginComputePass(const char* label) const {
  auto impl = std::make_shared<ComputePassImpl>();
  if (label) {
    wgpu::ComputePassDescriptor d{};
    d.label = label;
    impl->h = p_->h.BeginComputePass(&d);
  } else {
    impl->h = p_->h.BeginComputePass();
  }
  return ComputePass(std::move(impl));
}

ComputePass CommandEncoder::BeginComputePass(const char* label,
                                             const PassTimestampWrites& ts) const {
  wgpu::PassTimestampWrites tw{};
  tw.querySet = ts.querySet.Get()->h;
  tw.beginningOfPassWriteIndex = ts.beginIndex;
  tw.endOfPassWriteIndex = ts.endIndex;
  wgpu::ComputePassDescriptor d{};
  d.label = label;
  d.timestampWrites = &tw;
  auto impl = std::make_shared<ComputePassImpl>();
  impl->h = p_->h.BeginComputePass(&d);
  return ComputePass(std::move(impl));
}

RenderPass CommandEncoder::BeginRenderPass(const RenderPassDesc& desc) const {
  wgpu::RenderPassColorAttachment ca{};
  ca.view = desc.color.view.Get()->h;
  ca.loadOp = dawn::ToWgpu(desc.color.loadOp);
  ca.storeOp = dawn::ToWgpu(desc.color.storeOp);
  ca.clearValue = {desc.color.clearValue[0], desc.color.clearValue[1],
                   desc.color.clearValue[2], desc.color.clearValue[3]};

  wgpu::RenderPassDepthStencilAttachment da{};
  if (desc.hasDepth) {
    da.view = desc.depth.view.Get()->h;
    da.depthLoadOp = dawn::ToWgpu(desc.depth.loadOp);
    da.depthStoreOp = dawn::ToWgpu(desc.depth.storeOp);
    da.depthClearValue = desc.depth.clearValue;
  }

  wgpu::RenderPassDescriptor d{};
  d.label = desc.label;
  d.colorAttachmentCount = 1;
  d.colorAttachments = &ca;
  if (desc.hasDepth) d.depthStencilAttachment = &da;

  auto impl = std::make_shared<RenderPassImpl>();
  impl->h = p_->h.BeginRenderPass(&d);
  return RenderPass(std::move(impl));
}

CommandBuffer CommandEncoder::Finish() const {
  auto impl = std::make_shared<CommandBufferImpl>();
  impl->h = p_->h.Finish();
  return CommandBuffer(std::move(impl));
}

// --------------------------------------------------------------- Queue ----

void Queue::WriteBuffer(const Buffer& b, uint64_t offset, const void* data,
                        size_t size) const {
  p_->h.WriteBuffer(b.Get()->h, offset, data, size);
}

void Queue::Submit(const CommandBuffer& cmd) const {
  wgpu::CommandBuffer c = cmd.Get()->h;
  p_->h.Submit(1, &c);
}

void Queue::Submit(uint32_t count, const CommandBuffer* cmds) const {
  std::vector<wgpu::CommandBuffer> native;
  native.reserve(count);
  for (uint32_t i = 0; i < count; i++) native.push_back(cmds[i].Get()->h);
  p_->h.Submit(count, native.data());
}

// -------------------------------------------------------------- Device ----

Queue Device::GetQueue() const { return dawn::WrapQueue(p_->h.GetQueue()); }

Buffer Device::CreateBuffer(uint64_t size, BufferUsage usage, const char* label) const {
  wgpu::BufferDescriptor d{};
  d.size = size;
  d.usage = dawn::ToWgpu(usage);
  d.label = label;
  return dawn::WrapBuffer(p_->h.CreateBuffer(&d), size);
}

Texture Device::CreateTexture(const Extent3D& size, TextureFormat format,
                              TextureUsage usage, const char* label) const {
  wgpu::TextureDescriptor d{};
  d.size = {size.width, size.height, size.depthOrArrayLayers};
  d.format = dawn::ToWgpu(format);
  d.usage = dawn::ToWgpu(usage);
  d.label = label;
  return dawn::WrapTexture(p_->h.CreateTexture(&d));
}

QuerySet Device::CreateTimestampQuerySet(uint32_t count, const char* label) const {
  wgpu::QuerySetDescriptor d{};
  d.type = wgpu::QueryType::Timestamp;
  d.count = count;
  d.label = label;
  auto impl = std::make_shared<QuerySetImpl>();
  impl->h = p_->h.CreateQuerySet(&d);
  if (!impl->h) return {};
  return QuerySet(std::move(impl));
}

BindGroupLayout Device::CreateBindGroupLayout(const BindGroupLayoutEntry* entries,
                                              size_t count) const {
  std::vector<wgpu::BindGroupLayoutEntry> ne(count);
  for (size_t i = 0; i < count; i++) {
    ne[i] = {};
    ne[i].binding = entries[i].binding;
    ne[i].visibility = dawn::ToWgpu(entries[i].visibility);
    ne[i].buffer.type = dawn::ToWgpu(entries[i].type);
    ne[i].buffer.hasDynamicOffset = entries[i].hasDynamicOffset;
  }
  wgpu::BindGroupLayoutDescriptor d{};
  d.entryCount = (uint32_t)count;
  d.entries = ne.data();
  auto impl = std::make_shared<BindGroupLayoutImpl>();
  impl->h = p_->h.CreateBindGroupLayout(&d);
  return BindGroupLayout(std::move(impl));
}

PipelineLayout Device::CreatePipelineLayout(const BindGroupLayout* groups,
                                            size_t count) const {
  std::vector<wgpu::BindGroupLayout> ng(count);
  for (size_t i = 0; i < count; i++) ng[i] = groups[i].Get()->h;
  wgpu::PipelineLayoutDescriptor d{};
  d.bindGroupLayoutCount = (uint32_t)count;
  d.bindGroupLayouts = ng.data();
  auto impl = std::make_shared<PipelineLayoutImpl>();
  impl->h = p_->h.CreatePipelineLayout(&d);
  return PipelineLayout(std::move(impl));
}

BindGroup Device::CreateBindGroup(const BindGroupLayout& layout,
                                  const BindGroupEntry* entries, size_t count,
                                  const char* label) const {
  std::vector<wgpu::BindGroupEntry> ne(count);
  for (size_t i = 0; i < count; i++) {
    ne[i] = {};
    ne[i].binding = entries[i].binding;
    ne[i].buffer = entries[i].buffer.Get()->h;
    ne[i].offset = entries[i].offset;
    // size 0 means "to the end of the buffer" in the seam; wgpu spells that
    // kWholeSize, and leaving it 0 would be a zero-length binding.
    ne[i].size = entries[i].size ? entries[i].size
                                 : (entries[i].buffer.Size() - entries[i].offset);
  }
  wgpu::BindGroupDescriptor d{};
  d.layout = layout.Get()->h;
  d.entryCount = (uint32_t)count;
  d.entries = ne.data();
  d.label = label;
  auto impl = std::make_shared<BindGroupImpl>();
  impl->h = p_->h.CreateBindGroup(&d);
  return BindGroup(std::move(impl));
}

ShaderModule Device::CreateShaderModule(const std::string& wgsl,
                                        const char* label) const {
  wgpu::ShaderSourceWGSL src{};
  src.code = wgsl.c_str();
  wgpu::ShaderModuleDescriptor d{};
  d.nextInChain = &src;
  d.label = label;
  auto impl = std::make_shared<ShaderModuleImpl>();
  impl->h = p_->h.CreateShaderModule(&d);
  if (!impl->h) return {};
  return ShaderModule(std::move(impl));
}

ComputePipeline Device::CreateComputePipeline(const PipelineLayout& layout,
                                              const ShaderModule& module,
                                              const char* entry,
                                              const char* label) const {
  wgpu::ComputePipelineDescriptor d{};
  d.layout = layout.Get()->h;
  d.compute.module = module.Get()->h;
  d.compute.entryPoint = entry;
  d.label = label;
  auto impl = std::make_shared<ComputePipelineImpl>();
  impl->h = p_->h.CreateComputePipeline(&d);
  return ComputePipeline(std::move(impl));
}

RenderPipeline Device::CreateRenderPipeline(const RenderPipelineDesc& desc) const {
  wgpu::BlendState blend{};
  if (desc.blend) {
    blend.color.srcFactor = dawn::ToWgpu(desc.blend->color.srcFactor);
    blend.color.dstFactor = dawn::ToWgpu(desc.blend->color.dstFactor);
    blend.color.operation = dawn::ToWgpu(desc.blend->color.operation);
    blend.alpha.srcFactor = dawn::ToWgpu(desc.blend->alpha.srcFactor);
    blend.alpha.dstFactor = dawn::ToWgpu(desc.blend->alpha.dstFactor);
    blend.alpha.operation = dawn::ToWgpu(desc.blend->alpha.operation);
  }

  wgpu::ColorTargetState ct{};
  ct.format = dawn::ToWgpu(desc.colorFormat);
  if (desc.blend) ct.blend = &blend;

  wgpu::DepthStencilState ds{};
  ds.format = dawn::ToWgpu(desc.depth.format);
  ds.depthWriteEnabled = desc.depth.depthWriteEnabled;
  ds.depthCompare = dawn::ToWgpu(desc.depth.depthCompare);

  wgpu::FragmentState fs{};
  fs.module = desc.fragmentModule.Get()->h;
  fs.entryPoint = desc.fragmentEntry;
  fs.targetCount = 1;
  fs.targets = &ct;

  wgpu::RenderPipelineDescriptor d{};
  d.label = desc.label;
  d.layout = desc.layout.Get()->h;
  d.vertex.module = desc.vertexModule.Get()->h;
  d.vertex.entryPoint = desc.vertexEntry;
  d.primitive.topology = dawn::ToWgpu(desc.topology);
  d.primitive.cullMode = dawn::ToWgpu(desc.cullMode);
  d.depthStencil = &ds;
  d.fragment = &fs;

  auto impl = std::make_shared<RenderPipelineImpl>();
  impl->h = p_->h.CreateRenderPipeline(&d);
  return RenderPipeline(std::move(impl));
}

CommandEncoder Device::CreateCommandEncoder(const char* label) const {
  auto impl = std::make_shared<CommandEncoderImpl>();
  if (label) {
    wgpu::CommandEncoderDescriptor d{};
    d.label = label;
    impl->h = p_->h.CreateCommandEncoder(&d);
  } else {
    impl->h = p_->h.CreateCommandEncoder();
  }
  return CommandEncoder(std::move(impl));
}

void Device::PushValidationScope() const {
  p_->h.PushErrorScope(wgpu::ErrorFilter::Validation);
}

bool Device::PopValidationScopeBlocking() const {
  bool hadError = false;
  wgpu::Future f = p_->h.PopErrorScope(
      wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView msg) {
        if (type != wgpu::ErrorType::NoError) {
          hadError = true;
          std::fprintf(stderr, "validation error: %.*s\n", (int)msg.length, msg.data);
        }
      });
  p_->instance.WaitAny(f, UINT64_MAX);
  return hadError;
}

void Device::ProcessEvents() const { p_->instance.ProcessEvents(); }

void Device::WaitIdle() const {
  wgpu::Future f = p_->h.GetQueue().OnSubmittedWorkDone(
      wgpu::CallbackMode::WaitAnyOnly, [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {});
  p_->instance.WaitAny(f, UINT64_MAX);
}

// -------------------------------------------------------- read helpers ----

bool ReadBufferBlocking(const Device& dev, const Buffer& src, uint64_t offset,
                        void* out, size_t size) {
  if (!dev || !src || size == 0) return false;
  bool ok = false;
  wgpu::Future f = dawn::Native(src).MapAsync(
      wgpu::MapMode::Read, offset, size, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
        if (status != wgpu::MapAsyncStatus::Success) return;
        const void* p = dawn::Native(src).GetConstMappedRange(offset, size);
        if (!p) return;
        std::memcpy(out, p, size);
        ok = true;
      });
  dawn::NativeInstance(dev).WaitAny(f, UINT64_MAX);
  if (ok) dawn::Native(src).Unmap();
  return ok;
}

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
  const wgpu::Buffer& nb = dawn::Native(b);
  nb.MapAsync(wgpu::MapMode::Read, offset, size, wgpu::CallbackMode::AllowProcessEvents,
              [nb, offset, size, done = std::move(done)](wgpu::MapAsyncStatus status,
                                                         wgpu::StringView) {
                if (status != wgpu::MapAsyncStatus::Success) {
                  done(nullptr);
                  return;
                }
                done(nb.GetConstMappedRange(offset, size));
                nb.Unmap();
              });
}

// -------------------------------------------------------------- tickets ----

struct MapTicketImpl {
  wgpu::Buffer buf;
  wgpu::Instance instance;
  wgpu::Future future{};
  uint64_t offset = 0, size = 0;
  // 0 = pending, 1 = mapped, 2 = failed. Heap-owned via the shared_ptr so the
  // callback outlives any container reshuffle at the call site.
  std::shared_ptr<uint32_t> status;
};

MapTicket MapReadDeferred(const Device& dev, const Buffer& b, uint64_t offset,
                          uint64_t size) {
  auto impl = std::make_shared<MapTicketImpl>();
  impl->buf = dawn::Native(b);
  impl->instance = dawn::NativeInstance(dev);
  impl->offset = offset;
  impl->size = size;
  impl->status = std::make_shared<uint32_t>(0);
  impl->future = impl->buf.MapAsync(
      wgpu::MapMode::Read, offset, size, wgpu::CallbackMode::WaitAnyOnly,
      [st = impl->status](wgpu::MapAsyncStatus status, wgpu::StringView) {
        *st = status == wgpu::MapAsyncStatus::Success ? 1u : 2u;
      });
  return MapTicket(std::move(impl));
}

bool MapTicket::Ready() const {
  if (!p_) return false;
  return p_->instance.WaitAny(p_->future, 0) == wgpu::WaitStatus::Success;
}

void MapTicket::Wait() const {
  if (p_) p_->instance.WaitAny(p_->future, UINT64_MAX);
}

bool MapTicket::Succeeded() const { return p_ && *p_->status == 1u; }

const void* MapTicket::Data() const {
  if (!p_ || *p_->status != 1u) return nullptr;
  return p_->buf.GetConstMappedRange(p_->offset, p_->size);
}

void MapTicket::Unmap() const {
  if (p_ && *p_->status == 1u) p_->buf.Unmap();
}

}  // namespace rhi
