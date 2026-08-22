// rhi.h — the render/compute hardware interface seam.
//
// WHY THIS EXISTS
// ---------------
// The engine is being ported from WebGPU/Dawn to Vulkan (docs/PLAN_vulkan_port.md).
// Phase 2a — this file — confines every `wgpu::` name to src/gpu/ so that phase 3
// can add a Vulkan backend without touching sim/, test/, game/ or main.cpp. The
// grep that keeps that true:
//
//     grep -rn "wgpu::" src        # must hit ONLY src/gpu/ and src/ui/overlay.*
//
// (src/ui/overlay.* is the one sanctioned exception: it holds ImGui_ImplWGPU_*
// until PHASE 4 swaps it for imgui_impl_vulkan. It is marked there.)
//
// SHAPE
// -----
// Deliberately WGPU-SHAPED. Method names, argument order and semantics mirror
// webgpu_cpp.h, because that is what makes phase 2a a mechanical rename with a
// provably unchanged world hash — a seam that "improved" the API would have
// hidden a behavior change inside a refactor. Phase 3 changes the *implementation*
// under these names; if a name proves actively wrong for Vulkan, it is renamed
// then, with the Dawn backend still present as the hash oracle.
//
// Handles are value types with reference semantics (like wgpu::), each holding a
// backend handle in a small impl struct. Encoding-path cost is irrelevant at this
// scale (~5 compute passes and ~60 dispatches per tick, microseconds of CPU), so
// clarity beats avoiding an indirection.
//
// The ~10 concepts (docs/vulkan_pass_map.md §7):
//   Buffer, CommandEncoder, ComputePassEncoder, RenderPassEncoder,
//   Texture/TextureView, ShaderModule, BindGroupLayout/PipelineLayout/BindGroup,
//   ComputePipeline/RenderPipeline, Queue, Device.
// Plus two things the port needs that WebGPU spells awkwardly:
//   - ReadBufferBlocking(): ONE blocking readback helper, so the selftest does
//     not reimplement Future plumbing at a dozen call sites.
//   - Device::PollUntilIdle()/ProcessEvents(): the async-callback pump.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rhi {

// ---------------------------------------------------------------- enums ----
// Values are NOT assumed to match any backend's; every backend maps explicitly.

enum class BufferUsage : uint32_t {
  None = 0,
  MapRead = 1u << 0,
  MapWrite = 1u << 1,
  CopySrc = 1u << 2,
  CopyDst = 1u << 3,
  Index = 1u << 4,
  Vertex = 1u << 5,
  Uniform = 1u << 6,
  Storage = 1u << 7,
  Indirect = 1u << 8,
  QueryResolve = 1u << 9,
};
inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
  return (BufferUsage)((uint32_t)a | (uint32_t)b);
}
inline BufferUsage& operator|=(BufferUsage& a, BufferUsage b) { return a = a | b; }
inline bool Any(BufferUsage a, BufferUsage b) { return ((uint32_t)a & (uint32_t)b) != 0; }

enum class TextureUsage : uint32_t {
  None = 0,
  CopySrc = 1u << 0,
  CopyDst = 1u << 1,
  TextureBinding = 1u << 2,
  StorageBinding = 1u << 3,
  RenderAttachment = 1u << 4,
};
inline TextureUsage operator|(TextureUsage a, TextureUsage b) {
  return (TextureUsage)((uint32_t)a | (uint32_t)b);
}

// Only the formats the engine actually names. Undefined doubles as "not yet
// known", which EnsureRenderPipelines uses to force a first build.
enum class TextureFormat : uint32_t {
  Undefined = 0,
  RGBA8Unorm,
  BGRA8Unorm,
  Depth32Float,
};

enum class BufferBindingType : uint32_t {
  Uniform,
  Storage,
  ReadOnlyStorage,
};

enum class ShaderStage : uint32_t {
  None = 0,
  Vertex = 1u << 0,
  Fragment = 1u << 1,
  Compute = 1u << 2,
};
inline ShaderStage operator|(ShaderStage a, ShaderStage b) {
  return (ShaderStage)((uint32_t)a | (uint32_t)b);
}

enum class CompareFunction : uint32_t { Never, Less, LessEqual, Greater, GreaterEqual,
                                        Equal, NotEqual, Always };
enum class CullMode : uint32_t { None, Front, Back };
enum class PrimitiveTopology : uint32_t { PointList, LineList, LineStrip,
                                          TriangleList, TriangleStrip };
enum class LoadOp : uint32_t { Load, Clear };
enum class StoreOp : uint32_t { Store, Discard };
enum class BlendFactor : uint32_t { Zero, One, SrcAlpha, OneMinusSrcAlpha, Src,
                                    OneMinusSrc, Dst, OneMinusDst };
enum class BlendOperation : uint32_t { Add, Subtract, ReverseSubtract, Min, Max };

