// rhi_vulkan.h — the Vulkan backend's foundations (port phase 3a).
//
// SCOPE, AND WHAT IS DELIBERATELY MISSING
// ---------------------------------------
// Phase 3a builds the things a Vulkan backend needs before it can record
// anything: a device, memory, an upload path, a readback path, shader
// compilation, and pipelines. It executes NO sim work. The only commands this
// file ever submits are the zero-init fills and (from --vk-info) an empty
// command buffer, because the value of foundations is only provable by running
// them once.
//
// Phase 3b adds command recording and the barriers generated from
// src/sim/pass_table.def. Phase 3c wires the whole tick chain and the
// `--backend vulkan` flag. Until then Dawn is the only live backend and this
// type is reachable only from `--vk-info`.
//
// WHY THIS IS NOT YET AN rhi::Device IMPLEMENTATION
// -------------------------------------------------
// src/gpu/rhi.h is a seam with ONE backend behind it today (rhi_dawn.cpp). A
// second implementation of the same handle types cannot coexist in one binary
// without either a virtual dispatch layer or a compile-time switch, and phase 2a
// deliberately chose neither while Dawn is the hash oracle. So phase 3a exposes
// its own concrete `vk::Backend` type and phase 3b decides how it plugs in, with
// the pass table (which is backend-neutral) as the join. Naming the classes here
// after the seam's concepts keeps that a mechanical step.
//
// FIVE SEMANTICS THE SEAM PROMISES, AND WHERE EACH IS HONORED HERE
// (phase 2a recorded them; they are the reason this file is shaped as it is)
//
//   1. WriteBuffer is queue-ORDERED and deferred to the next submit.
//      -> QueueWrite() appends to a pending-upload queue that flushes at the
//         head of the next recorded command buffer, in issue order, never
//         coalesced. barrier_graph §4.1.
//   2. A binding with size 0 means "rest of the buffer from offset".
//      -> CreateBindGroup resolves 0 against the buffer's cached size, which is
//         why Buffer carries its size at all.
//   3. MapTicket: a map issued now, polled later, consumed later.
//      -> a fence + a persistently mapped host-visible allocation. Ready() is
//         vkGetFenceStatus; Wait() is vkWaitForFences.
//   4. Validation errors are reported device-scoped, not per-call.
//      -> the debug messenger collects into a scope the F5 reload can inspect,
//         mirroring PushValidationScope/PopValidationScopeBlocking.
//   5. Blocking readbacks exist and are sanctioned only in tests/screenshots.
//      -> ReadBufferBlocking: record copy, submit fenced, wait, read the map.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/rhi.h"
#include "gpu/vk_loader.h"

