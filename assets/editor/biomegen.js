/* biomegen.js — the biome DATA MODEL and the swatch that shows it.
 *
 * WHAT A BIOME IS HERE. A named region type that owns FEATURE STACKS: which
 * ground cover it wears, which tree species grow in it and how densely, which
 * water bodies appear and how rarely, which cave presets run under it. Every
 * row of every stack carries the same PLACEMENT CHAIN — rarity, then
 * conditions (ground band, slope, distance to water, a patch mask) — which is
 * the model every data-driven worldgen has converged on (Minecraft's
 * placed-feature modifier list is the best-documented; docs/PLAN_biomes.md §2).
 *
 * WHO OWNS WHAT. This is the part that matters for "one authoritative source":
 *
 *   * a TREE SPECIES file (assets/trees/*.json) owns what a tree LOOKS like and
 *     what ground it can physically tolerate (altitude band, treeline, slope).
 *   * a WATER PRESET (assets/water/*.json) owns the shape of a body of water
 *     and what grows in and around it.
 *   * a BIOME file (assets/biomes/*.json) owns WHICH species and presets appear
 *     in it, at what weight/rarity, and under what extra conditions.
 *
 * The tree atlas the engine reads bakes per-biome species weights into each
 * .svtree header (treegen.js BIOME_ORDER). Those words are now DERIVED from the
 * biome files: saving a biome in the tuner rewrites `placement.biomes.<biome>`
 * in every species file it names (and zeroes the ones it does not), and
 * `node scripts/seed_environment.mjs --sync` does the same headlessly. The
 * species file keeps the mirror because the bake reads one file per species;
 * the biome file is where you EDIT it.
 *
 * THE ENGINE READS THE BIOME FILES TODAY ONLY TO VALIDATE THEM (the `biomes`
 * gate: every species, preset and material a biome names must exist, indices
 * must match worldgen's B_* ids). Cover, water and cave stacks are scaffolding
 * until worldgen reads a biome table — PLAN_biomes.md §5 lists the seams and
 * which are outside the CPU-mirrored blocks. The tuner says so on the page.
 *
 * PURE MODULE: no DOM, no fetch. The swatch composer imports treegen.js and
 * watergen.js, both pure, so `scripts/test_environment.mjs` runs all of it
 * under Node.
 */

'use strict';

import * as TG from './treegen.js';
import * as WG from './watergen.js';

export const DEFAULT_VOX_PER_M = 10;
export const MAX_SWATCH = 320;      // cells per side
export const MAX_SWATCH_H = 320;     // a great oak is ~270 cells tall at 10 vpm

/** worldgen.wgsl's B_* ids, in id order. treegen.js BIOME_ORDER is the same
 *  list; scripts/check_invariants.py asserts all three agree with the files. */
export const ENGINE_BIOMES = ['forest', 'meadow', 'pine', 'desert'];

// =============================================================================
// hashing (lowbias32, as treegen/watergen)
// =============================================================================
function mix32(x) {
  x = (x ^ (x >>> 16)) >>> 0; x = Math.imul(x, 0x7feb352d) >>> 0;
  x = (x ^ (x >>> 15)) >>> 0; x = Math.imul(x, 0x846ca68b) >>> 0;
  return (x ^ (x >>> 16)) >>> 0;
}
export function hashN(...vals) {
  let h = 0x51ed270b;
  for (const v of vals) h = mix32((h ^ ((v | 0) >>> 0)) >>> 0) ^ 0x85ebca6b;
  return mix32(h);
}
const frand = (...v) => hashN(...v) / 4294967296;
function smooth(t) { return t * t * (3 - 2 * t); }
function vnoise2(x, z, seed) {
  const xi = Math.floor(x), zi = Math.floor(z);
  const fx = smooth(x - xi), fz = smooth(z - zi);
  const a = frand(seed, xi, zi), b = frand(seed, xi + 1, zi);
  const c = frand(seed, xi, zi + 1), d = frand(seed, xi + 1, zi + 1);
  return (a + (b - a) * fx) * (1 - fz) + (c + (d - c) * fx) * fz;
}
function fbm2(x, z, oct, seed) {
  let sum = 0, amp = 1, norm = 0;
  for (let o = 0; o < oct; o++) {
    sum += (vnoise2(x, z, seed + o * 131) * 2 - 1) * amp;
    norm += amp; amp *= 0.5; x = x * 2 + 17.3; z = z * 2 - 9.1;
  }
  return sum / norm;
}

// =============================================================================
// the data model
// =============================================================================

