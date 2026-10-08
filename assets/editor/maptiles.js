/* maptiles.js — the v2 world map's PLANES in memory (docs/PLAN_map_v2.md,
 * "The v2 map format"): pure, DOM-free, Node-runnable, so the codec and the
 * brushes are testable without a browser (scripts/test_maptiles.mjs).
 *
 * A plane is a lattice of samples ("posts") at extent.min + i * (1 << log2),
 * i = 0 .. size >> log2 INCLUSIVE, stored on disk as 256x256-sample tiles
 * (assets/worldmap/<name>/planes/<plane>/<tx>_<tz>.svt, tile index = post
 * index >> 8). An absent tile is every sample at the plane's `default`.
 *
 *   height  i16 voxels, absolute Y, a POST lattice (sample i sits ON
 *           extent.min + i * spacing); bilinear here, the engine's C1 scheme
 *           is the engine's (F1) -- this page draws and edits the posts.
 *   biome   u8 palette index, CELL-centred: sample i covers
 *           [min + i * spacing, min + (i + 1) * spacing). The format gives the
 *           biome plane the same (size >> log2) + 1 count as the height plane;
 *           the last row / column lies past the extent and nothing reads it.
 *
 * In memory a HEIGHT tile is a Float32Array (a soft brush moves a post by a
 * fraction of a voxel per dab, as the sculpt layer does) rounded to i16 on
 * save; a BIOME tile is a Uint8Array. A tile is in one of three states:
 *   absent   not on disk, not in memory: the default;
 *   listed   on disk (P.onDisk) but not loaded yet: NOT editable (a brush
 *            skips it and counts it in `skipped`) -- the page loads visible
 *            tiles first and the rest in the background;
 *   loaded   in P.tiles (whether or not a file exists).
 * Edits mark tiles dirty; a save POSTs only dirty tiles, and a dirty tile
 * that is all-default (or no longer in memory) is POSTed EMPTY, which
 * deletes the file -- an ocean tile is simply absent. */

export const TILE = 256;
export const TILE_MAGIC = 0x31545653;           // 'SVT1' read as a little-endian u32
export const PLANES = {height: {kind: 1, bytes: 2}, biome: {kind: 2, bytes: 1}};
export const HEIGHT_LIMIT = 32000;              // the loader refuses |height| > 32000

export const tileKey = (tx, tz) => tz * 65536 + tx;
export const tileXZ = k => [k % 65536, Math.floor(k / 65536)];
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);

/** A plane of map.json `json` ('height' | 'biome'), empty: every tile absent. */
export function makePlane(name, json) {
  const spec = PLANES[name];
  if (!spec) throw new Error('unknown plane ' + name);
  const pj = (json.planes || {})[name];
  if (!pj) throw new Error('map.json has no planes.' + name);
  const log2 = pj.log2 | 0, sp = 1 << log2;
  const [sx, sz] = json.extent.size;
  const nx = (sx >> log2) + 1, nz = (sz >> log2) + 1;
  let def = pj.default;
  if (name === 'biome') {
    def = typeof def === 'string' ? json.biomes.indexOf(def) : (def | 0);
    if (def < 0) def = 0;
  } else def = clamp(Math.round(+def || 0), -HEIGHT_LIMIT, HEIGHT_LIMIT);
  return {
    name, kind: spec.kind, bytes: spec.bytes, log2, sp, def,
    min: [json.extent.min[0], json.extent.min[1]],
    posts: [nx, nz], tilesX: Math.ceil(nx / TILE), tilesZ: Math.ceil(nz / TILE),
    tiles: new Map(), onDisk: new Set(), dirty: new Set(),
    ver: new Map(),      // key -> edit counter (display caches compare it)
    gen: 0,              // bumps on every change anywhere in the plane
    mips: new Map(),     // key -> {ver, levels[]}
    mm: new Map(),       // key -> {ver, min, max}
    defTile: null,       // shared all-default Float32Array for absent tiles' mips
  };
}
export function newTile(P) {
  const t = P.bytes === 2 ? new Float32Array(TILE * TILE) : new Uint8Array(TILE * TILE);
  if (P.def) t.fill(P.def);
  return t;
}
const copyTile = t => (t instanceof Float32Array ? new Float32Array(t) : new Uint8Array(t));
/** A listed tile that is not in memory yet: brushes must not touch it. */
export const blocked = (P, k) => P.onDisk.has(k) && !P.tiles.has(k);
/** True while any tile of the plane is still listed-but-unloaded. */
export function pending(P) { for (const k of P.onDisk) if (!P.tiles.has(k)) return true; return false; }
/** Valid sample count of tile (tx, tz) along x and z (edge tiles hang past the plane). */
export function tileSpan(P, tx, tz) {
  return [Math.min(TILE, P.posts[0] - tx * TILE), Math.min(TILE, P.posts[1] - tz * TILE)];
}

