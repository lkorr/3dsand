#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game/anim.h"    // Ease / ApplyEase — a cut's pacing IS a clip's easing
#include "game/hand.h"
#include "game/melee.h"
#include "math3d.h"

// ============================================================================
// ATTACK STYLES — an NPC swing, authored (assets/mobs/attack_styles.json).
//
// THE ONE IDEA. `game/melee.h` says the stroke driver "never sees a mouse: it
// consumes ABSTRACT CONTROL DELTAS, and an NPC attack is the same driver fed an
// AUTHORED CURVE instead". This file is that curve, and it is DATA rather than
// a table of C++ because CLAUDE.md design rule 4 applies to a swing exactly as
// it applies to a creature: adding an attack must be a JSON edit.
//
// So there is no `enum SwingKind`, no `switch (style)` and no list of style
// names anywhere in the engine. `ai_behavior.h` already refuses to know what
// styles exist — its `AttackTuning::styles` is a list of opaque authored ids —
// and a profile that names a style this file has never heard of gets a loud
// skip and its first available style, never a crash and never a silent no-op.
//
// ---------------------------------------------------------------------------
// THE SCHEMA, and why a stroke is three segments
//
//   {
//     "name":    "horizontal_r",
//     "label":   "Horizontal cut, weapon side across",
//     "windup":  { "ticks": 12, "az":  0.30, "el": 0.10, "reach": -0.05 },
//     "cut":     { "ticks":  7, "az": -2.30, "el": 0.00, "reach":  0.10 },
//     "recover": { "ticks": 10, "az": 0.25, "el": 0.10, "reach": -0.10,
//                  "settle": 7, "fade": 5 },
//     "jitter":  { "az": 0.20, "el": 0.08, "tempo": 0.25 }
//   }
//
// ...and `cut` MAY BE A LIST OF LEGS instead of one segment, which is the only
// way to author a stroke that is not a straight line (2026-09-21):
//
//     "cut": [
//       { "ticks": 4, "az": -1.10, "el": -0.35, "reach":  0.20 },
//       { "ticks": 5, "az": -1.20, "el":  0.35, "reach": -0.10, "aim": true }
//     ]
//
// See "A CUT IS A PATH" below for what the legs mean and which one meets the
// target. One segment and a one-leg list are the same program.
//
// WINDUP is a POSE, in the mob's own facing basis, expressed RELATIVE TO THE
// TARGET: az 0 / el 0 points STRAIGHT AT IT (see "THE STROKE FRAME" below),
// azimuth positive to the mob's right, elevation positive up, `reach` a fraction of the arm's own reach band so a lunge
// is the same lunge on a long arm and a short one. It is driven closed-loop and
// deliberately SLOWLY — capped at 16 input units/tick (about half
// `MeleeTuning::commitSpeed`). Its length is the whole telegraph: 12 ticks is 0.40 s
// of visible blade raise, and there is no UI indicator by design.
//
// CUT is a TRAVEL, not a pose: how far the point goes FROM THE WINDUP POSE and
// how fast. The deltas are divided by `cut.ticks` and delivered per tick at whatever pixel rate that
// implies, which gives the sweep the tip speed that scales the damage (melee.h note 2, "SPEED IS THE DAMAGE").
//
// RECOVER is a RETURN, and it is an absolute pose rather than an aim-relative
// one: the arm is DRIVEN back to a stance over `settle` ticks (closed-loop and
// capped, the windup's own drive) and
// only then is the claim handed back over `fade`. A style that authors no pose
// gets the historical behaviour — release on the first tick, freeze wherever
// the cut ended, crossfade. See `StrokeRecover` below for why that was not good
// enough.
//
// ---------------------------------------------------------------------------
// A CUT IS A PATH (2026-09-21)
//
// One segment can only express a straight line through the target: the deltas
// are divided by the ticks and delivered at a constant rate, so every cut in
// the library was a chord. A hook that comes round the guard, a chop that
// drops and then drags, a feint that checks and re-commits — none of them had
// an authoring surface, and "add another style" cannot make one, because the
// thing missing is INSIDE one stroke.
//
// So `cut` is a LIST OF LEGS, run back to back inside the one Cut phase. Each
// leg is an ordinary `StrokeSegment` and means exactly what the single cut
// meant: a TRAVEL from wherever the previous leg ended, its deltas divided by
// its own `ticks`. The path is therefore CUMULATIVE — leg 2's `az` is measured
// from the end of leg 1, not from the start of the cut — because that is what
// makes an author able to lengthen leg 1 without re-typing everything after it.
//
// THE PHASE DOES NOT SPLIT. `Phase::Cut` covers the whole path, `cutTicks` is
// its total and `phaseTick` counts across it, so everything outside this file
// that asks "is this stroke cutting" or "how many cut ticks are left" — the
// damage sweep, the bite holdout, the dev readout, every gate — is unchanged
// and needs to know nothing about legs. A leg boundary is a change of velocity,
// not a change of state; there is no frame of hand-back between them.
//
// A leg that travels nowhere is a HITCH, and is legal: it holds the point where
// it is for its ticks, which is the hesitation in a feint. What is refused is a
// whole path that travels nowhere — see the loader.
//
// ---------------------------------------------------------------------------
// THE CUT IS CENTRED ON THE AIM, AND THE AIM IS TAKEN ONCE
//
// A committed cut does not home. The aim — the target's bearing and elevation
// about this mob's own shoulder — is resolved at the END OF THE WINDUP and
// never again, so a target that steps offline after the blade has started
// moving is missed, which is the entire point of a telegraph. The windup's own
// target is then `aim - cut/2 + windup`: the blade ends up half a cut short of
// the aim so that the MIDDLE of the travel passes through it, rather than the
// beginning. A stroke aimed at its own start point cuts the air behind the
// target every time.
//
// WITH A PATH, "half a cut" BECOMES "WHICH LEG MEETS THEM", and it is authored
// rather than assumed: a leg may say `"aim": true`, and the target then sits at
// the middle of THAT leg (`AttackStyle::CutAimOffset`). Unstated, the aim sits
// at the midpoint of the whole travel, which for a one-leg cut is the old
// `cut/2` exactly. It matters because the midpoint of a dogleg is often the
// corner — the one point on the path where the blade is slowest and turning —
// and a hook whose target sits there lands its hesitation on them instead of
// its edge.
//
// ---------------------------------------------------------------------------
// THE STROKE FRAME (2026-09-25): THE WINDUP IS MEASURED FROM THE TARGET
//
// Until 2026-09-25 the windup was measured from "half a cut short of the aim"
// (`aim - CutAimOffset + windup`) and every cut leg's target was
// `aim - CutAimOffset + CutThrough(k)`, which kept the middle of the cut on the
// target automatically but made the windup numbers unreadable: 0/0/0 was not
// "at the target", it was half a cut away from it, and the panel's goal
// readout never matched the boxes.
//
// NOW, WHAT THE NUMBERS SAY IS WHERE THE ARM GOES:
//
//   windup pose     = aim + windup                (0/0 = pointing at them)
//   end of leg k    = aim + windup + CutThrough(k)
//
// so the cut starts where the windup put the arm and travels what it says.
// Centring the cut on the target is now the AUTHOR's choice: the classic
// through-the-middle stroke is `windup = -CutAimOffset()` (the tuner's
// "centre" button), and the `"aim": true` leg marker says which leg that
// centring aims at. It no longer moves anything at runtime.
//
// OLD FILES STILL LOAD AS THEY PLAYED. A file without `"strokeFrame":
// "target"` is in the old frame, and `AttackStyle::FromLegacyFrame` converts
// each style after the player overrides are merged — algebraically the same
// program: the windup pose moves by -CutAimOffset and leg 1 absorbs the old
// windup offset (the old cut ignored it and started from `aim - offset`), so
// every leg ends on the same absolute point it always did.
//
// ---------------------------------------------------------------------------
// VARIATION IS DETERMINISTIC (CLAUDE.md rule 1)
//
// Ten swings must not look stamped, and they must still replay. Every draw is
// `rng::Hash3(mobId ^ salt, tick, index)` — the same counter-based hash the
// shader and the AI use — so the variation is a pure function of who swung and
// when. `jitter.az`/`el` bow the start pose; `jitter.tempo` scales the windup
// and cut tick counts, which is what stops two duelists beating time together.
// Nothing here reads a clock, an accumulator or a Jolt float.
//
// The whole thing is CPU float presentation state, exactly like the melee state
// and the gait it drives. Damage reaches the world only through
// MeleeSweepDamage's ordinary MutationQueue paths.
// ============================================================================

