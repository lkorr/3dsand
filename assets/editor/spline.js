/* spline.js — THE spline of the world map (docs/PLAN_worldbuilder.md §2,
 * K1 item 2, aligned to the ENGINE by E2a): pure, DOM-free, Node-runnable.
 * Roads with `smooth: true`, rivers, lakes' smooth outlines and the editor's
 * mountain ranges all go through tessellate() below, and the engine's
 * `worldmap::SplineTessellate` / `PolylineResample` (src/sim/worldwater.cpp)
 * is the same function in C++. The reference implementation is
 * scripts/gen_spline_vectors.py, which writes tests/spline_vectors.json; the
 * `rivers` selftest gate holds the C++ to that table and
 * scripts/test_spline.mjs holds this file to it (within 2e-3).
 *
 * THE CONVENTION (the engine's; do not change one side alone):
 *
 *  1. Knots are [x, z, ...attrs] in world voxels. Consecutive knots with the
 *     SAME (x, z) are merged here (the first one's attributes kept) -- the
 *     engine's parsers drop consecutive duplicates the same way. One knot
 *     left: the output is that point. Zero: empty.
 *  2. Open curves get phantom ends by REFLECTION: P[-1] = 2 P[0] - P[1],
 *     P[n] = 2 P[n-1] - P[n-2]. Closed curves wrap their controls.
 *  3. CENTRIPETAL Catmull-Rom (alpha 0.5), Barry-Goldman evaluation: per
 *     segment P1 -> P2 (P0..P3 its four controls) t0 = 0, t1 = sqrt(|P1-P0|),
 *     t2 = t1 + sqrt(|P2-P1|), t3 = t2 + sqrt(|P3-P2|), each distance floored
 *     at 1e-4.
 *  4. DENSE: the segment at nd = clamp(ceil(|P2-P1| / 2), 8, 4096) uniform
 *     steps of t over [t1, t2] (the ends exactly the knots). `smooth: false` =
 *     the straight segment (dense = [P1, P2]).
 *  5. RESAMPLE PER SEGMENT: L = the dense chords' length, m = max(1,
 *     ceil(L / step)); the segment emits P1 then the m - 1 interior points at
 *     arc L k / m (linear on the dense chords), f = k / m. An open curve ends
 *     with P[n-1] exactly (seg n-2, f 1); a closed one does not repeat P0.
 *  6. ATTRIBUTES (w, d, crestY, ...: anything after x, z), as the river bake
 *     reads them: a knot's arc `knotS` is the cumulative CHORD length of the
 *     OUTPUT polyline at its first point; a knot missing a value takes the
 *     linear interpolation by knotS between the nearest knots that have one
 *     (held flat past the ends); an output point lerps its segment's two
 *     knots by f.
 *
 * Editor-only extension, NOT in the engine: a knot may carry Bezier HANDLES
 * (opts.handles[k] = {in: [dx, dz] | null, out: [dx, dz] | null}). A segment
 * touching a handle is evaluated as the cubic Bezier of the same curve
 * (Hermite form: c1 = P1 + m1 / 3, c2 = P2 - m2 / 3) with the override
 * replacing c1 (out of knot k) or c2 (into knot k+1), densified at the same
 * nd. Only editor-only splines (ranges[]) may use them; roads and rivers are
 * read by the engine, which has no handles. */

const sqrt = Math.sqrt;
const dist = (a, b) => sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]));

/** Rule 1: merge consecutive duplicate (x, z). Returns {pts, map} where
 *  map[i] is the original index of kept knot i. */
export function dedupe(points) {
  const pts = [], map = [];
  (points || []).forEach((p, i) => {
    if (!Array.isArray(p) || p.length < 2 || !isFinite(p[0]) || !isFinite(p[1])) return;
    const q = pts[pts.length - 1];
    if (q && q[0] === p[0] && q[1] === p[1]) return;
    pts.push(p); map.push(i);
  });
  return {pts, map};
}

