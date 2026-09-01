/* ============================================================================
   anim.js — line-by-line JS transcription of src/game/anim.cpp.

   THIS FILE IS NOT AN IMPLEMENTATION. It is a PORT. Every function below is a
   direct transcription of the engine's animation runtime, with the C++ source
   line cited above it. The editor's preview must show what the engine will do,
   so the only correct way to change anything here is to change anim.cpp first
   and then follow it.

   Kept deliberately free of three.js and DOM so it can be diffed against the
   C++ by eye and unit-tested headlessly.

   Coverage (see the report / the CITATIONS table at the bottom of this file):
     TRANSCRIBED  quaternion ops, easing, track sampling, clip fade, sample+
                  blend (override accumulator + additive), flatten, two-bone
                  IK, Holden spring, flipbook frame lookup, gait foot state
                  machine, body-from-feet, bob/sway/roll/spine/phase-lag.
     APPROXIMATED ground height (editor has no world: flat plane at y=0),
                  ragdoll/physics blend (stage 6, lives in mob.cpp and needs
                  Jolt), heading (preview walks along +Z with heading 0).
   ========================================================================== */

/* ============================================================================
   Vectors — anim.cpp uses Vec3 from math3d.h; these are the ops it calls.
   ========================================================================== */

export const v3 = (x = 0, y = 0, z = 0) => ({ x, y, z });
export const vadd = (a, b) => ({ x: a.x + b.x, y: a.y + b.y, z: a.z + b.z });
export const vsub = (a, b) => ({ x: a.x - b.x, y: a.y - b.y, z: a.z - b.z });
export const vmul = (a, s) => ({ x: a.x * s, y: a.y * s, z: a.z * s });
export const vdot = (a, b) => a.x * b.x + a.y * b.y + a.z * b.z;
export const vlen = a => Math.sqrt(vdot(a, a));
export const vcross = (a, b) => ({
  x: a.y * b.z - a.z * b.y,
  y: a.z * b.x - a.x * b.z,
  z: a.x * b.y - a.y * b.x,
});
export function vnorm(a) {
  const l = vlen(a);
  return l < 1e-9 ? { x: 0, y: 0, z: 0 } : { x: a.x / l, y: a.y / l, z: a.z / l };
}
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

/* ============================================================================
   Quaternions — anim.cpp:13-87
   ========================================================================== */

export const qid = () => ({ x: 0, y: 0, z: 0, w: 1 });

// anim.cpp:13 QuatMul
export const qmul = (a, b) => ({
  x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
  y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
  z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
  w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
});

// anim.cpp:20 QuatConj
export const qconj = q => ({ x: -q.x, y: -q.y, z: -q.z, w: q.w });

// anim.cpp:29 QuatDot
export const qdot = (a, b) => a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;

// anim.cpp:22 QuatAxisAngle — note the engine normalizes FIRST and returns
// identity for a degenerate axis.
export function qaxisangle(axis, angle) {
  const a = vnorm(axis);
  if (vlen(a) < 1e-6) return qid();
  const s = Math.sin(angle * 0.5);
  return { x: a.x * s, y: a.y * s, z: a.z * s, w: Math.cos(angle * 0.5) };
}

// anim.cpp:33 QuatNormalize
export function qnorm(q) {
  const l2 = qdot(q, q);
  if (l2 < 1e-12) return qid();
  const inv = 1 / Math.sqrt(l2);
  return { x: q.x * inv, y: q.y * inv, z: q.z * inv, w: q.w * inv };
}

// anim.cpp:40 QuatRotate
export function qrot(q, v) {
  const u = { x: q.x, y: q.y, z: q.z };
  const t = vmul(vcross(u, v), 2);
  return vadd(vadd(v, vmul(t, q.w)), vcross(u, t));
}

// anim.cpp:46 QuatRotateInv
export const qrotinv = (q, v) => qrot(qconj(q), v);

// anim.cpp:48 QuatNlerp — sign-fix onto the shortest arc, lerp, renormalize.
export function qnlerp(a, b, t) {
  let q = b;
  if (qdot(a, b) < 0) q = { x: -b.x, y: -b.y, z: -b.z, w: -b.w };
  const s = 1 - t;
  return qnorm({
    x: a.x * s + q.x * t, y: a.y * s + q.y * t,
    z: a.z * s + q.z * t, w: a.w * s + q.w * t,
  });
}

// anim.cpp:59 QuatSlerp
export function qslerp(a, b, t) {
  let d = qdot(a, b);
  let q = b;
  if (d < 0) { q = { x: -b.x, y: -b.y, z: -b.z, w: -b.w }; d = -d; }
  if (d > 0.9995) return qnlerp(a, q, t);
  const theta = Math.acos(clamp(d, -1, 1));
  const sinT = Math.sin(theta);
  const wa = Math.sin((1 - t) * theta) / sinT;
  const wb = Math.sin(t * theta) / sinT;
  return qnorm({
    x: a.x * wa + q.x * wb, y: a.y * wa + q.y * wb,
    z: a.z * wa + q.z * wb, w: a.w * wa + q.w * wb,
  });
}

// anim.cpp:75 QuatFromTo
export function qfromto(from, to) {
  const f = vnorm(from), t = vnorm(to);
  if (vlen(f) < 1e-6 || vlen(t) < 1e-6) return qid();
  const d = clamp(vdot(f, t), -1, 1);
  if (d > 0.999999) return qid();
  if (d < -0.999999) {
    const ax = Math.abs(f.x) < 0.9 ? v3(1, 0, 0) : v3(0, 1, 0);
    return qaxisangle(vnorm(vcross(f, ax)), 3.14159265);
  }
  const c = vcross(f, t);
  return qnorm({ x: c.x, y: c.y, z: c.z, w: 1 + d });
}

/* ============================================================================
   Easing — anim.cpp:91-122

   The enum order and every formula are transcribed exactly. ParseEase's
   default is Linear (anim.cpp:99), so an unknown string must NOT throw.
   ========================================================================== */

export const EASES = ['instant', 'linear', 'quadIn', 'quadOut', 'quadInOut',
                      'cubicIn', 'cubicOut', 'cubicInOut'];

// anim.cpp:102 ApplyEase
export function applyEase(e, t) {
  t = clamp(t, 0, 1);
  switch (e) {
    case 'instant': return 0;                                  // :105
    case 'quadIn': return t * t;                               // :106
    case 'quadOut': return 1 - (1 - t) * (1 - t);              // :107
    case 'quadInOut':                                          // :108
      return t < 0.5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
    case 'cubicIn': return t * t * t;                          // :110
    case 'cubicOut': {                                         // :111
      const u = 1 - t;
      return 1 - u * u * u;
    }
    case 'cubicInOut': {                                       // :115
      if (t < 0.5) return 4 * t * t * t;
      const u = -2 * t + 2;
      return 1 - u * u * u * 0.5;
    }
    default: return t;                                         // :120 Linear
  }
}

/* ============================================================================
   Track sampling — anim.cpp SampleTrack

   Keys are sorted by tMs; the first/last key of a CHANNEL is held outside its
   range. Easing is a property of the OUTGOING key of that channel.

   Rot and pos are sampled INDEPENDENTLY over the fused key list (engine fix
   2026-08-20): a key carrying only a position must not act as a rotation key —
   its rot field is default identity, and interpolating through it dragged
   every mid-segment rot sample toward upright (an authored 75° crawl pitch
   rendered as ~25°). Each channel interpolates between its own neighbours.

   A track here is { keys: [{tMs, rot, pos, hasRot, hasPos, ease}] }.
   ========================================================================== */

export function sampleTrack(track, tMs) {
  const keys = track.keys;
  if (!keys.length) return null;
  const channel = has => {
    let prev = -1, next = -1;
    for (let k = 0; k < keys.length; k++) {
      if (!keys[k][has]) continue;
      if (keys[k].tMs <= tMs) prev = k;
      else { next = k; break; }
    }
    if (prev < 0 && next < 0) return null;       // channel absent entirely
    if (prev < 0) return { a: keys[next], b: keys[next], u: 0 };
    if (next < 0) return { a: keys[prev], b: keys[prev], u: 0 };
    const a = keys[prev], b = keys[next];
    const span = b.tMs - a.tMs;
    const u = span > 0 ? (tMs - a.tMs) / span : 0;
    return { a, b, u: applyEase(a.ease || 'linear', u) };
  };
  const r = channel('hasRot');
  const p = channel('hasPos');
  if (!r && !p) return null;
  return {
    rot: r ? qnlerp(r.a.rot || qid(), r.b.rot || qid(), r.u) : qid(),
    pos: p ? vadd(p.a.pos || v3(),
                  vmul(vsub(p.b.pos || v3(), p.a.pos || v3()), p.u)) : v3(),
    gotRot: !!r,
    gotPos: !!p,
  };
}

/**
 * anim.cpp:215 ClipFade.
 *
 * THE BLEND-IN READS `ageMs`, NOT `tMs`. Time since the instance STARTED does
 * not wrap, so a looping clip fades in once instead of re-fading at the top of
 * every cycle — which is what a `tMs` blend-in does, and what this port did
 * until 2026-09-01: an 180 ms blend-in on a 600 ms looping walk re-dipped the
 * arms to zero weight three times a second in the preview and never in the
 * game. Callers that have no age (a one-shot scrub) pass tMs for both, which
 * is exactly what the pre-ageMs engine did.
 */
export function clipFade(clip, tMs, ageMs, stopping, stopFade) {
  let f = 1;
  if (clip.blendInMs > 0) f = Math.min(f, ageMs / clip.blendInMs);     // :218
  if (!clip.loop && clip.blendOutMs > 0 && clip.durationMs > 0)        // :219
    f = Math.min(f, (clip.durationMs - tMs) / clip.blendOutMs);
  if (stopping) f = Math.min(f, stopFade);                             // :221
  return clamp(f, 0, 1);
}

