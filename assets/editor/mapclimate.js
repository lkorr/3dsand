/* mapclimate.js — THE CLIMATE PANEL of the World map page (docs/PLAN_worldbuilder.md
 * E2 item 4): the auto-biome generator's inputs, its overlays, "Generate
 * biomes", and the LOCKS that keep hand-painted cells.
 *
 * THE INPUTS live in map.json `climate` (EDITOR-ONLY, §2.4: envlive lists it,
 * tuner_server checks only that it is an object, the engine never reads it):
 * the wind direction (a dial), sea temperature, latitude gradient and cold
 * edge, lapse, aspect, coastal pull, rainfall, the orographic factor, the
 * wetness weight, the T / M match weights, the border jitter, the min patch,
 * the seed, the glacier limits, and `never` (biomes the generator never
 * picks; the per-biome "auto" boxes). climate.js is the maths.
 *
 * OVERLAYS (this panel's own selector): temperature, precipitation, moisture,
 * wetness and the PREDICTED biome of the whole map, computed in the terrain
 * worker (job 'climate') a moment after the map or an input changes.
 *
 * GENERATE: over the Area (or the whole map) the worker classifies every
 * biome cell; the result is shown as a PREVIEW (the predicted colours inside
 * the target, nothing written); Commit writes the biome plane in ONE undo
 * step, adding any biome the map's palette (`biomes[]`) does not hold yet
 * (at most 256), Discard drops it.
 *
 * THE LOCKS: a cell painted with the Biome brush is LOCKED (while "lock
 * painted cells" is on) and every regeneration keeps its biome. The Lock tool
 * paints / clears locks directly. Stored in map.json climate.locks as a
 * run-length string over the biome plane's cells ({cells: [nx, nz], rle}:
 * base-36 runs, alternating unlocked / locked) -- chosen over an editor-only
 * plane of tiles because it needs no new server route or tile kind, rides
 * the map.json undo snapshots for free, and a hand-painted mask is a few
 * hundred runs, not megabytes. The engine never reads `climate`. */

import * as MT from './maptiles.js';
import * as CL from './climate.js';

const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const OVERLAYS = [['none', 'no climate overlay'], ['temp', 'temperature'], ['precip', 'precipitation'], ['moist', 'moisture'], ['wet', 'wetness'], ['biome', 'predicted biome']];
// the overlay colour ramps (the legend draws the same stops)
export const RAMPS = {
  temp: {lo: -20, hi: 30, stops: [[40, 60, 200], [120, 180, 240], [240, 240, 240], [250, 200, 80], [210, 50, 30]]},
  precip: {lo: 0, hi: 3000, stops: [[150, 100, 50], [220, 200, 90], [90, 180, 70], [30, 120, 200], [20, 40, 160]]},
  moist: {lo: 0, hi: 1, stops: [[150, 100, 50], [220, 200, 90], [90, 180, 70], [30, 120, 200], [20, 40, 160]]},
  wet: {lo: 0, hi: 1, stops: [[20, 40, 60], [40, 140, 200], [120, 230, 255]]},
};
export function rampAt(R, v) {
  const t = clamp((v - R.lo) / (R.hi - R.lo), 0, 1), s = t * (R.stops.length - 1), i = Math.min(R.stops.length - 2, Math.floor(s)), u = s - i;
  return R.stops[i].map((c, q) => c + (R.stops[i + 1][q] - c) * u);
}

