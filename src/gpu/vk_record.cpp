// vk_record.cpp — the last-access tracker and the table walk it drives.
//
// Read vk_record.h's header first, then docs/vulkan_barrier_graph.md §3. This
// file is the implementation of §3.3's algorithm verbatim; where it departs
// from the document, the departure is commented and the document is updated in
// the same commit (CLAUDE.md's rule about docs that contradict code).

#include "gpu/vk_record.h"

#include <cstdio>

#include "sim/world.h"  // kExplosionWg, kNumChunks (pass::kPassStride is in pass_table.h)

namespace vk {
namespace {

// ---- barrier_graph §3.2: Acc -> (stage, access) ---------------------------
//
// The shader-stage domain is COMPUTE_SHADER throughout: every table this phase
// records is a compute table. §3.2 notes that a render table would map
// StorageRead/Uniform to VERTEX|FRAGMENT instead, which is why the mapping is a
// property of the table rather than of the enum — phase 4 adds the parameter.

struct Scope {
  VkPipelineStageFlags2 stage;
  VkAccessFlags2 access;
  bool write;
  bool read;
};

Scope Map(pass::Acc a) {
  using A = pass::Acc;
  switch (a) {
    case A::StorageRead:
      return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false, true};
    case A::StorageWrite:
      return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true, false};
    case A::StorageRW:
    // StorageAtomicRMW is READ|WRITE, NOT something weaker: Vulkan has no
    // atomic-specific access flag, and two consecutive atomic-only passes on
    // one buffer still need a real WAW barrier. Atomics guarantee
    // per-operation atomicity, never visibility of one dispatch's results to
    // the next (barrier_graph §3.2, and §7.8 for the farVox/farOcc case where
    // the "they're both atomic" intuition is specifically wrong).
    case A::StorageAtomicRMW:
      return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              true, true};
    case A::Uniform:
      return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT,
              false, true};
    case A::IndirectRead:
      // DRAW_INDIRECT is the correct stage for vkCmdDispatchIndirect's argument
      // fetch as well as vkCmdDrawIndirect's — the name is historical.
      return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
              VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, false, true};
    case A::TransferRead:
      return {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false, true};
    case A::TransferWrite:
      // ALL_TRANSFER rather than COPY: a Fill row is vkCmdFillBuffer, whose
      // stage is CLEAR, and a Copy row is COPY. One flag that covers both is
      // correct and avoids a per-kind special case that would have to be kept
      // in step with the Kind enum.
      return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
              true, false};
  }
  return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, false, true};
}

constexpr VkAccessFlags2 kWriteMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                      VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                      VK_ACCESS_2_MEMORY_WRITE_BIT;
constexpr VkAccessFlags2 kReadMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                     VK_ACCESS_2_UNIFORM_READ_BIT |
                                     VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                     VK_ACCESS_2_TRANSFER_READ_BIT |
                                     VK_ACCESS_2_MEMORY_READ_BIT;

}  // namespace

bool Recorder::CondHolds(pass::Cond c, const RecordCtx& cx) {
  switch (c) {
    case pass::Cond::Always:    return true;
    case pass::Cond::Ops:       return cx.opsCount > 0;
    case pass::Cond::Cells:     return cx.cellCount > 0;
    case pass::Cond::Exp:       return cx.expCount > 0;
    case pass::Cond::Spawn:     return cx.spawnCount > 0;
    case pass::Cond::Particles: return cx.particlesActive;
    case pass::Cond::Hash:      return cx.hashEnable;
    case pass::Cond::DirtyTick: return !cx.hashEnable;
    case pass::Cond::GenCount:  return cx.genCount > 0;
    case pass::Cond::DenseWorldgen: return cx.denseWorldgen;
    case pass::Cond::FarCount:  return cx.farCount > 0;
    case pass::Cond::FluidSpawn: return cx.fluidSpawnCount > 0;
    case pass::Cond::WindWake:  return cx.windWakeCount > 0;
    case pass::Cond::CaActive:  return cx.caActive;
    case pass::Cond::VizActive:  return cx.vizActive;
    case pass::Cond::WaterBody:  return cx.waterChunkCount > 0;
    case pass::Cond::WaterDrain: return cx.waterDrainBodies > 0;
  }
  return false;
}

uint32_t Recorder::Extent(uint32_t v, const RecordCtx& cx) {
  if (v < (uint32_t)pass::DispatchSel::kDynBase) return v;
  switch ((pass::DispatchSel)v) {
    case pass::DispatchSel::Ops:      return 4 * cx.opsCount;
    case pass::DispatchSel::Cells:    return (cx.cellCount + 63) / 64;
    case pass::DispatchSel::Exp:      return kExplosionWg * cx.expCount;
    case pass::DispatchSel::ExpWg:    return kExplosionWg;
    case pass::DispatchSel::Spawn:    return (cx.spawnCount + 63) / 64;
    case pass::DispatchSel::Chunks:   return kNumChunks;
    case pass::DispatchSel::Chunks64: return kNumChunks / 64;
    case pass::DispatchSel::GenCount: return cx.genCount;
    case pass::DispatchSel::FarCount: return cx.farCount;
    case pass::DispatchSel::WindWakeSel: return (cx.windWakeCount + 63) / 64;
    case pass::DispatchSel::FluidSpawnSel: return (cx.fluidSpawnCount + 63) / 64;
    // One WORKGROUP per listed chunk (the reduce and the shave walk a
    // chunk's columns); one THREAD per listed chunk for the quiescence probe.
    case pass::DispatchSel::WaterChunks:   return cx.waterChunkCount;
    case pass::DispatchSel::WaterChunks64: return (cx.waterChunkCount + 63) / 64;
    case pass::DispatchSel::WaterDrainSel:
      return (cx.waterDrainBodies * kWaterDrainOpsPerBody + 63) / 64;
    default:                          return v;
  }
}