/** Barry-Goldman pyramid at t (rule 3). */
export function bgEval(p0, p1, p2, p3, t0, t1, t2, t3, t) {
  const out = [0, 0];
  for (let c = 0; c < 2; c++) {
    const a1 = (t1 - t) / (t1 - t0) * p0[c] + (t - t0) / (t1 - t0) * p1[c];
    const a2 = (t2 - t) / (t2 - t1) * p1[c] + (t - t1) / (t2 - t1) * p2[c];
    const a3 = (t3 - t) / (t3 - t2) * p2[c] + (t - t2) / (t3 - t2) * p3[c];
    const b1 = (t2 - t) / (t2 - t0) * a1 + (t - t0) / (t2 - t0) * a2;
    const b2 = (t3 - t) / (t3 - t1) * a2 + (t - t1) / (t3 - t1) * a3;
    out[c] = (t2 - t) / (t2 - t1) * b1 + (t - t1) / (t2 - t1) * b2;
  }
  return out;
}

/** Point of a cubic Bezier [c0, c1, c2, c3] at u. */
export function bezier(seg, u) {
  const [a, b, c, d] = seg, v = 1 - u;
  const w0 = v * v * v, w1 = 3 * v * v * u, w2 = 3 * v * u * u, w3 = u * u * u;
  return [w0 * a[0] + w1 * b[0] + w2 * c[0] + w3 * d[0], w0 * a[1] + w1 * b[1] + w2 * c[1] + w3 * d[1]];
}

/** The four controls of every segment: [[p0, p1, p2, p3], ...]. `pts` deduped. */
export function controls(pts, closed = false) {
  const n = pts.length, out = [];
  if (n < 2) return out;
  const P = k => closed ? [pts[((k % n) + n) % n][0], pts[((k % n) + n) % n][1]]
    : k < 0 ? [2 * pts[0][0] - pts[1][0], 2 * pts[0][1] - pts[1][1]]
    : k >= n ? [2 * pts[n - 1][0] - pts[n - 2][0], 2 * pts[n - 1][1] - pts[n - 2][1]]
    : [pts[k][0], pts[k][1]];
  const nseg = closed ? n : n - 1;
  for (let k = 0; k < nseg; k++) out.push([P(k - 1), P(k), P(k + 1), P(k + 2)]);
  return out;
}

/** The Bezier form [c0, c1, c2, c3] of every segment (the same curve as the
 *  Barry-Goldman evaluation; for handles and drawing). */
export function segments(pts, smooth = true, handles = null, mapIdx = null, closed = false) {
  const n = pts.length;
  return controls(pts, closed).map(([p0, p1, p2, p3], k) => {
    let c1, c2;
    if (!smooth) { c1 = p1; c2 = p2; }
    else {
      const t0 = 0, t1 = sqrt(Math.max(dist(p0, p1), 1e-4)), t2 = t1 + sqrt(Math.max(dist(p1, p2), 1e-4)),
        t3 = t2 + sqrt(Math.max(dist(p2, p3), 1e-4));
      const m = i => {
        const a = (p1[i] - p0[i]) / (t1 - t0) - (p2[i] - p0[i]) / (t2 - t0) + (p2[i] - p1[i]) / (t2 - t1);
        const b = (p2[i] - p1[i]) / (t2 - t1) - (p3[i] - p1[i]) / (t3 - t1) + (p3[i] - p2[i]) / (t3 - t2);
        return [(t2 - t1) * a, (t2 - t1) * b];
      };
      const mx = m(0), mz = m(1);
      c1 = [p1[0] + mx[0] / 3, p1[1] + mz[0] / 3];
      c2 = [p2[0] - mx[1] / 3, p2[1] - mz[1] / 3];
    }
    if (handles) {
      const ka = mapIdx ? mapIdx[k] : k, kb = mapIdx ? mapIdx[(k + 1) % n] : (k + 1) % n;
      const ha = handles[ka], hb = handles[kb];
      if (ha && Array.isArray(ha.out)) c1 = [p1[0] + ha.out[0], p1[1] + ha.out[1]];
      if (hb && Array.isArray(hb.in)) c2 = [p2[0] + hb.in[0], p2[1] + hb.in[1]];
    }
    return [p1, c1, c2, p2];
  });
}

