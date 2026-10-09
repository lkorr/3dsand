/* climate.js — CLIMATE and AUTO-BIOMES for the World map page
 * (docs/PLAN_worldbuilder.md E2 item 4). Pure, DOM-free, typed arrays only:
 * it runs in the terrain worker (terrainjobs.js job 'climate'), on the page
 * and in Node (scripts/test_climate.mjs, scripts/render_climate_preview.mjs).
 *
 * THE MODEL, per biome cell (25.6 m on the shipped maps), from the authored
 * height plane and the map's `climate` inputs (map.json `climate`,
 * EDITOR-ONLY: the engine reads the biome plane this writes, never this):
 *
 *   TEMPERATURE (mean annual air, degrees C, the biome files' scale):
 *     T = seaTemp + (Tlat - seaTemp) * (1 - coastal * exp(-coastKm / coastReachKm))
 *         - lapse * max(0, height above sea) / 1 km
 *         - aspect * min(1, |slope| / 0.5) * (downslope . cold direction)
 *     Tlat = seaTemp - latGrad * (km from the map's centre toward the cold
 *     edge). Coastal land is pulled toward the sea's temperature (maritime:
 *     milder in the cold north, cooler in the hot south); a poleward-facing
 *     slope is colder, an equator-facing one warmer.
 *   PRECIPITATION (mm a year) by the PREVAILING-WIND MARCH: the grid is
 *     resampled into wind scanlines; along each, a parcel of vapour q (0..1)
 *     is topped up over the sea (and off the map: the ring outside is ocean)
 *     and rains out over land at a base rate per km plus an OROGRAPHIC term
 *     per km of climb (on the ground smoothed over ~1 km); descending air
 *     rains less. The parcel arrives over the lee already wrung out: the RAIN
 *     SHADOW. Scanlines are blurred across the wind, then brought back.
 *   WETNESS (0..1): MFD flow accumulation (hydrology.mfd, on the ~100 m
 *     proxy) and the distance to map.json rivers[] / lakes[].
 *   MOISTURE (0..1, the biome files' scale: precipitation / 3000 mm, standing
 *     water counts) = P / 3000 + wetness weight * (0.12 flow + 0.2 near water
 *     + 0.3 waterlogged: flat, low ground where the flow gathers).
 *
 * THE BIOME of a cell is the nearest neighbour in (T, M) space over every
 * biome's `climate` pair (weighted: wT on T in 10 degree units, wM on M in
 * 0.2 units), after:
 *   - HARD eligibility (BIOME_RULES): ocean only below sea level and nothing
 *     else there; glacier only very cold AND high; swamp / glowfen only on
 *     flat, wet, low ground; dunes only near the coast, low and gentle;
 *   - SOFT preferences (a penalty): mesa / karst / downs / alpine prefer their
 *     landform's slope and height range;
 *   - the map's NEVER list (climate.never: biomes the generator never picks,
 *     e.g. the fantasy ashlands / crystal barrens you place by hand);
 *   - JITTER: the sea-level temperature and the moisture are read at a
 *     domain-warped position and get a little fBm, so borders interlock
 *     instead of drawing contour stripes;
 *   - MIN PATCH: 4-connected patches under `minPatch` cells merge into the
 *     neighbouring biome they share most border with (if it is eligible), so
 *     there are no single-cell speckles.
 * LOCKED cells (painted by hand, climate.locks) keep their biome.
 *
 * Everything is deterministic: fixed loop orders, seeded noise. */

import * as TG from './terraingen.js';
import * as HY from './hydrology.js';

const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const D2R = Math.PI / 180;

/** THE MAP PAGE'S BIOME COLOURS by name (map.js COLOR is this table; a biome
 *  not here gets a stable hash colour there). Kept here, in the pure module,
 *  so the Node previews draw the same colours as the page. */
export const BIOME_COLORS = {
  forest: '#2f7a3a', meadow: '#8cc95a', pine: '#1f5a3a', desert: '#d9c27a',
  tundra: '#d8e6f0', swamp: '#6a7a2f', alpine: '#8f959c', ocean: '#2b5d9e',
  // worldbuilder B1's fourteen (2026-10-09)
  ashlands: '#4a4440', mesa: '#b5532f', moor: '#7d5a7a', glowfen: '#3f8a86', glacier: '#a9d8ec',
  savanna: '#c9a74a', dunes: '#e8d9a0', amberwood: '#c8742c', eldwood: '#1c3b24', crystal_barrens: '#b9a3d6',
  steppe: '#b7b37a', jungle: '#1f8a3c', downs: '#a9d18e', karst: '#9aa39a',
};