// ---------------------------------------------------------------------------
// barrier_graph §3.4: every command buffer opens with one global memory
// barrier. Nothing in Vulkan makes memory written by submit N visible to submit
// N+1 — submission order gives execution ordering for the implicit-ordering
// guarantee, but availability and visibility still need a barrier. This one
// barrier is what makes "the tracker resets per command buffer" a sound
// statement rather than an assumption, and it removes the whole class of
// cross-submit reasoning errors: the page flip, the streaming submits, the
// eviction copies, drawArgs read a frame later.
// ---------------------------------------------------------------------------
void Recorder::Begin(VkCommandBuffer cmd) {
  cmd_ = cmd;
  for (auto& s : state_) s = BufState{};
  extra_.clear();
  pending_.clear();
  hostWritten_.clear();
  imgState_.clear();
  pendingImg_.clear();
  renderOpen_ = false;

  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.memoryBarrierCount = 1;
  di.pMemoryBarriers = &mb;
  be_.Fns().CmdPipelineBarrier2(cmd_, &di);
  stats_.barrierCalls++;
  stats_.globalBarriers++;
}

void Recorder::Sledgehammer() {
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.memoryBarrierCount = 1;
  di.pMemoryBarriers = &mb;
  be_.Fns().CmdPipelineBarrier2(cmd_, &di);
  stats_.barrierCalls++;
  stats_.globalBarriers++;
}

// ---------------------------------------------------------------------------
// barrier_graph §3.3, the algorithm. One buffer, one use.
//
// Three properties a reviewer should check, all visible below:
//   - WAR needs no separate case: a write folds readStagesSince/readAccessSince
//     into its own barrier's SOURCE scope. Passing a read access in
//     srcAccessMask is legal and harmless (the implementation may skip the
//     unnecessary availability operation).
//   - Read-after-read emits nothing: lastWrite is unchanged and both reads are
//     already visible.
//   - The FIRST touch of a buffer in a recording emits nothing if it was never
//     written here. That is safe ONLY because of §3.4's head barrier; it does
//     not mean buffers start clean.
// ---------------------------------------------------------------------------
void Recorder::TouchBuffer(pass::Buf id, pass::Acc acc, Buffer* explicitBuf) {
  // The off-table paths pass the concrete buffer alongside the id (phase 4a),
  // so the tracker works even in an encoder whose bindings were never set (the
  // eviction command buffer). A table row leaves explicitBuf null and resolves
  // through the bindings, exactly as before.
  Buffer* buf = explicitBuf ? explicitBuf : bind_.buffers[(int)id];
  if (!buf || !buf->buf) return;  // not bound in this configuration
  TouchState(state_[(int)id], buf->buf, acc);
}

// §3.3 against a pointer-keyed side state, for buffers with no pass::Buf id.
void Recorder::TouchExtra(Buffer* buf, pass::Acc acc) {
  if (!buf || !buf->buf) return;
  for (auto& e : extra_) {
    if (e.first == buf) {
      TouchState(e.second, buf->buf, acc);
      return;
    }
  }
  extra_.push_back({buf, BufState{}});
  TouchState(extra_.back().second, buf->buf, acc);
}

void Recorder::TouchState(BufState& s, VkBuffer buf, pass::Acc acc) {
  const Scope u = Map(acc);

  if (u.write) {
    // WAW against the last writer AND WAR against every reader since it. One
    // barrier expresses both, because its source scope covers write | reads.
    if (s.lastWriteStage != 0 || s.readStagesSince != 0) {
      VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
      b.srcStageMask = s.lastWriteStage | s.readStagesSince;
      b.srcAccessMask = s.lastWriteAccess | s.readAccessSince;
      b.dstStageMask = u.stage;
      b.dstAccessMask = u.access;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.buffer = buf;
      b.offset = 0;
      b.size = VK_WHOLE_SIZE;  // §2.2: whole-buffer granularity, deliberately
      pending_.push_back(b);
    }
    s.lastWriteStage = u.stage;
    s.lastWriteAccess = u.access & kWriteMask;
    s.readStagesSince = 0;
    s.readAccessSince = 0;
    // StorageRW / AtomicRMW also read. The read is already folded into this
    // barrier's dstAccess (u.access carries READ|WRITE); recording it here is
    // what makes a LATER writer see it and emit the WAR.
    if (u.read) {
      s.readStagesSince = u.stage;
      s.readAccessSince = u.access & kReadMask;
    }
  } else {
    // RAW against the last writer only.
    if (s.lastWriteStage != 0) {
      VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
      b.srcStageMask = s.lastWriteStage;
      b.srcAccessMask = s.lastWriteAccess;
      b.dstStageMask = u.stage;
      b.dstAccessMask = u.access;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.buffer = buf;
      b.offset = 0;
      b.size = VK_WHOLE_SIZE;
      pending_.push_back(b);
    }
    s.readStagesSince |= u.stage;
    s.readAccessSince |= u.access;
  }
}

