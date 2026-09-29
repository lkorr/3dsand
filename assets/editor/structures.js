/* structures.js — the Structures page of the Environment tab.
 *
 * WHAT IT IS. The authoring surface for building blueprints
 * (docs/PLAN_world_editor.md §2.2, package P2): assets/structures/<name>.vox
 * (the voxels) + <name>.struct.json (the ground anchor, the SLOTS — doors,
 * beds, chests, hearth, work spots, waynodes — and the generator record).
 * A slider column over housegen.js's SCHEMA, a live 3D preview with every slot
 * drawn and labelled, a seed + Reroll, Save / Save as, and the list of every
 * structure on disk with a HAND-EDITED badge.
 *
 * ONE GENERATOR. assets/editor/housegen.js is the only house builder: this page
 * calls it, `scripts/bake_structure.mjs` calls it, `scripts/test_housegen.mjs`
 * pins it. What you see here is byte-for-byte what Save writes.
 *
 * DISK vs GENERATED. Opening a structure shows the .vox ON DISK — which is the
 * truth once a person has edited it (PLAN §2.2) — with the sliders set from its
 * generator record. Touching a slider or the seed switches the preview to the
 * regenerated scaffold. Saving over a HAND-EDITED structure is refused (here,
 * and again by the tuner server): the page offers "<name>_v2" instead.
 *
 * THE PAGE WORKS WITH NO BUILT EXE. Generation is pure JS, the palette is the
 * materials.json the tuner holds, files go through /api/model (the server
 * allowlists structures/ and structures/samples/).
 */

import * as HG from './housegen.js';
import * as VOX from './vox.js';
import * as UI from './envui.js';

const PAGE = 'env-structures';
const CLS = 'sg';

let H = null;
let wv = null;
let params = null;
let seed = 1;
let name = '';              // the structure file this page is looking at ('' = new)
let onDisk = null;          // {json, grid, handEdited} of `name`, when opened
let mode = 'generated';     // 'disk' | 'generated'
let dirty = false;
let genTimer = 0;
let last = null;            // the current generated result (or null in disk mode)
let shownSlots = [];        // slots currently drawn (disk or generated)
let shownDim = null;
let selSlot = -1;
let list = [];              // [{name, handEdited}]
let els = {};
let widgets = [];
let undo = null;
let framed = false;

const vpm = () => (H && H.voxelsPerMetre ? H.voxelsPerMetre() : HG.DEFAULT_VOX_PER_M);
const mats = () => (H && H.materials && H.materials()) || [];

/* ===========================================================================
 * files
 * ======================================================================== */
async function listStructures() {
  const r = await fetch('/api/models', {cache: 'no-store'});
  const j = await r.json();
  const out = [];
  for (const f of j.files || []) {
    if (!(f.dir === 'structures' || f.dir === 'structures/samples')) continue;
    if (!f.name.endsWith('.struct.json')) continue;
    const n = (f.dir === 'structures' ? '' : 'samples/') + f.name.slice(0, -'.struct.json'.length);
    out.push(n);
  }
  out.sort((a, b) => (a.startsWith('samples/') - b.startsWith('samples/')) || a.localeCompare(b));
  const withFlags = [];
  for (const n of out) {
    let he = false;
    try {
      const jr = await fetch('/api/model?path=structures/' + encodeURIComponent(n).replace('%2F', '/') + '.struct.json', {cache: 'no-store'});
      if (jr.ok) he = !!(await jr.json()).handEdited;
    } catch (e) {}
    withFlags.push({name: n, handEdited: he});
  }
  return withFlags;
}

const pathOf = (n, ext) => 'structures/' + n + ext;

