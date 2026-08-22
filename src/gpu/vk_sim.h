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

  // Blocking read of the 4-byte world hash. Records the copy through the
  // recorder (so the hash buffer's last writer is ordered ahead of the transfer
  // read and the staging buffer gets its HOST_READ barrier), submits fenced,
  // waits, reads the map. The one sanctioned synchronous path.
  bool ReadHash(uint32_t& out, std::string& err);

  const RecordStats& LastStats() const { return lastStats_; }
  uint32_t Page() const { return page_; }

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
};

}  // namespace vk
