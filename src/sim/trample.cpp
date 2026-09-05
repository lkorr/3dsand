#include "sim/trample.h"

#include <algorithm>
#include <cmath>

void TrampleRing::Press(float xVox, float zVox, float yVox, float radiusVox,
                        float strength, float timeSec) {
  if (radiusVox <= 0.0f) return;
  // Standing in a live stamp: refresh it. The match radius is under half the
  // stamp, so a walking presser lays a new disc roughly every half stride and
  // the trail behind it recovers disc by disc rather than as one long slab.
  int best = -1;
  float bestD2 = 1e30f;
  for (uint32_t i = 0; i < count_; i++) {
    TrampleStamp& s = ring_[i];
    if (s.radius <= 0.0f) continue;
    const float dx = xVox - s.x, dz = zVox - s.z;
    const float d2 = dx * dx + dz * dz;
    const float lim = 0.45f * s.radius;
    if (d2 < lim * lim && std::fabs(yVox - s.y) < 2.0f && d2 < bestD2) {
      best = (int)i;
      bestD2 = d2;
    }
  }
  if (best >= 0) {
    TrampleStamp& s = ring_[best];
    s.tEnd = timeSec + kTrampleHoldSec;
    s.strength = std::max(s.strength, strength);
    s.radius = std::max(s.radius, radiusVox);
    return;
  }
  // New stamp: a free slot, else the oldest (smallest tEnd) — the one closest
  // to fully recovered, so overwriting it is the least visible thing to do.
  int slot = -1;
  if (count_ < kTrampleCap) {
    slot = (int)count_++;
  } else {
    float oldest = 1e30f;
    for (uint32_t i = 0; i < kTrampleCap; i++)
      if (ring_[i].tEnd < oldest) { oldest = ring_[i].tEnd; slot = (int)i; }
  }
  TrampleStamp& s = ring_[slot];
  s.x = xVox; s.z = zVox; s.y = yVox;
  s.radius = radiusVox;
  s.t0 = timeSec;
  s.tEnd = timeSec + kTrampleHoldSec;
  s.strength = strength;
}

void TrampleRing::Expire(float timeSec, float recoverSec) {
  // Compact in place so Count() is the live count the shader loops over.
  uint32_t w = 0;
  for (uint32_t i = 0; i < count_; i++) {
    const TrampleStamp& s = ring_[i];
    if (s.radius <= 0.0f) continue;
    if (s.tEnd + recoverSec <= timeSec) continue;
    ring_[w++] = s;
  }
  count_ = w;
}

void TrampleRing::Clear() {
  for (uint32_t i = 0; i < kTrampleCap; i++) ring_[i] = TrampleStamp{};
  count_ = 0;
}

bool TrampleRing::Bounds(float lo[3], float hi[3]) const {
  if (count_ == 0) return false;
  lo[0] = lo[1] = lo[2] = 1e30f;
  hi[0] = hi[1] = hi[2] = -1e30f;
  for (uint32_t i = 0; i < count_; i++) {
    const TrampleStamp& s = ring_[i];
    lo[0] = std::min(lo[0], s.x - s.radius);
    hi[0] = std::max(hi[0], s.x + s.radius);
    lo[2] = std::min(lo[2], s.z - s.radius);
    hi[2] = std::max(hi[2], s.z + s.radius);
    // The y band trampleAt accepts around a stamp's ground level.
    lo[1] = std::min(lo[1], s.y - 1.5f);
    hi[1] = std::max(hi[1], s.y + 3.0f);
  }
  return true;
}

TrampleRing& Tramples() {
  static TrampleRing s;
  return s;
}