async function openStructure(n) {
  const jr = await fetch('/api/model?path=' + encodeURIComponent(pathOf(n, '.struct.json')).replace(/%2F/g, '/'), {cache: 'no-store'});
  if (!jr.ok) throw new Error('structures/' + n + '.struct.json: HTTP ' + jr.status);
  const json = await jr.json();
  let grid = null;
  try {
    const vr = await fetch('/api/model?path=' + encodeURIComponent(pathOf(n, '.vox')).replace(/%2F/g, '/'), {cache: 'no-store'});
    if (vr.ok) {
      const doc = VOX.readVox(new Uint8Array(await vr.arrayBuffer()));
      const model = doc && (doc.models || [])[0];
      if (model) grid = VOX.modelToGrid(model);
    }
  } catch (e) { H.toast('could not read ' + n + '.vox: ' + (e && e.message || e), true); }
  onDisk = {json, grid, handEdited: !!json.handEdited};
  name = n;
  const gen = json.generator && json.generator.tool === 'housegen' ? json.generator : null;
  params = HG.normalizeParams(gen ? gen.params : {});
  seed = gen ? (gen.seed | 0) : 1;
  els.seed.value = String(seed);
  undo.clear();
  dirty = false;
  if (H.onDirty) H.onDirty(false);
  framed = false;
  buildPanel();
  mode = grid ? 'disk' : 'generated';
  if (mode === 'disk') showDisk(); else regenerate(true);
  paintList();
}

/** Save the current GENERATED result as `n`. Refuses a hand-edited target. */
async function save(n) {
  if (!n) return;
  if (!HG.validName(n)) { H.toast('structure names are lowercase words, optionally one folder: "' + n + '"', true); return; }
  const target = list.find(s => s.name === n);
  if (target && target.handEdited) {
    const alt = nextFreeName(n);
    if (!confirm(n + ' is HAND-EDITED: its .vox is the truth now and a regenerate may not overwrite it.\n\nSave the regenerated scaffold as "' + alt + '" instead?')) return;
    n = alt;
  }
  if (mode === 'disk' && !last) regenerate(true, true);
  if (!last) return;
  let files;
  try { files = HG.buildStructureFiles(last, n, mats(), VOX); }
  catch (e) { H.toast('cannot save: ' + (e && e.message || e), true); return; }
  // Round-trip the .vox before it is written: a file that does not read back
  // is a file that silently places the wrong house.
  const back = VOX.readVox(files.vox);
  if (!back || !back.models || !back.models.length) { H.toast('the .vox did not read back — not saved', true); return; }
  const q = (ext) => '/api/model?path=' + encodeURIComponent(pathOf(n, ext)).replace(/%2F/g, '/');
  let r = await (await fetch(q('.vox'), {method: 'POST', body: files.vox})).json();
  if (!r.ok) { H.toast('save failed (' + n + '.vox): ' + (r.error || '?'), true); return; }
  r = await (await fetch(q('.struct.json'), {method: 'POST', body: files.json})).json();
  if (!r.ok) { H.toast('save failed (' + n + '.struct.json): ' + (r.error || '?'), true); return; }
  name = n;
  dirty = false;
  if (H.onDirty) H.onDirty(false);
  H.toast('saved structures/' + n + ' (.vox + .struct.json, ' + last.slots.length + ' slots)');
  await refreshList();
  try { await openStructure(n); } catch (e) {}
  if (H.saved) H.saved();
}

function nextFreeName(n) {
  const base = n.replace(/_v\d+$/, '');
  for (let k = 2; k < 1000; k++) {
    const c = base + '_v' + k;
    if (!list.some(s => s.name === c)) return c;
  }
  return base + '_new';
}

async function refreshList() {
  try { list = await listStructures(); } catch (e) { list = []; }
  paintList();
  if (H.onLibraryChanged) H.onLibraryChanged('structures');
}

/* ===========================================================================
 * generation + the two display modes
 * ======================================================================== */
