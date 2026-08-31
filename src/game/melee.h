#pragma once
#include "sim/scale.h"  // MetresToCells / MetresPerSecToCells
#include <cstdint>
#include <vector>

#include "game/item.h"
#include "math3d.h"

// MOUSE-DIRECTED MELEE (the "half sword" experiment).
//
// THE IDEA. A swing is not an animation you trigger, it is a motion you make.
// Hold the attack button and you TAKE OVER THE ARM: the hand stays exactly
// where the animation had it, and from that instant the mouse MOVES it —
// every pixel of motion is a fixed distance of hand travel, added to where the
// hand already is. Move briskly and the blade commits — it accelerates along
// the direction you actually moved, so a diagonal flick is a diagonal cut and
// a flat sideways sweep is a flat sideways cut. Nothing selects from a list of
// canned attacks; the direction, the plane and the speed of the cut are all
// read off the mouse.
//
// THE MOUSE IS A HAND, NOT A POINTER (2026-08-30). The first version mapped
// mouse VELOCITY to a lean off a fixed guard pose: the hand snapped to
// guard-up-and-out the instant you clicked, and thereafter sat wherever the
// current mouse speed put it, saturating its clamp on any real flick and
// falling back to guard the moment you stopped. Two things were wrong with
// that and only one of them is a tuning value:
//
//   * clicking TELEPORTED the arm. A guard pose is a place; taking control of
//     something should start where that thing IS. Anything else is a pop the
//     player did not ask for, and it reads as the arm "shooting" somewhere.
//   * a velocity map has no memory. The hand position was a function of how
//     fast the mouse was moving THIS INSTANT, so it could not be aimed — you
//     could not put the blade somewhere and leave it there, and holding a slow
//     steady push got you a small constant offset rather than a long travel.
//
// The hand position is now INTEGRATED state (`handCam_`): mouse deltas add to
// it and nothing pulls it back. That is what "incremental" buys — the blade
// stays where you put it, and a long slow drag reaches as far as a fast one.
//
// WHY IT IS BUILT THIS WAY. Three properties are load-bearing and everything
// else here is negotiable:
//
//   1. The POSE IS THE HITBOX. Damage comes from sweeping the blade's authored
//      `edge` segment (MobLimbDef::edgeFrom/edgeTo) from where it was last
//      tick to where it is now — not from a cone in front of the camera, not
//      from a hitbox that switches on during an animation window. So what you
//      hit is exactly what the blade visibly passed through, and the location
//      struck is the location that loses voxels. That is the whole point of
//      the feature and the reason limb damage can be per-voxel.
//
//   2. SPEED IS THE DAMAGE. A blade barely moving does nothing; the same blade
//      moving fast opens a limb. Because the speed comes from the mouse, the
//      player's own motion is the damage roll — no cooldowns, no swing timer,
//      no randomness. Being slow is punished by being ineffective rather than
//      by being locked out.
//
//   3. IT IS PRESENTATION STATE (CLAUDE.md rule 1). Every field here is CPU
//      float, exactly like the animation and gait state it drives. The sim
//      never sees it. Damage reaches the world only through the ordinary
//      MutationQueue paths (CarveLimbRadial, BrushOp, ParticleSpawn), so a
//      swing cannot move the world hash by any route but the ops it emits.
//
// WHAT IS DELIBERATELY NOT HERE. No stamina, no parry/bind, no blade-on-blade
// physics, no combo table. This is a first pass at the *feel* of directing a
// cut with the mouse; the state machine below is small on purpose so it can be
// thrown away when the feel is wrong.

// The swing state machine. Small enough to reason about; the interesting
// behaviour is in how Wind reads the mouse, not in the graph.
enum class SwingPhase : uint8_t {
  Idle = 0,   // weapon at rest, following the walk cycle
  Guard,      // button held: the mouse owns the hand, moving it 1:1
  Wind,       // mouse moving fast, building the cut direction (still owning it)
  Slash,      // committed: the blade drives along the chosen direction, cutting
  Recover,    // the cut's follow-through unwinds; the arm is handed back
};

// One resolved cut of the blade this tick: the segment the edge swept, and how
// fast it was going. The damage side of melee.cpp turns this into carves.
struct EdgeSweep {
  Vec3 aPrev{}, bPrev{};   // edge base/tip last tick, world voxels
  Vec3 aNow{}, bNow{};     // edge base/tip now
  float speed = 0;         // tip speed, world voxels/sec
  float halfWidth = 0;     // carve radius, world voxels
  bool valid = false;
};