// One segment of a stroke program. `reach` is a FRACTION of the arm's reach
// band rather than voxels, for the kVoxelMeters reason every other length in
// this engine is derived: a bare "3 cells" halves in metres the moment the
// world scale moves.
struct StrokeSegment {
  int ticks = 8;
  float az = 0;      // windup: pose, relative to the aim. cut: travel.
  float el = 0;
  float reach = 0;
  // THIS SEGMENT'S OWN PACING ("ease" on the segment; 2026-09-25). False =
  // unstated: a cut leg then uses the style-wide `AttackStyle::ease`, and the
  // windup keeps its closed-loop under-commit chase (`steerTo`). True = the
  // segment's travel is spread over its ticks by this curve — see "PER-SEGMENT
  // PACING" below AttackStyle's ease note.
  bool paced = false;
  Ease ease = Ease::Linear;
};

// HOW MANY LEGS ONE CUT MAY HAVE. A ceiling because the live cursor carries
// the jittered tick count of each leg in a fixed array (a stroke program is
// per-creature presentation state stepped every tick; it does not allocate),
// and because a path with more corners than this is a clip rather than a
// stroke — the body animation lane is where that belongs. The loader drops the
// extras LOUDLY rather than silently truncating the motion.
constexpr int kMaxCutLegs = 6;

struct StrokeJitter {
  float az = 0;      // radians of start-azimuth bow, +-
  float el = 0;
  float tempo = 0;   // fraction of the tick counts, +-
};

// ---- A BALLISTIC OPENING (AttackStyle::lunge; plan §5) ---------------------
//
// A pounce is not a faster walk. The walk drive resolves a whole step against
// the body's box every tick and cannot leave the ground, so "jump at them and
// bite" had no expression at all — a zombie either stood in reach or did not.
// A lunge hands the body a VELOCITY (Mob::Launch) at the start of a named
// phase and lets `UpdateFall` carry it, wall test and all.
//
// METRES PER SECOND, converted once by `MetresPerSecToCells`, for the reason
// every other authored length in this engine is derived: "2.4" has to mean the
// same pounce when kVoxelMeters moves. `ticks` is the flight the author is
// budgeting for — the magnitude is capped so the body arrives at striking
// distance rather than inside the victim, and lining the landing up with the
// cut is the AUTHOR's job (windup.ticks ~ ticks; the tuner's Attacks lane
// shows both).
struct StyleLunge {
  int ticks = 0;             // flight the author is aiming the cut at
  float speed = 0;           // m/s, horizontal CEILING (the closing gap wins)
  float rise = 0;            // m/s, straight up
  // Which phase's START fires it: "windup" (the default — the leap IS the
  // telegraph) or "cut". A string rather than an enum because the phase names
  // are already this file's vocabulary and an enum would be a second list to
  // keep in step.
  std::string at = "windup";
  bool Any() const { return ticks > 0 && (speed > 0.0f || rise > 0.0f); }
};

// ---- WHICH LIMB A BLOW IS AIMED AT (AttackStyle::target; plan §5) ----------
//
// Weights over LIMB TAGS, not names: a style says "mostly the head, sometimes
// an arm" and any rig that tags its parts answers it, including one with four
// of them. Absent = today's behaviour, the chest. Drawn counter-based on
// (mobId, tick) at BeginStroke like every other variation here, and recorded
// on NpcStroke::targetLimb so a gate can report the distribution rather than
// inferring it from where the wounds landed.
struct StyleTargetWeight {
  std::string tag;
  float weight = 0;
};

// ---- HOW THE ARM COMES BACK (AttackStyle::recover; 2026-09-21) ------------
//
// THE RECOVER USED TO BE FOUR LINES AND NONE OF THEM WAS A MOTION. The runner
// set `held = false` and stepped the driver, and everything after that was out
// of the style's hands: the STORED stroke froze wherever the cut ended (only
// the follow-through arc decayed), `PoseWeight` ramped 1 -> 0 over the ONE
// global `melee.recoverTime` shared by every attack in the game, and
// Mob::ApplyWeaponArm handed that number to AnimSolveTwoBone as a BLEND
// WEIGHT. So the path from the end of a cut back to the walk cycle was a
// rotation-space crossfade with nothing in it that knows about anatomy — and a
// cut that ends with the blade across the body blends THROUGH the torso to get
// home. That is the "limbs move in impossible ways" report, and no amount of
// tuning the windup or the cut could reach it, because the recover was not
// being authored at all.
//
// So a recover is now a SEGMENT like the other two, and the arm is DRIVEN
// through it:
//
//   "recover": { "ticks": 10, "az": 0.25, "el": 0.10, "reach": -0.10,
//                "settle": 7, "fade": 5 }
//
// THE POSE IS ABSOLUTE, in the mob's own facing basis — the same vocabulary
// `StrokeCursor::Phase::Guard` already uses, and deliberately NOT the windup's
// aim-relative one. A recover is a return to STANCE, not a second aim: an
// arm that recovered relative to the target would end every swing pointed back
// at whatever it just hit, which reads as re-chambering rather than as
// finishing. `reach` is an offset from the neutral band position, exactly as
// `windup.reach` and `cut.reach` are, so it means the same thing on any rig.
//
// SETTLE is how many of the `ticks` are DRIVEN before the hand-back begins.
// Those ticks steer closed-loop and under `commitSpeed` (the windup's own
// drive, so no cut can fire out of a recover) with the button still down, so
// `PoseWeight` stays at 1 and the arm really travels. Only afterwards is the
// button released and the claim faded — from a pose that is already near the
// one being blended to, which is what makes the crossfade short enough to be
// invisible instead of long enough to go through the chest.
//
// FADE overrides `melee.recoverTime` FOR THIS STYLE, in ticks (0 = the global
// value). A jab and an overhead chop have no business handing the arm back on
// the same clock, and before this they had no choice: one number in tuning.json
// owned every recover in the game.
//
// EVERY FIELD DEFAULTS TO THE OLD BEHAVIOUR. `posed` is false unless the style
// authored an az/el/reach, and an unposed recover releases on tick 0 with the
// global fade — which is, line for line, what the four lines used to do. A
// style written before this loads and swings identically.
struct StrokeRecover {
  int ticks = 10;
  // Did the author state a return pose at all? False = freeze where the cut
  // ended and crossfade, the pre-2026-09-21 behaviour.
  bool posed = false;
  float az = 0;      // ABSOLUTE, mob's facing basis: + is the mob's right
  float el = 0;      // ABSOLUTE, 0 is level
  float reach = 0;   // offset from the neutral band position, like the others
  // Ticks DRIVEN to that pose before the button is released. Clamped to
  // `ticks` at load. Meaningless without a pose, and ignored without one.
  int settle = 0;
  // Ticks the hand-back fade takes, overriding melee.recoverTime; 0 = global.
  int fade = 0;
  // The SETTLE's pacing ("ease" on the recover). Unstated = the closed-loop
  // chase, exactly as before; meaningless without a pose.
  bool paced = false;
  Ease ease = Ease::Linear;
};