// Whole-buffer sentinel for ClearBuffer/size arguments (wgpu::kWholeSize).
inline constexpr uint64_t kWholeSize = ~0ull;

// ------------------------------------------------------------- handles ----
// Each wraps a backend object. Copyable (shared), default-constructed handles
// are invalid and test false — matching wgpu:: semantics, which several call
// sites rely on (`if (depthView_)`, `return {};` on failure).

struct BufferImpl;
struct TextureImpl;
struct TextureViewImpl;
struct ShaderModuleImpl;
struct BindGroupLayoutImpl;
struct BindGroupImpl;
struct PipelineLayoutImpl;
struct ComputePipelineImpl;
struct RenderPipelineImpl;
struct CommandEncoderImpl;
struct CommandBufferImpl;
struct ComputePassImpl;
struct RenderPassImpl;
struct QuerySetImpl;

#define RHI_HANDLE(Name, Impl)                                     \
  class Name {                                                     \
   public:                                                         \
    Name() = default;                                              \
    explicit Name(std::shared_ptr<Impl> p) : p_(std::move(p)) {}   \
    explicit operator bool() const { return p_ != nullptr; }       \
    bool operator==(const Name& o) const { return p_ == o.p_; }    \
    bool operator!=(const Name& o) const { return p_ != o.p_; }    \
    Impl* Get() const { return p_.get(); }                         \
    const std::shared_ptr<Impl>& Ref() const { return p_; }        \
                                                                   \
   protected:                                                      \
    std::shared_ptr<Impl> p_;                                      \
  }

RHI_HANDLE(Texture_, TextureImpl);
RHI_HANDLE(TextureView, TextureViewImpl);
RHI_HANDLE(ShaderModule, ShaderModuleImpl);
RHI_HANDLE(BindGroupLayout, BindGroupLayoutImpl);
RHI_HANDLE(BindGroup, BindGroupImpl);
RHI_HANDLE(PipelineLayout, PipelineLayoutImpl);
RHI_HANDLE(ComputePipeline, ComputePipelineImpl);
RHI_HANDLE(RenderPipeline, RenderPipelineImpl);
RHI_HANDLE(CommandBuffer, CommandBufferImpl);
RHI_HANDLE(QuerySet, QuerySetImpl);

// Texture adds CreateView(); everything else about it is opaque.
class Texture : public Texture_ {
 public:
  using Texture_::Texture_;
  Texture() = default;
  Texture(const Texture_& t) : Texture_(t) {}
  TextureView CreateView() const;
};

class Buffer {
 public:
  Buffer() = default;
  explicit Buffer(std::shared_ptr<BufferImpl> p) : p_(std::move(p)) {}
  explicit operator bool() const { return p_ != nullptr; }
  bool operator==(const Buffer& o) const { return p_ == o.p_; }
  BufferImpl* Get() const { return p_.get(); }
  const std::shared_ptr<BufferImpl>& Ref() const { return p_; }

  uint64_t Size() const;

 private:
  std::shared_ptr<BufferImpl> p_;
};

// ------------------------------------------------------- descriptor POD ----

struct BindGroupLayoutEntry {
  uint32_t binding = 0;
  ShaderStage visibility = ShaderStage::Compute;
  BufferBindingType type = BufferBindingType::Storage;
  bool hasDynamicOffset = false;
};

struct BindGroupEntry {
  uint32_t binding = 0;
  Buffer buffer;
  uint64_t offset = 0;
  // 0 means "the rest of the buffer from `offset`" (wgpu leaves size 0 = whole).
  uint64_t size = 0;
};

struct DepthState {
  TextureFormat format = TextureFormat::Depth32Float;
  bool depthWriteEnabled = true;
  CompareFunction depthCompare = CompareFunction::Always;
};

struct BlendComponent {
  BlendFactor srcFactor = BlendFactor::One;
  BlendFactor dstFactor = BlendFactor::Zero;
  BlendOperation operation = BlendOperation::Add;
};

struct BlendState {
  BlendComponent color;
  BlendComponent alpha;
};

struct RenderPipelineDesc {
  const char* label = nullptr;
  PipelineLayout layout;
  ShaderModule vertexModule;
  const char* vertexEntry = "vs";
  ShaderModule fragmentModule;
  const char* fragmentEntry = "fs";
  TextureFormat colorFormat = TextureFormat::Undefined;
  // null = opaque (no blending)
  const BlendState* blend = nullptr;
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;
  CullMode cullMode = CullMode::None;
  DepthState depth;
};

struct ColorAttachment {
  TextureView view;
  LoadOp loadOp = LoadOp::Clear;
  StoreOp storeOp = StoreOp::Store;
  double clearValue[4] = {0, 0, 0, 1};
};

