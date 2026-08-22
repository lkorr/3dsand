// vk_sim.h — the Vulkan mirror of the sim's GPU resources (port phase 3b).
//
// WHAT THIS IS, AND WHY IT IS A SECOND SET OF DECLARATIONS
// --------------------------------------------------------
// `World::Init` and `Simulation::Init` create buffers, layouts, bind groups and
// pipelines through the `rhi::` seam, whose handles are backed by Dawn
// (rhi_dawn.h defines every impl struct as a wgpu:: holder). Two backends
// cannot share those handle types without making every impl virtual — a
// refactor of the backend that is currently the port's only hash oracle, for no
// phase-3b benefit, since 3b is headless compute with no render path.
//
// So this file builds the SAME resources against `vk::Backend`, from the SAME
// descriptions, and hands them to `vk::Recorder` which walks the SAME
// `pass::kRows`. What is duplicated is the resource DESCRIPTION (17 bindings in
// this order, this layout, these pipelines); what is NOT duplicated — and what
// actually decides the world hash — is the table that says what runs and in
// what order.
//
// The duplication is bounded and it is checked: `--vk-smoke` compares world
// hashes against Dawn over worldgen plus 50 ticks, and any disagreement about a
// binding, a layout or a pipeline shows up as a hash mismatch rather than as
// something a reader has to notice. Phase 4/3c collapses this when the render
// path lands and the seam can carry both backends for real.
//
// SCOPE: headless compute. No render pipelines, no swapchain, no readback ring,
// no streaming. Blocking readbacks only, which is what a hash comparison needs
// and what CLAUDE.md sanctions for tests.

#pragma once

#include <string>
#include <vector>

#include "gpu/rhi_vulkan.h"
#include "gpu/vk_record.h"
#include "sim/materials.h"
#include "sim/pass_table.h"
#include "sim/world.h"

namespace vk {

// Every buffer the compute tables name, plus the staging buffer the blocking
// hash read lands in. Indexed for the recorder through `Resolve()`.
//
// This is deliberately NOT `World` — World owns rhi:: handles, a CPU mirror, a
// readback ring, a chunk cache and a streaming interface, none of which phase
// 3b has any use for. Taking only the buffers keeps it obvious that nothing
// here reaches back into the Dawn world.
struct SimResources {
  // ---- the world's compute buffers, in world.cpp's creation order ----
  Buffer* voxels = nullptr;
  Buffer* dirty[2] = {nullptr, nullptr};
  Buffer* dirtyList = nullptr;
  Buffer* argsStage = nullptr;
  Buffer* dispatchArgs = nullptr;
  Buffer* occupancy = nullptr;
  Buffer* support = nullptr;
  Buffer* hash = nullptr;
  Buffer* tickUBO = nullptr;
  Buffer* passUBO = nullptr;
  Buffer* opsBuf = nullptr;
  Buffer* renderUBO = nullptr;
  Buffer* pick = nullptr;
  Buffer* particles[2] = {nullptr, nullptr};
  Buffer* particleCounts = nullptr;
  Buffer* claim = nullptr;
  Buffer* pArgsStage = nullptr;
  Buffer* pDispatchArgs = nullptr;
  Buffer* drawArgs = nullptr;
  Buffer* expOps = nullptr;
  Buffer* expMask = nullptr;
  Buffer* cellOps = nullptr;
  Buffer* spawnOps = nullptr;
  Buffer* genList = nullptr;
  Buffer* farVox = nullptr;
  Buffer* farOcc = nullptr;
  Buffer* farList = nullptr;
  Buffer* farUBO = nullptr;
  // ---- the simulation's own ----
  Buffer* materials = nullptr;
  Buffer* reactions = nullptr;
  // ---- test-only ----
  Buffer* readback = nullptr;  // MapRead, for the blocking hash read
};

// The async readback ring (barrier_graph §4.2), Vulkan-side.
//
// WHAT THIS MUST REPRODUCE, EXACTLY
// ---------------------------------
// `World`'s ring is 3 slots, each a MapRead buffer holding one tick's worth of
// CPU-visible state at fixed offsets: the 3x3x3 mirror, the dirty flags, the
// per-chunk occupancy, the hash, the pick result, the particle counts, the
// support flags, and up to `kFetchPerTick` on-demand chunk fetches. The
// consumers of the resulting `WorldSnapshot` — streaming's evict filter, the
// player controller's `KindAt`, island detection, the selftest's active-chunk
// assertion — must not be able to tell the backends apart. So the OFFSETS, the
// SIZES and the population logic are the same, and the slot payload struct
// carries the same fields.
//
// WHAT CHANGES: `Slot::inFlight` was a bool cleared by a `MapAsync` callback
// that only ran inside `ProcessEvents()`. Here it is a borrowed reference to
// the FENCE of the submit that wrote the slot (§4.2: the slot does not own a
// fence — every submit gets one anyway, for staging-ring reclamation, so the
// readback borrows it). `PollReadbacks()` is `ProcessEvents()`' replacement and
// runs at the same point in the frame: it walks the slots, calls
// `vkGetFenceStatus`, and on VK_SUCCESS runs the body of what was the map
// callback. It NEVER blocks, matching `AllowProcessEvents` semantics.
struct ReadbackSlot {
  Buffer* buf = nullptr;
  VkFence fence = VK_NULL_HANDLE;  // BORROWED from the submit; never owned here
  bool inFlight = false;
  IVec3 base{};
  IVec3 origin{};
  uint32_t particleLivePage = 0;
  uint32_t tick = 0;
  std::vector<IVec3> fetchIds;
};

// Builds and drives the Vulkan-side sim. One instance owns a Backend.
class SimBackend {
 public:
  // Bring up the device, allocate every buffer, compile every compute shader,
  // build the descriptor sets and pipelines, and zero-init. `assetDir` is the
  // usual assets root; tuning must already be live (LoadShader bakes the tuning
  // prelude into every shader, so compiling before it is set compiles a source
  // string the engine never builds).
  bool Init(const std::string& assetDir, const std::vector<MaterialDef>& mats,
            const std::vector<ReactionGpu>& reactions, bool lowPower, bool validation,
            BarrierMode mode, std::string& err);
  void Shutdown();

