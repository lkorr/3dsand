// rhi_vk.cpp — Vulkan implementation of the rhi.h seam (port phase 4a).
//
// Read rhi_vk.h first. Structure:
//
//   VkrState    one per device: the shared vk::Backend, the barrier mode, the
//               fence of the most recent submit (what MapReadAsync borrows),
//               the pending host-map callbacks ProcessEvents fires, and the
//               last recording's stats.
//   Vkr*        the impl subclasses. Resources translate onto vk::Backend;
//               the encoder owns a vk::Recorder whose Begin() emits the §3.4
//               head barrier right after Backend::BeginCommands flushed the
//               pending uploads — the exact order phase 3c's RunTable used.
//   RecordTableVulkan   the bridge (rhi_record.h): downcasts Simulation's
//               resolved TableBindings to live Vulkan objects, hands them to
//               the encoder's recorder, and lets it walk pass::kRows itself.
//
// FIVE SEAM SEMANTICS, AND WHERE EACH IS HONORED (same list rhi_vulkan.h keeps):
//   1. WriteBuffer queue-ordered + deferred  -> VkrQueue::WriteBuffer ->
//      Backend::QueueWrite; drains at the head of the next command buffer.
//   2. Binding size 0 = rest of buffer       -> Backend::CreateDescriptorSet.
//   3. MapTicket poll/consume                -> a borrowed retained fence +
//      the persistently mapped allocation (VkrMapTicket).
//   4. Device-scoped validation reporting    -> Backend's debug messenger via
//      Push/PopValidationScope.
//   5. Blocking readbacks, tests only        -> VkrDevice::ReadBufferBlocking
//      (WaitIdle + memcpy from the persistent map).

#include "gpu/rhi_vk.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gpu/passtimer.h"
#include "gpu/rhi_impl.h"
#include "gpu/rhi_record.h"
#include "gpu/rhi_vulkan.h"
#include "gpu/vk_record.h"

namespace rhi {
namespace vkr {
namespace {

[[noreturn]] void NotUntil4b(const char* what) {
  std::fprintf(stderr,
               "FATAL: %s is not available on --backend vulkan until phase 4b "
               "(render path). A gate or code path that reaches this was not "
               "declared needsRender — fix the declaration, do not soften this "
               "abort.\n",
               what);
  std::abort();
}

struct VkrState {
  std::shared_ptr<vk::Backend> be;
  vk::BarrierMode mode = vk::BarrierMode::Precise;
  // Fence of the most recent Queue::Submit. MapReadAsync/MapReadDeferred borrow
  // it (RetainFence) because Dawn's semantics are "the map completes when the
  // work producing the contents completes", and every call site issues the map
  // AFTER submitting that work (KickReadback, Stream::EvictSlots).
  VkFence lastFence = VK_NULL_HANDLE;
  struct PendingMap {
    vk::Buffer* buf = nullptr;
    uint64_t offset = 0, size = 0;
    VkFence fence = VK_NULL_HANDLE;  // retained; released after the callback
    std::function<void(const void*)> done;
  };
  std::vector<PendingMap> maps;
  vk::RecordStats lastStats{};
  bool warnedNoTexture = false;
};

// ------------------------------------------------------------- resources ----

struct VkrBuffer final : BufferImpl {
  vk::Buffer* b = nullptr;
  std::shared_ptr<VkrState> st;
  ~VkrBuffer() override {
    // The seam handle is refcounted like wgpu::Buffer; releasing the last
    // reference frees the allocation (deferred until in-flight submits retire).
    // Without this, every gate's whole-world staging read would live until
    // Shutdown — 512 MiB apiece.
    if (b && st && st->be) st->be->DestroyBufferDeferred(b);
  }
  void MapReadAsync(uint64_t offset, uint64_t sz,
                    std::function<void(const void*)> done) override {
    VkFence f = st->lastFence;
    if (f != VK_NULL_HANDLE) st->be->RetainFence(f);
    st->maps.push_back({b, offset, sz, f, std::move(done)});
  }
};

vk::Buffer* NB(const Buffer& b) {
  return b ? static_cast<VkrBuffer*>(b.Get())->b : nullptr;
}

struct VkrShaderModule final : ShaderModuleImpl {
  // Compilation is DEFERRED to pipeline creation: Tint emits single-entry-point
  // SPIR-V, and the entry point arrives with CreateComputePipeline. The
  // backend's module cache (label + entry + source hash) keeps each combination
  // compiled once.
  std::string source;
  std::string label;
};

struct VkrBindGroupLayout final : BindGroupLayoutImpl {
  VkDescriptorSetLayout l = VK_NULL_HANDLE;
  // Kept because descriptor WRITES need the layout entry types matched by
  // binding number (Backend::CreateDescriptorSet's hard-won rule).
  std::vector<BindGroupLayoutEntry> entries;
};
struct VkrBindGroup final : BindGroupImpl { VkDescriptorSet set = VK_NULL_HANDLE; };
struct VkrPipelineLayout final : PipelineLayoutImpl { VkPipelineLayout l = VK_NULL_HANDLE; };
struct VkrComputePipeline final : ComputePipelineImpl { VkPipeline p = VK_NULL_HANDLE; };
struct VkrCommandBuffer final : CommandBufferImpl { VkCommandBuffer cmd = VK_NULL_HANDLE; };
struct VkrQuerySet final : QuerySetImpl {
  VkQueryPool pool = VK_NULL_HANDLE;
  uint32_t count = 0;
  std::shared_ptr<VkrState> st;
  ~VkrQuerySet() override;
};

// --------------------------------------------------------------- encoder ----

struct VkrEncoder final : CommandEncoderImpl {
  std::shared_ptr<VkrState> st;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  std::unique_ptr<vk::Recorder> rec;
  vk::Bindings bindings{};  // set by the bridge at the first RecordTable call

