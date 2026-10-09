// worldlib.mjs — grid helpers for scripts/design_world.mjs (pure, Node):
// polygons (point-in-polygon, signed distance, warped rasters), soft zone
// blending, profile carves along polylines, an A* router for roads, and the
// overview renderer (hillshade + biome + water + vectors) the design is
// LOOKED at with. Everything is deterministic (no Math.random).
import {pathToFileURL, fileURLToPath} from 'node:url';
import {join, dirname} from 'node:path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const imp = p => import(pathToFileURL(join(ROOT, p)).href);
const TG = await imp('assets/editor/terraingen.js');
const HY = await imp('assets/editor/hydrology.js');
const SPL = await imp('assets/editor/spline.js');

export const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
export const lerp = (a, b, t) => a + (b - a) * t;
export const sstep = TG.smoothstep;

/* ---- polygons ------------------------------------------------------------------ */
export function pip(poly, x, z) {
  let c = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const a = poly[i], b = poly[j];
    if ((a[1] > z) !== (b[1] > z) && x < (b[0] - a[0]) * (z - a[1]) / (b[1] - a[1]) + a[0]) c = !c;
  }
  return c;
}
function segD2(px, pz, ax, az, bx, bz) {
  const ex = bx - ax, ez = bz - az, L2 = ex * ex + ez * ez;
  let t = L2 > 0 ? ((px - ax) * ex + (pz - az) * ez) / L2 : 0;
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  const dx = px - ax - ex * t, dz = pz - az - ez * t;
  return dx * dx + dz * dz;
}
/** Signed distance (voxels, + inside any polygon) on grid G to a set of
 *  closed polygons (union). Brute force over edges with a per-row bbox cull. */
export function sdf(G, polys) {
  const N = G.w * G.hh, out = new Float32Array(N);
  const edges = [];
  for (const P of polys) for (let i = 0; i < P.length; i++) { const a = P[i], b = P[(i + 1) % P.length]; edges.push([a[0], a[1], b[0], b[1]]); }
  for (let j = 0; j < G.hh; j++) {
    const z = G.z0 + j * G.cell;
    // the row's crossings (even-odd over the union = per polygon parity OR)
    const ins = new Uint8Array(G.w);
    for (const P of polys) {
      const xs = [];
      for (let i = 0, k = P.length - 1; i < P.length; k = i++) {
        const a = P[i], b = P[k];
        if ((a[1] > z) !== (b[1] > z)) xs.push(a[0] + (z - a[1]) / (b[1] - a[1]) * (b[0] - a[0]));
      }
      xs.sort((p, q) => p - q);
      for (let q = 0; q + 1 < xs.length; q += 2) {
        const i0 = Math.max(0, Math.ceil((xs[q] - G.x0) / G.cell)), i1 = Math.min(G.w - 1, Math.floor((xs[q + 1] - G.x0) / G.cell));
        for (let i = i0; i <= i1; i++) ins[i] = 1;
      }
    }
    for (let i = 0; i < G.w; i++) {
      const x = G.x0 + i * G.cell;
      let best = Infinity;
      for (const e of edges) { const d = segD2(x, z, e[0], e[1], e[2], e[3]); if (d < best) best = d; }
      const d = Math.sqrt(best);
      out[j * G.w + i] = ins[i] ? d : -d;
    }
  }
  return out;
}
/** Bilinear sample of a field on grid G (clamped). */
export function bil(G, f, x, z) {
  const fx = clamp((x - G.x0) / G.cell, 0, G.w - 1), fz = clamp((z - G.z0) / G.cell, 0, G.hh - 1);
  const i = Math.min(G.w - 2, Math.floor(fx)), j = Math.min(G.hh - 2, Math.floor(fz)), u = fx - i, v = fz - j;
  const a = f[j * G.w + i], b = f[j * G.w + i + 1], c = f[(j + 1) * G.w + i], d = f[(j + 1) * G.w + i + 1];
  return (a + (b - a) * u) * (1 - v) + (c + (d - c) * u) * v;
}
/** Bicubic (Catmull-Rom) sample of a field on grid G at world (x, z). */
export function bic(G, f, x, z) {
  return HY.bicubicAt(f, G.w, G.hh, (x - G.x0) / G.cell, (z - G.z0) / G.cell);
}
/** A soft mask (0..1) of a polygon on grid G: rasterised at a domain-warped
 *  position (organic outline), then blurred `r` cells. */
