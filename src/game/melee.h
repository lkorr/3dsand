#pragma once
#include "sim/scale.h"  // MetresToCells / MetresPerSecToCells
#include <cstdint>
#include <vector>

#include "game/anim.h"   // Quat, for WeaponPose::Keyed
#include "game/item.h"
#include "math3d.h"

// THE STROKE DRIVER — one control law, three feeders (2026-09-01).
//
// The prose below describes the FREEFORM mouse-steer experiment the driver was
// built for. THAT MODE IS GONE (2026-09-25, with the driver's own
// speed-triggered Slash and follow-through arc): the player's only control is
// DISCRETE STRIKES — a click fires an authored stroke program from
// attack_styles.json, direction picked by the flick at the press
// (game/strike_pick.h), replayed through this same driver by the same
// StepStrokeProgram the NPCs use (game/strokes.h). Everything below the input
// layer — the tip-on-a-reach-sphere control law, the derived blade frame, the
// damage sweep, parry, block — is shared by all three feeders and unchanged.
//
// MOUSE-DIRECTED MELEE (the "half sword" experiment; REMOVED — history).
//
// THE IDEA. A swing is not an animation you trigger, it is a motion you make.
// Hold the attack button and you TAKE OVER THE BLADE: the sword stays exactly
// where the animation had it, and from that instant the mouse MOVES it — every
// pixel of motion is a fixed amount of blade travel, added to where the blade
// already is. Move briskly and it commits — the cut accelerates along the
// direction you actually moved, so a diagonal flick is a diagonal cut and a
// flat sideways sweep is a flat sideways cut. Nothing selects from a list of
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
// THE MOUSE STEERS THE TIP, NOT THE HAND (2026-08-31). The second version fixed
// both of those by integrating mouse pixels into a HAND POSITION in the camera
// plane — and inherited a worse problem from the geometry rather than from the
// input. A hand offset is THREE numbers fed to a two-bone solver with a fixed
// pole vector and a strict-hinge elbow; shoulder roll, forearm pronation and
// the wrist have no channel at all, and the blade's own orientation was
// computed, passed, stored and then deliberately never applied. So the sweep
// plane was whatever the elbow's authored hinge happened to allow, the blade
// pointed wherever the walk cycle had left the fist, and the player's report
// was the honest description of what the code did: "moving the mouse forwards
// and backwards just jabs the hand forward; it seems to control the elbow".
//
// The control surface is now the BLADE TIP on a reach surface around the
// shoulder, in the camera's frame:
//
//     mouse x  ->  AZIMUTH of the tip   (right/left around the character)
//     mouse y  ->  ELEVATION of the tip (overhead/low)
//     radius   ->  a separate, bounded channel (FeedReach); a thrust
//
// and EVERYTHING ELSE IS DERIVED FROM IT. The blade points roughly along the
// radius (leaning into the direction the tip is travelling), so the hand is
// simply the tip minus a blade length; the flat of the blade faces out of the
// stroke plane, so the edge leads the cut; the arm's bend plane IS the stroke
// plane, so a horizontal cut reads as shoulder rotation plus elbow extension
// in the horizontal plane, and a vertical cut as shoulder elevation in the
// vertical plane. The limbs serve the blade rather than the other way round.
//
// WHY BOTH MOUSE AXES ARE ANGULAR, and the thrust is not on them. Making the
// vertical axis do double duty as "reach" is precisely the bug above: a
// forward push then reads as an extension of the elbow instead of raising the
// point, and a right-to-left drag stops being a flat arc as soon as the
// player's hand wanders off the horizontal. Two angles are what a sweep needs;
// the radius is real, bounded and separately fed, so an NPC (or a future
// thrust binding) can lunge without stealing an axis the sweep depends on.
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

// ---- THE STROKE DRIVER'S INPUT SURFACE (Phase C: NPC attacks) ---------------
//
// The control law below never sees a mouse. It consumes ABSTRACT CONTROL
// DELTAS — two numbers per tick plus a button — and the player's binding is one
// line in main.cpp that forwards raw pixels into Feed(). An NPC attack is the
// same driver fed an AUTHORED CURVE instead: a polyline of the same deltas,
// replayed one sample per tick, which is exactly what a human hand produces.
//
//   MeleeState m;
//   m.SetStroke(handFromShoulder, tipFromShoulder, armReach);  // from the rig
//   for (const StrokeSample& s : script)
//     m.Step(s, dt, /*armed=*/true, right, up, fwd);
//   mob.SetWeaponPose(m.Pose());
//
// `right`/`up`/`fwd` are a BASIS, not a camera: a mob passes its own facing
// frame and gets the same stroke relative to where IT is looking. Nothing in
// here is player-specific, and SetWeaponPose lives on Mob rather than on
// PlayerAvatar for the same reason.
struct StrokeSample {
  float dx = 0, dy = 0;   // control delta this tick (mouse pixels for a player)
  float dReach = 0;       // radial channel: + thrusts the point out, - draws it back
  bool held = true;       // the attack button; false releases the stroke
};

// The driver's state machine. It has NO CUT OF ITS OWN (2026-09-25): the
// speed-triggered Slash and its follow-through arc were the freeform mouse
// melee's, and on an authored stroke they were a second controller fighting
// the program. Whether a swing is cutting is the PROGRAM's phase
// (StrokeCursor::Cutting, game/strokes.h), never this.
enum class SwingPhase : uint8_t {
  Idle = 0,   // weapon at rest, following the walk cycle
  Guard,      // driven: the input owns the tip, moving it 1:1
  Wind,       // driven and moving briskly (what Arrest reads as a live cut)
  Recover,    // released or arrested; the arm is handed back
};