/** The climate inputs and their defaults (map.json `climate` overrides). */
export const DEFAULTS = {
  windFrom: 270,       // compass degrees the prevailing wind blows FROM (0 = north = -z, 90 = east = +x): 270 = westerlies
  seaTemp: 14,         // degrees C: mean annual sea-level air temperature on the map's centre line
  latGrad: 0.8,        // degrees C colder per km toward the cold edge (a fantasy map compresses latitude)
  coldDir: 0,          // compass degrees of the COLD edge (0 = north = -z)
  lapse: 6.5,          // degrees C per km of height above sea
  aspect: 1.5,         // degrees C: a steep poleward slope is colder by up to this, an equatorward one warmer
  coastal: 0.4,        // 0..1: how much coastal land is pulled toward the sea's temperature
  coastReachKm: 3,     // the reach of that pull
  rain: 1,             // rainfall multiplier
  orographic: 1,       // multiplier on the orographic (uplift) rain-out
  wetness: 1,          // weight of flow accumulation + rivers / lakes on moisture
  wT: 1,               // weight of temperature in the biome match
  wM: 1,               // weight of moisture in the biome match
  jitter: 0.5,         // 0..1: border jitter (warp + noise)
  minPatch: 12,        // cells: smaller patches merge into a neighbour
  seed: 1,
  glacierT: -6,        // glacier: at most this cold (degrees C) ...
  glacierAltM: 600,    // ... and at least this high (m above sea)
  never: ['ashlands', 'crystal_barrens'],
};
/** [key, label, min, max, step, unit, title] for the Climate panel. */
export const PARAM_ROWS = [
  ['windFrom', 'wind from', 0, 359, 5, '°', 'the prevailing wind’s direction, compass degrees it blows FROM (0 north, 90 east, 180 south, 270 west = westerlies)'],
  ['seaTemp', 'sea temp', -20, 35, 0.5, '°C', 'mean annual sea-level air temperature on the map’s centre line (the biome files’ scale: tundra -10, forest 10, desert 24)'],
  ['latGrad', 'latitude', 0, 5, 0.05, '°C/km', 'how much colder per km toward the cold edge (0 = no latitude)'],
  ['coldDir', 'cold edge', 0, 359, 5, '°', 'compass direction of the cold edge (0 = north = -z)'],
  ['lapse', 'lapse', 0, 15, 0.1, '°C/km', 'colder per km of height above sea (6.5 = the standard atmosphere)'],
  ['aspect', 'aspect', 0, 8, 0.25, '°C', 'a steep poleward slope is colder by up to this, an equatorward one warmer'],
  ['coastal', 'coastal', 0, 1, 0.05, '', 'how much coastal land is pulled toward the sea’s temperature (maritime climate)'],
  ['rain', 'rainfall', 0, 4, 0.05, '×', 'rainfall multiplier (1 = about 1,000 mm a year on a windward coast)'],
  ['orographic', 'orographic', 0, 4, 0.05, '×', 'how hard climbing air rains out: the windward wet / leeward dry contrast (rain shadow)'],
  ['wetness', 'wetness', 0, 3, 0.05, '×', 'weight of flow accumulation and rivers / lakes (map.json) on moisture'],
  ['wT', 'T weight', 0, 4, 0.05, '', 'weight of temperature in the nearest-biome match'],
  ['wM', 'M weight', 0, 4, 0.05, '', 'weight of moisture in the nearest-biome match'],
  ['jitter', 'jitter', 0, 1, 0.05, '', 'border jitter: biomes interlock instead of following contour lines'],
  ['minPatch', 'min patch', 1, 400, 1, 'cells', 'patches smaller than this merge into a neighbour (no speckles)'],
  ['seed', 'seed', 0, 999999, 1, '', 'the jitter’s seed'],
  ['glacierT', 'glacier T', -40, 10, 0.5, '°C', 'glacier only where at most this cold ...'],
  ['glacierAltM', 'glacier alt', 0, 5000, 50, 'm', '... and at least this high above sea'],
];
/** map.json `climate` -> a complete, clamped parameter object. */
export function params(cl) {
  const o = {...DEFAULTS};
  const src = cl && typeof cl === 'object' ? cl : {};
  for (const [k, , lo, hi] of PARAM_ROWS) if (typeof src[k] === 'number' && isFinite(src[k])) o[k] = clamp(src[k], lo, hi);
  if (Array.isArray(src.never)) o.never = src.never.filter(s => typeof s === 'string');
  return o;
}

/* ---- the biome rules ---------------------------------------------------------------------- */
/** c = {sea, T, M, altM, slope (deg), coastKm, wet (0..1)}. `hard` = may the
 *  biome be here at all; `pen` = a penalty added to the squared distance. */
