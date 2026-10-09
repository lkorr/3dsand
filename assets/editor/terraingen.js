/* terraingen.js — the TERRAIN GENERATORS of the World map page
 * (docs/PLAN_worldbuilder.md K1 item 4): pure, DOM-free, Node-runnable, so a
 * Worker (terrain_worker.js), the page and scripts/test_terraingen.mjs /
 * scripts/render_terrain_preview.mjs all run the same code.
 *
 * Everything works on a GRID: {w, hh, x0, z0, cell, h} -- h a Float32Array of
 * ABSOLUTE heights in voxels (like the height plane), sample (i, j) at world
 * column (x0 + i * cell, z0 + j * cell). The page reads a window of the plane
 * (at the posts, or a coarser proxy), runs a generator, writes the result back
 * through maptiles.js (one undo step). DETERMINISTIC: every random choice
 * comes from a seeded counter generator or the integer-hashed noise of
 * heightshapes.js; same inputs -> bit-identical output.
 *
 *   noise       ridged multifractal (Musgrave: offset 1, gain 2), fBm, domain
 *               warp (Quilez), 1D noise along a curve.
 *   terrace     h' = step * (floor(x) + smoothstep(frac(x)) ^ sharp).
 *   RANGE       a mountain range from a spline of knots [x, z, crestY,
 *               halfWidth]: a crest with peaks and saddles (ridged noise in
 *               arc length), a wandering crest line, SPURS (ridged noise in
 *               the curve's own (along, across) frame, so they run down the
 *               flanks), FOOTHILLS (domain-warped ridged noise under a wide
 *               fading mask) and EROSION: the range is an UPLIFT field and the
 *               implicit stream-power solver grows dendritic valleys into it
 *               (Cordonnier et al. 2016, Schott et al. 2023), then the crest
 *               is renormalised to the knots' heights.
 *   primitives  crater (rim profile), canyon (terraced, meandering), mesa
 *               field, dunes (asymmetric: gentle stoss, steep lee), fjord /
 *               glacial U-valley carve, volcano, island / continent masks.
 *   TEMPLATES   Azgaar-style op lists (fill, hill, pit, range, trough,
 *               strait, mask, smooth, add, multiply, erode, + the primitives)
 *               with a seed: continent, island, archipelago, ...
 *   shade       a hypsometric + Horn-gradient hillshade RGBA image of a grid
 *               (the Range preview overlay, the preview PNGs). */

import {noise2} from './heightshapes.js';
import * as HY from './hydrology.js';
import * as SP from './spline.js';

