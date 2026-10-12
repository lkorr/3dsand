/* ui_core.js — the plumbing every page of the Dungeons tab shares
 * (docs/PLAN_dungeons.md §5, package D4). UI ONLY: the pure modules
 * (dgutil.js, kit_*.js, dungeongen.js) stay DOM-free; everything here is the
 * browser half.
 *
 *   h()          element builder (no globals from tuner.html)
 *   api          the server routes the tab uses (scripts/tuner_server.py)
 *   Previewer    one scratch program in the voxel server at a time:
 *                POST /api/dungeon/preview, serialised, latest wins, and the
 *                DIFF against what was installed before, so a slider moves
 *                only refetch the regions the change touches
 *   History      whole-state undo / redo with drag coalescing
 *   sliderPanel  a housegen-style SCHEMA -> sections of sliders / selects
 *   View3D       a WorldView (assets/worldview.js) hosted for a dungeon: the
 *                local (anchor) frame -> world transform of the preview, the
 *                x-ray clip box, the ROI, overlay lines + 2D labels + pick
 *                markers, the x-ray voxel pick and screen projection
 *
 * FRAMES. Everything the pages hold is in the ANCHOR frame (the compiled
 * program's frame: the entrance socket at 0,0,0; refs.json's frame).
 * dungeon.json pieces / props are in the DUNGEON frame = anchor + the
 * entrance's dungeon position (program.entrance.dungeonPos). The preview's
 * info.json says where anchor 0,0,0 landed in the world (`origin`) and the
 * placement's quarter turn (`yaw`): 3.4.1's rule, cells rotate about the
 * anchor CELL, so a continuous point p maps by rotating (p - 0.5) and adding
 * 0.5 back.
 */
'use strict';
import * as DG from './dungeongen.js';

export const C = 'dg';      // the class prefix every element of the tab uses

/* ---- elements ----------------------------------------------------------------- */
export function h(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k === 'class') e.className = v;
    else if (k === 'style') e.style.cssText = v;
    else if (k.startsWith('on') && typeof v === 'function') e.addEventListener(k.slice(2), v);
    else if (k === 'value' || k === 'checked' || k === 'disabled' || k === 'selected') e[k] = v;
    else e.setAttribute(k, v === true ? '' : String(v));
  }
  for (const k of kids.flat(Infinity)) {
    if (k === null || k === undefined || k === false) continue;
    e.append(k instanceof Node ? k : document.createTextNode(String(k)));
  }
  return e;
}
export function toast(msg, bad) {
  if (typeof window !== 'undefined' && typeof window.toast === 'function') window.toast(msg, !!bad);
  else console[bad ? 'warn' : 'log'](msg);
}
export const fmtM = (v) => (v / 10).toFixed(1) + ' m';
export const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

/* ---- the server ---------------------------------------------------------------- */
async function jfetch(url, opts) {
  let r;
  try { r = await fetch(url, Object.assign({cache: 'no-store'}, opts || {})); }
  catch (e) { throw new Error('the tuner server is not reachable (' + e.message + ')'); }
  let j = null;
  try { j = await r.json(); } catch (e) { j = null; }
  if (!r.ok || !j || j.ok === false) {
    const e = new Error((j && j.error) || ('HTTP ' + r.status + ' for ' + url.split('?')[0]));
    // `busy`: the voxel server could not get the machine-wide GPU lock in
    // time (tuner_server.py LockBusy). A queue, not a failure: callers retry.
    e.busy = !!(j && j.busy);
    throw e;
  }
  return j;
}
const post = (url, body) => jfetch(url, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)});
let palP = null, catP = null;
export const api = {
  list: () => jfetch('/api/dungeons'),
  get: (id) => jfetch('/api/dungeon?id=' + encodeURIComponent(id)),
  save: (id, body) => post('/api/dungeon?id=' + encodeURIComponent(id), body),
  preview: (body) => post('/api/dungeon/preview', body),
  preset: (body) => post('/api/dungeon/preset', body),
  play: (body) => post('/api/play', body),
  maps: () => jfetch('/api/worldmaps'),
  map: (name) => jfetch('/api/worldmap?name=' + encodeURIComponent(name)),
  catalog(force) { if (!catP || force) catP = jfetch('/api/dungeon/catalog').catch(e => { catP = null; throw e; }); return catP; },
  palette() { if (!palP) palP = jfetch('/api/voxpalette').catch(e => { palP = null; throw e; }); return palP; },
  async schedule(name) {
    const r = await fetch('/api/model?path=schedules/' + encodeURIComponent(name) + '.json', {cache: 'no-store'});
    if (!r.ok) throw new Error('no schedule ' + name);
    return r.text();
  },
  async saveSchedule(name, text) {
    const r = await fetch('/api/model?path=schedules/' + encodeURIComponent(name) + '.json', {method: 'POST', body: text});
    const j = await r.json().catch(() => ({ok: false, error: 'HTTP ' + r.status}));
    if (!j.ok) throw new Error(j.error || 'schedule save failed');
    return j;
  },
  /** The engine's ground (+ water depth) over a world square: /api/heightmap. */
  async heights(cx, cz, span, res, map) {
    const r = await fetch('/api/heightmap?cx=' + Math.round(cx) + '&cz=' + Math.round(cz) + '&span=' + Math.round(span) + '&res=' + res +
                          (map ? '&map=' + encodeURIComponent(map) : ''), {cache: 'no-store'});
    if (!r.ok) { let m = 'HTTP ' + r.status; try { m = (await r.json()).error || m; } catch (e) {} throw new Error(m); }
    const buf = await r.arrayBuffer(), d = new DataView(buf);
    if (d.getUint32(0, true) !== 0x4D485653) throw new Error('not a heightmap');
    const n = d.getUint32(8, true);
    const o = {res: n, span: d.getUint32(12, true), cx: d.getInt32(16, true), cz: d.getInt32(20, true), h: new Int32Array(n * n), water: new Int16Array(n * n)};
    for (let i = 0; i < n * n; i++) { const b = 48 + i * 8; o.h[i] = d.getInt32(b, true); o.water[i] = d.getInt16(b + 4, true); }
    o.at = (x, z) => {
      const i = Math.floor((x - (o.cx - o.span / 2)) / o.span * o.res), j = Math.floor((z - (o.cz - o.span / 2)) / o.span * o.res);
      if (i < 0 || j < 0 || i >= o.res || j >= o.res) return null;
      return {ground: o.h[j * o.res + i], water: o.water[j * o.res + i] === -32768 ? null : o.h[j * o.res + i] + o.water[j * o.res + i]};
    };
    return o;
  }
};