export const BIOME_RULES = {
  ocean: {hard: c => c.sea},
  glacier: {hard: (c, p) => c.T <= p.glacierT && c.altM >= p.glacierAltM},
  swamp: {hard: c => c.slope <= 5 && c.altM <= 250 && c.wet >= 0.25},
  glowfen: {hard: c => c.slope <= 5 && c.altM <= 300 && c.wet >= 0.25},
  dunes: {hard: c => c.coastKm <= 1.2 && c.altM <= 80 && c.slope <= 15, pen: c => 2 * Math.max(0, c.coastKm - 0.4) ** 2},
  // the LANDFORM biomes: a small base penalty (the climate must fit well) plus
  // their slope / height range -- mesa = dry hill country cut into steps,
  // karst = steep knolls and towers, downs = rolling low chalk
  mesa: {pen: c => 0.15 + band(c.slope, 8, 55, 4) + band(c.altM, 80, 2200, 150)},
  karst: {pen: c => 0.15 + band(c.slope, 14, 65, 5) + band(c.altM, 30, 1500, 100)},
  downs: {pen: c => 0.15 + band(c.slope, 1.5, 16, 4) + band(c.altM, 10, 600, 150)},
  alpine: {pen: c => band(c.altM, 700, 1e9, 400)},
  // rain alone does not make a jungle: past the temperate biomes' moisture a
  // wet 15 degree slope is temperate rainforest (eldwood), not tropical
  jungle: {pen: c => band(c.T, 19, 1e9, 2.5)},
};
/** 0 inside [lo, hi], growing as (outside / soft)^2 outside, capped at 3. */
function band(x, lo, hi, soft) {
  const d = x < lo ? (lo - x) / soft : x > hi ? (x - hi) / soft : 0;
  return Math.min(3, d * d);
}
/** May biome `name` stand on cell c? (every non-ocean biome: only above the sea) */
export function eligible(name, c, p) {
  if (name === 'ocean') return !!c.sea;
  if (c.sea) return false;
  const r = BIOME_RULES[name];
  return !r || !r.hard || !!r.hard(c, p);
}
export function penalty(name, c, p) {
  const r = BIOME_RULES[name];
  return r && r.pen ? r.pen(c, p) : 0;
}

/* ---- fields ----------------------------------------------------------------------------- */
/** Two-pass 8-neighbour chamfer distance (1, sqrt 2) in CELLS to the nearest
 *  set cell of `seed`; Infinity everywhere when nothing is set. */
export function distanceField(seed, w, hh) {
  const N = w * hh, d = new Float32Array(N).fill(Infinity), S = Math.SQRT2;
  for (let n = 0; n < N; n++) if (seed[n]) d[n] = 0;
  for (let j = 0; j < hh; j++)
    for (let i = 0; i < w; i++) {
      const n = j * w + i;
      let v = d[n];
      if (v === 0) continue;
      if (i > 0) v = Math.min(v, d[n - 1] + 1);
      if (j > 0) {
        v = Math.min(v, d[n - w] + 1);
        if (i > 0) v = Math.min(v, d[n - w - 1] + S);
        if (i < w - 1) v = Math.min(v, d[n - w + 1] + S);
      }
      d[n] = v;
    }
  for (let j = hh - 1; j >= 0; j--)
    for (let i = w - 1; i >= 0; i--) {
      const n = j * w + i;
      let v = d[n];
      if (v === 0) continue;
      if (i < w - 1) v = Math.min(v, d[n + 1] + 1);
      if (j < hh - 1) {
        v = Math.min(v, d[n + w] + 1);
        if (i < w - 1) v = Math.min(v, d[n + w + 1] + S);
        if (i > 0) v = Math.min(v, d[n + w - 1] + S);
      }
      d[n] = v;
    }
  return d;
}
const bilin = (a, w, hh, x, z) => {
  x = clamp(x, 0, w - 1); z = clamp(z, 0, hh - 1);
  const i = Math.max(0, Math.min(w - 2, Math.floor(x))), j = Math.max(0, Math.min(hh - 2, Math.floor(z)));
  const i1 = Math.min(w - 1, i + 1), j1 = Math.min(hh - 1, j + 1), u = x - i, v = z - j;
  const t = a[j * w + i] + (a[j * w + i1] - a[j * w + i]) * u, b = a[j1 * w + i] + (a[j1 * w + i1] - a[j1 * w + i]) * u;
  return t + (b - t) * v;
};
// mm a year per unit of vapour rained out per km (a saturated parcel at the
// base rate gives ~1,000 mm on a windward coast)
const MM_PER_Q = 25000, BASE_PER_KM = 0.04, ORO_PER_KM = 0.7, PICKUP_PER_KM = 0.5, FLOOR_MM = 60, RECYCLE_Q = 0.3, RECYCLE_PER_KM = 0.08;
/** Annual precipitation (mm) on grid h (w x hh, `cell` voxels) by the
 *  prevailing-wind march (see the header). o = {seaY, vpm, params}. */
