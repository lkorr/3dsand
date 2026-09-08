/* biome.js — the Biome page of the Environment tab.
 *
 * ONE BIOME, ALL ITS STACKS. The left column is the biome file
 * (assets/biomes/<name>.json) as sections — climate & identity, the relief
 * (the height curve and the hill / detail / grain multipliers), ground cover,
 * trees, water, caves — and the right column is a SWATCH: a square of the
 * biome composed by biomegen.js from the same files the engine will read
 * (tree species from assets/trees/, water presets from assets/water/), drawn
 * through WorldView.
 *
 * WHAT IS LIVE AND WHAT IS NOT is one table, assets/editor/envlive.js, and
 * every field on this page goes through envui.liveMark with its JSON path:
 * a field the engine does not read is DISABLED (greyed, tooltip = the plan
 * package that reads it and why). Since PLAN_environment_truth P-G
 * (2026-09-07) everything on this page but the climate coordinates and the
 * cave rows' rarity is read: the biome record the loader packs carries the
 * relief record, the skin, the patch mask, the flags, every cover row with
 * all eight conditions (the canopy pair is what the engine's hard-coded
 * undergrowth / flower chain became), the tree density + weights + row
 * conditions, the water rows and the cave thresholds. The biome itself comes
 * from the painted map (the World map page); there are no worldgen.*
 * thresholds any more.
 *
 * EVERY FEATURE ROW CARRIES THE SAME PLACEMENT CHAIN: a rarity (1-in-N tiles,
 * or a weight, or a percent — one form per stack, the others shown read-only
 * beside it) and CONDITIONS (ground band, slope, distance to water, patch
 * mask). This is Minecraft's placed-feature model, which is the one modders
 * already know how to read.
 */

import * as BG from './biomegen.js';
import * as TG from './treegen.js';
import * as UI from './envui.js';
import * as LV from './envlive.js';

const PAGE = 'env-biome';
const CLS = 'bm';

let H = null;
let wv = null;
let biome = null;
let biomeName = '';
let dirty = false;
let els = {};
let widgets = [];
let undo = null;
let genTimer = 0;
let seed = 1;
let framed = false;
let libs = {water: {}, trees: {}};
let treeCache = new Map();
let view = {trees: true, water: true, cover: true, showcase: true, sizeM: 96};
let lastSwatch = null;
// The swatch is composed in swatch_worker.js (see there for why). One request
// in flight; a request made while one is running waits as `queued` and
// replaces any earlier waiter, so a slider drag costs at most one extra
// compose. `null` = not tried yet, `false` = the worker could not be built and
// the page composes inline as it did before.
let swatchWorker = null, swatchReq = 0, swatchBusy = false, swatchQueued = null;

const vpm = () => (H.voxelsPerMetre && H.voxelsPerMetre()) || BG.DEFAULT_VOX_PER_M;

/* ===========================================================================
 * libraries — the species and preset files the biome refers to
 * ======================================================================== */
async function loadLibs() {
  const r = await fetch('/api/models', {cache: 'no-store'});
  const j = await r.json();
  const files = j.files || [];
  const get = async (dir, name) => {
    const rr = await fetch('/api/model?path=' + dir + '/' + encodeURIComponent(name + '.json'), {cache: 'no-store'});
    return rr.ok ? rr.json() : null;
  };
  const out = {water: {}, trees: {}};
  for (const f of files) {
    if (!f.name.endsWith('.json')) continue;
    if (f.dir === 'water' || f.dir === 'trees') {
      const nm = f.name.slice(0, -5);
      const o = await get(f.dir, nm);
      if (o) out[f.dir][nm] = o;
    }
  }
  libs = out;
  treeCache = new Map();
  if (swatchWorker) swatchWorker.postMessage({cmd: 'libs', libs});
  return libs;
}

export async function listBiomes() {
  const r = await fetch('/api/models', {cache: 'no-store'});
  const j = await r.json();
  const seen = new Set();
  return (j.files || [])
      // A leading underscore is a scratch file (the harnesses write
      // _harness.json); the C++ loader skips those too.
      .filter(f => f.dir === 'biomes' && f.name.endsWith('.json') && f.name[0] !== '_')
      .map(f => f.name.slice(0, -5))
      .filter(n => (seen.has(n) ? false : (seen.add(n), true)))
      .sort((a, b) => {
        const ia = BG.ENGINE_BIOMES.indexOf(a), ib = BG.ENGINE_BIOMES.indexOf(b);
        if (ia >= 0 && ib >= 0) return ia - ib;
        if (ia >= 0) return -1;
        if (ib >= 0) return 1;
        return a < b ? -1 : 1;
      });
}

async function readBiome(name) {
  const r = await fetch('/api/model?path=biomes/' + encodeURIComponent(name + '.json'), {cache: 'no-store'});
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return BG.normalizeBiome(await r.json());
}

/* ===========================================================================
 * the swatch
 * ======================================================================== */
function ensureWorker() {
  if (swatchWorker !== null) return swatchWorker;
  try {
    const w = new Worker(new URL('./swatch_worker.js', import.meta.url), {type: 'module'});
    w.onmessage = (e) => onSwatch(e.data);
    // A module worker that fails to LOAD reports here, not in the constructor;
    // fall back to composing inline and answer the request that was waiting.
    w.onerror = (e) => {
      console.error('biome: swatch worker failed, composing inline', e && e.message);
      if (swatchWorker === w) { swatchWorker = false; swatchBusy = false; }
      const q = swatchQueued; swatchQueued = null;
      if (q) composeInline(q);
    };
    w.postMessage({cmd: 'libs', libs});
    swatchWorker = w;
  } catch (e) {
    console.error('biome: no swatch worker, composing inline', e);
    swatchWorker = false;
  }
  return swatchWorker;
}

function composeInline(req) {
  // Yield a frame so the status paints before a multi-second tree bake.
  setTimeout(() => {
    const t0 = performance.now();
    try {
      const res = BG.generateSwatch(req.biome, libs, req.seed, Object.assign({treeCache}, req.opts));
      const missing = BG.remapToMaterials(res, req.matIds);
      onSwatch({cmd: 'swatch', id: req.id, res, missing, ms: performance.now() - t0});
    } catch (e) {
      onSwatch({cmd: 'error', id: req.id, error: String(e && e.stack || e)});
    }
  }, 0);
}