// barrier_graph §3.5: all barriers for ONE row go in ONE vkCmdPipelineBarrier2.
// Merging further (deferring across rows) is not done — emitting immediately
// before the row that needs them is both the tightest scope and the easiest to
// read in a capture.
void Recorder::FlushPending(bool global) {
  if (pending_.empty()) return;
  if (global) {
    // barrier_graph §3.6, form (B): the CA loop uses ONE VkMemoryBarrier2 over
    // the compute-storage access domain rather than N buffer barriers.
    //
    // (B) is stronger than (A) WITHIN that domain — it covers every buffer,
    // including a binding a future edit adds to sim_step.wgsl without updating
    // the table — but it is NOT a superset of (A) in general: it does not cover
    // INDIRECT_COMMAND_READ at DRAW_INDIRECT, and the CA row consumes
    // dispatchArgs indirectly on every iteration.
    //
    // Why the loop is sound anyway: NOTHING writes dispatchArgs inside the
    // repeat span, so the tracker emits the TRANSFER_WRITE -> INDIRECT_COMMAND_READ
    // barrier once, ahead of iteration 0, and correctly nothing for 1..53. That
    // soundness comes from the TRACKER, not from this barrier — which is why
    // the pending list is still computed above and merely collapsed here rather
    // than skipped. If a future row ever wrote dispatchArgs between iterations,
    // the tracker would have produced a buffer barrier for it and this collapse
    // would silently drop it; the assertion below is the honest form of the
    // guarantee §3.6 asks the checker to enforce.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

    // The one access the global form does NOT cover. If a pending barrier
    // carries it, collapsing would drop a real hazard, so widen instead of
    // silently losing it.
    VkPipelineStageFlags2 extraSrcStage = 0, extraDstStage = 0;
    VkAccessFlags2 extraSrcAcc = 0, extraDstAcc = 0;
    for (const auto& b : pending_) {
      const VkAccessFlags2 outside =
          ~(VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
      if ((b.srcAccessMask & outside) || (b.dstAccessMask & outside)) {
        extraSrcStage |= b.srcStageMask;
        extraDstStage |= b.dstStageMask;
        extraSrcAcc |= b.srcAccessMask;
        extraDstAcc |= b.dstAccessMask;
      }
    }
    mb.srcStageMask |= extraSrcStage;
    mb.dstStageMask |= extraDstStage;
    mb.srcAccessMask |= extraSrcAcc;
    mb.dstAccessMask |= extraDstAcc;

    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    be_.Fns().CmdPipelineBarrier2(cmd_, &di);
    stats_.barrierCalls++;
    stats_.globalBarriers++;
    pending_.clear();
    return;
  }

  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.bufferMemoryBarrierCount = (uint32_t)pending_.size();
  di.pBufferMemoryBarriers = pending_.data();
  be_.Fns().CmdPipelineBarrier2(cmd_, &di);
  stats_.barrierCalls++;
  stats_.bufferBarriers += (uint32_t)pending_.size();
  pending_.clear();
}

void Recorder::ApplyUses(const pass::Use* uses, int count, bool global) {
  if (mode_ == BarrierMode::Sledgehammer) {
    // The oracle mode still walks the tracker — the state must stay correct so
    // that switching back to Precise mid-run would be meaningful — but the
    // derived barriers are DISCARDED and replaced by a full one. That is what
    // makes an A/B a true A/B: identical recording, different barriers.
    for (int i = 0; i < count; i++) TouchBuffer(uses[i].buf, uses[i].acc);
    pending_.clear();
    Sledgehammer();
    return;
  }
  for (int i = 0; i < count; i++) TouchBuffer(uses[i].buf, uses[i].acc);
  FlushPending(global);
}

void Recorder::DeclareUse(Buffer* b, pass::Acc acc) {
  if (!b || !b->buf) return;
  bool table = false;
  for (int i = 0; i < (int)pass::Buf::kCount; i++)
    if (bind_.buffers[i] == b) {
      TouchBuffer((pass::Buf)i, acc);
      table = true;
    }
  if (!table) TouchExtra(b, acc);
  if (mode_ == BarrierMode::Sledgehammer) {
    pending_.clear();
    Sledgehammer();
  } else {
    FlushPending(/*global=*/false);
  }
  if (b->mapped && Map(acc).write) hostWritten_.push_back(b);
}

void Recorder::SetTimer(VkQueryPool pool,
                        std::function<bool(const char*, uint32_t&, uint32_t&)> alloc) {
  timerPool_ = pool;
  timerAlloc_ = std::move(alloc);
}

// --measure only. Begin latches at TOP_OF_PIPE (waits for nothing), end at
// ALL_COMMANDS (all previous work complete) — the same span Dawn's
// beginning/end-of-pass timestamp writes cover. The sim's recording is
// serialized by the generated barriers anyway, so per-group spans do not
// overlap and the numbers compare directly against the Dawn baseline.
void Recorder::TimerOpen(const char* group) {
  if (timerPool_ == VK_NULL_HANDLE || !timerAlloc_ || !group) return;
  uint32_t b = 0, e = 0;
  if (!timerAlloc_(group, b, e)) return;  // pool full: untimed, like BeginPass
  be_.Fns().CmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timerPool_, b);
  timerGroup_ = group;
  timerEndIdx_ = e;
}