export function precipitation(h, w, hh, cell, o) {
  const p = o.params || DEFAULTS, sea = o.seaY || 0, vpm = o.vpm || 10, cellKm = cell / vpm / 1000;
  const N = w * hh, hc = new Float32Array(N);
  for (let n = 0; n < N; n++) hc[n] = Math.max(h[n], sea);
  // the ground the air feels: smoothed ~0.6 km (a gully does not make a rain shadow)
  const hs = TG.blur(hc, w, hh, Math.max(1, Math.round(0.6 / cellKm)), 2);
  const a = p.windFrom * D2R, wx = -Math.sin(a), wz = Math.cos(a), nx = -wz, nz = wx;
  const cx = (w - 1) / 2, cz = (hh - 1) / 2;
  let umin = Infinity, umax = -Infinity, vmin = Infinity, vmax = -Infinity;
  for (const [x, z] of [[0, 0], [w - 1, 0], [0, hh - 1], [w - 1, hh - 1]]) {
    const u = (x - cx) * wx + (z - cz) * wz, v = (x - cx) * nx + (z - cz) * nz;
    umin = Math.min(umin, u); umax = Math.max(umax, u); vmin = Math.min(vmin, v); vmax = Math.max(vmax, v);
  }
  const u0 = umin - 2, v0 = vmin - 1, Nu = Math.ceil(umax - u0) + 3, Nv = Math.ceil(vmax - v0) + 3;
  const rot = new Float32Array(Nu * Nv);
  const pick = 1 - Math.exp(-PICKUP_PER_KM * cellKm), baseSea = 1 - Math.exp(-0.5 * BASE_PER_KM * cellKm);
  const oro = ORO_PER_KM * p.orographic, scale = MM_PER_Q * p.rain / cellKm, recyc = 1 - Math.exp(-RECYCLE_PER_KM * cellKm);
  for (let b = 0; b < Nv; b++) {
    let q = 1, prev = sea;
    for (let k = 0; k < Nu; k++) {
      const u = u0 + k, v = v0 + b;
      const x = cx + u * wx + v * nx, z = cz + u * wz + v * nz;
      const inside = x >= 0 && z >= 0 && x <= w - 1 && z <= hh - 1;
      const H = inside ? bilin(hs, w, hh, x, z) : sea;
      const wet = !inside || bilin(h, w, hh, x, z) < sea;
      let rain;
      if (wet) {
        q += (1 - q) * pick;
        rain = q * baseSea;
        q -= rain;
      } else {
        const dh = (H - prev) / vpm / 1000;                 // km climbed this step
        const lee = dh < 0 ? Math.max(0.3, 1 + 4 * dh / cellKm) : 1;
        rain = q * (1 - Math.exp(-(BASE_PER_KM * cellKm * lee + oro * Math.max(0, dh))));
        q -= rain;
        // land recycles a little vapour (evapotranspiration): a long lee dries to
        // a semi-arid interior, not to nothing
        if (q < RECYCLE_Q) q += (RECYCLE_Q - q) * recyc;
      }
      rot[b * Nu + k] = rain * scale;
      prev = H;
    }
  }
  // across the wind: the scanlines are independent, blur them together (~1.5 km)
  const rb = Math.max(1, Math.round(1.5 / cellKm)), tmp = new Float32Array(Nv);
  for (let pass = 0; pass < 2; pass++)
    for (let k = 0; k < Nu; k++) {
      let s = 0;
      for (let b = -rb; b <= rb; b++) s += rot[clamp(b, 0, Nv - 1) * Nu + k];
      for (let b = 0; b < Nv; b++) {
        tmp[b] = s / (2 * rb + 1);
        s += rot[clamp(b + rb + 1, 0, Nv - 1) * Nu + k] - rot[clamp(b - rb, 0, Nv - 1) * Nu + k];
      }
      for (let b = 0; b < Nv; b++) rot[b * Nu + k] = tmp[b];
    }
  const out = new Float32Array(N);
  for (let j = 0; j < hh; j++)
    for (let i = 0; i < w; i++) {
      const u = (i - cx) * wx + (j - cz) * wz, v = (i - cx) * nx + (j - cz) * nz;
      out[j * w + i] = Math.max(0, bilin(rot, Nu, Nv, u - u0, v - v0)) + (h[j * w + i] >= sea ? FLOOR_MM * p.rain : 0);
    }
  return out;
}

/** Rasterise map.json rivers[] ([[x, z, w?, d?], ...]) and lakes[] (polygons)
 *  onto grid G (cell-centred samples at x0 + i * cell): 1 = water. */
