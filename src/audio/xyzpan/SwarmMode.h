#pragma once
#include "xyzpan/Constants.h"
#include <cmath>

namespace xyzpan {

// Swarm movement mode enumeration.
// Determines how N nodes are positioned relative to the source center.
enum class SwarmMovementMode : int {
    StereoOrbit    = 0,  // existing stereo orbit behavior (2-node compatible)
    CongaLine      = 1,
    Helix          = 2,
    Lissajous      = 3,
    Scatter        = 4,
    Breathe        = 5,
    BrownianDrift  = 6,
    Bounce         = 7,
    kNumModes      = 8
};

// Per-node positional offset from the source center, computed by the movement mode.
struct NodeOffset {
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
};

// Compute N node offsets for StereoOrbit mode.
//
// Reproduces the exact same 2-node behavior as the existing inline orbit code:
//   Node i angle = orbitAngleXY + smoothedOffset + i * (2*PI/N) + smoothedPhase * (i>0 ? 1 : 0)
//   At N=2: node 1 angle = node 0 angle + PI + smoothedPhase (existing behavior exactly)
//
// Parameters:
//   nodeCount        - number of active nodes (1-16)
//   halfSpread       - smoothedWidth * kStereoMaxSpreadRadius
//   spreadX, spreadY - spread direction (face-listener or default X-axis)
//   orbitAngleXY     - orbit LFO angle in XY plane (radians)
//   smoothedOffset   - smoothed offset knob value (radians)
//   smoothedPhase    - smoothed phase knob value (radians, PI + phase for N=2 compat)
//   orbitDepXZ, orbitRawXZ - XZ plane orbit (depth * raw LFO)
//   orbitDepYZ, orbitRawYZ - YZ plane orbit (depth * raw LFO)
//   outOffsets       - output array of nodeCount offsets
//
// For N=2 and default phase=0:
//   node 0 angle = orbitAngleXY + smoothedOffset
//   node 1 angle = orbitAngleXY + smoothedOffset + PI + smoothedPhase
//   This exactly matches the legacy L/R code where blkRPhaseOffset = PI + smoothedPhase.
void computeStereoOrbitOffsets(
    int nodeCount,
    float halfSpread,
    float spreadX, float spreadY,
    float orbitAngleXY,
    float smoothedOffset,
    float smoothedPhase,     // raw smoothed phase (radians), NOT blkRPhaseOffset
    float orbitDepXZ, float orbitRawXZ,
    float orbitDepYZ, float orbitRawYZ,
    NodeOffset* outOffsets);

// Compute N node offsets for block-start positions (uses the same logic as per-sample
// but with block-start LFO values).
// Currently only StereoOrbit is implemented; other modes return zero offsets.
void computeSwarmOffsets(
    SwarmMovementMode mode,
    int nodeCount,
    float halfSpread,
    float spreadX, float spreadY,
    float orbitAngleXY,
    float orbitRawXY,
    float smoothedOffset,
    float smoothedPhase,
    float orbitDepXZ, float orbitRawXZ,
    float orbitDepYZ, float orbitRawYZ,
    float param1, float param2, float param3,
    NodeOffset* outOffsets);

} // namespace xyzpan