// ---- HOW A CUT'S TRAVEL IS PACED (AttackStyle::ease; 2026-09-22) ----------
//
// The cut's per-tick delta is `gap / ticksLeftInThisLeg`, and that is a
// CONSTANT RATE: the gap shrinks linearly and the point lands on the leg's end
// exactly on its last tick (the derivation is in StepStrokeProgram). It is the
// only pacing a cut has ever had, and on a long cut it is the wrong one to be
// stuck with -- an author who wanted a blade that accelerates into the target,
// or one that arrives early and settles, could only fake it by splitting the
// path into legs with different tick counts.
//
// So the rate is a CURVE over the leg now. IT IS THE ENGINE'S EXISTING EASE
// VOCABULARY (`anim.h` Ease / ParseEase / ApplyEase), not a second one: those
// eight curves are already what a clip keyframe interpolates with, already
// authored by name in assets/anims/*.json, and already ported to
// editor/anim.js. A style saying `"ease": "quadOut"` therefore means exactly
// what a keyframe saying it means, which is design guideline 4 ("author
// content by name") and guideline 3 ("one authoritative source per fact")
// applied to the one concept both systems needed. The first cut of this had
// its own three-value `linear|exp|log` enum, and `exp`/`log` were literally
// QuadOut and QuadIn respelled.
//
//   linear      f(p) = p            constant rate. THE DEFAULT, and
//                                   algebraically the pre-2026-09-22 drive.
//   quadOut     f(p) = 1-(1-p)^2    fast off the mark, decelerating in
//   quadIn      f(p) = p^2          slow off the mark, accelerating in
//   cubicIn/Out                     the same two, harder
//   quadInOut / cubicInOut          slow at both ends, fast through the middle
//   instant                         hold, then arrive on the last tick
//
// `f(p)` is the fraction of THE LEG'S TRAVEL that should be behind the point at
// progress `p`. What the drive actually spends is the share of the REMAINING
// gap the curve advances this tick (StrokeEaseStep) rather than an absolute
// position, which is what keeps the property the Cut phase is built on: each
// leg's target is an ABSOLUTE point on the path, so a leg that ran out of
// ticks short of its corner does not displace the legs after it.
//
// PER-SEGMENT PACING (2026-09-25). The style-wide `ease` is now only the
// DEFAULT for cut legs that state none: every segment may carry its own
// `"ease"` — the windup, each cut leg, and the recover (its settle). A cut leg
// with its own ease uses it instead of the style's.
//
// The windup and the settle are different: unstated, they stay the
// closed-loop chase to a POSE under `commitSpeed` (`steerTo`) they have always
// been, which has no authored travel to distribute and just closes at a capped
// rate. STATING an ease turns them into a TIMED approach: each tick spends
// `StrokeEaseStep`'s share of the remaining gap to the pose, so the arm
// arrives on the segment's last tick with the speed where the curve puts it.
// The per-tick step is STILL clamped to steerTo's envelope — a pacing curve
// that asked for more than that is slowed. (The clamp used to be what kept a
// windup from firing the driver's own Slash; the driver has no Slash since
// 2026-09-25, so it is now purely the telegraph's rate cap.)

// The share of the REMAINING gap to spend on the tick carrying progress
// p0 -> p1.
//
// THE LAST TICK ALWAYS SPENDS EVERYTHING. Forced rather than derived, because
// `Ease` contains curves that do not reach 1 -- `instant` is 0 for every t
// including t=1 -- and a cut whose curve never completes would leave residue
// for the next leg to absorb, or stop short of the path's end entirely. With
// the force, all eight curves arrive exactly and `instant` is a legitimate
// shape (hold, then snap home) rather than a way to freeze the blade.
//
// For `linear` over a leg of N ticks this is (1/N) / ((N-k)/N) = 1/(N-k),
// which IS the old `gap / left` divisor. The Cut phase still spells that case
// out longhand rather than calling this, so the default drive cannot move a
// hash on float rounding; the identity is what makes that safe to do.
inline float StrokeEaseStep(Ease e, float p0, float p1) {
  if (p1 >= 1.0f) return 1.0f;
  const float f0 = ApplyEase(e, p0), f1 = ApplyEase(e, p1);
  const float room = 1.0f - f0;
  if (room <= 1e-6f) return 1.0f;
  const float s = (f1 - f0) / room;
  return s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
}

// ============================================================================
// AN ATTACK IS A LIST OF FRAMES (2026-09-25)
//
// Every frame is the same kind of thing: WHERE THE TIP IS when the frame ends,
// how long it takes to get there, how the speed is spread over that time, and
// every control that shapes the arm on the way — which side the hand leans,
// how much the wrist lays the blade along the stroke, how much the torso turns
// with it, and the per-joint brakes. "windup", "cut" and "recover" are only
// NAMES. What makes a frame a cut is `cuts` (the damage sweep is live in it).
//
// Before this, the three rows of a style meant three different things — the
// windup a pose relative to the target driven closed-loop at a HIDDEN 16
// units/tick cap, the cut a TRAVEL added to wherever the windup ended, the
// recover a pose relative to the body — and a dozen numbers that shaped the
// motion (torso share, wrist alignment, lean side, joint smoothing) were either
// global or not exposed at all. The owner's words: "cut and windup shouldn't be
// different from each other other than name", and "every parameter that
// relates to how the character moves [...] directly controllable".
//
// THE PHASES THE REST OF THE ENGINE READS ARE DERIVED, not authored: frames
// before the first `cuts` frame are the Windup (the aim is FROZEN at the end
// of it), the `cuts` frames are the Cut (contiguous; the loader enforces it),
// the frames after are the Recover, and `release` ticks of hand-back follow
// the last frame. NPC AI, parry, lunge and the damage sweep keep their phase
// vocabulary unchanged.
enum class FrameLean : uint8_t {
  Follow = 0,  // the hand leads the tip's travel (the edge leads a cut)
  Hold,        // keep whichever side it is on (a return, a feint)
  Left,        // hand on the wielder's left of the tip
  Right,       // ...or right
  Angle,       // at `leanAngle` about the shoulder-to-tip line (0 = below)
};
// ---------------------------------------------------------------------------
// A FRAME IS A POSE (2026-09-25).
//
// The tip-target frames above ask the stroke driver where the arm goes, and
// the driver answers with a chain of chasers — the lean plane follows the
// tip's travel, the flat follows it too, the elbow pole chases the hand's
// velocity, the blade angle is a law-of-cosines solve, then the two-bone IK
// and the joint clamp. Between two frames the arm is whatever that feedback
// system does, and on a swing whose travel turns (every windup-to-cut) it
// turns the blade plane with it: "the sword spins out everywhere".
//
// A POSE FRAME states the arm instead: the weapon arm's shoulder, elbow and
// wrist rotations RELATIVE TO THEIR PARENTS, plus the torso's twist and pitch.
// A style whose every frame has one is KEYED, and a keyed stroke slerps each
// joint from the previous frame's pose to this one's on the frame's own ease
// (StrokeKeyedPose -> WeaponPose::keyed -> Mob::SmoothWeaponArm). Nothing is
// solved, so nothing between two frames can do what the frames do not show.
// The first frame blends from the arm as the stroke found it.
//
// Authored in the tuner (Attacks lane: "bake to poses", then the ✋ manual
// handles, which edit joints), and stored AS IF AIMED STRAIGHT AHEAD: a
// target-measured frame is turned about the shoulder by the target's bearing
// at playback (yaw about the body's up, then pitch about its right); a
// `from: body` frame is not. The driver still runs underneath a keyed stroke
// for the phase clock, the cut window and the sounds — its arm is not used.
//
//   "pose": { "shoulder": [x,y,z,w], "elbow": [x,y,z,w], "wrist": [x,y,z,w],
//             "twist": 0.2, "pitch": 0.0 }
//
// Parts are named by ROLE (the weapon chain's upper bone, lower bone, hand),
// not by limb name, so the same style plays on either arm of any rig with the
// human's topology. `wrist` may be absent (a chain whose lower bone is the
// hand has none; an absent one leaves the animation's).
struct StrokePose {
  bool has = false;
  Quat joint[kArmJoints]{};
  bool hasJoint[kArmJoints] = {false, false, false};
  float twist = 0, pitch = 0;   // torso, radians: + toward the right, + chest up
};

