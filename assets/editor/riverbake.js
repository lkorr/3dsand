/* riverbake.js — the ENGINE's river / lake bake in JS (docs/PLAN_worldbuilder.md
 * E2 item 1): pure, DOM-free, Node-runnable. A line-for-line port of
 * src/sim/worldwater.cpp ParseRiversLakes + BakeRiversLakes (steps 1-8 of
 * its header comment), so the World map page can draw a river's reaches,
 * waterfalls and longitudinal profile LIVE while it is edited, without a
 * save + --mapcheck round trip.
 *
 * The one input the editor cannot reproduce is the engine's BARE GROUND
 * (worldmap::BareGroundHeight: the height plane + the seeded terrain noise +
 * the sculpt layer). The bake takes it as a function `ground(x, z, i)` (i =
 * the sample index); the page passes the height plane (MT.heightAt), so a
 * live preview differs from the engine's bake by exactly the noise the plane
 * does not hold, and the inspector draws the engine's own numbers from
 * --mapcheck beside it once saved.
 *
 * PINNED: tests/river_vectors.json is the ENGINE's output for a set of
 * rivers and lakes on a copy of the `peaks` map (scripts/gen_river_vectors.py
 * runs `sandvox --mapcheck` on it), with the bare ground the engine sampled
 * at every sample; scripts/test_riverbake.mjs feeds that ground back in and
 * requires every sample's arc and surface, the reach count, the waterfalls,
 * the drain level, the piece count and each lake's outline size + bbox to be
 * identical. Change worldwater.cpp's bake and this file in the same commit.
 *
 * Integer rules: C++ `std::lround` rounds half AWAY from zero (lround below),
 * int / int truncates toward zero (Math.trunc), and every coordinate is an
 * integer voxel after step 2. */

import * as SPL from './spline.js';

// worldmap.h (kRiver* / kLake*): the loader's clamps and the bake's constants.
export const K = {
  riverLip: 4, riverPieceMax: 256, widthMin: 8, widthMax: 1024, depthMin: 2, depthMax: 120,
  bankMax: 1024, stepDefault: 4, stepMax: 64, waterfall: 20, lakeEdgeMax: 256,
  lakeBandDefault: 24, lakeBandMax: 128, roadProfileMax: 256,
};

/** C++ std::lround: half away from zero. */
export const lround = v => (v < 0 ? -Math.round(-v) : Math.round(v));
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

/* ---- PARSE: a map.json river / lake as the loader reads it ----------------------------- */
/** {knots: [[x, z, w|-1, d|-1]], smooth, stepVox, bank (-1 = auto), profile,
 *  joins, warnings[]} or {error}. */
export function parseRiver(r) {
  const warn = [];
  if (!r || typeof r.id !== 'string' || !r.id) return {error: 'a rivers[] entry needs an id'};
  if (!Array.isArray(r.points)) return {error: `river "${r.id}": needs points`};
  const knots = [];
  for (const p of r.points) {
    if (!Array.isArray(p) || (p.length !== 2 && p.length !== 4) || p.some(v => typeof v !== 'number' || !isFinite(v)))
      return {error: `river "${r.id}": a point that is not [x, z] or [x, z, w, d]`};
    const q = [lround(p[0]), lround(p[1]), -1, -1];
    if (p.length === 4) {
      q[2] = clamp(lround(p[2]), K.widthMin, K.widthMax);
      q[3] = clamp(lround(p[3]), K.depthMin, K.depthMax);
      if (q[2] !== lround(p[2])) warn.push(`river "${r.id}": w ${lround(p[2])} clamped to ${q[2]}`);
      if (q[3] !== lround(p[3])) warn.push(`river "${r.id}": d ${lround(p[3])} clamped to ${q[3]}`);
    }
    const b = knots[knots.length - 1];
    if (b && b[0] === q[0] && b[1] === q[1]) continue;
    knots.push(q);
  }
  if (knots.length < 2) return {error: `river "${r.id}": needs at least two distinct points`};
  if (knots[0][2] < 0 || knots[knots.length - 1][2] < 0) return {error: `river "${r.id}": the first and last points must carry [x, z, w, d]`};
  const stepVox = typeof r.stepVox === 'number' ? clamp(lround(r.stepVox), 1, K.stepMax) : K.stepDefault;
  const bank = typeof r.bank === 'number' ? clamp(lround(r.bank), 0, K.bankMax) : -1;
  let profile = typeof r.profile === 'string' ? r.profile : 'U';
  if (profile !== 'U' && profile !== 'V') { warn.push(`river "${r.id}": profile "${profile}" is not U or V; using U`); profile = 'U'; }
  return {id: r.id, knots, smooth: r.smooth === true, stepVox, bank, profile, joins: typeof r.joins === 'string' ? r.joins : '', warnings: warn};
}
/** {id, level, points, smooth, band} or {error}. */
export function parseLake(l) {
  if (!l || typeof l.id !== 'string' || !l.id) return {error: 'a lakes[] entry needs an id'};
  if (typeof l.level !== 'number' || !isFinite(l.level)) return {error: `lake "${l.id}": needs level`};
  if (!Array.isArray(l.points)) return {error: `lake "${l.id}": needs points`};
  const pts = [];
  for (const p of l.points) {
    if (!Array.isArray(p) || p.length !== 2 || !p.every(v => typeof v === 'number' && isFinite(v))) return {error: `lake "${l.id}": a point that is not [x, z]`};
    const q = [lround(p[0]), lround(p[1])], b = pts[pts.length - 1];
    if (b && b[0] === q[0] && b[1] === q[1]) continue;
    pts.push(q);
  }
  if (pts.length > 1 && pts[0][0] === pts[pts.length - 1][0] && pts[0][1] === pts[pts.length - 1][1]) pts.pop();
  if (pts.length < 3) return {error: `lake "${l.id}": a polygon needs at least three distinct points`};
  const band = typeof l.band === 'number' ? clamp(lround(l.band), 4, K.lakeBandMax) : K.lakeBandDefault;
  return {id: l.id, level: lround(l.level), points: pts, smooth: l.smooth === true, band};
}

