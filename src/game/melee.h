#pragma once
#include "sim/scale.h"  // MetresToCells / MetresPerSecToCells
#include <cstdint>
#include <vector>

#include "game/item.h"
#include "math3d.h"

// MOUSE-DIRECTED MELEE (the "half sword" experiment).
//
// THE IDEA. A swing is not an animation you trigger, it is a motion you make.
// Hold the attack button and the blade goes on guard, tracking the mouse
// one-for-one: the weapon is an extension of the hand and the hand follows the
// cursor. Move briskly and the blade commits — it accelerates along the
// direction you actually moved, so a diagonal flick is a diagonal cut and a
// flat sideways sweep is a flat sideways cut. Nothing selects from a list of
// canned attacks; the direction, the plane and the speed of the cut are all
// read off the mouse.
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
  Guard,      // button held: blade up, tracking the mouse 1:1
  Wind,       // mouse moving, building the cut direction (still tracking)
  Slash,      // committed: the blade drives along the chosen direction, cutting
  Recover,    // post-cut settle back toward guard (or idle if released)
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
  // How strongly the guard pose follows the mouse, in world voxels per
  // (pixel/sec). Small — this is a lean, not a teleport.
  float trackGain = MetresToCells(0.00038f);
  // Per-axis gain on the mouse BEFORE it becomes a cut direction and a lean.
  // These shape the *pose*, not the commit test: `commitSpeed` is still
  // compared against true mouse speed, so raising these makes the blade travel
  // further per pixel without making it fire off a twitch.
  //
  // Vertical is much higher than horizontal on purpose. A screen-space pixel
  // costs the same either way, but the arm's vertical range (hip to overhead)
  // is short and the player runs out of mousepad long before the blade gets
  // overhead; horizontal has the whole width of a sweep to play with.
  //
  // xGain is NEGATIVE: the raw screen-right vector is geometrically correct
  // (it matches `cam.Right()`, which is the same basis strafe uses), but a cut
  // that tracks the cursor reads as mirrored in the hand — you push the hilt
  // right to bring the EDGE left through the target. Mirroring here is what
  // makes a rightward flick cut rightward through what you are looking at.
  float xGain = -2.0f;
  float yGain = 5.0f;
  // Guard-pose offsets from the shoulder, authored in METRES.
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

  // Advance the state machine. `held` is the attack button, `armed` is whether
  // a melee weapon is actually equipped (an unarmed player never leaves Idle).
  // `right`/`up`/`fwd` are the camera basis the swing is expressed in, so a cut
  // is always described relative to where the player is looking.
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
  // Guard-relative lean from mouse tracking, decayed toward zero.
  Vec3 lean_{};
};