function requestSwatch() {
  if (!biome) return;
  const req = {
    cmd: 'swatch', id: ++swatchReq, biome, seed,
    matIds: (H.materials() || []).map(m => m.id),
    opts: {vpm: BG.swatchScale(view.sizeM, vpm()), sizeM: view.sizeM, showcase: view.showcase,
           noTrees: !view.trees, noWater: !view.water, noCover: !view.cover}
  };
  els.stats.innerHTML = '<span class="warn">composing…</span>';
  const w = ensureWorker();
  if (!w) { composeInline(req); return; }
  if (swatchBusy) { swatchQueued = req; return; }
  swatchBusy = true;
  w.postMessage(req);
}

function onSwatch(m) {
  if (m.cmd === 'progress') {
    if (m.id === swatchReq) els.stats.innerHTML = '<span class="warn">composing… ' + m.text + '</span>';
    return;
  }
  swatchBusy = false;
  const q = swatchQueued; swatchQueued = null;
  if (q && swatchWorker) { swatchBusy = true; swatchWorker.postMessage(q); }
  if (m.id !== swatchReq) return;            // superseded while it ran
  if (m.cmd === 'error') {
    els.stats.textContent = 'swatch failed: ' + m.error;
    console.error(m.error);
    return;
  }
  const res = m.res;
  lastSwatch = res;
  const full = vpm();
  const lod = full / res.vpm;                // bake cells -> world voxels
  if (wv) {
    wv.setLocalRegions([{cells: res.cells, nx: res.dim.x, ny: res.dim.y, nz: res.dim.z,
                         origin: [0, 0, 0], lod}]);
    if (!framed) {
      UI.frame(wv, {x: res.dim.x * lod, y: res.dim.y * lod, z: res.dim.z * lod}, [0, 0, 0], 0.3);
      wv.cam.pitch = -0.6; framed = true;
    }
  }
  const mt = res.meta;
  const fmt = (o) => Object.entries(o).map(([k, v]) => k + ' ' + v).join(', ') || '—';
  const scale = res.vpm === full ? '1:1' : `1:${(full / res.vpm).toFixed(full % res.vpm ? 1 : 0)}`;
  els.stats.innerHTML =
    `<b>${res.dim.x}×${res.dim.y}×${res.dim.z}</b> cells · ${view.sizeM} m at ${res.vpm} vpm (${scale}, ` +
    `${Math.round(100 / res.vpm)} cm cells) · ${mt.voxels.toLocaleString()} voxels · ${Math.round(m.ms)} ms` +
    (mt.clipped ? ' · <span class="warn">HEIGHT CLIPPED</span>' : '') +
    `<br>trees: ${fmt(mt.trees)}` + (mt.skipped.trees ? ` <span class="warn">(${mt.skipped.trees} gated out)</span>` : '') +
    `<br>water: ${fmt(mt.water)}` + (view.showcase && mt.waterBodies ? ' <span class="warn">(showcase — one of each, not true rarity)</span>' : '') +
    `<br>cover: ${fmt(mt.cover)}` +
    (m.missing.length ? '<br><span class="warn">materials.json has no ' + m.missing.join(', ') + '</span>' : '');
  validateInto(els.valid);
}

// 150 ms and not the 400 it used to be: composing no longer blocks the page,
// and a request that lands mid-compose just waits its turn (requestSwatch).
function regenerate(now) {
  clearTimeout(genTimer);
  if (now) requestSwatch(); else genTimer = setTimeout(requestSwatch, 150);
}

function validateInto(host) {
  if (!host || !biome) return;
  const mats = new Set((H.materials() || []).map(m => m.id));
  const bad = BG.validateBiome(biome, {trees: new Set(Object.keys(libs.trees)),
                                       water: new Set(Object.keys(libs.water)), materials: mats});
  host.innerHTML = '';
  if (!bad.length) { host.append(H.el('span', {class: 'ok'}, '✓ valid — every name resolves')); return; }
  for (const b of bad) host.append(H.el('div', {class: 'warn'}, '⚠ ' + b));
}

/* ===========================================================================
 * widgets
 * ======================================================================== */
function ctx() {
  return {el: H.el, cls: CLS, params: () => biome, widgets,
          snapshot: (k) => undo.snapshot(k),
          live: (path) => LV.lookup('biome', path),
          onChange: () => { markDirty(); regenerate(false); }};
}
function markDirty() {
  dirty = true;
  if (H.onDirty) H.onDirty(true);
  if (els.save) els.save.disabled = false;
}
function refreshWidgets() { for (const w of widgets) { try { w(); } catch (e) {} } }

const COND_FIELDS = [
  {k: 'minY', n: 'min Y', min: -1, max: 400, step: 1, title: 'Lowest ground Y (world voxels) this row tolerates. -1 = no bound.'},
  {k: 'maxY', n: 'max Y', min: -1, max: 400, step: 1, title: 'Highest ground Y. A per-row treeline. -1 = no bound.'},
  {k: 'maxSlope', n: 'slope ≤', min: 0, max: 1024, step: 8, title: 'Q8 landform slope gate: 256 = 45° = the angle of repose. 1024 = no bound.'},
  {k: 'nearWaterMax', n: 'water ≤ m', min: -1, max: 200, step: 0.5, title: 'Only within this many metres of a water body. -1 = anywhere.'},
  {k: 'nearWaterMin', n: 'water ≥ m', min: 0, max: 200, step: 0.5, title: 'Keep at least this far from water.'},
  {k: 'patchThreshold', n: 'patch >', min: 0, max: 255, step: 1, title: 'Only where this row’s patch noise (0..255) is above this. 0 = everywhere.'},
  {k: 'canopyMin', n: 'canopy ≥', min: 0, max: 255, step: 1, title: 'Cover rows: only where the canopy cover (0 = open sky .. 255 = deep under overlapping crowns) is at least this. 96 is under a crown, 40 its edge. 0 = no bound.'},
  {k: 'canopyMax', n: 'canopy ≤', min: 0, max: 255, step: 1, title: 'Cover rows: only where the canopy cover is at most this. 95 keeps a flower out of the shade. 255 = no bound.'}
];

/** The conditions line under a feature row: six small number boxes, each
 *  marked live / not-read by its own path (`base`.conditions.`k`) because the
 *  engine enforces some of a cover row's conditions and none of a tree row's. */
function conditionsLine(item, base) {
  const el = H.el;
  const C = ctx();
  const line = el('div', {class: CLS + 'cond'});
  for (const f of COND_FIELDS) {
    const inp = el('input', {type: 'number', class: CLS + 'num', value: item.conditions[f.k],
                             min: f.min, max: f.max, step: f.step, title: f.title});
    inp.addEventListener('change', () => {
      undo.snapshot(null);
      item.conditions[f.k] = +inp.value;
      markDirty(); regenerate(false);
    });
    const lab = el('label', {title: f.title}, f.n);
    const cell = el('span', {class: CLS + 'condcell'}, lab, inp);
    UI.liveMark(C, cell, base + '.conditions.' + f.k, [inp]);
    line.append(cell);
  }
  return line;
}