/* ---- the material list (validation's unknown-material check) ------------------- */
let matNamesP = null;
export function materialNames() {
  if (!matNamesP) matNamesP = fetch('/api/files', {cache: 'no-store'}).then(r => r.json()).then(j => {
    const m = j && (j.materials || (j.files && j.files.materials));
    const t = typeof m === 'string' ? JSON.parse(m) : m;
    return t && t.materials ? t.materials.map(x => x.id) : null;
  }).catch(() => null);
  return matNamesP;
}

/* ---- frames --------------------------------------------------------------------- */
/** 3.4.1 R(q) on (x, z): q1 (z, -x), q2 (-x, -z), q3 (-z, x). */
export function rotQ(q, x, z) {
  switch (q & 3) { case 0: return [x, z]; case 1: return [z, -x]; case 2: return [-x, -z]; default: return [-z, x]; }
}
/** The anchor frame of a preview: info = the engine's placement json. */
export function makeFrame(origin, yaw) {
  const o = origin.slice(), q = ((Math.round((yaw || 0) / 90) % 4) + 4) % 4;
  return {
    origin: o, q,
    /** continuous local point -> world point */
    pt(p) { const r = rotQ(q, p[0] - 0.5, p[2] - 0.5); return [o[0] + r[0] + 0.5, o[1] + p[1], o[2] + r[1] + 0.5]; },
    /** local cell -> world cell */
    cell(c) { const r = rotQ(q, c[0], c[2]); return [o[0] + r[0], o[1] + c[1], o[2] + r[1]]; },
    /** world point -> continuous local point */
    inv(w) { const r = rotQ((4 - q) & 3, w[0] - o[0] - 0.5, w[2] - o[2] - 0.5); return [r[0] + 0.5, w[1] - o[1], r[1] + 0.5]; },
    invCell(w) { const r = rotQ((4 - q) & 3, w[0] - o[0], w[2] - o[2]); return [r[0], w[1] - o[1], r[1]]; },
    /** a local inclusive cell box -> the world box (min, max+1 continuous) */
    box(min, max) {
      const a = this.pt(min), b = this.pt([max[0] + 1, max[1] + 1, max[2] + 1]);
      return {min: [0, 1, 2].map(i => Math.min(a[i], b[i])), max: [0, 1, 2].map(i => Math.max(a[i], b[i]))};
    }
  };
}

/* ---- the previewer ---------------------------------------------------------------- */
// The voxel server holds ONE scratch program. Each page has a Previewer; the
// one that installed last owns the server (OWNER), and another page coming
// back re-installs its own (a cache hit: same program, same region keys).
let OWNER = null;
export class Previewer {
  constructor(name) {
    this.name = name; this.busy = false; this.want = null; this.installed = null; this.info = null; this.key = '';
    this.retryW = null; this.retryT = 0; this.tries = 0;
    // One line for the view's status bar: '' when nothing is going on.
    this.status = '';
  }
  /** program: a compiled shell; place: {mode:'isolate'} | {mode:'place', at, yOffset, yaw, map?} | {mode:'map', id, map?}.
   *  -> {info, boxes: 'all' | [{min,max} local], program} once installed (the latest request wins). */
  install(program, place) {
    const key = JSON.stringify(place);
    return new Promise((resolve, reject) => {
      if (this.want) this.want.resolve(null);       // superseded
      if (this.retryW) { this.retryW.resolve(null); this.retryW = null; clearTimeout(this.retryT); }
      this.want = {program, place, key, resolve, reject};
      this._pump();
    });
  }
  /** Nothing queued, nothing in flight, nothing waiting to retry. */
  idle() { return !this.busy && !this.want && !this.retryW; }
  /** Is `program` THE program the voxel server holds for this page? (the
   *  readiness test: regions streamed before an install finished show the
   *  previous program, which is how a "ready" could pass too early). */
  holds(program) { return this.idle() && OWNER === this && this.installed === program; }
  async _pump() {
    if (this.busy || !this.want) return;
    const w = this.want;
    this.want = null;
    this.busy = true;
    this.status = 'installing the program in the voxel server' + (this.tries ? ' (try ' + (this.tries + 1) + ')' : '');
    try {
      const body = Object.assign({seed: 1337, shell: w.program}, w.place);
      const info = await api.preview(body);
      const prev = OWNER === this && this.key === w.key ? this.installed : null;
      const boxes = prev ? diffBoxes(prev, w.program) : 'all';
      this.installed = w.program; this.info = info; this.key = w.key;
      OWNER = this;
      this.tries = 0;
      this.status = '';
      w.resolve({info, boxes, program: w.program});
    } catch (e) {
      if (e.busy && !this.want) {
        // THE GPU LOCK, NOT THE PROGRAM: another sandvox run (a gate, the
        // game, a link) holds it past the server's patience. Wait and post the
        // same program again rather than failing the preview -- unless a newer
        // one arrives meanwhile, which supersedes this one (install()).
        this.tries++;
        this.status = 'waiting for the machine-wide GPU lock to install the program: ' +
          e.message.replace(/^busy: /, '') + ' · retry ' + this.tries;
        this.retryW = w;
        this.retryT = setTimeout(() => {
          if (this.retryW !== w) return;
          this.retryW = null;
          if (!this.want) this.want = w; else w.resolve(null);
          this._pump();
        }, 2000);
      } else {
        this.tries = 0;
        this.status = '';
        w.reject(e);
      }
    } finally {
      this.busy = false;
      if (this.want) this._pump();
    }
  }
  /** Does the server hold someone else's program (another page was here)? */
  stale() { return OWNER !== this; }
  forget() { if (OWNER === this) OWNER = null; this.installed = null; this.key = ''; }
}

/** Local boxes the voxels of `a` and `b` can differ in, or 'all'. Prims and
 *  instances are compared by content: every 64^3 cell outside the boxes of
 *  the ones that differ evaluates the same prims to the same cells. */