export function softMask(G, poly, {warpAmp = 0, warpL = 30000, seed = 1, r = 2} = {}) {
  const N = G.w * G.hh, m = new Float32Array(N);
  let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
  for (const p of poly) { x0 = Math.min(x0, p[0]); z0 = Math.min(z0, p[1]); x1 = Math.max(x1, p[0]); z1 = Math.max(z1, p[1]); }
  const pad = warpAmp * 1.6 + G.cell * 2;
  const i0 = Math.max(0, Math.floor((x0 - pad - G.x0) / G.cell)), i1 = Math.min(G.w - 1, Math.ceil((x1 + pad - G.x0) / G.cell));
  const j0 = Math.max(0, Math.floor((z0 - pad - G.z0) / G.cell)), j1 = Math.min(G.hh - 1, Math.ceil((z1 + pad - G.z0) / G.cell));
  for (let j = j0; j <= j1; j++)
    for (let i = i0; i <= i1; i++) {
      const x = G.x0 + i * G.cell, z = G.z0 + j * G.cell;
      const wx = warpAmp ? warpAmp * TG.fbm(x / warpL, z / warpL, seed + 3, 4) : 0;
      const wz = warpAmp ? warpAmp * TG.fbm(x / warpL + 5.2, z / warpL + 1.7, seed + 7, 4) : 0;
      if (pip(poly, x + wx, z + wz)) m[j * G.w + i] = 1;
    }
  return r > 0 ? TG.blur(m, G.w, G.hh, r, 3) : m;
}

/* ---- polylines ------------------------------------------------------------------ */
/** Lower the ground of G along a polyline to a floor profile (floorAt(s) ->
 *  absolute voxels, s = arc length), across a half-width `hw` with a soft
 *  shoulder (u = |d| / hw: full inside `core`, none past 1). Only lowers. */
export function carveAlong(G, pts, hw, floorAt, core = 0.35, opts = {}) {
  const T = SPL.tessellate(pts.map(p => [p[0], p[1]]), {step: Math.max(16, G.cell / 2), smooth: opts.smooth !== false});
  const C = TG.curveCoords(G, T.pts, hw, 0);
  const raise = !!opts.raise;
  for (let n = 0; n < G.w * G.hh; n++) {
    const d = C.d[n];
    if (!isFinite(d)) continue;
    const u = Math.sqrt(d * d + C.beyond[n] * C.beyond[n]) / hw;
    if (u >= 1) continue;
    const k = 1 - sstep(core, 1, u), fl = floorAt(C.s[n], u);
    if (raise) G.h[n] = lerp(G.h[n], fl, k);
    else if (G.h[n] > fl) G.h[n] = lerp(G.h[n], fl, k);
  }
  return T;
}
/** Resample a polyline every `step` voxels (straight segments). */
export function resample(pts, step) {
  const out = [pts[0].slice()];
  let carry = 0;
  for (let k = 1; k < pts.length; k++) {
    const a = pts[k - 1], b = pts[k], L = Math.hypot(b[0] - a[0], b[1] - a[1]);
    let s = step - carry;
    while (s <= L) { const t = s / L; out.push([a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]); s += step; }
    carry = L - (s - step);
  }
  const e = pts[pts.length - 1], l = out[out.length - 1];
  if (Math.hypot(e[0] - l[0], e[1] - l[1]) > step * 0.3) out.push(e.slice()); else out[out.length - 1] = e.slice();
  return out;
}
export function polyLength(pts) { let L = 0; for (let k = 1; k < pts.length; k++) L += Math.hypot(pts[k][0] - pts[k - 1][0], pts[k][1] - pts[k - 1][1]); return L; }

/* ---- A* road router -------------------------------------------------------------- */
// 16 directions (the 8 neighbours + knight moves): routes bend at 26.6 degree steps
const DIRS = [[1, 0], [-1, 0], [0, 1], [0, -1], [1, 1], [1, -1], [-1, 1], [-1, -1],
              [2, 1], [2, -1], [-2, 1], [-2, -1], [1, 2], [1, -2], [-1, 2], [-1, -2]];
class MinHeap {
  constructor() { this.k = []; this.v = []; }
  push(key, val) {
    const k = this.k, v = this.v;
    let c = k.length; k.push(key); v.push(val);
    while (c > 0) { const p = (c - 1) >> 1; if (k[p] < k[c] || (k[p] === k[c] && v[p] <= v[c])) break; [k[p], k[c]] = [k[c], k[p]]; [v[p], v[c]] = [v[c], v[p]]; c = p; }
  }
  pop() {
    const k = this.k, v = this.v, top = v[0];
    const lk = k.pop(), lv = v.pop();
    if (k.length) {
      k[0] = lk; v[0] = lv;
      let c = 0;
      for (;;) {
        const l = 2 * c + 1, r = l + 1;
        let m = c;
        if (l < k.length && (k[l] < k[m] || (k[l] === k[m] && v[l] < v[m]))) m = l;
        if (r < k.length && (k[r] < k[m] || (k[r] === k[m] && v[r] < v[m]))) m = r;
        if (m === c) break;
        [k[m], k[c]] = [k[c], k[m]]; [v[m], v[c]] = [v[c], v[m]]; c = m;
      }
    }
    return top;
  }
  get size() { return this.k.length; }
}
/** Route from world a to world b over grid G: cost = length * (1 + gw *
 *  (grade / maxGrade)^2) + penalties: grade past maxGrade * hard is barred,
 *  cell cost `extra[n]` (water, cliffs) multiplies a step, `bonus` (existing
 *  roads) discounts it. Returns world points [[x, z], ...] or null. */