struct StrokeFrame {
  std::string name;
  int ticks = 8;
  // WHERE THE TIP IS AT THE END OF THIS FRAME. az/el radians; reach a BAND
  // POSITION offset (0 = the neutral 0.60 of the arm's own annulus, +1 out,
  // -1 in). Measured from the TARGET (0/0 = straight at it) unless
  // `fromBody`, which measures it from the wielder's own facing instead.
  float az = 0, el = 0, reach = 0;
  bool fromBody = false;
  // How the travel is spread over the ticks. Every curve ARRIVES on the last
  // tick. OR `chase`: close on the pose at up to `chaseRate` input units per
  // tick (16 = about 4.6 deg/tick sideways, 6.1 up/down), arriving whenever
  // it arrives — possibly never, in a short frame. That is how every windup
  // and every return moved before frames, and a converted stock attack keeps
  // it, VISIBLY, until its author picks a curve.
  Ease ease = Ease::Linear;
  bool chase = false;
  float chaseRate = 16.0f;
  bool cuts = false;             // the damage sweep is live in this frame
  FrameLean lean = FrameLean::Follow;
  // 0..1: how much the wrist lays the blade along the commanded stroke
  // (1) versus letting it ride the grip (0).
  float wristAlign = 1.0f;
  // Fractions of the stroke's azimuth / elevation the torso carries; < 0 =
  // the global melee.torsoShare / torsoPitch.
  float torsoTwist = -1.0f, torsoPitch = -1.0f;
  // The per-joint brakes (melee.h ArmSmooth) IN FORCE DURING THIS FRAME.
  ArmSmooth joints;
  // THE ARM'S SHAPE at the end of the frame (melee.h ProgramDrive): the
  // blade's angle off the arm line (radians; < 0 = auto, the old
  // hand-at-melee.handExtend rule), and the elbow's direction about the
  // shoulder-to-hand line (radians, 0 = down, + = out; `elbowSet` false =
  // auto). Both are blended from where the arm is when the frame starts.
  float bladeAngle = -1.0f;
  bool elbowSet = false;
  float elbow = 0.0f;
  float leanAngle = 0.0f;   // used when lean == Angle; blended like the elbow
  // THE ARM ITSELF at the end of the frame ("A FRAME IS A POSE" below). When
  // every frame of a style has one, the style is KEYED and nothing above this
  // line but ticks / ease / cuts / fromBody / joints is read.
  StrokePose pose;
};

constexpr int kMaxFrames = 12;

struct AttackStyle {
  std::string name;    // the id a behaviour profile refers to
  std::string label;   // human text for the dev readout
  // How the cut's travel is paced over each leg, in the engine's shared ease
  // vocabulary (anim.h). Linear is the historical drive; see above.
  Ease ease = Ease::Linear;
  StrokeSegment windup;
  // THE CUT PATH, one leg or several ("A CUT IS A PATH" above). NEVER EMPTY
  // after `LoadAttackStyles` — a style whose whole path travels nowhere is
  // refused rather than loaded as a cut with no legs, so every reader may say
  // `cut[0]` and every writer that builds a style by hand owes it one leg.
  std::vector<StrokeSegment> cut{StrokeSegment{7, -2.0f, 0.0f, 0.10f}};
  // WHICH LEG THE TARGET IS IN THE MIDDLE OF (`"aim": true` on that leg);
  // -1 = the midpoint of the whole travel, which is what a one-leg cut has
  // always done.
  int aimLeg = -1;
  StrokeRecover recover;
  StrokeJitter jitter;
  // PER-JOINT BRAKES ON THE POSED ARM (melee.h ArmSmooth), authored as
  //   "joints": { "shoulder": { "smooth": 2, "maxDeg": 20 },
  //               "elbow": { ... }, "wrist": { ... } }
  // Absent = all zero = the solve untouched. Carried to the rig on the
  // WeaponPose by whoever runs the program.
  ArmSmooth joints;
  // ---- THE TRUTH (see "AN ATTACK IS A LIST OF FRAMES" above) --------------
  // Everything above this line — windup, cut, aimLeg, recover, joints, ease —
  // is a DERIVED VIEW kept for the readers that predate frames (gates, the AI
  // reach estimate). The runner reads only `frames` and `release`.
  std::vector<StrokeFrame> frames;
  // Ticks of hand-back after the last frame: the arm is released and its
  // claim fades to the walk cycle over exactly this long.
  int release = 6;
  int FirstCut() const {
    for (int k = 0; k < (int)frames.size(); k++) if (frames[k].cuts) return k;
    return -1;
  }
  int LastCut() const {
    for (int k = (int)frames.size() - 1; k >= 0; k--) if (frames[k].cuts) return k;
    return -1;
  }
  // Every frame is a pose ("A FRAME IS A POSE").
  bool Keyed() const {
    if (frames.empty()) return false;
    for (const StrokeFrame& f : frames) if (!f.pose.has) return false;
    return true;
  }
  // The pre-frames spelling (windup / cut legs / recover, already in the
  // TARGET stroke frame) turned into frames, replacing `frames`. Exact: the
  // runner's targets are the same expressions the old phases used.
  void BuildFramesFromLegacy();
  // ...and the other way: refill the derived views from `frames`.
  void DeriveLegacyViews();
  // The ease a cut leg with no `ease` of its own is paced by.
  Ease LegEase(int k) const {
    return (k >= 0 && k < (int)cut.size() && cut[k].paced) ? cut[k].ease : ease;
  }

  // ---- THE PATH, READ THREE WAYS ------------------------------------------
  // Here rather than at the call sites because a gate, the dev readout and the
  // runner all need the same arithmetic, and three copies of "sum the legs" is
  // three chances for one of them to still think a cut is a chord.

