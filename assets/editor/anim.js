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

// anim.cpp:185 ClipFade
export function clipFade(clip, tMs, stopping, stopFade) {
  let f = 1;
  if (clip.blendInMs > 0) f = Math.min(f, tMs / clip.blendInMs);       // :187
  if (!clip.loop && clip.blendOutMs > 0 && clip.durationMs > 0)        // :188
    f = Math.min(f, (clip.durationMs - tMs) / clip.blendOutMs);
  if (stopping) f = Math.min(f, stopFade);                             // :190
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

    inst.timeMs += dt * 1000;                                  // :225
    if (c.loop && c.durationMs > 0) {                          // :226
      while (inst.timeMs >= c.durationMs) inst.timeMs -= c.durationMs;
    } else if (c.durationMs > 0 && inst.timeMs >= c.durationMs) {  // :228
      inst.timeMs = c.durationMs;
      inst.stopping = true;
    }
    if (inst.stopping) inst.fade = Math.max(0, inst.fade - dt * 6);  // :232
    else inst.fade = 1;
    const w = inst.weight * clipFade(c, inst.timeMs, inst.stopping, inst.fade);  // :234
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

  const dirTarget = vnorm(toTarget);                           // :370
  let pole = vnorm(chain.pole || v3(0, 0, 1));                 // :373
  if (vlen(pole) < 1e-6) pole = v3(0, 0, 1);
  let bendAxis = vcross(dirTarget, pole);                      // :375
  if (vlen(bendAxis) < 1e-4) {                                 // :376
    const alt = Math.abs(dirTarget.y) < 0.9 ? v3(0, 1, 0) : v3(1, 0, 0);
    bendAxis = vcross(dirTarget, alt);
  }
  bendAxis = vnorm(bendAxis);
  if (vlen(bendAxis) < 1e-4) return;                           // :382

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

  // ---- springs, mob.cpp:824 ----
  for (let i = 0; i < sk.parts.length; i++) {
    const p = sk.parts[i];
    if (!p.hasSpring) continue;                                // :828
    const gain = p.spring.gain ?? 1;
    const maxAngle = p.spring.maxAngle ?? 0.7;
    const goal = v3(-st.velocity.z * gain * 0.05, 0, st.velocity.x * gain * 0.05);  // :829
    goal.x = clamp(goal.x, -maxAngle, maxAngle);               // :831
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
        (anim.cpp:187, f = tMs/blendInMs at tMs=0), and a zero-weight clip is
        skipped outright (anim.cpp:240). The first sampled frame never reaches
        the accumulator.
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
