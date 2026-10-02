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
// HOW THE PASS TABLE REACHES THE TRACKER
// --------------------------------------
// The `uses` array is the whole point, so the recording call must carry it.
// Rather than widen the wgpu-shaped `rhi::` encoder surface with a
// `DeclareUses()` — an API where forgetting the call silently removes
// barriers — the recorder walks `pass::kRows` itself, through `RecordTable()`
// below. The row is therefore not a parameter that can be omitted: it is the
// loop variable. Every command the recorder can issue is reachable only from a
// row, and each takes a `Uses` span that the row supplies. Simulation hands
// across only the RESOLUTION (page-symbolic ids -> live objects) through the
// rhi_record.h bridge.
//
// (Until the Dawn removal on 2026-08-22 there were two walkers of the one
// table, Dawn's deriving its own barriers from bind-group usage; this is the
// one that survived.)
//
// Off-table work goes through the same tracker: the readback/eviction copies
// (CopyTracked*), the render domain (BeginRendering and friends, phase 4b),
// and the ad-hoc copies of the seam's generic encoder calls (CopyToHost).

#pragma once

#include <cstdint>
#include <functional>
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

// Everything the recorder needs to resolve a row's selectors: the ONE struct
// Simulation fills and passes through the bridge by reference
// (sim/pass_table.h says why there is only one now).
using RecordCtx = pass::RecordCtx;

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
  VkPipeline pipelines[(int)pass::Pipe::kPipeCount] = {};  // by (int)pass::Pipe
  VkPipelineLayout simLayout = VK_NULL_HANDLE;      // GRP_SIM: one set
  VkPipelineLayout slimPartLayout = VK_NULL_HANDLE; // GRP_SLIM_PART: slim + particle
  VkPipelineLayout slimFarLayout = VK_NULL_HANDLE;  // GRP_SLIM_FAR: slim + far
  VkPipelineLayout slimFluidLayout = VK_NULL_HANDLE;// GRP_SLIM_FLUID: slim + fluid
  VkPipelineLayout slimFluidSeamLayout = VK_NULL_HANDLE; // GRP_SLIM_FLUIDSEAM
  VkPipelineLayout slimGasLayout = VK_NULL_HANDLE;  // GRP_SLIM_GAS: slim + gas
  VkPipelineLayout shadowLayout = VK_NULL_HANDLE;   // GRP_SHADOW: one set
  VkDescriptorSet simSet = VK_NULL_HANDLE;          // simBG_[page]
  VkDescriptorSet slimSet = VK_NULL_HANDLE;         // simSlimBG_[page]
  VkDescriptorSet particleSet = VK_NULL_HANDLE;     // particleBG_[page]
  VkDescriptorSet farSet = VK_NULL_HANDLE;          // farBG_
  VkDescriptorSet fluidSet = VK_NULL_HANDLE;        // fluidBG_
  VkDescriptorSet fluidSeamSet = VK_NULL_HANDLE;    // fluidSeamBG_[page]
  VkDescriptorSet gasSet = VK_NULL_HANDLE;          // gasBG_[page]
  VkDescriptorSet shadowSet = VK_NULL_HANDLE;       // shadowBG_
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
  uint32_t draws = 0;           // phase 4b: draw calls inside rendering scopes
  uint32_t barrierCalls = 0;    // vkCmdPipelineBarrier2 invocations
  uint32_t bufferBarriers = 0;  // VkBufferMemoryBarrier2 entries across them
  uint32_t globalBarriers = 0;  // VkMemoryBarrier2 entries (CA loop + head + host)
  uint32_t imageBarriers = 0;   // VkImageMemoryBarrier2 entries (layout transitions)
};

// Phase 4b: one frame's attachments, resolved to backend images by the seam.
// `color == nullptr` is invalid; `depth == nullptr` means no depth attachment
// (rhi::RenderPassDesc::hasDepth == false).
struct RenderAttachments {
  Image* color = nullptr;
  bool clearColor = true;
  float clearRGBA[4] = {0, 0, 0, 1};
  Image* depth = nullptr;
  bool clearDepth = true;
  float depthClear = 0.0f;  // reversed-Z: far
};