// VMA's handle types. The full header is included only by the .cpp files that
// need it, so vk_mem_alloc.h does not leak into the whole engine.
VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace vk {

// ---------------------------------------------------------------- caps ----
//
// The capability record. This is not diagnostics: phase 7's sparse-residency
// payoff (a 4 GiB virtual voxels buffer backed only where non-air lives, ~83%
// of the 512 MiB dense allocation saved) is GATED on two of these bits, and the
// window-size question is gated on maxStorageBufferRange. --vk-info prints the
// whole struct so the decision rests on measurements from the actual device.
struct Caps {
  std::string deviceName;
  uint32_t apiVersion = 0;
  uint32_t driverVersion = 0;
  uint32_t vendorId = 0, deviceId = 0;
  bool discrete = false;

  // --- phase 7 gates ---
  // Sparse binding at all, and specifically for buffers.
  bool sparseBinding = false;
  bool sparseResidencyBuffer = false;
  // THE decisive one. Sim kernels legitimately read empty neighbours (a dirty
  // chunk at a sky boundary), so unbound pages WILL be read. Only
  // residencyNonResidentStrict guarantees those reads return zero — and the
  // zero word is inert air by construction, which is what makes the whole
  // scheme deterministic. Without it, sparse is DISABLED and the dense path
  // runs; a "probably reads zero" would be a rule-1 violation.
  bool residencyNonResidentStrict = false;
  // Whether a single buffer binding can cover a 4 GiB virtual allocation, and
  // how big the window can grow.
  uint64_t maxStorageBufferRange = 0;
  uint64_t maxMemoryAllocationSize = 0;

  // --- dispatch/limits the CA loop cares about ---
  uint32_t maxComputeWorkGroupInvocations = 0;
  uint32_t maxComputeWorkGroupSize[3] = {0, 0, 0};
  uint32_t maxComputeWorkGroupCount[3] = {0, 0, 0};
  uint32_t maxBoundDescriptorSets = 0;
  uint32_t maxPerStageDescriptorStorageBuffers = 0;

  // --- alignment, which the upload path and dynamic offsets must respect ---
  uint64_t minStorageBufferOffsetAlignment = 0;
  uint64_t minUniformBufferOffsetAlignment = 0;
  uint64_t nonCoherentAtomSize = 0;

  // --- measurement ---
  bool timestampQuery = false;
  float timestampPeriodNs = 0.0f;

  // --- validation ---
  bool validationAvailable = false;
  bool validationEnabled = false;
  bool syncValidationEnabled = false;

  // --- dynamic rendering (phase 4b) ---
  // The render path records with vkCmdBeginRendering rather than VkRenderPass
  // objects (barrier_graph §1.2 already assumes it). Mandatory in core 1.3 —
  // like synchronization2 it still must be ENABLED at device creation, and the
  // backend refuses to init without it rather than shipping a second (render
  // pass object) code path nothing would exercise.
  bool dynamicRendering = false;

  // --- synchronization2 (phase 3b) ---
  // vkCmdPipelineBarrier2 is the ONE barrier command the generated-barrier
  // recorder emits, and every scope in docs/vulkan_barrier_graph.md §3.2 is
  // written in VkPipelineStageFlags2/VkAccessFlags2 terms. Core in 1.3, but a
  // core command still requires its FEATURE to be enabled at device creation.
  // If the device cannot offer it, the backend refuses to initialise rather
  // than falling back to the 1.0 barrier: the fallback would have to
  // down-convert every scope, and a silently weaker barrier is precisely the
  // failure mode rule 1 cannot tolerate.
  bool synchronization2 = false;

  // --- fragment-stage storage writes (the shadow cache) ---
  // The render bind group was entirely ReadOnlyStorage until the voxel-keyed
  // shadow cache, which is the first buffer a FRAGMENT shader writes. This is a
  // Vulkan 1.0 core feature and universally supported on desktop, but it is
  // OPTIONAL, and a storage write from a fragment shader on a device that did
  // not enable it is undefined behaviour rather than an error — the same class
  // of silent failure the synchronization2 note above refuses to accept.
  //
  // Unlike synchronization2 this one does NOT refuse to initialise. The cache
  // is selected by a COMPILE-TIME const in the shader prelude, so a device
  // without the feature compiles the original inline-shadow-ray variant of
  // raymarch.wgsl and loses performance, not correctness. That is also why the
  // toggle cannot be a runtime branch: the whole point of the cache is that the
  // shadow trace() call site is ABSENT from the compiled fragment shader
  // (measured 3.59 ms of register footprint), and a runtime branch would keep
  // it resident and give back most of the win.
  bool fragmentStoresAndAtomics = false;
};

// -------------------------------------------------------------- buffer ----

struct Buffer {
  VkBuffer buf = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;
  uint64_t size = 0;
  // Non-null for host-visible allocations (staging ring, readback slots).
  void* mapped = nullptr;
  std::string label;
};

// --------------------------------------------------------------- image ----
//
// Phase 4b: render attachments (offscreen color, the depth buffer, swapchain
// images) and the screenshot copy source. `layout` is the image's CURRENT
// layout — a property of the image, not of a recording, so it persists across
// command buffers and the recorder derives every transition from it
// (vk_record.h: no hand-placed image barriers either).
struct Image {
  VkImage img = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;  // null when the swapchain owns the VkImage
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
  VkImageAspectFlags aspect = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  // Swapchain image: the recorder's Finish() transitions it to PRESENT_SRC
  // after the last touch, and the submit that rendered it waits the acquire
  // semaphore / signals the per-image render-done semaphore.
  bool presentable = false;
  // SAMPLED image: something outside the recorder is going to READ this in a
  // shader, so the recorder's Finish() leaves it in SHADER_READ_ONLY_OPTIMAL
  // after the last touch — the exact parallel of `presentable` above, and for
  // the same reason. A colour attachment otherwise ends a pass in
  // COLOR_ATTACHMENT_OPTIMAL and stays there, so a descriptor that names it as
  // a sampled image is describing a layout the image is not in.
  //
  // The one consumer today is the character panel's avatar portrait, which
  // ImGui samples through a descriptor of its own (src/ui/overlay.cpp). Set
  // from TextureUsage::TextureBinding at creation, so any future
  // render-to-texture gets it without touching this file again.
  bool sampled = false;
  std::string label;
};

// ------------------------------------------------------------- backend ----

class Backend {
 public:
  Backend() = default;
  ~Backend();
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;

  // Create instance + device. `lowPower` prefers an integrated GPU (mirrors
  // `--adapter low`, which exists so the world hash can be compared across
  // vendors — DESIGN.md risk 3). `validation` enables VK_LAYER_KHRONOS_validation;
  // `syncValidation` additionally turns on synchronization validation, which the
  // barrier document names the PRIMARY detector for a missing barrier. Both are
  // plumbed now even though nothing records barriers yet, because the moment
  // phase 3b starts generating them is the moment it needs to already be here.
  // `instanceExts`/`instanceExtCount` (phase 4b D3): extra instance extensions,
  // i.e. GLFW's surface extensions when windowed. `wantSwapchain` additionally
  // enables VK_KHR_swapchain on the device (refused if absent).
  bool Init(bool lowPower, bool validation, bool syncValidation, std::string& err,
            const char* const* instanceExts = nullptr, uint32_t instanceExtCount = 0,
            bool wantSwapchain = false);
  void Shutdown();

  const Caps& GetCaps() const { return caps_; }
  VkDevice Device() const { return device_; }
  const vkl::DeviceFns& Fns() const { return dfn_; }
  // For the windowed path + imgui_impl_vulkan (src/ui/overlay.cpp via rhi_vk.h).
  VkInstance Instance() const { return instance_; }
  VkPhysicalDevice PhysicalDevice() const { return phys_; }
  VkQueue GpuQueue() const { return queue_; }
  uint32_t QueueFamily() const { return queueFamily_; }
  // vkGetInstanceProcAddr against the live instance — ImGui's function loader.
  PFN_vkVoidFunction InstanceProc(const char* name) const;

  // ---- swapchain (phase 4b D3) ----
  //
  // FIFO present mode, matching Dawn's PresentMode::Fifo. `surface` is taken
  // on the FIRST call and owned by the backend from then on; pass
  // VK_NULL_HANDLE to recreate at a new size (resize). Recreation drains the
  // queue first.
  bool ConfigureSwapchain(VkSurfaceKHR surface, uint32_t w, uint32_t h,
                          std::string& err);
  // Acquire the next image. Null on OUT_OF_DATE (caller skips the frame; the
  // resize path reconfigures) or if no swapchain exists.
  Image* AcquireSwapchainImage();
  // Present the acquired image (after the presenting submit). Tolerates
  // SUBOPTIMAL/OUT_OF_DATE — the next resize reconfigures.
  void PresentAcquired();
  rhi::TextureFormat SwapchainFormat() const;
  uint32_t SwapchainImageCount() const { return (uint32_t)swapImages_.size(); }
  // SubmitEnded, plus the swapchain semaphores: waits the pending acquire
  // semaphore at COLOR_ATTACHMENT_OUTPUT and signals the acquired image's
  // render-done semaphore (which PresentAcquired waits on). Used by the seam
  // for any command buffer whose render pass targeted a swapchain image.
  VkFence SubmitEndedPresenting(VkCommandBuffer cmd, std::string& err);

  // ---- buffers (barrier_graph §4.8) ----
  //
  // THE ONLY buffer constructor. It unconditionally adds TRANSFER_DST and
  // registers the buffer for ZeroInitAll(). WebGPU guarantees zero-initialized
  // buffers and Vulkan guarantees nothing, so this is a mechanism rather than a
  // list — the barrier doc's own draft tried to enumerate "buffers that need
  // zeroing", missed two of them including the worst case, and that is exactly
  // the shape this replaces. Do not add a "skip zero-init" parameter.
  Buffer* CreateBuffer(uint64_t size, rhi::BufferUsage usage, const char* label);

  // vkCmdFillBuffer(0) over every registered buffer, in one command buffer,
  // submitted and waited. Called once after all buffers exist (--vk-info's
  // explicit path; the seam relies on the queued per-buffer fill below).
  bool ZeroInitAll(std::string& err);

  // Free a buffer created by CreateBuffer, DEFERRED until every submit that was
  // in flight at the call has retired (phase 4a). The seam's rhi::Buffer
  // handles are refcounted and gates create/drop large staging buffers freely —
  // WebGPU frees them on release, so leaving them in the registry until
  // Shutdown would leak a 512 MiB staging read per whole-world gate.
  // Precondition kept by construction: only ad-hoc staging is ever destroyed
  // this way; buffers referenced by descriptor sets live for the device's life.
  void DestroyBufferDeferred(Buffer* b);

  // ---- uploads (barrier_graph §4.1) ----
  //
  // Reproduces queue.WriteBuffer's two guarantees: deferred to the next submit,
  // and applied in ISSUE ORDER so the last write to a range before a submit
  // wins. Class A (<= 65536 B, 4-aligned) captures the payload for
  // vkCmdUpdateBuffer; Class B copies into the staging ring for vkCmdCopyBuffer.
  // Nothing is coalesced, because coalescing is what would break last-write-wins.
  void QueueWrite(Buffer* dst, uint64_t offset, const void* data, size_t size);

  // Record every pending upload at the head of `cmd`, in issue order, and clear
  // the queue. Must run before any pass row. A drain (WaitIdle, shutdown, save)
  // that skips this leaves a FillSlots that submitted nothing unapplied — see
  // barrier_graph §4.9.
  void FlushUploads(VkCommandBuffer cmd);
  size_t PendingUploadCount() const { return pending_.size(); }

  // ---- submit (barrier_graph §4.2) ----
  //
  // EVERY submit gets a fence. No exceptions, and the reason is not readbacks:
  // Class B staging regions are reclaimed by the fence of the submit that
  // consumed them, so a fenceless submit leaks its ring region permanently. The
  // --shot far-fill loop is the case that proves it — it submits in a tight
  // loop, carries no readback, and uploads every iteration.
  VkCommandBuffer BeginCommands(const char* label);
  // Ends and submits `cmd` with a fence from the pool. Returns the fence, which
  // the caller may poll; it is retired automatically once signalled.
  VkFence SubmitCommands(VkCommandBuffer cmd, std::string& err);
  // Submit a command buffer the caller ALREADY ended (the seam's
  // CommandEncoder::Finish ends it, matching wgpu's Finish/Submit split).
  VkFence SubmitEnded(VkCommandBuffer cmd, std::string& err);
  bool WaitIdle(std::string& err);
  // Retire any signalled fences: reclaims staging-ring regions and command
  // buffers. Non-blocking; this is ProcessEvents()' replacement.
  void PollFences();

  // ---- borrowed fences (barrier_graph §4.2) ------------------------------
  //
  // The readback ring and the eviction pool do not own fences: §4.2 says a
  // readback slot "borrows a reference to that submit's fence", because every
  // submit gets one anyway (for staging-ring reclamation) and a second fence
  // per slot would decouple two lifetimes that should not be decoupled.
  //
  // BUT A BORROWED FENCE NEEDS A RETAIN, and phase 3c found this the hard way.
  // `PollFences()` recycles a signalled fence into `freeFences_` immediately,
  // and `BeginCommands()` calls `PollFences()` on EVERY command buffer. So a
  // slot that submitted at tick N and had not yet been polled by tick N+1 held
  // a handle that `AcquireFence` had already reset and handed to the tick-N+1
  // submit. `vkGetFenceStatus` on it then reports the LATER submit's status:
  // the slot reads its mapped memory when a completely different command buffer
  // finishes, which for a 3-deep ring means reading a slot the GPU is still
  // writing. That is silent data corruption in the CPU mirror, not a crash —
  // exactly the class of bug the ring exists to prevent.
  //
  // So: `RetainFence` pins a fence against recycling; `ReleaseFence` unpins it,
  // and the fence returns to the pool once BOTH the submit has retired and
  // every borrower has released. A borrower must release exactly once.
  void RetainFence(VkFence f);
  void ReleaseFence(VkFence f);
  // Non-blocking status of a retained fence. VK_SUCCESS = the submit that
  // signalled it has completed.
  VkResult FenceStatus(VkFence f) const;
  // Blocking wait on a retained fence — the eviction pool's `CompleteOldest`
  // (§4.3 step 5), which is a genuine block exactly as `WaitAny` blocks today.
  bool WaitFence(VkFence f, std::string& err);

  // ---- images + graphics pipelines (phase 4b) ----
  //
  // CreateImage allocates a device-local VkImage + view. No zero-init queue
  // entry: attachments are initialized by their loadOp (Clear) and the
  // UNDEFINED->attachment transition the recorder derives, which is the Vulkan
  // idiom WebGPU's lazy-clear maps onto for render targets.
  Image* CreateImage(uint32_t w, uint32_t h, rhi::TextureFormat fmt,
                     rhi::TextureUsage usage, const char* label);
  // Deferred like buffers: freed once every submit in flight at the call has
  // retired (the depth target is recreated on resize; gates create and drop
  // offscreen targets).
  void DestroyImageDeferred(Image* im);

  // Graphics pipeline against dynamic rendering (no VkRenderPass object).
  // `d` supplies formats/blend/cull/topology/depth; vs/fs are Tint-compiled
  // single-entry-point modules from GetShaderModule.
  VkPipeline CreateGraphicsPipeline(VkPipelineLayout layout, VkShaderModule vs,
                                    const char* vsEntry, VkShaderModule fs,
                                    const char* fsEntry, const rhi::RenderPipelineDesc& d,
                                    const char* label);

  // ---- shaders ----
  //
  // Compiles WGSL to SPIR-V through Tint and creates a VkShaderModule. Cached
  // by (label, source hash) so the 12 shader files that produce 20+ pipelines
  // compile once each rather than once per entry point.
  VkShaderModule GetShaderModule(const std::string& wgsl, const std::string& label,
                                 const std::string& entryPoint, uint32_t bodyLineOffset,
                                 std::string& diagnostics);

  // ---- descriptors and pipelines ----
  VkDescriptorSetLayout CreateSetLayout(const rhi::BindGroupLayoutEntry* entries,
                                        size_t count);
  VkPipelineLayout CreatePipelineLayout(const VkDescriptorSetLayout* sets, size_t count);
  VkPipeline CreateComputePipeline(VkPipelineLayout layout, VkShaderModule module,
                                   const char* entry, const char* label);
  // Allocates and writes a descriptor set. A binding with size 0 means "rest of
  // the buffer from offset" (rhi.h semantics), resolved here against the
  // buffer's cached size — which is the reason Buffer stores one.
  //
  // `layoutEntries` MUST be the same array the layout was created from: the
  // descriptor TYPE in a VkWriteDescriptorSet has to match the layout binding's
  // type exactly, and a mismatch is undefined behaviour that faults inside the
  // ICD here rather than erroring (no validation layer on this machine). An
  // earlier version of this function hardcoded STORAGE_BUFFER for every write,
  // which was silently wrong for every uniform binding — including passUBO,
  // the dynamic one.
  VkDescriptorSet CreateDescriptorSet(VkDescriptorSetLayout layout,
                                      const rhi::BindGroupLayoutEntry* layoutEntries,
                                      const rhi::BindGroupEntry* entries, size_t count,
                                      const std::vector<Buffer*>& buffers);

  // ---- validation scope (rhi.h PushValidationScope/PopValidationScopeBlocking) ----
  void PushValidationScope();
  bool PopValidationScope(std::string& messages);

  // Messages the debug messenger has collected, regardless of scope.
  const std::vector<std::string>& ValidationMessages() const { return validationMsgs_; }

 private:
  struct Pending {
    Buffer* dst = nullptr;
    uint64_t dstOffset = 0;
    uint64_t size = 0;
    // Class A: the payload, captured now and copied into the command buffer at
    // record time by vkCmdUpdateBuffer.
    std::vector<uint8_t> inlineData;
    // Class B: offset into the staging ring.
    uint64_t stagingOffset = 0;
    bool classA = true;
    // Zero-init (§4.8, phase 4a form): a whole-buffer vkCmdFillBuffer(0),
    // queued by CreateBuffer itself so it drains at the head of the next
    // command buffer — BEFORE any recorded use of the buffer, in issue order,
    // so a data upload queued after creation still wins. This replaced the
    // one-shot ZeroInitAll submit when buffer creation moved behind the seam
    // (buffers are now created at many times, not one init moment).
    bool zeroFill = false;
  };

  struct InFlight {
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint64_t stagingHigh = 0;  // ring high-water at submit; reclaimed on retire
    uint64_t serial = 0;       // submit order, for the buffer graveyard
  };

  // A buffer whose seam handle was released while submits that might reference
  // it were still in flight. Freed once every submit with serial <= `serial`
  // has retired.
  struct Doomed {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    uint64_t serial = 0;
  };
  // Same discipline for images (phase 4b).
  struct DoomedImage {
    VkImage img = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t serial = 0;
  };

  bool PickPhysicalDevice(bool lowPower, std::string& err);
  void QueryCaps();
  bool CreateLogicalDevice(std::string& err);
  bool InitAllocator(std::string& err);
  VkFence AcquireFence(std::string& err);

  bool swapchainRequested_ = false;  // Init(wantSwapchain): enable VK_KHR_swapchain

  vkl::GlobalFns gfn_{};
  vkl::InstanceFns ifn_{};
  vkl::DeviceFns dfn_{};

  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VkCommandPool cmdPool_ = VK_NULL_HANDLE;
  VkDescriptorPool descPool_ = VK_NULL_HANDLE;
  VmaAllocator allocator_ = nullptr;

  Caps caps_{};

  // The zero-init registry (§4.8). Owning, so the backend can free everything.
  std::vector<std::unique_ptr<Buffer>> buffers_;
  // Image registry (phase 4b). Owning, same lifetime rules as buffers_.
  std::vector<std::unique_ptr<Image>> images_;
  std::vector<DoomedImage> imageGraveyard_;

  // ---- swapchain state (phase 4b D3) ----
  void DestroySwapchainObjects();  // views/semaphores/swapchain, not the surface
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swapFormat_ = VK_FORMAT_UNDEFINED;
  std::vector<std::unique_ptr<Image>> swapImages_;  // wrap swapchain VkImages
  std::vector<VkSemaphore> renderDone_;             // one per swapchain image
  // Acquire semaphores: a small ring paced by the fence of the submit that
  // consumed each one — a semaphore handed to vkAcquireNextImageKHR must be
  // unsignaled and unused, and the fence wait is what proves it.
  struct AcquireSlot {
    VkSemaphore sem = VK_NULL_HANDLE;
    VkFence lastUse = VK_NULL_HANDLE;  // retained; released before reuse
  };
  static constexpr int kAcquireSlots = 3;
  AcquireSlot acquireSlots_[kAcquireSlots];
  int acquireCursor_ = 0;
  AcquireSlot* pendingAcquireSlot_ = nullptr;  // consumed by the next presenting submit
  uint32_t acquiredIndex_ = UINT32_MAX;

  // Pending uploads, in ISSUE ORDER. Never sorted, never coalesced.
  std::vector<Pending> pending_;
  Buffer* stagingRing_ = nullptr;
  uint64_t stagingHead_ = 0;

  std::vector<InFlight> inFlight_;
  uint64_t submitSerial_ = 0;
  std::vector<Doomed> graveyard_;
  std::vector<VkFence> freeFences_;
  // Borrow counts for fences pinned by RetainFence. A fence with a non-zero
  // count is never returned to freeFences_, so a borrower's handle stays valid
  // and keeps meaning the submit it was taken from. Entries are erased when the
  // count reaches zero AND the submit has retired.
  std::unordered_map<VkFence, uint32_t> fenceRetain_;
  // Fences whose submit retired while still retained: signalled, valid, and
  // waiting for the last ReleaseFence to hand them back to the pool.
  std::vector<VkFence> retiredRetained_;

  std::unordered_map<std::string, VkShaderModule> moduleCache_;
  VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
  std::string pipelineCachePath_;
  std::vector<VkDescriptorSetLayout> setLayouts_;
  std::vector<VkPipelineLayout> pipeLayouts_;
  std::vector<VkPipeline> pipelines_;

  std::vector<std::string> validationMsgs_;
  bool validationScopeOpen_ = false;
  size_t validationScopeMark_ = 0;

  friend VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT*, void*);
};

// Class A is the vkCmdUpdateBuffer path and its limit is 65536 bytes. Two
// engine buffers sit EXACTLY on that boundary — kMaxDebugBoxes * sizeof(DebugBox)
// and kMaterialSlots * sizeof(MicroBrickGpu) are both 65536 — so each is one
// constant bump away from silently becoming an illegal update. The
// static_asserts live in rhi_vulkan.cpp next to the classification rule, so a
// bump fails the BUILD rather than producing a validation error at runtime.
inline constexpr uint64_t kClassAMaxBytes = 65536;

}  // namespace vk