export function diffBoxes(a, b) {
  if (!a || !b) return 'all';
  if (JSON.stringify(a.materials) !== JSON.stringify(b.materials) || a.naturalCaves !== b.naturalCaves || a.keepOut !== b.keepOut) return 'all';
  const strip = (p) => { const o = Object.assign({}, p); delete o.piece; delete o.tag; return JSON.stringify(o); };
  const count = (list, f) => { const m = new Map(); for (const x of list) { const k = f(x); m.set(k, (m.get(k) || 0) + 1); } return m; };
  const out = [];
  const diffList = (la, lb, f, bounds) => {
    const ca = count(la, f), cb = count(lb, f);
    for (const x of la) { const k = f(x); if ((cb.get(k) || 0) < ca.get(k)) out.push(bounds(x)); }
    for (const x of lb) { const k = f(x); if ((ca.get(k) || 0) < cb.get(k)) out.push(bounds(x)); }
  };
  diffList(a.prims, b.prims, strip, DG.shellPrimBounds);
  const inst = (prog) => (i) => {
    const t = prog.props.templates[i.template];
    return JSON.stringify([t.size, t.palette, t.voxels.length, t.id, i.pos, i.yaw]);
  };
  const ib = (prog) => (i) => {
    const t = prog.props.templates[i.template], q = ((i.yaw / 90) | 0) & 1;
    const sx = q ? t.size[2] : t.size[0], sz = q ? t.size[0] : t.size[2];
    return {min: i.pos.slice(), max: [i.pos[0] + sx - 1, i.pos[1] + t.size[1] - 1, i.pos[2] + sz - 1]};
  };
  const fa = inst(a), fb = inst(b), ba = ib(a), bb = ib(b);
  const ca = count(a.props.instances, fa), cb = count(b.props.instances, fb);
  for (const x of a.props.instances) { const k = fa(x); if ((cb.get(k) || 0) < ca.get(k)) out.push(ba(x)); }
  for (const x of b.props.instances) { const k = fb(x); if ((ca.get(k) || 0) < cb.get(k)) out.push(bb(x)); }
  // Merge into a few boxes (a refresh per box costs a region walk, not a fetch).
  if (out.length > 12) {
    let u = null;
    for (const x of out) u = u ? {min: u.min.map((v, i) => Math.min(v, x.min[i])), max: u.max.map((v, i) => Math.max(v, x.max[i]))} : {min: x.min.slice(), max: x.max.slice()};
    return u ? [u] : [];
  }
  return out;
}

/* ---- undo / redo ----------------------------------------------------------------- */
export class History {
  /** get(): a JSON-able state; set(state): restore it; onChange(): after either. */
  constructor(get, set, onChange) {
    this.get = get; this.set = set; this.onChange = onChange || (() => {});
    this.undo = []; this.redo = []; this.key = null; this.until = 0;
  }
  /** Call BEFORE a change. Consecutive calls with the same key within 600 ms are one step. */
  push(label, key) {
    const now = Date.now();
    if (key && key === this.key && now < this.until) { this.until = now + 600; return; }
    this.key = key || null; this.until = now + 600;
    this.undo.push({label, state: JSON.stringify(this.get())});
    if (this.undo.length > 200) this.undo.shift();
    this.redo.length = 0;
  }
  doUndo() {
    const e = this.undo.pop();
    if (!e) { toast('nothing to undo'); return false; }
    this.redo.push({label: e.label, state: JSON.stringify(this.get())});
    this.key = null;
    this.set(JSON.parse(e.state));
    this.onChange();
    toast('undo: ' + e.label);
    return true;
  }
  doRedo() {
    const e = this.redo.pop();
    if (!e) { toast('nothing to redo'); return false; }
    this.undo.push({label: e.label, state: JSON.stringify(this.get())});
    this.key = null;
    this.set(JSON.parse(e.state));
    this.onChange();
    toast('redo: ' + e.label);
    return true;
  }
  clear() { this.undo = []; this.redo = []; this.key = null; }
}

/* ---- the slider panel ---------------------------------------------------------- */
/**
 * sliderPanel(schema, params, onChange(k, v, final), opts) -> {root, set(params)}
 * Rows are dgutil's vocabulary: {sec, note} | {k, n, d, min, max, step, int?}
 * | {k, n, d, kind: 'enum', options} | {k, n, d, kind: 'bool'}. A range input
 * reports every move with final = false and the release with final = true, so
 * a host can preview coarse while dragging and refine on release.
 * opts.closed: section titles to start folded.
 */
export function sliderPanel(schema, params, onChange, opts) {
  opts = opts || {};
  const root = h('div', {class: C + '-sliders'});
  const setters = new Map();
  let body = root;
  for (const r of schema) {
    if (r.sec) {
      const sb = h('div', {class: C + '-secbody'});
      const head = h('div', {class: C + '-sechead'}, r.sec);
      const sec = h('div', {class: C + '-sec' + ((opts.closed || []).includes(r.sec) ? ' closed' : '')}, head, sb);
      if (r.note) sb.append(h('div', {class: C + '-hint'}, r.note));
      head.addEventListener('click', () => sec.classList.toggle('closed'));
      root.append(sec);
      body = sb;
      continue;
    }
    if (!r.k) continue;
    const line = h('div', {class: C + '-row', title: r.d || ''});
    const lab = h('label', {}, r.n || r.k);
    line.append(lab);
    if (r.kind === 'enum') {
      const s = h('select', {}, r.options.map(o => h('option', {value: o}, o)));
      s.value = params[r.k];
      s.addEventListener('change', () => onChange(r.k, s.value, true));
      line.append(s);
      setters.set(r.k, v => { s.value = v; });
    } else if (r.kind === 'bool') {
      const c = h('input', {type: 'checkbox'});
      c.checked = !!params[r.k];
      c.addEventListener('change', () => onChange(r.k, c.checked, true));
      line.append(c);
      setters.set(r.k, v => { c.checked = !!v; });
    } else {
      const step = r.int ? 1 : (r.step || 0.01);
      const rg = h('input', {type: 'range', min: r.min, max: r.max, step});
      const nb = h('input', {type: 'number', min: r.min, max: r.max, step, class: C + '-num'});
      rg.value = nb.value = params[r.k];
      const fix = (v) => { v = clamp(+v, r.min, r.max); return r.int ? Math.round(v) : Number((Math.round(v / step) * step).toFixed(4)); };
      rg.addEventListener('input', () => { const v = fix(rg.value); nb.value = v; onChange(r.k, v, false); });
      rg.addEventListener('change', () => { const v = fix(rg.value); nb.value = v; onChange(r.k, v, true); });
      nb.addEventListener('change', () => { const v = fix(nb.value); nb.value = rg.value = v; onChange(r.k, v, true); });
      line.append(rg, nb);
      setters.set(r.k, v => { rg.value = nb.value = v; });
    }
    body.append(line);
  }
  return {root, set(p) { for (const [k, f] of setters) if (p[k] !== undefined) f(p[k]); }};
}

