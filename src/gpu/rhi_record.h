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
// resolve against are pass::RecordCtx (sim/pass_table.h) — the SAME struct
// Simulation fills and the recorder reads, passed through by reference. This
// header used to declare a copy of it (rhi::TableCtx) that the bridge copied
// field by field into a third; see pass::RecordCtx for what that cost.

// The live resources a table row resolves against, as NON-OWNING pointers to
// the seam objects behind Simulation's handles. The symbolic ids
// (DirtyIn/DirtyOut, ParticlesRead/ParticlesWrite) must already be resolved
// for the current page — Simulation::PassBuffer does that, in the same place
// in the flow (record time) it always has.
//
// RAW POINTERS, NOT HANDLES, and that is the point: this struct is rebuilt on
// every RecordTable call (up to fluidSubsteps + 4 per tick), and as ~180
// refcounted handles every build was ~180 atomic increments plus as many
// decrements, per call, for data the bridge only ever reads. The pointees are
// owned by Simulation for the whole call; nothing here outlives it.
struct TableBindings {
  BufferImpl* buffers[(int)pass::Buf::kCount] = {};
  ComputePipelineImpl* pipelines[(int)pass::Pipe::kPipeCount] = {};  // by (int)pass::Pipe
  PipelineLayoutImpl* simLayout = nullptr;           // GRP_SIM (simPL_)
  PipelineLayoutImpl* slimPartLayout = nullptr;      // GRP_SLIM_PART (simPL2_)
  PipelineLayoutImpl* slimFarLayout = nullptr;       // GRP_SLIM_FAR (farPL_)
  PipelineLayoutImpl* slimFluidLayout = nullptr;     // GRP_SLIM_FLUID (fluidPL_)
  PipelineLayoutImpl* slimFluidSeamLayout = nullptr; // GRP_SLIM_FLUIDSEAM (fluidSeamPL_)
  PipelineLayoutImpl* slimGasLayout = nullptr;       // GRP_SLIM_GAS (gasPL_)
  // GRP_SHADOW (shadowPL_) — ONE set, unlike every Slim* pair above: the
  // resolve pass shares no buffer with the sim groups.
  PipelineLayoutImpl* shadowLayout = nullptr;
  BindGroupImpl* simSet = nullptr;        // simBG_[page]
  BindGroupImpl* slimSet = nullptr;       // simSlimBG_[page]
  BindGroupImpl* particleSet = nullptr;   // particleBG_[page]
  BindGroupImpl* farSet = nullptr;        // farBG_
  BindGroupImpl* fluidSet = nullptr;      // fluidBG_
  BindGroupImpl* fluidSeamSet = nullptr;  // fluidSeamBG_[page]
  BindGroupImpl* gasSet = nullptr;        // gasBG_[page]
  BindGroupImpl* shadowSet = nullptr;     // shadowBG_
};

// Record one table through the encoder's generated-barrier recorder. This is
// the whole of Simulation::RecordTable now that the Dawn walk is gone — one
// walker, one table. `timer` is the --measure hook (may be null); when set,
// the recorder writes a GPU timestamp pair around each run of rows sharing a
// `group` label, which is the granularity the phase-0 per-pass baseline was
// measured at, so the numbers stay comparable to it.
void RecordTableVulkan(const CommandEncoder& enc, pass::Table which,
                       const pass::RecordCtx& cx, const TableBindings& tb,
                       PassTimer* timer);

}  // namespace rhi
