#pragma once
// ---- A SPELL CAST FROM A HAND: the mechanics every caster shares -------------
//
// The player (session.cpp, SPELLS IN HAND) and every creature that casts
// (game/demon_cast.cpp, MobHandCast) cast THE SAME WAY, and this is the one
// copy of it:
//
//   * a hand that holds a spell is ARMED: it plays the library clip
//     `spell_ready` (its derived mirror on the left), SpellReadyClip;
//   * a cast is a GESTURE (game/spellgesture.h, the delivery's): it starts
//     (BeginCastGesture), runs on the tick (AdvanceCastGesture), and the spell
//     leaves the hand on its RELEASE tick (CastGestureReleases) from where the
//     hand has got to (SpellHandWorld);
//   * the arm is posed through the rig's ONE weapon-arm claim, pointed at the
//     empty fist (CastArmPose), and handed back to the animation after the
//     gesture's last key.
//
// What differs is only WHO PRESSES: the player's button, or a creature's
// cast request — and where an item in the casting hand goes while the spell
// is in it (the player's hotbar; a creature's sheath or bag). On the tick,
// integer where it decides anything (the release), float only for the pose.

#include "game/hand.h"
#include "game/melee.h"
#include "game/spellgesture.h"
#include "math3d.h"

class Mob;
class Physics;
class MobSystem;

// One hand's gesture clock.
struct CastGestureRun {
  int gesture = -1;          // SpellGestureLib index; -1 = idle
  int ticks = 0;             // the clock (stops at a hold while held)
  bool fired = false;        // the cast has left the hand
  bool again = false;        // pressed during the wind-up: cast again after it fires
};

// The ONE arm claim's memory (one arm is posed at a time; the latest gesture
// takes it).
struct CastArmState {
  int hand = -1;             // HandIndex of the arm the claim is on; -1 = none
  float w = 0.0f;            // the claim's weight, 0..1
  Vec3 from{};               // the live hand at the start, gesture frame, reaches
  bool fresh = false;        // read `from` on the next pose
};

// Start `gesture` on hand `hk`'s clock and give it the arm claim (when there
// is a gesture to pose).
inline void BeginCastGesture(CastGestureRun& run, CastArmState& arm, int gesture, int hk) {
  run = CastGestureRun{};
  run.gesture = gesture;
  if (gesture >= 0) {
    arm.hand = hk;
    arm.fresh = true;
  }
}

// Does the cast leave the hand THIS tick? (Asked before AdvanceCastGesture.)
inline bool CastGestureReleases(const CastGestureRun& run, const SpellGesture& g) {
  return run.gesture >= 0 && !run.fired && run.ticks >= g.releaseTicks;
}

// One tick of the clock. A gesture with a hold (a beam) stops at it while
// `held`. Long enough after the last key for the arm to have faded out (~12
// ticks at CastArmPose's rate), the run is over (back to idle).
inline void AdvanceCastGesture(CastGestureRun& run, const SpellGesture& g, bool held) {
  const bool holding = run.fired && g.holdTicks >= 0 && run.ticks >= g.holdTicks && held;
  if (!holding) run.ticks++;
  if (run.fired && run.ticks > g.endTicks + 15) run = CastGestureRun{};
}

// Is the gesture still between its press and its last key (the body is busy
// casting: a creature does not start a swing or a guard through it)?
inline bool CastGestureBusy(const CastGestureRun& run, const SpellGestureLib& lib) {
  const SpellGesture* g = lib.At(run.gesture);
  return g != nullptr && run.ticks <= g->endTicks;
}

// THE ARM POSE for the claim's hand this tick. Sets the body's strike hand and
// effector (the empty fist, StrikeEffectorMode::Chain) and fills `out`; the
// caller applies it (SetWeaponPose — a creature's is re-applied after its own
// stroke step, Mob::SetCastArm). False = the claim is over or impossible (no
// gesture, a hand that cannot be posed, faded out): the arm is handed back and
// `arm` cleared. `aim` is the cast's direction, world.
bool CastArmPose(Mob& body, CastArmState& arm, const CastGestureRun& run,
                 const SpellGestureLib& lib, Vec3 aim, WeaponPose& out);
// Hand the arm straight back to the animation.
void DropCastArm(Mob& body, CastArmState& arm);

// THE ARMED HAND: `spell_ready` up while `armed`, down when not. `clipOn` is
// the caller's memory of which way it last set it (re-played when an R/F5
// reload rebuilt the rig's clip state under a hand that is still armed).
void SpellReadyClip(Mob& body, Hand h, bool armed, bool& clipOn);

// Where a spell in this hand is, world: the hand part's live body.
bool SpellHandWorld(const MobSystem& mobs, const Physics& phys, uint64_t mobId, Hand h,
                    Vec3& out);