/* ---- the 3D view ---------------------------------------------------------------------- */
const COL = {
  open: [1.0, 0.72, 0.25, 1], linked: [0.45, 0.95, 0.45, 1], broken: [1.0, 0.33, 0.33, 1], sel: [0.4, 0.85, 1.0, 1],
  surface: [0.65, 1.0, 0.85, 1], ghost: [0.80, 0.55, 1.0, 0.9], box: [0.55, 0.62, 0.75, 0.55], dim: [0.55, 0.62, 0.75, 0.25]
};
export {COL};
function css(c) { return 'rgba(' + Math.round(c[0] * 255) + ',' + Math.round(c[1] * 255) + ',' + Math.round(c[2] * 255) + ',' + (c[3] === undefined ? 1 : c[3]) + ')'; }
export {css};

/**
 * View3D(opts): a WorldView in a wrapper with a 2D label layer.
 *   opts.draw(D)      the host's overlay: D.line(a, b, col), D.box(min, max, col)
 *                     (local continuous, max exclusive), D.label(p, text, col),
 *                     D.marker(p, glyph, col, payload) (a pickable square)
 *   opts.onClick(e, hit) a click that did not move the camera: hit = {marker} | {voxel} | null
 *   opts.onHover(e)
 *   opts.capture(e)   pointerdown before the camera sees it; return true to take it
 */
export class View3D {
  constructor(opts) {
    this.opts = opts || {};
    this.canvas = h('canvas', {class: C + '-gl', tabindex: 0});
    this.labels = h('canvas', {class: C + '-labels'});
    this.status = h('div', {class: C + '-vstatus'}, '');
    this.el = h('div', {class: C + '-view'}, this.canvas, this.labels, this.status);
    this.wv = null;
    this.frame = makeFrame([0, 0, 0], 0);
    this.clip = {mode: 'xray', box: null, ymin: -Infinity, ymax: Infinity, cut: null};
    this.roiBox = null;
    this.markers = [];
    this.dirty = true;
    this.lastSig = '';
    this.raf = 0;
    this.visible = () => this.el.offsetParent !== null;
    this.error = '';
  }
  ensure() {
    if (this.wv) return this.wv;
    if (typeof window.WorldView !== 'function') { this.error = 'worldview.js is not loaded'; return null; }
    try { this.wv = new window.WorldView(this.canvas, {seaY: 0}); }
    catch (e) { this.error = '3D view unavailable: ' + e.message; this.wv = null; return null; }
    const wv = this.wv;
    wv.view.showFar = false; wv.view.showAxes = false; wv.view.overlayOnTop = true;
    wv.view.levels = 4; wv.view.detail = 1.0; wv.view.grid = 0.6;
    wv.cam.mode = 'orbit'; wv.cam.speed = 60;
    wv.onError = () => { this.dirty = true; };
    this._bind();
    this._palette();
    return wv;
  }
  /** The material palette (the mesher needs the classes); retried while it fails. */
  _palette() {
    if (this._palBusy || (this.wv && this.wv.pal)) return;
    this._palBusy = true;
    api.palette().then(p => { this.wv.loadPalette(p); this.error = ''; this.dirty = true; })
      .catch(e => { this.error = 'palette: ' + e.message + ' (retrying)'; this._palAt = performance.now(); })
      .finally(() => { this._palBusy = false; });
  }
  _bind() {
    const cv = this.canvas;
    // Our pointerdown runs FIRST (capture on the wrapper): a gizmo or a handle
    // under the cursor takes the press away from the camera.
    this.el.addEventListener('pointerdown', (e) => {
      this._down = {x: e.clientX, y: e.clientY, t: performance.now(), btn: e.button};
      if (e.button === 0 && this.opts.capture && this.opts.capture(e, this._px(e))) { e.stopPropagation(); e.preventDefault(); this._down = null; }
    }, true);
    this.el.addEventListener('pointerup', (e) => {
      const d = this._down;
      this._down = null;
      if (!d || d.btn !== 0 || Math.abs(e.clientX - d.x) + Math.abs(e.clientY - d.y) > 4) return;
      const p = this._px(e);
      const m = this.markerAt(p[0], p[1]);
      const hit = m ? {marker: m} : (this.wv ? {voxel: this.pickVoxel(p[0], p[1])} : null);
      if (this.opts.onClick) this.opts.onClick(e, hit, p);
      this.dirty = true;
    });
    cv.addEventListener('pointermove', (e) => { if (this.opts.onHover) this.opts.onHover(e, this._px(e)); });
  }
  _px(e) { const r = this.canvas.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; }