export function route(G, a, b, opts = {}) {
  const {w, hh, cell, h} = G, N = w * hh;
  const maxG = opts.maxGrade || 0.1, hard = opts.hard || 1.6, gw = opts.gw == null ? 3 : opts.gw;
  const extra = opts.extra || null, bonus = opts.bonus || null;
  const toIJ = p => [clamp(Math.round((p[0] - G.x0) / cell), 0, w - 1), clamp(Math.round((p[1] - G.z0) / cell), 0, hh - 1)];
  const open = n => !extra || isFinite(extra[n]);
  const nearOpen = (i, j) => {
    if (open(j * w + i)) return j * w + i;
    for (let r = 1; r < 40; r++) for (let dj = -r; dj <= r; dj++) for (let di = -r; di <= r; di++) {
      if (Math.max(Math.abs(di), Math.abs(dj)) !== r) continue;
      const ii = i + di, jj = j + dj;
      if (ii >= 0 && jj >= 0 && ii < w && jj < hh && open(jj * w + ii)) return jj * w + ii;
    }
    return j * w + i;
  };
  const s = nearOpen(...toIJ(a)), t = nearOpen(...toIJ(b)), bi = t % w, bj = (t / w) | 0;
  const g = new Float64Array(N).fill(Infinity), from = new Int32Array(N).fill(-1), closed = new Uint8Array(N);
  const H = n => Math.hypot((n % w) - bi, ((n / w) | 0) - bj) * cell;
  const heap = new MinHeap();
  g[s] = 0; heap.push(H(s), s);
  while (heap.size) {
    const c = heap.pop();
    if (closed[c]) continue;
    closed[c] = 1;
    if (c === t) break;
    const ci = c % w, cj = (c / w) | 0;
    for (const [di, dj] of DIRS) {
      const ii = ci + di, jj = cj + dj;
      if (ii < 0 || jj < 0 || ii >= w || jj >= hh) continue;
      const nb = jj * w + ii;
      if (closed[nb]) continue;
      // a knight move must not jump a barred cell: check the two cells it passes
      const L = Math.hypot(di, dj) * cell;
      const grade = Math.abs(h[nb] - h[c]) / L;
      if (grade > maxG * hard) continue;
      let k = 1 + gw * (grade / maxG) ** 2;
      if (extra) k *= Math.max(extra[nb], extra[c]);
      if (bonus && bonus[nb] && bonus[c]) k *= bonus[nb];
      if (!isFinite(k)) continue;
      const ng = g[c] + L * k;
      if (ng < g[nb]) { g[nb] = ng; from[nb] = c; heap.push(ng + H(nb), nb); }
    }
  }
  if (from[t] < 0 && s !== t) return null;
  const path = [];
  for (let n = t; n >= 0; n = from[n]) { path.push([G.x0 + (n % w) * cell, G.z0 + ((n / w) | 0) * cell]); if (n === s) break; }
  path.reverse();
  return path;
}

/* ---- rendering ---------------------------------------------------------------------- */
const hex = s => [1, 3, 5].map(i => parseInt(s.slice(i, i + 2), 16));
/** RGBA overview of window [x0, z0, x1, z1] at `res` voxels per pixel:
 *  hypsometric hillshade, biome colour (alpha), water from `waterAt`, then
 *  `lines` ([{pts, rgb, width}]) and `dots` ([{x, z, rgb, r}]). */
