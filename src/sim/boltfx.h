// boltfx.h (src/sim, beside weather.*: render-only) — THE BOLT YOU SEE (electricity wave 2, package D;
// docs/PLAN_electricity_wave2.md "Package D"). RENDER-ONLY.
//
// A strike (game/lightning.h PlanStrike) lays a jagged column of `lightning`
// cells that lives a tick or two. Drawn as voxels that is a column of white
// cubes. This module turns the PLAN'S PATH (StrikePlan::paths, carried to the
// frame on TickAuthorityCtx::strikes.events) into what a bolt looks like:
//
//   * the channel, resampled and displaced at sub-cell scale (a render-only
//     hash), so it is a jagged line and not a staircase;
//   * the channel continued UP out of the window toward the cloud deck, with
//     render-only side branches that die out on the way down;
//   * the plan's forks and splash walks as thinner, shorter-lived branches;
//   * a STROKE ENVELOPE on the render clock: the return stroke, two or three
//     re-strikes down the same channel, the continuing-current glow between
//     them, a flicker, then the fade. The branches light on the first stroke;
//     the re-strikes run down the channel only, as real ones do.
//   * light points along the lower channel: the raymarch lights the scene
//     FROM THE BOLT (raymarch.wgsl boltLightAt) while it burns.
//
// Everything here is a pure function of (paths, seed, render time). Nothing
// is read back into the sim; the world hash cannot see it.
//
// GPU: one small read-only storage buffer at renderBGL 44 (boltBuf, owned by
// Simulation and registered here). Rewritten by Upload() -- called from the
// ONE place every drawing path sets its RenderParams (WriteRenderParams) --
// only while a bolt lives, plus once to zero it after the last one dies. An
// empty buffer costs the raymarch ONE dynamically-uniform load a pixel.
//
// LAYOUT, in vec4 rows (raymarch.wgsl BOLT_* mirrors these; check_invariants
// is not involved because only raymarch reads it -- keep them together):
//   row 0                      (segCount, groupCount, lightCount, 0) as f32
//   rows kRowGroups..          two per group of kGroupSegs segments:
//                              (lo.xyz, firstSeg) (hi.xyz, segCount)
//   rows kRowLights..          (pos.xyz, intensity) per light point
//   rows kRowSegs..            two per segment: (a.xyz, intensity) (b.xyz, width)
// Positions are world fine-voxel units (the raymarch's R.camPos space).
#pragma once

#include <cstdint>
#include <vector>

#include "math3d.h"

namespace rhi {
class Buffer;
class Queue;
}  // namespace rhi

// THE PLAN'S SHAPE, for the frame (game/lightning.h PlanStrike fills
// StrikePlan::paths). The same walks PlanStrike pushes as CellOps, kept as
// ordered cell lists so the renderer can draw a luminous bolt ALONG the planned
// path instead of a column of white cubes. Render-only: nothing reads it back
// into the sim, it is not hashed and not in the op record. Positions are
// recorded whether or not the cell was in the window.
struct StrikePath {
  enum Kind : uint8_t { Channel = 0, Fork = 1, Splash = 2 };
  Kind kind = Channel;
  std::vector<IVec3> cells;   // in walk order: top -> foot for the channel,
                              // the channel cell it leaves first for a fork,
                              // the foot first for a splash walk
};

namespace boltfx {

constexpr uint32_t kMaxBolts = 4;
constexpr uint32_t kGroupSegs = 16;
constexpr uint32_t kMaxSegs = 1024;
constexpr uint32_t kMaxGroups = kMaxSegs / kGroupSegs;
constexpr uint32_t kMaxLights = 16;
constexpr uint32_t kRowGroups = 1;
constexpr uint32_t kRowLights = kRowGroups + 2 * kMaxGroups;
constexpr uint32_t kRowSegs = kRowLights + kMaxLights;
constexpr uint32_t kRows = kRowSegs + 2 * kMaxSegs;
constexpr uint64_t kBytes = (uint64_t)kRows * 16;

// Simulation::Init registers the buffer it bound at renderBGL 44.
void SetBuffer(const rhi::Buffer* buf);

// A strike the frame was told about (main.cpp drains strikes.events). `time`
// is the render clock WriteRenderParams is handed (wall seconds in play,
// the fixed shot time in --shot); `seed` keys the render-only shape.
void NoteStrike(const std::vector<StrikePath>& paths, float time, uint32_t seed);

// Rewrite the GPU table for render time `time` (no-op while nothing lives and
// the table is already empty).
void Upload(const rhi::Queue& queue, float time);

// Any bolt alive at `time` (the perf harness and the shots ask).
bool Active(float time);
// Forget every bolt (a world reload, a shot set-up).
void Clear();

}  // namespace boltfx
