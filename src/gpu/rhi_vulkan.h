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

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

  // --- VK_KHR_pipeline_executable_properties (`--shader-stats`) ------------
  // The instrument, not a feature the engine runs on. With it, the driver will
  // report per-executable statistics — register count, spill/scratch bytes,
  // occupancy — for a pipeline created with CAPTURE_STATISTICS, which is the
  // difference between MEASURING register pressure and guessing at it. Wholly
  // optional: absent, `--shader-stats` prints one line saying so and exits 2,
  // and nothing else in the engine notices.
  bool pipelineExecutableProps = false;
};

// One statistic the driver reports for one pipeline executable. Values are kept
// as a double plus the driver's own name/description: the KHR API is
// deliberately open-ended (every vendor names its own counters) so this must
// never grow a list of known statistic names — see vk_shader_stats.cpp.
struct PipelineStat {
  std::string name;
  std::string description;
  double value = 0.0;
  bool isBool = false;  // format was BOOL32; print YES/no rather than 1/0
};

// One executable (a stage's compiled ISA) of one pipeline.
struct PipelineExecutable {
  std::string pipeline;    // the engine's label: "raymarch", "sim_step", ...
  std::string stage;       // "compute", "fragment", "vertex", "vertex|fragment"
  std::string name;        // the driver's name for the executable
  std::string description;
  uint32_t subgroupSize = 0;
  std::vector<PipelineStat> stats;
};

// -------------------------------------------------------------- buffer ----

struct Buffer {
  VkBuffer buf = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;
  uint64_t size = 0;
  // Non-null for host-visible allocations (staging ring, readback slots).
  void* mapped = nullptr;
  // The memory type's property flags, as chosen by VMA. Recorded because the
  // ONE thing a reader of a mapped pointer needs to know is not derivable from
  // the pointer: whether the host cache has to be invalidated before the CPU
  // can see what the device wrote. Also what makes the HOST_CACHED question
  // answerable without a debugger: CreateBuffer prints the readback type's
  // flags once, on stderr, at startup.
  VkMemoryPropertyFlags memProps = 0;
  // Host-visible but NOT HOST_COHERENT: every CPU read of device-written bytes
  // must be preceded by vkInvalidateMappedMemoryRanges. False on every desktop
  // GPU seen so far (they all expose HOST_VISIBLE|HOST_COHERENT|HOST_CACHED),
  // but "prefer cached" is a PREFERENCE — a device that only has an uncoherent
  // cached type would silently read stale bytes without this.
  bool needsInvalidate = false;
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
  // Persist the driver pipeline cache to disk now (see the definition for why
  // Shutdown is the wrong only-place). Safe to call any time after Init.
  void SavePipelineCache();

  const Caps& GetCaps() const { return caps_; }

  // ---- `--shader-stats` (VK_KHR_pipeline_executable_properties) ----
  //
  // MUST BE SET BEFORE THE FIRST PIPELINE IS CREATED — i.e. before
  // Simulation::Init — because CAPTURE_STATISTICS is a CREATE flag, and a
  // pipeline already built without it reports no statistics at all. Setting it
  // also bypasses the on-disk pipeline cache for every subsequent create: a
  // cache hit hands back an object the driver never compiled, so there is
  // nothing for it to report. Both costs are paid once, in a headless
  // diagnostic mode nothing else runs.
  void SetCaptureStats(bool on) { captureStats_ = on; }
  bool CaptureStats() const { return captureStats_; }
  // Every pipeline created on this device, with its engine label, walked
  // through vkGetPipelineExecutable{Properties,Statistics}KHR. Empty when the
  // extension is absent or SetCaptureStats was never called.
  std::vector<PipelineExecutable> CollectPipelineStats() const;

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
  // Present mode per SetPresentMode (FIFO fallback). `surface` is taken
  // on the FIRST call and owned by the backend from then on; pass
  // VK_NULL_HANDLE to recreate at a new size (resize). Recreation drains the
  // queue first.
  bool ConfigureSwapchain(VkSurfaceKHR surface, uint32_t w, uint32_t h,
                          std::string& err);
  // The mode the NEXT ConfigureSwapchain asks for (rhi::PresentMode). Falls
  // back to FIFO when the surface does not offer it; ActivePresentMode says
  // what was actually created.
  void SetPresentMode(rhi::PresentMode m) { presentMode_ = m; }
  rhi::PresentMode RequestedPresentMode() const { return presentMode_; }
  rhi::PresentMode ActivePresentMode() const { return activePresentMode_; }
  // True when the swapchain images were created with TRANSFER_DST, i.e. a
  // CommandEncoder::BlitTexture into them is legal.
  bool SwapchainBlittable() const { return swapBlittable_; }
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