const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
export const smoothstep = (e0, e1, x) => { const t = clamp((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); };
const lerp = (a, b, t) => a + (b - a) * t;

/** A seeded generator of floats in [0, 1) (mulberry32). */
export function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6D2B79F5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/* ---- noise ------------------------------------------------------------------------------ */
/** fBm of noise2: about -1..1. x, z in noise units (1 = the base wavelength). */
export function fbm(x, z, seed, octaves = 5, lac = 2, gain = 0.5) {
  let a = 1, s = 0, n = 0, f = 1;
  for (let o = 0; o < octaves; o++) { s += a * noise2(x * f, z * f, (seed + o * 1013) | 0); n += a; a *= gain; f *= lac; }
  return s / n;
}
/** Musgrave's ridged multifractal (offset 1, gain 2, H 1): sharp crests whose
 *  detail is strongest where the signal is high (the crests), so ridges get
 *  rugged and valleys stay smooth. About 0..1. */
export function ridged(x, z, seed, octaves = 6, lac = 2, H = 1, offset = 1, gain = 2) {
  let f = 1, w = 1, sum = 0, norm = 0;
  for (let o = 0; o < octaves; o++) {
    let sig = offset - Math.abs(noise2(x * f, z * f, (seed + o * 7919) | 0) / 1.41);
    sig *= sig;
    sig *= w;
    w = clamp(sig * gain, 0, 1);
    const sw = Math.pow(f, -H);
    sum += sig * sw; norm += sw;
    f *= lac;
  }
  return clamp(sum / norm, 0, 1.2);
}
/** Quilez domain warp: the point moved by a two-component fBm, `amp` noise units. */
export function warp(x, z, seed, amp, octaves = 3) {
  return [x + amp * fbm(x, z, seed + 31, octaves), z + amp * fbm(x + 5.2, z + 1.3, seed + 57, octaves)];
}
/** 1D noise along a line (a row of noise2), -1..1. */
export const noise1 = (t, seed) => noise2(t, 0.5, seed) / 1.41;
/** 1D ridged (peaks): 0..1, sharp maxima. */
export function ridged1(t, seed, octaves = 3) {
  let f = 1, a = 1, s = 0, n = 0;
  for (let o = 0; o < octaves; o++) { const v = 1 - Math.abs(noise1(t * f, seed + o * 101)); s += a * v * v; n += a; a *= 0.5; f *= 2.1; }
  return s / n;
}

/** The terrace formula: h' = step * (floor(x) + smoothstep(frac(x)) ^ sharp),
 *  x = (h - datum) / step. sharp 1 = soft steps, 4+ = shelves and cliffs. */
export function terraceH(h, datum, step, sharp) {
  if (!(step > 0)) return h;
  const x = (h - datum) / step, b = Math.floor(x), f = x - b;
  const s = f * f * (3 - 2 * f);
  return datum + step * (b + Math.pow(s, Math.max(0.05, sharp)));
}

/* ---- grids ------------------------------------------------------------------------------ */
export function makeGrid(w, hh, x0, z0, cell, fill = 0) {
  const h = new Float32Array(w * hh);
  if (fill) h.fill(fill);
  return {w, hh, x0, z0, cell, h};
}
export const gx = (G, i) => G.x0 + i * G.cell;
export const gz = (G, j) => G.z0 + j * G.cell;
/** Bilinear sample of a grid at world (x, z), clamped. */
export function sampleGrid(G, x, z) {
  const fx = clamp((x - G.x0) / G.cell, 0, G.w - 1), fz = clamp((z - G.z0) / G.cell, 0, G.hh - 1);
  const i = Math.min(G.w - 2, Math.floor(fx)), j = Math.min(G.hh - 2, Math.floor(fz)), u = fx - i, v = fz - j;
  const h = G.h, w = G.w;
  if (G.w < 2 || G.hh < 2) return h[0];
  const a = h[j * w + i] + (h[j * w + i + 1] - h[j * w + i]) * u, b = h[(j + 1) * w + i] + (h[(j + 1) * w + i + 1] - h[(j + 1) * w + i]) * u;
  return a + (b - a) * v;
}
/** The part of G inside world box [x0, z0, x1, z1] as its own grid ({.., i0,
 *  j0} remember where it came from), or null if they do not overlap. */
export function subGrid(G, box) {
  const i0 = Math.max(0, Math.floor((box[0] - G.x0) / G.cell)), i1 = Math.min(G.w - 1, Math.ceil((box[2] - G.x0) / G.cell));
  const j0 = Math.max(0, Math.floor((box[1] - G.z0) / G.cell)), j1 = Math.min(G.hh - 1, Math.ceil((box[3] - G.z0) / G.cell));
  if (i1 - i0 < 2 || j1 - j0 < 2) return null;
  const w = i1 - i0 + 1, hh = j1 - j0 + 1, h = new Float32Array(w * hh);
  for (let j = 0; j < hh; j++) h.set(G.h.subarray((j0 + j) * G.w + i0, (j0 + j) * G.w + i0 + w), j * w);
  return {w, hh, x0: G.x0 + i0 * G.cell, z0: G.z0 + j0 * G.cell, cell: G.cell, h, i0, j0};
}
/** Write a subGrid back into G. */
export function pasteGrid(G, S) {
  for (let j = 0; j < S.hh; j++) G.h.set(S.h.subarray(j * S.w, (j + 1) * S.w), (S.j0 + j) * G.w + S.i0);
  return G;
}
/** Separable box blur, `r` cells, `passes` times (3 ~ a Gaussian). */
export function blur(src, w, hh, r, passes = 3) {
  let a = Float32Array.from(src), b = new Float32Array(a.length);
  r = Math.max(0, r | 0);
  if (!r) return a;
  for (let p = 0; p < passes; p++) {
    for (let j = 0; j < hh; j++) {
      let s = 0;
      for (let i = -r; i <= r; i++) s += a[j * w + clamp(i, 0, w - 1)];
      for (let i = 0; i < w; i++) {
        b[j * w + i] = s / (2 * r + 1);
        s += a[j * w + clamp(i + r + 1, 0, w - 1)] - a[j * w + clamp(i - r, 0, w - 1)];
      }
    }
    for (let i = 0; i < w; i++) {
      let s = 0;
      for (let j = -r; j <= r; j++) s += b[clamp(j, 0, hh - 1) * w + i];
      for (let j = 0; j < hh; j++) {
        a[j * w + i] = s / (2 * r + 1);
        s += b[clamp(j + r + 1, 0, hh - 1) * w + i] - b[clamp(j - r, 0, hh - 1) * w + i];
      }
    }
  }
  return a;
}

/* ---- curve coordinates --------------------------------------------------------------------- */
/** For every cell of G within `reach` of the polyline `pts` ([[x, z, ...attrs]]),
 *  the curve coordinates: s (arc length of the nearest point), d (signed
 *  distance: + = right of travel), beyond (how far past an END the cell lies,
 *  0 inside), and the attributes lerped at the nearest point. Cells farther
 *  than `reach` get d = Infinity. */
export function curveCoords(G, pts, reach, nAttr = 2) {
  const N = G.w * G.hh;
  const S = new Float32Array(N), D = new Float32Array(N).fill(Infinity), B = new Float32Array(N);
  const AT = [];
  for (let a = 0; a < nAttr; a++) AT.push(new Float32Array(N));
  if (pts.length < 2) return {s: S, d: D, beyond: B, attrs: AT};
  const segs = [];
  let cum = 0;
  for (let k = 0; k + 1 < pts.length; k++) {
    const a = pts[k], b = pts[k + 1], ex = b[0] - a[0], ez = b[1] - a[1], L = Math.hypot(ex, ez);
    segs.push({a, b, ex, ez, L, s0: cum, first: k === 0, last: k + 2 === pts.length});
    cum += L;
  }
  const r2 = reach * reach;
  for (const sg of segs) {
    const i0 = Math.max(0, Math.floor((Math.min(sg.a[0], sg.b[0]) - reach - G.x0) / G.cell));
    const i1 = Math.min(G.w - 1, Math.ceil((Math.max(sg.a[0], sg.b[0]) + reach - G.x0) / G.cell));
    const j0 = Math.max(0, Math.floor((Math.min(sg.a[1], sg.b[1]) - reach - G.z0) / G.cell));
    const j1 = Math.min(G.hh - 1, Math.ceil((Math.max(sg.a[1], sg.b[1]) + reach - G.z0) / G.cell));
    const L2 = sg.L * sg.L;
    for (let j = j0; j <= j1; j++) {
      const z = G.z0 + j * G.cell;
      for (let i = i0; i <= i1; i++) {
        const x = G.x0 + i * G.cell, n = j * G.w + i;
        const tr = L2 > 0 ? ((x - sg.a[0]) * sg.ex + (z - sg.a[1]) * sg.ez) / L2 : 0;
        const t = clamp(tr, 0, 1);
        const cx = sg.a[0] + sg.ex * t, cz = sg.a[1] + sg.ez * t;
        const d2 = (x - cx) * (x - cx) + (z - cz) * (z - cz);
        if (d2 >= r2 || d2 >= D[n] * D[n]) continue;
        const dist = Math.sqrt(d2);
        const side = sg.L > 0 ? (sg.ex * (z - cz) - sg.ez * (x - cx)) / sg.L : 0;
        D[n] = side < 0 ? -dist : dist;
        if (D[n] === 0) D[n] = 0;
        S[n] = sg.s0 + sg.L * t;
        B[n] = (sg.first && tr < 0) ? -tr * sg.L : (sg.last && tr > 1) ? (tr - 1) * sg.L : 0;
        for (let a = 0; a < nAttr; a++) {
          const va = sg.a[2 + a], vb = sg.b[2 + a];
          AT[a][n] = va == null ? 0 : va + ((vb == null ? va : vb) - va) * t;
        }
      }
    }
  }
  // a cell whose nearest point was set with a negative-zero tie keeps |d|
  return {s: S, d: D, beyond: B, attrs: AT, length: cum};
}

/* ---- THE RANGE ------------------------------------------------------------------------------ */
/** Cross-sections of a range: u = distance from the crest / half-width, 0..1;
 *  1 at the crest, 0 at the foot. */
export const RANGE_PROFILES = {
  // concave flanks under a steep upper wall: the eroded-mountain default
  alpine: u => Math.pow(1 - u, 1.6) * (1 + 0.6 * u),
  // a knife edge
  sharp: u => Math.pow(1 - u, 2.6),
  // a rounded, old range (Appalachian / Scottish)
  round: u => 0.5 * (1 + Math.cos(Math.PI * u)),
  // a high, broad massif with a steep edge (a plateau range)
  massif: u => 1 - Math.pow(u, 2.4) * (3 - 2 * u) * 0.98 - 0.02 * u,
};
export const RANGE_PROFILE_NAMES = Object.keys(RANGE_PROFILES);
export const RANGE_DEFAULTS = {profile: 'alpine', seed: 1, peaks: 0.6, spurs: 0.6, foothills: 0.5, erode: 0.6, wander: 0.5};

/** The reach of a range: how far from its spline anything moves (voxels). */
export function rangeReach(r) {
  const W = Math.max(0, ...(r.points || []).map(p => +p[3] || 0));
  const f = r.foothills == null ? RANGE_DEFAULTS.foothills : +r.foothills;
  // + the most the warp can move a column's (along, across) coordinates (~0.5 W)
  return W * (1.15 + 1.4 * f) + W * 0.6;
}
/** The world bbox [x0, z0, x1, z1] a range can touch. */
export function rangeBox(r) {
  const R = rangeReach(r);
  let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
  for (const p of r.points || []) { x0 = Math.min(x0, p[0]); z0 = Math.min(z0, p[1]); x1 = Math.max(x1, p[0]); z1 = Math.max(z1, p[1]); }
  return [x0 - R, z0 - R, x1 + R, z1 + R];
}

/** THE RANGE'S EROSION: a landscape-evolution run (FastScape-style). The
 *  range is an UPLIFT field shaped like the designed envelope (`rel`, its
 *  relief above the ground `gh`); the land starts LOW and rough and rises
 *  while the implicit stream-power solver erodes it. The drainage net forms
 *  while the land is low -- so it branches, captures and reaches the divide,
 *  as a real range's does -- and is inherited as the land rises toward the
 *  steady state of uplift vs erosion: dendritic valleys, spurs between them, a
 *  jagged crest of peaks and cols. Then a smooth local gain puts the relief's
 *  maxima back on the envelope's (the knots' crest heights) and a talus pass
 *  trims what the gain over-steepened. MULTI-RESOLUTION: on a big grid the
 *  run happens on a 2x coarser grid first and the fine grid continues from it
 *  for the last share of the steps (the steady state is scale-free in this
 *  K scaling, so the net carries over). Returns the eroded absolute heights. */
export function erodeRange(gh, rel, w, hh, x0, z0, cell, W0, seed, er, opts = {}) {
  const T = opts.tune || {}, N = w * hh;
  const fixed = new Uint8Array(N);
  let maxRel = 0;
  for (let n = 0; n < N; n++) { if (rel[n] <= 0.25 || (opts.outlets && opts.outlets[n])) fixed[n] = 1; if (rel[n] > maxRel) maxRel = rel[n]; }
  const steps = opts.iters || T.steps || Math.round(50 + 110 * er);
  const U = new Float32Array(N), K = new Float32Array(N);
  let z = new Float32Array(N);
  const kv = T.kVar == null ? 0.6 : T.kVar, ks = T.kScale || 1.0, nb = T.nBig == null ? 0.15 : T.nBig;
  for (let j = 0; j < hh; j++)
    for (let i = 0; i < w; i++) {
      const n = j * w + i, x = x0 + i * cell, zz = z0 + j * cell;
      U[n] = fixed[n] ? 0 : rel[n] / steps;
      // hardness: harder rock (lower K) in warped bands -> varied relief
      const hk = fbm(x / (W0 * ks), zz / (W0 * ks), seed + 41, 3);
      K[n] = 1 - kv + 2 * kv * smoothstep(-0.5, 0.5, hk);
      // start low and rough: the noise is what the first drainage net follows
      // (world-space noise at fixed wavelengths: the same on the coarse and the fine grid)
      // (the small-scale noise fades out with the relief: where the range ends,
      // nothing of it may be left for the gain below to blow up)
      z[n] = gh[n] + rel[n] * 0.03 + maxRel * 0.004 * (1 + fbm(x / 1536, zz / 1536, seed + 43, 3)) * Math.min(1, rel[n] / (0.05 * maxRel))
           + rel[n] * nb * fbm(x / (W0 * 0.45), zz / (W0 * 0.45), seed + 47, 3);
      // THE WINDOW'S EDGE is a base level (an outlet) the solver never moves: hold
      // it at the designed envelope, not the bare ground, or a range the window
      // cuts would be eroded down to the ground along the cut (a cliff there)
      if (!fixed[n] && (i === 0 || j === 0 || i === w - 1 || j === hh - 1)) z[n] = gh[n] + rel[n];
    }
  // K: at steady state (U = K sqrt(A) S) a trunk valley draining a quarter of
  // a flank holds `vs` of the envelope's mean flank slope; with A in cells this
  // is k0 = 2 / (steps * vs) whatever the cell size
  const vs = T.vs || 0.15, k0 = 2 / (steps * vs);
  for (let n = 0; n < N; n++) K[n] *= k0;
  let fineSteps = steps;
  const multi = opts.multires !== false && N > (opts.multiresCells || 120000) && w > 16 && hh > 16;
  if (multi) {
    // the coarse run (all the steps), its eroded relief brought up as the start
    const cg = HY.downsample(gh, w, hh, 2), cr = HY.downsample(rel, w, hh, 2);
    let co = null;
    if (opts.outlets) {
      const cw2 = Math.ceil(w / 2), ch2 = Math.ceil(hh / 2);
      co = new Uint8Array(cw2 * ch2);
      for (let j = 0; j < hh; j++) for (let i = 0; i < w; i++) if (opts.outlets[j * w + i]) co[(j >> 1) * cw2 + (i >> 1)] = 1;
    }
    const cz = erodeRange(cg.h, cr.h, cg.w, cg.hh, x0 + cell / 2, z0 + cell / 2, cell * 2, W0, seed, er,
                          {...opts, outlets: co, multires: false, raw: true, progress: opts.progress ? (p => opts.progress(p * 0.6)) : null});
    const d = new Float32Array(cz.length);
    for (let n = 0; n < d.length; n++) d[n] = cz[n] - cg.h[n];
    const up = HY.upsample(d, cg.w, cg.hh, 2, w, hh);
    for (let n = 0; n < N; n++) z[n] = fixed[n] ? gh[n] : gh[n] + Math.max(0, up[n]) + (z[n] - gh[n] - rel[n] * 0.03) * 0.5;
    fineSteps = Math.max(8, Math.round(steps * (T.fineShare || 0.3)));
  }
  HY.streamPower(z, w, hh, cell, {iters: fineSteps, U, K, fixed, seaY: opts.seaY, jitter: T.jitter == null ? 0.5 : T.jitter, seed,
                                  diffuse: multi ? 0 : (T.diffuse == null ? 0.25 : T.diffuse), diffuseUntil: 0.5, talusDeg: T.talus || 40,
                                  thermalPasses: 1, fillEvery: 8,
                                  progress: opts.progress ? (multi ? (p => opts.progress(0.6 + 0.4 * p)) : opts.progress) : null});
  if (opts.raw) return z;
  // the gain: local maxima of the eroded relief onto the envelope's
  const re = new Float32Array(N);
  for (let n = 0; n < N; n++) re[n] = Math.max(0, z[n] - gh[n]);
  const R = Math.max(1, Math.round(W0 * 0.5 / cell));
  const gE = blur(maxFilter(rel, w, hh, R), w, hh, R, 2), gR = blur(maxFilter(re, w, hh, R), w, hh, R, 2);
  for (let n = 0; n < N; n++) {
    if (fixed[n]) { z[n] = gh[n]; continue; }
    const g = gR[n] > 1 ? clamp(gE[n] / gR[n], 0.25, 4) : 1;
    z[n] = gh[n] + Math.min(re[n] * g, rel[n] * 1.6 + 0.02 * gE[n]);
  }
  HY.thermal(z, w, hh, cell, T.talus || 40, 4, {fixed});
  return z;
}

/** THE RANGE GENERATOR. r = a ranges[] entry: {points: [[x, z, crestY,
 *  halfWidth], ...] (world voxels; crestY absolute), profile, seed, peaks,
 *  spurs, foothills, erode, wander (0..1 each)}. G = the grid of the CURRENT
 *  ground (absolute voxels) covering rangeBox(r); opts = {seaY, iters (the
 *  erosion budget, default by `erode`), progress(fraction), noErode}.
 *  Returns {h: new heights (Float32Array, >= the ground everywhere: a range
 *  only raises), relief: the range's own height above the ground before
 *  erosion, mask: Uint8Array (1 = the range moved this cell)}. */
export function rangeField(r, G, opts = {}) {
  const o = {...RANGE_DEFAULTS, ...r};
  const prof = RANGE_PROFILES[o.profile] || RANGE_PROFILES.alpine;
  const seed = (o.seed | 0) * 7 + 11;
  const N = G.w * G.hh;
  const knots = (o.points || []).filter(p => Array.isArray(p) && p.length >= 2);
  const out = {h: Float32Array.from(G.h), relief: new Float32Array(N), mask: new Uint8Array(N)};
  if (knots.length < 2) return out;
  // the crest line, tessellated (shared spline), coarse enough for the per-cell search
  const W0 = Math.max(G.cell, ...knots.map(p => +p[3] || 0));
  const T = SP.tessellate(knots.map(p => [p[0], p[1], +p[2] || 0, Math.max(G.cell, +p[3] || 0)]), {step: Math.max(G.cell, W0 / 10), handles: o.handles || null});
  const reach = rangeReach(o);
  const C = curveCoords(G, T.pts, reach, 2);
  const L = C.length || T.length;
  // The knots' crest and width are taken at each column's NEAREST point of the
  // crest; where two stretches of a bent range are equally near (the medial
  // axis) that jumps. Smooth both fields (normalized convolution over the
  // columns in reach) so no seam runs out from the inside of a bend.
  {
    const N0 = G.w * G.hh, wt = new Float32Array(N0), r = Math.max(1, Math.round(W0 * 0.3 / G.cell));
    for (let n = 0; n < N0; n++) wt[n] = isFinite(C.d[n]) ? 1 : 0;
    const bw = blur(wt, G.w, G.hh, r, 2);
    for (let a = 0; a < 2; a++) {
      const v = new Float32Array(N0);
      for (let n = 0; n < N0; n++) v[n] = wt[n] ? C.attrs[a][n] : 0;
      const bv = blur(v, G.w, G.hh, r, 2);
      for (let n = 0; n < N0; n++) if (wt[n] && bw[n] > 1e-6) C.attrs[a][n] = bv[n] / bw[n];
    }
  }
  // the ground under the range, smoothed: the datum the relief stands on
  // (a range on a bumpy plain must not copy every bump up to its crest)
  const gb = blur(G.h, G.w, G.hh, Math.max(1, Math.round(W0 * 0.35 / G.cell)), 2);
  const peaks = clamp(+o.peaks, 0, 1), spurs = clamp(+o.spurs, 0, 1), foot = clamp(+o.foothills, 0, 1), wander = clamp(+o.wander, 0, 1);
  const rel = out.relief;
  let anyCell = false;
  for (let j = 0; j < G.hh; j++)
    for (let i = 0; i < G.w; i++) {
      const n = j * G.w + i;
      const d0 = C.d[n];
      if (!isFinite(d0)) continue;
      const x = G.x0 + i * G.cell, z = G.z0 + j * G.cell;
      const crest = C.attrs[0][n], W = C.attrs[1][n];
      const s = C.s[n];
      // 2D warp of the (s, d) frame: the crest wanders, spurs bend
      const wx = fbm(x / (W * 1.6), z / (W * 1.6), seed + 3, 3), wz = fbm(x / (W * 1.6) + 7.1, z / (W * 1.6) - 3.3, seed + 5, 3);
      const fadeS = 1 - smoothstep(W * 0.8, W * 1.25, Math.abs(d0));
      const sW = s + W * 0.45 * wx * (0.4 + wander);
      // the crest line itself wanders across: a low-frequency offset along s
      const off = fadeS * W * (0.18 * noise1(s / (W * 2.8), seed + 7) + 0.07 * noise1(s / (W * 0.9), seed + 9)) * (0.3 + wander);
      const d = d0 - off + W * 0.22 * wz * (0.4 + wander);
      const beyond = C.beyond[n];
      const dd = Math.sqrt(d * d + beyond * beyond);
      const u = dd / W;
      // peaks and saddles along the crest (ridged noise in arc length)
      const pk = 0.62 * ridged1(sW / (W * 1.9), seed + 13, 3) + 0.38 * ridged1(sW / (W * 0.75), seed + 17, 2);
      let cm = 1 - peaks * 0.5 * (1 - pk);
      // the ends: the crest falls away over one half-width past the first / last knot
      const endT = Math.min(smoothstep(-W * 0.2, W * 1.2, s - 0) , smoothstep(-W * 0.2, W * 1.2, L - s));
      cm *= lerp(0.45, 1, endT);
      const base = gb[n];
      const delta = Math.max(0, crest - base);
      let v = 0;
      if (u < 1) {
        let p = prof(u);
        // spurs: ridged noise in the curve frame, stretched ACROSS the range, so its
        // crests run down the flanks; the valleys between them are cut into the flank
        if (spurs > 0) {
          const T = opts.tune || {};
          const sx = sW / (W * (T.spurL || 0.5)), sz = d / (W * (T.spurD || 0.9));
          const [qx, qz] = warp(sx, sz, seed + 19, T.spurWarp || 0.45);
          const rn = ridged(qx, qz, seed + 23, T.spurOct || 2);
          const a0 = T.a0 != null ? T.a0 : 0.35;
          const a = (a0 + (1 - a0) * smoothstep(0.0, 0.22, u)) * (1 - smoothstep(0.75, 1.0, u));
          p *= 1 - spurs * (T.spurAmp || 0.7) * a * (1 - clamp(rn * 1.1, 0, 1));
        }
        v = delta * cm * p;
      }
      // foothills: warped ridged hills under a wide mask round the base of the range
      if (foot > 0) {
        const fu = (u - 0.55) / (0.6 + 1.4 * foot);
        const band = fu < 0 ? 1 : 1 - smoothstep(0, 1, fu);
        if (band > 0) {
          const [qx, qz] = warp(x / (W * 0.55), z / (W * 0.55), seed + 29, 0.6);
          const hn = ridged(qx, qz, seed + 31, 5);
          const fh = delta * foot * 0.3 * cm * band * hn * hn * (u < 1 ? 1 - prof(u) * 0.7 : 1);
          v = Math.max(v, fh) + 0.25 * Math.min(v, fh);
        }
      }
      // fade out before the reach: nothing the warp pulled in may end on a cliff
      v *= 1 - smoothstep(reach * 0.82, reach * 0.98, Math.abs(d0));
      if (v > 0.25) { rel[n] = v; anyCell = true; }
    }
  if (!anyCell) return out;
  // the designed envelope
  const env = new Float32Array(N);
  for (let n = 0; n < N; n++) env[n] = G.h[n] + rel[n];
  let result = env;
  const er = opts.noErode ? 0 : clamp(+o.erode, 0, 1);
  if (er > 0) {
    // in RELIEF space (a flat zero base): the range's own drainage, so the bumps of
    // the ground under it are neither eroded away nor routed through
    // (the sea is base level there too: a column whose envelope is under it is an outlet)
    const under = new Uint8Array(N);
    if (opts.seaY != null) for (let n = 0; n < N; n++) if (env[n] < opts.seaY) under[n] = 1;
    const z = erodeRange(new Float32Array(N), rel, G.w, G.hh, G.x0, G.z0, G.cell, W0, seed, er, {...opts, seaY: null, outlets: under});
    result = new Float32Array(N);
    for (let n = 0; n < N; n++) result[n] = lerp(env[n], G.h[n] + z[n], Math.min(1, er * 1.4));
    // the range's drainage was solved on its own relief; laid on the bumps of the
    // ground it can leave hollows where a valley crosses one. Fill those up to
    // ~40 m deep (an alluvial valley floor, epsilon-sloped so it drains); deeper
    // basins stay (they are lakes)
    const out2 = new Uint8Array(N);
    for (let n = 0; n < N; n++) if (rel[n] <= 0.25 || under[n]) out2[n] = 1;
    const F = HY.priorityFlood(result, G.w, G.hh, {outlets: out2, epsilon: 0.01});
    const cap = 40 * 10;
    for (let n = 0; n < N; n++) if (!out2[n] && F[n] > result[n] && F[n] - result[n] < cap) result[n] = F[n];
  }
  for (let n = 0; n < N; n++) {
    const v = Math.max(G.h[n], result[n]);
    if (v > G.h[n] + 0.05) out.mask[n] = 1;
    out.h[n] = v;
  }
  return out;
}
/** The proxy factor for a window of w x hh samples `cell` voxels apart: the
 *  smallest power of two that puts the proxy cell at >= `minCell` voxels and
 *  holds the proxy under `maxCells` cells. */
export function proxyFactor(w, hh, cell, minCell = 256, maxCells = 450000) {
  let f = Math.max(1, Math.round(minCell / cell));
  f = 1 << Math.round(Math.log2(f));
  while (Math.ceil(w / f) * Math.ceil(hh / f) > maxCells) f *= 2;
  return f;
}
/** The coarse PROXY grid of a fine grid (box average by f), aligned so proxy
 *  cell k sits at the centre of fine samples [k f, k f + f). */
export function proxyGrid(G, f) {
  if (f <= 1) return {...G, h: Float32Array.from(G.h)};
  const d = HY.downsample(G.h, G.w, G.hh, f);
  return {w: d.w, hh: d.hh, x0: G.x0 + (f - 1) / 2 * G.cell, z0: G.z0 + (f - 1) / 2 * G.cell, cell: G.cell * f, h: d.h};
}
/** Fine detail for a generated relief at post resolution: ridged noise at
 *  ~120 m and ~40 m, its amplitude a share of the local relief (none on flat
 *  land), so a 25.6 m proxy's ridges get rock texture at 6.4 m posts. */
export function reliefDetail(x, z, relief, seed, amount = 1) {
  if (!(relief > 1)) return 0;
  const a = ridged(x / 1200, z / 1200, seed + 61, 3) - 0.55, b = ridged(x / 400, z / 400, seed + 67, 2) - 0.55;
  return amount * relief * (0.045 * a + 0.02 * b);
}
/** BAKE A RANGE onto a window of posts G (the height plane's posts covering
 *  rangeBox(r), absolute voxels): the range is generated on a proxy (~25.6 m,
 *  opts.minCell / opts.maxCells / opts.factor override), its CHANGE is
 *  brought back to the posts (bicubic) with fine detail added, so the posts'
 *  own detail under it is kept. Returns {h (the new posts), changed: Uint8Array,
 *  factor, proxy: {w, hh, cell}}. */
export function rangeBake(r, G, opts = {}) {
  // the proxy: 25.6 m, finer for a narrow range (two dozen cells across a flank at least)
  const W0 = Math.max(...(r.points || []).map(p => +p[3] || 0), G.cell);
  const minCell = opts.minCell || Math.max(G.cell, Math.min(256, 1 << Math.floor(Math.log2(Math.max(1, W0 / 24)))));
  const f = opts.factor || proxyFactor(G.w, G.hh, G.cell, minCell, opts.maxCells || 450000);
  const P = proxyGrid(G, f);
  const R = rangeField(r, P, opts);
  const N = G.w * G.hh, out = new Float32Array(N), changed = new Uint8Array(N);
  const d = new Float32Array(P.w * P.hh);
  for (let n = 0; n < d.length; n++) d[n] = R.h[n] - P.h[n];
  const up = f > 1 ? HY.upsample(d, P.w, P.hh, f, G.w, G.hh) : d;
  const seed = ((r.seed | 0) * 7 + 11) | 0, det = opts.detail == null ? 1 : opts.detail;
  for (let j = 0; j < G.hh; j++)
    for (let i = 0; i < G.w; i++) {
      const n = j * G.w + i, c = up[n];
      if (!(c > 0.05)) { out[n] = G.h[n]; continue; }
      out[n] = G.h[n] + c + reliefDetail(G.x0 + i * G.cell, G.z0 + j * G.cell, c, seed, det);
      if (out[n] < G.h[n]) out[n] = G.h[n];
      changed[n] = 1;
    }
  return {h: out, changed, factor: f, proxy: {w: P.w, hh: P.hh, cell: P.cell}};
}
/** Running maximum over a (2r+1)^2 square (separable). */
export function maxFilter(src, w, hh, r) {
  const a = new Float32Array(src.length), b = new Float32Array(src.length);
  for (let j = 0; j < hh; j++)
    for (let i = 0; i < w; i++) {
      let m = -Infinity;
      for (let k = Math.max(0, i - r); k <= Math.min(w - 1, i + r); k++) m = Math.max(m, src[j * w + k]);
      a[j * w + i] = m;
    }
  for (let i = 0; i < w; i++)
    for (let j = 0; j < hh; j++) {
      let m = -Infinity;
      for (let k = Math.max(0, j - r); k <= Math.min(hh - 1, j + r); k++) m = Math.max(m, a[k * w + i]);
      b[j * w + i] = m;
    }
  return b;
}

/* ---- primitives ---------------------------------------------------------------------------
 * Each takes a grid G (modified IN PLACE, returned) and options in VOXELS and
 * world columns; `seed` drives every irregularity. They ADD to / CARVE the
 * ground that is there (a template's `fill` is what replaces it). */

/** A paths' polyline [[x, z], ...] across a box [x0, z0, x1, z1] along its LONG
 *  axis, meandering by `meander` (0..1) of the box's short side. */
export function boxPath(box, meander, seed, n = 9) {
  const [x0, z0, x1, z1] = box, wx = x1 - x0, wz = z1 - z0, alongX = wx >= wz;
  const L = alongX ? wx : wz, S = alongX ? wz : wx, pts = [];
  for (let k = 0; k < n; k++) {
    const t = k / (n - 1);
    const off = (k === 0 || k === n - 1 ? 0.4 : 1) * meander * S * 0.32 * noise1(t * 2.3, seed + 71);
    pts.push(alongX ? [x0 + t * L, z0 + S / 2 + off] : [x0 + S / 2 + off, z0 + t * L]);
  }
  return pts;
}
/** Noise round a circle (periodic in the angle): -1..1. */
const ringNoise = (ang, freq, seed) => noise2(Math.cos(ang) * freq + 17.3, Math.sin(ang) * freq - 4.1, seed) / 1.41;
/** CRATER: a bowl below the ground with a raised rim and an ejecta apron.
 *  o = {x, z, radius, depth, rim (height of the rim above the ground), peak
 *  (central peak height as a share of the depth; default by size), seed}.
 *  Profile in rho = r / radius: inside -depth + (depth + rim) rho^3 (a bowl
 *  with a flatter floor) with slump terraces under the rim, outside
 *  rim * rho^-3 fading to nothing by 2.4 R; the rim wobbles round the circle. */
export function crater(G, o) {
  const R = Math.max(G.cell, +o.radius || 0), depth = +o.depth || R * 0.2, rim = o.rim == null ? depth * 0.35 : +o.rim, seed = o.seed | 0;
  const peak = o.peak == null ? (R > 6000 ? 0.45 : 0) : +o.peak;
  const reach = R * 2.4;
  const i0 = Math.max(0, Math.floor((o.x - reach - G.x0) / G.cell)), i1 = Math.min(G.w - 1, Math.ceil((o.x + reach - G.x0) / G.cell));
  const j0 = Math.max(0, Math.floor((o.z - reach - G.z0) / G.cell)), j1 = Math.min(G.hh - 1, Math.ceil((o.z + reach - G.z0) / G.cell));
  // the floor sits below the ground at the centre, not below each column
  const g0 = sampleGrid(G, o.x, o.z);
  for (let j = j0; j <= j1; j++)
    for (let i = i0; i <= i1; i++) {
      const x = gx(G, i), z = gz(G, j), dx = x - o.x, dz = z - o.z;
      const ang = Math.atan2(dz, dx);
      const wob = 1 + 0.07 * ringNoise(ang, 1.6, seed + 5) + 0.035 * ringNoise(ang, 5, seed + 7);
      const rho = Math.hypot(dx, dz) / (R * wob);
      if (rho >= 2.4) continue;
      const n = j * G.w + i;
      if (rho < 1) {
        let b = rho * rho * rho;
        // slump terraces under the rim (big craters)
        if (R > 3000 && rho > 0.55) b = lerp(b, terraceH(b, 0, 0.18, 2.2), smoothstep(0.55, 0.75, rho) * (1 - smoothstep(0.9, 1, rho)));
        let target = g0 - depth + (depth + rim) * b;
        if (peak > 0) target += depth * peak * Math.pow(Math.max(0, 1 - rho / 0.22), 1.6) * (0.8 + 0.4 * ridged(dx / (R * 0.12), dz / (R * 0.12), seed + 3, 3));
        // inside: the bowl replaces the ground (blend in near the rim crest)
        const t = smoothstep(0.88, 1, rho);
        G.h[n] = lerp(target, G.h[n] + rim, t);
      } else G.h[n] += rim * Math.pow(rho, -3) * (1 - smoothstep(1.6, 2.4, rho)) * (1 + 0.3 * fbm(x / (R * 0.25), z / (R * 0.25), seed + 9, 3));
    }
  return G;
}
/** VOLCANO: a cone (concave flanks) with a summit crater and radial gullies.
 *  o = {x, z, radius, height (rise), crater (radius fraction, 0.12), seed}. */
export function volcano(G, o) {
  const R = Math.max(G.cell, +o.radius || 0), H = +o.height || R * 0.3, cr = o.crater == null ? 0.12 : +o.crater, seed = o.seed | 0;
  const i0 = Math.max(0, Math.floor((o.x - R - G.x0) / G.cell)), i1 = Math.min(G.w - 1, Math.ceil((o.x + R - G.x0) / G.cell));
  const j0 = Math.max(0, Math.floor((o.z - R - G.z0) / G.cell)), j1 = Math.min(G.hh - 1, Math.ceil((o.z + R - G.z0) / G.cell));
  for (let j = j0; j <= j1; j++)
    for (let i = i0; i <= i1; i++) {
      const x = gx(G, i), z = gz(G, j), dx = x - o.x, dz = z - o.z, ang = Math.atan2(dz, dx);
      const r = Math.hypot(dx, dz) / R;
      if (r >= 1) continue;
      let v = Math.pow(1 - r, 1.8) * (1 + 0.6 * r * (1 - r));
      // radial gullies: ridged in angle, deeper down the flank
      const gully = ridged(ang * 9 / Math.PI + 31, r * 3, seed + 13, 3);
      v *= 1 - 0.18 * smoothstep(0.15, 0.6, r) * (1 - gully);
      if (r < cr * 1.6) {   // the summit crater
        const q = r / cr;
        const rim = Math.pow(1 - cr, 1.8) * (1 + 0.6 * cr * (1 - cr));
        v = q < 1 ? rim - 0.35 * cr * 3 * (1 - q * q) : lerp(rim, v, smoothstep(1, 1.6, q));
      }
      G.h[j * G.w + i] += H * v;
    }
  return G;
}
/** Carve along a polyline: the cross-section `section(u)` (u = |d| / half-width,
 *  0..1, returns 0..1 = how far down to the floor) toward a floor that is
 *  `depth` below the SMOOTHED ground along the path (or absolute `floorY`),
 *  falling monotonically from the first point to the last. */
function carvePath(G, pts, halfW, section, floorOf) {
  const C = curveCoords(G, pts.map(p => [p[0], p[1]]), halfW, 0);
  for (let n = 0; n < G.w * G.hh; n++) {
    const d = C.d[n];
    if (!isFinite(d)) continue;
    const u = Math.min(1, Math.sqrt(d * d + C.beyond[n] * C.beyond[n]) / halfW);
    if (u >= 1) continue;
    const k = section(u), fl = floorOf(C.s[n]);
    if (G.h[n] > fl) G.h[n] = lerp(G.h[n], fl, k);
  }
  return G;
}
/** The floor profile along a path: `depth` below the path's smoothed ground,
 *  made non-increasing from the start (a valley drains one way). */
function pathFloor(G, pts, depth, floorY) {
  const T = SP.tessellate(pts.map(p => [p[0], p[1]]), {step: G.cell, smooth: true});
  const ss = T.s, fl = T.pts.map(p => sampleGrid(G, p[0], p[1]));
  const sm = blur(Float32Array.from(fl), fl.length, 1, Math.max(1, Math.round(fl.length / 20)), 2);
  let run = Infinity;
  const mono = Array.from(sm, v => { run = Math.min(run, v - depth); return floorY != null ? Math.min(run, floorY) : run; });
  // smooth the running minimum's kinks (a blur of a non-increasing profile stays non-increasing)
  const out = blur(Float32Array.from(mono), mono.length, 1, Math.max(1, Math.round(mono.length / 12)), 3);
  return s => {
    if (s <= 0) return out[0];
    let lo = 0, hi = ss.length - 1;
    while (hi - lo > 1) { const m = (lo + hi) >> 1; if (ss[m] <= s) lo = m; else hi = m; }
    const f = (s - ss[lo]) / Math.max(1e-6, ss[hi] - ss[lo]);
    return out[lo] + (out[hi] - out[lo]) * clamp(f, 0, 1);
  };
}
/** CANYON: a meandering, TERRACED gorge cut into the ground along `points`
 *  (or across `box`), its rim indented by SIDE CANYONS. o = {points | box,
 *  width (half-width at the rim), depth, steps (terrace bands, 0 = smooth),
 *  side (0..1, side canyons), meander, minY, seed}. */
export function canyon(G, o) {
  const pts = chaikinPts(o.points || boxPath(o.box, o.meander == null ? 0.6 : o.meander, o.seed | 0), 2);
  const W = Math.max(G.cell * 2, +o.width || 0), depth = +o.depth || W, steps = o.steps == null ? 4 : o.steps | 0, seed = o.seed | 0;
  const side = o.side == null ? 0.6 : +o.side;
  const f0 = pathFloor(G, pts, depth), floor = o.minY == null ? f0 : (s => Math.max(o.minY, f0(s)));
  const C = curveCoords(G, pts.map(p => [p[0], p[1]]), W * 1.9, 0);
  for (let n = 0; n < G.w * G.hh; n++) {
    const d = C.d[n];
    if (!isFinite(d)) continue;
    const sv = C.s[n], ad = Math.sqrt(d * d + C.beyond[n] * C.beyond[n]);
    // the rim: a wavering half-width, pushed out where a side canyon comes in
    let Wl = W * (1 + 0.22 * noise1(sv / (W * 2.5), seed + 3));
    if (side > 0) {
      const g = ridged(sv / (W * 1.6), (d < 0 ? 37 : 0) + ad / (W * 2.2), seed + 5, 2);
      Wl *= 1 + 0.55 * side * smoothstep(0.55, 0.95, g) * smoothstep(0.2, 0.7, ad / W);
    }
    const u = ad / Wl;
    if (u >= 1) continue;
    let v = clamp((1 - u) / 0.72, 0, 1);       // the floor: the inner ~28 %
    if (steps > 0) v = clamp(terraceH(v, 0, 1 / steps, 3.2), 0, 1);
    const fl = floor(sv);
    if (G.h[n] > fl) G.h[n] = lerp(G.h[n], fl, v);
  }
  return G;
}
/** GLACIAL VALLEY / FJORD: the U-shaped trough of a glacier along `points`
 *  (or across `box`): steep walls, a broad flat floor, hanging side-valley
 *  lips. fjord = the floor dug `depth` below the SEA (opts.seaY) so the sea
 *  floods it. o = {points | box, width, depth, fjord, seaY, meander, seed}. */
export function uValley(G, o) {
  const pts = o.points || boxPath(o.box, o.meander == null ? 0.35 : o.meander, o.seed | 0);
  const W = Math.max(G.cell * 2, +o.width || 0), depth = +o.depth || W * 0.4;
  const f0 = o.fjord ? pathFloor(G, pts, 0, (o.seaY || 0) - depth) : pathFloor(G, pts, depth);
  const floor = o.minY == null ? f0 : (s => Math.max(o.minY, f0(s)));
  return carvePath(G, chaikinPts(pts, 2), W, u => 1 - Math.pow(u, 3.5), floor);
}
function chaikinPts(pts, it) { return HY.chaikin(pts.map(p => [p[0], p[1]]), it, false); }
/** MESA FIELD: flat-topped mesas and buttes over a box, their sides terraced
 *  cliffs over a talus apron, their outlines ragged (two octaves of warp).
 *  o = {box, height (rise of the tops), size (mean radius), density (0..1),
 *  steps, seed}. */
export function mesaField(G, o) {
  const [x0, z0, x1, z1] = o.box, R = Math.max(G.cell * 2, +o.size || 2000), H = +o.height || R * 0.4;
  const dens = clamp(o.density == null ? 0.5 : +o.density, 0, 1), steps = o.steps == null ? 2 : o.steps | 0, seed = o.seed | 0;
  const cellSz = R * 2.4, rnd = rng(seed * 977 + 13);
  const cx0 = Math.floor(x0 / cellSz), cx1 = Math.floor(x1 / cellSz), cz0 = Math.floor(z0 / cellSz), cz1 = Math.floor(z1 / cellSz);
  const mesas = [];
  for (let cz = cz0; cz <= cz1; cz++)
    for (let cx = cx0; cx <= cx1; cx++) {
      const keep = rnd() <= dens, mx = (cx + 0.2 + rnd() * 0.6) * cellSz, mz = (cz + 0.2 + rnd() * 0.6) * cellSz, r = R * (0.3 + rnd() * 1.0), h = H * (0.6 + rnd() * 0.4);
      if (keep) mesas.push({x: mx, z: mz, r, h, sd: seed + mesas.length * 31});
    }
  for (const m of mesas) {
    if (m.x < x0 || m.x > x1 || m.z < z0 || m.z > z1) continue;
    const reach = m.r * 1.8;
    const i0 = Math.max(0, Math.floor((m.x - reach - G.x0) / G.cell)), i1 = Math.min(G.w - 1, Math.ceil((m.x + reach - G.x0) / G.cell));
    const j0 = Math.max(0, Math.floor((m.z - reach - G.z0) / G.cell)), j1 = Math.min(G.hh - 1, Math.ceil((m.z + reach - G.z0) / G.cell));
    const top = sampleGrid(G, m.x, m.z) + m.h;
    for (let j = j0; j <= j1; j++)
      for (let i = i0; i <= i1; i++) {
        const x = gx(G, i), z = gz(G, j);
        let [wx, wz] = warp((x - m.x) / m.r, (z - m.z) / m.r, m.sd + 17, 0.32, 3);
        [wx, wz] = warp(wx * 3.1, wz * 3.1, m.sd + 19, 0.35, 2);
        const r = Math.hypot(wx / 3.1, wz / 3.1);
        if (r >= 1.7) continue;
        // a flat top to 0.85, a cliff to 1.0, a talus apron to 1.7
        let k = r < 0.85 ? 1 : r < 1 ? 1 - smoothstep(0.85, 1, r) * 0.72 : 0.28 * Math.pow(1 - smoothstep(1, 1.7, r), 1.5);
        if (steps > 0 && r >= 0.85 && r < 1) k = 0.28 + 0.72 * clamp(terraceH(1 - (r - 0.85) / 0.15, 0, 1 / steps, 4), 0, 1);
        const n = j * G.w + i;
        const tt = top - (r < 0.85 ? m.h * 0.04 * ridged(x / (m.r * 0.5), z / (m.r * 0.5), m.sd + 23, 2) : 0);
        const v = G.h[n] + (tt - G.h[n]) * k;
        if (v > G.h[n]) G.h[n] = v;
      }
  }
  return G;
}
/** DUNES: asymmetric transverse dunes over a box -- a gentle stoss slope up
 *  wind, a steep lee (slip face) down wind -- with sinuous crests that fork
 *  and die out (Y-junctions where the amplitude fades). o = {box, height,
 *  wavelength, wind (deg, the direction the wind blows TOWARD: 0 = +x east,
 *  90 = +z south), seed}. */
export function dunes(G, o) {
  const [x0, z0, x1, z1] = o.box, H = +o.height || 200, L = Math.max(G.cell * 3, +o.wavelength || 2000), seed = o.seed | 0;
  const a = (o.wind == null ? 0 : +o.wind) * Math.PI / 180, wx = Math.cos(a), wz = Math.sin(a);
  const edge = Math.min(x1 - x0, z1 - z0) * 0.12;
  for (let j = 0; j < G.hh; j++)
    for (let i = 0; i < G.w; i++) {
      const x = gx(G, i), z = gz(G, j);
      if (x < x0 || x > x1 || z < z0 || z > z1) continue;
      const fade = smoothstep(0, edge, Math.min(x - x0, x1 - x, z - z0, z1 - z));
      const along = (x * wx + z * wz) / L, across = (-x * wz + z * wx) / L;
      // sinuous crests: a slow warp across the wind, a faster wobble on top
      const t = along + 0.55 * fbm(across * 0.28, along * 0.12, seed + 3, 3) + 0.12 * fbm(across * 1.1, along * 0.5, seed + 5, 2);
      const f = t - Math.floor(t), b = Math.floor(t);
      // stoss rises over 0..0.75, the slip face drops over 0.75..1
      const prof = f < 0.75 ? Math.pow(f / 0.75, 1.3) : 1 - smoothstep(0.75, 1, f);
      // each crest's height wanders along it and fades out in places (forks)
      const amp = H * clamp(0.6 + 0.7 * fbm(across * 0.45 + b * 3.7, b * 0.9, seed + 9, 2), 0, 1.2) * fade;
      G.h[j * G.w + i] += amp * prof;
    }
  return G;
}
/** An edge MASK: lowers the land toward the box's edge so the generated
 *  ground is an island / a continent in its sea. o = {power (1 = gentle, 3 =
 *  sharp), margin (fraction), seaY, depth (how far below the sea the edge
 *  goes), warp, seed}. */
export function edgeMask(G, o) {
  const p = o.power || 1.5, sea = o.seaY || 0, depth = o.depth == null ? 800 : +o.depth, margin = o.margin == null ? 0.1 : +o.margin, seed = o.seed | 0;
  const wv = o.warp == null ? 0.4 : +o.warp;
  for (let j = 0; j < G.hh; j++)
    for (let i = 0; i < G.w; i++) {
      let u = i / Math.max(1, G.w - 1) * 2 - 1, v = j / Math.max(1, G.hh - 1) * 2 - 1;
      // a fractal coast: a broad warp plus a finer one (bays, headlands)
      const u1 = u + wv * fbm(u * 1.1 + 3, v * 1.1, seed + 3, 3), v1 = v + wv * fbm(u * 1.1, v * 1.1 + 7, seed + 7, 3);
      u = u1 + wv * 0.35 * fbm(u1 * 4.5 + 1, v1 * 4.5, seed + 11, 4); v = v1 + wv * 0.35 * fbm(u1 * 4.5, v1 * 4.5 + 2, seed + 13, 4);
      // an OPEN side (o.open: any of 'n', 's', 'e', 'w') is not masked: the land
      // runs off the map there (a peninsula's mainland)
      if (o.open) {
        if (o.open.includes('n') && v < 0) v = 0;
        if (o.open.includes('s') && v > 0) v = 0;
        if (o.open.includes('w') && u < 0) u = 0;
        if (o.open.includes('e') && u > 0) u = 0;
      }
      const r = Math.max(Math.abs(u), Math.abs(v)) * 0.55 + Math.hypot(u, v) * 0.45;
      const m = 1 - smoothstep(1 - margin * 2 - 0.35 / p, 1 - margin, r);
      const n = j * G.w + i, k = Math.pow(m, p);
      G.h[n] = (sea - depth) + (G.h[n] - (sea - depth)) * k;
    }
  return G;
}
/** Stream-power + thermal erosion of a whole grid (no uplift): valleys cut
 *  into whatever is there. o = {strength (0..1), iters, seaY, talusDeg, seed}. */
export function erodeGrid(G, o = {}) {
  const s = clamp(o.strength == null ? 0.5 : +o.strength, 0, 1), iters = o.iters || Math.round(10 + 50 * s);
  let lo = Infinity, hi = -Infinity;
  for (const v of G.h) { lo = Math.min(lo, v); hi = Math.max(hi, v); }
  // K so the biggest valley (a tenth of the grid draining) cuts ~ s * a third of the relief
  const N = G.w * G.hh, Aref = N * 0.1, rel = Math.max(1, hi - Math.max(lo, o.seaY == null ? lo : o.seaY));
  const slope = rel / Math.max(G.w, G.hh) * 2;
  const K = (s * rel * 0.33 / iters) / (Math.sqrt(Aref) * slope);
  HY.streamPower(G.h, G.w, G.hh, G.cell, {iters, K, U: 0, seaY: o.seaY, jitter: 0.4, seed: o.seed | 0,
                                          talusDeg: o.talusDeg || 38, thermalPasses: 1, fillEvery: 8, progress: o.progress});
  return G;
}

/** EROSION OF LAND as a landscape-evolution run (erodeRange on the land's
 *  relief over the sea): the land rises from low under an uplift shaped like
 *  itself while stream power cuts it, then its local maxima are put back --
 *  so the heights a template designed stay, and the land gets a dendritic
 *  drainage net with valleys reaching every divide. Cells under the sea are
 *  base level. o = {strength (0..1), seaY, seed, scale (the feature size in
 *  voxels the gain works over; default a tenth of the grid), progress}. */
export function erodeLand(G, o = {}) {
  const sea = o.seaY == null ? -Infinity : o.seaY, N = G.w * G.hh;
  const s = clamp(o.strength == null ? 0.6 : +o.strength, 0, 1);
  if (s <= 0) return G;
  const base = new Float32Array(N), rel = new Float32Array(N);
  for (let n = 0; n < N; n++) { const b = Math.min(G.h[n], sea); base[n] = b; rel[n] = Math.max(0, G.h[n] - b); }
  const W0 = o.scale || Math.max(G.w, G.hh) * G.cell * 0.1;
  const z = erodeRange(base, rel, G.w, G.hh, G.x0, G.z0, G.cell, W0, (o.seed | 0) + 101, s, {seaY: o.seaY, progress: o.progress, tune: o.tune});
  for (let n = 0; n < N; n++) if (rel[n] > 0.25) G.h[n] = lerp(G.h[n], z[n], Math.min(1, s * 1.4));
  return G;
}

/* ---- TEMPLATES: Azgaar-style op lists ---------------------------------------------------------
 * A template is a list of ops run in order on a grid that covers the target
 * (the whole map, or a box). Coordinates are FRACTIONS of the grid (0..1),
 * heights METRES above the sea (seaY + m * vpm), sizes fractions of the grid's
 * larger side. Ranges become real range splines (rangeField). Every random
 * choice is drawn from rng(seed) in op order, so a template + seed is a
 * fixed world. */
export const TEMPLATES = {
  continent: [
    {op: 'fill', h: -90},
    {op: 'hill', count: 4, h: [220, 420], r: [0.16, 0.3], x: [0.25, 0.75], z: [0.25, 0.75]},
    {op: 'range', count: 2, h: [1300, 1800], len: [0.3, 0.55], w: [0.07, 0.1], x: [0.2, 0.8], z: [0.2, 0.8]},
    {op: 'range', count: 2, h: [600, 900], len: [0.15, 0.3], w: [0.05, 0.07], x: [0.15, 0.85], z: [0.15, 0.85]},
    {op: 'hill', count: 14, h: [40, 140], r: [0.04, 0.1], x: [0.1, 0.9], z: [0.1, 0.9]},
    {op: 'pit', count: 4, h: [40, 120], r: [0.04, 0.08], x: [0.2, 0.8], z: [0.2, 0.8]},
    {op: 'trough', count: 1, h: [120, 200], len: [0.25, 0.45], w: [0.03, 0.05], x: [0.2, 0.8], z: [0.2, 0.8]},
    {op: 'noise', h: 70, size: 0.06, style: 'ridged', land: 0.6},
    {op: 'mask', power: 1.4, margin: 0.06},
    {op: 'erode', strength: 0.6},
  ],
  island: [
    {op: 'fill', h: -120},
    {op: 'hill', count: 1, h: [400, 600], r: [0.28, 0.34], x: [0.45, 0.55], z: [0.45, 0.55]},
    {op: 'range', count: 1, h: [1100, 1500], len: [0.25, 0.4], w: [0.07, 0.09], x: [0.35, 0.65], z: [0.35, 0.65]},
    {op: 'hill', count: 8, h: [60, 200], r: [0.05, 0.12], x: [0.25, 0.75], z: [0.25, 0.75]},
    {op: 'pit', count: 2, h: [60, 140], r: [0.04, 0.07], x: [0.3, 0.7], z: [0.3, 0.7]},
    {op: 'noise', h: 60, size: 0.05, style: 'ridged', land: 0.6},
    {op: 'mask', power: 2.2, margin: 0.12},
    {op: 'erode', strength: 0.6},
  ],
  archipelago: [
    {op: 'fill', h: -110},
    {op: 'hill', count: 14, h: [140, 420], r: [0.05, 0.12], x: [0.1, 0.9], z: [0.1, 0.9]},
    {op: 'volcano', count: 2, h: [500, 900], r: [0.05, 0.08], x: [0.2, 0.8], z: [0.2, 0.8]},
    {op: 'hill', count: 20, h: [40, 160], r: [0.02, 0.05], x: [0.05, 0.95], z: [0.05, 0.95]},
    {op: 'strait', count: 2, h: [80, 160], w: [0.02, 0.04]},
    {op: 'noise', h: 50, size: 0.04, style: 'ridged', land: 0.7},
    {op: 'mask', power: 1, margin: 0.05},
    {op: 'erode', strength: 0.5},
  ],
  peninsula: [
    {op: 'fill', h: -100},
    {op: 'hill', count: 5, h: [200, 380], r: [0.14, 0.2], x: [0.38, 0.62], z: [0.0, 0.7]},
    {op: 'range', count: 1, h: [1200, 1700], len: [0.5, 0.65], w: [0.07, 0.09], x: [0.46, 0.54], z: [0.3, 0.42], dir: [80, 100]},
    {op: 'hill', count: 10, h: [50, 150], r: [0.04, 0.09], x: [0.3, 0.7], z: [0.05, 0.8]},
    {op: 'trough', count: 1, h: [100, 200], len: [0.2, 0.3], w: [0.03, 0.04], x: [0.35, 0.65], z: [0.2, 0.6]},
    {op: 'noise', h: 70, size: 0.06, style: 'ridged', land: 0.6},
    {op: 'mask', power: 1.6, margin: 0.08, open: 'n'},
    {op: 'erode', strength: 0.6},
  ],
  highlands: [
    {op: 'fill', h: 160},
    {op: 'hill', count: 8, h: [150, 350], r: [0.1, 0.22], x: [0.1, 0.9], z: [0.1, 0.9]},
    {op: 'range', count: 3, h: [1400, 2000], len: [0.3, 0.5], w: [0.07, 0.1], x: [0.15, 0.85], z: [0.15, 0.85]},
    {op: 'pit', count: 6, h: [80, 180], r: [0.04, 0.08], x: [0.1, 0.9], z: [0.1, 0.9]},
    {op: 'trough', count: 2, h: [150, 300], len: [0.3, 0.5], w: [0.03, 0.05], x: [0.2, 0.8], z: [0.2, 0.8]},
    {op: 'noise', h: 90, size: 0.06, style: 'ridged', land: 0.5},
    {op: 'erode', strength: 0.7},
  ],
};
export const TEMPLATE_NAMES = Object.keys(TEMPLATES);
export const TEMPLATE_OPS = ['fill', 'hill', 'pit', 'range', 'trough', 'strait', 'mask', 'smooth', 'add', 'multiply', 'noise', 'erode',
                             'volcano', 'crater', 'mesas', 'dunes', 'canyon', 'glacial', 'fjord'];

/** Run an op list on G (in place). opts = {seed, seaY, vpm (voxels per metre,
 *  10), progress(fraction)}. Returns {G, ranges: the range splines it drew
 *  (world voxels, ranges[] entries)}. */
export function runTemplate(G, ops, opts = {}) {
  const sea = opts.seaY || 0, vpm = opts.vpm || 10, R = rng(((opts.seed | 0) * 2654435761) >>> 0 ^ 0x5bd1e995);
  const between = (a, d) => (Array.isArray(a) ? a[0] + (a[1] - a[0]) * R() : a == null ? d : +a);
  const W = (G.w - 1) * G.cell, Hh = (G.hh - 1) * G.cell, S = Math.max(W, Hh);
  const X = f => G.x0 + f * W, Z = f => G.z0 + f * Hh;
  const ranges = [];
  const total = ops.length;
  ops.forEach((o, k) => {
    const count = Math.max(1, o.count | 0 || 1);
    const sub = p => opts.progress && opts.progress((k + p) / total);
    switch (o.op) {
      case 'fill': { const v = sea + between(o.h, 0) * vpm; G.h.fill(v); break; }
      case 'add': { const v = between(o.h, 0) * vpm; for (let n = 0; n < G.h.length; n++) G.h[n] += v; break; }
      case 'multiply': { const f = between(o.f, 1); for (let n = 0; n < G.h.length; n++) if (G.h[n] > sea) G.h[n] = sea + (G.h[n] - sea) * f; break; }
      case 'smooth': { G.h = blur(G.h, G.w, G.hh, Math.max(1, o.r | 0 || 1), o.passes || 2); break; }
      case 'noise': {
        // land relief: ridged (crests) or fbm, scaled by how high the land already is
        // (o.land: 0 = everywhere alike, 1 = in proportion to the height over the sea)
        const amp = between(o.h, 50) * vpm, size = between(o.size, 0.1) * S, sd = (R() * 1e6) | 0, land = o.land == null ? 0 : +o.land;
        let top = sea + 1;
        if (land > 0) for (const v of G.h) top = Math.max(top, v);
        for (let j = 0; j < G.hh; j++)
          for (let i = 0; i < G.w; i++) {
            const n = j * G.w + i, x = gx(G, i) / size, z = gz(G, j) / size;
            const v = o.style === 'ridged' ? ridged(...warp(x, z, sd + 3, 0.5), sd, 6) * 2 - 0.9 : fbm(x, z, sd, 6);
            const k = land > 0 ? lerp(1, clamp((G.h[n] - sea) / (top - sea), 0, 1), land) : 1;
            G.h[n] += amp * v * k;
          }
        break;
      }
      case 'hill': case 'pit': {
        // a BLOB: warped distance, so the outline is organic, a smooth dome inside
        for (let c = 0; c < count; c++) {
          const h = between(o.h, 200) * vpm * (o.op === 'pit' ? -1 : 1), r = between(o.r, 0.1) * S;
          const cx = X(between(o.x, 0.5)), cz = Z(between(o.z, 0.5)), sd = (R() * 1e6) | 0;
          const i0 = Math.max(0, Math.floor((cx - r * 1.5 - G.x0) / G.cell)), i1 = Math.min(G.w - 1, Math.ceil((cx + r * 1.5 - G.x0) / G.cell));
          const j0 = Math.max(0, Math.floor((cz - r * 1.5 - G.z0) / G.cell)), j1 = Math.min(G.hh - 1, Math.ceil((cz + r * 1.5 - G.z0) / G.cell));
          for (let j = j0; j <= j1; j++)
            for (let i = i0; i <= i1; i++) {
              const [wx, wz] = warp((gx(G, i) - cx) / r, (gz(G, j) - cz) / r, sd, 0.45, 4);
              const d = Math.hypot(wx, wz);
              if (d >= 1.25) continue;
              const k2 = Math.max(0, 1 - d * d / 1.5625);
              const n = j * G.w + i, dv = h * k2 * k2 * (0.7 + 0.6 * fbm(gx(G, i) / (r * 0.4), gz(G, j) / (r * 0.4), sd + 1, 5));
              // a pit is a basin in the LAND: it never digs below the sea floor that is there
              G.h[n] = o.op === 'pit' ? Math.max(G.h[n] + dv, Math.min(G.h[n], sea - 2 * vpm)) : G.h[n] + dv;
            }
        }
        break;
      }
      case 'range': case 'trough': {
        for (let c = 0; c < count; c++) {
          const len = between(o.len, 0.3) * S, hw = between(o.w, 0.05) * S, crest = between(o.h, 1000) * vpm;
          const cx = X(between(o.x, 0.5)), cz = Z(between(o.z, 0.5));
          const dir = (o.dir ? between(o.dir, 0) : R() * 360) * Math.PI / 180;
          const sd = (R() * 1e6) | 0, nk = 5;
          // a random-walk crest: knots along the direction, each turned a little
          const pts = [];
          let ang = dir, px = cx - Math.cos(dir) * len / 2, pz = cz - Math.sin(dir) * len / 2;
          for (let q = 0; q < nk; q++) {
            const hv = q === 0 || q === nk - 1 ? 0.7 : 0.85 + 0.15 * R();
            if (o.op === 'range') pts.push([Math.round(px), Math.round(pz), Math.round(sea + crest * hv), Math.round(hw * (0.8 + 0.4 * R()))]);
            else pts.push([Math.round(px), Math.round(pz)]);
            ang += (R() - 0.5) * 0.9;
            px += Math.cos(ang) * len / (nk - 1); pz += Math.sin(ang) * len / (nk - 1);
          }
          if (o.op === 'range') {
            const r = {id: 'gen_range_' + ranges.length, profile: 'alpine', seed: sd % 100000, peaks: 0.6, spurs: 0.6, foothills: 0.5, erode: 0, points: pts};
            // absolute crest on the ground the range finds: lift it over the land there
            for (const p of r.points) p[2] = Math.round(Math.max(p[2], sampleGrid(G, p[0], p[1]) + crest * 0.5));
            // generated (and eroded) on the sub-grid it can reach, pasted back
            r.erode = o.erode == null ? 0 : o.erode;
            const sg = subGrid(G, rangeBox(r));
            if (sg) {
              const out = rangeField(r, sg, {seaY: sea});
              sg.h = out.h;
              pasteGrid(G, sg);
            }
            ranges.push(r);
          } else uValley(G, {points: pts, width: hw, depth: crest, meander: 0, seed: sd, minY: sea + 15 * vpm});
        }
        break;
      }
      case 'strait': {
        for (let c = 0; c < count; c++) {
          const vert = R() < 0.5, f = 0.25 + 0.5 * R(), hw = between(o.w, 0.03) * S, depth = between(o.h, 100) * vpm, sd = (R() * 1e6) | 0;
          const box = vert ? [X(f) - hw, G.z0, X(f) + hw, G.z0 + Hh] : [G.x0, Z(f) - hw, G.x0 + W, Z(f) + hw];
          const pts = boxPath(box, 0, sd).map((p, q, a) => (vert ? [p[0] + hw * 3 * noise1(q / a.length * 3, sd), p[1]] : [p[0], p[1] + hw * 3 * noise1(q / a.length * 3, sd)]));
          const C = curveCoords(G, pts, hw * 1.5, 0);
          for (let n = 0; n < G.h.length; n++) {
            const d = C.d[n];
            if (!isFinite(d)) continue;
            const u = Math.abs(d) / (hw * 1.5);
            if (u < 1) G.h[n] = lerp(G.h[n], Math.min(G.h[n], sea - depth), 1 - smoothstep(0.4, 1, u));
          }
        }
        break;
      }
      case 'mask': edgeMask(G, {power: o.power, margin: o.margin, open: o.open, seaY: sea, depth: o.depth == null ? 900 : o.depth * vpm, seed: (R() * 1e6) | 0}); break;
      case 'erode': erodeLand(G, {strength: o.strength, seaY: sea, seed: (R() * 1e6) | 0, scale: S * 0.12, progress: sub}); break;
      case 'volcano': case 'crater': {
        for (let c = 0; c < count; c++) {
          const x = X(between(o.x, 0.5)), z = Z(between(o.z, 0.5)), r = between(o.r, 0.06) * S, h = between(o.h, 600) * vpm, sd = (R() * 1e6) | 0;
          if (o.op === 'volcano') volcano(G, {x, z, radius: r, height: h, seed: sd});
          else crater(G, {x, z, radius: r, depth: h, seed: sd});
        }
        break;
      }
      case 'mesas': mesaField(G, {box: [X(between(o.x0, 0)), Z(between(o.z0, 0)), X(between(o.x1, 1)), Z(between(o.z1, 1))], height: between(o.h, 150) * vpm,
                                  size: between(o.r, 0.04) * S, density: o.density, seed: (R() * 1e6) | 0}); break;
      case 'dunes': dunes(G, {box: [X(between(o.x0, 0)), Z(between(o.z0, 0)), X(between(o.x1, 1)), Z(between(o.z1, 1))], height: between(o.h, 20) * vpm,
                              wavelength: between(o.len, 0.02) * S, wind: o.wind, seed: (R() * 1e6) | 0}); break;
      case 'canyon': case 'glacial': case 'fjord': {
        const box = [X(between(o.x0, 0.2)), Z(between(o.z0, 0.4)), X(between(o.x1, 0.8)), Z(between(o.z1, 0.6))];
        const width = between(o.w, 0.03) * S, depth = between(o.h, 200) * vpm, sd = (R() * 1e6) | 0;
        if (o.op === 'canyon') canyon(G, {box, width, depth, seed: sd});
        else uValley(G, {box, width, depth, fjord: o.op === 'fjord', seaY: sea, seed: sd});
        break;
      }
      default: throw new Error('unknown template op ' + o.op);
    }
    sub(1);
  });
  return {G, ranges};
}

/* ---- shading ------------------------------------------------------------------------------ */
const LAND = [[0, [96, 128, 78]], [0.08, [128, 150, 92]], [0.25, [176, 168, 120]], [0.45, [156, 128, 96]],
              [0.65, [132, 122, 114]], [0.85, [190, 188, 186]], [1, [246, 246, 250]]];
const SEA = [[0, [118, 168, 206]], [1, [28, 58, 104]]];
function ramp(stops, t, out) {
  let k = 0;
  while (k < stops.length - 2 && t > stops[k + 1][0]) k++;
  const [t0, a] = stops[k], [t1, b] = stops[k + 1], u = clamp((t - t0) / (t1 - t0), 0, 1);
  out[0] = a[0] + (b[0] - a[0]) * u; out[1] = a[1] + (b[1] - a[1]) * u; out[2] = a[2] + (b[2] - a[2]) * u;
}
/** RGBA bytes of grid heights h (w x hh, cells `dx` voxels apart): a Horn
 *  (3x3) gradient hillshade lit from the NW at 45 degrees over a hypsometric
 *  tint. opts = {seaY, lo, hi (the tint's range), zf (exaggeration, 1),
 *  tint (0..1, how much colour under the shade), mask (only these cells:
 *  others transparent), alpha (0..255)}. */
export function shade(h, w, hh, dx, opts = {}) {
  const sea = opts.seaY == null ? -Infinity : opts.seaY;
  let lo = opts.lo, hi = opts.hi;
  if (lo == null || hi == null) { lo = Infinity; hi = -Infinity; for (let n = 0; n < h.length; n++) { lo = Math.min(lo, h[n]); hi = Math.max(hi, h[n]); } }
  const zf = opts.zf == null ? 1 : opts.zf, tint = opts.tint == null ? 0.65 : opts.tint, alpha = opts.alpha == null ? 255 : opts.alpha;
  const out = new Uint8ClampedArray(w * hh * 4), col = [0, 0, 0];
  const az = 315 * Math.PI / 180, alt = 45 * Math.PI / 180;
  const lx = Math.sin(az) * Math.cos(alt), lz = -Math.cos(az) * Math.cos(alt), ly = Math.sin(alt);
  const H = (i, j) => h[clamp(j, 0, hh - 1) * w + clamp(i, 0, w - 1)];
  for (let j = 0; j < hh; j++)
    for (let i = 0; i < w; i++) {
      const n = j * w + i, o = n * 4;
      if (opts.mask && !opts.mask[n]) continue;
      // Horn: dz/dx and dz/dy from the 3x3 window
      const a = H(i - 1, j - 1), b = H(i, j - 1), c = H(i + 1, j - 1), d = H(i - 1, j), f = H(i + 1, j), g = H(i - 1, j + 1), k = H(i, j + 1), m = H(i + 1, j + 1);
      const gxv = ((c + 2 * f + m) - (a + 2 * d + g)) / (8 * dx) * zf, gzv = ((g + 2 * k + m) - (a + 2 * b + c)) / (8 * dx) * zf;
      // normal (-gx, 1, -gz) normalised; light from the NW (x west = -, z north = -)
      const len = Math.sqrt(gxv * gxv + 1 + gzv * gzv);
      let s = (-gxv * -lx + ly + -gzv * lz) / len;
      s = clamp(s, 0, 1);
      const v = h[n];
      if (v < sea) ramp(SEA, clamp((sea - v) / Math.max(1, sea - lo), 0, 1), col);
      else ramp(LAND, clamp((v - Math.max(lo, sea)) / Math.max(1, hi - Math.max(lo, sea)), 0, 1), col);
      const sh = 0.18 + 1.0 * s;
      const grey = 255 * sh * 0.8;
      for (let q = 0; q < 3; q++) out[o + q] = (1 - tint) * grey + tint * col[q] * sh;
      out[o + 3] = alpha;
    }
  return out;
}