// Records one command buffer's worth of table rows, generating every barrier.
//
// LIFETIME: one Recorder per command buffer. `Begin()` emits the head global
// barrier (§3.4) and resets the tracker; `Finish()` emits the host-visibility
// barrier (§2.4 phase 7b) as the LAST command. State does not persist across
// command buffers — that is what the head barrier makes sound.
class Recorder {
 public:
  Recorder(Backend& be, BarrierMode mode) : be_(be), mode_(mode) {}

  // Replace the bindings mid-recording. Exists for the seam encoder (rhi_vk.cpp),
  // which is constructed before Simulation resolves the page-symbolic ids and
  // receives them at the first RecordTable bridge call. Within one command
  // buffer every caller supplies the SAME resolution (the page cannot flip
  // mid-buffer — FlipPage runs after submit), so tracker state stays coherent.
  void SetBindings(const Bindings& b) { bind_ = b; }

  // Open a recording on `cmd`. Emits the head-of-command-buffer global memory
  // barrier that makes "the tracker resets per command buffer" sound
  // (barrier_graph §3.4): submission order gives execution ordering between
  // submits, never memory visibility.
  //
  // RESETS EVERY PER-RECORDING FIELD — tracker state, bindings (to none),
  // stats, the --measure timer, the compute bind cache — because the seam
  // REUSES recorders across encoders (rhi_vk.cpp VkrState::freeRecorders). A
  // field left out here is state leaking from one command buffer into the
  // next.
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

  // Record a copy that is not a table row and carries no pass::Buf id — the
  // blocking-readback path and the seam's generic CopyBufferToBuffer. Still
  // goes through the tracker: a src/dst that IS a bound table buffer is found
  // by pointer and its hazard derived; one that is not is tracked by POINTER in
  // the side table (see extra_ below), so ad-hoc transfer chains (query-resolve
  // -> staging, repeated writes into one staging buffer) barrier correctly too.
  // The destination is registered for the Finish() host barrier when mapped.
  void CopyToHost(Buffer* src, uint64_t srcOffset, Buffer* dst, uint64_t dstOffset,
                  uint64_t size);
  // Declare an access performed by a command the recorder does not itself
  // issue (vkCmdCopyQueryPoolResults writing the timer resolve buffer). The
  // hazard against/for it is still DERIVED by the tracker; call BEFORE
  // recording the command, like every row's ApplyUses.
  void DeclareUse(Buffer* b, pass::Acc acc);

  // --measure (phase 4a D4): write a GPU timestamp pair around each run of
  // rows sharing a `group` label, mirroring Dawn's per-ComputePassEncoder
  // timestamps. `alloc` hands out (begin, end) query indices for a named pass
  // and may refuse (pool full) — the pass then simply goes untimed, matching
  // PassTimer::BeginPass's fallback.
  //
  // `perRow` keys the pair on the row's own `name` instead of its `group`
  // (gpu/passtimer.h): `prep(mutate+explode+compact)` is one group spanning
  // three architecture components, and the Performance tab has to tell them
  // apart. Timestamps are pure observation — this changes which dispatches a
  // pair BRACKETS, never the dispatches, the barriers or the world hash.
  void SetTimer(VkQueryPool pool,
                std::function<bool(const char*, uint32_t&, uint32_t&)> alloc,
                bool perRow = false);

  // ---- off-table copies whose SOURCE is a tracked table buffer -------------
  //
  // WHY THESE ARE NOT TABLE ROWS (phase 3c's one schema judgement).
  //
  // A `pass::Row` encodes a Copy's (srcOffset, dstOffset, size) as the literal
  // constants x/y/z. That is exact for every copy in the tick table — the
  // indirect-args staging hops are always 12 bytes at offset 0 or 16. It cannot
  // express the readback copies: `World::EncodeReadbacks` issues up to 64 chunk
  // fetches at slot indices chosen at RUNTIME from a queue, plus 27 mirror
  // copies whose source offsets come from the live window origin, into a
  // destination slot picked from a 3-deep ring. Their COUNT and their OFFSETS
  // are tick data, not table data.
  //
  // Making the table able to say that would mean adding runtime-parameterised
  // offsets to the row schema — i.e. making a row a closure — which dissolves
  // the property that makes the table checkable at all (`check_pass_table.py`
  // reads the .def as static text). So the readback and eviction copies stay
  // off-table and express their hazards as `pass::Use` against the SAME
  // tracker, which is the mechanism `CopyToHost` established in 3b and which
  // barrier_graph §8 already sanctions ("if a hazard needs expressing, it is
  // expressed as a table row's `uses`" — a Use, not necessarily a Row).
  //
  // What is NOT given up: the hazard is still DERIVED. `CopyTracked` takes the
  // source as a `pass::Buf` id, so the source's last writer in this recording
  // is ordered ahead of the transfer read by the ordinary §3.3 path. Nothing
  // here writes a scope by hand.

