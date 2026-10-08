/* erosion.js — hydraulic + thermal erosion of a height window for the World
 * map page's Erode tool (docs/PLAN_map_v2.md E2 item 4). Pure and DOM-free:
 * erosion_worker.js runs it off the page's thread, the erode BRUSH runs it
 * inline on a small window, scripts/test_heighttools.mjs runs it in Node.
 *
 * DETERMINISTIC FOR A SEED: the droplets come from a seeded counter generator
 * (mulberry32), every loop runs in a fixed order, and the thermal pass is a
 * Jacobi step (all moves computed from one snapshot, then applied), so the
 * result is a pure function of (heights, size, spacing, options). It is an
 * AUTHORING tool: floats are fine here, the output is rounded to i16 posts
 * before it reaches the engine.
 *
 * Units: the window holds heights in VOXELS on posts `sp` voxels apart. Both
 * passes work in post units (height / sp), so a slope of 1 is 45 degrees
 * whatever the map's post spacing.
 *
 * Hydraulic: particle (droplet) erosion after Beyer 2015 / Lague 2019 --
 * each droplet runs downhill with inertia, picks sediment up where it speeds
 * up and has capacity, drops it where it slows or climbs. It carves gullies
 * and fans, which is what reads as "eroded" from a map.
 * Thermal: material above the talus angle slumps to lower neighbours (scree
 * and smoothed cliff feet).
 *
 * The change fades to zero across the window's outer `feather` posts so an
 * eroded rectangle meets the land around it without a seam. */

/** A seeded generator of floats in [0, 1). */
export function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6D2B79F5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export const EROSION_DEFAULTS = {
  seed: 1, iterations: 6, strength: 0.5,   // passes; 0..1 scales erosion + deposition
  hydraulic: true, thermal: true,
  talusDeg: 38,                            // thermal: slopes steeper than this slump
  dropletsPerPost: 0.25,                   // per pass
  lifetime: 48, inertia: 0.05, capacity: 4, minSlope: 0.01,
  deposit: 0.3, erode: 0.3, evaporate: 0.02, gravity: 4,
  feather: 0.12,                           // of the window's smaller side, at least 3 posts
};

/** Erode heights `src` (Float32Array, w x h, voxels, posts `sp` apart) and
 *  return a NEW Float32Array. `progress(fraction)` is called between passes. */
export function erode(src, w, h, sp, opts, progress) {
  const o = {...EROSION_DEFAULTS, ...(opts || {})};
  const N = w * h, g = new Float32Array(N);
  for (let i = 0; i < N; i++) g[i] = src[i] / sp;
  const rnd = mulberry32((o.seed | 0) * 2654435761 + 12345);
  const strength = Math.max(0, Math.min(1, +o.strength));
  const passes = Math.max(0, Math.min(200, o.iterations | 0));
  const drops = Math.max(1, Math.round(N * o.dropletsPerPost));
  const talus = Math.tan(Math.max(1, Math.min(89, o.talusDeg)) * Math.PI / 180);
  const delta = new Float32Array(o.thermal ? N : 0);
  for (let p = 0; p < passes; p++) {
    if (o.hydraulic && w > 3 && h > 3) for (let d = 0; d < drops; d++) droplet(g, w, h, rnd, o, strength);
    if (o.thermal) thermal(g, w, h, talus, 0.5 * strength, delta);
    if (progress) progress((p + 1) / passes);
  }
  // back to voxels, the change feathered out at the window edge
  const fw = o.feather > 0 ? Math.max(3, Math.round(Math.min(w, h) * o.feather)) : 0;   // 0 = no feather
  const out = new Float32Array(N);
  for (let j = 0; j < h; j++)
    for (let i = 0; i < w; i++) {
      const e = Math.min(i, j, w - 1 - i, h - 1 - j);
      const m = e >= fw ? 1 : e <= 0 ? 0 : (e / fw) * (e / fw) * (3 - 2 * e / fw);
      const n = j * w + i;
      out[n] = src[n] + (g[n] * sp - src[n]) * m;
    }
  return out;
}

