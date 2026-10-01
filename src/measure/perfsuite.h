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

#include "math3d.h"   // IVec3 (the seam / far-refill helpers)

class GpuContext;
class World;
class Simulation;
struct MaterialDef;

struct CellOp;

namespace sandvox {

// ---- THE VILLAGE FIRE (owner report 2026-10-01: 10 fps) -------------------
// Harrowby's three houses (the default map's refs/harrowby.json) with their
// ground floors flooded `oilDepth` deep in oil and a match on the surface:
// --perf village-fire builds it after moving the window onto the green, the
// windowed `--burn-house` with SANDVOX_BURN_VILLAGE=1 after walking the
// player there. Every op is IfAir. False + `why` when the map has no Harrowby.
struct VillageFireOps {
  std::vector<CellOp> oil, fire;
  IVec3 green{};
  int houses = 0;
  uint32_t oilCells = 0;
  std::vector<IVec3> solidSample;   // house cells that must be solid (alignment check)
};
bool VillageGreen(IVec3& green, std::string& why);
bool BuildVillageFireOps(const World& world, const std::vector<MaterialDef>& mats,
                         int oilDepth, VillageFireOps& out, std::string& why);

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
  // --budget-arms a,b,c: run only these --render-budget arms (empty = all).
  // What makes a one-boot --verify affordable: the full table is ~14 arms of
  // 60 frames each, and a package usually has one suspect.
  std::vector<std::string> arms;
  // `--render-budget` only: comma-separated camera ids from the --render-budget
  // camera table (`noon,dusk,submerged`). Empty = all of them. Ignored when
  // `only` names a --perf scenario, which selects a single borrowed camera and
  // bypasses the table.
  std::string cams;
};

// One --render-budget row, for callers that record rather than read the
// terminal (--verify writes them into build/last_run.json).
struct RenderBudgetRow {
  std::string cam;  // which camera of the table (or the borrowed scenario)
  std::string arm;
  bool ok = false;
  double gpuP50Ms = 0, gpuP95Ms = 0;
  std::string why;  // when !ok
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
// THREE CAMERAS, not one. `noon` is the historical overlook and is unchanged to
// the digit; `dusk` is the same eye with the sun ~8 deg up (long shadow rays,
// raked terrain); `submerged` puts the eye inside the authored lake, which is
// the only way to reach shadeSubmerged — god rays, caustics, Snell's window —
// because the medium is derived from the ray, not from a render flag. Select
// with `opt.cams` ("noon,dusk"); empty runs all three. `opt.only` still picks a
// single camera borrowed from a --perf scenario and bypasses the table.
// `opt.width/height` set the resolution; `opt.arms` picks an arm subset
// (--budget-arms). Prints one table per camera and writes
// build/render_budget.json plus one BMP per camera; `rows`, when given, also
// receives one entry per (camera, arm) run so --verify can record them.
int RunRenderBudget(GpuContext& ctx, World& world, Simulation& sim,
                    const std::vector<MaterialDef>& mats,
                    const PerfOptions& opt,
                    std::vector<RenderBudgetRow>* rows = nullptr);

// ---- THE LOD SEAM AT EYE HEIGHT (2026-09-28, LOD-seam overhaul P0) --------
//
// Shared by --shot's screenshot_seam_* frames and the `seam` / `seamveg`
// --render-budget cameras, so the frame the look review judges is the frame
// the budget times. A pose is a column plus an eye 17 voxels (1.7 m) over its
// ground, the ground ASKED FOR (World::TerrainColumn), never written down.
struct SeamPose {
  int x = 0, z = 0;
  int ground = 0;           // TerrainColumn(x, z).h (or the water surface)
  float ex = 0, ey = 0, ez = 0;
  const char* what = "";    // how the site was chosen, for the log / note
};
// The flattest column of the harness pad (bare desert sand -- the pad refuses
// all cover) along +x (60 m) and toward the (-x,+z) corner: the geometry /
// shading seam with no plants and no hill in the way.
SeamPose SeamPlainPose();
// The flattest column, in the MEADOW-biome map cell nearest the spawn, whose
// +x view line stays meadow, dry and flat for 60 m (the harness pad has no
// vegetation; the nearest meadow is ~6 km out). Falls back to the spawn
// column (forest), and says so in `what`.
SeamPose SeamVegPose();
// Centre the residency window AND the far field on the eye exactly as play
// does (Stream::Update's target: origin = eye chunk - kNChunk/2 on all three
// axes; FarField recentred on the eye chunk), regenerate, drain the far fill
// and settle `settleTicks` ticks. Without this the window face sits wherever
// the harness origin put it, not 25.6 m ahead of the eye, and the frame is not
// of the in-game seam. The caller resets any Stream (OnRegen) first.
void CentreWindowOnEye(GpuContext& ctx, World& world, Simulation& sim,
                       float ex, float ey, float ez, uint32_t settleTicks);
// A wholesale far-field refill centred on `playerChunk` (a FINE chunk coord,
// as play's FarField::Update takes), drained to completion. What every
// headless view of the cascade needs: without it the budget runner rendered
// whatever cascade an earlier harness left behind, or none.
void RefillFarAround(GpuContext& ctx, World& world, Simulation& sim,
                     IVec3 playerChunk);
// The fine chunk at the residency window's centre (origin + kNChunk/2).
IVec3 WindowCentreChunk(const World& world);

}  // namespace sandvox
