/* ============================================================================
   melee.js — line-by-line JS transcription of the STROKE DRIVER:
              src/game/melee.cpp (MeleeState) and src/game/strokes.cpp
              (the shared stroke-program runner).

   THIS FILE IS NOT AN IMPLEMENTATION. It is a PORT, on exactly the terms
   anim.js states for the animation runtime: every function below is a
   transcription of the engine's own, with the C++ source line cited above it.
   The Attacks tab must show what the engine will do, so the only correct way
   to change anything here is to change melee.cpp / strokes.cpp first and then
   follow it.

   WHY IT HAS TO BE THE REAL DRIVER AND NOT A CURVE PLOT. An attack style
   authors four numbers per segment (az / el / reach / ticks) and NONE of them
   is where the blade goes. The style steers a closed loop under `commitSpeed`;
   the reach is a BAND POSITION resolved against the arm the rig actually has;
   the hand is the tip minus a blade, solved by a law of cosines; and three
   separate clamps (the azimuth window, the hand-back plane, the head keep-out)
   can each move the result. A preview that drew the authored angles directly
   would agree with the engine only on the styles that happen not to hit any of
   that, which is none of them.

   Coverage (see CITATIONS at the bottom):
     TRANSCRIBED  MeleeTuning + the tuning.json map, RadiusBand, RebuildFrame
                  (tip, blade lean, hand, hand-back plane, head keep-out,
                  wrist frame, elbow pole + cone, steer envelope), Update
                  (take-over seed, phase machine, stroke integration),
                  Pose/PoseWeight, StrokeReachIn,
                  BeginStrokeProgram, StepStrokeProgram, QuantizeStrike,
                  NeutralStrike, PickAttackStyle, rng::Hash3/SignedUnit.
     APPROXIMATED nothing in the driver. The CALLER (rig.js) approximates:
                  heading is 0 (the tuner previews a rig in its own frame),
                  the aim is the panel's own az/el rather than a live target,
                  and damage/parry (MeleeSweepDamage, FindParry) are absent
                  because they need a World.

   UNITS. The driver speaks WORLD VOXELS, because every tuning constant it
   reads is authored in metres and converted by kVoxelMeters at load. The rig
   editor's model space is FILE voxels — `skinScale` of them per world voxel.
   The caller converts at the seam (rig.js, weaponStep) rather than this file
   carrying a scale, so the constants here stay byte-identical to melee.h's.
   ========================================================================== */

/* THE ONE IMPORT, and it mirrors an include rather than breaking the rule
   below. `strokes.cpp` now includes `game/anim.h` for Ease/ApplyEase, because
   a cut's pacing IS a clip keyframe's easing and the engine keeps one
   implementation of it; anim.js is that function's line-cited port, so calling
   it here is what keeps the JS side one implementation too. Redeclaring it
   would be the duplication the shared vocabulary exists to avoid. Contrast the
   vectors below, which are primitives and are redeclared on purpose. */
import * as AN from './anim.js';

/* ============================================================================
   Vectors. Same shapes anim.js uses, so the two ports interoperate without
   conversion; re-declared rather than imported so this file can be diffed
   against melee.cpp on its own.
   ========================================================================== */

export const v3 = (x = 0, y = 0, z = 0) => ({ x, y, z });
const vadd = (a, b) => ({ x: a.x + b.x, y: a.y + b.y, z: a.z + b.z });
const vsub = (a, b) => ({ x: a.x - b.x, y: a.y - b.y, z: a.z - b.z });
const vmul = (a, s) => ({ x: a.x * s, y: a.y * s, z: a.z * s });
const vdot = (a, b) => a.x * b.x + a.y * b.y + a.z * b.z;
const vlen = a => Math.sqrt(vdot(a, a));
const vcross = (a, b) => ({
  x: a.y * b.z - a.z * b.y,
  y: a.z * b.x - a.x * b.z,
  z: a.x * b.y - a.y * b.x,
});
function vnorm(a) {
  const l = vlen(a);
  return l < 1e-9 ? v3() : { x: a.x / l, y: a.y / l, z: a.z / l };
}
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

// melee.cpp:488 SmoothAlpha — the halflife form. The naive `a += (b-a)*k`
// makes every constant in melee.h mean a different thing at 30 fps than at
// 144, and the editor's preview runs at neither.
export function smoothAlpha(halflife, dt) {
  if (halflife <= 1e-5) return 1.0;
  return 1.0 - Math.pow(2, -dt / halflife);
}

// melee.cpp:493 Lerp
const lerp3 = (a, b, t) => vadd(a, vmul(vsub(b, a), t));

// melee.cpp:499/502 ToWorld / ToBasis — the ONE place the basis frame and the
// world frame meet. Everything the driver remembers is in the BASIS frame
// (x = right, y = up, z = fwd).
const toWorld = (l, right, up, fwd) =>
  vadd(vadd(vmul(right, l.x), vmul(up, l.y)), vmul(fwd, l.z));
const toBasis = (w, right, up, fwd) =>
  v3(vdot(w, right), vdot(w, up), vdot(w, fwd));

// melee.cpp:508 AnyPerp
function anyPerp(d) {
  const alt = Math.abs(d.y) < 0.9 ? v3(0, 1, 0) : v3(1, 0, 0);
  const p = vsub(alt, vmul(d, vdot(d, alt)));
  return vlen(p) > 1e-5 ? vnorm(p) : v3(1, 0, 0);
}

/* ============================================================================
   rng — src/sim/rng.h. The stroke's jitter is counter-based so ten swings vary
   and the fight replays (CLAUDE.md rule 1); the editor draws the SAME numbers
   for a given seed, which is what makes "preview swing #3" mean anything.

   JS bitwise ops are signed 32-bit and `*` is float, so every step goes
   through Math.imul + >>> 0 to reproduce the u32 arithmetic exactly.
   ========================================================================== */

// rng.h:22 Pcg
export function pcg(v) {
  const s = (Math.imul(v >>> 0, 747796405) + 2891336453) >>> 0;
  const w = Math.imul(((s >>> ((s >>> 28) + 4)) ^ s) >>> 0, 277803737) >>> 0;
  return ((w >>> 22) ^ w) >>> 0;
}

// rng.h:28 Hash3
export const hash3 = (a, b, c) => pcg((a ^ pcg((b ^ pcg(c)) >>> 0)) >>> 0);

// rng.h:34 SignedUnit — 16 bits, uniform in [-1, 1). Coarse by design.
export const signedUnit = h => ((h & 0xffff) | 0) / 32768.0 - 1.0;

/* ============================================================================
   MeleeTuning — melee.h:160-380, and the tuning.json map at melee.cpp:760.

   THE UNIT SPLIT IS THE ENGINE'S. melee.h stores VOXELS (the frame the stroke
   integrates in); tuning.json stores METRES, because a JSON file that stored
   voxels would silently change meaning the next time kVoxelMeters moved. The
   key names carry the unit (`fullSpeedMps`, `guardUpM`) and this map is where
   the conversion happens, exactly as in ApplyMeleeTuning.
   ========================================================================== */

// src/sim/world.h:29. Quoted, not derived — if it moves, so does every length
// below, and the tuner must follow the header rather than guess.
export const kVoxelMeters = 0.10;
const metresToCells = m => m / kVoxelMeters;
const metresPerSecToCells = m => m / kVoxelMeters;

// melee.h's own defaults, in the same order ApplyMeleeTuning copies them.
// Used when tuning.json has no `melee` block (a file:// page, or a stripped
// asset dir) so the preview still runs rather than dividing by undefined.
export function defaultMeleeTuning() {
  return {
    commitSpeed: 900.0,                                    // melee.h:161
    recoverTime: 0.22,                                     // :164
    fullSpeed: metresPerSecToCells(3.4),                   // :168
    minSpeed: metresPerSecToCells(0.9),                    // :169
    aimGainX: 0.0050,                                      // :189
    aimGainY: 0.0067,                                      // :190
    reachGain: metresToCells(0.0030),                      // :195
    azOut: 1.83,                                           // :211
    azAcross: 1.40,                                        // :212
    elMin: -1.50,                                          // :217
    elMax: 1.48,                                           // :218
    handExtend: 0.78,                                      // :234
    extendSmoothing: 0.18,                                 // :238
    leanTurnRate: 18.0,                                    // :243
    // melee.h: the three lean/roll guards the port was missing until
    // 2026-09-25 (test_melee's "unmapped" line had been naming them).
    leanFlipHold: 3.14,
    leanMinSpeed: metresPerSecToCells(0.05),
    flatMinSin: 0.20,
    handLead: 1.0,                                         // :249
    fallbackReach: metresToCells(0.60),                    // :253
    reachFraction: 0.94,                                   // :260
    guardForward: metresToCells(0.22),                     // :264
    guardUp: metresToCells(0.26),
    guardSide: metresToCells(0.16),
    dirSmoothing: 0.06,                                    // :268
    bladeSmoothing: 0.055,                                 // :291
    wristMaxAngle: 3.10,                                   // :324
    steerSpeedLo: metresPerSecToCells(0.6),                // :355
    steerSpeedHi: metresPerSecToCells(3.0),                // :356
    steerFloor: 0.15,                                      // :361
    armSmoothing: 0.04,                                    // :373
    wristSmoothing: 0.10,                                  // :381
    handBackFrac: 0.05,                                    // :394
    elbowPoleCone: 1.75,                                   // :418
    elbowAxisCone: 3.14,                                   // :419
    edgeFloor: 0.35,
    sweepSpacing: metresToCells(0.05),                     // melee.cpp MeleeSweepDamage
    sweepMaxSteps: 24,
    blockGap: metresToCells(0.22),
    blockItemDamage: 0.35,
    blockNudgeAz: 0.30,
    blockNudgeEl: 0.18,
    torsoShare: 0.35,
    torsoPitch: 0.20,
    headClear: metresToCells(0.06),
  };
}

/**
 * melee.cpp:760 ApplyMeleeTuning — tuning.json's `melee` block -> MeleeTuning.
 *
 * `pickMinSpeed`/`aimYaw` are DELIBERATELY not
 * copied, exactly as in the engine: they are the controller's switches and the
 * swing-basis cone, read at the one site in main.cpp,
 * and a cached copy here could disagree across an F5. The Attacks panel reads
 * them straight off the tuning document instead.
 */
export function meleeTuningFrom(tuningJson) {
  const d = defaultMeleeTuning();
  const m = tuningJson && tuningJson.melee;
  if (!m || typeof m !== 'object') return d;
  const n = (k, dflt) => (Number.isFinite(+m[k]) ? +m[k] : dflt);
  return {
    ...d,
    commitSpeed: n('commitSpeed', d.commitSpeed),
    recoverTime: n('recoverTime', d.recoverTime),
    fullSpeed: metresPerSecToCells(n('fullSpeedMps', d.fullSpeed * kVoxelMeters)),
    minSpeed: metresPerSecToCells(n('minSpeedMps', d.minSpeed * kVoxelMeters)),
    aimGainX: n('aimGainX', d.aimGainX),
    aimGainY: n('aimGainY', d.aimGainY),
    reachGain: metresToCells(n('reachGainM', d.reachGain * kVoxelMeters)),
    azOut: n('azOut', d.azOut),
    azAcross: n('azAcross', d.azAcross),
    elMin: n('elMin', d.elMin),
    elMax: n('elMax', d.elMax),
    handExtend: n('handExtend', d.handExtend),
    extendSmoothing: n('extendSmoothing', d.extendSmoothing),
    leanTurnRate: n('leanTurnRate', d.leanTurnRate),
    leanFlipHold: n('leanFlipHold', d.leanFlipHold),
    leanMinSpeed: metresPerSecToCells(n('leanMinSpeed', d.leanMinSpeed * kVoxelMeters)),
    flatMinSin: n('flatMinSin', d.flatMinSin),
    handLead: n('handLead', d.handLead),
    fallbackReach: metresToCells(n('fallbackReachM', d.fallbackReach * kVoxelMeters)),
    reachFraction: n('reachFraction', d.reachFraction),
    guardForward: metresToCells(n('guardForwardM', d.guardForward * kVoxelMeters)),
    guardUp: metresToCells(n('guardUpM', d.guardUp * kVoxelMeters)),
    guardSide: metresToCells(n('guardSideM', d.guardSide * kVoxelMeters)),
    dirSmoothing: n('dirSmoothing', d.dirSmoothing),
    bladeSmoothing: n('bladeSmoothing', d.bladeSmoothing),
    wristMaxAngle: n('wristMaxAngle', d.wristMaxAngle),
    steerSpeedLo: metresPerSecToCells(n('steerSpeedLoMps', d.steerSpeedLo * kVoxelMeters)),
    steerSpeedHi: metresPerSecToCells(n('steerSpeedHiMps', d.steerSpeedHi * kVoxelMeters)),
    steerFloor: n('steerFloor', d.steerFloor),
    armSmoothing: n('armSmoothing', d.armSmoothing),
    wristSmoothing: n('wristSmoothing', d.wristSmoothing),
    handBackFrac: n('handBackFrac', d.handBackFrac),
    elbowPoleCone: n('elbowPoleCone', d.elbowPoleCone),
    elbowAxisCone: n('elbowAxisCone', d.elbowAxisCone),
    edgeFloor: n('edgeFloor', d.edgeFloor),
    sweepSpacing: metresToCells(n('sweepSpacingM', d.sweepSpacing * kVoxelMeters)),
    sweepMaxSteps: n('sweepMaxSteps', d.sweepMaxSteps),
    blockGap: metresToCells(n('blockGapM', d.blockGap * kVoxelMeters)),
    blockItemDamage: n('blockItemDamage', d.blockItemDamage),
    blockNudgeAz: n('blockNudgeAz', d.blockNudgeAz),
    blockNudgeEl: n('blockNudgeEl', d.blockNudgeEl),
    torsoShare: n('torsoShare', d.torsoShare),
    torsoPitch: n('torsoPitch', d.torsoPitch),
    headClear: metresToCells(n('headClearM', d.headClear * kVoxelMeters)),
  };
}

/* ============================================================================
   MeleeState — melee.h:134 / melee.cpp:829-1684.

   Field names keep the engine's trailing underscore so a diff against the C++
   is a line-for-line read. `L` suffixes are BASIS-frame copies, as in the
   header.
   ========================================================================== */

// melee.h SwingPhase. NO SLASH (2026-09-25): the driver's own speed-triggered
// cut and its follow-through arc were a second controller fighting every
// authored stroke. The PROGRAM's Cut phase is the cut.
export const PHASE = { Idle: 0, Guard: 1, Wind: 2, Recover: 3 };
export const PHASE_NAME = ['idle', 'guard', 'wind', 'recover'];

export class MeleeState {
  constructor(tuning) {
    this.tuning = tuning || defaultMeleeTuning();
    // ---- the stroke the input integrates (melee.h) ----
    this.phase_ = PHASE.Idle;
    this.phaseTime_ = 0;
    this.inputAccum_ = v3();
    this.mouseVel_ = v3();
    this.mouseSpeed_ = 0;
    this.az_ = 0; this.el_ = 0; this.radius_ = 0;
    this.azLive_ = 0; this.elLive_ = 0; this.radLive_ = 0;
    // ---- the derived frame ----
    this.tipL_ = v3(); this.tipPrev_ = v3(); this.tipVel_ = v3();
    this.tangent_ = v3();
    this.bladeDirL_ = v3(0, 0, 1); this.bladeFlatL_ = v3(0, 1, 0);
    this.wristDirL_ = v3(0, 0, 1); this.wristFlatL_ = v3(0, 1, 0);
    this.perpL_ = v3(0, 1, 0);
    this.poleL_ = v3(0, 0, -1);
    this.handL_ = v3(); this.handPrev_ = v3(); this.handVel_ = v3();
    this.extendLive_ = 0;
    this.steerLive_ = clamp(this.tuning.steerFloor, 0, 1);
    this.framePrimed_ = false;
    this.recoverHold_ = false;
    // melee.h SetRecoverTime: seconds, 0 = use tuning.recoverTime. An
    // authored stroke may own its own hand-back clock (strokes.h
    // StrokeRecover::fade).
    this.recoverOverride_ = 0;
    // melee.h SetProgramDrive: true for exactly the steps a stroke program
    // makes, with that frame's shaping in `drive_`. Cleared at the end of
    // update(); the torso shares are LATCHED (pose() is read after update).
    this.programDrive_ = false;
    this.drive_ = { lean: 0, wristAlign: 1, torsoTwist: -1, torsoPitch: -1,
                    bladeAngle: -1, elbowSet: false, elbowSwivel: 0, leanAngle: 0 };
    this.leanAngleNow_ = 0;
    this.torsoTwistShare_ = -1;
    this.torsoPitchShare_ = -1;
    // melee.h BladeAngleNow / ElbowSwivelNow — what the arm is doing now.
    this.bladeAngleNow_ = 0;
    this.elbowSwivelNow_ = 0;
    // ---- world-frame outputs (Pose reads these) ----
    this.tip_ = v3(); this.hand_ = v3();
    this.bladeDir_ = v3(); this.bladeFlat_ = v3();
    this.wristDir_ = v3(); this.wristFlat_ = v3();
    this.bendPole_ = v3(0, 0, -1);
    // ---- what the rig told us about the arm it has ----
    this.armHand_ = v3(); this.armTip_ = v3(); this.armFlat_ = v3();
    this.bladeLen_ = 0; this.armReach_ = 0; this.armValid_ = false;
    this.keepC_ = v3(); this.keepR_ = 0;
    this.handSign_ = 1.0;
  }

