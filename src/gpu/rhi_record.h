// rhi_record.h — the table-recording bridge between Simulation and the Vulkan
// backend's generated-barrier recorder (port phase 4a).
//
// THE CONSTRAINT THIS PRESERVES (phase 3b's seam decision, verbatim): barrier
// generation is NOT derived from the seam's wgpu-shaped encoder calls. The recorder
// walks pass::kRows ITSELF — the row is the loop variable, not a parameter that
// can be omitted — so every command it can issue is reachable only from a row,
// and its `uses` cannot be forgotten. What phase 4a changes is only WHO OWNS
// THE RESOURCES: Simulation resolves the page-symbolic ids against its own
// rhi:: handles (PassBuffer/PassPipeline) and hands the result across this
// bridge; vk_sim.cpp's parallel copy of the resolution is deleted.
//
// This header is includable from src/sim (no Vulkan headers): it speaks only
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
  uint32_t fluidCount = 0;       // MLS-MPM particles alive AFTER this tick's spawns
  uint32_t fluidSpawnCount = 0;  // MLS-MPM spawn ops this tick
  // Chunk slots this tick's wind primitives want dirty-marked
  // (docs/RESEARCH_wind.md §4.3). Zero on every tick of a world with no fan in
  // it, which is what skips the windWake row entirely.
  uint32_t windWakeCount = 0;
  bool hashEnable = false;
  bool particlesActive = false;
  // False under --residency paged (PLAN_page_table.md §3.5c).
  bool denseWorldgen = true;
  // False ONLY when the CPU can prove the dirty set is empty, dropping compact
  // + the args copy + all 54 CA iterations (ROADMAP_scale.md §3.4).
  bool caActive = true;
  bool vizActive = false;
};

// The live resources a table row resolves against, as SEAM handles. The
// symbolic ids (DirtyIn/DirtyOut, ParticlesRead/ParticlesWrite) must already be
// resolved for the current page — Simulation::PassBuffer does that, in the same
// place in the flow (record time) it always has.
struct TableBindings {
  Buffer buffers[(int)pass::Buf::kCount];
  ComputePipeline pipelines[64];  // indexed by (int)pass::Pipe
  PipelineLayout simLayout;       // GRP_SIM (simPL_)
  PipelineLayout slimPartLayout;  // GRP_SLIM_PART (simPL2_)
  PipelineLayout slimFarLayout;   // GRP_SLIM_FAR (farPL_)
  PipelineLayout slimFluidLayout; // GRP_SLIM_FLUID (fluidPL_)
  PipelineLayout slimFluidSeamLayout; // GRP_SLIM_FLUIDSEAM (fluidSeamPL_)
  BindGroup simSet;               // simBG_[page]
  BindGroup slimSet;              // simSlimBG_[page]
  BindGroup particleSet;          // particleBG_[page]
  BindGroup farSet;               // farBG_
  BindGroup fluidSet;             // fluidBG_
  BindGroup fluidSeamSet;         // fluidSeamBG_[page]
};

// Record one table through the encoder's generated-barrier recorder. This is
// the whole of Simulation::RecordTable now that the Dawn walk is gone — one
// walker, one table. `timer` is the --measure hook (may be null); when set,
// the recorder writes a GPU timestamp pair around each run of rows sharing a
// `group` label, which is the granularity the phase-0 per-pass baseline was
// measured at, so the numbers stay comparable to it.
void RecordTableVulkan(const CommandEncoder& enc, pass::Table which, const TableCtx& cx,
                       const TableBindings& tb, PassTimer* timer);

}  // namespace rhi
