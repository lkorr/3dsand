#pragma once
#include <string>

#include "gpu/context.h"
#include "sim/simulation.h"
#include "sim/stream.h"
#include "sim/world.h"

// Versioned chunk-RLE world serialization (M2, DESIGN.md §3). v2 (SVX2) is
// chunk-granular WITH world chunk coordinates: the file is simply the chunk
// store (everything ever streamed out) plus the flushed resident window and
// the window origin, so an infinite streamed world round-trips.
//
// Save flushes the resident window into the store (blocking readback) and
// serializes the store; Load replaces the store, re-fills the window from it
// (procgen for misses), and wakes the world. User-triggered whole-world
// snapshots, not per-frame paths. Stamp bytes are stripped on save.

bool SaveWorld(GpuContext& ctx, World& world, Stream& stream, const std::string& path);
bool LoadWorld(GpuContext& ctx, World& world, Simulation& sim, Stream& stream,
               const std::string& path);
