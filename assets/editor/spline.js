/* spline.js — THE spline of the world map (docs/PLAN_worldbuilder.md §2,
 * package K1 item 2): pure, DOM-free, Node-runnable. Roads with `smooth:
 * true`, rivers, lakes' smooth outlines and the editor's mountain ranges all go
 * through tessellate() below, and the ENGINE implements the same function in
 * C++ (package W1). tests/spline_vectors.json pins both to one table.
 *
 * THE CONVENTION (the C++ twin must do exactly this, in doubles):
 *
 *  1. Knots are [x, z, ...attrs] in world voxels. Consecutive knots with the
 *     SAME (x, z) are merged (the first one's attributes kept). One knot left:
 *     the output is that point. Zero: empty.
 *  2. Phantom end knots by REFLECTION: P[-1] = 2*P[0] - P[1],
 *     P[n] = 2*P[n-1] - P[n-2].
 *  3. CENTRIPETAL Catmull-Rom (alpha 0.5) through P[0..n-1]. Knot "times":
 *     t[k+1] = t[k] + sqrt(|P[k+1] - P[k]|) (the square root of the Euclidean
 *     distance, i.e. sqrt(sqrt(dx*dx + dz*dz))). Segment k runs P[k] -> P[k+1]
 *     as the cubic Bezier
 *        c0 = P1, c1 = P1 + m1 / 3, c2 = P2 - m2 / 3, c3 = P2
 *     with (P0..P3 = P[k-1..k+2], t0..t3 their times)
 *        m1 = (t2-t1) * ((P1-P0)/(t1-t0) - (P2-P0)/(t2-t0) + (P2-P1)/(t2-t1))
 *        m2 = (t2-t1) * ((P2-P1)/(t2-t1) - (P3-P1)/(t3-t1) + (P3-P2)/(t3-t2))
 *     (the Hermite form of the Barry-Goldman pyramid; identical curve).
 *     `smooth: false` = the straight segment (c1 = c0, c2 = c3).
 *  4. Each segment is evaluated at u = j / SUB, j = 0 .. SUB-1 (SUB = 32), plus
 *     the very last knot: the DENSE polyline. Arc length is measured along it.
 *  5. RESAMPLING: L = the dense polyline's length, N = max(1, ceil(L / step - 1e-9)),
 *     the output is N + 1 points at arc length i * L / N (i = 0..N), linearly
 *     interpolated on the dense polyline -- uniform spacing of at most `step`,
 *     both ends exact. (step 16 vox in the test vectors.)
 *  6. ATTRIBUTES (w, d, crestY, ...: anything after x, z): a knot whose value
 *     is missing (undefined / null) takes the linear interpolation, by the
 *     knots' arc length, between the nearest knots that have one (held flat
 *     past the last one). An output point takes the linear interpolation
 *     between its segment's two knots by arc-length fraction within the
 *     segment.
 *
 * Editor-only extension, NOT in the engine: a knot may carry Bezier HANDLES
 * (opts.handles[k] = {in: [dx, dz] | null, out: [dx, dz] | null}); an
 * override replaces c1 (out of knot k) or c2 (into knot k+1). Only editor-only
 * splines (ranges[]) may use them; roads and rivers are read by the engine,
 * which has no handles. */

export const SUB = 32;

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

/** The Bezier control points of every segment: [[c0, c1, c2, c3], ...]
 *  (each c an [x, z]). `pts` must be deduped. */
export function segments(pts, smooth = true, handles = null, mapIdx = null) {
  const n = pts.length, out = [];
  if (n < 2) return out;
  const P = k => (k < 0 ? [2 * pts[0][0] - pts[1][0], 2 * pts[0][1] - pts[1][1]]
                : k >= n ? [2 * pts[n - 1][0] - pts[n - 2][0], 2 * pts[n - 1][1] - pts[n - 2][1]]
                : [pts[k][0], pts[k][1]]);
  for (let k = 0; k + 1 < n; k++) {
    const p0 = P(k - 1), p1 = P(k), p2 = P(k + 1), p3 = P(k + 2);
    let c1, c2;
    if (!smooth) { c1 = p1; c2 = p2; }
    else {
      const t0 = 0, t1 = t0 + sqrt(dist(p0, p1)), t2 = t1 + sqrt(dist(p1, p2)), t3 = t2 + sqrt(dist(p2, p3));
      const m = (i) => {
        const a = (p1[i] - p0[i]) / (t1 - t0) - (p2[i] - p0[i]) / (t2 - t0) + (p2[i] - p1[i]) / (t2 - t1);
        const b = (p2[i] - p1[i]) / (t2 - t1) - (p3[i] - p1[i]) / (t3 - t1) + (p3[i] - p2[i]) / (t3 - t2);
        return [(t2 - t1) * a, (t2 - t1) * b];
      };
      const mx = m(0), mz = m(1);
      c1 = [p1[0] + mx[0] / 3, p1[1] + mz[0] / 3];
      c2 = [p2[0] - mx[1] / 3, p2[1] - mz[1] / 3];
    }
    if (handles) {
      const ha = handles[mapIdx ? mapIdx[k] : k], hb = handles[mapIdx ? mapIdx[k + 1] : k + 1];
      if (ha && Array.isArray(ha.out)) c1 = [p1[0] + ha.out[0], p1[1] + ha.out[1]];
      if (hb && Array.isArray(hb.in)) c2 = [p2[0] + hb.in[0], p2[1] + hb.in[1]];
    }
    out.push([p1, c1, c2, p2]);
  }
  return out;
}

