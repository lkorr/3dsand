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
                  (take-over seed, phase machine, stroke integration, the
                  slash arc), Pose/PoseWeight, StrokeReachIn,
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
    slashTime: 0.17,                                       // :163
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
    handLead: 1.0,                                         // :249
    fallbackReach: metresToCells(0.60),                    // :253
    reachFraction: 0.94,                                   // :260
    guardForward: metresToCells(0.22),                     // :264
    guardUp: metresToCells(0.26),
    guardSide: metresToCells(0.16),
    dirSmoothing: 0.06,                                    // :268
    swingArc: 2.0,                                         // :274
    swingAnticipate: 0.35,                                 // :281
    swingExtend: 0.16,                                     // :286
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
 * `controlMode`/`pickMinSpeed`/`aimYaw`/`aimReleaseYaw` are DELIBERATELY not
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
    slashTime: n('slashTime', d.slashTime),
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
    handLead: n('handLead', d.handLead),
    fallbackReach: metresToCells(n('fallbackReachM', d.fallbackReach * kVoxelMeters)),
    reachFraction: n('reachFraction', d.reachFraction),
    guardForward: metresToCells(n('guardForwardM', d.guardForward * kVoxelMeters)),
    guardUp: metresToCells(n('guardUpM', d.guardUp * kVoxelMeters)),
    guardSide: metresToCells(n('guardSideM', d.guardSide * kVoxelMeters)),
    dirSmoothing: n('dirSmoothing', d.dirSmoothing),
    swingArc: n('swingArc', d.swingArc),
    swingAnticipate: n('swingAnticipate', d.swingAnticipate),
    swingExtend: n('swingExtend', d.swingExtend),
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

export const PHASE = { Idle: 0, Guard: 1, Wind: 2, Slash: 3, Recover: 4 };
export const PHASE_NAME = ['idle', 'guard', 'wind', 'slash', 'recover'];

export class MeleeState {
  constructor(tuning) {
    this.tuning = tuning || defaultMeleeTuning();
    // ---- the stroke the input integrates (melee.h) ----
    this.phase_ = PHASE.Idle;
    this.phaseTime_ = 0;
    this.inputAccum_ = v3();
    this.mouseVel_ = v3();
    this.mouseSpeed_ = 0;
    this.cutDir_ = v3();
    this.cutAz_ = 0; this.cutEl_ = 0;
    this.az_ = 0; this.el_ = 0; this.radius_ = 0;
    this.azLive_ = 0; this.elLive_ = 0; this.radLive_ = 0;
    this.swingAz_ = 0; this.swingEl_ = 0; this.swingOut_ = 0;
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

  // melee.cpp:864 PoseWeight
  poseWeight() {
    switch (this.phase_) {
      case PHASE.Idle: return 0.0;
      case PHASE.Recover: {
        // Only the RELEASING recover fades: a recover between two cuts is
        // still the player's arm.
        if (this.recoverHold_) return 1.0;
        const t = this.tuning.recoverTime > 1e-4
          ? this.phaseTime_ / this.tuning.recoverTime : 1.0;
        return clamp(1.0 - t, 0, 1);
      }
      default: return 1.0;
    }
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
      wristMaxAngle: t.wristMaxAngle,
      steerAmount: this.steerLive_,
      elbowAxisCone: t.elbowAxisCone,
      steerBlade: true,
      // THE TORSO'S SHARE, off the SMOOTHED integrals and scaled by the same
      // weight that owns the arm. The caps are the anatomy, the tuning
      // fractions are the taste (melee.cpp:906).
      torsoTwist: clamp(this.azLive_ * t.torsoShare, -0.5, 0.5) * w,
      torsoPitch: clamp(this.elLive_ * t.torsoPitch, -0.22, 0.35) * w,
    };
  }

  // melee.cpp:921 Arrest
  arrest() {
    if (this.phase_ !== PHASE.Wind && this.phase_ !== PHASE.Slash) return;
    this.phase_ = PHASE.Recover;
    this.phaseTime_ = 0;
    this.swingAz_ = 0; this.swingEl_ = 0; this.swingOut_ = 0;
    this.recoverHold_ = true;
  }