/* ---- codec ----------------------------------------------------------------------- */
/** A tile's file bytes: 32-byte header + 256x256 samples, z-major little-endian. */
export function encodeTile(P, tx, tz, t) {
  const n = TILE * TILE;
  const out = new Uint8Array(32 + n * P.bytes);
  const d = new DataView(out.buffer);
  d.setUint32(0, TILE_MAGIC, true); d.setUint32(4, P.kind, true);
  d.setInt32(8, tx, true); d.setInt32(12, tz, true);
  d.setUint32(16, TILE, true); d.setUint32(20, TILE, true);
  d.setUint32(24, P.bytes, true); d.setUint32(28, 0, true);
  if (P.bytes === 2) for (let i = 0; i < n; i++) d.setInt16(32 + i * 2, clamp(Math.round(t[i]), -HEIGHT_LIMIT, HEIGHT_LIMIT), true);
  else out.set(t, 32);
  return out;
}
/** Parse a tile file; throws unless it is THIS plane's tile at (tx, tz). */
export function decodeTile(P, buf, tx, tz) {
  const d = new DataView(buf instanceof ArrayBuffer ? buf : buf.buffer, buf.byteOffset || 0, buf.byteLength);
  const n = TILE * TILE;
  if (d.byteLength < 32 || d.getUint32(0, true) !== TILE_MAGIC) throw new Error('not an SVT1 tile');
  if (d.getUint32(4, true) !== P.kind) throw new Error(`tile is plane kind ${d.getUint32(4, true)}, not ${P.name}`);
  if (d.getInt32(8, true) !== tx || d.getInt32(12, true) !== tz) throw new Error(`tile header (${d.getInt32(8, true)},${d.getInt32(12, true)}) is not (${tx},${tz})`);
  if (d.getUint32(16, true) !== TILE || d.getUint32(20, true) !== TILE) throw new Error('tile is not 256x256');
  if (d.getUint32(24, true) !== P.bytes) throw new Error('tile sampleBytes does not match the plane');
  if (d.byteLength !== 32 + n * P.bytes) throw new Error('tile size does not match its header');
  const t = newTile(P);
  if (P.bytes === 2) for (let i = 0; i < n; i++) t[i] = d.getInt16(32 + i * 2, true);
  else for (let i = 0; i < n; i++) t[i] = d.getUint8(32 + i);
  return t;
}
/** Every sample at the default once rounded the way a save rounds it. */
export function isDefaultTile(P, t) {
  if (P.bytes === 2) { for (let i = 0; i < t.length; i++) if (clamp(Math.round(t[i]), -HEIGHT_LIMIT, HEIGHT_LIMIT) !== P.def) return false; }
  else for (let i = 0; i < t.length; i++) if (t[i] !== P.def) return false;
  return true;
}
/** The save: [{tx, tz, key, bytes|null}] for every dirty tile (null = delete the file). */
export function dirtyPayload(P) {
  const out = [];
  for (const k of [...P.dirty].sort((a, b) => a - b)) {
    const [tx, tz] = tileXZ(k);
    const t = P.tiles.get(k);
    out.push({tx, tz, key: k, bytes: !t || isDefaultTile(P, t) ? null : encodeTile(P, tx, tz, t)});
  }
  return out;
}
/** After a successful save: what is on disk now is what was sent. */
export function markSaved(P, payload) {
  for (const e of payload) {
    if (e.bytes) {
      P.onDisk.add(e.key);
      const t = P.tiles.get(e.key);
      if (t && P.bytes === 2) for (let i = 0; i < t.length; i++) t[i] = clamp(Math.round(t[i]), -HEIGHT_LIMIT, HEIGHT_LIMIT);
    } else {
      P.onDisk.delete(e.key);
      P.tiles.delete(e.key);
    }
    P.dirty.delete(e.key);
    bump(P, e.key);
  }
}