/* ============================================================================
   Stages 1-3: sample + blend + additive — anim.cpp:196-293 AnimSampleAndBlend

   `sk`    = { parts:[{name,parent,rest:{rot,pos},...}], clips:[...] }
   `st`    = { clips:[ClipInstance], local:[Transform] }
   Returns nothing; writes st.local.
   ========================================================================== */

export const kBlendEpsilon = 0.1;                              // anim.h:229

export function animSampleAndBlend(sk, st, dt) {
  const n = sk.parts.length;
  st.local = new Array(n);
  for (let i = 0; i < n; i++)                                  // :200 rest baseline
    st.local[i] = { rot: { ...sk.parts[i].rest.rot }, pos: { ...sk.parts[i].rest.pos } };
  if (!st.clips.length) return;                                // :201

  const acc = new Array(n), accPos = new Array(n);
  const accW = new Array(n).fill(0), touched = new Array(n).fill(0);
  // acc starts at ZERO, not identity — this is a weighted SUM, not a pose.
  // (The engine seeded it with a default-constructed Quat, i.e. identity,
  // which halved every clip's strength until 2026-08-20; this port was right.)
  for (let i = 0; i < n; i++) { acc[i] = { x: 0, y: 0, z: 0, w: 0 }; accPos[i] = v3(); }
  const additives = [];                                        // :213

  for (let ci = 0; ci < st.clips.length;) {
    const inst = st.clips[ci];
    if (inst.clip < 0 || inst.clip >= sk.clips.length) {        // :219
      st.clips.splice(ci, 1);
      continue;
    }
    const c = sk.clips[inst.clip];

    // THE PLAYHEAD CARRIES THE INSTANCE'S RATE; THE AGE DOES NOT (anim.h:307).
    // Clamped positive so a bad rate can stall a clip but never run it
    // backwards through its own blend. `rate` is how the avatar locks one
    // authored arm cycle to one live stride at any pace — see strideRateFor.
    if (!Number.isFinite(inst.rate)) inst.rate = 1;
    if (!Number.isFinite(inst.ageMs)) inst.ageMs = inst.timeMs || 0;
    inst.timeMs += dt * 1000 * Math.max(inst.rate, 0);         // :301
    inst.ageMs += dt * 1000;                                   // :302
    if (c.loop && c.durationMs > 0) {                          // :303
      while (inst.timeMs >= c.durationMs) inst.timeMs -= c.durationMs;
    } else if (c.durationMs > 0 && inst.timeMs >= c.durationMs) {  // :305
      inst.timeMs = c.durationMs;
      inst.stopping = true;
    }
    if (inst.stopping) inst.fade = Math.max(0, inst.fade - dt * 6);  // :309
    else inst.fade = 1;
    const w = inst.weight *
      clipFade(c, inst.timeMs, inst.ageMs, inst.stopping, inst.fade);  // :312
    if (inst.stopping && w <= 0) { st.clips.splice(ci, 1); continue; }  // :235
    ci++;
    if (w <= 0) continue;                                      // :240

    for (const tr of c.tracks) {
      if (tr.part < 0 || tr.part >= n) continue;               // :243
      // :244 — mask is per-part 0/1 allowlist; empty mask = affects all
      // parts; parts beyond a short mask's length are masked OFF (engine
      // hardened 2026-08-20, formerly quirk D).
      if (c.mask && c.mask.length &&
          (tr.part >= c.mask.length || !c.mask[tr.part]))
        continue;
      const s = sampleTrack(tr, inst.timeMs);                  // :247
      if (!s) continue;

      if (c.mode === 'additive') {                             // :249
        // :253 the delta is measured against the clip's OWN frame 0
        const ref = tr.keys[0].rot || qid();
        const dq = qmul(qconj(ref), s.rot);                    // :254
        additives.push({ part: tr.part, dq, w });
        continue;
      }

      // :259 OVERRIDE: accumulator-aligned nlerp — align against the RUNNING
      // SUM, not against a fixed reference.
      const p = tr.part;
      if (s.gotRot) {
        let q = s.rot;
        if (accW[p] > 0 && qdot(acc[p], q) < 0)                // :265
          q = { x: -q.x, y: -q.y, z: -q.z, w: -q.w };
        acc[p].x += q.x * w; acc[p].y += q.y * w;
        acc[p].z += q.z * w; acc[p].w += q.w * w;
      }
      if (s.gotPos) accPos[p] = vadd(accPos[p], vmul(s.pos, w));  // :271
      accW[p] += w;
      touched[p] = 1;
    }
  }

  // ---- stage 2: normalize ---- anim.cpp:278
  for (let i = 0; i < n; i++) {
    if (!touched[i] || accW[i] < kBlendEpsilon) continue;      // :279 rest fallback
    const mag = Math.sqrt(qdot(acc[i], acc[i]));
    if (mag < 1e-6) continue;                                  // :281 degenerate
    st.local[i].rot = qnorm(acc[i]);                           // :282
    st.local[i].pos = vadd(sk.parts[i].rest.pos, vmul(accPos[i], 1 / accW[i]));  // :283
  }

  // ---- stage 3: additive, AFTER normalization ---- anim.cpp:289
  for (const a of additives) {
    const scaled = qnlerp(qid(), a.dq, clamp(a.w, 0, 1));      // :290
    st.local[a.part].rot = qnorm(qmul(st.local[a.part].rot, scaled));  // :291
  }
}

/* ============================================================================
   Stage 4: flatten — anim.cpp:297-313 AnimFlatten

   ONE linear pass; requires parts stored parent-before-child.
   ========================================================================== */

export function animFlatten(sk, st) {
  const n = sk.parts.length;
  st.model = new Array(n);
  for (let i = 0; i < n; i++) {
    const p = sk.parts[i];
    if (p.parent < 0) {                                        // :305
      st.model[i] = { rot: { ...st.local[i].rot }, pos: { ...st.local[i].pos } };
    } else {
      const par = st.model[p.parent];
      st.model[i] = {
        rot: qnorm(qmul(par.rot, st.local[i].rot)),             // :309
        pos: vadd(par.pos, qrot(par.rot, st.local[i].pos)),     // :310
      };
    }
  }
}

/* ============================================================================
   Stage 5: two-bone IK — anim.cpp:317-417 AnimSolveTwoBone
   ========================================================================== */

export function animSolveTwoBone(sk, st, chain, targetModel, weight) {
  if (weight <= 0) return;                                     // :319
  if (chain.parts.length < 2 || chain.effector < 0) return;    // :320
  const i0 = chain.parts[0], i1 = chain.parts[1], ie = chain.effector;
  const n = sk.parts.length;
  if (i0 < 0 || i1 < 0 || ie < 0 || i0 >= n || i1 >= n || ie >= n) return;  // :325
  if (st.partAlive && st.partAlive.length &&
      (!st.partAlive[i0] || !st.partAlive[i1])) return;        // :328

  const root = st.model[i0].pos;                               // :332
  const joint = st.model[i1].pos;
  let tip = st.model[ie].pos;
  if (ie === i1)                                               // :337
    tip = vadd(joint, qrot(st.model[i1].rot, sk.parts[i1].rest.pos));

  const L1 = vlen(vsub(joint, root));                          // :339
  const L2 = vlen(vsub(tip, joint));
  if (L1 < 1e-4 || L2 < 1e-4) return;                          // :341

  const toTarget = vsub(targetModel, root);                    // :343
  const d = vlen(toTarget);
  if (d < 1e-4) return;                                        // :345
  const kEps = 1e-3;                                           // :349
  const dMin = Math.abs(L1 - L2) + kEps;
  const dMax = L1 + L2 - kEps;
  const dc = clamp(d, dMin, dMax);                             // :352

  const cosInterior = clamp((L1 * L1 + L2 * L2 - dc * dc) / (2 * L1 * L2), -1, 1);  // :357
  const interior = Math.acos(cosInterior);
  const angle2 = 3.14159265 - interior;                        // :360

  const adj = L1 + L2 * Math.cos(angle2);                      // :365
  const opp = L2 * Math.sin(angle2);
  const tx = dc, ty = 0;                                       // :367
  const angle1 = Math.atan2(ty * adj - tx * opp, tx * adj + ty * opp);  // :368

  const dirTarget = vnorm(toTarget);                           // :481
  let pole = vnorm(chain.pole || v3(0, 0, 1));                 // :484
  if (vlen(pole) < 1e-6) pole = v3(0, 0, 1);
  // ORTHOGONALIZE THE POLE AGAINST THE TARGET (Gram-Schmidt) rather than
  // crossing with it raw — anim.cpp:490. The old raw cross collapsed to zero
  // when the pole was (anti)parallel to the target and fell back to bending
  // about world-up, i.e. sideways, and it SNAPPED in discontinuously as the
  // limb swung through that direction.
  let polePerp = vsub(pole, vmul(dirTarget, vdot(dirTarget, pole)));  // :513
  if (vlen(polePerp) < 1e-4) {                                 // :514
    // No information left: choose a plane CONSISTENTLY or the joint spins as
    // the target crosses this axis.
    const alt = Math.abs(dirTarget.y) < 0.9 ? v3(0, 1, 0) : v3(1, 0, 0);
    polePerp = vsub(alt, vmul(dirTarget, vdot(dirTarget, alt)));
  }
  if (vlen(polePerp) < 1e-6) return;
  polePerp = vnorm(polePerp);
  // NOTE THE ORDER: polePerp x dirTarget, not dirTarget x polePerp
  // (anim.cpp:524). angle1 comes out NEGATIVE, so crossing the other way
  // swings the joint AWAY from the pole — which put the KNEE BACKWARD at
  // every target and bulged the elbow forward.
  let bendAxis = vcross(polePerp, dirTarget);                  // :526
  if (vlen(bendAxis) < 1e-4) return;                           // :527
  bendAxis = vnorm(bendAxis);

  const curDir = vnorm(vsub(joint, root));                     // :386
  const aim = qfromto(curDir, dirTarget);                      // :387
  const bend = qaxisangle(bendAxis, angle1);                   // :388
  const upperDelta = qmul(bend, aim);                          // :389

  const newUpper = qnorm(qmul(upperDelta, st.model[i0].rot));  // :391
  const newJoint = vadd(root, qrot(upperDelta, vsub(joint, root)));  // :392

  const curLower = vnorm(vsub(tip, joint));                    // :395
  const wantLower = vnorm(vsub(targetModel, newJoint));        // :396
  const lowerDelta = qfromto(qrot(upperDelta, curLower), wantLower);  // :397
  const newLower = qnorm(qmul(lowerDelta, qmul(upperDelta, st.model[i1].rot)));  // :398
  const newLowerPos = newJoint;                                // :400

  const w = clamp(weight, 0, 1);                               // :403
  st.model[i0].rot = qslerp(st.model[i0].rot, newUpper, w);    // :404
  st.model[i1].rot = qslerp(st.model[i1].rot, newLower, w);    // :405
  st.model[i1].pos = vadd(st.model[i1].pos, vmul(vsub(newLowerPos, joint), w));  // :406

  for (let k = 0; k < n; k++) {                                // :410 descendants
    if (sk.parts[k].parent !== i1) continue;
    st.model[k].rot = qnorm(qmul(st.model[i1].rot, st.local[k].rot));   // :413
    st.model[k].pos = vadd(st.model[i1].pos, qrot(st.model[i1].rot, st.local[k].pos));  // :415
  }
}