// Tunable feel.
//
// THE VALUES NOW COME FROM tuning.json (`melee.*`, sim/tuning.h Tuning::Melee),
// applied through ApplyMeleeTuning below. The struct stayed exactly where it
// was and kept exactly its fields, because it is what MeleeState holds, what
// MeleeSweepDamage takes by const reference, and what every gate reads — moving
// the numbers is a different change from moving the type, and only the first
// one was wanted. The initialisers below are still the DEFAULTS in the strict
// sense: they are what you get when the JSON is missing, and tuning.json ships
// values identical to them, so behaviour at defaults is unchanged.
//
// The one number that DID move is `wristMaxAngle` (2.80 -> 1.50), and it moved
// because the grip it was compensating for was re-authored — see its own note.
struct MeleeTuning {
  // A REFERENCE DRIVE SPEED, input units per second. Nothing commits on it any
  // more (the driver's own Slash is gone): 35% of it is where Guard reads as
  // Wind, the stroke programs' closed-loop steer stays under it, and the swing
  // whoosh maps speed to volume against it.
  float commitSpeed = 900.0f;
  float recoverTime = 0.22f;
  // Blade tip speed (world voxels/sec) at and above which a hit does full
  // damage; below it damage falls off linearly to zero. This is what makes a
  // committed cut different from waving the weapon around.
  // Dead initialiser: melee.cpp fills this from Tuning::Melee (fullSpeedMps).
  // Kept equal to the tuning.h default so nobody reads two truths.
  float fullSpeed = MetresPerSecToCells(20.0f);
  float minSpeed = MetresPerSecToCells(0.9f);
  // HOW FAR THE TIP SWINGS PER CONTROL UNIT — RADIANS per mouse pixel, not
  // pixels/sec and not voxels. This is the whole control law now: the delta is
  // a displacement of the point on its reach sphere, integrated, so 200 px of
  // mouse is the same arc whether you took a tenth of a second or two seconds
  // over it.
  //
  // The defaults preserve the previous law's sensitivity exactly: it moved the
  // hand 3.0 mm/px horizontally at a 0.60 m arm, which is 0.0050 rad/px, and
  // 4.0 mm/px vertically, which is 0.0067 rad/px. Vertical stays higher for the
  // same reason it always was — hip to overhead is a short range and the player
  // runs out of mousepad, while a sweep has the whole width of an arc.
  //
  // Both are POSITIVE — screen-right swings the point right and screen-up
  // raises it. The very first (velocity) law mirrored X on the theory that you
  // push the hilt right to bring the edge left; that argument belongs to a
  // blade being AIMED from a fixed fist, and it is simply backwards for a point
  // being CARRIED.
  float aimGainX = 0.0050f;
  float aimGainY = 0.0067f;
  // The radial channel: world voxels of tip reach per unit of StrokeSample::
  // dReach. Nothing on the player's mouse feeds this yet (see the header note
  // on why neither mouse axis may be spent on it); it exists so an NPC thrust
  // and a future dedicated binding steer the same state.
  float reachGain = MetresToCells(0.0030f);
  // WHERE THE POINT MAY GO, radians. Asymmetric in azimuth because an arm is:
  // the weapon side has a whole sweep behind it, the far side runs out at the
  // midline. `handSign` in MeleeState says which side is which. These bound the
  // STORED state, so pushing into one banks nothing.
  //
  // azOut WAS 2.36 (135 degrees), and that is BEHIND THE CHARACTER. The stroke
  // is an arc about the shoulder measured from the basis's forward, so 135
  // degrees to the weapon side puts the commanded point past the frontal plane
  // and parks the shoulder ball on its authored `reach` stop (`normal
  // [0,0,-1], max 50` on every humanoid def — 50 degrees past the back plane
  // and no further). What the player sees is the arm driven behind the body
  // and then stuck there, which was reported as exactly that. 1.83 rad is 105
  // degrees: the whole front, plus fifteen degrees past side-on so a
  // right-to-left sweep still starts from a real wind-up rather than from the
  // shoulder line.
  float azOut = 1.83f;      // 105 deg to the weapon side: front + a little past
  float azAcross = 1.40f;   // 80 deg across the body
  // Elevation runs almost to straight down because THE SEED HAS TO FIT: a
  // walk cycle leaves the weapon arm hanging at roughly -85 degrees, and a
  // window that could not represent it would move the blade on the take-over
  // tick — the one thing the whole design forbids.
  float elMin = -1.50f;     // 86 deg below level: the arm hangs at the side
  float elMax = 1.48f;      // 85 deg: overhead, just short of straight up
  // HOW EXTENDED THE ARM IS HELD, as a fraction of its own reach. This is the
  // number that decides the blade's angle to the arm, and it decides it by
  // GEOMETRY rather than by taste: given where the point is and how long the
  // blade is, the hand can only be one distance from the shoulder, so fixing
  // that distance fixes the angle (see RebuildFrame's law of cosines).
  //
  // It replaced a fixed "the blade lies along the radius, leaning a little into
  // the travel", which is a fine description of a cut and an unusable control
  // law: this sword is 1.1 m and the arm is 1.0 m, so a radial blade put the
  // HAND at the shoulder and the IK folded the arm into the chest. Measured,
  // the sword then missed its commanded point by up to 19.8 voxels on a 11.2
  // voxel stroke — the arm was not following the mouse at all.
  //
  // 0.78 is a comfortably bent arm: enough extension to swing from, enough bend
  // left that a cut can drive through rather than starting locked.
  float handExtend = 0.78f;
  // Seconds of halflife on the arm's extension easing back to `handExtend`
  // after a take-over started it somewhere else. Slower than the blade's own
  // smoothing on purpose: this is the arm settling, not the wrist working.
  float extendSmoothing = 0.18f;
  // How fast the lean plane may rotate about the radius, radians/sec. A rate
  // limit rather than a halflife because the thing being limited is an ANGLE on
  // a circle: an exponential blend between two opposite directions has to pass
  // through the middle, and on this circle the middle is the degenerate case.
  float leanTurnRate = 18.0f;
  // WHICH END LEADS. +1 puts the HAND ahead of the point through the arc — a
  // sabre cut, where you drive the hilt and the blade whips through behind it.
  // -1 puts the point ahead. Only the SIGN is read: the size of the lean is
  // already fixed by handExtend above, and letting this scale it too would let
  // two knobs disagree about where the hand is.
  float handLead = 1.0f;
  // Fallback arm reach, used only when the rig has not reported one through
  // MeleeState::SetStroke (no weapon equipped yet, or a severed arm). The live
  // rig's own bone lengths are preferred — see SetStroke.
  float fallbackReach = MetresToCells(0.60f);
  // The HAND may be driven no further from the shoulder than this fraction of
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
  // ---- the derived half: blade, plane, arm --------------------------------
  // Seconds of halflife on the blade frame and on the arm's bend plane. This
  // is what makes a take-over continuous — control starts at the blade's ACTUAL
  // orientation and eases to the commanded one — and what stops the flat of the
  // blade from snapping through 90 degrees when the tip changes direction.
  float bladeSmoothing = 0.055f;
  // How far the wrist may take the blade away from the orientation the solved
  // arm would give it for free, radians.
  //
  // IT USED TO BE 2.80, then 1.50, and the number chased a GRIP that has since
  // moved back. The history is worth keeping because the reasoning failed the
  // same way twice.
  //
  // The overhaul re-authored assets/items/{sword,cleaver}.json from
  // `[0, -90, 0]` to `[0, 0, -90]` so that a NEUTRAL wrist laid the blade down
  // the arm's own line, on the theory that a cut wants a radial blade and the
  // wrist should not spend its budget undoing the grip. The budget argument
  // did not survive contact: measured on the repaired swing-plane fixture, the
  // wrist asks for 2.6..3.1 rad WHICHEVER grip is authored (the ask is
  // dominated by ROLL, not by direction), so the cap ended at 3.10 anyway and
  // the grip change bought nothing it was authored for.
  //
  // What it COST was the pose the player looks at for 99% of a session. At
  // Idle the item renders at `handQ * gripRot` with no steering at all (Mob::
  // ApplyWeaponArm returns at weight 0), so an along-the-arm grip is a hilt
  // buried in the fist and a blade lying down the forearm — reported as "the
  // hilt sticks through the hand". THE GRIP IS BACK TO `[0, -90, 0]`, verified
  // rather than reasoned:
  //
  //   python scripts/geometry.py euler_to_quat 0 -90 0   (blade is item +x)
  //   -> blade +x lands on hand +z (forward, out of the fist)
  //   -> with the arm out in front, that same +z is world UP: a ready guard
  //
  // 3.10 is kept because the ask is unchanged: the blade wants direction AND
  // roll, the neutral grip stands about pi of roll away from a committed cut,
  // and the STEERING gets this number minus that standing tax (~1.1 rad free).
  // Lives in tuning.json as `melee.wristMaxAngle`, so it is a JSON edit rather
  // than a rebuild.
  float wristMaxAngle = 3.10f;
  // ---- HOW MUCH OF THE WRIST'S ALIGNMENT IS ACTUALLY APPLIED --------------
  //
  // THE WRIST RAMPS WITH COMMITMENT. `wristMaxAngle` above is a CEILING; this
  // pair is the throttle, and it exists because a ceiling alone cannot express
  // the difference between a cut and a hold.
  //
  // RebuildFrame always commands the blade along the law-of-cosines direction,
  // which is mostly radial — correct for a cut, and wrong for standing still,
  // because it wrenches the fist round to lay the blade along the shoulder-to-
  // point line whatever the player is doing. Holding a guard out in front then
  // pointed the sword straight down the radius instead of up out of the fist,
  // which is what "the sword points vertically DOWN" was.
  //
  // So the applied alignment is scaled by the TIP'S OWN SPEED: a still blade
  // keeps the orientation the solved forearm gives it for free (the authored
  // grip: blade out of the fist, up when the arm is forward), and a moving one
  // is aligned to the stroke. Both ends are real poses and the ramp between
  // them is smoothed, so there is no tick where the blade jumps.
  //
  // Speeds are TIP speed in world voxels/sec, the same quantity the damage
  // ramp is stated on. ONLY SLASH bypasses the ramp — a cut is a cut even at
  // the moment it reverses through zero. Wind and Recover used to bypass it
  // too, and that was the owner-reported "overhead strikes just rotate the
  // wrist at the cursor": raising the sword is Wind (moderate speed, held),
  // so the raise itself wrenched the blade onto the radius instead of letting
  // it ride the grip (up out of the fist). The band is also wider now
  // (0.6..3.0 m/s, up from 0.25..1.10) for the same reason: a deliberate
  // raise moves the tip at 1-2 m/s and should mostly keep the grip pose;
  // only near-cut speeds earn full alignment.
  float steerSpeedLo = MetresPerSecToCells(0.6f);    // below: grip pose
  float steerSpeedHi = MetresPerSecToCells(3.0f);    // above: full alignment
  // What is applied at and below `steerSpeedLo`. Not zero: a guard that shares
  // NOTHING with the stroke reads as the weapon having been let go of, and the
  // take-over seed (SetStroke reads the rig's real blade every tick) is
  // smoother when the two poses are near neighbours.
  float steerFloor = 0.15f;
  // ---- PER-JOINT SMOOTHING, both halflives in seconds ---------------------
  //
  // Two knobs because the two joints have different jobs and different
  // tolerances for lag. The ARM carrying the hand can lag the mouse a little
  // and read as weight; the WRIST lagging a committed cut misaligns the edge
  // and costs real damage (MeleeEdgeAlign), so the two must be tunable apart.
  //
  // `armSmoothing` eases the integrated stroke (az/el/radius, follow-through
  // included) before the tip is built from it, so everything derived — hand,
  // bend pole, blade — inherits the same lag and stays one rigid assembly.
  // 0 is off (the pre-knob behaviour, bit for bit).
  float armSmoothing = 0.04f;
  // `wristSmoothing` owns BOTH halves of the wrist's motion: the commitment
  // envelope (attack = half this, release = four times it — the asymmetry is
  // measured, see RebuildFrame) and the chase of the commanded blade
  // orientation itself. Replaces the old derivation from
  // bladeSmoothing, which conflated the wrist's feel with the internal frame
  // continuity mechanism.
  float wristSmoothing = 0.10f;
  // ---- THE HAND STAYS IN FRONT OF THE BODY --------------------------------
  //
  // How far BEHIND the shoulder's frontal plane the hand may sit, as a
  // fraction of arm reach. The tip window (azOut 105 deg) already keeps the
  // COMMANDED POINT essentially in front, but the hand is the tip minus a
  // whole blade (RebuildFrame: `handL = tipL - bladeDir * bladeLen`) plus the
  // lean term, and nothing bounded THAT — measured, it sat several voxels
  // behind the plane at the azimuth stops, which is "the upper arm goes
  // behind him and sticks there". The clamp holds the hand at the plane and
  // re-aims the blade at the commanded tip from the clamped hand, so a
  // wind-up still angles the blade back while the arm stays in front (the
  // note in RebuildFrame has the A/B against keeping the solved lean).
  float handBackFrac = 0.05f;
  // ---- WHAT THE ARM MAY DO WHILE IT SERVES THE BLADE ----------------------
  //
  // THE ELBOW IS A ONE-WAY HINGE AND THE BEND PLANE IS NOT FREE. The stroke
  // hands the rig a bend pole (the plane the elbow bulges in) and the rig
  // turns it into a hinge-axis override, and BOTH were unbounded: any pole
  // direction was legal, so the solve plane could point anywhere, and
  // ApplyWeaponArm then sign-flipped the override so the measured bend was
  // always positive — which turns an authored `[0, 130]` hinge into a rubber
  // stamp that legalises a backwards elbow. Reported as "the elbow bends both
  // ways" and "the shoulder rolls freely", which are the same fact seen twice.
  //
  // `elbowPoleCone` bounds the POLE, here, in the driver's own basis frame:
  // radians away from straight-back ([0,0,-1], which is these rigs' authored
  // arm pole and where a resting elbow points). 1.75 rad is 100 degrees — the
  // elbow may bulge down, up or out to either side, and may not come forward.
  //
  // `elbowAxisCone` would bound the OVERRIDE AXIS in the rig, as radians away
  // from the forearm's AUTHORED hinge axis. IT DEFAULTS TO pi, i.e. OFF, and
  // the long note in mob.cpp says why: penning the axis makes the pose clamp
  // LOSSY (it keeps only the component about the axis it is given), and a
  // horizontal cut legitimately wants a bend plane 90 degrees off the resting
  // axis. Measured at 75 degrees it cost pass A of swing-plane 1.76 rad of
  // elbow clamp and 4.42 voxels of hand. It stays as the one-JSON-edit A/B.
  float elbowPoleCone = 1.75f;
  float elbowAxisCone = 3.14f;   // pi = off; see game/mob.cpp for why
  // ---- the edge leads the cut ---------------------------------------------
  // ---- BLOCKING IS EMERGENT (Phase C) --------------------------------------
  //
  // There is NO block button and no block state. A parry happens when two
  // swinging blades cross — both combatants must be in a committed cut for the
  // geometry test to fire. A held weapon that is not swinging is not a parry.
  //
  // The four numbers below are what a parry costs. They live in tuning.json as
  // `melee.block*` with the rest of the group (promoted at the phase C/D merge)
  // and are editable in the in-game Combat panel's Damage tab.

