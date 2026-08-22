// rhi_dawn.cpp — Dawn passthrough implementation of the rhi.h seam.
//
// Every method here is a translation of seam POD into a wgpu descriptor and a
// forwarded call. No decisions, no caching, no reordering: the command stream
// must stay byte-identical to what the call sites built by hand before the
// seam existed (and before the seam went polymorphic in phase 4a), and that is
// only auditable if this file is boring. The pinned determinism hash in
// tests/baseline.json is the mechanical check.

#include "gpu/rhi_dawn.h"

#include <cstdio>
#include <cstring>
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

// ------------------------------------------------------- impl structs ----
//
// Each subclass holds the wgpu:: handle and forwards. The Dawn methods live on
// the impls now (phase 4a polymorphism), but the CALLS they make are the same
// calls the flat seam made — nothing about the recorded command stream changed.

struct DawnBuffer final : BufferImpl {
  wgpu::Buffer h;
  void MapReadAsync(uint64_t offset, uint64_t size,
                    std::function<void(const void*)> done) override {
    wgpu::Buffer nb = h;
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
};

struct DawnTextureView final : TextureViewImpl { wgpu::TextureView h; };

struct DawnTexture final : TextureImpl {
  wgpu::Texture h;
  TextureView CreateView() override {
    auto p = std::make_shared<DawnTextureView>();
    p->h = h.CreateView();
    return TextureView(std::move(p));
  }
};

struct DawnShaderModule final : ShaderModuleImpl { wgpu::ShaderModule h; };
struct DawnBindGroupLayout final : BindGroupLayoutImpl { wgpu::BindGroupLayout h; };
struct DawnBindGroup final : BindGroupImpl { wgpu::BindGroup h; };
struct DawnPipelineLayout final : PipelineLayoutImpl { wgpu::PipelineLayout h; };
struct DawnComputePipeline final : ComputePipelineImpl { wgpu::ComputePipeline h; };
struct DawnRenderPipeline final : RenderPipelineImpl { wgpu::RenderPipeline h; };
struct DawnCommandBuffer final : CommandBufferImpl { wgpu::CommandBuffer h; };
struct DawnQuerySet final : QuerySetImpl { wgpu::QuerySet h; };

// Downcast helpers. Valid only for handles this backend created; every caller
// is on a Dawn-only code path (an encoder created by a Dawn device only ever
// sees resources created by the same device).
static const wgpu::Buffer& NB(const Buffer& b) {
  return static_cast<DawnBuffer*>(b.Get())->h;
}
static const wgpu::BindGroup& NBG(const BindGroup& g) {
  return static_cast<DawnBindGroup*>(g.Get())->h;
}

struct DawnComputePass final : ComputePassImpl {
  wgpu::ComputePassEncoder h;
  void SetPipeline(const ComputePipeline& pipe) override {
    h.SetPipeline(static_cast<DawnComputePipeline*>(pipe.Get())->h);
  }
  void SetBindGroup(uint32_t index, const BindGroup& bg, uint32_t dynCount,
                    const uint32_t* dynOffsets) override {
    if (dynCount)
      h.SetBindGroup(index, NBG(bg), dynCount, dynOffsets);
    else
      h.SetBindGroup(index, NBG(bg));
  }
  void Dispatch(uint32_t x, uint32_t y, uint32_t z) override {
    h.DispatchWorkgroups(x, y, z);
  }
  void DispatchIndirect(const Buffer& args, uint64_t offset) override {
    h.DispatchWorkgroupsIndirect(NB(args), offset);
  }
  void End() override { h.End(); }
};

struct DawnRenderPass final : RenderPassImpl {
  wgpu::RenderPassEncoder h;
  void SetPipeline(const RenderPipeline& pipe) override {
    h.SetPipeline(static_cast<DawnRenderPipeline*>(pipe.Get())->h);
  }
  void SetBindGroup(uint32_t index, const BindGroup& bg) override {
    h.SetBindGroup(index, NBG(bg));
  }
  void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
            uint32_t firstInstance) override {
    h.Draw(vertexCount, instanceCount, firstVertex, firstInstance);
  }
  void DrawIndirect(const Buffer& args, uint64_t offset) override {
    h.DrawIndirect(NB(args), offset);
  }
  void End() override { h.End(); }
};

struct DawnCommandEncoder final : CommandEncoderImpl {
  wgpu::CommandEncoder h;