  // The whole travel, summed: what a single segment with these numbers would
  // have been, `ticks` included. This is the displacement a style ASKS for and
  // what "is this style azimuth-dominant" is a question about.
  StrokeSegment CutTravel() const {
    StrokeSegment t{0, 0, 0, 0};
    for (const StrokeSegment& s : cut) {
      t.ticks += s.ticks;
      t.az += s.az;
      t.el += s.el;
      t.reach += s.reach;
    }
    return t;
  }
  // Cumulative travel from the start of the cut through leg `k` INCLUSIVE, so
  // `CutThrough(k)` is where the point stands when leg k ends. `k < 0` is the
  // start of the path, which is what makes the aim arithmetic below one line.
  StrokeSegment CutThrough(int k) const {
    StrokeSegment t{0, 0, 0, 0};
    for (int i = 0; i <= k && i < (int)cut.size(); i++) {
      t.ticks += cut[i].ticks;
      t.az += cut[i].az;
      t.el += cut[i].el;
      t.reach += cut[i].reach;
    }
    return t;
  }
  // WHERE THE TARGET SITS ALONG THE PATH, measured from the cut's start: the
  // middle of the `aim` leg, else of the whole travel. No longer used by the
  // runner (see "THE STROKE FRAME"); it is what "centre" writes into the
  // windup (`windup = -this`) and what the legacy conversion subtracts.
  void CutAimOffset(float& az, float& el) const {
    if (aimLeg >= 0 && aimLeg < (int)cut.size()) {
      const StrokeSegment before = CutThrough(aimLeg - 1);
      az = before.az + 0.5f * cut[aimLeg].az;
      el = before.el + 0.5f * cut[aimLeg].el;
      return;
    }
    const StrokeSegment all = CutTravel();
    az = 0.5f * all.az;
    el = 0.5f * all.el;
  }
  // OLD STROKE FRAME -> TARGET FRAME ("THE STROKE FRAME" above). The same
  // program, re-expressed: called by the loader on a file without
  // `"strokeFrame": "target"`, after the player overrides are merged.
  void FromLegacyFrame() {
    float offAz = 0, offEl = 0;
    CutAimOffset(offAz, offEl);
    const float wAz = windup.az, wEl = windup.el;
    windup.az = wAz - offAz;
    windup.el = wEl - offEl;
    if (!cut.empty()) {
      cut[0].az -= wAz;
      cut[0].el -= wEl;
    }
  }
  // ---- WHAT SWINGS IT (plan §4/§5) ---------------------------------------
  // "held" (the default, and every style authored before this existed) or the
  // NAME of a natural weapon on the creature's own rig (mob.h
  // MobNaturalWeaponDef): "fist.R", "jaws". `StyleUsable` resolves it against
  // the creature that is trying to swing, so a style naming a weapon this
  // creature does not have — or whose part has been cut off — is simply not
  // drawn, rather than starting a stroke with no edge on the end of it.
  std::string weapon = "held";
  // ONLY WHEN THERE IS NOTHING BETTER. A duelist lists its punches beside its
  // cuts; with a sword in its fist it must never draw one, and disarmed it
  // must. `PickAttackStyle` drops every fallback style when any non-fallback
  // style is still usable, which expresses that in one line of JSON per style
  // instead of in a second list per profile.
  bool fallback = false;
  // World voxels, centre-to-centre, OVERRIDING the profile's `attack.reach`
  // for this style alone; 0 = use the profile's. A lunging bite closes 22
  // voxels and a punch closes 9, and a profile can only state one number —
  // which is why this is per style and why MobSystem::AttackReachOf exists.
  float reach = 0;
  StyleLunge lunge;
  std::vector<StyleTargetWeight> target;
  // ---- ...AND WHAT IT GOES FOR WHEN IT IS ON THE GROUND (2026-09-17) ------
  //
  // The same table, used instead of `target` when the ATTACKER is prone --
  // i.e. when its active dismemberment loco state is one that lays the body on
  // the ground (anim.h AnimStateRule::groundAlign above 0: crawl, squirm).
  // A thing dragging itself along on its elbows cannot reach your head, and a
  // zombie whose legs are gone going for the legs is the whole reason the
  // crawl state is worth having.
  //
  // A SECOND AUTHORED TABLE, not a hardcoded rule in the draw: which limbs a
  // crawler can reach is a fact about that creature's shape, and a rig whose
  // prone form is a snake rearing up would want the opposite. Empty (the
  // default, and every style authored before this) means "no change" -- the
  // ordinary table is used whatever posture the attacker is in, which is
  // exactly the old behaviour.
  std::vector<StyleTargetWeight> targetProne;
  // AN AUTHORED BODY ANIMATION TO PLAY WITH THE STROKE, by name (empty = none).
  // The stroke program drives the WEAPON ARM through the melee driver; this is
  // everything else — the step, the shoulder drop, the off hand — keyframed in
  // the tuner's clip lane and saved to assets/anims/<name>.json, from where
  // LoadMobDefs compiles it onto every rig whose part names it fits (mob.cpp,
  // "THE SHARED CLIP LIBRARY"). Started by Mob::PlayClip at BeginStroke, so it
  // runs on the ordinary clip layer (mask, blend, mode) and the driver's arm
  // claim still wins on the arm. A name no rig knows is a loud loader line,
  // not a crash: PlayClip no-ops on a miss.
  std::string clip;
  // THE WEAPON FORM this copy is ("short" / "long" / "blunt"), "" for the
  // style itself. Set only on the copies the loader makes from `forms`.
  std::string form;
};

// ---- WEAPON FORMS (attack_styles.json `forms` per style) --------------------
// ONE STYLE, ONE VERSION PER KIND OF WEAPON. `horizontal_r` is the stroke the
// compass and every behaviour profile name; `forms.short` / `.long` / `.blunt`
// each may carry that stroke's own frames (and release) for a weapon of that
// class (ItemDef::weaponClass). A form that is not authored is the style's
// own frames, so a new weapon class never needs a line of JSON to swing.
// Resolved ONCE, at the stroke's start (the player's press and
// MobSystem::StartStroke), into a separate library entry, so every runner
// below sees an ordinary AttackStyle.
//
// `unarmed` IS A FORM TOO (2026-09-27): an EMPTY hand throws the held styles
// with its fist (Mob::ArmForStyle), and `forms.unarmed` is how those strikes
// get their own poses. It is never an item's weaponClass; the stroke's start
// resolves it when the striking hand holds nothing (FormForItem).
constexpr int kWeaponForms = 4;
extern const char* const kWeaponFormNames[kWeaponForms];   // short, long, blunt, unarmed
constexpr int kFormUnarmed = 3;
// An item's weaponClass -> its form index; -1 for none/unknown (the style's
// own frames).
int WeaponFormOf(const std::string& weaponClass);
// THE FORM A STROKE SWUNG WITH THIS ITEM TAKES: its class's, or `unarmed`
// for an empty hand (nullptr).
struct ItemDef;
int FormForItem(const ItemDef* item);

// THE PLAYER'S FLICK COMPASS (the `player` block of attack_styles.json): a
// screen-space direction per style, quantized by max dot at the attack press.
// Indices, not names — resolved against the library at load time (a sector
// naming an unknown style is skipped LOUDLY, the loader's convention), and
// rebuilt with the library on every R so hot-reload renumbering cannot bite.
// It lives ON the StyleLibrary because it is meaningless apart from one.
struct PlayerStrikeMap {
  struct Sector {
    float x = 0, y = 0;   // unit-ish, screen space: +x right, +y DOWN
    int style = -1;
  };
  std::vector<Sector> sectors;
  int neutral[2] = {-1, -1};   // the directionless click alternates these
  // THE CLICK THAT REPEATS (`clickRepeat`): after this style, a directionless
  // click with the same hand throws it again instead of a neutral, and chains
  // — but only for a weapon whose form bit is in repeatForms (StrikeRepeats).
  int repeat = -1;
  unsigned repeatForms = 0;    // bit f = weapon form f (kWeaponFormNames)
  bool Usable() const { return !sectors.empty() || neutral[0] >= 0; }
};

struct StyleLibrary {
  std::vector<AttackStyle> styles;
  PlayerStrikeMap player;
  // ...and the SAME compass for a player with nothing in their fist (the
  // `playerUnarmed` block). A second map rather than a `weapon` filter over
  // the first, because the two are different SHAPES: a sword's compass has an
  // overhead and a thrust, a fist's has a jab, a cross and a straight, and
  // asking one set of sectors to mean both would make every punch a
  // re-labelled sword cut.
  PlayerStrikeMap playerUnarmed;
  // forms[style][form] -> the entry for that style swung with that class of
  // weapon, -1 when the style has no such form (strokes.h WEAPON FORMS).
  std::vector<std::array<int, kWeaponForms>> forms;
  int ResolveForm(int style, int form) const {
    if (form < 0 || form >= kWeaponForms || style < 0 ||
        style >= (int)forms.size())
      return style;
    const int f = forms[style][form];
    return f >= 0 ? f : style;
  }
  int Find(const std::string& n) const {
    for (size_t i = 0; i < styles.size(); i++)
      if (styles[i].name == n) return (int)i;
    return -1;
  }
  const AttackStyle* At(int i) const {
    return (i >= 0 && i < (int)styles.size()) ? &styles[i] : nullptr;
  }
  bool empty() const { return styles.empty(); }
};