  // HOW CLOSE TWO EDGES HAVE TO PASS TO COUNT AS A PARRY, world voxels. Not a
  // fudge factor: it is the two blades' own thickness plus the width of the
  // guard and the fist behind them, none of which the authored `edge` segment
  // (a LINE down the cutting edge) represents. A blow that passes within a
  // couple of centimetres of a raised sword has been stopped by it, and a
  // player who watched that go through would be right to call it a bug.
  //
  // 0.14 m -> 0.22 m at the phase C/D merge, on the reasoning that the
  // along-the-arm grip left a defender's edge further from its own fist so a
  // crossing parry closed on it with more slack (measured then: 2.42 voxels
  // where the old grip gave 0.86).
  //
  // THE GRIP CAME BACK AND THE NUMBER DID NOT, and that is deliberate rather
  // than an oversight. Re-measured on `npc-block` with the restored
  // `[0,-90,0]` grip, the two swept edges close to 2.18 voxels — so 0.22 m
  // (2.20 voxels) is sized for the geometry that SHIPS, and it is sized by 1%.
  // The 0.86 figure belonged to a guard pose two rewrites ago and reverting to
  // 0.14 on the strength of it would make every parry in the game miss. If
  // `npc-block` starts flapping, this is the knob; measure the closing
  // distance the gate prints before touching it.
  float blockGap = MetresToCells(0.22f);
  // Fraction of the blow's own damage the BLOCKING WEAPON takes as item hp.
  // Items have hp and are severable (item.h ItemDef::hp/severable), so this
  // needs no new mechanism — a sword that blocks all day eventually breaks, and
  // one that blocks something far too heavy is knocked out of the hand by the
  // existing severImpactSpeed rule. 1.0 would make a parry cost the defender's
  // weapon exactly what it would have cost their arm, which is too much: an arm
  // absorbs a cut by being cut, a blade absorbs it by being a blade.
  float blockItemDamage = 0.35f;
  // Tuning::Melee::repeatHitScale: hp multiplier compounding per repeat
  // strike of one stroke on one creature (EdgeSweep::victimHits).
  float repeatHitScale = 0.5f;
  // THE SWEEP'S DENSITY (MeleeSweepDamage): the widest gap, world voxels,
  // between two rows of probe rays across one tick's travel, and the most rows
  // a sweep may cast. Dead initialisers: filled from Tuning::Melee
  // (sweepSpacingM, sweepMaxSteps), kept equal to its defaults.
  float sweepSpacing = MetresToCells(0.05f);
  int sweepMaxSteps = 24;
  // How far a blocked blow beats the DEFENDER'S guard open, radians of stroke
  // azimuth/elevation at full power. Blocking must not be free: a heavy blow
  // that lands on your blade should still move it. Hash-seeded and bounded, so
  // it is a deterministic shove rather than a random one (CLAUDE.md rule 1).
  float blockNudgeAz = 0.30f;
  float blockNudgeEl = 0.18f;
  // Damage multiplier for a cut whose travel lies exactly in the FLAT of the
  // blade — a slap with the side. 1.0 disables edge alignment entirely; 0 makes
  // a flat hit free. Real flats still bruise and still break bones, so this is
  // a floor rather than a gate, and it is what makes rolling the blade into the
  // cut worth doing.
  float edgeFloor = 0.35f;
  // ---- the body serves the swing -------------------------------------------
  // Fractions of the SMOOTHED stroke's azimuth/elevation the torso carries
  // (WeaponPose::torsoTwist/torsoPitch), the way avatar.headLookSpine shares
  // the look into the chest. Scaled by PoseWeight inside Pose(), so the lean
  // fades exactly with the arm claim and 0 is the arm-only A/B.
  float torsoShare = 0.35f;
  float torsoPitch = 0.20f;
  // ---- the blade stays out of the wielder's own face ------------------------
  // World voxels of clearance beyond the head's own radius the hand->tip
  // segment is held out to (RebuildFrame, after the handBackFrac clamp). The
  // keep-out sphere itself arrives per tick via SetKeepOut, because only the
  // rig knows where its head is. 0 = clamp OFF, the one-JSON-edit A/B.
  float headClear = MetresToCells(0.06f);
  // ---- ...AND OUT OF THE WIELDER'S OWN CHEST (2026-09-21) ------------------
  //
  // World voxels of clearance beyond the torso capsule's own radius the HAND
  // is held out to. Same shape as `headClear` and the same off switch (0), and
  // it exists because the head sphere covered the one body part an authored
  // windup could sweep a BLADE through and left the one an ARM actually goes
  // through untouched: a backhand's windup drives the commanded point to the
  // far azimuth stop, the hand is that point minus a whole blade, and it lands
  // inside the ribs with the forearm following it there. Reported as arms
  // clipping through the body; game/selfclip.h is what made it a number.
  //
  // The capsule arrives per tick via SetBodyKeepOut, because only the rig
  // knows where its own spine is (Mob::BodyKeepOut). THE HAND is what is
  // pushed, not the tip: the tip is the ASK and the hand is the ARM, and
  // pushing the ask would move where the cut lands — the blade is re-aimed at
  // the commanded point from the pushed hand, which is exactly the treatment
  // `handBackFrac` already gets and for the same reason.
  //
  // 0.10 m is a hand's width off the ribs: enough that a crossed guard reads
  // as being in front of the chest rather than in it, small enough that a
  // backhand windup still chambers across the body.
  float bodyClear = MetresToCells(0.05f);
  // ---- THE LEAN PLANE DOES NOT CHASE A REVERSAL (2026-09-21) -------------
  //
  // `leanTurnRate` above bounds how fast the blade's lean plane may rotate
  // about the radius, and a flat rate is the wrong KIND of bound at the one
  // place it matters. The plane chases the tip's own TRAVEL, so a reversal —
  // the end of every cut, and every tick of a follow-through unwinding — asks
  // it to turn by pi. Rotating `perpL_` by pi carries the HAND through an arc
  // of pi * bladeLen * sin(lean) about the shoulder-to-point line: on a sword
  // that is the whole arm whipping round a point that has nearly stopped, and
  // it is the reported "the sword spins near the end of the swing". The rate
  // limit does not prevent it, it only makes it take 0.17 s.
  //
  // RADIANS. A commanded turn larger than this is treated as a REVERSAL
  // rather than as a turn, and the plane holds its lean instead of swapping
  // which side the hilt leads on — which is what a real blade does. Anything
  // smaller is honest tracking and is chased as before.
  //
  // ---- IT SHIPS AT pi, WHICH IS OFF, AND THAT IS THE FINDING --------------
  //
  // Nothing exceeds a half turn, so pi never fires. The mechanism is here,
  // measured and reachable by one JSON edit, and it is NOT ON, because every
  // value tight enough to matter costs more than it buys:
  //
  //   * At 1.75 (100 degrees) the blade's unasked-for rotation drops by about
  //     a radian per swing — and `swing-plane` catches the elbow bulging
  //     0.674 rad in front of its own shoulder-to-wrist line against an
  //     authored 0.50, `player-styles` loses a thrust's posed travel, and
  //     `npc-strike` goes with them.
  //   * The bound tried FIRST — hold the plane to the arc the tip is actually
  //     covering, so the arm can never outrun the point — fails the same two
  //     gates for the same reason, and sweeping its ratio found exactly ONE
  //     value (4) that threaded between them, with 2 and 8 each failing a
  //     different one. A knob tuned to thread a needle between two thresholds
  //     is a flake, not a fix.
  //
  // So the spin is diagnosed, instrumented (`--gate swing-smooth` reports the
  // blade's direction arc per stroke) and left alone. Whoever takes it on
  // next needs the ARM's constraint solved with the plane's, not against it.
  float leanFlipHold = 3.14f;
  // ...and the tangential speed below which there is no travel to chase at
  // all, world voxels/sec. Under this the plane simply HOLDS — a decelerating
  // or stalled tip reports a tangent that is mostly noise, and re-aiming the
  // whole arm at noise is the other half of the same spin. Stated as a
  // fraction of `steerSpeedLo` would couple two unrelated feels; 0.05 m/s is
  // a blade that has stopped.
  float leanMinSpeed = MetresPerSecToCells(0.05f);
  // ---- HOW FAR OFF ITS OWN LINE THE TRAVEL HAS TO BE TO SET THE ROLL ------
  //
  // The blade's flat is `bladeDir x travel`, whose LENGTH is the sine of the
  // angle between them — so a thrust, where the two are nearly parallel, gives
  // a normal whose DIRECTION is almost pure noise. The guard used to be
  // 1e-3, which is three hundredths of a degree: in practice the roll was
  // being decided by numerical dust for the whole of every thrust and every
  // stall. Below this sine the previous roll is held, which is what a real
  // blade does. 0.20 is 11.5 degrees.
  float flatMinSin = 0.20f;
};