function regenerate(now, silent) {
  clearTimeout(genTimer);
  const run = () => {
    const t0 = performance.now();
    try { last = HG.generateHouse(params, seed, {vpm: vpm()}); }
    catch (e) { els.stats.textContent = 'generate failed: ' + (e && e.message || e); console.error(e); return; }
    if (silent) return;
    mode = 'generated';
    const missing = [];
    const cells = UI.toViewerCells(last, mats(), missing);
    show(cells, last.dim, last.slots);
    const m = last.meta;
    const counts = Object.entries(m.counts).sort((a, b) => b[1] - a[1]).map(([k, v]) => k + ' ' + v.toLocaleString()).join(', ');
    els.stats.innerHTML =
      '<b>' + (m.voxels).toLocaleString() + '</b> voxels · <b>' + last.dim.x + '×' + last.dim.y + '×' + last.dim.z + '</b> (' +
      (last.dim.x / vpm()).toFixed(1) + ' × ' + (last.dim.y / vpm()).toFixed(1) + ' × ' + (last.dim.z / vpm()).toFixed(1) + ' m) · pitch ' + m.pitchDeg + '° · ' +
      m.rooms + ' room(s), ' + m.doors + ' door(s), ' + m.windows + ' window(s), ' + m.beds + ' bed(s), ' + m.chests + ' chest(s)' +
      (m.stair ? ', stair' : '') + (m.loft ? ', loft' : '') + ' · ' + last.slots.length + ' slots · ' +
      Math.round(performance.now() - t0) + ' ms<br><span style="color:#6f7f97">' + counts + '</span>' +
      (m.warnings.length ? '<br><span class="warn">' + m.warnings.map(esc).join(' · ') + '</span>' : '') +
      (missing.length ? '<br><span class="warn">materials.json has no ' + missing.join(', ') + '</span>' : '');
    paintMode();
  };
  if (now) run(); else genTimer = setTimeout(run, 60);
}

function showDisk() {
  const g = onDisk.grid;
  // vox.js grids are x + y*dx + z*dx*dy of ENGINE ids; WorldView wants
  // (z*ny + y)*nx + x — the same order, as it happens — of engine ids.
  const cells = new Uint16Array(g.dim.x * g.dim.y * g.dim.z);
  for (let i = 0; i < cells.length; i++) cells[i] = g.data[i];
  last = null;
  show(cells, g.dim, onDisk.json.slots || []);
  const j = onDisk.json;
  const idOf = new Map(); mats().forEach((m, i) => idOf.set(m.id, i + 1));
  const moved = Object.entries(j.materials || {}).filter(([n, id]) => idOf.get(n) !== id).map(([n, id]) => n + ' (' + id + ' → ' + (idOf.get(n) || 'missing') + ')');
  let n = 0; for (let i = 0; i < g.data.length; i++) if (g.data[i]) n++;
  els.stats.innerHTML = '<b>ON DISK</b> structures/' + esc(name) + ' · ' + n.toLocaleString() + ' voxels · ' +
    g.dim.x + '×' + g.dim.y + '×' + g.dim.z + ' · ' + (j.slots || []).length + ' slots' +
    (onDisk.handEdited ? ' · <span class="warn">HAND-EDITED: this .vox is the truth; a regenerate saves as a new name</span>' : '') +
    (moved.length ? '<br><span class="warn">materials.json renumbered since this was written: ' + moved.join(', ') + ' — re-bake, or the house wears the wrong materials</span>' : '');
  paintMode();
}

let lastCells = null;
function show(cells, dim, slots) {
  lastCells = cells;
  shownSlots = slots;
  shownDim = dim;
  if (selSlot >= slots.length) selSlot = -1;
  if (wv) {
    wv.setLocalRegions([{cells: cutaway(cells, dim), nx: dim.x, ny: dim.y, nz: dim.z, origin: [0, 0, 0]}]);
    if (!framed) { UI.frame(wv, dim, [0, 0, 0], 0.3); wv.cam.dist *= 1.1; framed = true; }
    drawOverlay();
  }
  paintSlotTable();
}

/* CUTAWAY: WorldView's slice is a per-REGION filter, and the whole house is one
 * region, so the page cuts the grid itself: everything above the chosen row
 * becomes air in the copy handed to the viewer. Never in the saved file. */
function cutaway(cells, dim) {
  const v = els.view ? els.view.value : 'whole';
  if (v === 'whole') return cells;
  const grade = gradeRow();
  const H0 = Math.round((params ? params.storeyHeight : 2.6) * vpm());
  let cut;
  if (v === 'noroof') cut = wallTopRow() + 1;
  else if (v === 'ground') cut = grade + Math.round(H0 * 0.62);
  else cut = grade + H0 + 1 + Math.round(H0 * 0.62);   // upper floor / loft
  const out = cells.slice();
  for (let z = 0; z < dim.z; z++) for (let y = Math.max(0, cut); y < dim.y; y++) {
    const row = (z * dim.y + y) * dim.x;
    out.fill(0, row, row + dim.x);
  }
  return out;
}
function gradeRow() {
  if (mode === 'generated' && last) return last.meta.grade;
  return onDisk && onDisk.json.origin ? onDisk.json.origin[1] : 2;
}
function wallTopRow() {
  if (mode === 'generated' && last) return last.meta.wallTop;
  const H0 = Math.round((params ? params.storeyHeight : 2.6) * vpm());
  return gradeRow() + (params ? params.storeys : 1) * (H0 + 1) - 2;
}