/* ============================================================================
   Springs — anim.cpp:421-437 AnimSpringStep (Holden closed form)
   ========================================================================== */

export function animSpringStep(def, s, goal, dt) {
  const halflife = Math.max(def.halflife ?? 0.15, 1e-3);       // :424
  const y = (2.77258872 / halflife) * 0.5;                     // :425
  const e = Math.exp(-y * dt);                                 // :426
  const maxAngle = def.maxAngle ?? 0.7;
  const ax = ['x', 'y', 'z'];
  const gs = [goal.x, goal.y, goal.z];
  for (let a = 0; a < 3; a++) {
    const k = ax[a];
    const j0 = s.x[k] - gs[a];                                 // :431
    const j1 = s.v[k] + j0 * y;                                // :432
    s.x[k] = e * (j0 + j1 * dt) + gs[a];                       // :433
    s.v[k] = e * (s.v[k] - j1 * y * dt);                       // :434
    s.x[k] = clamp(s.x[k], -maxAngle, maxAngle);               // :435
  }
}

/* ============================================================================
   Flipbooks — anim.cpp:441-459 AnimFlipbookFrame
   ========================================================================== */

export function animFlipbookFrame(fb, elapsedMs) {
  if (!fb.frames || !fb.frames.length) return -1;              // :442
  let total = 0;
  for (const f of fb.frames) total += Math.max(1, f.durationMs | 0);  // :444
  if (total <= 0) return -1;
  let t = elapsedMs;
  if (fb.loop !== false) {                                     // :447 (default true)
    t %= total;
    if (t < 0) t += total;
  } else if (t >= total) {
    return fb.frames.length - 1;                               // :451
  }
  let acc = 0;
  for (let i = 0; i < fb.frames.length; i++) {
    acc += Math.max(1, fb.frames[i].durationMs | 0);
    if (t < acc) return i;                                     // :456
  }
  return fb.frames.length - 1;
}

/* ============================================================================
   Gait — mob.cpp:602-756 UpdateGait, and the procedural layer at :780-838.

   APPROXIMATION, stated plainly: the engine samples the world for ground
   height (mob.cpp:665 GroundHeightAt). The editor has no world, so ground is
   the flat plane y=0. Everything else — the one-group-swinging rule, the
   sin(t*pi) arc, body-from-feet, Newell tilt — is transcribed.
   ========================================================================== */

export const TWO_PI = 6.2831853;
// anim.h:485. Quoted, not re-derived: it is the constant both engine drivers
// scale their normalized spring goal by.
export const kSpringVelScale = 0.6;

/**
 * Advance the foot state machine one step. Mirrors UpdateGait.
 *
 * @param sk    skeleton { parts, chains, gait }
 * @param st    anim state { feet:[FootState], velocity, partAlive }
 * @param ctx   { origin:Vec3, heading:number, speedNow:number, defSpeed:number,
 *                prefabSize:{x,z}, groundY:(x,z)=>number }
 */
export function updateGait(sk, st, ctx, dt) {
  const g = sk.gait;
  if (!sk.chains.length) return;                               // mob.cpp:605

  /* ---- WHICH CHAINS STEP: the one place the two engine drivers DISAGREE ----
   *
   * avatar.cpp:479 skips every chain whose tag is not "leg" — "arm chains
   * never step" — and avatar.cpp:310 marks their FootState invalid outright.
   * mob.cpp's UpdateGait (2448) has no such filter: it walks `sk.chains` whole
   * and treats an arm chain as a foot, planting the hand on the ground and
   * scheduling steps for it. An arm chain matches none of the gait's leg
   * `groups`, so the one-group-swinging rule gives it a group of its own and
   * it steps freely alongside the legs.
   *
   * That is not a transcription choice, it is a real difference between the
   * two drivers, and it is visible: previewing the shipped human (an avatar
   * rig, two leg chains and two arm chains) under the NPC rule planted both
   * HANDS like feet. Measured, hand.R tracked foot.R at a correlation of
   * -1.00 and the arms' own authored 13-degree walk swing was reduced to about
   * 6 degrees of IK residue — which is most of "the tuner's walk looks
   * nothing like the game".
   *
   * The editor follows the AVATAR, because the rigs authored here are
   * humanoids whose hands must not step, and because the human rig IS the
   * player avatar. `ctx.armChainsStep` opts back into the NPC rule for anyone
   * who needs to see it.
   *
   * NOT FIXED IN THE ENGINE HERE. Whether mob.cpp should adopt the filter is a
   * question about every NPC with an arm chain, not about the editor.
   */
  const steps = c => ctx.armChainsStep || sk.chains[c].tag === 'leg' ||
                     !sk.chains.some(x => x.tag === 'leg');

  const fwd = v3(Math.sin(ctx.heading), 0, Math.cos(ctx.heading));  // :607
  const speedFactor = clamp(ctx.speedNow / Math.max(ctx.defSpeed, 0.01), 0, 1.5);  // :612

  // :623 which group currently has a swinging foot
  let swingingGroup = -1;
  for (let c = 0; c < st.feet.length; c++) {
    if (!st.feet[c].swinging) continue;
    for (let gi = 0; gi < (g.groups || []).length; gi++)
      for (const p of g.groups[gi])
        if (p === sk.chains[c].effector || p === sk.chains[c].parts[0])
          swingingGroup = gi;
    if (swingingGroup < 0) swingingGroup = c;                  // :629 ungrouped
  }

  let sumY = 0, nFeet = 0;
  const plantedPts = [];
  const pivot = v3(ctx.prefabSize.x * 0.5, 0, ctx.prefabSize.z * 0.5);

  for (let c = 0; c < sk.chains.length && c < st.feet.length; c++) {
    const ch = sk.chains[c];
    const f = st.feet[c];

    // avatar.cpp:310 — a non-stepping chain's FootState is invalid, not merely
    // unscheduled, so the caller's IK loop (which gates on `valid`) leaves the
    // arm to the clips and the weapon driver.
    if (!steps(c)) { f.valid = false; f.swinging = false; continue; }

    let alive = true;                                          // :640
    for (const p of ch.parts)
      if (p >= 0 && st.partAlive && p < st.partAlive.length && !st.partAlive[p])
        alive = false;
    if (!alive) { f.valid = false; f.swinging = false; continue; }  // :645

    // :651 hip position in world space (rest rig + yaw)
    const hipLocal = sk.parts[ch.parts[0]].anchorLocal;
    const yawQ = qaxisangle(v3(0, 1, 0), ctx.heading);
    const hipWorld = vadd(vadd(ctx.origin,
      qrot(yawQ, vsub(hipLocal, pivot))), pivot);

    // :661 ideal contact point
    let ideal = vadd(vadd(hipWorld,
      vmul(fwd, g.strideBias * f.legLength * speedFactor)),
      vmul(st.velocity, g.leadTime));
    // :663-668 GroundHeightAt -> flat ground in the editor (APPROXIMATION)
    ideal.y = ctx.groundY ? ctx.groundY(ideal.x, ideal.z) : 0;

    if (!f.valid) {                                            // :670
      f.valid = true;
      f.planted = { ...ideal };
      f.swinging = false;
    }

    if (f.swinging) {
      f.swingT += dt / Math.max(g.stepDuration, 1e-3);         // :677
      if (f.swingT >= 1) {                                     // :678
        f.swingT = 0;
        f.swinging = false;
        f.planted = { ...f.swingTo };
        // TOUCHDOWN. avatar.cpp:634 calls SyncStrideClock from exactly here
        // and mob.cpp does not — the NPC path keeps a free oscillator. The
        // hook keeps that split visible instead of picking one for both: a
        // caller that wants the avatar's behaviour installs it.
        if (st.onTouchdown) st.onTouchdown(c);
      } else {
        const flat = vadd(f.swingFrom, vmul(vsub(f.swingTo, f.swingFrom), f.swingT));  // :683
        // :685 sin(t*pi) arc: zero lift at both ends, peak at mid-swing
        flat.y += g.stepHeight * f.legLength * speedFactor *
                  Math.sin(f.swingT * 3.14159265);
        f.planted = flat;                                      // :687
      }
    } else {
      const drift = vsub(ideal, f.planted);                    // :690
      const driftLen = Math.sqrt(drift.x * drift.x + drift.z * drift.z);  // :691
      let myGroup = -1;                                        // :695
      for (let gi = 0; gi < (g.groups || []).length; gi++)
        for (const p of g.groups[gi])
          if (p === ch.effector || p === ch.parts[0]) myGroup = gi;
      const groupFree = swingingGroup < 0 || swingingGroup === myGroup;  // :698
      if (driftLen > g.stepThreshold * f.legLength && groupFree) {       // :699
        f.swinging = true;
        f.swingT = 0;
        f.swingFrom = { ...f.planted };
        f.swingTo = { ...ideal };
        swingingGroup = myGroup >= 0 ? myGroup : c;            // :704
      }
      sumY += f.planted.y;                                     // :706
      nFeet++;
      plantedPts.push(f.planted);
    }
  }

  // ---- body from feet ---- mob.cpp:714
  if (nFeet > 0) {
    // bodyY is the prefab MIN CORNER, so the foot average is converted out of
    // the sole's frame; rideHeight is a stance trim ABOUT the authored rest
    // pose (1.0 = stand as modelled), not an absolute lift. Mirrors mob.cpp.
    const stance = (g.rideHeight - 1) * st.feet[0].legLength;
    const targetY = sumY / nFeet - restSoleY(sk) + stance;     // :717
    if (!ctx.footInit) {                                       // :720
      st.bodyY = targetY;
      ctx.footInit = true;
    } else {
      st.bodyY += clamp(targetY - st.bodyY, -0.4, 0.4);        // :725
    }
  } else {
    st.bodyY += clamp(ctx.origin.y - st.bodyY, -0.4, 0.4);     // :727
    ctx.footInit = true;
  }

  // ---- body tilt: Newell's method ---- mob.cpp:733
  let targetUp = v3(0, 1, 0);
  if (plantedPts.length >= 3) {
    let nrm = v3();
    for (let i = 0; i < plantedPts.length; i++) {              // :738
      const a = plantedPts[i], b = plantedPts[(i + 1) % plantedPts.length];
      nrm.x += (a.y - b.y) * (a.z + b.z);
      nrm.y += (a.z - b.z) * (a.x + b.x);
      nrm.z += (a.x - b.x) * (a.y + b.y);
    }
    if (nrm.y < 0) nrm = vmul(nrm, -1);                        // :744
    const up = vnorm(nrm);
    if (vlen(up) > 0.5 && up.y > 0.6) targetUp = up;           // :747
  }
  // :750 ease toward the target normal
  st.bodyUp = vnorm(vadd(vmul(st.bodyUp || v3(0, 1, 0), 0.85), vmul(targetUp, 0.15)));
  if (vlen(st.bodyUp) < 0.5) st.bodyUp = v3(0, 1, 0);
}