export function rasterWater(G, rivers, lakes) {
  const m = new Uint8Array(G.w * G.hh);
  const toI = x => (x - G.x0) / G.cell, toJ = z => (z - G.z0) / G.cell;
  const disc = (fi, fj, r) => {
    const R = Math.max(0.5, r);
    for (let j = Math.floor(fj - R); j <= Math.ceil(fj + R); j++)
      for (let i = Math.floor(fi - R); i <= Math.ceil(fi + R); i++)
        if (i >= 0 && j >= 0 && i < G.w && j < G.hh && (i - fi) ** 2 + (j - fj) ** 2 <= R * R) m[j * G.w + i] = 1;
  };
  for (const r of rivers || []) {
    const P = (r && Array.isArray(r.points) ? r.points : []).filter(p => Array.isArray(p) && p.length >= 2);
    let lastW = 64;
    for (let k = 0; k < P.length; k++) {
      if (P[k].length >= 3 && isFinite(P[k][2])) lastW = +P[k][2];
      if (k === 0) { disc(toI(P[0][0]), toJ(P[0][1]), lastW / 2 / G.cell); continue; }
      const a = P[k - 1], b = P[k], L = Math.hypot(b[0] - a[0], b[1] - a[1]);
      const n = Math.max(1, Math.ceil(L / (G.cell * 0.5)));
      for (let q = 1; q <= n; q++) { const t = q / n; disc(toI(a[0] + (b[0] - a[0]) * t), toJ(a[1] + (b[1] - a[1]) * t), lastW / 2 / G.cell); }
    }
  }
  for (const L of lakes || []) {
    const P = (L && Array.isArray(L.points) ? L.points : []).filter(p => Array.isArray(p) && p.length >= 2);
    if (P.length < 3) continue;
    let j0 = Infinity, j1 = -Infinity;
    for (const p of P) { j0 = Math.min(j0, toJ(p[1])); j1 = Math.max(j1, toJ(p[1])); }
    for (let j = Math.max(0, Math.floor(j0)); j <= Math.min(G.hh - 1, Math.ceil(j1)); j++) {
      const z = G.z0 + j * G.cell, xs = [];
      for (let k = 0; k < P.length; k++) {
        const a = P[k], b = P[(k + 1) % P.length];
        if ((a[1] <= z) !== (b[1] <= z)) xs.push(a[0] + (z - a[1]) / (b[1] - a[1]) * (b[0] - a[0]));
      }
      xs.sort((p, q) => p - q);
      for (let k = 0; k + 1 < xs.length; k += 2)
        for (let i = Math.max(0, Math.ceil(toI(xs[k]))); i <= Math.min(G.w - 1, Math.floor(toI(xs[k + 1]))); i++) m[j * G.w + i] = 1;
    }
  }
  return m;
}

/** Every climate field on grid G = {w, hh, x0, z0, cell, h} (biome cells).
 *  o = {seaY, vpm, params, centre: [x, z] (the latitude datum; default the
 *  grid's centre), water: Uint8Array (rasterWater) | null, progress}.
 *  Returns {T, Tsl (sea-level part), P (mm), M, W (wetness 0..1), wetFlow,
 *  near, altM, slope (deg), coastKm, sea (Uint8Array)}. */