void Recorder::TimerClose() {
  if (timerGroup_ == nullptr) return;
  if (timerEndIdx_ != UINT32_MAX)
    be_.Fns().CmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timerPool_,
                                 timerEndIdx_);
  timerGroup_ = nullptr;
  timerEndIdx_ = UINT32_MAX;
}

void Recorder::RecordTable(pass::Table which, const RecordCtx& cx) {
  const vkl::DeviceFns& f = be_.Fns();

  for (int i = 0; i < pass::kRowCount; i++) {
    const pass::Row& r = pass::kRows[i];
    if (r.table != which) continue;
    // barrier_graph §3.9/§7.5: a row whose condition is false is never visited,
    // so it never touches BufState, so the next visited row computes its
    // barrier against the last ACTUAL accessor. There is no "skipped pass"
    // concept here at all — which is precisely why it is safe, and why any
    // implementation that precomputed barriers per adjacent table-index pair
    // would be wrong.
    if (!CondHolds(r.cond, cx)) continue;
    stats_.rows++;

    if (r.kind == pass::Kind::Fill) {
      TimerClose();  // a Fill/Copy row ends the open Dawn pass; mirror it
      ApplyUses(r.uses, r.useCount, /*global=*/false);
      Buffer* b = bind_.buffers[(int)r.uses[0].buf];
      if (b && b->buf) f.CmdFillBuffer(cmd_, b->buf, 0, VK_WHOLE_SIZE, 0);
      stats_.fills++;
      continue;
    }

    if (r.kind == pass::Kind::Copy) {
      TimerClose();
      ApplyUses(r.uses, r.useCount, /*global=*/false);
      // Copy rows carry (srcOffset, dstOffset, size) in x/y/z and exactly two
      // uses: the transfer read, then the transfer write.
      Buffer* src = bind_.buffers[(int)r.uses[0].buf];
      Buffer* dst = bind_.buffers[(int)r.uses[1].buf];
      if (src && dst && src->buf && dst->buf) {
        VkBufferCopy region{};
        region.srcOffset = r.x;
        region.dstOffset = r.y;
        region.size = r.z;
        f.CmdCopyBuffer(cmd_, src->buf, dst->buf, 1, &region);
        if (dst->mapped) hostWritten_.push_back(dst);
      }
      stats_.copies++;
      continue;
    }

    // ---- compute / computeIndirect ----
    //
    // There is no "compute pass" in Vulkan (barrier_graph §1.2), so the row's
    // `group` label — which under Dawn decides ComputePassEncoder boundaries —
    // is purely a name here. Phase 4's PassTimer will hang timestamps off it.
    VkPipeline pipe = bind_.pipelines[(int)r.pipe];
    if (pipe == VK_NULL_HANDLE) continue;

    // --measure: the row's `group` label is exactly where Dawn opens/closes a
    // ComputePassEncoder, so the timestamp pair spans the same rows.
    if (timerPool_ != VK_NULL_HANDLE && r.group != timerGroup_) {
      TimerClose();
      TimerOpen(r.group);
    }

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t setCount = 0;
    switch (r.groups) {
      case pass::Groups::Sim:
        layout = bind_.simLayout;
        sets[0] = bind_.simSet;
        setCount = 1;
        break;
      case pass::Groups::SlimPart:
        layout = bind_.slimPartLayout;
        sets[0] = bind_.slimSet;
        sets[1] = bind_.particleSet;
        setCount = 2;
        break;
      case pass::Groups::SlimFar:
        layout = bind_.slimFarLayout;
        sets[0] = bind_.slimSet;
        sets[1] = bind_.farSet;
        setCount = 2;
        break;
      case pass::Groups::SlimFluid:
        layout = bind_.slimFluidLayout;
        sets[0] = bind_.slimSet;
        sets[1] = bind_.fluidSet;
        setCount = 2;
        break;
      case pass::Groups::SlimFluidSeam:
        layout = bind_.slimFluidSeamLayout;
        sets[0] = bind_.slimSet;
        sets[1] = bind_.fluidSeamSet;
        setCount = 2;
        break;
      default:
        break;
    }
    if (layout == VK_NULL_HANDLE || setCount == 0) continue;

    f.CmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);

    const bool caLoop = r.dyn == pass::Dyn::Ca;
    // NOTE: a SANDVOX_CA_REPEAT env knob once lived here, truncating the CA
    // row's 54 iterations so the per-dispatch floor could be read off a slope
    // (that is where ROADMAP_scale §3.2's 2.25 µs figure came from). It was
    // REMOVED on merge: it breaks the colour lattice and therefore the world
    // hash, and rule 1 is not something to leave a live switch for in the hot
    // record loop. Recover it from branch perf/ca-phase-skip if the floor ever
    // needs re-measuring, and delete it again afterwards.
    for (uint32_t k = 0; k < r.repeat; k++) {
      // The barrier goes BEFORE the dispatch, every iteration. For the CA row
      // that means 54 ApplyUses calls: the first emits the dispatchArgs
      // TRANSFER_WRITE->INDIRECT_COMMAND_READ plus the voxels/dirtyOut/support
      // edges against whatever wrote them in phase 1; iterations 1..53 emit the
      // inter-iteration barrier that IS the colour lattice (§3.6/§7.1). Those
      // 53 are not an optimisation target and must never be merged away.
      ApplyUses(r.uses, r.useCount, /*global=*/caLoop);

      // Dynamic offsets. GRP_SIM's layout has exactly one dynamic uniform
      // (passUBO at binding 5); DYN_CA feeds iteration k the slice at
      // k * kPassStride, which is where colorPhase and substep come from. The
      // 256 B stride is legal here — minUniformBufferOffsetAlignment is 64 on
      // this device (see --vk-info), and the alignment requirement is that the
      // DYNAMIC OFFSET be a multiple of it.
      uint32_t dynOff = 0;
      uint32_t dynCount = 0;
      if (r.groups == pass::Groups::Sim) {
        dynCount = 1;
        dynOff = (r.dyn == pass::Dyn::Ca) ? k * pass::kPassStride : 0;
      }
      f.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, setCount,
                              sets, dynCount, dynCount ? &dynOff : nullptr);

      if (r.kind == pass::Kind::ComputeIndirect) {
        Buffer* args;
        switch ((pass::DispatchSel)r.x) {
          case pass::DispatchSel::IndPDispatchArgs:
            args = bind_.buffers[(int)pass::Buf::PDispatchArgs];
            break;
          case pass::DispatchSel::IndFluidArgs:
            args = bind_.buffers[(int)pass::Buf::FluidDispatchArgs];
            break;
          case pass::DispatchSel::IndFluidPArgs:
            args = bind_.buffers[(int)pass::Buf::FluidPDispatchArgs];
            break;
          default:
            args = bind_.buffers[(int)pass::Buf::DispatchArgs];
            break;
        }
        if (args && args->buf) f.CmdDispatchIndirect(cmd_, args->buf, 0);
      } else {
        uint32_t x = Extent(r.x, cx), y = Extent(r.y, cx), z = Extent(r.z, cx);
        // A zero-extent dispatch is legal in Vulkan (it does nothing), and the
        // table can produce one — D_FARCOUNT with count 0 is guarded by the
        // condition, but D_OPS with opsCount 0 is not reachable for the same
        // reason. Guarding anyway costs nothing and keeps a future row that
        // forgets its condition from recording a no-op.
        if (x && y && z) f.CmdDispatch(cmd_, x, y, z);
      }
      stats_.dispatches++;
    }
  }
  TimerClose();
}