// ---- WHERE THE NUMBERS COME FROM -------------------------------------------
//
// Fills a MeleeTuning from the `melee.*` group of tuning.json (sim/tuning.h
// Tuning::Melee). Call it at startup and again on every F5, which is the whole
// point: the feel loop is a JSON edit and a keypress, not a rebuild.
//
// A FREE FUNCTION taking the MeleeTuning by reference, rather than a
// constructor or a member on MeleeState, for two reasons that are both about
// the merge surface rather than about style:
//
//   * MeleeTuning is consumed by three callers (the player's tick, the NPC
//     driver, the gates) as a plain aggregate, and giving it a non-trivial
//     constructor would change how every one of them may declare one;
//   * a gate that wants the AUTHORED feel calls this, and a gate that wants a
//     fixture with one knob moved builds a default MeleeTuning and assigns the
//     field — neither has to know that tuning.json exists.
//
// `Tuning` is forward-declared: this header is included by mob.h, and pulling
// sim/tuning.h in here would put the whole tuning surface into every rig TU.
struct Tuning;
void ApplyMeleeTuning(MeleeTuning& dst, const Tuning& t);
// Same, from CurrentTuning(). The form every caller outside a gate wants.
void ApplyMeleeTuning(MeleeTuning& dst);

// HOW MUCH OF A CUT LANDS, given the blade's roll. `flat` is the normal of the
// blade's cutting plane (the flat's outward direction) and `travel` is where
// the edge is going; both world, neither need be normalized. Returns
// `floorFrac` for a pure flat-on slap rising to 1 for a perfectly edge-on cut.
//
// A free function rather than a method because BOTH the live damage sweep and
// the gate that measures it must use the same one — a test carrying its own
// copy of this formula would be a second source of truth that passes while
// measuring nothing.
float MeleeEdgeAlign(const Vec3& flat, const Vec3& travel, float floorFrac);

// A BLADE MET A BLADE. Reported out of the sweep rather than acted on inside
// it, for the same reason MobSystem reports sever and voice events instead of
// playing sounds: this layer knows nothing about audio, and both ends of a
// parry are owned by callers it cannot reach (the player's MeleeState lives in
// main.cpp, an NPC's in its Mob). Phase D turns this into the clang and the
// spark; MobSystem accumulates them so one drain serves both directions.
struct BlockEvent {
  uint64_t attackerId = 0;   // whose stroke was stopped
  uint64_t blockerId = 0;    // whose weapon stopped it
  uint64_t blockerBody = 0;  // the item's Jolt body, so a caller can charge it
  Vec3 at{};                 // world voxels, where the blades met
  float power = 0;           // 0..1, speed x edge alignment of the blow
};

// A BLOW LANDED ON A CREATURE (2026-09-28). Pushed by MeleeSweepDamage, only
// for a sweep that asks (EdgeSweep::reportStrikes), the FIRST time a stroke
// meets each victim -- one blow, one sound, not one per cut tick. The player's
// own sweep does not ask: it latches its cues by differencing the mob queues
// (session.cpp resolveSweep). An NPC's did not do anything at all, so an enemy
// hitting you made no weapon or gore sound; this is that sound's source.
struct StrikeEvent {
  uint64_t attackerId = 0;
  uint64_t victimId = 0;     // the creature struck (the player's actor id too)
  Vec3 at{};                 // world voxels, the probe's contact point
  float power = 0;           // 0..1, speed ramp x edge alignment
  float gainDb = 0;          // EdgeSweep::cueGainDb (a haft's softer level)
  bool flesh = false;        // met flesh (live or dead) rather than a shell
  bool edged = false;        // strike.cut > strike.blunt: ring + wet cut layer
};