/** The placement chain every feature row carries. -1 / 0 = unbounded. */
export function defaultConditions() {
  return {
    minY: -1,            // lowest ground Y (voxels, world) the row tolerates
    maxY: -1,            // highest; a per-row treeline
    maxSlope: 1024,      // Q8 landform slope gate (256 = 45 deg = repose)
    nearWaterMax: -1,    // metres; only within this distance of a water body
    nearWaterMin: 0,     // metres; keep at least this far from water
    patchThreshold: 0    // 0..255; only where the row's patch noise is above this
  };
}

export function defaultBiome() {
  return {
    name: 'biome',
    displayName: 'Biome',
    index: -1,                       // worldgen B_* id; -1 = not an engine biome yet
    climate: {
      temperature: 0.5,              // 0 cold .. 1 hot   (future climate grid)
      moisture: 0.5,                 // 0 arid .. 1 wet
      notes: ''
    },
    terrain: {
      overrides: {}                  // worldgen.<key>: value — this biome's terrain knobs
    },
    cover: {
      skin: 'grass',                 // the topmost ground cell
      skinDepth: 1,                  // cells of skin (desert sand is 4)
      subsoil: 'dirt',
      patch: {threshold: 0, cellLog2: 5},   // shared patch mask for the plant rows
      plants: []                     // [{material, head, chance, height, conditions}]
    },
    trees: {
      tile: 14.4,                    // metres between candidate trunk sites
      density: 40,                   // percent of tiles that grow a tree
      species: []                    // [{species, weight, conditions}]
    },
    water: {
      features: []                   // [{preset, tile, rarity, conditions}]
    },
    caves: {
      features: []                   // [{preset, threshold, rarity, conditions}]  (scaffold)
    },
    swatch: {
      sizeM: 24,                     // preview square, metres
      reliefM: 1.6,                  // preview ground relief amplitude
      reliefFreq: 1.0
    }
  };
}

function isObj(v) { return v && typeof v === 'object' && !Array.isArray(v); }
function merge(dst, src) {
  if (!isObj(src)) return dst;
  for (const k of Object.keys(src)) {
    const v = src[k];
    if (isObj(v) && isObj(dst[k])) merge(dst[k], v);
    else if (v !== undefined) dst[k] = Array.isArray(v) ? JSON.parse(JSON.stringify(v)) : v;
  }
  return dst;
}

export function normalizeConditions(c) { return merge(defaultConditions(), c || {}); }

export function normalizeBiome(src) {
  const b = merge(defaultBiome(), src || {});
  b.cover.plants = (b.cover.plants || []).map(p => ({
    material: String(p.material || ''), head: String(p.head || ''),
    chance: Math.max(0, p.chance | 0), height: +p.height || 0.3,
    conditions: normalizeConditions(p.conditions)
  }));
  b.trees.species = (b.trees.species || []).map(s => ({
    species: String(s.species || ''), weight: Math.max(0, s.weight | 0),
    conditions: normalizeConditions(s.conditions)
  }));
  b.water.features = (b.water.features || []).map(f => ({
    preset: String(f.preset || ''), tile: +f.tile || 44.8, rarity: Math.max(0, f.rarity | 0),
    conditions: normalizeConditions(f.conditions)
  }));
  b.caves.features = (b.caves.features || []).map(f => ({
    preset: String(f.preset || 'near_surface'), threshold: f.threshold | 0,
    rarity: Math.max(0, f.rarity | 0), conditions: normalizeConditions(f.conditions)
  }));
  return b;
}

// =============================================================================
// rarity arithmetic — one form authored, the others shown
// =============================================================================

/** For a "1 in N tiles of T metres" row: percent and count per hectare. */
export function rarityStats(tileM, rarity) {
  if (!tileM || !rarity) return {pct: 0, perHa: 0, perKm2: 0};
  const pct = 100 / rarity;
  const tilesPerHa = 10000 / (tileM * tileM);
  return {pct, perHa: tilesPerHa / rarity, perKm2: tilesPerHa * 100 / rarity};
}
/** For a "P percent of tiles of T metres" row. */
export function densityStats(tileM, pct) {
  if (!tileM) return {perHa: 0, oneIn: 0};
  const tilesPerHa = 10000 / (tileM * tileM);
  return {perHa: tilesPerHa * pct / 100, oneIn: pct > 0 ? 100 / pct : 0};
}

// =============================================================================
// species-weight sync: biome files -> species placement.biomes
// =============================================================================

/**
 * Given every biome (normalised) and a species' params, return the
 * `placement.biomes` object the species file should carry. Only ENGINE biomes
 * (those with an index in ENGINE_BIOMES) are written, because those are the
 * words the .svtree header has room for; a biome the engine does not know yet
 * is authored but not baked, and the tab says so.
 */
