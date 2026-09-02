/* biome.js — the Biome page of the Environment tab.
 *
 * ONE BIOME, ALL ITS STACKS. The left column is the biome file
 * (assets/biomes/<name>.json) as sections — climate & identity, terrain
 * overrides, ground cover, trees, water, caves — and the right column is a
 * SWATCH: a square of the biome composed by biomegen.js from the same files
 * the engine will read (tree species from assets/trees/, water presets from
 * assets/water/), drawn through WorldView.
 *
 * WHAT IS LIVE AND WHAT IS SCAFFOLD, said plainly because the page says it too:
 *
 *   LIVE   tree species + weights. Saving a biome rewrites placement.biomes in
 *          every species file and "Sync atlas" re-bakes the ones that changed,
 *          which is what the engine reads. The world hash moves when it does.
 *   LIVE   the biome BAND strip — the three worldgen thresholds that decide
 *          which biome a column is. Dragging a divider edits tuning.json.
 *   AUTHORED, NOT YET READ BY THE ENGINE   cover plants, water features, cave
 *          features, terrain overrides, tree row conditions, climate
 *          coordinates. The `biomes` gate validates every name they use; the
 *          swatch composes them; worldgen still runs its hardcoded per-biome
 *          blocks. docs/PLAN_biomes.md §5 is the wiring plan.
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
let view = {trees: true, water: true, cover: true, showcase: true, sizeM: 24};
let lastSwatch = null;

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
function regenerate(now) {
  clearTimeout(genTimer);
  const run = () => {
    if (!biome) return;
    els.stats.innerHTML = '<span class="warn">composing…</span>';
    // Yield a frame so the status paints before a multi-second tree bake.
    setTimeout(() => {
      const t0 = performance.now();
      let res;
      try {
        res = BG.generateSwatch(biome, libs, seed, {
          vpm: vpm(), treeCache, sizeM: view.sizeM, showcase: view.showcase,
          noTrees: !view.trees, noWater: !view.water, noCover: !view.cover
        });
      } catch (e) {
        els.stats.textContent = 'swatch failed: ' + (e && e.message || e);
        console.error(e);
        return;
      }
      lastSwatch = res;
      const mats = H.materials() || [];
      const missing = [];
      if (wv) {
        wv.setLocalRegions([{cells: UI.toViewerCells(res, mats, missing), nx: res.dim.x, ny: res.dim.y,
                             nz: res.dim.z, origin: [0, 0, 0]}]);
        if (!framed) { UI.frame(wv, res.dim, [0, 0, 0], 0.3); wv.cam.pitch = -0.6; framed = true; }
      }
      const m = res.meta;
      const fmt = (o) => Object.entries(o).map(([k, v]) => k + ' ' + v).join(', ') || '—';
      els.stats.innerHTML =
        `<b>${res.dim.x}×${res.dim.y}×${res.dim.z}</b> (${view.sizeM} m) · ${m.voxels.toLocaleString()} voxels · ` +
        `${Math.round(performance.now() - t0)} ms` +
        (m.clipped ? ' · <span class="warn">HEIGHT CLIPPED</span>' : '') +
        `<br>trees: ${fmt(m.trees)}` + (m.skipped.trees ? ` <span class="warn">(${m.skipped.trees} gated out)</span>` : '') +
        `<br>water: ${fmt(m.water)}` + (view.showcase && m.waterBodies ? ' <span class="warn">(showcase — one of each, not true rarity)</span>' : '') +
        `<br>cover: ${fmt(m.cover)}` +
        (missing.length ? '<br><span class="warn">materials.json has no ' + missing.join(', ') + '</span>' : '');
      validateInto(els.valid);
    }, 0);
  };
  if (now) run(); else genTimer = setTimeout(run, 400);
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
  {k: 'patchThreshold', n: 'patch >', min: 0, max: 255, step: 1, title: 'Only where this row’s patch noise (0..255) is above this. 0 = everywhere.'}
];

/** The conditions line under a feature row: six small number boxes. */
function conditionsLine(item) {
  const el = H.el;
  const line = el('div', {class: CLS + 'cond'});
  for (const f of COND_FIELDS) {
    const inp = el('input', {type: 'number', class: CLS + 'num', value: item.conditions[f.k],
                             min: f.min, max: f.max, step: f.step, title: f.title});
    inp.addEventListener('change', () => {
      undo.snapshot(null);
      item.conditions[f.k] = +inp.value;
      markDirty(); regenerate(false);
    });
    line.append(el('label', {title: f.title}, f.n), inp);
  }
  return line;
}