// The flick (screen space, +y down) -> a style index by max dot over the
// sectors; -1 when the map has none. The caller decides what a -1 means
// (fall back to NeutralStrike, or don't swing).
//
// TAKES THE MAP, NOT THE LIBRARY, since the library now holds two of them
// (`player` and `playerUnarmed`) and the caller is the only thing that knows
// which fist is empty.
int QuantizeStrike(const PlayerStrikeMap& map, float dx, float dy);
// The directionless click: one of the two neutral entries, `right` picking
// which. Returns the other one when the asked-for side is unresolved, and -1
// when neither is.
int NeutralStrike(const PlayerStrikeMap& map, bool right);
// DOES `next` CHAIN OFF `prev`? (the player's strike chaining, session.cpp)
// A swing ends with the weapon on the side it was flicked toward, so the
// follow-up that starts from there is the one flicked the OPPOSITE way: a
// sector of `next` within `leeway` compass steps (sectors sorted by angle) of
// the sector opposite a sector of `prev`. Base style indices, as the map
// holds them. A chained strike may begin during the recover and winds up
// faster; any other one waits the recover out.
bool StrikeChains(const PlayerStrikeMap& map, int prev, int next, int leeway);
// DOES `prev` REPEAT ON A PLAIN CLICK with a weapon of `form`? (the map's
// `clickRepeat`: the dagger's stab.) When it does, the directionless click
// names `prev` again and that repeat chains off it the way StrikeChains'
// opposite does. Base style index; form -1 (fists, no item) never repeats.
bool StrikeRepeats(const PlayerStrikeMap& map, int prev, int form);
// Which compass a player press reads: fists or armed. ONE armed compass for
// every weapon — what differs by weapon is the stroke's FORM, not which
// stroke a flick names. One function because the tick (session.cpp) and the
// HUD's strike compass must never disagree about it.
const PlayerStrikeMap& PlayerCompass(const StyleLibrary& lib, bool armed);

// Load assets/mobs/attack_styles.json. Follows every other loader here: a bad
// entry is skipped LOUDLY into `log` and is never fatal, and an unknown key is
// ignored so a newer authored file still loads on an older binary.
bool LoadAttackStyles(const std::string& path, StyleLibrary& out,
                      std::string& log);

// ---------------------------------------------------------------------------
// THE PROGRAM STATE of one live stroke, split from the NPC's swing (below)
// because TWO callers now replay authored programs through a MeleeState: a
// Mob's NpcStroke, and the player's discrete attacks (session.cpp), which own a bare cursor beside the player's own MeleeState. The split is
// exactly "what the stroke runner needs" — targets, edge memory and damage
// bookkeeping stay on NpcStroke, because the player's caller already owns
// those concerns its own way (main.cpp's sweep block and lastEdge* memory).
struct StrokeCursor {
  // GUARD is a WINDUP THAT NEVER ENDS: the same closed-loop, under-commitSpeed
  // drive to a stated pose, held indefinitely. It exists because "hold your
  // sword across this line" is a POSE and not an attack, and there was no way
  // to ask a creature for one — which is exactly what a defender in the block
  // gate, and a scripted encounter later, needs. Its target is ABSOLUTE
  // (`wantAz`/`wantEl` in the mob's own basis) rather than relative to an aim,
  // because a guard is not aimed at anything.
  enum class Phase : uint8_t { Idle = 0, Guard, Windup, Cut, Recover };

  Phase phase = Phase::Idle;
  int style = -1;
  int phaseTick = 0;         // ticks spent in the current phase
  int windupTicks = 0;       // after tempo jitter
  int cutTicks = 0;          // the WHOLE path, every leg (see "A CUT IS A PATH")
  int recoverTicks = 0;
  // ---- THE PATH, AS THIS SWING WILL ACTUALLY RUN IT -----------------------
  // The tempo jitter is applied PER LEG and resolved once at BeginStrokeProgram
  // rather than re-derived per tick: rounding each leg independently every tick
  // would let a boundary move under the drive, and a leg whose remaining ticks
  // changed mid-leg is a velocity step — the exact discontinuity `swing-smooth`
  // exists to catch. `cutTicks` is the sum of these, which is why nothing
  // outside this file has to know they exist.
  int cutLegs = 1;
  int legTicks[kMaxCutLegs] = {0, 0, 0, 0, 0, 0};
  // Which leg the cut is in right now, recorded for the dev readout and the
  // gates. Derived from `phaseTick` by the runner; never an input.
  int cutLeg = 0;
  // How many of `recoverTicks` are DRIVEN to the style's return pose before
  // the button is released (StrokeRecover::settle, resolved and clamped at
  // BeginStrokeProgram). 0 = release immediately, the historical recover.
  int settleTicks = 0;
  // ---- the frame program (AttackStyle::frames) ----------------------------
  int frames = 0;                  // how many frames this swing runs
  int frameTicks[kMaxFrames] = {}; // after tempo jitter
  int frame = 0;                   // the current one
  int frameTick = 0;               // ticks into it
  int firstCut = -1, lastCut = -1;
  // After the last frame: the arm is released and `releaseLeft` ticks of
  // hand-back remain. Also how a dropped guard ends (MobSystem::ClearGuard).
  bool releasing = false;
  int releaseLeft = 0;
  // Where the arm's shape was when the current frame started (blended FROM).
  float bladeFrom = 0.0f, elbowFrom = 0.0f, leanFrom = 0.0f;
  // The frame began on the TAKE-OVER tick, when the driver was still Idle and
  // had no shape of its own to read: capture on the next tick instead.
  bool fromPending = false;
  // The frame tick the shape was captured on (0, or 1 after a take-over): the
  // blend runs over the ticks left from there.
  int fromTick = 0;
  void StartRelease(int ticks) {
    phase = Phase::Recover;
    phaseTick = 0;
    releasing = true;
    releaseLeft = ticks > 0 ? ticks : 1;
    recoverTicks = releaseLeft;
  }
  uint32_t seed = 0;         // wielder ^ salt ^ startTick; every draw keys off it
  // The aim, resolved ONCE at the end of the windup and never refreshed.
  float aimAz = 0, aimEl = 0;
  bool aimed = false;
  // Where the windup is steering to, in the wielder's basis.
  float wantAz = 0, wantEl = 0, wantReach = 0;
  // The aim the last step was handed, which a KEYED frame's pose is turned
  // toward before the commit freezes `aimAz`/`aimEl` (StrokeKeyedPose).
  float liveAz = 0, liveEl = 0;

  // ---- WHAT THIS STROKE HAS ALREADY BRUISED (melee.h EdgeSweep::struck) --
  // Rig slots this swing has already delivered its BLUNT/BITE impulse to. On
  // the cursor rather than on the sweep because the sweep is one tick and the
  // impulse is one STROKE -- the thing that owns "this swing" is the thing
  // that owns the phase machine. Cleared by Reset(), so a new swing hits
  // afresh; a `std::vector` because a stroke meets a handful of slots at most
  // and a set would allocate for every one of them.
  std::vector<uint64_t> struck;
  // ...and whether this swing has already BITTEN (melee.h EdgeSweep::bitten).
  // A separate latch and not an entry in the set above, because a bite is once
  // per STROKE rather than once per slot: one pair of jaws closes on one
  // thing, however many bodies the head swept past on the way. Cleared by
  // Reset() with everything else.
  bool bitten = false;

  // ---- WHICH ARM, AND WHETHER IT IS THE AUTHORED ONE (dual wielding) -----
  // The hand this swing is thrown with, and whether that is the mirror of
  // the side its style was authored for (StrokeMirrored). Set by the caller
  // beside BeginStrokeProgram; a mirrored program runs in a MIRRORED BASIS
  // and its pose is reflected on the way to the rig (MirrorWeaponPose).
  Hand hand = Hand::Right;
  bool mirrored = false;