/* ---- LAKES: the outline the engine tests columns against ------------------------------- */
const roundDedupeRaw = raw => {
  const out = [];
  for (const q of raw) {
    const v = [lround(q.x), lround(q.z)], b = out[out.length - 1];
    if (b && b[0] === v[0] && b[1] === v[1]) continue;
    out.push(v);
  }
  return out;
};
/** The baked polygon (poly, x0, z0, x1, z1) of a parsed lake: the ring (closed
 *  spline at step 16 when smooth), Douglas-Peucker 0.75, edges split to <= 256. */
export function lakePoly(lk) {
  let ring = lk.smooth ? roundDedupeRaw(SPL.tessellateRaw(lk.points.map(p => [p[0], p[1]]), {step: 16, smooth: true, closed: true})) : lk.points.map(p => p.slice());
  while (ring.length > 1 && ring[0][0] === ring[ring.length - 1][0] && ring[0][1] === ring[ring.length - 1][1]) ring.pop();
  if (ring.length >= 4) {
    const open = SPL.simplify([...ring, ring[0]], 0.75);
    open.pop();
    if (open.length >= 3) ring = open;
  }
  const poly = [];
  for (let i = 0; i < ring.length; i++) {
    const [ax, az] = ring[i], [bx, bz] = ring[(i + 1) % ring.length];
    const len = Math.max(Math.abs(bx - ax), Math.abs(bz - az));
    const n = Math.max(1, Math.trunc((len + K.lakeEdgeMax - 1) / K.lakeEdgeMax));
    for (let k = 0; k < n; k++) poly.push([ax + Math.trunc((bx - ax) * k / n), az + Math.trunc((bz - az) * k / n)]);
  }
  let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
  for (const [x, z] of poly) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); z0 = Math.min(z0, z); z1 = Math.max(z1, z); }
  return {poly, x0, z0, x1, z1};
}
/** The engine's lake predicate (worldgen.wgsl lakeTest / Lake::Inside): column
 *  (x, z) is inside when an odd number of edges cross row z strictly right of it. */
export function lakeInside(poly, x, z) {
  let inside = false;
  const n = poly.length;
  for (let i = 0, k = n - 1; i < n; k = i++) {
    const ax = poly[k][0], az = poly[k][1], bx = poly[i][0], bz = poly[i][1];
    if ((az > z) === (bz > z)) continue;
    let den = bz - az, num = ax * den + (z - az) * (bx - ax);
    if (den < 0) { den = -den; num = -num; }
    const kx = Math.floor((num - 1) / den);
    if (x <= kx) inside = !inside;
  }
  return inside;
}

/* ---- RIVERS ------------------------------------------------------------------------------ */
/** Bake every river and lake of a map.json the way the engine does.
 *  opts = {seaY, ground(x, z, i, riverId) -> integer voxels}.
 *  Returns {lakes: [{id, level, band, poly, x0, z0, x1, z1}], rivers: [{id,
 *  joins, joinIdx, stepVox, bank, profile, samples: {x, z, s, hw, d, g, S}
 *  (arrays), line, reaches, starts, falls: [[x, z, drop]], pieces, endLevel,
 *  lakeIn, lakeOut, profile: {s, y, ground} (decimated like --mapcheck),
 *  warnings}], errors: [], warnings: []}. One river failing to parse is an
 *  error (the engine refuses the map); the rest still bake for the preview. */