/**
 * A reorderable stack of feature rows.
 *   spec = {items(): array, make(): item, cols: [{k, type, options(), min, max, step, title, w}],
 *           derived(item) -> string, addLabel, hint}
 */
function stackEditor(body, spec) {
  const el = H.el;
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
        if (c.label) cells.push(el('span', {class: CLS + 'unit'}, c.label));
        cells.push(inp);
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
        condEl = conditionsLine(it);
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
 * the biome BAND strip — the engine's three thresholds, live
 * ======================================================================== */
function bandStrip(body) {
  const el = H.el;
  const T = H.tuning && H.tuning();
  const wrap = el('div', {class: CLS + 'band'});
  const info = el('div', {class: CLS + 'hint'});
  body.append(wrap, info);
  if (!T || !T.tune || !T.tune.worldgen) {
    info.textContent = 'No tuning.json loaded — the band strip needs it.';
    return;
  }
  const W = T.tune.worldgen;
  const keys = ['meadowThreshold', 'pineThreshold', 'desertThreshold'];
  const segs = [['meadow', '#7fbf5a'], ['forest', '#3f8f4a'], ['pine', '#2f6f5f'], ['desert', '#d9b866']];
  const paint = () => {
    wrap.innerHTML = '';
    const th = keys.map(k => Math.max(0, Math.min(255, W[k] | 0)));
    const edges = [0, th[0], th[1], th[2], 255];
    for (let i = 0; i < 4; i++) {
      const w = Math.max(0, edges[i + 1] - edges[i]) / 255 * 100;
      const seg = el('div', {class: CLS + 'seg' + (segs[i][0] === biomeName ? ' cur' : ''),
                             style: 'width:' + w + '%;background:' + segs[i][1],
                             title: segs[i][0] + ': biome noise ' + edges[i] + '..' + edges[i + 1]},
                     segs[i][0]);
      seg.addEventListener('click', () => { if (H.openPage) H.openPage('biome', segs[i][0]); });
      wrap.append(seg);
      if (i < 3) {
        const grip = el('div', {class: CLS + 'grip', title: keys[i] + ' = ' + th[i] + ' (drag)'});
        grip.style.left = (edges[i + 1] / 255 * 100) + '%';
        let dragging = false;
        grip.addEventListener('mousedown', (e) => { dragging = true; e.preventDefault(); });
        window.addEventListener('mousemove', (e) => {
          if (!dragging) return;
          const r = wrap.getBoundingClientRect();
          let v = Math.round((e.clientX - r.left) / r.width * 255);
          const lo = i > 0 ? (W[keys[i - 1]] | 0) + 1 : 0, hi = i < 2 ? (W[keys[i + 1]] | 0) - 1 : 255;
          v = Math.max(lo, Math.min(hi, v));
          if (v === (W[keys[i]] | 0)) return;
          W[keys[i]] = v;
          T.touchTune();
          paint();
        });
        window.addEventListener('mouseup', () => { dragging = false; });
        wrap.append(grip);
      }
    }
    const pct = (a, b) => ((b - a) / 255 * 100).toFixed(0) + '%';
    info.textContent = 'Share of the biome noise: meadow ' + pct(0, th[0]) + ' · forest ' + pct(th[0], th[1]) +
        ' · pine ' + pct(th[1], th[2]) + ' · desert ' + pct(th[2], 255) +
        '. Drag a divider to move worldgen.' + keys.join('/') + ' (writes tuning.json; regen world in game).';
  };
  paint();
  widgets.push(paint);
}

/* ===========================================================================
 * terrain overrides — inherit-or-override over the worldgen rows
 * ======================================================================== */
function terrainSection(body) {
  const el = H.el;
  const T = H.tuning && H.tuning();
  const tab = T && T.schema ? T.schema.find(t => t.id === 'worldgen') : null;
  if (!tab || !T.tune || !T.tune.worldgen) {
    body.append(el('div', {class: CLS + 'hint'}, 'Needs tuning.json and the worldgen schema.'));
    return;
  }
  const filter = el('input', {type: 'text', class: CLS + 'num', placeholder: 'filter knobs…', style: 'text-align:left'});
  const onlyOver = el('input', {type: 'checkbox'});
  const list = el('div', {});
  body.append(el('div', {class: CLS + 'bar'}, filter,
                 el('label', {class: CLS + 'hint', style: 'display:flex;gap:4px;align-items:center'}, onlyOver, 'overridden only')),
              list);
  const render = () => {
    list.innerHTML = '';
    const q = filter.value.trim().toLowerCase();
    const ov = biome.terrain.overrides;
    let n = 0;
    for (const pr of tab.params) {
      if (pr.sec || pr.type || pr.bool) continue;
      const hit = !q || pr.k.toLowerCase().includes(q) || (pr.n || '').toLowerCase().includes(q);
      const has = Object.prototype.hasOwnProperty.call(ov, pr.k);
      if (!hit || (onlyOver.checked && !has)) continue;
      n++;
      const inherited = T.tune.worldgen[pr.k];
      const chk = el('input', {type: 'checkbox', title: 'override this knob for this biome'});
      chk.checked = has;
      const val = el('input', {type: 'number', class: CLS + 'num', min: pr.min, max: pr.max, step: pr.step,
                               value: has ? ov[pr.k] : inherited, disabled: !has});
      const tag = el('span', {class: CLS + 'unit' + (has ? ' over' : '')},
                     has ? 'overrides ' + inherited : 'inherited from Worldgen');
      chk.addEventListener('change', () => {
        undo.snapshot(null);
        if (chk.checked) ov[pr.k] = inherited; else delete ov[pr.k];
        markDirty(); render();
      });
      val.addEventListener('change', () => {
        undo.snapshot(null);
        ov[pr.k] = pr.int ? Math.round(+val.value) : +val.value;
        markDirty(); render();
      });
      list.append(el('div', {class: CLS + 'row', title: pr.d || ''}, el('label', {}, pr.n || pr.k), val, chk, tag));
    }
    if (!n) list.append(el('div', {class: CLS + 'hint'}, onlyOver.checked ? 'No overrides yet.' : 'No knob matches.'));
  };
  filter.addEventListener('input', render);
  onlyOver.addEventListener('change', render);
  render();
  widgets.push(render);
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
                     engine ? 'An ENGINE biome: worldgen id ' + BG.ENGINE_BIOMES.indexOf(biome.name) +
                              '. The band strip is live tuning; temperature/moisture are the coordinates the ' +
                              'planned climate grid will select by (PLAN_biomes.md §4) and are authored but not read.'
                            : 'NOT an engine biome yet: worldgen has four hardcoded ids. This file is authored, ' +
                              'validated and previewed, and will be selectable when biomeAt reads a table.');
  const disp = el('input', {type: 'text', class: CLS + 'num', value: biome.displayName, style: 'text-align:left'});
  disp.addEventListener('change', () => { undo.snapshot(null); biome.displayName = disp.value; markDirty(); });
  widgets.push(() => { disp.value = biome.displayName; });
  s.body.append(el('div', {class: CLS + 'row'}, el('label', {}, 'display name'), disp));
  s.body.append(el('div', {class: CLS + 'hint', style: 'margin:6px 0 2px'}, 'Where each engine biome sits on the biome noise'));
  bandStrip(s.body);
  UI.row(C, s.body, {k: 'temperature', n: 'temperature', min: 0, max: 1, step: 0.01,
                     d: '0 cold .. 1 hot. The planned climate field is seed-independent with a fixed compass: colder toward +Z.'}, 'climate');
  UI.row(C, s.body, {k: 'moisture', n: 'moisture', min: 0, max: 1, step: 0.01,
                     d: '0 arid .. 1 wet. Drier toward +X in the planned field. Also the natural knob for how FULL this biome’s basins are.'}, 'climate');
  const notes = el('textarea', {class: CLS + 'num', rows: '2', style: 'text-align:left;resize:vertical'});
  notes.value = biome.climate.notes || '';
  notes.addEventListener('change', () => { undo.snapshot(null); biome.climate.notes = notes.value; markDirty(); });
  widgets.push(() => { notes.value = biome.climate.notes || ''; });
  s.body.append(el('div', {class: CLS + 'row'}, el('label', {}, 'notes'), notes));
  col.append(s.wrap);

  // ---- terrain ------------------------------------------------------------------
  s = UI.section(el, CLS, 'Terrain overrides',
                 'The Worldgen tab’s knobs, per biome. Tick a knob to give this biome its own value; ' +
                 'untouched knobs inherit. AUTHORED, NOT YET READ: the plan of record (PLAN_terrain_overhaul §G) ' +
                 'is one height function whose octave amplitudes a smooth uplift field modulates, and these ' +
                 'are the per-biome inputs to that.', {closed: true});
  terrainSection(s.body);
  col.append(s.wrap);

  // ---- ground cover ---------------------------------------------------------------
  s = UI.section(el, CLS, 'Ground cover',
                 'The skin the ground wears and the small plants on it. Each plant row is 1-in-N ' +
                 'columns, gated by the shared patch mask and its own conditions.');
  UI.matRow(C, s.body, {k: 'skin', n: 'ground skin', d: 'Topmost cell. worldgen paints desert sand 4 deep, everything else grass 1 deep.'}, 'cover', mats, {filter: isSolid});
  UI.row(C, s.body, {k: 'skinDepth', n: 'skin depth (cells)', min: 1, max: 8, step: 1, int: true, d: ''}, 'cover');
  UI.matRow(C, s.body, {k: 'subsoil', n: 'subsoil', d: ''}, 'cover', mats, {filter: isSolid});
  UI.row(C, s.body, {k: 'threshold', n: 'patch threshold', min: 0, max: 255, step: 1, int: true,
                     d: 'Plants only grow where the patch noise (0..255) is above this. Higher leaves more open ground between stands — what makes a desert read as arid rather than as a dry lawn.'}, 'cover.patch');
  UI.row(C, s.body, {k: 'cellLog2', n: 'patch size (log2)', min: 2, max: 9, step: 1, int: true,
                     d: 'Patch cell as a power of two, in cells.'}, 'cover.patch');
  s.body.append(el('div', {class: CLS + 'hint', style: 'margin-top:6px'}, 'Plants'));
  stackEditor(s.body, {
    items: () => biome.cover.plants,
    make: () => ({material: 'grass_tuft', head: '', chance: 12, height: 0.2, conditions: BG.defaultConditions()}),
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
  const T = H.tuning && H.tuning();
  const treeTileM = T && T.tune && T.tune.worldgen ? (T.tune.worldgen.treeTile / vpm()) : biome.trees.tile;
  s = UI.section(el, CLS, 'Trees',
                 'Which species, at what weight. LIVE: the weights are what the tree atlas bakes — saving ' +
                 'writes placement.biomes into each species file, and "Sync atlas" re-bakes the changed ones. ' +
                 'Tile spacing and density are worldgen.treeTile / treeChance' + cap(biome.name) +
                 ' today (the Worldgen tab); the row here is the value this biome will carry when worldgen reads the table.');
  UI.row(C, s.body, {k: 'tile', n: 'tile (m)', min: 1.6, max: 51.2, step: 0.8, u: 'm',
                     d: 'Metres between candidate trunk sites: at most one tree per tile. Engine today: worldgen.treeTile = ' + treeTileM.toFixed(1) + ' m.'}, 'trees');
  UI.row(C, s.body, {k: 'density', n: 'density (%)', min: 0, max: 100, step: 1, int: true, u: '%',
                     d: 'Percent of tiles that grow a tree.'}, 'trees');
  const dstat = el('div', {class: CLS + 'derived'});
  const paintD = () => {
    const d = BG.densityStats(biome.trees.tile, biome.trees.density);
    dstat.textContent = '≈ ' + d.perHa.toFixed(0) + ' trees per hectare · 1 tree in ' + (d.oneIn ? d.oneIn.toFixed(1) : '∞') + ' tiles';
  };
  paintD(); widgets.push(paintD);
  s.body.append(dstat);
  const total = () => biome.trees.species.reduce((a, r) => a + (r.weight | 0), 0);
  stackEditor(s.body, {
    items: () => biome.trees.species,
    make: () => ({species: Object.keys(libs.trees)[0] || 'oak', weight: 10, conditions: BG.defaultConditions()}),
    cols: [
      {k: 'species', type: 'select', options: () => Object.keys(libs.trees).sort(), title: 'a file in assets/trees/'},
      {k: 'weight', label: 'weight', min: 0, max: 100, step: 1, w: '52px', title: 'Relative weight against the other rows. 0 = never here.'}
    ],
    derived: (it) => {
      const t = total();
      const sp = libs.trees[it.species];
      const spar = sp && sp.placement && sp.placement.sparsity > 1 ? ' ÷ sparsity ' + sp.placement.sparsity : '';
      const share = t ? (100 * it.weight / t).toFixed(0) : 0;
      const d = BG.densityStats(biome.trees.tile, biome.trees.density);
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
                 'AUTHORED, NOT YET READ: worldgen still grows its one parabolic tarn from the pond* rows; ' +
                 'the swatch composes these.');
  stackEditor(s.body, {
    items: () => biome.water.features,
    make: () => {
      const nm = Object.keys(libs.water)[0] || 'tarn';
      const p = libs.water[nm] && libs.water[nm].placement || {};
      return {preset: nm, tile: p.tile || 44.8, rarity: p.rarity || 4,
              conditions: Object.assign(BG.defaultConditions(), {maxSlope: p.maxSlope || 96})};
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
                 'SCAFFOLD. worldgen carves two bands world-wide (caveThreshold1 near the surface, ' +
                 'caveThreshold2 deep; the Caves page has the live knobs). A row here is the per-biome ' +
                 'version those knobs will become; the swatch does not cut caves (it shows the surface).', {closed: true});
  stackEditor(s.body, {
    items: () => biome.caves.features,
    make: () => ({preset: 'near_surface', threshold: 150, rarity: 1, conditions: BG.defaultConditions()}),
    cols: [
      {k: 'preset', type: 'select', options: () => ['near_surface', 'deep'], title: 'which band'},
      {k: 'threshold', label: 'noise >', min: 0, max: 255, step: 1, w: '50px', title: 'cave noise threshold (0..255); higher = fewer caves'},
      {k: 'rarity', label: '1 in', min: 0, max: 16, step: 1, w: '44px', title: 'one region in N has this band at all'}
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

function cap(s) { return s ? s[0].toUpperCase() + s.slice(1) : s; }

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
  H.toast('saved biomes/' + clean + '.json' + (BG.ENGINE_BIOMES.includes(clean) ? ' — Sync atlas if tree weights changed' : ''));
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
#${PAGE} .${CLS}stackrow select{flex:1 1 90px;min-width:70px}
#${PAGE} .${CLS}stackrow button{padding:0 4px;font-size:10px;line-height:18px}
#${PAGE} .${CLS}condbtn.set{color:#ffb454}
#${PAGE} .${CLS}cond{display:grid;grid-template-columns:repeat(6,auto 1fr);gap:2px 4px;align-items:center;margin:3px 0 2px;font-size:10px}
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
  els.size = el('select', {class: CLS + 'num', style: 'width:70px', title: 'swatch side'});
  [16, 24, 32].forEach(m => els.size.append(el('option', {value: m}, m + ' m')));
  els.size.value = '24';
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
         tog('showcase', 'showcase water', 'ON: one of every water row, centred — judge the shoreline. OFF: true tile + rarity, which on a 24 m swatch is usually nothing.')),
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
  if (biome && !builtWithTuning && H.tuning && H.tuning() && H.tuning().tune) buildPanel();
}

// test seams
export function _view() { return wv; }
export function _biome() { return biome; }
export function _undoDepth() { return undo ? undo.depth() : 0; }
export function _swatch() { return lastSwatch; }
