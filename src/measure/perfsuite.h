// perfsuite.h — `--perf`, the harness behind the tuner's Performance tab.
//
// WHAT IT IS FOR, and how it differs from the two harnesses next to it.
//
//   --measure  answers "how should the Vulkan port be sized": occupancy
//              histograms, chunk uniformity, per-pass GPU time averaged over
//              three synthetic scenarios. It BLOCKS after every tick, so its
//              wall clock is a latency, not a frame rate, and it says so.
//   --selftest answers "is the engine correct", with one advisory render
//              number attached at the end.
//   --perf     answers "where did my frame go". It runs the engine the way the
//              game runs it — submit, render, pump, never wait — and records a
//              per-FRAME row of CPU scopes, GPU pass times and the counters
//              that explain them, for five scenarios that each light up a
//              different part of the engine.
//
// THE MEASUREMENT DESIGN, and the one trap it exists to avoid.
//
// You cannot get honest frame times and honest per-pass GPU times out of a
// harness that blocks on the GPU each tick: the block IS the frame time. The
// obvious workaround — measure them in two separate arms — halves the run time
// budget and leaves the page correlating two different executions.
//
// So neither. PassTimer::KickDeferred/PollDeferred map the timestamp buffer
// through a fence ring, so the frame path never waits and the numbers arrive
// two or three frames late, TAGGED WITH THE FRAME THEY BELONG TO. Every sample
// row therefore holds real wall clock and real GPU attribution for the same
// frame, and rows whose queries have not landed yet are marked `gpuValid =
// false` rather than being drawn as a GPU that cost nothing.
//
// DETERMINISM. Every scenario's op stream is a pure function of its local tick,
// so a --perf run is reproducible and the world hash at the end of each
// scenario is printed. The harness asserts that attaching the timer does not
// move it: a timestamp is a pass-descriptor attachment that observes a dispatch
// without reordering it, and this is the check that keeps that true.

#pragma once

#include <string>
#include <vector>

class GpuContext;
class World;
class Simulation;
struct MaterialDef;

namespace sandvox {

struct PerfOptions {
  // Run only this scenario id (empty = all). Same shape as --gate.
  std::string only;
  // Where the JSON lands. The tuner reads build/perf.json.
  std::string out = "build/perf.json";
  // Offscreen render size. 1080p by default so the render number is comparable
  // to the selftest's "render 1080p" sweep.
  uint32_t width = 1920, height = 1080;
  // List the scenarios and exit.
  bool list = false;
};

// Returns 0 on success. Prints a human-readable summary as it goes — the JSON
// is for the page, the stdout is for the terminal, and neither is a
// reformatting of the other.
int RunPerf(GpuContext& ctx, World& world, Simulation& sim,
            const std::vector<MaterialDef>& mats, const PerfOptions& opt);

// `--render-budget`: the raymarch's OWN breakdown.
//
// --perf answers "where did my frame go" and, when the answer is "the render
// pass", stops — `raymarch` is one GPU span with nothing inside it. This is the
// next question: WHERE inside it. One settled world, one camera, one arm per
// suspected cost centre, each rendering the identical frame with exactly one
// knob moved, all in a single process. The delta from the baseline arm is that
// feature's cost.
//
// It exists so that diagnosing the render never becomes the feature-by-feature
// elimination sequence CLAUDE.md's rule 6 forbids: the whole table is one run.
// `opt.only` picks the camera (any --perf scenario id, default `idle`);
// `opt.width/height` set the resolution. Prints a table; writes no JSON.
int RunRenderBudget(GpuContext& ctx, World& world, Simulation& sim,
                    const std::vector<MaterialDef>& mats,
                    const PerfOptions& opt);

}  // namespace sandvox