export function bakeAll(json, opts = {}) {
  const sea = opts.seaY == null ? (json.seaLevelY | 0) : opts.seaY;
  const ground = opts.ground || (() => sea);
  const out = {lakes: [], rivers: [], errors: [], warnings: []};
  for (const l of json.lakes || []) {
    const p = parseLake(l);
    if (p.error) { out.errors.push(p.error); continue; }
    out.lakes.push({...p, ...lakePoly(p)});
  }
  const parsed = [];
  for (const r of json.rivers || []) {
    const p = parseRiver(r);
    if (p.error) { out.errors.push(p.error); continue; }
    out.warnings.push(...p.warnings);
    parsed.push(p);
  }
  for (const rv of parsed) {
    rv.joinIdx = -1;
    if (!rv.joins) continue;
    rv.joinIdx = parsed.findIndex(o => o.id === rv.joins);
    if (rv.joinIdx < 0) out.errors.push(`river "${rv.id}": joins "${rv.joins}", which is not a river on this map`);
    else if (parsed[rv.joinIdx] === rv) { out.errors.push(`river "${rv.id}": joins itself`); rv.joinIdx = -1; }
  }
  // parents before tributaries (the engine's DFS, index order)
  const order = [], state = new Array(parsed.length).fill(0);
  const visit = i => {
    if (state[i] === 2) return true;
    if (state[i] === 1) { out.errors.push(`rivers' joins form a cycle through "${parsed[i].id}"`); return false; }
    state[i] = 1;
    if (parsed[i].joinIdx >= 0 && !visit(parsed[i].joinIdx)) { parsed[i].joinIdx = -1; }
    state[i] = 2; order.push(i);
    return true;
  };
  for (let i = 0; i < parsed.length; i++) visit(i);
  const lakeAt = (x, z) => {
    for (let k = 0; k < out.lakes.length; k++) {
      const L = out.lakes[k];
      if (x >= L.x0 && x <= L.x1 && z >= L.z0 && z <= L.z1 && lakeInside(L.poly, x, z)) return k;
    }
    return -1;
  };
  const baked = new Array(parsed.length).fill(null);
  for (const ri of order) {
    const rv = parsed[ri];
    const par = rv.joinIdx >= 0 ? baked[rv.joinIdx] : null;
    const b = bakeRiver(rv, {sea, ground: (x, z, i) => ground(x, z, i, rv.id), lakes: out.lakes, lakeAt, parent: par});
    baked[ri] = b;
  }
  out.rivers = baked.filter(Boolean);
  for (const b of out.rivers) out.warnings.push(...b.warnings);
  return out;
}

/** One parsed river's bake (steps 1-8). ctx = {sea, ground(x, z, i), lakes,
 *  lakeAt(x, z) -> lake index | -1, parent: the parent's bake or null}. */
