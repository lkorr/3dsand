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
  bool Init(bool lowPower, bool validation, bool syncValidation, std::string& err);
  void Shutdown();

  const Caps& GetCaps() const { return caps_; }
  VkDevice Device() const { return device_; }
  const vkl::DeviceFns& Fns() const { return dfn_; }

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
  // submitted and waited. Called once after all buffers exist.
  bool ZeroInitAll(std::string& err);

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
  };

  struct InFlight {
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint64_t stagingHigh = 0;  // ring high-water at submit; reclaimed on retire
  };

  bool PickPhysicalDevice(bool lowPower, std::string& err);
  void QueryCaps();
  bool CreateLogicalDevice(std::string& err);
  bool InitAllocator(std::string& err);
  VkFence AcquireFence(std::string& err);

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

  // Pending uploads, in ISSUE ORDER. Never sorted, never coalesced.
  std::vector<Pending> pending_;
  Buffer* stagingRing_ = nullptr;
  uint64_t stagingHead_ = 0;

  std::vector<InFlight> inFlight_;
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