/* ---- sampling -------------------------------------------------------------------- */
/** Sample (i, j), clamped to the plane: the default where the tile is absent or unloaded. */
export function sample(P, i, j) {
  i = clamp(i, 0, P.posts[0] - 1); j = clamp(j, 0, P.posts[1] - 1);
  const t = P.tiles.get(tileKey(i >> 8, j >> 8));
  return t ? t[(j & 255) * TILE + (i & 255)] : P.def;
}
export const inExtent = (P, x, z) => x >= P.min[0] && z >= P.min[1] &&
  x <= P.min[0] + (P.posts[0] - 1) * P.sp && z <= P.min[1] + (P.posts[1] - 1) * P.sp;
/** The authored height at a world column (voxels), bilinear between posts. */
export function heightAt(P, x, z) {
  const fx = (x - P.min[0]) / P.sp, fz = (z - P.min[1]) / P.sp;
  const i = Math.floor(fx), j = Math.floor(fz), u = fx - i, v = fz - j;
  const a = sample(P, i, j) + (sample(P, i + 1, j) - sample(P, i, j)) * u;
  const b = sample(P, i, j + 1) + (sample(P, i + 1, j + 1) - sample(P, i, j + 1)) * u;
  return a + (b - a) * v;
}
/** The biome palette index of the cell holding world column (x, z). */
export function biomeAt(P, x, z) {
  return sample(P, Math.floor((x - P.min[0]) / P.sp), Math.floor((z - P.min[1]) / P.sp));
}

/* ---- windows: read a rectangle of samples, write it back tile by tile ------------- */
/** Samples [i0, i0+w) x [j0, j0+h), clamped at the plane edge, as a Float32Array. */
export function readWindow(P, i0, j0, w, h) {
  const out = new Float32Array(w * h);
  for (let j = 0; j < h; j++) {
    const jj = clamp(j0 + j, 0, P.posts[1] - 1), row = (jj & 255) * TILE, tz = jj >> 8;
    let ctx = -1, t = null;
    for (let i = 0; i < w; i++) {
      const ii = clamp(i0 + i, 0, P.posts[0] - 1), tx = ii >> 8;
      if (tx !== ctx) { ctx = tx; t = P.tiles.get(tileKey(tx, tz)) || null; }
      out[j * w + i] = t ? t[row + (ii & 255)] : P.def;
    }
  }
  return out;
}
function bump(P, k) { P.ver.set(k, (P.ver.get(k) || 0) + 1); P.gen++; }
/** Bump the display version of every tile within `pad` samples of a sample rect
 *  (a tile's hillshade and contours read one neighbour sample past its edge). */
export function touchRect(P, i0, j0, i1, j1, pad = 32) {
  const tx0 = Math.max(0, (i0 - pad) >> 8), tx1 = Math.min(P.tilesX - 1, (i1 + pad) >> 8);
  const tz0 = Math.max(0, (j0 - pad) >> 8), tz1 = Math.min(P.tilesZ - 1, (j1 + pad) >> 8);
  for (let tz = tz0; tz <= tz1; tz++) for (let tx = tx0; tx <= tx1; tx++) bump(P, tileKey(tx, tz));
}
/** For each sample of the window where `f(index, current)` returns a number
 *  different from the current value, write it. `before` (key -> copy | null)
 *  collects each touched tile's prior state for undo; null = was absent.
 *  Returns {changed, skipped} (skipped = samples in listed, unloaded tiles). */