/* ===========================================================================
 * slot overlay: boxes, pins, links, and HTML labels projected with the view's MVP
 * ======================================================================== */
const KIND_COL = {
  door: [1, 0.62, 0.2, 1], bed: [0.45, 0.7, 1, 1], container: [1, 0.9, 0.3, 1],
  marker: [0.45, 1, 0.55, 1], waynode: [0.3, 0.95, 1, 1]
};
function slotColor(s, i) {
  const c = KIND_COL[s.kind] || [1, 1, 1, 1];
  return i === selSlot ? [1, 1, 1, 1] : c;
}
function drawOverlay() {
  if (!wv) return;
  if (!els.showSlots.checked) { wv.onOverlay = null; return; }
  const slots = shownSlots;
  wv.onOverlay = (push) => {
    const bx = (a, b, c) => {
      const [x0, y0, z0] = a, [x1, y1, z1] = [b[0] + 1, b[1] + 1, b[2] + 1];
      push(x0, y0, z0, x1, y0, z0, c); push(x1, y0, z0, x1, y0, z1, c); push(x1, y0, z1, x0, y0, z1, c); push(x0, y0, z1, x0, y0, z0, c);
      push(x0, y1, z0, x1, y1, z0, c); push(x1, y1, z0, x1, y1, z1, c); push(x1, y1, z1, x0, y1, z1, c); push(x0, y1, z1, x0, y1, z0, c);
      push(x0, y0, z0, x0, y1, z0, c); push(x1, y0, z0, x1, y1, z0, c); push(x1, y0, z1, x1, y1, z1, c); push(x0, y0, z1, x0, y1, z1, c);
    };
    const byName = new Map(slots.map(s => [s.name, s]));
    slots.forEach((s, i) => {
      const c = slotColor(s, i);
      const [x, y, z] = s.pos;
      // a pin: the anchor cell's floor, a stalk, and a heading tick
      push(x + 0.5, y, z + 0.5, x + 0.5, y + 12, z + 0.5, c);
      const hr = (s.yaw || 0) * Math.PI / 180;
      push(x + 0.5, y + 0.2, z + 0.5, x + 0.5 + Math.sin(hr) * 6, y + 0.2, z + 0.5 + Math.cos(hr) * 6, c);
      if (s.props && s.props.leaf) {
        bx(s.props.leaf.min, s.props.leaf.max, c);
        const h = s.props.hingeLine;
        if (h) push(h[0], s.props.leaf.min[1], h[1], h[0], s.props.leaf.max[1] + 1, h[1], [1, 0.2, 0.2, 1]);
      }
      if (s.props && s.props.box) bx(s.props.box.min, s.props.box.max, c);
      if (s.kind === 'waynode' && s.props && Array.isArray(s.props.links)) {
        for (const ln of s.props.links) {
          const o = byName.get(ln);
          if (!o || o.name < s.name) continue;      // each edge once
          push(x + 0.5, y + 1, z + 0.5, o.pos[0] + 0.5, o.pos[1] + 1, o.pos[2] + 0.5, [0.3, 0.95, 1, 0.55]);
        }
      }
    });
  };
}

/** Called every frame after the render: move the HTML labels to the projected
 *  slot anchors. Cheap: one 4x4 multiply per slot. */