  // melee.h:768
  strokeAz() { return this.az_; }
  strokeEl() { return this.el_; }
  strokeRadius() { return this.radius_; }
  phaseName() { return PHASE_NAME[this.phase_] || '?'; }

  // melee.cpp:829 Feed / :834 FeedReach
  feed(dx, dy) { this.inputAccum_.x += dx; this.inputAccum_.y += dy; }
  feedReach(dr) { this.inputAccum_.z += dr; }

  // melee.cpp:836 Step
  step(s, dt, armed, right, up, fwd) {
    this.feed(s.dx || 0, s.dy || 0);
    this.feedReach(s.dReach || 0);
    this.update(dt, s.held !== false, armed, right, up, fwd);
  }

  // melee.cpp:843 SetStroke — THE BLADE LENGTH IS MEASURED, NEVER AUTHORED
  // HERE: it is the rigid hand-to-point distance of whatever is in the fist
  // this tick, so a dagger and a greatsword steer the same way.
  setStroke(handFromShoulder, tipFromShoulder, flat, reach) {
    this.armHand_ = handFromShoulder;
    this.armTip_ = tipFromShoulder;
    this.armFlat_ = flat || v3();
    this.bladeLen_ = vlen(vsub(tipFromShoulder, handFromShoulder));
    this.armReach_ = reach;
    this.armValid_ = reach > 1e-3;
  }

  // melee.cpp:857 ClearArm
  clearArm() {
    this.armValid_ = false; this.armReach_ = 0;
    this.bladeLen_ = 0; this.armFlat_ = v3();
  }

  // melee.h:720
  setKeepOut(centerFromShoulder, radius) {
    this.keepC_ = centerFromShoulder; this.keepR_ = radius;
  }
  clearKeepOut() { this.keepR_ = 0; }
  // melee.h:728 — ".L" is the character's LEFT in the name, and the name is
  // what the stroke's asymmetric azimuth limits want (mob.cpp:8123 HandSign).
  setHandSign(s) { this.handSign_ = s < 0 ? -1.0 : 1.0; }

  // melee.h SetRecoverTime / RecoverTime
  setRecoverTime(seconds) {
    this.recoverOverride_ = seconds > 1e-4 ? seconds : 0;
  }
  // melee.h SetProgramDrive — lean 0 follow / 1 hold / 2 left / 3 right.
  setProgramDrive(d) {
    this.programDrive_ = true;
    this.drive_ = { lean: 0, wristAlign: 1, torsoTwist: -1, torsoPitch: -1,
                    bladeAngle: -1, elbowSet: false, elbowSwivel: 0, leanAngle: 0,
                    ...(d || {}) };
    this.torsoTwistShare_ = this.drive_.torsoTwist;
    this.torsoPitchShare_ = this.drive_.torsoPitch;
  }
  recoverTime() {
    return this.recoverOverride_ > 1e-4
      ? this.recoverOverride_ : this.tuning.recoverTime;
  }

  // melee.cpp:864 PoseWeight
  poseWeight() {
    switch (this.phase_) {
      case PHASE.Idle: return 0.0;
      case PHASE.Recover: {
        // Only the RELEASING recover fades: a recover between two cuts is
        // still the player's arm.
        if (this.recoverHold_) return 1.0;
        // melee.cpp: EASED, on the same curve the arm's joints turn home on.
        return 1.0 - this.releaseProgress();
      }
      default: return 1.0;
    }
  }

  // melee.cpp ReleaseProgress -- 0..1 (smoothstep) while handing the arm back,
  // -1 otherwise (WeaponPose::release).
  releaseProgress() {
    if (this.phase_ !== PHASE.Recover || this.recoverHold_) return -1;
    const rt = this.recoverTime();
    const t = clamp(rt > 1e-4 ? this.phaseTime_ / rt : 1.0, 0, 1);
    return t * t * (3 - 2 * t);
  }

  // melee.cpp:883 Pose
  pose() {
    const w = this.poseWeight();
    const t = this.tuning;
    return {
      hand: this.hand_,
      // The WRIST'S eased frame, not the exact one (melee.cpp:886).
      bladeDir: this.wristDir_,
      bladeFlat: this.wristFlat_,
      bendPole: this.bendPole_,
      weight: w,
      // melee.h WeaponPose::release: the hand-back is a JOINT-SPACE move.
      release: this.releaseProgress(),
      wristMaxAngle: t.wristMaxAngle,
      steerAmount: this.steerLive_,
      elbowAxisCone: t.elbowAxisCone,
      steerBlade: true,
      // THE TORSO'S SHARE, off the SMOOTHED integrals and scaled by the same
      // weight that owns the arm. The caps are the anatomy, the tuning
      // fractions are the taste (melee.cpp:906).
      // A program frame may state its own shares (setProgramDrive).
      torsoTwist: clamp(this.azLive_ * (this.torsoTwistShare_ >= 0
        ? this.torsoTwistShare_ : t.torsoShare), -0.5, 0.5) * w,
      torsoPitch: clamp(this.elLive_ * (this.torsoPitchShare_ >= 0
        ? this.torsoPitchShare_ : t.torsoPitch), -0.22, 0.35) * w,
    };
  }

  // melee.cpp:921 Arrest
  arrest() {
    if (this.phase_ !== PHASE.Wind) return;
    this.phase_ = PHASE.Recover;
    this.phaseTime_ = 0;
    this.recoverHold_ = true;
  }

  // melee.cpp:949 Reset
  reset() {
    this.phase_ = PHASE.Idle;
    this.phaseTime_ = 0;
    this.inputAccum_ = v3(); this.mouseVel_ = v3(); this.mouseSpeed_ = 0;
    this.az_ = 0; this.el_ = 0; this.radius_ = 0;
    this.azLive_ = 0; this.elLive_ = 0; this.radLive_ = 0;
    this.tipPrev_ = v3(); this.tipVel_ = v3(); this.tangent_ = v3();
    this.extendLive_ = 0;
    this.steerLive_ = clamp(this.tuning.steerFloor, 0, 1);
    this.framePrimed_ = false;
    this.recoverHold_ = false;
    this.recoverOverride_ = 0;
    this.torsoTwistShare_ = -1;
    this.torsoPitchShare_ = -1;
  }

  /**
   * melee.cpp:974 RadiusBand — THE ANNULUS OF TIP RADII THIS ARM CAN SERVE,
   * world voxels, given the blade it holds and how extended it is being held.
   */
  radiusBand() {
    const t = this.tuning;
    const handReach =
      (this.armValid_ ? this.armReach_ : t.fallbackReach) * t.reachFraction;
    // THE LIVE extension, not the tuning target: a take-over starts the arm
    // wherever the animation had it.
    const handRadius = clamp(
      this.extendLive_ > 1e-4 ? this.extendLive_ : handReach * t.handExtend,
      0.05, handReach);
    const L = this.bladeLen_;
    if (L < 1e-4) {
      // No blade: the point IS the hand, so the band is simply the arm.
      return { lo: handReach * 0.15, hi: handReach, hand: handRadius };
    }
    const margin = Math.min(0.25, Math.max(L, handRadius) * 0.05);
    let lo = Math.abs(L - handRadius) + margin;
    let hi = L + handRadius - margin;
    if (hi <= lo) {                        // degenerate: the lengths coincide
      lo = Math.max(handRadius * 0.4, 0.05);
      hi = handRadius + L;
    }
    return { lo, hi, hand: handRadius };
  }

  /**
   * melee.cpp MeasureShapeNow — the blade angle, lean angle and elbow swivel
   * of the frame AS SEEDED, in the same conventions rebuildFrame reports
   * them. The take-over skips its rebuild, so without this the "now" angles
   * a program frame blends from are the previous swing's.
   */
  measureShapeNow_(tipL, handL) {
    const t = this.tuning;
    const downSide = (axis) => axisFrame(axis, this.handSign_);
    const rad = vlen(tipL) > 1e-5 ? vnorm(tipL) : v3(0, 0, 1);
    this.bladeAngleNow_ = Math.acos(clamp(vdot(this.bladeDirL_, rad), -1, 1));
    {
      const { down, side } = downSide(rad);
      const handSide = vmul(this.perpL_, t.handLead >= 0 ? 1 : -1);
      this.leanAngleNow_ = Math.atan2(vdot(handSide, side), vdot(handSide, down));
    }
    {
      const handDir = vlen(handL) > 1e-4 ? vnorm(handL) : v3(0, 0, 1);
      const { down, side } = downSide(handDir);
      this.elbowSwivelNow_ = Math.atan2(vdot(this.poleL_, side), vdot(this.poleL_, down));
    }
  }

  // melee.h:785 ReachBand
  reachBand() { const b = this.radiusBand(); return { lo: b.lo, hi: b.hi }; }

  /* ------------------------------------------------------------------------
     AN AUTHORING AFFORDANCE WITH NO ENGINE COUNTERPART, and the only one in
     this file. Everything else here is a line-cited port of melee.cpp; this
     is not, because the engine has no reason to want it — the game never
     teleports a blade to a pose, it drives it there.

     THE GOAL FRAME. Put the stroke exactly ON a stated (az, el, radius) with
     no interpolation of any kind — the raw integrals AND their eased copies
     are written together, so `armSmoothing` has nothing to lag and the pose
     on screen is the number that was authored rather than a frame on the way
     to it. That is the whole point: a windup or a cut leg is a DESTINATION,
     and reading it off a moving preview means reading it off whatever the
     smoothing had got to.

     Phase is forced to Guard so poseWeight is 1 and the rig actually takes
     the arm.

     A HELD FRAME IS A PURE FUNCTION OF ITS ARGUMENTS (2026-09-25). Only the
     TIP is az/el/radius; the hand, the blade's lean, its flat, the wrist and
     the elbow pole are EASED state that the live driver carries from tick to
     tick. Snapping just the tip left all of that wherever the previous
     motion had put it, so "recover", click off, "recover" again showed the
     same point with a different arm every time. So every piece of carried
     state is SEEDED canonically here — from `travel`, the direction the path
     moves through this destination (strokeGoalPose states it) — and the frame
     is then rebuilt until the eased channels sit on their fixed point: the
     arm a program would show if it arrived here moving that way and held.
     ---------------------------------------------------------------------- */
  snapToPose(az, el, radius, right, up, fwd, travel, drive) {
    const t = this.tuning;
    const { lo: rLo, hi: rHi } = this.radiusBand();
    const azHi = this.handSign_ > 0 ? t.azOut : t.azAcross;
    const azLo = this.handSign_ > 0 ? -t.azAcross : -t.azOut;
    this.phase_ = PHASE.Guard;
    this.phaseTime_ = 0;
    this.recoverHold_ = false;
    this.inputAccum_ = v3();
    this.mouseVel_ = v3();
    this.mouseSpeed_ = 0;
    this.az_ = clamp(az, azLo, azHi);
    this.el_ = clamp(el, t.elMin, t.elMax);
    this.radius_ = clamp(radius, rLo, rHi);
    this.azLive_ = this.az_;
    this.elLive_ = this.el_;
    this.radLive_ = this.radius_;

    // ---- THE CANONICAL SEED ------------------------------------------------
    // The tangent of the path at the tip, in BASIS coordinates: d(tip)/d(az)
    // and d(tip)/d(el) weighted by the travel. No travel (a style with a
    // zero leg) falls back to the horizontal the take-over seed prefers.
    const ce = Math.cos(this.el_), se = Math.sin(this.el_);
    const sa = Math.sin(this.az_), ca = Math.cos(this.az_);
    const radial = v3(ce * sa, se, ce * ca);
    const dAz = travel ? travel.az : 0, dEl = travel ? travel.el : 0;
    let tan = vadd(vmul(v3(ca, 0, -sa), dAz * ce),
                   vmul(v3(-se * sa, ce, -se * ca), dEl));
    if (vlen(tan) < 1e-5) tan = vcross(radial, v3(0, 1, 0));
    if (vlen(tan) < 1e-5) tan = anyPerp(radial);
    tan = vnorm(tan);
    this.tangent_ = tan;
    this.perpL_ = tan;
    this.tipVel_ = v3();
    this.handVel_ = v3();
    {
      // extendLive_'s own fixed point, so its slow ease has nothing to chase.
      const handReachNow =
        (this.armValid_ ? this.armReach_ : t.fallbackReach) * t.reachFraction;
      this.extendLive_ = clamp(handReachNow * t.handExtend, 0.05, handReachNow);
    }
    this.bladeFlatL_ = anyPerp(radial);
    {
      const f = vcross(radial, tan);
      if (vlen(f) > 1e-5) this.bladeFlatL_ = vnorm(f);
    }
    this.wristDirL_ = radial;
    this.wristFlatL_ = this.bladeFlatL_;
    this.poleL_ = v3(0, 0, -1);
    this.steerLive_ = 1.0;

    // ---- SETTLE THE EASED CHANNELS ----------------------------------------
    // Unprimed first (alpha 1 where the rebuild honours it), then primed with
    // the tip standing still — which is exactly "arrived and held". Every
    // channel here converges geometrically; two seconds of ticks is far past
    // the slowest halflife (extendSmoothing) and far below anything visible.
    // THE FRAME'S OWN SHAPING (setProgramDrive) is in force while it settles,
    // so a LEFT/RIGHT lean turns the plane to its side exactly as the live
    // frame does.
    const d = { lean: 0, wristAlign: 1, torsoTwist: -1, torsoPitch: -1, ...(drive || {}) };
    this.framePrimed_ = false;
    this.programDrive_ = true; this.drive_ = d;
    this.rebuildFrame(1 / 30, right, up, fwd);
    for (let i = 0; i < 60; i++) {
      this.tangent_ = tan;          // a still tip reads no tangent of its own
      this.programDrive_ = true; this.drive_ = d;
      this.rebuildFrame(1 / 30, right, up, fwd);
    }
    this.programDrive_ = false;
    // THE FRAME'S WRIST ALIGNMENT AND TORSO SHARES, as its live step has them.
    this.steerLive_ = clamp(d.wristAlign, 0, 1);
    this.torsoTwistShare_ = d.torsoTwist;
    this.torsoPitchShare_ = d.torsoPitch;
  }