export function bakeRiver(rv, ctx) {
  const warnings = [], wwho = `river "${rv.id}": `;
  const nk = rv.knots.length;
  // 1. the tessellation, each knot's arc along it, the missing w / d filled
  const tp = SPL.tessellateRaw(rv.knots.map(k => [k[0], k[1]]), {step: 16, smooth: rv.smooth, closed: false});
  const karc = new Array(nk).fill(0);
  {
    let arc = 0;
    for (let i = 0; i < tp.length; i++) {
      if (i > 0) arc += Math.hypot(tp[i].x - tp[i - 1].x, tp[i].z - tp[i - 1].z);
      if (tp[i].f === 0 && tp[i].seg >= 0 && tp[i].seg < nk) karc[tp[i].seg] = arc;
      if (i + 1 === tp.length) karc[nk - 1] = arc;
    }
  }
  const kw = new Array(nk).fill(0), kd = new Array(nk).fill(0);
  for (let a = 0; a < 2; a++) {
    const dst = a === 0 ? kw : kd;
    let prev = 0;
    for (let i = 0; i < nk; i++) {
      if (rv.knots[i][2 + a] < 0) continue;
      dst[i] = rv.knots[i][2 + a];
      for (let q = prev + 1; q < i; q++) {
        const span = karc[i] - karc[prev], f = span > 0 ? (karc[q] - karc[prev]) / span : 0;
        dst[q] = dst[prev] + (dst[i] - dst[prev]) * f;
      }
      prev = i;
    }
  }
  // 2. the integer centreline
  const cl = [];
  for (const q of tp) {
    const s0 = clamp(q.seg, 0, nk - 1), s1 = Math.min(s0 + 1, nk - 1);
    const v = {x: lround(q.x), z: lround(q.z), w: kw[s0] + (kw[s1] - kw[s0]) * q.f, d: kd[s0] + (kd[s1] - kd[s0]) * q.f};
    const b = cl[cl.length - 1];
    if (b && b.x === v.x && b.z === v.z) continue;
    cl.push(v);
  }
  if (cl.length < 2) return null;
  // 3. samples every 4 voxels
  const sm = [];
  {
    let arc = 0;
    for (let i = 0; i + 1 < cl.length; i++) {
      const dx = cl[i + 1].x - cl[i].x, dz = cl[i + 1].z - cl[i].z;
      const len = Math.max(1, lround(Math.sqrt(dx * dx + dz * dz)));
      const n = Math.max(1, Math.trunc((len + 3) / 4));
      for (let k = 0; k < n; k++) {
        const f = k / n;
        sm.push({x: cl[i].x + Math.trunc(dx * k / n), z: cl[i].z + Math.trunc(dz * k / n), s: arc + Math.trunc(len * k / n),
                 hw: Math.max(1, lround((cl[i].w + (cl[i + 1].w - cl[i].w) * f) / 2)), d: lround(cl[i].d + (cl[i + 1].d - cl[i].d) * f), g: 0, S: 0});
      }
      arc += len;
    }
    const e = cl[cl.length - 1];
    sm.push({x: e.x, z: e.z, s: arc, hw: Math.max(1, lround(e.w / 2)), d: lround(e.d), g: 0, S: 0});
  }
  const N = sm.length;
  const bankAt = hw => {
    let b = rv.bank >= 0 ? rv.bank : lround(1.5 * 2.0 * hw);
    b = Math.min(b, K.bankMax, 1536 - hw);
    return Math.max(b, 0);
  };
  // 4. the bare ground
  for (let i = 0; i < N; i++) sm[i].g = Math.round(ctx.ground(sm[i].x, sm[i].z, i));
  // 5. a source in a lake
  const lakes = ctx.lakes || [], lakeAt = ctx.lakeAt || (() => -1);
  const lakeIn = lakeAt(sm[0].x, sm[0].z), lakeOut = lakeAt(sm[N - 1].x, sm[N - 1].z);
  let lead = -1, leadLevel = 0;
  if (lakeIn >= 0) {
    const lk = lakes[lakeIn];
    let j = 0;
    while (j < N && lakeInside(lk.poly, sm[j].x, sm[j].z)) j++;
    const sj = sm[Math.min(j, N - 1)];
    const Lf = sj.hw + bankAt(sj.hw) + lk.band + K.riverLip + 4;
    lead = 0;
    while (lead + 1 < N && sm[lead + 1].s <= sj.s + Lf) lead++;
    leadLevel = lk.level;
  }
  // 6. running min, quantized into reaches
  {
    let m = 0, G0 = 0, R = 0;
    for (let i = 0; i < N; i++) {
      const s = sm[i];
      if (i <= lead) { s.S = leadLevel; m = G0 = leadLevel + 1; R = leadLevel; continue; }
      if (i === 0) { m = G0 = s.g; R = G0 - 1; }
      else { m = Math.min(m, s.g); if (m <= G0 - rv.stepVox) { G0 = m; R = m - 1; } }
      s.S = R;
    }
  }
  // 7. the level it drains to, and the tail
  let E = ctx.sea, tailFrom = N, drainsTo = 'sea';
  const par = ctx.parent;
  if (rv.joinIdx >= 0) {
    drainsTo = 'river';
    if (par && par.line.length) {
      let best = Infinity, bi = 0;
      for (let k = 0; k < par.line.length; k++) {
        const dx = par.line[k][0] - sm[N - 1].x, dz = par.line[k][1] - sm[N - 1].z;
        if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; bi = k; }
      }
      E = par.line[bi][2];
      const hwP = par.line[bi][3], dist = Math.sqrt(best);
      if (dist > hwP + 2) warnings.push(`${wwho}its mouth is ${Math.trunc(dist)} voxels from "${par.id}"'s centreline, outside its channel (half-width ${hwP})`);
      const se = sm[N - 1];
      const Lf = hwP + se.hw + bankAt(se.hw) + K.riverLip + 4;
      tailFrom = N - 1;
      while (tailFrom > 0 && sm[tailFrom - 1].s >= se.s - Lf) tailFrom--;
      let below = 0;
      for (let i = tailFrom; i < N; i++) below = Math.max(below, E - sm[i].S);
      if (below > 0) warnings.push(`${wwho}its surface at the junction was up to ${below} voxels below "${par.id}"'s (${E}); clamped to it`);
    }
  } else if (lakeOut >= 0) {
    drainsTo = 'lake';
    const lk = lakes[lakeOut];
    E = lk.level;
    let j = N - 1;
    while (j > 0 && lakeInside(lk.poly, sm[j - 1].x, sm[j - 1].z)) j--;
    const sj = sm[j];
    const Lf = sj.hw + bankAt(sj.hw) + lk.band + K.riverLip + 4;
    tailFrom = j;
    while (tailFrom > 0 && sm[tailFrom - 1].s >= sj.s - Lf) tailFrom--;
  }
  if (lakeIn >= 0 && leadLevel < E) warnings.push(`${wwho}its source lake's level ${leadLevel} is below the level it drains to (${E})`);
  for (let i = 0; i < N; i++) { sm[i].S = Math.max(sm[i].S, E); if (i >= tailFrom) sm[i].S = E; }
  // 8. reaches, waterfalls, pieces
  const starts = [0], falls = [];
  for (let i = 1; i < N; i++)
    if (sm[i].S !== sm[i - 1].S) {
      starts.push(i);
      const drop = sm[i - 1].S - sm[i].S;
      if (drop > K.waterfall) falls.push([sm[i].x, sm[i].z, drop]);
    }
  let pieces = 0;
  for (let r = 0; r < starts.length; r++) {
    const a0 = starts[r], b0 = r + 1 < starts.length ? starts[r + 1] : N - 1;
    if (b0 <= a0) continue;
    const stack = [[a0, b0]];
    while (stack.length) {
      const [a, b] = stack.pop();
      const A = sm[a], B = sm[b];
      const vx = B.x - A.x, vz = B.z - A.z, vl = Math.max(Math.sqrt(vx * vx + vz * vz), 1e-9);
      let dev = 0, farK = -1;
      for (let k = a + 1; k < b; k++) {
        const Q = sm[k], qx = Q.x - A.x, qz = Q.z - A.z;
        let d = Math.abs(qx * vz - qz * vx) / vl;
        const f = B.s > A.s ? (Q.s - A.s) / (B.s - A.s) : 0;
        d = Math.max(d, Math.abs(Q.hw - (A.hw + (B.hw - A.hw) * f)));
        d = Math.max(d, Math.abs(Q.d - (A.d + (B.d - A.d) * f)));
        if (d > dev) { dev = d; farK = k; }
      }
      const tooLong = Math.max(Math.abs(B.x - A.x), Math.abs(B.z - A.z)) > K.riverPieceMax;
      if (b - a > 1 && (tooLong || dev > 1.0)) {
        const mid = (dev <= 1.0 || farK < 0) ? Math.trunc((a + b) / 2) : farK;
        stack.push([mid, b], [a, mid]);
        continue;
      }
      pieces++;
    }
  }
  const line = sm.map(s => [s.x, s.z, s.S, s.hw]);
  const pk = Math.max(1, Math.trunc((N + K.roadProfileMax - 2) / (K.roadProfileMax - 1)));
  const prof = {s: [], y: [], ground: []};
  for (let i = 0; i < N; i += pk) { prof.s.push(sm[i].s); prof.y.push(sm[i].S); prof.ground.push(sm[i].g); }
  if ((N - 1) % pk !== 0) { prof.s.push(sm[N - 1].s); prof.y.push(sm[N - 1].S); prof.ground.push(sm[N - 1].g); }
  const col = key => sm.map(s => s[key]);
  return {
    id: rv.id, joins: rv.joins || null, joinIdx: rv.joinIdx, stepVox: rv.stepVox, bank: rv.bank, profile: rv.profile,
    samples: {x: col('x'), z: col('z'), s: col('s'), hw: col('hw'), d: col('d'), g: col('g'), S: col('S')},
    line, reaches: starts.length, starts, falls, pieces, endLevel: E, drainsTo,
    lakeIn: lakeIn >= 0 ? lakes[lakeIn].id : null, lakeOut: lakeOut >= 0 ? lakes[lakeOut].id : null,
    prof, bankAt, warnings,
  };
}