/**
 * A reorderable stack of feature rows.
 *   spec = {base: 'cover.plants[]' (the rows' manifest path), items(): array, make(): item,
 *           cols: [{k, type, options(), min, max, step, title, w}],
 *           derived(item) -> string, addLabel, hint}
 */
function stackEditor(body, spec) {
  const el = H.el;
  const C = ctx();
  const host = el('div', {});
  const render = () => {
    host.innerHTML = '';
    const items = spec.items();
    items.forEach((it, i) => {
      const cells = [el('span', {class: CLS + 'ord'}, String(i + 1))];
      for (const c of spec.cols) {
        let inp;
        if (c.type === 'select') {
          inp = el('select', {class: CLS + 'num', title: c.title || ''});
          const opts = c.options();
          opts.forEach(o => inp.append(el('option', {value: o}, o)));
          if (it[c.k] && !opts.includes(it[c.k])) inp.append(el('option', {value: it[c.k]}, it[c.k] + ' (missing)'));
          if (c.none) inp.prepend(el('option', {value: ''}, c.none));
          inp.value = it[c.k] || '';
        } else {
          inp = el('input', {type: 'number', class: CLS + 'num', value: it[c.k], min: c.min, max: c.max,
                             step: c.step, title: c.title || ''});
        }
        if (c.w) inp.style.width = c.w;
        inp.addEventListener('change', () => {
          undo.snapshot(null);
          it[c.k] = c.type === 'select' ? inp.value : +inp.value;
          render(); markDirty(); regenerate(false);
        });
        // Each column is its own field: a cave row's threshold is read and its
        // rarity is not, so the mark is per input, not per row.
        const cell = el('span', {class: CLS + 'stackcell'});
        if (c.label) cell.append(el('span', {class: CLS + 'unit'}, c.label));
        cell.append(inp);
        UI.liveMark(C, cell, spec.base + '.' + c.k, [inp]);
        cells.push(cell);
      }
      const up = el('button', {title: 'move up'}, '▲'), dn = el('button', {title: 'move down'}, '▼');
      const rm = el('button', {title: 'remove'}, '✕');
      up.disabled = i === 0; dn.disabled = i === items.length - 1;
      const swap = (a, b) => { undo.snapshot(null); [items[a], items[b]] = [items[b], items[a]]; render(); markDirty(); regenerate(false); };
      up.addEventListener('click', () => swap(i, i - 1));
      dn.addEventListener('click', () => swap(i, i + 1));
      rm.addEventListener('click', () => { undo.snapshot(null); items.splice(i, 1); render(); markDirty(); regenerate(false); });
      const cond = el('button', {class: CLS + 'condbtn', title: 'placement conditions'}, '⚙');
      const rowEl = el('div', {class: CLS + 'stackrow'}, ...cells, up, dn, rm, cond);
      const wrap = el('div', {class: CLS + 'stackitem'}, rowEl);
      if (spec.derived) wrap.append(el('div', {class: CLS + 'derived'}, spec.derived(it)));
      if (spec.link) {
        const lk = spec.link(it);
        if (lk) wrap.append(lk);
      }
      let condEl = null;
      cond.addEventListener('click', () => {
        if (condEl) { condEl.remove(); condEl = null; cond.classList.remove('on'); return; }
        condEl = conditionsLine(it, spec.base);
        wrap.append(condEl);
        cond.classList.add('on');
      });
      // A row with non-default conditions shows it.
      const d = BG.defaultConditions();
      if (Object.keys(d).some(k => it.conditions[k] !== d[k])) cond.classList.add('set');
      host.append(wrap);
    });
    const add = el('button', {}, spec.addLabel || '+ row');
    add.addEventListener('click', () => { undo.snapshot(null); items.push(spec.make()); render(); markDirty(); regenerate(false); });
    host.append(el('div', {class: CLS + 'bar', style: 'margin-top:4px'}, add,
                   spec.hint ? el('span', {class: CLS + 'hint'}, spec.hint) : null));
  };
  render();
  widgets.push(render);
  body.append(host);
}

/* ===========================================================================
 * the relief (P-G): the height curve and the three multipliers
 *
 * `terrain.curve` is nine Q14 knots on a uniform grid over the coarse
 * relief's full swing: knot i is the ground the biome has where the map's
 * relief sits at input i. The diagonal is the identity (the map's relief,
 * unchanged); below it is flatter, above it steeper, a run of equal knots
 * is a plateau. The engine interpolates monotone-cubic and BLENDS the four
 * map cells around a column, so two biomes' curves crossfade over a whole
 * cell. `hill` / `detail` / `grain` scale the map's three fine octaves (Q8:
 * 256 = the map's amplitude). All of it is read (worldmap.h kB_CurveKnot0..
 * kB_GrainMul); the canvas is the curve, dragged by its knots.
 * ======================================================================== */
