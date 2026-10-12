/* ui_kit.js — the KIT page of the Dungeons tab (docs/PLAN_dungeons.md §5.1):
 * the piece designer. Pick a style (kit) and a piece type, drag its sliders
 * (built from the type's SCHEMA: the kit's own rows plus the universal ones),
 * pick a preset, reroll the seed, and see THE PIECE ALONE in the real engine:
 * the program goes to the voxel server as `DUNGEON ISOLATE` (a cut-away block
 * of solid rock far from every map feature), and the view shows it x-rayed
 * with every socket drawn as a coloured frame and labelled. "Save as preset"
 * writes assets/dungeons/presets/<kit>.json (the kit modules stay pure code;
 * user presets are merged into the list here and in the stitcher).
 *
 * Props: the kit's prop generators, previewed LOCALLY (bakeProp's cells drawn
 * as an authored region -- a prop is a template, not worldgen).
 */
'use strict';
import * as DG from './dungeongen.js';
import {h, toast, api, Previewer, View3D, sliderPanel, drawSocket, socketCentre, COL, fmtM} from './ui_core.js';

export function createKitPage(T) {
  const st = {kit: 'mines', type: null, params: null, seed: 1, variant: null, preset: '', userPresets: {},
              program: null, info: null, sel: null, prop: null, propParams: null, propSeed: 1, showWay: true, showSug: true};
  const pv = new Previewer('kit');
  const view = new View3D({draw, onClick});
  let propView = null, propHost = null;
  const ui = {};
  let timer = 0, dragging = false;

  /* ---- layout ---- */
  function build() {
    ui.kitSel = h('select', {onchange: () => setKit(ui.kitSel.value)}, DG.STYLES.map(k => h('option', {value: k}, DG.KITS[k].label || k)));
    ui.varSel = h('select', {title: 'the palette variant: which materials the slots resolve to', onchange: () => { st.variant = ui.varSel.value; regen(true); }});
    ui.seed = h('input', {type: 'number', value: 1, title: 'the piece seed (0 = derived)', onchange: () => { st.seed = (+ui.seed.value >>> 0) || 1; regen(true); }});
    ui.preset = h('select', {title: 'presets of this type (★ = yours, assets/dungeons/presets/)', onchange: () => applyPreset(ui.preset.value)});
    ui.status = h('span', {class: 'dg-bar st'}, '');
    const bar = h('div', {class: 'dg-bar'},
      h('label', {}, 'style ', ui.kitSel), h('label', {}, 'palette ', ui.varSel),
      h('label', {}, 'preset ', ui.preset),
      h('button', {onclick: savePreset, title: 'write the current sliders as a named preset of this type'}, 'Save as preset'),
      h('label', {}, 'seed ', ui.seed),
      h('button', {onclick: () => { st.seed = (Math.imul(st.seed || 1, 1103515245) + 12345) >>> 0 || 1; ui.seed.value = st.seed; regen(true); }, title: 'a new seed: the same sliders, other seeded choices'}, 'Reroll'),
      h('button', {onclick: () => { if (st.program) view.frameBox(st.program.bounds.min, st.program.bounds.max); }}, 'Frame'),
      h('span', {class: 'grow'}), ui.status);
    ui.types = h('div', {});
    ui.props = h('div', {});
    const side = h('div', {class: 'dg-side'}, h('h4', {}, 'Piece types'), ui.types, h('h4', {}, 'Props'), ui.props);
    ui.sliders = h('div', {});
    ui.sockets = h('div', {});
    ui.propPanel = h('div', {});
    ui.insp = h('div', {class: 'dg-insp'}, h('h4', {}, 'Sockets'), ui.sockets, h('h4', {}, 'Sliders'), ui.sliders, ui.propPanel);
    ui.vbar = h('div', {class: 'dg-vbar'},
      viewBtn('x-ray', 'xray', 'the piece alone: rock is see-through, the carved space shows its own surfaces'),
      viewBtn('solid', 'solid', 'the cut-away rock block the piece is carved into'),
      h('span', {}, ' · '),
      chk('waynodes', 'showWay', 'the walking graph the piece generates'),
      chk('suggestions', 'showSug', 'suggested props / refs (lights, chests, doors...)'),
      h('span', {class: 'grow'}),
      h('span', {class: 'dg-hint', style: 'margin:0'}, 'drag = orbit · wheel = zoom · WASD/QE = move · dbl-click = orbit point'));
    const center = h('div', {class: 'dg-center'}, view.el, ui.vbar);
    T.page.append(bar, h('div', {class: 'dg-main dg-kitmain'}, side, center, ui.insp));
    ui.kitSel.value = st.kit;
    syncView();
  }
  function viewBtn(label, mode, title) {
    const b = h('button', {title, 'data-mode': mode, onclick: () => { st.viewMode = mode; syncView(); }}, label);
    return b;
  }
  function chk(label, key, title) {
    const c = h('input', {type: 'checkbox', checked: st[key], onchange: () => { st[key] = c.checked; view.dirty = true; }});
    return h('label', {title}, c, label);
  }
  function syncView() {
    for (const b of ui.vbar.querySelectorAll('button[data-mode]')) b.classList.toggle('on', b.dataset.mode === (st.viewMode || 'xray'));
    if (!st.program) return;
    const b = st.program.bounds;
    view.setClip(st.viewMode === 'solid' ? 'solid' : 'xray', {min: b.min.map(v => v - 1), max: b.max.map(v => v + 1)}, -Infinity, Infinity);
    if (st.viewMode === 'solid') view.wv && (view.wv.view.clip = null);
  }

  /* ---- data ---- */
  function kitPresets(kit, type) {
    const T0 = DG.getType(kit, type);
    const out = Object.keys(T0.PRESETS).map(n => ({name: n, params: T0.PRESETS[n].params, user: false}));
    const u = ((st.userPresets[kit] || {})[type]) || {};
    for (const n of Object.keys(u).sort()) out.push({name: n, params: u[n].params, user: true});
    return out;
  }
  function setKit(kit) {
    st.kit = kit;
    const K = DG.getKit(kit);
    ui.varSel.replaceChildren(...Object.keys(K.PALETTES).map(v => h('option', {value: v}, v)));
    st.variant = K.DEFAULT_PALETTE; ui.varSel.value = st.variant;
    renderTypes();
    renderProps();
    setType(DG.listTypes(kit)[0].type);
  }
  function renderTypes() {
    ui.types.replaceChildren(...DG.listTypes(st.kit).map(t => h('div', {class: 'dg-item' + (t.type === st.type ? ' sel' : ''), onclick: () => setType(t.type), 'data-type': t.type, title: t.type},
      t.label, t.surface ? h('span', {class: 'tag'}, 'surface') : h('span', {class: 'tag', title: t.presets.length + ' presets'}, t.presets.length + 'p'))));
  }
  function setType(type, params) {
    st.type = type;
    st.params = params || DG.presetParams(st.kit, type, null);
    st.preset = '';
    renderTypes();
    renderPresets();
    renderSliders();
    regen(true);
  }
  function renderPresets() {
    const ps = kitPresets(st.kit, st.type);
    ui.preset.replaceChildren(h('option', {value: ''}, '(defaults)'), ...ps.map(p => h('option', {value: (p.user ? 'u:' : 'k:') + p.name}, (p.user ? '★ ' : '') + p.name)));
    ui.preset.value = st.preset;
  }
  function applyPreset(v) {
    st.preset = v;
    if (!v) st.params = DG.presetParams(st.kit, st.type, null);
    else {
      const p = kitPresets(st.kit, st.type).find(x => (x.user ? 'u:' : 'k:') + x.name === v);
      st.params = DG.getType(st.kit, st.type).normalize(p ? p.params : {});
    }
    renderSliders();
    regen(true);
  }
  function renderSliders() {
    const T0 = DG.getType(st.kit, st.type);
    const sp = sliderPanel(T0.SCHEMA, st.params, (k, v, final) => {
      st.params = T0.normalize(Object.assign({}, st.params, {[k]: v}));
      if (!final && !dragging) { dragging = true; view.coarse(true); }
      if (final && dragging) { dragging = false; view.coarse(false); }
      regen(final);
    }, {closed: ['Surface', 'Age', 'Light', 'Join']});
    ui.sliders.replaceChildren(h('div', {class: 'dg-hint'}, T0.label + ' (' + st.kit + '/' + st.type + ', v' + T0.version + ')' + (T0.surface ? ' — a SURFACE piece: it breaks the ground on purpose' : '')), sp.root);
  }
  async function savePreset() {
    const name = (prompt('Preset name for ' + st.kit + '/' + st.type + ' (letters, digits, _ -, space):', st.preset ? st.preset.slice(2) : 'my ' + st.type) || '').trim();
    if (!name) return;
    try {
      const j = await api.preset({kit: st.kit, type: st.type, name, params: st.params});
      st.userPresets[st.kit] = j.presets;
      st.preset = 'u:' + name;
      renderPresets();
      toast('saved preset "' + name + '" (assets/dungeons/presets/' + st.kit + '.json)');
      T.presetsChanged(st.userPresets);
    } catch (e) { toast('preset: ' + e.message, true); }
  }

  /* ---- the program + preview ---- */
  function dungeonOf() {
    const d = DG.newDungeon({name: 'kit preview', style: st.kit, seed: 1});
    d.palette = {variant: st.variant};
    d.pieces.push(DG.newPiece(d, {kit: st.kit, type: st.type, params: st.params, seed: st.seed || 1}));
    return d;
  }
  function regen(final) {
    clearTimeout(timer);
    timer = setTimeout(() => run(final), final ? 0 : 60);
  }
  async function run(final) {
    if (!T.isActive()) return;
    const t0 = performance.now();
    let prog;
    try { prog = DG.compile(dungeonOf(), {id: '_kit_preview'}); }
    catch (e) { ui.status.textContent = 'generate failed: ' + e.message; return; }
    const ms = performance.now() - t0;
    st.program = prog;
    renderSockets();
    ui.status.textContent = prog.prims.length + ' prims · ' + prog.props.instances.length + ' props · ' + prog.sockets.length + ' sockets · ' +
      fmtM(prog.bounds.max[0] - prog.bounds.min[0] + 1) + ' × ' + fmtM(prog.bounds.max[1] - prog.bounds.min[1] + 1) + ' × ' + fmtM(prog.bounds.max[2] - prog.bounds.min[2] + 1) +
      ' · generate ' + ms.toFixed(0) + ' ms' + (prog.errors.length ? ' · ' + prog.errors.length + ' ERROR(S): ' + prog.errors[0].msg : '');
    if (!view.ensure()) { ui.status.textContent += ' · ' + view.error; return; }
    const slow = setTimeout(() => T.setStatus('installing the piece in the voxel server: it is waiting for the machine-wide GPU lock (another sandvox run holds it)'), 1500);
    try {
      const r = await pv.install(prog, {mode: 'isolate'});
      clearTimeout(slow);
      if (!r) return;               // superseded by a newer move
      const moved = view.setFrame(r.info.origin, r.info.yaw || 0);
      view.setRoi(prog.bounds.min, prog.bounds.max, 24);
      syncView();
      if (moved || !st.framed) { view.invalidate(); view.frameBox(prog.bounds.min, prog.bounds.max); st.framed = true; }
      else view.refresh(r.boxes);
      view.dirty = true;
      T.setStatus('kit preview: DUNGEON ISOLATE at ' + r.info.origin.join(', '));
    } catch (e) {
      clearTimeout(slow);
      view.extraStatus = 'preview: ' + e.message;
      T.setStatus('preview failed: ' + e.message, true);
    }
  }

  function renderSockets() {
    const p = st.program;
    ui.sockets.replaceChildren(...p.sockets.map(s => h('div', {class: 'dg-sock open' + (st.sel === s.name ? ' sel' : ''), onclick: () => { st.sel = s.name; view.flyTo(socketCentre(s), Math.max(60, s.w * 2.2)); renderSockets(); },
      title: 'click: fly to it'}, s.name + '  ' + s.w + '×' + s.h + ' ' + s.profile + ' ' + dirLabel(s.dir) + (s.tags.length ? '  [' + s.tags.join(', ') + ']' : ''))));
  }
  const dirLabel = (d) => d[1] > 0 ? 'up' : d[1] < 0 ? 'down' : d[0] > 0 ? '+X' : d[0] < 0 ? '-X' : d[2] > 0 ? '+Z' : '-Z';

  function draw(D) {
    const p = st.program;
    if (!p) return;
    D.box(p.bounds.min, p.bounds.max.map(v => v + 1), COL.dim);
    for (const s of p.sockets) {
      const col = st.sel === s.name ? COL.sel : s.tags.includes('surface') ? COL.surface : COL.open;
      drawSocket(D, s, col, s.name + ' ' + s.w + '×' + s.h);
    }
    if (st.showWay) for (const w of p.waynodes) {
      for (const l of w.links) { const o = p.waynodes.find(x => x.name === l); if (o && o.name > w.name) D.line(add(w.pos, 1), add(o.pos, 1), [0.5, 0.8, 1, 0.55]); }
      D.marker(add(w.pos, 1), '', [0.5, 0.8, 1, 0.8], {kind: 'way', name: w.name}, {size: 2});
    }
    if (st.showSug) for (const r of p.refs) D.marker(add(r.pos, 2), (r.kind[0] || '?').toUpperCase(), [0.85, 0.75, 1, 0.9], {kind: 'sug', ref: r}, {size: 5});
  }
  const add = (p, dy) => [p[0] + 0.5, p[1] + dy, p[2] + 0.5];
  function onClick(e, hit) {
    if (hit && hit.marker && hit.marker.kind === 'sug') { const r = hit.marker.ref; T.setStatus('suggested ' + r.kind + ' ' + r.id + ' ' + JSON.stringify(r.props || {})); }
    else if (hit && hit.marker && hit.marker.kind === 'way') T.setStatus('waynode ' + hit.marker.name);
    else if (hit && hit.voxel) {
      const v = hit.voxel, m = view.wv.pal && view.wv.pal.materials[v.mat];
      T.setStatus((m ? m.name : '#' + v.mat) + ' at ' + v.cell.join(', ') + ' (piece frame)');
    }
  }

  /* ---- props (local preview) ---- */
  function renderProps() {
    const ids = DG.PROP_IDS.filter(id => id.startsWith(st.kit + '/'));
    const others = DG.PROP_IDS.filter(id => !id.startsWith(st.kit + '/'));
    const item = (id) => h('div', {class: 'dg-item' + (st.prop === id ? ' sel' : ''), onclick: () => setProp(id), 'data-prop': id}, DG.PROPS[id].label || id, h('span', {class: 'tag'}, id.split('/')[0]));
    ui.props.replaceChildren(...ids.map(item), h('div', {class: 'dg-hint'}, 'other kits'), ...others.map(item));
  }
  function setProp(id) {
    st.prop = id;
    const P = DG.PROPS[id];
    st.propParams = DG.normalizeParams(P.SCHEMA, {});
    renderProps();
    if (!propHost) {
      propHost = h('div', {class: 'dg-propview'});
      propView = new View3D({draw: () => {}});
      propView.el.style.position = 'absolute'; propView.el.style.inset = '0';
      propHost.append(propView.el);
    }
    const pr = h('select', {onchange: () => { const x = P.PRESETS[pr.value]; st.propParams = DG.normalizeParams(P.SCHEMA, x ? x.params : {}); drawProp(); sp.set(st.propParams); }},
      h('option', {value: ''}, '(defaults)'), Object.keys(P.PRESETS || {}).map(n => h('option', {value: n}, n)));
    const sp = sliderPanel(P.SCHEMA, st.propParams, (k, v) => { st.propParams = DG.normalizeParams(P.SCHEMA, Object.assign({}, st.propParams, {[k]: v})); drawProp(); });
    ui.propPanel.replaceChildren(h('h4', {}, 'Prop: ' + (P.label || id)),
      h('div', {class: 'dg-btns'}, 'preset ', pr, h('button', {onclick: () => { st.propSeed = (st.propSeed * 7 + 3) >>> 0; drawProp(); }}, 'Reroll')),
      propHost, h('div', {class: 'dg-hint'}, 'Props are placed per dungeon on the Dungeon page (Props palette); a piece also suggests its own.'), sp.root);
    drawProp();
  }
  async function drawProp() {
    if (!st.prop || !propView.ensure()) return;
    const P = DG.PROPS[st.prop];
    let pal;
    try { pal = await api.palette(); } catch (e) { return; }
    const d = DG.newDungeon({style: st.kit}); d.palette = {variant: st.variant};
    const res = DG.resolvePalette(d, st.kit);
    const byName = new Map(pal.materials.map(m => [m.name, m.id]));
    const tpl = DG.bakeProp(st.prop, st.propParams, st.propSeed, (m) => (m && m[0] === '@' ? res[m.slice(1)] : res[m]) || m);
    const [nx, ny, nz] = tpl.size;
    const cells = new Uint16Array(nx * ny * nz);
    for (const [x, y, z, p] of tpl.voxels) {
      const name = tpl.palette[p];
      if (name === '@carve') continue;
      const id = byName.get(name) || byName.get('stone') || 1;
      cells[(z * ny + y) * nx + x] = id;
    }
    const wv = propView.wv;
    wv.opts.streaming = false;
    wv.setLocalRegions([{cells, nx, ny, nz, origin: [0, 0, 0]}]);
    wv.cam.mode = 'orbit'; wv.cam.target = [nx / 2, ny / 2, nz / 2]; wv.cam.dist = Math.max(12, Math.hypot(nx, ny, nz) * 1.4);
    wv.cam.yaw = 0.8; wv.cam.pitch = -0.45;
    propView.extraStatus = P.label + ' ' + nx + '×' + ny + '×' + nz + ' cells';
    propView.start();
    propView.dirty = true;
  }

  /* ---- page API ---- */
  let built = false, initP = null;
  async function init() {
    build(); built = true;
    try { const c = await api.catalog(); st.userPresets = c.presets || {}; } catch (e) { /* the page works without the catalog */ }
    setKit(st.kit);
  }
  async function activate() {
    const first = !initP;
    if (!initP) initP = init();
    await initP;
    if (!first && pv.stale()) regen(true);
    view.start();
  }
  function deactivate() { view.stop(); if (propView) propView.stop(); }
  return {activate, deactivate, view, st, setKit, setType, run, presets: kitPresets,
          setUserPresets(p) { st.userPresets = p; if (built && st.type) renderPresets(); }};
}