  /** Where anchor 0,0,0 sits and the placement's quarter turn. */
  setFrame(origin, yaw) {
    const f = makeFrame(origin, yaw);
    const moved = !this.frame || f.origin.join() !== this.frame.origin.join() || f.q !== this.frame.q;
    this.frame = f;
    return moved;
  }
  /** The streamed box (world, from a local box + pad) -- the octree grows nothing outside it. */
  setRoi(localMin, localMax, pad, extraWorld) {
    if (!this.ensure()) return;
    const b = this.frame.box(localMin, localMax);
    const p = pad || 0;
    let mn = b.min.map(v => Math.floor(v - p)), mx = b.max.map(v => Math.ceil(v + p));
    if (extraWorld) { mn = mn.map((v, i) => Math.min(v, extraWorld.min[i])); mx = mx.map((v, i) => Math.max(v, extraWorld.max[i])); }
    this.wv.view.roi = {min: mn, max: mx};
    this.roiBox = {min: mn, max: mx};
    this.dirty = true;
  }
  /** mode: 'xray' (only inside the box) | 'cut' (a hole over the box) | 'solid'; box local inclusive; y range local. */
  setClip(mode, box, ymin, ymax) {
    this.clip = {mode, box, ymin, ymax};
    if (!this.wv) return;
    if (!box || mode === 'solid' && !(isFinite(ymin) || isFinite(ymax))) { this.wv.view.clip = null; this.dirty = true; return; }
    const lo = [box.min[0], isFinite(ymin) ? Math.max(box.min[1], ymin) : box.min[1], box.min[2]];
    const hi = [box.max[0], isFinite(ymax) ? Math.min(box.max[1], ymax) : box.max[1], box.max[2]];
    const w = this.frame.box(lo, hi);
    if (mode === 'xray') this.wv.view.clip = {mode: 1, min: w.min.map(v => v - 0.01), max: w.max.map(v => v + 0.01)};
    else if (mode === 'cut') this.wv.view.clip = {mode: 2, min: w.min.map(v => v - 0.01), max: w.max.map(v => v + 0.01)};
    else this.wv.view.clip = {mode: 1, min: [-1e9, w.min[1] - 0.01, -1e9], max: [1e9, w.max[1] + 0.01, 1e9]};
    this.dirty = true;
  }
  /** Orbit the camera to show a local box. */
  frameBox(min, max, opts) {
    if (!this.ensure()) return;
    const b = this.frame.box(min, max);
    const c = [0, 1, 2].map(i => (b.min[i] + b.max[i]) / 2);
    const r = Math.max(24, Math.hypot(b.max[0] - b.min[0], b.max[1] - b.min[1], b.max[2] - b.min[2]) / 2);
    const wv = this.wv;
    wv.cam.mode = 'orbit';
    wv.cam.target = c;
    wv.cam.dist = Math.min(4000, r * ((opts && opts.k) || 1.9));
    if (!opts || !opts.keepAngle) { wv.cam.yaw = 0.75; wv.cam.pitch = -0.62; }
    this.dirty = true;
  }
  flyTo(localPt, dist) {
    if (!this.ensure()) return;
    const w = this.frame.pt(localPt);
    this.wv.cam.mode = 'orbit';
    this.wv.cam.target = w;
    this.wv.cam.dist = dist || 90;
    this.dirty = true;
  }
  invalidate() {
    if (!this.wv) return;
    this.wv.invalidate(1337);
    this.dirty = true;
    // PROGRESSIVE REFINEMENT: a fresh picture starts at the coarsest pass
    // (few lod-8 / lod-4 regions: the whole dungeon in seconds, even when
    // every region queues for the GPU lock) and steps to full detail as each
    // pass finishes streaming (_tick). The regions of a coarse pass are the
    // fine pass's fallback, so nothing is fetched twice.
    if (this.progressive) { this.stage = 0; this.wv.view.detail = this.progressive[0]; }
  }
  /** Has everything the current tree wants landed and meshed? */
  streamIdle() {
    const wv = this.wv;
    if (!wv || !wv.pal || !wv.tree) return false;
    const s = wv.stats;
    return s.live > 0 && s.ready >= s.live && !wv.inflight && !s.queued && !wv.staleCount() && !s.meshing;
  }
  /** Streamed AND refined to the last pass (the harness's readiness, with the
   *  page's own "the program is installed" test on top). */
  settled() { return this.streamIdle() && (!this.progressive || this.stage === undefined || this.stage >= this.progressive.length - 1); }
  /** Refetch (softly) the regions of local inclusive boxes. */
  refresh(boxes) {
    if (!this.wv) return;
    if (boxes === 'all') { const r = this.roiBox; if (r) this.wv.refreshBox(r.min, r.max); else this.invalidate(); return; }
    for (const b of boxes) {
      const w = this.frame.box(b.min, b.max);
      this.wv.refreshBox(w.min.map(v => Math.floor(v) - 2), w.max.map(v => Math.ceil(v) + 2));
    }
    this.dirty = true;
  }
  /** Coarse while dragging, full on release. */
  coarse(on) {
    if (!this.wv) return;
    const P = this.progressive;
    if (!on && P) this.stage = P.length - 1;      // a release goes straight to the finest pass
    this.wv.view.detail = on ? 0.3 : (P ? P[P.length - 1] : 1.0);
    this.dirty = true;
  }