const CURVE_W = 260, CURVE_H = 150;
function reliefSection(body) {
  const el = H.el;
  const C = ctx();
  const T = () => biome.terrain;
  const cv = el('canvas', {class: CLS + 'curve', width: CURVE_W, height: CURVE_H,
                           title: 'Drag a knot up (steeper) or down (flatter). The diagonal is the map\u2019s own relief. Double-click: reset to the identity.'});
  const info = el('div', {class: CLS + 'hint'});
  const line = el('div', {class: CLS + 'row'}, el('label', {}, 'height curve'), cv);
  body.append(line, info);
  UI.liveMark(C, line, 'terrain.curve', []);
  const kx = i => 12 + (CURVE_W - 24) * i / 8;
  const ky = v => CURVE_H - 12 - (CURVE_H - 24) * (v + 16384) / 32768;
  const paint = () => {
    const g = cv.getContext('2d');
    g.clearRect(0, 0, CURVE_W, CURVE_H);
    g.fillStyle = '#0e1219'; g.fillRect(0, 0, CURVE_W, CURVE_H);
    g.strokeStyle = '#2a3040'; g.lineWidth = 1;
    g.beginPath(); g.moveTo(kx(0), ky(-16384)); g.lineTo(kx(8), ky(16384)); g.stroke();   // the identity
    g.strokeStyle = '#7fd4ff'; g.lineWidth = 2;
    g.beginPath();
    T().curve.forEach((v, i) => { if (i) g.lineTo(kx(i), ky(v)); else g.moveTo(kx(i), ky(v)); });
    g.stroke();
    g.fillStyle = '#ffd866';
    T().curve.forEach((v, i) => { g.beginPath(); g.arc(kx(i), ky(v), 4, 0, Math.PI * 2); g.fill(); });
    const ident = BG.terrainIsIdentity(T());
    info.textContent = ident ? 'the identity: this biome keeps the map\u2019s relief exactly (the default)'
                             : 'knots (Q14): ' + T().curve.join(', ');
  };
  let drag = -1;
  const pick = (ev) => {
    const r = cv.getBoundingClientRect();
    const x = (ev.clientX - r.left) * CURVE_W / r.width;
    let best = -1, bd = 12;
    for (let i = 0; i < 9; i++) { const d = Math.abs(x - kx(i)); if (d < bd) { bd = d; best = i; } }
    return best;
  };
  const valueAt = (ev) => {
    const r = cv.getBoundingClientRect();
    const y = (ev.clientY - r.top) * CURVE_H / r.height;
    const v = (CURVE_H - 12 - y) / (CURVE_H - 24) * 32768 - 16384;
    return Math.max(-16384, Math.min(16384, Math.round(v / 256) * 256));
  };
  cv.addEventListener('pointerdown', (ev) => {
    if (cv.classList.contains(CLS + 'dead')) return;
    drag = pick(ev);
    if (drag < 0) return;
    cv.setPointerCapture(ev.pointerId);
    undo.snapshot('terrain.curve');
  });
  cv.addEventListener('pointermove', (ev) => {
    if (drag < 0) return;
    T().curve[drag] = valueAt(ev);
    paint(); markDirty();
  });
  const up = () => { if (drag >= 0) { drag = -1; regenerate(false); } };
  cv.addEventListener('pointerup', up);
  cv.addEventListener('pointercancel', up);
  cv.addEventListener('dblclick', () => {
    undo.snapshot(null);
    T().curve = BG.CURVE_IDENTITY.slice();
    paint(); markDirty(); regenerate(false);
  });
  paint();
  widgets.push(paint);
  for (const [k, n, d] of [['hill', 'hills ×', 'Q8 multiplier on the map\u2019s hill octave: 256 = the map\u2019s amplitude, 128 half, 512 double.'],
                           ['detail', 'detail ×', 'Q8 multiplier on the map\u2019s detail octave.'],
                           ['grain', 'grain ×', 'Q8 multiplier on the map\u2019s grain octave.']])
    UI.row(C, body, {k, n, min: 0, max: 1024, step: 8, int: true, u: 'Q8', d}, 'terrain');
}

/* ===========================================================================
 * the panel
 * ======================================================================== */
let builtWithTuning = false;

