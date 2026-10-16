#pragma once
// steer.h — locomotion stage 2, STEER, as a pure function
// (docs/PLAN_distant_npcs.md §2.5, audit A10).
//
// WHY A FREE FUNCTION. `MobSystem::Steer` closed a creature's heading onto its
// desired heading at a bounded, ramped rate — and it did it by reading and
// writing `Mob` fields (heading_, desiredHeading_, turnVel_, speedNow_, the
// one-shot turn caps) and calling `Mob::ShieldTurnRate`. The distant tier
// (game/distant.h) has no `Mob` for a creature past the window, only a
// `DistantMob`, and it must turn EXACTLY as the near tier does or a creature
// that crosses the edge changes how it moves. So the arithmetic lives here,
// over the four numbers it actually touches, and both tiers call it:
//
//   near     MobSystem::Steer copies the Mob's fields into a SteerState, calls
//            SteerStep, copies them back (mob.cpp);
//   distant  the distant drive (D1) owns its SteerState outright.
//
// BIT-IDENTICAL TO THE CODE IT REPLACED, and that is a gated claim, not a
// hope: `distant-roundtrip`'s steer arm keeps a verbatim copy of the old
// Mob-field arithmetic and compares it to SteerStep over 100k randomized
// states, every output bit. The one restatement is the shield: the old code
// called `ShieldTurnRate(freeRate)`, which is `freeRate` with no shield up and
// `min(freeRate, cap)` with one; here it is always `min(freeRate,
// shieldRateCap)` with `shieldRateCap = +inf` for "no shield", which is the
// same float for every input including NaN (std::min returns its first
// argument unless the second compares less).
//
// Deterministic by construction: no state outside the arguments, no RNG,
// float ops in a fixed order in ONE translation unit (steer.cpp), so the near
// and the distant callers run the same compiled body.

#include "game/anim.h"   // LocomotionDef

// What STEER reads and writes, and nothing else.
struct SteerState {
  // The ACTUATED facing, radians, heading 0 = +Z (CLAUDE.md conventions).
  // Kept normalized to [-pi, pi] by SteerStep.
  float heading = 0.0f;
  // The target the behaviour wrote. Deliberately NOT normalized (it is a
  // target; SteerStep's remainder() handles the wrap).
  float desiredHeading = 0.0f;
  // Yaw rate, radians/sec. Ramped by LocomotionDef::turnAccel.
  float turnVel = 0.0f;
  // Measured planar speed, voxels/sec (Mob::speedNow_). READ only here: it
  // sets how wide the turn is (turnRateMoving), the drive measures it.
  float speedNow = 0.0f;
};

// One tick of steering. `defSpeed` is the body's full walking speed
// (MobDef::speed, voxels/sec); `shieldRateCap` the yaw-rate ceiling a raised
// shield imposes (Mob::ShieldTurnCap, +inf with none); `capMulL` / `capMulR`
// the one-shot left / right multipliers a caller set for this call (a swimmer
// that lost a fin; 1 = none; + turnVel is a turn to the LEFT). Returns the
// forward-drive alignment factor in 0..1: full drive within
// lo.driveAlignFull of the target, zero past lo.driveAlignZero.
float SteerStep(SteerState& s, const LocomotionDef& lo, float defSpeed,
                float shieldRateCap, float capMulL, float capMulR, float dt);