struct DepthAttachment {
  TextureView view;
  LoadOp loadOp = LoadOp::Clear;
  StoreOp storeOp = StoreOp::Store;
  float clearValue = 0.0f;  // reversed-Z: clear to far
};

struct RenderPassDesc {
  const char* label = nullptr;
  ColorAttachment color;
  // hasDepth=false leaves the depth attachment off entirely.
  bool hasDepth = true;
  DepthAttachment depth;
};

// Timestamp writes attached to a compute pass (measurement only — PassTimer).
struct PassTimestampWrites {
  QuerySet querySet;
  uint32_t beginIndex = 0;
  uint32_t endIndex = 0;
};

// Texture <-> buffer copy descriptors, used by the screenshot path only.
struct TexelCopyTexture {
  Texture texture;
  uint32_t mipLevel = 0;
  uint32_t originX = 0, originY = 0, originZ = 0;
};

struct TexelCopyBuffer {
  Buffer buffer;
  uint64_t offset = 0;
  uint32_t bytesPerRow = 0;
  uint32_t rowsPerImage = 0;
};

struct Extent3D {
  uint32_t width = 1, height = 1, depthOrArrayLayers = 1;
};

// ------------------------------------------------------------ encoders ----

class ComputePass {
 public:
  ComputePass() = default;
  explicit ComputePass(std::shared_ptr<ComputePassImpl> p) : p_(std::move(p)) {}
  explicit operator bool() const { return p_ != nullptr; }

  void SetPipeline(const ComputePipeline& p) const;
  void SetBindGroup(uint32_t index, const BindGroup& bg) const;
  // dynamicOffsets applies to the layout entries flagged hasDynamicOffset.
  void SetBindGroup(uint32_t index, const BindGroup& bg, uint32_t dynamicOffsetCount,
                    const uint32_t* dynamicOffsets) const;
  void DispatchWorkgroups(uint32_t x, uint32_t y = 1, uint32_t z = 1) const;
  void DispatchWorkgroupsIndirect(const Buffer& args, uint64_t offset) const;
  void End() const;

 private:
  std::shared_ptr<ComputePassImpl> p_;
};

class RenderPass {
 public:
  RenderPass() = default;
  explicit RenderPass(std::shared_ptr<RenderPassImpl> p) : p_(std::move(p)) {}
  explicit operator bool() const { return p_ != nullptr; }
  RenderPassImpl* Get() const { return p_.get(); }

  void SetPipeline(const RenderPipeline& p) const;
  void SetBindGroup(uint32_t index, const BindGroup& bg) const;
  void Draw(uint32_t vertexCount, uint32_t instanceCount = 1,
            uint32_t firstVertex = 0, uint32_t firstInstance = 0) const;
  void DrawIndirect(const Buffer& args, uint64_t offset) const;
  void End() const;

 private:
  std::shared_ptr<RenderPassImpl> p_;
};

class CommandEncoder {
 public:
  CommandEncoder() = default;
  explicit CommandEncoder(std::shared_ptr<CommandEncoderImpl> p) : p_(std::move(p)) {}
  explicit operator bool() const { return p_ != nullptr; }
  CommandEncoderImpl* Get() const { return p_.get(); }

  void ClearBuffer(const Buffer& b, uint64_t offset = 0, uint64_t size = kWholeSize) const;
  void CopyBufferToBuffer(const Buffer& src, uint64_t srcOffset, const Buffer& dst,
                          uint64_t dstOffset, uint64_t size) const;
  void CopyTextureToBuffer(const TexelCopyTexture& src, const TexelCopyBuffer& dst,
                           const Extent3D& extent) const;
  void ResolveQuerySet(const QuerySet& qs, uint32_t firstQuery, uint32_t queryCount,
                       const Buffer& dst, uint64_t dstOffset) const;

  ComputePass BeginComputePass(const char* label = nullptr) const;
  // Overload carrying GPU timestamp writes (measurement harness only).
  ComputePass BeginComputePass(const char* label, const PassTimestampWrites& ts) const;
  RenderPass BeginRenderPass(const RenderPassDesc& d) const;

  CommandBuffer Finish() const;

 private:
  std::shared_ptr<CommandEncoderImpl> p_;
};

// ---------------------------------------------------------------- queue ----

class Queue {
 public:
  Queue() = default;
  explicit Queue(std::shared_ptr<struct QueueImpl> p) : p_(std::move(p)) {}
  explicit operator bool() const { return p_ != nullptr; }
  struct QueueImpl* Get() const { return p_.get(); }

  void WriteBuffer(const Buffer& b, uint64_t offset, const void* data, size_t size) const;
  void Submit(const CommandBuffer& cmd) const;
  void Submit(uint32_t count, const CommandBuffer* cmds) const;

 private:
  std::shared_ptr<struct QueueImpl> p_;
};

// --------------------------------------------------------------- device ----