/* ---- LAKE CONTAINMENT: where the engine's sill raises ground ---------------------------- */
/** Columns in the shore band OUTSIDE a parsed+baked lake whose ground is below
 *  level + 1 (the engine raises them to it: a sill). Sampled every `step`
 *  voxels over the bbox grown by the band; returns [[x, z, raise], ...]. */
export function sillCells(L, ground, step = 16) {
  const out = [], band = L.band;
  const near = (x, z) => {
    for (let i = 0; i < L.poly.length; i++) {
      const a = L.poly[i], b = L.poly[(i + 1) % L.poly.length];
      const ex = b[0] - a[0], ez = b[1] - a[1], L2 = ex * ex + ez * ez;
      const t = L2 > 0 ? clamp(((x - a[0]) * ex + (z - a[1]) * ez) / L2, 0, 1) : 0;
      if (Math.hypot(x - a[0] - ex * t, z - a[1] - ez * t) <= band) return true;
    }
    return false;
  };
  for (let z = Math.floor((L.z0 - band) / step) * step; z <= L.z1 + band; z += step)
    for (let x = Math.floor((L.x0 - band) / step) * step; x <= L.x1 + band; x += step) {
      if (lakeInside(L.poly, x, z) || !near(x, z)) continue;
      const g = ground(x, z);
      if (g < L.level + 1) out.push([x, z, L.level + 1 - g]);
    }
  return out;
}

