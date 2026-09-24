#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "game/mob.h"
#include "math3d.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/tuning.h"

// HOLD E TO DRAG A BODY — Elder Scrolls / Fallout item manipulation.
//
// TAP E is unchanged: pick the item up, open the corpse. HOLD E past
// player.grabHoldTime and whatever the crosshair is on comes off the floor and
// rides in front of the face until E is let go. The two cannot be confused
// because the pickup fires on RELEASE and only when the hold never latched.
//
// A VELOCITY SERVO, NOT A JOINT. Every tick the body's linear velocity is set
// to whatever would close the gap to the carry point, capped. Three reasons it
// is not a Fixed joint to the player proxy:
//   * a stiff constraint between the proxy and a two-tonne block is the same
//     energy-gaining configuration the armoured corpse blow-up was
//     (phys/debris.h StrapBody) — the solver would fight it every step;
//   * the cap IS the weight. A heavy thing simply cannot be moved at more than
//     a crawl, so it lags behind the crosshair, swings wide when you turn, and
//     catches up when you stop. Nothing simulates that: it falls out of one
//     clamp;
//   * overwriting velocity each tick cancels gravity for free, which is what
//     "held" means, and the body still COLLIDES — it scrapes walls, knocks
//     other debris about and cannot be pushed through terrain, because Jolt
//     still solves the step it was given a velocity for.
//
// The body is moved onto the no-player-contact layer while held. It is riding
// inside arm's reach with the player walking into it; on the normal layer that
// is a permanent deep overlap, and PlayerPushOut turns a permanent deep
// overlap into the player being shoved across the field (phys/physics.h
// ReleaseToWorldWhenClear, which is also how it gets back).
//
// NOT SIM STATE. Bodies are CPU float gameplay outside the hashed grid, and
// this only ever writes velocities; nothing here can reach the world hash
// except through the ops a body already emits.
class GrabHold {
 public:
  bool Active() const { return body_ != 0; }
  uint64_t Body() const { return body_; }
  float MassKg() const { return massKg_; }
  // True when the last Begin() was refused for weight. Consumed by the caller
  // for its one-line message; there is nothing else to report.
  bool RefusedTooHeavy() const { return refusedHeavy_; }

  // ---- WHAT CAN BE PICKED UP -------------------------------------------
  //
  // A body this DebrisSystem owns, and that is the whole test. It is the
  // positive form on purpose: loose debris, dropped items, corpses, severed
  // limbs and blast rubble are all debris bodies, while a LIVING creature's
  // limbs belong to MobSystem and are driven kinematically every tick — a
  // servo pulling on one would be arguing with its owner's animation, and the
  // owner wins. Enumerating what to refuse would have to be re-checked every
  // time something new learns to be a rigid body; asking who owns it does not.
  //
  // A WORN SHELL REDIRECTS TO WHAT IT IS WORN ON. A corpse's cuirass is a
  // follower with no dynamics of its own (StrapBody), so dragging it would
  // move nothing at all: the host is what has to be dragged, and the plate
  // comes with it.
  //
  // ...AND A CORPSE IS A DEAD MOB NOW (docs/PLAN_corpse_is_a_mob.md). Its limbs
  // are MobSystem's, but they are dynamic for good and nothing re-poses them,
  // which is the whole of why a living limb is refused: there is no owner
  // animation to argue with. So a dead Mob's limb is grabbable too, through
  // the same shell-to-host redirect (MobSystem::GrabbableDeadLimb); dragging
  // one limb drags the rest of the body on its joints.
  static uint64_t Grabbable(const DebrisSystem& debris, const MobSystem* mobs,
                            uint64_t body) {
    if (!body) return 0;
    if (mobs != nullptr)
      if (const uint64_t limb = mobs->GrabbableDeadLimb(body)) return limb;
    if (const uint64_t host = debris.WornHostOf(body)) body = host;
    return debris.HasBody(body) ? body : 0;
  }

  // Take `body` (already resolved through Grabbable). False when it is dead or
  // over player.grabMaxMass, in which case RefusedTooHeavy() says which.
  bool Begin(Physics& phys, uint64_t body, const Tuning::Player& tp,
             Vec3 handVoxel) {
    refusedHeavy_ = false;
    if (!body) return false;
    Vec3 com;
    if (!phys.BodyCenterOfMass(body, com)) return false;
    const float m = phys.BodyMass(body);
    if (tp.grabMaxMass > 0.0f && m > tp.grabMaxMass) {
      refusedHeavy_ = true;
      return false;
    }
    if (Active()) Release(phys);
    body_ = body;
    massKg_ = m;
    // IT STAYS WHERE IT WAS GRABBED. Latching the distance it is already at
    // (clamped to the reach) rather than snapping it to a fixed carry range is
    // the difference between lifting a crate and having it teleport into your
    // face — and a thing grabbed at arm's length while you are bent over a
    // table has to stay on the table.
    const float reach = tp.grabDistance / kVoxelMeters;
    dist_ = std::clamp((com - handVoxel).len(), 0.35f * reach, reach);
    phys.SetBodyAvatarLayer(body_, true);
    phys.ActivateBody(body_);
    return true;
  }