function placeLabels() {
  const box = els.labels;
  if (!box || !wv || !wv.mvp) return;
  const show = els.showSlots.checked && els.showLabels.checked;
  box.style.display = show ? 'block' : 'none';
  if (!show) return;
  const cv = els.canvas;
  const w = cv.clientWidth, h = cv.clientHeight;
  const M = wv.mvp;
  while (box.children.length < shownSlots.length) {
    const d = document.createElement('div');
    d.className = CLS + 'label';
    box.append(d);
  }
  for (let i = 0; i < box.children.length; i++) {
    const d = box.children[i];
    const s = shownSlots[i];
    if (!s) { d.style.display = 'none'; continue; }
    const x = s.pos[0] + 0.5, y = s.pos[1] + 12, z = s.pos[2] + 0.5;
    const cx = M[0] * x + M[4] * y + M[8] * z + M[12];
    const cy = M[1] * x + M[5] * y + M[9] * z + M[13];
    const cw = M[3] * x + M[7] * y + M[11] * z + M[15];
    if (cw <= 0.01) { d.style.display = 'none'; continue; }
    const px = (cx / cw * 0.5 + 0.5) * w, py = (1 - (cy / cw * 0.5 + 0.5)) * h;
    if (px < -40 || py < -20 || px > w + 40 || py > h + 20) { d.style.display = 'none'; continue; }
    d.style.display = 'block';
    d.style.left = px.toFixed(0) + 'px';
    d.style.top = py.toFixed(0) + 'px';
    if (d.textContent !== s.name) d.textContent = s.name;
    const c = slotColor(s, i);
    d.style.color = 'rgb(' + (c[0] * 255 | 0) + ',' + (c[1] * 255 | 0) + ',' + (c[2] * 255 | 0) + ')';
    d.classList.toggle('sel', i === selSlot);
  }
}

function paintSlotTable() {
  const el = H.el;
  const t = els.slotTable;
  t.innerHTML = '';
  t.append(el('div', {class: CLS + 'slothead'}, el('span', {}, 'slot'), el('span', {}, 'kind'), el('span', {}, 'pos (local vox)'), el('span', {}, 'yaw')));
  shownSlots.forEach((s, i) => {
    const r = el('div', {class: CLS + 'slotrow' + (i === selSlot ? ' sel' : '')},
      el('span', {}, s.name), el('span', {}, s.kind + (s.props && s.props.tags ? ' ' + s.props.tags.join(',') : '')),
      el('span', {}, s.pos.join(', ')), el('span', {}, String(s.yaw)));
    r.title = JSON.stringify(s.props || {});
    r.addEventListener('click', () => focusSlot(i));
    t.append(r);
  });
}

function focusSlot(i) {
  selSlot = i === selSlot ? -1 : i;
  const s = shownSlots[selSlot];
  if (s && wv) {
    wv.cam.mode = 'orbit';
    wv.cam.target = [s.pos[0] + 0.5, s.pos[1] + 6, s.pos[2] + 0.5];
    wv.cam.dist = Math.max(40, Math.min(wv.cam.dist, 90));
    // Looking at something inside a house needs the roof off.
    if (els.view.value === 'whole' && s.pos[1] <= wallTopRow()) { els.view.value = s.pos[1] > gradeRow() + 10 ? 'upper' : 'ground'; if (lastCells) show(lastCells, shownDim, shownSlots); }
  }
  drawOverlay();
  paintSlotTable();
}

/* ===========================================================================
 * panel
 * ======================================================================== */
function ctx() {
  return {
    el: H.el, cls: CLS, params: () => params, widgets,
    snapshot: (k) => undo.snapshot(k),
    onChange: () => { markDirty(); regenerate(false); }
  };
}

function markDirty() {
  dirty = true;
  if (H.onDirty) H.onDirty(true);
  paintMode();
}

function enumRow(c, container, r) {
  const el = H.el;
  const sel = el('select', {class: CLS + 'num'});
  r.options.forEach(o => sel.append(el('option', {value: o}, o)));
  sel.value = params[r.k];
  sel.addEventListener('change', () => {
    if (params[r.k] === sel.value) return;
    c.snapshot(null);
    params[r.k] = sel.value;
    c.onChange(r, r.k);
  });
  const line = el('div', {class: CLS + 'row', title: r.d || ''}, el('label', {}, r.n), sel);
  container.append(line);
  widgets.push(() => { sel.value = params[r.k]; });
}

function buildPanel() {
  const col = els.sliders;
  col.innerHTML = '';
  widgets = [];
  const c = ctx();
  let sec = null;
  for (const r of HG.SCHEMA) {
    if (r.sec) { sec = UI.section(H.el, CLS, r.sec, r.note); col.append(sec.wrap); continue; }
    if (r.kind === 'enum') enumRow(c, sec.body, r);
    else if (r.kind === 'bool') UI.boolRow(c, sec.body, r);
    else UI.row(c, sec.body, r);
  }
}