export function climateFields(G, o = {}) {
  const p = o.params || DEFAULTS, sea = o.seaY || 0, vpm = o.vpm || 10;
  const {w, hh, cell, h} = G, N = w * hh, cellM = cell / vpm;
  const prog = o.progress || (() => {});
  const seaM = new Uint8Array(N), landM = new Uint8Array(N);
  for (let n = 0; n < N; n++) { if (h[n] < sea) seaM[n] = 1; else landM[n] = 1; }
  const dSea = distanceField(seaM, w, hh), dLand = distanceField(landM, w, hh);
  const coastKm = new Float32Array(N);
  for (let n = 0; n < N; n++) coastKm[n] = (seaM[n] ? dLand[n] : dSea[n]) * cellM / 1000;
  prog(0.1);
  // the cold direction (compass -> x, z) and the latitude datum
  const ca = p.coldDir * D2R, cdx = Math.sin(ca), cdz = -Math.cos(ca);
  const cen = o.centre || [G.x0 + (w - 1) / 2 * cell, G.z0 + (hh - 1) / 2 * cell];
  const altM = new Float32Array(N), slope = new Float32Array(N), Tsl = new Float32Array(N), T = new Float32Array(N);
  // ASPECT reads the ground smoothed over ~250 m: a whole valley side faces the
  // sun or not; the facing of every gully side would hatch the map
  const hA = TG.blur(h, w, hh, Math.max(1, Math.round(250 / cellM)), 2);
  for (let j = 0; j < hh; j++)
    for (let i = 0; i < w; i++) {
      const n = j * w + i;
      const x = G.x0 + i * cell, z = G.z0 + j * cell;
      const dKm = ((x - cen[0]) * cdx + (z - cen[1]) * cdz) / vpm / 1000;
      const Tlat = p.seaTemp - p.latGrad * dKm;
      const ck = isFinite(coastKm[n]) ? coastKm[n] : 1e9;
      Tsl[n] = p.seaTemp + (Tlat - p.seaTemp) * (1 - (seaM[n] ? 1 : p.coastal * Math.exp(-ck / Math.max(0.01, p.coastReachKm))));
      altM[n] = (h[n] - sea) / vpm;
      const gx = (h[j * w + Math.min(w - 1, i + 1)] - h[j * w + Math.max(0, i - 1)]) / ((Math.min(w - 1, i + 1) - Math.max(0, i - 1)) * cell || 1);
      const gz = (h[Math.min(hh - 1, j + 1) * w + i] - h[Math.max(0, j - 1) * w + i]) / ((Math.min(hh - 1, j + 1) - Math.max(0, j - 1)) * cell || 1);
      const g = Math.hypot(gx, gz);
      slope[n] = Math.atan(g) / D2R;
      const ax = (hA[j * w + Math.min(w - 1, i + 1)] - hA[j * w + Math.max(0, i - 1)]) / ((Math.min(w - 1, i + 1) - Math.max(0, i - 1)) * cell || 1);
      const az = (hA[Math.min(hh - 1, j + 1) * w + i] - hA[Math.max(0, j - 1) * w + i]) / ((Math.min(hh - 1, j + 1) - Math.max(0, j - 1)) * cell || 1);
      const ga = Math.hypot(ax, az);
      // downslope . cold: > 0 = the slope faces the cold edge
      const face = ga > 1e-6 ? (-ax * cdx - az * cdz) / ga : 0;
      T[n] = Tsl[n] - p.lapse * Math.max(0, altM[n]) / 1000 - (seaM[n] ? 0 : p.aspect * Math.min(1, ga / 0.4) * face);
    }
  // the REGIONAL slope (~150 m): what the landform rules read, so a biome
  // follows the hill country, not every hillside's facing
  const slopeR = TG.blur(slope, w, hh, Math.max(1, Math.round(150 / cellM)), 2);
  prog(0.2);
  // the coarse proxy (~100 m, at most ~80k cells) for the wind march and the flow
  let f = Math.max(1, Math.round(1000 / cell));
  while (Math.ceil(w / f) * Math.ceil(hh / f) > 80000) f *= 2;
  const D = HY.downsample(h, w, hh, f);
  const Pc = precipitation(D.h, D.w, D.hh, cell * f, {seaY: sea, vpm, params: p});
  prog(0.45);
  const F = HY.mfd(D.h, D.w, D.hh, cell * f, {seaY: sea});
  const ckm2 = (cell * f / vpm / 1000) ** 2, wfC = new Float32Array(D.w * D.hh);
  for (let n = 0; n < wfC.length; n++) wfC[n] = D.h[n] < sea ? 0 : clamp(Math.log(F.A[n] * ckm2 / 0.05) / Math.log(1000), 0, 1);
  prog(0.7);
  const P = f > 1 ? HY.upsample(Pc, D.w, D.hh, f, w, hh) : Pc;
  const wetFlow = f > 1 ? HY.upsample(wfC, D.w, D.hh, f, w, hh) : wfC;
  const near = new Float32Array(N);
  if (o.water) {
    const dw = distanceField(o.water, w, hh);
    for (let n = 0; n < N; n++) near[n] = isFinite(dw[n]) ? Math.exp(-dw[n] * cellM / 250) : 0;
  }
  const M = new Float32Array(N), W = new Float32Array(N);
  for (let n = 0; n < N; n++) {
    if (P[n] < 0) P[n] = 0;
    wetFlow[n] = clamp(wetFlow[n], 0, 1);
    W[n] = Math.max(wetFlow[n], near[n]);
    // WATERLOGGING: flat, low ground where the water gathers holds it (the
    // biome files' moisture counts standing water: swamp 0.6, glowfen 0.7)
    const logged = (1 - TG.smoothstep(2, 8, slopeR[n])) * TG.smoothstep(0.15, 0.6, W[n]) * (1 - TG.smoothstep(150, 400, altM[n]));
    M[n] = seaM[n] ? 1 : clamp(P[n] / 3000 + p.wetness * (0.12 * wetFlow[n] + 0.2 * near[n] + 0.3 * logged), 0, 1);
  }
  prog(0.8);
  return {T, Tsl, P, M, W, wetFlow, near, altM, slope, slopeR, coastKm, sea: seaM, proxy: f};
}

/* ---- classification ----------------------------------------------------------------------- */
/** The biome of every cell of `rect` ([i0, j0, w, h] in G's cells; default
 *  all): an index into `table` ([{name, T, M}]), 255 = none / outside.
 *  o = {params, current (Uint8Array table index | 255 per cell: what is
 *  there now, kept on locked cells and read across the rect's edge),
 *  locks (Uint8Array | null), vpm}. Returns {biome, Tj, Mj} (the jittered
 *  T and M the match used). */