  // Let go. The body keeps the velocity the servo last gave it, which is what
  // makes a walked-along crate carry on into the wall you shoved it at.
  void Release(Physics& phys) {
    if (!body_) return;
    phys.ReleaseToWorldWhenClear(body_);
    phys.ActivateBody(body_);
    body_ = 0;
    massKg_ = 0.0f;
  }

  // The body vanished under us (burnt away, culled, or its collider rebuilt
  // under a new handle — phys/debris.h BodyHandle). No layer restore: there is
  // nothing left to restore it on.
  void Forget() {
    body_ = 0;
    massKg_ = 0.0f;
  }

  // ONE PHYSICS TICK of the servo. Call immediately before Physics::Step, with
  // the same dt that Step is about to be given.
  void Tick(Physics& phys, const DebrisSystem& debris, const MobSystem* mobs,
            const Tuning::Player& tp, Vec3 handVoxel, Vec3 fwd, float dt) {
    if (!body_ || dt <= 0.0f) return;
    // Still a body something holds: debris, or a dead Mob's limb (a carve
    // rebuilds a limb's collider under a NEW handle, and then this one is
    // simply gone, exactly like a debris body re-made by a shatter).
    if (!debris.HasBody(body_) &&
        !(mobs != nullptr && mobs->GrabbableDeadLimb(body_) == body_)) {
      Forget();
      return;
    }
    Vec3 com;
    if (!phys.BodyCenterOfMass(body_, com)) {
      Forget();
      return;
    }
    massKg_ = phys.BodyMass(body_);
    const Vec3 target = handVoxel + fwd * dist_;
    const Vec3 err = target - com;
    // LET GO WHEN IT IS NO LONGER WITH YOU. The gap grows when the thing is
    // wedged, caught on a doorframe, or too heavy to follow a sprint — all of
    // which should end with it left behind rather than dragged through the
    // wall on the next servo step.
    const float breakVox = tp.grabBreakDistance / kVoxelMeters;
    if (err.len() > breakVox) {
      Release(phys);
      return;
    }
    const float w = Weight(tp);
    const float capVox =
        (tp.grabCarrySpeed / kVoxelMeters) * std::max(w, kMinCarryFrac);
    Vec3 v = err * tp.grabStiffness;
    const float sp = v.len();
    if (sp > capVox) v = v * (capVox / sp);
    Vec3 lin, ang;
    phys.GetBodyVelocities(body_, lin, ang);
    // Spin is damped, never zeroed: a carried thing that cannot rotate at all
    // reads as welded to the camera, and one that keeps its spin windmills
    // forever because nothing it touches can slow it while the servo owns its
    // linear velocity.
    phys.SetBodyVelocities(body_, v, ang * std::clamp(tp.grabSpinDamp, 0.0f, 1.0f));
    // A servo'd body must not be allowed to fall asleep: it would stop being
    // stepped while still nominally held, and let go somewhere it never was.
    phys.ActivateBody(body_);
  }

  // What the player's movement is multiplied by while carrying this.
  // 1 with nothing held, and never below player.grabMinSpeedScale — being
  // unable to move at all while holding something you were allowed to pick up
  // is a softlock, not a weight.
  float SpeedScale(const Tuning::Player& tp) const {
    if (!body_) return 1.0f;
    return std::max(tp.grabMinSpeedScale, Weight(tp));
  }

 private:
  // THE ONE WEIGHT CURVE, so how slowly you walk and how slowly the thing
  // follows the crosshair can never disagree about which of two objects is
  // heavier. 1 at or below the free mass, then 1/(1 + excess/grabSlowMass):
  // half speed at free + slow, a third at free + 2*slow. Hyperbolic rather
  // than a linear ramp to a floor, because a linear ramp has a mass at which
  // one more kilogram stops mattering, and this way nothing is ever free.
  float Weight(const Tuning::Player& tp) const {
    const float slow = std::max(tp.grabSlowMass, 0.001f);
    const float excess = std::max(massKg_ - tp.grabFreeMass, 0.0f);
    return 1.0f / (1.0f + excess / slow);
  }
  // Floor under the CARRY speed only (the player's floor is a tuned knob).
  // Without it the heaviest liftable thing is servo'd at a speed indis-
  // tinguishable from zero and reads as a bug rather than as weight.
  static constexpr float kMinCarryFrac = 0.12f;

  uint64_t body_ = 0;
  float massKg_ = 0.0f;
  float dist_ = 0.0f;   // carry distance from the hand, voxels
  bool refusedHeavy_ = false;
};