/* ---- MEANDER: sum-of-sines along the normal ----------------------------------------------- */
/** A meandered copy of a river's knots. The centreline (spline, step w/2) is
 *  pushed along its normal by A(s) * sum_k a_k sin(2 pi s / (lambda_k) + phi_k),
 *  wavelength lambda ~ 12 x the local width, amplitude growing as the slope
 *  (from `ground`) falls: amp = clamp(amount * w * (1.5 - slope / slopeRef), 0,
 *  3 w); the ends are held (a sin^2 taper over one wavelength) so the source
 *  and the mouth (a junction, a lake, the coast) do not move. Resampled into
 *  knots every ~lambda / 4 with w / d interpolated. opts = {amount (0..2,
 *  default 1), seed, ground(x, z) (optional), slopeRef (default 0.02)}. */
export function meander(points, opts = {}) {
  const amount = opts.amount == null ? 1 : +opts.amount, seed = (opts.seed | 0) >>> 0;
  const T = SPL.tessellate(points, {step: 8, smooth: true});
  if (T.pts.length < 3) return points.map(p => p.slice());
  const L = T.length, wOf = p => (p[2] == null ? 16 : p[2]);
  let wMean = 0;
  for (const p of T.pts) wMean += wOf(p);
  wMean /= T.pts.length;
  const lambda = 12 * wMean;
  const rnd = k => { let h = Math.imul(seed ^ 0x9e3779b9, 0x85ebca6b) ^ Math.imul(k + 1, 0xc2b2ae35); h ^= h >>> 13; h = Math.imul(h, 0x27d4eb2d); h ^= h >>> 15; return (h >>> 0) / 4294967296; };
  const comps = [[1, 1, rnd(1) * 6.283], [0.45, 0.62, rnd(2) * 6.283], [0.25, 1.7, rnd(3) * 6.283]];
  const slopeRef = opts.slopeRef || 0.02, g = opts.ground || null;
  // the slope along the river (ground drop per arc length), smoothed over a wavelength
  const N = T.pts.length, slope = new Float64Array(N);
  if (g) {
    const gh = T.pts.map(p => g(p[0], p[1]));
    for (let i = 0; i < N; i++) {
      const a = T.pts.findIndex((_, k) => T.s[k] >= T.s[i] - lambda / 2), b = (() => { let k = i; while (k + 1 < N && T.s[k + 1] <= T.s[i] + lambda / 2) k++; return k; })();
      slope[i] = Math.abs(gh[Math.max(0, a)] - gh[b]) / Math.max(1, T.s[b] - T.s[Math.max(0, a)]);
    }
  }
  const off = new Float64Array(N);
  for (let i = 0; i < N; i++) {
    const s = T.s[i], w = wOf(T.pts[i]);
    const ampBase = amount * w * clamp(1.5 - slope[i] / slopeRef, 0, 1.5);
    const taper = Math.min(1, s / lambda, (L - s) / lambda);
    const tp = Math.sin(Math.PI / 2 * Math.max(0, taper)) ** 2;
    let v = 0;
    for (const [a, f, ph] of comps) v += a * Math.sin(2 * Math.PI * s * f / lambda + ph);
    off[i] = clamp(ampBase * v, -3 * w, 3 * w) * tp;
  }
  const moved = T.pts.map((p, i) => {
    const a = T.pts[Math.max(0, i - 1)], b = T.pts[Math.min(N - 1, i + 1)];
    const dx = b[0] - a[0], dz = b[1] - a[1], l = Math.hypot(dx, dz) || 1;
    return [p[0] - dz / l * off[i], p[1] + dx / l * off[i], p[2], p[3]];
  });
  // knots every ~lambda / 4 of arc, the ends exact
  const stepS = Math.max(16, lambda / 4), out = [];
  let next = 0;
  for (let i = 0; i < N; i++) {
    if (i === 0 || i === N - 1 || T.s[i] >= next) {
      const p = moved[i];
      const k = [Math.round(p[0]), Math.round(p[1])];
      if (i === 0 || i === N - 1) { k.push(Math.round(wOf(p)), Math.round(p[3] == null ? 6 : p[3])); }
      else if (p[2] != null) k.push(Math.round(p[2]), Math.round(p[3] == null ? 6 : p[3]));
      if (i === 0) { k[0] = points[0][0]; k[1] = points[0][1]; }
      if (i === N - 1) { const e = points[points.length - 1]; k[0] = e[0]; k[1] = e[1]; }
      const b = out[out.length - 1];
      if (!b || b[0] !== k[0] || b[1] !== k[1]) out.push(k);
      next = T.s[i] + stepS;
    }
  }
  // the very last knot carries the original mouth's width / depth
  const e = points[points.length - 1], o = points[0];
  if (e.length >= 4) { out[out.length - 1][2] = e[2]; out[out.length - 1][3] = e[3]; }
  if (o.length >= 4) { out[0][2] = o[2]; out[0][3] = o[3]; }
  return out;
}

