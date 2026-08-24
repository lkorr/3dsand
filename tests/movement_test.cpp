// Standalone player-movement harness: exercises the controller against a
// synthetic noisy heightfield, with no GPU and no world storage. This is the
// test the GPU selftest's walk check does not cover — that one is a bare
// drop-and-land onto smooth terrain, which passes even when crossing rough
// ground is completely broken.
//
// Build:  see tests/CMakeLists.txt  (target: movement_test)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

#include "game/player.h"

namespace {

// Deterministic value noise, CPU-only — this harness never touches the sim, so
// it is free to use floats without engaging the bit-determinism rules.
uint32_t Hash2(int x, int z, uint32_t seed) {
  uint32_t h = seed;
  h ^= (uint32_t)x * 0x9E3779B9u; h = (h ^ (h >> 15)) * 0x85EBCA6Bu;
  h ^= (uint32_t)z * 0xC2B2AE35u; h = (h ^ (h >> 13)) * 0xC2B2AE35u;
  return h ^ (h >> 16);
}

// A floor at `base` with coherent noise of `amp` voxels on top: the "slightly
// noisy ground" the controller is supposed to walk over smoothly.
//
// The noise is value noise interpolated over `feature` voxels, NOT independent
// per-column noise. That distinction is the whole test: the player AABB is
// 2*kHalfXZ voxels wide (4.8 at 0.125 m) and rests on the TALLEST column under
// its footprint, so white noise finer than the body is silently flattened —
// the footprint maximum is the peak nearly everywhere and the body glides
// along a constant height, scoring 100% while exercising nothing. Bumps have
// to be comparable to or wider than the body to be bumps at all.
struct NoisyFloor {
  int base = 40;
  int amp = 0;
  int feature = 8;  // voxels per noise cell
  uint32_t seed = 12345;

  float ValueAt(int x, int z) const {
    auto grad = [&](int cx, int cz) {
      return (float)(Hash2(cx, cz, seed) & 0xFFFF) / 65535.0f;
    };
    float fx = (float)x / (float)feature, fz = (float)z / (float)feature;
    int x0 = ifloor(fx), z0 = ifloor(fz);
    float tx = fx - (float)x0, tz = fz - (float)z0;
    // smoothstep for C1 continuity: no creases along cell borders
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    float a = grad(x0, z0), b = grad(x0 + 1, z0);
    float c = grad(x0, z0 + 1), d = grad(x0 + 1, z0 + 1);
    return (a + (b - a) * tx) * (1.0f - tz) + (c + (d - c) * tx) * tz;
  }
  // grit: independent per-column jitter added on top of the coherent shape.
  // This is what settled sand and gravel actually look like — a smooth bulk
  // form with a sharp, uncorrelated 1-2 voxel crust. It is the part of the
  // terrain that catches an AABB, and pure interpolated noise does not have it.
  int grit = 0;