export function writeWindow(P, i0, j0, w, h, f, before) {
  let changed = 0, skipped = 0;
  const ia = Math.max(0, i0), ib = Math.min(P.posts[0] - 1, i0 + w - 1);
  const ja = Math.max(0, j0), jb = Math.min(P.posts[1] - 1, j0 + h - 1);
  if (ia > ib || ja > jb) return {changed, skipped};
  const isH = P.bytes === 2;
  for (let tz = ja >> 8; tz <= jb >> 8; tz++)
    for (let tx = ia >> 8; tx <= ib >> 8; tx++) {
      const k = tileKey(tx, tz);
      const x0 = Math.max(ia, tx * TILE), x1 = Math.min(ib, tx * TILE + TILE - 1);
      const z0 = Math.max(ja, tz * TILE), z1 = Math.min(jb, tz * TILE + TILE - 1);
      if (blocked(P, k)) { skipped += (x1 - x0 + 1) * (z1 - z0 + 1); continue; }
      let t = P.tiles.get(k), fresh = false, here = 0;
      for (let j = z0; j <= z1; j++)
        for (let i = x0; i <= x1; i++) {
          const cur = t ? t[(j & 255) * TILE + (i & 255)] : P.def;
          let nv = f((j - j0) * w + (i - i0), cur);
          if (nv == null) continue;
          if (isH) { nv = clamp(nv, -HEIGHT_LIMIT, HEIGHT_LIMIT); if (Math.abs(nv - cur) < 1e-4) continue; }
          else if (nv === cur) continue;
          if (!t) { t = newTile(P); fresh = true; P.tiles.set(k, t); }
          if (before && !before.has(k)) before.set(k, fresh ? null : copyTile(t));
          t[(j & 255) * TILE + (i & 255)] = nv;
          here++;
        }
      if (here) { P.dirty.add(k); changed += here; }
    }
  if (changed) touchRect(P, ia, ja, ib, jb);
  return {changed, skipped};
}
/** Swap the tiles named in `m` (key -> tile | null) into the plane; returns the
 *  same keys' current state, which is the opposite (undo / redo) stack's entry. */
export function swapTiles(P, m) {
  const back = new Map();
  for (const [k, t] of m) {
    const cur = P.tiles.get(k);
    back.set(k, cur ? copyTile(cur) : null);
    if (t) P.tiles.set(k, copyTile(t)); else P.tiles.delete(k);
    P.dirty.add(k);
    const [tx, tz] = tileXZ(k);
    touchRect(P, tx * TILE, tz * TILE, tx * TILE + TILE - 1, tz * TILE + TILE - 1);
  }
  return back;
}

/* ---- brushes ----------------------------------------------------------------------- */
/** The smooth cosine falloff every brush uses: 1 at the centre, 0 at r. */
export const falloff = (d, r) => (d >= r ? 0 : 0.5 * (1 + Math.cos(Math.PI * d / r)));
/** The sample rectangle a world-space disc of radius r around (wx, wz) covers. */
function discRect(P, wx, wz, r, centred) {
  const o = centred ? 0.5 : 0;   // a cell-centred sample i sits at min + (i + 0.5) * sp
  return [Math.floor((wx - r - P.min[0]) / P.sp - o), Math.floor((wz - r - P.min[1]) / P.sp - o),
          Math.ceil((wx + r - P.min[0]) / P.sp - o), Math.ceil((wz + r - P.min[1]) / P.sp - o)];
}
/** The height modes this package ships (E2 adds the rest). */
export const HEIGHT_MODES = ['raise', 'lower', 'set', 'smooth'];
/** One dab of a height brush at world column (wx, wz).
 *  o = {mode, radius (vox), amount (vox at the centre per dab: raise / lower),
 *       target (vox: set), k (0..1: how far set / smooth go per dab)}.
 *  The brush is never narrower than one post spacing: a 1 m brush on 6.4 m
 *  posts moves the nearest post. Returns writeWindow's {changed, skipped}. */