  // ---- an ABANDONED command buffer gives its uploads back (P3-E) -----------
  //
  // FlushUploads runs in BeginCommands, i.e. when the command buffer is
  // CREATED, not when it is submitted. So `CreateCommandEncoder()` is what
  // consumes the pending-upload queue, and a caller that creates an encoder and
  // then decides it has nothing to record DELETES every write issued before it.
  //
  // That is not hypothetical and it is not survivable. The free-confirmation
  // probe in support.cpp created its encoder before discovering that none of
  // its candidate slots still had a page, and returned without submitting: the
  // page-table writes PageTable::Materialize had just issued died with it, the
  // GPU kept the pre-allocation JITTER sentinel, and the tick's `pagefill` and
  // the next shift's `genChunk` wrote 4,096 words each through a sentinel.
  // 21,733,376 lost voxels on one gate, and the CPU-side table was correct the
  // whole time, which is what made it invisible from every counter.
  //
  // So the encoder handle owns the debt: dropping it without Finish/Submit
  // calls this, which puts the swallowed writes back at the FRONT of the queue
  // (issue order is the contract) and releases the command buffer. Exact rather
  // than heuristic — the destructor knows the buffer is dead, where a "was the
  // previous one submitted yet?" test at the next BeginCommands could not tell
  // a dead encoder from a live second one.
  void AbandonCommands(VkCommandBuffer cmd);
  uint64_t FlushesRecovered() const { return flushesRecovered_; }

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

  // Make a range of a readback buffer's mapped memory readable by the CPU.
  // A no-op (and free) on HOST_COHERENT memory, which is every allocation this
  // engine has actually been given; the call exists so that "prefer cached"
  // cannot turn into "read stale bytes" on a device where the cached type is
  // not also coherent. Every path that hands a mapped pointer to a reader —
  // MapReadDeferred's Data(), MapReadAsync's callback, ReadBufferBlocking —
  // goes through here.
  void InvalidateForRead(Buffer* b, uint64_t offset, uint64_t size);

  // Staging-ring diagnostics: how many times QueueWrite had to BLOCK on a
  // fence to get ring space, and how many Class B writes fell back to Class A
  // because the ring could not be reclaimed at all. Both should be 0; a
  // non-zero fallback count means the ring is undersized for one unsubmitted
  // batch (see kStagingRingBytes).
  uint64_t StagingStalls() const { return stagingStalls_; }
  uint64_t StagingFallbacks() const { return stagingFallbacks_; }

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
  //
  // THREAD-SAFE AND CONCURRENT (Simulation::BuildPipelines fans the creates
  // out over a pool). `shaderMutex_` covers the cache lookup and the publish,
  // NOT the compile: Tint and — once package B lands — the SPIR-V optimizer
  // run outside it, which is where the parallel win is. Two threads asking for
  // the same key compile it once; the second waits on `shaderCv_`.
  VkShaderModule GetShaderModule(const std::string& wgsl, const std::string& label,
                                 const std::string& entryPoint, uint32_t bodyLineOffset,
                                 std::string& diagnostics);