void Recorder::CopyToHost(Buffer* src, uint64_t srcOffset, Buffer* dst,
                          uint64_t dstOffset, uint64_t size) {
  if (!src || !dst || !src->buf || !dst->buf) return;
  // Off-table copies still go through the tracker: the source's last writer
  // must be ordered ahead of the transfer read, and the destination must be
  // registered for the Finish() host barrier. A src/dst that is a bound table
  // buffer is found by POINTER and uses its table state; one that is not (a
  // staging buffer, the timer resolve buffer) is tracked by pointer in the
  // side table, so ad-hoc transfer chains barrier correctly too. Either way
  // the scope is DERIVED — nothing here writes one by hand.
  {
    bool srcTable = false, dstTable = false;
    for (int i = 0; i < (int)pass::Buf::kCount; i++) {
      if (bind_.buffers[i] == src) {
        TouchBuffer((pass::Buf)i, pass::Acc::TransferRead);
        srcTable = true;
      }
      if (bind_.buffers[i] == dst) {
        TouchBuffer((pass::Buf)i, pass::Acc::TransferWrite);
        dstTable = true;
      }
    }
    if (!srcTable) TouchExtra(src, pass::Acc::TransferRead);
    if (!dstTable) TouchExtra(dst, pass::Acc::TransferWrite);
    if (mode_ == BarrierMode::Sledgehammer) {
      // Same A/B discipline as ApplyUses: the tracker state was updated above,
      // the derived barriers are discarded and replaced by a full one.
      pending_.clear();
      Sledgehammer();
    } else {
      FlushPending(/*global=*/false);
    }
  }
  VkBufferCopy region{};
  region.srcOffset = srcOffset;
  region.dstOffset = dstOffset;
  region.size = size;
  be_.Fns().CmdCopyBuffer(cmd_, src->buf, dst->buf, 1, &region);
  stats_.copies++;
  if (dst->mapped) hostWritten_.push_back(dst);
}

