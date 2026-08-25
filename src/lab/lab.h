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
  // ---- WP5's two scenes. Everything above pours PARTICLES into an authored
  // box holding at most ~40,000 of them; none of them can reproduce the
  // user-reported excite burst, because that bug is about SETTLED water —
  // a still body many times the particle pool's size, disturbed from below.
  kLabPond,        // worldgen-sized still bowl + a plug pulled underneath
  kLabWorldLake,   // the SAME experiment on the real worldgen (labMode 0)
  kLabSceneCount,
};

// Name <-> id ("basin", "hill", "faucet", "pool", "slosh", "pond",
// "worldlake"). -1 if unknown.
int LabSceneFromName(const std::string& name);
const char* LabSceneName(int scene);

// The pond scene's disc radius, in voxels. Worldgen's own range is
// TUNE_POND_RADIUS_MIN..+SPAN = 68..127 (worldgen.wgsl pondInfo), and the
// default here is the SMALL end of that — the point of the scene is that even
// worldgen's smallest pond is ~210,000 water voxels, 6.4x the whole particle
// pool. Settable so the "maximum drainable body size" sweep (plan §8) is one
// bench argument (`--fluid-bench pond48`) rather than a rebuild.
void LabSetPondRadius(int r);
int LabPondRadius();

// Does this scene run on the flat lab slab (true) or on the real worldgen
// (false)? Only kLabWorldLake answers false — it is the main-world arm of the
// pond measurement, and it must see actual terrain, caves and the authored
// lake at (420,420).
bool LabSceneUsesLabWorld(int scene);

// Fixed per-scene camera/spawn pose (fly enabled in the windowed lab).
void LabSceneCamera(int scene, Vec3& eye, float& yaw, float& pitch);

// CellOps for scene tick `sceneTick` (1-based). The list covers the scene's
// ENTIRE build volume, air cells included, which is what makes re-submitting
// it the reset: every cell the scene (or its water) could have touched is
// restored to the post-worldgen-plus-build state.
//
// The volume is enumerated in a fixed linear order and sliced
// kMaxCellOpsPerTick per scene tick, so a build larger than one tick's op
// budget simply takes more ticks. Every scene through `slosh` fits one slice
// and still lands wholly on tick 1 (the largest, hill, is 57,400 of 65,536);
// `pond` at r=68 is 563,430 cells and takes 9.
void LabSceneBuildOps(int scene, uint32_t sceneTick, uint32_t waterMat,
                      std::vector<CellOp>& out);

// Scene tick the build finishes on (the last tick LabSceneBuildOps emits
// anything for). 1 for every pour scene.
uint32_t LabSceneBuildEndTick(int scene);

// The disturbance: the scene tick on which the plug is pulled (0 = this scene
// has no plug), and the CellOps that pull it. For `pond` / `worldlake` this is
// the whole experiment — a shaft and a sealed chamber carved out from under a
// still body of water, i.e. "dig a hole under a pond".
uint32_t LabScenePlugTick(int scene);
void LabScenePlugOps(int scene, std::vector<CellOp>& out);

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