function buildPanel() {
  const el = H.el;
  const col = els.sliders;
  col.innerHTML = '';
  widgets = [];
  const C = ctx();
  builtWithTuning = !!(H.tuning && H.tuning() && H.tuning().tune);
  const mats = () => H.materials() || [];
  const isSolid = m => m.class === 'solid' || m.class === 'powder';
  const solidNames = () => mats().filter(isSolid).map(m => m.id);
  const engine = BG.ENGINE_BIOMES.includes(biome.name);

  // ---- identity & climate ---------------------------------------------------
  let s = UI.section(el, CLS, 'Identity & climate',
                     engine ? 'An ENGINE biome: id ' + BG.ENGINE_BIOMES.indexOf(biome.name) +
                              ' in the packed record table; the painted map (World map page) says where it is. ' +
                              'Greyed fields are not read by the engine yet — the tooltip names the package that will.'
                            : 'NOT an engine biome yet: its name is not in ENGINE_BIOMES (biomegen.js), so Save ' +
                              'writes index -1 and the loader refuses it. Add the name there and to the map palette.');
  const disp = el('input', {type: 'text', class: CLS + 'num', value: biome.displayName, style: 'text-align:left'});
  disp.addEventListener('change', () => { undo.snapshot(null); biome.displayName = disp.value; markDirty(); });
  widgets.push(() => { disp.value = biome.displayName; });
  const dispRow = el('div', {class: CLS + 'row'}, el('label', {}, 'display name'), disp);
  s.body.append(dispRow);
  UI.liveMark(C, dispRow, 'displayName');
  UI.row(C, s.body, {k: 'temperature', n: 'temperature', min: 0, max: 1, step: 0.01,
                     d: '0 cold .. 1 hot. The planned climate field is seed-independent with a fixed compass: colder toward +Z.'}, 'climate');
  UI.row(C, s.body, {k: 'moisture', n: 'moisture', min: 0, max: 1, step: 0.01,
                     d: '0 arid .. 1 wet. Drier toward +X in the planned field. Also the natural knob for how FULL this biome’s basins are.'}, 'climate');
  const notes = el('textarea', {class: CLS + 'num', rows: '2', style: 'text-align:left;resize:vertical'});
  notes.value = biome.climate.notes || '';
  notes.addEventListener('change', () => { undo.snapshot(null); biome.climate.notes = notes.value; markDirty(); });
  widgets.push(() => { notes.value = biome.climate.notes || ''; });
  const notesRow = el('div', {class: CLS + 'row'}, el('label', {}, 'notes'), notes);
  s.body.append(notesRow);
  UI.liveMark(C, notesRow, 'climate.notes');
  col.append(s.wrap);

  // ---- relief ------------------------------------------------------------------
  s = UI.section(el, CLS, 'Relief',
                 'LIVE (P-G): how this biome reshapes the map\u2019s terrain. The curve remaps the coarse relief ' +
                 '(the painted landform + the range octave) \u2014 the diagonal is the map\u2019s own; below it flatter, ' +
                 'above it steeper, a level run a plateau \u2014 and the three multipliers scale the map\u2019s fine ' +
                 'octaves. The engine blends the four map cells around a column, so neighbours never meet on a cliff. ' +
                 'The per-map numbers (datum, landform range, octaves, home area, sediment, treeline) are on the World map page.');
  reliefSection(s.body);
  col.append(s.wrap);

  // ---- ground cover ---------------------------------------------------------------
  s = UI.section(el, CLS, 'Ground cover',
                 'The skin the ground wears and the small plants on it. LIVE: skin / subsoil / depth, the patch ' +
                 'mask, the three flags, and every plant row (1-in-N surface columns) with all eight conditions. ' +
                 'Rows roll in order, first hit wins. Since P-G there is no hard-coded ground flora: the shade ' +
                 'plants, litter, saplings and flowers are rows with a CANOPY condition (0 open sky .. 255 deep ' +
                 'shade: 96+ is under a crown, ≤95 a gap), and no treeline gate — a row\u2019s max Y is its own ' +
                 'snowline (the shipped rows stop at the map\u2019s treeline, 227; the alpine cushion above it is a row with min Y 228).');
  UI.matRow(C, s.body, {k: 'skin', n: 'ground skin', d: 'Topmost cell (WM_B_SKIN), skinDepth cells deep.'}, 'cover', mats, {filter: isSolid});
  UI.row(C, s.body, {k: 'skinDepth', n: 'skin depth (cells)', min: 1, max: 8, step: 1, int: true, d: ''}, 'cover');
  UI.matRow(C, s.body, {k: 'subsoil', n: 'subsoil', d: 'Under the skin: the sediment wedge’s topsoil (WM_B_SUBSOIL).'}, 'cover', mats, {filter: isSolid});
  UI.row(C, s.body, {k: 'threshold', n: 'patch threshold', min: 0, max: 255, step: 1, int: true,
                     d: 'Plants only grow where the patch noise (0..255) is above this. Higher leaves more open ground between stands — what makes a desert read as arid rather than as a dry lawn.'}, 'cover.patch');
  UI.row(C, s.body, {k: 'cellLog2', n: 'patch size (log2)', min: 2, max: 9, step: 1, int: true,
                     d: 'Patch cell as a power of two, in cells.'}, 'cover.patch');
  UI.boolRow(C, s.body, {k: 'groundFlora', n: 'ground flora blocks',
                         d: 'WM_BF_GROUND_FLORA: the world-wide flower / tall-grass / undergrowth blocks run in this biome (off for desert, ocean, alpine).'}, 'cover');
  UI.boolRow(C, s.body, {k: 'cacti', n: 'cactus block', d: 'WM_BF_CACTI: the cactus site scan runs in this biome.'}, 'cover');
  UI.boolRow(C, s.body, {k: 'sandCap', n: 'sand cap', d: 'WM_BF_SAND_CAP: four cells of sand under the skin, above the subsoil.'}, 'cover');
  UI.row(C, s.body, {k: 'cactusChance', n: 'cactus 1-in-N', min: 0, max: 400, step: 1, int: true,
                     d: 'One in this many cactus tiles grows a cactus; 0 = never. Today the global worldgen.cactusChance (a percent) decides; P-E reads this instead.'}, 'cover');
  UI.row(C, s.body, {k: 'saguaroFraction', n: 'saguaro %', min: 0, max: 100, step: 1, int: true, u: '%',
                     d: 'Percent of those cacti that are tall saguaro columns rather than barrels. Today the global worldgen.saguaroFraction decides; P-E reads this instead.'}, 'cover');
  s.body.append(el('div', {class: CLS + 'hint', style: 'margin-top:6px'}, 'Plants'));
  stackEditor(s.body, {
    base: 'cover.plants[]',
    items: () => biome.cover.plants,
    make: () => BG.defaultRows().coverPlant,
    cols: [
      {k: 'material', type: 'select', options: solidNames, title: 'the plant'},
      {k: 'head', type: 'select', options: solidNames, none: '(no head)', title: 'optional single cell on top'},
      {k: 'chance', label: '1 in', min: 0, max: 999, step: 1, w: '46px', title: '1 in N columns'},
      {k: 'height', label: 'h', min: 0.1, max: 4, step: 0.1, w: '46px', title: 'metres'}
    ],
    derived: (it) => {
      const per = it.chance > 0 ? (100 / it.chance) : 0;
      return it.chance > 0 ? per.toFixed(1) + '% of columns · ~' + Math.round(per / 100 * 10000 * vpm() * vpm()).toLocaleString() + ' per hectare before the patch mask'
                           : 'never';
    },
    addLabel: '+ plant'
  });
  col.append(s.wrap);

  // ---- trees ------------------------------------------------------------------------
  // THE DENSITY STAT USES THIS BIOME'S TILE, and since P-D that is the number
  // the world has: worldgen scans ONE lattice (the finest tile among the
  // biomes that grow trees) and thins each biome on it to density × (T/tile)²
  // (kB_TreeChanceQ16), so trees/ha is densityStats(tile, density) by
  // construction. There is no worldgen.treeTile any more.
  const treeTileM = biome.trees.tile;
  const tileNote = 'at this biome’s tile (live: one shared lattice, thinned to this spacing)';
  s = UI.section(el, CLS, 'Trees',
                 'LIVE: tile and density (folded into one thinning chance on the world’s finest tile, ' +
                 'WM_B_TREE_CHANCE_Q16), the species weights (the atlas biome table is built from these rows ' +
                 'at load; "Sync atlas" refreshes the Trees page’s read-only mirror), and every per-row ' +
                 'condition (packed per biome × species into the atlas and compared in treeInfoAt on top of ' +
                 'the species file’s own band and slope).');
  UI.row(C, s.body, {k: 'tile', n: 'tile (m)', min: 1.6, max: 51.2, step: 0.8, u: 'm',
                     d: 'Metres between trunk sites. The world scans the FINEST tile among tree-growing biomes and thins this biome on it to the same trees per hectare; the finest tile also sets the per-column scan cost, so keep it as coarse as the forest allows.'}, 'trees');
  UI.row(C, s.body, {k: 'density', n: 'density (%)', min: 0, max: 100, step: 1, int: true, u: '%',
                     d: 'Percent of tiles that grow a tree.'}, 'trees');
  const dstat = el('div', {class: CLS + 'derived'});
  const paintD = () => {
    const d = BG.densityStats(treeTileM, biome.trees.density);
    dstat.textContent = '≈ ' + d.perHa.toFixed(0) + ' trees per hectare ' + tileNote +
                        ' · 1 tree in ' + (d.oneIn ? d.oneIn.toFixed(1) : '∞') + ' tiles';
  };
  paintD(); widgets.push(paintD);
  s.body.append(dstat);
  const total = () => biome.trees.species.reduce((a, r) => a + (r.weight | 0), 0);
  stackEditor(s.body, {
    base: 'trees.species[]',
    items: () => biome.trees.species,
    make: () => Object.assign(BG.defaultRows().treeSpecies, {species: Object.keys(libs.trees)[0] || 'oak'}),
    cols: [
      {k: 'species', type: 'select', options: () => Object.keys(libs.trees).sort(), title: 'a file in assets/trees/'},
      {k: 'weight', label: 'weight', min: 0, max: 100, step: 1, w: '52px', title: 'Relative weight against the other rows. 0 = never here.'}
    ],
    derived: (it) => {
      const t = total();
      const sp = libs.trees[it.species];
      const spar = sp && sp.placement && sp.placement.sparsity > 1 ? ' ÷ sparsity ' + sp.placement.sparsity : '';
      const share = t ? (100 * it.weight / t).toFixed(0) : 0;
      const d = BG.densityStats(treeTileM, biome.trees.density);
      return share + '% of trees here' + spar + ' · ≈ ' + (d.perHa * (t ? it.weight / t : 0)).toFixed(1) + ' per hectare' +
             (sp && sp.placement && (sp.placement.maxY >= 0 || sp.placement.minY >= 0)
                ? ' · species band ' + sp.placement.minY + '..' + sp.placement.maxY + ' m' : '');
    },
    link: (it) => {
      const a = el('a', {href: '#', class: CLS + 'link'}, 'edit species →');
      a.addEventListener('click', (e) => { e.preventDefault(); if (H.openPage) H.openPage('trees', it.species); });
      return a;
    },
    addLabel: '+ species',
    hint: 'A species listed nowhere is never planted; listing it here dilutes the others.'
  });
  els.sync = el('div', {class: CLS + 'derived'});
  s.body.append(els.sync);
  col.append(s.wrap);

  // ---- water ----------------------------------------------------------------------------
  s = UI.section(el, CLS, 'Water bodies',
                 'Which presets from assets/water/ appear here, one per TILE at most, one tile in N. ' +
                 'NOT READ (P-F): no water row is packed; worldgen grows one parabolic tarn per pond tile ' +
                 'from worldgen.pond* (the Water bodies page shows those knobs). The swatch composes these.');
  stackEditor(s.body, {
    base: 'water.features[]',
    items: () => biome.water.features,
    make: () => {
      const nm = Object.keys(libs.water)[0] || 'tarn';
      const p = libs.water[nm] && libs.water[nm].placement || {};
      return Object.assign(BG.defaultRows().waterFeature,
                           {preset: nm, tile: p.tile || 44.8, rarity: p.rarity || 4,
                            conditions: Object.assign(BG.defaultConditions(), {maxSlope: p.maxSlope || 96})});
    },
    cols: [
      {k: 'preset', type: 'select', options: () => Object.keys(libs.water).sort(), title: 'a file in assets/water/'},
      {k: 'tile', label: 'tile m', min: 4, max: 400, step: 0.8, w: '56px', title: 'metres between candidate sites'},
      {k: 'rarity', label: '1 in', min: 0, max: 64, step: 1, w: '44px', title: 'one tile in N gets a body; 0 = never'}
    ],
    derived: (it) => {
      const r = BG.rarityStats(it.tile, it.rarity);
      const P = libs.water[it.preset];
      const rad = P && P.footprint ? P.footprint.radius : 0;
      return it.rarity ? r.pct.toFixed(0) + '% of tiles · ≈ ' + r.perKm2.toFixed(1) + ' per km² · ' +
                         (r.perHa >= 0.01 ? r.perHa.toFixed(2) + ' per hectare' : 'under 0.01 per hectare') +
                         (rad ? ' · r ≈ ' + rad + ' m' : '')
                       : 'never';
    },
    link: (it) => {
      const a = el('a', {href: '#', class: CLS + 'link'}, 'edit preset →');
      a.addEventListener('click', (e) => { e.preventDefault(); if (H.openPage) H.openPage('water', it.preset); });
      return a;
    },
    addLabel: '+ water body',
    hint: 'Tile and rarity default from the preset’s placement block; the values here are this biome’s.'
  });
  col.append(s.wrap);

  // ---- caves ----------------------------------------------------------------------------
  s = UI.section(el, CLS, 'Caves',
                 'LIVE: a near_surface row’s threshold is this biome’s WM_B_CAVE_T1 and a deep row’s is ' +
                 'WM_B_CAVE_T2 (a biome with no row keeps the global worldgen.caveThreshold1/2). NOT READ: ' +
                 'rarity (no package yet) and the mushroom / crystal chances — worldgen.caveMushroomChance / ' +
                 'caveCrystalChance are global until P-E. The swatch does not cut caves (it shows the surface).', {closed: true});
  stackEditor(s.body, {
    base: 'caves.features[]',
    items: () => biome.caves.features,
    make: () => BG.defaultRows().caveFeature,
    cols: [
      {k: 'preset', type: 'select', options: () => ['near_surface', 'deep'], title: 'which band'},
      {k: 'threshold', label: 'noise >', min: 0, max: 255, step: 1, w: '50px', title: 'cave noise threshold (0..255); higher = fewer caves'},
      {k: 'rarity', label: '1 in', min: 0, max: 16, step: 1, w: '44px', title: 'one region in N has this band at all'},
      {k: 'mushroomChance', label: 'mushroom 1 in', min: 0, max: 400, step: 1, w: '44px', title: 'one in N floor columns of this band grows a mushroom; 0 = never'},
      {k: 'crystalChance', label: 'crystal 1 in', min: 0, max: 400, step: 1, w: '44px', title: 'one in N floor/ceiling columns of this band grows a crystal; 0 = never'}
    ],
    addLabel: '+ cave band'
  });
  col.append(s.wrap);

  // ---- swatch settings -----------------------------------------------------------------------
  s = UI.section(el, CLS, 'Swatch', 'Preview-only: the ground the swatch stands on.', {closed: true});
  UI.row(C, s.body, {k: 'reliefM', n: 'relief (m)', min: 0, max: 8, step: 0.1, u: 'm', d: 'Ground noise amplitude in the swatch. Not the engine’s terrain.'}, 'swatch');
  UI.row(C, s.body, {k: 'reliefFreq', n: 'relief freq', min: 0.2, max: 4, step: 0.1, d: ''}, 'swatch');
  col.append(s.wrap);

  paintSync();
}

