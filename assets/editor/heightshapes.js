/* heightshapes.js — the HEIGHT AUTHORING MATHS of the World map page
 * (docs/PLAN_map_v2.md, package E2): pure, DOM-free, Node-runnable. Every
 * function here computes target heights; maptiles.js owns the plane and
 * writes them through writeWindow, so every tool gets the same tile-dirty
 * tracking and per-stroke undo.
 *
 *   falloffs    the brush edge: smooth (cosine), linear, flat (a hard core
 *               with a short soft rim), sphere.
 *   noise2      seeded 2D gradient noise, integer-hashed lattice (no Math.random,
 *               no platform trig): the same seed and column give the same value
 *               in every browser and in Node.
 *   terrace     the quantised target of the Terrace / Cliff brush.
 *   PROFILES    ridge / valley cross-sections (V, U, round, sharp).
 *   bakeLine    a ridge or valley LINE written into the height plane.
 *   SHAPES      the v1 landform site kinds as editor stamps (peak, ridge, basin,
 *               plateau, volcano, mesa) -- they write the plane, they are not
 *               sites any more (the format section of the plan).
 *
 * Heights are VOXELS, absolute Y, like the plane; lengths are voxels. The
 * page converts metres at the edge (map.json terrain.refVoxelsPerMetre). */