export function speciesWeightsFrom(biomes, speciesName) {
  const out = {};
  for (const nm of ENGINE_BIOMES) out[nm] = 0;
  for (const b of biomes) {
    if (!ENGINE_BIOMES.includes(b.name)) continue;
    for (const s of b.trees.species) if (s.species === speciesName) out[b.name] = s.weight | 0;
  }
  return out;
}

/** True when a species file's placement.biomes already matches the biomes. */
export function speciesWeightsMatch(biomes, speciesName, placementBiomes) {
  const want = speciesWeightsFrom(biomes, speciesName);
  const have = placementBiomes || {};
  return ENGINE_BIOMES.every(nm => (have[nm] | 0) === want[nm]);
}

// =============================================================================
// validation — the same checks the engine's `biomes` gate makes
// =============================================================================

/**
 * @param biome  normalised biome
 * @param libs   {trees:Set<string>, water:Set<string>, materials:Set<string>}
 * @returns string[] problems (empty = valid)
 */
export function validateBiome(biome, libs) {
  const bad = [];
  const mat = (nm, where) => {
    if (nm && libs.materials && !libs.materials.has(nm)) bad.push(where + ': unknown material "' + nm + '"');
  };
  if (!/^[a-z0-9_]+$/.test(biome.name)) bad.push('name must be [a-z0-9_]: "' + biome.name + '"');
  if (biome.index >= 0 && ENGINE_BIOMES[biome.index] !== biome.name)
    bad.push('index ' + biome.index + ' is worldgen\'s ' + (ENGINE_BIOMES[biome.index] || '(none)') +
             ', not ' + biome.name);
  mat(biome.cover.skin, 'cover.skin'); mat(biome.cover.subsoil, 'cover.subsoil');
  biome.cover.plants.forEach((p, i) => { mat(p.material, 'cover.plants[' + i + ']'); mat(p.head, 'cover.plants[' + i + '].head'); });
  biome.trees.species.forEach((s, i) => {
    if (libs.trees && !libs.trees.has(s.species)) bad.push('trees.species[' + i + ']: no species "' + s.species + '" in assets/trees/');
  });
  biome.water.features.forEach((f, i) => {
    if (libs.water && !libs.water.has(f.preset)) bad.push('water.features[' + i + ']: no preset "' + f.preset + '" in assets/water/');
    if (f.tile <= 0) bad.push('water.features[' + i + ']: tile must be > 0');
  });
  const seen = new Set();
  for (const s of biome.trees.species) {
    if (seen.has(s.species)) bad.push('trees.species: "' + s.species + '" listed twice');
    seen.add(s.species);
  }
  return bad;
}

// =============================================================================
// the swatch — one biome composed into one voxel region
// =============================================================================

/** Compact nonzero-cell list of a generated tree, so stamping costs the tree's
 *  voxels rather than its bounding box (a great oak's box is 8M cells of which
 *  ~300k are tree). */
function compactTree(res) {
  const {x: nx, y: ny, z: nz} = res.dim;
  const xs = [], ys = [], zs = [], ws = [];
  for (let z = 0; z < nz; z++)
    for (let y = 0; y < ny; y++) {
      const row = (z * ny + y) * nx;
      for (let x = 0; x < nx; x++) {
        const w = res.cells[row + x];
        if (w) { xs.push(x - res.anchor.x); ys.push(y); zs.push(z - res.anchor.z); ws.push(w); }
      }
    }
  return {xs: Int16Array.from(xs), ys: Int16Array.from(ys), zs: Int16Array.from(zs),
          ws: Uint16Array.from(ws), names: res.names, dim: res.dim, anchor: res.anchor};
}

/**
 * Compose a biome swatch.
 *
 * @param biome  normalised biome
 * @param libs   {water: {name: presetParams}, trees: {name: speciesParams}}
 * @param seed   integer
 * @param opts   {vpm, treeCache: Map, sizeM, noTrees, noWater, noCover}
 * @returns {dim, cells, names, anchor, meta, plan}
 */
