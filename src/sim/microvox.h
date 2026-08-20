#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sim/materials.h"
#include "sim/world.h"

// Static micro-detail (docs/PLAN_voxel_editor.md §A, DESIGN.md §9).
//
// A material may declare a `"micro"` block in materials.json. The world cell
// stays an ORDINARY 16-bit voxel of that material — the CA is untouched, the
// world hash is untouched, occupancy still counts it — and only the RAYMARCHER
// substitutes a finer subdiv^3 model when a primary ray lands on the cell.
// That is the whole trick: zero per-instance storage, zero new world state,
// zero sim cost, and a grass tuft or a flower is exactly as cheap to simulate
// as the dirt voxel it replaced.
//
// AUTHORING RULE — a micro material's own `colors` are its LOD COLOURS.
// Past TUNE_MICRO_LOD_DIST the cell shades as a plain voxel using them, so they
// must be the MODEL'S AVERAGE rather than its most interesting colour. Author a
// poppy's palette as muted green with a red cast, not as red: a distant meadow
// otherwise reads as a field of red cubes instead of as grass with flowers in
// it, and the LOD handoff pops instead of dissolving.
//
// Determinism (CLAUDE.md rule 1): everything here is RENDER-ONLY. The pool and
// the table are bound to the raymarch pipeline and to nothing else — they must
// never appear in a sim shader's bind group. Per-cell variation (yaw, jitter)
// is keyed on hash3(seed, 0, cellIndexW) so it is stable and identical
// everywhere without being sim state; the flipbook frame is an integer
// function of `tick`, for the same reason.

// ---- GPU layout ------------------------------------------------------------

// Per-material entry, 16 bytes — must match struct MicroBrick in common.wgsl.
// One per material slot (kMaterialSlots), so the shader indexes it with the
// voxel's material id and needs no indirection.
struct MicroBrickGpu {
  // Word index into the brick pool where this material's data starts.
  // kMicroNoBrick (0xFFFFFFFF) means "this material has no micro model" — the
  // renderer takes the ordinary cube path.
  //
  // POOL LAYOUT AT `base`:
  //   [0 .. frameCount)              per-frame CUMULATIVE tick offsets. Entry i
  //                                  is the tick at which frame i ENDS within
  //                                  one loop, so the last entry is the loop
  //                                  period. Cumulative rather than per-frame
  //                                  durations because the shader's job is
  //                                  "which frame is tick T in", and a
  //                                  cumulative table answers that with a
  //                                  compare per frame and no running sum.
  //   [frameCount .. )               the voxel payload: frameCount consecutive
  //                                  bricks of (subdiv^3 / 4) words, 4 packed
  //                                  8-bit palette indices per word, x-major
  //                                  then y then z (idx = (z*S + y)*S + x).
  //
  // Palette index == material ID (the project-wide .vox convention), so a micro
  // voxel shades through the existing material table with no extra mapping.
  // 8 bits caps micro materials at ids 1..255, which is checked at load.
  uint32_t base;
  uint32_t subdivLog2;  // 1, 2 or 3 (subdiv 2/4/8)
  // bits 0..7   frameCount (1..255)
  // bits 8..31  loop period in ticks (== the last cumulative entry)
  uint32_t frameInfo;
  uint32_t flags;  // kMicroYawVariants | kMicroJitter
};
static_assert(sizeof(MicroBrickGpu) == 16, "must match common.wgsl MicroBrick");

constexpr uint32_t kMicroNoBrick = 0xFFFFFFFFu;

// MicroBrickGpu.flags — must match MICROF_* in common.wgsl.
constexpr uint32_t kMicroYawVariants = 1;  // hash-keyed quarter-turn yaw
constexpr uint32_t kMicroJitter = 2;       // hash-keyed sub-cell XZ offset

// Hard ceiling on the pool, in u32 words. Defined in world.h (as
// kMicroPoolWordsWorld) because the WGSL prelude is generated from that file
// and the shader needs the same number for its bounds check; aliased here so
// the loader reads naturally.
constexpr uint32_t kMicroPoolWords = kMicroPoolWordsWorld;

// ---- CPU side --------------------------------------------------------------

// Everything the GPU needs, built once at load and re-built on R.
struct MicroSet {
  std::vector<MicroBrickGpu> table;  // exactly kMaterialSlots entries
  std::vector<uint32_t> pool;        // packed bricks; may be empty
  uint32_t materialCount = 0;        // how many materials declared a micro block
  uint32_t frameCount = 0;           // total frames across all of them
};

// Loads the `micro` block of every material in materials.json and packs the
// referenced .vox files into `out`. `assetDir` is the assets/ root (model paths
// in the JSON are relative to it). `mats` is the already-compiled material
// list; MATF_MICRO is set on the ones that got a valid brick, so this must run
// AFTER LoadAssets and BEFORE the material table is uploaded.
//
// Returns false only if the JSON itself is unreadable. Per-material problems
// (missing file, wrong dims, bad subdiv) are reported through `log` and that
// material simply keeps the plain cube path — a broken flower must not stop
// the world from loading (DESIGN.md §6: modders get diagnostics, not silent
// breakage, and not a hard stop either).
bool LoadMicroVox(const std::string& materialsPath, const std::string& assetDir,
                  std::vector<MaterialDef>& mats, MicroSet& out, std::string& log);