  bool Active() const { return phase != Phase::Idle; }
  bool Cutting() const { return phase == Phase::Cut; }
  void Reset() { *this = StrokeCursor{}; }
};

// ---------------------------------------------------------------------------
// THE LEFT HAND IS THE RIGHT, MIRRORED (dual wielding, 2026-09-27).
//
// Every held style is authored for the right arm, and the unarmed compass
// authors one punch per side. Re-authoring the whole library for the left
// hand would be a second copy of every stroke that drifts from the first the
// day either is tuned, so a stroke swung with the OTHER arm than its author's
// is the authored stroke REFLECTED through the body's sagittal plane:
//
//   * THE DRIVER runs in a mirrored basis — `right` negated. MeleeState keeps
//     everything in basis coordinates (melee.cpp ToWorld/ToBasis are the only
//     place the frames meet), so the authored azimuths, the keep-outs, the
//     seed read off the live blade and the aim bearing all reflect with it
//     and the driver never learns it is serving the other arm. Its hand sign
//     is the AUTHORED side's (+1), for the same reason.
//   * THE POSE the driver hands the rig is already reflected in position and
//     direction. What is not is what the basis cannot carry: the blade's
//     FLAT, a pseudovector (a reflection turns a right-handed edge frame into
//     a left-handed one, so the edge would trail — negated here), the torso
//     twist (a turn toward the wielder's right becomes one toward the left),
//     and a KEYED pose's joint rotations, which are rig-local (reflected as
//     (x, -y, -z, w), the quaternion of M R M with M = diag(-1, 1, 1)) along
//     with their aim yaws.
//   * THE WEAPON a natural style names swaps side (fist.R -> fist.L,
//     Mob::ArmForStyle), and so does its body clip if the library has the
//     mirror (`punch_r` -> `punch_l`, MirroredClipName); a clip with no
//     mirror is not played, because a right punch's body motion under a
//     left punch is worse than none.
//
// A style with no side (jaws) is never mirrored.
enum class StyleSide : uint8_t { None = 0, Right, Left };
struct AttackStyle;
StyleSide StyleSideOf(const AttackStyle& sty);
inline StyleSide StyleSideOfHand(Hand h) {
  return h == Hand::Left ? StyleSide::Left : StyleSide::Right;
}
bool StrokeMirrored(const AttackStyle& sty, Hand hand);
// Reflect what the basis could not (see above). Idempotent only in pairs:
// call it exactly once per pose, on the way to SetWeaponPose.
void MirrorWeaponPose(WeaponPose& wp);
// "punch_r" <-> "punch_l" (a trailing _r/_l); the name unchanged when it has
// no side suffix. The caller checks the library has the result.
std::string MirroredClipName(const std::string& clip);
// How much longer an arm in `condition` (Mob::HandCondition, 0..1) takes over
// a stroke: 1 at or above `melee.injuredArmFrom`, rising linearly to
// 1 + `melee.injuredArmSlow` as the condition nears 0. The multiplier
// BeginStrokeProgram's `slow` takes.
float InjuredArmSlow(float condition);

// ---------------------------------------------------------------------------
// THE RUNTIME: one live NPC swing.
//
// Owned by the Mob (one per creature), pure presentation state, never saved and
// never hashed. It holds its own MeleeState because that IS the stroke driver —
// an NPC that computed its own poses would be a second implementation of the
// feel, which is the thing melee.h's input surface exists to prevent.
struct NpcStroke : StrokeCursor {
  MeleeState melee;
  uint64_t targetId = 0;
  Vec3 targetPoint{};        // world voxels, where the blow was aimed
  // WHICH LIMB OF THE VICTIM was drawn from the style's `target` table, as a
  // rig slot on that victim; -1 = the chest (the historical aim, and what an
  // absent table means). Recorded rather than merely used so a gate can assert
  // the DISTRIBUTION — "40 bites chose at least two tags and not the head
  // every time" is a claim about the draw, and inferring it from where wounds
  // ended up would be measuring the sweep instead (CLAUDE.md rule 6).
  int targetLimb = -1;
  // ...and the LUNGE, once per stroke. `Mob::Launch` is idempotent-hostile (a
  // second call mid-flight would re-time the arc), and a phase can be stepped
  // more than once at a tempo jitter of zero ticks.
  bool lunged = false;
  // The blade's edge as it was last tick, for the damage sweep. Not valid on
  // the first cut tick — there is no previous position to sweep from, and
  // inventing one is a free hit at the start of every swing.
  Vec3 edgeBase{}, edgeTip{};
  bool edgeValid = false;
  // ...and the held item's HAFT (Mob::HaftEdge), swept beside it, with its
  // own once-per-stroke impulse set so a stick graze never spends the head's.
  Vec3 haftBase{}, haftTip{};
  bool haftValid = false;
  std::vector<uint64_t> haftStruck;
  // Set when a parry arrested this stroke (game/melee.h EdgeSweepResult), so
  // the dev readout and the gates can tell "it finished" from "it was stopped".
  bool arrested = false;
  // ---- WHAT THIS SWING DID, recorded at the point it happened -------------
  //
  // "The NPC does not seem to hit very hard" has at least four causes — the
  // sweep never ran, it ran and the blade was too slow to do anything
  // (MeleeTuning::minSpeed), it was fast enough and passed through nothing, or
  // it hit and the wound was small — and from outside all four are the same
  // bare zero (CLAUDE.md rule 6). These three separate them for the cost of
  // three words per stroke, and they are what the gates assert on.
  int sweeps = 0;        // ticks a damage sweep actually ran
  int bodiesHit = 0;     // summed over those ticks
  float topTipSpeed = 0; // fastest the edge went, world voxels/sec
  // ...and WHERE THE PROBE RAYS WENT, summed over the stroke (melee.h
  // EdgeSweepResult). A stroke that reports `bodiesHit 0` is answering a
  // question with a number that has four causes; these say which.
  int probesCast = 0, probesAir = 0, probesSelf = 0, probesBody = 0;

  // Shadows StrokeCursor::Reset on purpose: an NPC reset clears the whole
  // swing (melee state, edge memory, damage tallies), not just the program.
  void Reset() { *this = NpcStroke{}; }
};

// CAN THIS CREATURE SWING THIS STYLE AT ALL (plan §5)?
//
//   "held"  -> something is in its fist (HeldSlot() >= 0)
//   natural -> the def declares that weapon, its part is alive, and — for a
//              chain effector — so is every part of the chain that serves it.
//
// DECLARED HERE AND DEFINED IN mob.cpp, which is the one deliberate oddity in
// this header. `strokes.h` is included BY `mob.h`, so this file cannot see a
// Mob; the question is nevertheless part of the STYLE vocabulary, and putting
// it in mob.h would mean an author looking for "when does a style apply" has
// to know that the answer lives in the rig. `melee.h` already forward-declares
// Mob for exactly this reason and this header includes it.
bool StyleUsable(const Mob& who, const AttackStyle& sty);

// HOW FAR THIS STYLE REACHES ON THIS BODY, world voxels, centre-to-centre — the
// effector's own span plus whatever a lunge closes. Declared here and defined
// in mob.cpp for the same reason `StyleUsable` is (see its note): it is part of
// the style vocabulary, and it is the second half of the same question. A thin
// forward of `MobSystem::StyleReachOn`; 0 when the creature has no system, no
// def, or nothing to swing.
float StyleReachOn(const Mob& who, const AttackStyle& sty);