/** Rule 4: one segment's dense polyline. */
function denseSegment(ctl, smooth, bez) {
  const [p0, p1, p2, p3] = ctl;
  if (!smooth && !bez) return [p1, p2];
  const d12 = Math.max(dist(p1, p2), 1e-4);
  const nd = Math.min(Math.max(Math.ceil(d12 / 2), 8), 4096);
  const out = [p1];
  if (bez) {
    for (let k = 1; k < nd; k++) out.push(bezier(bez, k / nd));
  } else {
    const t0 = 0, t1 = sqrt(Math.max(dist(p0, p1), 1e-4)), t2 = t1 + sqrt(d12), t3 = t2 + sqrt(Math.max(dist(p2, p3), 1e-4));
    for (let k = 1; k < nd; k++) out.push(bgEval(p0, p1, p2, p3, t0, t1, t2, t3, t1 + (t2 - t1) * k / nd));
  }
  out.push(p2);
  return out;
}

/** Rules 3-5: [{x, z, seg, f}] -- exactly the engine's SplinePt list. */
export function tessellateRaw(pts, {step = 16, smooth = true, closed = false, handles = null, map = null} = {}) {
  const n = pts.length, out = [];
  if (!n) return out;
  if (n === 1) return [{x: pts[0][0], z: pts[0][1], seg: 0, f: 0}];
  const ctl = controls(pts, closed);
  const bz = handles ? segments(pts, smooth, handles, map, closed) : null;
  ctl.forEach((c, i) => {
    let bez = null;
    if (bz) {
      const ka = map ? map[i] : i, kb = map ? map[(i + 1) % n] : (i + 1) % n;
      if ((handles[ka] && Array.isArray(handles[ka].out)) || (handles[kb] && Array.isArray(handles[kb].in))) bez = bz[i];
    }
    const D = denseSegment(c, smooth, bez);
    const cum = [0];
    for (let k = 1; k < D.length; k++) cum.push(cum[k - 1] + dist(D[k - 1], D[k]));
    const L = cum[cum.length - 1];
    const m = Math.max(1, Math.ceil(L / step));
    out.push({x: D[0][0], z: D[0][1], seg: i, f: 0});
    let j = 0;
    for (let k = 1; k < m; k++) {
      const s = L * k / m;
      while (j + 2 < D.length && cum[j + 1] < s) j++;
      const span = cum[j + 1] - cum[j], f = span > 0 ? (s - cum[j]) / span : 0;
      const a = D[j], b = D[j + 1];
      out.push({x: a[0] + (b[0] - a[0]) * f, z: a[1] + (b[1] - a[1]) * f, seg: i, f: k / m});
    }
  });
  if (!closed) out.push({x: pts[n - 1][0], z: pts[n - 1][1], seg: n - 2, f: 1});
  return out;
}

/** Rule 6: every knot's attribute values, missing ones filled by knotS.
 *  rows[k] = [a0, a1, ...] (undefined where no knot carries that attribute). */
export function fillAttrs(pts, knotS) {
  const na = Math.max(0, ...pts.map(p => p.length - 2));
  const rows = pts.map(() => new Array(na));
  for (let a = 0; a < na; a++) {
    const def = [];
    pts.forEach((p, k) => { const v = p[2 + a]; if (v != null && isFinite(v)) def.push(k); });
    if (!def.length) continue;
    for (let k = 0; k < pts.length; k++) {
      const v = pts[k][2 + a];
      if (v != null && isFinite(v)) { rows[k][a] = +v; continue; }
      let lo = -1, hi = -1;
      for (const d of def) { if (d < k) lo = d; else if (hi < 0) hi = d; }
      if (lo < 0) rows[k][a] = +pts[hi][2 + a];
      else if (hi < 0) rows[k][a] = +pts[lo][2 + a];
      else {
        const s0 = knotS[lo], s1 = knotS[hi], f = s1 > s0 ? (knotS[k] - s0) / (s1 - s0) : 0;
        rows[k][a] = +pts[lo][2 + a] + (+pts[hi][2 + a] - +pts[lo][2 + a]) * f;
      }
    }
  }
  return rows;
}

/** THE TESSELLATION (rules 1-6). opts = {step (vox, default 16), smooth
 *  (default true), closed (default false), handles (editor-only)}. Returns
 *    {pts: [[x, z, ...attrs]], s: [arc length = cumulative chord], knotS,
 *     knots: the kept knots, length, seg: [segment of each point], f: [its arc
 *     fraction in the segment], closed}.
 *  A closed curve's `length` includes the closing chord back to pts[0]. */