  void ClearBuffer(const Buffer& b, uint64_t offset, uint64_t size) override {
    h.ClearBuffer(NB(b), offset, size == kWholeSize ? wgpu::kWholeSize : size);
  }
  void CopyBufferToBuffer(const Buffer& src, uint64_t srcOffset, const Buffer& dst,
                          uint64_t dstOffset, uint64_t size) override {
    h.CopyBufferToBuffer(NB(src), srcOffset, NB(dst), dstOffset, size);
  }
  // The tracked variants ARE the plain calls under Dawn: it derives barriers
  // from usage, so the id has nothing to tell it. Byte-identical recording is
  // the point — the pinned hash checks it.
  void CopyTracked(pass::Buf, const Buffer& src, uint64_t srcOffset, const Buffer& dst,
                   uint64_t dstOffset, uint64_t size) override {
    h.CopyBufferToBuffer(NB(src), srcOffset, NB(dst), dstOffset, size);
  }
  void FillTracked(pass::Buf, const Buffer& b) override {
    h.ClearBuffer(NB(b), 0, wgpu::kWholeSize);
  }
  void CopyTextureToBuffer(const TexelCopyTexture& src, const TexelCopyBuffer& dst,
                           const Extent3D& extent) override {
    wgpu::TexelCopyTextureInfo s{};
    s.texture = static_cast<DawnTexture*>(src.texture.Get())->h;
    s.mipLevel = src.mipLevel;
    s.origin = {src.originX, src.originY, src.originZ};
    wgpu::TexelCopyBufferInfo d{};
    d.buffer = NB(dst.buffer);
    d.layout.offset = dst.offset;
    d.layout.bytesPerRow = dst.bytesPerRow;
    d.layout.rowsPerImage = dst.rowsPerImage;
    wgpu::Extent3D e{extent.width, extent.height, extent.depthOrArrayLayers};
    h.CopyTextureToBuffer(&s, &d, &e);
  }
  void ResolveQuerySet(const QuerySet& qs, uint32_t firstQuery, uint32_t queryCount,
                       const Buffer& dst, uint64_t dstOffset) override {
    h.ResolveQuerySet(static_cast<DawnQuerySet*>(qs.Get())->h, firstQuery, queryCount,
                      NB(dst), dstOffset);
  }
  ComputePass BeginComputePass(const char* label, const PassTimestampWrites* ts) override {
    auto impl = std::make_shared<DawnComputePass>();
    if (ts) {
      wgpu::PassTimestampWrites tw{};
      tw.querySet = static_cast<DawnQuerySet*>(ts->querySet.Get())->h;
      tw.beginningOfPassWriteIndex = ts->beginIndex;
      tw.endOfPassWriteIndex = ts->endIndex;
      wgpu::ComputePassDescriptor d{};
      d.label = label;
      d.timestampWrites = &tw;
      impl->h = h.BeginComputePass(&d);
    } else if (label) {
      wgpu::ComputePassDescriptor d{};
      d.label = label;
      impl->h = h.BeginComputePass(&d);
    } else {
      impl->h = h.BeginComputePass();
    }
    return ComputePass(std::move(impl));
  }
  RenderPass BeginRenderPass(const RenderPassDesc& desc) override {
    wgpu::RenderPassColorAttachment ca{};
    ca.view = static_cast<DawnTextureView*>(desc.color.view.Get())->h;
    ca.loadOp = ToWgpu(desc.color.loadOp);
    ca.storeOp = ToWgpu(desc.color.storeOp);
    ca.clearValue = {desc.color.clearValue[0], desc.color.clearValue[1],
                     desc.color.clearValue[2], desc.color.clearValue[3]};

    wgpu::RenderPassDepthStencilAttachment da{};
    if (desc.hasDepth) {
      da.view = static_cast<DawnTextureView*>(desc.depth.view.Get())->h;
      da.depthLoadOp = ToWgpu(desc.depth.loadOp);
      da.depthStoreOp = ToWgpu(desc.depth.storeOp);
      da.depthClearValue = desc.depth.clearValue;
    }

    wgpu::RenderPassDescriptor d{};
    d.label = desc.label;
    d.colorAttachmentCount = 1;
    d.colorAttachments = &ca;
    if (desc.hasDepth) d.depthStencilAttachment = &da;

    auto impl = std::make_shared<DawnRenderPass>();
    impl->h = h.BeginRenderPass(&d);
    return RenderPass(std::move(impl));
  }
  CommandBuffer Finish() override {
    auto impl = std::make_shared<DawnCommandBuffer>();
    impl->h = h.Finish();
    return CommandBuffer(std::move(impl));
  }
};

