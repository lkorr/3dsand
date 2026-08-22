// measure.h — the `--measure` headless sizing harness. See measure.cpp.
// Measurement-only tooling; not reachable from the game or the selftest.
#pragma once

class GpuContext;
class World;
class Simulation;

namespace sandvox {

// Generates the default world, settles it, then prints (1) the occupancy
// histogram of the residency window and (2) per-compute-pass GPU timings in
// three activity scenarios. Returns a process exit code.
int RunMeasure(GpuContext& ctx, World& world, Simulation& sim);

}  // namespace sandvox