  start() { if (!this.raf) { this._last = 0; this.raf = requestAnimationFrame(t => this._tick(t)); } }
  stop() { if (this.raf) cancelAnimationFrame(this.raf); this.raf = 0; }
  _sig() {
    const wv = this.wv, c = wv.cam;
    return [c.mode, c.target, c.dist, c.yaw, c.pitch, c.pos, wv.drawRev, wv.view.detail, this.canvas.clientWidth, this.canvas.clientHeight].join('|');
  }
  _tick(now) {
    this.raf = requestAnimationFrame(t => this._tick(t));
    if (!this.visible()) return;
    if (!this.wv) { this.status.textContent = this.error || ''; return; }
    if (!this.wv.pal && !this._palBusy && performance.now() - (this._palAt || 0) > 3000) this._palette();
    const dt = Math.min(0.1, (now - (this._last || now)) / 1000);
    this._last = now;
    this.wv.step(dt);
    this.wv.update();
    this._refine();
    const s = this._sig();
    if (s !== this.lastSig || this.dirty) { this.lastSig = s; this.dirty = false; this.draw(); }
    const line = this.statusLine();
    if (this.status.textContent !== line) this.status.textContent = line;
  }
  /** Step the progressive passes: the next one once this one has streamed. */
  _refine() {
    const P = this.progressive;
    if (!P || !this.wv || this.stage === undefined || this.stage >= P.length - 1) return;
    if (this.wv.view.detail !== P[this.stage]) return;    // coarse() (a drag) owns it now
    if (!this.streamIdle()) return;
    this.stage++;
    this.wv.view.detail = P[this.stage];
    this.dirty = true;
  }
  /** The status line: the previewer's phase, the stream, the pass, the host's. */
  statusLine() {
    const wv = this.wv;
    if (!wv) return this.error || '';
    const pv = this.previewer && this.previewer.status;
    const P = this.progressive;
    const pass = P && this.stage !== undefined && this.stage < P.length - 1 ? ' · coarse pass ' + (this.stage + 1) + '/' + P.length : '';
    const st = wv.streamStatus() + (wv.staleCount() ? ' · refreshing ' + wv.staleCount() : '') + pass;
    return (this.error ? this.error + ' · ' : '') + (pv ? pv + ' · ' : '') + st + (this.extraStatus ? ' · ' + this.extraStatus : '');
  }
  /** One frame's work without requestAnimationFrame (a headless harness whose
   *  virtual clock stands still while its fetches are pending gets no frames). */
  pump() {
    if (!this.wv) return;
    if (!this.wv.pal) this._palette();
    this.wv.update();
    this._refine();
    this.draw();
    this.status.textContent = this.statusLine();
  }
  /** Render now (the harness calls it too). */
  draw() {
    const wv = this.wv;
    if (!wv) return;
    const lines = [], labels = [], markers = [];
    const F = this.frame;
    const D = {
      line: (a, b, col) => { const p = F.pt(a), q = F.pt(b); lines.push([p, q, col]); },
      box: (mn, mx, col) => {
        const P = (x, y, z) => [x, y, z];
        const c = [P(mn[0], mn[1], mn[2]), P(mx[0], mn[1], mn[2]), P(mx[0], mn[1], mx[2]), P(mn[0], mn[1], mx[2]),
                   P(mn[0], mx[1], mn[2]), P(mx[0], mx[1], mn[2]), P(mx[0], mx[1], mx[2]), P(mn[0], mx[1], mx[2])];
        const E = [[0, 1], [1, 2], [2, 3], [3, 0], [4, 5], [5, 6], [6, 7], [7, 4], [0, 4], [1, 5], [2, 6], [3, 7]];
        for (const [i, j] of E) D.line(c[i], c[j], col);
      },
      label: (p, text, col, o) => labels.push({w: F.pt(p), text, col, o: o || {}}),
      marker: (p, glyph, col, payload, o) => markers.push({w: F.pt(p), glyph, col, payload, o: o || {}})
    };
    try { if (this.opts.draw) this.opts.draw(D); } catch (e) { console.error('dungeon overlay', e); }
    wv.onOverlay = (push) => { for (const [a, b, c] of lines) push(a[0], a[1], a[2], b[0], b[1], b[2], c); };
    wv.render();
    // 2D layer: labels + markers, projected with the frame's own matrix.
    const cv = this.labels, dpr = Math.min(window.devicePixelRatio || 1, 2);
    const W = this.canvas.clientWidth, H = this.canvas.clientHeight;
    if (cv.width !== Math.round(W * dpr) || cv.height !== Math.round(H * dpr)) { cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr); }
    const g = cv.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, W, H);
    g.imageSmoothingEnabled = false;
    g.font = '11px "Cascadia Code", Consolas, monospace';
    g.textBaseline = 'middle';
    this.markers = [];
    for (const m of markers) {
      const s = this.projectW(m.w);
      if (!s) continue;
      const x = Math.round(s[0]), y = Math.round(s[1]), r = m.o.size || 6;
      g.fillStyle = '#0b0e13'; g.fillRect(x - r - 1, y - r - 1, 2 * r + 2, 2 * r + 2);
      g.fillStyle = css(m.col); g.fillRect(x - r, y - r, 2 * r, 2 * r);
      if (m.o.sel) { g.strokeStyle = '#ffffff'; g.lineWidth = 2; g.strokeRect(x - r - 3, y - r - 3, 2 * r + 6, 2 * r + 6); }
      if (m.glyph) { g.fillStyle = '#0b0e13'; g.textAlign = 'center'; g.fillText(m.glyph, x, y + 1); }
      this.markers.push({x, y, r: r + 3, payload: m.payload});
    }
    g.textAlign = 'left';
    for (const L of labels) {
      const s = this.projectW(L.w);
      if (!s) continue;
      const x = Math.round(s[0]) + (L.o.dx || 0), y = Math.round(s[1]) + (L.o.dy || 0);
      const w = Math.ceil(g.measureText(L.text).width) + 6;
      g.fillStyle = 'rgba(11,14,19,0.82)'; g.fillRect(x - 3, y - 7, w, 14);
      g.fillStyle = css(L.col || [1, 1, 1, 1]); g.fillText(L.text, x, y);
    }
  }
  /** World point -> [x, y] css px, or null behind the camera / off screen. */
  projectW(w) {
    const m = this.wv && this.wv.mvp;
    if (!m) return null;
    const cx = m[0] * w[0] + m[4] * w[1] + m[8] * w[2] + m[12];
    const cy = m[1] * w[0] + m[5] * w[1] + m[9] * w[2] + m[13];
    const cw = m[3] * w[0] + m[7] * w[1] + m[11] * w[2] + m[15];
    if (cw <= 0.05) return null;
    const W = this.canvas.clientWidth, H = this.canvas.clientHeight;
    const x = (cx / cw * 0.5 + 0.5) * W, y = (1 - (cy / cw * 0.5 + 0.5)) * H;
    if (x < -200 || y < -200 || x > W + 200 || y > H + 200) return null;
    return [x, y];
  }
  project(localPt) { return this.projectW(this.frame.pt(localPt)); }
  markerAt(px, py) {
    let best = null, bd = Infinity;
    for (const m of this.markers) { const d = Math.max(Math.abs(m.x - px), Math.abs(m.y - py)); if (d <= m.r && d < bd) { bd = d; best = m.payload; } }
    return best;
  }
  /** Eye + unit direction through a pixel, world. */
  ray(px, py) {
    const wv = this.wv, cv = this.canvas;
    const w = cv.clientWidth, hh = cv.clientHeight;
    const nx = (px / w) * 2 - 1, ny = 1 - (py / hh) * 2;
    const eye = wv.camEye(), f = wv.camForward();
    let rt = [f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0];
    const rl = Math.hypot(rt[0], rt[1], rt[2]) || 1; rt = rt.map(v => v / rl);
    const up = [rt[1] * f[2] - rt[2] * f[1], rt[2] * f[0] - rt[0] * f[2], rt[0] * f[1] - rt[1] * f[0]];
    const t = Math.tan(wv.fov() / 2), asp = w / hh;
    const d = [0, 1, 2].map(i => f[i] + rt[i] * nx * t * asp + up[i] * ny * t);
    const dl = Math.hypot(d[0], d[1], d[2]) || 1;
    return {o: eye, d: d.map(v => v / dl)};
  }
  /** The ray hits a plane y = const (local) at -> local point, or null. */
  rayPlaneY(px, py, yLocal) {
    const r = this.ray(px, py), yw = this.frame.origin[1] + yLocal;
    if (Math.abs(r.d[1]) < 1e-5) return null;
    const t = (yw - r.o[1]) / r.d[1];
    if (t <= 0) return null;
    return this.frame.inv([r.o[0] + r.d[0] * t, r.o[1] + r.d[1] * t, r.o[2] + r.d[2] * t]);
  }
  /**
   * THE X-RAY PICK: what the picture shows under the pixel. In x-ray the near
   * walls are culled (backfaces), so the first visible surface is the first
   * SOLID cell after the ray has been in AIR inside the clip box; outside the
   * box everything is see-through. -> {cell (local), face (local dir), world} or null.
   */
  pickVoxel(px, py) {
    const wv = this.wv;
    if (!wv || !wv.mvp) return null;
    const r = this.ray(px, py);
    const clip = wv.view.clip;
    const inBox = (x, y, z) => !clip || clip.mode !== 1 || (x + 0.5 >= clip.min[0] && x + 0.5 <= clip.max[0] && y + 0.5 >= clip.min[1] && y + 0.5 <= clip.max[1] && z + 0.5 >= clip.min[2] && z + 0.5 <= clip.max[2]);
    const cutAir = (x, y, z) => clip && clip.mode === 2 && x + 0.5 >= clip.min[0] && x + 0.5 <= clip.max[0] && z + 0.5 >= clip.min[2] && z + 0.5 <= clip.max[2] && y + 0.5 > clip.max[1];
    let x = Math.floor(r.o[0]), y = Math.floor(r.o[1]), z = Math.floor(r.o[2]);
    const [dx, dy, dz] = r.d;
    const sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
    const tdx = dx ? Math.abs(1 / dx) : Infinity, tdy = dy ? Math.abs(1 / dy) : Infinity, tdz = dz ? Math.abs(1 / dz) : Infinity;
    let tmx = dx ? ((dx > 0 ? x + 1 - r.o[0] : r.o[0] - x) * tdx) : Infinity;
    let tmy = dy ? ((dy > 0 ? y + 1 - r.o[1] : r.o[1] - y) * tdy) : Infinity;
    let tmz = dz ? ((dz > 0 ? z + 1 - r.o[2] : r.o[2] - z) * tdz) : Infinity;
    let face = [0, 0, 0], seenAir = !clip || clip.mode !== 1;
    const cache = [];
    for (let i = 0; i < 6000; i++) {
      let v = wv.cellAtWorld(x, y, z, 0);
      for (let L = 1; L < 4 && v < 0; L++) v = wv._wordAt(x, y, z, L, cache);
      const mat = v > 0 ? (v & 0xFFF) : 0;
      const cls = mat && wv.classes ? wv.classes[mat] : 0;
      const solid = mat && cls < 2 && inBox(x, y, z) && !cutAir(x, y, z);
      if (inBox(x, y, z) && v >= 0 && !solid) seenAir = true;
      if (solid && seenAir) {
        const fl = this.frame.invCell([x, y, z]);
        const fd = rotQ((4 - this.frame.q) & 3, face[0], face[2]);
        return {cell: fl, face: [fd[0], face[1], fd[1]], world: [x, y, z], mat};
      }
      if (tmx < tmy && tmx < tmz) { x += sx; tmx += tdx; face = [-sx, 0, 0]; }
      else if (tmy < tmz) { y += sy; tmy += tdy; face = [0, -sy, 0]; }
      else { z += sz; tmz += tdz; face = [0, 0, -sz]; }
    }
    return null;
  }
}