// Tunable feel. Lives here rather than in tuning.json for the first pass
// because these are the knobs being *designed*, not the ones being dialled;
// once the feel settles they belong in tuning.json like everything else.
struct MeleeTuning {
  // Mouse pixels per second past which a guarded blade commits to a cut. Low
  // enough that an intentional flick always fires, high enough that aiming
  // while guarding does not.
  float commitSpeed = 900.0f;
  // How far the hand travels from guard over a full cut. METRES at the call
  // site, cells in the field: a reach is a physical fact about an arm, and a
  // bare 5.5 would have become 27 cm the moment kVoxelMeters halved.
  float swingReach = MetresToCells(0.55f);
  // Seconds the committed slash takes. Short: a cut is a snap, not a wind-up.
  float slashTime = 0.17f;
  float recoverTime = 0.22f;
  // Blade tip speed (world voxels/sec) at and above which a hit does full
  // damage; below it damage falls off linearly to zero. This is what makes a
  // committed cut different from waving the weapon around.
  float fullSpeed = MetresPerSecToCells(3.4f);
  float minSpeed = MetresPerSecToCells(0.9f);
  // HOW FAR THE HAND TRAVELS PER MOUSE PIXEL — world voxels per pixel, NOT per
  // pixel/sec. This is the whole control law now: the mouse delta is a
  // displacement of the hand, integrated, so 200 px of mouse is the same
  // distance of travel whether you took a tenth of a second or two seconds
  // over it. `commitSpeed` below is still measured on true pixel SPEED, so
  // raising these makes the blade cover more ground without making a twitch
  // fire a cut.
  //
  // Vertical is higher than horizontal on purpose: the arm's vertical range
  // (hip to overhead) is short and the player runs out of mousepad long before
  // the blade gets overhead, while horizontal has a whole sweep to play with.
  //
  // Both are POSITIVE — screen-right moves the hand right and screen-up moves
  // it up. The velocity version mirrored X on the theory that you push the
  // hilt right to bring the edge left; that argument belongs to a blade that
  // is being AIMED, and it is simply backwards for a hand that is being MOVED.
  float moveGainX = MetresToCells(0.0030f);
  float moveGainY = MetresToCells(0.0040f);
  // Fallback arm reach, used only when the rig has not reported one through
  // MeleeState::SetArm (no weapon equipped yet, or a severed arm). The live
  // rig's own bone lengths are preferred — see SetArm.
  float fallbackReach = MetresToCells(0.60f);
  // The hand may be driven no further from the shoulder than this fraction of
  // the arm's reach. Slightly under 1 because a fully straight two-bone chain
  // is a locked elbow, and because AnimSolveTwoBone clamps to its own annulus:
  // a stored target OUTSIDE the reach would take mouse travel to wind back
  // before the arm visibly moved, which is exactly the "the input went dead"
  // feel integration is supposed to avoid.
  float reachFraction = 0.94f;
  // The seed used when the rig cannot say where the hand is: offsets from the
  // shoulder, authored in METRES. Not a pose the arm is ever snapped to while
  // control is live — only a starting point of last resort.
  float guardForward = MetresToCells(0.22f), guardUp = MetresToCells(0.26f),
        guardSide = MetresToCells(0.16f);
  // Seconds of mouse history the swing direction is averaged over. One tick of
  // raw delta is far too noisy to steer a cut with.
  float dirSmoothing = 0.06f;
};

// The player's melee state. One instance, owned by main.cpp beside the caster.
class MeleeState {
 public:
  // Feed raw mouse motion every FRAME (pixels), before Update. Kept separate
  // from Update because the tick loop runs 0..4 times per frame: the mouse is
  // sampled per frame and integrating it per tick would multiply-count it.
  void AddMouse(float dx, float dy);

  // WHERE THE ARM ACTUALLY IS, pushed in every tick before Update (Mob::
  // WeaponArmPose). Two jobs, and the feature does not work without either:
  //
  //   * `handFromShoulder` (world voxels, the same frame HandOffset returns) is
  //     the SEED. On the tick the blade comes up, control starts here, so the
  //     arm keeps the pose the walk cycle left it in and the player pushes it
  //     somewhere from there. No guard pose, no snap.
  //   * `reach` is the arm's own bone length, so the clamp on how far the mouse
  //     may push the hand is the rig's fact rather than a constant in this
  //     file — it follows a longer arm, and it follows kVoxelMeters.
  //
  // Call ClearArm when there is no arm to read (no rig, no weapon, severed);
  // the tuning fallbacks are used instead and the pose merely starts at a
  // guess rather than at the truth.
  void SetArm(const Vec3& handFromShoulder, float reach);
  void ClearArm();