// One resolved cut of the blade this tick: the quad the edge swept, how fast it
// was going, and which way its flat was facing while it did. MeleeSweepDamage
// turns this into carves.
struct EdgeSweep {
  Vec3 aPrev{}, bPrev{};   // edge base/tip last tick, world voxels
  Vec3 aNow{}, bNow{};     // edge base/tip now
  Vec3 flatNow{};          // blade's flat normal now, world; zero = unauthored
  float dt = 1.0f / 60.0f; // seconds the sweep covers (tip speed comes from it)
  float halfWidth = 0;     // carve radius, world voxels
  // WHAT THIS BLOW IS MADE OF (game/impact.h StrikeProfile): a CUT part, a
  // BLUNT part and a BITE part, each at full swing speed and each scaled by
  // the same `power` the bare `damage` float this replaced always was.
  //
  // A PROFILE RATHER THAN A NUMBER, because the sweep is the one place all
  // three can be resolved against the same struck thing: a mace must break
  // the plate AND bruise the arm under it in one pass, and a bite must be
  // refused its infection by the armour it did not get through. Splitting
  // that into three sweeps would mean three probe walks, three parry tests
  // and three chances for them to disagree about what was hit.
  //
  // A sword fills only `cut` (plus a token `blunt`), so a caller that ports
  // `damage` straight onto `strike.cut` gets byte-identical behaviour.
  StrikeProfile strike;
  float carveBonus = 0;    // extra carve radius beyond the blade's own
  // HOW MUCH WEAPON IS BEHIND THE EDGE, dimensionless (item.h
  // ItemDef::HeftFactor: the item's own voxel volume against
  // `gore.woundHeftRef`). Scales the kerf's depth and length, so a greatsword
  // cuts through what a knife has to saw at. 1 is the neutral value a
  // fabricated sweep gets for free, and it is deliberately the DEFAULT: a gate
  // that only wants to measure the geometry should not have to own an ItemDef.
  //
  // The FACTOR rather than the volume, because the conversion needs the gore
  // tuning and this header is included by item.h's consumers; the callers each
  // do the one-line `item->HeftFactor(g.woundHeftRef, g.woundHeftMax)`.
  float heft = 1.0f;
  // A WEAK SEGMENT'S SCALE on the blow (ItemDef::haftPower): multiplies the
  // sweep's one `power`, so everything it drives -- the hp, the dent, the
  // flinch, the knock -- comes out that much softer. 1 for the real edge.
  // EdgeSweepResult::power is reported UNSCALED (the speed ramp), so a caller
  // choosing a cue's loudness applies its own gain for the softer hit.
  float powerScale = 1.0f;
  // Sim tick, for the wound's counter-based seed. The ragged rim and the blood
  // soak must replay identically from the same tick+probe, and nothing in the
  // kerf may key on a Jolt float (game/mob.h BladeCut::seed).
  uint32_t tick = 0;
  bool valid = false;
  // ---- WHAT THIS STROKE HAS ALREADY STRUCK (2026-09-15) -----------------
  //
  // A CUT IS A KERF AND A BLUNT BLOW IS AN IMPULSE, and the difference only
  // shows up once a stroke lasts more than one tick. The kerf is CONTINUOUS:
  // the blade is still in the wound on the next tick, `CutLimb` snaps to the
  // entry plane it already opened, and carrying the same cut deeper is what a
  // sword does. Trauma is not like that. You are hit by a mace ONCE per swing,
  // and there is no sense in which the second tick of the same follow-through
  // is a second blow.
  //
  // Leaving it per-tick multiplied every blunt and bite number by the length
  // of the cut phase AND by the number of probes that met the same slot.
  // Measured through `--shot-strike` on `human+mace horizontal_r
  // human+iron_cuirass`: 14 body hits over 7 cut ticks, all SIX cuirass shells
  // taken from 40 hp to 0 and 1,664 voxels each, where package A's own
  // `impact-armor` gate -- which fabricates ONE sweep per blow -- measured 7
  // shell voxels for six direct `BluntHit` calls. The resolver was right and
  // the number of times it ran was wrong.
  //
  // So: a CALLER-OWNED set of the rig slots this stroke has already delivered
  // its impulse to. Blunt and bite land on FIRST CONTACT and are then silent
  // for the rest of the stroke; the cut is untouched and still runs every tick.
  //
  // BY BODY HANDLE, not by creature: a swing that crosses an arm and then the
  // chest legitimately bruises both, and a mace that meets three plates of the
  // same cuirass legitimately dents three plates. What it may not do is dent
  // the same plate four times because the phase was four ticks long.
  //
  // NULL MEANS "NO STROKE IDENTITY", which is exactly what a fabricated gate
  // sweep wants: `impact-blunt` hits a bare arm N times on purpose and each of
  // those N is its own blow. Only a live stroke owns one of these
  // (StrokeCursor::struck), and it is cleared when the stroke resets.
  std::vector<uint64_t>* struck = nullptr;
  // ---- ...AND A MOUTH CLOSES ONCE, ON ONE THING (2026-09-17) -------------
  //
  // `struck` above is BY BODY, and for the blunt part that is exactly right: a
  // swing that crosses an arm and then the chest legitimately bruises both. A
  // BITE is not that shape. Teeth are one pair of jaws arriving at one place,
  // and the sweep that carries them tiles the whole travel of the stroke with
  // rays -- so a lunging zombie whose head passed a shoulder, a chest and an
  // arm on its way in tore THREE holes, each with its own rot, out of one
  // snap. Owner report: a single lunge leaving a mark on the head, the torso
  // and an arm.
  //
  // A SECOND LATCH RATHER THAN A RULE ON THE FIRST, because the two really do
  // disagree: "once per slot" and "once, full stop" are different sentences
  // and a bite needs the second. Set only when `BiteHit` reports the teeth
  // found something to close on, so a probe that met a body whose limb lookup
  // failed does not spend the stroke's one bite on nothing.
  //
  // ...AND IT ALSO CLOSES A HOLE IN `struck` THAT ONLY A CARVING IMPULSE CAN
  // FALL THROUGH, which is the half the A/B actually measured. `struck` is a
  // set of BODY HANDLES, and a bite CARVES: `CarveLimb` ends in
  // `Mob::RebuildLimbBody`, whose own comment says "The handle CHANGES" --
  // Jolt gets a new body built round the new lattice. So the slot the teeth
  // just tore is no longer in the set under any name, and the next cut tick's
  // probe meets it as a stranger and bites it again. The blunt part never sees
  // this because jaws and fists author `bluntCarve` 0 and therefore never
  // rebuild anything. Measured on `--shot-strike zombie bite_lunge human`,
  // same seed, same stand-off, latch the only difference: ONE upper arm lost
  // 6 voxels, 8.2 hp and 22.7 of bleed budget to a single lunge, against 2
  // voxels, 4.9 hp and 4.5 with the latch in -- three bites reported as one.
  //
  // NOTE for whoever gets to the mace: the same hole is open for
  // `gore.bluntCarveRadius` > 0, because `CarveLimbRadial` reaches the same
  // rebuild. A blunt weapon with a dent will re-dent the same plate every cut
  // tick of its stroke, and `struck` cannot see it happening. The real fix is
  // to key that set on (mob, limb slot) rather than on a handle the impulse
  // itself invalidates; it is not done here because debris bodies in the same
  // set have no limb slot to key on.
  //
  // NULL MEANS "NO STROKE IDENTITY", exactly as `struck` does, and for the
  // same reason: `bite-rot` fabricates ten separate sweeps on purpose and each
  // of them is its own bite.
  bool* bitten = nullptr;
  // ---- ...AND EVERY STRIKE AFTER THE FIRST IS WORTH LESS (2026-09-28) -----
  //
  // One stroke meets the same creature many times: the kerf runs on every cut
  // tick, and a swing that crosses an arm carries on into the chest behind
  // it. Each of those was full hp, so the damage of a blow was set by how long
  // the blade stayed in the body rather than by where it landed. Now strike n
  // of this stroke on one creature does `MeleeTuning::repeatHitScale^(n-1)` of
  // its hp -- 100%, 50%, 25% at the default -- so the FIRST contact, the one
  // the blow was aimed and arrived with, is always the heaviest.
  //
  // (creature id, strikes so far). BY CREATURE, not by body like `struck`:
  // the falloff is about one person being hit repeatedly by one swing, whichever
  // limbs it crosses. A STRIKE is one body resolved in one sweep, so the kerf
  // still deepens every tick (the wound geometry is untouched) -- only the hp
  // it charges falls. Loose matter is not counted: it has no hp.
  //
  // NULL MEANS "NO STROKE IDENTITY", exactly as `struck` does: a gate that
  // fabricates N sweeps gets N full blows. The head and the haft of one stroke
  // share ONE of these -- it is the same swing hitting the same person.
  std::vector<std::pair<uint64_t, int>>* victimHits = nullptr;
  // Push a StrikeEvent the first time this stroke meets each creature (needs
  // `victimHits` to know "first"). Set by the NPC stroke runner; the player's
  // sweep latches its own cues and leaves it off.
  bool reportStrikes = false;
  // The level those events carry (ItemDef::haftGainDb for the haft sweep).
  float cueGainDb = 0.0f;
  // ---- ...AND THEY CLOSE ON THE THING THE STROKE AIMED AT (2026-09-19) ----
  //
  // THE DRAW CHOSE A LIMB AND THE JAWS BIT WHATEVER THEY MET FIRST. Those are
  // two different decisions and until now only the second one mattered:
  // `MobSystem::StartStroke` draws the style's `target` tag (zombie bites:
  // arm 0.40, head 0.20, spine 0.18, leg 0.14, hand 0.08) and 2026-09-19 made
  // the stroke AIM at the drawn limb — but the mouth is a capsule 2.4 world
  // voxels wide swung at a body whose torso is directly behind its arms, so the
  // probe walk met the chest on the way in and the stroke's one bite was spent
  // there. Owner report: "19 in 20 zombie bites end up on the head or chest".
  //
  // `bitePrefer` is the drawn limb's BODY HANDLE, and it means: of everything
  // these jaws touch, this is the one they are trying to close on. Zero = no
  // preference, which is what a fabricated gate sweep and the player's own fist
  // get (a player aims with the mouse and has no draw).
  //
  // A HANDLE RATHER THAN A SLOT, because that is the currency the probe walk
  // already speaks: `hitBodies`, `struck` and the armour retarget are all body
  // handles, and resolving a (mob, slot) pair per probe would mean a rig lookup
  // inside the hot loop for a comparison that is one integer compare.
  uint64_t bitePrefer = 0;
  // ...AND A PREFERENCE THAT CANNOT BE SATISFIED MUST NOT COST THE BITE. A limb
  // the jaws never reach would otherwise mean a zombie that chews air all fight
  // — the exact failure the aim fix was supposed to end. So the preference is a
  // HOLDOUT with a deadline: while this is true the bite waits for `bitePrefer`
  // and lands on nothing else, and on the LAST tick of the cut phase the caller
  // clears it and first contact wins as it always did. One stroke, one bite,
  // aimed where it was drawn when that is possible and landed regardless.
  //
  // Set by the stroke runner, which is the only thing that knows how many cut
  // ticks are left (strokes.h StrokeCursor::phaseTick / cutTicks).
  bool biteHoldout = false;
  // ---- IS THE EDGE PART OF THE WIELDER? (2026-09-15) --------------------
  //
  // A HELD BLADE IS A THING YOU POINT; A FIST IS A THING YOU THROW, and the
  // probe geometry is different in a way no tolerance can paper over.
  //
  // The probes cast ALONG THE EDGE'S OWN AXIS, which for a sword is exactly
  // right: the blade is long, the wrist lays it along the stroke, and tiling
  // rays down its length is what makes "the pose is the hitbox" true. A
  // natural weapon has neither property. Its edge is a voxel of knuckle or an
  // inch of jaw, it is deliberately NOT wrist-steered (there is nothing to lay
  // along a line), and the axis it does have points back down the arm that is
  // swinging it -- so every ray began inside the wielder's own hand and ran up
  // its own forearm. Measured through `--shot-strike`, with the knuckles'
  // hitbox OVERLAPPING the victim: `12 rays cast: 0 found air, 12 never left
  // the wielder, 0 found a body`. No fist or set of jaws in the game had ever
  // hit anything.
  //
  // So a self-mounted edge probes along the direction it is TRAVELLING, which
  // is what a fist actually damages: whatever it runs into. The flag is set by
  // the callers that know (Mob::StepStroke and main.cpp's player sweep, off
  // `EffectorWeapon`), never guessed from the geometry -- a short sword is not
  // a fist and the engine should not have to decide by length.
  bool selfMounted = false;
};

// What one tick's sweep actually did. Reported rather than printed so the gate
// and the HUD can both read it, and so a "did it hit" question never has to be
// answered by re-deriving the geometry a second time somewhere else.
struct EdgeSweepResult {
  int bodiesHit = 0;
  float tipSpeed = 0;      // world voxels/sec
  float power = 0;         // 0..1 speed ramp, before edge alignment
  float edgeAlign = 1;     // MeleeEdgeAlign's answer, 0..1
  // THE STROKE WAS STOPPED. One flag rather than a list, because a sweep can
  // only be arrested once: the first blade in the path kills the remainder of
  // this tick's probes, so a stroke cannot parry off one weapon and carry on
  // through a second. The CALLER is what ends the stroke early
  // (MeleeState::Arrest for the player, NpcStroke's cut phase for an NPC) —
  // this function has no business reaching into either.
  bool arrested = false;
  // ---- WHERE THE PROBE RAYS WENT (CLAUDE.md rule 6) --------------------
  // `bodiesHit 0` has four causes and from outside they are one number: the
  // sweep never ran, no ray was cast, every ray found empty air, or every ray
  // was eaten by the wielder's own body. These four words separate them, and
  // they are what `--shot-strike` prints. Cost: four increments per probe.
  // ---- WHERE THE BLOW LANDED (2026-09-19) ------------------------------
  // The FIRST contact point of the first body this sweep found, in world
  // voxels: the probe ray's own hit position, the exact place the kerf is bored
  // and the flinch impulse applied. Reported because the impact SOUND has to be
  // made there and callers had nothing better to use — main.cpp was placing the
  // flesh/clang/strike/cut cues at the midpoint of the blade segment, which on a
  // long weapon is half a metre from the wound and reads, correctly, as the
  // sound coming from the wrong place. `hasHitAt` is false when no body was
  // touched (a hit into terrain or thin air), and then `hitAt` is untouched.
  Vec3 hitAt{};
  bool hasHitAt = false;
  int probesCast = 0;      // rays actually fired
  int probesAir = 0;       // ...that found nothing at all
  int probesSelf = 0;      // ...that were still inside the wielder at the end
  int probesBody = 0;      // ...that found somebody else
  // ...that found flesh with a worn shell between it and the blade's travel,
  // and were redirected onto the shell ("armour defends from cuts").
  int probesCovered = 0;
  // ---- WAS ANY OF IT DEAD FLESH? (2026-09-20) ------------------------------
  //
  // main.cpp picks the cue for a landed blow by DIFFERENCING the sever and
  // voice queues across the call: severs grew -> a limb came off; voices grew
  // -> a live creature was hurt; neither -> a chip. A corpse fills neither
  // queue, so hacking a body apart on the ground got the cue a CRATE gets.
  // This is the missing third answer, and it is a fact only the sweep has:
  // the body it met said `DebrisSystem::BodyIsDeadFlesh`.
  bool hitDeadFlesh = false;
  BlockEvent block{};
};

// Forward declarations only: this header is included BY mob.h, so it may not
// include it back. Incomplete types are fine through references.
class Physics;
class MobSystem;
class Mob;
class DebrisSystem;
class World;
struct ParticleSpawn;