  /* ------------------------------------------------------------------------
     melee.cpp:1003 RebuildFrame

     Given the integrated stroke, produce the tip, the blade frame, the bend
     pole and finally the hand. Everything is in BASIS coordinates until the
     last seven lines; nothing here reads the input.
     ---------------------------------------------------------------------- */
  rebuildFrame(dt, right, up, fwd) {
    const t = this.tuning;
    let { lo: rLo, hi: rHi } = this.radiusBand();

    // THE STROKE, clamped (the band can move under a stored radius).
    const azHi = this.handSign_ > 0 ? t.azOut : t.azAcross;
    const azLo = this.handSign_ > 0 ? -t.azAcross : -t.azOut;
    const azSum = clamp(this.az_, azLo, azHi);
    const elSum = clamp(this.el_, t.elMin, t.elMax);
    const rSum = clamp(this.radius_, rLo, rHi);

    // ---- THE ARM'S OWN SMOOTHING, before anything is built (:1024) --------
    {
      const aArm = this.framePrimed_ ? smoothAlpha(t.armSmoothing, dt) : 1.0;
      this.azLive_ += (azSum - this.azLive_) * aArm;
      this.elLive_ += (elSum - this.elLive_) * aArm;
      this.radLive_ += (rSum - this.radLive_) * aArm;
      this.radLive_ = clamp(this.radLive_, rLo, rHi);
    }
    const az = this.azLive_, el = this.elLive_, r = this.radLive_;

    const ce = Math.cos(el), se = Math.sin(el);
    let tipL = v3(r * ce * Math.sin(az), r * se, r * ce * Math.cos(az));

    // ---- how fast the point is moving, and which way (:1046) --------------
    // Measured in BASIS coordinates on purpose: in world space, turning the
    // view moves the tip without the blade having moved at all.
    const inst = (this.framePrimed_ && dt > 1e-6)
      ? vmul(vsub(tipL, this.tipPrev_), 1 / dt) : v3();
    this.tipPrev_ = tipL;
    this.tipVel_ = this.framePrimed_
      ? lerp3(this.tipVel_, inst, smoothAlpha(t.bladeSmoothing, dt)) : v3();

    const radial = vlen(tipL) > 1e-5 ? vnorm(tipL) : v3(0, 0, 1);
    {
      // The TANGENTIAL part of the travel is the stroke; the radial part is a
      // thrust. Held from last tick when there is nothing to read.
      const tv = vsub(this.tipVel_, vmul(radial, vdot(radial, this.tipVel_)));
      // melee.cpp: only when there is travel to read (leanMinSpeed).
      if (vlen(tv) > Math.max(t.leanMinSpeed, 1e-3)) this.tangent_ = vnorm(tv);
    }

    // ---- HOW FAR THE BLADE LEANS OFF THE RADIUS (:1063) -------------------
    // NOT A TASTE CONSTANT — a law of cosines. The point is at radius `r`, the
    // blade is a rigid bladeLen_ long, the hand is held at extendLive_: that
    // triangle has exactly one interior angle at the point.
    const handReachNow =
      (this.armValid_ ? this.armReach_ : t.fallbackReach) * t.reachFraction;
    this.extendLive_ = this.extendLive_ > 1e-4
      ? this.extendLive_ : this.radiusBand().hand;
    this.extendLive_ +=
      (clamp(handReachNow * t.handExtend, 0.05, handReachNow) - this.extendLive_) *
      smoothAlpha(t.extendSmoothing, dt);

    // WHICH WAY THE LEAN GOES, rotated toward the target ABOUT THE RADIUS at a
    // bounded rate — the only way to cross a reversal without passing through
    // the degenerate middle (:1097).
    {
      let want = this.tangent_;
      if (vlen(want) < 1e-3) want = this.perpL_;
      want = vsub(want, vmul(radial, vdot(radial, want)));
      if (vlen(want) < 1e-4) want = anyPerp(radial);
      want = vnorm(want);
      let cur = vsub(this.perpL_, vmul(radial, vdot(radial, this.perpL_)));
      if (vlen(cur) < 1e-4) cur = anyPerp(radial);
      cur = vnorm(cur);
      const c = clamp(vdot(cur, want), -1, 1);
      const sgn = vdot(vcross(cur, want), radial) < 0 ? -1 : 1;
      let turn = Math.acos(c) * sgn;
      // melee.cpp: a reversal is not a turn (leanFlipHold), and a program's
      // RETURN never re-leans (melee.h SetProgramDrive's holdLean).
      if (Math.abs(turn) > Math.max(t.leanFlipHold, 0)) turn = 0;
      // A program frame says which side outright (melee.cpp).
      if (this.programDrive_ && this.drive_.lean === 1) turn = 0;
      if (this.programDrive_ && (this.drive_.lean === 2 || this.drive_.lean === 3)) {
        let side = v3(this.drive_.lean === 3 ? 1 : -1, 0, 0);
        side = vsub(side, vmul(radial, vdot(radial, side)));
        if (vlen(side) > 1e-4) {
          side = vnorm(side);
          const c2 = clamp(vdot(cur, side), -1, 1);
          const s2 = vdot(vcross(cur, side), radial) < 0 ? -1 : 1;
          turn = Math.acos(c2) * s2;
        }
      }
      const maxTurn = Math.max(t.leanTurnRate, 0) * dt;
      turn = clamp(turn, -maxTurn, maxTurn);
      // Rodrigues about the radius; `cur` is already perpendicular to it.
      let p = vadd(vmul(cur, Math.cos(turn)),
                   vmul(vcross(radial, cur), Math.sin(turn)));
      this.perpL_ = vlen(p) > 1e-5 ? vnorm(p) : cur;
    }
    // melee.cpp: THE LEAN AS AN ANGLE, measured and as a frame may state it.
    {
      const { down, side } = axisFrame(radial, this.handSign_);
      const hs = t.handLead >= 0 ? 1 : -1;
      if (this.programDrive_ && this.drive_.lean === 4) {
        const la = this.drive_.leanAngle;
        this.perpL_ = vmul(vadd(vmul(down, Math.cos(la)), vmul(side, Math.sin(la))), hs);
      }
      const handSide = vmul(this.perpL_, hs);
      this.leanAngleNow_ = Math.atan2(vdot(handSide, side), vdot(handSide, down));
    }

    let wantDir = radial;
    if (this.bladeLen_ > 1e-4 && r > 1e-4) {
      let cosT = clamp(
        (r * r + this.bladeLen_ * this.bladeLen_ -
         this.extendLive_ * this.extendLive_) / (2.0 * this.bladeLen_ * r),
        -1, 1);
      // melee.cpp: a program frame's BLADE ANGLE replaces the solve outright.
      if (this.programDrive_ && this.drive_.bladeAngle >= 0)
        cosT = Math.cos(this.drive_.bladeAngle);
      this.bladeAngleNow_ = Math.acos(clamp(cosT, -1, 1));
      const sinT = Math.sqrt(Math.max(0, 1 - cosT * cosT));
      const s = t.handLead >= 0 ? 1 : -1;
      wantDir = vsub(vmul(radial, cosT), vmul(this.perpL_, sinT * s));
    }
    if (vlen(wantDir) < 1e-5) wantDir = radial;
    wantDir = vnorm(wantDir);
    // THE FLAT FACES OUT OF THE STROKE PLANE (:1132).
    let wantFlat = vcross(wantDir, this.tangent_);
    // melee.cpp: no usable travel holds the roll (flatMinSin), and the sign
    // agrees with the roll the blade already has (a flat names a PLANE).
    if (vlen(wantFlat) < clamp(t.flatMinSin, 0, 0.95)) wantFlat = this.bladeFlatL_;
    else if (vdot(wantFlat, this.bladeFlatL_) < 0) wantFlat = vmul(wantFlat, -1);
    if (vlen(wantFlat) < 1e-3) wantFlat = anyPerp(wantDir);
    wantFlat = vnorm(wantFlat);
    const a = smoothAlpha(t.bladeSmoothing, dt);
    // NOT SMOOTHED — the smoothing lives in extendLive_ and perpL_, which the
    // direction is exactly derived from (:1137).
    this.bladeDirL_ = wantDir;
    this.bladeFlatL_ = lerp3(this.bladeFlatL_, wantFlat, a);
    this.bladeFlatL_ = vsub(this.bladeFlatL_,
      vmul(this.bladeDirL_, vdot(this.bladeDirL_, this.bladeFlatL_)));
    this.bladeFlatL_ = vlen(this.bladeFlatL_) > 1e-5
      ? vnorm(this.bladeFlatL_) : anyPerp(this.bladeDirL_);

    // THE HAND IS THE TIP MINUS A BLADE (:1150).
    let handL = vsub(tipL, vmul(this.bladeDirL_, this.bladeLen_));
    const handReach =
      (this.armValid_ ? this.armReach_ : t.fallbackReach) * t.reachFraction;
    const hd = vlen(handL);
    if (handReach > 1e-3 && hd > handReach) handL = vmul(handL, handReach / hd);

    // ---- AND THE HAND MAY NOT GO BEHIND THE BODY (:1157) ------------------
    {
      const backLim = -Math.max(t.handBackFrac, 0) * handReach;
      if (handL.z < backLim) {
        handL = { ...handL, z: backLim };
        const bd = vsub(tipL, handL);
        if (vlen(bd) > 1e-4) {
          this.bladeDirL_ = vnorm(bd);
          this.bladeFlatL_ = vsub(this.bladeFlatL_,
            vmul(this.bladeDirL_, vdot(this.bladeDirL_, this.bladeFlatL_)));
          this.bladeFlatL_ = vlen(this.bladeFlatL_) > 1e-5
            ? vnorm(this.bladeFlatL_) : anyPerp(this.bladeDirL_);
        }
      }
    }

    // ---- AND THE BLADE STAYS OUT OF THE WIELDER'S OWN FACE (:1199) --------
    // A RIGID TRANSLATE: |tip - hand| is preserved exactly, so unlike the
    // plane clamp there is no re-aim. Runs LAST among the position clamps.
    if (this.keepR_ > 0 && t.headClear > 0) {
      const cL = v3(vdot(this.keepC_, right), vdot(this.keepC_, up),
                    vdot(this.keepC_, fwd));
      const R = this.keepR_ + t.headClear;
      const ab = vsub(tipL, handL);
      const ab2 = vdot(ab, ab);
      const tt = ab2 > 1e-6 ? clamp(vdot(vsub(cL, handL), ab) / ab2, 0, 1) : 0;
      const close = vadd(handL, vmul(ab, tt));
      let away = vsub(close, cL);
      const dd = vlen(away);
      if (dd < R) {
        away = dd > 1e-4 ? vmul(away, 1 / dd) : v3(0, 0, 1);
        const push = vmul(away, R - dd);
        handL = vadd(handL, push);
        tipL = vadd(tipL, push);
      }
    }

    // ---- THE WRIST CHASES THE FRAME ON ITS OWN CLOCK (:1228) --------------
    {
      const hl = t.wristSmoothing;
      const aw = this.framePrimed_ ? smoothAlpha(hl, dt) : 1.0;
      this.wristDirL_ = lerp3(this.wristDirL_, this.bladeDirL_, aw);
      this.wristDirL_ = vlen(this.wristDirL_) > 1e-5
        ? vnorm(this.wristDirL_) : this.bladeDirL_;
      this.wristFlatL_ = lerp3(this.wristFlatL_, this.bladeFlatL_, aw);
      this.wristFlatL_ = vsub(this.wristFlatL_,
        vmul(this.wristDirL_, vdot(this.wristDirL_, this.wristFlatL_)));
      this.wristFlatL_ = vlen(this.wristFlatL_) > 1e-5
        ? vnorm(this.wristFlatL_) : anyPerp(this.wristDirL_);
    }

    // ---- THE ELBOW TRAILS THE HAND (:1249) --------------------------------
    // and it is the HAND'S OWN travel that says so, not the point's: the
    // solver orthogonalizes the pole against the shoulder-to-HAND direction,
    // so a tip tangent is 89% component the solver throws away.
    {
      const handVel = (this.framePrimed_ && dt > 1e-6)
        ? vmul(vsub(handL, this.handPrev_), 1 / dt) : v3();
      this.handPrev_ = handL;
      this.handVel_ = this.framePrimed_
        ? lerp3(this.handVel_, handVel, a) : v3();
      const handDir = vlen(handL) > 1e-4 ? vnorm(handL) : v3(0, 0, 1);
      const along = vsub(this.handVel_,
        vmul(handDir, vdot(handDir, this.handVel_)));
      let wantPole = vlen(along) > 1e-2
        ? vmul(vnorm(along), -1) : v3(0, 0, -1);
      wantPole = vsub(wantPole, vmul(handDir, vdot(handDir, wantPole)));
      if (vlen(wantPole) < 1e-3) {
        wantPole = v3(0, 0, -1);
        wantPole = vsub(wantPole, vmul(handDir, vdot(handDir, wantPole)));
      }
      if (vlen(wantPole) < 1e-3) wantPole = anyPerp(handDir);
      this.poleL_ = lerp3(this.poleL_, vnorm(wantPole), a);
      this.poleL_ = vsub(this.poleL_, vmul(handDir, vdot(handDir, this.poleL_)));
      this.poleL_ = vlen(this.poleL_) > 1e-5
        ? vnorm(this.poleL_) : anyPerp(handDir);
      // ---- AND THE ELBOW MAY NOT COME FORWARD (:1287) --------------------
      // A cone rather than a half-space so the bound is continuous: clamping
      // a direction to a plane leaves it free to slide along that plane.
      {
        const ref = v3(0, 0, -1);
        const cone = clamp(t.elbowPoleCone, 0.05, 3.10);
        const c = clamp(vdot(this.poleL_, ref), -1, 1);
        if (Math.acos(c) > cone) {
          let perp = vsub(this.poleL_, vmul(ref, c));
          if (vlen(perp) < 1e-4) perp = anyPerp(ref);
          perp = vnorm(perp);
          this.poleL_ = vadd(vmul(ref, Math.cos(cone)),
                             vmul(perp, Math.sin(cone)));
          this.poleL_ = vsub(this.poleL_,
            vmul(handDir, vdot(handDir, this.poleL_)));
          this.poleL_ = vlen(this.poleL_) > 1e-5
            ? vnorm(this.poleL_) : anyPerp(handDir);
        }
      }
      // ---- THE ELBOW DIRECTION, measured and as a frame may state it
      // (melee.cpp): 0 = down, + toward the weapon side.
      const { down, side } = axisFrame(handDir, this.handSign_);
      if (this.programDrive_ && this.drive_.elbowSet) {
        const sw = this.drive_.elbowSwivel;
        this.poleL_ = vadd(vmul(down, Math.cos(sw)), vmul(side, Math.sin(sw)));
        this.poleL_ = vlen(this.poleL_) > 1e-5 ? vnorm(this.poleL_) : anyPerp(handDir);
      }
      this.elbowSwivelNow_ = Math.atan2(vdot(this.poleL_, side), vdot(this.poleL_, down));
    }

    // ---- HOW COMMITTED THIS IS, AND THEREFORE HOW MUCH WRIST (:1327) ------
    {
      // NO PHASE FORCES FULL ALIGNMENT (melee.cpp says why); an authored
      // PROGRAM does (melee.h SetProgramDrive), otherwise speed earns it.
      let want = this.programDrive_ ? clamp(this.drive_.wristAlign, 0, 1) : 1.0;
      if (!this.programDrive_) {
        const floorF = clamp(t.steerFloor, 0, 1);
        const lo = Math.max(t.steerSpeedLo, 0);
        const hi = Math.max(t.steerSpeedHi, lo + 1e-3);
        // THE SPEED IS THE INSTANTANEOUS ONE, not tipVel_ — a smoothed vector
        // magnitude COLLAPSES through a direction reversal (:1338).
        const v = vlen(inst);
        const k = clamp((v - lo) / (hi - lo), 0, 1);
        want = floorF + (1.0 - floorF) * k;
      }
      // THE ENVELOPE IS ASYMMETRIC: attack half the halflife, release 4x.
      const ease = smoothAlpha(
        t.wristSmoothing * (want > this.steerLive_ ? 0.5 : 4.0), dt);
      this.steerLive_ = this.framePrimed_
        ? this.steerLive_ + (want - this.steerLive_) * ease : want;
    }

    this.tipL_ = tipL;
    this.handL_ = handL;
    this.tip_ = toWorld(tipL, right, up, fwd);
    this.hand_ = toWorld(handL, right, up, fwd);
    this.bladeDir_ = toWorld(this.bladeDirL_, right, up, fwd);
    this.bladeFlat_ = toWorld(this.bladeFlatL_, right, up, fwd);
    this.wristDir_ = toWorld(this.wristDirL_, right, up, fwd);
    this.wristFlat_ = toWorld(this.wristFlatL_, right, up, fwd);
    this.bendPole_ = toWorld(this.poleL_, right, up, fwd);
    this.framePrimed_ = true;
  }