  // melee.cpp:949 Reset
  reset() {
    this.phase_ = PHASE.Idle;
    this.phaseTime_ = 0;
    this.inputAccum_ = v3(); this.mouseVel_ = v3(); this.mouseSpeed_ = 0;
    this.cutDir_ = v3(); this.cutAz_ = 0; this.cutEl_ = 0;
    this.az_ = 0; this.el_ = 0; this.radius_ = 0;
    this.azLive_ = 0; this.elLive_ = 0; this.radLive_ = 0;
    this.swingAz_ = 0; this.swingEl_ = 0; this.swingOut_ = 0;
    this.tipPrev_ = v3(); this.tipVel_ = v3(); this.tangent_ = v3();
    this.extendLive_ = 0;
    this.steerLive_ = clamp(this.tuning.steerFloor, 0, 1);
    this.framePrimed_ = false;
    this.recoverHold_ = false;
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

  // melee.h:785 ReachBand
  reachBand() { const b = this.radiusBand(); return { lo: b.lo, hi: b.hi }; }

  /* ------------------------------------------------------------------------
     melee.cpp:1003 RebuildFrame

     Given the integrated stroke, produce the tip, the blade frame, the bend
     pole and finally the hand. Everything is in BASIS coordinates until the
     last seven lines; nothing here reads the input.
     ---------------------------------------------------------------------- */
  rebuildFrame(dt, right, up, fwd) {
    const t = this.tuning;
    let { lo: rLo, hi: rHi } = this.radiusBand();

    // THE TOTAL, clamped: the steered stroke plus the cut's follow-through.
    const azHi = this.handSign_ > 0 ? t.azOut : t.azAcross;
    const azLo = this.handSign_ > 0 ? -t.azAcross : -t.azOut;
    const azSum = clamp(this.az_ + this.swingAz_, azLo, azHi);
    const elSum = clamp(this.el_ + this.swingEl_, t.elMin, t.elMax);
    const rSum = clamp(this.radius_ + this.swingOut_, rLo, rHi);

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
      if (vlen(tv) > 1e-3) this.tangent_ = vnorm(tv);
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
      const maxTurn = Math.max(t.leanTurnRate, 0) * dt;
      turn = clamp(turn, -maxTurn, maxTurn);
      // Rodrigues about the radius; `cur` is already perpendicular to it.
      let p = vadd(vmul(cur, Math.cos(turn)),
                   vmul(vcross(radial, cur), Math.sin(turn)));
      this.perpL_ = vlen(p) > 1e-5 ? vnorm(p) : cur;
    }

    let wantDir = radial;
    if (this.bladeLen_ > 1e-4 && r > 1e-4) {
      const cosT = clamp(
        (r * r + this.bladeLen_ * this.bladeLen_ -
         this.extendLive_ * this.extendLive_) / (2.0 * this.bladeLen_ * r),
        -1, 1);
      const sinT = Math.sqrt(Math.max(0, 1 - cosT * cosT));
      const s = t.handLead >= 0 ? 1 : -1;
      wantDir = vsub(vmul(radial, cosT), vmul(this.perpL_, sinT * s));
    }
    if (vlen(wantDir) < 1e-5) wantDir = radial;
    wantDir = vnorm(wantDir);
    // THE FLAT FACES OUT OF THE STROKE PLANE (:1132).
    let wantFlat = vcross(wantDir, this.tangent_);
    if (vlen(wantFlat) < 1e-3) wantFlat = this.bladeFlatL_;
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
      const hl = t.wristSmoothing * (this.phase_ === PHASE.Slash ? 0.35 : 1.0);
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
    }