/* ---- shared drawing helpers ------------------------------------------------------------ */
/** A socket's opening as a frame (+ an arrow along its facing). s.pos = the
 *  floor-line centre (horizontal) or the opening centre (vertical). */
export function drawSocket(D, s, col, label) {
  const p = s.pos, w = s.w / 2;
  if (s.dir[1] !== 0) {
    const hh = s.h / 2;
    const c = [[p[0] - w, p[1], p[2] - hh], [p[0] + w, p[1], p[2] - hh], [p[0] + w, p[1], p[2] + hh], [p[0] - w, p[1], p[2] + hh]];
    for (let i = 0; i < 4; i++) D.line(c[i], c[(i + 1) % 4], col);
    D.line(p, [p[0], p[1] + s.dir[1] * 14, p[2]], col);
    if (label) D.label([p[0] + w, p[1], p[2]], label, col, {dx: 4});
    return;
  }
  const ax = s.dir[0] !== 0 ? [0, 0, 1] : [1, 0, 0];
  const A = (k, y) => [p[0] + ax[0] * k, p[1] + y, p[2] + ax[2] * k];
  const c = [A(-w, 0), A(w, 0), A(w, s.h), A(-w, s.h)];
  for (let i = 0; i < 4; i++) D.line(c[i], c[(i + 1) % 4], col);
  D.line(A(-w, 0), A(w, s.h), [col[0], col[1], col[2], col[3] * 0.35]);
  const m = [p[0], p[1] + s.h / 2, p[2]];
  D.line(m, [m[0] + s.dir[0] * 14, m[1], m[2] + s.dir[2] * 14], col);
  if (label) D.label(A(0, s.h + 3), label, col, {dx: -10});
}
/** The centre of a socket's opening (its pick point). */
export function socketCentre(s) { return s.dir[1] !== 0 ? s.pos.slice() : [s.pos[0], s.pos[1] + s.h / 2, s.pos[2]]; }

