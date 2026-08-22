// vk_record.h — command recording with GENERATED barriers (port phase 3b).
//
// THE ONE IDEA IN THIS FILE
// -------------------------
// No barrier in this engine is ever written at a call site. `Recorder` owns a
// per-buffer last-access tracker (docs/vulkan_barrier_graph.md §3.1/§3.3) and
// every recorded command declares what it READS and WRITES. The tracker
// compares those declarations against the live state, emits
// `vkCmdPipelineBarrier2` with the derived src/dst scopes, and updates itself.
// A hazard that is not expressed as a declared use gets no barrier — which is
// why the declarations come from `src/sim/pass_table.def` rather than from
// anything typed next to a dispatch.
//
// HOW THE PASS TABLE REACHES THE TRACKER (the seam phase 3b adds)
// ---------------------------------------------------------------
// Under Dawn, `Simulation::RecordTable` walks the table and calls
// `enc.ClearBuffer` / `enc.CopyBufferToBuffer` / `pass.Dispatch*`. Dawn derives
// its barriers from bind-group usage, so the row's `uses` array is inert there
// — it exists only for the checker.
//
// Under Vulkan the `uses` array is the whole point, so the recording call must
// carry it. Rather than widen the wgpu-shaped `rhi::` encoder surface with a
// `DeclareUses()` that only one backend can honour — an API where forgetting
// the call silently removes barriers — this backend walks the SAME
// `pass::kRows` array itself, through `RecordTable()` below. The row is
// therefore not a parameter that can be omitted: it is the loop variable. Every
// command the recorder can issue is reachable only from a row, and each takes a
// `Uses` span that the row supplies.
//
// The two walkers (Dawn's in simulation.cpp, Vulkan's in vk_record.cpp) read
// one table and must agree about WHAT is recorded; the checker plus
// cross-backend hash equality is what proves they do. That duplication is
// deliberate and bounded: the alternative was making `rhi::` handles virtual,
// which would have restructured the Dawn backend that is currently the port's
// only hash oracle.
//
// WHAT IS NOT HERE
// ----------------
// Render passes (phase 4), the readback ring and streaming (phase 3c). This
// file records compute, copies and fills, and nothing else.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/rhi_vulkan.h"
#include "sim/pass_table.h"

namespace vk {

// How barriers are emitted. `--barriers=sledgehammer` is the A/B oracle of
// barrier_graph §6.2: maximally-ordered execution of the same total order, so
// every hazard real or imagined is covered.
//
// Read §6.2 before trusting a green sledgehammer run. The oracle is WEAK at
// detecting a missing barrier (this hardware serializes back-to-back identical
// dispatches anyway) and STRONG at exonerating the barrier graph — a
// sledgehammer run that still diverges from Dawn proves the bug is somewhere
// other than the barriers.
enum class BarrierMode {
  Precise,      // derived from the tracker; the shipping path
  Sledgehammer, // a full ALL_COMMANDS/MEMORY_READ|WRITE barrier before EVERY command
};

// Everything the recorder needs to resolve a row's selectors. Mirrors the
// anonymous RecordCtx in simulation.cpp — same fields, same meanings, because
// the conditions and dispatch extents are properties of the TABLE, not of a
// backend.
struct RecordCtx {
  uint32_t opsCount = 0;
  uint32_t cellCount = 0;
  uint32_t expCount = 0;
  uint32_t spawnCount = 0;
  uint32_t genCount = 0;
  uint32_t farCount = 0;
  bool hashEnable = false;
  bool particlesActive = false;
};

// The live GPU objects a table row resolves against. The recorder is handed one
// of these per recording; it never looks anything up globally.
//
// `buffers` is indexed by `(int)pass::Buf`, and the SYMBOLIC ids (DirtyIn,
// DirtyOut, ParticlesRead, ParticlesWrite) are resolved by the owner against
// the current page BEFORE handing the table over — the same resolution
// Simulation::PassBuffer does, in the same place in the flow (record time).
// barrier_graph §2.2, and §4.1's [NEW EDGE]: DirtyIn and DirtyOut must never
// resolve to the same buffer, which Bind() asserts.
struct Bindings {
  Buffer* buffers[(int)pass::Buf::kCount] = {};
  VkPipeline pipelines[32] = {};        // indexed by (int)pass::Pipe
  VkPipelineLayout simLayout = VK_NULL_HANDLE;      // GRP_SIM: one set
  VkPipelineLayout slimPartLayout = VK_NULL_HANDLE; // GRP_SLIM_PART: slim + particle
  VkPipelineLayout slimFarLayout = VK_NULL_HANDLE;  // GRP_SLIM_FAR: slim + far
  VkDescriptorSet simSet = VK_NULL_HANDLE;          // simBG_[page]
  VkDescriptorSet slimSet = VK_NULL_HANDLE;         // simSlimBG_[page]
  VkDescriptorSet particleSet = VK_NULL_HANDLE;     // particleBG_[page]
  VkDescriptorSet farSet = VK_NULL_HANDLE;          // farBG_
};

// Per-buffer last-access state. barrier_graph §3.1: the standard minimal
// tracker, which is exactly enough for a total order.
//
// `readStagesSince`/`readAccessSince` ACCUMULATE and are cleared by a write.
// That is what makes WAR fall out without a separate case — a write folds them
// into its own barrier's source scope.
struct BufState {
  VkPipelineStageFlags2 lastWriteStage = 0;  // 0 = never written in this recording
  VkAccessFlags2 lastWriteAccess = 0;
  VkPipelineStageFlags2 readStagesSince = 0;
  VkAccessFlags2 readAccessSince = 0;
};

// Recording statistics, for --vk-smoke's report and for eyeballing that the CA
// loop emitted the 53 inter-iteration barriers it must (barrier_graph §7.1).
struct RecordStats {
  uint32_t rows = 0;
  uint32_t dispatches = 0;
  uint32_t copies = 0;
  uint32_t fills = 0;
  uint32_t barrierCalls = 0;    // vkCmdPipelineBarrier2 invocations
  uint32_t bufferBarriers = 0;  // VkBufferMemoryBarrier2 entries across them
  uint32_t globalBarriers = 0;  // VkMemoryBarrier2 entries (CA loop + head + host)
};

// Records one command buffer's worth of table rows, generating every barrier.
//
// LIFETIME: one Recorder per command buffer. `Begin()` emits the head global
// barrier (§3.4) and resets the tracker; `Finish()` emits the host-visibility
// barrier (§2.4 phase 7b) as the LAST command. State does not persist across
// command buffers — that is what the head barrier makes sound.
class Recorder {
 public:
  Recorder(Backend& be, const Bindings& bind, BarrierMode mode)
      : be_(be), bind_(bind), mode_(mode) {}

