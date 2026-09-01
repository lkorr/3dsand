#pragma once
#include "sim/scale.h"  // MetresToCells / MetresPerSecToCells
#include <cstdint>
#include <vector>

#include "game/item.h"
#include "math3d.h"

// MOUSE-DIRECTED MELEE (the "half sword" experiment).
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

// The swing state machine. Small enough to reason about; the interesting
// behaviour is in how Wind reads the mouse, not in the graph.
enum class SwingPhase : uint8_t {
  Idle = 0,   // weapon at rest, following the walk cycle
  Guard,      // button held: the mouse owns the hand, moving it 1:1
  Wind,       // mouse moving fast, building the cut direction (still owning it)
  Slash,      // committed: the blade drives along the chosen direction, cutting
  Recover,    // the cut's follow-through unwinds; the arm is handed back
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
  // Mouse pixels per second past which a guarded blade commits to a cut. Low
  // enough that an intentional flick always fires, high enough that aiming
  // while guarding does not.
  float commitSpeed = 900.0f;
  // Seconds the committed slash takes. Short: a cut is a snap, not a wind-up.
  float slashTime = 0.17f;
  float recoverTime = 0.22f;
  // Blade tip speed (world voxels/sec) at and above which a hit does full
  // damage; below it damage falls off linearly to zero. This is what makes a
  // committed cut different from waving the weapon around.
  float fullSpeed = MetresPerSecToCells(3.4f);
  float minSpeed = MetresPerSecToCells(0.9f);
  // HOW FAR THE TIP SWINGS PER CONTROL UNIT — RADIANS per mouse pixel, not
  // pixels/sec and not voxels. This is the whole control law now: the delta is
  // a displacement of the point on its reach sphere, integrated, so 200 px of
  // mouse is the same arc whether you took a tenth of a second or two seconds
  // over it. `commitSpeed` below is still measured on true pixel SPEED, so
  // raising these makes the blade cover more ground without making a twitch
  // fire a cut.
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
  float azOut = 2.36f;      // 135 deg to the weapon side
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
  // How far the committed cut carries the point on its own, in radians of arc
  // about the shoulder, ADDED to whatever the player is still steering. ~115
  // degrees is a cut; a canned stroke longer than that outruns the mouse and
  // stops reading as the player's own motion.
  float swingArc = 2.0f;
  // ANTICIPATION, as a fraction of the arc's own bow: how far the point pulls
  // BACK against the cut before driving through it. The previous law expressed
  // the same idea as a symmetric arc centred on the hand — which jumped half a
  // swing backwards on the tick it committed, a 57-degree pop in tip space. An
  // anticipation term is zero at both ends of the stroke by construction, so it
  // buys the wind-up without buying the discontinuity.
  float swingAnticipate = 0.35f;
  // Fraction of the tip reach the arc bows OUT by at mid-stroke. An arm swings
  // about a shoulder and is furthest from the body halfway through; that bulge
  // is also what carries the blade THROUGH a target rather than past it. This
  // is the radial channel doing the job the old law did with a forward offset.
  float swingExtend = 0.16f;
  // Seconds of halflife on the blade frame and on the arm's bend plane. This
  // is what makes a take-over continuous — control starts at the blade's ACTUAL
  // orientation and eases to the commanded one — and what stops the flat of the
  // blade from snapping through 90 degrees when the tip changes direction.
  float bladeSmoothing = 0.055f;
  // How far the wrist may take the blade away from the orientation the solved
  // arm would give it for free, radians.
  //
  // IT USED TO BE 2.80 — 160 degrees, which is a ball joint and not a wrist —
  // and the reason was structural rather than a taste for loose wrists. The
  // authored grip held the blade PERPENDICULAR to the forearm (sword.json's
  // `[0, -90, 0]`, and gen_sword_item.py said so in as many words), which was
  // right back when the sword was never re-aimed. A cut wants the blade roughly
  // along the arm's own line, so about 90 degrees of the budget was spent
  // undoing the authored grip before any steering happened at all — measured,
  // the stroke asked for 2.0 to 3.0 radians and an 85-degree cap clipped every
  // tick of it, which showed up as the blade pointing up to 100 degrees away
  // from the cut.
  //
  // THE GRIP IS NOW RE-AUTHORED, which is what makes this number honest.
  // assets/items/{sword,cleaver}.json rotate `[0, 0, -90]`: the blade is built
  // along the item's +x, the arm extends along the rig's -y, and a -90 about Z
  // carries +x onto -y, so a NEUTRAL wrist already holds the blade down the
  // arm's own line. Verified rather than reasoned, which is the rule for every
  // rotation in this repo:
  //
  //   python scripts/geometry.py rotate_point 0 0 1 -90 -- 1 0 0
  //   -> [0, -1, 0]                    (blade +x lands on -y, along the arm)
  //
  // It does NOT bound steering only, and assuming it did was the phase C/D
  // merge's one real interaction bug — the full note, with the three measured
  // budgets, is on Tuning::Melee::wristMaxAngle in sim/tuning.h. Short form:
  // the blade asks for direction AND ROLL, the neutral grip stands about pi of
  // roll away from a committed cut, and what the steering gets is this number
  // MINUS that. 3.10 leaves ~1.1 rad free, which is the wrist; 1.50 left none
  // and clamped half of every real cut. Lives in tuning.json as
  // `melee.wristMaxAngle`, so the number is a JSON edit rather than a rebuild.
  float wristMaxAngle = 3.10f;
  // ---- the edge leads the cut ---------------------------------------------
  // ---- BLOCKING IS EMERGENT (Phase C) --------------------------------------
  //
  // There is NO block button and no block state. A blade physically in the path
  // of a stroke stops it, because MeleeSweepDamage's probes hit the defender's
  // WEAPON before they hit the defender — the held item is a real rig slot with
  // a real collider (DESIGN.md 8c), so it was always in the way and the sweep
  // was simply carving straight through it. A parry is that hit, classified.
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
  // 0.14 m -> 0.22 m at the phase C/D merge, and the cause is geometric rather
  // than a retune: the re-authored grip lays the blade ALONG the arm, so the
  // same guard pose leaves the defender's edge further from its own fist and a
  // crossing parry closes on it with more slack. Measured on `npc-block`, the
  // two swept edges pass within 2.42 voxels where the old grip gave 0.86.
  float blockGap = MetresToCells(0.22f);
  // Fraction of the blow's own damage the BLOCKING WEAPON takes as item hp.
  // Items have hp and are severable (item.h ItemDef::hp/severable), so this
  // needs no new mechanism — a sword that blocks all day eventually breaks, and
  // one that blocks something far too heavy is knocked out of the hand by the
  // existing severImpactSpeed rule. 1.0 would make a parry cost the defender's
  // weapon exactly what it would have cost their arm, which is too much: an arm
  // absorbs a cut by being cut, a blade absorbs it by being a blade.
  float blockItemDamage = 0.35f;
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

// One resolved cut of the blade this tick: the quad the edge swept, how fast it
// was going, and which way its flat was facing while it did. MeleeSweepDamage
// turns this into carves.
struct EdgeSweep {
  Vec3 aPrev{}, bPrev{};   // edge base/tip last tick, world voxels
  Vec3 aNow{}, bNow{};     // edge base/tip now
  Vec3 flatNow{};          // blade's flat normal now, world; zero = unauthored
  float dt = 1.0f / 60.0f; // seconds the sweep covers (tip speed comes from it)
  float halfWidth = 0;     // carve radius, world voxels
  float damage = 0;        // the item's damage at full swing speed
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
  // Sim tick, for the wound's counter-based seed. The ragged rim and the blood
  // soak must replay identically from the same tick+probe, and nothing in the
  // kerf may key on a Jolt float (game/mob.h BladeCut::seed).
  uint32_t tick = 0;
  bool valid = false;
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
  float wristMaxAngle = 1.50f;
  // False is the legacy contract: drive the arm at `hand` and leave the blade
  // at whatever grip angle the fist gives it. True is the stroke driver: the
  // hand is ORIENTED so the blade points along bladeDir with its flat facing
  // bladeFlat, and the elbow's bend plane follows bendPole.
  bool steerBlade = false;
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
  // The committed cut direction in WORLD space, fixed at commit time so the
  // cut does not curve when the player keeps moving the mouse mid-slash.
  Vec3 cutDir_{};
  // ...and the same direction in CONTROL space (unit, x = azimuth, y =
  // elevation), which is what the follow-through arc is actually integrated in.
  // A world vector cannot be: the arc lives on the reach sphere.
  float cutAz_ = 0, cutEl_ = 0;
  Vec3 tip_{}, hand_{}, bladeDir_{0, 1, 0}, bladeFlat_{0, 0, 1},
      bendPole_{0, 0, -1};
  // THE INTEGRATED STROKE, in the basis's frame: azimuth about `up` measured
  // from `fwd` toward `right`, elevation above the fwd/right plane, and the
  // tip's distance from the shoulder. Stored as angles rather than as a point
  // so that yawing the view carries the blade with it, and so that a sideways
  // drag is an ARC in front of the character rather than a slide across a
  // plane — the difference between a sweep and a jab.
  float az_ = 0, el_ = 0, radius_ = 0;
  // The cut's own follow-through, ADDED to az_/el_ rather than replacing them:
  // the player keeps steering through the slash and the arc rides on top of
  // wherever they have steered to. Decays over Recover, which is what makes a
  // cut end where the mouse ended rather than snapping back to a pose.
  float swingAz_ = 0, swingEl_ = 0, swingOut_ = 0;
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
  bool framePrimed_ = false;
  // The live arm, from SetStroke. `armReach_` is a bone length, not a tuning;
  // `bladeLen_` is the rig's own hand-to-point distance.
  Vec3 armHand_{}, armTip_{}, armFlat_{};
  float armReach_ = 0, bladeLen_ = 0;
  bool armValid_ = false;
  float handSign_ = 1.0f;
  // Was the button still down when this recover started? A recover between two
  // cuts keeps the arm; a recover after the release hands it back (PoseWeight).
  bool recoverHold_ = false;
};