void Recorder::FillUntracked(Buffer* dst, uint64_t offset, uint64_t size) {
  if (!dst || !dst->buf) return;
  bool dstTable = false;
  for (int i = 0; i < (int)pass::Buf::kCount; i++) {
    if (bind_.buffers[i] == dst) {
      TouchBuffer((pass::Buf)i, pass::Acc::TransferWrite);
      dstTable = true;
    }
  }
  if (!dstTable) TouchExtra(dst, pass::Acc::TransferWrite);
  if (mode_ == BarrierMode::Sledgehammer) {
    pending_.clear();
    Sledgehammer();
  } else {
    FlushPending(/*global=*/false);
  }
  be_.Fns().CmdFillBuffer(cmd_, dst->buf, offset,
                          size == UINT64_MAX ? VK_WHOLE_SIZE : size, 0);
  stats_.fills++;
  if (dst->mapped) hostWritten_.push_back(dst);
}

// ---------------------------------------------------------------------------
// Off-table copies with a TRACKED source (phase 3c). See vk_record.h for why
// the readback and eviction copies are Uses rather than Rows.
//
// The two functions below are the whole mechanism. Neither writes a barrier
// scope: each declares what it touches as a `pass::Use` and lets §3.3 derive
// the barrier from the live tracker state, exactly as a row does.
// ---------------------------------------------------------------------------
void Recorder::CopyTracked(pass::Buf srcId, Buffer* src, uint64_t srcOffset, Buffer* dst,
                           uint64_t dstOffset, uint64_t size) {
  // The concrete buffer comes from the CALLER (phase 4a: seam call sites carry
  // both the id and the buffer), so this works in an encoder whose bindings
  // were never set — the eviction command buffer. When bindings ARE set they
  // must agree with the caller; the tracker state is keyed by the id either way.
  Buffer* s = src ? src : bind_.buffers[(int)srcId];
  if (!s || !s->buf || !dst || !dst->buf) return;
  TouchBuffer(srcId, pass::Acc::TransferRead, s);
  // The destination is deliberately NOT write-tracked here: every CopyTracked
  // destination is a readback slot or an eviction staging buffer whose copies
  // land in DISJOINT ranges and whose only reader is the HOST behind a fence +
  // the Finish() barrier — a per-copy WAW would order writes that cannot alias
  // (and did not exist in the 3c recording this reproduces). The generic
  // CopyToHost path, whose destinations can be re-read on the GPU, does track.
  if (mode_ == BarrierMode::Sledgehammer) {
    pending_.clear();
    Sledgehammer();
  } else {
    FlushPending(/*global=*/false);
  }

  VkBufferCopy region{};
  region.srcOffset = srcOffset;
  region.dstOffset = dstOffset;
  region.size = size;
  be_.Fns().CmdCopyBuffer(cmd_, s->buf, dst->buf, 1, &region);
  stats_.copies++;
  if (dst->mapped) hostWritten_.push_back(dst);
}

void Recorder::FillTracked(pass::Buf id, Buffer* dst) {
  Buffer* b = dst ? dst : bind_.buffers[(int)id];
  if (!b || !b->buf) return;
  // TransferWrite against a buffer the tracker has just seen a TransferRead on
  // produces the WAR barrier of §7.4 automatically. That is the entire point of
  // routing this through the tracker rather than calling CmdFillBuffer here.
  TouchBuffer(id, pass::Acc::TransferWrite, b);
  if (mode_ == BarrierMode::Sledgehammer) {
    pending_.clear();
    Sledgehammer();
  } else {
    FlushPending(/*global=*/false);
  }
  be_.Fns().CmdFillBuffer(cmd_, b->buf, 0, VK_WHOLE_SIZE, 0);
  stats_.fills++;
}

void Recorder::FillTrackedRange(pass::Buf id, Buffer* dst, uint64_t offset,
                                uint64_t size, uint32_t pattern) {
  Buffer* b = dst ? dst : bind_.buffers[(int)id];
  if (!b || !b->buf) return;
  TouchBuffer(id, pass::Acc::TransferWrite, b);
  if (mode_ == BarrierMode::Sledgehammer) {
    pending_.clear();
    Sledgehammer();
  } else {
    FlushPending(/*global=*/false);
  }
  be_.Fns().CmdFillBuffer(cmd_, b->buf, offset, size, pattern);
  stats_.fills++;
}

// ---------------------------------------------------------------------------
// Phase 4b: the render domain (barrier_graph §2.6/§3.2). See the block comment
// in vk_record.h above BeginRendering for the design; the invariant here is
// the same as everywhere else in this file — every scope is DERIVED from
// tracked state, none is written at a call site.
// ---------------------------------------------------------------------------

namespace {
// §3.2's render-domain read scope: what a draw can read of a buffer. One
// constant pair, because §2.6 establishes that draws are read-only over
// everything the tick wrote — a render-domain WRITE to a buffer does not exist
// in this engine.
constexpr VkPipelineStageFlags2 kRenderReadStages =
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
constexpr VkAccessFlags2 kRenderReadAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                             VK_ACCESS_2_UNIFORM_READ_BIT |
                                             VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
}  // namespace

