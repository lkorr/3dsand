#pragma once
#include <cstdint>

#include "sim/world.h"

// ---- THE TRAMPLE RING (render-only; DESIGN.md §9 "Analytic plants") --------
//
// A plant flattens under a foot and springs back after it. The plants are
// render-only geometry reconstructed per cell from hashes (microvox.h), so the
// flattening has to be render-only too: a bounded ring of footprint STAMPS
// rides RenderParams exactly the way the wave-impact ring does, and every
// plant reads it at its base through `trampleAt` in common.wgsl. No sim
// kernel sees it, it is not hashed, not saved, and a full ring overwrites its
// oldest entry — which is what a faded footprint is.
//
// A stamp is a disc on the ground: (x, z) and the ground y under the presser,
// a radius, the time it landed (t0) and the time it was last pressed (tEnd).
// A presser standing still keeps ONE stamp alive by refreshing tEnd; a presser
// walking lays a fresh stamp every ~half radius, so the trail behind it is a
// row of discs each recovering on its own clock. Strength scales the flatten
// amount (a rabbit does not lay the grass as flat as the player).
struct TrampleStamp {
  float x = 0.0f, z = 0.0f;   // world voxels
  float y = 0.0f;             // ground under the presser, world voxels
  float radius = 0.0f;        // world voxels; <= 0 is a dead slot
  float t0 = 0.0f;            // R.time it landed
  float tEnd = 0.0f;          // R.time it was last pressed (held until then)
  float strength = 1.0f;      // 0..1
};

class TrampleRing {
 public:
  // One call per presser per frame. Refreshes the stamp the presser is
  // standing in, or lays a new one when it has moved far enough from it.
  void Press(float xVox, float zVox, float yVox, float radiusVox,
             float strength, float timeSec);
  // Drop stamps that have fully recovered (tEnd + recover < now) so the
  // shader's loop only walks live ones.
  void Expire(float timeSec, float recoverSec);
  void Clear();
  const TrampleStamp* Data() const { return ring_; }
  uint32_t Count() const { return count_; }
  // Union AABB of the live stamps (xz discs, y band) for the shader's early
  // reject. False when empty.
  bool Bounds(float lo[3], float hi[3]) const;

 private:
  TrampleStamp ring_[kTrampleCap];
  uint32_t count_ = 0;
};

// THE live ring — a global for exactly the reason WaveImpacts() is: it has to
// reach the frame loop (the pressers), WriteRenderParams (the reader) and any
// gate that wants to plant a foot. Headless paths leave it empty, and an empty
// ring is an exact identity in the shader.
TrampleRing& Tramples();

// A stamp is held this long past its last press before it starts recovering,
// so a frame hitch never lets the grass twitch up between two footfalls.
constexpr float kTrampleHoldSec = 0.12f;