// THE POSE IS THE HITBOX (melee.h note 1), and this is the function that says
// so. Sweeps the blade's authored edge segment from where it was last tick to
// where it is now, carving live flesh and melting debris along the way.
//
// A FUNCTION, not a block in the frame loop, because there are now three
// callers and they must agree exactly: the player's tick, an attacking NPC's
// tick (Phase C), and the gate that measures whether an edge-on cut really does
// more than a flat-on one. A gate that re-implemented the damage curve would be
// measuring its own copy.
//
// `wielder` is excluded from its own sweep — a blade starts inside its owner's
// fist and would otherwise saw through the arm holding it.
//
// AND SOMEBODY ELSE'S FIST IS NOT. A probe that meets another creature's HELD
// ITEM is a PARRY, not a cut: the stroke is arrested, the remainder of this
// tick's probes are abandoned, the blocking weapon takes item hp, and a
// BlockEvent comes back on the result. Worn armour is deliberately NOT a parry
// — a shell is strapped to the limb it covers and stopping a blow with it is
// what armour is for, so it keeps taking the cut it always did (the wound model
// already carves shells and the occlusion probe already accounts for them).
// The three-way classification is the rig's own: a slot below `AppendedBase()`
// is FLESH, one at or above it tagged `worn` is a GARMENT, and the one at
// `HeldSlot()` is a WEAPON.
EdgeSweepResult MeleeSweepDamage(const EdgeSweep& sweep, const MeleeTuning& t,
                                 const Mob& wielder, Physics& phys,
                                 MobSystem& mobs, DebrisSystem& debris,
                                 World& world,
                                 std::vector<ParticleSpawn>& spawns);


// The whole pose the driver commands, in one value. Passed to Mob::
// SetWeaponPose; see the note there for what each channel does to the rig.
// ---- PER-JOINT SMOOTHING OF THE WEAPON ARM (AttackStyle::joints) ----------
//
// The driver commands a HAND and a POLE; the shoulder, elbow and wrist
// rotations are whatever the two-bone solve and the wrist steer make of them,
// and on some strokes that is a shoulder that whips round its own axis between
// the windup and the cut while the blade itself travels fine. These are the
// author's brakes on the RESULT, one per joint, applied after the solve by
// Mob::SmoothWeaponArm (and rig.js smoothWeaponArm for the tuner's preview):
//
//   smooth   low-pass half-life in TICKS on the joint's rotation relative to
//            its parent. 0 = off. 2 = the joint covers half the remaining
//            distance to the solved pose every two ticks.
//   maxDeg   cap on how far the joint may turn in ONE tick, degrees. 0 = off.
//            This is the "limit": a shoulder capped at 20 deg/tick cannot
//            spin 120 degrees across the windup->cut boundary.
//
// PRESENTATION ONLY, and the damage stays honest: the sweep reads the POSED
// blade (Mob::WeaponEdge reads the live limb bodies), so a lagging arm hits
// where it is drawn, not where the driver asked. Everything defaults to 0 =
// the solve untouched, which is every style authored before this.
struct ArmJointSmooth {
  float smooth = 0;   // half-life, ticks
  float maxDeg = 0;   // degrees per tick
  bool Any() const { return smooth > 0.0f || maxDeg > 0.0f; }
};
enum ArmJointIndex { kArmShoulder = 0, kArmElbow = 1, kArmWrist = 2, kArmJoints = 3 };
struct ArmSmooth {
  ArmJointSmooth joint[kArmJoints];
  bool Any() const {
    return joint[0].Any() || joint[1].Any() || joint[2].Any();
  }
};

struct WeaponPose {
  Vec3 hand{};              // hand offset from the shoulder, world voxels
  Vec3 bladeDir{0, 1, 0};   // along the blade, hilt -> point, world
  Vec3 bladeFlat{0, 0, 1};  // normal of the blade's cutting plane, world
  Vec3 bendPole{0, 0, -1};  // where the ELBOW should bulge, world
  float weight = 0;         // 0..1 claim on the arm
  // How far the wrist may take the blade away from the orientation the solved
  // forearm gives it for free, radians. Carried ON THE POSE rather than read
  // from a tuning struct in the rig code because Mob has no MeleeState and must
  // not grow one: the driver owns the feel, the rig owns the anatomy, and this
  // is the one number that crosses. (Default mirrors MeleeTuning's; see the
  // long note there for why it is what it is, and why it used to be 2.80.)
  float wristMaxAngle = 3.10f;
  // HOW MUCH OF THAT ALIGNMENT TO ACTUALLY APPLY, 0..1. Same crossing, same
  // reason: the DRIVER knows whether this is a cut or a hold (it owns the tip
  // speed and the phase) and the RIG knows how to apply a wrist, and neither
  // may reach into the other. At 0 the hand keeps the orientation the solved
  // forearm gives it and the blade sits at its authored grip angle — a natural
  // ready. At 1 the blade is laid along `bladeDir` up to `wristMaxAngle`.
  //
  // 1 is the DEFAULT because the legacy four-argument SetWeaponPose and every
  // gate that fabricates a pose mean "apply what I asked for"; only
  // MeleeState::Pose() ramps it. See MeleeTuning::steerSpeedLo.
  float steerAmount = 1.0f;
  // How far the elbow's steered hinge axis may sit from the forearm's AUTHORED
  // one, radians. The rig takes the tighter of this and the shoulder's own
  // authored twist range. See MeleeTuning::elbowAxisCone.
  float elbowAxisCone = 1.31f;
  // THE TORSO'S SHARE OF THE STROKE, radians: twist toward the wielder's
  // right, chest pitch up. Same seam-crossing rule as wristMaxAngle above —
  // the DRIVER knows the stroke (Pose() fills these from the smoothed az/el,
  // scaled by tuning.torsoShare/torsoPitch AND by PoseWeight, so they fade
  // with the arm claim), the RIG knows how to twist a spine
  // (AnimApplySpineTwist, the head-look's distribution law). 0/0 — the
  // default every fabricated gate pose gets — is exactly the old behaviour.
  float torsoTwist = 0;
  float torsoPitch = 0;
  // False is the legacy contract: drive the arm at `hand` and leave the blade
  // at whatever grip angle the fist gives it. True is the stroke driver: the
  // hand is ORIENTED so the blade points along bladeDir with its flat facing
  // bladeFlat, and the elbow's bend plane follows bendPole.
  bool steerBlade = false;
  // Honour `bendPole` WITHOUT steering the blade (the alchemy bench's carried
  // flask). The rig's own arm pole is straight back, which is exactly
  // antiparallel to a hand held straight out in front at shoulder height —
  // the two-bone solve's bend plane is noise there and the elbow flips from
  // tick to tick. A caller whose hand goes there hands in a pole that is
  // well-conditioned over its whole range instead.
  bool usePole = false;
  // The live style's per-joint brakes (ArmSmooth above). MeleeState::Pose()
  // never fills it — the driver has no style — so the stroke callers
  // (MobSystem::StepStroke, the player's discrete strike) copy it in.
  ArmSmooth smooth;
  // THE RELEASE IS A JOINT-SPACE MOVE (2026-09-25). -1 while the stroke owns
  // the arm; 0..1 (already eased) while it is handed back. The rig then does
  // NOT solve the arm at all: each joint -- shoulder, elbow, wrist -- turns,
  // relative to its parent, from where it was on the last driven tick to
  // where the animation holds it, by exactly this much (Mob::SmoothWeaponArm).
  // It used to fade the IK's hand target into the animation's and re-solve
  // every tick, with the torso unwinding under it: a blend of two solvers,
  // not a motion, and its first ticks visibly jerked on every style.
  float release = -1.0f;
  // A KEYED ARM (strokes.h "A FRAME IS A POSE", 2026-09-25). When `on`, the
  // rig solves NOTHING: Mob::SmoothWeaponArm writes each joint as the slerp
  // from `from` to `to` by `t` (already eased), the shoulder first turned
  // about itself by that end's aim (yaw about the body's up, then pitch about
  // its right). `fromLive` = blend from the arm as it stood when the stroke
  // took it (the first frame) instead of `from`. The torso rides
  // torsoTwist/torsoPitch above, which the stroke fills from the frames.
  struct Keyed {
    bool on = false;
    bool fromLive = false;
    Quat from[kArmJoints]{};
    Quat to[kArmJoints]{};
    bool hasJoint[kArmJoints] = {false, false, false};  // the wrist may be unkeyed
    float t = 0;
    float aimFromYaw = 0, aimFromPitch = 0;
    float aimToYaw = 0, aimToPitch = 0;
  } keyed;
};

// The player's melee state. One instance, owned by main.cpp beside the caster.
class MeleeState {
 public:
  // Feed a control delta every FRAME, before Update. Kept separate from Update
  // because the tick loop runs 0..4 times per frame: input is sampled per frame
  // and integrating it per tick would multiply-count it. The player forwards
  // raw mouse pixels; an NPC forwards one authored StrokeSample per tick.
  void Feed(float dx, float dy);
  // The radial channel — a thrust or a draw-back. Separate on purpose; see the
  // header note on why neither angular axis may be spent on reach.
  void FeedReach(float dr);
  // One scripted tick: feed the sample, then advance. The whole NPC-facing
  // surface, and what the swing gates drive.
  void Step(const StrokeSample& s, float dt, bool armed, const Vec3& right,
            const Vec3& up, const Vec3& fwd);