  /* ------------------------------------------------------------------------
     melee.cpp:1382 Update — the phase machine and the stroke integration.
     ---------------------------------------------------------------------- */
  update(dt, held, armed, right, up, fwd) {
    if (dt <= 0) return;
    const t = this.tuning;

    // ---- input velocity (:1387) ------------------------------------------
    const delta = this.inputAccum_;
    const instant = v3(this.inputAccum_.x / dt, this.inputAccum_.y / dt, 0);
    this.inputAccum_ = v3();
    this.mouseVel_ = lerp3(this.mouseVel_, instant, smoothAlpha(t.dirSmoothing, dt));
    this.mouseSpeed_ = Math.sqrt(
      this.mouseVel_.x * this.mouseVel_.x + this.mouseVel_.y * this.mouseVel_.y);

    if (!armed) { if (this.phase_ !== PHASE.Idle) this.reset(); }

    this.phaseTime_ += dt;

    let band = this.radiusBand();
    let rLo = band.lo, rHi = band.hi;
    const tipReach = rHi;
    const azHi = this.handSign_ > 0 ? t.azOut : t.azAcross;
    const azLo = this.handSign_ > 0 ? -t.azAcross : -t.azOut;

    // The seed of last resort (:1430).
    const guard = vadd(vadd(vmul(fwd, t.guardForward), vmul(up, t.guardUp)),
                       vmul(right, t.guardSide));

    let seededThisTick = false;

    switch (this.phase_) {
      case PHASE.Idle:
        if (armed && held) {
          // TAKE OVER FROM WHERE THE SWORD IS (:1436).
          const seedTip = this.armValid_ ? this.armTip_ : guard;
          const seedHand = this.armValid_ ? this.armHand_ : guard;
          let tipL = toBasis(seedTip, right, up, fwd);
          this.extendLive_ = Math.max(
            vlen(toBasis(seedHand, right, up, fwd)), 0.05);
          this.radius_ = vlen(tipL);
          band = this.radiusBand();
          rLo = band.lo; rHi = band.hi;
          if (this.radius_ < 1e-4) {
            tipL = v3(0, 0, tipReach);
            this.radius_ = tipReach;
          }
          this.el_ = Math.asin(clamp(tipL.y / this.radius_, -1, 1));
          this.az_ = Math.atan2(tipL.x, tipL.z);
          this.az_ = clamp(this.az_, azLo, azHi);
          this.el_ = clamp(this.el_, t.elMin, t.elMax);
          this.radius_ = clamp(this.radius_, rLo, rHi);
          this.azLive_ = this.az_;
          this.elLive_ = this.el_;
          this.radLive_ = this.radius_;

          // SEED THE DERIVED FRAME FROM THE BLADE ITSELF (:1480).
          const handL = toBasis(seedHand, right, up, fwd);
          const bd = vsub(tipL, handL);
          this.bladeDirL_ = vlen(bd) > 1e-4
            ? vnorm(bd)
            : (vlen(tipL) > 1e-5 ? vnorm(tipL) : v3(0, 0, 1));
          let fl = toBasis(this.armFlat_, right, up, fwd);
          fl = vsub(fl, vmul(this.bladeDirL_, vdot(this.bladeDirL_, fl)));
          this.bladeFlatL_ = vlen(fl) > 1e-3 ? vnorm(fl) : anyPerp(this.bladeDirL_);
          {
            // AND THE LEAN PLANE FROM THE BLADE'S OWN ANGLE (:1493).
            const rad = vlen(tipL) > 1e-5 ? vnorm(tipL) : v3(0, 0, 1);
            const pc = vsub(this.bladeDirL_,
              vmul(rad, vdot(rad, this.bladeDirL_)));
            const s = t.handLead >= 0 ? 1 : -1;
            if (vlen(pc) > 1e-4) {
              this.perpL_ = vmul(vnorm(pc), -s);
            } else {
              // A sword is swung sideways far more often than dropped, so the
              // HORIZONTAL perpendicular is the better prior (:1505).
              const h = vcross(rad, v3(0, 1, 0));
              this.perpL_ = vlen(h) > 1e-3 ? vnorm(h) : anyPerp(rad);
            }
          }
          this.poleL_ = v3(0, 0, -1);
          this.measureShapeNow_(tipL, handL);
          this.wristDirL_ = this.bladeDirL_;
          this.wristFlatL_ = this.bladeFlatL_;
          this.tangent_ = v3();
          this.tipVel_ = v3();
          this.tipPrev_ = tipL;
          this.handPrev_ = handL;
          this.handVel_ = v3();
          this.framePrimed_ = true;
          this.tipL_ = tipL; this.handL_ = handL;
          this.tip_ = toWorld(tipL, right, up, fwd);
          this.hand_ = toWorld(handL, right, up, fwd);
          this.bladeDir_ = toWorld(this.bladeDirL_, right, up, fwd);
          this.bladeFlat_ = toWorld(this.bladeFlatL_, right, up, fwd);
          this.wristDir_ = this.bladeDir_;
          this.wristFlat_ = this.bladeFlat_;
          this.bendPole_ = toWorld(this.poleL_, right, up, fwd);
          seededThisTick = true;

          this.phase_ = PHASE.Guard;
          this.phaseTime_ = 0;
        }
        break;

      case PHASE.Guard:
      case PHASE.Wind: {
        if (!held) {
          this.recoverHold_ = false;
          this.phase_ = PHASE.Recover;
          this.phaseTime_ = 0;
          break;
        }
        // Wind is only a LABEL (what arrest() reads as a live cut). NO
        // SPEED-TRIGGERED COMMIT (melee.cpp says why).
        this.phase_ = this.mouseSpeed_ > t.commitSpeed * 0.35
          ? PHASE.Wind : PHASE.Guard;
        break;
      }

      case PHASE.Recover:
        // ---- THE BUTTON WENT UP MID-FOLLOW-THROUGH (melee.cpp:2430) -------
        // `recoverHold_` is latched by arrest() and
        // until 2026-09-21 there was NO PATH THAT EVER CLEARED IT, in the
        // engine or here: a stroke that committed a cut and then released held
        // poseWeight at 1 for the whole recover and dropped it to 0 on the one
        // tick the phase ended. That is the "the sword teleports at the end"
        // report, and THIS PORT WENT ON SHOWING IT after the engine stopped —
        // a preview whose whole job is to be the engine was lying about the
        // one thing being authored.
        if (this.recoverHold_ && !(armed && held)) {
          this.recoverHold_ = false;
          this.phaseTime_ = 0;
        }
        if (this.phaseTime_ >= this.recoverTime()) {
          this.phase_ = (armed && held) ? PHASE.Guard : PHASE.Idle;
          this.phaseTime_ = 0;
        }
        break;
    }

    // ---- integrate the stroke (:1594) ------------------------------------
    // THE CLAMPS ARE ON THE STORED VALUE, which is what stops a sustained push
    // into a stop from banking travel.
    if (this.phase_ !== PHASE.Idle && !seededThisTick) {
      this.az_ = clamp(this.az_ + delta.x * t.aimGainX, azLo, azHi);
      this.el_ = clamp(this.el_ - delta.y * t.aimGainY, t.elMin, t.elMax);
      this.radius_ = clamp(this.radius_ + delta.z * t.reachGain, rLo, rHi);
    }

    if (this.phase_ === PHASE.Idle) {
      // Let the arm hang; the walk cycle owns the pose (:1650).
      const k = smoothAlpha(0.12, dt);
      this.hand_ = lerp3(this.hand_, v3(), k);
      this.tip_ = lerp3(this.tip_, v3(), k);
      this.framePrimed_ = false;
    } else if (!seededThisTick) {
      this.rebuildFrame(dt, right, up, fwd);
    }

    if (vlen(this.bladeDir_) < 1e-4) this.bladeDir_ = up;
    if (vlen(this.bladeFlat_) < 1e-4) this.bladeFlat_ = fwd;
    if (vlen(this.wristDir_) < 1e-4) this.wristDir_ = this.bladeDir_;
    if (vlen(this.wristFlat_) < 1e-4) this.wristFlat_ = this.bladeFlat_;
    if (vlen(this.bendPole_) < 1e-4) this.bendPole_ = vmul(fwd, -1);
    this.programDrive_ = false;   // one-shot, as melee.cpp
  }
}

/* ============================================================================
   THE STROKE PROGRAM — src/game/strokes.cpp / strokes.h.
   ========================================================================== */

// strokes.cpp:168 kNeutralReach. "A little past the middle, because a guard is
// held forward of centre."
export const kNeutralReach = 0.60;
// strokes.cpp — the style-pick salt.
const kSaltStyle = 0x5CA1E5;

/**
 * strokes.cpp:170 StrokeReachIn — an authored reach OFFSET -> a radius the arm
 * can actually serve.
 *
 * Authored against the ARM rather than the band, every chamber and lunge in
 * the library landed outside it and was clamped: the commanded radius moved
 * 0.15 voxels on a stroke asking for four, and a thrust read as a twitch.
 */
export function strokeReachIn(m, offset) {
  const { lo, hi } = m.reachBand();
  const span = hi > lo ? hi - lo : 0;
  return clamp(lo + (kNeutralReach + offset) * span, lo, hi);
}

// strokes.h:158 StrokeCursor.
export const STROKE_PHASE = {
  Idle: 0, Guard: 1, Windup: 2, Cut: 3, Recover: 4,
};
export const STROKE_PHASE_NAME = ['idle', 'guard', 'windup', 'cut', 'recover'];

export function newStrokeCursor() {
  return {
    phase: STROKE_PHASE.Idle,
    style: -1,
    phaseTick: 0,
    windupTicks: 0, cutTicks: 0, recoverTicks: 0, settleTicks: 0,
    // The cut path's resolved tick table (strokes.h StrokeCursor::legTicks):
    // the tempo jitter applied PER LEG, once, so a corner cannot drift under
    // the drive. `cutTicks` is their sum, which is why nothing outside the
    // runner has to know legs exist.
    cutLegs: 1, legTicks: [], cutLeg: 0,
    // strokes.h: the frame program.
    frames: 0, frameTicks: [], frame: 0, frameTick: 0,
    firstCut: -1, lastCut: -1, releasing: false, releaseLeft: 0,
    bladeFrom: 0, elbowFrom: 0, leanFrom: 0,
    fromPending: false, fromTick: 0,
    seed: 0,
    aimAz: 0, aimEl: 0, aimed: false,
    wantAz: 0, wantEl: 0, wantReach: 0,
    // strokes.h StrokeCursor::liveAz/liveEl — the aim the last step was
    // handed, which a KEYED frame's pose is turned toward (strokeKeyedPose).
    liveAz: 0, liveEl: 0,
  };
}

// strokes.h kMaxCutLegs.
export const MAX_CUT_LEGS = 6;

/**
 * strokes.h AttackStyle::CutTravel / CutThrough / CutAimOffset — the three
 * ways the path is read, ported so the preview, the panel's derived line and
 * the engine agree on what a style ASKS for.
 *
 * `cutThrough(sty, k)` is cumulative THROUGH leg k inclusive (k < 0 = the
 * start of the path); `cutTravel` is the whole of it.
 */
export function cutThrough(sty, k) {
  const out = { ticks: 0, az: 0, el: 0, reach: 0 };
  const legs = sty.cut || [];
  for (let i = 0; i <= k && i < legs.length; i++) {
    out.ticks += legs[i].ticks;
    out.az += legs[i].az;
    out.el += legs[i].el;
    out.reach += legs[i].reach;
  }
  return out;
}
export const cutTravel = (sty) => cutThrough(sty, (sty.cut || []).length - 1);
// WHERE THE TARGET SITS ALONG THE PATH, from the cut's start: the middle of
// the leg that claims the aim, or the midpoint of the whole travel (which for
// one leg is the historical cut/2). Not read by the runner any more (strokes.h
// "THE STROKE FRAME"): it is what the panel's "centre" writes into the windup
// (`windup = -this`) and what the legacy conversion subtracts.
export function cutAimOffset(sty) {
  const legs = sty.cut || [];
  const k = sty.aimLeg;
  if (Number.isInteger(k) && k >= 0 && k < legs.length) {
    const before = cutThrough(sty, k - 1);
    return { az: before.az + 0.5 * legs[k].az, el: before.el + 0.5 * legs[k].el };
  }
  const all = cutTravel(sty);
  return { az: 0.5 * all.az, el: 0.5 * all.el };
}

/**
 * strokes.h AttackStyle::FromLegacyFrame — a PARSED style from the
 * pre-2026-09-25 frame (windup measured from "half a cut short of the aim")
 * to the target frame (windup measured from the target, the cut starting at
 * the windup). The same program: the windup pose moves by -cutAimOffset and
 * leg 1 absorbs the old windup offset, which the old cut ignored.
 */
export function fromLegacyFrame(sty) {
  const off = cutAimOffset(sty);
  const wAz = sty.windup.az, wEl = sty.windup.el;
  sty.windup = { ...sty.windup, az: wAz - off.az, el: wEl - off.el };
  if (sty.cut && sty.cut.length) {
    sty.cut = sty.cut.map((l, i) => (i === 0
      ? { ...l, az: l.az - wAz, el: l.el - wEl } : l));
  }
  return sty;
}

/**
 * THE SAME CONVERSION ON THE RAW FILE, so the editor (and the one-off
 * migration of assets/mobs/attack_styles.json) rewrites the numbers an author
 * sees rather than converting them behind the panel's back. Returns true if
 * it converted anything; stamps `strokeFrame: "target"`. Player blocks are
 * converted as the loader would merge them: in the OLD frame first, and any
 * block that states geometry gets its windup az/el and first leg az/el
 * written out explicitly in the new one.
 */
export function migrateRawToTargetFrame(json) {
  if (!json || typeof json !== 'object' || json.strokeFrame === 'target')
    return false;
  const r6 = v => Math.round(v * 1e6) / 1e6;
  const num = (v, d) => (Number.isFinite(+v) ? +v : d);
  const legsOf = (cut) => (Array.isArray(cut)
    ? cut.filter(l => l && typeof l === 'object')
    : [cut && typeof cut === 'object' ? cut : {}]);
  const offOf = (legs) => {
    let k = legs.findIndex(l => l.aim === true);
    const az = legs.map(l => num(l.az, 0)), el = legs.map(l => num(l.el, 0));
    const sum = (a, n) => a.slice(0, n).reduce((x, y) => x + y, 0);
    if (k >= 0) return { az: sum(az, k) + 0.5 * az[k], el: sum(el, k) + 0.5 * el[k] };
    return { az: 0.5 * sum(az, az.length), el: 0.5 * sum(el, el.length) };
  };
  for (const s of (Array.isArray(json.styles) ? json.styles : [])) {
    if (!s || typeof s !== 'object') continue;
    // strokes.cpp's default windup when a style states none: write it out, so
    // the converted number is the one the engine was really using.
    if (!s.windup || typeof s.windup !== 'object')
      s.windup = { ticks: 12, az: 0.30, el: 0.10, reach: -0.05 };
    const W = { az: num(s.windup.az, 0.30), el: num(s.windup.el, 0.10) };
    if (s.cut === undefined) s.cut = { ticks: 7, az: -2.0, el: 0, reach: 0.10 };
    const legs = legsOf(s.cut);
    const off = offOf(legs);
    // ---- the player block, in the OLD frame, BEFORE the base moves -------
    const p = s.player && typeof s.player === 'object' ? s.player : null;
    let pOut = null;
    if (p) {
      const pw = p.windup && typeof p.windup === 'object' ? p.windup : null;
      const geomW = pw && (pw.az !== undefined || pw.el !== undefined);
      const geomC = Array.isArray(p.cut) ||
        (p.cut && typeof p.cut === 'object' &&
         (p.cut.az !== undefined || p.cut.el !== undefined));
      if (geomW || geomC) {
        const Wp = { az: num(pw?.az, W.az), el: num(pw?.el, W.el) };
        let legsP;
        if (Array.isArray(p.cut)) legsP = legsOf(p.cut);
        else {
          legsP = legs.map(l => ({ ...l }));
          if (p.cut && typeof p.cut === 'object')
            legsP[0] = { ...legsP[0],
                         ...(p.cut.az !== undefined ? { az: p.cut.az } : {}),
                         ...(p.cut.el !== undefined ? { el: p.cut.el } : {}) };
        }
        const offP = offOf(legsP);
        pOut = { wAz: r6(Wp.az - offP.az), wEl: r6(Wp.el - offP.el),
                 l0Az: r6(num(legsP[0].az, 0) - Wp.az),
                 l0El: r6(num(legsP[0].el, 0) - Wp.el) };
      }
    }
    // ---- the base --------------------------------------------------------
    s.windup.az = r6(W.az - off.az);
    s.windup.el = r6(W.el - off.el);
    legs[0].az = r6(num(legs[0].az, 0) - W.az);
    legs[0].el = r6(num(legs[0].el, 0) - W.el);
    if (pOut) {
      if (!p.windup || typeof p.windup !== 'object') p.windup = {};
      p.windup.az = pOut.wAz;
      p.windup.el = pOut.wEl;
      if (Array.isArray(p.cut)) {
        const lp = legsOf(p.cut);
        lp[0].az = pOut.l0Az;
        lp[0].el = pOut.l0El;
      } else {
        if (!p.cut || typeof p.cut !== 'object') p.cut = {};
        p.cut.az = pOut.l0Az;
        p.cut.el = pOut.l0El;
      }
    }
  }
  json.strokeFrame = 'target';
  return true;
}