/**
 * The procedural layer that runs between stages 3 and 4.
 * mob.cpp:780-838: legacy phase swing with progressive lag, then pelvis
 * bob/sway/roll and spine counter-rotation, then springs.
 */
export function applyProceduralLayer(sk, st, ctx, dt) {
  const g = sk.gait;
  const present = !!g.present;
  const speedFactor = clamp(ctx.speedNow / Math.max(ctx.defSpeed, 0.01), 0, 1.5);
  const phase = st.gaitPhase * TWO_PI;                         // mob.cpp:775

  // ---- legacy swing, mob.cpp:784 ----
  for (let i = 0; i < sk.parts.length; i++) {
    const p = sk.parts[i];
    if (!p.swingAmp) continue;                                 // :786
    let depth = 0;                                             // :791
    for (let k = p.parent; k >= 0; k = sk.parts[k].parent) depth++;
    const lag = present ? g.phaseLag * depth * TWO_PI : 0;     // :793
    const swing = p.swingAmp * Math.sin(phase - lag + p.swingPhase * 3.14159265);  // :794
    // :797 parts owned by a chain are IK-driven; the swing does not apply
    let inChain = false;
    for (const ch of sk.chains)
      for (const cp of ch.parts) inChain = inChain || (cp === i);
    if (inChain) continue;                                     // :800
    st.local[i].rot = qnorm(qmul(st.local[i].rot, qaxisangle(p.axis, swing)));  // :801
  }

  // ---- pelvis bob / sway / roll / spine, mob.cpp:803 ----
  if (present && ctx.rootLimb >= 0 && ctx.rootLimb < sk.parts.length) {
    const root = st.local[ctx.rootLimb];
    // :807 bob at bobFreqMul x the stride frequency
    root.pos.y += g.bobAmp * Math.sin(g.bobFreqMul * TWO_PI * st.gaitPhase) * speedFactor;
    // :811 sway at 1x
    root.pos.x += g.swayAmp * Math.sin(TWO_PI * st.gaitPhase) * speedFactor;
    const roll = g.rollAmp * Math.sin(TWO_PI * st.gaitPhase) * speedFactor;  // :812
    root.rot = qnorm(qmul(root.rot, qaxisangle(v3(0, 0, 1), roll)));         // :813
    // :815 spine counter-rotation
    for (let i = 0; i < sk.parts.length; i++) {
      if (sk.parts[i].tag !== 'spine') continue;
      const yawCounter = -g.spineCounter * roll;               // :817
      st.local[i].rot = qnorm(qmul(st.local[i].rot, qaxisangle(v3(0, 1, 0), yawCounter)));
    }
  }

  // ---- springs, mob.cpp:2694 ----
  //
  // VELOCITY IS NORMALIZED BY THE DEF'S OWN TOP SPEED, so `gain` means
  // "radians of lag at full speed" for every def regardless of how fast that
  // def moves. This port used RAW voxels/second times a bare 0.05 until
  // 2026-09-01, which is what mob.cpp and avatar.cpp both did before the
  // avatar's 60-voxel/s top speed pegged every spring at maxAngle permanently
  // and they were unified on the normalized form. Three drivers must agree
  // here or a def's springs mean something different depending on which one
  // is animating it — and the editor was the third.
  const speedRef = Math.max(ctx.defSpeed, 0.01);
  for (let i = 0; i < sk.parts.length; i++) {
    const p = sk.parts[i];
    if (!p.hasSpring) continue;                                // :2697
    const gain = p.spring.gain ?? 1;
    const maxAngle = p.spring.maxAngle ?? 0.7;
    const goal = v3(-st.velocity.z / speedRef * gain * kSpringVelScale, 0,
                    st.velocity.x / speedRef * gain * kSpringVelScale);  // :2702
    goal.x = clamp(goal.x, -maxAngle, maxAngle);               // :2704
    goal.z = clamp(goal.z, -maxAngle, maxAngle);
    if (!st.springs[i]) st.springs[i] = { x: v3(), v: v3() };
    animSpringStep(p.spring, st.springs[i], goal, dt);         // :833
    const s = st.springs[i].x;
    const jiggle = qmul(qaxisangle(v3(1, 0, 0), s.x),          // :835
                        qmul(qaxisangle(v3(0, 1, 0), s.y),
                             qaxisangle(v3(0, 0, 1), s.z)));
    st.local[i].rot = qnorm(qmul(st.local[i].rot, jiggle));    // :838
  }
}

/* ============================================================================
   Skeleton construction from the sidecar

   Mirrors mob.cpp:216-260: parts in TOPOLOGICAL order (parents first, which
   AnimFlatten requires), anchorLocal from the sidecar or the root rule, and
   rest.pos = anchorLocal - parent.anchorLocal (mob.cpp:255-259).

   The editor cannot reuse AutoAnchor (mob.cpp:59) without duplicating a
   heuristic that could silently drift, so a limb with no explicit anchor gets
   the model-centre fallback and the UI says so. That is the one documented
   divergence in the rig build.
   ========================================================================== */

/**
 * mob.cpp:505-580 — the sidecar's `poseLimit` block into the three shapes
 * AnimClampPoseLimits understands. BALL FORM is selected by the presence of
 * `bone`, because the bone direction is the thing the axis form has no use for
 * and the ball form cannot work without.
 *
 * The loader's perpendicularity check on `reach` normals is reproduced: the
 * closed-form projection in clampDirHalfSpaces is only the NEAREST legal
 * direction when the planes are perpendicular, so a tilted pair is a load
 * error in the engine and is reported (not silently solved) here.
 */