  // WHERE THE BLADE ACTUALLY IS, pushed in every tick before Update (Mob::
  // WeaponStrokePose). Three jobs, and the feature does not work without any of
  // them:
  //
  //   * `handFromShoulder` and `tipFromShoulder` (world voxels, the frame
  //     HandOffset/TipOffset return) are the SEED. On the tick the blade comes
  //     up, control starts at exactly that point, so the arm keeps the pose the
  //     walk cycle left it in and the player pushes it from there. No guard
  //     pose, no snap. The pair also gives the driver the BLADE LENGTH, which
  //     is how it converts a commanded tip back into a hand.
  //   * `reach` is the arm's own bone length, so the clamp on how far the
  //     stroke may push the hand is the rig's fact rather than a constant in
  //     this file — it follows a longer arm, and it follows kVoxelMeters.
  //
  // SetArm is the no-blade form (tip == hand): the arm is still steered, the
  // driver simply has no point to lead with. Call ClearArm when there is no arm
  // to read at all (no rig, no weapon, severed); the tuning fallbacks are used
  // instead and the pose merely starts at a guess rather than at the truth.
  //   * `flat` is the blade's CURRENT flat normal (world), so the roll the
  //     take-over starts from is the roll the blade is really at. Pass a zero
  //     vector when the rig cannot say; the driver then picks a perpendicular
  //     and the only cost is that the first tick's roll is arbitrary.
  void SetStroke(const Vec3& handFromShoulder, const Vec3& tipFromShoulder,
                 const Vec3& flat, float reach);
  void SetArm(const Vec3& handFromShoulder, float reach) {
    SetStroke(handFromShoulder, handFromShoulder, Vec3{}, reach);
  }
  void ClearArm();
  // THE WIELDER'S OWN HEAD, as a keep-out sphere: centre relative to the
  // shoulder in the same world frame SetStroke's offsets are in, radius in
  // world voxels (the head's own half-size; tuning.headClear rides on top).
  // Pushed per tick beside SetStroke by both drivers (Mob::HeadKeepOut);
  // Clear when the rig cannot say (no head, severed), which turns the clamp
  // off rather than clamping against a stale sphere.
  void SetKeepOut(const Vec3& centerFromShoulder, float radius) {
    keepC_ = centerFromShoulder;
    keepR_ = radius;
  }
  void ClearKeepOut() { keepR_ = 0; }
  // THE WIELDER'S OWN TORSO, as a keep-out CAPSULE: the spine's two ends
  // relative to the shoulder in the same world frame SetStroke's offsets are
  // in, and the body's half-WIDTH in world voxels. Pushed per tick beside
  // SetStroke by both drivers (Mob::BodyKeepOut); Clear when the rig cannot
  // say, which turns the clamp off rather than clamping against a stale one.
  void SetBodyKeepOut(const Vec3& aFromShoulder, const Vec3& bFromShoulder,
                      float halfWide, float halfDeep) {
    bodyA_ = aFromShoulder;
    bodyB_ = bFromShoulder;
    bodyWide_ = halfWide;
    bodyDeep_ = halfDeep;
  }
  void ClearBodyKeepOut() { bodyWide_ = bodyDeep_ = 0; }
  // WAS THE HAND PUSHED OUT OF THE BODY THIS TICK, and by how far (world
  // voxels)? Reported rather than merely applied, because "the arm still goes
  // through the chest" and "the clamp is off" are different failures and from
  // outside they are the same picture. Zero when the clamp did not fire.
  float BodyClampPush() const { return bodyPush_; }
  // +1 = the weapon is on the basis's RIGHT (a right-handed wielder), -1 = its
  // left. Only the asymmetric azimuth limits read it: "across the body" is a
  // different stop from "out to the weapon side".
  void SetHandSign(float s) { handSign_ = s < 0 ? -1.0f : 1.0f; }

  // Advance the state machine. `held` is the attack button, `armed` is whether
  // a melee weapon is actually equipped (an unarmed player never leaves Idle).
  // `right`/`up`/`fwd` are the basis the stroke is expressed in — the camera
  // for a player, the body's own facing for an NPC — so a cut is always
  // described relative to where the wielder is looking, and so is the
  // integrated stroke state, which is why turning the view carries the blade
  // around with you instead of leaving it pointing at a fixed piece of world.
  void Update(float dt, bool held, bool armed, const Vec3& right,
              const Vec3& up, const Vec3& fwd);

  // ---- outputs -------------------------------------------------------------
  // THE STROKE IS THE OUTPUT AND THE TIP IS THE STROKE. Everything below except
  // TipOffset is derived from it (melee.h header note): the hand is the tip
  // minus a blade, the blade frame is the radius leaning into the travel, and
  // the bend pole is the stroke's own tangent.
  Vec3 TipOffset() const { return tip_; }
  Vec3 HandOffset() const { return hand_; }
  // Along the blade, hilt to point. APPLIED now, through the hand's
  // orientation in the IK solve — never by rotating the held part after the
  // flatten, which is a bug this file has already shipped once.
  Vec3 BladeDir() const { return bladeDir_; }
  // Normal of the blade's cutting plane: the flat faces this way, so the EDGE
  // is perpendicular to it and to the blade. Derived from the tip's own
  // velocity, which is what makes the edge lead the travel.
  Vec3 BladeFlat() const { return bladeFlat_; }
  // Where the ELBOW should bulge, world. Handed to the two-bone solver as its
  // pole so the arm bends IN the plane of the cut instead of in the fixed
  // authored plane — a horizontal cut is then shoulder rotation plus elbow
  // extension in the horizontal plane.
  Vec3 BendPole() const { return bendPole_; }
  // The whole command in one value, ready for Mob::SetWeaponPose.
  WeaponPose Pose() const;

  // The raw stroke state, for gates and the HUD: azimuth and elevation of the
  // tip in the basis's frame (radians, azimuth 0 = straight ahead, positive to
  // the basis's right; elevation 0 = level) and its distance from the shoulder.
  // These are the pure integral of the control input — the property the swing
  // gate's "a displacement, not a rate" block is stated on.
  float StrokeAz() const { return az_; }
  float StrokeEl() const { return el_; }
  float StrokeRadius() const { return radius_; }
  // THE ANNULUS OF TIP RADII THIS ARM CAN ACTUALLY SERVE, world voxels, given
  // the blade it is holding and how extended it is being held.
  //
  // PUBLIC because a scripted stroke has to author its reach IN it. An NPC
  // thrust authored as a fraction of the ARM's length looked correct and did
  // nothing: measured on the human rig the band is only [3.8, 6.5] voxels wide
  // (the point is a blade's length off a hand that is itself most of an arm
  // out, so the two lengths nearly cancel), and every style's chamber and lunge
  // landed outside it and was clamped. The commanded radius then moved 0.15
  // voxels on a stroke asking for four, and the only symptom was a thrust that
  // read as a twitch.
  //
  // Zero-width or degenerate bands are already handled inside; the caller only
  // has to clamp into what it gets back.
  void ReachBand(float& lo, float& hi) const {
    float hand = 0;
    RadiusBand(lo, hi, hand);
  }

  // How much of the arm this claims, 0..1. Not a bool, because the tick the
  // claim ENDS is a pop otherwise: the hand is wherever the player left it and
  // the walk cycle wants it somewhere else, so the solve fades out over the
  // recover instead of being switched off. Coming IN needs no fade — control
  // starts at the arm's own current pose, so weight 1 changes nothing visible.
  float PoseWeight() const;
  // ---- ONE STROKE'S OWN HAND-BACK CLOCK (2026-09-21) ---------------------
  //
  // `tuning.recoverTime` is ONE NUMBER SHARED BY EVERY ATTACK IN THE GAME, and
  // for a held-button stroke that is right: it is the feel of releasing the
  // mouse, and there is one of those. For an AUTHORED stroke it is not — a jab
  // and an overhead chop have no business handing the arm back on the same
  // clock, and until this existed they had no choice.
  //
  // Seconds; 0 restores the global value, which is what every caller that
  // never touches it gets. Pushed by StepStrokeProgram from the style's
  // `recover.fade` (strokes.cpp says why it is pushed every tick), and cleared
  // by Reset() so a fresh stroke never inherits one. It deliberately does NOT
  // live on `tuning`: that struct is copied wholesale from tuning.json on every
  // F5 and per-swing state written into it would be silently reverted.
  void SetRecoverTime(float seconds) {
    recoverOverride_ = seconds > 1e-4f ? seconds : 0.0f;
  }
  // THIS STEP IS AN AUTHORED PROGRAM'S (2026-09-25). StepStrokeProgram sets it
  // before every Step and Update clears it after, so it is true for exactly
  // the ticks a program drives. While it is, the wrist aligns the blade in
  // FULL: the program states where the TIP goes, the hand is placed on the
  // assumption that the blade points there, and the speed-earned alignment
  // (steerSpeedLo/Hi, steerFloor — the freeform mode's "a slow raise rides
  // the grip") left a slow cut's blade at 15% alignment, its tip nowhere near
  // the authored point. More ticks made that WORSE, which is backwards.
  //
  // `holdLean`: THE RETURN DOES NOT RE-LEAN (2026-09-25). The lean plane
  // (perpL_) chases the tip's travel so the hand leads the edge through a cut.
  // A settle travels back the other way, so the chase swung the plane half a
  // turn about the blade — OVER THE TOP — and carried the hand up past the
  // wielder's head while the tip barely moved ("the hand teleports above the
  // head at the start of the settle"). A return is not a cut; there is no edge
  // to lead. The program passes true for its Recover ticks and the plane holds
  // the lean the cut left.
  //
  // FRAMES (2026-09-25, strokes.h "AN ATTACK IS A LIST OF FRAMES"): the whole
  // per-frame shaping arrives here. `lean` 0 follow / 1 hold / 2 left / 3
  // right (strokes.h FrameLean, as an int so this header does not need that
  // one); `wristAlign` replaces the full alignment above with the frame's own
  // 0..1; the torso shares < 0 mean the global tuning. The torso shares are
  // LATCHED (Pose() is read after Update) and cleared by Reset().
  struct ProgramDrive {
    int lean = 0;
    float wristAlign = 1.0f;
    float torsoTwist = -1.0f, torsoPitch = -1.0f;
    // BLADE ANGLE, radians: the blade's angle off the shoulder-to-tip line
    // (0 = the blade continues the arm straight). The hand is placed from it:
    // tip minus a blade at this angle. < 0 = AUTO, the old rule — the hand
    // held at melee.handExtend of the arm and the angle solved from that.
    float bladeAngle = -1.0f;
    // ELBOW DIRECTION, radians about the shoulder-to-hand line: 0 = the elbow
    // points down, +pi/2 = out to the weapon side, -pi/2 = in across the body,
    // pi = up. With the hand placed, this is the ONE remaining freedom of the
    // arm, so it fixes the shoulder's rotation and the elbow's bend plane
    // together. `elbowSet` false = AUTO, the old rule (the elbow trails the
    // hand's travel, held inside melee.elbowPoleCone).
    bool elbowSet = false;
    float elbowSwivel = 0.0f;
    // LEAN ANGLE (lean == 4): which way the HAND sits off the shoulder-to-tip
    // line, radians about that line — 0 = below it, + toward the weapon side,
    // the elbow's convention. Stated outright; nothing chases the travel.
    float leanAngle = 0.0f;
  };
  float LeanAngleNow() const { return leanAngleNow_; }
  // What the arm is doing NOW, for the program to blend a frame's blade angle
  // and elbow direction FROM (so a change of either between frames is a
  // motion, not a snap).
  float BladeAngleNow() const { return bladeAngleNow_; }
  // How far the hand-back is, 0..1 and eased, or -1 when not handing back
  // (WeaponPose::release).
  float ReleaseProgress() const;
  float ElbowSwivelNow() const { return elbowSwivelNow_; }
  void SetProgramDrive(const ProgramDrive& d) {
    programDrive_ = true;
    drive_ = d;
    torsoTwistShare_ = d.torsoTwist;
    torsoPitchShare_ = d.torsoPitch;
  }
  float RecoverTime() const {
    return recoverOverride_ > 1e-4f ? recoverOverride_ : tuning.recoverTime;
  }
  // HOW COMMITTED THIS STROKE IS, 0..1, and therefore how much of the blade's
  // commanded orientation the wrist should actually apply (WeaponPose::
  // steerAmount). Smoothed inside RebuildFrame on the blade's own halflife, so
  // reading it is free and the value never steps. Exposed for the gate that
  // measures the guard pose and for the HUD.
  float SteerAmount() const { return steerLive_; }