void Recorder::FlushForRenderDomain() {
  auto flushOne = [&](BufState& s, VkBuffer buf) {
    if (s.lastWriteStage == 0) return;  // never written in this recording
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = s.lastWriteStage;
    b.srcAccessMask = s.lastWriteAccess;
    b.dstStageMask = kRenderReadStages;
    b.dstAccessMask = kRenderReadAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buf;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    pending_.push_back(b);
    // Record the read so a post-render writer in the same recording WARs.
    s.readStagesSince |= kRenderReadStages;
    s.readAccessSince |= kRenderReadAccess;
  };
  for (int i = 0; i < (int)pass::Buf::kCount; i++) {
    Buffer* b = bind_.buffers[i];
    if (b && b->buf) flushOne(state_[i], b->buf);
  }
  for (auto& e : extra_)
    if (e.first && e.first->buf) flushOne(e.second, e.first->buf);
  FlushPending(/*global=*/false);
}

void Recorder::TransitionImage(Image* im, VkImageLayout newLayout,
                               VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
  if (!im || im->img == VK_NULL_HANDLE) return;
  BufState* s = nullptr;
  for (auto& e : imgState_)
    if (e.first == im) s = &e.second;
  if (!s) {
    imgState_.push_back({im, BufState{}});
    s = &imgState_.back().second;
  }
  // Nothing to do only when the layout is already right AND nothing in THIS
  // recording has touched the image (prior submits are ordered by the §3.4
  // head barrier). A same-layout re-use within one recording still needs the
  // execution/memory dependency, e.g. two renders into one offscreen target.
  if (im->layout == newLayout && s->lastWriteStage == 0 && s->readStagesSince == 0)
    return;
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  // First touch this recording: src NONE/0 is correct — availability of prior
  // submits' writes came from the head barrier; this transition only needs the
  // layout change itself ordered before dst.
  b.srcStageMask = s->lastWriteStage | s->readStagesSince;
  b.srcAccessMask = s->lastWriteAccess | s->readAccessSince;
  b.dstStageMask = dstStage;
  b.dstAccessMask = dstAccess;
  b.oldLayout = im->layout;
  b.newLayout = newLayout;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im->img;
  b.subresourceRange = {im->aspect, 0, 1, 0, 1};
  pendingImg_.push_back(b);
  im->layout = newLayout;
  // Fold the destination use into the tracked state so the NEXT transition
  // derives its source from it (attachment write -> transfer read is exactly
  // this chain). Keeping read bits in a later src scope is legal and harmless.
  s->lastWriteStage = dstStage;
  s->lastWriteAccess = dstAccess;
  s->readStagesSince = 0;
  s->readAccessSince = 0;
}

void Recorder::FlushPendingImages() {
  if (pendingImg_.empty()) return;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = (uint32_t)pendingImg_.size();
  di.pImageMemoryBarriers = pendingImg_.data();
  be_.Fns().CmdPipelineBarrier2(cmd_, &di);
  stats_.barrierCalls++;
  stats_.imageBarriers += (uint32_t)pendingImg_.size();
  pendingImg_.clear();
}