// Pick a style for one attack out of a profile's authored list. Counter-based
// on (mobId, tick) so the sequence replays; returns -1 when the list is empty
// or names nothing the library knows.
//
// `who`, when given, FILTERS: unusable styles are dropped, and then fallback
// styles are dropped as a group if any non-fallback style survived — so a
// duelist with a sword never throws a punch and a disarmed one throws nothing
// else. Null skips both filters, which is what a caller with no creature (the
// tuner's preview, a gate measuring the library itself) wants.
//
// ---- AND IT DRAWS ONLY FROM WHAT CAN LAND (2026-09-16) --------------------
//
// `distance` > 0 adds a third filter: a style whose own reach cannot cover it
// is not a candidate. THE ALTERNATIVE WAS A SILENT WASTED TURN, and it is the
// single biggest reason a zombie looks like it has stopped fighting.
//
// The draw used to be uniform over everything usable, and `BeginStroke` then
// refused the result if the target was outside THAT style's reach — dropping
// the attack after `ai::Think` had already advanced the cadence, the commit
// and the disengage clocks. A zombie's list is 50/50 between a 23-voxel lunging
// bite and a 3.3-voxel standing one, so at any distance past about four voxels
// HALF ITS SWINGS WERE DISCARDED BY ITS OWN DRAW, once every 48-tick cadence,
// with no log line anywhere (the refusal was commented "not a content error").
// Filtering first spends the turn on a style that can actually arrive.
//
// `slack` is the victim's own half-depth, the term `StyleReachOn`'s estimate is
// missing: it is a centre-to-centre number for a surface-to-surface event, and
// comparing it to an exact distance without it makes the test a coin flip at
// exactly the range creatures stand at (mob.cpp says so at length).
//
// Returns -1 when nothing can reach, which is a REAL answer and the caller is
// expected to say so — the creature is committed from further than anything it
// owns can serve, and that is worth a line.
int PickAttackStyle(const StyleLibrary& lib,
                    const std::vector<std::string>& names, uint64_t mobId,
                    uint32_t tick, const Mob* who = nullptr,
                    float distance = 0.0f, float slack = 0.0f);

// ---------------------------------------------------------------------------
// THE SHARED STROKE-PROGRAM RUNNER.
//
// The phase machine that turns an authored AttackStyle into per-tick
// StrokeSamples, extracted from MobSystem::StepStroke so the player's discrete
// attacks (session.cpp) replay the same programs through
// their own MeleeState. Neither caller owns a copy of the feel: this is the
// only place a windup, a cut or a recover is stepped, exactly as melee.cpp is
// the only place a stroke is integrated.

// Fill a cursor's program fields for one swing: style index, seed, the
// tempo-jittered tick counts, phase = Windup. The CALLER resets its own
// container first (an NpcStroke also clears its target/edge/tally fields; the
// player's bare cursor has nothing else) and owns re-seeding/re-tuning the
// MeleeState the program will drive — see MobSystem::BeginStroke for why the
// tuning is re-applied per swing.
//
// `windupRate` > 1 plays the WINDUP frames that many times faster (a chained
// strike, StrikeChains): each windup frame's ticks are divided by it before
// the two-tick telegraph floor, so a chain is quicker but never instant.
//
// `slow` >= 1 stretches EVERY frame by that factor — an injured arm
// (Mob::HandCondition, melee.injuredArm*) swings the same stroke at a
// slower tempo, and since speed is the damage (melee.h note 2) a slower cut
// is a weaker one with no second damage rule to keep in step.
void BeginStrokeProgram(StrokeCursor& cur, const AttackStyle& sty,
                        int styleIndex, uint32_t seed,
                        float windupRate = 1.0f, float slow = 1.0f);

// What one tick of the program did, so the caller knows whether to push a
// pose. Split three ways rather than a bool because the two "no pose" cases
// are different acts: Idle stepped nothing, Finished is the one tick where
// the caller must drop its pose claim (the NPC resets and pushes an empty
// WeaponPose; the player lets its MeleeState idle out).
enum class StrokeStepResult : uint8_t { Idle = 0, Live, Finished };

// One tick: synthesize the StrokeSample the current phase wants and Step the
// driver with it. `sty` may be null only for a Guard (a pose has no style).
// `liveAz`/`liveEl` are the aim's CURRENT bearing about the wielder's shoulder
// in the given basis — the NPC re-derives them from its target every tick and
// the windup tracks them until the commit freezes the aim; a player attack
// passes (0, 0), because the camera IS the aim. `dt` is the caller's tick.
// `liveDist` is HOW FAR THE TARGET IS from the same pivot `liveAz`/`liveEl`
// are measured about, in world voxels; 0 means "no target", which is what a
// player attack passes because the camera is the aim and a crosshair has no
// distance.
//
// IT BOUNDS THE CUT'S RADIUS, and that is the whole of what it is for. The
// authored `reach` offsets are positions in the arm's own BAND -- they say
// "chamber back a little, then drive to full extension" -- and nothing in them
// knows where the target is. A sword at ten voxels never notices: the band tops
// out well short of the victim and the blade covers the rest. A FIST AT TWO
// VOXELS DOES: the hand drove to 3.90 of a 4.7-voxel band at a chest 2.0 away
// and the knuckles sailed straight past it, which is the "a punch at arm's
// length misses" report. Clamped, the blow crosses the target instead of
// overshooting it, and every stroke already inside its band is unchanged.
StrokeStepResult StepStrokeProgram(StrokeCursor& cur, const AttackStyle* sty,
                                   MeleeState& m, float liveAz, float liveEl,
                                   float liveDist, float dt, const Vec3& right,
                                   const Vec3& up, const Vec3& fwd);

// An authored reach offset -> a radius the arm can actually serve. Public
// because the gates state their expectations in the same band positions the
// styles are authored in.
float StrokeReachIn(const MeleeState& m, float offset);

// The per-joint brakes in force RIGHT NOW: the current frame's (the last
// frame's through the release). What a caller hands WeaponPose::smooth.
const ArmSmooth& StrokeJointsNow(const StrokeCursor& cur, const AttackStyle& sty);

// A KEYED style's arm this tick ("A FRAME IS A POSE"), read off the cursor
// AFTER StepStrokeProgram, onto a pose the caller already filled from
// MeleeState::Pose(): sets `wp.keyed` while a frame is running and replaces
// the torso shares with the frames' own (faded by the release). A style that
// is not keyed leaves `wp` alone. Both stroke callers make this call right
// beside StrokeJointsNow.
void StrokeKeyedPose(const StrokeCursor& cur, const AttackStyle& sty, WeaponPose& wp);

// THE POSE A RUNNING STROKE HANDS THE RIG: the driver's (MeleeState::Pose),
// the current frame's joint brakes, and -- for a KEYED style -- the frames'
// joints and torso. The one call both stroke callers make, and the one a gate
// that ticks a stroke by hand must make too, or it measures the driver's arm
// where the game shows the keyed one.
WeaponPose StrokePoseNow(const StrokeCursor& cur, const AttackStyle* sty,
                         const MeleeState& m);

// A WORLD POINT -> THE THREE NUMBERS `StepStrokeProgram` AIMS WITH.
//
// The bearing of `target` about `pivot`, in the basis the stroke is integrated
// in, plus the distance between them. Extracted from MobSystem::StepStroke
// when the player's discrete strikes became a second caller, for the reason
// the runner itself was: two implementations of "where is the target" is two
// implementations of where a blow lands.
//
// THE PIVOT IS NOT THE EYE, and that is the whole reason this exists. A player
// strike used to pass (0, 0, 0) — "the camera IS the aim" — which is true only
// of a DIRECTION, and a punch is not a direction: the fist swings about a
// shoulder that sits ~2.5 voxels under the eye and ~2 to the side of it, so at
// arm's length a bearing copied from the camera lands the knuckles a shoulder
// offset low and wide of whatever the crosshair is on. At sword range the same
// error is a few degrees; at punching range it is the difference between a
// head and a collarbone.
void StrokeAimAt(const Vec3& pivot, const Vec3& target, const Vec3& right,
                 const Vec3& up, const Vec3& fwd, float& outAz, float& outEl,
                 float& outDist);