export function createClimateLayer(C) {
  const st = {
    table: null, tableErr: '', loading: null,
    overlay: 'none', alpha: 0.6,
    res: null,            // {key, T, P, M, W, biome?, w, hh} the last whole-map run
    img: null,            // {key, canvas}
    timer: 0, job: null,
    pending: null,        // {biome, rect, gen: [hgen, bgen], counts, names}
    lockPainted: true, showLocks: true,
    mask: null, maskKey: null, maskVer: 0, lockDirty: false, lockImg: null,
    lstroke: null,        // the Lock tool's stroke: {before (locks), unlock}
    lock: {radiusM: 100, mode: 'lock'},
  };
  const els = {};
  const J = () => C.map().json;
  const v = () => C.vpm();
  const clim = () => (J().climate && typeof J().climate === 'object' ? J().climate : (J().climate = {}));
  const P = () => CL.params(C.map() ? J().climate : null);
  const B = () => C.map().B;

  /* ---- the biome table (every assets/biomes/*.json climate pair) ---- */
  async function loadTable() {
    if (st.table || st.loading) return st.loading;
    st.loading = (async () => {
      try {
        const r = await fetch('/api/models', {cache: 'no-store'});
        const j = await r.json();
        const names = [...new Set((j.files || []).filter(f => f.dir === 'biomes' && f.name.endsWith('.json') && f.name[0] !== '_').map(f => f.name.slice(0, -5)))].sort();
        const rows = await Promise.all(names.map(async n => {
          const rr = await fetch('/api/model?path=biomes/' + encodeURIComponent(n + '.json'), {cache: 'no-store'});
          if (!rr.ok) return null;
          const b = await rr.json(), c = b.climate || {};
          return isFinite(+c.temperature) && isFinite(+c.moisture) ? {name: n, T: +c.temperature, M: +c.moisture} : null;
        }));
        st.table = rows.filter(Boolean);
        st.tableErr = st.table.length ? '' : 'no biome under assets/biomes carries a climate pair';
      } catch (e) { st.tableErr = 'biome list: ' + e.message; st.table = null; }
      st.loading = null;
      sync();
    })();
    return st.loading;
  }

  /* ---- locks ---- */
  const lockKey = L => (L ? L.cells + '|' + L.rle : '');
  function mask() {
    const P0 = B(), nx = P0.posts[0], nz = P0.posts[1];
    if (st.lockDirty && st.mask) return st.mask;
    const L = (J().climate && J().climate.locks) || null, key = lockKey(L);
    if (st.mask && st.mask.length === nx * nz && st.maskKey === key) return st.mask;
    st.mask = CL.decodeLocks(L, nx, nz) || new Uint8Array(nx * nz);
    st.maskKey = key; st.maskVer++;
    return st.mask;
  }
  function setLocks(L) {
    if (L) clim().locks = L; else if (J().climate) delete J().climate.locks;
    st.lockDirty = false; st.mask = null;
    mask();
  }
  const curLocks = () => { const L = J().climate && J().climate.locks; return L ? {cells: [...L.cells], rle: L.rle} : null; };
  const lockSwap = L => ({swap() { const cur = curLocks(); setLocks(L); sync(); C.paint(); return lockSwap(cur); }});
  function flushLocks() {
    if (!st.lockDirty) return;
    const P0 = B();
    setLocks(CL.encodeLocks(st.mask, P0.posts[0], P0.posts[1]));
  }
  /** Mark (or clear) every cell whose centre is inside the disc (the biome
   *  brush's own rule: at least the cell under the cursor). True if any changed. */
  function markDisc(wx, wz, r, val) {
    const P0 = B(), m = mask(), sp = P0.sp, nx = P0.posts[0], nz = P0.posts[1];
    const ci = Math.floor((wx - P0.min[0]) / sp), cj = Math.floor((wz - P0.min[1]) / sp);
    let any = false;
    for (let j = Math.floor((wz - r - P0.min[1]) / sp) - 1; j <= Math.ceil((wz + r - P0.min[1]) / sp); j++)
      for (let i = Math.floor((wx - r - P0.min[0]) / sp) - 1; i <= Math.ceil((wx + r - P0.min[0]) / sp); i++) {
        if (i < 0 || j < 0 || i >= nx || j >= nz) continue;
        const d = Math.hypot(P0.min[0] + (i + 0.5) * sp - wx, P0.min[1] + (j + 0.5) * sp - wz);
        if (!(d <= r || (i === ci && j === cj))) continue;
        const n = j * nx + i;
        if (m[n] !== val) { m[n] = val; any = true; }
      }
    if (any) { st.lockDirty = true; st.maskVer++; }
    return any;
  }
  /** map.js: the Biome brush painted a dab -- lock what it painted. The
   *  stroke's undo entry gets a `custom` that puts the locks back. */
  function biomePainted(wx, wz, r, stroke) {
    if (!st.lockPainted || !C.map() || !stroke) return;
    const before = st.lockDirty ? null : curLocks();
    if (markDisc(wx, wz, r, 1) && !stroke.custom) stroke.custom = lockSwap(before);
  }
  function lockCount() { const m = mask(); let n = 0; for (let k = 0; k < m.length; k++) n += m[k]; return n; }

  /* ---- the job ---- */
  function gridOf() {
    const P0 = B(), H = C.map().H, nx = P0.posts[0], nz = P0.posts[1];
    const h = new Float32Array(nx * nz);
    for (let j = 0; j < nz; j++) {
      const z = P0.min[1] + (j + 0.5) * P0.sp;
      for (let i = 0; i < nx; i++) h[j * nx + i] = MT.heightAt(H, P0.min[0] + (i + 0.5) * P0.sp, z);
    }
    return {w: nx, hh: nz, x0: P0.min[0] + P0.sp / 2, z0: P0.min[1] + P0.sp / 2, cell: P0.sp, h};
  }
  function rectOfArea() {
    const a = C.area();
    if (!a) return null;
    const P0 = B(), nx = P0.posts[0], nz = P0.posts[1];
    const i0 = clamp(Math.floor((a[0] - P0.min[0]) / P0.sp), 0, nx - 1), i1 = clamp(Math.floor((a[2] - P0.min[0]) / P0.sp), 0, nx - 1);
    const j0 = clamp(Math.floor((a[1] - P0.min[1]) / P0.sp), 0, nz - 1), j1 = clamp(Math.floor((a[3] - P0.min[1]) / P0.sp), 0, nz - 1);
    return [i0, j0, i1 - i0 + 1, j1 - j0 + 1];
  }
  const inputKey = () => JSON.stringify({c: {...(J().climate || {}), locks: undefined}, r: J().rivers || null, l: J().lakes || null});
  function makeJob(classify, rect) {
    const m = C.map(), P0 = m.B, nx = P0.posts[0], nz = P0.posts[1];
    const tbl = st.table, idx = new Map(tbl.map((b, k) => [b.name, k]));
    const cur = new Uint8Array(nx * nz);
    const src = MT.readWindow(P0, 0, 0, nx, nz), names = m.json.biomes;
    for (let n = 0; n < cur.length; n++) { const k = idx.get(names[src[n]]); cur[n] = k == null ? 255 : k; }
    const e = m.json.extent;
    return {job: 'climate', grid: gridOf(), seaY: C.seaY(), vpm: v(), params: {...(m.json.climate || {}), locks: undefined},
            centre: [e.min[0] + e.size[0] / 2, e.min[1] + e.size[1] / 2], biomes: tbl, current: cur, locks: Uint8Array.from(mask()),
            rect, rivers: m.json.rivers || [], lakes: m.json.lakes || [], classify};
  }
  function ready() {
    if (!C.map()) return false;
    if (!st.table) { loadTable(); C.status(st.tableErr || 'loading the biome list...'); return false; }
    if (MT.pending(C.map().H) || MT.pending(B())) { C.status('climate: plane tiles are still loading; it runs when they are in'); return false; }
    return true;
  }
  /* the overlays: a quiet whole-map run, a moment after anything changes */
  const wantKey = () => (C.map() ? [C.map().H.gen, C.map().B.gen, st.maskVer, inputKey()].join('#') : '');
  function scheduleOverlay() {
    if (st.overlay === 'none' || !C.map() || st.timer || st.job) return;
    if (st.res && st.res.key === wantKey()) return;
    st.timer = setTimeout(() => { st.timer = 0; runOverlay(); }, st.res ? 700 : 30);
  }
  function runOverlay() {
    if (st.overlay === 'none' || !ready()) return;
    const key = wantKey(), m = C.map();
    let run;
    try { run = C.runJob(makeJob(st.overlay === 'biome', null), {quiet: true, key: 'climate', onCancel: () => { st.job = null; }}); }
    catch (e) { C.status('climate overlay: ' + e.message); return; }
    st.job = run;
    run.then(res => {
      st.job = null;
      if (run.cancelled || C.map() !== m) return;
      st.res = {key, ...res, w: m.B.posts[0], hh: m.B.posts[1], table: st.table.map(b => b.name)};
      st.img = null;
      sync(); C.paint();
      if (wantKey() !== key) scheduleOverlay();
    }).catch(e => { st.job = null; if (!run.cancelled) C.status('climate overlay: ' + e.message); });
  }
  /** GENERATE: classify the Area (or the whole map) in the worker and hold
   *  the result as a preview; Commit writes it. Resolves to the pending record. */
  async function generate() {
    if (!ready()) { if (!st.table) await loadTable(); if (!ready()) return null; }
    if (C.jobBusy()) { C.toast('another terrain job is running', true); return null; }
    const m = C.map(), rect = rectOfArea();
    let run;
    try { run = C.runJob(makeJob(true, rect), {label: 'climate + biomes over ' + (rect ? 'the Area' : 'the whole map')}); }
    catch (e) { C.toast(e.message, true); return null; }
    const t0 = performance.now();
    let res;
    try { res = await run; } catch (e) { if (!run.cancelled) C.toast('climate: ' + e.message, true); return null; }
    if (run.cancelled || C.map() !== m) return null;
    st.pending = {biome: res.biome, rect: rect || [0, 0, m.B.posts[0], m.B.posts[1]], gen: [m.H.gen, m.B.gen], counts: res.counts, names: st.table.map(b => b.name)};
    // the whole-map fields come along: the overlays use them
    st.res = {key: wantKey(), ...res, biome: rect ? null : res.biome, w: m.B.posts[0], hh: m.B.posts[1], table: st.pending.names};
    st.img = null;
    const changed = countChanges();
    sync(); C.paint();
    C.status(`biomes generated over ${rect ? 'the Area' : 'the whole map'} in ${((performance.now() - t0) / 1000).toFixed(1)} s: ${changed} cells would change (${lockCount()} locked kept) — PREVIEW; Commit writes the biome plane, Discard drops it`);
    return st.pending;
  }
  function countChanges() {
    const p = st.pending;
    if (!p) return 0;
    const P0 = B(), nx = P0.posts[0], names = J().biomes;
    let n = 0;
    const [i0, j0, w, h] = p.rect;
    const cur = MT.readWindow(P0, i0, j0, w, h);
    for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
      const b = p.biome[(j0 + j) * nx + i0 + i];
      if (b !== 255 && names[cur[j * w + i]] !== p.names[b]) n++;
    }
    return n;
  }
  function commit() {
    const p = st.pending, m = C.map();
    if (!p || !m) return null;
    if (m.H.gen !== p.gen[0] || m.B.gen !== p.gen[1]) { C.toast('the map changed since the preview: Generate again', true); return null; }
    const snap = C.snapshot(), names = m.json.biomes, pal = new Map();
    const used = new Set();
    for (let n = 0; n < p.biome.length; n++) if (p.biome[n] !== 255) used.add(p.biome[n]);
    const add = [...used].map(b => p.names[b]).filter(n => !names.includes(n));
    if (names.length + add.length > 256) { C.toast(`the palette would hold ${names.length + add.length} biomes; at most 256`, true); return null; }
    for (const n of add) names.push(n);
    for (const b of used) pal.set(b, names.indexOf(p.names[b]));
    const P0 = m.B, nx = P0.posts[0], [i0, j0, w, h] = p.rect, before = new Map();
    const r = MT.writeWindow(P0, i0, j0, w, h, (n) => {
      const b = p.biome[(j0 + Math.floor(n / w)) * nx + i0 + n % w];
      return b === 255 ? null : pal.get(b);
    }, before);
    st.pending = null;
    if (r.changed || add.length) {
      C.pushUndo({tiles: {biome: before}, json: snap.json});
      C.markDirty(true);
    }
    if (C.rebuildPalette) C.rebuildPalette();
    C.imgCacheClear();
    sync(); C.paint();
    C.status(`committed: ${r.changed} biome cells written` + (add.length ? `; added ${add.join(', ')} to the palette` : '') + (r.skipped ? `; ${r.skipped} in tiles still loading SKIPPED` : '') + ' — Ctrl+Z takes it back');
    return r;
  }
  function discard() { st.pending = null; sync(); C.paint(); }

  /* ---- painting ---- */
  function overlayImage() {
    const R = st.res;
    if (!R) return null;
    const key = st.overlay + '|' + R.key + '|' + (st.pending ? 'p' : '');
    if (st.img && st.img.key === key) return st.img.canvas;
    const kind = st.pending ? 'biome' : st.overlay;
    const cv = document.createElement('canvas');
    cv.width = R.w; cv.height = R.hh;
    const ctx = cv.getContext('2d'), im = ctx.createImageData(R.w, R.hh), d = im.data;
    if (kind === 'biome') {
      const src = st.pending ? st.pending.biome : R.biome, names = st.pending ? st.pending.names : R.table;
      if (!src) return null;
      const cols = names.map(n => hexRgb(C.colorOf ? C.colorOf(n) : CL.BIOME_COLORS[n] || '#ff00ff'));
      for (let n = 0; n < src.length; n++) {
        const b = src[n];
        if (b === 255) continue;
        const c = cols[b], o = n * 4;
        d[o] = c[0]; d[o + 1] = c[1]; d[o + 2] = c[2]; d[o + 3] = 255;
      }
    } else {
      const f = {temp: R.T, precip: R.P, moist: R.M, wet: R.W}[kind], ramp = RAMPS[kind];
      if (!f) return null;
      for (let n = 0; n < f.length; n++) {
        const c = rampAt(ramp, f[n]), o = n * 4;
        d[o] = c[0]; d[o + 1] = c[1]; d[o + 2] = c[2]; d[o + 3] = kind === 'wet' ? Math.round(255 * clamp(f[n] * 1.4, 0, 1)) : 255;
      }
    }
    ctx.putImageData(im, 0, 0);
    st.img = {key, canvas: cv};
    return cv;
  }
  function lockImage() {
    const m = mask(), P0 = B(), nx = P0.posts[0], nz = P0.posts[1];
    if (st.lockImg && st.lockImg.ver === st.maskVer && st.lockImg.n === m.length) return st.lockImg.canvas;
    const cv = document.createElement('canvas');
    cv.width = nx; cv.height = nz;
    const ctx = cv.getContext('2d'), im = ctx.createImageData(nx, nz), d = im.data;
    for (let j = 0; j < nz; j++) for (let i = 0; i < nx; i++) {
      const n = j * nx + i;
      if (!m[n]) continue;
      const o = n * 4, stripe = ((i + j) & 3) === 0;
      d[o] = 255; d[o + 1] = 255; d[o + 2] = 255; d[o + 3] = stripe ? 170 : 45;
    }
    ctx.putImageData(im, 0, 0);
    st.lockImg = {ver: st.maskVer, n: m.length, canvas: cv};
    return cv;
  }
  function paint(ctx) {
    const m = C.map();
    if (!m) return;
    const P0 = m.B, SX = C.SX, SZ = C.SZ, view = C.view;
    const x0 = SX(P0.min[0]), z0 = SZ(P0.min[1]), W = P0.posts[0] * P0.sp * view.scale, Hh = P0.posts[1] * P0.sp * view.scale;
    if (st.overlay !== 'none' || st.pending) {
      scheduleOverlay();
      const img = overlayImage();
      if (img) {
        ctx.globalAlpha = st.pending ? Math.max(0.75, st.alpha) : st.alpha;
        ctx.imageSmoothingEnabled = !(st.pending || st.overlay === 'biome');
        ctx.drawImage(img, x0, z0, W, Hh);
        ctx.imageSmoothingEnabled = false;
        ctx.globalAlpha = 1;
      }
      if (st.pending) {
        const [i0, j0, w, h] = st.pending.rect;
        const a = SX(P0.min[0] + i0 * P0.sp), b = SZ(P0.min[1] + j0 * P0.sp);
        ctx.strokeStyle = 'rgba(255,216,102,0.85)'; ctx.setLineDash([3, 3]); ctx.lineWidth = 1;
        ctx.strokeRect(a + 0.5, b + 0.5, w * P0.sp * view.scale, h * P0.sp * view.scale); ctx.setLineDash([]);
        ctx.fillStyle = '#ffd866'; ctx.font = '11px monospace';
        ctx.fillText('PREVIEW: generated biomes (Commit / Discard in the Climate panel)', a + 4, b + 13);
      }
    }
    const t = C.tool();
    if ((st.showLocks || t === 'biolock') && (t === 'biolock' || t === 'biome' || st.pending || st.overlay === 'biome')) {
      ctx.globalAlpha = 1;
      ctx.drawImage(lockImage(), x0, z0, W, Hh);
    }
    syncHover();
  }
  // every paint calls this: write the DOM only when the text changes (a
  // textContent store replaces the node even when equal, and costs a style /
  // layout pass on the panel every frame of a pan)
  const setHover = t => { if (els.hover.textContent !== t) els.hover.textContent = t; };
  function syncHover() {
    if (!els.hover) return;
    const hw = C.hover(), R = st.res, m = C.map();
    if (!hw || !R || !m) { setHover(''); return; }
    const P0 = m.B, i = Math.floor((hw[0] - P0.min[0]) / P0.sp), j = Math.floor((hw[1] - P0.min[1]) / P0.sp);
    if (i < 0 || j < 0 || i >= R.w || j >= R.hh) { setHover(''); return; }
    const n = j * R.w + i, b = R.biome && R.biome[n] !== 255 ? R.table[R.biome[n]] : null;
    setHover(`at cursor: ${R.T[n].toFixed(1)} °C · ${R.P[n].toFixed(0)} mm · moisture ${R.M[n].toFixed(2)} · wetness ${R.W[n].toFixed(2)}` +
      (b ? ' · predicts ' + b : '') + (mask()[j * P0.posts[0] + i] ? ' · LOCKED' : ''));
  }

  /* ---- the panel ---- */
  function edit(fn) {
    C.pushUndo(C.snapshot());
    fn();
    C.markDirty(true);
    st.img = null;
    sync(); scheduleOverlay(); C.paint();
  }
  function dial(key, title) {
    const el = C.el, cv = el('canvas', {width: 56, height: 56, title, style: 'width:56px;height:56px;cursor:pointer;image-rendering:pixelated;border:1px solid #2a3040;border-radius:28px;background:#0e1219'});
    const draw = () => {
      const g = cv.getContext('2d'), a = P()[key] * Math.PI / 180;
      g.clearRect(0, 0, 56, 56);
      g.strokeStyle = '#2a3040'; g.beginPath(); g.arc(28, 28, 25, 0, Math.PI * 2); g.stroke();
      g.fillStyle = '#6f7f97'; g.font = '9px monospace'; g.textAlign = 'center';
      g.fillText('N', 28, 10); g.fillText('S', 28, 53); g.fillText('W', 6, 31); g.fillText('E', 50, 31);
      // the wind blows FROM `a` toward the centre and on: draw the arrow along its travel
      const fx = Math.sin(a), fz = -Math.cos(a);
      g.strokeStyle = '#9fe6ff'; g.lineWidth = 2;
      g.beginPath(); g.moveTo(28 + fx * 20, 28 + fz * 20); g.lineTo(28 - fx * 16, 28 - fz * 16); g.stroke();
      const hx = 28 - fx * 18, hz = 28 - fz * 18, px = -fz, pz = fx;
      g.fillStyle = '#9fe6ff'; g.beginPath(); g.moveTo(hx, hz); g.lineTo(hx + fx * 7 + px * 4, hz + fz * 7 + pz * 4); g.lineTo(hx + fx * 7 - px * 4, hz + fz * 7 - pz * 4); g.closePath(); g.fill();
    };
    let drag = false;
    const setFrom = ev => {
      const r = cv.getBoundingClientRect(), dx = ev.clientX - r.left - r.width / 2, dz = ev.clientY - r.top - r.height / 2;
      if (Math.hypot(dx, dz) < 3) return;
      const deg = (Math.round((Math.atan2(dx, -dz) * 180 / Math.PI) / 5) * 5 + 360) % 360;
      if (deg !== P()[key]) { clim()[key] = deg; st.img = null; draw(); sync(); }
    };
    cv.addEventListener('pointerdown', ev => { if (!C.map()) return; C.pushUndo(C.snapshot()); drag = true; setFrom(ev); });
    cv.addEventListener('pointermove', ev => { if (drag) setFrom(ev); });
    const end = () => { if (!drag) return; drag = false; C.markDirty(true); scheduleOverlay(); C.paint(); };
    cv.addEventListener('pointerup', end); cv.addEventListener('pointerleave', end);
    cv._draw = draw;
    return cv;
  }
  function buildPanel() {
    const el = C.el;
    const det = el('details', {class: 'maptools', open: false});
    det.append(el('summary', {}, 'Climate — temperature, rain shadow, auto-biomes (map.json climate: editor-only)'));
    det.addEventListener('toggle', () => { if (det.open) loadTable(); });
    els.wind = dial('windFrom', 'the PREVAILING WIND: drag to set the direction it blows FROM (the arrow shows where it goes). The windward side of a range gets the rain, the lee its rain shadow.');
    els.windOut = el('span', {class: 'mapnote'});
    els.params = el('div', {class: 'mapgrid2'});
    for (const [key, label, min, max, step, unit, title] of CL.PARAM_ROWS) {
      if (key === 'windFrom') continue;
      const i = el('input', {type: 'number', min, max, step, style: 'width:62px', title});
      i.addEventListener('change', () => edit(() => { clim()[key] = clamp(+i.value || 0, min, max); }));
      els['p_' + key] = i;
      els.params.append(el('label', {title}, label + ' ', el('span', {}, i, el('span', {class: 'mapunit'}, ' ' + unit))));
    }
    const rs = el('button', {title: 'every climate input back to its default (the never list and the locks stay)'}, 'defaults');
    rs.addEventListener('click', () => edit(() => { const c = clim(); for (const [k] of CL.PARAM_ROWS) delete c[k]; }));
    // the overlays
    els.ov = el('select', {title: 'a CLIMATE overlay: temperature (°C), precipitation (mm a year), moisture (0..1 = /3000 mm, the biome files’ scale), wetness (flow + rivers / lakes), or the biome the generator predicts for every cell'},
                ...OVERLAYS.map(([k, l]) => el('option', {value: k}, l)));
    els.ov.addEventListener('change', () => { st.overlay = els.ov.value; st.img = null; scheduleOverlay(); C.paint(); sync(); });
    els.alpha = el('input', {type: 'range', min: 10, max: 100, value: Math.round(st.alpha * 100), style: 'width:80px', title: 'overlay opacity'});
    els.alpha.addEventListener('input', () => { st.alpha = +els.alpha.value / 100; C.paint(); });
    els.hover = el('div', {class: 'mapnote', style: 'color:#9fe6ff'});
    // generate
    els.target = el('div', {class: 'mapnote'});
    els.gen = el('button', {class: 'mapgo', title: 'classify every biome cell of the Area (or the whole map) from its climate, in a worker; the result is a PREVIEW until Commit'}, 'Generate biomes');
    els.gen.addEventListener('click', () => generate());
    els.commit = el('button', {title: 'write the previewed biomes into the biome plane (one undo step); biomes the palette lacks are added'}, 'Commit');
    els.commit.addEventListener('click', commit);
    els.discard = el('button', {title: 'drop the preview; nothing was written'}, 'Discard');
    els.discard.addEventListener('click', discard);
    els.pend = el('div', {class: 'mapnote'});
    // the locks
    els.lockP = el('input', {type: 'checkbox', title: 'every cell the Biome brush paints is LOCKED: Generate keeps its biome'});
    els.lockP.checked = st.lockPainted;
    els.lockP.addEventListener('change', () => { st.lockPainted = els.lockP.checked; });
    els.lockS = el('input', {type: 'checkbox', title: 'draw the locked cells (white hatching) while the Biome / Lock tools are active or a biome preview is shown'});
    els.lockS.checked = st.showLocks;
    els.lockS.addEventListener('change', () => { st.showLocks = els.lockS.checked; C.paint(); });
    els.lockN = el('span', {class: 'mapnote'});
    const clr = el('button', {title: 'unlock every cell (one undo step)'}, 'clear locks');
    clr.addEventListener('click', () => { const before = curLocks(); if (!before) return; setLocks(null); C.pushUndo({custom: lockSwap(before)}); C.markDirty(true); sync(); C.paint(); });
    // the auto list (never)
    els.auto = el('div', {class: 'mapgrid2'});
    els.autoBox = el('details', {}, el('summary', {class: 'mapnote'}, 'auto biomes — untick one and Generate never picks it (map.json climate.never)'), els.auto);
    det.append(
      el('div', {class: 'mapsub'}, 'Inputs'),
      el('div', {class: 'mapbar'}, els.wind, el('div', {}, el('div', {}, 'wind from'), els.windOut)),
      els.params, el('div', {class: 'mapbar'}, rs),
      els.autoBox,
      el('div', {class: 'mapsub'}, 'Overlay'),
      el('div', {class: 'mapbar'}, els.ov, ' opacity ', els.alpha), els.hover,
      el('div', {class: 'mapsub'}, 'Generate'),
      els.target, el('div', {class: 'mapbar'}, els.gen, els.commit, els.discard), els.pend,
      el('div', {class: 'mapsub'}, 'Locks (hand-painted cells the generator keeps)'),
      el('div', {class: 'mapbar'}, el('label', {}, els.lockP, 'lock painted cells'), el('label', {}, els.lockS, 'show locks'), clr), els.lockN);
    sync();
    return det;
  }
  function sync() {
    if (!els.params || !C.map()) return;
    const el = C.el, p = P();
    for (const [key] of CL.PARAM_ROWS) { const i = els['p_' + key]; if (i && document.activeElement !== i) i.value = p[key]; }
    if (els.wind._draw) els.wind._draw();
    els.windOut.textContent = `${p.windFrom}° (${['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'][Math.round(p.windFrom / 45) % 8]})`;
    els.ov.value = st.overlay;
    const rect = rectOfArea();
    els.target.textContent = rect ? `target: the Area, ${rect[2]} × ${rect[3]} biome cells` : 'target: the WHOLE MAP (draw an Area with the Area tool to generate into a box)';
    const pend = st.pending;
    els.commit.disabled = els.discard.disabled = !pend;
    els.gen.disabled = !st.table;
    els.pend.textContent = pend ? 'PREVIEW in the map, not written: ' + Object.entries(pend.counts || {}).sort((a, b) => b[1] - a[1]).slice(0, 8).map(([k, n]) => `${k} ${n}`).join(', ') : (st.tableErr || (st.table ? `${st.table.length} biomes with a climate pair` : 'loading the biome list...'));
    els.lockN.textContent = `${lockCount()} cells locked`;
    if (st.table) {
      const never = new Set(p.never);
      els.auto.replaceChildren(...st.table.map(b => {
        const c = el('input', {type: 'checkbox'});
        c.checked = !never.has(b.name);
        c.addEventListener('change', () => edit(() => {
          const nv = new Set(P().never);
          if (c.checked) nv.delete(b.name); else nv.add(b.name);
          clim().never = [...nv].sort();
        }));
        return el('label', {title: `${b.name}: ${b.T} °C, moisture ${b.M}`}, el('span', {}, el('span', {style: `display:inline-block;width:10px;height:10px;margin-right:4px;background:${C.colorOf ? C.colorOf(b.name) : CL.BIOME_COLORS[b.name]}`}), b.name), c);
      }));
    }
    syncLockOpts();
  }

  /* ---- the Lock tool ---- */
  function lockOpts() {
    const el = C.el;
    els.lkMode = el('select', {title: 'LOCK: cells you paint keep their biome when Generate runs; UNLOCK: let Generate overwrite them again (shift+drag does the other one)'},
                    el('option', {value: 'lock'}, 'lock'), el('option', {value: 'unlock'}, 'unlock'));
    els.lkMode.addEventListener('change', () => { st.lock.mode = els.lkMode.value; });
    els.lkR = el('input', {type: 'number', min: 5, max: 5000, step: 10, value: st.lock.radiusM, style: 'width:60px', title: 'brush radius, m ([ and ])'});
    els.lkR.addEventListener('change', () => { st.lock.radiusM = clamp(+els.lkR.value || 0, 5, 5000); els.lkR.value = st.lock.radiusM; });
    const all = el('button', {title: 'lock every cell of the Area (or the map) whose biome is not the ocean'}, 'lock all land');
    all.addEventListener('click', () => lockLand());
    return el('span', {class: 'mapopts'}, 'locks ', els.lkMode, ' r ', els.lkR, 'm ', all, el('span', {class: 'mapnote'}, ' (white hatching = locked)'));
  }
  function syncLockOpts() { if (els.lkMode) { els.lkMode.value = st.lock.mode; els.lkR.value = st.lock.radiusM; } }
  function lockLand() {
    if (!C.map()) return;
    const before = curLocks(), m = mask(), P0 = B(), nx = P0.posts[0], ocean = J().biomes.indexOf('ocean');
    const rect = rectOfArea() || [0, 0, nx, P0.posts[1]], [i0, j0, w, h] = rect;
    const cur = MT.readWindow(P0, i0, j0, w, h);
    let n = 0;
    for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) if (cur[j * w + i] !== ocean && !m[(j0 + j) * nx + i0 + i]) { m[(j0 + j) * nx + i0 + i] = 1; n++; }
    if (!n) return;
    st.lockDirty = true; st.maskVer++;
    flushLocks();
    C.pushUndo({custom: lockSwap(before)}); C.markDirty(true); sync(); C.paint();
    C.status(`locked ${n} land cells`);
  }
  C.registerTool({
    key: 'biolock', label: 'Lock', group: 'paint', order: 15,
    title: 'LOCK biome cells: painted cells keep their biome when the Climate panel generates (shift+drag unlocks); the Biome brush locks what it paints while "lock painted cells" is on',
    opts: lockOpts,
    down(ev, wx, wz) {
      if (!C.map()) return true;
      const unlock = (st.lock.mode === 'unlock') !== !!ev.shiftKey;
      st.lstroke = {before: curLocks(), val: unlock ? 0 : 1, last: [wx, wz]};
      markDisc(wx, wz, st.lock.radiusM * v(), st.lstroke.val);
      C.paint();
      return true;
    },
    move(ev, fx, fz) {
      const s = st.lstroke;
      if (!s) return;
      const r = st.lock.radiusM * v(), gap = Math.max(B().sp * 0.5, r * 0.3);
      let [lx, lz] = s.last, d = Math.hypot(fx - lx, fz - lz);
      while (d >= gap) { lx += (fx - lx) * gap / d; lz += (fz - lz) * gap / d; markDisc(lx, lz, r, s.val); d = Math.hypot(fx - lx, fz - lz); }
      s.last = [lx, lz];
    },
    // every release (any tool): a Biome brush stroke's locks are written here
    up() {
      const s = st.lstroke, dirty = st.lockDirty;
      st.lstroke = null;
      if (!dirty) return;
      flushLocks();
      if (s) { C.pushUndo({custom: lockSwap(s.before)}); C.markDirty(true); }
      sync();
    },
    cursor(ctx, wx, wz) {
      const r = Math.max(st.lock.radiusM * v(), 1), sx = C.SX(wx), sz = C.SZ(wz);
      ctx.strokeStyle = 'rgba(255,255,255,0.85)'; ctx.setLineDash([2, 2]); ctx.lineWidth = 1;
      ctx.beginPath(); ctx.arc(sx, sz, Math.max(2, r * C.view.scale), 0, Math.PI * 2); ctx.stroke(); ctx.setLineDash([]);
      ctx.fillStyle = '#ffffff'; ctx.font = '11px monospace';
      ctx.fillText(st.lock.mode === 'unlock' ? 'unlock (shift: lock)' : 'lock (shift: unlock)', sx + 10, sz - 10);
    },
    resize(f) { st.lock.radiusM = clamp(Math.round(st.lock.radiusM * f), 5, 5000); syncLockOpts(); },
    paint, sync,
    guide: ['Lock — keep hand-painted biomes',
      'Marks biome cells as LOCKED (white hatching): the Climate panel’s Generate keeps their biome whatever the climate says. The Biome brush locks every cell it paints while “lock painted cells” is on (Climate panel), so your hand painting survives a regeneration.',
      'Left-drag locks (or unlocks, with the mode set to unlock); shift+drag does the other. [ ] radius. “lock all land” locks every non-ocean cell of the Area (or the map) — paint the land as it is now, then unlock only where you want the generator. “clear locks” in the Climate panel unlocks everything. Each stroke is one undo step.',
      'Editor-only: stored in map.json climate.locks (a run-length string over the 25.6 m biome cells); the game never reads it. A map whose extent or biome cell size changes drops its locks.'],
  });

  return {
    st, buildPanel, sync, paint, generate, commit, discard, biomePainted, mask, lockCount, loadTable, flushLocks,
    setOverlay(k) { st.overlay = k; st.img = null; scheduleOverlay(); sync(); C.paint(); },
    reset() {
      st.res = null; st.img = null; st.pending = null; st.mask = null; st.maskKey = null; st.lockDirty = false; st.lockImg = null; st.lstroke = null;
      clearTimeout(st.timer); st.timer = 0; if (st.job) st.job.cancel(); st.job = null;
      sync();
    },
  };
}
const hexRgb = s => {
  if (/^#[0-9a-f]{6}$/i.test(s)) return [1, 3, 5].map(i => parseInt(s.slice(i, i + 2), 16));
  const m = /hsl\((\d+)\s+(\d+)%\s+(\d+)%\)/.exec(s);
  if (!m) return [255, 0, 255];
  const h = +m[1] / 360, sa = +m[2] / 100, l = +m[3] / 100;
  const q = l < 0.5 ? l * (1 + sa) : l + sa - l * sa, p = 2 * l - q;
  const f = t => { t = (t + 1) % 1; return 255 * (t < 1 / 6 ? p + (q - p) * 6 * t : t < 0.5 ? q : t < 2 / 3 ? p + (q - p) * (2 / 3 - t) * 6 : p); };
  return [f(h + 1 / 3), f(h), f(h - 1 / 3)].map(Math.round);
};