/** Point of a Bezier segment at u. */
export function bezier(seg, u) {
  const [a, b, c, d] = seg, v = 1 - u;
  const w0 = v * v * v, w1 = 3 * v * v * u, w2 = 3 * v * u * u, w3 = u * u * u;
  return [w0 * a[0] + w1 * b[0] + w2 * c[0] + w3 * d[0], w0 * a[1] + w1 * b[1] + w2 * c[1] + w3 * d[1]];
}

/** Rule 4: the dense polyline and its cumulative arc length. */
export function dense(segs) {
  const xs = [], zs = [], cum = [];
  let L = 0;
  for (const seg of segs)
    for (let j = 0; j < SUB; j++) {
      const p = bezier(seg, j / SUB);
      if (xs.length) L += sqrt((p[0] - xs[xs.length - 1]) ** 2 + (p[1] - zs[zs.length - 1]) ** 2);
      xs.push(p[0]); zs.push(p[1]); cum.push(L);
    }
  if (segs.length) {
    const e = segs[segs.length - 1][3];
    L += sqrt((e[0] - xs[xs.length - 1]) ** 2 + (e[1] - zs[zs.length - 1]) ** 2);
    xs.push(e[0]); zs.push(e[1]); cum.push(L);
  }
  return {xs, zs, cum, length: L};
}

/** Rule 6: every knot's attribute values, missing ones filled. Returns
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
 *  (default true), handles (editor-only)}. Returns
 *    {pts: [[x, z, ...attrs]], s: [arc length], knotS: [arc length of each kept
 *     knot], knots: the kept knots, length, dense, segs, seg: [segment of each
 *     output point]}. */
export function tessellate(points, opts = {}) {
  const step = Math.max(1e-6, opts.step == null ? 16 : +opts.step);
  const smooth = opts.smooth !== false;
  const {pts, map} = dedupe(points);
  if (!pts.length) return {pts: [], s: [], knotS: [], knots: [], length: 0, dense: null, segs: [], seg: []};
  if (pts.length === 1) {
    const at = fillAttrs(pts, [0])[0];
    return {pts: [[pts[0][0], pts[0][1], ...at]], s: [0], knotS: [0], knots: pts, length: 0, dense: null, segs: [], seg: [0]};
  }
  const segs = segments(pts, smooth, opts.handles || null, map);
  const D = dense(segs);
  const knotS = pts.map((_, k) => D.cum[Math.min(k * SUB, D.cum.length - 1)]);
  const attrs = fillAttrs(pts, knotS);
  const L = D.length, N = Math.max(1, Math.ceil(L / step - 1e-9));
  const out = [], sOut = [], segOut = [];
  let a = 0;
  for (let i = 0; i <= N; i++) {
    const s = i === N ? L : i * L / N;
    while (a + 1 < D.cum.length - 1 && D.cum[a + 1] < s) a++;
    const s0 = D.cum[a], s1 = D.cum[a + 1], f = s1 > s0 ? Math.min(1, Math.max(0, (s - s0) / (s1 - s0))) : 0;
    let x = D.xs[a] + (D.xs[a + 1] - D.xs[a]) * f, z = D.zs[a] + (D.zs[a + 1] - D.zs[a]) * f;
    if (i === N) { x = D.xs[D.xs.length - 1]; z = D.zs[D.zs.length - 1]; }
    const k = Math.min(segs.length - 1, Math.floor(a / SUB));
    const ka = knotS[k], kb = knotS[k + 1], g = kb > ka ? Math.min(1, Math.max(0, (s - ka) / (kb - ka))) : 0;
    const row = [x, z];
    for (let q = 0; q < attrs[0].length; q++) {
      const va = attrs[k][q], vb = attrs[k + 1][q];
      row.push(va == null ? undefined : va + (vb - va) * g);
    }
    out.push(row); sOut.push(s); segOut.push(k);
  }
  return {pts: out, s: sOut, knotS, knots: pts, length: L, dense: D, segs, seg: segOut};
}

/** Arc length of a tessellation (or of raw knots, tessellated). */
export function arcLength(t, opts) { return (t && t.pts ? t : tessellate(t, opts)).length; }

/** Point (and attributes) at arc length s along a tessellation, linear between output points. */
export function pointAt(t, s) {
  const P = t.pts;
  if (!P.length) return null;
  if (P.length === 1 || s <= 0) return P[0].slice();
  if (s >= t.length) return P[P.length - 1].slice();
  let lo = 0, hi = P.length - 1;
  while (hi - lo > 1) { const m = (lo + hi) >> 1; if (t.s[m] <= s) lo = m; else hi = m; }
  const f = (s - t.s[lo]) / Math.max(1e-12, t.s[hi] - t.s[lo]);
  return P[lo].map((v, q) => (v == null || P[hi][q] == null ? v : v + (P[hi][q] - v) * f));
}

/** The closest point of the tessellated curve to (x, z):
 *  {s, i (output piece), u (0..1 along it), seg (knot segment), t (= seg + the
 *  arc-length fraction within it: the knot-space parameter), x, z, dist,
 *  offset (signed: + = RIGHT of the direction of travel, x east / z south),
 *  attrs (interpolated)}. */
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
  return {s, i, u, seg, t: seg + (kb > ka ? (s - ka) / (kb - ka) : 0), x: cx, z: cz, dist: Math.sqrt(bd), offset, attrs};
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
  // index into the ORIGINAL list: after the original of kept knot k
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

/** Round every coordinate to 3 decimals: the test-vector precision. */
export const round3 = v => (v == null ? null : Math.round(v * 1000) / 1000);