const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
export const smoothstep = (e0, e1, x) => { const t = clamp((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); };

/* ---- brush falloffs: t = distance / radius in [0, 1); 1 at the centre, 0 at the rim ---- */
export const FALLOFFS = {
  smooth: t => 0.5 * (1 + Math.cos(Math.PI * t)),
  linear: t => 1 - t,
  flat: t => (t < 0.8 ? 1 : 0.5 * (1 + Math.cos(Math.PI * (t - 0.8) / 0.2))),
  sphere: t => Math.sqrt(Math.max(0, 1 - t * t)),
};
export const FALLOFF_NAMES = Object.keys(FALLOFFS);
export function falloffFn(name) { return FALLOFFS[name] || FALLOFFS.smooth; }

/* ---- seeded gradient noise ---------------------------------------------------------- */
function hash3(a, b, c) {
  let h = Math.imul(a | 0, 0x27d4eb2d) ^ Math.imul(b | 0, 0x165667b1) ^ Math.imul(c | 0, 0x9e3779b1);
  h ^= h >>> 15; h = Math.imul(h, 0x85ebca6b);
  h ^= h >>> 13; h = Math.imul(h, 0xc2b2ae35);
  return (h ^ (h >>> 16)) >>> 0;
}
// eight unit-ish gradients: the axes and the diagonals (no trig at runtime)
const GX = [1, -1, 0, 0, 0.7071067811865476, -0.7071067811865476, 0.7071067811865476, -0.7071067811865476];
const GZ = [0, 0, 1, -1, 0.7071067811865476, 0.7071067811865476, -0.7071067811865476, -0.7071067811865476];
const fade = t => t * t * t * (t * (t * 6 - 15) + 10);
/** Gradient noise at (x, z) in lattice units, about -1..1. */
export function noise2(x, z, seed) {
  const ix = Math.floor(x), iz = Math.floor(z), fx = x - ix, fz = z - iz;
  const g = (i, j, dx, dz) => { const h = hash3(ix + i, iz + j, seed) & 7; return GX[h] * dx + GZ[h] * dz; };
  const u = fade(fx), v = fade(fz);
  const a = g(0, 0, fx, fz), b = g(1, 0, fx - 1, fz), c = g(0, 1, fx, fz - 1), d = g(1, 1, fx - 1, fz - 1);
  return 1.41 * (a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v);
}
/** Five octaves of noise2 at world column (x, z): `size` is the largest
 *  feature in voxels. style 'fbm' is -1..1 rolling relief; 'ridged' is 0..1
 *  with sharp crests (1 - |n|, squared), the shape of a mountain range. */
export function fbm2(x, z, size, seed, style) {
  let f = 1 / Math.max(1, size), amp = 1, sum = 0, norm = 0;
  for (let o = 0; o < 5; o++) {
    let n = noise2(x * f, z * f, (seed + o * 1013) | 0);
    if (style === 'ridged') { n = 1 - Math.abs(n); n *= n; }
    sum += n * amp; norm += amp;
    amp *= 0.5; f *= 2;
  }
  const v = sum / norm;
  return style === 'ridged' ? v * 2 - 1 : v;
}

/* ---- terrace / cliff ------------------------------------------------------------------ */
/** The terrace target of height h: bands `step` voxels tall measured from
 *  `datum` (the sea). sharpness 0 = soft steps (a smoothstep per band);
 *  towards 1 each band is a flat shelf and a cliff of (1 - sharpness) of the
 *  band's width -- 0.9 = 90 % shelf, 10 % cliff. */
export function terrace(h, datum, step, sharpness) {
  if (!(step > 0)) return h;
  const s = clamp(sharpness, 0, 0.98);
  const rel = (h - datum) / step, b = Math.floor(rel), f = rel - b;
  let q = clamp((f - s) / (1 - s), 0, 1);
  q = q * q * (3 - 2 * q);
  return datum + (b + q) * step;
}

/* ---- line cross-sections: t = distance from the line / width in [0, 1) ---------------- */
export const PROFILES = {
  V: t => 1 - t,                          // straight flanks
  U: t => 1 - t * t * t * t,              // a broad back (ridge) or a flat floor (valley), steep sides
  round: t => 0.5 * (1 + Math.cos(Math.PI * t)),
  sharp: t => (1 - t) * (1 - t),          // a knife edge / a slot gorge, flanks easing out
};
export const PROFILE_NAMES = Object.keys(PROFILES);

/** Distance field of a polyline over a post window: for each post of
 *  [i0, i0+w) x [j0, j0+h), the distance to the nearest point of the line
 *  and the line height there (lerped along the nearest segment).
 *  pts: [[x, z, y], ...] world voxels. Posts farther than `reach` get Infinity. */
export function lineField(P, pts, reach, i0, j0, w, h) {
  const D = new Float32Array(w * h).fill(Infinity), Y = new Float32Array(w * h);
  const segs = pts.length === 1 ? [[pts[0], pts[0]]] : pts.slice(1).map((p, k) => [pts[k], p]);
  for (const [a, b] of segs) {
    const ex = b[0] - a[0], ez = b[1] - a[1], L2 = ex * ex + ez * ez;
    const sx0 = Math.max(i0, Math.floor((Math.min(a[0], b[0]) - reach - P.min[0]) / P.sp));
    const sx1 = Math.min(i0 + w - 1, Math.ceil((Math.max(a[0], b[0]) + reach - P.min[0]) / P.sp));
    const sz0 = Math.max(j0, Math.floor((Math.min(a[1], b[1]) - reach - P.min[1]) / P.sp));
    const sz1 = Math.min(j0 + h - 1, Math.ceil((Math.max(a[1], b[1]) + reach - P.min[1]) / P.sp));
    for (let j = sz0; j <= sz1; j++) {
      const z = P.min[1] + j * P.sp;
      for (let i = sx0; i <= sx1; i++) {
        const x = P.min[0] + i * P.sp;
        let t = L2 > 0 ? ((x - a[0]) * ex + (z - a[1]) * ez) / L2 : 0;
        t = clamp(t, 0, 1);
        const dx = x - (a[0] + ex * t), dz = z - (a[1] + ez * t);
        const d = Math.sqrt(dx * dx + dz * dz);
        const n = (j - j0) * w + (i - i0);
        if (d < reach && d < D[n]) { D[n] = d; Y[n] = a[2] + (b[2] - a[2]) * t; }
      }
    }
  }
  return {D, Y};
}
/** The post rectangle [i0, j0, w, h] a line of half-width `width` can touch. */
export function lineRect(P, pts, width) {
  let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
  for (const p of pts) { x0 = Math.min(x0, p[0]); z0 = Math.min(z0, p[1]); x1 = Math.max(x1, p[0]); z1 = Math.max(z1, p[1]); }
  const i0 = Math.max(0, Math.floor((x0 - width - P.min[0]) / P.sp)), j0 = Math.max(0, Math.floor((z0 - width - P.min[1]) / P.sp));
  const i1 = Math.min(P.posts[0] - 1, Math.ceil((x1 + width - P.min[0]) / P.sp)), j1 = Math.min(P.posts[1] - 1, Math.ceil((z1 + width - P.min[1]) / P.sp));
  return [i0, j0, Math.max(0, i1 - i0 + 1), Math.max(0, j1 - j0 + 1)];
}
/** The writeWindow arguments that BAKE a ridge or valley line:
 *  {i0, j0, w, h, f}. A ridge only raises and a valley only lowers: each post
 *  within `width` of the line goes toward the line height by the profile's
 *  weight, new = cur + (Y - cur) * profile(d / width), kept only where it is
 *  above (ridge) / below (valley) the ground it lands on. So the line meets
 *  the land at its edge with the chosen cross-section, and baking a ridge
 *  over a higher hill leaves the hill alone. */
export function lineBake(P, line) {
  const pts = (line.points || []).filter(p => Array.isArray(p) && p.length >= 3);
  const width = Math.max(P.sp, +line.width || 0);
  if (!pts.length) return null;
  const [i0, j0, w, h] = lineRect(P, pts, width);
  if (!w || !h) return null;
  const {D, Y} = lineField(P, pts, width, i0, j0, w, h);
  const prof = PROFILES[line.profile] || PROFILES.round;
  const valley = line.kind === 'valley';
  const f = (n, cur) => {
    const d = D[n];
    if (!(d < width)) return null;
    const nv = cur + (Y[n] - cur) * prof(d / width);
    return valley ? (nv < cur ? nv : null) : (nv > cur ? nv : null);
  };
  return {i0, j0, w, h, f};
}

/* ---- shape stamps: the v1 landform kinds as plane writes --------------------------------
 * A stamp at (x, z) with radius R (voxels), height H (voxels), aspect (width /
 * length, 1 = round) and rotation (degrees; the long axis). Two families:
 *   ADDITIVE  peak, ridge, basin, volcano: new = cur + H * f(r) -- the land
 *             under it keeps its own shape (a peak on a slope is a peak on a
 *             slope), the edge meets the land with zero slope.
 *   LEVELLING plateau, mesa: new = cur + (top - cur) * W(r) -- a flat top at
 *             one height, the sides blending into whatever is there.
 * r is the elliptical radius in [0, 1). */
export const SHAPES = {
  peak:    {add: true, f: (r) => Math.pow(1 - r, 1.5)},
  // a crest along the long axis (u), held for its middle half, dropping at the ends
  ridge:   {add: true, aspect: 0.25, f: (r, u, v) => (1 - smoothstep(0.45, 1, Math.abs(u))) * Math.pow(Math.max(0, 1 - Math.abs(v)), 1.5),
            uv: true},
  basin:   {add: true, sign: -1, f: (r) => 1 - smoothstep(0.35, 1, r)},
  volcano: {add: true, f: (r) => {
    const rc = 0.2, cone = Math.pow(1 - r, 1.3);
    if (r >= rc) return cone;
    const rim = Math.pow(1 - rc, 1.3), q = r / rc;
    return rim - 0.42 * (1 - q * q);
  }},
  plateau: {add: false, f: (r) => 1 - smoothstep(0.55, 1, r)},
  mesa:    {add: false, f: (r) => 1 - smoothstep(0.8, 0.93, r)},
};
export const SHAPE_NAMES = Object.keys(SHAPES);
/** The writeWindow arguments of a shape stamp: {i0, j0, w, h, f}.
 *  o = {kind, x, z, radius, height, aspect, rot, top}: `height` is the rise
 *  (the depth for a basin) for the additive shapes; `top` is the absolute
 *  top for plateau / mesa. */
export function shapeStamp(P, o) {
  const S = SHAPES[o.kind];
  if (!S) throw new Error('unknown shape ' + o.kind);
  const R = Math.max(P.sp, +o.radius || 0);
  const asp = clamp(o.aspect == null ? (S.aspect || 1) : +o.aspect, 0.05, 1);
  const a = (+o.rot || 0) * Math.PI / 180, ca = Math.cos(a), sa = Math.sin(a);
  // the stamp's bounding box: the ellipse's own extent, rounded out to posts
  const ex = Math.sqrt((R * ca) ** 2 + (R * asp * sa) ** 2), ez = Math.sqrt((R * sa) ** 2 + (R * asp * ca) ** 2);
  const i0 = Math.max(0, Math.floor((o.x - ex - P.min[0]) / P.sp)), i1 = Math.min(P.posts[0] - 1, Math.ceil((o.x + ex - P.min[0]) / P.sp));
  const j0 = Math.max(0, Math.floor((o.z - ez - P.min[1]) / P.sp)), j1 = Math.min(P.posts[1] - 1, Math.ceil((o.z + ez - P.min[1]) / P.sp));
  const w = i1 - i0 + 1, h = j1 - j0 + 1;
  if (w <= 0 || h <= 0) return null;
  const H = (+o.height || 0) * (S.sign || 1);
  const f = (n, cur) => {
    const i = i0 + n % w, j = j0 + Math.floor(n / w);
    const dx = P.min[0] + i * P.sp - o.x, dz = P.min[1] + j * P.sp - o.z;
    const u = (dx * ca + dz * sa) / R, v = (-dx * sa + dz * ca) / (R * asp);
    const r = Math.sqrt(u * u + v * v);
    if (S.uv ? Math.abs(u) >= 1 || Math.abs(v) >= 1 : r >= 1) return null;
    const k = S.f(r, u, v);
    if (!(k > 0)) return null;
    return S.add ? cur + H * k : cur + (o.top - cur) * k;
  };
  return {i0, j0, w, h, f};
}