  SwingPhase Phase() const { return phase_; }
  float MouseSpeed() const { return mouseSpeed_; }

  void Reset();

  // ---- what a parry does to the two strokes involved ------------------------
  // END THIS STROKE NOW. The cut met something solid, so the follow-through it
  // had left does not happen: the driver drops into Recover with whatever arc
  // it had already spent, and the arm is handed back over the usual ramp rather
  // than being snapped anywhere. Idempotent — a stroke already recovering or
  // idle is left alone, so a caller may call it on every blocked tick.
  void Arrest();
  // ...AND SHOVE THE OTHER ONE. A bounded nudge to the stored stroke state, so
  // a guard that stops a heavy blow is beaten open a little. It moves az_/el_
  // and NOT a pose, which is what keeps it continuous: the point is displaced
  // on its own reach sphere and the arm follows it there like any other input.
  // The caller supplies the magnitudes AND the sign (hash-seeded); this only
  // applies and re-clamps them.
  void Nudge(float dAz, float dEl);

  MeleeTuning tuning;

 private:
  // The stroke's own frame, rebuilt each tick from the basis handed to Update.
  void RebuildFrame(float dt, const Vec3& right, const Vec3& up,
                    const Vec3& fwd);
  // The blade angle, lean angle and elbow swivel of the frame AS SEEDED by a
  // take-over, in RebuildFrame's conventions. The take-over skips its rebuild,
  // so without this the "now" angles a program frame blends FROM were the
  // previous swing's, and the first driven tick snapped the blade to them.
  void MeasureShapeNow(const Vec3& tipL, const Vec3& handL);
  // THE ANNULUS OF TIP RADII THE ARM CAN ACTUALLY SERVE, given the blade it is
  // holding and how extended `handExtend` says to hold it. Exactly the reach
  // annulus a two-bone solver clamps to, one link further out: hand-to-point is
  // a rigid bone too. Both the integrator and the seed clamp `radius_` with it,
  // which is what keeps the derived hand inside the arm without the CLAMP being
  // what does the keeping.
  void RadiusBand(float& lo, float& hi, float& handRadius) const;

  SwingPhase phase_ = SwingPhase::Idle;
  float phaseTime_ = 0;
  // Accumulated control motion this frame, and its smoothed velocity.
  Vec3 inputAccum_{};     // x = dx, y = dy, z = dReach
  Vec3 mouseVel_{};       // smoothed units/sec
  float mouseSpeed_ = 0;
  Vec3 tip_{}, hand_{}, bladeDir_{0, 1, 0}, bladeFlat_{0, 0, 1},
      bendPole_{0, 0, -1};
  // The wrist's eased blade frame in world space (see wristDirL_ below);
  // Pose() hands THESE to the rig, never the exact pair above.
  Vec3 wristDir_{0, 1, 0}, wristFlat_{0, 0, 1};
  // THE INTEGRATED STROKE, in the basis's frame: azimuth about `up` measured
  // from `fwd` toward `right`, elevation above the fwd/right plane, and the
  // tip's distance from the shoulder. Stored as angles rather than as a point
  // so that yawing the view carries the blade with it, and so that a sideways
  // drag is an ARC in front of the character rather than a slide across a
  // plane — the difference between a sweep and a jab.
  float az_ = 0, el_ = 0, radius_ = 0;
  // THE SMOOTHED STROKE the frame is actually built from — az/el/radius eased
  // toward the clamped stroke on the armSmoothing
  // halflife. Kept beside az_/el_/radius_ rather than replacing them because
  // the RAW integral is what the clamps bank against and what a take-over
  // seeds; the eased copy is presentation. At armSmoothing 0 these equal the
  // sums every tick and nothing changes.
  float azLive_ = 0, elLive_ = 0, radLive_ = 0;
  // Smoothed tip velocity in the basis frame (voxels/sec), and the tangent
  // derived from it. The blade frame and the bend pole are both built off this,
  // which is why they are smoothed and the tip is not.
  Vec3 tipPrev_{};
  Vec3 tipVel_{};
  Vec3 tangent_{};
  // The HAND's own travel, which is what the bend pole is built from — a plane
  // for the arm to bend in has to be stated about the arm.
  Vec3 handPrev_{}, handVel_{};
  // The blade frame and the bend pole, SMOOTHED, in the basis frame. Kept here
  // rather than only in world space for the same reason az_/el_ are: the basis
  // turns with the view every tick, and a world-space memory would read that
  // turn as blade motion and roll the edge into it.
  Vec3 bladeDirL_{0, 0, 1}, bladeFlatL_{0, 1, 0}, poleL_{0, 0, -1};
  // WHAT THE WRIST IS ASKED TO CHASE — the blade frame again, but eased on
  // the wristSmoothing halflife. bladeDirL_ must stay exact (the hand is
  // derived from it and |tip - hand| = bladeLen has to hold every tick), so
  // the wrist's own lag lives in this separate copy: the ARM is placed off
  // the exact frame, the POSE reports this one. That split is the whole
  // "unique smoothing per joint" mechanism.
  Vec3 wristDirL_{0, 0, 1}, wristFlatL_{0, 1, 0};
  // THE LEAN PLANE, and how extended the arm is being held. These two are the
  // smoothed state, and the blade direction is REBUILT from them every tick
  // rather than being smoothed itself — see RebuildFrame. Smoothing a direction
  // is what a first version did, and it is wrong for a reason that only shows
  // up on a REVERSAL: the two ends of that interpolation lean opposite ways, so
  // the path between them passes through the radius, where the hand is
  // |r - bladeLen| from the shoulder. With a sword longer than the arm that is
  // the shoulder itself, and the whole chain folded into the chest for three
  // ticks every time the player changed direction.
  Vec3 perpL_{0, 1, 0};       // unit, perpendicular to the radius
  float extendLive_ = 0;      // the hand's distance from the shoulder, eased
  // HOW MUCH WRIST ALIGNMENT IS BEING APPLIED, smoothed. A THIRD smoothed
  // quantity beside those two and for the same reason: the target is a step
  // function of the phase (a cut commits on one tick) and a step in the amount
  // of applied alignment is a step in the blade's orientation. Seeded to the
  // floor rather than to 1 so a take-over on a still mouse starts at the pose
  // the blade is really in.
  float steerLive_ = 0;
  bool framePrimed_ = false;
  // The live arm, from SetStroke. `armReach_` is a bone length, not a tuning;
  // `bladeLen_` is the rig's own hand-to-point distance.
  Vec3 armHand_{}, armTip_{}, armFlat_{};
  float armReach_ = 0, bladeLen_ = 0;

 public:
  // The rig's own hand-to-point distance, as the driver last received it. The
  // lean geometry is built from it, so a diagnostic that cannot see it cannot
  // tell a bladeless weapon from a mis-seeded one.
  float BladeLength() const { return bladeLen_; }

 private:
  bool armValid_ = false;
  // The head keep-out sphere (SetKeepOut). World-frame offset from the
  // shoulder; 0 radius = no sphere = clamp off.
  Vec3 keepC_{};
  float keepR_ = 0;
  // The torso keep-out capsule (SetBodyKeepOut). World-frame offsets from the
  // shoulder; 0 radius = no capsule = clamp off.
  Vec3 bodyA_{}, bodyB_{};
  // Half-width (across) and half-depth (front-to-back), world voxels. Either
  // at 0 = no capsule = clamp off.
  float bodyWide_ = 0, bodyDeep_ = 0;
  float bodyPush_ = 0;   // how far the hand was pushed out of it, last tick
  float handSign_ = 1.0f;
  // Was the button still down when this recover started? A recover between two
  // cuts keeps the arm; a recover after the release hands it back (PoseWeight).
  bool recoverHold_ = false;
  // Seconds, 0 = use tuning.recoverTime. See SetRecoverTime.
  float recoverOverride_ = 0.0f;
  // See SetProgramDrive. One-shot: cleared at the end of every Update.
  bool programDrive_ = false;
  ProgramDrive drive_{};    // see SetProgramDrive; one-shot like programDrive_
  // Latched torso shares from the last program step; < 0 = tuning's.
  float torsoTwistShare_ = -1.0f, torsoPitchShare_ = -1.0f;
  float bladeAngleNow_ = 0.0f, elbowSwivelNow_ = 0.0f, leanAngleNow_ = 0.0f;
};