export function generateSwatch(biome, libs, seed, opts) {
  opts = opts || {};
  const B = normalizeBiome(biome);
  const vpm = opts.vpm || DEFAULT_VOX_PER_M;
  const cellM = 1 / vpm;
  const m2v = (m) => Math.round(m * vpm);
  const sizeM = opts.sizeM || B.swatch.sizeM;
  let N = Math.min(MAX_SWATCH, Math.max(16, m2v(sizeM)));
  const names = [];
  const idx = new Map();
  const nameId = (nm) => {
    if (!nm || nm === 'none') return 0;
    let i = idx.get(nm);
    if (i === undefined) { names.push(nm); i = names.length; idx.set(nm, i); }
    return i;
  };
  const meta = {trees: {}, water: {}, cover: {}, skipped: {trees: 0, water: 0},
                sizeM, dim: null, clipped: false, treesPlaced: 0, waterBodies: 0};

  // ---- ground --------------------------------------------------------------
  const rockV = 2;
  const soilV = 8;
  const reliefV = m2v(B.swatch.reliefM);
  const base = rockV + soilV + reliefV + 26;      // headroom under the surface for basins
  const h = new Int32Array(N * N);
  const slope = new Uint16Array(N * N);
  for (let z = 0; z < N; z++)
    for (let x = 0; x < N; x++) {
      const f = fbm2(x * cellM * B.swatch.reliefFreq / 6 + 4.2, z * cellM * B.swatch.reliefFreq / 6 - 1.7,
                     4, seed ^ 0x2a);
      h[z * N + x] = base + Math.round(f * reliefV);
    }
  for (let z = 0; z < N; z++)
    for (let x = 0; x < N; x++) {
      const x0 = Math.max(0, x - 1), x1 = Math.min(N - 1, x + 1);
      const z0 = Math.max(0, z - 1), z1 = Math.min(N - 1, z + 1);
      const gx = (h[z * N + x1] - h[z * N + x0]) / (x1 - x0);
      const gz = (h[z1 * N + x] - h[z0 * N + x]) / (z1 - z0);
      slope[z * N + x] = Math.min(4095, Math.round((Math.abs(gx) + Math.abs(gz)) * 256));
    }
  let hmax = 0;
  for (let i = 0; i < N * N; i++) if (h[i] > hmax) hmax = h[i];

  // ---- water bodies: decide instances first -----------------------------------
  const bodies = [];
  const waterPresets = libs.water || {};
  if (!opts.noWater && opts.showcase) {
    // SHOWCASE: one instance of every water row, regardless of rarity, the
    // first centred and the rest spaced around it. A 24 m swatch at a real
    // 1-in-4 x 44.8 m tile shows a pond one time in twenty, which is the
    // truth about rarity and useless for judging a shoreline. The tab's
    // "true rarity" toggle turns this off.
    const rows = B.water.features.filter(f => waterPresets[f.preset] && f.rarity > 0);
    rows.forEach((f, fi) => {
      const P = WG.normalizeParams(waterPresets[f.preset], vpm);
      const rh = hashN(seed, 0xb0a7 + fi, 0, 0);
      const inst = WG.instanceOf(P, (rh >>> 3) ^ seed);
      const reachV = Math.ceil(inst.reach * vpm);
      let cx = N >> 1, cz = N >> 1;
      if (fi > 0) {
        const ang = (fi - 1) * 2.4 + 0.7;
        const d = Math.min(N * 0.42, (bodies[0].reachV + reachV) * 1.1 + 2);
        cx = Math.round(N / 2 + Math.cos(ang) * d); cz = Math.round(N / 2 + Math.sin(ang) * d);
      }
      const kx = Math.min(N - 1, Math.max(0, cx)), kz = Math.min(N - 1, Math.max(0, cz));
      if (bodies.some(b => Math.hypot(b.cx - cx, b.cz - cz) < (b.reachV + reachV) * 1.02)) {
        meta.skipped.water++; return;
      }
      bodies.push({P, I: inst, M: WG.paletteOf(P, names, idx), cx, cz, reachV, surf: h[kz * N + kx],
                   seed: (rh >>> 3) ^ seed, preset: f.preset});
    });
  } else if (!opts.noWater) {
    B.water.features.forEach((f, fi) => {
      const src = waterPresets[f.preset];
      if (!src || !f.rarity || f.tile <= 0) return;
      const P = WG.normalizeParams(src, vpm);
      const T = Math.max(8, m2v(f.tile));
      const t0 = -1, t1 = Math.ceil(N / T);
      for (let tz = t0; tz <= t1; tz++)
        for (let tx = t0; tx <= t1; tx++) {
          const rh = hashN(seed, 0xb0a7 + fi, tx, tz);
          if (rh % f.rarity !== 0) continue;
          const inst = WG.instanceOf(P, (rh >>> 3) ^ seed);
          const reachV = Math.ceil(inst.reach * vpm);
          // Jitter inside the tile, keeping the body inside it (Minecraft's
          // in_square, with the inset the engine's pondInfo uses).
          const room = Math.max(0, T - 2 * reachV - 2);
          const cx = tx * T + reachV + 1 + (room ? (hashN(rh, 1) % room) : 0);
          const cz = tz * T + reachV + 1 + (room ? (hashN(rh, 2) % room) : 0);
          if (cx + reachV < 0 || cz + reachV < 0 || cx - reachV >= N || cz - reachV >= N) continue;
          const kx = Math.min(N - 1, Math.max(0, cx)), kz = Math.min(N - 1, Math.max(0, cz));
          const c = normalizeConditions(f.conditions);
          if (slope[kz * N + kx] > c.maxSlope) { meta.skipped.water++; continue; }
          const surf = h[kz * N + kx];
          if (bodies.some(b => Math.hypot(b.cx - cx, b.cz - cz) < (b.reachV + reachV) * 1.05)) {
            meta.skipped.water++; continue;
          }
          bodies.push({P, I: inst, M: WG.paletteOf(P, names, idx), cx, cz, reachV, surf,
                       seed: (rh >>> 3) ^ seed, preset: f.preset});
        }
    });
  }

  // ---- allocate --------------------------------------------------------------
  const treeLib = libs.trees || {};
  let treeTop = 0;
  const treeCache = opts.treeCache || new Map();
  const speciesRows = opts.noTrees ? [] : B.trees.species.filter(s => s.weight > 0 && treeLib[s.species]);
  const totalW = speciesRows.reduce((a, s) => a + s.weight, 0);
  const treeOf = (species, variant) => {
    const key = species + '#' + variant + '@' + vpm;
    let t = treeCache.get(key);
    if (!t) {
      const res = TG.generateTree(treeLib[species], variant, {vpm});
      t = compactTree(res);
      treeCache.set(key, t);
    }
    return t;
  };
  if (totalW > 0)
    for (const s of speciesRows) treeTop = Math.max(treeTop, treeOf(s.species, 0).dim.y);
  let ny = hmax + Math.max(treeTop, 24) + 6;
  if (ny > MAX_SWATCH_H) { ny = MAX_SWATCH_H; meta.clipped = true; }
  const cells = new Uint16Array(N * ny * N);
  const at = (x, y, z) => (z * ny + y) * N + x;
  const put = (x, y, z, w) => { if (y >= 0 && y < ny && x >= 0 && x < N && z >= 0 && z < N) cells[at(x, y, z)] = w; };
  const solid = (id, x, z) => id ? (id | ((hashN(seed, x, z, 0x33) % 3) << 12)) : 0;

  const skin = nameId(B.cover.skin), subsoil = nameId(B.cover.subsoil), rock = nameId('stone');
  const skinDepth = Math.max(1, B.cover.skinDepth | 0);
  const zone = new Uint8Array(N * N);           // WG.ZONE values; 0 = plain land
  const nearWater = new Float32Array(N * N).fill(1e9);   // metres to the nearest shoreline
  const top = new Int32Array(N * N);            // topmost ground cell per column

  // ---- ground + water columns -------------------------------------------------
  for (let z = 0; z < N; z++)
    for (let x = 0; x < N; x++) {
      const pi = z * N + x;
      const natural = h[pi];
      let col = null, body = null;
      for (const b of bodies) {
        if (Math.abs(x - b.cx) > b.reachV + 60 || Math.abs(z - b.cz) > b.reachV + 60) continue;
        const c = WG.columnAt(b.P, b.I, b.M, (x - b.cx) * cellM, (z - b.cz) * cellM, natural, b.surf, x, z, b.seed);
        if (c) { col = c; body = b; break; }
      }
      const rockTo = (yTop) => { for (let y = 0; y < Math.min(rockV, yTop + 1); y++) put(x, y, z, solid(rock, x, z)); };
      if (!col) {
        rockTo(natural);
        for (let y = rockV; y < natural - skinDepth + 1; y++) put(x, y, z, solid(y >= natural - soilV ? subsoil : rock, x, z));
        for (let y = Math.max(rockV, natural - skinDepth + 1); y <= natural; y++) put(x, y, z, solid(skin, x, z));
        top[pi] = natural;
        continue;
      }
      zone[pi] = col.zone || 1;
      if (col.floor >= 0) {
        rockTo(col.bedBottom);
        for (let y = rockV; y < col.bedBottom; y++) put(x, y, z, solid(body.M.substrate, x, z));
        for (let y = Math.max(rockV, col.bedBottom); y <= col.floor; y++) put(x, y, z, solid(col.bed, x, z));
        if (col.waterTop >= 0) {
          for (let y = col.floor + 1; y <= col.waterTop; y++) put(x, y, z, body.M.fill | (WG.LIQ_FULL << 12));
          if (col.surfSkin) put(x, col.waterTop, z, solid(col.surfSkin, x, z));
          nearWater[pi] = 0;
        }
        top[pi] = col.waterTop >= 0 ? col.waterTop : col.floor;
        meta.water[body.preset] = (meta.water[body.preset] || 0) + (col.waterTop >= 0 ? 1 : 0);
      } else {
        rockTo(col.top);
        for (let y = rockV; y < col.top; y++) put(x, y, z, solid(y >= col.top - soilV ? subsoil : rock, x, z));
        // The body's own skin (mud, moss) wins on its fringe; plain land keeps the biome skin.
        const sk = (col.skin && col.skin !== body.M.skin) ? col.skin : skin;
        put(x, col.top, z, solid(sk, x, z));
        top[pi] = col.top;
        nearWater[pi] = Math.max(0, (col.u - 1) * body.I.R);
      }
      for (const pl of col.plants) {
        for (let y = pl.y0; y <= pl.y1; y++) put(x, y, z, solid(pl.id, x, z));
        if (pl.y1 >= pl.y0) meta.cover[pl.name] = (meta.cover[pl.name] || 0) + 1;
      }
    }
  meta.waterBodies = bodies.length;
  for (const b of bodies) meta.water[b.preset + ' (bodies)'] = (meta.water[b.preset + ' (bodies)'] || 0) + 1;

  // Distance to water for the conditions: a cheap two-pass chamfer over the
  // shoreline seeds (exact enough for a 1-in-N gate at metre resolution).
  {
    const INF = 1e9;
    for (let z = 0; z < N; z++)
      for (let x = 0; x < N; x++) {
        const pi = z * N + x;
        let d = nearWater[pi];
        if (x > 0) d = Math.min(d, nearWater[pi - 1] + cellM);
        if (z > 0) d = Math.min(d, nearWater[pi - N] + cellM);
        if (x > 0 && z > 0) d = Math.min(d, nearWater[pi - N - 1] + cellM * 1.414);
        nearWater[pi] = d;
      }
    for (let z = N - 1; z >= 0; z--)
      for (let x = N - 1; x >= 0; x--) {
        const pi = z * N + x;
        let d = nearWater[pi];
        if (x < N - 1) d = Math.min(d, nearWater[pi + 1] + cellM);
        if (z < N - 1) d = Math.min(d, nearWater[pi + N] + cellM);
        if (x < N - 1 && z < N - 1) d = Math.min(d, nearWater[pi + N + 1] + cellM * 1.414);
        nearWater[pi] = d >= INF ? INF : d;
      }
  }

  const passes = (c, pi, yTop) => {
    if (c.minY >= 0 && yTop < c.minY) return false;
    if (c.maxY >= 0 && yTop > c.maxY) return false;
    if (slope[pi] > c.maxSlope) return false;
    if (c.nearWaterMax >= 0 && nearWater[pi] > c.nearWaterMax) return false;
    if (c.nearWaterMin > 0 && nearWater[pi] < c.nearWaterMin) return false;
    return true;
  };

  // ---- ground cover plants -------------------------------------------------------
  if (!opts.noCover) {
    const patch = B.cover.patch;
    const pcell = 1 << Math.max(1, Math.min(10, patch.cellLog2 | 0));
    B.cover.plants.forEach((pl, k) => {
      const id = nameId(pl.material), head = nameId(pl.head);
      if (!id || pl.chance <= 0) return;
      const hV = Math.max(1, m2v(pl.height));
      for (let z = 0; z < N; z++)
        for (let x = 0; x < N; x++) {
          const pi = z * N + x;
          if (zone[pi]) continue;                       // a water body's fringe owns its plants
          if (hashN(seed, 0xc0 + k, x, z) % pl.chance !== 0) continue;
          const thr = Math.max(patch.threshold | 0, pl.conditions.patchThreshold | 0);
          if (thr > 0) {
            const nz = vnoise2(x / pcell + k * 7.3, z / pcell - k * 2.1, seed ^ (0x300 + k)) * 255;
            if (nz <= thr) continue;
          }
          if (!passes(pl.conditions, pi, top[pi])) continue;
          const y0 = top[pi] + 1;
          for (let y = y0; y < y0 + hV; y++) put(x, y, z, solid(id, x, z));
          if (head) put(x, y0 + hV, z, solid(head, x, z));
          meta.cover[pl.material] = (meta.cover[pl.material] || 0) + 1;
        }
    });
  }

  // ---- trees ---------------------------------------------------------------------
  if (totalW > 0 && B.trees.density > 0) {
    const T = Math.max(4, m2v(B.trees.tile));
    const remap = new Map();   // species -> Uint16Array(local palette -> swatch palette)
    for (let tz = 0; tz * T < N; tz++)
      for (let tx = 0; tx * T < N; tx++) {
        const rh = hashN(seed, 0x7bee, tx, tz);
        if (rh % 100 >= B.trees.density) continue;
        // Weighted draw, then the species' own conditions gate — a gated-out
        // pick grows NOTHING rather than re-rolling, exactly as the engine does.
        let roll = hashN(rh, 5) % totalW, row = speciesRows[0];
        for (const s of speciesRows) { if (roll < s.weight) { row = s; break; } roll -= s.weight; }
        const x = tx * T + (hashN(rh, 6) % T), z = tz * T + (hashN(rh, 7) % T);
        if (x >= N || z >= N) continue;
        const pi = z * N + x;
        if (zone[pi]) { meta.skipped.trees++; continue; }   // not in water or on its fringe
        if (!passes(row.conditions, pi, top[pi])) { meta.skipped.trees++; continue; }
        const sp = treeLib[row.species];
        const c = sp.placement || {};
        if ((c.maxSlope | 0) > 0 && c.maxSlope < 1024 && slope[pi] > c.maxSlope) { meta.skipped.trees++; continue; }
        const variant = hashN(rh, 8) % Math.max(1, Math.min(3, sp.variants | 0 || 1));
        const t = treeOf(row.species, variant);
        let rm = remap.get(row.species + variant);
        if (!rm) {
          rm = new Uint16Array(t.names.length + 1);
          t.names.forEach((nm, i) => { rm[i + 1] = nameId(nm); });
          remap.set(row.species + variant, rm);
        }
        const y0 = top[pi] + 1;
        // Rotation: 4 yaw steps from the hash, as the engine's tree sampler does.
        const rot = hashN(rh, 9) & 3;
        for (let i = 0; i < t.ws.length; i++) {
          let dx = t.xs[i], dz = t.zs[i];
          if (rot === 1) { const q = dx; dx = -dz; dz = q; }
          else if (rot === 2) { dx = -dx; dz = -dz; }
          else if (rot === 3) { const q = dx; dx = dz; dz = -q; }
          const w = t.ws[i];
          put(x + dx, y0 + t.ys[i], z + dz, rm[w & 0xFFF] | (w & 0xF000));
        }
        meta.trees[row.species] = (meta.trees[row.species] || 0) + 1;
        meta.treesPlaced++;
      }
  }

  let voxels = 0;
  for (let i = 0; i < cells.length; i++) if (cells[i]) voxels++;
  meta.voxels = voxels;
  meta.dim = {x: N, y: ny, z: N};
  return {dim: {x: N, y: ny, z: N}, cells, names, anchor: {x: N >> 1, y: base, z: N >> 1}, meta,
          plan: {zone, top, nearWater, slope, N}};
}