  int HeightAt(int x, int z) const {
    int h = base;
    if (amp > 0) h += (int)(ValueAt(x, z) * (float)amp + 0.5f);
    if (grit > 0) h += (int)(Hash2(x ^ 0x5bd1, z ^ 0x9e37, seed + 77u) %
                             (uint32_t)(grit + 1));
    return h;
  }
  CellKind operator()(IVec3 c) const {
    if (c.y < 0) return CellKind::Solid;
    return c.y < HeightAt(c.x, c.z) ? CellKind::Solid : CellKind::Air;
  }
};

struct Result {
  float distance = 0;    // voxels travelled along +X
  int jumps = 0;         // jumps that actually left the ground
  int stuckFrames = 0;   // frames with ~zero horizontal progress
  float groundedFrac = 0;
  float yRange = 0;      // spread of feet height over the run
};

// Run `frames` of holding forward, optionally mashing jump every `jumpEvery`
// frames. Returns distance covered and how often the body got stuck.
Result Run(const NoisyFloor& floor, int frames, int jumpEvery) {
  Player p;
  p.fly = false;

  // Spawn clear of the terrain and let gravity settle the body. Seeding from a
  // single column's height is wrong: the AABB is 2*kHalfXZ voxels wide (4.8 at
  // 0.125 m) and rests on the TALLEST column under its whole footprint, so a
  // single-column spawn can start the body embedded in a neighbouring pillar —
  // which reads as "the controller is stuck" when really the test never gave
  // it a legal starting position. Drop from above the footprint maximum.
  const float startX = 8.5f, startZ = 8.5f;
  int top = 0;
  for (int z = ifloor(startZ - Player::kHalfXZ); z <= ifloor(startZ + Player::kHalfXZ); z++)
    for (int x = ifloor(startX - Player::kHalfXZ); x <= ifloor(startX + Player::kHalfXZ); x++)
      top = std::max(top, floor.HeightAt(x, z));
  p.pos = Vec3{startX, (float)top + Player::kHalfY + 2.0f, startZ};

  Player::KindFn kindAt = [&](IVec3 c) { return floor(c); };
  const float dt = 1.0f / 60.0f;

  // Settle onto the ground first so the run starts from rest.
  for (int i = 0; i < 60; i++)
    p.Update(dt, PlayerInput{}, Vec3{1, 0, 0}, Vec3{0, 0, 1}, Vec3{1, 0, 0}, kindAt);
  if (!p.grounded) std::printf("  [warn] body never settled before the run\n");

  Result r;
  float originX = p.pos.x;
  float prevX = p.pos.x;
  float peakY = p.pos.y;
  bool airborne = false;
  int groundedFrames = 0;
  float yMin = p.pos.y, yMax = p.pos.y;

  for (int i = 0; i < frames; i++) {
    PlayerInput in{};
    in.forward = 1.0f;
    if (jumpEvery > 0 && i % jumpEvery == 0) in.jumpPressed = true;

    float beforeY = p.pos.y;
    p.Update(dt, in, Vec3{1, 0, 0}, Vec3{0, 0, 1}, Vec3{1, 0, 0}, kindAt);

    // Count a jump only when the body clearly rises off the surface.
    if (!airborne && p.vel.y > 0.5f / kVoxelMeters) {
      airborne = true;
      peakY = beforeY;
    } else if (airborne) {
      peakY = std::fmax(peakY, p.pos.y);
      if (p.grounded && p.vel.y <= 0.0f) {
        if (peakY - beforeY > 0.25f / kVoxelMeters) r.jumps++;
        airborne = false;
      }
    }

    if (p.grounded) groundedFrames++;
    yMin = std::fmin(yMin, p.pos.y);
    yMax = std::fmax(yMax, p.pos.y);
    float advanced = p.pos.x - prevX;
    if (advanced < 0.01f) r.stuckFrames++;
    prevX = p.pos.x;
  }
  r.distance = p.pos.x - originX;
  r.groundedFrac = (float)groundedFrames / (float)frames;
  r.yRange = yMax - yMin;
  return r;
}

// ---- impact latch (fall / wall-slam damage) --------------------------------
//
// Player::impactDeltaV is written once per FRAME but consumed inside the fixed
// 30 Hz tick loop, which runs zero times on any frame where the accumulator has
// not reached a whole tick. An impact is a SINGLE-frame event, so this harness
// has to reproduce that interleave or it proves nothing: at 60 fps a tick runs
// every other frame, and a landing that happens on an undrained frame is
// overwritten by the next frame's ~0 before any tick can read it.
//
// That is exactly the bug this covers — fall damage that never fired. Sweeping
// the tick PHASE is the point: under a plain per-frame assignment roughly half
// these phases report zero, and the ones that pass make it look merely flaky.
constexpr float kSimTickDt = 1.0f / 30.0f;  // main.cpp's fixed tick

struct ImpactRun {
  float observedMs = 0;  // biggest impact any TICK actually got to read, m/s
  float airborneMs = 0;  // ...restricted to ticks before the body landed
  bool landed = false;
};

// Drop the body `dropVox` voxels onto `floor` and return the hardest impact a
// tick observed, draining the latch exactly the way main.cpp does.
// `phase` (0..1) offsets the tick accumulator so the landing falls at a
// different point in the tick cycle each run.
template <class Floor>
ImpactRun DropImpact(const Floor& floor, float dropVox, float phase,
                     float horizVelVox = 0.0f) {
  Player p;
  p.fly = false;
  const float startX = 8.5f, startZ = 8.5f;
  int top = 0;
  for (int z = ifloor(startZ - Player::kHalfXZ); z <= ifloor(startZ + Player::kHalfXZ); z++)
    for (int x = ifloor(startX - Player::kHalfXZ); x <= ifloor(startX + Player::kHalfXZ); x++)
      top = std::max(top, floor.HeightAt(x, z));
  p.pos = Vec3{startX, (float)top + Player::kHalfY + dropVox, startZ};
  p.vel = Vec3{horizVelVox, 0, 0};

  Player::KindFn kindAt = [&](IVec3 c) { return floor(c); };
  const float dt = 1.0f / 60.0f;

  ImpactRun r;
  double acc = (double)phase * (double)kSimTickDt;
  int after = 0;
  for (int i = 0; i < 4000; i++) {
    PlayerInput in{};
    p.Update(dt, in, Vec3{1, 0, 0}, Vec3{0, 0, 1}, Vec3{1, 0, 0}, kindAt);
    const bool airborneNow = !p.grounded;
    // The fixed-tick loop, verbatim: consume-and-clear on the first tick of the
    // frame batch that sees the latch.
    acc += (double)dt;
    while (acc >= (double)kSimTickDt) {
      acc -= (double)kSimTickDt;
      const float ms = p.impactDeltaV.len() * kVoxelMeters;
      r.observedMs = std::fmax(r.observedMs, ms);
      if (airborneNow) r.airborneMs = std::fmax(r.airborneMs, ms);
      p.impactDeltaV = Vec3{0, 0, 0};
    }
    if (p.grounded) r.landed = true;
    if (r.landed && ++after > 8) break;
  }
  return r;
}

// Free-fall speed after `dropVox` voxels, in m/s, capped at terminal velocity.
float PredictedImpactMs(float dropVox) {
  const float g = 9.81f;  // tuning.json player.gravity default
  const float v = std::sqrt(2.0f * g * dropVox * kVoxelMeters);
  return std::fmin(v, 30.0f);  // player.maxFall default
}

// Hardest impact any tick sees while the body just STANDS there. Peak-hold has
// to stay a peak: gravity contributes a small cancelled velocity every grounded
// frame, and accumulating it instead would reach the lethal threshold in about
// a second of standing still.
float IdleMaxImpactMs(const NoisyFloor& floor, int frames) {
  Player p;
  p.fly = false;
  const float startX = 8.5f, startZ = 8.5f;
  int top = 0;
  for (int z = ifloor(startZ - Player::kHalfXZ); z <= ifloor(startZ + Player::kHalfXZ); z++)
    for (int x = ifloor(startX - Player::kHalfXZ); x <= ifloor(startX + Player::kHalfXZ); x++)
      top = std::max(top, floor.HeightAt(x, z));
  p.pos = Vec3{startX, (float)top + Player::kHalfY + 0.05f, startZ};
  Player::KindFn kindAt = [&](IVec3 c) { return floor(c); };
  const float dt = 1.0f / 60.0f;
  double acc = 0;
  float worst = 0;
  for (int i = 0; i < frames; i++) {
    p.Update(dt, PlayerInput{}, Vec3{1, 0, 0}, Vec3{0, 0, 1}, Vec3{1, 0, 0}, kindAt);
    acc += (double)dt;
    while (acc >= (double)kSimTickDt) {
      acc -= (double)kSimTickDt;
      // Ignore the initial settle drop; only steady-state standing counts.
      if (i > 30)
        worst = std::fmax(worst, p.impactDeltaV.len() * kVoxelMeters);
      p.impactDeltaV = Vec3{0, 0, 0};
    }
  }
  return worst;
}

// A floor with a solid wall standing at x >= wallX, for horizontal slams.
struct WalledFloor {
  NoisyFloor floor;
  int wallX = 40;
  int HeightAt(int x, int z) const { return floor.HeightAt(x, z); }
  CellKind operator()(IVec3 c) const {
    if (c.x >= wallX && c.y >= 0 && c.y < floor.base + 60) return CellKind::Solid;
    return floor(c);
  }
};

}  // namespace