  VkrEncoder(std::shared_ptr<VkrState> s, const char* label) : st(std::move(s)) {
    // BeginCommands flushes the pending uploads at the head of the command
    // buffer; Recorder::Begin then emits the §3.4 global barrier, which is what
    // makes those uploads (and every previous submit) visible to everything
    // recorded after it. Same order as phase 3c's RunTable — load-bearing.
    cmd = st->be->BeginCommands(label ? label : "enc");
    if (cmd == VK_NULL_HANDLE) return;
    rec = std::make_unique<vk::Recorder>(*st->be, bindings, st->mode);
    rec->Begin(cmd);
  }

  void ClearBuffer(const Buffer& b, uint64_t offset, uint64_t size) override {
    rec->FillUntracked(NB(b), offset, size == kWholeSize ? UINT64_MAX : size);
  }
  void CopyBufferToBuffer(const Buffer& src, uint64_t srcOffset, const Buffer& dst,
                          uint64_t dstOffset, uint64_t size) override {
    rec->CopyToHost(NB(src), srcOffset, NB(dst), dstOffset, size);
  }
  void CopyTracked(pass::Buf srcId, const Buffer& src, uint64_t srcOffset,
                   const Buffer& dst, uint64_t dstOffset, uint64_t size) override {
    rec->CopyTracked(srcId, NB(src), srcOffset, NB(dst), dstOffset, size);
  }
  void FillTracked(pass::Buf id, const Buffer& b) override {
    rec->FillTracked(id, NB(b));
  }
  void CopyTextureToBuffer(const TexelCopyTexture&, const TexelCopyBuffer&,
                           const Extent3D&) override {
    NotUntil4b("CopyTextureToBuffer");
  }
  void ResolveQuerySet(const QuerySet& qs, uint32_t firstQuery, uint32_t queryCount,
                       const Buffer& dst, uint64_t dstOffset) override;
  ComputePass BeginComputePass(const char*, const PassTimestampWrites*) override {
    // Nothing reaches this on Vulkan: sim recording goes through the
    // RecordTableVulkan bridge (the recorder walks the rows itself), and the
    // measure timer hangs its timestamps off the recorder's group transitions.
    NotUntil4b("BeginComputePass (generic compute-pass encoding)");
  }
  RenderPass BeginRenderPass(const RenderPassDesc&) override {
    NotUntil4b("BeginRenderPass");
  }
  CommandBuffer Finish() override {
    rec->Finish();
    st->lastStats = rec->Stats();
    std::string err;
    // End here (wgpu's Finish/Submit split); Queue::Submit uses SubmitEnded.
    if (st->be->Fns().EndCommandBuffer(cmd) != VK_SUCCESS) {
      std::fprintf(stderr, "vkEndCommandBuffer failed\n");
      return {};
    }
    auto impl = std::make_shared<VkrCommandBuffer>();
    impl->cmd = cmd;
    return CommandBuffer(std::move(impl));
  }
};

// ----------------------------------------------------------------- queue ----

struct VkrQueue final : QueueImpl {
  std::shared_ptr<VkrState> st;
  explicit VkrQueue(std::shared_ptr<VkrState> s) : st(std::move(s)) {}
  void WriteBuffer(const Buffer& b, uint64_t offset, const void* data,
                   size_t size) override {
    st->be->QueueWrite(NB(b), offset, data, size);
  }
  void Submit(uint32_t count, const CommandBuffer* cmds) override {
    for (uint32_t i = 0; i < count; i++) {
      if (!cmds[i]) continue;
      auto* c = static_cast<VkrCommandBuffer*>(cmds[i].Get());
      std::string err;
      VkFence f = st->be->SubmitEnded(c->cmd, err);
      if (f == VK_NULL_HANDLE) {
        std::fprintf(stderr, "vulkan submit failed: %s\n", err.c_str());
        continue;
      }
      st->lastFence = f;
    }
  }
};

// --------------------------------------------------------------- tickets ----

struct VkrMapTicket final : MapTicketImpl {
  std::shared_ptr<VkrState> st;
  vk::Buffer* buf = nullptr;
  uint64_t offset = 0, size = 0;
  VkFence fence = VK_NULL_HANDLE;  // retained at creation
  bool released = false;