/* ===========================================================================
 * atlas sync — biome weights -> species files -> re-bake
 * ======================================================================== */
async function allBiomes() {
  const names = await listBiomes();
  const out = [];
  for (const n of names) out.push(n === biomeName && biome ? biome : await readBiome(n));
  return out;
}

async function paintSync() {
  if (!els.sync) return;
  try {
    const biomes = await allBiomes();
    const stale = Object.keys(libs.trees).filter(sp =>
      !BG.speciesWeightsMatch(biomes, sp, libs.trees[sp].placement && libs.trees[sp].placement.biomes));
    els.sync.innerHTML = '';
    if (!stale.length) {
      els.sync.append(H.el('span', {class: 'ok'}, '✓ tree atlas in sync with the biome files'));
    } else {
      els.sync.append(H.el('span', {class: 'warn'}, '⚠ ' + stale.length + ' species file(s) carry stale weights: ' +
                                                    stale.join(', ') + ' — save, then Sync atlas'));
    }
  } catch (e) { els.sync.textContent = 'sync state unknown: ' + (e && e.message || e); }
}

async function syncAtlas() {
  els.syncBtn.disabled = true;
  els.syncBtn.textContent = 'syncing…';
  await new Promise(r => setTimeout(r, 0));
  try {
    const biomes = await allBiomes();
    let wrote = 0, baked = 0;
    for (const sp of Object.keys(libs.trees)) {
      const j = libs.trees[sp];
      if (BG.speciesWeightsMatch(biomes, sp, j.placement && j.placement.biomes)) continue;
      j.placement = j.placement || {};
      j.placement.biomes = BG.speciesWeightsFrom(biomes, sp);
      let r = await fetch('/api/model?path=trees/' + encodeURIComponent(sp + '.json'),
                          {method: 'POST', body: JSON.stringify(j, null, 2) + '\n'});
      if (!(await r.json()).ok) throw new Error('could not write trees/' + sp + '.json');
      wrote++;
      const out = TG.bakeAtlas(j, null, {vpm: vpm()});
      r = await fetch('/api/model?path=trees/' + encodeURIComponent(sp + '.svtree'), {method: 'POST', body: out.buf});
      if (!(await r.json()).ok) throw new Error('could not write trees/' + sp + '.svtree');
      baked++;
    }
    H.toast(wrote ? 'synced ' + wrote + ' species file(s), re-baked ' + baked + ' atlas(es). The world hash MOVES: ' +
                    'run --selftest --rebaseline.'
                  : 'tree atlas already in sync');
    if (H.onLibraryChanged) H.onLibraryChanged('trees');
  } catch (e) {
    H.toast('sync failed: ' + (e && e.message || e), true);
  }
  els.syncBtn.disabled = false;
  els.syncBtn.textContent = 'Sync atlas';
  paintSync();
}