int main() {
  const int kFrames = 600;  // 10 s at 60 Hz
  // Free-run reference: how far walking covers over 10 s on a flat floor.
  bool allPass = true;

  std::printf("=== player movement over noisy ground ===\n");
  std::printf("voxel size %.3f m, step budget %.2f m (%d voxels)\n\n",
              (double)kVoxelMeters, (double)Player::kStepUpM,
              Player::kMaxStepUpVoxels);

  NoisyFloor flat{40, 0, 8, 1};
  Result ref = Run(flat, kFrames, 0);
  std::printf("flat floor      : %6.1f vox  (reference)\n", (double)ref.distance);

  // Walking: noisy ground must not cost much distance versus flat ground.
  std::printf("\n-- walking (hold forward, no jump) --\n");
  for (int amp : {1, 2, 3, 4, 6}) {
    NoisyFloor f{40, amp, 6, (uint32_t)(amp * 7919 + 13)};
    Result r = Run(f, kFrames, 0);
    float frac = r.distance / ref.distance;
    // Rough ground should cost some speed, but crossing it must not collapse.
    // yRange guards against the body gliding along at a constant height above
    // the noise instead of actually walking the surface — that looks like a
    // perfect score while testing nothing.
    bool ok = frac > 0.55f && r.yRange > 0.5f;
    allPass &= ok;
    std::printf("noise +/-%d vox  : %6.1f vox  (%3.0f%% of flat, stuck %2.0f%% of "
                "frames, grounded %3.0f%%, dy %4.1f vox)  %s\n",
                amp, (double)r.distance, (double)(frac * 100.0f),
                (double)(100.0f * r.stuckFrames / kFrames),
                (double)(r.groundedFrac * 100.0f), (double)r.yRange,
                ok ? "PASS" : "FAIL");
  }

  // Jumping while moving — the original complaint. The press interval must be
  // longer than the airtime of a jump, or most presses land mid-flight and are
  // correctly ignored: at these settings a jump is v0=42 vox/s against
  // g=78.5 vox/s^2, so 2*v0/g = 1.07 s = ~64 frames airborne. Pressing every
  // 75 frames gives 8 attempts that can each actually be taken. The bar is
  // that noisy ground costs no jumps relative to flat ground.
  constexpr int kJumpEvery = 75;
  const int attempts = kFrames / kJumpEvery;
  std::printf("\n-- jumping while moving (jump every %.2f s, %d attempts) --\n",
              (double)kJumpEvery / 60.0, attempts);
  Result flatJump = Run(flat, kFrames, kJumpEvery);
  std::printf("flat floor      : %2d/%d jumps landed  (reference)\n",
              flatJump.jumps, attempts);
  for (int amp : {1, 2, 3, 4, 6}) {
    NoisyFloor f{40, amp, 6, (uint32_t)(amp * 7919 + 13)};
    Result r = Run(f, kFrames, kJumpEvery);
    // Rough ground must not swallow jumps that flat ground honours. One miss
    // is tolerated on the roughest floors: a press on a fixed schedule can
    // genuinely land while the body is airborne off a large bump, and refusing
    // that press is correct behaviour, not a swallowed jump.
    bool ok = r.jumps >= flatJump.jumps - 1;
    allPass &= ok;
    std::printf("noise +/-%d vox  : %2d/%d jumps landed, travelled %6.1f vox  %s\n",
                amp, r.jumps, attempts, (double)r.distance, ok ? "PASS" : "FAIL");
  }

  // Gritty ground: a rolling shape with a sharp uncorrelated crust on top —
  // settled sand/gravel, and the terrain the complaint was actually about.
  std::printf("\n-- walking over gritty ground (coherent shape + sharp crust) --\n");
  for (int grit : {1, 2, 3}) {
    NoisyFloor f{40, 3, 6, (uint32_t)(grit * 5147 + 91)};
    f.grit = grit;
    Result r = Run(f, kFrames, 0);
    float frac = r.distance / ref.distance;
    bool ok = frac > 0.55f;
    allPass &= ok;
    std::printf("crust +%d vox   : %6.1f vox  (%3.0f%% of flat, stuck %2.0f%% of "
                "frames, grounded %3.0f%%)  %s\n",
                grit, (double)r.distance, (double)(frac * 100.0f),
                (double)(100.0f * r.stuckFrames / kFrames),
                (double)(r.groundedFrac * 100.0f), ok ? "PASS" : "FAIL");
  }
  std::printf("\n-- jumping over gritty ground --\n");
  for (int grit : {1, 2, 3}) {
    NoisyFloor f{40, 3, 6, (uint32_t)(grit * 5147 + 91)};
    f.grit = grit;
    Result r = Run(f, kFrames, kJumpEvery);
    bool ok = r.jumps >= flatJump.jumps - 1;
    allPass &= ok;
    std::printf("crust +%d vox   : %2d/%d jumps landed, travelled %6.1f vox  %s\n",
                grit, r.jumps, attempts, (double)r.distance, ok ? "PASS" : "FAIL");
  }

  // ---- impact latch ------------------------------------------------------
  // An impact is a one-frame event consumed by a 30 Hz tick loop that runs on
  // roughly half of 60 fps frames. The tick PHASE sweep is the load-bearing
  // part: a per-frame assignment passes at some phases and silently reports
  // nothing at the others, which is precisely how fall damage came to never
  // fire while looking like a threshold problem.
  std::printf("\n-- impact latch survives the frame/tick interleave --\n");
  std::printf("(60 fps frames, %g Hz tick: most frames drain nothing)\n",
              (double)(1.0f / kSimTickDt));
  {
    const float drop = 200.0f;  // 20 m
    const float want = PredictedImpactMs(drop);
    for (int k = 0; k < 6; k++) {
      const float phase = (float)k / 6.0f;
      ImpactRun r = DropImpact(flat, drop, phase);
      // Generous band: the body gains one more frame of gravity than the
      // analytic value and the sweep can stop it a substep early.
      bool ok = r.landed && r.observedMs > want * 0.85f &&
                r.observedMs < want * 1.20f;
      allPass &= ok;
      std::printf("tick phase %.2f : impact seen %5.1f m/s  (predicted %5.1f)  %s\n",
                  (double)phase, (double)r.observedMs, (double)want,
                  ok ? "PASS" : "FAIL");
    }
  }

  std::printf("\n-- impact scales with drop height --\n");
  for (float drop : {20.0f, 60.0f, 200.0f, 500.0f}) {
    const float want = PredictedImpactMs(drop);
    ImpactRun r = DropImpact(flat, drop, 0.5f);
    bool ok = r.landed && r.observedMs > want * 0.85f &&
              r.observedMs < want * 1.20f;
    allPass &= ok;
    std::printf("drop %5.1f m    : impact seen %5.1f m/s  (predicted %5.1f)  %s\n",
                (double)(drop * kVoxelMeters), (double)r.observedMs,
                (double)want, ok ? "PASS" : "FAIL");
  }

  std::printf("\n-- standing still latches no impact (peak-hold, not sum) --\n");
  {
    const float idle = IdleMaxImpactMs(flat, 600);
    // Well under player.fallDamageSpeed (8 m/s default). Summing the per-frame
    // gravity cancellation instead would blow past it within a second.
    bool ok = idle < 1.0f;
    allPass &= ok;
    std::printf("10 s idle       : worst latch %5.2f m/s  (onset 8.0)  %s\n",
                (double)idle, ok ? "PASS" : "FAIL");
  }

  std::printf("\n-- horizontal slam into a wall registers --\n");
  {
    WalledFloor w;
    w.floor = flat;
    w.wallX = 40;
    // Launched sideways at 30 m/s from high enough to still be airborne on
    // contact: this is the case landing detection can never see, and the whole
    // reason impact is measured as velocity the sweep refused.
    ImpactRun r = DropImpact(w, 300.0f, 0.5f, 30.0f / kVoxelMeters);
    bool ok = r.airborneMs > 15.0f;
    allPass &= ok;
    std::printf("30 m/s into wall: airborne impact %5.1f m/s  %s\n",
                (double)r.airborneMs, ok ? "PASS" : "FAIL");
  }

  std::printf("\n=== movement_test %s ===\n", allPass ? "PASS" : "FAIL");
  return allPass ? 0 : 1;
}
