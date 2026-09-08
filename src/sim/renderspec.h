// renderspec.h — what the frame's RenderParams says about the raymarch's
// compile-time SPEC_* constants (W2-A, assets/shaders/raymarch.wgsl).
//
// raymarch.wgsl is compiled TWICE: the universal pipeline, and a LEAN one with
// `SPEC_FLUID` / `SPEC_DEBUG_VIZ` / `SPEC_SHORT_RANGE` folded to false, which
// deletes ~24% of the optimized fragment shader (the MPM fluid march and its
// two nested marches, the active-voxel debug probe inside trace()'s per-cell
// loop, and the short-range ceiling). Simulation::DrawWorld picks one per
// frame; this is what it picks on.
//
// EACH FIELD IS A UNIFORM THAT WAS ALREADY WRITTEN, NOT A PREDICTION OF ONE.
// WriteRenderParams (test/support.h) is documented as the single author of the
// flag word and fluidCount — every drawing path in the engine calls it before
// it draws — so it sets these three from the `rp` it is about to upload. There
// is nothing here to infer from a CPU mirror, nothing to read back, and no way
// for the variant to disagree with the frame it draws.
//
// ITS OWN HEADER, and a dependency-free one, for a build reason: the reader is
// sim/simulation.cpp, the single most-recompiled translation unit in the tree,
// and support.h drags in the avatar, the mob system, the debris system and the
// GPU context. The writer is support.cpp because that is where the one author
// lives.

#pragma once

namespace sandvox {

struct RenderSpec {
  bool fluid = true;       // R.fluidCount > 0
  bool debugViz = true;    // R.flags bit 1 — the active-voxel highlight
  bool shortRange = true;  // R.flags bit 2 — the 100 m ray ceiling
  // The lean variant is legal exactly when every specialized branch is off.
  // Defaults say "all of them might be live", so a draw that somehow preceded
  // any WriteRenderParams call takes the universal pipeline.
  bool AllOff() const { return !fluid && !debugViz && !shortRange; }
};

const RenderSpec& LastRenderSpec();

}  // namespace sandvox