/**
 * strokes.cpp:177 BeginStrokeProgram. Fills a cursor's program fields for one
 * swing; the CALLER resets its own container and re-tunes the MeleeState.
 */
export function beginStrokeProgram(cur, sty, styleIndex, seed) {
  cur.style = styleIndex;
  cur.fromPending = false;
  cur.fromTick = 0;
  cur.seed = seed >>> 0;
  const tempo = 1.0 + sty.jitter.tempo * signedUnit(hash3(cur.seed, 1, 0));
  const frames = sty.frames || [];
  cur.frames = Math.min(frames.length, MAX_FRAMES);
  cur.firstCut = Math.min(firstCut(sty), cur.frames - 1);
  cur.lastCut = Math.min(lastCut(sty), cur.frames - 1);
  // strokes.cpp: tempo stretches everything up to and including the cut.
  cur.frameTicks = [];
  for (let k = 0; k < cur.frames; k++) {
    const base = frames[k].ticks;
    const jit = cur.lastCut < 0 || k <= cur.lastCut;
    cur.frameTicks.push(Math.max(1, jit ? Math.round(base * tempo) : base));
  }
  let windup = 0, cut = 0, post = 0;
  for (let k = 0; k < cur.frames; k++) {
    const ph = phaseOfFrame(cur, k);
    if (ph === STROKE_PHASE.Windup) windup += cur.frameTicks[k];
    else if (ph === STROKE_PHASE.Cut) cut += cur.frameTicks[k];
    else post += cur.frameTicks[k];
  }
  if (cur.firstCut > 0 && windup < 2) { cur.frameTicks[cur.firstCut - 1] += 2 - windup; windup = 2; }
  if (cur.lastCut >= 0 && cut < 2) { cur.frameTicks[cur.lastCut] += 2 - cut; cut = 2; }
  cur.windupTicks = windup;
  cur.cutTicks = cut;
  cur.settleTicks = post;
  cur.recoverTicks = post + Math.max(1, sty.release);
  cur.cutLegs = cur.firstCut >= 0
    ? clamp(cur.lastCut - cur.firstCut + 1, 1, MAX_CUT_LEGS) : 1;
  cur.legTicks = [];
  for (let i = 0; i < cur.cutLegs; i++) cur.legTicks.push(cur.frameTicks[cur.firstCut + i] || 0);
  cur.cutLeg = 0;
  cur.frame = 0;
  cur.frameTick = 0;
  cur.releasing = false;
  cur.releaseLeft = 0;
  cur.aimed = false;
  if (cur.frames <= 0) { startRelease(cur, Math.max(1, sty.release)); return; }
  cur.phase = phaseOfFrame(cur, 0);
  cur.phaseTick = 0;
}

// strokes.h StrokeCursor::StartRelease
export function startRelease(cur, ticks) {
  cur.phase = STROKE_PHASE.Recover;
  cur.phaseTick = 0;
  cur.releasing = true;
  cur.releaseLeft = ticks > 0 ? ticks : 1;
  cur.recoverTicks = cur.releaseLeft;
}

function phaseOfFrame(cur, k) {
  if (cur.firstCut < 0 || k < cur.firstCut) return STROKE_PHASE.Windup;
  if (k <= cur.lastCut) return STROKE_PHASE.Cut;
  return STROKE_PHASE.Recover;
}

/* ============================================================================
   A FRAME IS A POSE (strokes.h "A FRAME IS A POSE", 2026-09-25).

   A frame with a `pose` states the ARM ITSELF at its end: the shoulder, elbow
   and wrist rotations relative to their parents, and the torso's twist and
   pitch. A style whose every frame has one is KEYED, and a keyed stroke never
   asks the stroke driver where the arm goes: each joint SLERPS from the frame
   before's pose to this one's on the frame's own ease, and the rig writes
   those rotations straight in (rig.js smoothWeaponArm, mob.cpp
   SmoothWeaponArm). No IK, no lean plane chasing the travel, no pole chasing
   the hand, so nothing between two frames can do anything the two frames did
   not already show. The driver still runs underneath for the phase clock, the
   cut window and the sounds; its arm output is simply not used.

   AIM. A pose is stored as authored against a target STRAIGHT AHEAD of the
   shoulder. A frame measured from the target (`from` target, the default)
   is turned, whole arm about the shoulder, by the target's bearing — yaw
   about the body's up, then pitch about its right — and a `from: body`
   frame is not. Each END of the blend carries its own turn, so a frame that
   goes from body-relative to target-relative swings over smoothly rather
   than jumping.
   ========================================================================== */

// [x, y, z, w] -> a unit quaternion, or null.
function readQuat(a) {
  if (!Array.isArray(a) || a.length !== 4 || !a.every(Number.isFinite)) return null;
  return AN.qnorm({ x: a[0], y: a[1], z: a[2], w: a[3] });
}
/** strokes.cpp ReadPose — a frame's `pose` block, or null. */
export function readPose(p) {
  if (!p || typeof p !== 'object') return null;
  const s = readQuat(p.shoulder), e = readQuat(p.elbow);
  if (!s || !e) return null;
  const num = (v) => (Number.isFinite(+v) ? +v : 0);
  return { joint: [s, e, readQuat(p.wrist)], twist: num(p.twist), pitch: num(p.pitch) };
}
export function poseToJson(p) {
  const r6 = v => Math.round(v * 1e6) / 1e6;
  const q = v => [r6(v.x), r6(v.y), r6(v.z), r6(v.w)];
  const o = { shoulder: q(p.joint[0]), elbow: q(p.joint[1]) };
  if (p.joint[2]) o.wrist = q(p.joint[2]);
  if (p.twist) o.twist = r6(p.twist);
  if (p.pitch) o.pitch = r6(p.pitch);
  return o;
}
/** strokes.h AttackStyle::Keyed — every frame is a pose. */
export const styleKeyed = (sty) =>
  !!sty && Array.isArray(sty.frames) && sty.frames.length > 0 &&
  sty.frames.every(f => !!f.pose);

/**
 * THE AIM ROTATION, rig (model) space at heading 0, where right = -X, up = +Y,
 * fwd = +Z: it carries straight ahead onto the bearing (yaw, pitch), yaw
 * positive toward the wielder's right and pitch positive up — the stroke
 * driver's own az/el convention. Pitch first, then yaw.
 */
export function aimQuat(yaw, pitch) {
  return AN.qmul(AN.qaxisangle(v3(0, 1, 0), -yaw), AN.qaxisangle(v3(1, 0, 0), -pitch));
}

/**
 * strokes.cpp StrokeKeyedPose — what the keyed arm is this tick, read off the
 * cursor AFTER the step (so it is the pose at the END of the tick just run).
 * Returns null for a style that is not keyed. `release` is the driver's eased
 * hand-back progress (-1 while the program owns the arm).
 *
 *   on        the rig writes these joints instead of solving the arm
 *   fromLive  blend from the arm as it stood when the stroke took it
 *   from/to   [shoulder, elbow, wrist] parent-relative rotations
 *   t         0..1, already eased
 *   aimFrom / aimTo   {yaw, pitch}: each end's turn about the shoulder
 *   twist/pitch       the torso, radians
 */
export function strokeKeyedPose(cur, sty, release = -1) {
  if (!styleKeyed(sty)) return null;
  const fr = sty.frames;
  const n = Math.max(1, Math.min(fr.length, cur.frames || fr.length));
  const last = fr[n - 1].pose;
  const bowAz = sty.jitter.az * signedUnit(hash3(cur.seed, 2, 0));
  const bowEl = sty.jitter.el * signedUnit(hash3(cur.seed, 3, 0));
  const aimOf = (k) => {
    const f = fr[k];
    if (f.fromBody) return { yaw: 0, pitch: 0 };
    const preCut = cur.firstCut < 0 || k < cur.firstCut;
    return {
      yaw: (cur.aimed ? cur.aimAz : cur.liveAz) + (preCut ? bowAz : 0),
      pitch: (cur.aimed ? cur.aimEl : cur.liveEl) + (preCut ? bowEl : 0),
    };
  };
  // The last frame's pose, still, with its aim.
  const hold = () => {
    const k = n - 1;
    const a = aimOf(k);
    return { on: true, fromLive: false, from: last.joint, to: last.joint, t: 1,
             aimFrom: a, aimTo: a, twist: last.twist, pitch: last.pitch };
  };
  if (cur.releasing || cur.phase === STROKE_PHASE.Idle) {
    // THE TICK THE LAST FRAME ENDS the program is already releasing while the
    // driver has not begun its hand-back (release < 0): the arm is still the
    // stroke's, so it HOLDS the last pose — left alone it was the driver's IK
    // arm for one tick, a 100-degree snap.
    if (cur.releasing && release < 0) return hold();
    // The torso comes home on the same curve the arm does.
    const w = release >= 0 ? 1 - release : 0;
    return { on: false, twist: last.twist * w, pitch: last.pitch * w };
  }
  let k = cur.frame, t;
  if (k >= n) { k = n - 1; t = 1; }
  else {
    const len = Math.max(1, cur.frameTicks[k] || fr[k].ticks);
    const e = fr[k].ease === 'chase' ? 'linear' : fr[k].ease;
    t = AN.applyEase(e, clamp(cur.frameTick / len, 0, 1));
  }
  const to = fr[k].pose;
  const from = k > 0 ? fr[k - 1].pose : null;
  const tw0 = from ? from.twist : 0, pt0 = from ? from.pitch : 0;
  return {
    on: true, fromLive: !from,
    from: from ? from.joint : null, to: to.joint, t,
    aimFrom: from ? aimOf(k - 1) : { yaw: 0, pitch: 0 },
    aimTo: aimOf(k),
    twist: tw0 + (to.twist - tw0) * t,
    pitch: pt0 + (to.pitch - pt0) * t,
  };
}

/** A HELD frame k of a keyed style: its pose, standing still. */
export function keyedFramePose(sty, k, aimAz, aimEl) {
  if (!sty || !sty.frames || !Number.isInteger(k) || k < 0 || k >= sty.frames.length ||
      !sty.frames[k].pose)
    return null;
  const f = sty.frames[k];
  const a = f.fromBody ? { yaw: 0, pitch: 0 } : { yaw: aimAz, pitch: aimEl };
  return { on: true, fromLive: false, from: f.pose.joint, to: f.pose.joint, t: 1,
           aimFrom: a, aimTo: a, twist: f.pose.twist, pitch: f.pose.pitch };
}

// strokes.h:253 StrokeStepResult.
export const STEP = { Idle: 0, Live: 1, Finished: 2 };

// strokes.cpp StrokeJointsNow — the per-joint brakes in force right now.
export function strokeJointsNow(cur, sty) {
  const fr = sty && sty.frames && sty.frames.length
    ? sty.frames[clamp(cur.frame, 0, sty.frames.length - 1)] : null;
  return fr ? fr.joints : (sty ? sty.joints : null);
}

/**
 * strokes.cpp StepStrokeProgram — one tick of the FRAME program. Every frame
 * is driven the same way: toward WHERE ITS TIP ENDS, arriving exactly on its
 * last tick, paced by its curve, shaped by its own lean / wrist / torso.
 *
 * `liveAz`/`liveEl` are the aim's CURRENT bearing about the wielder's
 * shoulder. A player attack passes (0, 0), because the camera IS the aim; the
 * tuner's preview passes whatever the panel's aim sliders say.
 */
export function stepStrokeProgram(cur, sty, m, liveAz, liveEl, dt, right, up, fwd,
                                  liveDist = 0) {
  // strokes.cpp toTarget: THE REACH A TARGET-MEASURED FRAME ASKS FOR is capped
  // a little past the target itself, so a stroke aimed at something close does
  // not punch through it. `liveDist` is the target's distance from the same
  // pivot liveAz/liveEl are measured about (0 = no target, no cap); it is a
  // trailing argument here only so callers that have no target stay as they were.
  const toTarget = banded => (liveDist > 0 ? Math.min(banded, liveDist * 1.2) : banded);
  cur.liveAz = liveAz; cur.liveEl = liveEl;
  const bowAz = sty ? sty.jitter.az * signedUnit(hash3(cur.seed, 2, 0)) : 0;
  const bowEl = sty ? sty.jitter.el * signedUnit(hash3(cur.seed, 3, 0)) : 0;
  const smp = { dx: 0, dy: 0, dReach: 0, held: true };
  const frames = (sty && sty.frames) || [];
  const fr = frames.length && cur.phase !== STROKE_PHASE.Idle &&
             cur.phase !== STROKE_PHASE.Guard
    ? frames[clamp(cur.frame, 0, frames.length - 1)] : null;
  if (cur.phase === STROKE_PHASE.Guard || fr) {
    let d = null;
    if (fr) {
      d = { lean: LEAN_CODE[fr.lean] || 0, wristAlign: fr.wristAlign,
            torsoTwist: fr.torsoTwist, torsoPitch: fr.torsoPitch };
      // strokes.cpp: the arm's shape, BLENDED over the frame from where it
      // was when the frame began, on the frame's own curve.
      const live = !cur.releasing && cur.frame < cur.frames;
      // strokes.cpp: THE TAKE-OVER TICK HAS NO SHAPE TO READ. The driver is
      // still Idle, so its "now" angles are whatever the LAST swing left, and
      // blending from those snapped the blade on the first driven tick. The
      // take-over measures the seeded arm (update()), and the capture is taken
      // on the tick after it.
      if (live && (cur.frameTick === 0 || cur.fromPending)) {
        if (m.phase_ === PHASE.Idle) cur.fromPending = true;
        else {
          cur.bladeFrom = m.bladeAngleNow_;
          cur.elbowFrom = m.elbowSwivelNow_;
          cur.leanFrom = m.leanAngleNow_;
          cur.fromPending = false;
          cur.fromTick = cur.frameTick;
        }
      }
      // The blend runs over the ticks LEFT once the shape was captured, so a
      // capture deferred past the take-over tick does not spend two ticks'
      // worth in its first one.
      let p = 1;
      if (live) {
        const len = Math.max(1, cur.frameTicks[cur.frame]);
        const q = (cur.frameTick + 1 - cur.fromTick) / Math.max(1, len - cur.fromTick);
        p = fr.ease === 'chase' ? q : AN.applyEase(fr.ease, q);
      }
      if (fr.bladeAngle >= 0) d.bladeAngle = cur.bladeFrom + (fr.bladeAngle - cur.bladeFrom) * p;
      // strokes.cpp: the two ANGLES ABOUT AN AXIS go the SHORT way round,
      // decided by the captured start and the frame's end -- fixed for the
      // frame, so the way round cannot flip mid-frame. Measured in axisFrame,
      // which has no bad point an arm can reach, so the blend is smooth.
      if (fr.elbowSet) {
        d.elbowSet = true;
        d.elbowSwivel = cur.elbowFrom + wrapPi(fr.elbow - cur.elbowFrom) * p;
      }
      if (fr.lean === 'angle')
        d.leanAngle = cur.leanFrom + wrapPi(fr.leanAngle - cur.leanFrom) * p;
    }
    m.setProgramDrive(d);
  }
  m.setRecoverTime(sty ? Math.max(1, sty.release) * dt : 0);

  switch (cur.phase) {
    case STROKE_PHASE.Guard: {
      const t = m.tuning;
      const { lo, hi } = m.reachBand();
      const wantR = clamp(lo + cur.wantReach * (hi - lo), lo, hi);
      smp.dx = clamp((cur.wantAz - m.strokeAz()) / t.aimGainX, -16, 16);
      smp.dy = clamp(-(cur.wantEl - m.strokeEl()) / t.aimGainY, -16, 16);
      smp.dReach = clamp((wantR - m.strokeRadius()) / Math.max(t.reachGain, 1e-4), -18, 18);
      m.step(smp, dt, true, right, up, fwd);
      break;
    }
    case STROKE_PHASE.Windup:
    case STROKE_PHASE.Cut:
    case STROKE_PHASE.Recover: {
      if (cur.releasing) {
        smp.held = false;
        m.step(smp, dt, true, right, up, fwd);
        cur.phaseTick++;
        if (--cur.releaseLeft <= 0) return STEP.Finished;
        break;
      }
      if (!sty || !frames.length || cur.frame >= cur.frames) {
        startRelease(cur, sty ? sty.release : 1);
        break;
      }
      const k = cur.frame;
      const f = frames[k];
      if (!cur.aimed && cur.firstCut >= 0 && k >= cur.firstCut) {
        cur.aimAz = liveAz; cur.aimEl = liveEl; cur.aimed = true;
      }
      let toAz = f.az, toEl = f.el;
      let toR = strokeReachIn(m, f.reach);
      if (!f.fromBody) {
        const preCut = cur.firstCut < 0 || k < cur.firstCut;
        toAz += (cur.aimed ? cur.aimAz : liveAz) + (preCut ? bowAz : 0);
        toEl += (cur.aimed ? cur.aimEl : liveEl) + (preCut ? bowEl : 0);
        toR = toTarget(toR);
      }
      cur.wantAz = toAz; cur.wantEl = toEl; cur.wantReach = toR;
      const t = m.tuning;
      const rg = Math.max(t.reachGain, 1e-4);
      const len = Math.max(1, cur.frameTicks[k]);
      const into = cur.frameTick;
      if (f.ease === 'chase') {
        // strokes.cpp: CHASE closes at the frame's own capped rate.
        const cap = f.chaseRate, capR = f.chaseRate * 18 / 16;
        smp.dx = clamp((toAz - m.strokeAz()) / t.aimGainX, -cap, cap);
        smp.dy = clamp(-(toEl - m.strokeEl()) / t.aimGainY, -cap, cap);
        smp.dReach = clamp((toR - m.strokeRadius()) / rg, -capR, capR);
      } else if (f.ease === 'linear') {
        const left = Math.max(1, len - into);
        smp.dx = ((toAz - m.strokeAz()) / left) / t.aimGainX;
        smp.dy = -((toEl - m.strokeEl()) / left) / t.aimGainY;
        smp.dReach = ((toR - m.strokeRadius()) / left) / rg;
      } else {
        const share = strokeEaseStep(f.ease, into / len, (into + 1) / len);
        smp.dx = ((toAz - m.strokeAz()) * share) / t.aimGainX;
        smp.dy = -((toEl - m.strokeEl()) * share) / t.aimGainY;
        smp.dReach = ((toR - m.strokeRadius()) * share) / rg;
      }
      m.step(smp, dt, true, right, up, fwd);
      if (cur.phase === STROKE_PHASE.Cut) cur.cutLeg = k - cur.firstCut;
      cur.phaseTick++;
      if (++cur.frameTick >= len) {
        if (k + 1 === cur.firstCut) { cur.aimAz = liveAz; cur.aimEl = liveEl; cur.aimed = true; }
        cur.frame++;
        cur.frameTick = 0;
        let next;
        if (cur.frame >= cur.frames) {
          cur.releasing = true;
          cur.releaseLeft = Math.max(1, sty.release);
          next = STROKE_PHASE.Recover;
        } else {
          next = phaseOfFrame(cur, cur.frame);
        }
        if (next !== cur.phase) { cur.phase = next; cur.phaseTick = 0; }
      }
      break;
    }
    default:
      return STEP.Idle;
  }
  return STEP.Live;
}