function refreshWidgets() { for (const w of widgets) { try { w(); } catch (e) {} } }

function paintMode() {
  if (!els.mode) return;
  const where = name ? 'structures/' + name : 'new, unsaved';
  const badge = onDisk && onDisk.handEdited && name ? ' <span class="' + CLS + 'badge">hand-edited</span>' : '';
  els.mode.innerHTML = (mode === 'disk'
    ? 'showing the file <b>on disk</b>: ' + esc(where) + badge + ' — move any slider to regenerate'
    : 'showing the <b>generated</b> scaffold (seed ' + seed + ')' + (name ? ' for ' + esc(where) + badge : '') + (dirty ? ' · <span class="warn">unsaved</span>' : ''));
  els.save.disabled = !(dirty || mode === 'generated') || !name;
}

function paintList() {
  if (!els.list) return;
  const el = H.el;
  els.list.innerHTML = '';
  els.pick.innerHTML = '';
  els.pick.append(el('option', {value: ''}, name ? '' : '(new, unsaved)'));
  for (const s of list) {
    const b = el('button', {class: CLS + 'chip' + (s.name === name ? ' on' : ''), title: s.handEdited
      ? 'HAND-EDITED: the .vox is the truth; regenerating saves a new name' : 'generated scaffold (safe to regenerate)'},
      s.name, s.handEdited ? el('span', {class: CLS + 'badge'}, 'hand-edited') : null);
    b.addEventListener('click', () => pick(s.name));
    els.list.append(b);
    els.pick.append(el('option', {value: s.name}, s.name + (s.handEdited ? '  ✎ hand-edited' : '')));
  }
  els.pick.value = name && list.some(s => s.name === name) ? name : '';
  paintMode();
}

async function pick(n) {
  if (!n || n === name && mode === 'disk') return;
  if (dirty && !confirm('Discard unsaved changes' + (name ? ' to ' + name : '') + '?')) { els.pick.value = name; return; }
  try { await openStructure(n); } catch (e) { H.toast(String(e && e.message || e), true); }
}

function newFromPreset(p) {
  const pr = HG.PRESETS[p];
  if (!pr) return;
  if (dirty && !confirm('Discard unsaved changes' + (name ? ' to ' + name : '') + '?')) return;
  params = HG.normalizeParams(pr.params);
  seed = pr.seed;
  els.seed.value = String(seed);
  name = '';
  onDisk = null;
  undo.clear();
  framed = false;
  buildPanel();
  markDirty();
  regenerate(true);
  paintList();
  H.toast('new ' + p + ' — Save as… to keep it');
}