/** Height and gradient at (x, z) in post units, bilinear. */
function sampleGrad(g, w, x, z, out) {
  const i = Math.floor(x), j = Math.floor(z), u = x - i, v = z - j, n = j * w + i;
  const a = g[n], b = g[n + 1], c = g[n + w], d = g[n + w + 1];
  out[0] = (b - a) * (1 - v) + (d - c) * v;
  out[1] = (c - a) * (1 - u) + (d - b) * u;
  out[2] = a * (1 - u) * (1 - v) + b * u * (1 - v) + c * (1 - u) * v + d * u * v;
}
const SG = new Float64Array(3), SG2 = new Float64Array(3);
function droplet(g, w, h, rnd, o, strength) {
  let x = 1 + rnd() * (w - 3), z = 1 + rnd() * (h - 3);
  let dx = 0, dz = 0, speed = 1, water = 1, sed = 0;
  let px = x, pz = z;            // the last position inside the window
  for (let life = 0; life < o.lifetime; life++) {
    const i = Math.floor(x), j = Math.floor(z), u = x - i, v = z - j, n = j * w + i;
    px = x; pz = z;
    sampleGrad(g, w, x, z, SG);
    const h0 = SG[2];
    dx = dx * o.inertia - SG[0] * (1 - o.inertia);
    dz = dz * o.inertia - SG[1] * (1 - o.inertia);
    const len = Math.sqrt(dx * dx + dz * dz);
    if (len < 1e-9) break;
    dx /= len; dz /= len;
    x += dx; z += dz;
    if (x < 1 || z < 1 || x >= w - 2 || z >= h - 2) break;
    sampleGrad(g, w, x, z, SG2);
    const dh = SG2[2] - h0;
    const cap = Math.max(-dh * speed * water * o.capacity, o.minSlope);
    if (sed > cap || dh > 0) {
      // deposit: fill the pit we climbed into, or drop the excess
      const amt = dh > 0 ? Math.min(dh, sed) : (sed - cap) * o.deposit * strength;
      sed -= amt;
      g[n] += amt * (1 - u) * (1 - v); g[n + 1] += amt * u * (1 - v);
      g[n + w] += amt * (1 - u) * v; g[n + w + 1] += amt * u * v;
    } else {
      // erode no deeper than the drop we just made (a droplet never digs a hole)
      const amt = Math.min((cap - sed) * o.erode * strength, -dh);
      const w00 = (1 - u) * (1 - v), w10 = u * (1 - v), w01 = (1 - u) * v, w11 = u * v;
      g[n] -= amt * w00; g[n + 1] -= amt * w10; g[n + w] -= amt * w01; g[n + w + 1] -= amt * w11;
      sed += amt;
    }
    speed = Math.sqrt(Math.max(0, speed * speed - dh * o.gravity));
    water *= 1 - o.evaporate;
  }
  // A dying droplet drops what it carries where it stopped (Lague's lets it
  // vanish, which on mountain slopes measured a third of the cone gone).
  if (sed > 0) {
    const i = Math.floor(px), j = Math.floor(pz), u = px - i, v = pz - j, n = j * w + i;
    g[n] += sed * (1 - u) * (1 - v); g[n + 1] += sed * u * (1 - v);
    g[n + w] += sed * (1 - u) * v; g[n + w + 1] += sed * u * v;
  }
}
/** One Jacobi step of talus slumping: every post above `talus` (rise per
 *  post) relative to its lower 4-neighbours sheds k * (excess / 2), split by
 *  excess; all moves are computed from one snapshot. */
function thermal(g, w, h, talus, k, delta) {
  delta.fill(0);
  for (let j = 1; j < h - 1; j++)
    for (let i = 1; i < w - 1; i++) {
      const n = j * w + i, c = g[n];
      const e0 = c - g[n - 1] - talus, e1 = c - g[n + 1] - talus, e2 = c - g[n - w] - talus, e3 = c - g[n + w] - talus;
      const s = (e0 > 0 ? e0 : 0) + (e1 > 0 ? e1 : 0) + (e2 > 0 ? e2 : 0) + (e3 > 0 ? e3 : 0);
      if (s <= 0) continue;
      const mx = Math.max(e0, e1, e2, e3);
      const move = k * mx * 0.5;
      delta[n] -= move;
      if (e0 > 0) delta[n - 1] += move * e0 / s;
      if (e1 > 0) delta[n + 1] += move * e1 / s;
      if (e2 > 0) delta[n - w] += move * e2 / s;
      if (e3 > 0) delta[n + w] += move * e3 / s;
    }
  for (let n = 0; n < g.length; n++) g[n] += delta[n];
}