export function tessellate(points, opts = {}) {
  const step = Math.max(1e-6, opts.step == null ? 16 : +opts.step);
  const smooth = opts.smooth !== false, closed = !!opts.closed;
  const {pts, map} = dedupe(points);
  if (!pts.length) return {pts: [], s: [], knotS: [], knots: [], length: 0, seg: [], f: [], closed};
  if (pts.length === 1) {
    const at = fillAttrs(pts, [0])[0];
    return {pts: [[pts[0][0], pts[0][1], ...at]], s: [0], knotS: [0], knots: pts, length: 0, seg: [0], f: [0], closed};
  }
  const raw = tessellateRaw(pts, {step, smooth, closed, handles: opts.handles || null, map});
  const s = [0];
  for (let i = 1; i < raw.length; i++) s.push(s[i - 1] + Math.hypot(raw[i].x - raw[i - 1].x, raw[i].z - raw[i - 1].z));
  const n = pts.length;
  const knotS = new Array(n).fill(0);
  for (let i = 0; i < raw.length; i++) if (raw[i].f === 0 && raw[i].seg < n) knotS[raw[i].seg] = s[i];
  let length = s[s.length - 1];
  if (closed) length += Math.hypot(raw[0].x - raw[raw.length - 1].x, raw[0].z - raw[raw.length - 1].z);
  else knotS[n - 1] = length;
  const attrs = fillAttrs(pts, knotS);
  const na = attrs[0].length;
  const out = raw.map(q => {
    const k0 = Math.min(Math.max(q.seg, 0), n - 1), k1 = closed ? (k0 + 1) % n : Math.min(k0 + 1, n - 1);
    const row = [q.x, q.z];
    for (let a = 0; a < na; a++) {
      const va = attrs[k0][a], vb = attrs[k1][a];
      row.push(va == null ? undefined : va + (vb - va) * q.f);
    }
    return row;
  });
  return {pts: out, s, knotS, knots: pts, length, seg: raw.map(q => q.seg), f: raw.map(q => q.f), closed};
}

/** Arc length of a tessellation (or of raw knots, tessellated). */
export function arcLength(t, opts) { return (t && t.pts ? t : tessellate(t, opts)).length; }

/** Point (and attributes) at arc length s along a tessellation, linear between output points. */
export function pointAt(t, s) {
  const P = t.pts;
  if (!P.length) return null;
  const last = t.s[t.s.length - 1];
  if (P.length === 1 || s <= 0) return P[0].slice();
  if (s >= last) return P[P.length - 1].slice();
  let lo = 0, hi = P.length - 1;
  while (hi - lo > 1) { const m = (lo + hi) >> 1; if (t.s[m] <= s) lo = m; else hi = m; }
  const f = (s - t.s[lo]) / Math.max(1e-12, t.s[hi] - t.s[lo]);
  return P[lo].map((v, q) => (v == null || P[hi][q] == null ? v : v + (P[hi][q] - v) * f));
}

/** The closest point of the tessellated curve to (x, z):
 *  {s, i (output piece), u (0..1 along it), seg (knot segment), t (= seg + the
 *  arc-length fraction within it), x, z, dist, offset (signed: + = RIGHT of the
 *  direction of travel, x east / z south), attrs (interpolated)}. */
export function closest(t, x, z) {
  const P = t.pts;
  if (!P.length) return null;
  if (P.length === 1) return {s: 0, i: 0, u: 0, seg: 0, t: 0, x: P[0][0], z: P[0][1], dist: Math.hypot(x - P[0][0], z - P[0][1]), offset: 0, attrs: P[0].slice(2)};
  let best = null, bd = Infinity;
  for (let i = 0; i + 1 < P.length; i++) {
    const a = P[i], b = P[i + 1], ex = b[0] - a[0], ez = b[1] - a[1], L2 = ex * ex + ez * ez;
    let u = L2 > 0 ? ((x - a[0]) * ex + (z - a[1]) * ez) / L2 : 0;
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    const cx = a[0] + ex * u, cz = a[1] + ez * u, d2 = (x - cx) ** 2 + (z - cz) ** 2;
    if (d2 < bd) { bd = d2; best = {i, u, cx, cz, ex, ez}; }
  }
  const {i, u, cx, cz, ex, ez} = best;
  const s = t.s[i] + (t.s[i + 1] - t.s[i]) * u;
  const len = Math.hypot(ex, ez) || 1;
  const offset = (ex * (z - cz) - ez * (x - cx)) / len;
  const seg = t.seg[i];
  const ka = t.knotS[seg], kb = t.knotS[seg + 1] == null ? ka : t.knotS[seg + 1];
  const attrs = P[i].slice(2).map((v, q) => (v == null ? v : v + (P[i + 1][2 + q] - v) * u));
  return {s, i, u, seg, t: seg + (kb > ka ? Math.min(1, (s - ka) / (kb - ka)) : 0), x: cx, z: cz, dist: Math.sqrt(bd), offset, attrs};
}