  // ---- descriptors and pipelines ----
  VkDescriptorSetLayout CreateSetLayout(const rhi::BindGroupLayoutEntry* entries,
                                        size_t count);
  VkPipelineLayout CreatePipelineLayout(const VkDescriptorSetLayout* sets, size_t count);
  // THREAD-SAFE. vkCreateComputePipelines is internally synchronized with
  // respect to the VkPipelineCache handed to it (Vulkan 1.3 §10.6: pipeline
  // cache objects are the one exception to the "externally synchronized"
  // default), and neither `layout` nor `module` is an externally-synchronized
  // parameter of the call. The only thing here that was NOT safe is our own
  // bookkeeping — the `pipelines_` label table — which now appends under
  // `pipelineMutex_`.
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
    // The ABSOLUTE (never wrapped) staging-ring span this submit's copies read
    // from: [stagingLow, stagingHigh). Reclaimed when the fence signals. These
    // used to be a single `stagingHigh` that NOTHING EVER READ — the ring was a
    // bump allocator that wrapped unconditionally, so a burst larger than the
    // ring silently overwrote bytes a queued vkCmdCopyBuffer had not read yet.
    // Nothing hit it because the class rule kept almost everything out of the
    // ring; routing 16 KiB page writes through it makes the burst 16 MiB.
    uint64_t stagingLow = 0;
    uint64_t stagingHigh = 0;
    uint64_t serial = 0;  // submit order, for the buffer graveyard
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

  // ---- the Class B staging ring (barrier_graph §4.1) ---------------------
  //
  // Reserve `size` bytes and return the PHYSICAL byte offset into the ring, or
  // kStagingNoRoom if the request cannot be satisfied without overwriting bytes
  // a submitted-or-still-pending copy has yet to read. The failure mode is a
  // stall (wait on the oldest submit) and then a Class A fallback — never a
  // silent overwrite.
  static constexpr uint64_t kStagingNoRoom = UINT64_MAX;
  uint64_t StagingAlloc(uint64_t size);
  // Push an InFlight entry for a just-submitted command buffer, charging it the
  // staging span its flush consumed. The ONE place inFlight_ grows, so the two
  // vkQueueSubmit call sites cannot disagree about ring accounting.
  void NoteSubmit(VkFence fence, VkCommandBuffer cmd);
  // Advance the reclaim floor to the low end of the oldest submit still in
  // flight. Side-effect free (no command buffers freed, no fences recycled), so
  // it is safe to call from QueueWrite with an encoder open.
  void ReclaimStaging();

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
  rhi::PresentMode presentMode_ = rhi::PresentMode::Fifo;
  rhi::PresentMode activePresentMode_ = rhi::PresentMode::Fifo;
  bool swapBlittable_ = false;
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
  // ABSOLUTE byte counters into a conceptually infinite ring; the physical
  // offset is `% stagingRing_->size`. Absolute rather than wrapped because
  // "does this allocation collide with something still in flight" is a
  // comparison the wrapped form cannot express (the old code just wrapped to 0
  // and hoped).
  uint64_t stagingHead_ = 0;       // next free
  uint64_t stagingTail_ = 0;       // reclaim floor: everything below has retired
  uint64_t stagingSubmitted_ = 0;  // high covered by the most recent submit
  // The ring high each OPEN command buffer's flush consumed, consumed in turn
  // by the submit of that command buffer. Keyed by command buffer rather than
  // kept as one scalar because a flush and its submit are separated by the
  // caller's whole recording, during which more QueueWrites can arrive whose
  // copies belong to a LATER command buffer.
  struct FlushMark {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint64_t high = 0;
  };
  std::vector<FlushMark> flushMarks_;
  // The uploads recorded into the command buffer that most recently consumed a
  // flush, held until that buffer is SUBMITTED (dropped then) or ABANDONED
  // (re-queued then). See AbandonCommands.
  std::vector<Pending> heldFlush_;
  VkCommandBuffer heldFlushCmd_ = VK_NULL_HANDLE;
  uint64_t flushesRecovered_ = 0;
  uint64_t stagingStalls_ = 0;
  uint64_t stagingFallbacks_ = 0;

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
  // Keys some thread is compiling RIGHT NOW. A second asker for the same key
  // waits on shaderCv_ instead of compiling it a second time; see
  // GetShaderModule for why the compile itself is NOT under the lock.
  std::unordered_set<std::string> moduleInFlight_;
  std::mutex shaderMutex_;               // guards moduleCache_ + moduleInFlight_
  std::condition_variable shaderCv_;     // a key left moduleInFlight_
  VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
  std::string pipelineCachePath_;
  std::vector<VkDescriptorSetLayout> setLayouts_;
  std::vector<VkPipelineLayout> pipeLayouts_;
  // Pipelines keep their ENGINE LABEL. They used to be bare handles — both
  // creates took a `const char* /*label*/` and dropped it — which made a
  // per-pipeline readout impossible to attribute: `--shader-stats` would have
  // printed 30 anonymous rows. The label costs one std::string per pipeline,
  // built once at startup.
  struct PipelineRec {
    VkPipeline pipe = VK_NULL_HANDLE;
    std::string label;
    bool compute = false;
  };
  std::vector<PipelineRec> pipelines_;
  // Guards pipelines_ only. Separate from shaderMutex_ so a create that is
  // already inside the driver does not block the next thread's Tint lookup.
  // Mutable so the const readers (CollectPipelineStats) can take it too.
  mutable std::mutex pipelineMutex_;
  bool captureStats_ = false;