void Recorder::BeginRendering(const RenderAttachments& att) {
  if (!att.color || att.color->img == VK_NULL_HANDLE || renderOpen_) return;

  // 1. Resolve every buffer hazard BEFORE the rendering scope opens — barriers
  //    are illegal inside it. Sledgehammer mode uses its full barrier instead,
  //    exactly as it replaces the derived barriers everywhere else.
  if (mode_ == BarrierMode::Sledgehammer) {
    pending_.clear();
    Sledgehammer();
  } else {
    FlushForRenderDomain();
  }

  // 2. Derived layout transitions for the attachments, batched into one call.
  TransitionImage(att.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  if (att.depth)
    TransitionImage(att.depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
  FlushPendingImages();

  // 3. The rendering scope itself.
  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = att.color->view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp =
      att.clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  for (int i = 0; i < 4; i++)
    color.clearValue.color.float32[i] = att.clearRGBA[i];

  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  if (att.depth) {
    depth.imageView = att.depth->view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp =
        att.clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil.depth = att.depthClear;  // reversed-Z: 0 = far
  }

  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {att.color->width, att.color->height}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &color;
  ri.pDepthAttachment = att.depth ? &depth : nullptr;
  be_.Fns().CmdBeginRendering(cmd_, &ri);

  // NEGATIVE-HEIGHT VIEWPORT (maintenance1, core 1.1): makes the viewport
  // transform — and therefore framebuffer-space geometry AND winding — match
  // WebGPU's Y-up NDC exactly. Paired with FRONT_FACE_COUNTER_CLOCKWISE in
  // CreateGraphicsPipeline; change one and the other is wrong.
  VkViewport vp{};
  vp.x = 0.0f;
  vp.y = (float)att.color->height;
  vp.width = (float)att.color->width;
  vp.height = -(float)att.color->height;
  vp.minDepth = 0.0f;
  vp.maxDepth = 1.0f;
  be_.Fns().CmdSetViewport(cmd_, 0, 1, &vp);
  VkRect2D sc{{0, 0}, {att.color->width, att.color->height}};
  be_.Fns().CmdSetScissor(cmd_, 0, 1, &sc);
  renderOpen_ = true;
}

void Recorder::EndRendering() {
  if (!renderOpen_) return;
  be_.Fns().CmdEndRendering(cmd_);
  renderOpen_ = false;
}

void Recorder::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                    uint32_t firstInstance) {
  if (!renderOpen_) return;
  be_.Fns().CmdDraw(cmd_, vertexCount, instanceCount, firstVertex, firstInstance);
  stats_.draws++;
}

void Recorder::DrawIndirect(Buffer* args, uint64_t offset) {
  if (!renderOpen_ || !args || !args->buf) return;
  // Note the read WITHOUT emitting: no barrier is legal here, and none is
  // needed — the visibility was established by BeginRendering's flush (same
  // recording) or the §3.4 head barrier (previous submits, §4.5's drawArgs
  // edge). Recording it makes a post-render writer of the args buffer WAR.
  auto note = [&](BufState& s) {
    s.readStagesSince |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    s.readAccessSince |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
  };
  bool table = false;
  for (int i = 0; i < (int)pass::Buf::kCount; i++)
    if (bind_.buffers[i] == args) {
      note(state_[i]);
      table = true;
    }
  if (!table) {
    for (auto& e : extra_)
      if (e.first == args) {
        note(e.second);
        table = true;
      }
    if (!table) {
      extra_.push_back({args, BufState{}});
      note(extra_.back().second);
    }
  }
  be_.Fns().CmdDrawIndirect(cmd_, args->buf, offset, 1, 0);
  stats_.draws++;
}

void Recorder::CopyImageToBuffer(Image* src, Buffer* dst, uint64_t dstOffset,
                                 uint32_t bytesPerRow, uint32_t w, uint32_t h) {
  if (!src || src->img == VK_NULL_HANDLE || !dst || !dst->buf || renderOpen_) return;

  // Destination hazard, same path as CopyToHost's dst half.
  bool dstTable = false;
  for (int i = 0; i < (int)pass::Buf::kCount; i++)
    if (bind_.buffers[i] == dst) {
      TouchBuffer((pass::Buf)i, pass::Acc::TransferWrite);
      dstTable = true;
    }
  if (!dstTable) TouchExtra(dst, pass::Acc::TransferWrite);
  if (mode_ == BarrierMode::Sledgehammer) {
    pending_.clear();
    Sledgehammer();
  } else {
    FlushPending(/*global=*/false);
  }

  // Source: derived transition from its tracked attachment write.
  TransitionImage(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  FlushPendingImages();

  // bufferRowLength is in TEXELS, not bytes — the one WebGPU/Vulkan unit
  // mismatch in this call. Only 4-byte color formats reach this path.
  const uint32_t texelBytes = 4;
  VkBufferImageCopy region{};
  region.bufferOffset = dstOffset;
  region.bufferRowLength = bytesPerRow / texelBytes;
  region.bufferImageHeight = h;
  region.imageSubresource = {src->aspect, 0, 0, 1};
  region.imageExtent = {w, h, 1};
  be_.Fns().CmdCopyImageToBuffer(cmd_, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 dst->buf, 1, &region);
  stats_.copies++;
  if (dst->mapped) hostWritten_.push_back(dst);
}

// ---------------------------------------------------------------------------
// barrier_graph §2.4 phase 7b: the host-visibility barrier is emitted at
// Finish() time, as the LAST command, after every writer of every host-visible
// buffer touched during the recording — NEVER at a fixed table index.
//
// The bug that rule exists to prevent: EncodeDirtyCopy is a separate function
// called AFTER EncodeReadbacks returns, so an index-anchored barrier would
// leave the dirtyFlags range the host reads behind the barrier meant to make it
// visible. Emitting here is index-independent by construction — a future path
// that appends another copy into a slot cannot get behind it.
// ---------------------------------------------------------------------------
void Recorder::Finish() {
  // Presentable (swapchain) images touched this recording go to PRESENT_SRC
  // as the last image operation — derived like every other transition (src is
  // the tracked attachment write). dst is ALL_COMMANDS with no access: the
  // present engine's visibility comes from the queue-submit semaphore, not
  // from an access mask.
  for (auto& e : imgState_) {
    if (e.first->presentable && e.first->layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
      TransitionImage(e.first, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
  }
  FlushPendingImages();

  if (hostWritten_.empty()) return;
  std::vector<VkBufferMemoryBarrier2> bs;
  bs.reserve(hostWritten_.size());
  for (Buffer* b : hostWritten_) {
    bool dup = false;
    for (const auto& e : bs)
      if (e.buffer == b->buf) dup = true;
    if (dup) continue;
    VkBufferMemoryBarrier2 m{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    m.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    m.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    m.buffer = b->buf;
    m.offset = 0;
    m.size = VK_WHOLE_SIZE;
    bs.push_back(m);
  }
  if (bs.empty()) return;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.bufferMemoryBarrierCount = (uint32_t)bs.size();
  di.pBufferMemoryBarriers = bs.data();
  be_.Fns().CmdPipelineBarrier2(cmd_, &di);
  stats_.barrierCalls++;
  stats_.bufferBarriers += (uint32_t)bs.size();
  hostWritten_.clear();
}

}  // namespace vk