/* ---- THE TOOLS' pure logic ------------------------------------------------------------- */
/** A lakes[] record for the basin under world column (x, z) on grid G = {w, hh,
 *  x0, z0, cell, h} (x0 / z0 = the column of sample (0, 0)): the Priority-Flood
 *  spill level (or opts.level: re-flooded at that level), the outline of the
 *  4-connected cells below it that hold the clicked one, simplified
 *  (opts.tolerance cells, default 1) -> {id, level, points, smooth: true,
 *  area (vox²), maxDepth} or null when the column drains (no basin / below the
 *  level). opts.seaY makes sea cells outlets. */
export function lakeFromGrid(G, x, z, opts = {}) {
  const {w, hh, cell, h} = G;
  const i = Math.round((x - G.x0) / cell), j = Math.round((z - G.z0) / cell);
  if (i < 0 || j < 0 || i >= w || j >= hh) return null;
  const seed = j * w + i;
  let level;
  if (opts.level != null) level = +opts.level;
  else {
    const filled = opts.filled || priorityFloodLevel(G, opts.seaY);
    level = Math.floor(filled[seed]);
  }
  if (!(h[seed] < level)) return null;
  const m = new Uint8Array(w * hh), q = new Int32Array(w * hh);
  let qh = 0, qt = 0, i0 = w, j0 = hh, i1 = 0, j1 = 0, maxD = 0;
  q[qt++] = seed; m[seed] = 1;
  while (qh < qt) {
    const c = q[qh++], ci = c % w, cj = (c / w) | 0;
    i0 = Math.min(i0, ci); j0 = Math.min(j0, cj); i1 = Math.max(i1, ci); j1 = Math.max(j1, cj);
    maxD = Math.max(maxD, level - h[c]);
    for (const [di, dj] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      const ii = ci + di, jj = cj + dj;
      if (ii < 0 || jj < 0 || ii >= w || jj >= hh) continue;
      const nb = jj * w + ii;
      if (m[nb] || !(h[nb] < level)) continue;
      m[nb] = 1; q[qt++] = nb;
    }
  }
  const bw = i1 - i0 + 1, bh = j1 - j0 + 1, mm = new Uint8Array(bw * bh);
  for (let k = 0; k < qt; k++) { const c = q[k]; mm[(((c / w) | 0) - j0) * bw + (c % w) - i0] = 1; }
  const ring = HYoutline(mm, bw, bh).map(([a, b]) => [a + i0, b + j0]);
  if (ring.length < 3) return null;
  const simp = SPL.simplify([...ring, ring[0]], opts.tolerance == null ? 1 : opts.tolerance).slice(0, -1);
  const pts = (simp.length >= 3 ? simp : ring).map(([a, b]) => [Math.round(G.x0 + (a - 0.5) * cell), Math.round(G.z0 + (b - 0.5) * cell)]);
  return {id: opts.id || 'lake', level: Math.round(level), points: pts, smooth: true, area: qt * cell * cell, maxDepth: maxD, cells: qt};
}
// hydrology.js's priorityFlood / outline, imported lazily so this module stays
// importable alone (the page imports both anyway).
import * as HY from './hydrology.js';
const HYoutline = (m, w, hh) => HY.outline(m, w, hh);
function priorityFloodLevel(G, seaY) { return HY.priorityFlood(G.h, G.w, G.hh, {seaY, epsilon: 0}); }

/** Where a river's MOUTH dropped at (x, z) lands, in priority order: on another
 *  river's centreline within `reach` + its half-width (the river it then
 *  JOINS; never itself or one of its own tributaries), inside a lake polygon
 *  (drains into the lake), on ground under the sea (the coast), else nothing.
 *  -> {kind: 'river' | 'lake' | 'sea' | 'none', x, z, joins, lake}. */