/* ---- the tab's stylesheet ---------------------------------------------------------------- */
// Pixel-flavoured like the rest of the tuner: square corners, 1-2 px rules,
// the mono face for numbers, the tuner's palette vars.
export const CSS = `
#view-dungeons{padding:0}
.dg-root{display:flex;flex-direction:column;height:calc(100vh - 150px);min-height:520px}
.dg-pages{display:flex;gap:0;border-bottom:2px solid var(--line);background:var(--bg2)}
.dg-pages button{border-radius:0;border:0;border-right:1px solid var(--line);background:transparent;padding:6px 16px;font-family:var(--mono);font-size:12px;color:var(--dim)}
.dg-pages button.on{background:var(--card2);color:var(--amber);box-shadow:inset 0 -2px 0 var(--amber)}
.dg-pages .grow{flex:1}
.dg-pages .st{font-family:var(--mono);font-size:11px;color:var(--faint);padding:6px 10px}
.dg-page{display:none;flex:1;min-height:0}
.dg-page.on{display:flex;flex-direction:column}
.dg-bar{display:flex;gap:6px;align-items:center;padding:5px 8px;border-bottom:1px solid var(--line);background:var(--bg2);flex-wrap:wrap}
.dg-bar button,.dg-bar select,.dg-bar input{border-radius:0;font-size:12px}
.dg-bar .grow{flex:1}
.dg-bar .st{font-family:var(--mono);font-size:11px;color:var(--dim)}
.dg-bar .st.dirty{color:var(--amber)}
.dg-bar label{font-size:11px;color:var(--dim);display:inline-flex;gap:4px;align-items:center}
.dg-bar input[type=number]{width:70px}
.dg-bar input.dg-name{width:170px}
.dg-main{flex:1;display:grid;min-height:0}
.dg-kitmain{grid-template-columns:220px 1fr 340px}
.dg-stmain{grid-template-columns:230px 1fr 350px}
.dg-side{border-right:1px solid var(--line);overflow:auto;padding:6px;background:var(--bg)}
.dg-insp{border-left:1px solid var(--line);overflow:auto;padding:6px 8px;background:var(--bg)}
.dg-side h4,.dg-insp h4{margin:10px 0 4px;font-size:10px;letter-spacing:1.5px;text-transform:uppercase;color:var(--faint);font-family:var(--mono)}
.dg-item{padding:3px 6px;cursor:pointer;font-family:var(--mono);font-size:12px;display:flex;gap:6px;align-items:center;border:1px solid transparent}
.dg-item:hover{background:var(--card2);border-color:var(--line2)}
.dg-item.sel{background:#1d3a6e;border-color:var(--acc)}
.dg-item .tag{font-size:10px;color:var(--faint);margin-left:auto;white-space:nowrap}
.dg-sock.sel{background:var(--card2);color:var(--acc)}
.dg-item .sw{width:8px;height:8px;flex:none}
.dg-item[draggable=true]{cursor:grab}
.dg-center{display:flex;flex-direction:column;min-width:0;min-height:0}
.dg-view{position:relative;flex:1;min-height:200px;background:#0b0e13;overflow:hidden}
.dg-gl{position:absolute;inset:0;width:100%;height:100%;display:block;outline:none}
.dg-labels{position:absolute;inset:0;width:100%;height:100%;pointer-events:none;image-rendering:pixelated}
.dg-vstatus{position:absolute;left:6px;bottom:4px;right:6px;font-family:var(--mono);font-size:10.5px;color:var(--faint);pointer-events:none;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.dg-vbar{display:flex;gap:6px;align-items:center;padding:4px 8px;border-top:1px solid var(--line);background:var(--bg2);flex-wrap:wrap;font-size:11px;color:var(--dim)}
.dg-vbar button,.dg-vbar select{border-radius:0;font-size:11px;padding:2px 7px}
.dg-vbar button.on{border-color:var(--amber);color:var(--amber)}
.dg-vbar input[type=range]{width:90px}
.dg-sliders{font-size:12px}
.dg-sec{border:1px solid var(--line);margin:4px 0;background:var(--card)}
.dg-sechead{padding:3px 6px;font-family:var(--mono);font-size:11px;color:var(--amber);cursor:pointer;background:var(--bg2);border-bottom:1px solid var(--line)}
.dg-sechead::before{content:'\\25BE ';color:var(--faint)}
.dg-sec.closed .dg-sechead::before{content:'\\25B8 '}
.dg-sec.closed .dg-secbody{display:none}
.dg-secbody{padding:3px 6px}
.dg-hint{font-size:11px;color:var(--faint);margin:2px 0 4px;line-height:1.35}
.dg-row{display:grid;grid-template-columns:118px 1fr 58px;gap:5px;align-items:center;margin:2px 0}
.dg-row label{font-size:11.5px;color:var(--dim);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.dg-row select{grid-column:2/4}
.dg-row input[type=range]{width:100%}
.dg-num{width:58px;font-family:var(--mono);font-size:11px;border-radius:0;padding:1px 3px}
.dg-field{display:grid;grid-template-columns:96px 1fr;gap:5px;align-items:center;margin:3px 0;font-size:12px}
.dg-field label{font-size:11px;color:var(--dim)}
.dg-field input,.dg-field select{border-radius:0;font-size:12px;min-width:0}
.dg-btns{display:flex;flex-wrap:wrap;gap:4px;margin:6px 0}
.dg-btns button{border-radius:0;font-size:11.5px;padding:3px 8px}
button.dg-pri{border-color:var(--amber);color:var(--amber)}
button.dg-bad{border-color:var(--red);color:var(--red)}
.dg-bottom{height:190px;border-top:2px solid var(--line);display:flex;flex-direction:column;background:var(--bg)}
.dg-bottom.min{height:26px}
.dg-btabs{display:flex;border-bottom:1px solid var(--line);background:var(--bg2)}
.dg-btabs button{border-radius:0;border:0;border-right:1px solid var(--line);background:transparent;font-size:11px;font-family:var(--mono);padding:3px 12px;color:var(--dim)}
.dg-btabs button.on{color:var(--amber);background:var(--card2)}
.dg-btabs .grow{flex:1}
.dg-bbody{flex:1;overflow:auto;position:relative}
.dg-issues{font-family:var(--mono);font-size:11.5px;padding:3px 8px}
.dg-issues div{cursor:pointer;padding:1px 3px;border-left:3px solid transparent}
.dg-issues div:hover{background:var(--card2)}
.dg-issues .e{color:var(--red);border-left-color:var(--red)}
.dg-issues .w{color:var(--amber);border-left-color:var(--amber)}
.dg-issues .i{color:var(--faint)}
.dg-issues .ok{color:var(--green);cursor:default}
.dg-graph{width:100%;height:100%;display:block;cursor:pointer;image-rendering:pixelated}
.dg-chip{display:inline-block;padding:0 5px;border:1px solid var(--line2);font-family:var(--mono);font-size:10.5px;color:var(--dim);margin:1px}
.dg-chip.warn{border-color:var(--amber);color:var(--amber)}
.dg-chip.bad{border-color:var(--red);color:var(--red)}
.dg-chip.ok{border-color:var(--green);color:var(--green)}
.dg-sock{font-family:var(--mono);font-size:11px;padding:2px 4px;border-left:3px solid var(--line2);margin:2px 0;cursor:pointer}
.dg-sock.open{border-left-color:var(--amber)}
.dg-sock.linked{border-left-color:var(--green)}
.dg-sock.broken{border-left-color:var(--red)}
.dg-tgrid{display:grid;grid-template-columns:1fr 1fr;gap:3px}
.dg-tgrid button{border-radius:0;font-size:11px;padding:3px 4px;text-align:left}
.dg-propview{height:200px;position:relative;border:1px solid var(--line);background:#0b0e13;margin-top:4px}
.dg-tabs2{display:flex;margin:0 0 4px}
.dg-tabs2 button{border-radius:0;flex:1;font-size:11px;padding:3px}
.dg-tabs2 button.on{border-color:var(--amber);color:var(--amber)}
.dg-empty{padding:30px;color:var(--faint);font-size:13px;line-height:1.6}
.dg-ghostbar{position:absolute;top:6px;left:50%;transform:translateX(-50%);background:#1b1430;border:2px solid #b78cff;padding:5px 10px;display:flex;gap:6px;align-items:center;font-family:var(--mono);font-size:12px;color:#d9c6ff;z-index:3}
.dg-ghostbar button{border-radius:0;font-size:11px}
.dg-drop{outline:2px dashed var(--amber);outline-offset:-4px}
`;
