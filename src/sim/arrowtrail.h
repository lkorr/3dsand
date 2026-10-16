#pragma once
// arrowtrail.h -- THE ARROW TRAILS' GPU LAYOUT (render-only, 2026-10-15).
//
// A faint ribbon behind every arrow in flight, so the arc it flies reads
// (owner: "a little trail behind them like the wind particle trail"). Its OWN
// system, deliberately not a slice of the gust streaks: the wind's look is
// expected to change, and the arrows' should not change with it.
//
//   CPU   game/arrows.h ArrowTrails: follows each flight, packs the rows.
//   GPU   Simulation::arrowTrailBuf_ (renderPartBGL_ binding 6, vertex),
//         queue-written each frame by Simulation::UploadArrowTrails.
//   DRAW  assets/shaders/arrow_trail.wgsl vsTrail/fsTrail.
//
// The buffer is kArrowTrailSlots trails of kArrowTrailPts vec4f rows,
// (x, y, z, alpha) in world voxels, point 0 the arrow's drawn point and on
// back through where it was. A row past a trail's end has alpha 0 and sits on
// the one before it (no segment). ARROW_TRAIL_PTS in the shader must match
// (check_invariants `arrowtrail`). Never hashed, never read by the tick.

#include <cstdint>

// More slots than live flights (ArrowFlights::kMaxLive = 64): a trail fading
// behind a landed arrow must not take a new one's row.
constexpr uint32_t kArrowTrailSlots = 96;
constexpr uint32_t kArrowTrailPts = 16;