export function renderView(view, o) {
  const {x0, z0, x1, z1, res} = view;
  const W = Math.round((x1 - x0) / res), Hh = Math.round((z1 - z0) / res);
  const hAt = o.heightAt, sea = o.seaY;
  const img = new Uint8ClampedArray(W * Hh * 4);
  const hv = new Float32Array((W + 2) * (Hh + 2));
  for (let j = -1; j <= Hh; j++) for (let i = -1; i <= W; i++) hv[(j + 1) * (W + 2) + i + 1] = hAt(x0 + (i + 0.5) * res, z0 + (j + 0.5) * res);
  const lo = o.lo == null ? sea : o.lo, hi = o.hi == null ? sea + 20000 : o.hi;
  const LAND = [[0, [96, 128, 78]], [0.03, [128, 150, 92]], [0.12, [176, 168, 120]], [0.3, [156, 128, 96]], [0.55, [132, 122, 114]], [0.8, [200, 198, 196]], [1, [250, 250, 252]]];
  const ramp = (t, out) => { let k = 0; while (k < LAND.length - 2 && t > LAND[k + 1][0]) k++; const [t0, a] = LAND[k], [t1, b] = LAND[k + 1], u = clamp((t - t0) / (t1 - t0), 0, 1); for (let q = 0; q < 3; q++) out[q] = a[q] + (b[q] - a[q]) * u; };
  const col = [0, 0, 0];
  const zf = o.zf || 2;
  for (let j = 0; j < Hh; j++)
    for (let i = 0; i < W; i++) {
      const W2 = W + 2, c = (j + 1) * W2 + i + 1;
      const gx = ((hv[c - W2 + 1] + 2 * hv[c + 1] + hv[c + W2 + 1]) - (hv[c - W2 - 1] + 2 * hv[c - 1] + hv[c + W2 - 1])) / (8 * res) * zf;
      const gz = ((hv[c + W2 - 1] + 2 * hv[c + W2] + hv[c + W2 + 1]) - (hv[c - W2 - 1] + 2 * hv[c - W2] + hv[c - W2 + 1])) / (8 * res) * zf;
      const len = Math.sqrt(gx * gx + 1 + gz * gz);
      const lx = -0.5, lz = -0.5, ly = 0.7071;
      let sh = clamp((-gx * lx + ly - gz * lz) / len, 0, 1);
      const v = hv[c], x = x0 + (i + 0.5) * res, z = z0 + (j + 0.5) * res;
      const p = (j * W + i) * 4;
      let r, g, b;
      const wat = o.waterAt ? o.waterAt(x, z, v) : null;
      if (v < sea && !wat) {
        const t = clamp((sea - v) / 600, 0, 1);
        r = 90 - 50 * t; g = 140 - 70 * t; b = 190 - 70 * t;
        const s2 = 0.75 + 0.25 * sh; r *= s2; g *= s2; b *= s2;
      } else {
        ramp(clamp((v - lo) / (hi - lo), 0, 1), col);
        const bc = o.biomeAt ? o.biomeAt(x, z) : null;
        if (bc) { const a = o.biomeAlpha == null ? 0.55 : o.biomeAlpha; for (let q = 0; q < 3; q++) col[q] = col[q] * (1 - a) + bc[q] * a; }
        const s2 = 0.25 + 0.95 * sh;
        r = col[0] * s2; g = col[1] * s2; b = col[2] * s2;
        if (wat) { const t = clamp(wat.depth / 60, 0.2, 1); r = 60 - 30 * t; g = 130 - 40 * t; b = 210 - 20 * t; if (wat.lava) { r = 250; g = 90; b = 20; } }
      }
      if (o.contour && v >= sea) {
        const ci = o.contour;
        const a = Math.floor((v - sea) / ci), bb = Math.floor((hv[c + 1] - sea) / ci), cc = Math.floor((hv[c + W2] - sea) / ci);
        if (a !== bb || a !== cc) { r *= 0.82; g *= 0.82; b *= 0.82; }
      }
      img[p] = r; img[p + 1] = g; img[p + 2] = b; img[p + 3] = 255;
    }
  const put = (px, pz, rgb, a = 1) => {
    if (px < 0 || pz < 0 || px >= W || pz >= Hh) return;
    const p = (pz * W + px) * 4;
    for (let q = 0; q < 3; q++) img[p + q] = img[p + q] * (1 - a) + rgb[q] * a;
  };
  for (const L of o.lines || []) {
    const pts = L.pts, wpx = Math.max(0, L.width || 0);
    for (let k = 1; k < pts.length; k++) {
      const ax = (pts[k - 1][0] - x0) / res, az = (pts[k - 1][1] - z0) / res, bx = (pts[k][0] - x0) / res, bz = (pts[k][1] - z0) / res;
      const n = Math.max(1, Math.ceil(Math.hypot(bx - ax, bz - az) * 2));
      for (let q = 0; q <= n; q++) {
        const px = ax + (bx - ax) * q / n, pz = az + (bz - az) * q / n;
        for (let dz = -wpx; dz <= wpx; dz++) for (let dx = -wpx; dx <= wpx; dx++) put(Math.round(px + dx), Math.round(pz + dz), L.rgb, L.alpha == null ? 1 : L.alpha);
      }
    }
  }
  for (const d of o.dots || []) {
    const px = Math.round((d.x - x0) / res), pz = Math.round((d.z - z0) / res), r = d.r || 3;
    for (let dz = -r; dz <= r; dz++) for (let dx = -r; dx <= r; dx++) if (dx * dx + dz * dz <= r * r) put(px + dx, pz + dz, d.rgb);
  }
  return {img, W, H: Hh};
}
export {hex, TG, HY, SPL};