// =============================================================================
// presets — today's four engine biomes, transcribed from worldgen.wgsl and the
// species files' placement.biomes. `node scripts/seed_environment.mjs --seed`
// writes them to assets/biomes/.
// =============================================================================

export const BIOME_PRESETS = {
  forest: {
    name: 'forest', displayName: 'Forest', index: 0,
    climate: {temperature: 0.55, moisture: 0.65,
              notes: 'The default band of the biome noise: between meadowThreshold and pineThreshold.'},
    cover: {
      skin: 'grass', skinDepth: 1, subsoil: 'dirt', patch: {threshold: 0, cellLog2: 5},
      plants: [
        {material: 'fern', chance: 16, height: 0.4},
        {material: 'flower_bluebell', chance: 40, height: 0.2},
        {material: 'mushroom_cluster', chance: 90, height: 0.2},
        {material: 'bramble', chance: 60, height: 0.4},
        {material: 'grass_tuft', chance: 9, height: 0.2}
      ]
    },
    trees: {tile: 14.4, density: 78, species: [
      {species: 'oak', weight: 42}, {species: 'great_oak', weight: 12}, {species: 'birch', weight: 18},
      {species: 'pine', weight: 14}, {species: 'spruce', weight: 4}, {species: 'redwood', weight: 3},
      {species: 'willow', weight: 6, conditions: {nearWaterMax: 6}}, {species: 'eucalyptus', weight: 5},
      {species: 'bush', weight: 16}, {species: 'dead', weight: 4}
    ]},
    water: {features: [
      {preset: 'tarn', tile: 44.8, rarity: 4, conditions: {maxSlope: 96}},
      {preset: 'spring_pool', tile: 25.6, rarity: 12, conditions: {maxSlope: 160}}
    ]},
    caves: {features: [{preset: 'near_surface', threshold: 150, rarity: 1}, {preset: 'deep', threshold: 140, rarity: 1}]}
  },
  meadow: {
    name: 'meadow', displayName: 'Meadow', index: 1,
    climate: {temperature: 0.6, moisture: 0.5, notes: 'Biome noise below meadowThreshold.'},
    cover: {
      skin: 'grass', skinDepth: 1, subsoil: 'dirt', patch: {threshold: 0, cellLog2: 5},
      plants: [
        {material: 'tall_grass', head: 'tall_grass_head', chance: 6, height: 0.5},
        {material: 'flower_poppy', chance: 30, height: 0.2},
        {material: 'flower_daisy', chance: 26, height: 0.2},
        {material: 'flower_buttercup', chance: 34, height: 0.2},
        {material: 'flower_clover', chance: 20, height: 0.1},
        {material: 'grass_tuft', chance: 7, height: 0.2}
      ]
    },
    trees: {tile: 14.4, density: 22, species: [
      {species: 'oak', weight: 16}, {species: 'great_oak', weight: 5}, {species: 'birch', weight: 30},
      {species: 'pine', weight: 2}, {species: 'eucalyptus', weight: 12},
      {species: 'willow', weight: 14, conditions: {nearWaterMax: 8}}, {species: 'bush', weight: 40},
      {species: 'dead', weight: 3}
    ]},
    water: {features: [
      {preset: 'tarn', tile: 44.8, rarity: 4, conditions: {maxSlope: 96}},
      {preset: 'marsh', tile: 76.8, rarity: 5, conditions: {maxSlope: 32}},
      {preset: 'kettle', tile: 38.4, rarity: 7, conditions: {maxSlope: 64}}
    ]},
    caves: {features: [{preset: 'near_surface', threshold: 150, rarity: 1}, {preset: 'deep', threshold: 140, rarity: 1}]}
  },
  pine: {
    name: 'pine', displayName: 'Pine highland', index: 2,
    climate: {temperature: 0.3, moisture: 0.55, notes: 'Biome noise between pineThreshold and desertThreshold.'},
    cover: {
      skin: 'grass', skinDepth: 1, subsoil: 'dirt', patch: {threshold: 120, cellLog2: 5},
      plants: [
        {material: 'heath_shrub', chance: 14, height: 0.4},
        {material: 'fern', chance: 30, height: 0.4},
        {material: 'moss_patch', chance: 40, height: 0.1},
        {material: 'toadstool_pale', chance: 120, height: 0.2}
      ]
    },
    trees: {tile: 14.4, density: 70, species: [
      {species: 'pine', weight: 60}, {species: 'spruce', weight: 34}, {species: 'redwood', weight: 10},
      {species: 'birch', weight: 8}, {species: 'oak', weight: 4}, {species: 'bush', weight: 12},
      {species: 'dead', weight: 4}
    ]},
    water: {features: [
      {preset: 'tarn', tile: 44.8, rarity: 4, conditions: {maxSlope: 96}},
      {preset: 'kettle', tile: 38.4, rarity: 6, conditions: {maxSlope: 64}},
      {preset: 'crater_lake', tile: 102.4, rarity: 14, conditions: {maxSlope: 128}}
    ]},
    caves: {features: [{preset: 'near_surface', threshold: 150, rarity: 1}, {preset: 'deep', threshold: 140, rarity: 1}]}
  },
  desert: {
    name: 'desert', displayName: 'Desert', index: 3,
    climate: {temperature: 0.9, moisture: 0.1, notes: 'Biome noise above desertThreshold.'},
    cover: {
      skin: 'sand', skinDepth: 4, subsoil: 'sand', patch: {threshold: 128, cellLog2: 5},
      plants: [
        {material: 'dry_tussock', chance: 20, height: 0.3},
        {material: 'desert_scrub', chance: 36, height: 0.5},
        {material: 'cactus_flesh', head: 'cactus_bloom', chance: 90, height: 1.2}
      ]
    },
    trees: {tile: 14.4, density: 6, species: [
      {species: 'bush', weight: 26}, {species: 'dead', weight: 10}, {species: 'eucalyptus', weight: 8}
    ]},
    water: {features: [
      {preset: 'oasis', tile: 102.4, rarity: 6, conditions: {maxSlope: 48}},
      {preset: 'playa', tile: 128.0, rarity: 5, conditions: {maxSlope: 24}}
    ]},
    caves: {features: [{preset: 'near_surface', threshold: 150, rarity: 1}, {preset: 'deep', threshold: 140, rarity: 1}]}
  }
};

export const BIOME_ORDER = ['forest', 'meadow', 'pine', 'desert'];