/** Knots with a new one inserted ON the curve at arc length s (its attributes
 *  interpolated); returns {points, index}. The curve through the new knot set
 *  passes the same place but is not identical elsewhere (CR is local: only the
 *  neighbouring segments move). */
export function insertKnot(points, s, opts = {}) {
  const t = tessellate(points, opts);
  if (t.knots.length < 2) return {points: points.slice(), index: points.length};
  const p = pointAt(t, s);
  let k = 0;
  while (k + 1 < t.knotS.length && t.knotS[k + 1] <= s) k++;
  const {map} = dedupe(points);
  const at = map[Math.min(k, map.length - 1)] + 1;
  const np = points.slice();
  np.splice(at, 0, p.map((v, q) => (q < 2 ? Math.round(v) : v == null ? v : Math.round(v * 1000) / 1000)));
  return {points: np, index: at};
}
/** Knots without knot i. */
export function deleteKnot(points, i) { const np = points.slice(); np.splice(i, 1); return np; }

/** Arc-length positions of the ORIGINAL knots (dropped duplicates get their
 *  kept twin's), for drawing handles. */
export function knotPositions(points, opts = {}) {
  const t = tessellate(points, opts), {map} = dedupe(points), out = new Array((points || []).length).fill(0);
  let k = 0;
  for (let i = 0; i < out.length; i++) { while (k + 1 < map.length && map[k + 1] <= i) k++; out[i] = t.knotS[k] || 0; }
  return out;
}

/** The engine's integer centreline of a road / river: the tessellation (step
 *  16) rounded to integer voxels, consecutive duplicates dropped (RoundDedupe). */
export function roundDedupe(t) {
  const out = [];
  for (const p of t.pts) {
    const v = [Math.round(p[0]), Math.round(p[1])];
    const q = out[out.length - 1];
    if (q && q[0] === v[0] && q[1] === v[1]) continue;
    out.push(v);
  }
  return out;
}

/** Douglas-Peucker on integer points, open polyline (the engine's Simplify). */
export function simplify(p, tol) {
  if (p.length < 3) return p.slice();
  const keep = new Uint8Array(p.length);
  keep[0] = keep[p.length - 1] = 1;
  const st = [[0, p.length - 1]];
  while (st.length) {
    const [a, b] = st.pop();
    if (b <= a + 1) continue;
    const ax = p[a][0], az = p[a][1], vx = p[b][0] - ax, vz = p[b][1] - az, vl = Math.sqrt(vx * vx + vz * vz);
    let best = -1, bi = a;
    for (let k = a + 1; k < b; k++) {
      const qx = p[k][0] - ax, qz = p[k][1] - az;
      const d = vl > 0 ? Math.abs(qx * vz - qz * vx) / vl : Math.sqrt(qx * qx + qz * qz);
      if (d > best) { best = d; bi = k; }
    }
    if (best > tol) { keep[bi] = 1; st.push([a, bi], [bi, b]); }
  }
  return p.filter((_, i) => keep[i]);
}

/** The engine's SmoothRoadPoints: what a `smooth: true` road's polyline is
 *  before the profile bake. */
export function smoothRoadPoints(points) {
  return simplify(roundDedupe(tessellate(points.map(p => [p[0], p[1]]), {step: 16, smooth: true})), 0.75);
}

/** Round every coordinate to 3 decimals: the test-vector precision. */
export const round3 = v => (v == null ? null : Math.round(v * 1000) / 1000);