  ~VkrMapTicket() override { ReleaseOnce(); }
  void ReleaseOnce() {
    if (!released && fence != VK_NULL_HANDLE) st->be->ReleaseFence(fence);
    released = true;
  }
  bool Ready() override {
    if (fence == VK_NULL_HANDLE) return true;
    return st->be->FenceStatus(fence) == VK_SUCCESS;
  }
  void Wait() override {
    if (fence == VK_NULL_HANDLE) return;
    std::string err;
    st->be->WaitFence(fence, err);
  }
  bool Succeeded() override { return buf && buf->mapped; }
  const void* Data() override {
    if (!buf || !buf->mapped) return nullptr;
    return (const uint8_t*)buf->mapped + offset;
  }
  void Unmap() override {
    // Persistent mapping: nothing to unmap. Releasing the fence borrow here is
    // what lets it return to the pool — a ticket is consumed exactly once.
    ReleaseOnce();
  }
};

// ---------------------------------------------------------------- device ----

struct VkrDevice final : DeviceImpl {
  std::shared_ptr<VkrState> st;

  BackendKind Kind() const override { return BackendKind::Vulkan; }

  Queue GetQueue() override { return Queue(std::make_shared<VkrQueue>(st)); }

  Buffer CreateBuffer(uint64_t size, BufferUsage usage, const char* label) override {
    vk::Buffer* b = st->be->CreateBuffer(size, usage, label);
    if (!b) return {};
    auto impl = std::make_shared<VkrBuffer>();
    impl->b = b;
    impl->st = st;
    impl->size = size;
    return Buffer(std::move(impl));
  }

  Texture CreateTexture(const Extent3D&, TextureFormat, TextureUsage,
                        const char* label) override {
    if (!st->warnedNoTexture) {
      st->warnedNoTexture = true;
      std::fprintf(stderr,
                   "note: CreateTexture('%s') returns null on --backend vulkan "
                   "until phase 4b (render path)\n",
                   label ? label : "?");
    }
    return {};
  }

  QuerySet CreateTimestampQuerySet(uint32_t count, const char* label) override;

  BindGroupLayout CreateBindGroupLayout(const BindGroupLayoutEntry* entries,
                                        size_t count) override {
    auto impl = std::make_shared<VkrBindGroupLayout>();
    impl->l = st->be->CreateSetLayout(entries, count);
    if (impl->l == VK_NULL_HANDLE) return {};
    impl->entries.assign(entries, entries + count);
    return BindGroupLayout(std::move(impl));
  }