  // Open a recording on `cmd`. Emits the head-of-command-buffer global memory
  // barrier that makes "the tracker resets per command buffer" sound
  // (barrier_graph §3.4): submission order gives execution ordering between
  // submits, never memory visibility.
  void Begin(VkCommandBuffer cmd);

  // Walk one table's rows in record order, recording each and emitting the
  // barriers its `uses` imply against the live tracker state. A row whose
  // condition is false is SKIPPED ENTIRELY — it records nothing and touches no
  // buffer's state (barrier_graph §3.9).
  void RecordTable(pass::Table which, const RecordCtx& cx);

  // Emit the host-visibility barrier for every host-visible buffer written
  // during this recording, as the last command. NEVER at a fixed table index:
  // a path that appends another copy into a readback slot after the table has
  // been walked would get behind an index-anchored barrier (§2.4 phase 7b).
  void Finish();

  // Record a copy that is not a table row — the blocking-readback path, whose
  // destination is a host-visible staging buffer. Still goes through the
  // tracker (so the source's last writer is ordered ahead of the read) and
  // still registers the destination for the Finish() host barrier.
  void CopyToHost(Buffer* src, uint64_t srcOffset, Buffer* dst, uint64_t dstOffset,
                  uint64_t size);

  const RecordStats& Stats() const { return stats_; }

 private:
  // --- the algorithm, barrier_graph §3.3 --------------------------------
  // Consult the tracker with one row's uses, emit the derived barriers, and
  // update state. `global` takes the §3.6 form: one VkMemoryBarrier2 over the
  // compute-storage access domain instead of N buffer barriers.
  void ApplyUses(const pass::Use* uses, int count, bool global);
  // One buffer's worth of the above; appends to pending_ rather than emitting,
  // so a row's barriers batch into ONE vkCmdPipelineBarrier2 (§3.5).
  void TouchBuffer(pass::Buf id, pass::Acc acc);
  void FlushPending(bool global);
  // The sledgehammer: a full ALL_COMMANDS/MEMORY_READ|WRITE barrier. Emitted
  // before every command in that mode, INSTEAD of the derived ones.
  void Sledgehammer();

  // Resolve a row's dispatch extent selector against this record's counts.
  static uint32_t Extent(uint32_t v, const RecordCtx& cx);
  static bool CondHolds(pass::Cond c, const RecordCtx& cx);

  Backend& be_;
  Bindings bind_;
  BarrierMode mode_ = BarrierMode::Precise;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;

  BufState state_[(int)pass::Buf::kCount] = {};
  std::vector<VkBufferMemoryBarrier2> pending_;
  // Host-visible buffers written during this recording, for the Finish()
  // barrier. Kept as a small vector rather than a set: it is never more than a
  // handful and order does not matter.
  std::vector<Buffer*> hostWritten_;
  RecordStats stats_{};
};

}  // namespace vk