/* ============================================================================
   THE GOAL FRAMES OF A STYLE — an authoring view, not a driver path.

   A stroke program is a list of DESTINATIONS with pacing between them, and
   until now the only way to see a destination was to watch the arm travel to
   it. These two functions name the destinations and resolve each one to the
   (az, el, radius) the Cut/Windup/Recover phases above actually target, so the
   preview can put the arm ON one and hold it.

   THE TARGETS ARE THE SAME EXPRESSIONS THE PHASES USE, deliberately — the
   windup's `aim + windup`, leg k's `aim + windup + CutThrough(k)`, the
   recover's ABSOLUTE stance. If a goal pose and the pose
   the program arrives at ever disagree, one of the two is wrong and the point
   of sharing the arithmetic is that it is visible.

   THE START BOW IS EXCLUDED. `jitter.az/el` is a per-swing DRAW, and a goal is
   what was authored; including it would make the frame move every reroll and
   make the author chase a number that is not in the file.
   ========================================================================== */

/** The destinations of `sty`: one per FRAME, in order. */
export function strokeGoals(sty) {
  if (!sty || !sty.frames) return [];
  return sty.frames.map((f, k) => ({
    key: 'f' + k, frame: k,
    label: f.name || `frame ${k + 1}`,
  }));
}

/**
 * One frame's destination resolved against the arm currently previewing, with
 * the TRAVEL through it (the direction from the frame before; a HOLD lean
 * keeps the travel of the frame that set it) and its DRIVE (lean, wrist,
 * torso), which is what MeleeState.snapToPose builds the arm from.
 */
export function strokeGoalPose(sty, m, key, aimAz, aimEl) {
  if (!sty || !m || !sty.frames || typeof key !== 'string' || key[0] !== 'f') return null;
  const k = +key.slice(1);
  const fr = sty.frames;
  if (!Number.isInteger(k) || k < 0 || k >= fr.length) return null;
  const at = (i) => ({
    az: fr[i].az + (fr[i].fromBody ? 0 : aimAz),
    el: fr[i].el + (fr[i].fromBody ? 0 : aimEl),
  });
  const travelInto = (i) => {
    if (fr.length < 2) return { az: 0, el: 0 };
    const a = i > 0 ? at(i - 1) : at(0), b = i > 0 ? at(i) : at(1);
    return { az: b.az - a.az, el: b.el - a.el };
  };
  let src = k;
  while (src > 0 && fr[src].lean === 'hold') src--;
  const p = at(k);
  const f = fr[k];
  return {
    az: p.az, el: p.el, reach: strokeReachIn(m, f.reach),
    travel: travelInto(src),
    drive: { lean: LEAN_CODE[f.lean] || 0, wristAlign: f.wristAlign,
             torsoTwist: f.torsoTwist, torsoPitch: f.torsoPitch,
             bladeAngle: f.bladeAngle, elbowSet: f.elbowSet, elbowSwivel: f.elbow,
             leanAngle: f.leanAngle || 0 },
  };
}

/**
 * WHERE THE PROGRAM ACTUALLY ARRIVES AT FRAME `key` — what the Attacks lane's
 * goal chips HOLD (2026-09-25).
 *
 * strokeGoalPose above is the AUTHORED destination, and a held frame built
 * from it (snapToPose: the tip on the target, every eased channel settled)
 * disagreed with what a solo'd segment actually stops on, for three reasons
 * that are all real behaviour of the driver, not bugs in either path:
 *   - a `chase` frame closes at a capped rate and a short one stops SHORT
 *     (the shipped windups, 0.3-0.9 rad short on the player styles);
 *   - the blade's lean follows the tip's TRAVEL, and the tip arrives at the
 *     windup moving AWAY from the target, where the settled pose leaned it
 *     along the cut that follows — the blade on the other side of the arm;
 *   - the arm, wrist and lean are eased and still in motion on the last tick.
 * So the held frame is now the program itself, run from a fresh take-over
 * through the last tick of frame k and stopped there: by construction the
 * pose a solo shows when that frame ends. Jitter is the caller's `seed`, the
 * same draw the preview's own swing uses, so reroll moves both together.
 *
 * `m` must already carry the arm to take over from (setStroke) and the
 * current tuning; it is RESET here. Returns the cursor, or null.
 */
export function runStrokeToFrame(sty, styleIndex, seed, m, key, aimAz, aimEl,
                                 dt, right, up, fwd, aimDist = 0) {
  if (!sty || !m || !sty.frames || typeof key !== 'string' || key[0] !== 'f') return null;
  // 'f<k>' = the end of frame k; 'f<k>@<t>' = after tick t of it (the bake's
  // mid-cut sample, attacks.js bakeStyle).
  const at = key.indexOf('@');
  const k = +key.slice(1, at < 0 ? undefined : at);
  const stopTick = at < 0 ? Infinity : +key.slice(at + 1);
  if (!Number.isInteger(k) || k < 0 || k >= sty.frames.length) return null;
  m.reset();
  const cur = newStrokeCursor();
  beginStrokeProgram(cur, sty, styleIndex, seed);
  if (k >= cur.frames) return null;
  // Every frame is at least one tick, so the guard is only against a program
  // that never advances.
  for (let guard = 0; guard < 4096; guard++) {
    const r = stepStrokeProgram(cur, sty, m, aimAz, aimEl, dt, right, up, fwd, aimDist);
    if (r !== STEP.Live || cur.releasing || cur.frame > k) break;
    if (cur.frame === k && cur.frameTick >= stopTick) break;
  }
  return cur;
}

/* ============================================================================
   THE STYLE LIBRARY — strokes.cpp:20-160 LoadAttackStyles + the flick compass.

   The loader's convention is the whole file's: a bad entry is skipped LOUDLY
   into `log` and is never fatal, and an unknown key is ignored so a newer
   authored file still loads on an older binary. The editor keeps the RAW json
   object alongside the parsed library, because it is what gets written back
   and it must preserve keys this port has never heard of.
   ========================================================================== */

// strokes.cpp ReadEaseKey — a SEGMENT'S own pacing (strokes.h "PER-SEGMENT
// PACING"). Absent leaves the segment unpaced (null); an unknown name is
// reported and ignored rather than read as linear.
function readEaseKey(j, where, log) {
  if (!j || typeof j !== 'object' || typeof j.ease !== 'string') return null;
  if (AN.EASES.includes(j.ease)) return j.ease;
  if (log) log.push(`${where} has unknown ease "${j.ease}" - ignored`);
  return null;
}

// strokes.cpp:20 ReadSegment. `ease` is null when the segment states none:
// a cut leg then uses the style's ease, a windup keeps its closed-loop chase.
function readSegment(j, dflt, where, log) {
  const n = (k, d) => (j && Number.isFinite(+j[k]) ? +j[k] : d);
  return {
    ticks: Math.max(1, Math.round(n('ticks', dflt.ticks))),
    az: n('az', dflt.az),
    el: n('el', dflt.el),
    reach: n('reach', dflt.reach),
    ease: readEaseKey(j, where, log),
  };
}

// strokes.cpp ReadJoints — the per-joint brakes on the posed arm (melee.h
// ArmSmooth). Field-wise onto `base`, so the player block merges the same way.
export const ARM_JOINTS = ['shoulder', 'elbow', 'wrist'];
export function readJoints(j, base) {
  const out = {};
  for (const k of ARM_JOINTS) {
    const b = (base && base[k]) || { smooth: 0, maxDeg: 0 };
    const q = j && typeof j === 'object' ? j[k] : null;
    const n = (v, d) => (Number.isFinite(+v) ? +v : d);
    out[k] = q && typeof q === 'object'
      ? { smooth: clamp(n(q.smooth, b.smooth), 0, 60),
          maxDeg: clamp(n(q.maxDeg, b.maxDeg), 0, 180) }
      : { smooth: b.smooth, maxDeg: b.maxDeg };
  }
  return out;
}
export const jointsAny = (js) => !!js && ARM_JOINTS.some(k =>
  js[k] && (js[k].smooth > 0 || js[k].maxDeg > 0));

// strokes.h AttackStyle::LegEase — the leg's own ease, else the style's.
export const legEase = (sty, k) => (sty.cut[k] && sty.cut[k].ease) || sty.ease || 'linear';

/* ---- HOW A CUT'S TRAVEL IS PACED (strokes.h, above AttackStyle) ----------
 *
 * THE VOCABULARY IS anim.js's, not a second one: a cut's pacing and a clip
 * keyframe's interpolation are the same concept, so they are the same eight
 * names and the same `applyEase`. `AN.EASES` is the list; a style says
 * `"ease": "quadOut"` exactly as a keyframe does.
 *
 * `strokeEaseStep` is the share of the REMAINING gap one tick spends, which is
 * what keeps each leg's target an ABSOLUTE point on the path. The last tick
 * always spends everything, because `Ease` contains curves that never reach 1
 * (`instant`) and a cut has to arrive.
 *
 * linear IS the historical `gap / ticksLeft` divisor (the identity is derived
 * in strokes.h), and stepStrokeProgram below spells that case out longhand for
 * the same reason the engine does.
 * ------------------------------------------------------------------------ */

// Re-exported so the Attacks panel does not need its own anim.js import just
// to fill a dropdown: attacks.js talks to the driver through this module.
export const EASES = AN.EASES;

/**
 * melee.cpp AxisFrame -- the reference an angle about an arm axis is measured
 * from: `down` and `side` across `axis`, 0 = down, + toward the weapon side.
 * "Down" CARRIED from B (behind the body and across it, where no arm points)
 * to the axis by the shortest rotation, so its one degenerate point is B and
 * not the vertical the old projected-down broke at (the arm hanging at rest,
 * a hand dropped after a cut). A level axis gets exactly the old down.
 */
export function axisFrame(axis, handSign) {
  const k = Math.SQRT1_2;
  const b = v3(handSign >= 0 ? -k : k, 0, -k);
  const d0 = v3(0, -1, 0);
  const c = vdot(b, axis);
  let down;
  if (c > -0.9999) {
    const w = vcross(b, axis);
    down = vadd(vadd(vmul(d0, c), vcross(w, d0)), vmul(w, vdot(w, d0) / (1 + c)));
  } else {
    down = d0;
  }
  down = vsub(down, vmul(axis, vdot(axis, down)));
  down = vlen(down) > 1e-6 ? vnorm(down) : anyPerp(axis);
  const side = vmul(vcross(axis, down), handSign >= 0 ? 1 : -1);
  return { down, side };
}

/** An angle about an axis, brought into [-pi, pi] (strokes.cpp WrapPi). */
export function wrapPi(a) {
  if (!Number.isFinite(a)) return 0;
  const t = 2 * Math.PI;
  return a - t * Math.round(a / t);
}

export function strokeEaseStep(ease, p0, p1) {
  if (p1 >= 1) return 1;
  const f0 = AN.applyEase(ease, p0), f1 = AN.applyEase(ease, p1);
  const room = 1 - f0;
  if (room <= 1e-6) return 1;
  return clamp((f1 - f0) / room, 0, 1);
}

/**
 * strokes.cpp ReadCutPath — `cut` is EITHER one segment or a list of legs,
 * read by the one function so the single-segment form cannot drift into a
 * special case. Returns { legs, aimLeg }; `legs` is never empty.
 *
 * A leg of the list defaults to a SHORT, STATIONARY segment rather than to the
 * single cut's whole horizontal slash: "{ az: -0.4 }" as a third leg means
 * "and then a little further", not "and then a second swing".
 */
function readCutPath(cutJ, styleName, log) {
  const legs = [];
  let aimLeg = -1;
  if (Array.isArray(cutJ)) {
    for (const leg of cutJ) {
      if (!leg || typeof leg !== 'object') {
        log.push(`style "${styleName}" has a cut leg that is not an object `
                 + '- skipped');
        continue;
      }
      if (legs.length >= MAX_CUT_LEGS) {
        log.push(`style "${styleName}" has more than ${MAX_CUT_LEGS} cut legs `
                 + '- the rest are dropped (a path with that many corners is a '
                 + 'body clip, not a stroke)');
        break;
      }
      if (leg.aim === true) {
        if (aimLeg >= 0)
          log.push(`style "${styleName}" marks leg ${legs.length + 1} with `
                   + '"aim" as well - the first marked leg keeps it');
        else aimLeg = legs.length;
      }
      legs.push(readSegment(leg, { ticks: 4, az: 0, el: 0, reach: 0 },
                            `style "${styleName}" cut leg ${legs.length + 1}`, log));
    }
    if (!legs.length)
      log.push(`style "${styleName}" has an empty cut list - falling back to `
               + 'one default cut');
  }
  if (!legs.length)
    legs.push(readSegment(cutJ && typeof cutJ === 'object' && !Array.isArray(cutJ)
      ? cutJ : {}, { ticks: 7, az: 0, el: 0, reach: 0 },
      `style "${styleName}" cut`, log));
  return { legs, aimLeg };
}

/**
 * strokes.cpp the `recover` block (strokes.h StrokeRecover).
 *
 * `posed` is DERIVED, not authored: stating an az, an el or a reach is what
 * asks for a driven return, so there is no second switch to forget. A recover
 * of `{ ticks: 10 }` is the pre-2026-09-21 hand-back, byte for byte.
 */
