#pragma once
#include "xyzpan/Types.h"

namespace xyzpan {

// Convert XYZ Cartesian coordinates to spherical coordinates.
//
// Coordinate convention (Y-forward):
//   Y=1  = front,  Y=-1 = behind
//   X=1  = right,  X=-1 = left
//   Z=1  = above,  Z=-1 = below
//
// Inputs are NOT clamped — LFO modulation may push coordinates beyond [-1, 1].
// Distance is floored at kMinDistance to prevent division-by-zero.
//
// azimuth   = atan2(X, Y)                -- 0 = front, clockwise positive
// elevation = atan2(Z, sqrt(X*X + Y*Y))  -- true spherical elevation
SphericalCoord toSpherical(float x, float y, float z);

// Compute Euclidean distance from the origin, floored at kMinDistance.
// This function is sample-rate independent — pure math, no time-domain state.
float computeDistance(float x, float y, float z);

} // namespace xyzpan
