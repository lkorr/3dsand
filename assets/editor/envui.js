/* envui.js — the widgets the Environment pages share.
 *
 * trees.js grew a row builder, a collapsible section, a coalescing undo stack
 * and a WorldView palette bridge. The water page and the biome page want
 * exactly the same four things, so they live here once rather than three
 * times. trees.js keeps its own copies on purpose — it predates this file and
 * is asserted line-by-line by scripts/check_trees.sh; folding it in is a
 * separate change with its own harness run.
 *
 * Every builder takes the host's `el` (tuner.html's element helper) so this
 * module has no DOM conventions of its own.
 */

'use strict';

export function getPath(obj, path) {
  if (!path) return obj;
  return path.split('.').reduce((o, k) => (o == null ? o : o[k]), obj);
}
export function setPath(obj, path, v) {
  const ks = path.split('.');
  const last = ks.pop();
  const t = ks.reduce((o, k) => (o[k] == null ? (o[k] = {}) : o[k]), obj);
  t[last] = v;
}

/* ---------------------------------------------------------------------------
 * UNDO — snapshots of the whole object, with drag coalescing.
 *
 * A range input fires `input` per pixel; without the coalesce window one drag
 * is fifty undo steps and Ctrl+Z appears dead. Consecutive edits to the SAME
 * key inside COALESCE_MS collapse into the snapshot taken before the drag.
 * ------------------------------------------------------------------------- */
export function makeUndo(opts) {
  const MAX = opts.max || 100, COALESCE_MS = opts.coalesceMs || 500;
  let stack = [], redo = [], key = null, until = 0;
  const get = opts.get, set = opts.set, toast = opts.toast || (() => {});
  return {
    snapshot(k) {
      const now = Date.now();
      if (k !== null && k === key && now < until) { until = now + COALESCE_MS; return; }
      key = k; until = now + COALESCE_MS;
      stack.push(JSON.stringify(get()));
      if (stack.length > MAX) stack.shift();
      redo.length = 0;
    },
    undo() {
      if (!stack.length) { toast('nothing to undo'); return false; }
      redo.push(JSON.stringify(get()));
      key = null;
      set(JSON.parse(stack.pop()));
      toast('undo (' + stack.length + ' left)');
      return true;
    },
    redo() {
      if (!redo.length) { toast('nothing to redo'); return false; }
      stack.push(JSON.stringify(get()));
      key = null;
      set(JSON.parse(redo.pop()));
      toast('redo (' + redo.length + ' left)');
      return true;
    },
    clear() { stack = []; redo = []; key = null; },
    depth() { return stack.length; }
  };
}

/* ---------------------------------------------------------------------------
 * A collapsible section: header toggles the body.
 * ------------------------------------------------------------------------- */
export function section(el, cls, title, note, opts) {
  const body = el('div', {class: cls + 'sec-body'});
  const head = el('div', {class: cls + 'sec-head'}, title);
  const wrap = el('div', {class: cls + 'sec'}, head, body);
  if (note) body.append(el('div', {class: cls + 'hint'}, note));
  if (opts && opts.closed) wrap.classList.add('closed');
  head.addEventListener('click', () => wrap.classList.toggle('closed'));
  return {wrap, body, head};
}

/* ---------------------------------------------------------------------------
 * A slider row: label, range, number. Same vocabulary as tuner_schema.js.
 *
 *   r = {k, n, d, min, max, step, int, maxOf(P), retunes}
 *   ctx = {el, cls, params:() => object, snapshot(key), onChange(r, path),
 *          widgets: []  (refreshers, called after an undo replaces params)}
 *
 * The row resolves its value from ctx.params() on EVERY access rather than
 * capturing the sub-object: undo replaces the object wholesale and a captured
 * reference would be an orphan (trees.js learned this the hard way).
 * ------------------------------------------------------------------------- */