class Device {
 public:
  Device() = default;
  explicit Device(std::shared_ptr<struct DeviceImpl> p) : p_(std::move(p)) {}
  explicit operator bool() const { return p_ != nullptr; }
  struct DeviceImpl* Get() const { return p_.get(); }

  Queue GetQueue() const;

  Buffer CreateBuffer(uint64_t size, BufferUsage usage, const char* label) const;
  Texture CreateTexture(const Extent3D& size, TextureFormat format, TextureUsage usage,
                        const char* label) const;
  QuerySet CreateTimestampQuerySet(uint32_t count, const char* label) const;

  BindGroupLayout CreateBindGroupLayout(const BindGroupLayoutEntry* entries,
                                        size_t count) const;
  PipelineLayout CreatePipelineLayout(const BindGroupLayout* groups, size_t count) const;
  BindGroup CreateBindGroup(const BindGroupLayout& layout, const BindGroupEntry* entries,
                            size_t count, const char* label = nullptr) const;

  // Compiles WGSL source. Returns an invalid module if compilation fails outright.
  ShaderModule CreateShaderModule(const std::string& wgsl, const char* label) const;
  ComputePipeline CreateComputePipeline(const PipelineLayout& layout,
                                        const ShaderModule& module, const char* entry,
                                        const char* label) const;
  RenderPipeline CreateRenderPipeline(const RenderPipelineDesc& d) const;

  CommandEncoder CreateCommandEncoder(const char* label = nullptr) const;

  // Validation-error capture around a block of resource creation, used by the
  // F5 shader hot-reload to keep the old pipelines when the new WGSL is bad.
  // Scopes do not nest in practice here; one push, one pop.
  void PushValidationScope() const;
  // Runs the pending work needed to retrieve the scope result and returns true
  // if any validation error occurred. Messages are printed to stderr.
  bool PopValidationScopeBlocking() const;

  // Pump async callbacks (the World readback ring maps with a
  // process-events callback and lands here).
  void ProcessEvents() const;
  // Block until all submitted GPU work has completed.
  void WaitIdle() const;

 private:
  std::shared_ptr<struct DeviceImpl> p_;
};

// -------------------------------------------------- blocking read helper ----
//
// Everything the selftest and the screenshot path needed from wgpu::Future.
// Deliberately explicit and deliberately blocking: CLAUDE.md permits a
// synchronous readback ONLY in tests and the selftest's hash read, and a named
// "Blocking" helper is what keeps that visible in a code review. Nothing in the
// frame path may call these.
//
// The buffer must have been created with BufferUsage::MapRead, and the work
// producing its contents must already be submitted. Returns false if the map
// failed; `out` is untouched in that case.
bool ReadBufferBlocking(const Device& dev, const Buffer& src, uint64_t offset,
                        void* out, size_t size);

// Convenience: create a MapRead|CopyDst staging buffer, copy `size` bytes from
// `src` at `srcOffset` into it, submit on `queue`, and block for the result.
// This is the shape ~12 selftest call sites had open-coded; used at every one.
bool ReadbackBlocking(const Device& dev, const Queue& queue, const Buffer& src,
                      uint64_t srcOffset, void* out, size_t size,
                      const char* label = "readback");

// Non-blocking map for the World readback ring: `done` fires from ProcessEvents()
// with the mapped pointer (null on failure). The pointer is valid only inside
// the callback; the buffer is unmapped automatically when it returns.
void MapReadAsync(const Buffer& b, uint64_t offset, uint64_t size,
                  std::function<void(const void*)> done);

// A map issued now and consumed later — the eviction ring in sim/stream.cpp.
// Ready() polls without blocking (per-tick harvest); Wait() blocks (ring full,
// or shutdown drain). After either reports completion, Data() is the mapped
// pointer (null if the map failed) and stays valid until Unmap().
//
// Vulkan note for phase 3: this is a fence + a persistently-mapped host-visible
// allocation. Ready() is vkGetFenceStatus, Wait() is vkWaitForFences. The
// two-step "poll, then consume" shape is what the ring needs and is why this is
// a ticket rather than a callback.
class MapTicket {
 public:
  MapTicket() = default;
  explicit MapTicket(std::shared_ptr<struct MapTicketImpl> p) : p_(std::move(p)) {}
  explicit operator bool() const { return p_ != nullptr; }

  bool Ready() const;               // non-blocking poll
  void Wait() const;                // block until resolved
  bool Succeeded() const;           // valid after Ready()==true or Wait()
  const void* Data() const;         // mapped range, or null
  void Unmap() const;               // releases the mapping; buffer is reusable

 private:
  std::shared_ptr<struct MapTicketImpl> p_;
};

// Issue a map that will be consumed through a MapTicket.
MapTicket MapReadDeferred(const Device& dev, const Buffer& b, uint64_t offset,
                          uint64_t size);

}  // namespace rhi