struct DawnQueue final : QueueImpl {
  wgpu::Queue h;
  void WriteBuffer(const Buffer& b, uint64_t offset, const void* data,
                   size_t size) override {
    h.WriteBuffer(NB(b), offset, data, size);
  }
  void Submit(uint32_t count, const CommandBuffer* cmds) override {
    std::vector<wgpu::CommandBuffer> native;
    native.reserve(count);
    for (uint32_t i = 0; i < count; i++)
      native.push_back(static_cast<DawnCommandBuffer*>(cmds[i].Get())->h);
    h.Submit(count, native.data());
  }
};

// The device impl additionally keeps the instance, because WebGPU's blocking
// primitives (WaitAny) hang off the instance rather than the device. The Vulkan
// backend has no equivalent split.
struct DawnDevice final : DeviceImpl {
  wgpu::Device h;
  wgpu::Instance instance;

  BackendKind Kind() const override { return BackendKind::Dawn; }

  Queue GetQueue() override { return WrapQueue(h.GetQueue()); }

  Buffer CreateBuffer(uint64_t size, BufferUsage usage, const char* label) override {
    wgpu::BufferDescriptor d{};
    d.size = size;
    d.usage = ToWgpu(usage);
    d.label = label;
    return WrapBuffer(h.CreateBuffer(&d), size);
  }

  Texture CreateTexture(const Extent3D& size, TextureFormat format, TextureUsage usage,
                        const char* label) override {
    wgpu::TextureDescriptor d{};
    d.size = {size.width, size.height, size.depthOrArrayLayers};
    d.format = ToWgpu(format);
    d.usage = ToWgpu(usage);
    d.label = label;
    return WrapTexture(h.CreateTexture(&d));
  }

  QuerySet CreateTimestampQuerySet(uint32_t count, const char* label) override {
    wgpu::QuerySetDescriptor d{};
    d.type = wgpu::QueryType::Timestamp;
    d.count = count;
    d.label = label;
    auto impl = std::make_shared<DawnQuerySet>();
    impl->h = h.CreateQuerySet(&d);
    if (!impl->h) return {};
    return QuerySet(std::move(impl));
  }

  BindGroupLayout CreateBindGroupLayout(const BindGroupLayoutEntry* entries,
                                        size_t count) override {
    std::vector<wgpu::BindGroupLayoutEntry> ne(count);
    for (size_t i = 0; i < count; i++) {
      ne[i] = {};
      ne[i].binding = entries[i].binding;
      ne[i].visibility = ToWgpu(entries[i].visibility);
      ne[i].buffer.type = ToWgpu(entries[i].type);
      ne[i].buffer.hasDynamicOffset = entries[i].hasDynamicOffset;
    }
    wgpu::BindGroupLayoutDescriptor d{};
    d.entryCount = (uint32_t)count;
    d.entries = ne.data();
    auto impl = std::make_shared<DawnBindGroupLayout>();
    impl->h = h.CreateBindGroupLayout(&d);
    return BindGroupLayout(std::move(impl));
  }

  PipelineLayout CreatePipelineLayout(const BindGroupLayout* groups,
                                      size_t count) override {
    std::vector<wgpu::BindGroupLayout> ng(count);
    for (size_t i = 0; i < count; i++)
      ng[i] = static_cast<DawnBindGroupLayout*>(groups[i].Get())->h;
    wgpu::PipelineLayoutDescriptor d{};
    d.bindGroupLayoutCount = (uint32_t)count;
    d.bindGroupLayouts = ng.data();
    auto impl = std::make_shared<DawnPipelineLayout>();
    impl->h = h.CreatePipelineLayout(&d);
    return PipelineLayout(std::move(impl));
  }

  BindGroup CreateBindGroup(const BindGroupLayout& layout, const BindGroupEntry* entries,
                            size_t count, const char* label) override {
    std::vector<wgpu::BindGroupEntry> ne(count);
    for (size_t i = 0; i < count; i++) {
      ne[i] = {};
      ne[i].binding = entries[i].binding;
      ne[i].buffer = NB(entries[i].buffer);
      ne[i].offset = entries[i].offset;
      // size 0 means "to the end of the buffer" in the seam; wgpu spells that
      // kWholeSize, and leaving it 0 would be a zero-length binding.
      ne[i].size = entries[i].size ? entries[i].size
                                   : (entries[i].buffer.Size() - entries[i].offset);
    }
    wgpu::BindGroupDescriptor d{};
    d.layout = static_cast<DawnBindGroupLayout*>(layout.Get())->h;
    d.entryCount = (uint32_t)count;
    d.entries = ne.data();
    d.label = label;
    auto impl = std::make_shared<DawnBindGroup>();
    impl->h = h.CreateBindGroup(&d);
    return BindGroup(std::move(impl));
  }