  Backend& Be() { return be_; }
  const Caps& GetCaps() const { return be_.GetCaps(); }

  // ---- the recorded paths (one command buffer + one submit each) ----------
  //
  // Each mirrors the corresponding Simulation::Encode* + the uniform writes its
  // caller does, because the two are one unit: the tick's TickParams write and
  // the tick's dispatches are not independently meaningful.
  bool SubmitWorldgen(uint32_t seed, std::string& err);
  // One tick against a QUIET world: no ops, no explosions, no cells, no
  // spawns, no streaming, no far-field. That is exactly the scope --vk-smoke
  // exercises, and it still drives fills, compact, the indirect args copy, 54
  // indirect CA dispatches with dynamic offsets, compactNext, occupancyDirty,
  // farDown, and both hash-tick branches.
  bool SubmitTick(uint32_t tick, uint32_t seed, bool hashEnable, std::string& err);
  // Standalone whole-world rehash (PT_HASHONLY): the same pass the save/load
  // verification uses, and what --vk-smoke's worldgen stage compares.
  bool SubmitHashOnly(uint32_t seed, std::string& err);

  // One tick with the FULL input set: brush ops, explosions, exact-cell ops,
  // particle spawns, far-field fills, and the readback ring. This is the
  // per-tick driver `test/support.cpp`'s SubmitTick is, expressed against the
  // Vulkan resources — the same TickParams, the same conditions, the same order.
  //
  // `wantReadback` claims a ring slot and records the readback copies into the
  // SAME command buffer as the tick, exactly as Dawn does: the copies must see
  // this tick's results, and a separate submit would both cost a submit and
  // change which tick's state the snapshot describes.
  struct TickInputs {
    uint32_t tick = 0;
    uint32_t seed = 0;
    bool hashEnable = false;
    bool particlesActive = false;
    const BrushOp* ops = nullptr;
    uint32_t opsCount = 0;
    const ExplosionOp* exps = nullptr;
    uint32_t expCount = 0;
    const CellOp* cells = nullptr;
    uint32_t cellCount = 0;
    const ParticleSpawn* spawns = nullptr;
    uint32_t spawnCount = 0;
    uint32_t farCount = 0;
    bool wantReadback = false;
    IVec3 playerChunkBase{};
    IVec3 windowOrigin{};
  };
  bool SubmitTickFull(const TickInputs& in, std::string& err);

  // The day/night sleep handshake (`Simulation::EncodeWakeAll`). Under Dawn
  // this is a bare `queue.WriteBuffer` with no encoder at all, issued from
  // SubmitTick BEFORE the tick's encoder exists. Under the pending-upload queue
  // it is simply a QueueWrite that drains at the head of the tick's command
  // buffer, ahead of the first row — NO SPECIAL CASE, which is the point of the
  // queue model (barrier_graph §4.1). The write must land before `compact`
  // reads DirtyIn, and the flush ordering guarantees exactly that.
  void WakeAll();

  // `ctx.ProcessEvents()`' replacement, called at the same point in the frame.
  // Walks the 3 ring slots, polls each borrowed fence with vkGetFenceStatus,
  // and on VK_SUCCESS runs the body of what was Dawn's map callback. NEVER
  // blocks (barrier_graph §4.2).
  void PollReadbacks();
  const WorldSnapshot& Snap() const { return snap_; }

  // Blocking read of the 4-byte world hash. Records the copy through the
  // recorder (so the hash buffer's last writer is ordered ahead of the transfer
  // read and the staging buffer gets its HOST_READ barrier), submits fenced,
  // waits, reads the map. The one sanctioned synchronous path.
  bool ReadHash(uint32_t& out, std::string& err);
  // Blocking read of an arbitrary tracked buffer, for gates that assert on GPU
  // state directly (active chunks, particle counts). Same sanctioned path.
  bool ReadBufferBlocking(pass::Buf src, uint64_t offset, void* out, uint64_t size,
                          std::string& err);

