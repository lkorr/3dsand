// lab.h — the fluid lab (docs/PLAN_fluid_overhaul.md §4, WP1).
//
// A dedicated test world for looking at, tuning, and benchmarking the MLS-MPM
// fluid: a flat stone slab (worldgen labMode — world.h kLabSlabY) with five
// scripted scenes built as CellOp lists and poured through FluidSpawnOp — the
// same mutation paths as everything else (CLAUDE.md rule 3). Deterministic by
// construction: fixed seed, fixed tick schedule, spawn jitter hashed from
// (sceneTick, index) exactly like the mpm dev tool's pour.
//
// Two consumers:
//   --lab [scene]         windowed: the scene runs live, L replays it from
//                         the post-worldgen state, tuning.json hot-follows.
//   --fluid-bench [scene] headless: fixed camera, N ticks, per-pass GPU
//                         timings + curves + mass ledger emitted as JSON.
//
// Lives in src/lab (the src/measure precedent): it consumes test/support.h's
// SubmitTick/SubmitWorldgen plumbing, which game code must not.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"
#include "sim/materials.h"
#include "sim/world.h"

class GpuContext;
class Simulation;
struct Tuning;

namespace sandvox {

// Scene ids, in bench order. `hill0` is the hill scene with exciteMode forced
// 0 — the A/B that reproduces the reported mid-slope trapdoor (plan §4.2).
enum LabScene {
  kLabBasin = 0,   // walled dam-break box: baseline look + timing
  kLabHill,        // ~31 deg stepped ramp + catch basin: THE acceptance scene
  kLabFaucet,      // sustained pour: budget pressure, settle churn
  kLabPool,        // pour then still: the sleep scene
  kLabSlosh,       // channel wave: inertia/liveliness A/B
  kLabSceneCount,
};

// Name <-> id ("basin", "hill", "faucet", "pool", "slosh"). -1 if unknown.
int LabSceneFromName(const std::string& name);
const char* LabSceneName(int scene);

// Fixed per-scene camera/spawn pose (fly enabled in the windowed lab).
void LabSceneCamera(int scene, Vec3& eye, float& yaw, float& pitch);

// CellOps for scene tick `sceneTick` (1-based; the whole build lands on tick
// 1 — every scene volume fits one kMaxCellOpsPerTick budget). The list covers
// the scene's ENTIRE bounding volume, air cells included, which is what makes
// re-submitting it the reset: every cell the scene (or its water) could have
// touched is restored to the post-worldgen-plus-build state.
void LabSceneBuildOps(int scene, uint32_t sceneTick, uint32_t waterMat,
                      std::vector<CellOp>& out);

// This scene tick's pour, as FluidSpawnOps. Budget charged BEFORE emission
// (rule 2): a cell whose 8 particles do not fit under kFluidCap against
// `liveEstimate` (+ what is already in `out`) is refused whole.
void LabScenePour(int scene, uint32_t sceneTick, uint32_t liveEstimate,
                  uint32_t waterMat, std::vector<FluidSpawnOp>& out);

// Last scene tick that pours (~0u = pours forever in the windowed lab).
uint32_t LabScenePourEnd(int scene);

// Bench run length per scene, in ticks.
uint32_t LabSceneBenchTicks(int scene);

// Mass-sweep AABB (inclusive, world voxels): where every poured eighth must
// be standing (or carried by a live particle) at the end of a bench run.
void LabSceneBounds(int scene, IVec3& lo, IVec3& hi);

// --fluid-bench [scene|hill0|all]: headless scripted runs. Writes a JSON
// report to `jsonPath` (empty -> "fluid_bench.json") and a per-scene BMP.
// Requires World::SetLabWorld(true) and timestamps requested at ctx.Init.
int RunFluidBench(GpuContext& ctx, World& world, Simulation& sim,
                  const std::vector<MaterialDef>& mats,
                  const std::string& sceneArg, const std::string& jsonPath);

// ---- live-tuning file plumbing (windowed lab) ------------------------------
// mtime in ns since epoch, or -1 if the file cannot be statted.
int64_t LabFileMtimeNs(const std::string& path);

// Surgical writeback of the ImGui fluid sliders into tuning.json: patches the
// eight sim.fluid* look-knob values in the file TEXT (never a re-serialize, so
// the tuner's formatting and every other key survive). Refuses when the file
// on disk is newer than *lastLoadedMtimeNs — the tuner wrote meanwhile, and
// the watcher will load that instead (last-writer-wins with an mtime check;
// the documented running-app-clobbers-tuning-json gotcha). fluidExciteMode is
// deliberately NOT written: the lab forces it to 1 at runtime, and a lab
// session must not flip the shipped default. Updates *lastLoadedMtimeNs on a
// successful write.
bool LabPatchTuningJson(const std::string& path, const Tuning& t,
                        int64_t* lastLoadedMtimeNs);

}  // namespace sandvox