  std::vector<std::string> validationMsgs_;
  // DebugCallback runs on whatever thread provoked the message, and since
  // pipeline creation is threaded that is no longer always the main one.
  // Guards the vector's mutation and the scope pop; ValidationMessages()
  // hands out a reference and is for after-the-join readers only.
  std::mutex validationMutex_;
  bool validationScopeOpen_ = false;
  size_t validationScopeMark_ = 0;

  friend VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT*, void*);
};

// Vulkan's HARD cap on one vkCmdUpdateBuffer. Two engine buffers sit EXACTLY on
// it — kMaxDebugBoxes * sizeof(DebugBox) and kMaterialSlots *
// sizeof(MicroBrickGpu) are both 65536 — so each is one constant bump away from
// becoming an illegal update. The static_asserts live in rhi_vulkan.cpp next to
// the classification rule, so a bump fails the BUILD rather than producing a
// validation error at runtime.
inline constexpr uint64_t kVkUpdateBufferLimit = 65536;

// THE CLASS A POLICY THRESHOLD, which is a different question from the legality
// limit above and used to be conflated with it.
//
// vkCmdUpdateBuffer captures its payload INTO THE COMMAND BUFFER at record
// time. That is the right trade for a UBO or a dirty flag and the wrong one for
// a 16 KiB voxel page: a revisited window-shift plane refills 1,024 slots
// (Stream::FillSlots' store-hit branch) and at the old 65536-byte threshold
// every one of those pages was a heap-allocated payload copy plus 16 KiB
// memcpied into the command stream — 16 MiB of inline command-buffer data per
// shift, for a command the spec describes as being for small updates.
// docs/vulkan_barrier_graph.md §4.1 already LISTED "streaming's per-slot 16 KiB
// voxels writes" under Class B; only the implementation disagreed.
//
// 4096 keeps the genuinely small writes (tickUBO, renderUBO, farUBO, the 4-byte
// dirty/occupancy flags, genList, the page-table entries) on the cheap inline
// path and sends everything above it through the staging ring. It is still ONE
// size-derived rule with no exceptions, which is the property the barrier
// document cares about.
inline constexpr uint64_t kClassAMaxBytes = 4096;
static_assert(kClassAMaxBytes <= kVkUpdateBufferLimit,
              "the Class A policy threshold cannot exceed vkCmdUpdateBuffer's "
              "own limit");

}  // namespace vk
