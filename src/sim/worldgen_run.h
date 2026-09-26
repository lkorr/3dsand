#pragma once
#include <cstdint>

#include "gpu/context.h"
#include "sim/simulation.h"
#include "sim/world.h"

// worldgen_run.h — generating the residency window from the seed, and the
// blocking voxel readback it (and every gate) reads chunks through. Sim-layer:
// the game's startup and regen call this, so it does not live with the
// selftest (src/test/support.* re-exports it for the gates).
namespace sandvox {

// Regenerate the whole residency window from `seed` (paged: in batches, with
// the GPU's per-chunk verdict demoting the empties), dispose of the derived
// state that described the old world (water-body ledger, held snapshot), and
// re-queue the authored edit layer's ops for the window.
void SubmitWorldgen(GpuContext& ctx, World& world, Simulation& sim,
                    uint32_t seed);

// THE CPU SEAM for gate voxel dumps (PLAN_page_table.md §2.1a, fifth site).
//
// Reads `count` chunks starting at slot `firstSlot` into `out` (count *
// kChunkVol words), resolving each slot through World::PageOffsetOfSlot and
// SYNTHESIZING sentinel chunks CPU-side. A gate that indexes the result with
// World::SlotCellIndex therefore gets a dense-looking snapshot in slot order
// whatever the residency mode is — which is what the gates want, since they
// test sim behaviour rather than residency.
//
// `page-roundtrip` is the gate that reads THROUGH the translation instead, on
// purpose; everything else goes through here.
void ReadVoxelsSync(GpuContext& ctx, World& world, uint32_t firstSlot,
                    uint32_t count, uint32_t* out, const char* label);

}  // namespace sandvox