  ShaderModule CreateShaderModule(const std::string& wgsl, const char* label) override {
    wgpu::ShaderSourceWGSL src{};
    src.code = wgsl.c_str();
    wgpu::ShaderModuleDescriptor d{};
    d.nextInChain = &src;
    d.label = label;
    auto impl = std::make_shared<DawnShaderModule>();
    impl->h = h.CreateShaderModule(&d);
    if (!impl->h) return {};
    return ShaderModule(std::move(impl));
  }

  ComputePipeline CreateComputePipeline(const PipelineLayout& layout,
                                        const ShaderModule& module, const char* entry,
                                        const char* label) override {
    wgpu::ComputePipelineDescriptor d{};
    d.layout = static_cast<DawnPipelineLayout*>(layout.Get())->h;
    d.compute.module = static_cast<DawnShaderModule*>(module.Get())->h;
    d.compute.entryPoint = entry;
    d.label = label;
    auto impl = std::make_shared<DawnComputePipeline>();
    impl->h = h.CreateComputePipeline(&d);
    return ComputePipeline(std::move(impl));
  }

  RenderPipeline CreateRenderPipeline(const RenderPipelineDesc& desc) override {
    wgpu::BlendState blend{};
    if (desc.blend) {
      blend.color.srcFactor = ToWgpu(desc.blend->color.srcFactor);
      blend.color.dstFactor = ToWgpu(desc.blend->color.dstFactor);
      blend.color.operation = ToWgpu(desc.blend->color.operation);
      blend.alpha.srcFactor = ToWgpu(desc.blend->alpha.srcFactor);
      blend.alpha.dstFactor = ToWgpu(desc.blend->alpha.dstFactor);
      blend.alpha.operation = ToWgpu(desc.blend->alpha.operation);
    }

    wgpu::ColorTargetState ct{};
    ct.format = ToWgpu(desc.colorFormat);
    if (desc.blend) ct.blend = &blend;

    wgpu::DepthStencilState ds{};
    ds.format = ToWgpu(desc.depth.format);
    ds.depthWriteEnabled = desc.depth.depthWriteEnabled;
    ds.depthCompare = ToWgpu(desc.depth.depthCompare);

    wgpu::FragmentState fs{};
    fs.module = static_cast<DawnShaderModule*>(desc.fragmentModule.Get())->h;
    fs.entryPoint = desc.fragmentEntry;
    fs.targetCount = 1;
    fs.targets = &ct;

    wgpu::RenderPipelineDescriptor d{};
    d.label = desc.label;
    d.layout = static_cast<DawnPipelineLayout*>(desc.layout.Get())->h;
    d.vertex.module = static_cast<DawnShaderModule*>(desc.vertexModule.Get())->h;
    d.vertex.entryPoint = desc.vertexEntry;
    d.primitive.topology = ToWgpu(desc.topology);
    d.primitive.cullMode = ToWgpu(desc.cullMode);
    d.depthStencil = &ds;
    d.fragment = &fs;

    auto impl = std::make_shared<DawnRenderPipeline>();
    impl->h = h.CreateRenderPipeline(&d);
    return RenderPipeline(std::move(impl));
  }

  CommandEncoder CreateCommandEncoder(const char* label) override {
    auto impl = std::make_shared<DawnCommandEncoder>();
    if (label) {
      wgpu::CommandEncoderDescriptor d{};
      d.label = label;
      impl->h = h.CreateCommandEncoder(&d);
    } else {
      impl->h = h.CreateCommandEncoder();
    }
    return CommandEncoder(std::move(impl));
  }

  void PushValidationScope() override {
    h.PushErrorScope(wgpu::ErrorFilter::Validation);
  }

