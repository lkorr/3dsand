// rhi_record.h — the table-recording bridge between Simulation and the Vulkan
// backend's generated-barrier recorder (port phase 4a).
//
// THE CONSTRAINT THIS PRESERVES (phase 3b's seam decision, verbatim): barrier
// generation is NOT derived from wgpu-shaped encoder calls. The Vulkan recorder
// walks pass::kRows ITSELF — the row is the loop variable, not a parameter that
// can be omitted — so every command it can issue is reachable only from a row,
// and its `uses` cannot be forgotten. What phase 4a changes is only WHO OWNS
// THE RESOURCES: Simulation resolves the page-symbolic ids against its own
// rhi:: handles (the same PassBuffer/PassPipeline resolution the Dawn walk
// uses) and hands the result across this bridge; vk_sim.cpp's parallel copy of
// the resolution is deleted.
//
// This header is includable from src/sim (no Vulkan, no wgpu): it speaks only
// rhi:: handles and pass:: ids. The downcasts to the live Vulkan objects happen
// on the other side, in rhi_vk.cpp.

#pragma once

#include "gpu/rhi.h"
#include "sim/pass_table.h"

class PassTimer;

namespace rhi {

// The per-call counts and flags the row conditions and dispatch selectors
// resolve against. Mirrors the RecordCtx both walkers already use — the values
// are properties of the TABLE, not of a backend.
struct TableCtx {
  uint32_t opsCount = 0;
  uint32_t cellCount = 0;
  uint32_t expCount = 0;
  uint32_t spawnCount = 0;
  uint32_t genCount = 0;
  uint32_t farCount = 0;
  bool hashEnable = false;
  bool particlesActive = false;
};

// The live resources a table row resolves against, as SEAM handles. The
// symbolic ids (DirtyIn/DirtyOut, ParticlesRead/ParticlesWrite) must already be
// resolved for the current page — Simulation::PassBuffer does that, in the same
// place in the flow (record time) it always has.
struct TableBindings {
  Buffer buffers[(int)pass::Buf::kCount];
  ComputePipeline pipelines[32];  // indexed by (int)pass::Pipe
  PipelineLayout simLayout;       // GRP_SIM (simPL_)
  PipelineLayout slimPartLayout;  // GRP_SLIM_PART (simPL2_)
  PipelineLayout slimFarLayout;   // GRP_SLIM_FAR (farPL_)
  BindGroup simSet;               // simBG_[page]
  BindGroup slimSet;              // simSlimBG_[page]
  BindGroup particleSet;          // particleBG_[page]
  BindGroup farSet;               // farBG_
};

// Record one table through the encoder's generated-barrier recorder. VULKAN
// ONLY — Simulation::RecordTable calls this when device.Kind() is Vulkan and
// runs its own Dawn walk otherwise. `timer` is the --measure hook (may be
// null); when set, the recorder writes a GPU timestamp pair around each run of
// rows sharing a `group` label, mirroring Dawn's per-ComputePassEncoder
// timestamps so the two backends report comparable per-pass numbers.
void RecordTableVulkan(const CommandEncoder& enc, pass::Table which, const TableCtx& cx,
                       const TableBindings& tb, PassTimer* timer);

}  // namespace rhi
