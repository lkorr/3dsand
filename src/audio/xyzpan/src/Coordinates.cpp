#include "xyzpan/Coordinates.h"
#include "xyzpan/Constants.h"
#include <cmath>
#include <algorithm>

namespace xyzpan {

SphericalCoord toSpherical(float x, float y, float z) {
    // Compute distance first (needed for azimuth guard)
    const float dist = computeDistance(x, y, z);

    // Elevation: atan2(Z, sqrt(X^2 + Y^2)) — true spherical elevation
    const float horizontalMag = std::sqrt(x * x + y * y);
    const float elevation = std::atan2(z, horizontalMag);

    // Azimuth: atan2(X, Y) — Y-forward, clockwise = positive
    // At origin (dist <= kMinDistance), azimuth defaults to 0 (front) by convention.
    const float azimuth = (dist > kMinDistance) ? std::atan2(x, y) : 0.0f;

    return SphericalCoord{ azimuth, elevation, dist };
}

float computeDistance(float x, float y, float z) {
    const float raw = std::sqrt(x * x + y * y + z * z);
    return std::max(raw, kMinDistance);
}

} // namespace xyzpan