export function snapMouth(json, selfId, x, z, opts = {}) {
  const reach = opts.reach == null ? 48 : opts.reach, sea = json.seaLevelY | 0;
  const rivers = json.rivers || [];
  // rivers that drain INTO selfId (directly or not) cannot be its parent
  const below = new Set([selfId]);
  for (let grew = true; grew;) {
    grew = false;
    for (const r of rivers) if (r.joins && below.has(r.joins) && !below.has(r.id)) { below.add(r.id); grew = true; }
  }
  let best = null;
  for (const r of rivers) {
    if (below.has(r.id) || !Array.isArray(r.points) || r.points.length < 2) continue;
    const t = SPL.tessellate(r.points.map(p => [p[0], p[1], p.length >= 4 ? p[2] : undefined]), {step: 16, smooth: r.smooth === true});
    const c = SPL.closest(t, x, z);
    if (!c) continue;
    const hw = (c.attrs[0] == null ? 16 : c.attrs[0]) / 2;
    if (c.dist <= reach + hw && (!best || c.dist < best.dist)) best = {dist: c.dist, kind: 'river', x: Math.round(c.x), z: Math.round(c.z), joins: r.id, lake: null};
  }
  if (best) { delete best.dist; return best; }
  for (const l of json.lakes || []) {
    const p = parseLake(l);
    if (p.error) continue;
    const L = lakePoly(p);
    if (lakeInside(L.poly, Math.round(x), Math.round(z))) return {kind: 'lake', x: Math.round(x), z: Math.round(z), joins: null, lake: l.id};
  }
  if (opts.ground && opts.ground(x, z) < sea) return {kind: 'sea', x: Math.round(x), z: Math.round(z), joins: null, lake: null};
  return {kind: 'none', x: Math.round(x), z: Math.round(z), joins: null, lake: null};
}

/* ---- DERIVE: hydrology.rivers() output -> rivers[] records ------------------------------ */
/** The ORDER of each derived river in its network: 1 = no tributaries,
 *  else 1 + the highest order among the rivers that join it (a hierarchy
 *  depth: the trunk of a big network has the highest order). */
export function riverOrders(list) {
  const kids = new Map(), ord = new Map();
  for (const r of list) if (r.joins) { if (!kids.has(r.joins)) kids.set(r.joins, []); kids.get(r.joins).push(r.id); }
  const of = (id, depth = 0) => {
    if (ord.has(id)) return ord.get(id);
    if (depth > list.length) return 1;
    const ks = (kids.get(id) || []).map(k => of(k, depth + 1));
    const o = ks.length ? Math.max(...ks) + 1 : 1;
    ord.set(id, o);
    return o;
  };
  for (const r of list) of(r.id);
  return ord;
}
/** Grid-unit derived rivers (HY.rivers on grid G = {x0, z0, cell}) -> world
 *  river records {id, points: [[x, z, w, d], ...], smooth: true, joins, order,
 *  areaKm2}. A grid point (i, j) (cell centres at i + 0.5) is world
 *  x0 + (i - 0.5) * cell (the grid's x0 is the CENTRE of cell 0). Width from
 *  the catchment at each point: w = widthK * sqrt(A km^2) metres (widthK
 *  default 2.5: 1 km^2 -> 2.5 m, 100 km^2 -> 25 m), clamped to the loader's
 *  8..1024 voxels; depth d = clamp(w / 6, 2, 40) voxels. */
export function derivedToRecords(list, G, opts = {}) {
  const ord = riverOrders(list), prefix = opts.prefix || 'river_d', taken = new Set(opts.taken || []);
  const vpm = opts.vpm || 10, widthK = opts.widthK == null ? 2.5 : +opts.widthK;
  const idOf = new Map();
  let n = 0;
  for (const r of list) { let id; do { id = prefix + (n++); } while (taken.has(id)); idOf.set(r.id, id); }
  const cellKm2 = (G.cell / vpm) * (G.cell / vpm) / 1e6;
  return list.map(r => {
    const line = r.line || r.points;
    const pts = line.map(p => {
      const x = Math.round(G.x0 + (p[0] - 0.5) * G.cell), z = Math.round(G.z0 + (p[1] - 0.5) * G.cell);
      const w = Math.round(clamp(widthK * Math.sqrt(Math.max(0, p[2] || 0) * cellKm2) * vpm, K.widthMin, K.widthMax));
      return [x, z, w, Math.round(clamp(w / 6, K.depthMin, 40))];
    });
    const dd = [];
    for (const p of pts) { const b = dd[dd.length - 1]; if (!b || b[0] !== p[0] || b[1] !== p[1]) dd.push(p); }
    return {id: idOf.get(r.id), points: dd, smooth: true, joins: r.joins ? idOf.get(r.joins) || null : null,
            order: ord.get(r.id) || 1, areaKm2: (r.A || 0) * cellKm2, from: r.id};
  });
}