export function row(ctx, container, r, base) {
  const el = ctx.el, cls = ctx.cls;
  const path = [base, r.k].filter(Boolean).join('.');
  const P = () => ctx.params();
  const cur = getPath(P(), path);
  const maxOf = () => (r.maxOf ? r.maxOf(P()) : r.max);
  const num = el('input', {type: 'number', class: cls + 'num', value: String(cur),
                           step: String(r.step), min: String(r.min), max: String(maxOf())});
  const rng = el('input', {type: 'range', class: cls + 'rng', value: String(cur),
                           step: String(r.step), min: String(r.min), max: String(maxOf())});
  const commit = (v, key) => {
    let x = parseFloat(v);
    if (!isFinite(x)) return;
    x = Math.min(maxOf(), Math.max(r.min, x));
    if (r.int) x = Math.round(x);
    if (x === getPath(P(), path)) return;
    ctx.snapshot(key);
    setPath(P(), path, x);
    num.value = String(x); rng.value = String(x);
    ctx.onChange(r, path);
  };
  rng.addEventListener('input', () => commit(rng.value, path));
  num.addEventListener('change', () => commit(num.value, null));
  const label = el('label', {}, r.n);
  const line = el('div', {class: cls + 'row', title: r.d || ''}, label, rng, num);
  if (r.u) line.append(el('span', {class: cls + 'unit'}, r.u));
  container.append(line);
  const set = (v) => {
    const mx = String(maxOf());
    num.max = mx; rng.max = mx;
    num.value = String(v); rng.value = String(v);
  };
  ctx.widgets.push(() => { const v = getPath(P(), path); if (v !== undefined) set(v); });
  return line;
}

/** A checkbox row over a boolean path. */
export function boolRow(ctx, container, r, base) {
  const el = ctx.el, cls = ctx.cls;
  const path = [base, r.k].filter(Boolean).join('.');
  const P = () => ctx.params();
  const chk = el('input', {type: 'checkbox'});
  chk.checked = !!getPath(P(), path);
  chk.addEventListener('change', () => {
    ctx.snapshot(null);
    setPath(P(), path, chk.checked);
    ctx.onChange(r, path);
  });
  const line = el('div', {class: cls + 'row', title: r.d || ''}, el('label', {}, r.n),
                  el('div', {}, chk));
  container.append(line);
  ctx.widgets.push(() => { chk.checked = !!getPath(P(), path); });
  return line;
}

/** A material <select> over a string path. `allowNone` adds a '(none)' option
 *  that writes ''. `filter(m)` narrows by class/tag. */
export function matRow(ctx, container, r, base, mats, opts) {
  const el = ctx.el, cls = ctx.cls;
  opts = opts || {};
  const path = [base, r.k].filter(Boolean).join('.');
  const P = () => ctx.params();
  const sel = el('select', {class: cls + 'num'});
  const rebuild = () => {
    sel.innerHTML = '';
    if (opts.allowNone) sel.append(el('option', {value: ''}, '(none)'));
    const list = (mats() || []).filter(m => !opts.filter || opts.filter(m));
    list.forEach(m => sel.append(el('option', {value: m.id}, m.id)));
    const cur = getPath(P(), path) || '';
    if (cur && ![...sel.options].some(o => o.value === cur))
      sel.append(el('option', {value: cur}, cur + ' (not in materials.json)'));
    sel.value = cur === 'none' ? '' : cur;
  };
  rebuild();
  sel.addEventListener('change', () => {
    if ((getPath(P(), path) || '') === sel.value) return;
    ctx.snapshot(null);
    setPath(P(), path, sel.value);
    ctx.onChange(r, path);
  });
  const line = el('div', {class: cls + 'row', title: r.d || ''}, el('label', {}, r.n), sel);
  container.append(line);
  ctx.widgets.push(rebuild);
  return line;
}

/* ---------------------------------------------------------------------------
 * WorldView bridge — the same two functions trees.js has, so a page's preview
 * is coloured with the game's own material table.
 * ------------------------------------------------------------------------- */
export function paletteFromMaterials(mats) {
  const CLASS = {solid: 0, powder: 1, liquid: 2, gas: 3};
  return {
    chunk: 16,
    materials: [{id: 0, name: 'air', colors: ['#000000'], class: 3, emission: 0,
                 opacity: 0, flags: 8}].concat(mats.map((m, i) => ({
      id: i + 1,
      name: m.id,
      colors: m.colors && m.colors.length ? m.colors : ['#ff00ff'],
      class: CLASS[m.class] !== undefined ? CLASS[m.class] : 0,
      emission: m.emission || 0,
      opacity: m.opacity === undefined ? 255 : m.opacity,
      flags: 0
    })))
  };
}

/** Local palette index -> engine material id, by NAME, as the C++ loaders do.
 *  Unknown names are collected in `missing` and become air. */
export function toViewerCells(res, mats, missing) {
  const idOf = new Map();
  (mats || []).forEach((m, i) => idOf.set(m.id, i + 1));
  const remap = new Uint16Array(res.names.length + 1);
  res.names.forEach((n, i) => {
    const id = idOf.get(n);
    if (id === undefined && missing && missing.indexOf(n) < 0) missing.push(n);
    remap[i + 1] = id || 0;
  });
  const cells = new Uint16Array(res.cells.length);
  for (let i = 0; i < cells.length; i++) {
    const w = res.cells[i];
    cells[i] = w ? ((remap[w & 0xFFF] & 0xFFF) | (w & 0xF000)) : 0;
  }
  return cells;
}