  PipelineLayout CreatePipelineLayout(const BindGroupLayout* groups,
                                      size_t count) override {
    std::vector<VkDescriptorSetLayout> sets(count);
    for (size_t i = 0; i < count; i++)
      sets[i] = static_cast<VkrBindGroupLayout*>(groups[i].Get())->l;
    auto impl = std::make_shared<VkrPipelineLayout>();
    impl->l = st->be->CreatePipelineLayout(sets.data(), count);
    if (impl->l == VK_NULL_HANDLE) return {};
    return PipelineLayout(std::move(impl));
  }

  BindGroup CreateBindGroup(const BindGroupLayout& layout, const BindGroupEntry* entries,
                            size_t count, const char* /*label*/) override {
    auto* bgl = static_cast<VkrBindGroupLayout*>(layout.Get());
    std::vector<vk::Buffer*> bufs(count);
    for (size_t i = 0; i < count; i++) bufs[i] = NB(entries[i].buffer);
    auto impl = std::make_shared<VkrBindGroup>();
    impl->set = st->be->CreateDescriptorSet(bgl->l, bgl->entries.data(), entries, count,
                                            bufs);
    if (impl->set == VK_NULL_HANDLE) return {};
    return BindGroup(std::move(impl));
  }

  ShaderModule CreateShaderModule(const std::string& wgsl, const char* label) override {
    // Deferred: Tint runs at pipeline creation, when the entry point is known.
    // The source is what LoadShader assembled (prelude + tuning + common +
    // body) — the exact string the engine feeds Dawn, which is the property
    // phase 3a's "no offline .spv" decision protects (F5 re-bakes tuning).
    auto impl = std::make_shared<VkrShaderModule>();
    impl->source = wgsl;
    impl->label = label ? label : "shader";
    return ShaderModule(std::move(impl));
  }

  ComputePipeline CreateComputePipeline(const PipelineLayout& layout,
                                        const ShaderModule& module, const char* entry,
                                        const char* label) override {
    auto* m = static_cast<VkrShaderModule*>(module.Get());
    auto* pl = static_cast<VkrPipelineLayout*>(layout.Get());
    std::string diag;
    VkShaderModule sm = st->be->GetShaderModule(m->source, m->label, entry,
                                                /*bodyLineOffset=*/0, diag);
    if (sm == VK_NULL_HANDLE) {
      std::fprintf(stderr, "shader compile failed for %s::%s\n%s\n", m->label.c_str(),
                   entry, diag.c_str());
      return {};
    }
    VkPipeline p = st->be->CreateComputePipeline(pl->l, sm, entry, label);
    if (p == VK_NULL_HANDLE) {
      std::fprintf(stderr, "compute pipeline creation failed for %s::%s\n",
                   m->label.c_str(), entry);
      return {};
    }
    auto impl = std::make_shared<VkrComputePipeline>();
    impl->p = p;
    return ComputePipeline(std::move(impl));
  }

  RenderPipeline CreateRenderPipeline(const RenderPipelineDesc& d) override {
    std::fprintf(stderr,
                 "note: CreateRenderPipeline('%s') returns null on --backend "
                 "vulkan until phase 4b\n",
                 d.label ? d.label : "?");
    return {};
  }

  CommandEncoder CreateCommandEncoder(const char* label) override {
    auto impl = std::make_shared<VkrEncoder>(st, label);
    if (impl->cmd == VK_NULL_HANDLE) return {};
    return CommandEncoder(std::move(impl));
  }

  void PushValidationScope() override { st->be->PushValidationScope(); }

  bool PopValidationScopeBlocking() override {
    std::string msgs;
    bool any = st->be->PopValidationScope(msgs);
    if (any) std::fprintf(stderr, "%s", msgs.c_str());
    return any;
  }

  void ProcessEvents() override {
    st->be->PollFences();
    FirePendingMaps();
  }

  void WaitIdle() override {
    // §4.9: a drain must not strand deferred uploads. If FillSlots (or any
    // QueueWrite) enqueued writes and nothing recorded a command buffer since,
    // flush them through a trivial submit before waiting — WebGPU's WriteBuffer
    // is queue-ordered, so "wait idle" must observe it applied.
    if (st->be->PendingUploadCount() > 0) {
      std::string err;
      VkCommandBuffer cmd = st->be->BeginCommands("waitIdleFlush");
      if (cmd != VK_NULL_HANDLE) st->be->SubmitCommands(cmd, err);
    }
    std::string err;
    st->be->WaitIdle(err);
  }