export function classify(F, G, table, o = {}) {
  const p = o.params || DEFAULTS, vpm = o.vpm || 10, {w, hh} = G, N = w * hh;
  const [ri, rj, rw, rh] = o.rect || [0, 0, w, hh];
  const cur = o.current || null, locks = o.locks || null;
  const never = new Set(p.never || []);
  const cand = [];
  table.forEach((b, k) => { if (!never.has(b.name) && isFinite(b.T) && isFinite(b.M)) cand.push(k); });
  const out = new Uint8Array(N).fill(255), Tj = new Float32Array(N), Mj = new Float32Array(N);
  const seed = p.seed | 0, jit = clamp(p.jitter, 0, 1);
  const warpCells = jit * 1000 * vpm / G.cell, nsz = 900 * vpm, wsz = 2500 * vpm;
  const wT = p.wT, wM = p.wM;
  const c = {sea: false, T: 0, M: 0, altM: 0, slope: 0, coastKm: 0, wet: 0};
  const feat = n => {
    c.sea = !!F.sea[n]; c.T = Tj[n]; c.M = Mj[n]; c.altM = F.altM[n]; c.slope = F.slopeR ? F.slopeR[n] : F.slope[n]; c.coastKm = F.coastKm[n]; c.wet = F.W[n];
    return c;
  };
  for (let j = rj; j < rj + rh; j++)
    for (let i = ri; i < ri + rw; i++) {
      if (i < 0 || j < 0 || i >= w || j >= hh) continue;
      const n = j * w + i, x = G.x0 + i * G.cell, z = G.z0 + j * G.cell;
      // the jitter: warp where the sea-level temperature and the moisture are read, plus fBm
      let ti = i, tj = j, dT = 0, dM = 0;
      if (jit > 0) {
        ti = i + warpCells * TG.fbm(x / wsz, z / wsz, seed + 11, 4);
        tj = j + warpCells * TG.fbm(x / wsz + 7.3, z / wsz + 2.1, seed + 23, 4);
        dT = jit * 3.5 * TG.fbm(x / nsz, z / nsz, seed + 37, 4);
        dM = jit * 0.08 * TG.fbm(x / nsz + 3.7, z / nsz + 9.2, seed + 41, 4);
      }
      Tj[n] = bilin(F.Tsl, w, hh, ti, tj) + (F.T[n] - F.Tsl[n]) + dT;
      Mj[n] = F.sea[n] ? 1 : clamp(bilin(F.M, w, hh, ti, tj) + dM, 0, 1);
      if (locks && locks[n] && cur) { out[n] = cur[n]; continue; }
      const cc = feat(n);
      let best = 255, bd = Infinity;
      for (const k of cand) {
        const b = table[k];
        if (!eligible(b.name, cc, p)) continue;
        // wetter than a biome's own is less wrong than drier (a forest takes more rain; a desert never takes less)
        const a = (cc.T - b.T) / 10, m = (cc.M - b.M) / 0.2 * (cc.M > b.M ? 0.6 : 1);
        const d = wT * a * a + wM * m * m + penalty(b.name, cc, p);
        if (d < bd) { bd = d; best = k; }
      }
      out[n] = best === 255 && cur ? cur[n] : best;
    }
  if (p.minPatch > 1) mergeSmall(out, G, table, {...o, rect: [ri, rj, rw, rh], feat, params: p});
  return {biome: out, Tj, Mj};
}
/** Merge 4-connected patches under params.minPatch cells (inside the rect,
 *  never a locked cell) into the neighbouring biome they share the most
 *  border with -- outside the rect the neighbour is `current` -- if every
 *  cell of the patch may hold it. Two passes. */