  // Copy from a tracked table buffer into an untracked destination (a readback
  // slot, an eviction staging buffer). The source hazard is derived from the id
  // against the tracker; the destination is registered for the Finish() host
  // barrier when host-visible. `src` is the CONCRETE buffer (phase 4a: the seam
  // call sites carry both the id and the buffer, so this works in encoders
  // whose bindings were never set — the eviction command buffer).
  void CopyTracked(pass::Buf srcId, Buffer* src, uint64_t srcOffset, Buffer* dst,
                   uint64_t dstOffset, uint64_t size);
  // The batched form: `count` regions from one tracked source into one
  // destination, as ONE tracker touch and ONE vkCmdCopyBuffer. A loop of
  // CopyTracked calls over the same pair (a readback's chunk fetches, an
  // eviction batch) derives the identical barrier on its first call and none
  // after, so this records the same dependencies with one command instead of
  // `count`. Same destination rules as CopyTracked (disjoint regions,
  // host-read only).
  void CopyTrackedRegions(pass::Buf srcId, Buffer* src, Buffer* dst,
                          const rhi::CopyRegion* regions, size_t count);

  // rhi::CommandEncoder::CopyRenderWritten — see the contract there. `src` is
  // an EXTRAS buffer (never a table slot): its state is set to "last written
  // by the fragment stage" and the transfer read is then derived through
  // TouchExtra like any other access. The write itself is never declared at
  // draw time because a fragment atomic has no row to declare it from; this is
  // the one place the render pass's single write enters the tracker.
  void CopyRenderWritten(Buffer* src, uint64_t srcOffset, Buffer* dst,
                         uint64_t dstOffset, uint64_t size);

  // vkCmdFillBuffer over a tracked table buffer, off-table. This exists for
  // exactly one caller: `EncodeReadbacks`' support clear immediately after
  // copying support out (barrier_graph §7.4's T76 -> T77). That pair is a
  // genuine transfer-read -> transfer-write WAR on one buffer and is the case
  // most likely to be dismissed as "just two copies"; routing the fill through
  // the tracker is what makes the WAR fall out instead of being remembered.
  void FillTracked(pass::Buf id, Buffer* dst);
  // Ranged fill with a 32-bit pattern (page materialization, §5.4). Same
  // tracker path as the whole-buffer form; only the vkCmdFillBuffer arguments
  // differ. The tracker is range-agnostic, which is CONSERVATIVE and correct:
  // it treats a ranged fill as touching the whole buffer, so a barrier is
  // emitted where a finer tracker might elide one. Never the other way round.
  void FillTrackedRange(pass::Buf id, Buffer* dst, uint64_t offset,
                        uint64_t size, uint32_t pattern);