  bool ReadBufferBlocking(const Buffer& src, uint64_t offset, void* out,
                          size_t size) override {
    vk::Buffer* b = NB(src);
    if (!b || !b->mapped) return false;
    // The producing copy is already submitted (seam contract). Wait, then read
    // the persistent map — the Finish() host barrier made it visible.
    std::string err;
    if (!st->be->WaitIdle(err)) return false;
    std::memcpy(out, (const uint8_t*)b->mapped + offset, size);
    return true;
  }

  MapTicket MapReadDeferred(const Buffer& b, uint64_t offset, uint64_t size) override {
    auto impl = std::make_shared<VkrMapTicket>();
    impl->st = st;
    impl->buf = NB(b);
    impl->offset = offset;
    impl->size = size;
    impl->fence = st->lastFence;
    if (impl->fence != VK_NULL_HANDLE) st->be->RetainFence(impl->fence);
    return MapTicket(std::move(impl));
  }

  void FirePendingMaps() {
    for (size_t i = 0; i < st->maps.size();) {
      VkrState::PendingMap& m = st->maps[i];
      bool done = m.fence == VK_NULL_HANDLE || st->be->FenceStatus(m.fence) == VK_SUCCESS;
      if (!done) {
        i++;
        continue;
      }
      // Move out before firing: the callback may issue new maps.
      VkrState::PendingMap fired = std::move(m);
      st->maps.erase(st->maps.begin() + i);
      const void* p = fired.buf && fired.buf->mapped
                          ? (const uint8_t*)fired.buf->mapped + fired.offset
                          : nullptr;
      fired.done(p);
      if (fired.fence != VK_NULL_HANDLE) st->be->ReleaseFence(fired.fence);
    }
  }
};

// ---------------------------------------------------- D4: timestamp query ----

VkrQuerySet::~VkrQuerySet() {
  if (pool != VK_NULL_HANDLE && st && st->be && st->be->Device() != VK_NULL_HANDLE) {
    std::string err;
    st->be->WaitIdle(err);  // measure-only path; a blocking teardown is fine
    st->be->Fns().DestroyQueryPool(st->be->Device(), pool, nullptr);
  }
}

QuerySet VkrDevice::CreateTimestampQuerySet(uint32_t count, const char* /*label*/) {
  if (!st->be->GetCaps().timestampQuery) return {};
  VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
  ci.queryCount = count;
  VkQueryPool pool = VK_NULL_HANDLE;
  if (!st->be->Fns().CreateQueryPool ||
      st->be->Fns().CreateQueryPool(st->be->Device(), &ci, nullptr, &pool) != VK_SUCCESS)
    return {};
  // A fresh pool's queries are in an undefined state; reset them once before
  // first use (vkCmdResetQueryPool; the resolve resets again after each read).
  VkCommandBuffer cmd = st->be->BeginCommands("queryPoolInit");
  if (cmd != VK_NULL_HANDLE) {
    st->be->Fns().CmdResetQueryPool(cmd, pool, 0, count);
    std::string err;
    st->be->SubmitCommands(cmd, err);
  }
  auto impl = std::make_shared<VkrQuerySet>();
  impl->pool = pool;
  impl->count = count;
  impl->st = st;
  return QuerySet(std::move(impl));
}

void VkrEncoder::ResolveQuerySet(const QuerySet& qs, uint32_t firstQuery,
                                 uint32_t queryCount, const Buffer& dst,
                                 uint64_t dstOffset) {
  auto* q = static_cast<VkrQuerySet*>(qs.Get());
  vk::Buffer* d = NB(dst);
  if (!q || q->pool == VK_NULL_HANDLE || !d) return;
  // The copy is a transfer WRITE into `dst`; declare it to the tracker so the
  // following resolve->staging CopyBufferToBuffer derives its RAW barrier the
  // ordinary way instead of needing a hand-placed one.
  rec->DeclareUse(d, pass::Acc::TransferWrite);
  st->be->Fns().CmdCopyQueryPoolResults(cmd, q->pool, firstQuery, queryCount, d->buf,
                                        dstOffset, 8,
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  // Reset the consumed range so the next command buffer can reuse it. Legal
  // outside a render pass; ordering against the copy above is by submission
  // order within the buffer (queries, not memory).
  st->be->Fns().CmdResetQueryPool(cmd, q->pool, firstQuery, queryCount);
}

}  // namespace

// ---------------------------------------------------------------- bridge ----

Device WrapDevice(std::shared_ptr<vk::Backend> be, bool sledgehammer) {
  if (!be) return {};
  auto st = std::make_shared<VkrState>();
  st->be = std::move(be);
  st->mode = sledgehammer ? vk::BarrierMode::Sledgehammer : vk::BarrierMode::Precise;
  auto impl = std::make_shared<VkrDevice>();
  impl->st = std::move(st);
  return Device(std::move(impl));
}

vk::Backend* NativeBackend(const Device& d) {
  if (!d || d.Kind() != BackendKind::Vulkan) return nullptr;
  return static_cast<VkrDevice*>(d.Get())->st->be.get();
}

Stats LastStats(const Device& d) {
  Stats out;
  if (!d || d.Kind() != BackendKind::Vulkan) return out;
  const vk::RecordStats& s = static_cast<VkrDevice*>(d.Get())->st->lastStats;
  out.rows = s.rows;
  out.dispatches = s.dispatches;
  out.copies = s.copies;
  out.fills = s.fills;
  out.barrierCalls = s.barrierCalls;
  out.bufferBarriers = s.bufferBarriers;
  out.globalBarriers = s.globalBarriers;
  return out;
}

}  // namespace vkr

// RecordTableVulkan lives in namespace rhi (declared in rhi_record.h).
void RecordTableVulkan(const CommandEncoder& enc, pass::Table which, const TableCtx& cx,
                       const TableBindings& tb, PassTimer* timer) {
  using namespace vkr;
  auto* e = static_cast<VkrEncoder*>(enc.Get());
  if (!e || !e->rec) return;

  vk::Bindings bd{};
  for (int i = 0; i < (int)pass::Buf::kCount; i++) bd.buffers[i] = NB(tb.buffers[i]);
  for (int i = 0; i < 32; i++) {
    bd.pipelines[i] =
        tb.pipelines[i] ? static_cast<VkrComputePipeline*>(tb.pipelines[i].Get())->p
                        : VK_NULL_HANDLE;
  }
  auto layout = [](const PipelineLayout& l) {
    return l ? static_cast<VkrPipelineLayout*>(l.Get())->l : VK_NULL_HANDLE;
  };
  auto set = [](const BindGroup& g) {
    return g ? static_cast<VkrBindGroup*>(g.Get())->set : VK_NULL_HANDLE;
  };
  bd.simLayout = layout(tb.simLayout);
  bd.slimPartLayout = layout(tb.slimPartLayout);
  bd.slimFarLayout = layout(tb.slimFarLayout);
  bd.simSet = set(tb.simSet);
  bd.slimSet = set(tb.slimSet);
  bd.particleSet = set(tb.particleSet);
  bd.farSet = set(tb.farSet);

  e->rec->SetBindings(bd);

  // --measure hook: timestamps around each run of rows sharing a group label,
  // mirroring Dawn's per-ComputePassEncoder writes so the reports compare.
  if (timer && timer->Valid()) {
    auto* q = static_cast<VkrQuerySet*>(timer->NativeQuerySet().Get());
    if (q && q->pool != VK_NULL_HANDLE) {
      e->rec->SetTimer(q->pool, [timer](const char* name, uint32_t& b, uint32_t& en) {
        return timer->AllocPassPair(name, b, en);
      });
    }
  }

  vk::RecordCtx cxv{};
  cxv.opsCount = cx.opsCount;
  cxv.cellCount = cx.cellCount;
  cxv.expCount = cx.expCount;
  cxv.spawnCount = cx.spawnCount;
  cxv.genCount = cx.genCount;
  cxv.farCount = cx.farCount;
  cxv.hashEnable = cx.hashEnable;
  cxv.particlesActive = cx.particlesActive;
  e->rec->RecordTable(which, cxv);
}

}  // namespace rhi