function mergeSmall(out, G, table, o) {
  const p = o.params, {w, hh} = G, [ri, rj, rw, rh] = o.rect, cur = o.current, locks = o.locks;
  const inR = (i, j) => i >= ri && j >= rj && i < ri + rw && j < rh + rj && i >= 0 && j >= 0 && i < w && j < hh;
  const valAt = (i, j) => (inR(i, j) ? out[j * w + i] : cur ? cur[j * w + i] : 255);
  const lab = new Int32Array(w * hh), stack = new Int32Array(w * hh), cells = [];
  const NI = [1, -1, 0, 0], NJ = [0, 0, 1, -1];
  for (let pass = 0; pass < 2; pass++) {
    lab.fill(0);
    let L = 0, merged = 0;
    for (let j = Math.max(0, rj); j < Math.min(hh, rj + rh); j++)
      for (let i = Math.max(0, ri); i < Math.min(w, ri + rw); i++) {
        const n0 = j * w + i;
        if (lab[n0] || (locks && locks[n0])) continue;
        const b0 = out[n0];
        L++;
        let sp = 0;
        cells.length = 0;
        stack[sp++] = n0; lab[n0] = L;
        let big = false;
        while (sp) {
          const n = stack[--sp];
          cells.push(n);
          if (cells.length >= p.minPatch) { big = true; }
          const ci = n % w, cj = (n / w) | 0;
          for (let k = 0; k < 4; k++) {
            const ii = ci + NI[k], jj = cj + NJ[k];
            if (!inR(ii, jj)) continue;
            const nb = jj * w + ii;
            if (lab[nb] || out[nb] !== b0 || (locks && locks[nb])) continue;
            lab[nb] = L; stack[sp++] = nb;
          }
        }
        if (big) continue;
        // the neighbours' biomes by shared border
        const votes = new Map();
        for (const n of cells) {
          const ci = n % w, cj = (n / w) | 0;
          for (let k = 0; k < 4; k++) {
            const ii = ci + NI[k], jj = cj + NJ[k];
            if (ii < 0 || jj < 0 || ii >= w || jj >= hh) continue;
            const v = valAt(ii, jj);
            if (v === b0 || v === 255) continue;
            votes.set(v, (votes.get(v) || 0) + 1);
          }
        }
        const order = [...votes.entries()].sort((a, b) => b[1] - a[1] || a[0] - b[0]);
        for (const [v] of order) {
          const name = table[v] && table[v].name;
          if (!name || !cells.every(n => eligible(name, o.feat(n), p))) continue;
          for (const n of cells) out[n] = v;
          merged++;
          break;
        }
      }
    if (!merged) break;
  }
}

/* ---- the locks: hand-painted cells the generator keeps --------------------------------------- */
/** A cell mask (nx x nz, row-major) as map.json climate.locks:
 *  {cells: [nx, nz], rle: "a.b.c..."} = base-36 run lengths, alternating
 *  unlocked / locked, starting unlocked. null when nothing is locked. */
export function encodeLocks(mask, nx, nz) {
  const runs = [];
  let v = 0, run = 0, any = false;
  for (let n = 0; n < nx * nz; n++) {
    const b = mask[n] ? 1 : 0;
    if (b) any = true;
    if (b === v) run++;
    else { runs.push(run); v = b; run = 1; }
  }
  runs.push(run);
  return any ? {cells: [nx, nz], rle: runs.map(r => r.toString(36)).join('.')} : null;
}
/** climate.locks -> a Uint8Array (nx x nz), or null if absent or for another size. */
export function decodeLocks(L, nx, nz) {
  if (!L || !Array.isArray(L.cells) || L.cells[0] !== nx || L.cells[1] !== nz || typeof L.rle !== 'string') return null;
  const m = new Uint8Array(nx * nz);
  let at = 0, v = 0;
  for (const s of L.rle.split('.')) {
    const r = parseInt(s, 36) || 0;
    if (v) m.fill(1, at, Math.min(m.length, at + r));
    at += r; v ^= 1;
  }
  return m;
}

/* ---- the job ----------------------------------------------------------------------------- */
/** terrainjobs.js 'climate': J = {grid: {w, hh, x0, z0, cell, h} (one sample
 *  per biome cell, at its centre), seaY, vpm, params, centre, biomes: [{name,
 *  T, M}], current (Uint8Array table index | 255), locks (Uint8Array | null),
 *  rect ([i0, j0, w, h] | null = all), rivers, lakes, classify (bool)}.
 *  -> {T, P, M, W, biome (Uint8Array, 255 = unchanged / outside the rect),
 *  counts: {name: cells}, proxy}. */
export function climateJob(J, progress = null) {
  const G = J.grid, prog = progress || (() => {});
  const p = params(J.params);
  const water = (J.rivers && J.rivers.length) || (J.lakes && J.lakes.length) ? rasterWater(G, J.rivers, J.lakes) : null;
  const F = climateFields(G, {seaY: J.seaY, vpm: J.vpm, params: p, centre: J.centre, water, progress: t => prog(t * 0.85)});
  const res = {T: F.T, P: F.P, M: F.M, W: F.W, proxy: F.proxy};
  if (J.classify !== false && J.biomes && J.biomes.length) {
    const C = classify(F, G, J.biomes, {params: p, rect: J.rect || null, current: J.current || null, locks: J.locks || null, vpm: J.vpm});
    res.biome = C.biome;
    const counts = {};
    for (let n = 0; n < C.biome.length; n++) { const b = C.biome[n]; if (b !== 255) counts[J.biomes[b].name] = (counts[J.biomes[b].name] || 0) + 1; }
    res.counts = counts;
  }
  prog(1);
  return res;
}