  bool PopValidationScopeBlocking() override {
    bool hadError = false;
    wgpu::Future f = h.PopErrorScope(
        wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView msg) {
          if (type != wgpu::ErrorType::NoError) {
            hadError = true;
            std::fprintf(stderr, "validation error: %.*s\n", (int)msg.length, msg.data);
          }
        });
    instance.WaitAny(f, UINT64_MAX);
    return hadError;
  }

  void ProcessEvents() override { instance.ProcessEvents(); }

  void WaitIdle() override {
    wgpu::Future f = h.GetQueue().OnSubmittedWorkDone(
        wgpu::CallbackMode::WaitAnyOnly,
        [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {});
    instance.WaitAny(f, UINT64_MAX);
  }

  bool ReadBufferBlocking(const Buffer& src, uint64_t offset, void* out,
                          size_t size) override {
    const wgpu::Buffer& nb = NB(src);
    bool ok = false;
    wgpu::Future f = nb.MapAsync(
        wgpu::MapMode::Read, offset, size, wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
          if (status != wgpu::MapAsyncStatus::Success) return;
          const void* p = nb.GetConstMappedRange(offset, size);
          if (!p) return;
          std::memcpy(out, p, size);
          ok = true;
        });
    instance.WaitAny(f, UINT64_MAX);
    if (ok) nb.Unmap();
    return ok;
  }

  MapTicket MapReadDeferred(const Buffer& b, uint64_t offset, uint64_t size) override;
};

// -------------------------------------------------------------- tickets ----

struct DawnMapTicket final : MapTicketImpl {
  wgpu::Buffer buf;
  wgpu::Instance instance;
  wgpu::Future future{};
  uint64_t offset = 0, size = 0;
  // 0 = pending, 1 = mapped, 2 = failed. Heap-owned via the shared_ptr so the
  // callback outlives any container reshuffle at the call site.
  std::shared_ptr<uint32_t> status;

  bool Ready() override {
    return instance.WaitAny(future, 0) == wgpu::WaitStatus::Success;
  }
  void Wait() override { instance.WaitAny(future, UINT64_MAX); }
  bool Succeeded() override { return *status == 1u; }
  const void* Data() override {
    if (*status != 1u) return nullptr;
    return buf.GetConstMappedRange(offset, size);
  }
  void Unmap() override {
    if (*status == 1u) buf.Unmap();
  }
};

MapTicket DawnDevice::MapReadDeferred(const Buffer& b, uint64_t offset, uint64_t size) {
  auto impl = std::make_shared<DawnMapTicket>();
  impl->buf = NB(b);
  impl->instance = instance;
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

// ------------------------------------------------------- wrap / unwrap ----

Device WrapDevice(const wgpu::Device& d, const wgpu::Instance& inst) {
  if (!d) return {};
  auto p = std::make_shared<DawnDevice>();
  p->h = d;
  p->instance = inst;
  return Device(std::move(p));
}

Buffer WrapBuffer(const wgpu::Buffer& b, uint64_t size) {
  if (!b) return {};
  auto p = std::make_shared<DawnBuffer>();
  p->h = b;
  p->size = size;
  return Buffer(std::move(p));
}

Texture WrapTexture(const wgpu::Texture& t) {
  if (!t) return {};
  auto p = std::make_shared<DawnTexture>();
  p->h = t;
  return Texture(Texture_(std::move(p)));
}

TextureView WrapTextureView(const wgpu::TextureView& v) {
  if (!v) return {};
  auto p = std::make_shared<DawnTextureView>();
  p->h = v;
  return TextureView(std::move(p));
}

Queue WrapQueue(const wgpu::Queue& q) {
  if (!q) return {};
  auto p = std::make_shared<DawnQueue>();
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
  return d ? static_cast<DawnDevice*>(d.Get())->h : kNullDevice;
}
const wgpu::Instance& NativeInstance(const Device& d) {
  return d ? static_cast<DawnDevice*>(d.Get())->instance : kNullInstance;
}
const wgpu::Buffer& Native(const Buffer& b) { return b ? NB(b) : kNullBuffer; }
const wgpu::Texture& Native(const Texture& t) {
  return t ? static_cast<DawnTexture*>(t.Get())->h : kNullTexture;
}
const wgpu::TextureView& Native(const TextureView& v) {
  return v ? static_cast<DawnTextureView*>(v.Get())->h : kNullView;
}
const wgpu::Queue& Native(const Queue& q) {
  return q ? static_cast<DawnQueue*>(q.Get())->h : kNullQueue;
}
const wgpu::RenderPassEncoder& Native(const RenderPass& p) {
  return p ? static_cast<DawnRenderPass*>(p.Get())->h : kNullRenderPass;
}
const wgpu::CommandEncoder& Native(const CommandEncoder& e) {
  return e ? static_cast<DawnCommandEncoder*>(e.Get())->h : kNullEncoder;
}

}  // namespace dawn
}  // namespace rhi