function parsePoseLimit(pl) {
  const off = {
    hasPoseLimit: false, poseAxis: v3(1, 0, 0), poseMin: 0, poseMax: 0,
    poseHinge: false,
    poseBall: { has: false, bone: v3(0, -1, 0), reachCount: 0,
                reachNormal: [v3(), v3()], reachSin: [0, 0],
                hasTwist: false, twistMin: 0, twistMax: 0 },
    poseLimitWarn: '',
  };
  if (!pl || typeof pl !== 'object') return off;
  const kDeg = 3.14159265 / 180;
  const vec = (key, dflt) => (Array.isArray(pl[key]) && pl[key].length === 3)
    ? v3(+pl[key][0], +pl[key][1], +pl[key][2]) : dflt;
  const out = { ...off, poseBall: { ...off.poseBall, reachNormal: [v3(), v3()], reachSin: [0, 0] } };
  if (pl.bone !== undefined) {
    const b = out.poseBall;
    b.has = true;
    b.bone = vec('bone', v3(0, -1, 0));
    if (vlen(b.bone) < 1e-5) { out.poseLimitWarn = 'poseLimit.bone is zero'; b.has = false; }
    else b.bone = vnorm(b.bone);
    if (Array.isArray(pl.reach)) {
      for (const r of pl.reach) {
        if (b.reachCount >= 2) {
          out.poseLimitWarn = 'poseLimit.reach takes at most 2 planes';
          break;
        }
        let nrm = (r && Array.isArray(r.normal) && r.normal.length === 3)
          ? v3(+r.normal[0], +r.normal[1], +r.normal[2]) : v3(0, 0, 1);
        if (vlen(nrm) < 1e-5) { out.poseLimitWarn = 'poseLimit.reach normal is zero'; continue; }
        nrm = vnorm(nrm);
        if (b.reachCount === 1 && Math.abs(vdot(nrm, b.reachNormal[0])) > 1e-3)
          out.poseLimitWarn = 'poseLimit.reach normals must be perpendicular';
        b.reachNormal[b.reachCount] = nrm;
        // "at most N degrees past this plane" — the plane itself is 0.
        b.reachSin[b.reachCount] =
          Math.sin(clamp(Number.isFinite(+r?.max) ? +r.max : 0, -90, 90) * kDeg);
        b.reachCount++;
      }
    }
    if (pl.twist && typeof pl.twist === 'object') {
      b.hasTwist = true;
      b.twistMin = (Number.isFinite(+pl.twist.min) ? +pl.twist.min : -180) * kDeg;
      b.twistMax = (Number.isFinite(+pl.twist.max) ? +pl.twist.max : 180) * kDeg;
      if (b.twistMin > b.twistMax) { const t = b.twistMin; b.twistMin = b.twistMax; b.twistMax = t; }
    }
  } else {
    out.hasPoseLimit = true;
    out.poseAxis = vec('axis', v3(1, 0, 0));
    out.poseMin = (Number.isFinite(+pl.min) ? +pl.min : -180) * kDeg;
    out.poseMax = (Number.isFinite(+pl.max) ? +pl.max : 180) * kDeg;
    if (out.poseMin > out.poseMax) { const t = out.poseMin; out.poseMin = out.poseMax; out.poseMax = t; }
    // A hinge is one DOF, not one bounded DOF: it also DISCARDS the off-axis
    // swing (anim.h, AnimPart::poseHinge).
    out.poseHinge = !!pl.hinge;
  }
  return out;
}

export function buildSkeleton(sidecar, models) {
  const rawLimbs = Array.isArray(sidecar?.limbs) ? sidecar.limbs : [];
  const rootName = sidecar?.root || '';

  // --- topological sort, parents before children (mob.cpp TopoSortLimbs) ---
  const byName = new Map(rawLimbs.map(l => [l.name, l]));
  const ordered = [];
  const seen = new Set(), visiting = new Set();
  const visit = l => {
    if (!l || seen.has(l.name)) return;
    if (visiting.has(l.name)) return;             // cycle: break, engine errors
    visiting.add(l.name);
    if (l.parent && byName.has(l.parent)) visit(byName.get(l.parent));
    visiting.delete(l.name);
    if (seen.has(l.name)) return;
    seen.add(l.name);
    ordered.push(l);
  };
  const rootLimb = byName.get(rootName);
  if (rootLimb) visit(rootLimb);
  for (const l of rawLimbs) visit(l);

  const index = new Map(ordered.map((l, i) => [l.name, i]));
  const modelByName = new Map(models.map((m, i) => [m.name, i]));

  const parts = ordered.map(l => {
    const mi = modelByName.get(l.name);
    const m = mi === undefined ? null : models[mi];
    return {
      name: l.name,
      tag: l.tag || '',
      parent: (l.name === rootName || !byName.has(l.parent)) ? -1
              : (index.get(l.parent) ?? -1),
      modelIndex: mi === undefined ? -1 : mi,
      axis: Array.isArray(l.axis) && l.axis.length === 3
            ? v3(l.axis[0], l.axis[1], l.axis[2]) : v3(1, 0, 0),   // anim.h:143
      swingAmp: +l.swingAmp || 0,
      swingPhase: +l.swingPhase || 0,
      hasSpring: !!l.spring,
      spring: l.spring || { halflife: 0.15, gain: 1, maxAngle: 0.7 },
      // POSE-SPACE joint limits (anim.h:243, parsed at mob.cpp:508). Degrees
      // in, radians out, exactly where the engine converts them.
      ...parsePoseLimit(l.poseLimit),
      rest: { rot: qid(), pos: v3() },
      anchorLocal: v3(),
      anchorAuto: !(Array.isArray(l.anchor) && l.anchor.length === 3),
      _model: m,
    };
  });

  // --- anchors (mob.cpp:240-254) ---
  const rootIndex = index.get(rootName) ?? -1;
  parts.forEach((p, i) => {
    const l = ordered[i];
    if (Array.isArray(l.anchor) && l.anchor.length === 3) {
      p.anchorLocal = v3(+l.anchor[0], +l.anchor[1], +l.anchor[2]);
    } else if (i === rootIndex && p._model) {
      // mob.cpp:247 root anchor = (offset.x + size.x/2, offset.y, offset.z + size.z/2)
      const m = p._model;
      p.anchorLocal = v3(m.offset.x + m.dim.x * 0.5, m.offset.y,
                         m.offset.z + m.dim.z * 0.5);
    } else if (p._model) {
      // DIVERGENCE: engine uses AutoAnchor(); we use the model centre.
      const m = p._model;
      p.anchorLocal = v3(m.offset.x + m.dim.x * 0.5, m.offset.y + m.dim.y * 0.5,
                         m.offset.z + m.dim.z * 0.5);
      p.anchorEstimated = true;
    }
  });

  // --- rest.pos = anchorLocal - parent.anchorLocal (mob.cpp:255-259) ---
  parts.forEach(p => {
    p.rest.pos = p.parent >= 0
      ? vsub(p.anchorLocal, parts[p.parent].anchorLocal)
      : { ...p.anchorLocal };
  });

  // --- chains (mob.cpp:293-313) ---
  const chains = [];
  for (const c of (Array.isArray(sidecar?.chains) ? sidecar.chains : [])) {
    const ps = (c.parts || []).map(nm => index.get(nm)).filter(v => v !== undefined);
    const effName = c.effector || '';
    const eff = effName ? (index.get(effName) ?? -1)
                        : (ps.length ? ps[ps.length - 1] : -1);
    if (ps.length >= 2 && eff >= 0)                            // mob.cpp:311
      chains.push({
        tag: c.tag || '', parts: ps, effector: eff,
        pole: Array.isArray(c.pole) && c.pole.length === 3
              ? v3(c.pole[0], c.pole[1], c.pole[2]) : v3(0, 0, 1),
        weight: 1,
      });
  }

  // --- gait (mob.cpp:262-291) ---
  const gj = sidecar?.gait;
  const gait = {
    present: !!(gj && typeof gj === 'object'),
    cadence: gj?.cadence ?? 2.2,
    strideBias: gj?.strideBias ?? 0.35,
    leadTime: gj?.leadTime ?? 0.2,
    stepThreshold: gj?.stepThreshold ?? 0.6,
    stepDuration: gj?.stepDuration ?? 0.22,
    stepHeight: gj?.stepHeight ?? 0.25,
    rideHeight: gj?.rideHeight ?? 0.9,
    bobAmp: gj?.bobAmp ?? 0.06,
    bobFreqMul: gj?.bobFreqMul ?? 2.0,
    swayAmp: gj?.swayAmp ?? 0.05,
    rollAmp: gj?.rollAmp ?? 0.09,
    spineCounter: gj?.spineCounter ?? 0.7,
    phaseLag: gj?.phaseLag ?? 0.05,
    groups: [],
  };
  for (const grp of (gj?.groups || [])) {
    const members = (grp || []).map(nm => index.get(nm)).filter(v => v !== undefined);
    if (members.length) gait.groups.push(members);
  }

  // --- dismemberment locomotion states (mob.cpp "states" parse) ---
  // Authored order IS the priority order: first match wins in animSelectState.
  const states = [];
  for (const s of (Array.isArray(sidecar?.states) ? sidecar.states : [])) {
    const names = key => (Array.isArray(s[key]) ? s[key] : [])
      .map(nm => index.get(nm)).filter(v => v !== undefined);
    states.push({
      name: s.name || '',
      missingAll: names('missing'),
      missingAnyOf: names('missingAny'),
      minChainsLost: +s.minChainsLost || 0,
      clip: s.clip || '',
      speedScale: s.speedScale ?? 1.0,
      disableGait: !!s.disableGait,
      bodyYOffset: +s.bodyYOffset || 0,
    });
  }

  // --- clips (mob.cpp:317-373) ---
  const clips = [];
  const cj = (sidecar?.clips && typeof sidecar.clips === 'object') ? sidecar.clips : {};
  for (const name of Object.keys(cj)) {
    const c = cj[name] || {};
    const clip = {
      name,
      durationMs: +c.durationMs || 0,
      loop: !!c.loop,
      mode: c.mode === 'additive' ? 'additive' : 'override',
      blendInMs: +c.blendInMs || 0,
      blendOutMs: +c.blendOutMs || 0,
      mask: null,
      tracks: [],
    };
    if (Array.isArray(c.mask)) {                               // mob.cpp:330
      clip.mask = new Array(parts.length).fill(0);
      for (const nm of c.mask) {
        const pi = index.get(nm);
        if (pi !== undefined) clip.mask[pi] = 1;
      }
    }
    const tj = (c.tracks && typeof c.tracks === 'object') ? c.tracks : {};
    for (const pname of Object.keys(tj)) {
      const pi = index.get(pname);
      if (pi === undefined) continue;                          // mob.cpp:338
      const keys = [];
      const upsert = tMs => {                                  // mob.cpp:349
        let k = keys.find(o => o.tMs === tMs);
        if (!k) { k = { tMs, rot: qid(), pos: v3(), hasRot: false, hasPos: false, ease: 'linear' }; keys.push(k); }
        return k;
      };
      for (const k of (tj[pname].rot || [])) {                 // mob.cpp:356
        const key = upsert(+k.t || 0);
        if (Array.isArray(k.q) && k.q.length === 4)
          key.rot = { x: +k.q[0], y: +k.q[1], z: +k.q[2], w: +k.q[3] };
        key.hasRot = true;
        if (k.ease) key.ease = EASES.includes(k.ease) ? k.ease : 'linear';
      }
      for (const k of (tj[pname].pos || [])) {                 // mob.cpp:362
        const key = upsert(+k.t || 0);
        if (Array.isArray(k.v) && k.v.length === 3)
          key.pos = v3(+k.v[0], +k.v[1], +k.v[2]);
        key.hasPos = true;
        if (k.ease) key.ease = EASES.includes(k.ease) ? k.ease : 'linear';
      }
      keys.sort((a, b) => a.tMs - b.tMs);                      // mob.cpp:368
      if (keys.length) clip.tracks.push({ part: pi, keys });
    }
    clips.push(clip);
  }

  // --- flipbooks (mob.cpp:376-392) ---
  const flipbooks = [];
  const fj = (sidecar?.flipbooks && typeof sidecar.flipbooks === 'object')
    ? sidecar.flipbooks : {};
  for (const name of Object.keys(fj)) {
    const frames = [];
    for (const f of (fj[name].frames || [])) {
      const pi = index.get(f.part);
      frames.push({ part: pi === undefined ? -1 : pi, model: +f.model || 0,
                    durationMs: +f.durationMs || 100 });
    }
    flipbooks.push({ name, loop: fj[name].loop !== false, frames });
  }

  return {
    parts, chains, gait, clips, flipbooks, states, rootLimb: rootIndex,
    order: ordered.map(l => l.name),
    findPart: nm => index.get(nm) ?? -1,
  };
}

