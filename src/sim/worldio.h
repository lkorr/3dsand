#pragma once
#include <string>

#include "gpu/context.h"
#include "sim/simulation.h"
#include "sim/world.h"

// Versioned RLE world serialization — the M2 disk-streaming core (DESIGN.md
// §3). Chunk-granular (each 16^3 chunk RLE-compressed independently) so the
// same format serves future toroidal region files; falling-sand worlds are
// extremely runny, so RLE typically shrinks 64 MB of voxels to a few MB.
//
// Save blocks on a full GPU readback and Load re-uploads + wakes the world —
// user-triggered whole-world snapshots, not per-frame paths. The stamp byte is
// stripped on save (it is per-tick scratch, not world state).

bool SaveWorld(GpuContext& ctx, World& world, const std::string& path);
bool LoadWorld(GpuContext& ctx, World& world, Simulation& sim, const std::string& path);