    // ---- HOW COMMITTED THIS IS, AND THEREFORE HOW MUCH WRIST (:1327) ------
    {
      // ONLY SLASH FORCES FULL ALIGNMENT. Wind and Recover used to as well,
      // and that was the overhead-strike wrist wrench.
      let want = 1.0;
      if (this.phase_ !== PHASE.Slash) {
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

    // Screen motion -> a direction in CONTROL space. Screen +y is DOWN, so it
    // maps to -elevation (:1400).
    const gAz = this.mouseVel_.x * t.aimGainX;
    const gEl = -this.mouseVel_.y * t.aimGainY;
    const gLen = Math.sqrt(gAz * gAz + gEl * gEl);
    const haveDir = gLen > 1e-9;
    const dirAz = haveDir ? gAz / gLen : 0;
    const dirEl = haveDir ? gEl / gLen : 0;

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
          this.swingAz_ = 0; this.swingEl_ = 0; this.swingOut_ = 0;

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
        this.phase_ = this.mouseSpeed_ > t.commitSpeed * 0.35
          ? PHASE.Wind : PHASE.Guard;
        // COMMIT. The ARC's direction is frozen here (:1553).
        if (this.mouseSpeed_ > t.commitSpeed && haveDir) {
          this.cutAz_ = dirAz;
          this.cutEl_ = dirEl;
          this.cutDir_ = vnorm(vadd(vmul(right, dirAz), vmul(up, dirEl)));
          this.phase_ = PHASE.Slash;
          this.phaseTime_ = 0;
        }
        break;
      }

      case PHASE.Slash:
        if (this.phaseTime_ >= t.slashTime) {
          // FOLD THE ARC INTO THE STROKE: a cut ENDS WHERE IT WENT (:1568).
          this.az_ = clamp(this.az_ + this.swingAz_, azLo, azHi);
          this.el_ = clamp(this.el_ + this.swingEl_, t.elMin, t.elMax);
          this.swingAz_ = 0; this.swingEl_ = 0;
          this.recoverHold_ = armed && held;
          this.phase_ = PHASE.Recover;
          this.phaseTime_ = 0;
        }
        break;

      case PHASE.Recover:
        if (this.phaseTime_ >= t.recoverTime) {
          this.phase_ = (armed && held) ? PHASE.Guard : PHASE.Idle;
          this.phaseTime_ = 0;
          this.swingAz_ = 0; this.swingEl_ = 0; this.swingOut_ = 0;
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

    switch (this.phase_) {
      case PHASE.Slash: {
        // THE CUT, as an arc ADDED to the stroke (:1616).
        const tt = clamp(this.phaseTime_ / Math.max(t.slashTime, 1e-4), 0, 1);
        const e = tt * tt * (3.0 - 2.0 * tt);         // smoothstep
        const bow = 4.0 * e * (1.0 - e);              // 0 at the ends, 1 mid
        const drive = e - t.swingAnticipate * bow;
        this.swingAz_ = this.cutAz_ * t.swingArc * drive;
        this.swingEl_ = this.cutEl_ * t.swingArc * drive;
        this.swingOut_ = bow * t.swingExtend * tipReach;
        break;
      }
      case PHASE.Recover: {
        // Unwind only the follow-through (:1638).
        const k = smoothAlpha(0.07, dt);
        this.swingAz_ += (0 - this.swingAz_) * k;
        this.swingEl_ += (0 - this.swingEl_) * k;
        this.swingOut_ += (0 - this.swingOut_) * k;
        break;
      }
      default: break;
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
    windupTicks: 0, cutTicks: 0, recoverTicks: 0,
    seed: 0,
    aimAz: 0, aimEl: 0, aimed: false,
    wantAz: 0, wantEl: 0, wantReach: 0,
  };
}

/**
 * strokes.cpp:177 BeginStrokeProgram. Fills a cursor's program fields for one
 * swing; the CALLER resets its own container and re-tunes the MeleeState.
 */
export function beginStrokeProgram(cur, sty, styleIndex, seed) {
  cur.style = styleIndex;
  cur.seed = seed >>> 0;
  const tempo = 1.0 + sty.jitter.tempo * signedUnit(hash3(cur.seed, 1, 0));
  cur.windupTicks = Math.max(2, Math.round(sty.windup.ticks * tempo));
  cur.cutTicks = Math.max(2, Math.round(sty.cut.ticks * tempo));
  cur.recoverTicks = Math.max(1, sty.recoverTicks);
  cur.phase = STROKE_PHASE.Windup;
  cur.phaseTick = 0;
}

// strokes.h:253 StrokeStepResult.
export const STEP = { Idle: 0, Live: 1, Finished: 2 };

/**
 * strokes.cpp:190 StepStrokeProgram — one tick: synthesize the StrokeSample
 * the current phase wants and Step the driver with it.
 *
 * `liveAz`/`liveEl` are the aim's CURRENT bearing about the wielder's shoulder.
 * A player attack passes (0, 0), because the camera IS the aim; the tuner's
 * preview passes whatever the panel's aim sliders say, which is the NPC case.
 */
export function stepStrokeProgram(cur, sty, m, liveAz, liveEl, dt, right, up, fwd) {
  // The start bow: a deterministic wobble so ten swings do not look stamped.
  const bowAz = sty ? sty.jitter.az * signedUnit(hash3(cur.seed, 2, 0)) : 0;
  const bowEl = sty ? sty.jitter.el * signedUnit(hash3(cur.seed, 3, 0)) : 0;
  const smp = { dx: 0, dy: 0, dReach: 0, held: true };
  // THE CLOSED-LOOP, UNDER-COMMIT DRIVE, shared by Guard and Windup: 16
  // units/tick is 480 px/s against a 900 px/s commitSpeed, so the driver stays
  // in Guard however far it has to travel.
  const steerTo = (wantAz, wantEl, wantR) => {
    const t = m.tuning;
    smp.dx = clamp((wantAz - m.strokeAz()) / t.aimGainX, -16, 16);
    smp.dy = clamp(-(wantEl - m.strokeEl()) / t.aimGainY, -16, 16);
    smp.dReach = clamp((wantR - m.strokeRadius()) / Math.max(t.reachGain, 1e-4),
                       -18, 18);
    m.step(smp, dt, true, right, up, fwd);
  };

  switch (cur.phase) {
    case STROKE_PHASE.Guard: {
      // ABSOLUTE, not aim-relative: a guard is a pose, not a blow. wantReach
      // is a BAND POSITION (0 = as drawn back as this arm goes, 1 = extended).
      const { lo, hi } = m.reachBand();
      steerTo(cur.wantAz, cur.wantEl,
              clamp(lo + cur.wantReach * (hi - lo), lo, hi));
      break;
    }
    case STROKE_PHASE.Windup: {
      if (!sty) break;
      // THE CUT IS CENTRED ON THE AIM, so the windup lands HALF A CUT SHORT.
      cur.wantAz = liveAz - 0.5 * sty.cut.az + sty.windup.az + bowAz;
      cur.wantEl = liveEl - 0.5 * sty.cut.el + sty.windup.el + bowEl;
      // AGAINST A NEUTRAL EXTENSION, not against the live radius: computing
      // it as StrokeRadius() + offset every tick is a RUNAWAY.
      cur.wantReach = strokeReachIn(m, sty.windup.reach);
      steerTo(cur.wantAz, cur.wantEl, cur.wantReach);
      if (++cur.phaseTick >= cur.windupTicks) {
        // ---- COMMIT. The aim is frozen HERE and never refreshed.
        cur.aimAz = liveAz;
        cur.aimEl = liveEl;
        cur.aimed = true;
        cur.phase = STROKE_PHASE.Cut;
        cur.phaseTick = 0;
      }
      break;
    }
    case STROKE_PHASE.Cut: {
      if (!sty) break;
      // FROM WHERE THE BLADE ACTUALLY IS, THROUGH THE AIM, TO HALF A CUT PAST
      // IT — derived per tick, so a windup that could not quite reach its pose
      // still produces a cut through the target.
      const toAz = cur.aimAz + 0.5 * sty.cut.az;
      const toEl = cur.aimEl + 0.5 * sty.cut.el;
      const toR = strokeReachIn(m, sty.windup.reach + sty.cut.reach);
      const left = Math.max(1, cur.cutTicks - cur.phaseTick);
      const t = m.tuning;
      smp.dx = ((toAz - m.strokeAz()) / left) / t.aimGainX;
      smp.dy = -((toEl - m.strokeEl()) / left) / t.aimGainY;
      smp.dReach = ((toR - m.strokeRadius()) / left) / Math.max(t.reachGain, 1e-4);
      m.step(smp, dt, true, right, up, fwd);
      if (++cur.phaseTick >= cur.cutTicks) {
        cur.phase = STROKE_PHASE.Recover;
        cur.phaseTick = 0;
      }
      break;
    }
    case STROKE_PHASE.Recover: {
      // Button RELEASED: the driver's own recover ramps PoseWeight down.
      smp.held = false;
      m.step(smp, dt, true, right, up, fwd);
      if (++cur.phaseTick >= cur.recoverTicks) return STEP.Finished;
      break;
    }
    default:
      return STEP.Idle;
  }
  return STEP.Live;
}

/* ============================================================================
   THE STYLE LIBRARY — strokes.cpp:20-160 LoadAttackStyles + the flick compass.

   The loader's convention is the whole file's: a bad entry is skipped LOUDLY
   into `log` and is never fatal, and an unknown key is ignored so a newer
   authored file still loads on an older binary. The editor keeps the RAW json
   object alongside the parsed library, because it is what gets written back
   and it must preserve keys this port has never heard of.
   ========================================================================== */

// strokes.cpp:20 ReadSegment
function readSegment(j, dflt) {
  const n = (k, d) => (j && Number.isFinite(+j[k]) ? +j[k] : d);
  return {
    ticks: Math.max(1, Math.round(n('ticks', dflt.ticks))),
    az: n('az', dflt.az),
    el: n('el', dflt.el),
    reach: n('reach', dflt.reach),
  };
}

/**
 * strokes.cpp:32 LoadAttackStyles. Returns { styles, player, log }.
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
    styles.push({
      name,
      label: typeof s.label === 'string' ? s.label : name,
      windup: readSegment(s.windup, { ticks: 12, az: 0, el: 0, reach: 0 }),
      cut: readSegment(s.cut, { ticks: 7, az: 0, el: 0, reach: 0 }),
      recoverTicks: Math.max(1, Math.round(
        Number.isFinite(+(s.recover && s.recover.ticks)) ? +s.recover.ticks : 10)),
      jitter: {
        az: Number.isFinite(+jt.az) ? +jt.az : 0,
        el: Number.isFinite(+jt.el) ? +jt.el : 0,
        tempo: Number.isFinite(+jt.tempo) ? +jt.tempo : 0,
      },
      // strokes.h AttackStyle::clip — the body animation played WITH the
      // stroke, by library name (assets/anims/<name>.json). '' = none.
      clip: typeof s.clip === 'string' ? s.clip : '',
      raw: s,                      // the object the editor mutates in place
    });
  }
  // strokes.h:111 PlayerStrikeMap — INDICES, not names, resolved against the
  // library at load time; a sector naming an unknown style is skipped LOUDLY.
  const player = { sectors: [], neutral: [-1, -1] };
  const pj = json && json.player;
  const find = n => styles.findIndex(s => s.name === n);
  if (pj && typeof pj === 'object') {
    for (const sec of (Array.isArray(pj.sectors) ? pj.sectors : [])) {
      if (!sec || !Array.isArray(sec.dir) || sec.dir.length !== 2) {
        log.push('player.sectors: an entry with no 2-element "dir" — skipped');
        continue;
      }
      const i = find(sec.style);
      if (i < 0) {
        log.push(`player.sectors: unknown style "${sec.style}" — skipped`);
        continue;
      }
      player.sectors.push({ x: +sec.dir[0] || 0, y: +sec.dir[1] || 0, style: i, raw: sec });
    }
    const na = Array.isArray(pj.neutralAlternate) ? pj.neutralAlternate : [];
    for (let k = 0; k < 2; k++) {
      if (na[k] === undefined) continue;
      const i = find(na[k]);
      if (i < 0) log.push(`player.neutralAlternate: unknown style "${na[k]}"`);
      else player.neutral[k] = i;
    }
  }
  return { styles, player, log, raw: json };
}

// strokes.h:118 PlayerStrikeMap::Usable
export const playerMapUsable = lib =>
  !!lib && (lib.player.sectors.length > 0 || lib.player.neutral[0] >= 0);

/**
 * strokes.cpp:127 QuantizeStrike — the flick (screen space, +y down) -> a
 * style index by MAX DOT over the sectors; -1 when the map has none.
 *
 * Adding a sector is adding a line in the JSON, which is the whole point of
 * the compass being data.
 */
export function quantizeStrike(lib, dx, dy) {
  if (!lib || !lib.player.sectors.length) return -1;
  const len = Math.sqrt(dx * dx + dy * dy);
  if (len < 1e-6) return -1;
  const nx = dx / len, ny = dy / len;
  let best = -1, bestDot = -2;
  for (const s of lib.player.sectors) {
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
  if (!lib) return -1;
  const n = lib.player.neutral;
  const want = right ? 0 : 1;
  if (n[want] >= 0) return n[want];
  return n[want ^ 1] >= 0 ? n[want ^ 1] : -1;
}

// strokes.cpp:302 PickAttackStyle — RESOLVE BY NAME, THEN PICK, so a profile
// listing one unknown style among four still varies over the other three.
export function pickAttackStyle(lib, names, mobId, tick) {
  if (!lib || !lib.styles.length) return -1;
  const found = [];
  for (const s of names || []) {
    if (found.length >= 8) break;
    const i = lib.styles.findIndex(x => x.name === s);
    if (i >= 0) found.push(i);
  }
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
     PickAttackStyle                                    strokes.cpp:302-320
     Hash3 / SignedUnit / Pcg                           sim/rng.h:22-36

   NOT PORTED, and each is a deliberate line rather than an omission:

     MeleeSweepDamage / FindParry (melee.cpp)   need a World, a Jolt scene and
       the debris system. The panel therefore reports the blade's SPEED and
       EDGE ALIGNMENT — the two inputs the damage formula scales by — and says
       nothing about damage numbers, rather than inventing a second formula.
     Mob::ApplyWeaponArm's hinge-axis override (mob.cpp:7850)  the editor's
       clamp port (anim.js animClampPoseLimits) accepts overrides; rig.js
       supplies the steered plane the same way mob.cpp does.
   ========================================================================== */