/** A WorldView on `canvas` set up as a local-region orbit viewer, rendering
 *  only while `isActive()` says so. Returns null (and reports) if WebGL2 is
 *  unavailable. */
export function makeView(canvas, mats, isActive, onFail) {
  try {
    const wv = new WorldView(canvas, {streaming: false, seaY: 0});
    wv.view.levels = 1;
    wv.view.showFar = false;
    wv.view.showAxes = false;
    wv.view.grid = 0;
    wv.cam.mode = 'orbit';
    wv.tool.name = 'none';
    if (mats && mats.length) wv.loadPalette(paletteFromMaterials(mats));
    const loop = () => {
      if (isActive()) wv.render();
      requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);
    return wv;
  } catch (e) {
    console.error('envui: WebGL2 view failed', e);
    if (onFail) onFail(e);
    return null;
  }
}

/** Frame an orbit camera on a box of `dim` cells at `origin`. */
export function frame(wv, dim, origin, yBias) {
  origin = origin || [0, 0, 0];
  wv.cam.mode = 'orbit';
  wv.cam.target = [origin[0] + dim.x / 2, origin[1] + dim.y * (yBias === undefined ? 0.35 : yBias),
                   origin[2] + dim.z / 2];
  wv.cam.dist = Math.max(dim.x, dim.z, dim.y * 0.8) * 1.25;
  wv.cam.yaw = 0.7; wv.cam.pitch = -0.45;
}

/** Shared page CSS, parameterised by the page id and its class prefix. Every
 *  selector is scoped under `#<pageId>` and the display rule is written as
 *  `#<pageId>.active` — never a bare id — for the reason check_tabs.sh
 *  exists (an id selector beats `.view{display:none}`). */
export function pageCss(pageId, cls, extra) {
  const P = '#' + pageId, c = '.' + cls;
  return `
${P}{display:none}
${P}.active{display:flex;gap:12px;height:100%;min-height:520px}
${P} ${c}left{width:380px;flex:0 0 380px;display:flex;flex-direction:column;gap:8px;min-height:0}
${P} ${c}right{flex:1;display:flex;flex-direction:column;gap:8px;min-width:0;min-height:0}
${P} ${c}bar{display:flex;gap:6px;align-items:center;flex-wrap:wrap}
${P} ${c}sliders{overflow-y:auto;flex:1;min-height:0;padding-right:4px}
${P} ${c}sec{border:1px solid #2a3040;border-radius:6px;margin-bottom:6px;overflow:hidden}
${P} ${c}sec-head{background:#1b2130;padding:6px 9px;font-weight:600;cursor:pointer;user-select:none}
${P} ${c}sec-head:before{content:'\\25be ';opacity:.6}
${P} ${c}sec.closed ${c}sec-head:before{content:'\\25b8 '}
${P} ${c}sec.closed ${c}sec-body{display:none}
${P} ${c}sec-body{padding:6px 8px}
${P} ${c}row{display:grid;grid-template-columns:130px minmax(0,1fr) 62px auto;gap:6px;align-items:center;margin:3px 0}
${P} ${c}row label{font-size:11px;color:#9fb0c8;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
${P} ${c}rng{width:100%;min-width:0;margin:0;accent-color:var(--acc,#5aa9e6);cursor:pointer}
${P} ${c}num{width:100%;min-width:0;box-sizing:border-box;background:#12161f;color:#dbe4f0;border:1px solid #2a3040;border-radius:4px;padding:2px 4px;font:11px monospace;text-align:right}
${P} select${c}num{text-align:left}
${P} ${c}unit{font-size:10px;color:#6f7f97}
${P} ${c}hint{font-size:11px;color:#7c8ba3;margin:2px 0 6px;line-height:1.35}
${P} ${c}stats{font:11px/1.5 monospace;color:#9fb0c8}
${P} ${c}stats .warn{color:#ffb454}
${P} ${c}stats .ok{color:#7fd48a}
${P} canvas${c}view{flex:1;min-height:0;width:100%;background:#0e1116;border:1px solid #2a3040;border-radius:6px}
${P} button.on{background:#2b4a6f;border-color:#5aa9e6}
${extra || ''}`;
}