  // ---- the render domain (phase 4b, barrier_graph §2.6/§3.2) ---------------
  //
  // §3.2's stage-parameter extension: for the render table, StorageRead/Uniform
  // map to VERTEX|FRAGMENT instead of COMPUTE, and the attachments add
  // COLOR_ATTACHMENT/DEPTH accesses. Rather than re-declaring §2.6's read-only
  // rows one by one, the domain shift happens at the ONE point where it can
  // matter: draws are read-only over everything the tick wrote (§2.6), and
  // barriers are illegal inside a dynamic-rendering scope anyway, so every
  // buffer hazard must resolve BEFORE vkCmdBeginRendering. BeginRendering()
  // therefore flushes the tracker: every buffer written earlier in THIS
  // recording gets one derived barrier src = its last writer, dst = the whole
  // render-read domain (VERTEX|FRAGMENT shader reads + DRAW_INDIRECT's
  // INDIRECT_COMMAND_READ — §4.5's drawArgs edge). Writers in PREVIOUS submits
  // are covered by the §3.4 head barrier, exactly as they are for compute.
  // Nothing is written by hand; the source scopes come from the tracker.
  //
  // Image layout transitions are derived the same way: Image::layout is the
  // authoritative current layout (it persists across command buffers), and the
  // per-recording image side state supplies the source stage/access. No
  // hand-placed image barrier either.
  void BeginRendering(const RenderAttachments& att);
  void EndRendering();
  // rhi::RenderPass::SplitAfterFragmentWrite: end the open scope, declare
  // `written` as written by the fragment stage of that scope, emit its
  // fragment-write -> vertex/fragment read+write barrier, and reopen on the
  // attachments of the last BeginRendering with LOAD. No-op with no scope open.
  void SplitRenderingAfterFragmentWrite(Buffer* written);
  // Draws record inside the open rendering scope. No barrier can be (or needs
  // to be) emitted here; routing them through the recorder keeps the "every
  // command is reachable only through the recorder" property plus stats, and
  // DrawIndirect notes the args read so a later same-buffer writer WARs.
  void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
            uint32_t firstInstance);
  void DrawIndirect(Buffer* args, uint64_t offset);
  // The screenshot path: transition `src` to TRANSFER_SRC (derived from its
  // tracked attachment write) and copy into `dst` (registered for the Finish()
  // host barrier when mapped). `bytesPerRow` is converted to texels here.
  void CopyImageToBuffer(Image* src, Buffer* dst, uint64_t dstOffset,
                         uint32_t bytesPerRow, uint32_t w, uint32_t h);
  // The reverse, for CPU-drawn UI pictures: `src` (a staging buffer the queue
  // wrote) into `dst`, which is transitioned to TRANSFER_DST first and left
  // for Finish() to move to its sampled layout.
  void CopyBufferToImage(Buffer* src, uint64_t srcOffset, uint32_t bytesPerRow,
                         Image* dst, uint32_t w, uint32_t h);
  // The upscale path (render.renderScale): whole-image blit from `src` to
  // `dst`, both transitioned by the same derived path CopyImageToBuffer
  // uses (src from its tracked attachment write; dst from whatever it was
  // last — PRESENT_SRC for a swapchain image that was shown last frame,
  // UNDEFINED on its first use). NEAREST unless `linear`, because the world
  // is pixel art and a 2/3-scale frame sampled linearly is a blur, not a
  // smaller picture. Outside a rendering scope only, like every transfer.
  void BlitImage(Image* src, Image* dst, bool linear);

  const RecordStats& Stats() const { return stats_; }

  // ---- ASYNC COMPUTE (docs/PLAN_async_compute.md; rhi_vulkan.h's block) ----
  // The command buffer commands are recorded into NOW: after a join split it
  // is the TAIL, not the buffer Begin() was handed.
  VkCommandBuffer Cmd() const { return cmd_; }
  // MAIN recording made while async work is outstanding: `split` ends the
  // current buffer and returns a fresh one (the tail), or VK_NULL_HANDLE if it
  // cannot (the caller then joins at the head). Called at most once, at the
  // first access that conflicts with the async work (NoteAsync).
  void SetAsyncJoin(std::function<VkCommandBuffer()> split) { asyncJoin_ = std::move(split); }
  bool AsyncJoined() const { return asyncJoined_; }
  // Join now, whatever comes next (BeginRendering: a draw's descriptor reads
  // are not table uses, so a rendering scope is conservatively a conflict).
  void JoinAsync();
  // ASYNC recording: collect every touched buffer and whether it was written.
  void SetAsyncRecording(bool on) { asyncRecord_ = on; }
  const std::vector<Backend::AsyncTouch>& AsyncTouched() const { return asyncTouched_; }

 private:
  void NoteAsync(Buffer* buf, pass::Acc acc);
  std::function<VkCommandBuffer()> asyncJoin_;
  bool asyncJoined_ = false;
  bool asyncRecord_ = false;
  std::vector<Backend::AsyncTouch> asyncTouched_;

  // --- the algorithm, barrier_graph §3.3 --------------------------------
  // Consult the tracker with one row's uses, emit the derived barriers, and
  // update state. `global` takes the §3.6 form: one VkMemoryBarrier2 over the
  // compute-storage access domain instead of N buffer barriers.
  void ApplyUses(const pass::Use* uses, int count, bool global);
  // One buffer's worth of the above; appends to pending_ rather than emitting,
  // so a row's barriers batch into ONE vkCmdPipelineBarrier2 (§3.5).
  // `explicitBuf` overrides the bindings lookup (the off-table paths pass the
  // concrete buffer alongside the id; table rows leave it null).
  void TouchBuffer(pass::Buf id, pass::Acc acc, Buffer* explicitBuf = nullptr);
  // §3.3 against a POINTER-keyed side state (untracked buffers: staging,
  // query-resolve). Same algorithm, same pending_ batch.
  void TouchExtra(Buffer* buf, pass::Acc acc);
  // §3.3's core on one (buffer, state) pair; shared by the two Touch* above.
  void TouchState(BufState& s, VkBuffer buf, pass::Acc acc);
  void FlushPending(bool global);
  // The sledgehammer: a full ALL_COMMANDS/MEMORY_READ|WRITE barrier. Emitted
  // before every command in that mode, INSTEAD of the derived ones.
  void Sledgehammer();

  // Resolve a row's dispatch extent selector against this record's counts.
  static uint32_t Extent(uint32_t v, const RecordCtx& cx);
  static bool CondHolds(pass::Cond c, const RecordCtx& cx);

  // --measure: open/close the timestamp pair at group transitions.
  void TimerOpen(const char* group);
  void TimerClose();

  // The compute bind point's last vkCmdBindDescriptorSets, so a row whose
  // (layout, sets, dynamic offset) is identical to what is already bound skips
  // the call. Bound sets survive vkCmdBindPipeline between pipelines of one
  // layout, and nothing else in this engine binds on the COMPUTE point of a
  // recorder's command buffer, so equality here is exactly "already bound".
  // Reset by Begin(). The CA loop never hits it (each iteration's dynamic
  // offset differs); consecutive rows on one layout do.
  struct ComputeBind {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t setCount = 0;
    uint32_t dynCount = 0;
    uint32_t dynOff = 0;
  };
  ComputeBind lastBind_{};

  Backend& be_;
  Bindings bind_{};
  BarrierMode mode_ = BarrierMode::Precise;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;

  // Emit the render-domain flush described above BeginRendering: one derived
  // barrier per buffer with a live write in this recording, dst = the render
  // read domain. Records the reads into the tracker so later writers WAR.
  void FlushForRenderDomain();
  // Derived image layout transition: src from the per-recording image state
  // (0 = untouched this recording — prior submits are ordered by the §3.4 head
  // barrier), oldLayout from Image::layout. Appends to pendingImg_; batched by
  // the caller into one vkCmdPipelineBarrier2.
  void TransitionImage(Image* im, VkImageLayout newLayout,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
  void FlushPendingImages();

  BufState state_[(int)pass::Buf::kCount] = {};
  // Last-access state for buffers with no pass::Buf id (staging destinations,
  // the timer resolve buffer), keyed by pointer. Never more than a handful per
  // command buffer; linear scan is fine.
  std::vector<std::pair<Buffer*, BufState>> extra_;
  // Per-recording image access state (stage/access since the recording began);
  // the LAYOUT itself lives on vk::Image and persists across recordings.
  std::vector<std::pair<Image*, BufState>> imgState_;
  std::vector<VkImageMemoryBarrier2> pendingImg_;
  bool renderOpen_ = false;
  RenderAttachments lastAtt_{};  // for SplitRenderingAfterFragmentWrite
  std::vector<VkBufferMemoryBarrier2> pending_;
  // Host-visible buffers written during this recording, for the Finish()
  // barrier. Kept as a small vector rather than a set: it is never more than a
  // handful and order does not matter.
  std::vector<Buffer*> hostWritten_;
  RecordStats stats_{};

  // --measure timer state (null/absent in every non-measure run).
  VkQueryPool timerPool_ = VK_NULL_HANDLE;
  std::function<bool(const char*, uint32_t&, uint32_t&)> timerAlloc_;
  const char* timerGroup_ = nullptr;   // open pass label, null when closed
  uint32_t timerEndIdx_ = UINT32_MAX;  // end query index of the open pair
  bool timerPerRow_ = false;           // key on Row::name, not Row::group
};

}  // namespace vk