/* ============================================================================
   Stage 3.5 — the torso serves a live swing. anim.cpp:399 AnimApplySpineTwist.

   Runs between the procedural layer and the flatten, in BOTH engine drivers
   (mob.cpp:2718, avatar.cpp:1147). The editor's preview omitted it entirely
   until 2026-09-01, so a rig posed mid-stroke stood square while the game
   leaned its chest into the cut.
   ========================================================================== */

export function animApplySpineTwist(sk, st, yawRight, pitchUp, rootLimb) {
  if (Math.abs(yawRight) < 1e-4 && Math.abs(pitchUp) < 1e-4) return;
  let nSpine = 0;
  for (let i = 0; i < sk.parts.length; i++)
    if (sk.parts[i].tag === 'spine' && i !== rootLimb) nSpine++;
  if (nSpine === 0) return;
  // Model-space signs, both verified in the engine against geometry.py: a
  // positive rotation about +X pitches the nose DOWN, so "chest up" is the
  // negative angle; a positive rotation about +Y takes +Z (the rig's forward)
  // to +X and the basis right is fwd x up = -X, so "toward the right" is also
  // the negative angle. If a rig ever reads mirrored the A/B is
  // melee.torsoShare = 0, not a sign hunt across callers.
  const yawPer = -yawRight / nSpine;
  const pitchPer = -pitchUp / nSpine;
  const d = qmul(qaxisangle(v3(0, 1, 0), yawPer),
                 qaxisangle(v3(1, 0, 0), pitchPer));
  for (let i = 0; i < sk.parts.length; i++) {
    if (sk.parts[i].tag !== 'spine' || i === rootLimb) continue;
    if (i < (st.partAlive?.length ?? 0) && !st.partAlive[i]) continue;
    st.local[i].rot = qnorm(qmul(st.local[i].rot, d));
  }
}

/* ============================================================================
   Stage 6 — the pose has to be anatomically possible.
   anim.cpp:585-830 ClampTwist / TwistAngleAboutImpl / ClampHinge /
   ClampDirHalfSpaces / ClampBall / AnimClampPoseLimits.

   Runs AFTER all IK, because the IK is what puts a joint outside its range;
   running it before would clamp a pose the solver is about to overwrite.
   Works purely in st.model[] and deliberately does NOT write st.local[].
   ========================================================================== */

// anim.cpp:592 ClampTwist — swing-twist decomposition, clamp the twist,
// recompose. The swing is left ALONE, which is what makes a one-axis limit
// useful on a ball joint.
function clampTwist(q, axis, lo, hi) {
  const d = q.x * axis.x + q.y * axis.y + q.z * axis.z;
  let tw = { x: axis.x * d, y: axis.y * d, z: axis.z * d, w: q.w };
  const mag = Math.sqrt(tw.x * tw.x + tw.y * tw.y + tw.z * tw.z + tw.w * tw.w);
  // Degenerate: `q` is a half turn about an axis perpendicular to ours, so the
  // projection carries no information. Inventing an angle would snap the joint.
  if (mag < 1e-5) return q;
  tw = { x: tw.x / mag, y: tw.y / mag, z: tw.z / mag, w: tw.w / mag };
  const s = tw.x * axis.x + tw.y * axis.y + tw.z * axis.z;
  let ang = 2 * Math.atan2(s, tw.w);
  // Wrap into (-pi, pi] BEFORE clamping: an angle past a half turn is nearer
  // the opposite stop, and clamping unwrapped drives the joint the long way.
  while (ang > 3.14159265) ang -= TWO_PI;
  while (ang <= -3.14159265) ang += TWO_PI;
  const clamped = clamp(ang, lo, hi);
  if (Math.abs(clamped - ang) < 1e-6) return q;        // already legal
  const swing = qmul(q, qconj(tw));
  return qnorm(qmul(swing, qaxisangle(axis, clamped)));
}

/**
 * anim.cpp:629 TwistAngleAboutImpl, exported as AnimHingeAngleAbout — THE
 * SIGNED ANGLE OF `q` ABOUT `axis`, WRAPPED INTO (-pi, pi].
 *
 * PUBLIC because two places must agree about it: the hinge clamp reads a
 * joint's bend with it, and the weapon arm reads the SAME bend one stage
 * earlier to decide which sense of its steered hinge axis makes the authored
 * [min, max] describe it. Those were two copies in the engine and they
 * differed in exactly the wrap.
 */
export function animHingeAngleAbout(q, axis) {
  const d = q.x * axis.x + q.y * axis.y + q.z * axis.z;
  let t = { x: axis.x * d, y: axis.y * d, z: axis.z * d, w: q.w };
  const mag = Math.sqrt(t.x * t.x + t.y * t.y + t.z * t.z + t.w * t.w);
  if (mag < 1e-5) return null;
  t = { x: t.x / mag, y: t.y / mag, z: t.z / mag, w: t.w / mag };
  let ang = 2 * Math.atan2(t.x * axis.x + t.y * axis.y + t.z * axis.z, t.w);
  while (ang > 3.14159265) ang -= TWO_PI;
  while (ang <= -3.14159265) ang += TWO_PI;
  return ang;
}

// anim.cpp:647 ClampHinge — ONE degree of freedom: a pure rotation about
// `axis` by the clamped angle, with the off-axis swing DISCARDED.
function clampHinge(q, axis, lo, hi) {
  // No recoverable angle: the nearest legal HINGE is the one the joint is
  // already closest to, and with no angle to read that is the rest end.
  const ang = animHingeAngleAbout(q, axis) ?? 0;
  return qaxisangle(axis, clamp(ang, lo, hi));
}

/**
 * anim.cpp:659 ClampDirHalfSpaces — clamp the unit direction `d` into the
 * intersection of up to two half-spaces `d . n[k] <= s[k]`, NEAREST legal
 * direction. The normals are mutually perpendicular (the loader enforces it),
 * so completing them to an orthonormal frame turns the projection into three
 * independent components and the unit-length identity supplies the third.
 */
function clampDirHalfSpaces(d, n, s, count) {
  if (count <= 0) return d;
  const n0 = n[0];
  let n1 = count >= 2 ? n[1] : (Math.abs(n0.x) < 0.9 ? v3(1, 0, 0) : v3(0, 1, 0));
  n1 = vsub(n1, vmul(n0, vdot(n0, n1)));
  if (vlen(n1) < 1e-5) return d;
  n1 = vnorm(n1);
  const n2 = vcross(n0, n1);
  const c0 = vdot(d, n0), c1 = vdot(d, n1), c2 = vdot(d, n2);
  const w0 = Math.min(c0, s[0]);
  const w1 = count >= 2 ? Math.min(c1, s[1]) : c1;
  if (w0 === c0 && w1 === c1) return d;      // already legal, bit-for-bit
  const rem = 1 - w0 * w0 - w1 * w1;
  if (rem <= 0) {
    // Both stops pinned so hard no unit vector satisfies them: give back the
    // closest thing that exists rather than a NaN.
    const v = vadd(vmul(n0, w0), vmul(n1, w1));
    return vlen(v) > 1e-5 ? vnorm(v) : d;
  }
  // The third component keeps its sign — taking it from the INPUT makes the
  // choice vary continuously everywhere except one measure-zero corner.
  const s2 = Math.sqrt(rem) * (c2 < 0 ? -1 : 1);
  return vnorm(vadd(vadd(vmul(n0, w0), vmul(n1, w1)), vmul(n2, s2)));
}

/**
 * anim.cpp:702 ClampBall — swing-twist for a ball joint: bound where the bone
 * POINTS, then bound how far it rolls about itself. The decomposition is exact
 * rather than approximate.
 */