function readRecover(j, styleName, log) {
  const out = { ticks: 10, posed: false, az: 0, el: 0, reach: 0,
                settle: 0, fade: 0, ease: null };
  if (!j || typeof j !== 'object') return out;
  const n = (k, d) => (Number.isFinite(+j[k]) ? +j[k] : d);
  out.ticks = Math.max(1, Math.round(n('ticks', 10)));
  out.posed = j.az !== undefined || j.el !== undefined || j.reach !== undefined;
  out.az = n('az', 0);
  out.el = n('el', 0);
  out.reach = n('reach', 0);
  out.settle = Math.max(0, Math.round(n('settle', 0)));
  out.fade = Math.max(0, Math.round(n('fade', 0)));
  out.ease = readEaseKey(j, `style "${styleName}" recover`, log);
  if (out.settle > 0 && !out.posed) {
    log.push(`style "${styleName}" has \`recover.settle\` but no return pose `
             + '(az/el/reach) - the arm has nowhere to be driven, so the '
             + 'settle is ignored');
    out.settle = 0;
  }
  if (out.settle > out.ticks) {
    log.push(`style "${styleName}" settles for ${out.settle} of ${out.ticks} `
             + 'recover ticks - clamped to the segment');
    out.settle = out.ticks;
  }
  // ...AND THE FADE HAS TO FIT IN WHAT IS LEFT, or the claim is dropped
  // mid-fade and poseWeight steps to 0 in one tick - the very snap this
  // block exists to remove, arrived at from the content side.
  if (out.fade > 0 && out.settle + out.fade > out.ticks) {
    const want = out.settle + out.fade;
    log.push(`style "${styleName}" settles ${out.settle} + fades ${out.fade} `
             + `= ${want} ticks in a ${out.ticks}-tick recover - extended to `
             + `${want} so the claim is not dropped mid-fade`);
    out.ticks = want;
  }
  return out;
}

// strokes.cpp:80 the `lunge` block (strokes.h StyleLunge). m/s, converted to
// cells by the engine at the moment of the launch; `at` names the PHASE whose
// start fires it, and only "windup" or "cut" can — a leap during the recover
// is a creature throwing itself at somebody after the blow landed.
function readLunge(j, log, styleName) {
  const out = { ticks: 0, speed: 0, rise: 0, at: 'windup' };
  if (!j || typeof j !== 'object') return out;
  out.ticks = Math.max(0, Math.round(Number.isFinite(+j.ticks) ? +j.ticks : 0));
  out.speed = Math.max(0, Number.isFinite(+j.speed) ? +j.speed : 0);
  out.rise = Math.max(0, Number.isFinite(+j.rise) ? +j.rise : 0);
  out.at = j.at === 'cut' ? 'cut' : 'windup';
  if (j.at !== undefined && j.at !== 'cut' && j.at !== 'windup')
    log.push(`style "${styleName}" lunges at "${j.at}", which is not `
             + '"windup" or "cut" - using "windup"');
  if (out.ticks > 0 && out.speed <= 0 && out.rise <= 0)
    log.push(`style "${styleName}" has a lunge with no speed and no rise `
             + '- it will not leap');
  return out;
}

// strokes.h StyleLunge::Any
export const lungeAny = (l) => !!l && l.ticks > 0 && (l.speed > 0 || l.rise > 0);

/* ============================================================================
   FRAMES — strokes.h "AN ATTACK IS A LIST OF FRAMES". Mirrors strokes.cpp's
   ReadFrame / ValidateFrames / BuildFramesFromLegacy / DeriveLegacyViews.
   ========================================================================== */
export const MAX_FRAMES = 12;
export const LEANS = ['follow', 'hold', 'left', 'right', 'angle'];
const LEAN_CODE = { follow: 0, hold: 1, left: 2, right: 3, angle: 4 };
export const firstCut = (sty) => (sty.frames || []).findIndex(f => f.cuts);
export const lastCut = (sty) => {
  const fr = sty.frames || [];
  for (let k = fr.length - 1; k >= 0; k--) if (fr[k].cuts) return k;
  return -1;
};

export function readFrame(j, styleJoints, where, log) {
  const num = (v, d) => (Number.isFinite(+v) ? +v : d);
  const o = j && typeof j === 'object' ? j : {};
  const from = o.from === undefined ? 'target' : o.from;
  if (from !== 'target' && from !== 'body')
    log.push(`${where} has unknown "from": "${from}" — measuring from the target`);
  let lean = o.lean === undefined ? 'follow' : o.lean;
  if (!LEANS.includes(lean)) {
    log.push(`${where} has unknown "lean": "${lean}" — following the travel`);
    lean = 'follow';
  }
  return {
    name: typeof o.name === 'string' ? o.name : '',
    ticks: Math.max(1, Math.round(num(o.ticks, 8))),
    az: num(o.az, 0), el: num(o.el, 0), reach: num(o.reach, 0),
    fromBody: from === 'body',
    ease: o.ease === 'chase' ? 'chase' : (readEaseKey(o, where, log) || 'linear'),
    chaseRate: clamp(num(o.chaseRate, 16), 1, 200),
    cuts: o.cuts === true,
    lean,
    wristAlign: clamp(num(o.wrist, 1), 0, 1),
    bladeAngle: o.bladeAngle === undefined ? -1 : clamp(num(o.bladeAngle, 0), 0, Math.PI),
    elbowSet: o.elbow !== undefined,
    // Angles ABOUT AN AXIS, so any number is one of these (strokes.cpp).
    elbow: wrapPi(num(o.elbow, 0)),
    leanAngle: wrapPi(num(o.leanAngle, 0)),
    torsoTwist: o.torsoTwist === undefined ? -1 : clamp(num(o.torsoTwist, 0), 0, 1),
    torsoPitch: o.torsoPitch === undefined ? -1 : clamp(num(o.torsoPitch, 0), 0, 1),
    joints: readJoints(o.joints, styleJoints),
    pose: readPose(o.pose),
    raw: o,
  };
}

function readFrames(arr, styleJoints, name, log) {
  const out = [];
  for (const fj of arr) {
    if (out.length >= MAX_FRAMES) {
      log.push(`style "${name}" has more than ${MAX_FRAMES} frames — the rest are dropped`);
      break;
    }
    out.push(readFrame(fj, styleJoints, `style "${name}" frame ${out.length + 1}`, log));
  }
  return out;
}

function validateFrames(st, log) {
  const fr = st.frames;
  if (!fr.length) return;
  const fc = firstCut(st), lc = lastCut(st);
  if (fc < 0) {
    log.push(`style "${st.name}" has no frame with "cuts": true — its last `
             + 'frame is made the cut so it can hurt anything');
    fr[fr.length - 1].cuts = true;
    return;
  }
  for (let k = fc; k <= lc; k++)
    if (!fr[k].cuts) {
      log.push(`style "${st.name}" frame ${k + 1} sits between two cutting `
               + 'frames — it cuts too (the cut is one run)');
      fr[k].cuts = true;
    }
}

// strokes.cpp AttackStyle::BuildFramesFromLegacy (the style is in the TARGET
// stroke frame already).
export function buildFramesFromLegacy(st) {
  const fr = [];
  const base = { lean: 'follow', wristAlign: 1, torsoTwist: -1, torsoPitch: -1,
                 fromBody: false, cuts: false, bladeAngle: -1, elbowSet: false, elbow: 0 };
  fr.push({ ...base, name: 'windup', ticks: st.windup.ticks, az: st.windup.az,
            el: st.windup.el, reach: st.windup.reach,
            // An unpaced windup WAS the capped chase: kept, visibly.
            ease: st.windup.ease || 'chase', chaseRate: 16,
            joints: readJoints(null, st.joints) });
  st.cut.forEach((leg, k) => {
    if (fr.length >= MAX_FRAMES) return;
    const t = cutThrough(st, k);
    fr.push({ ...base, name: st.cut.length > 1 ? `cut ${k + 1}` : 'cut',
              ticks: leg.ticks, az: st.windup.az + t.az, el: st.windup.el + t.el,
              reach: st.windup.reach + t.reach, ease: legEase(st, k), chaseRate: 16, cuts: true,
              joints: readJoints(null, st.joints) });
  });
  const settle = st.recover.posed ? Math.max(0, st.recover.settle) : 0;
  if (settle > 0 && fr.length < MAX_FRAMES)
    fr.push({ ...base, name: 'recover', ticks: settle, az: st.recover.az,
              el: st.recover.el, reach: st.recover.reach, fromBody: true,
              ease: st.recover.ease || 'chase', chaseRate: 16, lean: 'hold',
              joints: readJoints(null, st.joints) });
  st.frames = fr;
  st.release = st.recover.fade > 0 ? st.recover.fade
    : Math.max(1, st.recover.ticks - settle);
}

// strokes.cpp AttackStyle::DeriveLegacyViews.
export function deriveLegacyViews(st) {
  const fr = st.frames, fc = firstCut(st), lc = lastCut(st);
  if (!fr.length || fc < 0) return;
  let pre = 0;
  for (let k = 0; k < fc; k++) pre += fr[k].ticks;
  const w = fc > 0 ? fr[fc - 1] : fr[fc];
  st.windup = { ticks: Math.max(1, pre), az: w.az, el: w.el, reach: w.reach,
                ease: w.ease === 'chase' ? null : w.ease };
  st.cut = [];
  let pa = w.az, pe = w.el, pr = w.reach;
  for (let k = fc; k <= lc && st.cut.length < MAX_CUT_LEGS; k++) {
    const f = fr[k];
    st.cut.push({ ticks: f.ticks, az: f.az - pa, el: f.el - pe, reach: f.reach - pr, ease: f.ease });
    pa = f.az; pe = f.el; pr = f.reach;
  }
  st.aimLeg = -1;
  st.ease = 'linear';
  let post = 0;
  for (let k = lc + 1; k < fr.length; k++) post += fr[k].ticks;
  const posed = lc + 1 < fr.length;
  const r = fr[fr.length - 1];
  st.recover = { ticks: post + Math.max(1, st.release), posed,
                 az: posed ? r.az : 0, el: posed ? r.el : 0, reach: posed ? r.reach : 0,
                 settle: post, fade: st.release, ease: posed ? r.ease : null };
  st.joints = fr[fc].joints;
}

/** One parsed frame back to its JSON spelling (defaults omitted). */
export function frameToJson(f) {
  const r6 = v => Math.round(v * 1e6) / 1e6;
  // A POSE FRAME is the pose and its timing: the driver's steering fields
  // mean nothing to it and are not written.
  if (f.pose) {
    const o = { name: f.name || '', ticks: f.ticks };
    if (f.fromBody) o.from = 'body';
    if (f.ease && f.ease !== 'linear' && f.ease !== 'chase') o.ease = f.ease;
    if (f.cuts) o.cuts = true;
    if (jointsAny(f.joints)) {
      o.joints = {};
      for (const k of ARM_JOINTS)
        if (f.joints[k].smooth > 0 || f.joints[k].maxDeg > 0)
          o.joints[k] = { smooth: f.joints[k].smooth, maxDeg: f.joints[k].maxDeg };
    }
    o.pose = poseToJson(f.pose);
    return o;
  }
  const o = { name: f.name || '', ticks: f.ticks, az: r6(f.az), el: r6(f.el), reach: r6(f.reach) };
  if (f.fromBody) o.from = 'body';
  if (f.ease && f.ease !== 'linear') o.ease = f.ease;
  if (f.ease === 'chase' && f.chaseRate !== 16) o.chaseRate = f.chaseRate;
  if (f.cuts) o.cuts = true;
  if (f.lean && f.lean !== 'follow') o.lean = f.lean;
  if (f.wristAlign !== 1) o.wrist = r6(f.wristAlign);
  if (f.bladeAngle >= 0) o.bladeAngle = r6(f.bladeAngle);
  if (f.elbowSet) o.elbow = r6(f.elbow);
  if (f.lean === 'angle') o.leanAngle = r6(f.leanAngle || 0);
  if (f.torsoTwist >= 0) o.torsoTwist = r6(f.torsoTwist);
  if (f.torsoPitch >= 0) o.torsoPitch = r6(f.torsoPitch);
  if (jointsAny(f.joints)) {
    o.joints = {};
    for (const k of ARM_JOINTS)
      if (f.joints[k].smooth > 0 || f.joints[k].maxDeg > 0)
        o.joints[k] = { smooth: f.joints[k].smooth, maxDeg: f.joints[k].maxDeg };
  }
  return o;
}

/**
 * THE FILE, REWRITTEN INTO FRAMES (once; the editor calls it on load). Every
 * style and every player block that states motion is replaced by its frames
 * as the loader would build them, so an attack swings as it did — minus the
 * hidden windup cap, which no longer exists. Returns true if it changed
 * anything.
 */
export function migrateRawToFrames(json) {
  if (!json || !Array.isArray(json.styles)) return false;
  if (json.styles.every(s => !s || typeof s !== 'object' || Array.isArray(s.frames)))
    return false;
  const lib = parseStyleLibrary(json);
  const MOTION = ['windup', 'cut', 'recover', 'ease', 'joints'];
  for (const raw of json.styles) {
    if (!raw || typeof raw !== 'object' || Array.isArray(raw.frames)) continue;
    const base = lib.styles.find(s => s.name === raw.name && !s.derived);
    if (!base) continue;
    raw.frames = base.frames.map(frameToJson);
    raw.release = base.release;
    for (const k of MOTION) delete raw[k];
    const p = raw.player;
    if (p && typeof p === 'object' && MOTION.some(k => p[k] !== undefined)) {
      const d = lib.styles.find(s => s.name === raw.name + ':player');
      if (d) {
        p.frames = d.frames.map(frameToJson);
        p.release = d.release;
      }
      for (const k of MOTION) delete p[k];
    }
  }
  json.strokeFrame = 'target';
  return true;
}

// strokes.h kWeaponFormNames: the weapon classes a style may have a version
// for -- plus `unarmed`, the version an EMPTY hand throws with its fist.
export const WEAPON_FORMS = ['short', 'long', 'blunt', 'unarmed'];

/**
 * strokes.cpp:32 LoadAttackStyles. Returns { styles, player, playerUnarmed, log }.
 * `log` is the loader's own skip list, shown verbatim in the panel — the same
 * text the engine would print, so an author fixes it once.
 */