function esc(s) { return String(s).replace(/[&<>"]/g, (c) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c])); }

/* ===========================================================================
 * mount
 * ======================================================================== */
const CSS = UI.pageCss(PAGE, CLS, `
#${PAGE} .${CLS}viewwrap{position:relative;flex:1;min-height:0;display:flex}
#${PAGE} .${CLS}labels{position:absolute;left:0;top:0;right:0;bottom:0;pointer-events:none;overflow:hidden}
#${PAGE} .${CLS}label{position:absolute;transform:translate(-50%,-100%);font:10px/1.2 monospace;white-space:nowrap;padding:1px 3px;background:rgba(14,17,22,.72);border-radius:3px}
#${PAGE} .${CLS}label.sel{outline:1px solid #fff;z-index:2}
#${PAGE} .${CLS}slots{max-height:170px;overflow-y:auto;border:1px solid #2a3040;border-radius:6px;font:11px monospace}
#${PAGE} .${CLS}slothead,#${PAGE} .${CLS}slotrow{display:grid;grid-template-columns:minmax(0,1.6fr) minmax(0,1fr) 120px 40px;gap:6px;padding:2px 6px}
#${PAGE} .${CLS}slothead{color:#6f7f97;background:#161b26;position:sticky;top:0}
#${PAGE} .${CLS}slotrow{cursor:pointer;color:#c7d2e3}
#${PAGE} .${CLS}slotrow:hover{background:#1b2130}
#${PAGE} .${CLS}slotrow.sel{background:#2b4a6f}
#${PAGE} .${CLS}slotrow span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#${PAGE} .${CLS}list{display:flex;flex-wrap:wrap;gap:4px}
#${PAGE} .${CLS}chip{font-size:11px;padding:3px 7px;border:1px solid #2a3040;background:#12161f;color:#c7d2e3;border-radius:5px;cursor:pointer;display:flex;gap:5px;align-items:center}
#${PAGE} .${CLS}chip.on{border-color:#5aa9e6;background:#1f2a3d}
#${PAGE} .${CLS}badge{font:9px/1 monospace;color:#ffb454;border:1px solid #6b4a1a;border-radius:3px;padding:2px 3px}
#${PAGE} .${CLS}mode{font:11px/1.4 monospace;color:#9fb0c8}
#${PAGE} .${CLS}mode .warn{color:#ffb454}
#${PAGE} .${CLS}legend{font:10px monospace;color:#6f7f97;display:flex;gap:10px;flex-wrap:wrap}
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

  params = HG.normalizeParams(HG.PRESETS.cottage.params);
  seed = HG.PRESETS.cottage.seed;
  undo = UI.makeUndo({
    get: () => ({params, seed}),
    set: (o) => { params = HG.normalizeParams(o.params); seed = o.seed | 0; els.seed.value = String(seed); refreshWidgets(); markDirty(); regenerate(true); },
    toast: H.toast
  });

  els.pick = el('select', {class: CLS + 'num', style: 'width:190px', title: 'Open a structure from assets/structures/'});
  els.fromPreset = el('select', {class: CLS + 'num', style: 'width:120px', title: 'Start a new structure from a preset'});
  els.fromPreset.append(el('option', {value: ''}, 'new from…'));
  HG.PRESET_ORDER.forEach(n => els.fromPreset.append(el('option', {value: n}, n)));
  els.save = el('button', {disabled: true, title: 'Save over the open structure (refused if it is hand-edited)'}, 'Save');
  els.saveAs = el('button', {title: 'Save as a new structure: assets/structures/<name>.vox + .struct.json'}, 'Save as…');
  els.seed = el('input', {type: 'number', class: CLS + 'num', value: String(seed), style: 'width:64px', min: '0', max: '99999'});
  els.reroll = el('button', {title: 'A new seed: the same house, different small choices (braces, door offset, which end the beds go)'}, 'Reroll');
  els.undo = el('button', {title: 'Undo (Ctrl+Z)'}, '↶');
  els.redo = el('button', {title: 'Redo (Ctrl+Shift+Z)'}, '↷');
  els.view = el('select', {class: CLS + 'num', style: 'width:130px', title: 'Cut the preview away to see inside. The saved file is never cut.'});
  [['whole', 'whole house'], ['noroof', 'roof off'], ['ground', 'ground floor'], ['upper', 'upper / loft']]
    .forEach(([v, t]) => els.view.append(el('option', {value: v}, t)));
  els.showSlots = el('input', {type: 'checkbox', class: CLS + 'showslots'}); els.showSlots.checked = true;
  els.showLabels = el('input', {type: 'checkbox', class: CLS + 'showlabels'}); els.showLabels.checked = true;
  els.mode = el('div', {class: CLS + 'mode'});
  els.list = el('div', {class: CLS + 'list'});
  els.stats = el('div', {class: CLS + 'stats'}, 'loading…');
  els.sliders = el('div', {class: CLS + 'sliders'});
  els.slotTable = el('div', {class: CLS + 'slots'});
  els.canvas = el('canvas', {class: CLS + 'view'});
  els.labels = el('div', {class: CLS + 'labels'});

  els.pick.addEventListener('change', () => pick(els.pick.value));
  els.fromPreset.addEventListener('change', () => { const p = els.fromPreset.value; els.fromPreset.value = ''; newFromPreset(p); });
  els.save.addEventListener('click', () => { if (name) save(name); else els.saveAs.click(); });
  els.saveAs.addEventListener('click', () => {
    const def = name ? (onDisk && onDisk.handEdited ? nextFreeName(name) : name) : '';
    const n = prompt('Structure name — a file under assets/structures/ (lowercase, e.g. "harrowby_smithy" or "samples/barn"):', def);
    if (n) save(n.trim().toLowerCase().replace(/[^a-z0-9_/]/g, '_'));
  });
  els.seed.addEventListener('change', () => {
    const v = Math.max(0, parseInt(els.seed.value, 10) || 0);
    if (v === seed) return;
    undo.snapshot(null); seed = v; markDirty(); regenerate(true);
  });
  els.reroll.addEventListener('click', () => {
    undo.snapshot(null);
    seed = (HG.hashN(seed, Date.now() & 0xffff) % 99999) | 0;
    els.seed.value = String(seed);
    markDirty(); regenerate(true);
  });
  els.undo.addEventListener('click', () => undo.undo());
  els.redo.addEventListener('click', () => undo.redo());
  els.view.addEventListener('change', () => { if (lastCells) show(lastCells, shownDim, shownSlots); });
  els.showSlots.addEventListener('change', () => drawOverlay());

  document.addEventListener('keydown', (e) => {
    if (!root.classList.contains('active') || !H.isVisible()) return;
    if (!(e.ctrlKey || e.metaKey)) return;
    const k = (e.key || '').toLowerCase();
    if (k === 'z' && !e.shiftKey) { e.preventDefault(); e.stopImmediatePropagation(); undo.undo(); }
    else if ((k === 'z' && e.shiftKey) || k === 'y') { e.preventDefault(); e.stopImmediatePropagation(); undo.redo(); }
  }, true);

  const legend = el('div', {class: CLS + 'legend'},
    ...Object.entries(KIND_COL).map(([k, c]) => el('span', {style: 'color:rgb(' + (c[0] * 255 | 0) + ',' + (c[1] * 255 | 0) + ',' + (c[2] * 255 | 0) + ')'}, '■ ' + k)),
    el('span', {style: 'color:#ff3333'}, '| hinge'));

  root.append(
    el('div', {class: CLS + 'left'},
      el('div', {class: CLS + 'bar'}, el('span', {class: 'hint'}, 'structure'), els.pick, els.save, els.saveAs),
      el('div', {class: CLS + 'bar'}, els.fromPreset, el('span', {class: 'hint'}, 'seed'), els.seed, els.reroll, els.undo, els.redo),
      els.list,
      els.sliders),
    el('div', {class: CLS + 'right'},
      el('div', {class: CLS + 'bar'}, el('span', {class: 'hint'}, 'view'), els.view,
         el('label', {class: 'hint', style: 'display:flex;align-items:center;gap:4px'}, els.showSlots, 'slots'),
         el('label', {class: 'hint', style: 'display:flex;align-items:center;gap:4px'}, els.showLabels, 'labels'),
         legend),
      els.mode,
      el('div', {class: CLS + 'viewwrap'}, els.canvas, els.labels),
      els.stats,
      els.slotTable));

  const active = () => root.classList.contains('active') && H.isVisible();
  wv = UI.makeView(els.canvas, mats(), active,
                   (e) => { els.stats.textContent = 'the 3D preview needs WebGL2: ' + (e && e.message || e); });
  if (wv) {
    const loop = () => { if (active()) placeLabels(); requestAnimationFrame(loop); };
    requestAnimationFrame(loop);
  }
  buildPanel();
}

let activated = false;
export function activate() {
  if (activated) return;
  activated = true;
  refreshList().then(() => {
    // Open on the first sample if there is one; otherwise a fresh cottage.
    const first = list.find(s => s.name.startsWith('samples/')) || list[0];
    if (first) return openStructure(first.name);
    regenerate(true);
    paintList();
  }).catch(e => { els.stats.textContent = 'could not list structures: ' + (e && e.message || e); regenerate(true); });
}

export function saveFromHost() { if (name) save(name); else if (els.saveAs) els.saveAs.click(); }
export function isDirty() { return dirty; }
export function currentName() { return name; }
/** Open a named structure (a deep link, or a test). Always re-reads the disk:
 *  someone else may have written it since. */
export async function open(n) {
  if (dirty && !confirm('Discard unsaved changes' + (name ? ' to ' + name : '') + '?')) return;
  await refreshList();
  await openStructure(n);
}

// test seams (assets/environment_test.html)
export function _view() { return wv; }
export function _last() { return last; }
export function _mode() { return mode; }
export function _slots() { return shownSlots; }
export function _undoDepth() { return undo ? undo.depth() : 0; }
export function _list() { return list; }
export function _save(n) { return save(n); }
export function _params() { return params; }
