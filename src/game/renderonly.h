#pragma once
// renderonly.h -- A BODY THAT IS DRAWN AND NOTHING ELSE (2026-10-13).
//
// Some things are seen but are not bodies in any system: an arrow in flight
// is a fixed-point point in ArrowFlights (game/arrows.h), deliberately not a
// Jolt body (thin fast bodies tunnel, and CCD on them is the 3 fps trap). It
// still has to be SEEN, or a shot is a body that teleports from the string to
// wherever it stops. BodyRegistry (game/bodyreg.h) walks a list of these after
// every real body: a transform slot and a micro-body draw of a SHARED model,
// with no physics handle, no cube instances and no claim on the model (the
// brick-record audit never sees them, because they hold nothing).
//
// RENDER-ONLY: built per frame from sim state, never hashed, never saved.

#include <cstdint>

#include "math3d.h"

struct RenderOnlyBody {
  // The body's voxel-grid origin in world voxels and its rotation -- the
  // BodyTransform convention every drawn body uses (the corner, not a centre).
  Vec3 pos{};
  float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  // A micro-body model index (MicroBodySet::models); a negative model is
  // never pushed by a producer (it would have nothing to draw).
  uint32_t model = 0;
};