export function parseStyleLibrary(json) {
  const log = [];
  const styles = [];
  const arr = Array.isArray(json && json.styles) ? json.styles : [];
  for (const s of arr) {
    if (!s || typeof s !== 'object') continue;
    const name = typeof s.name === 'string' ? s.name : '';
    if (!name) { log.push('a style with no "name" — skipped'); continue; }
    if (styles.some(x => x.name === name)) {
      log.push(`duplicate style "${name}" — the second is skipped`);
      continue;
    }
    const jt = s.jitter || {};
    const cutPath = readCutPath(s.cut, name, log);
    // strokes.cpp: ParseEase is silent on an unknown name (right for a
    // keyframe, wrong for a style), so the typo is reported here.
    let ease = 'linear';
    if (s.ease !== undefined) {
      if (AN.EASES.includes(s.ease)) ease = s.ease;
      else log.push(`style "${name}" has unknown ease "${s.ease}" — using `
                    + 'linear (the names are anim.h\'s: ' + AN.EASES.join(', ')
                    + ')');
    }
    styles.push({
      name,
      label: typeof s.label === 'string' ? s.label : name,
      // How the cut's travel is paced over each leg (strokes.h StrokeEase).
      ease,
      windup: readSegment(s.windup, { ticks: 12, az: 0, el: 0, reach: 0 },
                          `style "${name}" windup`, log),
      // THE CUT PATH: always a LIST, one leg or several (strokes.h "A CUT IS
      // A PATH"). Readers that want the old single segment want cutTravel().
      cut: cutPath.legs,
      aimLeg: cutPath.aimLeg,
      recover: readRecover(s.recover, name, log),
      // Per-joint brakes on the posed arm (strokes.h AttackStyle::joints).
      joints: readJoints(s.joints, null),
      // THE TRUTH (strokes.h): the frame list and the release. Filled below
      // for the pre-frames spelling.
      frames: Array.isArray(s.frames)
        ? readFrames(s.frames, readJoints(s.joints, null), name, log) : null,
      release: Math.max(1, Math.round(Number.isFinite(+s.release) ? +s.release : 6)),
      jitter: {
        az: Number.isFinite(+jt.az) ? +jt.az : 0,
        el: Number.isFinite(+jt.el) ? +jt.el : 0,
        tempo: Number.isFinite(+jt.tempo) ? +jt.tempo : 0,
      },
      // strokes.h AttackStyle::clip — the body animation played WITH the
      // stroke, by library name (assets/anims/<name>.json). '' = none.
      clip: typeof s.clip === 'string' ? s.clip : '',
      // ---- strokes.cpp:78 the unarmed schema (PLAN_impact_unarmed §4/§5) ---
      // Every default here is the behaviour a style authored before these
      // existed already had: the held weapon, never a fallback, the profile's
      // reach, no leap, the chest. A file written against them loads on an
      // older binary too, because the C++ loader ignores unknown keys.
      weapon: typeof s.weapon === 'string' && s.weapon ? s.weapon : 'held',
      fallback: s.fallback === true,
      reach: Math.max(0, Number.isFinite(+s.reach) ? +s.reach : 0),
      lunge: readLunge(s.lunge, log, name),
      // Weights over LIMB TAGS. SORTED BY TAG, exactly as strokes.cpp sorts
      // them: the draw walks this list, and JSON object order is the
      // library's business rather than the author's — two files spelling the
      // same weights in a different order must pick the same limb from the
      // same (mob, tick). That is CLAUDE.md rule 1 applied to content.
      target: (s.target && typeof s.target === 'object'
        ? Object.entries(s.target)
            .filter(([, w]) => Number.isFinite(+w) && +w > 0)
            .map(([tag, w]) => ({ tag, weight: +w }))
            .sort((a, b) => (a.tag < b.tag ? -1 : a.tag > b.tag ? 1 : 0))
        : []),
      raw: s,                      // the object the editor mutates in place
    });
  }
  // ---- PLAYER OVERRIDES: create derived copies for styles with a `player`
  // block. The derived entry carries the merged numbers and is what the player
  // compass points at; the base entry is what NPCs use. Merge rule: per-field
  // within windup/cut/jitter (unstated fields inherit from base). For recover:
  // if the override is present but states no az/el/reach, the result has no
  // posed return even if the base did.
  const playerDerived = new Map();
  const baseCount = styles.length;
  const n = (v, d) => (Number.isFinite(+v) ? +v : d);
  for (let bi = 0; bi < baseCount; bi++) {
    const s = styles[bi].raw;
    if (!s.player || typeof s.player !== 'object') continue;
    const p = s.player;
    const base = styles[bi];
    const mergeSeg = (baseSeg, over) => {
      if (!over || typeof over !== 'object') return { ...baseSeg };
      return {
        ticks: Math.max(1, n(over.ticks, baseSeg.ticks)),
        az: n(over.az, baseSeg.az),
        el: n(over.el, baseSeg.el),
        reach: n(over.reach, baseSeg.reach),
        ease: readEaseKey(over, `style "${base.name}:player" segment`, log)
          || baseSeg.ease || null,
      };
    };
    const mergeJitter = (baseJ, over) => {
      if (!over || typeof over !== 'object') return { ...baseJ };
      return {
        az: n(over.az, baseJ.az),
        el: n(over.el, baseJ.el),
        tempo: n(over.tempo, baseJ.tempo),
      };
    };
    const mergeRecover = (baseR, over) => {
      if (!over || typeof over !== 'object') return { ...baseR };
      const posed = over.az !== undefined || over.el !== undefined ||
                    over.reach !== undefined;
      return {
        ticks: Math.max(1, n(over.ticks, baseR.ticks)),
        posed,
        az: posed ? n(over.az, 0) : 0,
        el: posed ? n(over.el, 0) : 0,
        reach: posed ? n(over.reach, 0) : 0,
        settle: posed ? Math.max(0, n(over.settle, baseR.settle)) : 0,
        fade: Math.max(0, n(over.fade, baseR.fade)),
        ease: readEaseKey(over, `style "${base.name}:player" recover`, log)
          || baseR.ease || null,
      };
    };
    // A LIST REPLACES, AN OBJECT MERGES INTO THE FIRST LEG (strokes.cpp says
    // why: merging list into list per index would reinterpret leg 2 of one
    // path as leg 2 of a differently-shaped one).
    let cut = base.cut.map(l => ({ ...l }));
    let aimLeg = base.aimLeg;
    if (Array.isArray(p.cut)) {
      const pp = readCutPath(p.cut, base.name + ':player', log);
      cut = pp.legs;
      aimLeg = pp.aimLeg;
    } else if (p.cut && typeof p.cut === 'object') {
      cut[0] = mergeSeg(cut[0], p.cut);
    }
    // The player's copy may pace the cut differently from the NPC's — a click
    // has to feel owned where an authored telegraph does not. Unstated
    // inherits, via the spread below.
    let pEase = base.ease;
    if (p.ease !== undefined) {
      if (AN.EASES.includes(p.ease)) pEase = p.ease;
      else log.push(`style "${base.name}:player" has unknown ease `
                    + `"${p.ease}" — inheriting the base's`);
    }
    const derived = {
      ...base,
      name: base.name + ':player',
      ease: pEase,
      windup: mergeSeg(base.windup, p.windup),
      cut,
      aimLeg,
      recover: mergeRecover(base.recover, p.recover),
      joints: readJoints(p.joints, base.joints),
      jitter: mergeJitter(base.jitter, p.jitter),
      // A player block in the frames spelling REPLACES the list.
      frames: Array.isArray(p.frames)
        ? readFrames(p.frames, readJoints(p.joints, base.joints), base.name + ':player', log)
        : (base.frames ? base.frames.map(f => ({ ...f, joints: { ...f.joints } })) : null),
      release: Number.isFinite(+p.release) ? Math.max(1, Math.round(+p.release)) : base.release,
      derived: true,
      baseName: base.name,
      baseIndex: bi,
      raw: s,
    };
    playerDerived.set(base.name, styles.length);
    styles.push(derived);
  }

  // ---- WEAPON FORMS (strokes.h / strokes.cpp): one copy per (style, form).
  // The engine makes copies only for AUTHORED forms and falls back to the
  // style for the rest; the editor makes one for EVERY form of every held-
  // weapon style (`formInherited` when unauthored) so each weapon's version
  // is always selectable, and the first edit writes it (attacks.js
  // framesDoc). Same frames either way, so the preview is the game's swing.
  for (let bi = 0; bi < baseCount; bi++) {
    const base = styles[bi];
    if (base.weapon !== 'held') continue;
    const fo = base.raw.forms && typeof base.raw.forms === 'object' ? base.raw.forms : {};
    for (const form of WEAPON_FORMS) {
      const fj = fo[form] && typeof fo[form] === 'object' ? fo[form] : null;
      const own = !!(fj && Array.isArray(fj.frames));
      styles.push({
        ...base,
        name: base.name + '@' + form,
        frames: own ? readFrames(fj.frames, base.joints, base.name + '@' + form, log)
          : (base.frames ? base.frames.map(f => ({ ...f, joints: { ...f.joints } })) : null),
        release: fj && Number.isFinite(+fj.release) ? Math.max(1, Math.round(+fj.release))
          : base.release,
        derived: true,
        form,
        formInherited: !own,
        baseName: base.name,
        baseIndex: bi,
        raw: base.raw,
      });
    }
  }

  // THE STROKE FRAME (strokes.h): a file without `strokeFrame: "target"` is
  // converted AFTER the player merge, over base and player copies alike,
  // exactly where strokes.cpp does it.
  if (!json || json.strokeFrame !== 'target')
    for (const st of styles) if (!st.frames) fromLegacyFrame(st);
  // EVERY STYLE RUNS ON FRAMES (strokes.cpp, the same place).
  for (const st of styles) {
    if (!st.frames) buildFramesFromLegacy(st);
    else { validateFrames(st, log); deriveLegacyViews(st); }
  }

  // strokes.h:111 PlayerStrikeMap — INDICES, not names, resolved against the
  // library at load time; a sector naming an unknown style is skipped LOUDLY.
  const find = n => styles.findIndex(s => s.name === n);
  const findPlayer = n => {
    const di = playerDerived.get(n);
    return di !== undefined ? di : find(n);
  };
  const readMap = (key) => {
    const map = { sectors: [], neutral: [-1, -1] };
    const pj = json && json[key];
    if (!pj || typeof pj !== 'object') return map;
    for (const sec of (Array.isArray(pj.sectors) ? pj.sectors : [])) {
      if (!sec || !Array.isArray(sec.dir) || sec.dir.length !== 2) {
        log.push(`${key}.sectors: an entry with no 2-element "dir" — skipped`);
        continue;
      }
      const i = findPlayer(sec.style);
      if (i < 0) {
        log.push(`${key}.sectors: unknown style "${sec.style}" — skipped`);
        continue;
      }
      map.sectors.push({ x: +sec.dir[0] || 0, y: +sec.dir[1] || 0, style: i, raw: sec });
    }
    const na = Array.isArray(pj.neutralAlternate) ? pj.neutralAlternate : [];
    for (let k = 0; k < 2; k++) {
      if (na[k] === undefined) continue;
      const i = findPlayer(na[k]);
      if (i < 0) log.push(`${key}.neutralAlternate: unknown style "${na[k]}"`);
      else map.neutral[k] = i;
    }
    return map;
  };
  const player = readMap('player');
  const playerUnarmed = readMap('playerUnarmed');
  return { styles, player, playerUnarmed, log, raw: json };
}

// strokes.h:118 PlayerStrikeMap::Usable. Takes a LIBRARY (meaning its armed
// map) or a bare map, which is the one place this port deliberately differs
// from the C++: there the signature moved to the map because the caller always
// knows which fist is empty, and here the existing callers (attacks.js, the
// data gate) pass a library and have no fists to speak of.
const mapOf = (x) => (x && x.player ? x.player : x);
export const playerMapUsable = lib =>
  !!lib && (mapOf(lib).sectors.length > 0 || mapOf(lib).neutral[0] >= 0);

/**
 * strokes.cpp:127 QuantizeStrike — the flick (screen space, +y down) -> a
 * style index by MAX DOT over the sectors; -1 when the map has none.
 *
 * Adding a sector is adding a line in the JSON, which is the whole point of
 * the compass being data.
 */
export function quantizeStrike(lib, dx, dy) {
  const map = mapOf(lib);
  if (!map || !map.sectors.length) return -1;
  const len = Math.sqrt(dx * dx + dy * dy);
  if (len < 1e-6) return -1;
  const nx = dx / len, ny = dy / len;
  let best = -1, bestDot = -2;
  for (const s of map.sectors) {
    const sl = Math.sqrt(s.x * s.x + s.y * s.y);
    if (sl < 1e-6) continue;
    const d = (nx * s.x + ny * s.y) / sl;
    if (d > bestDot) { bestDot = d; best = s.style; }
  }
  return best;
}

// strokes.cpp:142 NeutralStrike — the directionless click alternates the two
// neutral entries; returns the other one when the asked-for side is unresolved.
export function neutralStrike(lib, right) {
  const map = mapOf(lib);
  if (!map) return -1;
  const n = map.neutral;
  const want = right ? 0 : 1;
  if (n[want] >= 0) return n[want];
  return n[want ^ 1] >= 0 ? n[want ^ 1] : -1;
}

/**
 * strokes.cpp:302 PickAttackStyle — RESOLVE BY NAME, THEN FILTER, THEN PICK,
 * and the order is load-bearing at every step (the C++ carries the argument
 * in full):
 *
 *   - RESOLVE FIRST, so a profile listing one unknown style among four still
 *     varies over the other three instead of stuttering on the hole.
 *   - FILTER BEFORE DRAWING, so a disarmed duelist does not miss three turns
 *     in four rolling its way onto its one punch.
 *   - FALLBACKS LAST AND AS A GROUP: a style is not "worse", it is "only when
 *     there is nothing better", so the test is whether ANY non-fallback style
 *     survived — which is what makes an armed duelist's punches invisible and
 *     a disarmed one's the whole of its repertoire.
 *
 * `usable` is the port of `StyleUsable(const Mob&, const AttackStyle&)`, which
 * cannot come across as-is: it asks a RIG whether a natural weapon's part is
 * still attached, and this file has no rig. It arrives as a PREDICATE instead,
 * so the preview can answer it from the open sidecar's own `natural` block;
 * omitting it means "everything is usable", which is what a caller with no
 * creature in front of it wants.
 */
export function pickAttackStyle(lib, names, mobId, tick, usable) {
  if (!lib || !lib.styles.length) return -1;
  let found = [];
  let haveReal = false;
  for (const s of names || []) {
    if (found.length >= 8) break;
    const i = lib.styles.findIndex(x => x.name === s);
    if (i < 0) continue;
    if (usable && !usable(lib.styles[i])) continue;
    found.push(i);
    haveReal = haveReal || !lib.styles[i].fallback;
  }
  if (haveReal) found = found.filter(i => !lib.styles[i].fallback);
  if (!found.length) return -1;
  if (found.length === 1) return found[0];
  const h = hash3(((mobId >>> 0) ^ kSaltStyle) >>> 0, tick >>> 0, 0);
  return found[h % found.length];
}

/* ============================================================================
   CITATIONS — the C++ this file tracks. Re-check on every melee.cpp change.

     SmoothAlpha / Lerp / ToWorld / ToBasis / AnyPerp   melee.cpp:486-512
     ApplyMeleeTuning                                   melee.cpp:760-808
     MeleeState::Feed / FeedReach / Step                melee.cpp:829-841
     MeleeState::SetStroke / ClearArm                   melee.cpp:843-862
     MeleeState::PoseWeight / Pose                      melee.cpp:864-919
     MeleeState::Arrest / Reset                         melee.cpp:921-972
     MeleeState::RadiusBand                             melee.cpp:974-1001
     MeleeState::RebuildFrame                           melee.cpp:1003-1380
     MeleeState::Update                                 melee.cpp:1382-1684
     MeleeTuning defaults                               melee.h:160-420
     StrokeReachIn / BeginStrokeProgram                 strokes.cpp:170-188
     StepStrokeProgram                                  strokes.cpp:190-300
     LoadAttackStyles / QuantizeStrike / NeutralStrike  strokes.cpp:20-160
     PickAttackStyle (+ the usable/fallback filter)     strokes.cpp:302-340
     Hash3 / SignedUnit / Pcg                           sim/rng.h:22-36

   NOT PORTED, and each is a deliberate line rather than an omission:

     MeleeSweepDamage's IMPULSE RULE and PROBE GEOMETRY (melee.cpp, 2026-09-15)
       blunt and bite land ONCE per body per stroke (EdgeSweep::struck) where
       the kerf continues every tick, and a SELF-MOUNTED edge -- a fist, a set
       of jaws -- probes along its TRAVEL with the wielder's own bodies
       excluded from the cast, because its edge is one of them. Both live
       where the sweep does, and the panel reports speed and edge alignment
       rather than damage, so neither reaches this file. The Attacks lane
       showing "lands at tick N vs cut starts at M" is the part an author acts
       on.
     MobSystem::StyleReachOn (mob.cpp)  a natural style's `reach` of 0 means
       ASK THE BODY: the effector's own reach plus what the lunge closes. It
       needs a live rig, so the lane shows the authored 0 as "derived" rather
       than inventing a number here.
     MeleeSweepDamage / FindParry (melee.cpp)   need a World, a Jolt scene and
       the debris system. The panel therefore reports the blade's SPEED and
       EDGE ALIGNMENT — the two inputs the damage formula scales by — and says
       nothing about damage numbers, rather than inventing a second formula.
     StyleUsable (mob.cpp)  asks a RIG whether a natural weapon's part is
       still attached, which this file has no way to know. pickAttackStyle
       takes the answer as a PREDICATE instead, so the preview supplies the
       open sidecar's own `natural` block and the law stays in one place.
     Mob::Launch / UpdateFall's airVel_ integration (mob.cpp)  a lunge needs
       the world's footing probes and another mob counted as a wall; the
       Attacks lane shows the flight as a TIMELINE (lands at tick N vs cut
       starts at M), which is the part an author can act on, rather than a
       second ballistics model that would drift.
     Mob::ApplyWeaponArm's hinge-axis override (mob.cpp:7850)  the editor's
       clamp port (anim.js animClampPoseLimits) accepts overrides; rig.js
       supplies the steered plane the same way mob.cpp does.
   ========================================================================== */
