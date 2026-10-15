// steer.cpp — locomotion stage 2, STEER (game/steer.h).
//
// The body below is MobSystem::Steer's arithmetic as it stood on 2026-10-15
// (mob.cpp, "locomotion stage 2: steer"), moved here line for line with the
// Mob fields renamed onto SteerState. Do not "tidy" an expression without
// re-running `distant-roundtrip`: its steer arm holds the old code verbatim
// and compares every output bit, and the near crowd's `mob-cap64-twice`
// digest is downstream of every float here.
#include "game/steer.h"

#include <algorithm>
#include <cmath>

float SteerStep(SteerState& s, const LocomotionDef& lo, float defSpeed,
                float shieldRateCap, float capMulL, float capMulR, float dt) {
  // Shortest signed error, wrapped into [-pi, pi]. Doing this with remainder
  // rather than a while-loop matters: a desiredHeading that has accumulated
  // many turns (it is never normalized) would otherwise spin here.
  float err = std::remainder(s.desiredHeading - s.heading, 6.2831853f);

  // Turn tighter when slow, wider when fast — a physical body's trade.
  const float speedFrac =
      std::clamp(s.speedNow / std::max(defSpeed, 0.01f), 0.0f, 1.0f);
  // A RAISED SHIELD SLOWS THE TURN (item.h ShieldSpec::turnRate): the
  // shield faces where the body faces, so a fighter who circles a guard
  // outpaces it -- the counter to standing behind one. (+inf = no shield up;
  // see steer.h for why min() against it is the old ShieldTurnRate exactly.)
  const float rateCap = std::min(
      lo.turnRate * (1.0f + (lo.turnRateMoving - 1.0f) * speedFrac),
      shieldRateCap);
  // ...AND MAY BE SLOWER ONE WAY THAN THE OTHER: a swimmer that has lost the
  // fin a turn pivots on (MobDef::SwimDef::FinDef). One-shot multipliers the
  // caller set for this call only; + turnVel is a turn to the left.
  const float capL = rateCap * capMulL, capR = rateCap * capMulR;

  if (rateCap <= 0.0f) {
    s.turnVel = 0;
  } else if (lo.turnAccel <= 0.0f) {
    // No ramp: step straight to the capped rate.
    s.turnVel = std::clamp(err / std::max(dt, 1e-4f), -capR, capL);
  } else {
    // Ramp toward the rate that would arrive on target, but never past the cap.
    // The sqrt term is a critically-damped arrival: it is the fastest rate from
    // which the remaining error can still be bled off at turnAccel without
    // overshooting, so the mob decelerates INTO its heading instead of ringing
    // around it.
    const float arrive =
        std::sqrt(2.0f * lo.turnAccel * std::abs(err)) * (err < 0 ? -1.0f : 1.0f);
    const float want = std::clamp(arrive, -capR, capL);
    const float dv = lo.turnAccel * dt;
    s.turnVel += std::clamp(want - s.turnVel, -dv, dv);
  }

  // Never step past the target within one tick: at low dt the ramp handles it,
  // but a large dt (a hitch) would otherwise overshoot and oscillate.
  float step = s.turnVel * dt;
  if (std::abs(step) > std::abs(err)) {
    step = err;
    s.turnVel = err / std::max(dt, 1e-4f);
  }
  s.heading += step;
  // Keep the ACTUATED heading normalized. Intent is left un-normalized on
  // purpose (it is a target, and remainder() above handles the wrap), but
  // `heading` feeds sin/cos every tick for the whole life of the mob and would
  // slowly lose float precision if it drifted to large magnitudes.
  s.heading = std::remainder(s.heading, 6.2831853f);

  // Alignment: full drive within driveAlignFull, zero past driveAlignZero.
  const float e = std::abs(std::remainder(s.desiredHeading - s.heading,
                                          6.2831853f));
  if (e <= lo.driveAlignFull) return 1.0f;
  if (e >= lo.driveAlignZero) return 0.0f;
  return 1.0f - (e - lo.driveAlignFull) /
                    (lo.driveAlignZero - lo.driveAlignFull);
}