/* ===========================================================================
 * files
 * ======================================================================== */
function setBiome(b, name) {
  biome = b;
  biomeName = name;
  undo.clear();
  dirty = false;
  if (H.onDirty) H.onDirty(false);
  if (els.save) els.save.disabled = true;
  framed = false;
  els.title.textContent = b.displayName + (name ? '  ·  biomes/' + name + '.json' : '  ·  (unsaved)');
  buildPanel();
  regenerate(true);
}

export async function open(name) {
  if (!Object.keys(libs.trees).length) await loadLibs();
  setBiome(await readBiome(name), name);
}

export async function newBiome(fromName) {
  if (!Object.keys(libs.trees).length) await loadLibs();
  const b = fromName ? await readBiome(fromName) : BG.normalizeBiome({});
  b.name = 'new_biome'; b.displayName = 'New biome'; b.index = -1;
  setBiome(b, '');
  markDirty();
}

async function saveBiome(asName) {
  const name = (asName || biomeName || '').trim();
  if (!name) { const n = prompt('Biome name (a file under assets/biomes/, [a-z0-9_]):', biome.name); if (!n) return; return saveBiome(n); }
  const clean = name.replace(/[^a-zA-Z0-9_]/g, '_').toLowerCase();
  biome.name = clean;
  biome.index = BG.ENGINE_BIOMES.indexOf(clean);
  const body = JSON.stringify(biome, null, 2) + '\n';
  const r = await fetch('/api/model?path=biomes/' + encodeURIComponent(clean + '.json'), {method: 'POST', body});
  const j = await r.json();
  if (!j.ok) { H.toast('save failed: ' + (j.error || '?'), true); return; }
  biomeName = clean;
  dirty = false;
  if (H.onDirty) H.onDirty(false);
  els.save.disabled = true;
  els.title.textContent = biome.displayName + '  ·  biomes/' + clean + '.json';
  H.toast('saved biomes/' + clean + '.json — press Apply to game (or F7 in the game) to see it');
  if (H.onLibraryChanged) H.onLibraryChanged('biomes');
  paintSync();
}

/* ===========================================================================
 * mount
 * ======================================================================== */
const CSS = UI.pageCss(PAGE, CLS, `
#${PAGE} .${CLS}title{font-weight:600;color:#dbe4f0;font-size:13px}
#${PAGE} .${CLS}band{position:relative;display:flex;height:22px;border-radius:4px;overflow:visible;margin:4px 0 6px;user-select:none}
#${PAGE} .${CLS}seg{height:100%;font:10px monospace;color:#0e1116;display:flex;align-items:center;justify-content:center;overflow:hidden;cursor:pointer;opacity:.85}
#${PAGE} .${CLS}seg.cur{opacity:1;outline:2px solid #fff;outline-offset:-2px}
#${PAGE} .${CLS}grip{position:absolute;top:-3px;width:6px;height:28px;margin-left:-3px;background:#fff;border-radius:2px;cursor:ew-resize;box-shadow:0 0 0 1px #000}
#${PAGE} .${CLS}stackitem{border-left:2px solid #2a3040;padding:2px 0 2px 6px;margin:3px 0}
#${PAGE} .${CLS}stackrow{display:flex;gap:4px;align-items:center;flex-wrap:wrap;font-size:11px}
#${PAGE} .${CLS}stackcell{display:inline-flex;gap:3px;align-items:center;flex:0 1 auto}
#${PAGE} .${CLS}stackcell:has(select){flex:1 1 90px}
#${PAGE} .${CLS}stackrow select{flex:1 1 90px;min-width:70px}
#${PAGE} .${CLS}stackrow button{padding:0 4px;font-size:10px;line-height:18px}
#${PAGE} .${CLS}condbtn.set{color:#ffb454}
#${PAGE} .${CLS}cond{display:grid;grid-template-columns:repeat(3,1fr);gap:2px 6px;align-items:center;margin:3px 0 2px;font-size:10px}
#${PAGE} .${CLS}condcell{display:flex;gap:3px;align-items:center;min-width:0}
#${PAGE} .${CLS}cond label{color:#7c8ba3;white-space:nowrap}
#${PAGE} .${CLS}cond input{width:100%;min-width:38px}
#${PAGE} .${CLS}ord{color:#6f7f97;font:10px monospace;width:14px}
#${PAGE} .${CLS}derived{font:10px/1.4 monospace;color:#7c8ba3;margin:1px 0 0 18px}
#${PAGE} .${CLS}link{font-size:10px;color:#5aa9e6;margin-left:18px}
#${PAGE} .${CLS}unit.over{color:#ffb454}
#${PAGE} .${CLS}valid{font:11px/1.4 monospace}
#${PAGE} .${CLS}valid .ok{color:#7fd48a}
#${PAGE} .${CLS}valid .warn{color:#ffb454}
`);