function clampBall(q, lim) {
  const bone = lim.bone;
  const dOld = qrot(q, bone);
  const dNew = clampDirHalfSpaces(dOld, lim.reachNormal, lim.reachSin, lim.reachCount);
  const swing = qfromto(bone, dOld);
  let twist = qnorm(qmul(qconj(swing), q));
  if (lim.hasTwist) twist = clampTwist(twist, bone, lim.twistMin, lim.twistMax);
  const swingNew = (dNew.x === dOld.x && dNew.y === dOld.y && dNew.z === dOld.z)
    ? swing : qfromto(bone, dNew);
  return qnorm(qmul(swingNew, twist));
}

/**
 * anim.cpp:724 AnimClampPoseLimits.
 *
 * `overrides` is a list of { part, axis, blend } — anim.h PoseAxisOverride, A
 * HINGE WHOSE PLANE IS STEERED. The weapon arm supplies one: the stroke driver
 * rotates the elbow's bend plane into the plane of the cut, and an authored
 * axis fixed in the upper arm's frame no longer coincides with it. The RANGE
 * is never touched, only which plane the one degree of freedom lives in.
 */
export function animClampPoseLimits(sk, st, overrides) {
  const n = sk.parts.length;
  if ((st.model?.length ?? 0) < n) return;
  let any = false;
  for (let i = 0; i < n; i++) any = any || sk.parts[i].hasPoseLimit || sk.parts[i].poseBall.has;
  if (!any) return;              // the common case: nothing authored to pay for

  // SNAPSHOT THE POSE AS THE IK LEFT IT, expressed parent-relative. st.local[]
  // cannot be used: the IK writes st.model[] directly and never updates local,
  // so local still holds the PRE-IK pose. Re-deriving from the model pair is
  // what lets a clamp on the hip carry the knee with it while KEEPING the
  // knee's own solved bend.
  const eff = new Array(n);
  for (let i = 0; i < n; i++) {
    const par = sk.parts[i].parent;
    if (par < 0 || par >= n) { eff[i] = { rot: st.model[i].rot, pos: st.model[i].pos }; continue; }
    const inv = qconj(st.model[par].rot);
    eff[i] = {
      rot: qnorm(qmul(inv, st.model[i].rot)),
      pos: qrot(inv, vsub(st.model[i].pos, st.model[par].pos)),
    };
  }

  // Parents-first, which the topological part order guarantees: a child
  // recomposes against a parent that has already been clamped, so the
  // correction propagates down the limb in one pass with no second flatten.
  for (let i = 0; i < n; i++) {
    const p = sk.parts[i];
    const par = p.parent;
    if (p.hasPoseLimit || p.poseBall.has) {
      // Measured against REST, not against the parent: "0 degrees" has to mean
      // the pose the art was drawn in.
      const rest = p.rest.rot;
      let delta = qmul(qconj(rest), eff[i].rot);
      let moved = false;
      // BALL FIRST, THEN THE AXIS FORM. No rig authors both, and running the
      // ball last would make "which won" depend on the order, not the data.
      if (p.poseBall.has) {
        delta = clampBall(delta, p.poseBall);
        moved = true;
      } else if (p.hasPoseLimit) {
        let raw = p.poseAxis;
        // THE PLANE MAY BE STEERED; THE RANGE MAY NOT.
        for (const ov of (overrides || [])) {
          if (ov.part !== i || ov.blend <= 0) continue;
          const b = Math.min(ov.blend, 1);
          const a = vnorm(raw);
          const o = vnorm(ov.axis);
          // A defensive sign fix: the two senses name the same PLANE but only
          // one makes the authored [min, max] describe the bend the joint is
          // in, and blending an axis against its own negation passes through
          // zero, where the length guard below silently drops the clamp.
          const as = vdot(a, o) < 0 ? vmul(a, -1) : a;
          raw = vadd(as, vmul(vsub(o, as), b));
          break;
        }
        const len = vlen(raw);
        if (len > 1e-5) {
          const axis = vmul(raw, 1 / len);
          delta = p.poseHinge ? clampHinge(delta, axis, p.poseMin, p.poseMax)
                              : clampTwist(delta, axis, p.poseMin, p.poseMax);
          moved = true;
        }
      }
      if (moved) eff[i].rot = qnorm(qmul(rest, delta));
    }
    if (par < 0 || par >= n) {
      st.model[i] = eff[i];
    } else {
      st.model[i] = {
        rot: qnorm(qmul(st.model[par].rot, eff[i].rot)),
        pos: vadd(st.model[par].pos, qrot(st.model[par].rot, eff[i].pos)),
      };
    }
  }
}

/* ============================================================================
   The LOCOMOTION CLIP FAMILY — avatar.cpp:1651-1740.

   NOT part of anim.cpp: this is the AVATAR's clip selection, and it is the
   reason the editor's walk preview looked nothing like the game. The gait
   layer places FEET; the arms swing because `walk` (or `run`) is PLAYING over
   it, and nothing in the editor ever started one. A rig previewed with K
   showed IK legs and rest arms, which is not a pose the engine ever produces
   while moving.

   The three details that are not obvious and are all load-bearing:

     * THE FAMILY IS EXCLUSIVE. idle / walk / run / fall / hang are keyed on
       the same arms and spine, and `idle` was started-but-never-retired in the
       engine for a while: it kept composing with the walk forever and dragged
       an authored 14-degree arm swing down to about 4. That near-motionless
       pose is the reported "arms outstretched like a zombie".
     * THE WALK/RUN SPLIT IS NOT AT HALF SPEED. `def.speed` is the SPRINT
       reference, so a plain walk already sits at ~0.58 of it — right on top of
       a 0.55 threshold, and the selection then flipped every frame with
       neither clip getting past a fraction of its 180 ms blend-in. Split at
       0.80/0.70 with hysteresis instead.
     * `moving` IS 0.4 VOXELS/S, not "velocity is nonzero".
   ========================================================================== */

/**
 * Which clip of the family should be playing, by NAME, or '' for none.
 * `st` carries the running/latched flag so the hysteresis survives across
 * calls, exactly as avatar.cpp's `running_` member does.
 */
export function pickLocoClip(sk, st, speedNow, defSpeed, opts) {
  const o = opts || {};
  const has = n => sk.clips.some(c => c.name === n);
  const moving = speedNow > 0.4 && !o.airborne;
  const runOn = defSpeed * 0.80;
  const runOff = defSpeed * 0.70;
  st.running = st.running ? (speedNow > runOff) : (speedNow > runOn);
  if (o.hanging && has('hang')) return 'hang';
  if (o.airborne && has('fall')) return 'fall';
  if (!moving) return has('idle') ? 'idle' : '';
  if (st.running && has('run')) return 'run';
  return has('walk') ? 'walk' : (has('idle') ? 'idle' : '');
}

/* ============================================================================
   THE STRIDE CLOCK — avatar.cpp:840 SyncStrideClock and :945.

   ONE CLOCK, AND THE FEET OWN IT. This is the second half of why the editor's
   walk looked nothing like the game, and it is a bigger divergence than the
   missing clip: the preview advanced `gaitPhase` from `cadence * speedFactor`,
   which is the NPC oscillator — and avatar.cpp abandoned that because on the
   avatar the two clocks disagree by about 2.6x. Measured on the shipped human
   at a walk, the oscillator wanted 4.6 strides/s and the feet were taking
   about 1.8, so an arm cycle rated against it came out pinned at the rate
   clamp's ceiling of 3.0 and the arms cycled at nearly twice the legs.

   The rate is MEASURED between touchdowns; the phase is CORRECTED at each one.
   Until two steps have been timed the rate is zero and the pelvis holds still,
   which is the honest answer for a body that has not taken a stride yet.

   A rig with no leg chains (dummy.json) keeps the oscillator: it never plants
   a foot, so there is nothing to lock to. Both engine drivers state that
   fallback the same way and so does this.
   ========================================================================== */

// avatar.cpp:114-117. Quoted, not chosen.
export const kStrideSyncGain = 0.5;
export const kStepPeriodHalflife = 0.25;
export const kMaxStepPeriod = 1.2;

/** avatar.cpp:840 SyncStrideClock — call from the touchdown hook. */
export function animSyncStrideClock(sk, st, chain) {
  // Which leg is this, among the LEG chains? Ordinal, not chain index: a rig
  // whose arm chains are interleaved with its legs must still split the
  // stride evenly between the legs that actually step.
  let legOrdinal = 0, nLegs = 0, mine = 0;
  for (let c = 0; c < sk.chains.length; c++) {
    if (sk.chains[c].tag !== 'leg') continue;
    if (c === chain) mine = legOrdinal;
    legOrdinal++; nLegs++;
  }
  if (nLegs <= 0) return;

  const elapsed = st.sinceTouchdown ?? 0;
  st.sinceTouchdown = 0;
  // A first touchdown, or one after a stop, has no period to measure — adopt
  // the boundary outright and wait for the next step to time the rate.
  const haveStep = (st.lastFootDown ?? -1) >= 0 && elapsed > 1e-3 &&
                   elapsed < kMaxStepPeriod;
  st.lastFootDown = chain;
  if (haveStep) {
    const k = 1 - Math.pow(0.5, elapsed / kStepPeriodHalflife);
    st.stepPeriod = (st.stepPeriod ?? 0) + (elapsed - (st.stepPeriod ?? 0)) * k;
    // A stride is nLegs steps (two, for a biped): every leg lands once.
    const stride = st.stepPeriod * nLegs;
    if (stride > 1e-3) st.strideRate = 1 / stride;
  } else {
    st.stepPeriod = 0;
  }

  // Pull the phase onto this leg's boundary. Error taken the short way round
  // so a clock running a hair fast is nudged back rather than dragged forward
  // through a whole cycle.
  const want = mine / nLegs;
  let err = want - st.gaitPhase;
  err -= Math.floor(err + 0.5);
  st.gaitPhase += err * (haveStep ? kStrideSyncGain : 1.0);
  st.gaitPhase -= Math.floor(st.gaitPhase);
}