export function heightDab(P, wx, wz, o, before) {
  const r = Math.max(o.radius, P.sp);
  const [i0, j0, i1, j1] = discRect(P, wx, wz, r, false);
  const w = i1 - i0 + 1, h = j1 - j0 + 1;
  const wt = new Float32Array(w * h);
  let any = false;
  for (let j = 0; j < h; j++)
    for (let i = 0; i < w; i++) {
      const d = Math.hypot(P.min[0] + (i0 + i) * P.sp - wx, P.min[1] + (j0 + j) * P.sp - wz);
      const f = falloff(d, r);
      if (f > 0) { wt[j * w + i] = f; any = true; }
    }
  if (!any) return {changed: 0, skipped: 0};
  const k = clamp(o.k == null ? 0.5 : o.k, 0, 1);
  let f;
  if (o.mode === 'raise') f = (n, cur) => (wt[n] ? cur + o.amount * wt[n] : null);
  else if (o.mode === 'lower') f = (n, cur) => (wt[n] ? cur - o.amount * wt[n] : null);
  else if (o.mode === 'set') f = (n, cur) => (wt[n] ? cur + (o.target - cur) * Math.min(1, wt[n] * k * 2) : null);
  else if (o.mode === 'smooth') {
    // reads its neighbourhood (one sample of apron) BEFORE writing any of it
    const g = readWindow(P, i0 - 1, j0 - 1, w + 2, h + 2), W = w + 2;
    f = (n, cur) => {
      if (!wt[n]) return null;
      const i = n % w + 1, j = Math.floor(n / w) + 1;
      let s = 0;
      for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) s += g[(j + dz) * W + i + dx];
      return cur + (s / 9 - cur) * wt[n] * k;
    };
  } else throw new Error('unknown height mode ' + o.mode);
  return writeWindow(P, i0, j0, w, h, f, before);
}
/** One dab of the biome brush: every cell whose CENTRE is inside the disc
 *  (at least the cell under the cursor) becomes palette index `index`. */
export function biomeDab(P, wx, wz, radius, index, before) {
  const r = Math.max(0, radius);
  const [i0, j0, i1, j1] = discRect(P, wx, wz, r, true);
  const w = i1 - i0 + 1, h = j1 - j0 + 1;
  const ci = Math.floor((wx - P.min[0]) / P.sp), cj = Math.floor((wz - P.min[1]) / P.sp);
  return writeWindow(P, i0, j0, w, h, (n) => {
    const i = i0 + n % w, j = j0 + Math.floor(n / w);
    const d = Math.hypot(P.min[0] + (i + 0.5) * P.sp - wx, P.min[1] + (j + 0.5) * P.sp - wz);
    return d <= r || (i === ci && j === cj) ? index : null;
  }, before);
}

/* ---- display support: mip pyramids and the plane's real range ----------------------- */
/** Level L (0 = the posts) of tile k: (256 >> L)^2 box-averaged samples. */
export function mipLevel(P, k, L) {
  const ver = P.ver.get(k) || 0;
  let e = P.mips.get(k);
  if (!e || e.ver !== ver) { e = {ver, levels: []}; P.mips.set(k, e); }
  if (!e.levels[0]) {
    let t = P.tiles.get(k);
    if (!t) { if (!P.defTile) { P.defTile = new Float32Array(TILE * TILE); P.defTile.fill(P.def); } t = P.defTile; }
    e.levels[0] = t instanceof Float32Array ? t : Float32Array.from(t);
  }
  for (let l = 1; l <= L; l++) {
    if (e.levels[l]) continue;
    const s = e.levels[l - 1], n = TILE >> l, m = n * 2, d = new Float32Array(n * n);
    for (let j = 0; j < n; j++)
      for (let i = 0; i < n; i++) {
        const a = 2 * j * m + 2 * i;
        d[j * n + i] = (s[a] + s[a + 1] + s[a + m] + s[a + m + 1]) * 0.25;
      }
    e.levels[l] = d;
  }
  return e.levels[L];
}
/** [min, max] over every sample INSIDE the plane: loaded tiles' samples, plus
 *  the default if any tile is absent (or still unloaded). */
export function planeRange(P) {
  let lo = Infinity, hi = -Infinity, anyDefault = false;
  for (let tz = 0; tz < P.tilesZ; tz++)
    for (let tx = 0; tx < P.tilesX; tx++) {
      const k = tileKey(tx, tz), t = P.tiles.get(k);
      if (!t) { anyDefault = true; continue; }
      const ver = P.ver.get(k) || 0;
      let e = P.mm.get(k);
      if (!e || e.ver !== ver) {
        const [sx, sz] = tileSpan(P, tx, tz);
        let a = Infinity, b = -Infinity;
        for (let j = 0; j < sz; j++) for (let i = 0; i < sx; i++) { const v = t[j * TILE + i]; if (v < a) a = v; if (v > b) b = v; }
        e = {ver, min: a, max: b};
        P.mm.set(k, e);
      }
      if (e.min < lo) lo = e.min;
      if (e.max > hi) hi = e.max;
    }
  if (anyDefault) { lo = Math.min(lo, P.def); hi = Math.max(hi, P.def); }
  return [lo, hi];
}