export function attach(hooks) {
  H = hooks;
  const el = H.el;
  const root = H.section;
  if (!root) return;
  root.id = PAGE;
  const style = document.createElement('style');
  style.textContent = CSS;
  document.head.append(style);

  undo = UI.makeUndo({
    get: () => biome,
    set: (o) => { biome = BG.normalizeBiome(o); refreshWidgets(); markDirty(); regenerate(false); },
    toast: H.toast
  });

  els.title = el('div', {class: CLS + 'title'}, '—');
  els.save = el('button', {disabled: true}, 'Save');
  els.saveAs = el('button', {}, 'Save as…');
  els.syncBtn = el('button', {title: 'Write every biome’s tree weights into the species files and re-bake the changed atlases'}, 'Sync atlas');
  els.undo = el('button', {title: 'Undo (Ctrl+Z)'}, '↶');
  els.redo = el('button', {title: 'Redo (Ctrl+Shift+Z)'}, '↷');
  els.sliders = el('div', {class: CLS + 'sliders'});
  els.stats = el('div', {class: CLS + 'stats'}, 'loading…');
  els.valid = el('div', {class: CLS + 'valid'});
  els.seed = el('input', {type: 'number', class: CLS + 'num', value: '1', style: 'width:56px', min: '0', max: '9999'});
  els.size = el('select', {class: CLS + 'num', style: 'width:110px',
                            title: 'Swatch side. Up to 32 m the swatch is 1:1 with the engine; bigger swatches bake ' +
                                   'coarser (the cell size shown) so a whole biome fits — see biomegen.swatchScale.'});
  BG.SWATCH_SIZES_M.forEach(m => {
    const v = BG.swatchScale(m, vpm());
    els.size.append(el('option', {value: m}, m + ' m · ' + (v === vpm() ? '1:1' : Math.round(100 / v) + ' cm')));
  });
  els.size.value = String(view.sizeM);
  const tog = (key, label, title) => {
    const b = el('button', {class: view[key] ? 'on' : '', title}, label);
    b.addEventListener('click', () => { view[key] = !view[key]; b.classList.toggle('on', view[key]); regenerate(true); });
    return b;
  };
  const cv = el('canvas', {class: CLS + 'view'});

  els.save.addEventListener('click', () => saveBiome());
  els.saveAs.addEventListener('click', () => {
    const n = prompt('Biome name (a file under assets/biomes/, [a-z0-9_]):', biomeName || biome.name);
    if (n) saveBiome(n);
  });
  els.syncBtn.addEventListener('click', syncAtlas);
  els.undo.addEventListener('click', () => undo.undo());
  els.redo.addEventListener('click', () => undo.redo());
  els.seed.addEventListener('change', () => { seed = Math.max(0, parseInt(els.seed.value, 10) || 0); regenerate(true); });
  els.size.addEventListener('change', () => { view.sizeM = +els.size.value; framed = false; regenerate(true); });

  document.addEventListener('keydown', (e) => {
    if (!root.classList.contains('active') || !H.isVisible()) return;
    if (!(e.ctrlKey || e.metaKey)) return;
    const k = (e.key || '').toLowerCase();
    if (k === 'z' && !e.shiftKey) { e.preventDefault(); e.stopImmediatePropagation(); undo.undo(); }
    else if ((k === 'z' && e.shiftKey) || k === 'y') { e.preventDefault(); e.stopImmediatePropagation(); undo.redo(); }
  }, true);

  root.append(
    el('div', {class: CLS + 'left'},
      els.title,
      el('div', {class: CLS + 'bar'}, els.save, els.saveAs, els.syncBtn, els.undo, els.redo),
      els.valid,
      els.sliders),
    el('div', {class: CLS + 'right'},
      el('div', {class: CLS + 'bar'},
         el('span', {class: 'hint'}, 'swatch'), els.size,
         el('span', {class: 'hint'}, 'seed'), els.seed,
         tog('trees', 'trees', 'compose the tree stack'),
         tog('water', 'water', 'compose the water stack'),
         tog('cover', 'cover', 'compose the ground cover'),
         tog('showcase', 'showcase water', 'ON: one of every water row, centred — judge the shoreline. OFF: true tile + rarity — on a 24 m swatch usually nothing, on a 96–128 m swatch the real picture.')),
      cv, els.stats));

  wv = UI.makeView(cv, H.materials() || [], () => root.classList.contains('active') && H.isVisible(),
                   (e) => { els.stats.textContent = 'the 3D preview needs WebGL2: ' + (e && e.message || e); });
}

export function activate() {
  if (!biome) {
    loadLibs().then(listBiomes).then(names => {
      const pick = names.includes('forest') ? 'forest' : names[0];
      if (!pick) {
        els.stats.textContent = 'no biomes in assets/biomes/ — run `node scripts/seed_environment.mjs --seed`';
        return newBiome();
      }
      return open(pick);
    }).catch(e => { els.stats.textContent = 'could not load: ' + (e && e.message || e); });
  }
}

/** The host tells us a library changed (a preset or species was saved). */
export async function librariesChanged() {
  await loadLibs();
  if (biome) { refreshWidgets(); regenerate(false); paintSync(); }
}

export function saveFromHost() { if (biome) saveBiome(); }
export function isDirty() { return dirty; }
export function currentName() { return biomeName; }
/** True while the page holds a biome that has never been saved to a file. */
export function hasUnsaved() { return !!biome && !biomeName; }

/** tuning.json arrives asynchronously in the tuner; a panel built before it
 *  landed has no band strip and no terrain rows. The host calls this on every
 *  tab activation and the panel rebuilds once, when tuning first appears. */
export function tuningAvailable() {
  // Nothing on the panel needs tuning.json any more (P-G moved the terrain
  // to the map and the biome files); kept for the host's call.
}

// test seams
export function _view() { return wv; }
export function _biome() { return biome; }
export function _undoDepth() { return undo ? undo.depth() : 0; }
export function _swatch() { return lastSwatch; }
