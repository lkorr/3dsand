#pragma once
#include <string>

#include "gpu/context.h"
#include "sim/simulation.h"
#include "sim/stream.h"
#include "sim/world.h"

// Region-directory world persistence (M2 + region files, DESIGN.md §3).
// `path` is a DIRECTORY (e.g. "world.svd") holding meta.svm (magic, world
// constants, window origin) plus the chunk store's r_<x>_<y>_<z>.svr region
// files — an infinite streamed world round-trips without a monolithic file.
//
// Save flushes the resident window into the store (draining async evictions)
// then binds the store to the directory and flushes its dirty regions; meta
// is written last so a save is only valid if it completed. Load reads meta,
// points the store at the directory (regions lazy-load from disk), re-fills
// the window from it (procgen for misses), and wakes the world.
//
// Because the bound store also LRU-spills to the same directory while
// streaming, the directory is a live world store; a store already bound to a
// DIFFERENT directory refuses to save/load (one world dir per session).
// Stamp bytes are stripped on save.

bool SaveWorld(GpuContext& ctx, World& world, Stream& stream, const std::string& path);
bool LoadWorld(GpuContext& ctx, World& world, Simulation& sim, Stream& stream,
               const std::string& path);