/**
 * avatar.cpp:945 — advance gaitPhase, from the FEET when the rig has legs and
 * from the oscillator when it does not. Returns the live stride rate
 * (strides/sec), which is what the locomotion clips are re-rated against.
 */
export function animAdvanceGaitPhase(sk, st, ctx, dt) {
  const g = sk.gait;
  st.sinceTouchdown = (st.sinceTouchdown ?? 0) + dt;
  const haveLegs = (sk.chains || []).some(ch => ch.tag === 'leg');
  if (haveLegs) {
    // Park the clock when the feet stop reporting: a body standing still takes
    // no steps, and a rate left running would keep accumulating phase to land
    // on an arbitrary value the moment it moves again.
    if (st.sinceTouchdown > kMaxStepPeriod)
      st.strideRate = (st.strideRate ?? 0) * Math.pow(0.5, dt / 0.15);
    st.gaitPhase += dt * (st.strideRate ?? 0);
  } else {
    const speedFactor = clamp(ctx.speedNow / Math.max(ctx.defSpeed, 0.01), 0, 1.5);
    st.gaitPhase += dt * (g.present ? g.cadence : 2.2) * speedFactor;
  }
  st.gaitPhase -= Math.floor(st.gaitPhase);
  return st.strideRate ?? 0;
}

/**
 * avatar.cpp:1737 — RE-RATE the locomotion pair to the LIVE STRIDE.
 *
 * `walk` and `run` are each authored at ONE speed (the arm cycle is derived
 * from the runtime's own step model at walk pace and at sprint pace). Every
 * speed between them plays an arm cycle the feet do not share, and the arms
 * slide in and out of phase with the legs over a few strides. One authored
 * cycle spans one stride at any pace once the instance is re-rated.
 *
 * rate 1 == the clip's authored period equals one stride. Only the pair:
 * idle / fall / hang / jump / land are not stride-locked motions and must keep
 * their authored timing.
 */
export const clipRateForStride = (strideRate, durationMs) =>
  clamp(strideRate * (durationMs * 0.001), 0.25, 3.0);

/**
 * Dismemberment state selection — anim.cpp AnimSelectState: first rule whose
 * predicate holds against st.partAlive, or -1. A chain counts as lost when ANY
 * of its parts is dead (the same test updateGait uses to drop a leg), and an
 * empty predicate never matches (it would shadow every rule after it).
 */
export function animSelectState(sk, st) {
  const dead = p => p >= 0 && p < (st.partAlive?.length ?? 0) && !st.partAlive[p];
  let chainsLost = 0;
  for (const ch of (sk.chains || []))
    if (ch.parts.some(dead)) chainsLost++;
  for (let r = 0; r < (sk.states?.length ?? 0); r++) {
    const rule = sk.states[r];
    if (!rule.missingAll.length && !rule.missingAnyOf.length &&
        rule.minChainsLost <= 0) continue;
    let match = rule.missingAll.every(dead);
    if (rule.missingAnyOf.length) match = match && rule.missingAnyOf.some(dead);
    if (match && chainsLost >= rule.minChainsLost) return r;
  }
  return -1;
}

/**
 * legLength per chain — mob.cpp:541-549: the sum of the chain's rest bone
 * lengths, floored at 1.
 */
export function chainLegLength(sk, chain) {
  let len = 0;
  for (let k = 1; k < chain.parts.length; k++)
    len += vlen(sk.parts[chain.parts[k]].rest.pos);
  return Math.max(len, 1);
}

/**
 * Rest sole height above the prefab min corner — mob.cpp MobSystem::Spawn.
 * Walk each leg chain's rest offsets down from the root and keep the lowest
 * effector anchor. This is what converts the foot plane (a sole height) into
 * the min-corner frame bodyY is expressed in; without it the rig floats by
 * roughly a leg length, which is the bug this mirrors the fix for.
 */
export function restSoleY(sk) {
  let lowest = 0, any = false;
  for (const ch of sk.chains || []) {
    if (ch.tag !== 'leg' || !ch.parts.length) continue;
    let y = sk.parts[ch.parts[0]].anchorLocal.y;
    for (let k = 1; k < ch.parts.length; k++)
      y += sk.parts[ch.parts[k]].rest.pos.y;
    if (!any || y < lowest) { lowest = y; any = true; }
  }
  return any ? lowest : 0;
}

/* ============================================================================
   CITATIONS — what is transcribed vs approximated

   TRANSCRIBED (C++ line cited inline above each block):
     QuatMul/Conj/AxisAngle/Dot/Normalize/Rotate/RotateInv/Nlerp/Slerp/FromTo
     ApplyEase (all 8 modes)          anim.cpp:102-122
     SampleTrack (per-CHANNEL hold + ease-on-outgoing-key + nlerp; rot and
       pos interpolate between their own neighbouring keys)  anim.cpp SampleTrack
     ClipFade                          anim.cpp:185-192
     AnimSampleAndBlend (timing, loop/stop, fade, mask, additive ref-frame,
       accumulator-aligned override, normalize, kBlendEpsilon)  anim.cpp:196-293
     AnimFlatten                       anim.cpp:297-313
     AnimSolveTwoBone (reach clamp, law of cosines, atan2 root angle, pole
       bend axis + fallback, slerp weighting, descendant re-flatten)
                                       anim.cpp:317-417
     AnimSpringStep (Holden)           anim.cpp:421-437
     AnimFlipbookFrame                 anim.cpp:441-459
     UpdateGait (one-group rule, sin(t*pi) arc, drift threshold, body-from-
       feet, Newell tilt, 0.85/0.15 ease)  mob.cpp:602-756
     Procedural layer (phase lag by depth, in-chain skip, bob/sway/roll,
       spine counter, spring goal)     mob.cpp:780-838
     Skeleton build (topo order, anchors, rest.pos, chains, gait groups,
       clip/track fusion, flipbooks)   mob.cpp:216-392
     animSelectState (chain-lost test, empty-predicate skip, first match)
                                       anim.cpp AnimSelectState
     chainLegLength                    mob.cpp:541-549
     animApplySpineTwist (stage 3.5)   anim.cpp:399-424
     animClampPoseLimits (stage 6) +   anim.cpp:585-830
       clampTwist/clampHinge/clampBall/clampDirHalfSpaces, the steered-plane
       PoseAxisOverride, and animHingeAngleAbout (the ONE place a hinge bend
       is measured — two engine copies disagreed about the wrap)
     parsePoseLimit (axis / hinge / ball forms, degrees -> radians)
                                       mob.cpp:505-580
     pickLocoClip + strideRateFor      avatar.cpp:1651-1747

   ADDED 2026-09-01, and each closed a REAL divergence the preview was showing:

     * clipFade's blend-in now reads `ageMs`, not `timeMs`. A looping clip
       re-faded to zero weight at the top of every cycle in the editor and
       once in the game (anim.h:300, anim.cpp:218).
     * ClipInstance carries `rate`, multiplied into the playhead only. Without
       it the walk/run arm cycle cannot be locked to the live stride and the
       arms drift out of phase with the feet over a few strides (anim.h:307).
     * The spring goal is normalized by the def's top speed (mob.cpp:2702).
       This port had the pre-2026-08 raw-velocity form, so a def's springs
       meant a different thing in the editor than in either engine driver.
     * Stages 3.5 and 6 existed in both engine drivers and in neither preview.

   APPROXIMATED (documented, not silently):
     GroundHeightAt      -> flat plane y=0 (the editor has no World)
     stage 6 PHYSICS     -> omitted; ragdoll blending needs Jolt
     heading             -> fixed 0 (the model is authored in its own frame)
     AutoAnchor          -> model-centre fallback for anchor-less limbs,
                            flagged as `anchorEstimated` so the UI can say so

   ENGINE BEHAVIOURS THAT LOOK LIKE BUGS BUT ARE REPRODUCED ON PURPOSE.
   Each is covered by a regression test; do NOT "fix" any of them here — fix
   anim.cpp and then follow it.

     A. blendInMs > 0 makes the clip's FIRST frame weigh exactly 0
        (anim.cpp:218, f = ageMs/blendInMs at ageMs=0), and a zero-weight clip
        is skipped outright (anim.cpp:318). The first sampled frame never
        reaches the accumulator. Since 2026-09-01 this is keyed on the AGE, so
        it happens once per instance rather than once per loop.
     B. The blend-OUT ramp is a pure function of time vs durationMs, so it
        applies to any non-looping clip whether or not it is stopping
        (anim.cpp:188). Looping clips never fade out.
     C. The additive reference is `tr.keys.front().rot` — the first KEY, not
        the key at t=0 (anim.cpp:253). A track whose first key sits at t=500
        measures its delta from there.
     D. (RESOLVED 2026-08-20) The mask guard formerly let a mask array
        SHORTER than the part list pass high-index parts through unmasked.
        Engine and this port now treat out-of-range parts as masked OFF
        (anim.cpp:244). The loader always sizes masks to the part count, so
        only hand-built short masks ever saw the difference.
     E. Total accumulated weight below kBlendEpsilon (0.1) falls back to the
        REST pose rather than scaling up the contribution (anim.cpp:279).
     F. Override blending aligns each incoming quaternion against the RUNNING
        SUM, not a fixed reference (anim.cpp:265), so q and -q blend to q
        instead of cancelling to rest.
   ========================================================================== */