  // ---- streaming (barrier_graph §4.3 / §4.7) ------------------------------
  //
  // The eviction copies. Records `voxels -> staging` for each named slot,
  // submits EAGERLY (which is what orders them ahead of the refill writes — see
  // the §4.3 note: FillSlots only enqueues), and returns a handle whose fence
  // the caller waits on to read the staging buffer back.
  //
  // `CompleteEvict` is `CompleteOldest`'s fence wait.
  struct EvictBatch {
    Buffer* staging = nullptr;
    VkFence fence = VK_NULL_HANDLE;  // retained; released by CompleteEvict
    uint32_t count = 0;
  };
  bool EvictSlots(const uint32_t* slots, uint32_t count, EvictBatch& out,
                  std::string& err);
  // Blocks on the batch's fence, hands back the mapped staging pointer, and
  // returns the staging buffer to the pool. `data` is valid until the next
  // EvictSlots reuses the buffer.
  bool CompleteEvict(EvictBatch& b, const void*& data, std::string& err);

  // A store-hit refill: the per-slot voxels/occupancy/dirty writes, through the
  // pending-upload queue. Deliberately SUBMITS NOTHING — reproducing the case
  // §4.1 calls out, where every slot hits the store and `FillSlots` issues no
  // submit at all, so the writes belong to whatever command buffer is recorded
  // next.
  void FillSlotFromStore(uint32_t slot, const uint32_t* voxels, uint32_t occWord);
  // The procgen refill: writes genList + tickUBO and submits PT_GENLIST.
  // tickUBO is written LAST-WRITE-WINS against the tick's own write (§4.1).
  bool FillSlotsByGen(const uint32_t* slots, uint32_t count, uint32_t seed,
                      IVec3 windowOrigin, std::string& err);

  // The save/load reset path (PT_LOADRESET): the caller has already written the
  // voxel and dirty buffers through the upload queue (worldio's sanctioned
  // MutationQueue bypass), and this rebuilds occupancy + clears transient state.
  bool SubmitLoadReset(uint32_t seed, std::string& err);
  // Upload a decoded chunk straight into the voxels buffer — worldio's bulk
  // restore. Goes through the pending-upload queue like every other write.
  void UploadChunk(uint32_t slot, const uint32_t* voxels);
  void UploadDirtyWord(uint32_t slot, uint32_t value);

  const RecordStats& LastStats() const { return lastStats_; }
  uint32_t Page() const { return page_; }
  Buffer* Buf(pass::Buf id) const;

 private:
  // Fill a Bindings for the CURRENT page: the symbolic ids (DirtyIn/DirtyOut,
  // ParticlesRead/ParticlesWrite) resolve here, at record time, exactly where
  // Simulation::PassBuffer resolves them.
  Bindings Resolve() const;
  bool BuildPipelines(const std::string& assetDir, std::string& err);
  bool BuildDescriptors(std::string& err);
  // Record + submit one table, with the head and host barriers around it.
  bool RunTable(pass::Table which, const RecordCtx& cx, std::string& err);

  Backend be_;
  SimResources res_{};
  BarrierMode mode_ = BarrierMode::Precise;
  uint32_t page_ = 0;

  VkDescriptorSetLayout simSetL_ = VK_NULL_HANDLE, slimSetL_ = VK_NULL_HANDLE,
                        partSetL_ = VK_NULL_HANDLE, farSetL_ = VK_NULL_HANDLE;
  VkPipelineLayout simPL_ = VK_NULL_HANDLE, simPL2_ = VK_NULL_HANDLE,
                   farPL_ = VK_NULL_HANDLE;
  VkDescriptorSet simSet_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDescriptorSet slimSet_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDescriptorSet partSet_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDescriptorSet farSet_ = VK_NULL_HANDLE;
  VkPipeline pipelines_[32] = {};  // indexed by (int)pass::Pipe
  RecordStats lastStats_{};

  // ---- the readback ring (barrier_graph §4.2) ----
  static constexpr int kSlots = 3;   // World::kSlots
  ReadbackSlot slots_[kSlots];
  int lastSlot_ = -1;
  WorldSnapshot snap_;
  IVec3 origin_{0, 0, 0};

  // Record the readback copies into an open recorder, claiming a slot.
  // Returns false when all three are in flight — the tick then simply skips its
  // copies, which is what Dawn does and is why the flag means what it claims.
  bool EncodeReadbacks(Recorder& rec, IVec3 playerChunkBase, uint32_t particleLivePage,
                       uint32_t tick);
  void PopulateSnapshot(ReadbackSlot& s);

  // ---- the eviction staging pool (barrier_graph §4.3) ----
  std::vector<Buffer*> stagingPool_;
};

}  // namespace vk