  // Advance the state machine. `held` is the attack button, `armed` is whether
  // a melee weapon is actually equipped (an unarmed player never leaves Idle).
  // `right`/`up`/`fwd` are the camera basis the swing is expressed in, so a cut
  // is always described relative to where the player is looking — and so is the
  // integrated hand position, which is why turning the view carries the blade
  // around with you instead of leaving it pointing at a fixed piece of world.
  void Update(float dt, bool held, bool armed, const Vec3& right,
              const Vec3& up, const Vec3& fwd);

  // Where the weapon hand wants to be, as an offset from the shoulder in world
  // voxels. The avatar drives its arm IK at this; returning an offset rather
  // than a world point keeps this class ignorant of where the player is.
  //
  // THIS IS THE WHOLE OUTPUT THAT MATTERS. The blade is never re-aimed in the
  // fist: it keeps the grip angle its rig gives it and stays orthogonal to the
  // forearm, so holding the button buys CONTROL OF THE ARM rather than a
  // weapon that swivels to face wherever the mouse last moved.
  Vec3 HandOffset() const { return hand_; }
  // Nominal blade axis and roll. Reported for the HUD and for a future weapon
  // that genuinely does re-aim in the hand (a levelled spear, a raised
  // shield); the sword ignores them.
  Vec3 BladeDir() const { return bladeDir_; }
  Vec3 BladeUp() const { return bladeUp_; }

  // How much of the arm this claims, 0..1. Not a bool, because the tick the
  // claim ENDS is a pop otherwise: the hand is wherever the player left it and
  // the walk cycle wants it somewhere else, so the solve fades out over the
  // recover instead of being switched off. Coming IN needs no fade — control
  // starts at the arm's own current pose, so weight 1 changes nothing visible.
  float PoseWeight() const;

  SwingPhase Phase() const { return phase_; }
  bool Cutting() const { return phase_ == SwingPhase::Slash; }
  // 0..1 through the current slash; drives nothing but the HUD and audio.
  float SlashProgress() const {
    return tuning.slashTime > 0 ? phaseTime_ / tuning.slashTime : 0.0f;
  }
  // The direction the player actually flicked, in camera-plane terms. Exposed
  // so the HUD can show the cut the game read off the mouse — the player
  // needs to be able to tell "the game misread my flick" from "I misjudged
  // the distance", and without this that is unfalsifiable.
  Vec3 CutDir() const { return cutDir_; }
  float MouseSpeed() const { return mouseSpeed_; }

  void Reset();

  MeleeTuning tuning;

 private:
  SwingPhase phase_ = SwingPhase::Idle;
  float phaseTime_ = 0;
  // Accumulated mouse motion this frame, and its smoothed velocity.
  Vec3 mouseAccum_{};     // x = dx px, y = dy px (screen), z unused
  Vec3 mouseVel_{};       // smoothed px/sec
  float mouseSpeed_ = 0;
  // The committed cut direction in WORLD space, fixed at commit time so the
  // cut does not curve when the player keeps moving the mouse mid-slash.
  Vec3 cutDir_{};
  Vec3 hand_{}, bladeDir_{0, 1, 0}, bladeUp_{0, 0, 1};
  // THE INTEGRATED HAND POSITION, in the CAMERA frame: x along `right`, y along
  // `up`, z along `fwd`, in world voxels from the shoulder. Stored in that
  // frame rather than in world space so that yawing the view carries the hand
  // with it — a world-space offset would leave the blade behind as the player
  // turned, and the arm would fight the body.
  Vec3 handCam_{};
  // The cut's own travel, ADDED to handCam_ rather than replacing it: the
  // player keeps steering the hand through the slash, and the arc is a
  // follow-through on top of wherever they have steered it to. Decays over
  // Recover, which is what makes a cut end where the mouse ended rather than
  // snapping back to a pose.
  Vec3 swing_{};
  // The live arm, from SetArm. `armReach_` is a bone length, not a tuning.
  Vec3 armHand_{};
  float armReach_ = 0;
  bool armValid_ = false;
  // Was the button still down when this recover started? A recover between two
  // cuts keeps the arm; a recover after the release hands it back (PoseWeight).
  bool recoverHold_ = false;
};
